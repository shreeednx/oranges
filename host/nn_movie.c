/* movie::Decoder (subsdk0, NintendoSDK's movie library): its video decoders on FFmpeg's libavcodec.

   The library is Android's stagefright ported: movie::Decoder holds a DecoderImpl (+8) that drives an
   android::MediaCodec, whose video codecs are NVIDIA's hardware OMX components (NvMM: they open an nvhost channel,
   and NvOsDrvOpen aborts here, where the nv driver is never set up). Its audio codecs are software, and run lifted.
   So every movie::Decoder method is HLE'd (they are all virtual, and hooked at their definitions): a video decoder
   (DecoderType 1 AVC, 2 VP8, 3 VP9) is ours, an audio one (4 AAC, 5 Vorbis) is passed through to the lifted
   DecoderImpl, built as the library's own constructor builds it.

   Ours keeps MediaCodec's shape, as DecoderImpl shows it to the client (status codes, lists, events):
     - input buffers by index: AcquireInputBufferIndex takes the first free one (0x1011 when none), GetInputBuffer
       points the client's movie::Buffer at its guest memory, SendInputBufferForDecode queues it (flags as
       MediaCodec's: 4 end of stream). Its index is free again once its packet has gone to the codec;
     - output buffers by index: AcquireOutputBufferIndex takes the first finished picture (0x100e when none) with
       its time (us) and flags, GetOutputBuffer writes it into the client's buffer (output format 4: NV12, packed,
       Y stride = width), ReleaseOutputBufferIndex gives the index back;
     - the client's DecoderEvents (nn::os::EventType*: input available, output available, format changed, error)
       are signalled as MediaCodec's callbacks signal them.
   Decoding happens on the client's threads (the guest events are signalled from guest code): a pump, run by each
   call that can make progress, feeds queued packets to the codec while a free output index can take what comes out.
   Without FFmpeg (HOS_HAVE_FFMPEG unset) Configure fails with 0x100c, and the client goes without its video.

   HOS_MOVIE_LOG=1: every call of a video decoder's. */
#include "hos.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef HOS_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#endif

/* statuses (movie::Status, as DecoderImpl returns them) */
#define ST_OK 0
#define ST_FAILED 0x1000
#define ST_UNSUPPORTED 0x1006
#define ST_INVALID 0x100b
#define ST_NOT_CONFIGURED 0x100c
#define ST_NO_OUTPUT 0x100e
#define ST_NO_INPUT 0x1011
#define ST_TOO_SMALL ((uint32_t)-0x3f1)
#define FLAG_EOS 4u

#define VDEC_MAGIC 0x56444543ull   /* 'VDEC': a video decoder's impl slot is (magic << 32 | index), not a pointer */
#define MAX_VDECS 8
#define N_IN 6
#define IN_SIZE (2u << 20)        /* (an input buffer: a compressed frame) */
#define N_OUT 6
#define IMPL_SIZE 0x168           /* sizeof(DecoderImpl): the library's constructor allocates this (supported build) */

enum { IN_FREE, IN_CLIENT, IN_QUEUED };
enum { OUT_FREE, OUT_READY, OUT_CLIENT };

typedef struct {
    int in_use, type, format, mode;
    uint64_t events;                     /* (the client's DecoderEvents) */
    int width, height;                   /* (Configure's, then the pictures') */
    int started, eos_in, eos_out, format_sent;
    uint32_t last_error;
    int in_state[N_IN];
    uint64_t in_mem[N_IN];               /* (guest memory, kept across decoders) */
    int free_in[N_IN], nfree_in;         /* (free input indices, in order) */
    struct { int index, offset, size; int64_t pts; uint32_t flags; } pending[N_IN];
    int npending;
    int out_state[N_OUT];
    struct { uint8_t *nv12; size_t cap; int w, h; int64_t pts; uint32_t flags; } out[N_OUT];
    int ready[N_OUT], nready;
#ifdef HOS_HAVE_FFMPEG
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
#endif
} Vdec;
static Vdec vdecs[MAX_VDECS];
static pthread_mutex_t vlocks[MAX_VDECS] = {[0 ... MAX_VDECS - 1] = PTHREAD_MUTEX_INITIALIZER};   /* (each Vdec's) */
#define LOCK(d) pthread_mutex_lock(&vlocks[(d) - vdecs])
#define UNLOCK(d) pthread_mutex_unlock(&vlocks[(d) - vdecs])
static pthread_mutex_t vdecs_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t in_pool[MAX_VDECS][N_IN];   /* (input memory per decoder slot, made once) */

