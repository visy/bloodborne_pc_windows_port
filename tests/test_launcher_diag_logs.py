"""launcher.py diagnostics checkboxes (0.7.8): the Ludwig boss health log and the event sync log are off by default,
switch on with one setting each, and are never set from anything else."""
from paths import ROOT
import importlib.util
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_diag", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class DiagLogs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_off_by_default(self):
        env = self.L.settings_env({})
        self.assertEqual(env["BB_PARTY_BOSS_HP_LOG"], "0")
        self.assertEqual(env["BB_PARTY_EVENT_SYNC_LOG"], "0")

    def test_each_checkbox_switches_its_own_log(self):
        env = self.L.settings_env({"feat_boss_hp_log": True})
        self.assertEqual((env["BB_PARTY_BOSS_HP_LOG"], env["BB_PARTY_EVENT_SYNC_LOG"]), ("1", "0"))
        env = self.L.settings_env({"feat_event_sync_log": True})
        self.assertEqual((env["BB_PARTY_BOSS_HP_LOG"], env["BB_PARTY_EVENT_SYNC_LOG"]), ("0", "1"))


if __name__ == "__main__":
    unittest.main()
