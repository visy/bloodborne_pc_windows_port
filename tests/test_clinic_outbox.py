"""Outbox coalescing: the same tell / ask to one person goes out once, listing every bug."""
import asyncio
import importlib.util
import sys
import time
from pathlib import Path

DISCORD = Path(__file__).resolve().parent.parent / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(Path(__file__).parent))
import clinic_store  # noqa: E402
import clinic_fakes as F  # noqa: E402
import clinic_outbox as ob  # noqa: E402
from test_clinic_cli import js  # noqa: E402

spec = importlib.util.spec_from_file_location("netdoctor_bot", DISCORD / "netdoctor_bot.py")
bot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bot)

TEXT = "Older CPUs (AMD FX, Intel 2nd gen) are not supported."


def run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def world(tmp_path):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    forum = F.Forum(5, "bug-reports")
    ths = {n: F.Thread(300 + n, f"T{n}", forum) for n in (1, 2, 3, 4)}
    client = F.Client([forum.guild], list(ths.values()))
    client.user = F.User(1, "Iosefka")
    bugs = {1: c.new_bug("a", "forum post", 5, 301, 77), 2: c.new_bug("b", "forum post", 5, 302, 77),
            3: c.new_bug("c", "forum post", 5, 303, None), 4: c.new_bug("d", "forum post", 5, 304, 88)}
    # bug 3 has no reporter_id: the reporter is whoever posted its first log
    c.upsert_message({"message_id": 900, "author_id": 77, "author_name": "P", "created": time.time(),
                      "bug_id": bugs[3], "content": "x", "attachments": [
                          {"sha256": "a" * 64, "filename": "x.log", "kind": "log", "size": 5}]})
    ctx = bot.Ctx(c, live=True, tmp_root=tmp_path / "tmp", client=client)
    ctx.ask_delay = 0
    return c, ctx, bugs, ths


def test_identical_tells_go_out_once_with_all_ids(tmp_path):
    c, ctx, bugs, ths = world(tmp_path)
    for n in (1, 2, 3):
        c.queue_tell(bugs[n], "vlad", TEXT if n != 2 else "  " + TEXT.upper().replace(" ", "  "))
    assert run(bot.send_outbox(ctx)) == 1
    posted = [m for t in ths.values() for m in t.sent]
    assert len(posted) == 1 and len(ths[3].sent) == 1  # the newest bug's thread
    m = posted[0]
    assert m.content.count("<@77>") == 1
    assert all(bugs[n] in m.content for n in (1, 2, 3)) and bugs[4] not in m.content
    assert c.outbox_pending() == []
    for n in (1, 2):
        assert any("together" in x["text"] for x in c.notes(bugs[n]))
    assert run(bot.send_outbox(ctx)) == 0


def test_different_text_or_person_stay_separate(tmp_path):
    c, ctx, bugs, ths = world(tmp_path)
    c.queue_tell(bugs[1], "vlad", TEXT)
    c.queue_tell(bugs[2], "vlad", "Something else entirely.")
    c.queue_tell(bugs[4], "vlad", TEXT)
    assert run(bot.send_outbox(ctx)) == 3
    assert [len(ths[n].sent) for n in (1, 2, 3, 4)] == [1, 1, 0, 1]
    assert all(t.sent[0].content.count("NW-") == 0 for t in ths.values() if t.sent)


def test_same_text_again_within_window_not_resent(tmp_path, monkeypatch):
    c, ctx, bugs, ths = world(tmp_path)
    c.queue_tell(bugs[1], "vlad", TEXT)
    assert run(bot.send_outbox(ctx)) == 1
    c.queue_tell(bugs[2], "vlad", TEXT)
    assert run(bot.send_outbox(ctx)) == 0
    assert sum(len(t.sent) for t in ths.values()) == 1 and c.outbox_pending() == []
    assert any("not re-sent" in n["text"] for n in c.notes(bugs[2]))
    # past the window it goes out again
    monkeypatch.setenv("NW_TELL_WINDOW_HOURS", "0.0001")
    time.sleep(0.5)
    c.queue_tell(bugs[2], "vlad", TEXT)
    assert run(bot.send_outbox(ctx)) == 1


def test_asks_coalesce_only_with_same_text(tmp_path):
    import json
    c, ctx, bugs, ths = world(tmp_path)
    for n, need in ((1, "host log"), (2, "host log"), (3, "other thing")):
        c.queue_tell(bugs[n], "vlad", json.dumps({"bug": bugs[n], "issue": "i", "need": need}), kind="ask")
    assert run(bot.send_outbox(ctx)) == 2
    assert len(ths[2].sent) == 1 and bugs[1] in ths[2].sent[0].content and bugs[2] in ths[2].sent[0].content
    assert len(ths[3].sent) == 1 and bugs[3] in ths[3].sent[0].content and bugs[1] not in ths[3].sent[0].content


def test_cli_status_tell_reports_merge_and_warning(tmp_path):
    c, ctx, bugs, ths = world(tmp_path)
    db = tmp_path / "nd.db"
    r = js(db, "vlad", "status", bugs[1], "open", "--tell", TEXT)
    assert "tell_merge_with" not in r and "tell_warning" not in r
    r = js(db, "vlad", "status", bugs[2], "open", "--tell", TEXT)
    assert r["tell_merge_with"] == [bugs[1]]
    run(bot.send_outbox(ctx))
    r = js(db, "vlad", "status", bugs[3], "open", "--tell", TEXT)
    assert "already got this text" in r["tell_warning"]
    assert any(n["kind"] == "triage" for n in c.notes(bugs[3]))  # the per-bug history stays
