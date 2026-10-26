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
    std::uint64_t GetTicksRemaining() override { return 0; }
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
    uc.enable_cycle_counting = false;  /* runs until halted: preemption uses jit_halt */
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

void jit_load(jit_core *c, const jit_context *ctx) {
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

int jit_run(jit_core *c) { return stop_of(c, c->jit->Run()); }
int jit_step(jit_core *c) { return stop_of(c, c->jit->Step()); }
void jit_halt(jit_core *c) { c->jit->HaltExecution(kHaltExternal); }
uint32_t jit_svc_number(jit_core *c) { return c->svc; }
int jit_exception(jit_core *c, uint64_t *pc) {
    if (pc) *pc = c->exc_pc;
    return c->exc_kind;
}
void jit_invalidate(jit_core *c, uint64_t addr, size_t size) { c->jit->InvalidateCacheRange(addr, size); }
void jit_clear_cache(jit_core *c) { c->jit->ClearCache(); }

}  // extern "C"
