#include "loader/loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal.h"

#define PAGE 0x1000u
#define MAX_MODULES 16

struct ldr_set {
    ldr_module m[MAX_MODULES];
    unsigned n;
};

static uint64_t page_up(uint64_t x) { return (x + PAGE - 1) & ~(uint64_t)(PAGE - 1); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static int fail(char *err, size_t cap, const char *fmt, ...) {
    if (err && cap) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(err, cap, fmt, a);
        va_end(a);
    }
    return -1;
}

/* ---- LZ4 block format ---------------------------------------------------------------------------------------- */

int64_t ldr_lz4_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (i >= n) return -1;
                b = src[i++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - i || lit > cap - o) return -1;
        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i == n) break;                   /* the last sequence has literals only */
        if (n - i < 2) return -1;
        size_t off = src[i] | (size_t)src[i + 1] << 8;
        i += 2;
        if (!off || off > o) return -1;
        size_t len = (token & 15) + 4;
        if ((token & 15) == 15) {
            unsigned b;
            do {
                if (i >= n) return -1;
                b = src[i++];
                len += b;
            } while (b == 255);
        }
        if (len > cap - o) return -1;
        for (size_t k = 0; k < len; k++, o++) dst[o] = dst[o - off];   /* overlapping copies repeat the pattern */
    }
    return (int64_t)o;
}

/* ---- NSO --------------------------------------------------------------------------------------------------------- */

typedef struct { uint32_t file_off, mem_off, size, stored; } seg;

static int nso_header(const uint8_t *f, size_t n, seg s[3], uint32_t *bss, uint32_t *flags) {
    if (n < 0x100 || memcmp(f, "NSO0", 4)) return -1;
    *flags = rd32(f + 0x0c);
    for (int i = 0; i < 3; i++) {
        s[i].file_off = rd32(f + 0x10 + 16 * i);
        s[i].mem_off = rd32(f + 0x14 + 16 * i);
        s[i].size = rd32(f + 0x18 + 16 * i);
        s[i].stored = (*flags >> i & 1) ? rd32(f + 0x60 + 4 * i) : s[i].size;
    }
    *bss = rd32(f + 0x3c);
    return 0;
}

uint64_t ldr_nso_image_size(const uint8_t *nso, size_t n) {
    seg s[3];
    uint32_t bss, flags;
    if (nso_header(nso, n, s, &bss, &flags)) return 0;
    return page_up((uint64_t)s[2].mem_off + s[2].size + bss);
}

/* ---- set --------------------------------------------------------------------------------------------------------- */

ldr_set *ldr_create(void) { return (ldr_set *)calloc(1, sizeof(ldr_set)); }

void ldr_destroy(ldr_set *s) {
    if (!s) return;
    for (unsigned i = 0; i < s->n; i++) { free(s->m[i].symbols); free(s->m[i].hash); }
    free(s);
}

unsigned ldr_count(const ldr_set *s) { return s->n; }
const ldr_module *ldr_module_at(const ldr_set *s, unsigned i) { return i < s->n ? &s->m[i] : NULL; }

const ldr_module *ldr_find_module(const ldr_set *s, const char *name) {
    for (unsigned i = 0; i < s->n; i++) if (!strcmp(s->m[i].name, name)) return &s->m[i];
    return NULL;
}

