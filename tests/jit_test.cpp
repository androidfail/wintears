// JIT tests: ARM64 programs (tests/guest/programs.S) run through dynarmic in identity-mapped guest memory.
// No game data. Run on Windows or under Wine.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "guest/programs.h"
#include "jit/jit.h"
#include "pal.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

constexpr uint64_t kBase = 0x4000000000ull;   // below 2^39, like all Horizon guest memory
constexpr uint64_t kSize = 64ull << 20;
constexpr uint64_t kCode = kBase, kData = kBase + (1ull << 20), kStack = kBase + (32ull << 20);

static jit_monitor *monitor;

static jit_context start(uint64_t program) {
    jit_context c{};
    c.pc = kCode + program;
    c.sp = kStack;
    return c;
}

// Run until `svc #0xff`. on_svc(number, ctx) handles other svcs and may change registers.
template <typename F>
static int run(jit_core *core, jit_context &ctx, F on_svc) {
    jit_load(core, &ctx);
    for (;;) {
        int stop = jit_run(core);
        jit_save(core, &ctx);
        if (stop != JIT_STOP_SVC) return stop;
        uint32_t n = jit_svc_number(core);
        if (n == 0xff) return stop;
        on_svc(n, ctx);
        jit_load(core, &ctx);
    }
}
static int run(jit_core *core, jit_context &ctx) { return run(core, ctx, [](uint32_t, jit_context &) {}); }

static void test_basics(jit_core *core) {
    jit_context c = start(GUEST_P_SUM);
    CHECK(run(core, c) == JIT_STOP_SVC);
    CHECK(c.x[0] == 5050);
    CHECK(c.pc == kCode + GUEST_P_SVC);   // just past the final svc

    c = start(GUEST_P_SVC);
    uint64_t at_11 = 0;
    run(core, c, [&](uint32_t n, jit_context &x) {
        if (n == 0x10) x.x[0] += 100;
        if (n == 0x11) at_11 = x.x[0];
    });
    CHECK(at_11 == 102);
}

static void test_atomics() {
    // Four cores on four host threads increment one counter with LDAXR/STLXR through the shared monitor.
    auto *counter = reinterpret_cast<volatile uint32_t *>(kData);
    *counter = 0;
    std::thread t[4];
    std::atomic<int> ok{0};
    for (int i = 0; i < 4; i++) {
        t[i] = std::thread([i, &ok] {
            jit_config cfg{};
            cfg.fastmem = 1;
            jit_core *core = jit_core_create(&cfg, monitor, (size_t)i);
            jit_context c = start(GUEST_P_ATOMIC);
            c.x[0] = kData;
            c.x[1] = 100000;
            if (run(core, c) == JIT_STOP_SVC && c.x[1] == 0) ok++;
            jit_core_destroy(core);
        });
    }
    for (auto &x : t) x.join();
    CHECK(ok == 4);
    CHECK(*counter == 400000);
}

static void test_float(jit_core *core) {
    auto *r = reinterpret_cast<uint32_t *>(kData + 0x100);
    std::memset(r, 0xAA, 32);
    jit_context c = start(GUEST_P_FLOAT);
    c.x[0] = kData + 0x100;
    CHECK(run(core, c) == JIT_STOP_SVC);
    CHECK(r[0] == 0x7fffffffu);   // fcvtzs saturates
    CHECK(r[1] == 0);             // fcvtzu of -1.0
    CHECK(r[2] == 0);             // fcvtzs of NaN
    CHECK(r[3] == 0x7fc00000u);   // fmin with NaN: NaN
    CHECK(r[4] == 0x4f32d05eu);   // fminnm with NaN: the number
    CHECK(r[5] == 0);             // udiv by zero
    CHECK(r[6] == 0x80000000u);   // INT_MIN / -1
    CHECK(r[7] == 1);             // rbit
}

static void test_neon(jit_core *core) {
    auto *in = reinterpret_cast<uint8_t *>(kData + 0x200), *idx = in + 16, *out = in + 32;
    for (int i = 0; i < 16; i++) { in[i] = (uint8_t)(i * 17); idx[i] = (uint8_t)(15 - i); }
    idx[0] = 200;   // out of range: 0
    jit_context c = start(GUEST_P_NEON);
    c.x[0] = (uint64_t)(uintptr_t)in; c.x[1] = (uint64_t)(uintptr_t)idx; c.x[2] = (uint64_t)(uintptr_t)out;
    CHECK(run(core, c) == JIT_STOP_SVC);
    int tbl_ok = out[0] == 0, sat_ok = 1;
    for (int i = 1; i < 16; i++) tbl_ok &= out[i] == in[15 - i];
    for (int i = 0; i < 16; i++) { int s = in[i] * 2; sat_ok &= out[16 + i] == (s > 255 ? 255 : s); }
    CHECK(tbl_ok);
    CHECK(sat_ok);
}

