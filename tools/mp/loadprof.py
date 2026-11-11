#!/usr/bin/env python3
"""Summarise a BB_LOAD_PROFILE=1 game log (cwprof081): one table row per load (loading screen up -> gone) with where
the seconds went (the InGameStep / MoveMapStep steps grouped into phases), the busiest threads over the screen, the
main thread's busy / blocked split of the longest step, and the world block states of the load.

  python tools/mp/loadprof.py run.log [run2.log ...] [--loads] [--steps] [--blocks] [--samples]

Phases: pre = InGameStep 8 / 2 / 4 / 6 and MoveMapStep 0..3 (one-frame steps), 3-9 = the stage / placement / chr
steps, 9-10 = the world-ready wait (0x19E8190), 10-13 = ground probe, first chr update, screen gate, InGameStart,
post = from MoveMapStep 13 to the screen's "gone" (fade-in).
"""
import argparse, re, statistics, sys
from collections import defaultdict
from pathlib import Path

STEP = re.compile(r"Load trace: \[\s*([\d.]+) s\] (InGameStep|MoveMapStep) (-?\d+) -> (-?\d+) after (\d+) ms \((\d+) frames\)"
                  r".*?cpu ([\d.]+) s(.*)$")
UP = re.compile(r"Load profile: \[\s*([\d.]+) s\] SCREEN UP")
GONE = re.compile(r"Load profile: \[\s*([\d.]+) s\] SCREEN GONE after (\d+) ms, (\d+) frames \((\d+) fps\); drop -> gone (\d+) ms;"
                  r" io (\d+) reads, ([\d.]+) MB, (\d+) ms in reads; gpu (\d+) compiles (\d+) ms, (\d+) images, ([\d.]+) MB uploaded")
THREADS = re.compile(r"Load profile: threads over the screen \(ms cpu; (\d+) ms wall, ([\d.]+) cores busy\): (.*)$")
BLOCK = re.compile(r"Load profile: \[\s*([\d.]+) s\] block (m\d\d_\d\d) \(res (\d+)\) state (-?\d+) \S+ -> (\d+) (\S+) after (\d+) ms;"
                   r" selected (\d)")
COLL = re.compile(r"Load profile: \[\s*([\d.]+) s\] collision: .*?(\d+) requested, (\d+) not ready")
MAINBUSY = re.compile(r"main busy (\d+) ms \((\d+)%\), blocked (\d+) ms")
SAMPLES = re.compile(r"Load profile: samples of (\S+ \d+) \((\d+) ms\):")
SAMPLE = re.compile(r"Load trace: samples (main )?thread (\d+)(?: \((.*?)\))?: (\d+) \((\d+) in host modules\): (.*)$")
LAMP = re.compile(r"test: travel step \d+/\d+: host_lamp=(\d+)|lamp (\d+)")


def phase_of(kind, frm, to):
    if kind == "InGameStep":
        return "pre"
    if to <= 3:
        return "pre"
    if to <= 9:
        return "3-9"
    if to == 10:
        return "9-10"
    return "10-13"


def parse(path):
    loads, cur, last_samples_step = [], None, None
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        line = re.sub(r"^\[\+\s*[\d.]+\]\s?", "", raw)
        m = UP.search(line)
        if m:
            cur = {"up": float(m.group(1)), "phases": defaultdict(float), "frames": defaultdict(int), "steps": [],
                   "blocks": [], "threads": "", "cores": 0.0, "coll": [], "samples": defaultdict(list), "longest": None}
            continue
        m = THREADS.search(line)
        if m and loads:
            loads[-1]["cores"] = float(m.group(2))
            loads[-1]["threads"] = m.group(3)
            continue
        if cur is None:
            continue
        m = STEP.search(line)
        if m:
            t, kind, frm, to, ms, frames, cpu, rest = m.groups()
            ph = phase_of(kind, int(frm), int(to))
            cur["phases"][ph] += int(ms) / 1e3
            cur["frames"][ph] += int(frames)
            mb = MAINBUSY.search(rest)
            step = {"t": float(t), "kind": kind, "from": int(frm), "to": int(to), "ms": int(ms), "frames": int(frames),
                    "cpu": float(cpu), "busy": int(mb.group(1)) if mb else None, "blocked": int(mb.group(3)) if mb else None,
                    "threads": rest}
            cur["steps"].append(step)
            if cur["longest"] is None or step["ms"] > cur["longest"]["ms"]:
                cur["longest"] = step
            if kind == "MoveMapStep" and int(to) == 13:
                cur["t13"] = float(t)
            continue
        m = BLOCK.search(line)
        if m:
            cur["blocks"].append((float(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6),
                                  int(m.group(7)), int(m.group(8))))
            continue
        m = COLL.search(line)
        if m:
            cur["coll"].append((float(m.group(1)), int(m.group(2)), int(m.group(3))))
            continue
        m = SAMPLES.search(line)
        if m:
            last_samples_step = m.group(1)
            continue
        m = SAMPLE.search(line)
        if m and last_samples_step:
            cur["samples"][last_samples_step].append((m.group(3) or ("main" if m.group(1) else "thread"), int(m.group(4)),
                                                      int(m.group(5)), m.group(6)))
            continue
        m = GONE.search(line)
        if m:
            cur["gone"] = float(m.group(1))
            cur["screen"] = int(m.group(2)) / 1e3
            cur["fps"] = int(m.group(4))
            cur["fade_in"] = int(m.group(5)) / 1e3
            cur["reads"] = int(m.group(6))
            cur["mb"] = float(m.group(7))
            cur["read_ms"] = int(m.group(8))
            cur["compiles"] = int(m.group(9))
            cur["upload_mb"] = float(m.group(12))
            if "t13" in cur:
                cur["phases"]["post"] = cur["gone"] - cur["t13"]
            loads.append(cur)
            cur = None
    return loads


