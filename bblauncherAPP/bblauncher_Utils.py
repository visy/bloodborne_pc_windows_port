#!/usr/bin/env python3
"""Utility functions, constants, and batched background worker for Bloodborne PC Launcher."""

import ctypes
import hashlib
import locale
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from PySide6.QtCore import QThread, Signal

# Dynamic path resolution: works correctly whether run via python or as a compiled .exe
if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).resolve().parent
else:
    APP_DIR = Path(__file__).resolve().parent

if not (APP_DIR / "run.bat").exists() and (APP_DIR.parent / "run.bat").exists():
    APP_DIR = APP_DIR.parent

RUN_BAT = APP_DIR / "run.bat"
LOG_FILE = APP_DIR / "bblauncher.log"
LOG_DIR = APP_DIR / "logs"
LOG_KEEP = 20
SETTINGS_FILE = APP_DIR / "bblauncher_settings.json"
PATCHES_XML = APP_DIR / "patches" / "Bloodborne.xml"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", "../CUSA03173"))

FPS_CHOICES = ["uncap", "60", "90", "30"]
RES_CHOICES = ["1920x1080 (Native)", "1280x720", "2560x1440", "3840x2160"]
WINDOW_MODE_CHOICES = ["Windowed", "Borderless", "Fullscreen"]
ANISO_CHOICES = [
    ("16x", "16", "16x anisotropic filtering of scene textures (recommended)"),
    ("8x", "8", "8x anisotropic filtering"),
    ("4x", "4", "4x anisotropic filtering"),
    ("2x", "2", "2x anisotropic filtering"),
    ("Off", "0", "Game's default anisotropic filtering (no override)"),
]
VK_NOISE = "<Warning> vk_instance.cpp"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

DEFAULT_SETTINGS = {
    "eboot": "",
    "fps": "60",
    "res": RES_CHOICES[0],
    "aniso": "16x",
    "mods": False,
    "hide_vk": True,
    "feat_upscaler": False,
    "feat_overlay": True,
    "feat_hud": False,
    "feat_fps_patch": True,
    "feat_res_scaling": True,
    "window_mode": "Borderless",
    "fullscreen": False,
    "enabled_patches": ["Skip Intro"],
}

DEFAULT_KEYBINDS = [
    ("key.attack", "Light Attack (R1)", "Mouse Left"),
    ("key.heavy_attack", "Heavy Attack (R2)", "Shift + Mouse Left"),
    ("key.firearm", "Firearm / Gun (L2)", "Mouse Right"),
    ("key.transform", "Weapon Transform (L1)", "Tab"),
    ("key.lock_on", "Lock-On / Camera Reset (R3)", "Mouse Middle"),
    ("key.move_forward", "Move Forward", "W"),
    ("key.move_backward", "Move Backward", "S"),
    ("key.move_left", "Move Left", "A"),
    ("key.move_right", "Move Right", "D"),
    ("key.dodge", "Dodge / Roll / Sprint (Circle)", "Space"),
    ("key.interact", "Interact / Talk (Cross)", "E"),
    ("key.use_item", "Use Item (Square)", "R"),
    ("key.heal", "Heal / Blood Vial (Triangle)", "F"),
    ("key.dpad_up", "Blood Bullets (Up)", "1"),
    ("key.dpad_down", "Switch Item (Down)", "2"),
    ("key.dpad_left", "Switch Left Weapon (Left)", "3"),
    ("key.dpad_right", "Switch Right Weapon (Right)", "4"),
    ("key.gestures", "Gestures Menu (Touchpad L)", "G"),
    ("key.personal_effects", "Personal Effects (Touchpad R)", "Backspace"),
    ("key.menu", "System Menu (Options)", "Escape"),
]

KEYBINDINGS = [(k, desc) for _, desc, k in DEFAULT_KEYBINDS]

_SYS_ENCODING = locale.getpreferredencoding(False)
_instance_mutex = []
_bb_guard_mod = False


def get_bb_guard():
    global _bb_guard_mod
    if _bb_guard_mod is False:
        try:
            scripts_dir = str(APP_DIR / "scripts")
            if scripts_dir not in sys.path:
                sys.path.insert(0, scripts_dir)
            import bb_guard
            _bb_guard_mod = bb_guard
        except Exception:
            _bb_guard_mod = None
    return _bb_guard_mod


def acquire_single_instance() -> bool:
    if os.name != "nt":
        return True
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        name = "Local\\BloodborneLauncher_" + hashlib.md5(str(APP_DIR).lower().encode()).hexdigest()[:12]
        handle = kernel32.CreateMutexW(None, False, name)
        if handle and ctypes.get_last_error() == 183:
            return False
        _instance_mutex.append(handle)
    except Exception:
        pass
    return True


def decode_line(raw: bytes) -> str:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(_SYS_ENCODING, errors="replace")


def shader_cache_dir() -> Path:
    user = Path(os.environ.get("BB_USER_DIR") or "user")
    return (user if user.is_absolute() else APP_DIR / user) / "cache"


def resolve_game_dir(target: str):
    p = Path(target.strip().strip('"'))
    if p.is_file() and p.name.lower() == "eboot.bin":
        return p.parent
    if p.is_dir() and (p / "eboot.bin").is_file():
        return p
    return None


def running_instances():
    guard = get_bb_guard()
    if guard is None:
        return []
    try:
        return guard.find_instances(APP_DIR / "out")
    except Exception:
        return []


def kill_tree(proc: subprocess.Popen):
    guard = get_bb_guard()
    if guard:
        for instance in running_instances():
            guard.terminate(instance.pid)

    if proc and proc.poll() is None:
        try:
            proc.terminate()
            try:
                proc.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                proc.kill()
        except OSError:
            pass


