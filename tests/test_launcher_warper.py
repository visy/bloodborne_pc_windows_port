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


class SharedCacheBundle(unittest.TestCase):
    """A cache shared with another PC: the portable part, each GPU's part in its own folder, the
    drop folder user/cache_import merged at launch."""
    VKP = "vk_pipe_cache_10de_2684_9a4a8000.vkp"  # NVIDIA 617.42

    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def make_cache(self, root: Path, profile: bytes = b"P1") -> Path:
        t = root / "CUSA03173"
        t.mkdir(parents=True)
        for name, data in (("g_0x1.key", b"k"), ("0x2_0.src", b"s"), ("0x2_0.spv", b"v"),
                           ("0x0000000000000002.meta", b"m"), ("profile.bin", profile), (self.VKP, b"d"),
                           ("x.key.tmp", b"t")):
            (t / name).write_bytes(data)
        return root

    def test_label(self):
        self.assertEqual(self.L.gpu_label_from_vkp(self.VKP), "NVIDIA_10de-2684_driver_617.42")
        self.assertEqual(self.L.gpu_label_from_vkp("profile.bin"), "")
        self.assertEqual(self.L.driver_version_text(0x10de, (617 << 22) | (42 << 14)), "617.42")

    def test_bundle_layout(self):
        with tempfile.TemporaryDirectory() as t:
            root = self.make_cache(Path(t) / "cache")
            out = Path(t) / "b.zip"
            r = self.L.export_shader_cache_bundle(out, root, readme="hello")
            self.assertEqual((r["portable"], r["gpu"], r["label"]), (2, 4, "NVIDIA_10de-2684_driver_617.42"))
            with zipfile.ZipFile(out) as z:
                names = sorted(z.namelist())
                self.assertEqual(z.read("README.txt"), b"hello")
            g = "gpu/NVIDIA_10de-2684_driver_617.42/CUSA03173/"
            self.assertEqual(names, sorted(["README.txt", "CUSA03173/g_0x1.key", "CUSA03173/0x2_0.src",
                                            g + "0x2_0.spv", g + "0x0000000000000002.meta", g + "profile.bin",
                                            g + self.VKP]))

    def test_import_other_gpu_takes_only_the_portable_part(self):
        with tempfile.TemporaryDirectory() as t:
            src = self.make_cache(Path(t) / "a")
            out = Path(t) / "b.zip"
            self.L.export_shader_cache_bundle(out, src)
            dst = Path(t) / "other" / "CUSA03173"
            dst.mkdir(parents=True)
            (dst / "profile.bin").write_bytes(b"AMD")
            st = self.L.import_shader_cache(out, dst.parent)
            self.assertEqual((st["added"], st["gpu_skipped"], st["gpu_used"]), (2, 3, []))
            self.assertEqual(sorted(p.name for p in dst.iterdir()), ["0x2_0.src", "g_0x1.key", "profile.bin"])
            self.assertEqual((dst / "profile.bin").read_bytes(), b"AMD")

    def test_import_same_gpu_and_empty_cache_take_everything(self):
        with tempfile.TemporaryDirectory() as t:
            src = self.make_cache(Path(t) / "a")
            out = Path(t) / "b.zip"
            self.L.export_shader_cache_bundle(out, src)
            for local in (b"P1", None):
                dst = Path(t) / ("same" if local else "empty") / "CUSA03173"
                dst.mkdir(parents=True)
                if local:
                    (dst / "profile.bin").write_bytes(local)
                st = self.L.import_shader_cache(out, dst.parent)
                self.assertEqual(st["gpu_skipped"], 0)
                self.assertEqual(st["gpu_used"], ["NVIDIA_10de-2684_driver_617.42"])
                self.assertTrue((dst / self.VKP).is_file())
                self.assertEqual((dst / "profile.bin").read_bytes(), b"P1")
                self.assertFalse((dst / "x.key.tmp").exists())

    def test_old_flat_export_still_imports(self):
        with tempfile.TemporaryDirectory() as t:
            src = self.make_cache(Path(t) / "a")
            out = Path(t) / "flat.zip"
            self.L.export_shader_cache(out, src)
            dst = Path(t) / "b"
            st = self.L.import_shader_cache(out, dst)
            self.assertEqual(st["added"], 4)  # key, src, spv, meta (+ profile.bin copied)
            self.assertTrue((dst / "CUSA03173" / "profile.bin").is_file())

    def test_drop_folder(self):
        with tempfile.TemporaryDirectory() as t:
            src = self.make_cache(Path(t) / "a")
            root = Path(t) / "user" / "cache"
            drop = self.L.cache_import_dir(root)
            self.assertEqual(drop, Path(t) / "user" / "cache_import")
            self.assertEqual(self.L.import_dropped_caches(root), [])  # no folder: nothing
            drop.mkdir(parents=True)
            self.L.export_shader_cache_bundle(drop / "shared.zip", src)
            (drop / "broken.zip").write_bytes(b"not a zip")
            done = dict(self.L.import_dropped_caches(root))
            self.assertIsNone(done["broken.zip"])
            self.assertEqual(done["shared.zip"]["added"], 5)  # key, src, spv, meta, vkp (+ profile.bin)
            self.assertTrue((drop / "shared.zip.imported").is_file())
            self.assertTrue((drop / "broken.zip.bad").is_file())
            self.assertEqual(self.L.import_dropped_caches(root), [])  # merged once
            self.assertTrue((root / "CUSA03173" / self.VKP).is_file())


class CachesStayOutOfGit(unittest.TestCase):
    def test_gitignore(self):
        text = (ROOT / ".gitignore").read_text(encoding="utf-8")
        for pattern in ("*.zip", "user/", "world_warper/"):
            self.assertIn(pattern, text.split(), pattern)


if __name__ == "__main__":
    unittest.main()