static unsigned hash_name(const char *p) {
    unsigned h = 2166136261u;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static int make_hash(ldr_module *m) {
    unsigned cap = 16;
    while (cap < m->nsym * 2) cap <<= 1;
    m->hash = (unsigned *)calloc(cap, sizeof(unsigned));
    if (!m->hash) return -1;
    m->hash_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        if (!m->symbols[i].defined || m->symbols[i].binding == LDR_STB_LOCAL) continue;
        unsigned p = hash_name(m->symbols[i].name) & m->hash_mask;
        while (m->hash[p] && strcmp(m->symbols[m->hash[p]].name, m->symbols[i].name)) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
    return 0;
}

uint64_t ldr_module_symbol(const ldr_module *m, const char *name) {
    if (!m->hash) return 0;
    unsigned p = hash_name(name) & m->hash_mask;
    while (m->hash[p]) {
        const ldr_sym *s = &m->symbols[m->hash[p]];
        if (!strcmp(s->name, name)) return s->value < m->size ? m->base + s->value : 0;
        p = (p + 1) & m->hash_mask;
    }
    return 0;
}

uint64_t ldr_symbol(const ldr_set *s, const char *name) {
    for (unsigned i = 0; i < s->n; i++) {
        uint64_t a = ldr_module_symbol(&s->m[i], name);
        if (a) return a;
    }
    return 0;
}

const ldr_module *ldr_module_of(const ldr_set *s, uint64_t a) {
    for (unsigned i = 0; i < s->n; i++) if (a >= s->m[i].base && a - s->m[i].base < s->m[i].size) return &s->m[i];
    return NULL;
}

void ldr_describe(const ldr_set *s, uint64_t a, char *out, size_t cap) {
    const ldr_module *m = ldr_module_of(s, a);
    if (m) snprintf(out, cap, "%s+0x%llx", m->name, (unsigned long long)(a - m->base));
    else snprintf(out, cap, "0x%llx", (unsigned long long)a);
}

const char *ldr_nearest_symbol(const ldr_set *s, uint64_t a, uint64_t *off) {
    const ldr_module *m = ldr_module_of(s, a);
    if (!m) return NULL;
    uint64_t rel = a - m->base, best = 0;
    const char *name = NULL;
    for (unsigned i = 1; i < m->nsym; i++) {
        const ldr_sym *y = &m->symbols[i];
        if (!y->defined || y->type != 2 || y->value > rel || y->value < best || y->value >= m->ro_off) continue;
        if (y->size && rel >= y->value + y->size) continue;   /* past the end of a sized function */
        best = y->value;
        name = y->name;
    }
    if (name && off) *off = rel - best;
    return name;
}

/* ---- mapping ----------------------------------------------------------------------------------------------------- */

#define DT_NULL 0
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_PLTRELSZ 2
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37
#define NTAGS 38

typedef struct { uint64_t v[NTAGS]; uint8_t has[NTAGS]; } dyn_tags;

static int read_dynamic(const ldr_module *m, dyn_tags *t, char *err, size_t cap) {
    const uint8_t *img = (const uint8_t *)(uintptr_t)m->base;
    uint32_t mod0 = rd32(img + 4);
    if ((uint64_t)mod0 + 8 > m->size || memcmp(img + mod0, "MOD0", 4))
        return fail(err, cap, "%s: no MOD0 header", m->name);
    int64_t dyn = (int64_t)mod0 + (int32_t)rd32(img + mod0 + 4);
    memset(t, 0, sizeof *t);
    for (int i = 0;; i++) {
        uint64_t at = (uint64_t)dyn + 16u * (unsigned)i;
        if (dyn < 0 || i > 4096 || at + 16 > m->size) return fail(err, cap, "%s: bad dynamic table", m->name);
        int64_t tag = (int64_t)rd64(img + at);
        if (tag == DT_NULL) break;
        if (tag >= 0 && tag < NTAGS) { t->v[tag] = rd64(img + at + 8); t->has[tag] = 1; }
    }
    return 0;
}

static int in_image(const ldr_module *m, uint64_t off, uint64_t len) { return off <= m->size && len <= m->size - off; }

static int read_symbols(ldr_module *m, const dyn_tags *t, const uint8_t *nso, char *err, size_t cap) {
    const uint8_t *img = (const uint8_t *)(uintptr_t)m->base;
    if (!t->has[DT_SYMTAB] || !t->has[DT_STRTAB] || !t->has[DT_STRSZ] || (t->has[DT_SYMENT] && t->v[DT_SYMENT] != 24))
        return fail(err, cap, "%s: dynamic symbols missing", m->name);
    uint64_t nsym;
    if (t->has[DT_HASH]) {
        if (!in_image(m, t->v[DT_HASH], 8)) return fail(err, cap, "%s: bad DT_HASH", m->name);
        nsym = rd32(img + t->v[DT_HASH] + 4);
    } else {
        /* GNU hash only (newer SDKs): the NSO header has the dynsym extent, relative to rodata. */
        uint32_t off = rd32(nso + 0x98), size = rd32(nso + 0x9c);
        if (m->ro_off + off != t->v[DT_SYMTAB] || size % 24) return fail(err, cap, "%s: no symbol count", m->name);
        nsym = size / 24;
    }
    uint64_t strtab = t->v[DT_STRTAB], strsz = t->v[DT_STRSZ];
    if (!nsym || nsym > 1u << 22 || !in_image(m, t->v[DT_SYMTAB], nsym * 24) || !in_image(m, strtab, strsz) || !strsz ||
        img[strtab + strsz - 1] != 0)
        return fail(err, cap, "%s: bad symbol table", m->name);
    m->symbols = (ldr_sym *)calloc(nsym, sizeof(ldr_sym));
    if (!m->symbols) return fail(err, cap, "out of memory");
    m->nsym = (unsigned)nsym;
    for (unsigned i = 0; i < m->nsym; i++) {
        const uint8_t *e = img + t->v[DT_SYMTAB] + 24u * i;
        uint32_t name = rd32(e);
        if (name >= strsz) return fail(err, cap, "%s: bad symbol name", m->name);
        ldr_sym *s = &m->symbols[i];
        s->name = (const char *)(img + strtab + name);
        s->binding = e[4] >> 4;
        s->type = e[4] & 15;
        s->defined = (e[6] | e[7] << 8) != 0;
        s->value = rd64(e + 8);
        s->size = rd64(e + 16);
    }
    return make_hash(m) ? fail(err, cap, "out of memory") : 0;
}

ldr_module *ldr_map(ldr_set *s, const char *name, const uint8_t *nso, size_t n, uint64_t base, char *err, size_t cap) {
    seg sg[3];
    uint32_t bss, flags;
    if (s->n >= MAX_MODULES) { fail(err, cap, "too many modules"); return NULL; }
    if (nso_header(nso, n, sg, &bss, &flags)) { fail(err, cap, "%s: not an NSO file", name); return NULL; }
    if (base % PAGE || sg[0].mem_off != 0) { fail(err, cap, "%s: bad base or text offset", name); return NULL; }
    for (int i = 0; i < 3; i++) {
        if ((uint64_t)sg[i].file_off + sg[i].stored > n || sg[i].size > 512u << 20 || sg[i].mem_off % PAGE ||
            (i && sg[i].mem_off < (uint64_t)sg[i - 1].mem_off + sg[i - 1].size)) {
            fail(err, cap, "%s: bad segment %d", name, i);
            return NULL;
        }
    }
    ldr_module *m = &s->m[s->n];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base;
    m->size = ldr_nso_image_size(nso, n);
    m->text_off = sg[0].mem_off; m->text_size = sg[0].size;
    m->ro_off = sg[1].mem_off;   m->ro_size = sg[1].size;
    m->data_off = sg[2].mem_off; m->data_size = sg[2].size;
    m->bss_size = bss;
    memcpy(m->build_id, nso + 0x40, 32);
    if (pal_commit_private((uintptr_t)base, (size_t)m->size, PAL_PROT_READ | PAL_PROT_WRITE)) {
        fail(err, cap, "%s: cannot commit %#llx bytes at %#llx", name, (unsigned long long)m->size, (unsigned long long)base);
        return NULL;
    }
    uint8_t *img = (uint8_t *)(uintptr_t)base;
    for (int i = 0; i < 3; i++) {
        const uint8_t *src = nso + sg[i].file_off;
        if (flags >> i & 1) {
            if (ldr_lz4_decode(src, sg[i].stored, img + sg[i].mem_off, sg[i].size) != (int64_t)sg[i].size) {
                fail(err, cap, "%s: segment %d does not decompress", name, i);
                goto undo;
            }
        } else {
            memcpy(img + sg[i].mem_off, src, sg[i].size);
        }
    }
    dyn_tags t;
    if (read_dynamic(m, &t, err, cap) || read_symbols(m, &t, nso, err, cap)) goto undo;
    m->init = t.v[DT_INIT];
    m->fini = t.v[DT_FINI];
    m->init_array = t.v[DT_INIT_ARRAY];
    m->init_array_size = t.v[DT_INIT_ARRAYSZ];
    m->fini_array = t.v[DT_FINI_ARRAY];
    m->fini_array_size = t.v[DT_FINI_ARRAYSZ];
    s->n++;
    return m;
undo:
    free(m->symbols);
    free(m->hash);
    pal_release_private((uintptr_t)base, (size_t)m->size);
    memset(m, 0, sizeof *m);
    return NULL;
}

/* ---- linking ----------------------------------------------------------------------------------------------------- */

static int put(ldr_module *m, uint64_t where, uint64_t value, char *err, size_t cap) {
    /* As upstream: relocations only write into the data segment (the GOT and .data.rel.ro live there). */
    if (where % 8 || where < m->data_off || !in_image(m, where, 8))
        return fail(err, cap, "%s: relocation outside data at +%#llx", m->name, (unsigned long long)where);
    memcpy((void *)(uintptr_t)(m->base + where), &value, 8);
    return 0;
}

static int symbolic(ldr_set *s, ldr_module *m, unsigned kind, uint64_t where, uint32_t si, int64_t addend,
                    ldr_resolver resolve, void *user, char *err, size_t cap) {
    if (!si || si >= m->nsym) return fail(err, cap, "%s: bad relocation symbol %u", m->name, si);
    const ldr_sym *sym = &m->symbols[si];
    uint64_t p = resolve ? resolve(user, m, sym->name, kind, 0) : 0;
    if (p) m->overridden++;
    if (!p && sym->defined) p = sym->value < m->size ? m->base + sym->value : 0;
    if (!p) {
        p = ldr_symbol(s, sym->name);
        if (p) m->cross++;
    }
    if (!p) {
        if (sym->binding == LDR_STB_WEAK) m->weak_zero++;
        else if (!(resolve && (p = resolve(user, m, sym->name, kind, 1))))
            return fail(err, cap, "%s: unresolved symbol %s", m->name, sym->name);
    }
    m->symbolic++;
    /* JUMP_SLOT ignores the addend (as upstream); GLOB_DAT and ABS64 add it. A weak zero stays zero. */
    uint64_t v = p ? p + (kind == LDR_R_JUMP_SLOT ? 0 : (uint64_t)addend) : 0;
    return put(m, where, v, err, cap);
}

static int relocate(ldr_set *s, ldr_module *m, ldr_resolver resolve, void *user, char *err, size_t cap) {
    dyn_tags t;
    if (read_dynamic(m, &t, err, cap)) return -1;
    uint8_t *img = (uint8_t *)(uintptr_t)m->base;
    /* DT_RELR: an address entry, then bitmaps of the following 63 words; each listed word gets base added. */
    if (t.has[DT_RELR]) {
        if (t.v[DT_RELRENT] != 8 || t.v[DT_RELRSZ] % 8 || !in_image(m, t.v[DT_RELR], t.v[DT_RELRSZ]))
            return fail(err, cap, "%s: bad DT_RELR", m->name);
        uint64_t cursor = 0;
        int started = 0;
        for (uint64_t o = 0; o < t.v[DT_RELRSZ]; o += 8) {
            uint64_t e = rd64(img + t.v[DT_RELR] + o), where[64];
            int count = 0;
            if (!(e & 1)) {
                where[count++] = e;
                cursor = e + 8;
                started = 1;
            } else {
                if (!started) return fail(err, cap, "%s: RELR bitmap before an address", m->name);
                for (int bit = 1; bit < 64; bit++) if (e >> bit & 1) where[count++] = cursor + (uint64_t)(bit - 1) * 8;
                cursor += 63 * 8;
            }
            for (int k = 0; k < count; k++) {
                if (!in_image(m, where[k], 8)) return fail(err, cap, "%s: bad RELR address", m->name);
                uint64_t addend = rd64(img + where[k]);
                if (addend >= m->size) return fail(err, cap, "%s: RELR addend outside module", m->name);
                if (put(m, where[k], m->base + addend, err, cap)) return -1;
                m->relative++;
            }
        }
    }
    struct { int table, size; } lists[2] = {{DT_RELA, DT_RELASZ}, {DT_JMPREL, DT_PLTRELSZ}};
    if (t.has[DT_RELA] && t.has[DT_RELAENT] && t.v[DT_RELAENT] != 24) return fail(err, cap, "%s: RELAENT", m->name);
    if (t.has[DT_JMPREL] && t.has[DT_PLTREL] && t.v[DT_PLTREL] != 7) return fail(err, cap, "%s: PLT not RELA", m->name);
    for (int l = 0; l < 2; l++) {
        if (!t.has[lists[l].table]) continue;
        uint64_t at = t.v[lists[l].table], size = t.v[lists[l].size];
        if (size % 24 || !in_image(m, at, size)) return fail(err, cap, "%s: bad relocation table", m->name);
        for (uint64_t o = 0; o < size; o += 24) {
            const uint8_t *r = img + at + o;
            uint64_t where = rd64(r), info = rd64(r + 8);
            int64_t addend = (int64_t)rd64(r + 16);
            unsigned kind = (unsigned)(info & 0xffffffff);
            uint32_t si = (uint32_t)(info >> 32);
            if (kind == LDR_R_RELATIVE) {
                if (addend < 0 || (uint64_t)addend >= m->size) return fail(err, cap, "%s: relative addend", m->name);
                if (put(m, where, m->base + (uint64_t)addend, err, cap)) return -1;
                m->relative++;
            } else if (kind == LDR_R_ABS64 || kind == LDR_R_GLOB_DAT || kind == LDR_R_JUMP_SLOT) {
                if (symbolic(s, m, kind, where, si, addend, resolve, user, err, cap)) return -1;
            } else {
                return fail(err, cap, "%s: relocation kind %u", m->name, kind);
            }
        }
    }
    return 0;
}

static int protect(ldr_module *m, char *err, size_t cap) {
    uint64_t rx_end = m->ro_off, ro_end = m->data_off;
    if (pal_protect((uintptr_t)m->base, (size_t)rx_end, PAL_PROT_READ | PAL_PROT_EXEC) ||
        (ro_end > rx_end && pal_protect((uintptr_t)(m->base + rx_end), (size_t)(ro_end - rx_end), PAL_PROT_READ)))
        return fail(err, cap, "%s: cannot set segment protections", m->name);
    return 0;
}

int ldr_link(ldr_set *s, ldr_resolver resolve, void *user, char *err, size_t cap) {
    for (unsigned i = 0; i < s->n; i++) if (relocate(s, &s->m[i], resolve, user, err, cap)) return -1;
    for (unsigned i = 0; i < s->n; i++) if (protect(&s->m[i], err, cap)) return -1;
    return 0;
}
