/* wintears.exe: the Windows runtime. For now one mode, the boot probe of phase 3A:

     wintears boot <TOTK_win folder> [--trace]

   It loads the game's original modules from <TOTK_win>/data/v0/exefs (main, subsdk0, sdk; rtld is not run: the
   loader links instead, as upstream does), relocates and links them, and runs the SDK's own entry,
   nn::init::Start(main thread handle, 0, loader_ready, module_inits, module_finis), in the JIT on this thread. Every
   svc is logged; the Horizon host is not ported yet, so the first svc it cannot answer ends the run with the guest's
   registers and call chain. That is the measure of progress for this phase.

   Guest layout (as upstream runtime/services.c, so GetInfo answers stay the same when the host is ported):
     low region, 6 GiB, the first free GiB from 0x3_0000_0000:
       +0x0000_0000  modules (main, subsdk0, sdk), 2 MiB aligned
       +0x0ff0_0000  runtime page: bridge stubs, the return trap, the main thread's TLS
       +0x1000_0000  stack region (256 MiB): the main thread's stack at its top
       +0x3000_0000  heap (up to 4 GiB)
     alias region at 0x70_0000_0000: not needed before the host is ported */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge/bridge.h"
#include "jit/jit.h"
#include "loader/loader.h"
#include "pal.h"

#define LOW_SEARCH_FROM 0x300000000ull
#define LOW_SIZE (6ull << 30)
#define RUNTIME_OFF 0x0ff00000ull
#define RUNTIME_SIZE 0x00100000ull
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define MAIN_STACK (8ull << 20)

#define STUBS_OFF 0x0               /* bridge stubs, 64 KiB */
#define STUBS_SIZE 0x10000
#define TRAP_OFF 0x10000            /* svc #RETURN_SVC: the return address of host-to-guest calls */
#define TLS_OFF 0x11000             /* main thread: TLR (0x200, tpidrro_el0), its ThreadType stand-in, TPIDR_EL0 area */
#define RETURN_SVC 0xfffe
#define MAIN_THREAD_HANDLE 0xde00u  /* stand-in until the thread host is ported (upstream: svc_thread_main_init) */

static uint64_t low;
static ldr_set *mods;
static bridge *br;
static jit_core *core;
static jit_context *current;        /* the context the JIT runs now (nested host-to-guest calls replace it) */
static int dead, trace;
static unsigned svc_count, host_calls;

