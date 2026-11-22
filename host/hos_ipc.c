/* IPC: the requests the lifted SDK and the game send with svcSendSyncRequest, and the sessions they go to.

   Message layout (switchbrew's "IPC Marshalling", libnx's hipc.h / cmif.h / tipc.h): a HIPC header (2 words), an
   optional special header (process id, copy and move handles), the buffer descriptors, then the raw data words.
     - TIPC (type 16 + command id; sm: uses it from 12.0.0): the raw data follows the descriptors directly. A reply's
       first data word is its result.
     - CMIF (type 4 Request, 5 Control, 6/7 with context): the raw data is aligned to 16 bytes from the message start,
       then (on a domain session, Request only) a 16-byte domain header, then 'SFCI', version, command id, token, then
       the input. A reply has 'SFCO', version, result, token, then the output; a domain reply puts a 16-byte header
       (the number of output objects) before it and the output object ids after the output data.

   Every session is a Session: the service name it was opened with and, once ConvertCurrentObjectToDomain made it a
   domain, its objects. Each session or domain object has an interface: a HosIface of an emulated service, or NULL for
   one nobody emulates. Requests to the latter (and unknown commands of the former) are answered as successes with
   zeroed outputs, and the first of each kind is logged with its caller.

   sm: is served here (TIPC and the old CMIF commands): GetServiceHandle hands out a session for any name, so a client
   linked into the game (like the game's olsc one, which no nn:: HLE sees) gets a working session whose requests are
   answered. Emulated services are listed in services[] below. */
#include "hos.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OBJECTS 256
typedef struct {
    char name[12];                      /* the service (sm name or port name) */
    const HosIface *iface;              /* the session's own interface, while it is not a domain */
    int domain;
    uint8_t live[MAX_OBJECTS];          /* domain object ids in use (0 is never one) */
    const HosIface *obj[MAX_OBJECTS];
} Session;

static pthread_mutex_t ipc_lock = PTHREAD_MUTEX_INITIALIZER;
static const HosIface control_iface = {"control", NULL};   /* (the key for logging unknown control commands) */

/* ---- the services the host emulates */

static uint32_t sm_dispatch(HosIpc *r);
static const HosIface sm_iface = {"sm:", sm_dispatch};

static const struct { const char *name; const HosIface *iface; } services[] = {
    {"sm:", &sm_iface},
    {"olsc:u", &hos_olsc_iface},
};

static const HosIface *service_iface(const char *name) {
    for (size_t i = 0; i < sizeof services / sizeof *services; i++)
        if (!strcmp(services[i].name, name)) return services[i].iface;
    return NULL;
}

static uint32_t new_session(const char *name, const HosIface *iface) {
    Session *s = calloc(1, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->iface = iface;
    return hos_handle_new(HOS_H_SESSION, s, 0, 0, 0);
}

static Session *share_session(Session *s) {   /* a clone: the same service and interface (a domain stays shared) */
    if (s->domain) return s;
    Session *n = calloc(1, sizeof *n);
    memcpy(n->name, s->name, sizeof n->name);
    n->iface = s->iface;
    return n;
}

uint32_t hos_ipc_connect(const char *name) {
    return new_session(name, service_iface(name));
}

/* ---- request helpers for the services */

void hos_ipc_out(HosIpc *r, const void *data, uint32_t n) {
    if (r->out_size + n > sizeof r->out) hos_fatal(r->c, "ipc: %s command %u: reply too large", r->service, r->cmd);
    memcpy(r->out + r->out_size, data, n);
    r->out_size += n;
}

void hos_ipc_out_handle(HosIpc *r, uint32_t handle, int move) {
    if (r->n_handles >= 8) hos_fatal(r->c, "ipc: too many handles in a reply");
    r->out_handle[r->n_handles] = handle, r->out_handle_move[r->n_handles] = move, r->n_handles++;
}

void hos_ipc_out_object(HosIpc *r, const HosIface *iface) {
    if (r->n_objects >= 8) hos_fatal(r->c, "ipc: too many objects in a reply");
    r->out_object[r->n_objects++] = iface;
}

/* ---- sm: */

static uint32_t sm_dispatch(HosIpc *r) {
    char name[9] = {0};
    switch (r->cmd) {
    case 0:   /* RegisterClient (TIPC) / Initialize (CMIF), with the process id */
        return 0;
    case 1:   /* GetServiceHandle(u64 name) -> move handle */
        if (r->in_size >= 8) memcpy(name, r->in, 8);
        {
            const HosIface *iface = service_iface(name);
            const uint32_t h = new_session(name, iface);
            hos_log("ipc: sm GetServiceHandle(\"%s\") -> session %#x%s", name, h,
                    iface ? "" : " (not emulated: requests answered empty)");
            hos_ipc_out_handle(r, h, 1);
        }
        return 0;
    case 2: case 3: case 4:   /* RegisterService, UnregisterService, DetachClient */
        return 0;
    default:
        return HOS_IPC_UNKNOWN;
    }
}

/* ---- logging of requests nobody answers */

static int first_time(const char *service, const HosIface *iface, uint32_t cmd) {
    static struct { const char *service; const HosIface *iface; uint32_t cmd; char name[12]; } seen[1024];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i].iface == iface && seen[i].cmd == cmd && (iface || !strcmp(seen[i].name, service))) return 0;
    if (nseen < 1024) {
        seen[nseen].iface = iface, seen[nseen].cmd = cmd;
        snprintf(seen[nseen].name, sizeof seen[nseen].name, "%s", service);
        nseen++;
    }
    return 1;
}

