/* The replay state (vk_exec.c) and the draw path (vk_draw.c): programs, pipelines, resource binding, draws, dispatches. */
#pragma once
#include "gfx/vk/vk.h"

typedef struct {
    /* render targets and pass-level state (vk_exec.c) */
    p_set_rt rt; int has_rt;
    p_scissor sc; int has_sc;
    int render_disabled; int64_t copy_stride;
    struct { int ncolor; VkFormat color[8], depth, stencil; } pass_fmt;   /* formats of the open pass */
    uint32_t pass_w, pass_h, pass_samples;
    /* bound program and state objects (vk_draw.c) */
    const prog_t *prog; int prog_stages;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    uint32_t attr[16]; int nattr;
    vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][128], samp[6][128], img[6][32];
    float vp[16][4]; int has_vp;
    float dr[2];
    int stencil_vmask[2], stencil_wmask[2], stencil_ref[2];
    p_poly_offset po;
    float blend_color[4];
    int depth_clamp, raster_discard;
} vx_state;
extern vx_state S;

int vx_begin_pass(void);                    /* vk_exec.c: open a pass on the current targets (0 = none) */
void vd_state_op(uint32_t op, const void *a);
void vd_draw(const p_draw *dw, const p_draw_idx *di);
void vd_dispatch(const p_dispatch *d);
void vd_init(void);
int vd_ub_update(uint64_t dst, const void *data, uint64_t n);   /* 1: logged (renamed), 0: write it on the GPU timeline */
void vd_ub_guard(uint64_t lo, uint64_t hi);   /* before a non-uniform GPU access to [lo, hi) */
void vd_ub_flush(void);
