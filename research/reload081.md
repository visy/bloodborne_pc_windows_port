# reload081: what a map load waits on, and what would make loads (so party joins and warps) faster

Branch `agent/reload081`. Measured live with a new diagnostic (`BB_LOAD_TRACE`, off by default) on three solo runs
(heavy route, 25 lamp loads in all) and one 2-player loopback run (12 host travels, the guest following). Offsets
are ours (bbhost - 0x400000). Earlier docs this builds on: `research/remp081.md` (the step machines, section 3),
`research/reseam081.md` (seam A-D), `research/perf081/` (the routes). No game data or log excerpts of players here.

## 0. Summary

- A lamp load is **one phase**: MoveMapStep step 9 -> 10, the wait for "world ready" (0x19E8190), takes 60-85 % of
  the loading screen (1.3-7.0 s of 2.2-8.6 s). Everything else is about 1 s of serialized one-frame step work.
- That phase is **CPU-bound decompression on two game threads**: 57-60 % of each of the two worker threads is
  zlib `inflate_fast` (0x26219D0, the engine's own zlib 1.2.3 copy at 0x2067AC0), another 15-20 % the rest of
  inflate, and ~18 % waiting for the next job. File reads are not it: 200-640 MB per load are read in 0.1-0.4 s
  (warm) / 0.8-1.7 s (first visit), on an IO thread that is mostly idle. Pipeline compiles are 0 after the
  first load; image uploads are under 30 MB per load.
- **No fixed timer** is left in a solo lamp load (the 12 s death floor is already cut by `BB_FAST_RESPAWN`). The
  only vanilla waits are conditions (file done, block state 7, world ready, own chr ready).
- The ~26 s guest leg (Dream -> Tomb) **did not reproduce**: in 12 party travels here the guest's screen was
  3.2-7.0 s, within 0.5 s of the host's, and the guest waited 0.0-0.5 s for the host (parallel follow). On one PC
  both games decompress at once, so 2p loads are 20-40 % slower than solo here; on two PCs they would not be.
  Candidates for 26 s are the parallel follow's fallbacks (section 4), not the load itself.
- Ranked opportunities (section 5): more decompression workers (the biggest, ~2-3 s per big load, needs RE of the
  FD4 file repository's work groups), a faster inflate (zlib-ng, ~1-1.5 s), a frame cap on the loading screen
  (frees two cores on small CPUs), the ~1 s of one-frame steps (the game's own construction, hard), and the
  cold-visit extra (the engine's own resource cache, not the OS).

## 1. The instrument: `BB_LOAD_TRACE`

`gpu/shim/bbport_load_trace.cpp`, off by default. Prologue hooks (diagnostic "probe" names, outside the party's
hook-set check) on the MoveMapStep update (0x19386E0; the step index is +0xC) and the InGameStep step functions
2 (0x1928C90), 4 (0x1929180), 6 (0x1929220) and 8 (0x1929C10). One `Load trace:` line per step change with the
deltas since the previous one: wall time, frames, file opens / reads / MB / wall time inside the host reads
(`runtime_file_io_stats`, new), shader compiles (`g_bb_compiles_total`, new monotonic counters next to the frame
stats' ones), images registered / uploaded, process CPU time; the MoveMapStep's hold (+0x278) and others-gate
(+0x27C) timers and the NowLoading object's state / style / timer; a `LOAD TOTAL` line at step 13.
`BB_LOAD_TRACE=2` adds the four busiest threads per step (Toolhelp CPU deltas) and a sampler: while a MoveMapStep
is between steps 0 and 12, the busiest threads of the last 250 ms are sampled every 4 ms (suspend, context, 1 KB
of stack, resume), guest samples bucketed by function entry (nearest `push rbp` prologue), host-module samples
by their first guest return address and their first bbport.exe return address; printed at step 13 with the
thread's name. Offline: `deps/ghidra-export/functions.tsv` names the guest entries, `addr2line -e
out/bbport.exe.debug` the exe offsets (image base 0x140000000).

Runs (not committed, under `agents/reload081/perf/`): `solo_trace1` (2 heavy cycles, 13 loads, trace level 2
without the sampler), `solo_trace2` / `solo_trace3` (1 cycle each, sampler), `2p_trace1` (2 heavy cycles, both
games traced). Frame cap `uncap` (the perf route's default), 720p, `BB_STALL_PROBE=100`, 16-thread CPU, NVMe.

## 2. The phase table of a lamp load (solo)

Per load from `solo_trace1` (`[+s]` harness clock; ms per phase). "pre" = InGameStep 8 -> 2 -> 4 -> 6 plus
MoveMapStep ctor and steps 0 -> 3 (each a single frame of 70-150 ms of main-thread work); "3-9" = steps 3 -> 9
(stage files, placement, the chr build: 28-58 frames); "9-10" = the world-ready wait; "10-13" = ground probe,
first chr update, the screen gate and InGameStart (3 frames of ~90 ms each plus the gate's 8-10 frames).

| load | map | screen s | pre | 3-9 | 9-10 | 10-13 | MB read | ms in reads | CPU s | CPU cores busy in 9-10 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | m24_01 (Great Bridge), first visit | 7.5 | 667 | 452 | 6058 | 301 | 616 | 436 | 29.0 | 4.3 |
| 2 | m24_00 (Cathedral Ward), first | 8.6 | 755 | 336 | 6998 | 447 | 638 | 1659 | 39.0 | 5.2 |
| 3 | m23 (Old Yharnam), first | 4.9 | 727 | 369 | 3447 | 217 | 336 | 845 | 19.2 | 4.9 |
| 4 | m21 (Dream) | 2.5 | 453 | 431 | 1297 | 239 | 199 | 97 | 4.4 | 2.4 |
| 5 | m28 (Yahar'gul), first | 4.2 | 385 | 502 | 3105 | 205 | 472 | 795 | 19.0 | 5.5 |
| 6 | m24_01 (Tomb of Oedon) | 3.7 | 397 | 235 | 2807 | 185 | 451 | 404 | 16.1 | 5.4 |
| 7 | m24_01 (Great Bridge) | 3.3 | 390 | 317 | 2378 | 184 | 375 | 162 | 13.5 | 5.3 |
| 8 | m24_00 | 5.0 | 396 | 427 | 3954 | 214 | 640 | 289 | 22.0 | 5.3 |
| 9 | m23 | 3.2 | 411 | 437 | 2107 | 197 | 338 | 161 | 12.2 | 5.3 |
| 10 | m21 | 2.2 | 422 | 245 | 1250 | 187 | 199 | 82 | 4.8 | 2.7 |
| 11 | m28 | 3.9 | 399 | 316 | 3000 | 206 | 472 | 208 | 18.6 | 5.6 |
| 12 | m24_01 | 3.7 | 410 | 234 | 2813 | 202 | 451 | 207 | 16.2 | 5.3 |

Read as: the screen is up for pre + 3-9 + 9-10 + 10-13 (+0.1 s). The one-frame steps cost ~0.9-1.1 s per load on
the main thread whatever the map (teardown 70-145 ms, screen raise 70-90, load list 80-120, world singletons +
MoveMapStep ctor + step 0 100-150, steps 1 / 2 / 3 ~100 each, ground probe 86-110, first chr update 74-110,
InGameStart 90-250). Steps 1-3 (the stage file requests) wait for nothing measurable: reads are 0-20 MB there.

What step 9 -> 10 waits on (sampler, `solo_trace3`, three big loads, 4 ms samples over the whole load):

| thread | samples in guest | where |
|---|---|---|
| decompression worker A | 70-80 % | `inflate_fast` 0x26219D0 57-60 %, `inflate` 0x2067AC0 + its memcpy 8-9 %, 0x261E060 (the stream's byte fetch) 6-10 %, `inflate_table` 0x2069820 2-3 %; the rest a condition wait (0x2083E80: idle between jobs, 17-20 %) |
| decompression worker B | 70-80 % | the same split |
| main thread | ~55 % | 0x21B8710 10-12 % (the GX command stream), our own code with no guest frame 16-23 % (frame stats, party tick, the probe), the frame-end wait 0x2083E80 5-15 %, 0xE50D20 / 0x2691FD0 / 0x423A20 2-5 % each |
| file IO thread | 5-10 % | 50 % condition wait, 25 % `runtime_kernel.c` (sleep / poll), 7-8 % inside our `runtime_file.c` reads |
| two of our own threads (GPU command thread, a worker) | 0 | 95-100 % in the Vulkan driver / our texture cache: the loading screen is drawn uncapped (90-120 FPS during the load) |

So the load is **two game threads inflating DCX / BND payloads** at ~100 % while the IO thread idles. The
engine's zlib is 1.2.3 (version string at the init call in the stream reader 0x2066B70: `inflateInit2_(z, -15,
"1.2.3", 0x70)`: raw deflate, 0x70-byte z_stream; `inflate(z, 3)` per input chunk). A second zlib (1.2.7, 0x3A7BF0)
serves GFx and others and did not appear in any sample.

Cold vs warm: the first visit of a map in a session costs 2-3 s more (loads 1-3, 5 vs 6-12) with only 0.2-1.3 s
of it inside file reads; CPU for the same map is 29 s on the first visit vs 13-16 s later and 15-25 % fewer reads
later. The warm state is the engine's own resource repository (resources still resident from the previous visit),
not only the OS file cache. The orchestrator's cold/warm file numbers (m27 7.55 vs 6.52 s) are consistent: the OS
cache is worth ~0.5-1 s per big map, the rest of the first-visit cost is CPU.

Our side of the load (what the trace rules out): 0 shader compiles after the first load (the precompiled cache
holds), 1-27 MB of image uploads per load, no texture-cache collector run in any load window, the party tick
under 1 ms per frame (reseam081), our hooks' per-step cost under 20 ms (the Toolhelp snapshot of `BB_LOAD_TRACE=2`
is the largest and is itself off by default).

## 3. Vanilla waits that are pointless on PC

Everything below is static plus the live trace; nothing here was changed in this branch.

| what | where | current cost | proposed change | risk | kill switch |
|---|---|---|---|---|---|
| Death respawn floor 12.0 s | rodata 0x49280C8, read at 0x1938B68 (remp081 3.3) | 0 now: `BB_FAST_RESPAWN` (default 2.0 s) shipped in 0.8.1; the death load measured 2.1-2.3 s in the baseline route | - | - | `BB_FAST_RESPAWN=0` |
| "Others ready" gate 15 s | +0x27C, written 0x1938B7C, compared at 0x193A562 | 0 in solo (the timer runs out unseen; trace: 14.97 s at step 2, never reached in step 12); in a party the gate passes when every human task is past step 3, and the seam's native hold uses +0x278 instead | nothing | - | - |
| NowLoading minimum display | +0x17C0 of *0x5562878 | 0: the timer read -0.02 .. -0.03 s at every step in every load (it is never armed on this path) | nothing | - | - |
| Fade to black before the screen, fade in after | SprjFade objects (step 0 writes {0, 1.0} pairs at +0x14 / +0x24; InGameStep 2 the same) | lamp press -> screen up 0.6-0.7 s (director log) and the screen's own fade; not inside the step table | a shorter fade could save ~0.3-0.5 s per load but is cosmetic and visible | low | would be `BB_FAST_FADE=0` |
| The one-frame steps | InGameStep 8, 2, 4, 6; MoveMapStep ctor + 0, 1, 2, 3, 10, 11, 12 | ~1 s per load, each a main-thread frame of 70-150 ms of the game's own construction / teardown (world singletons, stage object, file requests, ground probe, first chr update, InGameStart Lua) | none that is small: they are real work, serialized by the step machine; the ctor + step 0 (105-150 ms) and InGameStart (90-250 ms) are the two worth a profile | - | - |
| Serialized IO | the FD4 file repository (bbhost `fd4/file.hpp`: 5 stream queues, 5 work groups, per-stream in-flight limits `values_13c[2][5]` from the seed's virtual +0xC0, applied by the update 0xF80850 bbhost) | reads are not on the critical path (0.1-0.4 s warm inside reads, IO thread idle); the limits matter for the decompression below | see 5.1 | - | - |
| Decompression on two threads | the two workers that run 0x2066B70 / 0x2065CF0 -> inflate; the work groups created by the repository ctor (0xF7F8D0 bbhost) | 2.4-7.0 s per big load with 14 hardware threads idle | 5.1 | medium | `BB_LOAD_WORKERS=0` (the game's count) |
| Loading screen rendered uncapped | our frame cap: during the load two of our threads are at 100 % drawing the loading screen | free on a 16-thread CPU; on 4-6 core CPUs it competes with the two inflate threads and the main thread | 5.3 | low | `BB_LOAD_FPS_CAP=0` |

## 4. The ~26 s Dream -> Tomb guest leg

Not reproduced. `2p_trace1` (loopback, both games rendered, parallel follow on by default, 2 heavy cycles):

| host travel to | host screen s | guest screen s | guest own load s | guest waited for the host s |
|---|---|---|---|---|
| Great Bridge m24_01 | 5.9 | 6.4 | 7.8 | 0.0 |
| Cathedral Ward m24_00 | 6.9 | 7.0 | 8.5 | 0.1 |
| Old Yharnam m23 | 4.4 | 4.3 | 6.2 | 0.5 |
| Dream m21 | 2.9 | 3.2 | 4.4 | 0.0 |
| Yahar'gul m28 | 5.7 | 5.7 | 7.3 | 0.0 |
| Tomb of Oedon m24_01 (from Yahar'gul) | 5.3 | 5.2 | 6.6 | 0.2 |
| second cycle, the same six | 6.9 / 4.4 / 3.7 / 7.7 / 5.6 | 6.9 / 4.2 / 3.4 / 5.8 / 5.8 | 8.3 / 5.7 / ... | 0.2 / 0.4 / ... |

The guest's screen ends within 0.5 s of the host's; the guest's "own load" number counts from its early screen,
so it is the host's lamp-press-to-screen 0.6 s plus the load. The seam2 run `lbpf_a` (2026-11-10, same branch
state) shows the same: Dream -> Tomb guest 6.4 s screen, 7.4 s own load, 0.6 s of waiting. Both games on one PC
decompress at the same time, which is why every 2p load here is 1-2 s slower than the same solo load (CPU 33 s
for a Cathedral Ward load vs 22-39 s solo) and why the second visit is not faster in 2p; on two PCs the guest's
load is a solo load.

Where 26 s can come from on this code (each leaves a line in the guest's log; worth checking in the run that
measured it):

1. The parallel follow's fallback: no host `dest` within 30 s, or a different destination map, -> today's
   AwaitSnapshot path = host load + the snapshot + a second guest load (~14 s in reseam081 run 1), under one
   screen (`FALLBACK` / `AwaitSnapshot` lines).
2. The hold caps: the InGameStart hold (`kHoldCapS` 60 s in party_seam.cpp) and the settled-drop cap keep the
   screen while the guest is not yet beside the host (`hold (...): over N s` lines); a guest whose own load ends
   far from the host's spot (another block, a lift) waits for the arrival warp.
3. A host death or a boss-fight load keeps the old serial path (vanilla respawn: 2 s floor + load, then the
   guest's load after the snapshot).
4. A title join into the Dream followed by the first travel (`^1` in the heavy route): the join's own screen
   (3.9 s here) overlapping the travel is logged as one long screen by `loads.csv` when the up / gone pairs
   interleave.
5. Two games on one PC with a texture-budget or frame-cap setting that starves the decompression threads
   (the 2p loads are CPU-bound on the sum of both games).

## 5. Ranked opportunities (seconds per load, evidence)

| # | what | saves per big load | evidence | risk / size | kill switch |
|---|---|---|---|---|---|
| 1 | **More decompression workers.** The two threads that inflate run at ~80 % of the 9 -> 10 phase while 12+ hardware threads idle; the work is per file (two files inflate in parallel today). Raise the worker count of the file repository's decompression work group (bbhost `FD4FileRepository::work_groups[5]`, created in the ctor 0xF7F8D0 bbhost, our image: the containing function 0xB7F3D0 calls 0xB722E0 per group with the queue pair at +0x400 / +0x500) and the per-stream in-flight limit (`values_13c`, seed virtual +0xC0) to min(cores - 4, 6). | ~2-3 s of a 5-7 s first-visit load, ~1-1.5 s of a 3-4 s warm load (the phase scales with 1/workers until the main thread's 55 % or the IO thread binds) | the sampler: both workers 57-60 % inflate_fast + 15-20 % inflate, 18 % idle; CPU 4.3-5.6 cores busy of 16; reads 0.1-0.4 s | medium: the game's allocator and the stream queues are shared across workers (they already are, with two); needs the RE of the ctor's thread-count argument and one A/B run; size M | `BB_LOAD_WORKERS=0` |
| 2 | **Faster inflate.** Hook the engine's zlib entry points (inflateInit2_ 0x2067900, inflate 0x2067AC0, inflateReset / End in 0x2066000-0x206A000; the z_stream is the standard 0x70-byte layout, raw deflate windowBits -15, `inflate(z, 3)` on 4-64 KB input chunks into the caller's output buffer) with zlib-ng's streaming inflate (our own state behind z_stream.state). libdeflate is 2-3x faster but has no streaming API; the reader feeds chunks, so it does not fit without buffering the whole file. | ~1-1.5 s per big load (inflate ~75 % of the two workers; zlib-ng's inflate is 1.3-1.6x 1.2.3's on x86-64) | sampler; the stream reader 0x2066B70 decompiled | medium: a vendored dependency (zlib-ng is not in MSYS2 here; libdeflate 1.26 is), the z_stream contract (total_in / total_out / adler / msg) must be kept for the reader's checks; size M | `BB_FAST_INFLATE=0` |
| 3 | **Frame cap on the loading screen.** During a load two of our threads draw the loading screen at 90-120 FPS (uncapped runs) and the main thread spends ~15 % in the frame end. Cap the loading screen at 30 FPS (NowLoading +0x804 != 0 or the MoveMapStep between steps 0 and 12) and hand the cores to the workers. | 0 on this 16-thread PC; up to ~1 s on 4-6 core CPUs (both inflate threads + main + our GPU thread + present already fill 6) | sampler threads 148476 / 145176 at 95-100 % in the driver during loads; `cpu/9-10` 4.3-5.6 cores | low: the loading screen animation only; size S; measure on a core-limited run (`BB_CPU_AFFINITY` / start /affinity) | `BB_LOAD_FPS_CAP=0` |
| 4 | **The one-frame steps** (~1 s per load): the biggest are the ctor + step 0 (105-150 ms: fade setup, respawn-point file request, net flow reset) and InGameStart (90-250 ms: Lua `InGameStart`, the record broadcast). A profile of each with the sampler at 1 ms would say whether any is our hook (the InGameStart clear hook, the step-7 placement hooks) or the game's. | 0.1-0.3 s if one of them is ours; otherwise nothing small | the phase table: every load pays 0.9-1.1 s here regardless of map | low to look, unknown to fix; size S for the look | - |
| 5 | **Read ahead for the first visit** (remp081 #6): the OS cache is worth 0.5-1.3 s on the first visit of a big map (ms in reads 436-1659 vs 160-400 later); a prefetch of the destination map's files while the guest waits for the host (title join, bell wait) hides it. | 0.5-1 s per first visit, only when the guest has idle time before its load | trace reads column | low; size S-M (the file list per map comes from the game's own load list, read at InGameStep 4) | `BB_PREFETCH_MAP=0` |
| 6 | Shorter fades (0.3-0.5 s) | cosmetic | director timestamps | low but visible | `BB_FAST_FADE=0` |

Not worth it (measured): file read path changes (sequential-scan hints, bigger reads: the IO thread idles),
shader compiles (0 after the first load), texture uploads (under 30 MB per load), the party tick (under 1 ms
per frame), the 15 s others gate and the NowLoading timer (never reached).

## 6. What was committed

- `BB_LOAD_TRACE` (gpu/shim/bbport_load_trace.cpp / .h, wired in bbgpu_patch_image after the stall probe),
  `runtime_file_io_stats` (src/runtime_file.c: read wall time counted), `g_bb_compiles_total` /
  `g_bb_compile_ns_total` (vk_pipeline_cache.cpp), the DEBUG_SWITCHES row, `changes.d/reload081.md`.
- No gameplay or engine behaviour changed; nothing is on by default. The opportunities above are proposals
  with numbers, not implementations (none of 1-3 is small enough to be "clearly proven" without its own A/B run).

## 7. Next steps, in order

1. Item 1: decompile the repository ctor at the bbhost RVA 0xF7F8D0 (our image puts it inside 0xB7F3D0, so the
   mapping of this region needs the +0x400000 check) and 0xB722E0 (the work-group ctor: thread count, stack,
   priority), find the SPRJ seed's +0xC0 / +0xC8 getters, then A/B `BB_LOAD_WORKERS=4` on the heavy route
   (12 loads per variant, one session each). Read the live values first: `*0x553B118` (SprjFile) -> repository
   -> +0x13C / +0x164 / +0x1F4.
2. Item 3 on a core-limited run (start the game with an affinity of 6 threads) to see whether the loading
   screen's draw cost binds there.
3. Item 2 only after 1: with more workers inflate stops being the single bottleneck.
