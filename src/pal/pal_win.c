/* Platform layer, Windows implementation (Windows 10 1803+ for placeholders; Windows 11 recommended). */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000005 /* NTDDI_WIN10_RS4: VirtualAlloc2, MapViewOfFile3 */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <knownfolders.h>
#include <process.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pal.h"

/* ---- newer APIs, looked up at run time so the exe still starts (and says why) on an old system ---------------- */

typedef PVOID(WINAPI *va2_fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *mv3_fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL(WINAPI *uv2_fn)(HANDLE, PVOID, ULONG);
typedef HRESULT(WINAPI *std_fn)(HANDLE, PCWSTR);
static va2_fn p_VirtualAlloc2;
static mv3_fn p_MapViewOfFile3;
static uv2_fn p_UnmapViewOfFile2;
static std_fn p_SetThreadDescription;
static INIT_ONCE api_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK api_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll"), k32 = GetModuleHandleW(L"kernel32.dll");
    if (kb) {
        p_VirtualAlloc2 = (va2_fn)(void *)GetProcAddress(kb, "VirtualAlloc2");
        p_MapViewOfFile3 = (mv3_fn)(void *)GetProcAddress(kb, "MapViewOfFile3");
        p_UnmapViewOfFile2 = (uv2_fn)(void *)GetProcAddress(kb, "UnmapViewOfFile2");
        p_SetThreadDescription = (std_fn)(void *)GetProcAddress(kb, "SetThreadDescription");
    }
    if (!p_SetThreadDescription && k32) p_SetThreadDescription = (std_fn)(void *)GetProcAddress(k32, "SetThreadDescription");
    return TRUE;
}
static void apis(void) { InitOnceExecuteOnce(&api_once, api_init, NULL, NULL); }

/* ---- memory ---------------------------------------------------------------------------------------------------- */

