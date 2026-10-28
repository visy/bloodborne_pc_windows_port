"""tools/discord/iosefka_cli.py: the dev bug tool (forced command, JSON output, tar streams)."""
import importlib.util
import io
import json
import os
import sys
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
spec = importlib.util.spec_from_file_location("iosefka_cli", DISCORD / "iosefka_cli.py")
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)
cs = cli.clinic_store

LOG = b"[coop] Net: party networking on (host, as HostA, port 9307)\n"


def run(db, who, *argv, stdin=b""):
    out = io.BytesIO()
    code = cli.main(list(argv), who, out, db=db, stdin=io.BytesIO(stdin))
    return code, out.getvalue()


def js(db, who, *argv):
    code, out = run(db, who, *argv)
    assert code == 0, argv
    return json.loads(out)


def seed(tmp_path):
    """Two bugs: a launcher one with a log and a screenshot, a co-op chalice one with a video still."""
    db = tmp_path / "nd.db"
    c = cs.Clinic(db)
    log = c.put_file(LOG, "nightwatch-20261024_204859.log", "log")
    img = c.put_file(b"\x89PNGfake", "shot.png", "image")
    vid = "v" * 64
    c.add_stills(vid, [(0.0, b"\xff\xd8still")])
    a = c.new_bug("Launcher will not start", "thread", 1, 11)
    b = c.new_bug("Chalice lamp stays dark", "thread", 1, 12)
    c.update_bug(a, tags={"component": "nw-launcher"}, status="open")
    c.update_bug(b, tags={"component": "coop-mod", "feature": ["chalice", "lamps"]}, status="open")
    c.upsert_message({"message_id": 100, "author_name": "PlayerA", "created": time.time(), "bug_id": a,
                      "content": "crashes at 198.51.100.7", "state": "deleted",
                      "attachments": [{"sha256": log, "filename": "x.log", "kind": "log", "size": len(LOG)},
                                      {"sha256": img, "filename": "shot.png", "kind": "image", "size": 8}]})
    c.upsert_message({"message_id": 101, "author_name": "PlayerB", "created": time.time(), "bug_id": b,
                      "content": "lamp issue", "attachments": [
                          {"sha256": vid, "filename": "clip.mp4", "kind": "video", "size": 5}]})
    c.add_note(a, "vlad", "note", "looks like a missing dll")
    return db, a, b, log


def test_forced_command_rejects_shell(tmp_path, monkeypatch, capsys):
    db, a, b, _ = seed(tmp_path)
    marker = tmp_path / "pwned"
    monkeypatch.setenv("NETDOCTOR_DB", str(db))
    for cmd in ("list; rm -rf ~", f"list `touch {marker}`", f"list $(touch {marker})",
                f"note {a} x; touch {marker}", "files ../../etc", "files --out /tmp/x", "export", "bogus",
                "list 'unbalanced"):
        monkeypatch.setenv("SSH_ORIGINAL_COMMAND", cmd)
        code = cli.entry(["--as", "vlad"])
        assert code in (1, 2), cmd
    assert not marker.exists()
    assert len(cs.Clinic(db).bugs()) == 2
    assert len(cs.Clinic(db).notes(a)) == 1
    monkeypatch.delenv("SSH_ORIGINAL_COMMAND")
    assert cli.entry([]) == 2          # no --as
    capsys.readouterr()


def test_forced_command_ignores_extra_argv(tmp_path, monkeypatch):
    db, a, _, _ = seed(tmp_path)
    monkeypatch.setenv("NETDOCTOR_DB", str(db))
    monkeypatch.setenv("SSH_ORIGINAL_COMMAND", "list --text")
    assert cli.entry(["--as", "vlad", "export"]) == 0


def test_show_json_roundtrip(tmp_path):
    db, a, _, log = seed(tmp_path)
    d = js(db, "vlad", "show", a)
    assert d["bug"]["id"] == a and d["bug"]["status"] == "open"
    assert d["bug"]["counts"] == {"messages": 1, "logs": 1, "images": 1, "stills": 0}
    assert d["messages"][0]["author"] == "PlayerA"
    assert d["messages"][0]["attachments"][0]["sha256"] == log
    assert d["notes"][0]["text"] == "looks like a missing dll"
    assert {f["kind"] for f in d["files"]} == {"log", "image"}
    brief = js(db, "vlad", "show", a, "--brief")
    assert brief["bug"]["id"] == a
    code, out = run(db, "vlad", "show", a, "--text")
    assert code == 0 and a.encode() in out
    code, _ = run(db, "vlad", "show", "NW-0000000")
    assert code == 1


