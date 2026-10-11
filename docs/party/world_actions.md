# Guest actions in the host's world: levers, doors, NPC talk, pickups (D3, D4, D8, G2, H5)

Bloodborne 1.09, our port; addresses are **our offsets** (raw ELF VA of `eboot.elf`). Code:
`gpu/shim/party/party_worldact.{h,cpp}` (world actions, ObjAct kick log, live test fixture),
`party_progress` (the guest's local changes, instanced items), `party_guestsvc` (NPC talk gate),
`party_phantom` (encounter Insight), `party_story` (time of day catch-up). Tests:
`tests/test_party_worldact.cpp` (`party-worldact-test`), `tests/party_fuzz.cpp` (`lamp` target:
`worldact::RequestFromJson`), `tests/test_party_phantom.cpp`.

User decisions (ERSC_PARITY.md "Design decisions", binding): world item pickups are **instanced**
per player; NPC quest choices are **shared** (the first choice anyone makes applies to the host's
world and every player's save); a guest behind the host **catches up fully** (bosses, Blood Moon,
lamps, shortcuts in its own save).

## 1. The model: a guest's game writes the host's world copy

A summoned guest (session role 6) runs its own EMEVD events, talk scripts and ObjActs on its copy
of the host's world: the host-world flag overlay (load mode 1, `event_flags.md` 3), which the
host-world mirror keeps equal to the host's world (`event_flags.md` 12). Whatever the guest does
there - pull a lever, open a door, kick down a ladder, ride an elevator, talk to an NPC - is a
change of that overlay only. The mirror applies, live, only the bits **the host** changed since its
last apply, so the guest's own changes stay in its overlay; `GuestWorldMirror::LocalChanges`
lists them: overlay bits (mirror mask) that differ from what the host's image last put there,
skipping groups with a host change not yet applied.

Before this work only lamp lighting went back to the host (EVENT `lamp_lit`). Now every such
change of an allowed category does.

## 2. EVENT `world_action` (guest -> host)

Guest (`worldact::GuestTick`, main thread, role 6, load mode 1, world up, not loading):

* a **baseline** 3 s after the world became steady (each load, each map change, each overlay
  rebuild): the local differences the guest's own game made while loading the map are held back.
  Client branches of events end early (`1003[6]` "end if client"), and an ended event writes its
  completion flag (0x12ED100) - those must never reach the host. Live: 93-102 such differences
  per load (m21_00, m24_01).
* every 0.25 s: new local differences. Allowed ones (`WorldActionAllowed`, below) go to the host
  as `{"v":1,"map":<guest map>,"set":[ids],"clr":[ids]}` (at most 32 / 16 ids); the set ones are
  also written into the guest's **own pool** (its own save keeps the progress; a save request
  follows). The rest is logged as "kept local".

Host (`worldact::OnHostEvent` on the link thread -> queue -> `HostTick` on the main thread in its
own world, steady): `RequestFromJson` validates shape and ids, then each id again against the
host's **current map** (`WorldActionAllowed(id, clear, host_map)`), a per-slot rate limit (96
flags per 10 s) and applies with SetEventFlag. The world mirror brings the result back to every
guest; C2 puts it into every guest's save.

`WorldActionAllowed(id, clear, host_map)`:

| Category | Set | Clear | Notes |
|---|---|---|---|
| shortcut_door | yes | yes | levers, gates, one-way doors, ladders, elevators (they toggle) |
| world_state | yes | yes | world objects' states |
| npc_quest | yes | yes | shared NPC quests; a quest step clears the old state flag and sets the new one (Iosefka live: 1141 off, 1143 on) |
| key_event | yes | no | world progression |
| anything else | no | no | boss_defeated, boss_area, lamp_unlocked (EVENT lamp_lit), cutscene_seen, item_lot_picked (instanced, 4), session_runtime, time_of_day, never_sync |

