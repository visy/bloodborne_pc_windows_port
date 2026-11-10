# reseam081: guest stalls (CL-7fc9fe) and a smoother join / warp path (P1-10)

Branch `agent/reseam081`. Addresses are our offsets (bbhost - 0x400000), checked against
`deps/ghidra-export/functions.tsv` and the eboot bytes. No player names, IPs or log lines here.

## 0. Summary

- **Guest stalls (CL-7fc9fe)**: in the players' logs the 400-1500 ms stalls follow GPU memory pressure
  per machine and build, not the guest role (same 4 GB machine: 49/h in the reported guest session,
  1.5-3/h as host); none had the GPU thread busy. On loopback under `BB_NET_SIM=dsl` (2 runs, one with a
  1200 MiB texture budget on the guest) **no frame went over 100 ms in steady play on either role**;
  our party tick costs 0.77 ms/frame on the guest (max 36 ms) and had 0 samples in the long frames.
  Not reproduced; ranked hypotheses in 1.3 (1: VRAM pressure / buffer moves in the NW engine).
- **New instrument** `BB_STALL_PROBE` (off): splits every long main-loop frame at the GX frame end
  (0x1d97853 -> 0x2198fd0, which waits on the render thread's "done" event, play object +0x68, set at
  0x21b6a68) into main work vs. render wait, with 1 ms samples of the right thread and the party share.
  `tools/party/stall_report.py` classifies Stall: blocks of any log (player logs too) per role and hour.
- **Join / warp path** (run 1): a party lamp warp keeps the guest under one 14 s screen of which ~7 s
  is idle waiting for the host's load (serial loads); every screen drop is followed by 150-790 ms
  frames on both roles; Leave + Rejoin = two screens (3.1 + 8.8 s); title join = one 4.6 s screen.
- **0.8.2 design** (section 3): A parallel guest load on party warps (~7 s saved), B hold the screen
  until frames settle + a frame-end quiet point for party work, C placement inside the map load,
  D rejoin without the home trip; each a toggle with its test.

## 1. Target 1: what a guest's game thread waits on during 400-1500 ms stalls

### 1.1 What the player logs can and cannot say

All logs with stall data come from Nightwatch Co-op (Lantern engine) builds with `BB_FRAME_STATS`.
Each `Stall:` block (driver.cpp) is one flip interval over 40 ms with the GPU thread's own time,
the image-allocation time, the fault-handler time and the lines around it.
`tools/party/stall_report.py` (new) sorts every block into one class (first match wins):
pipelines, image-alloc, fault-handlers, gpu-thread-busy, map-load, save-near, mem-move-near,
vram-pressure, or game-thread (GPU thread idle, nothing else in the log near it).

The CL's reporter (NW-a0c071c, 8 logs, one 4 GB GTX 1650 machine and one 6 GB GTX 1660 SUPER):

| log (role, NW build) | hours | 400-1500 ms /h | 1500+ /h | 150-400 /h | texture-cache pressure reports /h |
|---|---|---|---|---|---|
| co-op guest, 0.2.5b3 (the reported session) | 0.80 | **48.9** (mem-move 20, game-thread 12, vram-pressure 4) | 3.8 | 1717 | ~710 |
| solo, coop 0.7.6 (asked-for solo log) | 0.10 | 10.4 | 0 | 156 | ~620 |
| guest, 0.2.5b3, later session | 0.24 | 4.2 | 0 | 38 | 175 |
| host, same 4 GB machine, 0.2.5b3 | 1.35 | 3.0 | 2.2 | 24 | 130 |
| host, same 4 GB machine, 0.2.6b6 | 3.37 | 1.5 | 0.3 | 13 | 6 |
| guest, 6 GB machine, 0.2.6b2 | 3.16 | 2.5 | 0.3 | 8 | 0 |

Findings:

- **No role effect in the data.** The same 4 GB machine as host had 1.5-3 long stalls per hour;
  the bad guest session had 49 per hour and *also* the highest VRAM pressure (texture cache at its
  3393 MiB budget, ~700 reports per hour, against 6 per hour in the 0.2.6b6 host session). The
  4-player playtest of 2026-11-01 (graft note) found the same: stalls track the MiB moved between
  in-place memory and VRAM per window, per machine, and "being a guest does not explain it".
  Across all local player logs the 400 ms+ rate is 6.9/h for guests and 5.2/h for hosts, driven by
  a few machines.
