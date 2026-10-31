"""Iosefka's health: alerts, job heartbeats, flood guard, self-check, upkeep, the morning report, deploy.sh."""
import asyncio
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(ROOT / "tests"))
import clinic_store  # noqa: E402
import clinic_health as hl  # noqa: E402
import clinic_fakes as F  # noqa: E402
import iosefka_cli  # noqa: E402

spec = importlib.util.spec_from_file_location("netdoctor_bot", DISCORD / "netdoctor_bot.py")
bot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bot)
cd = bot.cd
LOG = b"[coop] Net: party networking on (host, as HostA, port 9307)\n"
GB = 2**30


def run(coro):
    return asyncio.run(coro)


class Clock:
    def __init__(self, t=1_800_000_000.0):
        self.t = t

    def __call__(self):
        return self.t


def make(tmp_path, clock=None, free=50 * GB, **kw):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    clock = clock or Clock()
    h = hl.Health(c, now=clock, free_fn=lambda p: free, nw_log=tmp_path / "nw_sync.log", **kw)
    sent = []

    async def send(text):
        sent.append(text)
    h.send = send
    return c, h, clock, sent


def test_alert_rate_limit_and_retrigger(tmp_path):
    c, h, clock, sent = make(tmp_path, free=1 * GB, expect=())
    h.check()
    run(h.flush())
    assert len(sent) == 1 and "disk free is 1.0 GB" in sent[0]
    for _ in range(3):
        clock.t += 3600
        h.check()
        run(h.flush())
    assert len(sent) == 1                      # still bad, inside 6 h: silent
    clock.t += 4 * 3600
    h.check()
    run(h.flush())
    assert len(sent) == 2                      # 6 h later: a reminder
    h.free_fn = lambda p: 50 * GB
    h.check()
    h.free_fn = lambda p: 1 * GB
    clock.t += 60
    h.check()
    run(h.flush())
    assert len(sent) == 3                      # resolved, then back: at once


def test_conditions_db_queue_overdue(tmp_path):
    c, h, clock, sent = make(tmp_path)
    h.db_cap = c.size_bytes() + 1              # over 80% of the cap
    for i in range(16):
        c.new_bug(f"t{i}", "thread", 1, 100 + i, tags={"triage": "pending", "unsure": ["x"]})
    clock.t += 3 * 3600                        # nothing ran for 3 h since start
    h.check()
    kinds = set(h.active_alerts())
    assert {"db-size", "queue-triage", "queue-unsure", "overdue:sweep", "overdue:outbox"} <= kinds
    assert "overdue:backup" not in kinds       # nightly: 30 h
    hl.record_job(c, "sweep", now=clock.t)
    hl.record_job(c, "outbox", now=clock.t)
    h.check()
    assert "overdue:sweep" not in h.active_alerts()


