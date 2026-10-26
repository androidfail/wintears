/* Direct Native: floating-point, vector and CRC IR operations for recompiled code, one helper per dynarmic opcode
   (wt_op_<Opcode>), with the arguments of the IR. Semantics follow dynarmic's x64 back end with FPCR 0 (round to
   nearest, no flush to zero) and NaNs not bit-exact (the JIT's TOTK_JIT_FAST_NAN default). Included by wt_native.h. */
#pragma once
#include <math.h>

typedef union { wt_v128 v; uint8_t b[16]; int8_t sb[16]; uint16_t h[8]; int16_t sh[8]; uint32_t w[4]; int32_t sw[4];
                uint64_t d[2]; int64_t sd[2]; float f[4]; double g[2]; } wt_lanes;
#define WT_L(x) ((wt_lanes){.v = (x)})

static inline float wt_f32(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static inline uint32_t wt_u32f(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }
static inline double wt_f64(uint64_t x) { double f; memcpy(&f, &x, 8); return f; }
static inline uint64_t wt_u64f(double f) { uint64_t x; memcpy(&x, &f, 8); return x; }

/* ---------- scalar floating point ---------- */
#define WT_FBIN(N, T, F, U, OP) static inline T wt_op_##N(T a, T b) { return U(F(a) OP F(b)); }
WT_FBIN(FPAdd32, uint32_t, wt_f32, wt_u32f, +) WT_FBIN(FPSub32, uint32_t, wt_f32, wt_u32f, -)
WT_FBIN(FPMul32, uint32_t, wt_f32, wt_u32f, *) WT_FBIN(FPDiv32, uint32_t, wt_f32, wt_u32f, /)
WT_FBIN(FPAdd64, uint64_t, wt_f64, wt_u64f, +) WT_FBIN(FPSub64, uint64_t, wt_f64, wt_u64f, -)
WT_FBIN(FPMul64, uint64_t, wt_f64, wt_u64f, *) WT_FBIN(FPDiv64, uint64_t, wt_f64, wt_u64f, /)
static inline uint32_t wt_op_FPAbs32(uint32_t a) { return a & 0x7FFFFFFFu; }
static inline uint64_t wt_op_FPAbs64(uint64_t a) { return a & 0x7FFFFFFFFFFFFFFFull; }
static inline uint32_t wt_op_FPNeg32(uint32_t a) { return a ^ 0x80000000u; }
static inline uint64_t wt_op_FPNeg64(uint64_t a) { return a ^ 0x8000000000000000ull; }
static inline uint32_t wt_op_FPSqrt32(uint32_t a) { return wt_u32f(sqrtf(wt_f32(a))); }
static inline uint64_t wt_op_FPSqrt64(uint64_t a) { return wt_u64f(sqrt(wt_f64(a))); }
static inline uint32_t wt_op_FPMulAdd32(uint32_t a, uint32_t b, uint32_t c) { return wt_u32f(fmaf(wt_f32(b), wt_f32(c), wt_f32(a))); }
static inline uint64_t wt_op_FPMulAdd64(uint64_t a, uint64_t b, uint64_t c) { return wt_u64f(fma(wt_f64(b), wt_f64(c), wt_f64(a))); }
static inline uint32_t wt_op_FPMulSub32(uint32_t a, uint32_t b, uint32_t c) { return wt_u32f(fmaf(-wt_f32(b), wt_f32(c), wt_f32(a))); }
static inline uint64_t wt_op_FPMulSub64(uint64_t a, uint64_t b, uint64_t c) { return wt_u64f(fma(-wt_f64(b), wt_f64(c), wt_f64(a))); }
static inline int wt_isinf32(uint32_t a) { return (a & 0x7FFFFFFFu) == 0x7F800000u; }
static inline int wt_iszero32(uint32_t a) { return !(a & 0x7FFFFFFFu); }
static inline int wt_isnan32(uint32_t a) { return (a & 0x7FFFFFFFu) > 0x7F800000u; }
static inline int wt_isnan64(uint64_t a) { return (a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static inline uint32_t wt_op_FPMulX32(uint32_t a, uint32_t b) {   /* infinity x zero = +-2 */
    if ((wt_isinf32(a) && wt_iszero32(b)) || (wt_iszero32(a) && wt_isinf32(b))) return ((a ^ b) & 0x80000000u) | 0x40000000u;
    return wt_op_FPMul32(a, b);
}
/* ARM FMIN / FMAX: a NaN wins; -0 is below +0 */
static inline uint32_t wt_op_FPMin32(uint32_t a, uint32_t b) {
    if (wt_isnan32(a)) return a | 0x00400000u; if (wt_isnan32(b)) return b | 0x00400000u;
    if (wt_iszero32(a) && wt_iszero32(b)) return a | b;
    return wt_f32(a) < wt_f32(b) ? a : b;
}
static inline uint32_t wt_op_FPMax32(uint32_t a, uint32_t b) {
    if (wt_isnan32(a)) return a | 0x00400000u; if (wt_isnan32(b)) return b | 0x00400000u;
    if (wt_iszero32(a) && wt_iszero32(b)) return a & b;
    return wt_f32(a) > wt_f32(b) ? a : b;
}
static inline uint64_t wt_op_FPMin64(uint64_t a, uint64_t b) {
    if (wt_isnan64(a)) return a | (1ull << 51); if (wt_isnan64(b)) return b | (1ull << 51);
    if (!(a << 1) && !(b << 1)) return a | b;
    return wt_f64(a) < wt_f64(b) ? a : b;
}
static inline uint64_t wt_op_FPMax64(uint64_t a, uint64_t b) {
    if (wt_isnan64(a)) return a | (1ull << 51); if (wt_isnan64(b)) return b | (1ull << 51);
    if (!(a << 1) && !(b << 1)) return a & b;
    return wt_f64(a) > wt_f64(b) ? a : b;
}
/* FMINNM / FMAXNM: a quiet NaN against a number gives the number */
static inline uint32_t wt_op_FPMinNumeric32(uint32_t a, uint32_t b) { if (wt_isnan32(a) && !wt_isnan32(b)) return b; if (wt_isnan32(b) && !wt_isnan32(a)) return a; return wt_op_FPMin32(a, b); }
static inline uint32_t wt_op_FPMaxNumeric32(uint32_t a, uint32_t b) { if (wt_isnan32(a) && !wt_isnan32(b)) return b; if (wt_isnan32(b) && !wt_isnan32(a)) return a; return wt_op_FPMax32(a, b); }
static inline uint64_t wt_op_FPMinNumeric64(uint64_t a, uint64_t b) { if (wt_isnan64(a) && !wt_isnan64(b)) return b; if (wt_isnan64(b) && !wt_isnan64(a)) return a; return wt_op_FPMin64(a, b); }
static inline uint64_t wt_op_FPMaxNumeric64(uint64_t a, uint64_t b) { if (wt_isnan64(a) && !wt_isnan64(b)) return b; if (wt_isnan64(b) && !wt_isnan64(a)) return a; return wt_op_FPMax64(a, b); }
/* FCMP: NZCV 0110 equal, 1000 less, 0010 greater, 0011 unordered */
static inline uint32_t wt_fcmp(int lt, int eq, int gt) { return eq ? 0x60000000u : lt ? 0x80000000u : gt ? 0x20000000u : 0x30000000u; }
static inline uint32_t wt_op_FPCompare32(uint32_t a, uint32_t b, unsigned q) { (void)q; float x = wt_f32(a), y = wt_f32(b); return wt_fcmp(x < y, x == y, x > y); }
static inline uint32_t wt_op_FPCompare64(uint64_t a, uint64_t b, unsigned q) { (void)q; double x = wt_f64(a), y = wt_f64(b); return wt_fcmp(x < y, x == y, x > y); }

/* rounding: dynarmic FP::RoundingMode (0 nearest even, 1 +inf, 2 -inf, 3 zero, 4 nearest away) */
static inline double wt_round(double x, unsigned mode) {
    switch (mode) {
    case 1: return ceil(x);
    case 2: return floor(x);
    case 3: return trunc(x);
    case 4: return round(x);
    default: return nearbyint(x);
    }
}
static inline uint32_t wt_op_FPRoundInt32(uint32_t a, uint8_t mode, unsigned exact) { (void)exact; if (wt_isnan32(a)) return a | 0x00400000u; return wt_u32f((float)wt_round(wt_f32(a), mode)); }
static inline uint64_t wt_op_FPRoundInt64(uint64_t a, uint8_t mode, unsigned exact) { (void)exact; if (wt_isnan64(a)) return a | (1ull << 51); return wt_u64f(wt_round(wt_f64(a), mode)); }
static inline uint64_t wt_op_FPSingleToDouble(uint32_t a, uint8_t mode) { (void)mode; return wt_u64f((double)wt_f32(a)); }
static inline uint32_t wt_op_FPDoubleToSingle(uint64_t a, uint8_t mode) { (void)mode; return wt_u32f((float)wt_f64(a)); }   /* the emitter allows mode 0 only */
/* FPToFixed: scaled by 2^fbits, rounded, saturated; NaN gives 0 */
static inline int64_t wt_tofix_s(double x, unsigned fbits, unsigned mode, int bits) {
    if (x != x) return 0;
    double r = wt_round(ldexp(x, (int)fbits), mode), hi = ldexp(1.0, bits - 1);
    if (r >= hi) return bits == 64 ? INT64_MAX : (int64_t)hi - 1;
    if (r < -hi) return bits == 64 ? INT64_MIN : -(int64_t)hi;
    return (int64_t)r;
}
static inline uint64_t wt_tofix_u(double x, unsigned fbits, unsigned mode, int bits) {
    if (x != x) return 0;
    double r = wt_round(ldexp(x, (int)fbits), mode), hi = ldexp(1.0, bits);
    if (r >= hi) return bits == 64 ? UINT64_MAX : (uint64_t)hi - 1;
    if (r <= 0) return 0;
    return (uint64_t)r;
}
#define WT_TOFIX(N, RT, IT, F, SU, B) static inline RT wt_op_##N(IT a, uint8_t fb, uint8_t m) { return (RT)wt_tofix_##SU(F(a), fb, m, B); }
WT_TOFIX(FPSingleToFixedS32, uint32_t, uint32_t, wt_f32, s, 32) WT_TOFIX(FPSingleToFixedS64, uint64_t, uint32_t, wt_f32, s, 64)
WT_TOFIX(FPSingleToFixedU32, uint32_t, uint32_t, wt_f32, u, 32) WT_TOFIX(FPSingleToFixedU64, uint64_t, uint32_t, wt_f32, u, 64)
WT_TOFIX(FPDoubleToFixedS32, uint32_t, uint64_t, wt_f64, s, 32) WT_TOFIX(FPDoubleToFixedS64, uint64_t, uint64_t, wt_f64, s, 64)
WT_TOFIX(FPDoubleToFixedU32, uint32_t, uint64_t, wt_f64, u, 32) WT_TOFIX(FPDoubleToFixedU64, uint64_t, uint64_t, wt_f64, u, 64)
/* FixedToFP: converted (one rounding, to nearest), then scaled down by 2^fbits (exact); the emitter allows mode 0 only */
#define WT_FROMFIX(N, RT, IT, CT, FT, U) static inline RT wt_op_##N(IT a, uint8_t fb, uint8_t m) { (void)m; return U((FT)ldexp((FT)(CT)a, -(int)fb)); }
WT_FROMFIX(FPFixedS32ToSingle, uint32_t, uint32_t, int32_t, float, wt_u32f) WT_FROMFIX(FPFixedU32ToSingle, uint32_t, uint32_t, uint32_t, float, wt_u32f)
WT_FROMFIX(FPFixedS64ToSingle, uint32_t, uint64_t, int64_t, float, wt_u32f) WT_FROMFIX(FPFixedU64ToSingle, uint32_t, uint64_t, uint64_t, float, wt_u32f)
WT_FROMFIX(FPFixedS32ToDouble, uint64_t, uint32_t, int32_t, double, wt_u64f) WT_FROMFIX(FPFixedU32ToDouble, uint64_t, uint32_t, uint32_t, double, wt_u64f)
WT_FROMFIX(FPFixedS64ToDouble, uint64_t, uint64_t, int64_t, double, wt_u64f) WT_FROMFIX(FPFixedU64ToDouble, uint64_t, uint64_t, uint64_t, double, wt_u64f)

/* FRECPE / FRSQRTE: ARM's 8-bit estimate tables (dynarmic common/math_util.cpp), single precision */
static inline uint8_t wt_recip_lut(uint64_t a) {   /* a in [256, 511] */
    a = a * 2 + 1; uint64_t b = (1u << 19) / a; return (uint8_t)((b + 1) / 2);
}
static inline uint8_t wt_rsqrt_lut(uint64_t a) {   /* a in [128, 511] */
    static uint8_t t[512]; static int ready;
    if (!ready) {
        for (uint64_t i = 128; i < 512; i++) {
            uint64_t x = i < 256 ? i * 2 + 1 : (i | 1) * 2, b = 512;
            while (x * (b + 1) * (b + 1) < (1u << 28)) b++;
            t[i] = (uint8_t)((b + 1) / 2);
        }
        ready = 1;
    }
    return t[a & 0x1FF];
}
static inline int wt_unpack32(uint32_t a, int *exp, uint64_t *mant) {   /* value = mant / 2^62 * 2^exp, mant bit 62 set */
    uint32_t e = a >> 23 & 0xFF, f = a & 0x7FFFFF;
    if (e) { *exp = (int)e - 127; *mant = (uint64_t)(f | 0x800000u) << 39; return 1; }
    int h = 31 - __builtin_clz(f); *exp = h - 149; *mant = (uint64_t)f << (62 - h); return 1;
}
static inline uint32_t wt_op_FPRecipEstimate32(uint32_t a) {
    uint32_t sign = a & 0x80000000u;
    if (wt_isnan32(a)) return a | 0x00400000u;
    if (wt_isinf32(a)) return sign;
    if (wt_iszero32(a)) return sign | 0x7F800000u;
    int exp; uint64_t mant; wt_unpack32(a, &exp, &mant);
    if (exp < -128) return sign | 0x7F800000u;   /* overflow, round to nearest: infinity */
    uint64_t est = (uint64_t)wt_recip_lut(mant >> 54) << 15;
    int rexp = -(exp + 1);
    if (rexp == -127) { est |= 0x800000u; est >>= 1; }
    else if (rexp == -128) { est |= 0x800000u; est >>= 2; rexp++; }
    return sign | (uint32_t)(rexp + 127) << 23 | ((uint32_t)est & 0x7FFFFFu);
}
static inline uint32_t wt_op_FPRSqrtEstimate32(uint32_t a) {
    if (wt_isnan32(a)) return a | 0x00400000u;
    if (wt_iszero32(a)) return (a & 0x80000000u) | 0x7F800000u;
    if (a & 0x80000000u) return 0x7FC00000u;
    if (wt_isinf32(a)) return 0;
    int exp; uint64_t mant; wt_unpack32(a, &exp, &mant);
    int rexp = (-(exp + 1)) >> 1, odd = exp % 2 == 0;
    uint64_t est = wt_rsqrt_lut(mant >> (62 - (odd ? 7 : 8)));
    return (uint32_t)(rexp + 127) << 23 | ((uint32_t)(est << 15) & 0x7FFFFFu);
}
static inline uint32_t wt_op_FPRecipStepFused32(uint32_t a, uint32_t b) {   /* 2 - a*b, fused; inf x 0 = 2 */
    if ((wt_isinf32(a) && wt_iszero32(b)) || (wt_iszero32(a) && wt_isinf32(b))) return 0x40000000u;
    return wt_u32f(fmaf(-wt_f32(a), wt_f32(b), 2.0f));
}
static inline uint32_t wt_op_FPRSqrtStepFused32(uint32_t a, uint32_t b) {   /* (3 - a*b) / 2, fused; inf x 0 = 1.5 */
    if ((wt_isinf32(a) && wt_iszero32(b)) || (wt_iszero32(a) && wt_isinf32(b))) return 0x3FC00000u;
    return wt_u32f(fmaf(-wt_f32(a), wt_f32(b), 3.0f) * 0.5f);
}

/* ---------- vectors: lane loops (the compiler vectorises them) ---------- */
#define WT_V2(N, F, L, EXPR) static inline wt_v128 wt_op_##N(wt_v128 x, wt_v128 y) { \
        wt_lanes a = WT_L(x), b = WT_L(y), r; for (unsigned i = 0; i < sizeof r.F / sizeof r.F[0]; i++) r.F[i] = (EXPR); return r.v; }
#define WT_V1(N, F, EXPR) static inline wt_v128 wt_op_##N(wt_v128 x) { \
        wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < sizeof r.F / sizeof r.F[0]; i++) r.F[i] = (EXPR); return r.v; }
