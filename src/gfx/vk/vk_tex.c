/* NVN textures as Vulkan images (upstream nvn_mtl.m "Texture -> MTLTexture" and "storage-shared GPU textures").

   An image belongs to storage: (memory, size, dimensions, levels, target, samples, format class). Every texture
   object over the same bytes with the same layout gets the same image, so what is rendered through one is seen
   through another (upstream: the deferred lighting read all-zero normals before this). Uncompressed colour formats of
   one texel size share an image (MUTABLE_FORMAT views); depth, compressed and converted formats are keyed by format.

   Mirrored textures (upstream tex_mirrored: CPU-visible pool, no render-target kind of flag, not depth / MS /
   buffer) take their bytes from pool memory: uploaded at first use, and again at the first use in a submission after
   the dirty tracker saw a write to their pages. A texture the GPU writes is owned by the GPU from then on.

   Images unused for 600 submissions (300 when their bytes are in pool memory) are dropped, except those whose content cannot be made again (rendered or copied
   into by the GPU) while a texture object still refers to them (upstream: while a serial holds the storage). Dropping
   those left a menu background, or a sky rendered once, blank when it was used again. */
#include <stdatomic.h>
#include "gfx/vk/vk.h"
#include "nvn_deswizzle.h"
#include "nvn_dirty.h"
#include "nvn_hmap.h"

int vk_astc_decode(const uint8_t *src, size_t n, unsigned bw, unsigned bh, unsigned w, unsigned h, int srgb, uint8_t *dst);   /* vk_astc.cpp */
int vk_astc_decode_bc3(const uint8_t *src, size_t n, unsigned bw, unsigned bh, unsigned w, unsigned h, int srgb, uint8_t *dst);

