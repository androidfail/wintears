/* movie::Decoder for VP9 video on the host with libvpx: the Windows port of upstream games/totk/runtime/movie_host.c.
   TOTK's pre-rendered cutscenes (the .webm files in Event/Movie, 28 of them: VP9 profile 0, 1920x1080, 30 fps, video
   only) all play through one SDK movie library in subsdk0, Android's MediaCodec over NVIDIA's NvMM. Its video decoder
   opens Tegra's NVDEC through nvdrv, which this port does not have, so the SDK aborted at the first cutscene (svc 0x26,
   Break). Replacing movie::Decoder's methods covers every cutscene.
   main imports the constructor and calls the 17 virtual methods through the vtable for movie::Decoder, whose slots are
   relocated by symbol name: overriding the names replaces the slots. A VP9 buffer-mode decoder (type 3, mode 1) built by
   the constructor is a host object, +0 the library's vtable and +8 a host Movie; every other decoder goes to the
   original code. Behaviour follows DecoderImpl:
   - output format 4 is packed NV12 (Y plane, then interleaved UV; stride = width), written into the client's Buffer;
   - input and output buffers are indices handed out FIFO, as MediaCodec does; flag 4 = end of stream;
   - client events {input available, output available, format changed, error} are signalled with nn::os::SignalEvent.
   Decoding is synchronous in SendInputBufferForDecode. While the game holds every output slot, inputs wait and are
   decoded when it releases one (ReleaseOutputBufferIndex).
   Pacing: the game shows one decoded frame per game frame, so decoded frames are handed over when their presentation
   time is due, counted from the first frame (movie_host_tick, at every present): a 30 fps movie plays at 30 fps at any
   game frame rate. A frame more than 1.5 periods late is dropped if the next one is due too, so the picture stays with
   the sound after a loading hitch. TOTK_MOVIE_PACE=0 hands every frame over as soon as it is decoded; TOTK_MOVIE_HOST=0
   leaves the movies to the SDK (it aborts at the first one).
   Guest-visible memory (the game reads and writes it, guest addresses stop at 2^39, malloc on Windows can land above):
   the input buffers, the strings and result slots of calls into the library come from host_guest_alloc. */
#include "host/hos.h"

#ifndef HAVE_LIBVPX
const host_fn *host_movie_override(const char *name) {
    static int said;
    if (!said && !strncmp(name, "_ZN5movie7Decoder", 17)) { said = 1; LOG("movie: built without libvpx, cutscenes abort the game"); }
    return NULL;
}
void movie_host_tick(void) {}
#else
#include <pthread.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

enum { IN_BUFS = 4, IN_CAP = 2 << 20, OUT_BUFS = 8, MAX_MOVIES = 16, IN_SETS = 2 };
enum { TYPE_VP9 = 3, MODE_BUFFER = 1, FORMAT_NV12 = 4, FLAG_EOS = 4 };
enum { ST_OK = 0, ST_ERROR = 0x1000, ST_NULL = 0x100b, ST_NO_OUTPUT = 0x100e, ST_NO_INPUT = 0x1011,
       ST_UNSUPPORTED = 0x2001, ST_NOT_CONFIGURED = 0x2002 };
enum { EV_INPUT, EV_OUTPUT, EV_FORMAT, EV_ERROR };

typedef struct { int index; unsigned flags; int64_t time; } Ready;
typedef struct { int index, offset, size; int64_t time; unsigned flags; } Pending;
typedef struct {
    void *guest;                         /* the movie::Decoder object */
    int format, configured, started, w, h, eos_in, format_sent, last_error;
    void **events;                       /* DecoderEvents: EventType* per EV_* */
    vpx_codec_ctx_t vpx; int vpx_live;
    uint8_t *in[IN_BUFS]; int in_set, in_free[IN_BUFS], nin_free;
    Pending pending[IN_BUFS]; int npending;
    uint8_t *out[OUT_BUFS]; int out_bytes[OUT_BUFS], out_held[OUT_BUFS];
    Ready ready[OUT_BUFS]; int nready;
    Ready decoded[OUT_BUFS]; int ndecoded;   /* paced: decoded, not due yet */
    uint64_t t0; int64_t pts0;               /* paced: host time and presentation time of the first frame handed over */
    uint64_t frames, decode_ns, errors, dropped;
    pthread_mutex_t mu;
} Movie;

