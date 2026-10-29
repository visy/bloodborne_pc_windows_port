"""Tests for tools/vpn/lobby/nwlobby.py (the Nightwatch VPN game lobby). Synthetic data only.

    python -m unittest tests.test_nwlobby        (from the repo root)
"""
import os, socket, sys, threading, time, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "vpn", "lobby"))
import nwlobby as nw  # noqa: E402

A, B, C = "10.8.0.10", "10.8.0.11", "10.8.0.12"


def ann(name="Alice", port=9307, players=1, maxp=4, build="0.7.7 (abc)", pw=True):
    return nw.encode_announce(name, port, players, maxp, build, pw)


def raw_announce(flags=1, port=9307, players=1, maxp=4, build=b"", name=b"A"):
    return (nw.MAGIC + bytes([1, 1, flags, port >> 8, port & 255, players, maxp, len(build)]) + build +
            bytes([len(name)]) + name)


class Parse(unittest.TestCase):
    def test_roundtrip(self):
        lob = nw.Lobby()
        self.assertIsNone(lob.handle(ann(), A, 0))
        total, off, ents = nw.parse_reply(lob.handle(nw.encode_list(0), B, 1))
        self.assertEqual((total, off, len(ents)), (1, 0, 1))
        e = ents[0]
        self.assertEqual((e["ip"], e["port"], e["name"], e["players"], e["max"], e["password"], e["build"], e["own"]),
                         (A, 9307, "Alice", 1, 4, True, "0.7.7 (abc)", False))

    def test_own_flag(self):
        lob = nw.Lobby()
        lob.handle(ann(), A, 0)
        ents = nw.parse_reply(lob.handle(nw.encode_list(), A, 1))[2]
        self.assertTrue(ents[0]["own"])

    def test_bad_packets_dropped(self):
        lob = nw.Lobby()
        good = ann()
        bads = [b"", b"\xfaL", good[:-1], good + b"x", b"\xfaL\x02\x01" + good[4:], b"xxL" + good[3:],
                good[:3] + b"\x09" + good[4:],                    # unknown type
                raw_announce(flags=0x80),                          # unknown flag bit
                raw_announce(port=80),                             # port < 1024
                raw_announce(players=5, maxp=4),                   # players > max
                raw_announce(maxp=9),                              # max > 8
                raw_announce(maxp=0),                              # max 0
                raw_announce(name=b""),                            # empty name
                raw_announce(name=b"\xff\xfe"),                    # not UTF-8
                raw_announce(name=b"\x01\x02"),                    # only control characters
                raw_announce(build=b"\x07"),                       # control char in the build
                raw_announce(build=b"x" * 33),                     # build too long
                ]
        for i, b in enumerate(bads):
            self.assertIsNone(lob.handle(b, A, i), i)
            self.assertEqual(len(lob.entries), 0, i)
        self.assertIsNotNone(nw.parse_announce(raw_announce()[4:]))

    def test_list_request_strict(self):
        lob = nw.Lobby()
        lob.handle(ann(), A, 0)
        self.assertIsNone(lob.handle(nw.encode_list() + b"\x00", B, 1))
        self.assertIsNone(lob.handle(nw.MAGIC + bytes([1, 2, 0]), B, 2))

    def test_oversize_packet(self):
        lob = nw.Lobby()
        self.assertIsNone(lob.handle(ann() + b"\x00" * 600, A, 0))
        self.assertEqual(len(lob.entries), 0)
        self.assertIsNone(lob.handle(b"\xfaL\x01\x09" + b"\x00" * (nw.MAX_PACKET - 4), A, 1))

    def test_never_raises(self):
        lob = nw.Lobby()
        for n in range(0, 40):
            lob.handle(os.urandom(n), A, n * 0.01)
        self.assertIsNone(lob.handle(None, A, 0))


