"""Iosefka's live duty in tools/discord/netdoctor_bot.py (fake Discord, no network)."""
import asyncio
import importlib.util
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(ROOT / "tests"))
import clinic_store  # noqa: E402
import clinic_fakes as F  # noqa: E402

spec = importlib.util.spec_from_file_location("netdoctor_bot", DISCORD / "netdoctor_bot.py")
bot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bot)
cd = bot.cd

LOG = b"[coop] Net: party networking on (host, as HostA, port 9307)\n"
NET_DIAG = ("**Co-op connection check**\n- Host (host)\n\n**What's wrong**\n"
            "1. **Host has no open port** (UPnP not available on the router). Guests can only get in.\n\n"
            "**What to try**\n1. Open the port.")
OK_DIAG = "**Co-op connection check**\n\nNo network problem found in these logs."


@pytest.fixture(autouse=True)
def _env(monkeypatch):
    monkeypatch.setattr(cd.media, "enough_disk", lambda *a, **k: True)
    monkeypatch.setattr(cd.media, "free_disk", lambda *a, **k: 10 * 2**30)
    monkeypatch.setattr(cd, "RETRY_DELAY", 0)


def info(path):
    return {"name": "HostA", "role": "host", "host": "HostA", "start": 600, "end": 660, "day": "d",
            "peers": {"GuestB"}}


def run(coro):
    return asyncio.run(coro)


def make(tmp_path, live=True, delete=True, diag=OK_DIAG, client=None):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    ctx = bot.Ctx(c, live=live, delete=delete, tmp_root=tmp_path / "tmp", log_info_fn=info,
                  diagnose_fn=lambda paths: diag, client=client)
    return c, ctx


def post(ch, mid=1, content="my game crashed", name="nightwatch-20261025_200000.log", minutes=0):
    return F.Msg(mid, ch, F.User(7, "SecretPlayer"), content, [F.Att(mid * 10, name, LOG)], minutes=minutes)


def test_verdict_keys_on_samples():
    assert bot.verdict_keys(NET_DIAG) == {"nat.noport"}
    assert bot.verdict_keys(OK_DIAG) == set()
    mixed = "**What's wrong**\n1. **Not network: Host counts as still in the prologue**, so no summons.\n" \
            "2. **Guest never reached the host** (x): 3 tries.\n3. Something new.\n\n**What to try**\n1. x"
    assert bot.verdict_keys(mixed) == {"other.notnet", "join.noreach", "other.unknown"}
    # the real doctor on a host log with a closed port
    p = Path(__file__).parent / "_tmp_hostlog.log"
    p.write_text("Nightwatch Co-op v0.2.3 beta 3, engine x, 2026-10-24 20:39:01\n"
                 "[coop] Net: party networking on (host, as HostA, port 9307)\n"
                 "[coop] Party: NAT: cone (no open port); no open port (cone NAT): guests join by UDP hole punching\n",
                 encoding="utf-8")
    try:
        keys = bot.verdict_keys(bot.diagnose([p], redact=True))
    finally:
        p.unlink()
    assert "nat.noport" in keys


def test_live_off_by_default(tmp_path):
    c, ctx = make(tmp_path, live=False)
    ch = F.Channel(100, "help")
    m = post(ch)
    assert run(bot.handle_log_message(m, live=True, ctx=ctx)) == "off"
    assert run(bot.dispatch_message(m, ctx)) is False
    assert c.message(1) is None and not ch.sent and not m.deleted
    assert bot.CTX is None  # the module default: nothing runs before main() builds a context


def test_live_without_delete_archives_silently(tmp_path):
    c, ctx = make(tmp_path, delete=False)
    ch = F.Channel(100, "help")
    m = post(ch)
    assert run(bot.handle_log_message(m, live=True, ctx=ctx)) == "would-replace"
    assert c.message(1)["state"] == "verified" and not ch.sent and not m.deleted


def test_bug_post_gets_bug_filed_in_one_message_and_poster_ping_only(tmp_path):
    c, ctx = make(tmp_path)
    ch = F.Channel(100, "help")
    m = post(ch, content="game crashed at the chalice")
    assert run(bot.dispatch_message(m, ctx)) is True
    assert m.deleted and len(ch.sent) == 1
    text = ch.sent[0].content
    bug = c.message(1)["bug_id"]
    assert bug in text and "other guests' logs" in text and "brought a log to my clinic" in text
    am = ch.sent[0].kw["allowed_mentions"]
    assert am.users == [m.author] and am.everyone is False and am.roles is False
    assert c.message(1)["state"] == "deleted"


def test_network_post_gets_redacted_advice(tmp_path):
    c, ctx = make(tmp_path, diag=NET_DIAG)
    ch = F.Channel(100, "help")
    m = post(ch, content="cannot join my friend")
    run(bot.dispatch_message(m, ctx))
    text = ch.sent[0].content
    assert "has no open port" in text and "what you tried" in text and m.deleted and len(ch.sent) == 1
    assert c.bug(c.message(1)["bug_id"])["tags"]["triage"] == "network"


