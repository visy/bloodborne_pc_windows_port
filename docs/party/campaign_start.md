# Party C1: campaign start (character creation → first summon)

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA of
`smoketest/out/eboot.elf`, file offset = VA + 0x4000; bbhost = ours + 0x400000;
`patches/Bloodborne.xml` `Address` = ours + 0x400000). Game data: `E:\games\bloodborne\dvdroot_ps4`
(EMEVD/EMELD, gameparam, MSB, talk ESD, msg). Tools: `tools/party/flag_tool.py` (EMEVD/param
reader); Ghidra `tools/re/decomp.py`; capstone over the eboot. Short scratch scripts were used for MSB
collision parsing and raw EMEVD listings; they are not part of the repo.

Markers: **[C]** = confirmed (disassembly/decompile or game data read here), **[I]** = inferred
(naming, shape, or one link not proven; each has a probe in §6).

---------------------------------------------------------------------------------------------------

## 0. TL;DR

* In vanilla the first co-op you can do is in **Central Yharnam outside the clinic**. Before that, each
  player has to: finish the opening cutscene (flag 12410000), die or use the lamp to reach the **first Hunter's
  Dream** (cutscene event/flag 9401), gain **Insight ≥ 1**, and load the Dream again so the **Doll wakes
  up** (12100105). After that the bath messenger hands over the **Beckoning Bell** (lot 10010,
  goods 200, flag 6622), and the **Small Resonant Bell** goes on sale in the Insight shop for 1 Insight (goods 205,
  flag 6610). [C]
* **Iosefka's clinic is a no-multiplayer zone**: its collisions carry SOS area **−241000**. Negative
  areas grey out the Beckoning Bell (0x157F8C2, unsigned `> 999999`) and refuse the resonant bell
  (0x191A8BC). The Hunter's Dream (area 210000) is blocked by flag **2100**, which the Dream sets on every load.
  The Abandoned Old Workshop (m21_01) is negative everywhere. [C]
* The existing party patches already cover most of the start: `Party: Bells anywhere` (negative
  areas), `Party: Bells without Insight`, `Party: Bells after boss defeated`. One addition is needed: the Dream table
  dword (**0x47304B0: `34 08 00 00` → `FF FF FF FF`**). With these patches the game allows a summon anywhere
  from the moment a player stands in the world. [C for bytes, I for runtime]
* The director rings the bells through Lua events. That path does not go through the inventory-availability
  check (0x157F200), and SendSign 0x1901320 has no inventory lookup, so **owning the bells
  is probably not required** for director summons [I, probe PR1]. For manual use, grant them natively (§4.3).
* Recommended flow: **party lobby from the title screen**. Everyone creates their own character in their
  own save, then plays the 3–5 minute prologue in parallel (clinic → first death → first
  Dream: cutscene + starting weapons). Each player becomes "ready" at **9401 ON + weapons taken**,
  and the director starts summons at the first-floor sickroom lamp (2412950) / Central Yharnam. Summoning
  inside the clinic before anyone has a weapon works with the patches but is an experimental option
  (§4.5).

---------------------------------------------------------------------------------------------------

## 1. Vanilla prerequisites (Q1)

### 1.1 Items

| Goods | Name (engus item.msgbnd) | How you get it | Flags | |
|---|---|---|---|---|
| 200 | Beckoning Bell | m21_00 event **12101024** "マルチアイテムくれるメッセンジャーアニメ": messenger 2100231 waits for **12100105** (Doll awake), then for **12101025** (set by talk ESD `t210721`, i.e. talking to it), then `2003[4]` **lot 10010** (only if chr type 0 = own world). Skipped once 6622 is ON | lot 10010 → goods 200 ×1, getItemFlagId **6622** | [C] |
| 205 | Small Resonant Bell | **Insight shop** ShopLineupParam 200000/210000/220000/230000/240000: shopType 5, value **1**, visible when qwcId **6622** (Beckoning Bell received), stock flag **6610**, quantity 1 | 6610 | [C] data; shopType 5 = Insight currency [I] (prices 1..60 match the bath messenger shop: 3020/3030 Blood Stone 20/60) |
| 225 | Sinister Resonant Bell | same rows `+1`, visible after 6610, stock flag 6650 | 6650 | [C] |
| 4312 | **Old Hunter Bell** (NPC summon bell, not the Beckoning Bell) | event 12101026: needs 12101022 (Notebook messenger done) + 12100105, talk flag 12101027 (`t210720`), lot 10050 | 6670 | [C]. Correction to npc_peer.md §"goods 4312 (presumably the Beckoning Bell)" |
| 120 | Notebook | event 12101022, lot 10000, talk flag 12101023 | 6620 | [C] |

