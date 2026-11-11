# cwprof081: where a Cathedral Ward lamp warp's seconds go

Branch `agent/cwprof081`. Question: faster inflate (zlib-ng) and more post-processor threads cut the Great Bridge /
Yahar'gul / Tomb of Oedon warps by 0.4-0.9 s (research/fastload081.md, workers081.md) but left Cathedral Ward
(lamp 2402950, m24_00: 6.2-6.7 s) and Old Yharnam (2302950: 3.8-4.3 s) where they were. So file read + inflate is
not their limit. What is? Offsets are ours (bbhost - 0x400000). Structure names from bbhost
(research/bbhost/include/bbhost/engine/sprj/world_info.hpp, map_ins.hpp). No game data or player logs here.

Answer in one line: the Cathedral Ward wait is **frame-bound**: the map-parts gate of the world-ready check walks the
destination block's 4048 map parts with a budget of 20 parts per frame, twice, and then once more after a mask
change, ~300 loading-screen frames whatever the CPU does; the control warps touch ~150-200 parts and are bound by
files + inflate instead.

## 1. The instrument: `BB_LOAD_PROFILE` (off by default; gpu/shim/bbport_load_trace.cpp)

`BB_LOAD_PROFILE=1` turns `BB_LOAD_TRACE` on (the per-step lines of reload081) and adds `Load profile:` lines:

- the loading screen's own clock: a 4 ms poller (`bb:LoadPoller`) of the party director's "loading screen" condition
  (NowLoading requested `*0x556286B` and no in-game frame, helper `*0x553E8B8` +0x50 / +0x51) prints `SCREEN UP` /
  `SCREEN GONE` (what tools/mp/loadab.py measures), every NowLoading state change (+0x804: 1 raising, 3 dropping)
  with the raise -> up and drop -> gone times, frames and fps over the screen, the IO / GPU counters, and at `GONE`
  every thread's CPU time over the screen by name (the game names its threads: `Core.Res.PostProcessor_N`,
  `Core.Res.CacheableFileLoader`, `CSChrBackInitializer`, `GXRenderThread`, `GXWorker:N`, `EzWorkPool_*`,
  `HavokWorkerThread`; ours `bb:*`, `shadPS4:*`; the game's main thread is `main`; the runtime cuts names at 15
  characters);
- per step: the eight busiest threads by name, the main thread's busy / blocked split of the step's wall time
  (GetThreadTimes) and the step's fps;
- what the world-ready gate (0x19E8190, the MoveMapStep 9 -> 10 wait) is waiting on, read every frame from the
  MoveMapStep update hook (section 2): every WorldBlockRes / WorldAreaRes state change with the count of the block's
  file caps still loading, the map collision entries not yet ready, the MapIns (map parts) blocks' two pass cursors
  and the parts requested but not loaded, the obj blocks, and WorldChrMan's chr-load budget;
- `BB_LOAD_PROFILE=2` adds the sampler of `BB_LOAD_TRACE=2`, bucketed per step (the main thread always sampled, 8 ms
  period, 4 KB of stack scanned). The sampler is its own thread (`bb:LoadSampler`; in s1 it was the unnamed thread
  with 7.8 s of CPU per load) and costs ~0.5 core and 1-3 s per load, so timing sessions run with `=1` and
  attribution sessions with `=2`. Its idle Toolhelp snapshot is now every 500 ms, not every 20 ms.

Summary: `python tools/mp/loadprof.py <run.log>` (`--steps`, `--blocks`, `--samples`, `--threads`): one row per
load with the phase split (pre = InGameStep 8/2/4/6 + MoveMapStep 0..3, 3-9, 9-10, 10-13), fps, MB read, cores
busy, the longest step's main-thread busy / blocked, and the thread families' CPU.

Cost when off: nothing is hooked or polled (the env read in `Install`). Cost when on (`=1`): ~0.5 s on a big load
(s3 vs the earlier harness numbers), the per-frame probes on the main thread.

## 2. What MoveMapStep 9 -> 10 waits on (0x19E8190, decompiled)

Called every frame by steps 9, 10, 11, 12 and 13 with (FieldArea `*0x553B148`, block index). It rebuilds the
load / draw group masks of the selected blocks (WorldRes `FieldArea+8`: +0x2B30 / +0x2B38 WorldBlockRes[0x420];
WorldInfo `FieldArea+0x10`), copies them into the four per-block managers, then returns the AND of four answers:

