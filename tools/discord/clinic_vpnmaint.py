"""Nightwatch VPN maintenance announcements: one scheduled VPN restart (e.g. the /22 pool cutover) told to players.

Standard library only. visy schedules it with the CLI (`iosefka vpn-maint schedule --at "2026-11-02 20:00" --channel
announcements`); the bot's health loop (every 5 min, and once a minute while one is pending) posts what is due:
  announce  as soon as it is scheduled (or at --announce-at): when, what happens, what to do before
  reminder  15 min before (skipped when scheduled closer than that)
  back      after the time, once a good VPN sample (clinic_vpn) taken after it shows players again, or 30 min after the
            time at the latest: how many are back and the new limit
Times are Discord timestamps (<t:UNIX:F>, <t:UNIX:R>): every reader sees their own local time. No names or addresses.
Rows live in nw_vpn_maint; cancel before the time with `vpn-maint cancel ID` (posts a short cancellation if announced).
"""
import time
from datetime import datetime
from zoneinfo import ZoneInfo

SCHEMA = """CREATE TABLE IF NOT EXISTS nw_vpn_maint (
    id INTEGER PRIMARY KEY, at REAL, channel TEXT, note TEXT, created REAL, by TEXT, announce_at REAL,
    announced REAL, reminded REAL, done REAL, cancelled REAL, cancel_told REAL, clients_before INTEGER)"""
REMIND_BEFORE = 15 * 60
BACK_MAX = 30 * 60      # the "back" post goes out this long after the time even without a good sample
BACK_MIN_WAIT = 60      # a sample at least this long after the time counts (the restart takes ~30 s)


def ensure(clinic):
    clinic.db.execute(SCHEMA)
    clinic.db.commit()


def parse_when(text, tz="Europe/Helsinki"):
    """'2026-11-02 20:00' (local in tz) or a unix time -> unix seconds."""
    t = str(text).strip()
    if t.isdigit():
        return float(t)
    for fmt in ("%Y-%m-%d %H:%M", "%Y-%m-%dT%H:%M", "%Y-%m-%d %H:%M:%S"):
        try:
            return datetime.strptime(t, fmt).replace(tzinfo=ZoneInfo(tz)).timestamp()
        except ValueError:
            continue
    raise ValueError("time must look like 2026-11-02 20:00 (in %s) or a unix time" % tz)


def schedule(clinic, at, channel, note="", by="", announce_at=None, now=None):
    ensure(clinic)
    now = now or time.time()
    if at <= now + 60:
        raise ValueError("the maintenance time must be at least a minute ahead")
    if not channel or not all(c.isalnum() or c in "-_" for c in channel):
        raise ValueError("channel: a channel name like announcements")
    pending = clinic.db.execute("SELECT id FROM nw_vpn_maint WHERE done IS NULL AND cancelled IS NULL").fetchone()
    if pending:
        raise ValueError("maintenance %d is already pending: cancel it first" % pending[0])
    cur = clinic.db.execute("INSERT INTO nw_vpn_maint (at, channel, note, created, by, announce_at) VALUES (?,?,?,?,?,?)",
                            (at, channel, (note or "")[:300], now, by, announce_at or now))
    clinic.db.commit()
    return cur.lastrowid


def cancel(clinic, mid, now=None):
    ensure(clinic)
    row = clinic.db.execute("SELECT done, cancelled FROM nw_vpn_maint WHERE id=?", (mid,)).fetchone()
    if not row:
        raise ValueError("no maintenance %s" % mid)
    if row[0] or row[1]:
        raise ValueError("maintenance %s is already over or cancelled" % mid)
    clinic.db.execute("UPDATE nw_vpn_maint SET cancelled=? WHERE id=?", (now or time.time(), mid))
    clinic.db.commit()


def rows(clinic, limit=10):
    ensure(clinic)
    cols = [d[1] for d in clinic.db.execute("PRAGMA table_info(nw_vpn_maint)")]
    return [dict(zip(cols, r)) for r in clinic.db.execute("SELECT * FROM nw_vpn_maint ORDER BY id DESC LIMIT ?", (limit,))]


