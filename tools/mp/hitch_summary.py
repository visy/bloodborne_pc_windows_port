#!/usr/bin/env python3
"""Walking summary of hitch_route runs, one line per run (door082):

  python tools/mp/hitch_summary.py OUT_DIR [OUT_DIR ...]

Per run (the --out folder of tools/mp/hitch_route.py run): the walking frames (route start + 1 s .. its end, as the
report's "ALL walk") with their mean and 99th percentile frame time, the frames over 25 / 45 / 60 ms, the worst,
buffer uploads and page protection per frame, the draw thread's CPU per frame, and the Vulkan memory and the guest
blocks in VRAM (means of the "Memory:" lines while walking). Below each run: the walking frames over 45 ms and the
frames that uploaded more than 40 MB of buffers (the first-draw bursts).
"""
import csv, re, sys
from pathlib import Path

TS = re.compile(r"^\[\+\s*(\d+(?:\.\d+)?)\]\s?(.*)$")


def load(run):
    out = Path(run)
    log = (out / "inst0.log").read_text(errors="replace").splitlines()
    loads, legs, cur = [], [], None
    mem = []
    for line in log:
        m = TS.match(line)
        if not m:
            continue
        t, s = float(m.group(1)), m.group(2)
        if s.startswith("Stall:"):
            loads.append((t, s))
        if "test: host_route:" in s:
            cur = t
        elif cur is not None and ("test: route: DONE" in s or "test: route: STOPPED" in s):
            legs.append((cur, t))
            cur = None
        if s.startswith("Memory: VRAM"):
            q = re.search(r"Vulkan memory (\d+) MiB allocated.*guest blocks in VRAM (\d+) MiB allocated.*usage (\d+) MiB", s)
            if q:
                mem.append((t, int(q.group(1)), int(q.group(2)), int(q.group(3))))
    rows = [r for r in csv.DictReader(open(out / "frames.csv")) if None not in r.values()]
    stall_ms = [(t, float(re.search(r"Stall: ([\d.]+) ms", s).group(1))) for t, s in loads]
    long_rows = [(float(r["t_s"]), float(r["frame_ms"])) for r in rows if float(r["frame_ms"]) > 40.0]
    offs, j = [], 0
    for t, ms in stall_ms:
        while j < len(long_rows) and abs(long_rows[j][1] - ms) > 0.06:
            j += 1
        if j >= len(long_rows):
            break
        offs.append(t - long_rows[j][0])
        j += 1
    offs.sort()
    off = offs[len(offs) // 2] if offs else 0.0
    return rows, off, legs, mem


def main():
    print("%-24s %6s %7s %6s %5s %5s %5s %7s %7s %8s %8s %8s %7s %7s" % (
        "run", "walk s", "frames", "mean", ">25", ">45", ">60", "worst", "p99", "bufMB/f", "prot ms", "gpuCPU", "VkMiB", "guestMiB"))
    for run in sys.argv[1:]:
        rows, off, legs, mem = load(run)
        sel = []
        for r in rows:
            t = float(r["t_s"]) + off
            if any(a + 1.0 <= t <= b for a, b in legs):
                sel.append(r)
        if not sel:
            print(run, "no walking frames")
            continue
        ms = sorted(float(r["frame_ms"]) for r in sel)
        n = len(ms)
        over = lambda x: sum(1 for m in ms if m > x)  # noqa: E731
        buf = sum(float(r["buffer_mb"]) for r in sel) / n
        prot = sum(float(r["protect_ms"]) for r in sel) / n
        cpu = sum(float(r["stage_a_cpu_ms"]) for r in sel) / n
        wm = [m for m in mem if any(a <= m[0] <= b for a, b in legs)]
        vk = sum(m[1] for m in wm) / max(1, len(wm))
        gb = sum(m[2] for m in wm) / max(1, len(wm))
        print("%-24s %6.0f %7d %6.2f %5d %5d %5d %7.1f %7.1f %8.2f %8.2f %8.2f %7.0f %7.0f" % (
            Path(run).name, sum(b - a - 1 for a, b in legs), n, sum(ms) / n, over(25), over(45), over(60), ms[-1], ms[int(n * 0.99)],
            buf, prot, cpu, vk, gb))
        big = [(float(r["t_s"]) + off, float(r["frame_ms"]), float(r["buffer_mb"]), int(r["images"]), float(r["protect_ms"]),
                float(r["alloc_mb"]), float(r["write_fault_ms"])) for r in sel if float(r["frame_ms"]) > 45 or float(r["buffer_mb"]) > 40]
        for b in big:
            print("      t=%.1f %.1f ms buf %.0f MB images %d protect %.1f ms alloc %.0f MB wf %.1f ms" % b)


main()
