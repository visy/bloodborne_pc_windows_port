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
Version 1 extension (0.7.8: invaders and "where the host is"). The v1 packets above are unchanged and an old client
keeps working: it never sends types 7-9 and its type 3 replies never carry the new flag bits.
  type 7 announce ext  (host -> lobby, right after every type 1; ignored when the sender has no live entry)
                   flags u8 (bit0 the host allows invaders, bit1 a boss fight is on), area u32 (the host's map id
                   0xAABBCCDD, 0 unknown), phase u8 (0 unknown, 1 day, 2 evening, 3 night, 4 Blood Moon),
                   ng u8 (0 = NG, 1..7 = NG+n, 255 unknown)                       -- 7 body bytes, strict
  type 8 list2     offset u16                      (as type 2, asks for the extended reply)
  type 9 reply2    total u16, offset u16, count u8, then count entries:
                   ipv4 4, port u16, flags u8 (bit0 password, bit1 your own entry, bit2 allows invaders, bit3 boss
                   fight), players u8, max u8, area u32, phase u8, ng u8, name: u8 len + UTF-8, build: u8 len + ASCII
The extension lives as long as the entry (TTL) and is replaced by the next type 7; a host that stops sending it (an
older build, or "Allow invaders" off and no extension) is listed with flags bit2 clear and area 0. The bit2 flag is
only ever set from the host's own type 7 sent in the last TTL seconds: a host with invasions off is never invadable
here. A client asks type 8 first and falls back to type 2 when the lobby does not answer (an older lobby drops type 8).
Mods label (0.7.8): what the host's game loaded, so a guest sees it before joining.
  type 10 announce mods  (host -> lobby, right after type 1/7; only when mods are loaded; ignored without a live entry)
                   label: u8 len + ASCII, 1..32 characters of a-z 0-9 @ . _ : + - (e.g. "nocturne@1.0", "mods:3f9a12c0")
  type 11 list3    offset u16                      (as type 8, asks for the reply with labels)
  type 12 reply3   as type 9, each entry followed by label: u8 len + ASCII (0 = no label)
The lobby only stores and relays the label (it never interprets it); it lives as long as the entry, like type 7. A
client asks 11, then 8, then 2. A client uses a label only as a key (a curated mod's id and version looked up in its own
built-in list); it never builds a path or an address from it.
The area id is a map id and nothing else (no coordinates); the client turns it into words (scripts/party_where.py
holds the table the launcher uses: "Currently in: Yahar'gul (Blood Moon), NG").
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
T_ANNOUNCE_EXT, T_LIST2, T_REPLY2 = 7, 8, 9
T_ANNOUNCE_MODS, T_LIST3, T_REPLY3 = 10, 11, 12
F_PASSWORD, F_SELF = 1, 2
F_INVADERS, F_BOSS = 4, 8   # type 9 only; type 3 never carries them

MAX_PACKET = 512            # a longer datagram is dropped unread
MAX_REPLY = 1024
MAX_PAGE_ENTRIES = 16
MAX_ENTRIES = 300
TTL = 30.0                  # seconds an entry lives without a new announce
NAME_MAX_CHARS = 16
NAME_MAX_BYTES = 48
BUILD_MAX = 32
LABEL_MAX = 32
LABEL_RE = re.compile(rb"[a-z0-9@._:+-]{1,32}")   # the mods label (type 10), whole match only
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


# ---- offensive names ---------------------------------------------------------------------------------
# A host whose name is offensive still gets listed, under a neutral masked name ("Hunter 3F2"). The matcher is
# the shared one (scripts/namefilter.py in the repo, installed next to this file as namefilter.py; its word lists are
# embedded in namefilter_data.py, and a folder name_filter/ beside it overrides them). The service runs with -I, so
# the module is loaded by explicit path. If it is missing the lobby still runs, unfiltered, and says so on stderr.
_FILTER = None


