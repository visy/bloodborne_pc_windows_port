"""Review fixes for Iosefka's clinic: DM/skip safety, prune cap, live retry, serialized downloads, replacement
dedup, stale messages, fail-closed skip, replaced re-archive, media kinds, reporter ping, memory/temp."""
import asyncio
import importlib.util
import hashlib
import shutil
import sqlite3
import sys
import threading
from collections import namedtuple
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(ROOT / "tests"))
import clinic_discord as cd  # noqa: E402
import clinic_store  # noqa: E402
import iosefka_cull as cull  # noqa: E402
import netdoctor_store as ns  # noqa: E402
import clinic_fakes as F  # noqa: E402

spec = importlib.util.spec_from_file_location("netdoctor_bot", DISCORD / "netdoctor_bot.py")
bot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bot)

LOG = b"[coop] Net: party networking on (host, as HostA, port 9307)\n"
FLAG = {"NW_DELETE": "1"}


@pytest.fixture(autouse=True)
def _env(monkeypatch):
    monkeypatch.setattr(cd.media, "enough_disk", lambda *a, **k: True)
    monkeypatch.setattr(cd.media, "free_disk", lambda *a, **k: 10 * 2**30)
    monkeypatch.setattr(cd, "RETRY_DELAY", 0)


def info(path):
    return {"name": "HostA", "role": "host", "host": "HostA", "start": 600, "end": 660, "day": "d", "peers": set()}


def run(coro):
    return asyncio.run(coro)


def mk(tmp_path):
    return clinic_store.Clinic(tmp_path / "nd.db")


def make_ctx(tmp_path, client=None, delete=True):
    c = mk(tmp_path)
    return c, bot.Ctx(c, live=True, delete=delete, tmp_root=tmp_path / "tmp", log_info_fn=info,
                      diagnose_fn=lambda p: "", client=client)


def logmsg(ch, mid=1, content="join fails", name="a.log", data=LOG, **kw):
    return F.Msg(mid, ch, F.User(7, "Poster"), content, [F.Att(mid * 10, name, data)], **kw)


def arch(c, m, tmp_path):
    return run(cd.archive_message(c, m, log_info_fn=info, tmpdir=tmp_path / "tmp"))


class HTTPError(Exception):
    def __init__(self, status):
        super().__init__(f"http {status}")
        self.status = status


# 1 --- DM logs are never replaced or culled -----------------------------------------------------------

def test_dm_record_is_flagged_not_cullable_and_blocked(tmp_path):
    c = mk(tmp_path)
    dm = F.Channel(50, "dm", guild=None)
    m = logmsg(dm, 1, guild=None)
    arch(c, m, tmp_path)
    assert c.message(1)["dm"] == 1 and c.message(1)["state"] == "verified"
    assert cull.cullable(c) == []
    assert run(cd.replace_and_delete(c, m, "NW-x", ping=False, delete=True, need_export=False)) == "blocked:dm"
    assert not dm.sent and not m.deleted


def test_skipped_channel_blocks_replace(tmp_path):
    c = mk(tmp_path)
    staff = F.Channel(60, "staff-chat")
    m = logmsg(staff, 1)
    arch(c, m, tmp_path)
    assert run(cd.replace_and_delete(c, m, "NW-x", ping=False, delete=True, need_export=False)) == "blocked:skipped"
    assert not staff.sent and not m.deleted


