"""tools/discord/iosefka_cl.py: the `cl` changelist commands and the bug list age filters."""
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_clinic_cli import cli, cs, js, run, seed  # noqa: E402

CL_RX = re.compile(r"^CL-[0-9a-f]{6}$")


def test_cl_add_list_show(tmp_path):
    db, a, b, _ = seed(tmp_path)
    it = js(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Guest lamp after join",
            "--tag", "area=lamps", "--bug", b)
    assert CL_RX.match(it["id"]) and it["bugs"] == [b] and it["tags"] == {"area": "lamps"}
    lst = js(db, "vlad", "cl", "list", "--track", "coop", "--version", "0.7.7")
    assert [i["id"] for i in lst] == [it["id"]]
    assert js(db, "vlad", "cl", "list", "--track", "nw") == []
    d = js(db, "visy", "cl", "show", it["id"])
    assert d["bugs"][0]["id"] == b and d["bugs"][0]["title"] == "Chalice lamp stays dark"
    assert any(n["kind"] == "add" and n["who"] == "vlad" for n in d["notes"])
    code, out = run(db, "vlad", "cl", "list", "--text")
    assert code == 0 and out.decode().startswith(f"{it['id']}  coop 0.7.7  planned  fixed  -  Guest lamp")
    assert run(db, "vlad", "cl", "show", "CL-ffffff")[0] == 1
    assert run(db, "vlad", "cl", "show", "nope")[0] == 2
    assert run(db, "vlad", "cl", "add", "coop", "0.7.7", "bogus", "x")[0] == 2


def test_cl_current_next_flags(tmp_path):
    db, *_ = seed(tmp_path)
    js(db, "vlad", "cl", "version", "coop", "0.7.6", "--status", "current")
    js(db, "vlad", "cl", "version", "coop", "0.7.7", "--status", "planned")
    x = js(db, "vlad", "cl", "add", "coop", "0.7.6", "added", "Old thing")["id"]
    y = js(db, "vlad", "cl", "add", "coop", "0.7.7", "added", "New thing")["id"]
    assert [i["id"] for i in js(db, "vlad", "cl", "list", "--current")] == [x]
    assert [i["id"] for i in js(db, "vlad", "cl", "list", "--next", "--track", "coop")] == [y]
    assert js(db, "vlad", "cl", "list", "--next", "--track", "nw") == []
    assert js(db, "vlad", "cl", "list", "--current", "--track", "nw") == []
    vs = js(db, "vlad", "cl", "versions", "--track", "coop")
    assert [v["version"] for v in vs] == ["0.7.6", "0.7.7"]


def test_cl_import_stdin(tmp_path):
    db, *_ = seed(tmp_path)
    good = json.dumps({"source_key": "git:abc", "track": "nw", "version": "0.2.5 beta 3",
                       "section": "added", "text": "Free stamina", "status": "done"})
    ver = json.dumps({"kind": "version", "track": "nw", "version": "0.2.5 beta 3", "status": "current"})
    lines = "\n".join([good, "", "{bad json", ver])
    code, out = run(db, "vlad", "cl", "import", stdin=lines.encode())
    assert code == 0
    d = json.loads(out)
    assert d["applied"] == 2 and d["skipped"][0]["line"] == 3
    assert len(js(db, "vlad", "cl", "list", "--track", "nw")) == 1
    code, out = run(db, "vlad", "cl", "import", stdin=good.encode() + b" " * (8 * 1024 * 1024))
    assert code == 2


