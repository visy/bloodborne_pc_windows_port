"""tools/discord/iosefka_cull.py: harvest, verify, report, starter-test, cull, announce (fake Discord)."""
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
import iosefka_cull as cull  # noqa: E402
import clinic_fakes as F  # noqa: E402

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


def world(tmp_path, n=2):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    calls = []
    forum = F.Forum(5, "bug-reports", calls)
    th = F.Thread(300, "Cannot join the party", forum, calls)
    ch = F.Channel(100, "help", calls)
    user = F.User(7, "SecretPlayer")
    th.messages = [F.Msg(1, th, user, "my log, address 203.0.113.9", [F.Att(10, "a.log", LOG)], minutes=0),
                   F.Msg(2, th, user, "and a shot", [F.Att(11, "s.png", b"\x89PNGx")], minutes=1)]
    ch.messages = [F.Msg(3, ch, user, "bug here", [F.Att(12, "b.log", LOG + b"2")], minutes=0)]
    forum.threads = [th]
    guild = F.Guild([ch], [forum])
    return c, guild, th, ch, calls


def harvest(c, guild, tmp_path, out=None):
    out = out if out is not None else []
    code = run(cull.harvest([guild], c, log_info_fn=info, tmpdir=tmp_path / "tmp", out=out.append))
    return code, out


def test_harvest_stores_and_is_idempotent(tmp_path):
    c, guild, th, ch, _ = world(tmp_path)
    code, out = harvest(c, guild, tmp_path)
    assert code == 0 and any("place Cannot join" in x for x in out)
    assert c.message(1)["state"] == "verified" and c.message(1)["bug_id"] == c.message(2)["bug_id"]
    assert c.message(3)["bug_id"] and c.message(3)["bug_id"] != c.message(1)["bug_id"]
    saves = [a.saves for m in th.messages + ch.messages for a in m.attachments]
    n_bugs = len(c.bugs())
    harvest(c, guild, tmp_path)
    assert [a.saves for m in th.messages + ch.messages for a in m.attachments] == saves  # nothing downloaded again
    assert len(c.bugs()) == n_bugs
    assert not list((tmp_path / "tmp").iterdir())  # temp files deleted


def test_harvest_retries_failed_download(tmp_path):
    c, guild, th, ch, _ = world(tmp_path)
    th.messages[0].attachments[0].fail = 99
    code, out = harvest(c, guild, tmp_path)
    assert code == 0 and c.message(1)["state"] == "archive_failed"
    th.messages[0].attachments[0].fail = 0
    harvest(c, guild, tmp_path)
    assert c.message(1)["state"] == "verified"


def test_harvest_disk_low_stops_cleanly(tmp_path, monkeypatch):
    c, guild, th, ch, _ = world(tmp_path)
    monkeypatch.setattr(cd.media, "enough_disk", lambda *a, **k: False)
    code, out = harvest(c, guild, tmp_path)
    assert code == 3 and any("disk low, resume later" in x for x in out)
    assert c.message(1) is None  # nothing half done
    monkeypatch.setattr(cd.media, "enough_disk", lambda *a, **k: True)
    assert harvest(c, guild, tmp_path)[0] == 0 and c.message(1)["state"] == "verified"


def test_verify_counts_and_detects_corruption(tmp_path):
    c, guild, *_ = world(tmp_path)
    harvest(c, guild, tmp_path)
    out = []
    assert cull.verify(c, out.append) == 0
    sha = c.message(1)["attachments"][0]["sha256"]
    c.db.execute("UPDATE nw_files SET blob=? WHERE sha256=?", (b"junk", sha))
    c.db.commit()
    assert cull.verify(c, out.append) == 1 and any(x.startswith("FAIL") for x in out)


def test_cull_requires_flag(tmp_path):
    c, guild, th, ch, calls = world(tmp_path)
    harvest(c, guild, tmp_path)
    client = F.Client([guild], [th, ch])
    out = []
    assert run(cull.cull(client, c, delay=0, out=out.append, env={})) == 2
    assert not calls and not th.messages[0].deleted


