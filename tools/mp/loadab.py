#!/usr/bin/env python3
"""Load-time A/B session (fastload081): ONE solo game session, a fixed scripted route of loads, the load screen
time of every load printed (loading screen up -> gone, the player's wait), and the BB_LOAD_TRACE "LOAD TOTAL" lines.

  python tools/mp/loadab.py run --out DIR [--cycles 2] [--env KEY=VALUE ...] [--build DIR]
  python tools/mp/loadab.py report --out DIR [--out DIR2 ...]

The route (instance 0, a lone host, the director as tools/mp/perf_route.py): Continue (the title -> world load),
then per cycle the lamp warps Great Bridge, Cathedral Ward, 2302950, 2102950, Yahar'gul, Tomb of Oedon and a
death respawn (host_die at the Tomb of Oedon). A kind label per load is printed by `report`.
"""
import argparse, os, re, statistics, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent
sys.path.insert(0, str(HERE))
from soak_party import LaterProc  # noqa: E402

LAMPS = (("lamp Great Bridge", 2412952), ("lamp Cathedral Ward", 2402950), ("lamp 2302950", 2302950),
         ("lamp 2102950", 2102950), ("lamp Yahar'gul", 2802950), ("lamp Tomb of Oedon", 2412953))


def chain(cycles, dwell=12, custom=None):
    if custom:   # "lampA,lampB,...": warps in that order, dwell s after each
        return ",".join("host_die@10%m24_01" if x == "die" else "host_lamp=%s@%d" % (x, 20 if i == 0 else dwell)
                        for i, x in enumerate(custom.split(",")))
    steps = []
    for c in range(cycles):
        for k, (_, lamp) in enumerate(LAMPS):
            steps.append("host_lamp=%d@%d" % (lamp, 20 if (c == 0 and k == 0) else dwell))
        steps.append("host_die@10%m24_01")
    return ",".join(steps)


def labels(cycles):
    out = ["Continue"]
    for _ in range(cycles):
        out += [n for n, _ in LAMPS] + ["death respawn"]
    return out


def run(a):
    root = Path(a.root)
    root.mkdir(parents=True, exist_ok=True)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    secs = int(a.cycles * 7 * 45 + 200)
    if a.chain:
        secs = len(a.chain.split(",")) * 40 + 200
    (root / "noreap").write_text(str(secs + 600))
    (root / "pad0.txt").write_text("lx=128 ly=128\n")
    last = len(a.chain.split(",")) if a.chain else a.cycles * 7
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "1", "--fresh-saves", "--timestamps",
           "--idle", "0", "--seconds", str(secs + 300), "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--fps", a.fps, "--reserve", "1", "--label", "loadab %s" % out.name, "--party",
           "--env", "BB_LOAD_TRACE=1", "--env", "BB_AUDIO=none", "--env", "BB_PAD_FILE=%s" % (root / "pad{i}.txt"),
           "--env", "BB_PARTY_DIRECTOR_TEST_SOLO=1",
           "--ienv", "0:BB_PARTY_DIRECTOR_TEST=" + chain(a.cycles, custom=a.chain),
           "--until", r"test: travel step %d/%d: %s" % (last, last, "host_lamp" if a.chain else "host_die"), "--until-hold", "40"]
    for e in a.env or []:
        cmd += ["--env", e]
    env = dict(os.environ, BB_MP_LEASE=str(secs + 900), BB_MP_FPS=a.fps)
    shooter = None
    if a.shots:   # frames 0.5 .. 5 s after every "loading screen gone"; shot.py names the black ones (BLACK, capture.log)
        (out / "shots").mkdir(exist_ok=True)
        shooter = LaterProc(out / "harness.log", [sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out",
                            str(out / "shots"), "--tag", "gone", "--repeat", "--seconds", str(secs + 400), "--watch",
                            "loading screen gone=gone@1,2,3,4,5,6,7,8,9,10"],
                            stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    print("[loadab] " + " ".join(cmd)[:250], flush=True)
    r = subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    if shooter:
        shooter.terminate()
    print("[loadab] harness exit %d after %.0f s" % (r.returncode, time.time() - t0), flush=True)
    p = root / "inst0" / "run.log"
    if p.exists():
        (out / "inst0.log").write_bytes(p.read_bytes())
    (out / "settings.txt").write_text("env=%s cycles=%d fps=%s\n" % (a.env, a.cycles, a.fps))
    try:
        (root / "noreap").unlink()
    except OSError:
        pass


TS = re.compile(r"^\[\+\s*(\d+(?:\.\d+)?)\]\s?(.*)$")


def loads(path):
    up, res = None, []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = TS.match(line)
        if not m:
            continue
        t, txt = float(m.group(1)), m.group(2)
        if "loading screen up" in txt:
            up = t
        elif "loading screen gone" in txt and up is not None:
            res.append(round(t - up, 2))
            up = None
    return res


def report(a):
    cyc = None
    cols = []
    for o in a.out:
        o = Path(o)
        s = (o / "settings.txt").read_text() if (o / "settings.txt").exists() else ""
        m = re.search(r"cycles=(\d+)", s)
        cyc = int(m.group(1)) if m else 2
        cols.append((o.name, loads(o / "inst0.log")))
    lab = labels(cyc)
    print("%-22s" % "load" + "".join("%-16s" % n[:15] for n, _ in cols))
    for i, l in enumerate(lab):
        print("%-22s" % l + "".join("%-16s" % (("%.2f" % v[i]) if i < len(v) else "-") for _, v in cols))
    kinds = {"Continue": lambda l: l == "Continue", "lamp warps": lambda l: l.startswith("lamp"),
             "death respawn": lambda l: l == "death respawn"}
    for k, f in kinds.items():
        row = "%-22s" % ("mean " + k)
        for _, v in cols:
            xs = [v[i] for i, l in enumerate(lab) if i < len(v) and f(l)]
            row += "%-16s" % (("%.2f (n=%d)" % (statistics.mean(xs), len(xs))) if xs else "-")
        print(row)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["run", "report"])
    ap.add_argument("--cycles", type=int, default=2)
    ap.add_argument("--fps", default=os.environ.get("BB_MP_FPS", "60"))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--root", default=str(AGENT_ROOT / "loadroot"))
    ap.add_argument("--out", action="append")
    ap.add_argument("--env", action="append")
    ap.add_argument("--chain", help="comma list of lamp ids to warp through (replaces the default route)")
    ap.add_argument("--shots", action="store_true", help="screenshots after every loading screen (black-frame count)")
    a = ap.parse_args()
    if a.cmd == "run":
        a.out = a.out[0]
        run(a)
        report(argparse.Namespace(out=[a.out]))
    else:
        report(a)


if __name__ == "__main__":
    main()