static DWORD win_prot(unsigned p) {
    int r = !!(p & PAL_PROT_READ), w = !!(p & PAL_PROT_WRITE), x = !!(p & PAL_PROT_EXEC);
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

size_t pal_page_size(void) { SYSTEM_INFO s; GetSystemInfo(&s); return s.dwPageSize; }
size_t pal_alloc_granularity(void) { SYSTEM_INFO s; GetSystemInfo(&s); return s.dwAllocationGranularity; }

/* Reservations made by pal_reserve_fixed: an address inside one is a placeholder piece, so views and private pieces
   there are replaced into, and given back as, placeholders. */
#define MAX_RESERVATIONS 128   /* guest regions, plus one per NVN virtual memory pool */
static struct { uintptr_t base; size_t size; } reservations[MAX_RESERVATIONS];
static SRWLOCK reservation_lock = SRWLOCK_INIT;

static int in_reservation(uintptr_t a) {
    int found = 0;
    AcquireSRWLockShared(&reservation_lock);
    for (int i = 0; i < MAX_RESERVATIONS; i++)
        if (reservations[i].size && a >= reservations[i].base && a - reservations[i].base < reservations[i].size) found = 1;
    ReleaseSRWLockShared(&reservation_lock);
    return found;
}

int pal_reserve_fixed(uintptr_t addr, size_t size) {
    apis();
    if (!p_VirtualAlloc2) return -1;
    void *p = p_VirtualAlloc2(NULL, (void *)addr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if ((uintptr_t)p != addr) { if (p) VirtualFree(p, 0, MEM_RELEASE); return -1; }
    AcquireSRWLockExclusive(&reservation_lock);
    int slot = -1;
    for (int i = 0; i < MAX_RESERVATIONS; i++) if (!reservations[i].size) { slot = i; break; }
    if (slot >= 0) { reservations[slot].base = addr; reservations[slot].size = size; }
    ReleaseSRWLockExclusive(&reservation_lock);
    if (slot < 0) { VirtualFree(p, 0, MEM_RELEASE); return -1; }
    return 0;
}

uintptr_t pal_reserve_below(size_t size, uintptr_t limit) {
    apis();
    if (!p_VirtualAlloc2 || !limit) return 0;
    MEM_ADDRESS_REQUIREMENTS req = {0};
    req.HighestEndingAddress = (PVOID)(limit - 1);
    MEM_EXTENDED_PARAMETER ext = {0};
    ext.Type = MemExtendedParameterAddressRequirements;
    ext.Pointer = &req;
    void *p = p_VirtualAlloc2(NULL, NULL, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &ext, 1);
    if (!p) return 0;
    AcquireSRWLockExclusive(&reservation_lock);
    int slot = -1;
    for (int i = 0; i < MAX_RESERVATIONS; i++) if (!reservations[i].size) { slot = i; break; }
    if (slot >= 0) { reservations[slot].base = (uintptr_t)p; reservations[slot].size = size; }
    ReleaseSRWLockExclusive(&reservation_lock);
    if (slot < 0) { VirtualFree(p, 0, MEM_RELEASE); return 0; }
    return (uintptr_t)p;
}

int pal_unreserve(uintptr_t addr, size_t size) {
    /* Join the placeholder pieces back into one, then free it. Coalescing fails harmlessly if it is already one. */
    VirtualFree((void *)addr, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    if (!VirtualFree((void *)addr, 0, MEM_RELEASE)) return -1;
    AcquireSRWLockExclusive(&reservation_lock);
    for (int i = 0; i < MAX_RESERVATIONS; i++) if (reservations[i].base == addr) reservations[i].size = 0;
    ReleaseSRWLockExclusive(&reservation_lock);
    return 0;
}

int pal_reserve_dynamic(uintptr_t addr, size_t size) {
    void *p = VirtualAlloc((void *)addr, size, MEM_RESERVE, PAGE_NOACCESS);
    if ((uintptr_t)p != addr) { if (p) VirtualFree(p, 0, MEM_RELEASE); return -1; }
    return 0;
}

/* Make [addr, addr + size) a placeholder of its own. Splitting fails when it already is exactly one: check that. */
static int carve(uintptr_t addr, size_t size) {
    if (VirtualFree((void *)addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return 0;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)addr, &m, sizeof m)) return -1;
    return ((uintptr_t)m.AllocationBase == addr && m.RegionSize == size && m.State == MEM_RESERVE) ? 0 : -1;
}

struct pal_section { HANDLE h; uint64_t size; };

pal_section *pal_section_create(uint64_t size) {
    /* SEC_RESERVE: no commit charge until pages are committed. Executable so the native ARM track can run code
       from guest memory; the JIT track never executes it. */
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_RESERVE,
                                  (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!h) return NULL;
    pal_section *s = (pal_section *)calloc(1, sizeof *s);
    if (!s) { CloseHandle(h); return NULL; }
    s->h = h; s->size = size;
    return s;
}

void pal_section_destroy(pal_section *s) { if (s) { CloseHandle(s->h); free(s); } }

uintptr_t pal_map_view(pal_section *s, uint64_t offset, uintptr_t addr, size_t size, unsigned prot) {
    apis();
    if (!p_MapViewOfFile3 || offset + size > s->size) return 0;
    ULONG flags = 0;
    if (addr) {
        if (!in_reservation(addr) || carve(addr, size)) return 0;
        flags = MEM_REPLACE_PLACEHOLDER;
    }
    void *p = p_MapViewOfFile3(s->h, GetCurrentProcess(), (void *)addr, offset, size, flags, win_prot(prot), NULL, 0);
    return (uintptr_t)p;
}

int pal_unmap_view(uintptr_t addr) {
    apis();
    if (in_reservation(addr) && p_UnmapViewOfFile2)
        return p_UnmapViewOfFile2(GetCurrentProcess(), (void *)addr, MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
    return UnmapViewOfFile((void *)addr) ? 0 : -1;
}

int pal_commit_private(uintptr_t addr, size_t size, unsigned prot) {
    apis();
    if (!p_VirtualAlloc2 || !in_reservation(addr) || carve(addr, size)) return -1;
    void *p = p_VirtualAlloc2(NULL, (void *)addr, size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if ((uintptr_t)p != addr) return -1;
    return VirtualAlloc((void *)addr, size, MEM_COMMIT, win_prot(prot)) ? 0 : -1;
}

int pal_release_private(uintptr_t addr, size_t size) {
    return VirtualFree((void *)addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
}

int pal_commit(uintptr_t addr, size_t size, unsigned prot) {
    return VirtualAlloc((void *)addr, size, MEM_COMMIT, win_prot(prot)) ? 0 : -1;
}

int pal_protect(uintptr_t addr, size_t size, unsigned prot) {
    /* VirtualProtect cannot cross allocations (views, private pieces): walk the regions. */
    DWORD want = win_prot(prot), old;
    uintptr_t end = addr + size;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)addr, &m, sizeof m)) return -1;
        uintptr_t region_end = (uintptr_t)m.BaseAddress + m.RegionSize;
        size_t n = (region_end < end ? region_end : end) - addr;
        if (!VirtualProtect((void *)addr, n, want, &old)) return -1;
        addr += n;
    }
    return 0;
}

int pal_discard(uintptr_t addr, size_t size) {
    return VirtualAlloc((void *)addr, size, MEM_RESET, PAGE_NOACCESS) ? 0 : -1;
}

int pal_decommit(uintptr_t addr, size_t size) {
    return VirtualFree((void *)addr, size, MEM_DECOMMIT) ? 0 : -1;
}

void *pal_alloc(size_t size, unsigned prot) { return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, win_prot(prot)); }
void pal_free(void *p, size_t size) { (void)size; if (p) VirtualFree(p, 0, MEM_RELEASE); }
void pal_flush_icache(const void *addr, size_t size) { FlushInstructionCache(GetCurrentProcess(), addr, size); }

