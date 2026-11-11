#!/usr/bin/env python3
"""Build the two-PC test kits (pc1.zip, pc2.zip) from a staged port build and a game folder.

  python tools/twopc/make_kits.py --stage E:\\games\\bbport\\rel081 --game E:\\games\\bbport\\agents\\fullsmoke081\\game
         [--out E:\\games\\bbport\\twopc081] [--saves E:\\games\\bbport\\test_saves] [--pcs pc1,pc2]
  python tools/twopc/make_kits.py --stage REL --game GAME --dir DIR --pcs pc1 --link   (an unpacked kit folder, hard-linked: the dry run)

Each kit holds   bbport\\     the staged port build (our files; no game data)
                 CUSA03173\\  the game files from --game (eboot.bin, sce_*, dvdroot_ps4: copied, stored not compressed, zip64)
                 twopc\\      this folder's runner + the tools it runs (net_doctor, session_score, verdict) + the test saves
                 START_PCn.bat, README.txt
The game files go only into the zips under --out (outside the repo). Nothing here is ever committed or uploaded.
"""
import argparse
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
GAME_PARTS = ("eboot.bin", "sce_discmap.plt", "sce_module", "sce_sys", "dvdroot_ps4")  # what the port reads of the game
KIT_GAME = "CUSA03173"
SAVE_FIXTURES = {"pc1": ["savedata"], "pc2": ["savedata", "chalice_pthumeru_ritual"]}
TOOLS = [  # (repo path, path inside twopc/repo/)
    "scripts/net_doctor.py", "scripts/ipmask.py", "scripts/save_hash.py", "tools/party/session_score.py",
    "tools/mp/smoke_party.py", "docs/party/ERSC_PARITY.md"]
TWOPC_FILES = ["runner.py", "blocks.py", "verdict.py"]
BAT = ('@echo off\r\ncd /d "%~dp0"\r\ntitle bbport 0.8.1 two-PC test - {role}\r\n'
       'echo bbport 0.8.1 two-PC test, {role} ({what}). Read README.txt once.\r\n'
       'echo Allow Windows Firewall prompts for bbport.exe and python.exe (Private networks).\r\n'
       'echo Press any key to start; it then runs by itself (about 1.5 hours), do not touch the game windows.\r\n'
       'pause >nul\r\n'
       'bbport\\python\\python.exe -I twopc\\runner.py --role {role} %*\r\n'
       'echo.\r\necho Finished. Send back the twopc_logs_{role}_*.zip next to this file.\r\npause\r\n')


def gb(n):
    return "%.1f GB" % (n / 1e9)


def tree_size(p):
    t = 0
    for r, _d, fs in os.walk(p):
        for f in fs:
            try:
                t += os.path.getsize(os.path.join(r, f))
            except OSError:
                pass
    return t


def game_files(game):
    game = Path(game)
    if not (game / "eboot.bin").exists():
        sys.exit("no eboot.bin in %s: not a game folder" % game)
    out = []
    for part in GAME_PARTS:
        p = game / part
        if p.is_file():
            out.append((p, KIT_GAME + "/" + part))
        elif p.is_dir():
            for r, _d, fs in os.walk(p):
                for f in fs:
                    fp = Path(r) / f
                    out.append((fp, KIT_GAME + "/" + fp.relative_to(game).as_posix()))
        else:
            print("warning: %s has no %s" % (game, part))
    return out


def bbport_files(stage):
    """The staged build, minus what must not travel: its own player data, caches and logs."""
    stage = Path(stage)
    skip_dirs = {"__pycache__", "logs", "cache", "captures", "savedata", "savedata_party"}
    out = []
    for r, ds, fs in os.walk(stage):
        rel = Path(r).relative_to(stage)
        ds[:] = [d for d in ds if d not in skip_dirs and not (rel.parts[:1] == ("user",) and d in ("savedata", "savedata_party"))]
        for f in fs:
            if rel.parts[:1] == ("user",) and f not in ("readme.txt",) and not f.endswith(".txt"):
                continue  # the build's own user\\ folder: only its readme
            if f in ("launcher_settings.json", "bbport.ini", "launcher.log", "party_code.txt") or f.endswith((".frames.csv", ".readbacks.csv")):
                continue
            out.append((Path(r) / f, "bbport/" + (rel / f).as_posix()))
    return out


def twopc_files(role):
    out = [(HERE / f, "twopc/" + f) for f in TWOPC_FILES]
    for t in TOOLS:
        out.append((REPO / t, "twopc/repo/" + t))
    return out


