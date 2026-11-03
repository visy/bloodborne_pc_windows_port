"""run.bat derives BB_LIVE_FPS and BB_VBLANK_HZ again after a party relaunch (exit code 75 jumps to
:prepare_patches), so a Live FPS change made in the game's menu (bbport.ini live_fps) is not lost."""
import unittest
from pathlib import Path

BAT = (Path(__file__).resolve().parent.parent / 'run.bat').read_text(encoding='utf-8')


class RunBatLiveFpsTests(unittest.TestCase):
    def test_environment_is_saved_once_before_the_derivation(self):
        saved = BAT.index('if not defined RUN_ENV_SAVED')
        self.assertLess(saved, BAT.index(':prepare_patches\n'))
        self.assertIn('set "RUN_LIVE_ENV=%BB_LIVE_FPS%"', BAT)
        self.assertIn('set "RUN_VBLANK_ENV=%BB_VBLANK_HZ%"', BAT)

    def test_derivation_is_inside_the_relaunch_loop(self):
        loop = BAT.index(':prepare_patches\n')
        patches_call = BAT.index('scripts\patches.py --out')
        live = BAT.index('set "BB_LIVE_FPS=%RUN_LIVE_ENV%"')
        vblank = BAT.index('set "BB_VBLANK_HZ=%RUN_VBLANK_ENV%"')
        self.assertTrue(loop < live < patches_call, 'BB_LIVE_FPS is read from the ini before patches.py runs')
        self.assertGreater(vblank, loop)
        self.assertIn('live_fps', BAT[live:patches_call])
        self.assertIn('goto prepare_patches', BAT)
        self.assertIn('set "fps=%BB_FPS%"', BAT[loop:live])

    def test_only_the_users_own_values_survive(self):
        # The derived values are never read back as the user's: the loop starts from RUN_*_ENV every pass.
        self.assertEqual(BAT.count('set "BB_LIVE_FPS=%RUN_LIVE_ENV%"'), 1)
        self.assertEqual(BAT.count('set "BB_VBLANK_HZ=%RUN_VBLANK_ENV%"'), 1)


if __name__ == '__main__':
    unittest.main()
