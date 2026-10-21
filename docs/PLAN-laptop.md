# Plan: memory and CPU on a laptop (written 2026-10-21, for whoever carries it out)

Symptoms reported on the user's laptop: memory and CPU at 100 %, GPU around 20 %, then

```
[wintears] FAIL: Vulkan: vmaCreateBuffer(vk_vma, &bi, &ac, &c->buf, &c->al, &info) failed (-2) at .../src/gfx/vk/vk_dev.c:301
```

`-2` is `VK_ERROR_OUT_OF_DEVICE_MEMORY`. It has nothing to do with file systems: it is the allocation of a 64 MiB
staging chunk (`new_chunk` in `src/gfx/vk/vk_dev.c`) failing because memory ran out. The GPU at 20 % means the GPU is
waiting for the CPU, so the work is: (1) stop the crash, (2) measure where memory and CPU go, (3) cut the biggest users.

Read `docs/HANDOFF.md` first. Its rules hold: work on `main`, never edit `upstream/`, never print, commit or upload
game files, keys, saves or anything extracted from them (shader caches and screenshots included), commit messages end
with `Co-Authored-By: ...` as in the existing history, the user reads on a phone (short messages, outcome first).
Build: MSYS2 UCRT64, `cmake --build build -j4` (more jobs can crash GCC). Run: see `docs/TESTING.md`.

## Step 0: what the laptop is (ask the user, do not guess)

Ask the user for the laptop's RAM size, CPU model (cores / threads) and GPU, and the first 30 lines of the game's
output (they include `[vk] device <name>, driver ..., BC yes/no, ASTC yes/no`). An integrated GPU (Intel Iris / UHD,
AMD Radeon Graphics in a Ryzen APU) shares system RAM; its "device local" memory heap is small (often 512 MiB to 2 GiB
reported). Everything in steps 2-3 matters much more there. With 8 GB of RAM the game can not fit (see "Memory budget"
below); 16 GB is the realistic minimum.

## Step 1: stop the crash (small, do first)

File `src/gfx/vk/vk_dev.c`, function `new_chunk`, and the constants above it.

1. `CHUNK_SIZE` 64 MiB -> 16 MiB. Larger requests already get a chunk of their own size (`size > CHUNK_SIZE`).
2. Keep at most 2 free chunks instead of 4 (`g_free_chunk_n < 4` in the completion code around line 362).
3. When `vmaCreateBuffer` fails: do not `die` at once. Wait for every in-flight submission (`vk_wait_done(g_fence)`;
   note `vk_mu` is held by the replay thread at this point, and the completion thread takes `g_done_mu`: check that
   waiting here can not deadlock by reading `done_thread` first), take a chunk from the free list if one came back,
   otherwise retry the allocation once with `.usage = VMA_MEMORY_USAGE_AUTO` and only
   `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT`. Only then `die`, with a
   message naming the memory heap sizes (see step 2).
4. `MAX_IN_FLIGHT` 6 -> 3: each in-flight submission keeps its staging chunks alive.

Check: the game still runs on the desktop PC (one run of about 2 minutes, `TOTK_INPUT_SCRIPT` as in the commit
history, look at the "last 10 s" log lines: fps stays at 30).

## Step 2: measure (no guessing: every later step starts from these numbers)

Add to the existing 10-second log in `submit()` (`src/gfx/vk/vk_dev.c`, the `VLOG("last 10 s: ...")` line), or a
second line next to it:

- **Vulkan memory per heap:** enable `VK_EXT_memory_budget` when the device has it (device creation in `vk_dev.c`)
  and pass `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` when creating the VMA allocator; then log `vmaGetHeapBudgets`:
  for each heap, usage / budget in MiB and whether it is DEVICE_LOCAL.
- **Process memory:** `GetProcessMemoryInfo` (psapi; link `-lpsapi`): working set and private (commit) bytes.
- **What the backend holds:** image bytes (`vk_tex.c` already counts "MiB of game texture storage": also count the
  bytes of the Vulkan images themselves, which are larger, see step 3a), staging chunk count and bytes, shadow bytes.
- **CPU per thread:** every 10 s, `GetThreadTimes` for each host thread, printed as % of one core, with a name. The
  threads are: the guest threads (pthreads in `src/host/threads.c`, one `jit_core` per virtual core), the replay
  (whatever thread calls `submit`), the completion thread (`done_thread`), the audio thread, the ASTC decode threads
  (`src/gfx/vk/vk_astc.cpp`, created per texture), the frontend / main thread. Keep a list of thread handles when they
  are created (or enumerate with `CreateToolhelp32Snapshot` + `OpenThread`).
- **Write faults of the dirty tracker:** count calls of the fault handler in `src/gfx/nvn_dirty.c` per 10 s (each is a
  page fault plus a Vulkan re-upload later).

