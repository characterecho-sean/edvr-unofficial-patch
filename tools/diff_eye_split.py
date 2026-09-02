#!/usr/bin/env python3
"""Compare the eye-split dump: for every stage of one frame, register the two
eyes to each other, then tile both images and report where they differ.

Usage: python tools/diff_eye_split.py <edvr_logs/dumps directory> [gfx log]
       python tools/diff_eye_split.py --no-register <dir> [gfx log]
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

WHY THE EYES ARE REGISTERED FIRST

The two eye projections are off-centre by different amounts, so content at
infinity does not land on the same pixel in both. At a 2517 px eye width
that constant offset is about 365 px, which is 91 px in the 4x-decimated
files this reads. Comparing tile (x,y) of one eye against tile (x,y) of the
other therefore compares a planet against empty sky and calls the result a
difference.

That is not hypothetical. On 2026-09-01 the unregistered tool reported a
"7% blue overlay in the healthy eye" of a DSS scan, and a fix was built on
the measurement. The tiles had landed on the Milky Way band. There was no
overlay.

So before tiling, this measures how far one eye's content sits from the
other's and moves eye1 by that vector. The measurement comes from the R32
linear-depth target when the dump has one (format 39, metres, with the sky
written as 1e17): the finite-depth region of a far body is the same shape
in both eyes even when one eye's colour is black, so the offset between the
two centroids is the vector, and it is checked by how well the two masks
then overlap. Without a depth target it falls back to cross-correlating
luminance profiles, which is the weaker measurement because the very bug
being hunted takes brightness out of one eye. --no-register restores the
old pixel-for-pixel behaviour, which is worth having only to reproduce an
old report.

Registration also needs the two files to BE two eyes, and where a shape
carries more than two targets that is a guess -- the pairing goes by order
of first sighting, which cannot tell a post chain's several buffers from
the second eye's copy. Moving two buffers of one eye apart by the eye
offset would invent a difference exactly as large as the one this fix
removes, so each stage is measured both ways and a stage that agrees better
unmoved is reported unmoved and labelled NOT A STEREO PAIR.

WHAT COUNTS AS A DIFFERENCE

The two eyes see the world from 6 cm apart, so they are SUPPOSED to differ,
and near geometry differs a lot -- registration lines up the far field, not
the near. This reports the shape of the difference rather than its
presence: a stereo disparity moves content sideways by a few tiles and
leaves the total brightness alone, while a missing body removes brightness
from a region and puts none back. The summary gives both the tile-
difference count and the signed brightness balance, and a body missing from
one eye shows as a strong one-sided imbalance that ordinary parallax never
produces.
"""

import contextlib
import io
import os
import re
import shutil
import struct
import sys
import tempfile

TILE = 16

# The linear-depth targets: one 32-bit float a texel holding metres, with the
# sky written as a value so large nothing real reaches it.
DEPTH_FMTS = (39, 40, 41)
FAR_METRES = 1e16

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


def tile_map(path, w, h, fmt, shift=(0, 0)):
    """Mean luminance per 16x16 tile, and which tiles are wholly inside the
    image once the registration shift is applied. Samples every 4th texel
    across and every row, which is plenty to see a body-sized region and
    keeps a 22 MB file readable in a second.

    shift is (dy, dx): how far this image's content has to move to land on
    the other eye's. The output tile at (Y, X) therefore reads the source at
    (Y-dy, X-dx). Tiles that would read from outside the source are marked
    invalid rather than filled with black, because inventing black along one
    edge is exactly the one-sided imbalance this tool exists to detect."""
    dy, dx = shift
    bpt, luma = picker(fmt)
    tw, th = w // TILE, h // TILE
    tiles = [[0.0] * tw for _ in range(th)]
    valid = [[True] * tw for _ in range(th)]
    unpack = {1: "<%dB", 2: "<%dH", 4: "<%dI"}.get(bpt, "<%dI")
    rowbytes = w * bpt
    xs = list(range(0, TILE, 4))
    with open(path, "rb") as f:
        for ty in range(th):
            block = []
            for yy in range(TILE):
                sy = ty * TILE + yy - dy
                if not 0 <= sy < h:
                    block.append(None)
                    continue
                f.seek(sy * rowbytes)
                raw = f.read(rowbytes)
                block.append(struct.unpack(unpack % w, raw)
                             if len(raw) == rowbytes else None)
            for tx in range(tw):
                acc = 0.0
                n = 0
                bad = False
                for row in block:
                    if row is None:
                        bad = True
                        continue
                    for xx in xs:
                        sx = tx * TILE + xx - dx
                        if 0 <= sx < w:
                            acc += luma(row[sx])
                            n += 1
                        else:
                            bad = True
                tiles[ty][tx] = acc / n if n else 0.0
                valid[ty][tx] = not bad
    return tiles, valid


