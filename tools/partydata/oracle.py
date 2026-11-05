#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Dev-only test oracle: the party game-data tables derived in Python (not shipped, not run by the game).

The engine derives the tables in C++ (gpu/shim/gamedata/). This is the slow reference implementation the C++
output is compared with (tests/test_party_tables.py): same sections, same cache text, no key.

  python -I -X utf8 tools/partydata/oracle.py GAME_DIR OUT_FILE
"""
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def game_root(path):
    p = Path(path)
    return p / "dvdroot_ps4" if (p / "dvdroot_ps4").is_dir() else p


def derive_all(game):
    """{section: [row]}: every table, from the player's own files."""
    import ids_tool
    import item_tool
    import npcfog_tool
    import warper_tool
    sections = {}
    item_sections, _audit = item_tool.derive(game)  # first: ids_tool changes flag_tool's marker table for a moment
    sections.update(item_sections)
    id_sections, _npcs = ids_tool.derive(game)
    sections.update(id_sections)
    sections["npcFog"] = npcfog_tool.derive(game)
    sections.update(warper_tool.derive(game))
    return sections


def render(key, sections):
    lines = ["BBPARTYTABLES 1", f"key {key}"]
    for name in sorted(sections):
        rows = sections[name]
        lines.append(f"[{name}] {len(rows)}")
        for r in rows:
            lines.append("\t".join(r))
    return "\n".join(lines) + "\n"


def main():
    game, out = game_root(sys.argv[1]), Path(sys.argv[2])
    t0 = time.time()
    text = render("oracle", derive_all(game))
    out.write_bytes(text.encode("utf-8"))
    print(f"oracle: {time.time() - t0:.1f} s -> {out}")


if __name__ == "__main__":
    main()
