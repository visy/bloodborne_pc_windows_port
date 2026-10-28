"""launcher.py 'Fix firewall issues': the generated elevated script (never run here)."""
from paths import ROOT
import importlib.util
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_fw", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FirewallScript(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_quote(self):
        self.assertEqual(self.L.ps_quote("a b"), "'a b'")
        self.assertEqual(self.L.ps_quote("C:\\O'Neil\\x y"), "'C:\\O''Neil\\x y'")

    def test_script(self):
        exe = "C:\\Games\\Blood's Port\\out\\bbport.exe"
        s = self.L.firewall_fix_script(exe, 9400, "C:\\Temp\\bb fw\\result.txt")
        self.assertIn("$exe = 'C:\\Games\\Blood''s Port\\out\\bbport.exe'", s)
        self.assertIn("-LocalPort 9400", s)
        self.assertIn("Remove-NetFirewallRule", s)
        self.assertIn("$_.Direction -eq 'Inbound' -and $_.Action -eq 'Block'", s)
        self.assertIn("-Profile Private,Public", s)
        self.assertIn("'UDP', 'TCP'", s)
        self.assertIn("-LiteralPath 'C:\\Temp\\bb fw\\result.txt'", s)
        self.assertLess(s.index("Action -eq 'Block'"), s.index("New-NetFirewallRule"))

    def test_port_is_int(self):
        s = self.L.firewall_fix_script("C:\\x\\bbport.exe", "9307", "r.txt")
        self.assertIn("-LocalPort 9307", s)


if __name__ == "__main__":
    unittest.main()
