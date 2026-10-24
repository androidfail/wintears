/* Draw path (upstream nvn_mtl.m state_op / draw_inner / encode_args / dispatch): bound state, pipelines, resources.

   Pipelines are keyed by the shader variants, the vertex input layout, the primitive class, the blend / write-mask /
   logic-op state and the pass's attachment formats; everything else is Vulkan 1.3 dynamic state, set at every draw.
   Each stage's resources live in its own descriptor set (vertex / compute 0, fragment 1: the translator moved them,
   tools/wt_xlate/VkService.cs), allocated per draw from pools that are reset when their submission completed.
   What each binding holds follows upstream encode_args: user uniforms / storage buffers by NVN slot, the driver
   segments c0 (storage-buffer table, compute grid and uniform table) and c2 (texture / sampler handles) filled from
   the bound state, the shader's own constants c1, Ryujinx's support buffer (render scale 1). */
#include "gfx/vk/vk_draw.h"
#include "gfx/vk/vk_shader.h"
#include "nvn_hmap.h"

static uint64_t g_draws, g_dispatches, g_skipped[8];
static int g_state_ready;
static uint32_t g_dispatch_grid[3];
VkDescriptorType vs_desc_type(int t);

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

/* ---------- fixed resources: support buffer, dummies ---------- */
static VkBuffer g_support, g_zero; static VmaAllocation g_support_al, g_zero_al;
static VkSampler g_dummy_samp;
static VkImageView g_dummy_view[7], g_dummy_storage[7];   /* by VkImageViewType */
static VkBufferView g_dummy_texel;
#define ZERO_SIZE 65536

static VkImageView dummy_image(VkImageViewType t, int storage) {
    VkImageView *slot = storage ? &g_dummy_storage[t] : &g_dummy_view[t];
    if (*slot) return *slot;
    int cube = t == VK_IMAGE_VIEW_TYPE_CUBE || t == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .imageType = t == VK_IMAGE_VIEW_TYPE_1D || t == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : t == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ac = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img; VmaAllocation al;
    VKC(vmaCreateImage(vk_vma, &ci, &ac, &img, &al, NULL));
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ci.arrayLayers}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(vk_pre(), &di);
    VkClearColorValue z = {0}; VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ci.arrayLayers};
    vkCmdClearColorImage(vk_pre(), img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    vk_barrier(vk_pre());
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = t, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, t == VK_IMAGE_VIEW_TYPE_CUBE || t == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 6 : 1}};
    VKC(vkCreateImageView(vk_dev, &vi, NULL, slot));
    return *slot;
}

static void fixed_init(void) {
    if (g_support) return;
    VmaAllocationCreateInfo ac = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1392, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKC(vmaCreateBuffer(vk_vma, &bi, &ac, &g_support, &g_support_al, NULL));
    bi.size = ZERO_SIZE;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VKC(vmaCreateBuffer(vk_vma, &bi, &ac, &g_zero, &g_zero_al, NULL));
    /* Ryujinx SupportBuffer (std140, 16-byte fields): alpha test 0, is_bgra 16..144, viewport inverse 144, viewport size
       160, frag_scale_count 176, render_scale[73] from 192 (all 1.0), tfe offset 1360, tfe vertex count 1376 */
    float sup[1392 / 4] = {0};
    for (int i = 0; i < 73; i++) sup[(192 + 16 * i) / 4] = 1.0f;
    vkCmdUpdateBuffer(vk_pre(), g_support, 0, sizeof sup, sup);
    vkCmdFillBuffer(vk_pre(), g_zero, 0, VK_WHOLE_SIZE, 0);
    vk_barrier(vk_pre());
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .maxLod = 1000};
    VKC(vkCreateSampler(vk_dev, &si, NULL, &g_dummy_samp));
    VkBufferViewCreateInfo bv = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = g_zero, .format = VK_FORMAT_R8G8B8A8_UNORM, .range = ZERO_SIZE};
    VKC(vkCreateBufferView(vk_dev, &bv, NULL, &g_dummy_texel));
}

/* ---------- descriptor pools (per submission) ---------- */
typedef struct dpool { VkDescriptorPool p; struct dpool *next; } dpool;
static dpool *g_dp_free, *g_dp_cur;
static pthread_mutex_t g_dp_mu = PTHREAD_MUTEX_INITIALIZER;
static void dp_return(void *arg) {
    dpool *d = arg; vkResetDescriptorPool(vk_dev, d->p, 0);
    pthread_mutex_lock(&g_dp_mu); d->next = g_dp_free; g_dp_free = d; pthread_mutex_unlock(&g_dp_mu);
}
static dpool *dp_new(void) {
    pthread_mutex_lock(&g_dp_mu); dpool *d = g_dp_free; if (d) g_dp_free = d->next; pthread_mutex_unlock(&g_dp_mu);
    if (d) return d;
    d = calloc(1, sizeof *d);
    VkDescriptorPoolSize sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192, .poolSizeCount = 8, .pPoolSizes = sz};
    VKC(vkCreateDescriptorPool(vk_dev, &ci, NULL, &d->p));
    return d;
}
static uint64_t g_dp_sub;
static VkDescriptorSet dset(VkDescriptorSetLayout l) {
    if (g_dp_sub != vk_sub_serial() || !g_dp_cur) { g_dp_cur = dp_new(); vk_after(dp_return, g_dp_cur); g_dp_sub = vk_sub_serial(); }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dp_cur->p, .descriptorSetCount = 1, .pSetLayouts = &l};
    VkDescriptorSet s;
    VkResult r = vkAllocateDescriptorSets(vk_dev, &ai, &s);
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) {
        g_dp_cur = dp_new(); vk_after(dp_return, g_dp_cur);
        ai.descriptorPool = g_dp_cur->p;
        r = vkAllocateDescriptorSets(vk_dev, &ai, &s);
    }
    if (r != VK_SUCCESS) die("Vulkan: descriptor set allocation failed (%d)", (int)r);
    return s;
}

/* ---------- resources of one stage ---------- */
static void drv_fill(uint8_t *d, int bank, int stage) {   /* upstream driver_bank_fill */
    memset(d, 0, 0xA00);
    if (bank == 0) {
        int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : stage == 1 ? 0x510 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (stage == 5) {
            memcpy(d + 0x200, g_dispatch_grid, sizeof g_dispatch_grid);
            for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); }
        } else if (stage == 0) { const float ps_max = 2047.9375f; memcpy(d + 0x610, &ps_max, sizeof ps_max); }
    } else {
        for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[stage][r], sh = (uint32_t)S.samp[stage][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh, 4); }
        for (int r = 0; r < 32; r++) { uint32_t th = (uint32_t)S.tex[stage][r]; memcpy(d + 0x20 + 8 * r, &th, 4); }
    }
}

