# WinTears: Direct Native plan

Goal: run the game's ARM64 code as x86-64 code translated **ahead of time**, with no JIT at run time. In the vault's
terms this moves wintears from Semi Native (dynarmic JIT) to Direct Native. Everything else stays: the Horizon host,
the NVN-to-Vulkan backend, the loader, the frontend.

This replaces the narrower phase 9 in `PORTING_PLAN.md` (hot functions only).

## Rule that shapes the design

The translated code is derived from the game, like `TOTK_win/`. It is made **on the player's machine** from their own
1.0.0 dump by the build step, and never committed, uploaded or put in a release. The repo ships the recompiler, not
its output.

## Why, from the measurements so far

- **JIT memory.** Three 128 MiB code caches (one per virtual core) committed, about 130 MiB of them resident on the
  8 GB laptop, plus dynarmic's IR slabs. Ahead-of-time code is a file mapping that Windows can drop and re-read.
- **JIT compile work.** The laptop log shows 2.24 million blocks compiled in one session: each core compiles the
  same code again, and full caches are cleared and refilled. That is CPU time and stutter that ahead-of-time code
  does not have.
- **Wine.** Under Proton, JIT code needs writable-and-executable memory and invalidation that cost more than on
  Windows. Ahead-of-time code is an ordinary DLL.
- **What it will not fix.** On the laptop the GPU is the limit (55 ms a frame, the game waits 5.8 s in every 10 s
  for it). Direct Native lowers memory and stutter there, not the steady frame rate. On the Steam Deck the CPU share
  has to be measured first (phase 0).

## Approach

**A static recompiler that emits C, built on dynarmic's own A64 front end.** dynarmic is already in the tree and
tested: its decoder and IR emitter give exact semantics for every instruction the game uses. The new part is a
back end that turns that IR into C instead of x86 machine code. The C is compiled with the MSYS2 GCC/Clang the
project already uses, into a DLL.

Alternatives considered:

- LLVM lifter (Remill-style): a large new dependency and slower builds, for the same result.
- A new decoder: repeats work dynarmic has done and tested.
- Emitting x86 directly: loses the C compiler's register allocation and optimisation across a whole function.

## Architecture

```
 build time (player's PC)                          run time
 TOTK_win/{main,sdk,subsdk0} ─► tools/recomp ─► C ─► gcc/clang ─► TOTK_win/native/totk_code.dll
                                  │                                  │
                         dynarmic A64 front end             src/cpu/native (same jit.h API)
                         + function discovery               dispatcher ─► recompiled functions
                         + IR→C back end                                └► JIT for misses (hybrid only)
```

- **Function discovery.** Recursive descent from: symbols (sdk 27,761, subsdk0 11,622, main only 704), every `BL`
  target, relocations that point into code (vtables and function pointers), `.eh_frame` when present, and jump
  tables (the `adr` / `ldr[bh]` / `add` / `br` pattern). Addresses found missing at run time are logged and fed back
  into the next build.
- **Code shape.** One C function per guest function, `void g_3001234a0(ctx *c)`, guest basic blocks as labels.
  Guest registers live in a context struct like `jit_context`; within a function the compiler keeps hot ones in host
  registers. Memory accesses are direct pointers (guest memory is already identity-mapped below 2^39).
- **Calls and returns.** `BL` becomes a direct C call followed by a check that the callee returned to the expected
  address. `RET` returns from the C function. `BR` / `BLR` and failed checks go through the dispatcher, a table from
  guest address to host function. A miss goes to the JIT (hybrid mode) or stops with the address logged (native mode).
- **System instructions.** `svc` calls the host's handler directly instead of stopping the core. `mrs tpidr_el0`,
  `tpidrro_el0` and `cntpct_el0` read the context or the host clock. `LDXR` / `STXR` use a host compare-exchange
  (dynarmic's exclusive-monitor model); `LDAR` / `STLR` / `DMB` become C11 atomics and fences.
- **Floating point and NEON.** First as calls into helpers that follow dynarmic's semantics (FPCR rounding, flush to
  zero, default NaN); hot ones later become inline SSE/AVX2. Accuracy settings match what the JIT uses today.
