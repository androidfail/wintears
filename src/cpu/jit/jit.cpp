#include "jit/jit.h"

#include <atomic>
#include <cstring>
#include <memory>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include "dynarmic/common/memory_pool.h"

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

using Dynarmic::A64::Vector;

/* Direct Native, hybrid mode: blocks recompiled ahead of time (tools/recomp/wt_recomp, DLLs under <game>/native) run
   from a table; a block missing from it runs in dynarmic, one block at a time (cycle counting with a budget of one
   instruction makes Run return after a single block). */
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
}  // namespace
using VAddr = Dynarmic::A64::VAddr;

/* Dynarmic's IR instruction pool, replaced at link time (this file always links before libdynarmic, so its
   memory_pool.cpp is never pulled in). Upstream mallocs a fresh 4096-instruction slab (416 KiB) for every block it
   compiles and frees it after; under Wine / Proton that size is decommitted on free and committed again on the next
   block, page faults included, and it showed up in the samples of every compiling core on the Steam Deck. Freed slabs
   are kept here and reused instead. */
namespace {
std::mutex g_slab_mu;
std::vector<std::pair<size_t, char *>> g_slab_spare;
std::atomic<uint64_t> g_pools{0}, g_slabs_new{0}, g_compile_ns{0};
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

uint64_t default_cntpct(void *) {
    /* 19.2 MHz = ns * 12 / 625; split to avoid overflow */
    uint64_t ns = pal_now_ns();
    return ns / 625 * 12 + ns % 625 * 12 / 625;
}

}  // namespace

struct jit_core final : Dynarmic::A64::UserCallbacks {
    jit_config cfg{};
    jit_monitor *monitor = nullptr;
    uint64_t tpidr_el0 = 0, tpidrro_el0 = 0;
    uint32_t svc = 0;
    int exc_kind = 0;
    uint64_t exc_pc = 0;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    bool hybrid = false;         /* recompiled blocks loaded: the guest state lives in nc, dynarmic gets it per missed block */
    wt_ctx nc{};
    volatile int halt_req = 0;
    uint64_t n_native = 0, n_missed = 0, n_fpcr = 0, last_fpcr = 0;   /* blocks run (read racily for the log) */
    std::unordered_map<uint64_t, uint64_t> *misses = nullptr;   /* TOTK_NATIVE_MISSES: missed pc -> count */

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
        svc = imm;
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
    std::uint64_t GetTicksRemaining() override { return hybrid ? 1 : 0; }   /* hybrid: one block per Run */
    std::uint64_t GetCNTPCT() override { return cfg.cntpct(cfg.user); }

    void stop_exception(VAddr pc, int kind) {
        exc_pc = pc;
        exc_kind = kind;
        jit->HaltExecution(kHaltException);
    }
};

