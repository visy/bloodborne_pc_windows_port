# Guest services: Doll, shops, workshop (and lamps) for a summoned guest

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA of
`smoketest/out/eboot.elf`; XML `Address` = ours + 0x400000). Talk scripts were decompiled from
`script/talk/m21_00_00_00.talkesdbnd.dcx` (soulstruct 2.3.2 ESP export), EMEVD with
`tools/party/flag_tool.py`'s parser, MSB `map/MapStudio/m21_00_00_00.msb.dcx` read raw. Status labels:
**[C]** confirmed statically, **[R]** confirmed at runtime (live 2-instance run), **[I]** inferred.

Goal (user requirement, like Elden Ring Seamless Co-op): a guest in the host's world can use the
Doll (level up), the Bath Messengers' shops (echoes, Insight), the workshop tools (weapon upgrade,
Blood Gem workbench, storage) and lamps, on its own character; nothing it does touches the host.

## 1. Whose data does a summoned guest change?

* **Character record** [C]: level (`PlayerGameData +0x90`), echoes (`+0x94`), Insight (`+0x84`),
  attributes, inventory (`+0x328`) and storage live in the local `PlayerGameData`
  (`[[0x553B130]+8]`). Joining another world (SetLoadMode(1)) replaces only the **event flag** tree
  with the host-world overlay (`event_flags.md` 3); the record is never overlaid or swapped. Every
  menu (level up `OpenSoul`, shops, upgrade, storage) reads and writes that local record.
* **Save** [C/R]: the game's save writer (`0x156AA50`, `event_flags.md`) serializes the whole local
  save - the record plus the flag **pool** (not the overlay). It runs while a guest is summoned
  as at home (vanilla: consumables a phantom uses stay used). So level-ups, purchases, echoes and
  Insight spent persist in the guest's own save (the party save when `BB_PARTY_SAVE=separate`).
  Runtime evidence: section 5.
* **Flags** [C]: talk-script `SetFlagState` on a summoned guest goes to the overlay (discarded on
  return), except 6000-8999, which load mode 1 keeps in the guest's own pool: exactly the
  personal Dream flags (Doll / messenger shop stock, dress-up unlocks, chalice unlocks). Shop stock
  limits therefore track the guest's own purchases. Nothing a guest's talk script does is sent to
  the host: talk flags are local (no `2000[2]` network sync, no Lua `CompleteEvent` packet).
* **Host** [C]: no menu of the guest has a network path; the host's record and save are not
  reachable from the guest's game.

## 2. Doll, shops, workshop: the gate and the lift (implemented)

### 2.1 Gate [C]

Every Hunter's Dream talk script that opens one of those menus has a "client" state machine
(`クライアント待機` = client wait; `ゲスト待機` = guest wait in the t21069x workshop scripts):

```
State_1: CALL_STATE_MACHINE[9](...)      # the normal talk / menu machine
         if IsClientPlayer() == 1: -> State_2
State_2: CALL_STATE_MACHINE[8]()         # "client wait": does nothing
         if IsClientPlayer() == 0: -> State_1
```

`IsClientPlayer` is talk test function 19. The talk environment's test-function switch is
`0x203AD60` (`call [vtbl+0x10]` = function id, `cmp eax,0x3e`, jump table at `0x20473B4`; ids 0-62,
the rest falls through to `0x203C747`). Case 19 (`0x203BA66`):

```
203ba66: mov rax,[r14] ; mov rdi,r14 ; call [rax+0x18]   ; argument count
203ba6f: cmp eax,1 ; jne 0x203c747
203ba78: 41 0f b6 47 25    movzx eax, byte [r15+0x25]   ; r15 = talk environment
203ba7d: e9 4d 34 00 00    jmp 0x203eecf                 ; [r12+8] = 2 (int), [r12] = eax
```