EquipParamGoods: 200 `consumeHeroPoint` 1, 205/225 0. 205 and 225 have `disable_offline` and the
"online only" bit, so they are greyed out offline (0x157F58E: `flow+0x1590 == 0 && goods+0x42 bit21`). [C]

### 1.2 Insight

* Beckoning Bell: greyed out when cost > Insight (`0x157FA51 movzx ecx,[goods+0x38]; cmp ecx,[PlayerGameData+0x84];
  setg`). Each ring costs 1 (`0x18C92BB`). [C]
* Small Resonant Bell: no Insight to ring, 1 Insight to buy. Being summoned needs no Insight. [C]
* Insight sources that matter at the start [C, EMEVD]:
  * common **9350** "SAN値獲得" (gain Insight): applies SpEffect **4680** N times (N = arg; one 4680 = +1 Insight).
    It ends at once on a client (`1003[6]`).
  * m24_01 **12410995** "診療所に入った時にSAN値を上げる" (raise Insight on entering the clinic): one shot, not on clients.
    Condition: local player on hit **2414110** (MSB collision `h000015`) and not inside region 2412900 → 9350(1).
    The exact trigger spot is [I].
  * boss first-encounter events (e.g. m21 12101802 → 9350(1) guarded by 9343) and Madman's Knowledge.

### 1.3 Doll awakening (gate for the Beckoning Bell and for levelling)

m21_00 **12100110** "人形のヒロイン_立ち位置制御" runs on every Dream load [C]:
```
if (flag 9401 == OFF  ||  Insight == 0)  -> label 9: Doll lifeless (warp 2102304, anim 9011), 12105100 = 1
else                                     -> 12100105 = 1 (Doll awake), 9404 = 1, random stand spot ...
```
So the Doll (and with it the Beckoning Bell messenger) needs the first Dream visit **and** Insight ≥ 1
**at a Dream load**. [C]

### 1.4 Intro / tutorial flags

| Flag / event | Meaning (from EMEVD) | |
|---|---|---|
| **12410000** (m24_01 event) | "キャラメイク後のカットシーン" (cutscene after character creation): plays 24010005/24011005 (by sex), clears 9180, sets respawn **2412959**. Started by the m24_01 constructor only while 12410999 is OFF | [C] |
| **9180** | Set to 1 by the m24_01 preconstructor while 12410000 is OFF; set to 1 then 0 around the cutscenes 21000000 (first Dream), endings, m28 9422. It works as a "scripted cutscene pending/playing" signal | writers [C], meaning [I] |
| 12410999 / 12410998 | "event version" (demo build) branch: 12410998 has no writer in any EMEVD; when ON, 12410999 = 1 and the constructor sets 9400/9401 and skips the opening | [C]; 12410998 = kiosk switch [I] |
| common **9400** | "初回死亡で拠点へ" (first death → base): ends if client, if 12410999 or **9401**. When in m24_01 it sets **9402**; on the player's death it sets respawn **2102962** (first-visit Dream point) | [C] |
| common **7200** (lamp → Dream) | if **9401 OFF**: `2003[49] 2102962` (first-visit Dream) instead of the normal Dream point | [C] |
| m21_00 event/flag **9401** | "初回死亡で拠点に来たポリ劇" (cutscene on first arrival at base): on Dream load, not on clients, if not done: 9180=1, cutscene **21000000**, 9180=0, sets **12417810**; completion = flag 9401 | [C] |
| 12417810 | m24_01 "first lamp lit" (12411010 sets it from 7015 = lamp slot 15 / entity 2410950). Also set by 9401 → the first-floor sickroom lamp is usable after the first Dream | [C]; slot 15 = sickroom lamp, ReturnPoint **2412950** (warpChairNo 18) [I] |
| common **9404** | "SAN値1以上で初回死亡" (first death with Insight ≥ 1): after 9401, on death with Insight ≥ 1 sets respawn 2102961; 12100110 sets 9404 directly | [C] |
| 12101020 / 12101021 | right-hand / left-hand starting weapon messengers done (choice flags 72101000..72101002 / 72101010..72101011 from ESD) | [C] |
| m24_01 12410010/11/12 | "チュートリアル_メッセンジャーが見えるようなる" (tutorial: messengers become visible): hint bloodstains only, nothing multiplayer-related | [C] |
| stage loader 0x19E8190 | reads **12410000** (four sites 0x19E833D/0x19E8524/0x19E8AB9/0x19E942D): special first load of map 0x18010000 (m24_01) while the flag is OFF. Not network related | [C] |