def test_fixed_in_links_existing_item(tmp_path):
    db, a, b, _ = seed(tmp_path)
    line = json.dumps({"source_key": "git:1", "track": "nw", "version": "0.2.5", "section": "fixed",
                       "text": "Launcher start", "status": "in-progress", "bugs": [a]})
    assert run(db, "vlad", "cl", "import", stdin=line.encode())[0] == 0
    cid = js(db, "vlad", "cl", "list", "--track", "nw")[0]["id"]
    js(db, "vlad", "cl", "move", cid, "nw", "0.2.6")  # locks version
    r = js(db, "vlad", "status", a, "fixed", "--in", "nw:0.2.5")
    assert r["change"] == {"id": cid, "created": False}
    items = js(db, "vlad", "cl", "list", "--track", "nw")
    assert len(items) == 1 and items[0]["status"] == "done" and items[0]["version"] == "0.2.6"
    # no existing item: a new one from the bug title; the version is planned so the item is too
    r = js(db, "vlad", "status", b, "fixed", "--in", "coop:0.7.7")
    assert r["change"]["created"] is True
    it = js(db, "vlad", "cl", "show", r["change"]["id"])["item"]
    assert it["source_key"] == f"bug:coop:{b}" and it["section"] == "fixed" and it["bugs"] == [b]
    assert it["text"] == "Chalice lamp stays dark" and it["status"] == "planned"
    js(db, "vlad", "cl", "version", "coop", "0.7.8", "--status", "released")
    n = cs.Clinic(db).new_bug("Another 198.51.100.7 thing", "thread", 1, 13)
    r = js(db, "vlad", "status", n, "fixed", "--in", "coop:0.7.8")
    it = js(db, "vlad", "cl", "show", r["change"]["id"])["item"]
    assert it["status"] == "done" and "198.51" not in it["text"]


def test_fixed_in_bad(tmp_path):
    db, a, b, _ = seed(tmp_path)
    assert run(db, "vlad", "status", a, "open", "--in", "nw:0.2.5")[0] == 2
    assert run(db, "vlad", "status", a, "fixed", "--in", "foo")[0] == 2
    assert run(db, "vlad", "status", a, "fixed", "--in", "nw:x")[0] == 2
    assert cs.Clinic(db).bug(a)["status"] == "open"


def test_notes_public_only(tmp_path):
    db, *_ = seed(tmp_path)
    js(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Lamp fix at 198.51.100.7",
       "--status", "done", "--dev", "see player_logs/secret-case")
    js(db, "vlad", "cl", "add", "coop", "0.7.7", "added", "Not done yet")
    for extra in ((), ("--text",)):
        code, out = run(db, "vlad", "cl", "notes", "coop", "0.7.7", *extra)
        s = out.decode()
        assert code == 0 and s.startswith("### Fixed\n- Lamp fix at ")
        assert "secret" not in s and "198.51.100.7" not in s and "Not done" not in s


def test_cl_add_via_forced_command(tmp_path, monkeypatch):
    db, *_ = seed(tmp_path)
    monkeypatch.setenv("NETDOCTOR_DB", str(db))
    monkeypatch.setenv("SSH_ORIGINAL_COMMAND",
                       'cl add nw "0.2.5 beta 3" added "Free stamina; out of combat $(rm -rf /)"')
    assert cli.entry(["--as", "vlad"]) == 0
    from clinic_changes import Changes
    got = Changes(cs.Clinic(db)).items(track="nw")
    assert [i["text"] for i in got] == ["Free stamina; out of combat $(rm -rf /)"]
    assert got[0]["version"] == "0.2.5 beta 3"


def test_list_age_and_sort(tmp_path):
    db, a, b, _ = seed(tmp_path)
    c = cs.Clinic(db)
    now = time.time()
    d = c.new_bug("Third bug", "thread", 1, 14)
    day = 86400
    for bid, created, act in ((a, now - 20 * day, now - 1 * day), (b, now - 12 * day, now - 9 * day),
                              (d, now - 2 * day, now - 2 * day)):
        c.db.execute("UPDATE nw_bugs SET created=?, activity=?, updated=? WHERE id=?", (created, act, act, bid))
        c.db.execute("UPDATE nw_messages SET created=? WHERE bug_id=?", (created, bid))  # reported follows
    c.db.commit()

    def ids(*x):
        return [r["id"] for r in js(db, "vlad", "list", *x)]
    assert ids() == [d, b, a]
    assert ids("--older-than", "10") == [b, a]
    assert ids("--idle", "5") == [b]
    assert ids("--sort", "oldest") == [a, b, d]
    assert ids("--sort", "activity") == [a, d, b]
    assert run(db, "vlad", "list", "--older-than", "x")[0] == 2
    assert run(db, "vlad", "list", "--sort", "foo")[0] == 2
    assert run(db, "vlad", "list", "--idle", "-3")[0] == 2


