/* nn::pl shared fonts: the six verified system BFTTF files laid out back to back, as in the pl shared memory.
   Ported from upstream games/totk/runtime/shared_font.c. GetSharedFontAddress points 8 bytes past each file start
   (after the BFTTF header), as the SDK adds the service-provided offset to the shared memory base; the size is the
   decoded font size.

   Upstream compiles the font list into the program (scripts/prepare_shared_fonts.py); here it is read from the
   manifest tools/build_win.py writes next to the fonts (<data>/firmware/shared-fonts/manifest.json), and every file is
   checked against its SHA-256 before the game sees it. The fonts live in the host arena: the game reads them. */
#include <pthread.h>

#include "host/hos.h"

#define FONT_COUNT 6
typedef struct { char name[64], sha256[65]; uint64_t file_bytes, font_bytes; } FontRow;
static FontRow rows[FONT_COUNT];
static uint8_t *region;
static uint64_t offsets[FONT_COUNT];
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* The value of "key" in a flat JSON object [obj, end): a string (into out) or an unsigned number (*num). */
static int json_field(const char *obj, const char *end, const char *key, char *out, size_t cap, uint64_t *num) {
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return 0;
    p += strlen(pat);
    while (p < end && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (out) {
        if (*p != '"') return 0;
        const char *q = memchr(p + 1, '"', (size_t)(end - p - 1));
        if (!q || (size_t)(q - p - 1) >= cap) return 0;
        memcpy(out, p + 1, (size_t)(q - p - 1)); out[q - p - 1] = 0;
        return 1;
    }
    char *e; unsigned long long v = strtoull(p, &e, 10);
    if (e == p) return 0;
    *num = v; return 1;
}

static void read_manifest(const char *dir) {
    char path[1200]; snprintf(path, sizeof path, "%s/manifest.json", dir);
    pal_file *f = pal_file_open(path, PAL_FILE_READ);
    if (!f) die("shared fonts: cannot open %s (run tools/build_win.py)", path);
    int64_t n = pal_file_size(f);
    char *text = n > 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL;
    if (!text || pal_file_pread(f, text, (size_t)n, 0) != n) die("shared fonts: cannot read %s", path);
    pal_file_close(f);
    text[n] = 0;
    char status[16];
    if (!json_field(text, text + n, "status", status, sizeof status, NULL) || strcmp(status, "pass"))
        die("shared fonts: %s does not say status pass", path);
    const char *p = strstr(text, "\"fonts\"");
    for (int i = 0; i < FONT_COUNT; i++) {
        const char *obj = p ? strchr(p, '{') : NULL, *end = obj ? strchr(obj, '}') : NULL;
        uint64_t type;
        if (!end || !json_field(obj, end, "type", NULL, 0, &type) || type != (uint64_t)i ||
            !json_field(obj, end, "name", rows[i].name, sizeof rows[i].name, NULL) ||
            !json_field(obj, end, "sha256", rows[i].sha256, sizeof rows[i].sha256, NULL) ||
            !json_field(obj, end, "bfttf_bytes", NULL, 0, &rows[i].file_bytes) ||
            !json_field(obj, end, "font_bytes", NULL, 0, &rows[i].font_bytes) ||
            rows[i].file_bytes != rows[i].font_bytes + 8)
            die("shared fonts: entry %d of %s is not what tools/build_win.py writes", i, path);
        p = end + 1;
    }
    free(text);
}

static void load(void) {
    char dir[1100]; snprintf(dir, sizeof dir, "%.1060s/firmware/shared-fonts", host_data_dir());
    read_manifest(dir);
    uint64_t total = 0;
    for (int i = 0; i < FONT_COUNT; i++) total += rows[i].file_bytes;
    region = host_guest_alloc((size_t)total);
    uint64_t at = 0;
    for (int i = 0; i < FONT_COUNT; i++) {
        char path[1200]; snprintf(path, sizeof path, "%.1100s/%d-%.60s.bfttf", dir, i, rows[i].name);
        pal_file *f = pal_file_open(path, PAL_FILE_READ);
        if (!f || pal_file_size(f) != (int64_t)rows[i].file_bytes ||
            pal_file_pread(f, region + at, (size_t)rows[i].file_bytes, 0) != (int64_t)rows[i].file_bytes)
            die("shared fonts: cannot read %s", path);
        pal_file_close(f);
        host_sha256 s; char hex[65];
        host_sha256_begin(&s); host_sha256_update(&s, region + at, (size_t)rows[i].file_bytes); host_sha256_hex(&s, hex);
        if (strcmp(hex, rows[i].sha256)) die("shared font integrity mismatch: %s", path);
        offsets[i] = at + 8;
        at += rows[i].file_bytes;
    }
    LOG("shared fonts loaded: %d fonts, %llu bytes", FONT_COUNT, (unsigned long long)total);
}
static int valid(int type) { if (type < 0 || type >= FONT_COUNT) runtime_fail("invalid shared font type"); pthread_once(&once, load); return type; }
static int load_state(int type) { valid(type); return 1; }   /* SharedFontLoadState_Loaded */
static const void *address(int type) { return region + offsets[valid(type)]; }
static size_t size(int type) { return (size_t)rows[valid(type)].font_bytes; }

static const host_fn table[] = {
    {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", (void *)load_state, HOST_RET_U32},
    {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", (void *)address, HOST_RET_U64},
    {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", (void *)size, HOST_RET_U64},
};
const host_fn *host_font_override(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(name, table[i].name)) return &table[i];
    return NULL;
}