/* a uniform buffer at a guest address: the pool buffer when aligned, else a copy of its bytes now */
/* Storage buffers the GPU writes get a video-memory copy for the submission: GPU atomics on system memory (the
   game's pools) cross PCIe one by one (one fragment shader took 45 ms). The copy is filled from pool memory when the
   submission starts and written back when it ends; every bind of those bytes in the submission uses it. */
typedef struct { uint64_t lo, hi; VkBuffer host; VkDeviceSize host_off; VkBuffer dev; VmaAllocation al; } shadow;
uint64_t g_sh_bytes;
static shadow *g_sh; static int g_nsh, g_csh; static uint64_t g_sh_sub;
static void shadow_free(void *arg) { shadow *x = arg; vmaDestroyBuffer(vk_vma, x->dev, x->al); free(x); }
static void shadows_now(void) { if (g_sh_sub != vk_sub_serial()) { g_nsh = 0; g_sh_sub = vk_sub_serial(); } }
static const shadow *shadow_of(uint64_t a, uint64_t n, int *overlap) {
    shadows_now(); *overlap = 0;
    for (int i = 0; i < g_nsh; i++) {
        if (a >= g_sh[i].lo && a + n <= g_sh[i].hi) return &g_sh[i];
        if (a < g_sh[i].hi && a + n > g_sh[i].lo) *overlap = 1;
    }
    return NULL;
}
static const shadow *shadow_make(uint64_t a, uint64_t n) {
    g_sh_bytes += n;
    VkBuffer hb; VkDeviceSize ho;
    if (!vk_buf(a, n, &hb, &ho)) return NULL;
    if (g_nsh == g_csh) { g_csh = g_csh ? g_csh * 2 : 32; g_sh = realloc(g_sh, (size_t)g_csh * sizeof *g_sh); }
    shadow *x = &g_sh[g_nsh++];
    *x = (shadow){a, a + n, hb, ho};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ac = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VKC(vmaCreateBuffer(vk_vma, &bi, &ac, &x->dev, &x->al, NULL));
    VkBufferCopy r = {ho, 0, n};
    vkCmdCopyBuffer(vk_pre(), hb, x->dev, 1, &r);
    vk_barrier(vk_pre());
    shadow *keep = malloc(sizeof *keep); *keep = *x; vk_after(shadow_free, keep);
    return x;
}
void vd_shadows_flush(void) {   /* end of the submission, outside a pass: the GPU's writes back into pool memory */
    shadows_now();
    for (int i = 0; i < g_nsh; i++) { VkBufferCopy r = {0, g_sh[i].host_off, g_sh[i].hi - g_sh[i].lo}; vkCmdCopyBuffer(vk_cmd(), g_sh[i].dev, g_sh[i].host, 1, &r); }
    if (g_nsh) vk_barrier(vk_cmd());
    g_nsh = 0;
}
/* Inline uniform updates without ending the pass (upstream's renaming, nvn_mtl.m mtl_sub_update). The game updates small
   uniform ranges between draws about 210 times a frame; each was a GPU copy that ended the pass and drained the GPU. Now
   an update is logged with its bytes, and a uniform binding that overlaps logged updates gets a private copy: memory, then
   the flushed updates the GPU has not finished, then this submission's, in order. Draws before an update read the old
   bytes, draws after it the new ones. The log reaches memory at the end of the submission, or earlier when a storage,
   vertex or index buffer or a copy touches an updated range (vd_ub_guard). */
