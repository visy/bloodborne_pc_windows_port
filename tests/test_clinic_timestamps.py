"""Original timestamps: bug `reported` (earliest message time) and item `dated` (when the change happened),
kept apart from `filed` / `added` (when Iosefka got them)."""
import importlib.util
import io
import json
import sqlite3
import sys
import tarfile
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_clinic_cli import cli, cs, js, run  # noqa: E402

DISCORD = Path(__file__).resolve().parents[1] / "tools" / "discord"


def _load(name):
    s = importlib.util.spec_from_file_location(name, DISCORD / f"{name}.py")
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


cc = _load("clinic_changes")
nw = _load("cl_sync_nw")
coop = _load("cl_import_coop")
DAY = 86400


def ts(s):
    d = datetime.fromisoformat(s.replace("Z", "+00:00"))
    return (d if d.tzinfo else d.replace(tzinfo=timezone.utc)).timestamp()


def old_bug(c, title, days, extra=()):
    """A bug filed now whose messages are `days` (and `extra`) days old."""
    b = c.new_bug(title, "thread", 1, zlib.crc32(title.encode()))
    for i, d in enumerate((days, *extra)):
        c.upsert_message({"message_id": zlib.crc32(f"{title}{i}".encode()), "author_name": "P",
                          "created": time.time() - d * DAY, "bug_id": b, "content": "x"})
    return b


# bugs

def test_store_reported_computed(tmp_path):
    c = cs.Clinic(tmp_path / "nd.db")
    b = old_bug(c, "Old report", 3, extra=(1,))
    empty = c.new_bug("No messages", "thread", 1, 2)
    got = c.bug(b)
    assert abs(got["reported"] - (time.time() - 3 * DAY)) < 5
    assert abs(got["created"] - time.time()) < 5
    assert c.bug(empty)["reported"] == c.bug(empty)["created"]
    assert {x["id"]: x for x in c.bugs()}[b]["reported"] == got["reported"]


def test_merge_moves_reported(tmp_path):
    c = cs.Clinic(tmp_path / "nd.db")
    keep = old_bug(c, "Keep", 1)
    dup = old_bug(c, "Dup", 5)
    assert abs(c.bug(keep)["reported"] - (time.time() - DAY)) < 5
    c.merge_bugs(keep, dup)
    assert abs(c.bug(keep)["reported"] - (time.time() - 5 * DAY)) < 5


def test_list_uses_reported(tmp_path):
    db = tmp_path / "nd.db"
    c = cs.Clinic(db)
    old = old_bug(c, "Old report", 3)
    newer = old_bug(c, "Newer report", 1)
    fresh = c.new_bug("Fresh nomsg", "thread", 1, 99)
    assert [b["id"] for b in js(db, "vlad", "list", "--older-than", "2")] == [old]
    assert [b["id"] for b in js(db, "vlad", "list", "--sort", "oldest")] == [old, newer, fresh]
    assert [b["id"] for b in js(db, "vlad", "list")] == [fresh, newer, old]
    since = datetime.fromtimestamp(time.time() - 2 * DAY, timezone.utc).strftime("%Y-%m-%d")
    assert {b["id"] for b in js(db, "vlad", "list", "--since", since)} == {newer, fresh}
    row = {b["id"]: b for b in js(db, "vlad", "list")}[old]
    assert row["reported"] < row["filed"] and row["created"] == row["filed"]
    assert row["reported"].endswith("Z") and row["reported"][:10] == \
        datetime.fromtimestamp(time.time() - 3 * DAY, timezone.utc).strftime("%Y-%m-%d")
    code, out = run(db, "vlad", "list", "--text")
    assert code == 0 and row["reported"][:10] in out.decode()
    assert js(db, "vlad", "show", old)["bug"]["reported"] == row["reported"]


def test_new_carries_reported(tmp_path):
    db = tmp_path / "nd.db"
    old_bug(cs.Clinic(db), "Old report", 3)
    res = js(db, "vlad", "new")
    assert res["bugs"][0]["reported"] < res["bugs"][0]["filed"]


