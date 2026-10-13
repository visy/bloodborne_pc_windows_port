#!/usr/bin/env python3
"""The Hunter's Dream mirror: the developers' unfinished "Put on Disguise" (appearance editor).

Ported from droogie/bbhost (GPL-3.0-or-later; src/engine/dream_mirror_layout.cpp and
change_appearance.cpp), which credits the community mod "Restored Change Appearance Feature".

m21_00_00_00.msb places a part named ダミーCHR_再キャラメイク (a dummy character "for re-making
the character") at the workshop's mirror: entity 2100202, talk script t210696 - "Put on
Disguise", the one-time note "Wear clothing to change appearance." and then the game's
appearance editor (character creation's Appearance list). The developers left it a DummyEnemy
(part type 10), which never spawns. As an Enemy (type 2) it spawns like the workbench and
storage dummies beside it and the mirror works.

The game reads the parts list grouped by type, in file order, so the part moves: its entry goes
from slot 807 (the first DummyEnemy) to slot 742 (after the workbench dummy, in the Enemy group),
the 65 entries between move down one, the type indices follow, and every field naming a part by
its slot (an event's +0x48/+0x50/+0x5c, a part's +0x128/+0x13c/+0x144/+0x178/+0x180/+0x188) is
renumbered. Input and output are checked by SHA-256 (the 1.09 file only; nothing is guessed).

scripts/mods.py adds the result as a generated mod layer (BB_DREAM_MIRROR=0 turns it off). In a
party the talk is already open to guests (gpu/shim/party/party_guestsvc.cpp, t210696), and the
layer counts toward the party's mod check like any gameplay file: every player needs the same
setting (default on).

  dream_mirror.py GAME_DIR --out OUT   prints the layer folder (or nothing when unavailable)
"""
import argparse
import bisect
import hashlib
import struct
import sys
import zlib
from pathlib import Path

LAYOUT = Path('dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx')
LAYER_NAME = 'Dream mirror (bbhost)'
STAMP = 'dream-mirror 1'

INPUT_SHA256 = '8e1de0ee223cd5137a93383cfc7bb1d27964bc480c63e35013674717b403ef8f'
OUTPUT_SHA256 = '99d5c7b7f52307344c6e8296b499d5e57969f21c443fcc607cb42cb2c38c8afa'

FROM, TO, FIRST_ENEMY = 807, 742, 736
ENEMY, DUMMY_ENEMY = 2, 10
EVENT_FIELDS = (0x48, 0x50, 0x5c)
PART_FIELDS = (0x128, 0x13c, 0x144, 0x178, 0x180, 0x188)
MIRROR_ENTITY, MIRROR_TALK = 2100202, 210696


class LayoutError(ValueError):
    pass


# --- DCX (DFLT: a zlib stream behind a fixed 0x4c-byte header) ------------------------------
def dcx_decompress(data):
    if len(data) < 0x4c or data[:4] != b'DCX\0' or data[0x28:0x2c] != b'DFLT':
        raise LayoutError('not a DFLT DCX file')
    size = struct.unpack_from('>I', data, 0x1c)[0]
    out = zlib.decompress(data[0x4c:])
    if len(out) != size:
        raise LayoutError('DCX size mismatch')
    return out


def dcx_compress(raw):
    body = zlib.compress(raw, 9)
    head = (b'DCX\0' + struct.pack('>5I', 0x10000, 0x18, 0x24, 0x44, 0x4c) +
            b'DCS\0' + struct.pack('>2I', len(raw), len(body)) +
            b'DCP\0' + b'DFLT' + struct.pack('>6I', 0x20, 0x9000000, 0, 0, 0, 0x00010100) +
            b'DCA\0' + struct.pack('>I', 8))
    assert len(head) == 0x4c
    return head + body


# --- the MSB rewrite -------------------------------------------------------------------------
def _rd32(b, at):
    return struct.unpack_from('<i', b, at)[0]


def _rd64(b, at):
    return struct.unpack_from('<q', b, at)[0]


def read_lists(b):
    """The MSB's four lists (MODEL, EVENT, POINT, PARTS): [(entry table offset, [entry offsets])]."""
    out, at = [], 0x10
    for k in range(4):
        if at + 16 > len(b):
            raise LayoutError("the layout's lists do not read")
        count = _rd32(b, at + 4)
        table = at + 16
        if count < 1 or table + 8 * count > len(b):
            raise LayoutError("the layout's lists do not read")
        entries = [_rd64(b, table + 8 * i) for i in range(count - 1)]
        if any(e <= 0 or e >= len(b) for e in entries):
            raise LayoutError("the layout's lists do not read")
        out.append((table, entries))
        at = _rd64(b, table + 8 * (count - 1))
        if k < 3 and not 0 < at < len(b):
            raise LayoutError("the layout's lists do not read")
    return out


