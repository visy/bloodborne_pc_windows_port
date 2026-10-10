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

## 3. Lamps (design; next step)

## 4. Safety

* Guest actions only change the guest's own record and own save (section 1); the host's game
  never sees a menu transaction. Talk flags: overlay (discarded) or the guest's own 6000-8999.
* Excluded talkers (Gehrman's ending, travel menus, chalice altars) keep the vanilla client wait.
* The filter reads the talker id from the environment the game just dereferenced; no allocation,
  no lock (main thread).

## 5. Runtime evidence
