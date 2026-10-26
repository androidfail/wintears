/* ARM64 guest code on x86-64, through dynarmic. C interface for the Horizon host.

   Model (matches MNG's scheduler): there is one jit_core per virtual core. A guest thread that owns virtual core i
   loads its registers into core i, runs it on its own host thread, and saves them when it stops. A core is used by
   one host thread at a time. `svc` stops the run (so the host can block or give the core away outside the JIT),
   and the guest continues at the next instruction on the next jit_run.

   Guest memory is identity-mapped: guest address == host address (all below 2^39), so host code can use guest
   pointers directly. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t x[31];
    uint64_t sp, pc;
    uint64_t v[32][2];          /* q0..q31, low half first */
    uint32_t fpcr, fpsr, pstate; /* pstate: NZCV in bits 31..28 */
    uint32_t pad;
    uint64_t tpidr_el0, tpidrro_el0;
} jit_context;

enum jit_stop {
    JIT_STOP_SVC = 1,        /* svc #imm: jit_svc_number() has imm; pc is already past the svc */
    JIT_STOP_HALT = 2,       /* jit_halt() from another thread (preemption) */
    JIT_STOP_EXCEPTION = 3,  /* undefined instruction, brk, wfi/wfe: jit_exception() has the details */
    JIT_STOP_STEP = 4,       /* jit_step() finished one instruction */
};

enum jit_exception_kind {
    JIT_EXC_UNDEFINED = 1, JIT_EXC_BREAKPOINT = 2, JIT_EXC_WAIT = 3, JIT_EXC_NO_EXECUTE = 4, JIT_EXC_OTHER = 5,
};

typedef struct jit_monitor jit_monitor;   /* the global exclusive monitor for LDXR/STXR, shared by all cores */
jit_monitor *jit_monitor_create(size_t cores);
/* Blocks compiled (all cores) and instruction slabs allocated rather than reused, since the start. */
void jit_compile_stats(uint64_t *blocks, uint64_t *new_slabs);
uint64_t jit_compile_ns(void);   /* time spent compiling blocks (all cores), since the start */
void jit_monitor_destroy(jit_monitor *m);

typedef struct jit_core jit_core;
typedef struct {
    /* Guest counter frequency is 19.2 MHz. NULL = derived from the host's monotonic clock. */
    uint64_t (*cntpct)(void *user);
    void *user;
    size_t code_cache_bytes;    /* 0 = 128 MiB */
    int fastmem;                /* 1 = guest loads/stores compile to direct host accesses (default on) */
    int fast_nan;               /* 1 = NaN results not made bit-exact (dynarmic Unsafe_InaccurateNaN): fewer checks
                                   after each floating-point instruction; only NaN payloads differ */
} jit_config;

/* Direct Native hybrid mode: load recompiled blocks (dir/totk_{main,subsdk0,sdk}.dll from tools/recomp/wt_recomp)
   made for main, subsdk0 and sdk at bases[0..2]. Returns the blocks loaded (0: JIT only; err says why when a DLL was
   refused). Call before jit_core_create: cores made afterwards run those blocks and give dynarmic the rest. */
size_t jit_native_load(const char *dir, const uint64_t bases[3], char *err, size_t cap);
/* Hybrid mode counts since the start: blocks run recompiled, blocks given to dynarmic, blocks seen with FPCR != 0
   (recompiled blocks assume 0) and the last such FPCR. */
void jit_native_stats(jit_core *c, uint64_t *native, uint64_t *missed, uint64_t *fpcr_other, uint64_t *last_fpcr);
jit_core *jit_core_create(const jit_config *cfg, jit_monitor *m, size_t core_id);
void jit_core_destroy(jit_core *c);

void jit_load(jit_core *c, const jit_context *ctx);
void jit_save(jit_core *c, jit_context *ctx);

int jit_run(jit_core *c);       /* returns a jit_stop */
int jit_step(jit_core *c);
void jit_halt(jit_core *c);     /* safe from any thread; the core stops at the next block boundary */
uint32_t jit_svc_number(jit_core *c);
int jit_exception(jit_core *c, uint64_t *pc);

/* After guest code changes (module loading, game patches), drop translations of [addr, addr + size). */
void jit_invalidate(jit_core *c, uint64_t addr, size_t size);
void jit_clear_cache(jit_core *c);

#ifdef __cplusplus
}
#endif