static int pace_on(void) { static int v = -1; if (v < 0) { const char *e = pal_getenv("TOTK_MOVIE_PACE"); v = e ? strcmp(e, "0") != 0 : 1; } return v; }
static Movie *g_movies[MAX_MOVIES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

#define MLOG(...) LOG("movie: " __VA_ARGS__)

/* ---- the library's own functions, called as guest code ---- */
enum { A_SIGNAL, A_SET_RANGE, A_SET_DATA, A_BASE, A_CAPACITY, A_FIND_INT, A_FIND_STR, A_SET_INT, A_SET_RECT, A_SET_STR, A_VTABLE, A_ENTRIES };
static const char *const A_NAMES[A_ENTRIES] = {
    "_ZN2nn2os11SignalEventEPNS0_9EventTypeE", "_ZN5movie6Buffer8SetRangeEii", "_ZN5movie6Buffer18SetDataAndCapacityEPvm",
    "_ZNK5movie6Buffer4BaseEv", "_ZNK5movie6Buffer8CapacityEv", "_ZNK5movie9MediaData9FindInt32EPKcPi",
    "_ZNK5movie9MediaData10FindStringEPKcPS2_", "_ZN5movie9MediaData8SetInt32EPKci", "_ZN5movie9MediaData7SetRectEPKciiii",
    "_ZN5movie9MediaData9SetStringEPKcS2_", "_ZTVN5movie7DecoderE"};
static uint64_t A[A_ENTRIES], A_vptr;
#define GC(fn, ...) ({ uint64_t a_[] = {__VA_ARGS__}; host_guest_call((fn), (int)(sizeof a_ / sizeof *a_), a_); })
#define U(x) ((uint64_t)(uint32_t)(x))
#define P(x) ((uint64_t)(uintptr_t)(x))

enum { MOVIE_CTOR, MOVIE_D2, MOVIE_D0, MOVIE_CONFIGURE, MOVIE_REGISTER_EVENTS, MOVIE_GET_OUTPUT_FORMAT, MOVIE_START,
       MOVIE_STOP, MOVIE_RESET, MOVIE_FLUSH, MOVIE_ACQUIRE_INPUT, MOVIE_GET_INPUT, MOVIE_SEND_INPUT, MOVIE_ACQUIRE_OUTPUT,
       MOVIE_GET_OUTPUT, MOVIE_GET_OUTPUT_SIZE, MOVIE_RELEASE_OUTPUT, MOVIE_GET_LAST_ERROR, MOVIE_GET_OUTPUT_MD,
       MOVIE_GET_OUTPUT_BUFFER_FORMAT, MOVIE_ENTRIES };
static const char *const NAMES[MOVIE_ENTRIES] = {
    "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "_ZN5movie7DecoderD2Ev",
    "_ZN5movie7DecoderD0Ev", "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE",
    "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE",
    "_ZN5movie7Decoder5StartEv", "_ZN5movie7Decoder4StopEv", "_ZN5movie7Decoder5ResetEv", "_ZN5movie7Decoder5FlushEv",
    "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE",
    "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_",
    "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "_ZN5movie7Decoder12GetLastErrorEv",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", "_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE"};
static uint64_t ORIG[MOVIE_ENTRIES];   /* the library's own code per entry, for decoders left to it */

/* Guest-visible scratch: result slots (int, pointer) and the strings the library reads. */
static uint8_t *g_scratch; static size_t g_str_used; static pthread_mutex_t g_scratch_mu = PTHREAD_MUTEX_INITIALIZER;
enum { SCRATCH_STR = 64, SCRATCH_SIZE = 4096 };
static const char *gstr(const char *s) {   /* a copy of a host string where the guest can read it (kept: few and fixed) */
    for (size_t at = SCRATCH_STR; at < g_str_used; at += strlen((char *)g_scratch + at) + 1) if (!strcmp((char *)g_scratch + at, s)) return (char *)g_scratch + at;
    size_t n = strlen(s) + 1, at = g_str_used ? g_str_used : SCRATCH_STR;
    if (at + n > SCRATCH_SIZE) runtime_fail("movie decoder: string scratch full");
    memcpy(g_scratch + at, s, n); g_str_used = at + n;
    return (char *)g_scratch + at;
}
static int g_ready;
static void guest_init(void) {   /* resolved the first time any entry runs (the modules are loaded and relocated by then) */
    pthread_mutex_lock(&g_lock);
    if (!g_ready) {
        for (int i = 0; i < A_ENTRIES; i++) if (!(A[i] = host_symbol(A_NAMES[i]))) { LOG("movie: missing guest symbol %s", A_NAMES[i]); runtime_fail("movie decoder: guest symbol missing"); }
        A_vptr = A[A_VTABLE] + 0x10;   /* what the constructor stores at +0 */
        for (int i = 0; i < MOVIE_ENTRIES; i++) if (!(ORIG[i] = host_symbol(NAMES[i]))) { LOG("movie: missing guest symbol %s", NAMES[i]); runtime_fail("movie decoder: guest symbol missing"); }
        g_scratch = host_guest_alloc(SCRATCH_SIZE);
        g_ready = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void signal_event(void *ev) { GC(A[A_SIGNAL], P(ev)); }
static void buf_set_range(void *b, int off, int size) { GC(A[A_SET_RANGE], P(b), U(off), U(size)); }
static void buf_set_data(void *b, void *data, size_t cap) { GC(A[A_SET_DATA], P(b), P(data), cap); }
static uint8_t *buf_base(const void *b) { return (uint8_t *)(uintptr_t)GC(A[A_BASE], P(b)); }
static size_t buf_capacity(const void *b) { return (size_t)GC(A[A_CAPACITY], P(b)); }
static void md_set_int32(void *md, const char *k, int v) { GC(A[A_SET_INT], P(md), P(gstr(k)), U(v)); }
static void md_set_rect(void *md, const char *k, int l, int t, int r, int b) { GC(A[A_SET_RECT], P(md), P(gstr(k)), U(l), U(t), U(r), U(b)); }
static void md_set_string(void *md, const char *k, const char *v) { GC(A[A_SET_STR], P(md), P(gstr(k)), P(gstr(v))); }
static int md_find_int32(const void *md, const char *k, int *v) {
    int *slot = (int *)g_scratch;
    if (!(GC(A[A_FIND_INT], P(md), P(gstr(k)), P(slot)) & 0xff)) return 0;
    *v = *slot; return 1;
}
static int md_find_string(const void *md, const char *k, const char **v) {
    const char **slot = (const char **)(g_scratch + 8);
    if (!(GC(A[A_FIND_STR], P(md), P(gstr(k)), P(slot)) & 0xff)) return 0;
    *v = *slot; return 1;
}

static Movie *find(const void *guest) {
    if (!g_ready) guest_init();
    pthread_mutex_lock(&g_lock); Movie *m = NULL;
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movies[i] && g_movies[i]->guest == guest) m = g_movies[i];
    pthread_mutex_unlock(&g_lock);
    return m;
}

/* Events are signalled after the decoder's lock is released: the woken game thread calls straight back in. */
typedef struct { void *ev[8]; int n; } Signals;
static void note(Movie *m, Signals *s, int which) { if (m->events && m->events[which] && s->n < 8) s->ev[s->n++] = m->events[which]; }
static void fire(const Signals *s) { for (int i = 0; i < s->n; i++) signal_event(s->ev[i]); }

static int frame_bytes(const Movie *m) { return m->w * m->h + (m->w / 2) * (m->h / 2) * 2; }

/* I420 (libvpx) -> packed NV12: Y rows, then rows of interleaved U, V. */
static void to_nv12(const vpx_image_t *img, uint8_t *dst, int w, int h) {
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, img->planes[VPX_PLANE_Y] + (size_t)y * img->stride[VPX_PLANE_Y], (size_t)w);
    uint8_t *uv = dst + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = img->planes[VPX_PLANE_U] + (size_t)y * img->stride[VPX_PLANE_U];
        const uint8_t *v = img->planes[VPX_PLANE_V] + (size_t)y * img->stride[VPX_PLANE_V];
        uint8_t *o = uv + (size_t)y * w;
        for (int x = 0; x < w / 2; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
static int free_out(const Movie *m) { for (int i = 0; i < OUT_BUFS; i++) if (!m->out_held[i]) return i; return -1; }
static void push_ready(Movie *m, Signals *s, int slot, unsigned flags, int64_t time, int bytes) {
    m->out_held[slot] = 1; m->out_bytes[slot] = bytes;
    if (pace_on()) { m->decoded[m->ndecoded++] = (Ready){slot, flags, time}; return; }
    m->ready[m->nready++] = (Ready){slot, flags, time};
    note(m, s, EV_OUTPUT);
}
/* Host time a decoded frame is due (ns); end of stream is due as soon as it is first in line. */
static uint64_t due_at(Movie *m, const Ready *d) {
    int64_t ahead = d->time - m->pts0; if (ahead < 0) ahead = 0;
    return m->t0 + (uint64_t)ahead * 1000;
}
/* One game frame (a present): drop leading frames that are more than 1.5 frame periods late while the next one is due
   too, then hand over at most one due frame (end of stream right after the last frame). */
static void release_due(Movie *m, Signals *s, uint64_t now) {
    while (m->ndecoded > 1 && !(m->decoded[0].flags & FLAG_EOS) && !(m->decoded[1].flags & FLAG_EOS) && m->t0) {
        uint64_t due = due_at(m, &m->decoded[0]), next = due_at(m, &m->decoded[1]);
        if (now < next || now - due <= (next - due) * 3 / 2) break;
        m->out_held[m->decoded[0].index] = 0; m->dropped++;
        memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof m->decoded[0]);
    }
    if (!m->ndecoded) return;
    Ready d = m->decoded[0];
    if (!(d.flags & FLAG_EOS)) {
        if (!m->t0) { m->t0 = now; m->pts0 = d.time; }
        if (now < due_at(m, &d)) return;
    }
    memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof m->decoded[0]);
    m->ready[m->nready++] = d; note(m, s, EV_OUTPUT);
}
void movie_host_tick(void) {   /* lock order g_lock -> Movie.mu, as in destroy; events fire after both are released */
    Signals s = {0};
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_MOVIES; i++) {
        Movie *m = g_movies[i]; if (!m) continue;
        pthread_mutex_lock(&m->mu); if (m->started) release_due(m, &s, now_ns()); pthread_mutex_unlock(&m->mu);
    }
    pthread_mutex_unlock(&g_lock);
    fire(&s);
}
/* Decode one input into output slots; returns 0 when it had to wait for a free output slot. */
static int decode_one(Movie *m, Signals *s, const Pending *p) {
    int need = p->size > 0 ? 1 : 0;
    if (p->flags & FLAG_EOS && !m->eos_in) need++;
    int freeslots = 0; for (int i = 0; i < OUT_BUFS; i++) freeslots += !m->out_held[i];
    if (freeslots < need) return 0;
    if (p->size > 0) {
        uint64_t t0 = now_ns();
        vpx_codec_err_t e = vpx_codec_decode(&m->vpx, m->in[p->index] + p->offset, (unsigned)p->size, NULL, 0);
        if (e != VPX_CODEC_OK) {
            m->errors++; m->last_error = ST_ERROR; note(m, s, EV_ERROR);
            if (m->errors <= 4) MLOG("VP9 decode error on a %d-byte frame at %lld us: %s", p->size, (long long)p->time, vpx_codec_error_detail(&m->vpx) ? vpx_codec_error_detail(&m->vpx) : vpx_codec_err_to_string(e));
        } else {
            vpx_codec_iter_t it = NULL; const vpx_image_t *img;
            while ((img = vpx_codec_get_frame(&m->vpx, &it)) != NULL) {
                if (img->fmt != VPX_IMG_FMT_I420 || (int)img->d_w != m->w || (int)img->d_h != m->h) {
                    MLOG("VP9 frame %ux%u format %d differs from the configured %dx%d I420", img->d_w, img->d_h, img->fmt, m->w, m->h);
                    runtime_fail("movie decoder: unexpected VP9 frame layout");
                }
                int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: output slots counted wrong");
                if (!m->format_sent) { m->format_sent = 1; note(m, s, EV_FORMAT); }
                to_nv12(img, m->out[slot], m->w, m->h);
                push_ready(m, s, slot, 0, p->time, frame_bytes(m));
                m->frames++;
            }
        }
        m->decode_ns += now_ns() - t0;
        if (m->frames && m->frames % 300 == 0) MLOG("%llu frames, decode %.2f ms per frame", (unsigned long long)m->frames, m->decode_ns / 1e6 / (double)m->frames);
    }
    if (p->flags & FLAG_EOS && !m->eos_in) {
        m->eos_in = 1; int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: no slot for end of stream");
        push_ready(m, s, slot, FLAG_EOS, p->time, 0);
        MLOG("end of stream after %llu frames (%.2f ms per frame, %llu errors)", (unsigned long long)m->frames, m->frames ? m->decode_ns / 1e6 / (double)m->frames : 0.0, (unsigned long long)m->errors);
    }
    m->in_free[m->nin_free++] = p->index; note(m, s, EV_INPUT);
    return 1;
}
static void drain_pending(Movie *m, Signals *s) {
    while (m->npending && decode_one(m, s, &m->pending[0])) { memmove(m->pending, m->pending + 1, (size_t)--m->npending * sizeof m->pending[0]); }
}