def test_list_filters_and(tmp_path):
    db, a, b, _ = seed(tmp_path)
    ids = lambda *args: [x["id"] for x in js(db, "vlad", "list", *args)]  # noqa: E731
    assert set(ids()) == {a, b}
    assert ids("--open", "--tag", "component=nw-launcher") == [a]
    assert ids("--open", "--tag", "component=coop-mod", "--tag", "feature=Chalice") == [b]
    assert ids("--tag", "component=coop-mod", "--tag", "feature=boss") == []
    assert ids("--status", "open", "--status", "fixed", "--q", "lamp") == [b]
    assert ids("--q", "missing dll") == [a]            # note text
    assert ids("--q", "lamp issue") == [b]             # message text
    assert ids("--since", "2999-01-01") == []
    assert set(ids("--since", "2020-01-01")) == {a, b}
    js(db, "visy", "assign", a, "vlad")
    assert ids("--assignee", "vlad") == [a]
    js(db, "vlad", "status", b, "fixed")
    assert ids("--open") == [a]
    row = js(db, "vlad", "list", "--tag", "component=nw-launcher")[0]
    assert set(row) >= {"id", "title", "status", "tags", "assignee", "created", "updated", "counts",
                        "nd_refs", "last_note"}
    assert row["counts"]["messages"] == 1 and row["last_note"]["text"].startswith("assigned")
    code, _ = run(db, "vlad", "list", "--since", "yesterday")
    assert code == 2


def _tar(data):
    return tarfile.open(fileobj=io.BytesIO(data), mode="r")


def test_files_by_query_tar_layout(tmp_path, monkeypatch):
    db, a, b, log = seed(tmp_path)
    monkeypatch.chdir(tmp_path)
    monkeypatch.setenv("TMPDIR", str(tmp_path))
    before = set(os.listdir(tmp_path))
    code, out = run(db, "vlad", "files", "--open", "--tag", "component=nw-launcher")
    assert code == 0
    assert set(os.listdir(tmp_path)) == before                 # the tar is streamed, no file written
    t = _tar(out)
    names = t.getnames()
    assert f"{a}/bug.json" in names
    assert any(n.startswith(f"{a}/logs/") and n.endswith("nightwatch-20261024_204859.log") for n in names)
    assert any(n.startswith(f"{a}/images/") for n in names)
    assert not any(n.startswith(b) for n in names)
    log_name = next(n for n in names if "/logs/" in n)
    assert t.extractfile(log_name).read() == LOG
    assert json.load(t.extractfile(f"{a}/bug.json"))["bug"]["id"] == a
    _, out = run(db, "vlad", "files", a, b)
    names = _tar(out).getnames()
    assert any(n.startswith(f"{b}/stills/") for n in names)
    _, out = run(db, "vlad", "files", a, "--no-media")
    names = _tar(out).getnames()
    assert f"{a}/bug.json" in names and not any("/images/" in n for n in names)
    code, _ = run(db, "vlad", "files")
    assert code == 2


def test_export_visy_only(tmp_path):
    db, a, _, log = seed(tmp_path)
    for who in ("vlad",):
        for cmd in ("export", "confirm-export", "import"):
            code, out = run(db, who, cmd)
            assert code == 1 and out == b""
    code, out = run(db, "visy", "export")
    assert code == 0
    t = _tar(out)
    manifest = json.load(t.extractfile("manifest.json"))
    shas = {f["sha256"] for f in manifest["files"]}
    assert log in shas
    assert t.extractfile(f"files/{log}").read() == LOG
    c = cs.Clinic(db)
    assert not c.exported(log)
    good = [{"sha256": log, "rehash": __import__("hashlib").sha256(LOG).hexdigest()}]
    bad = [{"sha256": s, "rehash": "0" * 64} for s in shas - {log}]
    body = json.dumps({"export_id": manifest["export_id"], "files": good + bad}).encode()
    code, out = run(db, "visy", "confirm-export", stdin=body)
    res = json.loads(out)
    assert code == 0 and res["confirmed"] == 1 and len(res["rejected"]) == len(bad)
    assert cs.Clinic(db).exported(log)


def test_import_local_case(tmp_path):
    db = tmp_path / "nd.db"
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        for name, data in (("case1/case.json", json.dumps({
                "title": "Local crash", "tags": {"component": "coop-mod"},
                "notes": ["seen on 0.7.6"], "files": ["last_session.log"]}).encode()),
                ("case1/last_session.log", LOG)):
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    code, out = run(db, "visy", "import", stdin=buf.getvalue())
    res = json.loads(out)
    assert code == 0 and res["imported"][0]["files"] == 1
    bug_id = res["imported"][0]["id"]
    b = cs.Clinic(db).bug(bug_id)
    assert b["source"] == "local" and b["tags"] == {"component": "coop-mod"}
    d = js(db, "vlad", "show", bug_id)
    assert d["files"][0]["kind"] == "log" and d["notes"][0]["text"] == "seen on 0.7.6"
    evil = io.BytesIO()
    with tarfile.open(fileobj=evil, mode="w") as t:
        ti = tarfile.TarInfo("../case.json")
        ti.size = 2
        t.addfile(ti, io.BytesIO(b"{}"))
    code, _ = run(db, "visy", "import", stdin=evil.getvalue())
    assert code == 2


