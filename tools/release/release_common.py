"""Shared rules of the release tools: what may ship, the stamp file, small helpers.

The stamp (`<stage>.stamp.json`, copied to `<zip>.stamp.json`) ties a staged package to the commit its
exe was built from; publish.py checks it against the commit the GitHub tag is made at.
"""
import fnmatch
import hashlib
import json
import re
import subprocess
from pathlib import Path

# Top-level entries a release may hold. Anything else in the stage is refused.
ALLOWED_TOP_DIRS = {"out", "docs", "scripts", "patches", "tools", "fsr4_shaders", "mods", "user",
                    "python", "map_generator", "licenses"}
ALLOWED_TOP_FILES = {"CHANGES.md", "LICENSE", "README.md", "README.ru.md", "VERSION", "launch_gui.bat",
                     "launcher.py", "run.bat", "net_doctor.bat"}
# The only files under user\ (the rest of the skeleton is empty folders).
ALLOWED_USER_FILES = {"user/readme.txt", "user/custom_trophy/Notice.txt"}

SAVE_DIRS = {"userdata", "savedata", "player_logs", "save_backup", "test_saves", "saves"}
GAME_DIRS = {"dvdroot", "game_data_dump", "ghidra-export", "param_dump", "msg_dump", "map_dump"}
GAME_NAMES = {"eboot.bin", "param.sfo", "sce_sys"}
GAME_EXTS = {".elf", ".self", ".pkg", ".sfo", ".dcx", ".bnd", ".bdt", ".bhd", ".param", ".parambnd",
             ".msgbnd", ".flver", ".sav", ".prx"}
KEY_EXTS = {".pem", ".key", ".p12", ".pfx", ".ppk", ".pub", ".crt"}
KEY_NAMES = {"id_rsa", "id_ed25519", "id_ecdsa", "id_dsa"}
KEY_OK_NAMES = {"cacert.pem"}  # a CA bundle is public data (certifi in the Python bundle)
DUMP_RE = re.compile(r"(param|msg|map)[_-]?(dump|export)|(dump|export)[_-]?(param|msg|map)", re.I)
DEFAULT_BIN_MB = 5


def sha256(path, block=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(block), b""):
            h.update(b)
    return h.hexdigest()


def git(repo, *args):
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def is_full_sha(s):
    return bool(re.fullmatch(r"[0-9a-f]{40}", s or ""))


def walk(stage):
    """Every file and directory under the stage as (posix relative path, Path, is_dir)."""
    stage = Path(stage)
    for p in sorted(stage.rglob("*")):
        rel = p.relative_to(stage).as_posix()
        if "__pycache__" in p.parts:
            continue
        yield rel, p, p.is_dir()


def check_stage(stage, bin_mb=DEFAULT_BIN_MB, allow=()):
    """The reasons the stage must not be shipped (empty list = fine).

    allow: fnmatch patterns (against the relative path) that exempt a file from the `*.bin` size rule only.
    """
    stage = Path(stage)
    bad = []
    if not stage.is_dir():
        return [f"stage is not a folder: {stage}"]
    seen_top = set()
    for rel, p, is_dir in walk(stage):
        parts = rel.split("/")
        low = rel.lower()
        name = parts[-1].lower()
        top = parts[0]
        seen_top.add(top)
        if p.is_symlink():
            bad.append(f"symlink: {rel}")
            continue
        if len(parts) == 1:
            if is_dir and top not in ALLOWED_TOP_DIRS:
                bad.append(f"top-level folder not allowed: {rel}")
            if not is_dir and top not in ALLOWED_TOP_FILES:
                bad.append(f"top-level file not allowed: {rel}")
        comps = {c.lower() for c in parts}
        if comps & SAVE_DIRS:
            bad.append(f"save / player data folder: {rel}")
        if comps & GAME_DIRS:
            bad.append(f"game data folder: {rel}")
        if top == "user" and not is_dir and rel not in ALLOWED_USER_FILES:
            bad.append(f"file under user/ (only readme.txt and custom_trophy/Notice.txt allowed): {rel}")
        if is_dir:
            continue
        ext = Path(name).suffix
        if ext == ".log":
            bad.append(f"log: {rel}")
        if ext == ".ini":
            bad.append(f"ini file: {rel}")
        if name == ".env" or name.startswith(".env.") or ext == ".env":
            bad.append(f".env file: {rel}")
        if (ext in KEY_EXTS and name not in KEY_OK_NAMES) or name in KEY_NAMES or name.startswith("id_rsa"):
            bad.append(f"key / certificate file: {rel}")
        if name in GAME_NAMES or ext in GAME_EXTS:
            bad.append(f"game file: {rel}")
        if ext == ".debug":
            bad.append(f"debug symbols belong in rel/symbols, not the package: {rel}")
        if DUMP_RE.search(name) or DUMP_RE.search("/".join(parts[:-1])):
            bad.append(f"param / msg / map dump: {rel}")
        if ext == ".bin" and p.stat().st_size > bin_mb * 1024 * 1024:
            if not any(fnmatch.fnmatch(rel, a) for a in allow):
                mb = p.stat().st_size / 1048576
                bad.append(f".bin larger than {bin_mb} MB ({mb:.1f} MB), not allowlisted: {rel}")
    return bad


def read_stamp(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def exe_build_id(exe, version=None):
    """(version, id) from the exe's embedded bbport_build_id string ("0.7.9 (v0.7.4-1225-gc7a4f91e35)"), or None."""
    data = Path(exe).read_bytes()
    ver = re.escape(version).encode() if version else rb"\d+(?:\.\d+){1,3}"
    m = re.search(b"(" + ver + rb") \(([A-Za-z0-9._-]{4,60})\)" + bytes([0]), data)
    return (m.group(1).decode(), m.group(2).decode()) if m else None
