/* NVN (the Switch's graphics API) and the NVIDIA driver setup in front of it: the exploration layer.

   The plan (Longshot's design, host/nvn_native.c there, for NVN2): the game's NVN calls are answered by the host, which
   keeps NVN's objects in the game's memory and records GPU work for a Vulkan renderer to replay. The NVIDIA driver and
   its nvdrv service never run. The game is a Switch 1 game and uses NVN1, so Longshot's layer is a model, not a
   drop-in.

   Until that layer exists, this file maps what the game asks of NVN:
     - nv::InitializeGraphics and the graphics allocator setters are recorded and return (the driver is not set up);
     - nvnBootstrapLoader / nvnDeviceGetProcAddress hand out one host call a proc name (hos_hostcall.c);
     - a proc that has no implementation logs its first call with its arguments. An ...Initialize proc answers true
       (NVN_TRUE) and every other one 0, so the boot goes on as far as it can and the log lists the procs in the
       order the game needs them. HOS_NVN_STRICT=1 stops at the first one instead. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t graphics_memory, graphics_size, alloc_fns[4];
static int strict = -1;

HLE(_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_) {   /* (alloc, free, realloc, user) */
    for (int i = 0; i < 4; i++) alloc_fns[i] = c->x[i];
    hos_log("nv: graphics allocator alloc %#llx free %#llx realloc %#llx", (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
}
HLE(_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_) { (void)c; }
HLE(_ZN2nv18InitializeGraphicsEPvm) {   /* (memory, size): the driver's own memory, from the game's heap */
    graphics_memory = c->x[0], graphics_size = c->x[1];
    hos_log("nv: InitializeGraphics(%#llx, %llu KB): the NVIDIA driver is not set up (the host answers NVN)",
            (unsigned long long)graphics_memory, (unsigned long long)(graphics_size >> 10));
}

typedef struct Proc { char name[96]; int seen; uint64_t addr; struct Proc *next; } Proc;
static Proc *procs;

static void unimplemented_proc(Ctx *A64_NOESC c, void *user) {
    Proc *p = user;
    if (strict < 0) {
        const char *s = getenv("HOS_NVN_STRICT");
        strict = s && *s && strcmp(s, "0");
    }
    if (strict) hos_fatal(c, "nvn: %s is not implemented", p->name);
    const size_t n = strlen(p->name);
    const int ok = n > 10 && !strcmp(p->name + n - 10, "Initialize");
    if (!p->seen++ || hos_trace)
        hos_log("nvn: %s(%#llx, %#llx, %#llx, %#llx) not implemented: returns %d", p->name, (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], ok);
    c->x[0] = (uint64_t)ok;
}

static void get_proc_address(Ctx *A64_NOESC c, void *user);

static uint64_t proc_for(const char *name) {
    for (Proc *p = procs; p; p = p->next)
        if (!strcmp(p->name, name)) return p->addr;
    Proc *p = calloc(1, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->addr = !strcmp(name, "nvnDeviceGetProcAddress") ? hos_hostcall(p->name, get_proc_address, p)
                                                       : hos_hostcall(p->name, unimplemented_proc, p);
    p->next = procs;
    procs = p;
    return p->addr;
}

static void get_proc_address(Ctx *A64_NOESC c, void *user) {   /* nvnDeviceGetProcAddress(device, name) */
    (void)user;
    c->x[0] = c->x[1] ? proc_for((const char *)GP(c->x[1])) : 0;
}

HLE(nvnBootstrapLoader) {   /* (name) -> the proc */
    const char *name = c->x[0] ? (const char *)GP(c->x[0]) : "";
    hos_log("nvn: nvnBootstrapLoader(\"%s\")", name);
    RET(proc_for(name));
}
