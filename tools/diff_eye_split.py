#!/usr/bin/env python3
"""Compare the eye-split dump: for every stage of one frame, tile both eyes'
images and report where they differ.

Usage: python tools/diff_eye_split.py <edvr_logs/dumps directory> [gfx log]
       python tools/diff_eye_split.py --self-test

WHY THIS EXISTS

A planetary body renders as a featureless black disc in the right eye and
correctly in the left (2026-08-30). Eight rounds of the draw census proved
the two eyes receive the same draws, in the same order, with the same
inputs -- and a live skip probe then confirmed which draw paints the body:
removing it turns the moon black in the GOOD eye, reproducing the symptom
exactly. That draw runs six times for each eye. So the divergence is not in
what the game asks for; it is in what comes out.

src/d3d11/eye_split.cpp writes every render target the scene drew into on
one frame, both eyes at every stage. This pairs them by shape and says
which stage first disagrees. The geometry buffer disagreeing means the
surface pass produced nothing for that eye; the geometry buffer agreeing
and the lit image disagreeing means the loss is in lighting. Those two
answers point at completely different fixes, which is the whole reason to
look at pixels rather than at bindings.

WHAT COUNTS AS A DIFFERENCE

The two eyes see the world from 6 cm apart, so they are SUPPOSED to differ,
and near geometry differs a lot. This reports the shape of the difference
rather than its presence: a stereo disparity moves content sideways by a
few tiles and leaves the total brightness alone, while a missing body
removes brightness from a region and puts none back. The summary gives both
the tile-difference count and the signed brightness balance, and a body
missing from one eye shows as a strong one-sided imbalance that ordinary
parallax never produces.
"""

import os
import re
import struct
import sys

TILE = 16

# Bytes per texel by DXGI_FORMAT, over the ranges the enum groups them in.
# Must agree with texelBytes() in src/d3d11/eye_split.cpp -- the file on disk
# was written at that stride, and reading it at another produces noise that
# looks like a finding.
def texel_bytes(f):
    if 1 <= f <= 4:   return 16
    if 5 <= f <= 8:   return 12
    if 9 <= f <= 14:  return 8
    if 15 <= f <= 22: return 8
    if 23 <= f <= 32: return 4
    if 33 <= f <= 38: return 4
    if 39 <= f <= 47: return 4
    if 48 <= f <= 52: return 2
    if 53 <= f <= 59: return 2
    if 60 <= f <= 65: return 1
    if 87 <= f <= 93: return 4
    return 4


def _f11(bits, mbits):
    e = (bits >> mbits) & 0x1F
    m = bits & ((1 << mbits) - 1)
    if e == 0:
        return m / (1 << mbits) * 2.0 ** -14
    return (1.0 + m / (1 << mbits)) * 2.0 ** (e - 15)


def luma_r11g11b10(w):
    r = _f11(w & 0x7FF, 6)
    g = _f11((w >> 11) & 0x7FF, 6)
    b = _f11((w >> 22) & 0x3FF, 5)
    return 0.299 * r + 0.587 * g + 0.114 * b


def luma_rgba8(w):
    return (0.299 * (w & 0xFF) + 0.587 * ((w >> 8) & 0xFF) +
            0.114 * ((w >> 16) & 0xFF)) / 255.0


def luma_bgra8(w):
    return (0.114 * (w & 0xFF) + 0.587 * ((w >> 8) & 0xFF) +
            0.299 * ((w >> 16) & 0xFF)) / 255.0


def luma_rgb10a2(w):
    return (0.299 * (w & 0x3FF) + 0.587 * ((w >> 10) & 0x3FF) +
            0.114 * ((w >> 20) & 0x3FF)) / 1023.0


def luma_r32f(w):
    v = struct.unpack("<f", struct.pack("<I", w))[0]
    # Depth and single-channel targets are not brightness, but their
    # DIFFERENCE is still the measurement -- a body present in one eye's
    # depth and absent from the other's shows here exactly as it would in
    # colour. Values are clamped only to keep the report readable.
    if v != v or v in (float("inf"), float("-inf")):
        return 0.0
    return max(0.0, min(1.0, v))


def picker(fmt):
    if fmt in (26,):                       return 4, luma_r11g11b10
    if fmt in (23, 24, 25, 89):            return 4, luma_rgb10a2
    if fmt in (27, 28, 29, 30, 31, 32):    return 4, luma_rgba8
    if 87 <= fmt <= 93:                    return 4, luma_bgra8
    if fmt in (39, 40, 41, 42, 43,
               44, 45, 46, 47):            return 4, luma_r32f
    if 60 <= fmt <= 65:                    return 1, lambda b: b / 255.0
    if 53 <= fmt <= 59:                    return 2, lambda b: b / 65535.0
    return 4, luma_rgba8


def tile_map(path, w, h, fmt):
    """Mean luminance per 16x16 tile. Samples every 4th texel across and
    every row, which is plenty to see a body-sized region and keeps a
    22 MB file readable in a second."""
    bpt, luma = picker(fmt)
    tw, th = w // TILE, h // TILE
    tiles = [[0.0] * tw for _ in range(th)]
    unpack = {1: "<%dB", 2: "<%dH", 4: "<%dI"}.get(bpt, "<%dI")
    with open(path, "rb") as f:
        for ty in range(th):
            rows = f.read(w * bpt * TILE)
            if len(rows) < w * bpt * TILE:
                break
            vals = struct.unpack(unpack % (w * TILE), rows)
            for tx in range(tw):
                acc = 0.0
                n = 0
                for yy in range(TILE):
                    base = yy * w + tx * TILE
                    for xx in range(0, TILE, 4):
                        acc += luma(vals[base + xx])
                        n += 1
                tiles[ty][tx] = acc / n if n else 0.0
    return tiles