- In the bad session none of the 39 stalls of 400-1500 ms had the GPU thread busy (its user + kernel
  time under half the frame in all of them) or a compile: the time went elsewhere. Half of them sit next to
  guest-memory moves (`N MiB of CPU-written blocks moved in place from VRAM` / `loaded blocks moved
  ... to VRAM`); a run of four (543, 943, 1362, 917 ms) sits on a `Texture cache: memory pressure,
  3300 of 3393 MiB` report. A CPU write to a block that lives in VRAM must bring it back in place
  inside the write-fault handler, i.e. on the writing thread: the game thread.
- Saves are not it: save mounts are about one per 5 s window for both roles (the game's own
  saves), and only 2-3 % of the long stalls sit near one. Pipelines: 0-5 %.
- What the logs cannot say: whether the main thread was busy itself or waiting for the game's
  render thread, and whether any of it was our party code. There was no instrument for that; see 1.2.

### 1.2 New instrument: the frame end and the render thread's "done" event

RE (bbhost's quiet point, verified here):

- `0x1d976e0` is the present step on the main thread; at **0x1d97853** it calls the GX device's
  frame end **0x2198fd0** (`e8 78 17 40 00`).
- The render thread body is **0x21b6910** (GXRenderThread). Its play object P = device `+0x348`.
  Loop: wait on P`+0x28` (work, vtable slot `+0x18`, -1 = forever, at 0x21b6a79), play the queued
  list (P`+0x18` -> 0x2199af0, then the P`+0x20` callback), then **set P`+0x68` ("done")** with
  `0x207e780(ev, 1)` at **0x21b6a68** (`e8 13 7d ec ff`).
- In queued mode (device `+0x84` != 0) the frame end waits on "done" at **0x21998db**
  (`ff 50 18`, `mov esi,-1`), stores the next list in P`+0x18` (and `+0x20`), resets "done"
  (`0x207e780(ev, 0)` at 0x2199919), sets the work event P`+0x28`, and swaps the double buffers.
  Event objects: Dantelion2 events (vtable 0x52b97a0 = bbhost 0x56b97a0; `+0x18` wait, `+0x20` poll).

So a main-loop frame = **main work** (previous frame-end exit -> this entry) + **frame end** (almost
all of it the wait for the render thread to finish the previous frame, whose Gnm submits run
through our HLE). `BB_STALL_PROBE=1|<ms>` (bbport_stall_probe.cpp, off by default, diagnostic
hooks only) prints one line per long frame with that split, when the render thread set "done",
and 1 ms samples of the main thread (or the render thread while the main thread waits) as guest
offsets / host module + first guest caller; samples inside our CoopTick are counted as
`party tick`. With BB_PARTY_PERF the party zones say which part of our tick.

**Run 1** (`agents\reseam081\mp1`, 2 instances, loopback, `BB_NET_SIM=dsl` both, 60 FPS,
`BB_STALL_PROBE=100 BB_FRAME_STATS=1 BB_PARTY_PERF=1`; title join, 30 s standing, party lamp warp
to the Tomb of Oedon, 45 s standing, guest Leave + Rejoin, 60 s standing; 180 s):

- **No frame over 100 ms outside loads on either game.** Steady windows: frame time p99 38-50 ms
  (the 60 FPS cap with dsl), max 37-90 ms, for host and guest alike. Every long frame (host 10,
  guest 11) is inside a loading screen or in the first 1.6 s after one dropped.
- Long frames in two shapes, both roles:
  - **main work, the main thread spinning on a GPU label**: samples at 0x11d7080-0x11d709a,
    function **0x11d7030**, the game's flush wait: `while (*(u64*)(*(obj+0xc30)) < target)` and after
    10,000,000 spins the log line `== Stall during rendering at flush N` (the line seen next to
    many player-log stalls). 202-383 ms frames during loads.
  - **frame end, waiting for the render thread**: main samples 100 % in the condition wait
    0x2083e80 (Dantelion2 PthreadCondition, the "done" wait); the render thread 100 % in a kernel
    wait (`ntdll+0x1628a4`) inside our HLE; "done" came exactly at the end (126-552 ms). The
    first frames after a screen drop on the guest: 430 ms, 352 ms, 227 ms (title join), 168 ms
    (warp follow); on the host 572 ms after its first load, 174 ms after the warp.
- **Our party code is not it**: `party tick 0` samples in every long frame but one (2 of 5
  samples in a 105 ms boot frame). BB_PARTY_PERF over the run: CoopTick 0.73 ms/frame on the host,
  0.77 on the guest; p99 2.1 / 4.2 ms; max 25 / 36 ms (one `invade::Tick` 35 ms on the guest,
  `give::Tick` 5-6 ms on both). Guest-only extra: `worldact::Tick` 134 us average (host 29 us),
  max 2.9 ms; the `phantom` zone over 2 ms in 204 of 8752 guest frames (host 1). Worth trimming,
  not a stall.

**Run 2** (`agents\reseam081\mp2`, rerun from scratch 2026-11-10; as run 1 plus `BB_GC_BUDGET_MB=1200`
on the guest only, a small card's texture budget: the guest was over it from its first load on,
1433-1573 of 1200 MiB, with 200-600 images evicted per 5 s window after each load; party warp to
the Tomb of Oedon, then both walk a route; 140 s):

- Again **no frame over 100 ms in steady play** on either game (worst frame of the late 5 s
  windows 35 / 37 ms), even with the guest's collector over its budget all the time.
- The only steady-state long frames (150 ms host, 142 ms guest) are at the start of the test
  aid's route walk: 6-7 of 7-8 main samples `party tick` (the nav route set-up of `host_route` /
  `guest_route`, a test-only path). This is the probe attributing a stall to our code correctly.
