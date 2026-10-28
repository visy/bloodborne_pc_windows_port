"""Late details land in the same bug (explicit id, reply, same session, reopen, duplicates, 24 h follow-up),
and Iosefka's doubts are left for the triage pass (fake Discord, no network)."""
import asyncio
import importlib.util
import io
import json
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(ROOT / "tests"))
import clinic_store  # noqa: E402
import clinic_fakes as F  # noqa: E402
import iosefka_cli  # noqa: E402

spec = importlib.util.spec_from_file_location("netdoctor_bot", DISCORD / "netdoctor_bot.py")
bot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bot)
cd = bot.cd

LOG = b"[coop] Net: party networking on (host, as HostA, port 9307)\n"
DAYS3 = 3 * 24 * 60


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


def make(tmp_path):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    ctx = bot.Ctx(c, live=True, delete=True, tmp_root=tmp_path / "tmp", log_info_fn=info,
                  diagnose_fn=lambda paths: "No network problem found in these logs.")
    return c, ctx


def log_post(ch, mid, user=7, content="game crashed at the chalice", minutes=0, data=None):
    data = data or LOG + str(mid).encode()
    return F.Msg(mid, ch, F.User(user, f"P{user}"), content, [F.Att(mid * 10, f"n{mid}.log", data)], minutes=minutes)


def text_post(ch, mid, content, user=8, minutes=0, reply_to=None, guild="auto"):
    return F.Msg(mid, ch, F.User(user, f"P{user}"), content, [], minutes=minutes, reply_to=reply_to, guild=guild)


def first_bug(tmp_path, **kw):
    c, ctx = make(tmp_path)
    ch = F.Channel(100, "help")
    run(bot.dispatch_message(log_post(ch, 1, **kw), ctx))
    return c, ctx, ch, c.message(1)["bug_id"]


def other_day(ctx, day="z"):
    ctx.log_info_fn = lambda p: {**info(p), "day": day}