typedef struct { uint64_t mem, size; uint32_t w, h, d, levels, target, samples, cls, pad; } stor_key;   /* 48 bytes, no padding holes */
typedef struct { VkFormat f; VkImageViewType type; uint32_t base_level, levels, base_layer, layers; VkComponentSwizzle swz[4]; VkImageAspectFlags aspect; } view_key;
typedef struct { view_key k; VkImageView v; } view_ent;
struct vtex {
    stor_key k;
    VkImage img; VmaAllocation al; VkFormat fmt; vk_fmt f; VkImageAspectFlags aspect; VkImageCreateFlags flags;
    uint32_t w, h, d, layers, levels, tsize; VkImageType type;   /* tsize: bytes per texel (block) in the image's own format */
    int mirrored, owned, uploaded;
    unsigned drop;         /* top mip levels left out (TOTK_MIP_DROP): image level l is the game's level l + drop */
    int kind; uint64_t bytes;   /* the memory line: 0 decoded ASTC, 1 other uploaded, 2 GPU-written; allocation size */
    char writer[128];   /* what last wrote it (diagnostics: the pass log prints it for sampled textures) */
    struct {   /* the write's inputs, so F9 can follow a sampled texture back to its sources (diagnostics) */
        uint64_t present; int ndraws, nsh, nin, ndrop; char drop[96];
        char sh[4][40];
        struct { uint64_t mem; uint32_t w, h, fmt; } in[8];
    } prov;
    uint64_t stamp, checked_sub, used_sub;
    /* content check (no write protection, the Steam Deck): whole and sampled hashes of what was uploaded, when each was
       last compared; hot = changed since its first upload */
    uint64_t hash, shash, hashed_sub, sampled_sub; int hot;
    int whole_only;   /* sampling once missed a change here: hashed whole at every check from then on */
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
/* who last wrote a texture, as text: read by the pass log, captures (F9) and TOTK_PROVENANCE=1 dumps only, and
   formatting it at every pass and upload was ~4% of the Steam Deck's replay */
int vt_prov_strings(void) { extern int vk_passlog_on; static int env = -1; if (env < 0) { const char *e = pal_getenv("TOTK_PROVENANCE"); env = e && *e == '1'; } return env || vk_passlog_on; }
void vt_written(vtex *e) {
    if (!e) return;
    if (e->drop) VK_ONCE("a texture uploaded without its top mip level is written by the GPU (%s): it may look wrong; TOTK_MIP_DROP=0 avoids it", vt_writer);
    e->owned = 1;
    if (vt_prov_strings()) snprintf(e->writer, sizeof e->writer, "%s (submission %llu)", vt_writer[0] ? vt_writer : "?", (unsigned long long)vk_sub_serial());
    memset(&e->prov, 0, sizeof e->prov);
    uint64_t vk_present_count(void); e->prov.present = vk_present_count();
}
const char *vt_writer_of(const vtex *e) { return e && e->writer[0] ? e->writer : "nothing since it was made (zero) or an upload"; }

/* ---- provenance (diagnostics): what each write read, followed back from a sampled texture when F9 logs a frame ---- */
static uint64_t stor_mem(const tex_t *t);
static struct { uint64_t mem; uint32_t w, h, fmt; } g_pin[8]; static int g_npin;
void vt_prov_reset(void) { g_npin = 0; }
void vt_prov_input(const tex_t *t) {   /* a texture the coming draw samples */
    if (!t || !t->mem || !vt_prov_strings()) return;
    uint64_t m = stor_mem(t);
    for (int i = 0; i < g_npin; i++) if (g_pin[i].mem == m && g_pin[i].fmt == (uint32_t)t->d.format) return;
    if (g_npin < 8) g_pin[g_npin++] = (typeof(g_pin[0])){m, t->d.width, t->d.height, (uint32_t)t->d.format};
}
static void prov_add(vtex *e, uint64_t mem, uint32_t w, uint32_t h, uint32_t fmt) {
    for (int i = 0; i < e->prov.nin; i++) if (e->prov.in[i].mem == mem && e->prov.in[i].fmt == fmt) return;
    if (e->prov.nin < 8) e->prov.in[e->prov.nin++] = (typeof(e->prov.in[0])){mem, w, h, fmt};
}
void vt_prov_drop(vtex *e, const char *why) { if (e) { e->prov.ndrop++; snprintf(e->prov.drop, sizeof e->prov.drop, "%s", why); } }
void vt_prov_copy(vtex *e, const tex_t *src) { if (e && src) prov_add(e, stor_mem(src), src->d.width, src->d.height, (uint32_t)src->d.format); }
void vt_prov_draw(vtex *e, const char *vs, const char *fs) {   /* a draw into e (a target of the open pass) read the pending inputs */
    if (!e || !vt_prov_strings()) return;   /* diagnostics only (the chains): a string and compares at every draw */
    e->prov.ndraws++;
    char nm[40]; snprintf(nm, sizeof nm, "%.18s+%.18s", vs ? vs : "-", fs ? fs : "-");
    int have = 0; for (int i = 0; i < e->prov.nsh; i++) if (!strcmp(e->prov.sh[i], nm)) have = 1;
    if (!have && e->prov.nsh < 4) snprintf(e->prov.sh[e->prov.nsh++], sizeof e->prov.sh[0], "%s", nm);
    for (int i = 0; i < g_npin; i++) prov_add(e, g_pin[i].mem, g_pin[i].w, g_pin[i].h, g_pin[i].fmt);
}
/* Print e's chain of writes back to its sources (depth-first, each image once per call). */
void vx_note_sampled(VkImage img, VkFormat fmt, uint32_t w, uint32_t h);
static void prov_walk(const vtex *e, int depth, const vtex **seen, int *nseen) {
    for (int i = 0; i < *nseen; i++) if (seen[i] == e) { VLOG("[chain] %*s(image %p again, see above)", depth * 2, "", (void *)e); return; }
    if (*nseen < 64) seen[(*nseen)++] = e;
    VLOG("[chain] %*simage %p: storage %#llx %ux%u class %#x VK %d; last written by %s at present %llu; %d draws, %d inputs",
         depth * 2, "", (void *)e, (unsigned long long)e->k.mem, e->w, e->h, e->k.cls, (int)e->fmt, vt_writer_of(e), (unsigned long long)e->prov.present, e->prov.ndraws, e->prov.nin);
    for (int i = 0; i < e->prov.nsh; i++) VLOG("[chain] %*s  shaders %s", depth * 2, "", e->prov.sh[i]);
    if (e->prov.ndrop) VLOG("[chain] %*s  %d draws into it were DROPPED since (last: %s)", depth * 2, "", e->prov.ndrop, e->prov.drop);
    if (!vk_fmt_block_compressed(e->fmt) && !e->f.depth && !e->f.stencil) vx_note_sampled(e->img, e->fmt, e->w, e->h);   /* its contents now, logged after the pass */
    if (depth >= 10) return;
    for (int i = 0; i < e->prov.nin; i++) {
        VLOG("[chain] %*s  input %d: %#llx %ux%u NVN format %#x", depth * 2, "", i, (unsigned long long)e->prov.in[i].mem, e->prov.in[i].w, e->prov.in[i].h, e->prov.in[i].fmt);
        int found = 0;
        for (uint32_t j = 0; j < g_by_stor.cap; j++) {   /* every image over that memory (any format class) */
            const vtex *o = g_by_stor.vals[j];
            if (o && o->k.mem == e->prov.in[i].mem && o->w == e->prov.in[i].w && o->h == e->prov.in[i].h) { found = 1; prov_walk(o, depth + 1, seen, nseen); }
        }
        if (!found) VLOG("[chain] %*s    (no image over it now)", depth * 2, "");
    }
    if (!e->prov.ndraws && !e->prov.nin) {   /* only cleared: what else lives in its memory (another texture object may draw there) */
        int n = 0;
        for (uint32_t j = 0; j < g_by_stor.cap && n < 16; j++) {
            const vtex *o = g_by_stor.vals[j];
            if (!o || o == e || o->k.mem >= e->k.mem + e->k.size || e->k.mem >= o->k.mem + o->k.size) continue;
            n++;
            VLOG("[chain] %*s  overlaps image %p: storage %#llx (%llu bytes) %ux%u levels %u class %#x VK %d; last written by %s at present %llu; %d draws, %d inputs",
                 depth * 2, "", (void *)o, (unsigned long long)o->k.mem, (unsigned long long)o->k.size, o->w, o->h, o->levels, o->k.cls, (int)o->fmt, vt_writer_of(o), (unsigned long long)o->prov.present, o->prov.ndraws, o->prov.nin);
            for (int i = 0; i < o->prov.nsh; i++) VLOG("[chain] %*s    shaders %s", depth * 2, "", o->prov.sh[i]);
        }
        if (!n) VLOG("[chain] %*s  (nothing else lives in its memory)", depth * 2, "");
    }
}
void vt_prov_dump(const vtex *e) {
    static const vtex *done[256]; static int ndone; static uint64_t at;
    uint64_t vk_present_count(void);
    if (at != vk_present_count()) { at = vk_present_count(); ndone = 0; }
    for (int i = 0; i < ndone; i++) if (done[i] == e) return;   /* each sampled image once per logged frame */
    if (ndone < 256) done[ndone++] = e;
    const vtex *seen[64]; int nseen = 0;
    prov_walk(e, 0, seen, &nseen);
}

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
/* The memory an image is keyed by: in a virtual pool, the physical pages behind it (nvn_vpool.c), so two virtual views
   of one page share the image and a remapped page is new storage; the image then also outlives remaps of the virtual
   range (the backend drops the images over a virtual run when the game maps pages next to it). TOTK_VPOOL_PHYS=0: the
   virtual address (A/B). */
uint64_t totk_vpool_physical(const void *mem, uint64_t size);
static uint64_t stor_mem(const tex_t *t) {
    static int on = -1; if (on < 0) { const char *e = pal_getenv("TOTK_VPOOL_PHYS"); on = !e || atoi(e); }
    if (on && t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) { uint64_t p = totk_vpool_physical(t->mem, t->L.size);
        static int n; if (n < 8) { n++; VLOG("texture %ux%u NVN format %#x at virtual %p: %s %#llx", t->d.width, t->d.height, t->d.format, (void *)t->mem, p ? "keyed by physical" : "pages not contiguous / unmapped, keyed by virtual", (unsigned long long)p); }
        if (p) return p; }
    return (uint64_t)(uintptr_t)t->mem;
}
static int key_of(const tex_t *t, const vk_fmt *f, stor_key *k) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!t->mem || !t->L.size || !bpb) return 0;
    memset(k, 0, sizeof *k);
    k->mem = stor_mem(t); k->size = t->L.size;
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

/* Large uploaded textures may leave out their top mip level (upstream does this on iPhone, nvn_mtl.m mip_drop_of): a
   quarter of the memory. The game samples them with normalised coordinates, so a texture one level smaller is drawn from
   what was its level 1. Only textures uploaded from pool memory, decoded from ASTC (the GPU never renders into those),
   2D / array / cube, with at least two levels and both sides >= TOTK_MIP_DROP_MIN (512). TOTK_MIP_DROP=0 off, =1 on;
   default on when the GPU shares system RAM or has under 6 GiB of its own (the Steam Deck, laptops). */
