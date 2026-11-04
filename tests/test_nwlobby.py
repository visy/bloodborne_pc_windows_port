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
        for ip in ("192.168.1.5", "10.8.4.5", "10.9.0.5", "127.0.0.1", "8.8.8.8", "10.8.0.0", "10.8.0.1", "10.8.3.255",
                   "::1", "not an ip", ""):
            self.assertIsNone(lob.handle(ann(), ip, 0), ip)
            self.assertIsNone(lob.handle(nw.encode_list(), ip, 0), ip)
        self.assertEqual(len(lob.entries), 0)
        lob.handle(ann(), "10.8.0.2", 0)
        lob.handle(ann(), "10.8.0.254", 0)
        lob.handle(ann(), "10.8.0.255", 0)   # 0.7.8: 10.8.0.0/22, room for 1021 players
        lob.handle(ann(), "10.8.1.5", 0)
        lob.handle(ann(), "10.8.3.254", 0)
        self.assertEqual(len(lob.entries), 5)

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


class Extension(unittest.TestCase):
    """0.7.8: the invader flag and 'where the host is' (types 7-9) and old / new client compatibility."""

    def lobby(self):
        lob = nw.Lobby()
        lob.handle(ann(name="Alice", pw=False), A, 0)
        lob.handle(ann(name="Bob", pw=True), B, 0)
        lob.handle(nw.encode_announce_ext(True, False, 0x1C010000, 4, 1), A, 0)   # Alice: invaders, Yahar'gul, Blood Moon, NG+1
        return lob

    def test_old_client_gets_the_v1_reply_unchanged(self):
        lob = self.lobby()
        ents = nw.parse_reply(lob.handle(nw.encode_list(), C, 1))[2]     # the strict v1 parser, as shipped
        self.assertEqual([(e["name"], e["password"]) for e in ents], [("Alice", False), ("Bob", True)])
        raw = lob.handle(nw.encode_list(), C, 1)
        self.assertEqual(raw[3], nw.T_REPLY)
        # no new flag bit anywhere in a type 3 row: the old parser would refuse the whole page
        for e in ents:
            self.assertNotIn("invaders", e)

    def test_new_client_gets_flags_and_where(self):
        lob = self.lobby()
        raw = lob.handle(nw.encode_list2(), C, 1)
        self.assertEqual(raw[3], nw.T_REPLY2)
        total, off, ents = nw.parse_reply2(raw)
        self.assertEqual((total, off), (2, 0))
        a, b = ents
        self.assertEqual((a["name"], a["invaders"], a["boss"], a["area"], a["phase"], a["ng"]),
                         ("Alice", True, False, 0x1C010000, 4, 1))
        # Bob never sent an extension (an older build): not invadable, nothing known
        self.assertEqual((b["name"], b["invaders"], b["area"], b["phase"], b["ng"]), ("Bob", False, 0, 0, 255))
        # an address is never in the strict v1 view of the same host, and the reply names no address field beyond ip
        self.assertTrue(nw.parse_reply(lob.handle(nw.encode_list(), C, 2)))

    def test_the_shipped_0_7_7_server_ignores_the_new_datagrams(self):
        """The server as deployed before 0.7.8 (git history) drops types 7 and 8 and keeps the host listed."""
        import subprocess, types
        try:
            src = subprocess.run(["git", "show", "c7ed408f:tools/vpn/lobby/nwlobby.py"], cwd=ROOT, capture_output=True,
                                 check=True).stdout.decode("utf-8")
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("git history not available")
        old = types.ModuleType("nwlobby_old")
        exec(compile(src, "nwlobby_old", "exec"), old.__dict__)
        lob = old.Lobby()
        lob.handle(ann(), A, 0)
        self.assertIsNone(lob.handle(nw.encode_announce_ext(True, False, 1, 1, 1), A, 1))
        self.assertIsNone(lob.handle(nw.encode_list2(), C, 1))      # no answer: the client falls back to type 2
        self.assertEqual(len(lob.entries), 1)
        self.assertIsNotNone(nw.parse_reply(lob.handle(nw.encode_list(), C, 2)))

    def test_invaders_off_is_never_listed_invadable(self):
        lob = self.lobby()
        lob.handle(nw.encode_announce_ext(False, False, 0x18010000, 2, 0), A, 5)    # the host switched it off
        a = nw.parse_reply2(lob.handle(nw.encode_list2(), C, 6))[2][0]
        self.assertFalse(a["invaders"])
        self.assertEqual((a["area"], a["phase"], a["ng"]), (0x18010000, 2, 0))

    def test_extension_expires_with_the_entry_and_needs_a_live_entry(self):
        lob = self.lobby()
        nw_ttl = nw.TTL
        a = nw.parse_reply2(lob.handle(nw.encode_list2(), C, nw_ttl - 1))[2]
        self.assertTrue(a[0]["invaders"] if a else False)
        # past the TTL the host (not refreshed) is gone and so is its flag
        self.assertEqual(nw.parse_reply2(lob.handle(nw.encode_list2(), C, nw_ttl + 1))[2], [])
        # an extension from a sender with no entry is ignored (no flag out of nothing)
        lob2 = nw.Lobby()
        lob2.handle(nw.encode_announce_ext(True, True, 1, 1, 1), A, 0)
        self.assertEqual(lob2.ext, {})
        # a host that said bye loses its extension
        lob3 = self.lobby()
        lob3.handle(nw.encode_bye(), A, 1)
        self.assertEqual(lob3.ext, {})

    def test_bad_extensions_dropped(self):
        lob = nw.Lobby()
        lob.handle(ann(), A, 0)
        good = nw.encode_announce_ext(True, True, 5, 3, 7)
        bads = [good[:-1], good + b"x", good[:4] + bytes([4]) + good[5:],        # unknown flag bit
                good[:9] + bytes([5]) + good[10:],                                 # phase 5
                good[:10] + bytes([8]),                                            # ng 8
                good[:4]]
        for i, b in enumerate(bads):
            lob.handle(b, A, 1 + i)
            self.assertEqual(lob.ext, {}, i)
        lob.handle(good, A, 20)
        self.assertEqual(lob.ext[A][1:], (3, 5, 3, 7))

    def test_hostile_senders_and_rate(self):
        lob = nw.Lobby()
        # outside the tunnel network: nothing is stored
        lob.handle(ann(), "192.168.1.5", 0)
        lob.handle(nw.encode_announce_ext(True, False, 1, 1, 1), "192.168.1.5", 0)
        self.assertEqual((lob.entries, lob.ext), ({}, {}))

    def test_reply2_parser_strict_and_paging(self):
        lob = nw.Lobby()
        for i in range(30):
            ip = f"10.8.0.{20 + i}"
            lob.handle(ann(name=f"H{i:02d}", pw=False), ip, 0)
            lob.handle(nw.encode_announce_ext(i % 2 == 0, False, 0x18000000 + i, 1, 0), ip, 0)
        seen, off = [], 0
        for _ in range(10):
            raw = lob.handle(nw.encode_list2(off), C, 1)
            self.assertLessEqual(len(raw), nw.MAX_REPLY)
            total, o, ents = nw.parse_reply2(raw)
            self.assertEqual(o, off)
            seen += ents
            off += len(ents)
            if not ents or off >= total:
                break
        self.assertEqual(len(seen), 30)
        self.assertEqual(sum(1 for e in seen if e["invaders"]), 15)
        raw = lob.handle(nw.encode_list2(), C, 1)
        self.assertIsNone(nw.parse_reply2(raw[:-1]))
        self.assertIsNone(nw.parse_reply2(raw + b"\x00"))
        self.assertIsNone(nw.parse_reply2(nw.MAGIC + bytes([1, 3]) + raw[4:]))