static inline wt_v128 wt_op_VectorOr(wt_v128 a, wt_v128 b) { return wt_v(a.lo | b.lo, a.hi | b.hi); }
static inline wt_v128 wt_op_VectorAnd(wt_v128 a, wt_v128 b) { return wt_v(a.lo & b.lo, a.hi & b.hi); }
static inline wt_v128 wt_op_VectorAndNot(wt_v128 a, wt_v128 b) { return wt_v(a.lo & ~b.lo, a.hi & ~b.hi); }
static inline wt_v128 wt_op_VectorEor(wt_v128 a, wt_v128 b) { return wt_v(a.lo ^ b.lo, a.hi ^ b.hi); }
static inline wt_v128 wt_op_VectorNot(wt_v128 a) { return wt_v(~a.lo, ~a.hi); }
WT_V2(VectorAdd8, b, 16, a.b[i] + b.b[i]) WT_V2(VectorAdd16, h, 8, a.h[i] + b.h[i]) WT_V2(VectorAdd32, w, 4, a.w[i] + b.w[i]) WT_V2(VectorAdd64, d, 2, a.d[i] + b.d[i])
WT_V2(VectorSub8, b, 16, a.b[i] - b.b[i]) WT_V2(VectorSub16, h, 8, a.h[i] - b.h[i]) WT_V2(VectorSub32, w, 4, a.w[i] - b.w[i]) WT_V2(VectorSub64, d, 2, a.d[i] - b.d[i])
WT_V2(VectorMultiply16, h, 8, a.h[i] * b.h[i]) WT_V2(VectorMultiply32, w, 4, a.w[i] * b.w[i])
WT_V2(VectorEqual8, b, 16, a.b[i] == b.b[i] ? 0xFF : 0) WT_V2(VectorEqual16, h, 8, a.h[i] == b.h[i] ? 0xFFFF : 0)
WT_V2(VectorEqual32, w, 4, a.w[i] == b.w[i] ? ~0u : 0) WT_V2(VectorEqual64, d, 2, a.d[i] == b.d[i] ? ~0ull : 0)
WT_V2(VectorGreaterS8, b, 16, a.sb[i] > b.sb[i] ? 0xFF : 0) WT_V2(VectorGreaterS16, h, 8, a.sh[i] > b.sh[i] ? 0xFFFF : 0)
WT_V2(VectorGreaterS32, w, 4, a.sw[i] > b.sw[i] ? ~0u : 0) WT_V2(VectorGreaterS64, d, 2, a.sd[i] > b.sd[i] ? ~0ull : 0)
WT_V2(VectorMinS8, sb, 16, a.sb[i] < b.sb[i] ? a.sb[i] : b.sb[i]) WT_V2(VectorMinS16, sh, 8, a.sh[i] < b.sh[i] ? a.sh[i] : b.sh[i]) WT_V2(VectorMinS32, sw, 4, a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i])
WT_V2(VectorMaxS8, sb, 16, a.sb[i] > b.sb[i] ? a.sb[i] : b.sb[i]) WT_V2(VectorMaxS16, sh, 8, a.sh[i] > b.sh[i] ? a.sh[i] : b.sh[i]) WT_V2(VectorMaxS32, sw, 4, a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i])
WT_V2(VectorMinU8, b, 16, a.b[i] < b.b[i] ? a.b[i] : b.b[i]) WT_V2(VectorMinU16, h, 8, a.h[i] < b.h[i] ? a.h[i] : b.h[i]) WT_V2(VectorMinU32, w, 4, a.w[i] < b.w[i] ? a.w[i] : b.w[i])
WT_V2(VectorMaxU8, b, 16, a.b[i] > b.b[i] ? a.b[i] : b.b[i]) WT_V2(VectorMaxU16, h, 8, a.h[i] > b.h[i] ? a.h[i] : b.h[i]) WT_V2(VectorMaxU32, w, 4, a.w[i] > b.w[i] ? a.w[i] : b.w[i])
WT_V2(VectorUnsignedAbsoluteDifference8, b, 16, a.b[i] > b.b[i] ? a.b[i] - b.b[i] : b.b[i] - a.b[i])
WT_V2(VectorUnsignedAbsoluteDifference16, h, 8, a.h[i] > b.h[i] ? a.h[i] - b.h[i] : b.h[i] - a.h[i])
WT_V2(VectorHalvingAddS16, sh, 8, (int16_t)(((int32_t)a.sh[i] + b.sh[i]) >> 1)) WT_V2(VectorHalvingAddS32, sw, 4, (int32_t)(((int64_t)a.sw[i] + b.sw[i]) >> 1))
WT_V2(VectorHalvingSubS16, sh, 8, (int16_t)(((int32_t)a.sh[i] - b.sh[i]) >> 1)) WT_V2(VectorHalvingSubS32, sw, 4, (int32_t)(((int64_t)a.sw[i] - b.sw[i]) >> 1))
WT_V2(VectorHalvingAddU8, b, 16, (uint8_t)(((unsigned)a.b[i] + b.b[i]) >> 1))
WT_V2(VectorSignedSaturatedAdd16, sh, 8, (int16_t)((int32_t)a.sh[i] + b.sh[i] > 32767 ? 32767 : (int32_t)a.sh[i] + b.sh[i] < -32768 ? -32768 : a.sh[i] + b.sh[i]))
WT_V2(VectorSignedSaturatedAdd32, sw, 4, (int32_t)((int64_t)a.sw[i] + b.sw[i] > INT32_MAX ? INT32_MAX : (int64_t)a.sw[i] + b.sw[i] < INT32_MIN ? INT32_MIN : (int64_t)a.sw[i] + b.sw[i]))
WT_V2(VectorSignedSaturatedSub32, sw, 4, (int32_t)((int64_t)a.sw[i] - b.sw[i] > INT32_MAX ? INT32_MAX : (int64_t)a.sw[i] - b.sw[i] < INT32_MIN ? INT32_MIN : (int64_t)a.sw[i] - b.sw[i]))
/* SQDMULH / SQRDMULH: (2ab [+ 2^(n-1)]) >> n, saturated (only INT_MIN * INT_MIN overflows) */
WT_V2(VectorSignedSaturatedDoublingMultiplyHigh16, sh, 8, (int16_t)(a.sh[i] == INT16_MIN && b.sh[i] == INT16_MIN ? INT16_MAX : ((int32_t)a.sh[i] * b.sh[i] * 2) >> 16))
WT_V2(VectorSignedSaturatedDoublingMultiplyHigh32, sw, 4, (int32_t)(a.sw[i] == INT32_MIN && b.sw[i] == INT32_MIN ? INT32_MAX : ((int64_t)a.sw[i] * b.sw[i] * 2) >> 32))
WT_V2(VectorSignedSaturatedDoublingMultiplyHighRounding32, sw, 4, (int32_t)(a.sw[i] == INT32_MIN && b.sw[i] == INT32_MIN ? INT32_MAX : ((int64_t)a.sw[i] * b.sw[i] * 2 + (1ll << 31)) >> 32))
WT_V1(VectorPopulationCount, b, (uint8_t)__builtin_popcount(a.b[i]))
WT_V1(VectorCountLeadingZeros32, w, a.w[i] ? (uint32_t)__builtin_clz(a.w[i]) : 32u)
WT_V1(VectorSignedSaturatedAbs16, sh, (int16_t)(a.sh[i] == INT16_MIN ? INT16_MAX : a.sh[i] < 0 ? -a.sh[i] : a.sh[i]))
/* shifts by an immediate: an amount of the lane width or more gives 0 (logical) or the sign (arithmetic) */
#define WT_VSH(N, F, W, EXPR) static inline wt_v128 wt_op_##N(wt_v128 x, uint8_t s) { \
        wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 128 / W; i++) r.F[i] = (EXPR); return r.v; }