`env+0x25` is set in `0x203ABB0` (`0x203AC0F`) when the local player's chr type (`[[WorldChrMan]+0x60]
+0x78`) is not 0 or 8 (host body / grey): any phantom. `env+0x10` is the talk script id
(`t210304` -> 210304) [R: logged for every Dream script in the live run]. Case 52 `IsMultiplayerInProgress`
(`0x203C505`) is the session presentation count (`[[0x5556678]+0x16F8]+0x14 > 1`) or the session
peer count; only the headstones (t210000-t210003) and the chalice altar (t210100) use it - those
are blocked for the host too while a guest is present (lamp phase, section 3).

No other gate: the shop scripts t2106xx (`OpenRegularShop`) have no client test at all, the menus
themselves (`OpenSoul`, shop, upgrade, `OpenRepository`) have no role test, and the messengers'
EMEVD animation events (12105043 Insight messenger, 12105064 dress-up messenger: `1003[6]` end if
multiplayer after the first animation request) only decide whether the messenger has risen; with
the host's flag (e.g. 12100320) in the overlay the guest sees the same messenger state.

### 2.2 Which talk scripts (MSB m21_00_00_00 entity -> talk script) [C]

The filter allows by talk script id; the entity column is where the script is placed.

| Entity | Talk | What | Guest |
|---|---|---|---|
| 2100700 | t210304 | Doll (level up; "talked to Gehrman" branches) | yes |
| 2100200 | t210693 | storage (`OpenRepository`) | yes |
| 2100201 | t210692 | workbench: weapon upgrade, Blood Gems, repair, crush materials | yes |
| 2100202 | t210696 | mirror (appearance, `OpenCharaMakeMenu`) | yes |
| 2100203, 2100211, 2100215, 2100216 | t210699, t210691, t210694, t210695 | Bath Messengers: shops, sell | yes |
| 2100218 | t210697 | dress-up messenger (attire for flags 6071..) | yes |
| 2100219 | t210698 | Insight messenger (Insight shop) | yes |
| 2100600 | t210305 | Gehrman (last event: the ending choice) | no |
| 2100800 | t210306 | (Gehrman / Doll death branch) | no |
| 2100950.. | t210000-3 | headstones (travel) | no (lamp phase) |
| 2100230, 2100231 | t210720, t210721 | Dream lamp / warp menus | no (lamp phase) |
| 2100954, 2100959, 2100960 | t210050, t210100, t210110 | chalice altars | no |

t210140 / t210141 / t210200 / t2107xx (other `OpenSoul` scripts) are not placed in the m21 MSB;
they are assigned elsewhere (NPC param / other maps) [I].

### 2.3 Lift (party_guestsvc.cpp) [C]

`HookValueFilter` (coop_hooks) replaces the 5-byte `movzx` at `0x203BA78` with a jmp to a stub
that runs it, then calls `IsClientFilter(env = r15, value = eax)` with all volatile registers
saved, and continues at `0x203EECF` with the filtered eax. The filter answers 0 only when

* the gate is open: `BB_PARTY_GUEST_SERVICES` on (default in party mode), the local player is a
  client (`SprjSessionManager+0x124 == 6`), world up, not loading, map `m21_xx` (Hunter's Dream);
* the game said 1; and
* the talk script (`env+0x10`) is in the list above (`BB_PARTY_GUEST_SERVICES_TALK=all|id,...`
  widens it for diagnostics).

Everywhere else the game's own answer stands: quest NPCs in the host's world keep refusing guests,
Gehrman's ending choice and the travel menus stay host-only. Nothing runs on the host (role 3: the
game already answers 0 there).

Log lines (`Party guest services:`): `open: guest in the host's m21_00_00_00; level L, echoes E,
Insight I`, `talk script tN (environment 0x...) asks IsClientPlayer: allowed|kept vanilla`,
`talk scripts asked IsClientPlayer N times (answered 0)`, `own character: level a -> b, echoes ...`.

### 2.4 The Doll on a guest (placement) [C]

User report from the first live run: the guest in the host's Dream did not see the Doll where the
host's Doll stands. What decides the Doll, per m21_00 EMEVD:

* No client guard and no character disable for the Doll (entity 2100700) anywhere in m21_00 or
  common EMEVD: the constructor only sets her update frequency (`2004[34]`), the preconstructor
  keeps the Dream state flags 1000-1003 (`1003` "Doll dead" via 12100101, 1002 / 1000 via 9462).
  Her talk script t210304 runs on the guest (the live run logged it asking IsClientPlayer), so the
  character exists on the guest.
* **12100110** `人形のヒロイン_立ち位置制御` (Doll stand position, every Dream load, both roles):
  lifeless (spot 2102304, anim 9011, sets 12105100) if flag 9401 is off or Insight is 0; else
  12100105 = 1 (awake), 9404 = 1; ends for the late states (1000 / 1002 on, 1026 / 1027, the DLC
  boss flags 13501800 / 13601800 without 72100116 / 72100117); then **re-rolls the stand spot**:
  index 24 `1003[1]` skip 2 if flag 100, index 25 `2003[22]` clear 12100500-12100509, index 26
  `2003[17]` random one of them, and short-warps her (`2004[41]`) to 2102301 (by the lamp, anim
  9009), 2102302 (in the workshop, -13.3, -1.1, 1.7) or 2102303 (lower garden, -7.4, -6.3, -25.1).
* Where the inputs come from on a guest (load mode 1, join snapshot 0x10): 9401 (global 9000-9999)
  and 100 / 1000-1003 / 1026 / 1027 (global 0-1999) are in the snapshot = the host's values;
  12100105 and 12100500-12100510 are m21 type-1 zone 0 = in the snapshot; Insight is the guest's
  own (the test guest has 6); 13501800 / 13601800 (other maps) and 72100116 / 72100117 (T7) are
  not in the snapshot (0 on the guest: only differs for a host that killed those DLC bosses).
* So a guest gets an awake Doll, but **rolls its own stand spot**: in two of three cases its Doll
  stands in the workshop or the lower garden while the host's stands by the lamp.

Fix (party mode, guest services on): client-only EMEVD skip rules `12100110:2003:22@25` and
`12100110:2003:17@26` (`EmevdSkipRule::client_only`, checked against `SprjSessionManager+0x124
== 6` in the EMEVD filter): on a client the re-roll is skipped, the host's 12100500-12100502 from
the snapshot stay, and the guest's Doll is warped to the host's spot. The host is unaffected (the
rules never match there). `BB_PARTY_GUEST_SERVICES=0` drops them.

### 2.5 The Doll as a wandering ghost on a guest [R, fixed]

Live: the guest saw a translucent Doll it could walk through and not talk to. Probe: her ChrIns
chr type (+0x78) was 5 on the host and **4 (wandering ghost)** on the guest. Chr creation
`0x18E4F94..0x18E4FDC`: when the local session role is 6 (client) or a presentation is active
(`[[0x5556678]+0x16F8]+0x98 != -1`), an NPC whose NpcParam row (`[chr+0x3C8]+8`) has
**isChangeWanderGhost** (byte 0x145 bit 7) gets `ghostModelId` (+0x10C) and chr type 4. The Doll's
row **803000** has it (1.09 byte `0x80`; 804000 too). Fix: a seamless param rule writes NpcParam
803000 byte 0x145 `0x80 -> 0x00` (row size 0x184; only clients read the bit;
`BB_PARTY_GUEST_SERVICES=0` keeps vanilla). Live after the fix: chr type 5 on the guest, solid,
her talk menu opens.

### 2.6 The Doll's level-up handshake [C, R]

`t210304` StateMachine x7: State_3 "レベルアップ" -> State_22 -> State_10 sets 72100100 and waits for
**72100101 and 72100102**, then State_7 runs `OpenSoul()` (talk command 31: switch `0x203FFD0`
case at `0x2040B4E`, menu request `0x1C4A520` with the factory `0x1BB7F60` that builds the menu
named "LevelUp"). 72100101 is set by m21_00 event **12100112** (the player's kneel), 72100102 by
**12100113** (the Doll's gesture); 12100114 is the level-up SFX (flag 7500). All three start with
`4[3] [15, 10000, 0]` + `1000[2]` = end unless the local chr type is 0 (12100112 repeats the test
in each distance branch). Live trace (guest): the kneel ran once 12100112 was rewritten, but no
talk command 31 and no LevelUp factory call followed - 72100102 never came. Fix: an EMEVD
rewriter (second slot, `SetEmevdRewriter2`) sets those `4[3]` chr-type bytes (args +8) to the local
player's type on a client while they execute and back to 0 on a non-client (the EVD data is
shared) for events 12100112-12100114. The command path itself has no role test (the shop
command uses the same menu request and works, 5.).

## 3. Lamps and headstones (implemented, opt-in `BB_PARTY_GUEST_LAMPS=1` until run live)

Bloodborne's world lamps only go to the Hunter's Dream (resting = the load into the Dream refills
and respawns), and the Dream's headstones go to a lit lamp. Seamless-style behaviour for a party:
whoever uses a lamp or headstone, **the whole party travels**, led by the host (B1 travel,
`travel.md`), so nobody is left alone in a world they cannot reach.

### 3.1 Gates [C]

* **World lamp object** `0x12F5C40`: byte `+0x49` = session peer count > 0 or presentation count
  > 1 (`0x12F8303..0x12F8323`); while it is set the action-button row is 90000 (no lamp use), for
  the host as well as for guests. That is why even the host cannot rest with guests present.
* **Common event 7200** `ワープOBJ_拠点へのワープ_XX` (lamp -> Dream, one instance per lamp): index 0
  `1003[6] [0, 1]` = end if client (multiplayer state 1); then wait for the lamp's flag, clear it,
  turn, SFX, `2003[49]` warp (-> `0x13CDF30(id)`).
* **Headstones** t210000-t210003 (and the warp menu t210721): their root state machine parks in an
  empty machine while talk test 52 `IsMultiplayerInProgress()` is 1 (case `0x203C505`: session
  presentation count > 1 or peer count > 0) - for the host too - and in "client wait" while
  `IsClientPlayer()` is 1.

### 3.2 Lift [C, runtime pending]

* `0x12F831C 85 c9 0f 9f c1 08 c1` -> `31 c9 90 90 90 90 90` (party mode): `+0x49` = 0, so lamps
  are usable with guests present (host and guests).
* EMEVD rule `7200:1003:6@0` (party mode): a guest's lamp event no longer ends at once; on the host
  `1003[6]` (end if client) never ends anything, so the rule is a no-op there.
* Value filter at `0x203C564` (`or cl, al; movzx eax, cl`, then `jmp 0x203EECF`): for the headstone
  scripts IsMultiplayerInProgress answers 0 in party mode; IsClientPlayer answers 0 for them on a
  guest in the Dream.
* **Guest warp -> host request.** `0x13CDF30` is now replaced (it was a pre-hook): our own replays
  pass through; any other call on a guest that is a client in the host's world is consumed and
  becomes EVENT `lamp_request {id, seq}` to the host (sent from the director tick).
* **Host.** `on_event` (host side) parses and checks the request (`id` must be a ReturnPointParam
  row, `TravelLampKnown`); the director tick runs `0x13CDF30(id)` itself when the host is in the
  world and not loading, at most once per 10 s. The host's own warp is captured by the travel funnel
  hook as usual (TRAVEL broadcast), every guest replays it and is summoned again: one party travel.
  No loop: the host's warp is not intercepted (host role), the guests' follow warps are our replays.
* Rest: travelling to the Dream is the rest (the host's load refills; guests get the phantom
  refill on the host's "rested" event, `phantom_limits.md` 1.2).
* Opt-in: `BB_PARTY_GUEST_LAMPS=1` (it also changes the host's lamp gate); default off until a
  live run confirms 3.3.

### 3.3 Still open [I]

* `+0x4B` (`0x191B6E0`): another character whose action target is the lamp (presumably the other
  player standing at it) still blocks the prompt; the lamp's chr-type range branch
  (`(type | 8) == 8` at the interaction distance code) may give a guest a different range.
* Whether the guest's lamp interaction itself sets the lamp's flag on the guest (ObjAct `0x2D`
  replication may also start the host's own 7200 instance - then the host warps natively and the
  guest's request is dropped by the 10 s gate).
* Headstone warp menu: confirm that the selection reaches `0x13CDF30` (shadp2p: menu travel does).

## 4. Safety

* Guest actions only change the guest's own record and own save (section 1); the host's game
  never sees a menu transaction. Talk flags: overlay (discarded) or the guest's own 6000-8999.
* Excluded talkers (Gehrman's ending, travel menus, chalice altars) keep the vanilla client wait.
* The filter reads the talker id from the environment the game just dereferenced; no allocation,
  no lock (main thread).

## 5. Runtime evidence

Live 2-instance runs (loopback party, the test save parked in the Hunter's Dream, guest summoned
into the host's Dream), guest log `Party guest services:` [R]:

* `open: guest in the host's m21_00_00_00; level 37, echoes 0, Insight 6` - the gate opens for the
  client in the host's Dream and closes during loads (`closed (role client, map m21_00_00_00,
  loading)`).
* Discovery: every Dream talk script near the guest asks IsClientPlayer through the filter, with
  its id at `env+0x10`: t210000-t210002 (headstones), t210304 (Doll), t210691 / t210694 /
  t210695 (messengers), t210721; `+0x8` holds the script table (210304, 210305, 210306, 210693 ...).
* `talk scripts asked IsClientPlayer 270-285 times (answered 0)` per second - the allowed scripts
  leave their client wait; `headstones asked IsMultiplayerInProgress ~90 times (answered 0)` with
  the lamp part on.
* **Bath Messenger purchase (t210691) [R]**: the fixture stands the guest 1.2 m in front of the
  messenger (talk table lookup, +0xC0 / +0xD0), the event-driven script talks, waits for the talk
  menu ('Purchase' 15000140 looked up), selects it, sees the shop's menu request, buys the first
  row: `PURCHASE on the guest: echoes 20000 -> 19460` (a Blood Vial for 540: vials 15 -> 16), the
  guest's game saved 3 s later, and the guest's party save (`savedata_party/.../userdata0000`)
  holds level 37 / echoes 19460 at the record (+0x86E8 / +0x86EC); the host's save keeps its
  echoes 0. Screenshots: `agents/guestsvc/evidence/bath_messenger_purchase` (the "Buy" list with
  the cursor on Blood Vial 540, the quantity box).
* **Insight shop purchase (t210698) [R]**: same script (`warp=talk:210698;buy`): the Insight
  messenger's Purchase list opened on the guest and the first row was bought: `Insight 6 -> 5`;
  after the session the guest's party save holds Insight 5 (+0x86DC), the host's save Insight 6.
* **Doll level up [R]**: with 2.4-2.6 in place (`echoes=20000;warp=doll;levelup`): talk, her
  lines, "Channel the Blood Echoes", the kneel and her gesture (12100112 / 12100113 rewritten),
  then talk command 31 OpenSoul and the LevelUp menu factory 0x1BB7F60 run on the guest; the
  Level Up screen shows the guest's own record (level 37, echoes 20000, Insight 6). Down to
  Endurance, Right (15 -> 16, "Required 8634"), Cross, the dialog "Spend 8634 Blood Echoes and
  level up?" (cursor on NO: Left to YES, Cross): `LEVEL UP on the guest: level 37 -> 38, echoes
  20000 -> 11366`. After the session the guest's party save holds level 38 / echoes 11366; the
  host's save keeps level 37 / echoes 0. Screenshots: `agents/guestsvc/evidence/doll_level_up`.