typedef struct { uint64_t lo, hi, fence; uint8_t *data; } ub_upd;
static ub_upd *g_upd, *g_inf; static int g_nupd, g_cupd, g_ninf, g_cinf;   /* this submission's / flushed, GPU not done */
static uint64_t g_upd_lo = UINT64_MAX, g_upd_hi, g_upd_seq;
typedef struct { uint64_t lo, n, seq, sub; VkBuffer b; VkDeviceSize off; } ub_copy;
static ub_copy g_ubc[64]; static unsigned g_ubc_next;
static int ub_hit(uint64_t lo, uint64_t hi) {
    if (!g_nupd || hi <= g_upd_lo || lo >= g_upd_hi) return 0;
    for (int i = 0; i < g_nupd; i++) if (g_upd[i].lo < hi && lo < g_upd[i].hi) return 1;
    return 0;
}
static void ub_apply(const ub_upd *u, int n, uint64_t lo, uint8_t *out, uint64_t len) {
    for (int i = 0; i < n; i++) {
        uint64_t a = u[i].lo > lo ? u[i].lo : lo, b = u[i].hi < lo + len ? u[i].hi : lo + len;
        if (a < b) memcpy(out + (a - lo), u[i].data + (a - u[i].lo), (size_t)(b - a));
    }
}
/* the bytes of [lo, lo+len) as the GPU would read them at this point of the submission */
static void ub_read(uint64_t lo, uint8_t *out, uint64_t len) {
    if (pal_guarded_copy(out, (const void *)(uintptr_t)lo, (size_t)len)) memset(out, 0, (size_t)len);
    if (g_ninf) {   /* drop what the GPU has finished: memory holds it now */
        uint64_t done = vk_gpu_done(); int k = 0;
        while (k < g_ninf && g_inf[k].fence <= done) free(g_inf[k++].data);
        if (k) { memmove(g_inf, g_inf + k, (size_t)(g_ninf - k) * sizeof *g_inf); g_ninf -= k; }
        ub_apply(g_inf, g_ninf, lo, out, len);
    }
    ub_apply(g_upd, g_nupd, lo, out, len);
}
static VkDescriptorBufferInfo ub_copy_of(uint64_t lo, uint64_t n, VkDeviceSize align) {
    for (unsigned i = 0; i < 64; i++) { ub_copy *c = &g_ubc[i];
        if (c->lo == lo && c->n == n && c->seq == g_upd_seq && c->sub == vk_sub_serial() && !(c->off % align)) return (VkDescriptorBufferInfo){c->b, c->off, n}; }
    ub_copy *c = &g_ubc[g_ubc_next++ & 63];
    ub_read(lo, vk_stage(n, align, &c->b, &c->off), n);
    c->lo = lo; c->n = n; c->seq = g_upd_seq; c->sub = vk_sub_serial();
    return (VkDescriptorBufferInfo){c->b, c->off, n};
}
int vd_ub_update(uint64_t dst, const void *data, uint64_t n) {
    static int on = -1; if (on < 0) { const char *e = pal_getenv("TOTK_UB_RENAME"); on = !(e && !atoi(e)); }   /* =0: A/B only */
    if (!on) return 0;
    int ov; if (shadow_of(dst, n, &ov) || ov) return 0;   /* a storage buffer the GPU writes this submission: copied in order */
    if (g_nupd == g_cupd) { g_cupd = g_cupd ? g_cupd * 2 : 256; g_upd = realloc(g_upd, (size_t)g_cupd * sizeof *g_upd); }
    uint8_t *d = malloc((size_t)n); memcpy(d, data, (size_t)n);
    g_upd[g_nupd++] = (ub_upd){dst, dst + n, 0, d};
    if (dst < g_upd_lo) g_upd_lo = dst;
    if (dst + n > g_upd_hi) g_upd_hi = dst + n;
    g_upd_seq++;
    return 1;
}
static int ub_cmp(const void *a, const void *b) { uint64_t x = ((const ub_upd *)a)->lo, y = ((const ub_upd *)b)->lo; return x < y ? -1 : x > y; }
void vd_ub_flush(void) {   /* the logged updates into memory, on the GPU timeline */
    if (!g_nupd) return;
    vk_n_ubflush++;
    vx_end_pass();
    ub_upd *s = malloc((size_t)g_nupd * sizeof *s); memcpy(s, g_upd, (size_t)g_nupd * sizeof *s);
    qsort(s, (size_t)g_nupd, sizeof *s, ub_cmp);
    for (int i = 0; i < g_nupd;) {   /* overlapping / touching updates merged: every byte of a merged range is covered by one */
        uint64_t lo = s[i].lo, hi = s[i].hi; int j = i + 1;
        for (; j < g_nupd && s[j].lo <= hi; j++) if (s[j].hi > hi) hi = s[j].hi;
        VkBuffer db, sb; VkDeviceSize dof, so;
        if (vk_buf(lo, hi - lo, &db, &dof)) {
            uint8_t *p = vk_stage(hi - lo, 16, &sb, &so);
            ub_apply(g_upd, g_nupd, lo, p, hi - lo);   /* in log order: the newest bytes win */
            VkBufferCopy r = {so, dof, hi - lo}; vkCmdCopyBuffer(vk_cmd(), sb, db, 1, &r);
        } else for (int k = 0; k < g_nupd; k++) if (g_upd[k].lo < hi && lo < g_upd[k].hi) {   /* spans pools: written by the CPU now */
            dm_guest_write_prepare((void *)(uintptr_t)g_upd[k].lo, (size_t)(g_upd[k].hi - g_upd[k].lo));
            memcpy((void *)(uintptr_t)g_upd[k].lo, g_upd[k].data, (size_t)(g_upd[k].hi - g_upd[k].lo));
        }
        i = j;
    }
    free(s);
    vk_barrier(vk_cmd());
    uint64_t f = vk_open_fence();
    if (g_ninf + g_nupd > g_cinf) { g_cinf = (g_ninf + g_nupd) * 2; g_inf = realloc(g_inf, (size_t)g_cinf * sizeof *g_inf); }
    for (int i = 0; i < g_nupd; i++) { g_upd[i].fence = f; g_inf[g_ninf++] = g_upd[i]; }
    g_nupd = 0; g_upd_lo = UINT64_MAX; g_upd_hi = 0;
}
void vd_ub_guard(uint64_t lo, uint64_t hi) { if (ub_hit(lo, hi)) vd_ub_flush(); }

static VkDescriptorBufferInfo ubo_at(uint64_t addr, uint64_t size) {
    VkBuffer b; VkDeviceSize off;
    if (!addr || !vk_buf(addr, 1, &b, &off)) return (VkDescriptorBufferInfo){g_zero, 0, ZERO_SIZE};
    { int ov; const shadow *x = shadow_of(addr, size ? size : 256, &ov); if (x && !((addr - x->lo) % vk_props.limits.minUniformBufferOffsetAlignment))
        return (VkDescriptorBufferInfo){x->dev, addr - x->lo, (size ? size : 256) < 65536 ? (size ? size : 256) : 65536}; }
    VkDeviceSize al = vk_props.limits.minUniformBufferOffsetAlignment, range = size && size < 65536 ? size : 65536;
    if (off % al) {
        VK_ONCE("uniform buffer at %#llx is not %llu-byte aligned: its bytes are copied at record time", (unsigned long long)addr, (unsigned long long)al);
        return ub_copy_of(addr, range, al);
    }
    VkBuffer b2; VkDeviceSize o2;   /* as much of the 64 KiB the shader may read as the pool has */
    while (range > 256 && !vk_buf(addr, range, &b2, &o2)) range >>= 1;
    if (ub_hit(addr, addr + range)) return ub_copy_of(addr, range, al);
    return (VkDescriptorBufferInfo){b, off, range};
}
static VkDescriptorBufferInfo ssbo_at(uint64_t addr, uint64_t size, int rw) {
    VkBuffer b; VkDeviceSize off;
    if (!addr || !vk_buf(addr, 1, &b, &off)) return (VkDescriptorBufferInfo){g_zero, 0, ZERO_SIZE};
    { VkDeviceSize lo = off & ~(VkDeviceSize)15, range = (size ? size : 65536) + (off - lo);
      uint64_t ga = addr - (off - lo); int ov;
      if (ub_hit(ga, ga + range)) {   /* uniform bytes updated this submission, read as storage */
          if (!rw) return ub_copy_of(ga, range < vk_props.limits.maxStorageBufferRange ? range : vk_props.limits.maxStorageBufferRange, vk_props.limits.minStorageBufferOffsetAlignment);
          vd_ub_flush();
      }
      const shadow *x = shadow_of(ga, range, &ov);
      if (!x && rw && !ov) x = shadow_make(ga, range);
      if (!x && rw && ov) VK_ONCE("written storage buffer %#llx+%#llx overlaps another one in part: left in system memory", (unsigned long long)ga, (unsigned long long)range);
      if (x) return (VkDescriptorBufferInfo){x->dev, ga - x->lo, range}; }
    /* bound at the 16-byte boundary below: the translator adds the address's low bits itself (Ryujinx storage buffer alignment) */
    VkDeviceSize lo = off & ~(VkDeviceSize)15, range = (size ? size : 65536) + (off - lo);
    if (range > vk_props.limits.maxStorageBufferRange) range = vk_props.limits.maxStorageBufferRange;
    VkBuffer b2; VkDeviceSize o2;
    while (range > 16 && !vk_buf(addr - (off - lo), range, &b2, &o2)) range >>= 1;
    return (VkDescriptorBufferInfo){b, lo, range};
}

