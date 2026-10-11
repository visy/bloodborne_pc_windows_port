# Split travel: each player travels alone, the session stays (ERSC B3 / B5)

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA; bbhost = ours + 0x400000).
Builds on b2_seamless.md (B2: the K hooks, CSMultiPlayerInsTask, the flow states), travel.md (B1,
the funnel, edgetravel's stale map-hit guard) and guest_services.md (lamps / headstones).

Marks: **[C]** confirmed in disassembly / decompile, **[R]** confirmed at runtime here, **[I]**
inferred.

User decisions this follows (ERSC_PARITY.md "Design decisions", binding):

* A lamp, headstone or Hunter's Mark **asks each time**: travel alone or travel with the party.
* The Hunter's Dream is **always shared**: everyone in the Dream is in the host's Dream together.
* Death outside a boss fight: every player, host included, respawns at their **own** last lamp,
  even in another map; the others play on (the hostdeath agent uses the mechanism below).
* The host is needed: the shared world exists only while the host plays.

## 1. Feasibility (answers)

**Verdict: feasible with the engine's own pieces; implemented for lamps and headstones.** A
member can stand in a different map from the host in host-world mode for as long as it likes, and
the session, the room and the party link stay up. The pieces:

1. **A guest-only host-world load needs no host snapshot.** The B2 guest load goes flow +0x84
   1 (await snapshot) -> 2 (applied) -> 3 (host-world load) -> 4. Only state 3 matters for the load
   itself: the MoveMapStep ctor `0x1937570` picks host-world mode (GSM +0x1592 = 0, the flag overlay)
   from +0x84 == 3, and the old stage dtor keeps the room and the tasks in state 3 [C]. The flags
   come from the host-world overlay, which the host-world mirror (event_flags.md 12) keeps filled
   with **every** group of the host's world, not only the snapshot's 12 [R]. So a guest can go 4 ->
   3 directly and warp (K7 keeps the room in the funnel): an *alone load*.
2. **The host's `CHR ` world blob does no harm in another map.** WorldChrMan `0x191BDD0` applies
   the packet-0x11 blob record by record, matching each entity id against the loaded map's chr
   groups (`(group key ^ id) < 0x1000000`, then the entity inside the group); records of another
   map match nothing and are skipped; the blob is freed after the first apply [C]. Enemies of a
   map the host is not in are therefore fresh (as after a rest), simulated by that player's game.
3. **Remote chrs in another map.** Every PlayerIns dies with its own world; remote chrs exist only
   through their member task's STEP_Create, which B2 drives (rewind). A remote chr whose owner
   stands in another map just follows the owner's coordinates (the edgetravel stale map-hit guard
   keeps it from crashing on streamed-out collision) [R, travel.md 7]. Split travel removes such a
   chr (`0x15BC080` slot release + `0x19186E0` WorldChrMan remove, as STEP_DeleteWait does, already
   live in the lone-death respawn r17) and keeps its task; the chr is built again (rewind) when the
   two meet.
4. **Packets of a snapshot sent to a member in state 4 wait.** The receive `0x1783160` pops 0x19 /
   0x11 / 0x10 only in flow state 1 (`0x17893E0(flow, type, buf, size)`, role 3/6, from the session
   owner) [C]; anything the host sends to a member that does not follow stays queued and would be
   applied at that member's next state 1 - stale (another map, older flags). Split travel drains
   those three types right before any state-1 entry.
5. **Nothing else ends the session while apart.** OnMatchingCheck (`0x13808B0`, K5) is the one
   game path that leaves when a member's chr is missing; it is suppressed while members are apart.
   The host watchdog (party_watchdog) counts members that are apart as present. The network
   flow's 60 s timer runs only in states 1..3.

