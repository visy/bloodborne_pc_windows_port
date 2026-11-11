#!/usr/bin/env python3
"""Row verdicts of a two-PC session from the two logs zips (run on either PC, or anywhere).

  python twopc\\verdict.py twopc_logs_pc1_*.zip twopc_logs_pc2_*.zip [--json OUT.json]

Prints PASS / FAIL / SKIP per row and checks that no unmasked IP address is in either zip. Exit status 1 when a row FAILED.
Each row looks at ONE PC's log with that PC's own clock ("[+seconds]" since the session start): the two PCs' clocks are
never compared, so a row that needs "after the host did X" uses the guest's own log line of X.
"""
import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

CRASH_RX = (r"(?i)unhandled exception|assertion failed|EXCEPTION_ACCESS|fatal error|Fault at RIP|"
            r"the game exited with (?:-?[1-9]\d*|0x[0-9a-f]*[1-9a-f][0-9a-f]*)")
TS = re.compile(r"^\[\+ *([\d.]+)\] ")
MAP = r"map [^>\n]*-> %s"
LOAD_CAP = 45.0   # s: a loading screen longer than this fails the 1b row (the one-PC smoke sees 2-12 s)


def _ipmask():
    for d in (HERE / "repo" / "scripts", HERE.parent.parent / "scripts", HERE.parent / "scripts"):
        if (d / "ipmask.py").exists():
            sys.path.insert(0, str(d))
            import ipmask
            return ipmask
    return None


class Log:
    def __init__(self, text=""):
        self.rows = []
        for line in text.splitlines():
            m = TS.match(line)
            self.rows.append((float(m.group(1)) if m else -1.0, line[m.end():] if m else line))

    def find(self, rx, after=0.0, before=1e9):
        r = re.compile(rx)
        for t, line in self.rows:
            if after <= t < before and r.search(line):
                return t, line
        return None

    def all(self, rx, after=0.0, before=1e9):
        r = re.compile(rx)
        return [(t, line) for t, line in self.rows if after <= t < before and r.search(line)]


class Rows:
    def __init__(self, logs):
        self.logs, self.out = logs, []   # logs: {"pc1": Log, "pc2": Log}

    def add(self, name, ok, detail=""):
        self.out.append((name, "PASS" if ok else "FAIL", str(detail)))

    def info(self, name, detail=""):
        self.out.append((name, "PASS", str(detail)))

    def need(self, name, who, rx, after=0.0, before=1e9):
        hit = self.logs[who].find(rx, after, before)
        self.add(name, hit, ("%s +%.1f s: %s" % (who, hit[0], hit[1][:130])) if hit else "missing on %s: /%s/" % (who, rx))
        return hit[0] if hit else None

    def none(self, name, whos, rx, after=0.0, before=1e9):
        bad = [(w, h) for w in whos for h in self.logs[w].all(rx, after, before)]
        self.add(name, not bad, "none" if not bad else "%s +%.1f s: %s (%d in all)" % (bad[0][0], bad[0][1][0], bad[0][1][1][:130], len(bad)))

    def common(self, sid):
        self.none("%s: no crash / assert" % sid, ("pc1", "pc2"), CRASH_RX)
        self.none("%s: no 'Session lost'" % sid, ("pc1", "pc2"), r"Session lost")
        self.none("%s: no boot stall" % sid, ("pc1", "pc2"), r"\[WATCHDOG\] Boot stalled|asking for a boot snapshot")


# ---- the sessions -------------------------------------------------------------------------------------------------

def screens(L, host):
    """Loading-screen seconds of one PC for each party travel: host = from its warp step to its next screen drop; guest = from
    'loading screen raised at the host's travel' to the next drop. Returns [seconds or None]."""
    drop = r"Party seam: (InGameStart: )?loading screen dropped"
    out = []
    if host:
        starts = [t for t, _ in L.all(r"test: travel step \d+/\d+: host_warp|test: host_use goods 100 ")]
        for k, t0 in enumerate(starts):
            end = starts[k + 1] if k + 1 < len(starts) else 1e9
            d = L.find(drop, t0, end)
            out.append(d[0] - t0 if d else None)
    else:
        for t0, _ in L.all(r"guest: loading screen raised at the host's travel"):
            d = L.find(drop, t0)
            out.append(d[0] - t0 if d else None)
    return out