typedef struct { tex_t t; view_t v; int has_v; } slot_tex;
static int slot_texture(uint64_t handle, slot_tex *o) { return handle && vt_slot_tex((int)(handle & 0xfffff), &o->t, &o->v, &o->has_v); }

static VkDescriptorSet stage_set(const vk_shader *sh, int st) {
    VkDescriptorSet set = dset(sh->dsl);
    enum { MAXW = 96, MAXI = 512 };
    VkWriteDescriptorSet w[MAXW]; VkDescriptorBufferInfo bi[MAXI]; VkDescriptorImageInfo ii[MAXI]; VkBufferView bv[MAXI];
    int nw = 0, nb = 0, ni = 0, nv = 0;
    static char exp_key[32]; static int exp_bind = -2;   /* TOTK_EXP_DUMMY=<shader name prefix>:<binding>: that binding gets dummy data (diagnostic) */
    if (exp_bind == -2) { const char *e = pal_getenv("TOTK_EXP_DUMMY"); exp_bind = -1; if (e && strchr(e, ':')) { snprintf(exp_key, sizeof exp_key, "%.*s", (int)(strchr(e, ':') - e), e); exp_bind = atoi(strchr(e, ':') + 1); } }
    int exp_me = exp_bind >= 0 && !strncmp(sh->name, exp_key, strlen(exp_key));
    for (int i = 0; i < sh->nb && nw < MAXW; i++) {
        const vs_bind *b = &sh->b[i];
        uint32_t cnt = b->count;
        vs_bind dummy_b;
        if (exp_me && b->binding == exp_bind) { dummy_b = *b; dummy_b.kind = RB_NONE; b = &dummy_b; }
        VkWriteDescriptorSet *x = &w[nw++];
        *x = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->binding, .descriptorCount = cnt,
                                    .descriptorType = vs_desc_type(b->type)};
        if (b->type == DT_UBO || b->type == DT_SSBO) {
            if (nb + (int)cnt > MAXI) { nw--; continue; }
            VkDescriptorBufferInfo d = {g_zero, 0, ZERO_SIZE};
            switch (b->kind) {
            case RB_SUPPORT: d = (VkDescriptorBufferInfo){g_support, 0, 1392}; break;
            case RB_C1: d = (VkDescriptorBufferInfo){sh->c1_buf, sh->c1_off, 65536}; break;
            case RB_DRV0: case RB_DRV2: {
                VkBuffer sb; VkDeviceSize so;
                uint8_t *p = vk_stage(0xA00, vk_props.limits.minUniformBufferOffsetAlignment, &sb, &so);
                drv_fill(p, b->kind == RB_DRV2 ? 2 : 0, st);
                d = (VkDescriptorBufferInfo){sb, so, 0xA00};
                break;
            }
            case RB_CBUF: if (b->reg >= 0 && b->reg < 16) d = b->type == DT_UBO ? ubo_at(S.ubo[st][b->reg].addr, S.ubo[st][b->reg].size) : ssbo_at(S.ubo[st][b->reg].addr, S.ubo[st][b->reg].size, b->rw); break;
            case RB_SBUF: if (b->reg >= 0 && b->reg < 16) d = ssbo_at(S.ssbo[st][b->reg].addr, S.ssbo[st][b->reg].size, b->rw); break;
            case RB_UBOPTR: if (b->reg >= 0 && b->reg < 16) d = ssbo_at(S.ubo[st][b->reg].addr, S.ubo[st][b->reg].size, b->rw); break;
            case RB_SBUFUBO: if (b->reg >= 0 && b->reg < 16 && S.ubo[st][b->reg].addr) {
                uint64_t va = 0;   /* the shader loads this storage buffer's address from the user uniform (upstream: read at bind time) */
                if (S.ubo[st][b->reg].size >= (uint64_t)b->off + 8 && !pal_guarded_copy(&va, (const void *)(uintptr_t)(S.ubo[st][b->reg].addr + b->off), 8)) d = ssbo_at(va, 65536, b->rw);
                break;
            }
            default: break;
            }
            if (b->type == DT_UBO && d.range > 65536) d.range = 65536;
            { extern int vk_passlog_on; if (vk_passlog_on) VLOG("[pass]   st%d b%d %s kind %d reg %d -> %s off %#llx range %#llx (ssbo addr %#llx size %#llx)", st, b->binding, b->type == DT_UBO ? "ubo" : "ssbo", b->kind, b->reg, d.buffer == g_zero ? "zero" : "real",
                (unsigned long long)d.offset, (unsigned long long)d.range, b->reg >= 0 && b->reg < 16 ? (unsigned long long)S.ssbo[st][b->reg].addr : 0ull, b->reg >= 0 && b->reg < 16 ? (unsigned long long)S.ssbo[st][b->reg].size : 0ull); }
            for (uint32_t k = 0; k < cnt; k++) bi[nb + k] = d;
            x->pBufferInfo = &bi[nb]; nb += (int)cnt;
        } else if (b->type == DT_UTB || b->type == DT_STB) {
            if (nv + (int)cnt > MAXI) { nw--; continue; }
            VkBufferView v = g_dummy_texel;
            uint64_t h = b->kind == RB_TEX && b->reg >= 0 && b->reg < 128 ? S.tex[st][b->reg]
                       : b->kind == RB_IMG && b->reg >= 0 && b->reg < 32 ? S.img[st][b->reg] : 0;
            slot_tex s;
            if (h && slot_texture(h, &s) && s.t.d.target == NVN_TEXTURE_TARGET_BUFFER) {
                VkBufferView real = vt_texel_view(&s.t);
                if (!real && b->type == DT_UTB) real = vt_texel_view_copy(&s.t);
                if (real) v = real;
                else VK_ONCE("%s texture in slot %d is not GPU-readable here (fmt %#x); bound as zeros", b->type == DT_UTB ? "utb" : "stb", (int)(h & 0xfffff), s.t.d.format);
            } else if (h) { VK_ONCE("%s slot %d points at a non-buffer texture; bound as zeros", b->type == DT_UTB ? "utb" : "stb", (int)(h & 0xfffff)); }
            for (uint32_t k = 0; k < cnt; k++) bv[nv + k] = v;
            x->pTexelBufferView = &bv[nv]; nv += (int)cnt;
        } else {
            if (ni + (int)cnt > MAXI) { nw--; continue; }
            VkDescriptorImageInfo d = {g_dummy_samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            int storage = b->type == DT_SIMG;
            if (b->type != DT_SAMP) d.imageView = dummy_image((VkImageViewType)b->view, storage);
            if (b->kind == RB_TEX && b->reg >= 0 && b->reg < 128) {
                uint64_t h = S.tex[st][b->reg];
                slot_tex s;
                if (slot_texture(h, &s)) {
                    vtex *e = vt_get(&s.t);
                    VkImageView v = e ? vt_view(e, &s.t, s.has_v ? &s.v : NULL, VT_SAMPLED, (VkImageViewType)b->view) : VK_NULL_HANDLE;
                    if (v) d.imageView = v;
                    extern int vk_passlog_on;
                    if (vk_passlog_on) VLOG("[pass]     tex %ux%ux%u levels %u fmt %#x target %d flags %#x", s.t.d.width, s.t.d.height, s.t.d.depth, s.t.d.levels, s.t.d.format, s.t.d.target, s.t.d.flags);
                }
                int sid = (int)((b->sreg >= 0 && b->sreg < 128 ? S.samp[st][b->sreg] : h) >> 20) & 0xfff;
                samp_par sp; if (sid && vt_slot_samp(sid, &sp)) d.sampler = vt_sampler(&sp);
            } else if (b->kind == RB_SAMP && b->reg >= 0 && b->reg < 128) {
                int sid = (int)(S.samp[st][b->reg] >> 20) & 0xfff;
                samp_par sp; if (sid && vt_slot_samp(sid, &sp)) d.sampler = vt_sampler(&sp);
            } else if (b->kind == RB_IMG && b->reg >= 0 && b->reg < 32) {
                slot_tex s;
                if (slot_texture(S.img[st][b->reg], &s)) {
                    vtex *e = vt_get(&s.t);
                    VkImageView v = e ? vt_view(e, &s.t, s.has_v ? &s.v : NULL, VT_STORAGE, (VkImageViewType)b->view) : VK_NULL_HANDLE;
                    if (v) { d.imageView = v; vt_written(e); }
                }
            }
            extern int vk_passlog_on;
            if (vk_passlog_on) VLOG("[pass]   st%d b%d type %d kind %d reg %d sreg %d handle %#llx -> view %s sampler %s", st, b->binding, b->type, b->kind, b->reg, b->sreg,
                                    (unsigned long long)(b->kind == RB_TEX && b->reg >= 0 ? S.tex[st][b->reg] : b->kind == RB_IMG && b->reg >= 0 ? S.img[st][b->reg] : 0),
                                    d.imageView && d.imageView != dummy_image((VkImageViewType)b->view, storage) ? "real" : "dummy", d.sampler != g_dummy_samp ? "real" : "dummy");
            for (uint32_t k = 0; k < cnt; k++) ii[ni + k] = d;
            x->pImageInfo = &ii[ni]; ni += (int)cnt;
        }
    }
    vkUpdateDescriptorSets(vk_dev, (uint32_t)nw, w, 0, NULL);
    return set;
}

