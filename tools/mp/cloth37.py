#!/usr/bin/env python3
"""GitHub #37: the Noble Dress (cloth skirt) clips through the player above 30 FPS. A repeatable A/B.

  python tools/mp/cloth37.py --root E:\\...\\clothrun --game E:\\...\\game [--rates 30,60,90,120,480]
         [--variants "stock;cur;jg;nw"] [--trials 2] [--armor "Noble Dress"] [--seconds 3000]

One game session (the frame-rate sweep, gpu/shim/bbport_frame_sweep.cpp BB_FRAME_SWEEP_CLOTH): the hunter wears the
armour (the director's `armor=SET`; the id is looked up BY NAME in the game's own files at run time, nothing from the
game is stored here), and at every rate and every cloth fix variant (BB_CLOTH_FIX spec, bbport_frame_rate_logic.h) a
scripted move runs: idle, two forward rolls, a stop, a backstep, a run and a stop. The game logs, per phase, the
cloth particles' horizontal distance from the player's axis (r) and their height (y) from its memory, and takes
screenshots at fixed moments. This tool then prints the table against the 30 FPS reference and converts the shots
to PNG (named by rate, variant, trial and phase) under <root>/shots.

  python tools/mp/cloth37.py --report <run.log> [--out DIR]     only the report of a finished run
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

LINE = re.compile(
    r"Frame sweep: cloth (?P<rate>\S+) (?P<variant>\S+) t(?P<trial>\d+) (?P<phase>\S+) inst (?P<inst>[0-9a-f]+) "
    r"n (?P<n>\d+) samples (?P<samples>\d+) r (?P<rmean>[-\d.]+)/(?P<rmin>[-\d.]+)/(?P<rmax>[-\d.]+) "
    r"\(lowest min (?P<rmin_lo>[-\d.]+), highest max (?P<rmax_hi>[-\d.]+)\) "
    r"y (?P<ymin>[-\d.]+)/(?P<ymax>[-\d.]+) \(lowest (?P<ymin_lo>[-\d.]+)\)")
SHOT = re.compile(r"Frame sweep: cloth shot (?P<index>\d+) (?P<rate>\S+) (?P<variant>\S+) t(?P<trial>\d+) (?P<phase>\S+)")
FIELDS = ("rmean", "rmin", "rmax", "rmin_lo", "rmax_hi", "ymin", "ymax", "ymin_lo")


def parse(lines):
    """([phase records], [shot records]) from a game log's lines."""
    recs, shots = [], []
    for line in lines:
        m = LINE.search(line)
        if m:
            d = m.groupdict()
            r = {k: d[k] for k in ("rate", "variant", "phase", "inst")}
            r["trial"] = int(d["trial"])
            r["n"] = int(d["n"])
            r["samples"] = int(d["samples"])
            for f in FIELDS:
                r[f] = float(d[f])
            recs.append(r)
            continue
        m = SHOT.search(line)
        if m:
            shots.append(dict(m.groupdict(), index=int(m["index"]), trial=int(m["trial"])))
    return recs, shots


def rate_key(rate):
    return 10000 if rate == "uncap" else int(rate) if rate.isdigit() else 0


def big_instances(recs, min_n=100):
    """The cloth instances with at least min_n particles (skirts and capes; the 24-particle ones are hair and tassels),
    by particle count, biggest first."""
    n = {}
    for r in recs:
        n[r["inst"]] = r["n"]
    return [i for i, c in sorted(n.items(), key=lambda kv: -kv[1]) if c >= min_n]


def skirt_instance(recs):
    """The Noble Dress skirt among the cloth instances: the biggest one (by particles) with particles below the player's
    reference height (a hem), None without one."""
    best = None
    for i in big_instances(recs):
        rs = [r for r in recs if r["inst"] == i]
        if min(r["ymin_lo"] for r in rs) < 0.0:
            best = best or i
    return best


def table(recs, inst, phases=("idle", "roll2", "stop_a", "stop_b", "backstep", "run", "run_stop_b")):
    """{(variant, rate): {phase: {field: mean over the trials}}} of one instance."""
    acc = {}
    for r in recs:
        if r["inst"] != inst or r["phase"] not in phases:
            continue
        cell = acc.setdefault((r["variant"], r["rate"]), {}).setdefault(r["phase"], {f: [] for f in FIELDS})
        for f in FIELDS:
            cell[f].append(r[f])
    return {k: {p: {f: sum(v) / len(v) for f, v in cell.items()} for p, cell in ph.items()} for k, ph in acc.items()}


def distance(cur, ref, fields=("rmean", "rmin", "rmax", "ymin", "ymax")):
    """How far a cell's phases are from the reference's (mean absolute difference over the shared phases and fields)."""
    tot, n = 0.0, 0
    for p, vals in cur.items():
        if p not in ref:
            continue
        for f in fields:
            tot += abs(vals[f] - ref[p][f])
            n += 1
    return tot / n if n else float("nan")