| part | manager | per block | ready when | function |
|---|---|---|---|---|
| collision | MapCollisionManager `*0x553E850` (+0x20 / +0x28 groups[0xB8], +0x78 / +0x80 entries[0x138]) | entries in the load groups | +0xD8 == 6 and (+0x118 bit 3 or +0x100 == 6); draw-only entries +0xD8 == 6 | 0x1886B50 |
| map parts | MapIns manager `*0x553B158` (+0x10 / +0x18 areas[0x20] -> +0x10 / +0x18 blocks[0x198]) | MapInsBlock: +0x150 parts, +0x158 records[0x48] (MapIns at +0x40) | +0x180 (every part in the masks has backread bits +0x190 0 and 1-2 and its +0x1A8 file list at state 4) and +0x184 (both passes done) | 0x15934D0 |
| objs | obj manager `*0x553B168` (blocks[0x1F8]) | | +0x120 | 0x15B2B20 |
| chrs | WorldChrMan `*0x553E878` (+0x278 / +0x280 per-block chr lists) | every chr | chr vfunc +0x78 true; +0x5F74 of +0x5F70 chr loads used this frame (-1 = no limit here) | 0x19192A0 |

0x15934D0 (the MapIns block update) restarts its two passes over the block's parts whenever the block's masks changed
(XOR of the new and old 32-byte load / draw masks), walks the parts from the cursors +0xB8 (pass 1) / +0xBC (pass 2),
and for every part inside the masks issues a backread request (0x1587ED0: state 3 clears, 1 / 2 queue into
`MapInsUpdateHelper`, whose assert `MapInsUpdateHelper.cpp(0xcc)` fires when its queue is full) and spends one unit of
a **per-call budget of 20 (`0x14`)**; the budget spent, it returns and continues next frame. +0x184 is set when both
cursors reached the end; +0x180 is cleared by any touched part whose resources are not loaded.

The WorldBlockRes state machine (0x155AAE0, bbhost names): 0 Unloaded, 1 BeginLoad (files requested), 2 Loading
(waits for the loading record's msb / context caps +0xD8 / +0xE0 at state 4), 3 Preparing (every cap of the block
at state 4: the six record caps, three base files, 75 partition files, the extra list +0x3EC / +0x3F0, the event
files of SprjEmkResMan; the nvm prepare +0x150 and the gparam bank), 4 ActivationDecision, 6 Activating (one frame:
collision entries registered with FrpgNetMan, partition slot picked), 7 Resident. A lamp warp selects the
destination block (`selected 1`) and its neighbour (`desired 1`): both m24_00 and m24_01 are loaded by a Cathedral
Ward warp and by a Tomb of Oedon / Great Bridge warp alike (the same 2 blocks, the same 194 collision entries).

## 3. Sessions (one solo game per session, through tools/mp/loadab.py; this PC: 16 threads, NVMe, 720p)

Route per session: Continue, then the lamps Cathedral Ward (2402950, m24_00), Tomb of Oedon (2412953, m24_01),
Cathedral Ward, Great Bridge (2412952, m24_01), and so on: 5 Cathedral Ward warps and 4 control warps (2 Tomb, 2
Bridge) per session, 20-30 s apart, no restart between data points. zlib-ng on, the game's 3 post-processors (the
defaults). Logs under `agents/cwprof081/prof/` (not committed).

| session | preset | profile | Cathedral Ward screen s (5 warps) | control screen s (4 warps) |
|---|---|---|---|---|
| s1_60 | 60 | `=1` + the old 4 ms sampler | 8.2-9.0 | 4.8-8.8 |
| s2_uncap | uncapped | `=1` | 4.67 / 4.86 / 5.37 / 5.41 / 6.42 (mean 5.35) | 2.64 / 2.99 / 3.56 / 3.71 (mean 3.22) |
| s3_60 | 60 | `=1` | 7.38 / 7.47 / 7.19 / 7.05 / 6.91 (mean 7.20) | 3.61 / 3.09 (Tomb), 3.82 / 3.32 (Bridge) (mean 3.46) |
| s4_60s | 60 | `=2` (sampler, 8 ms) | 8.61 / 8.00 / 8.31 / 8.15 / 7.95 (mean 8.20) | 3.97 / 4.35 / 4.38 / 4.60 (mean 4.32) |

s3 is within 0.5 s of the earlier harness numbers (fastload081 / workers081 at 60 fps: Cathedral Ward 6.5-6.7, Tomb
2.9-3.3, Bridge 2.4-3.3), so `=1` is the timing profile; `=2` costs 1-3 s more and is for attribution only.

## 4. Where the seconds go (s3, 60 FPS preset; s2, uncapped; means per map)

