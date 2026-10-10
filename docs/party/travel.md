# Party travel (Phase B): lamp, Hunter's Dream, Hunter's Mark, transitions, death

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA; bbhost source = ours + 0x400000;
bbhost `.hpp` `Rva{}` = ours; shadp2p = ours). Decompiles are Ghidra (`tools/re/decomp.py`), disassembly is
capstone over `smoketest/out/eboot.elf`. Sources besides our own RE: bbhost `network_script_state.hpp`,
`session_manager.hpp`, `cs/session_connect_state_step.hpp`; shadp2p `documents/bloodborne-seamless-re.md`
(runtime captures, cited as [shadp2p]).

Status markers: **[static]** proven from code here, **[runtime-shadp2p]** proven by shadp2p captures,
**[inferred]** name/shape based, needs one runtime confirmation (each has a probe listed in section 5).

## 0. Globals and names used below

| Symbol | Address | Notes |
|---|---|---|
| `GameStateMan`/WorldTransitionState ptr | `0x5556678` | `+0x08` transition requested, `+0x0C` packed target map (area<<24 \| block<<16 \| region<<8 \| idx), `+0x10` warp point / bonfire id, `+0x1520` forced-placement byte, `+0x14F0/+0x1500/+0x1510` forced map/pos/rot, `+0x1524`, `+0x1528` qword last-lamp record `{id, aux}`, `+0x1538` respawn mode (1 world, 2 Dream), `+0x153C` qword respawn record `{id, aux}`, `+0x1590` online, `+0x1592` "in own world" byte (see 1.0), `+0x1690/+0x16B0/+0x16D0` reload snapshots, `+0x16F8` session/presentation state |
| `SprjSessionManager` | `0x5540290` | assert string in every user names it so. `+0x124` role (0 idle, 1 creating, 3 host, 4 joining, 6 client, 7 leaving), `+0xF8..+0x100` peer vector (stride 0x20), `+0x280` leave step |
| `CSMultiPlayMan` | `0x5540230` | separate singleton (assert in `0x19470F0`); `FUN_01E54830(CSMultiPlayMan)` resets it |
| Summon-reload phase object | `0x553D6D0` | `+0x84` phase (3 = guest map-reload into host world), `+0xF0` |
| `BeginLeaveSession` | `0x1ED07A0` | the plan's "CSMultiPlayMan::Stop". Roles 0/2/5 → false; else role=7, `0xC8ED70` → `0xC98070(ctl, 0xFF000023)` → … → `sceNpMatching2LeaveRoom` |
| Reset tracked entities | `0x15BED10(+0x16F8 state)` | called after every session-ending site; clears presentation members |
| PlayerIns | `*(WorldChrMan 0x553E878 + 0x60)`; `+0x78` chr type: 0 own-world body, 8 own-world (dead/grey), 1 white phantom, 2 black phantom, 0xC other phantom |

`0xC8ED70` (the only place that supplies reason `0xFF000023`):
```c
uVar2 = FUN_00c98070(*param_1, 0xff000023);   // enqueue matching stop task 0x0C
```
`0x1ED07A0`:
```c
if (role > 5 || ((0x25 >> role) & 1) == 0) {   // not 0/2/5
  *(int*)(this+0x124) = 7;
  if (FUN_00c8f570(this+0x18) && FUN_00c8ed70(this+0x18)) { alloc leave step at +0x280; set bit 0; return 1; }
  FUN_01ed0910(this);                            // reset to idle
}
return 0;
```

## 1. Code paths

### 1.0 The common funnel: `SessionWorldTransition 0x13CDE30`

