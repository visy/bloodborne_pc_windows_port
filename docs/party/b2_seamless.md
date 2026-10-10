# B2: true seamless travel (session and remote players survive map loads)

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA; bbhost = ours + 0x400000).
Sources: Ghidra decompiles (`tools/re/decomp.py`, a persistent headless request server for this
work), capstone over `eboot.elf` with the SCE relative relocations applied (vtables / step tables),
the runtime probe of the prototype (section 6). Builds on travel.md (B1), event_flags.md,
npc_peer.md, four_players.md.

Marks: **[C]** confirmed in disassembly / decompile, **[R]** confirmed at runtime here,
**[I]** inferred (shape / names), needs a runtime check (two instances).

## TL;DR

* The "remote players can't be re-created after a load" assumption of travel.md §4 is **wrong**.
  Remote `PlayerIns` are created by a per-member task, `CSMultiPlayerInsTask`, whose step machine
  is re-enterable: rewinding it to `STEP_Create` (+0x54 = 4) with its create bit (+0xED |= 1)
  rebuilds the chr from data that already survives a load (the network flow's member list and the
  GameDataMan remote rows). No join, no summon blob, no NpId re-negotiation is needed. [C]
* The join snapshot is **re-sendable to one peer**: `0x1786050(_, peerId)` (packet 0x10 flags),
  `0x1786360(_, peerId)` (packet 0x11: decals + map items + `CHR ` enemy states of the current
  map), `0x17894E0(flow, peerId, type, buf, n)` for 0x19 / 0x42, plus the per-peer net-sync
  registrations `0x18F0DB0` / `0x18F0E80`, bullets `0x1A04190`, NPC summons `0x15BD0E0`. [C]
* The guest's "enter the host world" step is a state of the network flow (`0x553D6D0 +0x84`):
  1 = joined, waiting for the host snapshot; 2 = snapshot applied; 3 = loading into the host
  world; 4 = in world. Putting a connected guest back to 1, letting the host re-send, then 3 + a
  warp **re-runs only the world-entry half of a join** with the room and signaling untouched. [C]
* Host side, the room survives a load with the known retarget plus two more call sites; the
  CSMultiPlayMan reset can simply be skipped for human tasks (they hold peer ids, not chr
  pointers). [C]