| phase (what the step does) | Cathedral Ward 60 | control 60 | Cathedral Ward uncapped | control uncapped |
|---|---|---|---|---|
| lamp press -> screen up (the fade-out, before the screen) | 0.60-0.66 | 0.60-0.66 | 0.60 | 0.61 |
| pre: InGameStep 8 / 2 / 4 / 6, MoveMapStep 0..3 (teardown, load list, world singletons; one frame each) | 0.87 | 0.91 | 0.88 | 0.90 |
| 3 -> 9: stage files, placement, chr build | 0.55 | 0.52 | 0.37 | 0.30 |
| **9 -> 10: the world-ready wait (0x19E8190)** | **5.45** | **1.71** | **3.03** | **1.72** |
| 10 -> 13: ground probe, first chr update, screen gate | 0.22 | 0.21 | 1.01 (a second map-part sweep in step 11) | 0.25 |
| screen gone -> step 13 (the fade-in) | 0.00 | 0.00 | 0.00 | 0.00 |
| **screen up -> gone** | **7.20** | **3.46** | **5.35** | **3.22** |
| frames on the screen / fps | 360 / 50 | 127-165 / 40-44 | 420-520 / 81-110 | 230-330 / 74-91 |
| file reads (count / MB / ms inside reads) | 13-17 k / 617-757 / 260-470 | 7-9 k / 321-462 / 150-265 | the same / 320-404 ms | the same / 170-300 ms |
| GPU: pipeline compiles / image uploads | 0 / 17-219 images, under 1.5 MB | 0 / 19-126 images | 0 | 0 |
| cores busy over the screen (sum of thread CPU / wall) | 2.1-2.8 | 2.4-2.8 | 3.6-3.9 | 3.5-3.7 |
| main thread in 9 -> 10 (GetThreadTimes) | busy 3.3-4.1 s of 5.4-5.6 s (65 %), blocked 1.2-2.1 s | busy 1.0-1.4 of 1.4-2.0 s | busy 2.1-2.8 of 2.6-3.7 s (80 %) | busy 1.1-1.7 of 1.3-2.1 s |

Thread CPU over the screen, ms (s3: load 4 Cathedral Ward / load 3 Tomb of Oedon): main 5422 / 2844, the two busy
Core.Res.PostProcessors 2391 + 2297 / 1250 + 1125 (the third one 94), Core.Res.CacheableFileLoader 906 / 484,
shadPS4:GpuCommandProcessor 1031 / 359, bb:DrawRec 938 / 359, GXRenderThread 656 / 141, EzWorkPool 609 / 297,
CSChrBackInitializer 156 / 0, the five GXWorkers 100-160 each, HavokWorkerThreads under 200. The Cathedral Ward load
has twice the files (617-757 MB vs 321-462) and twice the loader CPU, but those threads are at 40-45 % of a core
each over a wait whose last 2.7 s they spend idle (below).

Everything but 9 -> 10 is the same for both maps. The whole difference (3.7 s at 60 fps, 1.3 s uncapped) is in the
world-ready wait, and the presets split it: at 60 fps the Cathedral Ward wait is 2.4 s longer than uncapped, the
control's wait is the same. The Cathedral Ward load is **bound by the number of frames** (~360 at 50 fps, ~310 at
108 fps), not by CPU, disk or inflate.

### 4.1 What the wait is (the sub-gate probe; s3 load 4, Cathedral Ward at 60 fps, times from screen up)

| t | gate | event |
|---|---|---|
| +1.5 | WorldRes | MoveMapStep 3 -> 9; m24_00 (selected) BeginLoad -> Loading (9 caps) -> Preparing (27 caps) -> Resident in 0.24 s; m24_01 (the neighbour, desired) 0.4 s later, Resident at +1.6 s |
| +1.6 .. +2.3 | collision | 87 then 91 entries requested; +0xD8 goes 0 -> 2 -> 3 / 5 -> 6 within 0.7 s; all ready at +2.3 s |
| +1.6 .. +3.8 | objs | m24_01's obj block ready at +2.6 s, m24_00's at +3.8 s |
| +1.6 .. | chr | 34 chrs seen, no per-frame budget (+0x5F70 = -1); never the last |
| +1.6 .. +7.0 | **map parts** | MapInsBlock m24_00 has **4048 parts** (m24_01 4056). Pass 1 (cursor +0xB8) 0 -> 4048 in 0.5 s; pass 2 (+0xBC) 0 -> 4048 in **2.3 s** while the parts' resources load ("requested parts not loaded" peaks at 1415 and is back to 29 when the cursor reaches 4041: the loaders stay ahead of the cursor); then the masks change (m24_01 went resident) and the sweep **restarts**: pass 1 0.8 s, pass 2 **1.9 s with 0 parts left to load**; READY at +7.0 s = MoveMapStep 9 -> 10 |