Run on the desktop once, send the build to the laptop (copy `build/wintears.exe` and `profile/shader-cache` to the
user's drive, see "Running the game" in `docs/TESTING.md`), ask the user for the log after 3 minutes of play.

## Step 3: memory (most likely order of size; confirm with step 2)

a. **Decoded ASTC textures.** Desktop GPUs (and most laptop ones) have no ASTC, so `vk_tex.c` decodes ASTC to RGBA8
   on upload (`vk_astc_decode`). RGBA8 is 4x the size of ASTC 4x4 and 16x the size of ASTC 8x8: 600 MiB of game
   texture storage can become several GiB of images, which on an integrated GPU is system RAM. Options, cheapest
   first:
   1. Skip the largest mip level of big textures (width or height >= 1024): create the image with one level fewer and
      half the size, upload from level 1. Upstream does this on iPhone: see `drop` in
      `upstream/games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m` around line 3291. A setting
      (`TOTK_TEX_DROP=0/1/2`, default 1 when the GPU's device-local heap is below 4 GiB). Careful: render targets and
      textures the game copies to or reads back must keep full size: only drop for textures uploaded from pool memory
      (the mirrored ones, `vt_*` upload path), never for images the GPU renders into.
   2. Decode ASTC to BC7 or BC3 instead of RGBA8 (4x smaller than RGBA8, the GPU samples it natively, "BC yes" in the
      device log). Needs an encoder (bc7enc is a single-file library; check its licence), costs CPU at load time, so
      do it in the background thread of step 4b. Larger job: only if (1) is not enough.
b. **Images the game no longer uses** are dropped after 600 idle submissions (`vt_end_submission` in `vk_tex.c`).
   Make it budget-driven: when the heap usage from step 2 passes 80 % of the budget, drop mirrored (re-uploadable)
   images idle for more than 60 submissions, oldest first. Never drop images the GPU rendered into: their content is
   only on the GPU.
c. **The JIT code cache:** `src/cpu/jit` sets `uc.code_cache_size` to 128 MiB per core (3+ cores). Check with step 2
   whether dynarmic commits it all; if so try 64 MiB and watch for "code cache full" behaviour (dynarmic clears it and
   recompiles, a small stall).
d. **The imported guest heap:** `vk_dev.c` imports the guest heap (up to 4 GiB, `HEAP_CAP` in `src/host/memory.c`) as
   one Vulkan host allocation (`VK_EXT_external_memory_host`). The driver may lock those pages in RAM. Step 2 shows it
   (working set close to heap size even early). If so, import only the ranges of the game's memory pools (the code
   already does per-pool imports for pools outside the heap) and accept more imports.
e. Staging chunks and shadows: covered by step 1; confirm with the numbers.

Memory budget to write into `docs/TESTING.md` once measured: the game's own memory (the Switch gives it about 3.2 GiB)
+ JIT code caches + decoded textures + Windows. On an 8 GB laptop that does not fit; say so in the docs.

## Step 4: CPU (confirm with step 2 which thread is hot)

a. **The guest's own threads under the JIT.** On the Mac the game runs natively on ARM; here every instruction goes
   through dynarmic, so the 3 game cores cost far more. Two cheap changes in `src/cpu/jit/*.cpp` where
   `Dynarmic::A64::UserConfig uc` is filled:
   - `uc.optimizations` = all safe optimizations plus the unsafe ones yuzu uses in its "Unsafe" CPU accuracy mode:
     `Unsafe_UnfuseFMA`, `Unsafe_ReducedErrorFP`, `Unsafe_InaccurateNaN`, `Unsafe_IgnoreStandardFPCRValue`,
     `Unsafe_IgnoreGlobalMonitor` (check the exact names in `externals/dynarmic/src/dynarmic/interface/optimization_flags.h`).
     Make it a setting (`TOTK_CPU_UNSAFE=1`, default on) and test that the game still boots, loads a save and plays 3
     minutes. `IgnoreGlobalMonitor` changes exclusive load/store behaviour: if anything hangs, drop that one first.
   - Check for busy-waiting: a guest thread showing ~100 % in step 2 while the game is idle (title screen, pause menu)
     means a spin. Look at how `svcSleepThread(0)` / yields and `jit_halt` waits are done in `src/host/threads.c` and
     make waits block (WaitOnAddress / condition variables) instead of spinning.
b. **ASTC decode on the render thread.** `vk_astc.cpp` decodes inside the replay (the game waits) and starts up to 8
   new threads per large texture. Use a fixed pool created once, with `max(1, cores - 4)` threads (leave room for the
   3 game cores and the replay). Later: decode in the background when the texture is registered and upload when ready
   (needs care: a draw that needs the texture before it is decoded must wait for it, never use stale content).
c. **The replay thread** costs about 3 ms a frame on the desktop (the "replay ... ms in all" number / 300). If step 2
   shows it high on the laptop, run `TOTK_PASSLOG` on one frame and look for the most frequent op.
d. **Shader translation** in play starts `wt_xlate.exe` (a .NET program, about 70 MB of memory). It is only needed
   for shaders `tools/prepare_shaders.py` missed. Make sure the prepared cache was copied to the laptop
   (`profile/shader-cache/vk-1`, about 1.4 GB, 22,600 variants); with it, the translator should rarely start.

## Order and checks

1 -> 2 -> (send to the user, get numbers) -> the biggest of 3a / 4a first -> measure again -> next. After each change:
build with `-j4`, one 2-minute run on the desktop with a screenshot (`TOTK_SHOT=3000:<path>.png`) compared by pixel
statistics only (never open game-derived images; a script that prints mean colour and a coarse grid is enough), then a
commit with the measured before / after numbers in the message.