/* The input buffers are read and written by the game: guest-visible, from a small pool made once. */
static uint8_t *g_in_pool[IN_SETS][IN_BUFS]; static int g_in_used[IN_SETS]; static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static void take_inputs(Movie *m) {
    pthread_mutex_lock(&g_pool_mu);
    int s = 0; while (s < IN_SETS && g_in_used[s]) s++;
    if (s == IN_SETS) runtime_fail("movie decoder: too many decoders configured at once");
    g_in_used[s] = 1;
    if (!g_in_pool[s][0]) for (int i = 0; i < IN_BUFS; i++) g_in_pool[s][i] = host_guest_alloc(IN_CAP);
    pthread_mutex_unlock(&g_pool_mu);
    m->in_set = s; for (int i = 0; i < IN_BUFS; i++) m->in[i] = g_in_pool[s][i];
}
static void release_codec(Movie *m) {
    if (m->vpx_live) { vpx_codec_destroy(&m->vpx); m->vpx_live = 0; }
    if (m->in_set >= 0) { pthread_mutex_lock(&g_pool_mu); g_in_used[m->in_set] = 0; pthread_mutex_unlock(&g_pool_mu); m->in_set = -1; }
    for (int i = 0; i < IN_BUFS; i++) m->in[i] = NULL;
    for (int i = 0; i < OUT_BUFS; i++) { free(m->out[i]); m->out[i] = NULL; }
    m->configured = m->started = 0; m->nin_free = m->npending = m->nready = m->ndecoded = 0; m->t0 = 0;
}
static void reset_queues(Movie *m) {
    m->nin_free = 0; for (int i = 0; i < IN_BUFS; i++) m->in_free[m->nin_free++] = i;
    memset(m->out_held, 0, sizeof m->out_held); m->npending = m->nready = m->ndecoded = 0; m->eos_in = 0; m->t0 = 0;
}

