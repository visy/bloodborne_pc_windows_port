#!/usr/bin/env python3
"""Bloodborne PC (Windows) Launcher GUI."""

import json
import locale
import os
import queue
import re
import subprocess
import sys
import threading
import tkinter as tk
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
from tkinter import ttk, filedialog, messagebox

APP_DIR = Path(__file__).resolve().parent
RUN_BAT = APP_DIR / "run.bat"
LOG_FILE = APP_DIR / "launcher.log"
SETTINGS_FILE = APP_DIR / "launcher_settings.json"
PATCHES_XML = APP_DIR / "patches" / "Bloodborne.xml"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", "../CUSA03173"))

FPS_CHOICES = ["uncap", "60", "90", "30"]
RES_CHOICES = ["From in-game menu", "1280x720", "1920x1080", "2560x1440", "3840x2160"]
# Earlier name of RES_CHOICES[0] in saved settings
LEGACY_RES_DEFAULT = "Default (1080p)"
TIMEOUT_CHOICES = ["0 (no limit)", "10", "30", "60", "120", "300", "600"]
ANISO_CHOICES = [
    ("16x", "16", "16x anisotropic filtering of scene textures (recommended)"),
    ("8x", "8", "8x anisotropic filtering"),
    ("4x", "4", "4x anisotropic filtering"),
    ("2x", "2", "2x anisotropic filtering"),
    ("Off", "0", "Game's default anisotropic filtering (no override)"),
]
# (label, BB_PREUPLOAD value); "" keeps run.bat's default (1)
PREUPLOAD_CHOICES = [("Normal", ""), ("Full (~3 GB more VRAM, 12 GB+ GPU)", "2"), ("Off", "0")]
# (label, BB_FRAMES_AHEAD value): guest frames the GPU thread may run ahead of the display
FRAMES_AHEAD_CHOICES = [("2 (smooth, default)", "2"), ("1 (lowest input lag)", "1"), ("3 (smoothest)", "3")]
DEFAULT_DISPLAY = "Primary (default)"
DEFAULT_GAMEPAD = "Any (default)"
GPU_CAPS = APP_DIR / "out" / "bb-gpu-capabilities.exe"
MAX_LOG_LINES = 5000
VK_NOISE = "<Warning> vk_instance.cpp"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# Party co-op (docs/PARTY_COOP_PLAN.md): (label, BB_PARTY value); "" = off (BB_PARTY unset)
PARTY_MODES = [("Off", ""), ("Host a party", "host"), ("Join a party", "join")]
PARTY_MAX_CHOICES = ["2", "3", "4"]
PARTY_DEFAULT_PORT = 9307
PARTY_DEFAULT_STUN = "stun.l.google.com:19302"
PARTY_NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,16}$")
# BBP1- + Crockford base32 groups (I/L/O accepted as aliases of 1/1/0, U is never used)
PARTY_CODE_RE = re.compile(r"^BBP1(-[0-9A-HJ-NP-TV-Z]+)+$", re.IGNORECASE)
PARTY_HOSTPORT_RE = re.compile(r"^(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+):(\d{1,5})$")
MP_INSTANCES = APP_DIR / "tools" / "mp" / "instances.py"
PARTY_GUIDE = APP_DIR / "docs" / "PARTY.md"
# Party rules & sync dialog. Combos: (label, env value); the first entry is the game's default.
PARTY_START_CHOICES = [("After the prologue (recommended)", "prologue_solo"), ("Right away", "immediate")]
PARTY_INSIGHT_CHOICES = [("Same as the host (default)", "parity"), ("Full bonus", "full"),
                         ("Game's own (+1 only)", "0")]
PARTY_BACKUP_MINUTES_CHOICES = ["5", "10", "15", "30", "60", "0"]  # 0 = only at the start
PARTY_BACKUP_KEEP_CHOICES = ["5", "10", "20", "50", "100"]
# On/off party options: (settings key, environment variable, default = the game's default).
# Every one is written as 1 / 0 while party mode is on and unset while it is off.
PARTY_HELP_SHORT = ("How to play together: the host picks Host a party and starts the game, then sends the "
                    "code shown under Your code to the friends. Each friend picks Join a party, pastes "
                    "the code and starts the game. Everyone needs the same game version (1.09), the same "
                    "gameplay patches and mods and the same Max players. Click for the full steps.")
PARTY_HELP = """Party co-op lets 2 to 4 players play the campaign together. It is still in development: its parts have been tested on one PC, but it has not been tested between two PCs yet - expect bugs.

HOST
1. Party tab: Mode = Host a party, enter your name. Max players: 3 is the game's normal limit.
2. Launch the game. Once it is running, your party code (BBP1-...) appears under Your code. Click Copy code and send it to your friends.
3. Leave UPnP on. If friends cannot connect, forward the port (default 9307, UDP and TCP) to this PC in your router, or use a VPN such as Tailscale and give them your VPN address as host:port.

JOIN
1. Party tab: Mode = Join a party, enter your name, paste the host's code (Paste button).
2. Pick the same Max players as the host.
3. Launch the game after the host's game is running.

IN THE GAME
- Everyone creates a character and plays the short prologue in their own world (until the first visit to the Hunter's Dream and taking a weapon). After that the host's game summons the others automatically (no bells or Insight needed).
- Story progress, boss kills, lamps, shortcuts, items from the host's world and cutscenes are shared with the guests' own saves.
- If a game crashes, it restarts by itself and rejoins the party.

IF JOINING FAILS
- "version mismatch": the game files, gameplay patches, gameplay mods or Max players differ from the host's. The message says which.
- "wrong password or party code": copy the code again; check the Password field on both sides.
- Nothing happens: the host's port is not reachable. See step 3 for the host.

Saves: with Separate party saves on, the party plays from user\\savedata_party and backs it up to user\\save_backups. README.txt there explains restoring a backup.

The full guide is docs\\PARTY.md."""
PARTY_SWITCHES = [
    ("party_grant_bells", "BB_PARTY_GRANT_BELLS", True),
    ("party_open_world", "BB_PARTY_OPEN_WORLD", True),
    ("party_guest_respawn", "BB_PARTY_GUEST_RESPAWN", True),
    ("party_guest_refill", "BB_PARTY_GUEST_REFILL", True),
    ("party_guest_mark", "BB_PARTY_GUEST_MARK", False),
    ("party_full_rewards", "BB_PARTY_FULL_REWARDS", True),
    ("party_items", "BB_PARTY_ITEMS", True),
    ("party_progress", "BB_PARTY_PROGRESS", True),
    ("party_progress_npc", "BB_PARTY_PROGRESS_NPC", False),
    ("party_story", "BB_PARTY_STORY", True),
    ("party_story_mirror", "BB_PARTY_STORY_MIRROR", True),
    ("party_restart", "BB_PARTY_RESTART", True),
]


def load_xml_patches(xml_path: Path = PATCHES_XML):
    """Load all 01.09 eboot.bin patches from Bloodborne.xml into a list of dicts."""
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
                note = meta.get("Note", "").strip()
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


def decode_line(raw: bytes) -> str:
    """Decode a console line: UTF-8 first, then the system code page."""
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(locale.getpreferredencoding(False), errors="replace")


def resolve_game_dir(target: str):
    """Return the game folder for an eboot.bin path or a folder path, else None."""
    p = Path(target.strip().strip('"'))
    if p.is_file() and p.name.lower() == "eboot.bin":
        return p.parent
    if p.is_dir() and (p / "eboot.bin").is_file():
        return p
    return None


def party_default_name() -> str:
    """The Windows user name reduced to a valid party name (1-16 of A-Z a-z 0-9 _ -)."""
    name = os.environ.get("USERNAME") or os.environ.get("USER") or ""
    name = re.sub(r"[^A-Za-z0-9_-]", "", name.replace(" ", "_"))[:16]
    return name or "Hunter"


def party_name_ok(name: str) -> bool:
    return bool(PARTY_NAME_RE.match(name or ""))


def party_port(value, default: int = PARTY_DEFAULT_PORT):
    """The port as an int for a number in 1024-65535, `default` for an empty value, else None."""
    text = str(value if value is not None else "").strip()
    if not text:
        return default
    if not text.isdigit():
        return None
    port = int(text)
    return port if 1024 <= port <= 65535 else None


def party_code_problem(code: str) -> str:
    """'' when `code` is a party code (BBP1-...) or host:port, else what is wrong with it."""
    code = (code or "").strip()
    if not code:
        return "enter the party code the host gave you (BBP1-...) or host:port"
    if code.upper().startswith("BBP1"):
        if PARTY_CODE_RE.match(code):
            return ""
        return "a party code is BBP1- followed by groups of 0-9 / A-Z (no U), separated by -"
    m = PARTY_HOSTPORT_RE.match(code)
    if m:
        port = int(m.group(2))
        return "" if 1 <= port <= 65535 else "the port after : must be 1-65535"
    return "not a party code (BBP1-...) or host:port"


def party_user_dir() -> Path:
    """The game's user folder as run.bat finds it: BB_USER_DIR, else <BB_DATA_DIR or launcher dir>\\user."""
    user = os.environ.get("BB_USER_DIR")
    if user:
        return Path(user)
    return Path(os.environ.get("BB_DATA_DIR", str(APP_DIR))) / "user"


