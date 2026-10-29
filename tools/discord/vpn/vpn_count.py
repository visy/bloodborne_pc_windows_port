#!/usr/bin/env python3
"""Print the number of connected OpenVPN clients as one JSON line. Runs ON the VPN server as an ssh forced command.

Reads only the count (management `load-stats`) and max-clients from server.conf: no addresses, no names.
  {"ok":true,"clients":N,"max_clients":M,"pool":253}   or   {"ok":false,"error":"management off"|"<short>"}
Standard library only. Paths/ports can be overridden with NWVPN_PW, NWVPN_CONF, NWVPN_MGMT_PORT.
"""
import json
import os
import re
import socket
import sys

POOL = 253   # 10.8.0.0/24, topology subnet
PW_FILE = os.environ.get("NWVPN_PW") or os.path.expanduser("~/nwvpn/mgmt.pw")
CONF = os.environ.get("NWVPN_CONF") or "/etc/openvpn/server/server.conf"
PORT = int(os.environ.get("NWVPN_MGMT_PORT") or 7505)


def parse_stats(line):
    """'SUCCESS: nclients=3,bytesin=1,bytesout=2' -> 3, else None."""
    m = re.search(r"nclients=(\d+)", line or "")
    return int(m.group(1)) if m and line.lstrip().startswith("SUCCESS") else None


def parse_max_clients(text):
    """max-clients from server.conf text (last uncommented setting), else None."""
    val = None
    for line in text.splitlines():
        m = re.match(r"\s*max-clients\s+(\d+)\s*(?:[#;].*)?$", line)
        if m:
            val = int(m.group(1))
    return val


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
                mx = parse_max_clients(f.read())
        except OSError:
            mx = None
        return {"ok": True, "clients": n, "max_clients": mx if mx is not None else POOL, "pool": POOL}
    except OSError as e:   # includes socket.timeout
        return {"ok": False, "error": type(e).__name__[:40]}
    finally:
        s.close()


def main():
    print(json.dumps(query(), separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
