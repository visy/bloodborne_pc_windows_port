#!/usr/bin/env python3
r"""Run a command while holding one machine-wide game slot (tools/mp/game_queue.py), for runs that start a
game outside the harness (the WSL / Proton tests in tools/linux): the lease counts against the 2-client cap.

  python tools/linux/with_game_slot.py --label "proton wsl" -- wsl -d Ubuntu-22.04 -- bash script.sh
"""
import argparse, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mp"))
import game_queue  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="proton wsl")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd:
        ap.error("no command")
    with game_queue.game_slots(1, a.root, label=a.label):
        return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
