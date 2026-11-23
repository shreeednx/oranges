/* The kernel's threads, handles and synchronization, for the SDK's nn::os, which runs lifted over these SVCs.

   Each guest thread is a host thread with its own guest context and thread-local region. Its entry is the SDK's own
   thread entry (nn::os), which sets up the thread's TLS itself, as on the console.

   Synchronization follows Horizon's semantics, under one kernel lock (klock) with one condition variable (kwake)
   that every state change broadcasts. Simple and correct; a waiter re-checks its own condition when woken.
     - mutexes (ArbitrateLock/Unlock): the guest word holds the owner's handle, with bit 30 set while others wait. The
       kernel hands the mutex to the waiter it wakes.
     - condition variables (WaitProcessWideKeyAtomic / SignalProcessWideKey): the wait releases the mutex, and a woken
       waiter takes it back before it returns (or queues for it).
     - address arbiters (WaitForAddress / SignalToAddress), events, and WaitSynchronization on events and threads.

   Threads run freely (no scheduler), but each one keeps the core the console would run it on: its ideal core and
   affinity mask from CreateThread / SetThreadCoreMask, and GetCurrentProcessorNumber reports it. The game relies on
   that: a thread pinned to one core takes `core + 1` as its index in TLS (f_0b7e2374), and the job scheduler's
   per-thread slots are indexed by it (f_086410b0). With every thread on core 0 they collided and the frame loop's
   barriers never let go. */
#include "hos.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CUR_THREAD 0xFFFF8000u
#define CUR_PROCESS 0xFFFF8001u
#define MUTEX_WAITERS 0x40000000u
#define KERN_CANCELLED HOS_RESULT(1, 118)
#define KERN_INVALID_CURRENT_MEMORY HOS_RESULT(1, 106)
#define KERN_TERMINATION_REQUESTED HOS_RESULT(1, 59)
#define KERN_INVALID_CORE_ID HOS_RESULT(1, 113)
#define PROCESS_CORE_MASK 0x7ULL    /* an application's cores, 0-2 (GetInfo AllowedCoreMask) */
#define IDEAL_CORE_DONT_CARE (-1)   /* SetThreadCoreMask's special ideal cores */
#define IDEAL_CORE_USE_PROCESS (-2)
#define IDEAL_CORE_NO_UPDATE (-3)

static pthread_mutex_t klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kwake;   /* (CLOCK_MONOTONIC, set up in hos_kernel_init) */

typedef struct KThread {
    Ctx ctx;
    uint32_t handle;
    uint64_t entry, arg, stack_top, tlr;
    int priority, started, exited;
    int ideal_core, core;           /* the ideal core (or IDEAL_CORE_DONT_CARE), and the core it runs on */
    uint64_t affinity;              /* the cores it may run on */
    pthread_t host;
    /* what the thread waits for, if anything */
    enum { W_NONE, W_MUTEX, W_CONDVAR, W_ARBITER } wait;
    uint64_t wait_addr, wait_key;   /* the mutex word; the condition variable's key */
    int woken, cancel;              /* handed the mutex / signalled; CancelSynchronization pending */
    uint64_t seq;                   /* when it began waiting (FIFO among equals) */
    volatile uint32_t svc;          /* the SVC it is in, plus 1 (0: in guest code), for hos_kernel_dump */
} KThread;

enum { H_FREE = HOS_H_FREE, H_THREAD = HOS_H_THREAD, H_EVENT = HOS_H_EVENT };
#define MAX_HANDLES 4096
static struct { int kind; void *obj; uint64_t a, b, c; } handles[MAX_HANDLES];
typedef struct { int signaled, autoclear; } KEvent;

#define MAX_THREADS 256
static KThread *threads[MAX_THREADS];
static int nthreads;
static uint64_t wait_seq;
static __thread KThread *self;

void hos_kernel_init(void) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&kwake, &a);
}

/* ---- handles */

uint32_t hos_handle_new(int kind, void *obj, uint64_t a, uint64_t b, uint64_t c) {
    pthread_mutex_lock(&klock);
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (handles[i].kind == H_FREE) {
            handles[i].kind = kind, handles[i].obj = obj, handles[i].a = a, handles[i].b = b, handles[i].c = c;
            pthread_mutex_unlock(&klock);
            return 0x1000u | i;
        }
    pthread_mutex_unlock(&klock);
    hos_fatal(NULL, "out of handles");
}

