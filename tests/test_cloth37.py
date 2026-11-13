"""tools/mp/cloth37.py: the log parser and the report (GitHub #37); synthetic lines only."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "mp"))
import cloth37  # noqa: E402


def line(rate, variant, phase, inst, n, rmean, trial=1):
    return ("[+ 1.0] Frame sweep: cloth %s %s t%d %s inst %s n %d samples 50 r %.3f/%.3f/%.3f "
            "(lowest min %.3f, highest max %.3f) y 0.500/1.000 (lowest 0.400)"
            % (rate, variant, trial, phase, inst, n, rmean, rmean - 0.1, rmean + 0.1, rmean - 0.1, rmean + 0.1))


def test_parse_lines_and_shots():
    recs, shots = cloth37.parse([
        line("30", "stock", "idle", "abc", 144, 0.3),
        "[+ 2.0] Frame sweep: cloth shot 3 uncap lin,all t1 roll2",
        "unrelated",
    ])
    assert len(recs) == 1 and recs[0]["n"] == 144 and abs(recs[0]["rmean"] - 0.3) < 1e-9
    assert shots == [dict(index=3, rate="uncap", variant="lin,all", trial=1, phase="roll2")]


def test_big_instances_skip_hair():
    recs, _ = cloth37.parse([line("30", "stock", "idle", "a1", 24, 0.1), line("30", "stock", "idle", "b2", 144, 0.3),
                             line("30", "stock", "idle", "c3", 234, 0.3)])
    assert cloth37.big_instances(recs) == ["c3", "b2"]


def test_report_distance_to_reference():
    lines = []
    for variant, off in (("stock", 0.1), ("fix", 0.0)):
        lines.append(line("30", variant, "idle", "b2", 144, 0.3))
        lines.append(line("60", variant, "idle", "b2", 144, 0.3 + off))
    recs, _ = cloth37.parse(lines)
    tb = cloth37.table(recs, "b2")
    assert cloth37.distance(tb[("fix", "60")], tb[("fix", "30")]) < 1e-9
    assert cloth37.distance(tb[("stock", "60")], tb[("stock", "30")]) > 0.05
    text = cloth37.report(recs)
    assert "stock" in text and "fix" in text and "144 particles" in text


def test_rate_key_orders_uncap_last():
    assert sorted(["uncap", "120", "30", "90"], key=cloth37.rate_key) == ["30", "90", "120", "uncap"]
