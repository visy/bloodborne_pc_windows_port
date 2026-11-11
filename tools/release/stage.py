#!/usr/bin/env python3
r"""Build the release folder and keep the symbols.

    python tools/release/stage.py <ver> <build dir> <stage dir> [--repo R] [--base PREV_STAGE]
                                  [--symbols-dir D] [--allow-dirty] [--replace]

<build dir> holds the release exe (bbport.exe, bb-gpu-capabilities.exe, bbport.exe.debug, and any DLL
the build produced), e.g. <clone>\out\release. The stage is made of:
  * the repo's tracked release files: CHANGES.md LICENSE README.md README.ru.md VERSION launch_gui.bat
    launcher.py run.bat net_doctor.bat, docs\ (no docs\superpowers), scripts\, patches\,
    tools\legacy-cpu-plan.json, licenses\ (git ls-files of --repo's HEAD, so nothing untracked leaks in);
  * from --base (the previous stage): what git does not hold: out\ (DLLs, licenses, bb-probe.exe, DLSS
    files), fsr4_shaders\, python\, map_generator\, mods\, user\ (skeleton; only the two note files kept);
  * the build dir's exes and DLLs on top of out\.
Then: bbport.exe --version must say <ver>; the exe's embedded build id must name the repo's HEAD and not
be -dirty; the stage must pass the zip's refuse rules (make_zip.py). Symbols (bbport.exe, bbport.exe.debug,
bb-gpu-capabilities.exe) go to <symbols dir>\<ver> (default <stage parent>\rel\symbols). A stamp
<stage>.stamp.json records version, full commit and exe sha256 for publish.py.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_common as rc  # noqa: E402
import changelog  # noqa: E402
import changelog  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
# Tracked paths that ship (exact file names and directory prefixes of git ls-files).
TRACKED_FILES = ["CHANGES.md", "LICENSE", "README.md", "README.ru.md", "VERSION", "launch_gui.bat",
                 "launcher.py", "run.bat", "net_doctor.bat", "tools/legacy-cpu-plan.json"]
TRACKED_DIRS = ("docs/", "scripts/", "patches/", "licenses/")
TRACKED_SKIP = ("docs/superpowers/",)
FROM_BASE = ["out", "fsr4_shaders", "python", "map_generator", "mods", "user"]
MINGW = r"C:\msys64\mingw64\bin"  # the exe's DLLs when the build dir has none (out/release holds the exe only)
REQUIRED = ["out/bbport.exe", "out/bb-zstdpack.exe", "out/SDL3.dll", "out/libgcc_s_seh-1.dll", "VERSION", "launcher.py"]


def die(msg):
    raise SystemExit(f"stage: REFUSED: {msg}")


def tracked_files(repo):
    out = []
    for n in rc.git(repo, "ls-files").splitlines():
        if n in TRACKED_FILES or (n.startswith(TRACKED_DIRS) and not n.startswith(TRACKED_SKIP)):
            out.append(n)
    return out


def copy_file(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def check_changes_dated(repo, ver):
    """CHANGES.md as committed (the zip takes it from git) must not call this version (unreleased)."""
    try:
        text = rc.git(repo, "show", "HEAD:CHANGES.md")
    except RuntimeError:
        return
    check_changes_text(text, ver)
    try:
        listing = rc.git(repo, "ls-tree", "-r", "--name-only", "HEAD", "changes.d/")
    except RuntimeError:
        return
    check_fragment_names(listing.splitlines(), ver)


def check_fragment_names(names, ver):
    """Dies when committed changes.d/ fragments are still waiting to be assembled into CHANGES.md."""
    frags = [n.strip() for n in names if n.strip() and changelog.is_fragment_path(n.strip())]
    if frags:
        die(f"changes.d/ still holds {len(frags)} changelog fragment(s) ({', '.join(sorted(frags)[:3])}"
            f"{', ...' if len(frags) > 3 else ''}): run `python tools/release/changelog.py assemble {ver} "
            f"--date YYYY-MM-DD` and commit before the build (the zip takes CHANGES.md from git)")


def check_changes_text(text, ver):
    """Dies when CHANGES.md text calls version `ver` (unreleased) in its heading."""
    for line in text.splitlines():
        m = re.match(r"##\s+" + re.escape(ver) + r"\b(.*)$", line.strip())
        if m and "unreleased" in m.group(1).lower():
            die(f"CHANGES.md still says '{line.strip()}': put the release date in the heading and commit it "
                f"before the build (the zip takes CHANGES.md from git)")


def verify_exe(exe, ver, repo, allow_dirty, run_exe=True):
    """(full commit, build id) the exe was built from; dies when that cannot be proven."""
    if run_exe:
        env = dict(os.environ)
        env["PATH"] = str(exe.parent) + os.pathsep + MINGW + os.pathsep + env.get("PATH", "")
        r = subprocess.run([str(exe), "--version"], capture_output=True, text=True, timeout=60, env=env)
        if f"bbport {ver} " not in r.stdout:
            die(f"{exe} --version exit code {r.returncode} (0x{r.returncode & 0xFFFFFFFF:08X}), stdout "
                f"{r.stdout.strip()!r}, stderr {r.stderr.strip()!r}, not {ver} "
                f"(a missing DLL exits silently: is {MINGW} there?)")
    ident = rc.exe_build_id(exe, ver)
    if not ident:
        die(f"no embedded build id for {ver} in {exe}")
    bid = ident[1]
    if ("dirty" in bid or bid == "nogit") and not allow_dirty:
        die(f"exe build id {bid!r} is dirty / has no git id (build from a clean checkout, or --allow-dirty)")
    m = re.search(r"(?:-g|^)([0-9a-f]{7,40})(?:-dirty)?$", bid)
    if not m:
        die(f"cannot read a commit from build id {bid!r}")
    head = rc.git(repo, "rev-parse", "HEAD")
    if not head.startswith(m.group(1)):
        die(f"exe was built from {m.group(1)}, repo HEAD is {head[:10]}: rebuild, or stage from that commit")
    return head, bid


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version")
    ap.add_argument("build_dir")
    ap.add_argument("stage_dir")
    ap.add_argument("--repo", default=str(REPO))
    ap.add_argument("--base", help="previous stage: source of the files git does not hold")
    ap.add_argument("--symbols-dir")
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument("--replace", action="store_true", help="delete an existing stage dir first")
    ap.add_argument("--no-run", action="store_true", help="do not run the exe (--version check skipped)")
    ap.add_argument("--bin-mb", type=int, default=rc.DEFAULT_BIN_MB)
    ap.add_argument("--allow", action="append", default=[], help="glob of a .bin allowed over the size limit")
    a = ap.parse_args(argv)
    ver, repo = a.version, Path(a.repo)
    build, stage = Path(a.build_dir), Path(a.stage_dir)
    if not re.fullmatch(r"\d+(\.\d+){1,3}", ver):
        die(f"bad version {ver!r}")
    have = (repo / "VERSION").read_text(encoding="utf-8").strip()
    if have != ver:
        die(f"VERSION file says {have}, not {ver}")
    check_changes_dated(repo, ver)
    exe = build / "bbport.exe"
    if not exe.is_file():
        die(f"no {exe}")
    head, bid = verify_exe(exe, ver, repo, a.allow_dirty, not a.no_run)
    if stage.exists():
        if not a.replace:
            die(f"{stage} exists (use --replace)")
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    if a.base:
        base = Path(a.base)
        for top in FROM_BASE:
            if (base / top).is_dir():
                shutil.copytree(base / top, stage / top, ignore=shutil.ignore_patterns("__pycache__"))
    # user\ is the skeleton only: drop anything the base had besides the two notes.
    user = stage / "user"
    if user.is_dir():
        for p in list(user.rglob("*")):
            if p.is_file() and p.relative_to(stage).as_posix() not in rc.ALLOWED_USER_FILES:
                p.unlink()
    names = tracked_files(repo)
    for name in names:
        copy_file(repo / name, stage / name)
    out = stage / "out"
    out.mkdir(exist_ok=True)
    for f in sorted(build.iterdir()):
        if f.is_file() and f.suffix.lower() in (".exe", ".dll"):
            copy_file(f, out / f.name)
    sym_root = Path(a.symbols_dir) if a.symbols_dir else stage.parent / "rel" / "symbols"
    sym = sym_root / ver
    sym.mkdir(parents=True, exist_ok=True)
    for name in ("bbport.exe", "bbport.exe.debug", "bb-gpu-capabilities.exe"):
        if (build / name).is_file():
            shutil.copy2(build / name, sym / name)
    if not (sym / "bbport.exe.debug").is_file():
        print("stage: warning: no bbport.exe.debug in the build dir (BB_STRIP off?); symbols not kept")
    missing = [r for r in REQUIRED if not (stage / r).exists()]
    if missing:
        die(f"stage lacks {missing} (give --base with the previous stage)")
    problems = rc.check_stage(stage, a.bin_mb, a.allow)
    if problems:
        print("\n".join("  " + p for p in problems))
        die(f"{len(problems)} problem(s) in the stage")
    exe_sha = rc.sha256(out / "bbport.exe")
    stamp = {"version": ver, "commit": head, "build_id": bid, "exe_sha256": exe_sha}
    stamp_path = Path(str(stage) + ".stamp.json")
    stamp_path.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    files = sum(1 for p in stage.rglob("*") if p.is_file())
    print(f"staged {stage}: {files} files ({len(names)} from git), version {ver}, commit {head[:10]}, build id {bid}")
    print(f"exe sha256 {exe_sha}")
    print(f"symbols {sym}")
    print(f"stamp {stamp_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
