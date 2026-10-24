/* Vulkan backend (in place of upstream's Metal nvn_mtl.m), shared declarations.

   Files: vk_dev.c (device, guest memory pools as imported host memory, submissions and their completion),
   vk_tex.c (NVN textures as images, uploads from pool memory, views, samplers), vk_exec.c (replay of the NVN
   recordings: render targets, clears, copies, state), vk_present.c (window swapchain), vk_fmt.c (formats),
   vk_mem.cpp (Vulkan Memory Allocator).

   The model, as upstream's on Metal:
   - GPU address = guest address: every GPU-visible memory pool is one VkBuffer over the game's own memory
     (VK_EXT_external_memory_host), so buffers, uniforms, vertices and copies need no copying.
   - Textures are images. Those whose bytes the CPU writes (pool CPU-visible, not a render target kind) are uploaded
     from pool memory and uploaded again when the dirty tracker (nvn_dirty.c) saw a write; textures the GPU renders
     into are owned by the GPU. Images are keyed by storage (memory + layout): every NVN texture object over the same
     bytes and layout sees the same image (upstream "storage-shared GPU textures").
   - One queue. A submission is one command buffer pair (uploads first, then the replay), signalling a timeline
     value; the completion thread then runs what the CPU must see after the GPU (events, counters, frees).
   - Every image stays in VK_IMAGE_LAYOUT_GENERAL; operations are ordered by full memory barriers.
     shortcut: GENERAL layout and full barriers cost GPU time; upgrade when profiling says so. */
#pragma once
#include <pthread.h>
#include <volk.h>
#include <vk_mem_alloc.h>

#include "gfx/nvn_backend.h"
#include "host/hos.h"
#include "nvn_tex.h"

#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) die("Vulkan: %s failed (%d) at %s:%d", #x, (int)r_, __FILE__, __LINE__); } while (0)
#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define VK_ONCE(...) do { static int said_; if (!said_++) VLOG(__VA_ARGS__); } while (0)

extern VkInstance vk_inst;
extern VkPhysicalDevice vk_phys;
extern VkDevice vk_dev;
extern VkQueue vk_q;
extern uint32_t vk_qfam;
extern VmaAllocator vk_vma;
extern VkPhysicalDeviceProperties vk_props;
/* texel buffer views (Vulkan 1.3 texelBufferAlignment): offset alignment in bytes, and whether a texel's own size is enough, for uniform [0] / storage [1] */
extern uint32_t vk_texel_align[2], vk_texel_single[2];
extern VkSurfaceKHR vk_surface;

/* vk_dev.c ------------------------------------------------------------------------------------------------------ */
/* The pool buffer holding [addr, addr + len): 1 and the buffer / offset, 0 if no GPU-visible pool holds it. */
int vk_buf(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);

/* The submission being recorded. Only the thread holding vk_mu records: gpu_submit (the replay), present, and the
   CPU texel reads / writes. vk_open makes sure one is open; vk_flush submits it (returns its fence value). */
extern pthread_mutex_t vk_mu;
void vk_open(void);
uint64_t vk_flush(const VkSemaphoreSubmitInfo *wait, const VkSemaphoreSubmitInfo *signal);
VkCommandBuffer vk_cmd(void);   /* the replay's command buffer */
VkCommandBuffer vk_pre(void);   /* runs before it in the same submission: uploads and image initialisation */
extern uint64_t vk_ns_xlate, vk_ns_pipe, vk_ns_tex, vk_n_pass, vk_n_ubo, vk_n_ubflush, vk_n_barrier, g_sh_bytes;   /* logged every 10 s: time translating shaders, creating pipelines, uploading textures (render thread); counts per kind; bytes copied to video memory */
uint64_t vk_sub_serial(void);   /* counts submissions: "once per submission" checks */
uint64_t vk_open_fence(void);   /* the fence the submission being recorded will signal */
uint64_t vk_gpu_done(void);     /* the last fence the GPU has finished */
/* Staging memory (host visible) for the open submission, freed when it completes. */
void *vk_stage(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);
/* fn(arg) on the completion thread once the open submission has completed (in submission order). */
void vk_after(void (*fn)(void *arg), void *arg);
void vk_wait_done(uint64_t fence);   /* the CPU-side effects up to fence have run too */
void vk_barrier(VkCommandBuffer c);  /* every earlier access before every later one */
uint64_t vk_ticks(void);             /* 19.2 MHz counter ticks, the clock of the game's CNTPCT reads */