/* ---------- pipeline layouts and pipelines ---------- */
typedef struct { VkDescriptorSetLayout a, b; } lay_key;
static nvn_hmap g_layouts = {.klen = sizeof(lay_key)};
static VkDescriptorSetLayout g_empty_dsl;
static VkPipelineLayout layout_of(VkDescriptorSetLayout a, VkDescriptorSetLayout b) {
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKC(vkCreateDescriptorSetLayout(vk_dev, &li, NULL, &g_empty_dsl)); }
    lay_key k = {a ? a : g_empty_dsl, b};
    VkPipelineLayout l = nvn_hm_get(&g_layouts, &k);
    if (l) return l;
    VkDescriptorSetLayout sets[2] = {k.a, b ? b : g_empty_dsl};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = b ? 2 : 1, .pSetLayouts = sets};
    VKC(vkCreatePipelineLayout(vk_dev, &ci, NULL, &l));
    nvn_hm_put(&g_layouts, &k, l);
    return l;
}

typedef struct {
    const vk_shader *vs, *fs;
    VkFormat color[8], depth, stencil;
    uint32_t samples, topo_class, poly_mode, depth_clamp, logic, nattr, nstream;
    struct { uint32_t fmt, binding, offset; } attr[16];
    struct { uint32_t used, divisor; } stream[16];
    struct { uint32_t en, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a, mask; } blend[8];
    uint32_t a2c;
} gp_key;
static nvn_hmap g_pipes = {.klen = sizeof(gp_key)};
static nvn_hmap g_cpipes = {.klen = sizeof(void *)};
static uint64_t g_pipe_n, g_cpipe_n;

static VkBlendFactor bfac(unsigned f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(unsigned e) { return e >= 1 && e <= 5 ? (VkBlendOp)(e - 1) : VK_BLEND_OP_ADD; }
static uint32_t topo_class(int prim) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return 0;
    case NVN_DRAW_PRIMITIVE_LINES: case NVN_DRAW_PRIMITIVE_LINE_LOOP: case NVN_DRAW_PRIMITIVE_LINE_STRIP: return 1;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return 3;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return 4;
    case NVN_DRAW_PRIMITIVE_PATCHES: return 5;
    default: return 2;
    }
}
static VkPrimitiveTopology topo(int prim) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: case NVN_DRAW_PRIMITIVE_LINE_LOOP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   /* triangles, quads (as indexed triangles) */
    }
}

