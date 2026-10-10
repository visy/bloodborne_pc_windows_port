#!/usr/bin/env python3
"""Decompile game functions with Ghidra (headless) from the analysed project.

  python tools/re/decomp.py 0x21d5a80 0xe45560 [--out FILE]

Addresses are our guest offsets (raw ELF VA of out/eboot.elf; bbhost addresses minus 0x400000).
Needs the one-time project made by:
  analyzeHeadless E:\\games\\bbport\\deps\\ghidra-proj bloodborne -import eboot.elf
      -loader ElfLoader -loader-imagebase 0
Env: GHIDRA_INSTALL_DIR (default E:\\games\\bbport\\deps\\ghidra\\ghidra_12.1.4_PUBLIC),
     BB_GHIDRA_PROJECT (default E:\\games\\bbport\\deps\\ghidra-proj).
"""
import argparse, os, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
GHIDRA = Path(os.environ.get("GHIDRA_INSTALL_DIR", r"E:\games\bbport\deps\ghidra\ghidra_12.1.4_PUBLIC"))
PROJECT = Path(os.environ.get("BB_GHIDRA_PROJECT", r"E:\games\bbport\deps\ghidra-proj"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addresses", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    out = Path(a.out) if a.out else Path(tempfile.gettempdir()) / "bb_decomp.txt"
    cmd = [str(GHIDRA / "support" / "analyzeHeadless.bat"), str(PROJECT), "bloodborne",
           "-process", "eboot.elf", "-noanalysis", "-readOnly",
           "-scriptPath", str(HERE / "ghidra_scripts"), "-postScript", "DecompileAt.java", str(out),
           *a.addresses]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if not out.exists():
        sys.stderr.write(r.stdout[-4000:] + r.stderr[-4000:])
        sys.exit(1)
    sys.stdout.write(out.read_text(encoding="utf-8", errors="replace"))


if __name__ == "__main__":
    main()