static int movie_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_MOVIE_LOG") && atoi(getenv("HOS_MOVIE_LOG")) > 0;
    return on;
}

/* ---- the guest functions used ---- */
static uint64_t sym(const char *name) {
    uint64_t a = hos_symbol_lookup(name);
    char alias[256];
    const char *s = strstr(name, "C1E") ? strstr(name, "C1E") : strstr(name, "D1E");
    if (!a && s && strlen(name) < sizeof alias) {   /* (C1/C2 and D1/D2 share an address: the table has one name) */
        strcpy(alias, name);
        alias[s - name + 1] = '2';
        a = hos_symbol_lookup(alias);
    }
    if (!a) hos_fatal(NULL, "movie: %s is not in the image", name);
    return a;
}
#define GUEST_FN(var, name)                                                                                            \
    static uint64_t var;                                                                                               \
    if (!var) var = sym(name)
static uint64_t call(Ctx *c, uint64_t fn, int n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                     uint64_t a5) {
    const uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
    return hos_guest_call(c, fn, n, args);
}
/* a guest copy of a constant string (MediaData keys), made once */
static uint64_t gstr(const char *s) {
    static struct { const char *s; uint64_t g; } tab[32];
    static int n;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n; i++)
        if (tab[i].s == s) {
            pthread_mutex_unlock(&lock);
            return tab[i].g;
        }
    const uint64_t g = hos_sys_alloc(strlen(s) + 1, 8);
    memcpy(GP(g), s, strlen(s) + 1);
    if (n < 32) tab[n].s = s, tab[n].g = g, n++;
    pthread_mutex_unlock(&lock);
    return g;
}
static void signal_event(Ctx *c, Vdec *d, int which) {   /* 0 input, 1 output, 2 format changed, 3 error */
    if (!d->events) return;
    const uint64_t ev = G64(d->events + 8 * which);
    if (!ev) return;
    GUEST_FN(signal, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    call(c, signal, 1, ev, 0, 0, 0, 0, 0);
}
static void set_int(Ctx *c, uint64_t md, const char *key, int v) {
    GUEST_FN(f, "_ZN5movie9MediaData8SetInt32EPKci");
    call(c, f, 3, md, gstr(key), (uint32_t)v, 0, 0, 0);
}
static int find_int(Ctx *c, uint64_t md, const char *key, int *v) {
    GUEST_FN(f, "_ZNK5movie9MediaData9FindInt32EPKcPi");
    const uint64_t out = c->x[31] - 0x200;   /* (below the guest stack pointer: the callee's frame goes below that) */
    G32(out) = 0;
    c->x[31] -= 0x210;
    const int ok = (int)(call(c, f, 3, md, gstr(key), out, 0, 0, 0) & 1);
    c->x[31] += 0x210;
    if (ok) *v = (int32_t)G32(out);
    return ok;
}
static void buffer_set(Ctx *c, uint64_t buf, uint64_t data, uint64_t capacity, int offset, int size) {
    GUEST_FN(set_data, "_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    GUEST_FN(set_range, "_ZN5movie6Buffer8SetRangeEii");
    if (data) call(c, set_data, 3, buf, data, capacity, 0, 0, 0);
    call(c, set_range, 3, buf, (uint32_t)offset, (uint32_t)size, 0, 0, 0);
}

/* ---- our decoders ---- */
static Vdec *vdec_of(uint64_t self) {
    const uint64_t h = G64(self + 8);
    if (h >> 32 != VDEC_MAGIC || (uint32_t)h >= MAX_VDECS) return NULL;
    Vdec *d = &vdecs[(uint32_t)h];
    return d->in_use ? d : NULL;
}
static int is_video(int type) { return type >= 1 && type <= 3; }

static void reset_buffers(Vdec *d) {   /* (every index back to the decoder, nothing queued or finished) */
    d->nfree_in = 0;
    for (int i = 0; i < N_IN; i++) d->in_state[i] = IN_FREE, d->free_in[d->nfree_in++] = i;
    d->npending = 0;
    for (int i = 0; i < N_OUT; i++) d->out_state[i] = OUT_FREE;
    d->nready = 0;
    d->eos_in = d->eos_out = 0;
}
static void close_codec(Vdec *d) {
#ifdef HOS_HAVE_FFMPEG
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
#else
    (void)d;
#endif
}

#ifdef HOS_HAVE_FFMPEG
static int free_out(Vdec *d) {
    for (int i = 0; i < N_OUT; i++)
        if (d->out_state[i] == OUT_FREE) return i;
    return -1;
}
/* a picture into output index o, as NV12 */
static int keep_picture(Vdec *d, int o, const AVFrame *f) {
    const int w = f->width, h = f->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const size_t size = (size_t)w * h + (size_t)2 * cw * ch;
    if (d->out[o].cap < size) {
        free(d->out[o].nv12);
        d->out[o].nv12 = malloc(size);
        d->out[o].cap = d->out[o].nv12 ? size : 0;
        if (!d->out[o].nv12) return 0;
    }
    uint8_t *y = d->out[o].nv12, *uv = y + (size_t)w * h;
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        for (int r = 0; r < h; r++) memcpy(y + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
        for (int r = 0; r < ch; r++) {
            const uint8_t *u = f->data[1] + (size_t)r * f->linesize[1], *v = f->data[2] + (size_t)r * f->linesize[2];
            uint8_t *o2 = uv + (size_t)r * 2 * cw;
            for (int x = 0; x < cw; x++) o2[2 * x] = u[x], o2[2 * x + 1] = v[x];
        }
    } else if (f->format == AV_PIX_FMT_NV12) {
        for (int r = 0; r < h; r++) memcpy(y + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
        for (int r = 0; r < ch; r++) memcpy(uv + (size_t)r * 2 * cw, f->data[1] + (size_t)r * f->linesize[1], 2 * cw);
    } else {   /* (another layout: grey, and said once) */
        static int said;
        if (!said++) hos_log("movie: pictures in pixel format %d are not converted (grey)", f->format);
        memset(y, 16, (size_t)w * h);
        memset(uv, 128, (size_t)2 * cw * ch);
    }
    d->out[o].w = w, d->out[o].h = h;
    d->out[o].pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    d->out[o].flags = 0;
    return 1;
}
#endif

/* Feeds queued packets to the codec while a free output index can take a picture; under its lock. What to signal
   comes back in sig (bit 0 input, 1 output, 2 format changed). */
static void pump(Vdec *d, int *sig) {
#ifdef HOS_HAVE_FFMPEG
    if (!d->ctx || !d->started) return;
    for (;;) {
        const int o = free_out(d);
        if (o < 0) return;
        int r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == 0) {
            if (keep_picture(d, o, d->frame)) {
                if (!d->format_sent || d->out[o].w != d->width || d->out[o].h != d->height) {
                    d->width = d->out[o].w, d->height = d->out[o].h, d->format_sent = 1;
                    *sig |= 4;
                }
                d->out_state[o] = OUT_READY, d->ready[d->nready++] = o;
                *sig |= 2;
            }
            av_frame_unref(d->frame);
            continue;
        }
        if (r == AVERROR_EOF) {   /* (drained: an empty buffer with the end-of-stream flag) */
            if (!d->eos_out) {
                d->eos_out = 1;
                d->out[o].w = d->out[o].h = 0, d->out[o].pts = 0, d->out[o].flags = FLAG_EOS;
                d->out_state[o] = OUT_READY, d->ready[d->nready++] = o;
                *sig |= 2;
            }
            return;
        }
        if (r != AVERROR(EAGAIN) || !d->npending) return;
        /* the codec wants a packet: the oldest queued one, whose index is free again */
        const int in = d->pending[0].index, off = d->pending[0].offset, size = d->pending[0].size;
        const int64_t pts = d->pending[0].pts;
        const uint32_t flags = d->pending[0].flags;
        memmove(d->pending, d->pending + 1, --d->npending * sizeof d->pending[0]);
        if (size > 0 && av_new_packet(d->pkt, size) == 0) {
            memcpy(d->pkt->data, GP(d->in_mem[in] + off), size);
            d->pkt->pts = d->pkt->dts = pts;
            r = avcodec_send_packet(d->ctx, d->pkt);
            av_packet_unref(d->pkt);
            if (r < 0) {
                static int said;
                if (said++ < 4) hos_log("movie: a packet does not decode (%d, %d bytes at %lld us)", r, size, (long long)pts);
            }
        }
        if (flags & FLAG_EOS) avcodec_send_packet(d->ctx, NULL), d->eos_in = 1;
        d->in_state[in] = IN_FREE, d->free_in[d->nfree_in++] = in;
        *sig |= 1;
    }
#else
    (void)d, (void)sig;
#endif
}
static void signals(Ctx *c, Vdec *d, int sig) {
    if (sig & 1) signal_event(c, d, 0);
    if (sig & 4) signal_event(c, d, 2);
    if (sig & 2) signal_event(c, d, 1);
}

/* ---- the audio decoders: the library's own ---- */
#define IMPL_CALL(c, self, mangled, n, a1, a2, a3, a4, a5, none)                                                      \
    do {                                                                                                               \
        GUEST_FN(fn_, mangled);                                                                                        \
        const uint64_t impl_ = G64((self) + 8);                                                                        \
        RET(impl_ ? call(c, fn_, n, impl_, a1, a2, a3, a4, a5) : (none));                                              \
    } while (0)

/* ---- movie::Decoder ---- */
static const char *type_name(int t) {
    static const char *n[] = {"?", "AVC", "VP8", "VP9", "AAC", "Vorbis"};
    return t >= 0 && t <= 5 ? n[t] : "?";
}

/* Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int) */
HLE(_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi) {
    const uint64_t self = ARG(0);
    const int type = (int)ARG(1), format = (int)ARG(2), mode = (int)ARG(3), arg = (int)ARG(4);
    GUEST_FN(vtable, "_ZTVN5movie7DecoderE");
    G64(self) = vtable + 0x10, G64(self + 8) = 0;
    if (!is_video(type)) {   /* the library's: a DecoderImpl */
        GUEST_FN(op_new, "_Znwm");
        GUEST_FN(ctor, "_ZN5movie11DecoderImplC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi");
        const uint64_t impl = call(c, op_new, 1, IMPL_SIZE, 0, 0, 0, 0, 0);
        call(c, ctor, 5, impl, (uint32_t)type, (uint32_t)format, (uint32_t)mode, (uint32_t)arg, 0);
        G64(self + 8) = impl;
        hos_log("movie: %s decoder (output format %d, mode %d): the library's", type_name(type), format, mode);
        return;
    }
    pthread_mutex_lock(&vdecs_lock);
    int i = 0;
    while (i < MAX_VDECS && vdecs[i].in_use) i++;
    if (i == MAX_VDECS) {
        pthread_mutex_unlock(&vdecs_lock);
        hos_log("movie: more than %d video decoders", MAX_VDECS);
        return;
    }
    Vdec *d = &vdecs[i];
    memset(d, 0, sizeof *d);
    d->in_use = 1, d->type = type, d->format = format, d->mode = mode;
    for (int k = 0; k < N_IN; k++) {
        if (!in_pool[i][k]) in_pool[i][k] = hos_sys_alloc(IN_SIZE, 0x100);
        d->in_mem[k] = in_pool[i][k];
    }
    reset_buffers(d);
    G64(self + 8) = VDEC_MAGIC << 32 | (uint32_t)i;
    pthread_mutex_unlock(&vdecs_lock);
    hos_log("movie: %s decoder %d (output format %d, mode %d): FFmpeg", type_name(type), i, format, mode);
    if (format != 4) hos_log("movie: video output format %d is not written (only 4, NV12)", format);
}
/* ~Decoder() (the deleting destructor calls this, then frees the object) */
HLE(_ZN5movie7DecoderD1Ev) {
    const uint64_t self = ARG(0);
    Vdec *d = vdec_of(self);
    GUEST_FN(vtable, "_ZTVN5movie7DecoderE");
    G64(self) = vtable + 0x10;
    if (d) {
        LOCK(d);
        close_codec(d);
        for (int i = 0; i < N_OUT; i++) free(d->out[i].nv12), d->out[i].nv12 = NULL, d->out[i].cap = 0;
        d->in_use = 0;
        UNLOCK(d);
        G64(self + 8) = 0;
        if (movie_log()) hos_log("movie: video decoder %d destroyed", (int)(d - vdecs));
        return;
    }
    const uint64_t impl = G64(self + 8);
    if (!impl) return;
    GUEST_FN(dtor, "_ZN5movie11DecoderImplD1Ev");
    GUEST_FN(op_delete, "_ZdlPv");
    call(c, dtor, 1, impl, 0, 0, 0, 0, 0);
    call(c, op_delete, 1, impl, 0, 0, 0, 0, 0);
}

/* Configure(MediaData*) */
HLE(_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl9ConfigureEPNS_9MediaDataE", 2, ARG(1), 0, 0, 0, 0, ST_NOT_CONFIGURED);
        return;
    }
    if (!ARG(1)) {
        RET(ST_INVALID);
        return;
    }
    int w = 0, h = 0;
    find_int(c, ARG(1), "width", &w);
    find_int(c, ARG(1), "height", &h);
#ifdef HOS_HAVE_FFMPEG
    LOCK(d);
    close_codec(d);
    const enum AVCodecID id = d->type == 1 ? AV_CODEC_ID_H264 : d->type == 2 ? AV_CODEC_ID_VP8 : AV_CODEC_ID_VP9;
    const AVCodec *codec = avcodec_find_decoder(id);
    d->ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    int ok = d->ctx != NULL;
    if (ok) {
        d->ctx->width = w, d->ctx->height = h;
        d->ctx->pkt_timebase = (AVRational){1, 1000000};
        d->ctx->thread_count = 4;
        ok = avcodec_open2(d->ctx, codec, NULL) == 0;
    }
    if (ok) d->pkt = av_packet_alloc(), d->frame = av_frame_alloc();
    if (!ok) close_codec(d);
    d->width = w, d->height = h, d->format_sent = 0;
    UNLOCK(d);
    hos_log("movie: video decoder %d: %s %dx%d %s", (int)(d - vdecs), type_name(d->type), w, h,
            ok ? "configured" : "has no FFmpeg decoder");
    RET(ok ? ST_OK : ST_NOT_CONFIGURED);
#else
    hos_log("movie: video decoder %d: %s %dx%d not configured (built without FFmpeg)", (int)(d - vdecs),
            type_name(d->type), w, h);
    RET(ST_NOT_CONFIGURED);
#endif
}
/* RegisterClientEvents(DecoderEvents*) */
HLE(_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl20RegisterClientEventsEPNS_13DecoderEventsE", 2, ARG(1), 0, 0, 0,
                  0, ST_FAILED);
        return;
    }
    d->events = ARG(1);
    RET(ST_OK);
}
/* Start(): every input index is the client's to take */
HLE(_ZN5movie7Decoder5StartEv) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl5StartEv", 1, 0, 0, 0, 0, 0, ST_FAILED);
        return;
    }
    LOCK(d);
