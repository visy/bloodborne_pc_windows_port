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
import argparse, os, re, shutil, struct, subprocess, sys, time
from pathlib import Path


class LaterProc:
    """Starts a helper (shot.py, the sampler) only once the harness has started the games (after the
    shared game queue's wait), so its run timer and window watch cover the run, not the wait."""

    def __init__(self, harness_log, args, **kw):
        import threading
        self.p, self.stop = None, False

        def run():
            while not self.stop:
                try:
                    if "instance 0 started" in Path(harness_log).read_text(errors="replace"):
                        break
                except OSError:
                    pass
                time.sleep(1)
            if not self.stop:
                self.p = subprocess.Popen(args, **kw)

        threading.Thread(target=run, daemon=True).start()

    def terminate(self):
        self.stop = True
        if self.p:
            self.p.terminate()

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent  # agents/NAME when run from an agent worktree
DEFAULT_ROOT = (AGENT_ROOT / "mptest") if AGENT_ROOT.parent.name == "agents" else (REPO.parent / "mptest")

# Fixture flags (docs/party/b2_seamless.md 6.7, ERSC_PARITY.md D10).
GASCOIGNE_CLEAR = "12411800,12411802,12411803,12414800,12414801,12414802,12414223,2412,9457,2410"
CLERIC_CLEAR = "12411700,12411701,12411702,12411703,12414700,12414701,12414223,72410511,2411,9456,2410"
# + the boss reward's ledger flag (lot 50000010, Sword Hunter Badge): given again at the kill, so the
# summoned guest gets an item box (its OK row, pard3)
CLERIC_CLEAR_REWARD = CLERIC_CLEAR + ",60009036"
GASCOIGNE_TRIGGER = "-48.76:-40.92:72.44"  # region 2412815 (the intro's trigger box)
CLERIC_TRIGGER_HOST = "-136.5:-26.6:52.8"   # region 2412805 (first fight)
CLERIC_SPOT_GUEST = "-134.6:-26.6:54.7"


# Speed (smokefix 2026-10-16, "lots of standing around"): every step waits only as long as the game
# needs (delays measured from the previous step's log, about 10 s of margin each), and a phase ends
# the moment its last checked line is logged (instances.py --until), not after a fixed run length;
# `seconds` is only the cap. Same steps and checks as before.
# The scripted host death, or the boss got there first (the step then logs it found the chr dead)
HOST_DIED = r"travel step \d+/\d+: host_die|travel step \d+ \(host_die\) waits: the own chr is dead"
LEAVE_OUT = r"the host ended the party|host lost \(the host left its room\)"
# The wipe itself ("a wipe; the stock funnel runs" / "a wipe; the party respawns together"), not the
# limbo line, which also says "..., a wipe or a rest" (it ended s1 before the guests died)
WIPE_RX = r"a wipe; the"
WIPE_UNTIL = "0:" + WIPE_RX


def phase_s1(only):
    host = ",".join([
        "host_warp=2412953@70",             # party lamp travel: the Tomb of Oedon (after the leave/rejoin and death)
        "host_die@6%m24_01~",               # in the fight: 6 s after the boss intro (the guest went in first)
    ])
    g1 = ",".join([
        "echoes=5000",                        # Blood Echoes to drop (the bloodstain)
        "guest_leave@10", "guest_rejoin@5",   # Party > Leave, Rejoin
        "guest_die@12",                       # a death outside a fight: bloodstain, respawn at the host's lamp
        "guest_obj=2411810@20%m24_01",        # at Gascoigne's fog, facing it
        "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!",  # Cross: the guest goes in first
        "guest_die@20%m24_01~",               # 14 s after the host's death in the fight: the wipe
    ])
    g2 = "guest_die@26%m24_01~"                # the headless guest: down too (everyone down: a wipe)
    return dict(
        name="s1", seconds=420,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2],
        crash=[],
        until=[WIPE_UNTIL, "0:spectating: following"],
    )