- **Threads.** Today a guest thread runs on a `jit_core` per virtual core and is preempted with `jit_halt` at block
  boundaries. Recompiled code polls a per-thread flag at loop back-edges and function entries, so the 3-core owner
  model in `src/host/threads.c` keeps working unchanged. Upstream MNG's native (Mac) path is the reference for game
  code on host threads.
- **Integration.** `src/cpu/native/` implements the same `jit.h` interface, so host code does not change. Selected
  with `TOTK_CPU=jit|hybrid|native`. Host overrides keep working: the loader still binds imports to host stubs, and
  calls through PLT slots reach them through the dispatcher.
- **Build step.** `tools/build_win.py --native` runs the recompiler and compiler. The DLL records a hash of the NSOs
  it came from; wintears.exe refuses a DLL that does not match the mapped code. Game patches are applied before
  recompiling, not at run time.

## Verification

- **Differential tests.** For every recompiled function: random register and memory state, run it once in the JIT and
  once recompiled, compare registers, flags and written memory. Part of the build's `--check`.
- **Instruction tests.** The existing 39 JIT checks run against the native back end too.
- **Boot comparison.** Scripted input (`TOTK_INPUT_SCRIPT`) to the title and into a save, in JIT and in native mode:
  same svc sequence per thread at start-up, same draw counts per frame, no dispatcher misses.
- **Play.** The `docs/TESTING.md` checklist on PC, Steam Deck and the laptop.

## Phases

| Phase | Work | Exit | Estimate |
|---|---|---|---|
| 0. Measure | Deck and PC profile: time in guest code, JIT compiling, host. Code sizes, function count, indirect branches | go / no-go with numbers | 1 week |
| 1. Recompiler skeleton | dynarmic front end → C for integer and memory instructions; function discovery; subsdk0 compiles and links | a DLL that loads | 3–4 weeks |
| 2. Hybrid mode | dispatcher, JIT fallback, svc / system registers / exclusives, thread polling | title screen in hybrid, miss log | 3–4 weeks |
| 3. Full instruction set | FP and NEON helpers; differential tests over every function | all functions pass | 4–6 weeks |
| 4. Full coverage | jump tables, misses from play fed back | native mode plays with no misses | 2–4 weeks |
| 5. Performance | inline NEON, fewer return checks where provably safe, profile-guided and link-time optimisation | faster and smaller than the JIT on PC, Deck, laptop | 2–4 weeks |
| 6. Direct Native | native mode by default; dynarmic left out of the native build once nothing falls back | 100% Direct Native | 1 week |

Total about 4–6 months at the current pace. Each phase ships: hybrid mode is useful on its own, because every
function it covers is code the JIT no longer compiles.

## Phase 0 results

### Static survey (2026-10-26, `tools/recomp/survey.py`, 1.0.0)

| | main | sdk | subsdk0 | all |
|---|---|---|---|---|
| Code | 42.7 MiB | 5.9 MiB | 3.3 MiB | 51.8 MiB, 13.58 M instructions |
| Function starts found | 243,446 | 38,305 | 13,690 | 295,441 |
| of them first from `.eh_frame` | 242,832 | 8,193 | 1,711 | |
| Code reached by recursive descent | 94.6% | 92.2% | 93.7% | |
| Switch tables (`br` after a table load) | 2,038 | 623 | 323 | 2,984 |
| Import stubs / veneers (`br x16/x17`) | 643 | 8,911 | 3,911 | 13,465 |
| Pointer tail calls (`ldr xN, [..]; br xN`) | 20,886 | 1,731 | 156 | 22,773 |
| Other `br` | 2,294 | 67 | 45 | 2,406 |
| Indirect calls (`blr`) | 104,749 | 12,665 | 3,833 | 121,247 |
| `svc` | 0 | 60 | 0 | 60 |
| Atomic / ordered memory instructions | 65,376 | 4,323 | 84 | 69,783 |
| SIMD / FP share | 10.4% | 3.3% | 1.0% | |

