# Potential optimizations report (Steam Deck)

Compiled 2026-10-30 from this session's Deck measurements, the user's open-area play log and the code audits (four
code-reading agents: samplers/depth/shadows, render state and sync, recompiled floating-point code, and a search for
larger "breakthrough" optimizations). Open audit items were re-checked against the code on 2026-10-30.

## 1. Where things stand

| | Start of session | Now |
|---|---|---|
| Translation (replay) per frame, test scene | 35-40 ms | 20-22 ms |
| Game submit to frame done, test scene | 40-55 ms | 24-34 ms |
| Medium scenes | 16-20 fps | 26-30 fps |
| User's open area | (not measured) | 20 fps, one drop to 9 |

- **The game picks 30 or 20 fps itself.** It uses present interval 2 or 3 depending on how long its recent frames
  took. Above about 33 ms it uses 20, with nothing in between. The PC holds 30 throughout.
- **The user's open area:**
  - Each frame took 39-52 ms from the game's submit to done:

    | Step | ms per frame |
    |---|---|
    | Translation, frame handed over after | 14-29 |
    | Waiting for the submit thread | about 5 |
    | `vkQueueSubmit2` | about 6 |
    | GPU and wake-up | 11-15 |

  - The drop to 9 fps was first-time shader translation (2.2 s per 10 s) plus texture loading (0.8 s).
- **Why the Deck is slower than the PC (about 3 ms of translation a frame):**
  - **CPU:** a slower core at a lower clock under the shared 15 W cap.
  - **Proton:** each Vulkan call and system call goes through Wine.
  - **Cores:** 4 cores shared with the game's threads; threads waited 40-60 s per 10 s for a core, and Wine ignores
    thread priority.
  - **Scene:** the open area has more passes and more uniform data.
- **Translation time splits about half and half:** in the open area, actual translation work is about 12 ms a frame.
  The rest is waiting for a core or for the previous frame.

## 2. Done this session

| Change | Commit | Effect |
|---|---|---|
| Mid-frame submission splits (`TOTK_SPLIT_MS`, default 8 ms), with the device-lost fixed (new submission at the split) and frame-scoped caches | `e21a86f` | GPU starts earlier; game submit to done 40-55 -> about 30 ms |
| Vulkan calls on their own thread (`vk_encode.c`, on under Wine); spin cut to 200 pauses | `522ed62`, `5af9f7d` | Replay thread no longer pays the Wine call cost |
| Dynamic uniform buffer offsets and an early descriptor-set reuse cache | `522ed62` | Most draws reuse their sets without resolving |
| Lock-free texture table reads (seqlock), cheaper per-draw work | `eab1ff5`, `522ed62` | Replay 35-40 -> 20-22 ms |
| GPU timestamps rebased to the game's submit (`TOTK_TS_REBASE`) | `e21a86f` | The game's timing excludes the port's delay |
| ASTC as BC3 for colour only (12 GiB RAM or more) | `995e6dd` | Gloss fix; all-BC3 saved only about 1 ms |
| Shadow and sampler fixes from the audit | `eab1ff5` | Visual; not yet confirmed on screen |

## 3. Tried and ruled out

| Idea | Result |
|---|---|
| Transparent huge pages | Already on (`always`); the game heap is 98% huge-page backed |
| Smaller import windows (`TOTK_WINDOW_MB=4`, `8`) | Same submit time as the 16 MiB default (A/B, 2026-10-30) |
| All ASTC as BC3 (`TOTK_ASTC_BC3=2`) | About 1 ms; not worth the gloss bug |
| Pacing presents, early, capped or doubly rebased timestamps | Did not move the game's 20/30 choice |
| `VK_KHR_push_descriptor`, descriptor buffers, bindless | Would only save set creation (0.2-0.3 s per 10 s); descriptor buffers need buffer device address, which puts every allocation in every submission on RADV |
| Recording a whole frame once and replaying it | Rejected by the breakthrough audit: each frame resolves against live state (texture registrations, memory contents). The per-pass version (5.2) checks that state instead of assuming it |
| Lower internal resolution | Handheld mode already renders at about 720p; GPU time is not the longest step |

## 4. Candidates, ranked

Gains are per frame in the user's open area unless noted. "PC" means it can be measured and checked on the PC first.

