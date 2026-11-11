#!/usr/bin/env python3
r"""Dry run of the whole two-PC kit on this one PC: two unpacked kits (pc1, pc2), two runners, loopback party, the game queue.

  python tools/twopc/dryrun.py --stage E:\games\bbport\rel081 --game E:\games\bbport\agents\twopc081\game --only 1a.rejoin,1e
        [--root DIR] [--saves DIR]

The games are the staged build's bbport.exe (hard links) with BB_MP_LOCAL_TEST-style loopback networking (the runner's
--loopback), BB_FPS=60. It holds two game-queue slots for the whole run (tools/mp/game_queue.py) and ends every game it
started; then verdict.py judges the two logs zips. Never touches a player's save folder (the kits' own bbport\user only).
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "mp"))
import make_kits  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", default=r"E:\games\bbport\rel081")
    ap.add_argument("--game", default=str(REPO.parent / "game"))
    ap.add_argument("--saves", default=r"E:\games\bbport\test_saves")
    ap.add_argument("--root", default=str(REPO.parent / "dry"))
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--port-base", type=int, default=47680)
    ap.add_argument("--lobby", action="store_true", help="start a local VPN lobby (tools/vpn/lobby/nwlobby.py) for session 3")
    ap.add_argument("--keep-kits", action="store_true", help="keep bbport/ and the saves of the kit folders (only twopc/ is refreshed)")
    a = ap.parse_args()
    root = Path(a.root)
    readme = (HERE / "README_kit.txt").read_text(encoding="utf-8")
    for role in ("pc1", "pc2"):
        d = root / role
        groups = make_kits.kit_members(role, a.stage, a.game, a.saves, readme)
        fresh = not a.keep_kits or not (d / "bbport" / "run.bat").exists()
        make_kits.write_dir(d, (groups[0] if fresh else [], groups[1], groups[2] if fresh else [], [], groups[4]), True)
        (d / "twopc" / "config.json").write_text(json.dumps({"role": role, "game_dir": str(Path(a.game))}), encoding="utf-8")
        for old in d.glob("twopc_logs_*.zip"):
            old.unlink()
    sync = root / "sync"
    if sync.exists():
        for f in sync.glob("*"):
            f.unlink()
    from game_queue import game_slots
    procs = []
    lobby = None
    lobby_addr = ""
    if a.lobby:
        lobby_addr = "127.0.0.1:%d" % (a.port_base + 90)
        lobby = subprocess.Popen([sys.executable, "-I", str(REPO / "tools" / "vpn" / "lobby" / "nwlobby.py")],
                                 env=dict(os.environ, NWLOBBY_BIND=lobby_addr, NWLOBBY_NET="127.0.0.0/8"))
    with game_slots(2, str(root), label="twopc081 dry run"):
        print("[dryrun] slots held; starting the two runners", flush=True)
        env = dict(os.environ, BB_MP_PARENT_PID=str(os.getpid()))
        for role in ("pc1", "pc2"):
            d = root / role
            cmd = [str(d / "bbport" / "python" / "python.exe"), "-I", str(d / "twopc" / "runner.py"), "--role", role,
                   "--loopback", "--sync-dir", str(sync), "--port-base", str(a.port_base)]
            if role == "pc1":
                cmd += ["--only", a.only, "--skip", a.skip]
            if lobby_addr:
                cmd += ["--vpn-lobby-test", lobby_addr]
                if role == "pc1":
                    cmd += ["--vpn"]
            procs.append((role, subprocess.Popen(cmd, cwd=str(d), env=env, stdout=open(root / ("%s_runner.txt" % role), "w"),
                                                 stderr=subprocess.STDOUT)))
        try:
            while any(p.poll() is None for _, p in procs):
                time.sleep(5)
        except KeyboardInterrupt:
            for _, p in procs:
                p.kill()
        finally:
            for _, p in procs:
                if p.poll() is None:
                    p.kill()
            if lobby:
                lobby.kill()
            for role in ("pc1", "pc2"):
                subprocess.run([str(root / role / "bbport" / "python" / "python.exe"), "-I", "-c",
                                "import sys; sys.path.insert(0, %r); import runner; runner.kill_kit_games(%r)"
                                % (str(root / role / "twopc"), str(root / role))], capture_output=True)
    zips = [sorted((root / r).glob("twopc_logs_%s_*.zip" % r))[-1:] for r in ("pc1", "pc2")]
    print("[dryrun] runners exit:", [p.returncode for _, p in procs], "zips:", zips, flush=True)
    if all(zips):
        subprocess.run([sys.executable, str(HERE / "verdict.py"), str(zips[0][0]), str(zips[1][0])])


if __name__ == "__main__":
    main()
