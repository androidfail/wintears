#include "jit/jit.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

/* WT_NATIVE_ONLY (cmake -DWINTEARS_NATIVE_ONLY=ON): Direct Native without dynarmic. Every block must come from the
   recompiled DLLs; a block missing from them stops the game with its address (to add to native/misses.txt). */
#ifndef WT_NATIVE_ONLY
#define WT_DYNARMIC 1
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include "dynarmic/common/memory_pool.h"
#else
#define WT_DYNARMIC 0
#endif

#include <cstdlib>
#include <mutex>
#include <vector>

#include "pal.h"
#include "native/wt_native.h"

#include <algorithm>
#include <array>
#include <string>
#include <xmmintrin.h>
#include <unordered_map>

/* Direct Native, hybrid mode: blocks recompiled ahead of time (tools/recomp/wt_recomp, DLLs under <game>/native) run
   from a table; a block missing from it runs in dynarmic, one block at a time (cycle counting with a budget of one
   instruction makes Run return after a single block). A core's dynarmic is made at its first missed block, so a run
   with full coverage never allocates the JIT's code caches. */
namespace {
struct NativeTable {
    std::vector<uint64_t> keys;
    std::vector<wt_block_fn> fns;
    unsigned shift = 64;
    size_t n = 0;
} g_nat;
inline wt_block_fn nat_find(uint64_t pc) {
    if (!g_nat.n) return nullptr;
    size_t mask = g_nat.keys.size() - 1, i = (size_t)(((pc >> 2) * 0x9E3779B97F4A7C15ull) >> g_nat.shift);
    for (;; i = (i + 1) & mask) {
        uint64_t k = g_nat.keys[i];
        if (k == pc) return g_nat.fns[i];
        if (!k) return nullptr;
    }
}
std::atomic<uint64_t> g_pools{0}, g_slabs_new{0}, g_compile_ns{0};
}  // namespace

#if WT_DYNARMIC
using Dynarmic::A64::Vector;
using VAddr = Dynarmic::A64::VAddr;

/* Dynarmic's IR instruction pool, replaced at link time (this file always links before libdynarmic, so its
   memory_pool.cpp is never pulled in). Upstream mallocs a fresh 4096-instruction slab (416 KiB) for every block it
   compiles and frees it after; under Wine / Proton that size is decommitted on free and committed again on the next
   block, page faults included, and it showed up in the samples of every compiling core on the Steam Deck. Freed slabs
   are kept here and reused instead. */
namespace {
std::mutex g_slab_mu;
std::vector<std::pair<size_t, char *>> g_slab_spare;
thread_local uint64_t t_compile_t0;   /* one pool per compiled block, alive from translation to emitted code */
char *slab_take(size_t bytes) {
    {
        std::lock_guard<std::mutex> l(g_slab_mu);
        for (size_t i = g_slab_spare.size(); i-- > 0;)
            if (g_slab_spare[i].first == bytes) { char *p = g_slab_spare[i].second; g_slab_spare.erase(g_slab_spare.begin() + (std::ptrdiff_t)i); return p; }
    }
    g_slabs_new.fetch_add(1, std::memory_order_relaxed);
    char *p = static_cast<char *>(std::malloc(bytes));
    if (!p) std::abort();
    return p;
}
void slab_give(char *p, size_t bytes) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> l(g_slab_mu);
        if (g_slab_spare.size() < 32) { g_slab_spare.emplace_back(bytes, p); return; }
    }
    std::free(p);
}
}  // namespace

