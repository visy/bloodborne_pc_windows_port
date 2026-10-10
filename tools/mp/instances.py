#!/usr/bin/env python3
"""Local multiplayer test harness: runs several game instances on one PC.

Each instance gets its own folder (hard links to the build's run.bat, scripts, patches and
out\\*.exe/*.dll, so no copies), its own user\\ (saves, shader cache, add-on folder), its own
bbport.ini (720p, upscaler off) and its own log. Instances find each other over loopback.

  python tools/mp/instances.py setup --count 2 [--root DIR] [--build DIR] [--saves DIR]
  python tools/mp/instances.py run --count 2 [--seconds 120] [--party] [--env KEY=VALUE ...]
  python tools/mp/instances.py crash --instance 1 [--after 0]
  python tools/mp/instances.py logs --count 2 [--grep REGEX]
  python tools/mp/instances.py kill

Instance i (0 = host) gets BB_MP_INSTANCE=i and BB_MP_PORT=base+i, which the party code reads
when BB_MP_LOCAL_TEST=1. Windows are placed side by side.

--party: a loopback party. Instance 0 hosts (BB_PARTY=host), the others join (BB_PARTY=join) with
the code the host writes to ROOT/party_code.txt (BB_PARTY_CODE_FILE, deleted before the run);
BB_PARTY_PORT=base+i, BB_PARTY_NAME=Hunter{i}, no UPnP, no public STUN, 127.0.0.1 everywhere.
--env still overrides any of these. --crash-at S kills instance --crash-instance's bbport.exe
(TerminateProcess, as a crash) S seconds into the run; run.bat then restarts it (BB_PARTY_RESTART).
"""
import argparse, ctypes, os, re, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_ROOT = REPO.parent / "mptest"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", r"E:\games\bloodborne"))
BASE_PORT = 47600  # default root; see base_port()


def base_port(root) -> int:
    """Each harness root gets its own port range, so agents' parallel runs (tools/agent/agent_env.py
    gives each its own root) never collide: BB_MP_BASE_PORT, else 47600 for the default root, else
    47700 + 10 * (a stable hash of the root path % 150)."""
    if os.environ.get("BB_MP_BASE_PORT"):
        return int(os.environ["BB_MP_BASE_PORT"])
    r = Path(root).resolve()
    if r == Path(DEFAULT_ROOT).resolve():
        return BASE_PORT
    import zlib
    return 47700 + 10 * (zlib.crc32(str(r).lower().encode()) % 150)
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


def builds_running():
    """Names of running ninja/ld processes (a build may be rewriting out\\bbport.exe)."""
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    return sorted({n for n in ("ninja.exe", "ld.exe", "lto-wrapper.exe") if f'"{n}"' in out})


def wait_for_builds(build=None, limit=600):
    """Waits until the build's own out\bbport.exe is a complete PE file that has not changed for
    5 s (other worktrees' builds - other agents - do not matter). Without `build`: until no
    ninja/ld runs at all (the old behaviour)."""
    t0 = time.time()
    if build is None:
        while (running := builds_running()) and time.time() - t0 < limit:
            print("waiting for the build to finish:", ", ".join(running), flush=True)
            time.sleep(10)
        if builds_running():
            sys.exit("a build is still running after %d s; not copying the executables" % limit)
        return
    exe = Path(build) / "out" / "bbport.exe"

    def stamp():
        try:
            st = exe.stat()
            return (st.st_size, st.st_mtime_ns)
        except OSError:
            return None

    while time.time() - t0 < limit:
        a = stamp()
        if a and time.time() - a[1] / 1e9 > 5 and exe_ok(exe, 1 << 20):
            time.sleep(2)
            if stamp() == a:
                return
        print("waiting for", exe, "to be complete and stable", flush=True)
        time.sleep(5)
    sys.exit("%s is not complete after %d s; not copying the executables" % (exe, limit))


