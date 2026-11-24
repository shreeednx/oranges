/* NVN, the Switch's graphics API, answered by the host: the game's NVN calls are implemented here, and the console's
   NVIDIA driver (in the sdk module) and its nvdrv service never run.

   - Objects with semantics (device, memory pools, buffers, textures, samplers, descriptor pools, queue, window, sync,
     events, command buffer memory) are implemented as NVN defines them, in place in the game's object memory
     (host/nvn/rx_nvn.h has their layouts, each within the size NVN reserves).
   - Calls that only change GPU state or do GPU work (state bound, draws, clears, copies...) are recorded as commands in
     host memory. A submit hands the recordings to the render back end (rx_nvn_submit_q), which replays them. The back
     end is host/rx_null.c for now: it carries out what the CPU can see of the GPU's work (events, counters, buffer
     copies and updates) and draws nothing. A Vulkan replay (Longshot's gpu/vk) is the next step.
   - A proc the game asks for that has no implementation here logs its first call and answers 0 (1 for an
     ...Initialize). HOS_NVN_STRICT=1 stops the game there instead.

   Calls come from lifted code: arguments are in the guest registers (x0-x7, floats in v0-v7), pointers are guest
   addresses.

   (oranges) Ported from Longshot's host/nvn_native.c (https://git.magicbean.dev/MagicBeanDev/Longshot, commit
   67ba729d, GPL-3.0-or-later), whose notes say: "Adapted from the MIT-licensed NVN layer of d2r-mobile /
   MacNativeGames (jhaizhou), which runs TOTK on Metal: its semantics, TOTK findings and command records", by way of
   TOTKPort's hos/nvn_native.c (e88744a). TOTK is a Switch 1 game on NVN 55, as the game is (55.15), so this is that
   NVN1 layer again: the Switch 2 (NVN2) additions, Longshot's game-specific code, its debugging watches and its Windows
   primitives are taken out, and the device limits are the game's own driver's. */
#include "hos.h"
#include "nvn/rx_nvn.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the host layouts fit in what NVN reserves for each object */
NVN_FITS(nvn_dev_builder, NVNdeviceBuilder);
NVN_FITS(nvn_dev, NVNdevice);
NVN_FITS(nvn_queue_builder, NVNqueueBuilder);
NVN_FITS(nvn_queue, NVNqueue);
NVN_FITS(nvn_pool_builder, NVNmemoryPoolBuilder);
NVN_FITS(nvn_pool, NVNmemoryPool);
NVN_FITS(nvn_dpool, NVNtexturePool);
NVN_FITS(nvn_buffer_builder, NVNbufferBuilder);
NVN_FITS(nvn_buffer, NVNbuffer);
NVN_FITS(nvn_tex_builder, NVNtextureBuilder);
NVN_FITS(nvn_tex, NVNtexture);
NVN_FITS(nvn_view, NVNtextureView);
NVN_FITS(nvn_samp_builder, NVNsamplerBuilder);
NVN_FITS(nvn_samp, NVNsampler);
NVN_FITS(nvn_prog, NVNprogram);
NVN_FITS(nvn_sync, NVNsync);
NVN_FITS(nvn_event_builder, NVNeventBuilder);
NVN_FITS(nvn_event, NVNevent);
NVN_FITS(nvn_win_builder, NVNwindowBuilder);
NVN_FITS(nvn_win, NVNwindow);
NVN_FITS(nvn_cmd, NVNcommandBuffer);
NVN_FITS(nvn_blend_st, NVNblendState);
NVN_FITS(nvn_color_st, NVNcolorState);
NVN_FITS(nvn_chmask_st, NVNchannelMaskState);
NVN_FITS(nvn_ms_st, NVNmultisampleState);
NVN_FITS(nvn_poly_st, NVNpolygonState);
NVN_FITS(nvn_ds_st, NVNdepthStencilState);
NVN_FITS(nvn_va_st, NVNvertexAttribState);
NVN_FITS(nvn_vs_st, NVNvertexStreamState);

static int env_on(const char *name) {
    const char *s = getenv(name);
    return s && *s && strcmp(s, "0");
}

/* ---- calling convention: arguments from the guest registers ---- */
#define NV(name) static void n_##name(Ctx *A64_NOESC c, void *u_)
static __attribute__((noreturn, format(printf, 1, 2))) void die(const char *fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    hos_fatal(NULL, "nvn: %s", b);
}
/* guest memory this layer reads or writes for the game: the n bytes at a must lie in the guest window. An object
   argument of no known size is checked for its first byte. */
static void *gw(uint64_t a, uint64_t n) {
    void *p = hos_guest_range(a, n);
    if (!p) die("guest memory %#llx + %#llx is outside the guest window", (unsigned long long)a, (unsigned long long)n);
    return p;
}
/* a host pointer read from guest memory (a pool or texture an object names): it must point into the window */
static const void *in_window(const void *p, size_t n) {
    if (!p) return NULL;
    const uintptr_t q = (uintptr_t)p, base = (uintptr_t)GP(0);
    if (q < base || !hos_guest_range(q - base, n)) die("object pointer %p is outside the guest window", p);
    return p;
}
#define POOL(p) ((const nvn_pool *)in_window((p), sizeof(nvn_pool)))
#define P(i) ((void *)(c->x[i] ? gw(c->x[i], 1) : NULL))      /* host pointer of a guest pointer argument */
#define PN(i, n) ((void *)(c->x[i] ? gw(c->x[i], (n)) : NULL))  /* the same for an array of n bytes */
#define I(i) ((int)c->x[i])
#define U32(i) ((uint32_t)c->x[i])
#define U64(i) (c->x[i])
#define BOOL(i) ((int)(c->x[i] & 0xff))                         /* NVNboolean: callers do not extend it */
static float fl(const Ctx *c, int i) {
    float f;
    memcpy(&f, &c->vreg[i][0], 4);
    return f;
}
#define F(i) fl(c, i)                                           /* the i-th float argument (v0-v7) */
static void retf(Ctx *c, float f) {
    memset(c->vreg[0], 0, 16);
    memcpy(&c->vreg[0][0], &f, 4);
}
static uint64_t h2g(const void *p) { return p ? (uint64_t)((const uint8_t *)p - (const uint8_t *)GP(0)) : 0; }
#define RETP(p) RET(h2g(p))
#define UNUSED (void)u_

static int nvn_log_on(void) {
    static int v = -1;
    if (v < 0) v = env_on("HOS_NVN_LOG_OBJECTS");
    return v;
}
#define OLOG(...) do { if (nvn_log_on()) hos_log("nvn: " __VA_ARGS__); } while (0)

/* ---- serials, GPU addresses, virtual pools ---- */
static uint64_t serials;
static uint64_t new_serial(void) { return __atomic_add_fetch(&serials, 1, __ATOMIC_RELAXED); }

#define VPAGE 0x10000ull                     /* NVN virtual mapping granularity (64 KiB) */
#define VBASE 0x10000000000ull               /* virtual pools' GPU addresses start at 1 TB: past the guest window */
/* page[i]: guest address, 0 unmapped; run[i]: how many pages from i on are contiguous in guest memory (0 unmapped).
   A finalized pool's map stays allocated (dead), so lookups can use a map without the lock. */
struct nvn_vmap { uint64_t gpu, size; uint64_t *page; uint32_t *run; struct nvn_vmap *next; };
static struct nvn_vmap *vmaps, *dead_vmaps;
static pthread_rwlock_t vmap_lock = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t vmap_gen = 1;                /* bumped when a map is added or removed */
static uint32_t vpage_gen = 1;               /* bumped by every virtual page (re)mapping too (the renderer's caches) */
uint32_t nvn_vmap_generation(void) {
    return __atomic_load_n(&vpage_gen, __ATOMIC_RELAXED) + __atomic_load_n(&vmap_gen, __ATOMIC_RELAXED);
}
static uint64_t next_vbase = VBASE;

static struct nvn_vmap *vmap_find(uint64_t gpu) {
    static __thread struct nvn_vmap *last;
    static __thread uint32_t last_gen;
    struct nvn_vmap *m = last;
    if (m && last_gen == __atomic_load_n(&vmap_gen, __ATOMIC_ACQUIRE) && gpu >= m->gpu && gpu < m->gpu + m->size)
        return m;
    pthread_rwlock_rdlock(&vmap_lock);
    for (m = vmaps; m; m = m->next)
        if (gpu >= m->gpu && gpu < m->gpu + m->size) break;
    last_gen = vmap_gen;
    pthread_rwlock_unlock(&vmap_lock);
    last = m;
    return m;
}
static void vmap_runs(struct nvn_vmap *m) {   /* (vmap_lock held exclusively) */
    const uint64_t n = m->size / VPAGE;
    for (uint64_t i = n; i-- > 0;)
        m->run[i] = !m->page[i] ? 0
                  : i + 1 < n && m->page[i + 1] && m->page[i + 1] == m->page[i] + VPAGE ? m->run[i + 1] + 1 : 1;
}
/* a virtual pool's page table, from the host's list by the pool's GPU address (the pool object is guest memory) */
static struct nvn_vmap *pool_vmap(const nvn_pool *o) {
    if (!(o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || o->gpu < VBASE) return NULL;
    struct nvn_vmap *m = vmap_find(o->gpu);
    return m && m->gpu == o->gpu ? m : NULL;
}
uint64_t nvn_gpu_guest(uint64_t gpu) {
    if (gpu < VBASE) return gpu >= HOS_SYSTEM_BASE && gpu < HOS_WINDOW ? gpu : 0;
    const struct nvn_vmap *m = vmap_find(gpu);
    if (!m) return 0;
    const uint64_t p = m->page[(gpu - m->gpu) / VPAGE];
    return p ? p + (gpu - m->gpu) % VPAGE : 0;
}
uint8_t *nvn_gpu_host(uint64_t gpu) {
    const uint64_t g = nvn_gpu_guest(gpu);
    return g ? (uint8_t *)GP(g) : NULL;
}
uint64_t nvn_gpu_contiguous(uint64_t gpu, uint64_t n) {
    if (gpu < VBASE) {
        if (gpu < HOS_SYSTEM_BASE || gpu >= HOS_WINDOW) return 0;
        return n < HOS_WINDOW - gpu ? n : HOS_WINDOW - gpu;
    }
    const struct nvn_vmap *m = vmap_find(gpu);
    if (!m) return 0;
    const uint64_t i = (gpu - m->gpu) / VPAGE;
    if (!m->page[i]) return 0;
    const uint64_t have = (uint64_t)m->run[i] * VPAGE - (gpu - m->gpu) % VPAGE;
    return have < n ? have : n;
}
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        const uint64_t k = nvn_gpu_contiguous(gpu, n);
        const uint8_t *s = k ? nvn_gpu_host(gpu) : NULL;
        if (!s) return 0;
        memcpy(d, s, k);
        d += k, gpu += k, n -= k;
    }
    return 1;
}
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        const uint64_t k = nvn_gpu_contiguous(gpu, n);
        uint8_t *d = k ? nvn_gpu_host(gpu) : NULL;
        if (!d) return 0;
        memcpy(d, s, k);
        s += k, gpu += k, n -= k;
    }
    return 1;
}

/* ---- device ---- */
static int depth_minus_w = 1;
int nvn_depth_minus_w(void) { return __atomic_load_n(&depth_minus_w, __ATOMIC_RELAXED) != 0; }
uint64_t nvn_native_proc(const char *name);

