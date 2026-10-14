"""launcher.py HDR output options (I4): bbport.ini hdr / hdr_peak_nits / hdr_paper_white <-> the
Graphics tab's checkbox and choices; off by default, a custom value from the game menu kept."""
from paths import ROOT
import importlib.util
import tempfile
import unittest
from pathlib import Path


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_hdr", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class HdrOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_defaults(self):
        self.assertEqual(self.L.hdr_peak_label({}), "Auto (display)")
        self.assertEqual(self.L.hdr_peak_label({"hdr_peak_nits": "0"}), "Auto (display)")
        self.assertEqual(self.L.hdr_paper_white_label({}), "200 nits (default)")
        self.assertEqual(self.L.hdr_ini(False, "Auto (display)", "200 nits (default)"),
                         {"hdr": "0", "hdr_peak_nits": "auto", "hdr_paper_white": "200"})

    def test_round_trip(self):
        for label, value in self.L.HDR_PEAK_CHOICES:
            ini = self.L.hdr_ini(True, label, "200 nits (default)")
            self.assertEqual(ini["hdr_peak_nits"], value)
            self.assertEqual(self.L.hdr_peak_label(ini), label)
        for label, value in self.L.HDR_PAPER_WHITE_CHOICES:
            ini = self.L.hdr_ini(True, "Auto (display)", label)
            self.assertEqual(ini["hdr_paper_white"], value)
            self.assertEqual(self.L.hdr_paper_white_label(ini), label)

    def test_custom_values_kept(self):
        # The game menu's 1200 nits / 180 nits are not launcher choices: shown and written back as they are.
        ini = {"hdr_peak_nits": "1200", "hdr_paper_white": "180"}
        peak, paper = self.L.hdr_peak_label(ini), self.L.hdr_paper_white_label(ini)
        self.assertEqual((peak, paper), ("1200 nits", "180 nits"))
        self.assertEqual(self.L.hdr_ini(True, peak, paper),
                         {"hdr": "1", "hdr_peak_nits": "1200", "hdr_paper_white": "180"})

    def test_saved_keys_keep_the_rest(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / "bbport.ini"
            ini.write_text("upscaler=dlss\nkey.cross=X\nhdr=0\nhdr_strength=1.20\n", encoding="utf-8")
            self.L.update_ini(ini, self.L.hdr_ini(True, "1000 nits", "250 nits"))
            values = self.L.read_ini(ini)
            self.assertEqual((values["hdr"], values["hdr_peak_nits"], values["hdr_paper_white"]),
                             ("1", "1000", "250"))
            self.assertEqual(values["hdr_strength"], "1.20")
            self.assertEqual(values["upscaler"], "dlss")
            self.assertEqual(values["key.cross"], "X")


if __name__ == "__main__":
    unittest.main()