static void logf_(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fputs("[wintears] ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
}
#define LOG(...) logf_(__VA_ARGS__)

static const char *svc_name(uint32_t n) {
    static const char *names[] = {
        [0x01] = "SetHeapSize", [0x02] = "SetMemoryPermission", [0x03] = "SetMemoryAttribute", [0x04] = "MapMemory",
        [0x05] = "UnmapMemory", [0x06] = "QueryMemory", [0x07] = "ExitProcess", [0x08] = "CreateThread",
        [0x09] = "StartThread", [0x0a] = "ExitThread", [0x0b] = "SleepThread", [0x0c] = "GetThreadPriority",
        [0x0d] = "SetThreadPriority", [0x0e] = "GetThreadCoreMask", [0x0f] = "SetThreadCoreMask",
        [0x10] = "GetCurrentProcessorNumber", [0x11] = "SignalEvent", [0x12] = "ClearEvent",
        [0x13] = "MapSharedMemory", [0x14] = "UnmapSharedMemory", [0x15] = "CreateTransferMemory",
        [0x16] = "CloseHandle", [0x17] = "ResetSignal", [0x18] = "WaitSynchronization",
        [0x19] = "CancelSynchronization", [0x1a] = "ArbitrateLock", [0x1b] = "ArbitrateUnlock",
        [0x1c] = "WaitProcessWideKeyAtomic", [0x1d] = "SignalProcessWideKey", [0x1e] = "GetSystemTick",
        [0x1f] = "ConnectToNamedPort", [0x21] = "SendSyncRequest", [0x22] = "SendSyncRequestWithUserBuffer",
        [0x24] = "GetProcessId", [0x25] = "GetThreadId", [0x26] = "Break", [0x27] = "OutputDebugString",
        [0x28] = "ReturnFromException", [0x29] = "GetInfo", [0x2c] = "MapPhysicalMemory",
        [0x2d] = "UnmapPhysicalMemory", [0x32] = "SetThreadActivity", [0x33] = "GetThreadContext3",
        [0x34] = "WaitForAddress", [0x35] = "SignalToAddress", [0x36] = "SynchronizePreemptionState",
        [0x40] = "CreateSession", [0x41] = "AcceptSession", [0x43] = "ReplyAndReceive", [0x45] = "CreateEvent",
        [0x4b] = "CreateCodeMemory", [0x4c] = "ControlCodeMemory", [0x50] = "CreateSharedMemory",
        [0x51] = "MapTransferMemory", [0x52] = "UnmapTransferMemory",
    };
    return n < sizeof names / sizeof *names && names[n] ? names[n] : "?";
}

static void describe(uint64_t a, char *out, size_t cap) {
    uint64_t rt = low + RUNTIME_OFF;
    if (a == rt + TRAP_OFF) snprintf(out, cap, "host (guest call return)");
    else if (a >= rt + STUBS_OFF && a < rt + STUBS_OFF + STUBS_SIZE)
        snprintf(out, cap, "host stub %s", bridge_name(br, BRIDGE_SVC_BASE + (uint32_t)((a - rt - STUBS_OFF) / 8)) ?: "?");
    else ldr_describe(mods, a, out, cap);
}

/* The guest frame-pointer chain (read with pal_guarded_copy: a bad pointer ends the walk). */
static void report_guest(const jit_context *c) {
    char where[64];
    describe(c->pc, where, sizeof where);
    LOG("  pc %s", where);
    describe(c->x[30], where, sizeof where);
    LOG("  lr %s", where);
    for (int i = 0; i < 8; i++) LOG("  x%d = 0x%llx", i, (unsigned long long)c->x[i]);
    LOG("  sp = 0x%llx  fp = 0x%llx", (unsigned long long)c->sp, (unsigned long long)c->x[29]);
    uint64_t fp = c->x[29];
    for (int depth = 0; depth < 16 && fp && !(fp & 7); depth++) {
        uint64_t frame[2];
        if (pal_guarded_copy(frame, (const void *)(uintptr_t)fp, sizeof frame)) break;
        describe(frame[1], where, sizeof where);
        LOG("  called from %s", where);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
}

/* ---- svcs ---------------------------------------------------------------------------------------------------------- */

/* Returns 1 if the svc was answered. None are yet: the Horizon host (upstream runtime/threads.c, services.c, ipc.c)
   is the next task. */
static int handle_svc(uint32_t n, jit_context *c) {
    (void)n; (void)c;
    return 0;
}

/* ---- running guest code ---------------------------------------------------------------------------------------- */

static int run(jit_context *c) {
    jit_context *outer = current;
    current = c;
    int result = -1;
    while (!dead) {
        jit_load(core, c);
        int stop = jit_run(core);
        jit_save(core, c);
        if (stop == JIT_STOP_HALT) continue;
        if (stop == JIT_STOP_EXCEPTION) {
            uint64_t pc;
            int kind = jit_exception(core, &pc);
            char where[64];
            describe(pc, where, sizeof where);
            uint32_t insn = 0;
            pal_guarded_copy(&insn, (const void *)(uintptr_t)pc, 4);
            LOG("guest exception %d at %s (instruction 0x%08x)", kind, where, insn);
            report_guest(c);
            dead = 1;
            break;
        }
        uint32_t n = jit_svc_number(core);
        if (n == RETURN_SVC) { result = 0; break; }
        if (n >= BRIDGE_SVC_BASE && bridge_dispatch(br, n, c)) continue;
        svc_count++;
        char where[64];
        describe(c->pc - 4, where, sizeof where);
        if (trace)
            LOG("svc 0x%02x %s at %s x0=0x%llx x1=0x%llx x2=0x%llx", n, svc_name(n), where,
                (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
        if (handle_svc(n, c)) continue;
        LOG("first unimplemented svc: 0x%02x %s at %s (svc number %u of this run)", n, svc_name(n), where, svc_count);
        report_guest(c);
        dead = 1;
    }
    current = outer;
    return dead ? -1 : result;
}

/* Call guest code from the host (thread entry, module constructors): a fresh context on the current guest stack,
   returning to the trap. */
static uint64_t guest_call(uint64_t fn, int n, const uint64_t *args) {
    jit_context c;
    memset(&c, 0, sizeof c);
    if (current) {
        c = *current;
        c.sp = (current->sp - 0x200) & ~15ull;   /* below the caller's frame, keeping its red zone */
    }
    for (int i = 0; i < n && i < 8; i++) c.x[i] = args[i];
    c.pc = fn;
    c.x[30] = low + RUNTIME_OFF + TRAP_OFF;
    c.x[29] = 0;
    run(&c);
    return c.x[0];
}

/* ---- host callbacks the SDK's nn::init::Start calls ------------------------------------------------------------ */

static void thunk_void(jit_context *c, void *fn) { (void)c; host_calls++; ((void (*)(void))fn)(); }

static void loader_ready(void) { LOG("rtld callback: modules linked"); }

static void module_inits(void) {
    LOG("SDK startup returned; running module constructors");
    for (unsigned i = 0; i < ldr_count(mods) && !dead; i++) {
        const ldr_module *m = ldr_module_at(mods, i);
        if (!m->init) continue;
        LOG("ENTER %s DT_INIT", m->name);
        guest_call(m->base + m->init, 0, NULL);
        if (!dead) LOG("RETURN %s DT_INIT", m->name);
    }
}

static void module_finis(void) {
    LOG("guest exited: module finalizers are not implemented");
    dead = 1;
}

/* Imports defined in no module become stubs that stop the run and name the import. */
static void thunk_unresolved(jit_context *c, void *name) {
    (void)c;
    LOG("call to an import no module defines: %s", (const char *)name);
    if (current) report_guest(current);
    dead = 1;
}

static uint64_t resolve(void *user, const ldr_module *m, const char *name, unsigned kind, int phase) {
    (void)user; (void)m; (void)kind;
    if (phase == 0) return 0;      /* no host overrides yet (upstream runtime_override): the SDK's own code runs */
    LOG("import defined nowhere: %s (bound to a trap stub)", name);
    return bridge_add(br, name, thunk_unresolved, (void *)name);
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

static int reserve_low(void) {
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= 0x7000000000ull; at += 1ull << 30)
        if (pal_reserve_fixed((uintptr_t)at, (size_t)LOW_SIZE) == 0) { low = at; return 0; }
    return -1;
}

static int load_modules(const char *root) {
    static const char *names[] = {"main", "subsdk0", "sdk"};   /* upstream's order (runtime_symbol search order) */
    char path[1024], err[256];
    uint64_t at = low;
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/data/v0/exefs/%s", root, names[i]);
        size_t size;
        uint8_t *nso = read_file(path, &size);
        if (!nso) { LOG("cannot read %s", path); return -1; }
        uint64_t need = ldr_nso_image_size(nso, size);
        if (!need || at + need > low + RUNTIME_OFF) { LOG("%s: not an NSO or too big", path); free(nso); return -1; }
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

static int boot(const char *root) {
    if (reserve_low()) { LOG("cannot reserve the guest's low region below 0x70_0000_0000"); return 1; }
    LOG("guest low region at 0x%llx", (unsigned long long)low);
    uint64_t rt = low + RUNTIME_OFF;
    uint64_t stack_top = low + STACK_OFF + STACK_SIZE;
    if (pal_commit_private((uintptr_t)rt, RUNTIME_SIZE, PAL_PROT_READ | PAL_PROT_WRITE) ||
        pal_commit_private((uintptr_t)(stack_top - MAIN_STACK), MAIN_STACK, PAL_PROT_READ | PAL_PROT_WRITE)) {
        LOG("cannot commit the runtime page or the main stack");
        return 1;
    }
    br = bridge_create((uintptr_t)(rt + STUBS_OFF), STUBS_SIZE);
    uint32_t trap = 0xd4000001u | RETURN_SVC << 5;
    memcpy((void *)(uintptr_t)(rt + TRAP_OFF), &trap, 4);
    mods = ldr_create();
    if (!br || !mods || load_modules(root)) return 1;

    /* Main thread TLS, as upstream runtime_thread_init: TLR (tpidrro_el0) whose +0x1f8 points to a ThreadType
       stand-in, and an 8 KiB area for TPIDR_EL0. */
    uint64_t tlr = rt + TLS_OFF, thread_type = tlr + 0x200, tpidr = tlr + 0x1000;
    memcpy((void *)(uintptr_t)(tlr + 0x1f8), &thread_type, 8);

    const char *start_name = "_ZN2nn4init5StartEmmPFvvES2_S2_";
    uint64_t start = ldr_symbol(mods, start_name);
    if (!start) { LOG("missing %s", start_name); return 1; }
    char where[64];
    describe(start, where, sizeof where);
    uint64_t args[5] = {MAIN_THREAD_HANDLE, 0, bridge_add(br, "loader_ready", thunk_void, (void *)loader_ready),
                        bridge_add(br, "module_inits", thunk_void, (void *)module_inits),
                        bridge_add(br, "module_finis", thunk_void, (void *)module_finis)};
    jit_config cfg = {0};
    cfg.fastmem = 1;
    core = jit_core_create(&cfg, NULL, 0);

    jit_context c;
    memset(&c, 0, sizeof c);
    c.sp = stack_top - 0x100;
    c.tpidrro_el0 = tlr;
    c.tpidr_el0 = tpidr;
    current = &c;
    LOG("ENTER original nn::init::Start at %s", where);
    uint64_t t0 = pal_now_ns();
    guest_call(start, 5, args);
    LOG("stopped after %.1f ms: %u svcs, %u host callbacks%s", (pal_now_ns() - t0) / 1e6, svc_count, host_calls,
        dead ? "" : " (nn::init::Start returned)");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "boot")) {
        for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--trace")) trace = 1;
        return boot(argv[2]);
    }
    fprintf(stderr, "usage: wintears boot <TOTK_win folder> [--trace]\n");
    return 2;
}
