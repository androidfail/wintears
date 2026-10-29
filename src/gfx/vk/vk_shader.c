/* Run-time shader translation for the Vulkan backend (upstream totk_shader_rt.m, the part that is not Metal).

   A shader is identified by SHA-256(control section + code) (upstream nvn_mtl.m prog_get / ctl_key_len). Each
   translation records the translator answers it depended on; a cached variant is used when those match the bound
   state, otherwise the translation service (build/shader-service/wt_xlate.exe, tools/build_xlate.py) makes a new one.
   The cache is <profile>/shader-cache/vk-1: <variant>.spv, <variant>.wtr (bindings and dependencies, see
   tools/wt_xlate/VkService.cs) and <shader key>.variants (the variants of a shader, one name per line).

   Translation runs on the submit thread when a draw first needs it (shortcut: a long first-run stutter per new shader;
   upgrade to background translation with the draw skipped meanwhile, as upstream does on iOS). */
#include "gfx/vk/vk_shader.h"

#include <windows.h>

#include "gfx/vk/vk_draw.h"
#include "nvn_hmap.h"

int totk_depth_near_minus_w(void);

/* ---------- translation service ---------- */
static HANDLE g_svc_in, g_svc_out, g_svc_proc;
static uint64_t g_svc_last;   /* the last request (g_svc_mu) */
static char g_dir[1024];

static void exe_dir(char *out, size_t cap) {
    wchar_t w[1024]; DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (!n || n >= 1024) die("executable path");
    while (n && w[n - 1] != L'\\' && w[n - 1] != L'/') n--;
    w[n] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
}

static void service_start(void) {
    if (g_svc_in) return;
    char exe[1200]; const char *e = pal_getenv("TOTK_SHADER_XLATE");
    if (e) snprintf(exe, sizeof exe, "%s", e);
    else { char d[1024]; exe_dir(d, sizeof d); snprintf(exe, sizeof exe, "%sshader-service\\wt_xlate.exe", d); }
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) die("shader service pipes");
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    char cmd[2600]; snprintf(cmd, sizeof cmd, "\"%s\" --service --out \"%s\"", exe, g_dir);
    wchar_t wcmd[2600]; MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 2600);
    STARTUPINFOW si = {sizeof si}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        die("cannot start the shader translator %s (error %lu): run python tools/build_xlate.py", exe, (unsigned long)GetLastError());
    CloseHandle(pi.hThread); CloseHandle(in_r); CloseHandle(out_w);
    g_svc_in = in_w; g_svc_out = out_r; g_svc_proc = pi.hProcess;
    VLOG("shader translation service started (%s), cache %s", exe, g_dir);
}
/* The translator goes after 20 s without a request (its memory back; started again by the next one): on an 8 GB laptop
   it stayed for the whole session. Requests and this hold g_svc_mu. */
static pthread_mutex_t g_svc_mu = PTHREAD_MUTEX_INITIALIZER;
static void *svc_idle(void *unused) {
    (void)unused;
    pal_thread_set_name("shader translator idle");
    for (;;) {
        pal_sleep_ns(5000000000ull);
        pthread_mutex_lock(&g_svc_mu);
        if (g_svc_in && pal_now_ns() - g_svc_last > 20000000000ull) {
            CloseHandle(g_svc_in); g_svc_in = NULL;   /* end of its input: it exits */
            if (WaitForSingleObject(g_svc_proc, 3000) != WAIT_OBJECT_0) TerminateProcess(g_svc_proc, 0);
            CloseHandle(g_svc_proc); CloseHandle(g_svc_out); g_svc_proc = g_svc_out = NULL;
            static int n; if (n++ < 5) VLOG("shader translation service stopped (idle)");
        }
        pthread_mutex_unlock(&g_svc_mu);
    }
    return NULL;
}
static void service_idle_watch(void) {   /* g_svc_mu held */
    static int started;
    g_svc_last = pal_now_ns();
    if (!started) { started = 1; pthread_t t; if (!pthread_create(&t, NULL, svc_idle, NULL)) pthread_detach(t); }
}
static void svc_write(const char *s) {
    for (size_t n = strlen(s); n;) { DWORD w; if (!WriteFile(g_svc_in, s, (DWORD)n, &w, NULL)) die("shader service: write failed"); s += w; n -= w; }
}
static void svc_line(char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        char c; DWORD r;
        if (!ReadFile(g_svc_out, &c, 1, &r, NULL) || !r) die("the shader translation service stopped");
        if (c == '\n') break;
        if (n + 1 < cap) buf[n++] = c;
    }
    buf[n] = 0;
}