### 1.5 Vanilla timeline

new game → char creation (title menus, no world) → m24_01 clinic, cutscene 12410000 →
(clinic area −241000: **no multiplayer**) → first death (9400 → respawn 2102962) or lamp (7200 → 2102962)
→ first Dream, cutscene 9401 (Doll lifeless while Insight 0; messengers give weapons/Notebook) →
headstone → sickroom lamp 2412950 → Central Yharnam streets (area 2410x0, flag 2410 OFF) → gain
Insight → load the Dream → Doll awake → talk to messenger → Beckoning Bell (+ Small Resonant from the shop) →
**first possible co-op**. Being summoned as a guest only needs the Small Resonant Bell, so it comes no earlier. [C]

---------------------------------------------------------------------------------------------------

## 2. Where multiplayer is forbidden (Q2)

### 2.1 The SOS area value [C]

`0x18BBA70` (PlayerIns: on hit change, `param_2` = the hit the character stands on) reads the MSB
collision part's type data (`part+0xB8`, MSB v3; `+0x70` for v<3) **`+0x10` = play region id** and
stores `PlayerIns+0x278 = id`, `PlayerIns+0x27C = −|id|` (variant chosen by vcall `+0x628`). Every bell
gate reads `+0x278` (or `+0x27C`). The id `AABRxx` decodes as area `AA`, block `B`, region `R` (the
digits used by 0x131D7B0). **Negative id = multiplayer forbidden**:

* Beckoning availability `0x157F8C2 cmp ebx,0xF423F; ja` (unsigned) → greyed out.
* Resonant/responder `0x191A750`: `return (area < 1000000 unsigned) & tableOK` (0x191A8BC/8C3) → refused.
* Active-bell updater `0x1506820` (`0x15068BB`), SOS status producer `0x186FE40` (`0x18700D3`).

### 2.2 The area flag tables (dumped from the eboot) [C]

Index `i = (|area|/10000)%100 − 21` (< 16), block `(|area|/1000)%10` (< 3), region `(|area|/100)%10` (< 4).

* **TBL1 0x47304B0** (read by validator `0x131D7B0`, 14 callers: Beckoning 0x157F90E, updater 0x15068F7,
  SOS status 0x18700CE, summon builder 0x18749E1, responder 0x191A968/0x191AA4C, ...): area blocked when
  `entry > 0 && SprjLuaEventMan-side flag read 0x1399BF0(entry)` returns ON (only if `*0x553B0D8 != 0`).
* **TBL2 0x47307B0** (same validator, OR-ed): all −1 (m36 zeros), so it never blocks.
* **TBL0 0x47301B0** (only the resonant/responder check `0x191A750`): guest side.