static uint64_t g_kind_bytes[3], g_drop_saved;
static unsigned mip_drop_of(const tex_t *t, const vk_fmt *f) {
    static int on = -1; static unsigned minside;
    if (on < 0) { const char *e = pal_getenv("TOTK_MIP_DROP"), *m = pal_getenv("TOTK_MIP_DROP_MIN");
        on = e ? atoi(e) != 0 : vk_integrated || vk_local_heap < (6ull << 30); minside = m ? (unsigned)atoi(m) : pal_total_memory() && pal_total_memory() < (12ull << 30) ? 256 : 512;   /* 256 under 12 GiB of memory */
        VLOG("large decoded ASTC textures leave out their top mip level: %s (both sides >= %u; TOTK_MIP_DROP=0/1)", on ? "on" : "off", minside); }
    unsigned g = (unsigned)t->d.target;
    if (!on || (f->convert != 3 && f->convert != 5) || !tex_mirrored(t) || t->d.levels < 2 || t->d.width < minside || t->d.height < minside) return 0;
    return g == NVN_TEXTURE_TARGET_2D || g == NVN_TEXTURE_TARGET_2D_ARRAY || g == NVN_TEXTURE_TARGET_CUBEMAP || g == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
}
void vt_memory(uint64_t out[4]) { out[0] = g_kind_bytes[0]; out[1] = g_kind_bytes[1]; out[2] = g_kind_bytes[2]; out[3] = g_drop_saved; }
static uint64_t g_images, g_image_bytes, g_uploads, g_upload_bytes, g_upload_ns, g_reuploads, g_untracked;
static vtex *create(const tex_t *t, const vk_fmt *f, const stor_key *k) {
    vtex *e = calloc(1, sizeof *e);
    e->k = *k; e->f = *f; e->fmt = f->vk; e->levels = k->levels;
    { int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
      e->tsize = f->convert == 1 || f->convert == 3 ? 4 : f->convert == 2 ? (uint32_t)bpb / 3 * 4 : f->convert == 4 ? 8 : f->convert == 5 ? 16 : (uint32_t)bpb; }
    shape(t, &e->type, &e->w, &e->h, &e->d, &e->layers);
    if ((e->drop = mip_drop_of(t, f))) { e->w = e->w >> 1 ? e->w >> 1 : 1; e->h = e->h >> 1 ? e->h >> 1 : 1; e->levels -= 1; }
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
    if (fl & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) {
        /* Views in other formats of the class (an UNORM storage view of an sRGB image, an RGBA16UI target view of BC
           blocks) need their usage on the image too: whatever any compatible format allows (EXTENDED_USAGE). */
        VkImageUsageFlags more = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (k->samples == 1 ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        VkImageFormatProperties ip;
        for (VkImageUsageFlags bit = VK_IMAGE_USAGE_SAMPLED_BIT; bit <= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; bit <<= 1)   /* each extra one the GPU takes */
            if ((more & bit) && !(usage & bit) &&
                vkGetPhysicalDeviceImageFormatProperties(vk_phys, e->fmt, e->type, VK_IMAGE_TILING_OPTIMAL, usage | bit, fl, &ip) == VK_SUCCESS &&
                (ip.sampleCounts & k->samples) && ip.maxArrayLayers >= e->layers && ip.maxMipLevels >= e->levels) usage |= bit;
    }
    e->flags = fl;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = fl, .imageType = e->type, .format = e->fmt,
        .extent = {e->w, e->h, e->d}, .mipLevels = e->levels, .arrayLayers = e->layers, .samples = (VkSampleCountFlagBits)k->samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ac = {.usage = vk_prefer_device()};
    VkResult r = vmaCreateImage(vk_vma, &ci, &ac, &e->img, &e->al, NULL);
    if (r != VK_SUCCESS) {
        VLOG("image %ux%ux%u x%u levels %u format %d (NVN %#x target %d) not created (%d)", e->w, e->h, e->d, e->layers, e->levels,
             (int)e->fmt, t->d.format, t->d.target, (int)r);
        free(e); return NULL;
    }
    e->mirrored = tex_mirrored(t);
    { VmaAllocationInfo ai; vmaGetAllocationInfo(vk_vma, e->al, &ai); e->bytes = ai.size;
      e->kind = f->convert == 3 || f->convert == 5 ? 0 : e->mirrored ? 1 : 2; g_kind_bytes[e->kind] += e->bytes;
      if (e->drop) g_drop_saved += e->bytes * 3; }   /* the left-out level is about three times the rest */
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
    g_kind_bytes[e->kind] -= e->bytes; if (e->drop) g_drop_saved -= e->bytes * 3;
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

static uint16_t half_of_int(int v) {   /* exact for |v| <= 2048 */
    if (!v) return 0;
    unsigned m = (unsigned)(v < 0 ? -v : v), e = 0;
    while (m >> (e + 1)) e++;
    return (uint16_t)((v < 0 ? 0x8000u : 0) | (e + 15) << 10 | ((m << (10 - e)) & 0x3ffu));
}
/* RGB10A2 signed kinds (0x69 SNORM, 0x6A SINT, 0x6C I2F) -> RGBA16 SNORM / SINT / SFLOAT */
static void unpack_10a2s(uint16_t *dst, const uint32_t *src, size_t texels, int format) {
    for (size_t i = 0; i < texels; i++) for (int c = 0; c < 4; c++) {
        int bits = c < 3 ? 10 : 2, v = (int32_t)(src[i] << (32 - 10 * c - bits)) >> (32 - bits);   /* sign-extended component c */
        int m = (1 << (bits - 1)) - 1;   /* the largest positive value: 511, 1 */
        if (format == 0x69) { if (v < -m) v = -m; v = (v * 32767 + (v < 0 ? -m / 2 : m / 2)) / m; }
        dst[4 * i + c] = format == 0x6C ? half_of_int(v) : (uint16_t)(int16_t)v;
    }
}

/* One layer of a level, as linear blocks in NVN's format (rows of whole blocks, `slice` bytes per depth slice): into
   staging memory in the image's format (convert: vk_fmt.c), copied to (level, layer) at o, w x h x d texels, with c.
   *out / *out_n: the staged bytes of the main plane (diagnostics). */
static void put_linear(vtex *e, int format, const uint8_t *lin, size_t slice, uint32_t w, uint32_t h, uint32_t d,
                       VkCommandBuffer c, uint32_t level, uint32_t layer, VkOffset3D o, uint8_t **out, size_t *out_n) {
    int bw, bh, bpb = nvn_fmt_bpb(format, &bw, &bh);
    size_t n = slice * d, texels = (size_t)w * h * d;
    size_t dn = e->f.convert == 2 ? n / 3 * 4 : e->f.convert == 3 ? texels * 4 : e->f.convert == 4 ? n * 2
              : e->f.convert == 5 ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16 * d : n;
    if (e->f.convert == 5 && ((o.x | o.y) & 3)) {   /* BC3 blocks start every 4 texels: a region elsewhere cannot be written */
        VK_ONCE("ASTC texture region at %d,%d (not on a 4-texel boundary) kept in BC3: left out", o.x, o.y);
        if (out) { *out = NULL; *out_n = 0; }
        return;
    }
    VkBuffer sb; VkDeviceSize so;
    uint8_t *dst = vk_stage(dn, 16, &sb, &so);
    if (e->f.convert == 5) {
        size_t bs = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
        for (uint32_t z = 0; z < d; z++)
            if (vk_astc_decode_bc3(lin + z * slice, slice, (unsigned)bw, (unsigned)bh, w, h, format >= 0x87, dst + z * bs)) {
                VK_ONCE("ASTC %dx%d texture %ux%u did not decode: left blank", bw, bh, w, h);
                memset(dst + z * bs, 0, bs);
            }
    } else if (e->f.convert == 3) {
        for (uint32_t z = 0; z < d; z++)
            if (vk_astc_decode(lin + z * slice, slice, (unsigned)bw, (unsigned)bh, w, h, format >= 0x87, dst + (size_t)z * w * h * 4)) {
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
            VkBufferImageCopy rs = {.bufferOffset = so2, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, level, layer, 1}, .imageOffset = o, .imageExtent = {w, h, d}};
            vkCmdCopyBufferToImage(c, sb2, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
        }
    } else if (e->f.convert == 2) {
        int is_int = (format >= 0x1B && format <= 0x1C) || (format >= 0x20 && format <= 0x21) || (format >= 0x23 && format <= 0x24);
        expand_rgb(dst, lin, texels, (unsigned)bpb / 3, is_int);
    } else if (e->f.convert == 4) unpack_10a2s((uint16_t *)dst, (const uint32_t *)lin, n / 4, format);
    else memcpy(dst, lin, n);
    VkBufferImageCopy r = {.bufferOffset = so, .imageSubresource = {e->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT, level, layer, 1}, .imageOffset = o, .imageExtent = {w, h, d}};
    vkCmdCopyBufferToImage(c, sb, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    if (out) { *out = dst; *out_n = dn; }
}

int vt_converted(const vtex *e) { return e->f.convert != 0; }
uint32_t vt_texel_size(const vtex *e) { return e->tsize; }
void vt_copy_linear(vtex *e, const tex_t *t, uint32_t level, uint32_t layer0, uint32_t layers, VkOffset3D o, VkExtent3D x, const uint8_t *src, size_t row) {
    if (level < e->drop) return;   /* a left-out level */
    level -= e->drop;              /* the image's level of the same size */
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t pitch = (size_t)((x.width + bw - 1) / bw) * bpb, rows = (x.height + bh - 1) / bh, slice = pitch * rows;
    uint8_t *lin = malloc(slice * x.depth);
    if (!lin) die("texture copy: %zu bytes", slice * x.depth);
    for (uint32_t l = 0; l < layers; l++) {
        for (size_t y = 0; y < rows * x.depth; y++) memcpy(lin + y * pitch, src + ((size_t)l * rows * x.depth + y) * row, pitch);
        put_linear(e, t->d.format, lin, slice, x.width, x.height, x.depth, vk_cmd(), level, layer0 + l, o, NULL, NULL);
    }
    free(lin);
}

/* Content check, used when pages are not write-protected (Wine / Proton; nvn_dirty.c). Every texture used by a
   submission is compared with what was uploaded: whole when it is up to 16 KiB or was rewritten lately ("hot": the
   game's font atlases get glyphs written into them a few at a time, and sampling alone missed those: blank and
   clipped dialogue on the Steam Deck); otherwise ("cold") looked at every 4th submission only, sampled (64 bytes of
   every 4 KiB) with a whole hash every 16th (64th above 1 MiB), at a phase of its own so that they do not all come at
   once. Checking every used texture at each submission was half the replay: whole hashes read about 50 MB a frame,
   and sampling touches every page of hundreds of MB (a cache and TLB miss each).
   A texture stays hot for 64 unchanged checks. vk_ns_hash (the 10-second line) is the time spent here.
   shortcut: the first rewrite of a cold texture is seen up to 4 submissions late (16 or 64 when sampling misses it); upgrade to
   Linux soft-dirty bits (/proc/self/pagemap) if that shows up. */
uint64_t vk_ns_hash;
int nvn_dirty_protecting(void);
static inline uint64_t rotl64(uint64_t x, int r) { return x << r | x >> (64 - r); }
static uint64_t hash_whole(const uint8_t *p, uint64_t n) {   /* eight independent add-rotate lanes: memory speed (2.2x the multiply lanes) */
    uint64_t h[8] = {1, 2, 3, 4, 5, 6, 7, 8}, v, i = 0;
    for (; i + 64 <= n; i += 64) for (int k = 0; k < 8; k++) { memcpy(&v, p + i + 8 * k, 8); h[k] = rotl64(h[k], 7) + (v ^ i); }
    uint64_t r = 0x9E3779B97F4A7C15ull ^ n;
    for (; i < n; i++) r = (r ^ p[i]) * 0x100000001b3ull;
    for (int k = 0; k < 8; k++) { r = (r ^ h[k]) * 0xff51afd7ed558ccdull; r ^= r >> 29; }
    return r;
}
/* Large textures are hashed in HASH_PARTS pieces at once, on helper threads (the replay thread does the first piece):
   whole hashes were ~40% of the replay without write protection (PC with TOTK_DIRTY_PROTECT=0, Steam Deck: ~11%). The
   result depends only on the bytes, never on the threads. */
enum { HASH_PARTS = 4, HASH_SPLIT = 1u << 20 };
static pthread_mutex_t g_hp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hp_go = PTHREAD_COND_INITIALIZER, g_hp_done = PTHREAD_COND_INITIALIZER;
static const uint8_t *g_hp_p[HASH_PARTS]; static uint64_t g_hp_n[HASH_PARTS], g_hp_h[HASH_PARTS];
static int g_hp_left, g_hp_next = HASH_PARTS, g_hp_threads = -1;   /* g_hp_next: the next piece to take (HASH_PARTS: none) */
static void *hash_worker(void *unused) {
    (void)unused; pal_thread_set_name("texture hash");
    pthread_mutex_lock(&g_hp_mu);
    for (;;) {
        while (g_hp_next >= HASH_PARTS) pthread_cond_wait(&g_hp_go, &g_hp_mu);
        int i = g_hp_next++;
        pthread_mutex_unlock(&g_hp_mu);
        uint64_t h = hash_whole(g_hp_p[i], g_hp_n[i]);
        pthread_mutex_lock(&g_hp_mu);
        g_hp_h[i] = h;
        if (!--g_hp_left) pthread_cond_signal(&g_hp_done);
    }
    return NULL;
}
static uint64_t hash_content(const uint8_t *p, uint64_t n) {   /* vk_mu held: one at a time */
    if (n < HASH_SPLIT) return hash_whole(p, n);
    if (g_hp_threads < 0) {
        g_hp_threads = 0;
        for (int i = 0; i < HASH_PARTS - 1; i++) { pthread_t t; if (!pthread_create(&t, NULL, hash_worker, NULL)) { pthread_detach(t); g_hp_threads++; } }
    }
    uint64_t part = (n / HASH_PARTS) & ~(uint64_t)63, at = 0;
    pthread_mutex_lock(&g_hp_mu);
    for (int i = 0; i < HASH_PARTS; i++) { g_hp_p[i] = p + at; g_hp_n[i] = i == HASH_PARTS - 1 ? n - at : part; at += part; }
    g_hp_left = HASH_PARTS; g_hp_next = 0;
    if (g_hp_threads) pthread_cond_broadcast(&g_hp_go);
    while (g_hp_next < HASH_PARTS) {   /* this thread takes pieces too (all of them without helpers) */
        int i = g_hp_next++;
        pthread_mutex_unlock(&g_hp_mu);
        uint64_t h = hash_whole(g_hp_p[i], g_hp_n[i]);
        pthread_mutex_lock(&g_hp_mu);
        g_hp_h[i] = h; g_hp_left--;
    }
    while (g_hp_left) pthread_cond_wait(&g_hp_done, &g_hp_mu);
    uint64_t r = 0x9E3779B97F4A7C15ull ^ n;
    for (int i = 0; i < HASH_PARTS; i++) { r = (r ^ g_hp_h[i]) * 0xff51afd7ed558ccdull; r ^= r >> 29; }
    pthread_mutex_unlock(&g_hp_mu);
    return r;
}
static uint64_t hash_sampled(const uint8_t *p, uint64_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, v;
    for (uint64_t at = 0; at + 64 <= n; at += 4096)
        for (int k = 0; k < 64; k += 8) { memcpy(&v, p + at + k, 8); h = (h ^ v) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    return h;
}
static void content_note(vtex *e, const tex_t *t, uint64_t sub) {   /* at upload */
    uint64_t t0 = pal_now_ns();
    e->hash = hash_content(t->mem, t->L.size);
    e->shash = hash_sampled(t->mem, t->L.size);
    e->hashed_sub = e->sampled_sub = sub;
    vk_ns_hash += pal_now_ns() - t0;
}
static int content_changed(vtex *e, const tex_t *t, uint64_t sub) {
    uint64_t t0 = pal_now_ns(), n = t->L.size;
    unsigned every = n > (1u << 20) ? 64 : 16, phase = (unsigned)((uintptr_t)e >> 6);
    /* 8-bit textures up to 16 MiB (the font glyph caches: a few glyphs written at a time, which sampling misses: letters
       and whole save-screen text missing on the Steam Deck) and textures up to 64 KiB are hashed whole at every check,
       never sampled. Up to 1 MiB were before (save thumbnails, UI): ~150 a frame, 4 GB per 10 s, the largest part of
       the Steam Deck's replay; a change sampling misses is caught by the next whole hash and the texture is then always
       hashed whole (whole_only). */
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    int glyphs = (bpb == 1 && bw == 1 && bh == 1 && n <= (16u << 20)) || e->whole_only;   /* TOTK's save-menu font: 2048x4096, 8 MiB */
    /* cold: looked at every 4th submission. Not glyph caches: text drawn once into a cached image (menus, dialogue)
       in a submission that missed new glyphs kept those letters missing on the Steam Deck. */
    if (n > (16u << 10) && !e->hot && !glyphs && (sub + phase) % 4) return 0;
    int whole = n <= (64u << 10) || glyphs || e->hot || (sub + phase) % every == 0;
    {   /* TOTK_DIAG: whole-hashed bytes by reason, every 10 s */
        static uint64_t by[5], cnt[5], last; static int diag = -1;
        if (diag < 0) { const char *d = pal_getenv("TOTK_DIAG"); diag = d && *d == '1'; }
        if (diag && whole) { int r = e->whole_only ? 0 : glyphs ? 1 : e->hot ? 2 : n <= (64u << 10) ? 3 : 4; by[r] += n; cnt[r]++; }
        uint64_t now = pal_now_ns();
        if (diag && now - last > 10000000000ull) {
            if (last) VLOG("[tex] whole hashes (last 10 s): missed-by-sampling %llu MiB (%llu), 8-bit %llu MiB (%llu), recently changed %llu MiB (%llu), small %llu MiB (%llu), periodic %llu MiB (%llu)",
                           (unsigned long long)(by[0] >> 20), (unsigned long long)cnt[0], (unsigned long long)(by[1] >> 20), (unsigned long long)cnt[1], (unsigned long long)(by[2] >> 20), (unsigned long long)cnt[2],
                           (unsigned long long)(by[3] >> 20), (unsigned long long)cnt[3], (unsigned long long)(by[4] >> 20), (unsigned long long)cnt[4]);
            memset(by, 0, sizeof by); memset(cnt, 0, sizeof cnt); last = now;
        }
    }
    int changed = whole ? hash_content(t->mem, n) != e->hash : hash_sampled(t->mem, n) != e->shash;
    /* a whole hash saw a change that sampling would have missed (a few texels written: glyphs in a large atlas,
       broken save-menu text on the Steam Deck): this texture is never sampled again */
    if (changed && whole && !e->whole_only && n > (16u << 10) && hash_sampled(t->mem, n) == e->shash) {
        e->whole_only = 1;
        static int said; if (said++ < 40) VLOG("[tex] sampling missed a change in a %ux%u texture (NVN format %#x, %llu KiB): hashed whole from now on",
                                         t->d.width, t->d.height, (unsigned)t->d.format, (unsigned long long)(n >> 10));
    }
    if (changed) e->hot = 64; else if (e->hot) e->hot--;
    vk_ns_hash += pal_now_ns() - t0;
    return changed;
}
static void upload(vtex *e, const tex_t *t) {
    uint64_t t0 = pal_now_ns();
    g_uploads++; g_upload_bytes += t->L.size; if (e->uploaded) g_reuploads++;
    if (!nvn_dirty_tracked(t->mem, (size_t)t->L.size)) g_untracked++;
    e->stamp = nvn_dirty_now();
    if (vt_prov_strings()) snprintf(e->writer, sizeof e->writer, "upload from pool memory (submission %llu)", (unsigned long long)vk_sub_serial());
    nvn_dirty_protect(t->mem, (size_t)t->L.size);   /* before copying: a write from here on is seen at the next check */
    if (!nvn_dirty_protecting()) content_note(e, t, vk_frame_serial());
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    VkCommandBuffer c = vk_pre();
    for (unsigned lv = 0; lv < e->levels; lv++) {
        uint32_t w, h, d; vt_size(e, lv, &w, &h, &d);
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh, slice = pitch * rows;
        for (unsigned ly = 0; ly < e->layers; ly++) {
            size_t n = slice * d;
            uint8_t *lin = malloc(n);
            if (!lin) die("texture upload: %zu bytes", n);
            if (nvn_deswizzle(&t->d, lv + e->drop, ly, t->mem, lin, pitch)) {   /* the game's level lv + drop */
                VK_ONCE("texture %ux%u format %#x target %d flags %#x: layout not supported for upload", t->d.width, t->d.height, t->d.format, t->d.target, t->d.flags);
                free(lin); goto done;
            }
            uint8_t *dst; size_t out_n;
            put_linear(e, t->d.format, lin, slice, w, h, d, c, lv, ly, (VkOffset3D){0, 0, 0}, &dst, &out_n);
            { static int texlog = -1, nlog;   /* TOTK_TEXLOG=1: the mean colour of what is uploaded (4-byte texels), to see what a texture holds without looking */
              if (texlog < 0) { const char *e = pal_getenv("TOTK_TEXLOG"); texlog = e && *e == '1'; }
              if (texlog && lv == 0 && ly == 0 && out_n == (size_t)w * h * 4 && nlog < 400) { nlog++;
                  uint64_t sum[4] = {0, 0, 0, 0}; for (size_t i = 0; i < (size_t)w * h; i++) for (int c = 0; c < 4; c++) sum[c] += dst[i * 4 + c];
                  double nn = (double)w * h * 255.0;
                  VLOG("[tex] upload %ux%u NVN format %#x (Vulkan %d) mean rgba %.3f %.3f %.3f %.3f", w, h, (unsigned)t->d.format, (int)e->fmt, sum[0] / nn, sum[1] / nn, sum[2] / nn, sum[3] / nn); } }
            free(lin);
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
    uint64_t sub = vk_frame_serial();
    e->used_sub = sub;
    { stor_key *held = nvn_hm_get(&g_ser, &t->serial);
      if (!held) { held = malloc(sizeof *held); if (held) { *held = e->k; nvn_hm_put(&g_ser, &t->serial, held); } }
      else if (memcmp(held, &e->k, sizeof *held)) *held = e->k; }
    if (e->mirrored && !e->owned && e->checked_sub != sub) {
        e->checked_sub = sub;
        if (!e->uploaded || (nvn_dirty_protecting() ? nvn_dirty_since(t->mem, (size_t)t->L.size, e->stamp) : content_changed(e, t, sub))) upload(e, t);
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
    k.levels = attachment ? 1 : v && v->set_levels && v->nlevels ? v->nlevels : e->levels + e->drop - k.base_level;
    if (e->drop) {   /* the game's levels -> the image's: the left-out top level reads as the next one */
        if (k.base_level >= e->drop) k.base_level -= e->drop;
        else { if (v && v->set_levels && v->nlevels) k.levels = k.levels > e->drop - k.base_level ? k.levels - (e->drop - k.base_level) : 1; else k.levels = e->levels; k.base_level = 0; }
    }
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
        if (e->type == VK_IMAGE_TYPE_3D) {   /* slices of a 3D target: a 2D array view of them (the image is 2D_ARRAY_COMPATIBLE).
               Slice 0 only made a colour-grading LUT rendered slice by slice (one target per slice) hold its last slice in
               slice 0: black graded to bright blue behind every menu. */
            uint32_t dz = mip(e->d, k.base_level), z0 = v && v->set_layers ? v->min_layer : 0;
            if (z0 >= dz) z0 = dz - 1;
            uint32_t nz = v && v->set_layers && v->nlayers ? v->nlayers : dz - z0;
            if (z0 + nz > dz) nz = dz - z0;
            k.base_layer = z0; k.layers = nz;
        }
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
    /* an image whose bytes are in pool memory is uploaded again when needed: dropped after 300 unused submissions (10 s
       at 30 fps), the others after 600 */
    if (e->used_sub + 300 >= a->now) return 0;   /* 120 kept the PC from ever loading a save: not lower */
    if (e->mirrored && !e->owned) return 1;
    if (e->used_sub + 600 >= a->now) return 0;
    return !nvn_hm_get(&a->held, &e->k);
}
/* The memory guard (vk_dev.c) over its budget: re-uploadable images not used in the last 120 submissions go now (only
   then: freeing them that early in normal play kept the game from loading a save). */
int vt_pressure;
static uint64_t g_pressure_freed;
static int pressure_idle(const vtex *e, const void *arg) {
    const idle_arg *a = arg;
    if (!(e->mirrored && !e->owned) || e->used_sub + 120 >= a->now) return 0;
    g_pressure_freed += e->bytes;
    return 1;
}
void vt_end_submission(void) {
    uint64_t now = vk_frame_serial();
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
    if (vt_pressure) {
        g_pressure_freed = 0;
        drop_where(pressure_idle, &a, 1);
        static int n; if (n++ < 20 || n % 100 == 0) VLOG("memory guard: over budget, %.0f MiB of re-uploadable textures freed (%d times so far)", g_pressure_freed / 1048576.0, n);
    }
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
/* The registered slots: read by every draw of the replay (shared), written when the game registers (alone). A slim
   reader-writer lock: winpthreads mutexes were ~15% of the replay on the PC. g_treg_ver counts registrations. */
static pal_mutex g_rmu;
static _Atomic uint64_t g_treg_ver;
uint64_t vt_treg_version(void) { return atomic_load_explicit(&g_treg_ver, memory_order_relaxed); }
static nvn_hmap g_samplers = {.klen = sizeof(samp_par)};
static int g_aniso, g_minmax;

void vt_init(void) {
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(vk_phys, &f);
    g_aniso = f.features.samplerAnisotropy; g_minmax = f12.samplerFilterMinmax;
}

void vt_set_pool_n(int n) { (void)n; }
/* Readers (every texture and sampler binding of every draw) take no lock: a sequence count, odd while a writer is at
   work, and a read that saw it change is made again. Under Wine the shared lock was ~10% of the Steam Deck's replay
   (the game registers textures from other threads all the time, and each meeting is a wait and a wake). Arrays that
   grow are not freed: a reader may still be copying from the old one (its read is then made again). */
static _Atomic uint64_t g_rseq;
static void wr_begin(void) { atomic_fetch_add_explicit(&g_rseq, 1, memory_order_acq_rel); atomic_thread_fence(memory_order_release); }
static void wr_end(void) { atomic_thread_fence(memory_order_release); atomic_fetch_add_explicit(&g_rseq, 1, memory_order_release); }
void vt_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) return;
    pal_mutex_lock(&g_rmu);
    if (id >= g_ntreg) {
        int n = g_ntreg ? g_ntreg : 1024; while (n <= id) n *= 2;
        treg *nt = calloc((size_t)n, sizeof *nt); if (!nt) die("texture slots");
        if (g_ntreg) memcpy(nt, g_treg, (size_t)g_ntreg * sizeof *nt);
        wr_begin(); g_treg = nt; g_ntreg = n; wr_end();   /* the old array stays (a reader may be in it) */
    }
    wr_begin();
    g_treg[id].t = *t; g_treg[id].has_v = v != NULL; if (v) g_treg[id].v = *v; g_treg[id].ok = 1;
    wr_end();
    atomic_fetch_add_explicit(&g_treg_ver, 1, memory_order_relaxed);
    pal_mutex_unlock(&g_rmu);
}
void vt_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id >= (1 << 12)) return;
    pal_mutex_lock(&g_rmu);
    if (id >= g_nsreg) {
        int n = g_nsreg ? g_nsreg : 256; while (n <= id) n *= 2;
        samp_par *ns = calloc((size_t)n, sizeof *ns); uint8_t *nok = calloc((size_t)n, 1); if (!ns || !nok) die("sampler slots");
        if (g_nsreg) { memcpy(ns, g_sreg, (size_t)g_nsreg * sizeof *ns); memcpy(nok, g_sreg_ok, (size_t)g_nsreg); }
        wr_begin(); g_sreg = ns; g_sreg_ok = nok; g_nsreg = n; wr_end();
    }
    wr_begin(); g_sreg[id] = *p; g_sreg_ok[id] = 1; wr_end();
    atomic_fetch_add_explicit(&g_treg_ver, 1, memory_order_relaxed);
    pal_mutex_unlock(&g_rmu);
}
/* The registered texture / sampler of a slot (copies, so the game may re-register meanwhile). */
int vt_slot_tex(int id, tex_t *t, view_t *v, int *has_v) {
    for (;;) {
        uint64_t s = atomic_load_explicit(&g_rseq, memory_order_acquire);
        if (s & 1) { pal_yield(); continue; }
        treg *r = g_treg; int n = g_ntreg;
        int ok = id >= 0 && id < n && r[id].ok;
        if (ok) { *t = r[id].t; *v = r[id].v; *has_v = r[id].has_v; }
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_rseq, memory_order_relaxed) == s) return ok;
    }
}
int vt_slot_samp(int id, samp_par *p) {
    for (;;) {
        uint64_t s = atomic_load_explicit(&g_rseq, memory_order_acquire);
        if (s & 1) { pal_yield(); continue; }
        const samp_par *r = g_sreg; const uint8_t *ok_ = g_sreg_ok; int n = g_nsreg;
        int ok = id >= 0 && id < n && ok_[id];
        if (ok) *p = r[id];
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_rseq, memory_order_relaxed) == s) return ok;
    }
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
    /* Without exact borders, upstream's rule: a depth compare reads R, so (1,0,0,1) must stay white (lit), not black */
    VkBorderColor border = bc[3] > 0.5f ? (bc[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK) : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    int fixed = !memcmp(bc, (float[4]){0, 0, 0, 0}, 16) || !memcmp(bc, (float[4]){0, 0, 0, 1}, 16) || !memcmp(bc, (float[4]){1, 1, 1, 1}, 16);
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT, .pNext = g_minmax && p->reduction ? &rd : NULL};
    static int n_custom;   /* samplers are cached per parameters, so few; the driver caps how many may exist */
    if (vk_custom_border && !fixed && n_custom < 1024) { memcpy(cb.customBorderColor.float32, bc, 16); border = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; n_custom++; }
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = border == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ? (void *)&cb : g_minmax && p->reduction ? (void *)&rd : NULL,
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

typedef struct { const uint8_t *st; uint8_t *dst; size_t row, img, wb, hb; int nimg; } texel_out;
static void texels_out(void *arg) {
    const texel_out *o = arg;
    for (int z = 0; z < o->nimg; z++) for (size_t y = 0; y < o->hb; y++)
        memcpy(o->dst + (size_t)z * o->img + y * o->row, o->st + ((size_t)z * o->hb + y) * o->wb, o->wb);
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
    /* Copied out by the completion thread, before it recycles the submission's staging chunks (and before vk_wait_done
       returns): read after the wait, the chunk was already reused or freed (garbage read-backs; on the Steam Deck, with
       chunks of their own, a crash reading unmapped memory). */
    texel_out o = {st, dst, row, img, wb, hb, nimg};
    vk_after(texels_out, &o);
    uint64_t f = vk_flush(NULL, NULL);
    pthread_mutex_unlock(&vk_mu);
    vk_wait_done(f);
}

/* ---------- write-back of rendered textures ----------
   A texture in CPU-visible pool memory that the GPU rendered into lives in its image; the memory still holds what the
   CPU or an upload put there. When the CPU invalidates a range of that memory (nvnMemoryPoolInvalidateMappedRange: it is
   about to read what the GPU wrote), the rendering is read back and swizzled into the pool, unless the CPU wrote the
   memory since the render (its bytes are newer). Of several textures over the same memory the newest wins. */
typedef struct { tex_t t; uint64_t stamp; } wb_ent;
int runtime_core_away(void);
void runtime_core_back(int *away);
static nvn_hmap g_wb = {.klen = 8};   /* by serial; g_wb_mu (inside vk_mu where both are held) */
static pthread_mutex_t g_wb_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_wb_done, g_wb_skipped;
/* Eager write-back: a small texture the CPU has read back before (TOTK: 1x1 targets, ~13 a second at the title) is
   copied out in the submission that renders it and written to pool memory when that submission completes, before
   its fence counts as done. The game's invalidate then finds nothing left to do; reading it back there waited for the
   translation thread's whole submission and then for the GPU (~4 ms a call on the PC). g_wb_mu. */
#define EAGER_MAX (64u << 10)
static uint64_t g_rb_hot[64]; static unsigned g_rb_hot_n;   /* serials read back lately (a ring) */
static tex_t g_eager[32]; static int g_neager;               /* rendered this submission, to copy out at its end */
static int rb_hot(uint64_t serial) { for (unsigned i = 0; i < 64; i++) if (g_rb_hot[i] == serial) return 1; return 0; }
static int eager_ok(const tex_t *t) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); vk_fmt f;
    return bpb && bw == 1 && bh == 1 && t->L.size <= EAGER_MAX && (t->d.levels <= 1) && t->L.layers <= 1 && t->d.samples <= 1 &&
           vk_fmt_of(t->d.format, &f) && !f.convert && !f.depth && !f.stencil;
}
void vt_note_rendered(const tex_t *t) {
    static int off = -1;   /* TOTK_WRITEBACK=0: no write-back (A/B) */
    if (off < 0) { const char *e = pal_getenv("TOTK_WRITEBACK"); off = e && *e == '0'; }
    if (off || !t || !t->serial || !tex_mirrored(t) || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return;
    uint64_t k = t->serial;
    pthread_mutex_lock(&g_wb_mu);
    wb_ent *e = nvn_hm_get(&g_wb, &k);
    if (e && nvn_dirty_tracked(t->mem, (size_t)t->L.size) && !nvn_dirty_since(t->mem, (size_t)t->L.size, e->stamp)) { e->stamp = nvn_dirty_now(); pthread_mutex_unlock(&g_wb_mu); return; }   /* untouched since: still protected */
    if (!e) { e = malloc(sizeof *e); if (!e) { pthread_mutex_unlock(&g_wb_mu); return; } nvn_hm_put(&g_wb, &k, e); }
    e->t = *t; e->stamp = nvn_dirty_now();
    if (rb_hot(k) && eager_ok(t)) {
        int i = 0; while (i < g_neager && g_eager[i].serial != k) i++;
        if (i == g_neager && g_neager < 32) g_eager[g_neager++] = *t;
    }
    pthread_mutex_unlock(&g_wb_mu);
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
_Atomic uint64_t vt_inv_n, vt_inv_wb, vt_inv_ns, vt_inv_lock_ns;   /* invalidates, those with write-backs, their time, time waiting for vk_mu */
void vt_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    if (!p || !p->mem) return;
    atomic_fetch_add(&vt_inv_n, 1);
    const uint8_t *a = (const uint8_t *)p->mem + off, *z = a + n;
    wb_ent *list[64]; int nl = 0;
    pthread_mutex_lock(&g_wb_mu);
    for (uint32_t i = 0; i < g_wb.cap && nl < 64; i++) {
        wb_ent *e = g_wb.vals ? g_wb.vals[i] : NULL;
        if (!e || (const uint8_t *)e->t.mem >= z || (const uint8_t *)e->t.mem + e->t.L.size <= a) continue;
        uint64_t k = e->t.serial; nvn_hm_del(&g_wb, &k); list[nl++] = e; i = (uint32_t)-1;   /* deletion shifts entries: rescan */
    }
    pthread_mutex_unlock(&g_wb_mu);
    if (!nl) return;
    atomic_fetch_add(&vt_inv_wb, 1); uint64_t inv0 = pal_now_ns();
    /* read-backs wait for the replay lock and the GPU: the guest thread gives its core up meanwhile */
    __attribute__((cleanup(runtime_core_back))) int away = runtime_core_away();
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
        if (ok) { pthread_mutex_lock(&g_wb_mu); if (!rb_hot(t->serial)) g_rb_hot[g_rb_hot_n++ % 64] = t->serial; pthread_mutex_unlock(&g_wb_mu); }
        if (ok) { if (++g_wb_done <= 4) VLOG("write-back: rendered texture %ux%u format %#x written to pool memory (%llu so far, %llu skipped: the CPU wrote since)", t->d.width, t->d.height, t->d.format, (unsigned long long)g_wb_done, (unsigned long long)g_wb_skipped); }
        else VK_ONCE("write-back of texture %ux%u format %#x target %d failed: pool memory keeps its old bytes", t->d.width, t->d.height, t->d.format, t->d.target);
        free(list[i]);
    }
    atomic_fetch_add(&vt_inv_ns, pal_now_ns() - inv0);
}