/* ---------- small helpers ---------- */
typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_add(sbuf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    for (;;) {
        va_list aq; va_copy(aq, ap);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, aq);
        va_end(aq);
        if (k < 0) die("format");
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; break; }
        b->cap = (b->cap + (size_t)k + 1) * 2; b->p = realloc(b->p, b->cap);
        if (!b->p) die("out of memory");
    }
    va_end(ap);
}
static void json_path(sbuf *b, const char *s) { sb_add(b, "\""); for (; *s; s++) sb_add(b, *s == '\\' ? "/" : "%c", *s); sb_add(b, "\""); }
static uint8_t *read_all(const char *path, size_t *n) {
    pal_file *f = pal_file_open(path, PAL_FILE_READ); if (!f) return NULL;
    int64_t sz = pal_file_size(f); uint8_t *b = sz > 0 ? malloc((size_t)sz + 1) : NULL;
    if (b && pal_file_pread(f, b, (size_t)sz, 0) != sz) { free(b); b = NULL; }
    pal_file_close(f);
    if (b) { b[sz] = 0; *n = (size_t)sz; }
    return b;
}
static int write_all(const char *path, const void *p, size_t n, int append) {
    pal_file *f = pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | (append ? 0 : PAL_FILE_TRUNCATE));
    if (!f) return -1;
    int64_t at = append ? pal_file_size(f) : 0;
    int ok = pal_file_pwrite(f, p, n, (uint64_t)at) == (int64_t)n;
    pal_file_close(f);
    return ok ? 0 : -1;
}

/* ---------- the cache packed into one file ----------
   <cache>/pack.wtp holds the .variants, .wtr and .spv files, mapped once at start-up. Opening the cache's small files
   one by one, on the replay thread at each program's first use, took over half of that thread's time on the PC (and
   Windows Defender scans each open). Files missing from the pack, or grown since (a .variants list the runtime
   appended to), are read from the loose files, which stay as they are; a thread at start-up writes pack.new with
   them, and the next start-up puts it in place of pack.wtp. Layout: "WTPACK01", u32 count, then per file a u16 name
   length, the name, u64 offset, u64 size; then the data. */