The same probe on the Tomb of Oedon warp: m24_01's pass 2 runs 1834 -> 3926 -> done within 0.3 s ("requested parts
not loaded" peaks at 173), the sweep is over before the files are, and 9 -> 10 is the file / inflate time (1.3-2.0
s) that zlib-ng and more workers already shortened.

Why the cursor crawls: the per-call budget of 20 is spent on every part inside the block's masks, whether or not the
part still needs anything. Cathedral Ward's lamp masks cover ~2900 of m24_00's 4048 parts (two passes = ~290 frames
= 2.9 s at 100 fps, 5.8 s at 50 fps, split over the first sweep's pass 2 and the whole second sweep); the Tomb of
Oedon / Great Bridge spots cover ~150-200 parts of m24_01 (10 frames per pass). The second sweep re-touches every
part after they are all loaded: 2.7 s at 60 fps for nothing. The parts are map pieces (MapIns) whose resources
(FLVER models, textures: the 617-757 MB) are what the Core.Res threads stream; they finish before the cursor does.

The main thread's "busy 65 %" in 9 -> 10 at 60 fps is the frame itself (the loading screen's draw, the per-frame
MapIns / chr / obj updates over 8000 parts, our overlay / party tick; `=2` puts 80 % of its samples in host code: the
frame pacing and the GPU layer), not loading work. Nothing waits on disk (0.3-0.5 s inside reads of a 5.5 s wait),
nothing compiles shaders, no texture upload is on the path.

### 4.2 Critical path

Main thread -> MoveMapStep step 9 -> 0x19E8190 -> MapIns block update 0x15934D0 of the destination block: a cursor
over 4048 parts advanced 20-30 parts per frame, two passes, then both again after a mask change = ~300 frames of the
loading screen. Busy or waiting: the main thread is *busy* (drawing frames) but the load is *waiting for frames*; the
resource pipeline (file loader + 3 post-processors) is idle for the wait's last 2.7 s.

Old Yharnam 2302950 (not profiled here) is the other warp zlib-ng did not touch; the same probe on it is the first
thing to run (one `--chain 2302950,2412953,...` session).

## 5. Speed-up candidates (ranked; estimated gain on a Cathedral Ward warp at the 60 FPS preset)

