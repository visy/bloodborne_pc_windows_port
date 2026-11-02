#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""nwlobby - the Nightwatch VPN game lobby (UDP 9310 on the VPN server's tunnel address).

A hosting game announces itself every ~10 s; a guest asks for the list and joins from it. LAN
discovery (UDP 9308 broadcast) does not cross an OpenVPN tun, hence this tiny service.

Everything is in memory: nothing is written to disk, no address is ever logged. Only senders from
the VPN network are answered (default 10.8.0.0/22, minus the network, server and broadcast
addresses). OpenVPN drops packets whose source is not the client's own tunnel address, so the
source address is the host's real tunnel address; the entry is that address, never anything the
packet claims.

Wire format (version 1, big endian, at most 512 bytes in, 1024 out):
  every packet   0xfa 'L' ver(1) type
  type 1 announce  flags u8 (bit0 password), port u16, players u8, max u8,
                   build: u8 len + ASCII (<= 32), name: u8 len + UTF-8 (<= 48 bytes)
  type 2 list      offset u16                      (page start in the sorted list)
  type 3 reply     total u16, offset u16, count u8, then count entries:
                   ipv4 4, port u16, flags u8 (bit0 password, bit1 this is your own entry), players u8, max u8,
                   name: u8 len + UTF-8, build: u8 len + ASCII
  type 4 bye       (no body) removes the sender's entry
  type 5 stats     (no body) only on the operator socket (127.0.0.1:9311, never the tunnel)
  type 6 stats reply  UTF-8 JSON (<= 1024 bytes): games, players (sum of the hosts' player counts), open (no
                   password), full (players >= max), joinable (open and not full), free_slots (in open games),
                   sizes {"<max>": games}, builds {"<version>": games} (top 5, first word of the build, <= 32 chars):
                   counts only, no names or addresses
