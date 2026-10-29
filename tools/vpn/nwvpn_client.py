#!/usr/bin/env python3
"""Reference client for the nwsign service (Nightwatch VPN per-install certificates).

    nwvpn_client.py --dir DIR --server <vpn-host>:8443 --pin <spki-sha256-hex> enroll
    nwvpn_client.py --dir DIR --server ... --pin ... renew      (only if renew_after has passed, or --force)
    nwvpn_client.py --dir DIR status

DIR receives: client.key (private, never leaves the machine), install.json (id, cert, expiry),
nightwatch.ovpn (ready to use, contains the key, user-only permissions).
Needs only Python 3 and the openssl CLI.
"""
import argparse, base64, hashlib, hmac, http.client, json, os, shutil, ssl, subprocess, sys, time


class ClientError(Exception):
    pass


def openssl(*args, data=None):
    r = subprocess.run(["openssl"] + list(args), input=data, capture_output=True)
    if r.returncode:
        raise ClientError("openssl %s failed: %s" % (args[0], r.stderr.decode(errors="replace")[:200]))
    return r.stdout


def restrict(path):
    """User-only permissions (chmod 600 / icacls on Windows)."""
    if os.name == "nt":
        user = os.environ.get("USERNAME", "")
        subprocess.run(["icacls", path, "/inheritance:r", "/grant:r", user + ":F"], capture_output=True)
    else:
        os.chmod(path, 0o600)


def write_private(path, text):
    # create empty, lock down, then write, so the secret is never world-readable
    open(path, "w").close()
    restrict(path)
    with open(path, "w", newline="\n") as f:
        f.write(text)


# ---- SPKI pin ---------------------------------------------------------
def _tlv(b, o):
    tag = b[o]
    n = b[o + 1]
    o += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[o:o + k], "big")
        o += k
    return tag, o, n  # tag, content start, content length


def spki_sha256(cert_der):
    """SHA-256 of the SubjectPublicKeyInfo DER inside an X.509 certificate."""
    _, o, _ = _tlv(cert_der, 0)           # Certificate
    _, o, _ = _tlv(cert_der, o)           # TBSCertificate
    tag, s, n = _tlv(cert_der, o)
    if tag == 0xA0:                       # [0] version
        o = s + n
    for _ in range(5):                    # serial, sigalg, issuer, validity, subject
        _, s, n = _tlv(cert_der, o)
        o = s + n
    _, s, n = _tlv(cert_der, o)           # SPKI
    return hashlib.sha256(cert_der[o:s + n]).hexdigest()


def call(server, pin, path, obj):
    host, _, port = server.partition(":")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE      # trust comes from the pin below, not from a CA/hostname
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    conn = http.client.HTTPSConnection(host, int(port or 8443), context=ctx, timeout=20)
    try:
        conn.connect()
        got = spki_sha256(conn.sock.getpeercert(binary_form=True))
        if not hmac.compare_digest(got, pin.lower()):
            raise ClientError("server certificate does not match the pinned key; refusing to send anything")
        body = json.dumps(obj).encode()
        conn.request("POST", path, body, {"Content-Type": "application/json"})
        r = conn.getresponse()
        data = json.loads(r.read(1 << 20) or b"{}")
        if r.status != 200:
            e = ClientError("HTTP %d: %s" % (r.status, data.get("error", "")))
            e.status, e.retry_after = r.status, r.getheader("Retry-After")
            raise e
        return data
    finally:
        conn.close()


# ---- operations -------------------------------------------------------
def new_key_csr(d):
    key = os.path.join(d, "client.key")
    if not os.path.exists(key):
        write_private(key, openssl("ecparam", "-genkey", "-name", "prime256v1", "-noout").decode())
    # the service ignores the subject; any value will do
    csr = openssl("req", "-new", "-key", key, "-subj", "/CN=nwvpn").decode()
    return key, csr


def save(d, resp):
    key = open(os.path.join(d, "client.key")).read()
    info = {k: resp[k] for k in ("install_id", "cert", "expires", "renew_after")}
    with open(os.path.join(d, "install.json"), "w") as f:
        json.dump(info, f)
    ovpn = (resp["profile"].rstrip("\n") + "\n<ca>\n" + resp["ca_bundle"].strip() + "\n</ca>\n<cert>\n"
            + resp["cert"].strip() + "\n</cert>\n<key>\n" + key.strip() + "\n</key>\n<tls-crypt-v2>\n"
            + resp["tls_crypt_v2_client_key"].strip() + "\n</tls-crypt-v2>\n")
    write_private(os.path.join(d, "nightwatch.ovpn"), ovpn)
    return os.path.join(d, "nightwatch.ovpn")


def enroll(d, server, pin):
    os.makedirs(d, exist_ok=True)
    _, csr = new_key_csr(d)
    return save(d, call(server, pin, "/v1/enroll", {"csr": csr}))


def renew(d, server, pin, force=False):
    info = json.load(open(os.path.join(d, "install.json")))
    if not force and time.time() < info["renew_after"]:
        return None
    # fresh key each renewal; the OLD key signs the request, the new key goes into the new cert
    oldkey = os.path.join(d, "client.key")
    newkey = os.path.join(d, "client.key.new")
    if os.path.exists(newkey):
        os.unlink(newkey)
    write_private(newkey, openssl("ecparam", "-genkey", "-name", "prime256v1", "-noout").decode())
    csr = openssl("req", "-new", "-key", newkey, "-subj", "/CN=nwvpn").decode()
    ts = int(time.time())
    msg = ("nwsign-renew-v1\n%s\n%s\n%d" % (info["install_id"], hashlib.sha256(csr.encode()).hexdigest(), ts)).encode()
    sig = base64.b64encode(openssl("dgst", "-sha256", "-sign", oldkey, data=msg)).decode()
    resp = call(server, pin, "/v1/renew", {"install_id": info["install_id"], "csr": csr, "timestamp": ts,
                                           "signature": sig, "cert": info["cert"]})
    os.replace(newkey, oldkey)
    return save(d, resp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(os.path.expanduser("~"), ".nwvpn"))
    ap.add_argument("--server", help="<vpn-host>:8443")
    ap.add_argument("--pin", help="SHA-256 of the server SPKI, hex")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("cmd", choices=["enroll", "renew", "status"])
    a = ap.parse_args()
    try:
        if a.cmd == "status":
            i = json.load(open(os.path.join(a.dir, "install.json")))
            print("install_id %s\nexpires %s\nrenew_after %s" % (i["install_id"], time.ctime(i["expires"]), time.ctime(i["renew_after"])))
            return
        if not (a.server and a.pin):
            sys.exit("--server and --pin are required")
        out = enroll(a.dir, a.server, a.pin) if a.cmd == "enroll" else renew(a.dir, a.server, a.pin, a.force)
        print(out or "not due yet")
    except ClientError as e:
        sys.exit("error: %s" % e)


if __name__ == "__main__":
    main()
