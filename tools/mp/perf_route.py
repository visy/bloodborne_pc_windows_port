#!/usr/bin/env python3
"""Performance / stability baseline route (0.8.1 P0-1): one FIXED, scripted, unattended route, solo
(1 game) or 2-player party (2 games, loopback), the same route in both, for 60 minutes. Later
fixes are measured against the numbers of research/perf081/ (same route, same settings).

  python tools/mp/perf_route.py run  --variant solo|2p [--minutes 60] [--fps uncap] [--build DIR] [--out DIR]
  python tools/mp/perf_route.py report --variant solo|2p --out DIR

--build DIR is any folder with out[+s*(d+(?:.d+)?)]bbport.exe (this clone, or a release tree such as E:\\games\\bbport\\rel080).
Settings (recorded in OUT/settings.txt): frame rate cap --fps (default uncap; the harness presets 30, 60, 90,
uncap), Live FPS off unless --live-fps (BB_LIVE_FPS=1, needs a cap above 30; GitHub #35), BB_PARTY_PERF=1 every 60 s, BB_FRAME_STATS=1 every 5 s,
BB_AUDIO=none, BB_WINDOW_BACKGROUND=1 (harness), both game slots held (--reserve 2: no other agent's game shares the GPU), default test save (fresh copy), no invincibility.

The run ends when the route's last step is done (+20 s), ~61 min for --minutes 60 (the harness cap is minutes + 5).
The route (one cycle ~8 min, repeated; all by the in-process director, pad file for the walks, no human input):
  lamp warp to the Great Bridge (m24_01), walk bridge arena <-> lamp twice (stick), die (host_die) and respawn
  by the game's own death load, lamp warps Cathedral Ward, 2302950, 2102950, Yahar'gul, Tomb of Oedon, then
  4 min idle in the Tomb of Oedon, then the next cycle starts with the lamp warp back to the bridge.
In the 2-player variant the host runs the route and the guest follows by the party travel (and shares the death
load); both are rendered.

Output (OUT): soak.csv (tools/mp/soak_sample.py: private, working set, handles, threads, GPU memory, CPU),
inst{0,1}.log, harness.log, shots/ (every 120 s), settings.txt; `report` adds fps.csv (every "Frame stats"
line), loads.csv (every load line) and summary.txt.
"""
import argparse, csv, os, re, statistics, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent
sys.path.insert(0, str(HERE))
from soak_party import LaterProc  # noqa: E402

LAMP_BRIDGE, LAMP_CATH, LAMP_A, LAMP_B, LAMP_YAH, LAMP_TOMB = 2412952, 2402950, 2302950, 2102950, 2802950, 2412953
# Great Bridge lamp spot and the Cleric Beast's arena in m24_01 (x, z), as tools/mp/soak_party.py.
LAMP = (-124.8, 64.3)
ARENA = (-136.5, 52.8)
CYCLE_S = 480


def cycle(variant, lead=20, first=False):
    """One cycle of host director steps ("name=value@seconds_after_previous_step[%map]"). Every lamp warp is
    host_lamp (the lamp as the player uses it, on the steady-world clock; a host_warp step needs a re-join to
    re-arm, which a solo run and a party's in-place death revive never give). In the 2-player variant the host's
    "travel with the party?" question is answered YES (BB_PARTY_SPLIT_TEST) and the first step waits for the
    guest (^1)."""
    def warp(lamp, dwell):
        return "host_lamp=%d@%d%s" % (lamp, dwell, "^1" if first_warp[0] and variant == "2p" else "")
    first_warp = [first]
    s = [warp(LAMP_BRIDGE, lead)]   # lead: the dwell in the previous area (the first cycle: the Dream; later: the idle in the Tomb)
    first_warp[0] = False
    for _ in range(2):
        s += ["host_stick=%.1f:%.1f:2:80@8%%m24_01" % ARENA, "host_stick=%.1f:%.1f:2:80@4%%m24_01" % LAMP]
    s += ["host_die@10%m24_01"]
    s += [warp(l, d) for l, d in ((LAMP_CATH, 30), (LAMP_A, 20), (LAMP_B, 20), (LAMP_YAH, 20), (LAMP_TOMB, 20))]
    return s


