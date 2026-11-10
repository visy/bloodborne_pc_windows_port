from paths import build_hint, test_exe
import os
import subprocess
import unittest

EXE=test_exe('thread-release-test')

@unittest.skipUnless(EXE.exists(),build_hint('thread-release-test'))
@unittest.skipUnless(os.name=='nt','counts Windows process handles')
class ThreadReleaseTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=30)

    def test_finished_guest_threads_release_their_handle(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_kill_switch_restores_the_old_behaviour(self):
        r=self.run_case('--old');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