def _case_tar(case):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        data = json.dumps(case).encode()
        ti = tarfile.TarInfo("c/case.json")
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def test_import_case_reported(tmp_path):
    db = tmp_path / "nd.db"
    want = {"2026-09-01": ts("2026-09-01T00:00:00"), "2026-09-02T10:30:00Z": ts("2026-09-02T10:30:00"),
            "2026-09-03T10:30:00+03:00": ts("2026-09-03T07:30:00"), 1790000000: 1790000000.0}
    for rep, epoch in want.items():
        code, out = run(db, "visy", "import", stdin=_case_tar({"title": f"T{rep}", "reported": rep}))
        assert code == 0
        bid = json.loads(out)["imported"][0]["id"]
        assert cs.Clinic(db).bug(bid)["reported"] == epoch
    code, out = run(db, "visy", "import", stdin=_case_tar({"title": "none"}))
    bid = json.loads(out)["imported"][0]["id"]
    assert abs(cs.Clinic(db).bug(bid)["reported"] - time.time()) < 5
    assert run(db, "visy", "import", stdin=_case_tar({"title": "bad", "reported": "yesterday"}))[0] == 2


# items

def imp(ch, **kw):
    rec = {"source_key": "git:a", "track": "nw", "version": "0.2.5", "section": "added", "text": "Thing",
           "status": "done"} | kw
    return ch.import_lines([json.dumps(rec)], who="sync")


def test_item_dated_forms(tmp_path):
    ch = cc.Changes(cs.Clinic(tmp_path / "nd.db"))
    cases = {"2026-10-05": ts("2026-10-05T00:00:00"), "2026-10-05T10:00:00Z": ts("2026-10-05T10:00:00"),
             "2026-10-06T10:00:00": ts("2026-10-06T10:00:00"),
             "2026-10-07T10:00:00+03:00": ts("2026-10-07T07:00:00"), 1790000000: 1790000000.0,
             1790000001.5: 1790000001.5}
    for i, v in enumerate(cases):
        r = imp(ch, source_key=f"git:{i}", dated=v)
        assert r["applied"] == 1 and not r["skipped"], v
    assert sorted(i["dated"] for i in ch.items()) == sorted(cases.values())
    assert imp(ch, source_key="git:bad", dated="nonsense")["skipped"]
    assert imp(ch, source_key="git:nodate")["applied"] == 1
    assert next(i for i in ch.items() if i["source_key"] == "git:nodate")["dated"] is None


def test_reimport_fills_dated_unless_locked(tmp_path):
    ch = cc.Changes(cs.Clinic(tmp_path / "nd.db"))
    imp(ch, source_key="git:a")
    imp(ch, source_key="git:b")
    assert all(i["dated"] is None for i in ch.items())
    imp(ch, source_key="git:a", dated="2026-10-05")
    imp(ch, source_key="git:b", dated="2026-10-06")
    a, b = (next(i for i in ch.items() if i["source_key"] == k) for k in ("git:a", "git:b"))
    assert a["dated"] == ts("2026-10-05T00:00:00")
    ch.edit(b["id"], who="vlad", dated="2026-01-01")
    assert "dated" in ch.get(b["id"])["locked"]
    r = imp(ch, source_key="git:b", dated="2026-10-09")
    assert ch.get(b["id"])["dated"] == ts("2026-01-01T00:00:00")
    assert r["locked"][0]["fields"] == ["dated"]
    imp(ch, source_key="git:a", dated="2026-10-07")  # unlocked: follows the source
    assert ch.get(a["id"])["dated"] == ts("2026-10-07T00:00:00")


def test_add_dated(tmp_path):
    ch = cc.Changes(cs.Clinic(tmp_path / "nd.db"))
    cid = ch.add("nw", "0.2.5", "added", "X", who="v", dated="2026-10-05T10:00:00Z")
    assert ch.get(cid)["dated"] == ts("2026-10-05T10:00:00")
    assert ch.get(ch.add("nw", "0.2.5", "added", "Y", who="v"))["dated"] is None


def test_migration_adds_dated_column(tmp_path):
    db = tmp_path / "old.db"
    raw = sqlite3.connect(db)
    raw.executescript("CREATE TABLE nw_changes (id TEXT PRIMARY KEY, track TEXT, version TEXT, section TEXT, "
                      "text TEXT, dev TEXT, status TEXT, tags TEXT, bugs TEXT, cases TEXT, commits TEXT, "
                      "source_key TEXT UNIQUE, author TEXT, locked TEXT, created REAL, updated REAL);"
                      "INSERT INTO nw_changes VALUES ('CL-000001','nw','0.2.5','added','Old','','done','{}',"
                      "'[]','[]','[]',NULL,'x','[]',1.0,1.0);")
    raw.commit()
    raw.close()
    it = cc.Changes(cs.Clinic(db)).get("CL-000001")
    assert it["text"] == "Old" and it["dated"] is None and it["created"] == 1.0
    cc.Changes(cs.Clinic(db))  # second open: no error


