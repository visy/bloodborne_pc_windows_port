#!/usr/bin/env python3
"""Post-merge party smoke test: a fixed ~15-20 min 3-player scenario with an automatic verdict.

  python tools/mp/smoke_party.py [--root DIR] [--build DIR] [--game DIR] [--out DIR] [--phase s1,s2]
  python tools/mp/smoke_party.py --check-only --out DIR     (verdict again from the saved logs)

Mandatory after every merge to master (agents/RULES_062.txt). Runs through tools/mp/instances.py
(the shared game queue; 3 players: host and guest 1 rendered, guest 2 headless-lite; 127.0.0.1,
the instance root's own port range, muted, background windows, BB_DLSS=0), the default test save
(E:\\games\\bbport\\test_saves\\savedata, fresh copy each phase), timed screenshots of both rendered
games (tools/mp/shot.py --every) and a PASS / FAIL per step from the logs.

Phase s1 (~8 min, Father Gascoigne un-killed: the d10 fixture flags cleared on every save):
  join (3 players), Leave + Rejoin (Party page), a guest's death and its bloodstain, a party lamp
  travel to the Tomb of Oedon, a guest-started Gascoigne fight (intro on everyone), the host's death
  in the fight (limbo + spectate), the two guests' deaths (a wipe: the party's one respawn load).
Phase s2 (~8 min, the Cleric Beast un-killed: the b2_seamless 6.7 fixture flags):
  join, a party lamp travel to the Cleric Beast's lamp, the fight, the boss killed by the host
  (echoes and the reward item box on every game: its OK row), Leave + Rejoin right after the
  kill (A3 / K10b), a guest crash and its title rejoin (C6), the host's crash and recovery (the
  guests wait, then are summoned back), the host's Leave (every guest home).

Test aids used (it is a smoke, not real play): BB_PARTY_DIRECTOR_TEST steps (lamp warps, moves into
the boss trigger, scripted deaths, the boss kill, Party > Leave / Rejoin presses),
BB_PARTY_PROGRESS_TEST_CLEAR (boss fixtures), BB_PARTY_TEST_INVINCIBLE (players moved into a fight
live until their scripted death), instances.py --crash-plan (process kills).

Output: OUT/<phase>/shots/*.png, OUT/<phase>/inst{0,1,2}.log (this run's part of each run.log),
OUT/verdict.txt (one line per step: PASS / FAIL, the evidence line or what was missing, timings).
Exit code 0 only when every step passed. LOOK AT THE SCREENSHOTS too (agents/RULES_062.txt): the
verdict reads logs, it cannot see a grey screen, a camera in a wall or a stuck dialog.
"""
import argparse, os, re, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent  # agents/NAME when run from an agent worktree
DEFAULT_ROOT = (AGENT_ROOT / "mptest") if AGENT_ROOT.parent.name == "agents" else (REPO.parent / "mptest")

# Fixture flags (docs/party/b2_seamless.md 6.7, ERSC_PARITY.md D10).
GASCOIGNE_CLEAR = "12411800,12411802,12411803,12414800,12414801,12414802,12414223,2412,9457,2410"
CLERIC_CLEAR = "12411700,12411701,12411702,12411703,12414700,12414701,12414223,72410511,2411,9456,2410"
GASCOIGNE_TRIGGER = "-48.76:-40.92:72.44"  # region 2412815 (the intro's trigger box)
CLERIC_TRIGGER_HOST = "-136.5:-26.6:52.8"   # region 2412805 (first fight)
CLERIC_SPOT_GUEST = "-134.6:-26.6:54.7"


def phase_s1():
    host = ",".join([
        "host_warp=2412953@150",            # party lamp travel: the Tomb of Oedon (after the leave/rejoin and death)
        "host_die@105%m24_01",              # in the Gascoigne fight (the guest-started intro is ~47 s)
    ])
    g1 = ",".join([
        "guest_leave@25", "guest_rejoin@10",  # Party > Leave, Rejoin
        "guest_die@25",                       # a death outside a fight: bloodstain, respawn at the host's lamp
        "guest_move=%s@45%%m24_01" % GASCOIGNE_TRIGGER,  # the guest starts the Gascoigne fight
        "guest_die@75%m24_01",                # after the host's death: part of the wipe
    ])
    g2 = "guest_die@185%m24_01"               # the headless guest: last one down (the wipe)
    return dict(
        name="s1", seconds=480,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2],
        crash=[],
    )


