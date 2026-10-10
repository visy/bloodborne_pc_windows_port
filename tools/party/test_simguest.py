#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs tools/party/simguest.py against the real host code (tests/party_host_harness.cpp: PartyLink
host + PartyHostService + FromApi + the party UDP port, no game) and checks the verdicts.

  ninja -C out/gpu party-host-harness
  python tools/party/test_simguest.py [--harness out/gpu/party-host-harness.exe] [--soak N] [-v]

Cases: join by party code + password, crash-rejoin as a restarted process (no token),
crash-rejoin with the resume token, a wrong password (must be REJECTed: auth), a different game
version (REJECT: mismatch), then optionally soak N. Exit code 0 when every case behaves.
"""
from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import simguest  # noqa: E402


def free_port() -> int:
    for _ in range(50):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as t, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
            try:
                t.bind(("0.0.0.0", 0))
                p = t.getsockname()[1]
                u.bind(("0.0.0.0", p))
                return p
            except OSError:
                continue
    raise RuntimeError("no free port")


class Harness:
    def __init__(self, exe: str, port: int, password: str, secret: str, verbose: bool):
        self.lines: list[str] = []
        self.verbose = verbose
        self.p = subprocess.Popen([exe, "--port", str(port), "--password", password, "--secret", secret],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.ready = threading.Event()
        self.code = ""
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.p.stdout:
            line = line.rstrip("\n")
            self.lines.append(line)
            if self.verbose:
                print("    | " + line)
            if line.startswith("HARNESS READY"):
                self.code = line.split("code=")[1].split()[0]
                self.ready.set()

    def stop(self) -> None:
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()

    def grep(self, text: str) -> list[str]:
        return [ln for ln in self.lines if text in ln]


def run_simguest(args: list[str], verbose: bool) -> tuple[int, str]:
    cmd = [sys.executable, os.path.join(HERE, "simguest.py")] + args
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
    out = r.stdout + r.stderr
    if verbose:
        print("\n".join("    > " + ln for ln in out.splitlines()))
    verdict = next((ln for ln in reversed(r.stdout.splitlines()) if ln.startswith(("PASS ", "FAIL "))), "(no verdict)")
    return r.returncode, verdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", default=os.path.join(ROOT, "out", "gpu", "party-host-harness.exe"))
    ap.add_argument("--soak", type=int, default=0)
    ap.add_argument("--logs", default=os.path.join(ROOT, "out", "simguest-test"), help="JSON logs go here")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if not os.path.isfile(a.harness):
        print(f"no harness at {a.harness}: ninja -C out/gpu party-host-harness")
        return 2
    os.makedirs(a.logs, exist_ok=True)
    port, password, secret = free_port(), "simpw", os.urandom(8).hex()
    t = time.monotonic()
    key, how = simguest.derive_party_key(password, bytes.fromhex(secret))
    print(f"party key derived via {how} in {time.monotonic() - t:.1f} s; harness on port {port}")
    h = Harness(a.harness, port, password, secret, a.verbose)
    results = []
    try:
        if not h.ready.wait(15):
            print("FAIL harness did not come up:\n" + "\n".join(h.lines[-20:]))
            return 1
        code = h.code
        log = lambda n: ["--json-log", os.path.join(a.logs, f"{n}.json")]  # noqa: E731
        cases = [
            # name, simguest args, expect pass, required verdict text
            ("join (code + password, real Argon2i)", ["--code", code, "--password", password, "--name", "SimA",
                                                      "--script", "join", "--hold", "6"] + log("join"), True, ""),
            ("crash-rejoin (restarted process)", ["--code", code, "--key-hex", key.hex(), "--name", "SimB",
                                                  "--script", "crash-rejoin", "--hold", "2"] + log("crash"), True, ""),
            ("crash-rejoin (resume token)", ["--code", code, "--key-hex", key.hex(), "--name", "SimC",
                                             "--script", "crash-rejoin", "--keep-token"] + log("crash-token"),
             True, ""),
            ("wrong password -> REJECT auth", ["--code", code, "--password", "nope", "--name", "SimD",
                                               "--script", "join"] + log("wrong-pw"), False, "rejected (auth)"),
            ("other game version -> REJECT mismatch", ["--code", code, "--key-hex", key.hex(), "--name", "SimE",
                                                       "--eboot-sha256", "11" * 32, "--script", "join"] +
             log("mismatch"), False, "rejected (mismatch)"),
            ("host:port + secret", ["--host", "127.0.0.1", "--port", str(port), "--secret", secret,
                                    "--key-hex", key.hex(), "--name", "SimF", "--script", "join"] + log("plain"),
             True, ""),
        ]
        if a.soak:
            cases.append((f"soak {a.soak}", ["--code", code, "--key-hex", key.hex(), "--name", "SimS",
                                             "--script", "soak", str(a.soak), "--hold", "3"] + log("soak"), True, ""))
        for name, args, expect, text in cases:
            t = time.monotonic()
            rc, verdict = run_simguest(args, a.verbose)
            good = (rc == 0) == expect and (not text or text in verdict)
            results.append(good)
            print(f"{'ok  ' if good else 'BAD '} {name}: {verdict} ({time.monotonic() - t:.1f} s)")
        # The host side saw what the guests did.
        signs = h.grep("valid")
        invalid = h.grep("INVALID")
        rejoined = h.grep("is back")
        print(f"host: {len(signs)} valid signs, {len(invalid)} invalid, {len(rejoined)} rejoins, "
              f"{len(h.grep('summons'))} summons")
        host_ok = bool(signs) and not invalid and len(rejoined) >= 2
        results.append(host_ok)
        print(f"{'ok  ' if host_ok else 'BAD '} host-side checks")
    finally:
        h.stop()
    status = [ln for ln in h.lines if ln.startswith("HARNESS EXIT") or ln.startswith("HARNESS STATUS")]
    if status:
        print("host: " + status[-1])
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'} test_simguest: {sum(results)}/{len(results)} checks")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
