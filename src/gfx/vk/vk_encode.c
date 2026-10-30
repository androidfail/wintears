/* The encode thread: every Vulkan command-buffer call (vkCmd*, vkBegin/EndCommandBuffer, vkUpdateDescriptorSets) and
   the hand-over of finished submissions are queued with copies of their arguments and made, in order, on a thread of
   their own. Under Wine / Proton each Vulkan call crosses into the Linux driver through a thunk: about a fifth of the
   Steam Deck's replay thread, which is the frame's critical path. The replay keeps deciding what to draw; this thread
   only makes the calls.
   Installed over volk's function pointers (vk_encode_install, after volkLoadDevice), so the callers are unchanged.
   Producers are serialized by a lock (the replay, the present path). vk_encode_drain waits until everything queued
   has been made (the GPU drain, swapchain changes). TOTK_ENCODE=0/1 (default: on under Wine). */
#include "gfx/vk/vk.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define RING (32u << 20)
typedef struct { void (*run)(void *); uint32_t size; uint32_t wrap; } rec_hdr;   /* size: the whole record, 16-byte aligned */
static uint8_t *g_ring;
static _Atomic uint64_t g_w, g_r;   /* bytes written / consumed, ever (positions are % RING) */
static atomic_flag g_prod = ATOMIC_FLAG_INIT;   /* one producer at a time (a spin lock: almost never contended) */
static inline void prod_lock(void) { while (atomic_flag_test_and_set_explicit(&g_prod, memory_order_acquire)) __builtin_ia32_pause(); }
static inline void prod_unlock(void) { atomic_flag_clear_explicit(&g_prod, memory_order_release); }
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_idle_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_sleeping, g_on;
static uint64_t g_resv_at; static uint32_t g_resv_n, g_resv_used;   /* the record being written (producer lock held) */