def heavy_chain(cycles, variant, dwell=25):
    """Heavy-release route (memdisc2): back-to-back big loads, no idle: lamp warps across the big maps and a death
    load per cycle (7 loads per cycle), a short dwell in each. For memory A/B (--env BB_MEM_DISCARD=0 vs default)."""
    steps = []
    for i in range(cycles):
        first = i == 0
        for k, lamp in enumerate((LAMP_BRIDGE, LAMP_CATH, LAMP_A, LAMP_B, LAMP_YAH, LAMP_TOMB)):
            steps.append("host_lamp=%d@%d%s" % (lamp, 20 if (first and k == 0) else dwell, "^1" if first and k == 0 and variant == "2p" else ""))
        steps.append("host_die@%d" % dwell)   # dies in the Tomb of Oedon: the death load
    return ",".join(steps)


def chain(minutes, variant, idle=255, route="baseline"):
    if route == "heavy":
        return heavy_chain(max(1, int(minutes)), variant)   # --minutes N = N cycles x 7 loads in heavy mode
    steps = []
    n = max(1, int(round(minutes * 60 / CYCLE_S)))
    for i in range(n):
        steps += cycle(variant, lead=20 if i == 0 else idle, first=(i == 0))
    return ",".join(steps)


def settings_text(a, secs):
    return "\n".join([
        "variant=%s minutes=%s seconds=%d" % (a.variant, a.minutes, secs),
        "build=%s" % a.build, "fps=%s live_fps=%s extra_env=%s" % (a.fps, a.live_fps, a.env or []),
        "env: BB_PARTY_PERF=1 BB_PARTY_PERF_SECONDS=60 BB_FRAME_STATS=1 BB_AUDIO=none BB_WINDOW_BACKGROUND=1 (harness)",
        "chain=" + chain(a.minutes, a.variant, route=a.route), ""])


def run(a):
    root = Path(a.root)
    root.mkdir(parents=True, exist_ok=True)
    out = Path(a.out)
    (out / "shots").mkdir(parents=True, exist_ok=True)
    secs = int(a.minutes * 60) if a.route != "heavy" else int(a.minutes * 7 * 60 + 120)   # heavy: --minutes = cycles
    count = 1 if a.variant == "solo" else 2
    (root / "noreap").write_text(str(secs + 600))
    (out / "settings.txt").write_text(settings_text(a, secs))
    starts = {}
    for i in range(count):
        p = root / f"inst{i}" / "run.log"
        starts[i] = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", str(count), "--fresh-saves", "--timestamps",
           "--idle", "0", "--seconds", str(secs + 300), "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--fps", a.fps, "--reserve", "2", "--label", "perf route %s %d min" % (a.variant, a.minutes),
           "--env", "BB_PARTY_PERF=1", "--env", "BB_PARTY_PERF_SECONDS=60", "--env", "BB_FRAME_STATS=1",
           "--env", "BB_PAD_FILE=%s" % (root / "pad{i}.txt"),
           "--ienv", "0:BB_PARTY_DIRECTOR_TEST=" + chain(a.minutes, a.variant, route=a.route)]
    cmd += ["--party"]   # as the smoke's one-player phases: instance 0 Continues and runs the director (a lone host)
    if a.variant == "solo":
        cmd += ["--env", "BB_PARTY_DIRECTOR_TEST_SOLO=1"]
    else:
        cmd += ["--ienv", "0:BB_PARTY_SPLIT_TEST=yes"]
    if a.live_fps:
        cmd += ["--env", "BB_LIVE_FPS=1"]
    for e in a.env or []:
        cmd += ["--env", e]
    for i in range(count):
        (root / f"pad{i}.txt").write_text("lx=128 ly=128\n")
    env = dict(os.environ, BB_MP_LEASE=str(secs + 900))
    (out / "harness.log").unlink(missing_ok=True)
    sampler = LaterProc(out / "harness.log", [sys.executable, str(HERE / "soak_sample.py"), "--root", str(root), "--out",
                        str(out / "soak.csv"), "--every", str(a.sample_every), "--seconds", str(secs + 600)] + (["--no-gpu"] if a.route == "heavy" else []),
                        stdout=open(out / "sampler.log", "w"), stderr=subprocess.STDOUT)
    shooter = LaterProc(out / "harness.log", [sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out",
                        str(out / "shots"), "--tag", "perf", "--every", "120", "--grey", "--seconds", str(secs + 600)],
                        stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    print("[perf] " + " ".join(cmd)[:300] + " ...", flush=True)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        sampler.terminate()
        shooter.terminate()
    print("[perf] harness exit %d after %.0f s" % (r.returncode, time.time() - t0), flush=True)
    for i in range(count):
        p = root / f"inst{i}" / "run.log"
        if p.exists():
            with open(p, "rb") as f:
                f.seek(starts[i][1] if os.fstat(f.fileno()).st_ino == starts[i][0] else 0)
                (out / f"inst{i}.log").write_bytes(f.read())
    try:
        (root / "noreap").unlink()
    except OSError:
        pass


# ---- report ----
TS = re.compile(r"^\[\+\s*(\d+(?:\.\d+)?)\]\s?(.*)$")
FS = re.compile(r"Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms")


def parse_log(path):
    fps, events = [], []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = TS.match(line)
        if not m:
            continue
        t, rest = float(m.group(1)), m.group(2)
        f = FS.search(rest)
        if f:
            fps.append((t, float(f.group(1)), float(f.group(2))))
        else:
            events.append((t, rest))
    return fps, events


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, int(round(p / 100 * (len(v) - 1)))))] if v else float("nan")


