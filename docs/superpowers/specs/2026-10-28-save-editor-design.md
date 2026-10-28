# Launcher save editor - design (CL-a6970c, 0.7.7, phase 1: research + design)

Status: design only, nothing implemented. Branch agent/saveeditor.

## Decisions (user, 2026-10-28)

1. Item catalogue: EVERYTHING. Cut content, unobtainable items and over-normal values are allowed behind one "may crash or break your save" confirm per session plus the automatic backup; the default list shows obtainable items first.
2. Party saves: FULL editing, with a warning that shared progress follows the host in a party; the repair tools are included.
3. Level: LOCKED to the stat formula (read-only, recomputed).
4. Item names are read from the PLAYER'S OWN GAME FOLDER at first use and cached under the user's bbport data folder; never shipped (no game text in the release; `gen_names.py` becomes the runtime loader `names.py`, no committed items_en.json).
5. Reference credit: Noxde's Bloodborne-save-editor (GPL-3.0) in CHANGES/README; no code copied.
Rule: every offset written must be pinned from test saves first; anything unverified stays read-only in the UI.

## 1. Research

### 1.1 Existing Bloodborne save editors

| Tool | Platform | Features | Licence |
|---|---|---|---|
| **Noxde/Bloodborne-save-editor** (Rust + Tauri + React; GitHub, ~80 stars) | desktop (releases); shadPS4/PC saves directly, PS4 saves after decryption | auto backup on open; item list with filters; change amounts; add items; transform item/weapon/armour into another (incl. cut content); stats; Echoes, Insight, gender, origin, voice, position; gems and runes; weapon + armour gem slots; kill/revive bosses; set flags; change spawn lantern | GPL-3.0 |
| Bloodborne Save Manager (Nexus mod 256, Chaoskiller) | shadPS4 | profiles + savestates only (no editing) | not checked |
| PS4 cheat tools (Save Wizard, Apollo cheats) | PS4 | patch-style cheats (echoes, items), not a structured editor | proprietary |
| Cheat Engine tables | live game | not save editors | n/a |

The web search found no other structured desktop Bloodborne editor; Noxde's is the one the community means by "the Bloodborne save editor".

**Parity target: Noxde's editor.** It is the only complete one, it works on the same unencrypted layout the port writes (shadPS4-compatible), and our repo already ported its parse path (`tools/party/editor_check.py`; README credits it). It does not cover storage (chest) edits, Chalice data, NG cycle, covenant or appearance, so those are not required for parity; we add NG cycle (already decoded) and storage only if the layout proves simple.

Licence handling: GPL-3.0 and bbport is GPL-compatible, but we do NOT copy its code. Only layout facts (offsets, record shapes) are learned, and each is re-verified by diffing our own test saves in phase 1 and recorded in `docs/party/save_layout.md` (new) with the evidence.

### 1.2 Our own save knowledge (verified, reusable)