- Long frames after each screen drop again on both: guest 789 ms (frame end 756 ms: render wait)
  right after its first load, host 595 ms (frame end 577 ms). Our engine's `Stall:` blocks for those
  frames classify as gpu-thread-busy (stall_report): the render thread waits for our GPU thread's
  first-frame uploads / pipelines.
- Limits: this PC's GPU has real VRAM to spare, so `BB_GC_BUDGET_MB` only moves the collector's
  marks; it cannot make Windows page video memory as a 4 GB card does, and the players' engine
  (Nightwatch / Lantern) has its own buffer promotion code (BB_DYNAMIC_RETRY, nightwatch-src) that
  bbport does not. **The 400-1500 ms steady stalls of the report were not reproduced** on loopback
  under dsl, with or without the budget.

### 1.3 Root cause / ranked hypotheses

Root cause not proven on a player's machine; ranked by the evidence above:

1. **GPU memory pressure on small cards (engine side, every role).** The bad guest session ran its
   texture cache at the budget for the whole hour (~700 pressure reports/h) and half of its
   400-1500 ms stalls sit beside guest-memory moves (CPU-written blocks brought back from VRAM
   inside the write-fault handler, i.e. on the game thread) or pressure reports; the same machine as
   host with little pressure had 1.5-3/h. Matches the 4-player playtest finding (stalls track MiB
   moved per window, per machine). The players' engine is Nightwatch's (buffer_cache.cpp
   ProcessDynamicRetries / QueuePromotions): the fix belongs there (cap promotions and in-place
   returns per frame under pressure, or stop promoting CPU-written blocks), with `BB_DYNAMIC_RETRY=0`
   as the A/B. Why the guest session had more pressure than the host sessions is open: a later
   NW build (0.2.6) had far less pressure on the same card, so the build may matter more than the role.
2. **The game's own GPU waits made long by a slow GPU thread** (main thread spinning in 0x11d7030
   on a flush label, or waiting in the frame end for the render thread, itself waiting on our HLE).
   Seen in every load and after every screen drop here; in steady play it needs the GPU thread to
   stall, which hypothesis 1 supplies. The probe tells 1 and 2 apart on a player's log.
