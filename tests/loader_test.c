/* Loader test: two synthetic NSO modules (no game data) are mapped, relocated and linked, then the JIT calls from one
   into the other through a resolved PLT slot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jit/jit.h"
#include "loader/loader.h"
#include "pal.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define BASE 0x5000000000ull
#define LIB (BASE)
#define APP (BASE + 0x100000)
#define STACK (BASE + 0x400000)

/* Image layout of both test modules: text 0x0000, rodata 0x1000, data 0x2000 (one page each), bss one page. */
enum { TEXT = 0x0000, RO = 0x1000, DATA = 0x2000, SEG = 0x1000, BSS = 0x1000 };

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void sym(uint8_t *img, unsigned i, uint32_t name, unsigned bind, unsigned type, uint16_t shndx, uint64_t value) {
    uint8_t *e = img + RO + 0x100 + 24 * i;
    w32(e, name);
    e[4] = (uint8_t)(bind << 4 | type);
    memcpy(e + 6, &shndx, 2);
    w64(e + 8, value);
}

static void rela(uint8_t *img, uint32_t table, unsigned i, uint64_t where, unsigned kind, uint32_t symbol, int64_t addend) {
    uint8_t *r = img + table + 24 * i;
    w64(r, where);
    w64(r + 8, (uint64_t)symbol << 32 | kind);
    w64(r + 16, (uint64_t)addend);
}

static unsigned ndyn;
static void dyn(uint8_t *img, int64_t tag, uint64_t value) {
    w64(img + DATA + 16 * ndyn, (uint64_t)tag);
    w64(img + DATA + 16 * ndyn + 8, value);
    ndyn++;
}

static void header(uint8_t *img) {
    w32(img + 4, 8);                                /* MOD0 offset */
    memcpy(img + 8, "MOD0", 4);
    w32(img + 12, (uint32_t)(DATA - 8));            /* dynamic table, relative to MOD0 */
    ndyn = 0;
}

/* LZ4 block with literals only (valid, and enough to exercise the compressed-segment path). */
static size_t lz4_literals(const uint8_t *src, size_t n, uint8_t *out) {
    size_t o = 0;
    if (n >= 15) {
        out[o++] = 0xf0;
        size_t rest = n - 15;
        while (rest >= 255) { out[o++] = 255; rest -= 255; }
        out[o++] = (uint8_t)rest;
    } else {
        out[o++] = (uint8_t)(n << 4);
    }
    memcpy(out + o, src, n);
    return o + n;
}

/* NSO file around a 3-page image; the text segment is LZ4 compressed when compress is set. */
static uint8_t *nso(const uint8_t *img, int compress, uint32_t dynsym_off, uint32_t dynsym_size, size_t *size) {
    uint8_t *f = calloc(1, 0x100 + 3 * SEG + 64);
    size_t text_stored = compress ? lz4_literals(img, SEG, f + 0x100) : (memcpy(f + 0x100, img, SEG), SEG);
    memcpy(f, "NSO0", 4);
    w32(f + 0x0c, compress ? 1 : 0);
    uint32_t off = 0x100;
    uint32_t stored[3] = {(uint32_t)text_stored, SEG, SEG};
    for (int i = 0; i < 3; i++) {
        w32(f + 0x10 + 16 * i, off);
        w32(f + 0x14 + 16 * i, (uint32_t)(i * SEG));
        w32(f + 0x18 + 16 * i, SEG);
        if (i) memcpy(f + off, img + i * SEG, SEG);
        off += stored[i];
    }
    w32(f + 0x3c, BSS);
    w32(f + 0x60, (uint32_t)text_stored);
    memcpy(f + 0x40, "synthetic build id", 18);
    w32(f + 0x98, dynsym_off);
    w32(f + 0x9c, dynsym_size);
    *size = off;
    return f;
}

static const char LIB_STR[] = "\0lib_add\0lib_value\0";
static const char APP_STR[] = "\0lib_add\0lib_value\0app_self\0weak_missing\0host_fn\0strong_missing\0";

/* lib: exports lib_add (code) and lib_value (data). GNU-hash style: no DT_HASH, symbol count from the NSO header. */
static uint8_t *make_lib(size_t *size) {
    static uint8_t img[3 * SEG];
    memset(img, 0, sizeof img);
    header(img);
    w32(img + 0x100, 0x8b010000);                   /* lib_add: add x0, x0, x1 */
    w32(img + 0x104, 0xd65f03c0);                   /* ret */
    memcpy(img + RO, LIB_STR, sizeof LIB_STR);
    sym(img, 1, 1, 1, 2, 1, 0x100);                 /* lib_add, global func */
    sym(img, 2, 9, 1, 1, 3, 0x2800);                /* lib_value, global object */
    w64(img + 0x2800, 0x1234);
    dyn(img, 6, RO + 0x100);
    dyn(img, 5, RO);
    dyn(img, 10, sizeof LIB_STR);
    dyn(img, 11, 24);
    dyn(img, 0, 0);
    return nso(img, 0, 0x100, 3 * 24, size);
}

