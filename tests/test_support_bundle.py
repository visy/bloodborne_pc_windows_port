"""scripts/support_bundle.py (redaction, bundle contents) and launcher.py's party logging env
(docs/party/logging.md)."""
from paths import ROOT
import importlib.util
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(ROOT / "scripts"))
import support_bundle as sb  # noqa: E402


class Redaction(unittest.TestCase):
    def test_party_codes(self):
        t = sb.redact_text("join with BBP1-ABCD-EFGH-1234 or bbp2-zz9-x now")
        self.assertNotIn("ABCD", t)
        self.assertNotIn("zz9", t)
        self.assertEqual(t.count("<party code>"), 2)
        t = sb.redact_text("code BBP3-NJR0-E43N-WWN9-2C2P, guest code BBG1-0ZR0-000V-KWFP")
        self.assertNotIn("NJR0", t)
        self.assertNotIn("0ZR0", t)

    def test_passwords_and_tokens(self):
        for text, secret in (("password=hunter2", "hunter2"), ("party_password = s3cret!", "s3cret!"),
                             ('"party_password": "pw with"', "pw"), ("BB_PARTY_PASSWORD=abc123", "abc123"),
                             ("token: 0123456789abcdef0123456789abcdef", "0123456789abcdef"),
                             ("set BB_PARTY_CODE=BBP2-AAAA-BBBB", "AAAA"), ("guest_code=XYZ123", "XYZ123"),
                             ('{"secret":"q1w2e3"}', "q1w2e3")):
            out = sb.redact_text(text)
            self.assertNotIn(secret, out, text)
            self.assertTrue("<redacted>" in out or "<party code>" in out, out)

    def test_explicit_secrets(self):
        out = sb.redact_text("the password is correcthorse in plain text", secrets=["correcthorse", "", "ab"])
        self.assertNotIn("correcthorse", out)
        self.assertIn("plain text", out)

    def test_ipv4(self):
        r = sb.Redactor()
        t = r.text("the guest joined (slot 1, from 203.0.113.7:62793); again 203.0.113.7:1000; lan 192.168.1.20:9307")
        self.assertNotIn("203.0.113.7", t)
        self.assertNotIn("192.168.1.20", t)
        self.assertIn("<ip-1>:62793", t)
        self.assertIn("<ip-1>:1000", t)  # the same address keeps its name
        self.assertIn("<lan-ip-2>:9307", t)
        # Loopback and the any address stay (no one's address); versions and times are no addresses.
        keep = "bind 127.0.0.1:47000 and 0.0.0.0; build 0.6.1 (0.51-1027-g8143fda47b); at 19:49:49.123; os 10.0.26200"
        self.assertEqual(sb.redact_text(keep), keep)

    def test_ipv6(self):
        t = sb.redact_text("peer [2001:db8:85a3::8a2e:370:7334]:9307 and fe80::1ff:fe23:4567:890a")
        self.assertNotIn("2001:db8", t)
        self.assertNotIn("fe80::", t)
        keep = "[19:49:49.123] Party: x; coop::plog::Tag; ratio 1:2:3"
        self.assertEqual(sb.redact_text(keep), keep)

    def test_user_paths_and_email(self):
        t = sb.redact_text(r"C:\Users\vp797\AppData\x and C:/Users/Some Body/y, mail me@example.com")
        self.assertNotIn("vp797", t)
        self.assertNotIn("example.com", t)
        self.assertIn(r"C:\Users\<user>\AppData", t)

    def test_crash_log_values_kept(self):
        keep = "Faulting RIP 0x802199310 (guest +0x2199310), RAX=0000000000000000, Party: world: enemy drop roll"
        self.assertEqual(sb.redact_text(keep), keep)

    def test_settings(self):
        s = sb.redact_settings({"party_password": "pw", "party_code": "BBP1-X", "party_name": "Hunter0",
                                "party_public_addr": "", "fps": "60"})
        self.assertEqual(s["party_password"], "<redacted>")
        self.assertEqual(s["party_code"], "<redacted>")
        self.assertEqual(s["party_public_addr"], "")
        self.assertEqual(s["party_name"], "Hunter0")
        self.assertEqual(s["fps"], "60")