| map | TBL1 [blk0 r0..3 / blk1 / blk2] | TBL0 (resonant) | TBL1 flag writers (EMEVD) |
|---|---|---|---|
| m21 Dream | 2100 | 2105 | m21_00 preconstructor sets **2100 = 1** on every load unless 12101802 (Gehrman first fight started, which clears it) |
| m22 | 2200 | 2205 | 12201800 boss defeat |
| m23 | 2300, 2301 | 2305, 2306 | 12301800, 12301700 boss defeats |
| m24 | 2400,2401,2402 / **2410,2411,2412** / 2420,2421 | 2405,2406,2407 / **2415** / 2425,2426 | 2400-2402 m24_00 constructor (state) + 12401800; **2410 = m24_01 12411899 "マルチ可否制御_聖堂街B" (multiplayer on/off control, Cathedral Ward B), set when 12411800 (Gascoigne) AND 12411700 are both done**; 2411 = 12411700, 2412 = 12411800; 2420 = 12421700, 2421 = 12421800 |
| m25 | 2500 | 2505 | 12501800 |
| m26 | 2600, 2601 | 2605, 2606 | 12601850, 12601800 |
| m27 | 2700, 2701 | 2705, 2706 | 12701800 |
| m28 | 2800 | 2805 | 12800400 (time of day, set/cleared), 12801800 |
| m29 (chalice) | 12901800..12901803 | 12901805..808 | chalice scripts (not traced) |
| m30, m31 | none | none | — |
| m32 / m33 | 3200 / 3300 | 3205 / 3305 | 13201800 / 13301800 |
| m34 | 3400..3403 | 3405..3408 | constructor + 13401800/13401850 |
| m35 | 3510..3513 | 3515..3518 | constructor + bosses (13501800/801/850, 13501141) |
| m36 | 3600, 3601 | 3605, 3606 | 13601800 |

TBL0 flags `xxx5..8` have **no EMEVD writer** except 2405/2406 (m24_00 constructor/gate 12400760),
so the guest table almost never blocks. Whether native code writes them is untested [I].
(`docs/party/event_flags.md` calls 2410 "multiplayer allowed". It is the opposite: ON = Beckoning blocked.
"聖堂街B" is the dev name of m24_01.)

### 2.3 Play region ids per map (MSB collisions, `PARTS_PARAM_ST` type 5) [C]

| map | positive (multiplayer by table) | negative (no multiplayer) |
|---|---|---|
| m21_00 Hunter's Dream | 210000 ×6 | −1 ×8 |
| m21_01 Abandoned Old Workshop | — | −211000 ×4, −1 ×2 (no multiplayer at all) |
| m24_01 Central Yharnam | 241010..241080 (all region 0 → flag 2410) | **−241000 ×14 (clinic)**, −241010, −241090, −241109, −241209, −1 ×29 |
| m24_00 | 240000..240160 | −240000, −240010 ×16, −240130, −240169/−240209, −1 |
| m24_02 | 242000..242100 | −242049, −242119, −1 |
| m22 / m23 / m25 / m26 / m27 / m28 | 22xx10.. / 23xx10.. / ... | −220000/−220010/−220059; −230000/−230099/−230119; −250079; −260020/−260049/−260159; −270040/−270060/−270100/−270199; −280000/−280010/−280050/−280099 (+ −1 everywhere) |
| m32..m36 | regular | −320000/−320039/−320040; −330099; −340229/−340309; −350000/−350010/−350209/−350309; −360000/−360030/−360099 |

Campaign-start places:

* **Iosefka's clinic (the intro, m24_01)**: collisions h000010..h000019, h000021, h000023, h000025, h000090 =
  **−241000**. h000015 = entity **2414110**, the "entering the clinic" hit of 12410995, so the clinic interior
  is negative [C]. The other −241000 parts are the surrounding clinic geometry [I]. The first-floor sickroom lamp
  (2412950) is inside it [I].
* **First Hunter's Dream visit / Doll awakening**: the same m21_00 map, area **210000**, blocked by 2100
  (always ON outside the Gehrman fight). There is no separate "first visit" map; the first visit only differs
  by flag 9401 and the respawn point 2102962 [C]. The Doll awakening is flag logic, not an area [C].
