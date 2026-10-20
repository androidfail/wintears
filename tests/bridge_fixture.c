/* Host functions the bridge test calls from ARM64 code. The NVN(...) form is what tools/gen_bridge.py reads. */
#include "bridge_fixture.h"
#define NVN(name, ret, ...) ret name(__VA_ARGS__)

/* 9 integer arguments (the 9th goes on the guest stack) mixed with 2 floats in their own registers. */
NVN(fx_mix, int64_t, const void *p, int a, float x, uint64_t b, double y, int c, int d, int e, int f, mode_t_ m, int h) {
    return (int64_t)(uintptr_t)p + a * 3 + (int64_t)(x * 100.0f) + (int64_t)b + (int64_t)(y * 1000.0) + c + d + e + f
         + m * 10000 + h * 100000;
}
NVN(fx_mul, float, float a, float b) { return a * b; }
NVN(fx_small, small_t, small_t s, int k) { small_t r = { s.v * 2 + (uint64_t)k }; return r; }
NVN(fx_big, big_t, int k) { big_t r = { { (uint64_t)k, (uint64_t)k + 1, (uint64_t)k + 2, (uint64_t)k + 3 } }; return r; }
NVN(fx_flag, uint8_t, uint32_t v) { return v > 5; }
NVN(fx_void, void, int *out, int v) { *out = v; }
