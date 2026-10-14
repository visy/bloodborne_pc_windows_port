# Chalice Dungeons in party co-op

Bloodborne 1.09, our offsets (raw ELF VA of `eboot.elf`; bbhost = ours + 0x400000). Confidence:
**C** confirmed (code read / test), **L** seen live, **I** inferred, **G** guess.
Code: `gpu/shim/net/from_api_channel.{h,cpp}` (the channel board), `gpu/shim/net/from_api.cpp`
(ChannelId sign matching), `gpu/shim/party/party_chalice.{h,cpp}` (the local dungeon),
`gpu/shim/party/party_travel.*` (the chalice travel guard). Tests: `party-host-test`
(`test_chalice_channels`), `party-travel-test` (`test_chalice`).

## 1. What a Chalice Dungeon is, in the game's data

* **Chalices** are goods whose id is a `HolygrailExParam` row (C): 6109 Pthumeru Chalice (ritualLv 1,
  `requiredMaterialId` 109), 6209 Central Pthumeru, 6219 ..., `holygrailTypeId` 0..3 is the
  family (Pthumeru / Hintertomb / Loran / Isz), `mapBaseId` the map set, `fixedSf_* / freeSf_*` the
  sub-feature (rite) lots. `RitualRequiredMatParam[requiredMaterialId]` lists the ritual's materials
  and echoes: row 109 = 1000 echoes + Ritual Blood (1) (goods 7000) x2 (C, param dump).
* **Maps**: area 29 (`m29_BB_RR_00`, 300+ pieces); a dungeon is assembled at load time from pieces
  according to the slot's layout parameters (I). Area-29 AreaIds in signs are ordinary packed maps
  (`0x1Dbbrrii`), all in `party_ids.inc` (C).
