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
5 ExecUpload_forResume, 6 WaitUpload_forResume, 7 Finish_forResume, 8 Exec_forSetupHolygrail
0x1aa7c00, 9 Exec_forJoin 0x1aa7e40, 10 Exec_forRandomJoin 0x1aa7e60, 11 Wait_forRandomJoin
0x1aa7f80, 12 Finish_forRandomJoin 0x1aa81d0, 13 Exec_forQuickJoin 0x1aa8410, 14 Exec_forCancel,
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
  0x1ac3ef0 -> QuickJoin 0x1aa5280 -> step 13 0x1aa8410): the slot-0 record is written from the
  fetched entry (FormData), the chalice groups are cleared (0x13bdc00), and the guest loads the
  host's dungeon. Slot 0 is the network / joined-dungeon slot; a member's own ritual dungeons use
  the others (I: the requesters always pick obj[0] for joins).
* So what the party needs is that **get_details_info finds the host's dungeon**: the host's game
  uploads it when it performs the ritual / resumes it online (helper steps 5-6, ChannelUpload), and
  the board keeps it. **Our rule** (FromApi::sign_get): a host whose sign query is in area 29 gets
  the members' signs (wherever they rang: the Hunter's Dream, the world, another dungeon) only while
  its ChannelId is on the board; otherwise none ("held back" in the log) — the summoned game would
  build its own slot's dungeon. Outside the dungeons nothing changes. The board notes each
  member's current ChannelId from these calls (`random_join` uses it).
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
  HolygrailExParam rows; isOnlyOne, never consumed by a ritual) the host owns go to every guest
  that lacks them (`chalice_goods` event, every change and every 60 s; the guest gives itself the
  missing ones with GiveItemDirect in its own world, load mode 0). Whichever member's dungeon the
  party clears, every member can perform the next ritual itself. A peer's list is filtered to the
  25 chalice ids (no other goods can be given this way).
* Dungeon flags (layer doors, boss kills, treasure in area 29, 1100xxxxx / 2001xxxxx lots) live in
  the **slot's own groups** (areas 40..47 remapped from area 29 by the slot), so a summoned guest
  (load mode 1, the host's overlay) never progresses its own copy, exactly like vanilla. They are
  never written from outside (party_progress `NeverWritable`, items.md: Chalice lots excluded).
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

## 7. Open / not done

* Live: the altar ritual / quick join path was not driven end to end in the test harness (the
  report lists what was run). The ChannelId flag offsets (+32/+64) are read by party_chalice and logged;
  the slot-0 assumption for joins is inferred from the requesters.
* A guest that ends in its own copy of the dungeon (e.g. it entered the same glyph itself) is not
  synchronised to the host's progress there (section 5).
* The exact meaning of Status / ShareLevel / TurnoutLevel values, FixedOrGeneral 1 vs 2.
