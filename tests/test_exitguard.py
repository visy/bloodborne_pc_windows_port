"""src/runtime_exitguard.c: the exit deadline (parse, saves first, process ends after the deadline)."""
from paths import build_hint, test_exe
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

EXE = test_exe('exitguard-test')


@unittest.skipUnless(EXE.exists(), build_hint('exitguard-test'))
class ExitGuardTests(unittest.TestCase):
    def test_logic(self):
        r = subprocess.run([str(EXE.resolve())], capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_hung_process_ends_after_deadline_with_save_intact(self):
        with tempfile.TemporaryDirectory() as d:
            t0 = time.time()
            r = subprocess.run([str(EXE.resolve()), '--hang', d], capture_output=True, timeout=30)
            self.assertEqual(r.returncode, 7)
            self.assertLess(time.time() - t0, 10)
            self.assertEqual((Path(d) / 'save.dat').read_bytes(), b'whole')
            self.assertIn(b'the rest skipped', r.stdout)


if __name__ == '__main__':
    unittest.main()