class Bundle(unittest.TestCase):
    def test_bundle_contents(self):
        with tempfile.TemporaryDirectory() as d:
            app = Path(d)
            (app / "logs").mkdir()
            (app / "out").mkdir()
            (app / "user" / "savedata").mkdir(parents=True)
            (app / "user" / "savedata" / "userdata0000").write_bytes(b"save")
            (app / "eboot.bin").write_bytes(b"\x7fELF")
            (app / "launcher.log").write_text("[coop] Party: build 0.6.1 (abc)\nPassword=topsecret from 198.51.100.9:1\n",
                                              encoding="utf-8")
            for i in range(14):
                (app / "logs" / f"launcher_20261013_1800{i:02d}.log").write_text(f"log {i}\n", encoding="utf-8")
            (app / "out" / "crash_20261013_203810.log").write_text("Faulting RIP 0x1\n", encoding="utf-8")
            (app / "out" / "summary.log").write_text("summary\n", encoding="utf-8")
            (app / "out" / "shader_cache.bin").write_bytes(b"x")
            (app / "bbport.ini").write_text("[party]\nparty_password=pw2\nshow_hud=1\n", encoding="utf-8")
            settings = {"party_password": "mypass", "party_code": "BBP2-QQQQ-RRRR", "party_mode": "join",
                        "party_log_level": "verbose", "party_log_areas": ["net"]}
            (app / "logs" / "launcher_20261013_180013.log").write_text(
                "guest: code BBP2-QQQQ-RRRR pw mypass\n", encoding="utf-8")
            out = app / "logs" / "support_test.zip"
            names = sb.make_bundle(app, out, max_logs=10, settings=settings, note="test", version="0.6.1")
            self.assertTrue(out.is_file())
            with zipfile.ZipFile(out) as z:
                listed = set(z.namelist())
                self.assertEqual(listed, set(names))
                self.assertIn("launcher.log", listed)
                self.assertIn("out/crash_20261013_203810.log", listed)
                self.assertIn("out/summary.log", listed)
                self.assertIn("bbport.ini", listed)
                self.assertIn("info.txt", listed)
                self.assertIn("launcher_settings.json", listed)
                self.assertEqual(len([n for n in listed if n.startswith("logs/launcher_")]), 10)
                self.assertFalse(any("save" in n or "eboot" in n or "shader" in n for n in listed), listed)
                blob = "".join(z.read(n).decode("utf-8") for n in listed)
                for secret in ("topsecret", "mypass", "QQQQ", "pw2", "198.51.100.9"):
                    self.assertNotIn(secret, blob)
                info = z.read("info.txt").decode("utf-8")
                self.assertIn("launcher version: 0.6.1", info)
                self.assertIn("build: 0.6.1 (abc)", info)
                self.assertIn("party logging: verbose", info)
                st = json.loads(z.read("launcher_settings.json").decode("utf-8"))
                self.assertEqual(st["party_password"], "<redacted>")

    def test_gpu_device_lost_report(self):
        # A device lost leaves out\device_lost_*.log and the stuck shaders' code (vk_breadcrumbs.cpp):
        # both go into the bundle, the code byte for byte.
        with tempfile.TemporaryDirectory() as d:
            app = Path(d)
            (app / "out").mkdir()
            (app / "out" / "device_lost_20261014_120000.log").write_text(
                "GPU device lost (submit)\n    #12 STUCK: dispatch cs 00000000deadbeef\n", encoding="utf-8")
            code = bytes(range(256)) * 300  # 75 KiB: kept to 64 KiB
            (app / "out" / "hang_shader_00000000deadbeef.bin").write_bytes(code)
            spv = b"\x03\x02\x23\x07" + bytes(range(256)) * 400  # ~100 KiB of SPIR-V: kept whole
            (app / "out" / "hang_shader_00000000deadbeef_0.spv").write_bytes(spv)
            (app / "out" / "shader_cache.bin").write_bytes(b"x")
            out = app / "support.zip"
            sb.make_bundle(app, out)
            with zipfile.ZipFile(out) as z:
                listed = set(z.namelist())
                self.assertIn("out/device_lost_20261014_120000.log", listed)
                self.assertEqual(z.read("out/hang_shader_00000000deadbeef.bin"), code[:64 * 1024])
                self.assertEqual(z.read("out/hang_shader_00000000deadbeef_0.spv"), spv)
                self.assertNotIn("out/shader_cache.bin", listed)
                self.assertIn("STUCK", z.read("out/device_lost_20261014_120000.log").decode("utf-8"))


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_log", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class LauncherLogEnv(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def test_default_normal(self):
        env = self.L.party_log_env({})
        self.assertIsNone(env["BB_PARTY_LOG"])
        self.assertIsNone(env["BB_PARTY_LOG_AREAS"])
        self.assertIsNone(self.L.settings_env({})["BB_PARTY_LOG"])

    def test_levels(self):
        self.assertEqual(self.L.party_log_env({"party_log_level": "verbose"})["BB_PARTY_LOG"], "verbose")
        self.assertEqual(self.L.party_log_env({"party_log_level": "trace"})["BB_PARTY_LOG"], "trace")
        self.assertIsNone(self.L.party_log_env({"party_log_level": "bogus"})["BB_PARTY_LOG"])

    def test_areas(self):
        all_keys = [k for k, _ in self.L.PARTY_LOG_AREAS]
        env = self.L.party_log_env({"party_log_level": "verbose", "party_log_areas": all_keys})
        self.assertIsNone(env["BB_PARTY_LOG_AREAS"])  # all = unset
        env = self.L.party_log_env({"party_log_level": "verbose", "party_log_areas": ["items", "net"]})
        self.assertEqual(env["BB_PARTY_LOG_AREAS"], "net,items")
        env = self.L.party_log_env({"party_log_level": "verbose", "party_log_areas": []})
        self.assertEqual(env["BB_PARTY_LOG_AREAS"], "none")
        env = self.L.party_log_env({"party_log_level": "", "party_log_areas": ["net"]})
        self.assertIsNone(env["BB_PARTY_LOG_AREAS"])  # Normal: no areas


if __name__ == "__main__":
    unittest.main()
