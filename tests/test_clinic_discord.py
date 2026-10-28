"""tools/discord/clinic_discord.py: walk, archive, bug assignment, replace-then-delete (fake Discord)."""
import asyncio
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(ROOT / "tests"))
import clinic_discord as cd  # noqa: E402
import clinic_store  # noqa: E402
import clinic_fakes as F  # noqa: E402

LOG = b"[coop] Net: party networking on (host, as HostA, port 9307)\n[coop] Party: public address 198.51.100.7:9307\n"


@pytest.fixture(autouse=True)
def _env(monkeypatch):
    monkeypatch.setattr(cd.media, "enough_disk", lambda *a, **k: True)
    monkeypatch.setattr(cd, "RETRY_DELAY", 0)


def fake_info(path):
    return {"name": "HostA", "role": "host", "host": "HostA", "start": 600, "end": 660, "day": "d", "peers": set()}


def run(coro):
    return asyncio.run(coro)


def mk(tmp_path):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    calls = []
    ch = F.Channel(100, "help", calls)
    return c, ch, calls


def logmsg(ch, mid=1, content="join fails", minutes=0, **kw):
    return F.Msg(mid, ch, F.User(7), content, [F.Att(mid * 10, "nightwatch-20261025_200000.log", LOG)],
                 minutes=minutes, **kw)


def test_walk_skips_staff_and_development():
    calls = []
    staff = F.Channel(1, "staff-chat", calls)
    staff.threads = [F.Thread(11, "t", staff)]
    staff.private_archived = [F.Thread(12, "secret", staff)]
    dev = F.Channel(2, "development", calls)
    mod = F.Channel(3, "moderator-only", calls)  # skipped too (owner decision)
    mod.private_archived = [F.Thread(31, "old private", mod)]
    mod.public_archived = [F.Thread(32, "old public", mod)]
    gen = F.Channel(6, "general", calls)
    gen.private_archived = [F.Thread(61, "old private", gen)]
    help_ = F.Channel(4, "help", calls)
    help_.threads = [F.Thread(41, "active", help_)]
    forum = F.Forum(5, "bug-reports", calls)
    forum.public_archived = [F.Thread(51, "post", forum)]
    g = F.Guild([staff, dev, mod, help_, gen], [forum])

    async def collect():
        return [p.id async for p in cd.walk_places(g)]
    ids = run(collect())
    assert set(ids) == {4, 41, 51, 6, 61}
    assert 5 not in ids  # forums have no messages of their own


def test_snapshot_attachments_counted(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = F.Msg(1, ch, F.User(7), "forwarded", [])
    snap = type("Snap", (), {"attachments": [F.Att(5, "last_session.log", LOG)]})()
    m.message_snapshots = [snap]
    res = run(cd.archive_message(c, m, log_info_fn=fake_info, tmpdir=tmp_path / "tmp"))
    assert res["state"] == "verified" and res["removable"] and len(res["infos"]) == 1
    assert c.message(1)["attachments"][0]["filename"] == "last_session.log"


def test_failed_download_blocks_delete(tmp_path):
    c, ch, calls = mk(tmp_path)
    m = logmsg(ch)
    m.attachments[0].fail = 99
    res = run(cd.archive_message(c, m, log_info_fn=fake_info, tmpdir=tmp_path / "tmp"))
    assert res["state"] == "archive_failed"
    bug = c.new_bug("x", "channel cluster", 100, None)
    out = run(cd.replace_and_delete(c, m, bug, ping=False, delete=True, need_export=False))
    assert out == "blocked:not-verified" and not m.deleted and not ch.sent
    # a later sweep retries and succeeds
    m.attachments[0].fail = 0
    assert run(cd.archive_message(c, m, log_info_fn=fake_info, tmpdir=tmp_path / "tmp"))["state"] == "verified"


def test_size_mismatch_is_a_failure(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    m.attachments[0].size = len(LOG) + 5  # partial download
    assert run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))["state"] == "archive_failed"
    assert not list((tmp_path / "tmp").iterdir())  # temp files cleaned


def test_disk_low_stops_before_download(tmp_path, monkeypatch):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    monkeypatch.setattr(cd.media, "enough_disk", lambda *a, **k: False)
    with pytest.raises(cd.DiskLow):
        run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    assert m.attachments[0].saves == 0 and c.message(1) is None