#ifdef HOS_HAVE_FFMPEG
    const int ok = d->ctx != NULL;
#else
    const int ok = 0;
#endif
    if (ok) reset_buffers(d), d->started = 1;
    UNLOCK(d);
    if (movie_log()) hos_log("movie: video decoder %d: Start (%s)", (int)(d - vdecs), ok ? "started" : "not configured");
    if (ok) signal_event(c, d, 0);
    RET(ok ? ST_OK : ST_NOT_CONFIGURED);
}
static void stop_or_flush(Ctx *c, Vdec *d, int stop, const char *what) {
    LOCK(d);
#ifdef HOS_HAVE_FFMPEG
    if (d->ctx) avcodec_flush_buffers(d->ctx);
#endif
    reset_buffers(d);
    if (stop) d->started = 0;
    UNLOCK(d);
    if (movie_log()) hos_log("movie: video decoder %d: %s", (int)(d - vdecs), what);
    if (!stop) signal_event(c, d, 0);
}
HLE(_ZN5movie7Decoder4StopEv) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl4StopEv", 1, 0, 0, 0, 0, 0, ST_FAILED);
        return;
    }
    stop_or_flush(c, d, 1, "Stop");
    RET(ST_OK);
}
HLE(_ZN5movie7Decoder5ResetEv) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl5ResetEv", 1, 0, 0, 0, 0, 0, ST_FAILED);
        return;
    }
    stop_or_flush(c, d, 1, "Reset");
    LOCK(d);
    close_codec(d);
    UNLOCK(d);
    RET(ST_OK);
}
HLE(_ZN5movie7Decoder5FlushEv) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl5FlushEv", 1, 0, 0, 0, 0, 0, ST_FAILED);
        return;
    }
    stop_or_flush(c, d, 0, "Flush");
    RET(ST_OK);
}

