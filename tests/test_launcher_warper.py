"""launcher.py world warper (I3, Advanced > Shader cache builder): the run plays on a copy of the
save folder (the player's saves are never written), the shader cache stays the player's own, the
progress file is read for resume / export, and the export has the launcher's usual format."""
from paths import ROOT
import importlib.util
import tempfile
import unittest
import zipfile
from pathlib import Path


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_warper", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class WorldWarper(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def make_user(self, tmp: Path) -> Path:
        user = tmp / "user"
        (user / "savedata" / "slot0").mkdir(parents=True)
        (user / "savedata" / "slot0" / "userdata0000").write_bytes(b"save")
        (user / "cache" / "CUSA03173").mkdir(parents=True)
        (user / "cache" / "CUSA03173" / "a.key").write_bytes(b"k")
        (user / "cache" / "CUSA03173" / "profile.bin").write_bytes(b"p")
        return user

    def test_save_copy_never_touches_the_real_saves(self):
        with tempfile.TemporaryDirectory() as t:
            user = self.make_user(Path(t))
            before = (user / "savedata" / "slot0" / "userdata0000").stat().st_mtime_ns
            paths = self.L.warper_paths(user)
            self.assertEqual(paths["base"], user / "world_warper")
            self.assertEqual(self.L.prepare_warper_saves(paths, fresh=True), 1)
            self.assertEqual((paths["saves"] / "slot0" / "userdata0000").read_bytes(), b"save")
            # A resume keeps the copy (the run's own progress); a fresh start copies again.
            (paths["saves"] / "slot0" / "userdata0000").write_bytes(b"warped")
            self.assertEqual(self.L.prepare_warper_saves(paths, fresh=False), 0)
            self.assertEqual((paths["saves"] / "slot0" / "userdata0000").read_bytes(), b"warped")
            paths["state"].write_text("start 2102950\n")
            self.assertEqual(self.L.prepare_warper_saves(paths, fresh=True), 1)
            self.assertFalse(paths["state"].exists())
            self.assertEqual((paths["saves"] / "slot0" / "userdata0000").read_bytes(), b"save")
            self.assertEqual((user / "savedata" / "slot0" / "userdata0000").read_bytes(), b"save")
            self.assertEqual((user / "savedata" / "slot0" / "userdata0000").stat().st_mtime_ns, before)

    def test_no_save(self):
        with tempfile.TemporaryDirectory() as t:
            paths = self.L.warper_paths(Path(t) / "user")
            with self.assertRaises(FileNotFoundError):
                self.L.prepare_warper_saves(paths, fresh=True)

    def test_env(self):
        with tempfile.TemporaryDirectory() as t:
            paths = self.L.warper_paths(Path(t) / "user")
            env = self.L.warper_env({"BB_PARTY": "host", "BB_PARTY_CODE": "x", "BB_FPS": "60", "BB_TIMEOUT": "30"},
                                    paths)
            self.assertFalse(any(k.startswith("BB_PARTY") for k in env))
            self.assertEqual(env["BB_USER_DIR"], str(paths["base"]))       # saves: the copy
            self.assertEqual(env["BB_GPU_USER_DIR"], str(paths["user"]))   # shader cache: the player's own
            self.assertEqual(env["BB_WORLD_WARPER"], "tour")
            self.assertEqual(env["BB_WORLD_WARPER_STATE"], str(paths["state"]))
            self.assertEqual(env["BB_TIMEOUT"], "0")
            self.assertEqual(env["BB_FPS"], "60")

    def test_progress(self):
        with tempfile.TemporaryDirectory() as t:
            state = Path(t) / "s.txt"
            self.assertEqual(self.L.read_warper_progress(state)["done"], 0)
            state.write_text("start 2102950\ndone 2102950 120 2 80 45 m21_00_00_00\nstart 2412950\n"
                             "start 2412951\nskip 2412951 no load\nstart 2402950\ndone 2402950 30 0 40 50 m\n")
            p = self.L.read_warper_progress(state)
            self.assertEqual((p["done"], p["skipped"], p["started"], p["finished"]), (2, 1, 1, False))
            self.assertEqual(p["new_pipelines"], 150)
            state.write_text(state.read_text() + "finished\n")
            self.assertTrue(self.L.read_warper_progress(state)["finished"])

    def test_export_format(self):
        with tempfile.TemporaryDirectory() as t:
            user = self.make_user(Path(t))
            out = Path(t) / "c.zip"
            self.assertEqual(self.L.export_shader_cache(out, user / "cache"), 2)
            with zipfile.ZipFile(out) as z:
                self.assertEqual(sorted(z.namelist()), ["CUSA03173/a.key", "CUSA03173/profile.bin"])


class CachesStayOutOfGit(unittest.TestCase):
    def test_gitignore(self):
        text = (ROOT / ".gitignore").read_text(encoding="utf-8")
        for pattern in ("*.zip", "user/", "world_warper/"):
            self.assertIn(pattern, text.split(), pattern)


if __name__ == "__main__":
    unittest.main()