/* ---- the message */

static uint64_t buf_addr(uint32_t w1, uint32_t w2) {   /* A/B/W descriptor address: bits 0-31, 32-35, 36-38 */
    return (uint64_t)w1 | (uint64_t)((w2 >> 28) & 0xF) << 32 | (uint64_t)((w2 >> 2) & 0x7) << 36;
}

static void control(HosIpc *r, Session *s) {
    switch (r->cmd) {
    case 0: {   /* ConvertCurrentObjectToDomain -> object id */
        uint32_t id = 1;
        if (!s->domain) {
            s->domain = 1, s->live[1] = 1, s->obj[1] = s->iface;
        } else {
            while (id < MAX_OBJECTS && s->live[id]) id++;
        }
        hos_ipc_out(r, &id, 4);
        return;
    }
    case 1: {   /* CopyFromCurrentDomain(object id) -> move handle */
        uint32_t id = 0;
        if (r->in_size >= 4) memcpy(&id, r->in, 4);
        const HosIface *iface = s->domain && id < MAX_OBJECTS && s->live[id] ? s->obj[id] : NULL;
        hos_ipc_out_handle(r, new_session(s->name, iface), 1);
        return;
    }
    case 2: case 4: {   /* CloneCurrentObject, CloneCurrentObjectEx(tag) -> move handle */
        Session *n = share_session(s);
        hos_ipc_out_handle(r, hos_handle_new(HOS_H_SESSION, n, 0, 0, 0), 1);
        return;
    }
    case 3: {   /* QueryPointerBufferSize -> u16 */
        const uint16_t size = 0x8000;
        hos_ipc_out(r, &size, 2);
        return;
    }
    default:
        r->result = HOS_IPC_UNKNOWN;
        return;
    }
}

