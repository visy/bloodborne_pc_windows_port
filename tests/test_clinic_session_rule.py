"""The 'session' auto-link rule: same calendar day, names or a known time overlap; and `split` repairs mistakes.
Synthetic log infos only."""
import importlib.util
import io
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCORD = ROOT / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
sys.path.insert(0, str(ROOT / "scripts"))
import netdoctor_store as ns  # noqa: E402
import net_doctor as nd  # noqa: E402

spec = importlib.util.spec_from_file_location("iosefka_cli", DISCORD / "iosefka_cli.py")
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)
cs = cli.clinic_store


def info(name, role="guest", host=None, day="2026-11-01", hhmm=20 * 60, end=None, peers=()):
    start = nd._minutes(day, hhmm) if day else None
    return {"name": name, "role": role, "host": host, "start": start, "end": end, "day": day or "",
            "peers": set(peers)}


def test_different_days_same_time_no_link():
    a = info("PA", "host", "PA", day="2026-11-01", peers={"PB"})
    b = info("PC", "host", "PC", day="2026-11-02")
    assert not ns.same_session(a, b)
    # even with a shared name and the same host, another day is another session
    c = info("PB", "guest", "PA", day="2026-11-02", peers={"PA"})
    assert not ns.same_session(a, c)


def test_host_role_without_host_field_blocks_other_hosts():
    a = info("PA", "host", "PA", peers={"PB"})
    b = info("PC", "host", None)  # host log whose host field is missing
    assert not ns.same_session(a, b)
    g = info("PD", "guest", "PA", hhmm=20 * 60 + 5)
    assert not ns.same_session(b, g)


def test_same_day_shared_names_link():
    a = info("PA", "host", "PA", peers={"PB"})
    b = info("PB", "guest", "PA", hhmm=20 * 60 + 90)
    assert ns.same_session(a, b)


def test_missing_day_without_names_no_link():
    a = info("PA", "host", "PA", day="2026-11-01")
    b = info("PE", "guest", None, day=None)
    b["start"] = None
    assert not ns.same_session(a, b)
    # a missing day never links by time alone, only by name
    b2 = info("PB", "guest", None, day=None)
    b2["start"] = a["start"]
    b2["day"] = ""
    b2["start"] = None
    a["peers"] = {"PB"}
    assert ns.same_session(a, b2)


def test_time_only_branch_needs_same_day_and_starts():
    a = info("PA", "guest", None, hhmm=600)
    b = info("PB", "guest", None, hhmm=610)
    assert ns.same_session(a, b)  # same day, 10 min apart, no names
    b["start"] = None
    assert not ns.same_session(a, b)


def test_old_row_without_day_derives_from_start():
    a = info("PA", "guest", None, hhmm=600)
    b = info("PB", "guest", None, day="2026-11-02", hhmm=600)
    a["day"] = ""  # old stored row: day empty, start still absolute
    assert not ns.same_session(a, b)


def _db(tmp_path):
    c = cs.Clinic(tmp_path / "nd.db")
    bug = c.new_bug("playtest", "thread", 1, 11)
    c.upsert_message({"message_id": 500, "channel_id": 7, "author_id": 9, "author_name": "x", "created": 1.7e9,
                      "content": "my log\nsecond line", "bug_id": bug, "attachments": []})
    return tmp_path / "nd.db", c, bug


def test_split_moves_message_to_new_bug(tmp_path):
    db, c, bug = _db(tmp_path)
    out = io.BytesIO()
    assert cli.main(["split", "500"], "visy", out, db=db, stdin=io.BytesIO(b"")) == 0
    res = json.loads(out.getvalue())
    new = res["to"]
    assert res["from"] == bug and new != bug and res["title"] == "my log"
    b = c.bug(new)
    assert b["status"] == "new" and b["tags"].get("triage") == "pending"
    assert c.message(500)["bug_id"] == new
    assert any("split" in n["text"] for n in c.notes(new)) and any("split" in n["text"] for n in c.notes(bug))
    # unknown message
    assert cli.main(["split", "1"], "visy", io.BytesIO(), db=db, stdin=io.BytesIO(b"")) != 0
