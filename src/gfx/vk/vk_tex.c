/* NVN textures as Vulkan images (upstream nvn_mtl.m "Texture -> MTLTexture" and "storage-shared GPU textures").

   An image belongs to storage: (memory, size, dimensions, levels, target, samples, format class). Every texture
   object over the same bytes with the same layout gets the same image, so what is rendered through one is seen
   through another (upstream: the deferred lighting read all-zero normals before this). Uncompressed colour formats of
   one texel size share an image (MUTABLE_FORMAT views); depth, compressed and converted formats are keyed by format.

   Mirrored textures (upstream tex_mirrored: CPU-visible pool, no render-target kind of flag, not depth / MS /
   buffer) take their bytes from pool memory: uploaded at first use, and again at the first use in a submission after
   the dirty tracker saw a write to their pages. A texture the GPU writes is owned by the GPU from then on.

   Images unused for 600 submissions are dropped, except those whose content cannot be made again (rendered or copied
   into by the GPU) while a texture object still refers to them (upstream: while a serial holds the storage). Dropping
   those left a menu background, or a sky rendered once, blank when it was used again. */
#include "gfx/vk/vk.h"
#include "nvn_deswizzle.h"
#include "nvn_dirty.h"
#include "nvn_hmap.h"

int vk_astc_decode(const uint8_t *src, size_t n, unsigned bw, unsigned bh, unsigned w, unsigned h, int srgb, uint8_t *dst);   /* vk_astc.cpp */

typedef struct { uint64_t mem, size; uint32_t w, h, d, levels, target, samples, cls, pad; } stor_key;   /* 48 bytes, no padding holes */
typedef struct { VkFormat f; VkImageViewType type; uint32_t base_level, levels, base_layer, layers; VkComponentSwizzle swz[4]; VkImageAspectFlags aspect; } view_key;
typedef struct { view_key k; VkImageView v; } view_ent;
struct vtex {
    stor_key k;
    VkImage img; VmaAllocation al; VkFormat fmt; vk_fmt f; VkImageAspectFlags aspect; VkImageCreateFlags flags;
    uint32_t w, h, d, layers, levels; VkImageType type;
    int mirrored, owned, uploaded;
    char writer[128];   /* what last wrote it (diagnostics: the pass log prints it for sampled textures) */
    uint64_t stamp, checked_sub, used_sub;
    view_ent *views; int nviews, cviews;
};
static nvn_hmap g_by_stor = {.klen = sizeof(stor_key)};
static nvn_hmap g_ser = {.klen = 8};   /* texture object (serial) -> the stor_key of the image it last used (vk_mu) */

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static int fmt_depth(int f) { return f >= 0x31 && f <= 0x36; }

VkImage vt_image(const vtex *e) { return e->img; }
VkFormat vt_vkformat(const vtex *e) { return e->fmt; }
VkImageAspectFlags vt_aspect(const vtex *e) { return e->aspect; }
int vt_depth(const vtex *e) { return e->f.depth || e->f.stencil; }
uint32_t vt_layers(const vtex *e) { return e->layers; }
void vt_size(const vtex *e, unsigned level, uint32_t *w, uint32_t *h, uint32_t *d) {
    *w = mip(e->w, level); *h = e->type == VK_IMAGE_TYPE_1D ? 1 : mip(e->h, level); *d = e->type == VK_IMAGE_TYPE_3D ? mip(e->d, level) : 1;
}
char vt_writer[96];
void vt_written(vtex *e) {
    if (!e) return;
    e->owned = 1;
    snprintf(e->writer, sizeof e->writer, "%s (submission %llu)", vt_writer[0] ? vt_writer : "?", (unsigned long long)vk_sub_serial());
}
const char *vt_writer_of(const vtex *e) { return e && e->writer[0] ? e->writer : "nothing since it was made (zero) or an upload"; }

static int tex_mirrored(const tex_t *t) {   /* upstream nvn_mtl.m tex_mirrored */
    if (!t || !t->mem) return 0;
    if (t->has_layout) return 1;
    const pool_t *p = t->pool;
    if (!p || p->host || !(p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL)) return 0;
    if (t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER || fmt_depth(t->d.format)) return 0;
    return 1;
}

/* NVN compression (upstream nvn_mtl.m stor_compressed): a compressible texture in a compressible pool keeps its bytes when
   it is cleared, the clear only sets the compression state, so a descriptor reading the same memory in another format does
   not see the clear. One shared image would: a clear of an RGBA8 target to white turns the R32F linear depth that used to
   live there into NaN, and the lighting and fog with it (upstream's frame-wide flashes). Such textures share an image only
   with the same format or its sRGB / linear twin. TOTK_COMPRESSED_ALIAS=1 shares across formats (A/B). */
static int stor_compressed(const tex_t *t) { return (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE); }
static int compressed_alias_on(void) { static int on = -1; if (on < 0) { const char *e = pal_getenv("TOTK_COMPRESSED_ALIAS"); on = e && atoi(e); } return on; }
static int key_of(const tex_t *t, const vk_fmt *f, stor_key *k) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!t->mem || !t->L.size || !bpb) return 0;
    memset(k, 0, sizeof *k);
    k->mem = (uint64_t)(uintptr_t)t->mem; k->size = t->L.size;
    k->w = t->d.width ? t->d.width : 1; k->h = t->d.height ? t->d.height : 1; k->d = t->d.depth ? t->d.depth : 1;
    k->levels = t->d.levels ? t->d.levels : 1; k->target = (uint32_t)t->d.target; k->samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1;
    /* format class: uncompressed colour by texel size, everything else by format */
    int plain = bw == 1 && bh == 1 && !f->depth && !f->stencil && !f->convert;
    if (plain && stor_compressed(t) && !compressed_alias_on()) {   /* the format, with the sRGB twin counted as the same */
        VkFormat v = f->vk == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f->vk == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f->vk;
        k->cls = 0x2000u + (uint32_t)v;
    } else k->cls = plain ? 0x1000u + (uint32_t)bpb : (uint32_t)t->d.format;
    return 1;
}

