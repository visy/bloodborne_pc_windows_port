#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boss rooms and co-op walls from the EMEVD scripts (party co-op; gpu/shim/party/party_bossfog_rules.h).

Game data (read-only, untrusted; run with python -I). EVENT = .../dvdroot_ps4/event.

  python -I tools/party/bossroom_tool.py scan EVENT
      walls: every event that puts up an object (2005[3](obj, 1)) or its SFX (2006[2]) right after
        a wait on the multiplayer state (3[6]); the events that only make an SFX (summon signs) are
        listed apart. Per wall: the wall / SFX instruction indexes and, when the event's take-down
        wait has a "not in multiplayer" group next to a flag (0[0] + 3[0]), that 0[0]'s index and
        the flag (bossfog::ConfinementWall).
      first fights: every host first-fight event (`ホストがボス部屋入場_初戦` / `_初回`, the DLC's
        two parts) and how it starts: `entry` (only 3[2] region / 3[3] distance next to flag and
        chr-type checks), `action` (an item held 3[4], a hit 3[23], or flag-only: waits on another
        event), with each re-fight event's `1003[5] skip 2 if client` index (bossfog::FirstFight).
  python -I tools/party/bossroom_tool.py check EVENT [--header gpu/shim/party/party_bossfog_rules.h]
      The walls the scan finds against ConfinementWalls() in the header; exit 1 on a difference.
"""
import argparse, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402


def decoded(edf, e):
    out = []
    for bank, iid, raw in e["ins"]:
        _n, args = edf.get((bank, iid), ("?", []))
        out.append((bank, iid, ft.unpack_args([t for _, t in args], bytes(raw))))
    return out


def events(event_dir):
    edf = ft.load_emedf(ft.find_emedf(event_dir))
    for stem, p, eld in ft.event_files(event_dir):
        names = ft.load_emeld(eld)
        for e in ft.Emevd(ft.read_maybe_dcx(p)).events:
            yield stem, e["id"], names.get(e["id"], ""), decoded(edf, e)


def wall_of(ins):
    """(wall index, SFX index, alone index, until flag) or None; sfx_only marks summon-sign SFX."""
    mp = False
    for k, (b, i, v) in enumerate(ins):
        if (b, i) == (3, 6):
            mp = True
        if (b, i) == (0, 0) and v and v[0] == 0 and mp:
            on = sfx = None
            for j in range(k + 1, len(ins)):
                bj, ij, vj = ins[j]
                if bj in (0, 1, 3, 4, 5):
                    break
                if (bj, ij) == (2005, 3) and len(vj) > 1 and vj[1] == 1 and on is None:
                    on = j
                if (bj, ij) == (2006, 2) and sfx is None:
                    sfx = j
            if on is not None or sfx is not None:
                alone, flag = -1, 0
                # the take-down wait: a group built from "a multiplayer group is false" and a flag ON
                for j in range(k + 1, len(ins)):
                    bj, ij, vj = ins[j]
                    if (bj, ij) == (0, 0) and len(vj) >= 3 and vj[0] > 0 and vj[1] == 0 and vj[2] < 0:
                        for m in range(j + 1, min(j + 3, len(ins))):
                            bm, im, vm = ins[m]
                            if (bm, im) == (3, 0) and len(vm) >= 4 and vm[0] == vj[0] and vm[1] == 1:
                                alone, flag = j, vm[3]
                return on, sfx, alone, flag
            mp = False
    return None


def first_kind(ins):
    conds = {(b, i) for b, i, _v in ins if b in (3, 4, 5)}
    if (3, 4) in conds or (3, 23) in conds or (3, 24) in conds:
        return "action"
    if (3, 2) in conds or (3, 3) in conds:
        return "entry"
    return "action"  # flag-only: waits on another event (Lady Maria: 13500947)


def scan(event_dir):
    walls, signs, firsts, refights = [], [], [], []
    for stem, eid, name, ins in events(event_dir):
        w = wall_of(ins)
        if w:
            (walls if w[0] is not None else signs).append((stem, eid, name, w))
        if stem.startswith("m29") or stem == "common":
            continue
        if re.search("ホストがボス部屋入場_初(戦|回)", name):
            firsts.append((stem, eid, name, first_kind(ins)))
        if "ホストがボス部屋入場_再戦" in name:
            skip = next((k for k, (b, i, v) in enumerate(ins) if (b, i) == (1003, 5) and v[:2] == [2, 1]), -1)
            refights.append((stem, eid, name, skip))
    return walls, signs, firsts, refights


def cmd_scan(a):
    sys.stdout.reconfigure(encoding="utf-8")
    walls, signs, firsts, refights = scan(a.event)
    print("co-op walls (bossfog::ConfinementWalls {event, wall, SFX, alone, until flag}):")
    for stem, eid, name, (on, sfx, alone, flag) in walls:
        print(f"  {{{eid}, {on}, {sfx if sfx is not None else -1}, {alone}, {flag}}},  // {stem} {name}")
    print(f"summon-sign SFX events (no wall): {len(signs)}")
    print("first fights (host first-fight events):")
    for stem, eid, name, kind in firsts:
        print(f"  {stem} {eid} {kind:6} {name}")
    print("re-fight events ('1003[5] skip 2 if client' index):")
    for stem, eid, name, skip in refights:
        print(f"  {stem} {eid} [{skip}] {name}")


def cmd_check(a):
    walls, _s, _f, _r = scan(a.event)
    found = {(eid, on, sfx if sfx is not None else -1, alone, flag) for _st, eid, _n, (on, sfx, alone, flag) in walls}
    text = Path(a.header).read_text(encoding="utf-8")
    body = text[text.index("ConfinementWalls(std::size_t* n)"):]
    body = body[:body.index("};")]
    have = {tuple(int(x) for x in m.groups()) for m in
            re.finditer(r"\{(\d+), (-?\d+), (-?\d+), (-?\d+), (\d+)\}", body)}
    ok = found == have
    for r in sorted(found - have):
        print("missing in the header:", r)
    for r in sorted(have - found):
        print("not in the game data:", r)
    print(f"co-op walls: {len(found)} in the data, {len(have)} in the header: {'same' if ok else 'DIFFERENT'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("event")
    c = sub.add_parser("check")
    c.add_argument("event")
    c.add_argument("--header", default=str(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_bossfog_rules.h"))
    a = ap.parse_args()
    return cmd_scan(a) if a.cmd == "scan" else cmd_check(a)


if __name__ == "__main__":
    sys.exit(main() or 0)
