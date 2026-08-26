#!/usr/bin/env python3
"""Compare the fss_eye_dump captures: for each checkpoint, tile both eyes'
images 16x16 and report where they differ.

Usage: python tools/diff_eye_dump.py <edvr_logs/dumps directory> [gfx log]

The dumps are raw rows (fssdump_c<N>_<tag>_eye<E>.bin), 32 bits per pixel.
Format comes from the FSSDUMP manifest lines in the gfx log when given, and
is assumed R11G11B10_FLOAT (fmt 26) otherwise. The question nineteen rounds
of channel probes could not answer is positional: WHICH interval of the
frame introduces the left/right difference. Tiles are the squares' own
granularity, so the defect shows up as a tile-count asymmetry."""

import os
import re
import struct
import sys

TILE = 16


def luma_r11g11b10(word):
    # 11-bit floats: 5-bit exponent (bias 15), 6/5-bit mantissa. Enough to
    # rank tiles; exact colour is not the question.
    def f11(bits, mbits):
        e = (bits >> mbits) & 0x1F
        m = bits & ((1 << mbits) - 1)
        if e == 0:
            return m / (1 << mbits) * 2.0 ** -14
        return (1.0 + m / (1 << mbits)) * 2.0 ** (e - 15)

    r = f11(word & 0x7FF, 6)
    g = f11((word >> 11) & 0x7FF, 6)
    b = f11((word >> 22) & 0x3FF, 5)
    return 0.299 * r + 0.587 * g + 0.114 * b


def luma_rgba8(word):
    r = word & 0xFF
    g = (word >> 8) & 0xFF
    b = (word >> 16) & 0xFF
    return (0.299 * r + 0.587 * g + 0.114 * b) / 255.0


def tile_map(path, w, h, fmt):
    tw, th = w // TILE, h // TILE
    tiles = [[0.0] * tw for _ in range(th)]
    luma = luma_rgba8 if fmt in (27, 28, 87) else luma_r11g11b10
    with open(path, "rb") as f:
        for ty in range(th):
            rows = f.read(w * 4 * TILE)
            words = struct.unpack("<%dI" % (w * TILE), rows)
            for tx in range(tw):
                acc = 0.0
                for yy in range(TILE):
                    base = yy * w + tx * TILE
                    for xx in range(0, TILE, 4):   # sample every 4th texel
                        acc += luma(words[base + xx])
                tiles[ty][tx] = acc / (TILE * TILE / 4)
    return tiles


def main():
    dumps = sys.argv[1]
    fmts = {}
    dims = {}
    if len(sys.argv) > 2:
        for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
            m = re.search(
                r"FSSDUMP c=(\d+) tag=(\S+) eye=(\d) w=(\d+) h=(\d+) "
                r"fmt=(\d+)", line)
            if m:
                key = (int(m.group(1)), int(m.group(3)))
                dims[key] = (int(m.group(4)), int(m.group(5)))
                fmts[key] = int(m.group(6))

    files = sorted(os.listdir(dumps))
    pairs = {}
    for fn in files:
        m = re.match(r"fssdump_c(\d+)_(\S+)_eye(\d)\.bin", fn)
        if m:
            pairs.setdefault((int(m.group(1)), m.group(2)), {})[
                int(m.group(3))] = os.path.join(dumps, fn)

    for (c, tag), eyes in sorted(pairs.items()):
        if 0 not in eyes or 1 not in eyes:
            print(f"c{c} {tag}: missing an eye, skipped")
            continue
        w, h = dims.get((c, 0), (4340, 4284))
        fmt = fmts.get((c, 0), 26)
        a = tile_map(eyes[0], w, h, fmt)
        b = tile_map(eyes[1], w, h, fmt)
        diff = []
        dark_a = dark_b = 0
        for ty in range(len(a)):
            for tx in range(len(a[0])):
                la, lb = a[ty][tx], b[ty][tx]
                if abs(la - lb) > 0.05 * max(la, lb, 0.02):
                    diff.append((ty, tx, la, lb))
                    if la < lb * 0.25:
                        dark_a += 1
                    elif lb < la * 0.25:
                        dark_b += 1
        print(f"c{c} {tag}: {len(diff)} differing tiles "
              f"(eye0 much darker in {dark_a}, eye1 much darker in {dark_b})")
        for ty, tx, la, lb in diff[:8]:
            print(f"    tile ({tx},{ty})  eye0={la:.4f} eye1={lb:.4f}")


if __name__ == "__main__":
    main()