Also required: a mirror group and mirror-mask bit (`WorldMirrorKey`, `WorldMirrorMask`: never
2000-2099, 9020-9026, 6000-8999, chalice areas, area 99), not `NeverWritable`; a map flag
(`TAABZnnn`, AA > 0) must name the **host's area**; area-0 ids only for npc_quest / key_event.
`party_fuzz` (target `lamp`) checks every accepted request against these rules and the round trip.

Switch: `BB_PARTY_WORLD_ACTIONS=0` turns it off (both sides). Diagnostics:
`BB_PARTY_WORLD_WATCH=id,...` logs visible / own-save / host-image values of those flags on each
change (both roles).

## 3. ObjActs (levers, doors) on a guest

No phantom gate exists in the ObjAct code (`phantom_limits.md` 2.4) and the live run confirms it:
the guest's lever prompt (text 10010501) shows and the lever works on the guest.

The ObjAct manager (`0x13B2790`, update; per-block managers 0xD8 bytes at `+0x28`, count `+0x20`;
entries 0x140 bytes at `+0x90`, count `+0x98`: `+0x8` list index, `+0xC` kick event id, `+0x58`
object handle, `+0x5C` trigger state):

* `0x13AE370(blockman, idx, chr)` - the **local** player's kick (from the action-button update
  `0x13ADCC0`; entry `+0x78..0x7A` = kick, change, send-net);
* `0x13AF830(blockman, idx, chr, state)` - a kick **another player** made, from the manager's
  network queue (`0x13B2F90`); `0x13AF540` re-applies a state;
* packets: 0x2E (state, all peers), 0x2F (host -> client state); `0x13AE600` runs the kick.

`party_worldact` logs both entries ("ObjAct kick (local player|another player)").

## 4. Items: instanced world pickups (D4)

Vanilla: MSB treasure is never created on a client (`0x17DC300`: role 6 / session slot gates) and a
client's pickup skips the pickup flag and the Lua event (`0x17D6780`). With
`BB_PARTY_GUEST_PICKUP` (default on, party mode) three byte patches (6-byte NOPs, verified):

| Site | Original | What |
|---|---|---|
| 0x17DC3D6 | `0f 84 a5 0c 00 00` (je, role 6) | treasure spawned on a client |
| 0x17DC3E3 | `0f 85 98 0c 00 00` (jne, session slot) | treasure spawned on a client |
| 0x17D6838 | `0f 84 2b 02 00 00` (je, not own world) | the pickup sets its flag and runs its Lua event on a client |

Instancing (`progress::SetInstancedItems`): the world mirror leaves the item_lot_picked bits alone
(`WorldItemMask`); after every mirror apply the overlay's item bits become the guest's **own pool**
bits (full copy after a rebuild, set-only live so a fresh pickup is not undone) - the guest sees the
treasure it has not taken itself, whatever the host took. A guest's pickup sets the bit in its
overlay; `GuestLocalWorldChanges` reports overlay item bits the pool lacks and the guest writes
them into its own pool (never to the host). The host's world keeps its item. C3 still hands the
host's pickups to the guests (their pool flag then hides the treasure for them too); boss rewards
and key items stay C3's (everyone present).

## 5. NPC talk with a guest present (D8)

Every world NPC talk script asks `IsClientPlayer` (talk test 19; scan of the talkesdbnd files:
all of t22xxxx-t36xxxx with NPCs do, 2-7 times each) and parks a client in its "client wait"
state. The guest-services filter (`party_guestsvc` `IsClientFilter`, `0x203BA78`) now answers 0
for a client when `WorldTalkAllowed(talk)`: the world maps' scripts (map 220-369), not the Dream's
(guest services keep their own list; Gehrman's ending is never a guest's), not the world lamps.
`BB_PARTY_GUEST_TALK=0` turns it off. What the talk sets (quest flags) is then a world action
(npc_quest), so the host's world and the guest's own save both advance; the host's npc_quest
flags reach every guest's save through C2 (`BB_PARTY_PROGRESS_NPC`, now **on** by default: the
user's decision). Gifts given in dialogue go to the player who talked (its own inventory).

## 6. Insight and the bath (G2)

