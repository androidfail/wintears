/* wintears.exe: the Windows runtime.

     wintears boot <TOTK_win folder> [--trace] [--no-window]

   With a window (the default) the game runs on its own thread and this thread runs the window's event loop
   (frontend.c); closing the window ends the program. --no-window runs the game on this thread without one (the null
   GPU backend draws nothing).

   It loads the game's original modules from <TOTK_win>/data/v0/exefs (main, subsdk0, sdk; rtld is not run: the
   loader links instead, as upstream does), relocates and links them into the host's guest layout, and runs the SDK's
   own entry, nn::init::Start(main thread handle, 0, loader_ready, module_inits, module_finis), in the JIT on this
   thread, which becomes the guest main thread. The Horizon host (src/host) answers the svcs it has ported; the first
   one it cannot answer ends the run with the guest's registers and call chain. --trace logs every svc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "frontend/frontend.h"
#include "host/host.h"
#include "loader/loader.h"
#include "pal.h"

#define LOG(...) do { fprintf(stderr, "[wintears] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static ldr_set *mods;
static unsigned host_calls;

/* ---- host callbacks the SDK's nn::init::Start calls ------------------------------------------------------------ */

static void thunk_void(jit_context *c, void *fn) { (void)c; host_calls++; ((void (*)(void))fn)(); }

static void loader_ready(void) { LOG("rtld callback: modules linked"); }

static void module_inits(void) {
    LOG("SDK startup returned; running module constructors");
    for (unsigned i = 0; i < ldr_count(mods); i++) {
        const ldr_module *m = ldr_module_at(mods, i);
        if (!m->init) continue;
        LOG("ENTER %s DT_INIT", m->name);
        host_guest_call(m->base + m->init, 0, NULL);
        LOG("RETURN %s DT_INIT", m->name);
    }
}

static void module_finis(void) {
    LOG("guest exited: module finalizers are not implemented");
    exit(0);
}

/* Imports defined in no module become stubs that stop the run and name the import. */
static void thunk_unresolved(jit_context *c, void *name) {
    (void)c;
    LOG("FAIL: call to an import no module defines: %s", (const char *)name);
    exit(1);
}

static uint64_t resolve(void *user, const ldr_module *m, const char *name, unsigned kind, int phase) {
    (void)user; (void)m;
    if (phase == 0) return host_override(name, kind);   /* upstream runtime_override / runtime_observer */
    LOG("import defined nowhere: %s (bound to a trap stub)", name);
    return bridge_add(host_bridge(), name, thunk_unresolved, (void *)name);
}

/* ---- setup ----------------------------------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *size) {
    pal_file *f = pal_file_open(path, PAL_FILE_READ);
    if (!f) return NULL;
    int64_t n = pal_file_size(f);
    uint8_t *buf = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (buf && pal_file_pread(f, buf, (size_t)n, 0) != n) { free(buf); buf = NULL; }
    pal_file_close(f);
    *size = buf ? (size_t)n : 0;
    return buf;
}

static int load_modules(const char *root) {
    static const char *names[] = {"main", "subsdk0", "sdk"};   /* upstream's order (runtime_symbol search order) */
    char path[1024], err[256];
    uint64_t at = host_code_base(), end = at + host_code_size();
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/data/v0/exefs/%s", root, names[i]);
        size_t size;
        uint8_t *nso = read_file(path, &size);
        if (!nso) { LOG("cannot read %s", path); return -1; }
        uint64_t need = ldr_nso_image_size(nso, size);
        if (!need || at + need > end) { LOG("%s: not an NSO or too big", path); free(nso); return -1; }
        ldr_module *m = ldr_map(mods, names[i], nso, size, at, err, sizeof err);
        free(nso);
        if (!m) { LOG("%s", err); return -1; }
        LOG("mapped %-7s at 0x%llx size 0x%llx, %u symbols", m->name, (unsigned long long)m->base,
            (unsigned long long)m->size, m->nsym);
        at = (at + need + 0x1fffff) & ~0x1fffffull;
    }
    if (ldr_link(mods, resolve, NULL, err, sizeof err)) { LOG("link: %s", err); return -1; }
    for (unsigned i = 0; i < ldr_count(mods); i++) {
        const ldr_module *m = ldr_module_at(mods, i);
        LOG("linked %-7s relative %u, symbolic %u (cross-module %u, weak zero %u)", m->name, m->relative, m->symbolic,
            m->cross, m->weak_zero);
    }
    return 0;
}