class Names(unittest.TestCase):
    def test_control_and_format_stripped(self):
        self.assertEqual(nw.clean_name("Al\x00ic\ne\r\t".encode()), "Alice")
        self.assertEqual(nw.clean_name("A‮B​C".encode()), "ABC")   # bidi override, zero width
        self.assertEqual(nw.clean_name("A B".encode()), "AB")           # line separator
        self.assertEqual(nw.clean_name("  A   B  ".encode()), "A B")
        self.assertEqual(nw.clean_name("ハンター".encode()), "ハンター")
        self.assertIsNone(nw.clean_name(b"\x01\x02"))
        self.assertIsNone(nw.clean_name("​‌".encode()))

    def test_length_cap(self):
        self.assertEqual(nw.clean_name(b"x" * 48), "x" * nw.NAME_MAX_CHARS)
        lob = nw.Lobby()
        self.assertIsNone(lob.handle(raw_announce(name=b"x" * 49), A, 0))     # > 48 bytes: rejected
        self.assertEqual(len(lob.entries), 0)
        lob.handle(ann(name="y" * 40), A, 1)
        self.assertEqual(len(lob.entries[A][6]), nw.NAME_MAX_CHARS)

    def test_stored_name_is_clean(self):
        lob = nw.Lobby()
        lob.handle(ann(name="Bob\x07‮"), A, 0)
        self.assertEqual(nw.parse_reply(lob.handle(nw.encode_list(), B, 1))[2][0]["name"], "Bob")


