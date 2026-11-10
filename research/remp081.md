# remp081: the original game's multiplayer join / load / leave path, and what it leaves on the table

Branch `agent/remp081`. Research only: no shipped behaviour changed, nothing built, no game run.
Bloodborne 1.09, **our offsets** (raw ELF VA; bbhost = ours + 0x400000). Static RE through
`tools/agent/ghidra_decomp.py` plus disassembly / relocation reads of the eboot; logs read are the
existing agent run logs (no player logs quoted). No decompiled bodies, tables or game text here: only
offsets, our names, field offsets and our own prose.

Marks: **[S]** static only, **[L]** static and confirmed by an existing log or an earlier doc's live
run, **[I]** inferred, **[R?]** needs one run to settle.

What this adds to the earlier docs (not repeated here: `docs/party/b2_seamless.md` sections 1-2 for the
session / task / packet basics, `docs/party/join_timing.md` for the bell chain and the title join,
`docs/party/travel.md` for the warp funnel, `research/reseam081.md` for the measured timings):

1. The whole load is two nested step machines whose tables are now named step by step (section 3),
   with every gate marked timer vs condition.
2. **A fixed 12.0 s floor on the loading screen of every death respawn** (section 3.3) [L]: 81 of 108
   death loads in the existing agent logs last 12.5-12.7 s, other loads have a 3.6 s median.
3. The loading screen already has a native "wait for the other players" gate with a 15 s cap, and a
   native hold timer that defers InGameStart itself (section 3.3) [S].
4. Where the spawn position really comes from during a load (section 3.4): MoveMapStep's own fields,
   re-applied at three call sites. It explains why writing GSM +0x14D0 did not place a lamp load.
5. The enter / leave presentation machine (CSEnterleaveDirectionStep) step by step, with the timers
   that are still fixed (section 1.2), and the leave step with its 10 s cap (section 4).
6. A vanilla file prefetch step with a hard-coded list (section 6).

## 1. The summon / join state machine

### 1.1 Chain from the bell to the host's world (guest side)

| # | State (our name) | Driver | Waits for | Kind |
|---|---|---|---|---|
| 1 | Bell rung, sign timer | sign sender update 0x18A9350 (object vtable 0x532F390) | its own countdown `+0x28` (5.0 s vanilla, 2.0 s with our patch), then SendSign 0x1901320 | fixed timer [L, join_timing] |
| 2 | Sign listed by the host | CSRequestGetSosStep (table 0x1E60580: Init 0x1E5FAF0, Update_forSeamless 0x1E5FBF0, Update_forWide 0x1E603B0, Finish 0x1E603C0) | server poll interval (ss.info); "forSeamless" / "forWide" are the two **search ranges of the sign query** (N near queries, then one wide one, N = ss.info `+0x8B4`, default 5) | interval [S] |
| 3 | Host asks to summon | CSRequestSummonStep (table 0x14BB770: Init 0x14B47A0, Update 0x14B47C0, RequestSummonCheckWait 0x14B4890, CheckComplete 0x14B48C0) | the server's answer byte `+0xF0`; on success queues the summon message (queue entry with tries `0x50001`) | network [S] |
| 4 | Room data at the guest | summon message handler 0x14BA340, pick-up 0x14BB200 from the in-world net task 0x1872360 | nothing: writes GSM +0x16F0 (join kind), +0x16F4 (slot), the return spot +0x14A0.., the summoned placement +0x14C4 / +0x14D0 / +0x14E0, then JoinSession 0x1789010 -> 0x1ED0670 (role 4) | immediate [L] |
| 5 | Joining (role 4 -> 6) | SessionManager update 0x1ED0B20, event 8 / 9 | transport connect + room join | network [L: 0.9 s] |
| 6 | Flow 1: joined, waiting for the snapshot | flow event handler 0x178B5E0 (event 8/9), packet handler 0x1783160 | packets 0x19, 0x11, then 0x10 (in that order of use; 0x10 is read only once the 0x11 blob is in) | network [L: 0.03 s] |
| 7 | Own presentation ("self prologue") | own CSMultiPlayerInsTask mode 0: step 1 waits packet 0x42 (bit 2), step 2 (0x1E4FD50) asks the EnterleaveDirector (0x19700A0) for a direction step and waits for its byte `+0x134` | **the prologue timer only** (section 1.2) | fixed timer [S] |
| 8 | Flow 2: snapshot applied | 0x1783160 -> flag apply 0x13BECA0 -> Lua `OnBeJoinStart_*` (0x1317780 picks the name from GSM +0x16F0) | - | immediate |
| 9 | SummonReloadStart | Lua -> 0x1389440 | presentation count >= 2 and own chr alive, else the join is dropped / left | condition [L, join_timing A4] |
| 10 | Flow 3: own world saved, load asked | serializer 0x131E5C0 (blobs GSM +0x1690 / +0x16B0 / +0x16D0, flow = 3) | - | immediate |
| 11 | Old world out, loading screen, host world in | InGameStep + MoveMapStep (section 3) | section 3 | conditions + 1 timer |
| 12 | Flow 4 | MoveMapStep step 7 (0x1939470) -> 0x178D9C0 | - ; sends packet 0xA (1 byte) to every pending member | immediate |
| 13 | Screen drop | MoveMapStep step 12 (0x193A3E0) | world ready, own chr ready, hold timer, **other players' tasks past first sync (15 s cap)** | conditions + cap |
| 14 | Remote chrs appear | the other members' tasks: Create (bit 0, set by the net task 0x1947310 once the member's row is complete), CreateWait | row occupancy 0x1F (packets 8 / 0xC / 0xD), then the "others" presentation | network + effect |