/* ---- faults ---------------------------------------------------------------------------------------------------- */

#define MAX_FAULT_HANDLERS 8
static struct { pal_fault_fn fn; void *user; } fault_handlers[MAX_FAULT_HANDLERS];
static volatile LONG fault_count;
static volatile LONG64 fault_total;
uint64_t pal_fault_total(void) { return (uint64_t)fault_total; }
static INIT_ONCE veh_once = INIT_ONCE_STATIC_INIT;

/* pal_win_x64.S */
extern char pal_guarded_copy_insn[], pal_guarded_copy_fault[];

static LONG CALLBACK veh(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement64(&fault_total);
    ULONG_PTR op = r->ExceptionInformation[0];
    int kind = op == 1 ? PAL_FAULT_WRITE : op == 8 ? PAL_FAULT_EXEC : PAL_FAULT_READ;
    uintptr_t addr = (uintptr_t)r->ExceptionInformation[1];
    LONG n = fault_count;
    for (LONG i = 0; i < n; i++)
        if (fault_handlers[i].fn(addr, kind, e->ContextRecord, fault_handlers[i].user)) return EXCEPTION_CONTINUE_EXECUTION;
#if defined(_M_X64) || defined(__x86_64__)
    if (e->ContextRecord->Rip == (DWORD64)(uintptr_t)pal_guarded_copy_insn) {
        e->ContextRecord->Rip = (DWORD64)(uintptr_t)pal_guarded_copy_fault;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CALLBACK veh_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    return AddVectoredExceptionHandler(1, veh) != NULL;
}

static SRWLOCK fault_add_lock = SRWLOCK_INIT;
int pal_fault_add(pal_fault_fn fn, void *user) {
    if (!InitOnceExecuteOnce(&veh_once, veh_init, NULL, NULL)) return -1;
    AcquireSRWLockExclusive(&fault_add_lock);
    LONG i = fault_count;
    if (i < MAX_FAULT_HANDLERS) {
        fault_handlers[i].user = user;
        fault_handlers[i].fn = fn;
        InterlockedExchange(&fault_count, i + 1);   /* full barrier: the handler sees the entry before the count */
    }
    ReleaseSRWLockExclusive(&fault_add_lock);
    return i < MAX_FAULT_HANDLERS ? 0 : -1;
}

extern int pal_guarded_copy_asm(void *dst, const void *src, size_t n);
int pal_guarded_copy(void *dst, const void *src, size_t n) {
    InitOnceExecuteOnce(&veh_once, veh_init, NULL, NULL);
    return pal_guarded_copy_asm(dst, src, n);
}

/* ---- threads and synchronization --------------------------------------------------------------------------- */

static void set_name_w(HANDLE h, const char *name) {
    apis();
    if (!p_SetThreadDescription || !name) return;
    wchar_t w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) p_SetThreadDescription(h, w);
}

