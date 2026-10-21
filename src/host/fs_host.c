/* nn::fs host implementation for the functions main imports. Ported from upstream games/totk/runtime/fs_host.c.
   RomFS paths and handles go to the verified read-only backend (romfs.c); save, temporary and cache storage are host
   directories under the profile directory (host_profile_dir: TOTK_PROFILE_DIR, or %LOCALAPPDATA%\WinTears\totk).
   Application save data is created on first start, as the system launcher does for titles that declare save data.
   Writes reach the host file immediately; Commit flushes them. runtime_fs_save_busy tells a window close to wait
   while a save is being written (several files, then a commit).

   Windows: POSIX descriptors and directory calls become pal_file / pal_dir (UTF-8 paths), and a path component may
   not hold the characters Windows gives a meaning (':' would reach an NTFS stream). */
#include <pthread.h>
#include <stdatomic.h>

#include "host/hos.h"

#define R_PATH_NOT_FOUND 0x202u
#define R_ALREADY_EXISTS 0x402u
#define R_NOT_EMPTY 0x1002u
#define R_TARGET_NOT_FOUND 0x7d402u
#define R_INVALID_ARGUMENT 0x2ee202u
#define R_INVALID_PATH 0x2ee602u
#define R_OPEN_MODE 0x307402u
#define R_APPEND_REQUIRED 0x2f5e02u
#define TOTK_APPLICATION_ID 0x0100f2c0115b6000ull
enum { M_FREE, M_SAVE, M_TEMP, M_CACHE };
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
#define FILE_BASE 0x200000000ull
#define DIR_BASE 0x300000000ull
#define NFILES 1024
#define NDIRS 256

typedef struct { int kind; char name[32]; char root[1100]; } Mount;
typedef struct { pal_file *f; int mode, save; } File;   /* save: on a save-data mount (counts for runtime_fs_save_busy) */
typedef struct { pal_dir *dir; char path[1100]; int mode; } Dir;
static Mount mounts[16];
static File files[NFILES];
static Dir dirs[NDIRS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char profile[1024], save_root[1024];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void need(int c, const char *m) { if (!c) runtime_fail(m); }

/* Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet */
static _Atomic int save_writers, save_dirty;
static _Atomic uint64_t save_changed_ns;
static void save_touched(void) { atomic_store(&save_changed_ns, pal_now_ns()); atomic_store(&save_dirty, 1); }
int runtime_fs_save_busy(void) {
    if (atomic_load(&save_writers) > 0) return 1;
    return atomic_load(&save_dirty) && pal_now_ns() - atomic_load(&save_changed_ns) < 3000000000ull;
}
static void make_dir(const char *p) { int r = pal_mkdir(p); if (r && r != PAL_EEXIST) runtime_fail("create profile storage directory"); }
static int remove_tree(const char *path) {
    pal_dir *d = pal_dir_open(path);
    if (!d) return pal_last_file_error() == PAL_ENOENT ? 0 : -1;
    char name[1024], child[2100];
    int is_dir, bad = 0;
    while (pal_dir_next(d, name, sizeof name, &is_dir, NULL)) {
        snprintf(child, sizeof child, "%s/%s", path, name);
        bad |= is_dir ? remove_tree(child) : pal_remove_file(child) != 0;
    }
    pal_dir_close(d);
    return bad | (pal_remove_dir(path) != 0);
}
static void init(void) {
    need(snprintf(profile, sizeof profile, "%s", host_profile_dir()) < (int)sizeof profile, "profile path");
    uint64_t uid[2];
    runtime_account_uid(uid);
    char p[1100];
    snprintf(p, sizeof p, "%s/save", profile); make_dir(p);
    need(snprintf(save_root, sizeof save_root, "%s/save/%016llx%016llx", profile, (unsigned long long)uid[1], (unsigned long long)uid[0]) < (int)sizeof save_root, "save path");
    make_dir(save_root);                                   /* launcher-style EnsureSaveData */
    snprintf(p, sizeof p, "%s/temp", profile); remove_tree(p); make_dir(p);
    snprintf(p, sizeof p, "%s/cache", profile); make_dir(p);
    LOG("host save storage: %s", save_root);
}
static void ready(void) { pthread_once(&once, init); }

/* ---------- mounts and paths ---------- */
static unsigned mount_add(const char *name, int kind, const char *root) {
    if (!name || !*name || strlen(name) >= 32 || strchr(name, ':') || strchr(name, '/')) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) { pthread_mutex_unlock(&lock); return R_ALREADY_EXISTS; }
        if (!mounts[i].kind && slot < 0) slot = i;
    }
    if (slot < 0) runtime_fail("fs mount table full");
    mounts[slot].kind = kind; strcpy(mounts[slot].name, name); snprintf(mounts[slot].root, sizeof mounts[slot].root, "%s", root);
    pthread_mutex_unlock(&lock);
    LOG("fs mount %s: -> %s", name, root);
    return 0;
}
static int component_ok(const char *c, size_t n) {
    if (!n || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)c[i] < 0x20 || strchr("\\:*?\"<>|", c[i])) return 0;   /* names Windows reads otherwise */
    return 1;
}
/* "name:/a/b" -> host path; 0 on success; *kind = the mount's kind (M_SAVE ...) when kind is given */
static unsigned host_path_kind(const char *path, char *out, size_t cap, int *kind) {
    if (!path) return R_INVALID_PATH;
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return R_INVALID_PATH;
    size_t n = (size_t)(colon - path);
    pthread_mutex_lock(&lock);
    const Mount *m = NULL;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n)) m = &mounts[i];
    char root[1100];
    if (m) strcpy(root, m->root);
    int k = m ? m->kind : M_FREE;
    pthread_mutex_unlock(&lock);
    if (!m) return R_PATH_NOT_FOUND;
    if (kind) *kind = k;
    const char *rest = colon + 1;
    for (const char *q = rest + 1; *q;) {
        const char *e = strchr(q, '/');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        if (!e && !len) break;
        if (!component_ok(q, len)) return R_INVALID_PATH;
        q = e ? e + 1 : q + len;
    }
    if (snprintf(out, cap, "%s%s", root, rest) >= (int)cap) return R_INVALID_PATH;
    size_t L = strlen(out);
    while (L > strlen(root) && out[L - 1] == '/') out[--L] = 0;
    return 0;
}
static unsigned host_path(const char *path, char *out, size_t cap) { return host_path_kind(path, out, cap, NULL); }
static unsigned save_change_path(const char *path, char *out, size_t cap) {
    int kind = M_FREE;
    unsigned r = host_path_kind(path, out, cap, &kind);
    if (!r && kind == M_SAVE) save_touched();
    return r;
}
static unsigned pal_result(int e) {
    return e == PAL_ENOENT ? R_PATH_NOT_FOUND : e == PAL_EEXIST ? R_ALREADY_EXISTS : e == PAL_ENOTEMPTY ? R_NOT_EMPTY : R_INVALID_ARGUMENT;
}