NV(nvnDeviceBuilderSetDefaults) { UNUSED; memset(P(0), 0, sizeof(nvn_dev_builder)); }
NV(nvnDeviceBuilderSetFlags) { UNUSED; ((nvn_dev_builder *)P(0))->flags = U32(1); }
NV(nvnDeviceInitialize) {
    UNUSED;
    nvn_dev *o = (nvn_dev *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('D', 'E', 'V', 0);
    o->flags = ((const nvn_dev_builder *)P(1))->flags;
    hos_log("nvn: device (flags %#x): NVN %d.%d answered by the host", o->flags, 55, 15);
    RET(1);
}
NV(nvnDeviceFinalize) { UNUSED; ((nvn_dev *)P(0))->magic = 0; }
/* A proc name the game passes: read only inside guest memory and up to 255 characters, copied into out ("" when it is
   not a string there) */
static const char *guest_name(uint64_t a, char *out, size_t cap) {
    out[0] = 0;
    const uint64_t room = a && a < HOS_WINDOW ? HOS_WINDOW - a : 0;
    const size_t lim = room < cap ? (size_t)room : cap;
    const char *s = lim ? (const char *)hos_guest_range(a, lim) : NULL;
    const char *end = s ? (const char *)memchr(s, 0, lim) : NULL;
    if (end) memcpy(out, s, (size_t)(end - s) + 1);
    return out;
}
NV(nvnDeviceGetProcAddress) {
    UNUSED;
    char name[256];
    RET(U64(1) ? nvn_native_proc(guest_name(U64(1), name, sizeof name)) : 0);
}
/* (oranges) the game's own NVN driver's device limits (API 55.15): its nvnDeviceGetInteger (sdk 0xe6c2ac0 in our image)
   run for every property under Unicorn with a zeroed device. The ones it reads from the device's state (zero there:
   0x17 0x18 0x2b 0x2d 0x37 0x38 0x3a 0x3d-0x41) are the Tegra X1's, as TOTKPort's NVN1 table had them (kept in
   Longshot's table). */
static const int devinfo[] = {
    [0x00] = 55, [0x01] = 15, [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4,
    [0x08] = 16, [0x09] = 32, [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32,
    [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2,
    [0x18] = 256, [0x19] = 4096, [0x1A] = 131072, [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64,
    [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535, [0x23] = 8, [0x24] = 1048576, [0x25] = 4096,
    [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048,
    [0x2E] = 1, [0x2F] = 1, [0x30] = 16, [0x31] = 5, [0x32] = 1, [0x33] = 8, [0x34] = 8, [0x35] = 4, [0x36] = 32,
    [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4, [0x3D] = 16384,
    [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 32, [0x60] = 65535, [0x61] = 1, [0x62] = 0, [0x63] = 256, [0x64] = 16,
    [0x65] = 65535, [0x66] = 65535, [0x67] = 256, [0x68] = 16383, [0x69] = 0,
};
NV(nvnDeviceGetInteger) {
    UNUSED;
    const unsigned w = U32(1);
    if (w >= sizeof devinfo / sizeof *devinfo) die("device property %#x", w);
    static int devinfo_log = -1;
    if (devinfo_log < 0) devinfo_log = env_on("HOS_NVN_DEVINFO_LOG");
    if (devinfo_log) hos_log("nvn: nvnDeviceGetInteger(%#x) -> %d", w, devinfo[w]);
    *(int *)PN(2, 4) = devinfo[w];
}
NV(nvnDeviceRegisterFastClearColor) { UNUSED; RET(0); }   /* "not registered": the game keeps its normal clears */
NV(nvnDeviceRegisterFastClearColori) { UNUSED; RET(0); }
NV(nvnDeviceRegisterFastClearColorui) { UNUSED; RET(0); }
NV(nvnDeviceRegisterFastClearDepth) { UNUSED; RET(0); }
/* (oranges) handles as the game's driver makes them: texture id in bits 0-19, sampler id from bit 20, sign-extended; texture
   handles carry bit 32 (sdk 0xe6c2b00-0xe6c2b30, 0xe6c2ab0) */
NV(nvnDeviceGetImageHandle) { UNUSED; RET((uint64_t)(int64_t)I(1)); }
NV(nvnDeviceGetTextureHandle) {
    UNUSED;
    RET((uint64_t)(int64_t)(int32_t)((uint32_t)I(1) | (uint32_t)I(2) << 20) | 1ull << 32);
}
NV(nvnDeviceGetTexelFetchHandle) { UNUSED; RET((uint64_t)(int64_t)I(1) | 1ull << 32); }
NV(nvnDeviceGetSeparateTextureHandle) { UNUSED; RET((uint64_t)(int64_t)I(1) | 1ull << 32); }
NV(nvnDeviceGetSeparateSamplerHandle) { UNUSED; RET((uint64_t)((int64_t)I(1) << 20)); }
NV(nvnDeviceInstallDebugCallback) { UNUSED; (void)c; }
NV(nvnDeviceSetWindowOriginMode) {
    UNUSED;
    ((nvn_dev *)P(0))->origin = U32(1);
    hos_log("nvn: window origin %s", U32(1) ? "upper left" : "lower left");
}
NV(nvnDeviceGetWindowOriginMode) { UNUSED; RET(((const nvn_dev *)P(0))->origin); }
NV(nvnDeviceGetDepthMode) { UNUSED; RET(((const nvn_dev *)P(0))->depth_mode); }
NV(nvnDeviceSetDepthMode) {
    UNUSED;
    ((nvn_dev *)P(0))->depth_mode = U32(1);
    __atomic_store_n(&depth_minus_w, U32(1) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W, __ATOMIC_RELAXED);
    hos_log("nvn: depth mode %s", U32(1) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? "near = -w (GL)" : "near = 0");
}
NV(nvnDeviceFinalizeCommandHandle) { UNUSED; (void)c; }
/* GPU timestamps count at the system tick's rate (19.2 MHz): ns = ticks * 625 / 12 (the driver's device ratio) */
static uint64_t ticks_to_ns(uint64_t t) { return t / 12 * 625 + t % 12 * 625 / 12; }
NV(nvnDeviceGetCurrentTimestampInNanoseconds) { UNUSED; RET(ticks_to_ns(a64_cntpct_host())); }
NV(nvnDeviceGetTimestampInNanoseconds) { UNUSED; RET(ticks_to_ns(((const NVNcounterData *)PN(1, 16))->timestamp)); }
NV(nvnDeviceSetDebugLabel) { UNUSED; (void)c; }
NV(nvnDeviceIsExternalDebuggerAttached) { UNUSED; RET(0); }
NV(nvnDeviceSetIntermediateShaderCache) { UNUSED; (void)c; }

/* ---- memory pools ---- */
NV(nvnMemoryPoolBuilderSetDefaults) { UNUSED; memset(P(0), 0, sizeof(nvn_pool_builder)); }
NV(nvnMemoryPoolBuilderSetDevice) { UNUSED; ((nvn_pool_builder *)P(0))->dev = P(1); }
NV(nvnMemoryPoolBuilderSetStorage) {
    UNUSED;
    nvn_pool_builder *b = (nvn_pool_builder *)P(0);
    b->mem = U64(1), b->size = U64(2);
}
NV(nvnMemoryPoolBuilderSetFlags) { UNUSED; ((nvn_pool_builder *)P(0))->flags = U32(1); }
NV(nvnMemoryPoolBuilderGetFlags) { UNUSED; RET(((const nvn_pool_builder *)P(0))->flags); }
NV(nvnMemoryPoolBuilderGetMemory) { UNUSED; RET(((const nvn_pool_builder *)P(0))->mem); }
NV(nvnMemoryPoolBuilderGetSize) { UNUSED; RET(((const nvn_pool_builder *)P(0))->size); }
NV(nvnMemoryPoolInitialize) {
    UNUSED;
    const nvn_pool_builder *b = (const nvn_pool_builder *)P(1);
    nvn_pool *o = (nvn_pool *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('P', 'O', 'O', 'L');
    o->flags = b->flags;
    o->size = b->size;
    if (o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        if (b->mem || !o->size || o->size % VPAGE)
            die("virtual pool storage %#llx size %#llx", (unsigned long long)b->mem, (unsigned long long)o->size);
        struct nvn_vmap *m = calloc(1, sizeof *m);
        m->page = calloc(o->size / VPAGE, sizeof(uint64_t));
        m->run = calloc(o->size / VPAGE, sizeof(uint32_t));
        if (!m->page || !m->run || o->size >= VBASE) die("virtual pool of %#llx bytes", (unsigned long long)o->size);
        pthread_rwlock_wrlock(&vmap_lock);
        m->gpu = next_vbase;
        m->size = o->size;
        next_vbase += (o->size + VPAGE - 1) & ~(VPAGE - 1);
        m->next = vmaps;
        vmaps = m;
        __atomic_add_fetch(&vmap_gen, 1, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&vmap_lock);
        o->gpu = m->gpu;
        OLOG("virtual pool %#llx bytes at GPU %#llx", (unsigned long long)o->size, (unsigned long long)o->gpu);
        RET(1);
        return;
    }
    if (!b->mem) die("memory pool without storage (flags %#x size %#llx)", o->flags, (unsigned long long)o->size);
    gw(b->mem, o->size);   /* (the storage lies in guest memory) */
    o->mem = b->mem;
    o->gpu = b->mem;
    rx_nvn_pool_created(o->gpu, o->size);
    OLOG("pool %#llx size %#llx flags %#x", (unsigned long long)o->gpu, (unsigned long long)o->size, o->flags);
    RET(1);
}
/* A memory-pool call made with no pool (TOTK passed 0 at times): 0 then, and who asked, once. The guard comes before
   any dereference (P(0) of 0 is NULL, and a NULL dereference lets the compiler fold the check away). */
static int no_pool(Ctx *c, const char *what) {
    if (c->x[0]) return 0;
    static int told;
    if (told++ < 4) hos_log("nvn: %s of no memory pool (lr %#llx): 0", what, (unsigned long long)c->x[30]);
    return 1;
}
NV(nvnMemoryPoolFinalize) {
    UNUSED;
    if (no_pool(c, "finalize")) return;
    nvn_pool *o = (nvn_pool *)P(0);
    struct nvn_vmap *vm = pool_vmap(o);
    if (vm) {   /* (its GPU range is not reused) */
        int mapped = 0;   /* (pages still mapped: what the GPU drew there goes into their memory first) */
        for (uint64_t k = 0; k < vm->size / VPAGE && !mapped; k++) mapped = vm->page[k] != 0;
        if (mapped) rx_nvn_vmap_changing(vm->gpu, vm->size);
        pthread_rwlock_wrlock(&vmap_lock);
        int found = 0;
        for (struct nvn_vmap **p = &vmaps; *p; p = &(*p)->next)
            if (*p == vm) {
                *p = vm->next;
                found = 1;
                break;
            }
        if (found) {   /* (kept: a lookup may still hold it) */
            vm->next = dead_vmaps;
            dead_vmaps = vm;
        }
        __atomic_add_fetch(&vmap_gen, 1, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&vmap_lock);
    }
    o->magic = 0;
}
NV(nvnMemoryPoolGetFlags) {
    UNUSED;
    if (no_pool(c, "flags")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->flags);
}
NV(nvnMemoryPoolGetSize) {
    UNUSED;
    if (no_pool(c, "size")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->size);
}
NV(nvnMemoryPoolMap) {
    UNUSED;
    if (no_pool(c, "map")) { RET(0); return; }
    const nvn_pool *o = (const nvn_pool *)P(0);
    if (o->magic != NVN_MAGIC('P', 'O', 'O', 'L') || (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) {
        RET(0);
        return;
    }
    if (!o->mem) die("CPU mapping of a virtual pool");
    RET(o->mem);
}
NV(nvnMemoryPoolGetBufferAddress) {
    UNUSED;
    if (no_pool(c, "buffer address")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->gpu);
}
/* the GPU reads guest memory as it is; the back end hears of the range (what it copied out of guest memory, textures,
   is stale there) */
NV(nvnMemoryPoolFlushMappedRange) {
    UNUSED;
    if (no_pool(c, "flush mapped range")) return;
    const nvn_pool *o = (const nvn_pool *)P(0);
    if ((int64_t)U64(1) >= 0) rx_nvn_flushed(o->gpu + U64(1), U64(2));
}
NV(nvnMemoryPoolInvalidateMappedRange) {   /* the CPU is about to read what the GPU wrote */
    UNUSED;
    if (no_pool(c, "invalidate mapped range")) return;
    const nvn_pool *o = (const nvn_pool *)P(0);
    if ((int64_t)U64(1) >= 0) rx_nvn_invalidate(o->gpu + U64(1), U64(2));
}
NV(nvnMemoryPoolSetDebugLabel) { UNUSED; (void)c; }
/* NVNmappingRequest (0x28 bytes): physical pool (guest pointer), physical offset, virtual offset, size, storage class */
NV(nvnMemoryPoolMapVirtual) {
    UNUSED;
    if (no_pool(c, "map virtual")) return;
    nvn_pool *v = (nvn_pool *)P(0);
    const int n = I(1);
    struct nvn_vmap *vm = pool_vmap(v);
    if (!vm || n < 0) die("MapVirtual on a pool that is not virtual");
    const uint8_t *r = (const uint8_t *)PN(2, 0x28ull * (uint64_t)n);
    for (int i = 0; i < n; i++) {
        uint64_t pool, size;
        int64_t poff, voff;
        memcpy(&pool, r + 0x28 * i, 8);
        memcpy(&poff, r + 0x28 * i + 8, 8);
        memcpy(&voff, r + 0x28 * i + 16, 8);
        memcpy(&size, r + 0x28 * i + 24, 8);
        if (voff < 0 || poff < 0 || !size || size > vm->size || (uint64_t)voff > vm->size - size ||
            ((uint64_t)voff | (uint64_t)poff | size) % VPAGE)
            die("MapVirtual request %d: virtual %#llx physical %#llx size %#llx", i, (unsigned long long)voff,
                (unsigned long long)poff, (unsigned long long)size);
        const nvn_pool *p = pool ? (const nvn_pool *)gw(pool, sizeof(nvn_pool)) : NULL;
        if (p && (!p->mem || p->gpu != p->mem || size > p->size || (uint64_t)poff > p->size - size ||
                  !hos_guest_range(p->gpu + (uint64_t)poff, size)))
            die("MapVirtual request %d: physical pool", i);
        int mapped = 0;   /* (pages mapped now that change: the GPU's drawing there goes to their memory first) */
        for (uint64_t k = 0; k < size / VPAGE && !mapped; k++) mapped = vm->page[(uint64_t)voff / VPAGE + k] != 0;
        if (mapped) rx_nvn_vmap_changing(v->gpu + (uint64_t)voff, size);
        pthread_rwlock_wrlock(&vmap_lock);
        for (uint64_t k = 0; k < size / VPAGE; k++)
            vm->page[(uint64_t)voff / VPAGE + k] = p ? p->gpu + (uint64_t)poff + k * VPAGE : 0;
        vmap_runs(vm);
        __atomic_add_fetch(&vpage_gen, 1, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&vmap_lock);
    }
    RET(1);
}

/* ---- buffers (a range of a pool) ---- */
NV(nvnBufferBuilderSetDevice) { UNUSED; ((nvn_buffer_builder *)P(0))->dev = P(1); }
NV(nvnBufferBuilderSetDefaults) {
    UNUSED;
    nvn_buffer_builder *b = (nvn_buffer_builder *)P(0);
    void *d = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = d;
}
NV(nvnBufferBuilderSetStorage) {
    UNUSED;
    nvn_buffer_builder *b = (nvn_buffer_builder *)P(0);
    b->pool = (nvn_pool *)P(1);
    b->off = (int64_t)U64(2);
    b->size = U64(3);
}
NV(nvnBufferBuilderGetMemoryPool) { UNUSED; RETP(((const nvn_buffer_builder *)P(0))->pool); }
NV(nvnBufferBuilderGetMemoryOffset) { UNUSED; RET((uint64_t)((const nvn_buffer_builder *)P(0))->off); }
NV(nvnBufferBuilderGetSize) { UNUSED; RET(((const nvn_buffer_builder *)P(0))->size); }
NV(nvnBufferInitialize) {
    UNUSED;
    const nvn_buffer_builder *b = (const nvn_buffer_builder *)P(1);
    const nvn_pool *bp = POOL(b->pool);
    if (!bp || bp->magic != NVN_MAGIC('P', 'O', 'O', 'L') || b->off < 0 || (uint64_t)b->off > bp->size ||
        b->size > bp->size - (uint64_t)b->off) {
        RET(0);
        return;
    }
    nvn_buffer *o = (nvn_buffer *)P(0);
    o->magic = NVN_MAGIC('B', 'U', 'F', 0);
    o->pool = b->pool;
    o->off = b->off;
    o->size = b->size;
    RET(1);
}
NV(nvnBufferFinalize) { UNUSED; memset(P(0), 0, sizeof(nvn_buffer)); }
NV(nvnBufferMap) {
    UNUSED;
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    if (!o || !o->pool) { RET(0); return; }
    const nvn_pool *pool = POOL(o->pool);
    RET(pool->mem ? pool->mem + (uint64_t)o->off : 0);
}
NV(nvnBufferGetAddress) {
    UNUSED;
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    RET(o->pool ? POOL(o->pool)->gpu + (uint64_t)o->off : 0);
}
NV(nvnBufferGetMemoryPool) { UNUSED; RETP(((const nvn_buffer *)P(0))->pool); }
NV(nvnBufferGetMemoryOffset) { UNUSED; RET((uint64_t)((const nvn_buffer *)P(0))->off); }
NV(nvnBufferGetSize) { UNUSED; RET(((const nvn_buffer *)P(0))->size); }
NV(nvnBufferFlushMappedRange) {   /* (as nvnMemoryPoolFlushMappedRange) */
    UNUSED;
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    if (o->pool) rx_nvn_flushed(POOL(o->pool)->gpu + (uint64_t)o->off + U64(1), U64(2));
}
NV(nvnBufferInvalidateMappedRange) {
    UNUSED;
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    if (o->pool) rx_nvn_invalidate(POOL(o->pool)->gpu + (uint64_t)o->off + U64(1), U64(2));
}
NV(nvnBufferSetDebugLabel) { UNUSED; (void)c; }

/* ---- descriptor pools ---- */
NV(nvnTexturePoolInitialize) {
    UNUSED;
    nvn_dpool *o = (nvn_dpool *)P(0);
    o->pool = (const nvn_pool *)P(1);
    o->off = (int64_t)U64(2);
    o->n = I(3);
    rx_nvn_set_texture_pool_size(o->n);
    hos_log("nvn: texture pool of %d descriptors", o->n);
    RET(1);
}
NV(nvnTexturePoolFinalize) { UNUSED; (void)c; }
NV(nvnTexturePoolSetDebugLabel) { UNUSED; (void)c; }
NV(nvnTexturePoolRegisterTexture) {
    UNUSED;
    rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3));
}
NV(nvnTexturePoolRegisterImage) {   /* (images share the texture pool's ids) */
    UNUSED;
    rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3));
}
NV(nvnTexturePoolGetMemoryPool) { UNUSED; RETP(((const nvn_dpool *)P(0))->pool); }
NV(nvnTexturePoolGetMemoryOffset) { UNUSED; RET((uint64_t)((const nvn_dpool *)P(0))->off); }
NV(nvnTexturePoolGetSize) { UNUSED; RET((uint32_t)((const nvn_dpool *)P(0))->n); }
NV(nvnSamplerPoolInitialize) {
    UNUSED;
    nvn_dpool *o = (nvn_dpool *)P(0);
    o->pool = (const nvn_pool *)P(1);
    o->off = (int64_t)U64(2);
    o->n = I(3);
    RET(1);
}
NV(nvnSamplerPoolFinalize) { UNUSED; (void)c; }
NV(nvnSamplerPoolSetDebugLabel) { UNUSED; (void)c; }
NV(nvnSamplerPoolRegisterSampler) { UNUSED; rx_nvn_register_sampler(I(1), &((const nvn_samp *)P(2))->p); }
NV(nvnSamplerPoolGetMemoryPool) { UNUSED; RETP(((const nvn_dpool *)P(0))->pool); }
NV(nvnSamplerPoolGetMemoryOffset) { UNUSED; RET((uint64_t)((const nvn_dpool *)P(0))->off); }
NV(nvnSamplerPoolGetSize) { UNUSED; RET((uint32_t)((const nvn_dpool *)P(0))->n); }

/* ---- textures ---- */
/* A texture builder of no address: a scratch one instead, said once (TOTK passed 0 at times) */
static nvn_tex_builder *tb_or_scratch(void *p, uint64_t lr) {
    static __thread nvn_tex_builder scratch;
    if (p) return (nvn_tex_builder *)p;
    static int told;
    if (__atomic_add_fetch(&told, 1, __ATOMIC_RELAXED) <= 4)
        hos_log("nvn: a texture builder of no address (lr %#llx): a scratch one", (unsigned long long)lr);
    memset(&scratch, 0, sizeof scratch);
    return &scratch;
}
#define TB(p) tb_or_scratch((p), c->x[30])
#define CTB(p) ((const nvn_tex_builder *)tb_or_scratch((p), c->x[30]))
#define TX(p) ((nvn_tex *)(p))
#define CTX(p) ((const nvn_tex *)(p))
NV(nvnTextureBuilderSetDefaults) {
    UNUSED;
    nvn_tex_builder *b = TB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->d.swizzle_r = 2, b->d.swizzle_g = 3, b->d.swizzle_b = 4, b->d.swizzle_a = 5;
    b->d.target = NVN_TEXTURE_TARGET_2D;
    b->d.width = b->d.height = b->d.depth = b->d.levels = 1;
}
NV(nvnTextureBuilderSetDevice) { UNUSED; TB(P(0))->dev = P(1); }
NV(nvnTextureBuilderSetFlags) { UNUSED; TB(P(0))->d.flags = I(1); }
NV(nvnTextureBuilderSetTarget) { UNUSED; TB(P(0))->d.target = I(1); }
NV(nvnTextureBuilderSetWidth) { UNUSED; TB(P(0))->d.width = U32(1); }
NV(nvnTextureBuilderSetHeight) { UNUSED; TB(P(0))->d.height = U32(1); }
NV(nvnTextureBuilderSetDepth) { UNUSED; TB(P(0))->d.depth = U32(1); }
NV(nvnTextureBuilderSetSize1D) {
    UNUSED;
    nvn_tex_builder *b = TB(P(0));
    b->d.width = U32(1), b->d.height = b->d.depth = 1;
}
NV(nvnTextureBuilderSetSize2D) {
    UNUSED;
    nvn_tex_builder *b = TB(P(0));
    b->d.width = U32(1), b->d.height = U32(2), b->d.depth = 1;
}
NV(nvnTextureBuilderSetSize3D) {
    UNUSED;
    nvn_tex_builder *b = TB(P(0));
    b->d.width = U32(1), b->d.height = U32(2), b->d.depth = U32(3);
}
NV(nvnTextureBuilderSetLevels) { UNUSED; TB(P(0))->d.levels = U32(1); }
NV(nvnTextureBuilderSetFormat) { UNUSED; TB(P(0))->d.format = I(1); }
NV(nvnTextureBuilderSetDepthStencilMode) {
    UNUSED;
    if (I(1) != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) die("stencil-plane texture sampling");
}
NV(nvnTextureBuilderSetSwizzle) {
    UNUSED;
    nvn_tex_builder *b = TB(P(0));
    for (int i = 1; i <= 4; i++)
        if (U32(i) > 5) die("texture swizzle %u", U32(i));
    b->d.swizzle_r = U32(1), b->d.swizzle_g = U32(2), b->d.swizzle_b = U32(3), b->d.swizzle_a = U32(4);
}
NV(nvnTextureBuilderSetSamples) { UNUSED; TB(P(0))->d.samples = I(1); }
NV(nvnTextureBuilderSetStride) { UNUSED; TB(P(0))->d.stride = (uint32_t)U64(1); }
NV(nvnTextureBuilderSetStorage) {
    UNUSED;
    nvn_tex_builder *b = TB(P(0));
    b->pool = (nvn_pool *)P(1);
    b->off = (int64_t)U64(2);
}
NV(nvnTextureBuilderSetPackagedTextureLayout) {
    UNUSED;
    nvn_tex_builder *b = TB(P(0));
    const NVNpackagedTextureLayout *l = (const NVNpackagedTextureLayout *)P(1);
    b->has_layout = l != NULL;
    if (l) memcpy(b->layout, l->layout, 8);
    b->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NV(nvnTextureBuilderSetPackagedTextureData) { UNUSED; TB(P(0))->pkg_data = U64(1); }
NV(nvnTextureBuilderGetPackagedTextureData) { UNUSED; RET(CTB(P(0))->pkg_data); }
/* storage classes are opaque identifiers here: 0 buffer textures, 1 the rest (not a Tegra memory kind) */
static int storage_class(const nvn_texdesc *d) { return d->target == NVN_TEXTURE_TARGET_BUFFER ? 0 : 1; }
NV(nvnTextureBuilderGetStorageClass) { UNUSED; RET(storage_class(&CTB(P(0))->d)); }
NV(nvnTextureBuilderGetFlags) { UNUSED; RET((uint32_t)CTB(P(0))->d.flags); }
/* A size or alignment asked of no builder: 0 then (TOTK did this), and who asked, once */
static int no_builder(Ctx *c, const char *what) {
    if (c->x[0]) return 0;
    static int told;
    if (told++ < 4) hos_log("nvn: %s of no texture builder (lr %#llx): 0", what, (unsigned long long)c->x[30]);
    return 1;
}
NV(nvnTextureBuilderGetStorageSize) {
    UNUSED;
    if (no_builder(c, "storage size")) { RET(0); return; }
    nvn_texlayout L;
    nvn_tex_layout(&CTB(P(0))->d, &L);
    if (!L.bpb) hos_log("nvn: texture format %#x unknown: storage size 0", CTB(P(0))->d.format);
    RET(L.size);
}
NV(nvnTextureBuilderGetStorageAlignment) {
    UNUSED;
    if (no_builder(c, "storage alignment")) { RET(512); return; }
    nvn_texlayout L;
    nvn_tex_layout(&CTB(P(0))->d, &L);
    RET(L.align);
}
NV(nvnTextureBuilderGetZCullStorageSize) { UNUSED; RET(0); }   /* no ZCull: nothing to store */
NV(nvnTextureBuilderGetTarget) { UNUSED; RET((uint32_t)CTB(P(0))->d.target); }
NV(nvnTextureBuilderGetWidth) { UNUSED; RET(CTB(P(0))->d.width); }
NV(nvnTextureBuilderGetHeight) { UNUSED; RET(CTB(P(0))->d.height); }
NV(nvnTextureBuilderGetDepth) { UNUSED; RET(CTB(P(0))->d.depth); }
NV(nvnTextureBuilderGetLevels) { UNUSED; RET(CTB(P(0))->d.levels); }
NV(nvnTextureBuilderGetFormat) { UNUSED; RET((uint32_t)CTB(P(0))->d.format); }
NV(nvnTextureBuilderGetSamples) { UNUSED; RET((uint32_t)CTB(P(0))->d.samples); }
NV(nvnTextureBuilderGetStride) { UNUSED; RET(CTB(P(0))->d.stride); }
NV(nvnTextureBuilderGetMemoryOffset) { UNUSED; RET((uint64_t)CTB(P(0))->off); }
NV(nvnTextureBuilderGetMemoryPool) { UNUSED; RETP(CTB(P(0))->pool); }
NV(nvnTextureBuilderGetSwizzle) {
    UNUSED;
    const nvn_texdesc *d = &CTB(P(0))->d;
    *(int *)PN(1, 4) = d->swizzle_r, *(int *)PN(2, 4) = d->swizzle_g, *(int *)PN(3, 4) = d->swizzle_b,
    *(int *)PN(4, 4) = d->swizzle_a;
}
NV(nvnTextureBuilderGetViewOffset) {
    UNUSED;
    if (no_builder(c, "view offset")) { RET(0); return; }
    const nvn_tex_builder *b = CTB(P(0));
    const nvn_view *w = (const nvn_view *)P(1);
    if (!w) { RET(0); return; }
    nvn_texlayout L;
    nvn_tex_layout(&b->d, &L);
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&b->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * L.layer_size;
    RET(off);
}

/* the texture currently initialised at each storage address (nvn_tex_live_at) */
#define LIVE_SLOTS 65536
static struct { uint64_t gpu; nvn_tex t; } live[LIVE_SLOTS];
static pthread_rwlock_t live_lock = PTHREAD_RWLOCK_INITIALIZER;
static unsigned live_slot(uint64_t gpu) { return (unsigned)((gpu >> 9) * 0x9E3779B97F4A7C15ull >> 48) & (LIVE_SLOTS - 1); }
static void live_put(const nvn_tex *t) {
    if (!t->gpu) return;
    pthread_rwlock_wrlock(&live_lock);
    unsigned s = live_slot(t->gpu), k;
    for (k = 0; k < 64; k++) {   /* (open addressing over a short run: a stale entry there is simply replaced) */
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == t->gpu || !live[j].gpu) {
            live[j].gpu = t->gpu;
            live[j].t = *t;
            break;
        }
    }
    if (k == 64) {
        live[s].gpu = t->gpu;
        live[s].t = *t;
    }
    pthread_rwlock_unlock(&live_lock);
}
static void live_del(const nvn_tex *t) {
    if (!t->gpu) return;
    pthread_rwlock_wrlock(&live_lock);
    const unsigned s = live_slot(t->gpu);
    for (unsigned k = 0; k < 64; k++) {
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == t->gpu) {
            if (live[j].t.serial == t->serial) live[j].gpu = 0, live[j].t.serial = 0;
            break;
        }
    }
    pthread_rwlock_unlock(&live_lock);
}
int nvn_tex_live_at(uint64_t gpu, nvn_tex *out) {
    int found = 0;
    pthread_rwlock_rdlock(&live_lock);
    const unsigned s = live_slot(gpu);
    for (unsigned k = 0; k < 64; k++) {
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == gpu) {
            *out = live[j].t;
            found = 1;
            break;
        }
    }
    pthread_rwlock_unlock(&live_lock);
    return found;
}

NV(nvnTextureInitialize) {
    UNUSED;
    const nvn_tex_builder *b = CTB(P(1));
    nvn_tex *o = TX(P(0));
    if (o->magic == NVN_MAGIC('T', 'E', 'X', 0) && o->serial) {   /* initialised again without Finalize */
        live_del(o);
        rx_nvn_texture_released(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('T', 'E', 'X', 0);
    o->d = b->d;
    o->pool = b->pool;
    o->off = b->off;
    o->has_layout = b->has_layout;
    memcpy(o->layout, b->layout, 8);
    nvn_tex_layout(&o->d, &o->L);
    o->gpu = b->pool ? POOL(b->pool)->gpu + (uint64_t)b->off : 0;
    if (b->pkg_data) {   /* packaged data: copied into the storage */
        if (!o->gpu || !nvn_gpu_write(o->gpu, gw(b->pkg_data, o->L.size), o->L.size))
            die("packaged texture data without storage");
    }
    o->serial = new_serial();
    live_put(o);
    rx_nvn_texture_created(o);
    OLOG("texture %ux%ux%u levels %u format %#x target %d flags %#x at %#llx size %#llx", o->d.width, o->d.height,
         o->d.depth, o->d.levels, o->d.format, o->d.target, o->d.flags, (unsigned long long)o->gpu,
         (unsigned long long)o->L.size);
    RET(1);
}
NV(nvnTextureFinalize) {
    UNUSED;
    nvn_tex *o = TX(P(0));
    live_del(o);
    rx_nvn_texture_released(o);
    o->magic = 0;
}
NV(nvnTextureGetStorageClass) { UNUSED; RET(storage_class(&CTX(P(0))->d)); }
NV(nvnTextureGetFlags) { UNUSED; RET((uint32_t)CTX(P(0))->d.flags); }
NV(nvnTextureGetWidth) { UNUSED; RET(CTX(P(0))->d.width); }
NV(nvnTextureGetHeight) { UNUSED; RET(CTX(P(0))->d.height); }
NV(nvnTextureGetDepth) { UNUSED; RET(CTX(P(0))->d.depth); }
NV(nvnTextureGetLevels) { UNUSED; RET(CTX(P(0))->d.levels); }
NV(nvnTextureGetFormat) { UNUSED; RET((uint32_t)CTX(P(0))->d.format); }
NV(nvnTextureGetSamples) { UNUSED; RET((uint32_t)CTX(P(0))->d.samples); }
NV(nvnTextureGetTarget) { UNUSED; RET((uint32_t)CTX(P(0))->d.target); }
NV(nvnTextureGetStride) { UNUSED; RET(CTX(P(0))->d.stride); }
NV(nvnTextureGetStorageSize) { UNUSED; RET(CTX(P(0))->L.size); }
NV(nvnTextureGetZCullStorageSize) { UNUSED; RET(0); }
NV(nvnTextureGetTextureAddress) { UNUSED; RET(CTX(P(0))->gpu); }
NV(nvnTextureGetMemoryOffset) { UNUSED; RET((uint64_t)CTX(P(0))->off); }
NV(nvnTextureGetMemoryPool) { UNUSED; RETP(CTX(P(0))->pool); }
NV(nvnTextureGetDebugID) { UNUSED; RET(CTX(P(0))->serial); }
NV(nvnTextureSetDebugLabel) { UNUSED; (void)c; }
NV(nvnTextureGetSwizzle) {
    UNUSED;
    const nvn_texdesc *d = &CTX(P(0))->d;
    *(int *)PN(1, 4) = d->swizzle_r, *(int *)PN(2, 4) = d->swizzle_g, *(int *)PN(3, 4) = d->swizzle_b,
    *(int *)PN(4, 4) = d->swizzle_a;
}
NV(nvnTextureCompare) {
    UNUSED;
    const nvn_tex *x = CTX(P(0)), *y = CTX(P(1));
    RET(!memcmp(&x->d, &y->d, sizeof x->d) && x->gpu == y->gpu);
}
NV(nvnTextureGetRawStorageClass) { UNUSED; RET(1); }
#define VW(p) ((nvn_view *)(p))
#define CVW(p) ((const nvn_view *)(p))
NV(nvnTextureViewSetDefaults) { UNUSED; memset(P(0), 0, sizeof(nvn_view)); }
NV(nvnTextureViewSetLevels) {
    UNUSED;
    if (U32(1) > 255 || U32(2) > 255) die("texture view levels %u/%u", U32(1), U32(2));
    nvn_view *v = VW(P(0));
    v->set_levels = 1, v->base_level = (uint8_t)U32(1), v->nlevels = (uint8_t)U32(2);
}
NV(nvnTextureViewSetLayers) {
    UNUSED;
    if (U32(1) > 65535 || U32(2) > 65535) die("texture view layers %u/%u", U32(1), U32(2));
    nvn_view *v = VW(P(0));
    v->set_layers = 1, v->min_layer = (uint16_t)U32(1), v->nlayers = (uint16_t)U32(2);
}
NV(nvnTextureViewSetFormat) {
    UNUSED;
    VW(P(0))->set_format = 1;
    VW(P(0))->format = (uint16_t)U32(1);
}
NV(nvnTextureViewSetTarget) {
    UNUSED;
    VW(P(0))->set_target = 1;
    VW(P(0))->target = (uint8_t)U32(1);
}
NV(nvnTextureViewSetSwizzle) {
    UNUSED;
    nvn_view *v = VW(P(0));
    for (int i = 1; i <= 4; i++)
        if (U32(i) > 5) die("texture view swizzle %u", U32(i));
    v->set_swizzle = 1;
    for (int i = 0; i < 4; i++) v->swz[i] = (uint8_t)U32(i + 1);
}
NV(nvnTextureViewSetDepthStencilMode) {
    UNUSED;
    if (I(1) != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) die("stencil-plane texture view sampling");
}
NV(nvnTextureViewGetLevels) {
    UNUSED;
    const nvn_view *v = CVW(P(0));
    *(int *)PN(1, 4) = v->base_level, *(int *)PN(2, 4) = v->nlevels;
    RET(v->set_levels);
}
NV(nvnTextureViewGetLayers) {
    UNUSED;
    const nvn_view *v = CVW(P(0));
    *(int *)PN(1, 4) = v->min_layer, *(int *)PN(2, 4) = v->nlayers;
    RET(v->set_layers);
}
NV(nvnTextureViewGetFormat) {
    UNUSED;
    const nvn_view *v = CVW(P(0));
    *(int *)PN(1, 4) = v->format;
    RET(v->set_format);
}
NV(nvnTextureViewGetTarget) {
    UNUSED;
    const nvn_view *v = CVW(P(0));
    *(int *)PN(1, 4) = v->target;
    RET(v->set_target);
}
NV(nvnTextureGetViewOffset) {
    UNUSED;
    const nvn_tex *t = CTX(P(0));
    const nvn_view *w = CVW(P(1));
    if (!w) { RET(0); return; }
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&t->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * t->L.layer_size;
    RET(off);
}
/* the bytes of the game's buffer that ReadTexels / WriteTexels touch at row and image strides; 0 when that overflows */
static uint64_t texels_extent(uint64_t wb, uint64_t hb, uint64_t depth, uint64_t row, uint64_t img) {
    uint64_t a = 0, b = 0, n = 0;
    if (!wb || !hb) return 1;
    if (__builtin_mul_overflow(depth - 1, img, &a) || __builtin_mul_overflow(hb - 1, row, &b) ||
        __builtin_add_overflow(a, b, &n) || __builtin_add_overflow(n, wb, &n))
        return 0;
    return n;
}
static void texels(Ctx *c, int write, int strided) {
    const nvn_tex *t = CTX(PN(0, sizeof(nvn_tex)));
    const nvn_view *v = CVW(PN(1, sizeof(nvn_view)));
    const NVNcopyRegion *r = (const NVNcopyRegion *)PN(2, sizeof(NVNcopyRegion));
    int bw, bh;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    const uint64_t wb = (uint64_t)((r->width > 0 ? r->width : 0) + bw - 1) / bw * (uint64_t)(bpb > 0 ? bpb : 0);
    const uint64_t hb = (uint64_t)((r->height > 0 ? r->height : 0) + bh - 1) / bh;
    uint64_t row = strided ? U64(4) : 0, img = strided ? U64(5) : 0;
    if (!row) row = wb;
    if (!img) img = row * hb;
    const uint64_t extent = texels_extent(wb, hb, r->depth > 1 ? (uint64_t)r->depth : 1, row, img);
    if (!extent)
        die("%sTexels of %#llx rows of %#llx, %#llx apart", write ? "Write" : "Read", (unsigned long long)hb,
            (unsigned long long)wb, (unsigned long long)row);
    uint8_t *data = (uint8_t *)PN(3, extent);
    const unsigned level = v && v->set_levels ? v->base_level : 0, layer0 = v && v->set_layers ? v->min_layer : 0;
    if (v && v->set_format && v->format != t->d.format) die("%sTexels through a format view", write ? "Write" : "Read");
    if (write) rx_nvn_write_texels(t, level, layer0, r, data, row, img);
    else rx_nvn_read_texels(t, level, layer0, r, data, row, img);
}
NV(nvnTextureReadTexels) { UNUSED; texels(c, 0, 0); }
NV(nvnTextureReadTexelsStrided) { UNUSED; texels(c, 0, 1); }
NV(nvnTextureWriteTexels) { UNUSED; texels(c, 1, 0); }
NV(nvnTextureWriteTexelsStrided) { UNUSED; texels(c, 1, 1); }
NV(nvnTextureFlushTexels) { UNUSED; (void)c; }
NV(nvnTextureInvalidateTexels) { UNUSED; (void)c; }

/* ---- samplers ---- */
#define SB(x) (&((nvn_samp_builder *)(x))->p)
#define CSB(x) (&((const nvn_samp_builder *)(x))->p)
#define CSP(x) (&((const nvn_samp *)(x))->p)
NV(nvnSamplerBuilderSetDefaults) {
    UNUSED;
    nvn_samp_builder *b = (nvn_samp_builder *)P(0);
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->p.minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR;
    b->p.magf = NVN_MAG_FILTER_LINEAR;
    b->p.ws = b->p.wt = b->p.wr = NVN_WRAP_MODE_REPEAT;
    b->p.cmp_func = NVN_COMPARE_FUNC_LESS;
    b->p.lod_min = -1000.f, b->p.lod_max = 1000.f, b->p.aniso = 1.f;
}
NV(nvnSamplerBuilderSetDevice) { UNUSED; ((nvn_samp_builder *)P(0))->dev = P(1); }
NV(nvnSamplerBuilderSetMinMagFilter) {
    UNUSED;
    SB(P(0))->minf = (uint8_t)U32(1);
    SB(P(0))->magf = (uint8_t)U32(2);
}
NV(nvnSamplerBuilderSetWrapMode) {
    UNUSED;
    nvn_samp_par *p = SB(P(0));
    p->ws = (uint8_t)U32(1), p->wt = (uint8_t)U32(2), p->wr = (uint8_t)U32(3);
}
NV(nvnSamplerBuilderSetLodClamp) {
    UNUSED;
    SB(P(0))->lod_min = F(0);
    SB(P(0))->lod_max = F(1);
}
NV(nvnSamplerBuilderSetLodBias) { UNUSED; SB(P(0))->lod_bias = F(0); }
NV(nvnSamplerBuilderSetCompare) {
    UNUSED;
    SB(P(0))->cmp_mode = (uint8_t)U32(1);
    SB(P(0))->cmp_func = (uint8_t)U32(2);
}
NV(nvnSamplerBuilderSetBorderColor) { UNUSED; memcpy(SB(P(0))->border, PN(1, 16), 16); }
NV(nvnSamplerBuilderSetBorderColori) {
    UNUSED;
    const int32_t *v = (const int32_t *)PN(1, 16);
    for (int i = 0; i < 4; i++) SB(P(0))->border[i] = (float)v[i];
}
NV(nvnSamplerBuilderSetBorderColorui) {
    UNUSED;
    const uint32_t *v = (const uint32_t *)PN(1, 16);
    for (int i = 0; i < 4; i++) SB(P(0))->border[i] = (float)v[i];
}
NV(nvnSamplerBuilderSetMaxAnisotropy) { UNUSED; SB(P(0))->aniso = F(0); }
NV(nvnSamplerBuilderSetLodSnap) { UNUSED; SB(P(0))->lod_snap = F(0); }
NV(nvnSamplerBuilderSetReductionFilter) {
    UNUSED;
    if (U32(1) > 2) die("sampler reduction %u", U32(1));
    SB(P(0))->reduction = (uint8_t)U32(1);
}
NV(nvnSamplerBuilderGetMinMagFilter) {
    UNUSED;
    *(int *)PN(1, 4) = CSB(P(0))->minf;
    *(int *)PN(2, 4) = CSB(P(0))->magf;
}
NV(nvnSamplerBuilderGetWrapMode) {
    UNUSED;
    const nvn_samp_par *p = CSB(P(0));
    *(int *)PN(1, 4) = p->ws, *(int *)PN(2, 4) = p->wt, *(int *)PN(3, 4) = p->wr;
}
NV(nvnSamplerBuilderGetReductionFilter) { UNUSED; RET(CSB(P(0))->reduction); }
NV(nvnSamplerInitialize) {
    UNUSED;
    nvn_samp *o = (nvn_samp *)P(0);
    o->magic = NVN_MAGIC('S', 'M', 'P', 0);
    o->p = *CSB(P(1));
    RET(1);
}
NV(nvnSamplerFinalize) { UNUSED; ((nvn_samp *)P(0))->magic = 0; }
NV(nvnSamplerSetDebugLabel) { UNUSED; (void)c; }
NV(nvnSamplerGetMinMagFilter) {
    UNUSED;
    *(int *)PN(1, 4) = CSP(P(0))->minf;
    *(int *)PN(2, 4) = CSP(P(0))->magf;
}
NV(nvnSamplerGetWrapMode) {
    UNUSED;
    const nvn_samp_par *p = CSP(P(0));
    *(int *)PN(1, 4) = p->ws, *(int *)PN(2, 4) = p->wt, *(int *)PN(3, 4) = p->wr;
}
NV(nvnSamplerGetLodClamp) {
    UNUSED;
    *(float *)PN(1, 4) = CSP(P(0))->lod_min;
    *(float *)PN(2, 4) = CSP(P(0))->lod_max;
}
NV(nvnSamplerGetLodBias) { UNUSED; retf(c, CSP(P(0))->lod_bias); }
NV(nvnSamplerGetCompare) {
    UNUSED;
    *(int *)PN(1, 4) = CSP(P(0))->cmp_mode;
    *(int *)PN(2, 4) = CSP(P(0))->cmp_func;
}
NV(nvnSamplerGetBorderColor) { UNUSED; memcpy(PN(1, 16), CSP(P(0))->border, 16); }
NV(nvnSamplerGetMaxAnisotropy) { UNUSED; retf(c, CSP(P(0))->aniso); }
NV(nvnSamplerGetReductionFilter) { UNUSED; RET(CSP(P(0))->reduction); }
NV(nvnSamplerGetDebugID) { UNUSED; RET(U64(0)); }

/* ---- programs: the stages' code addresses and control sections (translated by the replay) ---- */
NV(nvnProgramInitialize) {
    UNUSED;
    memset(P(0), 0, sizeof(nvn_prog));
    ((nvn_prog *)P(0))->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    RET(1);
}
NV(nvnProgramFinalize) {
    UNUSED;
    rx_nvn_program_released((const nvn_prog *)P(0));
    ((nvn_prog *)P(0))->magic = 0;
}
static int set_shaders(nvn_prog *o, int n, const uint8_t *sd, int stride) {
    if (n < 0 || n > 6) return 0;
    rx_nvn_program_released(o);
    o->nstage = (uint32_t)n;
    for (int i = 0; i < n; i++) {   /* {data (GPU address), control (guest pointer)[, reserved]} */
        memcpy(&o->sd[i].data, sd + stride * i, 8);
        memcpy(&o->sd[i].control, sd + stride * i + 8, 8);
    }
    o->serial = new_serial();
    rx_nvn_program_created(o);
    return 1;
}
NV(nvnProgramSetShaders) {
    UNUSED;
    RET(set_shaders((nvn_prog *)P(0), I(1), (const uint8_t *)PN(2, I(1) > 0 ? 16ull * (uint32_t)I(1) : 0), 16));
}
/* NVNshaderDataExt: {data, control, reserved}, 0x18 bytes */
NV(nvnProgramSetShadersExt) {
    UNUSED;
    RET(set_shaders((nvn_prog *)P(0), I(1), (const uint8_t *)PN(2, I(1) > 0 ? 0x18ull * (uint32_t)I(1) : 0), 0x18));
}
NV(nvnProgramSetDebugLabel) { UNUSED; (void)c; }

/* ---- sync, events ---- */
NV(nvnSyncInitialize) {
    UNUSED;
    nvn_sync *s = (nvn_sync *)P(0);
    s->magic = NVN_MAGIC('S', 'Y', 'N', 'C'), s->fenced = 0, s->fence = 0;
    RET(1);
}
NV(nvnSyncFinalize) { UNUSED; ((nvn_sync *)P(0))->magic = 0; }
NV(nvnSyncSetDebugLabel) { UNUSED; (void)c; }
/* a fence: everything the queue was given so far is done */
NV(nvnQueueFenceSync) {
    UNUSED;
    nvn_sync *s = (nvn_sync *)P(1);
    s->fenced = 1;
    s->fence = rx_nvn_fence_q(U64(0));
}
/* never fenced: times out, as on the console; fenced: released once the GPU reached the fence */
NV(nvnSyncWait) {
    UNUSED;
    const nvn_sync *s = (const nvn_sync *)P(0);
    if (!s->fenced) { RET(NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED); return; }
    if (rx_nvn_wait(s->fence, 0)) { RET(NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED); return; }
    RET(rx_nvn_wait(s->fence, U64(1)) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
}
NV(nvnEventBuilderSetDefaults) { UNUSED; memset(P(0), 0, sizeof(nvn_event_builder)); }
NV(nvnEventBuilderSetStorage) {
    UNUSED;
    nvn_event_builder *b = (nvn_event_builder *)P(0);
    b->pool = (const nvn_pool *)P(1);
    b->off = (int64_t)U64(2);
}
NV(nvnEventInitialize) {
    UNUSED;
    const nvn_event_builder *b = (const nvn_event_builder *)P(1);
    if (!b->pool) { RET(0); return; }
    nvn_event *e = (nvn_event *)P(0);
    e->magic = NVN_MAGIC('E', 'V', 'N', 'T');
    e->gpu = POOL(b->pool)->gpu + (uint64_t)b->off;
    RET(1);
}
NV(nvnEventFinalize) { UNUSED; ((nvn_event *)P(0))->magic = 0; }
NV(nvnEventGetValue) {
    UNUSED;
    uint32_t v = 0;
    nvn_gpu_read(((const nvn_event *)P(0))->gpu, &v, 4);
    RET(v);
}
NV(nvnEventSignal) {
    UNUSED;
    const nvn_event *e = (const nvn_event *)P(0);
    uint32_t v = 0;
    if (U32(1) == NVN_EVENT_SIGNAL_MODE_ADD) nvn_gpu_read(e->gpu, &v, 4);
    v = U32(1) == NVN_EVENT_SIGNAL_MODE_ADD ? v + U32(2) : U32(2);
    nvn_gpu_write(e->gpu, &v, 4);
    rx_nvn_event_signal(e->gpu, v);
}

/* ---- queue ---- */
#define QB(p) ((nvn_queue_builder *)(p))
NV(nvnQueueBuilderSetDefaults) {
    UNUSED;
    nvn_queue_builder *b = QB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->cmd_sz = 65536, b->comp_sz = 262144, b->ctl_sz = 16384, b->flush = 4096;   /* the device's defaults */
}
NV(nvnQueueBuilderSetDevice) { UNUSED; QB(P(0))->dev = P(1); }
NV(nvnQueueBuilderSetFlags) { UNUSED; QB(P(0))->flags = U32(1); }
NV(nvnQueueBuilderSetCommandMemorySize) { UNUSED; QB(P(0))->cmd_sz = U64(1); }
NV(nvnQueueBuilderSetComputeMemorySize) { UNUSED; QB(P(0))->comp_sz = U64(1); }
NV(nvnQueueBuilderSetControlMemorySize) { UNUSED; QB(P(0))->ctl_sz = U64(1); }
NV(nvnQueueBuilderSetCommandFlushThreshold) { UNUSED; QB(P(0))->flush = U64(1); }
/* the game allocates this much (we do not use it): the three blocks, rounded to 4 KiB */
NV(nvnQueueBuilderGetQueueMemorySize) {
    UNUSED;
    const nvn_queue_builder *b = QB(P(0));
    RET((b->cmd_sz + b->comp_sz + b->ctl_sz + 4095) & ~4095ull);
}
NV(nvnQueueBuilderSetQueueMemory) {
    UNUSED;
    QB(P(0))->mem = U64(1);
    QB(P(0))->mem_sz = U64(2);
}
NV(nvnQueueInitialize) {
    UNUSED;
    nvn_queue *q = (nvn_queue *)P(0);
    q->magic = NVN_MAGIC('Q', 'U', 'E', 'U');
    q->submits = 0;
    RET(1);
}
NV(nvnQueueFinalize) { UNUSED; ((nvn_queue *)P(0))->magic = 0; }
NV(nvnQueueGetError) {
    UNUSED;
    if (P(1)) memset(P(1), 0, 16);
    RET(NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR);
}
NV(nvnQueueFlush) { UNUSED; (void)c; }
NV(nvnQueueFinish) { UNUSED; (void)c; rx_nvn_finish(); }
NV(nvnQueueSetDebugLabel) { UNUSED; (void)c; }
/* nvnQueueWaitSync(queue, sync): the queue's later work waits on the GPU for the sync's fence. Every queue replays
   in one stream in submit order, so a fence submitted before this already came first; a sync not fenced yet is
   logged. */
NV(nvnQueueWaitSync) {
    UNUSED;
    const nvn_sync *s = (const nvn_sync *)P(1);
    static int n;
    if (s && !s->fenced && __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) <= 8)
        hos_log("nvn: QueueWaitSync on a sync not fenced yet (lr %#llx): not waited for", (unsigned long long)c->x[30]);
}

/* ---- command buffers ---- */
#define CB(p) ((nvn_cmd *)(p))
#define OP_RESERVE 24

/* Recording copies and command buffer segments come from power-of-two free lists and go back to them (through the
   heap, the frees and refaults cost Longshot ~1% of a recording thread). A class keeps at most POOL_KEEP bytes. */
#define POOL_MIN 6       /* 64 bytes */
#define POOL_MAX 26      /* 64 MiB: bigger blocks use the heap */
#define POOL_KEEP (64ull << 20)
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static void *pool_head[POOL_MAX + 1];
static uint64_t pool_bytes[POOL_MAX + 1];
static void *xmalloc(size_t n, const char *what) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%s, %zu bytes)", what, n);
    return p;
}
static void *xcalloc(size_t n, size_t k, const char *what) {
    void *p = calloc(n ? n : 1, k ? k : 1);
    if (!p) die("out of memory (%s)", what);
    return p;
}
static unsigned pool_class(size_t n) {
    unsigned c = POOL_MIN;
    while (c <= POOL_MAX && ((size_t)1 << c) < n) c++;
    return c;
}
/* a block of at least n bytes; *cls its class (POOL_MAX + 1: from the heap, n bytes) */
static void *pool_get(size_t n, unsigned *cls, const char *what) {
    const unsigned c = pool_class(n);
    *cls = c;
    if (c > POOL_MAX) return xmalloc(n, what);
    pthread_mutex_lock(&pool_lock);
    void *b = pool_head[c];
    if (b) {
        pool_head[c] = *(void **)b;
        pool_bytes[c] -= (uint64_t)1 << c;
    }
    pthread_mutex_unlock(&pool_lock);
    return b ? b : xmalloc((size_t)1 << c, what);
}
static void pool_put(void *b, unsigned cls) {
    if (!b) return;
    if (cls > POOL_MAX) {
        free(b);
        return;
    }
    pthread_mutex_lock(&pool_lock);
    const int keep = pool_bytes[cls] + ((uint64_t)1 << cls) <= POOL_KEEP;
    if (keep) {
        *(void **)b = pool_head[cls];
        pool_head[cls] = b;
        pool_bytes[cls] += (uint64_t)1 << cls;
    }
    pthread_mutex_unlock(&pool_lock);
    if (!keep) free(b);
}

/* A command buffer's recording buffer lives in host memory, and its pointers in this table, not in the object (guest
   memory). The object holds its slot's number; a slot names the object it belongs to, so a slot number that is not
   the object's own is not used: the object gets a new slot. Slots are in chunks that are never freed, so a lookup
   needs no lock (a command buffer records on one thread at a time). */
typedef struct { uint64_t owner; uint8_t *seg, *cur, *end; } cmd_rec;
#define CMD_CHUNK_BITS 10
#define CMD_CHUNKS 4096u
static cmd_rec *cmd_chunks[CMD_CHUNKS];
static pthread_mutex_t cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t cmd_next = 1, *cmd_free;     /* (slot 0: none) */
static size_t ncmd_free, cap_cmd_free;
static cmd_rec *cmd_slot(uint32_t s) {
    if (!s || (s >> CMD_CHUNK_BITS) >= CMD_CHUNKS) return NULL;
    cmd_rec *k = __atomic_load_n(&cmd_chunks[s >> CMD_CHUNK_BITS], __ATOMIC_ACQUIRE);
    return k ? &k[s & ((1u << CMD_CHUNK_BITS) - 1)] : NULL;
}
static cmd_rec *cmd_adopt(nvn_cmd *o) {
    const uint64_t me = h2g(o);
    pthread_mutex_lock(&cmd_lock);
    const uint32_t s = ncmd_free ? cmd_free[--ncmd_free] : cmd_next++;
    if ((s >> CMD_CHUNK_BITS) >= CMD_CHUNKS) die("too many command buffers");
    if (!cmd_chunks[s >> CMD_CHUNK_BITS])
        __atomic_store_n(&cmd_chunks[s >> CMD_CHUNK_BITS],
                         (cmd_rec *)xcalloc((size_t)1 << CMD_CHUNK_BITS, sizeof(cmd_rec), "command buffer table"),
                         __ATOMIC_RELEASE);
    cmd_rec *r = cmd_slot(s);
    memset(r, 0, sizeof *r);
    r->owner = me;
    pthread_mutex_unlock(&cmd_lock);
    o->slot = s;
    return r;
}
static cmd_rec *cmd_host(nvn_cmd *o) {
    cmd_rec *r = cmd_slot(o->slot);
    if (r && r->owner == h2g(o)) return r;
    static int told;
    if (o->slot && __atomic_add_fetch(&told, 1, __ATOMIC_RELAXED) <= 4)
        hos_log("nvn: command buffer %#llx names slot %u, which is not its own: a new one", (unsigned long long)h2g(o),
                o->slot);
    return cmd_adopt(o);
}
static void cmd_release(nvn_cmd *o) {   /* the object's slot and recording buffer back */
    cmd_rec *r = cmd_slot(o->slot);
    if (!r || r->owner != h2g(o)) {
        o->slot = 0;
        return;
    }
    if (r->seg) pool_put(r->seg, pool_class((size_t)(r->end - r->seg)));
    pthread_mutex_lock(&cmd_lock);
    memset(r, 0, sizeof *r);
    if (ncmd_free == cap_cmd_free) {
        cap_cmd_free = cap_cmd_free ? cap_cmd_free * 2 : 256;
        cmd_free = realloc(cmd_free, cap_cmd_free * sizeof *cmd_free);
        if (!cmd_free) die("out of memory (command buffer table)");
    }
    cmd_free[ncmd_free++] = o->slot;
    pthread_mutex_unlock(&cmd_lock);
    o->slot = 0;
}
NV(nvnCommandBufferInitialize) {
    UNUSED;
    nvn_cmd *o = CB(P(0));
    if (o->magic == NVN_MAGIC('C', 'M', 'D', 'B')) cmd_release(o);   /* (initialized again without Finalize) */
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('C', 'M', 'D', 'B');
    o->inline_cur = ~0ull;   /* (NVN 55's inline fast paths never find room: every call comes here) */
    o->inline_end = 0;
    cmd_adopt(o);
    RET(1);
}
NV(nvnCommandBufferFinalize) {
    UNUSED;
    nvn_cmd *o = CB(P(0));
    cmd_release(o);
    o->magic = 0;
}
NV(nvnCommandBufferSetMemoryCallback) { UNUSED; CB(P(0))->cb = U64(1); }
NV(nvnCommandBufferSetMemoryCallbackData) { UNUSED; CB(P(0))->cb_data = U64(1); }
NV(nvnCommandBufferGetMemoryCallback) { UNUSED; RET(CB(P(0))->cb); }
NV(nvnCommandBufferGetMemoryCallbackData) { UNUSED; RET(CB(P(0))->cb_data); }
NV(nvnCommandBufferSetCommandMemoryCallbackEnabled) { UNUSED; (void)c; }
NV(nvnCommandBufferSetControlMemoryCallbackEnabled) { UNUSED; (void)c; }
NV(nvnCommandBufferSetDebugLabel) { UNUSED; (void)c; }

static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) {
    nvn_op_hdr h = {op, size};
    memcpy(p, &h, sizeof h);
}
static void cmd_reserve_rec(cmd_rec *o, size_t need) {
    if (o->seg && need + OP_RESERVE <= (size_t)(o->end - o->cur)) return;
    const size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0;
    if (!cap) cap = 64 << 10;
    while (used + need + OP_RESERVE > cap) cap *= 2;
    unsigned cls;
    uint8_t *b = (uint8_t *)pool_get(cap, &cls, "recording buffer");
    if (o->seg) {
        memcpy(b, o->seg, used);
        pool_put(o->seg, pool_class((size_t)(o->end - o->seg)));
    }
    o->seg = b, o->cur = b + used, o->end = b + cap;
}
static uint8_t *cmd_alloc(nvn_cmd *o, uint32_t size) {
    size = (size + 7) & ~7u;
    cmd_rec *r = cmd_host(o);
    cmd_reserve_rec(r, size);
    uint8_t *p = r->cur;
    r->cur += size;
    o->cmd_used += size;
    o->hw_used += size;
    o->last_rec = size;
    put_hdr(p, 0, size);
    return p;
}
/* the game sums GetCommandMemoryUsed over recordings to size its heaps: the real encoding's size where it is known
   (Maxwell methods of a real recording) instead of our record's */
static void hw_fix(nvn_cmd *o, uint64_t bytes) {
    o->hw_used += bytes;
    o->hw_used -= o->last_rec;
    o->last_rec = bytes;
}

NV(nvnCommandBufferAddCommandMemory) {
    UNUSED;
    nvn_cmd *o = CB(P(0));
    o->game_size = U64(3);
    o->cmd_used = o->hw_used = 0;
}

/* Recordings: a handle is a host copy of the finished recording. Games reuse command memory for later recordings while
   still calling earlier handles, so the copy lives until a new recording takes the control memory slot it was made in
   (the console driver's handle is a descriptor there), or until a mark written there is overwritten, and then for
   RETIRE_NS more (see let_go). HOS_NVN_CTL_LOG=<n> logs the first n control memory events.
   Live copies are kept by the 64 KiB block of their control memory position. */
typedef struct rec_node { uint64_t ctl; uint8_t *copy; struct rec_node *next; int marked; } rec_node;
#define REC_BUCKETS 65536u
#define REC_SHIFT 16
static rec_node **rec_buckets;
static size_t ncopies;
static unsigned rec_bucket(uint64_t ctl) {
    return (unsigned)(((ctl >> REC_SHIFT) * 0x9E3779B97F4A7C15ull) >> 48) & (REC_BUCKETS - 1);
}
static pthread_rwlock_t copy_lock = PTHREAD_RWLOCK_INITIALIZER;
#define COPY_MAGIC 0x59504f43444d43ull   /* "CMDCOPY" */
typedef struct { uint64_t magic, bytes, cls; } copy_prefix;   /* (cls: its pool class) */
/* released copies wait until the replay passed every submit made before their release (the replay may run behind) */
typedef struct { copy_prefix *p; uint64_t after; } dead_copy;
static dead_copy *dead;
static size_t ndead, cap_dead;
/* The live handles, a host set (open addressing): a handle the game passes back is a host address, so it is used only
   when it is in the set, never on what the memory at that address holds. Readers probe the published table without
   the lock; tables are never freed (growth doubles them). */
static uint64_t *live_handles;
static size_t live_cap, nlive;
typedef struct { uint64_t *slots; size_t cap; } live_table;
static live_table *live_pub;
static size_t live_at(uint64_t h, size_t cap) { return (size_t)((h * 0x9E3779B97F4A7C15ull) >> 24) & (cap - 1); }
static void live_handle_add(uint64_t h) {   /* (copy_lock held exclusively) */
    if ((nlive + 1) * 2 > live_cap) {
        const size_t cap = live_cap ? live_cap * 2 : 65536;
        uint64_t *t = (uint64_t *)xcalloc(cap, sizeof *t, "recording handle set");
        live_table *pub = (live_table *)xmalloc(sizeof *pub, "recording handle set");
        for (size_t i = 0; i < live_cap; i++) {
            if (!live_handles[i]) continue;
            size_t k = live_at(live_handles[i], cap);
            while (t[k]) k = (k + 1) & (cap - 1);
            t[k] = live_handles[i];
        }
        live_handles = t, live_cap = cap;
        pub->slots = t, pub->cap = cap;
        __atomic_store_n(&live_pub, pub, __ATOMIC_RELEASE);
    }
    size_t k = live_at(h, live_cap);
    while (live_handles[k] && live_handles[k] != h) k = (k + 1) & (live_cap - 1);
    if (!live_handles[k]) __atomic_store_n(&live_handles[k], h, __ATOMIC_RELEASE), nlive++;
}
static int live_handle_seen(uint64_t h) {   /* (no lock: a miss is asked again under it) */
    const live_table *t = __atomic_load_n(&live_pub, __ATOMIC_ACQUIRE);
    if (!h || !t) return 0;
    for (size_t k = live_at(h, t->cap), n = 0; n < t->cap; k = (k + 1) & (t->cap - 1), n++) {
        const uint64_t v = __atomic_load_n(&t->slots[k], __ATOMIC_ACQUIRE);
        if (v == h) return 1;
        if (!v) return 0;
    }
    return 0;
}
static int live_handle_has(uint64_t h) {   /* (copy_lock held) */
    if (!h || !live_cap) return 0;
    for (size_t k = live_at(h, live_cap); live_handles[k]; k = (k + 1) & (live_cap - 1))
        if (live_handles[k] == h) return 1;
    return 0;
}
static void live_handle_remove(uint64_t h) {   /* (copy_lock held exclusively; backward shift, no tombstones) */
    if (!h || !live_cap) return;
    size_t k = live_at(h, live_cap);
    while (live_handles[k] && live_handles[k] != h) k = (k + 1) & (live_cap - 1);
    if (!live_handles[k]) return;
    __atomic_store_n(&live_handles[k], 0, __ATOMIC_RELEASE), nlive--;
    for (size_t j = (k + 1) & (live_cap - 1); live_handles[j]; j = (j + 1) & (live_cap - 1)) {
        const size_t want = live_at(live_handles[j], live_cap);
        /* (into the hole first, then out of its slot: a reader without the lock sees it twice rather than not at all) */
        if (((j - want) & (live_cap - 1)) >= ((j - k) & (live_cap - 1))) {
            __atomic_store_n(&live_handles[k], live_handles[j], __ATOMIC_RELEASE);
            __atomic_store_n(&live_handles[j], 0, __ATOMIC_RELEASE);
            k = j;
        }
    }
}
static uint64_t dead_first = UINT64_MAX;   /* (the least `after` of the dead copies) */
static void free_dead_copies(void) {       /* (copy_lock held exclusively) */
    static int keep = -1;                  /* HOS_NVN_KEEP_COPIES=1 (debugging): never freed, so a stale handle says why */
    if (keep < 0) keep = env_on("HOS_NVN_KEEP_COPIES");
    if (keep) return;
    const uint64_t done = rx_nvn_replayed();
    if (done < dead_first) return;
    uint64_t first = UINT64_MAX;
    for (size_t i = 0; i < ndead;) {
        if (dead[i].after <= done) {
            pool_put(dead[i].p, (unsigned)dead[i].p->cls);
            dead[i] = dead[--ndead];
        } else {
            if (dead[i].after < first) first = dead[i].after;
            i++;
        }
    }
    dead_first = first;
}
static void dead_copy_add(copy_prefix *p, uint64_t after) {   /* (copy_lock held exclusively) */
    if (ndead == cap_dead) {
        cap_dead = cap_dead ? cap_dead * 2 : 1024;
        dead = realloc(dead, cap_dead * sizeof *dead);
        if (!dead) die("out of memory (recording copies let go)");
    }
    dead[ndead].p = p;
    dead[ndead].after = after;
    ndead++;
    if (after < dead_first) dead_first = after;
}
#define COPY_GONE_ADDED 0x444441454e4f47ull   /* "GONEADD": a new recording took its control memory slot */
#define COPY_GONE_SWEPT 0x5457534e4f47ull     /* "GONSWT": the mark in its control memory was overwritten */
/* (oranges) A copy let go stays valid for RETIRE_NS first: the game's threads give their command buffers the same
   control memory at once and call each other's recordings right after, so a slot taken again (or a mark overwritten)
   does not mean the handle is dead yet. Retired copies then leave the live set and are freed once the replay passed
   them. */
#define RETIRE_NS 1000000000LL
typedef struct { copy_prefix *p; uint64_t why; int64_t at; } retiring_copy;
static retiring_copy *retiring;
static size_t nretiring, cap_retiring;
static void let_go(rec_node *n, uint64_t after, uint64_t why) {   /* (copy_lock held exclusively) */
    (void)after;
    if (nretiring == cap_retiring) {
        cap_retiring = cap_retiring ? cap_retiring * 2 : 1024;
        retiring = realloc(retiring, cap_retiring * sizeof *retiring);
        if (!retiring) die("out of memory (recording copies let go)");
    }
    retiring[nretiring++] = (retiring_copy){(copy_prefix *)n->copy - 1, why, hos_now_ns() + RETIRE_NS};
    free(n);
    ncopies--;
}
static void retire_due(void) {   /* (copy_lock held exclusively) */
    const int64_t now = hos_now_ns();
    const uint64_t after = rx_nvn_pushed();
    for (size_t i = 0; i < nretiring;) {
        if (retiring[i].at > now) { i++; continue; }
        copy_prefix *p = retiring[i].p;
        p->magic = retiring[i].why;   /* (no submit names it any more; why, for the message if one does) */
        live_handle_remove((uint64_t)(uintptr_t)(p + 1));
        dead_copy_add(p, after);
        retiring[i] = retiring[--nretiring];
    }
}
static int ctl_log(void) {   /* HOS_NVN_CTL_LOG=<n> (debugging): the first n control memory events */
    static int n = -1, left;
    if (n < 0) n = getenv("HOS_NVN_CTL_LOG") ? atoi(getenv("HOS_NVN_CTL_LOG")) : 0, left = n;
    return left > 0 && __atomic_sub_fetch(&left, 1, __ATOMIC_RELAXED) >= 0;
}
static void release_copies(uint64_t lo, uint64_t hi) {
    if (hi <= lo || !rec_buckets) return;
    const uint64_t after = rx_nvn_pushed();
    pthread_rwlock_wrlock(&copy_lock);
    retire_due();
    free_dead_copies();
    const uint64_t pages = ((hi - 1) >> REC_SHIFT) - (lo >> REC_SHIFT) + 1;
    const uint64_t steps = pages > REC_BUCKETS ? REC_BUCKETS : pages;   /* (a huge range: every bucket once) */
    for (uint64_t k = 0; k < steps; k++) {
        const unsigned b = pages > REC_BUCKETS ? (unsigned)k : rec_bucket(((lo >> REC_SHIFT) + k) << REC_SHIFT);
        for (rec_node **pn = &rec_buckets[b]; *pn;) {
            rec_node *n = *pn;
            if (n->ctl < lo || n->ctl >= hi) {
                pn = &n->next;
                continue;
            }
            *pn = n->next;
            let_go(n, after, COPY_GONE_ADDED);
        }
    }
    pthread_rwlock_unlock(&copy_lock);
}
/* Copies whose control memory the game reused for other data: a mark written in the descriptor's place (a magic and
   the copy's address) that the game has overwritten means the handle is dead on the console too. A slice of the
   buckets is swept every SWEEP_EVERY recordings (every bucket once in ~25,000). */
#define COPY_MARK 0x4b52414d59504f43ull   /* "COPYMARK" */
#define SWEEP_EVERY 1024u
#define SWEEP_SLICE 2688u
static void sweep_overwritten_copies(void) {   /* (copy_lock held exclusively) */
    if (!rec_buckets) return;
    retire_due();
    free_dead_copies();
    const uint64_t after = rx_nvn_pushed();
    static unsigned next_slice;
    const unsigned from = next_slice;
    next_slice = (next_slice + SWEEP_SLICE) % REC_BUCKETS;
    for (unsigned s = 0; s < SWEEP_SLICE; s++) {
        const unsigned b = (from + s) % REC_BUCKETS;
        for (rec_node **pn = &rec_buckets[b]; *pn;) {
            rec_node *n = *pn;
            uint64_t mark[2];
            void *at = n->marked ? hos_guest_range(n->ctl, sizeof mark) : NULL;
            if (at) memcpy(mark, at, sizeof mark);
            if (!at || (mark[0] == COPY_MARK && mark[1] == (uint64_t)(uintptr_t)n->copy)) {
                pn = &n->next;
                continue;
            }
            *pn = n->next;
            let_go(n, after, COPY_GONE_SWEPT);
        }
    }
}
/* (oranges) Copies are let go when a new recording takes their control memory slot (EndRecording), not when the memory
   is added again: the game adds a command buffer's control memory again and then still submits recordings made in it,
   which the console's descriptors there allow until a new recording overwrites them. */
NV(nvnCommandBufferAddControlMemory) {
    UNUSED;
    nvn_cmd *o = CB(P(0));
    if (ctl_log()) hos_log("nvn ctl: cmd %#llx add control %#llx + %#llx (was %#llx used %#llx)", (unsigned long long)U64(0),
                           (unsigned long long)U64(1), (unsigned long long)U64(2), (unsigned long long)o->ctl,
                           (unsigned long long)o->ctl_used);
    o->ctl = U64(1);
    o->ctl_size = U64(2);
    o->ctl_used = 0;
}
static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
NV(nvnCommandBufferGetControlMemorySize) { UNUSED; RET(CB(P(0))->ctl_size); }
NV(nvnCommandBufferGetControlMemoryUsed) {
    UNUSED;
    const nvn_cmd *o = CB(P(0));
    RET(min_u64(o->ctl_used, o->ctl_size));
}
NV(nvnCommandBufferGetControlMemoryFree) {
    UNUSED;
    const nvn_cmd *o = CB(P(0));
    RET(o->ctl_size - min_u64(o->ctl_used, o->ctl_size));
}
NV(nvnCommandBufferGetCommandMemorySize) { UNUSED; RET(CB(P(0))->game_size); }
NV(nvnCommandBufferGetCommandMemoryUsed) {
    UNUSED;
    const nvn_cmd *o = CB(P(0));
    RET(min_u64(o->hw_used, o->game_size));
}
NV(nvnCommandBufferGetCommandMemoryFree) {
    UNUSED;
    const nvn_cmd *o = CB(P(0));
    RET(o->game_size - min_u64(o->hw_used, o->game_size));
}

NV(nvnCommandBufferBeginRecording) {
    UNUSED;
    nvn_cmd *o = CB(P(0));
    if (o->recording) hos_log("nvn: BeginRecording while recording");
    o->recording = 1;
    cmd_rec *r = cmd_host(o);
    r->cur = r->seg;
    o->rec_start = o->cmd_used;
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, NOP_BEGIN, 8);
    o->hw_used -= 8;   /* BEGIN / END have no hardware equivalent */
    o->hw_start = o->hw_used;
}
/* the recording, BEGIN to END, is contiguous in the command buffer's segment: one copy of it */
static uint8_t *flatten(const uint8_t *p, size_t n) {
    unsigned cls;
    uint8_t *out = (uint8_t *)pool_get(sizeof(copy_prefix) + n, &cls, "recording copy");
    memcpy(out + sizeof(copy_prefix), p, n);
    copy_prefix *pre = (copy_prefix *)out;
    pre->cls = cls;
    pre->magic = COPY_MAGIC;
    pre->bytes = sizeof(copy_prefix) + n;
    return out + sizeof(copy_prefix);
}
NV(nvnCommandBufferEndRecording) {
    UNUSED;
    nvn_cmd *o = CB(P(0));
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, NOP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    const cmd_rec *r = cmd_host(o);
    uint8_t *copy = flatten(r->seg, (size_t)(r->cur - r->seg));
    /* the copy is tied to the control memory position the console driver's descriptor would take */
    const uint64_t at = o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    o->ctl_used += 64;
    if (ctl_log()) hos_log("nvn ctl: cmd %#llx end recording at %#llx -> %p", (unsigned long long)h2g(o),
                           (unsigned long long)at, (void *)copy);
    if (o->ctl) release_copies(at, at + 64);   /* (the recordings made in this slot before: their descriptor is gone) */
    rec_node *n = (rec_node *)xmalloc(sizeof *n, "recording copy");
    n->ctl = o->ctl ? at : ~0ull;
    n->copy = copy;
    n->marked = 0;
    if (o->ctl && o->ctl_used <= o->ctl_size) {   /* (the mark in the descriptor's place: sweep_overwritten_copies) */
        const uint64_t mark[2] = {COPY_MARK, (uint64_t)(uintptr_t)copy};
        memcpy(gw(at, sizeof mark), mark, sizeof mark);
        n->marked = 1;
    }
    pthread_rwlock_wrlock(&copy_lock);
    if (!rec_buckets) rec_buckets = (rec_node **)xcalloc(REC_BUCKETS, sizeof *rec_buckets, "recording copies");
    const unsigned b = rec_bucket(n->ctl);
    n->next = rec_buckets[b];
    rec_buckets[b] = n;
    ncopies++;
    live_handle_add((uint64_t)(uintptr_t)copy);
    static uint64_t made;
    const uint64_t recording = ++made;
    if (recording % SWEEP_EVERY == 0) sweep_overwritten_copies();
    const size_t live_copies = ncopies;
    pthread_rwlock_unlock(&copy_lock);
    if (recording % 50000 == 0)
        hos_log("nvn: %llu recordings made, %zu live copies", (unsigned long long)recording, live_copies);
    RET((uint64_t)(uintptr_t)copy);
}
NV(nvnCommandBufferIsRecording) { UNUSED; RET(CB(P(0))->recording != 0); }
static void check_handle(uint64_t h) {
    int live = live_handle_seen(h);
    if (!live) {
        pthread_rwlock_rdlock(&copy_lock);
        live = live_handle_has(h);
        pthread_rwlock_unlock(&copy_lock);
    }
    const uint64_t magic = h ? ((const copy_prefix *)(uintptr_t)h - 1)->magic : 0;
    if (!live || magic != COPY_MAGIC)
        die("command handle %#llx is not a live recording (%s)", (unsigned long long)h,
            magic == COPY_GONE_ADDED ? "let go: a new recording took its control memory slot"
            : magic == COPY_GONE_SWEPT ? "let go: the mark in its control memory was overwritten"
            : live ? "in the set, without its magic" : "never made, or its memory reused");
}
/* a called recording is copied into the caller at the call (it is complete now; the game may retire it before the
   caller is submitted) */
static void inline_recording(nvn_cmd *o, uint64_t h) {
    if (ctl_log()) hos_log("nvn ctl: cmd %#llx calls %#llx", (unsigned long long)h2g(o), (unsigned long long)h);
    check_handle(h);
    const copy_prefix *pre = (const copy_prefix *)(uintptr_t)h - 1;
    const size_t total = (size_t)pre->bytes - sizeof *pre;   /* BEGIN, the records, END */
    if (total <= 16) return;
    const size_t n = total - 16;
    cmd_rec *r = cmd_host(o);
    cmd_reserve_rec(r, n);
    memcpy(r->cur, (const uint8_t *)(uintptr_t)h + 8, n);
    r->cur += n;
    o->cmd_used += n;
    o->hw_used += n;
    o->last_rec = 0;
}
NV(nvnCommandBufferCallCommands) {
    UNUSED;
    const uint64_t *h = (const uint64_t *)PN(2, 8ull * (uint32_t)I(1));
    for (int i = 0; i < I(1); i++) inline_recording(CB(P(0)), h[i]);
}
NV(nvnCommandBufferCopyCommands) {
    UNUSED;
    const uint64_t *h = (const uint64_t *)PN(2, 8ull * (uint32_t)I(1));
    for (int i = 0; i < I(1); i++) inline_recording(CB(P(0)), h[i]);
}

/* records: payload after the header, zeroed */
static void *rec(nvn_cmd *o, uint32_t op, size_t payload) {
    const uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(o, OP, T) ((T *)rec((o), (OP), sizeof(T)))
#define RECN(o, OP, T, ARR, N) ((T *)rec((o), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))

NV(nvnCommandBufferSignalEvent) {
    UNUSED;
    nvn_p_signal *s = REC(CB(P(0)), NOP_SIGNAL_EVENT, nvn_p_signal);
    s->gpu = ((const nvn_event *)P(1))->gpu;
    s->mode = U32(2);
    s->value = U32(5);
}
NV(nvnCommandBufferWaitEvent) {
    UNUSED;
    const nvn_event *e = (const nvn_event *)P(1);
    if (!e) return;
    nvn_p_wait_event *w = REC(CB(P(0)), NOP_WAIT_EVENT, nvn_p_wait_event);
    w->gpu = e->gpu, w->mode = U32(2), w->value = U32(3);
}
NV(nvnCommandBufferWaitSync) { UNUSED; (void)c; }
NV(nvnCommandBufferResetCounter) { UNUSED; REC(CB(P(0)), NOP_RESET_COUNTER, nvn_p_counter)->type = U32(1); }
NV(nvnCommandBufferReportCounter) {
    UNUSED;
    nvn_p_counter *r = REC(CB(P(0)), NOP_REPORT_COUNTER, nvn_p_counter);
    r->type = U32(1);
    r->addr = U64(2);
}
NV(nvnCommandBufferClearBuffer) {
    UNUSED;
    nvn_p_clear_buf *r = REC(CB(P(0)), NOP_CLEAR_BUF, nvn_p_clear_buf);
    r->dst = U64(1), r->size = U64(2), r->value = U32(3), r->pad = 0;
}
NV(nvnCommandBufferCopyBufferToBuffer) {
    UNUSED;
    nvn_p_copy_bb *r = REC(CB(P(0)), NOP_COPY_B2B, nvn_p_copy_bb);
    r->src = U64(1), r->dst = U64(2), r->size = U64(3);
}
/* the data is copied at record time (the caller may change it afterwards); written in GPU order at replay */
NV(nvnCommandBufferUpdateUniformBuffer) {
    UNUSED;
    const uint64_t n = U64(4);
    if (n > 0x10000) die("UpdateUniformBuffer of %#llx bytes", (unsigned long long)n);   /* (NVN's limit: 64 KiB) */
    nvn_cmd *o = CB(P(0));
    const uint32_t sz = (uint32_t)(8 + sizeof(nvn_p_update_ub) + n);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, NOP_UPDATE_UB, (sz + 7) & ~7u);
    nvn_p_update_ub u = {U64(1) + U64(3), n};
    memcpy(p + 8, &u, sizeof u);
    memcpy(p + 8 + sizeof u, PN(5, n), n);
}

static void set_view(nvn_view *dst, uint8_t *has, uint64_t v) {
    *has = v != 0;
    if (v) memcpy(dst, gw(v, sizeof *dst), sizeof *dst);
}
static void tex_copy(const nvn_tex **dst, uint64_t t);
NV(nvnCommandBufferSetRenderTargets) {
    UNUSED;
    nvn_cmd *o = CB(P(0));
    int n = I(1);
    if (n < 0 || n > 8) die("SetRenderTargets with %d targets", n);
    const uint64_t *ct = (const uint64_t *)PN(2, 8 * (size_t)n), *cv = (const uint64_t *)PN(3, 8 * (size_t)n);
    nvn_p_set_rt *r = REC(o, NOP_SET_RT, nvn_p_set_rt);
    r->n = n;
    for (int i = 0; i < n; i++) {
        tex_copy(&r->color[i], ct ? ct[i] : 0);
        set_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : 0);
    }
    tex_copy(&r->depth, U64(4));
    set_view(&r->dview, &r->has_dview, U64(5));
}
NV(nvnCommandBufferClearColor) {
    UNUSED;
    nvn_p_clear_color *r = REC(CB(P(0)), NOP_CLEAR_COLOR, nvn_p_clear_color);
    r->index = I(1);
    memcpy(r->rgba, PN(2, 16), 16);
    r->mask = I(3);
}
NV(nvnCommandBufferClearColori) {
    UNUSED;
    nvn_p_clear_color *r = REC(CB(P(0)), NOP_CLEAR_COLOR, nvn_p_clear_color);
    const int32_t *v = (const int32_t *)PN(2, 16);
    r->index = I(1);
    for (int i = 0; i < 4; i++) r->rgba[i] = (float)v[i];
    r->mask = I(3);
}
NV(nvnCommandBufferClearColorui) {
    UNUSED;
    nvn_p_clear_color *r = REC(CB(P(0)), NOP_CLEAR_COLOR, nvn_p_clear_color);
    const uint32_t *v = (const uint32_t *)PN(2, 16);
    r->index = I(1);
    for (int i = 0; i < 4; i++) r->rgba[i] = (float)v[i];
    r->mask = I(3);
}
NV(nvnCommandBufferClearDepthStencil) {
    UNUSED;
    nvn_p_clear_ds *r = REC(CB(P(0)), NOP_CLEAR_DS, nvn_p_clear_ds);
    r->depth = F(0);
    r->depth_mask = BOOL(1);
    r->stencil = I(2);
    r->stencil_mask = I(3);
}
NV(nvnCommandBufferSetViewports) {
    UNUSED;
    const int first = I(1), n = I(2);
    if (n < 0 || first < 0 || first + n > 16) die("SetViewports %d+%d", first, n);
    nvn_p_viewports *r = REC(CB(P(0)), NOP_VIEWPORTS, nvn_p_viewports);
    r->first = first, r->n = n;
    memcpy(r->v, PN(3, 16 * (size_t)n), 16 * (size_t)n);
}
NV(nvnCommandBufferSetViewport) {
    UNUSED;
    nvn_p_viewports *r = REC(CB(P(0)), NOP_VIEWPORTS, nvn_p_viewports);
    r->first = 0, r->n = 1;
    r->v[0][0] = (float)I(1), r->v[0][1] = (float)I(2), r->v[0][2] = (float)I(3), r->v[0][3] = (float)I(4);
}
NV(nvnCommandBufferSetScissor) {
    UNUSED;
    nvn_p_scissor *r = REC(CB(P(0)), NOP_SCISSOR, nvn_p_scissor);
    r->x = I(1), r->y = I(2), r->w = I(3), r->h = I(4);
}
/* one scissor and depth range are replayed (index 0); differing others are reported */
NV(nvnCommandBufferSetScissors) {
    UNUSED;
    const int first = I(1), n = I(2);
    const int *v = (const int *)PN(3, n > 0 ? 16 * (size_t)n : 0);
    for (int i = 1; i < n; i++)
        if (memcmp(v + 4 * i, v, 16)) {
            static int once;
            if (!once++) hos_log("nvn: SetScissors: scissor %d differs (index 0 used)", first + i);
        }
    if (first == 0 && n > 0) {
        nvn_p_scissor *r = REC(CB(P(0)), NOP_SCISSOR, nvn_p_scissor);
        r->x = v[0], r->y = v[1], r->w = v[2], r->h = v[3];
    }
}
NV(nvnCommandBufferSetDepthRange) {
    UNUSED;
    nvn_p_depth_range *r = REC(CB(P(0)), NOP_DEPTH_RANGE, nvn_p_depth_range);
    r->n = F(0), r->f = F(1);
}
NV(nvnCommandBufferSetDepthRanges) {
    UNUSED;
    const int first = I(1), n = I(2);
    const float *v = (const float *)PN(3, n > 0 ? 8 * (size_t)n : 0);
    if (first == 0 && n > 0) {
        nvn_p_depth_range *r = REC(CB(P(0)), NOP_DEPTH_RANGE, nvn_p_depth_range);
        r->n = v[0], r->f = v[1];
    }
}
NV(nvnCommandBufferSetDepthClamp) { UNUSED; REC(CB(P(0)), NOP_DEPTH_CLAMP, nvn_p_flag)->value = BOOL(1) != 0; }
NV(nvnCommandBufferSetRenderEnable) { UNUSED; REC(CB(P(0)), NOP_RENDER_ENABLE, nvn_p_flag)->value = BOOL(1) != 0; }
/* rendering on when the two NVNcounterData.counter values at addr, addr + 16 are equal (mode 0) / differ (1) */
NV(nvnCommandBufferSetRenderEnableConditional) {
    UNUSED;
    if (I(1) != 0 && I(1) != 1) die("SetRenderEnableConditional mode %d", I(1));
    nvn_p_render_cond *r = REC(CB(P(0)), NOP_RENDER_COND, nvn_p_render_cond);
    r->mode = I(1);
    r->addr = U64(2);
}
NV(nvnCommandBufferSetRasterizerDiscard) {
    UNUSED;
    REC(CB(P(0)), NOP_RASTER_DISCARD, nvn_p_flag)->value = BOOL(1) != 0;
}
NV(nvnCommandBufferBindProgram) {
    UNUSED;
    nvn_p_bind_program *r = REC(CB(P(0)), NOP_BIND_PROGRAM, nvn_p_bind_program);
    r->prog = (const nvn_prog *)P(1);
    r->stages = I(2);
}
static void bind_state(Ctx *c, int kind, size_t n) {
    nvn_p_bind_state *r = REC(CB(P(0)), NOP_BIND_STATE, nvn_p_bind_state);
    r->kind = kind;
    memcpy(r->bytes, PN(1, n), n);
}
NV(nvnCommandBufferBindBlendState) { UNUSED; bind_state(c, 0, sizeof(nvn_blend_st)); }
NV(nvnCommandBufferBindColorState) { UNUSED; bind_state(c, 1, sizeof(nvn_color_st)); }
NV(nvnCommandBufferBindChannelMaskState) { UNUSED; bind_state(c, 2, sizeof(nvn_chmask_st)); }
NV(nvnCommandBufferBindMultisampleState) { UNUSED; bind_state(c, 3, sizeof(nvn_ms_st)); }
NV(nvnCommandBufferBindPolygonState) { UNUSED; bind_state(c, 4, sizeof(nvn_poly_st)); }
NV(nvnCommandBufferBindDepthStencilState) { UNUSED; bind_state(c, 5, sizeof(nvn_ds_st)); }
NV(nvnCommandBufferBindVertexAttribState) {
    UNUSED;
    const int n = I(1);
    if (n < 0 || n > 16) die("BindVertexAttribState %d", n);
    nvn_cmd *o = CB(P(0));
    nvn_p_bind_vattr *r = RECN(o, NOP_BIND_VATTR, nvn_p_bind_vattr, st, n);
    r->first = 0, r->n = n;
    memcpy(r->st, PN(2, 4 * (size_t)n), 4 * (size_t)n);
    hw_fix(o, 68);   /* the driver writes all 16 attribute formats: 17 words */
}
NV(nvnCommandBufferBindVertexStreamState) {
    UNUSED;
    const int n = I(1);
    if (n < 0 || n > 16) die("BindVertexStreamState %d", n);
    nvn_cmd *o = CB(P(0));
    nvn_p_bind_vstream *r = RECN(o, NOP_BIND_VSTREAM, nvn_p_bind_vstream, st, n);
    r->first = 0, r->n = n;
    memcpy(r->st, PN(2, 8 * (size_t)n), 8 * (size_t)n);
    hw_fix(o, 4 * (16 + (uint64_t)n));
}
static void bind_vbufs(nvn_cmd *o, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first < 0 || first + n > 16) die("BindVertexBuffers %d+%d", first, n);
    nvn_p_bind_vbuf *r = RECN(o, NOP_BIND_VBUF, nvn_p_bind_vbuf, r, n);
    r->first = first, r->n = n;
    memcpy(r->r, rg, sizeof *rg * (size_t)n);
    hw_fix(o, 24 * (uint64_t)n);
}
NV(nvnCommandBufferBindVertexBuffers) {
    UNUSED;
    bind_vbufs(CB(P(0)), I(1), I(2), (const NVNbufferRange *)PN(3, I(2) > 0 ? 16 * (size_t)I(2) : 0));
}
NV(nvnCommandBufferBindVertexBuffer) {
    UNUSED;
    NVNbufferRange r = {U64(2), U64(3)};
    bind_vbufs(CB(P(0)), I(1), 1, &r);
}
static void bind_buf(nvn_cmd *o, uint32_t op, int stage, int index, uint64_t addr, uint64_t size) {
    nvn_p_bind_buf *r = REC(o, op, nvn_p_bind_buf);
    r->stage = stage, r->index = index, r->addr = addr, r->size = size;
    hw_fix(o, 20);
}
NV(nvnCommandBufferBindUniformBuffer) { UNUSED; bind_buf(CB(P(0)), NOP_BIND_UBO, I(1), I(2), U64(3), U64(4)); }
NV(nvnCommandBufferBindStorageBuffer) { UNUSED; bind_buf(CB(P(0)), NOP_BIND_SSBO, I(1), I(2), U64(3), U64(4)); }
static void bind_bufs(Ctx *c, uint32_t op) {
    const NVNbufferRange *r = (const NVNbufferRange *)PN(4, I(3) > 0 ? sizeof *r * (uint64_t)I(3) : 0);
    for (int i = 0; i < I(3); i++) bind_buf(CB(P(0)), op, I(1), I(2) + i, r[i].address, r[i].size);
}
NV(nvnCommandBufferBindUniformBuffers) { UNUSED; bind_bufs(c, NOP_BIND_UBO); }
NV(nvnCommandBufferBindStorageBuffers) { UNUSED; bind_bufs(c, NOP_BIND_SSBO); }
/* deferred: the ranges are read from the game's array at submit (TOTK fills it for the frame just before) */
static void bind_bufs_deferred(Ctx *c, int ssbo) {
    const int n = I(3);
    if (n < 0 || n > 16) die("deferred bind of %d buffers", n);
    nvn_p_bind_bufs *r = RECN(CB(P(0)), NOP_BIND_BUFS, nvn_p_bind_bufs, r, n);
    r->ssbo = ssbo, r->stage = I(1), r->first = I(2), r->n = n, r->src = U64(4);
    memcpy(r->r, PN(4, sizeof(NVNbufferRange) * (size_t)n), sizeof(NVNbufferRange) * (size_t)n);
    hw_fix(CB(P(0)), 16 + 16 * (uint64_t)n);
}
NV(nvnCommandBufferBindUniformBuffersDeferred) { UNUSED; bind_bufs_deferred(c, 0); }
NV(nvnCommandBufferBindStorageBuffersDeferred) { UNUSED; bind_bufs_deferred(c, 1); }
static void bind_handles(nvn_cmd *o, uint32_t op, int stage, int first, int n, const uint64_t *h, uint64_t src,
                         uint64_t hw) {
    if (n < 0 || n > 32) die("bind of %d handles", n);
    nvn_p_bind_handles *r = RECN(o, op, nvn_p_bind_handles, h, n);
    r->stage = stage, r->first = first, r->n = n, r->src = src;
    if (n) memcpy(r->h, h, 8 * (size_t)n);
    hw_fix(o, hw ? hw : 16 + 4 * (uint64_t)n);
}
#define HANDLES(i, n) ((const uint64_t *)PN(i, (n) > 0 ? 8 * (size_t)(n) : 0))
NV(nvnCommandBufferBindTexture) {
    UNUSED;
    const uint64_t h = U64(3);
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), 1, &h, 0, 0);
}
NV(nvnCommandBufferBindTextures) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), HANDLES(4, I(3)), 0, 0);
}
NV(nvnCommandBufferBindImage) {
    UNUSED;
    const uint64_t h = U64(3);
    bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), 1, &h, 0, 24);
}
NV(nvnCommandBufferBindImages) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), I(3), HANDLES(4, I(3)), 0, 16 + 8 * (uint64_t)I(3));
}
NV(nvnCommandBufferBindSeparateTexture) {
    UNUSED;
    const uint64_t h = U64(3);
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), 1, &h, 0, 24);
}
NV(nvnCommandBufferBindSeparateTextures) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), HANDLES(4, I(3)), 0, 16 + 8 * (uint64_t)I(3));
}
NV(nvnCommandBufferBindSeparateSampler) {
    UNUSED;
    const uint64_t h = U64(3);
    bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), 1, &h, 0, 24);
}
NV(nvnCommandBufferBindSeparateSamplers) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), I(3), HANDLES(4, I(3)), 0, 16 + 8 * (uint64_t)I(3));
}
/* NVN 55 *Deferred binds (what the inline fast paths call: always, see nvn_cmd.inline_cur): the console driver only
   records the game's handle array and reads it at submit; the game fills material tables after recording */
