# chalice081: Chalice Dungeon co-op, three player reports, one cause (2026-11-10)

Agent chalice081, branch `agent/chalice081` on master cd92169a. Offsets are our raw ELF VAs (bbhost = ours + 0x400000).
Confidence: **C** confirmed (decompiled / log evidence / live), **L** likely, **I** inferred.

Reports: NW-cec05db (0.7.9: a guest "offline" on the title, cannot join a dungeon), NW-be0f493 (0.7.8: two friends cannot
join a dungeon together), NW-cab726f (0.7.7: the guest is kicked home when the host starts a dungeon, then "the load fails").

## 1. Vanilla: how a Chalice Dungeon instance is made, entered and joined (C unless marked)

Most of this was already in docs/party/chalice.md 1-4 (read and re-checked against bbhost's
`engine/cs/dungeon_ritual_helper.hpp`, GPL-3, credited; ported by intent only).

* **A dungeon is a record, not a server instance.** A player has 7 altar banks ("slots"): bank 0 = the transient QUICK bank
  (the Makeshift Altar / Short Ritual Root Chalice, and the bank every network join writes), banks 1..6 the generated altars
  of the Dream's altars (talk t210050 = Short Root, t210060..t210110 = banks 1..6; command 1:72 calls SelectDungeonAltar
  0x13b89b0 which selects the bank and remaps the area-29 flag groups). The one-hot flags 9020..9026 (0x233c..0x2342,
  MSB-first) select the current bank; the record of bank k lives in the event-flag store at `94000000 + k*100000`
  (ChannelId +32/+64, map id +96 (`0x1Dxxxxxx`, all ones = empty), +128, the **nine layout pairs** +160..+735, 16 bytes
  +736, the creator name +992). The map id range is area 29 (`m29_BB_RR_00`, 0x1D......); the dungeon is assembled from
  m29's pieces at load time from those nine pairs. So the "seed" the host must share is the record: the nine pairs (+ the
  chalice's HolygrailExParam row: family, depth, rites, and the 16-byte block), packed by the game as **FormData**
  (0x1a9c490 reader: type 4 = 0x7C bytes local, type 5 = 0x8C bytes server with the creator name; bbhost
  `DungeonMatchResultPrefix`). The ChannelId is the server's key for that FormData; the **glyph** (DiscernmentWord) is a
  server-assigned word for the ChannelId, not a seed: the layout comes only from the FormData.
* **Ritual (root / fixed chalice)**: the altar's SetupHolygrail request (helper +0x260 = 3) runs steps 8.. of
  CSDungeonRitualHelper (step table 0x557e248): 0x1aaeec0 generates the map UID, record, payload and metadata with the
  native RNG (bbhost `DungeonRitualSetup`), writes bank k's record, then uploads (`/channel/upload`: FormData, ShareLevel,
  Status, HolyGrailTypeId, RitualLevel, SubFeatureFlag; reply ChannelId + DiscernmentWord) and stores the id. A **fixed**
  chalice (goods ending in 9) is not uploaded: the game searches `FixedOrGeneralList [{2}]` with the chalice's family /
  level / rites, must find an entry, fetches it (`get_details_info`) and writes that ChannelId into its bank; the layout is
  generated locally from the fixed parameters.
* **Glyph entry**: the altar's "Search by Chalice Glyph" -> `/channel/word_search` (the root object is one entry) -> Join
  0x1aa4a10 (helper, list, index) -> steps 9/12: the entry's FormData is parsed into the helper and written into **the
  selected bank's record**; from then on it is that bank's dungeon, generated locally exactly as the uploader's game does.
  RandomJoin 0x1aa4cb0 (`/channel/random_join`, the unlocked depth per family from flags 6221+k+7i) ends the same way.