WT_VSH(VectorLogicalShiftLeft8, b, 8, s >= 8 ? 0 : (uint8_t)(a.b[i] << s)) WT_VSH(VectorLogicalShiftLeft16, h, 16, s >= 16 ? 0 : (uint16_t)(a.h[i] << s))
WT_VSH(VectorLogicalShiftLeft32, w, 32, s >= 32 ? 0 : a.w[i] << s) WT_VSH(VectorLogicalShiftLeft64, d, 64, s >= 64 ? 0 : a.d[i] << s)
WT_VSH(VectorLogicalShiftRight8, b, 8, s >= 8 ? 0 : a.b[i] >> s) WT_VSH(VectorLogicalShiftRight16, h, 16, s >= 16 ? 0 : a.h[i] >> s)
WT_VSH(VectorLogicalShiftRight32, w, 32, s >= 32 ? 0 : a.w[i] >> s) WT_VSH(VectorLogicalShiftRight64, d, 64, s >= 64 ? 0 : a.d[i] >> s)
WT_VSH(VectorArithmeticShiftRight8, sb, 8, a.sb[i] >> (s > 7 ? 7 : s)) WT_VSH(VectorArithmeticShiftRight16, sh, 16, a.sh[i] >> (s > 15 ? 15 : s))
WT_VSH(VectorArithmeticShiftRight32, sw, 32, a.sw[i] >> (s > 31 ? 31 : s)) WT_VSH(VectorArithmeticShiftRight64, sd, 64, a.sd[i] >> (s > 63 ? 63 : s))
/* USHL / SSHL: per lane by the signed low byte of b (negative shifts right) */
static inline uint64_t wt_ushl(uint64_t x, int8_t s, int w) { return s >= w || s <= -w ? 0 : s >= 0 ? x << s : x >> -s; }
static inline int64_t wt_sshl(int64_t x, int8_t s, int w) { return s >= w ? 0 : s <= -w ? (x < 0 ? -1 : 0) : s >= 0 ? (int64_t)((uint64_t)x << s) : x >> -s; }
WT_V2(VectorLogicalVShift32, w, 4, (uint32_t)wt_ushl(a.w[i], (int8_t)b.w[i], 32)) WT_V2(VectorLogicalVShift64, d, 2, wt_ushl(a.d[i], (int8_t)b.d[i], 64))
WT_V2(VectorArithmeticVShift32, sw, 4, (int32_t)wt_sshl(a.sw[i], (int8_t)b.w[i], 32)) WT_V2(VectorArithmeticVShift64, sd, 2, wt_sshl(a.sd[i], (int8_t)b.d[i], 64))