Host side for one joining member: session event 0xE -> task mode 1 + the burst of packets (7, 0x19, 0x10,
0x11, bullets, 0x42) [L, b2_seamless 2]; the task then waits 0x42 (step 1), first sync 0xC (step 3),
bit 0 (step 4 Create), and in CreateWait (0x1E505E0) the "others" direction's `+0x134`.

Fixed timers vs conditions in this chain: the only pure timers are the sign timer (1), the self
prologue (7) and, on the leave side, the return-home wait and fade (1.2). Everything else is a real
condition. The data exchange is not a bottleneck: the title-join run has 0.9 s for join -> client
and 0.03 s for the whole snapshot [L, join_timing].

### 1.2 CSEnterleaveDirectionStep (the white-light presentation), table built at 0x196C760

Object fields: `+0xD8` PlayerId, `+0xF8` chr handle, `+0xFC` session-type slot (index into the table at
0x553D75C, stride 0x80: effect ids `+0x44/+0x48/+0x4C/+0x54/+0x5C`, message kind byte `+0x09`),
`+0xFD` started, `+0xFF` abort, `+0x110` timer, `+0x118 / +0x120` effect objects (state byte `+0xC`,
3 = finished), `+0x128` chr effect, `+0x130` alpha, `+0x134 / +0x135` "go" bytes read by the tasks.
C2H = entering someone's world, H2C = going home. `0x196F360` = next step, `0x196F1C0(step, 0x10)` =
jump to Finish.

