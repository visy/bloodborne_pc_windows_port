"""scripts/boot_image.py: the boot image is built once and reused while its inputs stay the same."""
from paths import ROOT  # noqa: F401  (puts scripts/ on sys.path)
import os
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import boot_image


def fake_build(game, out):
    """Stands in for prepare + link_libc + link_modules: every output, a line printed."""
    out.mkdir(parents=True, exist_ok=True)
    for n in boot_image.OUTPUTS:
        (out / n).write_bytes(b'x' * (len(n) + 1))
    return ['built from ' + (game / 'eboot.bin').read_text()]


class BootImageTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.game = self.root / 'game'
        (self.game / 'sce_module').mkdir(parents=True)
        (self.game / 'sce_sys').mkdir()
        (self.game / 'eboot.bin').write_text('v1')
        (self.game / 'sce_module' / 'libc.prx').write_text('libc')
        (self.game / 'sce_sys' / 'param.sfo').write_text('sfo')

    def tearDown(self):
        self.dir.cleanup()

    def image(self, out, cache=None):
        args = SimpleNamespace(game=self.game, out=out, cache_dir=cache)
        with mock.patch.object(boot_image, 'build', side_effect=fake_build) as build:
            boot_image.image(args)
        return build.call_count

    def test_reused_until_the_eboot_changes(self):
        out = self.root / 'out'
        self.assertEqual(self.image(out), 1)
        self.assertEqual(self.image(out), 0)
        (self.game / 'eboot.bin').write_text('v2')
        st = (self.game / 'eboot.bin').stat()
        os.utime(self.game / 'eboot.bin', ns=(st.st_atime_ns, st.st_mtime_ns + 10**9))
        self.assertEqual(self.image(out), 1)

    def test_a_missing_or_changed_output_rebuilds(self):
        out = self.root / 'out'
        self.image(out)
        (out / 'boot-linked.bin').write_bytes(b'')
        self.assertEqual(self.image(out), 1)
        (out / 'link.json').unlink()
        self.assertEqual(self.image(out), 1)

    def test_skip_game_check_is_part_of_the_key(self):
        out = self.root / 'out'
        self.image(out)
        with mock.patch.dict(os.environ, {'BB_SKIP_GAME_CHECK': '1'}):
            self.assertEqual(self.image(out), 1)

    def test_script_data_is_part_of_the_key(self):
        # patches/redzone.json is linked into the image: a build changing only it relinks.
        data = self.root / 'redzone.json'
        data.write_text('{"sites": []}')
        out = self.root / 'out'
        with mock.patch.object(boot_image, 'DATA', (data,)):
            self.assertEqual(self.image(out), 1)
            self.assertEqual(self.image(out), 0)
            data.write_text('{"sites": [[1, "00", [1]]]}')
            self.assertEqual(self.image(out), 1)

    def test_shared_store_is_linked_into_other_out_folders(self):
        cache = self.root / 'cache'
        self.assertEqual(self.image(self.root / 'out0', cache), 1)
        self.assertEqual(len([p for p in cache.iterdir() if not p.name.startswith('.')]), 1)
        self.assertEqual(self.image(self.root / 'out1', cache), 0)
        for n in boot_image.OUTPUTS:
            self.assertEqual((self.root / 'out1' / n).read_bytes(), (self.root / 'out0' / n).read_bytes())
        # A rebuild never writes through the links into the store.
        stored = next(p for p in cache.iterdir() if not p.name.startswith('.'))
        before = (stored / 'boot-linked.bin').read_bytes()
        with mock.patch.dict(os.environ, {'BB_BOOT_CACHE': '0'}):
            with mock.patch.object(boot_image, 'build', side_effect=lambda g, o: (
                    [(o / n).unlink() for n in boot_image.OUTPUTS if (o / n).exists()],
                    fake_build(g, o))[1]):
                boot_image.image(SimpleNamespace(game=self.game, out=self.root / 'out1', cache_dir=cache))
        self.assertEqual((stored / 'boot-linked.bin').read_bytes(), before)

    def test_a_refused_swap_is_retried(self):
        # Upstream report: an antivirus scanning the fresh file refused the swap (WinError 32).
        calls = []
        real = os.replace

        def flaky(src, dst):
            calls.append(dst)
            if len(calls) < 3:
                raise PermissionError(32, 'The process cannot access the file')
            real(src, dst)

        src = self.root / 'a'
        src.write_bytes(b'new')
        dst = self.root / 'b'
        with mock.patch.object(boot_image.time, 'sleep'):
            boot_image.patiently(flaky, src, dst)
        self.assertEqual(len(calls), 3)
        self.assertEqual(dst.read_bytes(), b'new')

    def test_a_refusal_that_lasts_is_raised(self):
        def refused():
            raise PermissionError(5, 'Access is denied')
        with mock.patch.object(boot_image.time, 'sleep'),                 mock.patch.object(boot_image.time, 'monotonic', side_effect=[0.0, 5.0, 11.0]):
            with self.assertRaises(PermissionError):
                boot_image.patiently(refused)


if __name__ == '__main__':
    unittest.main()
