/* What the NVN layer (nvn.c, nvn_ext.c) asks of the GPU backend. Upstream's backend is Metal (nvn_mtl.m, mtl_*); the
   names here follow it without the prefix. Two backends implement them as a gpu_ops table, picked by gpu_init
   (nvn_gpu.c): Vulkan (vk/, with a window) and null (nvn_null.c: draws nothing; it does the effects the CPU can see,
   event values, counters, buffer copies, uniform updates, and keeps the swapchain pacing). TOTK_GPU=null|vulkan.

   Ordering rule (upstream's): every visible effect of "GPU completion" (sync object ready, event values, counters)
   happens only after the GPU has run the commands; CPU waits look only at the completed fence. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_obj.h"

void gpu_init(void);
const char *gpu_name(void);

/* memory pools: [mem, mem+size), GPU address = CPU address */
void gpu_pool_add(const void *mem, uint64_t size);
void gpu_pool_del(const void *mem);
void gpu_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n);   /* the CPU is about to read what the GPU wrote */

/* descriptor pools, textures, programs */
void gpu_set_tex_pool_n(int n);
void gpu_reg_tex(int id, const tex_t *t, const view_t *v);
void gpu_reg_samp(int id, const samp_par *p);
void gpu_tex_release(tex_t *t);
void gpu_prog_release(prog_t *p);
void gpu_window_tex(uint64_t serial);   /* a swapchain texture */
void gpu_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
void gpu_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img);
/* the backend's state that keeps recorded texture copies alive past a submit (nvn.c pin sweep) */
void gpu_pin_refs(void (*mark)(const void *));

/* submission: the recordings (handles) in order; returns the fence value it completes at */
uint64_t gpu_submit(const uint64_t *handles, int n);
uint64_t gpu_last_fence(void);                       /* 0 = never submitted */
int gpu_wait(uint64_t fence, uint64_t timeout_ns);   /* 1 = the GPU has completed up to fence */
void gpu_finish(void);                               /* everything submitted has completed */

/* present: enqueue the swapchain texture, return a ticket at once; period_s = how long it is shown at least */
uint64_t gpu_present(const tex_t *t, double period_s, const int crop[4]);
void gpu_present_wait(uint64_t ticket);
int gpu_display_hz(void);                            /* 0 = no window */

/* A backend: the functions above (nvn_gpu.c forwards each gpu_x to ops->x). */
typedef struct {
    const char *name;
    int (*init)(void);   /* 0 = ready */
    void (*pool_add)(const void *mem, uint64_t size);
    void (*pool_del)(const void *mem);
    void (*pool_invalidate)(const pool_t *p, uint64_t off, uint64_t n);
    void (*set_tex_pool_n)(int n);
    void (*reg_tex)(int id, const tex_t *t, const view_t *v);
    void (*reg_samp)(int id, const samp_par *p);
    void (*tex_release)(tex_t *t);
    void (*prog_release)(prog_t *p);
    void (*window_tex)(uint64_t serial);
    void (*read_texels)(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
    void (*write_texels)(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img);
    void (*pin_refs)(void (*mark)(const void *));
    uint64_t (*submit)(const uint64_t *handles, int n);
    uint64_t (*last_fence)(void);
    int (*wait)(uint64_t fence, uint64_t timeout_ns);
    void (*finish)(void);
    uint64_t (*present)(const tex_t *t, double period_s, const int crop[4]);
    void (*present_wait)(uint64_t ticket);
    int (*display_hz)(void);
} gpu_ops;
extern const gpu_ops gpu_null_ops, gpu_vk_ops;