* **Slots** (C, 0x1ab2970 / 0x1ab2b40 / 0x1ab2f70 / 0x1ab34d0 and their callers): a player has 7
  dungeon slots. The one-hot flags 9020..9026 select the current slot (`GetEventFlagValue(9020, 7)`,
  slot = 7 - bit length; event_flags.md 2). Slot k's **record lives in the event flag store**, in
  the type-9 group of area 40+k (ids `94000000 + k*100000`, block 0 zone 0, spilling into zone 1):

  | bits | content | writer |
  |---|---|---|
  | +32 / +64 | ChannelId low / high dword (`0x8000000000000000`: never uploaded) | 0x1ab2970 (helper +0xd8) |
  | +96..+127 | 4 bytes, stored high byte first (helper +0xe0) | 0x1ab2b40 |
  | +128 | 32 bits (helper +0xe8) | `SetEventFlagValue(+128, 32)` |
  | +160..+735 | 9 x {u32, u32} (helper +0xf8: the layout parameters, I) | 0x1ab2f70 |
  | +736 | 16 bytes (helper +0x140) | `0x13bcdf0` (byte writer) |
  | +992.. | 0x30 bytes (helper +0x158: `{2, 0x30, ...}`, an online id serialization, I) | `0x13bcdf0` |

  The glyph string (helper +0x150 -> object +0x180, wide) is normalised by 0x1ab34d0 (fullwidth
  `#%<>?\` and yen to ASCII) and stored too. The chalice groups (areas 40..47 / 99) are separate
  from the main world's (event_flags.md 2: the remap 0x13b89b0, the clear 0x13bdc00), so nothing
  dungeon-side touches main-world progress.
* **Singletons** (C): `SprjHolygrail` slot **0x553e9e8** = `{u32 count = 7, ..., +8 -> obj[7]}`,
  created on save load (0x156bc67 -> 0x1ac3ca0); each `obj[k]` (0x6a8 bytes, vtable 0x5336570,
  ctor 0x1ab4370, slot index at +0x100) holds a **CSDungeonRitualHelper** at **+0x108** (ctor
  0x1aa3110, step table 0x557e248).

## 2. The ritual helper (CSDungeonRitualHelper) — C

Steps (names at 0x49955ee.., functions registered at 0x1aaa978..0x1aaae0a): 0 Init 0x1aa64a0,
1 Wait_forRequest 0x1aa64c0, 2 Exec_forSuspend, 3 Exec_forResume, 4 Wait_forResume,
5 ExecUpload_forResume, 6 WaitUpload_forResume, 7 Finish_forResume 0x1aa76b0, 8
Exec_forSetupHolygrail 0x1aa79c0, 9 Exec_forJoin 0x1aa7c00, 10 Exec_forRandomJoin 0x1aa7e40, 11
Wait_forRandomJoin 0x1aa7e60, 12 Finish_forRandomJoin 0x1aa7f80, 13 Exec_forQuickJoin 0x1aa81d0,
14 Exec_forCancel 0x1aa8410 (the slot record reset to empty),
15..21 PresentOffering (offer materials: ChannelAddMaterial), 22..27 ChangeShareLevel, 28
ErrorRecovery, 29 StateSwitch 0x1aaa3d0, 30 Finish.

* Requests: `helper+0x260` = 1 Suspend, 2 Resume, 3 SetupHolygrail (the ritual), 4 Join, 5
  RandomJoin, 6 QuickJoin, 7 Cancel, 8 PresentOffering, 9 ChangeShareLevel, 10 ErrorRecovery;
  Wait_forRequest maps them to steps 2/3/8/9/10/13/14/15/22/28. `+0x264` = request in progress,
  `+0x25c` = state (1 idle, StateSwitch sets 2/3/4/5/6).
* Requesters: **Join** 0x1aa4a10(helper, list, index) takes a channel object from a search
  result list (refcounted, state at +0x110, ChannelId at +0x10, glyph wstring at +0x180);
  **RandomJoin** 0x1aa4cb0(helper) needs nothing else: it reads the player's unlocked depth per
  chalice family from flags 6221+k+7i, i = 1..9 (family k = 0..3), creates the random-join request object
  with 0x1e65580 (which sends **ChannelRandomJoin**) and starts step 10. Both refuse unless
  online (FrpgNetMan +8 set, +0x9f6/+0x9f8/+0xa50 clear), idle (+0x25c == 1) and no request
  pending. Callers: the altar menu (0x1abea30, menu event 2000 = decide; 0x1c0da40 functor).
* Join / random join end in 0x1aa7f80 / 0x1aa81d0: the channel object's data (0x1aa3b90 ->
  0x1a9c490: ChannelId, layout, glyph, creator) is written into **the helper's slot** record
  (above), then StateSwitch. So a joined dungeon becomes the slot's dungeon: generated locally from
  the record, exactly as the uploader's game generates it.

## 3. The server side: /channel/* — C (eboot), our answers

Builders and readers (from_api_channel.h has the summary). Request keys (C):

| API | path | request keys | reply the game reads |
|---|---|---|---|
| 12 Upload (0x1e9af40) | `/channel/upload` | HolyGrailTypeId, FormDataVersion (0), Status, FixedOrGeneral, FormData (base64), CharaId, ShareLevel, SubFeatureFlag, WishMaterialList [{WishMaterialMainTypeId, WishMaterialSubTypeId, WishMaterialCount, SpendMaterialCount}], RitualLevel | `ChannelId` (int), `DiscernmentWord` (string) (handler 0x1e804be) |
| 13 Share (0x1e9c0a0) | `/channel/share` | CharaId, ChannelId, ShareLevel | ResKind |
| 14 Search (0x1e9ee50) | `/channel/search` | HolyGrailTypeId / SubFeatureFlag / RitualLevel (null = any), FormDataVersion, Status, FixedOrGeneralList [{FixedOrGeneral}], GetCount | `ChannelList` [entry] |
| 15 WordSearch (0x1ea0420) | `/channel/word_search` | SearchWord, FormDataVersion | **the root object is one entry** (0x1e81c1c) |
| 16 GetDetailsInfo (0x1e9d5b0) | `/channel/get_details_info` | ChannelIdList [{ChannelId}] | `ChannelList` [entry] |
| 17 GetInfo (0x1e9cf60) | `/channel/get_info` | ChannelIdList [{ChannelId}] | `ChannelInfoList` [{ChannelId, Status, ShareLevel, TurnoutLevel}]; ResKind 0x1040a -> `LostChannelIdList` [{LostChannelId}] |
| 18 RandomJoin (0x1e9dbf0) | `/channel/random_join` | RandomJoinTargetList [{HolyGrailTypeId 0..3, RitualLevel}], CharaId, FormDataVersion | **the root object is one entry** |
| 19 AddMaterial (0x1e9c4d0) | `/channel/add_material` | ChannelId, MaterialList [{MaterialMainTypeId, MaterialSubTypeId, MaterialCount}] | `CompleteFlag` |
| 20 AddMaterialCompleteNotify | `/channel/notify_add_material_complete` | | ResKind |

An **entry** (reader 0x1e89da0, 0xE0-byte object): ChannelId (+0), ShareLevel (+8), Status (+0xc),
FormData (base64, decoded only when `FormDataVersion` == 0; +0x10 data, +0x18 size),
DiscernmentWord (+0x20 wstring), RitualLevel (+0x58), HolyGrailTypeId (+0x5c), FixedOrGeneral
(+0x60 = != 0, +0x61 = == 2), SubFeatureFlag (+0x64), CreateUserId (+0x70), CreateCharaId (+0x78),
CreateDate / LastPlayDate (`YYYY-MM-DDTHH:MM:SS`, +0x80 / +0x90), TurnoutLevel (+0xb0),
WishMaterialList (+0xb8). The bells (`summon_messenger/create|get`), tomb / blood messages and
wandering ghosts all carry a `ChannelId` too: the server keys everything in a dungeon by it.

Before this work the party answered `word_search` / `random_join` with `{"ResKind":0,
"ChannelList":[]}`, i.e. an entry with ChannelId 0 and no FormData — a "join" into nothing.

**Our board** (`ChannelBoard`, the party host's process; party-only by construction):
* `upload`: ChannelId = 52-bit FNV-1a of the FormData text, glyph = 8 chars of `[a-z0-9]` from
  another hash of it. The same dungeon (same FormData) keeps its id and glyph, also across host
  restarts and when a joined member's game re-uploads it; a different FormData under an existing
  id (collision / forgery) is refused. FormData must be strict base64, <= 256 KiB text,
  FormDataVersion 0. 64 channels max, 16 per member (least recently used dropped).
* `word_search`: by glyph (case-insensitive); unknown -> ResKind 0x1040a.
* `search`: the other members' dungeons matching the non-null criteria, newest first.
* `get_details_info` / `get_info`: by id; unknown ids are left out (never reported lost: a "lost"
  answer would make a game drop a dungeon it uploaded in an earlier session).
* `random_join`: the dungeon another member is in **now** (the ChannelId its bells last carried in
  area 29), else the newest other member's upload; never the caller's own; the target list's
  family / depth limits are ignored (the party joins its own dungeons).
* `share`: only the uploader changes the share level. `add_material`: `CompleteFlag` 0.
* **Shared fixed search** (live, the altar's ritual with a chalice): the game does not upload a
  "fixed" chalice's dungeon; it searches `FixedOrGeneralList [{2}]` with the chalice's
  HolyGrailTypeId / RitualLevel / SubFeatureFlag and **must** find one (an empty list -> 0x80000005
  -> msg 0x11f9 "Failed to connect to Bloodborne server" and back to the title). The board answers
  with a synthetic channel keyed `fixed:grail:level:rites` (same id for every member, also for the
  caller's own ritual) without FormData; the game then fetches it (get_details_info), writes the id
  into its slot record and generates the fixed layout locally. A later `upload` with
  FixedOrGeneral 2 reuses that id.
* **FormData** (reader 0x1a9c490; version 5 = 0x8C bytes, 4 = 0x7C bytes without the name):
  `+0 u32 version, +4 u32 size, +8 u32 (helper +0xe0), +0xc u32 (helper +0xe8), +0x10 18 u32 (9
  layout pairs, helper +0xf8), +0x58 16 bytes (helper +0x140), +0x68 creator name (v5)`. The slot
  record (event flags 94000000 + k*100000, written by 0x13bcdf0 as 32-bit chunks) holds the same:
  ChannelId +32/+64, +96, +128, +160..+735 the pairs, +736 the 16 bytes, +992 the name. The host's
  `party_chalice::Tick` rebuilds a version-4 FormData from its current slot's record and puts it on
  the board for the synthetic channel (`FormData from slot S's record (124 bytes) on the board`):
  otherwise the summoned guest's quick join fetched an entry with no layout and loaded an empty map
  (live run V: it fell to y -1500 and died).

## 4. Co-op inside a dungeon

* Vanilla matching is server-side (from_api_schema.md 5: no client-side area / password check on
  sign intake). The client sends `AreaId` and `ChannelId` with every sign and sign query.
* **The summoned game does not need the dungeon beforehand** (C, 0x14ba340 / 0x14bb200): the
  host's game sends its own summon message (the type-1 item over P2P, np_session.h) to the sign's
  owner; it carries the host's room data and, at +0x2c, the **host's ChannelId**. The guest's
  handler 0x14ba340 stores the room data in its sign entry (+0xb8, state +0xc8 = 2) and, for a
  ChannelId other than `0x8000000000000000`, creates a channel request by id (0x1e65290 ->
  **get_details_info**) in +0xc0. When the session join proceeds (0x1872360 -> 0x14bb200), the
  guest's game hands that channel to **slot 0's helper** (`SprjHolygrail.obj[0] + 0x108`,
  0x1ac3ef0 -> request 6 0x1aa5280 -> STEP_Exec_forQuickJoin 0x1aa81d0): the FormData is parsed
  (0x1aa3b90 -> 0x1a9c490) into the helper and written into the slot-0 record, then StateSwitch
  (0x1aaa3d0) sets the helper state (+0x25c): **5** when the channel object's +0x54 == 1 (or 3
  with +0x10c bit 0), else 3. 0x1ac3ef0 succeeds only with state 5; with 2..4 it requests 7
  (STEP_Exec_forCancel 0x1aa8410: the slot record reset to empty) and the summon goes on without a
  dungeon. The map load (0x1928680) reads the current slot's record (+96 = the dungeon map
  0x1dxxxxxx) and otherwise loads m255 (an empty world: the guest falls and dies).
  The channel object's +0x54 is the entry's `Status`, copied only when `ShareLevel` != 0
  (0x1e6dea2); `CompleteFlag` of add_material sets it too. Slot 0 is the network / joined-dungeon
  slot; a member's own ritual dungeons use the others (I: the requesters always pick obj[0]).
