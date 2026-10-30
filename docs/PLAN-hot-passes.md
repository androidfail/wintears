# Plan: cheaper hot render passes on the Steam Deck

Goal: hold 30 fps in the open-world scenes on the Steam Deck. Rewriting the whole renderer is too large, so this plan
finds the few render passes that cost the most to translate and makes only those cheap. The steps are ordered so that
each one is measured before the next starts, and so that the cheap, generic steps come before the expensive,
game-specific ones.

## Where things stand (2026-10-30)

From the user's open-area run (`TOTK_DIAG=1`, after the menu):

| Step after the game's submit | ms per frame |
|---|---|
| Translating the frame (the replay thread) | 14-29 |
| Waiting for the submit thread, then `vkQueueSubmit2` | about 11 |
| GPU and wake-up | 11-15 |
| Total (30 fps needs well under 33) | 39-52 |

- The game drops to 20 fps (present interval 3) once frames take over about 33 ms, and it has no step in between.
- A frame has about 175 render passes, about 4,000 NVN commands and about 420 inline uniform updates. Per 10 s that is
  about 2 GB of uniform copies.
- The Deck's 4 cores are oversubscribed: threads waited 40-60 s per 10 s for a core. CPU and GPU share 15 W.
- Smaller import windows (`TOTK_WINDOW_MB=4`) and huge pages did not change the submit cost (tested 2026-10-30).

So the replay thread is the longest step, and it competes for the same cores as the game. Cutting it helps twice: the
frame is ready sooner, and the GPU gets more of the power budget.

## The ground rules

- Every new path has an environment switch and is off by default until it passes the checks in step 4. The old path
  stays available, so a bad result means turning the switch off, not reverting code.
- Generic code (any NVN game) is committed. Anything derived from the game (its shaders, data, code or measured
  per-pass signatures of its content) stays local, like the recompiled DLLs and the shader cache. It is built or
  recorded on the player's machine and never committed.
- Measurements use the user's open-area save, skip the first 60-90 s of each run (menu and loading) and compare
  10-second blocks after that. The PC must stay at 30 fps with every change.

## Where to test: the PC first, the Deck to confirm

The game sends the same NVN commands on both machines, so which passes there are, how many draws they have, how much
of each repeats between frames and whether the output pixels match are the same on the PC. The PC builds and runs
much faster, with no copying over SSH, and correctness runs (step 4's pixel comparison) can run two or three at once
with separate profile folders. Timing differs (the PC replays a frame in about 3 ms, the Deck in 15-30), but the
ranking of passes by share of the replay should carry over; one Deck run per step confirms it. Timing runs on the PC
run one at a time, since parallel runs share the CPU and GPU.

## Step 0: test setup (needs the user)

1. The user saves the game in the open area where the Deck holds 20 fps and drops to 9, and the save is copied to the
   PC's profile (between the user's own machines, never committed).
2. The PC test script and `deck_run.sh` load that save; the input script walks the same path through it every run,
   so runs compare.
3. A baseline: three runs of the current build on the PC and one on the Deck, with the per-block numbers recorded.

## Step 1: measure the cost of each pass

Add per-pass accounting to the replay, under `TOTK_DIAG` only:

- **A pass signature:** the target formats and sizes, the number of targets, the first draw's shader pair and the
  number of draws. This groups the same pass across frames (for example "shadow cascade 1, 2048x2048 D32F").
- **Per signature, per 10 s:** the replay thread's CPU time (from `begin_pass` to the end of the pass, including the
  binds and uniform updates in between), draws, binding changes, uniform update bytes, and descriptor sets made or
  reused.
- **A ranked log line** every 10 s: the top 15 signatures by CPU time, with their share of the replay.
- **Per signature, how much repeats:** the fraction of draws whose bound shaders, buffers and textures are the same
  as in the same pass of the previous frame, and the fraction of passes whose whole command list is the same apart
  from buffer contents. This decides between step 2 and step 3 for each pass.

**Decision point:** if the top 3-5 signatures are 40% or more of the replay time, continue with those. If the cost is
spread evenly over many passes, stop here: this plan won't reach 30 fps, and the general per-draw work (and the
idea of starting translation on the game's own recording threads) comes back first.

Size: about 150 lines; an hour or two including the build and three Deck runs.

## Step 2: reuse a whole pass from the previous frame (generic, committed)

Many passes, especially shadow cascades and static geometry, draw the same objects with the same bindings each
frame. Only the contents of their buffers change (positions, matrices), and the GPU reads those directly from the
imported memory anyway.

- **Fingerprint a pass's recording:** hash the NVN commands from the pass's start to its end, including bound
  handles, addresses and state, but not the contents of the memory they point to. Inline uniform updates are hashed
  by their target address and size, not their data.
