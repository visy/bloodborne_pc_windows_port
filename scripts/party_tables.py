#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the party co-op tables from the player's own game files (run by run.bat / run.sh).

The party code needs lists that come from the game's data: which maps, lamps, cutscenes and NPCs exist,
which item lots carry a pickup flag, which awards are host-only, the Beckoning-Bell helpers, the world
warper's gear. None of it is shipped: this script derives it from the game folder the game is started
from (the mods overlay view included, so mods are respected) and writes one cache file,
<user dir>/party_tables.txt, that gpu/shim/party/party_gamedata.h loads when a party starts.

  python -I scripts/party_tables.py GAME_DIR --user-dir USER_DIR [--force] [--quiet]

GAME_DIR holds dvdroot_ps4 (or is dvdroot_ps4). The cache carries a key made of the game inputs (params,
event scripts, map and cutscene listings, MSB sizes and edges) and of the derivation code; it is rebuilt
when the key changes, the first start taking about a minute. Exit status: 0 = cache current or rebuilt,
1 = the game files could not be read (the party features that need the tables then refuse, and say so).

Cache format (text, UTF-8): `BBPARTYTABLES <version>`, `key <sha1>`, then per section `[name] <rows>` and
that many lines of tab-separated fields (docs/party/gamedata.md). Section names:
  maps returnPoints entities remos npcMirror           one u32 per row            (ids_tool)
  returnSpots warpSpots                                 id x y z yaw               (ids_tool)
  npcFog                                                npc join leave host region x y z yaw  (npcfog_tool)
  warperRight warperLeft warperArmor                    one u32 per row            (warper_tool)
  itemLotFlag itemLedger itemAwardSpot itemUnshared itemSharedTreasure itemBossDrop itemRequireShared
  itemPerPlayer itemKillAward                           lot rows                   (item_tool)
"""
import argparse
import hashlib
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "partydata"))

FORMAT = 1
CACHE_NAME = "party_tables.txt"
EDGE = 65536


def game_root(path):
    p = Path(path)
    return p / "dvdroot_ps4" if (p / "dvdroot_ps4").is_dir() else p


def _hash_file(h, path, full):
    try:
        st = path.stat()
        h.update(f"{path.name}\0{st.st_size}\0".encode())
        with open(path, "rb") as f:
            if full or st.st_size <= 2 * EDGE:
                while chunk := f.read(1 << 20):
                    h.update(chunk)
            else:
                h.update(f.read(EDGE))
                f.seek(-EDGE, os.SEEK_END)
                h.update(f.read(EDGE))
    except OSError as e:
        h.update(f"{path.name}\0missing {e.errno}\0".encode())


def inputs_key(game):
    """sha1 of what the tables are derived from: params and event scripts in full, MSBs by size and
    both edges, the map / remo listings, and the derivation code."""
    h = hashlib.sha1()
    h.update(f"format {FORMAT}\0".encode())
    for p in sorted((HERE / "partydata").glob("*.py")) + [Path(__file__)]:
        h.update(p.read_bytes())
    for rel in ("param/gameparam/gameparam.parambnd.dcx", "paramdef/paramdef.paramdefbnd.dcx"):
        _hash_file(h, game / rel, True)
    ev = game / "event"
    for p in sorted(ev.glob("*")) if ev.is_dir() else []:
        if p.is_file():
            _hash_file(h, p, True)
    ms = game / "map" / "mapstudio"
    for p in sorted(ms.rglob("*.msb*")) if ms.is_dir() else []:
        _hash_file(h, p, False)
    mp = game / "map"
    for name in sorted(os.listdir(mp)) if mp.is_dir() else []:
        h.update(name.encode() + b"\0")
    rm = game / "remo"
    for name in sorted(os.listdir(rm)) if rm.is_dir() else []:
        h.update(name.encode() + b"\0")
    return h.hexdigest()


def cache_key(path):
    try:
        with open(path, encoding="utf-8") as f:
            if f.readline().strip() != f"BBPARTYTABLES {FORMAT}":
                return None
            line = f.readline().strip()
            return line[4:] if line.startswith("key ") else None
    except OSError:
        return None


def derive_all(game):
    """{section: [row]}: every table, from the player's own files."""
    import flag_tool as ft
    import ids_tool
    import item_tool
    import npcfog_tool
    import warper_tool
    sections = {}
    item_sections, _audit = item_tool.derive(game)  # first: the others change flag_tool's marker table for a moment
    sections.update(item_sections)
    id_sections, _npcs = ids_tool.derive(game)
    sections.update(id_sections)
    sections["npcFog"] = npcfog_tool.derive(game)
    sections.update(warper_tool.derive(game))
    assert ft  # imported for its side effect on sys.path only
    return sections


def render(key, sections):
    lines = [f"BBPARTYTABLES {FORMAT}", f"key {key}"]
    for name in sorted(sections):
        rows = sections[name]
        lines.append(f"[{name}] {len(rows)}")
        for r in rows:
            for field in r:
                if "\t" in field or "\n" in field:
                    raise ValueError(f"{name}: tab / newline in a field")
            lines.append("\t".join(r))
    return "\n".join(lines) + "\n"


def build(game, out, force=False, quiet=False):
    say = (lambda *_: None) if quiet else (lambda m: print(m, flush=True))
    game = game_root(game)
    if not (game / "param" / "gameparam" / "gameparam.parambnd.dcx").is_file():
        say(f"party tables: no game data under {game}; the party features that need them are off")
        return 1
    key = inputs_key(game)
    out = Path(out)
    if not force and cache_key(out) == key:
        return 0
    say("party tables: reading your game files (first start and after game / mod changes, about a minute)...")
    t0 = time.time()
    try:
        sections = derive_all(game)
        text = render(key, sections)
    except Exception as e:  # a damaged / unexpected file: the game starts, the party tables are off
        say(f"party tables: could not be built ({type(e).__name__}: {e}); the party features that need them are off")
        return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    from safe_write import write_text
    write_text(out, text, encoding="utf-8", newline="\n")
    say(f"party tables: done in {time.time() - t0:.0f} s ({sum(len(v) for v in sections.values())} rows) -> {out}")
    return 0


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--user-dir", required=True)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    return build(a.game, Path(a.user_dir) / CACHE_NAME, a.force, a.quiet)


if __name__ == "__main__":
    sys.exit(main())
