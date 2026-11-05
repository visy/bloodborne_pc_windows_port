# Party game-data tables (read from the player's own game)

No FromSoftware data is committed. The party code still needs lists that only the game's files know: which maps,
lamps, cutscenes and NPCs exist, which item lots carry a pickup flag, which awards are host-only, the Beckoning-Bell
helpers, the world warper's gear. The engine derives them in C++ on the player's machine, as it starts, and caches
them. No Python runs at game start (a launcher that starts the engine directly and deletes `scripts\`, like
Nightwatch's `bb-start.exe`, gets the tables too).

## Flow

1. `src/probe.c` calls `bbgpu_party_data_start(app0, user_dir)` right after the file system is configured
   (`gpu/shim/gamedata/build.cpp`, `StartBackground`). `app0` is the game view `run.bat` / `run.sh` made (mods overlay
   merged), so mods are respected: the files read are the ones the game reads. `BB_PARTY_TABLES_BUILD=0` skips the
   build; `BB_PARTY_TABLES=<file>` names a ready cache and skips it too.
2. The key is a hash of the derivation version (`kDerivationVersion` in `build.cpp`: bump it when a rule changes
   what the tables hold), and of the name, size and modification time of the params, the paramdef, every event
   script, every MSB, plus the `map` / `remo` listings. No file is read for the key (the old Python key read the
   scripts in full and took 2 s). Key equal to the one in `<user dir>/party_tables.txt`: the file is parsed and
   done (milliseconds). Otherwise the tables are derived, the file is written atomically, and the text is parsed
   into the tables the game serves (so a fresh build and a cached start give the same numbers). The cache keeps the
   text format of the Python era (`BBPARTYTABLES 1`): readable, diffable against the oracle, no second format.
3. A pool of `cores - 1` threads (`BB_PARTY_TABLES_THREADS` overrides; below-normal priority) runs per-file work in
   parallel: one task per event script (read, DCX, parse), per MSB (the heaviest, started first) and for the
   params; then the event expansion per file (parallel), the calls into `common.emevd` gathered in file order and
   `common` expanded last; then the four derivations (items, ids, warper, NPC fog) in parallel. Results are stored
   by index and merged in the sorted file order, so the output never depends on scheduling.
4. Progress: console lines `[party-data] <step>: NN%` (steps: file list, params / events / maps, expanding event
   scripts, deriving the tables) and `[party-data] done in N s: <per step timings>`. Percent is weighted by input
   bytes. `BB_PARTY_DATA_TRACE=1` prints every step's wall time.
5. `gamedata::Get()` (`gpu/shim/party/party_gamedata.h`) waits for the build when one runs; `Preparing()` /
   `PreparingPercent()` never block. Starting a party while it runs raises the notice "Preparing co-op data from
   your game files: NN%" (`party_status_bridge.cpp`); a build that failed raises "Party game data missing" once it
   ended. No build (tests, a named cache): `Get()` parses the cache file the first time a table is asked for. No
   tables or a damaged file: every table is empty, the reason is logged once and the consumers say no (an unknown
   map, lamp or entity id is refused, no lot is shared, no NPC is mirrored, no bell helper is followed). Nothing
   crashes: every reader is bounds checked and a damaged file only fails the build with a message.

Measured (16 cores, 1.09 game with the DLC, 2668 event scripts, 92 MSBs): cold build about 1.8 s wall in the
background (file listing 1.25 s of that on a cold file system, read 0.25 s, expand 0.09 s, derive 0.02 s); the
Python derivation took 18.9 s on the same machine. Cache hit: key + parse, a few ms.

## Readers (`gpu/shim/gamedata/`, namespace `bbdata`)

| file | reads |
|---|---|
| `bytes.h` | bounds-checked LE / BE reads, UTF-16 to UTF-8 (`FormatError` on any out-of-range read) |
| `container.*` | DCX (Bloodborne: `DCP` + `DFLT`, a zlib stream; other compressions are refused), BND4 |
| `param.*` | PARAM + PARAMDEF (field layout, bitfields; `ParamTable::Get(row, "name")`) |
| `emevd.*` | EMEVD (version 0xCC), EMEDF (instruction definitions), EMELD (event names), argument unpacking |
| `events.*` | expansion of parameterised events per InitializeEvent call: flag operations, markers (boss defeat, item lot, cutscene, map move ...), flag reads, calls |
| `msb.*` | the entry lists of an MSB, POINT_PARAM_ST entries, the entity-id word scan |

## Sections (`derive_*.cpp`; `RenderCache` has the text format)

| section | derived by | consumer |
|---|---|---|
| maps returnPoints entities remos returnSpots warpSpots | `derive_ids.cpp` `DeriveIds` | `party_ids.h` |
| npcMirror | `DeriveIds` | `party_npc_mirror.h` |
| npcFog | `DeriveNpcFog` | `party_npcfight_rules.h` |
| warperRight warperLeft warperArmor | `DeriveWarper` | `world_warper_rules.h` |
| item* (lot flags, ledger, award spots, unshared, shared treasure, boss drops, audit rows, kill awards) | `derive_items.cpp` + the flag category table (`derive_flags.cpp`, `flag_category_rows.inc`) | `party_items.cpp` |

The ledger bit of a flagless award is `60009000 + index` in the player's own save, so an index must never change:
`kLedgerLegacy` keeps the indices the releases up to 0.7.9 gave out (our own flag allocation) and a new lot takes
the next free one from 43.

Adding a table: derive it in a new `derive_*.cpp` (or extend one), add it to `DeriveAll` in `build.cpp`, parse it in
`Parse` of `party_gamedata.h`, bump `kDerivationVersion`, and add it to the Python oracle if it has a counterpart.

## Tests

* `gamedata-test` (`tests/test_gamedata.cpp`): the readers and the derivation on synthetic data built in the test
  (DCX, BND4, PARAM + PARAMDEF, EMEDF / EMEVD with a parameterised event, MSB, a synthetic game folder, a param
  edit that the next build follows, cache reuse, no game data). `cmake --build out/gpu --target gamedata-test`.
* `party-gamedata-test` checks the cache parser with made-up numbers.
* `tests/test_party_tables.py` builds the tables with `party-tables-cli` (`tests/party_tables_cli.cpp`) and with the
  Python oracle (`tools/partydata/oracle.py`, dev only, not shipped: the former derivation, kept as the reference)
  on `BB_GAME_DIR` (else the clone's `../game`) and requires the cache text to be byte-identical (the key line
  aside); a second test does the same on a temporary mod view whose param file has one pickup flag edited, and
  requires the tables to follow it. Skipped without a game folder or the built CLI.
* Tests that need real tables build a cache from the clone's game folder and skip without it:
  `out/gpu/party-tables-cli.exe <game dir> <dir>/party_tables.txt`, then `BB_PARTY_TABLES=<dir>/party_tables.txt
  out/gpu/<test>.exe` (`tests/party_tables_test.h`). `tests/test_item_audit.py` runs the Python item audit on the
  game (`python -I tools/partydata/item_tool.py audit GAME --out FILE`, written only where asked, never into the repo).

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
