#!/usr/bin/env python3
"""Print the number of connected OpenVPN clients as one JSON line. Runs ON the VPN server as an ssh forced command.

Reads only the count (management `load-stats`) and max-clients from server.conf: no addresses, no names.
  {"ok":true,"clients":N,"max_clients":M,"pool":253,"lobby":L}   or   {"ok":false,"error":"management off"|"<short>"}
L = nwlobby's counts from its loopback socket (127.0.0.1:9311): {"games","players","open","full","joinable",
"free_slots","sizes","builds"}, or null when
the lobby does not answer (an older nwlobby, or it is down). Counts only.
"signups" = nwsign's loopback GET /v1/stats (https://127.0.0.1:$NWSIGN_PORT, default 8443): {installs, active_certs,
issued_last_24h, banned, enroll_global_used, enroll_global_day, enroll_per_ip_day, refused_24h:{reason: n}} or null.
Standard library only. Paths/ports can be overridden with NWVPN_PW, NWVPN_CONF, NWVPN_MGMT_PORT.
"""
import json
import os
import re
import socket
import ssl
import sys
import urllib.request

POOL = 253   # fallback: 10.8.0.0/24, topology subnet (the real size comes from server.conf's "server" line)
PW_FILE = os.environ.get("NWVPN_PW") or os.path.expanduser("~/nwvpn/mgmt.pw")
CONF = os.environ.get("NWVPN_CONF") or "/etc/openvpn/server/server.conf"
PORT = int(os.environ.get("NWVPN_MGMT_PORT") or 7505)
LOBBY_STATS = ("127.0.0.1", int(os.environ.get("NWVPN_LOBBY_STATS_PORT") or 9311))
LOBBY_MAGIC, LOBBY_VERSION, T_STATS, T_STATS_REPLY = bytes([0xFA, 0x4C]), 1, 5, 6   # tools/vpn/lobby/nwlobby.py


def parse_stats(line):
    """'SUCCESS: nclients=3,bytesin=1,bytesout=2' -> 3, else None."""
    m = re.search(r"nclients=(\d+)", line or "")
    return int(m.group(1)) if m and line.lstrip().startswith("SUCCESS") else None


def parse_pool(text):
    """Client addresses of the "server <net> <mask>" line (topology subnet: hosts minus the server), else None.
    /24 -> 253, /23 -> 509, /22 -> 1021."""
    val = None
    for line in text.splitlines():
        m = re.match(r"\s*server\s+(\d+\.\d+\.\d+\.\d+)\s+(\d+\.\d+\.\d+\.\d+)\s*(?:[#;].*)?$", line)
        if m:
            try:
                bits = sum(bin(int(o)).count("1") for o in m.group(2).split("."))
            except ValueError:
                continue
            if 8 <= bits <= 30:
                val = 2 ** (32 - bits) - 3
    return val


def parse_max_clients(text):
    """max-clients from server.conf text (last uncommented setting), else None."""
    val = None
    for line in text.splitlines():
        m = re.match(r"\s*max-clients\s+(\d+)\s*(?:[#;].*)?$", line)
        if m:
            val = int(m.group(1))
    return val


LOBBY_KEYS = ("games", "players", "open", "full", "joinable", "free_slots")


def parse_lobby_stats(data):
    """nwlobby's stats reply (JSON after the 4-byte header) -> counts dict, else None. Only known keys pass."""
    if len(data) < 5 or data[:2] != LOBBY_MAGIC or data[2] != LOBBY_VERSION or data[3] != T_STATS_REPLY:
        return None
    try:
        d = json.loads(data[4:].decode("utf-8"))
        out = {k: int(d[k]) for k in LOBBY_KEYS}
        out["sizes"] = {str(k)[:4]: int(v) for k, v in dict(d.get("sizes") or {}).items()}
        out["builds"] = {str(k)[:32]: int(v) for k, v in list(dict(d.get("builds") or {}).items())[:5]}
        return out
    except (ValueError, KeyError, TypeError, AttributeError):
        return None


def lobby(addr=LOBBY_STATS, timeout=2.0):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(timeout)
            s.sendto(LOBBY_MAGIC + bytes([LOBBY_VERSION, T_STATS]), addr)
            return parse_lobby_stats(s.recvfrom(2048)[0])
    except OSError:
        return None


SIGNUP_KEYS = ("installs", "active_certs", "issued_last_24h", "banned")


def parse_signups(data):
    """nwsign's /v1/stats JSON (bytes or str) -> counts dict, else None. Only known integer keys pass."""
    try:
        d = json.loads(data.decode("utf-8") if isinstance(data, bytes) else data)
        if not isinstance(d, dict) or d.get("ok") is not True:
            return None
        out = {k: int(d[k]) for k in SIGNUP_KEYS}
        lim = d.get("limits") or {}
        out["enroll_global_used"] = int(d["enroll_global_used"])
        out["enroll_global_day"] = int(lim["enroll_global_day"])
        out["enroll_per_ip_day"] = int(lim["enroll_per_ip_day"])
        out["refused_24h"] = {str(k)[:40]: int(v) for k, v in list(dict(d.get("refused_24h") or {}).items())[:10]}
        return out
    except (ValueError, KeyError, TypeError, AttributeError):
        return None


def signups(port=None, timeout=2.0):
    port = int(port or os.environ.get("NWSIGN_PORT") or 8443)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE   # loopback to our own self-signed service
    try:
        with urllib.request.urlopen("https://127.0.0.1:%d/v1/stats" % port, timeout=timeout, context=ctx) as r:
            return parse_signups(r.read(65536))
    except (OSError, ValueError):   # URLError, timeouts, ssl errors are OSError
        return None


def _wait(sock, want, timeout=5.0):
    """Read until a line starts with one of `want` (a prompt without newline counts); return that line, else ''."""
    sock.settimeout(timeout)
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return ""
        buf += chunk
        text = buf.decode("utf-8", "replace")
        for line in text.splitlines():
            for w in want:
                if line.startswith(w):
                    return line


def query(host="127.0.0.1", port=PORT, pw_file=PW_FILE, conf=CONF):
    try:
        s = socket.create_connection((host, port), timeout=5)
    except OSError:
        return {"ok": False, "error": "management off"}
    try:
        try:
            pw = open(pw_file).read().strip()
        except OSError:
            return {"ok": False, "error": "no password file"}
        first = _wait(s, ("ENTER PASSWORD", ">INFO", "SUCCESS"))
        if first.startswith("ENTER PASSWORD"):
            s.sendall((pw + "\n").encode())
            if not _wait(s, ("SUCCESS", "FAILURE")).startswith("SUCCESS"):
                return {"ok": False, "error": "bad password"}
        s.sendall(b"load-stats\n")
        n = parse_stats(_wait(s, ("SUCCESS", "ERROR")))
        try:
            s.sendall(b"quit\n")
        except OSError:
            pass
        if n is None:
            return {"ok": False, "error": "no stats"}
        try:
            with open(conf) as f:
                conf_text = f.read()
            mx, pool = parse_max_clients(conf_text), parse_pool(conf_text)
        except OSError:
            mx, pool = None, None
        pool = pool or POOL
        return {"ok": True, "clients": n, "max_clients": mx if mx is not None else pool, "pool": pool}
    except OSError as e:   # includes socket.timeout
        return {"ok": False, "error": type(e).__name__[:40]}
    finally:
        s.close()


def main():
    d = query()
    if d.get("ok"):
        d["lobby"] = lobby()
        try:
            d["signups"] = signups()
        except Exception:   # noqa: BLE001 - never fail the whole reply
            d["signups"] = None
    print(json.dumps(d, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
