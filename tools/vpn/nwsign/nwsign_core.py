"""nwsign core: sub-CA operations, state, rate limits. Stdlib + the openssl/openvpn CLIs only.

Privacy contract: nothing in here ever persists a client IP. The rate limiter is memory only and
keyed by a salted hash of the IP (salt random per process start).
"""
import base64, calendar, contextlib, hashlib, json, os, re, secrets, subprocess, tempfile, threading, time
try:
    import fcntl
except ImportError:  # Windows (tests)
    fcntl = None
    import msvcrt

DEFAULTS = {
    "state_dir": "/var/lib/nwsign",
    "vpn_host": "",                 # set by install.sh (never in the repo)
    "vpn_port": 1194,
    "listen_host": "0.0.0.0",
    "listen_port": 8443,
    "openssl": "openssl",
    "openvpn_cmd": ["/usr/sbin/openvpn"],
    "cert_days": 7,
    "renew_after_days": 4,          # launcher should renew once this has passed
    "grace_days": 3,                # renewal still accepted this long after expiry
    "crl_days": 90,
    "crl_refresh_hours": 6,
    "enroll_per_ip_day": 3,
    "enroll_global_day": 200,
    "renew_per_install_day": 10,
    "general_per_ip_hour": 120,
    "max_body": 8192,
    "max_threads": 16,
    "request_timeout": 15,
    "profile_extra": [
        "cipher AES-256-GCM",
        "data-ciphers AES-256-GCM:CHACHA20-POLY1305",
        "verb 3",
    ],
}
ID_RE = re.compile(r"^[0-9a-f]{32}$")
DAY = 86400


class CAError(Exception):
    def __init__(self, msg, status=400):
        super().__init__(msg)
        self.status = status


def load_config(path=None):
    path = path or os.environ.get("NWSIGN_CONF", "/etc/nwsign/nwsign.json")
    cfg = dict(DEFAULTS)
    with open(path, encoding="utf-8") as f:
        cfg.update(json.load(f))
    return cfg


def parse_ossl_date(s):
    return calendar.timegm(time.strptime(s.strip().replace("  ", " "), "%b %d %H:%M:%S %Y GMT"))


def atomic_write(path, text, mode=0o600):
    d = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".tmp-")
    try:
        with os.fdopen(fd, "w", newline="\n") as f:
            f.write(text)
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise


class State:
    def __init__(self, cfg):
        self.cfg = cfg
        self.dir = cfg["state_dir"]
        self.priv = os.path.join(self.dir, "priv")
        self.pub = os.path.join(self.dir, "pub")
        self._tlock = threading.RLock()

    def p(self, name):
        return os.path.join(self.priv, name)

    def pubp(self, name):
        return os.path.join(self.pub, name)

    # ---- locking / db -------------------------------------------------
    @contextlib.contextmanager
    def lock(self):
        with self._tlock:
            os.makedirs(self.priv, exist_ok=True)
            f = open(self.p("db.lock"), "a+b")
            try:
                if fcntl:
                    fcntl.flock(f, fcntl.LOCK_EX)
                else:
                    f.seek(0)
                    while True:
                        try:
                            msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)
                            break
                        except OSError:
                            pass
                yield
            finally:
                try:
                    if fcntl:
                        fcntl.flock(f, fcntl.LOCK_UN)
                    else:
                        f.seek(0)
                        msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
                finally:
                    f.close()

    def load_db(self):
        try:
            with open(self.p("db.json"), encoding="utf-8") as f:
                return json.load(f)
        except FileNotFoundError:
            return {"installs": {}}

    def save_db(self, db):
        atomic_write(self.p("db.json"), json.dumps(db, separators=(",", ":")), 0o600)

    # ---- openssl ------------------------------------------------------
    def ossl(self, args, data=None, binary=False):
        try:
            r = subprocess.run([self.cfg["openssl"]] + list(args), input=data, capture_output=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise CAError("openssl failed: %s" % e, 500)
        if r.returncode != 0:
            raise CAError("openssl %s: %s" % (args[0], r.stderr.decode("utf-8", "replace").strip()[:300]), 400)
        return r.stdout if binary else r.stdout.decode("utf-8", "replace")

    # ---- CSR ----------------------------------------------------------
    def check_csr(self, csr_pem):
        """Validate a CSR (PEM str); return its public key as PEM. Subject is ignored by design."""
        if not isinstance(csr_pem, str) or len(csr_pem) > 2048:
            raise CAError("csr missing or too large")
        s = csr_pem.strip()
        if not re.fullmatch(r"-----BEGIN (NEW )?CERTIFICATE REQUEST-----[A-Za-z0-9+/=\r\n ]+-----END (NEW )?CERTIFICATE REQUEST-----", s):
            raise CAError("csr is not a PEM certificate request")
        data = s.encode()
        self.ossl(["req", "-verify", "-noout"], data)  # proof of possession
        txt = self.ossl(["req", "-noout", "-text"], data)
        if "id-ecPublicKey" not in txt or not re.search(r"(prime256v1|P-256)", txt):
            raise CAError("csr key must be EC P-256")
        return self.ossl(["req", "-noout", "-pubkey"], data)

    # ---- issuing ------------------------------------------------------
    def issue(self, install_id, pubkey_pem, now):
        """Sign a client cert. CN is forced to install_id. Returns (cert_pem, serial_hex, not_after)."""
        not_after = now + self.cfg["cert_days"] * DAY
        serial = "%032x" % (secrets.randbits(127) | (1 << 126))
        fmt = lambda t: time.strftime("%Y%m%d%H%M%SZ", time.gmtime(t))
        with tempfile.TemporaryDirectory() as td:
            pub, ext, out = (os.path.join(td, n) for n in ("pub.pem", "ext.cnf", "cert.pem"))
            with open(pub, "w") as f:
                f.write(pubkey_pem)
            with open(ext, "w") as f:
                f.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                        "extendedKeyUsage=clientAuth\nsubjectKeyIdentifier=hash\n"
                        "authorityKeyIdentifier=keyid\n")
            self.ossl(["x509", "-new", "-force_pubkey", pub, "-subj", "/CN=" + install_id,
                       "-CA", self.p("subca.crt"), "-CAkey", self.p("subca.key"),
                       "-set_serial", "0x" + serial, "-not_before", fmt(now - 3600),
                       "-not_after", fmt(not_after), "-extfile", ext, "-sha256", "-out", out])
            with open(out) as f:
                cert = f.read()
        return cert, serial, not_after

    def tc2_client_key(self, install_id, expires):
        meta = base64.b64encode(("%s %d" % (install_id, expires)).encode()).decode()
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, "c.key")
            cmd = list(self.cfg["openvpn_cmd"]) + ["--tls-crypt-v2", self.p("tc2-server.key"),
                                                    "--genkey", "tls-crypt-v2-client", out, meta]
            try:
                r = subprocess.run(cmd, capture_output=True, timeout=30)
            except (OSError, subprocess.TimeoutExpired) as e:
                raise CAError("openvpn genkey failed: %s" % e, 500)
            if r.returncode != 0:
                raise CAError("openvpn genkey: %s" % r.stderr.decode("utf-8", "replace")[:200], 500)
            with open(out) as f:
                return f.read()

    def ca_bundle_for_clients(self):
        # Clients only need to trust the ROOT (it signed the server cert). The sub-CA is deliberately
        # not handed to clients so a stolen sub-CA key cannot be used to impersonate the server.
        with open(self.p("root-ca.crt")) as f:
            return f.read()

    def profile(self):
        lines = ["client", "dev tun", "proto udp", "remote %s %d" % (self.cfg["vpn_host"], self.cfg["vpn_port"]),
                 "nobind", "persist-key", "persist-tun", "remote-cert-tls server"] + list(self.cfg["profile_extra"])
        return "\n".join(lines) + "\n"

    def bundle_response(self, install_id, cert, expires):
        c = self.cfg
        return {
            "install_id": install_id, "cert": cert, "ca_bundle": self.ca_bundle_for_clients(),
            "tls_crypt_v2_client_key": self.tc2_client_key(install_id, expires),
            "expires": expires, "renew_after": expires - (c["cert_days"] - c["renew_after_days"]) * DAY,
            "profile": self.profile(),
        }

    # ---- enrollment / renewal ----------------------------------------
    def enroll(self, csr_pem, now=None):
        now = int(now or time.time())
        pub = self.check_csr(csr_pem)
        install_id = secrets.token_hex(16)
        cert, serial, exp = self.issue(install_id, pub, now)
        resp = self.bundle_response(install_id, cert, exp)
        with self.lock():
            db = self.load_db()
            db["installs"][install_id] = {"s": [[serial, exp]], "exp": exp, "banned": False}
            self.save_db(db)
        return resp

    def parse_cert(self, cert_pem):
        if not isinstance(cert_pem, str) or len(cert_pem) > 4096 or "BEGIN CERTIFICATE" not in cert_pem:
            raise CAError("bad cert", 401)
        data = cert_pem.encode()
        try:
            self.ossl(["verify", "-no_check_time", "-partial_chain", "-CAfile", self.p("subca.crt"), "-purpose", "sslclient"], data)
            txt = self.ossl(["x509", "-noout", "-subject", "-serial", "-enddate", "-pubkey"], data)
        except CAError:
            raise CAError("certificate not issued by this service", 401)
        subj = re.search(r"CN\s*=\s*([0-9a-f]{32})\b", txt)
        ser = re.search(r"serial=([0-9A-Fa-f]+)", txt)
        end = re.search(r"notAfter=(.+)", txt)
        pk = re.search(r"(-----BEGIN PUBLIC KEY-----.*?-----END PUBLIC KEY-----)", txt, re.S)
        if not (subj and ser and end and pk):
            raise CAError("bad cert", 401)
        return subj.group(1), ser.group(1).lower().lstrip("0"), parse_ossl_date(end.group(1)), pk.group(1) + "\n"

    @staticmethod
    def renew_message(install_id, csr_pem, ts):
        return ("nwsign-renew-v1\n%s\n%s\n%d" % (install_id, hashlib.sha256(csr_pem.encode()).hexdigest(), ts)).encode()

    def renew(self, install_id, csr_pem, ts, sig_b64, cert_pem, now=None):
        now = int(now or time.time())
        if not (isinstance(install_id, str) and ID_RE.match(install_id) and isinstance(ts, int)
                and isinstance(csr_pem, str) and isinstance(sig_b64, str) and len(sig_b64) < 400):
            raise CAError("malformed renew request")
        if abs(now - ts) > 300:
            raise CAError("timestamp out of range (clock?)", 401)
        cn, serial, not_after, cert_pub = self.parse_cert(cert_pem)
        if cn != install_id:
            raise CAError("certificate does not belong to install_id", 401)
        if now > not_after + self.cfg["grace_days"] * DAY:
            raise CAError("certificate expired beyond grace; enroll again", 401)
        with self.lock():
            rec = self.load_db()["installs"].get(install_id)
        if rec is None:
            raise CAError("unknown install; enroll again", 401)
        if rec["banned"]:
            raise CAError("banned", 403)
        if serial not in [s.lstrip("0") for s, _ in rec["s"]]:
            raise CAError("certificate is not a current one for this install", 401)
        try:
            sig = base64.b64decode(sig_b64, validate=True)
        except Exception:
            raise CAError("bad signature encoding", 401)
        with tempfile.TemporaryDirectory() as td:
            pk, sg, mg = (os.path.join(td, n) for n in ("pk", "sg", "mg"))
            with open(pk, "w") as f:
                f.write(cert_pub)
            with open(sg, "wb") as f:
                f.write(sig)
            with open(mg, "wb") as f:
                f.write(self.renew_message(install_id, csr_pem, ts))
            try:
                self.ossl(["dgst", "-sha256", "-verify", pk, "-signature", sg, mg])
            except CAError:
                raise CAError("bad signature", 401)
        pub = self.check_csr(csr_pem)
        cert, new_serial, exp = self.issue(install_id, pub, now)
        resp = self.bundle_response(install_id, cert, exp)
        with self.lock():
            db = self.load_db()
            rec = db["installs"].get(install_id)
            if rec is None or rec["banned"]:
                raise CAError("banned", 403)
            rec["s"] = [x for x in rec["s"] if x[1] > now] + [[new_serial, exp]]
            rec["exp"] = exp
            self.save_db(db)
        return resp

    # ---- ban / CRL ----------------------------------------------------
    def set_ban(self, install_id, banned):
        with self.lock():
            db = self.load_db()
            rec = db["installs"].get(install_id)
            if rec is None:
                raise CAError("unknown install id", 404)
            rec["banned"] = bool(banned)
            self.save_db(db)
            self.regen_crl_locked(db)

    def regen_crl(self):
        with self.lock():
            db = self.load_db()
            self.gc(db)
            self.save_db(db)
            self.regen_crl_locked(db)

    def gc(self, db, now=None):
        """Forget installs whose certs all expired > 30 days ago and that are not banned."""
        now = int(now or time.time())
        for k in [k for k, r in db["installs"].items() if not r["banned"] and r["exp"] + 30 * DAY < now]:
            del db["installs"][k]

    def regen_crl_locked(self, db):
        os.makedirs(self.pub, exist_ok=True)
        fmt = lambda t: time.strftime("%y%m%d%H%M%SZ", time.gmtime(t))
        now = int(time.time())
        rows = []
        for iid, r in db["installs"].items():
            if r["banned"]:
                for s, exp in r["s"]:
                    h = s.upper()
                    h = ("0" if len(h) % 2 else "") + h
                    rows.append("R\t%s\t%s\t%s\tunknown\t/CN=%s" % (fmt(exp), fmt(now), h, iid))
        idx, num, cnf, crl = (self.p(n) for n in ("crl.index", "crl.number", "crl.cnf", "subca.crl"))
        with open(idx, "w") as f:
            f.write("\n".join(rows) + ("\n" if rows else ""))
        with open(idx + ".attr", "w") as f:
            f.write("unique_subject = no\n")
        if not os.path.exists(num):
            with open(num, "w") as f:
                f.write("01\n")
        q = lambda p: p.replace("\\", "/")
        with open(cnf, "w") as f:
            f.write("[ca]\ndefault_ca=c\n[c]\ndatabase=%s\ncrlnumber=%s\ncertificate=%s\nprivate_key=%s\n"
                    "default_md=sha256\ndefault_crl_days=%d\nunique_subject=no\n"
                    % (q(idx), q(num), q(self.p("subca.crt")), q(self.p("subca.key")), self.cfg["crl_days"]))
        self.ossl(["ca", "-gencrl", "-config", cnf, "-out", crl, "-crldays", str(self.cfg["crl_days"])])
        parts = []
        try:
            with open(self.p("root-crl.pem")) as f:
                parts.append(f.read())
        except FileNotFoundError:
            pass
        with open(crl) as f:
            parts.append(f.read())
        atomic_write(self.pubp("crl-bundle.pem"), "".join(parts), 0o644)
        atomic_write(self.pubp("banned.txt"),
                     "".join(i + "\n" for i, r in sorted(db["installs"].items()) if r["banned"]), 0o644)

    # ---- admin views --------------------------------------------------
    def list_installs(self):
        with self.lock():
            db = self.load_db()
        return sorted(((i, r["exp"], r["banned"]) for i, r in db["installs"].items()), key=lambda x: x[1])

    def stats(self, now=None):
        now = int(now or time.time())
        rows = self.list_installs()
        cd = self.cfg["cert_days"] * DAY
        return {
            "installs": len(rows),
            "active_certs": sum(1 for _, e, b in rows if e > now and not b),
            "expired": sum(1 for _, e, b in rows if e <= now and not b),
            "banned": sum(1 for _, _, b in rows if b),
            "issued_last_24h": sum(1 for _, e, _ in rows if e - cd > now - DAY),
            "expiring_next_24h": sum(1 for _, e, b in rows if now < e <= now + DAY and not b),
        }

    # ---- one-time setup (also used by tests) ---------------------------
    def init_keys(self):
        """As the service user: create sub-CA key+CSR and the HTTPS key/cert. Idempotent."""
        os.makedirs(self.priv, exist_ok=True)
        os.makedirs(self.pub, exist_ok=True)
        if not os.path.exists(self.p("subca.key")):
            self.ossl(["genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", self.p("subca.key")])
            self.ossl(["req", "-new", "-key", self.p("subca.key"), "-subj", "/CN=nwsign-ca", "-out", self.p("subca.csr")])
        if not os.path.exists(self.p("https.key")):
            self.ossl(["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "3650",
                       "-subj", "/CN=nwsign", "-keyout", self.p("https.key"), "-out", self.p("https.crt")])
        for n in ("subca.key", "https.key"):
            os.chmod(self.p(n), 0o600)
        return self.https_pin()

    def https_pin(self):
        pub = self.ossl(["x509", "-in", self.p("https.crt"), "-noout", "-pubkey"]).encode()
        der = self.ossl(["pkey", "-pubin", "-outform", "DER"], pub, binary=True)
        return hashlib.sha256(der).hexdigest()

    def sign_subca(self, root_key, root_crt, days=3650):
        """Run as root once: sign priv/subca.csr with the root CA key. Writes priv/subca.crt."""
        exp = self.ossl(["x509", "-in", root_crt, "-noout", "-enddate"]).strip().split("=", 1)[1]
        left = (parse_ossl_date(exp) - int(time.time())) // DAY - 1
        days = min(days, left)
        if days < 30:
            raise CAError("root CA expires too soon")
        with tempfile.TemporaryDirectory() as td:
            ext = os.path.join(td, "ext.cnf")
            with open(ext, "w") as f:
                f.write("basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n"
                        "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n")
            self.ossl(["x509", "-req", "-in", self.p("subca.csr"), "-CA", root_crt, "-CAkey", root_key,
                       "-set_serial", "0x" + "%032x" % (secrets.randbits(127) | (1 << 126)),
                       "-days", str(days), "-sha256", "-extfile", ext, "-out", self.p("subca.crt")])


class RateLimiter:
    """In-memory only. Keys are salted hashes; the salt is new on every process start."""

    def __init__(self):
        self.salt = os.urandom(16)
        self.lock = threading.Lock()
        self.hits = {}

    def key(self, kind, who):
        if ":" in who:  # IPv6: bucket by /64 so a client cannot rotate through its prefix
            who = ":".join(who.split(":")[:4])
        return kind + hashlib.sha256(self.salt + who.encode()).hexdigest()[:24]

    def hit(self, key, limit, window, now=None):
        """Count a hit. Returns 0 if allowed, else seconds until a slot frees."""
        now = now or time.time()
        with self.lock:
            q = [t for t in self.hits.get(key, ()) if t > now - window]
            if len(q) >= limit:
                self.hits[key] = q
                return max(1, int(q[0] + window - now) + 1)
            q.append(now)
            self.hits[key] = q
            return 0

    def prune(self, now=None):
        now = now or time.time()
        with self.lock:
            for k in [k for k, q in self.hits.items() if not q or q[-1] < now - DAY]:
                del self.hits[k]