def exe_ok(path: Path, min_size=1):
    """A complete PE file: non-empty (at least min_size bytes) and starting with MZ."""
    try:
        if path.stat().st_size < max(min_size, 2):
            return False
        with open(path, "rb") as f:
            return f.read(2) == b"MZ"
    except OSError:
        return False


def min_size_for(name: str):
    return (1 << 20) if name.lower() == "bbport.exe" else 1


def copy_exe(src: Path, dst: Path, timeout=60):
    """Copies an exe to a temporary name, checks it (size equals the source's, MZ, bbport.exe
    > 1 MB) and moves it into place; retries while the source is being written."""
    tmp = dst.with_name(dst.name + ".copying")
    t0 = time.time()
    while True:
        try:
            size = src.stat().st_size
            shutil.copy2(src, tmp)
            if (size > 0 and tmp.stat().st_size == size == src.stat().st_size
                    and exe_ok(tmp, min_size_for(src.name))):
                os.replace(tmp, dst)
                return
        except OSError:
            pass
        if time.time() - t0 > timeout:
            if tmp.exists():
                tmp.unlink()
            sys.exit(f"{src} is not a complete executable after {timeout} s (a build writing it?); not copied")
        time.sleep(2)


def validate_instances(root, count):
    """Every exe/dll in each instance's out\\ must be a complete PE file; never launch otherwise."""
    bad = []
    for i in range(count):
        out = Path(root) / f"inst{i}" / "out"
        if not exe_ok(out / "bbport.exe", 1 << 20):
            bad.append(str(out / "bbport.exe"))
        for f in out.glob("*.dll"):
            if not exe_ok(f):
                bad.append(str(f))
    return bad


def setup(a):
    build = Path(a.build)
    wait_for_builds(build)
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
                if f.suffix.lower() == ".exe":
                    copy_exe(f, t)  # a copy: a rebuild may rewrite the build's exe in place
                    continue
                if t.exists():
                    t.unlink()
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
    port0 = base_port(a.root)
    user32 = ctypes.windll.user32 if os.name == "nt" else None
    bad = validate_instances(a.root, a.count)
    if bad:
        print("not launching: incomplete executables (run setup again once no build runs):", *bad, sep="\n  ")
        sys.exit(1)
    if os.name == "nt":
        # No error dialogs on the desktop from the children (inherited error mode): log-only tests.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    procs = []
    code_file = Path(a.root) / "party_code.txt"
    if a.party and code_file.exists():
        code_file.unlink()  # a stale code (old secret) would only get the guests rejected
    for i in range(a.count):
        inst = Path(a.root) / f"inst{i}"
        env = os.environ.copy()
        env.update({
            "BB_GAME_DIR": str(a.game), "BB_FROM_LAUNCHER": "1", "BB_NO_LAUNCHER_SETTINGS": "1",
            "BB_FULLSCREEN": "0", "BB_FPS": str(a.fps), "BB_TIMEOUT": str(a.seconds), "BB_VRAM_LIMIT_MB": str(a.vram),
            "BB_SHADER_PRECOMPILE": "1", "BB_MUTE_UNFOCUSED": "1", "BB_WINDOW_BACKGROUND": "1",
            "BB_PATCHES": "Skip Online/Offline Choice;Skip Intro",
            "BB_MP_LOCAL_TEST": "1", "BB_MP_INSTANCE": str(i), "BB_MP_PORT": str(port0 + i),
            "BB_MP_NAME": f"Hunter{i}",
        })
        if a.party:
            env.update({
                "BB_PARTY": "host" if i == 0 else "join", "BB_PARTY_CODE_FILE": str(code_file),
                "BB_PARTY_PORT": str(port0 + i), "BB_PARTY_NAME": f"Hunter{i}", "BB_PARTY_UPNP": "0",
                "BB_PARTY_STUN": "off", "BB_PARTY_LOOPBACK": "1", "BB_PARTY_LOCAL_IP": "127.0.0.1",
                "BB_SKIP_NETWORK_CHOICE": "online", "BB_PATCHES": "Skip Intro",
            })
        for kv in a.env or []:
            k, _, v = kv.partition("=")
            env[k] = v.replace("{i}", str(i)).replace("{port}", str(port0 + i))
        log = open(inst / "run.log", "wb")
        procs.append((i, subprocess.Popen(["cmd", "/c", str(inst / "run.bat")], cwd=inst, env=env,
                                          stdout=log, stderr=subprocess.STDOUT), log))
        time.sleep(a.stagger)
    t0 = time.time()
    crashed = False
    want_crash = bool(a.crash_at or a.crash_when)
    crash_rx = re.compile(a.crash_when) if a.crash_when else None
    until = [re.compile(u) for u in a.until or []]
    marks = [0] * a.count  # log offsets at the crash: --until then looks at what follows it
    verdict = None

    def log_text(i, start=0):
        try:
            with open(Path(a.root) / f"inst{i}" / "run.log", "rb") as f:
                f.seek(start)
                return f.read().decode("utf-8", errors="replace")
        except OSError:
            return ""

    while any(p.poll() is None for _, p, _ in procs) and time.time() - t0 < a.seconds + 90:
        if want_crash and not crashed:
            hit = (a.crash_at and time.time() - t0 >= a.crash_at) or (crash_rx and crash_rx.search(log_text(a.crash_instance)))
            if hit:
                crashed = True
                marks = [os.path.getsize(Path(a.root) / f"inst{i}" / "run.log") for i in range(a.count)]
                crash_instance(a.root, a.crash_instance)
        if until and (crashed or not want_crash):
            text = chr(10).join(log_text(i, marks[i]) for i in range(a.count))
            if all(u.search(text) for u in until):
                verdict = "all --until markers seen after %.0f s" % (time.time() - t0)
                break
        time.sleep(1)
    if until:
        print("verdict:", verdict or "TIMED OUT waiting for --until markers", flush=True)
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