def test_cl_list_sort_and_filter(tmp_path):
    db = tmp_path / "nd.db"
    cs.Clinic(db)
    a = js(db, "vlad", "cl", "add", "nw", "0.2.5", "added", "First added", "--dated", "2026-10-10")["id"]
    b = js(db, "vlad", "cl", "add", "nw", "0.2.5", "added", "Second added", "--dated", "2026-09-01")["id"]
    c = js(db, "vlad", "cl", "add", "nw", "0.2.5", "added", "Third undated")["id"]
    lst = js(db, "vlad", "cl", "list")
    assert [i["id"] for i in lst] == [c, b, a]  # default: added, newest first
    row = next(i for i in lst if i["id"] == a)
    assert row["dated"] == "2026-10-10T00:00:00Z" and row["added"] == row["created"] and row["added"].endswith("Z")
    assert next(i for i in lst if i["id"] == c)["dated"] is None
    assert [i["id"] for i in js(db, "vlad", "cl", "list", "--sort", "dated")] == [a, b, c]
    assert [i["id"] for i in js(db, "vlad", "cl", "list", "--sort", "added")] == [c, b, a]
    assert [i["id"] for i in js(db, "vlad", "cl", "list", "--dated-since", "2026-10-01")] == [a]
    assert run(db, "vlad", "cl", "list", "--sort", "bogus")[0] == 2
    assert run(db, "vlad", "cl", "list", "--dated-since", "nope")[0] == 2
    lines = run(db, "vlad", "cl", "list", "--sort", "dated", "--text")[1].decode().splitlines()
    assert "2026-10-10" in lines[0] and "2026-09-01" in lines[1] and lines[2].split()[-4:-3] != ["2026-10-10"]
    assert " -  " in lines[2] or "  -  " in lines[2]
    ed = js(db, "vlad", "cl", "edit", c, "--dated", "2026-08-01")
    assert ed["dated"] == "2026-08-01T00:00:00Z" and "dated" in ed["locked"]
    assert run(db, "vlad", "cl", "edit", c, "--dated", "junk")[0] == 2


def test_fixed_in_dated_is_bug_reported(tmp_path):
    db = tmp_path / "nd.db"
    c = cs.Clinic(db)
    b = old_bug(c, "Old crash", 4)
    rep = c.bug(b)["reported"]
    r = js(db, "vlad", "status", b, "fixed", "--in", "nw:0.2.5")
    assert r["change"]["created"] is True
    assert cc.Changes(cs.Clinic(db)).get(r["change"]["id"])["dated"] == rep
    b2 = old_bug(c, "Old hang", 6)  # existing item without dated: linked -> filled
    it = js(db, "vlad", "cl", "add", "nw", "0.2.5", "fixed", "Hang fix", "--bug", b2)["id"]
    assert js(db, "vlad", "cl", "show", it)["item"]["dated"] is None
    js(db, "vlad", "status", b2, "fixed", "--in", "nw:0.2.5")
    assert cc.Changes(cs.Clinic(db)).get(it)["dated"] == cs.Clinic(db).bug(b2)["reported"]
    b3 = old_bug(c, "Old freeze", 7)  # existing item with its own dated: untouched
    it3 = js(db, "vlad", "cl", "add", "nw", "0.2.5", "fixed", "Freeze fix", "--bug", b3,
             "--dated", "2026-01-01")["id"]
    js(db, "vlad", "status", b3, "fixed", "--in", "nw:0.2.5")
    assert cc.Changes(cs.Clinic(db)).get(it3)["dated"] == ts("2026-01-01T00:00:00")


# sync parsers

def test_nw_sync_lines_carry_commit_datetime():
    items, _ = nw.parse_log("aaa1111\x1f2026-10-01T10:00:00+03:00\x1f0.2.5: Add thing\x1f\x1e\n")
    assert items[0]["dated"] == "2026-10-01T10:00:00+03:00"


def test_coop_release_items_carry_published_date():
    items = coop.parse_release("v0.7.6", "### Added\n- Thing\n", dated="2026-10-26")
    assert items[0]["dated"] == "2026-10-26"
    assert "dated" not in coop.parse_release("v0.7.6", "### Added\n- Thing\n")[0]
