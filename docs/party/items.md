# Party items and rewards for guests (Phase C3)

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA of `smoketest/out/eboot.elf`;
bbhost source = ours + 0x400000). Decompiles: Ghidra (`tools/re/decomp.py`); disassembly and byte checks:
capstone over the ELF. Data: `dvdroot_ps4/param/gameparam` (ItemLotParam, EquipParamGoods/Weapon/Protector,
SpEffectParam, GameAreaParam), every EMEVD + EMELD (FROM's event names), `msg/engus/item.msgbnd` (names),
read with `tools/party/flag_tool.py` helpers.

Markers: **[static]** proven from code/data here, **[inferred]** from names/shape, needs one runtime check
(probes in section 8).

"Guest" = a summoned white phantom in the host's world. In code that state is
`isClient = (SprjSessionManager+0x124 == 6) || (NetworkFlow slots(+0x16F8)+0x98 != -1)`, usually combined with
`GSM(0x5556678)+0x1592 == 0` ("I am in someone else's world", see travel.md 1.0).

## 0. Short answers

| Question | Answer |
|---|---|
| Can a guest pick up world treasure (corpse loot)? | **No, it is never created on a guest.** MSB treasure spawn `0x17DC300` returns at `0x17DC3D6` (role 6) / `0x17DC3E3` (slot +0x98 != -1). [static] |
| Enemy drops? | Yes, rolled locally on the guest's machine, but on a client only items with `isGuestDrop = 1` survive: in the data that is only **Blood Vial (1000)** and **Quicksilver Bullets (900)**. Filter at `0x17DD75D`. [static] |
| Boss drops / key items / remembrance-type items (Oedon Tomb Key, Gold Pendant, badges, chalices, umbilical cords, keys)? | **No.** Boss events award with EMEVD `2003[4]`, which a client skips twice: the script jumps past it (`1003[105]` "jump if client") and `0x17DDC50` refuses `hostOnly` lots on a client (`0x17DDCD9..0x17DDD3E`). Keys that are world treasure (Hunter Chief Emblem, Orphanage Key, Iron Door Key, Lunarium Key, Upper Cathedral Key and so on) are not spawned on the guest. [static] |
| NPC quest rewards? | No for EMEVD gifts (`common 9040/9100/9110`, `2003[4]`, host only). Talk-script (ESD) gifts were not analysed. [static for EMEVD] |
| Blood Echoes | Enemy kills: **50 %** for a client (`0x18FC93D`). Boss clear bonus (GameAreaParam): **50 %** for a phantom (`0x1335578..0x1335591`). Black/other phantoms (chr type 2) get no kill echoes (`0x18C5179`). [static] |
| Insight | A white phantom gets **+1 Insight** when the host's boss dies: `BlockClear2` applies SpEffect **4680** (`heroPointDamage = -1`) at `0x13852B1`. The host pays 1 Insight per Beckoning Bell (from_api_schema.md 8). [static; that -1 means +1 is inferred from the name] |
| Bell-related items | Covenant/Vermin lots in `BlockClear2` (5900/5920/5930 = Vermin, 5910/5940/5950 empty) are given with `hostOnly = 0`, so clients get them too. Bells themselves come from per-player Messenger gifts (lot 10010, flag 6622, personal block 6000-8999). [static] |
| Shops, Doll, level-up | Not while summoned. Phantoms cannot use lamps (lamp gate `0x12F8315`, travel.md), and the Dream is host-only for clients (m21 `1003[6]` guards). Shop stock flags are per-player (6000-8999, kept in the own pool in load mode 1). Level, echoes, insight and inventory live in the guest's own `PlayerGameData` (`[[0x553B130]+8]`), so they persist [inferred: not overlaid like flags]. |

## 1. Globals and functions

| Symbol | Address | Notes |
|---|---|---|
| MapItemMan slot | `0x553D6E0` | `*(u64*)slot` is passed as `rdi` to every lot function (`0x17C17F8`, `0x132E484`) |
| SprjGaitem slot | `0x553E990` | instance handles for weapons/armour/gems (`0x1A876D0`, `0x1A88130`) |
| GameDataMan slot | `0x553B130` | `+8` PlayerGameData: `+0x70` stat fed to the roll as a float (luck-like, [inferred]), `+0x84` Insight, `+0x90` level, `+0x94` echoes (clamped to 999,999,999 in `0x131F980`), `+0xA4` remembered chr type, `+0xF8` float boss-clear-bonus accumulator, `+0x328` inventory |
| Chr-type capability table | `0x47348A0` | 14 × 12 bytes indexed by `PlayerGameData+0xA4`; byte **[2]** = "may receive item lots" (types 0, 1, 8 = 1; 2, 3, 4, 5…13 = 0). Users of byte [2]: only `0x17DC300`, `0x17DD0B0`, `0x17DDC50` [static] |
| ItemLot roll | `0x17CEAF0(LotReq*, char checkGlobal, ctx)` | reads ItemLotParam, skips items whose flag is set, rolls with the game RNG (`GSM+0xA78/+0xA80`) |
| Lot result builder | `0x1A8B080(LotReq*)` | the plan's "MapItemMan lot→items": calls the roll, then builds `{flag, rarity, n, {gaitemHandle, count}×n}` records (0x38 each) at `LotReq+0x88`, count at `+0x80`. It does **not** give anything |
| **Award lot to inventory** | **`0x17DDC50(MapItemMan*, int lot, char hostOnly)`** | the give function (section 2.3) |
| Give item list | `0x17D89F0(MapItemMan*, ItemList*)` | inventory add, overflow to storage `0x14D9A80/0x14DA0A0` or dropped on the ground `0x17DA960`, "item get" popup `0x13107B0(1, …)` |
| MSB treasure spawn | `0x17DC300(MapItemMan*, entityHandle, MsbEvent*, pos*, …)` | only for MSB event type 4 (`[rdx+0xC] == 4`, `0x17DC32B`) [inferred: 4 = Treasure as in the DS1 MSB] |
| Enemy-death drop | `0x17DD0B0(MapItemMan*, chrHandle, npc*)` | from `0x18C4F90` (chr death) |
| Hawk-girl drop (EMEVD 2003[34], DS leftover) | `0x17DB8C0` | spawns a lot as a map item |
| Pick up map item | `0x17D8570(MapItemMan*, &handle)` | from the action-button handler `0x12FD880` (vtable slot `0x531CC70`) |
| Remove map item (+ set its pickup flag) | `0x17D6780(MapItemMan*, &handle, char setFlag)` | |
| Echoes for a kill | `0x18C5070` → `0x18FC860(float soul, PlayerIns*, enemy, bool bonus, bool isClient)` | pays through `PlayerIns->vtbl+0x3D0 (AddSoul)` |
| Boss clear bonus | `0x1335400` (Lua "SetClearBonus") fills `+0xF8`; `0x13356C0` (Lua `AddBlockClearBonus`) pays it | |
| Lua `BlockClear2` (boss killed, every player) | `0x13849A0` | then `BlockClear2_1 0x1385470` (guests sent home, travel.md) |
| Lua `GetRateItem` / `GetRateItem_IgnoreMultiPlay` | `0x132E480` (hostOnly = 1) / `0x132E4A0` (hostOnly = 0) | both `→ 0x17DDC50` |

Lua names come from the registration code (`lea rax, fn` followed by the name `lea rdx`): `0x133D337`
(GetRateItem), `0x133D37D`, `0x138E729` (BlockClear2), `0x13421F2` (AddBlockClearBonus).

## 2. From a lot id to the inventory

### 2.1 ItemLotParam row (BB layout, matches the code in 0x17CEAF0) [static]

| Offset | Field |
|---|---|
| `+0x00..0x1F` | `lotItemId01..08` (s32) |
| `+0x20..0x3F` | `lotItemCategory01..08` (0 weapon, 1 protector, 2 accessory/rune, 4 goods, 8 gem) |
| `+0x40..0x4F` | `lotItemBasePoint01..08` (u16 weights) |
| `+0x50..0x5F` | `cumulateLotPoint01..08` |
| `+0x60..0x7F` | `getItemFlagId01..08` (0 = use the lot-wide flag) |
| `+0x80` | **`getItemFlagId`** (lot-wide pickup flag) |
| `+0x84 / +0x88` | `cumulateNumFlagId` / `cumulateNumMax` (drop-counter value) |
| `+0x89` | rarity |
| `+0x8A..0x91` | `lotItemNum01..08` (u8 counts) |
| `+0x92` | u16 bits: `enableLuck01..08` (bits 0-7), `cumulateReset01..08` (bits 8-15) |

**Chains:** the roll keeps going with `lotId+1, +2…` while rows exist (`0x17CEC00`), so a set such as
2110010/2110011 (Gehrman's cap and garb) is one award. **Flag mapping:** item flag =
`getItemFlagIdNN != 0 ? getItemFlagIdNN : getItemFlagId`, plus the request's byte `LotReq+9` (0 for awards,
an NPC-param offset for enemy drops). In the shipped data `getItemFlagId01..08` is always 0, so **the
lot-wide `getItemFlagId` is the flag**. Treasure lots follow `flag = 50,000,000 + lotId` (784 lots; chains
share the first row's flag; all 811 treasure rows are deterministic: one item per row with weight 100).
Overall 1,377 distinct flags over 7,187 lots that have a flag; 50 flags are shared by alternative lots (NPC
NG+ variants such as 34000/34030/34035 → 50001800), so **flag → lot is not unique** outside treasure.
Lots with `getItemFlagId = -1` (boss drops such as Oedon Tomb Key 31000 and Gold Pendant 50000001) have no
pickup flag at all; vanilla relies on the awarding event's own completion flag.

### 2.2 LotReq (stack struct built by every caller) [static]

`+0x00 lotId`, `+0x04 float` (`0x1581830(PlayerGameData+0x70 as float, +0x670)`), `+0x08 u8 checkFlags = 1`,
`+0x09 u8 flagOffset`, `+0x0A u8 rank`, `+0x0C` out count / `+0x10` out records (0x50 each:
`+0 flag, +4 rarity, +5 n, {+8 itemId, +0xC category, +0x10 count, +0x11 slot} × n`), `+0x20..` area
lot-modifier data (`0x553B148` table), `+0x80/+0x88` resolved records (see 1). Freed with `0x1A8AF00`.

The roll (`0x17CEAF0`) has one global gate: if byte `0x553D6D8 != 0` and `checkGlobal == 1` it produces
nothing (meaning unknown; probably a debug switch).

### 2.3 `0x17DDC50(MapItemMan*, int lot, char hostOnly)` — the award [static]

```c
pgd = GameDataMan->+8; if (!pgd) return;
st  = GSM->+0x16F8;
if ((role == 6 || st->+0x98 != -1) && GSM->+0x1592 == 0)  allowed = 0;     // 0x17DDCD9/0x17DDCE2/0x17DDCF4
else allowed = table_0x47348A0[pgd->+0xA4].byte[2];                          // 0x17DDD21
if (!allowed && hostOnly) return;                                            // 0x17DDD3A xor r14b,1 ; 0x17DDD3E je
LotReq r = {lot, luck, checkFlags=1, …};  0x1A8B080(&r);
for each resolved record:
    list = {n, {gaitem, itemId|category<<28, count, -1} × n};
    0x17D89F0(MapItemMan, &list);                       // inventory + popup
    if (record.flag >= 0) tree_bit(record.flag) |= 1;   // direct OR into the *current* flag tree, no SetEventFlag
0x1A8AF00(&r);
```
Callers: EMEVD `2003[4] アイテム取得` (`0x17C17F8`, hostOnly = 1); EMEVD `2003[36]
クライアントでもアイテム取得` "award even on a client" (hostOnly = 0); Lua `GetRateItem` (1) /
`GetRateItem_IgnoreMultiPlay` (0); `DeadInfoMsg_Host 0x1382A50` (lots 5510/5610/5710/5810, 0),
`DeadInfoMsg_ForceJoinBlack 0x1382ED0` (5500/5600/5700/5800, 1), `TextEffectEnd_PK_Success 0x1383AF0`
(16581, 1), `TextEffectEnd_NPCPK_Success 0x1383BD0` (5020, 0; 16581, 1), `BlockClear2 0x13849A0`
(5910/5930/5940/5950, 0).

Note: the flag is written into whatever tree is live. On a guest in load mode 1 that is the host-world
overlay, which is thrown away on return (event_flags.md 3); the item stays. That is how vanilla
`2003[36]` awards (Bloodied Arm Bands and so on) can be obtained twice.

### 2.4 Map items (treasure, enemy drops, shared drops)

* Record kinds (`+0x20 & 0xF`): 1 hawk drop, 2 enemy drop (`0x17D5320`), 3 treasure / object (`0x17D4FB0`);
  `+0x24` Lua event id, `+0x28` pickup flag, `+0x48` u16 flags (`0x40` takeable, `0x80` network-shared),
  `+0x54` n items, entries from `+0x58` (16 bytes each).
* **Treasure** (`0x17DC300`): gate `0x17DC3CF cmp [role],6 / 0x17DC3D6 je`, `0x17DC3DC cmp [st+0x98],-1 /
  0x17DC3E3 jne`, then chr-type byte [2] (`0x17DC411`). A guest never gets the object, so it cannot loot it.
* **Pickup** `0x17D8570`: unshared item → `0x17D6780(…, 1)` (remove; sets `+0x28` flag and fires the `+0x24`
  Lua event only when `!isClient || GSM+0x1592 != 0`, `0x17D6818..0x17D6831`) then `0x17D89F0` (give).
  Shared item (`0x80`) on a client (`0x17D85F4`) → packet `0x2C {1, 5, handle}` to the host via
  `0x1789A40(*0x553D6D0, 0x2C, buf, 8)`; on the host `0x17DE6E0` + give unless role 6 (`0x17D88BB`). Spawning a
  shared item on a client goes the same way (`0x17D8370`, packet 0x2C subtype 3). [static; the host's reply
  path for 0x2C is not traced]
* **Enemy drops** (`0x17DD0B0`): byte [2] gate only (`0x17DD1C3`; types 0/1/8 pass), then
  `isClient` is stored at `0x17DD193` and used once at `0x17DD75D`: a client keeps an item only if its
  equip param has `isGuestDrop` (goods byte 0x47 bit 3, weapon 0x104 bit 4, protector 0xDF bit 0; gems
  0x8C bit 3). Only goods 900 and 1000 have it.

## 3. Phantom/host checks found (one table)

| Site | Check | Effect for a guest |
|---|---|---|
| EMEVD scripts | `1003[105] (label 1, state 1 = client)` before boss awards; `1003[6] (end, client)` in many NPC/treasure events | event skips the award on clients |
| `0x17DDCD9..0x17DDD3E` | isClient && in host world → hostOnly lots refused | no `2003[4]` / `GetRateItem` lots |
| `0x17DC3CF..0x17DC418` | role 6 / slot +0x98 / chr-type byte [2] | no MSB treasure objects |
| `0x17DD186..0x17DD193`, `0x17DD75D` | isClient → `isGuestDrop` filter | only vials/bullets from enemies |
| `0x17D6818..0x17D6831` | isClient && not own world → no pickup flag / Lua event | (unreachable for treasure on a guest) |
| `0x17D85F4`, `0x17D88BB`, `0x17D8370` | role 6 → shared items go through the host (packet 0x2C) | |
| `0x18C5179` | `PlayerIns+0x78 == 2` (invader) → no kill echoes | |
| `0x18C5216/0x18C5220` → `0x18FC93D` | isClient → kill echoes × 0.5 | |
| `0x1335578..0x1335591` | session has members and own chr type not 0/8 → boss clear bonus × 0.5 (`0x4925B1C` = 0.5) | |
| `0x13849A0` (BlockClear2) | branches on `PlayerIns+0x78`: 0 host, 8 own-world grey, 1 white phantom, 2/0xC invaders (leave) | type 1: bonus paid, SpEffect 4680, flag 6006 cleared, Vermin lots |

## 4. What a guest gets in vanilla (summary)

| Source | Host | White phantom (guest) |
|---|---|---|
| Enemy kill echoes | 100 % | 50 % |
| Enemy item drops | full lot | Blood Vials / Quicksilver Bullets only |
| World treasure | yes | not spawned |
| Boss kill: echoes from the boss's NpcParam | 100 % | 50 % |
| Boss kill: GameAreaParam clear bonus (`bonusSoul_*`, for example Gascoigne 2410800 → 4000) | 100 % | 50 % |
| Boss kill: Insight | (host's own boss scripts) | +1 (SpEffect 4680) |
| Boss kill: item lots (keys, badges, chalices, Madman's Knowledge, cords) | yes | none |
| NPC gifts (EMEVD) | yes | none |
| Covenant/Vermin lots from BlockClear2 / PK texts | per rules | yes (hostOnly 0) |
| Flags of anything above | own save | overlay only (lost) |

## 5. Echoes, Insight, levels (Q4)

* Kill echoes: `echoes = soul × (isClient ? 0.5 : 1.0) × (npcFlag ? 1.2 : 1.0) × Π SpEffect soul rates`,
  rounded (constants `0x4927EBC = 1.0`, `0x4927EC0 = 0.5`, `0x4927EC4 = 1.2`) [static].
* Boss bonus: `0x1335400` reads the first u32 of the area row (`0x1F1B4C0`, GameAreaParam `bonusSoul_single`
  [inferred field]), adds the boss's NpcParam soul when the boss has the bonus bit, halves it for phantoms,
  applies SpEffect soul rates, stores it in `PlayerGameData+0xF8`; `BlockClear2` pays it via `AddSoul`
  (`0x13850A0` own world, `0x138525D` phantom).
* Insight: phantom +1 at `0x138528E mov esi,0x1248` / `0x13852B1 call [vtbl+0x3F0]` (SpEffect 4680:
  `heroPointDamage = -1`, `stateInfo = 119`). Bells: Beckoning costs the host 1 (EquipParamGoods 200 +0x38).
* Levels: only at the Doll in one's own Dream; a guest levels between sessions (B1 sends guests to their own
  world on every travel). Nothing for C3 to do.
* Optional parity patches (all byte-verified, only for a "guests earn like the host" party option):

| Patch | Address | Original | Replacement | Effect |
|---|---|---|---|---|
| Full kill echoes for clients | `0x18FC93F` | `75 06` (jne → 0.5) | `90 90` | multiplier always 1.0 |
| Full boss clear bonus for phantoms | `0x1335582` | `74 12` (je skip-halving) | `EB 12` | never halves |
| Full enemy drops for clients | `0x17DD191` | `08 C1` (or cl,al) | `30 C9` (xor cl,cl) | isClient = 0 for the guest-drop filter only (the byte at `[rbp-0x208]` has one reader, `0x17DD75D`) |

## 6. C3 design: host-authoritative lot replay

Goal: every member's own save receives every item lot the host's world hands out (key items, boss drops,
treasure the host picks up, NPC gifts), exactly once, even across crashes and rejoins.

### 6.1 What triggers a replay (host side)

1. **Flagged lots via the C2 flag diff** (covers treasure, keys, chalices, cords, NPC gifts, and ESD gifts
   that were not analysed): every host flag that flips 0→1 in category `item_lot_picked`
   (party_flags.inc; T5 `5AABZnnn` and global `5000xxxx`) is mapped to its lot:
   `lot = flag - 50,000,000` when `ItemLotParam[lot].getItemFlagId == flag` (all treasure), otherwise the
   generated reverse table (`flag → [lots]`; for the 50 shared flags take the lot the hook in 2 captured).
   **C2 must not sync these flags by itself** (syncing without the item loses it, event_flags.md 9); C3 sets
   them by giving.
2. **Hook the entry of `0x17DDC50`** on the host (cheap: all EMEVD/Lua awards): record `(lot, hostOnly)`.
   Broadcast only `hostOnly == 1` lots (clients already get the `0` ones). This is the only source for
   flagless lots (`getItemFlagId == -1`): Oedon Tomb Key 31000, Gold Pendant 50000001, Old Hunter Badge
   15000, Spark/Sword Hunter Badges 50800000/50000010, Mensis Cage 21000, Madman's Knowledge 75002405,
   25700005, 3401802, 3401852, Bold Hunter's Marks 15005/50800005, Bloodshot Eyeballs 21002950, Yellow
   Backbone 50700000, Kin Coldblood 51001900, 25700000/75002400 (item names missing from the FMG), 22502610,
   Tear Stone 14000 (and 9040 NPC slots with flag -1).
3. Deny list (never replay; per-player or repeatable): lots whose flag is in 6000-8999 (Messenger gifts
   10000/10010/10040/10050, DLC/costume 6670-6678), covenant gems `common 9440`, rune/gem use `9500`,
   repeatables `common 9100` / `13501940` (何度でも), DLC messenger hats `12105064`, PvP/covenant lots
   5500-5950, 16581, 5020, chalice-dungeon lots (area 29, flag -1 boss lots 1100xxxxx).

Message: `PARTY_CMD ITEM_LOT {seq, lot, flag (-1 if none), ledgerIdx (-1 if flagged), source}`.

### 6.2 Duplicate prevention

* Flagged lots: the lot's own flag in the guest's **saved pool** is the "already given" bit. The roll
  itself skips set flags (`0x17CEAF0`), so a replay of a lot the guest already has gives nothing.
* Flagless lots: a C3 **ledger** of event flags `60009000 + idx` (T6 global zone 9). That group is in the
  saved pool (key `3 + 9×5 = 48`), not in the vanilla join snapshot, and no EMEVD/param references any id in
  60000000-60009999 [static over all EMEVD + params; ESD unchecked]. `idx` = position in a generated
  table of the flagless host-only lots in 6.1 item 2 (about 30 entries). Set the bit right after the give.
* Host state for catch-up: the host derives its own "awarded" set from its save (pickup flags + the
  completion flags of the awarding events, for example 12411800 for lot 31000). On every (re)join the host
  sends the full set; the guest applies what it lacks. Live messages are only an optimisation; resending is
  always safe.
* Optional symmetry: members' own solo pickups can be pushed to the others the same way (union policy).

### 6.3 Applying on a guest (main thread)

Run from the A5 tick (`SprjFlipper::Update 0x2034770`). Apply only when **in the own world, load mode 0**:
`*(u32*)(EventFlagMan+0x80) == 0`, `GSM+0x1592 == 1`, `SprjSessionManager+0x124` is 0 or 3 (not 4/6/7),
`WorldChrMan(0x553E878)+0x60 != 0`, `GameDataMan+8 != 0`, `*(u64*)0x553D6E0 != 0`, NowLoading byte
`0x556286B == 0`, not inside the save writer. Queue everything that arrives while summoned (mode 1) and apply
it at the next own-world load (B1 brings guests home on every travel, death and boss clear). Reason: in mode 1
the roll reads the host-world overlay, where the host's flag is already set, so the award would be empty, and
any flag it writes is discarded.

```c
// SysV (guest code is sysv_abi), main thread only
typedef void (*AwardLotFn)(void *mapItemMan, int32_t lot, uint8_t hostOnly);
#define AwardLot ((AwardLotFn)(0x800000000ull + 0x17DDC50))
void *mim = *(void **)(0x800000000ull + 0x553D6E0);

if (flag >= 0 ? IsEventFlag(flag) : ledger_bit(idx)) drop();      // 0x13CFC00 / 0x13CFD80
AwardLot(mim, lot, 0);           // rolls with the guest's own flags, gives, shows the popup,
                                 // sets the lot flag in the live tree (= saved pool in mode 0)
if (flag < 0) SetEventFlag(*(void**)(0x800000000ull+0x553B100), 60009000 + idx, 1);   // 0x13CFCC0
```
* `hostOnly = 0` because the caller does the gating; in mode 0 the gate would pass anyway.
* One lot per tick (each award opens a popup), and the next one only after the popup queue drains.
* Inventory full: `0x17D89F0` sends the overflow to storage, then drops it on the ground (`0x17DA960`); the
  flag is set either way (same as vanilla).
* RNG: the roll consumes the game RNG (`GSM+0xA80`) even for single-item lots. That is harmless on a guest in
  its own world.
* Not recommended: giving while summoned (calling `0x17D89F0` with a hand-built list plus writing the pool bit).
  It needs gaitem handles for weapons/armour (`0x1A876D0`, `0x1A88130`) and racing the overlay.

### 6.4 Interplay with C2 and C4

* Boss-defeated flags synced by C2 make the guest's own boss event never award (its completion flag is set)
  and are why flagless boss lots must come from C3. Order on a guest: C3 apply, then the C2 boss flags (or the
  same tick).
* NPC quests (C2 `npc_quest`, whole blocks later): their gifts arrive through C3 as soon as the host gets them,
  even if the guest's NPC block is not synced yet.
* Endings and NG+ (C4): NG+ variant lots (34000/34030, 43800/43802…) depend on the cycle; the host's lot id is
  used as is.

## 7. Patches considered and rejected for C3

| Site | Original | Would do | Why not |
|---|---|---|---|
| `0x17DDD3E` | `0F 84 8F 07 00 00` | `2003[4]` gives on clients | client EMEVD already skips the instruction (`1003[105]`), and the flag lands in the overlay, so items duplicate |
| `0x17DC3D6` / `0x17DC3E3` | `0F 84 A5 0C 00 00` / `0F 85 98 0C 00 00` | spawns treasure on guests | pickups on a guest set no flag (`0x17D6831`), giving infinite loot and a desync with the host's world |

## 8. Probes

* **I1** log `0x17DDC50(lot, hostOnly)` and `0x17D6780` (handle, kind, `+0x28`) on host and guest during a boss
  kill (Gascoigne) and a treasure pickup; confirm the guest gets no call for 31000 and gets 4680 + bonus.
* **I2** guest replay: in its own world call `0x17DDC50(mim, 2400450, 0)` (Hunter Chief Emblem) twice and
  check one item, flag 52400450 set, popup shown.
* **I3** ledger: set 60009000, save, reload, read it back (pool block present in mode 0 and mode 1).
* **I4** echoes: compare a guest's `PlayerGameData+0x94` delta for one enemy with and without patch
  `0x18FC93F`; `+0x84` before/after a boss as a phantom.
* **I5** what flag 6006 (cleared for a phantom in BlockClear2 at `0x13852EE..0x1385339`) and the global byte
  `0x553D6D8` mean; the host-side handler of packet 0x2C.

## 9. Implementation (gpu/shim/party/party_items.{h,cpp})

* Tables: `party_items.inc`, generated by `tools/party/item_tool.py gen-inc GAME`: `ITEM_LOT_FLAG` (1133
  lots with a pickup flag, Chalice Dungeons left out) and `ITEM_LEDGER` (43 flagless host-only `2003[4]`
  lots, the list of 6.1 item 2, with the awarding event's completion flag = event id + slot, for
  example 31000 → 12411800, common 9040 slot 5 → 9045). Ledger indices are append-only (the generator
  keeps the indices of an existing file).
* Ledger range check: `item_tool.py check-ledger GAME` scans every EMEVD (decoded operands and raw bytes),
  the param bnd, the talk ESD and the Lua bnds for any u32/f32/f64 in 60009000..60009999: 0 hits.
* Host: hook at the entry of `0x17DDC50`; `hostOnly == 1` lots in the own world become grants (flagged
  lot → key = flag, ledger lot → key = 60009000 + idx); `OnHostFlagSet(flag)` (C2's
  `progress::SetItemFlagObserver`, registered by `InstallItemsPatches`) maps flags to lots, preferring
  the lot the hook captured for that flag. Every key is sent once (EVENT `items`); a member that
  (re)joins gets EVENT `items_full`, built on the main thread from the host's flags (lot flags and
  ledger done flags) once the host's own world is up.
* Guest: grants are applied one at a time (1.5 s apart) once the gate held 90 frames: world up with a
  known map, no loading screen, flag store load mode 0, `GSM+0x1592 == 1`, session role 0 or 3, game data
  and MapItemMan present. Skip if the done flag is set; else `0x17DDC50(mim, lot, 0)`, then the ledger
  flag. `0x17D89F0` is hooked only to log what our own award adds to the inventory.
* Guest "mark": a `hostOnly == 0` lot with a flag that the guest receives while summoned (its flag only
  reaches the overlay) is queued as a mark; at home its flag is set (no item) before any replay, so the
  host's later flag report does not duplicate it. The mark lives in memory: a crash before going home
  loses it (one duplicate possible) [known gap].
* Deviation from 6.1 item 3: Ludwig's and Laurence's boss lots 3401800 (flag 6674) and 3401850 (6673)
  are replayed even though their flags are in 6000-8999: a phantom never gets them (`2003[4]` skipped on
  clients) and the flag is the guest's own, so the replay is exact. All other lots with personal flags
  stay denied.
* Parity patches of section 5: written at start in party mode (`BB_PARTY` set) unless
  `BB_PARTY_FULL_REWARDS=0`, each byte-verified.
* Debug: `BB_PARTY_ITEMS_TEST=<lot>[,...]` applies lots once in the own world in any role (it also turns
  on the party layer's tick). Single-instance runs (Continue via `BB_PAD_FILE`, Hunter's Dream, save
  level 37): `2300090` gave goods 1000 (Blood Vial) x1, flag 52300090 set; ledger lot `24020` gave goods
  1110 x2, flag 60009008 set (log of the `0x17D89F0` list). After a restart the first lot of a run is
  skipped ("already given"), ledger or treasure alike; the second one, awarded 1.5 s later, is given again
  together with its vanilla treasure flag: the game saved once after the first item and the test run was
  killed (BB_TIMEOUT) before the next save, so item and flag share the save. The ledger bit is set before
  the award call (cleared again when the award adds nothing) so it is in the same save as the item.
  With `BB_PARTY` set the three parity patches log `applied`.
* Open: whether the 9040-slot completion flags (9045...) are set exactly when the gift is given
  [inferred from the event shape]; NG+ handling of the ledger flags (T6 global zone 9) [unchecked]; the
  `items_full` list also carries `hostOnly == 0` flagged lots, which a guest that was present in an earlier
  session (mark lost) gets a second time.
