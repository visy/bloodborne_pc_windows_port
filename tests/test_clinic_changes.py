"""tools/discord/clinic_changes.py: versioned changelists in the clinic database."""
import importlib.util
import json
import sys
import time
from pathlib import Path

import pytest

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
DAY = 86400


@pytest.fixture
def ch(tmp_path):
    return cc.Changes(cs.Clinic(tmp_path / "nd.db"))


def test_version_key_order():
    vs = ["0.2.6", "0.2.5", "0.2.5 rc 1", "0.2.5 beta 3", "0.2.5 beta 10"]
    assert sorted(vs, key=cc.version_key) == ["0.2.5 beta 3", "0.2.5 beta 10", "0.2.5 rc 1", "0.2.5", "0.2.6"]
    assert cc.version_key("0.2.5 alpha 9") < cc.version_key("0.2.5 beta 1")


def test_one_current_per_track(ch):
    ch.set_version("coop", "0.7.6", status="current", who="visy")
    ch.set_version("nw", "0.2.5", status="current", who="vlad")
    ch.set_version("coop", "0.7.7", status="current", who="visy")
    st = {v["version"]: v["status"] for v in ch.versions("coop")}
    assert st == {"0.7.6": "planned", "0.7.7": "current"}
    assert ch.current("coop") == "0.7.7" and ch.current("nw") == "0.2.5"
    assert ch.next("coop") is None
    ch.set_version("coop", "0.7.9", who="visy")
    ch.set_version("coop", "0.7.8", who="visy")
    assert ch.next("coop") == "0.7.8"
    assert ch.next("nw") is None


def test_add_and_filters(ch, monkeypatch):
    t = [1000.0 * DAY]
    monkeypatch.setattr(time, "time", lambda: t[0])
    a = ch.add("coop", "0.7.7", "fixed", "Chalice lamp fix", who="visy", tags={"area": "chalice"},
               bugs=["NW-0000001"])
    t[0] += 10 * DAY
    b = ch.add("coop", "0.7.7", "added", "Party chat", who="visy", status="done")
    t[0] += 10 * DAY
    c = ch.add("nw", "0.2.5 beta 3", "changed", "Camera tweak", who="vlad", status="in-progress")
    assert ch.versions("nw")[0]["status"] == "planned"
    assert [i["id"] for i in ch.items()] == [c, b, a]
    assert [i["id"] for i in ch.items(track="coop", tag="area=chalice")] == [a]
    assert {i["id"] for i in ch.items(status=["planned", "done"])} == {a, b}
    assert [i["id"] for i in ch.items(bug="NW-0000001")] == [a]
    assert [i["id"] for i in ch.items(q="lamp")] == [a]
    now = t[0]
    assert {i["id"] for i in ch.items(older_than=now - 15 * DAY)} == {a}
    assert {i["id"] for i in ch.items(since=now - 15 * DAY)} == {b, c}
    t[0] += 5 * DAY
    ch.set_status(b, "in-progress", who="visy")
    assert {i["id"] for i in ch.items(idle=t[0] - 3 * DAY)} == {a, c}
    assert ch.get(a)["tags"] == {"area": "chalice"} and ch.get(a)["locked"] == []
    assert ch.versions("coop")[0]["counts"] == {"planned": 1, "in-progress": 1, "done": 0, "dropped": 0}


def test_text_rules():
    assert "<public IP>" in cc.clean_text("host at 85.12.3.4")
    for bad in ("see player_logs/x", "<@123>", "id 156209977686910567", "x" * 301):
        with pytest.raises(ValueError):
            cc.clean_text(bad)
    with pytest.raises(ValueError):
        cc.clean_text("  ")


def test_import_respects_locks(ch):
    line = {"kind": "item", "source_key": "git:abc", "track": "nw", "version": "0.2.5", "section": "fixed",
            "text": "first text", "status": "in-progress"}
    assert ch.import_lines([json.dumps(line)], who="sync") == {"applied": 1, "skipped": []}
    cid = ch.items()[0]["id"]
    ch.move(cid, "nw", "0.2.6", who="vlad")
    ch.edit(cid, who="vlad", text="human text")
    line.update(text="new text", status="done")
    ch.import_lines([json.dumps(line)], who="sync")
    it = ch.get(cid)
    assert (it["version"], it["text"], it["status"]) == ("0.2.6", "human text", "done")
    assert len(ch.items()) == 1
    line["status"] = "planned"
    ch.import_lines([json.dumps(line)], who="sync")
    assert ch.get(cid)["status"] == "done"
    ch.set_status(cid, "dropped", who="vlad")
    line["status"] = "done"
    ch.import_lines([json.dumps(line)], who="sync")
    assert ch.get(cid)["status"] == "dropped"


def test_import_partial(ch):
    good = {"kind": "item", "source_key": "g:1", "track": "coop", "version": "0.7.7", "section": "added",
            "text": "ok"}
    lines = [json.dumps(good), "{bad", json.dumps(dict(good, source_key="g:2", track="x")),
             json.dumps({"kind": "item", "source_key": "g:3", "track": "coop", "version": "0.7.7",
                         "section": "added"}),
             json.dumps({"kind": "version", "track": "coop", "version": "0.7.8", "status": "planned"})]
    r = ch.import_lines(lines, who="sync")
    assert r["applied"] == 2
    assert [s["line"] for s in r["skipped"]] == [2, 3, 4]
    assert all(s["why"] for s in r["skipped"])
    assert len(ch.items()) == 1


