/* The GPU backend in use (nvn_backend.h): Vulkan when it starts (it needs the window), else the null backend.
   TOTK_GPU=null picks the null backend, TOTK_GPU=vulkan stops the run if Vulkan does not start. */
#include "gfx/nvn_backend.h"
#include "host/hos.h"

static const gpu_ops *B = &gpu_null_ops;

void gpu_init(void) {
    const char *want = pal_getenv("TOTK_GPU");
    if (!want || strcmp(want, "null")) {
        if (!gpu_vk_ops.init()) { B = &gpu_vk_ops; return; }
        if (want && !strcmp(want, "vulkan")) die("TOTK_GPU=vulkan: the Vulkan backend did not start");
        LOG("[gpu] Vulkan backend not available: nothing is drawn (null backend)");
    }
    B = &gpu_null_ops; B->init();
}
const char *gpu_name(void) { return B->name; }
void gpu_pool_add(const void *mem, uint64_t size) { B->pool_add(mem, size); }
void gpu_pool_del(const void *mem) { B->pool_del(mem); }
void gpu_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { B->pool_invalidate(p, off, n); }
void gpu_set_tex_pool_n(int n) { B->set_tex_pool_n(n); }
void gpu_reg_tex(int id, const tex_t *t, const view_t *v) { B->reg_tex(id, t, v); }
void gpu_reg_samp(int id, const samp_par *p) { B->reg_samp(id, p); }
void gpu_tex_release(tex_t *t) { B->tex_release(t); }
void gpu_prog_release(prog_t *p) { B->prog_release(p); }
void gpu_window_tex(uint64_t serial) { B->window_tex(serial); }
void gpu_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { B->read_texels(t, level, layer0, r, dst, row, img); }
void gpu_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) { B->write_texels(t, level, layer0, r, src, row, img); }
void gpu_pin_refs(void (*mark)(const void *)) { B->pin_refs(mark); }
uint64_t gpu_submit(const uint64_t *handles, int n) { return B->submit(handles, n); }
uint64_t gpu_last_fence(void) { return B->last_fence(); }
int gpu_wait(uint64_t fence, uint64_t timeout_ns) { return B->wait(fence, timeout_ns); }
void gpu_finish(void) { B->finish(); }
uint64_t gpu_present(const tex_t *t, double period_s, const int crop[4]) { return B->present(t, period_s, crop); }
void gpu_present_wait(uint64_t ticket) { B->present_wait(ticket); }
int gpu_display_hz(void) { return B->display_hz(); }