def window(rows, lo, hi):
    return [r for r in rows if lo * 60 <= r[0] < hi * 60]


def heavy_memory(out, lt, rows, settle=12):
    """Per load (inst 0): the sample nearest `settle` s after "loading screen gone": working set, private, commit,
    page faults; then growth first -> last load and the max. loads.csv times are the game log's (s since the
    harness start), soak.csv's elapsed_s is the sampler's: they start within a few seconds of each other."""
    lines = ["heavy-route memory per load (inst0, sample %d s after the load; MB):" % settle,
             "  n  load_s  ws  private  commit  faults_k"]
    rs = [r for r in rows if int(r["inst"]) == 0 and "commit_mb" in r]
    pts = []
    for n, (i, up, gone, dur) in enumerate([r for r in lt if r[0] == 0]):
        if not rs:
            break
        r = min(rs, key=lambda x: abs(float(x["elapsed_s"]) - (gone + settle)))
        pts.append((float(r["working_set_mb"]), float(r["private_mb"]), float(r["commit_mb"]), int(r["page_faults"]) / 1000))
        lines.append("  %2d %6.1f %7.0f %7.0f %7.0f %7.0f" % (n, dur, *pts[-1]))
    if len(pts) > 2:
        lines.append("  growth load 2 -> last: ws %+.0f  private %+.0f  commit %+.0f MB; max ws %.0f private %.0f commit %.0f" % (
            pts[-1][0] - pts[1][0], pts[-1][1] - pts[1][1], pts[-1][2] - pts[1][2],
            max(p[0] for p in pts), max(p[1] for p in pts), max(p[2] for p in pts)))
        lines.append("  mean ws %.0f private %.0f commit %.0f MB (loads 2..last)" % tuple(
            statistics.mean(p[k] for p in pts[1:]) for k in range(3)))
    return lines


