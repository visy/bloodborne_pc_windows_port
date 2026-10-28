"""Iosefka's clinic: pure rules (no Discord, no database). Skip rule, file kinds, IP mask, clustering."""
import ipaddress
import re
from pathlib import Path

try:
    import netdoctor_store as _ns
except ImportError:  # loaded by path without tools/discord on sys.path
    import importlib.util
    _spec = importlib.util.spec_from_file_location("netdoctor_store", Path(__file__).with_name("netdoctor_store.py"))
    _ns = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_ns)

WINDOW_S = 1800
REMOVABLE_KINDS = {"log", "dump", "game", "save"}
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"}
VIDEO_EXT = {".mp4", ".mov", ".webm", ".mkv", ".avi", ".m4v"}
ARCHIVE_EXT = {".zip", ".7z", ".rar"}


def skipped_channel(name: str) -> bool:
    n = (name or "").lower()
    return n.startswith("staff") or n in ("development", "moderator-only")


# Bloodborne save files (PS4 / shadPS4 layout): userdata0000..0010, backup0000.., with suffixes such as
# `.bak`, `_name`, ` (2)`; sizes are 0x140000 (character slots) or 0x40000 (the system slot)
SAVE_NAME_RX = re.compile(r"^(?:userdata|backup)\d{4}(?:[ ._\-(].*)?$")
SAVE_DIRS = {"sprj0005", "savedata"}
SAVE_SIZES = (0x140000, 0x40000)


def file_kind(filename: str) -> str:
    """log / dump / game / save / archive / image / video / other. The folders in a path count (an archive
    member `SPRJ0005/x/param.sfo`), a bare name does not."""
    parts = [p for p in re.split(r"[\\/]", filename or "") if p]
    n = (parts[-1] if parts else "").lower()
    ext = Path(n).suffix
    if SAVE_NAME_RX.match(n):
        return "save"
    if any(p.lower() in SAVE_DIRS for p in parts[:-1]) and ext not in (".log", ".txt", ".dmp", ".dcx") \
            and ext not in ARCHIVE_EXT and (ext not in IMAGE_EXT | VIDEO_EXT or n == "icon0.png"):
        return "save"  # anything else in the save folder: param.sfo, icon0.png, odd copies
    if n.startswith("userdata") or ext == ".dcx":
        return "game"
    if ext in (".log", ".txt"):
        return "log"
    if ext == ".dmp":
        return "dump"
    if ext in ARCHIVE_EXT:
        return "archive"
    if ext in IMAGE_EXT:
        return "image"
    if ext in VIDEO_EXT:
        return "video"
    return "other"


def holds(attachments: list[dict]) -> str:
    """What stored attachments carry for the clinic's words: "log" (the default, also unreadable archives),
    "save" (Bloodborne save files only) or "both". Archives count by their member names (`inner`)."""
    logs = saves = False
    for a in attachments or []:
        kind = file_kind(a.get("filename") or "")
        if kind == "archive":
            names = a.get("inner")
            kinds = ["log"] if names is None else [file_kind(i) for i in names]
        else:
            kinds = [kind]
        saves = saves or "save" in kinds
        logs = logs or any(k in ("log", "dump", "game") for k in kinds)
    if saves and not logs:
        return "save"
    return "both" if saves else "log"


def removable(filename: str, inner_names: list[str] | None = None) -> bool:
    kind = file_kind(filename)
    if kind in REMOVABLE_KINDS:
        return True
    if kind == "archive":
        if inner_names is None:  # unreadable: assume the worst
            return True
        return any(file_kind(i) in REMOVABLE_KINDS for i in inner_names)
    return False


# --- IP mask: same rules as scripts/net_doctor.py redact(ips=True) ------------------------------------

_IP_RX = re.compile(r"\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b(?::\d{1,5}\b)?")
_V6_RX = re.compile(r"(?<![\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:])")
_VPN_NETS = [ipaddress.ip_network(n) for n in ("26.0.0.0/8", "25.0.0.0/8", "100.64.0.0/10")]