/* app: imports from lib, a weak import nobody defines, a host override, a strong import nobody defines; RELA, JMPREL
   and RELR; text compressed. */
static uint8_t *make_app(size_t *size) {
    static uint8_t img[3 * SEG];
    memset(img, 0, sizeof img);
    header(img);
    uint32_t imm19 = (DATA + 0x100 - 0x100) / 4;
    w32(img + 0x100, 0x58000000u | imm19 << 5 | 16);   /* ldr x16, <GOT slot of lib_add at data+0x100> */
    w32(img + 0x104, 0xd61f0200);                      /* br x16 */
    w32(img + 0x200, 0xd4001fe1);                      /* svc #0xff: the test's return trap */
    memcpy(img + RO, APP_STR, sizeof APP_STR);
    sym(img, 1, 1, 1, 2, 0, 0);                     /* lib_add (import) */
    sym(img, 2, 9, 1, 1, 0, 0);                     /* lib_value (import) */
    sym(img, 3, 19, 1, 1, 3, 0x2400);               /* app_self */
    sym(img, 4, 28, 2, 0, 0, 0);                    /* weak_missing */
    sym(img, 5, 41, 1, 2, 0, 0);                    /* host_fn */
    sym(img, 6, 49, 1, 2, 0, 0);                    /* strong_missing */
    w32(img + RO + 0x600 + 4, 7);                   /* DT_HASH: nbucket 0 (unused here), nchain = 7 symbols */
    rela(img, RO + 0x300, 0, DATA + 0x108, 1025, 2, 0);
    rela(img, RO + 0x300, 1, DATA + 0x110, 257, 3, 0x10);
    rela(img, RO + 0x300, 2, DATA + 0x118, 1027, 0, 0x2400);
    rela(img, RO + 0x300, 3, DATA + 0x120, 1025, 4, 0);
    rela(img, RO + 0x300, 4, DATA + 0x128, 1025, 5, 0);
    rela(img, RO + 0x300, 5, DATA + 0x130, 1025, 6, 0);
    rela(img, RO + 0x400, 0, DATA + 0x100, 1026, 1, 0x55);    /* JUMP_SLOT ignores its addend */
    w64(img + RO + 0x500, DATA + 0x200);            /* RELR: address entry */
    w64(img + RO + 0x508, 1 | 1u << 1 | 1u << 3);   /* bitmap: data+0x208 and data+0x218 */
    w64(img + DATA + 0x200, 0x2400);
    w64(img + DATA + 0x208, 0x100);
    w64(img + DATA + 0x218, 0x1000);
    dyn(img, 6, RO + 0x100);
    dyn(img, 5, RO);
    dyn(img, 10, sizeof APP_STR);
    dyn(img, 11, 24);
    dyn(img, 4, RO + 0x600);
    dyn(img, 7, RO + 0x300);
    dyn(img, 8, 6 * 24);
    dyn(img, 9, 24);
    dyn(img, 23, RO + 0x400);
    dyn(img, 2, 24);
    dyn(img, 20, 7);
    dyn(img, 36, RO + 0x500);
    dyn(img, 35, 16);
    dyn(img, 37, 8);
    dyn(img, 12, 0x180);
    dyn(img, 0, 0);
    return nso(img, 1, 0, 0, size);
}

static int resolver_calls;
static uint64_t resolve(void *user, const ldr_module *m, const char *name, unsigned kind, int phase) {
    (void)user; (void)m; (void)kind;
    resolver_calls++;
    if (phase == 0 && !strcmp(name, "host_fn")) return 0xabcd000;
    if (phase == 1 && !strcmp(name, "strong_missing")) return 0x5555000;
    return 0;
}

static uint64_t at(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }

static void test_lz4(void) {
    /* "abc", then a match of 20 bytes at offset 3, then the literals "xyzzy" */
    const uint8_t block[] = {0x3f, 'a', 'b', 'c', 3, 0, 1, 0x50, 'x', 'y', 'z', 'z', 'y'};
    uint8_t out[64], want[28];
    for (int i = 0; i < 23; i++) want[i] = "abc"[i % 3];
    memcpy(want + 23, "xyzzy", 5);
    CHECK(ldr_lz4_decode(block, sizeof block, out, sizeof out) == 28);
    CHECK(!memcmp(out, want, 28));
    CHECK(ldr_lz4_decode(block, sizeof block, out, 20) == -1);           /* does not fit */
    const uint8_t bad_offset[] = {0x10, 'a', 5, 0};
    CHECK(ldr_lz4_decode(bad_offset, sizeof bad_offset, out, sizeof out) == -1);
}

