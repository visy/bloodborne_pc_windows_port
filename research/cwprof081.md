# cwprof081: where a Cathedral Ward lamp warp's seconds go

Branch `agent/cwprof081`. Question: faster inflate (zlib-ng) and more post-processor threads cut the Great Bridge /
Yahar'gul / Tomb of Oedon warps by 0.4-0.9 s (research/fastload081.md, workers081.md) but left Cathedral Ward
(lamp 2402950, m24_00: 6.2-6.7 s) and Old Yharnam (2302950: 3.8-4.3 s) where they were. So file read + inflate is
not their limit. What is? Offsets are ours (bbhost - 0x400000). Structure names from bbhost
(research/bbhost/include/bbhost/engine/sprj/world_info.hpp, map_ins.hpp). No game data or player logs here.

## 1. The instrument: `BB_LOAD_PROFILE` (off by default; gpu/shim/bbport_load_trace.cpp)

`BB_LOAD_PROFILE=1` turns `BB_LOAD_TRACE` on (the per-step lines of reload081) and adds `Load profile:` lines:

- the loading screen's own clock: a 4 ms poller of the party director's "loading screen" condition (NowLoading
  requested `*0x556286B` and no in-game frame, helper `*0x553E8B8` +0x50 / +0x51) prints `SCREEN UP` / `SCREEN
  GONE` (what tools/mp/loadab.py measures), every NowLoading state change (+0x804: 1 raising, 3 dropping) with the
  raise -> up and drop -> gone times, frames and fps over the screen, the IO / GPU counters, and at `GONE` every
  thread's CPU time over the screen by name (the game names its threads: `Core.Res.PostProcessor_N`,
  `Core.Res.CacheableFileLoader`, `CSChrBackInitializer`, `GXRenderThread`, `GXWorker:N`, `EzWorkPool_*`; ours
  `bb:*`, `shadPS4:*`; the game's main thread is `main`);
- per step: the eight busiest threads by name, the main thread's busy / blocked split of the step's wall time
  (GetThreadTimes) and the step's fps;
- what the world-ready gate (0x19E8190, the MoveMapStep 9 -> 10 wait) is waiting on, read every frame from the
  MoveMapStep update hook (section 2): every WorldBlockRes / WorldAreaRes state change with the count of the
  block's file caps still loading, the map collision entries not yet ready, the MapIns (map parts) blocks' two
  pass cursors and the parts requested but not loaded, the obj blocks, and WorldChrMan's chr-load budget;
- `BB_LOAD_PROFILE=2` adds the sampler of `BB_LOAD_TRACE=2`, bucketed per step (the main thread always sampled, 8 ms
  period, 4 KB of stack scanned). The sampler is its own thread (`bb:LoadSampler`) and costs ~0.5 core and 1-3 s
  per load, so timing sessions run with `=1` and attribution sessions with `=2`.

Summary: `python tools/mp/loadprof.py <run.log>` (`--steps`, `--blocks`, `--samples`, `--threads`): one row per
load with the phase split (pre = InGameStep 8/2/4/6 + MoveMapStep 0..3, 3-9, 9-10, 10-13), fps, MB read, cores
busy, the longest step's main-thread busy / blocked, and the thread families' CPU.

Cost when off: nothing is hooked or polled (the env read in `Install`).

## 2. What MoveMapStep 9 -> 10 waits on (0x19E8190, decompiled)

Called every frame by steps 9, 10, 11, 12 and 13 with (FieldArea `*0x553B148`, block index). It rebuilds the
load / draw group masks of the selected blocks (WorldRes `FieldArea+8`: +0x2B30 / +0x2B38 WorldBlockRes[0x420];
WorldInfo `FieldArea+0x10`), copies them into the four per-block managers, then returns the AND of four answers:

| part | manager | per block | ready when | function |
|---|---|---|---|---|
| collision | MapCollisionManager `*0x553E850` (+0x20 / +0x28 groups[0xB8], +0x78 / +0x80 entries[0x138]) | entries in the load groups | +0xD8 == 6 and (+0x118 bit 3 or +0x100 == 6); draw-only entries +0xD8 == 6 | 0x1886B50 |
| map parts | MapIns manager `*0x553B158` (+0x10 / +0x18 areas[0x20] -> +0x10 / +0x18 blocks[0x198]) | MapInsBlock: +0x150 parts, +0x158 records[0x48] (MapIns at +0x40) | +0x180 (every part in the masks has backread bits +0x190 0 and 1-2 and its +0x1A8 file list at state 4) and +0x184 (both passes done) | 0x15934D0 |
| objs | obj manager `*0x553B168` (blocks[0x1F8]) | | +0x120 | 0x15B2B20 |
| chrs | WorldChrMan `*0x553E878` (+0x278 / +0x280 per-block chr lists) | every chr | chr vfunc +0x78 true; +0x5F74 of +0x5F70 chr loads used this frame | 0x19192A0 |

Two per-frame budgets live in these: 0x15934D0 issues at most 20 part backread requests (0x1587ED0) per call and
advances its two cursors (+0xB8 / +0xBC) across frames, so a pass over N parts takes N / 20 frames; 0x19192A0
counts chr loads against WorldChrMan +0x5F70 per frame.

The WorldBlockRes state machine (0x155AAE0, bbhost names): 0 Unloaded, 1 BeginLoad (files requested), 2 Loading
(waits for the loading record's msb / context caps +0xD8 / +0xE0 at state 4), 3 Preparing (every cap of the block
at state 4: the six record caps, three base files, 75 partition files, the extra list +0x3EC / +0x3F0, the event
files of SprjEmkResMan; the nvm prepare +0x150 and the gparam bank), 4 ActivationDecision, 6 Activating (one frame:
collision entries registered with FrpgNetMan, partition slot picked), 7 Resident. A lamp warp selects the
destination block (`selected 1`) and its neighbour (`desired 1`): both m24_00 and m24_01 are loaded by a Cathedral
Ward warp and by a Tomb of Oedon / Great Bridge warp alike.

## 3. Sessions

(filled in below)