def report(a):
    out = Path(a.out)
    count = 1 if a.variant == "solo" else 2
    lines = ["variant %s" % a.variant]
    if (out / "settings.txt").exists():
        lines += (out / "settings.txt").read_text().splitlines()[:3]
    wins = [("5 min", 3, 8), ("30 min", 28, 33), ("60 min", 55, 60)]
    fps_rows, lt = [], []
    for i in range(count):
        fps, events = parse_log(out / f"inst{i}.log")
        fps_rows += [(i, t, f, w) for t, f, w in fps]
        lines.append("inst%d: %d Frame stats lines" % (i, len(fps)))
        for name, lo, hi in wins:
            w = window(fps, lo, hi)
            v = [r[1] for r in w]
            if v:
                lines.append("  fps %-6s mean %.1f  p1 %.1f  p50 %.1f  worst-frame p99 %.1f ms  (%d windows)" % (
                    name, statistics.mean(v), pct(v, 1), pct(v, 50), pct([r[2] for r in w], 99), len(v)))
        first = [r[1] for r in window(fps, 3, 8)]
        last = [r[1] for r in window(fps, 55, 60)]
        if first and last:
            lines.append("  FPS drift 5 -> 60 min: %+.1f %%" % (100 * (statistics.mean(last) / statistics.mean(first) - 1)))
        lines.append("  crash/assert lines: %d" % len([e for e in events if re.search(
            r"(?i)assert|exception|crash|device lost|access violation|fatal", e[1]) and "recorded for crash reports" not in e[1]]))
        up = None
        for t, txt in events:   # "loading screen up" ... "loading screen gone": one load
            if "loading screen up" in txt:
                up = t
            elif "loading screen gone" in txt and up is not None:
                lt.append((i, up, t, round(t - up, 1)))
                up = None
    with open(out / "fps.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["inst", "t_s", "fps", "worst_frame_ms"])
        w.writerows(fps_rows)
    soak = out / "soak.csv"
    if soak.exists():
        rows = list(csv.DictReader(open(soak)))
        for i in range(count):
            rs = [r for r in rows if int(r["inst"]) == i]
            lines.append("memory inst%d (%d samples):" % (i, len(rs)))
            for name, target in (("5 min", 300), ("30 min", 1800), ("60 min", 3600)):
                if not rs:
                    break
                r = min(rs, key=lambda x: abs(float(x["elapsed_s"]) - target))
                lines.append("  %-6s (t=%ss) private %s MB  ws %s MB  handles %s  threads %s  gpu ded %s MB  shared %s MB" % (
                    name, r["elapsed_s"], r["private_mb"], r["working_set_mb"], r["handles"], r["threads"],
                    r["gpu_dedicated_mb"], r["gpu_shared_mb"]))
            if len(rs) > 6:
                lines.append("  handle growth sample 5 -> last: %+d" % (int(rs[-1]["handles"]) - int(rs[5]["handles"])))
    # the same scene every cycle: the idle dwell in the Tomb of Oedon (the gap after a load that lasts > 150 s to the
    # next load): its FPS per cycle is the drift metric, free of what the route shows at the 5 / 30 / 60 min marks
    for i in range(count):
        fps = [(t, f) for (ii, t, f, w) in fps_rows if ii == i]
        ld = [(r[1], r[2]) for r in lt if r[0] == i]
        idle = []
        for (u0, g0), (u1, g1) in zip(ld, ld[1:]):
            if u1 - g0 > 150:
                v = [f for t, f in fps if g0 + 20 <= t <= u1 - 5]
                if v:
                    idle.append((round(g0 / 60, 1), statistics.mean(v), pct(v, 1)))
        if idle:
            lines.append("idle-dwell FPS inst%d (start min: mean / p1): %s" % (i, "; ".join("%.0f: %.1f / %.1f" % x for x in idle)))
            if len(idle) > 1:
                lines.append("  idle-dwell drift first -> last: %+.1f %%" % (100 * (idle[-1][1] / idle[0][1] - 1)))
    with open(out / "loads.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["inst", "up_t_s", "gone_t_s", "load_s"])
        w.writerows(lt)
    for i in range(count):
        d = [r[3] for r in lt if r[0] == i]
        if d:
            lines.append("loads inst%d: %d, mean %.1f s, median %.1f s, max %.1f s (loads.csv; first one is the title -> world)" % (
                i, len(d), statistics.mean(d), statistics.median(d), max(d)))
    text = "\n".join(lines)
    (out / "summary.txt").write_text(text + "\n")
    print(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["run", "report"])
    ap.add_argument("--variant", choices=["solo", "2p"], default="solo")
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--fps", default="uncap")
    ap.add_argument("--live-fps", action="store_true")
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--root", default=str(AGENT_ROOT / "perfroot"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--env", action="append")
    ap.add_argument("--route", choices=["baseline", "heavy"], default="baseline")
    ap.add_argument("--sample-every", type=int, default=30)
    a = ap.parse_args()
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game")
    a.out = a.out or str(AGENT_ROOT / "perf" / ("%s_%s" % (a.variant, time.strftime("%Y%m%d_%H%M"))))
    if a.cmd == "run":
        run(a)
    report(a)


if __name__ == "__main__":
    main()