def confirm_all(c):
    c.record_export("EX-1", [f["sha256"] for f in c.all_files()])
    c.confirm_export("EX-1", [f["sha256"] for f in c.all_files()])


def test_cull_needs_confirmed_export_then_deletes_oldest_first(tmp_path):
    c, guild, th, ch, calls = world(tmp_path)
    harvest(c, guild, tmp_path)
    client = F.Client([guild], [th, ch])
    out = []
    run(cull.cull(client, c, delay=0, out=out.append, env=FLAG))
    assert all("blocked:not-exported" in x for x in out if ": " in x and "cull:" not in x) and not th.messages[0].deleted
    confirm_all(c)
    out = []
    assert run(cull.cull(client, c, delay=0, out=out.append, env=FLAG)) == 0
    assert th.messages[0].deleted and ch.messages[0].deleted and not th.messages[1].deleted  # the picture stays
    kinds = [x[0] for x in calls]
    assert kinds.count("send") == 2 and kinds.count("delete") == 2
    assert all(kinds[i] == "send" for i in range(0, 4, 2))  # send then delete, each time


def test_cull_limit(tmp_path):
    c, guild, th, ch, calls = world(tmp_path)
    harvest(c, guild, tmp_path)
    confirm_all(c)
    client = F.Client([guild], [th, ch])
    run(cull.cull(client, c, limit=1, delay=0, out=lambda x: None, env=FLAG))
    assert sum(m.deleted for m in th.messages + ch.messages) == 1


def test_cull_resumes_each_step(tmp_path):
    c, guild, th, ch, calls = world(tmp_path)
    harvest(c, guild, tmp_path)
    confirm_all(c)
    client = F.Client([guild], [th, ch])
    first = th.messages[0]
    # crash 1: replacement posted, the delete fails -> the run stops on the unexpected error
    first.delete_error = RuntimeError("boom")
    with pytest.raises(RuntimeError):
        run(cull.cull(client, c, delay=0, out=lambda x: None, env=FLAG))
    assert c.message(1)["state"] == "replaced" and len(th.sent) == 1
    # rerun: one replacement in total, the delete now happens
    first.delete_error = None
    run(cull.cull(client, c, delay=0, out=lambda x: None, env=FLAG))
    assert len(th.sent) == 1 and first.deleted and c.message(1)["state"] == "deleted"
    # crash 2: deleted on Discord but never recorded -> 404 on fetch is treated as done, nothing is posted
    second = ch.messages[0]
    c.set_state(3, "replaced", 555)
    second.deleted = True
    sent_before = len(ch.sent)
    out = []
    run(cull.cull(client, c, delay=0, out=out.append, env=FLAG))
    assert c.message(3)["state"] == "deleted" and len(ch.sent) == sent_before


def test_report_has_no_names(tmp_path):
    c, guild, *_ = world(tmp_path)
    harvest(c, guild, tmp_path)
    p = tmp_path / "report.txt"
    assert cull.report(c, p) == 0
    text = p.read_text(encoding="utf-8")
    assert "SecretPlayer" not in text and "203.0.113.9" not in text and "<@7>" in text
    assert "2 message(s)" in text and "a.log" in text and not c.message(1)["state"] == "deleted"


def test_starter_test(tmp_path):
    c, guild, th, ch, calls = world(tmp_path)
    th.messages.insert(0, F.Msg(300, th, F.User(8), "starter with log", [F.Att(9, "x.log", LOG)]))
    client = F.Client([guild], [th, ch])
    out = []
    assert run(cull.starter_test(client, 300, out.append, env={})) == 2
    assert run(cull.starter_test(client, 300, out.append, env=FLAG)) == 0
    assert "starter-delete keeps thread: yes" in out and th.messages[0].deleted


