#!/usr/bin/env python3
"""Bloodborne PC (Windows) Launcher GUI."""

import json
import locale
import os
import hashlib
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
from tkinter import ttk, filedialog, messagebox

APP_DIR = Path(__file__).resolve().parent
# The port's version (VERSION at the repository root; the release has no such file).
PORT_VERSION = "0.7.2"
RUN_BAT = APP_DIR / "run.bat"
LOG_FILE = APP_DIR / "launcher.log"
LOG_DIR = APP_DIR / "logs"
LOG_KEEP = 20
SETTINGS_FILE = APP_DIR / "launcher_settings.json"
UPDATE_CACHE_FILE = APP_DIR / "launcher_update_check.json"  # scripts/update_check.py, 6 h
PATCHES_XML = APP_DIR / "patches" / "Bloodborne.xml"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", "../CUSA03173"))

WINDOW_MODE_CHOICES = ["Windowed", "Borderless", "Fullscreen"]
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

# Motion blur resolution (0.6.2's "Volumetric effects"; idea from a playtester): the game's
# half-resolution motion blur buffers. A game setting: bbport.ini volumetric_res (System > Display),
# read and written by the launcher. (label, value); "In-game setting" was a launcher-only choice.
VOLUMETRIC_LEGACY_INGAME = "In-game setting"
VOLUMETRIC_CHOICES = [
    ("Game default (half)", "0"),
    ("2x pixels", "1"),
    ("Full resolution", "2"),
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
# BBP3- (or the older BBP2- / BBP1-) + Crockford base32 groups (I/L/O accepted as aliases of 1/1/0,
# U is never used). party_code.h has the layouts.
PARTY_CODE_RE = re.compile(r"^BBP[123IL](-[0-9A-HJ-NP-TV-Z]+)+$", re.IGNORECASE)
PARTY_CODE_PREFIXES = ("BBP1", "BBPI", "BBPL", "BBP2", "BBP3")
# Shown wherever a code is asked for or shown (party_code.h PARTY_CODE_EXAMPLE): an Internet code
# with a password; without one it is 3 characters longer, a LAN code 5 shorter.
PARTY_CODE_EXAMPLE = "BBP3-NJR0-E43N-WWN9-2C2P"
PARTY_LAN_CODE_EXAMPLE = "BBP3-PAJY-EAMH-7TD"
PARTY_HOSTPORT_RE = re.compile(r"^(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+):(\d{1,5})$")
MP_INSTANCES = APP_DIR / "tools" / "mp" / "instances.py"
PARTY_GUIDE = APP_DIR / "docs" / "PARTY.md"
# Party rules & sync dialog. Combos: (label, env value); the first entry is the game's default.
PARTY_START_CHOICES = [("After the prologue (recommended)", "prologue_solo"), ("Right away", "immediate")]
PARTY_INSIGHT_CHOICES = [("Same as the host (default)", "parity"), ("Full bonus", "full"),
                         ("Game's own (+1 only)", "0")]
PARTY_BACKUP_MINUTES_CHOICES = ["5", "10", "15", "30", "60", "0"]  # 0 = only at the start
PARTY_BACKUP_KEEP_CHOICES = ["5", "10", "20", "50", "100"]
# Party logging (docs/party/logging.md): BB_PARTY_LOG level and BB_PARTY_LOG_AREAS.
PARTY_LOG_LEVELS = [("Normal", ""), ("Verbose", "verbose"), ("Trace", "trace")]
PARTY_LOG_AREAS = [("net", "Network / link"), ("summon", "Summons / joins"), ("travel", "Travel / loads"),
                   ("death", "Deaths / bloodstains"), ("items", "Items / drops"),
                   ("flags", "Flags / NPCs / events"), ("dialog", "Dialogs / prompts")]
# On/off party options: (settings key, environment variable, default = the game's default).
# Every one is written as 1 / 0 while party mode is on and unset while it is off.
PARTY_HELP_SHORT = ("How to play together: the host picks Host a party and starts the game, then sends the "
                    f"code shown under Internet code (like {PARTY_CODE_EXAMPLE}) to the friends. Each friend "
                    "picks Join a party, pastes the code and starts the game. Everyone needs the same game version (1.09), the same "
                    "gameplay patches and mods and the same Max players. Click for the full steps.")
PARTY_HELP = f"""Party co-op lets 2 to 4 players play the campaign together. It is still in development: it has been tested on one PC, between two PCs on a LAN and over the Internet between two PCs (one 2.5-hour session so far) - expect bugs.

You can also start a party without the launcher: in the game's title screen, Party (New / Join / Continue party game). That is the recommended way; this tab does the same.

HOST
1. Party tab: Mode = Host a party, enter your name. Max players: 3 is the game's normal limit. Optional: a Password (every player enters the same one; it also makes the code shorter).
2. Launch the game and allow the Windows Firewall prompt once. When the game is hosting, your codes appear under Internet code (friends elsewhere), like {PARTY_CODE_EXAMPLE}, and LAN code (players on your own network, same router), like {PARTY_LAN_CODE_EXAMPLE}. Click Copy next to one and send it to your friends.
3. Leave UPnP on. If friends cannot connect, forward the port (default 9307, UDP and TCP) to this PC in your router, or use a VPN such as Tailscale and give them your VPN address as host:port.

JOIN
1. Party tab: Mode = Join a party, enter your name, click Paste next to Party code (the host's code, like {PARTY_CODE_EXAMPLE}). At the host's home, use its LAN code.
2. Enter the host's Password, if it set one, and pick the same Max players as the host.
3. Launch the game after the host's game is running.

IN THE GAME
- Everyone creates a character and plays the short prologue in their own world (until the first visit to the Hunter's Dream and taking a weapon). After that the host's game summons the others automatically (no bells or Insight needed).
- Story progress, boss kills, lamps, shortcuts, items from the host's world and cutscenes are shared with the guests' own saves.
- If a game crashes, it restarts by itself and rejoins the party.

IF JOINING FAILS
- "version mismatch": the bbport build, the game files or params, gameplay patches, gameplay mods, party options or Max players differ from the host's. The message says which and what to do, and names the host's bbport version and yours. The launcher says when a new version is out.
- "wrong password or party code": copy the code again; check the Password field on both sides.
- Nothing happens: the host's port is not reachable. See step 3 for the host.

Saves: the party plays from user\\savedata_party and backs it up to user\\save_backups (README.txt there explains restoring a backup). Right before every party session the game also copies both save folders to user\\save_backups\\party_sessions: "Undo last party session" on this tab puts them back (it shows what it replaces first and keeps your current saves, so the undo can be undone). After a session the launcher checks the party save and offers the undo if something looks wrong.

The full guide is docs\\PARTY.md."""
PARTY_SWITCHES = [
    ("party_restart", "BB_PARTY_RESTART", True),
]
# The party gameplay switches, game settings in bbport.ini (also on the game's System pages Deaths &
# revives, World & PvP, Shared world, Story & dialogue, Party rewards; bbport_settings PartySwitches):
# (bbport.ini key = the launcher_settings.json key before 0.7.0, the variable the launcher set until
# then, the game's default; 0.7.1's: the variable that overrides the ini). The launcher sets no
# variable for them (an explicit BB_PARTY_* still wins).
PARTY_INI_SWITCHES = [
    ("party_host_revive", "BB_PARTY_B2_HOST_REVIVE", True),
    ("party_host_limbo_revive", "BB_PARTY_HOST_LIMBO_REVIVE", True),
    ("party_guest_revive_inplace", "BB_PARTY_GUEST_REVIVE_INPLACE", True),
    ("party_guest_respawn", "BB_PARTY_GUEST_RESPAWN", True),
    ("party_guest_refill", "BB_PARTY_GUEST_REFILL", True),
    ("party_chalice_rejoin", "BB_PARTY_CHALICE_REJOIN", True),
    ("party_guest_mark", "BB_PARTY_GUEST_MARK", True),
    ("party_full_rewards", "BB_PARTY_FULL_REWARDS", True),
    ("party_items", "BB_PARTY_ITEMS", True),
    ("party_grant_bells", "BB_PARTY_GRANT_BELLS", True),
    ("party_open_world", "BB_PARTY_OPEN_WORLD", True),
    ("party_progress", "BB_PARTY_PROGRESS", True),
    ("party_progress_npc", "BB_PARTY_PROGRESS_NPC", True),
    ("party_story", "BB_PARTY_STORY", True),
    ("party_story_mirror", "BB_PARTY_STORY_MIRROR", True),
    # 0.7.1 (no launcher_settings.json key before: nothing to migrate, INI_SYNC_REV stays)
    ("party_kill_record", "BB_PARTY_KILL_RECORD", True),
    ("party_gather", "BB_PARTY_GATHER", True),
    ("party_npc_mirror", "BB_PARTY_NPC_MIRROR", True),
    # 0.7.3: covenant outcomes per player (the host's rule; System > Shared world)
    ("party_covenant_per_player", "BB_PARTY_COVENANT_PER_PLAYER", True),
    # 0.7.3 (D11): NPC hunters fight the whole party (the host's rule; System > Shared world)
    ("party_npc_fights", "BB_PARTY_NPC_FIGHTS", True),
    ("party_key_view", "BB_PARTY_KEY_VIEW", True),
    ("party_key_items_summoned", "BB_PARTY_KEY_ITEMS_SUMMONED", True),
    # 0.7.2: rot (the death debuff, OFF by default since 0.7.2: an unset key is off, a saved value
    # stays) and what else cleanses it (the host's rules; System > Deaths & revives)
    ("party_rot", "BB_PARTY_ROT", False),
    ("party_rot_cleanse_boss", "BB_PARTY_ROT_CLEANSE_BOSS", True),
    ("party_rot_cleanse_killer", "BB_PARTY_ROT_CLEANSE_KILLER", True),
    ("party_rot_cleanse_echoes", "BB_PARTY_ROT_CLEANSE_ECHOES", True),
    # 0.7.3: lamp travel voting (the host's rule, OFF by default; System > Shared world)
    ("party_travel_vote", "BB_PARTY_TRAVEL_VOTE", False),
]
# Stale bbport.exe guard: a safety net, never a reason for the launcher not to start.
try:
    sys.path.insert(0, str(APP_DIR / "scripts"))
    import bb_guard
except Exception:
    bb_guard = None
# Support bundle (scripts/support_bundle.py): logs + crash logs + settings, secrets removed.
try:
    import support_bundle
except Exception:
    support_bundle = None

_instance_mutex = []


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
                if not name or name in WITHHELD_PATCHES:
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
    """'' when `code` is a party code (BBP3- / BBP2- / BBP1-...) or host:port, else what is wrong with it."""
    code = (code or "").strip()
    if not code:
        return f"enter the party code the host gave you (like {PARTY_CODE_EXAMPLE}) or host:port"
    if code.upper().startswith(PARTY_CODE_PREFIXES):
        if not PARTY_CODE_RE.match(code):
            return f"a party code looks like {PARTY_CODE_EXAMPLE} (0-9 and A-Z without U, in groups)"
        return "" if party_code_flags(code) is not None else "this party code has a typo (checksum mismatch)"
    m = PARTY_HOSTPORT_RE.match(code)
    if m:
        port = int(m.group(2))
        return "" if 1 <= port <= 65535 else "the port after : must be 1-65535"
    return f"not a party code (like {PARTY_CODE_EXAMPLE}) or host:port"


PARTY_CODE_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
PARTY_FLAG_LAN, PARTY_FLAG_PASSWORD = 1, 2  # party_code.h kCodeFlagLan / kCodeFlagPasswordRequired


def _crc8(data: bytes) -> int:
    """party_code.h crc8: poly 0x07, init 0."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def _crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# BBP3 check widths 8..12 -> CRC polynomial (party_code.cpp crc_bits)
_CRC_BITS_POLY = {8: 0x07, 9: 0x119, 10: 0x233, 11: 0x385, 12: 0x80F}


def _crc_bits(bits, k: int) -> int:
    """party_code.cpp crc_bits: a k-bit CRC over a bit list, MSB first, init all ones."""
    poly, mask = _CRC_BITS_POLY[k], (1 << k) - 1
    reg = mask
    for b in bits:
        top = ((reg >> (k - 1)) & 1) ^ b
        reg = (reg << 1) & mask
        if top:
            reg ^= poly
    return reg


def _v3_payload_bits(wire: int) -> int:
    """flags(4) | ipv4 32 (LAN: 8) | port 16 (none with flag 8) | secret 48 (password: 32)."""
    return 4 + (8 if wire & PARTY_FLAG_LAN else 32) + (0 if wire & 8 else 16) + (32 if wire & PARTY_FLAG_PASSWORD else 48)


def _v3_symbols(wire: int) -> int:
    return (_v3_payload_bits(wire) + 8 + 4) // 5


def encode_party_code_v3(flags: int, ip, port: int, secret: bytes) -> str:
    """A BBP3 code (party_code.h) - the game writes these; here for tests and examples."""
    wire = (flags & 7) | (8 if port == PARTY_DEFAULT_PORT else 0)
    bits = []

    def put(v, n):
        bits.extend((v >> i) & 1 for i in range(n - 1, -1, -1))

    put(wire, 4)
    for b in (ip[3:] if wire & PARTY_FLAG_LAN else ip):
        put(b, 8)
    if not wire & 8:
        put(port, 16)
    for b in secret[:4 if wire & PARTY_FLAG_PASSWORD else 6]:
        put(b, 8)
    symbols = _v3_symbols(wire)
    k = symbols * 5 - len(bits)
    put(_crc_bits(bits, k), k)
    sym = "".join(PARTY_CODE_ALPHABET[int("".join(map(str, bits[i:i + 5])), 2)] for i in range(0, len(bits), 5))
    return "BBP3" + "".join("-" + sym[i:i + 4] for i in range(0, len(sym), 4))


def _symbols_to_bytes(vals, n):
    """Crockford symbols -> n bytes (zero padding bits), else None."""
    if len(vals) != (n * 8 + 4) // 5:
        return None
    acc = bits = 0
    raw = bytearray()
    for v in vals:
        acc = (acc << 5) | v
        bits += 5
        if bits >= 8:
            raw.append((acc >> (bits - 8)) & 0xFF)
            bits -= 8
    if acc & ((1 << bits) - 1):
        return None
    return bytes(raw)


def party_code_flags(code: str):
    """The flags byte of a valid BBP3 / BBP2 / BBP1 code (checksum checked, as the game decodes it),
    else None (host:port, a typo, empty)."""
    t = (code or "").strip().upper()
    if not t.startswith(PARTY_CODE_PREFIXES):
        return None
    vals = []
    for c in t[4:]:
        if c in "- \t":
            continue
        c = {"I": "1", "L": "1", "O": "0"}.get(c, c)
        if c not in PARTY_CODE_ALPHABET:
            return None
        vals.append(PARTY_CODE_ALPHABET.index(c))
    if t[3] == "3":
        # flags(4) | ipv4 | port | secret | check (party_code.h), bit packed
        if not vals:
            return None
        wire = vals[0] >> 1
        if len(vals) != _v3_symbols(wire):
            return None
        bits = [(v >> i) & 1 for v in vals for i in range(4, -1, -1)]
        payload = _v3_payload_bits(wire)
        k = len(bits) - payload
        check = int("".join(map(str, bits[payload:])), 2)
        if check != _crc_bits(bits[:payload], k):
            return None
        port_ok = wire & 8 or int("".join(map(str, bits[4 + (8 if wire & PARTY_FLAG_LAN else 32):][:16])), 2) != 0
        return wire & 7 if port_ok else None
    if t[3] == "2":
        # flags | ipv4[4] (LAN: last octet) | port[2] (absent with flag 8) | secret[6] | crc8
        if len(vals) < 2:
            return None
        wire = (vals[0] << 3 | vals[1] >> 2) & 0xFF
        size = 1 + (1 if wire & PARTY_FLAG_LAN else 4) + (0 if wire & 8 else 2) + 6 + 1
        raw = _symbols_to_bytes(vals, size)
        if raw is None or _crc8(raw[:-1]) != raw[-1] or wire & ~0x0F:
            return None
        return wire & ~8
    raw = _symbols_to_bytes(vals, 18)
    if raw is None or raw[0] != 1 or _crc16_ccitt(raw[:16]) != (raw[16] << 8 | raw[17]):
        return None
    return raw[1]


def party_password_problem(code: str, password: str) -> str:
    """'' unless the code says the host uses a password and none is entered."""
    flags = party_code_flags(code)
    if flags is not None and flags & PARTY_FLAG_PASSWORD and not (password or "").strip():
        return "This party needs a password"
    return ""


def party_tracing_warning(mode: str, tracing: bool) -> str:
    """The warning for Debug tracing in a party game, or "" (playtest 1: a guest with tracing on
    logged 20 MB per run, hundreds of hang dumps and ran slower)."""
    if not mode or not tracing:
        return ""
    return ("Debug tracing is on (Advanced / Debug): party games run slower with it and write very "
            "large logs. Turn it off unless a developer asked for it.")


def party_user_dir() -> Path:
    """The game's user folder as run.bat finds it: BB_USER_DIR, else <BB_DATA_DIR or launcher dir>\\user."""
    user = os.environ.get("BB_USER_DIR")
    if user:
        return Path(user)
    return Path(os.environ.get("BB_DATA_DIR", str(APP_DIR))) / "user"


def parse_party_codes(text: str) -> tuple:
    """(internet, lan) from party_code.txt ("internet=..." / "lan=..." lines; a bare line counts as
    the Internet code). Either may be ''."""
    internet = lan = ""
    for ln in (text or "").splitlines():
        ln = ln.strip()
        if ln.startswith("internet="):
            internet = ln[len("internet="):].strip()
        elif ln.startswith("lan="):
            lan = ln[len("lan="):].strip()
        elif ln and not internet:
            internet = ln
    return internet, lan


def read_party_codes() -> tuple:
    """(internet, lan): the codes the hosting game wrote to <user dir>\\party_code.txt ('' if none)."""
    try:
        text = (party_user_dir() / "party_code.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "", ""
    return parse_party_codes(text)


def revoke_party_code(user_dir: Path) -> list:
    """New party code (the Party tab's button, the game not running): forgets the host's code
    secret so the next hosting start makes a new code (party_code.h pick_host_secret): deletes
    party_secret.txt, the old codes in party_code.txt and a crash marker (party_state.json: it
    carries the secret too) and drops party.json's host_secret (the menu flow's copy). Returns
    the names of what it changed."""
    changed = []
    for name in ("party_secret.txt", "party_code.txt", "party_state.json"):
        path = user_dir / name
        try:
            path.unlink()
            changed.append(name)
        except FileNotFoundError:
            pass
        except OSError:
            pass
    flow = user_dir / "party.json"
    try:
        data = json.loads(flow.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = None
    if isinstance(data, dict) and "host_secret" in data:
        del data["host_secret"]
        tmp = flow.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        os.replace(tmp, flow)
        changed.append("party.json")
    return changed


def read_party_code() -> str:
    """The code to give friends: the Internet code, else the LAN code ('' if none)."""
    internet, lan = read_party_codes()
    return internet or lan


def party_saves_module():
    """scripts\\party_saves.py: the separate party save folder helpers."""
    scripts_dir = str(APP_DIR / "scripts")
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    import party_saves
    return party_saves


def party_session_module():
    """scripts\\party_session.py: party session snapshots (Undo last party session, the check after)."""
    scripts_dir = str(APP_DIR / "scripts")
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    import party_session
    return party_session


PARTY_RENDEZVOUS_TIP = (
    "When a direct connection fails, your address is posted on the public BitTorrent DHT under a key only "
    "your party can compute, so the host finds you and punches through both routers without a guest code. "
    "DHT nodes can see your IP, like with any torrent client. Both the host and the guest must turn it on. "
    "Off: the guest sends the host its guest code (the host uses Add a guest code), or use port forwarding "
    "or a VPN. Also in the game: System > Party members. Default: off (BB_PARTY_RENDEZVOUS=1/0).")


PARTY_MEMBER_RELAY_TIP = (
    "When a guest cannot reach the host directly but can reach another party member, it joins through "
    "that member, who forwards its traffic to the host (the stuck guest pastes the member's guest code, or "
    "both find each other through the DHT option). Only party members (the party code and password are "
    "checked), at most 4 per member; it uses some of the member's upload (~3 KB/s per relayed player in play, "
    "at most 128 KB/s each). "
    "Also in the game: System > Party network. Default: on (BB_PARTY_MEMBER_RELAY=1/0).")
PARTY_IPV6_TIP = (
    "When both PCs have a global IPv6 address, the party link and the game's traffic try IPv6 before any "
    "relay: no NAT in the way, which helps players behind carrier-grade NAT. A host whose IPv4 port is not "
    "open then puts its IPv6 address into the party code (a longer code that shows this PC's IPv6 address: "
    "a temporary privacy address where Windows has one, so the code changes when it does). Also in the game: System > Party "
    "network. Default: off (BB_PARTY_IPV6=1/0).")


def party_env(s: dict) -> dict:
    """BB_PARTY_* environment from launcher settings; party off unsets every variable."""
    keys = ["BB_PARTY", "BB_PARTY_NAME", "BB_PARTY_PORT", "BB_PARTY_CODE", "BB_PARTY_PASSWORD",
            "BB_PARTY_UPNP", "BB_PARTY_STUN", "BB_PARTY_PUBLIC_ADDR", "BB_PARTY_MAX",
            "BB_PARTY_SEAMLESS", "BB_PARTY_DIRECT_SUMMON", "BB_PARTY_AUTO", "BB_PARTY_SAVE", "BB_PARTY_START",
            "BB_PARTY_GUEST_INSIGHT", "BB_SAVE_BACKUP_MINUTES", "BB_SAVE_BACKUP_KEEP", "BB_PARTY_RENDEZVOUS",
            "BB_PARTY_MEMBER_RELAY", "BB_PARTY_IPV6"]
    keys += [env_name for _, env_name, _ in PARTY_SWITCHES]
    # bbport.ini's (unset here, so a stale variable never overrides the ini; BB_PARTY_GUEST_INSIGHT too)
    keys += [env_name for _, env_name, _ in PARTY_INI_SWITCHES]
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
        env["BB_PARTY_CODE"] = (code.upper() if code.upper().startswith(PARTY_CODE_PREFIXES) else code) or None
    env["BB_PARTY_PASSWORD"] = password or None
    env["BB_PARTY_UPNP"] = "1" if s.get("party_upnp", True) else "0"
    # Hole punching through the public BitTorrent DHT: opt-in (it shares the IP with DHT nodes).
    env["BB_PARTY_RENDEZVOUS"] = "1" if s.get("party_rendezvous", False) else "0"
    # F2 (0.7.1): a member relays for a guest that cannot reach the host (on); IPv6 before any relay (off).
    env["BB_PARTY_MEMBER_RELAY"] = "1" if s.get("party_member_relay", True) else "0"
    env["BB_PARTY_IPV6"] = "1" if s.get("party_ipv6", False) else "0"  # off by default (0.7.1 decision)
    env["BB_PARTY_STUN"] = stun or None  # game default: stun.l.google.com:19302; "off" disables
    env["BB_PARTY_PUBLIC_ADDR"] = (public or None) if mode == "host" else None
    env["BB_PARTY_MAX"] = max_players if max_players in PARTY_MAX_CHOICES else "3"
    env["BB_PARTY_SEAMLESS"] = "1" if s.get("party_seamless", True) else "0"
    env["BB_PARTY_DIRECT_SUMMON"] = "1" if s.get("party_direct_summon", True) else "0"
    env["BB_PARTY_AUTO"] = "1" if s.get("party_auto", True) else "0"
    # Always the party saves (user\savedata_party): co-op sync never touches a single-player save
    # (docs/party/ingame_flow.md); the old "shared" choice is gone.
    env["BB_PARTY_SAVE"] = "separate"
    # Party rules & sync dialog (defaults = the game's defaults, docs/PARTY.md)
    start = str(s.get("party_start", "") or "")
    env["BB_PARTY_START"] = start if start in dict(PARTY_START_CHOICES).values() else PARTY_START_CHOICES[0][1]
    minutes = str(s.get("party_backup_minutes", "10"))
    env["BB_SAVE_BACKUP_MINUTES"] = minutes if minutes in PARTY_BACKUP_MINUTES_CHOICES else "10"
    keep = str(s.get("party_backup_keep", "10"))
    env["BB_SAVE_BACKUP_KEEP"] = keep if keep in PARTY_BACKUP_KEEP_CHOICES else "10"
    for key, env_name, default in PARTY_SWITCHES:
        env[env_name] = "1" if s.get(key, default) else "0"
    return env


def party_log_env(s: dict) -> dict:
    """BB_PARTY_LOG / BB_PARTY_LOG_AREAS from the Advanced tab (None = unset: Normal, every area)."""
    level = str(s.get("party_log_level", "") or "").lower()
    env = {"BB_PARTY_LOG": level if level in ("verbose", "trace") else None, "BB_PARTY_LOG_AREAS": None}
    areas = s.get("party_log_areas")
    known = [key for key, _ in PARTY_LOG_AREAS]
    if env["BB_PARTY_LOG"] and isinstance(areas, list):
        picked = [a for a in known if a in areas]
        if len(picked) < len(known):
            env["BB_PARTY_LOG_AREAS"] = ",".join(picked) if picked else "none"
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
    env["BB_DREAM_MIRROR"] = None if on("dream_mirror") else "0"
    env["BB_LAMP_TRAVEL"] = None if on("lamp_travel") else "0"
    # Game settings (the upscaler, object motion, the HUD, M&K mode, shader options, motion blur
    # resolution, the effect patches) live in bbport.ini only: no BB_* override from the launcher
    # (GAME_SETTING_ENVS; the variables stay for scripts and tests).
    env["BB_OVERLAY"] = None if on("feat_overlay") else "0"
    env["BB_FPS_PATCH"] = None if on("feat_fps_patch") else "0"
    env["BB_FRAME_HOOKS"] = None if on("feat_frame_hooks") else "0"

    # Resolution scaling
    res_choice = s.get("res", RES_CHOICES[0])
    if res_choice not in RES_CHOICES:  # including LEGACY_RES_DEFAULT
        res_choice = RES_CHOICES[0]
    env["BB_RENDER_RES"] = res_choice if on("feat_res_scaling") and res_choice != RES_CHOICES[0] else None

    env["BB_TRACE"] = "1" if on("feat_tracing") else "0"
    env["BB_TIMEOUT"] = str(s.get("timeout", "0")).split()[0]
    env["BB_WATCHDOG"] = None if on("feat_watchdog") else "0"
    env["BB_PREP_WORKERS"] = None if on("feat_draw_prep") else "0"
    # Window mode (Mrsuss60 e2a647d): Windowed / Borderless / Fullscreen; older settings only have
    # the fullscreen toggle. The game reads BB_WINDOW_MODE, BB_FULLSCREEN stays for older builds.
    win_mode = str(s.get("window_mode") or "").lower()
    if win_mode not in ("windowed", "borderless", "fullscreen"):
        win_mode = "fullscreen" if on("feat_fullscreen", bool(s.get("fullscreen", False))) else "windowed"
    env["BB_WINDOW_MODE"] = win_mode
    env["BB_FULLSCREEN"] = "1" if win_mode == "fullscreen" else "0"
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
    # Sound timing log for crackle reports: late buffers, underruns, a summary per minute
    env["BB_AUDIO_LOG"] = "1" if on("feat_audio_log", False) else "0"
    # Party arrival render log for black-world reports: did the 3D scene draw after a party travel
    env["BB_ARRIVAL_LOG"] = "1" if on("feat_arrival_log", False) else "0"
    # Party spectate camera log: camera resets and jumps while a dead player spectates a boss fight
    env["BB_PARTY_SPECTATE_LOG"] = "1" if on("feat_spectate_log", False) else "0"
    # Party host-death lock log for frozen-guest reports: the HostDead locks every 0.25 s on a guest
    env["BB_HOSTDEAD_LOG"] = "1" if on("feat_hostdead_log", False) else "0"
    # Boot freeze dump: thread states when the game's start stands still for 20 s
    env["BB_BOOT_WATCHDOG"] = "20" if on("feat_boot_watchdog", False) else "0"
    # Mute the game while its window is in the background
    env["BB_MUTE_UNFOCUSED"] = "1" if on("feat_mute_unfocused") else "0"
    # Mouse & keyboard mode (bbport.ini mk_enabled), the performance HUD (show_hud), async shaders
    # (async_pipelines), pipeline libraries (gpl) and the shader indicator (compile_indicator) are
    # game settings: bbport.ini only (INI_FLAG_SETTINGS).
    env["BB_SHADER_PRECOMPILE"] = "1" if on("feat_shader_precompile") else "0"

    # Frame ahead queue (smooth frametimes & bound queue latency: 2 = balanced)
    frames_ahead = str(s.get("frames_ahead", "2"))
    env["BB_FRAMES_AHEAD"] = frames_ahead if frames_ahead in ("1", "2", "3") else "2"

    # Anisotropic filtering (0 = game default, 2/4/8/16 = forced)
    aniso_mapping = {c[0]: c[1] for c in ANISO_CHOICES}
    env["BB_ANISO"] = aniso_mapping.get(s.get("aniso", "16x"), "16")

    # Bloodborne.xml Patches; the ones the game's Effects page switches (EFFECT_PATCHES, model LOD)
    # come from bbport.ini through scripts/patches.py, not from here.
    active_patches = sorted(n for n in s.get("enabled_patches", ["Skip Intro"])
                            if n not in EFFECT_PATCHES and n not in MODEL_LOD_PATCHES)
    env["BB_PATCHES"] = ";".join(active_patches) if active_patches else None

    # Party co-op (BB_PARTY=host|join; off = every BB_PARTY_* unset)
    env.update(party_env(s))
    env.update(party_log_env(s))  # without a party too: user marks (F9) work in solo games
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
    # run.bat without the launcher after an update: the old launcher values move to bbport.ini once.
    try:
        migrate_ini_rev(bbport_ini_path())
        if migrate_settings_to_ini(data, bbport_ini_path()) is not None:
            settings_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    except OSError:
        pass
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
    ("Mouse wheel", "D-pad up / down: quick items, tools"),
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
    ("V / H", "Party: ping / gather the party (host)"),
]


# Frame generation (from Supermedo/bloodborne_pc PR #69 by FIocker): a game setting in bbport.ini
# (frame_generation / fg_backend, also in the in-game menus), applied at the next start. Off by
# default. (label, frame_generation, fg_backend)
FRAME_GENERATION_CHOICES = [
    ("Off", "off", "dlss"),
    ("DLSS FG 2x", "2x", "dlss"),
    ("DLSS FG 3x", "3x", "dlss"),
    ("DLSS FG 4x", "4x", "dlss"),
    ("FSR FG 2x", "2x", "fsr"),
]


# NVIDIA Reflex (same PR): bbport.ini reflex, Off by default. (label, value)
REFLEX_CHOICES = [("Off", "off"), ("On", "on"), ("On + Boost", "boost")]


# Button prompts (from deadinside28/bloodborne_pc 72e5c8ce): bbport.ini prompt_style, a game setting
# (also in game: Mouse & keyboard page, Insert > Controls), live. (label, value)
PROMPT_STYLE_CHOICES = [("Auto", "auto"), ("PlayStation", "playstation"), ("Xbox", "xbox"),
                        ("Nintendo", "nintendo"), ("Keyboard", "keyboard")]


def prompt_style_label(ini: dict) -> str:
    value = str(ini.get("prompt_style", "auto")).strip().lower()
    if value.isdigit() and int(value) < len(PROMPT_STYLE_CHOICES):
        value = PROMPT_STYLE_CHOICES[int(value)][1]
    return next((label for label, v in PROMPT_STYLE_CHOICES if v == value), PROMPT_STYLE_CHOICES[0][0])


def reflex_label(ini: dict) -> str:
    value = str(ini.get("reflex", "off")).lower()
    value = {"1": "on", "2": "boost"}.get(value, value)
    return next((label for label, v in REFLEX_CHOICES if v == value), REFLEX_CHOICES[0][0])


# Native HDR output (docs/party/ERSC_PARITY.md I4): game settings in bbport.ini (hdr,
# hdr_peak_nits, hdr_paper_white; also System > HDR in the game), off by default; on/off applies
# at the next start, the curve live. (label, value)
HDR_PEAK_CHOICES = [("Auto (display)", "auto"), ("400 nits", "400"), ("600 nits", "600"),
                    ("800 nits", "800"), ("1000 nits", "1000"), ("1500 nits", "1500"),
                    ("2000 nits", "2000"), ("4000 nits", "4000")]
HDR_PAPER_WHITE_CHOICES = [("80 nits", "80"), ("100 nits", "100"), ("150 nits", "150"),
                           ("200 nits (default)", "200"), ("250 nits", "250"), ("300 nits", "300"),
                           ("400 nits", "400")]


def hdr_peak_label(ini: dict) -> str:
    value = str(ini.get("hdr_peak_nits", "auto")).strip().lower()
    if value in ("", "0"):
        value = "auto"
    for label, v in HDR_PEAK_CHOICES:
        if v == value:
            return label
    return f"{value} nits" if value.isdigit() else HDR_PEAK_CHOICES[0][0]


def hdr_paper_white_label(ini: dict) -> str:
    value = str(ini.get("hdr_paper_white", "200")).strip()
    for label, v in HDR_PAPER_WHITE_CHOICES:
        if v == value:
            return label
    return f"{value} nits" if value.isdigit() else "200 nits (default)"


def hdr_ini(enabled: bool, peak_label: str, paper_label: str) -> dict:
    """bbport.ini keys of the launcher's HDR options (a custom value from the game menu is kept)."""
    def value_of(label, choices, fallback):
        for name, v in choices:
            if name == label:
                return v
        digits = label.split(" ")[0]
        return digits if digits.isdigit() else fallback
    return {"hdr": "1" if enabled else "0",
            "hdr_peak_nits": value_of(peak_label, HDR_PEAK_CHOICES, "auto"),
            "hdr_paper_white": value_of(paper_label, HDR_PAPER_WHITE_CHOICES, "200")}


def frame_generation_label(ini: dict) -> str:
    """The launcher's choice for bbport.ini's frame_generation / fg_backend (Off when unset)."""
    factor = str(ini.get("frame_generation", "off")).lower().rstrip("x")
    backend = "fsr" if str(ini.get("fg_backend", "dlss")).lower() in ("fsr", "1") else "dlss"
    if factor not in ("2", "3", "4"):
        return FRAME_GENERATION_CHOICES[0][0]
    if backend == "fsr":
        return "FSR FG 2x"
    return f"DLSS FG {factor}x"


def frame_generation_ini(label: str) -> dict:
    for name, factor, backend in FRAME_GENERATION_CHOICES:
        if name == label:
            return {"frame_generation": factor, "fg_backend": backend}
    return {"frame_generation": "off", "fg_backend": "dlss"}


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


def ini_flag(path: Path, key: str, default: bool = True) -> bool:
    """A 0/1 game setting from bbport.ini (party_hud, party_name_tags): `default` when not set."""
    value = read_ini(path).get(key)
    return default if value is None or value == "" else value != "0"


def update_ini(path: Path, values: dict):
    """Sets keys of bbport.ini, keeping its other lines (the game's settings, key.* bindings)."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        lines = ["# bbport settings (in-game menu: Insert; L3+R3 with BB_OVERLAY_PAD=1)"]
        values = dict(values)
        values.setdefault("settings_rev", str(INI_SETTINGS_REV))  # a new file: nothing to migrate
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


# ---- Game settings: bbport.ini is their one source of truth ----
# Settings that are also in the game's menus (System pages, Insert overlay) are read from bbport.ini
# when the launcher opens (and again when the game exits or the window gets the focus) and written
# there on change; the launcher sets no BB_* override for them at launch. Launch-only options stay
# in launcher_settings.json.

# Upscaler (bbport.ini upscaler=off|fsr3|fsr4|fsr411|taa|dlss): the game menu's labels and order
# (bbport_game_menu_pages.cpp "Upscaler" row). "Enable upscaler" off writes off.
UPSCALER_CHOICES = [("FSR 3.1", "fsr3"), ("FSR 4", "fsr4"), ("FSR 4.1.1", "fsr411"), ("TAA", "taa"),
                    ("DLSS", "dlss")]
UPSCALER_DEFAULT = "FSR 3.1"


def upscaler_state(ini: dict, remembered: str = "") -> tuple:
    """(enabled, label) for bbport.ini's upscaler; when off, the label is the last choice the
    launcher remembered (launcher_settings.json upscaler_choice), else FSR 3.1."""
    value = str(ini.get("upscaler", "off")).strip().lower()
    for label, name in UPSCALER_CHOICES:
        if name == value:
            return True, label
    return False, remembered if remembered in dict(UPSCALER_CHOICES) else UPSCALER_DEFAULT


def upscaler_ini(enabled: bool, label: str) -> dict:
    return {"upscaler": dict(UPSCALER_CHOICES).get(label, "fsr3") if enabled else "off"}


# 0/1 game settings: (launcher_settings.json key before 0.6.8, bbport.ini key, the game's default
# without the key, the old launch variable)
INI_FLAG_SETTINGS = [
    ("feat_object_motion", "object_motion", False, "BB_OBJECT_MOTION"),
    ("feat_hud", "show_hud", False, "BB_HUD"),
    ("feat_mouse_keyboard", "mk_enabled", False, "BB_MOUSE_KEYBOARD"),
    ("feat_async_pipelines", "async_pipelines", True, "BB_ASYNC_SHADERS"),
    ("feat_gpl", "gpl", False, "BB_GPL"),
    ("feat_compile_indicator", "compile_indicator", True, "BB_COMPILE_INDICATOR"),
]
# Every BB_* variable the launcher no longer sets because its setting lives in bbport.ini.
GAME_SETTING_ENVS = ["BB_UPSCALER", "BB_VOLUMETRIC_RES"] + [env for *_, env in INI_FLAG_SETTINGS]


def ini_flag_value(ini: dict, key: str, default: bool) -> bool:
    value = str(ini.get(key, "")).strip()
    return default if value == "" else value != "0"


def party_insight_label(ini: dict) -> str:
    """bbport.ini party_guest_insight (parity / full / 0) as the Party Rules & Sync combo's label."""
    value = str(ini.get("party_guest_insight", "")).strip().lower()
    value = "0" if value == "off" else value
    return next((label for label, v in PARTY_INSIGHT_CHOICES if v == value), PARTY_INSIGHT_CHOICES[0][0])


def party_rules_ini(switches: dict, insight_label: str) -> dict:
    """bbport.ini values for the party gameplay switches ({key: bool}) and the Insight combo."""
    values = {key: "1" if switches.get(key, default) else "0" for key, _, default in PARTY_INI_SWITCHES}
    values["party_guest_insight"] = dict(PARTY_INSIGHT_CHOICES).get(insight_label, PARTY_INSIGHT_CHOICES[0][1])
    return values


def volumetric_label(ini: dict) -> str:
    """bbport.ini volumetric_res (0/1/2, or game/2x/full as the game also reads) as a launcher label."""
    value = str(ini.get("volumetric_res", "0")).strip().lower()
    value = {"full": "2", "2x": "1", "double": "1", "game": "0", "off": "0"}.get(value, value)
    return next((label for label, v in VOLUMETRIC_CHOICES if v == value), VOLUMETRIC_CHOICES[0][0])


def volumetric_ini(label: str) -> dict:
    return {"volumetric_res": dict(VOLUMETRIC_CHOICES).get(label, "0")}


# Community patches the game's Effects page switches (scripts/patches.py EFFECTS / MODEL_LOD,
# bbport_settings.h Effects): patch name -> (bbport.ini key, the value that applies the patch).
# A missing key is the game's default, which never applies one of these patches.
EFFECT_PATCHES = {
    "Disable Chromatic Aberration": ("effect_chromatic_aberration", "0"),
    "Disable DoF": ("effect_dof", "0"),
    "Disable Motion Blur (perf increase)": ("effect_motion_blur", "0"),
    "Disable SSAO": ("effect_ssao", "0"),
    "Disable AA": ("effect_game_aa", "0"),
    "Disable Dynamic Light Shadows (perf increase)": ("effect_dynamic_shadows", "0"),
    "Enable Screen Space Reflections (READ NOTE)": ("effect_ssr", "1"),
    "Skip Intro": ("skip_intro", "1"),
    "Restore Debug Camera": ("debug_camera", "1"),
    "Restore Debug Menu (READ NOTES)": ("debug_menu", "1"),
}
# "Model LOD 2 (Lowest)" is not offered (WITHHELD_PATCHES): the game then draws nothing for a model
# without a third level of detail, and some map floors have none (the Lecture Building's floor
# showed the background through it). An older settings file that has it gets the Lower patch.
# "Model LOD -2 (Highest)" is not offered either: many objects past their nearest level of detail
# (10-15 m) are then not drawn at all (Central Yharnam's corpses popped in and out); an older
# settings file that has it gets the game's own detail.
MODEL_LOD_PATCHES = {"Model LOD -2 (Highest)": "0", "Model LOD 1 (Lower)": "1", "Model LOD 2 (Lowest)": "1"}
WITHHELD_PATCHES = {"Model LOD 2 (Lowest)", "Model LOD -2 (Highest)"}


def effect_patches_from_ini(ini: dict) -> set:
    names = {name for name, (key, on) in EFFECT_PATCHES.items() if str(ini.get(key, "")).strip() == on}
    lod = str(ini.get("model_lod", "0")).strip()
    names |= {name for name, value in MODEL_LOD_PATCHES.items()
              if value == lod and name not in WITHHELD_PATCHES}
    return names


def effect_patches_ini(names) -> dict:
    """bbport.ini values for the effect patches in `names` (every effect key and model_lod)."""
    values = {key: (on if name in names else ("1" if on == "0" else "0"))
              for name, (key, on) in EFFECT_PATCHES.items()}
    values["model_lod"] = next((v for n, v in MODEL_LOD_PATCHES.items() if n in names and v != "0"), "0")
    return values


# bbport.ini settings_rev: one-time changes of saved game settings, shared with the game
# (bbport_settings.h kSettingsRev; whichever starts first does them, once).
#   1 (0.7.2): party_rot = 0 once (rot became off by default; 0.6.0 - 0.7.1 games wrote their default
#              party_rot=1 at every settings save, chosen or not). A later explicit 1 stays.
INI_SETTINGS_REV = 1


def migrate_ini_rev(ini_path: Path):
    """bbport.ini's one-time changes (settings_rev). Returns the values written, None when none."""
    if not ini_path.exists():
        return None  # a new bbport.ini starts at the current revision (update_ini, the game)
    ini = read_ini(ini_path)
    try:
        rev = int(str(ini.get("settings_rev", "0")).strip() or 0)
    except ValueError:
        rev = 0
    if rev >= INI_SETTINGS_REV:
        return None
    values = {"settings_rev": str(INI_SETTINGS_REV)}
    if rev < 1:
        values["party_rot"] = "0"
    update_ini(ini_path, values)
    return values


INI_SYNC_REV = 2  # launcher_settings.json ini_sync_rev: the old launcher values moved to bbport.ini
# 2 (0.7.0): the party gameplay switches (PARTY_INI_SWITCHES) and party_guest_insight.


def migrate_settings_to_ini(s: dict, ini_path: Path):
    """Moves the game settings of a launcher_settings.json from before 0.6.8 into bbport.ini, once.

    A key is written when bbport.ini lacks it, or when the old launcher value overrode the ini at
    every launch (it was what the game ran with); otherwise the ini already held the game's value.
    From then on the ini wins. Returns the values written ({} = none), None when already done."""
    rev = int(s.get("ini_sync_rev", 0) or 0)
    if rev >= INI_SYNC_REV:
        return None
    ini = read_ini(ini_path)
    values = {}
    # Rev 2: the party switches were set (BB_PARTY_*=1/0) at every party launch: a value the old
    # launcher_settings.json holds wins; one it lacks was the game's default.
    for key, _, _ in PARTY_INI_SWITCHES:
        if key in s:
            values[key] = "1" if s.pop(key) else "0"
    insight = s.pop("party_guest_insight", None)
    if insight in dict(PARTY_INSIGHT_CHOICES).values():
        values["party_guest_insight"] = insight
    if rev >= 1:
        if values:
            update_ini(ini_path, values)
        s["ini_sync_rev"] = INI_SYNC_REV
        return values

    def put(key, value, forced):
        if forced or key not in ini:
            values[key] = value

    upscaler_on = bool(s.get("feat_upscaler", True))
    put("upscaler", "fsr3" if upscaler_on else "off", not upscaler_on)  # BB_UPSCALER=off when unchecked
    for old, key, default, env in INI_FLAG_SETTINGS:
        launcher_default = {"feat_object_motion": True}.get(old, default)
        value = bool(s.get(old, launcher_default))
        # BB_OBJECT_MOTION only when off, BB_HUD only when on; the others were always set.
        forced = (not value if env == "BB_OBJECT_MOTION" else value if env == "BB_HUD" else True)
        put(key, "1" if value else "0", forced)
    volumetric = s.get("volumetric_res", VOLUMETRIC_LEGACY_INGAME)
    if volumetric in dict(VOLUMETRIC_CHOICES):
        put("volumetric_res", dict(VOLUMETRIC_CHOICES)[volumetric], True)
    # BB_PATCHES applied a launcher patch whatever the ini said: such a patch is written as on.
    patches = set(s.get("enabled_patches", ["Skip Intro"]))
    for name, (key, on) in EFFECT_PATCHES.items():
        if name in patches:
            values[key] = on
    lod = next((v for n, v in MODEL_LOD_PATCHES.items() if n in patches and v != "0"), None)
    if lod:
        values["model_lod"] = lod
    if values:
        update_ini(ini_path, values)
    s["ini_sync_rev"] = INI_SYNC_REV
    return values


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


CACHE_DRIVER = ".vkp"   # the driver's own pipeline binaries (VkPipelineCache), one GPU + driver
CACHE_IMPORT_DIR = "cache_import"  # user\cache_import\*.zip: merged at the next launch


def _cache_entries(zf) -> dict:
    """{title: [(entry, file name, gpu label or "")]} of a cache .zip. Two layouts: the launcher's
    export (`<title>/<file>`) and a shared bundle, which keeps the GPU-built part of each GPU in
    its own folder (`gpu/<label>/<title>/<file>`; README and other files at the top are ignored)."""
    entries = {}
    for n in zf.namelist():
        parts = n.split("/")
        if len(parts) == 2:
            title, fname, label = parts[0], parts[1], ""
        elif len(parts) == 4 and parts[0] == "gpu" and _cache_name_ok(parts[1]):
            label, title, fname = parts[1], parts[2], parts[3]
        else:
            continue
        if _cache_name_ok(title) and _cache_name_ok(fname):
            entries.setdefault(title, []).append((n, fname, label))
    return entries


def import_shader_cache(zip_path: Path, root: Path = None) -> dict:
    """Merges a cache .zip into the local cache without overwriting anything. Keys and sources
    are always taken; GPU-built .meta/.spv (and the driver's .vkp) only from the part made for
    the same GPU profile (otherwise the game rebuilds them from the sources before the intro).
    With no local cache yet the first GPU part is taken: the game checks its profile itself and
    rebuilds what does not fit, and loads a .vkp only when its header names this GPU and driver."""
    import zipfile
    root = root or shader_cache_root()
    stats = {"added": 0, "existing": 0, "gpu_skipped": 0, "titles": [], "gpu_used": []}
    with zipfile.ZipFile(zip_path) as zf:
        for title, files in sorted(_cache_entries(zf).items()):
            dest = root / title
            local_profile = dest / CACHE_PROFILE
            local = local_profile.read_bytes() if local_profile.is_file() else None
            labels = sorted({label for _, _, label in files})
            profiles = {label: next((zf.read(n) for n, f, lb in files if f == CACHE_PROFILE and lb == label), None)
                        for label in labels}
            if local is not None:
                usable = [lb for lb in labels if profiles[lb] is not None and profiles[lb] == local]
            else:  # empty cache: the game checks the profile itself
                usable = [lb for lb in labels if profiles[lb] is not None]
            use = usable[0] if usable else None
            if use:
                stats["gpu_used"].append(use)
            dest.mkdir(parents=True, exist_ok=True)
            stats["titles"].append(title)
            for n, fname, label in files:
                suffix = Path(fname).suffix
                if fname == CACHE_PROFILE:
                    if local is None and label == use:
                        local_profile.write_bytes(zf.read(n))
                    continue
                if suffix not in CACHE_GPU_INDEPENDENT | CACHE_GPU_SPECIFIC | {CACHE_DRIVER}:
                    continue
                if suffix in CACHE_GPU_SPECIFIC | {CACHE_DRIVER} and label != use:
                    stats["gpu_skipped"] += 1
                    continue
                target = dest / fname
                if target.exists():
                    stats["existing"] += 1
                    continue
                target.write_bytes(zf.read(n))
                stats["added"] += 1
    return stats


VENDOR_NAMES = {0x10de: "NVIDIA", 0x1002: "AMD", 0x8086: "Intel"}


def driver_version_text(vendor: int, version: int) -> str:
    """The driver version as the vendor writes it (VkPhysicalDeviceProperties.driverVersion)."""
    if vendor == 0x10de:  # NVIDIA: 10.8.8.6 bits
        return f"{version >> 22}.{(version >> 14) & 0xff:02d}"
    if vendor == 0x8086 and os.name == "nt":  # Intel on Windows: 18.14
        return f"{version >> 14}.{version & 0x3fff}"
    return f"{version >> 22}.{(version >> 12) & 0x3ff}.{version & 0xfff}"  # Vulkan's own packing


def gpu_label_from_vkp(name: str) -> str:
    """'vk_pipe_cache_10de_2684_04d4a800.vkp' -> 'NVIDIA_10de-2684_driver_617.42' ('' if not one)."""
    m = re.fullmatch(r"vk_pipe_cache_([0-9a-f]{4})_([0-9a-f]{4})_([0-9a-f]{8})\.vkp", name)
    if not m:
        return ""
    vendor, device, version = (int(g, 16) for g in m.groups())
    return (f"{VENDOR_NAMES.get(vendor, f'vendor{vendor:04x}')}_{vendor:04x}-{device:04x}"
            f"_driver_{driver_version_text(vendor, version)}")


def export_shader_cache_bundle(zip_path: Path, root: Path = None, readme: str = "", label: str = "") -> dict:
    """A cache to share with other PCs: the GPU-independent part (.key, .src) at `<title>/`, this
    GPU's part (.meta, .spv, profile.bin and the driver's .vkp) at `gpu/<label>/<title>/`, and a
    README.txt. import_shader_cache (and the user/cache_import drop folder) reads it. `label`
    defaults to the vendor / device / driver of the .vkp. Returns {"portable", "gpu", "label"}."""
    import zipfile
    root = root or shader_cache_root()
    titles = sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []
    if not label:
        for title in titles:
            label = next((gpu_label_from_vkp(f.name) for f in sorted(title.glob("*" + CACHE_DRIVER))
                          if gpu_label_from_vkp(f.name)), "")
            if label:
                break
    label = label or "this_gpu"
    out = {"portable": 0, "gpu": 0, "label": label}
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        if readme:
            zf.writestr("README.txt", readme)
        for title in titles:
            for f in sorted(title.iterdir()):
                if not f.is_file():
                    continue
                if f.suffix in CACHE_GPU_INDEPENDENT:
                    zf.write(f, f"{title.name}/{f.name}")
                    out["portable"] += 1
                elif f.suffix in CACHE_GPU_SPECIFIC | {CACHE_DRIVER} or f.name == CACHE_PROFILE:
                    zf.write(f, f"gpu/{label}/{title.name}/{f.name}")
                    out["gpu"] += 1
    return out


def cache_import_dir(root: Path = None) -> Path:
    """The drop folder for shared caches: user/cache_import (next to user/cache)."""
    return (root or shader_cache_root()).parent / CACHE_IMPORT_DIR


def import_dropped_caches(root: Path = None, drop: Path = None) -> list:
    """Merges every .zip in the drop folder (cache_import_dir) into the cache and renames it to
    .zip.imported (.zip.bad when it is not a readable cache archive), so each is merged once.
    Returns [(file name, stats or None)]."""
    import zipfile
    root = root or shader_cache_root()
    drop = drop or cache_import_dir(root)
    done = []
    if not drop.is_dir():
        return done
    for z in sorted(drop.glob("*.zip")):
        try:
            stats = import_shader_cache(z, root)
            suffix = ".imported"
        except (OSError, zipfile.BadZipFile):
            stats, suffix = None, ".bad"
        target = z.with_name(z.name + suffix)
        n = 1
        while target.exists():
            target = z.with_name(f"{z.name}{suffix}.{n}")
            n += 1
        try:
            z.rename(target)
        except OSError:
            pass
        done.append((z.name, stats))
    return done


# World warper (gpu/shim/party/world_warper.h): a solo run that warps through every lamp and area
# and plays a little in each, filling the shader cache; at the end the launcher exports the cache.
# It runs on a COPY of the save folder (user\world_warper\savedata), never on the player's saves;
# the shader cache it fills is the player's own (BB_GPU_USER_DIR = the real user folder).
WARPER_DIR = "world_warper"
WARPER_STATE = "world_warper_state.txt"
WARPER_MAX_RESTARTS = 6  # crashes in a row the launcher resumes from (the warper skips a row after 2)


def user_root() -> Path:
    """The user folder run.bat uses (saves, shader cache): BB_USER_DIR, else BB_DATA_DIR\\user."""
    return shader_cache_root().parent


def warper_paths(user: Path = None) -> dict:
    user = user or user_root()
    base = user / WARPER_DIR
    return {"user": user, "base": base, "saves": base / "savedata", "state": base / WARPER_STATE}


def read_warper_progress(state: Path) -> dict:
    """The progress file as the warper writes it: areas done / skipped, areas the game stopped in
    (started without a done) and whether the run finished."""
    out = {"done": 0, "skipped": 0, "started": 0, "finished": False, "new_pipelines": 0}
    try:
        text = state.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return out
    done, skipped, started = set(), set(), set()
    for line in text.splitlines():
        parts = line.split()
        if parts == ["finished"]:
            out["finished"] = True
        elif len(parts) >= 2 and parts[1].isdigit():
            if parts[0] == "done":
                done.add(parts[1])
                if len(parts) >= 3 and parts[2].isdigit():
                    out["new_pipelines"] += int(parts[2])
            elif parts[0] == "skip":
                skipped.add(parts[1])
            elif parts[0] == "start":
                started.add(parts[1])
    out["done"], out["skipped"] = len(done), len(skipped)
    out["started"] = len(started - done - skipped)
    return out


def prepare_warper_saves(paths: dict, fresh: bool) -> int:
    """Copies the player's save folder (user\\savedata) to the warper's own folder; `fresh` starts
    over (the old copy and the progress removed). Returns the number of files copied (0: the
    existing copy is kept: a resume). Raises FileNotFoundError when there is no save to copy.
    Never writes to the player's own savedata."""
    src = paths["user"] / "savedata"
    if not src.is_dir() or not any(p.is_file() for p in src.rglob("*")):
        raise FileNotFoundError(f"no save in {src}")
    base = paths["base"]
    if fresh and base.exists():
        shutil.rmtree(base)
    if paths["saves"].is_dir() and any(p.is_file() for p in paths["saves"].rglob("*")):
        return 0
    base.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, paths["saves"], dirs_exist_ok=True)
    addcont = paths["user"] / "addcont"  # the add-on (The Old Hunters) entitlement, when there is one
    if addcont.is_dir() and not (base / "addcont").exists():
        shutil.copytree(addcont, base / "addcont")
    return sum(1 for p in paths["saves"].rglob("*") if p.is_file())


def warper_env(env: dict, paths: dict, mode: str = "tour") -> dict:
    """The game's environment for a warper run: solo (every party variable dropped), the saves from
    the copy (BB_USER_DIR), the shader cache in the player's own user folder (BB_GPU_USER_DIR), the
    progress file, no time limit."""
    for key in [k for k in env if k.startswith("BB_PARTY")]:
        env.pop(key)
    env["BB_USER_DIR"] = str(paths["base"])
    env["BB_GPU_USER_DIR"] = str(paths["user"])
    env["BB_WORLD_WARPER"] = mode
    env["BB_WORLD_WARPER_STATE"] = str(paths["state"])
    env["BB_TIMEOUT"] = "0"
    env["BB_LAMP_TRAVEL"] = "1"
    return env


LOG_HISTORY = 20  # playtest 1: a guest's restarts pushed the session's logs out


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


def new_crash_logs(since) -> bool:
    """A crash log (out\\crash_*.log, or a GPU device-lost report) was written after `since` (a timestamp; None = never)."""
    if since is None:
        return False
    try:
        out = APP_DIR / "out"
        # A GPU device lost writes its own report (vk_breadcrumbs.cpp) before the crash.
        return any(p.stat().st_mtime >= since - 1
                   for pattern in ("crash_*.log", "device_lost_*.log") for p in out.glob(pattern))
    except OSError:
        return False


def descendant_pids(root_pid: int) -> list:
    """Every process started (directly or not) by root_pid, root included (Windows; [root] elsewhere)."""
    if os.name != "nt":
        return [root_pid]
    import ctypes
    from ctypes import wintypes

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if not snap or snap == wintypes.HANDLE(-1).value:
        return [root_pid]
    parent = {}
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            parent[e.th32ProcessID] = e.th32ParentProcessID
            ok = k32.Process32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    out, todo = [root_pid], [root_pid]
    while todo:
        p = todo.pop()
        for child, par in parent.items():
            if par == p and child not in out and child != p:
                out.append(child)
                todo.append(child)
    return out


def close_windows_of(pids: list) -> int:
    """Posts WM_CLOSE to the visible top-level windows of these processes; how many got it."""
    if os.name != "nt" or not pids:
        return 0
    import ctypes
    from ctypes import wintypes
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    want = set(pids)
    hits = []
    proc_t = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _):
        pid = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in want and u32.IsWindowVisible(hwnd):
            hits.append(hwnd)
        return True

    u32.EnumWindows(proc_t(cb), 0)
    for hwnd in hits:
        u32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE: the game shuts down normally (party BYE, UPnP unmap)
    return len(hits)


