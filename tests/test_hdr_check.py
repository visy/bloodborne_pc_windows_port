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

    def _dump(self, d, out_name, out_bytes, frame):
        h, w = frame.shape[:2]
        codes = np.round(np.clip(frame, 0, 1) * 1023).astype("<u4")
        words = (3 << 30) | (codes[..., 2] << 20) | (codes[..., 1] << 10) | codes[..., 0]
        (d / f"f001_hdr_frame_{w}x{h}_a2b10g10r10.raw").write_bytes(words.astype("<u4").tobytes())
        (d / out_name).write_bytes(out_bytes)
        (d / "log.txt").write_text(f"HDR dump params: encoding={2 if 'rgba16f' in out_name else 1} peak=1000.0 "
                                   f"paper_white=200.0 strength=1.000 saturation=1.000 fit=0,0,{w},{h} pattern=0\n")

    def _run(self, d):
        import contextlib, io, sys
        buf = io.StringIO()
        argv = sys.argv
        sys.argv = ["hdr_check", str(d), "--log", str(d / "log.txt")]
        try:
            with contextlib.redirect_stdout(buf):
                rc = self.H.main()
        finally:
            sys.argv = argv
        return rc, buf.getvalue()

    def test_scrgb_dump_checked_and_tone(self):
        # scRGB output (linear Rec.709, 1.0 = 80 nits): compared as PQ codes; the tone line reports
        # the mid-tones at the SDR light (ratio 1) and the highlights brighter.
        import tempfile
        from pathlib import Path
        frame = np.linspace(0.05, 1.0, 4 * 8 * 3).reshape(4, 8, 3)
        frame = np.round(frame * 1023) / 1023
        _, nits = self.H.expected_pq(frame, 1000.0, 200.0, 1.0, 1.0)
        scrgb = np.concatenate([nits @ np.linalg.inv(self.H.TO2020).T / 80.0, np.ones((4, 8, 1))], axis=-1)
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            self._dump(d, "f001_hdr_out_8x4_rgba16f.raw", scrgb.astype("<f2").tobytes(), frame)
            rc, text = self._run(d)
            self.assertEqual(rc, 0, text)
            self.assertIn("scRGB as PQ codes", text)
            self.assertIn("tone (SDR vs HDR): mid-tones", text)
            # A wrong picture (everything at half the light) fails.
            self._dump(d, "f001_hdr_out_8x4_rgba16f.raw", (scrgb * [0.5, 0.5, 0.5, 1]).astype("<f2").tobytes(), frame)
            rc, text = self._run(d)
            self.assertEqual(rc, 1, text)


if __name__ == "__main__":
    unittest.main()