| # | Candidate | Estimated gain | Cost | Risk | Test first |
|---|---|---|---|---|---|
| 1 | Native Linux build with driver-allocated memory mapped at the guest addresses | 5-10 ms (up to about 5 from the submit, 1-2 from Wine calls, plus scheduling that works) | About a week | Medium | A 1-hour native submit benchmark on the Deck (5.1) |
| 2 | Hot-pass plan: measure per pass, reuse unchanged passes, engine-level replacement for 1-3 passes | Unknown until measured; the share of replay in repeating passes | Hours to measure, about a week to reuse | Medium (reuse) to high (replacement) | Step 1 on the PC (`PLAN-hot-passes.md`) |
| 3 | Give the translation thread its own core (`SetThreadAffinityMask`, which Wine passes on) | Part of the 2-17 ms spent waiting for a core | An hour | Low | One Deck A/B run |
| 4 | Fewer full barriers between passes (150-250 `ALL_COMMANDS` barriers a frame) | Some of the 11-15 ms of GPU, and power left for the clock | Days | Medium (missing sync shows as flicker) | PC validation layers, then Deck |
| 5 | Fewer submissions per frame (tune `TOTK_SPLIT_MS`; every submission pays the kernel's memory check) | Up to a few ms of the 6 ms submit | Hours | Low | Deck A/B |
| 6 | Shaders translated ahead of time for the open world (copied from the PC's cache) | Removes the 9 fps drops in new areas | Hours | Low | Needs the user's OK to copy shaders to the Deck |
| 7 | Cheaper uniform copies (about 2 GB per 10 s): one persistent copy per address, updated in place | Memory bandwidth shared with the GPU; 1-3 ms | Days | Medium | PC |
| 8 | Start translating each command list on the game's own recording threads | Spreads translation over several threads; large if it works | Weeks | High (data only final at submit) | PC |
| 9 | Texture content checks off the critical path | About 1 ms (170-300 ms per 10 s) | A day | Low | PC |
| 10 | Dynamic frame-rate patch: the game never chooses 20 fps (`main+0x8f6bec`) | 25-28 fps instead of 20 at 36-40 ms frames | Hours | The game runs up to 15% slow in heavy moments | Optional switch, off by default |
| 11 | Manual GPU clock 1,600 MHz on the Deck (performance menu) | Shorter GPU step if the power budget allows | None | None | The user sets it; can't be set over SSH |

## 5. Details of the top candidates

### 5.1 Native Linux, and the submit benchmark

The submit costs about 6 ms because the port gives the GPU the game's memory as imported user pointers. The amdgpu
kernel driver re-checks those pages at every submission. Neither huge pages nor window size changed it.

A native Linux program can do the reverse: allocate the memory through the GPU driver (exportable), then `mmap` it at
the exact guest addresses. Ordinary driver memory is not re-checked page by page. Proton's Vulkan layer offers no
mapping at a chosen address (checked: no `VK_EXT_map_memory_placed` in winevulkan).

Benchmark before any porting: a small native program on the Deck that makes 1.4 GB of GPU-visible memory both ways
and times `vkQueueSubmit2` against each. If driver memory is much cheaper, the port (platform layer, recompiled code
as Linux libraries, window, sound and input) is worth about a week. If not, native Linux saves only 1-3 ms plus
scheduling.

### 5.2 Hot passes

See `docs/PLAN-hot-passes.md`. Per-pass cost and how much each pass repeats are measured on the PC from the user's
open-area save, with the Deck confirming the ranking. Then:
- reuse passes whose recording is unchanged (generic, committed);
- then engine-level replacement for at most 2-3 passes (game-specific, kept local).

Every step has a switch, is off by default, and is checked by automatic per-pass pixel comparison.

### 5.3 Cores

Threads wait 40-60 s per 10 s for a core, and Wine ignores the HIGH priority the port asks for. Pinning the
translation thread, and keeping the game's threads off that core, is the cheapest experiment on this list.

## 6. Correctness items from the audits that are still open

Re-checked in the code on 2026-10-30. None was confirmed as the cause of a visible bug. Each has a way to confirm it
from the log or with a switch.

| Item | Where | Possible effect |
|---|---|---|
| Depth-bounds test always off (the NVN call is dropped) | `nvn_ext.c:267`, `vk_draw.c:896` | Light and shadow passes limited to a depth range cover the whole screen: shadows from the wrong cascade, extra specular ("too glossy") |
| Primitive restart always off | `nvn_ext.c:263`, `vk_draw.c:896` | Strips joined across restart indices: spike triangles, stray shadow streaks |
| Copies between block-compressed and uncompressed formats skipped | `vk_exec.c:620` | Textures the game compresses on the GPU stay garbage or zero; zero roughness looks like a mirror |
| Depth24 stored as D32F | `vk_fmt.c:44-45` | Bias units shrink (shadow acne) and the shadow reference is no longer clamped to [0,1] (shadows past a cascade's far edge) |
| Shadow lookups through the dummy sampler (no compare) | `vk_draw.c:525` | On RADV such a lookup returns 0, fully shadowed |
| Conditional rendering reads occlusion results that may not have landed | `vk_exec.c:695` | Objects pop; old shadow-mask contents survive (`TOTK_OCCLUSION=0` tests it) |
| Recompiled floating-point code: difftest never exercises rounding or FMA; default-NaN mode ignored; FRECPE under flush-to-zero; FRSQRTE table filled lazily without atomics | `wt_native_ops.h`, `wt_recomp.cpp` | Audited as not a likely cause of the visual bugs; low impact |

## 7. Suggested order

1. **Cheap Deck experiments, an hour or two each:** pinning the translation thread (#3), split tuning (#5) and the
   native submit benchmark (5.1). Together they show whether the submit and scheduling costs can be cut, which is
   most of the gap.
2. **Per-pass measurement on the PC** (#2, step 1), once the open-area save is on the PC.
3. **Then the larger of** native Linux or pass reuse, depending on 1 and 2.
4. **In parallel, needing the user:**
   - set the manual GPU clock (#11);
   - decide on copying shaders (#6) and on the dynamic frame-rate patch (#10);
   - look for the visual items in section 6 on screen.