- Saves: `<install>\user\savedata\1\<TITLEID>\SPRJ0005\userdata0000..0009` (character slots, 0x140000 bytes, version 0x41), `userdata0010` (game data), `SPRJ0005.sce_sys\param.bin` (+ `sce_sys\param.sfo` inside the folder). Party sessions play from `user\savedata_party\...` (same layout). The port's saves are plain: no checksum or signature is known or needed (Noxde edits them with none; our `scripts/save_editor.py` import writes edited slots back and the game loads them). The size, version and header consistency must be kept (already enforced there).
- Header (`tools/party/save_decode.py`, `docs/party/save_safety.md`): u32 version, map id, play ms, then {size,end} section table; event flag section 150001 bytes (1200 groups x 125 + 1), group key formula and bit `0x80 >> (id%8)` in `save_decode.Save.flag/set_flags`; NG cycle block at header +0x2C (u32 NG cycle, u32 ending type); world-state blocks CHR/OBJ/DECL(LCED) at header +0x3C.
- Character fields relative to the UTF-16 name (graft note, verified 2026-10-25): HP/maxHP -0x94, Insight -0x24, level/Echoes -0x18/-0x14, remembered chr type -4 (1 = summoned-guest phantom = the soft-lock mark in own world offline; repair = set that u32 to 0). Name = section end - size - 0x10c91 (Noxde: FACE - 34028 - 469). The six stats sit in the same PlayerGameData run: exact offsets to be diffed from test saves in phase 1.
- Inventory: three lists written by 0x1AC5D10: normal (count + 1984 x {handle, id (category<<28|id), count, index}), saved key list (64), summoned key list (64); empty = {0, 0xffffffff, 0, 0}. `Save.key_sections()` / `duplicate_keys()` exist. Storage (chest) layout: not decoded yet. Weapon level = id + level (shop-gate note: id + 100*level? to verify; 0-15). Gem slots: Noxde scans 40-byte records from file 0x54 (to verify).
- Party effects on saves: separate folder; a guest save written in the host's world has empty world-state blocks (no LCED; `scripts/save_editor.py` export/import handles it); per-slot side files `party_characters.txt` (name hash + play time), `party_rot.txt`, `party_stain.txt`, `party_prologue.txt`, `party_gestures.txt`, `party_summoned_gives.txt`, `party_owed_lots.txt`. Renaming without updating `party_characters.txt` drops the slot's party state (save_editor.py handles it). Shared progress (lamps, bosses, quests, key items) is meant to be equal across members, so a flag edit on one member desyncs from the party (risks).
- Backups that exist: game rotation `user\save_backups\<folder>\<date>-<reason>\` (party sessions), `party_sessions\<time>-host|guest\`, `...\<time>-before-editor-import`. The editor adds `before-edit` in the same tree so the README restore steps apply.
- Code to reuse: `scripts/save_editor.py` (backup, temp+rename write, manifest, party_characters sync), `scripts/party_saves.py`, `scripts/party_session.py` (describe, hash compare), `launcher.py` `running_instances()` / `blocked_by_running_instance()` (game-running guard), `tools/party/flag_tool.py` (`read_maybe_dcx`, `bnd4`, `load_params`, `load_inc`, `categorize`).
- Which launcher: the real Windows one is `launcher.py` at the repo root (5843 lines, ttk.Notebook, `_tab`/`_section` helpers). `launcher/bbport_launcher.py` (1428 lines) is the Linux/Flatpak variant (bb-launcher.sh, .desktop); out of scope for v1.

### 1.3 Names data

- Game data `dvdroot_ps4`: `param\gameparam\gameparam.parambnd.dcx` + `paramdef\paramdef.paramdefbnd.dcx` (EquipParamWeapon / Protector / Accessory / Goods / Gem), names in `msg\<lang>\item.msgbnd.dcx` (FMG v2; entries are Japanese-named: buki = weapon, bougu = armour, aitemu-mei = goods; loader recipe in the graft note "Look up Bloodborne item lot id or goods id", scratch scripts `scratch\item_lookup.py` / `bbdata.py`, not in the repo).
- Plan: maintainer tool `tools/saveedit/gen_names.py GAME` writes `tools/saveedit/data/items_en.json` (id, category, name, goodsType, max stack, weapon max reinforce, gem category/shape, slot counts). Runtime falls back to ids when a name is missing. Other languages via `msg\<lang>` later. See open question 4 about shipping vs reading the user's own game folder.
- Flag names: `gpu/shim/party/party_flags.inc` (boss_defeated, lamp_unlocked, shortcut_door, key_event, cutscene_seen, item_lot_picked, ...) via `flag_tool.load_inc/categorize`; `docs/party/event_flags.md`, `quest_audit.md` for NPC quest steps; `tools/party/item_tool.py` for lot flags.

## 2. v1 feature list

V1 = in v1, V2 = later, NO = not offered.

- **Character (V1)**: name (UTF-16, party_characters synced), level (read-only, recomputed from the stats with the game's formula), the six stats clamped 1..99 with live level readout, HP/stamina refilled to max, Blood Echoes, Insight. V2: play time, gender/origin/voice, position. NO: appearance (face data).
- **Inventory (V1)**: consumables / materials / key items / runes with names; change quantity (clamped to the param maximum); add from a searchable catalogue; remove; duplicate-key warning. V2: transform item into another.
- **Equipment (V1/V2)**: weapons: reinforcement level (clamped to the param's max); add / remove weapons and armour pieces in the inventory (obtainable catalogue). Gem and rune slots per weapon/armour with selectors filtered by slot shape: V1 only if the record layout verifies, otherwise V2. V2: equipped slots, caryll rune slots.
- **Storage (V2)**: the chest (read-only in V1 if the layout is found).
- **World / flags (V1)**: sub-lists Bosses, Lamps, Shortcuts/doors, NPC quests (steps), Chalice (read-only); search; set/clear a flag; "Revive boss" clears the kill flag plus its companion flags from the table; "Unlock all lamps". V2: spawn lantern. NO: Chalice dungeon data edits.
- **NG cycle (V1)**: show / set the cycle (header +0x2C), warns that bosses are not reset.
- **Party (V1)**: slot list marks party saves; findings with one-click repair: phantom mark (summoned-guest chr type 1 -> 0), duplicate key stacks (merge), party_characters.txt mismatch. Never offered: deleting party side files; flag edits on party saves default to set-only with a banner.

## 3. UI sketch (tkinter, launcher style)

New top-level tab "Save editor" in `launcher.py` (`self.notebook`, `Card.TFrame`, `Secondary.TButton`, `Tooltip`); page class in `launcher_saveedit.py`, imported lazily like `save_editor_module()` so a missing file only disables the tab.

```
Save: (o) Single-player ( ) Party   Slot: [0  visy  L37 v]  [Open]  [Backups / Restore...]
[ Character | Items | Weapons & gear | Progress | Party ]
Character: Name [____]   Level 37 (calculated)
           Vigor [-][25][+]  Endurance ...  (six stats)   Echoes [..]  Insight [..]   NG cycle [0]
Items:     Search [______] [All | Consumables | Materials | Key | Runes]
           list: Name | Count    [Set count] [Remove]   [Add item... (catalogue dialog with search)]
