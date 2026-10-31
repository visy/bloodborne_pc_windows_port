"""Tests for tools/vpn/nwsign. Needs the openssl CLI; real openvpn tests are skipped when absent.

    python -m unittest tests.test_nwsign        (from the repo root)
"""
import base64, hashlib, http.client, json, os, shutil, ssl, subprocess, sys, tempfile, threading, time, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NW = os.path.join(ROOT, "tools", "vpn", "nwsign")
sys.path.insert(0, NW)
sys.path.insert(0, os.path.join(ROOT, "tools", "vpn"))
import nwsign_core as core            # noqa: E402
import nwsign_service as svc          # noqa: E402
import nwvpn_client as cli            # noqa: E402

OPENSSL = shutil.which("openssl")
OPENVPN = shutil.which("openvpn") or (os.path.exists("/usr/sbin/openvpn") and "/usr/sbin/openvpn") or None

FAKE_OPENVPN = """import sys
a = sys.argv
out = a[a.index("tls-crypt-v2-client") + 1]
open(out, "w").write("-----BEGIN OpenVPN tls-crypt-v2 client key-----\\nFAKE " + a[-1] + "\\n-----END OpenVPN tls-crypt-v2 client key-----\\n")
"""


def ossl(*a, data=None):
    r = subprocess.run([OPENSSL] + list(a), input=data, capture_output=True)
    assert r.returncode == 0, r.stderr
    return r.stdout.decode()


def make_csr(d, name="k", subj="/CN=evil", alg="ec"):
    key = os.path.join(d, name + ".key")
    if alg == "ec":
        ossl("ecparam", "-genkey", "-name", "prime256v1", "-noout", "-out", key)
    else:
        ossl("genrsa", "-out", key, "2048")
    return key, ossl("req", "-new", "-key", key, "-subj", subj)


def sign_msg(key, msg):
    r = subprocess.run([OPENSSL, "dgst", "-sha256", "-sign", key], input=msg, capture_output=True)
    return base64.b64encode(r.stdout).decode()


@unittest.skipUnless(OPENSSL, "openssl CLI not found")
class Base(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.mkdtemp(prefix="nwsign-t-")
        self.addCleanup(shutil.rmtree, self.td, True)
        root_key, root_crt = os.path.join(self.td, "root.key"), os.path.join(self.td, "root.crt")
        ossl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "3650",
             "-subj", "/CN=testroot", "-keyout", root_key, "-out", root_crt)
        fake = os.path.join(self.td, "fakevpn.py")
        open(fake, "w").write(FAKE_OPENVPN)
        self.cfg = dict(core.DEFAULTS, state_dir=os.path.join(self.td, "state"), vpn_host="<vpn-host>",
                        openssl=OPENSSL, openvpn_cmd=[sys.executable, fake], listen_host="127.0.0.1", listen_port=0)
        self.cfg.update(self.tune())
        self.st = core.State(self.cfg)
        self.pin = self.st.init_keys()
        self.st.sign_subca(root_key, root_crt)
        shutil.copy(root_crt, self.st.p("root-ca.crt"))
        open(self.st.p("tc2-server.key"), "w").write("x")
        self.st.regen_crl()
        self.root_crt = root_crt

    def tune(self):
        return {"enroll_per_ip_day": 1000}

    def start(self):
        self.srv = svc.make_server(self.cfg)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.server = "127.0.0.1:%d" % self.port

    def post(self, path, obj=None, raw=None, headers=None):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        c = http.client.HTTPSConnection("127.0.0.1", self.port, context=ctx, timeout=20)
        body = raw if raw is not None else json.dumps(obj).encode()
        c.request("POST", path, body, headers if headers is not None else {"Content-Type": "application/json"})
        r = c.getresponse()
        data = r.read()
        hdr = dict(r.getheaders())
        c.close()
        try:
            return r.status, json.loads(data), hdr
        except ValueError:
            return r.status, None, hdr

    def enroll(self, subj="/CN=evil"):
        key, csr = make_csr(self.td, "k%d" % time.time_ns(), subj)
        st, js, _ = self.post("/v1/enroll", {"csr": csr})
        self.assertEqual(st, 200, js)
        return key, js

    def cert_info(self, pem):
        return ossl("x509", "-noout", "-subject", "-dates", "-ext", "extendedKeyUsage,basicConstraints", data=pem.encode())

    def renew_req(self, key, js, ts=None, sign_key=None, install_id=None):
        nkey, csr = make_csr(self.td, "n%d" % time.time_ns())
        ts = ts or int(time.time())
        iid = install_id or js["install_id"]
        sig = sign_msg(sign_key or key, core.State.renew_message(iid, csr, ts))
        return nkey, {"install_id": iid, "csr": csr, "timestamp": ts, "signature": sig, "cert": js["cert"]}