/* ---------- storage mounts ---------- */
static unsigned mount_save(const char *name, const uint64_t *uid) {
    ready();
    uint64_t me[2];
    runtime_account_uid(me);
    if (!uid || uid[0] != me[0] || uid[1] != me[1]) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned char save_exists(uint64_t application, const uint64_t *uid) {
    ready();
    uint64_t me[2];
    runtime_account_uid(me);
    return application == TOTK_APPLICATION_ID && uid && uid[0] == me[0] && uid[1] == me[1];
}
/* Other titles' save data (e.g. data-link bonuses) is not present on this host. */
static unsigned mount_save_ro(const char *name, uint64_t application, const uint64_t *uid) {
    if (!save_exists(application, uid)) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned mount_temp(const char *name) { ready(); char p[1100]; snprintf(p, sizeof p, "%s/temp", profile); return mount_add(name, M_TEMP, p); }
static void cache_dir(int index, char *out, size_t cap) { snprintf(out, cap, "%s/cache/%d", profile, index); }
static unsigned create_cache(int index, int64_t size, int64_t journal) {
    ready();
    if (index < 0 || size < 0 || journal < 0) return R_INVALID_ARGUMENT;
    char p[1100];
    cache_dir(index, p, sizeof p);
    int r = pal_mkdir(p);
    return r ? pal_result(r) : 0;
}
static unsigned delete_cache(int index) { ready(); char p[1100]; cache_dir(index, p, sizeof p); return remove_tree(p) ? R_TARGET_NOT_FOUND : 0; }
static unsigned mount_cache(const char *name, int index) {
    ready();
    char p[1100];
    cache_dir(index, p, sizeof p);
    pal_stat st;
    if (pal_stat_path(p, &st) || !st.is_dir) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_CACHE, p);
}
typedef struct { int32_t index; uint8_t reserved[0x1c]; } CacheStorageInfo;
typedef struct { int list[64]; int count, next; } CacheList;
static unsigned open_cache_list(uint64_t *out) {
    ready();
    if (!out) return R_INVALID_ARGUMENT;
    CacheList *l = calloc(1, sizeof *l);
    need(l != NULL, "cache list allocation");
    char p[1100], name[256];
    snprintf(p, sizeof p, "%s/cache", profile);
    pal_dir *d = pal_dir_open(p);
    int is_dir;
    while (d && l->count < 64 && pal_dir_next(d, name, sizeof name, &is_dir, NULL)) {
        char *end;
        long v = strtol(name, &end, 10);
        if (*name && !*end && v >= 0) l->list[l->count++] = (int)v;
    }
    pal_dir_close(d);
    *out = (uint64_t)(uintptr_t)l;
    return 0;
}
static unsigned read_cache_list(int *count, CacheStorageInfo *out, uint64_t h, int max) {
    CacheList *l = (CacheList *)(uintptr_t)h;
    if (!l || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    int n = 0;
    while (n < max && l->next < l->count) { memset(&out[n], 0, sizeof *out); out[n++].index = l->list[l->next++]; }
    *count = n;
    return 0;
}
static void close_cache_list(uint64_t h) { free((void *)(uintptr_t)h); }
static void unmount(const char *name) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { mounts[i].kind = M_FREE; pthread_mutex_unlock(&lock); return; }
    pthread_mutex_unlock(&lock);
    if (!runtime_romfs_owns(name)) runtime_fail("unmount of unknown fs mount");
}
static unsigned commit(const char *name) {
    pthread_mutex_lock(&lock);
    int found = 0, save = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { found = 1; save = mounts[i].kind == M_SAVE; }
    pthread_mutex_unlock(&lock);
    if (!found) return R_PATH_NOT_FOUND;
    for (int i = 0; i < NFILES; i++) {
        pthread_mutex_lock(&lock);
        pal_file *f = files[i].f;
        if (f) pal_file_flush(f);
        pthread_mutex_unlock(&lock);
    }
    if (save) atomic_store(&save_dirty, 0);
    return 0;
}

/* ---------- files ---------- */
static File *file_of(uint64_t h, pal_file **f, int *mode) {
    if (h < FILE_BASE || h >= FILE_BASE + NFILES) return NULL;
    pthread_mutex_lock(&lock);
    File *e = &files[h - FILE_BASE];
    pal_file *v = e->f;
    if (f) *f = e->f;
    if (mode) *mode = e->mode;
    pthread_mutex_unlock(&lock);
    return v ? e : NULL;
}
static unsigned open_file(uint64_t *out, const char *path, int mode) {
    if (path && !runtime_romfs_owns(path)) runtime_fs_log(mode & 2 ? "write" : "read", path);
    if (runtime_romfs_owns(path)) return runtime_romfs_open(out, path, mode);
    if (!out || !(mode & (OPEN_READ | OPEN_WRITE)) || (mode & ~7)) return R_INVALID_ARGUMENT;
    char p[1100];
    int kind = M_FREE;
    unsigned r = host_path_kind(path, p, sizeof p, &kind);
    if (r) return r;
    pal_stat st;
    if (pal_stat_path(p, &st) || st.is_dir) return R_PATH_NOT_FOUND;
    int save_writer = kind == M_SAVE && (mode & OPEN_WRITE);
    pal_file *f = pal_file_open(p, (mode & OPEN_READ ? PAL_FILE_READ : 0) | (mode & OPEN_WRITE ? PAL_FILE_WRITE : 0));
    if (!f) return pal_result(pal_last_file_error());
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NFILES; i++)
        if (!files[i].f) {
            files[i] = (File){f, mode, save_writer};
            *out = FILE_BASE + (uint64_t)i;
            pthread_mutex_unlock(&lock);
            if (save_writer) { atomic_fetch_add(&save_writers, 1); save_touched(); }
            return 0;
        }
    pthread_mutex_unlock(&lock);
    runtime_fail("host file handle table full");
}
static unsigned read_file(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (runtime_romfs_handle(h)) return runtime_romfs_read(out, h, offset, buffer, size);
    pal_file *f;
    int mode;
    if (!out || offset < 0 || (!buffer && size) || !file_of(h, &f, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_READ)) return R_OPEN_MODE;
    if (size) dm_guest_write_prepare(buffer, (size_t)size);
    int64_t n = size ? pal_file_pread(f, buffer, (size_t)size, (uint64_t)offset) : 0;
    if (n < 0) runtime_fail("host save read failure");
    *out = (uint64_t)n;
    return 0;
}
static unsigned read_options(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size, const void *options) { (void)options; return read_file(out, h, off, buf, size); }
static unsigned read_exact(uint64_t h, int64_t off, void *buf, uint64_t size) { uint64_t n = 0; unsigned r = read_file(&n, h, off, buf, size); return r ? r : n == size ? 0 : R_INVALID_ARGUMENT; }
static unsigned write_file(uint64_t h, int64_t offset, const void *buffer, uint64_t size, const int32_t *option) {
    pal_file *f;
    int mode;
    File *e = offset < 0 || (!buffer && size) ? NULL : file_of(h, &f, &mode);
    if (!e) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    pal_stat st;
    if (pal_file_stat(f, &st)) runtime_fail("host save stat");
    if ((uint64_t)offset + size > st.size && !(mode & OPEN_APPEND)) return R_APPEND_REQUIRED;
    if (e->save) save_touched();
    if (size && pal_file_pwrite(f, buffer, (size_t)size, (uint64_t)offset) != (int64_t)size) runtime_fail("host save write failure");
    if (option && (*option & 1)) pal_file_flush(f);
    return 0;
}
static unsigned file_size(int64_t *out, uint64_t h) {
    if (runtime_romfs_handle(h)) return runtime_romfs_size(out, h);
    pal_file *f;
    pal_stat st;
    if (!out || !file_of(h, &f, NULL) || pal_file_stat(f, &st)) return R_INVALID_ARGUMENT;
    *out = (int64_t)st.size;
    return 0;
}
static unsigned set_file_size(uint64_t h, int64_t size) {
    pal_file *f;
    int mode;
    File *e = size < 0 ? NULL : file_of(h, &f, &mode);
    if (!e) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    if (e->save) save_touched();
    int r = pal_file_truncate(f, (uint64_t)size);
    return r ? pal_result(r) : 0;
}
static unsigned flush_file(uint64_t h) { pal_file *f; if (!file_of(h, &f, NULL)) return R_INVALID_ARGUMENT; pal_file_flush(f); return 0; }
static void close_file(uint64_t h) {
    if (runtime_romfs_handle(h)) { runtime_romfs_close(h); return; }
    if (!file_of(h, NULL, NULL)) runtime_fail("close invalid file handle");
    pthread_mutex_lock(&lock);
    pal_file *f = files[h - FILE_BASE].f;
    int save = files[h - FILE_BASE].save;
    files[h - FILE_BASE].f = NULL;
    pthread_mutex_unlock(&lock);
    pal_file_close(f);
    if (save) { save_touched(); atomic_fetch_sub(&save_writers, 1); }
}
static unsigned create_file(const char *path, int64_t size) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (size < 0) return R_INVALID_ARGUMENT;
    pal_file *f = pal_file_open(p, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_EXCL);
    if (!f) return pal_result(pal_last_file_error());
    int bad = pal_file_truncate(f, (uint64_t)size);
    pal_file_close(f);
    return bad ? pal_result(bad) : 0;
}
static unsigned delete_file(const char *path) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    int e = pal_remove_file(p);
    return e ? pal_result(e) : 0;
}
static unsigned rename_file(const char *from, const char *to) {
    char a[1100], b[1100];
    unsigned r = save_change_path(from, a, sizeof a);
    if (!r) r = host_path(to, b, sizeof b);
    if (r) return r;
    pal_stat st;
    if (!pal_stat_path(b, &st)) return R_ALREADY_EXISTS;
    int e = pal_rename(a, b);
    return e ? pal_result(e) : 0;
}
static unsigned entry_type(int *out, const char *path) {
    if (runtime_romfs_owns(path)) return runtime_romfs_entry_type(out, path);
    char p[1100];
    unsigned r = host_path(path, p, sizeof p);
    if (r) return r;
    pal_stat st;
    if (!out) return R_INVALID_ARGUMENT;
    if (pal_stat_path(p, &st)) return R_PATH_NOT_FOUND;
    *out = st.is_dir ? 0 : 1;
    return 0;
}