def _load_filter():
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (here, os.path.join(here, "..", "..", "..", "scripts")):
        path = os.path.join(d, "namefilter.py")
        if os.path.exists(path):
            spec = importlib.util.spec_from_file_location("namefilter", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod
    return None


def _filter():
    global _FILTER
    if _FILTER is None:
        try:
            _FILTER = _load_filter() or False
            if _FILTER and not _FILTER.default_filter():
                _FILTER = False
        except Exception as ex:   # a broken word list must not take the lobby down
            sys.stderr.write("nwlobby: name filter not loaded: %s\n" % ex)
            _FILTER = False
        if not _FILTER:
            sys.stderr.write("nwlobby: name filter not available; names are listed unmasked\n")
    return _FILTER or None


def name_offensive(name: str) -> bool:
    """True when a lobby name is a slur, targets a protected group with violence, or carries a hate code."""
    f = _filter()
    return bool(f and f.name_offensive(name))


def masked_name(name: str) -> str:
    """The neutral name shown instead of an offensive one: stable per name, short, ASCII."""
    from_mod = _filter()
    return from_mod.mask_text(name) if from_mod else name



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


def parse_announce_ext(body: bytes):
    """(flags, area, phase, ng) or None. Strict: 7 bytes, flags only bit0/bit1, phase 0..4, ng 0..7 or 255."""
    if len(body) != 7:
        return None
    flags, area, phase, ng = body[0], int.from_bytes(body[1:5], "big"), body[5], body[6]
    if flags > 3 or phase > 4 or (ng > 7 and ng != 255):
        return None
    return flags, area, phase, ng


def encode_announce_ext(invaders: bool, boss: bool, area: int, phase: int, ng: int) -> bytes:
    return (MAGIC + bytes([VERSION, T_ANNOUNCE_EXT, (1 if invaders else 0) | (2 if boss else 0)]) +
            area.to_bytes(4, "big") + bytes([phase, ng]))


def parse_announce_mods(body: bytes):
    """The label string or None. Strict: u8 len + exactly that many bytes, all of a-z 0-9 @ . _ : + -, 1..32."""
    if len(body) < 2 or body[0] != len(body) - 1 or body[0] > LABEL_MAX:
        return None
    label = body[1:]
    return label.decode("ascii") if LABEL_RE.fullmatch(label) else None


def encode_announce_mods(label: str) -> bytes:
    b = label.encode("ascii")
    return MAGIC + bytes([VERSION, T_ANNOUNCE_MODS, len(b)]) + b


def encode_list3(offset: int = 0) -> bytes:
    return MAGIC + bytes([VERSION, T_LIST3, (offset >> 8) & 255, offset & 255])


def encode_list2(offset: int = 0) -> bytes:
    return MAGIC + bytes([VERSION, T_LIST2, (offset >> 8) & 255, offset & 255])


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


def parse_reply3(data: bytes):
    """(total, offset, [entry dict]) of a type 12 reply or None. Entry: as parse_reply2 plus mods (the label, "" for none)."""
    if len(data) < 9 or data[:2] != MAGIC or data[2] != VERSION or data[3] != T_REPLY3:
        return None
    return _parse_rows2(data, labels=True)


def parse_reply2(data: bytes):
    """(total, offset, [entry dict]) of a type 9 reply or None. Entry: as parse_reply plus invaders, boss, area, phase, ng."""
    if len(data) < 9 or data[:2] != MAGIC or data[2] != VERSION or data[3] != T_REPLY2:
        return None
    return _parse_rows2(data, labels=False)


def _parse_rows2(data: bytes, labels: bool):
    total, offset, count = (data[4] << 8) | data[5], (data[6] << 8) | data[7], data[8]
    pos = 9
    out = []
    for _ in range(count):
        if pos + 16 > len(data):
            return None
        ip = ".".join(str(b) for b in data[pos:pos + 4])
        port = (data[pos + 4] << 8) | data[pos + 5]
        flags, players, maxp = data[pos + 6], data[pos + 7], data[pos + 8]
        area = int.from_bytes(data[pos + 9:pos + 13], "big")
        phase, ng, n = data[pos + 13], data[pos + 14], data[pos + 15]
        pos += 16
        if flags & ~15 or phase > 4 or (ng > 7 and ng != 255) or pos + n + 1 > len(data):
            return None
        name = data[pos:pos + n].decode("utf-8", "replace")
        pos += n
        b = data[pos]
        pos += 1
        if pos + b > len(data):
            return None
        build = data[pos:pos + b].decode("ascii", "replace")
        pos += b
        row = {"ip": ip, "port": port, "password": bool(flags & F_PASSWORD), "own": bool(flags & F_SELF),
               "invaders": bool(flags & F_INVADERS), "boss": bool(flags & F_BOSS), "players": players,
               "max": maxp, "area": area, "phase": phase, "ng": ng, "name": name, "build": build}
        if labels:
            if pos >= len(data) or pos + 1 + data[pos] > len(data):
                return None
            n = data[pos]
            label = data[pos + 1:pos + 1 + n]
            pos += 1 + n
            if n and not LABEL_RE.fullmatch(label):
                return None
            row["mods"] = label.decode("ascii")
        out.append(row)
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
        self.ext = {}       # ip -> (expiry, flags, area, phase, ng): the 0.7.8 extension (type 7)
        self.mods = {}      # ip -> (expiry, label): the mods label (type 10)

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
        for ip in [k for k, e in self.ext.items() if e[0] <= now or k not in self.entries]:
            del self.ext[ip]
        for ip in [k for k, e in self.mods.items() if e[0] <= now or k not in self.entries]:
            del self.mods[ip]
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
                name = masked_name(name)   # still listed, under a neutral name (no log line: the lobby logs nothing)
            self.entries[ip] = (now + self.ttl, flags, port, players, maxp, build, name)
            return None
        if kind == T_ANNOUNCE_EXT:
            p = parse_announce_ext(body)
            self.expire(now)
            if p is not None and ip in self.entries:   # only a host that announced (type 1) and is alive
                self.ext[ip] = (now + self.ttl,) + p
            return None
        if kind == T_ANNOUNCE_MODS:
            label = parse_announce_mods(body)
            self.expire(now)
            if label is not None and ip in self.entries:   # only a live host; the label is relayed, never interpreted
                self.mods[ip] = (now + self.ttl, label)
            return None
        if kind == T_BYE:
            if len(body) == 0:
                self.entries.pop(ip, None)
                self.ext.pop(ip, None)
                self.mods.pop(ip, None)
            return None
        if kind in (T_LIST, T_LIST2, T_LIST3):
            if len(body) != 2:
                return None
            return self.page((body[0] << 8) | body[1], ip, now, extended=kind != T_LIST, labels=kind == T_LIST3)
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

    def page(self, offset: int, requester: str, now: float, extended: bool = False, labels: bool = False) -> bytes:
        self.expire(now)
        rows = sorted(self.entries.items(), key=lambda kv: (kv[1][6].casefold(), kv[0]))
        total = len(rows)
        chunk = bytearray()
        count = 0
        for ip, (_, flags, port, players, maxp, build, name) in rows[offset:offset + MAX_PAGE_ENTRIES]:
            nb, bb = name.encode("utf-8"), build.encode("ascii")
            own = F_SELF if ip == requester else 0
            if extended:
                ex = self.ext.get(ip)
                xf, area, phase, ng = (ex[1], ex[2], ex[3], ex[4]) if ex else (0, 0, 0, 255)
                ent = (bytes(int(x) for x in ip.split(".")) + bytes([port >> 8, port & 255,
                       flags | own | (F_INVADERS if xf & 1 else 0) | (F_BOSS if xf & 2 else 0), players, maxp]) +
                       area.to_bytes(4, "big") + bytes([phase, ng, len(nb)]) + nb + bytes([len(bb)]) + bb)
                if labels:
                    m = self.mods.get(ip)
                    lb = m[1].encode("ascii") if m else b""
                    ent += bytes([len(lb)]) + lb
            else:   # the v1 row: an old client's strict parser accepts only the password and own bits
                ent = (bytes(int(x) for x in ip.split(".")) + bytes([port >> 8, port & 255,
                       flags | own, players, maxp, len(nb)]) + nb + bytes([len(bb)]) + bb)
            if 9 + len(chunk) + len(ent) > MAX_REPLY:
                break
            chunk += ent
            count += 1
        kind = T_REPLY3 if labels else T_REPLY2 if extended else T_REPLY
        return (MAGIC + bytes([VERSION, kind, total >> 8, total & 255, (offset >> 8) & 255, offset & 255,
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