class TestEnroll(Base):
    def test_cn_override_validity_eku(self):
        self.start()
        key, js = self.enroll("/CN=admin/O=evil")
        self.assertRegex(js["install_id"], r"^[0-9a-f]{32}$")
        info = self.cert_info(js["cert"])
        self.assertNotIn("evil", info)
        self.assertIn("TLS Web Client Authentication", info)
        self.assertIn("CA:FALSE", info)
        self.assertAlmostEqual(js["expires"], time.time() + 7 * 86400, delta=60)
        self.assertEqual(js["renew_after"], js["expires"] - 3 * 86400)
        self.assertIn(js["install_id"], info)
        for k in ("cert", "ca_bundle", "tls_crypt_v2_client_key", "profile"):
            self.assertTrue(js[k])
        self.assertIn("remote <vpn-host> 1194", js["profile"])
        self.assertNotIn("PRIVATE KEY", json.dumps(js))
        # metadata carried in the wrapped key: install id + expiry
        meta = js["tls_crypt_v2_client_key"].split("FAKE ")[1].split("\n")[0]
        self.assertEqual(base64.b64decode(meta).decode(), "%s %d" % (js["install_id"], js["expires"]))
        # client trusts only the root, never the sub-CA
        self.assertEqual(js["ca_bundle"], open(self.root_crt).read())

    def test_cert_chain_verifies(self):
        self.start()
        _, js = self.enroll()
        crt = os.path.join(self.td, "c.pem")
        open(crt, "w").write(js["cert"])
        bundle = os.path.join(self.td, "bundle.pem")
        open(bundle, "w").write(open(self.root_crt).read() + open(self.st.p("subca.crt")).read())
        out = ossl("verify", "-CAfile", bundle, "-purpose", "sslclient", crt)
        self.assertIn("OK", out)

    def test_bad_csrs(self):
        self.start()
        _, rsa = make_csr(self.td, "rsa", alg="rsa")
        for csr in (rsa, "garbage", "", None, 5, "-----BEGIN CERTIFICATE REQUEST-----\nAAAA\n-----END CERTIFICATE REQUEST-----",
                    "x" * 3000):
            st, js, _ = self.post("/v1/enroll", {"csr": csr})
            self.assertEqual(st, 400, (csr, js))
        _, good = make_csr(self.td, "ok")
        lines = good.splitlines()
        lines[3] = lines[3][:-4] + ("AAAA" if lines[3][-4:] != "AAAA" else "BBBB")  # corrupt signature/body
        st, _, _ = self.post("/v1/enroll", {"csr": "\n".join(lines) + "\n"})
        self.assertEqual(st, 400)

    def test_body_limits(self):
        self.start()
        st, _, _ = self.post("/v1/enroll", raw=b'{"csr":"' + b"A" * 9000 + b'"}')
        self.assertEqual(st, 413)
        st, _, _ = self.post("/v1/enroll", raw=b"not json")
        self.assertEqual(st, 400)
        st, _, _ = self.post("/v1/enroll", raw=b"[1,2]")
        self.assertEqual(st, 400)
        st, _, _ = self.post("/v1/nope", {})
        self.assertEqual(st, 404)

    def test_pin(self):
        self.start()
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        with ctx.wrap_socket(__import__("socket").create_connection(("127.0.0.1", self.port))) as s:
            self.assertEqual(cli.spki_sha256(s.getpeercert(binary_form=True)), self.pin)
        with self.assertRaises(cli.ClientError):
            cli.call(self.server, "00" * 32, "/v1/enroll", {"csr": "x"})
        with self.assertRaises(cli.ClientError) as e:
            cli.call(self.server, self.pin, "/v1/enroll", {"csr": "x"})
        self.assertEqual(e.exception.status, 400)  # got past the pin check