| Step | Fn | Does / waits | Kind |
|---|---|---|---|
| Init | 0x196B050 | - | |
| C2H_Init_forSelf | 0x196B0B0 | needs `+0xFD` | |
| C2H_Init_forSelfPrologue | 0x196B0F0 | chr effect, sound, the "being summoned" banner (0x1BFFE80), timer = `[0x5574AD0]` | |
| C2H_Wait_forSelfPrologue | 0x196B500 | **timer only** (5.0 s vanilla; our fast join 0.1 s) | fixed |
| C2H_Init_forSelfLoading | 0x196B580 | `+0x134 = +0x135 = 1` (releases the own task -> the Lua reload can run) | |
| C2H_Wait_forSelfLoading | 0x196B730 | nothing (the map load itself is the wait) | |
| C2H_Init_forSelfEpilogue | 0x196B770 | timer = `[0x5574AD0]` again (same constant, second reader 0x196B795) | |
| C2H_Wait_forSelfEpilogue | 0x196B7C0 | both effects finished (the timer is not an exit) | condition |
| C2H_Init_forOthers | 0x196B830 | needs `+0xFD` | |
| C2H_Init_forOthersPrologue | 0x196B870 | timer = `[0x5574AD8]` (5.0 s; ours 1.0 s), effect 1 | |
| C2H_Wait_forOthersPrologue | 0x196BA20 | the member's chr exists and its vfunc `+0x618` says ready; then effect 2 | condition |
| C2H_Init_forOthersEpilogue | 0x196BE40 | `+0x134 = +0x135 = 1`, timer = `[0x5574AD8]` | |
| C2H_Wait_forOthersEpilogue | 0x196BE90 | effect 2 finished **or** the timer | condition, capped |
| H2C_Init | 0x196BEF0 | clears a chr draw bit | |
| H2C_Init_forEffect | 0x196C030 | leave effect on the chr | |
| H2C_Wait_forEffect | 0x196C2D0 | alpha fade `dt / [0x5574AE8]` (2.0 s; ours 0.5) and timer `[0x5574AE0]` (5.0 s; ours 1.0) | fixed |
| Finish | 0x196C530 | | |

All [S]; the constants and their patches are [L] (seamless_rules.cpp). The director itself
(EnterleaveDirectorImp, table 0x19709F0: Init 0x19706D0, Update 0x19707D0) only owns the list of
running direction steps (`+0xF0`) and drops finished ones.

## 2. Session layer

Roles (SprjSessionManager 0x5540290 `+0x124`): 0 idle, 1 creating, 3 host, 4 joining, 5 join failed,
6 client, 7 leaving. Create 0x1ECFCD0 (room attributes: three "search" ids and eight numbered slots),
join 0x1ED0670, leave 0x1ED07A0 (42 call sites; roles 0 / 2 / 5 refuse). Update 0x1ED0B20 turns transport
events into flow events: 7 created (role 3), 8 / 9 joined (role 6), 0xE member joined, 0xF member left,
0x10 room data. Irregular leave: `+0x2E0` (seconds without the transport) above `+0x2F0` -> Lua
`OnIrregularLeaveSession` + leave (flow update 0x1789B60).

Player number / slot: the host's packet 7 (4 bytes) carries the member's number, written to GameData
`+0x10` and chr `+0x3B8`; the presentation slot comes with the summon message (GSM +0x16F4) and is
echoed in packet 0x42 (1 byte). The GameData remote row is the first free of 4 (`*(GameDataMan+0x18)+1+i`).

Packets before a guest can spawn (flow `0x553D6D0`; recv 0x17893E0(flow, type, buf, max), send to one
peer 0x17894E0, to all 0x1789A40). Sizes from the call sites [S]:

| Order | Type | Size | From -> to | Content | Needed for |
|---|---|---|---|---|---|
| 1 | 7 | 4 | host -> member | own player number | the member's record / chr number |
| 1 | 0x19 | 0x44 | host -> member | GameDataMan +0x68 block (NG state) | GSM +0x1593 / +0x1594 |
| 1 | 0x10 | 0x10 + deflate(groups * 12 + 0xA4) | host -> member | event flag groups (globals + current map) | flow 1 -> 2 |
| 1 | 0x11 | <= 0x6400 (receive buffer) | host -> member | three chunks: decals, map item bits, `CHR ` enemy states | the host-world load |
| 1 | 0x42 | 1 | every member -> new member and back | presentation slot | task step 1 |
| 2 | 0xA | 1 | loaded player -> pending members | "send me your record" | the reply set below |
| 3 | 0xB / 8 / 0x3B / 0x3C / 0xC / 0xD / 0x13 | 4 / 0x168 / 8 / 4 * n / 0x80 / 4 / 8 | answer to 0xA, and broadcast by everyone at every InGameStart (0x1781AC0) | player record, equipment, first sync (0xC sets task bit 3) | row occupancy 0x1F -> Create |
| - | 0x2C | 4 | client -> host | sent at MoveMapStep step 8 (0x1939970) by a role-6 client; the same type carries the shared-item requests | (host handler not traced) |