class Stats(unittest.TestCase):
    def test_counts(self):
        lob = nw.Lobby()
        lob.handle(ann(name="A", players=2, maxp=4, pw=True), A, 0)
        lob.handle(ann(name="B", players=3, maxp=3, pw=False), B, 0)
        lob.handle(ann(name="C", players=1, maxp=4, pw=False, build="0.7.6 (x)"), C, 0)
        self.assertEqual(nw.parse_stats(lob.stats(nw.encode_stats(), 1)),
                         {"games": 3, "players": 6, "open": 2, "full": 1, "joinable": 1, "free_slots": 3,
                          "sizes": {"3": 1, "4": 2}, "builds": {"0.7.7": 2, "0.7.6": 1}})

    def test_expired_not_counted(self):
        lob = nw.Lobby()
        lob.handle(ann(), A, 0)
        self.assertEqual(nw.parse_stats(lob.stats(nw.encode_stats(), nw.TTL + 1))["games"], 0)

    def test_stats_only_on_operator_socket(self):
        lob = nw.Lobby()
        lob.handle(ann(), A, 0)
        self.assertIsNone(lob.handle(nw.encode_stats(), B, 1))  # never answered over the tunnel
        self.assertIsNone(lob.stats(nw.encode_stats() + b"x", 1))
        self.assertIsNone(lob.stats(nw.encode_list(), 1))

    def test_reply_has_no_names(self):
        lob = nw.Lobby()
        lob.handle(ann(name="Secretname"), A, 0)
        self.assertNotIn(b"Secretname", lob.stats(nw.encode_stats(), 1))
        self.assertIsNone(nw.parse_stats(nw.MAGIC + bytes([1, 6]) + b"[1]"))
        self.assertIsNone(nw.parse_stats(nw.MAGIC + bytes([1, 6]) + b"{bad"))

    def test_reply_fits_one_datagram(self):
        lob = nw.Lobby(max_entries=300)
        for i in range(250):
            lob.handle(ann(name="P%d" % i, build="v%d-%s" % (i, "x" * 30)), "10.8.0.%d" % (i + 2), 0)
        self.assertLessEqual(len(lob.stats(nw.encode_stats(), 1)), nw.MAX_REPLY)
        self.assertEqual(len(nw.parse_stats(lob.stats(nw.encode_stats(), 1))["builds"]), 5)


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
            c.settimeout(0.3)
            reply = None
            for _ in range(20):  # the server thread may not be bound yet (Windows then reports a reset)
                try:
                    c.sendto(ann(name="Lo"), ("127.0.0.1", port))
                    time.sleep(0.05)
                    c.sendto(nw.encode_list(), ("127.0.0.1", port))
                    reply = c.recvfrom(2048)[0]
                    if nw.parse_reply(reply)[2]:
                        break  # an announce sent before the bind is lost: retry until it shows
                except (ConnectionResetError, socket.timeout):
                    time.sleep(0.1)
            self.assertIsNotNone(reply, "no reply from the lobby")
            total, off, ents = nw.parse_reply(reply)
            self.assertEqual((total, ents[0]["name"], ents[0]["own"]), (1, "Lo", True))
            c.close()
        finally:
            stop.set()
            th.join(3)

    def test_stats_socket(self):
        ports = []
        for _ in range(2):
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.bind(("127.0.0.1", 0))
            ports.append(probe.getsockname()[1])
            probe.close()
        port, sport = ports
        stop = threading.Event()
        th = threading.Thread(target=nw.serve, args=("127.0.0.1", port, "127.0.0.0/8", stop.is_set, False),
                              kwargs={"stats": ("127.0.0.1", sport)}, daemon=True)
        th.start()
        try:
            c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            c.settimeout(0.3)
            got = None
            for _ in range(20):
                try:
                    c.sendto(ann(name="Lo", players=2), ("127.0.0.1", port))
                    time.sleep(0.05)
                    c.sendto(nw.encode_stats(), ("127.0.0.1", sport))
                    got = nw.parse_stats(c.recvfrom(2048)[0])
                    if got and got["games"]:
                        break
                except (ConnectionResetError, socket.timeout):
                    time.sleep(0.1)
            self.assertEqual((got["games"], got["players"], got["open"], got["full"]), (1, 2, 0, 0))
            c.close()
        finally:
            stop.set()
            th.join(3)

    def test_no_files_no_prints(self):
        src = open(os.path.join(ROOT, "tools", "vpn", "lobby", "nwlobby.py")).read()
        for bad in ("open(", "print(", "logging", "syslog"):
            self.assertNotIn(bad, src, bad)