/* image shape from the NVN target (nvn_tex.c shape) */
static void shape(const tex_t *t, VkImageType *type, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *layers) {
    *w = t->d.width ? t->d.width : 1; *h = t->d.height ? t->d.height : 1; *d = 1; *layers = 1;
    uint32_t z = t->d.depth ? t->d.depth : 1;
    switch (t->d.target) {
    case NVN_TEXTURE_TARGET_1D: *type = VK_IMAGE_TYPE_1D; *h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: *type = VK_IMAGE_TYPE_1D; *layers = *h; *h = 1; break;
    case NVN_TEXTURE_TARGET_3D: *type = VK_IMAGE_TYPE_3D; *d = z; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: *type = VK_IMAGE_TYPE_2D; *layers = z; break;
    case NVN_TEXTURE_TARGET_CUBEMAP: *type = VK_IMAGE_TYPE_2D; *layers = 6; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: *type = VK_IMAGE_TYPE_2D; *layers = 6 * z; break;
    default: *type = VK_IMAGE_TYPE_2D; break;
    }
}

static VkFormatFeatureFlags features(VkFormat f) {
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(vk_phys, f, &p);
    return p.optimalTilingFeatures;
}

static void init_layout(vtex *e) {   /* UNDEFINED -> GENERAL, in the uploads command buffer (before any use) */
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->img,
        .subresourceRange = {e->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(vk_pre(), &di);
    /* start from zero, as fresh NVN memory reads (render targets are cleared by the game before use) */
    if (e->f.depth || e->f.stencil) {
        VkClearDepthStencilValue z = {0};
        VkImageSubresourceRange r = {e->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(vk_pre(), e->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    } else if (!vk_fmt_block_compressed(e->fmt)) {
        VkClearColorValue z = {0};
        VkImageSubresourceRange r = {e->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(vk_pre(), e->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    }
    vk_barrier(vk_pre());
}

static uint64_t g_images, g_image_bytes, g_uploads, g_upload_bytes, g_upload_ns, g_reuploads, g_untracked;
static vtex *create(const tex_t *t, const vk_fmt *f, const stor_key *k) {
    vtex *e = calloc(1, sizeof *e);
    e->k = *k; e->f = *f; e->fmt = f->vk; e->levels = k->levels;
    shape(t, &e->type, &e->w, &e->h, &e->d, &e->layers);
    e->aspect = f->depth || f->stencil ? (f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (f->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
    if (e->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) e->aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    VkFormatFeatureFlags ff = features(e->fmt);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && k->samples == 1) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        VLOG("format %#x (Vulkan %d) is not usable on this GPU: texture %ux%u left out", t->d.format, (int)e->fmt, e->w, e->h);
        free(e); return NULL;
    }
    VkImageCreateFlags fl = 0;
    if (!f->depth && !f->stencil) fl |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (e->type == VK_IMAGE_TYPE_2D && e->layers >= 6 && e->w == e->h && k->samples == 1) fl |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (e->type == VK_IMAGE_TYPE_3D) fl |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    if (vk_fmt_block_compressed(e->fmt) && !f->depth && !f->stencil) fl |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;   /* rendering into blocks through a one-texel-per-block view */
    e->flags = fl;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = fl, .imageType = e->type, .format = e->fmt,
        .extent = {e->w, e->h, e->d}, .mipLevels = e->levels, .arrayLayers = e->layers, .samples = (VkSampleCountFlagBits)k->samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ac = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(vk_vma, &ci, &ac, &e->img, &e->al, NULL);
    if (r != VK_SUCCESS) {
        VLOG("image %ux%ux%u x%u levels %u format %d (NVN %#x target %d) not created (%d)", e->w, e->h, e->d, e->layers, e->levels,
             (int)e->fmt, t->d.format, t->d.target, (int)r);
        free(e); return NULL;
    }
    e->mirrored = tex_mirrored(t);
    { static int near;   /* diagnostic: another layout already lives at this memory (the first 12): on hardware both descriptors read the same bytes */
      for (uint32_t i = 0; near < 12 && i < g_by_stor.cap; i++) {
          const vtex *o = g_by_stor.vals[i];
          if (o && o->k.mem == k->mem && o->k.size == k->size) { near++;
              VLOG("storage %p (%llu bytes): %ux%ux%u lv%u target %u samples %u class %#x (NVN format %#x, flags %#x) beside an existing %ux%ux%u lv%u target %u samples %u class %#x",
                   (void *)(uintptr_t)k->mem, (unsigned long long)k->size, k->w, k->h, k->d, k->levels, k->target, k->samples, k->cls, t->d.format, t->d.flags,
                   o->k.w, o->k.h, o->k.d, o->k.levels, o->k.target, o->k.samples, o->k.cls); break; } } }
    init_layout(e);
    g_images++; g_image_bytes += t->L.size;
    nvn_hm_put(&g_by_stor, k, e);
    return e;
}

static void destroy(void *arg) {
    vtex *e = arg;
    for (int i = 0; i < e->nviews; i++) vkDestroyImageView(vk_dev, e->views[i].v, NULL);
    free(e->views);
    vmaDestroyImage(vk_vma, e->img, e->al);
    free(e);
}

/* ---------- uploads from pool memory ---------- */
static void expand_rgb(uint8_t *dst, const uint8_t *src, size_t texels, unsigned comp_bytes, int is_int) {
    for (size_t i = 0; i < texels; i++) {
        memcpy(dst + i * 4 * comp_bytes, src + i * 3 * comp_bytes, 3 * comp_bytes);
        uint8_t *a = dst + i * 4 * comp_bytes + 3 * comp_bytes;
        if (comp_bytes == 1) a[0] = is_int ? 1 : 0xff;
        else if (comp_bytes == 2) { uint16_t one = is_int ? 1 : 0x3c00; memcpy(a, &one, 2); }   /* RGB16F: 1.0; the 16-bit unorm kinds read it as almost 0 */
        else { uint32_t one = is_int ? 1 : 0x3f800000u; memcpy(a, &one, 4); }
    }
}

static void upload(vtex *e, const tex_t *t) {
    uint64_t t0 = pal_now_ns();
    g_uploads++; g_upload_bytes += t->L.size; if (e->uploaded) g_reuploads++;
    if (!nvn_dirty_tracked(t->mem, (size_t)t->L.size)) g_untracked++;
    e->stamp = nvn_dirty_now();
    snprintf(e->writer, sizeof e->writer, "upload from pool memory (submission %llu)", (unsigned long long)vk_sub_serial());
    nvn_dirty_protect(t->mem, (size_t)t->L.size);   /* before copying: a write from here on is seen at the next check */
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    VkCommandBuffer c = vk_pre();
    for (unsigned lv = 0; lv < e->levels; lv++) {
        uint32_t w, h, d; vt_size(e, lv, &w, &h, &d);
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh, slice = pitch * rows;
        for (unsigned ly = 0; ly < e->layers; ly++) {
            size_t n = slice * d;
            uint8_t *lin = malloc(n);
            if (!lin) die("texture upload: %zu bytes", n);
            if (nvn_deswizzle(&t->d, lv, ly, t->mem, lin, pitch)) {
                VK_ONCE("texture %ux%u format %#x target %d flags %#x: layout not supported for upload", t->d.width, t->d.height, t->d.format, t->d.target, t->d.flags);
                free(lin); goto done;
            }
            size_t out_n = e->f.convert == 2 ? n / 3 * 4 : e->f.convert == 3 ? (size_t)w * h * d * 4 : n;
            VkBuffer sb; VkDeviceSize so;
            uint8_t *dst = vk_stage(out_n, 16, &sb, &so);
            if (e->f.convert == 3) {
                for (uint32_t z = 0; z < d; z++)
                    if (vk_astc_decode(lin + z * slice, slice, (unsigned)bw, (unsigned)bh, w, h, t->d.format >= 0x87, dst + (size_t)z * w * h * 4)) {
                        VK_ONCE("ASTC %dx%d texture %ux%u did not decode: left blank", bw, bh, w, h);
                        memset(dst + (size_t)z * w * h * 4, 0, (size_t)w * h * 4);
                    }
            } else if (e->f.convert == 1) {   /* 24-bit depth in the low bits of each 32-bit texel, stencil in the high byte: depth as float */
                const uint32_t *src = (const uint32_t *)lin; float *df = (float *)dst;
                for (size_t i = 0; i < n / 4; i++) df[i] = (float)(src[i] & 0xffffffu) * (1.0f / 16777215.0f);
                if (e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {   /* the stencil bytes are their own plane of the image */
                    VkBuffer sb2; VkDeviceSize so2;
                    uint8_t *sp = vk_stage(n / 4, 16, &sb2, &so2);
                    for (size_t i = 0; i < n / 4; i++) sp[i] = (uint8_t)(src[i] >> 24);
                    VkBufferImageCopy rs = {.bufferOffset = so2, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, lv, ly, 1}, .imageExtent = {w, h, d}};
                    vkCmdCopyBufferToImage(c, sb2, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
                }
            } else if (e->f.convert == 2) {
                int is_int = (t->d.format >= 0x1B && t->d.format <= 0x1C) || (t->d.format >= 0x20 && t->d.format <= 0x21) || (t->d.format >= 0x23 && t->d.format <= 0x24);
                expand_rgb(dst, lin, (size_t)w * h * d, (unsigned)bpb / 3, is_int);
            } else memcpy(dst, lin, n);
            { static int texlog = -1, nlog;   /* TOTK_TEXLOG=1: the mean colour of what is uploaded (4-byte texels), to see what a texture holds without looking */
              if (texlog < 0) { const char *e = pal_getenv("TOTK_TEXLOG"); texlog = e && *e == '1'; }
              if (texlog && lv == 0 && ly == 0 && out_n == (size_t)w * h * 4 && nlog < 400) { nlog++;
                  uint64_t sum[4] = {0, 0, 0, 0}; for (size_t i = 0; i < (size_t)w * h; i++) for (int c = 0; c < 4; c++) sum[c] += dst[i * 4 + c];
                  double nn = (double)w * h * 255.0;
                  VLOG("[tex] upload %ux%u NVN format %#x (Vulkan %d) mean rgba %.3f %.3f %.3f %.3f", w, h, (unsigned)t->d.format, (int)e->fmt, sum[0] / nn, sum[1] / nn, sum[2] / nn, sum[3] / nn); } }
            free(lin);
            VkBufferImageCopy r = {.bufferOffset = so, .imageSubresource = {e->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT, lv, ly, 1},
                                   .imageExtent = {w, h, d}};
            vkCmdCopyBufferToImage(c, sb, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
    }
done:
    vk_barrier(c);
    e->uploaded = 1;
    { uint64_t dt = pal_now_ns() - t0; g_upload_ns += dt; vk_ns_tex += dt; }
}

vtex *vt_get(const tex_t *t) {
    if (!t || !t->serial || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    vk_fmt f; stor_key k;
    if (!vk_fmt_of(t->d.format, &f)) { VK_ONCE("texture format %#x has no Vulkan format yet: left out", t->d.format); return NULL; }
    if (!key_of(t, &f, &k)) return NULL;
    vtex *e = nvn_hm_get(&g_by_stor, &k);
    if (!e && !(e = create(t, &f, &k))) return NULL;
    uint64_t sub = vk_sub_serial();
    e->used_sub = sub;
    { stor_key *held = nvn_hm_get(&g_ser, &t->serial);
      if (!held) { held = malloc(sizeof *held); if (held) { *held = e->k; nvn_hm_put(&g_ser, &t->serial, held); } }
      else if (memcmp(held, &e->k, sizeof *held)) *held = e->k; }
    if (e->mirrored && !e->owned && e->checked_sub != sub) {
        e->checked_sub = sub;
        if (!e->uploaded || nvn_dirty_since(t->mem, (size_t)t->L.size, e->stamp)) upload(e, t);
    }
    return e;
}

/* ---------- views ---------- */
static VkImageViewType view_type(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case NVN_TEXTURE_TARGET_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case NVN_TEXTURE_TARGET_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}
static VkComponentSwizzle sel(unsigned s, const VkComponentSwizzle fmt[4]) {
    return s == NVN_TEXTURE_SWIZZLE_ZERO ? VK_COMPONENT_SWIZZLE_ZERO : s == NVN_TEXTURE_SWIZZLE_ONE ? VK_COMPONENT_SWIZZLE_ONE : s <= 5 ? fmt[s - 2] : VK_COMPONENT_SWIZZLE_IDENTITY;
}

uint32_t vt_samples(const vtex *e) { return e->k.samples; }
VkFormat vt_last_vfmt;
int vt_block_view(const vtex *e, const tex_t *t, const view_t *v, int *bw, int *bh) {
    if (!vk_fmt_block_compressed(e->fmt)) return 0;
    int format = v && v->set_format ? v->format : t->d.format;
    vk_fmt f; int b2, b3, cb = nvn_fmt_bpb(t->d.format, bw, bh), vb = nvn_fmt_bpb(format, &b2, &b3);
    if (!vk_fmt_of(format, &f) || vk_fmt_block_compressed(f.vk) || f.depth || f.stencil || f.convert) return 0;
    return cb && vb == cb && b2 == 1 && b3 == 1 && (*bw > 1 || *bh > 1);
}
VkImageView vt_view(vtex *e, const tex_t *t, const view_t *v, int mode, VkImageViewType want) {
    int attachment = mode == VT_ATTACHMENT;
    view_key k; memset(&k, 0, sizeof k);
    int format = v && v->set_format ? v->format : t->d.format, target = v && v->set_target ? v->target : t->d.target;
    vk_fmt f;
    int bw0, bh0, blockview = vt_block_view(e, t, v, &bw0, &bh0);
    if (!vk_fmt_of(format, &f) || (f.vk != e->fmt && (e->f.depth || e->f.stencil || e->f.convert || f.convert || (vk_fmt_block_compressed(e->fmt) && !blockview)))) f = e->f;
    k.f = f.vk; vt_last_vfmt = f.vk;
    k.base_level = v && v->set_levels ? v->base_level : 0;
    k.levels = attachment ? 1 : v && v->set_levels && v->nlevels ? v->nlevels : e->levels - k.base_level;
    k.base_layer = v && v->set_layers ? v->min_layer : 0;
    k.layers = v && v->set_layers && v->nlayers ? v->nlayers : e->layers - k.base_layer;
    if (mode == VT_STORAGE || blockview) k.levels = 1;
    if (blockview) k.layers = 1;   /* a view of blocks as texels covers one level and one layer */
    if (k.base_level >= e->levels) k.base_level = e->levels - 1;
    if (k.base_level + k.levels > e->levels) k.levels = e->levels - k.base_level;
    if (k.base_layer >= e->layers) k.base_layer = e->layers - 1;
    if (k.base_layer + k.layers > e->layers) k.layers = e->layers - k.base_layer;
    if (attachment) {
        k.type = e->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : k.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : e->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
        if (e->type == VK_IMAGE_TYPE_3D) { k.base_layer = 0; k.layers = 1; }   /* slices of a 3D target: shortcut, slice 0 only */
        for (int i = 0; i < 4; i++) k.swz[i] = VK_COMPONENT_SWIZZLE_IDENTITY;
        k.aspect = e->aspect;
    } else {
        k.type = want != VT_ANY_TYPE ? want : view_type(target);
        if (e->type == VK_IMAGE_TYPE_3D) k.type = VK_IMAGE_VIEW_TYPE_3D;
        if (e->type == VK_IMAGE_TYPE_1D) k.type = k.type == VK_IMAGE_VIEW_TYPE_1D_ARRAY || k.type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        else if (e->type == VK_IMAGE_TYPE_2D && (k.type == VK_IMAGE_VIEW_TYPE_1D || k.type == VK_IMAGE_VIEW_TYPE_1D_ARRAY || k.type == VK_IMAGE_VIEW_TYPE_3D)) k.type = VK_IMAGE_VIEW_TYPE_2D;
        if ((k.type == VK_IMAGE_VIEW_TYPE_CUBE || k.type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && !(e->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) k.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (k.type == VK_IMAGE_VIEW_TYPE_CUBE) k.layers = 6;
        if (k.type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) k.layers -= k.layers % 6;
        if (k.type == VK_IMAGE_VIEW_TYPE_2D && k.layers > 1) k.layers = 1;
        if (k.type == VK_IMAGE_VIEW_TYPE_1D && k.layers > 1) k.layers = 1;
        unsigned s[4] = {t->d.swizzle_r, t->d.swizzle_g, t->d.swizzle_b, t->d.swizzle_a};
        if (v && v->set_swizzle) for (int i = 0; i < 4; i++) s[i] = v->swz[i];
        for (int i = 0; i < 4; i++) k.swz[i] = mode == VT_STORAGE ? VK_COMPONENT_SWIZZLE_IDENTITY : sel(s[i], f.swz);
        /* a depth / stencil image is sampled through one aspect: stencil when the view's format is the stencil one */
        k.aspect = e->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? (format == 0x31 ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : e->aspect;
    }
    for (int i = 0; i < e->nviews; i++) if (!memcmp(&e->views[i].k, &k, sizeof k)) return e->views[i].v;
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    VkFormatFeatureFlags ff = features(k.f);
    uci.usage = attachment ? (e->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                           : mode == VT_STORAGE ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mode == VT_STORAGE && !(ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { VK_ONCE("format %d cannot be a storage image here", (int)k.f); return VK_NULL_HANDLE; }
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &uci, .image = e->img, .viewType = k.type, .format = k.f,
        .components = {k.swz[0], k.swz[1], k.swz[2], k.swz[3]}, .subresourceRange = {k.aspect, k.base_level, k.levels, k.base_layer, k.layers}};
    VkImageView view;
    VkResult r = vkCreateImageView(vk_dev, &ci, NULL, &view);
    if (r != VK_SUCCESS) { VK_ONCE("image view (format %d type %d) not created (%d)", (int)k.f, (int)k.type, (int)r); return VK_NULL_HANDLE; }
    if (e->nviews == e->cviews) { e->cviews = e->cviews ? e->cviews * 2 : 4; e->views = realloc(e->views, (size_t)e->cviews * sizeof *e->views); }
    e->views[e->nviews++] = (view_ent){k, view};
    return view;
}

/* ---------- lifetime ---------- */
void vt_release_serial(uint64_t serial) {   /* the texture object is finalised: its image may go when idle */
    pthread_mutex_lock(&vk_mu);
    free(nvn_hm_del(&g_ser, &serial));
    pthread_mutex_unlock(&vk_mu);
}

static void drop_where(int (*pred)(const vtex *, const void *), const void *arg, int deferred) {
    vtex *gone[256]; int n;
    do {
        n = 0;
        for (uint32_t i = 0; i < g_by_stor.cap && n < 256; i++) { vtex *e = g_by_stor.vals[i]; if (e && pred(e, arg)) gone[n++] = e; }
        for (int i = 0; i < n; i++) {
            nvn_hm_del(&g_by_stor, &gone[i]->k);
            g_images--; g_image_bytes -= gone[i]->k.size;
            if (deferred) vk_after(destroy, gone[i]); else destroy(gone[i]);
        }
    } while (n == 256);
}
typedef struct { uintptr_t lo, hi; } range;
static int in_range(const vtex *e, const void *arg) { const range *r = arg; return e->k.mem >= r->lo && e->k.mem < r->hi; }
/* ---------- buffer textures (NVN_TEXTURE_TARGET_BUFFER) ---------- */
/* Zero-copy, as upstream Metal: one VkBufferView directly over the pool bytes (nvn_mtl.m, target == 10): the GPU
   reads the current pool content at shader execution time, so CPU writes land in the sampled texels without an
   upload. Cached per (mem, size, format); dropped when the pool finalises (vt_pool_gone). */
typedef struct { uint64_t mem; uint64_t size; VkFormat fmt; } bv_key;
static nvn_hmap g_bviews = {.klen = sizeof(bv_key)};
static void bv_gone_drop(void *arg) { VkBufferView v = *(VkBufferView *)arg; vkDestroyBufferView(vk_dev, v, NULL); free(arg); }
void vt_pool_gone(uintptr_t lo, uintptr_t hi) { range r = {lo, hi}; drop_where(in_range, &r, 1);
    bv_key gone[256]; int n;
    do {
        n = 0;
        for (uint32_t i = 0; i < g_bviews.cap && n < 256; i++) { bv_key *k = (bv_key *)(g_bviews.keys + (size_t)i * g_bviews.klen);
            if (g_bviews.vals[i] && k->mem >= lo && k->mem < hi) gone[n++] = *k; }
        for (int i = 0; i < n; i++) { void *v = nvn_hm_del(&g_bviews, &gone[i]);
            if (v) { VkBufferView *p = malloc(sizeof *p); if (p) { *p = (VkBufferView)(uintptr_t)v; vk_after(bv_gone_drop, p); }
            else vkDestroyBufferView(vk_dev, (VkBufferView)(uintptr_t)v, NULL); } }   /* destroy once the GPU is done */
    } while (n == 256);
}
/* What a buffer texture is as a texel buffer: format, bytes, the alignment a view of it must have; 0 when it cannot be one. */
static int texel_info(const tex_t *t, vk_fmt *f, uint64_t *size, uint32_t *align) {
    if (t->d.target != NVN_TEXTURE_TARGET_BUFFER) return 0;
    if (!t->mem || !t->d.width) { VK_ONCE("buffer texture has no pool storage"); return 0; }
    int bw, bh;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !vk_fmt_of(t->d.format, f) || f->vk == VK_FORMAT_UNDEFINED) { VK_ONCE("buffer texture format %#x is not linear", (unsigned)t->d.format); return 0; }
    if (vk_fmt_block_compressed(f->vk) || f->depth || f->stencil) { VK_ONCE("buffer texture format %#x cannot be a texel buffer", (unsigned)t->d.format); return 0; }
    VkFormatProperties fp;   /* texel buffer support is a buffer feature (not an image one) */
    vkGetPhysicalDeviceFormatProperties(vk_phys, f->vk, &fp);
    if (!(fp.bufferFeatures & (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT))) {
        VK_ONCE("format %#x has no texel buffer usage on this GPU", (unsigned)t->d.format); return 0; }
    *size = (uint64_t)t->d.width * (uint64_t)bpb;
    /* the offset of a view must be a multiple of the device's alignment; with single-texel alignment, of the texel's
       own alignment if that is smaller (the largest power of two dividing its size: 12 bytes -> 4) */
    uint32_t al = vk_texel_align[1] > vk_texel_align[0] ? vk_texel_align[1] : vk_texel_align[0], one = vk_texel_single[0] && vk_texel_single[1];
    uint32_t own = (uint32_t)bpb & (uint32_t)-(int32_t)bpb;
    *align = one && own < al ? own : al;
    return 1;
}
/* GPU writes to the bytes before the view reads them: a logged uniform update there reaches memory first */
void vd_ub_guard(uint64_t lo, uint64_t hi);
VkBufferView vt_texel_view(const tex_t *t) {
    vk_fmt f; uint64_t size; uint32_t al;
    if (!texel_info(t, &f, &size, &al)) return VK_NULL_HANDLE;
    VkBuffer buf; VkDeviceSize off;
    if (!vk_buf((uint64_t)(uintptr_t)t->mem, size, &buf, &off)) { VK_ONCE("buffer texture storage %#llx+%#llx is not in a registered pool", (unsigned long long)(uintptr_t)t->mem, (unsigned long long)size); return VK_NULL_HANDLE; }
    if (off % al) return VK_NULL_HANDLE;   /* vt_texel_view_copy */
    vd_ub_guard((uint64_t)(uintptr_t)t->mem, (uint64_t)(uintptr_t)t->mem + size);
    bv_key k = {(uint64_t)(uintptr_t)t->mem, size, f.vk};
    VkBufferView v = (VkBufferView)(uintptr_t)nvn_hm_get(&g_bviews, &k);
    if (v) return v;
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buf, .format = f.vk, .offset = off, .range = size};
    VkBufferView nv;
    VkResult r = vkCreateBufferView(vk_dev, &ci, NULL, &nv);
    if (r != VK_SUCCESS) { VK_ONCE("buffer view over pool bytes %#llx+%#llx (%u texels of %#x) failed (%d)", (unsigned long long)(uintptr_t)t->mem, (unsigned long long)size, t->d.width, t->d.format, (int)r); return VK_NULL_HANDLE; }
    nvn_hm_put(&g_bviews, &k, (void *)(uintptr_t)nv);
    return nv;
}
/* A buffer texture at an offset the device cannot view in place: its bytes copied into this submission's staging memory
   as they are now. For sampled (read only) use: writes through a storage texel buffer would be lost. */
VkBufferView vt_texel_view_copy(const tex_t *t) {
    vk_fmt f; uint64_t size; uint32_t al;
    if (!texel_info(t, &f, &size, &al)) return VK_NULL_HANDLE;
    vd_ub_guard((uint64_t)(uintptr_t)t->mem, (uint64_t)(uintptr_t)t->mem + size);
    VkBuffer sb; VkDeviceSize so;
    void *p = vk_stage(size, al, &sb, &so);
    if (pal_guarded_copy(p, (const void *)(uintptr_t)t->mem, (size_t)size)) memset(p, 0, (size_t)size);
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = sb, .format = f.vk, .offset = so, .range = size};
    VkBufferView nv;
    if (vkCreateBufferView(vk_dev, &ci, NULL, &nv) != VK_SUCCESS) { VK_ONCE("buffer view over a copy of %llu bytes (fmt %#x) failed", (unsigned long long)size, (unsigned)t->d.format); return VK_NULL_HANDLE; }
    VkBufferView *keep = malloc(sizeof *keep);
    if (keep) { *keep = nv; vk_after(bv_gone_drop, keep); }
    VK_ONCE("buffer texture (fmt %#x) at an offset not %u-byte aligned for a view: sampled from a copy made at each bind", (unsigned)t->d.format, al);
    return nv;
}
static void held_by_slots(nvn_hmap *held);   /* below: the images the texture pool's descriptors name */
typedef struct { uint64_t now; nvn_hmap held; } idle_arg;   /* held: the keys of the images texture objects still refer to */
static int idle(const vtex *e, const void *arg) {
    const idle_arg *a = arg;
    if (e->used_sub + 600 >= a->now) return 0;
    if (e->mirrored && !e->owned) return 1;   /* its bytes are in pool memory: uploaded again when needed */
    return !nvn_hm_get(&a->held, &e->k);
}
void vt_end_submission(void) {
    uint64_t now = vk_sub_serial();
    static uint64_t last_log;
    uint64_t t = pal_now_ns();
    if (!last_log) last_log = t;
    if (t - last_log >= 10000000000ull) {
        last_log = t;
        VLOG("%llu images (%.1f MiB of game texture storage); uploads %llu (%llu again, %llu untracked), %.1f MiB, %.0f ms",
             (unsigned long long)g_images, g_image_bytes / 1048576.0, (unsigned long long)g_uploads, (unsigned long long)g_reuploads,
             (unsigned long long)g_untracked, g_upload_bytes / 1048576.0, g_upload_ns / 1e6);
        g_uploads = g_reuploads = g_untracked = g_upload_bytes = g_upload_ns = 0;
    }
    if (now % 64) return;
    idle_arg a = {now, {.klen = sizeof(stor_key)}};
    for (uint32_t i = 0; i < g_ser.cap; i++) if (g_ser.vals[i]) nvn_hm_put(&a.held, g_ser.vals[i], (void *)1);
    held_by_slots(&a.held);   /* a descriptor can outlive the texture object it was made from */
    drop_where(idle, &a, 0);   /* unused for 600 submissions: no submission in flight uses them */
    free(a.held.keys); free(a.held.vals);
}

/* ---------- texture / sampler pool slots ---------- */
typedef struct { tex_t t; view_t v; uint8_t has_v, ok; } treg;
static treg *g_treg; static int g_ntreg;
static void held_by_slots(nvn_hmap *held) {
    for (int i = 0; i < g_ntreg; i++) {
        vk_fmt f; stor_key k;
        if (g_treg[i].ok && g_treg[i].t.serial && vk_fmt_of(g_treg[i].t.d.format, &f) && key_of(&g_treg[i].t, &f, &k)) nvn_hm_put(held, &k, (void *)1);
    }
}
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_samplers = {.klen = sizeof(samp_par)};
static int g_aniso, g_minmax;

void vt_init(void) {
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(vk_phys, &f);
    g_aniso = f.features.samplerAnisotropy; g_minmax = f12.samplerFilterMinmax;
}

void vt_set_pool_n(int n) { (void)n; }
void vt_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_ntreg) {
        int n = g_ntreg ? g_ntreg : 1024; while (n <= id) n *= 2;
        g_treg = realloc(g_treg, (size_t)n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *g_treg); g_ntreg = n;
    }
    g_treg[id].t = *t; g_treg[id].has_v = v != NULL; if (v) g_treg[id].v = *v; g_treg[id].ok = 1;
    pthread_mutex_unlock(&g_rmu);
}
void vt_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id >= (1 << 12)) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_nsreg) {
        int n = g_nsreg ? g_nsreg : 256; while (n <= id) n *= 2;
        g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, (size_t)n);
        memset(g_sreg_ok + g_nsreg, 0, (size_t)(n - g_nsreg)); g_nsreg = n;
    }
    g_sreg[id] = *p; g_sreg_ok[id] = 1;
    pthread_mutex_unlock(&g_rmu);
}
/* The registered texture / sampler of a slot (copies, so the game may re-register meanwhile). */
int vt_slot_tex(int id, tex_t *t, view_t *v, int *has_v) {
    pthread_mutex_lock(&g_rmu);
    int ok = id >= 0 && id < g_ntreg && g_treg[id].ok;
    if (ok) { *t = g_treg[id].t; *v = g_treg[id].v; *has_v = g_treg[id].has_v; }
    pthread_mutex_unlock(&g_rmu);
    return ok;
}
int vt_slot_samp(int id, samp_par *p) {
    pthread_mutex_lock(&g_rmu);
    int ok = id >= 0 && id < g_nsreg && g_sreg_ok[id];
    if (ok) *p = g_sreg[id];
    pthread_mutex_unlock(&g_rmu);
    return ok;
}

static VkSamplerAddressMode wrap(int m) {
    switch (m) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   /* CLAMP (GL's edge / border blend) and CLAMP_TO_EDGE */
    }
}
VkSampler vt_sampler(const samp_par *p) {
    VkSampler s = nvn_hm_get(&g_samplers, p);
    if (s) return s;
    int mipf = p->minf >= NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST;
    VkSamplerReductionModeCreateInfo rd = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
        .reductionMode = p->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : p->reduction == NVN_SAMPLER_REDUCTION_MAX ? VK_SAMPLER_REDUCTION_MODE_MAX : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    const float *bc = p->border;
    VkBorderColor border = bc[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : bc[0] + bc[1] + bc[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = g_minmax && p->reduction ? &rd : NULL,
        .magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(p->ws), .addressModeV = wrap(p->wt), .addressModeW = wrap(p->wr), .mipLodBias = p->lod_bias,
        .anisotropyEnable = g_aniso && p->aniso > 1.0f, .maxAnisotropy = p->aniso > 16.0f ? 16.0f : p->aniso < 1.0f ? 1.0f : p->aniso,
        .compareEnable = p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE, .compareOp = (VkCompareOp)(p->cmp_func ? p->cmp_func - 1 : 7),
        .minLod = p->lod_min, .maxLod = mipf ? p->lod_max : 0.25f, .borderColor = border};
    VKC(vkCreateSampler(vk_dev, &ci, NULL, &s));
    nvn_hm_put(&g_samplers, p, s);
    return s;
}

/* ---------- CPU texel reads and writes ---------- */
static int region_layers(const vtex *e, const NVNcopyRegion *r, unsigned layer0, unsigned *l0, unsigned *nl, int *z0, int *nz) {
    if (e->type == VK_IMAGE_TYPE_3D) { *l0 = 0; *nl = 1; *z0 = r->zoffset; *nz = r->depth > 0 ? r->depth : 1; }
    else { *l0 = layer0 + (unsigned)(e->layers > 1 ? r->zoffset : 0); *nl = e->layers > 1 && r->depth > 0 ? (unsigned)r->depth : 1; *z0 = 0; *nz = 1; }
    if (e->type == VK_IMAGE_TYPE_1D && e->layers > 1) { *l0 = layer0 + (unsigned)r->yoffset; *nl = r->height > 0 ? (unsigned)r->height : 1; }
    return *l0 + *nl <= e->layers;
}

void vt_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    int nimg = r->depth > 0 ? r->depth : 1;
    pthread_mutex_lock(&vk_mu);
    vtex *e = vt_get(t);
    unsigned l0, nl; int z0, nz;
    if (!e || e->f.convert || !region_layers(e, r, layer0, &l0, &nl, &z0, &nz) || level >= e->levels) {
        pthread_mutex_unlock(&vk_mu);
        VK_ONCE("texel read of texture format %#x not possible: zeros", t->d.format);
        for (int z = 0; z < nimg; z++) for (size_t y = 0; y < hb; y++) memset(dst + (size_t)z * img + y * row, 0, wb);
        return;
    }
    VkBuffer sb; VkDeviceSize so;
    size_t n = wb * hb * (size_t)nimg;
    uint8_t *st = vk_stage(n, 16, &sb, &so);
    VkCommandBuffer c = vk_cmd();
    VkBufferImageCopy cp = {.bufferOffset = so, .imageSubresource = {e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT && e->aspect != VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : e->aspect, level, l0, nl},
                            .imageOffset = {r->xoffset, e->type == VK_IMAGE_TYPE_1D ? 0 : r->yoffset, z0},
                            .imageExtent = {(uint32_t)r->width, e->type == VK_IMAGE_TYPE_1D ? 1 : (uint32_t)r->height, (uint32_t)nz}};
    vkCmdCopyImageToBuffer(c, e->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &cp);
    vk_barrier(c);
    uint64_t f = vk_flush(NULL, NULL);
    pthread_mutex_unlock(&vk_mu);
    vk_wait_done(f);   /* the staging memory stays until the completion thread moves past f, after this returns */
    for (int z = 0; z < nimg; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, st + ((size_t)z * hb + y) * wb, wb);
}

/* ---------- write-back of rendered textures ----------
   A texture in CPU-visible pool memory that the GPU rendered into lives in its image; the memory still holds what the
   CPU or an upload put there. When the CPU invalidates a range of that memory (nvnMemoryPoolInvalidateMappedRange: it is
   about to read what the GPU wrote), the rendering is read back and swizzled into the pool, unless the CPU wrote the
   memory since the render (its bytes are newer). Of several textures over the same memory the newest wins. */
typedef struct { tex_t t; uint64_t stamp; } wb_ent;
static nvn_hmap g_wb = {.klen = 8};   /* by serial; vk_mu */
static uint64_t g_wb_done, g_wb_skipped;
void vt_note_rendered(const tex_t *t) {
    static int off = -1;   /* TOTK_WRITEBACK=0: no write-back (A/B) */
    if (off < 0) { const char *e = pal_getenv("TOTK_WRITEBACK"); off = e && *e == '0'; }
    if (off || !t || !t->serial || !tex_mirrored(t) || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return;
    uint64_t k = t->serial;
    wb_ent *e = nvn_hm_get(&g_wb, &k);
    if (e && nvn_dirty_tracked(t->mem, (size_t)t->L.size) && !nvn_dirty_since(t->mem, (size_t)t->L.size, e->stamp)) { e->stamp = nvn_dirty_now(); return; }   /* untouched since: still protected */
    if (!e) { e = malloc(sizeof *e); if (!e) return; nvn_hm_put(&g_wb, &k, e); }
    e->t = *t; e->stamp = nvn_dirty_now();
    nvn_dirty_protect(t->mem, (size_t)t->L.size);   /* after the stamp: a CPU write from here on is seen */
}
static int wb_back(const tex_t *t, uint8_t *storage) {   /* the rendering, swizzled into `storage` (a copy of the texture's memory) */
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    vk_fmt f; VkImageType type; uint32_t w, h, d, layers;
    if (!bpb || bw != 1 || bh != 1 || !vk_fmt_of(t->d.format, &f) || f.convert || f.depth || f.stencil || t->d.samples > 1) return 0;
    shape(t, &type, &w, &h, &d, &layers);
    unsigned levels = t->d.levels ? t->d.levels : 1;
    for (unsigned lv = 0; lv < levels; lv++) {
        uint32_t lw = mip(w, lv), lh = type == VK_IMAGE_TYPE_1D ? 1 : mip(h, lv), ld = type == VK_IMAGE_TYPE_3D ? mip(d, lv) : 1;
        size_t pitch = (size_t)lw * bpb, n = pitch * lh * ld;
        uint8_t *lin = malloc(n);
        if (!lin) return 0;
        for (unsigned ly = 0; ly < layers; ly++) {
            NVNcopyRegion r = {.width = (int)lw, .height = (int)lh, .depth = (int)ld};
            vt_read_texels(t, lv, ly, &r, lin, pitch, pitch * lh);
            if (nvn_swizzle(&t->d, lv, ly, lin, pitch, storage)) { free(lin); return 0; }
        }
        free(lin);
    }
    return 1;
}
void vt_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    if (!p || !p->mem) return;
    const uint8_t *a = (const uint8_t *)p->mem + off, *z = a + n;
    wb_ent *list[64]; int nl = 0;
    pthread_mutex_lock(&vk_mu);
    for (uint32_t i = 0; i < g_wb.cap && nl < 64; i++) {
        wb_ent *e = g_wb.vals ? g_wb.vals[i] : NULL;
        if (!e || (const uint8_t *)e->t.mem >= z || (const uint8_t *)e->t.mem + e->t.L.size <= a) continue;
        uint64_t k = e->t.serial; nvn_hm_del(&g_wb, &k); list[nl++] = e; i = (uint32_t)-1;   /* deletion shifts entries: rescan */
    }
    pthread_mutex_unlock(&vk_mu);
    int fresh[64];
    for (int i = 0; i < nl; i++) fresh[i] = !nvn_dirty_tracked(list[i]->t.mem, (size_t)list[i]->t.L.size) || !nvn_dirty_since(list[i]->t.mem, (size_t)list[i]->t.L.size, list[i]->stamp);
    for (int i = 0; i < nl; i++) for (int j = 0; j < nl; j++)
        if (i != j && fresh[i] && fresh[j] && list[j]->t.mem == list[i]->t.mem && list[j]->stamp > list[i]->stamp) { fresh[i] = 0; break; }   /* superseded */
    for (int i = 0; i < nl; i++) {
        const tex_t *t = &list[i]->t;
        if (!fresh[i]) { g_wb_skipped++; free(list[i]); continue; }
        /* The GPU wait happens before the pages are opened: while they are busy to the dirty tracker, the completion thread
           (events and counters it writes into pool memory) would wait for them, and this thread for the completion thread. */
        size_t size = (size_t)t->L.size;
        uint8_t *tmp = malloc(size);
        int ok = 0;
        if (tmp) {
            memcpy(tmp, t->mem, size);
            if (wb_back(t, tmp)) {
                void *tok = nvn_dirty_gpu_write_begin(t->mem, size);   /* the GPU's bytes are not a CPU write */
                memcpy(t->mem, tmp, size);
                nvn_dirty_gpu_write_end(tok);
                ok = 1;
            }
            free(tmp);
        }
        if (ok) { if (++g_wb_done <= 4) VLOG("write-back: rendered texture %ux%u format %#x written to pool memory (%llu so far, %llu skipped: the CPU wrote since)", t->d.width, t->d.height, t->d.format, (unsigned long long)g_wb_done, (unsigned long long)g_wb_skipped); }
        else VK_ONCE("write-back of texture %ux%u format %#x target %d failed: pool memory keeps its old bytes", t->d.width, t->d.height, t->d.format, t->d.target);
        free(list[i]);
    }
}

void vt_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    int nimg = r->depth > 0 ? r->depth : 1;
    pthread_mutex_lock(&vk_mu);
    vtex *e = vt_get(t);
    unsigned l0, nl; int z0, nz;
    if (!e || e->f.convert || !region_layers(e, r, layer0, &l0, &nl, &z0, &nz) || level >= e->levels) {
        pthread_mutex_unlock(&vk_mu);
        VK_ONCE("texel write to texture format %#x not possible: dropped", t->d.format);
        return;
    }
    VkBuffer sb; VkDeviceSize so;
    uint8_t *st = vk_stage(wb * hb * (size_t)nimg, 16, &sb, &so);
    for (int z = 0; z < nimg; z++) for (size_t y = 0; y < hb; y++) memcpy(st + ((size_t)z * hb + y) * wb, src + (size_t)z * img + y * row, wb);
    VkCommandBuffer c = vk_cmd();
    VkBufferImageCopy cp = {.bufferOffset = so, .imageSubresource = {e->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : e->aspect, level, l0, nl},
                            .imageOffset = {r->xoffset, e->type == VK_IMAGE_TYPE_1D ? 0 : r->yoffset, z0},
                            .imageExtent = {(uint32_t)r->width, e->type == VK_IMAGE_TYPE_1D ? 1 : (uint32_t)r->height, (uint32_t)nz}};
    vkCmdCopyBufferToImage(c, sb, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    vk_barrier(c);
    e->owned = 1;   /* the image now holds bytes the pool memory does not */
    vk_flush(NULL, NULL);
    pthread_mutex_unlock(&vk_mu);
}