def test_import_version_lines_respect_human(ch):
    ch.set_version("coop", "0.2.5", status="current", who="visy")
    ch.import_lines([json.dumps({"kind": "version", "track": "coop", "version": "0.2.5",
                                 "status": "released"})], who="sync")
    assert ch.current("coop") == "0.2.5"
    # unlocked versions follow the import; a human-set current survives a newer imported current
    ch.import_lines([json.dumps({"kind": "version", "track": "nw", "version": "0.2.4", "status": "released",
                                 "released": "2026-01-02"}),
                     json.dumps({"kind": "version", "track": "nw", "version": "0.2.5", "status": "current"})],
                    who="sync")
    assert ch.current("nw") == "0.2.5"
    assert [v["status"] for v in ch.versions("nw")] == ["released", "current"]
    assert ch.versions("nw")[0]["released"] is not None
    ch.set_version("nw", "0.2.5", status="current", who="vlad")
    ch.import_lines([json.dumps({"kind": "version", "track": "nw", "version": "0.2.6", "status": "current"})],
                    who="sync")
    assert ch.current("nw") == "0.2.5"


def test_merge(ch):
    k = ch.add("coop", "0.7.7", "fixed", "keep", who="visy", bugs=["NW-1"], status="in-progress")
    d = ch.add("coop", "0.7.7", "fixed", "dup", who="visy", bugs=["NW-2"], cases=["case1"], status="done")
    ch.merge(k, d, who="visy")
    assert ch.get(k)["bugs"] == ["NW-1", "NW-2"] and ch.get(k)["cases"] == ["case1"]
    assert ch.get(d)["status"] == "dropped"
    assert ch.notes(k)[-1]["kind"] == "merge" and ch.notes(d)[-1]["kind"] == "merge"
    assert ch.item_for_bug("coop", "NW-2")["id"] in (k, d)
    assert ch.item_for_bug("coop", "NW-404") is None


def test_related(ch):
    nw = ch.add("nw", "0.2.5", "fixed", "Chalice lamp flicker", who="vlad", bugs=["NW-0000001"],
                tags={"area": "chalice"})
    co = ch.add("coop", "0.7.7", "fixed", "Guest lamp flicker in chalice dungeon", who="visy",
                bugs=["NW-0000001"], tags={"zone": "chalice"})
    same = ch.add("coop", "0.7.7", "fixed", "Another lamp flicker", who="visy", bugs=["NW-0000001"])
    rel = ch.related(co)
    ids = [r["id"] for r in rel]
    assert nw in ids and co not in ids and same not in ids
    r = next(r for r in rel if r["id"] == nw)
    assert "bug NW-0000001" in r["why"] and "tag chalice" in r["why"]
    assert any(w.startswith("words") and "flicker" in w for w in r["why"])
    unrelated = ch.add("nw", "0.2.5", "added", "Zzzz something else", who="vlad")
    assert unrelated not in [x["id"] for x in ch.related(co)]


def test_related_bugs(ch):
    b1 = ch.clinic.new_bug("Elevator softlock in cathedral ward", "discord", 1, 1, tags={"area": "nw-cathedral"})
    ch.add("coop", "0.7.7", "fixed", "Cathedral ward elevator softlock", who="visy")
    rel = ch.related(b1)
    assert [r["kind"] for r in rel] == ["item"] and rel[0]["track"] == "coop"


def test_render_notes(ch):
    ch.add("coop", "0.7.7", "fixed", "Fix one", who="v", status="done", dev="secret path")
    ch.add("coop", "0.7.7", "added", "Add one", who="v", status="done")
    ch.add("coop", "0.7.7", "fixed", "Not done", who="v")
    ch.add("coop", "0.7.7", "fixed", "Fix two at 85.12.3.4", who="v", status="done")
    out = ch.render_notes("coop", "0.7.7")
    assert out == "### Added\n- Add one\n\n### Fixed\n- Fix one\n- Fix two at <public IP>\n"
    assert "secret" not in out and "Not done" not in out and "Changed" not in out
    assert ch.render_notes("coop", "9.9.9") == ""


def test_set_status_automation_forward_only(ch):
    cid = ch.add("coop", "0.7.7", "fixed", "x", who="v")
    assert ch.set_status(cid, "done", who="sys", human=False) is True
    assert ch.get(cid)["locked"] == []
    assert ch.set_status(cid, "planned", who="sys", human=False) is False
    assert ch.get(cid)["status"] == "done"


def test_edit_link_notes(ch):
    cid = ch.add("coop", "0.7.7", "fixed", "x", who="v", tags={"a": "1"})
    ch.edit(cid, who="visy", tags={"b": "2", "a": None}, dev="path")
    it = ch.get(cid)
    assert it["tags"] == {"b": "2"} and set(it["locked"]) == {"tags", "dev"}
    ch.link(cid, who="visy", bugs=["NW-9"], commits=["abc"])
    ch.link(cid, who="visy", bugs=["NW-9"], unlink=True)
    it = ch.get(cid)
    assert it["bugs"] == [] and it["commits"] == ["abc"]
    assert [n["who"] for n in ch.notes(cid)] == ["v", "visy", "visy", "visy"]
    with pytest.raises(ValueError):
        ch.edit("CL-000000", who="x", text="y")