def test_tell_goes_to_outbox_masked(tmp_path):
    db, a, _, _ = seed(tmp_path)
    res = js(db, "vlad", "status", a, "fixed", "--tell",
             "Fixed in the next build. Your address 203.0.113.9:9307 or 192.168.1.5 was fine.")
    assert "203.0.113.9" not in res["tell_text"] and "192.x.x.x" in res["tell_text"]
    c = cs.Clinic(db)
    pending = c.outbox_pending()
    assert [p["bug_id"] for p in pending] == [a]
    assert "203.0.113.9" not in pending[0]["text"] and "9307" not in pending[0]["text"]
    assert any(n["kind"] == "triage" and "203.0.113.9" not in n["text"] for n in c.notes(a))
    code, _ = run(db, "vlad", "status", a, "open", "--dup", "NW-1a2b3c4")
    assert code == 2


def test_new_since_last_check(tmp_path):
    db, a, b, _ = seed(tmp_path)
    first = js(db, "vlad", "new")
    assert {x["id"] for x in first["bugs"]} == {a, b}
    assert first["notes"] == []                        # his own note is not news to him
    time.sleep(0.02)
    c = cs.Clinic(db)
    new = c.new_bug("Fresh report", "thread", 1, 13)
    js(db, "visy", "share", a, "vlad", "please look at this")
    js(db, "visy", "share", a, "other", "not for vlad")
    second = js(db, "vlad", "new")
    assert [x["id"] for x in second["bugs"]] == [new]
    assert [n["text"] for n in second["notes"]] == ["please look at this"]
    assert js(db, "vlad", "new")["bugs"] == []
    assert {x["id"] for x in js(db, "visy", "new")["bugs"]} == {a, b, new}


def test_note_tag_assign_merge_nd(tmp_path):
    db, a, b, _ = seed(tmp_path)
    js(db, "vlad", "note", a, "second note; with `odd` $(chars)")
    t = js(db, "vlad", "tag", a, "area=map", "feature=chalice,lamps", "version=0.7.6")
    assert t["tags"]["feature"] == ["chalice", "lamps"]
    t = js(db, "vlad", "tag", a, "version=")
    assert "version" not in t["tags"]
    assert js(db, "vlad", "assign", a, "-")["assignee"] is None
    js(db, "vlad", "merge", a, b)
    d = js(db, "vlad", "show", b)
    assert d["bug"]["status"] == "duplicate" and d["bug"]["dup_of"] == a
    assert any("odd" in n["text"] for n in js(db, "vlad", "show", a)["notes"])
    code, _ = run(db, "vlad", "nd", "ND-aaaaaaaa")
    assert code == 1
    code, _ = run(db, "vlad", "nd", "ND-zz")
    assert code == 2



def test_files_inner_logs_under_archive_name(tmp_path):
    db, a, _b, _log = seed(tmp_path)
    c = cs.Clinic(db)
    inner = c.put_file(b"deep log", "guest.log", "log")
    arch = c.put_file(b"PK fake archive", "saves.zip", "archive")
    c.upsert_message({"message_id": 200, "bug_id": a, "created": time.time(), "attachments": [
        {"sha256": arch, "filename": "saves.zip", "kind": "archive", "size": 15, "inner": ["x.zip/dir/guest.log"],
         "inner_shas": [inner], "inner_files": [{"sha256": inner, "name": "x.zip/dir/../guest.log", "kind": "log"}]}]})
    _, out = run(db, "vlad", "files", a)
    t = _tar(out)
    assert t.extractfile(f"{a}/logs/saves.zip/x.zip/dir/guest.log").read() == b"deep log"
    shown = js(db, "vlad", "show", a)
    assert any(f.get("inside") == "saves.zip" and f["path"] == "x.zip/dir/../guest.log" for f in shown["files"])


def test_files_tars_saves_under_saves(tmp_path):
    db, a, _b, _log = seed(tmp_path)
    c = cs.Clinic(db)
    top = c.put_file(b"save0", "userdata0000", "save")
    inner = c.put_file(b"save1", "backup0000", "save")
    arch = c.put_file(b"PK fake", "SPRJ0005.zip", "archive")
    c.upsert_message({"message_id": 300, "bug_id": a, "created": time.time(), "attachments": [
        {"sha256": top, "filename": "userdata0000", "kind": "save", "size": 5},
        {"sha256": arch, "filename": "SPRJ0005.zip", "kind": "archive", "size": 7, "inner": ["SPRJ0005/backup0000"],
         "inner_shas": [inner], "inner_files": [{"sha256": inner, "name": "SPRJ0005/backup0000", "kind": "save"}]}]})
    _, out = run(db, "vlad", "files", a)
    names = _tar(out).getnames()
    assert any(n.startswith(f"{a}/saves/") and n.endswith("userdata0000") for n in names)
    assert f"{a}/saves/SPRJ0005.zip/SPRJ0005/backup0000" in names
    assert not any("/logs/" in n and "userdata0000" in n for n in names)