- **On a match with the last frame:** instead of translating each draw, record that pass's Vulkan commands once into
  a secondary command buffer (with the pass's formats as inheritance info, since rendering is dynamic) and replay it
  with `vkCmdExecuteCommands`. Descriptor sets are kept with the cached pass.
- **When a cached pass is no longer valid:** any texture or sampler registration that changes (`g_treg_ver`), a
  texture whose contents changed (the existing content checks, `tex_gen`), a pipeline not yet built, a change of
  render-target images, or a frame where the pass's previous result is still in flight and the reuse would alias it.
  On any doubt, translate normally.
- **Inline uniform updates** inside a cached pass still have to be copied every frame (their data changes), but
  without the per-draw resolving around them. If the copies dominate, keep a persistent copy per address in the
  cached pass and update it in place.
- **Switch:** `TOTK_PASS_REUSE=0/1`, with hit and miss reasons in the diagnostic log.

Expected gain: the share of replay time in passes that repeat. Step 1 gives this number before any of step 2 is
written. Size: about 400-600 lines, about a week with tests. Risk: medium, since a stale binding shows as wrong
objects or flicker; step 4 catches it.

## Step 3: replace a pass at the engine level (game-specific, local only)

Only for a hot pass that step 2 can't reuse because its draw list changes every frame (foliage, particles, culled
geometry).

1. **Find the engine function** that records the pass: set a breakpoint on the pass's first `nvnCommandBufferSetRenderTargets`
   from the recompiled code, and walk up the guest call stack to the loop that emits its draws.
2. **Read what it reads:** the list of objects, their meshes and the per-object data it fills in. The structure
   offsets come from the recompiled code and stay local, in a file the build reads from the player's game folder.
3. **Hook that function** (the native DLLs already let a block be replaced) and emit Vulkan directly from the engine's
   data: one instanced draw per mesh, a buffer of per-object data the GPU reads, and the game's own translated
   shaders from the shader cache. The NVN recording and its translation for that pass are both skipped, so this
   also saves the game's own CPU time for the pass.
4. **Fallback:** if the hook sees anything it doesn't recognise (a new object type, a flag it doesn't handle), it
   calls the original function for that frame.

Size: a few days per pass, most of it verification. Risk: high (visual differences, game updates break offsets;
only version 1.0.0 is supported anyway). Do this for at most 2-3 passes, and only if step 2 left a large hot pass.

## Step 4: checks before a switch goes on by default

- **Pixel comparison, automatic:** the same frame rendered with the switch off and on, each pass's colour and depth
  targets read back and compared numerically (largest difference, mean difference, number of differing pixels). The
  sampled-patch logging in `vk_exec.c` (`pass_output_sample`) is extended to full read-backs for the passes under
  test. No screenshots are viewed; only the numbers are compared. Passes must match exactly (step 2) or within a
  small tolerance (step 3, where the order of floating-point work changes).
- **Play runs:** 200-second runs with scripted input on the Deck (open-area save) and on the PC, with no device lost,
  no new validation errors (`TOTK_VK_VALIDATE=1` on the PC) and the fps not lower than without the switch.
- **The user's eyes:** a short play session on the Deck after each switch goes on, since some differences only show in
  motion.

## Order and estimate

| Step | What | Estimate (this project's pace) | Gate |
|---|---|---|---|
| 0 | Open-area save, baseline runs | about 15 minutes (three 4-minute runs), needs the user's save | three comparable runs |
| 1 | Per-pass accounting and ranking | an hour or two | top passes are 40% or more of replay |
| 2 | Pass reuse across frames | about a week | matching pixels, replay time down |
| 3 | Engine-level replacement, 1-3 passes | a few days each | matching pixels, fps up |
| 4 | Checks, then default on | with each step | as above |

Most of the time goes into Deck test runs (a few minutes each) and verification, not into writing code.

## What this plan does not fix

- **The submit cost** (about 6 ms a frame in the open area): the kernel re-checks the game's imported memory on every
  submission, and window size and huge pages made no difference. It needs its own investigation, for example fewer
  submissions per frame or fewer imported ranges that a submission references.
- **The game's own CPU time** and the shared 15 W budget: step 3 removes some of the game's work for the replaced
  passes, but nothing else here does.
- **First-time shader translation stutter** (the drop to 9 fps): only translating ahead of time helps, and that is
  separate from this plan.
- If steps 1-3 together don't get the frame under about 30 ms, the remaining options are the dynamic frame-rate patch
  (the game never chooses 20 fps and slows down slightly instead) and a manual GPU clock on the Deck.
