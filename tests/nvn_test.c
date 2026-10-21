/* NVN layer test (no game data, no JIT): the host's NVN functions called directly, as the game would through its
   stubs. Ports upstream's CPU-only probes (hle_nvn.c dm_nvn_probe_commands, dm_nvn_probe_deferred_binds) and checks
   what the null backend does at submit: event values, counter reports, buffer copies and uniform updates. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/nvn_reg.h"
#include "nvn_obj.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define F(name) ((NVNT_##name)nvn_slot_fn(#name))
void nvn_debug_counts(size_t *copies, size_t *pins);
static size_t copies(void) { size_t c, p; nvn_debug_counts(&c, &p); return c; }

static void cb_init(NVNcommandBuffer *cb, uint8_t *ctl, size_t nctl, pool_t *pool, size_t ncmd) {
    F(nvnCommandBufferInitialize)(cb, NULL);
    F(nvnCommandBufferAddControlMemory)(cb, ctl, nctl);
    F(nvnCommandBufferAddCommandMemory)(cb, (NVNmemoryPool *)pool, 0, ncmd);
}

/* the payloads of every record of one op in a recording, in order */
static unsigned find_ops(NVNcommandHandle h, uint32_t op, const uint8_t **out, unsigned max) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)h; unsigned n = 0;
    for (;;) {
        op_hdr r; memcpy(&r, p, sizeof r);
        if (r.op == OP_END) return n;
        if (r.op == op && n < max) out[n] = p + 8;
        n += r.op == op;
        p += r.size;
    }
}

/* Independently recorded handles can share command memory; handles outlive their recorder; a called / copied
   recording is copied at the call; re-adding control memory retires exactly the recordings made in it. */
static void test_commands(void) {
    NVNcommandBuffer a, b, caller;
    static uint8_t control[3][256], commands[4096];
    pool_t pool = {.mem = commands};
    size_t before = copies();
    cb_init(&a, control[0], sizeof control[0], &pool, sizeof commands);
    cb_init(&b, control[1], sizeof control[1], &pool, sizeof commands);
    cb_init(&caller, control[2], sizeof control[2], &pool, sizeof commands);
    F(nvnCommandBufferBeginRecording)(&a);
    uint8_t *end = ((cmd_t *)&a)->end;
    ((uint8_t *)&a)[0x10] = 1;   /* NVN 55 inline code writes its dirty flag at +0x10 */
    CHECK(((cmd_t *)&a)->end == end);
    F(nvnCommandBufferSetScissor)(&a, 11, 22, 33, 44);
    NVNcommandHandle ha = F(nvnCommandBufferEndRecording)(&a);
    F(nvnCommandBufferFinalize)(&a);   /* handles outlive their recorder */
    F(nvnCommandBufferAddCommandMemory)(&b, (NVNmemoryPool *)&pool, 0, sizeof commands);
    CHECK(copies() == before + 1);     /* shared command memory retires nothing */
    F(nvnCommandBufferBeginRecording)(&b);
    F(nvnCommandBufferSetScissor)(&b, 55, 66, 77, 88);
    NVNcommandHandle hb = F(nvnCommandBufferEndRecording)(&b);
    F(nvnCommandBufferBeginRecording)(&caller);
    F(nvnCommandBufferCallCommands)(&caller, 1, &ha);
    F(nvnCommandBufferCopyCommands)(&caller, 1, &hb);
    NVNcommandHandle hc = F(nvnCommandBufferEndRecording)(&caller);
    F(nvnCommandBufferAddControlMemory)(&b, control[0], sizeof control[0]);   /* retires a's recording only */
    CHECK(copies() == before + 2);
    const uint8_t *sc[3];
    CHECK(find_ops(hc, OP_SCISSOR, sc, 3) == 2);
    const p_scissor want[] = {{11, 22, 33, 44}, {55, 66, 77, 88}};
    CHECK(!memcmp(sc[0], &want[0], sizeof want[0]) && !memcmp(sc[1], &want[1], sizeof want[1]));
    F(nvnCommandBufferAddControlMemory)(&b, control[1], sizeof control[1]);
    F(nvnCommandBufferAddControlMemory)(&caller, control[2], sizeof control[2]);
    F(nvnCommandBufferFinalize)(&b); F(nvnCommandBufferFinalize)(&caller);
    CHECK(copies() == before);   /* no leak */
}

