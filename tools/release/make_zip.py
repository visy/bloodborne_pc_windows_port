#!/usr/bin/env python3
r"""Write the release zip from a stage folder, after refusing anything that must not ship.

    python tools/release/make_zip.py <stage dir> <out zip> [--bin-mb N] [--allow GLOB ...]

REFUSES (nothing is written) when the stage holds: logs (*.log), *.ini, saves (userdata / savedata),
player_logs, .env files, keys / certificates; anything outside the top-level allowlist (see
release_common.py); game files (eboot.bin, *.elf, dvdroot, *.dcx / param / msg / map dumps, *.bin over
N MB unless --allow'ed); debug symbols; files under user\ other than readme.txt and
custom_trophy\Notice.txt; symlinks. Also refuses an existing output zip.

Writes directory entries (as earlier releases had), runs testzip, prints size, file count, the zip's
sha256 and the exe's sha256, and (when <stage>.stamp.json from stage.py exists) writes
<zip>.stamp.json = the stamp plus the zip's size and sha256, which publish.py checks.
"""
import argparse
import json
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_common as rc  # noqa: E402


def die(msg):
    raise SystemExit(f"make_zip: REFUSED: {msg}")


def build_zip(stage, out, bin_mb=rc.DEFAULT_BIN_MB, allow=()):
    """Returns a dict of the facts (size, files, zip_sha256, exe_sha256)."""
    stage, out = Path(stage), Path(out)
    problems = rc.check_stage(stage, bin_mb, allow)
    if problems:
        die(f"{len(problems)} problem(s) in the stage:\n  " + "\n  ".join(problems))
    exe = stage / "out" / "bbport.exe"
    if not exe.is_file():
        die("stage has no out/bbport.exe")
    if out.exists():
        die(f"{out} exists")
    stamp_path = Path(str(stage) + ".stamp.json")
    stamp = rc.read_stamp(stamp_path) if stamp_path.is_file() else None
    exe_sha = rc.sha256(exe)
    if stamp and stamp.get("exe_sha256") != exe_sha:
        die("out/bbport.exe changed since stage.py stamped it (re-stage, or delete the stamp)")
    nfiles = 0
    try:
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for rel, path, is_dir in rc.walk(stage):
                if is_dir:
                    z.write(path, rel + "/")
                else:
                    z.write(path, rel)
                    nfiles += 1
        with zipfile.ZipFile(out) as z:
            bad = z.testzip()
            names = z.namelist()
        if bad:
            die(f"testzip failed at {bad}")
        if sum(1 for n in names if not n.endswith("/")) != nfiles:
            die("zip file count differs from the stage")
    except BaseException:
        out.unlink(missing_ok=True)
        raise
    facts = {"size": out.stat().st_size, "files": nfiles, "entries": len(names),
             "zip_sha256": rc.sha256(out), "exe_sha256": exe_sha}
    if stamp:
        full = dict(stamp, zip_name=out.name, zip_size=facts["size"], zip_sha256=facts["zip_sha256"])
        Path(str(out) + ".stamp.json").write_text(json.dumps(full, indent=2) + "\n", encoding="utf-8")
    return facts


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stage_dir")
    ap.add_argument("out_zip")
    ap.add_argument("--bin-mb", type=int, default=rc.DEFAULT_BIN_MB)
    ap.add_argument("--allow", action="append", default=[], help="glob of a .bin allowed over the size limit")
    a = ap.parse_args(argv)
    f = build_zip(a.stage_dir, a.out_zip, a.bin_mb, a.allow)
    print("testzip: clean")
    print(f"size:       {f['size']:,} bytes")
    print(f"files:      {f['files']} (+ directory entries, {f['entries']} entries)")
    print(f"zip sha256: {f['zip_sha256']}")
    print(f"exe sha256: {f['exe_sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