What it means:

- **Function discovery is mostly solved.** main keeps a full `.eh_frame` (one entry per function) although it has only
  704 symbols. With symbols and `BL` targets, a plain recursive descent reaches 92–95% of the code before switch
  tables are resolved. The rest (about 730,000 non-zero words) is switch-table targets, functions reached only through
  pointers, and data; hybrid mode covers it until it is found.
- **Indirect control flow is common.** About 144,000 indirect calls and pointer tail calls (virtual calls). The
  dispatcher's lookup is on a hot path: it needs a direct-mapped table, not a hash map.
- **System instructions are rare.** All 60 `svc` are in the SDK; atomics are 0.5% of instructions.
- **Size.** At a few C lines per instruction, 13.6 M instructions make tens of millions of lines; compile time is
  measured first thing in phase 1.

### PC run (2026-10-26, desktop PC, 5.5 minutes, `TOTK_DIAG=1 TOTK_SAMPLE=1`, scripted input into the save)

- 30.0 fps throughout (the game's cap); the three virtual cores were free about 79% of the time.
- **Start-up:** 9.4 s of JIT compiling (728,000 blocks) in the first 10-second window.
- **After that:** compiling is 0–100 ms per 10 s (under 0.4% of the cores' time), small bursts when new areas load.
- **Steady state, per 10 s of the three cores (30 core-s):** guest code about 3.5 s (12%), svcs and host calls about
  2.7 s, free about 23.6 s.
- **JIT code caches resident:** about 220 MiB (89 + 67 + 65 MiB).

On the PC the CPU is not the limit: Direct Native would remove about 9 s of start-up compiling, the small compile
bursts and about 220 MiB of memory, not raise the frame rate. The decision rests on the Steam Deck run: its CPU is
slower, Wine makes JIT memory handling dearer, and an earlier Deck log (commit `9f7a9c4`) showed the cores inside
guest code for 2–3 cores' worth of wall time.

### Run-time split (Steam Deck run still needed)

The 10-second `waits` line (with `TOTK_DIAG=1`) now also shows **JIT compiling** time and blocks, next to the core-time
split "guest code (compiling included) / free / handing over / svcs and host calls". Wanted: about 5 minutes of play
on the PC and on the Steam Deck, title screen and the Great Sky Island, with `TOTK_DIAG=1`. Go / no-go from those:

- **Go** if JIT compiling plus guest code is a large share of the cores' busy time on the Deck, or compiling shows up
  as stutter (long 10-second windows with high compile time).
- **No-go (stop at hybrid or not at all)** if guest code is a small share and compiling is negligible after the first
  minutes: then only the JIT's memory (about 384 MiB committed) would be gained.

## Risks

- **Build time and size.** Three modules, 51.8 MiB of code (phase 0). The C output
  is split into many files and compiled in parallel at `-O1`, expected tens of minutes on a desktop PC. The Steam Deck
  gets a DLL built on the PC from the same dump; the hash check keeps them matched.
- **Indirect branches.** Jump tables and function pointers missed by discovery. Hybrid mode makes these misses
  harmless while coverage grows.
- **Floating-point accuracy.** ARM and x86 differ on NaNs and flush-to-zero. Helpers follow dynarmic, and the
  differential tests catch differences.
- **Game updates.** Each game version is recompiled separately; only 1.0.0 is supported now.
- **Effort against gain.** Phase 0 can end the project: if guest code and JIT work are a small share of frame time on
  the Deck, the memory gain alone may not be worth phases 3–5.