An announce is not answered; a list request is answered with one reply page. Anything malformed is
dropped without an answer. The client UI shows names, flags and counts, never the address.
"""
import ipaddress
import json
import os
import re
import select
import socket
import sys
import time
import unicodedata

MAGIC = b"\xfaL"
VERSION = 1
T_ANNOUNCE, T_LIST, T_REPLY, T_BYE, T_STATS, T_STATS_REPLY = 1, 2, 3, 4, 5, 6
F_PASSWORD, F_SELF = 1, 2

MAX_PACKET = 512            # a longer datagram is dropped unread
MAX_REPLY = 1024
MAX_PAGE_ENTRIES = 16
MAX_ENTRIES = 300
TTL = 30.0                  # seconds an entry lives without a new announce
NAME_MAX_CHARS = 16
NAME_MAX_BYTES = 48
BUILD_MAX = 32
RATE_PER_SEC = 2.0          # per source address, token bucket
RATE_BURST = 8.0
DEFAULT_NET = "10.8.0.0/22"   # 0.7.8: room for 1021 players (the server pool may still be /24)
DEFAULT_BIND = "10.8.0.1"
DEFAULT_PORT = 9310
DEFAULT_STATS = "127.0.0.1:9311"   # operator counts (nwstats); "off" disables
NAME_FILTER = os.environ.get("NWLOBBY_NAME_FILTER", "1") != "0"   # mask offensive host names


def clean_name(raw: bytes):
    """A display name from UTF-8 bytes: control, format, separator and surrogate characters stripped
    (so no line breaks, no bidi tricks, no zero-width padding), spaces folded, at most NAME_MAX_CHARS
    characters; None when it is empty or not UTF-8."""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return None
    out = []
    for ch in text:
        cat = unicodedata.category(ch)
        if ch == " ":
            out.append(ch)
        elif cat[0] in "CZ" or cat in ("Mn", "Me"):
            continue
        else:
            out.append(ch)
    name = " ".join("".join(out).split())[:NAME_MAX_CHARS].strip()
    return name or None


# ---- offensive names (interim lobby filter, 0.7.8) -------------------------------------------------
# A host whose name is offensive still gets listed, under a neutral masked name. Matching works on a
# normalised "skeleton": NFKC + casefold, marks and invisible characters dropped, look-alike letters
# (Cyrillic, Greek, small caps, regional indicators, ...) and leetspeak mapped to Latin. Three rules,
# chosen to keep false positives low (Bloodborne names like "Black Hunter" or "Vileblood" must pass):
#   1. slurs: whole words, plus inside a run of letters for long unambiguous ones;
#   2. a protected group word together with a violent word ("... people hunter");
#   3. known hate codes.
_CONFUSABLE = str.maketrans({
    # Cyrillic
    "\u0430": "a", "\u0432": "b", "\u0435": "e", "\u0451": "e", "\u0437": "3", "\u0438": "u", "\u0439": "u", "\u043a": "k", "\u043c": "m", "\u043d": "h",
    "\u043e": "o", "\u043f": "n", "\u0440": "p", "\u0441": "c", "\u0442": "t", "\u0443": "y", "\u0445": "x", "\u044c": "b", "\u0455": "s", "\u0456": "i",
    "\u0457": "i", "\u0458": "j", "\u0501": "d", "\u051b": "q", "\u051d": "w", "\u04cf": "l", "\u04bb": "h", "\u0261": "g",
    # Greek
    "\u03b1": "a", "\u03b2": "b", "\u03b3": "y", "\u03b4": "d", "\u03b5": "e", "\u03b6": "z", "\u03b7": "n", "\u03b9": "i", "\u03ba": "k", "\u03bb": "l",
    "\u03bc": "u", "\u03bd": "v", "\u03bf": "o", "\u03c0": "n", "\u03c1": "p", "\u03c3": "o", "\u03c2": "s", "\u03c4": "t", "\u03c5": "u", "\u03c6": "o",
    "\u03c7": "x", "\u03c9": "w",
    # Latin look-alikes NFKC keeps
    "\u0142": "l", "\u00f8": "o", "\u0111": "d", "\u0127": "h", "\u0131": "i", "\u0140": "l", "\u0192": "f", "\u0251": "a", "\u0269": "i", "\u028b": "u",
    "\u00df": "ss", "\u00e6": "ae", "\u0153": "oe", "\u00fe": "p",
    # small capitals
    "\u1d00": "a", "\u0299": "b", "\u1d04": "c", "\u1d05": "d", "\u1d07": "e", "\ua730": "f", "\u0262": "g", "\u029c": "h", "\u026a": "i", "\u1d0a": "j",
    "\u1d0b": "k", "\u029f": "l", "\u1d0d": "m", "\u0274": "n", "\u1d0f": "o", "\u1d18": "p", "\u0280": "r", "\ua731": "s", "\u1d1b": "t", "\u1d1c": "u",
    "\u1d20": "v", "\u1d21": "w", "\u028f": "y", "\u1d22": "z",
    # upside-down letters
    "\u0250": "a", "\u0254": "c", "\u01dd": "e", "\u025f": "f", "\u0183": "g", "\u0265": "h", "\u1d09": "i", "\u027e": "j", "\u029e": "k", "\u026f": "m",
    "\u0279": "r", "\u0287": "t", "\u028c": "v", "\u028d": "w", "\u028e": "y",
})
_CONFUSABLE.update({0x1F1E6 + i: chr(ord("a") + i) for i in range(26)})   # regional indicator letters
_LEET = {"0": "o", "1": "i", "2": "z", "3": "e", "4": "a", "5": "s", "6": "g", "7": "t", "8": "b", "9": "g",
         "@": "a", "$": "s", "!": "i", "|": "l", "+": "t", "\u20ac": "e", "\u00a3": "l", "\u00a5": "y"}
_LEET_I = str.maketrans(_LEET)
_LEET_L = str.maketrans({**_LEET, "1": "l", "!": "l"})

# Slurs as regular expressions over the name's letters (separators removed, so "n.i g-g" reads as one
# run). "+" lets a letter repeat ("niiigggerr"); patterns that need a doubled letter keep real words out
# (Niger the country has one g). WORD patterns must be a whole word of the name, or the whole name;
# RUN patterns may sit inside a longer run of letters (long and unambiguous only).
_SLUR_RUN = [re.compile(p) for p in (
    r"n+[iy]+g{2,}(e+r+|a+h?|u+h?|e+r+s+|a+z+|a+s+|l+e+t+)", r"n+e+g{2,}e+r+",
    r"f+a+g{2,}(o+t+|i+t+|e+t+)", r"t+r+a+n{2,}(y+|i+e+)", r"j+i+g+a+b+o+o+", r"p+o+r+c+h+m+o+n+k+e+y+",
    r"w+e+t+b+a+c+k+", r"z+i+p+e+r+h+e+a+d+", r"s+l+a+n+t+e+y+e+", r"r+a+g+h+e+a+d+", r"t+o+w+e+l+h+e+a+d+",
    r"s+a+n+d+n+i+g+", r"c+a+m+e+l+f+u+c+k+", r"s+h+e+m+a+l+e+", r"g+o+l+i+w+o+g+", r"n+e+g+r+a+t+a+",
    r"m+a+r+i+c+o+n+", r"s+c+h+w+u+c+h+t+e+l+", r"b+o+u+g+n+o+u+l+e+", r"r+i+c+h+i+o+n+e+",
    r"p+i+d+[oa]+r+a+s+", r"c+h+e+r+n+o+z+h+o+p+", r"n+e+e+k+e+r+i+", r"k+i+k+e+s+", r"b+e+a+n+e+r+s+",
    r"m+o+n+g+o+l+o+i+d+", r"s+p+i+c+s+", r"c+h+i+n+k+s+",
)]
# Real words that contain a RUN slur; a hit inside one of these does not count.
_RUN_ALLOW = ("snigger", "sniggers", "shemaledict")
_SLUR_WORD = [re.compile(p) for p in (
    r"n+i+g+(a+h?|a+[sz]+|u+r+|r+s?|e+r+s+|l+e+t+)", r"n+y+g+a+", r"k+[iy]+k+e+s?", r"s+p+i+c+k?s?",
    r"b+e+a+n+e+r+s?", r"c+h+i+n+k+s?", r"g+o+o+k+s?", r"y+i+d+s?", r"h+e+b+e+s?", r"p+a+k+i+s?",
    r"f+a+g+s?", r"f+a+g+o+t+s?", r"d+y+k+e+s", r"t+r+a+n+y+s?", r"r+e+t+a+r+d+s?", r"c+o+o+n+s",
    r"w+o+g+s?", r"g+y+p+o+", r"p+i+k+e+y+", r"s+a+m+b+o+", r"n+e+g+r+o+i+d+", r"s+u+d+a+c+a+",
    r"v+i+a+d+o+", r"n+e+g+e+r+", r"k+a+n+a+k+e+n?", r"t+a+p+e+t+e+", r"f+r+o+c+i+o+", r"p+i+d+o+r+",
    r"p+e+d+i+k+", r"z+h+i+d+y?", r"c+i+a+p+a+t+y+", r"i+b+n+e+", r"m+a+r+i+c+o+n+e+s?",
)]
# Slurs only when they are the whole name (inside a phrase they are often ordinary Spanish/French/Latin
# words or a surname): "Negro" alone is masked, "Lobo Negro" is fine.
_ALONE_ONLY = [re.compile(p) for p in (r"n+e+g+r+o+s?", r"n+e+g+r+e+", r"h+o+m+o+", r"p+u+t+o+",
                                        r"p+e+d+e+", r"c+o+o+n+", r"t+a+r+d+", r"d+y+k+e+")]

# Plural nouns name a group as a target; singular adjectives ("Chinese Hunter") usually describe the
# player, so they count only with a strong violent word.
_GROUP_WORDS = {
    "blacks", "negros", "negroes", "africans", "jew", "jews", "jewish", "jewry", "muslim", "muslims",
    "moslem", "moslems", "islam", "islamic", "arab", "arabs", "asian", "asians", "chinese", "japs", "jap",
    "mexican", "mexicans", "latino", "latinos", "immigrant", "immigrants", "refugee", "refugees",
    "migrant", "migrants", "gay", "gays", "lesbian", "lesbians", "homosexual", "homosexuals", "homos",
    "queer", "queers", "trans", "transgender", "transgenders", "gypsy", "gypsies", "hindu", "hindus",
    "sikh", "sikhs", "indians", "pakistani", "pakistanis", "christians", "catholics", "whites",
    "minorities", "disabled", "autists", "autistic", "women", "females",
}
_GROUP_ADJ = {"black", "white", "brown", "yellow", "jewish", "muslim", "gay", "trans", "asian", "arab",
              "african", "mexican", "chinese", "indian", "disabled", "negro"}
_PEOPLE = {"ppl", "pple", "people", "peoples", "peeps", "folk", "folks", "person", "persons", "guys",
           "kids", "babies", "women", "men", "girls", "boys", "race", "races", "skin", "skins", "ones"}
_VIOLENT = {
    "hunter", "hunters", "hunting", "hunt", "huntr", "killer", "killers", "kill", "kills", "killing",
    "slayer", "slayers", "slay", "slaying", "hater", "haters", "hate", "hates", "hating", "exterminator",
    "exterminate", "extermination", "gas", "gasser", "gassing", "lynch", "lyncher", "lynching", "burner",
    "burn", "burning", "oven", "ovens", "genocide", "cleanser", "cleansing", "remover", "removal", "die",
    "death", "murder", "murderer", "shooter", "shoot", "beater", "basher", "bash", "hanger", "hang",
    "destroyer", "eraser", "purge", "purger", "deport", "deporter", "enslaver", "slaver", "owner", "eater",
}
# Weak words are ordinary Bloodborne words ("Hunter", "Slayer"); they target only a plural group.
_VIOLENT_WEAK = {"hunter", "hunters", "hunting", "hunt", "huntr", "slayer", "slayers", "slay", "slaying",
                 "owner", "eater", "beater", "destroyer", "eraser", "burner", "burn", "burning", "hanger",
                 "shooter", "shoot", "basher", "bash"}
_GROUP_SINGULAR = {"jew", "jewish", "muslim", "moslem", "islam", "islamic", "arab", "asian", "chinese",
                   "mexican", "latino", "immigrant", "refugee", "migrant", "gay", "lesbian", "homosexual",
                   "queer", "trans", "transgender", "gypsy", "hindu", "sikh", "pakistani", "disabled",
                   "autistic", "jap"}
_VIOLENT_RUN = re.compile(r"hunt|kill|slay|hate|extermin|gas+er|gas+ing|lynch|genocid|cleans|"
                          r"remov|murder|shoot|bash|deport|enslav|purg|burn")
_GROUP_RUN = re.compile(r"blackp+e*o*p+l+e*|blackfolk|blackpers|whitep+e*o*p+l+e*|brownp+e*o*p+l+e*|"
                        r"jews|jewish|muslims|mexicans|asians|immigrants|refugees|negros")
# Hate codes, on the letters-and-digits run (separators removed, no leetspeak).
_HATE_CODES = ("1488", "14words", "fourteenwords", "siegheil", "sieghail", "heilhitler", "hailhitler",
               "whitepower", "whitepride", "whitegenocide", "racewar", "gasthejews", "killalljews",
               "holohoax", "rahowa", "zyklon", "gaschamber", "6mwe", "hitlerdidnothingwrong",
               "hitlerwasright", "adolfhitler", "kukluxklan", "ssdeathshead")
_HATE_WORDS = {"hitler", "heil", "nazi", "nazis", "kkk", "rahowa", "zog", "goyim", "kukluxklan"}
_HATE_CHARS = "\u5350\u0fd5\u0fd6\u0fd7\u0fd8"   # \u534d alone stays allowed (a Buddhist symbol in East Asian names)


def _skeleton(text: str) -> str:
    """Lower-case Latin/digit skeleton of a name: look-alikes, marks and invisible characters resolved."""
    t = unicodedata.normalize("NFKC", text).casefold()
    t = "".join(ch for ch in unicodedata.normalize("NFD", t)
                if unicodedata.category(ch) not in ("Mn", "Me", "Cf", "Cc", "Co", "Cs", "Cn"))
    return unicodedata.normalize("NFKC", t).translate(_CONFUSABLE)


def _letters_words(text: str):
    return "".join(c if "a" <= c <= "z" else " " for c in text).split()


def _run_hit(run: str) -> bool:
    for rx in _SLUR_RUN:
        for m in rx.finditer(run):
            if not any(a in run and run.find(a) <= m.start() < run.find(a) + len(a) for a in _RUN_ALLOW):
                return True
    return False


def name_offensive(name: str) -> bool:
    """True when a lobby name is a slur, targets a protected group with violence, or carries a hate code."""
    if not name:
        return False
    if any(c in name for c in _HATE_CHARS):
        return True
    if any(c in name for c in "\u202e\u2067\u200f") and name_offensive(
            "".join(c for c in name if c not in "\u202e\u2067\u200f\u202c\u2069")[::-1]):
        return True   # a right-to-left override shows the name reversed
    base = _skeleton(name)
    alnum_words = "".join(c if c.isalnum() else " " for c in base).split()
    alnum_run = "".join(alnum_words)
    if any(code in alnum_run for code in _HATE_CODES) or set(alnum_words) & _HATE_WORDS:
        return True
    if "88" in alnum_words and set(alnum_words) & {"14", "heil", "hail", "sieg", "ss", "hitler", "adolf"}:
        return True
    for table in (_LEET_I, _LEET_L):
        words = _letters_words(base.translate(table))
        if not words:
            continue
        run = "".join(words)
        if set(words) & _HATE_WORDS:
            return True
        # 1. slurs
        if any(rx.fullmatch(w) for rx in _SLUR_WORD for w in words + [run]):
            return True
        if _run_hit(run) or any(rx.fullmatch(run) for rx in _ALONE_ONLY):
            return True
        # 2. a protected group with a violent word
        ws = set(words)
        plural = (bool(ws & (_GROUP_WORDS - _GROUP_SINGULAR))
                  or any(a in _GROUP_ADJ and b in _PEOPLE for a, b in zip(words, words[1:]))
                  or _GROUP_RUN.search(run) is not None)
        singular = bool(ws & _GROUP_SINGULAR)
        violent = bool(ws & _VIOLENT) or (_VIOLENT_RUN.search(run) is not None and len(words) <= 2)
        strong = bool(ws & (_VIOLENT - _VIOLENT_WEAK))
        if (plural and violent) or (singular and strong):
            return True
    return False


def masked_name(name: str) -> str:
    """The neutral name shown instead of an offensive one: stable per name, short, ASCII."""
    import hashlib
    return "Hunter " + hashlib.sha256(name.encode("utf-8")).hexdigest()[:3].upper()


def clean_build(raw: bytes):
    if len(raw) > BUILD_MAX or any(c < 0x20 or c >= 0x7F for c in raw):
        return None
    return raw.decode("ascii")


def parse_announce(body: bytes):
    """(flags, port, players, max, build, name) or None."""
    if len(body) < 6:
        return None
    flags, port, players, maxp = body[0], (body[1] << 8) | body[2], body[3], body[4]
    if flags & ~F_PASSWORD or port < 1024 or maxp < 1 or maxp > 8 or players > maxp:
        return None
    pos = 5
    n = body[pos]
    pos += 1
    if n > BUILD_MAX or pos + n >= len(body):
        return None
    build = clean_build(body[pos:pos + n])
    pos += n
    m = body[pos]
    pos += 1
    if build is None or m == 0 or m > NAME_MAX_BYTES or pos + m != len(body):
        return None
    name = clean_name(body[pos:pos + m])
    if name is None:
        return None
    return flags, port, players, maxp, build, name


def encode_announce(name: str, port: int, players: int, maxp: int, build: str, password: bool) -> bytes:
    nb = name.encode("utf-8")[:NAME_MAX_BYTES]
    bb = build.encode("ascii", "replace")[:BUILD_MAX]
    return (MAGIC + bytes([VERSION, T_ANNOUNCE, F_PASSWORD if password else 0, port >> 8, port & 255,
                           players, maxp, len(bb)]) + bb + bytes([len(nb)]) + nb)


def encode_list(offset: int = 0) -> bytes:
    return MAGIC + bytes([VERSION, T_LIST, (offset >> 8) & 255, offset & 255])


def encode_bye() -> bytes:
    return MAGIC + bytes([VERSION, T_BYE])


def encode_stats() -> bytes:
    return MAGIC + bytes([VERSION, T_STATS])


def parse_stats(data: bytes):
    """The stats dict or None."""
    if len(data) < 5 or data[:2] != MAGIC or data[2] != VERSION or data[3] != T_STATS_REPLY:
        return None
    try:
        d = json.loads(data[4:].decode("utf-8"))
    except ValueError:
        return None
    return d if isinstance(d, dict) else None


def parse_reply(data: bytes):
    """(total, offset, [entry dict]) or None. Entry: ip (str), port, password, own, players, max,
    name, build. Strict (this is the reference parser the clients are tested against)."""
    if len(data) < 9 or data[:2] != MAGIC or data[2] != VERSION or data[3] != T_REPLY:
        return None
    total, offset, count = (data[4] << 8) | data[5], (data[6] << 8) | data[7], data[8]
    pos = 9
    out = []
    for _ in range(count):
        if pos + 10 > len(data):
            return None
        ip = ".".join(str(b) for b in data[pos:pos + 4])
        port = (data[pos + 4] << 8) | data[pos + 5]
        flags, players, maxp = data[pos + 6], data[pos + 7], data[pos + 8]
        n = data[pos + 9]
        pos += 10
        if pos + n + 1 > len(data):
            return None
        name = data[pos:pos + n].decode("utf-8", "replace")
        pos += n
        b = data[pos]
        pos += 1
        if pos + b > len(data):
            return None
        build = data[pos:pos + b].decode("ascii", "replace")
        pos += b
        out.append({"ip": ip, "port": port, "password": bool(flags & F_PASSWORD), "own": bool(flags & F_SELF),
                    "players": players, "max": maxp, "name": name, "build": build})
    if pos != len(data):
        return None
    return total, offset, out


class Lobby:
    """The whole state: {source ip string: entry}. handle() is the only entry point; it never raises."""

    def __init__(self, net=DEFAULT_NET, ttl=TTL, max_entries=MAX_ENTRIES, reserve_server=True):
        self.net = ipaddress.ip_network(net)
        self.reserve_server = reserve_server
        self.ttl = ttl
        self.max_entries = max_entries
        self.entries = {}   # ip -> (expiry, flags, port, players, max, build, name)
        self.buckets = {}   # ip -> (tokens, last)

    def allowed(self, ip: str) -> bool:
        try:
            a = ipaddress.ip_address(ip)
        except ValueError:
            return False
        if a not in self.net or a == self.net.network_address or a == self.net.broadcast_address:
            return False
        # the server's own address (.1 of the network) is never a player
        return not (self.reserve_server and int(a) == int(self.net.network_address) + 1)

    def _take(self, ip: str, now: float) -> bool:
        tokens, last = self.buckets.get(ip, (RATE_BURST, now))
        tokens = min(RATE_BURST, tokens + (now - last) * RATE_PER_SEC)
        if tokens < 1.0:
            self.buckets[ip] = (tokens, now)
            return False
        self.buckets[ip] = (tokens - 1.0, now)
        return True

    def expire(self, now: float):
        for ip in [k for k, e in self.entries.items() if e[0] <= now]:
            del self.entries[ip]
        # forget idle rate buckets (full again anyway)
        if len(self.buckets) > 2 * self.max_entries:
            self.buckets = {k: v for k, v in self.buckets.items() if now - v[1] < 10.0}

    def handle(self, data: bytes, ip: str, now: float):
        """The reply bytes for one datagram, or None."""
        try:
            return self._handle(data, ip, now)
        except Exception:  # noqa: BLE001 - a bad packet must never stop the service
            return None

    def _handle(self, data: bytes, ip: str, now: float):
        if len(data) > MAX_PACKET or len(data) < 4 or data[:2] != MAGIC or data[2] != VERSION:
            return None
        if not self.allowed(ip) or not self._take(ip, now):
            return None
        kind, body = data[3], data[4:]
        if kind == T_ANNOUNCE:
            p = parse_announce(body)
            if p is None:
                return None
            self.expire(now)
            if ip not in self.entries and len(self.entries) >= self.max_entries:
                return None
            flags, port, players, maxp, build, name = p
            if NAME_FILTER and name_offensive(name):
                name = masked_name(name)   # still listed, under a neutral name
            self.entries[ip] = (now + self.ttl, flags, port, players, maxp, build, name)
            return None
        if kind == T_BYE:
            if len(body) == 0:
                self.entries.pop(ip, None)
            return None
        if kind == T_LIST:
            if len(body) != 2:
                return None
            return self.page((body[0] << 8) | body[1], ip, now)
        return None

    def stats(self, data: bytes, now: float):
        """The stats reply for an operator-socket datagram, or None. Counts only."""
        if data != encode_stats():
            return None
        self.expire(now)
        rows = list(self.entries.values())   # (expiry, flags, port, players, max, build, name)
        open_rows = [e for e in rows if not e[1] & F_PASSWORD]
        sizes, builds = {}, {}
        for e in rows:
            sizes[str(e[4])] = sizes.get(str(e[4]), 0) + 1
            ver = (e[5].split() or ["?"])[0][:BUILD_MAX]
            builds[ver] = builds.get(ver, 0) + 1
        top = dict(sorted(builds.items(), key=lambda kv: (-kv[1], kv[0]))[:5])
        d = {"games": len(rows), "players": sum(e[3] for e in rows), "open": len(open_rows),
             "full": sum(1 for e in rows if e[3] >= e[4]),
             "joinable": sum(1 for e in open_rows if e[3] < e[4]),
             "free_slots": sum(max(0, e[4] - e[3]) for e in open_rows), "sizes": sizes, "builds": top}
        return MAGIC + bytes([VERSION, T_STATS_REPLY]) + json.dumps(d, separators=(",", ":"), sort_keys=True).encode()

    def page(self, offset: int, requester: str, now: float) -> bytes:
        self.expire(now)
        rows = sorted(self.entries.items(), key=lambda kv: (kv[1][6].casefold(), kv[0]))
        total = len(rows)
        chunk = bytearray()
        count = 0
        for ip, (_, flags, port, players, maxp, build, name) in rows[offset:offset + MAX_PAGE_ENTRIES]:
            nb, bb = name.encode("utf-8"), build.encode("ascii")
            ent = (bytes(int(x) for x in ip.split(".")) + bytes([port >> 8, port & 255,
                   flags | (F_SELF if ip == requester else 0), players, maxp, len(nb)]) + nb + bytes([len(bb)]) + bb)
            if 9 + len(chunk) + len(ent) > MAX_REPLY:
                break
            chunk += ent
            count += 1
        return (MAGIC + bytes([VERSION, T_REPLY, total >> 8, total & 255, (offset >> 8) & 255, offset & 255,
                               count]) + bytes(chunk))


def _bind(ip: str, port: int):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind((ip, port))
    except OSError:
        s.close()
        raise
    return s


def serve(bind_ip: str, port: int, net: str, stop=lambda: False, reserve_server=True, stats=None):
    """stats: (ip, port) of the operator socket (loopback only), or None."""
    lobby = Lobby(net, reserve_server=reserve_server)
    sock = None
    while sock is None and not stop():
        try:
            sock = _bind(bind_ip, port)
        except OSError:           # 10.8.0.1 exists only once openvpn is up: retry
            time.sleep(3)
    if sock is None:
        return
    socks = [sock]
    if stats and ipaddress.ip_address(stats[0]).is_loopback:
        try:
            socks.append(_bind(*stats))
        except OSError:
            pass                  # the lobby itself matters more than its counts
    try:
        while not stop():
            try:
                ready = select.select(socks, [], [], 1.0)[0]
            except OSError:
                time.sleep(0.2)
                continue
            for s in ready:
                try:
                    data, addr = s.recvfrom(MAX_PACKET + 1)
                except OSError:
                    continue
                if s is sock:
                    reply = lobby.handle(data, addr[0], time.monotonic())
                elif ipaddress.ip_address(addr[0]).is_loopback:
                    reply = lobby.stats(data, time.monotonic())
                else:
                    reply = None
                if reply:
                    try:
                        s.sendto(reply, addr)
                    except OSError:
                        pass
    finally:
        for s in socks:
            s.close()


def main():
    # NWLOBBY_STATS=<ip>:<port>|off: the operator counts socket (loopback only).
    # NWLOBBY_BIND=<ip>[:port] and NWLOBBY_NET=<cidr> exist for tests (127.0.0.1 / 127.0.0.0/8);
    # the defaults are the production values.
    bind = os.environ.get("NWLOBBY_BIND", DEFAULT_BIND)
    host, _, p = bind.partition(":")
    net = os.environ.get("NWLOBBY_NET")
    st = os.environ.get("NWLOBBY_STATS", DEFAULT_STATS)
    sh, _, sp = st.partition(":")
    serve(host, int(p) if p else DEFAULT_PORT, net or DEFAULT_NET, reserve_server=not net,
          stats=None if st == "off" else (sh, int(sp or 9311)))


if __name__ == "__main__":
    sys.exit(main())
