"""The in-game map's data pipeline (scripts/bbmap_bake.py, scripts/map_prepare.py): a synthetic generator
output is baked into .bbm files, read back with an independent reader that follows the format description in
gpu/shim/map/map_core.h, and map_prepare's "is the data current" check. No game files, synthetic data only."""
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import bbmap_bake  # noqa: E402
import map_prepare  # noqa: E402

# The same bytes the C++ test (tests/test_map_core.cpp TestGolden) parses: bake_area() of SAMPLE_AREA.
SAMPLE_AREA = {
    "id": "m24_01_00_00", "name": "Test Yharnam", "bounds": [-50.0, -20.0, 150.0, 80.0],
    "layers": [
        {"height": 0.0, "span": 3.0, "anchor": 1.5,
         "triangles": [[0, 0, 10, 0, 10, 10], [0, 0, 10, 10, 0, 10]], "heights": [[0, 0, 0], [0, 0, 3]],
         "outlines": [[[0, 0], [10, 0], [10, 10], [0, 10]]], "walls": [[0, 0, 10, 0]]},
        {"height": 6.0, "span": 3.0, "anchor": 7.5, "triangles": [[0, 0, 5, 0, 5, 10]], "outlines": [], "walls": []},
    ],
}
SAMPLE_LAMPS = [{"name": "Lamp A", "pos": [3.0, 0.0, 4.0], "return_point": 2412951}]
GOLDEN_TINY = {"id": "m21_01_00_00", "name": "Tiny", "bounds": [0.0, 0.0, 20.0, 10.0], "layers": [
    {"height": 0.0, "span": 3.0, "anchor": 1.5, "triangles": [[0, 0, 4, 0, 0, 4]], "heights": [[1, 2, 3]],
     "outlines": [[[0, 0], [4, 0], [0, 4]]], "walls": [[0, 0, 4, 0]]}]}
GOLDEN_HEX = ("42424d31010000000000011500000000000000000000a04100002041040054696e790c006d32315f30315f30305f30300100000000000000000040400000c03f0100000000000000000000000000804000000000000000000000804000000040010000000300000000000000000000000000804000000000000000000000804001000000000000000000000000008040000000000100000097d124000000803f000000400000404004004c616d70")


def read_area(blob):
    """An independent reader of the .bbm format (the description at the top of map_core.h)."""
    at = 0

    def take(fmt):
        nonlocal at
        size = struct.calcsize(fmt)
        out = struct.unpack_from("<" + fmt, blob, at)
        at += size
        return out

    def string():
        nonlocal at
        (n,) = take("H")
        text = blob[at:at + n].decode("utf-8")
        at += n
        return text

    assert blob[:4] == b"BBM1"
    at = 4
    version, packed = take("II")
    bounds = take("4f")
    name = string()
    map_id = string()
    layers = []
    (count,) = take("I")
    for _ in range(count):
        height, span, anchor = take("3f")
        (tris,) = take("I")
        flat = take("%df" % (tris * 7))
        (rings,) = take("I")
        ring_list = []
        for _ in range(rings):
            (pts,) = take("I")
            ring_list.append(take("%df" % (pts * 2)))
        (walls,) = take("I")
        wall_list = take("%df" % (walls * 4))
        layers.append(dict(height=height, span=span, anchor=anchor, tris=flat, rings=ring_list, walls=wall_list))
    (lamp_count,) = take("I")
    lamps = []
    for _ in range(lamp_count):
        rp, x, y, z = take("I3f")
        lamps.append(dict(rp=rp, pos=(x, y, z), name=string()))
    assert at == len(blob), "trailing bytes"
    return dict(version=version, packed=packed, bounds=bounds, name=name, id=map_id, layers=layers, lamps=lamps)


