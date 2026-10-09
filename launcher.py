#!/usr/bin/env python3
"""Bloodborne PC (Windows) Launcher GUI."""

import json
import locale
import os
import queue
import subprocess
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
RES_CHOICES = ["Default (1080p)", "1280x720", "1920x1080", "2560x1440", "3840x2160"]
TIMEOUT_CHOICES = ["0 (no limit)", "10", "30", "60", "120", "300", "600"]
ANISO_CHOICES = [
    ("16x", "16", "16x anisotropic filtering of scene textures (recommended)"),
    ("8x", "8", "8x anisotropic filtering"),
    ("4x", "4", "4x anisotropic filtering"),
    ("2x", "2", "2x anisotropic filtering"),
    ("Off", "0", "Game's default anisotropic filtering (no override)"),
]
# (label, BB_PREUPLOAD value); "" keeps run.bat's default (1)
PREUPLOAD_CHOICES = [("Normal", ""), ("Full (~3 GB more VRAM)", "2"), ("Off", "0")]
# (label, BB_FRAMES_AHEAD value): guest frames the GPU thread may run ahead of the display
FRAMES_AHEAD_CHOICES = [("2 (smooth, default)", "2"), ("1 (lowest input lag)", "1"), ("3 (smoothest)", "3")]
DEFAULT_DISPLAY = "Primary (default)"
DEFAULT_GAMEPAD = "Any (default)"
GPU_CAPS = APP_DIR / "out" / "bb-gpu-capabilities.exe"
MAX_LOG_LINES = 5000
VK_NOISE = "<Warning> vk_instance.cpp"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


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

    # Frame ahead queue (smooth frametimes & bound queue latency: 2 = balanced)
    frames_ahead = str(s.get("frames_ahead", "2"))
    env["BB_FRAMES_AHEAD"] = frames_ahead if frames_ahead in ("1", "2", "3") else "2"

    # Anisotropic filtering (0 = game default, 2/4/8/16 = forced)
    aniso_mapping = {c[0]: c[1] for c in ANISO_CHOICES}
    env["BB_ANISO"] = aniso_mapping.get(s.get("aniso", "16x"), "16")

    # Bloodborne.xml Patches
    active_patches = sorted(s.get("enabled_patches", ["Skip Intro"]))
    env["BB_PATCHES"] = ";".join(active_patches) if active_patches else None
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