typedef struct { const char *name; uint32_t nlen; uint64_t off, size; } pk_ent;
static const uint8_t *g_pk;
static pk_ent *g_pke; static uint32_t g_pkn, *g_pkh, g_pk_mask;   /* g_pkh: entry index + 1, open addressing */
static uint32_t pk_hash(const char *s, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static const pk_ent *pk_find(const char *name) {
    if (!g_pkh) return NULL;
    size_t n = strlen(name);
    for (uint32_t i = pk_hash(name, n) & g_pk_mask;; i = (i + 1) & g_pk_mask) {
        uint32_t k = g_pkh[i];
        if (!k) return NULL;
        const pk_ent *e = &g_pke[k - 1];
        if (e->nlen == n && !memcmp(e->name, name, n)) return e;
    }
}
static uint32_t pk_parse(const uint8_t *p, uint64_t size, pk_ent **out) {   /* the entries, or 0 if not a pack */
    uint32_t n; if (size < 12 || memcmp(p, "WTPACK01", 8)) return 0;
    memcpy(&n, p + 8, 4);
    if (!n || n > size / 18) return 0;
    pk_ent *e = calloc(n, sizeof *e); uint64_t at = 12;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t l; if (at + 2 > size) goto bad; memcpy(&l, p + at, 2); at += 2;
        if (at + l + 16 > size) goto bad;
        e[i].name = (const char *)p + at; e[i].nlen = l; at += l;
        memcpy(&e[i].off, p + at, 8); memcpy(&e[i].size, p + at + 8, 8); at += 16;
        if (e[i].off > size || e[i].size > size - e[i].off) goto bad;
    }
    *out = e; return n;
bad: free(e); return 0;
}
static void pk_open(void) {
    char path[1100], nw[1100];
    snprintf(path, sizeof path, "%s/pack.wtp", g_dir); snprintf(nw, sizeof nw, "%s/pack.new", g_dir);
    if (pal_path_exists(nw) && pal_rename_replace(nw, path)) VLOG("shader cache: pack.new could not replace pack.wtp");
    pal_file *f = pal_file_open(path, PAL_FILE_READ); if (!f) return;
    uint64_t size = 0; const uint8_t *p = pal_file_map(f, &size);
    pal_file_close(f);
    if (!p) return;
    uint32_t n = pk_parse(p, size, &g_pke);
    if (!n) { pal_file_unmap(p); VLOG("shader cache: pack.wtp is not a pack, left unused"); return; }
    g_pk = p; g_pkn = n;
    uint32_t cap = 1; while (cap < n * 2) cap <<= 1;
    uint32_t *h = calloc(cap, sizeof *h);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = pk_hash(g_pke[i].name, g_pke[i].nlen) & (cap - 1);
        while (h[j]) j = (j + 1) & (cap - 1);
        h[j] = i + 1;
    }
    g_pk_mask = cap - 1; g_pkh = h;
    VLOG("shader cache: %u files from pack.wtp (%llu MiB)", n, (unsigned long long)(size >> 20));
}
static int pk_wanted(const char *name) {
    const char *x = strrchr(name, '.');
    return x && (!strcmp(x, ".wtr") || !strcmp(x, ".spv") || !strcmp(x, ".variants"));
}
static int pk_put(pal_file *f, uint8_t *head, uint64_t *at, uint64_t *data, const char *name, size_t nlen, const void *p, uint64_t n) {
    uint16_t l = (uint16_t)nlen; memcpy(head + *at, &l, 2); memcpy(head + *at + 2, name, nlen); *at += 2 + nlen;
    memcpy(head + *at, data, 8); memcpy(head + *at + 8, &n, 8); *at += 16;
    int ok = !n || pal_file_pwrite(f, p, (size_t)n, *data) == (int64_t)n;
    *data += n;
    return ok;
}
/* start-up thread: pack.new = pack.wtp's files + the loose ones it lacks or that grew since */
static void *pk_build(void *arg) {
    (void)arg;
    pal_thread_set_name("shader cache pack");
    pal_dir *d = pal_dir_open(g_dir); if (!d) return NULL;
    typedef struct { char *name; uint64_t size; } loose;
    loose *v = NULL; size_t nv = 0, cv = 0; char name[512]; int is_dir; uint64_t size;
    while (pal_dir_next(d, name, sizeof name, &is_dir, &size) == 1) {
        if (is_dir || !pk_wanted(name) || strlen(name) > 400) continue;
        const pk_ent *e = pk_find(name);
        if (e && e->size == size) continue;
        if (nv == cv) { cv = cv ? cv * 2 : 256; v = realloc(v, cv * sizeof *v); }
        v[nv++] = (loose){_strdup(name), size};
    }
    pal_dir_close(d);
    /* the pack is rewritten whole (over a GiB): only once enough files were left out of it, a few are read loose */
    if (!nv || (g_pkn && nv < 512)) { for (size_t i = 0; i < nv; i++) free(v[i].name); free(v); return NULL; }
    uint8_t *drop = calloc(g_pkn + 1, 1); uint32_t count = g_pkn;   /* pack entries a loose file replaces */
    for (size_t i = 0; i < nv; i++) { const pk_ent *e = pk_find(v[i].name); if (e) { drop[e - g_pke] = 1; count--; } }
    count += (uint32_t)nv;
    uint64_t head = 12;
    for (uint32_t i = 0; i < g_pkn; i++) if (!drop[i]) head += 2 + g_pke[i].nlen + 16;
    for (size_t i = 0; i < nv; i++) head += 2 + strlen(v[i].name) + 16;
    uint8_t *hb = malloc(head); memcpy(hb, "WTPACK01", 8); memcpy(hb + 8, &count, 4);
    char tmp[1100], nw[1100];
    snprintf(tmp, sizeof tmp, "%s/pack.tmp", g_dir); snprintf(nw, sizeof nw, "%s/pack.new", g_dir);
    pal_file *f = pal_file_open(tmp, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE);
    int ok = f != NULL; uint64_t at = 12, data = head;
    for (uint32_t i = 0; i < g_pkn && ok; i++)
        if (!drop[i]) ok = pk_put(f, hb, &at, &data, g_pke[i].name, g_pke[i].nlen, g_pk + g_pke[i].off, g_pke[i].size);
    for (size_t i = 0; i < nv && ok; i++) {
        char fp[1100]; size_t n = 0; snprintf(fp, sizeof fp, "%s/%s", g_dir, v[i].name);
        uint8_t *b = read_all(fp, &n);
        ok = pk_put(f, hb, &at, &data, v[i].name, strlen(v[i].name), b, b ? n : 0);
        free(b);
    }
    if (ok) ok = pal_file_pwrite(f, hb, (size_t)head, 0) == (int64_t)head;
    if (f) pal_file_close(f);
    if (ok && !pal_rename_replace(tmp, nw)) VLOG("shader cache: pack.new written (%u files, %zu new or grown), used from the next start", count, nv);
    else if (f) pal_remove_file(tmp);
    for (size_t i = 0; i < nv; i++) free(v[i].name);
    free(v); free(hb); free(drop);
    return NULL;
}
/* a cache file by name: from the pack, else the loose file (NUL-terminated; the caller frees it) */
static uint8_t *cache_read(const char *name, size_t *n) {
    const pk_ent *e = pk_find(name);
    const char *x = strrchr(name, '.');
    if (e && x && !strcmp(x, ".variants")) {   /* a list the runtime appends to: the pack's copy only if still that size */
        char path[1400]; wchar_t w[1400]; WIN32_FILE_ATTRIBUTE_DATA a;
        snprintf(path, sizeof path, "%s/%s", g_dir, name);
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1400) && GetFileAttributesExW(w, GetFileExInfoStandard, &a) &&
            ((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow) != e->size) e = NULL;
    }
    if (e) {
        uint8_t *b = malloc((size_t)e->size + 1); if (!b) return NULL;
        memcpy(b, g_pk + e->off, (size_t)e->size); b[e->size] = 0; *n = (size_t)e->size;
        return b;
    }
    char path[1400]; snprintf(path, sizeof path, "%s/%s", g_dir, name);
    return read_all(path, n);
}

