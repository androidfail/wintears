/* Read-only game RomFS, backed by the player's fully IVFC-verified image. Ported from upstream
   games/totk/runtime/romfs.c: same index (runtime-index.bin, "TOTKRFS1"), same checks, same overlays.

   What upstream compiles into romfs_config.h (image size and SHA-256, index SHA-256) is read from
   <data>/v0/romfs/romfs-config.txt, which tools/build_win.py writes next to the index (and lists in the output
   manifest). The image is hashed in full once; a proof record next to it (.romfs-verified-v1: the hash, the file's
   identity, size and modification time) lets later starts skip that while the same file is unchanged
   (TOTK_ROMFS_VERIFY=full forces a new hash). Split images (upstream's romfs.bin.000..., for iOS) are not supported.

   Overlays: files listed in a directory's manifest.tsv are served in place of the originals (path, overlay size and
   SHA-256, original SHA-256, or "-" for a file the RomFS lacks). The directories, in order: <data>/v0/romfs-patches
   (tools/build_win.py; TOTK_ROMFS_PATCHES overrides, 0 = none) and the player's mods (TOTK_ROMFS_MODS). The true-2K
   HeapDef overlay (romfs-patches-out) joins when the output resolution setting is ported. */
#include <pthread.h>

#include "host/hos.h"
#include "../../upstream/games/totk/runtime/romfs_path.h"

#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 65536

static const uint8_t *index_bytes;
static uint64_t index_size, file_count, strings_offset;
static uint64_t image_size;
static char image_sha[65], index_sha[65];
static pal_file *image;
static char mount_name[32];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static uint64_t handles[NH];   /* file index + 1; 0 is free */

typedef struct { uint64_t index, size; char *path; pal_stat id; char *name; } Overlay;
static Overlay *overlays;      /* sorted by RomFS index after overlay_init */
static size_t noverlay, overlay_cap;
#define ADDED UINT64_MAX       /* an added file's index until overlay_init numbers it */
static char **added;           /* RomFS paths of the added files, sorted; added[k] is index file_count + k */
static size_t nadded;
static uint32_t handle_ovl[NH];   /* overlay slot + 1 of an open handle, 0 = original */
static pal_file *handle_file[NH]; /* the handle's open overlay file, when handle_ovl is set */

typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "index entry layout");
static Entry entry(uint64_t i) { Entry e; memcpy(&e, index_bytes + 24 + i * 24, 24); return e; }
static const char *entry_name(Entry e) { return (const char *)index_bytes + strings_offset + e.name; }
static void need(int c, const char *m) { if (!c) runtime_fail(m); }

static void hash_file(pal_file *f, uint64_t offset, uint64_t size, char hex[65], const char *progress) {
    host_sha256 s;
    host_sha256_begin(&s);
    size_t cap = 8u << 20;
    uint8_t *buffer = malloc(cap);
    need(buffer != NULL, "hash buffer");
    uint64_t t0 = pal_now_ns(), next = 0;
    for (uint64_t done = 0; done < size;) {
        size_t chunk = size - done > cap ? cap : (size_t)(size - done);
        int64_t n = pal_file_pread(f, buffer, chunk, offset + done);
        need(n > 0, "read hash input");
        host_sha256_update(&s, buffer, (size_t)n);
        done += (uint64_t)n;
        if (progress && done >= next) {
            LOG("%s: %llu of %llu MiB (%.0f s)", progress, (unsigned long long)(done >> 20),
                (unsigned long long)(size >> 20), (pal_now_ns() - t0) / 1e9);
            next = done + (2ull << 30);
        }
    }
    free(buffer);
    host_sha256_hex(&s, hex);
}

/* A full hash is reused only while the same file has the same identity, size and modification time. */
typedef struct { char magic[16], sha256[65]; uint64_t volume, index, size, mtime_ns; } RomfsProof;
static RomfsProof proof_for(const pal_stat *st) {
    RomfsProof p;
    memset(&p, 0, sizeof p);
    memcpy(p.magic, "WINTEARS-RFS-P1", 16);
    memcpy(p.sha256, image_sha, 65);
    p.volume = st->volume; p.index = st->index; p.size = st->size; p.mtime_ns = st->mtime_ns;
    return p;
}

