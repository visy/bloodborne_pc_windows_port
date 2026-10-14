"""tools/mp/fake_dht.py: the stand-in DHT node used by live NAT-simulator runs (F2)."""
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "mp"))
import fake_dht  # noqa: E402


def test_bencode_round_trip():
    v = {b"t": b"aa", b"y": b"q", b"a": {b"id": b"x" * 20, b"port": 5}, b"l": [1, b"z"]}
    data = fake_dht.bencode(v)
    assert fake_dht.bdecode(data) == (v, len(data))


def test_announce_then_get_peers():
    n = fake_dht.Node()
    ih = b"h" * 20
    guest = ("127.0.0.1", 50000)
    gp = fake_dht.bencode({b"t": b"01", b"y": b"q", b"q": b"get_peers", b"a": {b"id": b"g" * 20, b"info_hash": ih}})
    r, _ = fake_dht.bdecode(n.handle(gp, guest))
    token = r[b"r"][b"token"]
    assert b"values" not in r[b"r"]
    bad = fake_dht.bencode({b"t": b"02", b"y": b"q", b"q": b"announce_peer",
                            b"a": {b"id": b"g" * 20, b"info_hash": ih, b"port": 4321, b"token": b"nope"}})
    assert fake_dht.bdecode(n.handle(bad, guest))[0][b"y"] == b"e"
    ann = fake_dht.bencode({b"t": b"03", b"y": b"q", b"q": b"announce_peer",
                            b"a": {b"id": b"g" * 20, b"info_hash": ih, b"port": 4321, b"token": token}})
    assert fake_dht.bdecode(n.handle(ann, guest))[0][b"y"] == b"r"
    r, _ = fake_dht.bdecode(n.handle(gp, ("127.0.0.1", 9307)))
    assert r[b"r"][b"values"] == [socket.inet_aton("127.0.0.1") + (4321).to_bytes(2, "big")]
    assert n.handle(b"garbage", guest) is None
