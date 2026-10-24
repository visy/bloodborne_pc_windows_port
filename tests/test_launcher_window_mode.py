"""launcher.py window mode (0.7.4, WINDOW_REV 1): Borderless is the default; a saved exclusive
Fullscreen moves to Borderless once (exclusive fullscreen lost the GPU on hybrid-graphics laptops);
Fullscreen chosen again after that, and Windowed, stay."""
from paths import ROOT
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_window", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class WindowMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def mode(self, s):
        return self.L.window_mode_setting(s)

    def test_default_is_borderless(self):
        self.assertEqual(self.mode({}), "borderless")
        env = self.L.settings_env({})
        self.assertEqual(env["BB_WINDOW_MODE"], "borderless")
        self.assertEqual(env["BB_FULLSCREEN"], "0")

    def test_old_fullscreen_moves_to_borderless(self):
        for s in ({"window_mode": "fullscreen", "feat_fullscreen": True},
                  {"feat_fullscreen": True}, {"fullscreen": True}):
            self.assertEqual(self.mode(s), "borderless", s)
            self.assertEqual(self.L.settings_env(s)["BB_WINDOW_MODE"], "borderless")

    def test_windowed_and_borderless_stay(self):
        self.assertEqual(self.mode({"window_mode": "windowed"}), "windowed")
        self.assertEqual(self.mode({"feat_fullscreen": False}), "windowed")
        self.assertEqual(self.mode({"window_mode": "borderless"}), "borderless")

    def test_fullscreen_chosen_after_the_change_stays(self):
        s = {"window_mode": "fullscreen", "feat_fullscreen": True, "window_rev": self.L.WINDOW_REV}
        self.assertEqual(self.mode(s), "fullscreen")
        env = self.L.settings_env(s)
        self.assertEqual(env["BB_WINDOW_MODE"], "fullscreen")
        self.assertEqual(env["BB_FULLSCREEN"], "1")

    def test_migration_once(self):
        s = {"window_mode": "fullscreen", "feat_fullscreen": True}
        self.assertTrue(self.L.migrate_window_mode(s))
        self.assertEqual(s["window_mode"], "borderless")
        self.assertFalse(s["feat_fullscreen"])
        self.assertEqual(s["window_rev"], self.L.WINDOW_REV)
        # The player picks exclusive fullscreen again: no second change.
        s["window_mode"], s["feat_fullscreen"] = "fullscreen", True
        self.assertFalse(self.L.migrate_window_mode(s))
        self.assertEqual(self.mode(s), "fullscreen")

    def test_write_env_migrates_the_saved_file_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            settings = tmp / "launcher_settings.json"
            settings.write_text(json.dumps({"window_mode": "fullscreen", "feat_fullscreen": True}),
                                encoding="utf-8")
            old_ini = self.L.bbport_ini_path
            self.L.bbport_ini_path = lambda: tmp / "bbport.ini"
            try:
                self.L.write_env_bat(tmp / "env.bat", settings)
                data = json.loads(settings.read_text(encoding="utf-8"))
                self.assertEqual(data["window_mode"], "borderless")
                self.assertEqual(data["window_rev"], self.L.WINDOW_REV)
                self.assertIn('set "BB_WINDOW_MODE=borderless"', (tmp / "env.bat").read_text(errors="replace"))
                data.update(window_mode="fullscreen", feat_fullscreen=True)
                settings.write_text(json.dumps(data), encoding="utf-8")
                self.L.write_env_bat(tmp / "env.bat", settings)
                self.assertEqual(json.loads(settings.read_text(encoding="utf-8"))["window_mode"], "fullscreen")
                self.assertIn('set "BB_WINDOW_MODE=fullscreen"', (tmp / "env.bat").read_text(errors="replace"))
                # No saved file: none is written for it (the default needs none).
                missing = tmp / "none.json"
                self.L.write_env_bat(tmp / "env2.bat", missing)
                self.assertIn('set "BB_WINDOW_MODE=borderless"', (tmp / "env2.bat").read_text(errors="replace"))
            finally:
                self.L.bbport_ini_path = old_ini


if __name__ == "__main__":
    unittest.main()
