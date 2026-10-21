/* Module loader for the JIT track: the game's original NSO modules (main, subsdk0, sdk), mapped into guest memory,
   relocated and linked, as upstream's runtime/loader.c does for the rewritten images. Everything is parsed from the
   NSO files at run time (no generated tables of game data):

     NSO segments (LZ4) -> image at its base: text | rodata | data | bss
     MOD0 -> dynamic table -> dynsym/dynstr, RELA, JMPREL, RELR, DT_INIT/DT_FINI/init array
     relocations: R_AARCH64_ABS64 (257), GLOB_DAT (1025), JUMP_SLOT (1026), RELATIVE (1027) and DT_RELR

   Symbols resolve as upstream does: a host override first (the resolver; on the JIT track it returns a bridge stub
   address), then the module's own definition, then the first module that defines the name. Guest memory is identity
   mapped, so module addresses are host addresses too. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LDR_R_ABS64 = 257, LDR_R_GLOB_DAT = 1025, LDR_R_JUMP_SLOT = 1026, LDR_R_RELATIVE = 1027 };
enum { LDR_STB_LOCAL = 0, LDR_STB_GLOBAL = 1, LDR_STB_WEAK = 2 };

typedef struct {
    const char *name;           /* points into the module's dynstr (guest memory) */
    uint64_t value, size;
    uint8_t defined, binding, type;
} ldr_sym;

typedef struct {
    char name[16];
    uint64_t base, size;        /* mapped image, 4 KiB aligned, bss included */
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    uint8_t build_id[32];
    uint64_t init, fini, init_array, init_array_size, fini_array, fini_array_size;   /* offsets; 0 = none */
    ldr_sym *symbols;
    unsigned nsym;
    unsigned *hash, hash_mask;  /* defined symbols by name */
    /* relocation statistics after linking */
    unsigned relative, symbolic, cross, overridden, weak_zero;
} ldr_module;

/* Resolver for symbolic relocations. phase 0: asked for every symbolic relocation before the modules are searched;
   return a guest address to override the symbol (upstream's runtime_override / runtime_observer), or 0. phase 1:
   the symbol is defined nowhere; return a stand-in address (e.g. a trap stub), or 0 to fail (weak symbols become 0
   without asking). */
typedef uint64_t (*ldr_resolver)(void *user, const ldr_module *m, const char *name, unsigned kind, int phase);

typedef struct ldr_set ldr_set;
ldr_set *ldr_create(void);
void ldr_destroy(ldr_set *s);   /* frees the tables; guest memory belongs to the caller's reservation */

/* Decode an NSO file into guest memory at base (inside a range the caller reserved with pal_reserve_fixed; the
   loader commits it). Returns the module, or NULL with a message in err. */
ldr_module *ldr_map(ldr_set *s, const char *name, const uint8_t *nso, size_t nso_size, uint64_t base,
                    char *err, size_t errcap);
/* Bytes the module will occupy when mapped (4 KiB aligned), from the NSO header alone; 0 if it is not an NSO. */
uint64_t ldr_nso_image_size(const uint8_t *nso, size_t nso_size);

/* Relocate every mapped module, then set segment protections (text r-x, rodata r--, data and bss rw-).
   Returns 0, or -1 with a message in err. */
int ldr_link(ldr_set *s, ldr_resolver resolve, void *user, char *err, size_t errcap);

unsigned ldr_count(const ldr_set *s);
const ldr_module *ldr_module_at(const ldr_set *s, unsigned i);
const ldr_module *ldr_find_module(const ldr_set *s, const char *name);
/* Address of a defined symbol, searching modules in load order; 0 if none. */
uint64_t ldr_symbol(const ldr_set *s, const char *name);
uint64_t ldr_module_symbol(const ldr_module *m, const char *name);
/* "main+0x1234" or "0x..." */
void ldr_describe(const ldr_set *s, uint64_t addr, char *out, size_t cap);
const ldr_module *ldr_module_of(const ldr_set *s, uint64_t addr);

/* LZ4 block decompression (exposed for tests). Returns the number of bytes written, or -1 if the block is invalid
   or does not fit. */
int64_t ldr_lz4_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);

#ifdef __cplusplus
}
#endif
