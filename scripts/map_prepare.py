#!/usr/bin/env python3
"""Make the in-game map's data from the player's own game files (once; the launcher's "Prepare the map").

    python scripts/map_prepare.py --game-dir GAME [--out-dir USER/map] [--generator DIR_OR_EXE] [--force]
    python scripts/map_prepare.py --game-dir GAME --check

Hunter's Atlas by MikeTat1988 (github.com/MikeTat1988/HuntersAtlas, GPL-2.0-or-later) reads the game's collision,
map and menu files and writes the area geometry as JSON (its generator, tools/map/generator/src, run frozen as
map_generator\\generate_atlas_lite.exe in a release, or by a Python with its libraries in development: --python
or BB_MAP_PYTHON). scripts/bbmap_bake.py repacks that into compact <area>.bbm files, which the game's map
(gpu/shim/map) reads. Nothing made from the game's files is part of the release or the repository; the data is
written to <user>\\map (BB_MAP_DIR overrides) and rebuilt only when the game's executable changes or --force.

The game folder is only read. Output lines are JSON ({"progress": 0..100, "stage": ...}, the generator's own
progress, then {"stage": "done"}) for the launcher; the exit code is 0 on success, 1 on failure (the reason is the
last "error" line), 3 when the map data is already current (--check: 0 current, 3 missing or outdated).
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bbmap_bake  # noqa: E402

GENERATOR_EXE = "generate_atlas_lite.exe"


def emit(**kw):
    print(json.dumps(kw, ensure_ascii=True), flush=True)


def default_out_dir():
    if os.environ.get("BB_MAP_DIR"):
        return Path(os.environ["BB_MAP_DIR"])
    user = os.environ.get("BB_USER_DIR")
    if user:
        return Path(user) / "map"
    return Path(os.environ.get("BB_DATA_DIR", str(HERE.parent))) / "user" / "map"


def game_root(game):
    game = Path(game)
    if not (game / "dvdroot_ps4").is_dir() and (game / "Image0" / "dvdroot_ps4").is_dir():
        game = game / "Image0"
    return game


def eboot_sha(game):
    p = game / "eboot.bin"
    if not p.is_file():
        return ""
    h = hashlib.sha256()
    with p.open("rb") as f:
        while block := f.read(4 << 20):
            h.update(block)
    return h.hexdigest()


def is_current(out_dir, game):
    """The map data in out_dir was made from this game's executable and by this format."""
    try:
        stamp = json.loads((out_dir / "map.stamp").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    if stamp.get("format") != bbmap_bake.FORMAT_VERSION or not stamp.get("areas"):
        return False
    if len(list(out_dir.glob("*.bbm"))) < stamp["areas"]:
        return False
    return bool(stamp.get("eboot_sha256")) and stamp["eboot_sha256"] == eboot_sha(game)


def find_generator(arg):
    """('exe', path) for a frozen generator, ('py', script) for the source (needs --python), or None."""
    cands = []
    if arg:
        cands.append(Path(arg))
    if os.environ.get("BB_MAP_GENERATOR"):
        cands.append(Path(os.environ["BB_MAP_GENERATOR"]))
    cands += [HERE.parent / "map_generator", HERE.parent / "map_generator" / "generate_atlas_lite"]
    for c in cands:
        exe = c if c.suffix.lower() == ".exe" else c / GENERATOR_EXE
        if exe.is_file():
            return "exe", exe
    src = HERE.parent / "tools" / "map" / "generator" / "src" / "generate_atlas_lite.py"
    if src.is_file():
        return "py", src
    return None


def run_generator(kind, path, python, game, gen_out):
    if kind == "exe":
        cmd = [str(path), "--game-dir", str(game), "--output-dir", str(gen_out)]
    else:
        if not python:
            raise RuntimeError("no map generator: build tools/map/build_generator.py or pass --python (a Python "
                               "with tools/map/generator/requirements-runtime.txt installed)")
        src = str(path.parent)
        # -I keeps the working directory and the environment's PYTHONPATH out; the generator's own folder is added.
        code = ("import sys, runpy; sys.path.insert(0, %r); sys.argv = ['generate_atlas_lite', '--game-dir', %r, "
                "'--output-dir', %r]; runpy.run_path(%r, run_name='__main__')"
                % (src, str(game), str(gen_out), str(path)))
        cmd = [str(python), "-I", "-c", code]
    env = dict(os.environ, PYTHONUTF8="1")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", env=env, cwd=str(gen_out.parent))
    last_error = ""
    tail = []
    for line in proc.stdout:
        line = line.rstrip()
        try:
            msg = json.loads(line)
        except ValueError:
            tail.append(line)
            tail = tail[-12:]
            continue
        if isinstance(msg, dict):
            if "progress" in msg:
                emit(progress=msg["progress"], stage=msg.get("stage", ""))
            elif msg.get("stage") == "error":
                last_error = str(msg.get("message", ""))
    code = proc.wait()
    if code:
        raise RuntimeError(last_error or "the map generator failed (%d): %s" % (code, " | ".join(tail[-4:])))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", type=Path, default=Path(os.environ.get("BB_GAME_DIR", "../CUSA03173")))
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--generator", help="the frozen generator's folder or exe (default: map_generator next to scripts)")
    ap.add_argument("--python", default=os.environ.get("BB_MAP_PYTHON"),
                    help="development: a Python with the generator's libraries, runs tools/map/generator/src")
    ap.add_argument("--force", action="store_true", help="rebuild even when the data is current")
    ap.add_argument("--check", action="store_true", help="only report whether the data is current")
    a = ap.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    game = game_root(a.game_dir)
    out_dir = (a.out_dir or default_out_dir()).resolve()
    current = is_current(out_dir, game)
    if a.check:
        emit(stage="check", current=current, out_dir=str(out_dir))
        return 0 if current else 3
    if current and not a.force:
        emit(stage="done", current=True, out_dir=str(out_dir))
        return 3
    found = find_generator(a.generator)
    if not found:
        emit(stage="error", message="the map generator is not installed (map_generator folder missing)")
        return 1
    kind, path = found
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="map_build_", dir=str(out_dir.parent)))
    try:
        gen_out, baked = work / "gen", work / "baked"
        gen_out.mkdir()
        t0 = time.monotonic()
        emit(progress=0, stage="Starting the map generator")
        run_generator(kind, path, a.python, game, gen_out)
        emit(progress=99, stage="Packing the maps")
        bbmap_bake.bake(gen_out, baked)
        # Swap in: the data files first, the stamp last (its presence means the set is complete).
        out_dir.mkdir(parents=True, exist_ok=True)
        try:
            (out_dir / "map.stamp").unlink()
        except FileNotFoundError:
            pass
        for old in out_dir.glob("*.bbm"):
            old.unlink()
        for f in sorted(baked.glob("*.bbm")):
            os.replace(f, out_dir / f.name)
        os.replace(baked / "map.stamp", out_dir / "map.stamp")
        emit(progress=100, stage="done", seconds=round(time.monotonic() - t0, 1), out_dir=str(out_dir))
        return 0
    except Exception as error:  # noqa: BLE001 - reported to the launcher
        emit(stage="error", message=str(error))
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