3. **Synchronous log writes into a slow pipe reader** (the 10 s freezes): fixed in 0.7.9 (log drain);
   the reported log predates it.
4. **Our party code**: ruled out for 400 ms+ (party tick 0.77 ms/frame on the guest, max 36 ms;
   `party tick 0` samples in every long frame of run 1). Small guest-only extras to trim:
   `worldact::Tick` 134 us average (host 29 us), the `phantom` zone over 2 ms in 2 % of guest frames,
   one `invade::Tick` of 35 ms.
5. **Save IO**: 2-3 % of the long stalls sit near a save mount, saves are as frequent on hosts.

Next step that would settle it: a player with a 4 GB card plays one guest session with
`BB_STALL_PROBE=1` (bbport build, or ported into Nightwatch): every stall line then names main
work vs. render wait, the guest offset or host function, and the party share.

## 2. Target 2: the guest's join and load path

Measured in run 1 (one harness clock, `[+s]`; dsl both ways; the host booted later than the guest):

| path | step | time |
|---|---|---|
| title join | guest asks for the room -> the host is in its world (host boot) | 14.7 -> 24.9 (host-side wait) |
| | host room sent -> guest JoinSession -> client (role 6) -> snapshot applied (flow 2) | 25.7 -> 26.6 (0.9 s) |
| | flow 3 -> loading -> **guest screen gone** | 26.8 -> 31.4 (**4.6 s**, one screen) |
| | host `cooperators 0 -> 1` | 30.1 (before the guest's screen dropped) |
| | first in-world frames on the guest | 430 + 352 + 227 ms in the next 1.6 s |
| party lamp warp (Tomb of Oedon) | host warp -> host screen gone | 60.1 -> 66.1 (**6.0 s**) |
| | guest: travel event -> early screen up | 60.2 -> 60.2 |
| | guest waits in flow 1 for the host's snapshot (no load yet) | 60.2 -> ~67.5 (**~7 s idle**) |
| | guest flow 3 (its own load) -> guest screen gone | ~67.5 -> 73.1 (~5.5 s) |
| | `arrived` (host re-creates the guest's chr), cooperators 1 | 74.1 / 74.7 (**14.1 s after the warp**) |
| Leave + Rejoin | Leave pressed -> home lamp warp -> screen gone (screen 1) | 118.1 -> 121.2 (3.1 s) |
| | Rejoin pressed -> back in the session -> screen gone (screen 2) | 123.1 -> 129.0 -> 131.9 (8.8 s) |

Where the screens / waits are, and what could hide or shorten them:

1. **Party warp: the guest's load is serial after the host's** (`OnHostTravel` -> `BeginRejoin`;
   GuestPhase `AwaitSnapshot` loads only at flow 2 + `have_dest`, party_b2.cpp ~9590). The early
   screen (`RaiseLoadingScreen`, NowLoading object 0x5562878 +0x808 = 5, request byte 0x556286B)
   already makes it one screen, but 14 s long: ~7 s of it the guest waits doing nothing. Step A
   of the design starts the guest's load at once.
2. **The first frames after every screen drop** are long on both roles (render-thread waits and
   GPU-flush spins of 150-570 ms): the world's first draws, uploads and pipelines. The screen
   drops before they are done; keeping it up until two frames under 50 ms in a row (the
   frame-end timestamps give that for free) hides them. Step B.
3. **Title join**: one screen of 4.6 s after the room arrives; the rest is the host booting. The
   guest arrived 0.5 m from the host (the placement worked: no visible pop).
4. **Leave + Rejoin = two screens** (3.1 s + 8.8 s). Step D.
5. Map-load step hook points inside the screen exist and are already used: MoveMapStep's per-frame
   step 0x1939470 (B2 own-world chr type hook) and its refill pair 0x19398D9 (0x14DBB70) /
   0x19398DE (0x14DACE0 = bbhost sub_18dace0 from sub_1d39470; our phantom refill). Step C uses
   them for placement before the chr is built.


## 3. Design for 0.8.2: a more seamless join and warp

Rules kept: correct over seamless (any step that glitches falls back to today's path, which keeps
working), new party behaviour is a toggle (System > Party row + launcher + ini, off by default
until a release smoke proves it), every step has a kill switch, measured on loopback under
`BB_NET_SIM=dsl` with `--timestamps`, `join_timeline.py` and `BB_STALL_PROBE`, then on 2 PCs.

### 3.1 Step A: the guest loads in parallel with the host on a party warp ("parallel follow")

Today (`OnHostTravel` -> `BeginRejoin`, GuestPhase AwaitSnapshot, party_b2.cpp ~9560): the guest
raises its loading screen at the host's TRAVEL event, puts the network flow (0x553D6D0 +0x84) to
1, waits until the host has loaded *and* its join snapshot (0x10 flags, 0x11 world) has been
applied (flow 2, `have_dest`), and only then starts its own load (flow 3, the host-world load to
the host's spot). Wall time for the guest = host load + snapshot + guest load (run 1: 6.0 s host load, ~7 s idle, ~5.5 s own load; `arrived` 14.1 s after the warp).

Proposal: at the TRAVEL event the guest starts its own load of the destination lamp at once, as
split travel's `solo` load already does (GSM +0x14C4 stage, +0x14D0 lamp spot, +0x1521 cleared,
flow 3), behind the early loading screen, which it keeps up after its own load ends. When the
host's `dest` arrives the guest is in the same block: the existing `InWorldFollow` phase drains the
snapshot packets, applies them, warps beside the host and sends `arrived`; only then is the
screen dropped. The player sees one loading screen of max(host, guest) + the apply, not the sum.

- Toggle `party_parallel_follow` / `BB_PARTY_PARALLEL_FOLLOW` (off), only for lamp / Mark /
  Dream travel (not host death, not boss-fight loads, not Chalice layers: those keep today's path).
- Risks: (1) the guest's world runs ~seconds before the host's snapshot: enemies on its own AI
  (party_enemyai lone-guest mode) could move or aggro; keep the guest's chr disabled + invincible
  and the pad blocked under the screen until `arrived`; the census repair (party_enemy_census)
  re-aligns enemy deaths. (2) `InWorldFollow` is opt-in (`BB_PARTY_INWORLD`) since playtest 1
  (triage bugs 6/7: a host *death* in the same map needs the vanilla respawn load to end HostDead
  and reset the world): host deaths stay excluded. (3) The host's 0x11 snapshot applied into a
  world that already streamed its blocks: the same as the in-world follow and the Mark-in-map
  case, which are live. (4) A guest faster than the host waits under the screen; a slower guest
  is what today's path does anyway. (5) Fallback: no host `dest` within 30 s, or the guest's map
  differs from the host's dest map -> today's AwaitSnapshot load (one more screen).
- Test: smoke phases `lg` (Tomb of Oedon travel, a long load) and s1's travel with the toggle on and
  off in one session where possible; measure travel -> `arrived` per guest
  (`join_timeline.py`-style milestones: `guest: loading screen raised at the host's travel`,
  host `loading screen gone`, guest `arrived`); `--fps 60/30` (a slow guest); 2 guests; dsl.
  Screenshots: no frame of the old map after the screen goes up, no T-pose / falling host chr
  at the drop.

### 3.2 Step B: no main-thread work in the frame where the screen drops (quiet point)

The stall probe gives the place: right after the frame end's wait at 0x21998db returns, the
render thread is idle (waiting on P +0x28) and the main thread has not handed the next frame yet;
bbhost uses the same point for its resolution change. Snapshot apply bursts, remote-chr
re-creation (`CSMultiPlayerInsTask` rewind), our join-beside warp and the post-load party writes
(GameData +0x10 / +0xA4 / +0x5D8, title join) run on the main thread today in whatever frame they
land, often the first frames after the screen drops, which is when players notice a hitch.

Proposal: a frame-end hook (the probe's call-site hook at 0x1d97853, promoted from diagnostic to
a dispatcher) with a small queue: party work that touches chr / render-visible state is posted
there and runs after the frame end returns, a bounded budget per frame (e.g. 4 ms, rest next
frame), and the loading screen is dropped one frame after the queue is empty. Toggle
`BB_PARTY_QUIET_POINT` (stability: default on once proven, `=0` kill switch).

B2, the cheaper half (do first): run 1 shows every screen drop followed by 150-570 ms frames on
both roles (render-thread waits, GPU-flush spins: first draws, uploads, pipelines). The same
frame-end timestamps tell when the world has settled: keep the party's own screens (early travel
screen, title join, rejoin) up until two frame ends in a row come under 50 ms after the game's own
InGameStart clear (0x176D3C0), capped at 2 s. The player sees the screen ~1 s longer instead of a
frozen world. Toggle `BB_PARTY_SETTLED_DROP` (off; on after a smoke), test: count frames over
100 ms in the 2 s after each drop with `BB_STALL_PROBE=100`, before 3-4 per drop, target 0.

- Risks: work after the frame end delays the next frame's main work (budgeted); the hook must
  stay byte-checked (0x1d97853 `e8 78 17 40 00`); a game update could reorder; the render thread
  is idle only in queued mode (+0x84): check it once and skip the queue otherwise.
- Test: `BB_STALL_PROBE=50` on both games during travel / join / rejoin smoke phases: count frames
  over 50 ms in the 5 s after each screen drop, before / after; no new visual glitch in the
  screenshots of that window.

### 3.3 Step C: join placement inside the map load instead of a warp after it

Title join and the direct summon put the guest beside the host with a warp after arrival (the
"join-beside" warp, B2 spacing), after the summoned placement already chose a spot. The map-load
step machine has hook points that run inside the loading screen: MoveMapStep's per-frame step
0x1939470 (already hooked: own-world chr type) and its refill pair at 0x19398D9 / 0x19398DE
(0x14DBB70, 0x14DACE0, bbhost's map-load refill hook point; already used for the phantom refill).
Writing the beside-host spot into the summoned placement (GSM +0x14C4 / +0x14D0 / +0x14E0, as split
travel does for a lamp) before MoveMapStep builds the PlayerIns removes the visible post-load pop.
Toggle with the parallel follow.

- Risks: the host moves during the guest's load (spot is stale: keep the post-load correction
  when the host is over 5 m from the written spot); a spot over a hole (keep the B2 landing watch).
- Test: the join smoke (`--only join`) and title join: guest position at `loading screen gone` vs.
  1 s later (no move over 0.5 m), screenshots at the drop.

### 3.4 Step D: Leave + Rejoin without the home trip

A guest's Leave loads its own world (one screen), Rejoin then loads the host's world again (title-
join-like or direct summon + load): two loading screens for "reconnect". When the guest pressed
Leave only to rejoin (or a network drop recovered within the reconnect window), the session's
room and the B2 re-join half are still usable: re-run only the world-entry half (flow 1 -> host
re-sends -> flow 3, the same as a travel follow) without leaving the room. Out of scope for 0.8.2
unless steps A-C land early; listed so the order is clear.

### 3.5 Order and acceptance

1. Promote the probe hook to a dispatcher (B), measure the drop-frame hitch on today's paths.
2. A behind its toggle; smoke `lg` + s1 travel + `--fps 60/30`; numbers vs. today.
3. C with A's toggle; join + title join smoke.
4. Turn A/C on by default only after a full release smoke and one real 2-PC session.

## 4. What was committed

- `03613e9d` Stall probe (`BB_STALL_PROBE`, off by default; diagnostic hooks at 0x1d97853 / 0x21b6a68, left
  out of the party hook-set check by their "probe" names), `tools/party/stall_report.py`, unit tests
  `stall-probe-test` (ninja target) and `tests/test_stall_report.py`, DEBUG_SWITCHES row.
- `4ec5978a` Probe samples in a system DLL name the first return address into our exe (`via exe+0x...`).
- Last commit: this document; stall_report reads `instances.py --timestamps` logs.
- No gameplay change; nothing for CHANGES.md / COOP.md (diagnostic only).
- Not done: screenshots (the visual check run was stopped from outside before its first shot; the
  probe draws nothing). Runs: `agentseseam081\mp1` (run 1), `mp2` (run 2).
