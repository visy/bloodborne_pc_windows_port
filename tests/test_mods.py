from paths import ROOT, posix_env, posix_tool
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import shutil
import stat

# Not a bare 'bash': on Windows CreateProcess finds System32\bash.exe (WSL) before PATH (Git Bash).
BASH = posix_tool('bash') or 'bash'

spec = importlib.util.spec_from_file_location('bbmods', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)


class ModTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA03173'
        self.assets = self.game / 'dvdroot_ps4' / 'chr'
        self.assets.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'original executable')
        (self.assets / 'a.dcx').write_bytes(b'original')
        (self.assets / 'b.dcx').write_bytes(b'untouched')
        self.moddir = self.root / 'mods'

    def mod(self, name, contents=b'mod', file='a.dcx'):
        root = self.moddir / name
        folder = root / 'dvdroot_ps4' / 'chr'
        (folder / file).parent.mkdir(parents=True, exist_ok=True)
        (folder / file).write_bytes(contents)
        return root

    def test_merge_replacement_new_file_and_directory_listing(self):
        a = self.mod('A')
        self.mod('A', b'new', 'new.dcx')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        folder = result / 'dvdroot_ps4' / 'chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'mod')
        self.assertEqual((folder / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual((folder / 'new.dcx').read_bytes(), b'new')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx', 'new.dcx'})
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        self.assertFalse((self.assets / 'new.dcx').exists())
        self.assertEqual((result / 'eboot.bin').read_bytes(), b'original executable')

    def test_last_mod_wins_and_reordering_changes_winner(self):
        a, b = self.mod('A', b'A'), self.mod('B', b'B')
        for layers, winner in [([('A', a), ('B', b)], b'B'), ([('B', b), ('A', a)], b'A')]:
            result = mods.build_overlay(self.game, self.root / 'out', layers)
            self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), winner)

    def test_selection_disable_and_new_mod(self):
        for name in ['C', 'B', 'A']:
            self.mod(name)
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'order': ['B', 'A', 'deleted'], 'disabled': ['A']}))
        self.assertEqual(mods.selected(self.moddir, config), ['B', 'C'])

    def test_no_mod_returns_original_game(self):
        self.assertEqual(mods.build_overlay(self.game, self.root / 'out', []), self.game)
        self.assertFalse((self.root / 'out').exists())

    def test_directory_conflict_does_not_modify_base(self):
        a = self.mod('A')
        (a / 'dvdroot_ps4/chr/a.dcx').unlink()
        nested = a / 'dvdroot_ps4/chr/b.dcx'
        nested.mkdir()
        (nested / 'child').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertEqual((self.assets / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual(list((self.root / 'out').iterdir()), [])

    def test_mod_symlinks_rejected(self):
        a = self.mod('A')
        escape = a / 'dvdroot_ps4/escape'
        try:
            escape.symlink_to(self.assets, target_is_directory=True)
        except OSError:
            if os.name != 'nt':
                raise
            # Symlinks need Developer Mode or admin rights on Windows; a junction is the
            # directory link anyone can make (and what make_link falls back to).
            import _winapi
            _winapi.CreateJunction(str(self.assets), str(escape))
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def test_new_mod_directory_stays_out_of_the_game(self):
        # The overlay links the game's folders; a new folder must be made in the overlay, never
        # through a link (a junction on Windows without symlink rights) inside the game.
        a = self.mod('A', b'deep', 'sub/deep.dcx')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertEqual((result / 'dvdroot_ps4/chr/sub/deep.dcx').read_bytes(), b'deep')
        self.assertEqual({p.name for p in self.assets.iterdir()}, {'a.dcx', 'b.dcx'})

    @unittest.skipUnless(os.name == 'nt', 'Windows overlay links')
    def test_windows_overlay_files_report_their_size(self):
        # bb-probe stats with UCRT _stat64 and lists with FindFirstFileA: both report a file
        # symlink's own size (0), and the game panics loading it (FileTransferTask.cpp(865)).
        a = self.mod('A', b'replaced')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        folder = result / 'dvdroot_ps4' / 'chr'
        # DirEntry.stat(follow_symlinks=False) is FindFirstFile's data on Windows.
        sizes = {e.name: e.stat(follow_symlinks=False).st_size for e in os.scandir(folder)}
        self.assertEqual(sizes, {'a.dcx': len(b'replaced'), 'b.dcx': len(b'untouched')})
        self.assertFalse(any((folder / name).is_symlink() for name in sizes))

    def test_executable_replacement_rejected(self):
        a = self.mod('A')
        (a / 'eboot.bin').write_bytes(b'unsupported')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def test_shadps4_sibling_folder_and_global_disable(self):
        legacy = Path(str(self.game) + '-mods')
        folder = legacy / 'dvdroot_ps4/chr'
        folder.mkdir(parents=True)
        (folder / 'a.dcx').write_bytes(b'legacy')
        for enabled, expected in [('1', b'legacy'), ('0', b'original')]:
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                '--out', str(self.root / 'out'), '--mods-dir', str(self.moddir), '--enabled', enabled],
                capture_output=True, text=True, check=True)
            self.assertEqual((Path(result.stdout.strip()) / 'dvdroot_ps4/chr/a.dcx').read_bytes(), expected)

    def test_app0_wrapped_mod_discovered(self):
        self.moddir.mkdir()
        (self.moddir / 'Wrapped/app0/dvdroot_ps4').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Wrapped'])

    def test_other_case_replaces_the_game_file(self):
        root = self.moddir / 'Upper'
        (root / 'DVDROOT_PS4/Chr').mkdir(parents=True)
        (root / 'DVDROOT_PS4/Chr/A.DCX').write_bytes(b'upper')
        result = mods.build_overlay(self.game, self.root / 'out', [('Upper', root)])
        folder = result / 'dvdroot_ps4/chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'upper')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx'})
        self.assertEqual({p.name for p in result.iterdir()}, {'dvdroot_ps4', 'eboot.bin'})

    def test_wrapped_and_bare_layouts(self):
        layouts = {'Title': 'CUSA03173/dvdroot_ps4/chr', 'Archive': 'Archive v1.2/dvdroot_ps4/chr',
                   'Bare': 'chr', 'Nested': 'Nested/app0/dvdroot_ps4/chr'}
        for name, path in layouts.items():
            folder = self.moddir / name / path
            folder.mkdir(parents=True)
            (folder / 'a.dcx').write_bytes(name.encode())
        (self.moddir / 'Archive/readme.txt').write_text('notes')
        (self.moddir / 'Junk/stuff').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Archive', 'Bare', 'Nested', 'Title'])
        for name in layouts:
            with self.subTest(name=name):
                result = mods.build_overlay(self.game, self.root / 'out',
                                            [(name, self.moddir / name)])
                self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), name.encode())

    def test_mod_site_layouts(self):
        # From Supermedo/bloodborne_pc dd2314ee. As mods come from mod sites: notes without a
        # suffix and previews beside the wrapper, two wrappers deep, and the game's folders next
        # to screenshots.
        layouts = {'Readme': 'Readme v1/dvdroot_ps4/chr', 'Deep': 'Deep/Deep v2/dvdroot_ps4/chr',
                   'Shots': 'chr'}
        for name, path in layouts.items():
            folder = self.moddir / name / path
            folder.mkdir(parents=True)
            (folder / 'a.dcx').write_bytes(name.encode())
        (self.moddir / 'Readme/README').write_text('notes')
        (self.moddir / 'Readme/preview.webp').write_bytes(b'image')
        (self.moddir / 'Readme/Nexus page.url').write_text('[InternetShortcut]')
        (self.moddir / 'Shots/Screenshots').mkdir()
        (self.moddir / 'Shots/Screenshots/1.png').write_bytes(b'image')
        (self.moddir / 'Shots/readme.txt').write_text('notes')
        self.assertEqual(mods.discover(self.moddir), ['Deep', 'Readme', 'Shots'])
        for name in layouts:
            with self.subTest(name=name):
                result = mods.build_overlay(self.game, self.root / 'out',
                                            [(name, self.moddir / name)])
                self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), name.encode())
                self.assertFalse((result / 'dvdroot_ps4/Screenshots').exists())
                self.assertFalse((result / 'dvdroot_ps4/readme.txt').exists())

    def test_unrecognized_folders_say_why(self):
        self.moddir.mkdir()
        (self.moddir / 'Zipped').mkdir()
        (self.moddir / 'Zipped/mod.zip').write_bytes(b'PK')
        (self.moddir / 'Two/A/dvdroot_ps4').mkdir(parents=True)
        (self.moddir / 'Two/B/dvdroot_ps4').mkdir(parents=True)
        (self.moddir / 'Two/A/dvdroot_ps4/x').write_bytes(b'x')
        (self.moddir / 'Empty').mkdir()
        (self.moddir / 'Good/dvdroot_ps4/chr').mkdir(parents=True)
        found = dict(mods.unrecognized(self.moddir))
        self.assertEqual(sorted(found), ['Empty', 'Two', 'Zipped'])
        self.assertIn('archive', found['Zipped'])
        self.assertIn('several folders', found['Two'])
        self.assertEqual(found['Empty'], 'empty')
        # The start (run.bat -> mods.py) logs them; the launcher's log shows it.
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
            '--out', str(self.root / 'out'), '--mods-dir', str(self.moddir)],
            capture_output=True, text=True, check=True, env=dict(os.environ, BB_DREAM_MIRROR='0'))
        self.assertIn('Mods: not a mod: Zipped: still an archive', result.stderr)
        self.assertIn('Mods: not a mod: Empty: empty', result.stderr)
        self.assertNotIn('not a mod: Good', result.stderr)

    def test_invalid_profile_fails(self):
        self.mod('A')
        config = self.root / 'mods.json'
        config.write_text('{"disabled":"A"}')
        with self.assertRaises(ValueError):
            mods.selected(self.moddir, config)

    def test_read_only_game_files_are_replaced_and_keep_their_attribute(self):
        # Supermedo/bloodborne_pc ba34a3a2 (emurphy04, PR #77): a hard link shares the read-only
        # attribute of the game file, and Windows refuses to delete it (WinError 5).
        a = self.mod('A')
        self.mod('A', b'new', 'new.dcx')
        for f in (self.assets / 'a.dcx', self.assets / 'b.dcx', self.game / 'eboot.bin'):
            f.chmod(stat.S_IREAD)
            self.addCleanup(f.chmod, stat.S_IREAD | stat.S_IWRITE)
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'mod')
        self.assertEqual((result / 'dvdroot_ps4/chr/b.dcx').read_bytes(), b'untouched')
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        self.assertFalse(os.stat(self.assets / 'a.dcx').st_mode & stat.S_IWRITE)
        # A changed mod builds a new overlay; removing the overlay keeps the game files read-only.
        mods.remove_overlay(result, self.game)
        self.assertFalse(result.exists())
        for f in (self.assets / 'a.dcx', self.assets / 'b.dcx', self.game / 'eboot.bin'):
            self.assertTrue(f.is_file())
            self.assertFalse(os.stat(f).st_mode & stat.S_IWRITE, f)

    @unittest.skipUnless(os.name == 'nt', 'junctions and WinError 1 are Windows-only')
    def test_drive_without_junctions_builds_the_view_in_temp(self):
        # Supermedo/bloodborne_pc bf0878bf: on exFAT / FAT32 a folder cannot become a junction
        # ([WinError 1] "Incorrect function"); the view is made on another (NTFS) folder.
        a = self.mod('A')
        exfat, ntfs = self.root / 'exfat-out', self.root / 'temp-view'
        real = mods.make_link

        def fat_make_link(link_path, target_path, target_is_directory=False):
            if target_is_directory and Path(link_path).resolve().is_relative_to(exfat.resolve()):
                raise OSError(22, 'Incorrect function', str(link_path), 1)
            return real(link_path, target_path, target_is_directory)
        mods.make_link = fat_make_link
        self.addCleanup(setattr, mods, 'make_link', real)
        with self.assertRaises(OSError):
            mods.build_overlay(self.game, exfat, [('A', a)])
        self.assertEqual([p for p in exfat.iterdir() if p.is_dir()], [])  # nothing left behind
        result = mods.build_view(self.game, exfat, [('A', a)], fallback=ntfs)
        self.assertTrue(result.resolve().is_relative_to(ntfs.resolve()))
        self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'mod')
        self.assertEqual((result / 'dvdroot_ps4/chr/b.dcx').read_bytes(), b'untouched')
        self.assertEqual((result / 'eboot.bin').read_bytes(), b'original executable')
        # Any other error still fails the launch's mods step.
        mods.make_link = lambda *args, **kw: (_ for _ in ()).throw(OSError(13, 'denied'))
        with self.assertRaises(OSError):
            mods.build_view(self.game, self.root / 'other', [('A', a)], fallback=ntfs)

    def test_run_uses_overlay_propagates_exit_and_cleans_view(self):
        self.mod('A')
        python = self.root / 'python'
        python.write_text(f'#!{sys.executable}\nimport subprocess,sys\n'
            'if sys.argv[1] == "scripts/mods.py" or sys.argv[1] == "-c":\n'
            '    sys.exit(subprocess.call([sys.executable,*sys.argv[1:]]))\n')
        python.chmod(0o755)
        probe = self.root / 'probe'
        probe.write_text(f'#!{sys.executable}\nimport json,sys,os\nfrom pathlib import Path\n'
            'game=Path(sys.argv[sys.argv.index("--app0")+1])\n'
            'Path(os.environ["BB_DATA_DIR"],"mounted.json").write_text(json.dumps({\n'
            '"path":str(game),"content":(game/"dvdroot_ps4/chr/a.dcx").read_text()}))\n'
            'sys.exit(7)\n')
        probe.chmod(0o755)
        env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
            BB_DATA_DIR=str(self.root), BB_GAME_DIR=str(self.game),
            BB_MODS_DIR=str(self.moddir), BB_MODS_ENABLED='1', BB_MODS_CONFIG=str(self.root/'mods.json'))
        result = subprocess.run([BASH, 'run.sh'], cwd=ROOT, env=posix_env(env), capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 7, result.stderr)
        mounted = json.loads((self.root / 'mounted.json').read_text())
        self.assertEqual(mounted['content'], 'mod')
        self.assertFalse(Path(mounted['path']).exists())
        self.assertEqual((self.assets/'a.dcx').read_bytes(), b'original')