/* element moves */
#define WT_SETEL(N, F, T) static inline wt_v128 wt_op_##N(wt_v128 x, uint8_t i, T v) { wt_lanes a = WT_L(x); a.F[i] = v; return a.v; }
WT_SETEL(VectorSetElement8, b, uint8_t) WT_SETEL(VectorSetElement16, h, uint16_t) WT_SETEL(VectorSetElement32, w, uint32_t) WT_SETEL(VectorSetElement64, d, uint64_t)
#define WT_BCAST(N, F, T, LANES) static inline wt_v128 wt_op_##N(T v) { wt_lanes r = {{0, 0}}; for (unsigned i = 0; i < LANES; i++) r.F[i] = v; return r.v; }
WT_BCAST(VectorBroadcast8, b, uint8_t, 16) WT_BCAST(VectorBroadcast16, h, uint16_t, 8) WT_BCAST(VectorBroadcast32, w, uint32_t, 4) WT_BCAST(VectorBroadcast64, d, uint64_t, 2)
WT_BCAST(VectorBroadcastLower8, b, uint8_t, 8) WT_BCAST(VectorBroadcastLower16, h, uint16_t, 4) WT_BCAST(VectorBroadcastLower32, w, uint32_t, 2)
#define WT_BCASTEL(N, F, LANES) static inline wt_v128 wt_op_##N(wt_v128 x, uint8_t idx) { wt_lanes a = WT_L(x), r = {{0, 0}}; for (unsigned i = 0; i < LANES; i++) r.F[i] = a.F[idx]; return r.v; }
WT_BCASTEL(VectorBroadcastElement8, b, 16) WT_BCASTEL(VectorBroadcastElement16, h, 8) WT_BCASTEL(VectorBroadcastElement32, w, 4) WT_BCASTEL(VectorBroadcastElement64, d, 2)
WT_BCASTEL(VectorBroadcastElementLower8, b, 8) WT_BCASTEL(VectorBroadcastElementLower16, h, 4) WT_BCASTEL(VectorBroadcastElementLower32, w, 2)
static inline wt_v128 wt_op_VectorExtract(wt_v128 a, wt_v128 b, uint8_t pos) {   /* (b:a) >> pos */
    if (pos == 0) return a; if (pos == 128) return b;
    if (pos < 64) return wt_v(a.lo >> pos | a.hi << (64 - pos), a.hi >> pos | b.lo << (64 - pos));
    if (pos == 64) return wt_v(a.hi, b.lo);
    pos -= 64; return wt_v(a.hi >> pos | b.lo << (64 - pos), b.lo >> pos | b.hi << (64 - pos));
}
static inline wt_v128 wt_op_VectorExtractLower(wt_v128 a, wt_v128 b, uint8_t pos) {   /* (b.lo:a.lo) >> pos, upper 0 */
    return wt_v(pos == 0 ? a.lo : pos == 64 ? b.lo : a.lo >> pos | b.lo << (64 - pos), 0);
}
static inline wt_v128 wt_op_VectorRotateWholeVectorRight(wt_v128 a, uint8_t n) {
    n &= 127; if (!n) return a; if (n == 64) return wt_v(a.hi, a.lo);
    if (n < 64) return wt_v(a.lo >> n | a.hi << (64 - n), a.hi >> n | a.lo << (64 - n));
    n -= 64; return wt_v(a.hi >> n | a.lo << (64 - n), a.lo >> n | a.hi << (64 - n));
}
static inline wt_v128 wt_op_VectorZeroUpper(wt_v128 a) { return wt_v(a.lo, 0); }
static inline wt_v128 wt_op_ZeroVector(void) { return wt_v(0, 0); }
#define WT_WIDEN(N, FI, FO, LANES, CAST) static inline wt_v128 wt_op_##N(wt_v128 x) { wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < LANES; i++) r.FO[i] = CAST a.FI[i]; return r.v; }
WT_WIDEN(VectorZeroExtend8, b, h, 8, (uint16_t)) WT_WIDEN(VectorZeroExtend16, h, w, 4, (uint32_t)) WT_WIDEN(VectorZeroExtend32, w, d, 2, (uint64_t))
WT_WIDEN(VectorSignExtend8, sb, sh, 8, (int16_t)) WT_WIDEN(VectorSignExtend16, sh, sw, 4, (int32_t)) WT_WIDEN(VectorSignExtend32, sw, sd, 2, (int64_t))
static inline wt_v128 wt_op_VectorZeroExtend64(wt_v128 a) { return wt_v(a.lo, 0); }
static inline wt_v128 wt_op_VectorSignExtend64(wt_v128 a) { return wt_v(a.lo, (uint64_t)((int64_t)a.lo >> 63)); }
#define WT_NARROW(N, FI, FO, LANES, T) static inline wt_v128 wt_op_##N(wt_v128 x) { wt_lanes a = WT_L(x), r = {{0, 0}}; for (unsigned i = 0; i < LANES; i++) r.FO[i] = (T)a.FI[i]; return r.v; }
WT_NARROW(VectorNarrow16, h, b, 8, uint8_t) WT_NARROW(VectorNarrow32, w, h, 4, uint16_t) WT_NARROW(VectorNarrow64, d, w, 2, uint32_t)
static inline wt_v128 wt_op_VectorSignedSaturatedNarrowToSigned64(wt_v128 x) {
    wt_lanes a = WT_L(x), r = {{0, 0}};
    for (unsigned i = 0; i < 2; i++) r.sw[i] = a.sd[i] > INT32_MAX ? INT32_MAX : a.sd[i] < INT32_MIN ? INT32_MIN : (int32_t)a.sd[i];
    return r.v;
}
static inline wt_v128 wt_op_VectorSignedSaturatedNarrowToSigned32(wt_v128 x) {
    wt_lanes a = WT_L(x), r = {{0, 0}};
    for (unsigned i = 0; i < 4; i++) r.sh[i] = a.sw[i] > INT16_MAX ? INT16_MAX : a.sw[i] < INT16_MIN ? INT16_MIN : (int16_t)a.sw[i];
    return r.v;
}
static inline wt_v128 wt_op_VectorSignedSaturatedNarrowToUnsigned16(wt_v128 x) {
    wt_lanes a = WT_L(x), r = {{0, 0}};
    for (unsigned i = 0; i < 8; i++) r.b[i] = a.sh[i] > 255 ? 255 : a.sh[i] < 0 ? 0 : (uint8_t)a.sh[i];
    return r.v;
}
static inline wt_v128 wt_op_VectorSignedSaturatedNarrowToUnsigned32(wt_v128 x) {
    wt_lanes a = WT_L(x), r = {{0, 0}};
    for (unsigned i = 0; i < 4; i++) r.h[i] = a.sw[i] > 65535 ? 65535 : a.sw[i] < 0 ? 0 : (uint16_t)a.sw[i];
    return r.v;
}
/* widening multiply of the lower lanes */
static inline wt_v128 wt_op_VectorMultiplySignedWiden16(wt_v128 x, wt_v128 y) { wt_lanes a = WT_L(x), b = WT_L(y), r; for (unsigned i = 0; i < 4; i++) r.sw[i] = (int32_t)a.sh[i] * b.sh[i]; return r.v; }
static inline wt_v128 wt_op_VectorMultiplySignedWiden32(wt_v128 x, wt_v128 y) { wt_lanes a = WT_L(x), b = WT_L(y), r; for (unsigned i = 0; i < 2; i++) r.sd[i] = (int64_t)a.sw[i] * b.sw[i]; return r.v; }
static inline wt_v128 wt_op_VectorPolynomialMultiplyLong64(wt_v128 x, wt_v128 y) {   /* PMULL (lower 64 x 64 carry-less) */
    uint64_t a = x.lo, b = y.lo, lo = 0, hi = 0;
    for (int i = 0; i < 64; i++) if (b >> i & 1) { lo ^= a << i; if (i) hi ^= a >> (64 - i); }
    return wt_v(lo, hi);
}
/* ZIP / UZP / TRN */
#define WT_ZIP(N, F, LANES, OFF) static inline wt_v128 wt_op_##N(wt_v128 x, wt_v128 y) { wt_lanes a = WT_L(x), b = WT_L(y), r = {{0, 0}}; \
        for (unsigned i = 0; i < LANES / 2; i++) { r.F[2 * i] = a.F[OFF + i]; r.F[2 * i + 1] = b.F[OFF + i]; } return r.v; }