static void verify_image(const char *dir) {
    pal_stat before, after;
    need(pal_file_stat(image, &before) == 0, "RomFS image stat");
    RomfsProof want = proof_for(&before), saved;
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/.romfs-verified-v1", dir);
    const char *v = pal_getenv("TOTK_ROMFS_VERIFY");
    int force = v && !strcmp(v, "full");
    pal_file *pf = pal_file_open(path, PAL_FILE_READ);
    if (pf) {
        int ok = pal_file_size(pf) == sizeof saved && pal_file_pread(pf, &saved, sizeof saved, 0) == sizeof saved;
        pal_file_close(pf);
        if (ok && !force && !memcmp(&saved, &want, sizeof want)) {
            LOG("RomFS unchanged; reused full SHA-256 verification record (%llu bytes)", (unsigned long long)want.size);
            return;
        }
    }
    LOG("verifying the RomFS image SHA-256 (%llu bytes, once)%s", (unsigned long long)want.size, force ? " [forced]" : "");
    char hex[65];
    hash_file(image, 0, image_size, hex, "verifying RomFS");
    need(!strcmp(hex, image_sha), "RomFS integrity mismatch");
    need(pal_file_stat(image, &after) == 0, "RomFS image stat after hash");
    RomfsProof finished = proof_for(&after);
    need(!memcmp(&want, &finished, sizeof want), "RomFS changed during verification");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    pal_file *out = pal_file_open(tmp, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE);
    if (out) {
        int ok = pal_file_pwrite(out, &want, sizeof want, 0) == sizeof want && pal_file_flush(out) == 0;
        pal_file_close(out);
        if (!ok || pal_rename_replace(tmp, path)) pal_remove_file(tmp);
    }
}

static uint64_t lower(const char *name);
static int path_ok(const char *p) { return romfs_path_valid_relative(p); }

static void hash_range(pal_file *f, uint64_t offset, uint64_t size, const char *expected, const char *what) {
    char hex[65];
    hash_file(f, offset, size, hex, NULL);
    if (strcmp(hex, expected)) {
        LOG("RomFS overlay: %s SHA-256 %s, manifest says %s", what, hex, expected);
        runtime_fail("RomFS overlay integrity mismatch");
    }
}

static void overlay_load(const char *dir) {
    char path[1200];
    snprintf(path, sizeof path, "%s/manifest.tsv", dir);
    pal_file *mf = pal_file_open(path, PAL_FILE_READ);
    if (!mf) { LOG("RomFS overlay: none (%s)", path); return; }
    int64_t msize = pal_file_size(mf);
    need(msize >= 0 && msize < (64 << 20), "RomFS overlay manifest size");
    char *text = malloc((size_t)msize + 1);
    need(text && pal_file_pread(mf, text, (size_t)msize, 0) == msize, "read RomFS overlay manifest");
    text[msize] = 0;
    pal_file_close(mf);
    size_t first = noverlay;
    uint64_t hashed = 0, t0 = pal_now_ns();
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!len || line[0] == '#') continue;
        char name[4096], sha[65], orig[65];
        unsigned long long size = 0;
        need(sscanf(line, "%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]", name, &size, sha, orig) == 4 && strlen(sha) == 64 &&
             (strlen(orig) == 64 || !strcmp(orig, "-")), "RomFS overlay manifest line");
        need(path_ok(name), "RomFS overlay path");
        uint64_t i = lower(name);
        int known = i < file_count && !strcmp(entry_name(entry(i)), name), add = !strcmp(orig, "-");
        if (!known && !add) { LOG("RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks", dir, name); runtime_fail("RomFS overlay names a file the RomFS lacks"); }
        if (known && add) { LOG("RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has", dir, name); runtime_fail("RomFS overlay adds a file the RomFS already has"); }
        Entry e = {0};
        if (known) { e = entry(i); hash_range(image, e.offset, e.size, orig, "original"); }
        else i = ADDED;
        char file[8200];
        snprintf(file, sizeof file, "%s/%s", dir, name);
        pal_file *of = pal_file_open(file, PAL_FILE_READ);
        pal_stat st;
        need(of && pal_file_stat(of, &st) == 0 && st.size == size, "RomFS overlay file missing or wrong size");
        hash_range(of, 0, size, sha, name);
        pal_file_close(of);
        hashed += e.size + size;
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "RomFS overlay table");
        }
        char *copy = strdup(file), *rel = strdup(name);
        need(copy && rel, "RomFS overlay path copy");
        overlays[noverlay++] = (Overlay){i, size, copy, st, rel};
        if (noverlay - first <= 16) LOG("RomFS overlay: %s (%llu bytes) %s", name, size, add ? "is added" : "replaces the original");
    }
    free(text);
    if (noverlay > first)
        LOG("RomFS overlay: %s checked, %zu files, %.1f MB hashed in %.2f s", dir, noverlay - first, hashed / 1048576.0,
            (pal_now_ns() - t0) / 1e9);
}

