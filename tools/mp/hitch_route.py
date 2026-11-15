#!/usr/bin/env python3
"""Fixed-place hitch route (hitch082): one scripted, unattended solo walk through the places where players
report a 70-140 ms hitch at the same spot every run, with the frame log and the stall probe on.

  python tools/mp/hitch_route.py run    --out DIR [--fps 60] [--passes 2] [--env K=V ...] [--legs clinic,tomb,ward]
  python tools/mp/hitch_route.py report --out DIR [--ms 45]

Legs (all by the in-process director; lamp warp = the game's own loading screen, the walks by the pad stick over
the navmesh, coordinates = lamp spots / navmesh points computed from the player's own game data):
  clinic  1st Floor Sickroom lamp -> down the clinic, out, up to the Central Yharnam lamp
  tomb    Tomb of Oedon lamp -> the stairs east of it -> towards the chapel tower
  ward    Cathedral Ward lamp (Oedon Chapel) -> out of the chapel -> up the Cathedral Ward stairs
Each leg waits WAIT s after its load (default 12) so load warm-up is not counted, and --passes repeats the legs
in one game session (a hitch that is gone on the second pass was a first-use cost, one that stays is streaming).

report: every frame over --ms from OUT/frames.csv (BB_FRAME_LOG) with the player's position at that time (the
director's log_pos lines), the leg, and the "Stall probe:" line that covers it; hitches are grouped by place
(4 m cells) across passes.
"""
import argparse, bisect, csv, os, re, subprocess, sys, time
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent

LAMP_SICKROOM, LAMP_TOMB, LAMP_WARD = 2412950, 2412953, 2402950
LEGS = {
    "clinic": (LAMP_SICKROOM, "m24_01",
               "-198.4:-50.6:170.6/-198.4:-50.6:161.7/-197.6:-50.6:154.5/-193.9:-50.6:146.2/-190.1:-50.7:139.5/"
               "-189.1:-50.7:131.5/-189.4:-47.4:123.4/-189.9:-47.6:115.0/-190.5:-47.7:105.1/-184.4:-46.7:99.6/"
               "-186.9:-45.5:90.5/-187.9:-44.2:84.4/-187.0:-33.6:74.6/-191.9:-28.6:71.4/-192.3:-28.6:69.4"),
    "tomb": (LAMP_TOMB, "m24_01", "-24.0:-40.8:88.0/-14.0:-35.5:93.0/-12.0:-31.0:98.5"),
    "ward": (LAMP_WARD, "m24_0",
             "8.9:-10.2:107.8/3.4:-11.0:109.5/-3.9:-9.9:115.2/-10.8:-10.4:120.5/-10.2:-11.1:128.3/-6.1:-9.3:137.1/"
             "-2.1:-4.8:146.2/1.7:-2.7:154.0/5.5:0.7:160.9/7.4:0.9:170.2/11.1:0.7:173.3/6.0:0.1:185.0/10.0:1.8:192.9/"
             "9.0:5.3:200.9/8.3:5.4:206.7/-0.6:10.3:211.7/-6.2:11.7:217.0"),
}


def chain(legs, passes, wait):
    steps = ["log_pos=0.2"]
    first = True
    for _ in range(passes):
        for leg in legs:
            lamp, m, route = LEGS[leg]
            steps.append("host_lamp=%d@%d" % (lamp, 20 if first else 6))
            steps.append("host_route=%s@%d%%%s" % (route, wait, m))
            first = False
    steps.append("host_lamp=%d@6" % LAMP_TOMB)   # the end marker's step
    return ",".join(steps), len(steps) - 1