def test_archive_is_idempotent_and_skips_verified(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    assert m.attachments[0].saves == 1


def test_replace_posts_before_delete(tmp_path):
    c, ch, calls = mk(tmp_path)
    m = logmsg(ch, content="join fails, my address is 203.0.113.9")
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    bug = c.new_bug("x", "channel cluster", 100, None)
    out = run(cd.replace_and_delete(c, m, bug, ping=False, delete=True, need_export=False))
    assert out == "deleted"
    assert [x[0] for x in calls] == ["send", "delete"]
    assert "203.0.113.9" not in ch.sent[0].content and bug in ch.sent[0].content
    assert c.message(1)["state"] == "deleted" and c.message(1)["replacement_id"] == ch.sent[0].id


def test_dry_run_changes_nothing(tmp_path):
    c, ch, calls = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    assert run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=False, need_export=False)) == "would-replace"
    assert not calls and c.message(1)["state"] == "verified"


def test_replace_reopens_archived_thread(tmp_path):
    c, _, calls = mk(tmp_path)
    parent = F.Channel(200, "bug-reports", calls)
    th = F.Thread(300, "post", parent, calls, archived=True, locked=True)
    m = logmsg(th)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=False))
    kinds = [x[0] for x in calls]
    assert kinds == ["edit", "send", "delete", "edit"]
    assert calls[0][2] == {"archived": False, "locked": False}
    assert calls[-1][2] == {"locked": True, "archived": True}
    assert th.archived and th.locked


def test_no_ping_in_cull_and_poster_only_when_live(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=False))
    am = ch.sent[0].kw["allowed_mentions"]
    assert am.users is False and am.everyone is False and am.roles is False
    m2 = logmsg(ch, mid=2)
    run(cd.archive_message(c, m2, tmpdir=tmp_path / "tmp"))
    run(cd.replace_and_delete(c, m2, "NW-1", ping=True, delete=True, need_export=False))
    am2 = ch.sent[1].kw["allowed_mentions"]
    assert am2.users == [m2.author] and am2.everyone is False and am2.roles is False


def test_need_export_blocks(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    sha = c.message(1)["attachments"][0]["sha256"]
    assert run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=True)) == "blocked:not-exported"
    c.record_export("EX-1", [sha])
    assert run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=True)) == "blocked:not-exported"
    c.confirm_export("EX-1", [sha])
    assert run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=True)) == "deleted"


def test_edit_adding_unarchived_file_blocks(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    m.attachments.append(F.Att(99, "second.log", LOG + b"more\n"))  # edit adds a file after archiving
    assert run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=False)) == "blocked:not-verified"


def test_resume_after_replacement_and_after_delete(tmp_path):
    c, ch, calls = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    m.delete_error = RuntimeError("boom")  # crash between post and delete
    with pytest.raises(RuntimeError):
        run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=False))
    assert c.message(1)["state"] == "replaced" and len(ch.sent) == 1
    m.delete_error = F.NotFound("gone")  # the delete went through before the crash: 404 is done
    assert run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=False)) == "deleted"
    assert len(ch.sent) == 1 and c.message(1)["state"] == "deleted"


def test_media_note_when_a_picture_does_not_fit(tmp_path):
    c, ch, _ = mk(tmp_path)
    big = F.Att(22, "huge.png", b"x")
    m = F.Msg(1, ch, F.User(7), "see", [F.Att(20, "a.log", LOG), F.Att(21, "shot.png", b"y"), big])
    m.guild.filesize_limit = 5
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    big.size = 10  # larger than the guild's upload cap now (the stored copy is already verified)
    run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=False))
    sent = ch.sent[0]
    assert cd.MEDIA_NOTE in sent.content and [f[2] for f in sent.kw["files"]] == ["shot.png"]