/* ---------- directories ---------- */
typedef struct { char name[769]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");
static unsigned create_directory(const char *path) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    int e = pal_mkdir(p);
    return e ? pal_result(e) : 0;
}
static unsigned delete_recursive(const char *path) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    pal_stat st;
    if (pal_stat_path(p, &st)) return R_PATH_NOT_FOUND;
    return remove_tree(p) ? R_INVALID_ARGUMENT : 0;
}
static unsigned open_directory(uint64_t *out, const char *path, int mode) {
    if (runtime_romfs_owns(path)) { LOG("OpenDirectory on RomFS %s", path); runtime_fail("RomFS directory listing not implemented"); }
    char p[1100];
    unsigned r = host_path(path, p, sizeof p);
    if (r) return r;
    if (!out || mode < 1 || mode > 3) return R_INVALID_ARGUMENT;
    pal_dir *d = pal_dir_open(p);
    if (!d) return pal_result(pal_last_file_error());
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++)
        if (!dirs[i].dir) {
            dirs[i].dir = d; dirs[i].mode = mode; snprintf(dirs[i].path, sizeof dirs[i].path, "%s", p);
            *out = DIR_BASE + (uint64_t)i;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    pthread_mutex_unlock(&lock);
    runtime_fail("host directory handle table full");
}
static unsigned read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE];
    if (!d->dir) return R_INVALID_ARGUMENT;
    int64_t n = 0;
    char name[1024];
    int is_dir;
    uint64_t size;
    while (n < max && pal_dir_next(d->dir, name, sizeof name, &is_dir, &size)) {
        if (strlen(name) > 768) continue;
        if (!(d->mode & (is_dir ? 1 : 2))) continue;
        memset(&out[n], 0, sizeof *out);
        strcpy(out[n].name, name);
        out[n].type = is_dir ? 0 : 1;
        out[n].size = is_dir ? 0 : (int64_t)size;
        n++;
    }
    *count = n;
    return 0;
}
static void close_directory(uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !dirs[h - DIR_BASE].dir) runtime_fail("close invalid directory handle");
    pthread_mutex_lock(&lock);
    pal_dir_close(dirs[h - DIR_BASE].dir);
    dirs[h - DIR_BASE].dir = NULL;
    pthread_mutex_unlock(&lock);
}
/* The fs priority only orders I/O inside the system fs process; host I/O is unordered. */
static void set_priority(int priority) { (void)priority; }