def test_vlad_can_write_both_tracks(tmp_path):
    db, *_ = seed(tmp_path)
    assert run(db, "vlad", "cl", "add", "coop", "0.7.7", "added", "A")[0] == 0
    assert run(db, "vlad", "cl", "add", "nw", "0.2.6", "changed", "B")[0] == 0
    a, b = (i["id"] for i in js(db, "vlad", "cl", "list"))
    assert run(db, "vlad", "cl", "status", a, "done")[0] == 0
    assert run(db, "vlad", "cl", "link", a, "--case", "case1", "--commit", "abc123")[0] == 0
    assert run(db, "vlad", "cl", "merge", a, b)[0] == 0
    assert js(db, "vlad", "cl", "show", b)["item"]["status"] == "dropped"
    assert run(db, "vlad", "cl", "edit", a, "--text", "Better A", "--tag", "k=v")[0] == 0
    assert run(db, "vlad", "cl", "related", a)[0] == 0
    assert run(db, "vlad", "cl", "move", a, "nw", "0.2.7")[0] == 0


def test_fixed_in_both_tracks(tmp_path):
    db, a, b, _ = seed(tmp_path)
    js(db, "vlad", "cl", "version", "coop", "0.7.7", "--status", "current")
    js(db, "vlad", "cl", "version", "nw", "0.2.6", "--status", "current")
    r1 = js(db, "vlad", "status", b, "fixed", "--in", "coop:0.7.7")
    r2 = js(db, "vlad", "status", b, "fixed", "--in", "nw:0.2.6")
    assert r1["change"]["created"] and r2["change"]["created"] and r1["change"]["id"] != r2["change"]["id"]
    keys = {js(db, "vlad", "cl", "show", r["change"]["id"])["item"]["source_key"] for r in (r1, r2)}
    assert keys == {f"bug:coop:{b}", f"bug:nw:{b}"}
    assert cs.Clinic(db).bug(b)["status"] == "fixed"


def test_fixed_in_skips_dropped_and_no_idle_note(tmp_path):
    db, a, b, _ = seed(tmp_path)
    js(db, "vlad", "cl", "version", "coop", "0.7.7", "--status", "current")
    keep = js(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Lamp one", "--bug", b)["id"]
    dup = js(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Lamp two", "--bug", b)["id"]
    js(db, "vlad", "cl", "merge", keep, dup)
    r = js(db, "vlad", "status", b, "fixed", "--in", "coop:0.7.7")
    assert r["change"] == {"id": keep, "created": False}
    assert js(db, "vlad", "cl", "show", keep)["item"]["status"] == "done"
    assert js(db, "vlad", "cl", "show", dup)["item"]["status"] == "dropped"
    n = len(js(db, "vlad", "cl", "show", keep)["notes"])
    js(db, "vlad", "status", b, "fixed", "--in", "coop:0.7.7")  # nothing changes: no new note
    assert len(js(db, "vlad", "cl", "show", keep)["notes"]) == n


def test_fixed_in_moves_unlocked_item(tmp_path):
    db, a, b, _ = seed(tmp_path)
    cid = js(db, "vlad", "cl", "add", "coop", "0.7.8", "fixed", "Lamp", "--bug", b)["id"]
    js(db, "vlad", "status", b, "fixed", "--in", "coop:0.7.7")
    it = js(db, "vlad", "cl", "show", cid)["item"]
    assert it["status"] == "done" and it["version"] == "0.7.7"
    assert {v["version"] for v in js(db, "vlad", "cl", "versions", "--track", "coop")} == {"0.7.7", "0.7.8"}


def test_case_with_date_prefix(tmp_path):
    db, *_ = seed(tmp_path)
    it = js(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Lamp", "--case", "2026-10-25/lamp_x")
    assert it["cases"] == ["2026-10-25/lamp_x"]
    assert run(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Lamp", "--case", "../x")[0] == 2
    assert run(db, "vlad", "cl", "add", "coop", "0.7.7", "fixed", "Lamp", "--case", "a/b/c")[0] == 2
