#!/usr/bin/env python3
"""Census of the engine's fixed player tables (docs/party/six_players.md): which functions touch them.

  python tools/re/six_census.py [ELF] [functions.tsv]   (defaults: the game's out/eboot.elf, deps/ghidra-export)

Prints, for eboot 1.09 (ELF VA = file offset - 0x4000): the functions that load the presentation object
(*(flow 0x5556678 + 0x16f8)) with the displacements they read from it, and the functions that reach GameDataMan
(0x553b130) and use the remote-row stride 0x6a0. Read-only; needs numpy and capstone. Addresses only, no game data.
"""
import bisect, collections, sys
import numpy as np
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_REG

ELF = sys.argv[1] if len(sys.argv) > 1 else r"E:\games\bloodborne\out\eboot.elf"
FUNCS = sys.argv[2] if len(sys.argv) > 2 else r"E:\games\bbport\deps\ghidra-export\functions.tsv"
OFF, TEXT = 0x4000, 0x2400000
F = open(ELF, "rb").read()
fn = sorted((int(p[0], 16), int(p[3], 16)) for p in (l.split("\t") for l in open(FUNCS)) if len(p) > 3 and p[0].startswith("0x"))
keys = [f[0] for f in fn]
owner = lambda va: fn[bisect.bisect_right(keys, va) - 1][0]
b = np.frombuffer(F[OFF:OFF + 0x50d96dc], dtype=np.uint8).astype(np.uint32)
d = (b[:-3] | (b[1:-2] << 8) | (b[2:-1] << 16) | (b[3:] << 24)).astype(np.int64)
d = np.where(d >= 2**31, d - 2**32, d)
idx = np.arange(len(d), dtype=np.int64)

def xrefs(target):
    out = []
    for extra in range(5):
        for p in np.nonzero((d + idx) == (target - 4 - extra))[0]:
            if (F[OFF + int(p) - 1] & 0xC7) == 0x05:
                out.append(int(p))
    return sorted(out)

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
pres = collections.defaultdict(set)
for p in xrefs(0x5556678):
    if b"\xf8\x16\x00\x00" not in F[OFF + p + 4:OFF + p + 40]:
        continue
    ins = list(md.disasm(F[OFF + p - 3:OFF + p + 0x200], p - 3))
    reg = next((i.operands[0].reg for i in ins[:8] if i.mnemonic == "mov" and any(o.type == X86_OP_MEM and o.mem.disp == 0x16f8 for o in i.operands)), None)
    if reg is None:
        continue
    for i in ins[:80]:
        for o in i.operands:
            if o.type == X86_OP_MEM and o.mem.base == reg and o.mem.disp >= 0x14:
                pres[owner(p)].add(o.mem.disp)
print("presentation object users: %d functions" % len(pres))
for f, ds in sorted(pres.items()):
    print("  %#x" % f, " ".join("%#x" % x for x in sorted(ds) if x < 0xb0))
gdm = {owner(p) for p in xrefs(0x553b130)}
rows = collections.defaultdict(list)
pos = OFF
while True:
    pos = F.find(bytes.fromhex("a0060000"), pos, TEXT + OFF)
    if pos < 0:
        break
    if owner(pos - OFF) in gdm:
        rows[owner(pos - OFF)].append(pos - OFF)
    pos += 1
print("GameDataMan remote-row stride (0x6a0) users: %d functions" % len(rows))
for f, hs in sorted(rows.items()):
    print("  %#x" % f, " ".join("%#x" % x for x in hs))
