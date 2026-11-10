#!/usr/bin/env python3
"""Handle counts by kernel object TYPE for every bbport.exe under ROOT (leak hunt, 0.8.1).

  python tools/mp/handle_types.py --root DIR --out types.csv [--every 20] [--seconds 600]

One CSV row per sample and instance: elapsed_s, inst, pid, private_mb, handles, then one column per type.
Uses NtQuerySystemInformation(SystemExtendedHandleInformation) and no handle duplication; type index
-> name is learned by opening one object of each kind in this process (index is system wide).
"""
import argparse, csv, ctypes, re, sys, time
from ctypes import wintypes as W
from pathlib import Path

import psutil

ntdll = ctypes.WinDLL("ntdll")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = W.HANDLE
k32.CreateEventW.restype = W.HANDLE
k32.CreateSemaphoreW.restype = W.HANDLE
k32.CreateMutexW.restype = W.HANDLE
k32.CreateWaitableTimerW.restype = W.HANDLE
k32.CreateFileMappingW.restype = W.HANDLE
k32.CreateFileW.restype = W.HANDLE
k32.GetCurrentThread.restype = W.HANDLE
k32.GetCurrentProcess.restype = W.HANDLE
k32.DuplicateHandle.argtypes = [W.HANDLE, W.HANDLE, W.HANDLE, ctypes.POINTER(W.HANDLE), W.DWORD, W.BOOL, W.DWORD]


class Entry(ctypes.Structure):
    _fields_ = [("Object", ctypes.c_void_p), ("Pid", ctypes.c_size_t), ("Handle", ctypes.c_size_t),
                ("Access", ctypes.c_uint32), ("Creator", ctypes.c_uint16), ("TypeIndex", ctypes.c_uint16),
                ("Attr", ctypes.c_uint32), ("Res", ctypes.c_uint32)]


def snapshot():
    size = 1 << 24
    while True:
        buf = ctypes.create_string_buffer(size)
        ret = W.ULONG(0)
        st = ntdll.NtQuerySystemInformation(0x40, buf, size, ctypes.byref(ret))
        if st & 0xFFFFFFFF == 0xC0000004:
            size *= 2
            continue
        if st != 0:
            raise OSError("NtQuerySystemInformation %x" % (st & 0xFFFFFFFF))
        n = ctypes.c_size_t.from_buffer(buf, 0).value
        arr = (Entry * n).from_buffer(buf, 16)
        return buf, arr


def learn_types():
    """type index -> name for the common kinds, by creating one of each here."""
    objs = {}
    objs["Event"] = k32.CreateEventW(None, 0, 0, None)
    objs["Semaphore"] = k32.CreateSemaphoreW(None, 0, 1, None)
    objs["Mutant"] = k32.CreateMutexW(None, 0, None)
    objs["Timer"] = k32.CreateWaitableTimerW(None, 0, None)
    objs["Section"] = k32.CreateFileMappingW(W.HANDLE(-1), None, 4, 0, 4096, None)
    th = W.HANDLE()
    k32.DuplicateHandle(k32.GetCurrentProcess(), k32.GetCurrentThread(), k32.GetCurrentProcess(), ctypes.byref(th), 0, False, 2)
    objs["Thread"] = th.value
    objs["Process"] = k32.OpenProcess(0x400, False, k32.GetCurrentProcessId())
    objs["File"] = k32.CreateFileW(sys.executable, 0x80000000, 7, None, 3, 0, None)
    me = k32.GetCurrentProcess  # noqa
    import os
    mypid = os.getpid()
    buf, arr = snapshot()
    byh = {e.Handle: e.TypeIndex for e in arr if e.Pid == mypid}
    names = {}
    for name, h in objs.items():
        if h and int(h) in byh:
            names[byh[int(h)]] = name
    return names


def resolve_unknown(pid, arr, names):
    """Names of type indexes seen in process pid that are still unknown: duplicate one handle, NtQueryObject(type info)."""
    todo = {}
    for e in arr:
        if e.Pid == pid and e.TypeIndex not in names and e.TypeIndex not in todo:
            todo[e.TypeIndex] = e.Handle
    if not todo:
        return
    hp = k32.OpenProcess(0x40, False, pid)  # PROCESS_DUP_HANDLE
    if not hp:
        return
    for idx, h in todo.items():
        dup = W.HANDLE()
        if not k32.DuplicateHandle(hp, W.HANDLE(h), k32.GetCurrentProcess(), ctypes.byref(dup), 0, False, 2):
            names[idx] = "T%d" % idx
            continue
        buf = ctypes.create_string_buffer(1024)
        ret = W.ULONG(0)
        if ntdll.NtQueryObject(dup, 2, buf, 1024, ctypes.byref(ret)) == 0:
            ln = ctypes.c_ushort.from_buffer(buf, 0).value
            ptr = ctypes.c_void_p.from_buffer(buf, 8).value
            names[idx] = ctypes.wstring_at(ptr, ln // 2) if ln else "T%d" % idx
        else:
            names[idx] = "T%d" % idx
        k32.CloseHandle(dup)
    k32.CloseHandle(hp)


def games(root):
    want = str(Path(root).resolve()).lower()
    for p in psutil.process_iter(["pid", "name", "exe"]):
        try:
            exe = (p.info["exe"] or "").lower()
            if p.info["name"] and p.info["name"].lower() == "bbport.exe" and exe.startswith(want):
                m = re.search(r"inst(\d+)", exe)
                yield (int(m.group(1)) if m else 0), p
        except (psutil.Error, OSError):
            continue


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--every", type=float, default=20)
    ap.add_argument("--seconds", type=float, default=600)
    a = ap.parse_args()
    names = learn_types()
    t0 = time.time()
    cols = None
    out = Path(a.out)
    new = not out.exists()
    with open(out, "a", newline="") as f:
        w = csv.writer(f)
        while time.time() - t0 < a.seconds:
            procs = {p.pid: (i, p) for i, p in games(a.root)}
            if procs:
                _, arr = snapshot()
                cnt = {}
                for e in arr:
                    if e.Pid in procs:
                        d = cnt.setdefault(e.Pid, {})
                        n = names.get(e.TypeIndex, "T%d" % e.TypeIndex)
                        d[n] = d.get(n, 0) + 1
                for pid, (i, pr) in procs.items():
                    resolve_unknown(pid, arr, names)
                cnt = {}
                for e in arr:
                    if e.Pid in procs:
                        d = cnt.setdefault(e.Pid, {})
                        n = names.get(e.TypeIndex, "T%d" % e.TypeIndex)
                        d[n] = d.get(n, 0) + 1
                allnames = sorted({n for d in cnt.values() for n in d})
                if cols is None:
                    cols = ["Event", "Thread", "Section", "File", "Semaphore", "Mutant", "Timer", "Process"]
                    cols += [n for n in allnames if n not in cols]
                    if new:
                        w.writerow(["elapsed_s", "inst", "pid", "private_mb", "handles"] + cols)
                for pid, (i, p) in procs.items():
                    d = cnt.get(pid, {})
                    try:
                        priv = p.memory_full_info().private / 1048576
                    except psutil.Error:
                        priv = 0
                    w.writerow(["%.0f" % (time.time() - t0), i, pid, "%.0f" % priv, sum(d.values())] + [d.get(c, 0) for c in cols])
                f.flush()
            time.sleep(a.every)


if __name__ == "__main__":
    main()