* Small ids −241109 / −241209 / −241090 (entities 2414400/401, 2414122/124, 2414120/123) are probably
  boss arenas or one-way areas [I]; −1 parts are non-walkable/out-of-bounds hits [I].

---------------------------------------------------------------------------------------------------

## 3. Guests and hosts still in the intro (Q3)

| State of a player | What happens | |
|---|---|---|
| Title / character creation menu | No world, `WorldChrMan+0x60 == 0`, no Lua event context. Nobody can ring or be summoned. Cooperator count 0x15BDC20 asserts without a world (A5) | [C] |
| m24_01 load, opening cutscene (12410000 OFF, 9180 ON) | Cutscene `2002[3]` play mode 8. The director must not dispatch bell Lua events here | gating [C], the effect of ringing [I] |
| In the clinic after the cutscene (area −241000) | Vanilla: host Beckoning greyed out, guest resonant refused, SOS status restricted. With `Bells anywhere` these pass (`0x157F8C8 jg`, `0x191A8C3 jge`, `0x15068BB`, `0x18700D3`) | [C] code, runtime [I] |
| Host in clinic with guests present | Host-side tutorial events still run. 9400/9404/7200/9350/12410995/9401 all start with `1003[6]` "end if client", so they never run on guests. Host first death → `SoloPlayDeath` → room ends, guests sent home (`HostDead_1`), host respawns at the **first-visit Dream** 2102962, a single-player cutscene | [C] |
| Guest pre-Dream (9401 OFF) summoned | Works the same as any guest: the guest's own world flags are untouched while it plays on the host's overlay. Its character has **no weapons** (starting weapons only come from the Dream messengers 12101020/21). Sent home → own clinic at the pre-summon spot. A lamp warp at home with 9401 OFF goes to 2102962 | [C] flags, gameplay [I] |
| Guest follows the host (B1 `0x13CDF30(id)`) into the Dream with 9401 OFF | 0x13CDF30 bypasses 7200's 9401 test. In the Dream, 9401 plays the first-Dream cutscene on load (not a client then), then normal play | [C] events, runtime [I] |
| Lot awards while a guest | `0x17DDC50` (EMEVD `2003[4]`) branches on role 6 / `+0x16F8[+0x98]` ⇒ grant items only in the player's own world | [C] branch, semantics [I] |

Both saves must be in the following state for a summon [C unless marked]:

1. Online: `FrpgNetMan+0xA = 1`, NP/FROM login done (`Party: Skip Online/Offline Choice (Online)`).
2. In the world: `WorldChrMan+0x60 != 0`, NowLoading `0x556286B == 0`, `GSM+0x08 == 0`, 9180 OFF, roles
   (`SprjSessionManager+0x124`): guest 0, host 0/3.
3. Guest: SendSign gates `0x1901566` (`+0x16F8 table [+0x14] ≤ 1`) and `0x1901585` (session member list empty);
   PlayerIns `+0x78` own body (type 0).
4. Neither side checks level, NG cycle or area on the client when taking a sign. Matching is server-side and done by our
   party server (from_api_schema §5; level window LowerAbs −1000).
5. Recommended (not required by code): each player has 12410000 ON. For a useful guest, also 9401 ON and
   12101020/21 ON (armed).

---------------------------------------------------------------------------------------------------

## 4. Design (Q4)

### 4.1 Flow ("party lobby → parallel prologue → summon at the first lamp")

1. **Lobby (title)**: everyone connects with the party code. PartyLink roster state `TITLE`.
2. **Character creation**: each player picks New Game and creates their own character in their own save.
   Roster state `CHARMAKE` (no world). No director action.
3. **Prologue (parallel, solo)**: state `PROLOGUE` while `12410000 OFF || 9401 OFF || 12101020 OFF`.
   Each player wakes in the clinic, dies or uses the lamp, sees the first Dream, takes the starting weapons and
   uses the headstone. The overlay shows each member's step (flags below). The director rings no bells.
