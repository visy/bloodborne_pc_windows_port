"""Nightwatch VPN player-count monitor for Iosefka's health: sampler, thresholds, report lines.

Standard library only. The VPN server is reached with `ssh -o BatchMode=yes <NETDOCTOR_VPN_SSH>` where that is an
ssh Host alias (default "nwvpn") whose forced command runs tools/discord/vpn/vpn_count.py there and prints one
JSON line. Only a COUNT and the max come back: no addresses or names are read, stored or alerted, and the server's
address is configured outside the repo (the alias). Unset / "off" / no alias = "not set up", never an alert.
Samples live in nw_vpn (at, clients, max_clients, ok, error), kept 90 days.
"""
import json
import os
import subprocess
import time

POOL = 253
KEEP = 90 * 86400
NEAR = 0.8
DOWN_AFTER = 30 * 60


def ssh_target(env=None):
    v = (os.environ if env is None else env).get("NETDOCTOR_VPN_SSH", "nwvpn").strip()
    return None if v.lower() in ("", "off", "0", "no", "none") else v


def effective_max(max_clients, pool=POOL):
    return min(int(max_clients or pool), int(pool or POOL))


def parse_reply(text):
    """The forced command's output -> dict, or {"ok": False, "error": ...}."""
    try:
        d = json.loads((text or "").strip().splitlines()[-1])
        if not isinstance(d, dict):
            raise ValueError
    except (ValueError, IndexError):
        return {"ok": False, "error": "bad reply"}
    if d.get("ok"):
        try:
            return {"ok": True, "clients": int(d["clients"]), "max_clients": int(d.get("max_clients") or POOL),
                    "pool": int(d.get("pool") or POOL)}
        except (KeyError, TypeError, ValueError):
            return {"ok": False, "error": "bad reply"}
    return {"ok": False, "error": str(d.get("error") or "failed")[:60]}


def fetch(target, run=subprocess.run):
    """One sample over ssh: the parse_reply dict, plus "unconfigured": True when ssh has no such alias."""
    try:
        r = run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", target], stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        return {"ok": False, "error": type(e).__name__}
    out = r.stdout.decode("utf-8", "replace")
    if r.returncode != 0 and not out.strip():
        if "Could not resolve hostname" in r.stderr.decode("utf-8", "replace"):
            return {"ok": False, "error": "ssh alias missing", "unconfigured": True}
        return {"ok": False, "error": f"ssh failed ({r.returncode})"}
    return parse_reply(out)


def store(clinic, sample, now=None):
    t = now or time.time()
    ok = bool(sample.get("ok"))
    clinic.db.execute("INSERT INTO nw_vpn (at, clients, max_clients, ok, error) VALUES (?,?,?,?,?)",
                      (t, sample.get("clients") if ok else None, sample.get("max_clients") if ok else None,
                       int(ok), None if ok else sample.get("error")))
    clinic.db.execute("DELETE FROM nw_vpn WHERE at<?", (t - KEEP,))
    clinic.db.commit()


def sample_once(clinic, now=None, run=subprocess.run, target=None):
    """Take and store one sample. None when not set up (no target or no ssh alias), else the sample dict."""
    target = ssh_target() if target is None else target
    if not target:
        return None
    s = fetch(target, run)
    if s.get("unconfigured"):
        return None
    store(clinic, s, now)
    return s


def peak(clinic, since):
    return clinic.db.execute("SELECT MAX(clients) FROM nw_vpn WHERE ok=1 AND at>=?", (since,)).fetchone()[0]


def summary(clinic, now, target=None):
    """state: "off" (not set up), "waiting" (samples fail, none ever good), "ok", "down" (no good sample for 30 min after one)."""
    target = ssh_target() if target is None else target
    if not target:
        return {"state": "off"}
    row = clinic.db.execute("SELECT at, clients, max_clients FROM nw_vpn WHERE ok=1 ORDER BY at DESC LIMIT 1").fetchone()
    if row is None:   # no good sample ever: failures stored = set up but not working yet; nothing stored = not set up
        any_row = clinic.db.execute("SELECT 1 FROM nw_vpn LIMIT 1").fetchone()
        return {"state": "waiting" if any_row else "off"}
    at, clients, mx = row
    return {"state": "ok" if now - at <= DOWN_AFTER else "down", "clients": clients, "max_clients": mx,
            "effective_max": effective_max(mx), "age": now - at,
            "peak_24h": peak(clinic, now - 86400), "peak_7d": peak(clinic, now - 7 * 86400)}


def conditions(s):
    """[(kind, bad, text)] for the alert kinds. Nothing when not set up or before the first good sample."""
    if s["state"] in ("off", "waiting"):
        return []
    out = [("vpn-down", s["state"] == "down",
            "VPN monitor down: no good sample for over 30 min (check the ssh alias and the VPN server).")]
    if s["state"] == "ok":
        n, em = s["clients"], s["effective_max"]
        tail = "Raise max-clients in server.conf; beyond 253 the subnet must widen to /23 plus a firewall update."
        out.append(("vpn-full", n >= em, f"VPN full: {n} of {em} clients connected, new players are refused. " + tail))
        out.append(("vpn-near", NEAR * em <= n < em,
                    f"VPN near its limit: {n} of {em} clients (80% or more). " + tail))
    return out


def line(s):
    """One text line for `health` and the daily report (no addresses)."""
    if s["state"] == "off":
        return "VPN monitor: not set up"
    if s["state"] == "waiting":
        return "VPN monitor: set up, no sample yet"
    if s["state"] == "down":
        return "VPN monitor: DOWN (no good sample for over 30 min)"
    return (f"VPN players: {s['clients']} now, 24 h peak {s['peak_24h']}, 7 d peak {s['peak_7d']}, "
            f"limit {s['effective_max']} (max-clients {s['max_clients']}, pool {POOL})")
