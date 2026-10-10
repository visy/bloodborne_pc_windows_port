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
  chalice family from flags 6228+7i.. (four 9-step scans), creates the random-join request object
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
* **Both games must have the same dungeon**: the summoned guest loads the host's map from its own
  data, and for area 29 that data is its current slot's record (section 1). Hence: a member's game
  joins the host's dungeon first (glyph or random join at the Ritual Altar), then the bells work.
* **Our rule** (FromApi::sign_get): when the querying host or a sign is in area 29, the sign is
  listed only if both are in area 29 with the **same non-zero ChannelId**. Outside the dungeons
  nothing changes. The board also notes each member's current ChannelId from these calls
  (`random_join` uses it).
* **Travel** (party_travel, B1): a host warp into area 29 (packed map, exact-position map, lamp
  `29xxxxx`, bonfire entity, a host death / Mark respawning at a dungeon lamp) carries the host's
  ChannelId (`"chalice"` in the travel event). A guest replays it only when its own current slot
  holds that ChannelId (`ChaliceReplayAllowed`); otherwise it stays (logged: "join it at the Ritual
  Altar"), because a warp into area 29 would generate the guest's own slot's dungeon (or none) —
  a different layout. The guest death / Mark redirect to the host's last lamp skips a dungeon lamp
  of a dungeon the guest does not have.
* **4-player / scaling**: the m29 scripts already use the multiplayer doping (party_fourp.md,
  `Lua_MultiDoping`, 7501/7502).

## 5. Progress and rewards

* Dungeon flags (layer doors, boss kills, treasure in area 29, 1100xxxxx / 2001xxxxx lots) live in
  the **slot's own groups** (areas 40..47 remapped from area 29 by the slot), so a summoned guest
  (load mode 1, the host's overlay) never progresses its own copy, exactly like vanilla. They are
  never written from outside (party_progress `NeverWritable`, items.md: Chalice lots excluded).
* What a guest gets in vanilla, and here: boss kill echoes / Insight as a cooperator, the shared
  item drops it picks up itself. Chalice rewards (the next chalice, Ritual materials from bosses)
  are host-only `2003[4]` awards (items.md); C3's ledger leaves area 29 out.
* **Why no replay into the guest's slot**: the guest's slot record and the host's are the same
  dungeon only when the guest joined it (same ChannelId); writing the host's area-29 flags into the
  guest's slot groups would need the guest's slot index mapping (9020-9026 one-hot, remapped by
  0x13b89b0 on every load) and would bypass the game's own slot bookkeeping (the clear 0x13bdc00 on
  quick join / new ritual). Too risky for a save; not done. The vanilla path remains: a guest that
  joined the same dungeon (glyph / random join) plays it in its own world and keeps its own
  progress there; boss doors opened in co-op stay open in the host's world only.

## 6. Open / not done

* Fully automatic "follow the host into its dungeon": the random join (0x1aa4cb0 on
  `SprjHolygrail.obj[slot] + 0x108`) needs the helper's state machine running (the altar menu
  drives it) and the warp into the dungeon afterwards (the altar's own path). Not automated; the
  member uses the altar's "Join a random dungeon" (the board answers with the host's dungeon) or
  the host's glyph (logged by the host's game: `uploaded chalice dungeon N, glyph xxxxxxxx`).
* The exact meaning of Status / ShareLevel / TurnoutLevel values, FixedOrGeneral 1 vs 2.
