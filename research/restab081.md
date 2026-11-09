# restab081: crash 0x1a21134, first-load device lost, Live FPS ~64 after a respawn

Round 0.8.1, plan item P1-9. Guest offsets are our raw ELF VA of eboot.elf (bbhost minus 0x400000).
No player data, no game tables; only short decompile snippets with addresses.

## 1. Crash at guest 0x1a21134 (CL-078659)

### Evidence

- The only dump: `E:\games\bbport\mptest\inst1\out\crash_20261104_103948.log` (0.7.8 release smoke,
  phase `mw` boss_darkness, guest instance 1; the session log of that run was overwritten).
- `READ of 0x1`, instruction `cmp dword ptr [rax], 0x00`, RAX=1, thread `HavokWorkerThread`.
  RBX=RDI=0x1013e6e270, RSI=R14=0x10159d1b88, R15=0x1013e6e190.
- RBP chain: 0x1a2813f (in 0x1a27fb0) <- 0xacde32 (0xacd9b0) <- 0x99f3db <- 0x94beb4 <- 0x9b88b5 <-
  0x420965 / 0x42354d / 0x423474 / 0x4233c5 (worker loop) <- bbport.exe (thread entry).
- GUEST MEMORY: no allocation failures; no guest assertion.

### Decompiled path (disassembly checked for the register roles)

- `0x1a27fb0(edi = animation id, rsi = chr, xmm0-2 = times)`: Ghidra's signature is wrong;
  the disassembly is authoritative. It reads `chr +0x3b0` (the module block) `+0x30` (flag byte +0x384)
  and `+0x18` = the **behaviour module** (kept in **r15**). The module's `+0x10` is an animation table
  (`+0x30` count, `+0x38` pointers sorted by id; entry `{u32 id, ..., +0x10 animation object}`), binary
  searched by the id. The entry's `+0x10` goes to **r14**. With `SprjHkBehManager +0x58` set it calls
  `0x1a21100(rdi = [r15+0x18] listener, rsi = r14)`, otherwise `0x12e76a0`. On return it writes the
  event `{id, t0, t1, t2}` into a 10-slot ring at module `+0x20`, next index `+0xc0`.
- `0x1a21100` (annotation playback): `if (*(int**)(anim+0x18) && **(int**)(anim+0x18) == 0)` then
  walks the annotations between the two times (`0xf5bc30`) and calls `listener->vtbl[+0x18]`.
  The fault is the inner read: `anim +0x18` held **1**.
- Called from the behaviour graph update (0xacd9b0, an hkb generator/state machine update with the
  0x28-byte transition array at +0xe8), on a Havok worker, i.e. in the parallel behaviour jobs.

### What is stale

The chr, its module block, the behaviour module and the table were all readable and the id was
found (the search compares `*entry == id`). Only the **animation object** behind the table entry had a
non-pointer at +0x18: the object was freed (or its animation resource unloaded) and the memory reused,
while the table entry still pointed at it. So a chr's animation resource went away while its
behaviour graph still ran an event for it on a worker.

### Which chr / party code

The prior analysis (Iosefka dev text) put the fault 75 ms after the host test's ForceDead of the boss
(Mergo's Wet Nurse, entities 2600800-2600802, killed in 1% steps). Ranked:

1. **The guest's boss copies being torn down while a worker runs their behaviour event** (the
   boss dies on the guest through the game's own death/defeat sync, its parts despawn within ~100 ms of
   each other). A vanilla race the PS4 timing hides, made likelier by the test's kills: three parts in
   ~2 s instead of a fight's end. No party code touches chr +0x3b0 -> +0x18 or animation resources
   (grep: party code reads only +0x3b0 -> +0x8 / +0x20 / +0x58 / +0x68). Party `SetDisable`
   (0x18c6390) and ForceDead (0x13320c0 / 0x18bf9a0) calls run on the main thread, as the game's own
   Lua does.
2. **The host's remote PlayerIns copy on the guest** (the spectator `SetDisable` of the dead host 1.5 s
   before). Disabled, not deleted; its animations are the shared c0000 set. Less likely.
3. Our runtime: a guest heap block freed and reused early. No allocator failure in the dump; no
   evidence.

