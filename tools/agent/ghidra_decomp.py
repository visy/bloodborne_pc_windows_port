#!/usr/bin/env python3
"""Cached Ghidra decompiler + text export for agents (no Ghidra start for cached lookups).

  python tools/agent/ghidra_decomp.py 0x176cfa0 0x16439b0      # decompile (cached), print paths
  python tools/agent/ghidra_decomp.py --cat 0x176cfa0           # ...and print the C
  python tools/agent/ghidra_decomp.py --callers 0x16439b0       # from calls.tsv, instant
  python tools/agent/ghidra_decomp.py --callees 0x16439b0
  python tools/agent/ghidra_decomp.py --export                  # one-time text export (slow)

Cache:  E:\\games\\bbport\\deps\\ghidra-cache\\0x<addr>.c (one per requested address; the
        function containing it, with callers/callees header). All missing addresses of one call
        are decompiled in ONE headless Ghidra run.
Export: E:\\games\\bbport\\deps\\ghidra-export\\{functions,strings,xrefs_to_strings,calls}.tsv
        -- grep these first (see docs/RE_TOOLS.md).
Addresses are our guest offsets (raw ELF VA of eboot.elf; bbhost addresses minus 0x400000).

The Ghidra project is BB_GHIDRA_PROJECT (an agent's copy, from agent_env.py create NAME
--ghidra; read from ../env.txt next to the worktree if the variable is unset). Never point it at
deps\\ghidra-proj itself. Env overrides: GHIDRA_INSTALL_DIR, BB_GHIDRA_CACHE, BB_GHIDRA_EXPORT.
"""
import argparse, os, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "tools" / "re" / "ghidra_scripts"
DEPS = Path(r"E:\games\bbport\deps")
GHIDRA = Path(os.environ.get("GHIDRA_INSTALL_DIR", DEPS / "ghidra" / "ghidra_12.1.4_PUBLIC"))
CACHE = Path(os.environ.get("BB_GHIDRA_CACHE", DEPS / "ghidra-cache"))
EXPORT = Path(os.environ.get("BB_GHIDRA_EXPORT", DEPS / "ghidra-export"))


def norm(a):
    return "0x%x" % int(a.lower().replace("0x", ""), 16)


def project():
    p = os.environ.get("BB_GHIDRA_PROJECT")
    if not p:
        env = REPO.parent / "env.txt"  # agents/<name>/env.txt written by agent_env.py
        if env.exists():
            for line in env.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("BB_GHIDRA_PROJECT="):
                    p = line.split("=", 1)[1].strip()
    if not p:
        sys.exit("BB_GHIDRA_PROJECT not set: create an agent env with "
                 "`python tools/agent/agent_env.py create NAME --ghidra` (never use deps\\ghidra-proj)")
    if Path(p).resolve() == (DEPS / "ghidra-proj").resolve():
        sys.exit("refusing to open the shared deps\\ghidra-proj; use an agent copy (--ghidra)")
    return p


def headless(script, args, timeout):
    cmd = [str(GHIDRA / "support" / "analyzeHeadless.bat"), project(), "bloodborne",
           "-process", "eboot.elf", "-noanalysis", "-readOnly",
           "-scriptPath", str(SCRIPTS), "-postScript", script, *args]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors="replace")
    try:
        out, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        # the .bat spawns java; kill the whole tree
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
        out, _ = p.communicate()
        sys.stderr.write(out[-3000:])
        sys.exit("ghidra timed out after %d s" % timeout)
    sys.stderr.write("[ghidra %s: %.0f s]\n" % (script, time.time() - t0))
    return p.returncode, out


def tsv_rows(name):
    f = EXPORT / name
    if not f.exists():
        sys.exit("%s missing; run --export once" % f)
    with open(f, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line.startswith("#"):
                yield line.rstrip("\n").split("\t")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addresses", nargs="*")
    ap.add_argument("--cat", action="store_true", help="print the decompiled C, not just paths")
    ap.add_argument("--force", action="store_true", help="re-decompile even if cached")
    ap.add_argument("--callers", metavar="ADDR", help="callers of the function at ADDR (calls.tsv)")
    ap.add_argument("--callees", metavar="ADDR", help="callees of the function at ADDR (calls.tsv)")
    ap.add_argument("--export", action="store_true", help="(re)build the text export")
    ap.add_argument("--timeout", type=int, default=0, help="seconds per Ghidra run "
                    "(default 900 decompile, 3600 export)")
    a = ap.parse_args()

    if a.callers or a.callees:
        key, col = (norm(a.callers), 3) if a.callers else (norm(a.callees), 0)
        for r in tsv_rows("calls.tsv"):
            if len(r) >= 6 and r[col] == key:
                print("\t".join(r))
        return
    if a.export:
        EXPORT.mkdir(parents=True, exist_ok=True)
        rc, out = headless("ExportAll.java", [str(EXPORT)], a.timeout or 3600)
        sys.stderr.write("\n".join(l for l in out.splitlines() if "ExportAll.java>" in l) + "\n")
        if not (EXPORT / "calls.tsv").exists():
            sys.stderr.write(out[-3000:])
            sys.exit(1)
        print(EXPORT)
        return
    if not a.addresses:
        ap.error("give addresses, --callers/--callees or --export")

    keys = list(dict.fromkeys(norm(x) for x in a.addresses))
    CACHE.mkdir(parents=True, exist_ok=True)
    missing = [k for k in keys if a.force or not (CACHE / (k + ".c")).exists()]
    if missing:
        rc, out = headless("DecompileToDir.java", [str(CACHE), *missing], a.timeout or 900)
        lost = [k for k in missing if not (CACHE / (k + ".c")).exists()]
        if lost:
            sys.stderr.write(out[-3000:])
            sys.exit("not decompiled: " + " ".join(lost))
    for k in keys:
        path = CACHE / (k + ".c")
        print(path)
        if a.cat:
            print(path.read_text(encoding="utf-8", errors="replace"))


if __name__ == "__main__":
    main()
