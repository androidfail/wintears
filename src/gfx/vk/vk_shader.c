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
static HANDLE g_svc_in, g_svc_out;
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
    g_svc_in = in_w; g_svc_out = out_r;
    VLOG("shader translation service started (%s), cache %s", exe, g_dir);
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
        VmaAllocationCreateInfo ac = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
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
    char path[1400]; size_t n;
    snprintf(path, sizeof path, "%s/%s.wtr", g_dir, variant);
    char *txt = (char *)read_all(path, &n);
    snprintf(path, sizeof path, "%s/%s.spv", g_dir, variant);
    size_t spn; uint8_t *spv = read_all(path, &spn);
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
    c1_place(s, c1, c1n);
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
    char path[1200]; size_t n; snprintf(path, sizeof path, "%s/%s.variants", g_dir, hex);
    char *txt = (char *)read_all(path, &n);
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

static vk_shader *translate(stage_t *t, int st, const answers *a) {
    sbuf ans = {0}; answers_json(a, &ans);
    host_sha256 h; char md[65]; host_sha256_begin(&h); host_sha256_update(&h, ans.p, ans.n); host_sha256_hex(&h, md);
    char variant[112]; snprintf(variant, sizeof variant, "%s-%.12s", t->key, md);
    const char *stn = st == 0 ? "vs" : st == 1 ? "ps" : "cs";
    size_t skip = st == 5 ? 0 : 0x30;   /* graphics code drops the 0x30-byte NVN header (upstream) */
    char code_path[1300]; snprintf(code_path, sizeof code_path, "%s/%s.code", g_dir, variant);
    if (write_all(code_path, t->code + skip, t->length - skip, 0)) die("write shader code %s", code_path);
    sbuf rq = {0};
    sb_add(&rq, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code\": ", variant, stn); json_path(&rq, code_path);
    sb_add(&rq, ", \"c1\": \"");
    for (uint32_t i = t->c1o; i < t->length; i++) sb_add(&rq, "%02x", t->code[i]);
    sb_add(&rq, "\", %s", ans.p);
    if (st == 5) sb_add(&rq, ", \"local\": [%u, %u, %u]", t->local[0], t->local[1], t->local[2]);
    sb_add(&rq, ", \"depth_minus_w\": %s}\n", totk_depth_near_minus_w() ? "true" : "false");
    service_start();
    uint64_t t0 = pal_now_ns();
    svc_write(rq.p);
    char reply[2048]; svc_line(reply, sizeof reply);
    remove(code_path);   /* the translator has read it */
    free(rq.p); free(ans.p);
    if (!strstr(reply, "\"ok\": true")) { VLOG("shader %s (%s) not translated: %s", variant, stn, reply); return NULL; }
    vk_ns_xlate += pal_now_ns() - t0;
    vk_shader *s = load_variant(variant, st, t->code + t->c1o, t->length - t->c1o);
    if (!s) { VLOG("shader %s: translation output unreadable", variant); return NULL; }
    shader_key *k = t->k;
    char line[120]; snprintf(line, sizeof line, "%s\n", variant);
    char path[1200]; snprintf(path, sizeof path, "%s/%s.variants", g_dir, k->key);
    write_all(path, line, strlen(line), 1);
    if (++g_translated <= 8 || g_translated % 100 == 0) VLOG("translated %llu: %s %s in %.1f ms", (unsigned long long)g_translated, stn, variant, (pal_now_ns() - t0) / 1e6);
    return s;
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

vk_shader *vs_stage(vk_prog *pr, int st) {
    stage_t *t = &pr->st[st];
    if (!t->present) return NULL;
    answers a; answers_of(st, &a);
    uint64_t sig = answers_sig(&a);
    for (int i = 0; i < t->nlast; i++) if (t->last[i].sig == sig) return t->last[i].s;
    shader_key *k = t->k;
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
    if (!found && (found = translate(t, st, &a))) {
        if (k->nvar == k->cvar) { k->cvar = k->cvar ? k->cvar * 2 : 4; k->var = realloc(k->var, (size_t)k->cvar * sizeof *k->var); }
        k->var[k->nvar++] = found;
    }
    if (!found) return NULL;
    t->last[t->nlast < 8 ? t->nlast++ : (int)(sig & 7)] = (fit){sig, found};
    return found;
}

void vs_init(void) {
    snprintf(g_dir, sizeof g_dir, "%s/shader-cache/vk-1", host_profile_dir());
    if (pal_mkdirs(g_dir)) die("cannot create the shader cache %s", g_dir);
}
