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

Adding a table (the flag category table is next): derive it in `scripts/partydata/`, add its section to
`derive_all` in `party_tables.py`, parse it in `Parse` of `party_gamedata.h`.

## Tests

Tests that need real tables build a cache from the agent clone's game folder and skip without it:
`python -I scripts/party_tables.py <game dir> --user-dir <dir>`, then `BB_PARTY_TABLES=<dir>/party_tables.txt
out/gpu/<test>.exe` (`tests/party_tables_test.h`). `party-gamedata-test` checks the parser with made-up numbers;
`tests/test_party_tables.py` and `tests/test_item_audit.py` run the derivation on `BB_GAME_DIR` (else `../game`).
The item audit (`scripts/partydata/item_tool.py audit GAME --out FILE`) is written only where asked, never into the repo.
