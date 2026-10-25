/* operator new / delete with a per-thread cache of small blocks.

   On the Steam Deck (Wine 11) the cores spent about 40 % of their time inside ntdll's heap, called from dynarmic's
   compiler (IR nodes, xbyak labels, std::function, hash maps: many small allocations per compiled block, and the game
   compiles tens of thousands of blocks per 10 s while loading) with the threads asleep on the heap's lock. Wine refuses
   the low-fragmentation heap. Here a thread reuses the small blocks it freed without any lock; the heap is only
   reached when a thread's cache for a size is empty or full.

   Blocks up to 1 KiB come in 16-byte size classes behind a 16-byte header holding the class (0 = a large block, from
   malloc directly). A block freed by another thread joins that thread's cache. Linked statically with libstdc++
   (CMakeLists.txt), so every C++ allocation, the library's own included, comes here. */
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {
constexpr std::size_t kHeader = 16, kStep = 16, kClasses = 64, kKeep = 256;   /* classes 16..1024 bytes; at most 8.5 MiB cached per thread */
struct Free { Free *next; };
struct Cache { Free *head[kClasses + 1]; std::uint32_t count[kClasses + 1]; };
thread_local Cache t_cache;   /* zero-initialised, no destructor: a thread's cached blocks stay allocated when it exits */

void *alloc(std::size_t n) {
    std::size_t c = n <= kClasses * kStep ? (n + kStep - 1) / kStep : 0;
    if (c == 0 && n <= kClasses * kStep) c = 1;   /* new of 0 bytes */
    if (c) {
        Cache &k = t_cache;
        if (Free *f = k.head[c]) { k.head[c] = f->next; k.count[c]--; return f; }
    }
    auto *p = static_cast<unsigned char *>(std::malloc(kHeader + (c ? c * kStep : n)));
    if (!p) throw std::bad_alloc();
    *reinterpret_cast<std::size_t *>(p) = c;
    return p + kHeader;
}
void release(void *q) noexcept {
    if (!q) return;
    auto *p = static_cast<unsigned char *>(q) - kHeader;
    std::size_t c = *reinterpret_cast<std::size_t *>(p);
    if (c && c <= kClasses) {
        Cache &k = t_cache;
        if (k.count[c] < kKeep) { auto *f = static_cast<Free *>(q); f->next = k.head[c]; k.head[c] = f; k.count[c]++; return; }
    }
    std::free(p);
}
}  // namespace

void *operator new(std::size_t n) { return alloc(n); }
void *operator new[](std::size_t n) { return alloc(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { try { return alloc(n); } catch (...) { return nullptr; } }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { try { return alloc(n); } catch (...) { return nullptr; } }
void operator delete(void *p) noexcept { release(p); }
void operator delete[](void *p) noexcept { release(p); }
void operator delete(void *p, std::size_t) noexcept { release(p); }
void operator delete[](void *p, std::size_t) noexcept { release(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { release(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { release(p); }

/* Self-check at start (main.c): a block comes back from the cache, sizes keep their class. */
extern "C" int alloc_self_check(void) {
    void *a = ::operator new(40); ::operator delete(a);
    void *b = ::operator new(33);   /* same 48-byte class: the block just freed */
    int ok = a == b;
    void *big = ::operator new(5000); ::operator delete(big);
    ::operator delete(b);
    int *arr = new int[3]{1, 2, 3}; ok &= arr[2] == 3; delete[] arr;
    return ok;
}
