"""tools/discord/clinic_store.py: the clinic archive (nw_* tables) next to the netdoctor memory."""
import importlib.util
import sqlite3
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"


def _load(name):
    s = importlib.util.spec_from_file_location(name, DISCORD / f"{name}.py")
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


cs = _load("clinic_store")
ns = _load("netdoctor_store")

DAY = 86400


def _clinic(tmp_path):
    return cs.Clinic(tmp_path / "nd.db")


def test_put_file_dedups_and_verifies(tmp_path):
    c = _clinic(tmp_path)
    data = b"[coop] Net: party networking on\n" * 200
    a = c.put_file(data, "nightwatch-20261024_204859.log", "log")
    b = c.put_file(data, "copy.log", "log")
    assert a == b
    assert c.db.execute("SELECT COUNT(*) FROM nw_files").fetchone()[0] == 1
    assert c.verify_file(a)
    assert c.get_file(a) == ("nightwatch-20261024_204859.log", data, "lzma")
    c.db.execute("UPDATE nw_files SET blob=? WHERE sha256=?", (b"garbage", a))
    assert not c.verify_file(a)
    assert not c.verify_file("0" * 64)


def test_image_stored_copy_keeps_original_key(tmp_path):
    c = _clinic(tmp_path)
    sha = c.put_file(b"BIG" * 100, "shot.png", "image", stored=b"small", codec="webp85")
    assert c.get_file(sha) == ("shot.png", b"small", "webp85")
    assert c.verify_file(sha)


def test_nd_prune_never_touches_nw_tables(tmp_path):
    c = _clinic(tmp_path)
    old = time.time() - 400 * DAY
    c.put_file(b"old log", "a.log", "log")
    c.db.execute("UPDATE nw_files SET created=?", (old,))
    bug = c.new_bug("Old case", "channel", 1, None)
    c.db.execute("UPDATE nw_bugs SET created=?, updated=?", (old, old))
    c.add_note(bug, "vlad", "note", "old note")
    c.db.execute("UPDATE nw_notes SET at=?", (old,))
    c.upsert_message({"message_id": 5, "created": old, "bug_id": bug, "content": "hi"})
    st = ns.Store(tmp_path / "nd.db")
    st.put_logdata(b"x" * 100, "n.log")
    st.add_analysis("ND-aaaaaaa1", 1, 2, 3, [], "text")
    st.add_analysis("ND-aaaaaaa2", 1, 2, 3, [], "text")
    st.db.execute("UPDATE analyses SET created=? WHERE ref='ND-aaaaaaa1'", (time.time() - 200 * DAY,))
    st.db.execute("UPDATE analyses SET created=? WHERE ref='ND-aaaaaaa2'", (time.time() - 100 * DAY,))
    st.db.commit()
    st.prune()
    assert st.get_analysis("ND-aaaaaaa1") is None
    assert st.get_analysis("ND-aaaaaaa2") is not None
    c2 = _clinic(tmp_path)
    assert len(c2.all_files()) == 1
    assert c2.bug(bug) and c2.notes(bug) and c2.message(5)


def test_prune_ceiling_3gib():
    assert ns.ceiling(10 * 2**30) == 3 * 2**30
    assert ns.ceiling(2**20) == 2**20


def test_merge_bugs(tmp_path):
    c = _clinic(tmp_path)
    keep = c.new_bug("Keep", "channel", 1, None)
    dup = c.new_bug("Dup", "thread", 1, 77)
    c.upsert_message({"message_id": 10, "bug_id": dup, "created": 1.0, "content": "m"})
    c.add_note(dup, "vlad", "note", "n")
    c.update_bug(dup, tags={"area": "boss"})
    c.merge_bugs(keep, dup)
    assert [m["message_id"] for m in c.messages_for_bug(keep)] == [10]
    assert any(n["text"] == "n" for n in c.notes(keep))
    assert c.bug(dup)["status"] == "duplicate" and c.bug(dup)["dup_of"] == keep
    assert c.bug(keep)["tags"] == {"area": "boss"}
    assert c.bug_for_thread(77) is None


