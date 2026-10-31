"""Nightwatch VPN player-count monitor for Iosefka's health: sampler, thresholds, report lines.

Standard library only. The VPN server is reached with `ssh -o BatchMode=yes <NETDOCTOR_VPN_SSH>` where that is an
ssh Host alias (default "nwvpn") whose forced command runs tools/discord/vpn/vpn_count.py there and prints one
JSON line. Only a COUNT and the max come back: no addresses or names are read, stored or alerted, and the server's
address is configured outside the repo (the alias). Unset / "off" / no alias = "not set up", never an alert.
Samples live in nw_vpn (at, clients, max_clients, ok, error, and the lobby's games, open_games, joinable,
in_games: NULL when the lobby did not answer), kept 90 days.
"""
import json
import os
import re
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
            out = {"ok": True, "clients": int(d["clients"]), "max_clients": int(d.get("max_clients") or POOL),
                   "pool": int(d.get("pool") or POOL), "lobby": None}
        except (KeyError, TypeError, ValueError):
            return {"ok": False, "error": "bad reply"}
        lb = d.get("lobby")
        if isinstance(lb, dict):
            try:
                v = {k: int(lb[k]) for k in ("games", "players", "open", "full")}
                for k in ("joinable", "free_slots"):
                    v[k] = int(lb.get(k) or 0)
                v["sizes"] = {str(k)[:4]: int(n) for k, n in dict(lb.get("sizes") or {}).items()}
                v["builds"] = {str(k)[:32]: int(n) for k, n in list(dict(lb.get("builds") or {}).items())[:5]}
                out["lobby"] = v
            except (KeyError, TypeError, ValueError, AttributeError):
                pass
        return out
    return {"ok": False, "error": str(d.get("error") or "failed")[:60]}


def _plural(n, one, many=None):
    return f"{n} {one if n == 1 else (many or one + 's')}"


def _md(text):
    """A lobby-supplied build name made inert in Discord markdown and readable: Nightwatch's
    "nightwatch-coop-<nw version>-visy-<coop>-<sha>" (cut at 32 chars by the game) -> "NW <nw version>"."""
    t = "".join(c for c in text if c.isalnum() or c in ".-_+()")[:32]
    if t.startswith("nightwatch-coop-"):
        rest = re.sub(r"-v(?:i(?:sy?)?)?$", "", t[len("nightwatch-coop-"):].split("-visy")[0])  # a cut "-visy"
        t = ("NW " + rest.strip("-")) if rest.strip("-") else "Nightwatch"
    return t or "?"


def status_text(sample, s):
    """/vpnstatus: a live sample (fetch) + the stored summary -> a short Discord message for players. Counts only:
    no names or addresses."""
    if s.get("state") == "off" and not sample.get("ok"):
        return "**Nightwatch VPN**: monitor not set up."
    if not sample.get("ok"):
        return f"**Nightwatch VPN**: no answer from the server ({sample.get('error', 'failed')})."
    n = sample["clients"]
    em = effective_max(sample["max_clients"], sample.get("pool") or POOL)
    lines = ["**Nightwatch VPN** is up", f"- **{n}** of {em} players connected"]
    lb = sample.get("lobby")
    if lb is None:
        lines.append("- Game list: unavailable right now")
    else:
        g = lb["games"]
        lines.append(f"- **{_plural(g, 'game')}** listed: {lb['open']} public (no password), "
                     f"{g - lb['open']} with a password, {lb['full']} full")
        if lb["joinable"]:
            lines.append(f"- **{_plural(lb['joinable'], 'public game')}** with room right now "
                         f"({_plural(lb['free_slots'], 'free slot')})")
        elif lb["open"]:
            lines.append("- Every public game is full right now: host one!")
        elif g:
            lines.append("- No public game right now: host one without a password!")
        lines.append(f"- {_plural(lb['players'], 'player')} in listed games, "
                     f"{max(0, n - lb['players'])} connected but not in one")
        if lb.get("sizes"):
            sizes = sorted(lb["sizes"].items(), key=lambda kv: int(kv[0]) if kv[0].isdigit() else 99)
            lines.append("- Party sizes: " + ", ".join(f"{_plural(c, 'game')} up to {k}" for k, c in sizes))
        if lb.get("builds"):
            lines.append("- Versions: " + ", ".join(f"{_md(k)} ({c})" for k, c in lb["builds"].items()))
    if s.get("peak_24h") is not None:
        lines.append(f"- Busiest: {max(s['peak_24h'], n)} players in 24 h, {max(s.get('peak_7d') or 0, n)} in 7 days")
    d = s.get("day") or {}
    if d.get("lobby_samples"):
        lines.append(f"- Last 24 h: up to {d['games_peak']} games at once ({d['open_peak']} public), "
                     f"a public game with room {100 * d['room_share']:.0f}% of the time")
    return "\n".join(lines)


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
    lb = (sample.get("lobby") or {}) if ok else {}
    clinic.db.execute("INSERT INTO nw_vpn (at, clients, max_clients, ok, error, games, open_games, joinable, in_games, "
                      "pool) VALUES (?,?,?,?,?,?,?,?,?,?)",
                      (t, sample.get("clients") if ok else None, sample.get("max_clients") if ok else None,
                       int(ok), None if ok else sample.get("error"),
                       lb.get("games"), lb.get("open"), lb.get("joinable"), lb.get("players"),
                       sample.get("pool") if ok else None))
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


