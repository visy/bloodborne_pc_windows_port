#!/usr/bin/env python3
"""Bloodborne 1.09 lifts against the party's lift table (liftfix, gpu/shim/party/party_lift_rules.h).

Game data (read-only, untrusted; run with python -I). GAME = .../dvdroot_ps4.

  python -I tools/party/lift_tool.py check GAME [--rules gpu/shim/party/party_lift_rules.h]

Scans every map's EMEVD (not the Chalice Dungeons' m29 layouts, whose lifts are generated: the table
leaves them to the fall recovery's rate rule) for lift rides: an event named エレベ.. (elevator) that
waits for the player in a region (3[2] on entity 10000), plays an object animation (2003[18]) and
writes event flags. Each ride's objects must include a car of the table, with its position flag among
the flags the ride writes; every car of the table must have a ride; the rides whose end waits for the
local player alone (3[2] on 10000, outside) must be the rules' kLocalEndRideEvents (made all-players).
Then it lists the events that look like a ride but are not named one (a region test of the player, a
waited object animation and a flag write) for a reader to look through: none is a lift in 1.09 (boss
doors, traps, NPC summons).
Exit status 0: the table covers every lift; 1: a lift or a car is missing, or the end-check list
differs; 2: an error.
"""
import argparse, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402

ELEVATOR = "エレベ"
ROW = re.compile(r"\{\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)")


def table(rules):
    """[(pos, busy, host_state, car)] of Lifts() in the rules header."""
    text = Path(rules).read_text(encoding="utf-8")
    body = text[text.index("Lifts() {"):text.index("return lifts;")]
    return [tuple(int(x) for x in m.groups()) for m in ROW.finditer(body)]


def local_end_events(rules):
    """kLocalEndRideEvents of the rules header: the lifts whose end check the party rewrites."""
    text = Path(rules).read_text(encoding="utf-8")
    m = re.search(r"kLocalEndRideEvents\[\] = \{([^}]*)\}", text)
    return {int(x) for x in re.findall(r"\d+", m.group(1))} if m else set()


def rides(event_dir):
    """{(stem, event id, slot): (name, anims, waited, flags, region)} of every event of the main game."""
    edf = ft.load_emedf(ft.find_emedf(event_dir))
    out = {}
    for stem, p, eld in ft.event_files(event_dir):
        if stem.startswith("m29") or stem == "common":
            continue
        names = ft.load_emeld(eld)
        ev = ft.Emevd(ft.read_maybe_dcx(p))
        calls = {}
        for e in ev.events:
            for bank, iid, raw in e["ins"]:
                if (bank, iid) == (2000, 0) and len(raw) >= 8:
                    slot, target = struct.unpack_from("<iI", raw, 0)
                    calls.setdefault(target, []).append((slot, raw[8:]))
        for e in ev.events:
            for slot, pbuf in calls.get(e["id"], [(0, None)]):
                ins = [[bk, ii, bytearray(raw)] for bk, ii, raw in e["ins"]]
                for ii, tgt, src, cnt in e["params"]:
                    if pbuf is not None and src + cnt <= len(pbuf):
                        ins[ii][2][tgt:tgt + cnt] = pbuf[src:src + cnt]
                vals = []
                for bank, iid, raw in ins:
                    _n, args = edf.get((bank, iid), ("?", []))
                    vals.append((bank, iid, ft.unpack_args([t for _, t in args], bytes(raw))))
                region = any((b, i) == (3, 2) and len(v) >= 4 and v[2] == 10000 for b, i, v in vals)
                anims = {v[0] for b, i, v in vals if (b, i) == (2003, 18) and v and v[0] not in (0, 10000)}
                waited = {v[0] for b, i, v in vals if (b, i) == (2003, 18) and len(v) >= 4 and v[3] == 1 and
                          v[0] not in (0, 10000)}
                flags = {v[0] for b, i, v in vals if (b, i) == (2003, 2) and v and v[0] > 0}
                # the ride's end: a wait for the local player out of a region after the first animation
                first = next((k for k, (b, i, _v) in enumerate(vals) if (b, i) == (2003, 18)), len(vals))
                local_end = any((b, i) == (3, 2) and len(v) >= 4 and v[1] == 0 and v[2] == 10000
                                for b, i, v in vals[first:])
                out[(stem, e["id"], slot)] = (names.get(e["id"], ""), anims, waited, flags, region, local_end)
    return out


def cmd_check(a):
    lifts = table(a.rules)
    by_car = {car: (pos, busy, hs) for pos, busy, hs, car in lifts}
    found = {}
    missing = []
    candidates = []
    rewritten = local_end_events(a.rules)
    local_ends = set()
    for (stem, eid, slot), (name, anims, waited, flags, region, local_end) in sorted(rides(Path(a.game) / "event").items()):
        if not (region and anims and flags):
            continue
        if local_end and ELEVATOR in name:
            local_ends.add(eid)
        if ELEVATOR not in name:
            if waited:
                candidates.append((stem, eid, slot, name, sorted(waited), sorted(flags)))
            continue
        cars = [c for c in anims if c in by_car and by_car[c][0] in flags]
        if cars:
            for c in cars:
                found.setdefault(c, []).append((stem, eid, slot))
        else:
            missing.append((stem, eid, slot, name, sorted(anims), sorted(flags)))
    print("lift table: %d lifts (%s)" % (len(lifts), Path(a.rules).name))
    for pos, busy, hs, car in lifts:
        evs = found.get(car, [])
        print("  car %d pos %d busy %d host %d: %s" % (car, pos, busy, hs, ", ".join("%s %d" % (s, e) for s, e, _ in evs)
                                                     or "NO RIDE FOUND"))
    for m in missing:
        print("MISSING: %s event %d slot %d %s: objects %s, flags %s" % m)
    print("events that look like a ride but are not named one (check by hand): %d" % len(candidates))
    for c in candidates:
        print("  %s event %d slot %d %s: waits on %s, flags %s" % c)
    print("rides that end once the LOCAL player is out of the car: %s (the rules rewrite: %s)"
          % (sorted(local_ends) or "-", sorted(rewritten) or "-"))
    bad = missing or any(car not in found for _p, _b, _h, car in lifts) or local_ends != rewritten
    print("RESULT: %s" % ("a lift or a car is missing" if bad else "the table covers every lift"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("game")
    c.add_argument("--rules", default=str(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_lift_rules.h"))
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    try:
        return cmd_check(a)
    except (OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