def pending(clinic):
    """The one maintenance still to finish (announce / remind / back / cancel note), or None."""
    ensure(clinic)
    r = [x for x in rows(clinic, 5) if x["done"] is None and (x["cancelled"] is None or (x["announced"] and not x["cancel_told"]))]
    return r[0] if r else None


def _ts(t, style):
    return "<t:%d:%s>" % (int(t), style)


def announce_text(m):
    lines = ["**Nightwatch VPN maintenance** %s (%s)" % (_ts(m["at"], "F"), _ts(m["at"], "R")),
             "- The VPN restarts once: everyone on it drops for about 30 seconds and reconnects by itself.",
             "- A party playing over the VPN at that moment ends: rejoin after the restart.",
             "- Before then: update Nightwatch Co-op to the latest version (needed for the bigger player limit).",
             "- Afterwards: room for many more players on the VPN."]
    if m.get("note"):
        lines.append("- " + m["note"])
    return "\n".join(lines)


def reminder_text(m):
    return ("**Reminder:** Nightwatch VPN maintenance %s. The VPN drops for about 30 seconds; a party over the VPN "
            "ends, so finish your fight or wait it out, then rejoin." % _ts(m["at"], "R"))


def back_text(m, sample):
    if sample and sample.get("ok"):
        lim = min(int(sample.get("max_clients") or 0) or 10**6, int(sample.get("pool") or 0) or 10**6)
        return ("**Nightwatch VPN is back** after the maintenance: %d players connected, room for %d. If your launcher "
                "still shows the VPN as off, press Connect." % (sample["clients"], lim))
    return ("**Nightwatch VPN maintenance is over.** If your launcher still shows the VPN as off, press Connect. "
            "Problems? Post your launcher log in the help channel.")


def cancel_text(m):
    return "**Nightwatch VPN maintenance %s is cancelled.** Nothing changes; play on." % _ts(m["at"], "F")


def latest_sample_after(clinic, t):
    r = clinic.db.execute("SELECT at, clients, max_clients, pool FROM nw_vpn WHERE ok=1 AND at>=? ORDER BY at DESC LIMIT 1",
                          (t,)).fetchone()
    if not r:
        return None
    return {"ok": True, "at": r[0], "clients": r[1], "max_clients": r[2], "pool": r[3]}


def due(clinic, now=None):
    """[(kind, maint row, text)] to post now. The caller posts each, then calls mark(kind, id)."""
    now = now or time.time()
    m = pending(clinic)
    if not m:
        return []
    if m["cancelled"]:
        return [("cancel", m, cancel_text(m))]
    out = []
    if not m["announced"]:
        if now >= (m["announce_at"] or m["created"]):
            out.append(("announce", m, announce_text(m)))
        return out
    if not m["reminded"] and m["at"] - REMIND_BEFORE <= now < m["at"] and m["announced"] < m["at"] - REMIND_BEFORE:
        out.append(("remind", m, reminder_text(m)))
    if now >= m["at"] + BACK_MIN_WAIT:
        s = latest_sample_after(clinic, m["at"] + BACK_MIN_WAIT)
        if (s and s["clients"] > 0) or now >= m["at"] + BACK_MAX:
            out.append(("back", m, back_text(m, s)))
    return out


def mark(clinic, kind, mid, now=None):
    col = {"announce": "announced", "remind": "reminded", "back": "done", "cancel": "cancel_told"}[kind]
    clinic.db.execute("UPDATE nw_vpn_maint SET %s=? WHERE id=?" % col, (now or time.time(), mid))
    if kind == "back":   # a reminder that never went out (the bot was down) is not owed any more
        clinic.db.execute("UPDATE nw_vpn_maint SET reminded=COALESCE(reminded, ?) WHERE id=?", (now or time.time(), mid))
    clinic.db.commit()


def soon(clinic, now=None):
    """True while a maintenance is pending within the next hour or ran in the last 35 min: tick every minute."""
    now = now or time.time()
    m = pending(clinic)
    return bool(m) and (m["cancelled"] is not None or m["at"] - 3600 <= now <= m["at"] + BACK_MAX + 300 or not m["announced"])