def test_nw_sync_log_overdue_and_failed(tmp_path):
    c, h, clock, sent = make(tmp_path)
    stamp = datetime.fromtimestamp(clock.t - 5 * 3600, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    (tmp_path / "nw_sync.log").write_text(f"{stamp} pull failed\n")
    h.check()
    assert {"overdue:nw_sync", "nw_sync_failed"} <= set(h.active_alerts())


def test_errors_dedup_and_no_private_text_in_alert(tmp_path):
    c, h, clock, sent = make(tmp_path)
    for n in (1, 2, 3):
        hl.note_error(c, "clinic loop", ValueError(f"bad thing for SecretPlayer at 203.0.113.{n}"), clock.t)
    hl.note_error(c, "edit handling", KeyError("x"), clock.t)
    h.check()
    run(h.flush())
    assert len(sent) == 2 and "3 in the last hour" in "".join(sent)
    assert not any("SecretPlayer" in t or "203.0.113" in t for t in sent)
    clock.t += 7200
    h.check()                                  # an hour with no errors resolves them
    assert not [k for k in h.active_alerts() if k.startswith("error:")]


def test_forbidden_once_per_place(tmp_path):
    c, h, clock, sent = make(tmp_path)
    h.forbidden(77)
    h.forbidden(77)
    run(h.flush())
    clock.t += 30 * 3600
    h.forbidden(77)
    h.forbidden(78)
    run(h.flush())
    assert len(sent) == 2 and "channel 77" in sent[0] and "channel 78" in sent[1]


def test_unsent_alert_is_kept_and_no_destination_logs(tmp_path):
    c, h, clock, sent = make(tmp_path, free=1 * GB)

    async def boom(text):
        raise OSError("down")
    h.send = boom
    h.check()
    run(h.flush(out=lambda *_: None))
    assert c.db.execute("SELECT pending FROM nw_alerts").fetchone()[0]
    h.send = None
    lines = []
    run(h.flush(out=lines.append))
    assert lines and c.db.execute("SELECT pending FROM nw_alerts").fetchone()[0] is None


class Perms:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def test_self_check_permissions_db_and_tools(tmp_path):
    c, h, clock, sent = make(tmp_path)
    good = F.Channel(1, "help")
    bad = F.Channel(2, "bugs")
    staff = F.Channel(3, "staff-chat")
    good.permissions_for = lambda me: Perms(view_channel=True, read_message_history=True, send_messages=True,
                                            manage_messages=True)
    bad.permissions_for = lambda me: Perms(view_channel=True, read_message_history=False, send_messages=True,
                                           manage_messages=False)
    staff.permissions_for = lambda me: Perms(view_channel=False)
    g = F.Guild([good, bad, staff])
    g.me = object()
    problems = h.self_check([g], delete=True, skip=cd.place_skipped,
                            which=lambda n: None if n == "unrar" else "/bin/" + n)
    assert any("#bugs" in p and "read_message_history" in p and "manage_messages" in p for p in problems)
    assert "unrar missing" in problems and not any("staff-chat" in p or "#help" in p for p in problems)
    run(h.flush())
    assert any("#bugs" in t for t in sent) and any("unrar" in t for t in sent)
    assert "database quick_check failed" not in problems


def _guild_for_rules(tmp_path):
    c, h, clock, sent = make(tmp_path)
    everyone = object()
    me = object()
    ro = F.Channel(10, "rules")      # members cannot post; I cannot send either
    ann = F.Channel(11, "releases")
    forum = F.Channel(12, "pre-release-bug-reports")
    help_ = F.Channel(13, "help")    # members post here: I must be able to send

    def pf(ch, post_everyone, mine):
        def f(who):
            if who is everyone:
                return Perms(send_messages=post_everyone, attach_files=post_everyone,
                             send_messages_in_threads=post_everyone)
            return Perms(view_channel=True, read_message_history=True, **mine)
        ch.permissions_for = f
    pf(ro, False, dict(send_messages=False))
    pf(ann, False, dict(send_messages=False))
    pf(forum, False, dict(send_messages=False, send_messages_in_threads=True, manage_threads=True))
    pf(help_, True, dict(send_messages=False))
    g = F.Guild([ro, ann, help_], forums=[forum])
    g.me, g.default_role = me, everyone
    return c, h, sent, g, (ro, ann, forum, help_), pf


def test_self_check_only_requires_send_where_players_post(tmp_path):
    c, h, sent, g, (ro, ann, forum, help_), pf = _guild_for_rules(tmp_path)
    h.forbidden(10, "missing send_messages")   # an old per-channel alert from the previous rule
    h.forbidden(13, "missing send_messages")
    problems = h.self_check([g], which=lambda n: "/bin/" + n)
    assert [p for p in problems if "channel" in p] == ["channel #help: missing send_messages"]
    run(h.flush())
    perm = [t for t in sent if "permissions" in t]
    assert len(perm) == 1 and "#help" in perm[0] and "#rules" not in perm[0] and "13" not in perm[0]
    assert "forbidden:10" not in h.active_alerts() and "forbidden:13" in h.active_alerts()
    # forum: needs send in threads + manage threads/messages
    pf(forum, True, dict(send_messages=False, send_messages_in_threads=True, manage_threads=False, manage_messages=False))
    assert any("pre-release-bug-reports" in p and "manage_threads" in p
               for p in h.self_check([g], which=lambda n: "/bin/" + n))
    pf(forum, True, dict(send_messages=False, send_messages_in_threads=False, manage_threads=True))
    assert any("pre-release-bug-reports" in p and "send_messages_in_threads" in p
               for p in h.self_check([g], which=lambda n: "/bin/" + n))
    # all fixed: the permission alert resolves
    pf(help_, True, dict(send_messages=True))
    pf(forum, True, dict(send_messages_in_threads=True, manage_threads=True))
    assert not [p for p in h.self_check([g], which=lambda n: "/bin/" + n) if "channel" in p]
    assert "self-check-permissions" not in h.active_alerts()


def test_self_check_ignores_private_places_and_forbidden_hook_skips(tmp_path):
    c, h, sent, g, (ro, ann, forum, help_), pf = _guild_for_rules(tmp_path)
    pf(help_, True, dict(send_messages=True))
    everyone = g.default_role
    priv = F.Channel(1562986604342026291, "Voice lounge")
    priv.permissions_for = lambda who: Perms(view_channel=False)   # neither @everyone nor she can view it
    g.text_channels.append(priv)
    h.forbidden(priv.id)
    h.trigger(f"forbidden:{priv.id}", "old")
    assert f"forbidden:{priv.id}" in h.active_alerts()
    problems = h.self_check([g], skip=lambda ch: False, which=lambda n: "/bin/" + n)
    assert not [p for p in problems if "channel" in p]
    assert f"forbidden:{priv.id}" not in h.active_alerts()
    # skipped by name (Dev Test) is closed too
    dev = F.Channel(77, "Dev Test")
    dev.permissions_for = lambda who: Perms(view_channel=True, read_message_history=False)
    g.text_channels.append(dev)
    h.trigger("forbidden:77", "old")
    assert not [p for p in h.self_check([g], skip=lambda ch: ch.name.lower().startswith("dev"),
                                        which=lambda n: "/bin/" + n) if "channel" in p]
    assert "forbidden:77" not in h.active_alerts()
    h.is_skipped = lambda pid: pid == 555
    h.forbidden(555, "read or post")
    assert "forbidden:555" not in h.active_alerts()
    h.forbidden(556)
    assert "forbidden:556" in h.active_alerts()
    assert "self-check-permissions" not in h.active_alerts()


def test_upkeep_vacuum_only_with_room(tmp_path):
    c, h, clock, sent = make(tmp_path)
    c.new_bug("x", "thread", 1, 2)
    assert hl.db_upkeep(c.path, lambda p: 1) == {"integrity": "ok", "vacuumed": False}
    assert hl.db_upkeep(c.path, lambda p: 10**12) == {"integrity": "ok", "vacuumed": True}
    assert not h.upkeep_due(4) and h.upkeep_due(5)
    assert h.run_upkeep(out=lambda *_: None)["integrity"] == "ok"
    assert not h.upkeep_due(5)
    clock.t += 8 * 86400
    assert h.upkeep_due(5)


def test_upkeep_integrity_failure_alerts(tmp_path, monkeypatch):
    c, h, clock, sent = make(tmp_path)
    monkeypatch.setattr(hl, "db_upkeep", lambda *a: {"integrity": "page 3 corrupt", "vacuumed": False})
    h.run_upkeep(out=lambda *_: None)
    run(h.flush())
    assert "db-integrity" in h.active_alerts() and "integrity_check" in sent[0]


# --- flood guard ---------------------------------------------------------------------------------------

def test_flood_guard_window():
    clock = Clock()
    g = hl.FloodGuard(3, now=clock)
    assert [g.allow(1) for _ in range(4)] == [True, True, True, False]
    assert g.allow(2)                           # other authors are not affected
    clock.t += 3601
    assert g.allow(1)


def test_flood_queue_is_swept_in_order_and_still_deleted(tmp_path):
    ch = F.Channel(100, "help")
    client = F.Client([], [ch])
    c = clinic_store.Clinic(tmp_path / "nd.db")
    ctx = bot.Ctx(c, live=True, delete=True, tmp_root=tmp_path / "tmp", client=client,
                  log_info_fn=lambda p: {"name": "HostA", "role": "host", "host": "HostA", "start": 600, "end": 660,
                                         "day": "d", "peers": set()},
                  diagnose_fn=lambda paths: "**Co-op connection check**\n\nNo network problem found in these logs.")
    ctx.flood = hl.FloodGuard(2)
    msgs = []
    for i in range(1, 6):
        att = F.Att(i * 10, f"n{i}.log", LOG + bytes([65 + i]))
        m = F.Msg(i, ch, F.User(7, "SecretPlayer"), "crash", [att])
        ch.messages.append(m)
        msgs.append(m)
    taken = [run(bot.dispatch_message(m, ctx)) for m in msgs]
    assert taken == [True] * 5
    assert [m.deleted for m in msgs] == [True, True, False, False, False]
    assert [q["message_id"] for q in hl.queued_uploads(c)] == [3, 4, 5]
    assert run(bot.drain_queue(ctx)) == 3
    assert all(m.deleted for m in msgs) and hl.queue_size(c) == 0   # nothing dropped; logs gone from the channel
    assert all(c.message(i)["state"] == "deleted" for i in range(1, 6))


def test_flood_queue_keeps_row_when_disk_low(tmp_path, monkeypatch):
    ch = F.Channel(100, "help")
    client = F.Client([], [ch])
    c = clinic_store.Clinic(tmp_path / "nd.db")
    ctx = bot.Ctx(c, live=True, delete=True, tmp_root=tmp_path / "tmp", client=client)
    m = F.Msg(1, ch, F.User(7), "x", [F.Att(10, "a.log", LOG)])
    ch.messages.append(m)
    hl.queue_upload(c, m)

    async def low(*a, **k):
        return "disk-low"
    monkeypatch.setattr(bot, "handle_log_message", low)
    assert run(bot.drain_queue(ctx, out=lambda *_: None)) == 0 and hl.queue_size(c) == 1
    ch.messages.clear()
    ch.index.clear()                            # message gone: the row leaves
    assert run(bot.drain_queue(ctx)) == 0 and hl.queue_size(c) == 0


def test_dm_and_staff_not_throttled(tmp_path):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    ctx = bot.Ctx(c, live=True, delete=False, tmp_root=tmp_path / "tmp")
    ctx.flood = hl.FloodGuard(1)
    dm = F.Channel(900, "dm", guild=None)
    for i in (1, 2):
        m = F.Msg(i, dm, F.User(7), "log", [F.Att(i, f"a{i}.log", LOG + bytes([i]))], guild=None)
        run(bot.dispatch_message(m, ctx))
    assert hl.queue_size(c) == 0
    staff = F.Channel(5, "staff-chat")
    for i in (3, 4):
        run(bot.dispatch_message(F.Msg(i, staff, F.User(7), "log", [F.Att(i, "a.log", LOG)]), ctx))
    assert hl.queue_size(c) == 0 and c.message(3) is None   # staff places stay untouched


# --- report, delivery, CLI -----------------------------------------------------------------------------

def _tz_ok():
    try:
        import zoneinfo
        zoneinfo.ZoneInfo("Europe/Helsinki")
        return True
    except Exception:
        return False


@pytest.mark.skipif(not _tz_ok(), reason="no tz database")
def test_report_time_is_helsinki_dst_aware(tmp_path):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    summer = datetime(2026, 7, 1, 5, 29, tzinfo=timezone.utc).timestamp()   # 08:29 EEST
    winter = datetime(2026, 12, 1, 6, 29, tzinfo=timezone.utc).timestamp()  # 08:29 EET
    assert not hl.report_due(c, summer)[0] and hl.report_due(c, summer + 120)[0]
    assert not hl.report_due(c, winter)[0] and hl.report_due(c, winter + 120)[0]
    assert not hl.report_due(c, summer - 1800)[0]


@pytest.mark.skipif(not _tz_ok(), reason="no tz database")
def test_daily_report_sent_once_a_day_without_private_data(tmp_path):
    c, h, clock, sent = make(tmp_path)
    clock.t = datetime(2026, 12, 1, 7, 0, tzinfo=timezone.utc).timestamp()   # 09:00 Helsinki
    c.new_bug("Crash for SecretPlayer 203.0.113.9", "thread", 1, 2)
    assert run(hl.send_daily_report(h, at="08:30", tz="Europe/Helsinki")) is True
    assert run(hl.send_daily_report(h, at="08:30", tz="Europe/Helsinki")) is False
    clock.t += 86400
    assert run(hl.send_daily_report(h, at="08:30", tz="Europe/Helsinki")) is True
    text = sent[0]
    assert text.startswith("Good morning") and "SecretPlayer" not in text and "203.0.113" not in text
    assert "Jobs:" in text and "Queues:" in text and "Disk free" in text
    assert run(hl.send_daily_report(h, force=True)) is True    # always sends when forced


class Place:
    def __init__(self, exc=None):
        self.exc, self.sent = exc, []

    async def send(self, text, **kw):
        if self.exc:
            raise self.exc
        self.sent.append(text)


class DClient:
    def __init__(self, thread, user):
        self.thread, self.user = thread, user

    def get_channel(self, cid):
        return self.thread

    async def fetch_user(self, uid):
        return self.user


def test_deliver_thread_then_dm_fallback_logs_once():
    logs = []
    thread, user = Place(F.Forbidden("403")), Place()
    d = hl.Deliver(DClient(thread, user), thread_id=5, user_id=9, out=logs.append)
    assert run(d("a")) == "dm" and run(d("b")) == "dm"
    assert user.sent == ["a", "b"] and len(logs) == 1
    thread.exc = None
    assert run(d("c")) == "thread" and thread.sent == ["c"]
    d2 = hl.Deliver(DClient(Place(F.Forbidden("403")), user), thread_id=5, user_id=None, out=logs.append)
    with pytest.raises(RuntimeError):
        run(d2("x"))


def test_rest_send_thread_then_dm():
    calls = []

    class R(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *a):
            pass

    def opener(req, timeout=0):
        calls.append(req.full_url)
        if "/channels/5/" in req.full_url:
            raise OSError("403")
        return R(json.dumps({"id": "77"}).encode())
    assert hl.rest_send("tok", "hi", thread_id=5, user_id=9, opener=opener)
    assert calls[-1].endswith("/channels/77/messages")
    assert hl.rest_send("tok", "hi", thread_id=5, user_id=None, opener=opener) is False


def test_cli_health_command(tmp_path):
    db = tmp_path / "nd.db"
    c = clinic_store.Clinic(db)
    hl.record_start(c)
    hl.record_job(c, "sweep")
    hl.note_error(c, "clinic loop", ValueError("boom 203.0.113.9"))
    out = io.BytesIO()
    assert iosefka_cli.main(["health"], "vlad", out, db=db) == 0
    data = json.loads(out.getvalue())
    assert data["jobs"]["sweep"]["at"] and data["errors_24h"]["count"] == 1
    assert "203.0.113.9" not in json.dumps(data) and "tools" in data["versions"]
    out = io.BytesIO()
    assert iosefka_cli.main(["health", "--text"], "visy", out, db=db) == 0
    assert b"job sweep" in out.getvalue() and b"errors 24 h: 1" in out.getvalue()


def test_cull_health_report(tmp_path, capsys):
    spec2 = importlib.util.spec_from_file_location("iosefka_cull", DISCORD / "iosefka_cull.py")
    cull = importlib.util.module_from_spec(spec2)
    spec2.loader.exec_module(cull)
    c = clinic_store.Clinic(tmp_path / "nd.db")
    assert run(cull.health_report(c, dry_run=True)) == cull.OK
    assert "Good morning" in capsys.readouterr().out
    thread = Place()
    out = []
    assert run(cull.health_report(c, DClient(thread, Place()), out=out.append)) == cull.OK and thread.sent


@pytest.mark.skipif(not shutil.which("sh"), reason="no sh")
def test_deploy_dry_run_prints_every_step():
    r = subprocess.run(["sh", str(DISCORD / "deploy.sh"), "--dry-run"], capture_output=True, text=True,
                       env={"PATH": os.environ["PATH"], "HOME": str(ROOT / "nonexistent")})
    assert r.returncode == 0, r.stderr
    for step in ("1/6", "2/6", "3/6", "4/6", "5/6", "6/6", "[t]ools/discord/netdoctor_bot.py",
                 "pytest tests -q -k clinic", "health --text"):
        assert step in r.stdout


# --- Nightwatch VPN monitor ------------------------------------------------------------------------------

import clinic_vpn as vpn  # noqa: E402

sys.path.insert(0, str(DISCORD / "vpn"))
import vpn_count  # noqa: E402


def vpn_sample(c, clock, n, mx=100, ok=True):
    vpn.store(c, {"ok": True, "clients": n, "max_clients": mx} if ok else {"ok": False, "error": "x"}, clock.t)


def test_vpn_count_parsing():
    assert vpn_count.parse_stats("SUCCESS: nclients=7,bytesin=1,bytesout=2") == 7
    assert vpn_count.parse_stats("ERROR: nope") is None
    assert vpn_count.parse_stats("") is None
    conf = "port 1194\n# max-clients 5\nmax-clients 100\n"
    assert vpn_count.parse_max_clients(conf) == 100
    assert vpn_count.parse_max_clients("max-clients 255 # more\n") == 255
    assert vpn_count.parse_max_clients("port 1\n") is None
    assert vpn_count.query(port=1)["error"] in ("management off", "no password file")


def test_vpn_parse_reply_and_fetch():
    assert vpn.parse_reply('{"ok":true,"clients":3,"max_clients":100,"pool":253}\n')["clients"] == 3
    assert vpn.parse_reply('{"ok":false,"error":"management off"}')["error"] == "management off"
    assert vpn.parse_reply("garbage") == {"ok": False, "error": "bad reply"}

    class R:
        def __init__(s, rc, out=b"", err=b""):
            s.returncode, s.stdout, s.stderr = rc, out, err
    good = lambda *a, **k: R(0, b'{"ok":true,"clients":2,"max_clients":255,"pool":253}\n')
    assert vpn.fetch("nwvpn", good)["clients"] == 2
    assert vpn.fetch("nwvpn", lambda *a, **k: R(255, b"", b"Could not resolve hostname nwvpn"))["unconfigured"]
    assert vpn.fetch("nwvpn", lambda *a, **k: R(255, b"", b"Permission denied"))["error"].startswith("ssh failed")


def test_vpn_effective_max():
    assert vpn.effective_max(100) == 100
    assert vpn.effective_max(255) == 253


def test_vpn_not_set_up(tmp_path, monkeypatch):
    monkeypatch.setenv("NETDOCTOR_VPN_SSH", "off")
    c, h, clock, sent = make(tmp_path, expect=())
    h.check()
    run(h.flush())
    assert h.vpn() == {"state": "off"} and not [s for s in sent if "VPN" in s]
    assert "VPN monitor: not set up" in hl.report_text(h.snapshot())
    # default alias but never a sample stored (alias not in ssh config): still "not set up", no alert
    monkeypatch.delenv("NETDOCTOR_VPN_SSH")
    assert vpn.summary(c, clock.t)["state"] == "off"
    assert vpn.sample_once(c, clock.t, run=lambda *a, **k: type("R", (), {
        "returncode": 255, "stdout": b"", "stderr": b"ssh: Could not resolve hostname nwvpn"})()) is None
    assert vpn.summary(c, clock.t)["state"] == "off"
    assert "VPN monitor: not set up" in "\n".join(iosefka_cli._health_text(iosefka_cli._health(c)))


def test_vpn_thresholds_and_rate_limit(tmp_path, monkeypatch):
    monkeypatch.setenv("NETDOCTOR_VPN_SSH", "nwvpn")
    c, h, clock, sent = make(tmp_path, expect=())
    vpn_sample(c, clock, 79)
    h.check()
    run(h.flush())
    assert not [s for s in sent if "VPN" in s]
    clock.t += 300
    vpn_sample(c, clock, 80)
    h.check()
    run(h.flush())
    near = [s for s in sent if "near its limit" in s]
    assert len(near) == 1 and "raise" not in near[0].lower().split("raise max")[0] and "/23" in near[0]
    clock.t += 300
    vpn_sample(c, clock, 85)
    h.check()
    run(h.flush())
    assert len([s for s in sent if "near its limit" in s]) == 1        # rate limited
    clock.t += 300
    vpn_sample(c, clock, 100)
    h.check()
    run(h.flush())
    assert len([s for s in sent if "VPN full" in s]) == 1
    clock.t += 300
    vpn_sample(c, clock, 10)
    h.check()
    assert not [k for k in h.active_alerts() if k.startswith("vpn")]
    # max 255 -> effective 253: 203 is over 80%
    clock.t += 300
    vpn_sample(c, clock, 203, mx=255)
    assert [k for k, bad, _t in vpn.conditions(h.vpn()) if bad] == ["vpn-near"]


def test_vpn_monitor_down(tmp_path, monkeypatch):
    monkeypatch.setenv("NETDOCTOR_VPN_SSH", "nwvpn")
    c, h, clock, sent = make(tmp_path, expect=())
    vpn_sample(c, clock, None, ok=False)           # failures before any success: waiting, no alert
    clock.t += 3600
    h.check()
    assert h.vpn()["state"] == "waiting" and not h.active_alerts()
    vpn_sample(c, clock, 5)
    clock.t += 29 * 60
    vpn_sample(c, clock, None, ok=False)
    h.check()
    assert "vpn-down" not in h.active_alerts()
    clock.t += 2 * 60
    vpn_sample(c, clock, None, ok=False)
    h.check()
    run(h.flush())
    assert "vpn-down" in h.active_alerts() and any("VPN monitor down" in s for s in sent)
    assert "DOWN" in vpn.line(h.vpn())
    vpn_sample(c, clock, 5)
    h.check()
    assert "vpn-down" not in h.active_alerts()


def test_vpn_retention_peaks_and_report(tmp_path, monkeypatch):
    monkeypatch.setenv("NETDOCTOR_VPN_SSH", "nwvpn")
    c, h, clock, sent = make(tmp_path, expect=())
    vpn_sample(c, clock, 50)                        # will be 91 days old
    clock.t += 91 * 86400
    vpn_sample(c, clock, 20)
    assert c.db.execute("SELECT COUNT(*) FROM nw_vpn").fetchone()[0] == 1
    clock.t += 3 * 86400
    vpn_sample(c, clock, 40)
    clock.t += 3600
    vpn_sample(c, clock, 30)
    s = h.vpn()
    assert (s["clients"], s["peak_24h"], s["peak_7d"], s["effective_max"]) == (30, 40, 40, 100)
    text = hl.report_text(h.snapshot())
    assert "VPN players: 30 now, 24 h peak 40, 7 d peak 40, limit 100" in text
    clock.t += 2 * 86400                            # 24 h peak drops out
    vpn_sample(c, clock, 12)
    assert h.vpn()["peak_24h"] == 12 and h.vpn()["peak_7d"] == 40


# --- VPN game counts (nwlobby stats socket), /vpnstatus, the 24 h window -------------------------------------

sys.path.insert(0, str(ROOT / "tools" / "vpn" / "lobby"))
import nwlobby  # noqa: E402

LOBBY = {"games": 3, "players": 6, "open": 2, "full": 1, "joinable": 1, "free_slots": 3,
         "sizes": {"3": 1, "4": 2}, "builds": {"0.7.7": 2, "0.7.6": 1}}


def test_vpn_count_reads_nwlobby_stats():
    lob = nwlobby.Lobby()
    lob.handle(nwlobby.encode_announce("A", 9307, 2, 4, "0.7.7 (x)", False), "10.8.0.10", 0)
    lob.handle(nwlobby.encode_announce("B", 9307, 4, 4, "0.7.7 (x)", True), "10.8.0.11", 0)
    assert nwlobby.encode_stats() == vpn_count.LOBBY_MAGIC + bytes([vpn_count.LOBBY_VERSION, vpn_count.T_STATS])
    d = vpn_count.parse_lobby_stats(lob.stats(nwlobby.encode_stats(), 1))
    assert (d["games"], d["open"], d["full"], d["joinable"], d["free_slots"], d["builds"]) == (2, 1, 1, 1, 2, {"0.7.7": 2})
    assert vpn_count.parse_lobby_stats(b"junk") is None
    assert vpn_count.lobby(("127.0.0.1", 1), timeout=0.2) is None


def test_vpn_parse_reply_lobby():
    r = vpn.parse_reply(json.dumps({"ok": True, "clients": 9, "max_clients": 255, "pool": 253, "lobby": LOBBY}))
    assert r["lobby"]["joinable"] == 1 and r["lobby"]["sizes"] == {"3": 1, "4": 2}
    assert vpn.parse_reply('{"ok":true,"clients":9,"max_clients":255,"pool":253}')["lobby"] is None
    assert vpn.parse_reply('{"ok":true,"clients":9,"lobby":{"games":"x"}}')["lobby"] is None


def test_vpn_status_text():
    sample = {"ok": True, "clients": 9, "max_clients": 255, "pool": 253, "lobby": LOBBY}
    t = vpn.status_text(sample, {"state": "ok", "peak_24h": 15, "peak_7d": 20,
                                 "day": {"lobby_samples": 3, "games_peak": 5, "open_peak": 3, "room_share": 0.5}})
    for want in ("**9** of 253 players connected", "**3 games** listed: 2 public", "1 with a password", "1 full",
                 "**1 public game** with room right now (3 free slots)", "6 players in listed games, 3 connected",
                 "1 game up to 3, 2 games up to 4", "0.7.7 (2), 0.7.6 (1)", "Busiest: 15 players in 24 h, 20 in 7 days",
                 "up to 5 games at once (3 public), a public game with room 50% of the time"):
        assert want in t, want
    assert "Game list: unavailable" in vpn.status_text(dict(sample, lobby=None), {"state": "ok"})
    assert "no answer" in vpn.status_text({"ok": False, "error": "ssh failed (255)"}, {"state": "ok"})
    evil = dict(sample, lobby=dict(LOBBY, builds={"@everyone**<x>": 1}))
    assert "@" not in vpn.status_text(evil, {"state": "ok"}).split("Versions:")[1]


def test_vpn_daily_window(tmp_path, monkeypatch):
    monkeypatch.setenv("NETDOCTOR_VPN_SSH", "nwvpn")
    c, h, clock, sent = make(tmp_path, expect=())
    vpn.store(c, {"ok": True, "clients": 4, "max_clients": 255, "lobby": dict(LOBBY, games=1, open=1, joinable=0, players=2)}, clock.t)
    clock.t += 300
    vpn.store(c, {"ok": True, "clients": 10, "max_clients": 255, "lobby": LOBBY}, clock.t)
    clock.t += 300
    vpn.store(c, {"ok": True, "clients": 6, "max_clients": 255}, clock.t)   # lobby did not answer
    d = h.vpn()["day"]
    assert (d["samples"], d["players_peak"], d["lobby_samples"], d["games_peak"], d["open_peak"]) == (3, 10, 2, 3, 2)
    assert abs(d["games_avg"] - 2.0) < 1e-9 and d["room_share"] == 0.5
    text = hl.report_text(h.snapshot())
    assert "games 24 h: average 2.0, peak 3 (2 public at most)" in text and "room 50% of the time" in text


def test_vpnstatus_roles():
    import netdoctor_bot as nb
    role = lambda n: type("R", (), {"name": n})()
    assert nb.is_dev_or_staff(type("M", (), {"roles": [role("Dev")]})())
    assert nb.is_dev_or_staff(type("M", (), {"roles": [role("staff team")]})())
    assert not nb.is_dev_or_staff(type("M", (), {"roles": [role("Moderator"), role("hunter")]})())
    assert not nb.is_dev_or_staff(type("U", (), {})())