Not reproduced: 6 reruns of `mw` on 31baf629 (3 plain, 3 dsl), 20/20 rows each (prior agent).

### Diagnostic (this branch)

`src/crash_regmem.h` + `src/probe.c`: every crash dump now has **MEMORY AT REGISTERS** (64 bytes at
each register that holds a readable pointer, read through ReadProcessMemory so a bad pointer cannot
fault again) and, for a fault inside 0x1a21100, a **site decode**: the animation object's +0x18, the
behaviour module's listener / table / entry count, the last 10 behaviour events (animation ids), and
the owner chr found by its back pointer (`[module+k] = chr` with `chr +0x3b0 -> +0x18 == module`),
with its map entity id (+0x1f0) and entity id (+0x3b8). The next dump answers "boss part, remote
player or other". Crash time only, no cost while playing. The raw register bytes are opt-in (`BB_CRASH_REGMEM=1`, on in the test harness) since they can hold player text; the ids-only decode always runs.
Test: `crash-regmem-test` (tests/test_crash_regmem.c, fake memory) via tests/test_crash_regmem.py.

No guard added: a guard in 0x1a21100 would need a valid-object test on a freed pointer (the reused
memory can hold anything); the cause has to be pinned to a teardown path first.

## 2. GPU device lost on a guest's first load (CL-ab12f5)

### Evidence found

| report | when | fault | where | heap 2 (BAR) |
|---|---|---|---|---|
| `mptest\inst1\out\device_lost_20261104_123937.log` (guest instance) | 2026-11-04 12:39 | **invalid write at 0** (0..0xfff) | finished `presenter post process`, did not start the next `HLE copy shader`: in the copies / clears / barriers between | 32 MiB |
| livefps080 r4 session s8 (Iosefka text; logs deleted with the clone) | 2026-11-08 | **invalid write at 0** | the same two crumbs | ? |
| `agents\proof080\mp\inst0\out\device_lost_20261108_051733.log` (host) | 2026-11-08 | invalid read at 0x4be00000000 + 2 IPs | STUCK in an indexed draw (vs 9da6aacb / ps bb10e440) | 160 MiB |
| player reports (CHANGES 0.8.0 text) | - | write at 0 | "copies, clears, barriers" between two draws | - |

The write-at-0 signature repeats (player reports + 2 of ours) and always sits **at a frame boundary**:
after the presenter's post process pass and before the first HLE copy shader of the next frame. That
stretch holds the frame's start-up transfers (buffer-cache uploads from the stream buffer, image
uploads, CP DMA / WRITE_DATA buffer writes, label and timestamp writes), not draws or dispatches.

### What it is not

- **Not the PM4 packet-size race.** CHANGES 0.8.0 calls it "the most likely cause"; the fix is
  23b34447 (2026-11-02), and both of our write-at-0 sightings (11-04, 11-08) came on later builds.
- **Not the GX region (NW c6305d7) for this signature.** The GX range allocator is guest memory for
  CPU-updated GPU data (per-draw constants, CPU-deformed vertices). NW's Ebrietas crashes were reads of
  stale data in **draws** after it filled. Our write-at-0 is in the transfer stretch with no draw
  running, and the 11-04 report had only 32 MiB in heap 2. The 11-08 host report (a stuck draw, read
  fault) could be the GX kind; its BAR heap 160/171 MiB is our own allocations, not the GX region
  (that lives in the guest's GFX heap). The port in gxregion081 cannot be judged from these reports:
  the allocator's fill is only noted under BB_FRAME_STATS and was not in any report.
- Not a pipeline compile storm or VRAM exhaustion: heap 0 at 758-829 of 6414 MiB in both.

### Ranked hypotheses

1. A frame-start buffer copy / fill / update whose destination buffer has **no device address**
   (destroyed or never bound: GPU VA 0), recorded before its owner was released. Candidates: the
   buffer cache's upload copies into a buffer the GC / residency code released (gcport080 and the
   BB_GC_BUDGET path free VRAM copies), or a label / timestamp write (BB_GPU_TIMESTAMPS default on)
   into a buffer recycled during a load. Loads release and create the most buffers, which fits
   "first load".