def test_explicit_id_links(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    other = F.Channel(101, "chat")
    m = text_post(other, 2, f"more details for {bug.upper()}: it happens after the lamp", minutes=DAYS3)
    assert run(bot.dispatch_message(m, ctx)) is False and not m.deleted
    rec = c.message(2)
    assert rec["bug_id"] == bug and rec["state"] == "kept" and "lamp" in rec["content"]
    assert m.reactions == [] and not other.sent  # silent: no reaction, no post
    # the first existing id wins; a log with an id joins too and gets the usual note
    log = log_post(other, 3, user=9, content=f"NW-0000000 and {bug}", minutes=DAYS3 + 5)
    assert run(bot.dispatch_message(log, ctx)) is True
    assert c.message(3)["bug_id"] == bug and bug in other.sent[0].content and "added to" in other.sent[0].content
    # a DM works as well
    dm = F.Channel(900, "dm", guild=None)
    d = text_post(dm, 4, f"about {bug}: it was the second lamp", guild=None)
    run(bot.dispatch_message(d, ctx))
    assert c.message(4)["bug_id"] == bug and c.message(4)["dm"] == 1


def test_reply_to_note_links(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    note = ch.sent[0]
    assert c.message(1)["replacement_id"] == note.id
    r = text_post(ch, 2, "it also froze", user=8, minutes=DAYS3, reply_to=note.id)
    run(bot.dispatch_message(r, ctx))
    assert c.message(2)["bug_id"] == bug
    # a reply to a stored message of the bug (the original, long deleted) joins as well
    r2 = text_post(ch, 3, "yes, and again", user=9, minutes=DAYS3 + 9, reply_to=1)
    run(bot.dispatch_message(r2, ctx))
    assert c.message(3)["bug_id"] == bug
    # a reply to something unrelated does not
    r3 = text_post(ch, 4, "lol", user=9, reply_to=99999)
    run(bot.dispatch_message(r3, ctx))
    assert c.message(4) is None


def test_same_session_days_later_joins(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    other = F.Channel(101, "chat")
    late = log_post(other, 2, user=9, content="my side of it", minutes=DAYS3)
    assert run(bot.dispatch_message(late, ctx)) is True
    assert c.message(2)["bug_id"] == bug and len(c.bugs()) == 1
    assert "added to" in other.sent[0].content and bug in other.sent[0].content
    assert any(n["kind"] == "auto-link" for n in c.notes(bug))
    # another day's log of the same people is another session
    other_day(ctx, "e")
    run(bot.dispatch_message(log_post(other, 3, user=10, minutes=2 * DAYS3), ctx))
    assert len(c.bugs()) == 2


def test_reopen_fixed_bug(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    for old in ("fixed", "closed"):
        c.update_bug(bug, status=old)
        mid = 10 if old == "fixed" else 11
        m = text_post(ch, mid, f"{bug} is back", minutes=DAYS3 + mid)
        run(bot.dispatch_message(m, ctx))
        assert c.bug(bug)["status"] == "open"
        n = [x for x in c.notes(bug) if x["kind"] == "status"][-1]
        assert n["who"] == "iosefka" and n["text"].startswith(f"new report after {old}: https://discord.com/")
        assert str(mid) in n["text"]
    c.update_bug(bug, status="waiting-logs")  # other statuses are left alone
    run(bot.dispatch_message(text_post(ch, 12, f"{bug} again", minutes=DAYS3 + 20), ctx))
    assert c.bug(bug)["status"] == "waiting-logs"


def test_duplicate_forwards(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    dup = c.new_bug("same thing", "channel cluster", 100, None, 5)
    c.update_bug(dup, status="duplicate", dup_of=bug)
    c.update_bug(bug, status="fixed")
    m = text_post(ch, 2, f"{dup} still happens", minutes=DAYS3)
    run(bot.dispatch_message(m, ctx))
    assert c.message(2)["bug_id"] == bug and c.bug(bug)["status"] == "open"
    assert c.bug(dup)["status"] == "duplicate"


def cli(tmp_path, *args, who="visy"):
    out = io.BytesIO()
    code = iosefka_cli.main(list(args), who, out=out, db=tmp_path / "nd.db")
    return code, out.getvalue().decode()


def test_new_lists_updated_bugs(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    code, text = cli(tmp_path, "new")
    first = json.loads(text)
    assert [b["id"] for b in first["bugs"]] == [bug] and first["updated_bugs"] == []
    assert first["triage_pending"] == 1
    time.sleep(0.05)
    run(bot.dispatch_message(text_post(ch, 2, f"details for {bug}", minutes=DAYS3), ctx))
    code, text = cli(tmp_path, "new")
    second = json.loads(text)
    assert second["bugs"] == [] and [b["id"] for b in second["updated_bugs"]] == [bug]
    code, text = cli(tmp_path, "new", "--text")
    assert "updated" not in text
    time.sleep(0.05)
    run(bot.dispatch_message(text_post(ch, 3, f"and {bug}", minutes=DAYS3 + 1), ctx))
    code, text = cli(tmp_path, "new", "--text")
    assert f"updated {bug}" in text and "triage pending: 1" in text


def test_text_followup_24h(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    m = text_post(ch, 2, "forgot to say: it was on the second floor", user=7, minutes=120)
    run(bot.dispatch_message(m, ctx))
    assert c.message(2)["bug_id"] == bug and m.reactions == [] and len(ch.sent) == 1
    assert any(n["kind"] == "auto-link" and "followup" in n["text"] for n in c.notes(bug))
    stranger = text_post(ch, 3, "me too", user=8, minutes=130)
    run(bot.dispatch_message(stranger, ctx))
    assert c.message(3) is None
    late = text_post(ch, 4, "one more thing", user=7, minutes=120 + 25 * 60)
    run(bot.dispatch_message(late, ctx))  # past 24 h of the bug's last message
    assert c.message(4) is None
    chain = text_post(ch, 5, "and this happened twice there", user=7, minutes=120 + 10 * 60)  # 24 h of the previous follow-up
    run(bot.dispatch_message(chain, ctx))
    assert c.message(5)["bug_id"] == bug


def test_live_bug_pending_and_unsure(tmp_path):
    c, ctx = make(tmp_path)
    ch = F.Channel(100, "help")
    run(bot.dispatch_message(log_post(ch, 1, content=""), ctx))
    b = c.bug(c.message(1)["bug_id"])
    assert b["status"] == "new" and b["tags"]["triage"] == "pending" and b["tags"]["unsure"] == ["no-text"]
    other_day(ctx)
    run(bot.dispatch_message(log_post(ch, 3, user=11, content="cannot join, and the game crashes", minutes=2000), ctx))
    b3 = c.bug(c.message(3)["bug_id"])
    assert b3["tags"]["triage"] == "pending" and "network-or-bug" in b3["tags"]["unsure"]
    # a clear report carries no doubt
    other_day(ctx, "y")
    run(bot.dispatch_message(log_post(ch, 4, user=12, content="stuck in a wall", minutes=3000), ctx))
    assert "unsure" not in c.bug(c.message(4)["bug_id"])["tags"]
    # two candidate bugs named: which-bug on the chosen one
    m = text_post(ch, 5, f"{b3['id']} or {b['id']}?", minutes=4000)
    run(bot.dispatch_message(m, ctx))
    assert "which-bug" in c.bug(b3["id"])["tags"]["unsure"]
    # an archive nobody can read
    other_day(ctx, "x")
    z = F.Msg(6, ch, F.User(13), "see zip", [F.Att(60, "stuff.7z", b"7z junk")], minutes=5000)
    run(bot.dispatch_message(z, ctx))
    assert "unreadable-file" in c.bug(c.message(6)["bug_id"])["tags"]["unsure"]


def test_move_message(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    other_day(ctx)
    run(bot.dispatch_message(log_post(ch, 2, user=9, content="another one", minutes=3000), ctx))
    other = c.message(2)["bug_id"]
    assert other != bug and len(c.files_for_bug(other)) == 1
    code, text = cli(tmp_path, "move", "2", bug)
    assert code == 0 and json.loads(text) == {"ok": True, "message": 2, "from": other, "to": bug}
    assert c.message(2)["bug_id"] == bug and len(c.files_for_bug(bug)) == 2 and not c.files_for_bug(other)
    assert any("moved message 2" in n["text"] for n in c.notes(bug)) and c.notes(other)[-1]["text"].endswith(bug)
    assert cli(tmp_path, "move", "777", bug)[0] == 1
    assert cli(tmp_path, "move", "2", "NW-0000000")[0] == 1
    code, text = cli(tmp_path, "title", bug, "Lamp 203.0.113.9 stays dark")
    assert code == 0 and c.bug(bug)["title"] == "Lamp <public IP> stays dark"


def test_followup_skips_chat_and_stops_after_five(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    for i, chat in enumerate(("Okay, thank you", "i will wait", "😃", "thx!")):
        m = text_post(ch, 10 + i, chat, user=7, minutes=60 + i)
        run(bot.dispatch_message(m, ctx))
        assert c.message(10 + i) is None and m.reactions == []  # chat: not gathered, no reaction
    for i in range(7):
        m = text_post(ch, 20 + i, f"detail {i}: the lamp stays dark after the boss", user=7, minutes=70 + i)
        run(bot.dispatch_message(m, ctx))
        assert m.reactions == []
    joined = [i for i in range(7) if (c.message(20 + i) or {}).get("bug_id") == bug]
    assert joined == [0, 1, 2, 3, 4] and c.followups(bug) == 5
    # a reply or an explicit id still joins after the cap
    m = text_post(ch, 30, f"one more for {bug}: it was the second lamp", user=7, minutes=80)
    run(bot.dispatch_message(m, ctx))
    assert c.message(30)["bug_id"] == bug


def test_triage_queue(tmp_path):
    c, ctx, ch, bug = first_bug(tmp_path)
    other_day(ctx)
    run(bot.dispatch_message(log_post(ch, 2, user=9, content="second", minutes=500), ctx))
    second = c.message(2)["bug_id"]
    code, text = cli(tmp_path, "triage-queue")
    q = json.loads(text)
    assert [b["id"] for b in q["bugs"]] == [bug, second] and q["auto_links"] == []
    run(bot.dispatch_message(text_post(ch, 3, "extra words about the lamp", user=7, minutes=130), ctx))
    code, text = cli(tmp_path, "triage-queue")
    q = json.loads(text)
    assert [n["bug_id"] for n in q["auto_links"]] == [bug] and "followup" in q["auto_links"][0]["text"]
    assert cli(tmp_path, "tag", bug, "triage=done")[0] == 0
    code, text = cli(tmp_path, "triage-queue")
    q = json.loads(text)
    assert [b["id"] for b in q["bugs"]] == [second] and q["auto_links"] == []


def test_unreadable_marks_unsure(tmp_path):
    import zipfile
    c, ctx = make(tmp_path)
    ch = F.Channel(100, "help")
    z = F.Msg(6, ch, F.User(13), "see rar", [F.Att(60, "stuff.rar", b"Rar!\x1a\x07 junk")], minutes=0)
    run(bot.dispatch_message(z, ctx))
    assert "unreadable-file" in c.bug(c.message(6)["bug_id"])["tags"]["unsure"]
    # a readable zip without logs is not "unreadable" (and is not removable)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("shot.png", b"x")
    other_day(ctx, "q")
    ok = F.Msg(7, ch, F.User(14), "just a picture zip", [F.Att(70, "pics.zip", buf.getvalue())], minutes=3000)
    run(bot.dispatch_message(ok, ctx))
    rec = c.message(7)
    assert rec["attachments"][0]["inner"] == ["shot.png"] and not rec.get("bug_id")
    # a zip with a log inside a nested zip is stored with its inner log and no unsure
    buf2 = io.BytesIO()
    with zipfile.ZipFile(buf2, "w") as zf:
        zf.writestr("inner.zip", _zip_of({"last_session.log": LOG}))
    other_day(ctx, "r")
    nz = F.Msg(8, ch, F.User(15), "nested", [F.Att(80, "n.zip", buf2.getvalue())], minutes=6000)
    run(bot.dispatch_message(nz, ctx))
    a = c.message(8)["attachments"][0]
    assert [f["name"] for f in a["inner_files"]] == ["inner.zip/last_session.log"]
    assert "unreadable-file" not in (c.bug(c.message(8)["bug_id"])["tags"].get("unsure") or [])


def _zip_of(members):
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for n, d in members.items():
            z.writestr(n, d)
    return buf.getvalue()
