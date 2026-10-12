"""The stale bbport.exe guard finds, waits for and terminates only this install's processes."""
from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

import bb_guard

PING = Path(os.environ.get('SystemRoot', 'C:\\Windows')) / 'System32' / 'PING.EXE'


@unittest.skipUnless(os.name == 'nt' and PING.is_file(), 'needs Windows')
class StaleInstanceGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.procs = []
        self.addCleanup(self.kill_all)

    def kill_all(self):
        for proc in self.procs:
            proc.kill()
            proc.wait()

    def start_fake(self, folder):
        """A long-running process whose image is named bbport.exe in folder."""
        folder = Path(self.tmp.name) / folder
        folder.mkdir(parents=True, exist_ok=True)
        exe = folder / 'bbport.exe'
        shutil.copyfile(PING, exe)
        proc = subprocess.Popen([str(exe), '-n', '120', '127.0.0.1'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        self.procs.append(proc)
        return proc, folder

    def test_finds_only_the_process_of_this_install(self):
        mine, mine_dir = self.start_fake('install/out')
        _other, other_dir = self.start_fake('other/out')
        self.assertEqual([i.pid for i in bb_guard.find_instances(mine_dir)], [mine.pid])
        self.assertEqual(len(bb_guard.find_instances(other_dir)), 1)
        self.assertEqual(bb_guard.find_instances(Path(self.tmp.name) / 'empty'), [])

    def test_terminate_removes_it_and_wait_gone_reports_survivors(self):
        proc, folder = self.start_fake('out')
        self.assertEqual(bb_guard.wait_gone([proc.pid], 0.3), [proc.pid])
        self.assertTrue(bb_guard.terminate(proc.pid))
        self.assertEqual(bb_guard.wait_gone([proc.pid], 5), [])
        self.assertEqual(bb_guard.find_instances(folder), [])

    def test_command_line_terminates_after_the_grace_period_and_exits_zero(self):
        proc, folder = self.start_fake('out')
        run = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'bb_guard.py'),
                              '--exe-dir', str(folder), '--wait', '1'],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(f'terminated stale bbport.exe PID {proc.pid}', run.stdout)
        self.assertIsNotNone(proc.wait(timeout=10))

    def test_command_line_reports_a_process_that_exits_by_itself(self):
        proc, folder = self.start_fake('out')
        threading.Timer(0.5, proc.kill).start()  # the old instance finishing its exit
        run = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'bb_guard.py'),
                              '--exe-dir', str(folder), '--wait', '10'],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(f'previous bbport.exe (PID {proc.pid}', run.stdout)
        self.assertNotIn('terminated', run.stdout)

    def test_no_kill_only_reports(self):
        proc, folder = self.start_fake('out')
        run = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'bb_guard.py'),
                              '--exe-dir', str(folder), '--no-kill'],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(f'PID {proc.pid}', run.stdout)
        self.assertIsNone(proc.poll())


if __name__ == '__main__':
    unittest.main()