/* (under klock) the handle's slot, or -1 */
static int slot_of(uint32_t h) {
    if (h == CUR_THREAD && self) h = self->handle;
    const uint32_t i = h & 0xFFF;
    return (h & ~0xFFFu) == 0x1000u && i && i < MAX_HANDLES && handles[i].kind != H_FREE ? (int)i : -1;
}

void *hos_handle_object(uint32_t h, int kind) {
    pthread_mutex_lock(&klock);
    const int i = slot_of(h);
    void *obj = i >= 0 && handles[i].kind == kind ? handles[i].obj : NULL;
    pthread_mutex_unlock(&klock);
    return obj;
}

static KThread *thread_of(uint32_t h) {
    const int i = slot_of(h);
    return i >= 0 && handles[i].kind == H_THREAD ? handles[i].obj : NULL;
}

/* ---- waiting, under klock */

static int deadline_of(int64_t ns, struct timespec *ts) {
    if (ns < 0) return 0;
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ns / 1000000000;
    ts->tv_nsec += ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) ts->tv_sec++, ts->tv_nsec -= 1000000000;
    return 1;
}

/* wait for a broadcast: 0, or ETIMEDOUT once the deadline passed */
static int kwait(int timed, const struct timespec *ts) {
    return timed ? pthread_cond_timedwait(&kwake, &klock, ts) : pthread_cond_wait(&kwake, &klock);
}

static KThread *best_waiter(int kind, uint64_t addr) {   /* highest priority (lowest number), then the earliest */
    KThread *best = NULL;
    for (int i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        if ((int)t->wait != kind || t->woken || (kind == W_CONDVAR ? t->wait_key : t->wait_addr) != addr) continue;
        if (!best || t->priority < best->priority || (t->priority == best->priority && t->seq < best->seq)) best = t;
    }
    return best;
}

static int has_waiter(int kind, uint64_t addr) { return best_waiter(kind, addr) != NULL; }

/* hand the mutex at addr to its next waiter, or free it */
static void mutex_release(uint64_t addr) {
    KThread *next = best_waiter(W_MUTEX, addr);
    if (!next) {
        __atomic_store_n((uint32_t *)GP(addr), 0, __ATOMIC_RELEASE);
        return;
    }
    next->woken = 1;
    next->wait = W_NONE;
    __atomic_store_n((uint32_t *)GP(addr), next->handle | (has_waiter(W_MUTEX, addr) ? MUTEX_WAITERS : 0),
                     __ATOMIC_RELEASE);
    pthread_cond_broadcast(&kwake);
}