static int g_logic_op, g_divisor;
static VkPipeline graphics_pipeline(const vk_shader *vs, const vk_shader *fs, int prim, VkPipelineLayout lay) {
    gp_key k; memset(&k, 0, sizeof k);
    k.vs = vs; k.fs = fs;
    for (int i = 0; i < 8; i++) k.color[i] = i < S.pass_fmt.ncolor ? S.pass_fmt.color[i] : VK_FORMAT_UNDEFINED;
    k.depth = S.pass_fmt.depth; k.stencil = S.pass_fmt.stencil; k.samples = S.pass_samples ? S.pass_samples : 1;
    k.topo_class = topo_class(prim); k.poly_mode = S.poly.mode; k.depth_clamp = S.depth_clamp;
    k.logic = g_logic_op && S.color.logic_op != NVN_LOGIC_OP_COPY ? 0x100u | S.color.logic_op : 0;
    k.a2c = S.ms.a2c;
    for (int i = 0; i < S.nattr && i < 16; i++) {
        va_st va; memcpy(&va, &S.attr[i], 4);
        if (!va.fmt || !(vs->in_mask & (1u << i))) continue;
        k.attr[k.nattr].fmt = va.fmt; k.attr[k.nattr].binding = va.stream; k.attr[k.nattr].offset = (uint32_t)i << 24 | va.offset;
        if (va.stream < 16) { k.stream[va.stream].used = 1; k.stream[va.stream].divisor = va.stream < S.nstream ? S.stream[va.stream].divisor : 0; }
        k.nattr++;
    }
    for (int i = 0; i < 8; i++) {
        if (k.color[i] == VK_FORMAT_UNDEFINED) continue;
        const blend_st *b = &S.blend[i];
        k.blend[i].mask = (S.chmask.mask >> (4 * i)) & 0xf;
        if ((S.color.blend_en >> i) & 1) {
            k.blend[i].en = 1; k.blend[i].src_rgb = b->src_rgb; k.blend[i].dst_rgb = b->dst_rgb; k.blend[i].src_a = b->src_a;
            k.blend[i].dst_a = b->dst_a; k.blend[i].eq_rgb = b->eq_rgb; k.blend[i].eq_a = b->eq_a;
        }
    }
    VkPipeline p = nvn_hm_get(&g_pipes, &k);
    if (p) return p;

    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs->mod, .pName = "main"}};
    int nst = 1;
    if (fs) st[nst++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs->mod, .pName = "main"};
    VkVertexInputAttributeDescription va[16]; VkVertexInputBindingDescription vb[16]; VkVertexInputBindingDivisorDescriptionEXT vd[16];
    uint32_t nva = 0, nvb = 0, nvd = 0;
    for (uint32_t i = 0; i < k.nattr; i++) {
        VkFormat f = vk_vfmt((int)k.attr[i].fmt);
        if (f == VK_FORMAT_UNDEFINED) { VK_ONCE("vertex attribute format %#x has no Vulkan format: left out", k.attr[i].fmt); continue; }
        va[nva++] = (VkVertexInputAttributeDescription){k.attr[i].offset >> 24, k.attr[i].binding, f, k.attr[i].offset & 0xffffff};
    }
    for (uint32_t s = 0; s < 16; s++) if (k.stream[s].used) {
        uint32_t div = k.stream[s].divisor;
        vb[nvb++] = (VkVertexInputBindingDescription){s, 0, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (div > 1 && g_divisor) vd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, div};
        else if (div > 1) VK_ONCE("instance divisor %u: the GPU has no divisor support, every instance steps", div);
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vds = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nvd ? &vds : NULL,
        .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = vb, .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo(prim)};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k.depth_clamp ? VK_TRUE : VK_FALSE,
        .polygonMode = k.poly_mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : k.poly_mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)k.samples,
                                               .alphaToCoverageEnable = k.a2c ? VK_TRUE : VK_FALSE};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb[8]; uint32_t ncb = 0;
    for (int i = 0; i < 8 && i < S.pass_fmt.ncolor; i++) {
        cb[i] = (VkPipelineColorBlendAttachmentState){k.blend[i].en, bfac(k.blend[i].src_rgb), bfac(k.blend[i].dst_rgb), bop(k.blend[i].eq_rgb),
                                                      bfac(k.blend[i].src_a), bfac(k.blend[i].dst_a), bop(k.blend[i].eq_a), k.blend[i].mask};
        ncb = (uint32_t)i + 1;
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .logicOpEnable = k.logic ? VK_TRUE : VK_FALSE,
        .logicOp = (VkLogicOp)(k.logic & 0xf), .attachmentCount = ncb, .pAttachments = cb};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = (uint32_t)S.pass_fmt.ncolor,
        .pColorAttachmentFormats = k.color, .depthAttachmentFormat = k.depth, .stencilAttachmentFormat = k.stencil};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = (uint32_t)nst, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = lay};
    uint64_t t0 = pal_now_ns();
    VkResult r = vkCreateGraphicsPipelines(vk_dev, vk_pcache, 1, &ci, NULL, &p);
    if (r != VK_SUCCESS) { VLOG("pipeline for %s + %s not created (%d)", vs->name, fs ? fs->name : "-", (int)r); p = VK_NULL_HANDLE; }
    vk_ns_pipe += pal_now_ns() - t0;
    nvn_hm_put(&g_pipes, &k, p ? (void *)p : (void *)1);   /* 1 = failed: not tried again */
    if (++g_pipe_n <= 4 || g_pipe_n % 500 == 0) VLOG("%llu pipelines (last one %.1f ms)", (unsigned long long)g_pipe_n, (pal_now_ns() - t0) / 1e6);
    return p;
}

/* ---------- draws ---------- */

