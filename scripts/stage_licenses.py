"""Put the licence texts of everything a build ships into out\\licenses.

Called by stage_dlls.py after the DLLs are staged, so every build (and so every release, which
copies out\\) carries them:
  licenses\\LICENSE                  the port's own GPL-3.0 text
  licenses\\LGPL-*.txt, msys2\\, third_party\\SOURCE-NOTICES.txt   from the repo's licenses\\
  licenses\\msys2\\<pkg>\\...          licence files of the MSYS2 package owning each staged DLL
  licenses\\msys2\\PACKAGES.txt        DLL -> package, version, declared licence
  licenses\\third_party\\<lib>\\...    libraries compiled into bbport.exe
After HatsyRei's CI staging in Mrsuss60/bloodborne_pc_windows_port (c286c572, 4667f9f6), done
here with the local MSYS2 (pacman -Qo / -Qi / -Ql) instead of CI.

    python scripts/stage_licenses.py [--out out]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MSYS2 = Path(os.environ.get("MSYS2_ROOT", r"C:\msys64"))
DEPS = Path(os.environ.get("BB_DEPS_DIR", ROOT.parent.parent / "deps"))
if not DEPS.is_dir():
    DEPS = Path(r"E:\games\bbport\deps")

# Libraries compiled into bbport.exe: (folder under third_party, licence file). An entry that
# starts with "mingw64:" is under MSYS2's /mingw64/share/licenses, "deps:" under the deps folder
# (magic_enum, miniz and xbyak are built outside pacman).
BUILT_IN = [
    ("imgui", "gpu/third_party/imgui/LICENSE.txt"),
    ("LibAtrac9", "third_party/LibAtrac9/LICENSE"),
    ("fsr-vulkan", "gpu/third_party/fsr-vulkan/LICENSE.txt"),
    ("fsr-vulkan", "gpu/third_party/fsr-vulkan/NOTICE.md"),
    ("fsr-vulkan", "gpu/third_party/fsr-vulkan/UPSTREAM.md"),
    ("fsr-vulkan/ffx-1.1.4", "gpu/third_party/fsr-vulkan/upstream/ffx-1.1.4/sdk/LICENSE.txt"),
    ("sirit", "gpu/third_party/sirit/LICENSE.txt"),
    ("SPIRV-Headers", "gpu/third_party/sirit/externals/SPIRV-Headers/LICENSE"),
    ("half", "gpu/third_party/half/LICENSE.txt"),
    ("DejaVu-fonts", "gpu/third_party/fonts/LICENSE-DejaVu.txt"),
    ("monocypher", "gpu/third_party/monocypher/LICENCE.md"),
    ("robin-map", "mingw64:robin-map/LICENSE"),
    ("VulkanMemoryAllocator", "mingw64:VulkanMemoryAllocator/LICENSE"),
    ("vulkan-headers", "mingw64:vulkan-headers/LICENSE"),
    ("fmt", "mingw64:fmt/LICENSE"),
    ("boost", "mingw64:boost/LICENSE"),
    ("magic_enum", "deps:magic_enum/LICENSE"),
    ("miniz", "deps:miniz312/LICENSE"),
    ("xbyak", "deps:xbyak/COPYRIGHT"),
]

# DLLs that are not from MSYS2 and carry their own licence file next to bbport.exe.
OWN_LICENSE = {
    "bbport_dlss.dll": "bbport_dlss_LICENSE.txt",
    "nvngx_dlss.dll": "NVIDIA_DLSS_LICENSE.txt",
    "nvngx_dlssg.dll": "NVIDIA_DLSS_LICENSE.txt",
}


# MSYS2's declared licence where it says less than upstream does (x264: "custom"; its COPYING
# is the GPL 2, "or any later version").
LICENSE_OVERRIDE = {
    "mingw-w64-x86_64-libx264": "GPL-2.0-or-later",
}

# GPL/LGPL family: the standard texts LICENSE (GPL-3.0), GPL-2.0.txt, LGPL-2.1.txt and
# LGPL-3.0.txt cover a package that ships no licence file of its own.
_GNU = re.compile(r"^(L?GPL[0-9.]*|L?GPL-[23]\.[01](-only|-or-later|\+)?)$")


def gnu_only(licence):
    words = [w for w in re.split(r"[\s,()]+", licence.replace("spdx:", "").replace("documentation:", ""))
             if w and w not in ("OR", "AND", "WITH", "GCC-exception-3.1")]
    return bool(words) and all(_GNU.match(w) for w in words)


def resolve(spec):
    if spec.startswith("mingw64:"):
        return MSYS2 / "mingw64/share/licenses" / spec[len("mingw64:"):]
    if spec.startswith("deps:"):
        return DEPS / spec[len("deps:"):]
    return ROOT / spec


def pacman(*args):
    exe = MSYS2 / "usr/bin/pacman.exe"
    res = subprocess.run([str(exe), *args], capture_output=True, text=True, check=False)
    return res.stdout


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)


def stage(out):
    lic = out / "licenses"
    if lic.exists():
        shutil.rmtree(lic)
    (lic / "msys2").mkdir(parents=True)
    copy(ROOT / "LICENSE", lic / "LICENSE")
    shutil.copytree(ROOT / "licenses", lic, dirs_exist_ok=True)
    problems = []

    # The MSYS2 package that owns each staged DLL ("<path> is owned by <pkg> <version>").
    dlls = sorted(out.glob("*.dll"), key=lambda p: p.name.lower())
    owner = {}
    in_msys2 = [d.name for d in dlls if (MSYS2 / "mingw64/bin" / d.name).is_file()]
    if in_msys2:
        for line in pacman("-Qo", *(f"/mingw64/bin/{n}" for n in in_msys2)).splitlines():
            path, sep, rest = line.partition(" is owned by ")
            if sep:
                owner[path.rsplit("/", 1)[-1]] = rest.split()[0]
    info, with_files = {}, set()
    pkgs = sorted(set(owner.values()))
    if pkgs:
        name = version = None
        for line in pacman("-Qi", *pkgs).splitlines():
            key, _, value = line.partition(":")
            key, value = key.strip(), value.strip()
            if key == "Name":
                name = value
            elif key == "Version":
                version = value
            elif key == "Licenses" and name:
                info[name] = (version, value)
        for line in pacman("-Ql", *pkgs).splitlines():
            pkg, _, path = line.partition(" ")
            if path.startswith("/mingw64/share/licenses/") and not path.endswith("/"):
                copy(MSYS2 / path.lstrip("/"), lic / "msys2" / path[len("/mingw64/share/licenses/"):])
                with_files.add(pkg)

    lines = ["DLLs shipped in out\\ and where their licence texts are.",
             "MSYS2 packages: their files are in msys2\\<name>\\; LGPL texts in LGPL-2.1.txt / LGPL-3.0.txt,",
             "GPL-3.0 in LICENSE. Sources: https://repo.msys2.org/mingw/sources/", "",
             f"{'DLL':32} {'package':40} {'version':16} licence"]
    for d in dlls:
        pkg = owner.get(d.name)
        if pkg:
            version, licence = info.get(pkg, ("?", "?"))
            licence = LICENSE_OVERRIDE.get(pkg, licence)
            # A few packages ship no licence file: the repo's licenses\msys2\ has those, or the
            # licence is a GPL/LGPL whose standard text is in licenses\.
            covered = pkg in with_files or (lic / "msys2" / pkg.replace("mingw-w64-x86_64-", "")).is_dir()
            note = ""
            if not covered and gnu_only(licence):
                covered, note = True, "  (standard text)"
            elif not covered:
                note = "  (no licence file in the package)"
            lines.append(f"{d.name:32} {pkg:40} {version:16} {licence}{note}")
            if not covered:
                problems.append(f"{d.name}: {pkg} ships no licence file")
        elif d.name.lower() in OWN_LICENSE:
            lines.append(f"{d.name:32} {'(not MSYS2)':40} {'':16} see {OWN_LICENSE[d.name.lower()]}")
        else:
            lines.append(f"{d.name:32} {'(no MSYS2 package owns this DLL)':40} {'':16} UNKNOWN")
            problems.append(f"{d.name}: no MSYS2 package owns it, licence not determined")
    (lic / "msys2" / "PACKAGES.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    for folder, spec in BUILT_IN:
        src = resolve(spec)
        if src.is_file():
            copy(src, lic / "third_party" / folder / src.name)
        else:
            problems.append(f"built-in {folder}: {src} not found")
    return problems


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path("out"))
    args = ap.parse_args()
    problems = stage(args.out)
    print(f"Licence texts staged in {args.out / 'licenses'}.")
    for p in problems:
        print(f"  licence warning: {p}")