def depth_mask(path, w, h):
    """One byte a texel: 1 where the linear-depth target holds a real
    distance, 0 where it holds the far value that stands for sky."""
    rows = []
    rowbytes = w * 4
    with open(path, "rb") as f:
        for _ in range(h):
            raw = f.read(rowbytes)
            if len(raw) != rowbytes:
                rows.append(bytearray(w))
                continue
            rows.append(bytearray(
                1 if 0.0 < v < FAR_METRES else 0
                for v in struct.unpack("<%df" % w, raw)))
    return rows


def mask_centroid(rows, w, h):
    """(count, cy, cx) of the marked texels."""
    n = 0
    sy = 0.0
    sx = 0.0
    for y in range(h):
        r = rows[y]
        c = sum(r)
        if not c:
            continue
        n += c
        sy += c * y
        for x in range(w):
            if r[x]:
                sx += x
    if not n:
        return 0, 0.0, 0.0
    return n, sy / n, sx / n


def mask_overlap(a, b, w, h, dy, dx):
    """Of b's marked texels, the fraction landing on a marked texel of a
    once moved by (dy, dx). Two views of the same far body register at well
    over 0.9; a poor score means the two centroids were pulled by different
    content in each eye and the vector cannot be trusted."""
    hit = 0
    tot = 0
    for y in range(h):
        rb = b[y]
        c = sum(rb)
        if not c:
            continue
        tot += c
        ty = y + dy
        if not 0 <= ty < h:
            continue
        ra = a[ty]
        for x in range(w):
            if rb[x]:
                tx = x + dx
                if 0 <= tx < w and ra[tx]:
                    hit += 1
    return hit / tot if tot else 0.0


def luma_profiles(path, w, h, fmt):
    """Mean luminance down each column and across each row."""
    bpt, luma = picker(fmt)
    cols = [0.0] * w
    rows = [0.0] * h
    unpack = {1: "<%dB", 2: "<%dH", 4: "<%dI"}.get(bpt, "<%dI")
    rowbytes = w * bpt
    with open(path, "rb") as f:
        for y in range(h):
            raw = f.read(rowbytes)
            if len(raw) != rowbytes:
                break
            vals = struct.unpack(unpack % w, raw)
            acc = 0.0
            for x in range(w):
                v = luma(vals[x])
                cols[x] += v
                acc += v
            rows[y] = acc / w
    return [c / h for c in cols], rows


def best_shift(pa, pb, span):
    """The shift s of profile pb that best lines it up with pa -- pa[i+s]
    against pb[i] -- by zero-mean normalised correlation, with the peak
    value so a weak match can be seen for what it is. Correlating a profile
    rather than the whole image is what keeps this cheap, and it is sound
    because the offset being measured is a rigid translation."""
    n = min(len(pa), len(pb))
    best = (0, -2.0)
    floor = n // 4
    for s in range(-span, span + 1):
        lo = max(0, -s)
        hi = min(n, n - s)
        if hi - lo < floor:
            continue
        m = hi - lo
        ma = sum(pa[i + s] for i in range(lo, hi)) / m
        mb = sum(pb[i] for i in range(lo, hi)) / m
        num = 0.0
        da2 = 0.0
        db2 = 0.0
        for i in range(lo, hi):
            u = pa[i + s] - ma
            v = pb[i] - mb
            num += u * v
            da2 += u * u
            db2 += v * v
        if da2 <= 0.0 or db2 <= 0.0:
            continue
        r = num / (da2 * db2) ** 0.5
        if r > best[1]:
            best = (s, r)
    return best