Serial that need not be: (a) the guest starts nothing until flow 2, although the destination (GSM
+0x14C4 / +0x14D0) is known at step 4 of 1.1, before the room join; (b) the guest asks for the
members' records (0xA) only at step 7 of its load, so a remote chr can never be built before the last
third of the load; (c) the host builds the guest's chr only after the guest's first sync, which the
guest sends only once it is in state 4. All three are ordering choices, not data dependencies: the
snapshot is two small packets.

Session timers still fixed [S]: flow states 1-3 give up after `[0x55640E0]` (60 s, written at
0x1790243); room timeout flow `+0xB8` (55 / 85 s); the leave step's 10 s (section 4); the host's
summon-message retry (patched).

## 3. Map / world load on a join and on area changes

### 3.1 InGameStep (table 0x192A110, steps at 0x5571C78; current step `+0xC`)

| Step | Fn | Does / waits |
|---|---|---|
| 0 | 0x1928B00 | reset |
| 1 | 0x1928B70 | exit-to-title check (`+0x34`) |
| 2 | 0x1928C90 | with a pending request (`+0x30 == 1`): **raises the loading screen** (NowLoading `*0x5562878`: `+0x808` = 5, or 3 when 0x1AE9B70 says so; request byte 0x556286B = 1; `+0x17C0` = 0; `+0x804` = 1) -> step 4. Without one: drops it (0x176D3C0) once NowLoading `+0x17C0` <= 0 |
| 3 | 0x1929110 | leave the game step |
| 4 | 0x1929180 | asks for the map's load list file |
| 5 | 0x1929210 | waits for it |
| 6 | 0x1929220 | builds the world singletons (WorldChrMan 0x61F0 bytes, ...), **constructs MoveMapStep** (0x1937570, child task `+0x38`) from the request at `+0x40` |
| 7 | 0x19297E0 | lives here while MoveMapStep runs (in game). When it ends: next request = **GSM +0x14C4 when flow == 3** (the summoned placement map), else GSM +0xC, queued with 0x1928680 |
| 8 | 0x1929C10 | waits for two helper tasks, destroys the world singletons -> step 0 |

So a load is: MoveMapStep ends (fade) -> step 7 queues -> step 8 tears the old world down -> step 2
raises the screen -> 4-6 -> a new MoveMapStep. The screen goes up only **after** the teardown. [S]

### 3.2 MoveMapStep (ctor 0x1937570, update 0x19386E0, step table 0x53BE170 through relocations)

Request -> object: request `+0x08` map -> `+0x58`; `+0x54` load kind -> `+0xA4`; `+0x58` placement mode
-> `+0xA8`; `+0x60 / +0x70` explicit position / rotation -> `+0xB0 / +0xC0`; `+0x80` warp point id ->
`+0xD0`; `+0x94` "the player died" (from GSM +0x1522 in the request builder 0x1943F10) -> `+0xE4`.
The ctor sets flow `+0xC5` (cleanup deferred) and, with flow == 3, host-world mode (b2_seamless 1.3).

