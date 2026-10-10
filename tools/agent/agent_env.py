#!/usr/bin/env python3
"""Per-agent work environments, so parallel agents never queue on one build or one Ghidra project.

  python tools/agent/agent_env.py create NAME [--ghidra] [--game] [--no-configure]
  python tools/agent/agent_env.py list
  python tools/agent/agent_env.py remove NAME        (worktree + build + clones; branch kept)

create makes, under E:\\games\\bbport\\agents\\NAME (AGENTS_ROOT):
  repo\\           a git worktree of this repo on branch agent/NAME (from HEAD)
  repo\\out\\gpu\\   its own Ninja build dir, configured like the main one (BB_OUT_DIR = repo\\out)
  ghidra-proj\\    (--ghidra) a copy of the analysed Ghidra project: set BB_GHIDRA_PROJECT to it
  game\\           (--game) the game data as hardlinks (no extra disk space): every file of the
                   game dir except *.pkg, out\\, user\\ and the launcher's own files.
  env.txt          the variables to use (BB_GHIDRA_PROJECT, BB_GAME_DIR, build commands)

Saves are never linked: tools/mp/instances.py copies the save folder into each instance. The
user's game dir (GAME) is only read.
"""
import argparse, os, shutil, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
AGENTS_ROOT = Path(os.environ.get("BB_AGENTS_ROOT", r"E:\games\bbport\agents"))
GHIDRA_PROJECT = Path(os.environ.get("BB_GHIDRA_PROJECT", r"E:\games\bbport\deps\ghidra-proj"))
GAME = Path(os.environ.get("BB_GAME_DIR", r"E:\games\bloodborne"))
MAIN_CACHE = REPO / "out" / "gpu" / "CMakeCache.txt"
# Cache entries copied from the main build dir (everything machine-specific; BB_OUT_DIR is per agent).
CACHE_KEYS = ("CMAKE_BUILD_TYPE", "BB_LTO", "BB_MARCH", "BB_PGO", "MAGIC_ENUM_INC", "MINIZ_INC", "MINIZ_LIB",
              "VMA_INC", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
              "CMAKE_MAKE_PROGRAM")
GAME_SKIP_DIRS = {"out", "user", "__pycache__", "tools", "scripts", "patches", "mods", "fsr4_shaders"}
GAME_SKIP_SUFFIXES = {".pkg", ".zip", ".log", ".bak"}
MINGW = r"C:\msys64\mingw64\bin"


def git(*args, cwd=REPO):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def cache_values():
    vals = {}
    for line in MAIN_CACHE.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key_type, value = line.split("=", 1)
        key, typ = key_type.split(":", 1)
        if key in CACHE_KEYS:
            vals[key] = (typ, value)
    return vals


def configure(repo: Path):
    build = repo / "out" / "gpu"
    args = ["cmake", "-S", str(repo / "gpu"), "-B", str(build), "-G", "Ninja",
            f"-DBB_OUT_DIR:PATH={(repo / 'out').as_posix()}"]
    for key, (typ, value) in cache_values().items():
        args.append(f"-D{key}:{typ}={value}")
    env = os.environ.copy()
    env["PATH"] = MINGW + os.pathsep + env.get("PATH", "")
    subprocess.run(args, check=True, env=env)


def copy_submodules(repo: Path):
    """The worktree's submodule dirs are empty: copy the main checkout's (with its local changes)."""
    for line in git("submodule", "status").splitlines():
        path = line[1:].split()[1]
        src, dst = REPO / path, repo / path
        if dst.exists() and any(dst.iterdir()):
            continue
        shutil.copytree(src, dst, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".git"))
        print("submodule copied:", path)


def seed_out(repo: Path):
    r"""out\ holds prebuilt inputs the build links (libatrac9.a) and the runtime DLLs: hardlink the
    main checkout's (never its exes, logs or build dirs - those are per agent)."""
    src, dst = REPO / "out", repo / "out"
    dst.mkdir(parents=True, exist_ok=True)
    for f in src.iterdir():
        t = dst / f.name
        if t.exists() or f.name in ("gpu", "gpu-pgo-gen") or f.suffix.lower() in (".exe", ".log", ".pdb"):
            continue
        if f.is_dir():
            shutil.copytree(f, t)
        else:
            os.link(f, t)


def link_game(dst: Path):
    """Hardlinks of the game data (read-only use). Never the pkg, never user\\ (saves) or out\\."""
    n = 0
    for root, dirs, files in os.walk(GAME):
        rel = Path(root).relative_to(GAME)
        if rel == Path("."):
            dirs[:] = [d for d in dirs if d.lower() not in GAME_SKIP_DIRS]
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for f in files:
            if rel == Path(".") and (Path(f).suffix.lower() in GAME_SKIP_SUFFIXES or f.lower().endswith((".py", ".bat", ".json", ".md", ".txt", ".ini", ".exe"))
                                      or f in ("LICENSE",)):
                continue
            target = dst / rel / f
            if target.exists():
                continue
            os.link(Path(root) / f, target)
            n += 1
    return n


def create(a):
    home = AGENTS_ROOT / a.name
    repo = home / "repo"
    home.mkdir(parents=True, exist_ok=True)
    if not repo.exists():
        git("worktree", "add", "-B", f"agent/{a.name}", str(repo), "HEAD")
        print("worktree:", repo, "on branch", f"agent/{a.name}")
    copy_submodules(repo)
    seed_out(repo)
    if not a.no_configure and not (repo / "out" / "gpu" / "build.ninja").exists():
        configure(repo)
    env_lines = [f"REPO={repo}", f"BUILD: PATH={MINGW};%PATH% & cmake --build {repo / 'out' / 'gpu'}"]
    if a.ghidra:
        gp = home / "ghidra-proj"
        if not gp.exists():
            shutil.copytree(GHIDRA_PROJECT, gp, ignore=shutil.ignore_patterns("*.lock", "*.lock~"))
        env_lines.append(f"BB_GHIDRA_PROJECT={gp}")
    if a.game:
        gd = home / "game"
        print("game data hardlinks:", link_game(gd))
        env_lines.append(f"BB_GAME_DIR={gd}")
    (home / "env.txt").write_text("\n".join(env_lines) + "\n", encoding="utf-8")
    print("\n".join(env_lines))


def remove(a):
    home = AGENTS_ROOT / a.name
    repo = home / "repo"
    if repo.exists():
        git("worktree", "remove", "--force", str(repo))
    shutil.rmtree(home, ignore_errors=True)
    print("removed", home, "(branch agent/%s kept)" % a.name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["create", "list", "remove"])
    ap.add_argument("name", nargs="?")
    ap.add_argument("--ghidra", action="store_true")
    ap.add_argument("--game", action="store_true")
    ap.add_argument("--no-configure", action="store_true")
    a = ap.parse_args()
    if a.cmd == "list":
        print(git("worktree", "list"))
        return
    if not a.name:
        sys.exit("NAME is required")
    create(a) if a.cmd == "create" else remove(a)


if __name__ == "__main__":
    main()