def window(clinic, since):
    """24 h (or any) window of good samples: players avg/peak, games avg/peak, public games peak, joinable share.
    None for a value with no samples (a lobby-less sample only counts for players)."""
    r = clinic.db.execute(
        "SELECT COUNT(*), AVG(clients), MAX(clients), COUNT(games), AVG(games), MAX(games), MAX(open_games), "
        "AVG(in_games), SUM(CASE WHEN joinable>0 THEN 1 ELSE 0 END) FROM nw_vpn WHERE ok=1 AND at>=?",
        (since,)).fetchone()
    n, avg_p, max_p, n_l, avg_g, max_g, max_o, avg_in, with_room = r
    return {"samples": n, "players_avg": avg_p, "players_peak": max_p, "lobby_samples": n_l, "games_avg": avg_g,
            "games_peak": max_g, "open_peak": max_o, "in_games_avg": avg_in,
            "room_share": (with_room / n_l) if n_l else None}


def summary(clinic, now, target=None):
    """state: "off" (not set up), "waiting" (samples fail, none ever good), "ok", "down" (no good sample for 30 min after one)."""
    target = ssh_target() if target is None else target
    if not target:
        return {"state": "off"}
    row = clinic.db.execute("SELECT at, clients, max_clients, pool FROM nw_vpn WHERE ok=1 ORDER BY at DESC LIMIT 1").fetchone()
    if row is None:   # no good sample ever: failures stored = set up but not working yet; nothing stored = not set up
        any_row = clinic.db.execute("SELECT 1 FROM nw_vpn LIMIT 1").fetchone()
        return {"state": "waiting" if any_row else "off"}
    at, clients, mx, pool = row
    pool = pool or POOL
    return {"state": "ok" if now - at <= DOWN_AFTER else "down", "clients": clients, "max_clients": mx,
            "pool": pool, "effective_max": effective_max(mx, pool), "age": now - at,
            "peak_24h": peak(clinic, now - 86400), "peak_7d": peak(clinic, now - 7 * 86400),
            "day": window(clinic, now - 86400)}


def conditions(s):
    """[(kind, bad, text)] for the alert kinds. Nothing when not set up or before the first good sample."""
    if s["state"] in ("off", "waiting"):
        return []
    out = [("vpn-down", s["state"] == "down",
            "VPN monitor down: no good sample for over 30 min (check the ssh alias and the VPN server).")]
    if s["state"] == "ok":
        n, em = s["clients"], s["effective_max"]
        pool = s.get("pool") or POOL
        tail = (f"Raise max-clients in server.conf; beyond {pool} the pool must widen (/23 = 509, /22 = 1021; the game "
                "accepts 10.8.0.0/22 from 0.7.8) plus a firewall update and one VPN restart.")
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
    out = (f"VPN players: {s['clients']} now, 24 h peak {s['peak_24h']}, 7 d peak {s['peak_7d']}, "
           f"limit {s['effective_max']} (max-clients {s['max_clients']}, pool {s.get('pool') or POOL})")
    d = s.get("day") or {}
    if d.get("samples"):
        out += f"; 24 h average {d['players_avg']:.1f} players"
    if d.get("lobby_samples"):
        out += (f"; games 24 h: average {d['games_avg']:.1f}, peak {d['games_peak']} ({d['open_peak']} public at most), "
                f"{d['in_games_avg']:.1f} players in games on average, a public game with room "
                f"{100 * d['room_share']:.0f}% of the time")
    return out