/* ---- entries (the guest calls them with x0.. as integers: int arguments arrive as 64-bit values, results are int) ---- */
static uint64_t orig(int which, int n, const uint64_t *a) { return host_guest_call(ORIG[which], n, a); }
static int orig_int(int which, int n, const uint64_t *a) { return (int)(uint32_t)orig(which, n, a); }
static void h_ctor(void *obj, int type, int format, int mode, int priority) {
    if (!g_ready) guest_init();
    if (type != TYPE_VP9 || mode != MODE_BUFFER) {
        MLOG("decoder type %d mode %d format %d: left to the library (only VP9 in buffer mode runs on the host)", type, mode, format);
        uint64_t a[] = {P(obj), U(type), U(format), U(mode), U(priority)}; orig(MOVIE_CTOR, 5, a); return;
    }
    Movie *m = calloc(1, sizeof *m); if (!m) runtime_fail("movie decoder allocation");
    m->guest = obj; m->format = format; m->in_set = -1; pthread_mutex_init(&m->mu, NULL);
    pthread_mutex_lock(&g_lock); int i = 0;
    while (i < MAX_MOVIES && g_movies[i] && g_movies[i]->guest != obj) i++;
    if (i == MAX_MOVIES) runtime_fail("movie decoder table full");
    if (g_movies[i]) { MLOG("decoder object %p reused without destruction", obj); release_codec(g_movies[i]); free(g_movies[i]); }
    g_movies[i] = m; pthread_mutex_unlock(&g_lock);
    ((uint64_t *)obj)[0] = A_vptr; ((void **)obj)[1] = m;
    MLOG("VP9 decoder %p on the host (libvpx %s), output format %d", obj, vpx_codec_version_str(), format);
}
static void destroy(void *obj) {
    pthread_mutex_lock(&g_lock); Movie *m = NULL;
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movies[i] && g_movies[i]->guest == obj) { m = g_movies[i]; g_movies[i] = NULL; }
    if (m) { pthread_mutex_lock(&m->mu); pthread_mutex_unlock(&m->mu); }   /* wait for an operation in progress (movie_host_tick) */
    pthread_mutex_unlock(&g_lock);
    if (!m) return;
    MLOG("decoder %p destroyed after %llu frames", obj, (unsigned long long)m->frames);
    release_codec(m); pthread_mutex_destroy(&m->mu); free(m);
    ((uint64_t *)obj)[0] = A_vptr; ((uint64_t *)obj)[1] = 0;   /* what the library's destructor leaves (no DecoderImpl) */
}
static void h_d2(void *obj) { if (!find(obj)) { uint64_t a[] = {P(obj)}; orig(MOVIE_D2, 1, a); return; } destroy(obj); }
/* Deleting destructor: free the host state, then the library's own deleting destructor frees the object (impl now 0). */
static void h_d0(void *obj) { if (find(obj)) destroy(obj); uint64_t a[] = {P(obj)}; orig(MOVIE_D0, 1, a); }

