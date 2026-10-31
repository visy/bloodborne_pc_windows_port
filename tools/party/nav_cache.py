#!/usr/bin/env python3
"""Compact per-area navmesh cache for the test walker (gpu/shim/party/nav_walk.h, docs/party/walker.md).

  python -I tools/party/nav_cache.py GAME OUTDIR [AREA ...]      AREA = 24 (maps m24_*), default: all areas

Reads each map's mAA_BB_CC_DD.nvmhktbnd.dcx (game data, read-only, untrusted: Havok hkaiNavMesh packfiles, see
nav_route.py) and writes OUTDIR/nav_mAA.bin: faces (convex polygons), their vertices and the links between
faces with the portal (shared edge) of each link. Links inside one file use the face's opposite edge; links across
files and maps use boundary edges that lie on each other (0.5 m cells, 0.6 m in height); their portal is the
overlap sample point (a degenerate portal).

File (little endian):  "BBNV" u32 version(1) u32 nverts u32 nfaces u32 nlinks u32 nidx
  float[3] * nverts | per face: u32 first, u32 count, u32 map (index of mAA_BB_CC_DD, i.e. BB*... see maps) |
  u32 idx * nidx | per face u32 linkStart (nfaces+1 entries) | per link: u32 face, float[6] portal a,b
  then u32 nmaps, per map: 16 bytes zero padded name.
"""
import math, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402
import nav_route as nr  # noqa: E402


def build(game, maps):
    faces, edges_of, border = [], [], {}   # faces: (map, [vertex idx]); links: face -> {nb: portal}
    verts, vkey = [], {}
    links = []

    def vid(p):
        k = (round(p[0], 3), round(p[1], 3), round(p[2], 3))
        if k not in vkey:
            vkey[k] = len(verts)
            verts.append(k)
        return vkey[k]

    for mi, m in enumerate(maps):
        p = Path(game) / "map" / m / f"{m}.nvmhktbnd.dcx"
        if not p.exists():
            continue
        files = ft.bnd4(ft.read_maybe_dcx(p))
        for name, b in files.items():
            r = nr.navmesh(b)
            if not r:
                continue
            fs, es, vs = r
            base = len(faces)
            for fi, f in enumerate(fs):
                faces.append((mi, [vid(vs[es[f[0] + k][0]][:3]) for k in range(f[2])]))
                links.append({})
                for k in range(f[2]):
                    e = es[f[0] + k]
                    a, c = vs[e[0]][:3], vs[e[1]][:3]
                    if e[3] != 0xFFFFFFFF and e[3] < len(fs):
                        links[base + fi][base + e[3]] = (a, c)
                        continue
                    n = max(1, int(math.dist(a, c) / 0.25))
                    for t in range(n + 1):
                        q = [a[j] + (c[j] - a[j]) * t / n for j in range(3)]
                        border.setdefault((round(q[0] * 2), round(q[2] * 2)), []).append((base + fi, q, name))
    for lst in border.values():
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                if lst[i][2] != lst[j][2] and abs(lst[i][1][1] - lst[j][1][1]) < 0.6:
                    q = lst[i][1]
                    links[lst[i][0]].setdefault(lst[j][0], (q, q))
                    links[lst[j][0]].setdefault(lst[i][0], (q, q))
    return verts, faces, links


def write(path, maps, verts, faces, links):
    idx, first = [], []
    for _, vl in faces:
        first.append((len(idx), len(vl)))
        idx += vl
    nl = sum(len(l) for l in links)
    out = bytearray(b"BBNV")
    out += struct.pack("<5I", 1, len(verts), len(faces), nl, len(idx))
    for v in verts:
        out += struct.pack("<3f", *v)
    for (s, c), (mi, _) in zip(first, faces):
        out += struct.pack("<3I", s, c, mi)
    out += struct.pack("<%dI" % len(idx), *idx)
    ls = 0
    for l in links:
        out += struct.pack("<I", ls)
        ls += len(l)
    out += struct.pack("<I", ls)
    for l in links:
        for nb, (a, c) in l.items():
            out += struct.pack("<I6f", nb, *a, *c)
    out += struct.pack("<I", len(maps))
    for m in maps:
        out += m.encode().ljust(16, b"\0")
    Path(path).write_bytes(bytes(out))


def main():
    game, outdir, areas = sys.argv[1], Path(sys.argv[2]), sys.argv[3:]
    allmaps = sorted(p.name for p in (Path(game) / "map").iterdir()
                     if p.is_dir() and len(p.name) == 12 and (p / f"{p.name}.nvmhktbnd.dcx").exists())
    if not areas:
        areas = sorted({m[1:3] for m in allmaps})
    outdir.mkdir(parents=True, exist_ok=True)
    for a in areas:
        maps = [m for m in allmaps if m[1:3] == a]
        if not maps:
            continue
        verts, faces, links = build(game, maps)
        write(outdir / f"nav_m{a}.bin", maps, verts, faces, links)
        print("area m%s: %d maps, %d faces, %d verts" % (a, len(maps), len(faces), len(verts)))


if __name__ == "__main__":
    main()