static void *encode_thread(void *unused) {
    (void)unused; pal_thread_set_name("vk encode"); pal_thread_set_priority(PAL_PRIO_HIGH);
    for (;;) {
        uint64_t r = atomic_load_explicit(&g_r, memory_order_relaxed), w = atomic_load_explicit(&g_w, memory_order_acquire);
        if (r == w) {
            for (int i = 0; i < 4000 && r == atomic_load_explicit(&g_w, memory_order_acquire); i++) __builtin_ia32_pause();
            if (r == atomic_load_explicit(&g_w, memory_order_acquire)) {
                pthread_mutex_lock(&g_mu);
                atomic_store(&g_sleeping, 1);
                pthread_cond_broadcast(&g_idle_cv);
                while (r == atomic_load_explicit(&g_w, memory_order_acquire)) pthread_cond_wait(&g_cv, &g_mu);
                atomic_store(&g_sleeping, 0);
                pthread_mutex_unlock(&g_mu);
            }
            continue;
        }
        rec_hdr *h = (rec_hdr *)(g_ring + r % RING);
        if (!h->wrap) h->run(h + 1);
        atomic_store_explicit(&g_r, r + h->size, memory_order_release);
    }
    return NULL;
}
/* room for n payload bytes in a new record (producer lock held) */
static void *reserve(void (*run)(void *), uint32_t tsize, uint32_t more) {   /* the argument struct, then room for copied arrays */
    uint32_t need = (uint32_t)((sizeof(rec_hdr) + tsize + more + 15) & ~15u);
    for (;;) {
        uint64_t w = atomic_load_explicit(&g_w, memory_order_relaxed), at = w % RING;
        if (at + need > RING) {   /* no room before the end: a wrap record over the rest */
            if (w + (RING - at) - atomic_load_explicit(&g_r, memory_order_acquire) > RING) { pal_yield(); continue; }
            rec_hdr *h = (rec_hdr *)(g_ring + at); h->run = NULL; h->size = (uint32_t)(RING - at); h->wrap = 1;
            atomic_store_explicit(&g_w, w + (RING - at), memory_order_release);
            continue;
        }
        if (w + need - atomic_load_explicit(&g_r, memory_order_acquire) > RING) { pal_yield(); continue; }   /* full: the encoder catches up */
        rec_hdr *h = (rec_hdr *)(g_ring + at); h->run = run; h->size = need; h->wrap = 0;
        g_resv_at = w; g_resv_n = need; g_resv_used = (uint32_t)sizeof(rec_hdr) + tsize;
        return h + 1;
    }
}
static void *extra(const void *src, size_t n) {   /* copy n more bytes into the record being written */
    if (!src || !n) return NULL;
    uint32_t at = (g_resv_used + 15u) & ~15u;
    void *p = g_ring + (g_resv_at + at) % RING;
    memcpy(p, src, n); g_resv_used = at + (uint32_t)n;
    return p;
}
static void commit(void) {
    atomic_store_explicit(&g_w, g_resv_at + g_resv_n, memory_order_release);
    if (atomic_load(&g_sleeping)) { pthread_mutex_lock(&g_mu); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
    prod_unlock();
}
#define BEGIN(T, n) prod_lock(); T *a = reserve(run_##T, (uint32_t)sizeof(T), (uint32_t)(n))
#define SZ(T, cnt) ((size_t)(cnt) * sizeof(T) + 16)

void vk_encode_drain(void) {
    if (!atomic_load(&g_on)) return;
    prod_lock();
    uint64_t w = atomic_load(&g_w);
    pthread_mutex_lock(&g_mu);
    while (atomic_load_explicit(&g_r, memory_order_acquire) < w) {
        if (atomic_load(&g_sleeping)) pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mu); pal_yield(); pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    prod_unlock();
}

/* a function of the program, called on the encode thread in order (the hand-over of a submission) */
typedef struct { void (*fn)(void *); void *arg; } a_call;
static void run_a_call(void *p) { a_call *a = p; a->fn(a->arg); }
void vk_encode_call(void (*fn)(void *), void *arg) {
    if (!atomic_load(&g_on)) { fn(arg); return; }
    BEGIN(a_call, 0); a->fn = fn; a->arg = arg; commit();
}

/* ---------- the wrapped calls ---------- */
#define O(name) static PFN_##name o_##name
O(vkCmdCopyBuffer); O(vkCmdPipelineBarrier2); O(vkCmdWriteTimestamp2); O(vkCmdCopyImageToBuffer); O(vkCmdCopyImage); O(vkCmdCopyBufferToImage);
O(vkCmdClearColorImage); O(vkCmdClearAttachments); O(vkCmdBlitImage); O(vkCmdBindPipeline); O(vkCmdDrawIndexed); O(vkCmdUpdateBuffer);
O(vkCmdSetViewportWithCount); O(vkCmdSetStencilWriteMask); O(vkCmdSetStencilReference); O(vkCmdSetStencilCompareMask); O(vkCmdSetScissorWithCount);
O(vkCmdResetQueryPool); O(vkCmdFillBuffer); O(vkCmdEndRendering); O(vkCmdDraw); O(vkCmdBindDescriptorSets); O(vkCmdSetStencilTestEnable);
O(vkCmdSetStencilOp); O(vkCmdSetRasterizerDiscardEnable); O(vkCmdSetPrimitiveTopology); O(vkCmdSetPrimitiveRestartEnable); O(vkCmdSetLineWidth);
O(vkCmdSetFrontFace); O(vkCmdSetDepthWriteEnable); O(vkCmdSetDepthTestEnable); O(vkCmdSetDepthCompareOp); O(vkCmdSetDepthBoundsTestEnable);
O(vkCmdSetDepthBiasEnable); O(vkCmdSetDepthBias); O(vkCmdSetCullMode); O(vkCmdSetBlendConstants); O(vkCmdPushConstants); O(vkCmdEndQuery);
O(vkCmdDispatch); O(vkCmdClearDepthStencilImage); O(vkCmdBindVertexBuffers2); O(vkCmdBindIndexBuffer); O(vkCmdBeginRendering); O(vkCmdBeginQuery);
O(vkBeginCommandBuffer); O(vkEndCommandBuffer); O(vkUpdateDescriptorSets);

/* calls of plain values: the arguments as they are */
#define V1(name, T1) typedef struct { VkCommandBuffer c; T1 a1; } a_##name; \
    static void run_a_##name(void *p) { a_##name *a = p; o_##name(a->c, a->a1); } \
    static VKAPI_ATTR void VKAPI_CALL w_##name(VkCommandBuffer c, T1 a1) { BEGIN(a_##name, 0); a->c = c; a->a1 = a1; commit(); }
#define V2(name, T1, T2) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; } a_##name; \
    static void run_a_##name(void *p) { a_##name *a = p; o_##name(a->c, a->a1, a->a2); } \
    static VKAPI_ATTR void VKAPI_CALL w_##name(VkCommandBuffer c, T1 a1, T2 a2) { BEGIN(a_##name, 0); a->c = c; a->a1 = a1; a->a2 = a2; commit(); }
#define V3(name, T1, T2, T3) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; T3 a3; } a_##name; \
    static void run_a_##name(void *p) { a_##name *a = p; o_##name(a->c, a->a1, a->a2, a->a3); } \
    static VKAPI_ATTR void VKAPI_CALL w_##name(VkCommandBuffer c, T1 a1, T2 a2, T3 a3) { BEGIN(a_##name, 0); a->c = c; a->a1 = a1; a->a2 = a2; a->a3 = a3; commit(); }
#define V4(name, T1, T2, T3, T4) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; T3 a3; T4 a4; } a_##name; \
    static void run_a_##name(void *p) { a_##name *a = p; o_##name(a->c, a->a1, a->a2, a->a3, a->a4); } \
    static VKAPI_ATTR void VKAPI_CALL w_##name(VkCommandBuffer c, T1 a1, T2 a2, T3 a3, T4 a4) { BEGIN(a_##name, 0); a->c = c; a->a1 = a1; a->a2 = a2; a->a3 = a3; a->a4 = a4; commit(); }
#define V5(name, T1, T2, T3, T4, T5) typedef struct { VkCommandBuffer c; T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; } a_##name; \
    static void run_a_##name(void *p) { a_##name *a = p; o_##name(a->c, a->a1, a->a2, a->a3, a->a4, a->a5); } \
    static VKAPI_ATTR void VKAPI_CALL w_##name(VkCommandBuffer c, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5) { BEGIN(a_##name, 0); a->c = c; a->a1 = a1; a->a2 = a2; a->a3 = a3; a->a4 = a4; a->a5 = a5; commit(); }

typedef struct { VkCommandBuffer c; } a_vkCmdEndRendering;
static void run_a_vkCmdEndRendering(void *p) { o_vkCmdEndRendering(((a_vkCmdEndRendering *)p)->c); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdEndRendering(VkCommandBuffer c) { BEGIN(a_vkCmdEndRendering, 0); a->c = c; commit(); }
V3(vkCmdWriteTimestamp2, VkPipelineStageFlags2, VkQueryPool, uint32_t)
V2(vkCmdBindPipeline, VkPipelineBindPoint, VkPipeline)
V5(vkCmdDrawIndexed, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
V4(vkCmdDraw, uint32_t, uint32_t, uint32_t, uint32_t)
V2(vkCmdSetStencilWriteMask, VkStencilFaceFlags, uint32_t)
V2(vkCmdSetStencilReference, VkStencilFaceFlags, uint32_t)
V2(vkCmdSetStencilCompareMask, VkStencilFaceFlags, uint32_t)
V3(vkCmdResetQueryPool, VkQueryPool, uint32_t, uint32_t)
V4(vkCmdFillBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t)
V1(vkCmdSetStencilTestEnable, VkBool32)
V5(vkCmdSetStencilOp, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp)
V1(vkCmdSetRasterizerDiscardEnable, VkBool32)
V1(vkCmdSetPrimitiveTopology, VkPrimitiveTopology)
V1(vkCmdSetPrimitiveRestartEnable, VkBool32)
V1(vkCmdSetLineWidth, float)
V1(vkCmdSetFrontFace, VkFrontFace)
V1(vkCmdSetDepthWriteEnable, VkBool32)
V1(vkCmdSetDepthTestEnable, VkBool32)
V1(vkCmdSetDepthCompareOp, VkCompareOp)
V1(vkCmdSetDepthBoundsTestEnable, VkBool32)
V1(vkCmdSetDepthBiasEnable, VkBool32)
V3(vkCmdSetDepthBias, float, float, float)
V1(vkCmdSetCullMode, VkCullModeFlags)
V2(vkCmdEndQuery, VkQueryPool, uint32_t)
V3(vkCmdDispatch, uint32_t, uint32_t, uint32_t)
V3(vkCmdBindIndexBuffer, VkBuffer, VkDeviceSize, VkIndexType)
V3(vkCmdBeginQuery, VkQueryPool, uint32_t, VkQueryControlFlags)

/* calls with arrays: copied after the record */
typedef struct { VkCommandBuffer c; VkBuffer s, d; uint32_t n; const VkBufferCopy *r; } a_cb;
static void run_a_cb(void *p) { a_cb *a = p; o_vkCmdCopyBuffer(a->c, a->s, a->d, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    BEGIN(a_cb, SZ(VkBufferCopy, n)); a->c = c; a->s = s; a->d = d; a->n = n; a->r = extra(r, n * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; VkImage i; VkImageLayout l; VkBuffer b; uint32_t n; const VkBufferImageCopy *r; } a_cib;
static void run_a_cib(void *p) { a_cib *a = p; o_vkCmdCopyImageToBuffer(a->c, a->i, a->l, a->b, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) {
    BEGIN(a_cib, SZ(VkBufferImageCopy, n)); a->c = c; a->i = i; a->l = l; a->b = b; a->n = n; a->r = extra(r, n * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; VkBuffer b; VkImage i; VkImageLayout l; uint32_t n; const VkBufferImageCopy *r; } a_cbi;
static void run_a_cbi(void *p) { a_cbi *a = p; o_vkCmdCopyBufferToImage(a->c, a->b, a->i, a->l, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    BEGIN(a_cbi, SZ(VkBufferImageCopy, n)); a->c = c; a->b = b; a->i = i; a->l = l; a->n = n; a->r = extra(r, n * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageCopy *r; } a_ci;
static void run_a_ci(void *p) { a_ci *a = p; o_vkCmdCopyImage(a->c, a->s, a->sl, a->d, a->dl, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    BEGIN(a_ci, SZ(VkImageCopy, n)); a->c = c; a->s = s; a->sl = sl; a->d = d; a->dl = dl; a->n = n; a->r = extra(r, n * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageBlit *r; VkFilter f; } a_bi;
static void run_a_bi(void *p) { a_bi *a = p; o_vkCmdBlitImage(a->c, a->s, a->sl, a->d, a->dl, a->n, a->r, a->f); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    BEGIN(a_bi, SZ(VkImageBlit, n)); a->c = c; a->s = s; a->sl = sl; a->d = d; a->dl = dl; a->n = n; a->r = extra(r, n * sizeof *r); a->f = f; commit();
}
typedef struct { VkCommandBuffer c; VkImage i; VkImageLayout l; VkClearColorValue v; uint32_t n; const VkImageSubresourceRange *r; } a_cci;
static void run_a_cci(void *p) { a_cci *a = p; o_vkCmdClearColorImage(a->c, a->i, a->l, &a->v, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    BEGIN(a_cci, SZ(VkImageSubresourceRange, n)); a->c = c; a->i = i; a->l = l; a->v = *v; a->n = n; a->r = extra(r, n * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; VkImage i; VkImageLayout l; VkClearDepthStencilValue v; uint32_t n; const VkImageSubresourceRange *r; } a_cdi;
static void run_a_cdi(void *p) { a_cdi *a = p; o_vkCmdClearDepthStencilImage(a->c, a->i, a->l, &a->v, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    BEGIN(a_cdi, SZ(VkImageSubresourceRange, n)); a->c = c; a->i = i; a->l = l; a->v = *v; a->n = n; a->r = extra(r, n * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; uint32_t n, nr; const VkClearAttachment *at; const VkClearRect *r; } a_ca;
static void run_a_ca(void *p) { a_ca *a = p; o_vkCmdClearAttachments(a->c, a->n, a->at, a->nr, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearAttachments(VkCommandBuffer c, uint32_t n, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r) {
    BEGIN(a_ca, SZ(VkClearAttachment, n) + SZ(VkClearRect, nr)); a->c = c; a->n = n; a->nr = nr; a->at = extra(at, n * sizeof *at); a->r = extra(r, nr * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; VkBuffer b; VkDeviceSize o, n; const void *d; } a_ub;
static void run_a_ub(void *p) { a_ub *a = p; o_vkCmdUpdateBuffer(a->c, a->b, a->o, a->n, a->d); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize n, const void *d) {
    BEGIN(a_ub, (size_t)n + 16); a->c = c; a->b = b; a->o = o; a->n = n; a->d = extra(d, (size_t)n); commit();
}
typedef struct { VkCommandBuffer c; uint32_t n; const VkViewport *v; } a_vp;
static void run_a_vp(void *p) { a_vp *a = p; o_vkCmdSetViewportWithCount(a->c, a->n, a->v); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetViewportWithCount(VkCommandBuffer c, uint32_t n, const VkViewport *v) {
    BEGIN(a_vp, SZ(VkViewport, n)); a->c = c; a->n = n; a->v = extra(v, n * sizeof *v); commit();
}
typedef struct { VkCommandBuffer c; uint32_t n; const VkRect2D *r; } a_sc;
static void run_a_sc(void *p) { a_sc *a = p; o_vkCmdSetScissorWithCount(a->c, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetScissorWithCount(VkCommandBuffer c, uint32_t n, const VkRect2D *r) {
    BEGIN(a_sc, SZ(VkRect2D, n)); a->c = c; a->n = n; a->r = extra(r, n * sizeof *r); commit();
}
typedef struct { VkCommandBuffer c; float v[4]; } a_bc;
static void run_a_bc(void *p) { a_bc *a = p; o_vkCmdSetBlendConstants(a->c, a->v); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetBlendConstants(VkCommandBuffer c, const float v[4]) { BEGIN(a_bc, 0); a->c = c; memcpy(a->v, v, sizeof a->v); commit(); }
typedef struct { VkCommandBuffer c; VkPipelineLayout l; VkShaderStageFlags st; uint32_t o, n; const void *d; } a_pc;
static void run_a_pc(void *p) { a_pc *a = p; o_vkCmdPushConstants(a->c, a->l, a->st, a->o, a->n, a->d); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t o, uint32_t n, const void *d) {
    BEGIN(a_pc, (size_t)n + 16); a->c = c; a->l = l; a->st = st; a->o = o; a->n = n; a->d = extra(d, n); commit();
}
typedef struct { VkCommandBuffer c; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; const VkDescriptorSet *s; const uint32_t *d; } a_bds;
static void run_a_bds(void *p) { a_bds *a = p; o_vkCmdBindDescriptorSets(a->c, a->bp, a->l, a->first, a->n, a->s, a->nd, a->d); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    BEGIN(a_bds, SZ(VkDescriptorSet, n) + SZ(uint32_t, nd)); a->c = c; a->bp = bp; a->l = l; a->first = first; a->n = n; a->nd = nd;
    a->s = extra(s, n * sizeof *s); a->d = extra(d, nd * sizeof *d); commit();
}
typedef struct { VkCommandBuffer c; uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o, *sz, *st; } a_bvb;
static void run_a_bvb(void *p) { a_bvb *a = p; o_vkCmdBindVertexBuffers2(a->c, a->first, a->n, a->b, a->o, a->sz, a->st); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindVertexBuffers2(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *sz, const VkDeviceSize *st) {
    BEGIN(a_bvb, SZ(VkBuffer, n) + 3 * SZ(VkDeviceSize, n)); a->c = c; a->first = first; a->n = n;
    a->b = extra(b, n * sizeof *b); a->o = extra(o, n * sizeof *o); a->sz = extra(sz, n * sizeof *sz); a->st = extra(st, n * sizeof *st); commit();
}
typedef struct { VkCommandBuffer c; VkDependencyInfo d; } a_pb;
static void run_a_pb(void *p) { a_pb *a = p; o_vkCmdPipelineBarrier2(a->c, &a->d); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo *d) {
    BEGIN(a_pb, SZ(VkMemoryBarrier2, d->memoryBarrierCount) + SZ(VkBufferMemoryBarrier2, d->bufferMemoryBarrierCount) + SZ(VkImageMemoryBarrier2, d->imageMemoryBarrierCount));
    a->c = c; a->d = *d;   /* barriers without pNext chains here */
    a->d.pMemoryBarriers = extra(d->pMemoryBarriers, d->memoryBarrierCount * sizeof *d->pMemoryBarriers);
    a->d.pBufferMemoryBarriers = extra(d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount * sizeof *d->pBufferMemoryBarriers);
    a->d.pImageMemoryBarriers = extra(d->pImageMemoryBarriers, d->imageMemoryBarrierCount * sizeof *d->pImageMemoryBarriers);
    commit();
}
typedef struct { VkCommandBuffer c; VkRenderingInfo r; } a_br;
static void run_a_br(void *p) { a_br *a = p; o_vkCmdBeginRendering(a->c, &a->r); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo *r) {
    BEGIN(a_br, SZ(VkRenderingAttachmentInfo, r->colorAttachmentCount + 2));
    a->c = c; a->r = *r;
    a->r.pColorAttachments = extra(r->pColorAttachments, r->colorAttachmentCount * sizeof *r->pColorAttachments);
    a->r.pDepthAttachment = extra(r->pDepthAttachment, r->pDepthAttachment ? sizeof *r->pDepthAttachment : 0);
    a->r.pStencilAttachment = extra(r->pStencilAttachment, r->pStencilAttachment ? sizeof *r->pStencilAttachment : 0);
    commit();
}
typedef struct { VkCommandBuffer c; VkCommandBufferBeginInfo b; } a_bcb;
static void run_a_bcb(void *p) { a_bcb *a = p; VkResult r = o_vkBeginCommandBuffer(a->c, &a->b); if (r != VK_SUCCESS) VLOG("encode: vkBeginCommandBuffer failed (%d)", (int)r); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *b) {
    BEGIN(a_bcb, 0); a->c = c; a->b = *b; a->b.pInheritanceInfo = NULL; commit(); return VK_SUCCESS;
}
typedef struct { VkCommandBuffer c; } a_ecb;
static void run_a_ecb(void *p) { VkResult r = o_vkEndCommandBuffer(((a_ecb *)p)->c); if (r != VK_SUCCESS) VLOG("encode: vkEndCommandBuffer failed (%d)", (int)r); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkEndCommandBuffer(VkCommandBuffer c) { BEGIN(a_ecb, 0); a->c = c; commit(); return VK_SUCCESS; }
typedef struct { uint32_t n; VkWriteDescriptorSet *w; } a_uds;
static void run_a_uds(void *p) { a_uds *a = p; o_vkUpdateDescriptorSets(vk_dev, a->n, a->w, 0, NULL); }
static VKAPI_ATTR void VKAPI_CALL w_vkUpdateDescriptorSets(VkDevice dev, uint32_t n, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *cp) {
    if (nc) { vk_encode_drain(); o_vkUpdateDescriptorSets(dev, n, w, nc, cp); return; }   /* not used here: made in place */
    size_t sz = SZ(VkWriteDescriptorSet, n);
    for (uint32_t i = 0; i < n; i++)
        sz += w[i].pImageInfo ? SZ(VkDescriptorImageInfo, w[i].descriptorCount) : w[i].pBufferInfo ? SZ(VkDescriptorBufferInfo, w[i].descriptorCount) : SZ(VkBufferView, w[i].descriptorCount);
    BEGIN(a_uds, sz); a->n = n; a->w = extra(w, n * sizeof *w);
    for (uint32_t i = 0; i < n; i++) {   /* only the array the type uses (the others may point anywhere) */
        VkDescriptorType t = w[i].descriptorType; uint32_t k = w[i].descriptorCount;
        int img = t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        int texel = t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        a->w[i].pImageInfo = img ? extra(w[i].pImageInfo, k * sizeof *w[i].pImageInfo) : NULL;
        a->w[i].pTexelBufferView = texel ? extra(w[i].pTexelBufferView, k * sizeof *w[i].pTexelBufferView) : NULL;
        a->w[i].pBufferInfo = !img && !texel ? extra(w[i].pBufferInfo, k * sizeof *w[i].pBufferInfo) : NULL;
    }
    commit();
}

void vk_encode_install(void) {
    const char *e = pal_getenv("TOTK_ENCODE");
    int on = e && *e ? *e != '0' : pal_under_wine();
    VLOG("Vulkan calls made on an encode thread: %s (TOTK_ENCODE=0/1)", on ? "on" : "off");
    if (!on) return;
    g_ring = malloc(RING);
    if (!g_ring) { VLOG("encode: no ring memory, calls stay on the replay thread"); return; }
#define I(name) o_##name = name; name = w_##name
    I(vkCmdCopyBuffer); I(vkCmdPipelineBarrier2); I(vkCmdWriteTimestamp2); I(vkCmdCopyImageToBuffer); I(vkCmdCopyImage); I(vkCmdCopyBufferToImage);
    I(vkCmdClearColorImage); I(vkCmdClearAttachments); I(vkCmdBlitImage); I(vkCmdBindPipeline); I(vkCmdDrawIndexed); I(vkCmdUpdateBuffer);
    I(vkCmdSetViewportWithCount); I(vkCmdSetStencilWriteMask); I(vkCmdSetStencilReference); I(vkCmdSetStencilCompareMask); I(vkCmdSetScissorWithCount);
    I(vkCmdResetQueryPool); I(vkCmdFillBuffer); I(vkCmdEndRendering); I(vkCmdDraw); I(vkCmdBindDescriptorSets); I(vkCmdSetStencilTestEnable);
    I(vkCmdSetStencilOp); I(vkCmdSetRasterizerDiscardEnable); I(vkCmdSetPrimitiveTopology); I(vkCmdSetPrimitiveRestartEnable); I(vkCmdSetLineWidth);
    I(vkCmdSetFrontFace); I(vkCmdSetDepthWriteEnable); I(vkCmdSetDepthTestEnable); I(vkCmdSetDepthCompareOp); I(vkCmdSetDepthBoundsTestEnable);
    I(vkCmdSetDepthBiasEnable); I(vkCmdSetDepthBias); I(vkCmdSetCullMode); I(vkCmdSetBlendConstants); I(vkCmdPushConstants); I(vkCmdEndQuery);
    I(vkCmdDispatch); I(vkCmdClearDepthStencilImage); I(vkCmdBindVertexBuffers2); I(vkCmdBindIndexBuffer); I(vkCmdBeginRendering); I(vkCmdBeginQuery);
    I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkUpdateDescriptorSets);
#undef I
    atomic_store(&g_on, 1);
    pthread_t t; if (pthread_create(&t, NULL, encode_thread, NULL)) die("encode thread"); pthread_detach(t);
}
