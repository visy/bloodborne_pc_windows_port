#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""nwlobby - the Nightwatch VPN game lobby (UDP 9310 on the VPN server's tunnel address).

A hosting game announces itself every ~10 s; a guest asks for the list and joins from it. LAN
discovery (UDP 9308 broadcast) does not cross an OpenVPN tun, hence this tiny service.

Everything is in memory: nothing is written to disk, no address is ever logged. Only senders from
the VPN network are answered (default 10.8.0.0/24, minus the network, server and broadcast
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
An announce is not answered; a list request is answered with one reply page. Anything malformed is
dropped without an answer. The client UI shows names, flags and counts, never the address.
"""
import ipaddress
import os
import socket
import sys
import time
import unicodedata

MAGIC = b"\xfaL"
VERSION = 1
T_ANNOUNCE, T_LIST, T_REPLY, T_BYE = 1, 2, 3, 4
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
DEFAULT_NET = "10.8.0.0/24"
DEFAULT_BIND = "10.8.0.1"
DEFAULT_PORT = 9310


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


def serve(bind_ip: str, port: int, net: str, stop=lambda: False, reserve_server=True):
    lobby = Lobby(net, reserve_server=reserve_server)
    sock = None
    while sock is None and not stop():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((bind_ip, port))
            sock = s
        except OSError:           # 10.8.0.1 exists only once openvpn is up: retry
            s.close()
            time.sleep(3)
    if sock is None:
        return
    sock.settimeout(1.0)
    with sock:
        while not stop():
            try:
                data, addr = sock.recvfrom(MAX_PACKET + 1)
            except socket.timeout:
                continue
            except OSError:
                time.sleep(0.2)
                continue
            reply = lobby.handle(data, addr[0], time.monotonic())
            if reply:
                try:
                    sock.sendto(reply, addr)
                except OSError:
                    pass


def main():
    # NWLOBBY_BIND=<ip>[:port] and NWLOBBY_NET=<cidr> exist for tests (127.0.0.1 / 127.0.0.0/8);
    # the defaults are the production values.
    bind = os.environ.get("NWLOBBY_BIND", DEFAULT_BIND)
    host, _, p = bind.partition(":")
    net = os.environ.get("NWLOBBY_NET")
    serve(host, int(p) if p else DEFAULT_PORT, net or DEFAULT_NET, reserve_server=not net)


if __name__ == "__main__":
    sys.exit(main())