Every stage change goes through this function (19 call sites, table in 1.6):
```c
undefined8 FUN_013cde30(void) {
  *(u8*)(GSM + 8) = 1;                                    // arm stage request
  lVar1 = *(long*)(GameDataMan + 8);
  if (lVar1) {
    *(u32*)(lVar1 + 0xa4) = PlayerIns ? PlayerIns->+0x78 : -1;   // remember chr type
    FUN_014ccd40(lVar1 + 0x1d0, *(u8*)(lVar1 + 0xca));
    if (*(u8*)(GSM + 0x1592) == 0) {                      // 0x13CDEA2: cmp byte [rax+0x1592],0 ; jne 0x13CDF1C
      st = *(GSM + 0x16f8);
      if (BeginLeaveSession(SprjSessionManager))          // 0x13CDEF6
        *(u32*)(ReloadPhase + 0xf0) = *(u32*)(GameDataMan->+8 + 0xa4);
      FUN_015bed10(st);                                   // drop presentation members
    }
    return 1;
  }
  return 0;
}
```
**What `+0x1592` means** [static]: writers are the GSM ctor (`0x1566743`, =1), the setter `0x156EEA0`
(`mov [rax+0x1592], dil`), stage update `0x1942216` (=1 after a load), and `MoveMapStep` ctor
`0x1937570`: `=0` when `ReloadPhase+0x84 == 3` (guest loading into the host's world, host event flags,
`FrpgNetMan+0xB14 |= 8`), `=1` otherwise (own world, own flags restored). So `+0x1592 == 0` means "I am a
summoned guest in someone else's world". Consequence:

* **Guest** (`+0x1592 == 0`): any warp leaves the room *immediately* inside `0x13CDE30` (`0x13CDEF6`).
* **Host/solo** (`+0x1592 == 1`): `0x13CDE30` does **not** leave. The host's room dies later, during the
  load, from one of two stage sites:
  * `0x19471B1` in the stage payload destructor `0x19470F0` — whenever `ReloadPhase+0x84 != 3`
    (that is always for a host):
    ```c
    if (*(int*)(ReloadPhase + 0x84) != 3) {
      if (BeginLeaveSession(SprjSessionManager)) *(ReloadPhase+0xf0) = GameDataMan->+8->+0xa4;  // 0x19471B1
      FUN_01e54830(CSMultiPlayMan);                         // tear down multiplayer ins tasks
    }
    ```
    [runtime-shadp2p: proven caller with reason 0xFF000023 on a guest; same branch for a host by static flow]
  * `0x193C657` in stage update `0x193AC10`: when the active stage record UID (`stage+0x28[idx*0xA0]+8`,
    `idx = stage+0x118->+0x20`) differs from `PlayerIns+0x3F8`, it caches the new UID and, **only for role 1
    or 3 (host)**, calls `BeginLeaveSession` then `0x15BED10`.
    ```
    193c537: cmp r13d, [rax+0x3f8] ; je 193c691      ; same map → nothing
    193c5de: mov eax,[rdi+0x124] ; cmp eax,3 ; je 193c61d ; cmp eax,1 ; jne 193c67b
    193c657: call 0x1ed07a0                             ; host leaves
    193c689: call 0x15bed10
    ```
  Which of the two fires first for a host warp is a runtime detail (probe P1); both end in reason
  `0xFF000023`, both must be handled for B2, neither matters for B1.

### 1.1 (a) Host uses a lamp (world lamp → Hunter's Dream)

0. **Vanilla blocks it while guests are present.** The lamp action object update `0x12F5C40` computes
   four "unavailable" bytes; byte `+0x49` is the multiplayer gate (`0x12F8315`):
   ```
   12f8303: mov rcx,[rax+0x100] ; sub rcx,[rax+0xf8] ; shr rcx,5   ; SprjSessionManager peer count
   12f8315: cmp dword [rbx+0x14],1 ; setg al                        ; presentation_count > 1
   12f831c: test ecx,ecx ; setg cl ; or cl,al
   12f8323: mov [r13+0x49], cl
   ```
   `+0x48` (lamp type 0x7A8 + event state), `+0x4A` (no lamp record `0x13C9AF0` or record `+0x270 == 0`),
   `+0x4B = FUN_0191B6E0(WorldChrMan, lampEntity)` (computed only if `+0x48/+0x49/+0x4A` are all 0: scans
   the other chr sets for a character whose current action-target entity equals the lamp and whose state
   is in table `0x5127DF0`). The action-button row is `r15 = 90000 + type` when all are 0, `90000` when
   `+0x49`/`+0x4B`, `90005` for `+0x48`, `-1` for `+0x4A`; the final enable is
   `FUN_0146E250(entity, all4==0, type)` at `0x12F8EBF` (writes `FrpgNetMan+0xC70` list entry `+0x48/+0x49`).
   Every interaction path also refreshes `+0x1528` (last-lamp record) from `entity + 1000` via
   `FUN_01F26B40` (same row lookup `0x13CDF30` uses).