4. **Ready**: `12410000 && 9401 && 12101020 && 12101021`, in the world, not loading, 9180 OFF. Optional grants
   (§4.3) run here once, in the player's own world (role 0).
5. **Gather**: the host director picks the meeting point. Default: the host's current spot. If the host is in the
   Dream, B1 travel to the sickroom lamp **2412950** (or stay in the Dream with the Dream patch).
6. **Summon**: host rings Beckoning (`OnEvent_Call_SOS`), guests ring Small Resonant
   (`OnEvent_SendSoulSign_NormalCoop`) via `LuaEvent_DispatchByName 0x1339870`, as in A5. From here A6/B1
   take over (auto-rejoin, travel follow).
7. A guest that drops back (crash, death as phantom) re-enters at step 6. A guest that never reached "ready"
   but should join anyway uses the experimental mode (§4.5).

Flags read through `GetEventFlagValue 0x13CFD80(EventFlagMan *0x553B100, flag, 1)` on the main-thread tick:
`12410000, 9180, 9401, 9402, 12101020, 12101021, 12417810, 12100105, 6622, 6610`, plus Insight
`PlayerGameData+0x84`, map via `PlayerIns_GetBloodMarkMap 0x19046C0`, and the SOS area `PlayerIns+0x278`
(diagnostics: negative = vanilla multiplayer-forbidden spot).

### 4.2 Byte patches

Existing (verified again here against the eboot, bytes at file offset VA+0x4000) [C]:

| Patch (XML name) | Sites (ours) | Needed for the start because |
|---|---|---|
| Party: Bells anywhere | 0x157F8C8 `0f8790000000→0f8f90000000`, 0x157F960 `3401→31c0`, 0x157F6D1 `88c3→b101`, 0x157F6D3 `4c89f7e8f59f3400→e9e9090000909090`, 0x15068BB `774a→7f4a`, 0x15068FC `88c380f301→b301909090`, 0x191A8C3 `18c920c1eb02→7d0488c1eb02`, 0x18700D3 `84c041bdffffffff440f44eb41c1ed1f→4531ed`+13×`90` | clinic −241000, workshop −211000, Dream 2100 (Beckoning result 0x157F960) |
| Party: Bells without Insight | 0x157FA5C `0f9fc0→30c090`, 0x18C92BB `410fb64738f7d8→31c00f1f440000` | Insight 0 before the first boss |
| Party: Bells after boss defeated | 0x18749E8, 0x18749F0 (6×`90`), 0x14B714A (6×`90`), 0x1874B79 `740d→eb0d` | 2410 later; cross-area candidates |
| Party: Skip Online/Offline Choice (Online) | 0x1B39030 | online mode (goods 205 online-only) |

**New, proposed** (data, byte-verified):

| Name | Our offset | XML Address | Original | Value | Effect |
|---|---|---|---|---|---|
| Party: Hunter's Dream multiplayer | **0x47304B0** (file 0x47344B0) | 0x4B304B0 | `34 08 00 00` (2100) | `FF FF FF FF` | TBL1 m21 blk0 r0 → −1 (`< 1` → never blocked). Removes the Dream lock for all 14 callers of 0x131D7B0 (Beckoning, SOS status, builder, responder), including the first visit. Not needed while `Bells anywhere` + `after boss` are on for Beckoning/builder, but it also covers the remaining callers (0x18C5360, 0x1FEB60C/0x1FEBCFC/0x1FEC37C, 0x18CF975, 0x1AC6543/0x1AC65F4, 0x131D913). [C] bytes; runtime [I] |

TBL0 (0x47301B0, m21 = `39 08 00 00` 2105) needs no patch: 2105 has no writer. The guest bell in the Dream
fails for another reason (travel.md §2 item 2, effect mask), which `Bells anywhere` 0x157F6D1/6D3 already
covers. [C]/[I]

No patch is needed for the clinic beyond `Bells anywhere`: the negative id is the only gate there. The
per-collision bit checked by `0x18C96D0` (collision type data `+0xA` bit 1) is clear in every MSB. [C]