static void test_sysregs(jit_core *core) {
    jit_context c = start(GUEST_P_SYSREGS);
    c.tpidrro_el0 = 0x1234; c.tpidr_el0 = 0x5678; c.x[2] = 0x9abc;
    CHECK(run(core, c) == JIT_STOP_SVC);
    CHECK(c.x[0] == 0x1234 && c.x[1] == 0x5678);
    CHECK(c.tpidr_el0 == 0x9abc);
    CHECK(c.x[4] == 19200000);
    uint64_t first = c.x[3];
    pal_sleep_ns(2000000);
    c = start(GUEST_P_SYSREGS);
    run(core, c);
    CHECK(c.x[3] > first && c.x[3] - first >= 19200 * 1);   // at least ~1 ms of 19.2 MHz ticks
}

// A guest store into a write-protected page (dirty-page tracking) faults inside JIT code; the platform fault
// handler unprotects the page and the store completes.
static uintptr_t watched;
static std::atomic<int> watch_faults;
static int on_fault(uintptr_t addr, int kind, void *, void *) {
    if ((addr & ~(uintptr_t)4095) != watched || kind != PAL_FAULT_WRITE) return 0;
    watch_faults++;
    return pal_protect(watched, 4096, PAL_PROT_READ | PAL_PROT_WRITE) == 0;
}

static void test_tracked_store(jit_core *core) {
    watched = kData + 0x10000;
    CHECK(pal_fault_add(on_fault, nullptr) == 0);
    CHECK(pal_protect(watched, 4096, PAL_PROT_READ) == 0);
    jit_context c = start(GUEST_P_STORE);
    c.x[0] = watched + 8; c.x[1] = 0x1122334455667788ull;
    CHECK(run(core, c) == JIT_STOP_SVC);
    CHECK(*reinterpret_cast<volatile uint64_t *>(watched + 8) == 0x1122334455667788ull);
    CHECK(watch_faults == 1);
}

static void test_patch(jit_core *core) {
    jit_context c = start(GUEST_P_PATCH);
    run(core, c);
    CHECK(c.x[0] == 1);
    auto *insn = reinterpret_cast<uint32_t *>(kCode + GUEST_P_PATCH);
    *insn = 0xd2800040;   // mov x0, #2
    jit_invalidate(core, kCode + GUEST_P_PATCH, 4);
    c = start(GUEST_P_PATCH);
    run(core, c);
    CHECK(c.x[0] == 2);
}

static void test_halt_and_undefined(jit_core *core) {
    jit_context c = start(GUEST_P_SPIN);
    std::thread stopper([core] { pal_sleep_ns(20000000); jit_halt(core); });
    int stop = run(core, c);
    stopper.join();
    CHECK(stop == JIT_STOP_HALT);
    CHECK(c.x[0] > 1000);
    // the core runs again after a halt
    c = start(GUEST_P_SUM);
    CHECK(run(core, c) == JIT_STOP_SVC && c.x[0] == 5050);

    c = start(GUEST_P_UDF);
    uint64_t pc = 0;
    CHECK(run(core, c) == JIT_STOP_EXCEPTION);
    CHECK(jit_exception(core, &pc) == JIT_EXC_UNDEFINED);
    CHECK(pc == kCode + GUEST_P_UDF);
}

int main() {
    CHECK(pal_reserve_fixed(kBase, 1ull << 30) == 0);
    pal_section *s = pal_section_create(kSize);
    CHECK(pal_map_view(s, 0, kBase, kSize, PAL_PROT_READ | PAL_PROT_WRITE) == kBase);
    CHECK(pal_commit(kBase, kSize, PAL_PROT_READ | PAL_PROT_WRITE) == 0);
    std::memcpy(reinterpret_cast<void *>(kCode), guest_code, sizeof guest_code);

    monitor = jit_monitor_create(5);
    jit_config cfg{};
    cfg.fastmem = 1;
    jit_core *core = jit_core_create(&cfg, monitor, 4);
    test_basics(core);
    test_atomics();
    test_float(core);
    test_neon(core);
    test_sysregs(core);
    test_tracked_store(core);
    test_patch(core);
    test_halt_and_undefined(core);
    jit_core_destroy(core);
    jit_monitor_destroy(monitor);
    std::printf("jit_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