* Boss Insight: native +1 for a white phantom plus the director's N-1 (`phantom_limits.md` 5).
* **New**: Insight the host gets from a 9350 start that is not a boss kill (a boss's first
  encounter, story moments such as entering a clinic) goes to every guest present too (EVENT
  `phantom {insight, n, boss:false}`; the guest applies all N, no native +1). Older hosts omit
  `boss` (= a boss kill).
* Insight items: white-enabled (`phantom_limits.md` 1.1); they act on the guest's own record.
* Bath Messengers (t210691/694/695/699 shops, t210698 Insight shop, t210697 dress-up): open to
  guests (guest_services.md 2.2), purchases on the guest's own echoes / Insight and own stock flags
  (6000-8999 stay in the own pool).

## 7. Catch-up of a guest behind the host

| Progress | Into the guest's own save | How |
|---|---|---|
| Bosses (boss_defeated) | yes | C2 set-only (+ the boss-in-this-map hold, event_flags.md 11) |
| Lamps (lamp_unlocked) | yes | C2 |
| Shortcuts / doors (shortcut_door, world_state, key_event) | yes | C2 |
| NPC quests | yes (now default) | C2 with BB_PARTY_PROGRESS_NPC=1 (default) |
| Blood Moon / time of day (9800-9802) | yes (**new**) | C4: the host now re-sends its time of day to each member that joins (`StoryMemberJoined`); the guest's planner never turns its clock back |
| Items the host picked up | yes | C3 full list on join |
| Area gate flags 2AAx (boss cleared per area) | no | never_sync on purpose (2410-style flags block summons, event_flags.md 7) - open item |

## 8. Character freedom audit (H5)

Talk scripts of the Dream (`IsClientPlayer` counts from the talkesdbnd scan): the Doll, storage,
workbench, mirror, every Bath Messenger (shops, Insight shop, dress-up) are on the guest list;
t210690 (no client test) needs none. Chalice altars (t210050/100/110) stay host-only here (chalice
rows, G1). Equipment, outfits (equip menu), gestures (menu), runes already held, items in the
storage box: local record, no gate. Blood Vials / Quicksilver Bullets: bought in the Messengers'
shops (open), refilled by the phantom refill.

## 9. Live evidence

Two loopback instances, save `savedata_20261010` in each (level 37), host lamp-warps to Central
Yharnam (2412951), guest follows (B2). Host test save: `BB_PARTY_PROGRESS_TEST_CLEAR` closes the
m24_01 gates 12410200/202/203/205 in the host's world. Guest fixture
`BB_PARTY_WORLDACT_TEST=obj=...;chr=...` stands at each object / NPC, waits for the action
prompt, presses Cross (in-process pad).

Run 2 (2026-10-11 11:4x):

| Step | Guest | Host |
|---|---|---|
| lever 2411310 (gate 2411300) | prompt 10010501 at the first spot, Cross; +5.9 s `world action sent ... set 12410200(shortcut_door); 1 written to the own save` | +0.06 s `slot 1 changed the host's world ... 12410200=1[shortcut_door]`; mirror back: guest `host image 1` |
| lever 2411312 (gate 2411302) | same, `set 12410202` | `12410202=1` |
| Gilbert (chr 2410782, talk at his window) | prompt 10010200 (Talk), dialogue paged with Cross | - |
| Iosefka (chr 2410702, clinic door) | prompt 10010400, talk; `set 62411300(world_state)`, `set 12410739(npc_quest)`, then `set 1143 72410337 (npc_quest)` | all applied |

(Run 3: the host standing at the gate, ObjAct kick log - section 3.)

## 10. Open items

* The host-side **animation** of a guest's lever / door: see section 3 / run 3.
* Elevators: toggling state flags are shortcut_door; a guest and the host using one elevator at
  the same time can fight over the state (not tested).
* Area gate flags 2AAx are not caught up (section 7).
* Live pickup of a treasure by a guest (the patches are in; not exercised live yet).
* Memory Altar / covenants / chalice ritual by a guest not run live.
