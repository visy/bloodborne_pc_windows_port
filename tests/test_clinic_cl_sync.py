"""tools/discord/cl_sync_nw.py and cl_import_coop.py: PC-side changelist import scripts."""
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))


def _load(name):
    s = importlib.util.spec_from_file_location(name, DISCORD / f"{name}.py")
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


cs = _load("clinic_store")
cc = _load("clinic_changes")
nw = _load("cl_sync_nw")
coop = _load("cl_import_coop")

LOG = (
    "aaa1111\x1f2026-10-01T10:00:00+03:00\x1f0.2.5 beta 3: VRAM cleanup\x1fSee NW-1234567 for details\x1e\n"
    "bbb2222\x1f2026-10-02T10:00:00+03:00\x1f0.2.5 Beta 3: Fix firewall prompt\x1f\x1e\n"
    "ccc3333\x1f2026-10-03T10:00:00+03:00\x1fCheats: free stamina\x1f\x1e\n"
    "ddd4444\x1f2026-09-01T09:00:00+03:00\x1f0.2.4: Add logs menu\x1f\x1e\n"
)


def test_parse_log_versions():
    items, vers = nw.parse_log(LOG)
    assert len(items) == 3
    by = {i["source_key"]: i for i in items}
    assert by["git:aaa1111"]["section"] == "changed"
    assert by["git:bbb2222"]["section"] == "fixed"
    assert by["git:ddd4444"]["section"] == "added"
    assert all(i["status"] == "done" and i["track"] == "nw" for i in items)
    assert by["git:aaa1111"]["version"] == "0.2.5 beta 3"
    assert by["git:aaa1111"]["bugs"] == ["NW-1234567"]
    vv = {v["version"]: v for v in vers}
    assert vv["0.2.5 beta 3"]["status"] == "current"
    assert vv["0.2.4"]["status"] == "released" and vv["0.2.4"]["released"] == "2026-09-01"


def test_sync_rerun_idempotent(tmp_path):
    ch = cc.Changes(cs.Clinic(tmp_path / "nd.db"))
    items, vers = nw.parse_log(LOG)
    lines = [json.dumps(r) for r in vers + items]
    r = ch.import_lines(lines, who="sync")
    assert not r["skipped"]
    cid = ch.items(track="nw", version="0.2.5 beta 3", section="fixed")[0]["id"]
    ch.move(cid, "nw", "0.2.4", who="visy")
    ch.import_lines(lines, who="sync")
    assert len(ch.items(track="nw")) == 3
    assert ch.get(cid)["version"] == "0.2.4"


def test_unsafe_text_skipped(capsys):
    log = "eee5555\x1f2026-10-03T10:00:00+03:00\x1f0.2.5: fix 12345678901234567890 crash\x1f\x1e\n"
    items, _ = nw.parse_log(log)
    assert items == []
    assert "eee5555" in capsys.readouterr().err


def test_parse_release():
    body = "## Notes\n\n### Added\n- Party chat\n* Colored names\n### Fixed\n- Crash on load\n"
    items = coop.parse_release("v0.7.6", body)
    assert [(i["section"], i["text"]) for i in items] == [
        ("added", "Party chat"), ("added", "Colored names"), ("fixed", "Crash on load")]
    assert items[0]["source_key"] == "release:v0.7.6:1"
    assert items[0]["version"] == "0.7.6" and items[0]["status"] == "done" and items[0]["track"] == "coop"
    flat = coop.parse_release("v0.7.5", "- one thing\n- two thing\n")
    assert [i["section"] for i in flat] == ["changed", "changed"]
    priv = coop.parse_release("v0.7.4", "- Fixed crash seen in player_logs/case1/x.log\n")
    assert "player_logs" not in priv[0]["text"] and "player_logs" in priv[0]["dev"]
    cc.clean_text(priv[0]["text"])


def test_unmatched(tmp_path):
    for n in ("2026-10-01_alice_crash", "2026-10-02_bob_hang", "2026-10-03_carol"):
        (tmp_path / n).mkdir()
    (tmp_path / "readme.txt").write_text("x")
    names = tmp_path / "names.json"
    names.write_text(json.dumps(["2026-10-02_bob_hang"]))
    out = coop.unmatched(tmp_path, json.loads(names.read_text()))
    assert out == ["2026-10-01_alice_crash", "2026-10-03_carol"]
