/* Direct Native: the interface between recompiled guest code (C made by tools/recomp/wt_recomp from the game's own
   NSOs, on the player's machine) and the runtime. Each guest block becomes `void B_<pc>(wt_ctx *c)`: it runs the
   block with the guest state in *c and leaves the next guest pc in c->pc. Semantics follow dynarmic's IR, block for
   block, so recompiled and JIT blocks can be mixed (hybrid mode). */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } wt_v128;

enum { WT_RUN = 0, WT_HALT_SVC = 1, WT_HALT_EXCEPTION = 2, WT_HALT_UNSUPPORTED = 3 };

typedef struct wt_ctx {
    uint64_t x[31], sp, pc;
    wt_v128 v[32];
    uint32_t nzcv;               /* ARM layout: N 31, Z 30, C 29, V 28 */
    uint32_t fpcr, fpsr;
    uint32_t halt;               /* WT_HALT_*: why the block returned to the runtime instead of continuing */
    uint32_t svc;                /* svc immediate (WT_HALT_SVC) */
    uint32_t exc_kind;           /* dynarmic A64::Exception (WT_HALT_EXCEPTION) */
    uint64_t exc_pc;
    uint64_t tpidr, tpidrro;
    uint64_t excl_addr;          /* exclusive monitor (LDXR / STXR): address and value read, valid flag */
    wt_v128 excl_val;
    uint32_t excl_valid;
    uint32_t check_bit;
    volatile int *halt_request;  /* set by another thread (preemption): checked where dynarmic checks for a halt */
    uint64_t (*cntpct)(void *);  /* guest counter, 19.2 MHz */
    void *cntpct_user;
    /* appended (older DLLs never read them): the runtime's per-core cache of recent blocks, for WT_DISPATCH, and the
       FPCR the runtime set MXCSR for (a block whose FPCR differs goes back to the runtime) */
    const struct wt_lce *lc;
    uint32_t disp_fpcr, lc_mask;
} wt_ctx;

typedef void (*wt_block_fn)(wt_ctx *);
typedef struct wt_lce { uint64_t pc; wt_block_fn fn; } wt_lce;   /* the runtime's cache entry (jit.cpp: fn 1 and 2 mark host stubs) */
/* Indirect branches and returns (c->pc set): straight on to the next block when it is in the cache, else back to the
   runtime's loop. Skipping the loop for these (~8 million a second on the Steam Deck) is phase 5's return linking. */
#define WT_DISPATCH(c) do { \
        if (!*(c)->halt_request && (c)->fpcr == (c)->disp_fpcr && (c)->lc) { \
            const wt_lce *e_ = &(c)->lc[((c)->pc >> 2) & (c)->lc_mask]; \
            if (e_->pc == (c)->pc && (uintptr_t)e_->fn > 2) { __attribute__((musttail)) return e_->fn(c); } \
        } \
        return; \
    } while (0)
typedef struct { uint64_t pc; wt_block_fn fn; } wt_entry;
/* a DLL exports wt_count, wt_table (sorted by pc) and wt_bases: the guest addresses of main, subsdk0 and sdk it was
   made for (the runtime refuses it if the loader put them elsewhere) */

/* ---- helpers used by the generated code ---- */
typedef uint8_t __attribute__((aligned(1), may_alias)) wt_u8u;
typedef uint16_t __attribute__((aligned(1), may_alias)) wt_u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) wt_u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) wt_u64u;

#define WT_LD(T, a) (*(volatile T *)(uintptr_t)(a))
#define WT_ST(T, a, v) (*(volatile T *)(uintptr_t)(a) = (v))

static inline wt_v128 wt_ld128(uint64_t a) { wt_v128 r; r.lo = WT_LD(wt_u64u, a); r.hi = WT_LD(wt_u64u, a + 8); return r; }
static inline void wt_st128(uint64_t a, wt_v128 v) { WT_ST(wt_u64u, a, v.lo); WT_ST(wt_u64u, a + 8, v.hi); }
static inline wt_v128 wt_v(uint64_t lo, uint64_t hi) { wt_v128 r = {lo, hi}; return r; }

