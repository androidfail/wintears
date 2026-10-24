/* Replay of the NVN recordings into Vulkan (upstream nvn_mtl.m mtl_exec and its helpers).

   Render passes are dynamic rendering: SetRenderTargets ends the pass in progress, the next clear or draw begins one
   on the new targets (load and store everything; images are in GENERAL layout). Clears inside a pass are
   vkCmdClearAttachments within the scissor, as NVN clears are. Copies, uniform updates and texture clears end the pass
   first. What the CPU sees of the GPU (event values, counter reports) happens on the completion thread once the
   submission is done, in recording order. The state (targets, bindings) carries over between submissions, as on the
   GPU's one queue. */
#include "gfx/vk/vk.h"
#include "vk_draw.h"

vx_state S;
static int g_pass;                  /* a render pass is open */
static VkRect2D g_area;             /* its render area */
static uint32_t g_pass_layers = 1;  /* its layers: every attachment view covers at least this many (a layered target; the vertex shader picks the layer) */
static uint64_t g_stat_skip[8];

/* ---------- CPU-visible effects ---------- */
typedef struct { volatile uint32_t *val; uint32_t mode, value; } ev_act;
static void do_event(void *arg) {
    ev_act *e = arg;
    if (e->mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += e->value; else *e->val = e->value;
    free(e);
}
/* Samples passed (occlusion queries). NVN: a counter on the queue; ResetCounter zeroes it, every draw after adds to it,
   ReportCounter writes the value at that point. Here every pass opened while counting has one occlusion query (a slot of
   a ring); a reset starts a new list of slots (g_oq_start), a report ends the open pass and takes the list [from, to);
   when the submission has completed the slots are summed. TOTK_OCCLUSION=0: every query says "visible" (diagnosis). */
enum { OQ_N = 16384 };
static VkQueryPool g_oq; static uint64_t g_oq_n, g_oq_start; static int g_oq_on, g_oq_open, g_oq_env = -1;
static int oq_enabled(void) {
    if (g_oq_env < 0) { const char *e = pal_getenv("TOTK_OCCLUSION"); g_oq_env = !(e && *e == '0'); }
    return g_oq_env;
}
static int oq_make(void) {
    if (g_oq) return 1;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = OQ_N};
    if (vkCreateQueryPool(vk_dev, &qi, NULL, &g_oq) != VK_SUCCESS) { g_oq = VK_NULL_HANDLE; g_oq_env = 0; VLOG("occlusion queries: no query pool, every query says visible"); return 0; }
    return 1;
}
typedef struct { uint64_t type, addr, from, to; int real; } ctr_act;
static uint64_t oq_sum(uint64_t from, uint64_t to) {
    uint64_t n = to - from, sum = 0;
    if (n > OQ_N) { from = to - OQ_N; n = OQ_N; }
    uint64_t *r = malloc(n * sizeof *r);
    for (uint64_t done = 0; r && done < n;) {   /* the ring wraps: at most two runs */
        uint32_t s = (uint32_t)((from + done) % OQ_N); uint64_t k = n - done < OQ_N - s ? n - done : OQ_N - s;
        if (vkGetQueryPoolResults(vk_dev, g_oq, s, (uint32_t)k, k * 8, r + done, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) break;
        done += k;
    }
    for (uint64_t i = 0; r && i < n; i++) sum += r[i];
    free(r);
    return sum;
}
static void do_counter(void *arg) {
    ctr_act *c = arg;
    /* NVNcounterData = {counter, timestamp}. Without counting on the GPU (never reset, or TOTK_OCCLUSION=0): "visible". */
    uint64_t rec[2] = {c->real ? oq_sum(c->from, c->to) : c->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1u << 20 : 0, vk_ticks()};
    dm_guest_write_prepare((void *)(uintptr_t)c->addr, sizeof rec);
    memcpy((void *)(uintptr_t)c->addr, rec, sizeof rec);
    free(c);
}

/* ---------- TOTK_PASSLOG=<present>: one line per pass and copy of the submissions between that present and the next */
static int g_plog = -1; int vk_passlog_on;
uint64_t vk_present_count(void);
static int plog(void) {
    if (g_plog < 0) { const char *e = pal_getenv("TOTK_PASSLOG"); g_plog = e ? atoi(e) : 0; }
    return vk_passlog_on = g_plog && vk_present_count() == (uint64_t)g_plog;
}
static int g_pass_draws, g_pass_clears;
/* GPU time of each pass of the logged frame (timestamps) */
static VkQueryPool g_qp; static uint32_t g_q;
typedef struct { uint32_t q; char what[160]; } ptime;
static void ptime_log(void *arg) {
    ptime *t = arg; uint64_t ts[2];
    if (vkGetQueryPoolResults(vk_dev, g_qp, t->q, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        VLOG("[pass] %.3f ms GPU: %s", (double)(ts[1] - ts[0]) * vk_props.limits.timestampPeriod / 1e6, t->what);
    free(t);
}
static uint32_t ptime_begin(void) {
    if (!g_qp) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8192};
        VKC(vkCreateQueryPool(vk_dev, &qi, NULL, &g_qp));
        vkResetQueryPool(vk_dev, g_qp, 0, 8192);
    }
    if (g_q + 2 > 8192) return UINT32_MAX;
    uint32_t q = g_q; g_q += 2;
    vkCmdWriteTimestamp2(vk_cmd(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_qp, q);
    return q;
}
static uint32_t g_pass_q = UINT32_MAX;
/* per draw in the logged frame */
uint32_t vx_draw_time_begin(void) { return vk_passlog_on ? ptime_begin() : UINT32_MAX; }
void vx_draw_time_end(uint32_t q, const char *what) {
    if (q == UINT32_MAX) return;
    vkCmdWriteTimestamp2(vk_cmd(), VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_qp, q + 1);
    ptime *t = malloc(sizeof *t); t->q = q; snprintf(t->what, sizeof t->what, "draw %s", what);
    vk_after(ptime_log, t);
}
void vx_note_draw(void) { g_pass_draws++; }

/* ---------- render passes ---------- */
void vx_end_pass(void) {
    if (!g_pass) return;
    if (plog()) VLOG("[pass] %ux%u colors %d (%d %d) depth %d: %d draws, %d clears", S.pass_w, S.pass_h, S.pass_fmt.ncolor, (int)S.pass_fmt.color[0], (int)S.pass_fmt.color[1],
                     (int)S.pass_fmt.depth, g_pass_draws, g_pass_clears);
    if (g_oq_open) { vkCmdEndQuery(vk_cmd(), g_oq, (uint32_t)((g_oq_n - 1) % OQ_N)); g_oq_open = 0; }
    vkCmdEndRendering(vk_cmd());
    if (g_pass_q != UINT32_MAX) {
        vkCmdWriteTimestamp2(vk_cmd(), VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_qp, g_pass_q + 1);
        ptime *t = malloc(sizeof *t); t->q = g_pass_q;
        snprintf(t->what, sizeof t->what, "%ux%u colors %d (%d) depth %d, %d draws", S.pass_w, S.pass_h, S.pass_fmt.ncolor, (int)S.pass_fmt.color[0], (int)S.pass_fmt.depth, g_pass_draws);
        vk_after(ptime_log, t); g_pass_q = UINT32_MAX;
    }
    g_pass_draws = g_pass_clears = 0;
    vk_barrier(vk_cmd());
    g_pass = 0;
}

/* The targets' images and views, prepared before the pass begins (uploads cannot happen inside one). */
/* the layers an attachment view covers (as vt_view makes it): its count, else everything from its first layer on, never more than the image has */
static uint32_t view_layers(const view_t *v, uint32_t total) {
    uint32_t first = v && v->set_layers ? v->min_layer : 0;
    if (first >= total) first = total - 1;
    uint32_t n = v && v->set_layers && v->nlayers ? v->nlayers : total - first;
    return n > total - first ? total - first : n;
}
static int targets(vtex **col, VkImageView *cv, vtex **dep, VkImageView *dv, uint32_t *w, uint32_t *h, uint32_t *layers) {
    *w = *h = UINT32_MAX; *layers = UINT32_MAX; int any = 0;
    for (int i = 0; i < 8; i++) {
        col[i] = NULL; cv[i] = VK_NULL_HANDLE;
        if (i >= S.rt.n || !S.rt.color[i]) continue;
        const view_t *v = S.rt.has_cview[i] ? &S.rt.cview[i] : NULL;
        if (!(col[i] = vt_get(S.rt.color[i]))) continue;
        vt_note_rendered(S.rt.color[i]);
        if (!(cv[i] = vt_view(col[i], S.rt.color[i], v, VT_ATTACHMENT, VT_ANY_TYPE))) { col[i] = NULL; continue; }
        unsigned lv = v && v->set_levels ? v->base_level : 0; uint32_t a, b, c; vt_size(col[i], lv, &a, &b, &c);
        uint32_t nl = view_layers(v, vt_layers(col[i]));
        *w = a < *w ? a : *w; *h = b < *h ? b : *h; *layers = nl < *layers ? nl : *layers;
        any = 1;
    }
    *dep = NULL; *dv = VK_NULL_HANDLE;
    if (S.rt.depth && (*dep = vt_get(S.rt.depth))) {
        const view_t *v = S.rt.has_dview ? &S.rt.dview : NULL;
        if ((*dv = vt_view(*dep, S.rt.depth, v, VT_ATTACHMENT, VT_ANY_TYPE))) {
            unsigned lv = v && v->set_levels ? v->base_level : 0; uint32_t a, b, c; vt_size(*dep, lv, &a, &b, &c);
            uint32_t nl = view_layers(v, vt_layers(*dep));
            *w = a < *w ? a : *w; *h = b < *h ? b : *h; *layers = nl < *layers ? nl : *layers;
            any = 1;
        } else *dep = NULL;
    }
    if (*layers == UINT32_MAX) *layers = 1;
    return any;
}

/* Open a pass on the current targets; 0 when there is nothing to render into. */
static int begin_pass(void) {
    if (g_pass) return 1;
    vtex *col[8], *dep; VkImageView cv[8], dv; uint32_t w, h, layers;
    if (!S.has_rt || !targets(col, cv, &dep, &dv, &w, &h, &layers)) return 0;
    VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa;
    int n = S.rt.n < 8 ? S.rt.n : 8;
    for (int i = 0; i < n; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = cv[i], .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (col[i]) vt_written(col[i]);
    }
    int has_d = dep && (vt_aspect(dep) & VK_IMAGE_ASPECT_DEPTH_BIT), has_s = dep && (vt_aspect(dep) & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (dep) {
        vt_written(dep);
        da.imageView = dv; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    sa = da;
    g_area = (VkRect2D){{0, 0}, {w, h}}; g_pass_layers = layers;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = g_area, .layerCount = layers,
                          .colorAttachmentCount = (uint32_t)n, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    S.pass_fmt.ncolor = n;
    for (int i = 0; i < 8; i++) S.pass_fmt.color[i] = i < n && col[i] ? vt_vkformat(col[i]) : VK_FORMAT_UNDEFINED;
    S.pass_fmt.depth = has_d ? vt_vkformat(dep) : VK_FORMAT_UNDEFINED;
    S.pass_fmt.stencil = has_s ? vt_vkformat(dep) : VK_FORMAT_UNDEFINED;
    S.pass_w = w; S.pass_h = h; S.pass_samples = 1;
    for (int i = 0; i < n; i++) if (col[i]) { S.pass_samples = vt_samples(col[i]); break; }
    if (dep && S.pass_samples == 1) S.pass_samples = vt_samples(dep);
    if (vk_passlog_on) g_pass_q = ptime_begin();
    vk_n_pass++;
    vkCmdBeginRendering(vk_cmd(), &ri);
    if (g_oq_on) {   /* counting samples: this pass has its own query */
        uint32_t q = (uint32_t)(g_oq_n++ % OQ_N);
        vkResetQueryPool(vk_dev, g_oq, q, 1);
        vkCmdBeginQuery(vk_cmd(), g_oq, q, 0); g_oq_open = 1;
    }
    g_pass = 1;
    return 1;
}
int vx_begin_pass(void) { return begin_pass(); }

/* The scissor rectangle within the pass area (NVN clears honour the scissor). */
static VkRect2D clear_rect(void) {
    VkRect2D r = g_area;
    if (S.has_sc) {
        int64_t x0 = S.sc.x > 0 ? S.sc.x : 0, y0 = S.sc.y > 0 ? S.sc.y : 0;
        int64_t x1 = (int64_t)S.sc.x + S.sc.w, y1 = (int64_t)S.sc.y + S.sc.h;
        if (x1 > g_area.extent.width) x1 = g_area.extent.width;
        if (y1 > g_area.extent.height) y1 = g_area.extent.height;
        r = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {x1 > x0 ? (uint32_t)(x1 - x0) : 0, y1 > y0 ? (uint32_t)(y1 - y0) : 0}};
    }
    return r;
}

static void clear_color(const p_clear_color *c) {
    if (S.render_disabled) return;
    if (c->index < 0 || c->index >= S.rt.n || !S.rt.color[c->index] || !(c->mask & 0xf)) return;
    if (!begin_pass()) return;
    if (S.pass_fmt.color[c->index] == VK_FORMAT_UNDEFINED) return;
    if ((c->mask & 0xf) != 0xf) {   /* only some channels: drawn with a write mask */
        uint32_t bits[4]; memcpy(bits, c->rgba, 16);
        if (vx_clear_masked_color(c->index, c->mask & 0xf, bits, clear_rect(), g_pass_layers)) { g_pass_clears++; return; }
        g_stat_skip[0]++; VK_ONCE("colour clear with channel mask %#x: the masked draw failed, done as a full clear", c->mask);
    }
    g_pass_clears++;
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index};
    memcpy(ca.clearValue.color.float32, c->rgba, 16);   /* integer targets: NVN passes the integer bits the same way */
    VkClearRect r = {clear_rect(), 0, g_pass_layers};   /* a clear covers every layer of a layered target */
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(vk_cmd(), 1, &ca, 1, &r);
}
static void clear_ds(const p_clear_ds *c) {
    if (S.render_disabled || !S.rt.depth) return;
    if (!begin_pass()) return;
    VkImageAspectFlags asp = 0;
    uint32_t smask = c->stencil_mask & 0xff;
    if (c->depth_mask && S.pass_fmt.depth != VK_FORMAT_UNDEFINED) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (smask && S.pass_fmt.stencil != VK_FORMAT_UNDEFINED) {
        if (smask == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else if (vx_clear_masked_stencil(smask, (uint32_t)c->stencil, clear_rect(), g_pass_layers)) smask = 0;   /* some bits: drawn with a write mask */
        else { asp |= VK_IMAGE_ASPECT_STENCIL_BIT; VK_ONCE("stencil clear with write mask %#x: the masked draw failed, done as a full clear", c->stencil_mask); }
    }
    if (!asp) return;
    VkClearAttachment ca = {asp, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect r = {clear_rect(), 0, g_pass_layers};   /* a clear covers every layer of a layered target */
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(vk_cmd(), 1, &ca, 1, &r);
}

/* ---------- copies ---------- */
static void copy_b2b(uint64_t src, uint64_t dst, uint64_t n) {
    VkBuffer sb, db; VkDeviceSize so, dof;
    if (!n) return;
    vd_ub_guard(src, src + n); vd_ub_guard(dst, dst + n);
    if (!vk_buf(src, n, &sb, &so) || !vk_buf(dst, n, &db, &dof) || (sb == db && src < dst + n && dst < src + n)) {
        /* not both GPU-visible, or overlapping: done by the CPU at submit, as the null backend does */
        VK_ONCE("buffer copy %#llx -> %#llx (%llu bytes) outside GPU memory or overlapping: copied by the CPU", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
        dm_guest_write_prepare((void *)(uintptr_t)dst, (size_t)n);
        memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
        return;
    }
    vx_end_pass();
    VkBufferCopy r = {so, dof, n};
    vkCmdCopyBuffer(vk_cmd(), sb, db, 1, &r);
    vk_barrier(vk_cmd());
}
static void update_ub(uint64_t dst, const uint8_t *data, uint64_t n) {
    VkBuffer db; VkDeviceSize off;
    if (!n) return;
    if (!vk_buf(dst, n, &db, &off) || (off & 3) || (n & 3)) {
        VK_ONCE("uniform update at %#llx (%llu bytes) outside GPU memory or unaligned: written by the CPU", (unsigned long long)dst, (unsigned long long)n);
        dm_guest_write_prepare((void *)(uintptr_t)dst, (size_t)n);
        memcpy((void *)(uintptr_t)dst, data, (size_t)n);
        return;
    }
    vk_n_ubo++;
    if (vd_ub_update(dst, data, n)) return;
    vd_ub_flush();   /* earlier logged updates first: the order of writes is kept */
    vx_end_pass();
    for (uint64_t at = 0; at < n; at += 65536) vkCmdUpdateBuffer(vk_cmd(), db, off + at, n - at > 65536 ? 65536 : n - at, data + at);
    vk_barrier(vk_cmd());
}

static unsigned view_level(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
static unsigned view_layer(const view_t *v, int has) { return has && v->set_layers ? v->min_layer : 0; }
static VkImageAspectFlags copy_aspect(const vtex *e) { VkImageAspectFlags a = vt_aspect(e); return a == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : a; }
/* region -> subresource layers + offset / extent (arrays: z is the layer) */
static void region(const vtex *e, const NVNcopyRegion *r, unsigned level, unsigned layer0, VkImageSubresourceLayers *sub, VkOffset3D *o, VkExtent3D *x) {
    uint32_t w, h, d; vt_size(e, level, &w, &h, &d);
    int is3d = d > 1 || (vt_layers(e) == 1 && r->depth > 1);
    *sub = (VkImageSubresourceLayers){copy_aspect(e), level, layer0 + (is3d ? 0 : (unsigned)r->zoffset), is3d ? 1 : (unsigned)(r->depth > 0 ? r->depth : 1)};
    *o = (VkOffset3D){r->xoffset, r->yoffset, is3d ? r->zoffset : 0};
    *x = (VkExtent3D){(uint32_t)r->width, (uint32_t)(r->height > 0 ? r->height : 1), is3d ? (uint32_t)(r->depth > 0 ? r->depth : 1) : 1};
    if (sub->baseArrayLayer + sub->layerCount > vt_layers(e)) sub->layerCount = vt_layers(e) > sub->baseArrayLayer ? vt_layers(e) - sub->baseArrayLayer : 1;
}
static uint32_t row_texels(const tex_t *t, int64_t stride) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    return stride > 0 && bpb ? (uint32_t)(stride / bpb) * (uint32_t)bw : 0;
}
static void copy_b2t(const p_copy_b2t *c) {
    vtex *e = c->tex ? vt_get(c->tex) : NULL;
    if (!e) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    VkImageSubresourceLayers sub; VkOffset3D o; VkExtent3D x;
    region(e, &c->r, view_level(&c->view, c->has_view), view_layer(&c->view, c->has_view), &sub, &o, &x);
    uint64_t row = S.copy_stride > 0 ? (uint64_t)S.copy_stride : (uint64_t)((x.width + bw - 1) / bw) * bpb;
    uint64_t n = row * ((x.height + bh - 1) / bh) * x.depth * sub.layerCount;
    VkBuffer sb; VkDeviceSize so;
    vd_ub_guard(c->src, c->src + n);
    if (!vk_buf(c->src, n, &sb, &so)) { VK_ONCE("buffer-to-texture copy from %#llx: not GPU memory", (unsigned long long)c->src); return; }
    vx_end_pass();
    VkBufferImageCopy r = {.bufferOffset = so, .bufferRowLength = row_texels(c->tex, S.copy_stride), .imageSubresource = sub, .imageOffset = o, .imageExtent = x};
    vkCmdCopyBufferToImage(vk_cmd(), sb, vt_image(e), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    vk_barrier(vk_cmd());
    vt_written(e);
}
static void copy_t2b(const p_copy_b2t *c) {   /* the T2B record: tex is the source, src the destination address */
    vtex *e = c->tex ? vt_get(c->tex) : NULL;
    if (!e) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    VkImageSubresourceLayers sub; VkOffset3D o; VkExtent3D x;
    region(e, &c->r, view_level(&c->view, c->has_view), view_layer(&c->view, c->has_view), &sub, &o, &x);
    uint64_t row = S.copy_stride > 0 ? (uint64_t)S.copy_stride : (uint64_t)((x.width + bw - 1) / bw) * bpb;
    uint64_t n = row * ((x.height + bh - 1) / bh) * x.depth * sub.layerCount;
    VkBuffer db; VkDeviceSize dof;
    vd_ub_guard(c->src, c->src + n);
    if (!vk_buf(c->src, n, &db, &dof)) { VK_ONCE("texture-to-buffer copy to %#llx: not GPU memory", (unsigned long long)c->src); return; }
    vx_end_pass();
    VkBufferImageCopy r = {.bufferOffset = dof, .bufferRowLength = row_texels(c->tex, S.copy_stride), .imageSubresource = sub, .imageOffset = o, .imageExtent = x};
    vkCmdCopyImageToBuffer(vk_cmd(), vt_image(e), VK_IMAGE_LAYOUT_GENERAL, db, 1, &r);
    vk_barrier(vk_cmd());
}
/* bytes of one texel as a buffer copy sees it (the depth aspect of a depth image: 2 for D16, else 4); *block = texels per block row */
static int texel_bytes(const vtex *e, const tex_t *t, int *block, int *bh) {
    if (vt_depth(e)) { *block = *bh = 1; return vt_vkformat(e) == VK_FORMAT_D16_UNORM ? 2 : 4; }
    return nvn_fmt_bpb(t->d.format, block, bh);
}
static void copy_t2t(const p_copy_t2t *c) {
    vtex *s = c->src ? vt_get(c->src) : NULL, *d = c->dst ? vt_get(c->dst) : NULL;
    if (plog()) VLOG("[pass] copy %ux%u fmt %#x -> %ux%u fmt %#x (%s %s)", c->src ? c->src->d.width : 0, c->src ? c->src->d.height : 0, c->src ? c->src->d.format : 0,
                     c->dst ? c->dst->d.width : 0, c->dst ? c->dst->d.height : 0, c->dst ? c->dst->d.format : 0, s ? "src" : "-", d ? "dst" : "-");
    if (!s || !d) return;
    VkImageSubresourceLayers ss, ds; VkOffset3D so, dof; VkExtent3D sx, dx;
    region(s, &c->sr, view_level(&c->sv, c->has_sv), view_layer(&c->sv, c->has_sv), &ss, &so, &sx);
    region(d, &c->dr, view_level(&c->dv, c->has_dv), view_layer(&c->dv, c->has_dv), &ds, &dof, &dx);
    vx_end_pass();
    int same = sx.width == dx.width && sx.height == dx.height && sx.depth == dx.depth && !(c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT));
    /* depth <-> colour (the game copies depth into an R32F texture) or two different depth formats: Vulkan allows neither a
       copy nor a blit there, so the depth aspect goes through a buffer (same texel size on both sides, bit for bit) */
    if (same && (vt_depth(s) || vt_depth(d)) && vt_vkformat(s) != vt_vkformat(d)) {
        int bw, bh, bd, bs = texel_bytes(s, c->src, &bw, &bh), bt = texel_bytes(d, c->dst, &bd, &bh);
        if (bs && bs == bt && bw == 1 && bd == 1) {
            uint32_t layers = ss.layerCount < ds.layerCount ? ss.layerCount : ds.layerCount;
            ss.layerCount = ds.layerCount = layers;
            VkBuffer sb; VkDeviceSize so2;
            vk_stage((VkDeviceSize)sx.width * sx.height * sx.depth * layers * (unsigned)bs, 16, &sb, &so2);
            VkBufferImageCopy out = {.bufferOffset = so2, .imageSubresource = ss, .imageOffset = so, .imageExtent = sx};
            VkBufferImageCopy in = {.bufferOffset = so2, .imageSubresource = ds, .imageOffset = dof, .imageExtent = dx};
            vkCmdCopyImageToBuffer(vk_cmd(), vt_image(s), VK_IMAGE_LAYOUT_GENERAL, sb, 1, &out);
            vk_barrier(vk_cmd());
            vkCmdCopyBufferToImage(vk_cmd(), sb, vt_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &in);
            vk_barrier(vk_cmd());
            vt_written(d);
            return;
        }
        VK_ONCE("copy between formats %#x and %#x (depth involved, texel sizes %d / %d): not possible, left to the blit", c->src->d.format, c->dst->d.format, bs, bt);
    }
    if (same && (vt_vkformat(s) == vt_vkformat(d) || (!vt_depth(s) && !vt_depth(d) && !vk_fmt_block_compressed(vt_vkformat(s)) && !vk_fmt_block_compressed(vt_vkformat(d))))) {
        if (ds.layerCount != ss.layerCount) ds.layerCount = ss.layerCount = ss.layerCount < ds.layerCount ? ss.layerCount : ds.layerCount;
        VkImageCopy r = {ss, so, ds, dof, sx};
        vkCmdCopyImage(vk_cmd(), vt_image(s), VK_IMAGE_LAYOUT_GENERAL, vt_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        VkImageBlit r = {ss, {so, {so.x + (int32_t)sx.width, so.y + (int32_t)sx.height, so.z + (int32_t)sx.depth}},
                         ds, {dof, {dof.x + (int32_t)dx.width, dof.y + (int32_t)dx.height, dof.z + (int32_t)dx.depth}}};
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int32_t t = r.dstOffsets[0].x; r.dstOffsets[0].x = r.dstOffsets[1].x; r.dstOffsets[1].x = t; }
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int32_t t = r.dstOffsets[0].y; r.dstOffsets[0].y = r.dstOffsets[1].y; r.dstOffsets[1].y = t; }
        if (ds.layerCount != ss.layerCount) ds.layerCount = ss.layerCount = ss.layerCount < ds.layerCount ? ss.layerCount : ds.layerCount;
        r.srcSubresource = ss; r.dstSubresource = ds;
        int linear = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && !vt_depth(s);
        vkCmdBlitImage(vk_cmd(), vt_image(s), VK_IMAGE_LAYOUT_GENERAL, vt_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    vk_barrier(vk_cmd());
    vt_written(d);
}
static void clear_tex(const p_clear_tex *c) {
    vtex *e = c->tex ? vt_get(c->tex) : NULL;
    if (!e || vt_depth(e) || vk_fmt_block_compressed(vt_vkformat(e))) return;
    if ((c->mask & 0xf) != 0xf) VK_ONCE("texture clear with channel mask %#x: done as a full clear for now", c->mask);
    unsigned lv = view_level(&c->view, c->has_view);
    VkImageSubresourceLayers sub; VkOffset3D o; VkExtent3D x;
    region(e, &c->r, lv, view_layer(&c->view, c->has_view), &sub, &o, &x);
    uint32_t w, h, d; vt_size(e, lv, &w, &h, &d);
    if (o.x || o.y || x.width < w || x.height < h) VK_ONCE("partial texture clear (%ux%u of %ux%u): the whole level is cleared for now", x.width, x.height, w, h);
    vx_end_pass();
    VkClearColorValue v; memcpy(&v, c->c.u, 16);
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, sub.baseArrayLayer, sub.layerCount};
    vkCmdClearColorImage(vk_cmd(), vt_image(e), VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    vk_barrier(vk_cmd());
    vt_written(e);
}

/* TOTK: SetRenderEnableConditional, mode 0 = render if the two counters at addr / addr + 16 are equal. The counters
   are written when earlier submissions complete; read at replay (shortcut: a counter reported earlier in the same
   submission is not seen yet). */
static int cond_eval(const p_render_cond *c) {
    uint64_t a, b; memcpy(&a, (const void *)(uintptr_t)c->addr, 8); memcpy(&b, (const void *)(uintptr_t)(c->addr + 16), 8);
    return c->mode == 0 ? a == b : a != b;
}

void vx_run(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("Vulkan replay: corrupt recording at %p (op %u size %u)", (const void *)p, h.op, h.size);
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); vx_run((const uint8_t *)(uintptr_t)t); break; }
        case OP_SIGNAL_EVENT: {
            ev_act *e = malloc(sizeof *e); memcpy(&e->val, a, 8); memcpy(&e->mode, a + 8, 4); memcpy(&e->value, a + 12, 4);
            vk_after(do_event, e); break;
        }
        case OP_REPORT_COUNTER: {
            ctr_act *c = calloc(1, sizeof *c); memcpy(&c->type, a, 8); memcpy(&c->addr, a + 8, 8);
            if (g_oq_on && c->type == NVN_COUNTER_TYPE_SAMPLES_PASSED) { vx_end_pass(); c->real = 1; c->from = g_oq_start; c->to = g_oq_n; }   /* later draws do not count */
            vk_after(do_counter, c); break;
        }
        case OP_RESET_COUNTER: {
            uint64_t t; memcpy(&t, a, 8);
            if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_enabled() && oq_make()) {
                vx_end_pass();   /* draws before the reset are not part of the new count; the next pass starts with its own query */
                g_oq_on = 1; g_oq_start = g_oq_n;
            }
            break;
        }
        case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); copy_b2b(s, d, n); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); update_ub(d, a + 16, n); break; }
        case OP_SET_RT: vx_end_pass(); memcpy(&S.rt, a, sizeof S.rt); S.has_rt = 1; break;
        case OP_CLEAR_COLOR: clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: clear_ds((const p_clear_ds *)a); break;
        case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a); break;
        case OP_COPY_T2B: copy_t2b((const p_copy_b2t *)a); break;
        case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
        case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
        case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; vd_state_op(h.op, a); break;
        case OP_RENDER_ENABLE: S.render_disabled = !((const p_flag *)a)->value; break;
        case OP_RENDER_COND: S.render_disabled = !cond_eval((const p_render_cond *)a); break;
        case OP_BARRIER: vx_end_pass(); vk_barrier(vk_cmd()); break;
        case OP_DRAW: if (!S.render_disabled) vd_draw((const p_draw *)a, NULL); break;
        case OP_DRAW_IDX: if (!S.render_disabled) vd_draw(NULL, (const p_draw_idx *)a); break;
        case OP_DISPATCH: vd_dispatch((const p_dispatch *)a); break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
        default: vd_state_op(h.op, a); break;   /* binds and render state */
        }
        p += h.size;
    }
}

void vd_shadows_flush(void);
void vx_submit_done(void) { vd_ub_flush(); vx_end_pass(); vd_shadows_flush(); }

void vx_pin_refs(void (*mark)(const void *)) {
    pthread_mutex_lock(&vk_mu);
    if (S.has_rt) {
        for (int i = 0; i < 8; i++) if (S.rt.color[i]) mark(S.rt.color[i]);
        if (S.rt.depth) mark(S.rt.depth);
    }
    pthread_mutex_unlock(&vk_mu);
}
