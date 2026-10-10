#!/usr/bin/env python3
"""Machine-wide limit on running game clients, shared by every agent's harness runs.

The state is one JSON file (default E:\\games\\bbport\\agents\\game_queue.json, BB_GAME_QUEUE_FILE),
edited under an exclusive lock file next to it:

  {"max_clients": 2,
   "holders": [{"id", "pid", "count", "root", "since"}],   # runs whose games are up
   "queue":   [{"id", "pid", "count", "root", "since"}]}   # waiting, first come first served

A run needing `count` clients waits until it is first in the queue and the holders' clients plus
`count` fit in max_clients. Entries whose harness process is gone are dropped, so a killed run
never blocks the queue. max_clients is read from the file each time (edit it to change the limit;
BB_GAME_QUEUE_MAX sets it when the file is created).

  python tools/mp/game_queue.py status      # who runs, who waits
  python tools/mp/game_queue.py clear-dead  # drop entries of dead processes now
"""
import contextlib, json, os, sys, time
from pathlib import Path

QUEUE_FILE = Path(os.environ.get("BB_GAME_QUEUE_FILE", r"E:\games\bbport\agents\game_queue.json"))
DEFAULT_MAX = int(os.environ.get("BB_GAME_QUEUE_MAX", "2"))


def _alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = k.GetExitCodeProcess(h, ctypes.byref(code))
        k.CloseHandle(h)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


@contextlib.contextmanager
def _locked():
    QUEUE_FILE.parent.mkdir(parents=True, exist_ok=True)
    lock = QUEUE_FILE.with_suffix(".lock")
    t0 = time.time()
    while True:
        try:
            fd = os.open(str(lock), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            break
        except FileExistsError:
            # A lock older than 30 s belongs to a process that died mid-edit.
            try:
                if time.time() - lock.stat().st_mtime > 30:
                    lock.unlink()
                    continue
            except OSError:
                pass
            if time.time() - t0 > 120:
                raise SystemExit(f"game queue: cannot lock {lock}")
            time.sleep(0.1)
    try:
        try:
            state = json.loads(QUEUE_FILE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            state = {}
        state.setdefault("max_clients", DEFAULT_MAX)
        state.setdefault("holders", [])
        state.setdefault("queue", [])
        yield state
        tmp = QUEUE_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, indent=1), encoding="utf-8")
        os.replace(tmp, QUEUE_FILE)
    finally:
        try:
            lock.unlink()
        except OSError:
            pass


def _prune(state):
    state["holders"] = [e for e in state["holders"] if _alive(e["pid"])]
    state["queue"] = [e for e in state["queue"] if _alive(e["pid"])]


@contextlib.contextmanager
def game_slots(count: int, root: str):
    """Hold `count` client slots for the duration of the block (waits its turn)."""
    me = {"id": f"{os.getpid()}-{time.time():.0f}", "pid": os.getpid(), "count": count, "root": str(root),
          "since": time.strftime("%H:%M:%S")}
    with _locked() as st:
        _prune(st)
        if count > st["max_clients"]:
            raise SystemExit(f"game queue: this run needs {count} clients but the machine-wide limit is "
                             f"{st['max_clients']} ({QUEUE_FILE}: max_clients)")
        st["queue"].append(me)
    last_note = 0.0
    try:
        while True:
            with _locked() as st:
                _prune(st)
                used = sum(e["count"] for e in st["holders"])
                first = st["queue"] and st["queue"][0]["id"] == me["id"]
                if first and used + count <= st["max_clients"]:
                    st["queue"] = [e for e in st["queue"] if e["id"] != me["id"]]
                    me["since"] = time.strftime("%H:%M:%S")
                    st["holders"].append(me)
                    break
                pos = next((i for i, e in enumerate(st["queue"]) if e["id"] == me["id"]), -1)
                busy = ", ".join(f"{e['root']} ({e['count']})" for e in st["holders"])
            if time.time() - last_note > 30:
                print(f"game queue: waiting (position {pos + 1}, {used}/{st['max_clients']} clients in use: {busy})",
                      flush=True)
                last_note = time.time()
            time.sleep(2)
        print(f"game queue: got {count} client slot(s)", flush=True)
        yield
    finally:
        with _locked() as st:
            st["holders"] = [e for e in st["holders"] if e["id"] != me["id"]]
            st["queue"] = [e for e in st["queue"] if e["id"] != me["id"]]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    with _locked() as st:
        if cmd == "clear-dead":
            _prune(st)
        print(f"max_clients {st['max_clients']}  ({QUEUE_FILE})")
        for e in st["holders"]:
            print(f"  running  {e['count']}  {e['root']}  since {e['since']}  pid {e['pid']}"
                  f"{'' if _alive(e['pid']) else '  (dead)'}")
        for i, e in enumerate(st["queue"]):
            print(f"  wait #{i + 1} {e['count']}  {e['root']}  since {e['since']}  pid {e['pid']}"
                  f"{'' if _alive(e['pid']) else '  (dead)'}")


if __name__ == "__main__":
    main()