1. Interaction sets the lamp flag (e.g. `72410100`); **common event 7200** (`common.emevd`, EVD record 31,
   instruction 0 = `1003[6] End if Client`, args at EVD `0xF55C`) waits for it and runs
   `2003[49] Warp Player to Respawn Point` → EMEVD dispatcher `0x17C0420` at `0x17C1CF0`:
   ```
   17c1cee: mov edi, dword ptr [rcx]   ; respawn/WarpParam id
   17c1cf0: call 0x13cdf30
   ```
2. `0x13CDF30(u32 id)` — the real "warp to lamp" primitive [static; sole direct caller is the EMEVD
   dispatcher, menu travel reaches it too per runtime-shadp2p]:
   ```c
   FUN_01f26b40(&row, id);                         // param row lookup
   if (row) {
     mode = row[6].lo == 0 ? 2 : 1;                // 2 = Hunter's Dream, 1 = world
     *(u32*)(GSM+0xc)    = row[0] << 24 | (row[1] & 0xff) << 16;   // area, block
     *(u32*)(GSM+0x1538) = mode;
     FUN_01f26b40(&row2, id);
     *(u64*)(GSM+0x153c) = row2 ? (row2->+4 | (u64)row2->+8 << 32) : (id | 0xffffffff00000000);
     FUN_013cde30();
   }
   ```
   Observed ids [runtime-shadp2p]: Great Bridge `2412952` (m24_01, mode 1, aux 101163), Central Yharnam
   `2412951`, Hypogean Gaol `2802952`, Dream from Great Bridge `2102950`, Dream from Gaol `2102952`
   (m21_00, mode 2, aux 101200). Timing: request ack `0x194100A` +533 ms, descriptor `0x1944D76`
   +650 ms, map resource +1033 ms, SOS area settles +4.3 s (Dream) / +6.6 s (Gaol).
3. Load: descriptor builder copies `+0x1538` → desc `+0x98`, nonzero → request type/subtype 9/2,
   `+0x153C/+0x1540` → desc `+0x80/+0x84` (`0x1944B33`, `0x1944D76`); `0x1928680` → stage manager →
   `MoveMapStep` ctor `0x1937570` (`+0x58` map, `+0xA4/+0xA8` type, `+0xD0` respawn id).
4. **Session end** (host): `0x13CDE30` skips the leave (`+0x1592 == 1`); the room is closed by
   `0x19471B1` and/or `0x193C657` → `0x1ED07A0` → `0xC8ED70` reason **`0xFF000023`**. Guests then get
   member-left/room-gone → Lua `OnRoomDisappeared 0x138A590` / `OnIrregularLeaveSession_1 0x13840E0` →
   state `OnLeave_Limit 0x138A4A0`:
   ```c
   if (phantom count > 0) {                      // PlayerIns->+0x3b0->+0x20->+0xf8
     *DAT_0553b0d8 = 2;
     *(u8*)(GSM+0x1520) = 1;                     // forced placement = my pre-summon spot (+0x14F0..)
     FUN_01332bc0(ctx, -1);                      // selector: +0x0C = +0x14F0
     FUN_013cde30();                             // guest: +0x1592==0 → BeginLeave(0xFF000023)
   }
   ```

### 1.2 (b) Host goes to the Hunter's Dream

