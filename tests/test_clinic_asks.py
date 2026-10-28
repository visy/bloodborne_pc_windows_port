"""Retest asks after a #releases post, the waiting-logs nudge / close, the release command, NW local sync."""
import asyncio
import importlib.util
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(ROOT / "tests"))
import clinic_asks as asks  # noqa: E402
import clinic_changes as cc  # noqa: E402
import clinic_store  # noqa: E402
import clinic_fakes as F  # noqa: E402
from test_clinic_cli import js, run as cli_run, seed  # noqa: E402

spec = importlib.util.spec_from_file_location("netdoctor_bot", DISCORD / "netdoctor_bot.py")
bot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bot)
cd = bot.cd
sync_spec = importlib.util.spec_from_file_location("cl_sync_nw", DISCORD / "cl_sync_nw.py")
nw = importlib.util.module_from_spec(sync_spec)
sync_spec.loader.exec_module(nw)

DAY = 86400


def arun(coro):
    return asyncio.run(coro)


async def _coro(x):
    return x


def test_versions_in_posts():
    assert asks.versions_in("Co-op v0.7.7 is out, thanks") == ["0.7.7"]
    assert asks.versions_in("Nightwatch 0.2.6 and 0.2.5 beta 4 / 0.2.5-beta.4") == ["0.2.6", "0.2.5 beta 4"]
    assert asks.versions_in("see 10.1.2.3.4 or 203.0.113.9") == []  # not a release number
    assert asks.versions_in("no numbers here, only 2026") == []


def world(tmp_path, source="forum post", status="fixed", reporter=77, item_status="done", track="coop", ver="0.7.7"):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    forum = F.Forum(5, "bug-reports")
    th = F.Thread(300, "Crash", forum, archived=True)
    client = F.Client([forum.guild], [th])
    client.user = F.User(1, "Iosefka")
    bug = c.new_bug("Crash on join", source, 5, 300, reporter)
    c.update_bug(bug, status=status)
    ch = cc.Changes(c)
    ch.set_version(track, ver, status="released", who="visy")
    ch.add(track, ver, "fixed", "Crash on join fixed", who="visy", status=item_status, bugs=[bug])
    ctx = bot.Ctx(c, live=True, tmp_root=tmp_path / "tmp", client=client)
    ctx.ask_delay = 0
    return c, ctx, bug, th, ch


def test_retest_ask_once_per_version(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path)
    res = arun(asks.send_retest_asks(ctx, "Co-op 0.7.7 is out"))
    assert res == [(bug, "0.7.7", "sent")]
    assert len(th.sent) == 1
    txt = th.sent[0].content
    assert txt.startswith("<@77> ") and "0.7.7" in txt and bug in txt
    assert th.sent[0].kw["allowed_mentions"].users[0].id == 77
    assert th.archived  # restored
    assert any(n["kind"] == "retest-asked" and n["text"].startswith("0.7.7:") for n in c.notes(bug))
    assert arun(asks.send_retest_asks(ctx, "0.7.7 again")) == [] and len(th.sent) == 1
    # a later version that also lists the bug asks again (once)
    cc.Changes(c).set_version("coop", "0.7.8", status="released", who="visy")
    cc.Changes(c).add("coop", "0.7.8", "fixed", "Crash again", who="visy", status="done", bugs=[bug])
    assert len(arun(asks.send_retest_asks(ctx, "0.7.8"))) == 1 and len(th.sent) == 2


def test_retest_dry_run_posts_nothing(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path)
    assert arun(asks.send_retest_asks(ctx, "0.7.7", dry_run=True)) == [(bug, "0.7.7", "would-ask")]
    assert th.sent == [] and not [n for n in c.notes(bug) if n["kind"] == "retest-asked"]


@pytest.mark.parametrize("kw", [dict(source="local"), dict(status="open"), dict(reporter=None),
                                dict(item_status="planned")])
def test_retest_skips(tmp_path, kw):
    c, ctx, bug, th, _ = world(tmp_path, **kw)
    assert arun(asks.send_retest_asks(ctx, "0.7.7")) == [] and th.sent == []


def test_retest_other_track_version_not_asked(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path, track="nw", ver="0.2.6")
    assert arun(asks.send_retest_asks(ctx, "Co-op 0.7.7")) == []
    assert len(arun(asks.send_retest_asks(ctx, "Nightwatch 0.2.6"))) == 1


def test_retest_replies_to_replacement_note(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path)
    note = F.Msg(900, th, F.User(1, "Iosefka"), "note")
    c.upsert_message({"message_id": 55, "channel_id": 5, "thread_id": 300, "author_id": 77, "bug_id": bug,
                      "state": "deleted", "replacement_id": 900, "attachments": []})
    arun(asks.send_retest_asks(ctx, "0.7.7"))
    assert th.sent[-1].kw["reference"] is note