| Step | Fn | Does / waits | Kind |
|---|---|---|---|
| 0 | 0x1938870 | fade to black, flow `+0xFC` = 0, **`+0x278` = 12.0 s if `+0xE4`, else 0**, **`+0x27C` = 15.0 s** (0x1938B7C), asks for a respawn-point file for some kinds | |
| 1 | 0x1938DE0 | that file loaded, the event system ready; builds the stage object (`+0x118`) | IO |
| 2 | 0x19392A0 | stage file request done | IO |
| 3 | 0x1939350 | the map block's load state reaches 7 | IO |
| 4 / 5 | 0x19393E0 / 0x1939400 | bookkeeping | |
| 6 | 0x1939410 | **session role != 7**: a pending leave must have finished (0x193944D) | condition |
| 7 | 0x1939470 | flow 3 -> 4 + packet 0xA (0x178D9C0), builds the local PlayerIns (0x18DB3C0), **placement** (0x19423A0, 0x1939C80), registers it for the presentation, vial refill for death / Dream loads; a host or client left alone in its room leaves it here | |
| 8 | 0x1939970 | applies the `CHR ` blob (0x191BDD0), client sends 0x2C | |
| 9 | 0x1939BB0 | **re-applies the placement** (0x1939C02), camera, waits for the world's "ready" (0x19E8190) | streaming |
| 10 | 0x1939E90 | ground probe under the player, wakes the bodies there | |
| 11 | 0x193A2D0 | world ready again, first chr update | streaming |
| 12 | 0x193A3E0 | **the screen gate** (3.3), then InGameStart | gate |
| 13 | 0x193AC10 | the in-game stage update (the game) | |
| 14 | 0x1941C20 | fade out, waits for helper tasks, ends | |

### 3.3 The screen gate (step 12, 0x193A3E0) [S; the 12 s floor is L]

The screen is dropped (0x176D3C0 at 0x193A755), Lua `InGameStart` is fired, every player's record is
re-broadcast (0x1781AC0) and the step moves to 13 only when **all** of these hold in one frame:

1. the world says ready (0x19E8190, call 0x193A416) and the own chr's byte `+0x4B5` is set (0x193A4D1);
2. the hold timer `+0x278` has run out. Both timers are counted down every frame by the step update
   0x19386E0 (clamped at 0), whatever the step;
3. no human CSMultiPlayerInsTask (CSMultiPlayMan `+0x10..+0x18`) is still at step <= 3 (`cmp eax,3`
   at 0x193A562), i.e. every other player is past FirstSyncCompleteWait, **or** `+0x27C` (15 s from
   step 0) has run out. Once the check passes `+0x27C` is zeroed (0x193A56C).

Until then the step keeps streaming the world (0x1548740) and updating the own chr, but nothing of
step 13 runs and no InGameStart has happened.