class Rules(unittest.TestCase):
    def test_source_filter(self):
        lob = nw.Lobby()
        for ip in ("192.168.1.5", "10.8.1.5", "127.0.0.1", "8.8.8.8", "10.8.0.0", "10.8.0.1", "10.8.0.255",
                   "::1", "not an ip", ""):
            self.assertIsNone(lob.handle(ann(), ip, 0), ip)
            self.assertIsNone(lob.handle(nw.encode_list(), ip, 0), ip)
        self.assertEqual(len(lob.entries), 0)
        lob.handle(ann(), "10.8.0.2", 0)
        lob.handle(ann(), "10.8.0.254", 0)
        self.assertEqual(len(lob.entries), 2)

    def test_test_override_net(self):
        lob = nw.Lobby("127.0.0.0/8", reserve_server=False)
        lob.handle(ann(), "127.0.0.1", 0)
        self.assertEqual(len(lob.entries), 1)

    def test_one_entry_per_source(self):
        lob = nw.Lobby()
        lob.handle(ann(name="One"), A, 0)
        lob.handle(ann(name="Two", port=9400), A, 1)
        self.assertEqual(len(lob.entries), 1)
        e = nw.parse_reply(lob.handle(nw.encode_list(), B, 2))[2]
        self.assertEqual((e[0]["name"], e[0]["port"]), ("Two", 9400))

    def test_bye(self):
        lob = nw.Lobby()
        lob.handle(ann(), A, 0)
        lob.handle(nw.encode_bye(), B, 1)       # someone else's bye does nothing
        self.assertEqual(len(lob.entries), 1)
        lob.handle(nw.encode_bye(), A, 2)
        self.assertEqual(len(lob.entries), 0)

    def test_expiry(self):
        lob = nw.Lobby()
        lob.handle(ann(), A, 0)
        self.assertEqual(nw.parse_reply(lob.handle(nw.encode_list(), B, 29))[0], 1)
        self.assertEqual(nw.parse_reply(lob.handle(nw.encode_list(), B, 31))[0], 0)
        lob.handle(ann(), A, 40)                 # re-announce keeps it alive
        lob.handle(ann(), A, 60)
        self.assertEqual(nw.parse_reply(lob.handle(nw.encode_list(), B, 85))[0], 1)

    def test_entry_cap(self):
        lob = nw.Lobby("10.8.0.0/16", max_entries=300)
        ips = ["10.8.%d.%d" % (j // 200, 2 + j % 200) for j in range(400)]
        for i, ip in enumerate(ips):
            lob.handle(ann(name="p%d" % i), ip, 0)
        self.assertEqual(len(lob.entries), 300)
        lob.handle(ann(name="renamed"), ips[0], 1)              # an existing source can still update
        self.assertEqual(lob.entries[ips[0]][6], "renamed")
        lob.handle(ann(name="late"), ips[399], 100)             # after expiry there is room again
        self.assertIn(ips[399], lob.entries)

    def test_rate_limit(self):
        lob = nw.Lobby()
        got = sum(1 for i in range(50) if lob.handle(nw.encode_list(), A, 0.0) is not None)
        self.assertEqual(got, int(nw.RATE_BURST))
        self.assertIsNotNone(lob.handle(nw.encode_list(), B, 0.0))   # another source is unaffected
        self.assertIsNotNone(lob.handle(nw.encode_list(), A, 2.0))   # the first recovers with time

    def test_rate_limit_drops_announce_too(self):
        lob = nw.Lobby()
        for i in range(20):
            lob.handle(ann(name="n%d" % i), A, 0.0)
        self.assertEqual(lob.entries[A][6], "n%d" % (int(nw.RATE_BURST) - 1))

    def test_strangers_leave_no_state(self):
        lob = nw.Lobby()
        lob.handle(ann(), "192.168.9.9", 0)
        self.assertEqual(lob.buckets, {})


class Reply(unittest.TestCase):
    def fill(self, n):
        lob = nw.Lobby("10.8.0.0/16")
        for i in range(n):
            lob.handle(ann(name="Hunter%03d" % i, build="0.7.7 (0123456789abcdef0123456789ab)"),
                       "10.8.%d.%d" % (i // 200, 2 + i % 200), 0)
        return lob

    def test_paging_covers_everything_without_overlap(self):
        lob = self.fill(100)
        names, off = [], 0
        for _ in range(40):
            lob.buckets.clear()
            data = lob.handle(nw.encode_list(off), B, 0)
            self.assertLessEqual(len(data), nw.MAX_REPLY)
            total, o, ents = nw.parse_reply(data)
            self.assertEqual((total, o), (100, off))
            if not ents:
                break
            self.assertLessEqual(len(ents), nw.MAX_PAGE_ENTRIES)
            names += [e["name"] for e in ents]
            off += len(ents)
        self.assertEqual(names, sorted(names))
        self.assertEqual(len(set(names)), 100)

    def test_size_cap_with_longest_names(self):
        lob = nw.Lobby("10.8.0.0/16")
        for i in range(30):
            lob.handle(ann(name="ハ" * 16, build="B" * 32), "10.8.0.%d" % (2 + i), 0)
        data = lob.handle(nw.encode_list(0), B, 1)
        self.assertLessEqual(len(data), nw.MAX_REPLY)
        total, off, ents = nw.parse_reply(data)
        self.assertEqual(total, 30)
        self.assertGreaterEqual(len(ents), 1)
        self.assertLessEqual(len(ents), nw.MAX_PAGE_ENTRIES)

    def test_offset_past_end(self):
        lob = self.fill(3)
        self.assertEqual(nw.parse_reply(lob.handle(nw.encode_list(500), B, 0)), (3, 500, []))

    def test_reply_parser_strict(self):
        lob = self.fill(2)
        data = lob.handle(nw.encode_list(), B, 0)
        self.assertIsNotNone(nw.parse_reply(data))
        self.assertIsNone(nw.parse_reply(data[:-1]))
        self.assertIsNone(nw.parse_reply(data + b"\x00"))
        self.assertIsNone(nw.parse_reply(b"\xfaL\x02\x03" + data[4:]))


class OverUdp(unittest.TestCase):
    def test_serve_on_loopback(self):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        stop = threading.Event()
        th = threading.Thread(target=nw.serve, args=("127.0.0.1", port, "127.0.0.0/8", stop.is_set, False), daemon=True)
        th.start()
        try:
            c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            c.settimeout(2)
            c.sendto(ann(name="Lo"), ("127.0.0.1", port))
            time.sleep(0.3)
            c.sendto(nw.encode_list(), ("127.0.0.1", port))
            total, off, ents = nw.parse_reply(c.recvfrom(2048)[0])
            self.assertEqual((total, ents[0]["name"], ents[0]["own"]), (1, "Lo", True))
            c.close()
        finally:
            stop.set()
            th.join(3)

    def test_no_files_no_prints(self):
        src = open(os.path.join(ROOT, "tools", "vpn", "lobby", "nwlobby.py")).read()
        for bad in ("open(", "print(", "logging", "syslog"):
            self.assertNotIn(bad, src, bad)


if __name__ == "__main__":
    unittest.main()
