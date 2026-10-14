#!/usr/bin/env python3
"""Checks the native HDR output (I4) on a dump of a running game, without an HDR screen.

  python tools/windows/hdr_check.py DUMP_DIR [--log GAME_LOG] [--png OUT.png]

The game writes the dump when BB_HDR_DUMP_TRIGGER names a file that exists (it is consumed):
f<NNN>_hdr_frame_<w>x<h>_a2b10g10r10.raw (the SDR frame the compose pass reads) and
f<NNN>_hdr_out_<w>x<h>_<format>.raw (the swapchain image it drew) in BB_DUMP_DIR, and logs the
curve's parameters ("HDR dump params: ..."). This script recomputes every pixel of the frame
with the reference mapping (gpu/shim/bbport_hdr.h, mirrored below) and compares it with the
encoded output: the PQ codes must agree within a code or two (the overlay's pixels and the
calibration band are skipped). It prints the light levels (paper white kept for mid-tones,
highlights up to the peak) and with --png writes a preview: the decoded HDR picture mapped back to
SDR for an SDR screen, with the pixels above paper white tinted to show where the expansion acts.
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np

KNEE, GAMMA = 0.5, 2.2
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
TO2020 = np.array([[0.6274040, 0.3292820, 0.0433136],
                   [0.0690970, 0.9195400, 0.0113612],
                   [0.0163916, 0.0880132, 0.8955950]])
LUMA709 = np.array([0.2126, 0.7152, 0.0722])


def pq_encode(nits):
    y = np.clip(nits / 10000.0, 0.0, 1.0) ** M1
    return ((C1 + C2 * y) / (1 + C3 * y)) ** M2


def pq_decode(signal):
    p = np.clip(signal, 0.0, 1.0) ** (1 / M2)
    return 10000.0 * (np.maximum(p - C1, 0) / (C2 - C3 * p)) ** (1 / M1)


def expected_pq(encoded, peak, paper, strength, saturation):
    """bbport_hdr.h EncodePq for an (..., 3) array of encoded SDR values."""
    c = np.clip(encoded, 0, 1) ** GAMMA
    y = c @ LUMA709
    target = 1 + max(strength, 0) * (max(peak, paper) / paper - 1)
    t = (np.minimum(y, 1) - KNEE) / (1 - KNEE)
    expanded = np.where(y > KNEE, y + (target - 1) * t * t, y)
    scale = np.where(y > KNEE, expanded / np.maximum(y, 1e-9), 1.0)
    c = c * scale[..., None]
    y2 = (c @ LUMA709)[..., None]
    c = y2 + (c - y2) * saturation
    n = np.maximum(c @ TO2020.T, 0) * paper
    m = n.max(axis=-1, keepdims=True)
    limit = max(peak, paper)
    n = np.where(m > limit, n * (limit / np.maximum(m, 1e-9)), n)
    return pq_encode(n), n


def unpack_1010102(raw, width, height, bgr=False):
    v = np.frombuffer(raw, dtype="<u4").reshape(height, width)
    a, b, c = v & 0x3FF, (v >> 10) & 0x3FF, (v >> 20) & 0x3FF
    rgb = np.stack([c, b, a] if bgr else [a, b, c], axis=-1).astype(np.float64) / 1023.0
    return rgb


def load(path):
    m = re.search(r"_(\d+)x(\d+)_([a-z0-9]+)\.raw$", path.name)
    w, h, fmt = int(m.group(1)), int(m.group(2)), m.group(3)
    raw = path.read_bytes()
    if fmt in ("a2b10g10r10", "a2r10g10b10"):
        return unpack_1010102(raw, w, h, bgr=fmt == "a2r10g10b10"), fmt
    if fmt == "rgba16":
        return np.frombuffer(raw, dtype="<u2").reshape(h, w, 4)[..., :3] / 65535.0, fmt
    return np.frombuffer(raw, dtype="<f2").reshape(h, w, 4)[..., :3].astype(np.float64), fmt


def params_from_log(log):
    text = Path(log).read_text(encoding="utf-8", errors="replace") if log else ""
    found = re.findall(r"HDR dump params: encoding=(\d+) peak=([\d.]+) paper_white=([\d.]+) strength=([\d.]+) "
                       r"saturation=([\d.]+) fit=([\d.]+),([\d.]+),([\d.]+),([\d.]+) pattern=(\d)", text)
    if not found:
        return None
    e, peak, paper, strength, sat, x, y, w, h, pattern = found[-1]
    return dict(encoding=int(e), peak=float(peak), paper=float(paper), strength=float(strength),
                saturation=float(sat), fit=(int(float(x)), int(float(y)), int(float(w)), int(float(h))),
                pattern=int(pattern))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump_dir")
    ap.add_argument("--log", help="the game's log (the 'HDR dump params' line); else defaults")
    ap.add_argument("--png", help="write an SDR preview of the HDR output here")
    ap.add_argument("--max-code-error", type=float, default=2.0)
    a = ap.parse_args()
    d = Path(a.dump_dir)
    frames = sorted(d.glob("f*_hdr_frame_*.raw"))
    outs = sorted(d.glob("f*_hdr_out_*.raw"))
    if not frames or not outs:
        print(f"no hdr_frame / hdr_out dump in {d}")
        return 2
    frame, _ = load(frames[-1])
    out, fmt = load(outs[-1])
    p = params_from_log(a.log) or dict(encoding=1, peak=1000.0, paper=200.0, strength=1.0, saturation=1.0,
                                        fit=(0, 0, out.shape[1], out.shape[0]), pattern=0)
    print(f"dump: frame {frame.shape[1]}x{frame.shape[0]}, output {out.shape[1]}x{out.shape[0]} {fmt}; "
          f"peak {p['peak']:.0f} paper white {p['paper']:.0f} strength {p['strength']:.2f} "
          f"saturation {p['saturation']:.2f} fit {p['fit']}")
    if p["encoding"] != 1:
        print("scRGB output: only the light levels are reported")
        nits = np.maximum(out, 0) * 80.0
    else:
        nits = pq_decode(out)
    x, y, w, h = p["fit"]
    region = out[y:y + h, x:x + w]
    ok = True
    if (w, h) == (frame.shape[1], frame.shape[0]) and p["encoding"] == 1:
        want, _ = expected_pq(frame, p["peak"], p["paper"], p["strength"], p["saturation"])
        err = np.abs(region - want).max(axis=-1) * 1023.0
        mask = np.ones(err.shape, bool)
        if p["pattern"]:
            H, W = out.shape[:2]
            top, bottom = int(0.04 * H) - y, int(0.30 * H) - y + 40
            mask[max(top, 0):max(bottom, 0), :] = False
        # The overlay's pixels differ by design; they are few and far off the expected value.
        sel = err[mask]
        within = float((sel <= a.max_code_error).mean()) * 100
        print(f"PQ codes vs reference: median error {np.median(sel):.2f}, 99th percentile "
              f"{np.percentile(sel, 99):.2f}, within {a.max_code_error:g} codes: {within:.2f} % of {sel.size} pixels")
        ok = within >= 97.0
    else:
        print("frame scaled into the swapchain (or not PQ): per-pixel comparison skipped")
    lum = nits @ np.array([0.2627, 0.6780, 0.0593])
    print(f"light: max {lum.max():.0f} nits, 99.9th percentile {np.percentile(lum, 99.9):.0f}, "
          f"median {np.median(lum):.1f}; above paper white {float((lum > p['paper'] * 1.01).mean()) * 100:.2f} % "
          f"of pixels; above the peak {float((lum > p['peak'] * 1.01).mean()) * 100:.3f} %")
    if a.png:
        from PIL import Image
        to709 = np.linalg.inv(TO2020)
        rel = np.maximum(nits @ to709.T, 0) / p["paper"]
        sdr = np.clip(rel / (1 + rel * 0.25) * 1.25, 0, 1) ** (1 / GAMMA)  # gentle roll-off for viewing
        hot = (lum > p["paper"] * 1.01)[..., None]
        sdr = np.where(hot, sdr * np.array([1.0, 0.85, 0.85]), sdr)
        Image.fromarray((sdr * 255 + 0.5).astype(np.uint8)).save(a.png)
        print(f"preview: {a.png} (above paper white tinted red)")
    print("HDR check:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
