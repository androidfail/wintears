/* The GPU backend in use (nvn_backend.h): Vulkan when it starts (it needs the window), else the null backend.
   TOTK_GPU=null picks the null backend, TOTK_GPU=vulkan stops the run if Vulkan does not start.

   Translation thread (TOTK_ASYNC_REPLAY=1, upstream's design in games/by3 hle_nvn.c): submits and presents are queued in
   order to one host thread that replays them; the game's submit returns at once, so the game builds the next frame while
   this one is translated (on the Steam Deck the replay was ~27 ms of every frame on the game's critical path).
    - Fences: FenceSync takes the number of submits queued so far; a wait first waits until that submit is replayed, then
      for the backend fence it ended with. "Signalled" thus means translated and GPU done, so memory the game reuses after
      a fence (command / control memory, pools, objects) is never read by a queued replay.
    - Pool add / delete and texture / program release are jobs in the same order; reading or writing what the GPU holds
      (texels, finish) first waits until the queue is empty; invalidate relies on the game's fence wait. */
#include "gfx/nvn_backend.h"
#include "host/hos.h"

static const gpu_ops *B = &gpu_null_ops;
static int A;   /* the translation thread is on */

enum { J_SUBMIT = 1, J_PRESENT, J_TEXREL, J_PROGREL, J_POOL_ADD, J_POOL_DEL };
typedef struct { int kind, n; uint64_t *h; const tex_t *t; double period; int crop[4]; void *obj; const void *mem; uint64_t size, sub; } job;
#define XQ 64     /* queued jobs at most (the game waits for room) */
#define XF 4096   /* fences / present tickets remembered */
static job g_q[XQ];
static uint64_t g_in, g_done, g_sub_in, g_sub_done, g_sub_fence[XF], g_ticket[XF];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

void nvn_pin_step_now(void);   /* nvn.c: free texture copies no recording refers to, before a submit's replay */

static void *x_thread(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_done == g_in) pthread_cond_wait(&g_cv, &g_mu);
        job j = g_q[g_done % XQ];
        pthread_mutex_unlock(&g_mu);
        uint64_t ticket = 0, fence = 0;
        switch (j.kind) {
        case J_SUBMIT: nvn_pin_step_now(); fence = B->submit(j.h, j.n); free(j.h); break;
        case J_PRESENT: ticket = B->present(j.t, j.period, j.crop); break;
        case J_TEXREL: B->tex_release(j.obj); free(j.obj); break;
        case J_PROGREL: B->prog_release(j.obj); free(j.obj); break;
        case J_POOL_ADD: B->pool_add(j.mem, j.size); break;
        case J_POOL_DEL: B->pool_del(j.mem); break;
        }
        pthread_mutex_lock(&g_mu);
        if (j.kind == J_SUBMIT) { g_sub_fence[j.sub % XF] = fence; g_sub_done = j.sub; }
        if (j.kind == J_PRESENT) g_ticket[(g_done + 1) % XF] = ticket;
        g_done++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static uint64_t push(job *j) {   /* the job's number (1-based) */
    int away = runtime_core_away();   /* a guest thread waiting for room gives its virtual core back */
    pthread_mutex_lock(&g_mu);
    while (g_in - g_done >= XQ) pthread_cond_wait(&g_cv, &g_mu);
    if (j->kind == J_SUBMIT) j->sub = ++g_sub_in;
    g_q[g_in % XQ] = *j;
    uint64_t no = ++g_in;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    runtime_core_back(&away);
    return no;
}
static void wait_job(uint64_t no) {
    pthread_mutex_lock(&g_mu); int busy = g_done < no; pthread_mutex_unlock(&g_mu);
    if (!busy) return;
    int away = runtime_core_away();
    pthread_mutex_lock(&g_mu); while (g_done < no) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu);
    runtime_core_back(&away);
}
static void drain(void) {   /* everything queued so far has run */
    if (!A) return;
    pthread_mutex_lock(&g_mu); uint64_t no = g_in; pthread_mutex_unlock(&g_mu);
    wait_job(no);
}
static void *dup_obj(const void *o, size_t n) { void *c = malloc(n); if (!c) die("translation thread job"); memcpy(c, o, n); return c; }