struct pal_thread { HANDLE h; pal_thread_fn fn; void *arg; char name[64]; };

static unsigned __stdcall thread_start(void *p) {
    pal_thread *t = (pal_thread *)p;
    set_name_w(GetCurrentThread(), t->name);
    t->fn(t->arg);
    return 0;
}

pal_thread *pal_thread_create(pal_thread_fn fn, void *arg, size_t stack_size, const char *name) {
    pal_thread *t = (pal_thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack_size, thread_start, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { free(t); return NULL; }
    t->h = (HANDLE)h;
    return t;
}

void pal_thread_join(pal_thread *t) {
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

void pal_thread_set_name(const char *name) { set_name_w(GetCurrentThread(), name); }

void pal_thread_set_priority(int prio) {
    int p = prio <= PAL_PRIO_LOW ? THREAD_PRIORITY_BELOW_NORMAL : prio == PAL_PRIO_NORMAL ? THREAD_PRIORITY_NORMAL
          : prio == PAL_PRIO_HIGH ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_TIME_CRITICAL;
    SetThreadPriority(GetCurrentThread(), p);
}

uint64_t pal_thread_id(void) { return GetCurrentThreadId(); }
void pal_yield(void) { SwitchToThread(); }

void pal_mutex_lock(pal_mutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
int pal_mutex_trylock(pal_mutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) ? 1 : 0; }
void pal_mutex_unlock(pal_mutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }

/* Nanoseconds to a millisecond timeout for the Win32 waits (rounded up so a short wait never becomes a spin). */
static DWORD ms_timeout(int64_t ns) {
    if (ns < 0) return INFINITE;
    uint64_t ms = ((uint64_t)ns + 999999u) / 1000000u;
    return ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
}

int pal_cond_wait(pal_cond *c, pal_mutex *m, int64_t timeout_ns) {
    if (SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, ms_timeout(timeout_ns), 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? 1 : 0;
}
void pal_cond_signal(pal_cond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void pal_cond_broadcast(pal_cond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

int pal_wait_u32(volatile uint32_t *addr, uint32_t expected, int64_t timeout_ns) {
    if (WaitOnAddress((volatile VOID *)addr, &expected, sizeof expected, ms_timeout(timeout_ns))) return 0;
    return GetLastError() == ERROR_TIMEOUT ? 1 : 0;
}
void pal_wake_one(volatile void *addr) { WakeByAddressSingle((PVOID)addr); }
void pal_wake_all(volatile void *addr) { WakeByAddressAll((PVOID)addr); }

pal_tls_key pal_tls_alloc(void) { return TlsAlloc(); }
void *pal_tls_get(pal_tls_key k) { return TlsGetValue(k); }
void pal_tls_set(pal_tls_key k, void *v) { TlsSetValue(k, v); }

/* ---- time ------------------------------------------------------------------------------------------------------ */

static LARGE_INTEGER qpc_freq;
static INIT_ONCE qpc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK qpc_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; return QueryPerformanceFrequency(&qpc_freq); }

uint64_t pal_now_ns(void) {
    InitOnceExecuteOnce(&qpc_once, qpc_init, NULL, NULL);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t f = (uint64_t)qpc_freq.QuadPart, v = (uint64_t)t.QuadPart;
    return v / f * 1000000000ull + v % f * 1000000000ull / f;
}

uint64_t pal_wall_time_ns(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
    return t * 100;
}

static DWORD timer_tls = TLS_OUT_OF_INDEXES;
static INIT_ONCE timer_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK timer_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; timer_tls = TlsAlloc(); return TRUE; }

void pal_sleep_ns(uint64_t ns) {
    InitOnceExecuteOnce(&timer_once, timer_init, NULL, NULL);
    HANDLE t = timer_tls != TLS_OUT_OF_INDEXES ? (HANDLE)TlsGetValue(timer_tls) : NULL;
    if (!t && timer_tls != TLS_OUT_OF_INDEXES) {
        t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) t = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);   /* before Windows 10 1803 */
        TlsSetValue(timer_tls, t);
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100);
    if (t && SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(t, INFINITE);
    else Sleep(ms_timeout((int64_t)ns));
}

/* ---- files and paths ------------------------------------------------------------------------------------------- */

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static size_t utf8(const wchar_t *w, char *out, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return n > 0 ? (size_t)n - 1 : 0;
}

struct pal_file { HANDLE h; HANDLE map; };

pal_file *pal_file_open(const char *path_utf8, unsigned mode) {
    wchar_t *w = wide(path_utf8);
    if (!w) return NULL;
    DWORD access = (mode & PAL_FILE_READ ? GENERIC_READ : 0) | (mode & PAL_FILE_WRITE ? GENERIC_WRITE : 0);
    DWORD disp = (mode & PAL_FILE_CREATE)
                     ? ((mode & PAL_FILE_EXCL) ? CREATE_NEW : (mode & PAL_FILE_TRUNCATE) ? CREATE_ALWAYS : OPEN_ALWAYS)
                     : ((mode & PAL_FILE_TRUNCATE) ? TRUNCATE_EXISTING : OPEN_EXISTING);
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    pal_file *f = (pal_file *)calloc(1, sizeof *f);
    if (!f) { CloseHandle(h); return NULL; }
    f->h = h;
    return f;
}

void pal_file_close(pal_file *f) {
    if (!f) return;
    if (f->map) CloseHandle(f->map);
    CloseHandle(f->h);
    free(f);
}

int64_t pal_file_size(pal_file *f) {
    LARGE_INTEGER s;
    return GetFileSizeEx(f->h, &s) ? (int64_t)s.QuadPart : -1;
}

int64_t pal_file_pread(pal_file *f, void *buf, size_t n, uint64_t offset) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED o = {0};
        uint64_t at = offset + done;
        o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32);
        DWORD want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done), got = 0;
        if (!ReadFile(f->h, (char *)buf + done, want, &got, &o)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return -1;
        }
        if (!got) break;
        done += got;
    }
    return (int64_t)done;
}