def test_announce_needs_yes_and_pins(tmp_path):
    _, guild, th, ch, calls = world(tmp_path)
    pinned = []

    async def pin():
        pinned.append(1)
    orig = ch.send

    async def send(text, **kw):
        m = await orig(text, **kw)
        m.pin, m.jump_url = pin, "https://discord.invalid/m/1"
        return m
    ch.send = send
    out = []
    assert run(cull.announce([guild], ["help"], dry_run=False, yes=False, out=out.append)) == 2 and not ch.sent
    assert run(cull.announce([guild], ["help"], dry_run=True, yes=False, out=out.append)) == 0 and not ch.sent
    assert "clinic" in out[-1].lower()
    assert run(cull.announce([guild], ["help"], dry_run=False, yes=True, out=out.append)) == 0
    assert len(ch.sent) == 1 and pinned and ch.sent[0].kw["allowed_mentions"].users is False
    assert out[-1] == "help: https://discord.invalid/m/1"


def test_harvest_forbidden_place_is_counted_not_fatal(tmp_path):
    c, guild, th, ch, _ = world(tmp_path)
    locked = F.Channel(101, "secrets", [])
    locked.forbidden = True
    guild.text_channels.insert(0, locked)
    code, out = harvest(c, guild, tmp_path)
    assert code == 0 and "no access: secrets" in out and "1 places without access" in out[-1]
    assert c.message(1)["state"] == "verified"


def test_announce_everyone_only_for_named_channel(tmp_path):
    _, guild, th, ch, calls = world(tmp_path)
    news = F.Channel(102, "announcements", [])
    news.is_news = lambda: True
    guild.text_channels.append(news)
    published = []
    for c in (ch, news):
        orig = c.send

        async def send(text, _o=orig, **kw):
            m = await _o(text, **kw)

            async def pin():
                pass

            async def publish():
                published.append(1)
            m.pin, m.publish, m.jump_url = pin, publish, "u"
            return m
        c.send = send
    out = []
    code = run(cull.announce([guild], ["announcements", "help"], dry_run=False, yes=True,
                             everyone={"announcements"}, out=out.append))
    assert code == 0
    assert news.sent[0].content.startswith("@everyone ") and news.sent[0].kw["allowed_mentions"].everyone is True
    assert not ch.sent[0].content.startswith("@everyone") and ch.sent[0].kw["allowed_mentions"].everyone is False
    assert published == [1]


# --- runs: consecutive messages of one author for one bug get one note ------------------------------------

def run_world(tmp_path, between=None, ids=(1, 2, 3)):
    c = clinic_store.Clinic(tmp_path / "nd.db")
    calls = []
    ch = F.Channel(100, "help", calls)
    user = F.User(7, "SecretPlayer")
    msgs = [F.Msg(ids[0], ch, user, "first log 203.0.113.9", [F.Att(10, "a.log", LOG)], minutes=0),
            F.Msg(ids[1], ch, user, "second log", [F.Att(11, "b.log", LOG + b"2"), F.Att(14, "s.png", b"\x89PNGx")],
                  minutes=1),
            F.Msg(ids[2], ch, user, "third log", [F.Att(12, "c.log", LOG + b"3")], minutes=2)]
    if between:
        msgs.insert(between[0], between[1](ch))
    ch.messages = msgs
    guild = F.Guild([ch], [])
    ch.guild = guild
    for m in msgs:
        m.guild = guild
    harvest(c, guild, tmp_path)
    confirm_all(c)
    return c, guild, ch, calls, F.Client([guild], [ch])


def other_msg(ch):
    return F.Msg(2, ch, F.User(8, "Other"), "me too", [F.Att(20, "o.png", b"\x89PNGo")], minutes=1)