void gpu_init(void) {
    const char *want = pal_getenv("TOTK_GPU");
    B = &gpu_null_ops;
    if (!want || strcmp(want, "null")) {
        if (!gpu_vk_ops.init()) B = &gpu_vk_ops;
        else if (want && !strcmp(want, "vulkan")) die("TOTK_GPU=vulkan: the Vulkan backend did not start");
        else LOG("[gpu] Vulkan backend not available: nothing is drawn (null backend)");
    }
    if (B == &gpu_null_ops) B->init();
    const char *e = pal_getenv("TOTK_ASYNC_REPLAY"); A = e && atoi(e) > 0;
    if (A) {
        pthread_t t;
        if (pthread_create(&t, NULL, x_thread, NULL)) die("translation thread");
        pthread_detach(t);
        LOG("[gpu] translation thread: submits and presents are replayed on their own thread (TOTK_ASYNC_REPLAY)");
    }
}
int gpu_async(void) { return A; }
const char *gpu_name(void) { return B->name; }
void gpu_pool_add(const void *mem, uint64_t size) { if (A) push(&(job){.kind = J_POOL_ADD, .mem = mem, .size = size}); else B->pool_add(mem, size); }
void gpu_pool_del(const void *mem) { if (A) push(&(job){.kind = J_POOL_DEL, .mem = mem}); else B->pool_del(mem); }
/* Not drained: the game reads back after waiting on a fence, which already means "replayed and GPU done"; draining here
   made each of ~150 calls a second wait ~2 ms for the next frame's replay (Steam Deck). */
void gpu_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { B->pool_invalidate(p, off, n); }
void gpu_set_tex_pool_n(int n) { B->set_tex_pool_n(n); }
void gpu_reg_tex(int id, const tex_t *t, const view_t *v) { B->reg_tex(id, t, v); }   /* the backend locks its tables */
void gpu_reg_samp(int id, const samp_par *p) { B->reg_samp(id, p); }
void gpu_tex_release(tex_t *t) { if (A) push(&(job){.kind = J_TEXREL, .obj = dup_obj(t, sizeof *t)}); else B->tex_release(t); }
void gpu_prog_release(prog_t *p) { if (A) push(&(job){.kind = J_PROGREL, .obj = dup_obj(p, sizeof *p)}); else B->prog_release(p); }
void gpu_window_tex(uint64_t serial) { B->window_tex(serial); }
void gpu_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { drain(); B->read_texels(t, level, layer0, r, dst, row, img); }
void gpu_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) { drain(); B->write_texels(t, level, layer0, r, src, row, img); }
void gpu_pin_refs(void (*mark)(const void *)) { B->pin_refs(mark); }
uint64_t gpu_submit(const uint64_t *handles, int n) {
    if (!A) return B->submit(handles, n);
    job j = {.kind = J_SUBMIT, .n = n, .h = dup_obj(handles, sizeof *handles * (size_t)(n > 0 ? n : 1))};
    push(&j);
    return j.sub;
}
uint64_t gpu_last_fence(void) {
    if (!A) return B->last_fence();
    pthread_mutex_lock(&g_mu); uint64_t k = g_sub_in; pthread_mutex_unlock(&g_mu); return k;
}
int gpu_wait(uint64_t k, uint64_t timeout_ns) {
    if (!A) return B->wait(k, timeout_ns);
    if (!k) return 1;
    uint64_t t0 = pal_now_ns();
    pthread_mutex_lock(&g_mu);
    if (g_sub_done < k && timeout_ns) {
        uint64_t end = pal_wall_time_ns() + (timeout_ns > (1ull << 62) ? (1ull << 62) : timeout_ns);
        struct timespec ts = {(time_t)(end / 1000000000ull), (long)(end % 1000000000ull)};
        while (g_sub_done < k && pthread_cond_timedwait(&g_cv, &g_mu, &ts) == 0) {}
    }
    int ok = g_sub_done >= k;
    uint64_t f = g_sub_done - k < XF ? g_sub_fence[k % XF] : g_sub_fence[g_sub_done % XF];   /* overwritten: a later fence is safe */
    pthread_mutex_unlock(&g_mu);
    if (!ok) return 0;
    uint64_t used = pal_now_ns() - t0;
    return B->wait(f, timeout_ns > used ? timeout_ns - used : 0);
}
void gpu_finish(void) { drain(); B->finish(); }
uint64_t gpu_present(const tex_t *t, double period_s, const int crop[4]) {
    if (!A) return B->present(t, period_s, crop);
    job j = {.kind = J_PRESENT, .t = t, .period = period_s};
    memcpy(j.crop, crop, sizeof j.crop);
    return push(&j);
}
void gpu_present_wait(uint64_t ticket) {
    if (!A) { B->present_wait(ticket); return; }
    wait_job(ticket);
    pthread_mutex_lock(&g_mu); uint64_t t = g_ticket[ticket % XF]; pthread_mutex_unlock(&g_mu);
    B->present_wait(t);
}
int gpu_display_hz(void) { return B->display_hz(); }