| # | what | gain | risk / size | kill switch |
|---|---|---|---|---|
| 1 | **Raise the per-call part budget of 0x15934D0 while the loading screen is up** (the `0x14` immediate at the top of its parts loop; e.g. 400, or no limit while NowLoading is requested). Each pass over the ~2900 masked parts takes 8 frames instead of 145; the wait becomes the real load (the control's 1.3-2.0 s: files + inflate, which zlib-ng / more workers already shorten). | **~3-3.5 s of 7.2 s at 60 fps, ~1-1.5 s of 5.35 uncapped** (9 -> 10 from 5.45 to ~1.8-2.2 s); Old Yharnam likely similar | medium, size S: a byte patch behind a flag. The budget throttles `MapInsUpdateHelper`'s request queue (its assert fires when `count >= capacity`: read the capacity `*helper` at runtime first and keep the budget below it); in-world block crossings keep the game's 20 so streaming hitches cannot appear. Prove with loadab 60 / uncap plus one in-world walk (Great Bridge -> Cathedral Ward on foot) for hitches and missing geometry | `BB_LOAD_PARTS_BUDGET=0` |
| 2 | **Uncap the loading screen** (`BB_FAST_LOADING=1` exists: no SprjFlipper sleep while NowLoading is up; off by default). The crawl is per frame, so more frames = a shorter wait: the s2 vs s3 delta. | 1.85 s at 60 fps (7.20 -> 5.35), 0.25 s on the control; nothing once #1 is in (frames then no longer bind) | low, size 0 (a default change; fastload081 kept it as an A/B aid). A 4-6 core PC gains less: the uncapped screen's own frames compete with the loaders | `BB_FAST_LOADING=0` |
| 3 | Skip the redundant second sweep: 0x15934D0 restarts both passes whenever a block's masks change (the neighbour going resident changes them); the second sweep touched 4048 parts with 0 left to load (2.7 s at 60 fps). Only matters if #1 is rejected (with #1 a sweep is 2-3 frames); alternatively make the neighbour resident before the destination's sweep starts. | 2.7 s at 60 fps (subsumed by #1) | medium (ordering inside the game's own state machine), size M | - |
| 4 | Cheaper loading-screen frames: at the 60 preset the screen runs at 48-52 fps with the main thread 65 % busy (the draw, our overlay, the 8000-part MapIns updates); every ms off a frame is 300 frames x 1 ms. | 0.3-0.6 s | low, size M (profile the main thread's frame with `BB_STALL_PROBE` during a load) | - |
| 5 | More post-processors / faster inflate: nothing for Cathedral Ward now (the loaders idle for the wait's last 2.7 s; fastload081 / workers081 measured the same); they pay once #1 removes the frame bound, and then the control's numbers (0.4-0.9 s) apply | 0 now, 0.4-0.9 s after #1 | shipped (`BB_LOAD_WORKERS=4`, zlib-ng on) | - |

Not on the path (measured): file reads (0.26-0.47 s inside reads, loader thread at 15 %), inflate (two
post-processors at 40-45 %), shader compiles (0), texture uploads (under 1.5 MB per load), fades (the 0.6 s lamp
press -> screen is before the screen, the fade-in is 0 ms after the gate), the collision / obj / chr gates (all ready
3 s before the map parts), the 15 s others gate and the NowLoading timer (never bind in solo).

## 6. What was committed

- `BB_LOAD_PROFILE` (gpu/shim/bbport_load_trace.cpp: the screen poller, threads by name, the sub-gate probe, the
  sampler per step with the 8 ms period and 4 KB stack scan, its idle Toolhelp snapshot every 500 ms instead of
  20 ms), `tools/mp/loadprof.py`, the DEBUG_SWITCHES row. Off by default: nothing hooked or polled.
- No gameplay or engine behaviour changed; candidates 1-3 are proposals with numbers, not implementations.

## 7. Candidate 1 implemented: `BB_LOAD_PARTS_BUDGET` (cwfast081, gpu/shim/bbport_parts_budget.cpp, on by default)

The two budget loads of 0x15934D0 are `mov r14d, 0x14` (41 BE 14 00 00 00) at +0x1593764 and +0x1593773 (a scan of
the body for `mov r32, 0x14` finds these two and no other; both are byte-checked, all or nothing, one `Parts
budget:` line). A 4 ms poller (`bb:PartsBudget`) rewrites both imm32 to the budget when NowLoading requested
(`*0x556286B`) rises and back to 0x14 when it clears, so block crossings in the world keep the game's 20 per frame.
The write is cross-thread but safe: any torn mix of the old and new bytes is a valid budget for one call. The
requests go to the block's `MapInsUpdateHelper` queue, whose capacity equals the block's part count (4048 for
m24_00, 629 for m21_00; probe line `helper queue N of CAP`, high-water 3115 of 4048) and 0x1587ED0 never queues a
part twice (+0xC bit 0), so no budget can reach its assert. Default 4096 (more than any block's parts: a pass is
one call); `=0` / `off` the game's 20; `=N` 20..1000000.

A/B (tools/mp/loadab.py, chain Cathedral Ward / Old Yharnam / Tomb / death / Cathedral Ward / Old Yharnam / Bridge
/ Cathedral Ward / Old Yharnam, `BB_LOAD_PROFILE=1`, the lane shared with another agent's sessions the whole time,
so fps on the screen were 34-50 instead of 50; on and off ran back to back under the same load; screen seconds,
mean of 3 warps, Continue and the death respawn once):

| preset | budget | Cathedral Ward | Old Yharnam | Tomb / Bridge | death respawn | Continue |
|---|---|---|---|---|---|---|
| 60 | off (20) | 7.21 | 4.83 | 3.40 | 3.15 | 2.72 |
| 60 | 400 | 4.70 | 3.38 | 3.31 | 2.93 | 2.86 |
| 60 | **4096 (default)** | **4.40** | **3.35** | 3.37 | 2.98 | 2.45 |
| uncapped | off (20) | 5.87 | 3.22 | 3.41 | 3.12 | 2.18 |
| uncapped | 400 | 4.51 | 3.10 | 3.29 | 3.13 | 2.52 |

Cathedral Ward -2.8 s (-39 %) and Old Yharnam -1.5 s (-31 %) at 60 fps, -1.4 s / -0.1 s uncapped; the control
warps, the death respawn and Continue within noise. The map-parts probe with the budget on: both passes complete
within one probe interval (cursors `0 / 0`), "requested parts not loaded" rises to ~3000 in 0.3 s and drains over
2.5-3 s: the Cathedral Ward wait is now files + inflate (the loaders at work the whole time), so `BB_LOAD_WORKERS`
and zlib-ng now pay there as on the other maps. No panic, no assert in 5 sessions (46 loads). The sweep restart
after the neighbour's mask change still happens but costs 1-2 frames.

Walk test: section 7.1 (below) after the on-foot run.
