"""launcher.py party bits: both host codes from party_code.txt, and Stop closing the game normally
before the process-tree kill."""
from pathlib import Path
from paths import ROOT
import importlib.util
import os
import subprocess
import sys
import tempfile
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

    SECRET = bytes([0x5E, 0x72, 0xA9, 0x13, 0xC4, 0x8D])

    def test_bbp3_examples(self):
        """The examples every tooltip shows are real codes, the same as party_code.h's."""
        L = self.L
        self.assertEqual(L.encode_party_code_v3(2, (203, 0, 113, 7), 9307, self.SECRET), L.PARTY_CODE_EXAMPLE)
        self.assertEqual(L.encode_party_code_v3(3, (192, 168, 1, 42), 9307, self.SECRET), L.PARTY_LAN_CODE_EXAMPLE)
        header = (ROOT / "gpu" / "shim" / "party" / "party_code.h").read_text(encoding="utf-8")
        self.assertIn(f'#define PARTY_CODE_EXAMPLE "{L.PARTY_CODE_EXAMPLE}"', header)
        self.assertIn(f'#define PARTY_LAN_CODE_EXAMPLE "{L.PARTY_LAN_CODE_EXAMPLE}"', header)
        for text in (L.party_code_problem(""), L.party_code_problem("BBP3-!!"), L.party_code_problem("what?"),
                     L.PARTY_HELP, L.PARTY_HELP_SHORT):
            self.assertIn(L.PARTY_CODE_EXAMPLE, text)
        self.assertNotIn("not been tested between two PCs", L.PARTY_HELP)

    def test_bbp3_shapes(self):
        L = self.L
        # (flags, ip, port, symbols after BBP3-)
        shapes = [(2, (203, 0, 113, 7), 9307, 16), (0, (203, 0, 113, 7), 9307, 19),
                  (3, (192, 168, 1, 42), 9307, 11), (1, (192, 168, 1, 42), 9307, 14),
                  (2, (203, 0, 113, 7), 48000, 19), (4, (203, 0, 113, 7), 48000, 22),
                  (7, (192, 168, 1, 42), 47001, 14), (1, (192, 168, 1, 42), 47001, 17)]
        for flags, ip, port, symbols in shapes:
            code = L.encode_party_code_v3(flags, ip, port, self.SECRET)
            self.assertEqual(len(code.replace("-", "")) - 4, symbols, code)
            self.assertEqual(L.party_code_problem(code), "", code)
            self.assertEqual(L.party_code_flags(code), flags, code)
            # case, blanks, I/L for 1 and O for 0
            sloppy = " " + code.lower().replace("1", "l").replace("0", "O").replace("-", " ") + " "
            self.assertEqual(L.party_code_flags(sloppy), flags, sloppy)
            body = [i for i, c in enumerate(code) if i >= 5 and c != "-"]
            for i in body:
                for r in L.PARTY_CODE_ALPHABET:
                    if r != code[i]:
                        typo = code[:i] + r + code[i + 1:]
                        self.assertIsNone(L.party_code_flags(typo), typo)
            for a, b in zip(body, body[1:]):
                if code[a] != code[b]:
                    swapped = list(code)
                    swapped[a], swapped[b] = swapped[b], swapped[a]
                    self.assertIsNone(L.party_code_flags("".join(swapped)), "".join(swapped))
            self.assertIn("typo", L.party_code_problem(code[:-1] + ("0" if code[-1] != "0" else "1")))
            self.assertIsNone(L.party_code_flags(code[:-1]))
        self.assertEqual(L.party_password_problem(L.PARTY_CODE_EXAMPLE, ""), "This party needs a password")
        self.assertEqual(L.party_password_problem(L.encode_party_code_v3(0, (1, 2, 3, 4), 9307, self.SECRET), ""), "")

    def test_old_codes_still_valid(self):
        # Texts written by the BBP2 / BBP1 encoders (also in test_party_link.cpp).
        self.assertEqual(self.L.party_code_flags("BBP2-1B5G-0W87-0410-6105-0SBG"), 2)
        self.assertEqual(self.L.party_code_flags("bbp2-1cn0-20g3-og2g-cwo"), 3)
        self.assertEqual(self.L.party_code_flags("BBP1-041C-P03H-0WJ5-P082-0C20-A1G7-138R-W"), 2)
        self.assertEqual(self.L.party_code_flags("BBPL-041C-P03H-0WJ5-P082-0C20-A1G7-138R-W"), 2)

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


class PartyNameTagsSetting(unittest.TestCase):
    """The launcher's Party name tags checkbox: bbport.ini party_name_tags, on unless set to 0."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_on_and_round_trip(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertTrue(self.L.ini_flag(ini, "party_name_tags"))  # no file: on
            ini.write_text("party_hud=1\nkey.cross=Enter\n", encoding="utf-8")
            self.assertTrue(self.L.ini_flag(ini, "party_name_tags"))  # not set: on
            self.L.update_ini(ini, {"party_name_tags": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_name_tags"))
            self.assertTrue(self.L.ini_flag(ini, "party_hud"))
            self.assertIn("key.cross=Enter", ini.read_text(encoding="utf-8"))  # other lines kept
            self.L.update_ini(ini, {"party_name_tags": "1"})
            self.assertTrue(self.L.ini_flag(ini, "party_name_tags"))


class CompanionsWithoutGlowSetting(unittest.TestCase):
    """The launcher's Companions without glow checkbox: bbport.ini party_glow_off, on unless set to 0."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_on_and_round_trip(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertTrue(self.L.ini_flag(ini, "party_glow_off"))  # no file: on (glow removed)
            ini.write_text("party_name_tags=0" + chr(10), encoding="utf-8")
            self.assertTrue(self.L.ini_flag(ini, "party_glow_off"))  # not set: on
            self.L.update_ini(ini, {"party_glow_off": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_glow_off"))
            self.assertFalse(self.L.ini_flag(ini, "party_name_tags"))  # other keys kept
            self.L.update_ini(ini, {"party_glow_off": "1"})
            self.assertTrue(self.L.ini_flag(ini, "party_glow_off"))


class ShareNpcDialogueSetting(unittest.TestCase):
    """The launcher's Share NPC dialogue checkbox: bbport.ini party_npc_talk, on unless set to 0."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_on_and_round_trip(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertTrue(self.L.ini_flag(ini, "party_npc_talk"))  # no file: on
            ini.write_text("party_name_tags=0" + chr(10), encoding="utf-8")
            self.assertTrue(self.L.ini_flag(ini, "party_npc_talk"))  # not set: on
            self.L.update_ini(ini, {"party_npc_talk": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_npc_talk"))
            self.assertFalse(self.L.ini_flag(ini, "party_name_tags"))  # other keys kept
            self.L.update_ini(ini, {"party_npc_talk": "1"})
            self.assertTrue(self.L.ini_flag(ini, "party_npc_talk"))


class PingsSettings(unittest.TestCase):
    """The launcher's Pings (on by default) and Show party through walls (off) checkboxes."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_defaults_and_round_trip(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertTrue(self.L.ini_flag(ini, "party_pings"))
            self.assertFalse(self.L.ini_flag(ini, "party_through_walls", False))
            self.L.update_ini(ini, {"party_pings": "0", "party_through_walls": "1"})
            self.assertFalse(self.L.ini_flag(ini, "party_pings"))
            self.assertTrue(self.L.ini_flag(ini, "party_through_walls", False))
        src = (Path(self.L.__file__).read_text(encoding="utf-8"))
        self.assertIn("System > My party: Display", src)


class FriendlyFireSetting(unittest.TestCase):
    """I1: the host's Friendly fire / PvP checkbox: bbport.ini party_friendly_fire, off by default."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_off_and_round_trip(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertFalse(self.L.ini_flag(ini, "party_friendly_fire", False))
            self.L.update_ini(ini, {"party_friendly_fire": "1"})
            self.assertTrue(self.L.ini_flag(ini, "party_friendly_fire", False))
            self.L.update_ini(ini, {"party_friendly_fire": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_friendly_fire", False))
        src = Path(self.L.__file__).read_text(encoding="utf-8")
        self.assertIn("System > Party rules: PvP & lamps", src)
        # the host's rule: a row of the Party Rules tab
        self.assertIn('("check", "party_friendly_fire", "Friendly fire / PvP",', src)


class ShareLampsSetting(unittest.TestCase):
    """Statsaudit finding 5: the host's Shared lamps checkbox: bbport.ini party_share_lamps, on by default."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_on_and_round_trip(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertTrue(self.L.ini_flag(ini, "party_share_lamps", True))
            self.L.update_ini(ini, {"party_share_lamps": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_share_lamps", True))
            self.L.update_ini(ini, {"party_share_lamps": "1"})
            self.assertTrue(self.L.ini_flag(ini, "party_share_lamps", True))
        src = Path(self.L.__file__).read_text(encoding="utf-8")
        self.assertIn('("check", "party_share_lamps", "Shared lamps",', src)


class GuestsBringLampsSetting(unittest.TestCase):
    """0.7.2: the host's Guests bring their lamps checkbox: bbport.ini party_guests_bring_lamps, off by default."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_off_and_round_trip(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertFalse(self.L.ini_flag(ini, "party_guests_bring_lamps", False))
            self.L.update_ini(ini, {"party_guests_bring_lamps": "1"})
            self.assertTrue(self.L.ini_flag(ini, "party_guests_bring_lamps", False))
            self.L.update_ini(ini, {"party_guests_bring_lamps": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_guests_bring_lamps", False))
        src = Path(self.L.__file__).read_text(encoding="utf-8")
        self.assertIn('("check", "party_guests_bring_lamps", "Guests bring their lamps",', src)
        self.assertIn('("party_guests_bring_lamps", False)', src)  # PARTY_INI_FLAGS


class TravelVoteSetting(unittest.TestCase):
    """0.7.3: the host's Party votes on travel checkbox: bbport.ini party_travel_vote, off by default,
    no variable set (System > Party rules: World)."""

    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_switch_default_off_and_ini(self):
        switches = {key: (env, default) for key, env, default in self.L.PARTY_INI_SWITCHES}
        self.assertEqual(switches["party_travel_vote"], ("BB_PARTY_TRAVEL_VOTE", False))
        env = self.L.party_env({"party_mode": "join", "party_name": "A", "party_code": "x"})
        self.assertIsNone(env["BB_PARTY_TRAVEL_VOTE"])  # bbport.ini decides
        self.assertEqual(self.L.party_rules_ini({}, "")["party_travel_vote"], "0")
        self.assertEqual(self.L.party_rules_ini({"party_travel_vote": True}, "")["party_travel_vote"], "1")
        from pathlib import Path
        src = Path(self.L.__file__).read_text(encoding="utf-8")
        self.assertIn('("check", "party_travel_vote", "Party votes on travel",', src)


class PartyRules071Settings(unittest.TestCase):
    """0.7.1: the switches that had only an env variable got rows (System > Party rules: World / Party
    rewards) and launcher checkboxes (Party Rules / My Party tabs): bbport.ini keys, on by default, no variable set."""
    KEYS = {"party_kill_record": "BB_PARTY_KILL_RECORD", "party_gather": "BB_PARTY_GATHER",
            "party_npc_mirror": "BB_PARTY_NPC_MIRROR", "party_npc_fights": "BB_PARTY_NPC_FIGHTS",
            "party_key_view": "BB_PARTY_KEY_VIEW", "party_any_kidnap": "BB_PARTY_ANY_KIDNAP",
            "party_level_sync": "BB_PARTY_LEVEL_SYNC", "party_phantom_hp": "BB_PARTY_PHANTOM_HP",
            "party_pvp_full_hp": "BB_PARTY_PVP_FULL_HP",
            "party_key_items_summoned": "BB_PARTY_KEY_ITEMS_SUMMONED"}

    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_switches_defaults_and_ini(self):
        switches = {key: (env, default) for key, env, default in self.L.PARTY_INI_SWITCHES}
        for key, env in self.KEYS.items():
            self.assertEqual(switches[key], (env, True), key)
        env = self.L.party_env({"party_mode": "join", "party_name": "A", "party_code": "x"})
        for name in self.KEYS.values():
            self.assertIsNone(env[name], name)  # bbport.ini decides
        values = self.L.party_rules_ini({"party_kill_record": False, "party_key_items_summoned": False}, "")
        self.assertEqual((values["party_kill_record"], values["party_gather"], values["party_npc_mirror"],
                          values["party_key_view"], values["party_key_items_summoned"]), ("0", "1", "1", "1", "0"))

    def test_dialog_has_the_checkboxes(self):
        from pathlib import Path
        src = Path(self.L.__file__).read_text(encoding="utf-8")
        for key in self.KEYS:
            self.assertIn('("check", "%s", ' % key, src)
        self.assertIn('(0, 1, "World", [', src)


class CovenantRuleSetting(unittest.TestCase):
    """0.7.3: Covenant outcomes per player, the host's rule (System > Party rules: World): bbport.ini
    party_covenant_per_player, on by default, a Party Rules tab checkbox, no variable set."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_ini_and_checkbox(self):
        switches = {key: (env, default) for key, env, default in self.L.PARTY_INI_SWITCHES}
        self.assertEqual(switches["party_covenant_per_player"], ("BB_PARTY_COVENANT_PER_PLAYER", True))
        env = self.L.party_env({"party_mode": "host", "party_name": "A"})
        self.assertIsNone(env["BB_PARTY_COVENANT_PER_PLAYER"])  # bbport.ini decides
        self.assertEqual(self.L.party_rules_ini({}, "")["party_covenant_per_player"], "1")
        self.assertEqual(self.L.party_rules_ini({"party_covenant_per_player": False}, "")["party_covenant_per_player"], "0")
        from pathlib import Path
        src = Path(self.L.__file__).read_text(encoding="utf-8")
        self.assertIn('("check", "party_covenant_per_player", "Covenant outcomes per player",', src)


class DirectSummonSetting(unittest.TestCase):
    """The launcher's Direct summon checkbox: BB_PARTY_DIRECT_SUMMON, on unless turned off."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_on_and_off(self):
        base = {"party_mode": "host", "party_name": "A"}
        self.assertEqual(self.L.party_env(base)["BB_PARTY_DIRECT_SUMMON"], "1")
        self.assertEqual(self.L.party_env(dict(base, party_direct_summon=False))["BB_PARTY_DIRECT_SUMMON"], "0")


class RendezvousSetting(unittest.TestCase):
    """The launcher's DHT checkbox: BB_PARTY_RENDEZVOUS, opt-in (off unless turned on), unset solo."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_off_and_on(self):
        for mode in ("host", "join"):
            base = {"party_mode": mode, "party_name": "A"}
            self.assertEqual(self.L.party_env(base)["BB_PARTY_RENDEZVOUS"], "0")
            self.assertEqual(self.L.party_env(dict(base, party_rendezvous=True))["BB_PARTY_RENDEZVOUS"], "1")
            self.assertEqual(self.L.party_env(dict(base, party_rendezvous=False))["BB_PARTY_RENDEZVOUS"], "0")
        self.assertIn("BB_PARTY_RENDEZVOUS", self.L.party_env({}))
        self.assertIsNone(self.L.party_env({"party_rendezvous": True})["BB_PARTY_RENDEZVOUS"])  # no party

    def test_tooltip_says_what_it_shares(self):
        tip = self.L.PARTY_RENDEZVOUS_TIP
        for words in ("public BitTorrent DHT", "IP", "guest code", "Both", "Default: off"):
            self.assertIn(words, tip)


class NetRouteSettings(unittest.TestCase):
    """F2 (0.7.1): the member relay and IPv6 checkboxes: BB_PARTY_MEMBER_RELAY on by default,
    BB_PARTY_IPV6 off by default (the game's default too), each as ticked, unset solo."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_on_and_off(self):
        for mode in ("host", "join"):
            base = {"party_mode": mode, "party_name": "A"}
            self.assertEqual(self.L.party_env(base)["BB_PARTY_MEMBER_RELAY"], "1")
            self.assertEqual(self.L.party_env(base)["BB_PARTY_IPV6"], "0")  # IPv6 off unless ticked
            for key, env in (("party_member_relay", "BB_PARTY_MEMBER_RELAY"), ("party_ipv6", "BB_PARTY_IPV6")):
                self.assertEqual(self.L.party_env(dict(base, **{key: False}))[env], "0")
                self.assertEqual(self.L.party_env(dict(base, **{key: True}))[env], "1")
        for env in ("BB_PARTY_MEMBER_RELAY", "BB_PARTY_IPV6"):
            self.assertIsNone(self.L.party_env({})[env])  # no party: unset

    def test_tooltips(self):
        for words in ("party members", "guest code", "upload", "128 KB/s", "Default: on"):
            self.assertIn(words, self.L.PARTY_MEMBER_RELAY_TIP)
        for words in ("IPv6", "before any relay", "party code", "privacy address", "Default: off"):
            self.assertIn(words, self.L.PARTY_IPV6_TIP)


class LampTravelSetting(unittest.TestCase):
    """The launcher's Direct lamp travel checkbox: BB_LAMP_TRAVEL, on (unset) unless turned off; solo too."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_on_and_off(self):
        self.assertIsNone(self.L.settings_env({})["BB_LAMP_TRAVEL"])
        self.assertEqual(self.L.settings_env({"lamp_travel": False})["BB_LAMP_TRAVEL"], "0")
        self.assertIsNone(self.L.settings_env({"party_mode": "host", "lamp_travel": True})["BB_LAMP_TRAVEL"])


class TracingWarning(unittest.TestCase):
    """Debug tracing in a party game gets a warning (playtest 1: huge logs, false hang dumps)."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_only_party_with_tracing(self):
        self.assertIn("Debug tracing", self.L.party_tracing_warning("host", True))
        self.assertIn("Debug tracing", self.L.party_tracing_warning("join", True))
        self.assertEqual(self.L.party_tracing_warning("join", False), "")
        self.assertEqual(self.L.party_tracing_warning("", True), "")
        self.assertEqual(self.L.party_tracing_warning(None, True), "")


class RevokeCode(unittest.TestCase):
    """New party code in the launcher: the game's saved secret goes, nothing else in party.json."""
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_revoke(self):
        import json
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            u = Path(d)
            (u / "party_secret.txt").write_text("a1b2c3d4e5f60000\n")
            (u / "party_code.txt").write_text("internet=BBP3-AAAA\nlan=BBP3-BBBB\n")
            (u / "party_state.json").write_text('{"pid":1,"secret":"a1b2c3d4e5f60000"}')
            (u / "party.json").write_text(json.dumps({"name": "Hunter", "role": "host",
                                                      "host_secret": "a1b2c3d4e5f60000",
                                                      "password_protected": "AAAA"}))
            changed = self.L.revoke_party_code(u)
            self.assertEqual(sorted(changed), ["party.json", "party_code.txt", "party_secret.txt",
                                               "party_state.json"])
            for name in ("party_secret.txt", "party_code.txt", "party_state.json"):
                self.assertFalse((u / name).exists())
            left = json.loads((u / "party.json").read_text())
            self.assertEqual(left, {"name": "Hunter", "role": "host", "password_protected": "AAAA"})
            self.assertEqual(self.L.revoke_party_code(u), [])  # nothing left to forget


class DiagnosticsAndPingToggles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("bb_launcher_ping", ROOT / "launcher.py")
        cls.L = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.L)

    def test_heap_watch_env_off_by_default(self):
        self.assertEqual(self.L.settings_env({}).get("BB_HEAP_WATCH_LOG"), "0")
        self.assertEqual(self.L.settings_env({"feat_heap_watch_log": True}).get("BB_HEAP_WATCH_LOG"), "1")

    def test_conn_warnings_flag_default_on(self):
        self.assertIn(("party_conn_warnings", True), self.L.PARTY_INI_FLAGS)
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertTrue(self.L.ini_flag(ini, "party_conn_warnings"))
            self.L.update_ini(ini, {"party_conn_warnings": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_conn_warnings"))

    def test_hud_ping_flag_default_on(self):
        self.assertIn(("party_hud_ping", True), self.L.PARTY_INI_FLAGS)
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / "bbport.ini"
            self.assertTrue(self.L.ini_flag(ini, "party_hud_ping"))
            self.L.update_ini(ini, {"party_hud_ping": "0"})
            self.assertFalse(self.L.ini_flag(ini, "party_hud_ping"))


if __name__ == "__main__":
    unittest.main()