/* Command memory usage is what the real driver's encoding takes (the game sizes heaps from it). */
static void test_usage(void) {
    NVNcommandBuffer cb; static uint8_t ctl[256], cmds[4096]; pool_t pool = {.mem = cmds};
    cb_init(&cb, ctl, sizeof ctl, &pool, sizeof cmds);
    F(nvnCommandBufferBeginRecording)(&cb);
    CHECK(F(nvnCommandBufferGetCommandMemoryUsed)(&cb) == 0);
    F(nvnCommandBufferDrawElements)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 3, 0x1000);
    CHECK(F(nvnCommandBufferGetCommandMemoryUsed)(&cb) == 48);
    NVNbufferRange r[2] = {{0x2000, 64}, {0x3000, 64}};
    F(nvnCommandBufferBindVertexBuffers)(&cb, 0, 2, r);
    CHECK(F(nvnCommandBufferGetCommandMemoryUsed)(&cb) == 48 + 48);
    CHECK(F(nvnCommandBufferGetCommandMemoryFree)(&cb) == sizeof cmds - 96);
    (void)F(nvnCommandBufferEndRecording)(&cb);
    CHECK(F(nvnCommandBufferGetCommandMemoryUsed)(&cb) == 96);
    CHECK(F(nvnCommandBufferGetControlMemoryUsed)(&cb) == 64);
    F(nvnCommandBufferAddControlMemory)(&cb, ctl, sizeof ctl);
    F(nvnCommandBufferFinalize)(&cb);
}

static void queue_init(NVNqueue *q) {
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb);
    F(nvnQueueBuilderSetDefaults)(&qb);
    CHECK(F(nvnQueueBuilderGetQueueMemorySize)(&qb) == ((65536 + 262144 + 16384 + 4095) & ~4095u));
    CHECK(F(nvnQueueInitialize)(q, &qb));
}

/* Deferred binds take the handles the game's array holds at submit, also through CallCommands; immediate binds keep
   the recorded value. */
static void test_deferred(void) {
    typedef void (*bind_fn)(NVNcommandBuffer *, NVNshaderStage, int, int, const NVNtextureHandle *);
    bind_fn deferred = (bind_fn)nvn_slot_fn("nvnCommandBufferBindTexturesDeferred");
    NVNcommandBuffer cb, caller; static uint8_t control[2][256], cmds[4096]; pool_t pool = {.mem = cmds};
    cb_init(&cb, control[0], 256, &pool, sizeof cmds);
    cb_init(&caller, control[1], 256, &pool, sizeof cmds);
    NVNtextureHandle table[2] = {0x10200001, 0x10200002}, immediate = 0x10200003;
    F(nvnCommandBufferBeginRecording)(&cb);
    deferred(&cb, NVN_SHADER_STAGE_FRAGMENT, 4, 2, table);   /* the game fills the table later */
    F(nvnCommandBufferBindTextures)(&cb, NVN_SHADER_STAGE_FRAGMENT, 6, 1, &immediate);
    NVNcommandHandle hd = F(nvnCommandBufferEndRecording)(&cb);
    F(nvnCommandBufferBeginRecording)(&caller);
    F(nvnCommandBufferCallCommands)(&caller, 1, &hd);
    NVNcommandHandle hc = F(nvnCommandBufferEndRecording)(&caller);
    table[0] = 0x10200011; table[1] = 0x10200012; immediate = 0x10200013;   /* written after recording, before submit */
    NVNqueue q; queue_init(&q);
    NVNcommandHandle both[2] = {hd, hc};
    F(nvnQueueSubmitCommands)(&q, 2, both);
    for (int k = 0; k < 2; k++) {
        const uint8_t *b[4]; unsigned n = find_ops(both[k], OP_BIND_TEX, b, 4);
        CHECK(n == 2);
        const p_bind_handles *d = (const void *)b[0], *i = (const void *)b[1];
        CHECK(d->first == 4 && d->n == 2 && d->h[0] == 0x10200011 && d->h[1] == 0x10200012);
        CHECK(i->first == 6 && i->h[0] == 0x10200003);
    }
    F(nvnCommandBufferAddControlMemory)(&cb, control[1], 256);
    F(nvnCommandBufferAddControlMemory)(&caller, control[0], 256);
    F(nvnCommandBufferFinalize)(&cb); F(nvnCommandBufferFinalize)(&caller);
}