def dest(load):
    """The map the load went to: the block the load selected (selected 1; a neighbour is desired but not selected)."""
    for b in load["blocks"]:
        if b[7] == 1 and b[4] >= 1:
            return b[1]
    res = [b for b in load["blocks"] if b[4] == 7]
    return res[-1][1] if res else "?"


def thread_groups(threads):
    """Sum the per-thread ms by family: main, Core.Res.PP (post-processors), Core.Res.IO (the loaders / task manager),
    CSChrThread, render / GX, ours. The game's thread names are cut at 15 characters by the runtime."""
    out = defaultdict(float)
    for part in threads.split(", "):
        m = re.match(r"(.*) (\d+) (\d+)$", part.strip())
        if not m:
            continue
        name, ms = m.group(1), float(m.group(3))
        fam = ("main" if name == "main" else "Core.Res.PP" if name.startswith("Core.Res.PostPr") else
               "Core.Res.IO" if name.startswith("Core.Res") else "CSChr" if name.startswith("CSChrThread") else
               "Job" if name.startswith("Job") or name.startswith("CSJob") else name.split("_")[0][:14])
        out[fam] += ms
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--steps", action="store_true", help="every step of every load")
    ap.add_argument("--blocks", action="store_true", help="the block / collision state lines of every load")
    ap.add_argument("--samples", action="store_true", help="the sampler's program counters per step")
    ap.add_argument("--threads", action="store_true", help="the full per-thread table of every load")
    a = ap.parse_args()
    for log in a.logs:
        loads = parse(log)
        print("== %s: %d loads" % (log, len(loads)))
        print("%-3s %-7s %6s | %5s %5s %6s %6s %5s | %4s %6s %6s %5s | %s" % (
            "#", "map", "screen", "pre", "3-9", "9-10", "10-13", "post", "fps", "MB", "rd ms", "cores", "longest step: main busy / blocked"))
        for i, l in enumerate(loads):
            lg = l["longest"]
            busy = ("%s %d->%d %d ms: busy %s blocked %s" % (lg["kind"][:4], lg["from"], lg["to"], lg["ms"], lg["busy"], lg["blocked"])
                    if lg else "-")
            print("%-3d %-7s %6.2f | %5.2f %5.2f %6.2f %6.2f %5.2f | %4d %6.0f %6d %5.2f | %s" % (
                i + 1, dest(l), l.get("screen", 0), l["phases"]["pre"], l["phases"]["3-9"], l["phases"]["9-10"],
                l["phases"]["10-13"], l["phases"].get("post", 0), l.get("fps", 0), l.get("mb", 0), l.get("read_ms", 0),
                l["cores"], busy))
            fam = thread_groups(l["threads"])
            if fam:
                print("    threads (ms cpu): " + ", ".join("%s %.0f" % (k, v) for k, v in sorted(fam.items(), key=lambda kv: -kv[1])[:8]))
            if a.threads:
                print("    " + l["threads"])
            if a.steps:
                for s in l["steps"]:
                    print("    [%8.3f] %-11s %2d -> %2d %5d ms %3d fr cpu %5.2f s%s" % (
                        s["t"], s["kind"], s["from"], s["to"], s["ms"], s["frames"], s["cpu"],
                        (" busy %d blocked %d" % (s["busy"], s["blocked"])) if s["busy"] is not None else ""))
            if a.blocks:
                for b in l["blocks"]:
                    print("    [%8.3f] block %s res %d: %d -> %d %s after %d ms (selected %d)" % b)
                for c in l["coll"]:
                    print("    [%8.3f] collision: %d requested, %d not ready" % c)
            if a.samples:
                for step, rows in l["samples"].items():
                    print("    samples %s:" % step)
                    for name, total, host, pcs in rows:
                        print("      %-26s %5d (%4d host): %s" % (name, total, host, pcs[:300]))
        by_map = defaultdict(list)
        for l in loads:
            by_map[dest(l)].append(l)
        print("per map (mean): map n screen pre 3-9 9-10 10-13 post")
        for mp, ls in sorted(by_map.items()):
            print("  %-7s %2d %6.2f %5.2f %5.2f %6.2f %6.2f %5.2f" % (
                mp, len(ls), statistics.mean(l.get("screen", 0) for l in ls), statistics.mean(l["phases"]["pre"] for l in ls),
                statistics.mean(l["phases"]["3-9"] for l in ls), statistics.mean(l["phases"]["9-10"] for l in ls),
                statistics.mean(l["phases"]["10-13"] for l in ls), statistics.mean(l["phases"].get("post", 0) for l in ls)))


if __name__ == "__main__":
    main()
