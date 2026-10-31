"""src/runtime_savecommit.c: the save commit runs in the background; ordering, and what a kill leaves."""
from paths import build_hint, test_exe
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

EXE = test_exe('savecommit-test')


@unittest.skipUnless(EXE.exists(), build_hint('savecommit-test'))
class SaveCommitTests(unittest.TestCase):
    def check(self, path):
        r = subprocess.run([str(EXE.resolve()), '--check', str(path)], capture_output=True, text=True, timeout=20)
        return int(r.stdout.strip())

    def test_ordering_and_waiting(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([str(EXE.resolve()), d], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def kill_at(self, stage):
        with tempfile.TemporaryDirectory() as d:
            p = subprocess.Popen([str(EXE.resolve()), '--child', stage, d])
            try:
                marker = Path(d) / 'reached'
                for _ in range(200):
                    if marker.exists() or p.poll() is not None:
                        break
                    time.sleep(0.05)
                self.assertTrue(marker.exists(), 'the commit never reached ' + stage)
            finally:
                p.kill()
                p.wait()
            return self.check(Path(d) / 'file.dat')

    def test_kill_before_rename_keeps_the_old_file_whole(self):
        self.assertEqual(self.kill_at('before-rename'), 0xA1)

    def test_kill_after_rename_has_the_new_file_whole(self):
        self.assertEqual(self.kill_at('after-rename'), 0xB2)


if __name__ == '__main__':
    unittest.main()
