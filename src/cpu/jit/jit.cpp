#include "jit/jit.h"

#include <atomic>
#include <cstring>
#include <memory>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "pal.h"

using Dynarmic::A64::Vector;
using VAddr = Dynarmic::A64::VAddr;

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
    uc.code_cache_size = c->cfg.code_cache_bytes ? c->cfg.code_cache_bytes : 128u << 20;
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