int64_t pal_file_pwrite(pal_file *f, const void *buf, size_t n, uint64_t offset) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED o = {0};
        uint64_t at = offset + done;
        o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32);
        DWORD want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done), put = 0;
        if (!WriteFile(f->h, (const char *)buf + done, want, &put, &o) || !put) return -1;
        done += put;
    }
    return (int64_t)done;
}

int pal_file_flush(pal_file *f) { return FlushFileBuffers(f->h) ? 0 : -1; }

const void *pal_file_map(pal_file *f, uint64_t *size_out) {
    int64_t size = pal_file_size(f);
    if (size <= 0) return NULL;
    if (!f->map) f->map = CreateFileMappingW(f->h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!f->map) return NULL;
    const void *p = MapViewOfFile(f->map, FILE_MAP_READ, 0, 0, 0);
    if (p && size_out) *size_out = (uint64_t)size;
    return p;
}

void pal_file_unmap(const void *p) { if (p) UnmapViewOfFile(p); }

int pal_mkdirs(const char *path_utf8) {
    wchar_t *w = wide(path_utf8);
    if (!w) return -1;
    int rc = 0;
    for (wchar_t *p = w; ; p++) {
        if (*p == L'\\' || *p == L'/' || *p == 0) {
            wchar_t c = *p;
            *p = 0;
            size_t len = wcslen(w);
            /* skip "", drive roots like "C:" and the "\\?" / "\\server" prefixes */
            if (len && !(len == 2 && w[1] == L':') && !(w[0] == L'\\' && wcschr(w + 2, L'\\') == NULL))
                if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) rc = -1;
            *p = c;
            if (!c) break;
        }
    }
    DWORD a = GetFileAttributesW(w);
    free(w);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : rc ? rc : -1;
}

