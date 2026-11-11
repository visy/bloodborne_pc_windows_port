"""tools/release: the refuse rules, the stamp checks and the publish dry run (temp dirs, no network)."""
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import shutil
import unittest
from unittest import mock
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'tools' / 'release'))
import build_release  # noqa: E402
import configure_pgo  # noqa: E402
import make_zip  # noqa: E402
import publish  # noqa: E402
import release_common as rc  # noqa: E402
import stage as stage_mod  # noqa: E402

VERSION = '9.8.7'


def touch(root, rel, data=b'x'):
    p = Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return p


def good_stage(root):
    root = Path(root)
    for rel in ('CHANGES.md', 'LICENSE', 'README.md', 'VERSION', 'launcher.py', 'run.bat',
                'docs/COOP.md', 'scripts/prepare.py', 'patches/redzone.json', 'tools/legacy-cpu-plan.json',
                'mods/readme.txt', 'user/readme.txt', 'user/custom_trophy/Notice.txt',
                'out/SDL3.dll', 'out/libgcc_s_seh-1.dll', 'python/Lib/site-packages/certifi/cacert.pem'):
        touch(root, rel)
    (root / 'user' / 'trophy').mkdir()  # empty skeleton folders are fine
    touch(root, 'out/bbport.exe', b'MZ fake exe')
    touch(root, 'out/bb-zstdpack.exe', b'MZ fake zstdpack')
    return root


def refused(stage, **kw):
    return rc.check_stage(stage, **kw)


class CheckStageTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.stage = good_stage(Path(self._t.name) / 'stage')

    def tearDown(self):
        self._t.cleanup()

    def assertRefuses(self, rel, data=b'x', text=None):
        touch(self.stage, rel, data)
        bad = refused(self.stage)
        self.assertTrue(any(rel in b and (text is None or text in b) for b in bad), (rel, bad))

    def test_good_stage_passes(self):
        self.assertEqual(refused(self.stage), [])

    def test_logs_ini_env_keys(self):
        self.assertRefuses('out/bbport.log', text='log')
        self.assertRefuses('out/bbport.ini', text='ini')
        self.assertRefuses('scripts/.env', text='.env')
        self.assertRefuses('scripts/.env.local', text='.env')
        self.assertRefuses('scripts/deploy.key', text='key')
        self.assertRefuses('scripts/id_rsa', text='key')
        self.assertRefuses('scripts/cert.pfx', text='key')

    def test_saves_and_player_logs(self):
        self.assertRefuses('user/savedata/slot0.dat', text='save')
        self.assertRefuses('docs/player_logs/case1/a.txt', text='save')
        self.assertRefuses('out/userdata/x.bin', text='save')

    def test_user_only_two_notes(self):
        self.assertRefuses('user/home/x.txt', text='user')
        self.assertRefuses('user/custom_trophy/extra.txt', text='user')

    def test_top_level_allowlist(self):
        touch(self.stage, 'agents/x.txt')
        touch(self.stage, 'secrets.txt')
        touch(self.stage, 'pgo/a.gcda')
        bad = ' | '.join(refused(self.stage))
        for name in ('agents', 'secrets.txt', 'pgo'):
            self.assertIn('top-level', bad)
            self.assertIn(name, bad)

    def test_game_files(self):
        self.assertRefuses('out/eboot.bin', text='game')
        self.assertRefuses('out/game.elf', text='game')
        self.assertRefuses('docs/dvdroot/a.txt', text='game')
        self.assertRefuses('scripts/regulation.dcx', text='game')
        self.assertRefuses('scripts/param_dump.tsv', text='dump')
        self.assertRefuses('docs/msg-export/x.json', text='dump')

    def test_big_bin_needs_allowlist(self):
        big = b'\0' * (2 * 1024 * 1024)
        touch(self.stage, 'out/blob.bin', big)
        self.assertEqual(refused(self.stage), [])  # under the default 5 MB
        self.assertTrue(refused(self.stage, bin_mb=1))
        self.assertEqual(refused(self.stage, bin_mb=1, allow=['out/blob.bin']), [])

    def test_debug_symbols_stay_out(self):
        self.assertRefuses('out/bbport.exe.debug', text='symbols')

    def test_pycache_is_ignored(self):
        touch(self.stage, 'scripts/__pycache__/a.cpython-312.pyc')
        touch(self.stage, 'scripts/__pycache__/x.log')
        self.assertEqual(refused(self.stage), [])


class MakeZipTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.tmp = Path(self._t.name)
        self.stage = good_stage(self.tmp / 'stage')

    def tearDown(self):
        self._t.cleanup()

    def test_writes_zip_with_directory_entries(self):
        import zipfile
        out = self.tmp / 'r.zip'
        f = make_zip.build_zip(self.stage, out)
        with zipfile.ZipFile(out) as z:
            names = z.namelist()
            self.assertIn('out/', names)
            self.assertIn('user/trophy/', names)  # an empty folder keeps its entry
            self.assertIn('out/bbport.exe', names)
        self.assertEqual(f['exe_sha256'], rc.sha256(self.stage / 'out' / 'bbport.exe'))
        self.assertEqual(f['zip_sha256'], rc.sha256(out))
        self.assertGreater(f['files'], 5)

    def test_refuses_and_writes_nothing(self):
        touch(self.stage, 'out/bbport.log')
        out = self.tmp / 'r.zip'
        with self.assertRaises(SystemExit) as cm:
            make_zip.build_zip(self.stage, out)
        self.assertIn('REFUSED', str(cm.exception))
        self.assertFalse(out.exists())

    def test_refuses_existing_output(self):
        out = touch(self.tmp, 'r.zip')
        with self.assertRaises(SystemExit):
            make_zip.build_zip(self.stage, out)
        self.assertEqual(out.read_bytes(), b'x')

    def test_stamp_must_match_exe(self):
        Path(str(self.stage) + '.stamp.json').write_text(json.dumps({'exe_sha256': '0' * 64}))
        with self.assertRaises(SystemExit) as cm:
            make_zip.build_zip(self.stage, self.tmp / 'r.zip')
        self.assertIn('changed since', str(cm.exception))


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), '-c', 'user.name=t', '-c', 'user.email=t@example.com', *args],
                          check=True, capture_output=True, text=True).stdout.strip()


class StageTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.tmp = Path(self._t.name)
        self.repo = self.tmp / 'repo'
        self.repo.mkdir()
        git(self.repo, 'init', '-q')
        for rel in ('CHANGES.md', 'LICENSE', 'README.md', 'launcher.py', 'run.bat', 'docs/COOP.md',
                    'docs/superpowers/plan.md', 'scripts/prepare.py', 'tools/legacy-cpu-plan.json',
                    'tools/other.py', 'src/main.c'):
            touch(self.repo, rel)
        touch(self.repo, 'VERSION', VERSION.encode() + b'\n')
        git(self.repo, 'add', '.')
        git(self.repo, 'commit', '-q', '-m', 'v')
        self.head = git(self.repo, 'rev-parse', 'HEAD')
        self.build = self.tmp / 'build'
        for dll in ('SDL3.dll', 'libgcc_s_seh-1.dll'):
            touch(self.build, dll)
        self.set_exe(f'v0.1-5-g{self.head[:10]}')
        touch(self.build, 'bbport.exe.debug', b'dbg')
        touch(self.build, 'bb-zstdpack.exe', b'MZ fake zstdpack')
        self.base = self.tmp / 'base'
        touch(self.base, 'out/libx.dll')
        touch(self.base, 'user/readme.txt')
        touch(self.base, 'user/home/save.dat')  # base leftovers under user\ must not travel
        touch(self.base, 'mods/readme.txt')
        touch(self.base, 'fsr4_shaders/a.spv')

    def tearDown(self):
        self._t.cleanup()

    def set_exe(self, bid):
        touch(self.build, 'bbport.exe', b'MZ..' + f'{VERSION} ({bid})'.encode() + bytes([0]) + b'tail')

    def run_stage(self, *extra):
        argv = [VERSION, str(self.build), str(self.tmp / 'rel9'), '--repo', str(self.repo), '--base',
                str(self.base), '--no-run', *extra]
        with contextlib.redirect_stdout(io.StringIO()):
            return stage_mod.main(argv)

    def test_stage_builds_folder_symbols_and_stamp(self):
        self.assertEqual(self.run_stage(), 0)
        st = self.tmp / 'rel9'
        self.assertTrue((st / 'docs' / 'COOP.md').is_file())
        self.assertFalse((st / 'docs' / 'superpowers').exists())
        self.assertFalse((st / 'src').exists())
        self.assertTrue((st / 'tools' / 'legacy-cpu-plan.json').is_file())
        self.assertFalse((st / 'tools' / 'other.py').exists())
        self.assertTrue((st / 'out' / 'libx.dll').is_file())
        self.assertFalse((st / 'user' / 'home' / 'save.dat').exists())
        self.assertTrue((self.tmp / 'rel' / 'symbols' / VERSION / 'bbport.exe.debug').is_file())
        self.assertFalse((st / 'out' / 'bbport.exe.debug').exists())
        stamp = rc.read_stamp(str(st) + '.stamp.json')
        self.assertEqual(stamp['commit'], self.head)
        self.assertEqual(stamp['version'], VERSION)
        self.assertEqual(stamp['exe_sha256'], rc.sha256(st / 'out' / 'bbport.exe'))

    def test_refuses_exe_from_another_commit(self):
        self.set_exe('v0.1-5-gdeadbeef12')
        with self.assertRaises(SystemExit) as cm:
            self.run_stage()
        self.assertIn('built from', str(cm.exception))
        self.assertFalse((self.tmp / 'rel9').exists())

    def test_refuses_dirty_exe_unless_allowed(self):
        self.set_exe(f'v0.1-5-g{self.head[:10]}-dirty')
        with self.assertRaises(SystemExit):
            self.run_stage()
        self.assertEqual(self.run_stage('--allow-dirty'), 0)

    def commit_changes(self, text):
        touch(self.repo, 'CHANGES.md', text.encode())
        git(self.repo, 'add', 'CHANGES.md')
        git(self.repo, 'commit', '-q', '-m', 'changes')
        self.head = git(self.repo, 'rev-parse', 'HEAD')
        self.set_exe(f'v0.1-6-g{self.head[:10]}')

    def test_refuses_unreleased_changes_heading(self):
        self.commit_changes(f'## {VERSION} (unreleased)\n\n### Fixed\n')
        with self.assertRaises(SystemExit) as cm:
            self.run_stage()
        self.assertIn('unreleased', str(cm.exception))
        self.assertFalse((self.tmp / 'rel9').exists())
        self.commit_changes(f'## 9.8.8 (unreleased)\n\n## {VERSION} (2026-11-09)\n')  # another version may be open
        self.assertEqual(self.run_stage(), 0)

    def test_refuses_unassembled_changelog_fragments(self):
        self.commit_changes(f'## {VERSION} (2026-11-09)\n')
        touch(self.repo, 'changes.d/README.md')
        touch(self.repo, 'changes.d/foo.md', b'### Fixed\n\n- x\n')
        git(self.repo, 'add', 'changes.d')
        git(self.repo, 'commit', '-q', '-m', 'frag')
        self.head = git(self.repo, 'rev-parse', 'HEAD')
        self.set_exe(f'v0.1-7-g{self.head[:10]}')
        with self.assertRaises(SystemExit) as cm:
            self.run_stage()
        self.assertIn('changelog.py assemble', str(cm.exception))
        self.assertIn('changes.d/foo.md', str(cm.exception))
        git(self.repo, 'rm', '-q', 'changes.d/foo.md')
        git(self.repo, 'commit', '-q', '-m', 'assembled')  # the README alone is fine
        self.head = git(self.repo, 'rev-parse', 'HEAD')
        self.set_exe(f'v0.1-8-g{self.head[:10]}')
        self.assertEqual(self.run_stage(), 0)

    def test_version_check_gets_mingw_path_and_reports_failure(self):
        seen = {}

        def fake_run(cmd, **kw):
            seen['env'] = kw.get('env')
            return subprocess.CompletedProcess(cmd, 3221225781, stdout='', stderr='')
        with mock.patch.object(stage_mod.subprocess, 'run', fake_run):
            with self.assertRaises(SystemExit) as cm:
                stage_mod.verify_exe(self.build / 'bbport.exe', VERSION, self.repo, False, True)
        self.assertIn(stage_mod.MINGW, seen['env']['PATH'])
        msg = str(cm.exception)
        self.assertIn('exit code 3221225781', msg)
        self.assertIn('0xC0000135', msg)
        self.assertIn('stderr', msg)

    def test_refuses_wrong_version_and_existing_stage(self):
        with self.assertRaises(SystemExit):
            stage_mod.main(['1.2.3', str(self.build), str(self.tmp / 'rel9'), '--repo', str(self.repo), '--no-run'])
        self.run_stage()
        with self.assertRaises(SystemExit):
            self.run_stage()
        self.assertEqual(self.run_stage('--replace'), 0)


