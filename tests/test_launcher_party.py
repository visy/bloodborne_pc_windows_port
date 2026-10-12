"""launcher.py party bits: both host codes from party_code.txt, and Stop closing the game normally
before the process-tree kill."""
from paths import ROOT
import importlib.util
import os
import subprocess
import sys
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_party", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class PartyCodes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_both_codes(self):
        text = "internet=BBP1-AAAA-BBBB\nlan=BBP1-CCCC-DDDD\n"
        self.assertEqual(self.L.parse_party_codes(text), ("BBP1-AAAA-BBBB", "BBP1-CCCC-DDDD"))

    def test_crlf_and_missing_internet(self):
        self.assertEqual(self.L.parse_party_codes("internet=\r\nlan=BBP1-CCCC\r\n"), ("", "BBP1-CCCC"))

    @staticmethod
    def encode(flags: int) -> str:
        """A BBP1 code (party_code.h layout) for 203.0.113.7:9307 with these flags."""
        raw = bytearray([1, flags, 203, 0, 113, 7, 0x24, 0x5B, 1, 2, 3, 4, 5, 6, 7, 8])
        crc = 0xFFFF
        for b in raw:
            crc ^= b << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        raw += bytes([crc >> 8, crc & 0xFF])
        alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
        acc = bits = 0
        sym = ""
        for b in raw:
            acc = (acc << 8) | b
            bits += 8
            while bits >= 5:
                sym += alphabet[(acc >> (bits - 5)) & 31]
                bits -= 5
        sym += alphabet[(acc << (5 - bits)) & 31]
        return "BBP1" + "".join("-" + sym[i:i + 4] for i in range(0, len(sym), 4))

    def test_password_flag(self):
        needs, free = self.encode(2), self.encode(0)
        self.assertEqual(self.L.party_code_problem(needs), "")
        self.assertEqual(self.L.party_code_flags(needs), 2)
        self.assertEqual(self.L.party_code_flags(free.lower()), 0)
        self.assertEqual(self.L.party_password_problem(needs, ""), "This party needs a password")
        self.assertEqual(self.L.party_password_problem(needs, "  "), "This party needs a password")
        self.assertEqual(self.L.party_password_problem(needs, "pw"), "")
        self.assertEqual(self.L.party_password_problem(free, ""), "")
        self.assertEqual(self.L.party_password_problem("192.168.1.5:9307", ""), "")
        typo = needs[:-3] + ("0" if needs[-3] != "0" else "1") + needs[-2:]
        self.assertIsNone(self.L.party_code_flags(typo))  # checksum

    @staticmethod
    def encode2(flags: int, ip=(203, 0, 113, 7), port=9307) -> str:
        """A BBP2 code (party_code.h layout) with the 6-byte secret 1..6."""
        wire = flags | (8 if port == 9307 else 0)
        raw = bytearray([wire]) + bytes(ip[3:] if flags & 1 else ip)
        if port != 9307:
            raw += bytes([port >> 8, port & 0xFF])
        raw += bytes([1, 2, 3, 4, 5, 6])
        crc = 0
        for b in raw:
            crc ^= b
            for _ in range(8):
                crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
        raw.append(crc)
        alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
        acc = bits = 0
        sym = ""
        for b in raw:
            acc = (acc << 8) | b
            bits += 8
            while bits >= 5:
                sym += alphabet[(acc >> (bits - 5)) & 31]
                bits -= 5
        if bits:
            sym += alphabet[(acc << (5 - bits)) & 31]
        return "BBP2" + "".join("-" + sym[i:i + 4] for i in range(0, len(sym), 4))

    def test_bbp2(self):
        needs, free = self.encode2(2), self.encode2(0)
        self.assertEqual(len(needs), len("BBP2-XXXX-XXXX-XXXX-XXXX-XXXX"))
        self.assertEqual(self.L.party_code_problem(needs), "")
        self.assertEqual(self.L.party_code_flags(needs), 2)
        self.assertEqual(self.L.party_code_flags(free.lower().replace("1", "l")), 0)
        self.assertEqual(self.L.party_password_problem(needs, ""), "This party needs a password")
        lan = self.encode2(1 | 2, ip=(192, 168, 1, 42))
        self.assertEqual(len(lan), len("BBP2-XXXX-XXXX-XXXX-XXX"))
        self.assertEqual(self.L.party_code_flags(lan), 3)
        self.assertEqual(self.L.party_code_flags(self.encode2(4, port=48000)), 4)
        for i, c in enumerate(needs):
            if i < 5 or c == "-":
                continue
            for r in "0123456789ABCDEFGHJKMNPQRSTVWXYZ":
                if r != c:
                    typo = needs[:i] + r + needs[i + 1:]
                    self.assertIsNone(self.L.party_code_flags(typo), typo)
        typo = needs[:-1] + ("0" if needs[-1] != "0" else "1")
        self.assertIn("typo", self.L.party_code_problem(typo))

    def test_bare_line(self):
        self.assertEqual(self.L.parse_party_codes("BBP1-ZZZZ\n"), ("BBP1-ZZZZ", ""))
        self.assertEqual(self.L.parse_party_codes(""), ("", ""))


@unittest.skipUnless(os.name == "nt", "Windows process tree")
class StopTree(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_no_window_falls_back_to_kill(self):
        # A windowless child: no WM_CLOSE target, so the tree is killed at once.
        proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                creationflags=subprocess.CREATE_NO_WINDOW)
        lines = []
        try:
            self.assertIn(proc.pid, self.L.descendant_pids(proc.pid))
            self.assertEqual(self.L.stop_tree(proc, timeout=2, log=lines.append), "killed")
            proc.wait(timeout=10)
            self.assertTrue(any("no game window" in ln for ln in lines), lines)
        finally:
            if proc.poll() is None:
                proc.kill()

    def test_exited(self):
        proc = subprocess.Popen([sys.executable, "-c", "pass"], creationflags=subprocess.CREATE_NO_WINDOW)
        proc.wait(timeout=10)
        self.assertEqual(self.L.stop_tree(proc), "exited")


if __name__ == "__main__":
    unittest.main()