class ModsLabel(unittest.TestCase):
    """0.7.8: the mods label (types 10-12): relayed as-is, strict charset, compatible with type 8/9 and 2/3 clients."""

    def lobby(self):
        lob = nw.Lobby()
        lob.handle(ann(name="Alice", pw=False), A, 0)
        lob.handle(ann(name="Bob", pw=False), B, 0)
        lob.handle(nw.encode_announce_ext(True, False, 0x1C010000, 4, 1), A, 0)
        lob.handle(nw.encode_announce_mods("nocturne@1.0"), A, 0)
        return lob

    def test_reply3_carries_the_label(self):
        raw = self.lobby().handle(nw.encode_list3(), C, 1)
        self.assertEqual(raw[3], nw.T_REPLY3)
        total, off, (a, b) = nw.parse_reply3(raw)
        self.assertEqual((total, off), (2, 0))
        self.assertEqual((a["name"], a["mods"], a["invaders"], a["area"]), ("Alice", "nocturne@1.0", True, 0x1C010000))
        self.assertEqual((b["name"], b["mods"]), ("Bob", ""))   # no type 10: no label

    def test_older_clients_unchanged(self):
        lob = self.lobby()
        e2 = nw.parse_reply2(lob.handle(nw.encode_list2(), C, 1))[2]
        self.assertTrue(all("mods" not in e for e in e2))
        self.assertIsNotNone(nw.parse_reply(lob.handle(nw.encode_list(), C, 2)))
        self.assertEqual(lob.handle(nw.encode_list2(), C, 3)[3], nw.T_REPLY2)

    def test_label_charset_and_length(self):
        for good in ["nocturne@1.0", "mods:3f9a12c0", "a", "x" * 32, "a-b_c.d+e:f@0"]:
            self.assertEqual(nw.parse_announce_mods(nw.encode_announce_mods(good)[4:]), good)
        for bad in ["", "Nocturne@1.0", "x" * 33, "a b", "a/b", "../x", "a\\b", "é"]:
            raw = bad.encode("utf-8")
            body = bytes([len(raw) & 255]) + raw
            self.assertIsNone(nw.parse_announce_mods(body), bad)
        self.assertIsNone(nw.parse_announce_mods(b"\x05abc"))      # length beyond the datagram
        self.assertIsNone(nw.parse_announce_mods(b"\x01ab"))       # trailing byte

    def test_bad_label_not_stored_and_needs_a_live_entry(self):
        lob = nw.Lobby()
        lob.handle(nw.encode_announce_mods("nocturne@1.0"), A, 0)   # no type 1 yet
        self.assertEqual(lob.mods, {})
        lob.handle(ann(), A, 1)
        lob.handle(nw.MAGIC + bytes([1, nw.T_ANNOUNCE_MODS, 3]) + b"A/B", A, 2)
        self.assertEqual(lob.mods, {})
        lob.handle(nw.encode_announce_mods("mods:00ff00ff"), A, 3)
        self.assertEqual(lob.mods[A][1], "mods:00ff00ff")

    def test_label_expires_and_bye_clears_it(self):
        lob = self.lobby()
        self.assertEqual(nw.parse_reply3(lob.handle(nw.encode_list3(), C, nw.TTL + 1))[2], [])
        self.assertEqual(lob.mods, {})
        lob = self.lobby()
        lob.handle(nw.encode_bye(), A, 1)
        self.assertEqual(lob.mods, {})

    def test_latest_label_wins(self):
        lob = self.lobby()
        lob.handle(nw.encode_announce_mods("nocturne@1.1"), A, 5)
        self.assertEqual(nw.parse_reply3(lob.handle(nw.encode_list3(), C, 6))[2][0]["mods"], "nocturne@1.1")

    def test_reply3_parser_strict_and_paging_with_long_labels(self):
        lob = nw.Lobby()
        for i in range(30):
            ip = f"10.8.0.{20 + i}"
            lob.handle(ann(name="H" * 16, pw=False, build="b" * 32), ip, 0)
            lob.handle(nw.encode_announce_mods(f"mods:{i:027d}"), ip, 0)
        seen, off = 0, 0
        for _ in range(10):
            raw = lob.handle(nw.encode_list3(off), C, 1)
            self.assertLessEqual(len(raw), nw.MAX_REPLY)
            total, _, ents = nw.parse_reply3(raw)
            if not ents:
                break
            self.assertTrue(all(len(e["mods"]) == 32 for e in ents))
            seen += len(ents)
            off += len(ents)
        self.assertEqual(seen, 30)
        good = lob.handle(nw.encode_list3(), C, 2)
        self.assertIsNone(nw.parse_reply3(good[:-1]))
        self.assertIsNone(nw.parse_reply3(good + b"x"))
        self.assertIsNone(nw.parse_reply2(good))

    def test_the_0_7_8_invasion_server_ignores_type_10_and_11(self):
        """The server with types 7-9 only (git history) drops the new datagrams and keeps the host listed."""
        import subprocess, types
        try:
            src = subprocess.run(["git", "show", "9f7f64bb:tools/vpn/lobby/nwlobby.py"], cwd=ROOT, capture_output=True,
                                 check=True).stdout.decode("utf-8")
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("git history not available")
        old = types.ModuleType("nwlobby_078")
        exec(compile(src, "nwlobby_078", "exec"), old.__dict__)
        lob = old.Lobby()
        lob.handle(ann(), A, 0)
        self.assertIsNone(lob.handle(nw.encode_announce_mods("nocturne@1.0"), A, 1))
        self.assertIsNone(lob.handle(nw.encode_list3(), C, 1))      # no answer: the client falls back to type 8
        self.assertEqual(len(lob.entries), 1)
        self.assertIsNotNone(nw.parse_reply2(lob.handle(nw.encode_list2(), C, 2)))