WT_ZIP(VectorInterleaveLower8, b, 16, 0) WT_ZIP(VectorInterleaveLower16, h, 8, 0) WT_ZIP(VectorInterleaveLower32, w, 4, 0) WT_ZIP(VectorInterleaveLower64, d, 2, 0)
WT_ZIP(VectorInterleaveUpper8, b, 16, 8) WT_ZIP(VectorInterleaveUpper16, h, 8, 4) WT_ZIP(VectorInterleaveUpper32, w, 4, 2) WT_ZIP(VectorInterleaveUpper64, d, 2, 1)
#define WT_UZP(N, F, LANES, ODD) static inline wt_v128 wt_op_##N(wt_v128 x, wt_v128 y) { wt_lanes a = WT_L(x), b = WT_L(y), r; \
        for (unsigned i = 0; i < LANES / 2; i++) { r.F[i] = a.F[2 * i + ODD]; r.F[LANES / 2 + i] = b.F[2 * i + ODD]; } return r.v; }
WT_UZP(VectorDeinterleaveEven8, b, 16, 0) WT_UZP(VectorDeinterleaveEven16, h, 8, 0) WT_UZP(VectorDeinterleaveEven32, w, 4, 0) WT_UZP(VectorDeinterleaveEven64, d, 2, 0)
WT_UZP(VectorDeinterleaveOdd8, b, 16, 1) WT_UZP(VectorDeinterleaveOdd16, h, 8, 1) WT_UZP(VectorDeinterleaveOdd32, w, 4, 1) WT_UZP(VectorDeinterleaveOdd64, d, 2, 1)
#define WT_UZPL(N, F, LANES, ODD) static inline wt_v128 wt_op_##N(wt_v128 x, wt_v128 y) { wt_lanes a = WT_L(x), b = WT_L(y), r = {{0, 0}}; \
        for (unsigned i = 0; i < LANES / 4; i++) { r.F[i] = a.F[2 * i + ODD]; r.F[LANES / 4 + i] = b.F[2 * i + ODD]; } return r.v; }
