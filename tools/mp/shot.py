#!/usr/bin/env python3
"""Screenshots of the test instances' game windows (PrintWindow, works for background windows).

  python tools/mp/shot.py --root DIR --out DIR [--tag NAME]
      one PNG per bbport.exe window whose process runs from DIR\\inst*\\out
  python tools/mp/shot.py --root DIR --out DIR --watch "regex=tag[@delay[,delay...]]" ...
      tails DIR\\inst*\\run.log and shoots every window when a line matches regex
      (after each delay in s; default 0), until --seconds pass.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, time
from pathlib import Path

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

# Per-monitor DPI aware: otherwise GetClientRect reports the scaled (logical) size of the game's
# window while PrintWindow draws physical pixels, so on a 150 % / 200 % display only the top-left
# part of the frame was saved (the bottom-centre item-get box and the top-right Insight / echoes
# counter were never in the shots).
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2
except (AttributeError, OSError):
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        user32.SetProcessDPIAware()


def proc_path(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(1024)
    n = wt.DWORD(1024)
    ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    kernel32.CloseHandle(h)
    return buf.value if ok else ""


def windows(root):
    root = str(Path(root).resolve()).lower()
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        p = proc_path(pid.value).lower()
        if p.endswith("bbport.exe") and p.startswith(root):
            m = re.search(r"inst(\d+)", p)
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            if r.right > 100:
                found.append((int(m.group(1)) if m else -1, hwnd))
        return True

    user32.EnumWindows(cb, 0)
    return sorted(found)


def grab(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    buf = ctypes.create_string_buffer(w * h * 4)
    bmi = (ctypes.c_byte * 44)()
    ctypes.memmove(bmi, ctypes.byref(ctypes.c_uint32(40)), 4)
    ctypes.memmove(ctypes.byref(bmi, 4), ctypes.byref(ctypes.c_int32(w)), 4)
    ctypes.memmove(ctypes.byref(bmi, 8), ctypes.byref(ctypes.c_int32(-h)), 4)
    ctypes.memmove(ctypes.byref(bmi, 12), ctypes.byref(ctypes.c_uint16(1)), 2)
    ctypes.memmove(ctypes.byref(bmi, 14), ctypes.byref(ctypes.c_uint16(32)), 2)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, bmi, 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    return Image.frombytes("RGBA", (w, h), buf.raw, "raw", "BGRA").convert("RGB")


def shoot(root, out, tag):
    out.mkdir(parents=True, exist_ok=True)
    names = []
    for inst, hwnd in windows(root):
        p = out / f"{tag}_inst{inst}.png"
        grab(hwnd).save(p)
        names.append(p.name)
    print("shot", tag, names, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default="shot")
    ap.add_argument("--watch", action="append", default=[])
    ap.add_argument("--seconds", type=float, default=600)
    a = ap.parse_args()
    out = Path(a.out)
    if not a.watch:
        shoot(a.root, out, a.tag)
        return
    rules = []
    for w in a.watch:
        rx, _, rest = w.rpartition("=")
        tag, _, d = rest.partition("@")
        rules.append((re.compile(rx), tag, [float(x) for x in d.split(",")] if d else [0.0]))
    logs = sorted(Path(a.root).glob("inst*/run.log"))
    pos = {p: (p.stat().st_size if p.exists() else 0) for p in logs}
    fired, pending, t0 = set(), [], time.time()
    while time.time() - t0 < a.seconds:
        for p in logs:
            if not p.exists():
                continue
            size = p.stat().st_size
            if size < pos.get(p, 0):
                pos[p] = 0
            with open(p, "rb") as f:
                f.seek(pos.get(p, 0))
                text = f.read().decode("utf-8", "replace")
                pos[p] = size
            for rx, tag, delays in rules:
                if tag not in fired and rx.search(text):
                    fired.add(tag)
                    for d in delays:
                        pending.append((time.time() + d, f"{tag}_{d:g}s"))
        now = time.time()
        for item in sorted(pending):
            if item[0] <= now:
                shoot(a.root, out, item[1])
                pending.remove(item)
        if len(fired) == len(rules) and not pending:
            return
        time.sleep(0.2)


if __name__ == "__main__":
    main()