/* ---------- answers from the bound state (upstream nvn_mtl.m rt_answers) ---------- */
static const char *target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
                 case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *image_format(int f) {
    switch (f) {
    case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
    case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
    case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
    case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
    default: return NULL;
    }
}
static const char *fmt_class(int f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F: return "uint";
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30: return "sint";
    default: return "float";
    }
}
typedef struct { const char *tex[128]; const char *img_type[32], *img_fmt[32]; const char *attr[16], *fout[8]; } answers;
static void answers_of(int st, answers *a) {
    memset(a, 0, sizeof *a);
    tex_t t; view_t v; int has_v;
    for (int r = 0; r < 128; r++) if (S.tex[st][r] && vt_slot_tex((int)(S.tex[st][r] & 0xfffff), &t, &v, &has_v))
        a->tex[r] = target_name(has_v && v.set_target ? v.target : t.d.target);
    for (int r = 0; r < 32; r++) if (S.img[st][r] && vt_slot_tex((int)(S.img[st][r] & 0xfffff), &t, &v, &has_v)) {
        a->img_type[r] = target_name(has_v && v.set_target ? v.target : t.d.target);
        a->img_fmt[r] = image_format(has_v && v.set_format ? v.format : t.d.format);
    }
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { va_st va; memcpy(&va, &S.attr[i], 4); a->attr[i] = fmt_class(va.fmt); }
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) if (S.rt.color[i])
        a->fout[i] = fmt_class(S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format);
}
static uint64_t mix(uint64_t h, const void *p) { return (h ^ (uint64_t)(uintptr_t)p) * 0x100000001b3ull; }
static uint64_t answers_sig(const answers *a) {   /* the strings are static: their addresses identify them */
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 128; i++) h = mix(h, a->tex[i]);
    for (int i = 0; i < 32; i++) { h = mix(h, a->img_type[i]); h = mix(h, a->img_fmt[i]); }
    for (int i = 0; i < 16; i++) h = mix(h, a->attr[i]);
    for (int i = 0; i < 8; i++) h = mix(h, a->fout[i]);
    return h;
}
static void answers_json(const answers *a, sbuf *b) {
    sb_add(b, "\"tex\": {"); int k = 0;
    for (int i = 0; i < 128; i++) if (a->tex[i]) sb_add(b, "%s\"%d\": \"%s\"", k++ ? ", " : "", i, a->tex[i]);
    sb_add(b, "}, \"img\": {"); k = 0;
    for (int i = 0; i < 32; i++) if (a->img_type[i]) {
        sb_add(b, "%s\"%d\": {\"type\": \"%s\"", k++ ? ", " : "", i, a->img_type[i]);
        if (a->img_fmt[i]) sb_add(b, ", \"fmt\": \"%s\"", a->img_fmt[i]);
        sb_add(b, "}");
    }
    sb_add(b, "}, \"attr\": {"); k = 0;
    for (int i = 0; i < 16; i++) if (a->attr[i]) sb_add(b, "%s\"%d\": \"%s\"", k++ ? ", " : "", i, a->attr[i]);
    sb_add(b, "}, \"fout\": {"); k = 0;
    for (int i = 0; i < 8; i++) if (a->fout[i]) sb_add(b, "%s\"%d\": \"%s\"", k++ ? ", " : "", i, a->fout[i]);
    sb_add(b, "}");
}
enum { DEP_TEX, DEP_IMG, DEP_ATTR, DEP_FOUT };
static int deps_match(const vk_shader *s, const answers *a) {
    for (int i = 0; i < s->ndep; i++) {
        const struct dep *d = &s->dep[i];
        char have[64];
        switch (d->kind) {
        case DEP_TEX: snprintf(have, sizeof have, "%s", d->slot < 128 && a->tex[d->slot] ? a->tex[d->slot] : "2d"); break;
        case DEP_IMG: snprintf(have, sizeof have, "%s %s", d->slot < 32 && a->img_type[d->slot] ? a->img_type[d->slot] : "-", d->slot < 32 && a->img_fmt[d->slot] ? a->img_fmt[d->slot] : "-"); break;
        case DEP_ATTR: snprintf(have, sizeof have, "%s", d->slot < 16 && a->attr[d->slot] ? a->attr[d->slot] : "float"); break;
        default: snprintf(have, sizeof have, "%s", d->slot < 8 && a->fout[d->slot] ? a->fout[d->slot] : "float"); break;
        }
        if (strcmp(have, d->val)) return 0;
    }
    return 1;
}

/* ---------- variants: loading a translation ---------- */
static VkBuffer g_c1_buf; static VmaAllocation g_c1_al; static VkDeviceSize g_c1_used;
#define C1_ARENA (64ull << 20)
static void c1_place(vk_shader *s, const uint8_t *data, uint32_t n) {
    VkDeviceSize al = vk_props.limits.minUniformBufferOffsetAlignment > 256 ? vk_props.limits.minUniformBufferOffsetAlignment : 256;
    VkDeviceSize at = (g_c1_used + al - 1) & ~(al - 1);
    if (!g_c1_buf || at + n + 65536 > C1_ARENA) {   /* a new arena (the old one stays: shaders keep pointing into it) */
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = C1_ARENA, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ac = {.usage = vk_prefer_device()};
        VKC(vmaCreateBuffer(vk_vma, &bi, &ac, &g_c1_buf, &g_c1_al, NULL));
        at = 0;
        vkCmdFillBuffer(vk_pre(), g_c1_buf, 0, VK_WHOLE_SIZE, 0);
    }
    s->c1_buf = g_c1_buf; s->c1_off = at; s->c1_size = n;
    if (n) {
        VkBuffer sb; VkDeviceSize so; uint8_t *p = vk_stage(n, 16, &sb, &so);
        memcpy(p, data, n);
        VkBufferCopy r = {so, at, (n + 3) & ~3u};
        vk_barrier(vk_pre());
        vkCmdCopyBuffer(vk_pre(), sb, g_c1_buf, 1, &r);
        vk_barrier(vk_pre());
    }
    g_c1_used = at + n;
}

