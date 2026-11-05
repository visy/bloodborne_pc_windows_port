"""scripts/party_tables.py: the cache format, its key, and (with a game folder at hand) a real build.
No game data is stored here: the synthetic cache uses made-up numbers, the real one is built from
BB_GAME_DIR (else the agent clone's game/) and skipped when there is none."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import party_tables as pt  # noqa: E402


def game_dir():
    for c in (os.environ.get("BB_GAME_DIR"), str(ROOT.parent / "game")):
        if c and (Path(c) / "dvdroot_ps4" / "param").is_dir():
            return Path(c)
    return None


class Format(unittest.TestCase):
    def test_render_and_key(self):
        text = pt.render("abc", {"maps": [["1"], ["2"]], "npcFog": [["1", "2", "3", "4", "5", "0.50", "1.00", "2.00", "3.0"]]})
        lines = text.split("\n")
        self.assertEqual(lines[0], f"BBPARTYTABLES {pt.FORMAT}")
        self.assertEqual(lines[1], "key abc")
        self.assertIn("[maps] 2", lines)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.txt"
            p.write_text(text, encoding="utf-8")
            self.assertEqual(pt.cache_key(p), "abc")
            self.assertIsNone(pt.cache_key(Path(d) / "missing.txt"))
            p.write_text("garbage\n", encoding="utf-8")
            self.assertIsNone(pt.cache_key(p))

    def test_tab_in_field_rejected(self):
        with self.assertRaises(ValueError):
            pt.render("k", {"x": [["a\tb"]]})

    def test_no_game_data_is_reported_not_raised(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(pt.build(Path(d) / "nogame", Path(d) / "u" / pt.CACHE_NAME, quiet=True), 1)
            self.assertFalse((Path(d) / "u" / pt.CACHE_NAME).exists())


@unittest.skipUnless(game_dir(), "no game folder (BB_GAME_DIR)")
class RealBuild(unittest.TestCase):
    def test_build_and_rebuild_only_on_change(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / pt.CACHE_NAME
            self.assertEqual(pt.build(game_dir(), out, quiet=True), 0)
            key = pt.cache_key(out)
            self.assertEqual(key, pt.inputs_key(pt.game_root(game_dir())))
            mtime = out.stat().st_mtime_ns
            self.assertEqual(pt.build(game_dir(), out, quiet=True), 0)  # current: untouched
            self.assertEqual(out.stat().st_mtime_ns, mtime)
            sections = {}
            cur = None
            for line in out.read_text(encoding="utf-8").split("\n")[2:]:
                if line.startswith("["):
                    cur = line[1:line.index("]")]
                    sections[cur] = []
                elif line:
                    sections[cur].append(line.split("\t"))
            for name in ("maps", "returnPoints", "entities", "remos", "npcMirror", "npcFog", "warperRight", "warperLeft",
                         "warperArmor", "itemLotFlag", "itemLedger", "itemUnshared", "itemSharedTreasure"):
                self.assertTrue(sections.get(name), name)


if __name__ == "__main__":
    unittest.main()
