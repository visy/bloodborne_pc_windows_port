"""The port's version is the same everywhere it is shown (VERSION is the source)."""
import re
import unittest

from paths import ROOT


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.version = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()

    def test_version_is_dotted_numbers(self):
        self.assertRegex(self.version, r'^\d+(\.\d+){1,3}$')

    def test_launcher_title_matches(self):
        text = (ROOT / 'launcher.py').read_text(encoding='utf-8')
        found = re.search(r'^PORT_VERSION = "([^"]+)"', text, re.M)
        self.assertIsNotNone(found)
        self.assertEqual(found.group(1), self.version)

    def test_readme_and_changes_name_it(self):
        self.assertIn(f'**Version {self.version}**', (ROOT / 'README.md').read_text(encoding='utf-8'))
        self.assertIn(f'## {self.version} ', (ROOT / 'CHANGES.md').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
