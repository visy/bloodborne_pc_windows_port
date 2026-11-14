### Added
- **Category tabs in the community Chalice glyph list**: the Ritual Altar's glyph list now has tabs for Blood gems, Equipment, Farming and Testing (last), switched with Left / Right on the keyboard or L1 / R1 on a pad (D-pad Left / Right or PageUp / PageDown jump ten rows). Blood gems are sorted by gem type (radial, triangle, waning, circle).

### Fixed
- **"No dungeons found" after a slow pick**: choosing a glyph from the altar's list more than 20 seconds after opening the box did nothing and the game answered "no dungeons found using the given Chalice Glyph". The pick now counts however long you take.
- **Crash when entering some community glyphs**: a list entry whose chalice does not exist in the game's data (checked against the loaded game data, nothing is bundled) is left out of the list, and a pick that fails the check is refused instead of being saved into a dungeon slot that crashed on every load.
- Party smoke phase `cdl` (30 s wait in the glyph box, a non-6100 pick, a tab switch).