def test_bug_for_files(tmp_path):
    c = _clinic(tmp_path)
    a = c.put_file(b"log a", "a.log", "log")
    b = c.put_file(b"log b", "b.log", "log")
    bug = c.new_bug("Case", "channel", 1, None)
    c.upsert_message({"message_id": 1, "bug_id": bug, "attachments": [{"sha256": a, "filename": "a.log"}]})
    assert c.bug_for_files([a]) == bug
    assert c.bug_for_files([a, b]) is None
    c.upsert_message({"message_id": 2, "bug_id": bug, "attachments": [{"sha256": b, "filename": "b.log"}]})
    assert c.bug_for_files([b, a]) == bug
    assert c.bug_for_files([]) is None


def test_export_confirm(tmp_path):
    c = _clinic(tmp_path)
    c.record_export("EX1", ["s1", "s2"])
    assert not c.exported("s1")
    assert c.confirm_export("EX1", ["s1"]) == 1
    assert c.exported("s1") and not c.exported("s2")
    assert c.confirm_export("OTHER", ["s2"]) == 0
    assert not c.exported("s2")
    c.record_export("EX2", ["s1"])           # a later export does not unconfirm
    assert c.exported("s1")


def test_changes_since(tmp_path):
    c = _clinic(tmp_path)
    before = c.new_bug("Before", "channel", 1, None)
    assert c.mark_seen("vlad") == 0.0
    time.sleep(0.02)
    after = c.new_bug("After", "channel", 1, None)
    c.add_note(before, "visy", "share", "look at this", to="vlad")
    c.add_note(before, "visy", "share", "for someone else", to="other")
    prev = c.mark_seen("vlad")
    assert prev > 0
    got = c.changes_since(prev, who="vlad")
    assert [b["id"] for b in got["bugs"]] == [after]
    assert [n["text"] for n in got["notes"]] == ["look at this"]


def test_bugs_filters_and_tags(tmp_path):
    c = _clinic(tmp_path)
    a = c.new_bug("Lamp does not light", "thread", 1, 5)
    b = c.new_bug("Boss join fails", "thread", 1, 6)
    c.update_bug(a, tags={"area": "map", "feature": ["lamps", "Chalice"]}, status="open")
    c.update_bug(a, tags={"version": "0.2.3"})
    c.update_bug(a, tags={"version": None})
    assert c.bug(a)["tags"] == {"area": "map", "feature": ["lamps", "Chalice"]}
    assert [x["id"] for x in c.bugs(status="open")] == [a]
    assert [x["id"] for x in c.bugs(tag="feature=chalice")] == [a]
    assert [x["id"] for x in c.bugs(tag=["feature=chalice", "area=boss"])] == []
    assert [x["id"] for x in c.bugs(q="join")] == [b]
    c.add_note(a, "vlad", "note", "fog gate sync")
    assert [x["id"] for x in c.bugs(q="FOG")] == [a]
    assert [x["id"] for x in c.bugs(exclude_status=["open"])] == [b]
    assert c.bug_for_thread(5) == a


def test_message_upsert_keeps_state_and_stills(tmp_path):
    c = _clinic(tmp_path)
    c.upsert_message({"message_id": 9, "content": "v1", "attachments": [{"sha256": "x"}]})
    c.set_state(9, "replaced", 44)
    c.upsert_message({"message_id": 9, "content": "v2"})
    m = c.message(9)
    assert (m["content"], m["state"], m["replacement_id"], m["attachments"]) == (
        "v2", "replaced", 44, [{"sha256": "x"}])
    assert c.add_stills("v" * 64, [(0.0, b"RIFFxxxxWEBPdata"), (4.0, b"\xff\xd8jpeg")]) == 2
    names = [c.file_info(s["sha256"])["filename"] for s in c.stills("v" * 64)]
    assert names[0].endswith(".webp") and names[1].endswith(".jpg")


def test_outbox_watermark_backup(tmp_path):
    c = _clinic(tmp_path)
    i = c.queue_tell("NW-1a2b3c4", "vlad", "hello")
    assert [r["id"] for r in c.outbox_pending()] == [i]
    c.outbox_sent(i)
    assert c.outbox_pending() == []
    assert c.watermark(3) == 0
    c.set_watermark(3, 99)
    assert c.watermark(3) == 99
    assert c.backup(tmp_path / "nd.db.bak")
    n = sqlite3.connect(str(tmp_path / "nd.db.bak")).execute("SELECT COUNT(*) FROM nw_outbox").fetchone()[0]
    assert n == 1
    assert c.db.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