def pair(files):
    """The two images of a shape to compare, by lowest index. eye_split
    numbers them from zero in the order the frame first touched each target,
    so the two lowest are the earliest pair -- and a dump that dropped a
    target still reports instead of failing to open eye0."""
    lo = sorted(files)[:2]
    return files[lo[0]], files[lo[1]]


def registration(groups):
    """How far eye1's content has to move to land on eye0's, in texels of
    the shape it was measured at. Returns (dy, dx, (w, h), note), or None
    when nothing in the dump can carry the measurement."""
    depth = sorted((k for k in groups
                    if k[2] in DEPTH_FMTS and len(groups[k]) >= 2),
                   key=lambda k: -(k[0] * k[1]))
    for w, h, fmt in depth:
        f0, f1 = pair(groups[(w, h, fmt)])
        ma = depth_mask(f0, w, h)
        mb = depth_mask(f1, w, h)
        na, cya, cxa = mask_centroid(ma, w, h)
        nb, cyb, cxb = mask_centroid(mb, w, h)
        area = float(w * h)
        if not na or not nb:
            continue
        # A mask covering most of the frame is a planet surface or a
        # cockpit, not a far body against sky, and its centroid sits near
        # the frame centre in both eyes however the content moved. A mask of
        # a few texels is noise. Neither registers anything.
        if not 2e-4 < na / area < 0.4 or not 2e-4 < nb / area < 0.4:
            continue
        if min(na, nb) < 0.6 * max(na, nb):
            continue
        dy = int(round(cya - cyb))
        dx = int(round(cxa - cxb))
        ov = mask_overlap(ma, mb, w, h, dy, dx)
        if ov < 0.7:
            continue
        return dy, dx, (w, h), (
            "the depth target %dx%d fmt=%d: %d finite texels, %.0f%% "
            "overlapping once moved"
            % (w, h, fmt, min(na, nb), 100.0 * ov))

    # No usable depth target: line the eyes up on brightness instead. This
    # is the weaker measurement -- the very bug being hunted takes
    # brightness out of one eye -- so the correlation peak is reported and a
    # feeble one is refused.
    colour = sorted((k for k in groups
                     if k[2] not in DEPTH_FMTS and len(groups[k]) >= 2),
                    key=lambda k: (-(k[0] * k[1]), k[2]))
    best = None
    for w, h, fmt in colour[:3]:
        f0, f1 = pair(groups[(w, h, fmt)])
        ca, ra = luma_profiles(f0, w, h, fmt)
        cb, rb = luma_profiles(f1, w, h, fmt)
        dx, rx = best_shift(ca, cb, max(4, w // 3))
        dy, ry = best_shift(ra, rb, max(4, h // 8))
        if ry < 0.5:
            dy = 0
        cand = (rx, dy, dx, (w, h),
                "brightness on %dx%d fmt=%d, correlation %.2f across and "
                "%.2f down" % (w, h, fmt, rx, ry))
        if best is None or rx > best[0]:
            best = cand
        if rx >= 0.6:
            break
    if best and best[0] >= 0.35:
        return best[1], best[2], best[3], best[4]
    return None


def scale_shift(reg, w, h):
    """The registration vector in another stage's texels. The offset comes
    from the projection, so it scales with the size of the target."""
    if reg is None:
        return 0, 0
    dy, dx, (rw, rh), _ = reg
    return int(round(dy * h / float(rh))), int(round(dx * w / float(rw)))


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


def report(dirname, logpath=None, register=True):
    groups = scan(dirname)
    if not groups:
        print("no eyesplit_*.bin files in %s" % dirname)
        return 1
    order = stage_order(logpath)
    keys = sorted(groups, key=lambda k: order.get(k, (999, {}))[0])

    print("eye-split comparison: %d stage(s) in %s\n" % (len(keys), dirname))

    # The vector goes in the header at length and again in one line at the
    # end, because the end is where the verdict is read and a comparison
    # made at the wrong offset has been believed before.
    reg = registration(groups) if register else None
    if not register:
        print("REGISTRATION OFF (--no-register): tiles are compared at "
              "identical pixel")
        print("  positions, which reads a far body as missing from whichever "
              "eye's projection")
        print("  did not put it on the same pixel. Every number below "
              "inherits that.\n")
        regline = ("Registration was off: every number above is a "
                   "pixel-for-pixel comparison.")
    elif reg is None:
        print("REGISTRATION FAILED: nothing in this dump could line the eyes "
              "up, so tiles are")
        print("  compared at identical pixel positions and far content reads "
              "as a difference")
        print("  it is not. Treat every number below as an upper bound.\n")
        regline = ("Registration failed: every number above is a "
                   "pixel-for-pixel comparison.")
    else:
        dy, dx, _, note = reg
        print("REGISTRATION: eye1 moved dy=%+d dx=%+d px to land on eye0, "
              "measured from" % (dy, dx))
        print("  %s.\n" % note)
        regline = ("Registered: eye1 moved dy=%+d dx=%+d px onto eye0."
                   % (dy, dx))

    verdicts = []
    unpaired = []
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

        f0, f1 = pair(files)
        sdy, sdx = scale_shift(reg, w, h)
        a, av = tile_map(f0, w, h, fmt)
        b, bv = tile_map(f1, w, h, fmt, (sdy, sdx))
        th_, tw_ = len(a), len(a[0])

        # A tile counts as differing when it moves by more than this
        # fraction of the frame's own mean brightness -- relative, so an HDR
        # stage and an 8-bit stage are judged on the same footing. Tiles the
        # shift pushed off an edge take no part in any of it.
        cells = [(y, x) for y in range(th_) for x in range(tw_)
                 if av[y][x] and bv[y][x]]
        if not cells:
            print("%-34s the registration shift leaves no overlap -- skipped"
                  % label)
            continue

        # Registration assumes the two files are one scene seen from two
        # eyes. Where a shape has more than two targets that is a guess, and
        # it can be wrong: two buffers of ONE eye's post chain hold the same
        # image, and moving one of them by the eye offset invents a
        # difference where there was none. So check it. If moving the image
        # makes the two agree LESS, they were never a stereo pair, and the
        # honest comparison is the unmoved one.
        paired = True
        if sdy or sdx:
            raw, rv = tile_map(f1, w, h, fmt)
            mad = sum(abs(a[y][x] - b[y][x]) for y, x in cells) / len(cells)
            mad_raw = sum(abs(a[y][x] - raw[y][x])
                          for y, x in cells) / len(cells)
            if mad > 1.15 * mad_raw:
                paired = False
                unpaired.append(label.strip())
                b, bv = raw, rv
                cells = [(y, x) for y in range(th_) for x in range(tw_)]

        mean = sum(a[y][x] for y, x in cells) / len(cells) or 1e-6
        thresh = 0.25 * mean
        diff = 0
        signed = 0.0
        worst = (0.0, 0, 0)
        for y, x in cells:
            d = a[y][x] - b[y][x]
            if abs(d) > thresh:
                diff += 1
            signed += d
            if abs(d) > abs(worst[0]):
                worst = (d, x, y)
        pct = 100.0 * diff / len(cells)
        bal = signed / len(cells) / mean
        note = ""
        if draws:
            note = "  draws %s" % "/".join(
                str(draws[k]) for k in sorted(draws))
        if len(cells) < th_ * tw_:
            note += "  [%d/%d tiles]" % (len(cells), th_ * tw_)
        if not paired:
            note += "  NOT A STEREO PAIR"
        print("%-34s tiles differing %5.1f%%   balance %+6.2f   "
              "worst %+.4f at tile (%d,%d)%s"
              % (label, pct, bal, worst[0], worst[1], worst[2], note))
        verdicts.append((st if st is not None else 999, label, pct, bal))

    print()
    print(regline)
    if unpaired:
        print("NOT A STEREO PAIR: %s." % ", ".join(unpaired))
        print("  Moving one of those onto the other made them agree less, so "
              "they are two")
        print("  buffers of one eye rather than one buffer of each, and they "
              "are compared")
        print("  unmoved. A shape carrying more than two targets is where "
              "this happens: the")
        print("  pairing goes by order of first sighting, which cannot tell "
              "a post chain's")
        print("  buffers apart from the second eye's.")
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


def _write_depth(path, w, h, discs):
    """Discs of finite depth on a far-value ground, the way the linear-depth
    target writes a body against sky. discs is a list of (cx, cy, r)."""
    near = struct.pack("<f", 3.5e6)
    far = struct.pack("<f", 1e17)
    with open(path, "wb") as f:
        for y in range(h):
            row = bytearray()
            for x in range(w):
                on = any((x - cx) ** 2 + (y - cy) ** 2 <= r * r
                         for cx, cy, r in discs)
                row += near if on else far
            f.write(row)


def _pattern(u, v):
    """Two blocks of different size and brightness -- structure a
    translation cannot be mistaken for, unlike anything periodic."""
    if 40 <= u < 90 and 30 <= v < 80:
        return 220
    if 120 <= u < 140 and 60 <= v < 70:
        return 140
    return 10


def _write_bars(path, w, h, ox, seed):
    """Vertical bars moved by ox, under a faint horizontal wash picked by
    seed. The bars give a strong column profile; the wash is what makes two
    such images disagree down the rows, which is the case the row
    correlation has to refuse rather than answer."""
    with open(path, "wb") as f:
        for y in range(h):
            wash = ((y * seed) % 97) * 0.2 / 97.0
            row = bytearray()
            for x in range(w):
                u = x - ox
                bar = ((u * 37) % 101) / 101.0 if 0 <= u < w else 0.0
                g = int(255 * min(1.0, 0.8 * bar + wash))
                row += bytes((g, g, g, 255))
            f.write(row)


def _write_rgba8(path, w, h, ox, oy):
    """The pattern drawn with its origin moved to (ox, oy)."""
    with open(path, "wb") as f:
        for y in range(h):
            row = bytearray()
            for x in range(w):
                u, v = x - ox, y - oy
                g = _pattern(u, v) if 0 <= u < w and 0 <= v < h else 0
                row += bytes((g, g, g, 255))
            f.write(row)


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

    # The primitive the brightness fallback rests on: a real shift comes
    # back exactly and at full confidence, while profiles that share nothing
    # produce a nonsense shift at a confidence low enough to be refused.
    prof = [((i * 37) % 101) / 101.0 for i in range(200)]
    moved = [prof[i + 23] if i + 23 < 200 else 0.0 for i in range(200)]
    got = best_shift(prof, moved, 200 // 3)
    if got[0] != 23 or got[1] < 0.99:
        print("best_shift on a known 23 shift = %s, want (23, ~1.0)" % (got,))
        ok = False
    junk = best_shift(prof, [((i * 53) % 97) / 97.0 for i in range(200)],
                      200 // 3)
    if junk[1] >= 0.5:
        print("best_shift scored unrelated profiles %.2f, high enough to be "
              "believed" % junk[1])
        ok = False
    if best_shift(prof, [0.5] * 200, 200 // 3)[1] > -1.0:
        print("best_shift scored a flat profile instead of refusing it")
        ok = False

    # Registration, against a synthetic offset. The whole point of the tool
    # is that this vector is right and applied the right way round, so the
    # test builds a dump whose answer is known and checks both.
    w, h = 192, 160
    dy, dx = -7, 29          # what eye1 must move by to land on eye0
    tmp = tempfile.mkdtemp(prefix="eyesplit_selftest_")
    try:
        # Depth route: one disc, drawn where each eye's projection puts it.
        # eye1's copy sits at (cy-dy, cx-dx).
        _write_depth(os.path.join(tmp, "eyesplit_%dx%df39_eye0.bin" % (w, h)),
                     w, h, [(70, 80, 20)])
        _write_depth(os.path.join(tmp, "eyesplit_%dx%df39_eye1.bin" % (w, h)),
                     w, h, [(70 - dx, 80 - dy, 20)])
        reg = registration(scan(tmp))
        if reg is None or (reg[0], reg[1]) != (dy, dx):
            print("depth registration = %s, want (%d, %d)"
                  % (None if reg is None else (reg[0], reg[1]), dy, dx))
            ok = False
        elif "depth target" not in reg[3]:
            print("depth registration did not say it came from depth: %s"
                  % reg[3])
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    tmp = tempfile.mkdtemp(prefix="eyesplit_selftest_")
    try:
        # Brightness route: the same offset, in a dump with no depth target
        # at all.
        p0 = os.path.join(tmp, "eyesplit_%dx%df27_eye0.bin" % (w, h))
        p1 = os.path.join(tmp, "eyesplit_%dx%df27_eye1.bin" % (w, h))
        _write_rgba8(p0, w, h, 0, 0)
        _write_rgba8(p1, w, h, -dx, -dy)
        reg = registration(scan(tmp))
        if reg is None or (reg[0], reg[1]) != (dy, dx):
            print("brightness registration = %s, want (%d, %d)"
                  % (None if reg is None else (reg[0], reg[1]), dy, dx))
            ok = False
        elif "brightness" not in reg[3]:
            print("brightness registration did not say where it came from: "
                  "%s" % reg[3])
            ok = False

        # And the shift has to be applied in the direction that CANCELS the
        # offset. Getting the sign backwards doubles it, and the tile counts
        # would still look plausible, so measure it: unregistered the two
        # images disagree over many tiles, registered over none.
        a, _ = tile_map(p0, w, h, 27)
        raw, _ = tile_map(p1, w, h, 27)
        fixed, valid = tile_map(p1, w, h, 27, (dy, dx))
        n_raw = sum(1 for y in range(len(a)) for x in range(len(a[0]))
                    if abs(a[y][x] - raw[y][x]) > 0.02)
        n_fix = sum(1 for y in range(len(a)) for x in range(len(a[0]))
                    if valid[y][x] and abs(a[y][x] - fixed[y][x]) > 0.02)
        if n_raw < 20:
            print("the synthetic offset is too small to test: only %d tiles "
                  "differ unregistered" % n_raw)
            ok = False
        if n_fix:
            print("%d tiles still differ after registration, want 0" % n_fix)
            ok = False
        # The tiles the shift pushed off an edge must be excluded, not
        # counted as black.
        dropped = sum(1 for r in valid for v in r if not v)
        if dropped < (abs(dx) // TILE) * len(valid):
            print("only %d tiles marked invalid after a %d px shift"
                  % (dropped, dx))
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # Two centroids can be lined up perfectly and still mean nothing, when
    # each eye's depth holds different content -- a cockpit in one, a body
    # in both. One disc against two half-size ones has centroids that
    # register and masks that then overlap nowhere, and the tool has to
    # notice and go to brightness instead of answering with the vector it
    # just computed.
    tmp = tempfile.mkdtemp(prefix="eyesplit_selftest_")
    try:
        _write_depth(os.path.join(tmp, "eyesplit_%dx%df39_eye0.bin" % (w, h)),
                     w, h, [(70, 80, 20)])
        _write_depth(os.path.join(tmp, "eyesplit_%dx%df39_eye1.bin" % (w, h)),
                     w, h, [(40, 80, 14), (140, 80, 14)])
        _write_rgba8(os.path.join(tmp, "eyesplit_%dx%df27_eye0.bin" % (w, h)),
                     w, h, 0, 0)
        _write_rgba8(os.path.join(tmp, "eyesplit_%dx%df27_eye1.bin" % (w, h)),
                     w, h, -dx, -dy)
        reg = registration(scan(tmp))
        if reg is None or (reg[0], reg[1]) != (dy, dx):
            print("registration on depth masks that do not match = %s, want "
                  "the brightness answer (%d, %d)"
                  % (None if reg is None else (reg[0], reg[1]), dy, dx))
            ok = False
        elif "brightness" not in reg[3]:
            print("a depth registration whose masks overlap nowhere was "
                  "believed: %s" % reg[3])
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # A weak row correlation must be refused, not answered. Two images that
    # agree across but not down are the case: a vertical shift invented from
    # noise would blank whole tile rows and read as a body gone missing.
    tmp = tempfile.mkdtemp(prefix="eyesplit_selftest_")
    try:
        _write_bars(os.path.join(tmp, "eyesplit_%dx%df27_eye0.bin" % (w, h)),
                    w, h, 0, 37)
        _write_bars(os.path.join(tmp, "eyesplit_%dx%df27_eye1.bin" % (w, h)),
                    w, h, -dx, 53)
        reg = registration(scan(tmp))
        if reg is None or (reg[0], reg[1]) != (0, dx):
            print("registration on rows that do not correlate = %s, want "
                  "(0, %d)"
                  % (None if reg is None else (reg[0], reg[1]), dx))
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # Depth that fills the frame -- a planet surface, a cockpit -- is the
    # case no later check can rescue. Both centroids sit at the frame centre
    # however far the content actually moved, so the vector comes out zero
    # and the masks overlap perfectly, and the overlap test waves it
    # through. Only the area of the mask gives it away.
    tmp = tempfile.mkdtemp(prefix="eyesplit_selftest_")
    try:
        for eye in (0, 1):
            _write_depth(os.path.join(
                tmp, "eyesplit_%dx%df39_eye%d.bin" % (w, h, eye)),
                w, h, [(w // 2, h // 2, w)])
        _write_rgba8(os.path.join(tmp, "eyesplit_%dx%df27_eye0.bin" % (w, h)),
                     w, h, 0, 0)
        _write_rgba8(os.path.join(tmp, "eyesplit_%dx%df27_eye1.bin" % (w, h)),
                     w, h, -dx, -dy)
        reg = registration(scan(tmp))
        if reg is None or (reg[0], reg[1]) != (dy, dx):
            print("registration on frame-filling depth = %s, want the "
                  "brightness answer (%d, %d)"
                  % (None if reg is None else (reg[0], reg[1]), dy, dx))
            ok = False
        elif "brightness" not in reg[3]:
            print("frame-filling depth was read as a body and registered to "
                  "zero: %s" % reg[3])
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # And when nothing correlates, the answer is "failed", not a number.
    # Every shift scores something; returning the best of a bad field would
    # move one eye by a made-up amount and put the tool right back where it
    # started.
    tmp = tempfile.mkdtemp(prefix="eyesplit_selftest_")
    try:
        _write_bars(os.path.join(tmp, "eyesplit_%dx%df27_eye0.bin" % (w, h)),
                    w, h, 0, 37)
        _write_rgba8(os.path.join(tmp, "eyesplit_%dx%df27_eye1.bin" % (w, h)),
                     w, h, 0, 0)
        reg = registration(scan(tmp))
        if reg is not None:
            print("registration answered %s for two images with nothing in "
                  "common" % ((reg[0], reg[1]),))
            ok = False
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            report(tmp)
        if "REGISTRATION FAILED" not in buf.getvalue():
            print("the report did not say registration had failed:\n%s"
                  % buf.getvalue())
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # A shape carrying more than two targets is paired by guesswork, and
    # registering two buffers of one eye would invent a difference the size
    # of the eye offset. The report has to catch that and say so.
    tmp = tempfile.mkdtemp(prefix="eyesplit_selftest_")
    try:
        _write_depth(os.path.join(tmp, "eyesplit_%dx%df39_eye0.bin" % (w, h)),
                     w, h, [(70, 80, 20)])
        _write_depth(os.path.join(tmp, "eyesplit_%dx%df39_eye1.bin" % (w, h)),
                     w, h, [(70 - dx, 80 - dy, 20)])
        for eye in range(3):       # three buffers of ONE eye: same image
            _write_rgba8(os.path.join(
                tmp, "eyesplit_%dx%df27_eye%d.bin" % (w, h, eye)), w, h, 0, 0)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            report(tmp)
        out = buf.getvalue()
        if "dy=%+d dx=%+d" % (dy, dx) not in out:
            print("the report did not state the registration vector:\n%s"
                  % out)
            ok = False
        if "NOT A STEREO PAIR" not in out:
            print("the report registered two buffers of one eye without "
                  "noticing:\n%s" % out)
            ok = False
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            report(tmp, None, False)
        if "REGISTRATION OFF" not in buf.getvalue():
            print("--no-register did not say it was off:\n%s"
                  % buf.getvalue())
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--self-test" in args:
        sys.exit(self_test())
    register = "--no-register" not in args
    args = [a for a in args if a != "--no-register"]
    if not args:
        print(__doc__)
        sys.exit(1)
    sys.exit(report(args[0], args[1] if len(args) > 1 else None, register))
