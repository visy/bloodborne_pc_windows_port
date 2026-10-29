#!/usr/bin/env python3
"""Smoke test of the Nightwatch VPN lobby (CL-cec865) on loopback, two real games.

  python tools/mp/vpnlobby_smoke.py [--root DIR] [--out DIR]

What it does: runs tools/vpn/lobby/nwlobby.py on 127.0.0.1 (test override NWLOBBY_NET), starts a host game with a
password and BB_PARTY_VPN_LOBBY=1 (announces to the test lobby), waits until the launcher's own client code
(launcher.py lobby_fetch / lobby_rows, the same functions the Join list uses) lists the host by its character name,
writes the joined address from the chosen row to a code file ONLY the guest reads (the address never goes to the
screen or a log), starts the guest with the host's password and checks that it joins from that list entry.
Screenshots of both games are taken after the join. PASS / FAIL lines on stdout, exit 0 only when every check passed.
"""
import argparse
import importlib.util
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
import instances  # noqa: E402

PASSWORD = "lobby-smoke-pw"
failures = []


def check(ok, text):
    print(("PASS " if ok else "FAIL ") + text, flush=True)
    if not ok:
        failures.append(text)


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_smoke", REPO / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def log_text(root, i):
    try:
        return (Path(root) / f"inst{i}" / "run.log").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--seconds", type=int, default=420)
    a = ap.parse_args()
    root = Path(a.root or (REPO.parent / "mptest")).resolve()
    out = Path(a.out or (root / "vpnlobby_shots"))
    out.mkdir(parents=True, exist_ok=True)
    port0 = instances.base_port(root)
    lobby_port = port0 + 8
    addr = f"127.0.0.1:{lobby_port}"
    code_file = root / "lobby_code.txt"
    code_file.unlink(missing_ok=True)

    L = load_launcher()
    lobby_env = dict(os.environ, NWLOBBY_BIND=addr, NWLOBBY_NET="127.0.0.0/8")
    lobby = subprocess.Popen([sys.executable, "-I", str(REPO / "tools" / "vpn" / "lobby" / "nwlobby.py")], env=lobby_env)
    games = None
    try:
        time.sleep(1)
        run = [sys.executable, str(HERE / "instances.py"), "run", "--party", "--count", "2", "--root", str(root),
               "--seconds", str(a.seconds),
               "--env", "BB_PARTY_PASSWORD=" + PASSWORD, "--env", "BB_PARTY_VPN_LOBBY=1",
               "--env", "BB_PARTY_VPNLOBBY_ADDR=" + addr,
               "--ienv", "1:BB_PARTY_CODE_FILE=" + str(code_file),
               "--start-when", "1/0:VPN lobby: announcing"]
        games = subprocess.Popen(run)
        # 1. the host shows up in the list, by its character name
        rows, deadline = [], time.time() + 1500
        while time.time() < deadline and not rows:
            time.sleep(3)
            ents = L.lobby_fetch(("127.0.0.1", lobby_port), "127.0.0.1")
            rows = L.lobby_rows(ents or [], "")  # no version filter needed for the check
            if games.poll() is not None:
                break
        check(bool(rows), "the host is in the lobby list")
        if rows:
            r = rows[0]
            print("list row: name=%r slots=%s password=%s build=%r" % (r["name"], r["players"], r["password"], r["build"]),
                  flush=True)
            check(r["password"] == "yes", "the row says the game has a password")
            check(r["name"] != "" and not re.search(r"\d+\.\d+\.\d+\.\d+", r["name"]), "the row name is a character name, no address")
            check(not any("127.0.0.1" in str(v) for k, v in r.items() if k != "address"), "no address in anything shown")
            # 2. join from the list entry: the code file only the guest reads
            code_file.write_text(L.with_party_port(r["address"]) + "\n")
        # 3. the guest joins the host from that entry
        deadline = time.time() + a.seconds
        joined = False
        while time.time() < deadline and games.poll() is None and rows:
            time.sleep(3)
            if "cooperators 0 -> 1" in log_text(root, 0):
                joined = True
                break
        check(joined, "the guest joined the host from the list entry (host log: cooperators 0 -> 1)")
        if joined:
            time.sleep(20)
            subprocess.run([sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(out), "--tag", "joined"])
            ents = L.lobby_fetch(("127.0.0.1", lobby_port), "127.0.0.1") or []
            check(any(e["players"] >= 2 for e in ents), "the list shows the second player after the join")
            gl = log_text(root, 1)
            check("password" not in gl.lower() or PASSWORD not in gl, "the guest log does not contain the password")
            # the address stays out of the game logs of both sides
            check("announcing to the Nightwatch lobby" in log_text(root, 0), "host log: announcing line without an address")
            check(not re.search(r"VPN lobby:.*\d+\.\d+\.\d+\.\d+", log_text(root, 0)), "host log: no address on lobby lines")
    finally:
        subprocess.run([sys.executable, str(HERE / "instances.py"), "kill", "--root", str(root)])
        if games and games.poll() is None:
            games.terminate()
        lobby.terminate()
    print("RESULT: " + ("PASS" if not failures else "FAIL (%d)" % len(failures)), flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