/* What the CPU can see of a submit: event values, counter reports, buffer copies, uniform updates, and the fence. */
static void test_null_backend(void) {
    static _Alignas(4096) uint8_t mem[4096];
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = sizeof mem};
    NVNeventBuilder eb; F(nvnEventBuilderSetDefaults)(&eb);
    F(nvnEventBuilderSetStorage)(&eb, (const NVNmemoryPool *)&pool, 0);
    NVNevent ev; CHECK(F(nvnEventInitialize)(&ev, &eb));
    NVNcommandBuffer cb; static uint8_t ctl[256], cmds[4096]; pool_t cpool = {.mem = cmds};
    cb_init(&cb, ctl, sizeof ctl, &cpool, sizeof cmds);
    uint8_t data[16]; for (int i = 0; i < 16; i++) data[i] = (uint8_t)(0xa0 + i);
    uint64_t base = (uint64_t)(uintptr_t)mem;
    memset(mem + 256, 0xee, 16);
    F(nvnCommandBufferBeginRecording)(&cb);
    F(nvnCommandBufferSignalEvent)(&cb, &ev, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 7);
    F(nvnCommandBufferUpdateUniformBuffer)(&cb, base + 64, 64, 0, sizeof data, data);
    F(nvnCommandBufferCopyBufferToBuffer)(&cb, base + 64, base + 128, sizeof data, 0);
    F(nvnCommandBufferReportCounter)(&cb, NVN_COUNTER_TYPE_TIMESTAMP, base + 256);
    NVNcommandHandle h = F(nvnCommandBufferEndRecording)(&cb);
    memset(data, 0, sizeof data);   /* the update took its bytes at record time */
    CHECK(F(nvnEventGetValue)(&ev) == 0 && mem[64] == 0);   /* nothing happens before the submit */
    NVNqueue q; queue_init(&q);
    F(nvnQueueSubmitCommands)(&q, 1, &h);
    CHECK(F(nvnEventGetValue)(&ev) == 7);
    CHECK(mem[64] == 0xa0 && mem[79] == 0xaf);
    CHECK(mem[128] == 0xa0 && mem[143] == 0xaf);
    uint64_t rec[2]; memcpy(rec, mem + 256, sizeof rec);
    CHECK(rec[0] == 0 && rec[1] != 0 && rec[1] != 0xeeeeeeeeeeeeeeeeull);
    F(nvnEventSignal)(&ev, NVN_EVENT_SIGNAL_MODE_ADD, 3);
    CHECK(F(nvnEventGetValue)(&ev) == 10);
    NVNsync s; CHECK(F(nvnSyncInitialize)(&s, NULL));
    CHECK(F(nvnSyncWait)(&s, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);   /* never fenced */
    F(nvnQueueFenceSync)(&q, &s, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    CHECK(F(nvnSyncWait)(&s, 1000000) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    F(nvnCommandBufferAddControlMemory)(&cb, ctl, sizeof ctl);
    F(nvnCommandBufferFinalize)(&cb);
}

/* Texture storage: block-linear layout from upstream nvn_tex.c (256x256 RGBA8: 16 GOBs wide, 2 blocks of 16 GOBs). */
static void test_texture(void) {
    NVNtextureBuilder tb; memset(&tb, 0, sizeof tb);
    F(nvnTextureBuilderSetDefaults)(&tb);
    F(nvnTextureBuilderSetSize2D)(&tb, 256, 256);
    F(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    CHECK(F(nvnTextureBuilderGetStorageSize)(&tb) == 256 * 256 * 4);
    CHECK(F(nvnTextureBuilderGetStorageAlignment)(&tb) >= 512);
    static _Alignas(4096) uint8_t mem[256 * 256 * 4];
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = sizeof mem};
    F(nvnTextureBuilderSetStorage)(&tb, (NVNmemoryPool *)&pool, 0);
    NVNtexture t; memset(&t, 0, sizeof t);
    CHECK(F(nvnTextureInitialize)(&t, &tb));
    CHECK(F(nvnTextureGetWidth)(&t) == 256 && F(nvnTextureGetFormat)(&t) == NVN_FORMAT_RGBA8);
    CHECK(F(nvnTextureGetTextureAddress)(&t) == (NVNtextureAddress)(uintptr_t)mem);
    F(nvnTextureFinalize)(&t);
}

int main(void) {
    test_commands();
    test_usage();
    test_deferred();
    test_null_backend();
    test_texture();
    printf("nvn_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
