"""Iosefka's health: job heartbeats, alerts, error log, upload queue, weekly DB upkeep, the daily report.

Standard library only (the CLI imports it without discord.py). All state is in the clinic database
(nw_health, nw_alerts, nw_errors, nw_queue), so `iosefka_cli.py health` can read what the bot recorded.

Alerts never contain player names, IPs or log lines: they name a job, a count, a size or a channel id.
Delivery (`deliver`) goes to the health thread (NETDOCTOR_HEALTH_THREAD) and falls back to a DM to the
owner (NETDOCTOR_ALERT_USER) when Discord answers 403/404. See tools/discord/CLINIC.md, "Health".
"""
import hashlib
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import time
import urllib.request
from collections import defaultdict, deque
from datetime import datetime, timezone
from pathlib import Path

import clinic_vpn

ALERT_EVERY = 6 * 3600          # one alert per kind per 6 h, unless it was resolved and came back
DISK_MIN = 2 * 2**30            # alert below this much free disk
DB_FRACTION = 0.8               # alert above this share of the DB size cap
QUEUE_MAX = 15                  # triage=pending / unsure bugs
UPKEEP_EVERY = 7 * 86400
DEFAULT_THREAD = 1563914486996279387
# job -> seconds without a run before it counts as overdue
JOB_LIMITS = {"sweep": 2.5 * 3600, "outbox": 10 * 60, "backup": 30 * 3600}
NW_SYNC_LIMIT = 3 * 3600
CLOSED = ("fixed", "closed", "duplicate")
TOOLS = (("7z", ("7z", "7zz", "7za")), ("unrar", ("unrar",)), ("ffmpeg", ("ffmpeg",)))
STAMP = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)")


def env_float(name, default):
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return float(default)


def db_cap_bytes():
    return int(min(env_float("NETDOCTOR_DB_MAX_MB", 3072), 3072) * 1024 * 1024)


# --- job heartbeats -----------------------------------------------------------------------------------

def record_job(clinic, job, detail="", now=None):
    clinic.db.execute("INSERT INTO nw_health (key, at, detail) VALUES (?,?,?) ON CONFLICT(key) DO UPDATE SET "
                      "at=excluded.at, detail=excluded.detail", (job, now or time.time(), detail))
    clinic.db.commit()


def job_row(clinic, job):
    row = clinic.db.execute("SELECT at, detail FROM nw_health WHERE key=?", (job,)).fetchone()
    return (row[0], row[1] or "") if row else (None, "")


def record_start(clinic, now=None):
    now = now or time.time()
    _at, detail = job_row(clinic, "start")
    starts = [float(x) for x in detail.split(",") if x][-30:] + [now]
    record_job(clinic, "start", ",".join(f"{x:.0f}" for x in starts), now)


def running_commit_file(conf=None):
    return Path(conf or os.environ.get("NETDOCTOR_DIR") or Path.home() / ".config" / "netdoctor") / "running_commit"


