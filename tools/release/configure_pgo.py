#!/usr/bin/env python3
r"""Configure the PGO release build dir (out/gpu-pgo-gen) of a clone, with the profile remapped.

    python tools/release/configure_pgo.py <clone> [--src E:\games\bbport\src] [--dry-run]

<clone> is an agent folder (agents\NAME, holding repo\) or the repo itself. The values come from the main
checkout's out/gpu-pgo-gen/CMakeCache.txt (Ninja, RelWithDebInfo, BB_LTO=ON, BB_STRIP=ON, BB_PGO=use,
VMA_INC, MINIZ_INC, compilers). gcc names a .gcda after the absolute object path, so the profile
(<src>/pgo/<mangled src build dir>) is copied to <clone>/pgo_map/<mangled clone build dir> and BB_PGO_DIR
points there. The exe goes to <clone>/out/release (build: tools/agent/build.py is for agent builds and
never configures; use `cmake --build <clone>/out/gpu-pgo-gen` with mingw64\bin on PATH, see docs/RELEASE.md).
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

MINGW = r"C:\msys64\mingw64\bin"
MAIN_SRC = Path(r"E:\games\bbport\src")
BUILD_NAME = "gpu-pgo-gen"
# Cache entries copied as they are (machine-specific paths and the release switches).
COPY_KEYS = ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM", "VMA_INC", "MINIZ_INC",
             "MINIZ_LIB", "MAGIC_ENUM_INC", "BB_MARCH", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS")
# Release switches: forced, but a cache that disagrees is reported (the source cache is the reference).
FORCED = {"CMAKE_BUILD_TYPE": ("STRING", "RelWithDebInfo"), "BB_LTO": ("BOOL", "ON"),
          "BB_STRIP": ("BOOL", "ON"), "BB_PGO": ("STRING", "use")}


def mangle(path):
    """gcc's profile directory for an object path: 'E:/a/b' -> 'E~/a/b' (drive colon becomes ~)."""
    return Path(path).as_posix().replace(":", "~")


def read_cache(path):
    vals = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key_type, value = line.split("=", 1)
        key, typ = key_type.split(":", 1)
        vals[key] = (typ, value)
    return vals


def find_repo(clone):
    clone = Path(clone).resolve()
    for cand in (clone / "repo", clone):
        if (cand / "gpu" / "CMakeLists.txt").is_file():
            return cand
    raise SystemExit(f"not a clone (no gpu/CMakeLists.txt in {clone} or {clone / 'repo'})")


def plan(clone, src=MAIN_SRC):
    """The cmake argument list, the profile copy (from, to) and notes; no side effects."""
    repo = find_repo(clone)
    src = Path(src)
    cache_file = src / "out" / BUILD_NAME / "CMakeCache.txt"
    if not cache_file.is_file():
        raise SystemExit(f"no source cache: {cache_file}")
    cache = read_cache(cache_file)
    notes = []
    build = repo / "out" / BUILD_NAME
    pgo_dst = repo / "pgo_map" / mangle(build)
    # Where the profile of the source build lives: BB_PGO_DIR / <mangled source build dir>.
    pgo_root = Path(cache.get("BB_PGO_DIR", ("", str(src / "pgo")))[1])
    pgo_src = pgo_root / mangle(src / "out" / BUILD_NAME)
    args = ["cmake", "-S", str(repo / "gpu"), "-B", str(build), "-G", "Ninja"]
    for key, (typ, value) in FORCED.items():
        have = cache.get(key, (typ, None))[1]
        if have is not None and have.upper() != value.upper():
            notes.append(f"note: source cache has {key}={have}, release uses {value}")
        args.append(f"-D{key}:{typ}={value}")
    for key in COPY_KEYS:
        if key in cache and cache[key][1] != "":
            args.append(f"-D{key}:{cache[key][0]}={cache[key][1]}")
    args.append(f"-DBB_OUT_DIR:PATH={(repo / 'out' / 'release').as_posix()}")
    args.append(f"-DBB_PGO_DIR:PATH={pgo_dst.as_posix()}")
    return repo, args, (pgo_src, pgo_dst), notes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("clone")
    ap.add_argument("--src", default=str(MAIN_SRC), help="main checkout holding out/gpu-pgo-gen and pgo/")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, change nothing")
    a = ap.parse_args(argv)
    repo, args, (pgo_src, pgo_dst), notes = plan(a.clone, a.src)
    if not pgo_src.is_dir():
        raise SystemExit(f"profile not found: {pgo_src}")
    gcda = sorted(pgo_src.glob("*.gcda"))
    if not gcda:
        raise SystemExit(f"no .gcda files in {pgo_src}")
    print(f"clone:      {repo}")
    print(f"profile:    {pgo_src} ({len(gcda)} gcda)")
    print(f"remapped:   {pgo_dst}")
    for n in notes:
        print(n)
    print("cmake:      " + " ".join(args))
    if a.dry_run:
        print("dry run: nothing changed")
        return 0
    pgo_dst.mkdir(parents=True, exist_ok=True)
    for f in gcda:
        shutil.copy2(f, pgo_dst / f.name)
    print(f"copied {len(gcda)} profile files")
    env = os.environ.copy()
    env["PATH"] = MINGW + os.pathsep + env.get("PATH", "")
    subprocess.run(args, check=True, env=env)
    build = repo / "out" / BUILD_NAME
    exe_dir = repo / "out" / "release"
    print(f"configured {build}")
    print(f"build:      set PATH={MINGW};%PATH% & cmake --build {build}   (exe: {exe_dir / 'bbport.exe'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