WT_UZPL(VectorDeinterleaveEvenLower8, b, 16, 0) WT_UZPL(VectorDeinterleaveEvenLower16, h, 8, 0) WT_UZPL(VectorDeinterleaveEvenLower32, w, 4, 0)
WT_UZPL(VectorDeinterleaveOddLower8, b, 16, 1) WT_UZPL(VectorDeinterleaveOddLower16, h, 8, 1) WT_UZPL(VectorDeinterleaveOddLower32, w, 4, 1)
#define WT_TRN(N, F, LANES) static inline wt_v128 wt_op_##N(wt_v128 x, wt_v128 y, unsigned part) { wt_lanes a = WT_L(x), b = WT_L(y), r; \
        for (unsigned i = 0; i < LANES / 2; i++) { r.F[2 * i] = a.F[2 * i + part]; r.F[2 * i + 1] = b.F[2 * i + part]; } return r.v; }
WT_TRN(VectorTranspose8, b, 16) WT_TRN(VectorTranspose16, h, 8) WT_TRN(VectorTranspose32, w, 4) WT_TRN(VectorTranspose64, d, 2)
/* REV16 / REV32 / REV64 */
#define WT_REV(N, F, LANES, GROUP) static inline wt_v128 wt_op_##N(wt_v128 x) { wt_lanes a = WT_L(x), r; \
        for (unsigned i = 0; i < LANES; i++) r.F[i] = a.F[(i / GROUP) * GROUP + GROUP - 1 - i % GROUP]; return r.v; }