* **Session / room**: nothing dungeon-specific. The host's game creates its NP session / room as in the world; every sign and
  sign query carries `AreaId` (an area-29 packed map) and the **ChannelId** of the current bank (FrpgNetMan +0xa88, written
  every frame by the in-game step 0x193ac10 from the current bank's record while online and in an area-29 map). Matching is
  server-side by those two keys.
* **How a guest gets the same layout**: the summoned game does not need the dungeon beforehand. The host's own game sends the
  type-1 summon message over P2P with the room data and, at +0x2c, its ChannelId (filled only in area 29). The guest's
  handler 0x14ba340 keeps the room data in its sign entry and, for a ChannelId other than `0x8000000000000000` and with
  FrpgNetMan +0xb14 bit 0 set (SpEffect 9900 = the Dream bell's "chalice wait"), creates a channel request by id
  (0x1e65290 -> `get_details_info`) in entry +0xc0. When the session join proceeds (0x1872360 -> 0x14bb200) the channel is
  handed to **bank 0's helper** (SprjHolygrail.obj[0] + 0x108, request 6 = QuickJoin, step 13 0x1aa81d0): the FormData is
  written into bank 0's record and StateSwitch 0x1aaa3d0 sets helper state 5 when the entry's Status is 1 and ShareLevel
  != 0; any other state cancels (step 14: the record reset) and the join goes on without a dungeon, so the map load
  (0x1928680, reads the CURRENT bank's record at 0x192876A) loads m255: an empty world. Load mode 1 (SetLoadMode 0x13bb590)
  then puts the host's flag overlay in place and carries only the bank bit.
* **What 0x1e65290 needs (C, decompiled by chal078)**: FrpgNetMan +8 set (the FROM login finished) and +0x9f6 / +0x9f8 /
  +0xa50 clear. The login (`basic_utils/login`) only runs in the title's sign-in chain built by 0x1C37B30: NP auth
  0x1C48F80, **ss.info 0x1C492C0** (fetch 0x1e8aa40, completion 0x1e89850), login 0x1C48AA0, sync_chara_id 0x1C43EE0,
  notice 0x1C485D0, each stage started by the menu framework when the previous future resumes (~1 s apart). A game whose
  ss.info did not parse never reaches the login and plays the whole session with +8 clear; nothing in a world runs the chain
  again (docs/party/network_robustness.md 8b, chalice.md 4).

## 2. Our party mod on the host's dungeon entry, and what the three reports show

What we do (docs/party/chalice.md 4, code party_chalice.cpp, party_b2.cpp DeliverSummon, party_travel.cpp): the host's
`party_chalice::Tick` puts its current bank's FormData on the party's channel board under the host's ChannelId (or an alias
when the id's top bit is set); a host warp into area 29 is a travel event with `"chalice"` that a guest replays only when
its own current bank holds that ChannelId (`ChaliceReplayAllowed`), otherwise the guest stays, the director gives it
SpEffect 9900 and rings, and the direct summon / the bell summon carries the ChannelId so the guest's game quick-joins
(above). Before the room is handed over, `DeliverSummon` asks 0x1e65290 for the id (`ChannelRequestById`): not possible ->
not delivered (3 tries), and since 0.7.8 `ChaliceOfflineHold` keeps such a guest where it is with the notice instead of
the vanilla send-home (OnLeave_Limit +0x138a51c).

**The logs (player names / IPs withheld).** Every guest log of the three reports shows the same title sequence, and the
healthy host logs (and the SAME PC playing as host in be0f493) show the other one:

| | guest (broken) | host / the same PC as host (healthy) |
|---|---|---|
| ss.info | `GET .../bb-us/ss.info -> 200 (7912 bytes, 37-276 ms)` | `GET .../bb-eu/ss.info -> 200 (7912 bytes, 0-1 ms)` (cec05db host), `bb-us ... 1 ms` (be0f493 host) |
| the FROM client after it | `client ss.info not parsed (failures 0)`, user id none, no login request ever | `client ss.info <ss>0`, `POST .../basic_utils/login -> 200` within 1 s, `net 1 online 1` |
| the title | `the FROM sign-in is over` 5 s after the answer (nothing in flight), then the world loads and `Party: online mode 1 -> 0` | `online mode 1` kept |
| the dungeon | 0.7.7 (cab726f): `send-home ... not dropped: the host is in a Chalice Dungeon this game does not have` -> own world -> rings with 9900 -> `direct summon: ... cannot make the channel request yet (FrpgNetMan +8 0 ...) this game is OFFLINE` x3 -> notice "This game is offline" (the screenshot). 0.7.8 / 0.7.9 (be0f493, cec05db): `chalice: notice shown: This game is in offline mode (... no channel request is possible)`, send-home dropped, the guest held in the host's previous map | the host enters m29 online |

Nothing else differs: same patches on both PCs of cec05db (incl. "Party: Skip Online/Offline Choice (Online)"), the party link
up, net_doctor finds no network fault. The 37-276 ms (vs 0-1 ms) is the only visible difference: the guest's FROM requests
go to the host's FromApi over the party link (`http_hle.cpp perform`, the non-host branch), the host's in-process.

**Root cause (C):** `from_api_ss_info()` writes the ss.info element index N (`<gameurlN>`, every numeric tag) from the
FrpgNetMan+0x9e8 **of the process that builds the document**, and the parser 0x1eb65e0 (called at 0x1e89a21 from the
completion 0x1e89850) looks for the ASKING game's N (`local_4d4 = *(FrpgNetMan+0x9e8)`, decompiled). N follows the sell
region (table 0x4736550: us 1, eu 2, jp 3, asia 4, region 4 = 2, region 5 = 3; the "Unlock Game Region" patch makes region
5). So a host in region eu answered a guest in region us with a `<gameurl2>` only, and the guest wanted `<gameurl1>`.
cec05db shows the two regions directly (host bb-eu, guest bb-us); in be0f493 the same PC signed in fine as host (its own N)
and failed as guest (the friend's N); cab726f's guest asked bb-us (host log not sent). Why it is silent: on a parse
failure 0x1e89850 frees mgr+0x18 and only sets the error string at +0x20 (`FUN_0262a720(param_1 + 0x20, ...)`): **no
+0x3e4 failure count** (that is the non-200 path in the dispatcher 0x1e7f240, hence `failures 0`), **no reload timer
reset** (+0xd0 is written only on success), and the drop-offline routine 0x1ea5ce0 is never called, so our
`Party online: ... drops offline` log never fires. The title's ss.info stage sees no document, the chain stops, Continue
loads the world and the game's online choice (FrpgNetMan +0xa) falls back to 0. Our 0.7.9 sign-in hold (BB_PARTY_SIGNIN_HOLD)
correctly released the button 5 s after the answer, because nothing was in flight any more.

The same PC worked as host because the host's own N is the one emitted; loopback smokes never saw it because both games
are the same region (eu, N 2), and `caa` (the region patch) puts the patch on BOTH instances (N 3 and 3).

**Per report:**

1. **NW-cec05db** (guest "offline" on the title, Chalice join impossible) - **C**: host eu / guest us. The "offline" the player sees
   on the title is the game's own fallback dialog (`Party: online mode 1 -> 0`); the "Online: offline mode" launcher line is
   Nightwatch's usual line (not a cause, as noted in 0.7.8).
2. **NW-be0f493** (two friends cannot join a dungeon) - **C** for the mechanism, **L** for the regions (the friend's log not sent, but the
   reporter's PC parsed its own document as host and not the friend's as guest; the reporter says it only ever worked with
   him hosting).
3. **NW-cab726f** (0.7.7: kicked home, then "the load fails" with "This game is offline") - **C** for the chain (the log has every
   step), **L** for the regions (host log not sent). The kick home is 0.7.7's vanilla send-home (fixed in 0.7.8 by the hold);
   "the load fails" is the failed summon's offline notice at the First Ritual Altar in its own Dream (the screenshot).

## 3. The fix (implemented), alternatives, risks

**Done:** a guest answers its ss.info in its own process: `http_hle.cpp perform()` builds
`b64(from_api_ss_info())` for any URL whose path ends in `ss.info` on a non-host (`from_api.h ss_info_answered_locally`),
with THIS game's N; every other FROM request still goes to the host's FromApi. Log once: `http: ss.info answered in this
process ...`. Kill switch `BB_PARTY_SSINFO_LOCAL=0` = the old relay. The ss.info is static content (the gameurl base
`http://bbparty.invalid:18671`, intervals; `fast_join()` now reads the guest's own BB_PARTY_FAST_JOIN switch instead of
the host's, which is what the polling intervals of the guest's own client should follow anyway). The FROM login, chara id
and notices still run through the host as before; the only observable change on a guest is a 0 ms ss.info and a parsed
document whatever the regions.

Alternative (not taken): the host could emit the index set {host N} U {1,2,3,4} for every answer. It fixes the same thing
but multiplies the host's own document (~6 KB -> ~30 KB through the game's 0x400-byte reads and the base64 decoder) and the
guest's region 4/5 cases need N 2/3 as well; a guest of a future build could still be served by an older host. The local
answer needs nothing from the host and nothing of the host's version.

Risks: a guest with no readable FrpgNetMan at answer time gets every fallback index (0..9), as the host did before; a
Nightwatch engine without shadNet is unaffected (same HLE). The 0.7.8 hold and notices stay for a guest that is really
offline (title choice, a failed ss.info URL): `camh` / `cas` cover them.

Unit: `party-host-test` `test_ss_info_local` (the rule, the fallback document). Smoke: **`crg`** (group chalice_alias),
built on `cae`: the guest runs `BB_PATCHES=Skip Intro;Unlock Game Region` (region 5, N 3, URL bb-eu by the 0.7.8 region fix),
the host its own region (eu, N 2); rows: the guest's ss.info 200, the guest's login finished (`FrpgNetMan net 1 online 1 ...
session N chars`), no `online mode 1 -> 0`, the host's dungeon load, the guest into m29 by the summon, no offline notice /
send-home. `CRG_LOCAL=0` puts BB_PARTY_SSINFO_LOCAL=0 on the guest = the old route = fails the login / online rows on master.

## 4. Results

* `party-host-test`: all passed (incl. `test_ss_info_local`).
* **`crg` with the fix** (`agents/chalice081/smoke/crg_fix`, 2 rendered games, loopback): **14/14** (the run was repeated once
  only because my first "host has no patch" row read the region-fix log line, which every start prints; both tries
  otherwise green). Guest: `Patches: ... 'Unlock Game Region'` (region 5, N 3), `http: ss.info answered in this process`,
  `GET .../bb-eu/ss.info -> 200 (7912 bytes, 1 ms)`, `FrpgNetMan net 1 online 1 ... client ss.info <ss>0 ... session 8
  chars` 1.4 s later, no `online mode 1 -> 0`; host (eu, N 2): `altar: the party's dungeon is loading` +79.5 s; guest
  `map - -> m29_00_00_00` +124.5 s; no offline notice, no send-home. Screenshots (220 s): both in the Pthumerian Labyrinth
  layer 1, 0.8 m apart, the guest's name tag on the host's screen, no glitches.
* **`crg` reproduction, `CRG_LOCAL=0`** (BB_PARTY_SSINFO_LOCAL=0 on the guest = the pre-fix relay;
  `agents/chalice081/smoke/crg_repro`, twice, the same 4 rows each time = deterministic): the guest's
  `GET .../bb-eu/ss.info -> 200 (7912 bytes, 1 ms)` then `client ss.info not parsed (failures 0)`, no login, `online mode 1 -> 0`
  at +18.6 s (Continue), `send-home ... dropped: the host is in a Chalice Dungeon and this game is offline (no channel
  request ...)` at +84.9 s and the offline notice: the players' logs line for line (host eu N 2 vs guest region 5 N 3). The
  rows "sign-in finished", "never fell back to offline mode", "guest into m29" and "not offline-held" FAIL; everything else
  (the host's own entry) passes, as in the reports.
