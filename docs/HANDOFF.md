# Handoff: state of the Windows port (2026-10-21)

For the next session (expected to be a local session on the user's Windows PC). Read this first, then
`docs/PORTING_PLAN.md` (the full plan, phases, decisions) and `docs/TESTING.md` (how the user runs things).

## The project in three lines

WinTears ports [MacNativeGames](https://github.com/ly05010419/macnativegames) (MNG: Tears of the Kingdom on Apple
silicon, not an emulator: the game's ARM64 code runs natively, a small host answers Horizon OS calls, NVN graphics
on Metal) to Windows. Windows PCs are x86-64, so the game's code runs through an ARM64-to-x86 JIT (dynarmic) instead
of natively; everything else follows MNG's design, with graphics on Vulkan.

## Where things stand

| Part | State | Tests |
|---|---|---|
| Repo, CMake, CI | done. Work on **`main`** (the user asked for that; `claude/porting-plan` is an old branch) | CI: Linux cross-build + Wine, native Windows (MSYS2), Python tools: all green |
| `src/pal` platform layer | done for bring-up: fixed reservations below 2^39 (placeholders), aliasable section views, protect + write-fault hook (VEH), guarded copy, threads, SRW locks, `WaitOnAddress`, timers, UTF-8 files | `tests/pal_test.c`, 56 checks |
| `src/cpu/jit` | dynarmic A64 wrapper: one core per virtual core, identity-mapped fastmem, exclusive monitor, svc/halt/exception stops, 19.2 MHz counter | `tests/jit_test.cpp`, 39 checks |
| `src/bridge` | guest stubs (`svc #0x1000+n; ret`) + generated typed thunks (AAPCS64 → Win64); 422 NVN thunks from upstream's `NVN(...)` definitions | `tests/bridge_test.c`, 17 checks |
| `tools/build_win.py` | extracts and verifies 1.0.0 ExeFS/RomFS/fonts/overlays with upstream's scripts, writes `TOTK_win/` + manifest, `--check`. Works on the user's real NSP | `tools/tests`, 11 tests |
| `tools/romfs_rebuild.py` | rewrites a RomFS image re-packed by a converter in Nintendo's layout; `build_win` keeps it only if it matches the verified 1.0.0 hash | `tools/tests/test_romfs_rebuild.py`, 6 tests |
| `tools/nsp_info.py` | describes a game file's NCAs and section encryption (no key values), with a verdict | `tools/tests/test_nsp_info.py`, 5 tests |
| `tools/upstream_patches.py` | exact text patches applied to the work copy of upstream's scripts (never to `upstream/`); now: NSPs with unencrypted sections (type 1) | `tools/tests/test_upstream_patches.py`, 6 tests |
| `src/loader` | maps the **original** NSO modules (LZ4) into guest memory, RELR/RELA/JMPREL relocations, cross-module linking with a resolver hook for host overrides, segment protections, address-to-module lookup; all parsed at run time, no generated tables | `tests/loader_test.c`, 43 checks (two synthetic NSOs, a JIT call through a resolved PLT slot) |
| `wintears.exe boot <TOTK_win>` | loads main/subsdk0/sdk at upstream's guest layout, runs the SDK's `nn::init::Start` in the JIT, logs svcs (`--trace`), stops at the first unimplemented one with registers and the guest call chain | run by hand on the real game |
| `src/host` (Horizon host) | ported from upstream `runtime/`: memory regions and svcs (`memory.c`), threads and sync on virtual cores with one `jit_core` each (`threads.c`), upstream `ipc.c` compiled unchanged, SDK overrides through bridge stubs (`overrides.c`; oe/aoc/diag/err/pctl and observers in `services.c`), RomFS backend and fs host (`romfs.c`, `fs_host.c`), local account (`account.c`); windowless `vi` with a 60 Hz vsync (`vi_stub.c`), the SDK's graphics entry points (`gfx_bridge.c`) | through the boot probe on the real game; no unit tests yet |
| `src/gfx` (NVN layer) | port of upstream `hle_nvn.c` + `totk_nvn_ext.c` (421 of upstream's 422 NVN functions, same signatures and object layouts; `nvn_obj.h`, `nvn_tex.c` used unchanged), each reached through its generated bridge thunk; physical / virtual pools as sections and placeholder views (`nvn_vpool.c`); the GPU backend interface (`nvn_backend.h`) with the **null backend** (`nvn_null.c`: event values, counter reports, buffer copies and uniform updates at submit, nothing drawn) | `tests/nvn_test.c`, 47 checks (recording lifetimes, memory accounting, deferred binds, submit effects, texture layout, physical / virtual pool aliasing); the game's graphics start-up on the real game |
| `src/frontend` | SDL3 window on the main thread (the game on its own thread), keyboard and controllers to the Switch controller, F11 fullscreen | by hand |
| `src/gfx/vk` (Vulkan backend) | device, guest pools as imported host memory (the heap as one import), NVN textures as images keyed by storage, uploads from pool memory with the ported dirty-page tracker (`src/gfx/nvn_dirty.c`), clears, copies, events / counters on a completion thread, present into the window; draws and dispatches with run-time shader translation (`tools/build_xlate.py` builds `wt_xlate.exe`: upstream's translator, `tools/wt_xlate/VkService.cs` for Vulkan output), pipelines with Vulkan 1.3 dynamic state, per-draw descriptor sets. `TOTK_SHOT=frame:path.png`, `TOTK_PASSLOG=frame` for checks | the real game: playable in a window at 30 fps (2026-10-21); `tools/prepare_shaders.py` translates the shaders before play |

## The user's NSP: unencrypted sections and a re-packed RomFS (solved)

History (2026-10-20, local session):

1. The first real run stopped in `extract_exefs.py` with `Only AES-CTR ExeFS is supported`.
2. `tools/nsp_info.py` on `Zelda.nsp`: Program NCA `b74b2a7f...nca` (expected name `9d827368...nca`), key area (no
   ticket), key generation 0; section 0 ExeFS, section 1 RomFS, section 2 logo PFS0, **all encryption type 1
   (none)**. Control and Meta NCAs are normal AES-CTR. So a conversion tool removed the section encryption.
3. Fixed: `tools/upstream_patches.py` patches the work copies of `extract_exefs.py` and `extract_romfs.py` to read
   type 1 sections as they are and derive the key only for AES-CTR sections. Every hash check is unchanged.
   Upstream PR with the same change: not done yet (outward-facing, ask the user).
4. Second run: **ExeFS passes** (main, rtld, sdk, subsdk0 byte-identical to 1.0.0; main.npdm "another genuine
   dump"). **RomFS: all six IVFC levels verify, but the image differs from 1.0.0:** size 17,069,139,636 instead of
   17,069,139,640, SHA-256 `2fc73227...` instead of `4751542f...`, 304,207 files. Not trailing zeros or table
   padding (both tested). Upstream's two per-file checks (`make_romfs_patches.py`: the Common.Product.100 layout
   archive and `DmF_SY_GameOver.bfevfl`) pass, so file contents look genuine, but the converter rebuilt the RomFS
   image (other table layout, IVFC recomputed). The IVFC master hash sits in an FS header whose NCA signature
   nobody checks, so IVFC passing proves only internal consistency, not that it is 1.0.0.

5. The user cannot re-dump (no working Switch). The converter's order was found by analysis: it sorts paths
   with `\` as the separator (`AISchedule/...` before `AI/...`); otherwise 16-byte data alignment, tables right
   after the data, standard bucket counts, prepend chains. A search over Nintendo-style orders found the exact
   match: **paths sorted with `/`, compared as bytes, tables at the next 8-byte boundary, same table rules.** The
   user's files in that layout give SHA-256 `4751542f...`, the verified 1.0.0 image, byte for byte. That proves
   the content is genuine 1.0.0 (the expected hash is the oracle).
6. Fixed: `tools/romfs_rebuild.py` (stdlib only) rewrites an image in that layout; `build_win.canonical_romfs`
   runs it when the extracted image's hash is wrong and keeps the result only on an exact SHA-256 and size match
   (then `index.json` gets the new offsets and a `rebuilt_from` record), else stops as before.
7. Third run (2026-10-20): **the whole build passes.** RomFS rebuilt and matched, fonts from Ryujinx's firmware
   (four bound by their bytes, as upstream allows), overlays made, `TOTK_win/` written (19 files) and
   `--check --full` ok. The data build is done; next is the loader (below).


Not an option: getting game files from anywhere but the user's own dump.

## Next tasks

1. ~~Loader for the JIT~~ **done (2026-10-20).** First real boot probe on the user's files:
   ```
   mapped main    at 0x300000000 size 0x4666000, 704 symbols
   mapped subsdk0 at 0x304800000 size 0x6a9000, 11622 symbols
   mapped sdk     at 0x305000000 size 0xdd9000, 27761 symbols
   linked main    relative 502345, symbolic 2091 (cross-module 2079, weak zero 12)
   ENTER original nn::init::Start at sdk+0xfa060          (the address upstream's loader names: layout confirmed)
   first unimplemented svc: 0x29 GetInfo at sdk+0x1584b0  (x1 = 11: random entropy)
   ```
   No import is undefined in all modules. Not done yet, on purpose: host overrides (`runtime_override`,
   `runtime_observer`: the resolver's phase 0 returns 0, so the SDK's own code runs everywhere), MNG's game patches
   (`game_patches.c`), the DT_INIT constructor-state check (`ctor_flag`), NSO segment SHA-256 checks (build_win
   already verifies the ExeFS files). The guest layout constants live at the top of `src/app/main.c` and match
   upstream `services.c` (low region from 0x3_0000_0000: stack +0x1000_0000, heap +0x3000_0000; alias 0x70_0000_0000).
2. **Horizon host**: started (2026-10-20). The boot probe on the user's files now runs `nn::init::Start`,
   `nnosInitialize`, `nninitInitializeSdkModule` (with upstream's overrides), `nninitStartup`, all three DT_INIT
   constructors and the game's `nnMain` up to graphics start-up: `nv::InitializeGraphics`, `nn::vi` (display, layer,
   vsync event), `nvnBootstrapLoader`, then (2026-10-21, NVN layer with the null backend) the whole graphics start-up:
   NVN device, queue, a 2-image 1920x1080 RGBA8 swapchain (`nvnWindowInitialize`); it creates its first two
   threads; with input, time, shared fonts, applet overrides, NVN physical / virtual pools, the audio renderer and
   the Opus decoder **the game runs its main loop**: 55 threads, the startup logo finished ("game finished startup
   logo"), then a steady 30 fps (present interval 2) on the null GPU backend for as long as it is left running, with
   music mixed and played through WASAPI. Most likely the title screen waiting for a button (no picture yet to check).
   How it is built:
   - Each guest thread is a pthread (winpthreads) running its registers on the `jit_core` of the virtual core it
     holds (3 cores, upstream's scheduler). A better thread waiting for a core calls `jit_halt` on the owner, which
     gives the core up at the halt (`runtime_core_checkpoint`). Host-to-guest calls (`host_guest_call`) run nested on
     the calling guest thread, returning through a trap stub (`svc #0xfffe`).
   - Overrides (upstream `runtime_override` / `runtime_observer`) bind imports to bridge stubs with generic thunks:
     x0..x7 as Win64 integer arguments, result back as void / u8 / u32 / u64. Floating-point arguments or by-value
     structs need their own thunk (none so far).
   - Upstream files compile unchanged where they can (`ipc.c`, with `-include src/host/upstream_compat.h`); the
     others are adapted copies in `src/host` with the same names and functions, so upstream fixes merge by hand.
   - The RomFS needs `runtime-index.bin` and `romfs-config.txt` from `tools/build_win.py` (run it again on an older
     output folder). The image is hashed once; `.romfs-verified-v1` next to it keeps the proof.
   - Saves and the account id live in `%LOCALAPPDATA%\WinTears\totk` (or `TOTK_PROFILE_DIR`).
3. **Next, in order:**
   - **Next (Vulkan backend, 2026-10-21):** the game is playable (ASTC decoded on upload; written storage buffers in
     video memory per submission). Measured on the RTX 4070: about 3 ms of replay per frame once shaders are cached; the
     GPU's "13 ms" per frame is the GPU idling at 400-650 MHz (the game asks for 30 fps), not a bottleneck. Copying
     read-only uniform / storage / vertex / index buffers into video memory was tried and gained nothing (NVIDIA caches
     system-memory reads). Stutters were shader translation (up to 2.8 s in a new area): `tools/prepare_shaders.py`
     now translates ahead of play; the 10 s log line splits replay time into translation, pipelines, textures.
     Done 2026-10-24 (not yet seen in play): layered rendering (a pass covers the layers of its attachment views; the
     vertex shader picks the layer; clears cover every layer), buffer textures (texel buffer support is read from the
     format's buffer features, not its image features, which made every format fall back to zeros; views at offsets the
     device cannot view in place are sampled from a copy), partial-mask clears (a colour clear with a channel mask and a
     stencil clear with a write mask are drawn with a write mask: `vk_clear.c`, shaders from
     `tools/build_clear_shaders.py`). Also done 2026-10-24 (not seen in play): depth to colour / other-depth texture
     copies go through a buffer (Vulkan allows neither copy nor blit there); 24-bit depth uploads are converted to
     float (`upload` in `vk_tex.c`); rendered textures in CPU-visible pools are written back on InvalidateMappedRange
     (`vt_note_rendered` / `vt_pool_invalidate`; skipped when the CPU wrote the memory since the render; the GPU wait must happen before the dirty-tracker pages are opened, else the completion thread and the game thread wait for each other; `TOTK_WRITEBACK=0` turns it off); samples-passed
     counters are real occlusion queries, one per pass while counting (`vk_exec.c`; `TOTK_OCCLUSION=0` makes every
     query "visible" again). The game culls objects whose query reports 0 and scales effects by it, so a wrongly
     missing object or glare is the first thing to try with that switch. Also done (not seen in play): instance divisors above 1
     (VK_EXT_vertex_attribute_divisor, enabled when the GPU has it), a pipeline cache kept in `<profile>/pipeline-cache.bin`
     (`vk_draw.c`), and a log line saying why a pool could not be imported (`vk_winmem.c` describes the memory; a no-access
     placeholder page in a virtual pool is the first suspect). Texture / image fixes found by comparing with upstream's notes (2026-10-24, not seen in play; the
     first three are the best candidates for the sky island haze, the black-void scenes and the blue menus): compressible
     textures share an image only with their own format (`stor_compressed`: a white clear of an RGBA8 target no longer
     turns the R32F linear depth that lived there into NaN; `TOTK_COMPRESSED_ALIAS=1` for A/B); images the GPU wrote stay
     while a texture object or a texture-pool descriptor still names them (they were dropped, and came back blank, after
     600 idle submissions); passes use the attachment view's format (an sRGB view of a linear image rendered with the
     image's format); BC textures can be rendered into through a one-texel-per-block view (BC4 as RGBA16UI);
     texture clears of an sRGB / linear twin convert the value. The log prints the first layouts that alias one memory
     range (`storage ... beside an existing ...`). Not done yet: pools the driver cannot import (one 111 MiB pool), background shader translation (the translation depends on state known only at the draw, so it cannot start earlier), performance (full barriers between
     passes, copies and dispatches, about 285 a frame; GENERAL layouts). Inline uniform updates (about 210 a frame) are
     renamed as upstream does (`vd_ub_update` in `vk_draw.c`; `TOTK_UB_RENAME=0` for A/B): passes no longer split at
     them, barriers per frame went from about 510 to 285.
   - **Cutscenes** (2026-10-22): `src/host/movie.c`, the Windows port of upstream `movie_host.c`: one host VP9 decoder (libvpx)
     replaces the SDK's movie::Decoder, which needs Tegra NVDEC. It covers all 28 cutscenes (same library, same codec).
     Guest-visible data (input buffers, strings, result slots) comes from `host_guest_alloc`; `movie_host_tick` runs after
     the present's core-away scope. Not yet confirmed to play a whole cutscene.
   - **NVN layer, left out of the port so far** (`src/gfx/nvn.c` header comment): CPU-visible physical memory pools
     (`src/gfx/nvn_vpool.c`: they would alias the game's private heap memory), the recording-copy marks in control memory (copies of control memory the game frees to
     its heap stay until it is re-added), the translation thread, the game's frame-step patches.
   - **Host tests**: guest programs (`tests/guest`) that exercise the sync svcs (mutex hand-off, condition variable
     order, address arbiter, events, WaitSynchronization timeouts) on several threads.
   - **Frontend** still to do: rumble, gyro, the software keyboard, a controller layout file.
   - `long` is 32-bit on Windows: audited 2026-10-24 over `src/` and the upstream files that are compiled (`ipc.c`, audio, `nvn_tex.c`, `nvn_deswizzle.c`): the only uses left are `strtol` results, `timespec` and error codes, none holds a guest value.

## Building and testing

Native Windows: MSYS2 is installed on the user's PC (`C:\msys64`, UCRT64: gcc 16, cmake, ninja, boost). From another
shell: set `MSYSTEM=UCRT64` and run `C:\msys64\usr\bin\bash.exe -lc "..."`. Fresh machine: install
[MSYS2](https://www.msys2.org/), open the UCRT64 shell, then:

```
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,boost} git
git clone --recursive https://github.com/androidfail/wintears && cd wintears
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```

Linux cross-build with Wine: see `README.md`. Python tools: Python 3.14+ (`pip install -r
upstream/games/totk/requirements.txt`), then `python -m unittest discover -s tools/tests -v`.

Regenerated files (CI checks they are current):
- `tests/guest/programs.h` from `tests/guest/programs.S`: `python3 tools/embed_guest.py tests/guest/programs.S tests/guest/programs.h` (needs `llvm-mc`, `llvm-objcopy`, `llvm-nm`).
- `src/bridge/nvn_bridge_gen.c` and `tests/bridge_fixture_gen.c`: commands in `.github/workflows/ci.yml`.

## Things learned the hard way

- **Submodules:** a GitHub ZIP download and a plain `git clone` both leave `upstream/` empty, and `build_win.py`
  then fails with `ModuleNotFoundError: No module named 'build_app'`. Fix: `git submodule update --init upstream`.
  A clearer error message for this was proposed and declined by the user for now; ask before adding it.
- **Wine** commits `SEC_RESERVE` sections immediately (Windows does not): `pal_test` skips that one check under Wine.
  Wine also needs a UTF-8 locale (`LANG=C.UTF-8`) for non-ASCII file names.
- **MinGW headers on Ubuntu 24.04** lack newer CPU-topology structs: dynarmic's xbyak needs `XBYAK_CPU_CACHE=0`
  (set in `CMakeLists.txt`). Boost is header-only for dynarmic; on Linux point `Boost_INCLUDE_DIR` at a folder
  containing only a `boost` link, not at `/usr/include`.
- **dynarmic fork:** `azahar-emu/dynarmic` is maintained; `merryhime/dynarmic` is gone from GitHub.
- **Upstream Python tests** that need macOS (`clonefile`, app bundles) fail elsewhere; CI runs only the portable ones
  (`test_game_containers test_relr test_mod_manifest test_romfs_paths test_shader_list`).
- **winpthreads' first TLS key is 0**: upstream's `g_keythr ? pthread_getspecific(...) : NULL` idiom then reads as
  "no key". `src/host/threads.c` keeps a separate flag.
- **MSYS login shells drop `LOCALAPPDATA`** from the environment of the programs they start: the PAL asks the shell
  API (`SHGetKnownFolderPath`) first.
- **Shell heredocs and backslashes**: in this tool setup, `\\0` / `\\n` inside a heredoc edit script land in files as
  real NUL bytes / newlines. Write edit scripts to a file first.
- **Python 3.14** is required by upstream's scripts (zstd in the standard library).

## The user

- PCs: Intel/AMD with an RTX 4070 and an RTX 3080; later target: a laptop with integrated AMD Radeon (see the
  plan's "Low-end budget"). Uses Ryujinx: game files are in Ryujinx's *Game Directories*, keys in
  `%APPDATA%\Ryujinx\system\prod.keys`, firmware in `%APPDATA%\Ryujinx\bis\system\Contents\registered`.
- Owns TOTK 1.0.0 (as an NSP).
- Never ask for, print, commit or upload keys, game files, saves or anything extracted from them.
- Commit to `main`. Commit messages end with the session's attribution lines.