WT_REV(VectorReverseElementsInHalfGroups8, b, 16, 2) WT_REV(VectorReverseElementsInWordGroups8, b, 16, 4) WT_REV(VectorReverseElementsInWordGroups16, h, 8, 2)
WT_REV(VectorReverseElementsInLongGroups8, b, 16, 8) WT_REV(VectorReverseElementsInLongGroups16, h, 8, 4) WT_REV(VectorReverseElementsInLongGroups32, w, 4, 2)
/* ADDV and ADDP */
#define WT_REDUCE(N, F, LANES) static inline wt_v128 wt_op_##N(wt_v128 x) { wt_lanes a = WT_L(x), r = {{0, 0}}; \
        for (unsigned i = 0; i < LANES; i++) r.F[0] += a.F[i]; return r.v; }
WT_REDUCE(VectorReduceAdd8, b, 16) WT_REDUCE(VectorReduceAdd16, h, 8) WT_REDUCE(VectorReduceAdd32, w, 4) WT_REDUCE(VectorReduceAdd64, d, 2)
#define WT_PAIRL(N, F, LANES) static inline wt_v128 wt_op_##N(wt_v128 x, wt_v128 y) { wt_lanes a = WT_L(x), b = WT_L(y), r = {{0, 0}}; \
        for (unsigned i = 0; i < LANES / 4; i++) { r.F[i] = a.F[2 * i] + a.F[2 * i + 1]; r.F[LANES / 4 + i] = b.F[2 * i] + b.F[2 * i + 1]; } return r.v; }
WT_PAIRL(VectorPairedAddLower8, b, 16) WT_PAIRL(VectorPairedAddLower16, h, 8) WT_PAIRL(VectorPairedAddLower32, w, 4)
#define WT_PAIR(N, F, LANES) static inline wt_v128 wt_op_##N(wt_v128 x, wt_v128 y) { wt_lanes a = WT_L(x), b = WT_L(y), r; \
        for (unsigned i = 0; i < LANES / 2; i++) { r.F[i] = a.F[2 * i] + a.F[2 * i + 1]; r.F[LANES / 2 + i] = b.F[2 * i] + b.F[2 * i + 1]; } return r.v; }
WT_PAIR(VectorPairedAdd8, b, 16) WT_PAIR(VectorPairedAdd16, h, 8) WT_PAIR(VectorPairedAdd32, w, 4) WT_PAIR(VectorPairedAdd64, d, 2)
static inline wt_v128 wt_op_VectorPairedMinLowerS32(wt_v128 x, wt_v128 y) {
    wt_lanes a = WT_L(x), b = WT_L(y), r = {{0, 0}};
    r.sw[0] = a.sw[0] < a.sw[1] ? a.sw[0] : a.sw[1]; r.sw[1] = b.sw[0] < b.sw[1] ? b.sw[0] : b.sw[1]; return r.v;
}
/* TBL / TBX: index bytes past the table take the byte of `defaults` */
typedef struct { wt_v128 t[4]; unsigned n; } wt_tbl;
static inline wt_v128 wt_op_VectorTableLookup128(wt_v128 def, wt_tbl t, wt_v128 idx) {
    wt_lanes d = WT_L(def), x = WT_L(idx), r; uint8_t bytes[64]; memcpy(bytes, t.t, sizeof bytes);
    for (unsigned i = 0; i < 16; i++) r.b[i] = x.b[i] < 16 * t.n ? bytes[x.b[i]] : d.b[i];
    return r.v;
}

/* ---------- floating-point vectors (the last U1 argument, fpcr_controlled, is always on in A64) ---------- */
#define WT_FV2(N, EXPR) static inline wt_v128 wt_op_##N##32(wt_v128 x, wt_v128 y, unsigned fc) { (void)fc; \
        wt_lanes a = WT_L(x), b = WT_L(y), r; for (unsigned i = 0; i < 4; i++) r.w[i] = EXPR(a.w[i], b.w[i]); return r.v; }
#define WT_FV2D(N, EXPR) static inline wt_v128 wt_op_##N##64(wt_v128 x, wt_v128 y, unsigned fc) { (void)fc; \
        wt_lanes a = WT_L(x), b = WT_L(y), r; for (unsigned i = 0; i < 2; i++) r.d[i] = EXPR(a.d[i], b.d[i]); return r.v; }
WT_FV2(FPVectorAdd, wt_op_FPAdd32) WT_FV2(FPVectorSub, wt_op_FPSub32) WT_FV2(FPVectorMul, wt_op_FPMul32) WT_FV2(FPVectorDiv, wt_op_FPDiv32)
WT_FV2(FPVectorMin, wt_op_FPMin32) WT_FV2(FPVectorMax, wt_op_FPMax32) WT_FV2(FPVectorMinNumeric, wt_op_FPMinNumeric32) WT_FV2(FPVectorMaxNumeric, wt_op_FPMaxNumeric32)
WT_FV2(FPVectorMulX, wt_op_FPMulX32) WT_FV2(FPVectorRecipStepFused, wt_op_FPRecipStepFused32) WT_FV2(FPVectorRSqrtStepFused, wt_op_FPRSqrtStepFused32)
WT_FV2D(FPVectorAdd, wt_op_FPAdd64) WT_FV2D(FPVectorSub, wt_op_FPSub64) WT_FV2D(FPVectorMul, wt_op_FPMul64) WT_FV2D(FPVectorDiv, wt_op_FPDiv64)
WT_FV2D(FPVectorMin, wt_op_FPMin64) WT_FV2D(FPVectorMax, wt_op_FPMax64)
static inline uint32_t wt_feq(uint32_t a, uint32_t b) { return wt_f32(a) == wt_f32(b) ? ~0u : 0; }
static inline uint32_t wt_fgt(uint32_t a, uint32_t b) { return wt_f32(a) > wt_f32(b) ? ~0u : 0; }
static inline uint32_t wt_fge(uint32_t a, uint32_t b) { return wt_f32(a) >= wt_f32(b) ? ~0u : 0; }
WT_FV2(FPVectorEqual, wt_feq) WT_FV2(FPVectorGreater, wt_fgt) WT_FV2(FPVectorGreaterEqual, wt_fge)
#define WT_FV1(N, EXPR) static inline wt_v128 wt_op_##N##32(wt_v128 x, unsigned fc) { (void)fc; \
        wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 4; i++) r.w[i] = EXPR(a.w[i]); return r.v; }
