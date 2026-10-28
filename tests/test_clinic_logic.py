"""tools/discord/clinic_logic.py: pure rules (skip, file kinds, IP mask, clustering, classify)."""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
D = ROOT / "tools" / "discord"


def load(name):
    spec = importlib.util.spec_from_file_location(name, D / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(D))
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.path.remove(str(D))
    return mod


cl = load("clinic_logic")


def info(name, role, host, start, end=None, peers=()):
    return {"name": name, "role": role, "host": host, "start": start, "end": end,
            "day": "2026-10-24", "peers": set(peers)}


def test_skip_rule():
    for n in ("staff", "staff-archived", "development"):
        assert cl.skipped_channel(n)
    for n in ("moderator-only", "general", "dev-chat", "development-notes"):
        assert not cl.skipped_channel(n)


def test_removable_kinds():
    for n in ("nightwatch-x.log", "notes.txt", "crash.dmp", "userdata0010", "x.dcx"):
        assert cl.removable(n), n
    assert not cl.removable("a.png") and not cl.removable("b.mp4")
    assert cl.removable("saves.zip", ["last_session.log"])
    assert not cl.removable("saves.zip", ["a.png"])
    assert cl.removable("saves.zip", None)
    assert cl.file_kind("A.PNG") == "image" and cl.file_kind("b.mp4") == "video"
    assert cl.file_kind("x.7z") == "archive" and cl.file_kind("x.dmp") == "dump"
    assert cl.file_kind("userdata0001") == "game" and cl.file_kind("readme.md") == "other"


def test_mask_ips():
    out = cl.mask_ips("host 198.51.100.7:9307 lan 192.168.1.5 radmin 26.1.2.3 v6 2001:db8::1")
    assert "198.51.100.7" not in out and "9307" not in out
    assert "<public IP>" in out
    assert "192.x.x.x" in out and "26.x.x.x" in out  # same as net_doctor: only the first octet stays
    assert cl.mask_ips("203.0.113.9") == "<public IP>"
    assert cl.mask_ips("2606:4700::1111") == "<public IP>"
    assert cl.mask_ips("fe80::1 and ::1") == "fe80::1 and ::1"


def M(i, author, t, reply=None, log=False):
    return {"id": i, "author_id": author, "created": t, "reply_to": reply, "has_log": log}


def test_cluster_time_window_and_replies():
    msgs = [M(1, 10, 1000, log=True), M(2, 10, 1500), M(3, 10, 1000 + 1801),
            M(4, 11, 1200, reply=1), M(5, 12, 1300), M(6, 11, 1250, reply=4),
            M(7, 20, 9000, log=True)]
    out = cl.cluster(msgs, {1: [info("A", "host", "A", 100)], 7: [info("Z", "host", "Z", 900)]})
    assert out == [[1, 4, 6, 2], [7]]


def test_cluster_joins_peer_logs_same_session():
    msgs = [M(1, 10, 1000, log=True), M(2, 11, 90000, log=True), M(3, 12, 95000, log=True)]
    infos = {1: [info("HostA", "host", "HostA", 600, 700, {"GuestB"})],
             2: [info("GuestB", "guest", "HostA", 605, 695, {"HostA"})],
             3: [info("Other", "host", "Other", 605, 695)]}
    assert cl.cluster(msgs, infos) == [[1, 2], [3]]


def test_cluster_joins_existing_bug_on_same_logs():
    # the same two logs posted in two places by two people become one cluster...
    a = info("HostA", "host", "HostA", 600, 700, {"GuestB"})
    b = info("GuestB", "guest", "HostA", 605, 695, {"HostA"})
    msgs = [M(1, 10, 1000, log=True), M(2, 11, 50000, log=True)]
    assert cl.cluster(msgs, {1: [a, b], 2: [a, b]}) == [[1, 2]]
    # ...and a bug that already holds all those files is joined instead of a new one
    index = {"NW-aaaaaaa": {"s1", "s2", "s3"}, "NW-bbbbbbb": {"s9"}}
    assert cl.bug_for_shas(index, ["s1", "s2"]) == "NW-aaaaaaa"
    assert cl.bug_for_shas(index, ["s1", "s9"]) is None
    assert cl.bug_for_shas(index, []) is None


def test_missing_peers():
    infos = [info("HostA", "host", "HostA", 600, peers={"GuestB", "GuestC"})]
    assert cl.missing_peers(infos) == ["GuestB", "GuestC"]
    assert cl.missing_roles(infos) == ["guest"]
    infos = [info("GuestB", "guest", "HostA", 600, peers={"HostA"})]
    assert cl.missing_peers(infos) == ["HostA"]
    assert cl.missing_roles(infos) == ["host"]
    both = infos + [info("HostA", "host", "HostA", 600, peers={"GuestB"})]
    assert cl.missing_peers(both) == [] and cl.missing_roles(both) == []


def test_classify():
    assert cl.classify("hello", {"net.nat"}) == "network"
    assert cl.classify("I can't join my friend, timed out", set()) == "network"
    assert cl.classify("can't join, the game crashes", set()) == "bug"
    assert cl.classify("the lamp is glitched", set()) == "bug"
    assert cl.classify("nothing special", {"other.x"}) == "bug"
    assert cl.classify("radmin firewall?", set()) == "network"