def test_extra_joins_the_replacement_or_splits(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    run(cd.replace_and_delete(c, m, "NW-1", ping=False, delete=True, need_export=False, extra="Thank you."))
    assert len(ch.sent) == 1 and ch.sent[0].content.endswith("Thank you.")
    m2 = logmsg(ch, mid=2)
    run(cd.archive_message(c, m2, tmpdir=tmp_path / "tmp"))
    run(cd.replace_and_delete(c, m2, "NW-1", ping=False, delete=True, need_export=False, extra="y" * 3500))
    assert all(len(s.content) <= 2000 for s in ch.sent) and len(ch.sent) == 4


def test_zip_with_logs_is_removable_with_inner_rows(tmp_path):
    import io
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("last_session.log", LOG)
    c, ch, _ = mk(tmp_path)
    m = F.Msg(1, ch, F.User(7), "zip", [F.Att(5, "saves.zip", buf.getvalue())])
    res = run(cd.archive_message(c, m, log_info_fn=fake_info, tmpdir=tmp_path / "tmp"))
    assert res["removable"] and res["state"] == "verified" and len(res["infos"]) == 1
    assert len(c.message(1)["attachments"][0]["inner_shas"]) == 1


def test_assign_bug_thread_and_forum(tmp_path):
    c, _, calls = mk(tmp_path)
    forum = F.Forum(5, "bug-reports", calls)
    th = F.Thread(300, "Chalice crash near 203.0.113.4", forum, calls)
    m1, m2 = logmsg(th, 1), logmsg(th, 2)
    for m in (m1, m2):
        run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    b1 = run(cd.assign_bug(c, m1, []))
    b2 = run(cd.assign_bug(c, m2, []))
    assert b1 == b2 and c.bug(b1)["source"] == "forum post" and "203.0.113.4" not in c.bug(b1)["title"]


def test_assign_bug_channel_cluster_and_same_logs_join(tmp_path):
    c, ch, _ = mk(tmp_path)
    ch.messages = []
    a = logmsg(ch, 1, "my game cannot join", minutes=0)
    run(cd.archive_message(c, a, tmpdir=tmp_path / "tmp"))
    ch.messages = [F.Msg(2, ch, F.User(7), "it says timeout", minutes=5)]
    bug = run(cd.assign_bug(c, a, [], log_info_fn=fake_info, tmpdir=tmp_path / "tmp"))
    assert c.message(2)["bug_id"] == bug and c.message(2)["state"] == "kept"  # the poster's follow-up joined
    # the same log posted again in another channel joins the same bug
    other = F.Channel(101, "general", ch.calls)
    again = logmsg(other, 3, "same log", minutes=600)
    run(cd.archive_message(c, again, tmpdir=tmp_path / "tmp"))
    assert run(cd.assign_bug(c, again, [], log_info_fn=fake_info, tmpdir=tmp_path / "tmp")) == bug


def test_assign_bug_dm_per_day(tmp_path):
    c, _, _ = mk(tmp_path)
    dm = F.Channel(900, "dm", guild=None)
    a = F.Msg(1, dm, F.User(7), "log", [F.Att(1, "a.log", LOG)], guild=None)
    b = F.Msg(2, dm, F.User(7), "log2", [F.Att(2, "b.log", LOG + b"x")], minutes=30, guild=None)
    for m in (a, b):
        run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    assert run(cd.assign_bug(c, a, [])) == run(cd.assign_bug(c, b, []))


def _save_zip():
    import io
    import zipfile
    buf = io.BytesIO()
    names = [f"SPRJ0005/userdata{i:04d}" for i in range(3)] + [
        "SPRJ0005/backup0000", "SPRJ0005/userdata0000.bak", "SPRJ0005/userdata0000_Hunter (2)"]
    with zipfile.ZipFile(buf, "w") as z:
        for n in names:
            z.writestr(n, b"\0" * 64)
    return buf.getvalue()


def test_save_zip_is_archived_as_saves_and_replied_as_saves(tmp_path):
    c, ch, calls = mk(tmp_path)
    m = F.Msg(1, ch, F.User(7), "my saves", [F.Att(5, "SPRJ0005.zip", _save_zip())])
    res = run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    assert res["removable"] and res["state"] == "verified"
    att = c.message(1)["attachments"][0]
    assert len(att["inner_files"]) == 6 and {f["kind"] for f in att["inner_files"]} == {"save"}
    assert all(c.file_info(f["sha256"])["kind"] == "save" and c.verify_file(f["sha256"]) for f in att["inner_files"])
    bug = c.new_bug("x", "channel cluster", 100, None)
    assert run(cd.replace_and_delete(c, m, bug, ping=False, delete=True, need_export=False)) == "deleted"
    text = ch.sent[0].content
    assert "memories of their hunt" in text and "a log" not in text and "keep them safe" in text
    assert c.bug(bug)["tags"]["has_saves"] == "yes"


def test_log_message_gets_no_has_saves_tag(tmp_path):
    c, ch, _ = mk(tmp_path)
    m = logmsg(ch)
    run(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"))
    bug = c.new_bug("x", "channel cluster", 100, None)
    run(cd.replace_and_delete(c, m, bug, ping=False, delete=True, need_export=False))
    assert "has_saves" not in c.bug(bug)["tags"] and "brought a log" in ch.sent[0].content