static int map_error(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_DIRECTORY: return PAL_ENOENT;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return PAL_EEXIST;
    case ERROR_DIR_NOT_EMPTY: return PAL_ENOTEMPTY;
    default: return PAL_ERR;
    }
}

int pal_last_file_error(void) { return map_error(GetLastError()); }

static uint64_t filetime_ns(FILETIME t) { return (((uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime) - 116444736000000000ull) * 100; }

int pal_file_stat(pal_file *f, pal_stat *out) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(f->h, &i)) return map_error(GetLastError());
    out->is_dir = !!(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    out->size = (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow;
    out->mtime_ns = filetime_ns(i.ftLastWriteTime);
    out->volume = i.dwVolumeSerialNumber;
    out->index = (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow;
    return 0;
}

int pal_stat_path(const char *path_utf8, pal_stat *out) {
    wchar_t *w = wide(path_utf8);
    if (!w) return PAL_ERR;
    /* FILE_FLAG_BACKUP_SEMANTICS opens directories too; no access rights are needed for the information */
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return map_error(GetLastError());
    struct pal_file f = {h, NULL};
    int rc = pal_file_stat(&f, out);
    CloseHandle(h);
    return rc;
}

int pal_file_truncate(pal_file *f, uint64_t size) {
    FILE_END_OF_FILE_INFO e;
    e.EndOfFile.QuadPart = (LONGLONG)size;
    return SetFileInformationByHandle(f->h, FileEndOfFileInfo, &e, sizeof e) ? 0 : map_error(GetLastError());
}

int pal_mkdir(const char *path_utf8) {
    wchar_t *w = wide(path_utf8);
    int ok = w && CreateDirectoryW(w, NULL);
    int rc = ok ? 0 : map_error(GetLastError());
    free(w);
    return rc;
}

int pal_remove_file(const char *path_utf8) {
    wchar_t *w = wide(path_utf8);
    int ok = w && DeleteFileW(w);
    int rc = ok ? 0 : map_error(GetLastError());
    free(w);
    return rc;
}

int pal_remove_dir(const char *path_utf8) {
    wchar_t *w = wide(path_utf8);
    int ok = w && RemoveDirectoryW(w);
    int rc = ok ? 0 : map_error(GetLastError());
    free(w);
    return rc;
}

int pal_rename(const char *from, const char *to) {
    wchar_t *a = wide(from), *b = wide(to);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_WRITE_THROUGH);
    int rc = ok ? 0 : map_error(GetLastError());
    free(a); free(b);
    return rc;
}

struct pal_dir { HANDLE h; WIN32_FIND_DATAW d; int pending; };

pal_dir *pal_dir_open(const char *path_utf8) {
    size_t n = strlen(path_utf8);
    char *pattern = (char *)malloc(n + 3);
    if (!pattern) return NULL;
    memcpy(pattern, path_utf8, n);
    memcpy(pattern + n, "\\*", 3);
    wchar_t *w = wide(pattern);
    free(pattern);
    if (!w) return NULL;
    pal_dir *d = (pal_dir *)calloc(1, sizeof *d);
    if (!d) { free(w); return NULL; }
    d->h = FindFirstFileW(w, &d->d);
    free(w);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(d);
        SetLastError(e);
        return NULL;
    }
    d->pending = 1;
    return d;
}