def s_1a_travel(R):
    c1, c2 = R.logs["pc1"], R.logs["pc2"]
    R.need("1a.travel join: 2 players", "pc1", r"cooperators 0 -> 1")
    n = len(c1.all(r"test: travel step \d+/\d+: host_warp"))
    R.add("1a.travel the host's 3 party warps ran (lamp, lamp, the Dream)", n >= 3, "%d warps" % n)
    R.need("1a.travel the host used the Hunter's Mark from the Dream", "pc1", r"test: host_use goods 100 ")
    j = len(c2.all(r"guest: re-join done after"))
    R.add("1a.travel the guest was back with the host after every travel (re-join done, >= 3)", j >= 3, "%d" % j)
    for tag, rx in (("Central Yharnam", "m24_01"), ("Cathedral Ward", "m24_00"), ("the Hunter's Dream", "m21_00")):
        R.need("1a.travel the guest arrived in %s (%s)" % (tag, rx), "pc2", MAP % rx)
    R.none("1a.travel nobody died", ("pc1", "pc2"), r"local death: HP 0")
    R.none("1a.travel no B1 fallback on the guest", ("pc2",), r"guest: FALLBACK")
    R.common("1a.travel")
    # 1b: fast load - the loading screens, per PC, from its own log
    h, g = screens(c1, True), screens(c2, False)
    for who, vals in (("pc1 (host)", h), ("pc2 (guest)", g)):
        good = [v for v in vals if v is not None]
        R.add("1b load times %s: every travel's screen measured (>= 3)" % who, len(good) >= 3,
              "screens s: " + (", ".join("%.1f" % v for v in good) or "none"))
        if good:
            R.add("1b load times %s: every screen <= %.0f s" % (who, LOAD_CAP), max(good) <= LOAD_CAP,
                  "mean %.1f s, max %.1f s" % (sum(good) / len(good), max(good)))
    gl = [t for t, _ in c2.all(r"guest: parallel follow: placed beside the host")]
    R.info("1b (info) parallel follows placed beside the host (guest)", "%d" % len(gl))
    st = [int(re.search(r"(\d+) ms", ln).group(1)) for _, ln in c1.all(r"Stall probe: \d+ ms frame") + c2.all(r"Stall probe: \d+ ms frame")]
    R.info("1b (info) frames over 100 ms after the screen drops (both PCs)", "%d" % sum(1 for x in st if x >= 100))


def s_1a_rejoin(R):
    c2 = R.logs["pc2"]
    R.need("1a.rejoin join: 2 players", "pc1", r"cooperators 0 -> 1")
    t0 = R.need("1a.rejoin the guest pressed Party > Rejoin in the host's world", "pc2", r"Party > Rejoin pressed now")
    if t0 is None:
        return R.common("1a.rejoin")
    R.need("1a.rejoin the quick rejoin started (seam D)", "pc2", r"quick rejoin \(seam D\): a re-join beside the host", t0)
    R.need("1a.rejoin the host re-sent its snapshot", "pc1", r"host: a guest respawns in this world|host: destination sent")
    R.need("1a.rejoin the guest is back beside the host (re-join done)", "pc2", r"guest: re-join done after", t0)
    n = len(c2.all(r"Party seam: (InGameStart: )?loading screen dropped", t0))
    R.add("1a.rejoin one loading screen", n == 1, "%d" % n)
    R.none("1a.rejoin no trip home (no own-world load)", ("pc2",), r"Party > Leave pressed|left the session", t0)
    R.none("1a.rejoin no B1 fallback", ("pc2",), r"guest: FALLBACK", t0)
    R.common("1a.rejoin")


def s_1a_join(R):
    c1, c2 = R.logs["pc1"], R.logs["pc2"]
    R.need("1a.join join: 2 players", "pc1", r"cooperators 0 -> 1")
    lv = R.need("1a.join the guest left (Party > Leave)", "pc2", r"guest_leave: Party > Leave pressed")
    out = R.need("1a.join the host saw it go (cooperators 1 -> 0)", "pc1", r"cooperators 1 -> 0")
    R.need("1a.join the guest pressed Party > Rejoin", "pc2", r"guest_rejoin: Party > Rejoin pressed", lv or 0)
    back = c1.find(r"cooperators 0 -> 1", out or 0)
    R.add("1a.join the guest is back in the host's party (cooperators 0 -> 1 again)", bool(back), "host +%.1f s" % back[0] if back else "never")
    w2 = c1.find(r"test: travel step 2/\d+: host_warp", back[0] if back else 0)
    R.add("1a.join the host's next warp started right after the guest's rejoin (<= 15 s)", bool(w2 and back and w2[0] - back[0] <= 15),
          "%.1f s after the rejoin" % (w2[0] - back[0]) if w2 and back else "never")
    R.need("1a.join the guest followed the host's warp to the Cathedral Ward (m24_00)", "pc2", MAP % "m24_00")
    j = len(c2.all(r"guest: re-join done after"))
    R.add("1a.join the guest was back with the host after each travel (re-join done, >= 2)", j >= 2, "%d" % j)
    R.none("1a.join no failed direct summon", ("pc1", "pc2"), r"direct summon: FAILED")
    R.none("1a.join no join waits on a crash hold (nobody crashed)", ("pc1", "pc2"), r"a crashed member's (old )?connection is still held")
    R.none("1a.join no B1 fallback on the guest", ("pc2",), r"guest: FALLBACK")
    R.common("1a.join")


