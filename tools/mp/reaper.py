#!/usr/bin/env python3
"""Central idle reaper for agents' test games (works whatever harness version an agent runs).

  python tools/mp/reaper.py [--idle 10]

Every 2 s, for each run holding game slots (tools/mp/game_queue.py), it reads the run's
<root>/inst*/run.log and counts the distinct "Party:"/"Net:" lines (numbers blanked, so periodic
status lines don't count). When that count has not grown for --idle seconds, the run has what it
can get: its run.bat shells and games are ended (the harness then sees them exit and finishes).
"""
import argparse, json, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_queue import _locked, _prune  # noqa: E402

PARTY = re.compile(r"^(Party|Net): ")
NUM = re.compile(r"0x[0-9a-fA-F]+|\d[\d.:]*")


def distinct_party_lines(root: Path) -> int:
    seen = set()
    for log in root.glob("inst*/run.log"):
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            if PARTY.match(line):
                seen.add(NUM.sub("#", line))
    return len(seen)


def all_in_world(root: Path) -> bool:
    """Every instance's latest director world line is "Party: world up" (not "loading screen" or
    "world down")."""
    logs = list(root.glob("inst*/run.log"))
    if not logs:
        return False
    for log in logs:
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return False
        marks = re.findall(r"^Party: (world up|world down|loading screen)", text, re.M)
        if not marks or marks[-1] != "world up":
            return False
    return True


def end_run(root: Path):
    """Ends the run.bat shells and games under `root` (not the harness itself)."""
    r = str(root).replace("/", "\\").rstrip("\\").lower()
    ps = ("Get-CimInstance Win32_Process | Where-Object { "
          "($_.Name -in 'bbport.exe','cmd.exe') -and "
          f"((\"$($_.ExecutablePath)\".ToLower().StartsWith('{r}\\')) -or (\"$($_.CommandLine)\".ToLower().Contains('{r}\\'))) "
          "} | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; $_.ProcessId }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    return out.split()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--idle", type=float, default=10)
    a = ap.parse_args()
    state = {}  # holder id -> (count, last change)
    while True:
        try:
            with _locked("game") as st:
                _prune(st)
                holders = [dict(e) for e in st["holders"]]
        except SystemExit:
            holders = []
        now = time.time()
        live = set()
        for e in holders:
            hid = e["id"]
            live.add(hid)
            root = Path(e["root"])
            # A run with a legitimately long quiet phase opts out with a file <root>/noreap
            # holding the number of seconds of quiet it needs.
            idle = a.idle
            try:
                idle = max(idle, float((root / "noreap").read_text().strip() or 600))
            except (OSError, ValueError):
                pass
            n = distinct_party_lines(root)
            # Only once every instance has reached its world: boots, title and loads are quiet
            # phases that must not end a run.
            if not all_in_world(root):
                state[hid] = (n, now)
                continue
            count, since = state.get(hid, (-1, now))
            if n != count:
                state[hid] = (n, now)
            elif n > 0 and now - since > idle:
                pids = end_run(root)
                print(time.strftime("%H:%M:%S"), f"idle {idle:.0f}s: ended {e.get('label') or root} "
                      f"({len(pids)} processes)", flush=True)
                state[hid] = (n, now + 3600)  # don't repeat while the harness winds down
        for hid in list(state):
            if hid not in live:
                del state[hid]
        time.sleep(2)


if __name__ == "__main__":
    main()