def bbport_pids(root):
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process bbport*,bb-probe* -ErrorAction SilentlyContinue | "
                          "ForEach-Object { $_.Id.ToString() + '|' + $_.Path }"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, path = line.partition("|")
        if path.lower().startswith(str(Path(root)).lower()):
            yield pid, path


def crash_instance(root, i):
    """Kills instance i's game process only (not run.bat): a crash, as far as run.bat can tell."""
    want = str(Path(root) / f"inst{i}").lower() + os.sep
    for pid, path in bbport_pids(root):
        if path.lower().startswith(want):
            subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True)
            print(f"crashed instance {i}: killed {pid} {path}", flush=True)


def crash(a):
    time.sleep(a.after)
    crash_instance(a.root, a.instance)


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
    ap.add_argument("cmd", choices=["setup", "run", "logs", "kill", "crash"])
    ap.add_argument("--count", type=int, default=2)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=str(DEFAULT_GAME))
    ap.add_argument("--saves", default=str(DEFAULT_GAME / "user" / "savedata"))
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--vram", type=int, default=3072)
    ap.add_argument("--fps", default="30", help="frame cap of the test instances (30, 60, 90, uncap)")
    ap.add_argument("--stagger", type=float, default=8.0)
    ap.add_argument("--party", action="store_true", help="instance 0 hosts a loopback party, the others join")
    ap.add_argument("--crash-at", type=float, default=0, help="run: kill one instance's game this many s in")
    ap.add_argument("--crash-instance", type=int, default=1)
    ap.add_argument("--crash-when", help="run: kill that instance's game once its log matches this regex")
    ap.add_argument("--until", action="append",
                    help="run: stop (kill all) once every such regex matched in the logs (after the crash, if any)")
    ap.add_argument("--instance", type=int, default=1, help="crash: which instance")
    ap.add_argument("--after", type=float, default=0, help="crash: wait this many s first")
    ap.add_argument("--env", action="append")
    ap.add_argument("--grep")
    a = ap.parse_args()
    {"setup": setup, "run": run, "logs": logs, "kill": kill, "crash": crash}[a.cmd](a)


if __name__ == "__main__":
    main()
