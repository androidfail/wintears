/* The program's allocator: operator new / delete and, through the linker (--wrap, CMakeLists.txt), malloc, calloc,
   realloc and free of everything linked into wintears.exe. Written for Wine / Proton, whose heap is the main cost:

   - Small blocks (up to 1 KiB, 16-byte classes) come from a per-thread cache without any lock. Under Wine every
     HeapAlloc takes the process heap's lock, and on the Steam Deck the cores spent about 40 % of their time asleep on
     it (the JIT compiler, and the renderer's per-draw and per-update allocations on several threads).
   - Large blocks (256 KiB and up: texture conversion buffers, write-back copies) are kept in a small process-wide
     cache for reuse. Wine maps every large heap block with mmap and unmaps it on free, so each one cost page faults
     to fill it and a cross-CPU TLB flush to release it.
   Wine's low-fragmentation heap is no help: Wine refuses to turn it on.

   Each block has a 16-byte header: its class mixed with its address and a constant (so a pointer that the C runtime
   allocated by itself, strdup's for example, is recognised and given back to the runtime), and its usable size.
   A block freed by another thread joins that thread's cache. Linked statically with libstdc++, so the library's own
   allocations come here too. */
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>
#include <windows.h>

extern "C" {
void *__real_malloc(std::size_t n);
void __real_free(void *p);
void *__real_realloc(void *p, std::size_t n);
}