* **Verdict: feasible.** Not "research-grade, months": the engine already contains every piece
  (it is how a vanilla guest gets from its own world into the host's world). Effort for a working
  2-player B2: **~2-3 weeks** (prototype here + two-instance bring-up + edge cases), 4 players and
  death/boss/Dream edge cases another 1-2 weeks. Main risks in section 7.

## 1. What a map load destroys and keeps

### 1.1 Session / room / signaling

| Object | On a host load (vanilla) | On a guest host-world load (+0x84 == 3) |
|---|---|---|
| SprjSessionManager room (`0x5540290`, role +0x124, peers +0xF8..+0x100 stride 0x20) | closed: stage payload dtor `0x19470F0` calls BeginLeaveSession `0x1ED07A0` at **0x19471B1** whenever flow +0x84 != 3; also stage-record UID change `0x193C657` (role 1/3) [C] | kept: the dtor skips both the leave and the CSMultiPlayMan reset when +0x84 == 3 (`cmp [flow+0x84],3 ; je 0x194720C` at 0x1947171) [C] |
| Network flow `0x553D6D0` (+0x68 member list {PlayerId @+0x10, GameData slot @+0x30}, +0x48 pending-data list, +0x84 state) | kept (a persistent singleton; only `0x178A900` "session ended" clears it, and that is deferred while +0xC5 / +0x84 == 3) [C] | kept [C] |
| Signaling / P2P | follows the room (Matching2 leave) [C via travel.md]; with the room kept, untouched [I] | untouched [C] |
| Flow state timer | states 1..3 accumulate +0x108; at >= 60.0 s (`0x55640E0`, set by 0x1790243) BeginLeave [C] | same |

### 1.2 Remote PlayerIns, ChrIns, manipulators

* **Every PlayerIns dies with the world.** The world dtor `0x1547B40` calls WorldChrMan finalize
  `0x1914F40`: clears the map chr set (+0x48) and the player set (+0x30, `0x18DA510` destroys each
  entry) and writes WorldChrMan +0x60 = 0. The local player is re-created in MoveMapStep
  `0x1939470` (`if (+0x60 == 0) 0x18DB3C0(...)`). [C] (runtime: section 6)
* Remote PlayerIns are created only by `CSMultiPlayerInsTask::STEP_Create` `0x1E50090` →
  `0x18DB500(WorldChrMan+0x30, chrType, row+0x10, peerId)` (first free entry from index 1), then
  vfunc +0x640, +0x1A0, +0xB0, +0x610(1), the slot-table registration `0x15BC590(...,kind 1)` and
  (host) a playlog record. The NetworkManipulator comes with the PlayerIns factory. [C]
* `0x1E54830` (CSMultiPlayMan reset, also from the dtor at **0x1947207**) destroys every task
  (human vector +0x10/+0x18, NPC vector +0x38/+0x40); the task dtor `0x1E54340` only frees the
  task (no chr access). [C]
* The tasks reference their member by **PlayerId only** (+0xC8, 0x20 bytes, compared with
  `0xCA1C70`); every step looks the chr up again (`WorldChrMan+0x38` entries, chr +0x3D8). So a
  kept task across a load is safe. [C]

### 1.3 The guest's host-world state

* Host world mode = `GSM(0x5556678)+0x1592 == 0`, set by the MoveMapStep ctor `0x1937570` when
  flow +0x84 == 3 (also FrpgNetMan +0xB14 |= 8, local GameData +0x5D8 = session type,
  `SetLoadMode(1)`); +0x1592 is set back to 1 by the stage-end step `0x1942720` and re-zeroed by
  the next ctor if +0x84 is still 3. [C]
* The flags of a host-world load come from the overlay (+0x88 of SprjEventFlagMan), filled only
  by the packet 0x10 apply `0x13BECA0` (12 groups: globals + **the current map**). [C]
* The enemy / object state of a host-world load comes from the host's packet 0x11: the receive
  (state 1 only) stores three blobs at GSM +0x16C0 / +0x16A0 / +0x1684; WorldChrMan `0x191BDD0`
  applies the `CHR ` blob (+0x1684 in host-world mode, the guest's own +0x1690 at home). [C]
* The guest's "go home" data: own-world snapshots +0x1690/+0x16B0/+0x16D0 (written by the
  SummonedMapReload serializer `0x131E5C0`, which also sets +0x84 = 3) and the return spot
  GSM +0x14A0..+0x14C4 → forced placement in `0x1942720`. Both must **not** be rewritten by a B2
  re-entry (or the guest's own world would get the host's state when it goes home). [C]

## 2. The join, as the engine runs it (what B2 re-runs)

`CSMultiPlayerInsTask` (0x100 B, vtable 0x534F650, ctor `0x1E4F9F0(task, peerId, mode)`; step
table built at 0x1E50F00, names from the strings) [C]:

| Step | Fn | Waits for / does |
|---|---|---|
| 0 Init | 0x1E4FCD0 | - |
| 1 StartMultiPlayNotifyWait | 0x1E4FCF0 | flag +0xED bit 2 (packet 0x42 from the member: +0xEC = its presentation slot; also bit 5 from FrpgNetMan +0xA98) |
| 2 StartMultiPlayWait | 0x1E4FD50 | enter presentation (EnterleaveDirector 0x553E8B0); mode 0 copies the return spot +0x1620.. → +0x14A0.. |
| 3 FirstSyncCompleteWait | 0x1E4FF70 | modes 1/2: bit 3 (set when the member's packet 0xC arrives, 0x178351D) |
| 4 **Create** | 0x1E50090 | modes 1/2: **bit 0**; finds the member's GameData slot in flow +0x68, builds the PlayerIns (above) |
| 5 CreateWait | 0x1E505E0 | presentation; clears bit 0 |
| 6 Update | 0x1E50830 | until bit 1 (leave) |
| 7 WaitExitMultiPlay | 0x1E50860 | |
| 8 Delete / 9 DeleteWait | 0x1E50880 / 0x1E50A70 | leave presentation; release slot `0x15BC080`, remove chr `0x19186E0` |
| 10 | 0x1E50D70 | +0x54 = -1 (done) |

Fields: +0x50 current step, +0x54 requested step (the update `0x1E52FE0` copies +0x54 to +0x50
and runs it; steps advance with `0x1E54250` = +0x54 += 1; vfunc +0x28 = set +0x54), +0xC8 PlayerId,
+0xE8 mode (0 local guest, 1 a guest member, 2 the session owner as seen by a guest, 3 local host),
+0xEC presentation slot, +0xED flags (0 create, 1 leave, 2 start notified, 3 first sync, 4 return
via presentation, 5). Mode makers (standalone, uncalled copies of the inline code): 0x1E549C0 (0),
0x1E54B60 (1), 0x1E54A90 (2), 0x1E548F0 (3). [C]

Session events, handler `0x178B5E0(flow, event)` [C]:
* 7 create success (host): flow +0x84 = 4, reset GameData remote rows, local task mode 3.
* 8/9 join success (guest): +0x84 = 1, Lua `OnJoinMutiplay`, local task mode 0.
* 0xE member joined: task mode 1 (or 2 if the member is the room owner `SessionMan+0xD0`), a free
  GameData row (`*(GameDataMan+0x18)+1+i |= 1`), the member into +0x68 and the pending list +0x48;
  **host only**: packet 7 (4 B), 0x19 (0x44 B, GameDataMan +0x68), `0x1786050` flags (0x10),
  `0x1786360` world (0x11), bullets `0x1A04190`; all roles: packet 0x42 (GSM +0x16F4),
  `0x18F0DB0(WorldChrMan+0xA40, id, *(0x553B148)+0x20)`.
* 0xF member left: delete its task (`0x1E54DB0`), free its row.

Per-frame packet handler `0x1783160` (from the flow update `0x1789B60`) [C]:
* 0xA (data request) on a state-4 peer → replies 0xB, 8 (0x168 B player record), 0x3B/0x3C,
  0xC, 0xD, 0x13, and (host) `0x15BD0E0` NPC summons + `0x18F0E80`.
* 8 / 0xC / 0xD fill the member's GameData row; occupancy byte bits 2/4/8 (+0x10 elsewhere);
  0xC also sets the task's bit 3.
* state 1 only: 0x19 → GSM +0x1593/+0x1594; 0x11 → the three blobs; then (role 6, +0x16A0 > 0)
  0x10 → inflate, `0x13BECA0`, **+0x84 = 2**, Lua `OnBeJoinStart_White` (`0x1317780`, call
  **0x178524F**). The Lua then runs `SummonReloadStart` (`0x1389440` → `0x131E5C0`: own-world
  snapshot, +0x84 = 3) and the warp.
* At the end of a host-world MoveMapStep, `0x1939470` → `0x178D9C0`: +0x84 = 4, packet 0xA to
  every pending member. `0x1947310` (net update, +0x84 == 4) pops a pending member whose row
  occupancy is 0x1F and sets its task's bit 0 → STEP_Create. At "InGameStart" (`0x193A3E0`) every
  player re-broadcasts its own record (`0x1781AC0`: 0xB, 8, 0x3B/0x3C, 0xD) - **on every load**.

## 3. Answers

**(1) destroyed / kept** - section 1. Room: closed by two call sites (fixable). Remote chrs: always
destroyed with the world, their tasks destroyed by one call site (fixable), their data (member
list, GameData rows, task flags) kept. Guest host-world state: kept except what is per map (flag
overlay groups of the new map, the `CHR` blob) - both come with the re-sent snapshot.

**(2) re-join without leaving** - yes, and it does not need the join-time insertion code path as
such: keep or recreate the task, rewind to STEP_Create with bit 0. Inputs: PlayerId (task +0xC8 /
flow +0x68 / SessionMan peers), the GameData row (kept), the presentation slot (+0xEC, kept or
packet 0x42). No summon blob, no session type is needed (the row carries chr type +0xA4 and
session type +0x5D8). The guest side re-enters the host world through the flow states 1 → 2 → 3
→ 4 with the room up; only the Lua join start must be kept quiet (it would overwrite the
own-world snapshot).

**(3) the guest's load and world sync** - the guest's load must be a host-world load
(+0x84 == 3 before the warp; the old stage dtor reads it, and the MoveMapStep ctor), the
destination is the host's own (B1's TravelIntent replayed with `0x13CDF30` / `0x132E010`). The
snapshot senders take a target peer and read the *current* world, so they must run after the host
has finished its own load and before the guest's load starts. The receive side reads 0x11 / 0x10
only in state 1 (queued until then). Enemy net sync per peer is re-armed by `0x18F0DB0` /
`0x18F0E80` on the host's new WorldChrMan (+0xA40 is per world [I]).

## 4. Patch / hook list (byte-verified, prototype `gpu/shim/party/party_b2.{h,cpp}`)

| Id | Site | Bytes (1.09) | Change | Why |
|---|---|---|---|---|
| K1 | 0x19471B1 | `e8 ea 95 58 00` call 0x1ED07A0 | call-site hook: no leave while role 3/6 (or the existing XML retarget `e8 ea 8e 58 00` → 0x1ED00A0) | host keeps the room at unload |
| K2 | 0x1947207 | `e8 24 d6 50 00` call 0x1E54830 | call-site hook: with a session, keep human tasks, destroy NPC tasks as the reset does | tasks survive |
| K3 | 0x193C657 | `e8 44 41 59 00` call 0x1ED07A0 | no leave with a session | stage-record change (streamed blocks) |
| K4 | 0x193C689 | `e8 82 26 c8 ff` call 0x15BED10 | skip with a session | keeps the presentation members |
| K5 | 0x13809B7 | `e8 e4 fd b4 00` call 0x1ED07A0 | no leave while a re-join runs | OnMatchingCheck: member chr missing while loading |
| K6 | 0x178524F | `e8 2c 25 b9 ff` call 0x1317780 | skip while a guest re-join runs | no vanilla SummonReloadStart |
| K7 | 0x13CDEAE | `75 6c` | `eb 6c` written / restored around the guest's own warp (main thread) | guest warp without leaving |
| D1 | flow +0x84 | - | guest: 1 on host travel, 3 after the snapshot (the setter 0x178D9A0's write) | host-world load |
| D2 | task +0x54 / +0xED | - | = 4 / \|= 1 for remote tasks without a chr | re-create remote chrs |
| D3 | per member | - | host after its load: 0x19, `0x1786050`, `0x1786360`, `0x1A04190`, `0x18F0DB0`, `0x18F0E80`, `0x15BD0E0`, 0x42 | fresh snapshot / net sync |

Ordering: guest D1(1) on the TRAVEL event (room still up) → host loads (K1, K2, K3/K4) → host D3
→ guest gets 0x11 + 0x10, +0x84 = 2 (K6 quiet) → guest D1(3) + B1 replay under K7 → guest
load (old dtor keeps tasks and room by itself in state 3) → +0x84 = 4 → guest D2 (host chr),
PartyLink "arrived" → host D2 (guest chr). Fallback: no snapshot within 45 s → +0x84 = 4 and the
B1 path.

## 5. Prototype status

`BB_PARTY_B2=1` (implies the party layer; `BB_PARTY_B2_FORCE=1` exercises K2 without a session).
Wired: `party_director` (tick, outgoing "b2" events), `party_runtime` (the guest takes the host's
TRAVEL event when B2 is on; the host receives "arrived"), `party_travel::ReplayTravelNow`.
Single-instance results in section 6; the two-instance path (D1-D3) is **untested**.

## 6. Runtime (single instance)

See section 6 results below (filled from the probe run).

## 7. Risks / open items

* [I] GameData remote rows surviving a host load (MoveMapStep ctor touches only the local row) -
  check +0x10 of each row after the load in the two-instance run.
* [I] EnterleaveDirector entries: STEP_CreateWait waits on a presentation entry; after a load it
  is created again (`0x19700A0` is find-or-create) - the summon effect may replay on re-creation.
* [I] WorldChrMan +0xA40 net-sync object per world: if it survives, `0x18F0DB0` double-registers
  the peer (it is a list push).
* Host death / host Hunter's Mark: the task path is fine, but ChrIns_OnDeath (role 3) requests the
  return of every human task (`0x1E54DB0`) - B2 death needs that branch suppressed (not done).
* Boss clear / BlockClear2 send-home, Dream gate, 4 players: unchanged from B1; with B2 a member
  that does go home must leave cleanly (the K hooks only act with role 3/6).
* Different load speeds: the guest's state 1..3 window is limited to 60 s by the flow timer
  (`0x55640E0`); slow guests need +0x108 reset each tick (cheap).
* Packets 0x06 from a guest still loading reach a host whose task re-created the chr only after
  "arrived" - correct order; the reverse (guest early) is handled by D2 only after state 4.