/* take the mutex at addr for t, or queue t for it (t then waits until it is handed over) */
static int mutex_take_or_queue(KThread *t, uint64_t addr) {
    uint32_t *word = GP(addr);
    uint32_t cur = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!cur) {
            if (__atomic_compare_exchange_n(word, &cur, t->handle, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
            continue;
        }
        if (__atomic_compare_exchange_n(word, &cur, cur | MUTEX_WAITERS, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    t->wait = W_MUTEX, t->wait_addr = addr, t->woken = 0, t->seq = ++wait_seq;
    return 0;
}

/* ---- threads */

static void *thread_main(void *p) {
    KThread *t = p;
    self = t;
    hos_current = &t->ctx;
    Ctx *c = &t->ctx;
    c->x[0] = t->arg;
    c->x[30] = 0;
    c->x[31] = t->stack_top & ~15ULL;
    A64Fn f = a64_lookup(t->entry);
    if (!f) hos_fatal(c, "thread %#x: entry %#llx is not a function", t->handle, (unsigned long long)t->entry);
    if (hos_trace) hos_log("thread %#x: start at %#llx", t->handle, (unsigned long long)t->entry);
    f(c);
    hos_log("thread %#x: its entry returned", t->handle);
    pthread_mutex_lock(&klock);
    t->exited = 1;
    pthread_cond_broadcast(&kwake);
    pthread_mutex_unlock(&klock);
    return NULL;
}

/* (under klock) the core t runs on after a change of its ideal core or affinity: the ideal core, else the one it is on
   if still allowed, else the highest allowed (the kernel's rule, KThread::SetCoreMask) */
static void thread_place(KThread *t) {
    if (t->ideal_core >= 0) t->core = t->ideal_core;
    else if (!(t->affinity >> t->core & 1)) t->core = 63 - __builtin_clzll(t->affinity);
}

/* core: CreateThread's ideal core, already checked (IDEAL_CORE_USE_PROCESS or one of the process's cores) */
static KThread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int priority, int core) {
    KThread *t = calloc(1, sizeof *t);
    t->entry = entry, t->arg = arg, t->stack_top = stack_top, t->priority = priority;
    t->ideal_core = t->core = core == IDEAL_CORE_USE_PROCESS ? hos_main_core : core;
    t->affinity = 1ULL << t->ideal_core;
    t->tlr = hos_sys_alloc(HOS_TLS_SIZE, HOS_TLS_SIZE);
    t->handle = hos_handle_new(H_THREAD, t, 0, 0, 0);
    G32(t->tlr + 0x110) = t->handle;
    t->ctx.tpidrro_el0 = t->tlr;
    t->ctx.thread = t;
    pthread_mutex_lock(&klock);
    if (nthreads == MAX_THREADS) hos_fatal(NULL, "too many threads");
    threads[nthreads++] = t;
    pthread_mutex_unlock(&klock);
    return t;
}

/* the main thread, which hos_main.c runs on the process's own host thread */
Ctx *hos_kernel_main_thread(uint64_t stack_top, int priority) {
    KThread *t = thread_new(0, 0, stack_top, priority, hos_main_core);
    t->started = 1;
    t->host = pthread_self();
    t->ctx.x[31] = stack_top & ~15ULL;
    self = t;
    hos_current = &t->ctx;
    return &t->ctx;
}

uint64_t hos_main_thread_handle(void) { return threads[0] ? threads[0]->handle : 0; }

/* ---- events for the host's own use (system events the HLE hands the game: hos_system_event_init) */

uint32_t hos_kernel_event_new(uint32_t *readable) {
    KEvent *e = calloc(1, sizeof *e);
    const uint32_t w = hos_handle_new(H_EVENT, e, 0, 0, 0);
    *readable = hos_handle_new(H_EVENT, e, 0, 0, 0);
    return w;
}

void hos_kernel_event_signal(uint32_t handle) {
    pthread_mutex_lock(&klock);
    const int i = slot_of(handle);
    if (i >= 0 && handles[i].kind == H_EVENT) {
        ((KEvent *)handles[i].obj)->signaled = 1;
        pthread_cond_broadcast(&kwake);
    }
    pthread_mutex_unlock(&klock);
}

/* ---- what every thread does, for a person debugging a stall (HOS_SAMPLE, the watchdog) */

void hos_kernel_svc_mark(Ctx *c, uint32_t imm_plus_1) {
    KThread *t = c->thread;
    if (t) t->svc = imm_plus_1;
}

/* the name nn::os gave a thread: ThreadType's name pointer points into its own name buffer (TLS + 0x1f8 holds the
   ThreadType *), so look for a word that points a little before itself */
static const char *thread_name(KThread *t, char *buf, size_t n) {
    snprintf(buf, n, "?");
    const uint64_t tt = G64(t->tlr + 0x1f8);
    if (!tt || tt >= HOS_WINDOW - 0x200) return buf;
    for (uint64_t k = 8; k < 0x200; k += 8) {
        const uint64_t p = G64(tt + k);
        if (p < tt || p >= tt + k || p + 32 > tt + k) continue;
        const char *s = GP(p);
        size_t i = 0;
        while (i < 31 && s[i] >= 0x20 && s[i] < 0x7f) i++;
        if (i && !s[i]) { snprintf(buf, n, "%s", s); break; }
    }
    return buf;
}

/* a thread's guest call chain: lr, then the return addresses of its frame records. The chain of a thread that runs on
   is read while it changes, so only frames inside its stack are followed. */
static int thread_chain(KThread *t, uint64_t *out, int max) {
    uint64_t lo = 0, size = 0;
    int state = 0, perm = 0;
    hos_query(t->ctx.x[31], &lo, &size, &state, &perm);
    const uint64_t hi = state ? lo + size : 0;
    int n = 0;
    out[n++] = t->ctx.x[30];
    for (uint64_t fp = t->ctx.x[29]; n < max && fp >= lo && fp + 16 <= hi && !(fp & 7);) {
        const uint64_t lr = G64(fp + 8), next = G64(fp);
        if (!lr) break;
        out[n++] = lr;
        if (next <= fp) break;
        fp = next;
    }
    return n;
}

static void frame_line(const char *label, uint64_t pc, const char *extra) {
    uint64_t off = 0;
    const char *name = hos_symbol(pc, &off);
    if (name && off < 0x1000) hos_log("    %-4s %#llx  %s+%#llx%s", label, (unsigned long long)pc, name,
                                      (unsigned long long)off, extra);
    else hos_log("    %-4s %#llx%s", label, (unsigned long long)pc, extra);
}

static void thread_backtrace(KThread *t) {
    uint64_t chain[24];
    const int n = thread_chain(t, chain, 24);
    for (int i = 0; i < n; i++) {
        char label[8];
        snprintf(label, sizeof label, i ? "#%d" : "lr", i);
        frame_line(label, chain[i], "");
    }
}

void hos_kernel_dump(const char *why) {
    KThread *list[MAX_THREADS];
    pthread_mutex_lock(&klock);
    const int n = nthreads;
    memcpy(list, threads, n * sizeof *list);
    pthread_mutex_unlock(&klock);
    hos_log("threads (%s), %.1f s:", why, hos_now_ns() / 1e9);
    for (int i = 0; i < n; i++) {
        KThread *t = list[i];
        if (!t->started || t->exited) continue;
        char name[40], what[160];
        const uint32_t svc = t->svc;
        if (t->wait == W_MUTEX) snprintf(what, sizeof what, "waits for the mutex at %#llx (owner %#x)",
                                         (unsigned long long)t->wait_addr, G32(t->wait_addr) & ~MUTEX_WAITERS);
        else if (t->wait == W_ARBITER) snprintf(what, sizeof what, "waits for the address %#llx (now %#x)",
                                                (unsigned long long)t->wait_addr, G32(t->wait_addr));
        else if (t->wait) {   /* (and the words around the mutex: a semaphore's count is 8 bytes before it) */
            const uint32_t *m = GP(t->wait_addr - 0x10);
            snprintf(what, sizeof what, "waits for a condition variable at %#llx (mutex %#llx: %x %x %x %x [%x] %x)",
                     (unsigned long long)t->wait_key, (unsigned long long)t->wait_addr, m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        else if (svc) snprintf(what, sizeof what, "in svc %#x (x0 %#llx x1 %#llx)", svc - 1,
                               (unsigned long long)t->ctx.x[0], (unsigned long long)t->ctx.x[1]);
        else snprintf(what, sizeof what, "runs");
        hos_log("  thread %#x \"%s\" priority %d core %d: %s", t->handle, thread_name(t, name, sizeof name), t->priority,
                t->core, what);
        thread_backtrace(t);
    }
}

/* ---- the SVCs: 1 when handled */

int hos_kernel_svc(Ctx *A64_NOESC c, uint32_t imm) {
    switch (imm) {
    case 0x08: {   /* CreateThread(-, entry, arg, stack top, priority, core) -> handle */
        const int core = (int32_t)c->x[5];
        if (core != IDEAL_CORE_USE_PROCESS && (core < 0 || core > 63 || !(PROCESS_CORE_MASK >> core & 1))) {
            c->x[0] = KERN_INVALID_CORE_ID;
            return 1;
        }
        KThread *t = thread_new(c->x[1], c->x[2], c->x[3], (int)c->x[4], core);
        if (hos_trace) hos_log("thread %#x: created, entry %#llx arg %#llx stack %#llx prio %d", t->handle,
                               (unsigned long long)t->entry, (unsigned long long)t->arg,
                               (unsigned long long)t->stack_top, t->priority);
        if (hos_trace) hos_backtrace(c);
        hos_log("thread %#x: created (entry %#llx, priority %d, core %d) by thread %#x", t->handle,
                (unsigned long long)c->x[1], t->priority, core, self ? self->handle : 0);
        c->x[0] = 0, c->x[1] = t->handle;
        return 1;
    }
    case 0x09: {   /* StartThread(handle) */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[0]);
        const int ok = t && !t->started;
        if (ok) t->started = 1;
        pthread_mutex_unlock(&klock);
        if (!ok) { c->x[0] = KERN_INVALID_HANDLE; return 1; }
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 8 << 20);   /* (host frames of lifted code: deep guest call chains) */
        if (pthread_create(&t->host, &a, thread_main, t)) hos_fatal(c, "cannot start a host thread");
        pthread_attr_destroy(&a);
        c->x[0] = 0;
        return 1;
    }
    case 0x0A:     /* ExitThread */
        pthread_mutex_lock(&klock);
        if (self) self->exited = 1;
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        if (self == threads[0]) hos_fatal(c, "ExitThread on the main thread");
        pthread_exit(NULL);
    case 0x0B: {   /* SleepThread(ns): 0, -1 and -2 are yields */
        const int64_t ns = (int64_t)c->x[0];
        if (ns > 0) {
            struct timespec ts = {ns / 1000000000, ns % 1000000000};
            while (nanosleep(&ts, &ts) && errno == EINTR) {}
        } else {
            sched_yield();
        }
        return 1;
    }
    case 0x0C: {   /* GetThreadPriority(-, handle) -> priority */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[1]);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE, c->x[1] = t ? (uint64_t)t->priority : 0;
        pthread_mutex_unlock(&klock);
        return 1;
    }
    case 0x0D: {   /* SetThreadPriority(handle, priority) */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[0]);
        if (t) t->priority = (int)c->x[1];
        pthread_mutex_unlock(&klock);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE;
        return 1;
    }
    case 0x0E: {   /* GetThreadCoreMask(-, handle) -> ideal core, affinity mask */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[2]);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE;
        c->x[1] = t ? (uint64_t)(uint32_t)t->ideal_core : 0, c->x[2] = t ? t->affinity : 0;
        pthread_mutex_unlock(&klock);
        return 1;
    }
    case 0x0F: {   /* SetThreadCoreMask(handle, ideal core, affinity mask): the checks are the svc's */
        int ideal = (int32_t)c->x[1];
        uint64_t mask = c->x[2];
        uint32_t r = 0;
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[0]);
        if (ideal == IDEAL_CORE_USE_PROCESS) ideal = hos_main_core, mask = 1ULL << ideal;
        if (!t) r = KERN_INVALID_HANDLE;
        else if (!mask || mask & ~PROCESS_CORE_MASK) r = KERN_INVALID_CORE_ID;
        else if (ideal >= 0 && (ideal > 63 || !(PROCESS_CORE_MASK >> ideal & 1))) r = KERN_INVALID_CORE_ID;
        else if (ideal >= 0 && !(mask >> ideal & 1)) r = KERN_INVALID_COMBINATION;
        else if (ideal < 0 && ideal != IDEAL_CORE_DONT_CARE && ideal != IDEAL_CORE_NO_UPDATE) r = KERN_INVALID_CORE_ID;
        else if (ideal == IDEAL_CORE_NO_UPDATE && t->ideal_core >= 0 && !(mask >> t->ideal_core & 1))
            r = KERN_INVALID_COMBINATION;
        if (!r) {
            if (ideal != IDEAL_CORE_NO_UPDATE) t->ideal_core = ideal;
            t->affinity = mask;
            thread_place(t);
        }
        pthread_mutex_unlock(&klock);
        c->x[0] = r;
        return 1;
    }
    case 0x10: c->x[0] = self ? (uint64_t)self->core : (uint64_t)hos_main_core; return 1;   /* GetCurrentProcessorNumber */
    case 0x45: {   /* CreateEvent -> writable, readable (one event behind both) */
        KEvent *e = calloc(1, sizeof *e);
        c->x[1] = hos_handle_new(H_EVENT, e, 0, 0, 0);
        c->x[2] = hos_handle_new(H_EVENT, e, 0, 0, 0);
        c->x[0] = 0;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {   /* SignalEvent, ClearEvent, ResetSignal(handle) */
        pthread_mutex_lock(&klock);
        const int i = slot_of((uint32_t)c->x[0]);
        if (i >= 0 && handles[i].kind == H_EVENT) {
            ((KEvent *)handles[i].obj)->signaled = imm == 0x11;
            pthread_cond_broadcast(&kwake);
        }
        pthread_mutex_unlock(&klock);
        c->x[0] = i >= 0 ? 0 : KERN_INVALID_HANDLE;
        return 1;
    }
    case 0x16: {   /* CloseHandle */
        pthread_mutex_lock(&klock);
        const int i = slot_of((uint32_t)c->x[0]);
        if (i >= 0 && handles[i].kind != H_THREAD) handles[i].kind = H_FREE;   /* (thread objects stay: they run) */
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    case 0x18: {   /* WaitSynchronization(-, handles, count, timeout) -> index */
        const uint32_t *hs = GP(c->x[1]);
        const int n = (int)c->x[2];
        struct timespec ts;
        const int timed = deadline_of((int64_t)c->x[3], &ts);
        pthread_mutex_lock(&klock);
        for (;;) {
            for (int k = 0; k < n; k++) {
                const int i = slot_of(hs[k]);
                if (i < 0) { pthread_mutex_unlock(&klock); c->x[0] = KERN_INVALID_HANDLE; return 1; }
                int ready = 0;
                if (handles[i].kind == H_EVENT) {
                    KEvent *e = handles[i].obj;
                    ready = e->signaled;
                } else if (handles[i].kind == H_THREAD) {
                    ready = ((KThread *)handles[i].obj)->exited;
                }
                if (ready) { pthread_mutex_unlock(&klock); c->x[0] = 0, c->x[1] = (uint64_t)k; return 1; }
            }
            if (self && self->cancel) {
                self->cancel = 0;
                pthread_mutex_unlock(&klock);
                c->x[0] = KERN_CANCELLED;
                return 1;
            }
            if ((int64_t)c->x[3] == 0 || kwait(timed, &ts) == ETIMEDOUT) break;
        }
        pthread_mutex_unlock(&klock);
        c->x[0] = KERN_TIMED_OUT;
        return 1;
    }
    case 0x19: {   /* CancelSynchronization(thread) */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[0]);
        if (t) t->cancel = 1;
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE;
        return 1;
    }
    case 0x1A: {   /* ArbitrateLock(owner handle, mutex, requester handle) */
        const uint64_t addr = c->x[1];
        pthread_mutex_lock(&klock);
        const uint32_t cur = __atomic_load_n((uint32_t *)GP(addr), __ATOMIC_ACQUIRE);
        if (cur != ((uint32_t)c->x[0] | MUTEX_WAITERS)) {   /* (released meanwhile: the caller tries again) */
            pthread_mutex_unlock(&klock);
            c->x[0] = 0;
            return 1;
        }
        self->wait = W_MUTEX, self->wait_addr = addr, self->woken = 0, self->seq = ++wait_seq;
        while (!self->woken) kwait(0, NULL);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    case 0x1B:     /* ArbitrateUnlock(mutex) */
        pthread_mutex_lock(&klock);
        mutex_release(c->x[0]);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    case 0x1C: {   /* WaitProcessWideKeyAtomic(mutex, key, handle, timeout) */
        const uint64_t mutex = c->x[0], key = c->x[1];
        struct timespec ts;
        const int timed = deadline_of((int64_t)c->x[3], &ts);
        pthread_mutex_lock(&klock);
        /* the has-waiter flag before the mutex goes, as the kernel orders them: the SDK's Signal skips the SVC while
           the flag is 0, so a thread that took the mutex in between and signalled would be lost (a semaphore's
           waiter slept on with a count of 1: the frame stall) */
        __atomic_store_n((uint32_t *)GP(key), 1, __ATOMIC_SEQ_CST);
        mutex_release(mutex);
        self->wait = W_CONDVAR, self->wait_key = key, self->wait_addr = mutex, self->woken = 0, self->seq = ++wait_seq;
        /* woken: signalled and handed the mutex (at once, or after a wait in its queue). A timeout in either wait
           leaves both queues and returns without the mutex, as the kernel's KConditionVariable::Wait: the SDK's
           TimedWait takes the critical section itself on TimedOut (taking it here deadlocked that thread on itself) */
        uint32_t r = 0;
        while (!self->woken)
            if ((int64_t)c->x[3] == 0 || kwait(timed, &ts) == ETIMEDOUT) {
                if (!self->woken) r = KERN_TIMED_OUT;
                break;
            }
        self->wait = W_NONE;
        pthread_mutex_unlock(&klock);
        c->x[0] = r;
        return 1;
    }
    case 0x1D: {   /* SignalProcessWideKey(key, count): -1 all */
        const uint64_t key = c->x[0];
        int n = (int32_t)c->x[1];
        pthread_mutex_lock(&klock);
        while (n != 0) {
            KThread *t = best_waiter(W_CONDVAR, key);
            if (!t) break;
            if (mutex_take_or_queue(t, t->wait_addr)) {   /* the mutex was free: it wakes owning it */
                t->wait = W_NONE;
                t->woken = 1;
            }
            if (n > 0) n--;
        }
        if (!has_waiter(W_CONDVAR, key)) __atomic_store_n((uint32_t *)GP(key), 0, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    case 0x25: {   /* GetThreadId(-, handle) -> id */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[1]);
        pthread_mutex_unlock(&klock);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE, c->x[1] = t ? t->handle : 0;
        return 1;
    }
    case 0x34: {   /* WaitForAddress(addr, type, value, timeout): 0 if less than, 1 decrement and wait if less than,
                      2 if equal */
        const uint64_t addr = c->x[0];
        const int type = (int)c->x[1];
        const int32_t value = (int32_t)c->x[2];
        int32_t *word = GP(addr);
        struct timespec ts;
        const int timed = deadline_of((int64_t)c->x[3], &ts);
        pthread_mutex_lock(&klock);
        /* the check and the decrement are one atomic step, as the kernel's (guest threads change the word with their
           own atomics, outside klock) */
        int32_t cur = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        for (;;) {
            const int waits = type == 2 ? cur == value : cur < value;
            if (!waits) { pthread_mutex_unlock(&klock); c->x[0] = KERN_INVALID_STATE; return 1; }
            if (type != 1 || __atomic_compare_exchange_n(word, &cur, cur - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                break;
        }
        if ((int64_t)c->x[3] == 0) { pthread_mutex_unlock(&klock); c->x[0] = KERN_TIMED_OUT; return 1; }
        self->wait = W_ARBITER, self->wait_addr = addr, self->woken = 0, self->seq = ++wait_seq;
        uint32_t r = 0;
        while (!self->woken) {
            if (self->cancel) { self->cancel = 0; r = KERN_CANCELLED; break; }
            if (kwait(timed, &ts) == ETIMEDOUT && !self->woken) { r = KERN_TIMED_OUT; break; }
        }
        self->wait = W_NONE;
        pthread_mutex_unlock(&klock);
        c->x[0] = r;
        return 1;
    }
    case 0x35: {   /* SignalToAddress(addr, type, value, count): 0 signal, 1 increment if equal, 2 modify by waiters if
                      equal */
        const uint64_t addr = c->x[0];
        const int type = (int)c->x[1];
        const int32_t value = (int32_t)c->x[2];
        int count = (int32_t)c->x[3];
        int32_t *word = GP(addr);
        pthread_mutex_lock(&klock);
        if (type) {
            int32_t next = value + 1;
            if (type == 2) {   /* by how many wait (the kernel's rule as Atmosphère's KAddressArbiter has it; to check
                                  against its source): none wait -> value + 1; count <= 0 (all) -> value - 2; all of
                                  them woken -> value - 1; some left waiting -> value */
                int waiting = 0;
                for (int i = 0; i < nthreads; i++)
                    waiting += threads[i]->wait == W_ARBITER && !threads[i]->woken && threads[i]->wait_addr == addr;
                next = !waiting ? value + 1 : count <= 0 ? value - 2 : waiting <= count ? value - 1 : value;
            }
            /* compare and update in one atomic step (the kernel's UpdateIfEqual): a load and a store would lose a guest
               thread's atomic change of the word made between them */
            int32_t cur = value;
            if (!__atomic_compare_exchange_n(word, &cur, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                pthread_mutex_unlock(&klock);
                c->x[0] = KERN_INVALID_STATE;
                return 1;
            }
        }
        while (count != 0) {
            KThread *t = best_waiter(W_ARBITER, addr);
            if (!t) break;
            t->woken = 1;
            if (count > 0) count--;
        }
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- the sampler, the profiler and the frame watchdog (a host thread of their own) */

/* HOS_PROFILE=<s>[,thread name]: sample one thread's guest call chain every millisecond for s seconds (from the first
   frame on), then log each return address by how many samples had it in their chain (the call tree, inclusive) */
#define PROF_SLOTS 8192
static struct { uint64_t addr, hits, leaf; } prof[PROF_SLOTS];

static void prof_add(uint64_t a, int leaf) {
    for (uint32_t h = (uint32_t)(a * 0x9E3779B97F4A7C15ULL >> 51), k = 0; k < PROF_SLOTS; k++, h = (h + 1) % PROF_SLOTS)
        if (prof[h].addr == a || !prof[h].addr) {
            prof[h].addr = a, prof[h].hits++, prof[h].leaf += leaf;
            return;
        }
}

static int prof_cmp(const void *x, const void *y) {
    const uint64_t a = ((const typeof(prof[0]) *)x)->hits, b = ((const typeof(prof[0]) *)y)->hits;
    return a < b ? 1 : a > b ? -1 : 0;
}

static KThread *thread_named(const char *want) {
    pthread_mutex_lock(&klock);
    const int n = nthreads;
    KThread *list[MAX_THREADS];
    memcpy(list, threads, n * sizeof *list);
    pthread_mutex_unlock(&klock);
    for (int i = 0; i < n; i++) {
        char name[40];
        if (list[i]->started && !list[i]->exited && !strcmp(thread_name(list[i], name, sizeof name), want)) return list[i];
    }
    return NULL;
}

static void profile(double seconds, const char *want) {
    KThread *t = thread_named(want);
    if (!t) { hos_log("profile: no thread \"%s\"", want); return; }
    hos_log("profile: thread %#x \"%s\" for %.0f s", t->handle, want, seconds);
    memset(prof, 0, sizeof prof);
    const int64_t end = hos_now_ns() + (int64_t)(seconds * 1e9);
    uint64_t samples = 0, in_svc[0x80] = {0};
    while (hos_now_ns() < end) {
        uint64_t chain[48];
        const int n = thread_chain(t, chain, 48);
        for (int i = 0; i < n; i++) {
            int dup = 0;   /* (recursion: count an address once per sample) */
            for (int j = 0; j < i; j++) dup |= chain[j] == chain[i];
            if (!dup) prof_add(chain[i], i == 0);
        }
        const uint32_t svc = t->svc;
        if (svc && svc - 1 < 0x80) in_svc[svc - 1]++;
        samples++;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    qsort(prof, PROF_SLOTS, sizeof prof[0], prof_cmp);
    hos_log("profile: %llu samples; return addresses by samples (inclusive, leaf):", (unsigned long long)samples);
    for (int i = 0; i < 150 && prof[i].addr; i++) {
        char extra[48];
        snprintf(extra, sizeof extra, "  %5.1f%%  %5.1f%%", 100.0 * prof[i].hits / samples, 100.0 * prof[i].leaf / samples);
        frame_line("", prof[i].addr, extra);
    }
    for (int i = 0; i < 0x80; i++)
        if (in_svc[i]) hos_log("profile: in svc %#x %.1f%%", i, 100.0 * in_svc[i] / samples);
}

static void *sampler_main(void *p) {
    (void)p;
    extern uint64_t hos_present_count;   /* (nvn_native.c) */
    const char *s = getenv("HOS_SAMPLE"), *w = getenv("HOS_WATCHDOG"), *pr = getenv("HOS_PROFILE");
    const double every = s ? atof(s) : 0, watch = w ? atof(w) : 5;
    int64_t next = every > 0 ? (int64_t)(every * 1e9) : INT64_MAX, last_frame = 0;
    uint64_t frames = 0;
    int fired = 0;
    for (;;) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        const int64_t now = hos_now_ns();
        const uint64_t f = __atomic_load_n(&hos_present_count, __ATOMIC_RELAXED);
        if (f != frames) frames = f, last_frame = now, fired = 0;
        if (pr && frames > 30) {
            const char *comma = strchr(pr, ',');
            profile(atof(pr), comma ? comma + 1 : "MainThread");
            pr = NULL;
        }
        if (watch > 0 && !fired && now - last_frame > (int64_t)(watch * 1e9)) {   /* (the first frame, too) */
            fired = 1;
            char why[64];
            snprintf(why, sizeof why, "watchdog: no frame for %.0f s after %llu", watch, (unsigned long long)frames);
            hos_kernel_dump(why);
        }
        if (now >= next) {
            hos_kernel_dump("HOS_SAMPLE");
            next += (int64_t)(every * 1e9);
        }
    }
    return NULL;
}

void hos_kernel_profile(double seconds, const char *thread) { profile(seconds, thread); }

void hos_sampler_start(void) {
    pthread_t t;
    pthread_create(&t, NULL, sampler_main, NULL);
    pthread_detach(t);
}