def phase_s2():
    host = ",".join([
        "host_warp=2412952@30",                                   # party lamp travel: the Cleric Beast's lamp
        "host_move=%s@25%%m24_01" % CLERIC_TRIGGER_HOST,          # starts the fight
        "host_kill=2410800@12%m24_01",                            # the boss kill (its defeat path)
    ])
    g1 = ",".join([
        "guest_move=%s@50%%m24_01" % CLERIC_SPOT_GUEST,
        "guest_leave@30%m24_01",            # Leave right after the kill (no load between), then Rejoin
        "guest_rejoin@8",
    ])
    return dict(
        name="s2", seconds=540,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              # crash-restarted games: no second run of the chain; the restarted host Leaves last
              "0:BB_PARTY_DIRECTOR_TEST_RESTART=host_leave@60", "1:BB_PARTY_DIRECTOR_TEST_RESTART=none",
              "2:BB_PARTY_DIRECTOR_TEST_RESTART=none"],
        # guest 1 crashes 20 s after its rejoin press; the host crashes 40 s after guest 1 is back.
        crash=[r"1:guest_rejoin: Party > Rejoin pressed@25", r"1/0:cooperators 1 -> 2@40"],
    )


PHASES = {"s1": phase_s1, "s2": phase_s2}


def run_phase(a, ph, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    root = Path(a.root)
    (root).mkdir(parents=True, exist_ok=True)
    (root / "noreap").write_text(str(ph["seconds"] + 120))
    starts = {}
    for i in range(3):
        p = root / f"inst{i}" / "run.log"
        starts[i] = p.stat().st_size if p.exists() else 0
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "3", "--headless", "1", "--party",
           "--fresh-saves", "--timestamps", "--idle", "0", "--seconds", str(ph["seconds"]),
           "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--label", "smoke %s" % ph["name"]]
    for e in ph["env"]:
        cmd += ["--env", e]
    for e in ph["ienv"]:
        cmd += ["--ienv", e]
    for c in ph["crash"]:
        cmd += ["--crash-plan", c]
    env = dict(os.environ, BB_MP_LEASE=os.environ.get("BB_MP_LEASE", "1200"))
    shots = out / "shots"
    shots.mkdir(exist_ok=True)
    print("[smoke] %s: %s" % (ph["name"], " ".join(cmd)), flush=True)
    shooter = subprocess.Popen([sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                                "--tag", ph["name"], "--every", str(a.every), "--seconds", str(ph["seconds"] + 400)],
                               stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        shooter.terminate()
    print("[smoke] %s: harness exit %d after %.0f s" % (ph["name"], r.returncode, time.time() - t0), flush=True)
    for i in range(3):
        p = root / f"inst{i}" / "run.log"
        with open(p, "rb") as f:
            f.seek(starts[i])
            (out / f"inst{i}.log").write_bytes(f.read())
    try:
        (root / "noreap").unlink()
    except OSError:
        pass


# ---------------------------------------------------------------------------------------------
# Verdict

TS = re.compile(r"^\[\+ *([\d.]+)\] ")


class Logs:
    def __init__(self, d: Path):
        self.lines = {}
        for i in range(3):
            p = d / f"inst{i}.log"
            self.lines[i] = p.read_text(encoding="utf-8", errors="replace").splitlines() if p.exists() else []

    def find(self, i, rx, after=0.0, before=1e9):
        """First (t, line) on instance i matching rx with after <= t < before."""
        r = re.compile(rx)
        for line in self.lines[i]:
            m = TS.match(line)
            t = float(m.group(1)) if m else -1.0
            if t < after or t >= before:
                continue
            if r.search(line):
                return t, line[m.end():] if m else line
        return None

    def all(self, i, rx, after=0.0, before=1e9):
        r = re.compile(rx)
        out = []
        for line in self.lines[i]:
            m = TS.match(line)
            t = float(m.group(1)) if m else -1.0
            if after <= t < before and r.search(line):
                out.append((t, line[m.end():] if m else line))
        return out


class Verdict:
    def __init__(self):
        self.rows = []

    def step(self, name, ok, detail):
        self.rows.append((name, bool(ok), detail))
        print("%-4s %-40s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def need(v, L, name, i, rx, after=0.0, before=1e9, what=""):
    hit = L.find(i, rx, after, before)
    v.step(name, hit, ("+%.1f s inst%d: %s" % (hit[0], i, hit[1][:150])) if hit else
           "missing on inst%d after +%.0f s: /%s/ %s" % (i, after, rx, what))
    return hit[0] if hit else None


def none_of(v, L, name, rx, after=0.0, before=1e9, insts=(0, 1, 2)):
    bad = [(i, h) for i in insts for h in L.all(i, rx, after, before)]
    v.step(name, not bad, "none" if not bad else "inst%d +%.1f s: %s (%d in all)" % (bad[0][0], bad[0][1][0],
                                                                                    bad[0][1][1][:150], len(bad)))


def check_common(v, L, tag):
    none_of(v, L, tag + ": no crash / assert", r"(?i)unhandled exception|assertion failed|EXCEPTION_ACCESS|fatal error")
    none_of(v, L, tag + ": no 'Session lost'", r"Session lost")


def check_s1(v, L):
    j = need(v, L, "s1 join: 3 players (host cooperators -> 2)", 0, r"cooperators [01] -> 2")
    lv = need(v, L, "s1 Leave (guest 1, Party page)", 1, r"guest_leave: Party > Leave pressed")
    if lv is not None:
        need(v, L, "s1 Leave: host sees it (2 -> 1)", 0, r"cooperators 2 -> 1", lv)
        rj = need(v, L, "s1 Rejoin pressed", 1, r"guest_rejoin: Party > Rejoin pressed", lv)
        if rj is not None:
            back = need(v, L, "s1 Rejoin: back in (1 -> 2)", 0, r"cooperators 1 -> 2", rj)
            if back is not None:
                v.step("s1 Rejoin time (press -> host room)", back - rj < 30, "%.1f s (limit 30)" % (back - rj))
    d = need(v, L, "s1 guest death (scripted)", 1, r"travel step \d+/\d+: guest_die", lv or 0)
    if d is not None:
        need(v, L, "s1 bloodstain recorded", 1, r"bloodstain", d)
    tr = need(v, L, "s1 party lamp travel (host)", 0, r"travel step \d+/\d+: host_warp to lamp 2412953")
    if tr is not None:
        for i in (1, 2):
            need(v, L, "s1 travel: guest %d in m24_01" % i, i, r"map .*-> m24_01", tr)
    gm = need(v, L, "s1 guest-started boss: guest in the trigger", 1, r"travel step \d+/\d+: guest_move", tr or 0)
    if gm is not None:
        need(v, L, "s1 boss intro / fight on the host", 0, r"(?i)boss (health )?bar|cutscene|remo", gm)
    hd = need(v, L, "s1 host death in the fight", 0, r"travel step \d+/\d+: host_die", gm or 0)
    if hd is not None:
        need(v, L, "s1 limbo / spectate (host)", 0, r"(?i)spectat|limbo", hd)
    need(v, L, "s1 wipe: the party respawns together", 0, r"(?i)wipe|every member is down", hd or 0)
    check_common(v, L, "s1")


def check_s2(v, L):
    need(v, L, "s2 join: 3 players", 0, r"cooperators [01] -> 2")
    tr = need(v, L, "s2 party lamp travel to the Cleric Beast", 0, r"travel step \d+/\d+: host_warp to lamp 2412952")
    k = need(v, L, "s2 boss kill (host)", 0, r"travel step \d+/\d+: host_kill", tr or 0)
    if k is not None:
        for i in (0, 1, 2):
            need(v, L, "s2 boss kill counted on inst%d" % i, i, r"(?i)boss kill|K10|defeat", k)
        need(v, L, "s2 item box OK row (guest 1)", 1, r"(?i)item box", k)
    lv = need(v, L, "s2 Leave after the boss kill", 1, r"guest_leave: Party > Leave pressed", k or 0)
    if lv is not None:
        rj = need(v, L, "s2 Rejoin pressed", 1, r"guest_rejoin: Party > Rejoin pressed", lv)
        if rj is not None:
            back = need(v, L, "s2 Rejoin after the kill: back in", 0, r"cooperators 1 -> 2", rj)
            none_of(v, L, "s2 no kick after the rejoin", r"KickoutRoomMember|kicked", rj, (back or rj) + 30, (0,))
    c1 = need(v, L, "s2 guest crash (harness)", 1, r"Party: .*crash|restart", (lv or 0) + 10)
    check_common(v, L, "s2")


CHECKS = {"s1": check_s1, "s2": check_s2}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out", default=str(AGENT_ROOT / "smoke" / time.strftime("%Y%m%d_%H%M")))
    ap.add_argument("--phase", default="s1,s2")
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--check-only", action="store_true")
    a = ap.parse_args()
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game (agents/RULES_062.txt)")
    out = Path(a.out)
    v = Verdict()
    for name in [p.strip() for p in a.phase.split(",") if p.strip()]:
        ph = PHASES[name]()
        if not a.check_only:
            run_phase(a, ph, out / name)
        print("[smoke] verdict %s" % name, flush=True)
        CHECKS[name](v, Logs(out / name))
    fails = [r for r in v.rows if not r[1]]
    text = "\n".join("%s  %s  %s" % ("PASS" if ok else "FAIL", n, d) for n, ok, d in v.rows)
    text += "\n\n%d steps, %d failed\n" % (len(v.rows), len(fails))
    out.mkdir(parents=True, exist_ok=True)
    (out / "verdict.txt").write_text(text, encoding="utf-8")
    print("[smoke] %d steps, %d failed; %s" % (len(v.rows), len(fails), out / "verdict.txt"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