static inline int wt_cond(uint32_t f, int cond) {
    int n = f >> 31 & 1, z = f >> 30 & 1, c = f >> 29 & 1, v = f >> 28 & 1, r;
    switch (cond >> 1) {
    case 0: r = z; break;                 /* EQ / NE */
    case 1: r = c; break;                 /* CS / CC */
    case 2: r = n; break;                 /* MI / PL */
    case 3: r = v; break;                 /* VS / VC */
    case 4: r = c && !z; break;           /* HI / LS */
    case 5: r = n == v; break;            /* GE / LT */
    case 6: r = n == v && !z; break;      /* GT / LE */
    default: return 1;                    /* AL / NV */
    }
    return cond & 1 ? !r : r;
}
static inline uint32_t wt_nz64(uint64_t r) { return (uint32_t)(r >> 63) << 31 | (uint32_t)(r == 0) << 30; }
static inline uint32_t wt_nz32(uint32_t r) { return (r >> 31) << 31 | (uint32_t)(r == 0) << 30; }
/* a + b + carry: result, carry out, signed overflow */
static inline uint32_t wt_add_flags64(uint64_t a, uint64_t b, unsigned ci, uint64_t r) {
    unsigned __int128 s = (unsigned __int128)a + b + ci;
    return wt_nz64(r) | (uint32_t)(s >> 64) << 29 | (uint32_t)((((a ^ r) & (b ^ r)) >> 63) & 1) << 28;
}
static inline uint32_t wt_add_flags32(uint32_t a, uint32_t b, unsigned ci, uint32_t r) {
    uint64_t s = (uint64_t)a + b + ci;
    return wt_nz32(r) | (uint32_t)(s >> 32) << 29 | ((((a ^ r) & (b ^ r)) >> 31) & 1) << 28;
}
static inline uint64_t wt_lsl64(uint64_t x, unsigned n) { return n >= 64 ? 0 : x << n; }
static inline uint64_t wt_lsr64(uint64_t x, unsigned n) { return n >= 64 ? 0 : x >> n; }
static inline uint64_t wt_asr64(uint64_t x, unsigned n) { return (uint64_t)((int64_t)x >> (n > 63 ? 63 : n)); }
static inline uint64_t wt_ror64(uint64_t x, unsigned n) { n &= 63; return n ? x >> n | x << (64 - n) : x; }
static inline uint32_t wt_lsl32(uint32_t x, unsigned n) { return n >= 32 ? 0 : x << n; }
static inline uint32_t wt_lsr32(uint32_t x, unsigned n) { return n >= 32 ? 0 : x >> n; }
static inline uint32_t wt_asr32(uint32_t x, unsigned n) { return (uint32_t)((int32_t)x >> (n > 31 ? 31 : n)); }
static inline uint32_t wt_ror32(uint32_t x, unsigned n) { n &= 31; return n ? x >> n | x << (32 - n) : x; }
static inline uint64_t wt_sdiv64(uint64_t a, uint64_t b) { return !b ? 0 : (int64_t)b == -1 ? (uint64_t)0 - a : (uint64_t)((int64_t)a / (int64_t)b); }
static inline uint32_t wt_sdiv32(uint32_t a, uint32_t b) { return !b ? 0 : (int32_t)b == -1 ? 0u - a : (uint32_t)((int32_t)a / (int32_t)b); }
static inline uint64_t wt_udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t wt_udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t wt_clz64(uint64_t x) { return x ? (uint64_t)__builtin_clzll(x) : 64; }
static inline uint32_t wt_clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint64_t wt_extr64(uint64_t hi, uint64_t lo, unsigned n) { return n ? lo >> n | hi << (64 - n) : lo; }
static inline uint32_t wt_extr32(uint32_t hi, uint32_t lo, unsigned n) { return n ? lo >> n | hi << (32 - n) : lo; }

/* exclusives: the value read is remembered; the write succeeds (returns 0) if memory still holds it */
#ifndef __cplusplus   /* the C++ test harness needs only the ABI */
#include "wt_native_ops.h"
#endif

#define WT_EXCL_READ(c, T, a, out) do { (c)->excl_addr = (a); (out) = WT_LD(T, a); (c)->excl_val.lo = (uint64_t)(out); (c)->excl_valid = 1; } while (0)
#define WT_EXCL_WRITE(c, T, a, v, out) do { \
        if (!(c)->excl_valid || (c)->excl_addr != (a)) (out) = 1; \
        else { T e_ = (T)(c)->excl_val.lo; (out) = !__atomic_compare_exchange_n((T *)(uintptr_t)(a), &e_, (T)(v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); } \
        (c)->excl_valid = 0; } while (0)