def phase_s2(only):
    host_tail = not only or bool({"host_crash", "host_leave"} & only)
    host = ",".join([
        "goods=4114:0",                                           # the test save holds the badge: the kill's reward would say "storage max"
        "host_warp=2412952@15",                                   # party lamp travel: the Cleric Beast's lamp
        "host_move=%s@15%%m24_01" % CLERIC_TRIGGER_HOST,          # starts the fight
        "host_kill=2410800@12%m24_01",                            # the boss kill (its defeat path)
    ])
    g1 = ",".join([
        "guest_move=%s@25%%m24_01" % CLERIC_SPOT_GUEST,
        "guest_press=0x4000@16%m24_01", "guest_press=0x4000@4%m24_01",  # Cross: OK on the reward's item box
        "guest_leave@10%m24_01",            # Leave right after the kill (no load between), then Rejoin
        "guest_rejoin@5",
    ])
    # guest 1 crashes 15 s after its Rejoin press (after the boss kill: a title join into a cleared
    # area); the host crashes 15 s after guest 1 is back in its room.
    crash = [r"1:guest_rejoin: Party > Rejoin pressed@15"]
    if host_tail:
        crash.append(r"0:cooperators [01] -> 2@15")
        until = ["0:host_leave: Party > Leave pressed", "1:" + LEAVE_OUT, "2:" + LEAVE_OUT]
    else:
        until = ["0:cooperators [01] -> 2"]  # after the guest crash: back in the host's room
    return dict(
        name="s2", seconds=480,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR_REWARD, "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              # crash-restarted games: no second run of the chain; the restarted host Leaves last
              "0:BB_PARTY_DIRECTOR_TEST_RESTART=host_leave@25", "1:BB_PARTY_DIRECTOR_TEST_RESTART=none",
              "2:BB_PARTY_DIRECTOR_TEST_RESTART=none"],
        crash=crash,
        until=until,
    )


def _psf(entries):
    """A PSF file (param.sfo): [(key, fmt, value bytes, max length)], keys sorted, as shadPS4 writes it."""
    entries = sorted(entries)
    keys = b"".join(k.encode() + b"\0" for k, *_ in entries)
    keys += b"\0" * (-len(keys) % 4)
    key_off = 20 + 16 * len(entries)
    index, data, ko = b"", b"", 0
    for k, fmt, v, mx in entries:
        index += struct.pack("<HHIII", ko, fmt, len(v), mx, len(data))
        data += v + b"\0" * (mx - len(v))
        ko += len(k) + 1
    return b"\0PSF" + struct.pack("<IIII", 0x101, key_off, key_off + len(keys), len(entries)) + index + keys + data