class PartyModSetTests(unittest.TestCase):
    """out/party_mods.txt: only mods that change gameplay files count in the party's version check."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def mod(self, name, files):
        root = self.root / 'mods' / name
        for relative, contents in files.items():
            path = root / 'dvdroot_ps4' / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        return root

    def test_cosmetic_files(self):
        for path in ['dvdroot_ps4/shader/gxflvershader.shaderbnd.dcx', 'dvdroot_ps4/Sound/fdlc.fsb',
                     'dvdroot_ps4/parts/wp_a_0400.partsbnd.dcx', 'dvdroot_ps4/map/m24/m24_0000.tpfbhd',
                     'dvdroot_ps4/chr/c2000.texbnd.dcx', 'dvdroot_ps4/menu/menu.tpf.dcx', 'dvdroot_ps4/sfx/frpg_sfxbnd_common.ffxbnd.dcx']:
            self.assertTrue(mods.mod_file_is_cosmetic(path), path)
        for path in ['dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx', 'dvdroot_ps4/event/m24_00_00_00.emevd.dcx',
                     'dvdroot_ps4/chr/c2000.anibnd.dcx', 'dvdroot_ps4/script/talk/t000001.talkesdbnd.dcx',
                     'dvdroot_ps4/map/mapstudio/m24_00_00_00.msb.dcx']:
            self.assertFalse(mods.mod_file_is_cosmetic(path), path)

    def test_texture_and_shader_mods_do_not_count(self):
        textures = self.mod('HD Textures', {'chr/c2000.texbnd.dcx': b'tex', 'shader/x.shaderbnd.dcx': b'shader'})
        digest, items, cosmetic = mods.party_mod_set([('HD Textures', textures)])
        self.assertEqual(digest, '0' * 64)
        self.assertEqual(items, [])
        self.assertEqual(cosmetic, ['HD Textures'])
        self.assertEqual(mods.party_mod_set([])[0], '0' * 64)

    def test_gameplay_mods_hash_contents_not_folder_names(self):
        files = {'param/gameparam/gameparam.parambnd.dcx': b'params v1', 'chr/c2000.texbnd.dcx': b'tex'}
        a = self.mod('Rebalance', files)
        b = self.mod('rebalance-1.0', files)
        cache = self.root / 'cache.json'
        digest_a, items_a, _ = mods.party_mod_set([('Rebalance', a)], cache)
        digest_b, items_b, _ = mods.party_mod_set([('rebalance-1.0', b)], cache)
        self.assertNotEqual(digest_a, '0' * 64)
        self.assertEqual(digest_a, digest_b)
        self.assertEqual(items_a, ['Rebalance (1 files)'])
        self.assertTrue(cache.is_file())
        # Same size, other bytes: another hash (contents, not sizes).
        self.mod('rebalance-1.0', {'param/gameparam/gameparam.parambnd.dcx': b'params v2'})
        self.assertNotEqual(mods.party_mod_set([('rebalance-1.0', b)], cache)[0], digest_a)
        # A texture-only change in the gameplay mod does not matter.
        self.mod('Rebalance', {'chr/c2000.texbnd.dcx': b'other texture'})
        self.assertEqual(mods.party_mod_set([('Rebalance', a)], cache)[0], digest_a)

    def test_main_writes_the_party_file(self):
        self.mod('Params', {'param/gameparam/gameparam.parambnd.dcx': b'p'})
        self.mod('Shaders', {'shader/x.shaderbnd.dcx': b's'})
        game = self.root / 'game'
        (game / 'dvdroot_ps4').mkdir(parents=True)
        (game / 'eboot.bin').write_bytes(b'eboot')
        out = self.root / 'out'
        out.mkdir()
        (out / mods.PARTY_MODS_FILE).write_text('stale')
        # Mods off: the file says none.
        subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(game), '--out', str(out),
                        '--mods-dir', str(self.root / 'mods'), '--enabled', '0'], check=True, capture_output=True)
        text = (out / mods.PARTY_MODS_FILE).read_text(encoding='utf-8')
        self.assertIn('hash ' + '0' * 64, text.splitlines())
        self.assertFalse([line for line in text.splitlines() if line.startswith('mod ')])
        # The layers themselves (build_overlay links need privileges on Windows): party_mod_set.
        layers = [(n, self.root / 'mods' / n) for n in mods.selected(self.root / 'mods', None)]
        digest, items = mods.write_party_mods_file(out, layers)
        text = (out / mods.PARTY_MODS_FILE).read_text(encoding='utf-8').splitlines()
        self.assertIn(f'hash {digest}', text)
        self.assertIn('mod Params (1 files)', text)
        self.assertIn('# cosmetic Shaders', text)