static int h_configure(void *obj, void *format) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), P(format)}; return orig_int(MOVIE_CONFIGURE, 2, a); }
    if (!format) return ST_NULL;
    if (m->format != FORMAT_NV12) { MLOG("output format %d not implemented on the host (NV12 = 4 only)", m->format); return ST_UNSUPPORTED; }
    const char *mime = NULL; int w = 0, h = 0;
    pthread_mutex_lock(&g_scratch_mu);
    md_find_string(format, "mime", &mime); md_find_int32(format, "width", &w); md_find_int32(format, "height", &h);
    int mime_ok = mime && !strcmp(mime, "video/x-vnd.on2.vp9");
    pthread_mutex_unlock(&g_scratch_mu);
    if (!mime_ok || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w | h) & 1) {
        MLOG("configure: mime %s %dx%d not supported", mime ? mime : "(none)", w, h); return ST_UNSUPPORTED;
    }
    pthread_mutex_lock(&m->mu);
    release_codec(m); m->w = w; m->h = h;
    vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = (unsigned)w, .h = (unsigned)h};
    if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) runtime_fail("libvpx VP9 decoder initialisation");
    m->vpx_live = 1;
    take_inputs(m);
    for (int i = 0; i < OUT_BUFS; i++) if (!(m->out[i] = malloc((size_t)frame_bytes(m)))) runtime_fail("movie output buffers");
    reset_queues(m); m->format_sent = 0; m->configured = 1; m->last_error = 0;
    pthread_mutex_unlock(&m->mu);
    MLOG("configured %dx%d VP9, %d input buffers of %d KiB, %d output frames of %d bytes", w, h, IN_BUFS, IN_CAP >> 10, OUT_BUFS, frame_bytes(m));
    return ST_OK;
}
static int h_register_events(void *obj, void *events) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), P(events)}; return orig_int(MOVIE_REGISTER_EVENTS, 2, a); }
    m->events = events; return ST_OK;
}
static void set_format(Movie *m, void *md) {
    pthread_mutex_lock(&g_scratch_mu);
    md_set_string(md, "mime", "video/raw");
    md_set_int32(md, "width", m->w); md_set_int32(md, "height", m->h);
    md_set_int32(md, "stride", m->w); md_set_int32(md, "slice-height", m->h);
    md_set_int32(md, "color-format", 21);   /* COLOR_FormatYUV420SemiPlanar */
    md_set_rect(md, "crop", 0, 0, m->w - 1, m->h - 1);
    md_set_int32(md, "crop-width", m->w); md_set_int32(md, "crop-height", m->h);
    md_set_int32(md, "nv12-y-stride", m->w); md_set_int32(md, "nv12-uv-stride", m->w);
    md_set_int32(md, "nv12-y-offset", 0); md_set_int32(md, "nv12-uv-offset", m->w * m->h);
    pthread_mutex_unlock(&g_scratch_mu);
}
static int h_get_output_format(void *obj, void *md) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), P(md)}; return orig_int(MOVIE_GET_OUTPUT_FORMAT, 2, a); }
    if (!md) return ST_NULL;
    if (!m->configured) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