class TestLimits(Base):
    def tune(self):
        return {"enroll_per_ip_day": 3, "enroll_global_day": 200, "renew_per_install_day": 2}

    def test_per_ip_enroll(self):
        self.start()
        for _ in range(3):
            self.enroll()
        _, csr = make_csr(self.td, "x")
        st, js, hdr = self.post("/v1/enroll", {"csr": csr})
        self.assertEqual(st, 429)
        self.assertGreater(int(hdr["Retry-After"]), 0)

    def test_renew_limit(self):
        self.start()
        key, js = self.enroll()
        codes = []
        for _ in range(3):
            _, req = self.renew_req(key, js)
            codes.append(self.post("/v1/renew", req)[0])
        self.assertEqual(codes, [200, 200, 429])

    def test_limiter_memory_only(self):
        a, b = core.RateLimiter(), core.RateLimiter()
        self.assertNotEqual(a.key("e", "1.2.3.4"), b.key("e", "1.2.3.4"))   # salt per process
        self.assertEqual(a.key("e", "2001:db8:1:2:3:4:5:6"), a.key("e", "2001:db8:1:2:ffff::1"))  # /64
        self.assertEqual(a.hit("k", 1, 10, now=100), 0)
        self.assertGreater(a.hit("k", 1, 10, now=101), 0)
        self.assertEqual(a.hit("k", 1, 10, now=111), 0)   # window expired

    def test_no_ip_on_disk(self):
        self.start()
        self.enroll()
        _, csr = make_csr(self.td, "x2")
        for _ in range(4):
            self.post("/v1/enroll", {"csr": csr})
        hits = []
        for dp, _, fs in os.walk(self.cfg["state_dir"]):
            for f in fs:
                if b"127.0.0.1" in open(os.path.join(dp, f), "rb").read():
                    hits.append(f)
        self.assertEqual(hits, [])


class TestGlobalLimit(Base):
    def tune(self):
        return {"enroll_per_ip_day": 100, "enroll_global_day": 2}

    def test_global(self):
        self.start()
        self.enroll()
        self.enroll()
        _, csr = make_csr(self.td, "g")
        self.assertEqual(self.post("/v1/enroll", {"csr": csr})[0], 429)


class TestRenew(Base):
    def test_renew_ok_same_id(self):
        self.start()
        key, js = self.enroll()
        nkey, req = self.renew_req(key, js)
        st, js2, _ = self.post("/v1/renew", req)
        self.assertEqual(st, 200, js2)
        self.assertEqual(js2["install_id"], js["install_id"])
        self.assertNotEqual(js2["cert"], js["cert"])
        self.assertIn(js["install_id"], self.cert_info(js2["cert"]))
        # the new cert carries the NEW key
        pk = lambda c: ossl("x509", "-noout", "-pubkey", data=c.encode())
        self.assertEqual(pk(js2["cert"]), ossl("pkey", "-in", nkey, "-pubout"))
        # old cert still valid until it expires (both serials tracked)
        self.assertEqual(len(self.st.load_db()["installs"][js["install_id"]]["s"]), 2)

    def test_renew_rejections(self):
        self.start()
        key, js = self.enroll()
        other, js_other = self.enroll()
        _, req = self.renew_req(key, js, sign_key=other)             # signed with someone else's key
        self.assertEqual(self.post("/v1/renew", req)[0], 401)
        _, req = self.renew_req(key, js, ts=int(time.time()) - 1000)  # stale timestamp
        self.assertEqual(self.post("/v1/renew", req)[0], 401)
        _, req = self.renew_req(key, js)
        req["csr"] = make_csr(self.td, "swap")[1]                     # csr swapped after signing
        self.assertEqual(self.post("/v1/renew", req)[0], 401)
        _, req = self.renew_req(key, js, install_id=js_other["install_id"])  # cert/id mismatch
        self.assertEqual(self.post("/v1/renew", req)[0], 401)
        _, req = self.renew_req(key, js)
        req["cert"] = "nope"
        self.assertEqual(self.post("/v1/renew", req)[0], 401)
        self.assertEqual(self.post("/v1/renew", {"install_id": "zz"})[0], 400)

    def test_foreign_ca_cert_rejected(self):
        self.start()
        key, js = self.enroll()
        # cert for the right CN but signed by a different CA
        fk = os.path.join(self.td, "fk.key")
        fc = os.path.join(self.td, "fc.crt")
        ossl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "5",
             "-subj", "/CN=" + js["install_id"], "-keyout", fk, "-out", fc)
        _, req = self.renew_req(fk, js)
        req["cert"] = open(fc).read()
        self.assertEqual(self.post("/v1/renew", req)[0], 401)

    def test_grace_window(self):
        self.start()
        key, js = self.enroll()
        d = 86400
        nk, csr = make_csr(self.td, "gr")
        now = js["expires"] + 2 * d                    # expired 2 days ago: inside the 3 day grace
        sig = sign_msg(key, core.State.renew_message(js["install_id"], csr, now))
        r = self.st.renew(js["install_id"], csr, now, sig, js["cert"], now=now)
        self.assertEqual(r["install_id"], js["install_id"])
        now = js["expires"] + 4 * d                    # beyond grace
        sig = sign_msg(key, core.State.renew_message(js["install_id"], csr, now))
        with self.assertRaises(core.CAError) as e:
            self.st.renew(js["install_id"], csr, now, sig, js["cert"], now=now)
        self.assertEqual(e.exception.status, 401)

    def test_banned_gets_403(self):
        self.start()
        key, js = self.enroll()
        self.st.set_ban(js["install_id"], True)
        _, req = self.renew_req(key, js)
        self.assertEqual(self.post("/v1/renew", req)[0], 403)
        self.st.set_ban(js["install_id"], False)
        _, req = self.renew_req(key, js)
        self.assertEqual(self.post("/v1/renew", req)[0], 200)


