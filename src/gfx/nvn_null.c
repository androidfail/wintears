/* The null GPU backend: draws nothing, so the game runs on past graphics start-up before the Vulkan backend exists.

   It does what the CPU can observe of the GPU, in submission order: event values (SignalEvent), counter reports
   (ReportCounter: {counter, timestamp} in 19.2 MHz ticks on the CNTPCT clock), buffer copies and uniform updates.
   Everything runs at submit, so every fence is complete when the submit call returns. Texture contents do not exist:
   CPU reads of textures return zeros. */
#include <pthread.h>

#include "gfx/nvn_backend.h"
#include "host/hos.h"
#include "nvn_tex.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence, g_tickets;

void gpu_init(void) {}
const char *gpu_name(void) { return "null"; }

void gpu_pool_add(const void *mem, uint64_t size) { (void)mem; (void)size; }
void gpu_pool_del(const void *mem) { (void)mem; }
void gpu_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { (void)p; (void)off; (void)n; }

void gpu_set_tex_pool_n(int n) { (void)n; }
void gpu_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void gpu_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void gpu_tex_release(tex_t *t) { (void)t; }
void gpu_prog_release(prog_t *p) { (void)p; }
void gpu_window_tex(uint64_t serial) { (void)serial; }
void gpu_pin_refs(void (*mark)(const void *)) { (void)mark; }

void gpu_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)level; (void)layer0;
    static int said; if (!said++) LOG("[nvn-null] texture read back by the CPU: zeros (no GPU contents)");
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)(r->height + bh - 1) / (size_t)bh;
    for (int z = 0; z < (r->depth > 0 ? r->depth : 1); z++)
        for (size_t y = 0; y < hb; y++) memset(dst + (size_t)z * img + y * row, 0, wb);
}
void gpu_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)r; (void)src; (void)row; (void)img;
}

/* 19.2 MHz ticks on the clock of the game's CNTPCT reads (src/cpu/jit/jit.cpp default_cntpct) */
static uint64_t gpu_ticks(void) { uint64_t ns = pal_now_ns(); return ns / 625 * 12 + ns % 625 * 12 / 625; }

static void run(const uint8_t *p, int depth) {
    if (depth > 16) die("null backend: call depth over 16");
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("null backend: corrupt recording at %p (op %u size %u)", (const void *)p, h.op, h.size);
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); run((const uint8_t *)(uintptr_t)t, depth + 1); break; }
        case OP_SIGNAL_EVENT: {
            volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
            if (m == NVN_EVENT_SIGNAL_MODE_ADD) *val += v; else *val = v;
            break;
        }
        case OP_REPORT_COUNTER: {
            /* NVNcounterData = {counter, timestamp}. TIMESTAMP / TIMESTAMP_TOP reports write counter 0 (the game's
               conditional clears depend on it); occlusion queries report "visible", as no depth exists to test. */
            uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8);
            uint64_t rec[2] = {t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1u << 20 : 0, gpu_ticks()};
            memcpy((void *)(uintptr_t)ad, rec, sizeof rec);
            break;
        }
        case OP_COPY_B2B: {
            uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8);
            memmove((void *)(uintptr_t)d, (const void *)(uintptr_t)s, n);
            break;
        }
        case OP_UPDATE_UB: {
            uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8);
            memcpy((void *)(uintptr_t)d, a + 16, n);
            break;
        }
        default: break;   /* GPU state, draws, clears, texture copies: nothing to draw into */
        }
        p += h.size;
    }
}

uint64_t gpu_submit(const uint64_t *handles, int n) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < n; i++) run((const uint8_t *)(uintptr_t)handles[i], 0);
    uint64_t f = ++g_fence;
    pthread_mutex_unlock(&g_mu);
    static int said; if (!said++) LOG("[nvn-null] first submit (%d command lists)", n);
    return f;
}
uint64_t gpu_last_fence(void) { pthread_mutex_lock(&g_mu); uint64_t f = g_fence; pthread_mutex_unlock(&g_mu); return f; }
int gpu_wait(uint64_t fence, uint64_t timeout_ns) { (void)timeout_ns; return fence <= gpu_last_fence(); }
void gpu_finish(void) {}

uint64_t gpu_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)period_s; (void)crop;
    return __atomic_add_fetch(&g_tickets, 1, __ATOMIC_RELAXED);
}
void gpu_present_wait(uint64_t ticket) { (void)ticket; }
int gpu_display_hz(void) { return 0; }
