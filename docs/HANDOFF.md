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
| `tools/build_win.py` | extracts and verifies 1.0.0 ExeFS/RomFS/fonts/overlays with upstream's scripts, writes `TOTK_win/` + manifest, `--check` | `tools/tests`, 11 tests |
| `tools/nsp_info.py` | describes a game file's NCAs and section encryption (no key values), with a verdict | `tools/tests/test_nsp_info.py`, 4 tests |
| Loader, Horizon host, Vulkan, frontend | not started (task list below) | |

## Current blocker: the user's NSP

The user's first real run (`tools\build_win.py Zelda.nsp -k prod.keys --firmware <Ryujinx firmware>`) stopped in
upstream's `extract_exefs.py`:

```
require(fs[4] == 3, 'Only AES-CTR ExeFS is supported')  ->  ValueError: Only AES-CTR ExeFS is supported
```

So the base Program NCA was found (title and metadata checks passed, the NCA header decrypted and its FS header hash
matched), but the ExeFS section's encryption type is not 3 (AES-CTR). Most likely it is 1 ("none"): some NSP
conversion tools remove section encryption. It could also be an update file (type 4, BKTR).

**Step 1:** have the user run, from the repo folder:

```
python tools\nsp_info.py "<path to>\Zelda.nsp" -k "%APPDATA%\Ryujinx\system\prod.keys"
```

and read the `verdict:` line. It prints no key values, so the output is safe to paste.

**Step 2, by verdict:**

- **"NOT encrypted (type 1)"**: add support for unencrypted sections. Rules: `upstream/` is a pinned submodule and is
  never edited. Options, in order of preference:
  1. Patch the *work copies* at build time: `build_win.prepare_work` copies `upstream/games/totk/scripts` into
     `<work>/totk/scripts`; apply small, exact text patches there (a `patches/` folder + an apply step that fails
     loudly if the expected text is missing). Places to change:
     - `extract_exefs.py`: `require(fs[4] == 3, ...)` → accept 1 too; in `section_read`, skip `decrypt(...)` when
       `fs[4] == 1`; compute `ctr_key` only when needed (`nca_ctr_key` fails for a rights-managed NCA whose ticket
       was removed along with the encryption).
     - `extract_romfs.py`: `if fs[2:5]!=bytes([0,3,3]):continue` → also accept `[0,3,1]`; skip decryption in
       `read` for type 1; same lazy key.
     - `extract_shared_fonts.py` probably needs nothing: fonts come from Ryujinx's firmware NCAs, which are standard.
  2. Contribute the same change upstream to MNG as a PR, then bump the submodule.
  - All hash checks after extraction (ExeFS file hashes, RomFS SHA-256, IVFC levels) stay as they are: they check the
    decrypted content, so a converted NSP of the same 1.0.0 game must still match exactly.
  - Test with the `plain_nca()` fixture in `tools/tests/test_nsp_info.py` (an NCA with an unencrypted code section);
    extend it with an unencrypted RomFS section for `extract_romfs.py`.
- **"update patch (BKTR)"**: the file is an update, not the base game. Find the base 1.0.0 file (Ryujinx keeps
  updates separately: right-click the game → *Manage Title Updates* shows their paths).
- **"not the TOTK base game"**: wrong file.
- **"standard AES-CTR"**: then the failure is elsewhere; look at `TOTK_build\logs\extract_exefs.log`.

Not an option: getting game files from anywhere but the user's own dump.

## Next tasks after the data build works

1. **Loader for the JIT** (task "Phase 3A next"). Port `upstream/games/totk/runtime/loader.c` to load the
   *original* (unrewritten) `main`, `sdk`, `subsdk0` from `TOTK_win/data/v0/exefs`: decompress NSO segments
   (see `upstream/games/totk/scripts/prepare_modules.py` and `third_party/d2r/nsoimg.py`), map them at MNG's layout
   inside a `pal_reserve_fixed` range, apply relocations (RELA kinds 257/1025/1026/1027, and RELR via
   `third_party/d2r/relr.py`'s logic), resolve imports across modules. Imports MNG binds to host functions
   (`runtime_override`, `runtime_observer`) become `bridge_add` stub addresses instead of host pointers.
   Then run `nn::init::Start` in a `jit_core` and log every svc until the first unimplemented one.
   Upstream's generator `scripts/prepare_runtime.py` works on rewritten images; we need an equivalent for originals.
2. **Horizon host**: port `upstream/games/totk/runtime/*.c` (threads, services, ipc, fs_host, romfs, time, account,
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

Native Windows (what the local session will likely use): install [MSYS2](https://www.msys2.org/), open the UCRT64
shell, then:

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