def prep_savecopy(root: Path):
    r"""The host's party save as a player brings one from another build (savecopy): a copy of the
    instance's test save in shadPS4's layout (SPRJ0005\sce_sys\param.sfo inside, no SPRJ0005.sce_sys
    beside it), with the Bloodborne Save Editor's *.bak leftovers, over a party folder whose side
    files belong to another character in the same slot (party_characters.txt says so; party_rot.txt
    has its rot). The game must take the save in, drop that rot and load it."""
    user = root / "inst0" / "user"
    solo = user / "savedata"
    if not solo.is_dir():
        subprocess.run([sys.executable, str(HERE / "instances.py"), "setup", "--count", "2", "--root", str(root)], check=False)
    if not solo.is_dir():
        sys.exit("savecopy: %s has no test save (instances.py setup failed)" % solo)
    party = user / "savedata_party"
    if party.exists():
        shutil.rmtree(party)
    shutil.copytree(solo, party)
    for name in ("party_state.json", "party_story.json", "party_rot.txt", "party_prologue.txt", "party_stain.txt",
                 "party_gestures.txt", "party_characters.txt"):
        (user / name).unlink(missing_ok=True)
    slots = []
    for meta in list(party.rglob("*.sce_sys")):
        folder = meta.with_name(meta.name[:-8])
        p = (meta / "param.bin").read_bytes()
        title, sub, detail = (p[a:b].split(b"\0")[0] + b"\0" for a, b in ((0, 128), (128, 256), (256, 1280)))
        sfo = _psf([("ACCOUNT_ID", 0x0004, b"\0" * 8, 8), ("MAINTITLE", 0x0204, title, 128), ("SUBTITLE", 0x0204, sub, 128),
                    ("DETAIL", 0x0204, detail, 1024), ("SAVEDATA_DIRECTORY", 0x0204, folder.name.encode() + b"\0", 32),
                    ("SAVEDATA_LIST_PARAM", 0x0404, p[1280:1284], 4),
                    ("TITLE_ID", 0x0204, folder.parent.name.encode() + b"\0", 16)])
        (folder / "sce_sys").mkdir(exist_ok=True)
        (folder / "sce_sys" / "param.sfo").write_bytes(sfo)
        if (meta / "icon0.png").exists():
            shutil.copy2(meta / "icon0.png", folder / "sce_sys" / "icon0.png")
        shutil.rmtree(meta)
        for f in list(folder.glob("userdata00*")) + list(folder.glob("backup0000")):
            shutil.copy2(f, f.with_name(f.name + ".bak"))
        slots += [int(f.name[-4:]) for f in folder.glob("userdata000[0-9]") if f.read_bytes().strip(b"\0")]
    if not slots:
        sys.exit("savecopy: the test save has no character")
    (user / "party_characters.txt").write_text("".join("slot %d used %d play %u name %016x\n" % (
        s, 1 if s in slots else 0, 99 * 3600000 if s in slots else 0, 0x1234 if s in slots else 0) for s in range(10)))
    (user / "party_rot.txt").write_text("".join("slot %d kinds 1 0 0 0 0\n" % s for s in slots))
    print("[smoke] savecopy: inst0's party save in shadPS4's layout with *.bak files; slot(s) %s marked as another "
          "character's, with its rot" % slots, flush=True)


def phase_sc(only):
    # A save brought in from another build (savecopy): taken in, the old character's rot dropped,
    # loaded by the host's Continue, a guest joins.
    return dict(
        name="sc", seconds=300, count=2, headless=0, fresh=False, prep=prep_savecopy,
        env=[], ienv=[], crash=[],
        until=["0:cooperators 0 -> 1", "1:session .* -> client"],
    )


PHASES = {"s1": phase_s1, "s2": phase_s2, "sc": phase_sc}
# --only groups (a smaller run for repeat cycles): the phase each needs and the verdict rows it keeps
GROUPS = {"join": "s1", "leave": "s1", "death": "s1", "travel": "s1", "boss": "s1", "host_death": "s1",
          "wipe": "s1", "kill": "s2", "itembox": "s2", "rejoin_kill": "s2", "c6": "s2", "host_crash": "s2",
          "host_leave": "s2", "savecopy": "sc"}
GROUP_ROWS = {"join": r"join", "leave": r"s1 (Leave|Rejoin)", "death": r"guest death|bloodstain|revived",
              "travel": r"travel", "boss": r"Gascoigne|intro", "host_death": r"host death|limbo",
              "wipe": r"wipe", "kill": r"boss kill|party stays", "itembox": r"item box|reward on the summoned",
              "rejoin_kill": r"s2 (Leave after|Rejoin|no kick)", "c6": r"guest crash",
              "host_crash": r"host crash", "host_leave": r"host Leave", "savecopy": r"^sc "}


# --only: the log lines that end each group's checks (instances.py --until "I:RX"); a phase with
# --only ends once the chosen groups' lines are all seen, so the steps after them never run.
GROUP_UNTIL = {
    "join": ["0:cooperators [01] -> 2"],
    "leave": ["1:guest_rejoin: Party > Rejoin pressed", "0:cooperators 1 -> 2"],
    "death": ["1:guest: respawned at the host's lamp|guest: died alone: revived|revived at"],
    "travel": ["1:map - -> m24_01", "2:map - -> m24_01"],
    "boss": ["1:cutscene 24010010"],
    "host_death": ["0:spectating: following"],
    "wipe": [WIPE_UNTIL],
    "kill": ["2:host boss kill", "0:K10: boss-clear leave suppressed"],
    "itembox": ["1:item notice closed|lot 50000010 .*not given while summoned|owed key item: lot 50000010"],
    "rejoin_kill": ["1:guest_rejoin: Party > Rejoin pressed"],
}
NEEDS_CRASH = {"c6", "host_crash", "host_leave"}  # s2 groups that need the crash plan and its full chain