WT_FV1(FPVectorSqrt, wt_op_FPSqrt32) WT_FV1(FPVectorRecipEstimate, wt_op_FPRecipEstimate32) WT_FV1(FPVectorRSqrtEstimate, wt_op_FPRSqrtEstimate32)
static inline wt_v128 wt_op_FPVectorAbs32(wt_v128 a) { return wt_v(a.lo & 0x7FFFFFFF7FFFFFFFull, a.hi & 0x7FFFFFFF7FFFFFFFull); }
static inline wt_v128 wt_op_FPVectorNeg32(wt_v128 a) { return wt_v(a.lo ^ 0x8000000080000000ull, a.hi ^ 0x8000000080000000ull); }
static inline wt_v128 wt_op_FPVectorAbs64(wt_v128 a) { return wt_v(a.lo & 0x7FFFFFFFFFFFFFFFull, a.hi & 0x7FFFFFFFFFFFFFFFull); }
static inline wt_v128 wt_op_FPVectorNeg64(wt_v128 a) { return wt_v(a.lo ^ 0x8000000000000000ull, a.hi ^ 0x8000000000000000ull); }
static inline wt_v128 wt_op_FPVectorMulAdd32(wt_v128 x, wt_v128 y, wt_v128 z, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), b = WT_L(y), c = WT_L(z), r;
    for (unsigned i = 0; i < 4; i++) r.w[i] = wt_op_FPMulAdd32(a.w[i], b.w[i], c.w[i]);
    return r.v;
}
static inline wt_v128 wt_op_FPVectorPairedAddLower32(wt_v128 x, wt_v128 y, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), b = WT_L(y), r = {{0, 0}};
    r.w[0] = wt_op_FPAdd32(a.w[0], a.w[1]); r.w[1] = wt_op_FPAdd32(b.w[0], b.w[1]); return r.v;
}
static inline wt_v128 wt_op_FPVectorPairedAdd32(wt_v128 x, wt_v128 y, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), b = WT_L(y), r;
    r.w[0] = wt_op_FPAdd32(a.w[0], a.w[1]); r.w[1] = wt_op_FPAdd32(a.w[2], a.w[3]);
    r.w[2] = wt_op_FPAdd32(b.w[0], b.w[1]); r.w[3] = wt_op_FPAdd32(b.w[2], b.w[3]); return r.v;
}
static inline wt_v128 wt_op_FPVectorRoundInt32(wt_v128 x, uint8_t m, unsigned exact, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 4; i++) r.w[i] = wt_op_FPRoundInt32(a.w[i], m, exact); return r.v;
}
static inline wt_v128 wt_op_FPVectorRoundInt64(wt_v128 x, uint8_t m, unsigned exact, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 2; i++) r.d[i] = wt_op_FPRoundInt64(a.d[i], m, exact); return r.v;
}
static inline wt_v128 wt_op_FPVectorToSignedFixed32(wt_v128 x, uint8_t fb, uint8_t m, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 4; i++) r.w[i] = wt_op_FPSingleToFixedS32(a.w[i], fb, m); return r.v;
}
static inline wt_v128 wt_op_FPVectorToUnsignedFixed32(wt_v128 x, uint8_t fb, uint8_t m, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 4; i++) r.w[i] = wt_op_FPSingleToFixedU32(a.w[i], fb, m); return r.v;
}
static inline wt_v128 wt_op_FPVectorToSignedFixed64(wt_v128 x, uint8_t fb, uint8_t m, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 2; i++) r.d[i] = wt_op_FPDoubleToFixedS64(a.d[i], fb, m); return r.v;
}
static inline wt_v128 wt_op_FPVectorFromSignedFixed32(wt_v128 x, uint8_t fb, uint8_t m, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 4; i++) r.w[i] = wt_op_FPFixedS32ToSingle(a.w[i], fb, m); return r.v;
}
static inline wt_v128 wt_op_FPVectorFromUnsignedFixed32(wt_v128 x, uint8_t fb, uint8_t m, unsigned fc) {
    (void)fc; wt_lanes a = WT_L(x), r; for (unsigned i = 0; i < 4; i++) r.w[i] = wt_op_FPFixedU32ToSingle(a.w[i], fb, m); return r.v;
}

/* ---------- CRC32 (ISO: reflected 0xEDB88320, Castagnoli: reflected 0x82F63B78) ---------- */
static inline uint32_t wt_crc(uint32_t crc, uint64_t v, int bytes, uint32_t poly) {
    for (int i = 0; i < bytes * 8; i++) { crc ^= (uint32_t)(v >> i & 1); crc = crc & 1 ? crc >> 1 ^ poly : crc >> 1; }
    return crc;
}
static inline uint32_t wt_op_CRC32ISO8(uint32_t c, uint32_t v) { return wt_crc(c, v, 1, 0xEDB88320u); }
static inline uint32_t wt_op_CRC32ISO16(uint32_t c, uint32_t v) { return wt_crc(c, v, 2, 0xEDB88320u); }
static inline uint32_t wt_op_CRC32ISO32(uint32_t c, uint32_t v) { return wt_crc(c, v, 4, 0xEDB88320u); }
static inline uint32_t wt_op_CRC32ISO64(uint32_t c, uint64_t v) { return wt_crc(c, v, 8, 0xEDB88320u); }
static inline uint32_t wt_op_CRC32Castagnoli8(uint32_t c, uint32_t v) { return wt_crc(c, v, 1, 0x82F63B78u); }
static inline uint32_t wt_op_CRC32Castagnoli16(uint32_t c, uint32_t v) { return wt_crc(c, v, 2, 0x82F63B78u); }
static inline uint32_t wt_op_CRC32Castagnoli32(uint32_t c, uint32_t v) { return wt_crc(c, v, 4, 0x82F63B78u); }
static inline uint32_t wt_op_CRC32Castagnoli64(uint32_t c, uint64_t v) { return wt_crc(c, v, 8, 0x82F63B78u); }
