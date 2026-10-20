/* Bridge test: ARM64 code calls host functions through generated thunks (AAPCS64 in, Windows x64 out). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge/bridge.h"
#include "bridge_fixture.h"
#include "guest/programs.h"
#include "pal.h"

int64_t fx_mix(const void *p, int a, float x, uint64_t b, double y, int c, int d, int e, int f, mode_t_ m, int h);
float fx_mul(float a, float b);
small_t fx_small(small_t s, int k);
big_t fx_big(int k);
uint8_t fx_flag(uint32_t v);
void fx_void(int *out, int v);
extern const bridge_entry bridge_fixture_table[];
extern const unsigned bridge_fixture_count;

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define BASE 0x4000000000ull
#define SIZE (16ull << 20)
#define STUBS (BASE + (1ull << 20))
#define DATA (BASE + (2ull << 20))
#define STACK (BASE + (8ull << 20))

static bridge *br;
static jit_core *core;

static bridge_thunk thunk_of(const char *name) {
    for (unsigned i = 0; i < bridge_fixture_count; i++)
        if (!strcmp(bridge_fixture_table[i].name, name)) return bridge_fixture_table[i].thunk;
    return NULL;
}

/* Run p_call with ctx (x9 = the stub), dispatching bridge svcs, until svc #0xff. */
static void guest_call(jit_context *c, uint64_t stub) {
    c->pc = BASE + GUEST_P_CALL;
    c->x[9] = stub;
    jit_load(core, c);
    for (;;) {
        int stop = jit_run(core);
        jit_save(core, c);
        if (stop != JIT_STOP_SVC) { CHECK(!"unexpected stop"); return; }
        uint32_t n = jit_svc_number(core);
        if (n == 0xff) return;
        CHECK(bridge_dispatch(br, n, c));
        jit_load(core, c);
    }
}

static void fbits(uint64_t v[2], float x) { v[0] = v[1] = 0; memcpy(&v[0], &x, 4); }
static void dbits(uint64_t v[2], double x) { v[0] = v[1] = 0; memcpy(&v[0], &x, 8); }

int main(void) {
    CHECK(pal_reserve_fixed(BASE, 1ull << 30) == 0);
    pal_section *s = pal_section_create(SIZE);
    CHECK(pal_map_view(s, 0, BASE, SIZE, PAL_PROT_READ | PAL_PROT_WRITE) == BASE);
    CHECK(pal_commit(BASE, SIZE, PAL_PROT_READ | PAL_PROT_WRITE) == 0);
    memcpy((void *)(uintptr_t)BASE, guest_code, sizeof guest_code);
    br = bridge_create(STUBS, 4096);
    jit_config cfg = {0};
    cfg.fastmem = 1;
    core = jit_core_create(&cfg, NULL, 0);

    uint64_t mix = bridge_add(br, "fx_mix", thunk_of("fx_mix"), (void *)fx_mix);
    uint64_t mul = bridge_add(br, "fx_mul", thunk_of("fx_mul"), (void *)fx_mul);
    uint64_t sml = bridge_add(br, "fx_small", thunk_of("fx_small"), (void *)fx_small);
    uint64_t big = bridge_add(br, "fx_big", thunk_of("fx_big"), (void *)fx_big);
    uint64_t flg = bridge_add(br, "fx_flag", thunk_of("fx_flag"), (void *)fx_flag);
    uint64_t vod = bridge_add(br, "fx_void", thunk_of("fx_void"), (void *)fx_void);
    CHECK(mix && mul && sml && big && flg && vod);
    CHECK(bridge_name(br, BRIDGE_SVC_BASE + 1) && !strcmp(bridge_name(br, BRIDGE_SVC_BASE + 1), "fx_mul"));

    /* 9 integer arguments (the 9th on the guest stack) and 2 floats */
    jit_context c = {0};
    c.sp = STACK;
    c.x[0] = 0x1000; c.x[1] = 2; c.x[2] = 40; c.x[3] = 1; c.x[4] = 2; c.x[5] = 3; c.x[6] = 4; c.x[7] = MODE_B;
    fbits(c.v[0], 1.5f); dbits(c.v[1], 0.25);
    *(uint64_t *)(uintptr_t)STACK = 6;
    guest_call(&c, mix);
    CHECK((int64_t)c.x[0] == fx_mix((void *)0x1000, 2, 1.5f, 40, 0.25, 1, 2, 3, 4, MODE_B, 6));
    CHECK(c.pc == BASE + GUEST_P_CALL + 8);   /* returned to the caller and reached svc #0xff */

    memset(&c, 0, sizeof c); c.sp = STACK;
    fbits(c.v[0], 3.0f); fbits(c.v[1], 0.5f);
    guest_call(&c, mul);
    float r; memcpy(&r, &c.v[0][0], 4);
    CHECK(r == 1.5f);

    memset(&c, 0, sizeof c); c.sp = STACK; c.x[0] = 21; c.x[1] = 4;
    guest_call(&c, sml);
    CHECK(c.x[0] == 46);

    memset(&c, 0, sizeof c); c.sp = STACK; c.x[0] = 10; c.x[8] = DATA;
    guest_call(&c, big);
    uint64_t *b = (uint64_t *)(uintptr_t)DATA;
    CHECK(b[0] == 10 && b[1] == 11 && b[2] == 12 && b[3] == 13);

    memset(&c, 0, sizeof c); c.sp = STACK; c.x[0] = 9;
    guest_call(&c, flg);
    CHECK(c.x[0] == 1);

    memset(&c, 0, sizeof c); c.sp = STACK; c.x[0] = DATA + 64; c.x[1] = 77;
    guest_call(&c, vod);
    CHECK(*(int *)(uintptr_t)(DATA + 64) == 77);

    /* the generated NVN table is sorted and found by name */
    CHECK(bridge_nvn_count > 300);
    CHECK(bridge_nvn_thunk("nvnSamplerBuilderSetLodClamp") != NULL);
    CHECK(bridge_nvn_thunk("nvnNoSuchFunction") == NULL);
    int sorted = 1;
    for (unsigned i = 1; i < bridge_nvn_count; i++) sorted &= strcmp(bridge_nvn_table[i - 1].name, bridge_nvn_table[i].name) < 0;
    CHECK(sorted);

    printf("bridge_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
