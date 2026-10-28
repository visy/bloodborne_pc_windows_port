"""tools/discord/iosefka_voice.py: reply texts."""
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


v = load("iosefka_voice")


def test_replacement_masks_and_fits():
    out = v.replacement("<@123>", "NW-1a2b3c4", "cannot join 203.0.113.9:9307 please help")
    assert "203.0.113.9" not in out and "NW-1a2b3c4" in out and out.startswith("<@123> brought a log")
    assert "Now, go. And good hunting." in out
    note = "(pictures/video kept in the clinic)"
    big = v.replacement("<@123>", "NW-1a2b3c4", "word " * 2000, note)
    assert len(big) <= 2000 and big.endswith(note) and "\n> word" in big
    assert len(v.replacement("<@1>", "NW-1a2b3c4", "x", "n" * 5000)) <= 2000
    assert "@everyone" not in v.replacement("<@1>", "NW-1a2b3c4", "hi @everyone")
    assert v.replacement("<@1>", "NW-1a2b3c4", "") .count(">") == 1  # only the mention's closing bracket


def test_bug_filed_asks_for_missing():
    assert "host's log" in v.bug_filed("NW-1a2b3c4", ["host"])
    assert "guests' logs" in v.bug_filed("NW-1a2b3c4", ["guest"])
    plain = v.bug_filed("NW-1a2b3c4", [])
    assert "NW-1a2b3c4" in plain and "log too" not in plain and "host" not in plain


def test_network_help_masks():
    out = v.network_help("Host at 198.51.100.7:9307 is behind a strict NAT.")
    assert "198.51.100.7" not in out and out.endswith("I'll do what I can.") and len(out) <= 2000
    assert len(v.network_help("y" * 9000)) <= 2000


def test_dm_and_told():
    assert "NW-1a2b3c4" in v.dm_ack("NW-1a2b3c4")
    t = v.told("try again, host 203.0.113.9")
    assert "203.0.113.9" not in t and "try again" in t


def test_announcement_mentions_duties():
    a = v.announcement()
    low = a.lower()
    assert len(a) < 2000 and "@everyone" not in a and "@here" not in a
    assert "triage" in low and "logs" in low and "mention me" in low and "NW id" in a
    assert "private" in low and "other players' logs" in low
