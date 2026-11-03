# The in-game map's data (Hunter's Atlas)

`generator/` is Hunter's Atlas's map generator by MikeTat1988 (github.com/MikeTat1988/HuntersAtlas, commit 06d4021,
GPL-2.0-or-later), unchanged: `src/` is its `source/generator/src`, `generate_atlas.spec` and the `requirements-*.txt` pins
are its own. It reads the game's collision, map and menu files and writes area geometry (`maps/<area>.json`), lamp data
(`features.json`) and names. Nothing made from the game's files is committed or shipped.

* **A player's PC**: `scripts/map_prepare.py` (the launcher's *Prepare the map*) runs the frozen generator
  (`map_generator\generate_atlas_lite.exe`, shipped in the release) on the player's game folder, then
  `scripts/bbmap_bake.py` packs the JSON into `user\map\<area>.bbm` + `map.stamp`; the game reads those
  (`gpu/shim/map/map_core.h` describes the format).
* **Building the frozen generator** (release step): `python tools/map/build_generator.py --python VENV\Scripts\python.exe`,
  the venv a CPython 3.12 with `generator/requirements-frozen.txt` and `generator/requirements-build.txt` installed.
  The result (`out/map_generator`) is copied into the release next to `scripts\`; it carries every bundled library's licence
  and Soulstruct's source archive. PyInstaller notes are in `build_generator.py` (SciPy >= 1.16 needs `scipy.spatial` collected
  whole; the self test `generate_atlas_lite.exe --self-test-imports` fails the build when an import is missing).
* **Development without freezing**: `python -I scripts/map_prepare.py --game-dir GAME --out-dir DIR --python VENV\Scripts\python.exe`
  (the venv with `generator/requirements-runtime.txt`) runs the generator's source.
* The generator accepts the game's 1.09 `eboot.bin` (a fixed file hash) and takes about 10 minutes.