namespace {
constexpr std::size_t kHeader = 16, kStep = 16, kClasses = 64, kKeep = 256;   /* at most 8.5 MiB cached per thread */
constexpr std::size_t kLarge = 256u << 10;
constexpr std::uint64_t kMagic = 0x57696e5465617273ull;   /* "WinTears" */
constexpr std::uint64_t kLargeClass = kClasses + 1;
struct Free { Free *next; };
struct Cache { Free *head[kClasses + 1]; std::uint32_t count[kClasses + 1]; };
/* The thread's cache, in a Windows TLS slot (C++ thread_local would allocate through malloc: this file). A thread's
   cached blocks stay allocated when it exits. Callers' GetLastError is kept: HeapAlloc does not change it either. */
volatile LONG g_tls = (LONG)TLS_OUT_OF_INDEXES;
Cache *cache() {
    DWORD err = GetLastError();
    LONG i = g_tls;
    if (i == (LONG)TLS_OUT_OF_INDEXES) {
        LONG n = (LONG)TlsAlloc();
        if (InterlockedCompareExchange(&g_tls, n, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES) TlsFree((DWORD)n);
        i = g_tls;
    }
    auto *c = static_cast<Cache *>(TlsGetValue((DWORD)i));
    if (!c) {
        c = static_cast<Cache *>(__real_malloc(sizeof(Cache)));
        if (c) { std::memset(c, 0, sizeof *c); TlsSetValue((DWORD)i, c); }
    }
    SetLastError(err);
    return c;
}

struct Header { std::uint64_t tag, size; };
inline Header *header_of(void *user) { return reinterpret_cast<Header *>(static_cast<unsigned char *>(user) - kHeader); }
inline std::uint64_t tag_for(void *user, std::uint64_t cls) { return kMagic ^ reinterpret_cast<std::uintptr_t>(user) ^ cls; }
/* The block's class (1..kClasses small, kClasses + 1 large, 0 plain), or -1 when the runtime allocated it. */
inline long class_of(void *user) {
    std::uint64_t c = header_of(user)->tag ^ kMagic ^ reinterpret_cast<std::uintptr_t>(user);
    return c <= kLargeClass ? static_cast<long>(c) : -1;
}

/* Large blocks: sizes rounded to a quarter of a power of two, freed ones kept by size, up to 64 MiB in all and none
   of 16 MiB or more (rarely asked for again at the same size: 249 MiB sat unused in the cache at the title screen). */
SRWLOCK g_large_mu = SRWLOCK_INIT;
struct Lock { Lock() { AcquireSRWLockExclusive(&g_large_mu); } ~Lock() { ReleaseSRWLockExclusive(&g_large_mu); } };
struct LargeFree { LargeFree *next; std::size_t size; };
LargeFree *g_large_head;
std::size_t g_large_bytes;
constexpr std::size_t kLargeKeep = 64u << 20, kLargeKeepMax = 16u << 20;
std::size_t large_round(std::size_t n) {
    std::size_t p = kLarge; while (p * 2 <= n) p *= 2;
    std::size_t q = p / 4, r = p; while (r < n) r += q;
    return r;
}

/* Blocks of 16 MiB and up alive, with the code that asked for them (the memory report, alloc_big_report). */
struct Big { void *p; std::size_t n; void *ra; };
Big g_big[64];
void big_note(void *p, std::size_t n, void *ra) {
    if (!p || n < (16u << 20)) return;
    Lock l;
    for (Big &b : g_big) if (!b.p) { b = {p, n, ra}; return; }
}
void big_gone(void *p) {
    Lock l;
    for (Big &b : g_big) if (b.p == p) { b.p = nullptr; return; }
}

void *place(unsigned char *raw, std::uint64_t cls, std::uint64_t size) {
    void *user = raw + kHeader;
    Header *h = reinterpret_cast<Header *>(raw); h->tag = tag_for(user, cls); h->size = size;
    return user;
}

void *alloc_or_null(std::size_t n) {
    if (n <= kClasses * kStep) {
        std::size_t c = n ? (n + kStep - 1) / kStep : 1;
        if (Cache *k = cache()) if (Free *f = k->head[c]) { k->head[c] = f->next; k->count[c]--; return f; }
        auto *raw = static_cast<unsigned char *>(__real_malloc(kHeader + c * kStep));
        return raw ? place(raw, c, c * kStep) : nullptr;
    }
    if (n >= kLarge && n <= (std::size_t(1) << 40)) {
        std::size_t r = large_round(n);
        {
            Lock l;
            for (LargeFree **pp = &g_large_head; *pp; pp = &(*pp)->next)
                if ((*pp)->size == r) {
                    LargeFree *f = *pp; *pp = f->next; g_large_bytes -= r;
                    return place(reinterpret_cast<unsigned char *>(f), kLargeClass, r);
                }
        }
        auto *raw = static_cast<unsigned char *>(__real_malloc(kHeader + r));
        return raw ? place(raw, kLargeClass, r) : nullptr;
    }
    auto *raw = static_cast<unsigned char *>(__real_malloc(kHeader + n));
    return raw ? place(raw, 0, n) : nullptr;
}
void release(void *q) noexcept {
    if (!q) return;
    long c = class_of(q);
    if (c == static_cast<long>(kLargeClass) && header_of(q)->size >= (16u << 20)) big_gone(q);
    if (c < 0) { __real_free(q); return; }   /* the runtime's own block */
    auto *raw = reinterpret_cast<unsigned char *>(header_of(q));
    if (c >= 1 && c <= static_cast<long>(kClasses)) {
        Cache *k = cache();
        if (k && k->count[c] < kKeep) { auto *f = static_cast<Free *>(q); f->next = k->head[c]; k->head[c] = f; k->count[c]++; return; }
    } else if (c == static_cast<long>(kLargeClass)) {
        std::size_t r = header_of(q)->size;
        Lock l;
        if (r < kLargeKeepMax && g_large_bytes + r <= kLargeKeep) {
            auto *f = reinterpret_cast<LargeFree *>(raw); f->size = r; f->next = g_large_head; g_large_head = f; g_large_bytes += r;
            return;
        }
    }
    header_of(q)->tag = 0;   /* a stale pointer freed twice is not taken for ours */
    __real_free(raw);
}
std::size_t usable(void *q) { return header_of(q)->size; }
}  // namespace

extern "C" {
void *__wrap_malloc(std::size_t n) { void *p = alloc_or_null(n); big_note(p, n, __builtin_return_address(0)); return p; }
void __wrap_free(void *p) { release(p); }
void *__wrap_calloc(std::size_t a, std::size_t b) {
    if (b && a > SIZE_MAX / b) return nullptr;
    void *p = alloc_or_null(a * b);
    big_note(p, a * b, __builtin_return_address(0));
    if (p) std::memset(p, 0, a * b);
    return p;
}
void *__wrap_realloc(void *p, std::size_t n) {
    if (!p) return alloc_or_null(n);
    if (class_of(p) < 0) return __real_realloc(p, n);   /* the runtime's own block */
    if (!n) { release(p); return nullptr; }
    std::size_t have = usable(p);
    if (n <= have && (n >= have / 2 || have <= kClasses * kStep)) return p;   /* fits, without wasting most of it */
    void *q = alloc_or_null(n);
    if (!q) return nullptr;
    big_note(q, n, __builtin_return_address(0));
    std::memcpy(q, p, have < n ? have : n);
    release(p);
    return q;
}
}

void *operator new(std::size_t n) { if (void *p = alloc_or_null(n)) { big_note(p, n, __builtin_return_address(0)); return p; } throw std::bad_alloc(); }
void *operator new[](std::size_t n) { if (void *p = alloc_or_null(n)) { big_note(p, n, __builtin_return_address(0)); return p; } throw std::bad_alloc(); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return alloc_or_null(n); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return alloc_or_null(n); }
void operator delete(void *p) noexcept { release(p); }
void operator delete[](void *p) noexcept { release(p); }
void operator delete(void *p, std::size_t) noexcept { release(p); }
void operator delete[](void *p, std::size_t) noexcept { release(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { release(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { release(p); }

/* The live blocks of 16 MiB and up: "size MiB from code address" (addresses relative to the exe's preferred base, for
   addr2line), and the bytes the large-block cache holds. */
extern "C" void alloc_big_report(char *out, std::size_t cap) {
    std::size_t len = 0;
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    Lock l;
    len += (std::size_t)std::snprintf(out, cap, "freed large blocks kept %.0f MiB; blocks of 16 MiB and up:", g_large_bytes / 1048576.0);
    for (const Big &b : g_big)
        if (b.p && len < cap)
            len += (std::size_t)std::snprintf(out + len, cap - len, " %.0f MiB from %#llx", b.n / 1048576.0,
                                              (unsigned long long)(reinterpret_cast<std::uintptr_t>(b.ra) - base + 0x140000000ull));
}

/* Self-check at start (main.c): blocks come back from the caches, sizes keep their class, foreign blocks pass through. */
extern "C" int alloc_self_check(void) {
    void *a = ::operator new(40); ::operator delete(a);
    void *b = ::operator new(33);   /* same 48-byte class: the block just freed */
    int ok = a == b;
    ::operator delete(b);
    void *big = __wrap_malloc(300000); std::memset(big, 1, 300000); __wrap_free(big);
    void *big2 = __wrap_malloc(290000); ok &= big2 == big; __wrap_free(big2);   /* same large size class: reused */
    char *s = static_cast<char *>(__wrap_malloc(8)); std::strcpy(s, "abc");
    s = static_cast<char *>(__wrap_realloc(s, 5000)); ok &= s && !std::strcmp(s, "abc"); __wrap_free(s);
    void *z = __wrap_calloc(10, 100); for (int i = 0; i < 1000; i++) ok &= !static_cast<unsigned char *>(z)[i]; __wrap_free(z);
    void *foreign = __real_malloc(64); ok &= class_of(foreign) < 0; __wrap_free(foreign);   /* given back to the runtime */
    int *arr = new int[3]{1, 2, 3}; ok &= arr[2] == 3; delete[] arr;
    return ok;
}