def s_boss(R, sid):
    c1, c2 = R.logs["pc1"], R.logs["pc2"]
    R.need("%s join: 2 players" % sid, "pc1", r"cooperators 0 -> 1")
    died = R.need("%s the guest dies at the lamp, before the fight" % sid, "pc2", r"local death: HP 0")
    R.need("%s the guest gets up in place (no load)" % sid, "pc2", r"died alone: revived at its own last lamp", died or 0)
    R.need("%s the host's copy of the guest was rebuilt (member revived, rewound)" % sid, "pc1",
           r"member revived: task mode 1 \(step \d+\) rewound to STEP_Create")
    R.need("%s the rebuilt copy's dead bit is read and cleared once" % sid, "pc1",
           r"rebuilt member chr [0-9a-f]+ \(HP [1-9]\d*, rebuilt\): target-bank state")
    bar = R.need("%s the fight starts (bar, host)" % sid, "pc1", r"boss health bar shown")
    if bar is None:
        return R.common(sid)
    lim = R.need("%s the host dies in the fight: the limbo" % sid, "pc1", r"host death in a boss fight", bar)
    mortal = R.need("%s the guest is mortal (god_bar) after the host's death" % sid, "pc2", r"test: god_bar: mortal now")
    if lim is None or mortal is None:
        return R.common(sid)
    hits = c2.all(r"Party balance: hit: HP", mortal, mortal + 60)
    dhit = c2.find(r"local death: HP 0", mortal, mortal + 60)
    first = min([t for t, _ in hits] + ([dhit[0]] if dhit else []), default=None)
    R.add("%s the boss hits the guest within 60 s of the host's death (hit lines, or its death)" % sid, first is not None,
          "%d hit(s), guest died: %s, first %s" % (len(hits), dhit is not None,
                                                   "%+.1f s after the mortal point" % (first - mortal) if first is not None else "never"))
    gh = None
    for _t, line in c1.all(r"boss target dump \S+: players", lim, lim + 120.0):
        chrs = re.findall(r"\[chr ([0-9a-f]+) hp", line)
        if len(chrs) >= 2:
            gh = chrs[1]
    k0 = [t for t, d in c1.all(r"boss target dump \S+: boss [0-9a-f]+: ", lim, lim + 120.0)
          if gh and re.search(r"kind [01] [^\]]*h %s\]" % gh, d)]
    R.add("%s the host's boss target dump lists an entry (kind 0 / 1) for the guest" % sid, bool(k0), "guest chr %s: %d dump(s)" % (gh, len(k0)))
    stuck = c1.all(r"boss state: [0-9a-f]+ may be stuck", lim, lim + 120.0)
    R.add("%s no 'may be stuck' line on the host after the death" % sid, not stuck, "%d" % len(stuck))
    R.common(sid)


def s_1d(R):
    R.need("1d guest: the region patch is on (sell region 5)", "pc2", r"Patches: .*writes from \[[^\]]*'Unlock Game Region'")
    R.none("1d host: the region patch is NOT on the host", ("pc1",), r"Patches: .*writes from \[[^\]]*'Unlock Game Region'")
    R.need("1d guest: ss.info built in its own process (the region fix)", "pc2", r"http: ss\.info answered in this process")
    t = R.need("1d guest: ss.info answered (HTTP 200)", "pc2", r"http: GET https://\S+/ss\.info -> 200")
    R.need("1d guest: the sign-in finished (net up)", "pc2", r"online: FrpgNetMan net 1 online 1 .*user \d+ session [1-9]\d* chars", t or 0)
    R.none("1d guest: never fell back to offline mode at the title", ("pc2",), r"Party: (\[[^\]]*\] )?online mode 1 -> 0")
    R.need("1d host: the dungeon load started", "pc1", r"altar: the party's dungeon is loading")
    R.need("1d guest followed into the dungeon (m29) through the summon", "pc2", MAP % "m29_00", t or 0)
    R.none("1d the guest was not offline-held / sent home", ("pc2",),
           r"chalice: notice shown: This game is (in offline mode|offline)|send-home at \+0x138a51c \(OnLeave_Limit\) dropped")
    R.common("1d")