static VkCompareOp cmp(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp sop(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }

static void dynamic_state(VkCommandBuffer c, int prim) {
    VkViewport v = {0, 0, (float)S.pass_w, (float)S.pass_h, S.dr[0], S.dr[1]};
    if (S.has_vp) { v.x = S.vp[0][0]; v.y = S.vp[0][1]; v.width = S.vp[0][2]; v.height = S.vp[0][3]; }
    if (v.width <= 0) v.width = 1;
    if (v.height == 0) v.height = 1;
    v.minDepth = v.minDepth < 0 ? 0 : v.minDepth > 1 ? 1 : v.minDepth; v.maxDepth = v.maxDepth < 0 ? 0 : v.maxDepth > 1 ? 1 : v.maxDepth;
    /* NVN (as Metal) puts NDC y = +1 at the top row; Vulkan puts it at the bottom: flip with a negative height */
    v.y += v.height; v.height = -v.height;
    vkCmdSetViewportWithCount(c, 1, &v);
    VkRect2D sc = {{0, 0}, {S.pass_w, S.pass_h}};
    if (S.has_sc) {
        int64_t x0 = S.sc.x > 0 ? S.sc.x : 0, y0 = S.sc.y > 0 ? S.sc.y : 0, x1 = (int64_t)S.sc.x + S.sc.w, y1 = (int64_t)S.sc.y + S.sc.h;
        if (x1 > S.pass_w) x1 = S.pass_w;
        if (y1 > S.pass_h) y1 = S.pass_h;
        sc = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {x1 > x0 ? (uint32_t)(x1 - x0) : 0, y1 > y0 ? (uint32_t)(y1 - y0) : 0}};
    }
    vkCmdSetScissorWithCount(c, 1, &sc);
    vkCmdSetCullMode(c, S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : S.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE);
    /* the flipped viewport keeps GL / NVN facing */
    vkCmdSetFrontFace(c, S.poly.front == NVN_FRONT_FACE_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(c, topo(prim));
    int has_d = S.pass_fmt.depth != VK_FORMAT_UNDEFINED, has_s = S.pass_fmt.stencil != VK_FORMAT_UNDEFINED;
    vkCmdSetDepthTestEnable(c, has_d && S.ds.dt);
    vkCmdSetDepthWriteEnable(c, has_d && S.ds.dw);
    vkCmdSetDepthCompareOp(c, cmp(S.ds.df));
    vkCmdSetDepthBoundsTestEnable(c, VK_FALSE);
    vkCmdSetStencilTestEnable(c, has_s && S.ds.st);
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_BIT, sop(S.ds.fail_f), sop(S.ds.zpass_f), sop(S.ds.zfail_f), cmp(S.ds.sf_f));
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, sop(S.ds.fail_b), sop(S.ds.zpass_b), sop(S.ds.zfail_b), cmp(S.ds.sf_b));
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.stencil_vmask[0]); vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.stencil_vmask[1]);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.stencil_wmask[0]); vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.stencil_wmask[1]);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.stencil_ref[0]); vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.stencil_ref[1]);
    vkCmdSetRasterizerDiscardEnable(c, S.raster_discard ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBiasEnable(c, S.poly.offset_en ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBias(c, S.po.units, S.po.clamp, S.po.factor);
    vkCmdSetBlendConstants(c, S.blend_color);
    vkCmdSetPrimitiveRestartEnable(c, VK_FALSE);
    vkCmdSetLineWidth(c, 1.0f);
}

static void vertex_buffers(VkCommandBuffer c, const vk_shader *vs) {
    VkBuffer b[16]; VkDeviceSize o[16], sz[16], stride[16];
    uint32_t used = 0;
    for (int i = 0; i < S.nattr && i < 16; i++) { va_st va; memcpy(&va, &S.attr[i], 4); if (va.fmt && (vs->in_mask & (1u << i)) && va.stream < 16) used |= 1u << va.stream; }
    for (uint32_t s = 0; s < 16; s++) {
        if (!(used >> s & 1)) continue;
        VkBuffer pb; VkDeviceSize po;
        uint64_t addr = S.vbuf[s].address, size = S.vbuf[s].size;
        if (addr && vk_buf(addr, 1, &pb, &po)) { b[s] = pb; o[s] = po; sz[s] = VK_WHOLE_SIZE; (void)size; }
        else { b[s] = g_zero; o[s] = 0; sz[s] = VK_WHOLE_SIZE; }
        stride[s] = s < (uint32_t)S.nstream ? S.stream[s].stride : 0;
        vkCmdBindVertexBuffers2(c, s, 1, &b[s], &o[s], &sz[s], &stride[s]);
    }
}

/* quads -> triangles (0 1 2, 0 2 3 per quad): indices into staging memory */
static int quad_indices(VkCommandBuffer c, const uint8_t *src, int itype, int first, int count, int base_vertex, uint32_t *out_count) {
    uint32_t nq = (uint32_t)count / 4, n = nq * 6;
    if (!n) return 0;
    VkBuffer sb; VkDeviceSize so;
    uint32_t *d = vk_stage((VkDeviceSize)n * 4, 4, &sb, &so);
    for (uint32_t q = 0; q < nq; q++) {
        uint32_t v[4];
        for (int k = 0; k < 4; k++) {
            uint32_t i = q * 4 + (uint32_t)k;
            v[k] = !src ? (uint32_t)first + i : itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? src[i] : itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
        }
        uint32_t *t = d + q * 6; t[0] = v[0]; t[1] = v[1]; t[2] = v[2]; t[3] = v[0]; t[4] = v[2]; t[5] = v[3];
    }
    vkCmdBindIndexBuffer(c, sb, so, VK_INDEX_TYPE_UINT32);
    (void)base_vertex;
    *out_count = n;
    return 1;
}

static void draw_inner(VkCommandBuffer c, const p_draw *dw, const p_draw_idx *di, int prim, int quads);
void vd_draw(const p_draw *dw, const p_draw_idx *di) {
    g_draws++;
    if (!g_state_ready) state_reset();
    fixed_init();
    vk_prog *pr = vs_prog(S.prog);
    if (!pr || !vs_has_stage(pr, 0)) { g_skipped[0]++; return; }
    vk_shader *vs = vs_stage(pr, 0), *fs = vs_has_stage(pr, 1) ? vs_stage(pr, 1) : NULL;
    if (!vs || (vs_has_stage(pr, 1) && !fs)) { g_skipped[1]++; return; }
    if (g_nupd) {   /* vertex / index buffers read memory: logged uniform updates there reach it first */
        for (int i = 0; i < S.nattr && i < 16; i++) { va_st va; memcpy(&va, &S.attr[i], 4);
            if (va.fmt && va.stream < 16 && S.vbuf[va.stream].address) vd_ub_guard(S.vbuf[va.stream].address, S.vbuf[va.stream].address + (S.vbuf[va.stream].size ? S.vbuf[va.stream].size : 1)); }
        if (di) vd_ub_guard(di->ibuf, di->ibuf + (uint64_t)di->count * 4);
    }
    /* the descriptor sets first: they may upload textures (into this submission's upload buffer, before the pass) */
    VkDescriptorSet s0 = stage_set(vs, 0), s1 = fs ? stage_set(fs, 1) : VK_NULL_HANDLE;
    if (!vx_begin_pass()) { g_skipped[2]++; return; }
    int prim = dw ? dw->prim : di->prim;
    VkPipelineLayout lay = layout_of(vs->dsl, fs ? fs->dsl : VK_NULL_HANDLE);
    VkPipeline p = graphics_pipeline(vs, fs, prim, lay);
    if (!p || p == (VkPipeline)1) { g_skipped[3]++; return; }
    void vx_note_draw(void); vx_note_draw();
    { extern int vk_passlog_on; if (vk_passlog_on) VLOG("[pass]  draw %s + %s prim %d vp %g,%g %gx%g sc %d %d,%d %dx%d cull %d dt %d blend_en %#x mask %#x raster_discard %d",
        vs->name, fs ? fs->name : "-", prim, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.has_sc, S.sc.x, S.sc.y, S.sc.w, S.sc.h, S.poly.cull, S.ds.dt, S.color.blend_en, S.chmask.mask, S.raster_discard); }
    VkCommandBuffer c = vk_cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkDescriptorSet sets[2] = {s0, s1};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, fs ? 2 : 1, sets, 0, NULL);
    dynamic_state(c, prim);
    vertex_buffers(c, vs);
    int quads = prim == NVN_DRAW_PRIMITIVE_QUADS;
    uint32_t vx_draw_time_begin(void); void vx_draw_time_end(uint32_t q, const char *what);
    uint32_t tq = vx_draw_time_begin();
    char what[160]; if (tq != UINT32_MAX) snprintf(what, sizeof what, "%.16s + %.16s", vs->name, fs ? fs->name : "-");
    draw_inner(c, dw, di, prim, quads);
    vx_draw_time_end(tq, what);
}
static void draw_inner(VkCommandBuffer c, const p_draw *dw, const p_draw_idx *di, int prim, int quads) {
    (void)prim;
    if (dw) {
        uint32_t inst = dw->instances > 0 ? (uint32_t)dw->instances : 1;
        if (quads) { uint32_t n; if (quad_indices(c, NULL, 0, dw->first, dw->count, 0, &n)) vkCmdDrawIndexed(c, n, inst, 0, 0, (uint32_t)dw->base_instance); return; }
        vkCmdDraw(c, (uint32_t)dw->count, inst, (uint32_t)dw->first, (uint32_t)dw->base_instance);
        return;
    }
    uint32_t inst = di->instances > 0 ? (uint32_t)di->instances : 1;
    unsigned isz = di->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? 1 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4;
    VkBuffer ib; VkDeviceSize io;
    if (quads || isz == 1) {
        const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
        if (quads) { uint32_t n; if (quad_indices(c, src, di->itype, 0, di->count, di->base_vertex, &n)) vkCmdDrawIndexed(c, n, inst, 0, di->base_vertex, (uint32_t)di->base_instance); return; }
        VkBuffer sb; VkDeviceSize so;   /* 8-bit indices widened to 16 bits */
        uint16_t *d = vk_stage((VkDeviceSize)di->count * 2, 4, &sb, &so);
        for (int i = 0; i < di->count; i++) d[i] = src[i];
        vkCmdBindIndexBuffer(c, sb, so, VK_INDEX_TYPE_UINT16);
    } else {
        if (!vk_buf(di->ibuf, (uint64_t)di->count * isz, &ib, &io) || io % isz) { g_skipped[4]++; VK_ONCE("index buffer %#llx not in GPU memory / unaligned: draw skipped", (unsigned long long)di->ibuf); return; }
        vkCmdBindIndexBuffer(c, ib, io, isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
    vkCmdDrawIndexed(c, (uint32_t)di->count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
    if (g_draws % 200000 == 0) VLOG("%llu draws, skipped: no program %llu, no shader %llu, no target %llu, no pipeline %llu, index %llu",
        (unsigned long long)g_draws, (unsigned long long)g_skipped[0], (unsigned long long)g_skipped[1], (unsigned long long)g_skipped[2],
        (unsigned long long)g_skipped[3], (unsigned long long)g_skipped[4]);
}

void vd_dispatch(const p_dispatch *d) {
    g_dispatches++;
    if (!g_state_ready) state_reset();
    fixed_init();
    vk_prog *pr = vs_prog(S.prog);
    if (!pr || !vs_has_stage(pr, 5)) { g_skipped[5]++; return; }
    g_dispatch_grid[0] = (uint32_t)d->x; g_dispatch_grid[1] = (uint32_t)d->y; g_dispatch_grid[2] = (uint32_t)d->z;
    vk_shader *cs = vs_stage(pr, 5);
    if (!cs) { g_skipped[6]++; return; }
    vx_end_pass();
    VkDescriptorSet s0 = stage_set(cs, 5);
    VkPipelineLayout lay = layout_of(cs->dsl, VK_NULL_HANDLE);
    VkPipeline p = nvn_hm_get(&g_cpipes, &cs);
    if (!p) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs->mod, .pName = "main"}, .layout = lay};
        uint64_t t0 = pal_now_ns();
        VkResult r = vkCreateComputePipelines(vk_dev, vk_pcache, 1, &ci, NULL, &p); g_cpipe_n++;
        vk_ns_pipe += pal_now_ns() - t0;
        if (r != VK_SUCCESS) { VLOG("compute pipeline for %s not created (%d)", cs->name, (int)r); p = (VkPipeline)1; }
        nvn_hm_put(&g_cpipes, &cs, (void *)p);
    }
    if (p == (VkPipeline)1) { g_skipped[7]++; return; }
    VkCommandBuffer c = vk_cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &s0, 0, NULL);
    vkCmdDispatch(c, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vk_barrier(c);
}