int pal_dir_next(pal_dir *d, char *name, size_t cap, int *is_dir, uint64_t *size) {
    for (;;) {
        if (!d->pending && !FindNextFileW(d->h, &d->d)) return 0;
        d->pending = 0;
        if (!wcscmp(d->d.cFileName, L".") || !wcscmp(d->d.cFileName, L"..")) continue;
        if (!utf8(d->d.cFileName, name, cap)) continue;   /* a name too long for the caller is skipped */
        if (is_dir) *is_dir = !!(d->d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (size) *size = (uint64_t)d->d.nFileSizeHigh << 32 | d->d.nFileSizeLow;
        return 1;
    }
}

void pal_dir_close(pal_dir *d) { if (d) { FindClose(d->h); free(d); } }

int pal_rename_replace(const char *from, const char *to) {
    wchar_t *a = wide(from), *b = wide(to);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a); free(b);
    return ok ? 0 : -1;
}

int pal_path_exists(const char *path_utf8) {
    wchar_t *w = wide(path_utf8);
    if (!w) return 0;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

size_t pal_user_data_dir(const char *app, char *out, size_t cap) {
    wchar_t base[MAX_PATH];
    PWSTR known = NULL;
    DWORD n = 0;
    /* the shell's answer first: shells that rebuild the environment (MSYS login shells) can drop LOCALAPPDATA */
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known) == S_OK && wcslen(known) < MAX_PATH) {
        wcscpy(base, known);
        n = (DWORD)wcslen(base);
    }
    CoTaskMemFree(known);
    if (!n) n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        n = GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH - 20);
        if (!n || n >= MAX_PATH - 20) return 0;
        wcscat(base, L"\\AppData\\Local");
    }
    char b[MAX_PATH * 3];
    if (!utf8(base, b, sizeof b)) return 0;
    int len = snprintf(out, cap, "%s\\WinTears\\%s", b, app ? app : "");
    if (len <= 0 || (size_t)len >= cap) return 0;
    if (len && (out[len - 1] == '\\')) out[--len] = 0;
    return pal_mkdirs(out) ? 0 : (size_t)len;
}

size_t pal_exe_dir(char *out, size_t cap) {
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(w, L'\\');
    if (slash) *slash = 0;
    return utf8(w, out, cap);
}

/* ---- misc ------------------------------------------------------------------------------------------------------ */

const char *pal_getenv(const char *name) {
    static __thread char *buf;
    wchar_t *wn = wide(name);
    if (!wn) return NULL;
    wchar_t v[32768];
    DWORD n = GetEnvironmentVariableW(wn, v, 32768);
    free(wn);
    if (!n || n >= 32768) return NULL;
    if (!buf) buf = (char *)malloc(32768 * 3);
    if (!buf) return NULL;
    utf8(v, buf, 32768 * 3);
    return buf;
}

/* ---- the run's log file: stdout and stderr go through a pipe; a thread writes everything both to the console and to
   the file, so the whole run is in the file once the game closes (or fails) ---- */
#include <fcntl.h>
#include <io.h>
static HANDLE g_tee_console = INVALID_HANDLE_VALUE, g_tee_file = INVALID_HANDLE_VALUE, g_tee_thread;
static DWORD WINAPI tee_run(LPVOID arg) {
    HANDLE rd = (HANDLE)arg; char buf[8192]; DWORD n, w;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        if (g_tee_console != INVALID_HANDLE_VALUE) WriteFile(g_tee_console, buf, n, &w, NULL);
        WriteFile(g_tee_file, buf, n, &w, NULL);
    }
    return 0;
}
void pal_tee_flush(void) {   /* before the process ends: the pipe is closed and the thread writes what is left */
    if (!g_tee_thread) return;
    fflush(stdout); fflush(stderr);
    _close(1); _close(2);   /* the last write ends of the pipe: the reader sees its end */
    WaitForSingleObject(g_tee_thread, 3000);
    FlushFileBuffers(g_tee_file);
    g_tee_thread = NULL;
}
int pal_tee_stdio(const char *path_utf8) {
    wchar_t *wp = wide(path_utf8);
    if (!wp) return -1;
    g_tee_file = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wp);
    if (g_tee_file == INVALID_HANDLE_VALUE) return -1;
    HANDLE con = GetStdHandle(STD_ERROR_HANDLE);
    if (con && con != INVALID_HANDLE_VALUE) DuplicateHandle(GetCurrentProcess(), con, GetCurrentProcess(), &g_tee_console, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, NULL, 1 << 16)) return -1;
    int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_BINARY);
    if (fd < 0) return -1;
    fflush(stdout); fflush(stderr);
    if (_dup2(fd, 1) || _dup2(fd, 2)) return -1;
    _close(fd);
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(1)); SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    g_tee_thread = CreateThread(NULL, 0, tee_run, rd, 0, NULL);
    if (!g_tee_thread) return -1;
    atexit(pal_tee_flush);
    return 0;
}