### 4.3 Granting the bells (manual use, and in case PR1 shows ownership is required)

Only in the player's own world (role 0, not loading, main thread):

* **Beckoning Bell**: `0x17DDC50(rdi = *(u64*)0x553D6E0, esi = 10010, edx = 1)`, the same call EMEVD `2003[4]`
  makes (`0x17C17F8..0x17C1806`; `2003[36]` passes `edx = 0`). The lot also sets **6622**, so the messenger
  event 12101024 skips and the Small Resonant Bell appears in the Insight shop. [C] call shape; popup and
  flag side effects [I]
* **Small Resonant Bell**: `0x131CB70(rdi = unused, esi = 0x40000000 /*goods*/, edx = 205, ecx = 1)`, the
  native of EMEVD `2003[43]` "アイテム直接入手" (give item directly; call at 0x17C2ACB: `esi=r15d` type bits
  {0:weapon 0, 1:0x10000000, 2:0x20000000, 3:0x40000000}, `edx` item id, `ecx` count). Then
  set flag 6610 ON (SetEventFlag 0x13CFCC0, with the same calling convention the C2 flag sync uses) so the shop does not offer it again (`isOnlyOne` anyway). [C] call shape;
  count semantics [I]
* Lot 9906791 (category 8, goods 205) exists, but category 8 is not understood. Avoid it. [I]
* The Old Hunter Bell (4312, lot 10050) is not needed.
* **Do not** grant Insight by default. If wanted, SpEffect **4680** = +1 Insight (common 9350); 1 Insight
  also wakes the Doll at the next Dream load (needs 9401).
* Starting weapons are not granted: the prologue gives them. For §4.5, `0x131CB70(0, 0 /*weapon*/, id, 1)`.

### 4.4 Director sequencing (state machine additions to A5)

```
TITLE ──new game──▶ CHARMAKE ──WorldChrMan+0x60──▶ PROLOGUE
PROLOGUE: wait (12410000 && 9180==0) → wait 9401 → wait 12101020 && 12101021 → READY
READY (own world, role 0): one-shot grants (§4.3, if enabled) → announce READY on PartyLink (PROGRESS)
HOST: when host READY and ≥1 guest READY → optional B1 TRAVEL to 2412950 (or stay) → ring loop (A5)
GUEST: when READY and host announced "gathered" → ring loop (A5); on send-home → back to ring loop
```
Ring preconditions (all [C] from §3): in world, NowLoading 0, `GSM+0x08 == 0`, 9180 OFF, role guest 0 /
host 0|3, not during `MoveMapStep`. The host should also not ring while its own 9401 is OFF (the first death
ends the session and leads into a single-player cutscene).

### 4.5 Experimental: summon from the clinic (right after character creation)

Possible with the patches in §4.2 (negative areas pass). Gate: both 12410000 ON and 9180 OFF. Costs:

* Guests are unarmed (fists) until their own first Dream. Mitigation: grant a starting weapon through
  `0x131CB70`, which changes the vanilla start.
* The host's scripted first death ends the session and sends the host to the first-visit Dream (single
  player). With the Dream patch, guests can be re-summoned there after the 9401 cutscene ends.
* A guest's own save stays pre-Dream (9401 OFF) until C2 syncs prologue flags or the guest dies/lamps at
  home. Minimal C2 "prologue set" proposal: 12410000, 9402, 9401, 12417810 (first lamp lit), set-only,
  applied at a load boundary in the guest's own world. Weapons messengers do not depend on 9401, so the guest
  can still collect them later. [C] dependency, policy [I]

### 4.6 Save state summary

Every player keeps a normal own save (no shared world). Start-related differences between saves never block a
summon. They only decide what the guest finds at home: the opening cutscene, the first-Dream cutscene, Doll
state and weapons. Keep the C5 rule: party saves separate from solo saves.

---------------------------------------------------------------------------------------------------

## 5. Corrections for other docs

