#!/usr/bin/env python3
"""Local multiplayer test harness: runs several game instances on one PC.

Each instance gets its own folder (hard links to the build's run.bat, scripts, patches and
out\\*.exe/*.dll, so no copies), its own user\\ (saves, shader cache, add-on folder), its own
bbport.ini (720p, upscaler off) and its own log. Instances find each other over loopback.

  python tools/mp/instances.py setup --count 2 [--root DIR] [--build DIR] [--saves DIR]
  python tools/mp/instances.py run --count 2 [--seconds 120] [--env KEY=VALUE ...]
  python tools/mp/instances.py logs --count 2 [--grep REGEX]
  python tools/mp/instances.py kill

Instance i (0 = host) gets BB_MP_INSTANCE=i and BB_MP_PORT=base+i, which the party code reads
when BB_MP_LOCAL_TEST=1. Windows are placed side by side.
"""
import argparse, ctypes, os, re, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_ROOT = REPO.parent / "mptest"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", r"E:\games\bloodborne"))
BASE_PORT = 47600
INI = ("upscaler=off\npreset=0\noutput_res=1280x720\nlive_resolution=0\nshow_fps=1\n"
       "skip_intro=1\n")


def link_tree(src: Path, dst: Path):
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for f in files:
            target = dst / rel / f
            if target.exists():
                target.unlink()
            try:
                os.link(Path(root) / f, target)
            except OSError:
                shutil.copy2(Path(root) / f, target)


def setup(a):
    build = Path(a.build)
    for i in range(a.count):
        inst = Path(a.root) / f"inst{i}"
        (inst / "out").mkdir(parents=True, exist_ok=True)
        for name in ("run.bat", "launcher.py"):
            if (build / name).exists():
                shutil.copy2(build / name, inst / name)
        for sub in ("scripts", "patches"):
            link_tree(build / sub, inst / sub)
        for f in (build / "out").iterdir():
            if f.suffix.lower() in (".exe", ".dll"):
                t = inst / "out" / f.name
                if t.exists():
                    t.unlink()
                if f.suffix.lower() == ".exe":
                    shutil.copy2(f, t)  # a rebuild may rewrite the build's exe in place
                    continue
                try:
                    os.link(f, t)
                except OSError:
                    shutil.copy2(f, t)
        user = inst / "user"
        user.mkdir(exist_ok=True)
        if a.saves and Path(a.saves).is_dir() and not (user / "savedata").exists():
            shutil.copytree(a.saves, user / "savedata")
        (inst / "bbport.ini").write_text(INI, encoding="utf-8")
        print("instance", i, "->", inst)


def run(a):
    user32 = ctypes.windll.user32 if os.name == "nt" else None
    procs = []
    for i in range(a.count):
        inst = Path(a.root) / f"inst{i}"
        env = os.environ.copy()
        env.update({
            "BB_GAME_DIR": str(a.game), "BB_FROM_LAUNCHER": "1", "BB_NO_LAUNCHER_SETTINGS": "1",
            "BB_FULLSCREEN": "0", "BB_FPS": str(a.fps), "BB_TIMEOUT": str(a.seconds), "BB_VRAM_LIMIT_MB": str(a.vram),
            "BB_SHADER_PRECOMPILE": "1", "BB_MUTE_UNFOCUSED": "1",
            "BB_PATCHES": "Skip Online/Offline Choice;Skip Intro",
            "BB_MP_LOCAL_TEST": "1", "BB_MP_INSTANCE": str(i), "BB_MP_PORT": str(BASE_PORT + i),
            "BB_MP_NAME": f"Hunter{i}",
        })
        for kv in a.env or []:
            k, _, v = kv.partition("=")
            env[k] = v.replace("{i}", str(i)).replace("{port}", str(BASE_PORT + i))
        log = open(inst / "run.log", "wb")
        procs.append((i, subprocess.Popen(["cmd", "/c", str(inst / "run.bat")], cwd=inst, env=env,
                                          stdout=log, stderr=subprocess.STDOUT), log))
        time.sleep(a.stagger)
    t0 = time.time()
    while any(p.poll() is None for _, p, _ in procs) and time.time() - t0 < a.seconds + 90:
        time.sleep(1)
    for i, p, log in procs:
        if p.poll() is None:
            subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
        log.close()
        print(f"instance {i}: exit {p.wait()}")


def logs(a):
    rx = re.compile(a.grep) if a.grep else None
    for i in range(a.count):
        path = Path(a.root) / f"inst{i}" / "run.log"
        if not path.exists():
            continue
        print(f"===== instance {i}")
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not rx or rx.search(line):
                print(line)


def kill(a):
    root = str(Path(a.root)).lower()
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process bbport*,bb-probe* -ErrorAction SilentlyContinue | "
                          "ForEach-Object { $_.Id.ToString() + '|' + $_.Path }"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, path = line.partition("|")
        if path.lower().startswith(root):
            subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True)
            print("killed", pid, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["setup", "run", "logs", "kill"])
    ap.add_argument("--count", type=int, default=2)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=str(DEFAULT_GAME))
    ap.add_argument("--saves", default=str(DEFAULT_GAME / "user" / "savedata"))
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--vram", type=int, default=3072)
    ap.add_argument("--fps", default="30", help="frame cap of the test instances (30, 60, 90, uncap)")
    ap.add_argument("--stagger", type=float, default=8.0)
    ap.add_argument("--env", action="append")
    ap.add_argument("--grep")
    a = ap.parse_args()
    {"setup": setup, "run": run, "logs": logs, "kill": kill}[a.cmd](a)


if __name__ == "__main__":
    main()