class FakeGitHub:
    def __init__(self, release_exists=False, tag_exists=False):
        self.calls = []
        self.release_exists, self.tag_exists = release_exists, tag_exists

    def call(self, url, data=None, method='GET', ctype='application/json', length=None):
        self.calls.append((method if data is None and method == 'GET' else method, url))
        if '/releases/tags/' in url:
            return (200, {'html_url': 'https://example.invalid/r'}) if self.release_exists else (404, {})
        if '/git/ref/tags/' in url:
            return (200, {'object': {'sha': 'a' * 40}}) if self.tag_exists else (404, {})
        if url.endswith('/releases'):
            return 201, {'html_url': 'https://example.invalid/new', 'upload_url': 'https://up.invalid/assets{?name,label}'}
        return 201, {'browser_download_url': 'https://example.invalid/a.zip', 'size': 1}


class PublishTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.tmp = Path(self._t.name)
        self.stage = good_stage(self.tmp / 'stage')
        self.commit = 'b' * 40
        Path(str(self.stage) + '.stamp.json').write_text(json.dumps({
            'version': VERSION, 'commit': self.commit, 'build_id': 'x',
            'exe_sha256': rc.sha256(self.stage / 'out' / 'bbport.exe')}))
        self.zip = self.tmp / 'r.zip'
        make_zip.build_zip(self.stage, self.zip)
        self.notes = touch(self.tmp, 'notes.md', b'### Added\n- a thing\n')
        self.norepo = self.tmp / 'empty'
        self.norepo.mkdir()
        git(self.norepo, 'init', '-q')

    def tearDown(self):
        self._t.cleanup()

    def argv(self, **over):
        a = {'--repo': 'o/n', '--tag': 'v' + VERSION, '--commit': self.commit, '--title': 'T',
             '--notes': str(self.notes), '--zip': str(self.zip), '--git-repo': str(self.norepo)}
        a.update(over)
        return [x for kv in a.items() for x in kv]

    def run_main(self, argv, gh=None):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc_ = publish.main(argv, github=gh)
        return rc_, buf.getvalue()

    def test_dry_run_prints_request_and_never_calls_github(self):
        gh = FakeGitHub()
        code, out = self.run_main(self.argv(), gh)
        self.assertEqual(code, 0)
        self.assertEqual(gh.calls, [])
        self.assertIn('DRY RUN', out)
        self.assertIn(f'"tag_name": "v{VERSION}"', out)
        self.assertIn(f'"target_commitish": "{self.commit}"', out)
        self.assertIn('/repos/o/n/releases', out)

    def test_publish_flag_creates_release_and_uploads(self):
        gh = FakeGitHub()
        code, out = self.run_main(self.argv() + ['--yes-publish'], gh)
        self.assertEqual(code, 0)
        self.assertIn('release: https://example.invalid/new', out)
        methods = [m for m, _ in gh.calls]
        self.assertEqual(methods.count('POST'), 2)

    def test_refuses_existing_release_or_tag(self):
        for gh in (FakeGitHub(release_exists=True), FakeGitHub(tag_exists=True)):
            with self.assertRaises(SystemExit) as cm:
                self.run_main(self.argv() + ['--yes-publish'], gh)
            self.assertIn('already exists', str(cm.exception))
            self.assertNotIn('POST', [m for m, _ in gh.calls])

    def test_refuses_commit_other_than_exe_commit(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main(self.argv(**{'--commit': 'c' * 40}))
        self.assertIn('built from', str(cm.exception))

    def test_refuses_short_sha_and_wrong_tag(self):
        with self.assertRaises(SystemExit):
            self.run_main(self.argv(**{'--commit': 'bbbbbbb'}))
        with self.assertRaises(SystemExit):
            self.run_main(self.argv(**{'--tag': 'v1.0.0'}))

    def test_refuses_local_tag_on_another_commit(self):
        repo = self.norepo
        touch(repo, 'a.txt')
        git(repo, 'add', '.')
        git(repo, 'commit', '-q', '-m', 'c')
        git(repo, 'tag', 'v' + VERSION)
        with self.assertRaises(SystemExit) as cm:
            self.run_main(self.argv())
        self.assertIn('local tag', str(cm.exception))
        self.assertIn('tag -d', str(cm.exception))  # says what to do

    def test_refuses_changed_zip_and_missing_notes(self):
        with open(self.zip, 'ab') as f:
            f.write(b'junk')
        with self.assertRaises(SystemExit):
            self.run_main(self.argv())
        self.notes.write_text('  \n')
        with self.assertRaises(SystemExit):
            publish.local_checks('o/n', 'v' + VERSION, self.commit, self.notes, self.zip, self.norepo)

    def test_refuses_without_stamp(self):
        Path(str(self.zip) + '.stamp.json').unlink()
        with self.assertRaises(SystemExit) as cm:
            self.run_main(self.argv())
        self.assertIn('stamp', str(cm.exception))


class ConfigurePgoTests(unittest.TestCase):
    def test_mangle(self):
        self.assertEqual(configure_pgo.mangle('E:/games/x/out/gpu-pgo-gen'), 'E~/games/x/out/gpu-pgo-gen')
        self.assertEqual(configure_pgo.mangle(r'E:\games\x'), 'E~/games/x')

    def test_plan_remaps_profile_and_sets_release_switches(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            src = t / 'src'
            (src / 'out' / 'gpu-pgo-gen').mkdir(parents=True)
            (src / 'out' / 'gpu-pgo-gen' / 'CMakeCache.txt').write_text(
                'VMA_INC:PATH=/inc/vma\nMINIZ_INC:PATH=/inc/miniz\nBB_LTO:BOOL=ON\n'
                'CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\nBB_PGO:STRING=use\n'
                f'BB_PGO_DIR:PATH={(src / "pgo").as_posix()}\n// comment\n', encoding='utf-8')
            clone = t / 'agents' / 'x' / 'repo'
            (clone / 'gpu').mkdir(parents=True)
            (clone / 'gpu' / 'CMakeLists.txt').write_text('')
            repo, args, (pgo_src, pgo_dst), notes = configure_pgo.plan(t / 'agents' / 'x', src)
            self.assertEqual(repo, clone.resolve())
            self.assertEqual(pgo_src, src / 'pgo' / configure_pgo.mangle(src / 'out' / 'gpu-pgo-gen'))
            self.assertEqual(pgo_dst, clone.resolve() / 'pgo_map' / configure_pgo.mangle(clone.resolve() / 'out' / 'gpu-pgo-gen'))
            for want in ('Ninja', '-DBB_LTO:BOOL=ON', '-DBB_STRIP:BOOL=ON', '-DBB_PGO:STRING=use',
                         '-DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo', '-DVMA_INC:PATH=/inc/vma',
                         '-DMINIZ_INC:PATH=/inc/miniz'):
                self.assertIn(want, args)
            self.assertIn(f'-DBB_PGO_DIR:PATH={pgo_dst.as_posix()}', args)
            self.assertEqual(notes, [])


class BuildReleaseTests(unittest.TestCase):
    def test_refuses_fragments_in_commit(self):
        with self.assertRaises(SystemExit) as cm:
            stage_mod.check_fragment_names(['changes.d/README.md', 'changes.d/a.md'], '0.8.1')
        self.assertIn('assemble 0.8.1', str(cm.exception))
        stage_mod.check_fragment_names(['changes.d/README.md', 'CHANGES.md'], '0.8.1')

    def test_notes_from_changes(self):
        text = '# t\n## 0.8.1 (2026-11-10)\n\n### Added\n- a\n\n### Fixed\n- b\n## 0.8.0 (2026-11-09)\n- old\n'
        self.assertEqual(build_release.notes_from_changes(text, '0.8.1'), '### Added\n- a\n\n### Fixed\n- b\n')
        self.assertEqual(build_release.notes_from_changes(text, '0.8.0'), '- old\n')
        self.assertEqual(build_release.notes_from_changes(text, '0.9.0'), '')
        self.assertEqual(build_release.notes_from_changes('## 0.8.1 (x)\n\n## 0.8.0 (y)\n', '0.8.1'), '')

    def test_exe_and_stamp_match(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            commit = 'ab12cd34ef' + '0' * 30
            exe = touch(t, 'bbport.exe', b'MZ' + f'{VERSION} (v0.1-5-g{commit[:10]})'.encode() + bytes([0]))
            self.assertTrue(build_release.exe_matches(exe, VERSION, commit))
            self.assertFalse(build_release.exe_matches(exe, VERSION, 'ffffffffff' + '0' * 30))
            dirty = touch(t, 'd.exe', b'MZ' + f'{VERSION} (v0.1-5-g{commit[:10]}-dirty)'.encode() + bytes([0]))
            self.assertFalse(build_release.exe_matches(dirty, VERSION, commit))
            self.assertFalse(build_release.exe_matches(t / 'none.exe', VERSION, commit))
            stamp = t / 's.json'
            self.assertFalse(build_release.stamp_ok(stamp, commit))
            stamp.write_text(json.dumps({'commit': commit, 'exe_sha256': rc.sha256(exe)}))
            self.assertTrue(build_release.stamp_ok(stamp, commit, exe))
            self.assertFalse(build_release.stamp_ok(stamp, 'f' * 40, exe))
            self.assertFalse(build_release.stamp_ok(stamp, commit, dirty))

    def test_tracked_dirty_ignores_untracked(self):
        with tempfile.TemporaryDirectory() as t:
            repo = Path(t) / 'r'
            repo.mkdir()
            git(repo, 'init', '-q')
            touch(repo, 'a.txt')
            git(repo, 'add', '.')
            git(repo, 'commit', '-q', '-m', 'c')
            touch(repo, 'untracked.txt')
            self.assertFalse(build_release.tracked_dirty(repo))
            touch(repo, 'a.txt', b'changed')
            self.assertTrue(build_release.tracked_dirty(repo))

    def test_clone_has_no_source_tags(self):
        """The release clone must not carry the source repository's (upstream) tags."""
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            src = t / 'src'
            src.mkdir()
            git(src, 'init', '-q')
            touch(src, 'a.txt')
            git(src, 'add', '.')
            git(src, 'commit', '-q', '-m', 'c')
            git(src, 'tag', 'v9.9.9')
            ns = type('A', (), {'src': str(src)})()
            head = git(src, 'rev-parse', 'HEAD')
            build_release.step_clone(ns, t / 'clone' / 'repo', head, False)
            self.assertEqual(git(t / 'clone' / 'repo', 'tag'), '')
            build_release.step_clone(ns, t / 'clone' / 'repo', head, False)  # second call: no-op (resumable)


@unittest.skipUnless(shutil.which('cmake') and shutil.which('git'), 'needs cmake and git')
class BuildIdTests(unittest.TestCase):
    """gpu/cmake/bbport_build_id.cmake: tracked changes mark -dirty, a changed submodule does not."""

    def build_id(self, repo, out):
        subprocess.run(['cmake', f'-DSRC={repo}', f'-DOUT={out}', '-DVERSION=1.0', '-P',
                        str(ROOT / 'gpu' / 'cmake' / 'bbport_build_id.cmake')], check=True, capture_output=True)
        return Path(out).read_text()

    def test_dirty_only_for_the_main_tree(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            sub = t / 'sub'
            sub.mkdir()
            git(sub, 'init', '-q')
            touch(sub, 'f.txt')
            git(sub, 'add', '.')
            git(sub, 'commit', '-q', '-m', 's')
            repo = t / 'repo'
            repo.mkdir()
            git(repo, 'init', '-q')
            touch(repo, 'a.txt')
            git(repo, '-c', 'protocol.file.allow=always', 'submodule', 'add', '-q', str(sub), 'dep')
            git(repo, 'add', '.')
            git(repo, 'commit', '-q', '-m', 'c')
            out = t / 'id.c'
            self.assertNotIn('dirty', self.build_id(repo, out))
            touch(repo, 'dep/f.txt', b'patched inside the submodule')  # the FSR patches do this
            self.assertNotIn('dirty', self.build_id(repo, out))
            touch(repo, 'a.txt', b'changed')
            self.assertIn('-dirty', self.build_id(repo, out))


if __name__ == '__main__':
    unittest.main()