static VkImageViewType dim_view(const char *d) {
    int arr = strchr(d, 'a') != NULL;
    if (!strncmp(d, "1d", 2)) return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    if (!strncmp(d, "3d", 2)) return VK_IMAGE_VIEW_TYPE_3D;
    if (!strncmp(d, "cube", 4)) return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}
static const VkDescriptorType DESC[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
VkDescriptorType vs_desc_type(int t) { return DESC[t]; }

static int word_index(const char *w, const char *const *names, int n) { for (int i = 0; i < n; i++) if (!strcmp(w, names[i])) return i; return -1; }

/* <variant>.wtr + .spv -> a shader; NULL when the files are missing or broken (it is translated again) */
static vk_shader *load_variant(const char *variant, int stage, const uint8_t *c1, uint32_t c1n) {
    char name[200]; size_t n;
    snprintf(name, sizeof name, "%s.wtr", variant);
    char *txt = (char *)cache_read(name, &n);
    snprintf(name, sizeof name, "%s.spv", variant);
    size_t spn; uint8_t *spv = cache_read(name, &spn);
    if (!txt || !spv || strncmp(txt, "wt-vk 1\n", 8) || (spn & 3)) { free(txt); free(spv); return NULL; }
    vk_shader *s = calloc(1, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", variant); s->stage = stage;
    static const char *const types[] = {"ubo", "ssbo", "cis", "si", "samp", "utb", "simg", "stb"};
    static const char *const kinds[] = {"support", "c1", "drv0", "drv2", "cbuf", "sbuf", "ubo_ptr", "sbuf_ubo", "tex", "samp", "img", "none"};
    static const char *const depk[] = {"tex", "img", "attr", "fout"};
    int cb = 0, cd = 0;
    for (char *line = txt, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        char w0[16] = "", w1[24] = "", w2[24] = "", w3[24] = "", w4[24] = "", dim[16] = "";
        int a, b, c, d, e, f, g;
        if (sscanf(line, "bind %d %15s %d %15s %d %d %d %d %15s", &a, w1, &b, w2, &c, &d, &e, &f, dim) == 9) {
            int ti = word_index(w1, types, 8), ki = word_index(w2, kinds, 12);
            if (ti < 0 || ki < 0) { free(txt); free(spv); free(s); return NULL; }
            if (s->nb == cb) { cb = cb ? cb * 2 : 16; s->b = realloc(s->b, (size_t)cb * sizeof *s->b); }
            s->b[s->nb++] = (vs_bind){(uint16_t)a, (uint16_t)(b > 0 ? b : 1), (uint8_t)ti, (uint8_t)ki, (uint8_t)f,
                                      (uint8_t)(strcmp(dim, "-") ? dim_view(dim) : VK_IMAGE_VIEW_TYPE_2D), (int16_t)c, (int16_t)d, (uint16_t)e};
        } else if (sscanf(line, "dep %15s %d %23s %23s", w0, &a, w3, w4) >= 3) {
            int k = word_index(w0, depk, 4);
            if (k < 0 || a < 0 || a > 255) continue;
            if (s->ndep == cd) { cd = cd ? cd * 2 : 16; s->dep = realloc(s->dep, (size_t)cd * sizeof *s->dep); }
            struct dep *dp = &s->dep[s->ndep++]; dp->kind = (uint8_t)k; dp->slot = (uint8_t)a;
            if (k == DEP_IMG) snprintf(dp->val, sizeof dp->val, "%s %s", w3, w4[0] ? w4 : "-"); else snprintf(dp->val, sizeof dp->val, "%s", w3);
        } else if (!strncmp(line, "in", 2) && (line[2] == ' ' || !line[2])) { for (char *p = line + 2; *p;) { long v = strtol(p, &p, 10); if (v >= 0 && v < 32) s->in_mask |= 1u << v; while (*p == ' ') p++; } }
        else if (!strncmp(line, "out", 3) && (line[3] == ' ' || !line[3])) { for (char *p = line + 3; *p;) { long v = strtol(p, &p, 10); if (v >= 0 && v < 32) s->out_mask |= 1u << v; while (*p == ' ') p++; } }
        else if (sscanf(line, "local %d %d %d", &a, &b, &c) == 3) { s->local[0] = (uint32_t)a; s->local[1] = (uint32_t)b; s->local[2] = (uint32_t)c; }
        else if (!strcmp(line, "depth 1")) s->writes_depth = 1;
        (void)g;
    }
    free(txt);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spn, .pCode = (const uint32_t *)spv};
    VkResult r = vkCreateShaderModule(vk_dev, &mi, NULL, &s->mod);
    free(spv);
    if (r != VK_SUCCESS) { VLOG("shader %s: vkCreateShaderModule failed (%d)", variant, (int)r); free(s->b); free(s->dep); free(s); return NULL; }
    VkShaderStageFlags sf = stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding lb[96]; int nl = 0;
    for (int i = 0; i < s->nb && nl < 96; i++) lb[nl++] = (VkDescriptorSetLayoutBinding){s->b[i].binding, DESC[s->b[i].type], s->b[i].count, sf, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nl, .pBindings = lb};
    VKC(vkCreateDescriptorSetLayout(vk_dev, &li, NULL, &s->dsl));
    if (c1) c1_place(s, c1, c1n);   /* NULL: the caller places it (it records into the current submission: the replay only) */
    return s;
}

/* ---------- shaders by key, programs ---------- */
typedef struct { char key[65]; int nvar, cvar; vk_shader **var; char (*names)[112]; int nnames, loaded; } shader_key;
static nvn_hmap g_keys = {.klen = 64};
static uint64_t g_translated;

typedef struct { uint64_t sig; vk_shader *s; } fit;
typedef struct { int present; char key[65]; const uint8_t *control, *code; uint32_t length, c1o; uint32_t local[3]; shader_key *k; fit last[8]; int nlast; } stage_t;
struct vk_prog { int bad; stage_t st[6]; };
static nvn_hmap g_progs = {.klen = 8};

static uint32_t ctl_key_len(const uint8_t *control) { uint32_t n; memcpy(&n, control + 0x20, 4); return n >= 0x7cc && n < 0x880 ? n : 0x880; }

static shader_key *key_get(const char *hex) {
    shader_key *k = nvn_hm_get(&g_keys, hex);
    if (k) return k;
    k = calloc(1, sizeof *k); memcpy(k->key, hex, 65);
    char name[200]; size_t n; snprintf(name, sizeof name, "%s.variants", hex);
    char *txt = (char *)cache_read(name, &n);
    for (char *line = txt, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;   /* a list written with Windows line ends */
        if (strlen(line) < 70 || strlen(line) >= 112) continue;
        k->names = realloc(k->names, (size_t)(k->nnames + 1) * sizeof *k->names);
        snprintf(k->names[k->nnames++], 112, "%s", line);
    }
    free(txt);
    nvn_hm_put(&g_keys, k->key, k);
    return k;
}

/* A translation, with copies of everything it needs: it may run on the translation worker (async_shaders, vk_draw.c)
   after the program is gone. */
typedef struct xjob { struct xjob *next; shader_key *k; int st, worker; char key[65], variant[112]; uint8_t *code; uint32_t length, c1o, local[3]; char *ans; vk_shader *out; } xjob;
/* g_svc_mu (above): one request at a time to the translation service */
static xjob *xjob_make(stage_t *t, int st, const answers *a) {
    xjob *j = calloc(1, sizeof *j);
    if (!j) die("shader translation job");
    sbuf ans = {0}; answers_json(a, &ans);
    host_sha256 h; char md[65]; host_sha256_begin(&h); host_sha256_update(&h, ans.p, ans.n); host_sha256_hex(&h, md);
    snprintf(j->variant, sizeof j->variant, "%s-%.12s", t->key, md);
    memcpy(j->key, t->key, 65); j->k = t->k; j->st = st; j->ans = ans.p;
    j->length = t->length; j->c1o = t->c1o; memcpy(j->local, t->local, sizeof j->local);
    j->code = malloc(t->length ? t->length : 1);
    if (!j->code) die("shader translation job");
    memcpy(j->code, t->code, t->length);
    return j;
}
static void xjob_free(xjob *j) { free(j->code); free(j->ans); free(j); }
static vk_shader *xjob_run(xjob *j) {
    int st = j->st; const char *variant = j->variant;
    const char *stn = st == 0 ? "vs" : st == 1 ? "ps" : "cs";
    size_t skip = st == 5 ? 0 : 0x30;   /* graphics code drops the 0x30-byte NVN header (upstream) */
    char code_path[1300]; snprintf(code_path, sizeof code_path, "%s/%s.code", g_dir, variant);
    if (write_all(code_path, j->code + skip, j->length - skip, 0)) die("write shader code %s", code_path);
    sbuf rq = {0};
    sb_add(&rq, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code\": ", variant, stn); json_path(&rq, code_path);
    sb_add(&rq, ", \"c1\": \"");
    for (uint32_t i = j->c1o; i < j->length; i++) sb_add(&rq, "%02x", j->code[i]);
    sb_add(&rq, "\", %s", j->ans);
    if (st == 5) sb_add(&rq, ", \"local\": [%u, %u, %u]", j->local[0], j->local[1], j->local[2]);
    if (vk_sb_align != 16) sb_add(&rq, ", \"sb_align\": %u", vk_sb_align);
    sb_add(&rq, ", \"depth_minus_w\": %s}\n", totk_depth_near_minus_w() ? "true" : "false");
    pthread_mutex_lock(&g_svc_mu);
    service_idle_watch();
    service_start();
    uint64_t t0 = pal_now_ns();
    svc_write(rq.p);
    char reply[2048]; svc_line(reply, sizeof reply);
    pthread_mutex_unlock(&g_svc_mu);
    remove(code_path);   /* the translator has read it */
    free(rq.p);
    if (!strstr(reply, "\"ok\": true")) { VLOG("shader %s (%s) not translated: %s", variant, stn, reply); return NULL; }
    vk_ns_xlate += pal_now_ns() - t0;
    vk_shader *s = load_variant(variant, st, j->worker ? NULL : j->code + j->c1o, j->length - j->c1o);
    if (!s) { VLOG("shader %s: translation output unreadable", variant); return NULL; }
    shader_key *k = j->k;
    char line[120]; snprintf(line, sizeof line, "%s\n", variant);
    char path[1200]; snprintf(path, sizeof path, "%s/%s.variants", g_dir, k->key);
    write_all(path, line, strlen(line), 1);
    if (++g_translated <= 8 || g_translated % 100 == 0) VLOG("translated %llu: %s %s in %.1f ms", (unsigned long long)g_translated, stn, variant, (pal_now_ns() - t0) / 1e6);
    return s;
}
static vk_shader *translate(stage_t *t, int st, const answers *a) { xjob *j = xjob_make(t, st, a); vk_shader *s = xjob_run(j); xjob_free(j); return s; }

/* Graphics translations on a worker (async_shaders): jobs by variant name, so one is asked for once; finished ones
   are added to their key's variants by the replay (vs_stage), which alone reads and grows those lists. */
int async_shaders(void);
static pthread_mutex_t g_xq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_xq_cv = PTHREAD_COND_INITIALIZER;
static xjob *g_xq_head, *g_xq_tail, *g_xdone;
static nvn_hmap g_xpending = {.klen = 112};   /* variant -> job (queued, running or done but not yet added) */
static void *xlate_worker(void *unused) {
    (void)unused;
    pal_thread_set_name("vk shader translation");
    for (;;) {
        pthread_mutex_lock(&g_xq_mu);
        while (!g_xq_head) pthread_cond_wait(&g_xq_cv, &g_xq_mu);
        xjob *j = g_xq_head; g_xq_head = j->next; if (!g_xq_head) g_xq_tail = NULL;
        pthread_mutex_unlock(&g_xq_mu);
        j->worker = 1; j->out = xjob_run(j);
        pthread_mutex_lock(&g_xq_mu);
        j->next = g_xdone; g_xdone = j;
        pthread_mutex_unlock(&g_xq_mu);
    }
    return NULL;
}
static void xlate_collect(void) {   /* the replay: finished translations into their keys' variant lists */
    if (!g_xdone) return;
    pthread_mutex_lock(&g_xq_mu);
    xjob *d = g_xdone; g_xdone = NULL;
    pthread_mutex_unlock(&g_xq_mu);
    for (xjob *j = d, *n; j; j = n) {
        n = j->next;
        if (j->out) {
            c1_place(j->out, j->code + j->c1o, j->length - j->c1o);
            shader_key *k = j->k;
            if (k->nvar == k->cvar) { k->cvar = k->cvar ? k->cvar * 2 : 4; k->var = realloc(k->var, (size_t)k->cvar * sizeof *k->var); }
            k->var[k->nvar++] = j->out;
            pthread_mutex_lock(&g_xq_mu); nvn_hm_del(&g_xpending, j->variant); pthread_mutex_unlock(&g_xq_mu);
            xjob_free(j);
        }   /* a failed one stays pending: not asked for again */
    }
}
static void xlate_ask(stage_t *t, int st, const answers *a) {
    static int started;
    if (!started) { started = 1; pthread_t th; if (pthread_create(&th, NULL, xlate_worker, NULL)) die("shader translation worker"); pthread_detach(th); }
    xjob *j = xjob_make(t, st, a);
    pthread_mutex_lock(&g_xq_mu);
    if (nvn_hm_get(&g_xpending, j->variant)) { pthread_mutex_unlock(&g_xq_mu); xjob_free(j); return; }
    nvn_hm_put(&g_xpending, j->variant, j);
    if (g_xq_tail) g_xq_tail->next = j; else g_xq_head = j;
    g_xq_tail = j;
    pthread_cond_signal(&g_xq_cv);
    pthread_mutex_unlock(&g_xq_mu);
}

vk_prog *vs_prog(const prog_t *p) {
    if (!p || !p->serial) return NULL;
    vk_prog *pr = nvn_hm_get(&g_progs, &p->serial);
    if (pr) return pr->bad ? NULL : pr;
    pr = calloc(1, sizeof *pr);
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *control = p->sd[i].control;
        if (!control || !p->sd[i].data) { pr->bad = 1; break; }
        uint32_t length, stage, c1o; memcpy(&length, control + 0x704, 4); memcpy(&stage, control + 0x714, 4); memcpy(&c1o, control + 0x700, 4);
        if (stage != 0 && stage != 1 && stage != 5) { VK_ONCE("program with NVN stage %u (tessellation / geometry): not supported, its draws are skipped", stage); pr->bad = 1; break; }
        if (!length || length > (16u << 20) || c1o > length || (stage != 5 && c1o < 0x30)) { VLOG("program stage %u: code %#x / c1 %#x not plausible", stage, length, c1o); pr->bad = 1; break; }
        stage_t *t = &pr->st[stage];
        t->present = 1; t->control = control; t->code = (const uint8_t *)(uintptr_t)p->sd[i].data; t->length = length; t->c1o = c1o;
        if (stage == 5) memcpy(t->local, control + 0x748, 12);
        host_sha256 h; host_sha256_begin(&h); host_sha256_update(&h, control, ctl_key_len(control)); host_sha256_update(&h, t->code, length); host_sha256_hex(&h, t->key);
        t->k = key_get(t->key);
    }
    nvn_hm_put(&g_progs, &p->serial, pr);
    return pr->bad ? NULL : pr;
}
int vs_has_stage(const vk_prog *pr, int stage) { return pr && pr->st[stage].present; }
void vs_prog_release(uint64_t serial) { vk_prog *pr = nvn_hm_del(&g_progs, &serial); free(pr); }