* npc_peer.md: goods **4312 = Old Hunter Bell** (NPC summon), not the Beckoning Bell (200).
* event_flags.md §7: 2410 is set by 12411899 when **both** 12411800 and 12411700 are done, and ON **blocks**
  the host bell (validator 0x131D7B0 → 1). It does not "allow" multiplayer.
* travel.md §2: the validator also needs `*0x553B0D8 != 0` before reading the flag. The resonant/responder
  gate 0x191A750 uses its **own table 0x47301B0** (xxx5 flags), not 0x47304B0.

## 6. Runtime probes

* **PR1** Fresh save, Central Yharnam, patches on, no goods 200/205: director Lua bells on both sides →
  summon succeeds? (decides whether §4.3 is needed).
* **PR2** Host in the clinic (log `PlayerIns+0x278`, expect −241000), guest in Central Yharnam (2410x0):
  summon with `Bells anywhere`; then the host dies to the scripted werewolf → guests sent home, host in the
  first Dream.
* **PR3** `0x17DDC50(*0x553D6E0, 10010, 1)`: item popup, flag 6622, no duplicate from the messenger;
  `0x131CB70(0, 0x40000000, 205, 1)`: one bell, inventory intact.
* **PR4** Dream patch: Beckoning in m21_00 at Insight 0 (first visit, after 9401) and a guest's resonant
  sign in the Dream.
* **PR5** Guest with 9401 OFF follows a host lamp warp to the Dream via 0x13CDF30: cutscene 21000000 plays,
  no softlock, the guest can ring again afterwards.
* **PR6** 9180 timing: ON from the m24_01 preconstructor until the end of 24010005; OFF in normal play.

## 7. Implementation and probe results (C1 wiring)

* `gpu/shim/party/party_start.{h,cpp}` (pure logic, `party-start-test`): `BB_PARTY_START=prologue_solo`
  (default: ready at 12410000 && 9401 && 12101020 && 12101021 && 9180 OFF) | `immediate` (ready at
  12410000 && 9180 OFF). A member in its own world that is not ready reports roster state **Prologue**
  (new `MemberState` 6), so the host never rings for it and it never rings itself.
* `game_state`: `ReadEventFlag` (0x13CFD80), `WriteEventFlag` (0x13CFCC0), `GoodsCount` (0x14D9E80 lookup on
  PlayerGameData+0x328, count at entry+8), `AwardItemLot` (0x17DDC50(*0x553D6E0, lot, **0**): the EMEVD
  call at 0x17C1806 passes edx 0, not 1; edx 1 = only when the role table allows), `GiveGoods` (0x131CB70).
  Every function's first bytes are compared with 1.09 before the first call.
* Director: `BB_PARTY_GRANT_BELLS` (default on in party mode): once ready, in its own world (role 0), 5 s
  after the world is up: lot 10010 if goods 200 is missing (goods 200 directly when 6622 is already ON),
  goods 205 + flag 6610 if 205 is missing. Readiness and lobby state go to the party status board.
* Runtime (1.09 save copy, level 37, Hunter's Dream): flags read correctly (all prologue flags 1, 9180 0 →
  READY); inventory read goods 200 x1, 205 x0 (6610 0). Grant: goods 205 x0 → x1, 6610 → 1 [C].
  `award_lot=10010` on a character that already holds the bell: call returns, no crash, count stays 1
  (unique item) — the award itself on a character without the bell is still [I]. `GiveItemDirect` with a
  negative count does **not** remove goods 200 [C].
* **PR1 not decided at runtime**: the Lua `OnEvent_SendSoulSign_NormalCoop` ran (dispatcher 1) on a
  character without goods 205, but the game was offline (FrpgNetMan+0xa 1 → 0 during the title: the FROM
  login never ran after ss.info), so no `summon_messenger/create` could follow either way. Static: SendSign
  0x1901320 reaches none of 0x14D9E80 / 0x157F200 / 0x131CB70 / 0x14D0BE0 within 7 levels of direct calls
  (virtual calls not followed) [I]. Grants stay on by default, so ownership does not matter for the party.
