"""eboot.elf next to eboot.bin (Mrsuss60/bloodborne_pc_windows_port 2054129f / d7b2db11 by facundo1701,
PR #189 there): a game folder whose executable is a raw ELF named eboot.elf works wherever eboot.bin
does. Synthetic files only; the image hash (what the check and the co-op identity compare) is the
same for the SELF and the raw ELF of one executable."""
from paths import ROOT
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import boot_image
import eboot_file
import game_check
import mods
from prepare import GameCheckError, parse_self, prepare
from test_game_check import param_sfo
from test_prepare import fixture


def raw_elf():
    """The ELF inside the synthetic SELF, as an Itemzflow-style dump writes it."""
    return bytes(parse_self(bytes(fixture()))[0])


class FindEboot(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.game = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_none_bin_elf_and_both(self):
        self.assertIsNone(eboot_file.find_eboot(self.game))
        self.assertEqual(eboot_file.eboot_path(self.game).name, 'eboot.bin')
        (self.game / 'eboot.elf').write_bytes(raw_elf())
        self.assertEqual(eboot_file.find_eboot(self.game).name, 'eboot.elf')
        (self.game / 'eboot.bin').write_bytes(bytes(fixture()))
        self.assertEqual(eboot_file.find_eboot(self.game).name, 'eboot.bin')  # bin wins

    def test_a_folder_is_not_an_eboot(self):
        (self.game / 'eboot.bin').mkdir()
        self.assertIsNone(eboot_file.find_eboot(self.game))


class ImageHash(unittest.TestCase):
    """The clean-1.09 check hashes the loaded image, so a raw-ELF dump of the same executable passes
    with the same constant: no second hash for the ELF form is needed or kept."""

    def game(self, name, content):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name)
        (path / 'sce_sys').mkdir()
        (path / 'sce_sys/param.sfo').write_bytes(param_sfo({'APP_VER': '01.09', 'TITLE_ID': 'CUSA03173'}))
        (path / name).write_bytes(content)
        return path

    def test_elf_and_self_give_the_same_image_hash(self):
        a = self.game('eboot.bin', bytes(fixture()))
        b = self.game('eboot.elf', raw_elf())
        self.assertEqual(game_check.image_sha256(a), game_check.image_sha256(b))

    def test_an_elf_only_folder_passes_the_check_like_the_self(self):
        game = self.game('eboot.elf', raw_elf())
        with mock.patch.object(game_check, 'SUPPORTED_IMAGE', game_check.image_sha256(game)):
            self.assertIsNone(game_check.problem(game))
            self.assertTrue(game_check.eboot_clean(game))
        self.assertEqual(game_check.problem(game)[0], 'wrong_eboot')  # the real constant: not clean

    def test_the_pass_cache_key_follows_the_file_in_use(self):
        game = self.game('eboot.elf', raw_elf())
        k1 = game_check._pass_key(game)
        (game / 'eboot.bin').write_bytes(bytes(fixture()))
        self.assertNotEqual(k1, game_check._pass_key(game))

    def test_the_legacy_cpu_source_hash_of_a_clean_elf_is_the_reviewed_one(self):
        parsed = parse_self(bytes(fixture()))
        module = dict(elf=parsed[0], ph=parsed[2], sha256='f' * 64, raw_elf=True)
        image = eboot_file.image_sha256(module['elf'], module['ph'])
        self.assertEqual(eboot_file.reviewed_source_sha256(module, image, 'a' * 64), 'a' * 64)
        self.assertEqual(eboot_file.reviewed_source_sha256(module, '0' * 64, 'a' * 64), 'f' * 64)
        module['raw_elf'] = False  # a SELF keeps its own hash
        self.assertEqual(eboot_file.reviewed_source_sha256(module, image, 'a' * 64), 'f' * 64)


class PrepareUsesTheFile(unittest.TestCase):
    def run_prepare(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / 'game'
            game.mkdir()
            for name, content in files.items():
                (game / name).write_bytes(content)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                try:
                    prepare(game, Path(tmp) / 'out')
                except Exception as error:
                    return error, out.getvalue()
        return None, out.getvalue()

    def test_missing_names_both_files(self):
        error, _ = self.run_prepare({})
        self.assertIsInstance(error, FileNotFoundError)
        self.assertIn('eboot.elf', str(error))

    def test_elf_only_is_read_and_logged(self):
        error, log = self.run_prepare({'eboot.elf': raw_elf()})
        self.assertIsInstance(error, GameCheckError)  # got as far as the game check
        self.assertIn('eboot.elf', log)

    def test_both_files_use_the_bin_and_say_so(self):
        error, log = self.run_prepare({'eboot.elf': b'', 'eboot.bin': bytes(fixture())})
        self.assertIsInstance(error, GameCheckError)  # the empty eboot.elf was not touched
        self.assertIn('using eboot.bin', log)

    def test_empty_and_truncated_elf_name_the_file(self):
        error, _ = self.run_prepare({'eboot.elf': b''})
        self.assertRegex(str(error), r'eboot\.elf at .* is empty')
        error, _ = self.run_prepare({'eboot.elf': b'\x7fELF'})
        self.assertRegex(str(error), r'eboot\.elf at .* is truncated')


class BootImageKey(unittest.TestCase):
    def test_key_changes_when_the_executable_file_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            (game / 'eboot.elf').write_bytes(b'abc')
            k1 = boot_image.key(game)
            (game / 'eboot.bin').write_bytes(b'abc')
            self.assertNotEqual(k1, boot_image.key(game))


class ModsProtectTheElf(unittest.TestCase):
    def test_a_mod_cannot_replace_eboot_elf(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / 'mod'
            (folder / 'dvdroot_ps4').mkdir(parents=True)
            (folder / 'eboot.elf').write_bytes(b'x')
            with self.assertRaisesRegex(ValueError, 'Executable/system replacement'):
                list(mods.mod_files(folder))


class LauncherResolve(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('bb_launcher_eboot', ROOT / 'launcher.py')
        cls.L = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(cls.L)
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f'launcher.py not importable here: {ex}')

    def test_resolve_game_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            self.assertIsNone(self.L.resolve_game_dir(str(game)))
            (game / 'eboot.elf').write_bytes(b'x')
            self.assertEqual(self.L.resolve_game_dir(str(game)), game)
            self.assertEqual(self.L.resolve_game_dir(str(game / 'eboot.elf')), game)
            self.assertEqual(self.L.find_eboot(game).name, 'eboot.elf')
            (game / 'eboot.bin').write_bytes(b'x')
            self.assertEqual(self.L.find_eboot(game).name, 'eboot.bin')


if __name__ == '__main__':
    unittest.main()
