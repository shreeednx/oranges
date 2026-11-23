/* The null render back end: what host/nvn_native.c hands the renderer (host/nvn/rx_nvn.h), with nothing drawn.

   The Vulkan back end (host/vk/) replaces it where it is built; its rx_null_* parts serve that one when it does not
   start (HOS_VK=0, no device). It does what the CPU can see of the GPU's work, so the game's own logic goes on as on
   the console: at a submit, events are signalled, counters are reported (timestamps
   from the system tick), buffers are copied, cleared and updated, in recording order. Draws, clears of textures and
   texture copies are skipped; ReadTexels reads zeros. Every fence is reached at once. */
#include "hos.h"
#include "nvn/rx_nvn.h"

#include <string.h>


static void counter(const nvn_p_counter *r) {
    NVNcounterData d = {0, 0};
    const uint32_t type = (uint32_t)r->type;
    if (type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) d.timestamp = a64_cntpct_host();
    if (r->type & NVN_COUNTER_WRITTEN) return;
    nvn_gpu_write(r->addr, &d, sizeof d);
}

static void copy_b2b(uint64_t src, uint64_t dst, uint64_t n) {
    uint8_t buf[4096];
    while (n) {   /* (through a buffer: the ranges may be virtual, and may overlap) */
        const uint64_t k = n < sizeof buf ? n : sizeof buf;
        if (!nvn_gpu_read(src, buf, k) || !nvn_gpu_write(dst, buf, k)) return;
        src += k, dst += k, n -= k;
    }
}

static void clear_buf(uint64_t dst, uint64_t n, uint32_t value) {
    uint32_t buf[1024];
    for (size_t i = 0; i < sizeof buf / 4; i++) buf[i] = value;
    while (n) {
        const uint64_t k = n < sizeof buf ? n : sizeof buf;
        if (!nvn_gpu_write(dst, buf, k)) return;
        dst += k, n -= k;
    }
}

static void run(const uint8_t *p) {
    for (;;) {
        nvn_op_hdr h;
        memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
        case NOP_END:
            return;
        case NOP_SIGNAL_EVENT: {
            const nvn_p_signal *s = (const nvn_p_signal *)a;
            uint32_t v = 0;
            if (s->mode == NVN_EVENT_SIGNAL_MODE_ADD) nvn_gpu_read(s->gpu, &v, 4);
            v = s->mode == NVN_EVENT_SIGNAL_MODE_ADD ? v + s->value : s->value;
            nvn_gpu_write(s->gpu, &v, 4);
            break;
        }
        case NOP_REPORT_COUNTER:
            counter((const nvn_p_counter *)a);
            break;
        case NOP_COPY_B2B: {
            const nvn_p_copy_bb *r = (const nvn_p_copy_bb *)a;
            copy_b2b(r->src, r->dst, r->size);
            break;
        }
        case NOP_UPDATE_UB: {
            const nvn_p_update_ub *u = (const nvn_p_update_ub *)a;
            nvn_gpu_write(u->dst, a + sizeof *u, u->size);
            break;
        }
        case NOP_CLEAR_BUF: {
            const nvn_p_clear_buf *r = (const nvn_p_clear_buf *)a;
            clear_buf(r->dst, r->size, r->value);
            break;
        }
        default:
            break;
        }
        p += h.size;
    }
}

void rx_null_submit(const uint64_t *handles, int n) {
    for (int i = 0; i < n; i++) run((const uint8_t *)(uintptr_t)handles[i]);
}
void rx_null_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                         size_t row, size_t img) {
    (void)t, (void)level, (void)layer0;
    int bw, bh;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!dst || bpb <= 0 || r->width <= 0 || r->height <= 0) return;
    const size_t wb = (size_t)(r->width + bw - 1) / bw * (size_t)bpb, hb = (size_t)(r->height + bh - 1) / bh;
    for (int z = 0; z < (r->depth > 1 ? r->depth : 1); z++)
        for (size_t y = 0; y < hb; y++) memset(dst + z * img + y * row, 0, wb);
}

#ifndef HOS_HAVE_VULKAN
static uint64_t pushed;   /* submits so far (every one carried out at once: replayed == pushed) */
void rx_nvn_submit(const uint64_t *handles, int n) {
    rx_null_submit(handles, n);
    __atomic_add_fetch(&pushed, 1, __ATOMIC_RELEASE);
}
void rx_nvn_submit_q(const uint64_t *handles, int n, uint64_t queue) {
    (void)queue;
    rx_nvn_submit(handles, n);
}
uint64_t rx_nvn_fence(void) { return __atomic_load_n(&pushed, __ATOMIC_ACQUIRE); }
uint64_t rx_nvn_fence_q(uint64_t queue) {
    (void)queue;
    return rx_nvn_fence();
}
uint64_t rx_nvn_pushed(void) { return rx_nvn_fence(); }
uint64_t rx_nvn_replayed(void) { return rx_nvn_fence(); }
int rx_nvn_wait(uint64_t fence, uint64_t timeout_ns) {
    (void)fence, (void)timeout_ns;
    return 1;
}
void rx_wait_writebacks_reading(const void *p, uint64_t n) { (void)p, (void)n; }
void rx_nvn_finish(void) {}
void rx_nvn_present(const nvn_tex *t, const int crop[4]) { (void)t, (void)crop; }
void rx_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) { (void)id, (void)t, (void)v; }
void rx_nvn_register_sampler(int id, const nvn_samp_par *p) { (void)id, (void)p; }
void rx_nvn_set_texture_pool_size(int n) { (void)n; }
void rx_nvn_texture_released(const nvn_tex *t) { (void)t; }
void rx_nvn_texture_created(const nvn_tex *t) { (void)t; }
void rx_nvn_program_released(const nvn_prog *p) { (void)p; }
void rx_nvn_program_created(const nvn_prog *p) { (void)p; }
void rx_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                        size_t row, size_t img) {
    rx_null_read_texels(t, level, layer0, r, dst, row, img);
}
void rx_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                         size_t row, size_t img) {
    (void)t, (void)level, (void)layer0, (void)r, (void)src, (void)row, (void)img;
}
void rx_nvn_invalidate(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; }
void rx_nvn_pool_created(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; }
void rx_nvn_vmap_changing(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; }
void rx_nvn_event_signal(uint64_t gpu, uint32_t value) { (void)gpu, (void)value; }
#endif