#define U32(f) (void *)(f), HOST_RET_U32
#define V(f) (void *)(f), HOST_RET_VOID
static const host_fn map[] = {
    {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", U32(open_file)}, {"_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", U32(file_size)},
    {"_ZN2nn2fs9CloseFileENS0_10FileHandleE", V(close_file)}, {"_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", U32(entry_type)},
    {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", U32(read_file)}, {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", U32(read_options)},
    {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", U32(read_exact)},
    {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", U32(write_file)},
    {"_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", U32(set_file_size)}, {"_ZN2nn2fs9FlushFileENS0_10FileHandleE", U32(flush_file)},
    {"_ZN2nn2fs10CreateFileEPKcl", U32(create_file)}, {"_ZN2nn2fs10DeleteFileEPKc", U32(delete_file)}, {"_ZN2nn2fs10RenameFileEPKcS2_", U32(rename_file)},
    {"_ZN2nn2fs15CreateDirectoryEPKc", U32(create_directory)}, {"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", U32(delete_recursive)},
    {"_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", U32(open_directory)},
    {"_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", U32(read_directory)},
    {"_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", V(close_directory)},
    {"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", U32(mount_save)},
    {"_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", U32(mount_save_ro)},
    {"_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", (void *)save_exists, HOST_RET_U8},
    {"_ZN2nn2fs21MountTemporaryStorageEPKc", U32(mount_temp)}, {"_ZN2nn2fs7UnmountEPKc", V(unmount)}, {"_ZN2nn2fs6CommitEPKc", U32(commit)},
    {"_ZN2nn2fs18CreateCacheStorageEill", U32(create_cache)}, {"_ZN2nn2fs18DeleteCacheStorageEi", U32(delete_cache)},
    {"_ZN2nn2fs17MountCacheStorageEPKci", U32(mount_cache)},
    {"_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", U32(open_cache_list)},
    {"_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", U32(read_cache_list)},
    {"_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", V(close_cache_list)},
    {"_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", V(set_priority)},
};
const host_fn *host_fs_host_override(const char *name) {
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return &map[i];
    return NULL;
}