/* AcquireInputBufferIndex(int* index, int* remaining) */
HLE(_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl23AcquireInputBufferIndexEPiS1_", 3, ARG(1), ARG(2), 0, 0, 0,
                  ST_FAILED);
        return;
    }
    if (!ARG(1) || !ARG(2)) {
        RET(ST_INVALID);
        return;
    }
    LOCK(d);
    int index = -1, remaining = 0;
    uint32_t st = ST_NO_INPUT;
    if (d->started && d->nfree_in > 0) {
        index = d->free_in[0];
        memmove(d->free_in, d->free_in + 1, --d->nfree_in * sizeof d->free_in[0]);
        d->in_state[index] = IN_CLIENT;
        remaining = d->nfree_in, st = ST_OK;
    }
    UNLOCK(d);
    G32(ARG(1)) = (uint32_t)index, G32(ARG(2)) = (uint32_t)remaining;
    RET(st);
}
/* GetInputBuffer(int index, Buffer*) */
HLE(_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl14GetInputBufferEiPNS_6BufferE", 3, ARG(1), ARG(2), 0, 0, 0,
                  ST_FAILED);
        return;
    }
    const int index = (int)ARG(1);
    if (!ARG(2) || index < 0 || index >= N_IN || d->in_state[index] != IN_CLIENT) {
        if (ARG(2)) buffer_set(c, ARG(2), 0, 0, 0, 0);
        RET(ST_INVALID);
        return;
    }
    buffer_set(c, ARG(2), d->in_mem[index], IN_SIZE, 0, IN_SIZE);
    RET(ST_OK);
}
/* SendInputBufferForDecode(int index, int offset, int size, long time_us, unsigned flags) */
HLE(_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl24SendInputBufferForDecodeEiiilj", 6, ARG(1), ARG(2), ARG(3),
                  ARG(4), ARG(5), ST_FAILED);
        return;
    }
    const int index = (int)ARG(1), offset = (int)ARG(2), size = (int)ARG(3);
    const int64_t pts = (int64_t)ARG(4);
    const uint32_t flags = (uint32_t)ARG(5);
    if (movie_log()) hos_log("movie: video decoder %d: input %d, %d bytes at %lld us, flags %#x", (int)(d - vdecs), index,
                             size, (long long)pts, flags);
    LOCK(d);
    if (index < 0 || index >= N_IN || d->in_state[index] != IN_CLIENT || offset < 0 || size < 0 ||
        (uint64_t)offset + size > IN_SIZE) {
        UNLOCK(d);
        RET(ST_INVALID);
        return;
    }
    d->in_state[index] = IN_QUEUED;
    d->pending[d->npending].index = index, d->pending[d->npending].offset = offset;
    d->pending[d->npending].size = size, d->pending[d->npending].pts = pts, d->pending[d->npending].flags = flags;
    d->npending++;
    int sig = 0;
    pump(d, &sig);
    UNLOCK(d);
    signals(c, d, sig);
    RET(ST_OK);
}