static int h_start(void *obj) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj)}; return orig_int(MOVIE_START, 1, a); }
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    if (!m->configured) { pthread_mutex_unlock(&m->mu); return ST_NOT_CONFIGURED; }
    m->started = 1; note(m, &s, EV_INPUT);
    pthread_mutex_unlock(&m->mu); fire(&s);
    return ST_OK;
}
static int h_stop(void *obj) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj)}; return orig_int(MOVIE_STOP, 1, a); }
    pthread_mutex_lock(&m->mu); MLOG("stop after %llu frames (%llu dropped to keep time)", (unsigned long long)m->frames, (unsigned long long)m->dropped); release_codec(m); pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_reset(void *obj) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj)}; return orig_int(MOVIE_RESET, 1, a); }
    pthread_mutex_lock(&m->mu); release_codec(m); m->last_error = 0; pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_flush(void *obj) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj)}; return orig_int(MOVIE_FLUSH, 1, a); }
    pthread_mutex_lock(&m->mu);
    if (m->configured) {   /* drop queued work; the next input starts a new decode (after a seek: a key frame) */
        vpx_codec_destroy(&m->vpx); m->vpx_live = 0;
        vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = (unsigned)m->w, .h = (unsigned)m->h};
        if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) runtime_fail("libvpx VP9 decoder re-initialisation");
        m->vpx_live = 1; reset_queues(m);
    }
    pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_acquire_input(void *obj, int *index, int *remaining) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), P(index), P(remaining)}; return orig_int(MOVIE_ACQUIRE_INPUT, 3, a); }
    if (!index || !remaining) return ST_NULL;
    pthread_mutex_lock(&m->mu);
    *index = -1; *remaining = 0; int st = ST_NO_INPUT;
    if (m->started && m->nin_free > 0) {
        *index = m->in_free[0]; memmove(m->in_free, m->in_free + 1, (size_t)--m->nin_free * sizeof m->in_free[0]);
        *remaining = m->nin_free; st = ST_OK;
    }
    pthread_mutex_unlock(&m->mu); return st;
}
static int h_get_input(void *obj, int index, void *buf) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), U(index), P(buf)}; return orig_int(MOVIE_GET_INPUT, 3, a); }
    if (!buf) return ST_NULL;
    if (index < 0 || index >= IN_BUFS || !m->in[index]) { buf_set_range(buf, 0, 0); buf_set_data(buf, NULL, 0); return ST_ERROR; }
    buf_set_range(buf, 0, IN_CAP); buf_set_data(buf, m->in[index], IN_CAP);
    return ST_OK;
}
static int h_send_input(void *obj, int index, int offset, int size, int64_t time, unsigned flags) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), U(index), U(offset), U(size), (uint64_t)time, U(flags)}; return orig_int(MOVIE_SEND_INPUT, 6, a); }
    if (index < 0 || index >= IN_BUFS || offset < 0 || size < 0 || (int64_t)offset + size > IN_CAP) return ST_ERROR;
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    if (!m->started) { pthread_mutex_unlock(&m->mu); return ST_ERROR; }
    Pending p = {index, offset, size, time, flags};
    if (m->eos_in) { m->in_free[m->nin_free++] = index; note(m, &s, EV_INPUT); }   /* after end of stream: hand the buffer back */
    else if (m->npending || !decode_one(m, &s, &p)) m->pending[m->npending++] = p;
    pthread_mutex_unlock(&m->mu); fire(&s);
    return ST_OK;
}
static int h_acquire_output(void *obj, int *index, int64_t *time, unsigned *flags, int *remaining) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), P(index), P(time), P(flags), P(remaining)}; return orig_int(MOVIE_ACQUIRE_OUTPUT, 5, a); }
    if (!index || !time || !flags || !remaining) return ST_NULL;
    pthread_mutex_lock(&m->mu);   /* paced frames become ready only at presents (movie_host_tick): one per game frame */
    *index = -1; *time = 0; *flags = 0; *remaining = 0; int st = ST_NO_OUTPUT;
    if (m->nready > 0) {
        Ready r = m->ready[0]; memmove(m->ready, m->ready + 1, (size_t)--m->nready * sizeof m->ready[0]);
        *index = r.index; *time = r.time; *flags = r.flags; *remaining = m->nready; st = ST_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return st;
}
static int h_get_output(void *obj, int index, void *buf) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), U(index), P(buf)}; return orig_int(MOVIE_GET_OUTPUT, 3, a); }
    if (!buf) return ST_NULL;
    buf_set_range(buf, 0, 0);
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        int bytes = m->out_bytes[index]; uint8_t *dst = buf_base(buf);
        if (!bytes) st = ST_OK;
        else if (dst && buf_capacity(buf) >= (size_t)bytes) { memcpy(dst, m->out[index], (size_t)bytes); buf_set_range(buf, 0, bytes); st = ST_OK; }
        else MLOG("output buffer of %zu bytes for a %d-byte frame", buf_capacity(buf), bytes);
    }
    pthread_mutex_unlock(&m->mu); return st;
}
static int h_get_output_size(void *obj, int *size, int *count) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), P(size), P(count)}; return orig_int(MOVIE_GET_OUTPUT_SIZE, 3, a); }
    if (!size || !count) return ST_NULL;
    *count = 1; *size = m->configured ? frame_bytes(m) : 0;
    return ST_OK;
}
static int h_release_output(void *obj, int index) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), U(index)}; return orig_int(MOVIE_RELEASE_OUTPUT, 2, a); }
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        int queued = 0; for (int i = 0; i < m->nready; i++) queued |= m->ready[i].index == index;
        for (int i = 0; i < m->ndecoded; i++) queued |= m->decoded[i].index == index;
        if (!queued) { m->out_held[index] = 0; drain_pending(m, &s); st = ST_OK; }
    }
    pthread_mutex_unlock(&m->mu); fire(&s);
    return st;
}
static int h_get_last_error(void *obj) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj)}; return orig_int(MOVIE_GET_LAST_ERROR, 1, a); }
    return m->last_error;
}
static int h_get_output_buffer_format(void *obj, int index, void *md) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), U(index), P(md)}; return orig_int(MOVIE_GET_OUTPUT_BUFFER_FORMAT, 3, a); }
    if (!md) return ST_NULL;
    if (index < 0 || index >= OUT_BUFS || !m->out_held[index]) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
