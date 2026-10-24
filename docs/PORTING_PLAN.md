# WinTears: porting plan

Goal: play your own copy of *The Legend of Zelda: Tears of the Kingdom* on Windows, using the approach of
[MacNativeGames](https://github.com/ly05010419/macnativegames) (MNG): no full Switch emulation, the Switch system
services answered by a small host, and the game's NVN graphics calls answered by a native graphics API.

Upstream reference: MNG at commit `4fdda41562a1c783fb1aacd18891ad4f48b8c360` (2026-10-21).

## Status

| Phase | State |
|---|---|
| 0. Repository and CI | done: submodules, CMake cross-build, CI (Linux + Wine, native Windows, Python tools) |
| 1. Build pipeline | done: `tools/build_win.py` extracts and verifies 1.0.0 code and data, including NSPs with unencrypted sections (`tools/upstream_patches.py`) and re-packed RomFS (`tools/romfs_rebuild.py`, kept only on an exact 1.0.0 hash match). Fonts, overlays, `TOTK_win/` and `--check --full` all pass |
| 2. Platform layer | done for bring-up: `src/pal`, 56 checks |
| 3A. CPU (JIT) | done: `src/cpu/jit` (39 checks), `src/bridge` (422 NVN thunks, 17 checks), `src/loader` (43 checks), `src/host` (Horizon host: svcs, IPC, overrides, RomFS, audio) — the boot probe runs the SDK start-up, the game's `nnMain`, and its main loop |
| 4. Frontend | mostly done: SDL3 window, keyboard + XInput/PlayStation/Switch Pro controllers, WASAPI audio, F11 fullscreen. Left: rumble, gyro, software keyboard, a controller layout file |
| 5. Vulkan backend | done with leftovers: device, guest pools, textures, clears, copies, events/counters, present, and draws/dispatches via run-time shader translation (`wt_xlate.exe`) on Vulkan 1.3 dynamic state. Layered rendering, buffer textures and masked clears are in (2026-10-24, untested in play). Left: 24-bit depth uploads, rendered-texture write-back (InvalidateMappedRange), occlusion queries (always visible), instance divisor, one 111 MiB unimportable pool, background translation, a performance pass |
| 6. In-game correctness | in progress: playable at ~30 fps with sound, cutscenes play (`src/host/movie.c`, libvpx VP9), saves are handled. Open: a black skybox in some scenes. Left: MNG game patches (`game_patches.c`), guest sync tests, the `long`/`unsigned long` audit |
| 7. Performance and polish | started: `tools/prepare_shaders.py` translates the shaders ahead of play; per-frame barriers cut by inlining uniform updates (~510→285). Left: FSR, `VK_EXT_graphics_pipeline_library`, the 60 FPS patch, the HUD overlay, the mods folder |
| 8. Packaging and release | not started |
| 9. Ahead-of-time translation (optional) | not started |

## Decisions

- **CPU track: A (x86-64, dynarmic JIT).** The test PCs are Intel/AMD.
- **Test GPUs: NVIDIA RTX 4070 and RTX 3080.** Driver-specific fixes are checked on NVIDIA first.
- **Low-end target for later: a laptop with integrated AMD Radeon graphics.** Not a bring-up target, but the design
  keeps it reachable (see "Low-end budget" below).
- **Bring-up version: 1.0.0** (base game only, no update merge).
- **Upstream as git submodules** (MNG and dynarmic, pinned), not a subtree: lighter, and upstream stays untouched.
  Files we must change are reimplemented under `src/` or patched at build time.

---

## 1. Ground rules

- **You must own the game.** Use only a game image you dumped yourself and `prod.keys` from your own Switch.
- The repository never contains game code, assets, shaders, saves or keys. The build makes everything from the
  player's own files on the player's machine. The resulting game folder is for personal use and must not be shared.
- Not affiliated with Nintendo. Upstream MNG code is MIT-licensed (and its third-party parts MIT / zlib / Apache-2.0).
  Reused code keeps its license file and is listed in `THIRD_PARTY_NOTICES.md`.

## 2. Scope

| In scope (v1) | Later | Out of scope |
|---|---|---|
| TOTK 1.0.0 (bring-up), then 1.4.3 | 1.1.2 / 1.2.1, Breath of the Wild | Online features, DLC beyond what MNG supports |
| Windows 11, x86-64 (Intel/AMD + NVIDIA/AMD/Intel GPU) | Windows on ARM (Snapdragon X) as a second track | Windows 10 before 1803, 32-bit |
| Vulkan 1.3 | Direct3D 12 fallback (only if a driver forces it) | OpenGL |
| Keyboard, XInput/PlayStation/Switch Pro controllers | Gyro aiming, HD rumble | Touch, iOS |

## 3. Two CPU tracks

MNG runs the game's ARM64 code directly because Apple chips are ARM64. Windows PCs come in two kinds:

- **Track A, x86-64 (default, most PCs).** The game's code runs through an ARM64-to-x86 JIT
  ([dynarmic](https://github.com/merryhime/dynarmic), 0BSD; use a maintained fork). MNG's instruction rewriter is not
  needed: the JIT handles `svc`, the thread registers and the counter registers through callbacks.
- **Track B, Windows on ARM.** The game's code runs natively, as on the Mac. MNG's rewriter is kept with a small change
  (section 6.3B).

Everything else (platform layer, Horizon services, graphics, frontend, build) is shared. Track A is planned first;
Track B is a few weeks of extra work once the shared parts exist.

Full ahead-of-time translation of the game to x86 is **not** planned. It is reconsidered only in phase 9, and only
for hot functions that profiling shows are the bottleneck.

## 4. Architecture

```
            ┌──────────────────────── wintears.exe ─────────────────────────┐
 game code  │  CPU backend                                                  │
 (main/sdk/ │   Track A: dynarmic JIT        Track B: native (rewritten)    │
  subsdk0)  │        │  svc / host calls / thread registers / counters      │
            │        ▼                                                      │
            │  Guest↔host bridge  (calling-convention thunks, generated)    │
            │        ▼                                                      │
            │  Horizon host (from MNG runtime/): threads, IPC, fs, time,    │
            │  account, settings, audio renderer, movies, swkbd, input      │
            │        │                         │                            │
            │        ▼                         ▼                            │
            │  NVN layer (from MNG hle_nvn.c)  Frontend (SDL3): window,     │
            │        ▼                          input, audio out, dialogs   │
            │  Vulkan backend (new)                                         │
            │        ▼                                                      │
            │  Platform layer (PAL): memory, threads, faults, time, files   │
            └───────────────────────────────────────────────────────────────┘
 shaders: Maxwell → totk_xlate (Ryujinx translator, win-x64/arm64 NativeAOT) → SPIR-V → Vulkan
```

### Key design decisions

1. **Same addresses for game and host.** Guest memory is mapped at the same virtual addresses in the Windows process
   (all below 2^39, the limit the SDK's IPC encoding needs). The host code can then dereference guest pointers
   directly, as MNG does, and dynarmic's fastmem base is 0. The exe is linked at a high base address so it never
   collides with the guest range.
2. **Guest memory is backed by section objects** (`CreateFileMapping2` + `MapViewOfFile3` + `VirtualAlloc2`
   placeholders), because Horizon lets the same physical memory appear at two addresses (MNG uses `mach_vm_remap`).
3. **Vulkan, not Direct3D 12.** The translator already produces SPIR-V, so MNG's SPIR-V-to-Metal step disappears.
4. **SDL3 for window, input and audio.** One dependency covers controllers, rumble, gyro and WASAPI.
5. **Stay close to upstream.** MNG changes daily. Platform-specific code sits behind a thin interface so the
   Horizon host and NVN layer can take upstream fixes with small, mechanical merges.

## 5. Repository layout

```
wintears/
  upstream/            MNG, pinned git submodule (never edited)
  externals/           pinned: dynarmic, SDL3, volk, VMA, opus, astc-encoder, Vulkan-Headers
  src/
    pal/               platform layer (Windows; a POSIX shim for tests)
    cpu/jit/           Track A, dynarmic A64→x86
    bridge/            generated guest↔host thunks + generator
    loader/            maps the original NSO modules, relocations, linking
    host/              Horizon host (services, fs, accounts, input, movie)
    gfx/               NVN layer (`nvn_*.c`) + Vulkan backend (`vk/`) + texture decoders
    frontend/          SDL3 window, input, audio, text entry
    app/               wintears.exe entry point and boot sequence
  tools/               build_app (Windows), shader seed, diagnostics
  tests/               game-free tests (run in CI)
  docs/                this plan, TESTING.md, design notes
```

## 6. Phases

Estimates assume Claude writes the code and you test on your PC about once a day. "Exit" is what must work before
the phase counts as done. Phases 1, 2 and 4 can run in parallel. The critical path is 0 → 2 → 3 → 5 → 6.

### Phase 0: repository and CI (2–4 days)

- Import MNG as a subtree under `upstream/`, add `THIRD_PARTY_NOTICES.md` and the license files.
- CMake + Ninja + clang build. Two toolchains: native Windows (clang-cl + Windows SDK) and cross-builds from Linux
  (llvm-mingw) so CI can build without a Windows machine.
- GitHub Actions: Linux cross-build, unit tests under Wine, Windows x64 build; Windows ARM64 runner where available.
- Port MNG's game-free Python tests (`tests/`) and make them pass on Windows paths.
- **Exit:** a push builds an empty `wintears.exe` for x64 and ARM64, and all game-free tests pass in CI.

### Phase 1: build pipeline on Windows (1–2 weeks, parallel)

- Port `build_app.py`: XCI/NSP/NSZ import, ExeFS/RomFS extraction and IVFC verification, shared fonts, RomFS
  overlays, version detection by exact hashes. These are mostly Python already.
- Replace macOS-only steps: `xcrun`/`codesign`/`lsregister`/APFS `clonefile` → plain folder output; use ReFS block
  cloning (Dev Drive) when available, else hard links or a copy.
- Build hactool on Windows (pinned commit, with MNG's title-key-file and bounded-input hooks).
- Build `totk_xlate` with .NET NativeAOT for `win-x64` and `win-arm64`; ship it prebuilt with a checked SHA-256,
  as MNG does.
- Output: `TOTK_win/` with the exe, data, shader seed, and a launcher config pointing at `%LOCALAPPDATA%\WinTears\TOTK`
  (saves, shader cache, logs, mods).
- **You test:** run the build on your files once.
- **Exit:** the RomFS and ExeFS hashes match MNG's expected manifests (`assets/expected-v1.0.0.json`).

### Phase 2: platform layer (1–2 weeks)

| macOS (MNG) | Windows (PAL) |
|---|---|
| `mach_vm_allocate` / `mmap` fixed | `VirtualAlloc2` with placeholders at fixed addresses |
| `mach_vm_remap` (alias views) | section object + `MapViewOfFile3` |
| `mprotect` | `VirtualProtect` (batched: it is slower than `mprotect`) |
| `mach_vm_read_overwrite` (safe reads) | guarded read via the fault handler (`__try`/VEH) |
| `sigaction` SIGSEGV/SIGBUS/SIGILL | `AddVectoredExceptionHandler` |
| `pthread_kill(SIGUSR2)` preemption | special user APC (`QueueUserAPC2`), or dynarmic halt requests on Track A |
| `pthread_*`, `os_unfair_lock` | Win32 threads, `SRWLOCK`, `CONDITION_VARIABLE` |
| `__ulock_wait/wake` | `WaitOnAddress` / `WakeByAddress*` |
| `dispatch_*` | small thread pool + work queue |
| `mach_absolute_time` | `QueryPerformanceCounter` |
| `pthread_set_qos_class_self_np` | `SetThreadPriority` + `AvSetMmThreadCharacteristics` (audio) |
| `~/Library/Application Support` | `%LOCALAPPDATA%\WinTears` |

Also:

- **`long` audit.** `long` is 32-bit on Windows and 64-bit on macOS. Replace the ~540 plain `long`/`unsigned long`
  uses in reused code with fixed-width types, starting with every struct that mirrors guest memory. Add
  `static_assert`s on guest struct sizes.
- **Stack limits.** Guest threads run on their own small stacks. Set the TEB stack range when a guest thread starts,
  and keep fault handling off small stacks, because Windows has no alternate signal stack.
- **Dirty-page tracking** (MNG's `nvn_dirty.c`): write-protect and catch the fault in the vectored handler. Measure
  the cost of `VirtualProtect` with 4 KB pages early; fall back to coarser regions if needed.
- Crash reports: `MiniDumpWriteDump` + a readable log of guest registers and the guest call chain.
- **Exit:** PAL unit tests pass under Wine and on Windows: fixed reservation below 2^39, alias views see each
  other's writes, protect→fault→unprotect, `WaitOnAddress` stress, preemption of a busy thread.

### Phase 3A: CPU, x86-64 JIT (2–4 weeks)

- Integrate dynarmic (A64 frontend, x64 backend): one JIT state per guest thread, a shared code cache, fastmem with
  base 0, page-table fallback for unmapped access.
- Callbacks: `svc` → MNG's `runtime_svc`; `TPIDR_EL0`/`TPIDRRO_EL0` → per-thread values; `CNTPCT_EL0` → host timer
  scaled to 19.2 MHz; exclusive monitor (global, as yuzu does); cache maintenance ops ignored.
- Keep MNG's loader (map main/sdk/subsdk0 at original layout, relocations, imports) but load the **original,
  unrewritten** modules plus MNG's fixed game patches (frame timing, 60 FPS, resolution), which still check the
  original instructions first.
- **Guest↔host bridge:**
  - Every host function the game reaches (SDK overrides, the 521 NVN entry points from `nvn_protos.h`, fonts, codecs,
    movie, swkbd) gets a small guest-side stub (`svc #0xF000+n; ret`) and a host thunk that converts AArch64
    arguments (x0–x7, v0–v7, stack) to the Win64 x64 convention and back. Thunks are generated from a signature table.
  - `nvnDeviceGetProcAddress` and similar return stub addresses, never host pointers.
  - Host → guest calls (thread entry, callbacks the game registers) run the JIT at the guest address.
- Port MNG's virtual-core scheduler (`threads.c`): preemption by dynarmic halt requests instead of signals.
- **You test:** run with graphics stubbed out and send the log.
- **Exit:** the game reaches `nn::init::Start`, starts its threads, reads RomFS files and issues its first NVN calls;
  the loader/boot verification scripts pass.

### Phase 3B: CPU, Windows on ARM (1–3 weeks, after 3A or instead of it)

- `patch_nso.py`: the thread-slot stubs read `mrs xN, tpidrro_el0` (the macOS TSD base). On Windows x18 is the
  thread block (TEB), and the game's own x18 uses are already rewritten away, so the stubs use `x18` instead, and
  the key page stores `TlsSlots` offsets (`0x1480 + index*8` on 64-bit Windows; verify).
- Convert the Apple-syntax assembly (`gate.S`, `hle_stack.S`, `thread_enter.S`, and the generator of `nvn_thunks.S`):
  `@PAGE/@PAGEOFF` → `:lo12:`, no leading underscores, COFF sections.
- Re-check variadic host functions: Windows ARM64 passes variadic floats in integer registers.
- Disable Control Flow Guard for the exe (or register the guest code as valid call targets).
- Re-run MNG's rewrite verification (site-by-site against an emulator) with the changed stubs.
- **Exit:** same as 3A, on a Snapdragon X machine.

### Phase 4: frontend (1–2 weeks, parallel)

- SDL3 window (windowed, borderless fullscreen, resize, docked/handheld resolution), F-key shortcuts as in MNG.
- Input: keyboard map, SDL gamepads (Xbox, PlayStation, Switch Pro, Joy-Con), rumble, gyro → MNG's `input.c` /
  `motion_fusion.c`.
- Audio: SDL3 or WASAPI output behind MNG's `audio_mixer.c`; libopus and libvpx built for Windows.
- Software keyboard: an SDL text-entry bar replacing the AppKit one.
- **Exit:** with graphics stubbed, title music plays and controller input reaches the game's HID state.

### Phase 5: Vulkan backend to the title screen (3–6 weeks)

Rebuild what `nvn_mtl.m` (~14k lines, Metal 4) does, on Vulkan 1.3 (dynamic rendering, synchronization2,
descriptor buffers or descriptor indexing, timeline semaphores):

- Device, queues, swapchain, frame pacing, present interval.
- Memory pools → `VkDeviceMemory` + `VkBuffer`, GPU addresses (buffer device address), CPU writes found through
  dirty-page tracking and uploaded before use.
- Textures: block-linear deswizzle (reuse `nvn_deswizzle.c`, port the GPU path to a compute shader), formats,
  views, samplers, mip/array layouts (reuse `nvn_tex.c`, which carries the SDK-exact layout fixes).
- **ASTC.** Most TOTK textures are ASTC. Apple and Snapdragon GPUs decode it natively, desktop GPUs do not. Add a
  compute-shader ASTC decoder (to RGBA8, optionally re-encoded to BC7 later to save VRAM), and use native ASTC when
  the device supports it.
- Render targets → rendering passes; clears, blits, resolves, copies, queries, compute dispatch, indirect draws.
- Shaders: change the `totk_xlate` service to emit SPIR-V + Vulkan reflection (descriptor sets/bindings, push
  constants), build pipelines from NVN state, `VkPipelineCache` on disk, background compilation.
- Port MNG's game-free GPU probes (texture layout, depth16 clear, copy alias, vertex counter) to Vulkan; several MNG
  fixes are Apple-GPU specific and must be re-checked, not copied blindly.
- **You test:** daily runs, screenshots of the title screen and menus.
- **Exit:** the title screen, main menu and first loading screen look right on your GPU.

### Phase 6: in-game correctness (4–10 weeks)

The longest phase: a loop of "you play, send log + screenshot; Claude fixes".

- Checklist: Great Sky Island start, cutscenes (VP9), first shrine, abilities (Ultrahand, Fuse, Recall, Ascend —
  the effects MNG had to fix), Hyrule surface, the Depths, weather, save/load, day/night, docked and handheld modes.
- Port relevant upstream fixes as they land.
- **Exit:** Great Sky Island to Lookout Landing playable with correct picture, sound and saves; no known crash.

### Phase 7: performance and polish (4–8 weeks)

- Shader pre-warming from your own files: SPIR-V seed at build time, pipeline compilation behind the title screen,
  `VK_EXT_graphics_pipeline_library` to cut first-use stutter. Support MNG's shared shader list (identifiers only).
- Profile in heavy areas (Lookout Landing, the Depths, sky islands) with PIX/RenderDoc/Tracy. Decide from the data
  whether the CPU side needs phase 9.
- Upscaling: FSR 1 first, FSR 2/3 later (open source); MNG's render-resolution options, 60 FPS patch, ultrawide.
- FPS/shader/load overlay, mods folder (MNG's manifest format).
- **Exit:** a stable target frame rate on your PC in the checklist areas, and stutter limited to new shaders.

### Phase 8: packaging and release (≈1 week)

- One command: `python build_win.py --game <xci/nsp> --keys prod.keys [--update <nsp>]` → `TOTK_win/`.
- Start-menu shortcut, `--check` self-verification, README, troubleshooting, and the "what to send in a bug report"
  guide.
- **Exit:** a fresh Windows machine goes from game file to playing by following the README.

### Phase 9 (optional): ahead-of-time translation of hot code

Only if phase 7 profiling shows guest CPU code limits the frame rate. Translate the hottest functions ahead of time
(ARM64 → LLVM IR → x86) and keep the JIT for everything else. Expected gain: tens of percent of the game-code time,
not multiples. Estimate: 2–4 months; skipped unless the numbers justify it.

## 7. Timeline

| Milestone | Cumulative estimate |
|---|---|
| Builds in CI, pipeline produces verified data (phases 0–1) | ~2 weeks |
| Game boots without graphics (phases 2–3A) | ~1–2 months |
| Title screen (phase 5) | ~2–3 months |
| Playable early game (phase 6) | **~2.5–5 months** |
| Close to the Mac version (phase 7) | ~3.5–7 months |
| Windows on ARM track (3B) | +2–4 weeks after the shared parts |

Testing once a day is assumed. Testing a few times a week stretches this by about 2–3×.

## 8. Testing

- **CI (no game data):** unit tests, PAL tests, bridge-thunk round trips, JIT instruction tests, texture/ASTC
  decoders against reference images, GPU probes on a software Vulkan driver (lavapipe).
- **Your PC (with your game):** each build prints its version; you run it, then send `latest.log`, any crash dump,
  and a screenshot. `docs/TESTING.md` (phase 0) will list the exact steps and which logs to send.
- Game files, keys and saves never leave your machine and are never uploaded to the repo or to CI.

## 9. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Guest range below 2^39 collides with Windows DLLs or ASLR | game cannot load | reserve early at startup, high exe base, shrink the alias region as MNG does on macOS 27 |
| `VirtualProtect` too slow for dirty tracking | low frame rate | batch calls, coarser regions, hash-compare small buffers |
| ASTC decode cost / VRAM use on desktop GPUs | stutter, memory | GPU compute decoder, cache decoded textures, BC7 re-encode later |
| Bridge overhead on thousands of NVN calls per frame | CPU time | generated tight thunks, batch-friendly NVN paths |
| Exclusive monitor (LDXR/STXR) approximations | rare hangs or corruption | yuzu's proven global monitor; stress tests |
| Vulkan driver differences (NVIDIA/AMD/Intel/Qualcomm) | visual bugs on some GPUs | test on your GPU first, gate fixes per vendor |
| Upstream MNG moves fast | merge pain | thin platform interface, numbered patches, regular syncs |
| Bugs only reproducible with game data | slow fixes | detailed logs, frame captures (RenderDoc), replayable input |

## 10. Low-end budget (integrated AMD Radeon, later)

Decisions made now so the integrated-GPU laptop stays reachable without a rewrite:

- **Memory is shared with the CPU.** Keep GPU copies of guest memory pools small: upload only dirty ranges, no
  second full copy of each pool. Prefer `VK_EXT_external_memory_host` (import guest pages directly) where the driver
  offers it, as AMD's does.
- **ASTC costs bandwidth and memory.** Decoding to RGBA8 is 4–16× larger than ASTC. The texture path keeps a
  "transcode to BC7/BC3" option behind the same interface, for when VRAM or bandwidth is the limit.
- **Resolution first.** Render at 720p–900p internally with FSR 1 (cheap, no motion vectors) on the iGPU; FSR 2/3
  only on the discrete GPUs.
- **Shader stutter hurts more on a slower CPU.** Pre-warming at build time and `VK_EXT_graphics_pipeline_library`
  matter more here; the pipeline cache is keyed per GPU and driver.
- **CPU time.** The laptop's CPU is the likely bottleneck for the JIT: keep dynarmic's code cache warm, avoid
  per-frame `VirtualProtect` storms (batch dirty tracking), and keep the host-call bridge thunks tight.
- A `--profile low` switch will bundle these settings once phase 7 exists.

## 11. Open decisions

1. **License for new code:** MIT, matching upstream (planned).