def test_skipped_places_are_left_alone(tmp_path):
    c, ctx = make(tmp_path)
    for name in ("staff-chat", "development", "moderator-only"):
        ch = F.Channel(1, name)
        m = post(ch)
        assert run(bot.dispatch_message(m, ctx)) is False and not m.deleted and not ch.sent
    assert c.message(1) is None


def test_dm_never_deleted(tmp_path):
    c, ctx = make(tmp_path)
    dm = F.Channel(900, "dm", guild=None)
    m = F.Msg(1, dm, F.User(7), "my log", [F.Att(10, "a.log", LOG)], guild=None)
    assert run(bot.dispatch_message(m, ctx)) is False  # not "taken off": the older DM reply still runs
    assert not m.deleted and len(dm.sent) == 1 and c.message(1)["bug_id"] in dm.sent[0].content
    assert dm.sent[0].kw["allowed_mentions"].users is False


def test_thread_followups_join_the_bug(tmp_path):
    c, ctx = make(tmp_path)
    forum = F.Forum(5, "bug-reports")
    th = F.Thread(300, "Crash in Yharnam", forum)
    run(bot.dispatch_message(post(th, 1), ctx))
    bug = c.message(1)["bug_id"]
    text = F.Msg(2, th, F.User(8), "same here", [])
    shot = F.Msg(3, th, F.User(8), "screenshot", [F.Att(30, "s.png", b"\x89PNGx")])
    assert run(bot.dispatch_message(text, ctx)) is False and run(bot.dispatch_message(shot, ctx)) is False
    assert c.message(2)["bug_id"] == bug and c.message(3)["bug_id"] == bug
    assert not text.deleted and not shot.deleted


def test_raw_edit_adds_log(tmp_path):
    ch = F.Channel(100, "help")
    client = F.Client([], [ch])
    c, ctx = make(tmp_path, client=client)
    m = F.Msg(1, ch, F.User(7), "edited later", [])
    ch.messages = [m]
    payload = type("P", (), {"channel_id": 100, "message_id": 1, "guild_id": None})()
    assert run(bot.handle_raw_edit(payload, ctx)) == "known"  # no files yet
    m.attachments.append(F.Att(10, "last_session.log", LOG))  # the edit adds a log to an old, uncached message
    assert run(bot.handle_raw_edit(payload, ctx)) == "deleted"
    assert len(ch.sent) == 1 and m.deleted
    assert run(bot.handle_raw_edit(payload, ctx)) == "gone"  # a second edit event does nothing more
    assert len(ch.sent) == 1


def test_busy_message_handled_once(tmp_path):
    c, ctx = make(tmp_path)
    ch = F.Channel(100, "help")
    m = post(ch)

    async def both():
        return await asyncio.gather(bot.handle_log_message(m, live=True, ctx=ctx),
                                    bot.handle_log_message(m, live=True, ctx=ctx))
    res = run(both())
    assert set(res) <= {"busy", "deleted"} and len(ch.sent) == 1  # a duplicate event never double-posts


def test_catch_up_uses_watermark_and_never_pings(tmp_path):
    ch = F.Channel(100, "help")
    guild = ch.guild
    guild.text_channels = [ch]
    c, ctx = make(tmp_path)
    ch.messages = [post(ch, 1), post(ch, 2, content="again", name="b.log")]
    ch.messages[1].attachments[0].data = LOG + b"2"
    ch.messages[1].attachments[0].size = len(LOG) + 1
    assert run(bot.catch_up(ctx, [guild])) == 2
    assert c.watermark(100) == 2 and all(m.deleted for m in ch.messages)
    assert all(s.kw["allowed_mentions"].users is False for s in ch.sent)
    assert run(bot.catch_up(ctx, [guild])) == 0


def test_outbox_sent_once(tmp_path):
    forum = F.Forum(5, "bug-reports")
    th = F.Thread(300, "Crash", forum, archived=True)
    client = F.Client([], [th])
    c, ctx = make(tmp_path, client=client)
    bug = c.new_bug("Crash", "forum post", 5, 300)
    c.queue_tell(bug, "visy", "Fixed in 0.7.7. Your address 203.0.113.9 was never needed.")
    assert run(bot.send_outbox(ctx)) == 1
    assert run(bot.send_outbox(ctx)) == 0
    assert len(th.sent) == 1 and "203.0.113.9" not in th.sent[0].content and "Fixed in 0.7.7" in th.sent[0].content
    assert th.archived  # restored after posting
    assert th.sent[0].kw["allowed_mentions"].users is False


def test_disk_guard_warns_once_a_day(tmp_path, monkeypatch):
    staff = F.Channel(1, "staff-chat")
    guild = staff.guild
    guild.text_channels = [staff]
    c, ctx = make(tmp_path)
    monkeypatch.setattr(cd.media, "free_disk", lambda *a, **k: 100)
    assert run(bot.disk_guard(ctx, [guild])) is True
    assert run(bot.disk_guard(ctx, [guild])) is False and len(staff.sent) == 1