void hos_ipc_request(Ctx *c, uint8_t *msg, uint32_t msg_size, uint32_t handle) {
    Session *s = hos_handle_object(handle, HOS_H_SESSION);
    const uint32_t w0 = *(uint32_t *)msg, w1 = *(uint32_t *)(msg + 4);
    const uint32_t type = w0 & 0xFFFF;
    if (!s) {
        hos_log("ipc: request (hipc %08x %08x) on %#x, which is no session", w0, w1, handle);
        c->x[0] = KERN_INVALID_HANDLE;
        return;
    }
    if (type == 2 || type == 15) { c->x[0] = 0; return; }   /* Close (CMIF / TIPC): the handle is closed next */

    HosIpc *r = calloc(1, sizeof *r);
    r->c = c, r->service = s->name;

    /* the header, the special header and the descriptors */
    uint32_t p = 8;
    if (w1 >> 31) {
        const uint32_t sh = *(uint32_t *)(msg + p);
        p += 4;
        if (sh & 1) r->pid_sent = 1, memcpy(&r->pid, msg + p, 8), p += 8;
        const int ncopy = (sh >> 1) & 0xF, nmove = (sh >> 5) & 0xF;
        for (int i = 0; i < ncopy + nmove && i < 16; i++) r->in_handle[r->n_in_handles++] = *(uint32_t *)(msg + p + 4 * i);
        p += 4 * (ncopy + nmove);
    }
    const int nx = (w0 >> 16) & 0xF, na = (w0 >> 20) & 0xF, nb = (w0 >> 24) & 0xF, nw = (w0 >> 28) & 0xF;
    for (int i = 0; i < nx; i++, p += 8) {   /* X (send statics): index, address, size */
        const uint32_t d0 = *(uint32_t *)(msg + p), d1 = *(uint32_t *)(msg + p + 4);
        r->x[i].addr = (uint64_t)d1 | (uint64_t)((d0 >> 12) & 0xF) << 32 | (uint64_t)((d0 >> 6) & 0x7) << 36;
        r->x[i].size = d0 >> 16;
    }
    HosIpcBuffer *const lists[3] = {r->a, r->b, r->w};
    const int counts[3] = {na, nb, nw};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < counts[k]; i++, p += 12) {
            const uint32_t d0 = *(uint32_t *)(msg + p), d1 = *(uint32_t *)(msg + p + 4), d2 = *(uint32_t *)(msg + p + 8);
            lists[k][i].addr = buf_addr(d1, d2);
            lists[k][i].size = (uint64_t)d0 | (uint64_t)((d2 >> 24) & 0xF) << 32;
        }
    r->nx = nx, r->na = na, r->nb = nb, r->nw = nw;
    const uint32_t data = p, ndata = w1 & 0x3FF;
    const uint32_t data_end = data + 4 * ndata;
    const uint32_t cmode = (w1 >> 10) & 0xF;   /* C (receive list) descriptors: after the raw data, or at their offset */
    if (cmode >= 2) {
        uint32_t q = (w1 >> 20) & 0x7FF ? 4 * ((w1 >> 20) & 0x7FF) : data_end;
        const int nc = cmode == 2 ? 1 : (int)cmode - 2;
        for (int i = 0; i < nc && i < 16 && q + 8 <= msg_size; i++, q += 8) {
            const uint32_t d0 = *(uint32_t *)(msg + q), d1 = *(uint32_t *)(msg + q + 4);
            r->cbuf[i].addr = (uint64_t)d0 | (uint64_t)(d1 & 0xFFFF) << 32;
            r->cbuf[i].size = d1 >> 16;
        }
        r->nc = nc < 16 ? nc : 16;
    }

    /* the command */
    const int tipc = type >= 16, is_control = type == 5 || type == 7;
    int domain_request = 0, domain_close = 0;
    uint32_t object = 0;
    const HosIface *iface = s->iface;
    if (tipc) {
        r->cmd = type - 16, r->in = msg + data, r->in_size = 4 * ndata;
    } else {
        uint32_t q = (data + 15) & ~15u;
        uint32_t payload = data_end > q ? data_end - q : 0;
        if (s->domain && (type == 4 || type == 6)) {
            domain_request = 1;
            const uint8_t dtype = msg[q];
            const uint16_t dsize = *(uint16_t *)(msg + q + 2);
            object = *(uint32_t *)(msg + q + 4);
            domain_close = dtype == 2;
            q += 16;
            payload = dsize;
            iface = object < MAX_OBJECTS && s->live[object] ? s->obj[object] : NULL;
            if (!domain_close && (object >= MAX_OBJECTS || !s->live[object]))
                hos_log("ipc: %s: request to domain object %u, which does not exist", s->name, object);
        }
        if (!domain_close) {
            if (payload >= 16) r->cmd = *(uint32_t *)(msg + q + 8);
            r->in = msg + q + 16, r->in_size = payload >= 16 ? payload - 16 : 0;
        }
    }
    r->iface = iface;

    pthread_mutex_lock(&ipc_lock);
    if (domain_close) {
        if (object < MAX_OBJECTS) s->live[object] = 0;
    } else if (is_control) {
        control(r, s);
    } else if (iface && iface->dispatch) {
        r->result = iface->dispatch(r);
    } else {
        r->result = HOS_IPC_UNKNOWN;
    }
    if (r->result == HOS_IPC_UNKNOWN) {   /* answered as a success with zeroed outputs */
        if (first_time(s->name, is_control ? &control_iface : iface, r->cmd) || hos_trace) {
            char obj[24] = "";
            if (domain_request) snprintf(obj, sizeof obj, ", domain object %u", object);
            hos_log("ipc: %s %s command %u (hipc %08x %08x%s): %s, from:", s->name,
                    is_control ? "control" : iface ? iface->name : "-", r->cmd, w0, w1, obj,
                    iface || is_control ? "unknown command, answered empty" : "not emulated, answered empty");
            hos_backtrace(c);
        }
        r->result = 0, r->out_size = 0, r->n_handles = 0, r->n_objects = 0;
        memset(r->out, 0, 0x40);
        r->out_size = 0x40;   /* room for any small output a client reads */
    }
    /* objects a non-domain command returns are new sessions (move handles); a domain's are new object ids */
    uint32_t out_ids[8];
    int n_ids = 0;
    for (int i = 0; i < r->n_objects; i++) {
        if (s->domain) {
            uint32_t id = 1;
            while (id < MAX_OBJECTS && s->live[id]) id++;
            if (id == MAX_OBJECTS) hos_fatal(c, "ipc: %s: domain full", s->name);
            s->live[id] = 1, s->obj[id] = r->out_object[i];
            out_ids[n_ids++] = id;
        } else {
            hos_ipc_out_handle(r, new_session(s->name, r->out_object[i]), 1);
        }
    }
    pthread_mutex_unlock(&ipc_lock);
    if (r->result) r->out_size = 0, r->n_handles = 0, n_ids = 0;

    /* the reply */
    uint8_t out[0x100] = {0};
    uint32_t o = 8;
    int ncopy = 0, nmove = 0;
    for (int i = 0; i < r->n_handles; i++) r->out_handle_move[i] ? nmove++ : ncopy++;
    if (r->n_handles) {
        *(uint32_t *)(out + o) = (uint32_t)ncopy << 1 | (uint32_t)nmove << 5;
        o += 4;
        for (int pass = 0; pass < 2; pass++)   /* copy handles first, then move handles */
            for (int i = 0; i < r->n_handles; i++)
                if (r->out_handle_move[i] == pass) *(uint32_t *)(out + o) = r->out_handle[i], o += 4;
    }
    const uint32_t data_start = o;
    if (tipc) {
        *(uint32_t *)(out + o) = r->result;
        o += 4;
        memcpy(out + o, r->out, r->out_size);
        o += (r->out_size + 3) & ~3u;
    } else {
        o = (o + 15) & ~15u;
        if (domain_request) {
            *(uint32_t *)(out + o) = (uint32_t)n_ids;
            o += 16;
        }
        *(uint32_t *)(out + o) = 0x4F434653u;   /* 'SFCO' */
        *(uint32_t *)(out + o + 8) = r->result;
        o += 16;
        memcpy(out + o, r->out, r->out_size);
        o += (r->out_size + 3) & ~3u;
        if (domain_request) memcpy(out + o, out_ids, 4 * n_ids), o += 4 * n_ids;
        if (o < data_start + 16 + (domain_request ? 16 : 0) + 16) o = data_start + 16 + (domain_request ? 16 : 0) + 16;
    }
    if (o > msg_size || o > sizeof out) hos_fatal(c, "ipc: %s command %u: reply too large", s->name, r->cmd);
    *(uint32_t *)out = tipc ? type : 0;
    *(uint32_t *)(out + 4) = ((o - data_start + 3) / 4) | (r->n_handles ? 0x80000000u : 0);
    memset(msg, 0, msg_size < 0x100 ? msg_size : 0x100);
    memcpy(msg, out, o);
    free(r);
    c->x[0] = 0;
}