Weapons:   list: Name | +Level | gem slots (dropdowns, filtered)   [Add weapon...]
Progress:  Bosses | Lamps | Shortcuts | NPCs | Chalice  - checklists + search, [Revive boss]
Party:     findings list, each with [Repair]
Bottom:    [Apply changes]  [Revert]   status: "Backup: ...\before-edit\..."
```

Validation: stat range with live level, counts within the param max, name length, Apply disabled with no change, a "Changes" summary dialog (from `SaveModel.diff()`) before the write, tooltips on every control.

## 4. Safety

1. Refuse while a game of this install runs (`running_instances()`), re-checked right at Apply.
2. Backup before every write: whole save folder to `user\save_backups\<savedata|savedata_party>\<YYYYMMDD-HHMMSS>-before-edit\` with SHA-256 manifest (as `save_editor.import_save`). "Backups..." lists them (slot, level, time) with **Restore** (backs the current state up first, like Undo last party session, then swaps). Keep the last 20 editor backups.
3. Write path: the model patches only the fields it understands inside a bytearray of the slot (never rebuilds the file); temp file + `os.replace`; then **write-verify**: re-read, re-parse, assert the edited fields equal the intent and the byte diff against the backup is exactly the edited ranges; on mismatch restore from the backup and report.
4. Never touch `userdata0010` / `param.bin` other than the `party_characters.txt` sync `scripts/save_editor.py` already does.
5. Open validates size, version, header (`save_decode.Save` + `editor_check` checks); unknown layout opens read-only.
6. Tests never use live saves: copies under `E:\games\bbport\test_saves` (default `savedata`, a guest-written phantom save; `ng_plus_guest` is private and never committed) plus synthetic saves built in the test; the test helpers refuse any path under `E:\games\bloodborne\user`.
7. Unit tests `tests/test_saveedit.py`: no-change round trip byte identical; each edit changes only its range; flags vs `save_decode`; level formula vs stored levels on all test saves; inventory add / remove / overflow / handle uniqueness; phantom repair; write-verify failure restores; game-running refusal; party_characters sync.
8. Visual check (rule): load an edited copy in a test instance (solo and party) and screenshot the stats and inventory screens.

## 5. Module layout

```
tools/saveedit/            pure Python, no tkinter (shipped beside scripts\)
  model.py     SaveModel: open, get/set character, inventory, gems, flags; diff(); to_bytes()
  layout.py    every offset/anchor in one place, each with a source comment and asserts
  flags.py     flag id <-> group/bit (shares save_decode), category tables from party_flags.inc
  items.py     catalogue loader (data/items_en.json), search, limits
  backup.py    backup, list, restore, game-running guard, atomic write, write-verify
  party.py     phantom mark, duplicate keys, party_characters sync (reuses scripts/save_editor)
  gen_names.py game data -> data/items_en.json (maintainer tool)
launcher_saveedit.py       tkinter page
tests/test_saveedit.py
docs/party/save_layout.md  verified layout + evidence
```
`tools/party/save_decode.py` stays; `model.py` imports its flag logic (no second copy).

## 6. Phased plan

0. This design.
1. Layout verification: diff test saves with known level / stats / items to pin stat, equipped, gem, storage and spawn offsets; write `save_layout.md`; `gen_names.py` + catalogue. Gate: the parser reads all 43 saves on this PC and agrees with `save_decode`.
2. `model / backup / flags / party` + tests (character, flags, counts, phantom repair, NG). No UI.
3. Launcher tab: Character, Items, Progress, Party, backups / restore; visual check; release-notes line.
4. Weapons, gem slots, add / remove weapons and armour (riskiest). V2 items after.
5. Smoke: edited copies load in a test instance (solo and party), restore tested; add to the full smoke.

## 7. Risks

- Layout drift: stat / inventory / gem offsets are anchor-derived; a wrong anchor writes garbage. Mitigation: layout asserts, unknown layout = read-only.
- Invalid items (cut content, wrong category, stack over max, weapon level over its max, gem in a wrong slot shape) can crash or soft-lock: catalogue-only items, clamps, slot filters, cut content behind an advanced toggle.
- Inventory entry handle / index fields must stay unique; adding items needs a free-slot allocator mirroring the game's.
- Party saves: shared progress is host-driven, so flag / item edits on one member desync or get overwritten at the next join; key-item double stacks reproduce the known duplicate-key bug; a rename drops party state unless party_characters is synced.
- The post-session save check / "Undo last party session" could read edits as loss (level down, flag cleared) and offer to revert them; the editor must refresh the newest snapshot baseline or note the edit in it.
- Cheating-adjacent when used online (Noxde carries a disclaimer): add a short note, nothing blocking.

## 8. Open questions for the user

1. Scope of cheats: allow adding any item incl. cut content / unobtainable weapons and 99 stats, or limit v1 to the obtainable catalogue?
2. Party saves: allow flag / item edits in v1, or only the repair tools (phantom mark, duplicate keys, party_characters) with full editing for single-player saves?
3. Level: locked to the stat formula (recommended), or also typeable?
4. May the release ship the generated English name catalogue (game text, ~300 KB), or must names be read from the user's own game folder at runtime?