Same as (a): world lamp → event 7200 → `0x13CDF30(21xxxxx)` (mode 2). Dream headstone travel back out
uses the warp menu (`FrpgMenuDlgWarp`, Lua `GetWarpMenuResult`/`OnWarpMenuClose`) and ends in the same
`0x13CDF30(id)` (mode 1) [runtime-shadp2p: "every observed menu travel entered through the WarpParam
handler"]. Session end identical to 1.1.4. Additionally in vanilla the Dream itself refuses multiplayer
(section 2), so nobody can rejoin there.

### 1.3 (c) Hunter's Mark

Lua handlers (`name → fn`, recovered from the registration code `0x138E000..0x1392000`):
`OnReviveMagic 0x1389D20`, `OnReviveMagic_1 0x1389E80`, `OnLeaveMagic 0x13881E0`,
`OnLeaveMenu_Yes 0x1389FB0`, `OnSpEffectRevive 0x1389F30`. Hunter's Mark is the BB Darksign; it maps to
`OnReviveMagic` [inferred, probe P3; Silencing Blank / Small Resonant "leave" map to `OnLeaveMagic`].

`OnReviveMagic 0x1389D20`:
```c
if (PlayerIns) { if (type == 2) return;           // invader can't
                 if (type == 1) FUN_0132e960(ev, 0xfd7, 1); }   // white phantom: notify
FUN_0132ed40(ev, 0xfdf, myId);
st = *(GSM + 0x16f8);
if (BeginLeaveSession(SprjSessionManager)) ...;    // 0x1389DDD  ← session ends here, both roles
FUN_015bed10(st);
FUN_0130fd40(0,0,-1,0,-1,1);
FUN_0132b1c0(..., 0xfce, "OnReviveMagic_1", ...);   // next Lua state
```
`OnReviveMagic_1 0x1389E80`: `*(GSM+0x1520)=1; FUN_01332bc0(ctx,-1); *(GameDataMan+0x8c)=0; FUN_013cde30();`
(forced placement path → own lamp). So **host or guest**: the leave is immediate at `0x1389DDD`,
reason `0xFF000023`, before any load. The guest goes to *its own* last lamp in its own world.

### 1.4 (d) Area transition / fog into another map

* Most of Yharnam is streamed (`m24_00/01/02` blocks) — no load and no `0x13CDE30`. The only session
  policy on map identity is the host stage-UID check `0x193C657` (1.0): it fires when the *active stage
  record* changes; whether streaming between `m24_0x` blocks changes that record is probe P2.
* Scripted loading transitions (elevators/doors/kidnapper to Gaol, Nightmare entries, Lecture Building,
  etc.) are EMEVD `2003[14] Warp Player` (`0x17C0420`, `0x17C0B80..0x17C0BED`):
  ```
  movzx r15d, byte [rcx]   ; area      movzx r14d, byte [rcx+1] ; block
  mov r12d, [rcx+4]        ; warp point (10000 → current target)
  mov [GSM+0xc], area<<24|block<<16 ; mov [GSM+0x10], r12d ; call 0x13cde30   (0x17C0BED)
  ```
  Session end: host as in 1.1.4 (load-time stop), guest immediately in `0x13CDE30`. Boss fog walls are
  not stage changes; guest-side gating there is `Lua_MultiWall 0x138B720` / `Lua_InvalidMultiWall 0x138B890`.
* Lua `WarpNextStage 0x132E010(ctx, area, block, region, index, warp_point)`,
  `WarpNextStage_Bonfire 0x132E050(ctx, id)` (area = id/100000, block = (id/10000)%10, `+0x10 = id`),
  `WarpNextStageKick 0x1332B80()`, `Lua_Warp_1 0x138C570` (plays anim `0x205C`, then same as
  `_Bonfire` with event `+0x20`) are script-driven variants of the same funnel.

### 1.5 (e) Host dies

Death dispatcher Lua `OnEvent_4000 0x13813D0` (all players): reads role `SprjSessionManager+0x124`,
`n = presentation state(+0x16F8)+0x18`, chr type `t`:
```c
own = (t == 0 || t == 8);
if (own && n > 1 && role == 3)      → state "SoloPlayDeath"   (host in co-op)
if (t == 1 || t == 2 || t == 0xc)   → state "PartyGhostDeath" (phantom died)
if (own && n < 2)                   → state "SoloPlayDeath"   (solo)
```
* Host: `SoloPlayDeath_2 0x1381DE0` clears snapshots `+0x1690/+0x16B0`, `FUN_01332BC0()` (no forced
  byte → `+0x1528 != -1` ⇒ `FUN_013CA100` = last lamp map, `+0x10=-1`, `+0x1524=1`), `0x13CDE30` (no leave,
  `+0x1592==1`) → room closes at load time (1.0), reason `0xFF000023`.
* Guests receive the host-death event; Lua `HostDead 0x1381870` (returns at once if role==3; invader gets
  the PK-success text) → `HostDead_1 0x1381A60`:
  ```c
  *DAT_0553b0d8 = 2; *(u8*)(GSM+0x1520) = 1;      // forced = my pre-summon spot
  FUN_01332bc0(ctx, -1); FUN_013cde30();         // guest: BeginLeave here (0x13CDEF6), 0xFF000023
  ```
  (`OnDeadEvent_HostDead 0x138CA40` is only the message/effect.)
* A guest's own death: `PartyGhostDeath_2 0x13824C0` → forced home + `0x13CDE30` (leave).
* Also in the same family: `BlockClear2_1 0x1385470`/`BlockClear2_3 0x1385930` (boss killed → guests home),
  `Failed_BossAreaMission_LeaveMap 0x138B4E0`, `PlayerKill_4030_1 0x1383A00`, direct
  `BeginLeaveSession` in `OnMatchingCheck 0x13808B0` (`0x13809B7`, remote chr not found in WorldChrMan),
  `OnMatchingError 0x13809F0`, `OnLeaveMenu_Yes 0x1389FB0` (`0x138A11E`).

### 1.6 Call sites of `0x13CDE30` → travel kind (for the hook in 3.1)

| Return addr − 5 (call) | Caller | Kind |
|---|---|---|
| `0x13CE002` | `0x13CDF30` (EMEVD `2003[49]`) | lamp / Dream / headstone travel (respawn id in `+0x153C`) |
| `0x17C0BED` | EMEVD `2003[14]` | scripted area transition (`+0x0C/+0x10`) |
| `0x132E03E` / `0x132E0B4` / `0x1332B84` | WarpNextStage / _Bonfire / Kick | Lua stage warps |
| `0x138C5F0` | `Lua_Warp_1` | Lua bonfire warp |
| `0x1381EDE` | `SoloPlayDeath_2` | own death (host in co-op) |
| `0x1389F20` | `OnReviveMagic_1` | Hunter's Mark |
| `0x1381A93` | `HostDead_1` | guest sent home: host died |
| `0x138A51C` | `OnLeave_Limit` | guest sent home: room gone/kicked/left |
| `0x1382663` | `PartyGhostDeath_2` | guest died |
| `0x13856F4` / `0x13859DF` | `BlockClear2_1/_3` | boss cleared |
| `0x138B513` / `0x1383A5C` | `Failed_BossAreaMission_LeaveMap` / `PlayerKill_4030_1` | mission/PK |
| `0x132E11E`, `0x132E166` | `0x132E0C0` | map `0x01000000` (title/tutorial style) |
| `0x13175EF`, `0x1317632` | `0x1317550` | leftover debug preset maps (DS1 ids) |

## 2. Why co-op is blocked in the Hunter's Dream, and the patch

**Not the action bit.** `UseItem` = action bit `0x80` (`SprjChrActionFlagModule` updater `0x1A0F910`;
first consumer `0x18F7516`) fires in the Dream as elsewhere [runtime-shadp2p]. The bell is rejected
earlier, by item availability `0x157F200` (the inventory greys it out), so nothing at `0x18F7516` needs
patching.

**The real gate is the area restriction table.** All 14 callers of `FUN_0131D7B0` (bell availability
`0x157F90E`, active-bell updater `0x15068F7`, SOS status `0x18700CE`, summon builder `0x18749E1`, responder
search `0x191A968/0x191AA4C`, …) use it:
```c
a = |sosArea|;  i = (a/10000)%100 - 21;  blk = (a/1000)%10;  rgn = (a/100)%10;
if (i < 16 && blk < 3 && rgn < 4) {
  f1 = TBL1[i][blk][rgn];  /* 0x47304B0 */   f2 = TBL2[i][blk][rgn]; /* 0x47307B0 */
  blocked = (f1 > 0 && GetEventFlag(f1)) || (f2 > 0 && GetEventFlag(f2));
}
```
Dumped `TBL1` rows (12 ints each = 3 blocks × 4 regions): `m21: 2100,-1…`, `m22: 2200`, `m23: 2300,2301`,
`m24: 2400,2401,2402,-1, 2410,2411,2412,-1, 2420,2421`, `m25: 2500`, `m26: 2600,2601`, `m27: 2700,2701`,
`m28: 2800`, `m29: 12901800..803`, `m30/m31: none`, `m32: 3200`, `m33: 3300`, `m34: 3400..3403`,
`m35: 3510..3513`, `m36: 3600,3601`; `TBL2` all −1 (m36 zeros). These are the "area boss defeated" flags;
the Dream's SOS area `210000` maps to flag **2100**, which the Dream keeps ON (the same mechanism as
`2800` in m28 [runtime-shadp2p]). Hence Beckoning greyed, no SOS request, Small Resonant refused.