class BakeTests(unittest.TestCase):
    def test_pack_map_id(self):
        self.assertEqual(bbmap_bake.pack_map_id("m24_01_00_00"), 0x18010000)
        self.assertEqual(bbmap_bake.pack_map_id("m21_00_00_00"), 0x15000000)
        with self.assertRaises(ValueError):
            bbmap_bake.pack_map_id("x24_01_00_00")
        with self.assertRaises(ValueError):
            bbmap_bake.pack_map_id("m24_01")

    def test_bake_area_round_trip(self):
        a = read_area(bbmap_bake.bake_area(SAMPLE_AREA, SAMPLE_LAMPS))
        self.assertEqual(a["version"], 1)
        self.assertEqual(a["packed"], 0x18010000)
        self.assertEqual(a["id"], "m24_01_00_00")
        self.assertEqual(a["name"], "Test Yharnam")
        self.assertEqual(a["bounds"], (-50.0, -20.0, 150.0, 80.0))
        self.assertEqual(len(a["layers"]), 2)
        first = a["layers"][0]
        self.assertEqual(len(first["tris"]), 14)  # two triangles, 7 floats each
        self.assertEqual(first["tris"][6], 0.0)  # the mean of its vertex heights
        self.assertAlmostEqual(first["tris"][13], 1.0)  # (0 + 0 + 3) / 3
        self.assertEqual(len(first["rings"]), 1)
        self.assertEqual(len(first["rings"][0]), 8)
        self.assertEqual(first["walls"], (0.0, 0.0, 10.0, 0.0))
        # A layer without vertex heights falls back to its anchor.
        self.assertEqual(a["layers"][1]["tris"][6], 7.5)
        self.assertEqual(a["lamps"], [dict(rp=2412951, pos=(3.0, 0.0, 4.0), name="Lamp A")])

    def test_bad_input_is_refused_or_skipped(self):
        bad = dict(SAMPLE_AREA, bounds=[1.0, 1.0, 1.0, 1.0])
        with self.assertRaises(ValueError):
            bbmap_bake.bake_area(bad, [])
        nan = float("nan")
        weird = json.loads(json.dumps(SAMPLE_AREA))
        weird["layers"][0]["triangles"].append([0, 0, nan, 0, 1, 1])  # dropped, not written
        a = read_area(bbmap_bake.bake_area(weird, [{"name": "x", "pos": [nan, 0, 0], "return_point": 1}]))
        self.assertEqual(len(a["layers"][0]["tris"]), 14)
        self.assertEqual(a["lamps"], [])

    def test_golden_bytes_match_the_cpp_test(self):
        # tests/test_map_core.cpp TestGolden parses exactly these bytes (the C++ reader and this baker agree).
        blob = bbmap_bake.bake_area(GOLDEN_TINY, [{"name": "Lamp", "pos": [1.0, 2.0, 3.0], "return_point": 2412951}])
        self.assertEqual(blob.hex(), GOLDEN_HEX)

    def _generated(self, root):
        maps = Path(root) / "maps"
        maps.mkdir()
        (maps / "m24_01_00_00.json").write_text(json.dumps(dict(SAMPLE_AREA, version=2, lamps=[])), encoding="utf-8")
        (maps / "atlas.json").write_text(json.dumps({"version": 1, "maps": [{"id": "m24_01_00_00"}]}), encoding="utf-8")
        (maps / "features.json").write_text(json.dumps({"areas": {"m24_01_00_00": {"lamps": [
            {"id": "m24_01_00_00:1", "name": "Lamp A", "pos": [3.0, 0.0, 4.0], "return_point": 2412951},
            {"id": "x", "name": "no return point", "pos": [0, 0, 0]}]}}}), encoding="utf-8")
        (maps / "generated-assets.json").write_text(json.dumps({"game_eboot_sha256": "ab" * 32}), encoding="utf-8")
        return Path(root)

    def test_bake_directory(self):
        with tempfile.TemporaryDirectory() as src, tempfile.TemporaryDirectory() as out:
            self._generated(src)
            written = bbmap_bake.bake(src, out)
            self.assertEqual(written, ["m24_01_00_00.bbm"])
            stamp = json.loads((Path(out) / "map.stamp").read_text(encoding="utf-8"))
            self.assertEqual(stamp, {"format": 1, "areas": 1, "eboot_sha256": "ab" * 32})
            a = read_area((Path(out) / "m24_01_00_00.bbm").read_bytes())
            self.assertEqual([l["name"] for l in a["lamps"]], ["Lamp A"])  # the lamp without a return point is left out


class PrepareTests(unittest.TestCase):
    def test_is_current(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            game = tmp / "game"
            game.mkdir()
            (game / "eboot.bin").write_bytes(b"synthetic executable")
            sha = hashlib.sha256(b"synthetic executable").hexdigest()
            out = tmp / "map"
            out.mkdir()
            self.assertFalse(map_prepare.is_current(out, game))  # no stamp
            (out / "a.bbm").write_bytes(b"x")
            (out / "map.stamp").write_text(json.dumps({"format": 1, "areas": 1, "eboot_sha256": sha}), encoding="utf-8")
            self.assertTrue(map_prepare.is_current(out, game))
            (out / "map.stamp").write_text(json.dumps({"format": 1, "areas": 2, "eboot_sha256": sha}), encoding="utf-8")
            self.assertFalse(map_prepare.is_current(out, game))  # a file is missing
            (out / "map.stamp").write_text(json.dumps({"format": 1, "areas": 1, "eboot_sha256": "00" * 32}), encoding="utf-8")
            self.assertFalse(map_prepare.is_current(out, game))  # another executable
            (out / "map.stamp").write_text(json.dumps({"format": 99, "areas": 1, "eboot_sha256": sha}), encoding="utf-8")
            self.assertFalse(map_prepare.is_current(out, game))  # another format

    def test_game_root_and_check_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "Image0" / "dvdroot_ps4").mkdir(parents=True)
            self.assertEqual(map_prepare.game_root(tmp), tmp / "Image0")
            self.assertEqual(map_prepare.game_root(tmp / "nowhere"), tmp / "nowhere")
            # --check on an empty folder: not current (exit 3), nothing is run or written.
            self.assertEqual(map_prepare.main(["--game-dir", str(tmp), "--out-dir", str(tmp / "map"), "--check"]), 3)
            self.assertFalse((tmp / "map").exists())

    def test_no_generator_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "game").mkdir()
            (tmp / "game" / "eboot.bin").write_bytes(b"x")
            found = map_prepare.find_generator(str(tmp / "nope"))
            # Either the repository's source (development) or nothing; never an exe that does not exist.
            self.assertTrue(found is None or found[0] in ("exe", "py"))
            if found:
                self.assertTrue(found[1].exists())


if __name__ == "__main__":
    unittest.main()