def fixture_files(saves, role):
    out = []
    for fx in SAVE_FIXTURES[role]:
        base = Path(saves) / fx if fx == "savedata" else Path(saves) / fx / "savedata"
        if not base.is_dir():
            sys.exit("test save %s is missing (docs/party/test_saves.md)" % base)
        for r, _d, fs in os.walk(base):
            for f in fs:
                p = Path(r) / f
                out.append((p, "twopc/fixtures/%s/savedata/%s" % (fx, p.relative_to(base).as_posix())))
    return out


def text_members(role, readme):
    what = "the host" if role == "pc1" else "the guest and the invader"
    return [("START_%s.bat" % role.upper(), BAT.format(role=role, what=what)),
            ("README.txt", readme),
            ("twopc/config.json", '{"role": "%s", "game_dir": "%s"}\n' % (role, KIT_GAME))]


def kit_members(role, stage, game, saves, readme):
    return (bbport_files(stage), twopc_files(role), fixture_files(saves, role), game_files(game), text_members(role, readme))


def write_zip(path, prefix, groups):
    n = 0
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
        for src, arc in groups[0] + groups[1] + groups[2]:
            ctype = zipfile.ZIP_DEFLATED if str(src).endswith((".py", ".txt", ".md", ".bat", ".json", ".ini")) else zipfile.ZIP_STORED
            z.write(src, "%s/%s" % (prefix, arc), compress_type=ctype)
            n += 1
        for src, arc in groups[3]:  # the game: stored (already compressed), streamed
            z.write(src, "%s/%s" % (prefix, arc), compress_type=zipfile.ZIP_STORED)
            n += 1
        for arc, text in groups[4]:
            z.writestr("%s/%s" % (prefix, arc), text)
    return n


def write_dir(dest, groups, link):
    """An unpacked kit folder. link: hard links where the volume allows (the dry run; the game itself is not copied)."""
    dest = Path(dest)
    for src, arc in groups[0] + groups[1] + groups[2]:
        t = dest / arc
        t.parent.mkdir(parents=True, exist_ok=True)
        t.unlink(missing_ok=True)
        if link and arc.startswith("bbport/out/"):
            try:
                os.link(src, t)
                continue
            except OSError:
                pass
        shutil.copy2(src, t)
    for arc, text in groups[4]:
        t = dest / arc
        t.parent.mkdir(parents=True, exist_ok=True)
        t.write_text(text, encoding="utf-8", newline="")


def check_space(out, need):
    out.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(out).free
    if free < need:
        sys.exit("not enough free space on %s: need about %s (two kits plus a margin), have %s" % (out.anchor, gb(need), gb(free)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", required=True, help="the staged port build (E:\\games\\bbport\\rel081)")
    ap.add_argument("--game", required=True, help="a game folder (eboot.bin, dvdroot_ps4, ...): copied into the kits")
    ap.add_argument("--out", default=r"E:\games\bbport\twopc081")
    ap.add_argument("--saves", default=r"E:\games\bbport\test_saves")
    ap.add_argument("--pcs", default="pc1,pc2")
    ap.add_argument("--dir", default="", help="write an unpacked kit folder (for --pcs with one role) instead of the zips")
    ap.add_argument("--link", action="store_true", help="with --dir: hard-link bbport\\out files")
    a = ap.parse_args()
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        print("note: building from the player's own game folder (read only)")
    readme_t = (HERE / "README_kit.txt").read_text(encoding="utf-8")
    roles = [r for r in a.pcs.split(",") if r]
    gfiles = game_files(a.game)
    gsize = sum(f.stat().st_size for f, _ in gfiles)
    print("game files: %d files, %s" % (len(gfiles), gb(gsize)))
    est = "about " + gb(gsize + tree_size(a.stage))
    readme_in = readme_t.replace("{PC1_SIZE}", est).replace("{PC2_SIZE}", est)
    if a.dir:
        for r in roles:
            groups = kit_members(r, a.stage, a.game, a.saves, readme_t)
            write_dir(a.dir, (groups[0], groups[1], groups[2], [], groups[4]), a.link)
            print("kit folder:", a.dir)
        return
    out = Path(a.out)
    stage_size = tree_size(a.stage)
    check_space(out, 2 * (gsize + stage_size) + 8 * 1024 ** 3)
    sizes = {}
    for r in roles:
        t0 = time.time()
        zp = out / ("%s.zip" % r)
        groups = kit_members(r, a.stage, a.game, a.saves, readme_in)
        n = write_zip(zp, "twopc081_%s" % r, groups)
        sizes[r] = zp.stat().st_size
        print("%s: %d files, %s, %.0f s" % (zp, n, gb(sizes[r]), time.time() - t0))
    readme = readme_t.replace("{PC1_SIZE}", gb(sizes.get("pc1", 0))).replace("{PC2_SIZE}", gb(sizes.get("pc2", 0)))
    (out / "README.txt").write_text(readme, encoding="utf-8")
    print("README:", out / "README.txt")


if __name__ == "__main__":
    main()
