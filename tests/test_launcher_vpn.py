"""launcher.py host over a VPN (0.7.5): the adapter choice (same rules as the game's
vpn_adapter_score), the join box's port suffix and the host's copied address."""
from paths import ROOT
import importlib.util
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_vpn", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


LAN = {"name": "Ethernet / Realtek PCIe 2.5GbE Family Controller", "ip": "192.168.1.20"}
LAN2 = {"name": "Ethernet 2 / Intel(R) Ethernet I219-V", "ip": "192.168.1.20"}  # same address (samelanip)
RADMIN = {"name": "Radmin VPN / Famatech Radmin VPN Ethernet Adapter", "ip": "26.41.7.9"}
HAMACHI = {"name": "Hamachi / LogMeIn Hamachi Virtual Ethernet Adapter", "ip": "25.4.5.6"}
TAIL = {"name": "Tailscale / Tailscale Tunnel", "ip": "100.101.102.103"}
ZT = {"name": "ZeroTier One [8056c2e21c000001] / ZeroTier Virtual Port", "ip": "10.147.17.5"}
NORD = {"name": "NordLynx / NordLynx Tunnel", "ip": "10.5.0.2"}


class VpnAdapters(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_kind(self):
        k = self.L.vpn_kind
        self.assertEqual(k(RADMIN["name"], RADMIN["ip"]), "Radmin VPN")
        self.assertEqual(k(HAMACHI["name"], HAMACHI["ip"]), "Hamachi")
        self.assertEqual(k(TAIL["name"], TAIL["ip"]), "Tailscale")
        self.assertEqual(k(ZT["name"], ZT["ip"]), "ZeroTier")
        self.assertEqual(k(NORD["name"], NORD["ip"]), "other")
        self.assertEqual(k(LAN["name"], LAN["ip"]), "")
        # an unnamed virtual adapter by its range, a real adapter never
        self.assertEqual(k("Ethernet 5 / Virtual Adapter", "26.1.2.3"), "Radmin VPN")
        self.assertEqual(k("Ethernet 5 / Virtual Adapter", "100.64.0.9"), "Tailscale")
        self.assertEqual(k("Ethernet 5 / Virtual Adapter", "100.128.0.9"), "")
        self.assertEqual(k("Ethernet / Realtek", "26.1.2.3"), "")

    def test_pref(self):
        n = self.L.vpn_pref_normalize
        self.assertEqual((n(""), n(None), n(" AUTO ")), ("auto", "auto", "auto"))
        self.assertEqual((n("Off"), n("0")), ("off", "off"))
        self.assertEqual(n(" Radmin VPN "), "Radmin VPN")

    def test_select(self):
        sel = self.L.select_vpn_adapter
        self.assertIs(sel([LAN, RADMIN, NORD], "auto"), RADMIN)  # a commercial VPN is never taken unasked
        self.assertIs(sel([LAN, RADMIN, NORD], ""), RADMIN)
        self.assertIsNone(sel([LAN, RADMIN, NORD], "off"))
        self.assertIsNone(sel([LAN, NORD], "auto"))
        self.assertIsNone(sel([LAN], "auto"))
        self.assertIs(sel([LAN, RADMIN, NORD], "radmin"), RADMIN)
        self.assertIs(sel([LAN, RADMIN, NORD], "NORDLYNX"), NORD)
        self.assertIsNone(sel([LAN, RADMIN, NORD], "zerotier"))  # not up: the usual address
        self.assertIs(sel([LAN, HAMACHI, TAIL, RADMIN], "auto"), RADMIN)  # Radmin > Hamachi > Tailscale > ZeroTier
        self.assertIs(sel([LAN, ZT, TAIL], "auto"), TAIL)
        self.assertIsNone(sel([{"name": "Radmin VPN", "ip": "169.254.1.1"}], "auto"))
        # two homes with the same router address: the VPN address tells them apart
        self.assertIs(sel([LAN2, RADMIN], "auto"), RADMIN)

    def test_label(self):
        self.assertEqual(self.L.vpn_adapter_label(RADMIN), "Radmin VPN")
        self.assertEqual(self.L.vpn_adapter_label(NORD), "NordLynx")
        self.assertEqual(self.L.vpn_choice_label("auto"), "Auto")
        self.assertEqual(self.L.vpn_choice_label("My Adapter"), "My Adapter")
        self.assertEqual(self.L.vpn_choice_pref("Off"), "off")


class PortSuffix(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_bare_ip_gets_the_port(self):
        w = self.L.with_party_port
        self.assertEqual(w("26.41.7.9"), "26.41.7.9:9307")
        self.assertEqual(w("  26.41.7.9 "), "26.41.7.9:9307")
        self.assertEqual(w("26.41.7.9", 9400), "26.41.7.9:9400")
        self.assertEqual(w("my.host.example"), "my.host.example:9307")

    def test_explicit_port_wins(self):
        w = self.L.with_party_port
        self.assertEqual(w("26.41.7.9:9500"), "26.41.7.9:9500")
        self.assertEqual(w("[::1]:9307"), "[::1]:9307")

    def test_codes_and_junk_are_left_alone(self):
        w = self.L.with_party_port
        for text in ("", "BBP3-ABCD-EFGH", "bbp3-abcd", "hello", "300.1.1.1", "1.2.3", "what is this"):
            self.assertEqual(w(text), text.strip())

    def test_bare_address_is_a_valid_join_address(self):
        self.assertEqual(self.L.party_code_problem("26.41.7.9"), "")
        self.assertEqual(self.L.party_code_problem("26.41.7.9:9500"), "")
        self.assertNotEqual(self.L.party_code_problem("hello"), "")

    def test_join_env_carries_the_port(self):
        env = self.L.party_env({"party_mode": "join", "party_code": "26.41.7.9"})
        self.assertEqual(env["BB_PARTY_CODE"], "26.41.7.9:9307")
        env = self.L.party_env({"party_mode": "join", "party_code": "26.41.7.9:9500"})
        self.assertEqual(env["BB_PARTY_CODE"], "26.41.7.9:9500")
        self.assertIsNone(self.L.party_env({"party_mode": "host"})["BB_PARTY_VPN"])

    def test_copied_host_address_has_the_host_port(self):
        h = self.L.host_address_text
        self.assertEqual(h("26.41.7.9", "9307"), "26.41.7.9:9307")
        self.assertEqual(h("26.41.7.9", "9400"), "26.41.7.9:9400")
        self.assertEqual(h("26.41.7.9", ""), "26.41.7.9:9307")
        self.assertEqual(h("26.41.7.9", "abc"), "26.41.7.9:9307")

    def test_offline_vpn_is_not_offered(self):
        """0.7.6: Windows keeps an overlay VPN's adapter Up with its address while the VPN app is off."""
        sel = self.L.select_vpn_adapter
        still = [1380, 23, 0]  # measured on an offline Radmin: nothing arrives
        off_radmin = dict(RADMIN, vendor="offline", rx1=still, rx2=still)
        on_radmin = dict(RADMIN, vendor="online")
        self.assertIsNone(sel([LAN, off_radmin], "auto"))
        self.assertIsNone(sel([LAN, off_radmin], "radmin"))
        self.assertIs(sel([LAN, on_radmin], "auto"), on_radmin)
        # no vendor status: the counters decide
        quiet = dict(HAMACHI, vendor="", rx1=still, rx2=still)
        busy = dict(HAMACHI, vendor="", rx1=still, rx2=[1500, 24, 0])
        self.assertIsNone(sel([LAN, quiet], "auto"))
        self.assertIs(sel([LAN, busy], "auto"), busy)
        self.assertIs(sel([LAN, quiet], "hamachi"), quiet)  # a named adapter is trusted on quiet counters
        self.assertIs(sel([LAN, HAMACHI], "auto"), HAMACHI)  # no evidence at all: trusted
        self.assertFalse(self.L.vpn_adapter_offline(LAN))
        self.assertTrue(self.L.vpn_adapter_offline(off_radmin))

    def test_vendor_outputs(self):
        L = self.L
        log = ("2026.10.23 19:58:26.453\tINFO\tConnected to server (17)\n"
               "2026.10.23 19:59:19.382\tINFO\tConnected to 154680149/'x' via TcpRelay/outgoing\n"
               "2026.10.23 21:37:57.218\tINFO\tSwitched Off\n"
               "2026.10.23 21:37:57.219\tINFO\tDisconnected from server(code:4)\n")
        self.assertEqual(L.vpn_state_from_radmin_log(log), "offline")
        self.assertEqual(L.vpn_state_from_radmin_log(log + "t\tINFO\tConnected to server (3)\n"), "online")
        self.assertEqual(L.vpn_state_from_radmin_log("t\tINFO\tConnected to 1/'x' via TcpRelay\n"), "")
        self.assertEqual(L.vpn_state_from_radmin_log(""), "")
        self.assertEqual(L.vpn_state_from_tailscale_json('{"BackendState": "Running"}'), "online")
        self.assertEqual(L.vpn_state_from_tailscale_json('{"BackendState":"Stopped"}'), "offline")
        self.assertEqual(L.vpn_state_from_tailscale_json('{"BackendState":"NeedsLogin"}'), "offline")
        self.assertEqual(L.vpn_state_from_tailscale_json('{"BackendState":"Starting"}'), "")
        self.assertEqual(L.vpn_state_from_tailscale_json("not json"), "")
        self.assertEqual(L.vpn_state_from_zerotier_info("200 info 8056c2e21c 1.12.2 ONLINE"), "online")
        self.assertEqual(L.vpn_state_from_zerotier_info("200 info 8056c2e21c 1.12.2 OFFLINE"), "offline")
        self.assertEqual(L.vpn_state_from_zerotier_info("missing authtoken"), "")
        d = L.vpn_decide_online
        self.assertTrue(d("online", [1, 1, 0], [1, 1, 0]))
        self.assertFalse(d("offline", [1, 1, 0], [9, 9, 9]))
        self.assertFalse(d("", [1, 1, 0], [1, 1, 0]))
        self.assertTrue(d("", [1, 1, 0], [1, 1, 1]))
        self.assertTrue(d("", None, None))
        self.assertTrue(d("", [1, 1, 0], [1, 1, 0], counters_trusted=False))


if __name__ == "__main__":
    unittest.main()
