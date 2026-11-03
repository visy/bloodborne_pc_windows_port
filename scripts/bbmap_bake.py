#!/usr/bin/env python3
"""Bake Hunter's Atlas generator output into the in-game map's data files (gpu/shim/map/map_core.h).

    python -I scripts/bbmap_bake.py GENERATED_DIR OUT_DIR

GENERATED_DIR is the generator's output (`maps/*.json`, `maps/features.json`, `maps/atlas.json`),
OUT_DIR gets one `mAA_BB_CC_DD.bbm` per area and `map.stamp` (a JSON line: format version, area
count, the generator manifest's eboot hash). Only the Python standard library is used, so this runs
on the launcher's bundled Python. The files hold geometry made from the player's own game files: they
are made on the player's PC (launcher.py "Prepare the map" / scripts/map_prepare.py) and never shipped.

Hunter's Atlas by MikeTat1988 (github.com/MikeTat1988/HuntersAtlas, GPL-2.0-or-later) makes the JSON;
this file only repacks it.
"""
import json
import math
import os
import struct
import sys
from pathlib import Path

FORMAT_VERSION = 1
MAX_LAYERS = 64
DREAM_RETURN_POINT = 2102950


def finite(*values):
    return all(isinstance(v, (int, float)) and math.isfinite(v) for v in values)


def put_str(out, text):
    raw = text.encode("utf-8")[:0xFFFF]
    out += struct.pack("<H", len(raw)) + raw


def pack_map_id(map_id):
    """'m24_01_00_00' -> 0x18010000."""
    parts = map_id[1:].split("_")
    if map_id[0] != "m" or len(parts) != 4 or not all(p.isdigit() for p in parts):
        raise ValueError(f"not a map id: {map_id!r}")
    a, b, c, d = (int(p) for p in parts)
    return (a << 24) | (b << 16) | (c << 8) | d


def bake_area(data, lamps):
    """The .bbm bytes of one generator map (`data`) with its lamps ([{name, pos, return_point}])."""
    map_id = data["id"]
    bounds = [float(v) for v in data["bounds"]]
    if len(bounds) != 4 or not finite(*bounds) or not (bounds[0] < bounds[2] and bounds[1] < bounds[3]):
        raise ValueError(f"{map_id}: bad bounds {bounds}")
    layers = data["layers"][:MAX_LAYERS]
    out = bytearray(b"BBM1")
    out += struct.pack("<II", FORMAT_VERSION, pack_map_id(map_id))
    out += struct.pack("<4f", *bounds)
    put_str(out, data.get("name") or map_id)
    put_str(out, map_id)
    out += struct.pack("<I", len(layers))
    for layer in layers:
        out += struct.pack("<3f", float(layer["height"]), float(layer.get("span", 3.0)), float(layer["anchor"]))
        tris = layer.get("triangles", [])
        heights = layer.get("heights")
        flat = []
        for i, t in enumerate(tris):
            if len(t) != 6 or not finite(*t):
                continue
            if heights is not None and i < len(heights) and len(heights[i]) == 3 and finite(*heights[i]):
                y = sum(heights[i]) / 3.0
            else:
                y = float(layer["anchor"])
            flat.extend((*t, y))
        out += struct.pack("<I", len(flat) // 7)
        out += struct.pack(f"<{len(flat)}f", *flat)
        rings = [r for r in layer.get("outlines", []) if len(r) > 1 and all(len(p) == 2 and finite(*p) for p in r)]
        out += struct.pack("<I", len(rings))
        for ring in rings:
            out += struct.pack("<I", len(ring))
            out += struct.pack(f"<{len(ring) * 2}f", *(c for p in ring for c in p))
        walls = [w for w in layer.get("walls", []) if len(w) == 4 and finite(*w)]
        out += struct.pack("<I", len(walls))
        out += struct.pack(f"<{len(walls) * 4}f", *(c for w in walls for c in w))
    lamps = [l for l in lamps if len(l["pos"]) == 3 and finite(*l["pos"])][:256]
    out += struct.pack("<I", len(lamps))
    for lamp in lamps:
        out += struct.pack("<I3f", int(lamp["return_point"]), *(float(v) for v in lamp["pos"]))
        put_str(out, lamp["name"])
    return bytes(out)


def bake(generated, out_dir):
    maps = Path(generated)
    if (maps / "maps").is_dir():
        maps = maps / "maps"
    atlas = json.loads((maps / "atlas.json").read_text(encoding="utf-8"))
    features = json.loads((maps / "features.json").read_text(encoding="utf-8"))
    manifest_path = maps / "generated-assets.json"
    eboot = ""
    if manifest_path.is_file():
        eboot = json.loads(manifest_path.read_text(encoding="utf-8")).get("game_eboot_sha256", "")
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for item in atlas["maps"]:
        map_id = item["id"]
        data = json.loads((maps / f"{map_id}.json").read_text(encoding="utf-8"))
        lamps = [
            {"name": l["name"], "pos": l["pos"], "return_point": l["return_point"]}
            for l in features.get("areas", {}).get(map_id, {}).get("lamps", [])
            if isinstance(l.get("return_point"), int)
        ]
        blob = bake_area(data, lamps)
        target = out_dir / f"{map_id}.bbm"
        tmp = target.with_suffix(".tmp")
        tmp.write_bytes(blob)
        os.replace(tmp, target)
        written.append(target.name)
    stamp = {"format": FORMAT_VERSION, "areas": len(written), "eboot_sha256": eboot}
    tmp = out_dir / "map.stamp.tmp"
    tmp.write_text(json.dumps(stamp), encoding="utf-8")
    os.replace(tmp, out_dir / "map.stamp")  # last: its presence means the set is complete
    return written


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    written = bake(argv[1], argv[2])
    print(json.dumps({"stage": "baked", "areas": len(written)}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
