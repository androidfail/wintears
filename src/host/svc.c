/* svc dispatch (upstream runtime/services.c runtime_svc), logging and guest reports. */
#include <stdarg.h>

#include "host/hos.h"

ldr_set *host_mods;
int host_trace_svcs;
static pal_mutex log_lock = PAL_MUTEX_INIT;

void host_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    pal_mutex_lock(&log_lock);
    fputs("[wintears] ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    pal_mutex_unlock(&log_lock);
    va_end(a);
}

_Noreturn void host_fail(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    pal_mutex_lock(&log_lock);
    fputs("[wintears] FAIL: ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(a);
    _Exit(1);   /* other guest threads may be running: end the process here */
}

_Noreturn void runtime_fail(const char *message) { host_fail("%s", message); }

/* upstream's guest call-chain print (failure reports in ipc.c) */
void runtime_trace_guest(uint64_t fp, uint64_t lr) {
    char where[96];
    host_describe(lr, where, sizeof where);
    LOG("  guest caller %s", where);
    for (int depth = 0; depth < 16 && fp && !(fp & 7); depth++) {
        uint64_t frame[2];
        if (pal_guarded_copy(frame, (const void *)(uintptr_t)fp, sizeof frame)) break;
        host_describe(frame[1], where, sizeof where);
        LOG("  called from %s", where);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
}

void host_init(ldr_set *mods) {
    host_mods = mods;
    threads_init();
}

void host_set_trace(int svcs) { host_trace_svcs = svcs; }

uint64_t host_symbol(const char *name) { return ldr_symbol(host_mods, name); }

void host_describe(uint64_t a, char *out, size_t cap) {
    uint64_t rt = host_runtime_page();
    if (a == host_return_trap()) snprintf(out, cap, "host (guest call return)");
    else if (a >= rt && a < rt + 0x10000) {
        const char *n = bridge_name(host_bridge(), BRIDGE_SVC_BASE + (uint32_t)((a - rt) / 8));
        snprintf(out, cap, "host stub %s", n ? n : "?");
    } else {
        ldr_describe(host_mods, a, out, cap);
        uint64_t off;
        const char *sym = ldr_nearest_symbol(host_mods, a, &off);
        size_t n = strlen(out);
        if (sym && n + 4 < cap) snprintf(out + n, cap - n, " (%s+0x%llx)", sym, (unsigned long long)off);
    }
}

/* Registers and the guest frame-pointer chain (pal_guarded_copy: a bad pointer ends the walk). */
void host_report_guest(const jit_context *c) {
    char where[96];
    host_describe(c->pc, where, sizeof where);
    LOG("  pc %s", where);
    host_describe(c->x[30], where, sizeof where);
    LOG("  lr %s", where);
    for (int i = 0; i < 8; i += 2)
        LOG("  x%d = 0x%llx  x%d = 0x%llx", i, (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1]);
    LOG("  sp = 0x%llx  fp = 0x%llx", (unsigned long long)c->sp, (unsigned long long)c->x[29]);
    uint64_t fp = c->x[29];
    for (int depth = 0; depth < 16 && fp && !(fp & 7); depth++) {
        uint64_t frame[2];
        if (pal_guarded_copy(frame, (const void *)(uintptr_t)fp, sizeof frame)) break;
        host_describe(frame[1], where, sizeof where);
        LOG("  called from %s", where);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
}

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

int host_svc(unsigned num, jit_context *c) {
    uint64_t *r = c->x;
    runtime_core_checkpoint();
    if (host_trace_svcs) {
        char where[96];
        host_describe(c->pc - 4, where, sizeof where);
        LOG("svc 0x%02x %s at %s x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx", num, svc_name(num), where,
            (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]);
    }
    if (svc_thread((int)num, r)) return 1;
    if (runtime_ipc_svc(num, r)) return 1;
    if (memory_svc(num, r)) return 1;
    LOG("unimplemented svc 0x%02x %s", num, svc_name(num));
    if (num == 0x1f) LOG("  named port %.12s", (const char *)(uintptr_t)r[1]);
    return 0;
}