def test_forbidden_delete_does_not_abort_the_cull(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    ch.guild.text_channels = [ch]
    a, b = logmsg(ch, 1, name="a.log"), logmsg(ch, 2, name="b.log", data=LOG + b"2")
    ch.messages = [a, b]
    a.delete_error = F.Forbidden("403")
    code = run(cull.harvest([ch.guild], c, log_info_fn=info, tmpdir=tmp_path / "tmp", out=lambda x: None))
    assert code == 0
    c.record_export("E", [f["sha256"] for f in c.all_files()])
    c.confirm_export("E", [f["sha256"] for f in c.all_files()])
    out = []
    run(cull.cull(F.Client([ch.guild], [ch]), c, delay=0, out=out.append, env=FLAG))
    assert c.message(1)["state"] == "delete_forbidden" and b.deleted and c.message(2)["state"] == "deleted"
    assert any("delete_forbidden" in x for x in out)


# 2 --- prune measures the bot-memory tables only ------------------------------------------------------

def test_prune_cap_ignores_clinic_tables_and_trims_nd(tmp_path):
    c = mk(tmp_path)
    c.put_file(bytes(range(256)) * 20000, "big.bin", "other")  # 5 MB the cap must not count
    st = ns.Store(tmp_path / "nd.db")
    for i in range(20):
        st.add_log(1, i, 0, "a.log", {"name": "G", "role": "guest", "peers": ()})
    assert st.nd_bytes() < 200_000
    st.prune(max_bytes=1_000_000)
    assert len(st.logs(1)) == 20  # the clinic's 5 MB did not push the cap
    st.prune(max_bytes=ns.Store.nd_bytes(st) // 2)
    assert 0 < len(st.logs(1)) < 20
    assert c.all_files()


def test_prune_never_vacuums_without_room(tmp_path, monkeypatch):
    st = ns.Store(tmp_path / "nd.db")
    for i in range(200):
        st.put_logdata(bytes([i]) * 5000 + bytes(range(256)) * 20, f"{i}.log")
    big = st.size_bytes()
    usage = namedtuple("usage", "total used free")
    monkeypatch.setattr(ns.shutil, "disk_usage", lambda p: usage(10**12, 0, big))  # less than 2x
    st.prune(max_bytes=big // 4)
    assert st.size_bytes() == big  # rows dropped, file untouched
    assert st.nd_bytes() < big
    monkeypatch.setattr(ns.shutil, "disk_usage", lambda p: usage(10**12, 0, 10**12))
    st.prune(max_bytes=big // 4)
    assert st.size_bytes() < big


def test_safe_prune_logs_and_survives():
    class Boom:
        def prune(self, **kw):
            raise sqlite3.OperationalError("database or disk is full")
    out = []
    assert bot.safe_prune(Boom(), 1, out=out.append) is None and "prune failed" in out[0]


# 3 --- live retry -------------------------------------------------------------------------------------

def sweep_world(tmp_path):
    ch = F.Channel(100, "help")
    ch.guild.text_channels = [ch]
    client = F.Client([ch.guild], [ch])
    c, ctx = make_ctx(tmp_path, client=client)
    return ch, c, ctx


def test_catch_up_watermark_stops_before_a_failed_message_and_retries(tmp_path):
    ch, c, ctx = sweep_world(tmp_path)
    ok1, bad, ok2 = logmsg(ch, 1, name="a.log"), logmsg(ch, 2, name="b.log", data=LOG + b"2"), \
        logmsg(ch, 3, name="c.log", data=LOG + b"3")
    ch.messages = [ok1, bad, ok2]
    bad.attachments[0].fail = 99
    run(bot.catch_up(ctx, [ch.guild]))
    assert ok1.deleted and ok2.deleted and not bad.deleted
    assert c.watermark(100) == 1 and c.message(2)["state"] == "archive_failed"
    bad.attachments[0].fail = 0
    run(bot.catch_up(ctx, [ch.guild]))
    assert bad.deleted and c.message(2)["state"] == "deleted" and c.watermark(100) == 2


def test_sweep_retries_stored_messages_behind_the_watermark_and_marks_gone(tmp_path):
    ch, c, ctx = sweep_world(tmp_path)
    m = logmsg(ch, 5)
    arch(c, m, tmp_path)           # stored, never replaced
    gone = logmsg(ch, 6, name="g.log", data=LOG + b"g")
    arch(c, gone, tmp_path)
    gone.deleted = True            # removed on Discord meanwhile
    c.set_watermark(100, 99)       # history shows nothing after 99
    ch.messages = []
    ch.index = {5: m, 6: gone}
    run(bot.catch_up(ctx, [ch.guild]))
    assert m.deleted and c.message(5)["state"] == "deleted" and c.message(6)["state"] == "deleted"


def test_disk_low_stops_the_sweep_without_advancing(tmp_path, monkeypatch):
    ch, c, ctx = sweep_world(tmp_path)
    ch.messages = [logmsg(ch, 1), logmsg(ch, 2, name="b.log", data=LOG + b"2")]
    monkeypatch.setattr(cd.media, "enough_disk", lambda *a, **k: False)
    out = []
    run(bot.catch_up(ctx, [ch.guild], out=out.append))
    assert c.watermark(100) == 0 and any("disk low" in x for x in out)
    assert not any(m.deleted for m in ch.messages) and not ch.sent


# 4 --- downloads one at a time, CPU work off the loop -------------------------------------------------

def test_downloads_are_serialized_and_cpu_work_is_off_the_loop(tmp_path, monkeypatch):
    state = {"now": 0, "max": 0}

    class SlowAtt(F.Att):
        async def save(self, path):
            state["now"] += 1
            state["max"] = max(state["max"], state["now"])
            await asyncio.sleep(0.02)
            await super().save(path)
            state["now"] -= 1
    threads = []
    real = cd.media.shrink_image
    monkeypatch.setattr(cd.media, "shrink_image", lambda d: (threads.append(threading.current_thread()), real(d))[1])
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    atts = [SlowAtt(i, f"s{i}.png", b"\x89PNG" + bytes([i])) for i in range(1, 4)]
    m = F.Msg(1, ch, F.User(7), "x", atts + [SlowAtt(9, "a.log", LOG)])

    async def both():
        m2 = F.Msg(2, ch, F.User(7), "y", [SlowAtt(20, "z.log", LOG + b"z")])
        await asyncio.gather(cd.archive_message(c, m, tmpdir=tmp_path / "tmp"),
                             cd.archive_message(c, m2, tmpdir=tmp_path / "tmp"))
    run(both())
    assert state["max"] == 1
    assert threads and all(t is not threading.main_thread() for t in threads)


# 5 --- duplicate replacement -------------------------------------------------------------------------

def media_msg(ch, mid=1):
    return F.Msg(mid, ch, F.User(7), "x", [F.Att(10, "a.log", LOG), F.Att(11, "s.png", b"\x89PNGx")])


def test_replacement_carries_a_nonce(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    m = F.Msg(123456789, ch, F.User(7), "x", [F.Att(10, "a.log", LOG)])
    arch(c, m, tmp_path)
    run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False))
    assert ch.sent[0].kw["nonce"] == "123456789"


def test_only_payload_too_large_drops_the_files(tmp_path):
    for status, falls_back in ((413, True), (400, True), (500, False)):
        sub = tmp_path / str(status)
        sub.mkdir()
        c = mk(sub)
        ch = F.Channel(100, "help")
        m = media_msg(ch)
        arch(c, m, sub)
        real, n = ch.send, [0]

        async def send(content=None, _real=real, _n=n, _status=status, **kw):
            _n[0] += 1
            if kw.get("files"):
                raise HTTPError(_status)
            return await _real(content, **kw)
        ch.send = send
        if falls_back:
            assert run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False)) == "deleted"
            assert "kept in the clinic" in ch.sent[0].content and ch.sent[0].kw["nonce"]
        else:
            with pytest.raises(HTTPError):
                run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False))
            assert not ch.sent and not m.deleted