static int boot(const char *root, int trace) {
    char err[256];
    if (host_memory_init(err, sizeof err)) { LOG("%s", err); return 1; }
    mods = ldr_create();
    if (!mods || load_modules(root)) return 1;
    host_init(mods);
    host_set_trace(trace);
    char data[1100], profile[1100];
    snprintf(data, sizeof data, "%s/data", root);
    const char *p = pal_getenv("TOTK_PROFILE_DIR");
    if (p) snprintf(profile, sizeof profile, "%s", p);
    else if (!pal_user_data_dir("totk", profile, sizeof profile)) { LOG("no profile folder"); return 1; }
    host_set_dirs(data, profile);
    LOG("game data %s, profile %s", data, profile);

    const char *start_name = "_ZN2nn4init5StartEmmPFvvES2_S2_";
    uint64_t start = ldr_symbol(mods, start_name);
    if (!start) { LOG("missing %s", start_name); return 1; }
    jit_context ctx;
    uint32_t main_handle = host_thread_main_init(&ctx);
    bridge *br = host_bridge();
    uint64_t args[5] = {main_handle, 0, bridge_add(br, "loader_ready", thunk_void, (void *)loader_ready),
                        bridge_add(br, "module_inits", thunk_void, (void *)module_inits),
                        bridge_add(br, "module_finis", thunk_void, (void *)module_finis)};
    char where[64];
    ldr_describe(mods, start, where, sizeof where);
    LOG("ENTER original nn::init::Start at %s (main thread handle %#x)", where, main_handle);
    host_guest_call(start, 5, args);
    LOG("nn::init::Start returned (%u host callbacks)", host_calls);
    return 0;
}

typedef struct { const char *root; int trace; } boot_args;
static void *boot_thread(void *arg) {
    const boot_args *b = arg;
    int rc = boot(b->root, b->trace);
    LOG("the game ended (%d)", rc);
    exit(rc);
}

/* The run's whole output goes to <profile>/logs/last.log too (the previous run's is kept as previous.log). */
static void log_to_file(void) {
    char dir[1100], logs[1200], last[1300], prev[1300];
    const char *p = pal_getenv("TOTK_PROFILE_DIR");
    if (p) snprintf(dir, sizeof dir, "%s", p);
    else if (!pal_user_data_dir("totk", dir, sizeof dir)) return;
    snprintf(logs, sizeof logs, "%s/logs", dir);
    if (pal_mkdirs(logs)) return;
    snprintf(last, sizeof last, "%s/last.log", logs); snprintf(prev, sizeof prev, "%s/previous.log", logs);
    pal_rename_replace(last, prev);
    if (pal_tee_stdio(last) == 0) LOG("this run's log: %s", last);
    LOG("timing: a 100 us sleep takes %.0f us, a 1 ms sleep %.0f us%s", pal_sleep_probe(100000) / 1e3, pal_sleep_probe(1000000) / 1e3, pal_under_wine() ? " (Wine / Proton)" : "");
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "boot")) {
        log_to_file();
        int trace = 0, window = 1;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--trace")) trace = 1;
            else if (!strcmp(argv[i], "--no-window")) window = 0;
        }
        if (!window || fe_init("WinTears")) return boot(argv[2], trace);
        static boot_args b;
        b = (boot_args){argv[2], trace};
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 64u << 20);
        pthread_t t;
        if (pthread_create(&t, &at, boot_thread, &b)) { LOG("cannot start the game thread"); return 1; }
        fe_run();
        LOG("window closed");
        exit(0);
    }
    fprintf(stderr, "usage: wintears boot <TOTK_win folder> [--trace] [--no-window]\n");
    return 2;
}