def test_run_one_note_three_deletes(tmp_path):
    c, guild, ch, calls, client = run_world(tmp_path)
    assert len({c.message(i)["bug_id"] for i in (1, 2, 3)}) == 1
    assert [len(r) for r in cull.cull_runs(c)] == [3]
    out = []
    assert run(cull.cull(client, c, delay=0, out=out.append, env=FLAG)) == 0
    assert [x[0] for x in calls] == ["send", "delete", "delete", "delete"]
    assert len(ch.sent) == 1 and "brought 3 logs" in ch.sent[0].content
    assert all(m.deleted for m in ch.messages)
    rids = {c.message(i)["replacement_id"] for i in (1, 2, 3)}
    assert rids == {ch.sent[0].id} and all(c.message(i)["state"] == "deleted" for i in (1, 2, 3))
    assert ch.sent[0].kw["nonce"] == "1" and ch.sent[0].kw["files"] == [("file", 14, "s.png")]
    assert "203.0.113.9" not in ch.sent[0].content and "first log" in ch.sent[0].content
    p = tmp_path / "r.txt"
    c.set_state(1, "verified")
    c.set_state(2, "verified")
    c.set_state(3, "verified")
    cull.report(c, p)
    assert "run of 3 messages" in p.read_text(encoding="utf-8")


def test_run_split_by_other_author(tmp_path):
    c, guild, ch, calls, client = run_world(tmp_path, between=(1, other_msg), ids=(1, 3, 4))
    assert [[r["message_id"] for r in x] for x in cull.cull_runs(c)] == [[1], [3, 4]]
    run(cull.cull(client, c, delay=0, out=lambda x: None, env=FLAG))
    assert len(ch.sent) == 2 and "brought" in ch.sent[1].content and "brought 2" in ch.sent[1].content
    assert "brought" not in ch.sent[0].content.split(".")[0].replace("brought a log", "")
    assert not [m for m in ch.messages if m.author.id == 8][0].deleted


def test_run_split_by_blocked_message(tmp_path):
    c, guild, ch, calls, client = run_world(tmp_path)
    c.db.execute("UPDATE nw_files SET blob=? WHERE sha256=?", (b"junk", c.message(2)["attachments"][0]["sha256"]))
    c.db.commit()
    out = []
    run(cull.cull(client, c, delay=0, out=out.append, env=FLAG))
    assert any("2 " in x and "blocked:not-verified" in x for x in out)
    assert ch.messages[0].deleted and not ch.messages[1].deleted and ch.messages[2].deleted
    assert len(ch.sent) == 2
    assert not any("brought 2 logs" in m.content for m in ch.sent)


def test_run_resume_reuses_replacement(tmp_path):
    c, guild, ch, calls, client = run_world(tmp_path)
    ch.messages[1].delete_error = RuntimeError("boom")  # first delete ok, second fails after the note
    with pytest.raises(RuntimeError):
        run(cull.cull(client, c, delay=0, out=lambda x: None, env=FLAG))
    assert len(ch.sent) == 1 and ch.messages[0].deleted and not ch.messages[1].deleted
    assert c.message(2)["state"] == "replaced" and c.message(3)["state"] == "replaced"
    ch.messages[1].delete_error = None
    run(cull.cull(client, c, delay=0, out=lambda x: None, env=FLAG))
    assert len(ch.sent) == 1  # no second note
    assert all(m.deleted for m in ch.messages) and c.message(3)["replacement_id"] == ch.sent[0].id


def test_replacement_many_fits_2000():
    voice = cd.voice
    texts = ["a long wall of words " * 80, "second " * 300, "", "third log text", "x" * 5000]
    out = voice.replacement_many("<@7>", "NW-0000001", texts, "(pictures/video kept in the clinic)")
    assert len(out) <= 2000 and "brought 5 logs" in out and "third log text" in out
    assert out.count("\n> ") + out.startswith("> ") >= 3 and out.endswith("(pictures/video kept in the clinic)")
    assert voice.replacement_many("<@7>", "NW-0000001", ["only"]) == voice.replacement("<@7>", "NW-0000001", "only")
    assert "203.0.113.9" not in voice.replacement_many("<@7>", "NW-1", ["ip 203.0.113.9", "b"])