# 6 --- stale message ----------------------------------------------------------------------------------

def test_replace_refetches_and_blocks_a_message_edited_since(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    stale = F.Msg(1, ch, F.User(7), "x", [F.Att(10, "a.log", LOG)])
    arch(c, stale, tmp_path)
    fresh = F.Msg(1, ch, F.User(7), "x", [F.Att(10, "a.log", LOG), F.Att(12, "b.log", LOG + b"2")])
    res = run(cd.replace_and_delete(c, stale, "NW-a", ping=False, delete=True, need_export=False))
    assert res == "blocked:not-verified" and not fresh.deleted and not stale.deleted and not ch.sent


def test_replace_on_a_vanished_message_records_deleted(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    m = F.Msg(1, ch, F.User(7), "x", [F.Att(10, "a.log", LOG)])
    arch(c, m, tmp_path)
    m.deleted = True
    assert run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False)) == "deleted"
    assert c.message(1)["state"] == "deleted" and not ch.sent


# 7 --- skip rule fails closed -------------------------------------------------------------------------

def test_unknown_places_are_skipped():
    class Partial:  # discord.PartialMessageable: an id, no name
        id = 5
    assert cd.place_skipped(Partial())
    forum = F.Forum(5, "bug-reports")
    ok = F.Thread(9, "t", forum)
    assert not cd.place_skipped(ok)
    orphan = F.Thread(10, "t", forum)
    orphan.parent = None
    assert cd.place_skipped(orphan)  # parent unknown and the guild cannot say
    orphan.guild = type("G", (), {"get_channel": lambda self, i: forum})()
    assert not cd.place_skipped(orphan)
    staff = F.Channel(6, "staff-x")
    orphan.guild = type("G", (), {"get_channel": lambda self, i: staff})()
    assert cd.place_skipped(orphan)


def test_replace_rechecks_a_thread_with_an_unresolvable_parent(tmp_path):
    c = mk(tmp_path)
    forum = F.Forum(5, "bug-reports")
    th = F.Thread(300, "t", forum)
    m = logmsg(th, 1)
    arch(c, m, tmp_path)
    th.parent = None
    th.guild = type("G", (), {"get_channel": lambda self, i: None,
                              "fetch_channel": lambda self, i: _raise()})()
    assert run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False)) == "blocked:skipped"