def stop_tree(proc: subprocess.Popen, timeout: float = 8.0, log=None) -> str:
    """Stops run.bat and the game: WM_CLOSE to the game's window(s) first, so it exits normally
    (party BYE, UPnP mappings removed, the GPU released), then kill_tree after `timeout` seconds
    or when there is no window. Returns "closed", "killed" or "exited"."""
    say = log or (lambda m: None)
    if proc.poll() is not None:
        return "exited"
    sent = close_windows_of(descendant_pids(proc.pid))
    if sent:
        say(f"Stopping: asked the game to close ({sent} window(s)); waiting up to {timeout:.0f} s...")
        try:
            proc.wait(timeout=timeout)
            say("Stopping: the game closed normally.")
            return "closed"
        except subprocess.TimeoutExpired:
            say(f"Stopping: the game did not close in {timeout:.0f} s; force-killing the process tree.")
    else:
        say("Stopping: no game window found; force-killing the process tree.")
    kill_tree(proc)
    return "killed"
def running_instances():
    """bbport.exe processes of this install that are alive (empty when the guard is unavailable)."""
    if bb_guard is None:
        return []
    try:
        return bb_guard.find_instances(APP_DIR / "out")
    except Exception:
        return []


def kill_tree(proc: subprocess.Popen):
    """Kill run.bat and everything it started (bbport.exe included)."""
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=NO_WINDOW,
        )
        # The tree walk misses a bbport.exe orphaned by an in-game restart or a hung crash handler.
        for instance in running_instances():
            bb_guard.terminate(instance.pid)
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
def acquire_single_instance():
    """True for the first launcher of this install; a second window could start a second game."""
    if os.name != "nt":
        return True
    try:
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        name = "Local\\BloodborneLauncher_" + hashlib.md5(str(APP_DIR).lower().encode()).hexdigest()[:12]
        handle = kernel32.CreateMutexW(None, False, name)
        if handle and ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS
            return False
        _instance_mutex.append(handle)  # held until the process exits
    except Exception:
        pass
    return True