int pal_under_wine(void) {
    static int w = -1;
    if (w < 0) { HMODULE h = GetModuleHandleA("ntdll.dll"); w = h && GetProcAddress(h, "wine_get_version") != NULL; }
    return w;
}

#include <tlhelp32.h>
void pal_thread_cpu_report(char *out, size_t cap) {
    enum { MAXT = 256 };
    static struct { DWORD id; uint64_t cpu; } prev[MAXT]; static int nprev; static uint64_t prev_wall;
    struct { DWORD id; uint64_t cpu, used; char name[48]; } cur[MAXT]; int ncur = 0;
    uint64_t wall = pal_now_ns();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    out[0] = 0;
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && ncur < MAXT; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        FILETIME c, x, k, u;
        if (GetThreadTimes(th, &c, &x, &k, &u)) {
            uint64_t t = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) * 100;   /* ns */
            cur[ncur].id = te.th32ThreadID; cur[ncur].cpu = t; cur[ncur].used = t;
            for (int i = 0; i < nprev; i++) if (prev[i].id == te.th32ThreadID) { cur[ncur].used = t - prev[i].cpu; break; }
            snprintf(cur[ncur].name, sizeof cur[ncur].name, "%lu", (unsigned long)te.th32ThreadID);
            typedef HRESULT (WINAPI *gtd_t)(HANDLE, PWSTR *);
            static gtd_t gtd; static int looked;
            if (!looked) { looked = 1; gtd = (gtd_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription"); }
            PWSTR d = NULL;
            if (gtd && SUCCEEDED(gtd(th, &d)) && d) { if (d[0]) WideCharToMultiByte(CP_UTF8, 0, d, -1, cur[ncur].name, sizeof cur[ncur].name, NULL, NULL); LocalFree(d); }
            ncur++;
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    uint64_t span = prev_wall ? wall - prev_wall : 0;
    nprev = ncur; for (int i = 0; i < ncur; i++) { prev[i].id = cur[i].id; prev[i].cpu = cur[i].cpu; }
    prev_wall = wall;
    if (!span) { snprintf(out, cap, "(first sample)"); return; }
    size_t at = 0;
    for (int shown = 0; shown < 10; shown++) {   /* the busiest first */
        int best = -1;
        for (int i = 0; i < ncur; i++) if (cur[i].used && (best < 0 || cur[i].used > cur[best].used)) best = i;
        if (best < 0 || cur[best].used * 100 < span) break;   /* under 1 % */
        at += (size_t)snprintf(out + at, at < cap ? cap - at : 0, "%s%s %.0f%%", shown ? ", " : "", cur[best].name, cur[best].used * 100.0 / span);
        cur[best].used = 0;
        if (at >= cap) break;
    }
}

int pal_process_memory(uint64_t *resident, uint64_t *committed) {
    PROCESS_MEMORY_COUNTERS_EX m = {.cb = sizeof m};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m)) return -1;
    *resident = m.WorkingSetSize; *committed = m.PrivateUsage;
    return 0;
}

void pal_debug_write(const char *text) {
    fputs(text, stderr);
    fflush(stderr);
    OutputDebugStringA(text);
}
