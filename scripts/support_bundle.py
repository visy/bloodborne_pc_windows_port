#!/usr/bin/env python3
"""Support bundle: one zip with what a bug report needs, secrets removed (docs/party/logging.md).

  python scripts/support_bundle.py [GAME_DIR] [-o ZIP] [--logs N]

Library (launcher.py "Save support bundle" and the automatic bundle after a crash):
  make_bundle(app_dir, zip_path, max_logs=10, settings=None, note="") -> list of names in the zip
  redact_text(text, secrets=()) -> text without passwords, party codes, addresses, tokens, user name

Taken: launcher.log, the newest logs\\launcher_*.log, out\\crash_*.log, out\\hang_dump*.log,
out\\device_lost_*.log and out\\gpu_stuck_*.log (GPU reports) with out\\hang_shader_*.bin / *.spv (the
stuck command's guest shader code and its SPIR-V, binary as written), out\\heartbeat*.log,
out\\summary.log, bbport.ini and launcher_settings.json (both redacted) and
info.txt (version, build, OS, CPU, GPU). Never taken: saves (user\\), eboot / game data, shader or
pipeline caches (only the stuck shaders above), the party password or code.
"""
import argparse
import ctypes
import json
import os
import platform
import re
import subprocess
import sys
import zipfile
from datetime import datetime
from pathlib import Path

REDACTED = "<redacted>"
MAX_FILE_BYTES = 40 * 1024 * 1024  # a longer log keeps its last 40 MB

# Party codes: BBP1 / BBP2 / BBP3 / BBPI / BBPL (and later versions) followed by letter-digit groups (launcher.py PARTY_CODE_RE).
_CODE_RE = re.compile(r"\bBB[PG][0-9A-Z](?:-[0-9A-Za-z]+)+\b", re.IGNORECASE)
# key = value / key: value / "key": "value" for secret keys (ini, env dumps, JSON, log lines).
_SECRET_WORDS = r"password|passwd|pwd|passphrase|token|secret|party_code|guest_code|lan_code|public_addr"
_KV_RE = re.compile(r"(?P<key>\"?(?<![A-Za-z0-9])[A-Za-z0-9_.]*(?:" + _SECRET_WORDS + r")[A-Za-z0-9_]*\"?\s*[=:]\s*)"
                    r"(?:\"(?P<qval>[^\"]*)\"|(?P<val>[^\"\s,;}]*))", re.IGNORECASE)
_IPV4_RE = re.compile(r"(?<![\w.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\w.]*\d)")
_IPV6_RE = re.compile(r"(?<![\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:])")
_USER_RE = re.compile(r"(?i)([A-Z]:[\\/]+Users[\\/]+)([^\\/\s\"']+)")
_EMAIL_RE = re.compile(r"\b[\w.+-]+@[\w-]+\.[\w.-]+\b")


def _private_v4(o):
    return o[0] == 10 or (o[0] == 172 and 16 <= o[1] <= 31) or (o[0] == 192 and o[1] == 168) or \
        (o[0] == 100 and 64 <= o[1] <= 127) or (o[0] == 169 and o[1] == 254)