def report(recs, ref_rate="30", insts=None):
    out = []
    for inst in insts or big_instances(recs):
        tb = table(recs, inst)
        variants = list(dict.fromkeys(r["variant"] for r in recs))
        rates = sorted({r for _v, r in tb}, key=rate_key)
        n = next(r["n"] for r in recs if r["inst"] == inst)
        out.append("cloth instance %s (%d particles): mean |diff| (m) of r mean/min/max and y min/max against the same "
                   "variant at %s FPS | against stock at %s FPS" % (inst, n, ref_rate, ref_rate))
        out.append("%-26s" % "variant" + "".join("%8s" % r for r in rates) + " |" + "".join("%8s" % r for r in rates))
        stock_ref = tb.get(("stock", ref_rate))
        for v in variants:
            own_ref = tb.get((v, ref_rate))
            row = "%-26s" % v
            for ref in (own_ref, stock_ref):
                for r in rates:
                    cell = tb.get((v, r))
                    row += "%8.4f" % distance(cell, ref) if cell and ref else "%8s" % "-"
                row += " |"
            out.append(row.rstrip("|"))
    return chr(10).join(out)


def armor_set(game, name):
    """The director's armor=SET value for a body armour piece by name (EquipParamProtector id - 1000), from the game's
    own files at run time (scripts/saveedit/names.py); SystemExit when the game or the name is not found."""
    sys.path.insert(0, str(REPO / "scripts"))
    from saveedit.names import Catalogue  # noqa: E402
    import tempfile
    cat = Catalogue.load(game, tempfile.gettempdir())
    if not cat.ok:
        sys.exit("cannot read the game's item names from %s" % game)
    rows = [n for n, nm in cat.search(1, name) if nm and nm.lower() == name.lower()]
    if not rows:
        sys.exit("no armour named %r in the game files" % name)
    return rows[0] - 1000


def convert_shots(root, shots, out):
    """The game's raw frame dumps (inst0/shotdump/f<N>_shot_<w>x<h>_bgra.raw) as PNGs named by rate, variant, trial, phase."""
    try:
        from PIL import Image
    except ImportError:
        print("[cloth37] no PIL: the shots stay raw under", root / "inst0" / "shotdump")
        return 0
    out.mkdir(parents=True, exist_ok=True)
    dumps = {}
    for f in (root / "inst0" / "shotdump").glob("f*_shot_*_*.raw"):
        m = re.match(r"f(\d+)_shot_(\d+)x(\d+)_(bgra|rgba)\.raw$", f.name)
        if m:
            dumps[int(m[1])] = (f, int(m[2]), int(m[3]), m[4])
    order = sorted(dumps)
    n = 0
    for s in shots:
        if s["index"] >= len(order):
            continue
        f, w, h, fmt = dumps[order[s["index"]]]
        data = f.read_bytes()
        if len(data) < w * h * 4:
            continue
        img = Image.frombuffer("RGBA", (w, h), data[:w * h * 4], "raw", "BGRA" if fmt == "bgra" else "RGBA", 0, 1).convert("RGB")
        img.save(out / ("%s_%s_t%d_%s.png" % (s["rate"], s["variant"].replace(",", "+"), s["trial"], s["phase"])))
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root")
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", ""))
    ap.add_argument("--saves", default=r"E:\games\bbport\test_saves\savedata")
    ap.add_argument("--rates", default="30,60,90,120,480")
    ap.add_argument("--variants", default="stock;cur;jg;nw")
    ap.add_argument("--trials", type=int, default=2)
    ap.add_argument("--armor", default="Noble Dress")
    ap.add_argument("--seconds", type=int, default=3000)
    ap.add_argument("--start-fps", default="uncap")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--report")
    ap.add_argument("--out")
    a = ap.parse_args()
    if a.report:
        recs, shots = parse(Path(a.report).read_text(encoding="utf-8", errors="replace").splitlines())
        print(report(recs))
        if a.out:
            print("shots converted:", convert_shots(Path(a.report).parent.parent, shots, Path(a.out)))
        return 0
    if not a.root or not a.game:
        ap.error("--root and --game (or BB_GAME_DIR) are required")
    root = Path(a.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    pad = root / "pad0.txt"
    pad.write_text("")
    env_list = ["BB_PAD_FILE=" + str(pad), "BB_PARTY_DIRECTOR_TEST_SOLO=1",
                "BB_PARTY_DIRECTOR_TEST=armor=%d,god" % armor_set(a.game, a.armor),
                "BB_FRAME_SWEEP=" + a.rates, "BB_FRAME_SWEEP_CLOTH=" + a.variants,
                "BB_FRAME_SWEEP_CLOTH_TRIALS=%d" % a.trials, "BB_FRAME_SWEEP_WAIT=300", "BB_CLOTH_TRACE=1",
                "BB_WINDOW_BACKGROUND=1", "BB_AUDIO=none"]
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "1", "--headless", "0", "--party",
           "--fresh-saves", "--saves", a.saves, "--timestamps", "--idle", "0", "--seconds", str(a.seconds),
           "--stuck", "300", "--fps", a.start_fps, "--until", r"Frame sweep: done", "--root", str(root),
           "--build", a.build, "--game", a.game, "--label", "cloth37"]
    for e in env_list + a.env:
        cmd += ["--env", e]
    envp = dict(os.environ, BB_MP_LEASE=str(a.seconds + 300), BB_MP_PARENT_PID=str(os.getpid()))
    (root / "noreap").write_text(str(a.seconds + 120))
    rc = subprocess.call(cmd, env=envp)
    log = root / "inst0" / "run.log"
    recs, shots = parse(log.read_text(encoding="utf-8", errors="replace").splitlines())
    print(report(recs))
    print("shots converted:", convert_shots(root, shots, root / "shots"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