NV(nvnCommandBufferBindTexturesDeferred) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), HANDLES(4, I(3)), U64(4), 0);
}
NV(nvnCommandBufferBindImagesDeferred) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), I(3), HANDLES(4, I(3)), U64(4), 16 + 8 * (uint64_t)I(3));
}
NV(nvnCommandBufferBindSeparateTexturesDeferred) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), HANDLES(4, I(3)), U64(4), 16 + 8 * (uint64_t)I(3));
}
NV(nvnCommandBufferBindSeparateSamplersDeferred) {
    UNUSED;
    bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), I(3), HANDLES(4, I(3)), U64(4), 16 + 8 * (uint64_t)I(3));
}
static void stencil(Ctx *c, int which) {
    nvn_p_stencil *r = REC(CB(P(0)), NOP_STENCIL, nvn_p_stencil);
    r->face = I(1), r->which = which;
    if (which == 0) r->value_mask = I(2);
    else if (which == 1) r->mask = I(2);
    else r->ref = I(2);
}
NV(nvnCommandBufferSetStencilValueMask) { UNUSED; stencil(c, 0); }
NV(nvnCommandBufferSetStencilMask) { UNUSED; stencil(c, 1); }
NV(nvnCommandBufferSetStencilRef) { UNUSED; stencil(c, 2); }
NV(nvnCommandBufferSetPolygonOffsetClamp) {
    UNUSED;
    nvn_p_poly_offset *r = REC(CB(P(0)), NOP_POLY_OFFSET, nvn_p_poly_offset);
    r->factor = F(0), r->units = F(1), r->clamp = F(2);
}
NV(nvnCommandBufferSetPrimitiveRestart) {
    UNUSED;
    nvn_p_restart *r = REC(CB(P(0)), NOP_PRIMITIVE_RESTART, nvn_p_restart);
    r->enable = BOOL(1), r->index = I(2);
}
NV(nvnCommandBufferDrawArrays) {
    UNUSED;
    nvn_p_draw *r = REC(CB(P(0)), NOP_DRAW, nvn_p_draw);
    r->prim = I(1), r->first = I(2), r->count = I(3), r->instances = 1;
}
NV(nvnCommandBufferDrawArraysInstanced) {
    UNUSED;
    if (I(5) <= 0) return;   /* no instances: nothing */
    nvn_p_draw *r = REC(CB(P(0)), NOP_DRAW, nvn_p_draw);
    r->prim = I(1), r->first = I(2), r->count = I(3), r->base_instance = I(4), r->instances = I(5);
}
static void draw_idx(Ctx *c, int base_vertex, int base_instance, int instances, uint64_t hw) {
    nvn_cmd *o = CB(P(0));
    nvn_p_draw_idx *r = REC(o, NOP_DRAW_IDX, nvn_p_draw_idx);
    r->prim = I(1), r->itype = I(2), r->count = I(3), r->ibuf = U64(4);
    r->base_vertex = base_vertex, r->base_instance = base_instance, r->instances = instances;
    hw_fix(o, hw);
}
NV(nvnCommandBufferDrawElements) { UNUSED; draw_idx(c, 0, 0, 1, 48); }
NV(nvnCommandBufferDrawElementsBaseVertex) { UNUSED; draw_idx(c, I(5), 0, 1, 44); }
NV(nvnCommandBufferDrawElementsInstanced) {
    UNUSED;
    if (I(7) > 0) draw_idx(c, I(5), I(6), I(7), 44);
}
/* GPU-driven draws: the arguments are read at replay, after the GPU's writes reached memory */
static void draw_indirect(Ctx *c, int indexed, int prim, int itype, uint64_t ibuf, uint64_t indirect,
                          uint64_t count_addr, int max, int stride) {
    nvn_p_draw_indirect *r = REC(CB(P(0)), NOP_DRAW_INDIRECT, nvn_p_draw_indirect);
    r->indexed = indexed, r->prim = prim, r->itype = itype, r->max = max, r->stride = stride;
    r->ibuf = ibuf, r->indirect = indirect, r->count_addr = count_addr;
}
/* DrawArraysIndirect(cmd, prim, indirect); DrawElementsIndirect(cmd, prim, index type, index buffer, indirect) */
NV(nvnCommandBufferDrawArraysIndirect) { UNUSED; draw_indirect(c, 0, I(1), 0, 0, U64(2), 0, 1, 16); }
NV(nvnCommandBufferDrawElementsIndirect) { UNUSED; draw_indirect(c, 1, I(1), I(2), U64(3), U64(4), 0, 1, 20); }
/* MultiDraw*IndirectCount(cmd, prim, [index type, index buffer,] indirect, count address, max draws, stride) */
NV(nvnCommandBufferMultiDrawArraysIndirectCount) {
    UNUSED;
    draw_indirect(c, 0, I(1), 0, 0, U64(2), U64(3), I(4), I(5));
}
NV(nvnCommandBufferMultiDrawElementsIndirectCount) {
    UNUSED;
    draw_indirect(c, 1, I(1), I(2), U64(3), U64(4), U64(5), I(6), I(7));
}
NV(nvnCommandBufferDispatchCompute) {
    UNUSED;
    nvn_p_dispatch *r = REC(CB(P(0)), NOP_DISPATCH, nvn_p_dispatch);
    r->x = I(1), r->y = I(2), r->z = I(3);
}
NV(nvnCommandBufferDispatchComputeIndirect) {
    UNUSED;
    REC(CB(P(0)), NOP_DISPATCH_INDIRECT, nvn_p_dispatch_indirect)->addr = U64(1);
}
NV(nvnCommandBufferBarrier) { UNUSED; *(int32_t *)rec(CB(P(0)), NOP_BARRIER, 8) = I(1); }
NV(nvnCommandBufferSetTexturePool) {
    UNUSED;
    REC(CB(P(0)), NOP_SET_TEX_POOL, nvn_p_set_pool)->pool = (const nvn_dpool *)P(1);
}
NV(nvnCommandBufferSetSamplerPool) {
    UNUSED;
    REC(CB(P(0)), NOP_SET_SAMP_POOL, nvn_p_set_pool)->pool = (const nvn_dpool *)P(1);
}
NV(nvnCommandBufferSetShaderScratchMemory) { UNUSED; (void)c; }