def s_1e(R):
    c1, c2 = R.logs["pc1"], R.logs["pc2"]
    R.none("1e invader: no modal refusal box at any time", ("pc2",), r"refusal notice \(modal\)")
    R.need("1e invader: dialled before the host had a world and was told to wait (refused for now, asking again)", "pc2",
           r"invasion refused for now \(.*\): asking again in \d+ s")
    R.need("1e invader: the host's gate named the moment (not in a world / loading) before admitting", "pc1",
           r"invade: gate: invader \(name hidden\): (not_in_world|loading)")
    ins = [t for t, _ in c1.all(r"invade: invader slot \d+: in the world")]
    R.add("1e host: the invader got into the world", bool(ins), "%d time(s)" % len(ins))
    R.need("1e invader: told it invades", "pc2", r"invade: notice: You invade ")
    warp = c1.find(r"test: travel step \d+/\d+: host_warp")
    if warp and ins:
        tw = warp[0]
        before = [t for t in ins if t <= tw]
        ends = [t for t, _ in c1.all(r"invasion of slot \d+ ends")]
        inside = bool(before) and not any(before[-1] < e < tw for e in ends)
        R.add("1e the host travelled while an invader was inside (in the world, no end before the warp)", inside,
              "invader in +%.1f s, host warp +%.1f s" % (before[-1] if before else -1, tw))
        end = c1.find(r"invasion of slot \d+ ends: travel", tw, tw + 15)
        R.add("1e the host's travel ended that invasion (ends: travel)", bool(end), "host +%.1f s" % end[0] if end else "no end line")
        again = [t for t in ins if t > tw]
        R.add("1e the invader asked again after the travel and was let back in (retry)", bool(again), "host +%.1f s" % again[0] if again else "never")
    else:
        R.add("1e the host travelled while an invader was inside", False, "no host warp or no invader in the world")
    home = c2.find(r"host lost \([^)]*\): home now \(OnLeave_Limit\)")
    R.add("1e invader: sent home when the host left the world", bool(home), home[1][:130] if home else "no home line")
    R.none("1e invader: no hang (not sent home by the 90 s / 120 s guards)", ("pc2",),
           r"could not get into the host's world|did not finish within 90 s|FallbackToB1|no host snapshot within 45 s")
    R.common("1e")


def s_3(R, vpn, sid="3"):
    R.need("%s join over the tunnel: 2 players" % sid, "pc1", r"cooperators 0 -> 1")
    R.need("%s the party warp ran (host)" % sid, "pc1", r"test: travel step \d+/\d+: host_warp")
    R.need("%s the guest arrived in Central Yharnam (m24_01)" % sid, "pc2", MAP % "m24_01")
    R.none("%s nobody died" % sid, ("pc1", "pc2"), r"local death: HP 0")
    p1, p2 = vpn.get("pc1"), vpn.get("pc2")
    R.add("%s both PCs had a Nightwatch tunnel address" % sid, bool(p1 and p2),
          "pc1 %s, pc2 %s" % ((p1 or {}).get("own", "none"), (p2 or {}).get("own", "none")))
    if p2:
        R.info("%s (info) tunnel /24s: pc1 %s, pc2 %s: %s" % (sid, (p1 or {}).get("own", "?"), p2.get("own"),
                                                              "same /24" if p2.get("same_24") else "different /24"), "joined via " + p2.get("via", "?"))
    if sid == "3":
        # NW-03d11e5: the guest joined with the host's published code, and that code was a full address (not a LAN-partial one:
        # a partial code makes the guest log 'LAN code: host ... (first three octets from our address ...)')
        R.need("3 the guest joined by the host's published party code", "pc2", r"joining \S+ as \S+ \((LAN code|Internet code|address)\)")
        R.none("3 the host's code is a full address (not a LAN-partial BBP code)", ("pc2",), r"LAN code: host \S+ \(first three octets")
        R.none("3 no LAN fallback probe was needed to find the host", ("pc2",), r"LAN fallback: ")
        R.add("3 the code came from the host (not the lobby row)", bool(p2) and p2.get("via") == "party code", (p2 or {}).get("via", "no report"))
    else:
        R.add("3b the guest found the host in the VPN lobby (fallback)", bool(p2) and p2.get("via") == "lobby row", (p2 or {}).get("via", "no report"))
    R.common(sid)


