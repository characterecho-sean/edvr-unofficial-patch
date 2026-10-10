#!/usr/bin/env python3
"""Compare the eye-dump captures: for each checkpoint, tile both eyes'
images 16x16 and report where they differ.

Usage: python tools/diff_eye_dump.py <edvr_logs/dumps directory> [gfx log]
       python tools/diff_eye_dump.py --self-test

The dumps are raw rows (fssdump_c<N>_<tag>_eye<E>.bin), 32 bits per pixel.
Format comes from the FSSDUMP manifest lines in the gfx log when given, and
is assumed R11G11B10_FLOAT (fmt 26) otherwise. The question nineteen rounds
of channel probes could not answer is positional: WHICH interval of the
frame introduces the left/right difference. Tiles are the squares' own
granularity, so the defect shows up as a tile-count asymmetry.

It only reads and prints. --self-test builds 32x32 dumps in the temp folder
with known tile values and checks the two pixel decodes, the tile map, the
manifest parse and the report against them."""

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


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if "--self-test" in argv:
        return self_test()
    dumps = argv[0]
    fmts = {}
    dims = {}
    if len(argv) > 1:
        for line in open(argv[1], encoding="utf-8", errors="replace"):
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
    return 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def self_test():
    """Known 32x32 dumps (2x2 tiles) in the temp folder, through both pixel
    decodes, the tile map, the manifest parse and the report."""
    import contextlib
    import io
    import shutil
    import tempfile

    failures = []

    def expect(name, condition, what=""):
        if not condition:
            failures.append("%s: %s" % (name, what))

    def near(a, b, tol=1e-6):
        return abs(a - b) <= tol

    # R11G11B10_FLOAT words: 1.0 is exponent 15, no mantissa, in each field.
    white = (15 << 6) | ((15 << 6) << 11) | ((15 << 5) << 22)
    half = (14 << 6) | ((14 << 6) << 11) | ((14 << 5) << 22)
    expect("r11g11b10", near(luma_r11g11b10(white), 1.0) and near(luma_r11g11b10(half), 0.5)
           and luma_r11g11b10(0) == 0.0,
           "%r %r %r" % (luma_r11g11b10(white), luma_r11g11b10(half), luma_r11g11b10(0)))
    # Denormals: exponent 0 is mantissa / 2^bits * 2^-14, not a hidden-one float.
    expect("r11g11b10-denormal", near(luma_r11g11b10(32), 0.299 * (32 / 64) * 2.0 ** -14, 1e-12),
           repr(luma_r11g11b10(32)))
    # RGBA8: R in the low byte, and the weights are Rec.601's.
    expect("rgba8", near(luma_rgba8(0x00FFFFFF), 1.0) and near(luma_rgba8(0x000000FF), 0.299)
           and near(luma_rgba8(0x0000FF00), 0.587) and near(luma_rgba8(0x00FF0000), 0.114)
           and luma_rgba8(0) == 0.0, "the RGBA8 weights")

    base = tempfile.mkdtemp(prefix="edvr-eye-dump-")

    def dump(name, fmt_words):
        """A 32x32 raw dump whose four tiles are the four words, row-major."""
        path = os.path.join(base, name)
        with open(path, "wb") as f:
            for y in range(32):
                row = [fmt_words[(y // TILE) * 2 + (x // TILE)] for x in range(32)]
                f.write(struct.pack("<32I", *row))
        return path

    def run(*argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = main(list(argv))
        return code, out.getvalue()

    try:
        # The tile map: each tile is the mean luma of the texels it samples.
        tiles = tile_map(dump("map.bin", [white, 0, half, white]), 32, 32, 26)
        expect("tile-map", len(tiles) == 2 and len(tiles[0]) == 2
               and near(tiles[0][0], 1.0) and near(tiles[0][1], 0.0)
               and near(tiles[1][0], 0.5) and near(tiles[1][1], 1.0), repr(tiles))
        opaque = 0x00FFFFFF
        tiles = tile_map(dump("map8.bin", [opaque, 0, 0x00FF0000, opaque]), 32, 32, 27)
        expect("tile-map-rgba8", near(tiles[0][0], 1.0) and near(tiles[0][1], 0.0)
               and near(tiles[1][0], 0.114) and near(tiles[1][1], 1.0), repr(tiles))

        # The report. Checkpoint 1: the eyes agree except the top-right tile,
        # dark in eye 1. Checkpoint 2: identical. Checkpoint 3: one eye only.
        # Checkpoint 4 is RGBA8, named so by the manifest, and eye 0 is darker.
        d = os.path.join(base, "dumps")
        os.makedirs(d)
        same = [white, white, white, white]
        # Checkpoint 5: a tile 12.5% dimmer in eye 1 is a difference (the threshold is
        # 5% of the brighter tile) and not a dark one; a tile 3% brighter is not.
        dim = (14 << 6 | 48) | ((14 << 6 | 48) << 11) | ((14 << 5 | 24) << 22)     # 0.875
        slight = (15 << 6 | 2) | ((15 << 6 | 2) << 11) | ((15 << 5 | 1) << 22)     # 1.03125
        for leaf, words in (("fssdump_c1_pre_eye0.bin", same),
                            ("fssdump_c1_pre_eye1.bin", [white, 0, white, white]),
                            ("fssdump_c2_post_eye0.bin", same), ("fssdump_c2_post_eye1.bin", same),
                            ("fssdump_c3_lone_eye0.bin", same),
                            ("fssdump_c4_ui_eye0.bin", [0, opaque, opaque, opaque]),
                            ("fssdump_c4_ui_eye1.bin", [opaque, opaque, opaque, opaque]),
                            ("fssdump_c5_mid_eye0.bin", same),
                            ("fssdump_c5_mid_eye1.bin", [dim, slight, white, white])):
            os.replace(dump(leaf, words), os.path.join(d, leaf))
        with open(os.path.join(base, "gfx.log"), "w", encoding="utf-8") as f:
            f.write("noise before\n")
            for c, fmt in ((1, 26), (2, 26), (3, 26), (4, 27), (5, 26)):
                for eye in (0, 1):
                    f.write("[12:00:00.000] FSSDUMP c=%d tag=t eye=%d w=32 h=32 fmt=%d\n" % (c, eye, fmt))
            f.write("FSSDUMP not a manifest line\n")
        code, said = run(d, os.path.join(base, "gfx.log"))
        expect("report", code == 0, "exit %r\n%s" % (code, said))
        expect("report", "c1 pre: 1 differing tiles (eye0 much darker in 0, eye1 much darker in 1)" in said, said)
        expect("report", "    tile (1,0)  eye0=1.0000 eye1=0.0000" in said, said)
        expect("report", "c2 post: 0 differing tiles (eye0 much darker in 0, eye1 much darker in 0)" in said, said)
        expect("report", "c3 lone: missing an eye, skipped" in said, said)
        expect("report", "c4 ui: 1 differing tiles (eye0 much darker in 1, eye1 much darker in 0)" in said
               and "    tile (0,0)  eye0=0.0000 eye1=1.0000" in said, said)
        expect("report", "c5 mid: 1 differing tiles (eye0 much darker in 0, eye1 much darker in 0)" in said
               and "    tile (0,0)  eye0=1.0000 eye1=0.8750" in said and "tile (1,0)" not in said.split("c5 mid")[1], said)
        # The pairs are reported in checkpoint order.
        expect("report", said.index("c1 pre") < said.index("c2 post") < said.index("c3 lone")
               < said.index("c4 ui") < said.index("c5 mid"), said)
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print("diff_eye_dump: self-test FAILED")
        for f in failures:
            print("  " + f.replace("\n", "\n    "))
        return 1
    print("diff_eye_dump: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
