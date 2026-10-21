/* Draw path: bound state (upstream nvn_mtl.m state_op / state_reset), then programs, pipelines and draws.
   Draws and dispatches are counted and skipped until the shader translation is in (next step). */
#include "gfx/vk/vk_draw.h"

static uint64_t g_draws, g_dispatches;
static int g_state_ready;

static void state_reset(void) {
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.stencil_vmask[0] = S.stencil_vmask[1] = S.stencil_wmask[0] = S.stencil_wmask[1] = 0xff; S.dr[1] = 1;
    g_state_ready = 1;
}

void vd_state_op(uint32_t op, const void *a) {
    if (!g_state_ready) state_reset();
    switch (op) {
    case OP_SCISSOR: break;   /* vk_exec.c keeps it */
    case OP_BIND_PROGRAM: S.prog = ((const p_bind_program *)a)->prog; S.prog_stages = ((const p_bind_program *)a)->stages; break;
    case OP_BIND_STATE: {
        const p_bind_state *b = a;
        switch (b->kind) {
        case 0: { blend_st bs; memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S.blend[bs.target] = bs; break; }
        case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
        case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
        case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
        case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
        case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
        }
        break;
    }
    case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->first == 0 && b->n <= 16) { memcpy(S.attr, b->st, 4 * (size_t)b->n); S.nattr = b->n; } break; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->first == 0 && b->n <= 16) { memcpy(S.stream, b->st, 8 * (size_t)b->n); S.nstream = b->n; } break; }
    case OP_BIND_VBUF: { const p_bind_vbuf *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
    case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
    case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S.samp[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_STENCIL: {
        const p_stencil *b = a;
        for (int f = 0; f < 2; f++) if (b->face & (1 << f)) {
            if (b->which == 0) S.stencil_vmask[f] = b->value_mask; else if (b->which == 1) S.stencil_wmask[f] = b->mask; else S.stencil_ref[f] = b->ref;
        }
        break;
    }
    case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
    case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
    case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
    case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
    case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_flag *)a)->value; break;
    case OP_RASTER_DISCARD: S.raster_discard = ((const p_flag *)a)->value; break;
    default: break;
    }
}

void vd_pass_begun(void) {}

void vd_draw(const p_draw *dw, const p_draw_idx *di) {
    (void)dw; (void)di;
    if (!g_draws++) VLOG("draws are skipped until shader translation is in (first draw)");
}
void vd_dispatch(const p_dispatch *d) {
    (void)d;
    if (!g_dispatches++) VLOG("compute dispatches are skipped until shader translation is in (first dispatch)");
}
