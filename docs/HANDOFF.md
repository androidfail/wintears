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
| Horizon host, Vulkan, frontend | not started (task list below) | |

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
2. **Horizon host** (next): start with what the boot probe asks for (GetInfo, then memory, threads, IPC). Port `upstream/games/totk/runtime/*.c` (threads, services, ipc, fs_host, romfs, time, account,
   audio) onto `src/pal`, replacing `pthread`/`mach_*`/`dispatch_*`/signals per the table in the plan (section 6,
   phase 2). Key points:
   - MNG's scheduler (`threads.c`) already limits running guest threads to virtual cores: give each virtual core a
     `jit_core`; a thread loads its context into the core it owns. svcs exit the JIT (`JIT_STOP_SVC`), so blocking
     host calls happen outside it. Preemption uses `jit_halt` instead of `SIGUSR2`.
   - MNG's gates (`gate.S`, `hle_stack.S`) and the rewriter's thread slots are not needed on the JIT track:
     dynarmic gives svc callbacks and `tpidr_el0`/`tpidrro_el0` directly (`jit_context`).
   - Host calls back into guest code (thread entry, NVN callbacks) need a "guest call" helper: save context, set
     `pc = fn`, `x30 =` a return-trap stub (e.g. `svc #0xFFFE`), run until the trap, restore.
   - `upstream/.../nvn_impl.h` registers implementations with a Mach-O section (`__DATA,swnvn`). On Windows, shadow
     it with our own `nvn_impl.h` (earlier in the include path) that uses a PE grouped section (`.nvn$b` between
     `$a`/`$c` markers) or a generated table.
   - `long` is 32-bit on Windows: audit reused code (~540 plain `long`/`unsigned long` uses), starting with structs
     that mirror guest memory.
3. **Frontend** (SDL3: window, input, audio), then **Vulkan** (phase 5 in the plan). The bridge thunks already exist
   for all NVN entry points.

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
- **Python 3.14** is required by upstream's scripts (zstd in the standard library).

## The user

- PCs: Intel/AMD with an RTX 4070 and an RTX 3080; later target: a laptop with integrated AMD Radeon (see the
  plan's "Low-end budget"). Uses Ryujinx: game files are in Ryujinx's *Game Directories*, keys in
  `%APPDATA%\Ryujinx\system\prod.keys`, firmware in `%APPDATA%\Ryujinx\bis\system\Contents\registered`.
- Owns TOTK 1.0.0 (as an NSP). Reads on a phone between other things: short messages, outcome first.
- Never ask for, print, commit or upload keys, game files, saves or anything extracted from them.
- Commit to `main`. Commit messages end with the session's attribution lines.
