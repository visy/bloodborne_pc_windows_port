"""tools/release/changelog.py: fragment checks and assemble into CHANGES.md (temp dirs, no network)."""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'tools' / 'release'))
import changelog  # noqa: E402

BASE = ('# Title\n\nintro\n\n## 0.8.1 (unreleased)\n\n### Added\n\n### Changed\n\n- **c1** old change.\n\n'
        '### Fixed\n\n- **f1** old fix\n  continued.\n- **f2** second.\n\n## 0.8.0 (2026-11-09)\n\n### Added\n\n- **a0** x\n')


class ChangelogTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.repo = Path(self._t.name)
        (self.repo / 'changes.d').mkdir()
        (self.repo / 'changes.d' / 'README.md').write_text('# readme\n')
        (self.repo / 'CHANGES.md').write_text(BASE, encoding='utf-8', newline='\n')

    def tearDown(self):
        self._t.cleanup()

    def frag(self, name, text):
        (self.repo / 'changes.d' / name).write_bytes(text.encode('utf-8'))

    def errs(self, text):
        self.frag('x.md', text)
        return changelog.check_fragment(self.repo / 'changes.d' / 'x.md')

    def changes(self):
        return (self.repo / 'CHANGES.md').read_bytes().decode('utf-8')

    def test_parse_errors(self):
        self.assertEqual(self.errs('### Fixed\n\n- ok\n'), [])
        self.assertIn('not one of', self.errs('### Removed\n\n- a\n')[0][1])
        self.assertTrue(self.errs('### Fixed\n'))  # no bullet
        self.assertTrue(self.errs('- a\n'))  # bullet before heading
        self.assertTrue(self.errs('hello\n### Fixed\n- a\n'))  # text before heading
        e = self.errs('### Fixed\n\n- a\n<<<<<<< HEAD\n- b\n')
        self.assertEqual(e[0][0], 4)
        self.assertIn('conflict', e[0][1])
        (self.repo / 'changes.d' / 'x.md').write_bytes(b'### Fixed\n- \xff\n')
        self.assertIn('UTF-8', changelog.check_fragment(self.repo / 'changes.d' / 'x.md')[0][1])

    def test_check_cli_reports_file_line(self):
        self.frag('bad.md', '### Nope\n- a\n')
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = changelog.main(['--repo', str(self.repo), 'check'])
        self.assertEqual(rc, 1)
        self.assertIn('bad.md:1:', err.getvalue())

    def test_assemble_into_existing_unreleased(self):
        self.frag('b.md', '### Fixed\n\n- **fb** from b\n\n### Added\n\n- **ab** added b\n')
        self.frag('a.md', '### Fixed\n\n- **fa** from a\n  second line\n')
        self.assertEqual(changelog.assemble(self.repo, '0.8.1', '2026-11-10'), 2)
        text = self.changes()
        self.assertIn('## 0.8.1 (2026-11-10)', text)
        self.assertNotIn('unreleased', text)
        self.assertLess(text.index('- **f1**'), text.index('- **f2**'))
        self.assertLess(text.index('- **f2**'), text.index('- **fa**'))  # existing first, then fragments by name
        self.assertLess(text.index('- **fa**'), text.index('- **fb**'))
        self.assertIn('- **fa** from a\n  second line\n', text)
        self.assertLess(text.index('- **ab**'), text.index('- **c1**'))  # Added before Changed
        self.assertLess(text.index('- **fb**'), text.index('## 0.8.0 (2026-11-09)'))
        self.assertIn('- **a0** x', text)
        self.assertEqual(list((self.repo / 'changes.d').glob('*.md')), [self.repo / 'changes.d' / 'README.md'])

    def test_assemble_new_section_and_idempotent(self):
        self.frag('n.md', '### Changed\n\n- **cn** new\n')
        changelog.assemble(self.repo, '0.9.0', '2026-12-01')
        text = self.changes()
        self.assertLess(text.index('intro'), text.index('## 0.9.0 (2026-12-01)'))
        self.assertLess(text.index('## 0.9.0 (2026-12-01)'), text.index('## 0.8.1 (unreleased)'))
        self.assertIn('### Changed\n\n- **cn** new\n', text)
        self.assertEqual(changelog.assemble(self.repo, '0.9.0', '2026-12-01'), 0)
        self.assertEqual(self.changes(), text)

    def test_assemble_refuses_bad_fragment(self):
        self.frag('bad.md', '### Fixed\n')
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(ValueError):
                changelog.assemble(self.repo, '0.8.1', '2026-11-10')
        self.assertEqual(self.changes(), BASE)
        self.assertTrue((self.repo / 'changes.d' / 'bad.md').exists())

    def test_preview(self):
        self.frag('a.md', '### Fixed\n\n- **fa** x\n')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(changelog.main(['--repo', str(self.repo), 'preview']), 0)
        self.assertIn('### Fixed\n\n- **fa** x', out.getvalue())


if __name__ == '__main__':
    unittest.main()
