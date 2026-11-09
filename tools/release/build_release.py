#!/usr/bin/env python3
r"""One command from a committed release commit to a checked zip and the publish command.

    python tools/release/build_release.py X.Y.Z --base E:\games\bbport\rel<previous>
           [--commit SHA] [--src E:\games\bbport\src] [--work DIR] [--out-root E:\games\bbport]
           [--title T] [--notes FILE] [--jobs N] [--dry-run]

Steps, each skipped when its output exists and matches (so a stopped run resumes):
  1. clone   <src> at --commit (default: master) into --work (default <out-root>\agents\relclone<XYZ>)
             with `git clone --no-tags`; tags are then fetched from the fork remote only (the upstream
             Mrsuss60 tags, e.g. a v0.8.0 at another commit, must not reach the release clone).
             Refuses a src whose tracked files are modified (submodules ignored).
  2. checks  VERSION is X.Y.Z; CHANGES.md at that commit has `## X.Y.Z (date)`, not `(unreleased)`.
  3. configure  tools/release/configure_pgo.py (submodules, libatrac9, FSR patches, committed PGO profile).
  4. build   cmake --build <work>\repo\out\gpu-pgo-gen with mingw64 on PATH, below-normal priority, through
             the machine-wide build queue (tools/mp/game_queue.py pool "build", as tools/agent/build.py).
  5. stage   stage.py X.Y.Z <work>\repo\out\release <out-root>\relXYZ --base BASE  (build id must be this commit,
             not -dirty; submodule changes from the FSR patches do not count).
  6. zip     make_zip.py -> <out-root>\bloodborne_pc_windows_port_vX.Y.Z.zip (+ .stamp.json).
  7. notes + publish.py DRY RUN; prints the exact --yes-publish command. This tool never publishes.

--dry-run prints the plan and the state of each step and changes nothing.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "mp"))
import release_common as rc  # noqa: E402
import stage as stage_mod  # noqa: E402

MAIN_SRC = Path(r"E:\games\bbport\src")
OUT_ROOT = Path(r"E:\games\bbport")
MINGW = r"C:\msys64\mingw64\bin"
BUILD_NAME = "gpu-pgo-gen"
GITHUB_REPO = "visy/bloodborne_pc_windows_port"
BELOW_NORMAL = 0x00004000


def die(msg):
    raise SystemExit(f"build_release: {msg}")


def say(msg):
    print(f"[build_release] {msg}", flush=True)


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    if check and r.returncode:
        die(f"git {' '.join(args)} in {repo}: {r.stderr.strip()}")
    return r


def tracked_dirty(repo):
    """True when a tracked file differs from HEAD (submodule working trees are ignored)."""
    git(repo, "update-index", "-q", "--refresh", check=False)
    return git(repo, "diff-index", "--quiet", "--ignore-submodules=all", "HEAD", "--", check=False).returncode != 0


def digits(ver):
    return ver.replace(".", "")


def notes_from_changes(text, ver):
    """The body of `## ver (...)` in CHANGES.md text (up to the next `## `), stripped; '' when absent."""
    out, on = [], False
    for line in text.splitlines():
        if re.match(r"##\s", line):
            if on:
                break
            on = re.match(r"##\s+" + re.escape(ver) + r"\b", line) is not None
            continue
        if on:
            out.append(line)
    return "\n".join(out).strip() + "\n" if any(s.strip() for s in out) else ""


def stamp_ok(stamp_path, commit, exe=None):
    """A stamp file exists and names this commit (and, when given, this exe's sha256)."""
    p = Path(stamp_path)
    if not p.is_file():
        return False
    try:
        st = rc.read_stamp(p)
    except ValueError:
        return False
    return st.get("commit") == commit and (exe is None or st.get("exe_sha256") == rc.sha256(exe))


def exe_matches(exe, ver, commit):
    """The built exe names this commit and is not dirty."""
    if not Path(exe).is_file():
        return False
    ident = rc.exe_build_id(exe, ver)
    if not ident or "dirty" in ident[1] or ident[1] == "nogit":
        return False
    m = re.search(r"(?:-g|^)([0-9a-f]{7,40})$", ident[1])
    return bool(m) and commit.startswith(m.group(1))


def build_env(jobs):
    env = os.environ.copy()
    env["PATH"] = MINGW + os.pathsep + env.get("PATH", "")
    env.setdefault("MAKE", os.path.join(MINGW, "mingw32-make.exe"))  # lto-wrapper's parallel LTRANS
    tmp = OUT_ROOT / "tmp"
    if not env.get("BB_KEEP_TMP") and tmp.parent.is_dir():
        tmp.mkdir(exist_ok=True)
        env["TMP"] = env["TEMP"] = str(tmp)
    return env


def run(cmd, **kw):
    say("run: " + " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        die(f"{Path(str(cmd[1] if len(cmd) > 1 else cmd[0])).name} failed with exit code {r.returncode}")


def step_clone(a, repo, commit, dry):
    if (repo / ".git").exists():
        head = git(repo, "rev-parse", "HEAD").stdout.strip()
        if head != commit:
            die(f"{repo} exists at {head[:10]}, not {commit[:10]}: delete it, or pass another --work")
        if tracked_dirty(repo):
            die(f"{repo} has changes to tracked files: delete it, or pass another --work")
        say(f"clone: {repo} already at {commit[:10]}")
        return
    say(f"clone: {a.src} at {commit[:10]} -> {repo} (no tags from the source)")
    if dry:
        return
    repo.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--no-tags", "-q", a.src, repo])
    git(repo, "checkout", "-q", "--detach", commit)
    fork = git(a.src, "remote", "get-url", "fork", check=False).stdout.strip()
    if fork:
        r = git(repo, "fetch", "-q", "--no-tags", fork, "+refs/tags/*:refs/tags/*", check=False)
        if r.returncode:
            say(f"clone: warning: could not fetch the fork's tags ({r.stderr.strip()}); publish.py checks tags on GitHub")
    else:
        say("clone: warning: src has no 'fork' remote; the clone has no tags")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version")
    ap.add_argument("--base", required=True, help="previous stage dir (source of DLLs, python, mods, ...)")
    ap.add_argument("--commit", help="commit to release (default: master of --src)")
    ap.add_argument("--src", default=str(MAIN_SRC))
    ap.add_argument("--out-root", default=str(OUT_ROOT), help="holds relXYZ and the zip")
    ap.add_argument("--work", help="clone folder (default <out-root>\\agents\\relclone<XYZ>)")
    ap.add_argument("--title")
    ap.add_argument("--notes", help="release notes file (default: the CHANGES.md section, written next to the zip)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("BB_BUILD_JOBS", "6")))
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    ver = a.version
    if not re.fullmatch(r"\d+(\.\d+){1,3}", ver):
        die(f"bad version {ver!r}")
    out_root = Path(a.out_root)
    work = Path(a.work) if a.work else out_root / "agents" / f"relclone{digits(ver)}"
    repo = work / "repo"
    stage_dir = out_root / f"rel{digits(ver)}"
    zip_path = out_root / f"bloodborne_pc_windows_port_v{ver}.zip"
    base = Path(a.base)
    if not base.is_dir():
        die(f"--base {base} is not a folder")

    if tracked_dirty(a.src):
        die(f"{a.src} has uncommitted changes to tracked files: commit or revert them first")
    commit = git(a.src, "rev-parse", a.commit or "master").stdout.strip()
    if not rc.is_full_sha(commit):
        die(f"cannot resolve {a.commit or 'master'} in {a.src}")
    say(f"version {ver}, commit {commit}, work {work}, stage {stage_dir}, zip {zip_path}")

    # Checks that need only git, before any clone or build time is spent.
    have = git(a.src, "show", f"{commit}:VERSION").stdout.strip()
    if have != ver:
        die(f"VERSION at {commit[:10]} says {have}, not {ver}")
    changes = git(a.src, "show", f"{commit}:CHANGES.md").stdout
    try:
        stage_mod.check_changes_text(changes, ver)
    except SystemExit as e:
        raise SystemExit(str(e).replace("stage: REFUSED", "build_release: REFUSED")) from None
    if not notes_from_changes(changes, ver):
        die(f"CHANGES.md has no '## {ver}' section with text")

    step_clone(a, repo, commit, a.dry_run)

    exe = repo / "out" / "release" / "bbport.exe"
    build = repo / "out" / BUILD_NAME
    built = exe_matches(exe, ver, commit)
    if built:
        say("build: out/release/bbport.exe already names this commit, skipped")
    else:
        if (build / "build.ninja").is_file() and (build / "CMakeCache.txt").is_file():
            say("configure: build dir exists, skipped")
        else:
            say("configure: configure_pgo.py")
            if not a.dry_run:
                run([sys.executable, HERE / "configure_pgo.py", work])
        say("build: cmake --build (below normal priority, machine-wide build queue)")
        if not a.dry_run:
            from game_queue import game_slots
            env = build_env(a.jobs)
            with game_slots(1, str(build), pool="build", label="build_release"):
                run(["cmake", "--build", build, "-j", a.jobs], env=env,
                    creationflags=BELOW_NORMAL if os.name == "nt" else 0)
            if not exe_matches(exe, ver, commit):
                ident = rc.exe_build_id(exe, ver) if exe.is_file() else None
                die(f"the build finished but {exe} names {ident} (wanted a clean build of {commit[:10]})")

    stamp = Path(str(stage_dir) + ".stamp.json")
    if exe.is_file() and stamp_ok(stamp, commit, stage_dir / "out" / "bbport.exe") and \
            rc.sha256(exe) == rc.sha256(stage_dir / "out" / "bbport.exe"):
        say(f"stage: {stage_dir} already staged from this exe, skipped")
    else:
        if stage_dir.exists() and not stamp.is_file():
            die(f"{stage_dir} exists without a stamp (not made by stage.py): move it away")
        say("stage: stage.py")
        if not a.dry_run:
            run([sys.executable, HERE / "stage.py", ver, exe.parent, stage_dir, "--repo", repo,
                 "--base", base, "--replace"])

    zstamp = Path(str(zip_path) + ".stamp.json")
    if zip_path.is_file() and stamp_ok(zstamp, commit) and stamp_ok(stamp, commit, stage_dir / "out" / "bbport.exe") \
            and rc.read_stamp(zstamp).get("exe_sha256") == rc.read_stamp(stamp)["exe_sha256"]:
        say(f"zip: {zip_path} already made from this stage, skipped")
    else:
        if zip_path.exists():
            die(f"{zip_path} exists but does not match this commit's stage: move it away")
        say("zip: make_zip.py")
        if not a.dry_run:
            run([sys.executable, HERE / "make_zip.py", stage_dir, zip_path])

    notes = Path(a.notes) if a.notes else out_root / f"notes_v{ver}.md"
    if not a.notes and not a.dry_run:
        notes.write_text(notes_from_changes(changes, ver), encoding="utf-8")
        say(f"notes: {notes} (CHANGES.md section)")
    title = a.title or f"Bloodborne PC port {ver}"
    pub = [sys.executable, HERE / "publish.py", "--repo", GITHUB_REPO, "--tag", f"v{ver}", "--commit", commit,
           "--title", title, "--notes", notes, "--zip", zip_path, "--git-repo", repo]
    if a.dry_run:
        say("dry run: nothing changed. The publish dry run would be:")
        print(" ".join(f'"{c}"' if " " in str(c) else str(c) for c in pub))
        return 0
    say("publish.py dry run")
    run(pub)
    shown = " ".join(f'"{c}"' if " " in str(c) else str(c) for c in pub[1:])
    print("\nNOT published. To publish (after the smoke, merge and push of the checklist):")
    print(f"  python {shown} --yes-publish")
    return 0


if __name__ == "__main__":
    sys.exit(main())