static int overlay_order(const void *a, const void *b) {
    uint64_t x = ((const Overlay *)a)->index, y = ((const Overlay *)b)->index;
    return (x > y) - (x < y);
}
static int added_order(const void *a, const void *b) { return strcmp((*(Overlay *const *)a)->name, (*(Overlay *const *)b)->name); }

/* Number the added files in path order after the RomFS's own (file_count + k). */
static void added_init(void) {
    Overlay **list = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == ADDED) {
            list = realloc(list, (nadded + 1) * sizeof *list);
            need(list != NULL, "RomFS added files");
            list[nadded++] = &overlays[k];
        }
    if (!nadded) return;
    qsort(list, nadded, sizeof *list, added_order);
    added = malloc(nadded * sizeof *added);
    need(added != NULL, "RomFS added files");
    for (size_t k = 0; k < nadded; k++) {
        if (k && !strcmp(list[k]->name, list[k - 1]->name)) runtime_fail("RomFS overlay: two overlays add the same file");
        list[k]->index = file_count + k;
        added[k] = list[k]->name;
    }
    free(list);
    LOG("RomFS overlay: %zu files added to the RomFS", nadded);
}

static uint64_t file_index(const char *p) {
    uint64_t i = lower(p);
    if (i < file_count && !strcmp(p, entry_name(entry(i)))) return i;
    size_t lo = 0, hi = nadded;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (strcmp(added[m], p) < 0) lo = m + 1; else hi = m; }
    return lo < nadded && !strcmp(added[lo], p) ? file_count + lo : UINT64_MAX;
}

static int folder_exists(const char *folder) {
    size_t n = strlen(folder);
    uint64_t i = lower(folder);
    if (i < file_count && !strncmp(entry_name(entry(i)), folder, n)) return 1;
    size_t lo = 0, hi = nadded;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (strcmp(added[m], folder) < 0) lo = m + 1; else hi = m; }
    return lo < nadded && !strncmp(added[lo], folder, n);
}

static uint32_t overlay_slot(uint64_t i) {
    size_t lo = 0, hi = noverlay;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (overlays[m].index < i) lo = m + 1; else hi = m; }
    return lo < noverlay && overlays[lo].index == i ? (uint32_t)(lo + 1) : 0;
}

static void overlay_init(void) {
    char d[1100];
    const char *dir = pal_getenv("TOTK_ROMFS_PATCHES");
    if (!dir || strcmp(dir, "0")) {
        if (!dir) { snprintf(d, sizeof d, "%s/v0/romfs-patches", host_data_dir()); dir = d; }
        overlay_load(dir);
    }
    const char *mods = pal_getenv("TOTK_ROMFS_MODS");
    if (mods && *mods && strcmp(mods, "0")) { snprintf(d, sizeof d, "%s", mods); overlay_load(d); }
    added_init();
    if (noverlay) qsort(overlays, noverlay, sizeof *overlays, overlay_order);
    for (size_t k = 1; k < noverlay; k++)
        if (overlays[k].index == overlays[k - 1].index) {
            LOG("RomFS overlay: %s and %s replace the same file %s", overlays[k - 1].path, overlays[k].path, overlays[k].name);
            runtime_fail("RomFS overlay: two overlays replace the same file");
        }
}

static void read_config(const char *dir) {
    char path[1200], text[512] = {0};
    snprintf(path, sizeof path, "%s/romfs-config.txt", dir);
    pal_file *f = pal_file_open(path, PAL_FILE_READ);
    if (!f) { LOG("missing %s: run tools/build_win.py again", path); runtime_fail("RomFS configuration missing"); }
    int64_t n = pal_file_pread(f, text, sizeof text - 1, 0);
    pal_file_close(f);
    unsigned long long size = 0;
    const char *a = strstr(text, "image_size "), *b = strstr(text, "image_sha256 "), *c = strstr(text, "index_sha256 ");
    need(n > 0 && a && b && c && sscanf(a, "image_size %llu", &size) == 1 && sscanf(b, "image_sha256 %64[0-9a-f]", image_sha) == 1 &&
         sscanf(c, "index_sha256 %64[0-9a-f]", index_sha) == 1 && strlen(image_sha) == 64 && strlen(index_sha) == 64,
         "RomFS configuration format");
    image_size = size;
}