def record_running_commit(repo_dir=None, conf=None, run=subprocess.run):
    """Write `git rev-parse HEAD` of the code this process was started from to <conf>/running_commit, for
    deploy.sh to roll back to. Returns the commit or None (not a git checkout: the old file is removed)."""
    f = running_commit_file(conf)
    try:
        r = run(["git", "-C", str(repo_dir or Path(__file__).resolve().parent), "rev-parse", "HEAD"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=20)
        commit = r.stdout.decode().strip() if r.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        commit = ""
    try:
        if commit:
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(commit + "\n")
        elif f.exists():
            f.unlink()
    except OSError:
        pass
    return commit or None


def nw_sync_state(log_path=None):
    """(last timestamp or None, last line failed?) from nw_sync.log; (None, False) when there is no log."""
    path = Path(log_path or Path.home() / ".config" / "netdoctor" / "nw_sync.log")
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None, False
    last_at = None
    failed = False
    for line in reversed(lines[-50:]):
        m = STAMP.match(line)
        if m:
            last_at = datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(
                tzinfo=timezone.utc).timestamp()
            failed = "failed" in line
            break
    return last_at, failed


# --- errors -------------------------------------------------------------------------------------------

def error_sig(place, first_line):
    norm = re.sub(r"\d+", "#", first_line)
    return hashlib.sha1(f"{place}|{norm}".encode("utf-8", "replace")).hexdigest()[:8]


def note_error(clinic, place, exc, now=None):
    """Record a traceback caught in Iosefka's own handling. Returns the dedup signature."""
    first = (f"{type(exc).__name__}: {exc}".strip().splitlines() or [type(exc).__name__])[0][:200]
    sig = error_sig(place, first)
    try:
        clinic.db.execute("INSERT INTO nw_errors (at, sig, place, first_line) VALUES (?,?,?,?)",
                          (now or time.time(), sig, place, first))
        clinic.db.commit()
    except sqlite3.Error:
        pass  # the error log must never raise
    return sig


def errors_since(clinic, t):
    """[(sig, place, first_line, count, last_at)] for errors since t, most frequent first."""
    rows = clinic.db.execute("SELECT sig, place, first_line, COUNT(*), MAX(at) FROM nw_errors WHERE at>=? "
                             "GROUP BY sig ORDER BY COUNT(*) DESC, MAX(at) DESC", (t,)).fetchall()
    return [tuple(r) for r in rows]


# --- upload queue (flood guard) -----------------------------------------------------------------------

class FloodGuard:
    """At most `limit` upload messages per author in any hour; `allow` is False beyond that."""

    def __init__(self, limit=10, now=time.time):
        self.limit, self.now = limit, now
        self.seen = defaultdict(deque)

    def allow(self, author_id):
        if self.limit <= 0:
            return True
        t = self.now()
        q = self.seen[author_id]
        while q and q[0] <= t - 3600:
            q.popleft()
        if len(q) >= self.limit:
            return False
        q.append(t)
        return True


def queue_upload(clinic, message, now=None):
    clinic.db.execute("INSERT OR IGNORE INTO nw_queue (message_id, channel_id, author_id, queued) VALUES (?,?,?,?)",
                      (message.id, message.channel.id, getattr(message.author, "id", None), now or time.time()))
    clinic.db.commit()


def queued_uploads(clinic, limit=200):
    rows = clinic.db.execute("SELECT message_id, channel_id, author_id, queued FROM nw_queue "
                             "ORDER BY message_id LIMIT ?", (limit,))
    return [dict(zip(("message_id", "channel_id", "author_id", "queued"), r)) for r in rows]


def queue_size(clinic):
    return clinic.db.execute("SELECT COUNT(*) FROM nw_queue").fetchone()[0]


def drop_queued(clinic, message_id):
    clinic.db.execute("DELETE FROM nw_queue WHERE message_id=?", (message_id,))
    clinic.db.commit()


# --- DB upkeep ------------------------------------------------------------------------------------------

def db_upkeep(path, free_fn=None):
    """integrity_check, WAL checkpoint(TRUNCATE), VACUUM only when free disk >= 2x the DB size.
    Own connection (run it in an executor). Returns {"integrity": "ok"|text, "vacuumed": bool}."""
    free_fn = free_fn or (lambda p: shutil.disk_usage(p).free)
    con = sqlite3.connect(str(path), timeout=30, isolation_level=None)
    try:
        con.execute("PRAGMA busy_timeout=30000")
        res = [r[0] for r in con.execute("PRAGMA integrity_check")]
        integrity = "ok" if res == ["ok"] else "; ".join(map(str, res))[:200]
        con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        size = con.execute("PRAGMA page_count").fetchone()[0] * con.execute("PRAGMA page_size").fetchone()[0]
        vacuumed = False
        if integrity == "ok" and free_fn(Path(path).parent) >= 2 * size:
            con.execute("VACUUM")
            vacuumed = True
        return {"integrity": integrity, "vacuumed": vacuumed}
    finally:
        con.close()


# --- the health object ----------------------------------------------------------------------------------

def find_tool(names, which=shutil.which):
    return next((n for n in names if which(n)), None)


class Health:
    def __init__(self, clinic, *, db_cap=None, now=time.time, free_fn=None, nw_log=None, expect=("sweep", "outbox", "backup"),
                 started=None):
        self.clinic, self.now, self.expect, self.nw_log = clinic, now, tuple(expect), nw_log
        self.db_cap = db_cap or db_cap_bytes()
        self.free_fn = free_fn or (lambda p: shutil.disk_usage(p).free)
        self.started = started if started is not None else now()
        self.send = None   # async callable(text), set by the bot; None = log only
        self.is_skipped = None   # callable(place_id) -> True for a skipped place: never alerted about

    # alerts: trigger/resolve are sync (callable from anywhere), flush sends

    def _alert(self, kind):
        row = self.clinic.db.execute("SELECT last_sent, active, pending FROM nw_alerts WHERE kind=?", (kind,)).fetchone()
        return row

    def trigger(self, kind, text):
        row = self._alert(kind)
        t = self.now()
        due = row is None or not row[1] or (row[0] is not None and t - row[0] >= ALERT_EVERY and not row[2])
        if row is None:
            self.clinic.db.execute("INSERT INTO nw_alerts (kind, last_sent, active, pending) VALUES (?,NULL,1,?)", (kind, text))
        elif due:
            self.clinic.db.execute("UPDATE nw_alerts SET active=1, pending=? WHERE kind=?", (text, kind))
        self.clinic.db.commit()
        return due

    def trigger_once(self, kind, text):
        """Never again after the first time (a missing permission on one place)."""
        if self._alert(kind) is not None:
            return False
        return self.trigger(kind, text)

    def resolve(self, kind):
        self.clinic.db.execute("UPDATE nw_alerts SET active=0 WHERE kind=? AND active=1", (kind,))
        self.clinic.db.commit()

    def set_condition(self, kind, bad, text):
        if bad:
            self.trigger(kind, text)
        else:
            self.resolve(kind)

    def active_alerts(self):
        return [r[0] for r in self.clinic.db.execute("SELECT kind FROM nw_alerts WHERE active=1 ORDER BY kind")]

    async def flush(self, out=print):
        rows = self.clinic.db.execute("SELECT kind, pending FROM nw_alerts WHERE pending IS NOT NULL").fetchall()
        for kind, text in rows:
            if self.send is not None:
                try:
                    await self.send("Health alert: " + text)
                except Exception as e:
                    out(f"alert not delivered ({kind}): {type(e).__name__}; will retry")
                    continue
            else:
                out(f"alert (no destination): {text}")
            self.clinic.db.execute("UPDATE nw_alerts SET pending=NULL, last_sent=? WHERE kind=?", (self.now(), kind))
            self.clinic.db.commit()

    # checks

    def free_disk(self):
        return self.free_fn(self.clinic.path.parent)

    def queues(self):
        c = self.clinic
        return {"triage": len(c.bugs(tag="triage=pending")),
                "unsure": len(c.bugs(tag="unsure", exclude_status=CLOSED)), "uploads": queue_size(c)}

    def job_status(self):
        """{job: {"at": t|None, "age": s|None, "overdue": bool}}"""
        t = self.now()
        out = {}
        for job, limit in JOB_LIMITS.items():
            at, _d = job_row(self.clinic, job)
            base = max(at or 0, self.started)
            out[job] = {"at": at, "age": None if at is None else t - at,
                        "overdue": job in self.expect and t - base > limit}
        at, failed = nw_sync_state(self.nw_log)
        out["nw_sync"] = {"at": at, "age": None if at is None else t - at, "failed": failed,
                          "overdue": at is not None and t - at > NW_SYNC_LIMIT}
        return out

    def check(self):
        """Evaluate every condition; trigger or resolve its alert."""
        t = self.now()
        for job, st in self.job_status().items():
            label = {"sweep": "hourly sweep", "outbox": "outbox sender", "backup": "nightly backup",
                     "nw_sync": "Nightwatch sync"}[job]
            age = f"last run {st['age'] / 3600:.1f} h ago" if st["age"] is not None else "no run since start"
            self.set_condition(f"overdue:{job}", st["overdue"], f"the {label} is overdue ({age}).")
        self.set_condition("nw_sync_failed", bool(self.job_status()["nw_sync"].get("failed")),
                           "the last Nightwatch sync run failed (see nw_sync.log).")
        free = self.free_disk()
        self.set_condition("disk-low", free < DISK_MIN, f"disk free is {free / 2**30:.1f} GB (below 2 GB).")
        size = self.clinic.size_bytes()
        self.set_condition("db-size", size > DB_FRACTION * self.db_cap,
                           f"the database is {size / 2**20:.0f} MiB of {self.db_cap / 2**20:.0f} MiB cap (over 80%).")
        q = self.queues()
        self.set_condition("queue-triage", q["triage"] > QUEUE_MAX, f"{q['triage']} bugs wait for triage.")
        self.set_condition("queue-unsure", q["unsure"] > QUEUE_MAX, f"{q['unsure']} bugs are marked unsure.")
        for kind, bad, text in clinic_vpn.conditions(self.vpn()):
            self.set_condition(kind, bad, text)
        recent = {e[0]: e for e in errors_since(self.clinic, t - 3600)}
        for sig, place, _first, n, _last in recent.values():
            self.trigger(f"error:{sig}", f"an error in {place} ({n} in the last hour, id {sig}); see `iosefka health`.")
        for kind in self.active_alerts():
            if kind.startswith("error:") and kind[6:] not in recent:
                self.resolve(kind)
        self.clinic.db.execute("DELETE FROM nw_errors WHERE at<?", (t - 7 * 86400,))
        self.clinic.db.commit()

    def vpn(self):
        return clinic_vpn.summary(self.clinic, self.now())

    def forbidden(self, place_id, what="read or post"):
        """Discord said 403 on a place she needs: one alert per place, ever. Never for a skipped place."""
        try:
            if self.is_skipped is not None and self.is_skipped(place_id):
                return
        except Exception:
            pass
        self.trigger_once(f"forbidden:{place_id}", f"Discord refuses me ({what}) in channel {place_id}.")

    # self-check at start

    def self_check(self, guilds, *, delete=False, skip=lambda ch: False, which=shutil.which):
        """Permissions preflight, quick_check and tools. Returns a list of problem texts (also alerted, as ONE
        alert for all channels). Send Messages is only required where @everyone can post or attach files (a
        player could upload a log there); a forum needs Send Messages in Threads + Manage Threads/Messages
        instead; read-only places need View Channel + Read Message History."""
        problems, bad_names, scanned, bad_ids = [], [], set(), set()
        for g in guilds:
            me = getattr(g, "me", None)
            everyone = getattr(g, "default_role", None)

            def skip_private(ch, me, everyone):
                """A private place nobody but staff can view and she was never given: no reason to be there."""
                if everyone is None:
                    return False
                try:
                    return not getattr(ch.permissions_for(everyone), "view_channel", True) and                         not getattr(ch.permissions_for(me), "view_channel", True)
                except Exception:
                    return False
            forums = list(getattr(g, "forums", None) or [])
            for ch in list(getattr(g, "text_channels", None) or []) + forums:
                if me is None or not hasattr(ch, "permissions_for"):
                    continue
                if skip(ch):
                    scanned.add(str(ch.id))   # never her place: an old forbidden alert for it is closed
                    continue
                try:
                    perms = ch.permissions_for(me)
                    ev = ch.permissions_for(everyone) if everyone is not None else None
                except Exception:
                    continue
                scanned.add(str(ch.id))
                if skip_private(ch, me, everyone):
                    continue
                need = ["view_channel", "read_message_history"]
                if ch in forums:
                    if ev is None or getattr(ev, "send_messages_in_threads", True):
                        need.append("send_messages_in_threads")
                        if not (getattr(perms, "manage_threads", True) or getattr(perms, "manage_messages", True)):
                            need.append("manage_threads")
                else:
                    if ev is None or getattr(ev, "send_messages", True) or getattr(ev, "attach_files", True):
                        need.append("send_messages")
                    if delete:
                        need.append("manage_messages")
                miss = [p for p in need if not getattr(perms, p, True)]
                if miss:
                    problems.append(f"channel #{ch.name}: missing {', '.join(miss)}")
                    bad_names.append(f"#{ch.name} ({', '.join(miss)})")
                    bad_ids.add(str(ch.id))
        self.set_condition("self-check-permissions", bool(bad_names),
                           "at startup Discord permissions are missing in " + "; ".join(bad_names) + ".")
        for kind in self.active_alerts():
            if kind.startswith("forbidden:") and kind[10:] in scanned and kind[10:] not in bad_ids:
                self.resolve(kind)
        qc = [r[0] for r in self.clinic.db.execute("PRAGMA quick_check")]
        if qc != ["ok"]:
            problems.append("database quick_check failed")
            self.trigger("db-quick-check", "PRAGMA quick_check on the database failed.")
        else:
            self.resolve("db-quick-check")
        for name, names in TOOLS:
            have = find_tool(names, which)
            if not have:
                problems.append(f"{name} missing")
            self.set_condition(f"tool:{name}", not have, f"the {name} program is not installed.")
        return problems

    # weekly upkeep

    def upkeep_due(self, hour):
        t = self.now()
        last, _d = job_row(self.clinic, "db_upkeep")
        tried, _d = job_row(self.clinic, "db_upkeep_try")
        return hour == 5 and t - (last or 0) >= UPKEEP_EVERY and t - (tried or 0) >= 20 * 3600

    def run_upkeep(self, out=print):
        record_job(self.clinic, "db_upkeep_try", "", self.now())
        try:
            res = db_upkeep(self.clinic.path, self.free_fn)
        except Exception as e:
            out(f"db upkeep failed: {type(e).__name__}: {e}")
            record_job(self.clinic, "db_upkeep_failed", type(e).__name__, self.now())
            return None
        record_job(self.clinic, "db_upkeep", json.dumps(res), self.now())
        self.set_condition("db-integrity", res["integrity"] != "ok", "PRAGMA integrity_check on the database failed.")
        out(f"db upkeep: integrity {res['integrity']}, vacuumed {res['vacuumed']}")
        return res

    # reports

    def snapshot(self, with_versions=False):
        t = self.now()
        c = self.clinic
        _s, starts = job_row(c, "start")
        starts = [float(x) for x in starts.split(",") if x]
        tick, _d = job_row(c, "health_tick")
        errs = errors_since(c, t - 86400)
        size = c.size_bytes()
        snap = {
            "now": t,
            "last_start": starts[-1] if starts else None,
            "restarts_24h": sum(1 for x in starts if x >= t - 86400),
            "last_tick": tick,
            "jobs": self.job_status(),
            "errors_24h": {"count": sum(e[3] for e in errs),
                           "kinds": [{"place": e[1], "first_line": e[2], "count": e[3], "last": e[4]} for e in errs]},
            "disk_free": self.free_disk(),
            "db": {"size": size, "cap": self.db_cap, "fraction": size / self.db_cap if self.db_cap else 0},
            "queues": self.queues(),
            "new_bugs_24h": len(c.bugs(since=t - 86400)),
            "logs_24h": c.db.execute("SELECT COUNT(*) FROM nw_files WHERE kind='log' AND created>=?", (t - 86400,)).fetchone()[0],
            "alerts_active": self.active_alerts(),
            "vpn": self.vpn(),
            "upkeep": job_row(c, "db_upkeep"),
        }
        if with_versions:
            snap["versions"] = versions()
        return snap


def versions(which=shutil.which):
    here = Path(__file__).resolve().parent
    commit = None
    try:
        commit = subprocess.run(["git", "-C", str(here), "rev-parse", "--short", "HEAD"], capture_output=True,
                                text=True, timeout=10).stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        from importlib import metadata
        dpy = metadata.version("discord.py")
    except Exception:
        dpy = None
    return {"git": commit, "discord.py": dpy, "python": sys.version.split()[0],
            "tools": {name: bool(find_tool(names, which)) for name, names in TOOLS}}


# --- the daily report -------------------------------------------------------------------------------------

def _ago(s):
    if s is None:
        return "never"
    return f"{s / 60:.0f} min ago" if s < 5400 else f"{s / 3600:.1f} h ago"


def report_text(snap):
    """Iosefka's morning report: a greeting and a compact list. No names, IPs or log lines."""
    jobs = snap["jobs"]
    late = [j for j, st in jobs.items() if st["overdue"]]
    q, db = snap["queues"], snap["db"]
    up = f"up since {datetime.fromtimestamp(snap['last_start']).strftime('%m-%d %H:%M')}" if snap["last_start"] else "uptime unknown"
    lines = ["Good morning. I made my rounds; here is how the clinic is doing.",
             f"- Me: {up}, {snap['restarts_24h']} start(s) in 24 h",
             "- Jobs: " + ("all on time" if not late else "OVERDUE: " + ", ".join(late)),
             f"- Errors in 24 h: {snap['errors_24h']['count']}",
             f"- Disk free: {snap['disk_free'] / 2**30:.1f} GB; database {db['size'] / 2**20:.0f} of {db['cap'] / 2**20:.0f} MiB",
             f"- Queues: triage {q['triage']}, unsure {q['unsure']}, uploads waiting {q['uploads']}",
             f"- Last 24 h: {snap['new_bugs_24h']} new bugs, {snap['logs_24h']} logs"]
    lines.append("- " + clinic_vpn.line(snap.get("vpn") or {"state": "off"}))
    if snap["alerts_active"]:
        lines.append(f"- Open alerts: {len(snap['alerts_active'])}")
    return "\n".join(lines)


def local_day(now, tz):
    """(date string, HH:MM) of the unix time `now` in zone `tz` (DST aware)."""
    from zoneinfo import ZoneInfo
    d = datetime.fromtimestamp(now, ZoneInfo(tz))
    return d.strftime("%Y-%m-%d"), d.strftime("%H:%M")


def report_due(clinic, now, at="08:30", tz="Europe/Helsinki"):
    day, hm = local_day(now, tz)
    _t, last_day = job_row(clinic, "report_day")
    return hm >= at and last_day != day, day


async def send_daily_report(health, *, at=None, tz=None, force=False, out=print):
    at = at or os.environ.get("NETDOCTOR_HEALTH_AT", "08:30")
    tz = tz or os.environ.get("NETDOCTOR_HEALTH_TZ", "Europe/Helsinki")
    due, day = report_due(health.clinic, health.now(), at, tz)
    if not (due or force) or health.send is None:
        return False
    try:
        await health.send(report_text(health.snapshot()))
    except Exception as e:
        out(f"daily report not delivered: {type(e).__name__}; will retry")
        return False
    if not force:
        record_job(health.clinic, "report_day", day, health.now())
    return True


# --- delivery ---------------------------------------------------------------------------------------------

class Deliver:
    """Post to the health thread; on 403/404 log once and DM the owner (NETDOCTOR_ALERT_USER) instead.
    Only ever sends: it reads nothing from the thread."""

    def __init__(self, client, *, thread_id=None, user_id=None, allowed_mentions=None, out=print):
        env = os.environ
        self.client = client
        self.thread_id = thread_id if thread_id is not None else int(env.get("NETDOCTOR_HEALTH_THREAD") or DEFAULT_THREAD)
        self.user_id = user_id if user_id is not None else (int(env["NETDOCTOR_ALERT_USER"]) if env.get("NETDOCTOR_ALERT_USER") else None)
        self.am, self.out, self.warned = allowed_mentions, out, False

    async def __call__(self, text):
        text = text[:1990]
        kw = {"allowed_mentions": self.am} if self.am is not None else {}
        if self.thread_id:
            try:
                place = self.client.get_channel(self.thread_id) or await self.client.fetch_channel(self.thread_id)
                await place.send(text, **kw)
                self.warned = False
                return "thread"
            except Exception as e:
                if type(e).__name__ not in ("Forbidden", "NotFound"):
                    raise
                if not self.warned:
                    self.warned = True
                    self.out(f"health thread {self.thread_id}: {type(e).__name__}; falling back to a DM")
        if not self.user_id:
            raise RuntimeError("no health destination: thread refused and NETDOCTOR_ALERT_USER unset")
        user = await self.client.fetch_user(self.user_id)
        await user.send(text, **kw)
        return "dm"


def rest_send(token, text, *, thread_id=None, user_id=None, opener=urllib.request.urlopen):
    """Post `text` with plain REST (for deploy.sh, no discord.py): thread first, else a DM. True on success."""
    def call(url, body):
        req = urllib.request.Request("https://discord.com/api/v10" + url, data=json.dumps(body).encode(), method="POST",
                                     headers={"Authorization": "Bot " + token, "Content-Type": "application/json",
                                              "User-Agent": "DiscordBot (https://github.com/visy/bloodborne_pc_windows_port, 1)"})
        with opener(req, timeout=20) as r:
            return json.loads(r.read() or b"{}")
    text = text[:1990]
    thread_id = thread_id or os.environ.get("NETDOCTOR_HEALTH_THREAD") or DEFAULT_THREAD
    user_id = user_id or os.environ.get("NETDOCTOR_ALERT_USER")
    try:
        call(f"/channels/{thread_id}/messages", {"content": text, "allowed_mentions": {"parse": []}})
        return True
    except Exception:
        pass
    if not user_id:
        return False
    try:
        dm = call("/users/@me/channels", {"recipient_id": str(user_id)})
        call(f"/channels/{dm['id']}/messages", {"content": text, "allowed_mentions": {"parse": []}})
        return True
    except Exception:
        return False


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) >= 2 and argv[0] == "alert":
        env = Path.home() / ".config" / "netdoctor" / "env"
        if env.is_file():
            for line in env.read_text().splitlines():
                if "=" in line and not line.lstrip().startswith("#"):
                    k, v = line.split("=", 1)
                    os.environ.setdefault(k.strip(), v.strip())
        ok = rest_send(os.environ.get("NETDOCTOR_TOKEN", ""), "Health alert: " + " ".join(argv[1:]))
        print("alert sent" if ok else "alert NOT sent")
        return 0 if ok else 1
    print("usage: clinic_health.py alert TEXT", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