def test_retest_skipped_and_gone_places(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path)
    th.parent.name = "staff-bugs"
    assert arun(asks.send_retest_asks(ctx, "0.7.7"))[0][2] == "skip:skipped" and th.sent == []
    assert not [n for n in c.notes(bug) if n["kind"] == "retest-asked"]  # retried if the rule changes


def test_retest_gone_thread_is_skipped(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path)
    ctx.client._ch.clear()
    assert arun(asks.send_retest_asks(ctx, "0.7.7"))[0][2] == "skip:NotFound"
    assert not [n for n in c.notes(bug) if n["kind"] == "retest-asked"]


def test_retest_dm_bug_gets_dm(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path, source="dm")
    dm = F.Channel(8, "dm")
    ctx.client.fetch_user = lambda uid: _coro(dm if uid == 77 else None)
    assert arun(asks.send_retest_asks(ctx, "0.7.7"))[0][2] == "sent"
    assert len(dm.sent) == 1 and "<@" not in dm.sent[0].content and th.sent == []


def test_release_post_live_and_catch_up(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path)
    rel = F.Channel(60, "releases", guild=th.guild)
    th.guild.id = asks.GUILD_ID
    th.guild.text_channels = [rel]
    ctx.guild_id = asks.GUILD_ID
    msg = F.Msg(1, rel, F.User(9, "Dev"), "Release v0.7.7: chalice fixes", guild=th.guild)
    other = F.Channel(61, "general", guild=th.guild)
    arun(bot.dispatch_message(F.Msg(2, other, F.User(9, "Dev"), "v0.7.7", guild=th.guild), ctx))
    assert th.sent == []
    arun(bot.dispatch_message(msg, ctx))
    assert len(th.sent) == 1
    arun(bot.dispatch_message(msg, ctx))
    assert len(th.sent) == 1  # once only
    # catch-up sees a fresh post the bot missed
    ch = cc.Changes(c)
    bug2 = c.new_bug("Other", "forum post", 5, 300, 78)
    c.update_bug(bug2, status="fixed")
    ch.add("coop", "0.7.7", "fixed", "Other fixed", who="visy", status="done", bugs=[bug2])
    fresh = F.Msg(3, rel, F.User(9, "Dev"), "0.7.7 notes", guild=th.guild)
    import datetime as dt
    fresh.created_at = dt.datetime.now(dt.timezone.utc)
    rel.messages = [fresh]
    arun(bot.catch_up(ctx, [th.guild]))
    assert len(th.sent) == 2 and "<@78>" in th.sent[1].content


def test_old_release_post_ignored_by_catch_up(tmp_path):
    c, ctx, bug, th, _ = world(tmp_path)
    rel = F.Channel(60, "releases", guild=th.guild)
    th.guild.id = asks.GUILD_ID
    th.guild.text_channels = [rel]
    ctx.guild_id = asks.GUILD_ID
    old = F.Msg(3, rel, F.User(9, "Dev"), "0.7.7", guild=th.guild)
    old.created_at = old.created_at.replace(year=2025)
    rel.messages = [old]
    arun(bot.catch_up(ctx, [th.guild]))
    assert th.sent == []


# --- waiting-logs ---

def waiting(tmp_path, idle_days, source="forum post", reporter=77):
    tmp_path.mkdir(parents=True, exist_ok=True)
    c = clinic_store.Clinic(tmp_path / "nd.db")
    forum = F.Forum(5, "bug-reports")
    th = F.Thread(300, "Crash", forum, archived=False)
    client = F.Client([forum.guild], [th])
    bug = c.new_bug("Crash on join", source, 5, 300, reporter)
    c.update_bug(bug, status="waiting-logs")
    old = time.time() - idle_days * DAY
    c.db.execute("UPDATE nw_bugs SET created=?, activity=?, updated=? WHERE id=?", (old, old, old, bug))
    c.db.commit()
    ctx = bot.Ctx(c, live=True, tmp_root=tmp_path / "tmp", client=client)
    ctx.ask_delay = 0
    return c, ctx, bug, th


def test_waiting_nudge_once_then_close(tmp_path):
    c, ctx, bug, th = waiting(tmp_path, 10)
    assert arun(asks.sweep_waiting(ctx)) == ([], [])
    c.db.execute("UPDATE nw_bugs SET created=?, activity=?", (time.time() - 15 * DAY,) * 2)
    c.db.commit()
    assert arun(asks.sweep_waiting(ctx)) == ([bug], [])
    assert len(th.sent) == 1 and th.sent[0].content.startswith("<@77> ")
    assert arun(asks.sweep_waiting(ctx)) == ([], []) and len(th.sent) == 1  # one nudge only
    c.db.execute("UPDATE nw_bugs SET created=?, activity=?", (time.time() - 31 * DAY,) * 2)
    c.db.commit()
    assert arun(asks.sweep_waiting(ctx)) == ([], [bug])
    assert c.bug(bug)["status"] == "closed" and len(th.sent) == 1
    assert any(n["text"] == "closed: no logs for 30 days; reopens when logs arrive" for n in c.notes(bug))


