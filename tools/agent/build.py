#!/usr/bin/env python3
"""Build through the machine-wide build queue, at low CPU priority, so parallel agents' builds
never starve the running test games.

  python tools/agent/build.py [BUILD_DIR] [--target T ...] [-j N]

BUILD_DIR defaults to out/gpu of the repo this script is in. At most 3 builds run at once across
all agents (tools/mp/game_queue.py pool "build"; edit build_queue.json max_clients to change); a
build runs with -j 8 by default (BB_BUILD_JOBS) at BELOW_NORMAL priority, which its compiler and
linker children inherit. TMP/TEMP go to <repo>/../tmp when unset (keeps C: free).
"""
import argparse, os, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "tools" / "mp"))
from game_queue import game_slots  # noqa: E402

MINGW = r"C:\msys64\mingw64\bin"
BELOW_NORMAL = 0x00004000
BBPORT = Path(r"E:\games\bbport")  # every checkout and worktree lives under it


def apply_fsr_patches(env):
    """gpu/patches/fsr-vulkan/*.patch onto the FSR-Vulkan submodule when not applied yet (as
    build.bat / build.sh do; agent worktrees hold a plain copy of the submodule, without .git).
    GIT_CEILING_DIRECTORIES keeps git from taking the superproject for the patch's repository."""
    sub = REPO / "gpu" / "third_party" / "fsr-vulkan"
    patches = sorted((REPO / "gpu" / "patches" / "fsr-vulkan").glob("*.patch"))
    if not (sub / "CMakeLists.txt").is_file():
        return
    genv = dict(env, GIT_CEILING_DIRECTORIES=str(sub.parent))
    for patch in patches:
        def git_apply(*args):
            return subprocess.run(["git", "apply", *args, str(patch)], cwd=sub, env=genv,
                                  capture_output=True).returncode == 0
        if git_apply("--reverse", "--check"):
            continue  # applied already
        if git_apply("--check") and git_apply():
            print(f"FSR-Vulkan: applied {patch.name}", flush=True)
        else:
            print(f"FSR-Vulkan: {patch.name} does not apply (local changes?)", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build_dir", nargs="?", default=str(REPO / "out" / "gpu"))
    ap.add_argument("--target", action="append", default=[])
    ap.add_argument("-j", type=int, default=int(os.environ.get("BB_BUILD_JOBS", "6")))
    a = ap.parse_args()
    env = os.environ.copy()
    env["PATH"] = MINGW + os.pathsep + env.get("PATH", "")
    # GCC's lto-wrapper runs -flto=auto's LTRANS jobs in parallel through `make` (MAKE); mingw64
    # only has mingw32-make, so without this the release link compiled its 174 jobs one by one.
    env.setdefault("MAKE", os.path.join(MINGW, "mingw32-make.exe"))
    if not env.get("BB_KEEP_TMP"):
        tmp = BBPORT / "tmp"
        tmp.mkdir(exist_ok=True)
        env["TMP"] = env["TEMP"] = str(tmp)
    # One compile cache for every worktree: keys use paths relative to the agents' root.
    env.setdefault("CCACHE_BASEDIR", str(BBPORT))
    env.setdefault("CCACHE_DIR", str(BBPORT / "ccache"))
    env.setdefault("CCACHE_MAXSIZE", "40G")
    env.setdefault("CCACHE_NOHASHDIR", "1")
    apply_fsr_patches(env)
    cmd = ["cmake", "--build", a.build_dir, "-j", str(a.j)]
    for t in a.target:
        cmd += ["--target", t]
    flags = BELOW_NORMAL if os.name == "nt" else 0
    with game_slots(1, a.build_dir, pool="build"):
        r = subprocess.run(cmd, env=env, creationflags=flags)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