def rotate_log():
    try:
        if LOG_FILE.is_file() and LOG_FILE.stat().st_size > 0:
            LOG_DIR.mkdir(exist_ok=True)
            stamp = datetime.fromtimestamp(LOG_FILE.stat().st_mtime).strftime("%Y%m%d_%H%M%S")
            LOG_FILE.replace(LOG_DIR / f"bblauncher_{stamp}.log")
            for old in sorted(LOG_DIR.glob("bblauncher_*.log"))[:-LOG_KEEP]:
                old.unlink()
    except OSError:
        pass


def load_xml_patches(xml_path: Path = PATCHES_XML):
    import xml.etree.ElementTree as ET

    patches = []
    if not xml_path.is_file():
        return patches
    try:
        tree = ET.parse(xml_path)
        for meta in tree.getroot().iter("Metadata"):
            if meta.get("AppVer") == "01.09" and meta.get("AppElf", "eboot.bin") == "eboot.bin":
                name = meta.get("Name", "").strip()
                if not name:
                    continue
                author = meta.get("Author", "").strip() or "Unknown"
                note = meta.get("Note", "").strip().replace("\\n", "\n")
                n = name.lower()
                if "resolution patch" in n or "light grid" in n or "optimal 1080p" in n:
                    cat = "Resolutions & Grids"
                elif "fps" in n:
                    cat = "Framerate & Engine"
                elif any(k in n for k in ["debug", "capture", "http"]):
                    cat = "Debug & Tools"
                elif any(k in n for k in ["stealth", "silent", "no dead", "rally", "cheat", "sensitive analog", "unlock game region", "enemy control"]):
                    cat = "Gameplay & Cheats"
                elif any(k in n for k in ["blur", "shadow", "dof", "ssao", "chromatic", "camera", "reflections", "text scale", "physics", "aa"]):
                    cat = "Visuals & Camera"
                else:
                    cat = "Performance & Fixes"

                patches.append({
                    "name": name,
                    "author": author,
                    "note": note,
                    "category": cat,
                })
    except Exception as e:
        print(f"Error loading patches XML: {e}")
    return patches


def verify_prelaunch(game_dir: Path) -> tuple[bool, str, list[str]]:
    details = []

    eboot_path = game_dir / "eboot.bin"
    if not eboot_path.is_file():
        return False, f"Missing eboot.bin in game directory:\n{game_dir}", details
    eboot_size = eboot_path.stat().st_size
    if eboot_size == 0:
        return False, f"eboot.bin is empty in:\n{eboot_path}", details
    details.append(f"[OK] Game executable: eboot.bin ({eboot_size / (1024*1024):.1f} MiB)")

    try:
        scripts_dir = str(APP_DIR / "scripts")
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        import game_check
        chk = game_check.problem(str(game_dir))
        if chk:
            kind, title, version = chk
            explanation = game_check.explain(kind, title, version)
            return False, f"Game File Verification Failed ({kind}):\n\n{explanation}", details
        details.append("[OK] Game Title & Version: Bloodborne CUSA03173 v1.09 verified")
    except Exception:
        details.append("[NOTE] Version check bypassed (standalone)")

    dvdroot = game_dir / "dvdroot_ps4"
    if dvdroot.is_dir():
        details.append("[OK] Asset directory: dvdroot_ps4 detected")

    if not RUN_BAT.exists():
        return False, f"Launcher script run.bat not found in:\n{APP_DIR}", details
    details.append("[OK] Launcher script: run.bat ready")

    bbport_exe = APP_DIR / "out" / "bbport.exe"
    if not bbport_exe.is_file():
        return False, f"Compiled binary bbport.exe missing at:\n{bbport_exe}\nRun build.bat first.", details
    details.append(f"[OK] Compiled binary: bbport.exe ({bbport_exe.stat().st_size / (1024*1024):.1f} MiB)")

    try:
        vk = ctypes.CDLL("vulkan-1.dll")
        details.append("[OK] Vulkan runtime: vulkan-1.dll loaded")
        if os.name == "nt" and hasattr(vk, "_handle") and vk._handle:
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(vk._handle))
    except OSError:
        details.append("[WARN] vulkan-1.dll not detected in system path")

    return True, "", details


class ProcessWorker(QThread):
    batch_emitted = Signal(list)
    process_finished = Signal(int)

    def __init__(self, cmd, env, cwd):
        super().__init__()
        self.cmd = cmd
        self.env = env
        self.cwd = cwd
        self.proc = None

    def run(self):
        try:
            self.proc = subprocess.Popen(
                self.cmd,
                cwd=str(self.cwd),
                env=self.env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                creationflags=NO_WINDOW,
            )
        except Exception as ex:
            self.batch_emitted.emit([f"Launch Error: {ex}"])
            self.process_finished.emit(-1)
            return

        batch = []
        last_flush = time.monotonic()

        try:
            for raw in iter(self.proc.stdout.readline, b""):
                batch.append(decode_line(raw).rstrip())
                now = time.monotonic()
                if len(batch) >= 60 or (now - last_flush) >= 0.035:
                    self.batch_emitted.emit(batch)
                    batch = []
                    last_flush = now
            if batch:
                self.batch_emitted.emit(batch)
        except Exception as ex:
            self.batch_emitted.emit([f"Read error: {ex}"])
        finally:
            try:
                self.proc.stdout.close()
            except OSError:
                pass
            rc = self.proc.wait()
            self.process_finished.emit(rc)

    def terminate_proc(self):
        if self.proc and self.proc.poll() is None:
            kill_tree(self.proc)