namespace Dynarmic::Common {
Pool::Pool(size_t object_size, size_t initial_pool_size) : object_size(object_size), slab_size(initial_pool_size) {
    g_pools.fetch_add(1, std::memory_order_relaxed);
    t_compile_t0 = pal_now_ns();
    AllocateNewSlab();
}
Pool::~Pool() {
    g_compile_ns.fetch_add(pal_now_ns() - t_compile_t0, std::memory_order_relaxed);
    slab_give(current_slab, object_size * slab_size);
    for (char *s : slabs) slab_give(s, object_size * slab_size);
}
void *Pool::Alloc() {
    if (remaining == 0) { slabs.push_back(current_slab); AllocateNewSlab(); }
    void *ret = current_ptr;
    current_ptr += object_size;
    remaining--;
    return ret;
}
void Pool::AllocateNewSlab() {
    current_slab = slab_take(object_size * slab_size);
    current_ptr = current_slab;
    remaining = slab_size;
}
}  // namespace Dynarmic::Common

struct jit_monitor {
    explicit jit_monitor(size_t n) : m(n) {}
    Dynarmic::ExclusiveMonitor m;
};

namespace {

constexpr Dynarmic::HaltReason kHaltSvc = Dynarmic::HaltReason::UserDefined1;
constexpr Dynarmic::HaltReason kHaltExternal = Dynarmic::HaltReason::UserDefined2;
constexpr Dynarmic::HaltReason kHaltException = Dynarmic::HaltReason::UserDefined3;

template <typename T>
T load(VAddr a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof v);   /* identity-mapped; unaligned is fine on x86 */
    return v;
}
template <typename T>
void store(VAddr a, T v) {
    std::memcpy(reinterpret_cast<void *>(a), &v, sizeof v);
}
template <typename T>
bool cas(VAddr a, T value, T expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<T *>(a), &expected, value, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

}  // namespace
#else
struct jit_monitor {};   /* recompiled exclusives keep their monitor in wt_ctx */
#endif

namespace {
uint64_t default_cntpct(void *) {
    /* 19.2 MHz = ns * 12 / 625; split to avoid overflow */
    uint64_t ns = pal_now_ns();
    return ns / 625 * 12 + ns % 625 * 12 / 625;
}
}  // namespace

struct jit_dyn;
struct jit_core {
    jit_config cfg{};
    jit_monitor *monitor = nullptr;
    size_t core_id = 0;
    uint64_t tpidr_el0 = 0, tpidrro_el0 = 0;
    uint32_t svc = 0;
    int exc_kind = 0;
    uint64_t exc_pc = 0;
    bool hybrid = false;         /* recompiled blocks loaded: the guest state lives in nc, dynarmic gets it per missed block */
    wt_ctx nc{};
    volatile int halt_req = 0;
    uint64_t n_native = 0, n_missed = 0, n_fpcr = 0, last_fpcr = 0;   /* blocks run (read racily for the log) */
    std::unordered_map<uint64_t, uint64_t> *misses = nullptr;   /* TOTK_NATIVE_MISSES: missed pc -> count */
    struct { uint64_t pc; wt_block_fn fn; } lc[8192] = {};   /* recent pc -> block (128 KiB) */
    std::atomic<jit_dyn *> dyn{nullptr};   /* made on first use (hybrid: the first missed block) */
    ~jit_core();
};

#if WT_DYNARMIC
struct jit_dyn final : Dynarmic::A64::UserCallbacks {
    jit_core *c;
    std::unique_ptr<Dynarmic::A64::Jit> jit;

    std::uint8_t MemoryRead8(VAddr a) override { return load<uint8_t>(a); }
    std::uint16_t MemoryRead16(VAddr a) override { return load<uint16_t>(a); }
    std::uint32_t MemoryRead32(VAddr a) override { return load<uint32_t>(a); }
    std::uint64_t MemoryRead64(VAddr a) override { return load<uint64_t>(a); }
    Vector MemoryRead128(VAddr a) override { return Vector{load<uint64_t>(a), load<uint64_t>(a + 8)}; }
    void MemoryWrite8(VAddr a, std::uint8_t v) override { store(a, v); }
    void MemoryWrite16(VAddr a, std::uint16_t v) override { store(a, v); }
    void MemoryWrite32(VAddr a, std::uint32_t v) override { store(a, v); }
    void MemoryWrite64(VAddr a, std::uint64_t v) override { store(a, v); }
    void MemoryWrite128(VAddr a, Vector v) override { store(a, v[0]); store(a + 8, v[1]); }