int main(void) {
    char err[256] = "";
    test_lz4();
    CHECK(pal_reserve_fixed(BASE, 64ull << 20) == 0);

    size_t lib_size, app_size;
    uint8_t *lib_nso = make_lib(&lib_size), *app_nso = make_app(&app_size);
    CHECK(ldr_nso_image_size(app_nso, app_size) == 4 * SEG);

    ldr_set *s = ldr_create();
    CHECK(ldr_map(s, "bad", app_nso, 0x80, APP, err, sizeof err) == NULL);
    CHECK(strstr(err, "not an NSO") != NULL);
    ldr_module *lib = ldr_map(s, "lib", lib_nso, lib_size, LIB, err, sizeof err);
    ldr_module *app = ldr_map(s, "app", app_nso, app_size, APP, err, sizeof err);
    CHECK(lib && app);
    if (!lib || !app) { fprintf(stderr, "map: %s\n", err); return 1; }
    CHECK(lib->nsym == 3 && app->nsym == 7);
    CHECK(app->init == 0x180 && app->bss_size == BSS && app->size == 4 * SEG);
    CHECK(!memcmp(app->build_id, "synthetic build id", 18));
    CHECK(at(APP + 0x100) == ((uint64_t)0xd61f0200 << 32 | (0x58000000u | (DATA / 4) << 5 | 16)));  /* decompressed */

    err[0] = 0;
    CHECK(ldr_link(s, resolve, NULL, err, sizeof err) == 0);
    if (*err) fprintf(stderr, "link: %s\n", err);
    CHECK(at(APP + DATA + 0x100) == LIB + 0x100);              /* JUMP_SLOT lib_add, cross-module, addend ignored */
    CHECK(at(APP + DATA + 0x108) == LIB + 0x2800);             /* GLOB_DAT lib_value */
    CHECK(at(APP + DATA + 0x110) == APP + 0x2400 + 0x10);      /* ABS64 own symbol + addend */
    CHECK(at(APP + DATA + 0x118) == APP + 0x2400);             /* RELA RELATIVE */
    CHECK(at(APP + DATA + 0x120) == 0);                        /* weak, defined nowhere */
    CHECK(at(APP + DATA + 0x128) == 0xabcd000);                /* host override */
    CHECK(at(APP + DATA + 0x130) == 0x5555000);                /* stand-in for a strong import defined nowhere */
    CHECK(at(APP + DATA + 0x200) == APP + 0x2400);             /* RELR address entry */
    CHECK(at(APP + DATA + 0x208) == APP + 0x100);              /* RELR bitmap */
    CHECK(at(APP + DATA + 0x210) == 0);                        /* bit clear: untouched */
    CHECK(at(APP + DATA + 0x218) == APP + 0x1000);
    CHECK(app->relative == 4 && app->symbolic == 6 && app->cross == 2 && app->overridden == 1 && app->weak_zero == 1);
    CHECK(resolver_calls == 7);   /* phase 0 for each of the 6 symbolic relocations, phase 1 only for strong_missing */

    CHECK(ldr_symbol(s, "lib_add") == LIB + 0x100);
    CHECK(ldr_symbol(s, "app_self") == APP + 0x2400);
    CHECK(ldr_symbol(s, "host_fn") == 0);
    CHECK(ldr_find_module(s, "app") == app && ldr_module_of(s, APP + 0x2fff) == app);
    char where[64];
    ldr_describe(s, APP + 0x104, where, sizeof where);
    CHECK(!strcmp(where, "app+0x104"));
    ldr_describe(s, 0x1234, where, sizeof where);
    CHECK(!strcmp(where, "0x1234"));

    /* protections: text and rodata are not writable, data is */
    uint32_t word = 0;
    CHECK(pal_guarded_copy((void *)(uintptr_t)(APP + 0x100), &word, 4) == 1);
    CHECK(pal_guarded_copy((void *)(uintptr_t)(APP + RO), &word, 4) == 1);
    CHECK(pal_guarded_copy((void *)(uintptr_t)(APP + DATA + 0x800), &word, 4) == 0);
    CHECK(pal_guarded_copy(&word, (void *)(uintptr_t)(APP + 0x100), 4) == 0);

    /* run app code: ldr x16 from its GOT, br into lib_add, return to the trap */
    CHECK(pal_commit_private(STACK, 0x10000, PAL_PROT_READ | PAL_PROT_WRITE) == 0);
    jit_core *core = jit_core_create(NULL, NULL, 0);
    jit_context c;
    memset(&c, 0, sizeof c);
    c.x[0] = 40;
    c.x[1] = 2;
    c.x[30] = APP + 0x200;
    c.sp = STACK + 0x10000;
    c.pc = APP + 0x100;
    jit_load(core, &c);
    int stop = jit_run(core);
    jit_save(core, &c);
    CHECK(stop == JIT_STOP_SVC && jit_svc_number(core) == 0xff);
    CHECK(c.x[0] == 42);
    jit_core_destroy(core);

    /* a strong import with no resolver fallback fails, naming the symbol */
    ldr_set *s2 = ldr_create();
    CHECK(ldr_map(s2, "app2", app_nso, app_size, APP + 0x200000, err, sizeof err) != NULL);
    CHECK(ldr_link(s2, NULL, NULL, err, sizeof err) == -1);
    CHECK(strstr(err, "unresolved symbol") != NULL);
    ldr_destroy(s2);

    ldr_destroy(s);
    free(lib_nso);
    free(app_nso);
    printf("loader: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