Patches to allow a party in the Dream (all exact, byte-verifiable):

1. **Data patch** `dword [0x47304B0] = 0xFFFFFFFF` (was 2100). Removes the Dream from the table for all
   14 consumers at once. (Equivalent finer option: shadp2p's bell set `0x157F8C8/0x157F960/0x157F6D1/
   0x157F6D3/0x15068BB/0x15068FC/0x191A8C3/0x18700D3`, which A6 may already apply for boss-cleared areas.)
2. **Guest bell in the Dream**: the Small Resonant branch also fails after the multiplayer effect mask
   changes (`0x202205 → 0x202207`) in the Dream; shadp2p's `0x157F6D1` (`cl=1`) + `0x157F6D3` (jump to
   epilogue) — goods 205/225 only.
3. **Same-map is enough**: when everyone is in the Dream the area-group checks `0x14B714A` and
   `0x1874B79` pass natively; they are only needed for cross-map joins. `0x18749E8/0x18749F0` (boss-cleared
   / negative event id in the builder) are already in A6.
4. Expected leftovers to test: m21 EMEVD client guards (`1003[6]`) make the Doll/headstones/workshop
   host-only for guests (desired: travel is host-led); host headstone menu with guests present is not yet
   proven open (probe P4); Insight cost of the Beckoning Bell (A5 policy).

## 3. B1 "follow and rejoin" — design and hook points

### 3.1 Host: warp detection
* Hook **entry of `0x13CDE30`** (19 callers, one place). In the hook read the caller (return address
  − 5, table 1.6) and the already-written state: `+0x0C` packed map, `+0x10` warp point, `+0x1520`,
  `+0x14F0/+0x1500/+0x1510`, `+0x1528` (qword), `+0x1538`, `+0x153C` (qword). Also hook entry of
  **`0x13CDF30`** to capture the *key* `edi` (the id to replay; `+0x153C` stores the row's `+4` field,
  which equals the key in all observed rows — probe P5).
* Act only if role (`SprjSessionManager+0x124`) is 3 and party mode is on; send
  `PARTY_CMD TRAVEL {seq, kind, key/respawn id, mode, packed map, warp point, last-lamp record,
  forced transform}` on PartyLink **before** calling the original (the host's room survives until its
  load, ≥ 0.5 s, so guests get the message while still connected; PartyLink is independent anyway).
* Host death needs no special hook: `SoloPlayDeath_2` reaches the same entry with kind = own death,
  destination = `+0x1528` (send the record; `+0x10 = -1`).
* Kinds to broadcast: lamp/headstone (`0x13CE002`), scripted transition (`0x17C0BED`), Lua warps,
  host death (`0x1381EDE`), host Hunter's Mark (`0x1389F20`; note the host already left at `0x1389DDD`,
  so hook `0x1389D20` entry too to send TRAVEL first).

### 3.2 Guest: warp to the same place, on the main thread
Run from the A5 tick (`SprjFlipper::Update 0x2034770`). Preconditions: `WorldChrMan+0x60 != 0`,
NowLoading `0x556286B == 0`, `GSM+0x08 == 0` (no pending request), role not 4/7, no menu open.

* **Lamp / Dream / headstone / host death** — call the native respawn warp:
  `void 0x13CDF30(u32 id)` (SysV: `edi = id`; no `this`). Same function event 7200 uses; resolves map
  and mode itself (Dream = mode 2). For host death use the low dword of the host's `+0x1528`.
* **Scripted transition or Lua stage warp** — `0x132E010(ctx=0, int area, u8 block, u8 region, u8 index,
  int warp_point)` (`rsi, edx, ecx, r8b, r9d`; returns 1) or `0x132E050(0, int bonfire_id)`.
* **Exact transform** (fallback, e.g. no lamp id): native forced-placement setters, all taking a pointer:
  `0x156CF10(&u32 map)` → `+0x14F0`, `0x156CF20(&vec4 pos)` → `+0x1500`, `0x156CF40(&vec4 rot)` →
  `+0x1510`, `0x156CF60()` → `+0x1520 = 1`; then selector `0x1332BC0(0, -1)` (copies `+0x14F0` to
  `+0x0C`, ignores arg 1) and `0x13CDE30()` — exactly the `HostDead_1` sequence; shadp2p validated it
  lands at an exact host transform when no room exists.
* The guest is still a summoned phantom when it calls these: `+0x1592 == 0` → `0x13CDE30` leaves the
  room at once (that is wanted), and because `ReloadPhase+0x84 != 3` by then, `MoveMapStep` takes the
  normal path: **the guest loads into its own world with its own flags** at the host's lamp.
* **Suppress the stock "send home" warps** while a follow is pending: in the same `0x13CDE30` hook on the
  guest, if the caller is `0x138A51C` (OnLeave_Limit), `0x1381A93` (HostDead_1), `0x13856F4/0x13859DF`
  (boss clear, if A6 policy is "stay") and a TRAVEL is pending, rewrite instead of going home: clear
  `+0x1520`, then return through `0x13CDF30(id)` (re-entrancy guard on the hook). This also covers the
  race where room teardown reaches the guest before the tick runs. `OnRoomDisappeared 0x138A590` shows a
  dialog (FrpgNetMan `+0xA05` bit 0 clear) — suppress with A6's member-left suppression.
* Guest-initiated Hunter's Mark (`0x1389D20`, type 1): policy hook — either let it go home (leave party
  temporarily) or redirect to the host's last lamp (`+0x1528` from the host's PROGRESS) via the same
  rewrite.
* **Peer validation** (`SanitizePeerTravel`, bbport security pass): every id of a host's intent is looked
  up in tables generated from the game data (`gpu/shim/party/party_ids.inc`, `tools/party/ids_tool.py`):
  the 0x13CDF30 ids (`lamp`, the low dwords of `respawn` / `last_lamp`, the phantom event's lamp) must be
  **ReturnPointParam** rows (the param the lamp warp looks up: +0 area, +1 block, +4 returnPointEntityId,
  +8 returnAnimId, `isRegistDeadReturn` 0 = Hunter's Dream; rows 1 and 9902950 have area 0 and are left
  out); `map` and the transform's map must be existing maps (map folders and MSBs); a stage-warp point
  must be an entity of the destination map (int32 ids AABnnnn found in that map's MSBs, plus the EMEVD
  2003[14] / 2002 warp points), a 0x132E050 bonfire id an entity of the map it names. An unknown id
  becomes "none" (a stage warp then uses the map's default entry; no lamp id leaves the lamp warp out).

### 3.3 Auto-rejoin and timing
1. T0 host hook sends TRAVEL; host request ack ≈ T0+0.53 s; host room closes during its load
   (`0x19471B1`/`0x193C657`, reason `0xFF000023`); host SOS area settles ≈ T0+4–7 s.
2. Guests start their own load ≈ T0+RTT+1 frame (in parallel with the host).
3. "Arrived" = NowLoading clear (`0x176D3C0`), `PlayerIns_GetBloodMarkMap 0x19046C0` == TRAVEL map,
   SOS area stable, role 0 and no controller, for 20 consecutive ticks (shadp2p's guard).
4. Host rings Beckoning (`OnEvent_Call_SOS` via `0x1339870`), guests ring Small Resonant
   (`OnEvent_SendSoulSign_NormalCoop`); PartyHostService matches immediately (same map → no area
   patches needed except boss-cleared/Dream ones). Guest then does `SummonedMapReload` (second, short load
   into the host world).
5. Expected: guests visible again ≈ host load + ~1 s + guest join reload (≈ 8–15 s after the lamp press),
   two loading screens for guests, one for the host. Retry the bell once after 15 s without effect 9005
   (shadp2p) / summon result.
* Edge rules: if a guest's world lacks the destination (DLC not owned, map locked) fall back to the
  host's transform via the forced setters; if a guest is mid-load when a second TRAVEL arrives, keep only
  the newest `seq`.

## 4. B2 true seamless warp — assessment

What would have to hold, with the blocking sites:

| Requirement | Site(s) | Difficulty |
|---|---|---|
| Host can open the lamp/headstone with guests | `+0x49` at `0x12F8323` (patch to 0); second gate unresolved: candidates `+0x4B` (`0x191B6E0` — another character targeting the lamp entity, i.e. a phantom near the lamp), action-button row 90000 vs 90000+type (`0x12F83E0`), the enable consumer of `FrpgNetMan+0xC70` entries (`0x146E250` writers `0x12F5A50/0x12F5B40/0x13274C0/0x1327650`), Lua `OnEvent_Bonfire` scripts | medium (probe P4) |
| Host keeps the room through its load | retarget `0x19471B1` → `0x1ED00A0` (shadp2p), skip `0x193C657` (role-3 stage-UID stop), skip the `0x15BED10` calls after them | low |
| Guests keep the room through *their* load | `0x13CDE30` leave (`+0x1592==0`, `jne` at `0x13CDEAE` → force jump while traveling), `OnMatchingCheck 0x13809B7` (remote chr missing during rebuild), OnLeave_Limit/HostDead_1 send-home | low–medium |
| Guests load the new map **in the host world** | `MoveMapStep` ctor host-world path needs `ReloadPhase+0x84 == 3`; only the `SummonedMapReload` serializer `0x131E5C0` / setter `0x178D9A0` sets it, and it snapshots the *source* map into `+0x1690/+0x16B0/+0x16D0` (must be emptied, as `SoloPlayDeath_2` does); ordering trap: phase 3 must be set after the target descriptor exists (`0x1944F23`) [runtime-shadp2p] | high |
| Host's flags for the new map on guests | vanilla sends the flag snapshot only at join (serialize `0x13BE3C0` / apply `0x13BECA0`); needs a re-send before guests' `MoveMapStep` (C2 territory) | high |
| Remote PlayerIns re-created after load | WorldChrMan is rebuilt; remote chrs are created only by the join insertion tasks `0x1E4FCF0..0x1E50D70` (post-copy `0x1E4FF3A`), and the payload dtor calls `FUN_01E54830(CSMultiPlayMan)` which resets them; there is no native "re-insert existing member" entry — must drive insertion manually for host and every guest | very high |
| Net enemies/objects across different load speeds | net chr sync tables are per loaded map; packets referencing unloaded entities → stale pointers (DS2 crash class); needs per-peer "map ready" gating of packets | high |
| Guest event 7200 / map events | `1003[6]` client guards end events on clients; fine (guest follows our message, not the event) | low |

`SetSessionTargetMap 0x156CE80` (`+0x14C4 = *rdi`) and its route siblings `0x156CE90/0x156CEB0`
(`+0x14D0/+0x14E0`) only set the summoned-placement bank used by the join/reload path; they do not by
themselves move anyone or keep a session, so they are useful only inside the high-difficulty "host-world
load" row.

**Verdict:** B2 is research-grade (several weeks, highest crash risk, depends on C2 flag sync and on a
custom re-insertion driver). Ship **B1** (one hook on `0x13CDE30` + one on `0x13CDF30`, guest replay via
`0x13CDF30`/forced setters, send-home rewrite, auto-rejoin), plus the Dream table patch so the Dream works
as a lobby. Revisit B2 only after B1 and C2 are stable, starting with the cheap rows (host room retention,
guest leave suppression) behind a flag, and measure whether `0x1E50xxx` insertion can be re-run.

## 5. Runtime probes (to close the [inferred] items)

* **P1** log return address of every `0x1ED07A0` call and `0x193C657`/`0x19471B1` order during a host lamp
  warp with a guest present (after the `+0x49` patch) and during host death.
* **P2** walk Central Yharnam ↔ Cathedral Ward ↔ Old Yharnam as host with a guest: does `0x193C537` see a
  UID change (would end vanilla co-op)?
* **P3** use Hunter's Mark as host and as guest: confirm `0x1389D20`/`0x1389E80` fire (vs `0x1389F30`).
* **P4** at `0x12F8E8A` log `+0x48..+0x4B`, `r15` at `0x12F83FD`, and the `FrpgNetMan+0xC70` entry for the
  lamp, with and without the `+0x49` patch; read `GetEventFlagValue 0x13CFD80(2100)` in the Dream.
* **P5** at `0x13CDF30` entry log `edi` and the stored `+0x153C` qword for lamp, headstone and death
  (key vs row `+4`).
