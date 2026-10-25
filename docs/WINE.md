# Running under Wine / Proton (Steam Deck)

WinTears is a Windows program; on the Steam Deck it runs under Proton (Wine 11 on Linux, RADV for Vulkan). Wine
implements the Win32 calls on top of Linux, and several of them behave very differently in cost or in what the Linux
GPU driver accepts. Each rule below cost at least one broken Deck run. Code checks `pal_under_wine()`.

## Memory

- **VirtualAlloc / VirtualProtect / VirtualFree are mmap / mprotect / munmap.** A protection change inside a range
  splits the Linux mapping (VMA) there.
- **The GPU only sees memory through `VK_EXT_external_memory_host` imports, which on Linux are amdgpu user pointers.**
  The kernel takes them only when the whole range is one private, anonymous mapping (`AMDGPU_GEM_USERPTR_ANONONLY`):
  - no write protection of imported memory: no `mprotect`-based dirty tracking (`nvn_dirty.c` is off under Wine;
    textures are checked by content, `vk_tex.c`);
  - no importing of section views (`CreateFileMapping` / `MapViewOfFile`, shared mappings): the NVN virtual and
    physical pools get device memory in their place (`vk_dev.c`, `standin_make`), which is right only because TOTK's
    physical pools are CPU_NO_ACCESS;
  - the whole guest heap is not one import (its pages change protection); each pool is imported on its own
    (`TOTK_IMPORT_HEAP`).
- **The kernel checks every page of every imported range a submission uses, at each `vkQueueSubmit2`** (about 10 ms a
  submission on the Deck). Submissions and presents therefore run on their own thread (`vk_dev.c`, submission thread),
  and no device feature that makes RADV put every allocation in every submission is enabled (buffer device address,
  update-after-bind descriptors).
- **An import must be gone before the game frees its memory.** Pool finalize waits for the GPU and frees the import
  itself; freeing it later made a submission name pages that no longer existed (device lost).
- **Wine's heap takes one process-wide lock per HeapAlloc, and maps / unmaps every large block.** Wine refuses the
  low-fragmentation heap. All C and C++ allocations of the exe go through `src/app/alloc.cpp` (per-thread cache for
  small blocks, a cache for large ones; `--wrap` for malloc / free).

## Threads and timing

- **Do not stop threads (SuspendThread / GetThreadContext) while the game runs.** A thread stopped inside a driver call
  has the call interrupted, and the kernel starts a GPU submission over: the sampler (`TOTK_SAMPLE=1`) is off by
  default.
- **Sleep and waits are rounded to the timer**: `pal_sleep_ns` uses `NtDelayExecution` under Wine (a 1 ms sleep takes
  about 1.07 ms; the start of each log says).
- Thread priorities are not applied by Wine without realtime rights; nothing may depend on them.
- Wine 11 synchronisation uses the kernel's ntsync: waits and wakes are system calls, cheap but not free.

## Exceptions

- **An exception under Wine is a Linux signal and costs tens of microseconds.** Nothing on a hot path may rely on
  faults (no write-watch or guard-page tricks); the JIT's fastmem faults are counted in the log (`[exceptions]` with
  the sampler) and are 0 in play.

## Files and the system

- `/proc` and `/sys` are readable through Wine's `Z:` drive, but `Z:\proc\self` is not always this process: find it
  by name (`pal_win.c`, `proc_dir`).
- Paths: Proton maps `Z:\` to `/`; the Deck's game folder is `Z:\home\deck\Games\WinTears`.

## Checking a change for the Deck on a Windows PC

`TOTK_IMPORT_HEAP=0 TOTK_DIRTY_PROTECT=0` gives the Deck's memory paths (per-pool imports, content checks). The
kernel's per-submission cost, section-view imports and Wine's heap cannot be reproduced on Windows.
