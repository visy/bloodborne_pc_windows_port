"""tools/windows/hdr_check.py: its mirror of the HDR mapping (gpu/shim/bbport_hdr.h) and the
10-bit unpacking it reads the game's dumps with."""
from paths import ROOT
import importlib.util
import unittest

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


def load():
    spec = importlib.util.spec_from_file_location("hdr_check", ROOT / "tools" / "windows" / "hdr_check.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@unittest.skipIf(np is None, "numpy missing")
class HdrCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.H = load()

    def test_pq_reference_values(self):
        self.assertAlmostEqual(float(self.H.pq_encode(np.array(100.0))), 0.5081, places=3)
        self.assertAlmostEqual(float(self.H.pq_encode(np.array(1000.0))), 0.7518, places=3)
        self.assertAlmostEqual(float(self.H.pq_decode(self.H.pq_encode(np.array(640.0)))), 640.0, places=1)

    def test_mid_tones_kept_white_at_peak(self):
        px = np.array([[[0.5, 0.5, 0.5], [1.0, 1.0, 1.0], [0.0, 0.0, 0.0]]])
        pq, nits = self.H.expected_pq(px, 1000.0, 200.0, 1.0, 1.0)
        self.assertAlmostEqual(nits[0, 0, 1], 0.5 ** 2.2 * 200.0, places=3)
        self.assertAlmostEqual(nits[0, 1, 0], 1000.0, places=2)
        self.assertAlmostEqual(nits[0, 2, 2], 0.0, places=6)

    def test_unpack_a2b10g10r10(self):
        r, g, b = 1023, 512, 0
        word = (3 << 30) | (b << 20) | (g << 10) | r
        rgb = self.H.unpack_1010102(np.array([word], dtype="<u4").tobytes(), 1, 1)
        self.assertAlmostEqual(rgb[0, 0, 0], 1.0)
        self.assertAlmostEqual(rgb[0, 0, 1], 512 / 1023)
        self.assertAlmostEqual(rgb[0, 0, 2], 0.0)


if __name__ == "__main__":
    unittest.main()