/* The variant answers of a stage are worked out again only when what they depend on changed: the bound texture and
   image handles, a texture registration, the vertex formats (vertex stage), the targets' formats (fragment stage).
   Working them out takes the slot lock and copies every bound slot, at every draw (most of the replay's locking). */
/* The bound handles are compared whole (memcmp, vectorised); a hash chained through all 160 of them was one multiply
   after another at every draw, about a tenth of the replay thread's time. */
static struct { uint64_t raw; int valid; uint64_t sig; uint64_t tex[128], img[32]; answers a; } g_memo[6];
static inline uint64_t mixk(uint64_t h, uint64_t v) { h ^= v; h *= 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }
static uint64_t raw_key(int st) {   /* what the answers depend on besides the bound handles */
    uint64_t h = mixk(0x51ED27ull, vt_treg_version());
    if (st == 0) { h = mixk(h, (uint64_t)S.nattr); for (int i = 0; i < S.nattr && i < 16; i++) h = mixk(h, S.attr[i]); }
    if (st == 1) { h = mixk(h, (uint64_t)S.has_rt); if (S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++)
        h = mixk(h, S.rt.color[i] ? (uint64_t)(S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format) + 1 : 0); }
    return h;
}
vk_shader *vs_stage(vk_prog *pr, int st) {
    stage_t *t = &pr->st[st];
    if (!t->present) return NULL;
    uint64_t rk = raw_key(st);
    if (!g_memo[st].valid || g_memo[st].raw != rk || memcmp(g_memo[st].tex, S.tex[st], sizeof g_memo[st].tex) ||
        memcmp(g_memo[st].img, S.img[st], sizeof g_memo[st].img)) {
        answers_of(st, &g_memo[st].a); g_memo[st].sig = answers_sig(&g_memo[st].a); g_memo[st].raw = rk; g_memo[st].valid = 1;
        memcpy(g_memo[st].tex, S.tex[st], sizeof g_memo[st].tex); memcpy(g_memo[st].img, S.img[st], sizeof g_memo[st].img);
    }
    uint64_t sig = g_memo[st].sig;
    for (int i = 0; i < t->nlast; i++) if (t->last[i].sig == sig) return t->last[i].s;
    answers a = g_memo[st].a;   /* not one of this stage's recent variants: the full answers */
    shader_key *k = t->k;
    xlate_collect();
    vk_shader *found = NULL;
    for (int i = 0; i < k->nvar && !found; i++) if (deps_match(k->var[i], &a)) found = k->var[i];
    if (!found && !k->loaded) {   /* the cached variants of this shader, loaded on first need */
        k->loaded = 1;
        for (int i = 0; i < k->nnames; i++) {
            vk_shader *s = load_variant(k->names[i], st, t->code + t->c1o, t->length - t->c1o);
            if (!s) continue;
            if (k->nvar == k->cvar) { k->cvar = k->cvar ? k->cvar * 2 : 4; k->var = realloc(k->var, (size_t)k->cvar * sizeof *k->var); }
            k->var[k->nvar++] = s;
            if (!found && deps_match(s, &a)) found = s;
        }
    }
    if (!found && st != 5 && async_shaders()) { xlate_ask(t, st, &a); return NULL; }   /* the draw waits for it (compute never does) */
    if (!found && (found = translate(t, st, &a))) {
        if (k->nvar == k->cvar) { k->cvar = k->cvar ? k->cvar * 2 : 4; k->var = realloc(k->var, (size_t)k->cvar * sizeof *k->var); }
        k->var[k->nvar++] = found;
    }
    if (!found) return NULL;
    t->last[t->nlast < 8 ? t->nlast++ : (int)(sig & 7)] = (fit){sig, found};
    return found;
}

void vs_init(void) {
    /* translations depend on the storage buffer alignment: other alignments than 16 keep their own cache */
    if (vk_sb_align == 16) snprintf(g_dir, sizeof g_dir, "%s/shader-cache/vk-1", host_profile_dir());
    else snprintf(g_dir, sizeof g_dir, "%s/shader-cache/vk-1-sb%u", host_profile_dir(), vk_sb_align);
    if (pal_mkdirs(g_dir)) die("cannot create the shader cache %s", g_dir);
    pk_open();
    { pthread_t t; if (!pthread_create(&t, NULL, pk_build, NULL)) pthread_detach(t); }
}