def test_waiting_dry_run_and_local(tmp_path):
    c, ctx, bug, th = waiting(tmp_path / "a", 40)
    assert arun(asks.sweep_waiting(ctx, dry_run=True)) == ([], [bug])
    assert c.bug(bug)["status"] == "waiting-logs"
    c2, ctx2, bug2, th2 = waiting(tmp_path / "b", 40, source="local")
    assert arun(asks.sweep_waiting(ctx2)) == ([], []) and c2.bug(bug2)["status"] == "waiting-logs"


def test_closed_waiting_bug_reopens_on_new_material(tmp_path):
    c, ctx, bug, th = waiting(tmp_path, 40)
    arun(asks.sweep_waiting(ctx))
    assert c.bug(bug)["status"] == "closed"
    m = F.Msg(500, th, F.User(77, "Reporter"), "here are the logs you wanted, from the host", minutes=5)
    assert cd.link_text(c, m)
    assert c.bug(bug)["status"] == "open"
    assert any("new report after closed" in n["text"] for n in c.notes(bug))


# --- release command ---

def test_cl_release(tmp_path):
    db, *_ = seed(tmp_path)
    js(db, "vlad", "cl", "version", "coop", "0.7.7", "--status", "current")
    js(db, "vlad", "cl", "version", "coop", "0.7.8", "--status", "planned")
    d = js(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Done thing", "--status", "done")["id"]
    p = js(db, "vlad", "cl", "add", "coop", "0.7.7", "added", "Late thing")["id"]
    w = js(db, "vlad", "cl", "add", "coop", "0.7.7", "added", "Busy thing", "--status", "in-progress")["id"]
    dry = js(db, "visy", "cl", "release", "coop", "0.7.7", "--dry-run")
    assert dry["dry_run"] and sorted(dry["moved"]) == sorted([p, w]) and dry["next"] == "0.7.8"
    assert js(db, "vlad", "cl", "versions", "--track", "coop")[0]["status"] == "current"
    out = js(db, "visy", "cl", "release", "coop", "0.7.7")
    assert out["released"] == "0.7.7" and out["next"] == "0.7.8" and "Done thing" in out["notes"]
    assert "Late thing" not in out["notes"]
    vs = {v["version"]: v for v in js(db, "vlad", "cl", "versions", "--track", "coop")}
    assert vs["0.7.7"]["status"] == "released" and vs["0.7.7"]["released"]
    assert vs["0.7.8"]["status"] == "current"
    assert {i["id"] for i in js(db, "vlad", "cl", "list", "--track", "coop", "--version", "0.7.8")} == {p, w}
    assert [i["id"] for i in js(db, "vlad", "cl", "list", "--track", "coop", "--version", "0.7.7")] == [d]
    show = js(db, "vlad", "cl", "show", p)
    assert any(n["kind"] == "move" for n in show["notes"])


def test_cl_release_next_created_and_errors(tmp_path):
    db, *_ = seed(tmp_path)
    js(db, "vlad", "cl", "version", "nw", "0.2.6", "--status", "current")
    js(db, "vlad", "cl", "add", "nw", "0.2.6", "added", "Todo")
    assert cli_run(db, "vlad", "cl", "release", "nw", "0.2.6")[0] == 2  # no next version, none given
    out = js(db, "vlad", "cl", "release", "nw", "0.2.6", "--next", "0.2.7")
    assert out["next"] == "0.2.7" and len(out["moved"]) == 1
    assert cli_run(db, "vlad", "cl", "release", "nw", "9.9.9")[0] == 1
    assert cli_run(db, "vlad", "cl", "release", "xx", "0.2.6")[0] == 2


# --- NW local sync ---

def test_sync_local(tmp_path, capsys):
    repo = tmp_path / "repo"
    repo.mkdir()
    full = dict(os.environ, GIT_AUTHOR_NAME="d", GIT_AUTHOR_EMAIL="d@x", GIT_COMMITTER_NAME="d",
                GIT_COMMITTER_EMAIL="d@x")
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    (repo / "VERSION").write_text("0.2.6\n")
    (repo / "a.txt").write_text("x")
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True, env=full)
    subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", "0.2.6: Fix the lamp"], check=True, env=full)
    db = tmp_path / "nd.db"
    assert nw.main([str(repo), "--local", str(db)]) == 0
    assert "applied=2 skipped=0 locked=0" in capsys.readouterr().out
    c = clinic_store.Clinic(db)
    items = cc.Changes(c).items(track="nw")
    assert len(items) == 1 and items[0]["author"] == "nw-sync"
    assert nw.main([str(repo), "--local", str(db)]) == 0
    assert len(cc.Changes(c).items(track="nw")) == 1
