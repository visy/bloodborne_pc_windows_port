"""0.7.8 invasions in the launcher and the lists: the 'where the host is' words, the extended lobby reply (type 9) against
the real server, the old / new client fallbacks over loopback, the LAN extension datagram and the Invade button rules.
Synthetic data only."""
from paths import ROOT
import importlib.util
import socket
import sys
import threading
import unittest

sys.path.insert(0, str(ROOT / "tools" / "vpn" / "lobby"))
sys.path.insert(0, str(ROOT / "scripts"))
import nwlobby as nw  # noqa: E402
import party_where as pw  # noqa: E402


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_invade", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Words(unittest.TestCase):
    def test_currently_in(self):
        self.assertEqual(pw.currently_in(0x1C010000, 4, 0), "Currently in: Yahar'gul (Blood Moon), NG")
        self.assertEqual(pw.currently_in(0x18000000, 2, 3, True),
                         "Currently in: Cathedral Ward (evening), NG+3, boss fight")
        self.assertEqual(pw.where_text(0x1D000000, 1, 255), "Chalice Dungeon (day)")
        self.assertEqual(pw.where_text(0x15000100, 3, 255), "Hunter's Dream (night)")

    def test_unknown_is_quiet(self):
        self.assertEqual(pw.currently_in(0, 0, 255), "")
        self.assertEqual(pw.where_text(0x63000000, 0, 255), "")          # an area id nobody knows: no text, no crash
        self.assertEqual(pw.where_text(0, 4, 255), "Blood Moon")
        self.assertEqual(pw.ng_name(8), "")
        self.assertEqual(pw.ng_name(7), "NG+7")
        self.assertEqual(pw.phase_name(9), "")

    def test_every_area_has_a_name_and_no_address_leaks(self):
        for aa, name in pw.AREA_NAMES.items():
            self.assertTrue(name and 0 < aa < 256)