* **What the board answers for it** (live runs V..#13): the entry must carry the dungeon's
  **FormData** (the host's game never uploads a shared fixed dungeon; party_chalice puts a
  version-4 FormData built from the host's live ritual helper on the board, `FormData from slot
  S's record (124 bytes) on the board`), and a **ShareLevel != 0 with Status 1** for the summoned
  member. The member performing the ritual of that dungeon (its fixed search within 120 s) gets
  the stored ShareLevel 0: with 1 its own ritual generated a local dungeon (ChannelId with the high
  bit set) instead (run #12).
* **Load mode 1**: SetLoadMode(1) (0x13bb590) moves the flag groups onto other storage and only
  carries the slot bit 9020+k. party_chalice replaces it to also carry the quick-joined slot
  record (snapshot before, written after; only within 180 s of a quick join; transient storage,
  never the member's save): `load mode 1: slot 0's record (dungeon N, map 0x1d0a5a00) carried
  over`. The map load itself already happened in mode 0 (the record is there then); this keeps the
  record readable for later loads in the host's world.
* So what the party needs is that **get_details_info finds the host's dungeon**: the host's game
  uploads it when it performs the ritual / resumes it online (helper steps 5-6, ChannelUpload), and
  the board keeps it. **Our rule** (FromApi::sign_get): a host whose sign query is in area 29 gets
  the members' signs (wherever they rang: the Hunter's Dream, the world, another dungeon) only while
  its ChannelId is on the board; otherwise none ("held back" in the log) — the summoned game would
  build its own slot's dungeon. Outside the dungeons nothing changes. The board notes each
  member's current ChannelId from these calls (`random_join` uses it).
* **The summon message carries the ChannelId only for a waiting guest** (C): the host's game fills
  +0x2c from FrpgNetMan +0xa88 only in area 29, and the guest's 0x14ba340 takes it only with
  FrpgNetMan +0xb14 bit 0 set (from +0xb9c bit 0 = SpEffect stateInfo 269 = **SpEffect 9900**, the
  vanilla Dream bell's "chalice wait"). The party rings with its own bell patch (9005), so the
  director also applies 9900 to a guest that rings while the host's roster map is in area 29, and
  to a guest whose sign is already out when the host enters its dungeon (run #10: the guest rang
  ~10 s before, the summon then carried no quick join) (live: `summon message type 1, host map
  0x1d0a5a00, ChannelId 0x3f22b458bd799` -> two get_details_info -> quick join).
* **No gather into a dungeon**: GatherGuest skips the gather while the host's map or lamp is in
  area 29 (it warped the guest to the host's dungeon lamp = its own slot's dungeon).
* **The automatic flow**: host and guests are in the Hunter's Dream (B1 travel follows the host
  there); the host performs the ritual / enters its dungeon at the altar (the upload reaches the
  board); the guests stay (below) and keep ringing the Small Resonant Bell (PartyDirector); the
  host's director rings the Beckoning Bell inside the dungeon; the host's game summons; each guest's
  game quick-joins the host's dungeon by ChannelId and loads in next to the host.
* **Travel** (party_travel, B1): a host warp into area 29 (packed map, exact-position map, lamp
  `29xxxxx`, bonfire entity, a host death / Mark respawning at a dungeon lamp) carries the host's
  ChannelId (`"chalice"` in the travel event). A guest replays it only when its own current slot
  holds that ChannelId (`ChaliceReplayAllowed`); otherwise it stays where it is and is summoned
  (above): a warp into area 29 would generate the guest's own slot's dungeon (or none) — a
  different layout. The guest death / Mark redirect to the host's last lamp skips a dungeon lamp of
  a dungeon the guest does not have. `Party: chalice: slot S, dungeon N, load mode M` logs each
  game's current dungeon (party_chalice.h reads it from the flags).
* **4-player / scaling**: the m29 scripts already use the multiplayer doping (four_players.md,
  `Lua_MultiDoping`, 7501/7502).

## 5. Progress and rewards

* **Chalices for everyone** (party_chalice.h): the chalices (goods 6100..6539 that are
  HolygrailExParam rows; isOnlyOne, never consumed by a ritual) any member owns go to every member
  that lacks them (`chalice_goods` event, every change and every 60 s): a guest sends its list to
  the host, the host gives itself the missing ones and broadcasts its (now merged) list, and each
  guest gives itself the missing ones in its own world (load mode 0; GiveItemDirect). Whichever
  member's dungeon the party clears, every member can perform the next ritual itself, and the host
  can open the altar menu for a dungeon a guest made (the altar refuses everything with "A Holy
  Chalice is required to conduct a ritual." when the player owns no chalice, run c16). A peer's
  list is filtered to the 25 chalice ids (no other goods can be given this way).
* Dungeon flags (layer doors, boss kills, treasure in area 29, 1100xxxxx / 2001xxxxx lots) live in
  the **slot's own groups** (areas 40..47 remapped from area 29 by the slot), so a summoned guest
  (load mode 1, the host's overlay) never progresses its own copy, exactly like vanilla. They are
  never written from outside (party_progress `NeverWritable`, items.md: Chalice lots excluded).
* Insight (parg, 0.6.2): the dungeon's boss Insight is not common 9350 but m29's own events
  (12901718/722 first encounter, 12901708/712 kill table, 12901588/589/591 the layer boss's +2),
  bare SpEffect 4680 that end on a client; the host's are counted and sent to the guests
  (party_phantom DirectInsight; live by the `host4680` probe, not yet with a real boss).
* What a guest gets as a cooperator: boss kill echoes / Insight (and the C3 parity patches' full
  rewards), the drops it picks up itself. Boss reward lots in the dungeons (Ritual materials, the
  chalice drops) are host-only `2003[4]` awards; C3 does not replay them: they are flagless and
  repeatable per dungeon (no per-player flag or ledger bit to give them exactly once). The chalices
  among them reach the guests through the chalice share above.
* **Why no flag replay into the guest's slot**: the guest's quick join cleared its slot-0 groups and
  rebuilt the record; writing the host's area-29 flags there would need the guest's slot mapping
  (9020-9026 one-hot, remapped by 0x13b89b0 on every load) and would bypass the game's own slot
  bookkeeping (0x13bdc00). Too risky for a save; not done. Main-world flags are never touched by
  any of this: the chalice groups are separate (event_flags.md 2).

* **Vanilla side effect, unchanged**: the quick join rewrites the summoned game's slot-0 record
  and clears the current chalice groups (0x1aa8410 -> 0x13bdc00) — a member summoned into a chalice
  dungeon loses the progress of the dungeon it had in that slot, exactly as in the original game.

## 6. The altar objects (for automation later) — C

* `SprjHolygrail.obj[k]` is a **SprjDungeonGateIns** (the per-slot altar object, step table
  0x557e840, steps registered at 0x1abf510: Init 0x1ab5370, ChannelLoad, ModeSwitch 0x1ab5490,
  DispError, NoHolygrail 0x1ab5750 (the altar menu: its decide handler 0x1abea30 calls the random
  join), LocalRitual 0x1ab71c0, ServerRitual 0x1ab7740, LocalDungeon 0x1ab7a50, ServerDungeon
  0x1ab7d00, Finish). `+0x364` = mode (0..6 -> steps 1/8/11/14/17/20/5), `+0x36c` = helper busy,
  `+0x368` = the request it forwards to its helper (`+0x108`).
* The ritual request 0x1aa47a0(helper, ritual) takes a ritual descriptor the altar menu builds
  (0x1ab9270 / 0x1b038b0; chalice id at +0x10) and the creator's name (PlayerGameData +0x5d0).
* The test fixtures used to get there: `BB_PARTY_DIRECTOR_TEST=goods=6109:1,goods=7000:2,
  echoes=5000,log_pos,place=MAP:X:Y:Z:YAW,warp=MAP:POINT` (party_director.h). The Ritual Altar
  part (`アイテム_祭壇`, entity 2111101) stands at (136.19, -19.17, 139.77) in m21_01; its action
  region 2112999 (box 2.2 x 8 x 1) at (131.77, -19.96, 140.54).

## 7. Live evidence and open points

* **Run #13** (2 instances, `chalice PARTY #13`): the host (Hunter0) performs the Pthumeru
  Chalice ritual at the First Ritual Altar (MenuScript), its search makes the shared fixed channel
  1110692594440089 (glyph wmqbhonw), it enters m29_00 and the board gets the FormData from its
  helper. The guest (Hunter1, Hunter's Dream) rings with 9900, is summoned: `summon message type
  1 ... ChannelId 0x3f22b458bd799` -> `quick join request` -> `STEP_Exec_forQuickJoin` -> `slot 0,
  dungeon 1110692594440089 ... gate mode 5` -> `load mode 1: slot 0's record ... carried over` ->
  map m29_00_00_00, position (40.4, 31.1, -91.0) steady, cooperators 1, roster in_host_world in
  the host's dungeon until the run ended. **Run #14** reproduced it with screenshots: both games in the
  Pthumerian Labyrinth, the host sees the guest phantom (Hunter1) in the corridor ahead.
* Before the fixes: run V/W the guest fell through an empty map (m255: no FormData, then the
  quick join cancelled by state 3), run #10 the guest loaded the host's Dream copy (no 9900).
* A guest that ends in its own copy of the dungeon (e.g. it entered the same glyph itself) is not
  synchronised to the host's progress there (section 5).
* The vanilla quick join overwrites the summoned member's slot-0 record (section 5).
* The exact meaning of ShareLevel / TurnoutLevel values, FixedOrGeneral 1 vs 2; uploaded (root /
  "general") dungeons with pending rite materials (Status 0) are not joinable by a summon (as in
  vanilla: +0x54 stays 0).
* **Full clear of a layer, 2026-10-11 (runs c11-c15, 2 instances, 1792 MiB each)**: host ritual at
  the First Ritual Altar (MenuScript), guest summoned (bell_host@29 / bell_guest@29); lever
  12900192 and the layer door by host_press; layer boss 2900100 by host_kill: PREY SLAUGHTERED on
  both, `K10: boss-clear leave suppressed` for host and guest, the guest's send-home at +0x13859df
  dropped; guest award lots 131302160 / 110001000 "in another's world" (item-get on screen).
  The post-boss lift 2901004 (EMEVD 12900354 / 12900351, regions 2903020 top y 31 / 2903021 bottom
  y 10): host on the car at (-65, 31.6, -91), guest at (-64.6, 31.6, -90.4) -> both ride 31.5 ->
  10.5 in 6 s (log_pos, c15). Warping 114 m straight from the lamp into the empty shaft (c13) fell
  to death (the car rests at the bottom until called); the call lever 2901005 did not react to a
  Cross from (-58.5, -90.6). host_lamp=2102961 -> both in the Dream with one load each, the guest
  re-joined (cooperators 1).
* **A ritual by a guest** (user decision: anyone performs the ritual, the party enters together).
  The guest performs the ritual in its own Dream; the host enters that dungeon from its own altar
  with one menu choice, and the guests are summoned as for a host ritual. Pieces:
  - the chalices go both ways (section 5): the altar refuses everything without one (c16:
    "A Holy Chalice is required to conduct a ritual.");
  - the board remembers each member's last ritual (`latest_ritual`: its fixed search or upload);
    while another member's dungeon is newer than an hour and not in the host's slot, the host's
    altar item 262002 "Search by Chalice Glyph" reads **"Enter the party's dungeon"** (help: "Enter
    the Chalice Dungeon <member> made (glyph ...) with the party.") through a text override on the
    game's message lookup, and its 8-character glyph box is confirmed at once with that glyph
    (`bbgpu_ime_preset`; logs `party dungeon at the altar: ...`, `the altar's glyph box gets the
    party dungeon's glyph ...`). No typing;
  - the guest sends its slot's FormData (`chalice_form` event; a shared fixed dungeon is never
    uploaded): without it the host's join did nothing (c18);
  - the glyph answer carries a ShareLevel (as get_details_info does): with the stored 0, "Join the
    selected Chalice Dungeon?" -> YES returned to the altar without a load (c19);
  - a guest outside a dungeon no longer follows the host's warp into its own copy of the same
    dungeon (it was then summoned out of it again: two loads, c20); it waits for the summon;
  - the IME buffer holds maxTextLength + 1 units (the glyph lost its 8th character, c17).
  **Run c20** (2026-10-11): guest ritual -> host altar "Enter the party's dungeon" -> OK -> the
  result -> YES -> altar -> "Select location to awaken in" -> m29; guest summoned by quick join;
  boss kill (K10 both), guest's own lots, the lift down with the host (31.5 -> 10.5). c23 repeated
  the entry up to the join. Still more presses than needed: the search result, YES and the
  awaken prompt could be taken automatically.
* **A dungeon from an earlier session** (c22): the board is new each session, so a host entering a
  dungeon of an earlier ritual had no ChannelId on it and the members' signs were held back. The
  host's game now adds it (`... is in chalice dungeon N (not on the board yet): added`); the guest
  was summoned (c22, host started from a post-ritual test save).
* **The lift call lever** (c22): the guest, in the host's world at the top, pulled 2901005 from
  (-58.5, 31.6, -90.1) facing +z while the car was at the bottom with the host: EMEVD 12900351 ran
  and the car came up to the guest. The earlier "did nothing" (c13/c15) was a press while the car
  was already at the top (the car starts there) or from 0.5 m further back.
* **Layer 2**: the next layer boss 2900200 (MSB (56.3, -59.7, -35.2)) is not loaded at the bottom of
  the first lift (host_kill: NOT loaded); a real descent through layer 2 needs its route (not done).
  Lamps 2902950..2902959 are the dungeon's ReturnPoints; the lamp objects 2900950..2900958 stand at
  (40, 31.5, -54) layer 1 start, (-48.1, 30.7, -93.4) layer 1 boss room, (-106, 10.1, -89.8),
  (-132.7, 17.4, -15.5), (-128, -10.9, 53.2), (24.5, -17, 49.5), (98.7, -59.9, 50), (95, -66, -115.5)
  (a lamp id +2000 = its object id is a guess, not checked).
* **Since 0.5.2 work (2026-10-11, runs c24-c26)**:
  - a guest's current dungeon (from any session) reaches the host's board through its FormData event
    and becomes the party's dungeon at the host's altar (`dungeon N of Hunter1 (slot 1): FormData
    added to the board`, `party dungeon at the altar: ...`, c24b/c24c: verified up to the altar;
    both runs were ended early by the central idle reaper before the host's entry);
  - "one press": `BB_PARTY_ALTAR_AUTO=1` answers the prompts after the glyph box (OK, the result,
    YES, the altar, the first awaken point). **Off by default: not verified live** (no run reached
    it before the 0.5.2 freeze);
  - the bottom lift lever 2901006 (c26b, from (-71.5, 10.6, -91.9) facing -z and (-71.5, 10.6,
    -93.5) facing +z) showed no lift call; the guest's top-lever call (c22, c25) did not repeat in
    c26b (the guest stood at z -90.5 after the warp, 0.4 m further back than in c22). Open;
  - a guest outside a dungeon that holds its own copy no longer follows the host into it (one load
    instead of two): code in place, not confirmed live (c24c ended before the entry);
  - screenshots c26_* (entry, boss, lift, both instances): item-get boxes on both, the boss-clear
    banner fading on the guest; no floating bars or tags, no ghost characters.
  - test harness: the central reaper (tools/mp/reaper.py) ends runs whose party log lines do not
    change for 10 s; a `<root>/noreap` file with seconds lets the long ritual / altar phases pass.

