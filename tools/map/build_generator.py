#!/usr/bin/env python3
"""Build the release's map_generator folder: Hunter's Atlas's map generator frozen with PyInstaller.

    python tools/map/build_generator.py --python VENV\\Scripts\\python.exe [--out DIR] [--no-sdist]

The venv is a CPython 3.12 with `tools/map/generator/requirements-frozen.txt` and `requirements-build.txt`
installed (pip install -r each; the pins are Hunter's Atlas 3.x's). The result (default `out/map_generator`) is
what the release ships beside scripts\\: generate_atlas_lite.exe plus the libraries it freezes (NumPy, SciPy,
Shapely, Pillow, Soulstruct, ...), with every library's licence and Soulstruct's source archive (GPL-3.0-or-later)
under licenses\\. No game file goes in. scripts/map_prepare.py runs it once on each player's PC.

Why a frozen exe: the release's bundled Python has no site-packages; the generator needs SciPy / NumPy / Shapely.
Freezing notes (from Hunter's Atlas's own spec, tools/map/generator/generate_atlas.spec): SciPy >= 1.16 loads compiled
submodules of scipy.spatial.transform at import (scipy.spatial is collected whole, `--self-test-imports` below
catches a missing one), the duplicate OpenBLAS DLLs PyInstaller's SciPy hook adds are dropped, and `zstandard`
(Soulstruct's DCX reader) is collected with its binaries.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
GEN = HERE / "generator"
REPO = HERE.parents[1]

README = """The map generator for the in-game map.

Hunter's Atlas by MikeTat1988 (github.com/MikeTat1988/HuntersAtlas, commit 06d4021, GPL-2.0-or-later). The launcher's
"Prepare the map" runs generate_atlas_lite.exe once on your PC; it reads your own game files and writes the area
geometry, which scripts\\map_prepare.py packs into user\\map. No game file is part of this folder.

Its source is in the port's repository, tools\\map\\generator\\src (built by tools\\map\\build_generator.py). The
libraries it bundles keep their licences in licenses\\; Soulstruct (GPL-3.0-or-later) is the generator's reader for
the game's archive formats, its exact source distribution is licenses\\soulstruct-*.tar.gz.
"""

LICENSE_NAMES = ("licen", "copying", "notice", "authors", "copyright")


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def collect_licenses(python: Path, dest: Path):
    """Every installed distribution's licence files (dist-info/licenses, LICENSE*, COPYING*, NOTICE*) into
    dest/<name>/ (what the frozen build contains)."""
    code = r"""
import importlib.metadata as m, json
out = []
for d in m.distributions():
    name = d.metadata["Name"]
    files = [str(f) for f in (d.files or []) if any(t in f.name.lower() for t in %r) and 'dist-info' in str(f)]
    out.append([name, d.version, [str(d.locate_file(f)) for f in (d.files or []) if str(f) in files]])
print(json.dumps(out))
""" % (LICENSE_NAMES,)
    res = subprocess.run([str(python), "-I", "-c", code], capture_output=True, text=True, check=True)
    count = 0
    for name, version, files in json.loads(res.stdout):
        for f in files:
            src = Path(f)
            if src.is_file():
                target = dest / f"{name}-{version}" / src.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, target)
                count += 1
    print(f"licence files collected: {count}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--python", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=REPO / "out" / "map_generator")
    ap.add_argument("--no-sdist", action="store_true", help="do not download Soulstruct's source archive")
    a = ap.parse_args()
    work = a.out.parent / "map_generator_build"
    for d in (work, a.out):
        shutil.rmtree(d, ignore_errors=True)
    work.mkdir(parents=True)
    run([a.python, "-m", "PyInstaller", "--noconfirm", "--distpath", work / "dist", "--workpath", work / "build",
         GEN / "generate_atlas.spec"])
    shutil.copytree(work / "dist" / "generate_atlas_lite", a.out)
    exe = a.out / "generate_atlas_lite.exe"
    run([exe, "--self-test-imports"], stdout=subprocess.DEVNULL)  # every import the generator needs resolves
    collect_licenses(a.python, a.out / "licenses")
    shutil.copyfile(GEN / "LICENSE", a.out / "licenses" / "HuntersAtlas-GPL-2.0.txt")
    if not a.no_sdist:
        import re
        text = subprocess.run([str(a.python), "-I", "-m", "pip", "show", "soulstruct"], capture_output=True, text=True).stdout
        version = re.search(r"^Version: (\S+)", text, re.M).group(1)
        meta = json.load(urllib.request.urlopen(f"https://pypi.org/pypi/soulstruct/{version}/json"))
        sdist = next(u for u in meta["urls"] if u["packagetype"] == "sdist")
        target = a.out / "licenses" / sdist["filename"]
        urllib.request.urlretrieve(sdist["url"], target)
        if hashlib.sha256(target.read_bytes()).hexdigest() != sdist["digests"]["sha256"]:
            sys.exit("Soulstruct's source distribution does not match PyPI's hash")
    (a.out / "README.txt").write_text(README, encoding="utf-8", newline="\r\n")
    shutil.rmtree(work, ignore_errors=True)
    print("map generator built:", a.out)


if __name__ == "__main__":
    main()