class Client(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def lobby(self):
        lob = nw.Lobby("10.8.0.0/16")
        lob.handle(nw.encode_announce("Alice", 9307, 1, 4, "0.7.8 (abc)", True), "10.8.0.10", 0)
        lob.handle(nw.encode_announce_ext(True, False, 0x1C010000, 4, 1), "10.8.0.10", 0)
        lob.handle(nw.encode_announce("Bob", 9307, 4, 4, "0.7.8 (abc)", False), "10.8.0.11", 0)
        lob.handle(nw.encode_announce_ext(True, True, 0x18000000, 2, 0), "10.8.0.11", 0)   # allows invaders but full
        lob.handle(nw.encode_announce("Cy", 9307, 1, 4, "0.7.8 (abc)", False), "10.8.0.12", 0)    # no extension: old build
        return lob

    def test_reply2_parser_matches_server_and_is_strict(self):
        raw = self.lobby().handle(nw.encode_list2(0), "10.8.0.99", 1)
        got = self.L.lobby_parse_reply2(raw)
        ref = nw.parse_reply2(raw)
        self.assertEqual(got[0:2], ref[0:2])
        self.assertEqual(got[2], ref[2])
        for bad in (raw[:-1], raw + b"\x00", b"", None, nw.MAGIC + bytes([1, 3]) + raw[4:], b"x" * 2000):
            self.assertIsNone(self.L.lobby_parse_reply2(bad))
        # a flag bit the server never sets
        evil = bytearray(raw)
        evil[9 + 6] |= 0x40
        self.assertIsNone(self.L.lobby_parse_reply2(bytes(evil)))

    def test_rows_invadable_only_when_flagged_and_not_full(self):
        raw = self.lobby().handle(nw.encode_list2(0), "10.8.0.99", 1)
        rows = {r["name"]: r for r in self.L.lobby_rows(self.L.lobby_parse_reply2(raw)[2], "0.7.8")}
        self.assertTrue(rows["Alice"]["invadable"])
        self.assertEqual(rows["Alice"]["where"], "Currently in: Yahar'gul (Blood Moon), NG+1")
        self.assertEqual(rows["Alice"]["area"], "Yahar'gul (Blood Moon)")
        self.assertFalse(rows["Bob"]["invadable"])                 # full
        self.assertIn("boss fight", rows["Bob"]["where"])
        self.assertFalse(rows["Cy"]["invadable"])                  # an older host: nothing announced
        self.assertEqual(rows["Cy"]["area"], "-")
        # the address stays in "address" only and never in anything shown
        for r in rows.values():
            self.assertNotIn("10.8.0", r["name"] + r["area"] + r["where"] + r["players"])

    def serve(self, lobby, answer_ext):
        """A loopback lobby; answer_ext False behaves like the 0.7.7 server (drops types 7-9)."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(0.2)
        stop = threading.Event()

        def run():
            while not stop.is_set():
                try:
                    data, addr = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if len(data) >= 4 and data[3] == 8 and not answer_ext:
                    continue
                reply = lobby.handle(data, "10.8.0.99", 1)
                if reply:
                    sock.sendto(reply, addr)
        t = threading.Thread(target=run, daemon=True)
        t.start()
        return sock, stop, t

    def test_fetch_new_client_new_server(self):
        sock, stop, t = self.serve(self.lobby(), True)
        try:
            got = self.L.lobby_fetch(sock.getsockname(), "", 1.0)
        finally:
            stop.set(), t.join(2), sock.close()
        self.assertEqual({e["name"] for e in got}, {"Alice", "Bob", "Cy"})
        self.assertTrue(next(e for e in got if e["name"] == "Alice")["invaders"])

    def test_fetch_new_client_old_server_falls_back_to_v1(self):
        sock, stop, t = self.serve(self.lobby(), False)
        try:
            got = self.L.lobby_fetch(sock.getsockname(), "", 0.5)
        finally:
            stop.set(), t.join(2), sock.close()
        self.assertEqual({e["name"] for e in got}, {"Alice", "Bob", "Cy"})
        self.assertFalse(any(e.get("invaders") for e in got))       # an old lobby never offers an invasion
        rows = self.L.lobby_rows(got, "0.7.8")
        self.assertFalse(any(r["invadable"] for r in rows))

    def test_old_client_parser_still_reads_the_new_servers_v1_reply(self):
        raw = self.lobby().handle(nw.encode_list(0), "10.8.0.99", 1)
        self.assertEqual(raw[3], 3)
        self.assertEqual(len(self.L.lobby_parse_reply(raw)[2]), 3)

    def test_discovery_extension_merges_and_old_packets_still_parse(self):
        L = self.L
        announce = bytes.fromhex("fa410100" "0000002a" "245b" "000a" "01" "04") + bytes([5]) + b"0.7.8" + bytes([5]) + b"Alice" + bytes([0])
        ext = bytes([0xFA, 0x49, 1]) + (42).to_bytes(4, "big") + bytes([1]) + (0x1C010000).to_bytes(4, "big") + bytes([4, 1])
        self.assertEqual(len(ext), 14)
        book = L.DiscoveryBook()
        self.assertTrue(book.feed(announce, "192.168.1.5", 100.0))
        snap = book.snapshot(100.5)
        self.assertEqual(len(snap), 1)
        self.assertFalse(snap[0]["invaders"])                          # no extension yet
        self.assertTrue(book.feed(ext, "192.168.1.5", 101.0))
        row = book.snapshot(101.5)[0]
        self.assertEqual((row["invaders"], row["area_id"], row["phase"], row["ng"]), (True, 0x1C010000, 4, 1))
        rows = L.discovery_rows(book.snapshot(101.5), [], "0.7.8")
        self.assertTrue(rows[0]["invadable"])
        self.assertEqual(rows[0]["area"], "Yahar'gul (Blood Moon)")
        # the extension expires with its own freshness: a host that stops sending it is not invadable
        row = book.snapshot(101.0 + L.DISCOVERY_TTL_S + 1)
        self.assertEqual(row, [])
        # junk: wrong length / unknown flag / ext for an unknown announcement never makes an entry
        book2 = L.DiscoveryBook()
        self.assertTrue(book2.feed(ext, "192.168.1.6", 1.0))
        self.assertEqual(book2.snapshot(1.5), [])
        for bad in (ext[:-1], ext + b"\x00", ext[:7] + bytes([4]) + ext[8:], ext[:12] + bytes([5]) + ext[13:]):
            self.assertFalse(book2.feed(bad, "192.168.1.7", 2.0))
        self.assertIsNone(L.parse_discovery_packet(ext))              # an old listener: junk
        self.assertIsNone(L.parse_discovery_ext(announce))

    def test_host_toggle_exists_and_the_join_flag_is_launch_only(self):
        src = (ROOT / "launcher.py").read_text(encoding="utf-8")
        self.assertIn('update_ini(bbport_ini_path(), {"party_invaders"', src)        # the host's ini key
        self.assertNotIn('"party_invade"', src)                                       # joining as an invader is never saved
        self.assertIn('env["BB_PARTY_INVADE"] = "1"', src)                            # ... it is this launch's environment only


if __name__ == "__main__":
    unittest.main()