SESSIONS = {
    "1a.travel": s_1a_travel, "1a.rejoin": s_1a_rejoin, "1a.join": s_1a_join,
    "1c": lambda R: s_boss(R, "1c"), "1d": s_1d, "1e": s_1e,
}


# ---- reading the zips -------------------------------------------------------------------------------------------------

def read_zip(path):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    meta = {}
    for n in names:
        if n.endswith("/meta.json"):
            meta = json.loads(z.read(n).decode("utf-8"))
    files = {n.split("/", 1)[1]: z.read(n).decode("utf-8", errors="replace") for n in names if "/" in n and not n.endswith("/")}
    return meta, files, z


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zips", nargs=2)
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    data = {}
    for p in a.zips:
        meta, files, _ = read_zip(p)
        role = meta.get("role")
        if role not in ("pc1", "pc2") or role in data:
            sys.exit("%s: not a pc1/pc2 logs zip (or both are the same PC's)" % p)
        data[role] = (meta, files)
    if set(data) != {"pc1", "pc2"}:
        sys.exit("need one pc1 and one pc2 logs zip")
    plan = data["pc1"][0].get("plan", [])
    rows, ok = [], True
    ipm = _ipmask()
    for sid in ("1a.travel", "1a.rejoin", "1a.join", "1c", "1d", "1e", "2a", "2c", "3", "3b"):
        if sid not in plan:
            rows.append(("%s: not in this session's plan" % sid, "SKIP", ""))
            continue
        logs = {r: Log(data[r][1].get("%s/%s.log" % (sid, r), "")) for r in ("pc1", "pc2")}
        R = Rows(logs)
        res = {r: next((x for x in data[r][0].get("results", []) if x.get("id") == sid), {}) for r in ("pc1", "pc2")}
        for r in ("pc1", "pc2"):
            if res[r] and not res[r].get("ok"):
                R.add("%s %s: the session ran to its end" % (sid, r), False, res[r].get("note", ""))
        if sid in ("2a", "2c"):
            R.need("%s the guest ran the simulated DSL (BB_NET_SIM=dsl,any=1: it acts on a real peer, not loopback only)" % sid, "pc2",
                   r"network simulator on \(lat=80,[^)]*\)(?! \(loopback peers only\))")
            R.none("%s the simulator was not limited to loopback peers" % sid, ("pc2",), r"loopback peers only")
        if sid == "2a":
            tmp = Rows(logs)
            s_1a_travel(tmp)
            R.out += [(n.replace("1a.travel", "2a").replace("1b ", "2a/1b "), s, d) for n, s, d in tmp.out]
        elif sid == "2c":
            s_boss(R, "2c")
        elif sid in ("3", "3b"):
            vpn = {}
            for r in ("pc1", "pc2"):
                txt = data[r][1].get("%s/vpn_report_%s.json" % (sid, r))
                if txt:
                    vpn[r] = json.loads(txt)
            s_3(R, vpn, sid)
        else:
            SESSIONS[sid](R)
        rows += R.out
    # no unmasked address anywhere
    for role in ("pc1", "pc2"):
        bad = []
        for n, txt in data[role][1].items():
            if ipm:
                bad += [(n, x) for x in ipm.unmasked_ips(txt)]
        rows.append(("no unmasked IP address in the %s zip" % role, "PASS" if not bad else "FAIL",
                     "none" if not bad else "%s: %s (%d in all)" % (bad[0][0], re.sub(r"\d+$", "x", str(bad[0][1])), len(bad))))
    width = max(len(n) for n, _, _ in rows)
    for n, s, d in rows:
        print("%-4s  %-*s  %s" % (s, min(width, 100), n, d[:150]))
    fails = [r for r in rows if r[1] == "FAIL"]
    print("\n%d rows, %d passed, %d failed, %d skipped" % (len(rows), sum(1 for r in rows if r[1] == "PASS"), len(fails),
                                                          sum(1 for r in rows if r[1] == "SKIP")))
    if a.json:
        Path(a.json).write_text(json.dumps(rows, indent=1), encoding="utf-8")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
