"""launcher.py frame generation choice (from Supermedo/bloodborne_pc PR #69 by FIocker, adapted):
bbport.ini frame_generation / fg_backend <-> the Graphics tab's choice; Off by default."""
from paths import ROOT
import importlib.util
import tempfile
import unittest
from pathlib import Path


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_fg", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FrameGenerationChoice(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_off_by_default(self):
        self.assertEqual(self.L.frame_generation_label({}), "Off")
        self.assertEqual(self.L.frame_generation_label({"frame_generation": "bad"}), "Off")
        self.assertEqual(self.L.frame_generation_ini("Off"), {"frame_generation": "off", "fg_backend": "dlss"})

    def test_round_trip(self):
        for label, factor, backend in self.L.FRAME_GENERATION_CHOICES:
            ini = self.L.frame_generation_ini(label)
            self.assertEqual(ini, {"frame_generation": factor, "fg_backend": backend})
            self.assertEqual(self.L.frame_generation_label(ini), label)

    def test_fsr_is_2x(self):
        self.assertEqual(self.L.frame_generation_label({"frame_generation": "4x", "fg_backend": "fsr"}), "FSR FG 2x")
        self.assertEqual(self.L.frame_generation_label({"frame_generation": "3", "fg_backend": "dlss"}), "DLSS FG 3x")

    def test_reflex(self):
        self.assertEqual(self.L.reflex_label({}), "Off")
        self.assertEqual(self.L.reflex_label({"reflex": "boost"}), "On + Boost")
        self.assertEqual(self.L.reflex_label({"reflex": "1"}), "On")
        self.assertEqual(self.L.reflex_label({"reflex": "nonsense"}), "Off")

    def test_saved_keys_keep_the_rest(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / "bbport.ini"
            ini.write_text("upscaler=fsr3\nkey.cross=X\nframe_generation=off\n", encoding="utf-8")
            self.L.update_ini(ini, self.L.frame_generation_ini("FSR FG 2x"))
            values = self.L.read_ini(ini)
            self.assertEqual(values["frame_generation"], "2x")
            self.assertEqual(values["fg_backend"], "fsr")
            self.assertEqual(values["upscaler"], "fsr3")
            self.assertEqual(values["key.cross"], "X")


if __name__ == "__main__":
    unittest.main()