class BloodborneLauncher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"Bloodborne PC Launcher {PORT_VERSION}")
        self.geometry("780x760")  # the Party tab is the tallest settings tab
        self.minsize(720, 680)

        self.proc = None
        self.log_fh = None
        self.hidden_count = 0
        self.log_queue = queue.Queue()
        self.settings = self.load_settings()
        try:  # the game settings of an older launcher_settings.json move to bbport.ini once
            migrate_ini_rev(bbport_ini_path())  # bbport.ini's one-time changes (rot off, 0.7.2)
            moved = migrate_settings_to_ini(self.settings, bbport_ini_path())
            if moved is not None:
                SETTINGS_FILE.write_text(json.dumps(self.settings, indent=2), encoding="utf-8")
        except OSError:
            pass
        self.all_xml_patches = load_xml_patches()
        # Effect patches (Skip Intro, Disable Motion Blur, model LOD ...) are game settings: bbport.ini.
        self.enabled_patches = ({n for n in self.settings.get("enabled_patches", ["Skip Intro"])
                                 if n not in EFFECT_PATCHES and n not in MODEL_LOD_PATCHES}
                                | effect_patches_from_ini(read_ini(bbport_ini_path())))

        self.feat_skip_intro = tk.BooleanVar(value="Skip Intro" in self.enabled_patches)
        self.feat_perf_patch = tk.BooleanVar(value="Performance Patch (perf increase)" in self.enabled_patches)
        self.feat_no_blur = tk.BooleanVar(value="Disable Motion Blur (perf increase)" in self.enabled_patches)

        self.build_style()
        self.build_ui()
        # Tall enough for the tallest settings tab when the screen allows (the tabs outgrew a fixed
        # 760); on a smaller screen the tabs are clipped, never the Launch row (build_ui).
        self.update_idletasks()
        req_h = max(self.notebook.nametowidget(t).winfo_reqheight() for t in self.notebook.tabs())
        need = self.winfo_reqheight() - self.notebook.winfo_reqheight() + req_h + 40
        self.geometry(f"780x{max(760, min(need, self.winfo_screenheight() - 80))}")

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self._ini_stamp = self.ini_stamp()
        self.bind("<FocusIn>", lambda e: self.refresh_game_settings() if e.widget is self else None, add="+")
        self.after(50, self.poll_queue)
        self.log("Ready. Select your eboot.bin and press Launch Bloodborne.")
        self.after(1200, self.check_last_party_session)
        self.start_update_check()

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
        # Update notice (scripts/update_check.py): packed here once a newer release is known.
        self.update_label = tk.Label(main, text="", fg="#c5a059", bg="#1a1a1a", cursor="hand2",
                                     font=("Segoe UI", 10, "underline"), anchor="w")
        self._update_anchor = main.winfo_children()[-2]

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
        eboot_tip = ("The game's eboot.bin, or the folder that contains it: Bloodborne (any retail region) with "
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
        self.volumetric_var = tk.StringVar(value=volumetric_label(read_ini(bbport_ini_path())))
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

        # Game settings from bbport.ini (refresh_game_settings re-reads them)
        _ini = read_ini(bbport_ini_path())
        _up_on, _up_label = upscaler_state(_ini, str(self.settings.get("upscaler_choice", "")))
        self.feat_upscaler = tk.BooleanVar(value=_up_on)
        self.upscaler_var = tk.StringVar(value=_up_label)
        self.ini_flag_vars = {key: tk.BooleanVar(value=ini_flag_value(_ini, key, default))
                              for _, key, default, _ in INI_FLAG_SETTINGS}
        self.feat_object_motion = self.ini_flag_vars["object_motion"]
        self.feat_overlay = tk.BooleanVar(value=self.settings.get("feat_overlay", True))
        self.feat_fps_patch = tk.BooleanVar(value=self.settings.get("feat_fps_patch", True))
        self.feat_frame_hooks = tk.BooleanVar(value=self.settings.get("feat_frame_hooks", True))
        self.feat_mods = tk.BooleanVar(value=self.settings.get("mods", True))
        self.feat_dream_mirror = tk.BooleanVar(value=self.settings.get("dream_mirror", True))
        self.feat_lamp_travel = tk.BooleanVar(value=self.settings.get("lamp_travel", True))
        self.feat_res_scaling = tk.BooleanVar(value=self.settings.get("feat_res_scaling", True))
        # Off by default since 0.6.1 (playtest 1: a guest on the old default "on" got 20 MB logs and
        # false hang dumps); settings saved before that ("tracing_rev" missing) are switched off once.
        self.feat_tracing = tk.BooleanVar(value=self.settings.get("tracing_rev", 0) >= 1
                                          and self.settings.get("feat_tracing", False))
        self.feat_watchdog = tk.BooleanVar(value=self.settings.get("feat_watchdog", True))
        self.feat_draw_prep = tk.BooleanVar(value=self.settings.get("feat_draw_prep", True))
        self.feat_fullscreen = tk.BooleanVar(  # "fullscreen": key of the upstream launcher toggle (PR #39)
            value=self.settings.get("feat_fullscreen", self.settings.get("fullscreen", False)))
        _wm = str(self.settings.get("window_mode") or "").capitalize()
        self.window_mode_var = tk.StringVar(
            value=_wm if _wm in WINDOW_MODE_CHOICES else ("Fullscreen" if self.feat_fullscreen.get() else "Windowed"))
        self.feat_overlay_pad = tk.BooleanVar(value=self.settings.get("feat_overlay_pad", False))
        self.feat_game_menu = tk.BooleanVar(value=self.settings.get("feat_game_menu", True))
        self.feat_skip_network_choice = tk.BooleanVar(value=self.settings.get("feat_skip_network_choice", True))
        self.feat_save_log = tk.BooleanVar(value=self.settings.get("feat_save_log", False))
        self.feat_audio_log = tk.BooleanVar(value=self.settings.get("feat_audio_log", False))
        self.feat_arrival_log = tk.BooleanVar(value=self.settings.get("feat_arrival_log", False))
        self.feat_spectate_log = tk.BooleanVar(value=self.settings.get("feat_spectate_log", False))
        self.feat_hostdead_log = tk.BooleanVar(value=self.settings.get("feat_hostdead_log", False))
        self.feat_boot_watchdog = tk.BooleanVar(value=self.settings.get("feat_boot_watchdog", False))
        _lvl = str(self.settings.get("party_log_level", "") or "")
        self.party_log_level_var = tk.StringVar(
            value=next((label for label, v in PARTY_LOG_LEVELS if v == _lvl), PARTY_LOG_LEVELS[0][0]))
        _areas = self.settings.get("party_log_areas")
        self.party_log_area_vars = {key: tk.BooleanVar(value=not isinstance(_areas, list) or key in _areas)
                                    for key, _ in PARTY_LOG_AREAS}
        self.auto_support_bundle = tk.BooleanVar(value=self.settings.get("auto_support_bundle", True))
        self.check_updates = tk.BooleanVar(value=self.settings.get("check_updates", True))
        self.feat_mute_unfocused = tk.BooleanVar(value=self.settings.get("feat_mute_unfocused", True))
        self.feat_async_shaders = self.ini_flag_vars["async_pipelines"]
        self.feat_gpl = self.ini_flag_vars["gpl"]
        self.feat_compile_indicator = self.ini_flag_vars["compile_indicator"]
        self.feat_shader_precompile = tk.BooleanVar(value=self.settings.get("feat_shader_precompile", True))
        self.feat_hud = self.ini_flag_vars["show_hud"]
        self.feat_mouse_keyboard = self.ini_flag_vars["mk_enabled"]

        # Party co-op (BB_PARTY_*)
        party_mode = str(self.settings.get("party_mode", "") or "")
        self.party_mode_var = tk.StringVar(value=next(
            (label for label, value in PARTY_MODES if value == party_mode), PARTY_MODES[0][0]))
        self.party_name_var = tk.StringVar(value=str(self.settings.get("party_name") or party_default_name()))
        party_max = str(self.settings.get("party_max", "3"))
        self.party_max_var = tk.StringVar(value=party_max if party_max in PARTY_MAX_CHOICES else "3")
        self.party_auto = tk.BooleanVar(value=self.settings.get("party_auto", True))
        self.party_seamless = tk.BooleanVar(value=self.settings.get("party_seamless", True))
        self.party_direct_summon = tk.BooleanVar(value=self.settings.get("party_direct_summon", True))
        # Party HUD: a game setting (bbport.ini party_hud, also in the game's menus), not a launch
        # option: read from and written to bbport.ini directly.
        self.frame_generation_var = tk.StringVar(value=frame_generation_label(read_ini(bbport_ini_path())))
        self.reflex_var = tk.StringVar(value=reflex_label(read_ini(bbport_ini_path())))
        self.prompt_style_var = tk.StringVar(value=prompt_style_label(read_ini(bbport_ini_path())))
        # HDR output: game settings too (bbport.ini hdr / hdr_peak_nits / hdr_paper_white).
        _ini = read_ini(bbport_ini_path())
        self.hdr_var = tk.BooleanVar(value=str(_ini.get("hdr", "0")).strip() not in ("", "0"))
        self.hdr_peak_var = tk.StringVar(value=hdr_peak_label(_ini))
        self.hdr_paper_var = tk.StringVar(value=hdr_paper_white_label(_ini))
        self.party_hud = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_hud"))
        self.party_name_tags = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_name_tags"))
        # Companions without the phantom glow: a game setting (bbport.ini party_glow_off, default on).
        self.party_glow_off = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_glow_off"))
        # Shared NPC dialogue: a game setting (bbport.ini party_npc_talk, also in the game's menus).
        self.party_npc_talk = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_npc_talk"))
        # Pings (I2): game settings (bbport.ini party_pings on, party_through_walls off by default).
        self.party_pings = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_pings"))
        self.party_through_walls = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_through_walls", False))
        # The marker on the own lost-echoes bloodstain (bbport.ini party_stain_marker, off by default).
        self.party_stain_marker = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_stain_marker", False))
        # I1 friendly fire / PvP: a host rule (bbport.ini party_friendly_fire, off by default).
        self.party_friendly_fire = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_friendly_fire", False))
        # Shared lamps (statsaudit finding 5): a host rule (bbport.ini party_share_lamps, on by default).
        self.party_share_lamps = tk.BooleanVar(value=ini_flag(bbport_ini_path(), "party_share_lamps", True))
        # 0.7.2: a joining guest's own lamps in the host's world, a host rule (party_guests_bring_lamps, off).
        self.party_guests_bring_lamps = tk.BooleanVar(
            value=ini_flag(bbport_ini_path(), "party_guests_bring_lamps", False))
        self.party_separate_save = tk.BooleanVar(value=self.settings.get("party_separate_save", True))
        self.party_port_var = tk.StringVar(value=str(self.settings.get("party_port", PARTY_DEFAULT_PORT)))
        self.party_upnp = tk.BooleanVar(value=self.settings.get("party_upnp", True))
        self.party_rendezvous = tk.BooleanVar(value=bool(self.settings.get("party_rendezvous", False)))
        self.party_member_relay = tk.BooleanVar(value=bool(self.settings.get("party_member_relay", True)))
        self.party_ipv6 = tk.BooleanVar(value=bool(self.settings.get("party_ipv6", False)))
        self.party_public_var = tk.StringVar(value=str(self.settings.get("party_public_addr", "") or ""))
        self.party_stun_var = tk.StringVar(value=str(self.settings.get("party_stun", PARTY_DEFAULT_STUN) or ""))
        self.party_password_var = tk.StringVar(value=str(self.settings.get("party_password", "") or ""))
        self.party_code_var = tk.StringVar(value=str(self.settings.get("party_code", "") or ""))
        _internet, _lan = read_party_codes()
        self.party_host_code_var = tk.StringVar(value=_internet)
        self.party_lan_code_var = tk.StringVar(value=_lan)
        self.party_status_var = tk.StringVar()
        self.party_test_running = False
        # Party rules & sync dialog (PARTY_SWITCHES, start mode, guest Insight, backups)
        self.party_switch_vars = {key: tk.BooleanVar(value=bool(self.settings.get(key, default)))
                                  for key, _, default in PARTY_SWITCHES}
        # The party gameplay switches and the guest Insight: game settings (bbport.ini), also on the
        # game's System pages; read again with the other game settings (refresh_game_settings).
        self.party_switch_vars.update({key: tk.BooleanVar(value=ini_flag_value(_ini, key, default))
                                       for key, _, default in PARTY_INI_SWITCHES})

        def combo_label(choices, key):
            value = str(self.settings.get(key, "") or "")
            return next((label for label, v in choices if v == value), choices[0][0])
        self.party_start_var = tk.StringVar(value=combo_label(PARTY_START_CHOICES, "party_start"))
        self.party_insight_var = tk.StringVar(value=party_insight_label(_ini))
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
        Tooltip(everything_btn, "Preset: turns the features back on (including async shaders and GPL; not Debug tracing), sets "
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
        self._combo(sec, "Window mode:", self.window_mode_var, WINDOW_MODE_CHOICES,
                    "Windowed (default): a normal window with title bar. Borderless: a frameless window "
                    "covering the monitor (quick Alt+Tab). Fullscreen: exclusive fullscreen at the desktop's "
                    "display mode (the driver may present with less latency). Alt+Enter toggles "
                    "fullscreen in game (BB_WINDOW_MODE).", width=12)
        self._combo(sec, "Monitor:", self.display_var, [c[0] for c in self.display_choices],
                    "Which monitor the game opens on. The list comes from bb-gpu-capabilities; "
                    "Primary (default) uses the main monitor (BB_DISPLAY).", width=24)

        sec = self._section(tab, 1, "Frame rate")
        self._combo(sec, "Frame rate target:", self.fps_var, FPS_CHOICES,
                    "Target frame rate: uncap, 60, 90, or 30 (the game's native rate). Above 30 needs the "
                    "FPS patch. Default: uncap (BB_FPS).", width=10)
        self._check(sec, "FPS patch (Uncap / 60 / 90)", self.feat_fps_patch,
                    "Lets the game run above 30 FPS at the chosen target, with physics and movement "
                    "speeds kept correct. Off = the game stays at 30 FPS. Default: on (BB_FPS_PATCH).")
        self._check(sec, "Frame rate by game hooks", self.feat_frame_hooks,
                    "On: the frame rate is freed by hooks in the game's code (deadinside's 0.6-pre1): every "
                    "frame-tied timer, damping and speed follows the real frame time (no sprint dropping to "
                    "a walk above 90 FPS, physics timers at the right speed). Off: the community FPS++ "
                    "patches as before; with the hooks on, a patch that changes the same code or constants "
                    "(e.g. an FPS patch from the patches folder) is left out, with a line in the log. "
                    "Default: on (BB_FRAME_HOOKS).")
        self._combo(sec, "Frames ahead:", self.frames_ahead_var, [c[0] for c in FRAMES_AHEAD_CHOICES],
                    "How many frames the GPU thread may queue ahead of the display. 1 = least input lag, "
                    "2 = smoother frame times (default), 3 = smoothest but more lag (BB_FRAMES_AHEAD).",
                    width=20)
        self._check(sec, "Performance HUD (F11)", self.feat_hud,
                    "In-game overlay with FPS, frame time and a frame-time graph, VRAM, GPU, CPU and RAM "
                    "load. F11 shows or hides it at any time; its corner, size and metrics are in the "
                    "in-game menu (Insert > Display). A game setting (bbport.ini show_hud): F11 and the "
                    "in-game menu change the same switch. Default: off.",
                    command=lambda: self.save_game_settings("show_hud"))

        # 2. Graphics
        tab = self._tab("Graphics")
        sec = self._section(tab, 0, "Image quality")
        upscaler_tip = ("Temporal upscaling and anti-aliasing; off = none. FSR 3.1 runs on every GPU, TAA "
                        "is the lightest. FSR 4 and FSR 4.1.1 need a GPU that runs their shaders, DLSS an "
                        "NVIDIA RTX GPU and NVIDIA's library (below). The launcher cannot check the GPU, "
                        "so every choice is listed: at start the game checks and uses FSR 3.1 instead of "
                        "one this GPU lacks (its menu then greys that choice out). A game setting "
                        "(bbport.ini upscaler; in game: System > Display and the Insert overlay), used "
                        "at the next start. Default: on, FSR 3.1.")
        up_row = ttk.Frame(sec, style="Card.TFrame")
        up_row.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=2)
        sec.next_row += 1
        up_cb = ttk.Checkbutton(up_row, text="Enable upscaler", variable=self.feat_upscaler,
                                style="Card.TCheckbutton", command=self.save_upscaler)
        up_cb.pack(side="left", padx=(0, 8))
        self.upscaler_combo = ttk.Combobox(up_row, textvariable=self.upscaler_var, state="readonly",
                                           values=[c[0] for c in UPSCALER_CHOICES], width=10)
        self.upscaler_combo.pack(side="left")
        self.upscaler_combo.bind("<<ComboboxSelected>>", lambda e: self.save_upscaler())
        for widget in (up_cb, self.upscaler_combo):
            Tooltip(widget, upscaler_tip)
        self.update_upscaler_state()
        self._check(sec, "Object motion vectors", self.feat_object_motion,
                    "Exact motion vectors for animated characters and objects, so the upscaler smears less "
                    "on moving things. Small GPU cost. A game setting (bbport.ini object_motion; in game: "
                    "System > Upscaler > Character motion), used at the next start.",
                    command=lambda: self.save_game_settings("object_motion"))
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
        volumetric_combo = self._combo(
            sec, "Motion blur resolution:", self.volumetric_var, [c[0] for c in VOLUMETRIC_CHOICES],
            "Resolution of the game's motion blur buffers (half resolution in the game): 2x pixels / "
            "Full resolution give the blur of moving characters finer edges, some GPU cost. Smoke and "
            "mist are full resolution already. A game setting (bbport.ini volumetric_res; in game: "
            "System > Display).", width=18)
        volumetric_combo.bind("<<ComboboxSelected>>", lambda e: self.save_game_settings("volumetric_res"))
        # Upstream 0.51: NVIDIA's DLSS library chosen by the player (vk_dlss.cpp looks in
        # <user>/dlss first, then next to bbport.exe; the bridge bbport_dlss.dll stays next to it).
        dlss_row = ttk.Frame(sec, style="Card.TFrame")
        dlss_row.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=3)
        sec.next_row += 1
        dlss_tip = ("DLSS (NVIDIA RTX) needs NVIDIA's nvngx_dlss.dll. A newer one (from a game or "
                    "NVIDIA's DLSS SDK) can be chosen here: it is copied to user\\dlss and used instead "
                    "of the one next to bbport.exe. Remove goes back to that one.")
        # Label and the buttons, the short status on its own line under the buttons: the buttons
        # never move whatever the status says (a long status once pushed them off the card at the
        # minimum width); the long explanation is in the tooltip.
        lbl = ttk.Label(dlss_row, text="DLSS library:", style="Card.TLabel")
        lbl.grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.dlss_choose_btn = ttk.Button(dlss_row, text="Choose...", style="Secondary.TButton",
                                          command=self.choose_dlss_dll)
        self.dlss_choose_btn.grid(row=0, column=1, sticky="w", padx=(0, 4))
        self.dlss_remove_btn = ttk.Button(dlss_row, text="Remove", style="Secondary.TButton",
                                          command=self.remove_dlss_dll)
        self.dlss_remove_btn.grid(row=0, column=2, sticky="w")
        self.dlss_var = tk.StringVar()
        self.dlss_status_lbl = ttk.Label(dlss_row, textvariable=self.dlss_var, style="Hint.TLabel",
                                         wraplength=220, justify="left")
        self.dlss_status_lbl.grid(row=1, column=1, columnspan=3, sticky="w", pady=(2, 0))
        dlss_row.columnconfigure(3, weight=1)  # extra width of the status goes here, not to the buttons
        self.dlss_base_tip = dlss_tip
        self.dlss_tips = [Tooltip(widget, dlss_tip)
                          for widget in (lbl, self.dlss_choose_btn, self.dlss_remove_btn, self.dlss_status_lbl)]
        self.update_dlss_status()
        fg_tip = ("Generated frames between the rendered ones: smoother motion on a high refresh "
                  "display, slightly more input lag. DLSS FG needs an RTX 40 or 50 series GPU and "
                  "NVIDIA's nvngx_dlssg.dll (bundled; or chosen above); FSR FG (2x) runs on most GPUs. "
                  "The HUD is taken from the rendered frame. Without support the game presents "
                  "normally and says why in its menu and log. A game setting (bbport.ini "
                  "frame_generation / fg_backend, also in the in-game menu), used at the next start. "
                  "Default: Off.")
        fg_combo = self._combo(sec, "Frame generation:", self.frame_generation_var,
                               [c[0] for c in FRAME_GENERATION_CHOICES], fg_tip, width=14)
        fg_combo.bind("<<ComboboxSelected>>", lambda e: self.save_frame_generation())
        reflex_tip = ("NVIDIA's low-latency mode (VK_NV_low_latency2): the driver paces the game so "
                      "frames queue less; Boost also keeps the GPU clocks up. NVIDIA GPUs with a "
                      "current driver; elsewhere it stays off. A game setting (bbport.ini reflex, also "
                      "in the in-game menu), used at the next start. Default: Off.")
        reflex_combo = self._combo(sec, "NVIDIA Reflex:", self.reflex_var, [c[0] for c in REFLEX_CHOICES],
                                   reflex_tip, width=14)
        reflex_combo.bind("<<ComboboxSelected>>", lambda e: self.save_frame_generation())
        self._check(sec, "HDR output (HDR10)", self.hdr_var,
                    "An HDR10 picture (PQ, Rec.2020) for an HDR display: the game's SDR picture keeps its "
                    "mid-tones at paper white and only its highlights are expanded towards the peak. Needs "
                    "HDR on in Windows (Settings > Display > Use HDR); without it the game stays SDR. "
                    "Works with every upscaler and frame generation. Peak, paper white, highlights, "
                    "saturation and a calibration pattern are in the game's System > HDR. A game setting "
                    "(bbport.ini hdr, BB_HDR), used at the next start. Default: off.",
                    command=self.save_hdr)
        hdr_peak = self._combo(sec, "HDR peak brightness:", self.hdr_peak_var,
                               [c[0] for c in HDR_PEAK_CHOICES],
                               "The brightest the HDR picture gets. Auto: what the display reports to "
                               "Windows (1000 nits when it reports nothing). Fine-tune it with the "
                               "calibration pattern in System > HDR (bbport.ini hdr_peak_nits, "
                               "BB_HDR_PEAK).", width=18, indent=True)
        hdr_peak.bind("<<ComboboxSelected>>", lambda e: self.save_hdr())
        hdr_paper = self._combo(sec, "HDR paper white:", self.hdr_paper_var,
                                [c[0] for c in HDR_PAPER_WHITE_CHOICES],
                                "Brightness of the picture's mid-tones and of the port's menus and HUD "
                                "under HDR. 200 nits by default; lower in a dark room (bbport.ini "
                                "hdr_paper_white, BB_HDR_PAPER_WHITE).", width=18, indent=True)
        hdr_paper.bind("<<ComboboxSelected>>", lambda e: self.save_hdr())

        sec = self._section(tab, 1, "Streaming & effects")
        self._combo(sec, "VRAM pre-upload:", self.preupload_var, [c[0] for c in PREUPLOAD_CHOICES],
                    "Uploads game memory into VRAM in the background, so there are fewer stutters when new "
                    "areas stream in. Normal = default; Full = everything (~3 GB more VRAM, 12 GB+ GPU); "
                    "Off = upload on demand (BB_PREUPLOAD).", width=30)
        self._check(sec, "Disable Motion Blur", self.feat_no_blur,
                    "Game patch by Kyo: removes the motion blur effect (and its velocity-map pass), a small "
                    "performance gain. Same as the 'Disable Motion Blur (perf increase)' XML patch and "
                    "the in-game Effects page's Motion blur switch (bbport.ini effect_motion_blur).",
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
                    "A game setting (bbport.ini async_pipelines; in game: System > Shaders > Async "
                    "shaders, live). Default: on.",
                    command=lambda: self.save_game_settings("async_pipelines"))
        self._check(sec, "Graphics pipeline library (GPL)", self.feat_gpl,
                    "Compiles shader parts ahead of time (startup and loading screens) and links them quickly "
                    "at draw time. Needs driver support (NVIDIA/AMD; falls back automatically). Restart "
                    "required. A game setting (bbport.ini gpl; in game: System > Shaders > Pipeline "
                    "libraries). Default: off.",
                    command=lambda: self.save_game_settings("gpl"))
        self._check(sec, "Precompile shaders at startup", self.feat_shader_precompile,
                    "Before the intro logos, builds every pipeline in the shader cache (and rebuilds the "
                    "cache for a new GPU) with a progress screen, so areas already in the cache never "
                    "stutter. Slow only the first time; later launches are quick. Esc skips. "
                    "Default: on (BB_SHADER_PRECOMPILE).")
        self._check(sec, "Shader compile progress indicator", self.feat_compile_indicator,
                    "Shows 'Compiling shaders: NN%' in a corner while new shaders compile, only on loading "
                    "screens and the main menu, never during gameplay. A game setting (bbport.ini "
                    "compile_indicator, also in the in-game menu). Default: on.",
                    command=lambda: self.save_game_settings("compile_indicator"))
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
                    "'Skip Intro' XML patch and the in-game Effects page's Skip intros (bbport.ini "
                    "skip_intro). Default: on.",
                    command=self.on_quick_patch_toggle)
        self._check(sec, "Skip online/offline choice", self.feat_skip_network_choice,
                    "Skips the screen that asks whether to play online or offline at startup (game patch). "
                    "With party mode on (Party tab) the game always takes the online path instead. "
                    "Default: on (BB_SKIP_NETWORK_CHOICE).")

        sec = self._section(tab, 1, "Menus, mods & patches")
        self._check(sec, "Settings in game's System menu", self.feat_game_menu,
                    "Adds all of the port's settings as pages of the game's own System menu (Display, Effects, "
                    "Upscaler, Performance HUD, Mouse & keyboard, Game patches, Party ...). Off leaves the "
                    "game's menu untouched (the overlay, Insert, still has them). Default: on (BB_GAME_MENU).")
        self._check(sec, "Enable mods folder (mods/)", self.feat_mods,
                    "Loads loose-file mods from the mods\\ folder (switches and load order in mods.json); the "
                    "original game files are not modified. Default: on (BB_MODS_ENABLED).")
        self._check(sec, "Hunter's Dream mirror: change appearance", self.feat_dream_mirror,
                    "The mirror in the Hunter's Dream workshop opens the appearance editor (the developers' "
                    "unfinished 'Put on Disguise', restored as in bbhost). Each party member edits their own "
                    "hunter; every player in a party needs the same setting (party mod check). "
                    "Default: on (BB_DREAM_MIRROR).")
        self._check(sec, "Direct lamp travel", self.feat_lamp_travel,
                    "A lamp offers travel to the Hunter's Dream or straight to any other lamp you have lit "
                    "(the headstones' list, in the game's own travel menu): one loading screen instead of two. "
                    "In a party it is a normal party travel. Off: lamps only return to the Hunter's Dream, as "
                    "in the original game. Default: on (BB_LAMP_TRAVEL).")
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
                    "switched in game (Insert > Controls). A game setting (bbport.ini mk_enabled). "
                    "Default: off.",
                    command=lambda: self.save_game_settings("mk_enabled"))
        prompt_combo = self._combo(
            sec, "Button prompts:", self.prompt_style_var, [c[0] for c in PROMPT_STYLE_CHOICES],
            "The icons in the game's button prompts and key guides. Auto (default): keys after "
            "keyboard or mouse input, the gamepad's own buttons after gamepad input (PlayStation pads "
            "keep the game's icons; Switch-style pads get Nintendo letters, others Xbox letters). "
            "Pick Nintendo for a pad with Nintendo-printed buttons that reports as an Xbox pad (e.g. "
            "an 8BitDo in X-input mode): only the letters change, the bottom button still confirms "
            "and then shows B. Also in game (Insert > Controls); bbport.ini prompt_style, "
            "BB_PROMPT_STYLE.", width=14)
        prompt_combo.bind("<<ComboboxSelected>>", lambda e: self.save_prompt_style())
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
                    "Diagnostic tracing for bug reports: a once-a-second heartbeat (frames and GPU submits per "
                    "second) and hang dumps with thread states and main-thread samples in out\\. Cheap: no "
                    "measurable fps cost. Default: off (BB_TRACE). The slow full import log is separate: "
                    "BB_TRACE_IMPORTS=1.")
        self._check(sec, "Save frame stats to logs\\", self.feat_save_log,
                    "Writes frame and readback statistics to logs\\<time>.frames.csv / .readbacks.csv for "
                    "performance analysis. Default: off (BB_SAVE_LOG).")
        self._check(sec, "Audio log (crackling sound)", self.feat_audio_log,
                    "Logs late sound buffers, sound dropouts and a sound timing summary once a minute, "
                    "for crackling or popping reports. At most one line a second. Default: off (BB_AUDIO_LOG).")
        self._check(sec, "Party arrival render log (black world)", self.feat_arrival_log,
                    "After each party travel or join load: whether the 3D world drew, where the camera is "
                    "and the video memory state, 3 s and 10 s after arriving. Two lines per arrival. "
                    "Default: off (BB_ARRIVAL_LOG).")
        self._check(sec, "Party spectate camera log", self.feat_spectate_log,
                    "While a dead player watches a boss fight: camera resets per minute and the largest "
                    "camera jump and turn between frames, one line every 10 s. Default: off "
                    "(BB_PARTY_SPECTATE_LOG).")
        self._check(sec, "Party host-death lock log (guest frozen)", self.feat_hostdead_log,
                    "On a guest, from the host's death until the next load: four lines a second with the "
                    "game's host-death locks, the left stick and how far your character moved. For reports of "
                    "a guest that cannot move or open menus after the host died. Default: off (BB_HOSTDEAD_LOG).")
        self._check(sec, "Boot freeze dump (stuck before the title)", self.feat_boot_watchdog,
                    "Until the first world load: when the game's start stands still for 20 s (no frames, "
                    "or the title's party Continue stops), writes every game thread's state and stack to "
                    "out\boot_hang_<time>.log and the log. For reports of a game that stays on a gray or "
                    "black screen at start. Default: off (BB_BOOT_WATCHDOG).")

        sec = self._section(tab, 1, "Watchdog")
        self._check(sec, "Watchdog", self.feat_watchdog,
                    "Watches the game for freezes (no frames presented) and writes a hang dump with the "
                    "thread stacks when it hangs. Default: on (BB_WATCHDOG).")
        self._combo(sec, "Timeout (s):", self.timeout_var, TIMEOUT_CHOICES,
                    "Ends the game automatically after this many seconds (for testing). "
                    "0 = no limit, the default (BB_TIMEOUT).", width=12, indent=True)

        # Party logging (docs/party/logging.md): BB_PARTY_LOG / BB_PARTY_LOG_AREAS
        sec = self._section(tab, 0, "Party logging", row=1)
        self._combo(sec, "Level:", self.party_log_level_var, [label for label, _ in PARTY_LOG_LEVELS],
                    "Normal: the usual party log (no extra cost). Verbose: every party line gets a tag that "
                    "lines up the host's and the guests' logs (run id, slot, seconds since start, the shared "
                    "party clock, local time), a state snapshot every 10 s and events for loads, deaths, "
                    "dialogs, prompts, item boxes and every message between the games. Trace: verbose plus "
                    "message contents. Use the same level on every PC (BB_PARTY_LOG).", width=12)
        for key, label in PARTY_LOG_AREAS:
            self._check(sec, label, self.party_log_area_vars[key],
                        f"Verbose / Trace: log the '{key}' area (BB_PARTY_LOG_AREAS). All on = everything.")

        sec = self._section(tab, 1, "Support", row=1)
        self._check(sec, "Save a support bundle after a crash", self.auto_support_bundle,
                    "When the game ends with an error, the launcher writes logs\\support_<time>.zip "
                    "(the same as the button below). Default: on.")
        self._check(sec, "Tell me when a new version is out", self.check_updates,
                    "At start, asks GitHub (at most every 6 hours, 3 s timeout, nothing sent about you) "
                    "whether a newer bbport release exists and shows a line with its link. Every PC in a "
                    "party needs the same version. Default: on.", command=self.save_settings)
        btn = ttk.Button(sec, text="Save support bundle...", style="Secondary.TButton",
                         command=self.save_support_bundle)
        btn.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(6, 2))
        sec.next_row += 1
        Tooltip(btn, "One zip to send with a bug report: this launcher's logs (the last sessions), crash and "
                     "hang logs, summary.log, bbport.ini, the launcher settings and info.txt (version, build, "
                     "CPU, GPU). Passwords, party codes, tokens, IP addresses and your Windows user name are "
                     "removed; saves, game files and shader caches are never included.")
        ttk.Label(sec, text="In game: F9 (or Party tab > Mark an issue) writes\n"
                            "=== USER MARK n === into every player's log.",
                  style="Card.TLabel", justify="left").grid(row=sec.next_row, column=0, columnspan=2,
                                                            sticky="w", pady=(4, 0))
        sec.next_row += 1

        sec = self._section(tab, 0, "Shader cache builder", row=2)
        self.warper_btn = ttk.Button(sec, text="Build shader cache (world warper)...", style="Secondary.TButton",
                                     command=self.build_shader_cache_warper)
        self.warper_btn.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(2, 2))
        sec.next_row += 1
        Tooltip(self.warper_btn,
                "Starts the game in a test mode that travels through every lamp and area by itself (about "
                "70 areas, one loading screen each), looks round, walks, uses the weapons and items and "
                "fights a few enemies in each, so the shader cache holds what those areas need: later "
                "play stutters less. Takes about an hour; Stop cancels (pressing this again resumes). "
                "It plays on a COPY of your save folder (user\\world_warper): your own saves are never "
                "changed. At the end the shader cache is exported as one .zip - made from the game's "
                "shaders, so share it only with people who own the game.")
        ttk.Label(sec, text="Long run (about an hour) on a copy of your save;\n"
                            "exports the shader cache .zip at the end.",
                  style="Card.TLabel", justify="left").grid(row=sec.next_row, column=0, columnspan=2,
                                                            sticky="w", pady=(2, 0))
        sec.next_row += 1

        self.update_res_scaling_state()
        self.update_party_state()
        self.notebook.bind("<<NotebookTabChanged>>", lambda e: self.refresh_party_code(), add="+")

        # Launch, log and log buttons: packed before the notebook, so a window shorter than the
        # content clips the settings tabs and then shrinks the log, never the Launch button.
        bottom = ttk.Frame(main)
        bottom.pack(side="bottom", fill="both", expand=True, before=self.notebook)

        # launch / stop
        action = ttk.Frame(bottom)
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
        log_header = ttk.Frame(bottom)
        log_header.pack(fill="x")
        ttk.Label(log_header, text="Launcher Log:", font=("Segoe UI", 9, "bold")).pack(side="left")
        self.hide_vk_var = tk.BooleanVar(value=self.settings.get("hide_vk", True))
        hide_vk_cb = ttk.Checkbutton(log_header, text="Hide Vulkan warnings", variable=self.hide_vk_var)
        hide_vk_cb.pack(side="right")
        Tooltip(hide_vk_cb, "Hides the noisy Vulkan instance warnings in this panel; "
                            "launcher.log still gets the full output.")

        # bottom button bar
        bar = ttk.Frame(bottom)
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
        reset_btn = ttk.Button(bar, text="Reset shader cache", style="Secondary.TButton",
                               command=self.reset_shader_cache)
        reset_btn.pack(side="right")
        Tooltip(reset_btn, "Deletes the shader cache (only while the game is closed). Use it when an area "
                "is drawn wrong on this PC; the shaders are compiled again while you play.")

        # log text + scrollbar
        log_frame = ttk.Frame(bottom)
        log_frame.pack(fill="both", expand=True, pady=(2, 0))
        self.log_text = tk.Text(log_frame, height=3, bg="#111111", fg="#a0a0a0", insertbackground="white",
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

    def _section(self, tab, column: int, title: str, row: int = 0) -> ttk.Frame:
        sec = ttk.Frame(tab, style="Card.TFrame")
        sec.grid(row=row, column=column, sticky="nw", padx=(0, 16) if column == 0 else 0,
                 pady=(12 if row else 0, 0))
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
                    "enter the host's code. Off: the game starts single-player; a party game can then "
                    "be started or joined in the game itself (title screen: Party, after New Game, or "
                    "System > Party; it restarts into party mode on its own and remembers the host "
                    "address and password in user\\party.json). Party mode uses the game's online "
                    "title path and always the party saves. Default: Off (BB_PARTY=host|join, unset = off).")
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
        dsummon_cb = self._check(sec, "Direct summon", self.party_direct_summon,
                                 "The host hands its world straight to a ready companion's game, so the "
                                 "summon starts within seconds instead of waiting for bell signs (the bell "
                                 "signs stay as the fallback). Default: on (BB_PARTY_DIRECT_SUMMON).",
                                 command=self.save_settings)
        hud_cb = self._check(sec, "Party HUD", self.party_hud,
                             "Your companions' names and health in the bottom left corner of the game, "
                             "greyed while they are not in your world; on the same map their distance, a "
                             "direction arrow and a name tag above them. Never shown in single-player. Also "
                             "in the game: System > Party display > Party HUD. Default: on (bbport.ini "
                             "party_hud).",
                             command=self.save_party_hud)
        tags_cb = self._check(sec, "Party name tags", self.party_name_tags,
                              "On: in a party our name plate (name and a thin health bar) above each "
                              "companion replaces the game's own phantom plate, which hides behind any "
                              "tree or corner. Off: the game's own plate, as in the original game. Also in "
                              "the game: System > Party display > Party name tags. Default: on (bbport.ini "
                              "party_name_tags).",
                              command=self.save_party_name_tags)
        glow_cb = self._check(sec, "Companions without glow", self.party_glow_off,
                              "On: the other hunters of the party look like normal hunters on your screen, "
                              "without the game's blue, glowing phantom look. Off: that look, as in the "
                              "original game. Only how you see them: they stay cooperators. Also in the game: "
                              "System > Party look. Default: on (bbport.ini party_glow_off).",
                              command=self.save_party_glow_off)
        npc_talk_cb = self._check(sec, "Share NPC dialogue", self.party_npc_talk,
                                  "When a companion near you talks to an NPC you get the conversation too: "
                                  "the game's subtitles, the NPC's voice, their choices and gifts (passive, "
                                  "no menu opens). Range: in the game, System > Story & dialogue > NPC dialogue "
                                  "range (default 25 m). Default: on (bbport.ini party_npc_talk).",
                                  command=self.save_party_npc_talk)
        pings_cb = self._check(sec, "Pings", self.party_pings,
                               "Hold the touchpad (keyboard Tab) and press R3 (Q), or press V: a marker on the "
                               "spot you look at. Every party member sees it through walls with its distance "
                               "in metres, in your colour, for 10 seconds. Also in the game: System > Party display. "
                               "Default: on (bbport.ini party_pings).",
                               command=self.save_party_pings)
        walls_cb = self._check(sec, "Show party through walls", self.party_through_walls,
                               "A small diamond with the distance on every companion on your map, drawn over "
                               "walls and trees in the companion's colour. Also in the game: System > Party display. "
                               "Default: off (bbport.ini party_through_walls).",
                               command=self.save_party_through_walls)
        stain_cb = self._check(sec, "Bloodstain marker", self.party_stain_marker,
                               "A red drop with \"Blood Echoes\" and the distance on the bloodstain of your lost "
                               "echoes in the host's world, drawn over walls. The bloodstain is there either "
                               "way. Also in the game: System > Party display. Default: off (bbport.ini "
                               "party_stain_marker).",
                               command=self.save_party_stain_marker)
        ff_cb = self._check(sec, "Friendly fire / PvP (host)", self.party_friendly_fire,
                            "Host only: party members can hurt each other, both ways, like invaders (ERSC's "
                            "Judicator's Rulebook). A party kill is a normal party death; the killer gets no "
                            "Echoes or Insight. Your setting is the party's rule: guests see it and get a notice "
                            "when you switch it. Also in the game: System > World & PvP (live). Default: off "
                            "(bbport.ini party_friendly_fire).",
                            command=self.save_party_friendly_fire)
        lamps_cb = self._check(sec, "Shared lamps (host)", self.party_share_lamps,
                               "Host only: a lamp any party member lights in your world is lit for everyone, in "
                               "each member's own save too, and your lamps reach the guests' saves. Your setting "
                               "is the party's rule. Also in the game: System > World & PvP (Shared lamps). "
                               "Default: on (bbport.ini party_share_lamps).",
                               command=self.save_party_share_lamps)
        bring_cb = self._check(sec, "Guests bring their lamps (host)", self.party_guests_bring_lamps,
                               "Host only: a joining guest's lamps from its own world light up in yours too "
                               "(needs Shared lamps). Off: your world keeps its own lamps; any lamp a party "
                               "member lights in it is saved for everyone. Your setting is the party's rule. "
                               "Also in the game: System > World & PvP. Default: off (bbport.ini "
                               "party_guests_bring_lamps).",
                               command=self.save_party_guests_bring_lamps)
        self.party_separate_save.set(True)
        save_cb = self._check(sec, "Separate party saves (always)", self.party_separate_save,
                              "Party play always uses its own save folder (user\\savedata_party), so your "
                              "single-player save (user\\savedata) is never touched by a party session. "
                              "The first time, the launcher offers to copy your single-player save. The "
                              "game backs up the party save at every party start and every 10 minutes "
                              "(user\\save_backups, newest 10 kept, both changeable in Rules & sync; "
                              "README.txt there explains restoring). Party games can also be started in "
                              "the game: System > Party on the title screen (or Party after New Game).",
                              command=self.save_settings)
        save_cb.configure(state="disabled")
        copy_btn = ttk.Button(sec, text="Copy single-player save to party save", style="Secondary.TButton",
                              command=self.copy_solo_save_to_party)
        copy_btn.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(4, 2))
        sec.next_row += 1
        Tooltip(copy_btn, "Copies user\\savedata (your single-player characters) to user\\savedata_party, "
                          "the folder party sessions with separate saves play from. Your single-player "
                          "save is only read. An existing party save is backed up to "
                          "user\\save_backups\\savedata_party first.")
        undo_btn = ttk.Button(sec, text="Undo last party session", style="Secondary.TButton",
                              command=self.undo_last_party_session)
        undo_btn.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(2, 2))
        sec.next_row += 1
        Tooltip(undo_btn, "Puts your saves back as they were right before your last party session "
                          "(host or guest): the game copies user\\savedata and user\\savedata_party to "
                          "user\\save_backups\\party_sessions at every party start. You see what will be "
                          "replaced before anything changes, and your current saves are copied there "
                          "first, so the undo can be undone the same way.")
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
        rdv_cb = self._check(sec, "Find each other through the public DHT (shares your IP)", self.party_rendezvous,
                             PARTY_RENDEZVOUS_TIP, command=self.save_settings)
        relay_cb = self._check(sec, "Relay for party members that cannot reach the host", self.party_member_relay,
                               PARTY_MEMBER_RELAY_TIP, command=self.save_settings)
        ipv6_cb = self._check(sec, "Use IPv6 when both PCs have it", self.party_ipv6, PARTY_IPV6_TIP,
                              command=self.save_settings)
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
            "already contains a secret; a password also keeps out people who get hold of the code, and "
            f"makes the code 3 characters shorter (like {PARTY_CODE_EXAMPLE}). Pick one that "
            "is not easy to guess: it then carries the party's protection. Setting or removing it "
            "gives the host a new party code (once): send friends the new one. "
            "Stored in launcher_settings.json. Default: none (BB_PARTY_PASSWORD).", show="*")
        code_tip = (f"The code the host gave you, like {PARTY_CODE_EXAMPLE} (letters are not "
                    "case-sensitive; I, L and O count as 1, 1 and 0), or the host's address as host:port "
                    "(LAN, VPN, port forwarding). Older BBP2- / BBP1- codes work too. (BB_PARTY_CODE)")
        code_lbl, self.party_code_entry, paste_btn, _ = self._entry(
            sec, "Party code:", self.party_code_var, code_tip, width=20,
            button=("Paste", self.paste_party_code, "Pastes the party code from the clipboard."))
        host_tip = (f"Your party code for friends elsewhere, like {PARTY_CODE_EXAMPLE} (3 characters "
                    "longer without a password); they paste it with Join a party. It appears after the "
                    "game starts hosting (user\\party_code.txt). Players on your own network can use it "
                    "too: the game then finds you on the LAN.")
        host_lbl, self.party_host_code_entry, copy_btn, host_holder = self._entry(
            sec, "Internet code\n(friends elsewhere):", self.party_host_code_var, host_tip, width=20,
            button=("Copy", self.copy_party_code, "Copies the Internet code to the clipboard."))
        self.party_host_code_entry.state(["readonly"])
        new_code_btn = ttk.Button(host_holder, text="New code", style="Secondary.TButton",
                                  command=self.new_party_code, width=10)
        new_code_btn.pack(side="left", padx=(6, 0))
        Tooltip(new_code_btn, "Your party code stays the same every time you host, so friends can keep "
                              "it. New code makes a new one (the old one stops working; friends need the "
                              "new code). While the game hosts, use System > Party mode > New party code in "
                              "the game instead. Setting or removing a password also gives a new code "
                              "once.")
        lan_tip = (f"Your party code for players on the same network (same router), like "
                   f"{PARTY_LAN_CODE_EXAMPLE}. Use it when the "
                   "Internet code does not work at home: most routers cannot reach their own public "
                   "address.")
        lan_lbl, self.party_lan_code_entry, lan_copy_btn, _ = self._entry(
            sec, "LAN code\n(same network):", self.party_lan_code_var, lan_tip, width=20,
            button=("Copy", self.copy_lan_code, "Copies the LAN code to the clipboard."))
        self.party_lan_code_entry.state(["readonly"])
        self.party_join_row = [code_lbl, self.party_code_entry.master]
        self.party_host_row = [host_lbl, self.party_host_code_entry.master,
                               lan_lbl, self.party_lan_code_entry.master]
        self.party_status_lbl = ttk.Label(sec, textvariable=self.party_status_var, style="Hint.TLabel",
                                          wraplength=340, justify="left")
        self.party_status_lbl.grid(row=sec.next_row, column=0, columnspan=2, sticky="w", pady=(2, 0))
        sec.next_row += 1

        # Fields enabled in each mode: on (host or join), host only, join only
        self.party_widgets_on = [name_lbl, self.party_name_entry, self.party_max_combo, auto_cb, seamless_cb,
                                 dsummon_cb, hud_cb, tags_cb, glow_cb, npc_talk_cb, pings_cb, walls_cb, stain_cb, save_cb,
                                 port_lbl, self.party_port_entry, upnp_cb, rdv_cb, relay_cb, ipv6_cb, stun_lbl,
                                 self.party_stun_entry,
                                 pw_lbl, self.party_password_entry]
        self.party_widgets_host = [ff_cb, lamps_cb, bring_cb, public_lbl, self.party_public_entry,
                                   host_lbl, self.party_host_code_entry, copy_btn, new_code_btn,
                                   lan_lbl, self.party_lan_code_entry, lan_copy_btn]
        self.party_widgets_join = [code_lbl, self.party_code_entry, paste_btn]
        for var in (self.party_name_var, self.party_port_var, self.party_code_var, self.party_password_var,
                    self.feat_tracing):
            var.trace_add("write", lambda *a: self.update_party_hints())
        self.after(2000, self.party_code_tick)

    def party_mode(self) -> str:
        return dict(PARTY_MODES).get(self.party_mode_var.get(), "")

    # ---- game settings in bbport.ini (the launcher keeps no copy) ----
    def update_upscaler_state(self):
        if hasattr(self, "upscaler_combo"):
            self.upscaler_combo.config(state="readonly" if self.feat_upscaler.get() else "disabled")

    def save_upscaler(self):
        """Enable upscaler + the choice: bbport.ini upscaler (off when unchecked); the choice is
        remembered for re-enabling (launcher_settings.json upscaler_choice)."""
        self.update_upscaler_state()
        self.save_game_settings("upscaler")
        self.save_settings()

    def game_settings_ini(self) -> dict:
        """Every game setting the launcher shows, as bbport.ini values."""
        values = upscaler_ini(self.feat_upscaler.get(), self.upscaler_var.get())
        values.update({key: "1" if var.get() else "0" for key, var in self.ini_flag_vars.items()})
        values.update(volumetric_ini(self.volumetric_var.get()))
        values.update(effect_patches_ini(self.enabled_patches))
        return values

    def save_game_settings(self, *keys):
        """Writes these game settings (all when none given; "effects" = the effect patches) to
        bbport.ini, keeping its other keys."""
        values = self.game_settings_ini()
        if keys:
            wanted = set(keys)
            if "effects" in wanted:
                wanted |= set(effect_patches_ini(()))
            values = {k: v for k, v in values.items() if k in wanted}
        try:
            update_ini(bbport_ini_path(), values)
        except OSError as ex:
            self.log(f"[SETTINGS] Could not save to bbport.ini: {ex}")
        self._ini_stamp = self.ini_stamp()

    @staticmethod
    def ini_stamp():
        try:
            st = bbport_ini_path().stat()
            return st.st_mtime_ns, st.st_size
        except OSError:
            return None

    def refresh_game_settings(self, force=False):
        """Re-reads the game settings from bbport.ini (after the game exits; on focus when the file
        changed), so the launcher shows what was chosen in the game's menus."""
        stamp = self.ini_stamp()
        if not force and stamp == getattr(self, "_ini_stamp", None):
            return
        self._ini_stamp = stamp
        ini = read_ini(bbport_ini_path())
        enabled, label = upscaler_state(ini, self.upscaler_var.get())
        self.feat_upscaler.set(enabled)
        self.upscaler_var.set(label)
        self.update_upscaler_state()
        for _, key, default, _ in INI_FLAG_SETTINGS:
            self.ini_flag_vars[key].set(ini_flag_value(ini, key, default))
        self.volumetric_var.set(volumetric_label(ini))
        self.enabled_patches = ({n for n in self.enabled_patches
                                 if n not in EFFECT_PATCHES and n not in MODEL_LOD_PATCHES}
                                | effect_patches_from_ini(ini))
        self.sync_quick_patch_vars()
        self.frame_generation_var.set(frame_generation_label(ini))
        self.reflex_var.set(reflex_label(ini))
        self.prompt_style_var.set(prompt_style_label(ini))
        self.hdr_var.set(str(ini.get("hdr", "0")).strip() not in ("", "0"))
        self.hdr_peak_var.set(hdr_peak_label(ini))
        self.hdr_paper_var.set(hdr_paper_white_label(ini))
        for key, var, default in (("party_hud", self.party_hud, True), ("party_name_tags", self.party_name_tags, True),
                                  ("party_glow_off", self.party_glow_off, True),
                                  ("party_npc_talk", self.party_npc_talk, True), ("party_pings", self.party_pings, True),
                                  ("party_through_walls", self.party_through_walls, False),
                                  ("party_stain_marker", self.party_stain_marker, False),
                                  ("party_friendly_fire", self.party_friendly_fire, False),
                                  ("party_share_lamps", self.party_share_lamps, True),
                                  ("party_guests_bring_lamps", self.party_guests_bring_lamps, False)):
            var.set(ini_flag_value(ini, key, default))
        for key, _, default in PARTY_INI_SWITCHES:
            self.party_switch_vars[key].set(ini_flag_value(ini, key, default))
        self.party_insight_var.set(party_insight_label(ini))

    def save_frame_generation(self):
        """The Frame generation choice: bbport.ini frame_generation / fg_backend (next start)."""
        try:
            values = frame_generation_ini(self.frame_generation_var.get())
            values["reflex"] = dict(REFLEX_CHOICES).get(self.reflex_var.get(), "off")
            update_ini(bbport_ini_path(), values)
        except OSError as ex:
            self.log(f"[GRAPHICS] Could not save the frame generation setting to bbport.ini: {ex}")

    def save_prompt_style(self):
        """The Button prompts choice: bbport.ini prompt_style (a running game picks it up within a
        second)."""
        try:
            update_ini(bbport_ini_path(),
                       {"prompt_style": dict(PROMPT_STYLE_CHOICES).get(self.prompt_style_var.get(), "auto")})
        except OSError as ex:
            self.log(f"[INPUT] Could not save the button prompts setting to bbport.ini: {ex}")

    def save_hdr(self):
        """The HDR options: bbport.ini hdr / hdr_peak_nits / hdr_paper_white (next start)."""
        try:
            update_ini(bbport_ini_path(), hdr_ini(self.hdr_var.get(), self.hdr_peak_var.get(),
                                                  self.hdr_paper_var.get()))
        except OSError as ex:
            self.log(f"[GRAPHICS] Could not save the HDR setting to bbport.ini: {ex}")

    def save_party_hud(self):
        """The Party HUD checkbox: bbport.ini party_hud (a running game picks it up on its next start;
        in game: System > Party members)."""
        try:
            update_ini(bbport_ini_path(), {"party_hud": "1" if self.party_hud.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Party HUD setting to bbport.ini: {ex}")

    def save_party_name_tags(self):
        """The Party name tags checkbox: bbport.ini party_name_tags (a running game picks it up on its
        next start; in game: System > Party members)."""
        try:
            update_ini(bbport_ini_path(), {"party_name_tags": "1" if self.party_name_tags.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Party name tags setting to bbport.ini: {ex}")

    def save_party_glow_off(self):
        """The Companions without glow checkbox: bbport.ini party_glow_off (a running game picks it up
        on its next start; in game: System > Party look, live)."""
        try:
            update_ini(bbport_ini_path(), {"party_glow_off": "1" if self.party_glow_off.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Companions without glow setting to bbport.ini: {ex}")

    def save_party_npc_talk(self):
        """The Share NPC dialogue checkbox: bbport.ini party_npc_talk (a running game picks it up on its
        next start; in game: System > Party members)."""
        try:
            update_ini(bbport_ini_path(), {"party_npc_talk": "1" if self.party_npc_talk.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Share NPC dialogue setting to bbport.ini: {ex}")

    def save_party_rules(self):
        """The Party Rules & Sync switches and Insight combo: bbport.ini (the game's party rules
        pages show the same keys; a running game reads them at its next start)."""
        try:
            update_ini(bbport_ini_path(), party_rules_ini({k: v.get() for k, v in self.party_switch_vars.items()},
                                                          self.party_insight_var.get()))
        except OSError as ex:
            self.log(f"[PARTY] Could not save the party rules to bbport.ini: {ex}")
        self._ini_stamp = self.ini_stamp()

    def save_party_friendly_fire(self):
        """The Friendly fire / PvP checkbox (host): bbport.ini party_friendly_fire (in game: System >
        Friendly fire)."""
        try:
            update_ini(bbport_ini_path(), {"party_friendly_fire": "1" if self.party_friendly_fire.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Friendly fire setting to bbport.ini: {ex}")

    def save_party_share_lamps(self):
        """The Shared lamps checkbox (host): bbport.ini party_share_lamps (in game: System > World &
        PvP)."""
        try:
            update_ini(bbport_ini_path(), {"party_share_lamps": "1" if self.party_share_lamps.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Shared lamps setting to bbport.ini: {ex}")

    def save_party_guests_bring_lamps(self):
        """The Guests bring their lamps checkbox (host): bbport.ini party_guests_bring_lamps (in game:
        System > World & PvP)."""
        try:
            update_ini(bbport_ini_path(),
                       {"party_guests_bring_lamps": "1" if self.party_guests_bring_lamps.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Guests bring their lamps setting to bbport.ini: {ex}")

    def save_party_pings(self):
        """The Pings checkbox: bbport.ini party_pings (in game: System > Party display)."""
        try:
            update_ini(bbport_ini_path(), {"party_pings": "1" if self.party_pings.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Pings setting to bbport.ini: {ex}")

    def save_party_through_walls(self):
        """The Show party through walls checkbox: bbport.ini party_through_walls (in game: System > Party display)."""
        try:
            update_ini(bbport_ini_path(), {"party_through_walls": "1" if self.party_through_walls.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Show party through walls setting to bbport.ini: {ex}")

    def save_party_stain_marker(self):
        """The Bloodstain marker checkbox: bbport.ini party_stain_marker (in game: System > Party display)."""
        try:
            update_ini(bbport_ini_path(), {"party_stain_marker": "1" if self.party_stain_marker.get() else "0"})
        except OSError as ex:
            self.log(f"[PARTY] Could not save the Bloodstain marker setting to bbport.ini: {ex}")

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
            else:
                pw_problem = party_password_problem(self.party_code_var.get(), self.party_password_var.get())
                if pw_problem:
                    problems.append(f"{pw_problem}: enter the host's password under Password.")
        return problems

    def update_party_hints(self):
        mode = self.party_mode()
        problems = self.party_problems()
        if not mode:
            text = "Party is off: the game runs single-player, as without this tab."
        elif problems:
            text = "✗ " + " ".join(problems)
        elif mode == "host":
            text = ("Give a code to your friends: Internet code for friends elsewhere, LAN code "
                    "for players on your network."
                    if self.party_host_code_var.get() or self.party_lan_code_var.get() else
                    f"The codes (like {PARTY_CODE_EXAMPLE}) appear here after the game starts hosting.")
        else:
            text = "✓ Party code OK. Start the game after the host's game is running."
        warning = party_tracing_warning(mode, self.feat_tracing.get())
        if warning and not problems:
            text += "\n⚠ " + warning
        self.party_status_var.set(text)
        self.party_status_lbl.config(foreground="#d06060" if problems else "#888888")

    def refresh_party_code(self):
        internet, lan = read_party_codes()
        changed = False
        if internet != self.party_host_code_var.get():
            self.party_host_code_var.set(internet)
            changed = True
            if internet and self.party_mode() == "host":
                self.log(f"[PARTY] Internet code (friends elsewhere): {internet}")
        if lan != self.party_lan_code_var.get():
            self.party_lan_code_var.set(lan)
            changed = True
            if lan and self.party_mode() == "host":
                self.log(f"[PARTY] LAN code (same network): {lan}")
        if changed:
            self.update_party_hints()

    def party_code_tick(self):
        if self.party_mode() == "host":
            self.refresh_party_code()
        self.after(2000, self.party_code_tick)

    def copy_party_code(self, lan: bool = False):
        self.refresh_party_code()
        code = (self.party_lan_code_var if lan else self.party_host_code_var).get()
        what = "LAN code" if lan else "Internet code"
        if not code:
            messagebox.showinfo(f"No {what} yet", f"The {what} appears after the game starts hosting "
                                f"(it writes {party_user_dir() / 'party_code.txt'}).")
            return
        self.clipboard_clear()
        self.clipboard_append(code)
        self.log(f"[PARTY] {what} copied to the clipboard.")

    def copy_lan_code(self):
        self.copy_party_code(lan=True)

    def new_party_code(self):
        """The Party tab's New code: the next hosting start makes a new party code."""
        if (self.proc is not None and self.proc.poll() is None) or running_instances():
            messagebox.showinfo(
                "New party code",
                "The game is running. To change the code while hosting, use System > Party > "
                "New party code in the game: the old code stops working at once.")
            return
        if not messagebox.askyesno(
                "New party code",
                "Make a new party code?\n\nThe old code stops working: friends who saved it need the "
                "new one. The new code appears when the game starts hosting.\n\n(Setting or removing "
                "a password also gives a new code, once.)"):
            return
        changed = revoke_party_code(party_user_dir())
        self.party_host_code_var.set("")
        self.party_lan_code_var.set("")
        self.update_party_hints()
        self.log("[PARTY] New party code: the next hosting start makes a new code (old one forgotten"
                 + (": " + ", ".join(changed) if changed else ": there was none yet") + ").")

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
        dlg.geometry("720x700")
        dlg.minsize(640, 480)
        dlg.transient(self)
        dlg.grab_set()
        dlg.configure(bg="#1a1a1a")
        # The sections scroll (0.7.2: with the Rot section they are taller than the 700-pixel window,
        # which has to fit a 1366x768 screen); the button bar below always shows.
        bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        bar.pack(side="bottom", fill="x", padx=10, pady=(0, 10))
        holder = ttk.Frame(dlg, style="Card.TFrame")
        holder.pack(fill="both", expand=True, padx=10, pady=(10, 6))
        canvas = tk.Canvas(holder, bg="#252526", highlightthickness=0, height=300)
        vscroll = ttk.Scrollbar(holder, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vscroll.set)
        vscroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        body = ttk.Frame(canvas, style="Card.TFrame", padding=(14, 10, 14, 12))
        body_window = canvas.create_window((0, 0), window=body, anchor="nw")
        body.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda e: canvas.itemconfig(body_window, width=e.width))
        dlg.bind("<MouseWheel>", lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), "units"))
        dlg.party_rules_parts = (body, bar)  # tests/test_launcher_rules_layout.py
        body.columnconfigure(0, weight=1, uniform="sections")
        body.columnconfigure(1, weight=1, uniform="sections")
        ttk.Label(body, text="Hover over an option for details. Changes apply at the next launch. "
                             "Defaults are the recommended settings.", style="Hint.TLabel").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
        var = self.party_switch_vars

        def save():
            self.save_settings()
            self.save_party_rules()

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
                    "are for ringing by hand. Default: on (bbport.ini party_grant_bells; in the game: System > Party rewards).", command=save)
        self._check(sec, "Open world (no co-op area walls)", var["party_open_world"],
                    "Removes the walls the game puts on area borders while a helper is in your world, "
                    "so the party can walk anywhere the host can. Off: the game's walls stay. Keep it the "
                    "same for everyone. Default: on (bbport.ini party_open_world; in the game: System > World & PvP).", command=save)
        self._check(sec, "Get up after a boss kill", var["party_host_limbo_revive"],
                    "When you die in a boss fight and the others kill the boss, you get up right there in "
                    "the arena (where you fell, or beside a living hunter), with no loading screen; your "
                    "Blood Echoes stay in your bloodstain. Host and guests alike; the host's setting. Off: a host's death "
                    "takes the whole party's respawn loading screen at the host's lamp, a guest gets up "
                    "at the host's lamp. "
                    "Default: on (bbport.ini party_host_limbo_revive; in the game: System > Deaths & revives).", command=save)
        self._check(sec, "Host gets up without a loading screen", var["party_host_revive"],
                    "When the host dies, it gets up at its lamp in the loaded area with no loading screen for "
                    "anyone (a lamp in another area: the host loads alone). Off: the whole party gets the "
                    "respawn loading screen. Host's setting. Default: on (bbport.ini party_host_revive; in "
                    "the game: System > Deaths & revives).", command=save)

        sec = section(1, 1, "Guests")
        self._check(sec, "Get up without a loading screen", var["party_guest_revive_inplace"],
                    "A guest who dies gets up at the host's lamp right away, with no loading screen (after a "
                    "fall: the respawn load). Off: every death takes the respawn loading screen. Applies to "
                    "you while you are a guest. Default: on (bbport.ini party_guest_revive_inplace; in the "
                    "game: System > Deaths & revives).", command=save)
        self._check(sec, "Respawn at the host's lamp", var["party_guest_respawn"],
                    "A guest who dies comes back at the host's last lamp and is summoned again, instead of "
                    "being sent home to their own world. Applies to you while you are a guest. "
                    "Default: on (bbport.ini party_guest_respawn; in the game: System > Deaths & revives).", command=save)
        self._check(sec, "Refill vials and bullets", var["party_guest_refill"],
                    "Guests get their Blood Vials and Quicksilver Bullets refilled when the host rests at a "
                    "lamp, dies or travels to the Dream, as the host does. Applies to you while you are a "
                    "guest. Default: on (bbport.ini party_guest_refill; in the game: System > Deaths & revives).", command=save)
        self._check(sec, "Stay in the host's Chalice Dungeon", var["party_chalice_rejoin"],
                    "A guest who dies in the host's Chalice Dungeon gets up beside the party there. A guest "
                    "pulled out of the dungeon by a death or a forced trip (a scripted warp, a host's trip it "
                    "could not follow) goes back to their own world and is summoned into the dungeon again "
                    "automatically. A guest who chose to leave (\"alone\" at a lamp, their own Hunter's Mark) "
                    "stays out until they press Party > Rejoin. Applies to you while you are a guest. Default: on "
                    "(bbport.ini party_chalice_rejoin; in the game: System > Chalice Dungeons).", command=save)
        self._check(sec, "Guests can use Hunter's Mark", var["party_guest_mark"],
                    "Lets a guest use the Hunter's Mark (and Bold Hunter's Mark). Like a lamp it asks "
                    "\"Travel with the party?\": alone to your own last lamp, or the party to the host's. "
                    "Off: guests cannot use it, as in the normal game. Default: on (bbport.ini party_guest_mark; in the game: System > World & PvP).",
                    command=save)
        self._check(sec, "Full rewards for guests", var["party_full_rewards"],
                    "Guests get the full Blood Echoes for kills and bosses and the full enemy drops, like the "
                    "host. Off: the game's co-op rules (half echoes, only vials and bullets drop). Applies "
                    "to you while you are a guest. Default: on (bbport.ini party_full_rewards; in the game: System > Party rewards).", command=save)
        self._check(sec, "Share the host's item rewards", var["party_items"],
                    "Boss drops, key items and gifts the host receives (which guests never get in the "
                    "normal game) are given to each guest too, in their own world, once each. "
                    "Default: on (bbport.ini party_items; in the game: System > Party rewards).", command=save)
        insight_combo = self._combo(
            sec, "Insight for bosses:", self.party_insight_var, [c[0] for c in PARTY_INSIGHT_CHOICES],
            "Insight a guest gets when the host kills a boss. Same as the host: the guest ends up with "
            "the same Insight the host got for that boss. Full bonus: that much on top of the game's own "
            "+1. Game's own: only the +1 the game gives a helper. "
            "Default: Same as the host (bbport.ini party_guest_insight=parity|full|0; in the game: System > Party rewards).", width=24)
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
                    "Default: on (bbport.ini party_progress; in the game: System > Story & dialogue).", command=update_dependents)
        npc_cb = self._check(sec, "    Also NPC quests", var["party_progress_npc"],
                             "NPC quest choices are shared: the first choice anyone makes (Eileen, Alfred, "
                             "Djura ...) applies to the host's world and every player's save. Guest's setting. "
                             "Default: on (bbport.ini party_progress_npc; in the game: System > Story & dialogue).", command=save)
        self._check(sec, "Share cutscenes and endings", var["party_story"],
                    "Every guest also sees the cutscenes the host triggers (shown again in the guest's own "
                    "world if needed), the time of day (evening, night, Blood Moon) follows the host, and "
                    "the ending the host gets plays for everyone. Needs to be on for the host and for "
                    "each guest. Default: on (bbport.ini party_story; in the game: System > Story & dialogue).", command=update_dependents)
        mirror_cb = self._check(sec, "    Play them live with the host", var["party_story_mirror"],
                                "A guest in the host's world watches the cutscene at the same time as the "
                                "host. Off: the guest sees it later, back in their own world. Guest's "
                                "setting. Default: on (bbport.ini party_story_mirror; in the game: System > Story & dialogue).", command=save)

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

        # 0.7.1: the host's world rules (System > Shared world) and the guest's key items (Party rewards)
        sec = section(3, 0, "Shared world (host)")
        self._check(sec, "Enemies stay dead", var["party_kill_record"],
                    "An enemy the party killed is dead on every player's game: a kill across an area border "
                    "reaches the others, and an enemy that comes back alive when its area loads again as you "
                    "walk is put down again. Only a rest or a party wipe brings enemies back. The host's "
                    "setting; guests follow it. Off: a kill another player did not see can leave the enemy "
                    "standing on their game. Default: on (bbport.ini party_kill_record; in the game: System > Shared world).", command=save)
        self._check(sec, "Host can gather the party", var["party_gather"],
                    "Lets the host bring every guest to it with System > Party > Gather the party (each guest "
                    "is asked first and may say no). The host's setting. Off: the row says gathering is off. "
                    "Default: on (bbport.ini party_gather; in the game: System > Shared world).", command=save)
        self._check(sec, "NPCs as in the host's world", var["party_npc_mirror"],
                    "An NPC that is gone from the host's world (moved on by a story event, such as the one "
                    "at the clinic's door) is gone for the guests too. The host's setting; guests follow it. "
                    "Default: on (bbport.ini party_npc_mirror; in the game: System > Shared world).", command=save)
        self._check(sec, "Covenant outcomes per player", var["party_covenant_per_player"],
                    "How the covenant quests end stays in each player's own world and save: Alfred killing "
                    "Queen Annalise, her revival, Valtr's death and the other covenant leaders' fates. While "
                    "you are summoned the host's world shows the host's state; nothing of it reaches your "
                    "save, and nothing of yours reaches the host's. Lamps, bosses, doors and other quests "
                    "stay shared. The host's setting; guests follow it. Off: covenant outcomes are shared like "
                    "other NPC quests. Default: on (bbport.ini party_covenant_per_player; in the game: "
                    "System > Shared world).", command=save)
        self._check(sec, "Party votes on travel", var["party_travel_vote"],
                    "A party travel by lamp, headstone or Hunter's Mark only happens when every member votes yes: "
                    "each one gets the game's YES / NO question. One no, or no answer within 20 s, cancels it "
                    "and everyone is told who said no or did not answer. The host's setting; guests see it. "
                    "Default: off (bbport.ini party_travel_vote; in the game: System > Shared world).", command=save)
        self._check(sec, "NPC hunters fight the whole party", var["party_npc_fights"],
                    "A story NPC hunter who turns on the host (Alfred, the blood-drunk Eileen, Valtr, the "
                    "DLC hunters) turns on the guests too, and Brador is shown on every guest's screen: "
                    "everyone sees and fights the same hunter, and its death counts for all. The host's "
                    "setting; guests follow it. Default: on (bbport.ini party_npc_fights; in the game: "
                    "System > Shared world).", command=save)

        # 0.7.2: rot, the host's rules (System > Rot)
        sec = section(4, 0, "Rot on death (host)")
        rot_cbs = []

        def update_rot():
            for cb in rot_cbs:
                cb.state(["!disabled"] if var["party_rot"].get() else ["disabled"])
            save()
        self._check(sec, "Rot on death", var["party_rot"],
                    "Each death stacks one penalty, picked at random, up to 5: max HP -10 %, max stamina -10 %, "
                    "stamina costs +10 %, rally -15 % or item discovery -10. Resting at a lamp (even just "
                    "opening its travel menu) or levelling up cleanses it. The host's setting counts for the "
                    "whole party. Default: off (bbport.ini party_rot; in the game: System > Rot).",
                    command=update_rot)
        rot_cbs.append(self._check(sec, "    A boss kill cleanses it", var["party_rot_cleanse_boss"],
                                   "When a boss dies, every party member in the host's world loses all their rot, "
                                   "alive or dead in that fight. Needs Rot on death. Default: on (bbport.ini "
                                   "party_rot_cleanse_boss; in the game: System > Rot).", command=save))
        rot_cbs.append(self._check(sec, "    Killing your killer cleanses it", var["party_rot_cleanse_killer"],
                                   "When the enemy that killed you dies (anyone in the party may kill it), you lose "
                                   "the rot of that death. A fall or a trap has no killer; a rest brings the killer "
                                   "back. Needs Rot on death. Default: on (bbport.ini party_rot_cleanse_killer; in "
                                   "the game: System > Rot).", command=save))
        rot_cbs.append(self._check(sec, "    Retrieving your Blood Echoes cleanses it", var["party_rot_cleanse_echoes"],
                                   "Picking up your bloodstain removes the rot of the death that dropped it (not a "
                                   "second time if your killer already died). Needs Rot on death. Default: on "
                                   "(bbport.ini party_rot_cleanse_echoes; in the game: System > Rot).",
                                   command=save))

        sec = section(3, 1, "Key items (guest)")
        keys_now_cb = None

        def update_keys():
            keys_now_cb.state(["!disabled"] if var["party_key_view"].get() else ["disabled"])
            save()
        self._check(sec, "Own key items as a guest", var["party_key_view"],
                    "While you are a guest in the host's world, your Key Items tab shows your own keys, "
                    "badges and tools. Off: the game's view for a visiting hunter (an empty list; your save "
                    "keeps them). Applies to you while you are a guest; at the next start. "
                    "Default: on (bbport.ini party_key_view; in the game: System > Party rewards).", command=update_keys)
        keys_now_cb = self._check(sec, "    Key items at once", var["party_key_items_summoned"],
                                  "A key item the host's world gives you (Oedon Tomb Key, a badge) is yours "
                                  "right away, while you are still in the host's world. Off: it waits until "
                                  "you are back in your own world. Needs Own key items. Default: on "
                                  "(bbport.ini party_key_items_summoned; in the game: System > Party rewards).",
                                  command=save)

        update_dependents()
        update_keys()
        update_rot()

        def on_defaults():
            for key, _, default in PARTY_SWITCHES + PARTY_INI_SWITCHES:
                var[key].set(default)
            self.party_start_var.set(PARTY_START_CHOICES[0][0])
            self.party_insight_var.set(PARTY_INSIGHT_CHOICES[0][0])
            self.party_backup_minutes_var.set("10")
            self.party_backup_keep_var.set("10")
            update_dependents()
            update_keys()
            update_rot()
            self.save_party_rules()

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

    def start_update_check(self):
        """Background check for a newer release (scripts/update_check.py); never blocks or fails."""
        if not self.check_updates.get():
            return
        self._update_result = []

        def work():
            try:
                scripts_dir = str(APP_DIR / "scripts")
                if scripts_dir not in sys.path:
                    sys.path.insert(0, scripts_dir)
                import update_check
                self._update_result.append(update_check.check(PORT_VERSION, UPDATE_CACHE_FILE))
            except Exception:
                self._update_result.append(None)
        threading.Thread(target=work, daemon=True).start()
        self.after(500, lambda: self.poll_update_check(20))

    def poll_update_check(self, tries):
        if not self._update_result:
            if tries > 0:
                self.after(500, lambda: self.poll_update_check(tries - 1))
            return
        notice = self._update_result[0]
        if notice is None:
            return
        import webbrowser
        self.update_label.config(text=f"{notice.text} (download: {notice.url})")
        self.update_label.bind("<Button-1>", lambda _e: webbrowser.open(notice.url))
        self.update_label.pack(anchor="w", pady=(0, 6), after=self._update_anchor)
        self.log(f"[UPDATE] {notice.text}: {notice.url}")

    def undo_last_party_session(self):
        """Restore the newest party session snapshot (scripts/party_session.py) after a confirmation."""
        if self.proc is not None or running_instances():
            messagebox.showwarning("Game running", "Close the game before restoring saves.")
            return False
        try:
            pss = party_session_module()
        except ImportError as ex:
            messagebox.showerror("Undo last party session", f"scripts\\party_session.py is missing:\n{ex}")
            return False
        user = party_user_dir()
        session = pss.last_session(user)
        if session is None:
            messagebox.showinfo("Undo last party session",
                                "There is nothing to undo yet: the game saves a copy of your saves at the start of "
                                f"every party session (in {user / pss.SESSIONS}).")
            return False
        lines = pss.describe(user, session)
        if not messagebox.askyesno(
                "Undo last party session?",
                f"Put your saves back as they were at {session.label}?\n\nThis replaces:\n"
                + "\n".join(lines)
                + "\n\nYour current saves are copied to save_backups\\party_sessions first "
                  "(\"your saves before an undo\"): pressing Undo last party session again brings them back."):
            return False
        try:
            restored, backup = pss.undo(user, session)
        except (OSError, ValueError) as ex:
            messagebox.showerror("Undo last party session", f"The restore failed:\n{ex}")
            return False
        self.log(f"[PARTY] Saves restored from {restored.path} (your saves before it: {backup})")
        messagebox.showinfo("Undo last party session",
                            f"Your saves are back as they were at {restored.label}.\n\n"
                            f"Your saves from before the undo are in:\n{backup}")
        return True

    def check_last_party_session(self):
        """After a party session (the game closed, or at the next launcher start): do the party saves
        still load, and did nothing go backwards? On a problem: offer the undo."""
        if self.proc is not None or running_instances():
            return
        try:
            pss = party_session_module()
            user = party_user_dir()
            session = pss.needs_check(user)
            if session is None:
                return
            problems = pss.check(user, session)
            pss.mark_checked(user, session, problems)
        except Exception as ex:  # a check must never keep the launcher from working
            self.log(f"[PARTY] The save check after the party session failed: {ex}")
            return
        if not problems:
            self.log(f"[PARTY] Save check after the party session ({session.label}): OK")
            return
        for level, msg in problems:
            self.log(f"[PARTY] Save check after the party session: {level}: {msg}")
        if messagebox.askyesno(
                "Party save check",
                "Your party save looks wrong after the last party session:\n\n"
                + "\n".join(f"- {msg}" for _, msg in problems[:8])
                + f"\n\nUndo the last party session? Your saves go back to {session.label} "
                  "(you see what is replaced first; the current saves are kept, so this can be undone)."):
            self.undo_last_party_session()

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
        self.update_upscaler_state()
        self.save_settings()
        self.save_game_settings()
        self.log("Preset applied: Vanilla mode (all optional features off, FPS preset=30 native, BB_ANISO=0, all patches off, party off).")

    def set_everything_on(self):
        self.feat_upscaler.set(True)
        self.feat_object_motion.set(True)
        self.feat_overlay.set(True)
        self.feat_fps_patch.set(True)
        self.feat_frame_hooks.set(True)
        self.feat_mods.set(True)
        self.feat_res_scaling.set(True)
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
        self.update_upscaler_state()
        self.save_settings()
        self.save_game_settings()
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
        self.save_game_settings("effects")

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
            self.save_game_settings("effects")
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
        wheel = tk.BooleanVar(value=ini.get("mk_wheel", "1") not in ("", "0"))
        inv_row = ttk.Frame(main, style="Card.TFrame")
        inv_row.grid(row=row, column=0, columnspan=3, sticky="w", pady=(4, 8))
        row += 1
        for text, var, key in (("Invert horizontal look", invert_x, "mk_invert_x"),
                               ("Invert vertical look", invert_y, "mk_invert_y")):
            cb = ttk.Checkbutton(inv_row, text=text, variable=var, style="Card.TCheckbutton")
            cb.pack(side="left", padx=(0, 16))
            Tooltip(cb, f"Turns the camera the other way for this mouse axis (bbport.ini {key}).")
        cb = ttk.Checkbutton(inv_row, text="Mouse wheel: quick items", variable=wheel, style="Card.TCheckbutton")
        cb.pack(side="left", padx=(0, 16))
        Tooltip(cb, "The wheel steps the quick items and tools (d-pad up / down), one press per notch "
                    "(bbport.ini mk_wheel).")
        direct_cam = tk.BooleanVar(value=ini.get("mk_direct_camera", "0") not in ("", "0"))
        cb = ttk.Checkbutton(inv_row, text="Direct mouse camera", variable=direct_cam, style="Card.TCheckbutton")
        cb.pack(side="left", padx=(0, 16))
        Tooltip(cb, "The mouse turns the camera itself, 1:1 like a PC game, instead of through the right "
                    "stick (no dead zone or turn curve); the stick look is kept while locked on. "
                    "Imedved's Bloodborne mouse camera (bbport.ini mk_direct_camera).")

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
                             "(Insert) or the name box is open. Rebind keys in the in-game menu (Insert > "
                             "Controls: select a row, press a key or mouse button), or with mkkey.<input>= lines in "
                             "bbport.ini (key.<input>= lines are the classic layout's); mouse buttons and "
                             "the wheel are names there too (Mouse Left, Mouse X1, Wheel Down ...). Off: the classic "
                             "layout (WASD / IJKL / Space / Shift ...) is used and the mouse does nothing.",
                  style="Hint.TLabel", wraplength=580, justify="left").grid(
            row=row, column=0, columnspan=3, sticky="w", pady=(8, 0))

        bottom_bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        bottom_bar.pack(fill="x", padx=10, pady=(0, 10))

        def on_save():
            values = {key: f"{var.get():.2f}" for key, var in tuning_vars.items()}
            values["mk_invert_x"] = "1" if invert_x.get() else "0"
            values["mk_invert_y"] = "1" if invert_y.get() else "0"
            values["mk_wheel"] = "1" if wheel.get() else "0"
            values["mk_direct_camera"] = "1" if direct_cam.get() else "0"
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
            "dream_mirror": self.feat_dream_mirror.get(),
            "lamp_travel": self.feat_lamp_travel.get(),
            "hide_vk": self.hide_vk_var.get(),
            # Game settings (upscaler, object motion, HUD, M&K, shader switches, motion blur resolution,
            # effect patches) are in bbport.ini; only the last upscaler choice is remembered here.
            "ini_sync_rev": INI_SYNC_REV,
            "upscaler_choice": self.upscaler_var.get(),
            "feat_overlay": self.feat_overlay.get(),
            "feat_fps_patch": self.feat_fps_patch.get(),
            "feat_frame_hooks": self.feat_frame_hooks.get(),
            "feat_res_scaling": self.feat_res_scaling.get(),
            "feat_tracing": self.feat_tracing.get(),
            "tracing_rev": 1,
            "feat_watchdog": self.feat_watchdog.get(),
            "feat_draw_prep": self.feat_draw_prep.get(),
            "feat_fullscreen": self.window_mode_var.get() == "Fullscreen",
            "window_mode": self.window_mode_var.get().lower(),
            "feat_overlay_pad": self.feat_overlay_pad.get(),
            "feat_game_menu": self.feat_game_menu.get(),
            "feat_skip_network_choice": self.feat_skip_network_choice.get(),
            "feat_save_log": self.feat_save_log.get(),
            "feat_audio_log": self.feat_audio_log.get(),
            "feat_arrival_log": self.feat_arrival_log.get(),
            "feat_spectate_log": self.feat_spectate_log.get(),
            "feat_hostdead_log": self.feat_hostdead_log.get(),
            "feat_boot_watchdog": self.feat_boot_watchdog.get(),
            "feat_mute_unfocused": self.feat_mute_unfocused.get(),
            "feat_shader_precompile": self.feat_shader_precompile.get(),
            "display": dict(self.display_choices).get(self.display_var.get(), ""),
            "gamepad": gamepad,
            "gamepad_name": gamepad_label.removesuffix(" (not connected)") if gamepad else "",
            "preupload": dict(PREUPLOAD_CHOICES).get(self.preupload_var.get(), ""),
            "frames_ahead": dict(FRAMES_AHEAD_CHOICES).get(self.frames_ahead_var.get(), "2"),
            "enabled_patches": sorted(n for n in self.enabled_patches
                                      if n not in EFFECT_PATCHES and n not in MODEL_LOD_PATCHES),
            "party_mode": self.party_mode(),
            "party_name": self.party_name_var.get().strip(),
            "party_max": self.party_max_var.get(),
            "party_auto": self.party_auto.get(),
            "party_seamless": self.party_seamless.get(),
            "party_direct_summon": self.party_direct_summon.get(),
            "party_separate_save": self.party_separate_save.get(),
            "party_port": self.party_port_var.get().strip(),
            "party_upnp": self.party_upnp.get(),
            "party_rendezvous": self.party_rendezvous.get(),
            "party_member_relay": self.party_member_relay.get(),
            "party_ipv6": self.party_ipv6.get(),
            "party_public_addr": self.party_public_var.get().strip(),
            "party_stun": self.party_stun_var.get().strip(),
            "party_password": self.party_password_var.get(),
            "party_code": self.party_code_var.get().strip(),
            "party_start": dict(PARTY_START_CHOICES).get(self.party_start_var.get(), PARTY_START_CHOICES[0][1]),
            "party_backup_minutes": self.party_backup_minutes_var.get(),
            "party_backup_keep": self.party_backup_keep_var.get(),
            "party_log_level": dict(PARTY_LOG_LEVELS).get(self.party_log_level_var.get(), ""),
            "party_log_areas": [key for key, var in self.party_log_area_vars.items() if var.get()],
            "auto_support_bundle": self.auto_support_bundle.get(),
            "check_updates": self.check_updates.get(),
            **{key: self.party_switch_vars[key].get() for key, _, _ in PARTY_SWITCHES},
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

    # ------------------------------------------------------------- DLSS library (upstream 0.51)
    @staticmethod
    def dlss_user_dll() -> Path:
        return party_user_dir() / "dlss" / "nvngx_dlss.dll"

    def dlss_status(self) -> tuple:
        """(short status for the DLSS row, its explanation for the row's tooltip)."""
        def found(name):
            return ((party_user_dir() / "dlss" / name).is_file() or (APP_DIR / "out" / name).is_file()
                    or (APP_DIR / name).is_file())
        fg = found("nvngx_dlssg.dll")
        fg_detail = (" NVIDIA's frame generation runtime (nvngx_dlssg.dll) was found."
                     if fg else " No frame generation runtime (nvngx_dlssg.dll) was found.")
        if self.dlss_user_dll().is_file():
            short, detail = "your own", "Now: your own library (user\\dlss\\nvngx_dlss.dll) is used."
        elif (APP_DIR / "out" / "nvngx_dlss.dll").is_file() or (APP_DIR / "nvngx_dlss.dll").is_file():
            short, detail = "bundled", "Now: the bundled library next to bbport.exe is used."
        else:
            return "none (FSR used)", "Now: no DLSS library was found; DLSS falls back to FSR."
        return short + (" + FG" if fg else ""), detail + fg_detail

    def update_dlss_status(self):
        short, detail = self.dlss_status()
        self.dlss_var.set(short)
        for tip in getattr(self, "dlss_tips", []):
            tip.text = self.dlss_base_tip + "\n\n" + detail

    def choose_dlss_dll(self):
        # nvngx_dlss.dll (Super Resolution) or nvngx_dlssg.dll (frame generation), kept under its
        # own name in user\dlss.
        chosen = filedialog.askopenfilename(title="Select NVIDIA's nvngx_dlss.dll or nvngx_dlssg.dll",
                                            filetypes=[("NVIDIA DLSS", "nvngx_dlss.dll nvngx_dlssg.dll"),
                                                       ("DLL", "*.dll")])
        if not chosen:
            return
        target = self.dlss_user_dll()
        if Path(chosen).name.lower() == "nvngx_dlssg.dll":
            target = target.with_name("nvngx_dlssg.dll")
        try:
            target.parent.mkdir(parents=True, exist_ok=True)
            import shutil
            shutil.copyfile(chosen, target)
            self.log(f"[DLSS] {chosen} copied to {target}")
        except OSError as e:
            self.log(f"[ERROR] Could not copy the DLSS library: {e}")
        self.update_dlss_status()

    def remove_dlss_dll(self):
        try:
            self.dlss_user_dll().unlink(missing_ok=True)
            self.dlss_user_dll().with_name("nvngx_dlssg.dll").unlink(missing_ok=True)
            self.log("[DLSS] Your own DLSS library removed; the bundled one is used")
        except OSError as e:
            self.log(f"[ERROR] Could not remove the DLSS library: {e}")
        self.update_dlss_status()

    def reset_shader_cache(self):
        """Delete the pipeline cache: a damaged entry draws parts of an area wrong until then."""
        if self.proc is not None and self.proc.poll() is None:
            messagebox.showinfo("Reset shader cache", "Close the game first.")
            return
        cache_dir = shader_cache_root()
        files = [p for p in cache_dir.rglob("*") if p.is_file()] if cache_dir.is_dir() else []
        if not files:
            messagebox.showinfo("Reset shader cache", "The shader cache is already empty.")
            return
        if not messagebox.askyesno(
                "Reset shader cache",
                f"Delete the shader cache ({len(files)} files in {cache_dir})?\n\n"
                "Use it when an area is drawn wrong (missing or broken textures) on this PC "
                "but not on others. The game compiles the shaders again while you play: a few "
                "more stutters the first time each area is visited."):
            return
        try:
            shutil.rmtree(cache_dir)
        except OSError as ex:
            messagebox.showerror("Reset shader cache", f"Could not delete {cache_dir}:\n{ex}")
            return
        self.log(f"[CACHE] Shader cache deleted ({len(files)} files); it is rebuilt while playing.")

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

        # 1b. Game Title & Version verification (any retail release, 1.09)
        try:
            import sys
            scripts_dir = str(APP_DIR / "scripts")
            if scripts_dir not in sys.path:
                sys.path.insert(0, scripts_dir)
            import game_check
            chk = game_check.problem(str(game_dir), cache=APP_DIR / "out" / "game_check.json")
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
                    f"Expected: a retail Bloodborne release (any region, e.g. CUSA03173 or CUSA00900) with Update 1.09 merged.\n"
                    f"Why blocked: Upstream port patches and memory hooks rely strictly on the 1.09 executable layout.\n"
                    f"To bypass (not recommended): set environment variable BB_SKIP_GAME_CHECK=1."
                ), details
            else:
                details.append("[OK] Game Title & Version: Bloodborne v1.09 verified")
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

    def warn_unclean_eboot(self, game_dir):
        """With the game check bypassed (BB_SKIP_GAME_CHECK=1), a modified eboot.bin starts but the
        port cannot patch it: a warning, the launch goes on (from Supermedo/bloodborne_pc 155a270b,
        where it was only in the log)."""
        if os.environ.get("BB_SKIP_GAME_CHECK") != "1":
            return  # verify_prelaunch already refused it
        try:
            scripts_dir = str(APP_DIR / "scripts")
            if scripts_dir not in sys.path:
                sys.path.insert(0, scripts_dir)
            import game_check
            clean = game_check.eboot_clean(str(game_dir))
        except Exception as ex:
            self.log(f"[PRE-LAUNCH] eboot.bin check skipped: {ex}")
            return
        if clean is not False:
            return
        text = ("Your eboot.bin is not the clean 1.09 one (modified, or from another dump). "
                + game_check.UNCLEAN_EBOOT_EFFECTS[0].upper() + game_check.UNCLEAN_EBOOT_EFFECTS[1:]
                + ".\n\nReplace eboot.bin with the one from an unmodified 1.09 update.")
        self.log(f"[PRE-LAUNCH WARNING] {text.splitlines()[0]}")
        messagebox.showwarning("Bloodborne", text)

    def blocked_by_running_instance(self) -> bool:
        """True while another bbport.exe is alive: a second copy exhausts RAM and VRAM.

        The launch stays blocked until the old process is gone, closed by hand or terminated here.
        """
        found = running_instances()
        if not found:
            return False
        lines = "\n".join(bb_guard.describe(i) for i in found)
        self.log(f"[PRE-LAUNCH] bbport.exe is already running ({lines.replace(chr(10), '; ')})")
        if not messagebox.askyesno(
                "Bloodborne is already running",
                f"bbport.exe is still running:\n\n{lines}\n\n"
                "A second copy would run out of memory, so the launch is blocked until it is gone.\n"
                "Terminate it now and continue?",
                icon="warning"):
            self.log("[PRE-LAUNCH] Launch cancelled: close the running bbport.exe first.")
            return True
        survivors = [i.pid for i in found if not bb_guard.terminate(i.pid)]
        if survivors:
            self.log(f"[PRE-LAUNCH ERROR] Could not terminate PID {', '.join(map(str, survivors))}")
            messagebox.showerror(
                "Cannot terminate bbport.exe",
                f"PID {', '.join(map(str, survivors))} did not exit (stuck in the GPU driver?).\n"
                "End it in Task Manager or restart Windows, then launch again.")
            return True
        self.log("[PRE-LAUNCH] Old bbport.exe terminated.")
        return False

    def launch_game(self, warper: bool = False):
        if self.proc is not None:
            return
        self._warper_run = warper

        if self.blocked_by_running_instance():
            return

        target = self.eboot_var.get().strip()
        if not target:
            messagebox.showerror("Error", "Please select eboot.bin or the game folder first.")
            return

        game_dir = resolve_game_dir(target)
        if game_dir is None:
            messagebox.showerror("Invalid Path", f"Could not find eboot.bin in:\n{target}")
            return

        party_problems = [] if warper else self.party_problems()
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
        self.warn_unclean_eboot(game_dir)
        for name, stats in import_dropped_caches():
            if stats is None:
                self.log(f"Shader cache {name} in {cache_import_dir()}: not a cache archive (renamed .bad)")
            else:
                self.log(f"Shader cache {name} from {cache_import_dir()} merged: {stats['added']} new files, "
                         f"{stats['existing']} already present, {stats['gpu_skipped']} made for another GPU"
                         + (f" (GPU part used: {', '.join(stats['gpu_used'])})" if stats["gpu_used"] else ""))

        env = os.environ.copy()
        env["BB_FROM_LAUNCHER"] = "1"  # run.bat: do not reload launcher_settings.json
        # run.bat uses the Python running the launcher (3.10+ already), not its own search
        # (from Mrsuss60/bloodborne_pc_windows_port 1af887a). pythonw has no console output.
        py = Path(sys.executable)
        if py.name.lower() == "pythonw.exe" and (py.parent / "python.exe").exists():
            py = py.parent / "python.exe"
        if py.resolve().parent == APP_DIR / "python":
            # The release's bundled python\ folder: always hand run.bat that one (a PYTHON set in the
            # user's environment would otherwise win), and never write __pycache__ into it.
            env["PYTHON"] = str(py)
            env["PYTHONDONTWRITEBYTECODE"] = "1"
            env.pop("PYTHONHOME", None)
            env.pop("PYTHONPATH", None)
        else:
            env.setdefault("PYTHON", str(py))
        for key, value in settings_env(self.current_settings()).items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
        env["BB_GAME_DIR"] = str(game_dir)
        if warper:
            warper_env(env, warper_paths())
        elif not self.offer_party_save_copy(env):
            self.log("[PARTY] Not started (party save copy cancelled).")
            return
        res_choice = self.res_var.get() if self.feat_res_scaling.get() else "Native (1080p, scaling off)"
        active_patches = sorted(list(self.enabled_patches))

        rotate_launcher_log()
        self.launch_time = datetime.now().timestamp()
        self._stop_requested = False
        try:
            self.log_fh = open(LOG_FILE, "w", encoding="utf-8")
        except OSError:
            self.log_fh = None

        features_str = (
            f"Features: FSR={'on' if self.feat_upscaler.get() else 'off'}, "
            f"ObjectMotion={'on' if self.feat_object_motion.get() else 'off'}, "
            f"Overlay={'on' if self.feat_overlay.get() else 'off'}, "
            f"FPSPatch={'on' if self.feat_fps_patch.get() else 'off'}, "
            f"FrameHooks={'on' if self.feat_frame_hooks.get() else 'off'}, "
            f"Mods={'on' if self.feat_mods.get() else 'off'}, "
            f"ResScaling={'on' if self.feat_res_scaling.get() else 'off'}, "
            f"Tracing={'on' if self.feat_tracing.get() else 'off'}, "
            f"Watchdog={'on' if self.feat_watchdog.get() else 'off'}, "
            f"DrawPrep={'on' if self.feat_draw_prep.get() else 'off'}, "
            f"WindowMode={self.window_mode_var.get()}, "
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
        if warper:
            self.log(f"World warper: building the shader cache on a copy of the save ({env['BB_USER_DIR']}); "
                     f"progress file {env['BB_WORLD_WARPER_STATE']}")
        elif env.get("BB_PARTY"):
            self.log(f"Party: {env['BB_PARTY']} | Name: {env['BB_PARTY_NAME']} | Port: {env['BB_PARTY_PORT']} | "
                     f"Max: {env['BB_PARTY_MAX']} | Auto: {env['BB_PARTY_AUTO']} | "
                     f"Seamless: {env['BB_PARTY_SEAMLESS']} | Save: {env['BB_PARTY_SAVE']} | "
                     f"UPnP: {env['BB_PARTY_UPNP']}"
                     + (f" | Code: {env['BB_PARTY_CODE']}" if env.get("BB_PARTY_CODE") else ""))
        else:
            self.log("Party: off")
        tracing_warning = party_tracing_warning(env.get("BB_PARTY") or "", self.feat_tracing.get())
        if tracing_warning:
            self.log(f"[WARNING] {tracing_warning}")
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
        self.warper_btn.config(state="disabled")
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
        if proc is not None and proc.poll() is None and not getattr(self, "_stopping", False):
            self._stopping = True
            self._stop_requested = True  # a stopped game is no crash: no automatic support bundle
            self.stop_btn.config(state="disabled")

            def run():
                try:
                    stop_tree(proc, log=self.log_queue.put)
                finally:
                    self._stopping = False
            threading.Thread(target=run, daemon=True).start()

    def on_finished(self, rc: int):
        if self.hidden_count:
            self.log(f"({self.hidden_count} Vulkan warning lines hidden; full output is in launcher.log)")
        code = f"{rc}" if 0 <= rc < 256 else f"{rc} (0x{rc & 0xFFFFFFFF:08X})"
        self.log(f"Process finished with code: {code}")
        for instance in running_instances():
            self.log(f"[WARNING] bbport.exe is still running after the game ended "
                     f"({bb_guard.describe(instance)}); the next launch will ask to terminate it.")
        crashed = rc != 0 or new_crash_logs(getattr(self, "launch_time", None))
        self.proc = None
        self.close_log_file()
        if crashed and self.auto_support_bundle.get() and not getattr(self, "_stop_requested", False):
            self.write_support_bundle(None, note=f"automatic: the game ended with code {code}")
        self.launch_btn.config(state="normal")
        self.warper_btn.config(state="normal")
        self.stop_btn.config(state="disabled")
        self.refresh_game_settings(force=True)  # what was chosen in the game's menus
        self.after(1500, self.check_last_party_session)
        if getattr(self, "_warper_run", False):
            self._warper_run = False
            self.after(500, lambda: self.warper_finished(rc))

    # ---- World warper (Advanced > Shader cache builder) ----
    def build_shader_cache_warper(self):
        if self.proc is not None:
            messagebox.showinfo("World warper", "Close the game first.")
            return
        paths = warper_paths()
        progress = read_warper_progress(paths["state"])
        resume = False
        if paths["state"].is_file() and not progress["finished"] and (progress["done"] or progress["skipped"]):
            answer = messagebox.askyesnocancel(
                "World warper",
                f"A shader cache run stopped after {progress['done']} areas.\n\n"
                "Yes: continue it.\nNo: start over (a new copy of your save).")
            if answer is None:
                return
            resume = answer
        if not resume:
            if not messagebox.askokcancel(
                    "Build shader cache (world warper)",
                    "The game starts in a test mode that travels through every lamp and area by itself "
                    "(about 70 areas) and in each looks round, walks, uses the weapons and items and "
                    "fights a few enemies, so their shaders are compiled and cached.\n\n"
                    "- It takes a long time: about an hour. Leave the game window alone meanwhile.\n"
                    "- It plays on a COPY of your save folder (user\\world_warper\\savedata). Your own "
                    "saves are never changed.\n"
                    "- Stop cancels it; pressing this button again continues where it stopped.\n"
                    "- At the end the shader cache is exported as one .zip. It is made from the game's "
                    "shaders: share it only with people who own the game.\n\nStart?",
                    icon="warning"):
                return
        default = f"bloodborne_shader_cache_{datetime.now():%Y%m%d}.zip"
        target = filedialog.asksaveasfilename(title="Where to save the shader cache .zip at the end",
                                              defaultextension=".zip", initialfile=default,
                                              filetypes=[("Zip files", "*.zip")])
        if not target:
            return
        try:
            copied = prepare_warper_saves(paths, fresh=not resume)
        except FileNotFoundError:
            messagebox.showerror("World warper", "There is no save to copy yet (user\\savedata). Play past "
                                 "the opening first: the warper continues your save.")
            return
        except OSError as ex:
            messagebox.showerror("World warper", f"Could not copy the save folder:\n{ex}")
            return
        self.log(f"[WARPER] {'Continuing' if resume else 'Starting'} the shader cache run"
                 + (f" ({copied} save files copied to {paths['saves']})" if copied else "")
                 + f"; the .zip goes to {target}")
        self._warper_zip = Path(target)
        self._warper_restarts = 0
        self._warper_cancelled = False
        self.launch_game(warper=True)

    def warper_finished(self, rc: int):
        paths = warper_paths()
        progress = read_warper_progress(paths["state"])
        if progress["finished"]:
            try:
                count = export_shader_cache(self._warper_zip, shader_cache_root())
            except OSError as ex:
                self.log(f"[WARPER] Export failed: {ex}")
                messagebox.showerror("World warper", f"The run finished, but the export failed:\n{ex}")
                return
            self.log(f"[WARPER] Finished: {progress['done']} areas, {progress['skipped']} skipped, "
                     f"{progress['new_pipelines']} new pipelines; shader cache exported: {count} files -> "
                     f"{self._warper_zip}")
            messagebox.showinfo("World warper",
                                f"Done: {progress['done']} areas visited, {progress['new_pipelines']} new "
                                f"pipelines cached.\n\nThe shader cache was exported to:\n{self._warper_zip}\n\n"
                                "Share it only with people who own the game.")
            return
        if getattr(self, "_stop_requested", False):
            self.log(f"[WARPER] Stopped after {progress['done']} areas; press the button again to continue.")
            return
        self._warper_restarts += 1
        if self._warper_restarts > WARPER_MAX_RESTARTS:
            self.log(f"[WARPER] The game ended {WARPER_MAX_RESTARTS} times without finishing; stopped "
                     f"after {progress['done']} areas. Press the button again to continue.")
            return
        self.log(f"[WARPER] The game ended (code {rc}) after {progress['done']} areas; continuing "
                 f"(restart {self._warper_restarts} of {WARPER_MAX_RESTARTS}).")
        self.launch_game(warper=True)

    def save_support_bundle(self):
        """Advanced tab button: asks where, then writes the bundle in the background."""
        if support_bundle is None:
            messagebox.showerror("Support bundle", "scripts\\support_bundle.py is missing next to the launcher.")
            return
        default = support_bundle.default_zip_path(APP_DIR)
        path = filedialog.asksaveasfilename(title="Save support bundle", initialdir=str(default.parent),
                                            initialfile=default.name, defaultextension=".zip",
                                            filetypes=[("Zip files", "*.zip")])
        if path:
            self.write_support_bundle(Path(path), note="saved from the launcher")

    def write_support_bundle(self, path, note=""):
        """Writes the bundle on a thread (the GPU query and zipping take a few seconds); logs the result."""
        if support_bundle is None:
            self.log("[SUPPORT] scripts\\support_bundle.py is missing; no support bundle.")
            return
        target = Path(path) if path else support_bundle.default_zip_path(APP_DIR)
        settings = self.current_settings()
        self.log(f"[SUPPORT] Writing the support bundle {target} ...")

        def run():
            try:
                names = support_bundle.make_bundle(APP_DIR, target, settings=settings, note=note,
                                                   version=PORT_VERSION)
                self.log_queue.put(f"[SUPPORT] Support bundle saved: {target} ({len(names)} files). Send this "
                                   f"zip with your bug report; passwords, party codes and addresses are removed.")
            except Exception as ex:
                self.log_queue.put(f"[SUPPORT] Support bundle failed: {ex}")
        threading.Thread(target=run, daemon=True).start()

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
            # Close the game normally first (up to 8 s), then quit; the window stays responsive.
            self.stop_game()
            self._quit_wait = 0

            def wait_then_quit():
                self._quit_wait += 1
                if (self.proc is not None and self.proc.poll() is None) and self._quit_wait < 100:
                    self.after(100, wait_then_quit)
                    return
                if self.proc is not None and self.proc.poll() is None:
                    kill_tree(self.proc)
                self.save_settings()
                self.destroy()
            self.after(100, wait_then_quit)
            return
        self.save_settings()
        self.destroy()


if __name__ == "__main__":
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == "--write-env":
        write_env_bat(Path(sys.argv[2]))
    else:
        if not acquire_single_instance():
            tk.Tk().withdraw()
            messagebox.showinfo("Bloodborne PC Launcher", "The launcher is already open.")
            sys.exit(0)
        BloodborneLauncher().mainloop()