def narrow_phase(ph, only):
    """--only: end the phase on the chosen groups' lines; drop the s2 crash plan nobody asked for."""
    mine = [g for g in only if GROUPS[g] == ph["name"]]
    if not mine or NEEDS_CRASH & set(mine):
        return ph
    ph = dict(ph, until=[u for g in mine for u in GROUP_UNTIL.get(g, [])] or ph["until"], crash=[])
    return ph


def run_phase(a, ph, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    root = Path(a.root)
    (root).mkdir(parents=True, exist_ok=True)
    (root / "noreap").write_text(str(ph["seconds"] + 120))
    starts = {}
    for i in range(3):
        p = root / f"inst{i}" / "run.log"
        # instances.py moves the old run.log to run.prev.log, unless something holds it open (then
        # it appends): remember the file id and size, and copy from there when it is the same file.
        starts[i] = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
    count = ph.get("count", 3)
    if ph.get("prep"):
        ph["prep"](root)
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", str(count), "--headless", str(ph.get("headless", 1)),
           "--party"] + (["--fresh-saves"] if ph.get("fresh", True) else []) + ["--timestamps", "--idle", "0", "--seconds", str(ph["seconds"]),
           "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--label", "smoke %s" % ph["name"]]
    for e in ph["env"]:
        cmd += ["--env", e]
    for e in ph["ienv"]:
        cmd += ["--ienv", e]
    for c in ph["crash"]:
        cmd += ["--crash-plan", c]
    for u in ph.get("until", []):
        cmd += ["--until", u]
    env = dict(os.environ, BB_MP_LEASE=os.environ.get("BB_MP_LEASE", "1200"))
    shots = out / "shots"
    shots.mkdir(exist_ok=True)
    print("[smoke] %s: %s" % (ph["name"], " ".join(cmd)), flush=True)
    (out / "harness.log").unlink(missing_ok=True)
    shooter = LaterProc(out / "harness.log", [sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                                "--tag", ph["name"], "--every", str(a.every), "--seconds", str(ph["seconds"] + 400)],
                               stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        shooter.terminate()
    print("[smoke] %s: harness exit %d after %.0f s" % (ph["name"], r.returncode, time.time() - t0), flush=True)
    for i in range(count):
        p = root / f"inst{i}" / "run.log"
        with open(p, "rb") as f:
            f.seek(starts[i][1] if os.fstat(f.fileno()).st_ino == starts[i][0] else 0)
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
    def __init__(self, only=None):
        self.rows = []
        self.keep = [re.compile(GROUP_ROWS[g]) for g in only] if only else None

    def step(self, name, ok, detail):
        if self.keep and not any(k.search(name) for k in self.keep) and not name.endswith(("no crash / assert", "no 'Session lost'")):
            return
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


def harness_crashes(d: Path):
    p = d / "harness.log"
    text = p.read_text(errors="replace") if p.exists() else ""
    return [(float(t), int(i)) for t, i in re.findall(r"\[harness \+([\d.]+) s\] crash \d+ of \d+: instance (\d+)", text)]


def check_s1(v, L):
    need(v, L, "s1 join: 3 players (host cooperators -> 2)", 0, r"cooperators [01] -> 2")
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
        need(v, L, "s1 bloodstain recorded (5000 echoes)", 1, r"guest: bloodstain recorded", d)
        # 0.6.9: a guest's death takes the respawn load (the in-place revive left it unhittable)
    need(v, L, "s1 guest respawned at the lamp", 1, r"guest: respawned at the host's lamp|guest: died alone: revived|revived at", d)
    tr = need(v, L, "s1 party lamp travel (host)", 0, r"travel step \d+/\d+: host_warp to lamp 2412953")
    if tr is not None:
        for i in (1, 2):
            need(v, L, "s1 travel: guest %d in m24_01" % i, i, r"map .*-> m24_01", tr)
    gm = need(v, L, "s1 guest at Gascoigne's fog", 1, r"travel step \d+/\d+: guest_obj", tr or 0)
    if gm is not None:
        need(v, L, "s1 guest-started intro on the host", 0, r"(?i)cutscene 24010010|remo 24010010|24010010", gm)
        need(v, L, "s1 guest-started intro on guest 1 (live mirror)", 1, r"(?i)live mirror|24010010", gm)
    hd = need(v, L, "s1 host death in the fight", 0, HOST_DIED, gm or 0)
    if hd is not None:
        need(v, L, "s1 host limbo + spectate", 0, r"spectating: following", hd)
    need(v, L, "s1 wipe: the party respawns together", 0, WIPE_RX, hd or 0)
    check_common(v, L, "s1")


def check_s2(v, L, d: Path):
    need(v, L, "s2 join: 3 players", 0, r"cooperators [01] -> 2")
    tr = need(v, L, "s2 party lamp travel to the Cleric Beast", 0, r"travel step \d+/\d+: host_warp to lamp 2412952")
    k = need(v, L, "s2 boss kill (host)", 0, r"host boss kill", tr or 0)
    if k is not None:
        need(v, L, "s2 boss kill reached guest 1", 1, r"host boss kill", k)
        need(v, L, "s2 boss kill reached guest 2", 2, r"host boss kill", k)
        need(v, L, "s2 the party stays after the boss (K10)", 0, r"K10: boss-clear leave suppressed", k)
        # The Cleric Beast's reward (lot 50000010, Sword Hunter Badge) is a key item: a summoned guest
        # gets it at once and owed to its own world too (items.md 10.3; pard2, D4); any other reward
        # shows the box.
        ib = need(v, L, "s2 reward on the summoned guest (item box or owed key item)", 1,
                  r"item notice up on a summoned guest|lot 50000010 .*not given while summoned; waits for the own world|"
                  r"owed key item: lot 50000010", 0)
        if ib is not None and L.find(1, r"item notice up on a summoned guest", k):
            need(v, L, "s2 item box OK row closes it (Cross)", 1, r"item notice closed", ib)
    lv = need(v, L, "s2 Leave after the boss kill", 1, r"guest_leave: Party > Leave pressed", k or 0)
    rj = need(v, L, "s2 Rejoin pressed", 1, r"guest_rejoin: Party > Rejoin pressed", lv or 0) if lv else None
    if rj is not None:
        back = need(v, L, "s2 Rejoin after the kill: back in", 0, r"cooperators 1 -> 2", rj)
        none_of(v, L, "s2 no kick after the rejoin", r"KickoutRoomMember req", rj, (back or rj) + 30, (0,))
    crashes = harness_crashes(d)
    g = [t for t, i in crashes if i == 1]
    h = [t for t, i in crashes if i == 0]
    v.step("s2 guest crash (harness kill)", bool(g), "+%.1f s" % g[0] if g else "the crash plan did not fire")
    if g:
        end = h[0] if h else 1e9
        back = need(v, L, "s2 guest crash: back in the host's room", 0, r"cooperators 1 -> 2", g[0], end)
        if back is not None:
            v.step("s2 guest crash rejoin time (kill -> room)", back - g[0] < 45, "%.1f s (limit 45)" % (back - g[0]))
        none_of(v, L, "s2 guest crash: no failed title join", r"title join: FAILED", g[0], end, (1,))
        none_of(v, L, "s2 guest crash: nobody kicked", r"KickoutRoomMember req", g[0], end, (0,))
    v.step("s2 host crash (harness kill)", bool(h), "+%.1f s" % h[0] if h else "the crash plan did not fire")
    if h:
        need(v, L, "s2 host crash: guest 1 goes home / waits", 1, r"host lost|Waiting for the host", h[0])
        need(v, L, "s2 host crash: the host restarts", 0, r"crash restart: the director test chain", h[0])
        back = need(v, L, "s2 host crash: the party back together", 0, r"cooperators 0 -> [12]", h[0])
        if back is not None:
            v.step("s2 host crash recovery time (kill -> first guest back)", back - h[0] < 90, "%.1f s (limit 90)" % (back - h[0]))
        hl = need(v, L, "s2 host Leave (restarted host)", 0, r"host_leave: Party > Leave pressed", h[0])
        if hl is not None:
            for i in (1, 2):
                t = need(v, L, "s2 host Leave: guest %d out" % i, i, r"the host ended the party|host lost \(the host left its room\)", hl, hl + 15)
    check_common(v, L, "s2")


def check_sc(v, L, d: Path, root: Path):
    need(v, L, "sc save check OK (*.bak ignored)", 0, r"Saves: check OK: .*savedata_party")
    need(v, L, "sc shadPS4 layout taken in", 0, r"is in shadPS4's layout \(sce_sys/param\.sfo inside\)")
    need(v, L, "sc old character's side files dropped", 0, r"Saves: save slot \d: another character")
    w = need(v, L, "sc host Continue loads the copied character", 0, r"party character in the world")
    need(v, L, "sc guest joins the host", 0, r"cooperators 0 -> 1", w or 0)
    need(v, L, "sc guest in the host's session", 1, r"session .* -> client")
    party = root / "inst0" / "user" / "savedata_party"
    both = bool(list(party.rglob("*.sce_sys/param.bin")) and list(party.rglob("sce_sys/param.sfo")))
    v.step("sc both layouts' descriptions on disk", both, "param.bin and sce_sys/param.sfo" if both else "missing")
    rot = root / "inst0" / "user" / "party_rot.txt"
    left = rot.read_text().strip() if rot.exists() else ""
    v.step("sc the other character's rot is gone", not left, "gone" if not left else repr(left[:80]))
    none_of(v, L, "sc: no crash / assert", r"(?i)unhandled exception|assertion failed|EXCEPTION_ACCESS|fatal error", insts=(0, 1))


CHECKS = {"s1": lambda v, L, d, root: check_s1(v, L), "s2": lambda v, L, d, root: check_s2(v, L, d), "sc": check_sc}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out", default=str(AGENT_ROOT / "smoke" / time.strftime("%Y%m%d_%H%M")))
    ap.add_argument("--phase", default="s1,s2")
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--only", default="", help="comma list of step groups (a smaller run): " + ",".join(GROUPS))
    a = ap.parse_args()
    only = {g.strip() for g in a.only.split(",") if g.strip()}
    bad = only - set(GROUPS)
    if bad:
        sys.exit("unknown --only group(s): " + ",".join(sorted(bad)))
    if only:
        a.phase = ",".join(p for p in ("s1", "s2", "sc") if p in {GROUPS[g] for g in only})
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game (agents/RULES_062.txt)")
    out = Path(a.out)
    v = Verdict(only)
    t_all = time.time()
    for name in [p.strip() for p in a.phase.split(",") if p.strip()]:
        ph = PHASES[name](only)
        if only:
            ph = narrow_phase(ph, only)
        if not a.check_only:
            run_phase(a, ph, out / name)
        print("[smoke] verdict %s" % name, flush=True)
        CHECKS[name](v, Logs(out / name), out / name, Path(a.root))
    fails = [r for r in v.rows if not r[1]]
    text = "\n".join("%s  %s  %s" % ("PASS" if ok else "FAIL", n, d) for n, ok, d in v.rows)
    text += "\n\n%d steps, %d failed\n" % (len(v.rows), len(fails))
    if not a.check_only:
        text += "wall time %.0f s (queue wait included)" % (time.time() - t_all) + chr(10)
    out.mkdir(parents=True, exist_ok=True)
    (out / "verdict.txt").write_text(text, encoding="utf-8")
    print("[smoke] %d steps, %d failed; %s" % (len(v.rows), len(fails), out / "verdict.txt"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