class BloodborneLauncher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Bloodborne PC Launcher")
        self.geometry("780x940")
        self.minsize(680, 720)

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

        # Path Entry styling
        style.configure("TEntry",
                        fieldbackground=bg_input,
                        foreground="#ffffff",
                        insertcolor="#ffffff",
                        bordercolor="#3e3e42",
                        lightcolor="#3e3e42",
                        darkcolor="#3e3e42")

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
        main = ttk.Frame(self, padding=16)
        main.pack(fill="both", expand=True)

        ttk.Label(main, text="BLOODBORNE PC", style="Header.TLabel").pack(anchor="w")
        ttk.Label(main, text="Native Windows Port (bbport) Launcher",
                  style="SubHeader.TLabel").pack(anchor="w", pady=(0, 10))

        # eboot.bin selection
        card = ttk.Frame(main, style="Card.TFrame", padding=12)
        card.pack(fill="x", pady=(0, 10))
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
        ttk.Entry(select_frame, textvariable=self.eboot_var,
                  font=("Consolas", 10)).pack(side="left", fill="x", expand=True, padx=(0, 10))
        ttk.Button(select_frame, text="Browse...", style="Secondary.TButton",
                   command=self.browse_eboot).pack(side="right")

        # Performance & Render options
        opts = ttk.Frame(main, style="Card.TFrame", padding=12)
        opts.pack(fill="x", pady=(0, 10))
        ttk.Label(opts, text="Performance & Render Options:", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 8))

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

        ttk.Label(opts, text="Frame Rate Target:", style="Card.TLabel").grid(row=1, column=0, sticky="w", pady=3)
        ttk.Combobox(opts, textvariable=self.fps_var, values=FPS_CHOICES,
                     state="readonly", width=12).grid(row=1, column=1, sticky="w", pady=3, padx=(6, 20))

        ttk.Label(opts, text="Render Resolution:", style="Card.TLabel").grid(row=1, column=2, sticky="w", pady=3)
        self.res_combo = ttk.Combobox(opts, textvariable=self.res_var, values=RES_CHOICES,
                                      state="readonly", width=16)
        self.res_combo.grid(row=1, column=3, sticky="w", pady=3, padx=(6, 0))

        ttk.Label(opts, text="Anisotropic Filtering:", style="Card.TLabel").grid(row=2, column=0, sticky="w", pady=3)
        self.aniso_combo = ttk.Combobox(opts, textvariable=self.aniso_var,
                                        values=[c[0] for c in ANISO_CHOICES],
                                        state="readonly", width=12)
        self.aniso_combo.grid(row=2, column=1, sticky="w", pady=3, padx=(6, 20))
        self.aniso_combo.bind("<<ComboboxSelected>>", lambda e: self.update_aniso_desc())

        self.aniso_desc_lbl = ttk.Label(opts, textvariable=self.aniso_desc_var,
                                        style="Card.TLabel", font=("Segoe UI", 8),
                                        foreground="#888888")
        self.aniso_desc_lbl.grid(row=2, column=2, columnspan=2, sticky="w", pady=3)
        self.update_aniso_desc()

        # Monitor (BB_DISPLAY) and controller (BB_GAMEPAD), listed by bb-gpu-capabilities.exe
        saved_display = str(self.settings.get("display", "") or "")
        self.display_choices = [(DEFAULT_DISPLAY, "")] + [(label, value) for value, label in connected_displays()]
        if saved_display and saved_display not in [v for _, v in self.display_choices]:
            self.display_choices.append((f"{saved_display} (saved)", saved_display))
        self.display_var = tk.StringVar(value=next(
            (label for label, value in self.display_choices if value == saved_display), DEFAULT_DISPLAY))
        ttk.Label(opts, text="Monitor:", style="Card.TLabel").grid(row=3, column=0, sticky="w", pady=3)
        ttk.Combobox(opts, textvariable=self.display_var, values=[c[0] for c in self.display_choices],
                     state="readonly", width=22).grid(row=3, column=1, sticky="w", pady=3, padx=(6, 20))

        saved_gamepad = str(self.settings.get("gamepad", "") or "")
        self.gamepad_choices = [(DEFAULT_GAMEPAD, "")] + [(name, guid) for guid, name in connected_gamepads()]
        if saved_gamepad and saved_gamepad not in [v for _, v in self.gamepad_choices]:
            name = self.settings.get("gamepad_name") or saved_gamepad
            self.gamepad_choices.append((f"{name} (not connected)", saved_gamepad))
        self.gamepad_var = tk.StringVar(value=next(
            (label for label, value in self.gamepad_choices if value == saved_gamepad), DEFAULT_GAMEPAD))
        ttk.Label(opts, text="Controller:", style="Card.TLabel").grid(row=3, column=2, sticky="w", pady=3)
        ttk.Combobox(opts, textvariable=self.gamepad_var, values=[c[0] for c in self.gamepad_choices],
                     state="readonly", width=22).grid(row=3, column=3, sticky="w", pady=3, padx=(6, 0))

        preupload = str(self.settings.get("preupload", ""))
        self.preupload_var = tk.StringVar(value=next(
            (label for label, value in PREUPLOAD_CHOICES if value == preupload), PREUPLOAD_CHOICES[0][0]))
        ttk.Label(opts, text="VRAM pre-upload:", style="Card.TLabel").grid(row=4, column=0, sticky="w", pady=3)
        ttk.Combobox(opts, textvariable=self.preupload_var, values=[c[0] for c in PREUPLOAD_CHOICES],
                     state="readonly", width=22).grid(row=4, column=1, sticky="w", pady=3, padx=(6, 20))
        ttk.Label(opts, text="↳ fewer stutters when areas stream in", style="Card.TLabel",
                  font=("Segoe UI", 8), foreground="#888888").grid(row=4, column=2, columnspan=2, sticky="w", pady=3)

        frames_ahead = str(self.settings.get("frames_ahead", "2"))
        self.frames_ahead_var = tk.StringVar(value=next(
            (label for label, value in FRAMES_AHEAD_CHOICES if value == frames_ahead), FRAMES_AHEAD_CHOICES[0][0]))
        ttk.Label(opts, text="Frames ahead:", style="Card.TLabel").grid(row=5, column=0, sticky="w", pady=3)
        ttk.Combobox(opts, textvariable=self.frames_ahead_var, values=[c[0] for c in FRAMES_AHEAD_CHOICES],
                     state="readonly", width=22).grid(row=5, column=1, sticky="w", pady=3, padx=(6, 20))
        ttk.Label(opts, text="↳ 1 = less input lag, higher = steadier frame times", style="Card.TLabel",
                  font=("Segoe UI", 8), foreground="#888888").grid(row=5, column=2, columnspan=2, sticky="w", pady=3)

        # Features Card
        feat_card = ttk.Frame(main, style="Card.TFrame", padding=12)
        feat_card.pack(fill="x", pady=(0, 10))

        feat_header = ttk.Frame(feat_card, style="Card.TFrame")
        feat_header.pack(fill="x", pady=(0, 8))
        ttk.Label(feat_header, text="Features & Toggles:", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).pack(side="left")

        ttk.Button(feat_header, text="⚙  All XML Patches...", style="Secondary.TButton",
                   command=self.open_patches_dialog).pack(side="left", padx=(12, 0))

        ttk.Button(feat_header, text="Vanilla mode", style="Secondary.TButton",
                   command=self.set_vanilla_mode).pack(side="right", padx=(6, 0))
        ttk.Button(feat_header, text="Everything on", style="Secondary.TButton",
                   command=self.set_everything_on).pack(side="right")

        # Feature Checkbox variables
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

        grid_f = ttk.Frame(feat_card, style="Card.TFrame")
        grid_f.pack(fill="x")

        # Row 0
        ttk.Checkbutton(grid_f, text="Upscaler (FSR 3.1)", variable=self.feat_upscaler,
                        style="Card.TCheckbutton").grid(row=0, column=0, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Object motion vectors", variable=self.feat_object_motion,
                        style="Card.TCheckbutton").grid(row=0, column=1, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="In-game overlay menu", variable=self.feat_overlay,
                        style="Card.TCheckbutton").grid(row=0, column=2, sticky="w", pady=2)

        # Row 1
        ttk.Checkbutton(grid_f, text="FPS patch (Uncap / 60 / 90)", variable=self.feat_fps_patch,
                        style="Card.TCheckbutton").grid(row=1, column=0, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Enable mods folder (mods/)", variable=self.feat_mods,
                        style="Card.TCheckbutton").grid(row=1, column=1, sticky="w", pady=2, padx=(0, 15))
        cb_res = ttk.Checkbutton(grid_f, text="Resolution scaling", variable=self.feat_res_scaling,
                                 style="Card.TCheckbutton", command=self.update_res_scaling_state)
        cb_res.grid(row=1, column=2, sticky="w", pady=2)

        # Row 2
        ttk.Checkbutton(grid_f, text="Debug tracing (heartbeat/hang dump)", variable=self.feat_tracing,
                        style="Card.TCheckbutton").grid(row=2, column=0, sticky="w", pady=2, padx=(0, 15))
        wd_box = ttk.Frame(grid_f, style="Card.TFrame")
        wd_box.grid(row=2, column=1, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(wd_box, text="Watchdog", variable=self.feat_watchdog,
                        style="Card.TCheckbutton").pack(side="left")
        ttk.Label(wd_box, text="Timeout (s):", style="Card.TLabel", font=("Segoe UI", 9)).pack(side="left", padx=(8, 4))
        ttk.Combobox(wd_box, textvariable=self.timeout_var, values=TIMEOUT_CHOICES,
                     state="readonly", width=11).pack(side="left")

        ttk.Checkbutton(grid_f, text="GPU draw prep workers", variable=self.feat_draw_prep,
                        style="Card.TCheckbutton").grid(row=2, column=2, sticky="w", pady=2)

        # Row 3 (Popular XML Patches)
        ttk.Checkbutton(grid_f, text="Skip Intro & Logos", variable=self.feat_skip_intro,
                        style="Card.TCheckbutton", command=self.on_quick_patch_toggle).grid(row=3, column=0, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Performance Patch (Kyo)", variable=self.feat_perf_patch,
                        style="Card.TCheckbutton", command=self.on_quick_patch_toggle).grid(row=3, column=1, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Disable Motion Blur", variable=self.feat_no_blur,
                        style="Card.TCheckbutton", command=self.on_quick_patch_toggle).grid(row=3, column=2, sticky="w", pady=2)

        # Row 4
        ttk.Checkbutton(grid_f, text="Fullscreen", variable=self.feat_fullscreen,
                        style="Card.TCheckbutton").grid(row=4, column=0, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Overlay menu on L3 + R3", variable=self.feat_overlay_pad,
                        style="Card.TCheckbutton").grid(row=4, column=1, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Settings in game's System menu", variable=self.feat_game_menu,
                        style="Card.TCheckbutton").grid(row=4, column=2, sticky="w", pady=2)

        # Row 5
        ttk.Checkbutton(grid_f, text="Skip online/offline choice", variable=self.feat_skip_network_choice,
                        style="Card.TCheckbutton").grid(row=5, column=0, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Save frame stats to logs\\", variable=self.feat_save_log,
                        style="Card.TCheckbutton").grid(row=5, column=1, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Mute when in background", variable=self.feat_mute_unfocused,
                        style="Card.TCheckbutton").grid(row=5, column=2, sticky="w", pady=2)

        self.update_res_scaling_state()

        # launch / stop
        action = ttk.Frame(main)
        action.pack(fill="x", pady=(4, 8))
        self.launch_btn = ttk.Button(action, text="▶  LAUNCH BLOODBORNE", style="Action.TButton",
                                     command=self.launch_game)
        self.launch_btn.pack(side="left", fill="x", expand=True, ipady=8)
        self.stop_btn = ttk.Button(action, text="■  Stop", style="Secondary.TButton",
                                   command=self.stop_game, state="disabled")
        self.stop_btn.pack(side="right", padx=(10, 0), ipady=8)

        # log header
        log_header = ttk.Frame(main)
        log_header.pack(fill="x")
        ttk.Label(log_header, text="Launcher Log:", font=("Segoe UI", 9, "bold")).pack(side="left")
        self.hide_vk_var = tk.BooleanVar(value=self.settings.get("hide_vk", True))
        ttk.Checkbutton(log_header, text="Hide Vulkan warnings",
                        variable=self.hide_vk_var).pack(side="right")

        # bottom button bar
        bar = ttk.Frame(main)
        bar.pack(side="bottom", fill="x", pady=(6, 0))
        ttk.Button(bar, text="✓ Verify Setup", style="Secondary.TButton",
                   command=self.manual_verify_setup).pack(side="left")
        ttk.Button(bar, text="Copy log", style="Secondary.TButton", command=self.copy_log).pack(side="left", padx=6)
        ttk.Button(bar, text="Clear", style="Secondary.TButton", command=self.clear_log).pack(side="left")
        ttk.Button(bar, text="Open launcher.log", style="Secondary.TButton",
                   command=self.open_log_file).pack(side="left", padx=6)

        # log text + scrollbar
        log_frame = ttk.Frame(main)
        log_frame.pack(fill="both", expand=True, pady=(2, 0))
        self.log_text = tk.Text(log_frame, height=8, bg="#111111", fg="#a0a0a0", insertbackground="white",
                                font=("Consolas", 9), relief="flat", wrap="word")
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.log_text.pack(side="left", fill="both", expand=True)

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
        self.fps_var.set("30")
        self.aniso_var.set("Off")
        self.enabled_patches.clear()
        self.sync_quick_patch_vars()
        self.update_aniso_desc()
        self.update_res_scaling_state()
        self.save_settings()
        self.log("Preset applied: Vanilla mode (all optional features off, FPS preset=30 native, BB_ANISO=0, all patches off).")

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
            "display": dict(self.display_choices).get(self.display_var.get(), ""),
            "gamepad": gamepad,
            "gamepad_name": gamepad_label.removesuffix(" (not connected)") if gamepad else "",
            "preupload": dict(PREUPLOAD_CHOICES).get(self.preupload_var.get(), ""),
            "frames_ahead": dict(FRAMES_AHEAD_CHOICES).get(self.frames_ahead_var.get(), "2"),
            "enabled_patches": sorted(list(self.enabled_patches)),
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
        res_choice = self.res_var.get() if self.feat_res_scaling.get() else "Native (1080p, scaling off)"
        active_patches = sorted(list(self.enabled_patches))

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