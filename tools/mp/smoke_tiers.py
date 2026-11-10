"""Smoke tier cut (0.8.1 tools081): which phases the release tier runs and which wait for the nightly tier.

Pure functions, no game: smoke_party.py feeds in its phases, groups, flaky set and per-phase times.

  nightly  4-player-or-more phases (a phase that starts 4+ games), phases that are still FLAKY, and the release
           tier's variants (RELEASE_SKIP). Together with release it is the whole `full` list.
  release  one cheap, stable phase per feature AREA first, then the phases that cover the most still-uncovered
           groups per second, until the time budget (RELEASE_BUDGET_S, 6 h at one lane) is spent.

An AREA is the part of a group name before its first underscore (chalice_altar -> chalice, lift_walk -> lift): the
groups are --only selectors and 186 of 227 hold a single phase, so "every group" is the whole suite; the area is
the unit a player would name ("lifts", "chalice", "NPC fights", "6 players"). The release tier covers every area
that `full` covers (and as many groups as the budget allows).

A flaky phase expires from the nightly tier after FLAKY_EXPIRE_CLEAN clean runs in a row (flaky_history.json next to
phase_times.json, written by smoke_party.py): flaky means "has not shown three clean runs", not "was once flaky".
"""
import json
from pathlib import Path

RELEASE_BUDGET_S = int(5.8 * 3600)   # under the 6 h cap with room for the spread of measured times
FLAKY_EXPIRE_CLEAN = 3   # clean runs in a row after which a phase leaves the flaky set
BIG_COUNT = 4            # a phase that starts this many games or more is nightly


def area_of(group):
    return group.split("_")[0]


def load_history(path):
    try:
        d = json.loads(Path(path).read_text(encoding="utf-8"))
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def record_run(path, name, outcome, today=""):
    """outcome: 'clean' (green on the first try), 'flaky' (green on a retry) or 'fail'. Keeps {phase: {...}} small."""
    h = load_history(path)
    e = h.setdefault(name, {"clean": 0, "runs": 0})
    e["runs"] = e.get("runs", 0) + 1
    e["clean"] = e.get("clean", 0) + 1 if outcome == "clean" else 0
    if outcome != "clean":
        e["last_bad"] = today
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(h, indent=0, sort_keys=True), encoding="utf-8")


def active_flaky(flaky, history, expire=FLAKY_EXPIRE_CLEAN):
    """The flaky phases that have not yet shown `expire` clean runs in a row."""
    return {p for p in flaky if history.get(p, {}).get("clean", 0) < expire}


def build(phases, groups, counts, times, flaky, skip, must=(), budget=RELEASE_BUDGET_S, default_s=300):
    """-> (release list, nightly list), both in `phases` order.

    phases: ordered names; groups: {group: "a,b"}; counts: {phase: games started}; times: {phase: seconds};
    flaky: phases to keep out of release; skip: variants (RELEASE_SKIP) kept out too; must: phases that are in
    release whatever they are (the fast tier: what gates every merge also gates the release)."""
    t = {p: times.get(p, default_s) for p in phases}
    gof = {p: [g for g, ps in groups.items() if p in ps.split(",")] for p in phases}
    aof = {p: {area_of(g) for g in gof[p]} for p in phases}
    big = {p for p in phases if counts.get(p, 3) >= BIG_COUNT}
    out = set(big) | set(flaky) | set(skip)
    pool = [p for p in phases if p not in out]
    chosen = [p for p in phases if p in set(must)]
    areas = set().union(*[aof[p] for p in chosen]) if chosen else set()
    grps = {g for p in chosen for g in gof[p]}
    # 1. every area: the cheapest stable phase of it (an area with only nightly phases gets its cheapest one back,
    #    stable ones first), so release keeps the coverage list of full
    all_areas = set().union(*aof.values()) if aof else set()
    for a in sorted(all_areas):
        if a in areas:
            continue
        mem = [p for p in phases if a in aof[p]]
        stable = [p for p in mem if p in pool]
        best = min(stable or mem, key=lambda p: (p in out, t[p], phases.index(p)))
        if best not in chosen:
            chosen.append(best)
        areas |= aof[best]
        grps |= set(gof[best])
    # 2. fill the budget with the phases that add the most uncovered groups per second (ties: cheaper, earlier)
    used = sum(t[p] for p in chosen)
    left = [p for p in pool if p not in chosen]
    while left:
        def gain(p):
            return len(set(gof[p]) - grps)
        cand = [p for p in left if gain(p) and used + t[p] <= budget]
        if not cand:
            break
        b = max(cand, key=lambda p: (gain(p) / t[p], -phases.index(p)))
        chosen.append(b)
        used += t[b]
        grps |= set(gof[b])
        left.remove(b)
    rel = [p for p in phases if p in set(chosen)]
    return rel, [p for p in phases if p not in set(chosen)]