class Redactor:
    """Stable pseudonyms per bundle: the same address is "ip-1" in every file, so the host's and a
    guest's lines can still be told apart."""

    def __init__(self, secrets=()):
        self.secrets = sorted({s for s in secrets if s and len(s) >= 3}, key=len, reverse=True)
        self.ips = {}

    def _ip_name(self, ip, kind):
        if ip not in self.ips:
            self.ips[ip] = f"{kind}-{len(self.ips) + 1}"
        return self.ips[ip]

    def _v4(self, m):
        o = [int(g) for g in m.groups()]
        if any(x > 255 for x in o):
            return m.group(0)
        ip = m.group(0)
        if o[0] == 127 or o == [0, 0, 0, 0] or o == [255, 255, 255, 255]:
            return ip  # loopback / any / broadcast: no one's address
        return "<" + self._ip_name(ip, "lan-ip" if _private_v4(o) else "ip") + ">"

    def _v6(self, m):
        s = m.group(0)
        groups = [g for g in s.split(":") if g]
        # An IPv6 address has all 8 groups or "::" (compressed). Clock times (19:49:49), EMEVD ids
        # (7600:2005:3) and C++ scopes (a::b) are not addresses.
        if not (len(groups) == 8 or ("::" in s and len(groups) >= 2)):
            return s
        if not all(re.fullmatch(r"[0-9A-Fa-f]{1,4}", g) for g in groups):
            return s
        return "<" + self._ip_name(s.lower(), "ip6") + ">"

    @staticmethod
    def _kv(m):
        if m.group("qval") is not None:
            return m.group("key") + '"' + (REDACTED if m.group("qval") else "") + '"'
        return m.group("key") + (REDACTED if m.group("val") else "")

    def text(self, text):
        for s in self.secrets:
            text = text.replace(s, REDACTED)
        text = _CODE_RE.sub("<party code>", text)
        text = _KV_RE.sub(self._kv, text)
        text = _IPV4_RE.sub(self._v4, text)
        text = _IPV6_RE.sub(self._v6, text)
        text = _USER_RE.sub(lambda m: m.group(1) + "<user>", text)
        text = _EMAIL_RE.sub("<email>", text)
        return text


def redact_text(text, secrets=()):
    return Redactor(secrets).text(text)


SETTINGS_DROP = ("party_password", "party_code", "party_public_addr", "party_guest_codes", "party_lan_code")


def redact_settings(data):
    """launcher_settings.json without its secrets (the keys stay, so the report shows they were set)."""
    out = {}
    for k, v in data.items():
        if k in SETTINGS_DROP or re.search(r"password|token|secret", k, re.IGNORECASE):
            out[k] = REDACTED if v else v
        else:
            out[k] = v
    return out


def _read_tail(path: Path) -> str:
    size = path.stat().st_size
    with open(path, "rb") as f:
        if size > MAX_FILE_BYTES:
            f.seek(size - MAX_FILE_BYTES)
            head = f"[support bundle: first {size - MAX_FILE_BYTES} bytes left out]\n"
        else:
            head = ""
        return head + f.read().decode("utf-8", errors="replace")


def _newest(paths, n):
    files = [p for p in paths if p.is_file()]
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return files[:n]


def collect_files(app_dir: Path, max_logs=10):
    """(path, name in the zip) of every file a bundle takes."""
    app_dir = Path(app_dir)
    out_dir = app_dir / "out"
    picks = []
    if (app_dir / "launcher.log").is_file():
        picks.append((app_dir / "launcher.log", "launcher.log"))
    for p in _newest((app_dir / "logs").glob("launcher_*.log"), max_logs):
        picks.append((p, f"logs/{p.name}"))
    for p in _newest(out_dir.glob("crash_*.log"), max_logs):
        picks.append((p, f"out/{p.name}"))
    for p in _newest(out_dir.glob("hang_dump*.log"), 3):
        picks.append((p, f"out/{p.name}"))
    # GPU device lost / stuck GPU (vk_breadcrumbs.cpp): the report and the guest code of the
    # shaders of the command the GPU did not finish.
    for pattern in ("device_lost_*.log", "gpu_stuck_*.log"):
        for p in _newest(out_dir.glob(pattern), 3):
            picks.append((p, f"out/{p.name}"))
    for p in _newest(list(out_dir.glob("hang_shader_*.bin")) + list(out_dir.glob("hang_shader_*.spv")), 8):
        picks.append((p, f"out/{p.name}"))
    for p in _newest(out_dir.glob("heartbeat*.log"), 3):
        picks.append((p, f"out/{p.name}"))
    if (out_dir / "summary.log").is_file():
        picks.append((out_dir / "summary.log", "out/summary.log"))
    return picks