def run(a):
    root = Path(a.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    legs = a.legs.split(",")
    ch, n = chain(legs, a.passes, a.wait)
    secs = a.passes * len(legs) * (a.wait + 150) + 240
    (root / "noreap").write_text(str(secs + 300))
    (out / "settings.txt").write_text("fps=%s passes=%d legs=%s env=%s\nchain=%s\n" % (a.fps, a.passes, a.legs, a.env or [], ch))
    p = root / "inst0" / "run.log"
    start = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
    frames = out / "frames.csv"
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "1", "--fresh-saves", "--timestamps",
           "--idle", "0", "--seconds", str(secs), "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--fps", a.fps, "--vram", str(a.vram), "--label", "hitch route", "--party",
           "--env", "BB_FRAME_STATS=1", "--env", "BB_FRAME_LOG=%s" % frames, "--env", "BB_STALL_PROBE=%d" % a.probe_ms,
           "--env", "BB_STALL_PROBE_DUMP=%s" % (out / "stacks.txt"),
           "--env", "BB_PAD_FILE=%s" % (root / "pad{i}.txt"), "--env", "BB_PARTY_DIRECTOR_TEST_SOLO=1",
           "--ienv", "0:BB_PARTY_DIRECTOR_TEST=" + ch,
           "--until", r"test: travel step %d/%d: host_lamp" % (n, n), "--until-hold", "25"]
    for e in a.env or []:
        cmd += ["--env", e]
    (root / "pad0.txt").write_text("lx=128 ly=128\n")
    env = dict(os.environ, BB_MP_LEASE=str(secs + 300))
    shooter = subprocess.Popen([sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(out / "shots"),
                                "--tag", "hitch", "--every", "20", "--grey", "--seconds", str(secs)],
                               stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        shooter.terminate()
    print("[hitch] harness exit %d after %.0f s" % (r.returncode, time.time() - t0), flush=True)
    if p.exists():
        with open(p, "rb") as f:
            f.seek(start[1] if os.fstat(f.fileno()).st_ino == start[0] else 0)
            (out / "inst0.log").write_bytes(f.read())
    try:
        (root / "noreap").unlink()
    except OSError:
        pass
    report(a)


TS = re.compile(r"^\[\+\s*(\d+(?:\.\d+)?)\]\s?(.*)$")
POS = re.compile(r"pos area \w+ \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)")
STEP = re.compile(r"test: travel step (\d+)/\d+: (\w+)")


def report(a):
    out = Path(a.out).resolve()
    log = (out / "inst0.log").read_text(errors="replace").splitlines()
    pos, steps, probes, loads = [], [], [], []
    first_stats = None
    for line in log:
        m = TS.match(line)
        if not m:
            continue
        t, s = float(m.group(1)), m.group(2)
        if "test: pos" in s or "log_pos" in s:
            p = POS.search(s)
            if p:
                pos.append((t, tuple(float(p.group(i)) for i in (1, 2, 3))))
        elif (q := STEP.search(s)):
            steps.append((t, int(q.group(1)), q.group(2)))
        elif s.startswith("Stall probe:"):
            probes.append((t, s))
        elif s.startswith("Stall:"):
            loads.append((t, s))
        if first_stats is None and s.startswith("Frame stats:"):
            first_stats = t
    rows = [r for r in csv.DictReader(open(out / "frames.csv")) if None not in r.values()]   # the last row may be cut
    if not rows:
        print("no frames")
        return
    # The frame log's clock starts at its first flip; the harness clock at the game's start. Anchor: every
    # "Stall:" line is printed at the flip it describes, so its harness time minus its frame-log time is the offset
    # (median over all of them; pipe delay is a few ms).
    stall_ms = [(t, float(re.search(r"Stall: ([\d.]+) ms", s).group(1))) for t, s in loads]
    long_rows = [(float(r["t_s"]), float(r["frame_ms"])) for r in rows if float(r["frame_ms"]) > 40.0]
    offs = []
    j = 0
    for t, ms in stall_ms:
        while j < len(long_rows) and abs(long_rows[j][1] - ms) > 0.06:
            j += 1
        if j >= len(long_rows):
            break
        offs.append(t - long_rows[j][0])
        j += 1
    offs.sort()
    off = offs[len(offs) // 2] if offs else 0.0
    pt = [p[0] for p in pos]
    st = [s[0] for s in steps]
    lines = ["offset frame log -> harness clock: %.2f s (%d anchors); %d frames, %d pos lines" % (off, len(offs), len(rows), len(pos))]
    cells = defaultdict(list)
    for r in rows:
        ms = float(r["frame_ms"])
        if ms < a.ms:
            continue
        t = float(r["t_s"]) + off
        i = bisect.bisect_right(pt, t) - 1
        k = bisect.bisect_right(st, t) - 1
        step = "%d %s" % (steps[k][1], steps[k][2]) if k >= 0 else "-"
        where = "?"
        moving = False
        if i >= 0 and t - pt[i] < 1.0:
            x, y, z = pos[i][1]
            where = "(%.1f, %.1f, %.1f)" % (x, y, z)
            if i > 5:
                x0, _, z0 = pos[i - 5][1]
                moving = abs(x - x0) + abs(z - z0) > 0.5
            if moving and "host_route" in step:
                cells[(round(x / 4), round(z / 4))].append((t, ms, step, where))
        pr = [s for (tp, s) in probes if abs(tp - t) < 0.25]
        lines.append("t=%.2f %6.1f ms step %-14s at %s%s | images %s img_mb %s buf_mb %s protect %s create %s refresh %s wf %s "
                     "rf %s copy_mb %s compiles %s preup %s kernel %s alloc %s gc %s present_wait %s tick_wait %s cpuA %s"
                     % (t, ms, step, where, " walking" if moving else "", r["images"], r["image_mb"], r["buffer_mb"], r["protect_ms"],
                        r["create_ms"], r["refresh_ms"], r["write_fault_ms"], r["read_faults"], r["guest_copy_mb"],
                        r["compiles"], r["preupload_mb"], r["kernel_ms"], r["alloc_mb"], r["gc_ms"], r["present_wait_ms"],
                        r["tick_wait_ms"], r["stage_a_cpu_ms"]))
        for s in pr:
            lines.append("      " + s)
    lines.append("")
    lines.append("walking hitches by place (4 m cells; passes that hit it):")
    for c, v in sorted(cells.items(), key=lambda kv: kv[1][0][0]):
        lines.append("  cell (%d, %d): %s" % (c[0] * 4, c[1] * 4, "; ".join("%.0f ms @%.0f s %s [%s]" % (ms, t, w, s) for t, ms, s, w in v)))
    # Per leg: the walk (host_route start + 1 s .. its end line) and the stand before it (the last 10 s at the
    # lamp, after the load), from the frame log.
    legs, cur = [], None
    for line in log:
        m = TS.match(line)
        if not m:
            continue
        t, s = float(m.group(1)), m.group(2)
        if "test: host_route:" in s:
            cur = t
        elif cur is not None and ("test: route: DONE" in s or "test: route: STOPPED" in s):
            legs.append((cur, t))
            cur = None
    fr = [(float(r["t_s"]) + off, float(r["frame_ms"]), int(r["images"]), int(r["gc_evicted"]), int(r["gc_level"] or 0)) for r in rows]
    lines.append("")
    lines.append("per leg (walk = route start + 1 s .. end; stand = the 10 s before the walk):")
    tot = defaultdict(float)
    for i, (a0, b0) in enumerate(legs):
        for name, lo, hi in (("stand", a0 - 10.0, a0), ("walk", a0 + 1.0, b0)):
            v = [f for f in fr if lo <= f[0] <= hi]
            if not v:
                continue
            ms = sorted(f[1] for f in v)
            over = lambda x: sum(1 for m_ in ms if m_ > x)  # noqa: E731
            lines.append("  leg %d %-5s %5.0f s: %5d frames, over 25 ms %3d, over 45 ms %3d, over 60 ms %3d, worst %6.1f ms; "
                         "%5d images registered, %5d evicted, collector at its Over rule in %3.0f %% of frames"
                         % (i + 1, name, hi - lo, len(v), over(25), over(45), over(60), ms[-1],
                            sum(f[2] for f in v), sum(f[3] for f in v), 100.0 * sum(1 for f in v if f[4] >= 4) / len(v)))
            for k, x in (("frames", len(v)), ("o25", over(25)), ("o45", over(45)), ("o60", over(60)), ("reg", sum(f[2] for f in v)),
                         ("ev", sum(f[3] for f in v)), ("s", hi - lo)):
                tot[name + k] += x
            tot[name + "worst"] = max(tot[name + "worst"], ms[-1])
    for name in ("stand", "walk"):
        if tot[name + "frames"]:
            lines.append("  ALL %-5s %5.0f s: %5d frames, over 25 ms %3d, over 45 ms %3d, over 60 ms %3d, worst %6.1f ms; %5d images registered, %5d evicted"
                         % (name, tot[name + "s"], tot[name + "frames"], tot[name + "o25"], tot[name + "o45"], tot[name + "o60"],
                            tot[name + "worst"], tot[name + "reg"], tot[name + "ev"]))
    (out / "hitches.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines[-30:]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["run", "report", "chain"])
    ap.add_argument("--out", default=str(AGENT_ROOT / "hitch" / "run"))
    ap.add_argument("--root", default=str(AGENT_ROOT / "hitchroot"))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--fps", default="60")
    ap.add_argument("--passes", type=int, default=2)
    ap.add_argument("--wait", type=int, default=12)
    ap.add_argument("--legs", default="clinic,tomb,ward")
    ap.add_argument("--probe-ms", type=int, default=45)
    ap.add_argument("--vram", type=int, default=6144,
                    help="BB_VRAM_LIMIT_MB (the harness default 1792 keeps the texture collector under pressure, which players' cards are not)")
    ap.add_argument("--ms", type=float, default=45.0)
    ap.add_argument("--env", action="append")
    a = ap.parse_args()
    if a.cmd == "chain":
        print(chain(a.legs.split(","), a.passes, a.wait)[0])
    elif a.cmd == "run":
        run(a)
    else:
        report(a)


if __name__ == "__main__":
    main()
