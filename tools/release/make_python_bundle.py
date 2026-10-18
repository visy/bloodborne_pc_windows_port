#!/usr/bin/env python3
"""Build the release's python\\ folder: a trimmed CPython 3.12 x64 with tkinter.

The launcher (a Tk app) and run.bat's scripts (prepare.py, patches.py, mods.py, ...) run on it,
so a player needs no installed Python. python.org's embeddable zip has no tkinter, so this
copies from a full python.org install instead (default C:\\python3).

    python tools/release/make_python_bundle.py --out <release>\\python [--src C:\\python3]

The stdlib is precompiled to unchecked-hash .pyc files (optimization 0, next to the sources in
__pycache__): they are used without looking at file times, so the interpreter never needs to
write into the bundle, and the output does not depend on the build machine's clock.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT_FILES = ["python.exe", "pythonw.exe", "python3.dll", "python312.dll",
              "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt"]

# Lib\ subtrees left out (paths relative to Lib, forward slashes).
LIB_SKIP_DIRS = {"test", "idlelib", "ensurepip", "lib2to3", "pydoc_data", "turtledemo",
                 "tkinter/test", "unittest/test", "site-packages"}
LIB_SKIP_NAMES = {"__pycache__", "tests"}  # anywhere in Lib

# DLLs\: everything but the CPython test modules and icons / the signing catalog.
DLL_SKIP_PREFIX = ("_test", "_ctypes_test")
DLL_SKIP_EXT = {".ico", ".cat"}

# tcl\: runtime scripts only (no link libraries, nmake files, build config scripts).
TCL_SKIP_EXT = {".lib", ".sh"}
TCL_SKIP_DIRS = {"nmake"}

README = """\
Bundled Python for the Bloodborne PC Windows port
=================================================

This folder is CPython {version} (64-bit, python.org build) with Tcl/Tk 8.6, trimmed
for the port: launch_gui.bat runs the launcher (launcher.py, a Tk app) on pythonw.exe, and
run.bat runs the patch and setup scripts (scripts\\*.py) on python.exe. No installed Python
is needed. The standard library is precompiled; tests, IDLE, pip and site-packages are left out.

CPython is under the PSF License (LICENSE.txt in this folder). Tcl/Tk is under a BSD-style
license (tcl\\tcl8.6\\license.terms, tcl\\tk8.6\\license.terms). LICENSE.txt also lists the
licenses of the other libraries CPython includes (OpenSSL, libffi, SQLite, zlib, ...).

Made by tools/release/make_python_bundle.py from the port's source repository.
"""


def check_install(src: Path) -> str:
    exe = src / "python.exe"
    if not exe.is_file():
        sys.exit(f"[ERROR] {exe} not found: --src must be a full python.org CPython install")
    out = subprocess.run([str(exe), "-I", "-c",
                          "import sys, struct, tkinter; v = sys.version_info; "
                          "print(f'{v.major}.{v.minor}.{v.micro}', struct.calcsize('P') * 8, "
                          "tkinter.TkVersion)"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"[ERROR] {exe} failed to report its version / import tkinter:\n{out.stderr}")
    version, bits, tk = out.stdout.split()
    if not version.startswith("3.12.") or bits != "64":
        sys.exit(f"[ERROR] {exe} is Python {version} {bits}-bit; the bundle needs 3.12.x x64")
    for name in ROOT_FILES:
        if not (src / name).is_file():
            sys.exit(f"[ERROR] {src / name} missing")
    print(f"Source: {src} (Python {version}, {bits}-bit, Tk {tk})")
    return version


def copy_file(s: Path, d: Path) -> None:
    d.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(s, d)  # contents only: no source timestamps or attributes


def copy_tree(src: Path, dst: Path, keep) -> None:
    for dirpath, dirnames, filenames in os.walk(src):
        rel = Path(dirpath).relative_to(src)
        dirnames[:] = sorted(d for d in dirnames if keep(rel / d, True))
        for f in sorted(filenames):
            if keep(rel / f, False):
                copy_file(Path(dirpath) / f, dst / rel / f)


def keep_lib(rel: Path, is_dir: bool) -> bool:
    if not is_dir:
        return rel.suffix not in {".pyc", ".pyo"}
    return rel.name not in LIB_SKIP_NAMES and rel.as_posix() not in LIB_SKIP_DIRS


def keep_dlls(rel: Path, is_dir: bool) -> bool:
    if is_dir:
        return False
    return not rel.name.lower().startswith(DLL_SKIP_PREFIX) and rel.suffix.lower() not in DLL_SKIP_EXT


def keep_tcl(rel: Path, is_dir: bool) -> bool:
    if is_dir:
        return rel.name not in TCL_SKIP_DIRS
    return rel.suffix.lower() not in TCL_SKIP_EXT


def tree_size(p: Path) -> tuple:
    files = [f for f in p.rglob("*") if f.is_file()]
    return len(files), sum(f.stat().st_size for f in files)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--src", type=Path, default=Path(r"C:\python3"), help="full CPython 3.12 x64 install")
    ap.add_argument("--out", type=Path, required=True, help="the release's python folder (replaced)")
    args = ap.parse_args()
    src, out = args.src.resolve(), args.out.resolve()
    version = check_install(src)
    if out.exists():
        if not (out / "python.exe").exists() and any(out.iterdir()):
            sys.exit(f"[ERROR] {out} exists and is not a python bundle; not replacing it")
        shutil.rmtree(out)
    out.mkdir(parents=True)

    for name in ROOT_FILES:
        copy_file(src / name, out / name)
    copy_tree(src / "DLLs", out / "DLLs", keep_dlls)
    copy_tree(src / "tcl", out / "tcl", keep_tcl)
    copy_tree(src / "Lib", out / "Lib", keep_lib)
    (out / "Lib" / "site-packages").mkdir(exist_ok=True)
    (out / "README.txt").write_text(README.format(version=version), encoding="utf-8", newline="\r\n")

    # Compile with the bundle's own interpreter so the .pyc magic matches it exactly; -B so its own
    # startup imports do not leave timestamp-based .pyc files behind (force rewrites everything).
    ok = subprocess.run([str(out / "python.exe"), "-I", "-B", "-c",
                         "import compileall, py_compile, sys; "
                         "sys.exit(0 if compileall.compile_dir(sys.argv[1], quiet=1, workers=1, force=True, "
                         "invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH, "
                         "optimize=0, stripdir=sys.argv[1], prependdir='') else 1)",
                         str(out / "Lib")])
    if ok.returncode != 0:
        # Only lib2to3-style grammar files may fail; anything else in the kept stdlib is a bug.
        sys.exit("[ERROR] compileall reported errors")

    n, size = tree_size(out)
    print(f"Bundle: {out}")
    print(f"  {n} files, {size / 1e6:.1f} MB ({size} bytes)")


if __name__ == "__main__":
    main()