2. The presenter side after the post process (frame generation prepare, upscaler dumps): only with
   FG / dumps on; not in the lfd runs (FG off). Lower.
3. GX range allocator full -> stale pointers: explains draw faults (the 11-08 host report), not
   write-at-0 in transfers.

### Diagnostics

- Existing (0.8.0): `BB_BREADCRUMBS_TRANSFERS=1` numbers every copy / fill / update / upload. It was
  off in both of our sightings. **This branch turns it on in smoke phase `lfd`** (the 11-08 sighting
  was a Live FPS respawn run), so the next one names the transfer.
- This branch: the device-lost report (and the gpu_stuck report) list the game's **GPU range
  allocators** (live ranges, MB free) after the heaps, ported from Nightwatch c6305d7 (Lantern
  Engine, credited in the code). They are filled only with BB_FRAME_STATS=1 (the collector hook is an
  int3 trap, so not on by default); the gxregion081 agent can use it to show the allocator's fill in
  an Ebrietas / first-load run.
- Suggested (not done): add BB_BREADCRUMBS_TRANSFERS=1 to the first-load phases (lift_walk `lfw*`,
  guest joins) of the release smoke, where the player reports point.

## 3. Live FPS ~64 after a respawn (GitHub #35)

### What is known (livefps080 rounds 1-4, fps01)

- Stuck about 1 respawn in 5 in rounds 1-3 (RTX 3070), then 0 in 7 sessions (r4) and 0 in 36 respawns
  (fps01, 2026-11-09). Per respawn; ends at the next load or (reporter) at a Live FPS target change.
- Stuck stay = **~2x CPU per frame** in DrawRec, GpuCommandProcessor, DrawPrep0 and the Vulkan
  recorders; GPU clock lower as a consequence. Not the clock, the pacer fields, the presenter limit,
  Reflex, priority, affinity, the timer resolution.

### The bbhost lead (ad79a0b flip-pacing keeper)

bbhost's state: at a fixed 60 Hz flip clock the game waits for the flip two back; after a stall it can
come back "off the tick", its wait never blocks, and frames follow their own timing until the next
stall. That needs a flip that completes on a vblank tick. In our live mode `limit_wanted()` gives the
presenter its 240 ceiling, so **flips are immediate** (driver.cpp `immediate_flips`), the game's pacer
holds the target itself (pacer +0x18 written every frame by LiveClockTick), and the presenter's own
wait is BB_FRAMES_AHEAD (GPU of the frame one back). There is no tick for the game to be off of. Also
a phase state changes *when* work runs, not *how much*: it cannot double the CPU of the draw-recording
threads per frame. **The keeper does not port and does not explain #35.**

### What survives a respawn and doubles the draw work

The numbers say the GPU-emulation threads do twice the work per frame, so the game submits more (or
heavier) GPU work per step for the whole stay. Ranked:

1. **More GPU work per game step**: two GPU frames (or two scene passes) per step, or ~2x draws. A
   game-side render state left on by the death / respawn sequence (a full-scene capture for the death
   fade or the respawn transition, a cache update that should be amortised) and cleared by the next
   load. The reporter's "changing the target restores it" happens in the System menu, which opens the
   game's own menu and pauses parts of the frame: the menu may be what resets it, not the target.
2. **Costlier draws**: the same draws with more uploads per draw (buffer-cache or texture re-uploads of
   a range left in a "CPU-modified every frame" state after the load's remaps).
3. A pacing state (bbhost-style): ruled out above.

### Diagnostic (this branch)

`BB_LIVE_FPS_LOG=1` now also prints, once a second, **per game step**: draws, dispatches, submissions,
flips, GPU frames, buffer-upload KB and image-upload KB (counters that are always kept; no
BB_FRAME_STATS, which round 4 used and may have changed the timing). One stuck stay with it separates
hypothesis 1 (draws or GPU frames per step x2) from 2 (uploads per step up, draws flat). Next step for
whoever gets a stuck stay: `BB_LIVE_FPS_TEST_TOGGLE=1` (already in) shows whether a target change
alone (no menu) ends it.