    bool MemoryWriteExclusive8(VAddr a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(VAddr a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(VAddr a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(VAddr a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive128(VAddr a, Vector v, Vector e) override {
        unsigned __int128 nv = (unsigned __int128)v[1] << 64 | v[0], ne = (unsigned __int128)e[1] << 64 | e[0];
        return __sync_bool_compare_and_swap(reinterpret_cast<unsigned __int128 *>(a), ne, nv);   /* cmpxchg16b */
    }

    void InterpreterFallback(VAddr pc, size_t) override { stop_exception(pc, JIT_EXC_UNDEFINED); }
    void CallSVC(std::uint32_t imm) override {
        c->svc = imm;
        jit->HaltExecution(kHaltSvc);
    }
    void ExceptionRaised(VAddr pc, Dynarmic::A64::Exception e) override {
        using E = Dynarmic::A64::Exception;
        switch (e) {
        case E::Yield: case E::SendEvent: case E::SendEventLocal: return;   /* hints: nothing to do */
        case E::WaitForInterrupt: case E::WaitForEvent: stop_exception(pc, JIT_EXC_WAIT); return;
        case E::Breakpoint: stop_exception(pc, JIT_EXC_BREAKPOINT); return;
        case E::NoExecuteFault: stop_exception(pc, JIT_EXC_NO_EXECUTE); return;
        case E::UnallocatedEncoding: case E::ReservedValue: case E::UnpredictableInstruction:
            stop_exception(pc, JIT_EXC_UNDEFINED); return;
        default: stop_exception(pc, JIT_EXC_OTHER); return;
        }
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return c->hybrid ? 1 : 0; }   /* hybrid: one block per Run */
    std::uint64_t GetCNTPCT() override { return c->cfg.cntpct(c->cfg.user); }

    void stop_exception(VAddr pc, int kind) {
        c->exc_pc = pc;
        c->exc_kind = kind;
        jit->HaltExecution(kHaltException);
    }
};

/* The core's dynarmic, made by the thread that runs the core (jit_halt from other threads only reads the pointer). */
static Dynarmic::A64::Jit &J(jit_core *c) {
    if (jit_dyn *d = c->dyn.load(std::memory_order_acquire)) return *d->jit;
    auto *d = new jit_dyn;
    d->c = c;
    Dynarmic::A64::UserConfig uc;
    uc.callbacks = d;
    uc.processor_id = c->core_id;
    uc.global_monitor = c->monitor ? &c->monitor->m : nullptr;
    uc.tpidr_el0 = &c->tpidr_el0;
    uc.tpidrro_el0 = &c->tpidrro_el0;
    uc.cntfrq_el0 = 19200000;          /* Switch counter frequency */
    uc.dczid_el0 = 4;                  /* DC ZVA zeroes 64 bytes, as on the Switch's Cortex-A57 */
    uc.ctr_el0 = 0x8444c004;
    uc.enable_cycle_counting = c->hybrid;  /* JIT only: runs until halted (preemption uses jit_halt) */
    /* Architecturally unpredictable forms run as the hardware does them instead of stopping: TOTK main+0x12497b8 has
       "stlxr wzr, xzr, [x8]" (status register = data register), executed as the store with the status discarded. */
    uc.define_unpredictable_behaviour = true;
    uc.code_cache_size = c->cfg.code_cache_bytes ? c->cfg.code_cache_bytes : 128u << 20;
    if (c->cfg.fast_nan) {
        uc.unsafe_optimizations = true;
        uc.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
    }
    if (c->cfg.fastmem) {
        uc.fastmem_pointer = uintptr_t{0};     /* identity mapping: host address == guest address */
        uc.fastmem_address_space_bits = 39;
        uc.silently_mirror_fastmem = false;
        uc.recompile_on_fastmem_failure = true;
        uc.fastmem_exclusive_access = true;
        uc.recompile_on_exclusive_fastmem_failure = true;
    }
    d->jit = std::make_unique<Dynarmic::A64::Jit>(uc);
    if (c->hybrid) fprintf(stderr, "[wintears] Direct Native: core %zu made its JIT for the block at %#llx\n", c->core_id,
                           (unsigned long long)c->nc.pc);
    c->dyn.store(d, std::memory_order_release);
    return *d->jit;
}
jit_core::~jit_core() { delete dyn.load(); delete misses; }
#else
jit_core::~jit_core() { delete misses; }
#endif

extern "C" {

jit_monitor *jit_monitor_create(size_t cores) {
#if WT_DYNARMIC
    return new jit_monitor(cores);
#else
    (void)cores; return new jit_monitor;
#endif
}
void jit_compile_stats(uint64_t *blocks, uint64_t *new_slabs) { *blocks = g_pools.load(); *new_slabs = g_slabs_new.load(); }
uint64_t jit_compile_ns(void) { return g_compile_ns.load(); }
void jit_monitor_destroy(jit_monitor *m) { delete m; }

jit_core *jit_core_create(const jit_config *cfg, jit_monitor *m, size_t core_id) {
    auto *c = new jit_core;
    if (cfg) c->cfg = *cfg;
    else c->cfg.fastmem = 1;
    if (!c->cfg.cntpct) c->cfg.cntpct = default_cntpct;
    c->monitor = m;
    c->core_id = core_id;
    c->hybrid = g_nat.n != 0;
    c->nc.halt_request = &c->halt_req;
    c->nc.lc = reinterpret_cast<const wt_lce *>(c->lc); c->nc.lc_mask = sizeof c->lc / sizeof c->lc[0] - 1; c->nc.disp_fpcr = ~0u;
    static_assert(sizeof c->lc[0] == sizeof(wt_lce), "the cache entry generated code reads");
    c->nc.cntpct = c->cfg.cntpct;
    c->nc.cntpct_user = c->cfg.user;
    if (c->hybrid && pal_getenv("TOTK_NATIVE_MISSES")) c->misses = new std::unordered_map<uint64_t, uint64_t>;
#if WT_DYNARMIC
    if (!c->hybrid) J(c);   /* JIT only: needed at once, and jit_halt must find it */
#endif
    return c;
}

void jit_core_destroy(jit_core *c) { delete c; }

#if WT_DYNARMIC
static void to_jit(jit_core *c) {   /* hybrid: the state of nc into dynarmic for one block */
    auto &j = J(c); const wt_ctx &n = c->nc;
    std::array<uint64_t, 31> r; memcpy(r.data(), n.x, sizeof r); j.SetRegisters(r);
    std::array<Vector, 32> v; for (size_t i = 0; i < 32; i++) v[i] = Vector{n.v[i].lo, n.v[i].hi}; j.SetVectors(v);
    j.SetSP(n.sp); j.SetPC(n.pc); j.SetFpcr(n.fpcr); j.SetFpsr(n.fpsr); j.SetPstate(n.nzcv);
    c->tpidr_el0 = n.tpidr; c->tpidrro_el0 = n.tpidrro;
}
static void from_jit(jit_core *c) {
    auto &j = J(c); wt_ctx &n = c->nc;
    auto r = j.GetRegisters(); memcpy(n.x, r.data(), sizeof n.x);
    auto v = j.GetVectors(); for (size_t i = 0; i < 32; i++) n.v[i] = wt_v128{v[i][0], v[i][1]};
    n.sp = j.GetSP(); n.pc = j.GetPC(); n.fpcr = j.GetFpcr(); n.fpsr = j.GetFpsr(); n.nzcv = j.GetPstate() & 0xF0000000u;
    n.tpidr = c->tpidr_el0;
}
#endif

void jit_load(jit_core *c, const jit_context *ctx) {
    if (c->hybrid) {
        wt_ctx &n = c->nc;
        memcpy(n.x, ctx->x, sizeof n.x); n.sp = ctx->sp; n.pc = ctx->pc;
        for (size_t i = 0; i < 32; i++) n.v[i] = wt_v128{ctx->v[i][0], ctx->v[i][1]};
        n.fpcr = ctx->fpcr; n.fpsr = ctx->fpsr; n.nzcv = ctx->pstate & 0xF0000000u;
        n.tpidr = ctx->tpidr_el0; n.tpidrro = ctx->tpidrro_el0; n.excl_valid = 0;
        c->tpidr_el0 = ctx->tpidr_el0; c->tpidrro_el0 = ctx->tpidrro_el0;
#if WT_DYNARMIC
        if (jit_dyn *d = c->dyn.load(std::memory_order_relaxed)) d->jit->ClearExclusiveState();
#endif
        return;
    }
#if WT_DYNARMIC
    auto &j = J(c);
    for (size_t i = 0; i < 31; i++) j.SetRegister(i, ctx->x[i]);
    j.SetSP(ctx->sp);
    j.SetPC(ctx->pc);
    for (size_t i = 0; i < 32; i++) j.SetVector(i, Vector{ctx->v[i][0], ctx->v[i][1]});
    j.SetFpcr(ctx->fpcr);
    j.SetFpsr(ctx->fpsr);
    j.SetPstate(ctx->pstate);
    c->tpidr_el0 = ctx->tpidr_el0;
    c->tpidrro_el0 = ctx->tpidrro_el0;
    j.ClearExclusiveState();
#else
    c->nc.pc = ctx->pc;   /* no recompiled blocks: jit_run reports the miss */
#endif
}

void jit_save(jit_core *c, jit_context *ctx) {
    if (c->hybrid) {
        const wt_ctx &n = c->nc;
        memcpy(ctx->x, n.x, sizeof n.x); ctx->sp = n.sp; ctx->pc = n.pc;
        for (size_t i = 0; i < 32; i++) { ctx->v[i][0] = n.v[i].lo; ctx->v[i][1] = n.v[i].hi; }
        ctx->fpcr = n.fpcr; ctx->fpsr = n.fpsr; ctx->pstate = n.nzcv;
        ctx->tpidr_el0 = n.tpidr; ctx->tpidrro_el0 = n.tpidrro;
        return;
    }
#if WT_DYNARMIC
    auto &j = J(c);
    for (size_t i = 0; i < 31; i++) ctx->x[i] = j.GetRegister(i);
    ctx->sp = j.GetSP();
    ctx->pc = j.GetPC();
    for (size_t i = 0; i < 32; i++) { Vector v = j.GetVector(i); ctx->v[i][0] = v[0]; ctx->v[i][1] = v[1]; }
    ctx->fpcr = j.GetFpcr();
    ctx->fpsr = j.GetFpsr();
    ctx->pstate = j.GetPstate();
    ctx->tpidr_el0 = c->tpidr_el0;
    ctx->tpidrro_el0 = c->tpidrro_el0;
#endif
}

#if WT_DYNARMIC
static int stop_of(jit_core *c, Dynarmic::HaltReason r) {
    using HR = Dynarmic::HaltReason;
    auto has = [&](HR x) { return (static_cast<uint32_t>(r) & static_cast<uint32_t>(x)) != 0; };
    int stop = has(kHaltException) ? JIT_STOP_EXCEPTION : has(kHaltSvc) ? JIT_STOP_SVC
             : has(kHaltExternal) ? JIT_STOP_HALT : JIT_STOP_STEP;
    auto &j = J(c);
    j.ClearHalt(kHaltSvc);
    j.ClearHalt(kHaltException);
    j.ClearHalt(kHaltExternal);
    return stop;
}
#endif

static int exc_kind_of(uint32_t e) {   /* dynarmic A64::Exception numbers (as wt_native.h raises them) to jit_exception_kind */
    switch (e) {
    case 0: case 1: case 2: return JIT_EXC_UNDEFINED;    /* UnallocatedEncoding, ReservedValue, UnpredictableInstruction */
    case 3: case 4: return JIT_EXC_WAIT;                 /* WaitForInterrupt, WaitForEvent */
    case 8: return JIT_EXC_BREAKPOINT;
    case 9: return JIT_EXC_NO_EXECUTE;
    default: return JIT_EXC_OTHER;
    }
}
#if WT_DYNARMIC
static_assert(static_cast<uint32_t>(Dynarmic::A64::Exception::UnallocatedEncoding) == 0 &&
              static_cast<uint32_t>(Dynarmic::A64::Exception::ReservedValue) == 1 &&
              static_cast<uint32_t>(Dynarmic::A64::Exception::UnpredictableInstruction) == 2 &&
              static_cast<uint32_t>(Dynarmic::A64::Exception::WaitForInterrupt) == 3 &&
              static_cast<uint32_t>(Dynarmic::A64::Exception::WaitForEvent) == 4 &&
              static_cast<uint32_t>(Dynarmic::A64::Exception::Breakpoint) == 8 &&
              static_cast<uint32_t>(Dynarmic::A64::Exception::NoExecuteFault) == 9,
              "exc_kind_of's numbers");
#endif

/* FPCR to MXCSR for the recompiled blocks: FZ (24) -> FTZ + DAZ; native-only builds also take the rounding mode
   (FPCR RMode 23:22 = nearest, +inf, -inf, zero; MXCSR RC 14:13 = nearest, -inf, +inf, zero). */
static unsigned mxcsr_of(unsigned host, uint32_t fpcr) {
    unsigned m = (host & ~0x8040u) | (fpcr & 0x01000000u ? 0x8040u : 0);
#if !WT_DYNARMIC
    static const unsigned rc[4] = {0, 2, 1, 3};
    m = (m & ~0x6000u) | rc[fpcr >> 22 & 3] << 13;
#endif
    return m;
}

/* per-core cache markers for the host's stubs (never real block addresses) */
static const wt_block_fn kStubSvc = reinterpret_cast<wt_block_fn>(uintptr_t{1}), kStubRet = reinterpret_cast<wt_block_fn>(uintptr_t{2});
int jit_run(jit_core *c) {
#if WT_DYNARMIC
    if (!c->hybrid) return stop_of(c, J(c).Run());
    const auto stops = kHaltSvc | kHaltException | kHaltExternal;
#endif
    wt_ctx &n = c->nc;
    /* Recompiled blocks are the same for every FPCR whose only bits are FZ (24) and DN (25): flush-to-zero is
       MXCSR FTZ + DAZ while they run (TOTK's threads run with FZ), NaNs are not bit-exact anyway. Other bits
       (rounding modes, trap enables) go to dynarmic, or with no dynarmic become the MXCSR rounding mode. The host's
       MXCSR is restored before returning. */
    const unsigned host_mxcsr = _mm_getcsr();
    unsigned mx_fpcr = ~0u;
    int stop;
    for (;;) {
        if (c->halt_req) {
            c->halt_req = 0; c->nc.halt_now = 0;
#if WT_DYNARMIC
            if (jit_dyn *d = c->dyn.load(std::memory_order_relaxed)) d->jit->ClearHalt(kHaltExternal);
#endif
            stop = JIT_STOP_HALT; break;
        }
        bool ok_fpcr = !WT_DYNARMIC || (n.fpcr & ~0x03000000u) == 0;
        wt_block_fn f = nullptr;
        if (ok_fpcr) {   /* a small per-core cache in front of the big table (megabytes: a cache miss each time) */
            auto &e = c->lc[(n.pc >> 2) & (sizeof c->lc / sizeof c->lc[0] - 1)];
            if (e.pc == n.pc) f = e.fn;
            else if ((f = nat_find(n.pc))) { e.pc = n.pc; e.fn = f; }
            else {   /* the host's stubs ("svc #n; ret", outside the modules) are remembered too: every host call went
                        through two missed lookups in the big table */
                uint32_t w; memcpy(&w, reinterpret_cast<const void *>(n.pc), 4);
                if ((w & 0xFFE0001Fu) == 0xD4000001u) { e.pc = n.pc; e.fn = kStubSvc; f = kStubSvc; }
                else if (w == 0xD65F03C0u) { e.pc = n.pc; e.fn = kStubRet; f = kStubRet; }
            }
            if (f == kStubSvc) { uint32_t w; memcpy(&w, reinterpret_cast<const void *>(n.pc), 4); c->svc = w >> 5 & 0xFFFF; n.pc += 4; stop = JIT_STOP_SVC; break; }
            if (f == kStubRet) { n.pc = n.x[30]; continue; }
        }
        if (!ok_fpcr || (n.fpcr & ~0x03000000u)) { c->n_fpcr++; c->last_fpcr = n.fpcr; }   /* counted: FPCR other than FZ / DN */
        if (f) {
            c->n_native++;
            if (n.fpcr != mx_fpcr) { mx_fpcr = n.fpcr; _mm_setcsr(mxcsr_of(host_mxcsr, n.fpcr)); }
            n.disp_fpcr = mx_fpcr;   /* blocks may dispatch on by themselves while FPCR stays this (WT_DISPATCH) */
            n.halt = WT_RUN;
            f(&n);
            if (n.halt == WT_RUN) continue;
            if (n.halt == WT_HALT_SVC) { c->svc = n.svc; stop = JIT_STOP_SVC; }
            else if (n.halt == WT_HALT_EXCEPTION) { c->exc_pc = n.exc_pc; c->exc_kind = exc_kind_of(n.exc_kind); stop = JIT_STOP_EXCEPTION; }
            else { c->exc_pc = n.pc; c->exc_kind = JIT_EXC_UNDEFINED; stop = JIT_STOP_EXCEPTION; }   /* Interpret terminal */
            break;
        }
        {   /* the host's stubs in guest memory (bridge entries "svc #n; ret", the return trap): no need for dynarmic */
            uint32_t w; memcpy(&w, reinterpret_cast<const void *>(n.pc), 4);
            if ((w & 0xFFE0001Fu) == 0xD4000001u) { c->svc = w >> 5 & 0xFFFF; n.pc += 4; stop = JIT_STOP_SVC; break; }
            if (w == 0xD65F03C0u) { n.pc = n.x[30]; continue; }
        }
        c->n_missed++;
        if (c->misses) (*c->misses)[n.pc]++;
#if WT_DYNARMIC
        to_jit(c);
        Dynarmic::HaltReason r = J(c).Run();   /* dynarmic sets its own MXCSR and puts back the one it found */
        from_jit(c);
        if ((r & stops) != Dynarmic::HaltReason{}) { stop = stop_of(c, r); if (stop == JIT_STOP_HALT) { c->halt_req = 0; c->nc.halt_now = 0; } break; }
#else
        fprintf(stderr, "[wintears] Direct Native (no JIT): no recompiled block at %#llx%s. Add the line \"%llx 1\" to "
                "native/misses.txt and run tools/recomp/build_native.py again.\n", (unsigned long long)n.pc,
                g_nat.n ? "" : " (no recompiled-code DLLs loaded)", (unsigned long long)n.pc);
        c->exc_pc = n.pc; c->exc_kind = JIT_EXC_NO_EXECUTE; stop = JIT_STOP_EXCEPTION;
        break;
#endif
    }
    _mm_setcsr(host_mxcsr);
    n.disp_fpcr = ~0u;   /* MXCSR is the host's again */
    return stop;
}
int jit_step(jit_core *c) {
#if WT_DYNARMIC
    if (!c->hybrid) return stop_of(c, J(c).Step());
    to_jit(c); int s = stop_of(c, J(c).Step()); from_jit(c); return s;
#else
    c->exc_pc = c->nc.pc; c->exc_kind = JIT_EXC_OTHER;   /* single steps need the JIT */
    return JIT_STOP_EXCEPTION;
#endif
}
void jit_halt(jit_core *c) {
    c->halt_req = 1; c->nc.halt_now = 1;
#if WT_DYNARMIC
    if (jit_dyn *d = c->dyn.load(std::memory_order_acquire)) d->jit->HaltExecution(kHaltExternal);
#endif
}
void jit_native_misses(jit_core *const *cores, size_t n, const char *path) {
    std::unordered_map<uint64_t, uint64_t> all;   /* read racily: a diagnostic */
    for (size_t i = 0; i < n; i++) if (cores[i]->misses) for (auto &[pc, k] : *cores[i]->misses) all[pc] += k;
    if (all.empty()) return;
    std::vector<std::pair<uint64_t, uint64_t>> v(all.begin(), all.end());
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (auto &[pc, k] : v) fprintf(f, "%llx %llu\n", (unsigned long long)pc, (unsigned long long)k);
    fclose(f);
}
void jit_native_stats(jit_core *c, uint64_t *native, uint64_t *missed, uint64_t *fpcr_other, uint64_t *last_fpcr) {
    *native = c->n_native; *missed = c->n_missed; *fpcr_other = c->n_fpcr; *last_fpcr = c->last_fpcr;
}

size_t jit_native_load(const char *dir, const uint64_t bases[3], char *err, size_t cap) {
    std::vector<wt_entry> all;
    err[0] = 0;
    for (const char *name : {"totk_main.dll", "totk_subsdk0.dll", "totk_sdk.dll"}) {
        std::string path = std::string(dir) + "/" + name;
        void *lib = pal_lib_open(path.c_str());
        if (!lib) continue;
        auto *cnt = static_cast<const unsigned long long *>(pal_lib_symbol(lib, "wt_count"));
        auto *tab = static_cast<const wt_entry *>(pal_lib_symbol(lib, "wt_table"));
        auto *b = static_cast<const unsigned long long *>(pal_lib_symbol(lib, "wt_bases"));
        if (!cnt || !tab || !b) { snprintf(err, cap, "%s: not a recompiled-code DLL", name); continue; }
        if (b[0] != bases[0] || b[1] != bases[1] || b[2] != bases[2]) {
            snprintf(err, cap, "%s was made for modules at %#llx/%#llx/%#llx, not where they are now: left out", name, b[0], b[1], b[2]);
            continue;
        }
        all.insert(all.end(), tab, tab + *cnt);
    }
    if (all.empty()) return 0;
    size_t cap2 = 1; unsigned bits = 0;
    while (cap2 < all.size() * 2) { cap2 <<= 1; bits++; }
    g_nat.keys.assign(cap2, 0); g_nat.fns.assign(cap2, nullptr); g_nat.shift = 64 - bits;
    for (const wt_entry &e : all) {
        size_t i = (size_t)(((e.pc >> 2) * 0x9E3779B97F4A7C15ull) >> g_nat.shift);
        while (g_nat.keys[i] && g_nat.keys[i] != e.pc) i = (i + 1) & (cap2 - 1);
        g_nat.keys[i] = e.pc; g_nat.fns[i] = e.fn;
    }
    g_nat.n = all.size();
    return g_nat.n;
}
uint32_t jit_svc_number(jit_core *c) { return c->svc; }
int jit_exception(jit_core *c, uint64_t *pc) {
    if (pc) *pc = c->exc_pc;
    return c->exc_kind;
}
void jit_invalidate(jit_core *c, uint64_t addr, size_t size) {
#if WT_DYNARMIC
    if (jit_dyn *d = c->dyn.load(std::memory_order_relaxed)) d->jit->InvalidateCacheRange(addr, size);
#endif
    for (auto &e : c->lc) if (e.pc >= addr && e.pc - addr < size) e = {};   /* stubs and blocks remembered there */
}
void jit_clear_cache(jit_core *c) {
#if WT_DYNARMIC
    if (jit_dyn *d = c->dyn.load(std::memory_order_relaxed)) d->jit->ClearCache();
#else
    (void)c;
#endif
}

}  // extern "C"