typedef struct { tex_t t; const uint8_t *st; size_t pitch, n; uint64_t stamp; } eager_out;
static void eager_done(void *arg) {   /* completion thread: the submission is done; its staging is still mapped */
    eager_out *o = arg;
    const tex_t *t = &o->t;
    pthread_mutex_lock(&g_wb_mu);
    uint64_t k = t->serial; wb_ent *e = nvn_hm_get(&g_wb, &k);
    int mine = e && e->stamp == o->stamp;   /* not rendered again since: this copy is the newest */
    if (mine) { nvn_hm_del(&g_wb, &k); free(e); }
    pthread_mutex_unlock(&g_wb_mu);
    if (mine) {
        size_t size = (size_t)t->L.size; uint8_t *tmp = malloc(size);
        if (tmp) {
            memcpy(tmp, t->mem, size);
            if (!nvn_swizzle(&t->d, 0, 0, o->st, o->pitch, tmp)) {
                void *tok = nvn_dirty_gpu_write_begin(t->mem, size);
                memcpy(t->mem, tmp, size);
                nvn_dirty_gpu_write_end(tok);
            }
            free(tmp);
        }
    }
    free(o);
}
void vt_eager_writebacks(void) {   /* vk_mu held, outside a pass, at the end of a submission's replay */
    pthread_mutex_lock(&g_wb_mu);
    tex_t list[32]; int n = g_neager; memcpy(list, g_eager, sizeof *list * (size_t)n); g_neager = 0;
    uint64_t stamps[32];
    for (int i = 0; i < n; i++) { uint64_t k = list[i].serial; wb_ent *e = nvn_hm_get(&g_wb, &k); stamps[i] = e ? e->stamp : 0; }
    pthread_mutex_unlock(&g_wb_mu);
    for (int i = 0; i < n; i++) {
        const tex_t *t = &list[i];
        vtex *e = vt_get(t);
        if (!e || !stamps[i]) continue;
        int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        VkImageType type; uint32_t w, h, d, layers; shape(t, &type, &w, &h, &d, &layers);
        size_t pitch = (size_t)w * bpb, sz = pitch * (type == VK_IMAGE_TYPE_1D ? 1 : h) * (type == VK_IMAGE_TYPE_3D ? d : 1);
        VkBuffer sb; VkDeviceSize so;
        uint8_t *st = vk_stage(sz, 16, &sb, &so);
        VkCommandBuffer c = vk_cmd();
        VkBufferImageCopy cp = {.bufferOffset = so, .imageSubresource = {e->aspect, 0, 0, 1},
                                .imageExtent = {w, type == VK_IMAGE_TYPE_1D ? 1 : h, type == VK_IMAGE_TYPE_3D ? d : 1}};
        vkCmdCopyImageToBuffer(c, e->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &cp);
        vk_barrier_lazy();
        eager_out *o = malloc(sizeof *o); if (!o) continue;
        *o = (eager_out){*t, st, pitch, sz, stamps[i]};
        vk_after(eager_done, o);
        static int said; if (!said++) VLOG("eager write-back: rendered %ux%u format %#x copied out with its submission (the CPU reads it back)", t->d.width, t->d.height, t->d.format);
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