/* vk_fmt.c ------------------------------------------------------------------------------------------------------ */
typedef struct { VkFormat vk; VkComponentSwizzle swz[4]; uint8_t depth, stencil, convert; } vk_fmt;
/* NVN format -> Vulkan; 0 = no counterpart (vk_fmt.vk == VK_FORMAT_UNDEFINED). convert: the bytes need converting
   on upload (vk_tex.c says which). */
int vk_fmt_of(int nvn_format, vk_fmt *out);
int vk_fmt_block_compressed(VkFormat f);   /* BC / ETC / EAC / ASTC */
VkFormat vk_vfmt(int nvn_format);          /* vertex attribute format, VK_FORMAT_UNDEFINED if none */

/* vk_tex.c ------------------------------------------------------------------------------------------------------ */
typedef struct vtex vtex;
void vt_init(void);
/* The image of texture t; uploads its bytes first when the CPU owns them and they changed. NULL if t cannot be an
   image (no storage, unknown format). Call with vk_mu held, outside a render pass. */
vtex *vt_get(const tex_t *t);
void vt_written(vtex *e);            /* the GPU writes it (render target, copy destination): the GPU owns it from now on */
VkImage vt_image(const vtex *e);
VkFormat vt_vkformat(const vtex *e);
VkImageAspectFlags vt_aspect(const vtex *e);
int vt_depth(const vtex *e);
void vt_size(const vtex *e, unsigned level, uint32_t *w, uint32_t *h, uint32_t *d);
uint32_t vt_layers(const vtex *e);
/* A view: the whole texture (v NULL) or an NVN view. mode: VT_SAMPLED (NVN swizzle, the view's levels / layers),
   VT_ATTACHMENT (one level, no swizzle), VT_STORAGE (no swizzle). want: the view type the shader declares, or
   VT_ANY_TYPE for the texture's own. */
enum { VT_SAMPLED, VT_ATTACHMENT, VT_STORAGE };
#define VT_ANY_TYPE VK_IMAGE_VIEW_TYPE_MAX_ENUM
VkImageView vt_view(vtex *e, const tex_t *t, const view_t *v, int mode, VkImageViewType want);
uint32_t vt_samples(const vtex *e);
void vt_release_serial(uint64_t serial);
void vt_pool_gone(uintptr_t lo, uintptr_t hi);   /* images over a finalised pool */
void vt_end_submission(void);                    /* idle images are released */
/* Registered texture / sampler pool slots (snapshots, as upstream mtl_reg_tex) */
void vt_reg_tex(int id, const tex_t *t, const view_t *v);
void vt_reg_samp(int id, const samp_par *p);
void vt_set_pool_n(int n);
VkSampler vt_sampler(const samp_par *p);
int vt_slot_tex(int id, tex_t *t, view_t *v, int *has_v);   /* the slot's registered texture (a copy); 0 = empty */
int vt_slot_samp(int id, samp_par *p);
void vt_note_rendered(const tex_t *t);   /* a pass renders into t: a CPU read of its pool memory must see it (vt_pool_invalidate) */
void vt_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n);
void vt_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
void vt_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img);
/* Buffer texture (NVN_TEXTURE_TARGET_BUFFER): a zero-copy VkBufferView over the pool bytes the GPU samples.
   Call with vk_mu held; the view is cached per (mem,size,format) and dropped with the pool. 0 = unusable. */
VkBufferView vt_texel_view(const tex_t *t);
VkBufferView vt_texel_view_copy(const tex_t *t);   /* sampled use only: a view of a copy of the bytes */

/* vk_exec.c ----------------------------------------------------------------------------------------------------- */
void vx_run(const uint8_t *list);   /* replay one recording into the open submission */
void vx_end_pass(void);             /* the render pass in progress ends (before copies, uploads, present) */
void vx_submit_done(void);          /* end of a game submission: CPU effects queued, state kept for the next */
void vx_pin_refs(void (*mark)(const void *));   /* texture copies the replay state still points to (nvn.c pin sweep) */

/* vk_present.c -------------------------------------------------------------------------------------------------- */
int vp_init(void);
uint64_t vp_present(const tex_t *t, const int crop[4]);