def remap(slot):
    if TO <= slot < FROM:
        return slot + 1
    if slot == FROM:
        return TO
    return slot


def mirror_layout(msb):
    """The 1.09 m21_00_00_00.msb (decompressed) with the mirror's character live."""
    if hashlib.sha256(msb).hexdigest() != INPUT_SHA256:
        raise LayoutError('not the 1.09 m21_00_00_00 layout')
    b = bytearray(msb)
    lists = read_lists(b)
    events, parts = lists[1][1], lists[3][1]
    parts_table = lists[3][0]
    if len(parts) <= FROM + 1:
        raise LayoutError('too few parts')
    starts = sorted(e for _, entries in lists for e in entries)

    def extent(entry):
        i = bisect.bisect_right(starts, entry)
        return (starts[i] if i < len(starts) else len(b)) - entry

    # 1) the fields naming a part by its slot, as they will be after the move
    for entries, fields in ((events, EVENT_FIELDS), (parts, PART_FIELDS)):
        for e in entries:
            size = extent(e)
            for f in fields:
                if f + 4 > size:
                    continue
                v = _rd32(b, e + f)
                if v >= 0 and remap(v) != v:
                    struct.pack_into('<i', b, e + f, remap(v))
    # 2) type indices
    for i, e in enumerate(parts):
        kind, index = _rd32(b, e + 0x14), _rd32(b, e + 0x18)
        if kind == ENEMY and i >= TO:
            struct.pack_into('<i', b, e + 0x18, index + 1)
        if kind == DUMMY_ENEMY and i > FROM:
            struct.pack_into('<i', b, e + 0x18, index - 1)
    moved = parts[FROM]
    if _rd32(b, moved + 0x14) != DUMMY_ENEMY:
        raise LayoutError('the stand-in is not a DummyEnemy')
    struct.pack_into('<i', b, moved + 0x14, ENEMY)
    struct.pack_into('<i', b, moved + 0x18, TO - FIRST_ENEMY)
    # 3) the bytes: the part's entry to slot TO's place, the entries between down by its size
    start, frm, end = parts[TO], parts[FROM], parts[FROM + 1]
    if not start < frm < end:
        raise LayoutError('the parts are not in file order')
    size = end - frm
    b[start:end] = b[frm:end] + b[start:frm]
    for i in range(TO, FROM):
        struct.pack_into('<q', b, parts_table + 8 * (i + 1), parts[i] + size)
    struct.pack_into('<q', b, parts_table + 8 * TO, start)
    if hashlib.sha256(b).hexdigest() != OUTPUT_SHA256:
        raise LayoutError('the rewrite is not the known result')
    return bytes(b)


# --- the generated mod layer -----------------------------------------------------------------
def enabled(env):
    return env.get('BB_DREAM_MIRROR', '1').strip() not in ('0', 'off', 'false', 'no')


def ensure_layer(game, out, log=None):
    """OUT/generated/dream_mirror (a loose mod folder with the rewritten layout), made once and
    again only when the stamp changes; None when the game has no 1.09 layout."""
    log = log or (lambda msg: print(f'Dream mirror: {msg}', file=sys.stderr))
    layer = Path(out) / 'generated' / 'dream_mirror'
    target = layer / LAYOUT
    stamp = layer / 'stamp'
    try:
        if target.is_file() and stamp.is_file() and stamp.read_text(encoding='utf-8') == STAMP:
            return layer
        source = Path(game) / LAYOUT
        if not source.is_file():
            log(f'off - the game has no {LAYOUT.as_posix()}')
            return None
        packed = dcx_compress(mirror_layout(dcx_decompress(source.read_bytes())))
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_name(target.name + '.tmp')
        tmp.write_bytes(packed)
        tmp.replace(target)
        stamp.write_text(STAMP, encoding='utf-8')
        log(f"made the Dream's layout with the mirror's character ({len(packed)} bytes)")
        return layer
    except (OSError, LayoutError, zlib.error) as error:
        log(f'off - {error}')
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', type=Path)
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    layer = ensure_layer(a.game, a.out)
    if layer:
        print(layer)


if __name__ == '__main__':
    main()