static int h_get_output_md(void *obj, int index, void *buf, void *md) {
    Movie *m = find(obj); if (!m) { uint64_t a[] = {P(obj), U(index), P(buf), P(md)}; return orig_int(MOVIE_GET_OUTPUT_MD, 4, a); }
    if (!buf && !md) return ST_NULL;
    int st = md ? h_get_output_buffer_format(obj, index, md) : ST_OK;
    return st == ST_OK && buf ? h_get_output(obj, index, buf) : st;
}

static void *const ENTRIES[MOVIE_ENTRIES] = {
    h_ctor, h_d2, h_d0, h_configure, h_register_events, h_get_output_format, h_start, h_stop, h_reset, h_flush,
    h_acquire_input, h_get_input, h_send_input, h_acquire_output, h_get_output, h_get_output_size, h_release_output,
    h_get_last_error, h_get_output_md, h_get_output_buffer_format};

/* constructor, destructors: no result; the rest return an int status */
static const host_fn ALIASES[] = {
    {"_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", h_ctor, HOST_RET_VOID},   /* base-object constructor */
    {"_ZN5movie7DecoderD1Ev", h_d2, HOST_RET_VOID},                                                                 /* complete-object destructor */
};
const host_fn *host_movie_override(const char *name) {
    static int off = -1; static host_fn table[MOVIE_ENTRIES];
    if (off < 0) { const char *e = pal_getenv("TOTK_MOVIE_HOST"); off = e && !strcmp(e, "0"); }
    if (off || strncmp(name, "_ZN5movie7Decoder", 17)) return NULL;
    for (int i = 0; i < MOVIE_ENTRIES; i++) if (!strcmp(name, NAMES[i])) {
        table[i] = (host_fn){NAMES[i], ENTRIES[i], i <= MOVIE_D0 ? HOST_RET_VOID : HOST_RET_U32};
        return &table[i];
    }
    for (size_t i = 0; i < sizeof ALIASES / sizeof *ALIASES; i++) if (!strcmp(name, ALIASES[i].name)) return &ALIASES[i];
    return NULL;
}
#endif   /* libvpx */