if __name__ == "__main__":
    unittest.main()


class NameFilterTests(unittest.TestCase):
    """The interim offensive-name filter: a masked name in the list, look-alike tricks resolved, and
    ordinary Bloodborne names never masked."""

    POSITIVE = ["Black ppl hunter", "B1ack p30ple hunt3r", "blackpplhunter", "\u0412l\u0430\u0441k \u0440\u0440l hunt\u0435r",
                "\uff22\uff4c\uff41\uff43\uff4b \uff50\uff50\uff4c \uff48\uff55\uff4e\uff54\uff45\uff52",
                "\U0001d401\U0001d425\U0001d41a\U0001d41c\U0001d424 \U0001d429\U0001d429\U0001d425 hunter",
                "B\u200black p\u200dpl hunter", "\u24d1\u24db\u24d0\u24d2\u24da \u24df\u24df\u24db hunter",
                "b\u0337l\u0337a\u0337c\u0337k\u0337 p\u0337p\u0337l\u0337 hunter", "\u202eretnuh lpp kcalb",
                "Jews hunter", "blacks hunter", "muslim hater", "gas the jews", "Sieg Heil", "1488", "kkk member",
                "N I G G E R", "n.i.g.g.a", "niiiigger", "mexicans die"]
    NEGATIVE = ["Black Hunter", "Beast Hunter", "Hunter of Hunters", "Gehrman", "Vileblood", "Scunthorpe",
                "Classic Assassin", "Lobo Negro", "Niger", "Nigeria", "Snigger", "Chinese Hunter", "Gay Slayer",
                "Jewel Hunter", "Blacksmith", "Black Knight", "Hunter 88", "Dyke Ave", "Killer Queen", "Kirkhammer",
                "Lady Maria", "Eileen the Crow", "Executioner", "Dragonslayer", "\u0410\u0433\u0430\u0442\u0430", "\u592a\u90ce"]

    def test_positive(self):
        for n in self.POSITIVE:
            self.assertTrue(nw.name_offensive(n), n)

    def test_negative(self):
        for n in self.NEGATIVE:
            self.assertFalse(nw.name_offensive(n), n)

    def test_listed_masked(self):
        lob = nw.Lobby(reserve_server=False)
        ann = nw.encode_announce("Black ppl hunter", 9307, 1, 3, "0.7.8", False)
        lob.handle(ann, "10.8.0.5", 0.0)
        rows = nw.parse_reply(lob.handle(nw.encode_list(0), "10.8.0.6", 1.0))
        names = [r["name"] for r in rows[2]]
        self.assertEqual(names, [nw.masked_name("Black ppl hunter")])
        self.assertTrue(names[0].startswith("Hunter "))

    def test_deployed_layout_under_isolated_python(self):
        """/opt/nwlobby holds nwlobby.py, namefilter.py and namefilter_data.py; the unit runs python3 -I."""
        import shutil, subprocess, sys, tempfile
        from pathlib import Path
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as d:
            shutil.copy(root / "tools" / "vpn" / "lobby" / "nwlobby.py", d)
            shutil.copy(root / "scripts" / "namefilter.py", d)
            shutil.copy(root / "scripts" / "namefilter_data.py", d)
            code = ("import importlib.util,sys;s=importlib.util.spec_from_file_location('nw',sys.argv[1]);"
                    "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                    "print(m.name_offensive('Black ppl hunter'), m.name_offensive('Black Hunter'), m.masked_name('Black ppl hunter'))")
            out = subprocess.run([sys.executable, "-I", "-c", code, str(Path(d) / "nwlobby.py")], capture_output=True, text=True)
            self.assertTrue(out.stdout.startswith("True False Hunter "), out.stdout + out.stderr)