- **The 12 s floor.** `+0x278` is 12.0 s (rodata float at 0x49280C8, one reader in the whole image:
  the load at 0x1938B68) for a load whose request has the "player died" byte. Evidence [L]: in the
  existing agent run logs (400 logs, `loading screen up` -> `gone` pairs) 81 of 108 loads that follow a
  death last 12.5-12.7 s, with almost no spread across machines and settings; the other 772 loads have
  a 3.6 s median. The remaining death-context loads (3-6 s) are loads of another player than the one
  who died (a guest following a host's death has no floor of its own) or mis-tagged by the script.
  The floor counts from MoveMapStep step 0, so it is a floor on the screen, not a pause before it.
- **The native "wait for the others" gate** means vanilla already hides the first sync of the other
  players under the screen, but not their chr build: Create needs bit 0 from the net task, then the
  resources. In run 1 of reseam081 the guest's screen was gone at 73.1 s, `arrived` at 74.1 s and
  cooperators 1 at 74.7 s: the host's chr appeared 1.0-1.6 s after the drop [L].
- **`+0x278` is a native hold lever**: any writer that keeps it above zero keeps the loading screen up
  *and* keeps InGameStart, the record broadcast and the whole in-game update from starting. seam081's
  hold (the call site 0x193A755) only delays the NowLoading clear: the game underneath has started.

### 3.4 Where the spawn position is applied [S]

`0x19423A0` (step 7, call sites 0x19396EB / 0x193993F) resolves the placement into MoveMapStep
`+0xF0` (position) / `+0x100` (rotation): mode `+0xA8` == 1 copies the explicit `+0xB0 / +0xC0`; mode 2
looks the warp point `+0xD0` up in the stage (0x154AFC0 / 0x154AA70) and **ignores every position
bank**. `0x1939C80` then writes `+0xF0 / +0x100` into the chr's physics (`+0x1E0`, `+0x1F0`, yaw
`+0x1D0`) at three call sites: 0x19396F3 and 0x1939947 (step 7) and 0x1939C02 (step 9), plus one
conditional site in the in-game update (0x193BA0C). A lamp load is mode 2, which is why the
summoned-placement write (GSM +0x14D0) did not move it (seam C's finding). The request builder
0x1943F10 reads the forced-placement byte GSM +0x1521; that it selects mode 1 is inferred from the
title join, where the summoned placement worked with that byte set [I].

### 3.5 Phantoms and area changes [L, travel.md / phantom_limits.md; listed for completeness]

Vanilla never moves a phantom between maps. A guest's own warp leaves the room inside the funnel
(0x13CDEF6, GSM +0x1592 == 0). A host's warp closes the room from the stage dtor (0x19471B1) and a
host crossing a streamed block boundary closes it in the stage update (0x193C657, when the block UID
differs from chr `+0x3F8`; roles 1 / 3 only). The lamp is refused with guests present (0x12F8323).
Streaming between blocks of one map never shows a loading screen (step 13 + 0x1548740); a loading
screen is always a new MoveMapStep. Phantoms are kept inside the area by the confinement walls.

## 4. Leave / return to the own world

1. Leave 0x1ED07A0: role = 7, transport leave (0xC8ED70), allocates the leave step
   (CSSessionConnectStateStep, `+0x280`; table 0x1ED2E80: Init 0x1ECE570, Update 0x1ECE590 on bit 0,
   LeaveSession 0x1ECE5D0 arms 10.0 s, LeaveSessionWait 0x1ECE5E0 ends on "left" (bit 1, set by the
   reset 0x1ED0910) **or the 10 s**, Finish 0x1ECE630). The reset 0x1ED0910 clears the owner id, the
   member lists and sets role 0. [S]
2. Flow cleanup 0x178A900 (deferred while flow == 3, a cutscene runs or `+0xC5`): flow = 0, GSM +0x16F0
   = 0, every member's task deleted (0x1E54DB0), every GameData remote row freed, own GameData `+0x10`
   = 0, pending lists emptied, SessionManager lists 0x1ED2710. [S]
3. The H2C presentation (1.2), then the home load: an ordinary MoveMapStep whose ctor (flow != 3)
   sets GSM +0x1592 = 1, clears the overlay and FrpgNetMan `+0xA98`; the own-world blobs
   +0x1690 / +0x16B0 / +0x16D0 and the return spot +0x14A0.. put the world back. Step 6 of that load
   waits for role != 7. [S]

Torn down: transport session, room membership, member list, tasks, remote rows, all remote chrs, the
host world. Kept: the own-world blobs, the return spot, the own save state. What a fast rejoin could
keep alive is everything in point 2 (the B2 hooks K1-K5 already do it for travel): a guest that stays
in the room needs only flow 1 -> the host's re-send -> flow 3 -> one load (seam D). The engine's own
re-sync primitive for the records is packet 0xA: any state-4 peer answers it with its full record set.

Our transport answers LeaveRoom 300 ms after the session ends (np_matching2.cpp). Step 6 comes after
the stage files (steps 1-3), so this is probably not on the critical path of the 3.1 s home load
[I, R?: a role 7 -> 0 timestamp against step 6 would settle it].

## 5. Sync start

- Position / animation of a remote player starts with its PlayerIns (task step 4 Create ->
  0x18DB500); nothing is sent to a member before its row is complete. The warm-up is the record
  exchange: 0xA -> 0xB, 8, 0x3B / 0x3C, 0xC, 0xD, 0x13, and the unconditional re-broadcast at every
  InGameStart. [L]
- Enemy sync: the host registers a new member in two per-world lists (0x18F0DB0 -> WorldChrMan net
  object `+0x688`, at the member-joined event; 0x18F0E80 -> `+0x6A8`, when the member's 0xA arrives).
  Both are host-only and per world, so they are re-armed after every host load (b2 D3). Their
  consumers were not traced [S, I].
- The joining guest gets the enemy *state* once, in packet 0x11, applied in step 8 of its load.
- What blocks a join: SummonReloadStart's two conditions (1.1 step 9), the flow 1-3 60 s limit, the
  room timeout, the host leaving at step 7 when alone. Boss-fight / fog refusals sit in the sign filter
  and the Lua, covered by `docs/party/phantom_limits.md` and `npc_fights.md`; not re-done here.

## 6. Things vanilla has that we do not use

| What | Where | Use |
|---|---|---|
| Hold timer of the loading screen | MoveMapStep `+0x278` (3.3) | a hold that keeps the world from starting, instead of hiding a running one |
| "Others ready" gate with a cap | MoveMapStep `+0x27C`, the step compare at 0x193A562 | the place to wait for remote chrs under the screen |
| Death floor constant | rodata 0x49280C8 (12.0), single reader | cut the death respawn screen |
| Placement fields and re-apply sites | MoveMapStep `+0xA8 / +0xB0 / +0xC0 / +0xF0 / +0x100`, call 0x1939C02 | spawn beside the host inside the load |
| Record re-sync request | packet 0xA | rejoin / reconnect without our own resend |
| File prefetch step | CSPrefetchForSlowStorageStep (table 0x1FDFFD0: Init 0x1FDFAA0, Init_Chache 0x1FDFAC0, Wait_Chache 0x1FDFD20, Finish 0x1FDFE60): requests a hard-coded list of the first area's map files (two id tables, 38 + 26 entries) through the file system and releases them when all are read: an OS-cache warm-up for slow storage. Its creator was not traced [S] | precedent for reading the destination map ahead while a guest waits |
| Second loading-screen style | NowLoading `+0x808` = 3 instead of 5 when 0x1AE9B70 (three counters) is true | not understood [S] |
| A "join in place" path | none found: flow 3 always goes through a full MoveMapStep; SprjRemoIns has `STEP_DummyForWarp_*` (a cutscene stand-in for a warp), not a join path | - |

## Proposals (ranked)

All party items are toggles, off by default; the engine item 1 is a speed option with a kill switch
(round rule: stability / perf engine fixes may default on only after numbers). Sizes: S < 1 day,
M 2-4 days, L a week or more.

| # | What | Expected gain (evidence) | Risk | Size | Test in the smoke harness | Extends |
|---|---|---|---|---|---|---|
| 1 | **Cut the death respawn floor**: rodata 0x49280C8 12.0 -> 2.0 (or 0), `BB_FAST_RESPAWN` + launcher row; solo and party | **~8 s per death** for the player who died, and for every guest that follows a host death (12.6 s -> the real load, 3.6-5 s). Proven: 81 / 108 death loads at 12.5-12.7 s vs 3.6 s median; one reader of the constant | Low: a pure wait. Check that "YOU DIED" and the death sound are not cut short (they play before the screen) and that the blood-echo drop / respawn events are unaffected. Party version check: mark cosmetic (local pacing only) | S | one death in `--only` a hostdeath group and one solo death: `loading screen up -> gone` before / after (expect ~4 s), screenshots of the death banner and the respawned player; `--fps 30,60` | new; helps A's excluded host-death case |
| 2 | **Native hold**: seam B / C / A hold by keeping MoveMapStep `+0x278` above zero (write a small value each frame from the step hook 0x1939470's object) instead of intercepting 0x193A755 | No seconds. Removes "the world runs under our screen" (enemy aggro, events, the pad block and invincibility of seam A's risk 1); the settle test then runs before InGameStart. Evidence: step 12 is static-proven to keep streaming and to defer InGameStart | Medium: frames may not "settle" before InGameStart if the long frames are caused by what starts at InGameStart (then keep B's hook as the second stage); a held step 12 delays this player's record broadcast, so the others see it later | S-M | seam081's sd1 / pfsd / lb1 with `BB_PARTY_HOLD_NATIVE=1`: frames over 100 ms in the 2 s after the drop (BB_STALL_PROBE), enemy census equal at the drop, `arrived` time unchanged | seam B (and the holds of A, C) |
| 3 | **Remote chrs under the screen**: hold (item 2) until every human task is at step 6 Update, cap 2 s; for every load kind (title join, direct summon, follow) | Hides the 1.0-1.6 s between screen drop and the other player appearing (run 1: 73.1 -> 74.1 -> 74.7 s) and moves the chr build hitch under the screen | Medium: needs the net task (0x1947310, sets Create's bit 0) to run while step 12 waits [R?]; if it does not, pump the flow update as the title join does | M | join + travel groups: time from `loading screen gone` to the remote chr existing (target 0), no T-pose in the drop screenshot; 2 guests | seam A's "screen drops once the host's chr is here", generalised |
| 4 | **Placement inside the load**: write the beside-host spot to MoveMapStep `+0xF0 / +0x100` at the step 9 call site 0x1939C02 (freshest host position), or request mode 1 with `+0xB0 / +0xC0` | No seconds; removes the arrival warp and the hold that hides it; fixes seam C's "GSM +0x14D0 does not place a lamp load" by construction | Low-medium: a spot over a hole (keep the B2 landing watch); the step-13 site 0x193BA0C may re-apply | S | lb1: `arrival warp: the load put this guest N m` expected under 0.5 m with the warp skipped; screenshots at the drop | seam C |
| 5 | **Start the guest's teardown at the invite** (direct summon / cross-map join): raise the screen and run InGameStep steps 8, 2, 4, 5 while join -> client -> snapshot runs, hold before step 6 until flow 2 -> 3 | up to ~1.5-2 s per bell join [I]: bell -> client 3.1 s and client -> screen 0.8 s are idle for the load today; the teardown + load-list share of the 4.6 s load is not measured | High: a failed join needs a full own-world load back; the Lua SummonReloadStart conditions (own chr alive, presentation count) no longer hold in a torn-down world, so the serializer must run before the teardown | L | join group with a forced join failure (netsim outage) and a normal join: bell -> `loading screen gone`, the fallback lands in the own world | seam A's idea applied to the join |
| 6 | **Read the destination map ahead** while a guest waits (title join waiting for the host, a slow host on a follow, the bell wait): our loader reads the map's files into the OS cache, as the vanilla prefetch step does for the first area | unknown; measure first: the same map loaded cold vs warm in one session. Likely small on NVMe, real on HDD / SATA | Low | measurement run only at first (one session: load A, load B, load A again; compare `up -> gone`) | new |
| 7 | **0xA as the re-sync** in quick rejoin and after a link reconnect | No seconds; one engine-native request replaces hand-built record resends | Low | qr0 / qr1 with a trace of the reply set | seam D |

Order suggested: 1 (large, proven, small), 4 and 2 (small, make C and B simpler), 3, then 6's
measurement before deciding on 5.

## Proven vs not

- Proven statically and by logs: the 12 s death floor (3.3), the timers of 1.2, the packet order and
  sizes, the measured gaps quoted from reseam081 / join_timing.
- Static only: the step tables and gates of section 3, the placement path (3.4), the leave step (4),
  the prefetch step (6). Each has a byte anchor: 0x1938B7C (15.0 write), 0x1938B68 (12.0 read),
  0x193A562 (`cmp eax,3`), 0x193A56C, 0x193A755, 0x1939C02, 0x193944D.
- Not proven: that the net task runs during step 12 (item 3); that frames settle before InGameStart
  (item 2); whether LeaveRoom's 300 ms reaches step 6 (4); the teardown share of a load (item 5); the
  IO share of a load (item 6); the host handler of packet 0x2C; which counters choose the second
  loading-screen style; who creates the prefetch step.
- No run was made for this document (the game lane was busy; none was strictly needed).