async def _raise():
    raise F.NotFound("x")


# 8 --- a replaced message that gains a file ----------------------------------------------------------

def test_replaced_message_archives_a_new_attachment_and_is_then_deleted(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    m = F.Msg(1, ch, F.User(7), "x", [F.Att(10, "a.log", LOG)])
    arch(c, m, tmp_path)
    m.delete_error = RuntimeError("boom")
    with pytest.raises(RuntimeError):
        run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False))
    assert c.message(1)["state"] == "replaced"
    m.attachments.append(F.Att(12, "b.log", LOG + b"2"))
    assert run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False)) == "blocked:not-verified"
    res = arch(c, m, tmp_path)
    assert res["state"] == "replaced" and {a["id"] for a in c.message(1)["attachments"]} == {10, 12}
    assert c.message(1)["state"] == "replaced"
    m.delete_error = None
    assert run(cd.replace_and_delete(c, m, "NW-a", ping=False, delete=True, need_export=False)) == "deleted"
    assert len(ch.sent) == 1 and m.deleted


# 9 --- only pictures and videos are reposted -----------------------------------------------------------

def test_only_pictures_and_videos_are_reposted(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    m = F.Msg(1, ch, F.User(7), "x", [F.Att(10, "a.log", LOG), F.Att(11, "s.png", b"\x89PNGx"),
                                      F.Att(12, "x.bin", b"bin"), F.Att(13, "v.mp4", b"mp4")])
    rec = {"attachments": [{"id": 10, "filename": "a.log"}]}
    files, left_out = run(cd._media_files(m, rec))
    assert [f[1] for f in files] == [11, 13] and left_out


# 10 --- --tell pings the reporter only ---------------------------------------------------------------

def test_bug_stores_reporter_and_tell_pings_only_that_user(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    chatter = F.Msg(1, ch, F.User(8, "Chatty"), "same here", minutes=0)
    seed = F.Msg(2, ch, F.User(7, "Reporter"), "crash", [F.Att(20, "a.log", LOG)], minutes=1)
    ch.messages = [chatter, seed]
    res = arch(c, seed, tmp_path)
    bug = run(cd.assign_bug(c, seed, res["infos"], log_info_fn=info, tmpdir=tmp_path / "tmp"))
    assert c.bug(bug)["reporter_id"] == 7
    c.upsert_message({"message_id": 1, "bug_id": bug})
    client = F.Client([], [ch])
    c2, ctx = make_ctx(tmp_path, client=client)
    c2.queue_tell(bug, "visy", "fixed")
    ch.threads = []
    c2.update_bug(bug, thread_id=None)
    run(bot.send_outbox(ctx))
    sent = ch.sent[0]
    assert sent.content.startswith("<@7> ") and [u.id for u in sent.kw["allowed_mentions"].users] == [7]


def test_old_database_gets_the_new_columns(tmp_path):
    path = tmp_path / "old.db"
    db = sqlite3.connect(str(path))
    db.executescript("""
CREATE TABLE nw_messages (message_id INTEGER PRIMARY KEY, channel_id INTEGER, thread_id INTEGER,
    author_id INTEGER, author_name TEXT, created REAL, edited REAL, content TEXT, reply_to INTEGER,
    attachments TEXT, bug_id TEXT, state TEXT, replacement_id INTEGER, seen REAL);
CREATE TABLE nw_bugs (id TEXT PRIMARY KEY, title TEXT, source TEXT, channel_id INTEGER, thread_id INTEGER,
    status TEXT, dup_of TEXT, tags TEXT, assignee TEXT, nd_refs TEXT, created REAL, updated REAL);
INSERT INTO nw_bugs VALUES ('NW-old','t','dm',1,NULL,'new',NULL,'{}',NULL,'[]',1,1);""")
    db.commit()
    db.close()
    c = clinic_store.Clinic(path)
    assert c.bug("NW-old")["reporter_id"] is None
    new = c.new_bug("n", "channel", 1, None, 42)
    assert c.bug(new)["reporter_id"] == 42


# 11 --- memory and temp -------------------------------------------------------------------------------

def test_big_non_video_files_are_stored_with_streaming_lzma(tmp_path, monkeypatch):
    monkeypatch.setattr(cd, "STREAM_BYTES", 50)
    data = b"line of log text\n" * 20
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    m = F.Msg(1, ch, F.User(7), "x", [F.Att(10, "big.log", data), F.Att(11, "pic.png", b"\x89PNG" * 30)])
    called = []
    monkeypatch.setattr(Path, "read_bytes", lambda self: called.append(self) or b"")
    res = arch(c, m, tmp_path)
    monkeypatch.undo()
    assert not called and res["state"] == "verified"
    sha = hashlib.sha256(data).hexdigest()
    assert c.verify_file(sha) and c.get_file(sha)[1] == data and c.file_info(sha)["codec"] == "lzma"


def test_stale_download_dirs_are_removed(tmp_path):
    (tmp_path / "att_old").mkdir()
    (tmp_path / "att_old" / "f.bin").write_bytes(b"x")
    (tmp_path / "keep").mkdir()
    assert cd.clean_stale_tmp(tmp_path) == 1
    assert not (tmp_path / "att_old").exists() and (tmp_path / "keep").exists()


def test_backup_cleans_tmp_on_failure_and_runs_in_a_thread(tmp_path, monkeypatch):
    c = mk(tmp_path)
    c.put_file(b"x", "a.log", "log")
    dest = tmp_path / "nd.db.bak"
    real = sqlite3.connect
    n = [0]

    class Src:
        def __init__(self):
            self.real = real(str(tmp_path / "nd.db"))

        def execute(self, *a):
            return self.real.execute(*a)

        def backup(self, out):
            raise sqlite3.OperationalError("disk full")

        def close(self):
            self.real.close()

    def fake(*a, **k):
        n[0] += 1
        return Src() if n[0] == 1 else real(*a, **k)
    monkeypatch.setattr(clinic_store.sqlite3, "connect", fake)
    with pytest.raises(sqlite3.OperationalError):
        c.backup(dest)
    assert not list(tmp_path.glob("*.tmp")) and not dest.exists()
    monkeypatch.undo()
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(1) as ex:
        assert ex.submit(c.backup, dest).result() is True
    assert clinic_store.Clinic(dest).all_files()


# 12 --- voice/stage chats, DM acknowledgement -----------------------------------------------------------

def test_walk_includes_voice_and_stage_text_chats():
    calls = []
    voice, stage = F.Channel(1, "Lobby", calls), F.Channel(2, "Stage", calls)
    hidden = F.Channel(3, "staff-voice", calls)
    text = F.Channel(4, "help", calls)
    g = F.Guild([text], [], voice_channels=[voice, hidden], stage_channels=[stage])

    async def names():
        return [p.name async for p in cd.walk_places(g)]
    assert run(names()) == ["help", "Lobby", "Stage"]


def test_dm_is_acknowledged_only_when_deleting_is_on(tmp_path):
    for delete, sent in ((False, 0), (True, 1)):
        sub = tmp_path / str(delete)
        sub.mkdir()
        c, ctx = make_ctx(sub, delete=delete)
        dm = F.Channel(50, "dm", guild=None)
        m = logmsg(dm, 1, guild=None)
        assert run(bot.handle_log_message(m, live=True, ctx=ctx)) == "dm"
        assert len(dm.sent) == sent and not m.deleted


# 13 --- the CDN may serve pictures with other bytes --------------------------------------------------

def test_image_size_mismatch_is_accepted_but_logs_stay_strict(tmp_path):
    c = mk(tmp_path)
    ch = F.Channel(100, "help")
    img = F.Att(10, "s.png", b"\x89PNG" + b"x" * 96, size=140)  # served 100 of a listed 140
    short = F.Att(11, "t.png", b"\x89PNG" + b"x" * 6, size=140)  # truncated: < 50%
    empty = F.Att(12, "e.png", b"", size=140)
    log = F.Att(13, "a.log", LOG, size=len(LOG) + 5)
    vid = F.Att(14, "v.mp4", b"v" * 100, size=130)
    m = F.Msg(1, ch, F.User(7), "x", [img, short, empty, log, vid])
    arch(c, m, tmp_path)
    by_id = {a["id"]: a for a in c.message(1)["attachments"]}
    assert by_id[10]["sha256"] and by_id[10]["served_size"] == 100 and by_id[10]["size"] == 100
    assert by_id[14]["sha256"] and by_id[14]["served_size"] == 100
    for bad in (11, 12, 13):
        assert by_id[bad]["sha256"] is None and "size mismatch" in by_id[bad]["error"]
    assert c.message(1)["state"] == "archive_failed"