# documentation ranges are not "global" for ipaddress, but they stand in for public addresses (tests, examples)
_DOC_NETS = [ipaddress.ip_network(n) for n in ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24")]


def _is_public(ip: str) -> bool:
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False
    if any(a in n for n in _VPN_NETS):
        return False
    return a.is_global or any(a in n for n in _DOC_NETS)


def mask_ips(text: str) -> str:
    """Public IPv4 (with port) -> `<public IP>`; private and VPN (Radmin, Hamachi, Tailscale) IPv4 keep only
    the first octet (`192.x.x.x`) exactly as net_doctor does; global IPv6 -> `<public IP>`."""
    def v4(m):
        ip = ".".join(m.group(i) for i in range(1, 5))
        return "<public IP>" if _is_public(ip) else m.group(1) + ".x.x.x"

    def v6(m):
        try:
            a = ipaddress.ip_address(m.group(0))
        except ValueError:
            return m.group(0)
        return "<public IP>" if a.is_global else m.group(0)
    return _V6_RX.sub(v6, _IP_RX.sub(v4, text or ""))


# --- clustering -------------------------------------------------------------------------------------------

def cluster(msgs: list[dict], infos: dict[int, list[dict]]) -> list[list[int]]:
    """Group messages into reports. Seeds are messages with a log; same-author messages within +-1800 s,
    replies to cluster messages and other logs of the same session join. Oldest cluster first."""
    by_id = {m["id"]: m for m in msgs}
    parent = {m["id"]: m["id"] for m in msgs}

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(a, b):
        parent[find(a)] = find(b)

    seeds = [m for m in msgs if m.get("has_log")]
    for s in seeds:
        for m in msgs:
            if m["id"] != s["id"] and m["author_id"] == s["author_id"] \
                    and abs(m["created"] - s["created"]) <= WINDOW_S:
                union(s["id"], m["id"])
    for i, a in enumerate(seeds):
        for b in seeds[i + 1:]:
            ia, ib = infos.get(a["id"], []), infos.get(b["id"], [])
            if any(_ns.same_session(x, y) for x in ia for y in ib):
                union(a["id"], b["id"])
    changed = True
    while changed:  # replies, repeatedly: a reply to a reply to the report belongs to it
        changed = False
        seed_roots = {find(s["id"]) for s in seeds}
        for m in msgs:
            r = m.get("reply_to")
            if r in by_id and find(r) in seed_roots and find(m["id"]) != find(r):
                union(m["id"], r)
                changed = True
                seed_roots = {find(s["id"]) for s in seeds}
    seed_roots = {find(s["id"]) for s in seeds}
    groups: dict[int, list[dict]] = {}
    for m in msgs:
        if find(m["id"]) in seed_roots:
            groups.setdefault(find(m["id"]), []).append(m)
    out = [sorted(g, key=lambda m: (m["created"], m["id"])) for g in groups.values()]
    out.sort(key=lambda g: (g[0]["created"], g[0]["id"]))
    return [[m["id"] for m in g] for g in out]


def bug_for_shas(index: dict[str, set[str]], shas: list[str]) -> str | None:
    """The bug (id -> its file shas) that holds all of `shas`; mirrors Clinic.bug_for_files."""
    want = set(shas)
    if not want:
        return None
    for bug_id in sorted(index):
        if want <= set(index[bug_id]):
            return bug_id
    return None


def missing_peers(infos: list[dict]) -> list[str]:
    """Players named in these logs (peers, host) who have no log of their own here. Names stay private."""
    have = {i["name"] for i in infos if i.get("name")}
    named = set()
    for i in infos:
        named |= set(i.get("peers") or ())
        if i.get("host"):
            named.add(i["host"])
    return sorted(n for n in named if n and n not in have)


def missing_roles(infos: list[dict]) -> list[str]:
    """`host` and/or `guest`: which kind of log is missing, for the public reply."""
    missing = set(missing_peers(infos))
    hosts = {i["host"] for i in infos if i.get("host")}
    roles = []
    if missing & hosts:
        roles.append("host")
    if missing - hosts:
        roles.append("guest")
    return roles


# --- network or bug --------------------------------------------------------------------------------------

_NET_RX = re.compile(r"\b(join|joining|connect|connection|summon|can't see|cant see|timeout|timed out|"
                     r"firewall|vpn|port|nat|hamachi|radmin|zerotier)\b", re.I)
_BUG_RX = re.compile(r"\b(bug|crash|broken|glitch|stuck|softlock)\w*\b", re.I)
_NET_KEYS = ("net.", "nat.", "join.", "firewall.")


def classify(text: str, verdict_keys: set[str]) -> str:
    if any(k.startswith(_NET_KEYS) for k in verdict_keys):
        return "network"
    text = (text or "").replace("’", "'")
    if _NET_RX.search(text) and not _BUG_RX.search(text):
        return "network"
    return "bug"


def classify_unsure(text: str, verdict_keys: set[str]) -> bool:
    """True when `classify` was a guess: the text has both network and bug words, or net_doctor's verdict
    and the words point different ways."""
    text = (text or "").replace("’", "'")
    net, bug = bool(_NET_RX.search(text)), bool(_BUG_RX.search(text))
    key_net = any(k.startswith(_NET_KEYS) for k in verdict_keys)
    return (net and bug) or (key_net and bug and not net) or (net and not bug and not key_net)