extern "C" {

jit_monitor *jit_monitor_create(size_t cores) { return new jit_monitor(cores); }
void jit_compile_stats(uint64_t *blocks, uint64_t *new_slabs) { *blocks = g_pools.load(); *new_slabs = g_slabs_new.load(); }
uint64_t jit_compile_ns(void) { return g_compile_ns.load(); }
void jit_monitor_destroy(jit_monitor *m) { delete m; }

jit_core *jit_core_create(const jit_config *cfg, jit_monitor *m, size_t core_id) {
    auto *c = new jit_core;
    if (cfg) c->cfg = *cfg;
    if (!c->cfg.cntpct) c->cfg.cntpct = default_cntpct;
    c->monitor = m;

    Dynarmic::A64::UserConfig uc;
    uc.callbacks = c;
    uc.processor_id = core_id;
    uc.global_monitor = m ? &m->m : nullptr;
    uc.tpidr_el0 = &c->tpidr_el0;
    uc.tpidrro_el0 = &c->tpidrro_el0;
    uc.cntfrq_el0 = 19200000;          /* Switch counter frequency */
    uc.dczid_el0 = 4;                  /* DC ZVA zeroes 64 bytes, as on the Switch's Cortex-A57 */
    uc.ctr_el0 = 0x8444c004;
    c->hybrid = g_nat.n != 0;
    uc.enable_cycle_counting = c->hybrid;  /* JIT only: runs until halted (preemption uses jit_halt) */
    c->nc.halt_request = &c->halt_req;
    c->nc.cntpct = c->cfg.cntpct;
    c->nc.cntpct_user = c->cfg.user;
    if (c->hybrid && pal_getenv("TOTK_NATIVE_MISSES")) c->misses = new std::unordered_map<uint64_t, uint64_t>;
    /* Architecturally unpredictable forms run as the hardware does them instead of stopping: TOTK main+0x12497b8 has
       "stlxr wzr, xzr, [x8]" (status register = data register), executed as the store with the status discarded. */
    uc.define_unpredictable_behaviour = true;
    uc.code_cache_size = c->cfg.code_cache_bytes ? c->cfg.code_cache_bytes : 128u << 20;
    if (c->cfg.fast_nan) {
        uc.unsafe_optimizations = true;
        uc.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
    }
    if (!cfg || cfg->fastmem) {
        uc.fastmem_pointer = uintptr_t{0};     /* identity mapping: host address == guest address */
        uc.fastmem_address_space_bits = 39;
        uc.silently_mirror_fastmem = false;
        uc.recompile_on_fastmem_failure = true;
        uc.fastmem_exclusive_access = true;
        uc.recompile_on_exclusive_fastmem_failure = true;
    }
    c->jit = std::make_unique<Dynarmic::A64::Jit>(uc);
    return c;
}

void jit_core_destroy(jit_core *c) { delete c; }

static void to_jit(jit_core *c) {   /* hybrid: the state of nc into dynarmic for one block */
    auto &j = *c->jit; const wt_ctx &n = c->nc;
    std::array<uint64_t, 31> r; memcpy(r.data(), n.x, sizeof r); j.SetRegisters(r);
    std::array<Vector, 32> v; for (size_t i = 0; i < 32; i++) v[i] = Vector{n.v[i].lo, n.v[i].hi}; j.SetVectors(v);
    j.SetSP(n.sp); j.SetPC(n.pc); j.SetFpcr(n.fpcr); j.SetFpsr(n.fpsr); j.SetPstate(n.nzcv);
    c->tpidr_el0 = n.tpidr; c->tpidrro_el0 = n.tpidrro;
}
static void from_jit(jit_core *c) {
    auto &j = *c->jit; wt_ctx &n = c->nc;
    auto r = j.GetRegisters(); memcpy(n.x, r.data(), sizeof n.x);
    auto v = j.GetVectors(); for (size_t i = 0; i < 32; i++) n.v[i] = wt_v128{v[i][0], v[i][1]};
    n.sp = j.GetSP(); n.pc = j.GetPC(); n.fpcr = j.GetFpcr(); n.fpsr = j.GetFpsr(); n.nzcv = j.GetPstate() & 0xF0000000u;
    n.tpidr = c->tpidr_el0;
}

void jit_load(jit_core *c, const jit_context *ctx) {
    if (c->hybrid) {
        wt_ctx &n = c->nc;
        memcpy(n.x, ctx->x, sizeof n.x); n.sp = ctx->sp; n.pc = ctx->pc;
        for (size_t i = 0; i < 32; i++) n.v[i] = wt_v128{ctx->v[i][0], ctx->v[i][1]};
        n.fpcr = ctx->fpcr; n.fpsr = ctx->fpsr; n.nzcv = ctx->pstate & 0xF0000000u;
        n.tpidr = ctx->tpidr_el0; n.tpidrro = ctx->tpidrro_el0; n.excl_valid = 0;
        c->tpidr_el0 = ctx->tpidr_el0; c->tpidrro_el0 = ctx->tpidrro_el0;
        c->jit->ClearExclusiveState();
        return;
    }
    auto &j = *c->jit;
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
    auto &j = *c->jit;
    for (size_t i = 0; i < 31; i++) ctx->x[i] = j.GetRegister(i);
    ctx->sp = j.GetSP();
    ctx->pc = j.GetPC();
    for (size_t i = 0; i < 32; i++) { Vector v = j.GetVector(i); ctx->v[i][0] = v[0]; ctx->v[i][1] = v[1]; }
    ctx->fpcr = j.GetFpcr();
    ctx->fpsr = j.GetFpsr();
    ctx->pstate = j.GetPstate();
    ctx->tpidr_el0 = c->tpidr_el0;
    ctx->tpidrro_el0 = c->tpidrro_el0;
}

static int stop_of(jit_core *c, Dynarmic::HaltReason r) {
    using HR = Dynarmic::HaltReason;
    auto has = [&](HR x) { return (static_cast<uint32_t>(r) & static_cast<uint32_t>(x)) != 0; };
    int stop = has(kHaltException) ? JIT_STOP_EXCEPTION : has(kHaltSvc) ? JIT_STOP_SVC
             : has(kHaltExternal) ? JIT_STOP_HALT : JIT_STOP_STEP;
    c->jit->ClearHalt(kHaltSvc);
    c->jit->ClearHalt(kHaltException);
    c->jit->ClearHalt(kHaltExternal);
    return stop;
}

static int exc_kind_of(uint32_t e) {   /* dynarmic A64::Exception to jit_exception_kind, as ExceptionRaised */
    using E = Dynarmic::A64::Exception;
    switch (static_cast<E>(e)) {
    case E::WaitForInterrupt: case E::WaitForEvent: return JIT_EXC_WAIT;
    case E::Breakpoint: return JIT_EXC_BREAKPOINT;
    case E::NoExecuteFault: return JIT_EXC_NO_EXECUTE;
    case E::UnallocatedEncoding: case E::ReservedValue: case E::UnpredictableInstruction: return JIT_EXC_UNDEFINED;
    default: return JIT_EXC_OTHER;
    }
}

int jit_run(jit_core *c) {
    if (!c->hybrid) return stop_of(c, c->jit->Run());
    wt_ctx &n = c->nc;
    const auto stops = kHaltSvc | kHaltException | kHaltExternal;
    /* Recompiled blocks are the same for every FPCR whose only bits are FZ (24) and DN (25): flush-to-zero is
       MXCSR FTZ + DAZ while they run (TOTK's threads run with FZ), NaNs are not bit-exact anyway. Other bits
       (rounding modes, trap enables) go to dynarmic. The host's MXCSR is restored before returning. */
    const unsigned host_mxcsr = _mm_getcsr();
    unsigned mx_fpcr = ~0u;
    int stop;
    for (;;) {
        if (c->halt_req) { c->halt_req = 0; c->jit->ClearHalt(kHaltExternal); stop = JIT_STOP_HALT; break; }
        bool ok_fpcr = (n.fpcr & ~0x03000000u) == 0;
        wt_block_fn f = ok_fpcr ? nat_find(n.pc) : nullptr;
        if (!ok_fpcr) { c->n_fpcr++; c->last_fpcr = n.fpcr; }   /* counted: FPCR the blocks cannot take */
        if (f) {
            c->n_native++;
            if (n.fpcr != mx_fpcr) { mx_fpcr = n.fpcr; _mm_setcsr((host_mxcsr & ~0x8040u) | (n.fpcr & 0x01000000u ? 0x8040u : 0)); }
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
        to_jit(c);
        Dynarmic::HaltReason r = c->jit->Run();   /* dynarmic sets its own MXCSR and puts back the one it found */
        from_jit(c);
        if ((r & stops) != Dynarmic::HaltReason{}) { stop = stop_of(c, r); if (stop == JIT_STOP_HALT) c->halt_req = 0; break; }
    }
    _mm_setcsr(host_mxcsr);
    return stop;
}
int jit_step(jit_core *c) {
    if (!c->hybrid) return stop_of(c, c->jit->Step());
    to_jit(c); int s = stop_of(c, c->jit->Step()); from_jit(c); return s;
}
void jit_halt(jit_core *c) { c->halt_req = 1; c->jit->HaltExecution(kHaltExternal); }
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
void jit_invalidate(jit_core *c, uint64_t addr, size_t size) { c->jit->InvalidateCacheRange(addr, size); }
void jit_clear_cache(jit_core *c) { c->jit->ClearCache(); }

}  // extern "C"