/* ---------- pipeline cache ----------
   Kept in <profile>/pipeline-cache.bin between runs (the driver ignores data from another driver version or GPU), so a
   pipeline made in an earlier run costs a lookup. Saved from the 10 s log tick when new pipelines were made, at most once a minute. */
VkPipelineCache vk_pcache;
static char g_pc_path[1100]; static uint64_t g_pc_seen, g_pc_last;
static void pcache_init(void) {
    snprintf(g_pc_path, sizeof g_pc_path, "%s/pipeline-cache.bin", host_profile_dir());
    void *data = NULL; int64_t size = 0;
    pal_file *f = pal_file_open(g_pc_path, PAL_FILE_READ);
    if (f) {
        size = pal_file_size(f); data = size > 0 ? malloc((size_t)size) : NULL;
        if (data && pal_file_pread(f, data, (size_t)size, 0) != size) { free(data); data = NULL; size = 0; }
        pal_file_close(f);
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = data ? (size_t)size : 0, .pInitialData = data};
    if (vkCreatePipelineCache(vk_dev, &ci, NULL, &vk_pcache) != VK_SUCCESS) { vk_pcache = VK_NULL_HANDLE; VLOG("pipeline cache not created"); }
    else if (data) VLOG("pipeline cache: %.1f MiB read", size / 1048576.0);
    free(data);
}
void vd_pcache_save(void) {
    uint64_t made = g_pipe_n + g_cpipe_n, now = pal_now_ns();
    if (!vk_pcache || made == g_pc_seen || now - g_pc_last < 60000000000ull) return;
    g_pc_last = now; g_pc_seen = made;
    size_t n = 0;
    if (vkGetPipelineCacheData(vk_dev, vk_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n);
    if (data && vkGetPipelineCacheData(vk_dev, vk_pcache, &n, data) == VK_SUCCESS) {
        char tmp[1110]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pc_path);
        pal_file *f = pal_file_open(tmp, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE);
        int ok = f && pal_file_pwrite(f, data, n, 0) == (int64_t)n;
        if (f) pal_file_close(f);
        if (ok && pal_rename_replace(tmp, g_pc_path)) VLOG("pipeline cache: could not replace %s", g_pc_path);
    }
    free(data);
}

void vd_init(void) {
    pcache_init();
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(vk_phys, &f);
    g_logic_op = f.logicOp;
    g_divisor = vk_divisor;
}