class TestBan(Base):
    def verify_script(self, install_id, expiry):
        mf = os.path.join(self.td, "meta")
        open(mf, "w").write("%s %d" % (install_id, expiry))
        env = dict(os.environ, metadata_type="0", metadata_file=mf, NWSIGN_STATE=self.cfg["state_dir"])
        return subprocess.run([sys.executable, os.path.join(NW, "nwsign-verify")], env=env).returncode

    def chain_check(self, cert):
        bundle = os.path.join(self.td, "bundle.pem")
        open(bundle, "w").write(open(self.root_crt).read() + open(self.st.p("subca.crt")).read())
        crt = os.path.join(self.td, "c.pem")
        open(crt, "w").write(cert)
        r = subprocess.run([OPENSSL, "verify", "-crl_check_all", "-CAfile", bundle,
                            "-CRLfile", self.st.pubp("crl-bundle.pem"), "-purpose", "sslclient", crt],
                           capture_output=True)
        return r.returncode == 0

    def test_ban_crl_and_script(self):
        self.start()
        # root CRL stand-in so the bundle holds two CRLs like production
        _, js = self.enroll()
        _, js2 = self.enroll()
        iid = js["install_id"]
        self.assertEqual(self.verify_script(iid, js["expires"]), 0)
        # the root CRL is missing in this fixture, so check only the sub-CA CRL entry for revocation
        subcrl = self.st.p("subca.crl")
        self.st.set_ban(iid, True)
        txt = ossl("crl", "-in", subcrl, "-noout", "-text")
        ser = [s for s, _ in self.st.load_db()["installs"][iid]["s"]][0].upper()
        self.assertIn(ser, txt)
        ser2 = [s for s, _ in self.st.load_db()["installs"][js2["install_id"]]["s"]][0].upper()
        self.assertNotIn(ser2, txt)
        self.assertEqual(open(self.st.pubp("banned.txt")).read().split(), [iid])
        self.assertEqual(self.verify_script(iid, js["expires"]), 1)
        self.assertEqual(self.verify_script(js2["install_id"], js2["expires"]), 0)
        self.assertEqual(self.verify_script(js2["install_id"], int(time.time()) - 5), 1)   # expired metadata
        self.assertEqual(self.verify_script("zz", 99999999999), 1)
        self.st.set_ban(iid, False)
        self.assertNotIn(ser, ossl("crl", "-in", subcrl, "-noout", "-text"))
        self.assertEqual(self.verify_script(iid, js["expires"]), 0)

    def test_crl_signed_by_subca_and_openssl_revocation(self):
        self.start()
        _, js = self.enroll()
        self.st.set_ban(js["install_id"], True)
        # openssl's own check: revoked cert fails -crl_check against the sub-CA CRL
        bundle = os.path.join(self.td, "b.pem")
        open(bundle, "w").write(open(self.root_crt).read() + open(self.st.p("subca.crt")).read())
        crt = os.path.join(self.td, "c.pem")
        open(crt, "w").write(js["cert"])
        r = subprocess.run([OPENSSL, "verify", "-crl_check", "-CAfile", bundle, "-CRLfile", self.st.p("subca.crl"), crt],
                           capture_output=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"revoked", r.stderr + r.stdout)
        self.st.set_ban(js["install_id"], False)
        r = subprocess.run([OPENSSL, "verify", "-crl_check", "-CAfile", bundle, "-CRLfile", self.st.p("subca.crl"), crt],
                           capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_admin_cli_and_stats(self):
        self.start()
        _, js = self.enroll()
        cfgf = os.path.join(self.td, "c.json")
        json.dump(self.cfg, open(cfgf, "w"))
        run = lambda *a: subprocess.run([sys.executable, os.path.join(NW, "nwsign-admin"), "--config", cfgf] + list(a),
                                        capture_output=True, text=True)
        r = run("stats")
        self.assertIn("installs", r.stdout)
        self.assertEqual(self.st.stats()["installs"], 1)
        self.assertEqual(self.st.stats()["issued_last_24h"], 1)
        self.assertEqual(run("ban", js["install_id"]).returncode, 0)
        out = run("list").stdout
        self.assertIn("count: 1", out)
        self.assertIn("BANNED", out)
        self.assertNotEqual(run("ban", "0" * 32).returncode, 0)   # unknown id
        self.assertNotEqual(run("ban", "bad").returncode, 0)
        self.assertEqual(run("unban", js["install_id"]).returncode, 0)

    def test_gc_forgets_old_but_keeps_banned(self):
        self.start()
        _, a = self.enroll()
        _, b = self.enroll()
        self.st.set_ban(b["install_id"], True)
        db = self.st.load_db()
        for r in db["installs"].values():
            r["exp"] = int(time.time()) - 40 * 86400
        self.st.gc(db)
        self.assertEqual(list(db["installs"]), [b["install_id"]])


class TestClientRoundTrip(Base):
    def test_client_enroll_renew(self):
        self.start()
        d = os.path.join(self.td, "client")
        path = cli.enroll(d, self.server, self.pin)
        txt = open(path).read()
        for tag in ("<ca>", "<cert>", "<key>", "<tls-crypt-v2>"):
            self.assertIn(tag, txt)
        self.assertIn("remote-cert-tls server", txt)
        info = json.load(open(os.path.join(d, "install.json")))
        self.assertNotIn("PRIVATE KEY", json.dumps(info))
        self.assertIsNone(cli.renew(d, self.server, self.pin))      # not due yet
        old_key = open(os.path.join(d, "client.key")).read()
        cli.renew(d, self.server, self.pin, force=True)
        self.assertEqual(json.load(open(os.path.join(d, "install.json")))["install_id"], info["install_id"])
        self.assertNotEqual(open(os.path.join(d, "client.key")).read(), old_key)
        with self.assertRaises(cli.ClientError):
            cli.renew(d, self.server, "ab" * 32, force=True)


@unittest.skipUnless(OPENVPN, "openvpn binary not available")
class TestWithOpenvpn(Base):
    def tune(self):
        return {"openvpn_cmd": [OPENVPN]}

    def test_real_tls_crypt_v2_key_and_metadata(self):
        subprocess.run([OPENVPN, "--genkey", "tls-crypt-v2-server", self.st.p("tc2-server.key")], check=True,
                       capture_output=True)
        self.start()
        _, js = self.enroll()
        self.assertIn("OpenVPN tls-crypt-v2 client key", js["tls_crypt_v2_client_key"])


if __name__ == "__main__":
    unittest.main()


class TestFailedEnrollFree(Base):
    def tune(self):
        return {"enroll_per_ip_day": 2, "enroll_global_day": 200}

    def test_failed_enrollments_do_not_count(self):
        self.start()
        for _ in range(5):   # bad CSRs: refused, but no day-long lockout
            self.assertNotEqual(self.post("/v1/enroll", {"csr": "not a csr"})[0], 429)
        self.enroll()
        self.enroll()
        _, csr = make_csr(self.td, "f")
        self.assertEqual(self.post("/v1/enroll", {"csr": csr})[0], 429)   # two successes use the two slots

    def test_peek_counts_nothing(self):
        lim = core.RateLimiter()
        for _ in range(5):
            self.assertEqual(lim.peek("k", 1, 10, now=100), 0)
        self.assertEqual(lim.hit("k", 1, 10, now=100), 0)
        self.assertGreater(lim.peek("k", 1, 10, now=101), 0)
