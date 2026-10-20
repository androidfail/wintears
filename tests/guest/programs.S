// Small ARM64 test programs for the JIT tests (tests/jit_test.cpp). Every program ends with `svc #0xff`.
// Regenerate the header after editing: python3 tools/embed_guest.py tests/guest/programs.S tests/guest/programs.h
    .text
    .p2align 2

    .globl p_sum                // x0 = 1 + 2 + ... + 100
p_sum:
    mov x0, #0
    mov x1, #100
1:  add x0, x0, x1
    subs x1, x1, #1
    b.ne 1b
    svc #0xff

    .globl p_svc                // the host adds 100 to x0 on svc #0x10; x0 = 102 at svc #0x11
p_svc:
    mov x0, #1
    svc #0x10
    add x0, x0, #1
    svc #0x11
    svc #0xff

    .globl p_atomic             // x0 = counter address, x1 = iterations: exclusive-monitor increment loop
p_atomic:
1:  ldaxr w2, [x0]
    add w2, w2, #1
    stlxr w3, w2, [x0]
    cbnz w3, 1b
    subs x1, x1, #1
    b.ne 1b
    svc #0xff

    .globl p_float              // x0 = results (8 words): ARM semantics that differ from x86
p_float:
    movz w9, #0x4f32, lsl #16   // 3e9f (0x4f32d05e)
    movk w9, #0xd05e
    fmov s0, w9
    fcvtzs w1, s0               // out of range: saturates to 0x7fffffff (x86 cvttss2si gives 0x80000000)
    fmov s1, #-1.0
    fcvtzu w2, s1               // negative to unsigned: 0
    movz w9, #0x7fc0, lsl #16   // quiet NaN
    fmov s2, w9
    fcvtzs w3, s2               // NaN to int: 0
    fmin s3, s2, s0             // NaN in: NaN out
    fminnm s4, s2, s0           // NaN in, number wins: 3e9
    udiv w5, w1, wzr            // divide by zero: 0 (x86 traps)
    movz w10, #0x8000, lsl #16  // INT_MIN
    movn w11, #0                // -1
    sdiv w6, w10, w11           // INT_MIN / -1: INT_MIN (x86 traps)
    rbit w7, w10                // bit reverse: 1
    str w1, [x0]
    str w2, [x0, #4]
    str w3, [x0, #8]
    fmov w12, s3
    str w12, [x0, #12]
    fmov w12, s4
    str w12, [x0, #16]
    str w5, [x0, #20]
    str w6, [x0, #24]
    str w7, [x0, #28]
    svc #0xff

    .globl p_neon               // x0 = 16 input bytes, x1 = 16 index bytes, x2 = 32 output bytes
p_neon:
    ld1 {v0.16b}, [x0]
    ld1 {v1.16b}, [x1]
    tbl v2.16b, {v0.16b}, v1.16b    // table lookup; out-of-range index gives 0
    uqadd v3.16b, v0.16b, v0.16b    // unsigned saturating add
    st1 {v2.16b, v3.16b}, [x2]
    svc #0xff

    .globl p_sysregs            // thread registers and the counter
p_sysregs:
    mrs x0, tpidrro_el0
    mrs x1, tpidr_el0
    msr tpidr_el0, x2
    mrs x3, cntpct_el0
    mrs x4, cntfrq_el0
    svc #0xff

    .globl p_store              // x0 = address, x1 = value
p_store:
    str x1, [x0]
    svc #0xff

    .globl p_patch              // x0 = 1; the test rewrites the immediate and invalidates the translation
p_patch:
    mov x0, #1
    svc #0xff

    .globl p_spin               // counts in x0 until the host halts the core
p_spin:
1:  add x0, x0, #1
    b 1b

    .globl p_udf                // an undefined instruction
p_udf:
    udf #0
    svc #0xff

    .globl p_call               // call the function at x9 with the registers as set, then stop
p_call:
    blr x9
    svc #0xff

    .globl p_end
p_end:
