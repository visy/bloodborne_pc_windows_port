"""launcher.py Nightwatch VPN lobby client (0.7.7): the reply parser against the real server, the rows (no address
in anything shown), the fetch over loopback, and the wire vector shared with tests/test_party_net.cpp."""
from paths import ROOT
import importlib.util
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, str(ROOT / "tools" / "vpn" / "lobby"))
import nwlobby as nw  # noqa: E402


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_lobby", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# The same bytes as test_vpnlobby() in tests/test_party_net.cpp (the game's encoder).
GAME_VECTOR = bytes.fromhex("fa4c010101245b01040b302e372e3720286162632905416c696365")


class Wire(unittest.TestCase):
    def test_game_vector_matches_server_encoder(self):
        self.assertEqual(nw.encode_announce("Alice", 9307, 1, 4, "0.7.7 (abc)", True), GAME_VECTOR)
        self.assertEqual(nw.encode_bye().hex(), "fa4c0104")

    def test_game_vector_accepted_by_server(self):
        lob = nw.Lobby()
        lob.handle(GAME_VECTOR, "10.8.0.10", 0)
        self.assertEqual(lob.entries["10.8.0.10"][6], "Alice")


class Client(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def reply(self, *hosts):
        lob = nw.Lobby("10.8.0.0/16")
        for i, (name, ip, pw) in enumerate(hosts):
            lob.handle(nw.encode_announce(name, 9307, 1, 4, "0.7.7 (abc)", pw), ip, 0)
        return lob.handle(nw.encode_list(0), "10.8.0.99", 1)

    def test_parse_matches_server_reply(self):
        got = self.L.lobby_parse_reply(self.reply(("Alice", "10.8.0.10", True), ("bob", "10.8.0.11", False)))
        ref = nw.parse_reply(self.reply(("Alice", "10.8.0.10", True), ("bob", "10.8.0.11", False)))
        self.assertEqual(got[0:2], ref[0:2])
        self.assertEqual([(e["name"], e["ip"], e["port"], e["password"]) for e in got[2]],
                         [(e["name"], e["ip"], e["port"], e["password"]) for e in ref[2]])

    def test_parse_is_strict(self):
        good = self.reply(("Alice", "10.8.0.10", True))
        self.assertIsNotNone(self.L.lobby_parse_reply(good))
        for bad in (good[:-1], good + b"\x00", b"", b"\xfaL\x01\x02" + good[4:], good[:3] + b"\x09" + good[4:], None,
                    b"x" * 2000):
            self.assertIsNone(self.L.lobby_parse_reply(bad))
        # a hostile lobby: control characters in a name are stripped; a name of only those is refused
        evil = bytearray(good)
        evil[evil.index(b"Alice")] = 0x07
        e = self.L.lobby_parse_reply(bytes(evil))
        self.assertEqual(e[2][0]["name"], "lice")
        only = bytearray(good)
        only[only.index(b"Alice"):only.index(b"Alice") + 5] = b"\x01\x02\x03\x04\x05"
        self.assertIsNone(self.L.lobby_parse_reply(bytes(only)))

    def test_rows_hide_the_address_and_skip_own(self):
        lob = nw.Lobby("10.8.0.0/16")
        lob.handle(nw.encode_announce("Zed", 9400, 2, 4, "0.7.7 (abc)", True), "10.8.0.20", 0)
        lob.handle(nw.encode_announce("Amy", 9307, 1, 3, "0.7.5 (old)", True), "10.8.0.21", 0)
        lob.handle(nw.encode_announce("Me", 9307, 1, 3, "0.7.7 (abc)", True), "10.8.0.22", 0)
        total, off, ents = nw.parse_reply(lob.handle(nw.encode_list(0), "10.8.0.22", 1))
        rows = self.L.lobby_rows(ents, "0.7.7")
        self.assertEqual([r["name"] for r in rows], ["Amy", "Zed"])
        self.assertEqual([r["ok"] for r in rows], [False, True])
        self.assertEqual(rows[1]["address"], "10.8.0.20:9400")
        shown = [str(v) for r in rows for k, v in r.items() if k not in ("address",)]
        self.assertFalse(any("10.8." in s for s in shown), shown)

    def test_tunnel_ip(self):
        f = self.L.lobby_tunnel_ip
        self.assertEqual(f(["192.168.1.2", "10.8.0.7"]), "10.8.0.7")
        self.assertEqual(f(["10.8.1.7"]), "10.8.1.7")      # 0.7.8: 10.8.0.0/22
        self.assertEqual(f(["10.8.0.255"]), "10.8.0.255")
        self.assertEqual(f(["10.8.3.254"]), "10.8.3.254")
        for none in (["10.8.0.1"], ["10.8.0.0"], ["10.8.3.255"], ["10.8.4.7"], ["10.9.0.7"], [], ["10.8.0.x"]):
            self.assertEqual(f(none), "")

    def test_target_override(self):
        old = os.environ.pop("BB_PARTY_VPNLOBBY_ADDR", None)
        try:
            self.assertEqual(self.L.lobby_target(), (("10.8.0.1", 9310), False))
            os.environ["BB_PARTY_VPNLOBBY_ADDR"] = "127.0.0.1:19999"
            self.assertEqual(self.L.lobby_target(), (("127.0.0.1", 19999), True))
        finally:
            os.environ.pop("BB_PARTY_VPNLOBBY_ADDR", None)
            if old is not None:
                os.environ["BB_PARTY_VPNLOBBY_ADDR"] = old

    def test_fetch_over_loopback(self):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        stop = threading.Event()
        th = threading.Thread(target=nw.serve, args=("127.0.0.1", port, "127.0.0.0/8", stop.is_set, False), daemon=True)
        th.start()
        try:
            time.sleep(0.3)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host:
                host.bind(("127.0.0.1", 0))
                host.sendto(nw.encode_announce("Loop", 9307, 1, 4, "0.7.7 (abc)", True), ("127.0.0.1", port))
                time.sleep(0.3)
                # the fetch comes from another source port on the same address: the lobby keys by address,
                # so this is the same "player" and the entry is flagged as its own
                ents = self.L.lobby_fetch(("127.0.0.1", port), "127.0.0.1")
            self.assertEqual([e["name"] for e in ents], ["Loop"])
            self.assertTrue(ents[0]["own"])
            self.assertEqual(self.L.lobby_rows(ents, "0.7.7"), [])
            # nobody home: None, not a hang
            self.assertIsNone(self.L.lobby_fetch(("127.0.0.1", 9), "127.0.0.1", timeout=0.3))
        finally:
            stop.set()
            th.join(3)


if __name__ == "__main__":
    unittest.main()
