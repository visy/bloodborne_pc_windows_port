#!/usr/bin/env python3
"""Find references with Ghidra (headless): `refs.py 0x553b120 summon_messenger [--out FILE]`.

Hex arguments list references to that address; text arguments list defined strings (ASCII or
UTF-16) containing it and the functions referencing each. Addresses are our guest offsets.
See decomp.py for the project setup.
"""
import argparse, os, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
GHIDRA = Path(os.environ.get("GHIDRA_INSTALL_DIR", r"E:\games\bbport\deps\ghidra\ghidra_12.1.4_PUBLIC"))
PROJECT = Path(os.environ.get("BB_GHIDRA_PROJECT", r"E:\games\bbport\deps\ghidra-proj"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("queries", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    out = Path(a.out) if a.out else Path(tempfile.gettempdir()) / "bb_refs.txt"
    cmd = [str(GHIDRA / "support" / "analyzeHeadless.bat"), str(PROJECT), "bloodborne",
           "-process", "eboot.elf", "-noanalysis", "-readOnly",
           "-scriptPath", str(HERE / "ghidra_scripts"), "-postScript", "FindRefs.java", str(out),
           *a.queries]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if not out.exists():
        sys.stderr.write(r.stdout[-4000:] + r.stderr[-4000:])
        sys.exit(1)
    sys.stdout.write(out.read_text(encoding="utf-8", errors="replace"))


if __name__ == "__main__":
    main()