def read_party_code() -> str:
    """The last code the hosting game wrote to <user dir>\\party_code.txt ('' if none)."""
    try:
        text = (party_user_dir() / "party_code.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return next((ln.strip() for ln in text.splitlines() if ln.strip()), "")


def party_saves_module():
    """scripts\\party_saves.py: the separate party save folder helpers."""
    scripts_dir = str(APP_DIR / "scripts")
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    import party_saves
    return party_saves


def party_env(s: dict) -> dict:
    """BB_PARTY_* environment from launcher settings; party off unsets every variable."""
    keys = ["BB_PARTY", "BB_PARTY_NAME", "BB_PARTY_PORT", "BB_PARTY_CODE", "BB_PARTY_PASSWORD",
            "BB_PARTY_UPNP", "BB_PARTY_STUN", "BB_PARTY_PUBLIC_ADDR", "BB_PARTY_MAX",
            "BB_PARTY_SEAMLESS", "BB_PARTY_AUTO", "BB_PARTY_SAVE", "BB_PARTY_START",
            "BB_PARTY_GUEST_INSIGHT", "BB_SAVE_BACKUP_MINUTES", "BB_SAVE_BACKUP_KEEP"]
    keys += [env_name for _, env_name, _ in PARTY_SWITCHES]
    env = dict.fromkeys(keys)
    mode = str(s.get("party_mode", "") or "")
    if mode not in ("host", "join"):
        return env
    name = str(s.get("party_name", "") or "").strip()
    port = party_port(s.get("party_port", ""))
    max_players = str(s.get("party_max", "3"))
    stun = str(s.get("party_stun", "") or "").strip()
    public = str(s.get("party_public_addr", "") or "").strip()
    password = str(s.get("party_password", "") or "")
    env["BB_PARTY"] = mode
    env["BB_PARTY_NAME"] = name if party_name_ok(name) else party_default_name()
    env["BB_PARTY_PORT"] = str(port or PARTY_DEFAULT_PORT)
    if mode == "join":
        code = str(s.get("party_code", "") or "").strip()
        env["BB_PARTY_CODE"] = (code.upper() if code.upper().startswith("BBP1") else code) or None
    env["BB_PARTY_PASSWORD"] = password or None
    env["BB_PARTY_UPNP"] = "1" if s.get("party_upnp", True) else "0"
    env["BB_PARTY_STUN"] = stun or None  # game default: stun.l.google.com:19302; "off" disables
    env["BB_PARTY_PUBLIC_ADDR"] = (public or None) if mode == "host" else None
    env["BB_PARTY_MAX"] = max_players if max_players in PARTY_MAX_CHOICES else "3"
    env["BB_PARTY_SEAMLESS"] = "1" if s.get("party_seamless", True) else "0"
    env["BB_PARTY_AUTO"] = "1" if s.get("party_auto", True) else "0"
    env["BB_PARTY_SAVE"] = "separate" if s.get("party_separate_save", True) else "shared"
    # Party rules & sync dialog (defaults = the game's defaults, docs/PARTY.md)
    start = str(s.get("party_start", "") or "")
    env["BB_PARTY_START"] = start if start in dict(PARTY_START_CHOICES).values() else PARTY_START_CHOICES[0][1]
    insight = str(s.get("party_guest_insight", "") or "")
    env["BB_PARTY_GUEST_INSIGHT"] = insight if insight in dict(PARTY_INSIGHT_CHOICES).values() \
        else PARTY_INSIGHT_CHOICES[0][1]
    minutes = str(s.get("party_backup_minutes", "10"))
    env["BB_SAVE_BACKUP_MINUTES"] = minutes if minutes in PARTY_BACKUP_MINUTES_CHOICES else "10"
    keep = str(s.get("party_backup_keep", "10"))
    env["BB_SAVE_BACKUP_KEEP"] = keep if keep in PARTY_BACKUP_KEEP_CHOICES else "10"
    for key, env_name, default in PARTY_SWITCHES:
        env[env_name] = "1" if s.get(key, default) else "0"
    return env


def settings_env(s: dict) -> dict:
    """BB_* environment for run.bat from launcher settings (None = unset the variable).

    Shared by the GUI and `launcher.py --write-env` (run.bat started without the launcher).
    Defaults match the GUI's defaults for a missing launcher_settings.json.
    """
    def on(key, default=True):
        return bool(s.get(key, default))

    env = {}
    game_dir = resolve_game_dir(s.get("eboot", "")) if s.get("eboot") else None
    if game_dir is not None:
        env["BB_GAME_DIR"] = str(game_dir)
    env["BB_FPS"] = s.get("fps", "uncap")
    env["BB_MODS_ENABLED"] = "1" if on("mods") else "0"
    env["BB_UPSCALER"] = None if on("feat_upscaler") else "off"
    env["BB_OBJECT_MOTION"] = None if on("feat_object_motion") else "0"
    env["BB_OVERLAY"] = None if on("feat_overlay") else "0"
    env["BB_FPS_PATCH"] = None if on("feat_fps_patch") else "0"

    # Resolution scaling
    res_choice = s.get("res", RES_CHOICES[0])
    if res_choice not in RES_CHOICES:  # including LEGACY_RES_DEFAULT
        res_choice = RES_CHOICES[0]
    env["BB_RENDER_RES"] = res_choice if on("feat_res_scaling") and res_choice != RES_CHOICES[0] else None

    env["BB_TRACE"] = "1" if on("feat_tracing") else "0"
    env["BB_TIMEOUT"] = str(s.get("timeout", "0")).split()[0]
    env["BB_WATCHDOG"] = None if on("feat_watchdog") else "0"
    env["BB_PREP_WORKERS"] = None if on("feat_draw_prep") else "0"
    env["BB_FULLSCREEN"] = "1" if on("feat_fullscreen", bool(s.get("fullscreen", False))) else "0"
    # Overlay menu toggle on gamepad L3 + R3 (Insert always works)
    env["BB_OVERLAY_PAD"] = "1" if on("feat_overlay_pad", False) else "0"

    # Upstream 0.5: the port's settings as pages of the game's System menu (BB_GAME_MENU=0: off),
    # the online/offline screen skipped by a game patch (scripts/patches.py adds it unless
    # BB_SKIP_NETWORK_CHOICE=0), the monitor (BB_DISPLAY) and gamepad (BB_GAMEPAD) to use.
    env["BB_GAME_MENU"] = None if on("feat_game_menu") else "0"
    env["BB_SKIP_NETWORK_CHOICE"] = "1" if on("feat_skip_network_choice") else "0"
    env["BB_DISPLAY"] = str(s.get("display") or "") or None
    env["BB_GAMEPAD"] = str(s.get("gamepad") or "") or None
    # Background pre-upload of GPU memory into VRAM: "" = run.bat's default (1), "2" full, "0" off.
    preupload = str(s.get("preupload", ""))
    env["BB_PREUPLOAD"] = preupload if preupload in ("0", "2") else None
    # Frame and readback statistics into logs\<time>.frames.csv / .readbacks.csv
    env["BB_SAVE_LOG"] = "1" if on("feat_save_log", False) else "0"
    # Mute the game while its window is in the background
    env["BB_MUTE_UNFOCUSED"] = "1" if on("feat_mute_unfocused") else "0"
    # Mouse & keyboard controls (scheme from Mrsuss60/bloodborne_pc_windows_port): opt-in, and the
    # launcher decides at each start (the in-game Controls tab switches it for the session). Its
    # tuning (mk_* keys) lives in bbport.ini: the launcher's dialog and the in-game menu edit it.
    env["BB_MOUSE_KEYBOARD"] = "1" if on("feat_mouse_keyboard", False) else "0"
    # Performance HUD (F11 in game): on = shown at every start; off = the game keeps the choice
    # made with F11 or in its menu (bbport.ini show_hud).
    env["BB_HUD"] = "1" if on("feat_hud", False) else None

    # Shader compilation: background compile that skips the draw meanwhile (also a live toggle in
    # the in-game menu), graphics pipeline library (stages compiled ahead, linked at draw time) and
    # the "Compiling shaders: NN%" indicator.
    # On by default since 0.5-pre4 (upstream's BB_ASYNC_PIPELINES); stored as feat_async_pipelines:
    # the old feat_async_shaders key mostly held the old default (off) and is not read any more.
    env["BB_ASYNC_SHADERS"] = "1" if on("feat_async_pipelines", True) else "0"
    env["BB_GPL"] = "1" if on("feat_gpl", False) else "0"
    env["BB_COMPILE_INDICATOR"] = "1" if on("feat_compile_indicator") else "0"
    env["BB_SHADER_PRECOMPILE"] = "1" if on("feat_shader_precompile") else "0"

    # Frame ahead queue (smooth frametimes & bound queue latency: 2 = balanced)
    frames_ahead = str(s.get("frames_ahead", "2"))
    env["BB_FRAMES_AHEAD"] = frames_ahead if frames_ahead in ("1", "2", "3") else "2"

    # Anisotropic filtering (0 = game default, 2/4/8/16 = forced)
    aniso_mapping = {c[0]: c[1] for c in ANISO_CHOICES}
    env["BB_ANISO"] = aniso_mapping.get(s.get("aniso", "16x"), "16")

    # Bloodborne.xml Patches
    active_patches = sorted(s.get("enabled_patches", ["Skip Intro"]))
    env["BB_PATCHES"] = ";".join(active_patches) if active_patches else None

    # Party co-op (BB_PARTY=host|join; off = every BB_PARTY_* unset)
    env.update(party_env(s))
    if env["BB_PARTY"]:
        # The party needs the game's ONLINE title path: scripts/patches.py applies the skip
        # patch's online variant ("Party: Skip Online/Offline Choice (Online)").
        env["BB_SKIP_NETWORK_CHOICE"] = "online"
    return env


def write_env_bat(path: Path, settings_path: Path = SETTINGS_FILE):
    """Write the saved launcher settings as `set` lines for run.bat to `call`."""
    try:
        data = json.loads(settings_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = {}
    if not isinstance(data, dict):
        data = {}
    lines = ["@echo off"]
    for key, value in settings_env(data).items():
        value = "" if value is None else value.replace('"', "").replace("%", "%%")
        lines.append(f'set "{key}={value}"')
    # cmd reads batch files in the OEM code page (non-ASCII game paths)
    path.write_text("\r\n".join(lines) + "\r\n", encoding="oem" if os.name == "nt" else "utf-8",
                    errors="replace", newline="")


# Mouse & keyboard tuning in bbport.ini (key: default, low, high), edited by the launcher's dialog
# and the in-game Controls tab; a running game re-reads them when the file changes.
MK_TUNING = {
    "mk_sens_x": (1.0, 0.1, 5.0),
    "mk_sens_y": (1.0, 0.1, 5.0),
    "mk_smoothing": (0.2, 0.0, 0.8),
    "mk_deadzone": (0.05, 0.0, 0.2),
}
# Mouse & keyboard mode's bindings (src/runtime_pad.c; scheme from Mrsuss60/bloodborne_pc_windows_port)
MK_BINDINGS = [
    ("Mouse movement", "Camera (right stick)"),
    ("Left click", "R1: attack"),
    ("Shift + left click", "R2: strong attack"),
    ("Right click", "L2: firearm / left hand"),
    ("Shift + right click", "L1: transform trick weapon"),
    ("Middle click / Q", "R3: lock on / reset camera"),
    ("Mouse side buttons", "L1 (back) / R2 (forward)"),
    ("W A S D", "Move (left stick)"),
    ("Space", "Circle: dodge, run (hold)"),
    ("E / Enter", "Cross: interact, confirm"),
    ("R", "Square: use quick item"),
    ("X", "Triangle: use Blood Vial"),
    ("C / Z", "L3"),
    ("Tab / G", "Touchpad: gestures, menus"),
    ("Backspace", "Touchpad right side"),
    ("Esc / F1", "Options: game menu"),
    ("Arrows / 1 2 3 4", "D-pad up / down / left / right"),
    ("I J K L", "Camera (right stick)"),
]


def bbport_ini_path() -> Path:
    """bbport.ini as run.bat finds it: BB_CONFIG, else BB_DATA_DIR (default: the launcher's folder)."""
    if os.environ.get("BB_CONFIG"):
        return Path(os.environ["BB_CONFIG"])
    data_dir = Path(os.environ.get("BB_DATA_DIR") or ".")
    return (data_dir if data_dir.is_absolute() else APP_DIR / data_dir) / "bbport.ini"


def read_ini(path: Path) -> dict:
    values = {}
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            key, sep, value = line.partition("=")
            if sep and not line.lstrip().startswith("#"):
                values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def update_ini(path: Path, values: dict):
    """Sets keys of bbport.ini, keeping its other lines (the game's settings, key.* bindings)."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        lines = ["# bbport settings (in-game menu: Insert; L3+R3 with BB_OVERLAY_PAD=1)"]
    written = set()
    out = []
    for line in lines:
        key, sep, _ = line.partition("=")
        key = key.strip()
        if sep and not line.lstrip().startswith("#") and key in values:
            if key not in written:
                out.append(f"{key}={values[key]}")
                written.add(key)
            continue
        out.append(line)
    out += [f"{key}={value}" for key, value in values.items() if key not in written]
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text("\n".join(out) + "\n", encoding="utf-8")
    os.replace(temporary, path)


# Shader cache: <user>\cache\<title id>\ (run.bat: BB_USER_DIR or BB_DATA_DIR\user). Pipeline keys
# (.key) and shader sources (.src) do not depend on the GPU; .meta/.spv are built for the GPU in
# profile.bin and are rebuilt from the sources when a cache moves to another GPU.
CACHE_GPU_INDEPENDENT = {".key", ".src"}
CACHE_GPU_SPECIFIC = {".meta", ".spv"}
CACHE_PROFILE = "profile.bin"


def shader_cache_root() -> Path:
    user = os.environ.get("BB_USER_DIR")
    if user:
        return Path(user) / "cache"
    return Path(os.environ.get("BB_DATA_DIR", str(APP_DIR))) / "user" / "cache"


def _cache_name_ok(name: str) -> bool:
    """A plain file or folder name: nothing in an imported archive may leave the cache folder."""
    return bool(name) and name not in (".", "..") and not any(c in name for c in '/\\:')


def export_shader_cache(zip_path: Path, root: Path = None) -> int:
    """Writes every title's shader cache into one .zip; returns the number of files."""
    import zipfile
    root = root or shader_cache_root()
    titles = sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []
    count = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for title in titles:
            for f in sorted(title.iterdir()):
                if f.is_file() and (f.suffix in CACHE_GPU_INDEPENDENT | CACHE_GPU_SPECIFIC
                                    or f.name == CACHE_PROFILE):
                    zf.write(f, f"{title.name}/{f.name}")
                    count += 1
    return count


def import_shader_cache(zip_path: Path, root: Path = None) -> dict:
    """Merges a cache .zip into the local cache without overwriting anything. Keys and sources
    are always taken; GPU-built .meta/.spv only when the .zip was made for the same GPU profile
    (otherwise the game rebuilds them from the sources before the intro)."""
    import zipfile
    root = root or shader_cache_root()
    stats = {"added": 0, "existing": 0, "gpu_skipped": 0, "titles": []}
    with zipfile.ZipFile(zip_path) as zf:
        entries = {}
        for n in zf.namelist():
            title, _, fname = n.partition("/")
            if _cache_name_ok(title) and _cache_name_ok(fname):
                entries.setdefault(title, []).append((n, fname))
        for title, files in sorted(entries.items()):
            dest = root / title
            local_profile = dest / CACHE_PROFILE
            zip_profile = next((zf.read(n) for n, f in files if f == CACHE_PROFILE), None)
            if local_profile.is_file():
                same_gpu = zip_profile is not None and local_profile.read_bytes() == zip_profile
            else:
                same_gpu = zip_profile is not None  # empty cache: the game checks the profile itself
            dest.mkdir(parents=True, exist_ok=True)
            stats["titles"].append(title)
            for n, fname in files:
                suffix = Path(fname).suffix
                if fname == CACHE_PROFILE:
                    if not local_profile.is_file() and same_gpu:
                        local_profile.write_bytes(zf.read(n))
                    continue
                if suffix not in CACHE_GPU_INDEPENDENT | CACHE_GPU_SPECIFIC:
                    continue
                if suffix in CACHE_GPU_SPECIFIC and not same_gpu:
                    stats["gpu_skipped"] += 1
                    continue
                target = dest / fname
                if target.exists():
                    stats["existing"] += 1
                    continue
                target.write_bytes(zf.read(n))
                stats["added"] += 1
    return stats


LOG_HISTORY = 5


def rotate_launcher_log(keep: int = LOG_HISTORY):
    """Moves the previous session's launcher.log to logs\\launcher_<time>.log (keeping the last
    `keep`), so the log of a session that crashed is still there after the next start."""
    if not LOG_FILE.is_file() or LOG_FILE.stat().st_size == 0:
        return
    logs = APP_DIR / "logs"
    try:
        logs.mkdir(exist_ok=True)
        stamp = datetime.fromtimestamp(LOG_FILE.stat().st_mtime).strftime("%Y%m%d_%H%M%S")
        target = logs / f"launcher_{stamp}.log"
        if not target.exists():
            LOG_FILE.replace(target)
        old = sorted(logs.glob("launcher_*.log"))
        for f in old[:-keep]:
            f.unlink()
    except OSError:
        pass


def kill_tree(proc: subprocess.Popen):
    """Kill run.bat and everything it started (bbport.exe included)."""
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=NO_WINDOW,
        )
    else:
        proc.kill()


def gpu_capabilities_lines(option: str) -> list:
    """Output lines of out/bb-gpu-capabilities.exe --displays / --gamepads ([] if unavailable)."""
    if not GPU_CAPS.is_file():
        return []
    try:
        run = subprocess.run([str(GPU_CAPS), option], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=5,
                             creationflags=NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        return []
    return run.stdout.splitlines()


def connected_displays() -> list:
    """(BB_DISPLAY value, label) of the monitors: the name, or the number when names repeat."""
    rows = [line.split("\t") for line in gpu_capabilities_lines("--displays") if line.count("\t") == 2]
    names = [name for name, _, _ in rows]
    return [(name if names.count(name) == 1 else str(number),
             f"{number}: {name} ({size})" + (", primary" if primary == "1" else ""))
            for number, (name, size, primary) in enumerate(rows, 1)]


def connected_gamepads() -> list:
    """(BB_GAMEPAD value = GUID, label = name) of the connected gamepads."""
    return [tuple(line.split("\t", 1)) for line in gpu_capabilities_lines("--gamepads") if "\t" in line]


class Tooltip:
    """Hover help for a Tk/ttk widget: a small dark popup next to the pointer after a short delay."""

    DELAY_MS = 400
    WRAP_PX = 380

    def __init__(self, widget, text: str):
        self.widget = widget
        self.text = text
        self.tip = None
        self.after_id = None
        widget.bind("<Enter>", self.schedule, add="+")
        widget.bind("<Leave>", self.hide, add="+")
        widget.bind("<ButtonPress>", self.hide, add="+")
        widget.bind("<Destroy>", self.hide, add="+")

    def schedule(self, _event=None):
        self.cancel()
        self.after_id = self.widget.after(self.DELAY_MS, self.show)

    def cancel(self):
        if self.after_id is not None:
            try:
                self.widget.after_cancel(self.after_id)
            except tk.TclError:
                pass
            self.after_id = None

    def show(self):
        self.after_id = None
        if self.tip is not None or not self.text:
            return
        try:
            px, py = self.widget.winfo_pointerx(), self.widget.winfo_pointery()
            tip = tk.Toplevel(self.widget)
        except tk.TclError:
            return
        tip.wm_overrideredirect(True)
        try:
            tip.wm_attributes("-topmost", True)
        except tk.TclError:
            pass
        border = tk.Frame(tip, bg="#5a4a2a", padx=1, pady=1)
        border.pack()
        tk.Label(border, text=self.text, justify="left", wraplength=self.WRAP_PX,
                 bg="#2b2b2e", fg="#e0e0e0", font=("Segoe UI", 9), padx=9, pady=6).pack()
        tip.update_idletasks()
        w, h = tip.winfo_reqwidth(), tip.winfo_reqheight()
        sw, sh = tip.winfo_screenwidth(), tip.winfo_screenheight()
        x, y = px + 14, py + 20
        if x + w > sw - 4:
            x = max(4, sw - w - 4)
        if y + h > sh - 4:
            y = max(4, py - h - 12)
        tip.wm_geometry(f"+{x}+{y}")
        self.tip = tip

    def hide(self, _event=None):
        self.cancel()
        if self.tip is not None:
            try:
                self.tip.destroy()
            except tk.TclError:
                pass
            self.tip = None


class BloodborneLauncher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Bloodborne PC Launcher")
        self.geometry("780x760")  # the Party tab is the tallest settings tab
        self.minsize(720, 680)

        self.proc = None
        self.log_fh = None
        self.hidden_count = 0
        self.log_queue = queue.Queue()
        self.settings = self.load_settings()
        self.all_xml_patches = load_xml_patches()
        self.enabled_patches = set(self.settings.get("enabled_patches", ["Skip Intro"]))

        self.feat_skip_intro = tk.BooleanVar(value="Skip Intro" in self.enabled_patches)
        self.feat_perf_patch = tk.BooleanVar(value="Performance Patch (perf increase)" in self.enabled_patches)
        self.feat_no_blur = tk.BooleanVar(value="Disable Motion Blur (perf increase)" in self.enabled_patches)

        self.build_style()
        self.build_ui()

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(50, self.poll_queue)
        self.log("Ready. Select your eboot.bin and press Launch Bloodborne.")

    # ------------------------------------------------------------------ UI
    def build_style(self):
        self.configure(bg="#1a1a1a")
        style = ttk.Style(self)
        style.theme_use("clam")

        bg_dark, bg_card, fg_text = "#1a1a1a", "#252526", "#e0e0e0"
        bg_input = "#1e1e1e"
        accent, accent_hover = "#990000", "#b30000"

        style.configure(".", background=bg_dark, foreground=fg_text, font=("Segoe UI", 10))
        style.configure("TLabel", background=bg_dark, foreground=fg_text)
        style.configure("Card.TFrame", background=bg_card, relief="flat")
        style.configure("Card.TLabel", background=bg_card, foreground=fg_text)
        style.configure("Card.TCheckbutton", background=bg_card, foreground=fg_text)
        style.configure("Header.TLabel", font=("Segoe UI", 16, "bold"), foreground="#c5a059", background=bg_dark)
        style.configure("SubHeader.TLabel", font=("Segoe UI", 9), foreground="#888888", background=bg_dark)
        style.configure("Section.TLabel", font=("Segoe UI", 8, "bold"), foreground="#c5a059", background=bg_card)
        style.configure("Hint.TLabel", font=("Segoe UI", 8), foreground="#888888", background=bg_card)
        style.map("Card.TCheckbutton", background=[("active", "#2d2d30")],
                  foreground=[("disabled", "#666666")])

        # Settings notebook: dark tabs, the selected one joins the card below it
        style.configure("TNotebook", background=bg_dark, borderwidth=0, tabmargins=(0, 0, 0, 0),
                        bordercolor="#3e3e42", lightcolor=bg_card, darkcolor=bg_card)
        style.configure("TNotebook.Tab", background="#2a2a2c", foreground="#9a9a9a", padding=(12, 5),
                        font=("Segoe UI", 9), bordercolor="#3e3e42", lightcolor="#2a2a2c",
                        darkcolor="#2a2a2c", focuscolor=bg_card)
        style.map("TNotebook.Tab",
                  background=[("selected", bg_card), ("active", "#333336")],
                  foreground=[("selected", "#c5a059"), ("active", fg_text)],
                  lightcolor=[("selected", accent)],
                  focuscolor=[("selected", bg_card)],
                  expand=[("selected", (1, 1, 1, 0))])

        # Path Entry styling
        style.configure("TEntry",
                        fieldbackground=bg_input,
                        foreground="#ffffff",
                        insertcolor="#ffffff",
                        bordercolor="#3e3e42",
                        lightcolor="#3e3e42",
                        darkcolor="#3e3e42")
        style.map("TEntry",
                  fieldbackground=[("disabled", "#2a2a2c"), ("readonly", "#202022")],
                  foreground=[("disabled", "#666666")])
        style.map("Card.TLabel", foreground=[("disabled", "#666666")])

        # Combobox styling
        style.configure("TCombobox",
                        fieldbackground=bg_input,
                        background="#333333",
                        foreground="#ffffff",
                        arrowcolor="#ffffff",
                        bordercolor="#3e3e42",
                        lightcolor="#3e3e42",
                        darkcolor="#3e3e42")
        style.map("TCombobox",
                  fieldbackground=[("readonly", bg_input)],
                  foreground=[("readonly", "#ffffff")],
                  selectbackground=[("readonly", bg_input)],
                  selectforeground=[("readonly", "#ffffff")])

        # Style the dropdown popdown listbox via Tk option database
        self.option_add("*TCombobox*Listbox.background", bg_input)
        self.option_add("*TCombobox*Listbox.foreground", "#ffffff")
        self.option_add("*TCombobox*Listbox.selectBackground", accent)
        self.option_add("*TCombobox*Listbox.selectForeground", "#ffffff")

        style.configure("Action.TButton", font=("Segoe UI", 11, "bold"),
                        background=accent, foreground="white", borderwidth=0)
        style.map("Action.TButton",
                  background=[("disabled", "#4a2a2a"), ("active", accent_hover)],
                  foreground=[("disabled", "#888888")])

        style.configure("Secondary.TButton", font=("Segoe UI", 9),
                        background="#3a3a3c", foreground="white", borderwidth=0)
        style.map("Secondary.TButton",
                  background=[("disabled", "#2a2a2c"), ("active", "#4a4a4c")],
                  foreground=[("disabled", "#777777")])

    def build_ui(self):
        main = ttk.Frame(self, padding=(16, 12, 16, 12))
        main.pack(fill="both", expand=True)

        ttk.Label(main, text="BLOODBORNE PC", style="Header.TLabel").pack(anchor="w")
        ttk.Label(main, text="Native Windows Port (bbport) Launcher",
                  style="SubHeader.TLabel").pack(anchor="w", pady=(0, 8))

        # eboot.bin selection
        card = ttk.Frame(main, style="Card.TFrame", padding=12)
        card.pack(fill="x", pady=(0, 8))
        ttk.Label(card, text="Game Executable (eboot.bin):", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).pack(anchor="w", pady=(0, 5))

        select_frame = ttk.Frame(card, style="Card.TFrame")
        select_frame.pack(fill="x")

        initial_path = self.settings.get("eboot", "")
        if not initial_path or not Path(initial_path).exists():
            initial_path = ""
            if (DEFAULT_GAME / "eboot.bin").exists():
                initial_path = str(DEFAULT_GAME / "eboot.bin")
            elif DEFAULT_GAME.exists():
                initial_path = str(DEFAULT_GAME)

        self.eboot_var = tk.StringVar(value=initial_path)
        eboot_entry = ttk.Entry(select_frame, textvariable=self.eboot_var, font=("Consolas", 10))
        eboot_entry.pack(side="left", fill="x", expand=True, padx=(0, 10))
        browse_btn = ttk.Button(select_frame, text="Browse...", style="Secondary.TButton",
                                command=self.browse_eboot)
        browse_btn.pack(side="right")
        eboot_tip = ("The game's eboot.bin, or the folder that contains it: Bloodborne CUSA03173 with "
                     "update 1.09 merged. The folder is passed to run.bat as BB_GAME_DIR.")
        Tooltip(eboot_entry, eboot_tip)
        Tooltip(browse_btn, "Pick the game's eboot.bin. " + eboot_tip.split(": ", 1)[1])

        # ---- setting variables
        fps = self.settings.get("fps", "uncap")
        res = self.settings.get("res", RES_CHOICES[0])
        timeout_val = str(self.settings.get("timeout", "0"))
        if timeout_val not in [c.split()[0] for c in TIMEOUT_CHOICES]:
            matching_choice = "0 (no limit)"
        else:
            matching_choice = next((c for c in TIMEOUT_CHOICES if c.startswith(timeout_val + " ") or c == timeout_val), TIMEOUT_CHOICES[0])

        aniso = self.settings.get("aniso", "16x")
        if aniso not in [c[0] for c in ANISO_CHOICES]:
            aniso = "16x"

        self.fps_var = tk.StringVar(value=fps if fps in FPS_CHOICES else "uncap")
        self.res_var = tk.StringVar(value=res if res in RES_CHOICES else RES_CHOICES[0])
        self.timeout_var = tk.StringVar(value=matching_choice)
        self.aniso_var = tk.StringVar(value=aniso)
        self.aniso_desc_var = tk.StringVar()

        # Monitor (BB_DISPLAY) and controller (BB_GAMEPAD), listed by bb-gpu-capabilities.exe
        saved_display = str(self.settings.get("display", "") or "")
        self.display_choices = [(DEFAULT_DISPLAY, "")] + [(label, value) for value, label in connected_displays()]
        if saved_display and saved_display not in [v for _, v in self.display_choices]:
            self.display_choices.append((f"{saved_display} (saved)", saved_display))
        self.display_var = tk.StringVar(value=next(
            (label for label, value in self.display_choices if value == saved_display), DEFAULT_DISPLAY))

        saved_gamepad = str(self.settings.get("gamepad", "") or "")
        self.gamepad_choices = [(DEFAULT_GAMEPAD, "")] + [(name, guid) for guid, name in connected_gamepads()]
        if saved_gamepad and saved_gamepad not in [v for _, v in self.gamepad_choices]:
            name = self.settings.get("gamepad_name") or saved_gamepad
            self.gamepad_choices.append((f"{name} (not connected)", saved_gamepad))
        self.gamepad_var = tk.StringVar(value=next(
            (label for label, value in self.gamepad_choices if value == saved_gamepad), DEFAULT_GAMEPAD))

        preupload = str(self.settings.get("preupload", ""))
        self.preupload_var = tk.StringVar(value=next(
            (label for label, value in PREUPLOAD_CHOICES if value == preupload), PREUPLOAD_CHOICES[0][0]))

        frames_ahead = str(self.settings.get("frames_ahead", "2"))
        self.frames_ahead_var = tk.StringVar(value=next(
            (label for label, value in FRAMES_AHEAD_CHOICES if value == frames_ahead), FRAMES_AHEAD_CHOICES[0][0]))

        self.feat_upscaler = tk.BooleanVar(value=self.settings.get("feat_upscaler", True))
        self.feat_object_motion = tk.BooleanVar(value=self.settings.get("feat_object_motion", True))
        self.feat_overlay = tk.BooleanVar(value=self.settings.get("feat_overlay", True))
        self.feat_fps_patch = tk.BooleanVar(value=self.settings.get("feat_fps_patch", True))
        self.feat_mods = tk.BooleanVar(value=self.settings.get("mods", True))
        self.feat_res_scaling = tk.BooleanVar(value=self.settings.get("feat_res_scaling", True))
        self.feat_tracing = tk.BooleanVar(value=self.settings.get("feat_tracing", True))
        self.feat_watchdog = tk.BooleanVar(value=self.settings.get("feat_watchdog", True))
        self.feat_draw_prep = tk.BooleanVar(value=self.settings.get("feat_draw_prep", True))
        self.feat_fullscreen = tk.BooleanVar(  # "fullscreen": key of the upstream launcher toggle (PR #39)
            value=self.settings.get("feat_fullscreen", self.settings.get("fullscreen", False)))
        self.feat_overlay_pad = tk.BooleanVar(value=self.settings.get("feat_overlay_pad", False))
        self.feat_game_menu = tk.BooleanVar(value=self.settings.get("feat_game_menu", True))
        self.feat_skip_network_choice = tk.BooleanVar(value=self.settings.get("feat_skip_network_choice", True))
        self.feat_save_log = tk.BooleanVar(value=self.settings.get("feat_save_log", False))
        self.feat_mute_unfocused = tk.BooleanVar(value=self.settings.get("feat_mute_unfocused", True))
        self.feat_async_shaders = tk.BooleanVar(value=self.settings.get("feat_async_pipelines", True))
        self.feat_gpl = tk.BooleanVar(value=self.settings.get("feat_gpl", False))
        self.feat_compile_indicator = tk.BooleanVar(value=self.settings.get("feat_compile_indicator", True))
        self.feat_shader_precompile = tk.BooleanVar(value=self.settings.get("feat_shader_precompile", True))
        self.feat_hud = tk.BooleanVar(value=self.settings.get("feat_hud", False))
        self.feat_mouse_keyboard = tk.BooleanVar(value=self.settings.get("feat_mouse_keyboard", False))

        # Party co-op (BB_PARTY_*)
        party_mode = str(self.settings.get("party_mode", "") or "")
        self.party_mode_var = tk.StringVar(value=next(
            (label for label, value in PARTY_MODES if value == party_mode), PARTY_MODES[0][0]))
        self.party_name_var = tk.StringVar(value=str(self.settings.get("party_name") or party_default_name()))
        party_max = str(self.settings.get("party_max", "3"))
        self.party_max_var = tk.StringVar(value=party_max if party_max in PARTY_MAX_CHOICES else "3")
        self.party_auto = tk.BooleanVar(value=self.settings.get("party_auto", True))
        self.party_seamless = tk.BooleanVar(value=self.settings.get("party_seamless", True))
        self.party_separate_save = tk.BooleanVar(value=self.settings.get("party_separate_save", True))
        self.party_port_var = tk.StringVar(value=str(self.settings.get("party_port", PARTY_DEFAULT_PORT)))
        self.party_upnp = tk.BooleanVar(value=self.settings.get("party_upnp", True))
        self.party_public_var = tk.StringVar(value=str(self.settings.get("party_public_addr", "") or ""))
        self.party_stun_var = tk.StringVar(value=str(self.settings.get("party_stun", PARTY_DEFAULT_STUN) or ""))
        self.party_password_var = tk.StringVar(value=str(self.settings.get("party_password", "") or ""))
        self.party_code_var = tk.StringVar(value=str(self.settings.get("party_code", "") or ""))
        self.party_host_code_var = tk.StringVar(value=read_party_code())
        self.party_status_var = tk.StringVar()
        self.party_test_running = False
        # Party rules & sync dialog (PARTY_SWITCHES, start mode, guest Insight, backups)
        self.party_switch_vars = {key: tk.BooleanVar(value=bool(self.settings.get(key, default)))
                                  for key, _, default in PARTY_SWITCHES}

        def combo_label(choices, key):
            value = str(self.settings.get(key, "") or "")
            return next((label for label, v in choices if v == value), choices[0][0])
        self.party_start_var = tk.StringVar(value=combo_label(PARTY_START_CHOICES, "party_start"))
        self.party_insight_var = tk.StringVar(value=combo_label(PARTY_INSIGHT_CHOICES, "party_guest_insight"))
        minutes = str(self.settings.get("party_backup_minutes", "10"))
        self.party_backup_minutes_var = tk.StringVar(
            value=minutes if minutes in PARTY_BACKUP_MINUTES_CHOICES else "10")
        keep = str(self.settings.get("party_backup_keep", "10"))
        self.party_backup_keep_var = tk.StringVar(value=keep if keep in PARTY_BACKUP_KEEP_CHOICES else "10")

        # ---- presets row
        preset_row = ttk.Frame(main)
        preset_row.pack(fill="x", pady=(0, 4))
        ttk.Label(preset_row, text="Settings", font=("Segoe UI", 10, "bold")).pack(side="left")
        ttk.Label(preset_row, text="hover over any option for details",
                  style="SubHeader.TLabel").pack(side="left", padx=(8, 0))
        vanilla_btn = ttk.Button(preset_row, text="Vanilla mode", style="Secondary.TButton",
                                 command=self.set_vanilla_mode)
        vanilla_btn.pack(side="right", padx=(6, 0))
        everything_btn = ttk.Button(preset_row, text="Everything on", style="Secondary.TButton",
                                    command=self.set_everything_on)
        everything_btn.pack(side="right")
        Tooltip(vanilla_btn, "Preset: turns the port's optional features off (upscaler, object motion, overlay, "
                             "FPS patch, mods, resolution scaling, tracing, watchdog, draw prep workers, async "
                             "shaders, GPL, performance HUD, mouse & keyboard controls, party mode), sets "
                             "30 FPS, anisotropic filtering Off and disables all XML patches. Saved "
                             "immediately.")
        Tooltip(everything_btn, "Preset: turns the features back on (including async shaders and GPL), sets "
                                "Uncap FPS and 16x anisotropic filtering, and enables the recommended patches "
                                "(Skip Intro, Performance Patch, Disable Motion Blur). The performance HUD, "
                                "mouse & keyboard controls and the Party tab are preferences and stay as "
                                "they are. Saved immediately.")

        # ---- settings notebook
        self.notebook = ttk.Notebook(main)
        self.notebook.pack(fill="x", pady=(0, 8))

        # 1. Display
        tab = self._tab("Display")
        sec = self._section(tab, 0, "Resolution & window")
        self._check(sec, "Resolution scaling", self.feat_res_scaling,
                    "Lets the render resolution below override the game. Off = native 1080p rendering. "
                    "Default: on (BB_RENDER_RES).",
                    command=self.update_res_scaling_state)
        self.res_combo = self._combo(
            sec, "Render resolution:", self.res_var, RES_CHOICES,
            "From in-game menu (recommended): the in-game Output resolution and the upscaler preset "
            "(DLSS / FSR quality levels) decide the render size, and changes are kept. A fixed value "
            "here renders the 3D scene at exactly that size for the whole session and overrides the "
            "in-game preset (BB_RENDER_RES).",
            width=18, indent=True)
        self._check(sec, "Fullscreen", self.feat_fullscreen,
                    "Starts the game in fullscreen instead of a window. You can also toggle it in game with "
                    "Alt+Enter. Default: off (BB_FULLSCREEN).")
        self._combo(sec, "Monitor:", self.display_var, [c[0] for c in self.display_choices],
                    "Which monitor the game opens on. The list comes from bb-gpu-capabilities; "
                    "Primary (default) uses the main monitor (BB_DISPLAY).", width=24)

        sec = self._section(tab, 1, "Frame rate")
        self._combo(sec, "Frame rate target:", self.fps_var, FPS_CHOICES,
                    "Target frame rate: uncap, 60, 90, or 30 (the game's native rate). Above 30 needs the "
                    "FPS patch. Default: uncap (BB_FPS).", width=10)
        self._check(sec, "FPS patch (Uncap / 60 / 90)", self.feat_fps_patch,
                    "Applies the community high-framerate patches (FPS++) for the chosen target, with "
                    "physics and movement speeds kept correct. Off = the game stays at 30 FPS. "
                    "Default: on (BB_FPS_PATCH).")
        self._combo(sec, "Frames ahead:", self.frames_ahead_var, [c[0] for c in FRAMES_AHEAD_CHOICES],
                    "How many frames the GPU thread may queue ahead of the display. 1 = least input lag, "
                    "2 = smoother frame times (default), 3 = smoothest but more lag (BB_FRAMES_AHEAD).",
                    width=20)
        self._check(sec, "Performance HUD (F11)", self.feat_hud,
                    "In-game overlay with FPS, frame time and a frame-time graph, VRAM, GPU, CPU and RAM "
                    "load. F11 shows or hides it at any time; its corner, size and metrics are in the "
                    "in-game menu (Insert > Display). On = shown at every start; off = the game keeps "
                    "your last F11 / menu choice. Default: off (BB_HUD).")

        # 2. Graphics
        tab = self._tab("Graphics")
        sec = self._section(tab, 0, "Image quality")
        self._check(sec, "Upscaler (FSR 3.1)", self.feat_upscaler,
                    "Temporal upscaling and anti-aliasing with FSR 3.1. DLSS / FSR 4 can be selected in the "
                    "in-game menu when available. Off = no upscaler. Default: on (BB_UPSCALER).")
        self._check(sec, "Object motion vectors", self.feat_object_motion,
                    "Exact motion vectors for animated characters and objects, so FSR smears less on moving "
                    "things. Small GPU cost. Default: on (BB_OBJECT_MOTION).")
        aniso_tip = ("Sharper textures at grazing angles (floors, roads). "
                     + "; ".join(f"{label}: {desc[0].lower() + desc[1:]}" for label, _, desc in ANISO_CHOICES)
                     + ". Very small cost. Default: 16x (BB_ANISO).")
        self.aniso_combo = self._combo(sec, "Anisotropic filtering:", self.aniso_var,
                                       [c[0] for c in ANISO_CHOICES], aniso_tip, width=10)
        self.aniso_combo.bind("<<ComboboxSelected>>", lambda e: self.update_aniso_desc())
        self.aniso_desc_lbl = ttk.Label(sec, textvariable=self.aniso_desc_var, style="Hint.TLabel")
        self.aniso_desc_lbl.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(0, 2))
        sec.next_row += 1
        self.update_aniso_desc()

        sec = self._section(tab, 1, "Streaming & effects")
        self._combo(sec, "VRAM pre-upload:", self.preupload_var, [c[0] for c in PREUPLOAD_CHOICES],
                    "Uploads game memory into VRAM in the background, so there are fewer stutters when new "
                    "areas stream in. Normal = default; Full = everything (~3 GB more VRAM, 12 GB+ GPU); "
                    "Off = upload on demand (BB_PREUPLOAD).", width=30)
        self._check(sec, "Disable Motion Blur", self.feat_no_blur,
                    "Game patch by Kyo: removes the motion blur effect (and its velocity-map pass), a small "
                    "performance gain. Same as the 'Disable Motion Blur (perf increase)' XML patch.",
                    command=self.on_quick_patch_toggle)

        # 3. Performance & Shaders
        tab = self._tab("Performance & Shaders")
        sec = self._section(tab, 0, "Performance")
        self._check(sec, "Performance Patch (Kyo)", self.feat_perf_patch,
                    "Game patch by Kyo: changes some of the game's debug parameters for better performance; "
                    "may slightly affect some visuals. Same as the 'Performance Patch (perf increase)' XML patch.",
                    command=self.on_quick_patch_toggle)
        self._check(sec, "GPU draw prep workers", self.feat_draw_prep,
                    "Prepares draw calls on parallel worker threads to take load off the GPU thread. "
                    "Default: on (BB_PREP_WORKERS).")

        sec = self._section(tab, 1, "Shader compilation")
        self._check(sec, "Async shader compilation", self.feat_async_shaders,
                    "Compiles a new pipeline of a pass drawn every frame in the background and skips drawing "
                    "that object for a frame or two instead of stuttering; brief pop-in is possible. Loading "
                    "screens, one-time renders, the final frame, the UI and compute work are never skipped. "
                    "Can also be toggled live in the in-game menu. Default: on "
                    "(BB_ASYNC_SHADERS; upstream's name BB_ASYNC_PIPELINES works too).")
        self._check(sec, "Graphics pipeline library (GPL)", self.feat_gpl,
                    "Compiles shader parts ahead of time (startup and loading screens) and links them quickly "
                    "at draw time. Needs driver support (NVIDIA/AMD; falls back automatically). Restart "
                    "required. Default: off (BB_GPL).")
        self._check(sec, "Precompile shaders at startup", self.feat_shader_precompile,
                    "Before the intro logos, builds every pipeline in the shader cache (and rebuilds the "
                    "cache for a new GPU) with a progress screen, so areas already in the cache never "
                    "stutter. Slow only the first time; later launches are quick. Esc skips. "
                    "Default: on (BB_SHADER_PRECOMPILE).")
        self._check(sec, "Shader compile progress indicator", self.feat_compile_indicator,
                    "Shows 'Compiling shaders: NN%' in a corner while new shaders compile, only on loading "
                    "screens and the main menu, never during gameplay. Default: on (BB_COMPILE_INDICATOR).")
        cache_row = ttk.Frame(sec, style="Card.TFrame")
        cache_row.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(6, 2))
        sec.next_row += 1
        export_btn = ttk.Button(cache_row, text="Export shader cache...", style="Secondary.TButton",
                                command=self.export_cache)
        export_btn.pack(side="left", padx=(0, 6))
        import_btn = ttk.Button(cache_row, text="Import shader cache...", style="Secondary.TButton",
                                command=self.import_cache)
        import_btn.pack(side="left")
        Tooltip(export_btn, "Saves your shader cache (user\\cache) as one .zip to share. It works on any "
                            "GPU: the game rebuilds it for the other GPU before the intro. It is made from "
                            "the game's shaders, so share it only with people who own the game.")
        Tooltip(import_btn, "Merges a shader cache .zip from another player into yours (nothing is "
                            "overwritten). With 'Precompile shaders at startup' on, everything in it is "
                            "built before the intro on the next launch.")

        # 4. Game
        tab = self._tab("Game")
        sec = self._section(tab, 0, "Startup")
        self._check(sec, "Skip Intro & Logos", self.feat_skip_intro,
                    "Game patch by illusion: skips the intro logo screens at startup. Same as the "
                    "'Skip Intro' XML patch. Default: on.",
                    command=self.on_quick_patch_toggle)
        self._check(sec, "Skip online/offline choice", self.feat_skip_network_choice,
                    "Skips the screen that asks whether to play online or offline at startup (game patch). "
                    "With party mode on (Party tab) the game always takes the online path instead. "
                    "Default: on (BB_SKIP_NETWORK_CHOICE).")

        sec = self._section(tab, 1, "Menus, mods & patches")
        self._check(sec, "Settings in game's System menu", self.feat_game_menu,
                    "Adds the port's settings as pages of the game's own System menu (Display / Game effects "
                    "/ Game patches). Off leaves the game's menu untouched. Default: on (BB_GAME_MENU).")
        self._check(sec, "Enable mods folder (mods/)", self.feat_mods,
                    "Loads loose-file mods from the mods\\ folder (switches and load order in mods.json); the "
                    "original game files are not modified. Default: on (BB_MODS_ENABLED).")
        patches_btn = ttk.Button(sec, text="⚙  All XML Patches...", style="Secondary.TButton",
                                 command=self.open_patches_dialog)
        patches_btn.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(6, 2))
        sec.next_row += 1
        Tooltip(patches_btn, "Browse, search and enable any of the community patches from "
                             "patches\\Bloodborne.xml (game version 1.09). The quick patch boxes in these tabs "
                             "stay in sync (BB_PATCHES).")

        # 5. Input & Audio
        tab = self._tab("Input & Audio")
        sec = self._section(tab, 0, "Controller & overlay")
        self._combo(sec, "Controller:", self.gamepad_var, [c[0] for c in self.gamepad_choices],
                    "Which gamepad the game uses. The list comes from bb-gpu-capabilities; Any (default) "
                    "uses whichever controller is connected (BB_GAMEPAD).", width=24)
        self._check(sec, "In-game overlay menu", self.feat_overlay,
                    "The port's overlay menu (Insert key) with the display settings plus the advanced "
                    "options, usable with gamepad, keyboard and mouse. Default: on (BB_OVERLAY).")
        self._check(sec, "Overlay menu on L3 + R3", self.feat_overlay_pad,
                    "Also opens the overlay menu by clicking both sticks (L3 + R3) on the gamepad. "
                    "The Insert key always works. Default: off (BB_OVERLAY_PAD).")
        self._check(sec, "Mouse & keyboard controls", self.feat_mouse_keyboard,
                    "PC-style controls: the mouse turns the camera (captured while playing), left click "
                    "R1, Shift+left R2, right click L2, Shift+right L1, middle click lock-on; Space "
                    "dodge, E interact, R item, X Blood Vial... (full list in the settings dialog). "
                    "Replaces the classic keyboard layout while on; a gamepad keeps working. Can be "
                    "switched in game (Insert > Controls). Default: off (BB_MOUSE_KEYBOARD).")
        mk_btn = ttk.Button(sec, text="Mouse & keyboard settings...", style="Secondary.TButton",
                            command=self.open_mk_dialog)
        mk_btn.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(4, 2))
        sec.next_row += 1
        Tooltip(mk_btn, "Mouse sensitivity, inverted look, smoothing and dead zone, and the list of "
                        "key bindings. Saved to bbport.ini (mk_* keys); a running game picks the "
                        "changes up within a second.")

        sec = self._section(tab, 1, "Audio")
        self._check(sec, "Mute when in background", self.feat_mute_unfocused,
                    "Mutes the game while its window is not focused (e.g. after Alt+Tab). "
                    "Default: on (BB_MUTE_UNFOCUSED).")

        # 6. Party (co-op over the Internet / LAN, BB_PARTY_*)
        self.build_party_tab()

        # 7. Advanced / Debug
        tab = self._tab("Advanced / Debug")
        sec = self._section(tab, 0, "Diagnostics")
        self._check(sec, "Debug tracing (heartbeat/hang dump)", self.feat_tracing,
                    "Extra diagnostic tracing: writes heartbeat logs and hang dumps to out\\ for bug reports. "
                    "Slightly slower. Default: on (BB_TRACE).")
        self._check(sec, "Save frame stats to logs\\", self.feat_save_log,
                    "Writes frame and readback statistics to logs\\<time>.frames.csv / .readbacks.csv for "
                    "performance analysis. Default: off (BB_SAVE_LOG).")

        sec = self._section(tab, 1, "Watchdog")
        self._check(sec, "Watchdog", self.feat_watchdog,
                    "Watches the game for freezes (no frames presented) and writes a hang dump with the "
                    "thread stacks when it hangs. Default: on (BB_WATCHDOG).")
        self._combo(sec, "Timeout (s):", self.timeout_var, TIMEOUT_CHOICES,
                    "Ends the game automatically after this many seconds (for testing). "
                    "0 = no limit, the default (BB_TIMEOUT).", width=12, indent=True)

        self.update_res_scaling_state()
        self.update_party_state()
        self.notebook.bind("<<NotebookTabChanged>>", lambda e: self.refresh_party_code(), add="+")

        # launch / stop
        action = ttk.Frame(main)
        action.pack(fill="x", pady=(0, 8))
        self.launch_btn = ttk.Button(action, text="▶  LAUNCH BLOODBORNE", style="Action.TButton",
                                     command=self.launch_game)
        self.launch_btn.pack(side="left", fill="x", expand=True, ipady=8)
        self.stop_btn = ttk.Button(action, text="■  Stop", style="Secondary.TButton",
                                   command=self.stop_game, state="disabled")
        self.stop_btn.pack(side="right", padx=(10, 0), ipady=8)
        Tooltip(self.launch_btn, "Saves the settings, checks the game files and the build "
                                 "(bbport.exe, run.bat, patches) and starts the game through run.bat. "
                                 "Its output appears in the log below and in launcher.log.")
        Tooltip(self.stop_btn, "Stops the running game and everything run.bat started.")

        # log header
        log_header = ttk.Frame(main)
        log_header.pack(fill="x")
        ttk.Label(log_header, text="Launcher Log:", font=("Segoe UI", 9, "bold")).pack(side="left")
        self.hide_vk_var = tk.BooleanVar(value=self.settings.get("hide_vk", True))
        hide_vk_cb = ttk.Checkbutton(log_header, text="Hide Vulkan warnings", variable=self.hide_vk_var)
        hide_vk_cb.pack(side="right")
        Tooltip(hide_vk_cb, "Hides the noisy Vulkan instance warnings in this panel; "
                            "launcher.log still gets the full output.")

        # bottom button bar
        bar = ttk.Frame(main)
        bar.pack(side="bottom", fill="x", pady=(6, 0))
        for text, command, tip, padx in (
                ("✓ Verify Setup", self.manual_verify_setup,
                 "Checks the game folder (eboot.bin, version 1.09), the compiled bbport.exe, run.bat, "
                 "the patch script and the Vulkan runtime without launching.", 0),
                ("Copy log", self.copy_log, "Copies the log panel's text to the clipboard.", 6),
                ("Clear", self.clear_log, "Clears the log panel (launcher.log is kept).", 0),
                ("Open launcher.log", self.open_log_file,
                 "Opens the full log of the last launch in your text editor.", 6)):
            btn = ttk.Button(bar, text=text, style="Secondary.TButton", command=command)
            btn.pack(side="left", padx=padx)
            Tooltip(btn, tip)

        # log text + scrollbar
        log_frame = ttk.Frame(main)
        log_frame.pack(fill="both", expand=True, pady=(2, 0))
        self.log_text = tk.Text(log_frame, height=8, bg="#111111", fg="#a0a0a0", insertbackground="white",
                                font=("Consolas", 9), relief="flat", wrap="word")
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.log_text.pack(side="left", fill="both", expand=True)

    # Settings tab helpers: a tab holds two side-by-side sections; a section is a grid of
    # checkboxes (full width) and label + combobox rows, each with a tooltip.
    def _tab(self, title: str) -> ttk.Frame:
        tab = ttk.Frame(self.notebook, style="Card.TFrame", padding=(14, 10, 14, 12))
        tab.columnconfigure(0, weight=1, uniform="sections")
        tab.columnconfigure(1, weight=1, uniform="sections")
        self.notebook.add(tab, text=title)
        return tab

    def _section(self, tab, column: int, title: str) -> ttk.Frame:
        sec = ttk.Frame(tab, style="Card.TFrame")
        sec.grid(row=0, column=column, sticky="nw", padx=(0, 16) if column == 0 else 0)
        ttk.Label(sec, text=title.upper(), style="Section.TLabel").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 4))
        sec.next_row = 1
        return sec

    def _check(self, sec, text, var, tip, command=None):
        cb = ttk.Checkbutton(sec, text=text, variable=var, style="Card.TCheckbutton", command=command)
        cb.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=2)
        sec.next_row += 1
        Tooltip(cb, tip)
        return cb

    def _combo(self, sec, text, var, values, tip, width=16, indent=False):
        lbl = ttk.Label(sec, text=text, style="Card.TLabel")
        lbl.grid(row=sec.next_row, column=0, sticky="w", pady=3, padx=(22 if indent else 0, 8))
        combo = ttk.Combobox(sec, textvariable=var, values=values, state="readonly", width=width)
        combo.grid(row=sec.next_row, column=1, sticky="w", pady=3)
        sec.next_row += 1
        Tooltip(lbl, tip)
        Tooltip(combo, tip)
        return combo

    def _entry(self, sec, text, var, tip, width=22, show=None, button=None):
        """Label + text entry row (optionally with a small button after the entry)."""
        lbl = ttk.Label(sec, text=text, style="Card.TLabel")
        lbl.grid(row=sec.next_row, column=0, sticky="w", pady=3, padx=(0, 8))
        holder = ttk.Frame(sec, style="Card.TFrame")
        holder.grid(row=sec.next_row, column=1, sticky="w", pady=3)
        entry = ttk.Entry(holder, textvariable=var, width=width, show=show or "")
        entry.pack(side="left")
        btn = None
        if button is not None:
            btn_text, command, btn_tip = button
            btn = ttk.Button(holder, text=btn_text, style="Secondary.TButton", command=command, width=10)
            btn.pack(side="left", padx=(6, 0))
            Tooltip(btn, btn_tip)
        sec.next_row += 1
        Tooltip(lbl, tip)
        Tooltip(entry, tip)
        return lbl, entry, btn, holder

    # ------------------------------------------------------------- party
    def build_party_tab(self):
        tab = self.party_tab = self._tab("Party")
        sec = self._section(tab, 0, "Party")
        mode_tip = ("Play the game together with friends (co-op). Host a party: your game is the party's "
                    "host and the launcher shows a party code to give to your friends. Join a party: "
                    "enter the host's code. Off: the game runs single-player as before. Party mode "
                    "uses the game's online title path. Default: Off (BB_PARTY=host|join, unset = off).")
        self.party_mode_combo = self._combo(sec, "Mode:", self.party_mode_var, [m[0] for m in PARTY_MODES],
                                            mode_tip, width=16)
        self.party_mode_combo.bind("<<ComboboxSelected>>", lambda e: self.on_party_change())
        name_lbl, self.party_name_entry, _, _ = self._entry(
            sec, "Your name:", self.party_name_var,
            "The name the others see for you: 1-16 letters, digits, _ or -. It is also your online ID "
            "in the game. Default: your Windows user name (BB_PARTY_NAME).", width=18)
        max_tip = ("Most players in the party, you included: 2, 3 (the game's normal co-op limit) or 4 "
                   "(experimental: the game was not made for 4 players, expect issues). Everyone - the "
                   "host and every player who joins - must pick the same value: a different value is "
                   "refused when joining ('party rules differ'). Default: 3 (BB_PARTY_MAX).")
        self.party_max_combo = self._combo(sec, "Max players:", self.party_max_var, PARTY_MAX_CHOICES,
                                           max_tip, width=6)
        auto_cb = self._check(sec, "Automatic summoning", self.party_auto,
                              "The party members are summoned into the host's world automatically (no "
                              "bells, no Insight needed) and come back after a death or a boss. Off: use "
                              "the Beckoning / Small Resonant Bells yourself. Default: on (BB_PARTY_AUTO).",
                              command=self.save_settings)
        seamless_cb = self._check(sec, "Seamless party rules", self.party_seamless,
                                  "Party rules instead of the game's co-op rules: bells work anywhere "
                                  "(also after the area's boss is dead), the session survives boss kills, "
                                  "and guests keep full HP instead of 70%. The host's setting counts. "
                                  "Default: on (BB_PARTY_SEAMLESS).",
                                  command=self.save_settings)
        save_cb = self._check(sec, "Separate party saves", self.party_separate_save,
                              "Party play uses its own save folder (user\\savedata_party), so your "
                              "single-player save (user\\savedata) is never touched by a party session. "
                              "The first time, the launcher offers to copy your single-player save. "
                              "Off (shared): the party plays on your normal save. Either way the game "
                              "backs up the save at every party start and every 10 minutes "
                              "(user\\save_backups, newest 10 kept, both changeable in Rules & sync; "
                              "README.txt there explains restoring). "
                              "Default: on (BB_PARTY_SAVE=separate|shared).",
                              command=self.save_settings)
        copy_btn = ttk.Button(sec, text="Copy single-player save to party save", style="Secondary.TButton",
                              command=self.copy_solo_save_to_party)
        copy_btn.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(4, 2))
        sec.next_row += 1
        Tooltip(copy_btn, "Copies user\\savedata (your single-player characters) to user\\savedata_party, "
                          "the folder party sessions with separate saves play from. Your single-player "
                          "save is only read. An existing party save is backed up to "
                          "user\\save_backups\\savedata_party first.")
        # Rules & sync dialog, the party guide and (developer checkout) the local test in one row
        tools_row = ttk.Frame(sec, style="Card.TFrame")
        tools_row.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(6, 2))
        sec.next_row += 1
        rules_btn = ttk.Button(tools_row, text="Rules & sync...", style="Secondary.TButton",
                               command=self.open_party_rules_dialog)
        rules_btn.pack(side="left")
        Tooltip(rules_btn, "More party options, with their defaults: how the party starts, bells, the "
                           "open world, what happens when a guest dies, rewards for guests, progress / "
                           "story / cutscene sync, save backups and the automatic restart after a crash.")
        help_btn = ttk.Button(tools_row, text="Party help", style="Secondary.TButton",
                              command=self.show_party_help)
        help_btn.pack(side="left", padx=(6, 0))
        Tooltip(help_btn, PARTY_HELP_SHORT)
        self.party_test_btn = None
        if MP_INSTANCES.is_file():
            self.party_test_btn = ttk.Button(tools_row, text="Local test", style="Secondary.TButton",
                                             command=self.run_party_local_test)
            self.party_test_btn.pack(side="left", padx=(6, 0))
            Tooltip(self.party_test_btn, "Developer test: runs tools\\mp\\instances.py setup --count 2, "
                                         "then run --count 2 --seconds 300 (two game instances on this "
                                         "PC, side by side, for 5 minutes). Its output appears in the "
                                         "log below. Needs a built game (out\\bbport.exe).")

        sec = self._section(tab, 1, "Connection")
        port_tip = ("UDP/TCP port of the party on this PC (1024-65535). The host's friends connect to it; "
                    "forward it in your router if UPnP cannot open it. Default: 9307 (BB_PARTY_PORT).")
        port_lbl = ttk.Label(sec, text="Port:", style="Card.TLabel")
        port_lbl.grid(row=sec.next_row, column=0, sticky="w", pady=3, padx=(0, 8))
        port_row = ttk.Frame(sec, style="Card.TFrame")
        port_row.grid(row=sec.next_row, column=1, sticky="w", pady=3)
        sec.next_row += 1
        self.party_port_entry = ttk.Entry(port_row, textvariable=self.party_port_var, width=7)
        self.party_port_entry.pack(side="left")
        upnp_cb = ttk.Checkbutton(port_row, text="UPnP", variable=self.party_upnp, style="Card.TCheckbutton",
                                  command=self.save_settings)
        upnp_cb.pack(side="left", padx=(10, 0))
        Tooltip(port_lbl, port_tip)
        Tooltip(self.party_port_entry, port_tip)
        Tooltip(upnp_cb, "Asks your router (UPnP) to open the party port while the game runs, so friends "
                         "can connect without manual port forwarding. Harmless when the router does not "
                         "support it. Default: on (BB_PARTY_UPNP=1/0).")
        public_lbl, self.party_public_entry, _, _ = self._entry(
            sec, "Public address:", self.party_public_var,
            "Host only: the address put into the party code. Empty (recommended) = found automatically "
            "through the STUN server. Set it for a fixed IP / DNS name or a VPN address, e.g. "
            "203.0.113.5 or 203.0.113.5:9307 (BB_PARTY_PUBLIC_ADDR).")
        stun_lbl, self.party_stun_entry, _, _ = self._entry(
            sec, "STUN server:", self.party_stun_var,
            "Server (host:port) that tells the game its public Internet address for the party code and "
            "connections. 'off' = no STUN (LAN / VPN / fixed public address only). Empty = the default "
            f"{PARTY_DEFAULT_STUN} (BB_PARTY_STUN).")
        pw_lbl, self.party_password_entry, _, _ = self._entry(
            sec, "Password:", self.party_password_var,
            "Optional extra password: the host and every member must enter the same one. The party code "
            "already contains a secret; a password also keeps out people who get hold of the code. "
            "Stored in launcher_settings.json. Default: none (BB_PARTY_PASSWORD).", show="*")
        code_tip = ("The code the host gave you: BBP1- followed by letter/digit groups (letters are not "
                    "case-sensitive), or the host's address as host:port (LAN, VPN, port forwarding). "
                    "(BB_PARTY_CODE)")
        code_lbl, self.party_code_entry, paste_btn, _ = self._entry(
            sec, "Party code:", self.party_code_var, code_tip, width=20,
            button=("Paste", self.paste_party_code, "Pastes the party code from the clipboard."))
        host_tip = ("The code of your party, to give to your friends (they paste it with Join a party). "
                    "The game writes it to party_code.txt in the user folder once it starts hosting, so "
                    "it appears here after the game has started; it changes when your address or port "
                    "changes.")
        host_lbl, self.party_host_code_entry, copy_btn, _ = self._entry(
            sec, "Your code:", self.party_host_code_var, host_tip, width=20,
            button=("Copy code", self.copy_party_code, "Copies your party code to the clipboard."))
        self.party_host_code_entry.state(["readonly"])
        self.party_join_row = [code_lbl, self.party_code_entry.master]
        self.party_host_row = [host_lbl, self.party_host_code_entry.master]
        self.party_status_lbl = ttk.Label(sec, textvariable=self.party_status_var, style="Hint.TLabel",
                                          wraplength=340, justify="left")
        self.party_status_lbl.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(2, 0))
        sec.next_row += 1

        # Fields enabled in each mode: on (host or join), host only, join only
        self.party_widgets_on = [name_lbl, self.party_name_entry, self.party_max_combo, auto_cb, seamless_cb,
                                 save_cb,
                                 port_lbl, self.party_port_entry, upnp_cb, stun_lbl, self.party_stun_entry,
                                 pw_lbl, self.party_password_entry]
        self.party_widgets_host = [public_lbl, self.party_public_entry,
                                   host_lbl, self.party_host_code_entry, copy_btn]
        self.party_widgets_join = [code_lbl, self.party_code_entry, paste_btn]
        for var in (self.party_name_var, self.party_port_var, self.party_code_var):
            var.trace_add("write", lambda *a: self.update_party_hints())
        self.after(2000, self.party_code_tick)

    def party_mode(self) -> str:
        return dict(PARTY_MODES).get(self.party_mode_var.get(), "")

    def on_party_change(self):
        self.update_party_state()
        self.save_settings()

    def update_party_state(self):
        mode = self.party_mode()
        for widgets, enabled in ((self.party_widgets_on, bool(mode)),
                                 (self.party_widgets_host, mode == "host"),
                                 (self.party_widgets_join, mode == "join")):
            for w in widgets:
                w.state(["!disabled"] if enabled else ["disabled"])
        # One code row at a time: the host's own code, or the code to join with
        show, hide = (self.party_host_row, self.party_join_row) if mode == "host" else \
                     (self.party_join_row, self.party_host_row)
        for w in hide:
            w.grid_remove()
        for w in show:
            w.grid()
        if mode == "host":
            self.refresh_party_code()
        self.update_party_hints()

    def party_problems(self) -> list:
        """What keeps the party settings from working (empty when the party is off or fine)."""
        mode = self.party_mode()
        if not mode:
            return []
        problems = []
        if not party_name_ok(self.party_name_var.get().strip()):
            problems.append("Your name: 1-16 letters, digits, _ or - (no spaces).")
        if party_port(self.party_port_var.get()) is None:
            problems.append("Port: a number from 1024 to 65535.")
        if mode == "join":
            problem = party_code_problem(self.party_code_var.get())
            if problem:
                problems.append(f"Party code: {problem}.")
        return problems

    def update_party_hints(self):
        mode = self.party_mode()
        problems = self.party_problems()
        if not mode:
            text = "Party is off: the game runs single-player, as without this tab."
        elif problems:
            text = "✗ " + " ".join(problems)
        elif mode == "host":
            text = ("Give your code to your friends." if self.party_host_code_var.get() else
                    "The code appears here after the game starts hosting.")
        else:
            text = "✓ Party code OK. Start the game after the host's game is running."
        self.party_status_var.set(text)
        self.party_status_lbl.config(foreground="#d06060" if problems else "#888888")

    def refresh_party_code(self):
        code = read_party_code()
        if code != self.party_host_code_var.get():
            self.party_host_code_var.set(code)
            if code and self.party_mode() == "host":
                self.log(f"[PARTY] Party code: {code}")
            self.update_party_hints()

    def party_code_tick(self):
        if self.party_mode() == "host":
            self.refresh_party_code()
        self.after(2000, self.party_code_tick)

    def copy_party_code(self):
        self.refresh_party_code()
        code = self.party_host_code_var.get()
        if not code:
            messagebox.showinfo("No party code yet", "The party code appears after the game starts hosting "
                                f"(it writes {party_user_dir() / 'party_code.txt'}).")
            return
        self.clipboard_clear()
        self.clipboard_append(code)
        self.log("[PARTY] Party code copied to the clipboard.")

    def paste_party_code(self):
        try:
            text = self.clipboard_get()
        except tk.TclError:
            return
        text = " ".join(str(text).split())  # one line, no surrounding spaces
        if text:
            self.party_code_var.set(text)
            self.save_settings()

    def show_party_help(self):
        """How to host / join, in a small window; opens docs\\PARTY.md on request."""
        dlg = tk.Toplevel(self)
        dlg.title("Party help")
        dlg.geometry("620x600")
        dlg.minsize(480, 400)
        dlg.transient(self)
        dlg.configure(bg="#1a1a1a")
        frame = ttk.Frame(dlg, style="Card.TFrame", padding=12)
        frame.pack(fill="both", expand=True, padx=10, pady=(10, 6))
        text = tk.Text(frame, bg="#252526", fg="#e0e0e0", relief="flat", wrap="word",
                       font=("Segoe UI", 10), padx=4, pady=4)
        scroll = ttk.Scrollbar(frame, orient="vertical", command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        text.pack(side="left", fill="both", expand=True)
        text.insert("1.0", PARTY_HELP)
        text.configure(state="disabled")
        bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        bar.pack(fill="x", padx=10, pady=(0, 10))
        if PARTY_GUIDE.is_file():
            guide_btn = ttk.Button(bar, text="Open full guide (PARTY.md)", style="Secondary.TButton",
                                   command=lambda: self.open_party_guide())
            guide_btn.pack(side="left", ipady=4)
            Tooltip(guide_btn, f"Opens {PARTY_GUIDE} in your text editor: ports, firewall, what is "
                               "shared, saves and backups, crash recovery, 4 players, troubleshooting.")
        ttk.Button(bar, text="Close", style="Secondary.TButton", command=dlg.destroy).pack(side="right", ipady=4)

    def open_party_guide(self):
        try:
            os.startfile(str(PARTY_GUIDE))  # type: ignore[attr-defined]
        except (OSError, AttributeError) as ex:
            messagebox.showinfo("Party guide", f"Open this file in a text editor:\n{PARTY_GUIDE}\n\n({ex})")

    def open_party_rules_dialog(self):
        """Party rules & sync: the PARTY_SWITCHES, start mode, guest Insight and save backups."""
        dlg = tk.Toplevel(self)
        dlg.title("Party Rules & Sync")
        dlg.geometry("720x560")
        dlg.minsize(640, 500)
        dlg.transient(self)
        dlg.grab_set()
        dlg.configure(bg="#1a1a1a")
        body = ttk.Frame(dlg, style="Card.TFrame", padding=(14, 10, 14, 12))
        body.pack(fill="both", expand=True, padx=10, pady=(10, 6))
        body.columnconfigure(0, weight=1, uniform="sections")
        body.columnconfigure(1, weight=1, uniform="sections")
        ttk.Label(body, text="Hover over an option for details. Changes apply at the next launch. "
                             "Defaults are the recommended settings.", style="Hint.TLabel").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
        var = self.party_switch_vars
        save = self.save_settings

        def section(row, column, title):
            sec = self._section(body, column, title)
            sec.grid_configure(row=row, pady=(0, 12))
            return sec

        sec = section(1, 0, "Start & world")
        start_combo = self._combo(
            sec, "Party starts:", self.party_start_var, [c[0] for c in PARTY_START_CHOICES],
            "When a player counts as ready to be summoned. After the prologue (recommended): once the "
            "player has seen the opening, reached the Hunter's Dream for the first time and taken a "
            "weapon; until then the others see '(prologue)' next to the name. Right away: as soon as "
            "the opening cutscene is over (summons inside the clinic, before anyone has a weapon - "
            "experimental). Each player's own setting; keep it the same for everyone. "
            "Default: After the prologue (BB_PARTY_START=prologue_solo|immediate).", width=26)
        start_combo.bind("<<ComboboxSelected>>", lambda e: save())
        self._check(sec, "Give the bells", var["party_grant_bells"],
                    "Gives each player the Beckoning Bell and the Small Resonant Bell once the prologue is "
                    "done, so nobody has to buy or find them. Automatic summoning does not need them; they "
                    "are for ringing by hand. Default: on (BB_PARTY_GRANT_BELLS).", command=save)
        self._check(sec, "Open world (no co-op area walls)", var["party_open_world"],
                    "Removes the walls the game puts on area borders while a helper is in your world, "
                    "so the party can walk anywhere the host can. Off: the game's walls stay. Keep it the "
                    "same for everyone. Default: on (BB_PARTY_OPEN_WORLD).", command=save)

        sec = section(1, 1, "Guests")
        self._check(sec, "Respawn at the host's lamp", var["party_guest_respawn"],
                    "A guest who dies comes back at the host's last lamp and is summoned again, instead of "
                    "being sent home to their own world. Applies to you while you are a guest. "
                    "Default: on (BB_PARTY_GUEST_RESPAWN).", command=save)
        self._check(sec, "Refill vials and bullets", var["party_guest_refill"],
                    "Guests get their Blood Vials and Quicksilver Bullets refilled when the host rests at a "
                    "lamp, dies or travels to the Dream, as the host does. Applies to you while you are a "
                    "guest. Default: on (BB_PARTY_GUEST_REFILL).", command=save)
        self._check(sec, "Guests can use Hunter's Mark", var["party_guest_mark"],
                    "Lets a guest use the Hunter's Mark (and Bold Hunter's Mark); it takes the guest to the "
                    "host's last lamp, like a death. Off: guests cannot use it, as in the normal game. "
                    "Default: off (BB_PARTY_GUEST_MARK).", command=save)
        self._check(sec, "Full rewards for guests", var["party_full_rewards"],
                    "Guests get the full Blood Echoes for kills and bosses and the full enemy drops, like the "
                    "host. Off: the game's co-op rules (half echoes, only vials and bullets drop). Applies "
                    "to you while you are a guest. Default: on (BB_PARTY_FULL_REWARDS).", command=save)
        self._check(sec, "Share the host's item rewards", var["party_items"],
                    "Boss drops, key items and gifts the host receives (which guests never get in the "
                    "normal game) are given to each guest too, in their own world, once each. "
                    "Default: on (BB_PARTY_ITEMS).", command=save)
        insight_combo = self._combo(
            sec, "Insight for bosses:", self.party_insight_var, [c[0] for c in PARTY_INSIGHT_CHOICES],
            "Insight a guest gets when the host kills a boss. Same as the host: the guest ends up with "
            "the same Insight the host got for that boss. Full bonus: that much on top of the game's own "
            "+1. Game's own: only the +1 the game gives a helper. "
            "Default: Same as the host (BB_PARTY_GUEST_INSIGHT=parity|full|0).", width=24)
        insight_combo.bind("<<ComboboxSelected>>", lambda e: save())

        sec = section(2, 0, "Progress & story")
        npc_cb = mirror_cb = None

        def update_dependents():
            npc_cb.state(["!disabled"] if var["party_progress"].get() else ["disabled"])
            mirror_cb.state(["!disabled"] if var["party_story"].get() else ["disabled"])
            save()
        self._check(sec, "Share story progress", var["party_progress"],
                    "The host's progress - bosses killed, lamps lit, shortcuts and doors opened, key "
                    "events - is copied into each guest's own save, so the guests' worlds keep up with "
                    "the host's. Needs to be on for the host and for each guest. "
                    "Default: on (BB_PARTY_PROGRESS).", command=update_dependents)
        npc_cb = self._check(sec, "    Also NPC quests (experimental)", var["party_progress_npc"],
                             "Also copies the host's NPC quest steps (Eileen, Alfred, Djura ...) into the "
                             "guests' saves. Quests are chains of steps, so a partly copied quest can get "
                             "stuck. Guest's setting. Default: off (BB_PARTY_PROGRESS_NPC).", command=save)
        self._check(sec, "Share cutscenes and endings", var["party_story"],
                    "Every guest also sees the cutscenes the host triggers (shown again in the guest's own "
                    "world if needed), the time of day (evening, night, Blood Moon) follows the host, and "
                    "the ending the host gets plays for everyone. Needs to be on for the host and for "
                    "each guest. Default: on (BB_PARTY_STORY).", command=update_dependents)
        mirror_cb = self._check(sec, "    Play them live with the host", var["party_story_mirror"],
                                "A guest in the host's world watches the cutscene at the same time as the "
                                "host. Off: the guest sees it later, back in their own world. Guest's "
                                "setting. Default: on (BB_PARTY_STORY_MIRROR).", command=save)

        sec = section(2, 1, "Saves & crashes")
        minutes_combo = self._combo(
            sec, "Back up every (min):", self.party_backup_minutes_var, PARTY_BACKUP_MINUTES_CHOICES,
            "During a party session the game copies the save folder to user\\save_backups at the start "
            "and then every N minutes (0 = only at the start). README.txt there explains restoring. "
            "Default: 10 (BB_SAVE_BACKUP_MINUTES).", width=6)
        minutes_combo.bind("<<ComboboxSelected>>", lambda e: save())
        keep_combo = self._combo(
            sec, "Backups to keep:", self.party_backup_keep_var, PARTY_BACKUP_KEEP_CHOICES,
            "How many backups are kept; the oldest are deleted. Backups that fail the save check are "
            "kept separately, so a broken save never pushes good backups out. "
            "Default: 10 (BB_SAVE_BACKUP_KEEP).", width=6)
        keep_combo.bind("<<ComboboxSelected>>", lambda e: save())
        self._check(sec, "Restart the game after a crash", var["party_restart"],
                    "If the game crashes during a party session it starts again by itself, continues the "
                    "save and rejoins the party (the host keeps your place for 60 s). At most 5 restarts "
                    "in 10 minutes. Default: on (BB_PARTY_RESTART).", command=save)

        update_dependents()

        def on_defaults():
            for key, _, default in PARTY_SWITCHES:
                var[key].set(default)
            self.party_start_var.set(PARTY_START_CHOICES[0][0])
            self.party_insight_var.set(PARTY_INSIGHT_CHOICES[0][0])
            self.party_backup_minutes_var.set("10")
            self.party_backup_keep_var.set("10")
            update_dependents()

        bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        bar.pack(fill="x", padx=10, pady=(0, 10))
        defaults_btn = ttk.Button(bar, text="Defaults", style="Secondary.TButton", command=on_defaults)
        defaults_btn.pack(side="left", ipady=4)
        Tooltip(defaults_btn, "Puts every option in this window back to its default.")
        help_btn = ttk.Button(bar, text="Party help", style="Secondary.TButton", command=self.show_party_help)
        help_btn.pack(side="left", padx=(6, 0), ipady=4)

        def on_close():
            save()
            dlg.destroy()
        ttk.Button(bar, text="Close", style="Secondary.TButton", command=on_close).pack(side="right", ipady=4)
        dlg.protocol("WM_DELETE_WINDOW", on_close)

    def copy_solo_save_to_party(self, ask=True) -> bool:
        """Explicit copy of the single-player save into the party save folder (party_saves.py)."""
        if self.proc is not None:
            messagebox.showwarning("Game running", "Close the game before copying saves.")
            return False
        try:
            ps = party_saves_module()
        except ImportError as ex:
            messagebox.showerror("Party save", f"scripts\\party_saves.py is missing:\n{ex}")
            return False
        user = party_user_dir()
        if not ps.has_saves(user / ps.SOLO):
            messagebox.showinfo("Party save", f"There is no single-player save to copy in:\n{user / ps.SOLO}")
            return False
        replace = ps.has_saves(user / ps.PARTY)
        if replace and not messagebox.askyesno(
                "Replace the party save?",
                f"{user / ps.PARTY} already has a party save.\n\nReplace it with a copy of your "
                f"single-player save? The current party save is first backed up to "
                f"{user / ps.BACKUPS / ps.PARTY}."):
            return False
        if ask and not replace and not messagebox.askokcancel(
                "Copy single-player save",
                f"Copy {user / ps.SOLO} to {user / ps.PARTY}?\n\nYour single-player save is only read."):
            return False
        try:
            backup = ps.copy_solo_to_party(user, replace=replace)
        except (OSError, ValueError) as ex:
            messagebox.showerror("Party save", f"The copy failed; nothing was changed:\n{ex}")
            return False
        self.log(f"[PARTY] Copied {user / ps.SOLO} to {user / ps.PARTY}"
                 + (f" (the old party save is in {backup})" if backup else ""))
        return True

    def offer_party_save_copy(self, env) -> bool:
        """First party session with separate saves: offer the copy. False: do not launch."""
        if not env.get("BB_PARTY") or env.get("BB_PARTY_SAVE") != "separate":
            return True
        try:
            ps = party_saves_module()
        except ImportError:
            return True
        user = party_user_dir()
        if not ps.should_offer_copy(user):
            return True
        answer = messagebox.askyesnocancel(
            "Party save",
            "Party sessions use their own save folder (Separate party saves), and it is empty.\n\n"
            f"Copy your single-player save ({user / ps.SOLO}) into the party save folder "
            f"({user / ps.PARTY})? Your single-player save is only read, never changed.\n\n"
            "Yes: copy it and start\nNo: start with an empty party save (a new character)\n"
            "Cancel: do not start")
        if answer is None:
            return False
        if answer:
            return self.copy_solo_save_to_party(ask=False)
        return True

    def run_party_local_test(self):
        if self.party_test_running:
            self.log("[PARTY TEST] Already running.")
            return
        if not messagebox.askokcancel(
                "Local party test", "Sets up two test instances (tools\\mp\\instances.py) and runs them "
                "side by side for 5 minutes. Continue?"):
            return
        game_args = []
        game_dir = resolve_game_dir(self.eboot_var.get()) if self.eboot_var.get().strip() else None
        if game_dir is not None:
            game_args = ["--game", str(game_dir)]
        steps = [["setup", "--count", "2"] + game_args,
                 ["run", "--count", "2", "--seconds", "300"] + game_args]
        self.party_test_running = True
        self.log("[PARTY TEST] Starting: instances.py setup, then run (2 instances, 300 s).")
        threading.Thread(target=self.party_test_thread, args=(steps,), daemon=True).start()

    def party_test_thread(self, steps):
        """Runs instances.py steps one after the other; output goes to the log panel (strings only
        on log_queue: a tuple there means the game finished)."""
        try:
            for args in steps:
                cmd = [sys.executable, str(MP_INSTANCES)] + args
                self.log_queue.put("[PARTY TEST] > " + " ".join(cmd[1:]))
                try:
                    proc = subprocess.Popen(cmd, cwd=str(APP_DIR), stdin=subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            creationflags=NO_WINDOW)
                except OSError as ex:
                    self.log_queue.put(f"[PARTY TEST] Could not start: {ex}")
                    return
                for raw in iter(proc.stdout.readline, b""):
                    self.log_queue.put("[PARTY TEST] " + decode_line(raw).rstrip())
                proc.stdout.close()
                rc = proc.wait()
                if rc != 0:
                    self.log_queue.put(f"[PARTY TEST] {args[0]} failed with code {rc}; stopped.")
                    return
            self.log_queue.put("[PARTY TEST] Done (instances.py logs --count 2 shows the game logs).")
        finally:
            self.party_test_running = False

    def update_aniso_desc(self):
        curr = self.aniso_var.get()
        desc = next((c[2] for c in ANISO_CHOICES if c[0] == curr), "")
        self.aniso_desc_var.set(f"↳ {desc}" if desc else "")

    def update_res_scaling_state(self):
        if self.feat_res_scaling.get():
            self.res_combo.config(state="readonly")
        else:
            self.res_combo.config(state="disabled")

    def set_vanilla_mode(self):
        self.feat_upscaler.set(False)
        self.feat_object_motion.set(False)
        self.feat_overlay.set(False)
        self.feat_fps_patch.set(False)
        self.feat_mods.set(False)
        self.feat_res_scaling.set(False)
        self.feat_tracing.set(False)
        self.feat_watchdog.set(False)
        self.feat_draw_prep.set(False)
        self.feat_async_shaders.set(False)
        self.feat_gpl.set(False)
        self.feat_compile_indicator.set(True)
        self.feat_shader_precompile.set(True)
        self.feat_hud.set(False)
        self.feat_mouse_keyboard.set(False)
        self.fps_var.set("30")
        self.aniso_var.set("Off")
        self.party_mode_var.set(PARTY_MODES[0][0])
        self.enabled_patches.clear()
        self.sync_quick_patch_vars()
        self.update_aniso_desc()
        self.update_res_scaling_state()
        self.update_party_state()
        self.save_settings()
        self.log("Preset applied: Vanilla mode (all optional features off, FPS preset=30 native, BB_ANISO=0, all patches off, party off).")

    def set_everything_on(self):
        self.feat_upscaler.set(True)
        self.feat_object_motion.set(True)
        self.feat_overlay.set(True)
        self.feat_fps_patch.set(True)
        self.feat_mods.set(True)
        self.feat_res_scaling.set(True)
        self.feat_tracing.set(True)
        self.feat_watchdog.set(True)
        self.feat_draw_prep.set(True)
        self.feat_game_menu.set(True)
        self.feat_skip_network_choice.set(True)
        self.feat_async_shaders.set(True)
        self.feat_gpl.set(True)
        self.feat_compile_indicator.set(True)
        self.feat_shader_precompile.set(True)
        self.fps_var.set("uncap")
        self.aniso_var.set("16x")
        self.enabled_patches = {"Skip Intro", "Performance Patch (perf increase)", "Disable Motion Blur (perf increase)"}
        self.sync_quick_patch_vars()
        self.update_aniso_desc()
        self.update_res_scaling_state()
        self.save_settings()
        self.log("Preset applied: Everything on (restored all defaults and recommended patches).")

    def on_quick_patch_toggle(self):
        if self.feat_skip_intro.get():
            self.enabled_patches.add("Skip Intro")
        else:
            self.enabled_patches.discard("Skip Intro")

        if self.feat_perf_patch.get():
            self.enabled_patches.add("Performance Patch (perf increase)")
        else:
            self.enabled_patches.discard("Performance Patch (perf increase)")

        if self.feat_no_blur.get():
            self.enabled_patches.add("Disable Motion Blur (perf increase)")
        else:
            self.enabled_patches.discard("Disable Motion Blur (perf increase)")

        self.save_settings()

    def sync_quick_patch_vars(self):
        self.feat_skip_intro.set("Skip Intro" in self.enabled_patches)
        self.feat_perf_patch.set("Performance Patch (perf increase)" in self.enabled_patches)
        self.feat_no_blur.set("Disable Motion Blur (perf increase)" in self.enabled_patches)

    def open_patches_dialog(self):
        dlg = tk.Toplevel(self)
        dlg.title(f"Bloodborne XML Patch Manager ({len(self.all_xml_patches)} Patches)")
        dlg.geometry("740x700")
        dlg.minsize(580, 500)
        dlg.transient(self)
        dlg.grab_set()
        dlg.configure(bg="#1a1a1a")

        working_set = set(self.enabled_patches)
        bg_dark = "#1a1a1a"
        bg_card = "#252526"

        top_frame = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        top_frame.pack(fill="x", padx=10, pady=(10, 6))

        filter_row = ttk.Frame(top_frame, style="Card.TFrame")
        filter_row.pack(fill="x", pady=(0, 6))

        ttk.Label(filter_row, text="Search:", style="Card.TLabel").pack(side="left", padx=(0, 4))
        search_var = tk.StringVar()
        search_entry = ttk.Entry(filter_row, textvariable=search_var, width=22)
        search_entry.pack(side="left", padx=(0, 10))

        ttk.Label(filter_row, text="Category:", style="Card.TLabel").pack(side="left", padx=(0, 4))
        cats = ["All Categories", "Performance & Fixes", "Visuals & Camera", "Gameplay & Cheats",
                "Framerate & Engine", "Resolutions & Grids", "Debug & Tools"]
        cat_var = tk.StringVar(value="All Categories")
        cat_combo = ttk.Combobox(filter_row, textvariable=cat_var, values=cats, state="readonly", width=18)
        cat_combo.pack(side="left")

        btn_row = ttk.Frame(top_frame, style="Card.TFrame")
        btn_row.pack(fill="x")

        status_lbl = ttk.Label(btn_row, text="", style="Card.TLabel", font=("Segoe UI", 9, "bold"))
        status_lbl.pack(side="right")

        list_container = ttk.Frame(dlg, style="Card.TFrame", padding=6)
        list_container.pack(fill="both", expand=True, padx=10, pady=(0, 6))

        canvas = tk.Canvas(list_container, bg=bg_dark, highlightthickness=0)
        vscroll = ttk.Scrollbar(list_container, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vscroll.set)

        scrollable_frame = ttk.Frame(canvas, style="Card.TFrame")
        scrollable_frame.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
        )
        canvas_window = canvas.create_window((0, 0), window=scrollable_frame, anchor="nw")

        def on_canvas_configure(e):
            canvas.itemconfig(canvas_window, width=e.width)
        canvas.bind("<Configure>", on_canvas_configure)

        def on_mousewheel(event):
            canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        dlg.bind("<MouseWheel>", on_mousewheel)

        canvas.pack(side="left", fill="both", expand=True)
        vscroll.pack(side="right", fill="y")

        all_patch_vars = {}
        visible_vars = {}
        for p in self.all_xml_patches:
            pname = p["name"]
            all_patch_vars[pname] = tk.BooleanVar(value=(pname in working_set))

        def update_status():
            status_lbl.config(text=f"{len(working_set)} of {len(self.all_xml_patches)} active")

        def make_toggle_cb(pname, var):
            def on_toggle():
                if var.get():
                    working_set.add(pname)
                else:
                    working_set.discard(pname)
                update_status()
            return on_toggle

        def select_visible():
            for p, var in visible_vars.items():
                var.set(True)
                working_set.add(p)
            update_status()

        def deselect_visible():
            for p, var in visible_vars.items():
                var.set(False)
                working_set.discard(p)
            update_status()

        def set_recommended():
            working_set.clear()
            working_set.update(["Skip Intro", "Performance Patch (perf increase)", "Disable Motion Blur (perf increase)"])
            for p, var in all_patch_vars.items():
                var.set(p in working_set)
            update_status()

        ttk.Button(btn_row, text="Select Visible", style="Secondary.TButton",
                   command=select_visible).pack(side="left", padx=(0, 6))
        ttk.Button(btn_row, text="Deselect Visible", style="Secondary.TButton",
                   command=deselect_visible).pack(side="left", padx=(0, 6))
        ttk.Button(btn_row, text="Recommended Defaults", style="Secondary.TButton",
                   command=set_recommended).pack(side="left")

        def refresh_list(*args):
            for widget in scrollable_frame.winfo_children():
                widget.destroy()
            visible_vars.clear()

            q = search_var.get().strip().lower()
            selected_cat = cat_var.get()

            for patch in self.all_xml_patches:
                pname = patch["name"]
                author = patch["author"]
                note = patch["note"]
                cat = patch["category"]

                if selected_cat != "All Categories" and cat != selected_cat:
                    continue

                if q and (q not in pname.lower() and q not in note.lower() and q not in author.lower()):
                    continue

                var = all_patch_vars[pname]
                visible_vars[pname] = var

                item_card = tk.Frame(scrollable_frame, bg=bg_card, padx=8, pady=4, relief="flat",
                                     highlightbackground="#333333", highlightthickness=1)
                item_card.pack(fill="x", padx=4, pady=3)

                header_row = tk.Frame(item_card, bg=bg_card)
                header_row.pack(fill="x")

                cb = tk.Checkbutton(
                    header_row, text=pname, variable=var,
                    command=make_toggle_cb(pname, var),
                    bg=bg_card, fg="#ffffff", selectcolor="#1e1e1e",
                    activebackground=bg_card, activeforeground="#ffffff",
                    font=("Segoe UI", 9, "bold"), anchor="w"
                )
                cb.pack(side="left")

                badge_text = f"by {author}" if author else ""
                if badge_text:
                    tk.Label(header_row, text=badge_text, bg=bg_card, fg="#888888",
                             font=("Segoe UI", 8)).pack(side="left", padx=(8, 0))

                tk.Label(header_row, text=f"[{cat}]", bg=bg_card, fg="#c5a059",
                         font=("Segoe UI", 8)).pack(side="right")

                if note:
                    note_lbl = tk.Label(
                        item_card, text=note, bg=bg_card, fg="#aaaaaa",
                        font=("Segoe UI", 8, "italic"), justify="left", wraplength=640, anchor="w"
                    )
                    note_lbl.pack(fill="x", padx=(24, 0), pady=(1, 2))

        search_var.trace_add("write", refresh_list)
        cat_combo.bind("<<ComboboxSelected>>", refresh_list)

        refresh_list()
        update_status()

        bottom_bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        bottom_bar.pack(fill="x", padx=10, pady=(0, 10))

        def on_save():
            self.enabled_patches = set(working_set)
            self.sync_quick_patch_vars()
            self.save_settings()
            self.log(f"[PATCHES] Saved {len(self.enabled_patches)} active patch(es): "
                     f"{', '.join(sorted(self.enabled_patches)) if self.enabled_patches else 'None'}")
            dlg.destroy()

        def on_cancel():
            dlg.destroy()

        dlg.protocol("WM_DELETE_WINDOW", on_cancel)

        ttk.Button(bottom_bar, text="Save & Apply", style="Action.TButton",
                   command=on_save).pack(side="right", padx=(8, 0), ipady=4)
        ttk.Button(bottom_bar, text="Cancel", style="Secondary.TButton",
                   command=on_cancel).pack(side="right", ipady=4)

    def open_mk_dialog(self):
        """Mouse & keyboard tuning (bbport.ini mk_* keys) and the bindings list. After the dialog of
        Mrsuss60/bloodborne_pc_windows_port, in this launcher's style."""
        ini_path = bbport_ini_path()
        ini = read_ini(ini_path)

        dlg = tk.Toplevel(self)
        dlg.title("Mouse & Keyboard Settings")
        dlg.geometry("640x660")
        dlg.minsize(560, 560)
        dlg.transient(self)
        dlg.grab_set()
        dlg.configure(bg="#1a1a1a")

        main = ttk.Frame(dlg, style="Card.TFrame", padding=14)
        main.pack(fill="both", expand=True, padx=10, pady=(10, 6))
        ttk.Label(main, text="MOUSE LOOK", style="Section.TLabel").grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 4))

        def number(key):
            default, low, high = MK_TUNING[key]
            try:
                return min(max(float(ini.get(key, default)), low), high)
            except ValueError:
                return default

        tips = {
            "mk_sens_x": "How fast the camera turns left/right for a mouse movement. Default 1.0.",
            "mk_sens_y": "How fast the camera tilts up/down for a mouse movement. Default 1.0.",
            "mk_smoothing": "Averages the mouse movement over a few frames: smoother, slightly delayed. "
                            "0 = raw. Default 0.20.",
            "mk_deadzone": "Mouse movement slower than this share of a full stick push is ignored "
                           "(hand tremor). Default 0.05.",
        }
        labels = {"mk_sens_x": "Horizontal sensitivity:", "mk_sens_y": "Vertical sensitivity:",
                  "mk_smoothing": "Smoothing:", "mk_deadzone": "Dead zone:"}
        tuning_vars = {}
        value_labels = {}
        row = 1
        for key, (_, low, high) in MK_TUNING.items():
            var = tk.DoubleVar(value=number(key))
            tuning_vars[key] = var
            lbl = ttk.Label(main, text=labels[key], style="Card.TLabel")
            lbl.grid(row=row, column=0, sticky="w", pady=3)
            value_lbl = ttk.Label(main, text=f"{var.get():.2f}", style="Card.TLabel", width=6)
            value_labels[key] = value_lbl
            scale = ttk.Scale(main, from_=low, to=high, variable=var,
                              command=lambda v, out=value_lbl: out.config(text=f"{float(v):.2f}"))
            scale.grid(row=row, column=1, sticky="ew", padx=8, pady=3)
            value_lbl.grid(row=row, column=2, sticky="w", pady=3)
            for widget in (lbl, scale, value_lbl):
                Tooltip(widget, tips[key] + f" (bbport.ini {key})")
            row += 1
        main.columnconfigure(1, weight=1)

        invert_x = tk.BooleanVar(value=ini.get("mk_invert_x", "0") not in ("", "0"))
        invert_y = tk.BooleanVar(value=ini.get("mk_invert_y", "0") not in ("", "0"))
        inv_row = ttk.Frame(main, style="Card.TFrame")
        inv_row.grid(row=row, column=0, columnspan=3, sticky="w", pady=(4, 8))
        row += 1
        for text, var, key in (("Invert horizontal look", invert_x, "mk_invert_x"),
                               ("Invert vertical look", invert_y, "mk_invert_y")):
            cb = ttk.Checkbutton(inv_row, text=text, variable=var, style="Card.TCheckbutton")
            cb.pack(side="left", padx=(0, 16))
            Tooltip(cb, f"Turns the camera the other way for this mouse axis (bbport.ini {key}).")

        ttk.Label(main, text="BINDINGS (WHILE MOUSE & KEYBOARD CONTROLS ARE ON)", style="Section.TLabel").grid(
            row=row, column=0, columnspan=3, sticky="w", pady=(4, 4))
        row += 1
        binds = ttk.Frame(main, style="Card.TFrame")
        binds.grid(row=row, column=0, columnspan=3, sticky="nsew")
        row += 1
        half = (len(MK_BINDINGS) + 1) // 2
        for index, (keys, action) in enumerate(MK_BINDINGS):
            r, c = index % half, (index // half) * 2
            ttk.Label(binds, text=keys, style="Card.TLabel", font=("Segoe UI", 8, "bold"),
                      foreground="#c5a059").grid(row=r, column=c, sticky="w", padx=(0, 6), pady=1)
            ttk.Label(binds, text=action, style="Card.TLabel", font=("Segoe UI", 8),
                      foreground="#cccccc").grid(row=r, column=c + 1, sticky="w", padx=(0, 18), pady=1)
        ttk.Label(main, text="The mouse is captured while you play and released while the in-game menu "
                             "(Insert) or the name box is open. Rebind keys with mkkey.<input>= lines in "
                             "bbport.ini (key.<input>= lines are the classic layout's). Off: the classic "
                             "layout (WASD / IJKL / Space / Shift ...) is used and the mouse does nothing.",
                  style="Hint.TLabel", wraplength=580, justify="left").grid(
            row=row, column=0, columnspan=3, sticky="w", pady=(8, 0))

        bottom_bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        bottom_bar.pack(fill="x", padx=10, pady=(0, 10))

        def on_save():
            values = {key: f"{var.get():.2f}" for key, var in tuning_vars.items()}
            values["mk_invert_x"] = "1" if invert_x.get() else "0"
            values["mk_invert_y"] = "1" if invert_y.get() else "0"
            try:
                update_ini(ini_path, values)
                self.log(f"[M&K] Saved to {ini_path}: " + ", ".join(f"{k}={v}" for k, v in values.items()))
            except OSError as e:
                self.log(f"[ERROR] Could not save {ini_path}: {e}")
            dlg.destroy()

        def on_defaults():
            for key, var in tuning_vars.items():
                var.set(MK_TUNING[key][0])
                value_labels[key].config(text=f"{var.get():.2f}")  # set() does not run the command
            invert_x.set(False)
            invert_y.set(False)

        def on_cancel():
            dlg.destroy()

        dlg.protocol("WM_DELETE_WINDOW", on_cancel)
        ttk.Button(bottom_bar, text="Save & Apply", style="Action.TButton",
                   command=on_save).pack(side="right", padx=(8, 0), ipady=4)
        ttk.Button(bottom_bar, text="Cancel", style="Secondary.TButton",
                   command=on_cancel).pack(side="right", ipady=4)
        defaults_btn = ttk.Button(bottom_bar, text="Defaults", style="Secondary.TButton", command=on_defaults)
        defaults_btn.pack(side="left", ipady=4)
        Tooltip(defaults_btn, "Sensitivity 1.0, smoothing 0.20, dead zone 0.05, no inverted look.")

    # ------------------------------------------------------------ settings
    def load_settings(self) -> dict:
        try:
            data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def current_settings(self) -> dict:
        gamepad_label = self.gamepad_var.get()
        gamepad = dict(self.gamepad_choices).get(gamepad_label, "")
        return {
            "eboot": self.eboot_var.get().strip(),
            "fps": self.fps_var.get(),
            "res": self.res_var.get(),
            "aniso": self.aniso_var.get(),
            "timeout": self.timeout_var.get().split()[0],
            "mods": self.feat_mods.get(),
            "hide_vk": self.hide_vk_var.get(),
            "feat_upscaler": self.feat_upscaler.get(),
            "feat_object_motion": self.feat_object_motion.get(),
            "feat_overlay": self.feat_overlay.get(),
            "feat_fps_patch": self.feat_fps_patch.get(),
            "feat_res_scaling": self.feat_res_scaling.get(),
            "feat_tracing": self.feat_tracing.get(),
            "feat_watchdog": self.feat_watchdog.get(),
            "feat_draw_prep": self.feat_draw_prep.get(),
            "feat_fullscreen": self.feat_fullscreen.get(),
            "feat_overlay_pad": self.feat_overlay_pad.get(),
            "feat_game_menu": self.feat_game_menu.get(),
            "feat_skip_network_choice": self.feat_skip_network_choice.get(),
            "feat_save_log": self.feat_save_log.get(),
            "feat_mute_unfocused": self.feat_mute_unfocused.get(),
            "feat_async_pipelines": self.feat_async_shaders.get(),
            "feat_gpl": self.feat_gpl.get(),
            "feat_compile_indicator": self.feat_compile_indicator.get(),
            "feat_shader_precompile": self.feat_shader_precompile.get(),
            "feat_hud": self.feat_hud.get(),
            "feat_mouse_keyboard": self.feat_mouse_keyboard.get(),
            "display": dict(self.display_choices).get(self.display_var.get(), ""),
            "gamepad": gamepad,
            "gamepad_name": gamepad_label.removesuffix(" (not connected)") if gamepad else "",
            "preupload": dict(PREUPLOAD_CHOICES).get(self.preupload_var.get(), ""),
            "frames_ahead": dict(FRAMES_AHEAD_CHOICES).get(self.frames_ahead_var.get(), "2"),
            "enabled_patches": sorted(list(self.enabled_patches)),
            "party_mode": self.party_mode(),
            "party_name": self.party_name_var.get().strip(),
            "party_max": self.party_max_var.get(),
            "party_auto": self.party_auto.get(),
            "party_seamless": self.party_seamless.get(),
            "party_separate_save": self.party_separate_save.get(),
            "party_port": self.party_port_var.get().strip(),
            "party_upnp": self.party_upnp.get(),
            "party_public_addr": self.party_public_var.get().strip(),
            "party_stun": self.party_stun_var.get().strip(),
            "party_password": self.party_password_var.get(),
            "party_code": self.party_code_var.get().strip(),
            "party_start": dict(PARTY_START_CHOICES).get(self.party_start_var.get(), PARTY_START_CHOICES[0][1]),
            "party_guest_insight": dict(PARTY_INSIGHT_CHOICES).get(self.party_insight_var.get(),
                                                                   PARTY_INSIGHT_CHOICES[0][1]),
            "party_backup_minutes": self.party_backup_minutes_var.get(),
            "party_backup_keep": self.party_backup_keep_var.get(),
            **{key: var.get() for key, var in self.party_switch_vars.items()},
        }

    def save_settings(self):
        data = self.current_settings()
        try:
            SETTINGS_FILE.write_text(json.dumps(data, indent=2), encoding="utf-8")
        except OSError:
            pass

    # ------------------------------------------------------------- logging
    def insert_lines(self, lines):
        if not lines:
            return
        at_bottom = self.log_text.yview()[1] >= 0.999
        self.log_text.insert("end", "\n".join(lines) + "\n")
        total = int(self.log_text.index("end-1c").split(".")[0])
        if total > MAX_LOG_LINES:
            self.log_text.delete("1.0", f"{total - MAX_LOG_LINES}.0")
        if at_bottom:
            self.log_text.see("end")

    def write_file(self, lines):
        if self.log_fh:
            try:
                self.log_fh.write("\n".join(lines) + "\n")
                self.log_fh.flush()
            except OSError:
                self.log_fh = None

    def log(self, message: str):
        self.write_file([message])
        self.insert_lines([message])

    def poll_queue(self):
        lines, finished = [], None
        try:
            for _ in range(500):
                item = self.log_queue.get_nowait()
                if isinstance(item, tuple):
                    finished = item[1]
                    break
                lines.append(item)
        except queue.Empty:
            pass

        if lines:
            for ln in lines:
                self.check_feature_warnings(ln)
            self.write_file(lines)
            if self.hide_vk_var.get():
                shown = [ln for ln in lines if VK_NOISE not in ln]
                self.hidden_count += len(lines) - len(shown)
                lines = shown
            self.insert_lines(lines)
        if finished is not None:
            self.on_finished(finished)
        self.after(50, self.poll_queue)

    def check_feature_warnings(self, line: str):
        if not self.feat_upscaler.get() and "Upscaler: FSR" in line and "available (on)" in line:
            self.log(f"[WARNING] Feature switch discrepancy: Upscaler is toggled OFF but startup reported: {line.strip()}")
        if not self.feat_object_motion.get() and "Object motion: on" in line:
            self.log(f"[WARNING] Feature switch discrepancy: Object motion is toggled OFF but startup reported: {line.strip()}")
        if not self.feat_overlay.get() and "Overlay: menu ready" in line:
            self.log(f"[WARNING] Feature switch discrepancy: Overlay menu is toggled OFF but startup reported: {line.strip()}")
        if not self.feat_fps_patch.get() and "writes from ['" in line and "FPS++" in line:
            self.log(f"[WARNING] Feature switch discrepancy: FPS patch is toggled OFF but patches reported: {line.strip()}")

    def copy_log(self):
        self.clipboard_clear()
        self.clipboard_append(self.log_text.get("1.0", "end-1c"))

    def clear_log(self):
        self.log_text.delete("1.0", "end")

    def open_log_file(self):
        if not LOG_FILE.exists():
            messagebox.showinfo("No log yet", "launcher.log is created the first time you launch the game.")
            return
        try:
            os.startfile(str(LOG_FILE))
        except (AttributeError, OSError) as ex:
            messagebox.showerror("Error", f"Could not open {LOG_FILE}:\n{ex}")

    def export_cache(self):
        root = shader_cache_root()
        if not root.is_dir() or not any(root.iterdir()):
            messagebox.showinfo("No shader cache", f"There is no shader cache yet in:\n{root}\n\n"
                                "It is written while you play.")
            return
        target = filedialog.asksaveasfilename(
            title="Export shader cache", defaultextension=".zip",
            initialfile=f"bloodborne_shader_cache_{datetime.now():%Y%m%d}.zip",
            filetypes=[("Zip archive", "*.zip")])
        if not target:
            return
        try:
            count = export_shader_cache(Path(target), root)
        except OSError as ex:
            messagebox.showerror("Export failed", str(ex))
            return
        self.log(f"Shader cache exported: {count} files -> {target}")
        messagebox.showinfo("Shader cache exported",
                            f"{count} files saved to:\n{target}\n\nIt contains data derived from the game's "
                            "shaders: share it only with people who own the game.")

    def import_cache(self):
        if self.proc is not None:
            messagebox.showwarning("Game running", "Close the game before importing a shader cache.")
            return
        source = filedialog.askopenfilename(title="Import shader cache",
                                            filetypes=[("Zip archive", "*.zip")])
        if not source:
            return
        import zipfile
        try:
            stats = import_shader_cache(Path(source))
        except (OSError, zipfile.BadZipFile) as ex:
            messagebox.showerror("Import failed", f"Could not read the shader cache archive:\n{ex}")
            return
        if not stats["titles"]:
            messagebox.showwarning("Nothing imported", "The archive contains no shader cache.")
            return
        note = ("" if not stats["gpu_skipped"] else
                f"\n{stats['gpu_skipped']} GPU-specific files were made for another GPU: the game rebuilds "
                "them for yours before the intro.")
        self.log(f"Shader cache imported from {source}: {stats['added']} new files, "
                 f"{stats['existing']} already present, {stats['gpu_skipped']} rebuilt for this GPU")
        messagebox.showinfo("Shader cache imported",
                            f"{stats['added']} new files merged ({stats['existing']} already present)."
                            f"{note}\n\nThe next launch prepares them before the intro.")

    # ------------------------------------------------------------- actions
    def browse_eboot(self):
        current = resolve_game_dir(self.eboot_var.get()) if self.eboot_var.get().strip() else None
        start = current or (DEFAULT_GAME if DEFAULT_GAME.exists() else Path("D:/"))
        chosen = filedialog.askopenfilename(
            title="Select Bloodborne eboot.bin",
            filetypes=[("PS4 Executable (*.bin)", "*.bin"), ("All files", "*.*")],
            initialdir=str(start),
        )
        if chosen:
            chosen = os.path.normpath(chosen)
            self.eboot_var.set(chosen)
            self.log(f"Selected executable: {chosen}")

    def verify_prelaunch(self, game_dir: Path) -> tuple[bool, str, list[str]]:
        """Verify that game directory, EBOOT, compiled source binaries, and scripts are ready."""
        details = []

        # 1. Game directory and eboot.bin
        eboot_path = game_dir / "eboot.bin"
        if not eboot_path.is_file():
            return False, f"Missing eboot.bin in game directory:\n{game_dir}\n\nPlease ensure your dumped game folder contains eboot.bin.", details
        eboot_size = eboot_path.stat().st_size
        if eboot_size == 0:
            return False, f"eboot.bin is empty (0 bytes) in:\n{eboot_path}", details
        eboot_mb = eboot_size / (1024 * 1024)
        details.append(f"[OK] Game executable: eboot.bin ({eboot_mb:.1f} MiB)")

        # 1b. Game Title & Version verification (CUSA03173 1.09)
        try:
            import sys
            scripts_dir = str(APP_DIR / "scripts")
            if scripts_dir not in sys.path:
                sys.path.insert(0, scripts_dir)
            import game_check
            chk = game_check.problem(str(game_dir))
            if chk:
                kind, title, version = chk
                explanation = game_check.explain(kind, title, version)
                if kind == "damaged_files":
                    # The right game, broken by the extraction tool (issue #81): the game would
                    # hang while loading its shaders.
                    return False, (
                        f"Game File Verification Failed (damaged extraction):\n\n"
                        f"{explanation}\n\n"
                        f"Check every file: python scripts\\game_check.py \"{game_dir}\"\n"
                        f"To bypass (not recommended): set environment variable BB_SKIP_GAME_CHECK=1."
                    ), details
                return False, (
                    f"Game File Verification Failed ({kind}):\n\n"
                    f"{explanation}\n\n"
                    f"Expected: Bloodborne CUSA03173 with Update 1.09 merged.\n"
                    f"Why blocked: Upstream port patches and memory hooks rely strictly on the 1.09 executable layout.\n"
                    f"To bypass (not recommended): set environment variable BB_SKIP_GAME_CHECK=1."
                ), details
            else:
                details.append("[OK] Game Title & Version: Bloodborne CUSA03173 v1.09 verified")
        except Exception as e:
            details.append(f"[NOTE] Game version check check skipped: {e}")

        # 2. Check for game asset directory if present
        dvdroot = game_dir / "dvdroot_ps4"
        if dvdroot.is_dir():
            details.append("[OK] Game asset directory: dvdroot_ps4 detected")
        else:
            details.append("[NOTE] dvdroot_ps4 not detected directly under game root (custom layout or loose files)")

        # 3. Launcher script
        if not RUN_BAT.exists():
            return False, f"Launcher script run.bat not found in:\n{APP_DIR}", details
        details.append("[OK] Launcher script: run.bat ready")

        # 4. Compiled bbport.exe
        bbport_exe = APP_DIR / "out" / "bbport.exe"
        if not bbport_exe.is_file():
            return False, (
                f"Compiled game executable 'bbport.exe' was not found in:\n{bbport_exe}\n\n"
                "The source code has not been compiled yet.\n"
                "Please run 'build.bat' in the project directory to compile the project before launching."
            ), details
        bbport_stat = bbport_exe.stat()
        if bbport_stat.st_size == 0:
            return False, f"Compiled 'bbport.exe' is empty (0 bytes) in:\n{bbport_exe}", details
        bbport_time = datetime.fromtimestamp(bbport_stat.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
        details.append(f"[OK] Compiled binary: bbport.exe ({bbport_stat.st_size / (1024*1024):.1f} MiB, built {bbport_time})")

        # 5. GPU subsystem
        bbgpu_dll = APP_DIR / "out" / "bbgpu.dll"
        libbbgpu_a = APP_DIR / "out" / "gpu" / "libbbgpu.a"
        if bbgpu_dll.is_file():
            bbgpu_stat = bbgpu_dll.stat()
            bbgpu_time = datetime.fromtimestamp(bbgpu_stat.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
            details.append(f"[OK] GPU library: bbgpu.dll ({bbgpu_stat.st_size / (1024*1024):.1f} MiB, built {bbgpu_time})")
        elif libbbgpu_a.is_file():
            gpu_stat = libbbgpu_a.stat()
            gpu_time = datetime.fromtimestamp(gpu_stat.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
            details.append(f"[OK] GPU subsystem: statically linked via libbbgpu.a ({gpu_stat.st_size / (1024*1024):.1f} MiB, built {gpu_time})")
        else:
            details.append("[OK] GPU subsystem: statically linked into bbport.exe")

        # 6. Critical runtime libraries staged in out/
        sdl3_dll = APP_DIR / "out" / "SDL3.dll"
        if sdl3_dll.is_file():
            details.append("[OK] Runtime library: out/SDL3.dll staged")
        else:
            details.append("[WARN] Runtime library: out/SDL3.dll not staged in out/")

        # 6. Patches script
        patches_py = APP_DIR / "scripts" / "patches.py"
        if not patches_py.is_file():
            return False, f"Game patch script not found at:\n{patches_py}", details
        details.append("[OK] Patch engine: scripts/patches.py ready")

        # 7. Check Vulkan driver runtime
        try:
            import ctypes
            ctypes.CDLL("vulkan-1.dll")
            details.append("[OK] Vulkan runtime: vulkan-1.dll loaded successfully")
        except OSError:
            details.append("[WARN] Vulkan runtime: vulkan-1.dll not found in standard system paths")

        return True, "", details

    def manual_verify_setup(self):
        target = self.eboot_var.get().strip()
        if not target:
            messagebox.showerror("Error", "Please select eboot.bin or the game folder first.")
            return

        game_dir = resolve_game_dir(target)
        if game_dir is None:
            messagebox.showerror("Invalid Path", f"Could not find eboot.bin in:\n{target}")
            return

        self.log("--- Running Pre-Launch Verification Check ---")
        ready, err_msg, details = self.verify_prelaunch(game_dir)
        for line in details:
            self.log(f"  {line}")

        if ready:
            self.log("[PRE-LAUNCH] All checks PASSED! System and game files are ready.")
            messagebox.showinfo("Verification Passed", "All components are ready for launch!\n\n" + "\n".join(details))
        else:
            self.log(f"[PRE-LAUNCH ERROR] {err_msg.splitlines()[0]}")
            messagebox.showerror("Pre-Launch Verification Failed", err_msg)

    def launch_game(self):
        if self.proc is not None:
            return

        target = self.eboot_var.get().strip()
        if not target:
            messagebox.showerror("Error", "Please select eboot.bin or the game folder first.")
            return

        game_dir = resolve_game_dir(target)
        if game_dir is None:
            messagebox.showerror("Invalid Path", f"Could not find eboot.bin in:\n{target}")
            return

        party_problems = self.party_problems()
        if party_problems:
            self.notebook.select(self.party_tab)
            messagebox.showerror("Party settings", "Fix the Party tab before launching:\n\n"
                                 + "\n".join(party_problems))
            return

        ready, err_msg, details = self.verify_prelaunch(game_dir)
        if not ready:
            self.log(f"[PRE-LAUNCH ERROR] Verification failed: {err_msg.splitlines()[0]}")
            messagebox.showerror("Pre-Launch Verification Failed", err_msg)
            return

        self.log("[PRE-LAUNCH] Pre-flight verification passed:")
        for line in details:
            self.log(f"  {line}")

        env = os.environ.copy()
        env["BB_FROM_LAUNCHER"] = "1"  # run.bat: do not reload launcher_settings.json
        for key, value in settings_env(self.current_settings()).items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
        env["BB_GAME_DIR"] = str(game_dir)
        if not self.offer_party_save_copy(env):
            self.log("[PARTY] Not started (party save copy cancelled).")
            return
        res_choice = self.res_var.get() if self.feat_res_scaling.get() else "Native (1080p, scaling off)"
        active_patches = sorted(list(self.enabled_patches))

        rotate_launcher_log()
        try:
            self.log_fh = open(LOG_FILE, "w", encoding="utf-8")
        except OSError:
            self.log_fh = None

        features_str = (
            f"Features: FSR={'on' if self.feat_upscaler.get() else 'off'}, "
            f"ObjectMotion={'on' if self.feat_object_motion.get() else 'off'}, "
            f"Overlay={'on' if self.feat_overlay.get() else 'off'}, "
            f"FPSPatch={'on' if self.feat_fps_patch.get() else 'off'}, "
            f"Mods={'on' if self.feat_mods.get() else 'off'}, "
            f"ResScaling={'on' if self.feat_res_scaling.get() else 'off'}, "
            f"Tracing={'on' if self.feat_tracing.get() else 'off'}, "
            f"Watchdog={'on' if self.feat_watchdog.get() else 'off'}, "
            f"DrawPrep={'on' if self.feat_draw_prep.get() else 'off'}, "
            f"Fullscreen={'on' if self.feat_fullscreen.get() else 'off'}, "
            f"OverlayPad={'on' if self.feat_overlay_pad.get() else 'off'}, "
            f"GameMenu={'on' if self.feat_game_menu.get() else 'off'}, "
            f"SkipOnlineChoice={'on' if self.feat_skip_network_choice.get() else 'off'}, "
            f"AsyncShaders={'on' if self.feat_async_shaders.get() else 'off'}, "
            f"GPL={'on' if self.feat_gpl.get() else 'off'}, "
            f"CompileIndicator={'on' if self.feat_compile_indicator.get() else 'off'}, "
            f"Precompile={'on' if self.feat_shader_precompile.get() else 'off'}, "
            f"Monitor={self.display_var.get()}, Controller={self.gamepad_var.get()}, "
            f"PreUpload={self.preupload_var.get()}, "
            f"FramesAhead={dict(FRAMES_AHEAD_CHOICES).get(self.frames_ahead_var.get(), '2')}"
        )

        self.hidden_count = 0
        self.log("=" * 50)
        self.log(f"Starting Bloodborne from: {game_dir}   [{datetime.now():%Y-%m-%d %H:%M:%S}]")
        self.log(f"FPS: {self.fps_var.get()} | Res: {res_choice} | Mods: {env['BB_MODS_ENABLED']}")
        self.log(features_str)
        self.log(f"XML Patches ({len(active_patches)} active): {', '.join(active_patches) if active_patches else 'None'}")
        if env.get("BB_PARTY"):
            self.log(f"Party: {env['BB_PARTY']} | Name: {env['BB_PARTY_NAME']} | Port: {env['BB_PARTY_PORT']} | "
                     f"Max: {env['BB_PARTY_MAX']} | Auto: {env['BB_PARTY_AUTO']} | "
                     f"Seamless: {env['BB_PARTY_SEAMLESS']} | Save: {env['BB_PARTY_SAVE']} | "
                     f"UPnP: {env['BB_PARTY_UPNP']}"
                     + (f" | Code: {env['BB_PARTY_CODE']}" if env.get("BB_PARTY_CODE") else ""))
        else:
            self.log("Party: off")
        self.log("=" * 50)

        cmd = ["cmd.exe", "/d", "/c", RUN_BAT.name] if os.name == "nt" else [str(RUN_BAT)]
        try:
            self.proc = subprocess.Popen(
                cmd,
                cwd=str(APP_DIR),
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                creationflags=NO_WINDOW,
            )
        except Exception as ex:
            self.log(f"Launch Error: {ex}")
            self.proc = None
            self.close_log_file()
            return

        self.save_settings()
        self.launch_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        threading.Thread(target=self.reader_thread, args=(self.proc,), daemon=True).start()

    def reader_thread(self, proc: subprocess.Popen):
        try:
            for raw in iter(proc.stdout.readline, b""):
                self.log_queue.put(decode_line(raw).rstrip())
        except Exception as ex:
            self.log_queue.put(f"Launcher read error: {ex}")
        finally:
            try:
                proc.stdout.close()
            except OSError:
                pass
            self.log_queue.put(("exit", proc.wait()))

    def stop_game(self):
        proc = self.proc
        if proc is not None and proc.poll() is None:
            self.log("Stopping game process tree...")
            kill_tree(proc)

    def on_finished(self, rc: int):
        if self.hidden_count:
            self.log(f"({self.hidden_count} Vulkan warning lines hidden; full output is in launcher.log)")
        code = f"{rc}" if 0 <= rc < 256 else f"{rc} (0x{rc & 0xFFFFFFFF:08X})"
        self.log(f"Process finished with code: {code}")
        self.proc = None
        self.close_log_file()
        self.launch_btn.config(state="normal")
        self.stop_btn.config(state="disabled")

    def close_log_file(self):
        if self.log_fh:
            try:
                self.log_fh.close()
            except OSError:
                pass
            self.log_fh = None

    def on_close(self):
        if self.proc is not None and self.proc.poll() is None:
            if not messagebox.askokcancel("Quit", "Bloodborne is still running. Stop it and quit?"):
                return
            kill_tree(self.proc)
        self.save_settings()
        self.destroy()


if __name__ == "__main__":
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == "--write-env":
        write_env_bat(Path(sys.argv[2]))
    else:
        BloodborneLauncher().mainloop()