def scan(dirname):
    """Every eyesplit file, grouped by shape. Returns {(w,h,fmt): {idx: path}}."""
    pat = re.compile(r"^eyesplit_(\d+)x(\d+)f(\d+)_eye(\d+)\.bin$")
    out = {}
    for name in sorted(os.listdir(dirname)):
        m = pat.match(name)
        if not m:
            continue
        w, h, fmt, idx = (int(m.group(i)) for i in (1, 2, 3, 4))
        out.setdefault((w, h, fmt), {})[idx] = os.path.join(dirname, name)
    return out


def stage_order(logpath):
    """The EYESPLIT manifest lines give the order the frame drew the stages
    in, which is what makes 'the FIRST stage that disagrees' meaningful.
    Without a log the shapes are reported in filename order instead."""
    order = {}
    if not logpath or not os.path.exists(logpath):
        return order
    pat = re.compile(r"EYESPLIT stage=(\d+) shape=(\d+)x(\d+)f(\d+) eye=(\d+) "
                     r"draws=(\d+)")
    with open(logpath, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                st = int(m.group(1))
                key = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
                order.setdefault(key, (st, {}))[1][int(m.group(5))] = \
                    int(m.group(6))
    return order


def report(dirname, logpath=None):
    groups = scan(dirname)
    if not groups:
        print("no eyesplit_*.bin files in %s" % dirname)
        return 1
    order = stage_order(logpath)
    keys = sorted(groups, key=lambda k: order.get(k, (999, {}))[0])

    print("eye-split comparison: %d stage(s) in %s\n" % (len(keys), dirname))
    verdicts = []
    for key in keys:
        w, h, fmt = key
        files = groups[key]
        st, draws = order.get(key, (None, {}))
        label = "%dx%d fmt=%d" % (w, h, fmt)
        if st is not None:
            label = "stage %d  %s" % (st, label)
        if len(files) < 2:
            print("%-34s only %d image -- not a pair, skipped"
                  % (label, len(files)))
            continue

        a = tile_map(files[0], w, h, fmt)
        b = tile_map(files[1], w, h, fmt)
        th_, tw_ = len(a), len(a[0])

        # A tile counts as differing when it moves by more than this
        # fraction of the frame's own mean brightness -- relative, so an HDR
        # stage and an 8-bit stage are judged on the same footing.
        mean = sum(sum(r) for r in a) / (th_ * tw_) or 1e-6
        thresh = 0.25 * mean
        diff = 0
        signed = 0.0
        worst = (0.0, 0, 0)
        for y in range(th_):
            for x in range(tw_):
                d = a[y][x] - b[y][x]
                if abs(d) > thresh:
                    diff += 1
                signed += d
                if abs(d) > abs(worst[0]):
                    worst = (d, x, y)
        pct = 100.0 * diff / (th_ * tw_)
        bal = signed / (th_ * tw_) / mean
        drawnote = ""
        if draws:
            drawnote = "  draws %s" % "/".join(
                str(draws[k]) for k in sorted(draws))
        print("%-34s tiles differing %5.1f%%   balance %+6.2f   "
              "worst %+.4f at tile (%d,%d)%s"
              % (label, pct, bal, worst[0], worst[1], worst[2], drawnote))
        verdicts.append((st if st is not None else 999, label, pct, bal))

    print()
    # A one-sided imbalance is the signature we are hunting: parallax moves
    # content between the eyes and cancels out, a body drawn for one eye
    # only does not.
    flagged = [v for v in verdicts if abs(v[3]) > 0.05]
    if flagged:
        flagged.sort()
        st, label, pct, bal = flagged[0]
        print("FIRST ONE-SIDED STAGE: %s  (balance %+.2f)" % (label, bal))
        print("  One eye is systematically brighter here, which parallax "
              "alone does not do.")
        print("  This is the stage to look at; anything after it is "
              "downstream of the loss.")
    else:
        print("No stage shows a one-sided imbalance. Every difference looks "
              "like ordinary stereo,")
        print("which would mean the two eyes' images agree and the loss is "
              "past the last target here.")
    return 0


def self_test():
    ok = True
    for f, want in ((26, 4), (23, 4), (60, 1), (54, 2), (10, 8), (90, 4)):
        got = texel_bytes(f)
        if got != want:
            print("texel_bytes(%d) = %d, want %d" % (f, got, want))
            ok = False
    if abs(luma_rgba8(0xFFFFFFFF) - 1.0) > 1e-6:
        print("luma_rgba8 white is not 1.0")
        ok = False
    if luma_rgba8(0xFF000000) != 0.0:
        print("luma_rgba8 opaque black is not 0.0")
        ok = False
    if abs(luma_rgb10a2(0x3FFFFFFF) - 1.0) > 1e-3:
        print("luma_rgb10a2 white is not 1.0")
        ok = False
    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        sys.exit(self_test())
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    sys.exit(report(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