What is **not** possible cheaply: an in-world warp to a lamp of the same map (no load). Bloodborne
lamps only go to the Dream and headstones to a lamp, so "same map" only happens for a Hunter's
Mark; the lamp's spot is a ReturnPointParam row resolved by the load (MSB region), and the chr
warp `+0x5F8` needs the position. A guest's warp into the map the host is in is a load for that
guest only (the B2 re-join, with the host's fresh snapshot).

## 2. Authority while apart

| What | Rule |
|---|---|
| World flags (doors, lamps, bosses, NPC state) | The host's world: the mirror keeps every player's overlay current. A flag a guest sets in a map the host is not in stays in that guest's overlay only (D3: guest flag routing, as `lamp_lit` does for lamps, is the next step) |
| Ordinary enemies | Simulated by each player's own game in the map it loaded; they respawn on every load as in single-player |
| Bosses / unique enemies | Their defeat flags are the host's (K10 / D1); a guest's lone kill in a map the host is not in is not sent to the host yet (open, D3) |
| Item pickups | Vanilla client rules in host-world mode; the shared-world items are the host's (C3 grants) |
| Meeting again | The member whose world is older loads once (the B2 re-join, the host's snapshot) unless both are in the Hunter's Dream (no enemies: the chrs are just built again) |

## 3. Interface (party_b2.h) - for reuse (hostdeath)

```c++
namespace coop::b2 {
// Why a member travels alone (logs, the dead-respawn records).
enum class AloneWhy { Lamp, Headstone, HuntersMark, Death };
// Guest, main thread, world up, B2 on, a client in the host's world. Loads t's destination
// ALONE and stays in the session: the other members do not load.
//  - destination in the host's current area: a B2 re-join (the host re-sends its snapshot,
//    one load for this guest only), placed at the destination (not beside the host);
//  - otherwise: an alone load in host-world mode (no snapshot; flags from the mirror).
// `why == Death`: the player is dead; its own respawn records are kept (the death load goes to
// the guest's OWN last lamp). False: not possible now (the caller keeps the old behaviour).
bool TravelAlone(const TravelIntent& t, AloneWhy why);
// Host, main thread: the next own travel the funnel captures is alone (TravelIntent::alone):
// guests do not follow, the host does not re-send its snapshot after the load.
void HostTravelAloneNext();
// Members apart now (host: members in another area; guest: 1 when the host is elsewhere).
int MembersApart();
}
```

Wire (PartyLink EVENT "b2"): `{"op":"away","pos":[x,y,z]}` (guest -> host, it leaves the shared
area: the host drops its chr), `{"op":"rejoin","pos":[...]}` (guest -> host: re-send the snapshot
and the destination, as `respawn` without the death), `{"op":"meet"}` (either way: chrs built
again without a load, Dream). `TravelIntent::alone` ("alone": 1 in the travel JSON) marks a
host travel the guests do not follow.

For hostdeath (party_travel.h / party_b2.h):

* **Host death, the guests play on**: `coop::SetHostDeathAlone(true)` - every captured
  `TravelKind::HostDeath` is then sent with `alone` set; the guests stay where they are (no load,
  the host's chr is removed on their side), the host loads its own last lamp, re-sends nothing
  (`LastAloneCaptureAgo() < 10 s` at its load) and meets the guests again without a load when it
  is back in their area (SplitTick: chrs re-created, net sync re-registered).
* **Guest death, own last lamp**: `coop::b2::RespawnAloneAtOwnLamp()` (main thread, the held
  death's place: `HeldDeathTick` / `SoloRespawn`) = `TravelAlone(LampIntent(GSM +0x1528),
  AloneWhy::Death)`: a re-join when that lamp is in the host's area, else an alone load. The death
  load uses the guest's own respawn records, which already name that lamp.
* `MembersApart()` > 0 keeps the host watchdog quiet and K5 suppressed.

## 4. Implementation (gpu/shim/party/party_split.cpp, party_b2.cpp, party_travel.cpp)

**The question.** A lamp or headstone warp `0x13CDF30(id)` the player starts, or the Hunter's Mark
item reaching the funnel (site `0x18FFA1D`), is consumed in a party (host with a member task, or a
guest that is a client) and held. The main thread then opens the game's own generic dialog, the
way the Lua `OnYesNoDialog 0x132D2C0` does: the struct at `*(0x5562878 +0xD08)` gets `[0]` the
message id, `[1]`/`[2]` = 1 / 2 (YES / NO buttons), `[4]` byte 1, `[6]` = -1 (the result),
`0x5562878 +0x254` = 0, then `0x16BEBF0(dialog, 7)` [C]. The message is the port's text id
(`BbGameMenu::Pages::SplitTravelText`, answered by the System menu's text lookup hook):
"Travel with the party? YES: everyone travels together. NO: you travel alone." The answer is
`[6]`: 1 = YES, 0 / 2 = NO / closed (the event `0x531CFD0`'s check reads it the same way) [C].

* YES: the old party move. Host: `RunLampWarpDecided(id, false)` (the interceptor stands aside,
  the funnel capture broadcasts TRAVEL, every guest follows with B2). Guest: `lamp_request` to the
  host (guest_services.md 3). A guest's Hunter's Mark "with the party" goes to the host's last lamp.
* NO: host: `HostTravelAloneNext()` + `RunLampWarpDecided(id, true)`: the captured TravelIntent
  carries `alone`; guests receiving it do not follow (`b2::OnHostTravel`: no load, the host's chr
  is removed on their side); the host's B2 driver does not re-send its snapshot after that load.
  Guest: `TravelAlone(LampIntent(id))`.
* `BB_PARTY_TRAVEL_CHOICE=party|alone` skips the question (always that answer); `ask` (default).

**Guest alone, into the host's area** (the Dream when the host is there, or the host's map): a
B2 re-join that the guest starts (`BeginRejoin(solo)`): stale snapshot packets are drained, flow
state 1, EVENT b2 `rejoin` -> the host re-sends its snapshot (the lone-respawn path, without the
dead chr), the guest loads ITS destination (the lamp warp itself, not the transform beside the
host; GSM +0x1521 cleared for the load so the summoned placement does not apply), arrives, both
sides rewind the member tasks. One load, for the traveller only.

**Guest alone, elsewhere**: `TravelAlone` writes flow +0x84 = 3 and runs the lamp warp with K7
armed (the funnel keeps the room), EVENT b2 `away` (the host removes the guest's chr). The load is
a host-world load (flags from the overlay); the game sets flow 4 at its end. Phase `AwayLoading`.

**Members apart** (`SplitTick`, both roles, every frame while no re-join runs): members are
placed by the party link's vitals (map + position, ~4 Hz). A remote chr whose owner (the member
reporting a position within 5 m of it) is in another area is removed (task kept). A member in our
area for 2 s without a chr here is built again (rewind; the host also re-registers its net sync,
`0x18F0DB0` / `0x18F0E80`, without snapshot packets) - a meet without a load. `MembersApart()`
feeds the host watchdog (present, never "stuck") and K5 (OnMatchingCheck stays quiet while
members are apart and 15 s after any split chr change).

**Tests.** Director steps `host_lamp=ID`, `guest_lamp=ID` (the player's warp, with the question),
`host_alone=ID` (decided alone); `BB_PARTY_SPLIT_TEST=no,yes,...` answers the questions in order
with the in-process pad (Left = YES / Right = NO, then Cross).

## 5. Live results

(see below)
