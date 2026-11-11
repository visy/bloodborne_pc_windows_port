"""launcher.py: Noxde's community Chalice glyph list (0.8.1, glyphdb081): strict validation, cache file, offline
behaviour, the checkbox. Synthetic data only (nothing of the real list)."""
from pathlib import Path
from paths import ROOT
import importlib.util
import json
import os
import tempfile
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_glyphdb", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def record(map_word=0x1D123456, goods=6109):
    words = [map_word, 77, 0, map_word, goods] + [0xA0 + i for i in range(18)] + [0x4E4E4E4E] * 8
    data = b"".join(w.to_bytes(4, "big") for w in words)
    assert len(data) == 124
    return ["0x%02x" % b for b in data]


def gist_text(dungeons):
    return json.dumps({"files": {"dungeons.json": {"content": json.dumps(dungeons), "truncated": False}}})


class GlyphDb(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_validate_keeps_good_and_drops_bad(self):
        good = record()
        dungeons = {
            "farming": [
                {"glyph": "aaaa1111", "bytes": good, "desc": "Echoes"},
                {"glyph": "BBBB2222", "bytes": good, "desc": "Upper case is folded"},
                {"glyph": "short", "bytes": good, "desc": "bad glyph"},
                {"glyph": "cccc3333", "bytes": good[:-1], "desc": "123 bytes"},
                {"glyph": "dddd4444", "bytes": good, "desc": "x" * 201},  # kept, cut to 200
                {"glyph": "eeee5555", "bytes": good, "desc": "tab\there"},
                {"glyph": "aaaa1111", "bytes": good, "desc": "duplicate"},
                {"glyph": "ffff6666", "bytes": good[:5] + ["0x1ff"] + good[6:], "desc": "byte out of range"},
                {"glyph": "gggg7777", "bytes": good, "desc": ""},
                "not a row",
            ],
            "equipment": [{"glyph": "hhhh88889", "bytes": good, "desc": "Nine characters are fine"}],
            "bloodgems": "not a list",
            "unknown": [{"glyph": "iiii9999", "bytes": good, "desc": "unknown category"}],
        }
        cache, dropped = self.L.glyphdb_validate(dungeons)
        self.assertEqual([g["glyph"] for g in cache["glyphs"]], ["aaaa1111", "bbbb2222", "dddd4444", "eeee5555", "hhhh88889"])
        self.assertEqual(dropped, 6)
        cut = cache["glyphs"][2]["desc"]
        self.assertEqual((len(cut), cut[-3:]), (200, "..."))
        self.assertEqual(cache["version"], 1)
        self.assertEqual(cache["glyphs"][3]["desc"], "tab here")
        row = cache["glyphs"][4]
        self.assertEqual(row["cat"], "equipment")
        self.assertEqual(len(row["rec"]), 248)
        self.assertEqual(row["rec"][184:], "00" * 32)  # the creator's name field is never kept
        self.assertEqual(row["rec"][40:48], "000000a0")  # word 5, the first layout dword, is kept
        self.assertEqual(row["rec"][:8], "1d123456")  # the record's first dword, big endian as in the save
        self.assertEqual(set(row), {"glyph", "cat", "desc", "rec"})

    def test_validate_never_raises(self):
        for junk in (None, [], "x", 5, {"farming": [None, 3, {"glyph": 5, "bytes": 7, "desc": 1}]}):
            cache, _ = self.L.glyphdb_validate(junk)
            self.assertEqual(cache["glyphs"], [])

    def test_gist_document(self):
        cache, dropped = self.L.glyphdb_from_gist(gist_text({"testing": [{"glyph": "aaaa1111", "bytes": record(), "desc": "d"}]}))
        self.assertEqual((len(cache["glyphs"]), dropped), (1, 0))
        self.assertEqual(self.L.glyphdb_from_gist("not json"), (None, 0))
        self.assertEqual(self.L.glyphdb_from_gist("{}"), (None, 0))
        truncated = json.dumps({"files": {"dungeons.json": {"content": "{}", "truncated": True}}})
        self.assertEqual(self.L.glyphdb_from_gist(truncated), (None, 0))

    def test_refresh_caches_then_works_offline(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "community" / "chalice_glyphs.json"
            text = gist_text({"farming": [{"glyph": "aaaa1111", "bytes": record(), "desc": "d"}]})
            self.assertEqual(self.L.glyphdb_refresh(fetch=lambda: text, path=path), "cached 1")
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["glyphs"][0]["glyph"], "aaaa1111")

            def offline():
                raise OSError("no network")

            # fresh cache: no fetch at all
            self.assertEqual(self.L.glyphdb_refresh(fetch=offline, path=path), "fresh 1")
            # stale cache, no network: the cache stays and is reported
            later = path.stat().st_mtime + self.L.GLYPHDB_MAX_AGE_S + 5
            self.assertEqual(self.L.glyphdb_refresh(fetch=offline, path=path, now=later), "offline, cached 1")
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["glyphs"][0]["glyph"], "aaaa1111")
            # a garbage reply never replaces a good cache
            self.assertEqual(self.L.glyphdb_refresh(force=True, fetch=lambda: "garbage", path=path), "offline, cached 1")
            # neither cache nor network: the feature is quietly absent
            missing = Path(tmp) / "none" / "c.json"
            self.assertEqual(self.L.glyphdb_refresh(fetch=offline, path=missing), "offline, none")
            self.assertFalse(missing.exists())
            # a reply with nothing usable writes nothing
            empty = gist_text({"farming": [{"glyph": "x", "bytes": [], "desc": "d"}]})
            self.assertEqual(self.L.glyphdb_refresh(fetch=lambda: empty, path=missing), "unusable")
            self.assertFalse(missing.exists())

    def test_fetch_limits_the_reply(self):
        class Reply:
            def __init__(self, data):
                self.data = data

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def read(self, n):
                return self.data[:n]

        self.assertEqual(self.L.glyphdb_fetch(opener=lambda req: Reply(b"{}")), "{}")
        seen = []

        def opener(req):
            seen.append(req.full_url)
            return Reply(b"x" * (self.L.GLYPHDB_MAX_BYTES + 10))

        with self.assertRaises(ValueError):
            self.L.glyphdb_fetch(opener=opener)
        self.assertEqual(seen, ["https://api.github.com/gists/a29f699f4175bf315d9bd4baeebefb66"])

    def test_cache_lives_in_the_user_dir(self):
        old = os.environ.get("BB_USER_DIR")
        os.environ["BB_USER_DIR"] = r"C:\somewhere\user"
        try:
            self.assertEqual(self.L.glyphdb_cache_path(), Path(r"C:\somewhere\user") / "community" / "chalice_glyphs.json")
        finally:
            if old is None:
                os.environ.pop("BB_USER_DIR", None)
            else:
                os.environ["BB_USER_DIR"] = old

    def test_switch_default_off_and_ini(self):
        switches = {key: (env, default) for key, env, default in self.L.PARTY_INI_SWITCHES}
        self.assertEqual(switches["chalice_glyphdb"], ("BB_CHALICE_GLYPHDB", False))
        self.assertEqual(self.L.party_rules_ini({}, "")["chalice_glyphdb"], "0")
        self.assertEqual(self.L.party_rules_ini({"chalice_glyphdb": True}, "")["chalice_glyphdb"], "1")
        src = Path(self.L.__file__).read_text(encoding="utf-8")
        self.assertIn('("check", "chalice_glyphdb", "Community Chalice glyphs (Noxde\'s list)",', src)
        self.assertIn("https://gist.github.com/Noxde/a29f699f4175bf315d9bd4baeebefb66", src)


if __name__ == "__main__":
    unittest.main()