/* Records keep textures by pointer to an immutable copy: the game may finalize or reuse a texture object after
   recording, before the submit. One copy per distinct content (the object with its serial left out); copies live for
   the session (~100 bytes each); the table doubles when half full. */
static nvn_tex **pins;
static size_t pin_slots, npins;
static pthread_mutex_t pin_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t pin_hash(const nvn_tex *o) {   /* (FNV-1a over the object, its serial as 0) */
    nvn_tex k = *o;
    k.serial = 0;
    const uint8_t *p = (const uint8_t *)&k;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static int pin_same(const nvn_tex *a, const nvn_tex *b) {
    nvn_tex x = *a, y = *b;
    x.serial = y.serial = 0;
    return !memcmp(&x, &y, sizeof x);
}
static void pins_grow(void) {
    const size_t n = pin_slots ? pin_slots * 2 : (1u << 16);
    nvn_tex **t = (nvn_tex **)xcalloc(n, sizeof *t, "texture copies table");
    for (size_t i = 0; i < pin_slots; i++)
        if (pins[i]) {
            size_t s = (size_t)(pin_hash(pins[i]) >> 20) & (n - 1);
            while (t[s]) s = (s + 1) & (n - 1);
            t[s] = pins[i];
        }
    free(pins);
    pins = t, pin_slots = n;
}
static void tex_copy(const nvn_tex **dst, uint64_t t) {
    if (!t) { *dst = NULL; return; }
    const nvn_tex *o = (const nvn_tex *)gw(t, sizeof(nvn_tex));
    if (!o->serial) die("recording refers to an uninitialized texture object %#llx", (unsigned long long)t);
    const uint64_t h = pin_hash(o);
    pthread_mutex_lock(&pin_lock);
    if (npins * 2 >= pin_slots) pins_grow();
    size_t s = (size_t)(h >> 20) & (pin_slots - 1);
    while (pins[s] && !pin_same(pins[s], o)) s = (s + 1) & (pin_slots - 1);
    if (!pins[s]) {
        pins[s] = (nvn_tex *)xmalloc(sizeof(nvn_tex), "texture copy");
        *pins[s] = *o;
        npins++;
    }
    *dst = pins[s];
    pthread_mutex_unlock(&pin_lock);
}
NV(nvnCommandBufferCopyBufferToTexture) {
    UNUSED;
    nvn_p_copy_bt *r = REC(CB(P(0)), NOP_COPY_B2T, nvn_p_copy_bt);
    r->buf = U64(1);
    tex_copy(&r->tex, U64(2));
    set_view(&r->view, &r->has_view, U64(3));
    r->r = *(const NVNcopyRegion *)PN(4, sizeof(NVNcopyRegion));
    r->flags = I(5);
}
NV(nvnCommandBufferCopyTextureToBuffer) {
    UNUSED;
    nvn_p_copy_bt *r = REC(CB(P(0)), NOP_COPY_T2B, nvn_p_copy_bt);
    tex_copy(&r->tex, U64(1));
    set_view(&r->view, &r->has_view, U64(2));
    r->r = *(const NVNcopyRegion *)PN(3, sizeof(NVNcopyRegion));
    r->buf = U64(4);
    r->flags = I(5);
}
NV(nvnCommandBufferCopyTextureToTexture) {
    UNUSED;
    nvn_p_copy_tt *r = REC(CB(P(0)), NOP_COPY_T2T, nvn_p_copy_tt);
    tex_copy(&r->src, U64(1));
    set_view(&r->sv, &r->has_sv, U64(2));
    r->sr = *(const NVNcopyRegion *)PN(3, sizeof(NVNcopyRegion));
    tex_copy(&r->dst, U64(4));
    set_view(&r->dv, &r->has_dv, U64(5));
    r->dr = *(const NVNcopyRegion *)PN(6, sizeof(NVNcopyRegion));
    r->flags = I(7);
}
static void clear_tex(Ctx *c, int is_uint) {
    nvn_p_clear_tex *r = REC(CB(P(0)), NOP_CLEAR_TEX, nvn_p_clear_tex);
    tex_copy(&r->tex, U64(1));
    set_view(&r->view, &r->has_view, U64(2));
    r->r = *(const NVNcopyRegion *)PN(3, sizeof(NVNcopyRegion));
    memcpy(r->c.f, PN(4, 16), 16);
    r->mask = I(5);
    r->is_uint = (uint8_t)is_uint;
}
NV(nvnCommandBufferClearTexture) { UNUSED; clear_tex(c, 0); }
NV(nvnCommandBufferClearTextureui) { UNUSED; clear_tex(c, 1); }
NV(nvnCommandBufferSetCopyRowStride) {
    UNUSED;
    REC(CB(P(0)), NOP_COPY_STRIDE, nvn_p_copy_stride)->stride = (int64_t)U64(1);
}
NV(nvnCommandBufferSetCopyImageStride) {   /* (the replay copies layers tightly packed: a real stride is logged) */
    UNUSED;
    static int once;
    if (U64(1) && !once++) hos_log("nvn: SetCopyImageStride(%lld): not replayed", (long long)U64(1));
}
NV(nvnCommandBufferSetBlendColor) {
    UNUSED;
    memcpy(REC(CB(P(0)), NOP_BLEND_COLOR, nvn_p_blend_color)->rgba, PN(1, 16), 16);
}

/* hints and state with no effect on the picture here: lines and points come out of the shaders, ZCull and the tiled
   cache are Tegra hardware; non-default values are reported once */
NV(nvnCommandBufferSetLineWidth) {
    UNUSED;
    static int once;
    if (F(0) != 1.0f && !once++) hos_log("nvn: SetLineWidth %g: lines are one pixel", F(0));
}
NV(nvnCommandBufferSetPointSize) { UNUSED; (void)c; }
NV(nvnCommandBufferSetDepthBounds) {
    UNUSED;
    static int once;
    if (BOOL(1) && (F(0) > 0.0f || F(1) < 1.0f) && !once++)
        hos_log("nvn: SetDepthBounds [%g, %g]: no depth bounds test", F(0), F(1));
}
NV(nvnCommandBufferSetTiledCacheAction) { UNUSED; (void)c; }
NV(nvnCommandBufferSetTiledCacheTileSize) { UNUSED; (void)c; }
NV(nvnCommandBufferSetSubpixelPrecisionBias) { UNUSED; (void)c; }
NV(nvnCommandBufferDiscardColor) { UNUSED; (void)c; }
NV(nvnCommandBufferDiscardDepthStencil) { UNUSED; (void)c; }
NV(nvnCommandBufferSaveZCullData) { UNUSED; (void)c; }
NV(nvnCommandBufferRestoreZCullData) { UNUSED; (void)c; }
NV(nvnCommandBufferSetAlphaRef) { UNUSED; (void)c; }
NV(nvnCommandBufferSetSampleMask) { UNUSED; (void)c; }
NV(nvnCommandBufferSetConservativeRasterEnable) { UNUSED; (void)c; }
NV(nvnCommandBufferSetConservativeRasterDilate) { UNUSED; (void)c; }
NV(nvnCommandBufferSetPatchSize) { UNUSED; (void)c; }
NV(nvnCommandBufferSetInnerTessellationLevels) { UNUSED; (void)c; }
NV(nvnCommandBufferSetOuterTessellationLevels) { UNUSED; (void)c; }
NV(nvnCommandBufferSetWindowRectangles) { UNUSED; (void)c; }
NV(nvnCommandBufferPushDebugGroupStatic) { UNUSED; (void)c; }
NV(nvnCommandBufferPushDebugGroupDynamic) { UNUSED; (void)c; }
NV(nvnCommandBufferPushDebugGroup) { UNUSED; (void)c; }
NV(nvnCommandBufferPopDebugGroup) { UNUSED; (void)c; }
NV(nvnCommandBufferPopDebugGroupId) { UNUSED; (void)c; }
NV(nvnCommandBufferInsertDebugMarkerStatic) { UNUSED; (void)c; }
NV(nvnCommandBufferInsertDebugMarkerDynamic) { UNUSED; (void)c; }
NV(nvnCommandBufferInsertDebugMarker) { UNUSED; (void)c; }

/* ---- state objects: packed in place ---- */
#define ST(T, p) ((T *)(p))
NV(nvnBlendStateSetDefaults) {
    UNUSED;
    nvn_blend_st *b = ST(nvn_blend_st, P(0));
    memset(b, 0, sizeof *b);
    b->src_rgb = NVN_BLEND_FUNC_ONE, b->dst_rgb = NVN_BLEND_FUNC_ZERO, b->src_a = NVN_BLEND_FUNC_ONE,
    b->dst_a = NVN_BLEND_FUNC_ZERO;
    b->eq_rgb = NVN_BLEND_EQUATION_ADD, b->eq_a = NVN_BLEND_EQUATION_ADD;
}
NV(nvnBlendStateSetBlendTarget) { UNUSED; ST(nvn_blend_st, P(0))->target = U32(1); }
NV(nvnBlendStateSetBlendFunc) {
    UNUSED;
    nvn_blend_st *b = ST(nvn_blend_st, P(0));
    b->src_rgb = U32(1), b->dst_rgb = U32(2), b->src_a = U32(3), b->dst_a = U32(4);
}
NV(nvnBlendStateSetBlendEquation) {
    UNUSED;
    ST(nvn_blend_st, P(0))->eq_rgb = U32(1);
    ST(nvn_blend_st, P(0))->eq_a = U32(2);
}
NV(nvnBlendStateSetAdvancedMode) {
    UNUSED;
    if (I(1)) die("advanced blend mode %d", I(1));
}
NV(nvnBlendStateSetAdvancedOverlap) { UNUSED; (void)c; }
NV(nvnBlendStateSetAdvancedPremultipliedSrc) { UNUSED; (void)c; }
NV(nvnBlendStateSetAdvancedNormalizedDst) { UNUSED; (void)c; }
NV(nvnBlendStateGetBlendTarget) { UNUSED; RET(ST(nvn_blend_st, P(0))->target); }
NV(nvnColorStateSetDefaults) {
    UNUSED;
    nvn_color_st *s = ST(nvn_color_st, P(0));
    memset(s, 0, sizeof *s);
    s->logic_op = NVN_LOGIC_OP_COPY;
    s->alpha_func = NVN_ALPHA_FUNC_ALWAYS;
}
NV(nvnColorStateSetBlendEnable) {
    UNUSED;
    nvn_color_st *s = ST(nvn_color_st, P(0));
    const int i = I(1);
    if (i < 0 || i > 7) return;
    s->blend_en = BOOL(2) ? (s->blend_en | 1u << i) : (s->blend_en & ~(1u << i));
}
NV(nvnColorStateSetLogicOp) { UNUSED; ST(nvn_color_st, P(0))->logic_op = U32(1); }
NV(nvnColorStateSetAlphaTest) { UNUSED; ST(nvn_color_st, P(0))->alpha_func = U32(1); }
NV(nvnChannelMaskStateSetDefaults) { UNUSED; ST(nvn_chmask_st, P(0))->mask = 0xffffffffu; }
NV(nvnChannelMaskStateSetChannelMask) {
    UNUSED;
    const int i = I(1);
    if (i < 0 || i > 7) return;
    const uint32_t m = (BOOL(2) ? 1 : 0) | (BOOL(3) ? 2 : 0) | (BOOL(4) ? 4 : 0) | (BOOL(5) ? 8 : 0);
    nvn_chmask_st *s = ST(nvn_chmask_st, P(0));
    s->mask = (s->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NV(nvnMultisampleStateSetDefaults) {
    UNUSED;
    memset(P(0), 0, sizeof(nvn_ms_st));
    ST(nvn_ms_st, P(0))->enable = 1;
}
NV(nvnMultisampleStateSetMultisampleEnable) { UNUSED; ST(nvn_ms_st, P(0))->enable = BOOL(1) != 0; }
NV(nvnMultisampleStateSetSamples) { UNUSED; ST(nvn_ms_st, P(0))->samples = (uint8_t)I(1); }
NV(nvnMultisampleStateSetAlphaToCoverageEnable) { UNUSED; ST(nvn_ms_st, P(0))->a2c = BOOL(1) != 0; }
NV(nvnMultisampleStateSetAlphaToCoverageDither) { UNUSED; ST(nvn_ms_st, P(0))->a2c_dither = BOOL(1) != 0; }
NV(nvnMultisampleStateSetRasterSamples) { UNUSED; ST(nvn_ms_st, P(0))->raster_samples = (uint8_t)I(1); }
NV(nvnMultisampleStateSetSampleLocationsEnable) { UNUSED; ST(nvn_ms_st, P(0))->sl_en = BOOL(1) != 0; }
NV(nvnMultisampleStateSetSampleLocations) { UNUSED; (void)c; }
NV(nvnPolygonStateSetDefaults) {
    UNUSED;
    nvn_poly_st *s = ST(nvn_poly_st, P(0));
    memset(s, 0, sizeof *s);
    s->cull = NVN_FACE_NONE, s->front = NVN_FRONT_FACE_CCW, s->mode = NVN_POLYGON_MODE_FILL;
}
NV(nvnPolygonStateSetCullFace) { UNUSED; ST(nvn_poly_st, P(0))->cull = U32(1); }
NV(nvnPolygonStateSetFrontFace) { UNUSED; ST(nvn_poly_st, P(0))->front = U32(1); }
NV(nvnPolygonStateSetPolygonMode) { UNUSED; ST(nvn_poly_st, P(0))->mode = U32(1); }
NV(nvnPolygonStateSetPolygonOffsetEnables) { UNUSED; ST(nvn_poly_st, P(0))->offset_en = U32(1); }
NV(nvnDepthStencilStateSetDefaults) {
    UNUSED;
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    memset(s, 0, sizeof *s);
    s->dw = 1, s->df = NVN_DEPTH_FUNC_LESS, s->sf_f = s->sf_b = NVN_STENCIL_FUNC_ALWAYS;
    s->fail_f = s->zfail_f = s->zpass_f = s->fail_b = s->zfail_b = s->zpass_b = NVN_STENCIL_OP_KEEP;
}
NV(nvnDepthStencilStateSetDepthTestEnable) { UNUSED; ST(nvn_ds_st, P(0))->dt = BOOL(1) != 0; }
NV(nvnDepthStencilStateSetDepthWriteEnable) { UNUSED; ST(nvn_ds_st, P(0))->dw = BOOL(1) != 0; }
NV(nvnDepthStencilStateSetDepthFunc) { UNUSED; ST(nvn_ds_st, P(0))->df = U32(1); }
NV(nvnDepthStencilStateSetStencilTestEnable) { UNUSED; ST(nvn_ds_st, P(0))->st = BOOL(1) != 0; }
NV(nvnDepthStencilStateSetStencilFunc) {
    UNUSED;
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    if (U32(1) & NVN_FACE_FRONT) s->sf_f = U32(2);
    if (U32(1) & NVN_FACE_BACK) s->sf_b = U32(2);
}
NV(nvnDepthStencilStateSetStencilOp) {
    UNUSED;
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    if (U32(1) & NVN_FACE_FRONT) s->fail_f = U32(2), s->zfail_f = U32(3), s->zpass_f = U32(4);
    if (U32(1) & NVN_FACE_BACK) s->fail_b = U32(2), s->zfail_b = U32(3), s->zpass_b = U32(4);
}
NV(nvnVertexAttribStateSetDefaults) { UNUSED; memset(P(0), 0, sizeof(nvn_va_st)); }
NV(nvnVertexAttribStateSetFormat) {
    UNUSED;
    if (U64(2) >= (1u << 19)) hos_log("nvn: vertex attribute offset %#llx too large", (unsigned long long)U64(2));
    ST(nvn_va_st, P(0))->fmt = U32(1);
    ST(nvn_va_st, P(0))->offset = (uint32_t)U64(2);
}
NV(nvnVertexAttribStateSetStreamIndex) { UNUSED; ST(nvn_va_st, P(0))->stream = U32(1); }
NV(nvnVertexStreamStateSetDefaults) { UNUSED; memset(P(0), 0, sizeof(nvn_vs_st)); }
NV(nvnVertexStreamStateSetStride) { UNUSED; ST(nvn_vs_st, P(0))->stride = (uint32_t)U64(1); }
NV(nvnVertexStreamStateSetDivisor) { UNUSED; ST(nvn_vs_st, P(0))->divisor = U32(1); }

/* ---- submit ---- */
/* deferred binds read the game's handle arrays now (it fills them for the next frame as soon as the submit returns) */
static void resolve_deferred(uint8_t *p) {
    for (;;) {
        nvn_op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= NOP_COUNT_) die("corrupt recording at submit (op %u size %u)", h.op, h.size);
        if (h.op == NOP_END) return;
        if (h.op == NOP_BIND_TEX || h.op == NOP_BIND_IMG || h.op == NOP_BIND_SAMP) {
            nvn_p_bind_handles *b = (nvn_p_bind_handles *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) memcpy(b->h, gw(b->src, 8 * (size_t)b->n), 8 * (size_t)b->n);
        }
        if (h.op == NOP_BIND_BUFS) {
            nvn_p_bind_bufs *b = (nvn_p_bind_bufs *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 16)
                memcpy(b->r, gw(b->src, sizeof(NVNbufferRange) * (size_t)b->n), sizeof(NVNbufferRange) * (size_t)b->n);
        }
        p += h.size;
    }
}
NV(nvnQueueSubmitCommands) {
    UNUSED;
    nvn_queue *q = (nvn_queue *)P(0);
    const int n = I(1);
    uint64_t h[64];
    if (n < 0 || n > 64) die("submit of %d handles", n);
    memcpy(h, PN(2, 8 * (size_t)n), 8 * (size_t)n);
    q->submits++;
    if (ctl_log()) hos_log("nvn ctl: submit %d: %#llx %#llx ...", n, (unsigned long long)h[0], (unsigned long long)(n > 1 ? h[1] : 0));
    for (int i = 0; i < n; i++) {
        check_handle(h[i]);
        resolve_deferred((uint8_t *)(uintptr_t)h[i]);
    }
    rx_nvn_submit_q(h, n, U64(0));
}

/* ---- window ---- */
#define WB(p) ((nvn_win_builder *)(p))
/* a window texture: a guest address, checked at each use (the window object is guest memory) */
static const nvn_tex *win_tex(const nvn_win *o, int i) {
    if (i < 0 || i >= 4 || !o->tex[i]) die("window texture %d", i);
    return (const nvn_tex *)gw(o->tex[i], sizeof(nvn_tex));
}
NV(nvnWindowBuilderSetDefaults) {
    UNUSED;
    nvn_win_builder *b = WB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->interval = 1;
}
NV(nvnWindowBuilderSetDevice) { UNUSED; WB(P(0))->dev = P(1); }
NV(nvnWindowBuilderSetNativeWindow) { UNUSED; WB(P(0))->native = U64(1); }
NV(nvnWindowBuilderSetTextures) {
    UNUSED;
    nvn_win_builder *b = WB(P(0));
    int n = I(1);
    if (n < 0 || n > 4) die("window with %d textures", n);
    b->ntex = n;
    memcpy(b->tex, PN(2, 8 * (size_t)n), 8 * (size_t)n);
}
NV(nvnWindowBuilderSetNumActiveTextures) {
    UNUSED;
    if (I(1) < 2 || I(1) > 4) die("%d active window textures", I(1));
    WB(P(0))->active = I(1);
}
NV(nvnWindowBuilderSetPresentInterval) { UNUSED; WB(P(0))->interval = I(1); }
NV(nvnWindowBuilderGetNativeWindow) { UNUSED; RET(WB(P(0))->native); }
NV(nvnWindowBuilderGetPresentInterval) { UNUSED; RET((uint32_t)WB(P(0))->interval); }
NV(nvnWindowInitialize) {
    UNUSED;
    const nvn_win_builder *b = WB(P(1));
    nvn_win *o = (nvn_win *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('W', 'I', 'N', 'D');
    o->ntex = b->ntex;
    o->interval = b->interval;
    o->active = b->active ? b->active : b->ntex;
    if (o->ntex < 2 || o->ntex > 4 || o->active < 2 || o->active > o->ntex) { RET(0); return; }
    for (int i = 0; i < o->ntex; i++) o->tex[i] = b->tex[i];
    const nvn_tex *t0 = win_tex(o, 0);
    hos_log("nvn: window of %d textures (%d active), %dx%d format %#x, present interval %d", o->ntex, o->active,
            (int)t0->d.width, (int)t0->d.height, t0->d.format, o->interval);
    RET(1);
}
NV(nvnWindowFinalize) { UNUSED; ((nvn_win *)P(0))->magic = 0; }
NV(nvnWindowSetDebugLabel) { UNUSED; (void)c; }
NV(nvnWindowSetNumActiveTextures) {
    UNUSED;
    nvn_win *o = (nvn_win *)P(0);
    if (I(1) < 2 || I(1) > o->ntex) die("%d active window textures", I(1));
    o->active = I(1);
}
NV(nvnWindowGetNumActiveTextures) {
    UNUSED;
    const nvn_win *o = (const nvn_win *)P(0);
    RET((uint32_t)(o->active ? o->active : o->ntex));
}
NV(nvnWindowGetNumTextures) { UNUSED; RET((uint32_t)((const nvn_win *)P(0))->ntex); }
NV(nvnWindowGetPresentInterval) { UNUSED; RET((uint32_t)((const nvn_win *)P(0))->interval); }
NV(nvnWindowSetPresentInterval) {
    UNUSED;
    nvn_win *o = (nvn_win *)P(0);
    if (o->interval != I(1)) hos_log("nvn: present interval %d -> %d", o->interval, I(1));
    o->interval = I(1);
}
NV(nvnWindowSetCrop) {
    UNUSED;
    nvn_win *o = (nvn_win *)P(0);
    const int v[4] = {I(1), I(2), I(3), I(4)};
    if (memcmp(o->crop, v, sizeof v)) hos_log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
    memcpy(o->crop, v, sizeof v);
}
NV(nvnWindowGetCrop) {
    UNUSED;
    const nvn_win *o = (const nvn_win *)P(0);
    NVNrectangle *r = (NVNrectangle *)PN(1, sizeof(NVNrectangle));
    if (!o->crop[2] && !o->crop[3] && o->ntex) {
        const nvn_tex *t0 = win_tex(o, 0);
        r->x = r->y = 0, r->width = (int)t0->d.width, r->height = (int)t0->d.height;
        return;
    }
    r->x = o->crop[0], r->y = o->crop[1], r->width = o->crop[2], r->height = o->crop[3];
}
/* (oranges) Frames are paced here, at 60 Hz over the present interval (the console blocks here when every window
   texture waits for display), not in the present: the game presents with locks held. HOS_NO_PACING=1: as fast as
   it goes. HOS_PRESENT_INTERVAL=<n> (debugging): that interval instead of the game's (the game asks for 2: 30 fps). */
static void pace(int interval) {
    static int off = -1, forced;
    static int64_t next;
    if (off < 0) {
        off = env_on("HOS_NO_PACING");
        const char *e = getenv("HOS_PRESENT_INTERVAL");
        forced = e ? atoi(e) : 0;
    }
    if (forced > 0) interval = forced;
    if (off || interval <= 0) return;
    const int64_t period = 16666667LL * interval, now = hos_now_ns();
    int64_t due = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (due > now + 4 * period) due = now;   /* (a stop in the debugger: no catching up) */
    if (due > now) {
        struct timespec ts = {(time_t)((due - now) / 1000000000LL), (long)((due - now) % 1000000000LL)};
        while (nanosleep(&ts, &ts)) {}
    }
    __atomic_store_n(&next, (due > now ? due : now) + period, __ATOMIC_RELAXED);
}
NV(nvnWindowAcquireTexture) {
    UNUSED;
    nvn_win *o = (nvn_win *)P(0);
    nvn_sync *s = (nvn_sync *)P(1);
    *(int *)PN(2, 4) = o->active ? o->next++ % o->active : 0;
    pace(o->interval);
    /* the acquire sync: this texture is no longer read by the display path (presents copy it in GPU order) */
    if (s) s->fenced = 3, s->fence = 0;
    RET(NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
}
uint64_t hos_present_count;
NV(nvnQueuePresentTexture) {
    UNUSED;
    nvn_win *o = (nvn_win *)P(1);
    const int i = I(2);
    if (i < 0 || i >= o->ntex) die("present of window texture %d", i);
    rx_nvn_present(win_tex(o, i), o->crop);
    o->presents++;
    const uint64_t n = __atomic_add_fetch(&hos_present_count, 1, __ATOMIC_RELAXED);
    static int64_t since;
    static uint64_t at;
    const int64_t now = hos_now_ns();
    if (n == 1) hos_log("nvn: the first frame is presented");
    static int64_t last;
    static int gaps = -1;
    if (gaps < 0) gaps = env_on("HOS_LOG_TIME");
    if (gaps && last && now - last >= 250000000LL)   /* (with HOS_LOG_TIME: a load shows as a gap between presents) */
        hos_log("nvn: no present for %.2f s (frame %llu)", (now - last) / 1e9, (unsigned long long)n);
    last = now;
    if (now - since >= 10000000000LL) {   /* (every 10 s: the frame rate) */
        if (since) hos_log("nvn: %llu frames presented, %.1f fps", (unsigned long long)n, (n - at) * 1e9 / (now - since));
        since = now, at = n;
    }
}

/* ---- the procs ---- */
typedef struct { const char *name; HosHostFn fn; } nvn_impl;
#define E(name) {#name, n_##name}
static const nvn_impl impls[] = {
/* table: every NV() above (tests/test_nvn.py checks that the two agree) */
#include "nvn/nvn_procs.inc"
};
#undef E

/* A proc not implemented here: logged at its first call and answered 0 (initializers: 1, success), so the game goes
   on and the log lists what is left. HOS_NVN_STRICT=1 stops the game instead. */
static void unimplemented(Ctx *A64_NOESC c, void *name) {
    static int strict = -1;
    if (strict < 0) strict = env_on("HOS_NVN_STRICT");
    if (strict)
        die("%s is not implemented (x0 %#llx x1 %#llx x2 %#llx, lr %#llx)", (const char *)name,
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
            (unsigned long long)c->x[30]);
    static const char *seen[512];
    static int nseen;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    const int ok = strstr((const char *)name, "Initialize") != NULL;
    pthread_mutex_lock(&lock);
    int i;
    for (i = 0; i < nseen && seen[i] != name; i++) {}
    if (i == nseen && nseen < 512) {
        seen[nseen++] = (const char *)name;
        hos_log("nvn: %s is not implemented: answered %d (x0 %#llx x1 %#llx x2 %#llx, lr %#llx)", (const char *)name,
                ok, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                (unsigned long long)c->x[30]);
    }
    pthread_mutex_unlock(&lock);
    RET(ok);
}
static int cmp_impl(const void *a, const void *b) {
    return strcmp(((const nvn_impl *)a)->name, ((const nvn_impl *)b)->name);
}
uint64_t nvn_native_proc(const char *name) {
    static nvn_impl *sorted;
    static uint64_t *addr;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    enum { N = sizeof impls / sizeof *impls };
    pthread_mutex_lock(&lock);
    if (!sorted) {
        sorted = xmalloc(sizeof impls, "proc table");
        memcpy(sorted, impls, sizeof impls);
        qsort(sorted, N, sizeof *sorted, cmp_impl);
        addr = xcalloc(N, sizeof *addr, "proc table");
        hos_log("nvn: %d procs implemented", (int)N);
    }
    const nvn_impl key = {name, NULL};
    const nvn_impl *f = (const nvn_impl *)bsearch(&key, sorted, N, sizeof *sorted, cmp_impl);
    uint64_t a;
    if (f) {
        const size_t i = (size_t)(f - sorted);
        if (!addr[i]) addr[i] = hos_hostcall(f->name, f->fn, NULL);
        a = addr[i];
    } else {   /* asked for, maybe never called: it is reported only when called */
        static struct { char *name; uint64_t a; } *un;
        static size_t nun;
        size_t k;
        for (k = 0; k < nun && strcmp(un[k].name, name); k++) {}
        if (k == nun) {
            un = realloc(un, (nun + 1) * sizeof *un);
            if (!un) die("out of memory (proc table)");
            un[nun].name = strdup(name);
            un[nun].a = hos_hostcall(un[nun].name, unimplemented, un[nun].name);
            nun++;
        }
        a = un[k].a;
    }
    pthread_mutex_unlock(&lock);
    return a;
}

/* nvnBootstrapLoader(char const* name) -> the proc (the sdk's export; nvnDeviceGetProcAddress hands out the rest) */
HLE(nvnBootstrapLoader) {
    char name[256];
    RET(nvn_native_proc(guest_name(ARG(0), name, sizeof name)));
}
