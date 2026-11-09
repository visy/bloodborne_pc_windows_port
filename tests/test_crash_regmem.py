from paths import build_hint, test_exe
import subprocess
import unittest

EXE = test_exe('crash-regmem-test')


@unittest.skipUnless(EXE.exists(), build_hint('crash-regmem-test'))
class CrashRegmemTests(unittest.TestCase):
    def test_register_memory_and_hkb_site_decode(self):
        r = subprocess.run([str(EXE.resolve())], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('crash-regmem-test: OK', r.stdout)


if __name__ == '__main__':
    unittest.main()
