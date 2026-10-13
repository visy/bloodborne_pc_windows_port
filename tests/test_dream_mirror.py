"""scripts/dream_mirror.py (the Hunter's Dream mirror, ported from bbhost) and its mods.py layer."""
from paths import ROOT
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(ROOT / 'scripts'))
import dream_mirror as dm  # noqa: E402

spec = importlib.util.spec_from_file_location('bbmods_dm', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)

GAME = Path(os.environ.get('BB_GAME_DIR') or '/nonexistent')  # only a dump named by BB_GAME_DIR


class DreamMirrorTests(unittest.TestCase):
    def test_dcx_round_trip(self):
        raw = bytes(range(256)) * 50
        packed = dm.dcx_compress(raw)
        self.assertEqual(packed[:4], b'DCX\0')
        self.assertEqual(packed[0x28:0x2c], b'DFLT')
        self.assertEqual(struct.unpack_from('>I', packed, 0x1c)[0], len(raw))
        self.assertEqual(dm.dcx_decompress(packed), raw)
        with self.assertRaises(dm.LayoutError):
            dm.dcx_decompress(b'not a dcx' * 10)

    def test_remap_moves_the_part_and_shifts_the_range(self):
        self.assertEqual(dm.remap(dm.FROM), dm.TO)
        self.assertEqual(dm.remap(dm.TO), dm.TO + 1)
        self.assertEqual(dm.remap(dm.FROM - 1), dm.FROM)
        self.assertEqual(dm.remap(dm.TO - 1), dm.TO - 1)
        self.assertEqual(dm.remap(dm.FROM + 1), dm.FROM + 1)
        # a permutation of the slots
        slots = range(dm.TO - 2, dm.FROM + 3)
        self.assertEqual(sorted(dm.remap(s) for s in slots), list(slots))

    def test_other_layouts_are_refused(self):
        with self.assertRaises(dm.LayoutError):
            dm.mirror_layout(b'\0' * 4096)

    def test_switch(self):
        self.assertTrue(dm.enabled({}))
        self.assertTrue(dm.enabled({'BB_DREAM_MIRROR': '1'}))
        for off in ('0', 'off', 'false'):
            self.assertFalse(dm.enabled({'BB_DREAM_MIRROR': off}))

    def test_layer_without_the_game_file_is_none(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertIsNone(dm.ensure_layer(Path(t) / 'game', Path(t) / 'out', log=lambda m: None))

    def test_generated_layer_hashes_by_stamp(self):
        self.assertEqual(mods.GENERATED_STAMPS, {dm.LAYER_NAME: dm.STAMP})
        with tempfile.TemporaryDirectory() as t:
            hashes = []
            for content in (b'zlib build A', b'zlib build B'):
                layer = Path(t) / content.decode()
                f = layer / dm.LAYOUT
                f.parent.mkdir(parents=True)
                f.write_bytes(content)
                hashes.append(mods.party_mod_set([(dm.LAYER_NAME, layer)])[0])
            self.assertEqual(hashes[0], hashes[1])
            self.assertNotEqual(hashes[0], '0' * 64)
            other = mods.party_mod_set([('Some mod', Path(t) / 'zlib build A')])[0]
            self.assertNotEqual(other, hashes[0])

    @unittest.skipUnless((GAME / dm.LAYOUT).is_file(), 'no game dump')
    def test_the_real_layout(self):
        with tempfile.TemporaryDirectory() as t:
            layer = dm.ensure_layer(GAME, Path(t), log=lambda m: None)
            self.assertIsNotNone(layer)
            msb = dm.dcx_decompress((layer / dm.LAYOUT).read_bytes())
            self.assertEqual(hashlib.sha256(msb).hexdigest(), dm.OUTPUT_SHA256)
            parts = dm.read_lists(msb)[3][1]
            moved = parts[dm.TO]
            self.assertEqual(struct.unpack_from('<i', msb, moved + 0x14)[0], dm.ENEMY)
            self.assertEqual(struct.unpack_from('<i', msb, moved + 0x10c)[0], dm.MIRROR_ENTITY)
            self.assertEqual(struct.unpack_from('<i', msb, moved + 0x130)[0], dm.MIRROR_TALK)
            mtime = (layer / dm.LAYOUT).stat().st_mtime_ns
            self.assertEqual(dm.ensure_layer(GAME, Path(t), log=lambda m: None), layer)  # reused
            self.assertEqual((layer / dm.LAYOUT).stat().st_mtime_ns, mtime)


class OverlayReuseTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'win32', 'junctions are Windows only')
    def test_junction_is_a_link_on_every_python(self):
        # Python 3.10 / 3.11 (run.bat's pick) have no Path.is_junction: the overlay once wrote
        # into the game folder through a junction it took for a real directory.
        import _winapi
        with tempfile.TemporaryDirectory() as t:
            target, link = Path(t) / 'target', Path(t) / 'link'
            target.mkdir()
            _winapi.CreateJunction(str(target), str(link))
            self.assertTrue(mods.is_link(link))
            self.assertFalse(mods.is_link(target))
            os.rmdir(link)


    def test_same_mods_reuse_the_overlay(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            game = root / 'game'
            (game / 'dvdroot_ps4/chr').mkdir(parents=True)
            (game / 'eboot.bin').write_bytes(b'x')
            (game / 'dvdroot_ps4/chr/a.dcx').write_bytes(b'orig')
            mod = root / 'mods/A/dvdroot_ps4/chr'
            mod.mkdir(parents=True)
            (mod / 'a.dcx').write_bytes(b'mod')
            first = mods.build_overlay(game, root / 'out', [('A', root / 'mods/A')])
            second = mods.build_overlay(game, root / 'out', [('A', root / 'mods/A')])
            self.assertEqual(first, second)
            self.assertEqual((second / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'mod')
            (mod / 'a.dcx').write_bytes(b'mod, changed')
            third = mods.build_overlay(game, root / 'out', [('A', root / 'mods/A')])
            self.assertNotEqual(third, first)
            self.assertEqual((third / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'mod, changed')


if __name__ == '__main__':
    unittest.main()
