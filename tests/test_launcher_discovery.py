"""launcher.py party discovery (0.7.5): the announcement parser (mirrors gpu/shim/net/party_discovery.cpp),
the list book (own packets, rate limit, expiry, cap), Tailscale peers, the rows and the open-network warning."""
from paths import ROOT
import importlib.util
import json
import socket
import time
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_disc", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def packet(pid=0xDEADBEEF, port=9307, level=120, players=2, maxp=4, password=True, build="0.7.5 (abc)",
           name="Hunter_One", area="the Cathedral Ward", flags=None, version=1):
    def s(t):
        raw = t.encode("ascii")
        return bytes([len(raw)]) + raw
    fl = (1 if password else 0) if flags is None else flags
    return (bytes([0xFA, 0x41, version, fl]) + pid.to_bytes(4, "big") + port.to_bytes(2, "big") +
            level.to_bytes(2, "big") + bytes([players, maxp]) + s(build) + s(name) + s(area))


class Discovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_parse_valid(self):
        e = self.L.parse_discovery_packet(packet())
        self.assertEqual((e["name"], e["level"], e["players"], e["max"], e["port"], e["password"]),
                         ("Hunter_One", 120, 2, 4, 9307, True))
        self.assertEqual((e["build"], e["area"], e["id"]), ("0.7.5 (abc)", "the Cathedral Ward", 0xDEADBEEF))
        self.assertFalse(self.L.parse_discovery_packet(packet(password=False))["password"])

    def test_parse_rejects(self):
        good = packet()
        P = self.L.parse_discovery_packet
        for n in range(len(good)):  # every truncation
            self.assertIsNone(P(good[:n]), n)
        self.assertIsNone(P(good + b"\0"))
        self.assertIsNone(P(packet(version=2)))
        self.assertIsNone(P(packet(flags=2)))
        self.assertIsNone(P(packet(port=0)))
        self.assertIsNone(P(packet(players=5, maxp=4)))
        self.assertIsNone(P(packet(maxp=9, players=1)))
        self.assertIsNone(P(packet(name="")))
        self.assertIsNone(P(b"\xfa" + b"x" * 300))
        self.assertIsNone(P("text"))
        bad = bytearray(good)
        bad[15 + len("0.7.5 (abc)")] = 7  # a control character in the name
        self.assertIsNone(P(bytes(bad)))
        bad = bytearray(good)
        bad[14] = 200  # a length that overruns
        self.assertIsNone(P(bytes(bad)))

    def test_parse_junk_never_raises(self):
        import random
        rnd = random.Random(7)
        for _ in range(5000):
            data = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 90)))
            if len(data) > 3 and rnd.random() < 0.5:
                data = b"\xfa\x41\x01" + bytes([data[3] & 1]) + data[4:]
            e = self.L.parse_discovery_packet(data)
            if e:
                self.assertTrue(e["port"] and e["players"] <= e["max"] and e["name"])

    def test_book(self):
        B = self.L.DiscoveryBook
        book = B(own_ips={"192.168.1.20"})
        self.assertFalse(book.feed(packet(), "192.168.1.20", 100.0))  # own packet
        self.assertTrue(book.feed(packet(), "192.168.1.30", 100.0))
        self.assertTrue(book.feed(packet(players=3), "192.168.1.30", 101.0))  # same host: updated
        self.assertTrue(book.feed(packet(pid=5, name="Other"), "26.1.2.3", 103.0))
        snap = book.snapshot(102.0)
        self.assertEqual([e["name"] for e in snap], ["Hunter_One", "Other"])
        self.assertEqual(snap[0]["players"], 3)
        self.assertEqual(len(book.snapshot(107.5)), 1)  # Hunter_One (101) expired, Other (103) is not
        self.assertEqual(book.snapshot(110.0), [])
        self.assertFalse(book.feed(b"junk", "10.0.0.9", 120.0))

    def test_book_rate_limit_and_cap(self):
        book = self.L.DiscoveryBook()
        ok = sum(book.feed(packet(pid=i), "10.0.0.5", 50.2) for i in range(100))
        self.assertEqual(ok, self.L.DISCOVERY_MAX_PER_SEC)  # one source cannot flood
        self.assertTrue(book.feed(packet(pid=999), "10.0.0.5", 51.1))  # next second
        book = self.L.DiscoveryBook()
        taken = 0
        for i in range(300):  # many sources, one packet each: capped
            taken += book.feed(packet(pid=i), f"10.1.{i // 250}.{i % 250}", 10.0)
        self.assertEqual(taken, self.L.DISCOVERY_MAX_ENTRIES)
        self.assertEqual(len(book.snapshot(10.0)), self.L.DISCOVERY_MAX_ENTRIES)

    def test_listener_loopback(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        book = self.L.DiscoveryBook()
        lst = self.L.DiscoveryListener(book, port=port)
        lst.start()
        try:
            time.sleep(0.3)
            tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            tx.sendto(packet(), ("127.0.0.1", port))
            tx.sendto(b"garbage", ("127.0.0.1", port))
            tx.close()
            deadline = time.monotonic() + 3
            snap = []
            while time.monotonic() < deadline and not snap:
                time.sleep(0.1)
                snap = book.snapshot(time.monotonic())
            self.assertEqual([e["name"] for e in snap], ["Hunter_One"])
            self.assertEqual(snap[0]["ip"], "127.0.0.1")
        finally:
            lst.stop()
            lst.join(2)

    def test_build_match_and_rows(self):
        L = self.L
        self.assertTrue(L.discovery_build_matches("0.7.5 (abc)", "0.7.5"))
        self.assertTrue(L.discovery_build_matches("0.7.5 (dirty)", "0.7.5 (abc)"))
        self.assertFalse(L.discovery_build_matches("0.7.4 (abc)", "0.7.5"))
        found = [dict(L.parse_discovery_packet(packet()), ip="26.1.2.3"),
                 dict(L.parse_discovery_packet(packet(pid=2, name="Old", build="0.7.4 (x)", password=False)),
                      ip="26.1.2.4")]
        rows = L.discovery_rows(found, [], "0.7.5")
        self.assertEqual([r["name"] for r in rows], ["Hunter_One", "Old"])
        self.assertEqual((rows[0]["address"], rows[0]["players"], rows[0]["password"], rows[0]["ok"]),
                         ("26.1.2.3:9307", "2/4", "yes", True))
        self.assertEqual((rows[1]["password"], rows[1]["ok"]), ("no", False))  # greyed: other build
        # the same game heard on two adapters (same id) is one row
        both = [dict(L.parse_discovery_packet(packet()), ip="192.168.1.5"),
                dict(L.parse_discovery_packet(packet()), ip="26.1.2.3")]
        self.assertEqual(len(L.discovery_rows(both, [], "0.7.5")), 1)

    def test_tailscale(self):
        L = self.L
        text = json.dumps({"Self": {"HostName": "me"}, "Peer": {
            "a": {"HostName": "Friend-PC", "TailscaleIPs": ["100.64.0.2", "fd7a::2"], "Online": True},
            "b": {"HostName": "Asleep", "TailscaleIPs": ["100.64.0.3"], "Online": False},
            "c": {"HostName": "NoIp", "TailscaleIPs": [], "Online": True}}})
        peers = L.parse_tailscale_status(text)
        self.assertEqual([(p["name"], p["ip"], p["online"]) for p in peers],
                         [("Friend-PC", "100.64.0.2", True), ("Asleep", "100.64.0.3", False)])
        for bad in ("", "nope", "[]", "null", '{"Peer": 5}', '{"Peer": {"x": 3}}'):
            self.assertEqual(L.parse_tailscale_status(bad), [])
        rows = L.discovery_rows([], peers, "0.7.5")  # only online peers, details unknown
        self.assertEqual(len(rows), 1)
        self.assertEqual((rows[0]["address"], rows[0]["kind"], rows[0]["password_set"]),
                         ("100.64.0.2:9307", "tailscale", None))
        # a peer that announced itself is not listed twice
        found = [dict(L.parse_discovery_packet(packet()), ip="100.64.0.2")]
        self.assertEqual(len(L.discovery_rows(found, peers, "0.7.5")), 1)

    def test_open_network_warning(self):
        W = self.L.discovery_open_network_warning
        self.assertIn("Radmin VPN", W("host", "", "Radmin VPN"))
        self.assertIn("Hamachi", W("host", "  ", "Hamachi"))
        self.assertEqual(W("host", "secret", "Radmin VPN"), "")
        self.assertEqual(W("join", "", "Radmin VPN"), "")
        self.assertEqual(W("host", "", ""), "")       # LAN: nothing to say
        self.assertEqual(W("host", "", "ZeroTier"), "")  # private networks


if __name__ == "__main__":
    unittest.main()