def _build_id(app_dir: Path):
    for log in [app_dir / "launcher.log"] + _newest((app_dir / "logs").glob("launcher_*.log"), 5):
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.startswith("[coop] "):
                        line = line[len("[coop] "):]
                    if line.startswith("Party: build "):
                        return line.strip()[len("Party: build "):]
        except OSError:
            pass
    try:
        r = subprocess.run(["git", "-C", str(app_dir), "describe", "--always", "--dirty", "--abbrev=10"],
                           capture_output=True, text=True, timeout=5,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if r.returncode == 0 and r.stdout.strip():
            return "git " + r.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown"


def _cpu_info():
    name = platform.processor() or "unknown"
    flags = []
    if os.name == "nt":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as k:
                name = winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
        except OSError:
            pass
        try:
            present = ctypes.windll.kernel32.IsProcessorFeaturePresent
            for pf, label in ((36, "SSE4.1"), (37, "SSE4.2"), (39, "AVX"), (40, "AVX2"), (41, "AVX-512F")):
                if present(pf):
                    flags.append(label)
        except (AttributeError, OSError):
            pass
    return name, flags


def _gpu_info(app_dir: Path):
    if os.name != "nt":
        return []
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name + ' (driver ' + "
                            "$_.DriverVersion + ')' }"],
                           capture_output=True, text=True, timeout=15,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return [line.strip() for line in r.stdout.splitlines() if line.strip()]
    except (OSError, subprocess.SubprocessError):
        return []


def info_text(app_dir: Path, version="", note="", settings=None):
    cpu, flags = _cpu_info()
    lines = [
        "bbport support bundle",
        f"created: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
        f"launcher version: {version or 'unknown'}",
        f"build: {_build_id(Path(app_dir))}",
        f"os: {platform.platform()}",
        f"python: {sys.version.split()[0]}",
        f"cpu: {cpu}",
        f"cpu features: {', '.join(flags) if flags else 'unknown'}",
    ]
    for g in _gpu_info(Path(app_dir)) or ["unknown"]:
        lines.append(f"gpu: {g}")
    if settings:
        lines.append(f"party mode: {settings.get('party_mode') or 'off'}; party logging: "
                     f"{settings.get('party_log_level') or 'normal'} "
                     f"({', '.join(settings.get('party_log_areas') or []) or 'all areas'})")
    if note:
        lines.append(f"note: {note}")
    lines.append("")
    lines.append("Redacted: passwords, party codes, tokens, IP addresses (ip-N / lan-ip-N: the same number is the "
                 "same address in every file), the Windows user name, e-mail addresses. Not included: saves, game "
                 "files, shader caches.")
    return "\n".join(lines) + "\n"


def make_bundle(app_dir, zip_path, max_logs=10, settings=None, note="", version=""):
    """Writes the bundle; returns the names in the zip."""
    app_dir = Path(app_dir)
    zip_path = Path(zip_path)
    secrets = []
    if settings:
        for key in ("party_password", "party_code", "party_public_addr"):
            v = str(settings.get(key) or "").strip()
            if v:
                secrets.append(v)
    red = Redactor(secrets)
    names = []
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path, name in collect_files(app_dir, max_logs):
            try:
                if name.endswith((".bin", ".spv")):  # shader code: binary, nothing to redact
                    z.writestr(name, path.read_bytes()[:(64 if name.endswith(".bin") else 1024) * 1024])
                    names.append(name)
                    continue
                z.writestr(name, red.text(_read_tail(path)))
                names.append(name)
            except OSError:
                pass
        ini = app_dir / "bbport.ini"
        if ini.is_file():
            try:
                z.writestr("bbport.ini", red.text(ini.read_text(encoding="utf-8", errors="replace")))
                names.append("bbport.ini")
            except OSError:
                pass
        sfile = app_dir / "launcher_settings.json"
        data = settings
        if data is None and sfile.is_file():
            try:
                data = json.loads(sfile.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                data = None
        if isinstance(data, dict):
            z.writestr("launcher_settings.json", red.text(json.dumps(redact_settings(data), indent=2)))
            names.append("launcher_settings.json")
        z.writestr("info.txt", red.text(info_text(app_dir, version, note, data if isinstance(data, dict) else None)))
        names.append("info.txt")
    return names


def default_zip_path(app_dir, tag="support"):
    return Path(app_dir) / "logs" / f"{tag}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.zip"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_dir", nargs="?", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("-o", "--output")
    ap.add_argument("--logs", type=int, default=10)
    a = ap.parse_args()
    out = Path(a.output) if a.output else default_zip_path(a.game_dir)
    names = make_bundle(a.game_dir, out, a.logs)
    print(f"{out}: {len(names)} files ({', '.join(names)})")


if __name__ == "__main__":
    main()
