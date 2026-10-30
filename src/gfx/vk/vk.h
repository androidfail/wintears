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
extern VkPipelineCache vk_pcache;   /* vk_draw.c: kept in the profile between runs */
extern int vk_divisor, vk_custom_border;
extern int vk_integrated;        /* the GPU shares system RAM */
extern uint64_t vk_local_heap;   /* bytes of the largest device-local heap */
void vk_log_memory(void);
void vt_memory(uint64_t out[4]);   /* image bytes: decoded ASTC, other uploaded, GPU-written, saved by the mip drop */   /* VK_EXT_vertex_attribute_divisor is enabled: instance divisors above 1 */
extern uint32_t vk_sb_align;   /* storage buffers are bound at their address rounded down to this; the translated shaders add the rest */
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
void vk_lost(int r);
void vk_barrier_lazy(void);   /* a full barrier before the next command recorded through vk_cmd (several in a row: one) */
/* VMA usage for what the GPU reads and writes: its own memory, or shared memory on an integrated GPU whose own heap is
   under 1 GiB (a laptop's 256 MiB carve-out filled up and allocations failed; TOTK_PREFER_HOST=1 forces it) */
VmaMemoryUsage vk_prefer_device(void);   /* VK_ERROR_DEVICE_LOST: logs what the driver says went wrong (then the caller dies) */
/* n bytes to guest address dst by the CPU once the open submission has completed: src's bytes as they are now
   (snapshot) or as they are then (src stays valid, e.g. guest memory) */
void vk_after_write(uint64_t dst, const void *src, size_t n, int snapshot);
void vk_barrier(VkCommandBuffer c);  /* every earlier access before every later one */
int vk_done_anchor(uint64_t *gpu_end, uint64_t *cpu_ns);   /* completion thread: the completing submission's GPU end and when it was seen */
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
int vt_converted(const vtex *e);          /* its texels are converted from NVN's bytes on upload (vk_fmt convert) */
uint32_t vt_texel_size(const vtex *e);    /* bytes per texel (block) in the image's own format */
/* Linear NVN bytes (rows of `row` bytes, layers after depth slices) into a region of e, converted as an upload is;
   recorded in the replay's command buffer. */
void vt_copy_linear(vtex *e, const tex_t *t, uint32_t level, uint32_t layer0, uint32_t layers, VkOffset3D o, VkExtent3D x, const uint8_t *src, size_t row);
/* A view: the whole texture (v NULL) or an NVN view. mode: VT_SAMPLED (NVN swizzle, the view's levels / layers),
   VT_ATTACHMENT (one level, no swizzle), VT_STORAGE (no swizzle). want: the view type the shader declares, or
   VT_ANY_TYPE for the texture's own. */
enum { VT_SAMPLED, VT_ATTACHMENT, VT_STORAGE };
#define VT_ANY_TYPE VK_IMAGE_VIEW_TYPE_MAX_ENUM
VkImageView vt_view(vtex *e, const tex_t *t, const view_t *v, int mode, VkImageViewType want);
extern VkFormat vt_last_vfmt;
extern char vt_writer[96];   /* set before vt_written: what is writing (diagnostics) */
const char *vt_writer_of(const vtex *e);   /* the format of the view vt_view returned last (a pass renders in the view's format, not the image's) */
/* provenance (diagnostics): the inputs of each write, printed back to their sources for textures an F9 frame samples */
void vt_prov_reset(void);
void vt_prov_input(const tex_t *t);
void vt_prov_draw(vtex *e, const char *vs, const char *fs);
void vt_prov_copy(vtex *e, const tex_t *src);
void vt_prov_dump(const vtex *e);
void vt_prov_drop(vtex *e, const char *why);
/* A view of a block-compressed image whose texel is one block (BC4 as RGBA16UI: the game renders into BC textures this way):
   1 and the block size in texels in *bw / *bh; its extent is the image's in blocks. */
int vt_block_view(const vtex *e, const tex_t *t, const view_t *v, int *bw, int *bh);
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
void vt_eager_writebacks(void);
uint64_t vt_treg_version(void);   /* changes whenever the game registers a texture (vk_shader.c: variant answers kept until then) */   /* end of a submission's replay: copy out rendered textures the CPU reads back (vk_tex.c) */
void vt_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
void vt_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img);
/* Buffer texture (NVN_TEXTURE_TARGET_BUFFER): a zero-copy VkBufferView over the pool bytes the GPU samples.
   Call with vk_mu held; the view is cached per (mem,size,format) and dropped with the pool. 0 = unusable. */
VkBufferView vt_texel_view(const tex_t *t);
VkBufferView vt_texel_view_copy(const tex_t *t);   /* sampled use only: a view of a copy of the bytes */

/* vk_exec.c ----------------------------------------------------------------------------------------------------- */
void vx_run(const uint8_t *list);   /* replay one recording into the open submission */
int vt_prov_strings(void);   /* the texture writer texts are wanted (pass log, capture, TOTK_PROVENANCE=1) */
void vk_encode_install(void);   /* vk_encode.c: Vulkan command calls made on their own thread */
void vk_encode_drain(void);
void vk_encode_call(void (*fn)(void *), void *arg);
uint64_t vk_frame_serial(void);   /* moves on per frame, not at a timed split (vk_split(1)) */
void vk_after_frame(void (*fn)(void *), void *arg);   /* run when the frame's last submission is done */
int vk_split_due(void);   /* TOTK_SPLIT_MS of replay since the submission began */
int vk_stage_heavy(void);   /* the open submission holds a lot of staging: split it (vk_split) at the next command */
void vk_split(int carry);   /* carry: a timed split (the frame stays open) */
void vx_end_pass_at(const char *file, int line);   /* the render pass in progress ends (before copies, uploads, present) */
#define vx_end_pass() vx_end_pass_at(__FILE__, __LINE__)   /* TOTK_DIAG counts pass ends by where they happen */
void vx_pass_end_report(char *out, size_t cap);
int vx_barrier_pending(void);        /* a game barrier waits in the open pass (vk_exec.c) */
void vx_pass_wrote_storage(void);    /* a draw of the open pass writes storage buffers / images */   /* the most frequent places, since the last report */
void vx_submit_done(void);          /* end of a game submission: CPU effects queued, state kept for the next */
void vx_pin_refs(void (*mark)(const void *));   /* texture copies the replay state still points to (nvn.c pin sweep) */

/* vk_present.c -------------------------------------------------------------------------------------------------- */
int vp_init(void);
uint64_t vp_present(const tex_t *t, const int crop[4]);