/* AcquireOutputBufferIndex(int* index, long* time_us, unsigned* flags, int* remaining) */
HLE(_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl24AcquireOutputBufferIndexEPiPlPjS1_", 5, ARG(1), ARG(2), ARG(3),
                  ARG(4), 0, ST_FAILED);
        return;
    }
    if (!ARG(1) || !ARG(2) || !ARG(3) || !ARG(4)) {
        RET(ST_INVALID);
        return;
    }
    int sig = 0;
    LOCK(d);
    pump(d, &sig);
    int index = -1, remaining = 0;
    int64_t pts = 0;
    uint32_t flags = 0, st = ST_NO_OUTPUT;
    if (d->nready > 0) {
        index = d->ready[0];
        memmove(d->ready, d->ready + 1, --d->nready * sizeof d->ready[0]);
        d->out_state[index] = OUT_CLIENT;
        pts = d->out[index].pts, flags = d->out[index].flags, remaining = d->nready, st = ST_OK;
    }
    UNLOCK(d);
    signals(c, d, sig);
    G32(ARG(1)) = (uint32_t)index, G64(ARG(2)) = (uint64_t)pts, G32(ARG(3)) = flags, G32(ARG(4)) = (uint32_t)remaining;
    if (movie_log() && st == ST_OK)
        hos_log("movie: video decoder %d: output %d at %lld us, flags %#x", (int)(d - vdecs), index, (long long)pts, flags);
    RET(st);
}
/* the picture at output index `index` into the client's buffer (NV12) */
static uint32_t output_to(Ctx *c, Vdec *d, int index, uint64_t buf) {
    GUEST_FN(base_fn, "_ZNK5movie6Buffer4BaseEv");
    GUEST_FN(cap_fn, "_ZNK5movie6Buffer8CapacityEv");
    LOCK(d);
    if (index < 0 || index >= N_OUT || d->out_state[index] != OUT_CLIENT) {
        UNLOCK(d);
        buffer_set(c, buf, 0, 0, 0, 0);
        return ST_INVALID;
    }
    const int w = d->out[index].w, h = d->out[index].h;
    const size_t size = w && h ? (size_t)w * h + (size_t)2 * ((w + 1) / 2) * ((h + 1) / 2) : 0;
    UNLOCK(d);
    if (!size) {
        buffer_set(c, buf, 0, 0, 0, 0);
        return ST_OK;
    }
    const uint64_t base = call(c, base_fn, 1, buf, 0, 0, 0, 0, 0), cap = call(c, cap_fn, 1, buf, 0, 0, 0, 0, 0);
    if (!base || size > cap) {
        buffer_set(c, buf, 0, 0, 0, 0);
        static int said;
        if (said++ < 4) hos_log("movie: a picture (%zu bytes) does not fit the client's buffer (%llu)", size,
                                (unsigned long long)cap);
        return ST_TOO_SMALL;
    }
    if (d->format == 4) memcpy(GP(base), d->out[index].nv12, size);   /* (the index is the client's: not reused) */
    buffer_set(c, buf, 0, 0, 0, (int)size);
    return ST_OK;
}
/* the picture's layout (as DecoderImpl::GetOutputBufferFormat sets it for NV12) */
static void format_to(Ctx *c, Vdec *d, int w, int h, uint64_t md) {
    (void)d;
    set_int(c, md, "width", w);
    set_int(c, md, "height", h);
    set_int(c, md, "crop-width", w);
    set_int(c, md, "crop-height", h);
    set_int(c, md, "nv12-y-stride", w);
    set_int(c, md, "nv12-uv-stride", w);
    set_int(c, md, "nv12-y-offset", 0);
    set_int(c, md, "nv12-uv-offset", w * h);
}
/* GetOutputBuffer(int index, Buffer*) */
HLE(_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl15GetOutputBufferEiPNS_6BufferE", 3, ARG(1), ARG(2), 0, 0, 0,
                  ST_FAILED);
        return;
    }
    RET(ARG(2) ? output_to(c, d, (int)ARG(1), ARG(2)) : ST_INVALID);
}
/* GetOutputBuffer(int index, Buffer*, MediaData*) */
HLE(_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", 4, ARG(1), ARG(2),
                  ARG(3), 0, 0, ST_FAILED);
        return;
    }
    if (!ARG(2)) {
        RET(ST_INVALID);
        return;
    }
    const int index = (int)ARG(1);
    const uint32_t st = output_to(c, d, index, ARG(2));
    if (st == ST_OK && ARG(3)) format_to(c, d, d->out[index].w, d->out[index].h, ARG(3));
    RET(st);
}
/* GetOutputBufferFormat(int index, MediaData*) */
HLE(_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl21GetOutputBufferFormatEiPNS_9MediaDataE", 3, ARG(1), ARG(2), 0, 0,
                  0, ST_FAILED);
        return;
    }
    const int index = (int)ARG(1);
    if (!ARG(2) || index < 0 || index >= N_OUT || d->out_state[index] != OUT_CLIENT) {
        RET(ARG(2) ? ST_FAILED : ST_INVALID);
        return;
    }
    format_to(c, d, d->out[index].w, d->out[index].h, ARG(2));
    RET(ST_OK);
}
/* GetOutputFormat(MediaData*): the stream's (MediaCodec's output format, with its crop rect) */
HLE(_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl15GetOutputFormatEPNS_9MediaDataE", 2, ARG(1), 0, 0, 0, 0,
                  ST_FAILED);
        return;
    }
    if (!ARG(1)) {
        RET(ST_INVALID);
        return;
    }
    GUEST_FN(set_string, "_ZN5movie9MediaData9SetStringEPKcS2_");
    GUEST_FN(set_rect, "_ZN5movie9MediaData7SetRectEPKciiii");
    const int w = d->width, h = d->height;
    call(c, set_string, 3, ARG(1), gstr("mime"), gstr("video/raw"), 0, 0, 0);
    set_int(c, ARG(1), "stride", w);
    set_int(c, ARG(1), "slice-height", h);
    call(c, set_rect, 6, ARG(1), gstr("crop"), 0, 0, (uint32_t)(w - 1), (uint32_t)(h - 1));
    format_to(c, d, w, h, ARG(1));
    if (movie_log()) hos_log("movie: video decoder %d: output format %dx%d", (int)(d - vdecs), w, h);
    RET(ST_OK);
}
/* GetOutputBufferSize(int*, int*): audio decoders' (a video decoder's answer is 0x1006, as the library's) */
HLE(_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl19GetOutputBufferSizeEPiS1_", 3, ARG(1), ARG(2), 0, 0, 0, ST_FAILED);
        return;
    }
    RET(ARG(1) && ARG(2) ? ST_UNSUPPORTED : ST_INVALID);
}
/* ReleaseOutputBufferIndex(int index) */
HLE(_ZN5movie7Decoder24ReleaseOutputBufferIndexEi) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl24ReleaseOutputBufferIndexEi", 2, ARG(1), 0, 0, 0, 0, ST_FAILED);
        return;
    }
    const int index = (int)ARG(1);
    int sig = 0;
    uint32_t st = ST_INVALID;
    LOCK(d);
    if (index >= 0 && index < N_OUT && d->out_state[index] == OUT_CLIENT) {
        d->out_state[index] = OUT_FREE, st = ST_OK;
        pump(d, &sig);
    }
    UNLOCK(d);
    signals(c, d, sig);
    RET(st);
}
HLE(_ZN5movie7Decoder12GetLastErrorEv) {
    Vdec *d = vdec_of(ARG(0));
    if (!d) {
        IMPL_CALL(c, ARG(0), "_ZN5movie11DecoderImpl12GetLastErrorEv", 1, 0, 0, 0, 0, 0, ST_FAILED);
        return;
    }
    RET(d->last_error);
}
