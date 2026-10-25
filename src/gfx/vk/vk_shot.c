/* TOTK_SHOT=<frame>:<path.png>[,<frame>:<path.png>...]: the game's swapchain texture at those presents, saved as
   PNG (RGBA8, stored deflate blocks: no compression library). For checking the picture without looking at the screen. */
#include <stdatomic.h>

#include "gfx/vk/vk.h"

static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), d, n); uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
int vk_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch) {
    size_t raw_n = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_n);
    if (!raw) return -1;
    for (uint32_t y = 0; y < h; y++) { raw[y * (1 + (size_t)w * 4)] = 0; memcpy(raw + y * (1 + (size_t)w * 4) + 1, rgba + y * pitch, (size_t)w * 4); }
    size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(zn); if (!z) { free(raw); return -1; }
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw_n; at += 65535) {
        uint16_t n = (uint16_t)(raw_n - at > 65535 ? 65535 : raw_n - at);
        z[o++] = at + n >= raw_n; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + at, n); o += n;
    }
    be32(z + o, b << 16 | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)o); chunk(f, "IEND", NULL, 0);
    int ok = !ferror(f); fclose(f); free(raw); free(z);
    return ok ? 0 : -1;
}

/* the path to save present n to, or NULL */
extern _Atomic uint64_t vk_capture_at;
const char *vk_shot_path(uint64_t n) {
    static char list[2048]; static int init;
    uint64_t cap = atomic_load(&vk_capture_at);
    if (cap && n == cap + 1) {   /* the F9 frame */
        static char cp[1200]; char dir[1100]; snprintf(dir, sizeof dir, "%s/logs", host_profile_dir()); pal_mkdirs(dir);
        snprintf(cp, sizeof cp, "%s/capture-%llu.png", dir, (unsigned long long)n);
        return cp;
    }
    if (!init) { init = 1; const char *e = pal_getenv("TOTK_SHOT"); snprintf(list, sizeof list, "%s", e ? e : ""); }
    static char out[1024];
    for (const char *p = list; *p;) {
        char *end; unsigned long long f = strtoull(p, &end, 10);
        if (*end != ':') return NULL;
        const char *q = end + 1, *c = strchr(q, ',');
        size_t len = c ? (size_t)(c - q) : strlen(q);
        if (f == n && len < sizeof out) { memcpy(out, q, len); out[len] = 0; return out; }
        if (!c) break;
        p = c + 1;
    }
    return NULL;
}
