# Party game-data tables (read from the player's own game)

No FromSoftware data is committed. The party code still needs lists that only the game's files know: which maps,
lamps, cutscenes and NPCs exist, which item lots carry a pickup flag, which awards are host-only, the Beckoning-Bell
helpers, the world warper's gear. They are derived on the player's machine and cached.

## Flow

1. `run.bat` / `run.sh` merge the mods overlay into the game view, then run `scripts/party_tables.py <game view>
   --user-dir <user dir>` (`BB_PARTY_TABLES_BUILD=0` skips it). Mods are respected because the view is what the game
   itself reads.
2. The script computes a key (params and event scripts in full, MSBs by size and both ends, the map / remo listings,
   and the derivation code). If `<user dir>/party_tables.txt` already has that key, it stops (about 2 s). Otherwise it
   rebuilds (about a minute: first start, a game or mod change, a bbport update that changes the derivation) and writes
   the file atomically. A failure is printed and the game starts anyway.
3. The game parses the file the first time a table is asked for (`gpu/shim/party/party_gamedata.h`;
   `BB_PARTY_TABLES=<file>` names another cache, the test harness uses it). No file or a damaged one: every table is
   empty, the reason is logged once (`[party] game-data tables unavailable ...`) and starting a party raises the alert
   "Party game data missing". Consumers then say no: an unknown map, lamp or entity id is refused, no lot is shared, no
   NPC is mirrored, no bell helper is followed. Nothing crashes.

## Why Python, not the game

The tables need the DCX / BND4 / EMEVD / MSB readers of `scripts/partydata/flag_tool.py` (and the item rules built on
the EMEVD scan: ledger, award spots, boss drops). The game side has none of those. The param-only tables (the
world warper's gear, the lot-flag list) could be read from the loaded params; they are derived with the rest so the
whole set is one consistent snapshot of the same files.

## Sections (scripts/party_tables.py has the columns)

| section | derived by | consumer |
|---|---|---|
| maps returnPoints entities remos returnSpots warpSpots | `ids_tool.derive` | `party_ids.h` |
| npcMirror | `ids_tool.derive` | `party_npc_mirror.h` |
| npcFog | `npcfog_tool.derive` | `party_npcfight_rules.h` |
| warperRight warperLeft warperArmor | `warper_tool.derive` | `world_warper_rules.h` |
| item* (lot flags, ledger, award spots, unshared, shared treasure, boss drops, audit rows, kill awards) | `item_tool.derive` | `party_items.cpp` |

The ledger bit of a flagless award is `60009000 + index` in the player's own save, so an index must never change:
`item_tool.LEDGER_LEGACY_INDEX` keeps the indices the releases up to 0.7.9 gave out (our own flag allocation) and a
new lot takes the next free one from 43.

Adding a table (the flag categories below are the first one after these): derive it in `scripts/partydata/`, add its section to
`derive_all` in `party_tables.py`, parse it in `Parse` of `party_gamedata.h`.

## Tests

Tests that need real tables build a cache from the agent clone's game folder and skip without it:
`python -I scripts/party_tables.py <game dir> --user-dir <dir>`, then `BB_PARTY_TABLES=<dir>/party_tables.txt
out/gpu/<test>.exe` (`tests/party_tables_test.h`). `party-gamedata-test` checks the parser with made-up numbers;
`tests/test_party_tables.py` and `tests/test_item_audit.py` run the derivation on `BB_GAME_DIR` (else `../game`).
The item audit (`scripts/partydata/item_tool.py audit GAME --out FILE`) is written only where asked, never into the repo.

## Event-flag categories (`flagRows`)

`party_flags.inc` (277 KB, 2761 rows) is gone. `party_progress.cpp` reads the table from the `flagRows` section
(`first<TAB>last<TAB>category<TAB>note`, file order = match order, first match wins, no row = never_sync;
`FlagRow` in `party_gamedata.h`). A row naming a category this build does not know is skipped. Without the section
every id is never_sync, so nothing is shared (progress, doors, lamps, NPC quests, shop gates) until the tables exist.
`Rows()` is built on first use, so tests install their tables (`tests/party_tables_test.h`) before the first lookup.

Derivation, in match order (reference implementation: `flag_tool.category_rows`, prints with
`python -I scripts/partydata/flag_tool.py table GAME`; a C++ port must reproduce its rows exactly):

1. Covenant rows (`COVENANT_ROWS`, 15 ranges) and override rows (`OVERRIDE_ROWS`, 87 ranges: the area-gate flags, endings,
   cutscene / time-of-day flags, per-player Doll / messenger / badge flags). Hand rules with our own notes, no game scan.
2. Specific rows (about 1700 ranges) from the game: every EMEVD script expanded once per `InitializeEvent` call with its
   own arguments (name from the matching `.emeld`), plus ItemLotParam / ShopLineupParam / ObjActParam. Per event record
   (done flag = event id + slot for events >= 10000000), priority wins, equal priority = last write:
   boss-defeat marker (2003[12/15/53]) -> the done flag (9) and the single flags the event sets, except ids below 2100,
   5000-8999 and 9000-9451 (8) -> `boss_defeated`;
   lamp events (`ワープOBJ_起動`) -> `lamp_unlocked` 7AAB02xx (9), other 7AAB0xxx reads -> `session_runtime` (6); lamp
   registration 2009[5] -> `lamp_unlocked` (9), ladder 2009[0] -> `shortcut_door` (7); restart-on-reload events in zones
   0/1 -> `session_runtime` (3); by event name keyword (key events, door / lever / elevator words, chest / item-lot
   marker, cutscene marker, NPC words) -> `key_event` / `shortcut_door` / `item_lot_picked` / `cutscene_seen` /
   `npc_quest` (6 / 6 / 5 / 4 / 4); ItemLotParam getItemFlagId and cumulate flags -> `item_lot_picked` (2), ShopLineupParam
   eventFlag -> `never_sync` "shop stock row" (2), and each qwcId gate that has no lot, is not 5500-5521 / 6000-8999 /
   type 9 and would otherwise be item_lot / never_sync / session_runtime -> `world_state` (10). Contiguous ids of one
   category inside one 1000-flag group merge into one row.
3. Global rows (`GLOBAL_ROWS`, about 50 ranges, our notes) and the structural rows: per map of the flag store's 17-map
   table and per type 1/5/6/7/9 and zone 0-9 a default (`ZONE_DEFAULT`), the m29 layouts, chalice instance areas 41-46
   and area 99 -> `never_sync`.

The scan reads the player's own files, so a mod that changes a script or a param changes its rows.

Other consumers of the same derivation, none of them committed data:
- `scripts/saveedit/flagsdata.py` `derive(game)` (the save editor is Python): the boss kill flags (`boss_defeated` rows
  of the form 1AAB1800 / 1AAB1850, areas below 40, not 29) and lit-lamp flags (`lamp_unlocked` rows 7AAB02nn, same area
  rule), plus each boss's NPC param id (the entity named by its boss-defeat marker -> the MSB character part -> the
  param id at +8 of the part's enemy block) for the name lookup. `names.Catalogue` caches the result (`progress`) with
  the item names and the launcher hands it to `party_session.check` for the after-session save comparison. Same lists
  as the old typed ones: 19 bosses, 44 lamps (equal on vanilla).
- Tools (`save_check.py`, `save_decode.py`, `shop_gates.py`, `flag_tool.py dump / diff`) read the section from a cache
  file (`--tables`, default `BB_PARTY_TABLES` or `user/party_tables.txt`) or derive it from the game folder.