static void fs_init_once(void) {
    char dir[1100], path[1200];
    const char *e = pal_getenv("TOTK_ROMFS");
    if (e) snprintf(dir, sizeof dir, "%s", e);
    else snprintf(dir, sizeof dir, "%s/v0/romfs", host_data_dir());
    read_config(dir);
    snprintf(path, sizeof path, "%s/runtime-index.bin", dir);
    pal_file *f = pal_file_open(path, PAL_FILE_READ);
    if (!f) { LOG("missing %s: run tools/build_win.py again", path); runtime_fail("RomFS index missing"); }
    int64_t size = pal_file_size(f);
    need(size >= 24 && size < 128 * 1024 * 1024, "RomFS index size");
    char hex[65];
    hash_file(f, 0, (uint64_t)size, hex, NULL);
    need(!strcmp(hex, index_sha), "RomFS index integrity mismatch");
    uint8_t *bytes = malloc((size_t)size);
    need(bytes && pal_file_pread(f, bytes, (size_t)size, 0) == size, "read RomFS index");
    pal_file_close(f);
    index_bytes = bytes;
    index_size = (uint64_t)size;
    need(!memcmp(index_bytes, "TOTKRFS1", 8), "RomFS index format");
    memcpy(&file_count, index_bytes + 8, 8);
    memcpy(&strings_offset, index_bytes + 16, 8);
    need(file_count < 1000000 && strings_offset == 24 + file_count * 24 && strings_offset < index_size, "RomFS index layout");
    const char *previous = NULL;
    for (uint64_t i = 0; i < file_count; i++) {
        Entry en = entry(i);
        need(en.offset <= image_size && en.size <= image_size - en.offset, "RomFS file bounds");
        need(en.name < index_size - strings_offset && en.len > 1 && en.len <= index_size - strings_offset - en.name, "RomFS name bounds");
        const char *name = entry_name(en);
        need(name[en.len - 1] == 0 && strlen(name) == en.len - 1 && (!previous || strcmp(previous, name) < 0), "RomFS sorted paths");
        previous = name;
    }
    snprintf(path, sizeof path, "%s/romfs.bin", dir);
    image = pal_file_open(path, PAL_FILE_READ);
    need(image != NULL, "RomFS image missing");
    need(pal_file_size(image) == (int64_t)image_size, "RomFS image size");
    verify_image(dir);
    overlay_init();
    LOG("read-only RomFS ready: %llu files", (unsigned long long)file_count);
}

void runtime_fs_init(void) { pthread_once(&init_once, fs_init_once); }

static const char *rom_path(const char *path) { return romfs_path_relative(mount_name, path); }

static uint64_t lower(const char *name) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) { uint64_t m = lo + (hi - lo) / 2; if (strcmp(entry_name(entry(m)), name) < 0) lo = m + 1; else hi = m; }
    return lo;
}

static unsigned mount_rom(const char *name, void *cache, uint64_t size) {
    (void)cache; (void)size;
    runtime_fs_init();
    if (!name || !path_ok(name) || strlen(name) >= sizeof mount_name || strchr(name, '/') || strchr(name, ':')) return INVALID;
    if (*mount_name && strcmp(name, mount_name)) runtime_fail("second RomFS mount not implemented");
    strcpy(mount_name, name);
    return 0;
}
static unsigned cache_size(uint64_t *out) { if (!out) return INVALID; *out = 0; return 0; }

/* TOTK_FS_LOG=1: log every file open with the time since start */
void runtime_fs_log(const char *what, const char *path) {
    static int on = -1;
    static uint64_t t0;
    if (on < 0) { on = pal_getenv("TOTK_FS_LOG") != NULL; t0 = pal_now_ns(); }
    if (on) LOG("fs %8.3f %s %s", (pal_now_ns() - t0) / 1e9, what, path);
}

unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) {
    runtime_fs_log("rom", path ? path : "(null)");
    if (!out || mode != 1) return UNSUPPORTED;
    const char *p = rom_path(path);
    if (!p) return NOT_FOUND;
    uint64_t i = file_index(p);
    if (i == UINT64_MAX) return NOT_FOUND;
    uint32_t ov = overlay_slot(i);
    pal_file *of = NULL;
    if (ov) {
        const Overlay *o = &overlays[ov - 1];
        of = pal_file_open(o->path, PAL_FILE_READ);
        pal_stat st;
        if (!of || pal_file_stat(of, &st) || st.size != o->size || st.volume != o->id.volume || st.index != o->id.index ||
            st.mtime_ns != o->id.mtime_ns) {
            LOG("RomFS overlay: %s is missing or changed since the start check", o->path);
            runtime_fail("RomFS overlay file changed while the game ran (restart the game)");
        }
    }
    pthread_mutex_lock(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handles[j]) {
            handles[j] = i + 1; handle_ovl[j] = ov; handle_file[j] = of;
            *out = 0x100000000ull + j;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    pthread_mutex_unlock(&lock);
    runtime_fail("file handle table full");
}

static int handle_entry(uint64_t h, Entry *e) {
    if (h <= 0x100000000ull || h >= 0x100000000ull + NH) return 0;
    pthread_mutex_lock(&lock);
    uint64_t i = handles[h - 0x100000000ull];
    pthread_mutex_unlock(&lock);
    if (!i) return 0;
    *e = i - 1 < file_count ? entry(i - 1) : (Entry){0};   /* an added file's bytes are all in its overlay */
    return 1;
}

static void handle_source(uint64_t h, const Entry *e, pal_file **f, uint64_t *base, uint64_t *size) {
    pthread_mutex_lock(&lock);
    uint32_t ov = handle_ovl[h - 0x100000000ull];
    pal_file *of = handle_file[h - 0x100000000ull];
    pthread_mutex_unlock(&lock);
    if (ov) { *f = of; *base = 0; *size = overlays[ov - 1].size; }
    else { *f = image; *base = e->offset; *size = e->size; }
}

unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    Entry e;
    if (!out || offset < 0 || !handle_entry(h, &e)) return INVALID;
    pal_file *f;
    uint64_t base, fsize;
    handle_source(h, &e, &f, &base, &fsize);
    if ((uint64_t)offset > fsize || (!buffer && size)) return INVALID;
    uint64_t n = size;
    if (n > fsize - (uint64_t)offset) n = fsize - (uint64_t)offset;
    if (n && pal_file_pread(f, buffer, (size_t)n, base + (uint64_t)offset) != (int64_t)n) runtime_fail("RomFS read failure");
    *out = n;
    return 0;
}

unsigned runtime_romfs_size(int64_t *out, uint64_t h) {
    Entry e;
    if (!out || !handle_entry(h, &e)) return INVALID;
    pal_file *f;
    uint64_t base, size;
    handle_source(h, &e, &f, &base, &size);
    *out = (int64_t)size;
    return 0;
}

void runtime_romfs_close(uint64_t h) {
    Entry e;
    if (!handle_entry(h, &e)) runtime_fail("close invalid file handle");
    uint64_t j = h - 0x100000000ull;
    pthread_mutex_lock(&lock);
    pal_file *of = handle_ovl[j] ? handle_file[j] : NULL;
    handles[j] = 0; handle_ovl[j] = 0; handle_file[j] = NULL;
    pthread_mutex_unlock(&lock);
    if (of) pal_file_close(of);
}

unsigned runtime_romfs_entry_type(int *out, const char *path) {
    if (!out) return INVALID;
    const char *p = rom_path(path);
    if (!p) return NOT_FOUND;
    if (file_index(p) != UINT64_MAX) { *out = 1; return 0; }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return INVALID;
    if (folder_exists(dir)) { *out = 0; return 0; }
    return NOT_FOUND;
}

int runtime_romfs_owns(const char *path) { size_t n = strlen(mount_name); return n && path && !strncmp(path, mount_name, n) && path[n] == ':'; }
int runtime_romfs_handle(uint64_t h) { return h > 0x100000000ull && h < 0x100000000ull + NH; }

static const host_fn table[] = {
    {"_ZN2nn2fs8MountRomEPKcPvm", (void *)mount_rom, HOST_RET_U32},
    {"_ZN2nn2fs22QueryMountRomCacheSizeEPm", (void *)cache_size, HOST_RET_U32},
};
const host_fn *host_fs_override(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(name, table[i].name)) return &table[i];
    return host_fs_host_override(name);
}
