#!/usr/bin/env python3
"""Stale bbport.exe guard (Windows).

An in-game restart (probe.c runtime_restart) or a crash can leave the old bbport.exe alive: it
keeps gigabytes of RAM and VRAM, and starting the game on top of it runs the machine out of
memory. This lists the bbport.exe processes started from this install and terminates them.

Library (launcher.py): find_instances(exe_dir), terminate(pid), wait_gone(pids, timeout).
Command line (run.bat): python scripts/bb_guard.py --exe-dir out --wait 10
It never fails the launch: a problem in the guard is reported and run.bat goes on.
"""
import argparse
import os
import sys
import time
from collections import namedtuple
from pathlib import Path

PROCESS_NAMES = ('bbport.exe',)

Instance = namedtuple('Instance', 'pid path private_bytes started')

if os.name == 'nt':
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _psapi = ctypes.WinDLL('psapi', use_last_error=True)

    TH32CS_SNAPPROCESS = 0x2
    PROCESS_TERMINATE = 0x0001
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    SYNCHRONIZE = 0x00100000
    WAIT_TIMEOUT = 0x102
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

    class _ProcessEntry(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
                    ('th32ProcessID', wintypes.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                    ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                    ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG),
                    ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]

    class _MemoryCounters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD),
                    ('PeakWorkingSetSize', ctypes.c_size_t), ('WorkingSetSize', ctypes.c_size_t),
                    ('QuotaPeakPagedPoolUsage', ctypes.c_size_t), ('QuotaPagedPoolUsage', ctypes.c_size_t),
                    ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t), ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                    ('PagefileUsage', ctypes.c_size_t), ('PeakPagefileUsage', ctypes.c_size_t),
                    ('PrivateUsage', ctypes.c_size_t)]

    _k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]
    _k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]
    _k32.OpenProcess.restype = wintypes.HANDLE
    _k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                ctypes.POINTER(wintypes.DWORD)]
    _k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    _k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _k32.WaitForSingleObject.restype = wintypes.DWORD
    _k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(_MemoryCounters), wintypes.DWORD]

    def _processes_named(names):
        """(pid, exe name) of every running process whose image name is in names."""
        snapshot = _k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if not snapshot or snapshot == INVALID_HANDLE_VALUE:
            raise OSError(ctypes.get_last_error(), 'CreateToolhelp32Snapshot failed')
        found = []
        try:
            entry = _ProcessEntry()
            entry.dwSize = ctypes.sizeof(_ProcessEntry)
            ok = _k32.Process32FirstW(snapshot, ctypes.byref(entry))
            while ok:
                if entry.szExeFile.lower() in names:
                    found.append((entry.th32ProcessID, entry.szExeFile))
                ok = _k32.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            _k32.CloseHandle(snapshot)
        return found

    def _details(pid):
        """(full image path, private bytes, start time as epoch seconds), None where unreadable."""
        handle = _k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return None, None, None
        try:
            size = wintypes.DWORD(32768)
            buf = ctypes.create_unicode_buffer(size.value)
            path = buf.value if _k32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)) else None
            counters = _MemoryCounters()
            counters.cb = ctypes.sizeof(_MemoryCounters)
            private = counters.PrivateUsage if _psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb) else None
            created, _exit, _kernel, _user = (wintypes.FILETIME() for _ in range(4))
            started = None
            if _k32.GetProcessTimes(handle, *(ctypes.byref(t) for t in (created, _exit, _kernel, _user))):
                ticks = (created.dwHighDateTime << 32) | created.dwLowDateTime  # 100 ns since 1601
                started = ticks / 1e7 - 11644473600
            return path, private, started
        finally:
            _k32.CloseHandle(handle)

    def is_alive(pid):
        handle = _k32.OpenProcess(SYNCHRONIZE, False, pid)
        if not handle:
            return False  # gone (or never ours to see)
        try:
            return _k32.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT
        finally:
            _k32.CloseHandle(handle)

    def terminate(pid, timeout=5.0):
        """Terminate pid and wait for it to disappear. True when it is gone."""
        handle = _k32.OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, False, pid)
        if not handle:
            return not is_alive(pid)
        try:
            _k32.TerminateProcess(handle, 1)
            return _k32.WaitForSingleObject(handle, int(timeout * 1000)) != WAIT_TIMEOUT
        finally:
            _k32.CloseHandle(handle)
else:
    def _processes_named(names):
        return []

    def _details(pid):
        return None, None, None

    def is_alive(pid):
        return False

    def terminate(pid, timeout=5.0):
        return True


def _same_dir(path, exe_dir):
    return os.path.normcase(os.path.dirname(os.path.abspath(path))) == os.path.normcase(os.path.abspath(exe_dir))


def find_instances(exe_dir):
    """The bbport.exe processes running from exe_dir (this install's out folder)."""
    found = []
    for pid, _name in _processes_named(PROCESS_NAMES):
        path, private, started = _details(pid)
        if path and _same_dir(path, exe_dir):
            found.append(Instance(pid, path, private, started))
    return found


def wait_gone(pids, timeout):
    """Wait up to timeout seconds for the pids to exit on their own. Returns those still alive."""
    deadline = time.monotonic() + timeout
    alive = [pid for pid in pids if is_alive(pid)]
    while alive and time.monotonic() < deadline:
        time.sleep(0.25)
        alive = [pid for pid in alive if is_alive(pid)]
    return alive


def describe(instance):
    """One line for a message: PID, memory and how long it has been running."""
    parts = [f'PID {instance.pid}']
    if instance.private_bytes:
        parts.append(f'{instance.private_bytes / 2**30:.1f} GB')
    if instance.started:
        parts.append('started ' + time.strftime('%H:%M:%S', time.localtime(instance.started)))
    return ', '.join(parts)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--exe-dir', default='out', help='folder holding bbport.exe (default: out)')
    parser.add_argument('--wait', type=float, default=10.0,
                        help='seconds an old instance gets to exit on its own before it is terminated')
    parser.add_argument('--no-kill', action='store_true', help='only report, terminate nothing')
    args = parser.parse_args(argv)
    try:
        instances = find_instances(args.exe_dir)
        if not instances:
            return 0
        if args.no_kill:
            for instance in instances:
                print(f'Guard: bbport.exe is still running ({describe(instance)})', flush=True)
            return 0
        # After an in-game restart the old process is normally still exiting: say how long
        # it took, so a slow or stuck exit shows up in launcher.log.
        start = time.monotonic()
        left = wait_gone([i.pid for i in instances], args.wait)
        waited = time.monotonic() - start
        for instance in instances:
            if instance.pid not in left:
                print(f'Guard: previous bbport.exe ({describe(instance)}) exited after {waited:.1f} s',
                      flush=True)
            elif terminate(instance.pid):
                print(f'Guard: terminated stale bbport.exe PID {instance.pid} '
                      f'({describe(instance)}): still running after {waited:.0f} s', flush=True)
            else:
                print(f'Guard: could not terminate PID {instance.pid} '
                      '(stuck in the GPU driver? reboot may be needed)', flush=True)
    except Exception as ex:  # a guard problem must never stop the launch
        print(f'Guard: skipped ({ex})', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
