/* The NVIDIA driver's setup in front of NVN (nv::): recorded and answered, never run. NVN itself is answered by the
   host (nvn_native.c), so the driver, its memory and its nvdrv service are not needed. */
#include "hos.h"

static uint64_t graphics_memory, graphics_size, alloc_fns[4];

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
