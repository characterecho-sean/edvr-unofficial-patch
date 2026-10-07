#!/usr/bin/env python3
"""The on-foot ship split rig's fixture: patches of two real eye dumps, and the independent arithmetic that checks them.

tools\\on_foot_split_test\\fixture_onfoot.bin holds, for a Steam Explorer Cam dump (eye_090359, v0.18.2-90, the commander walking on
a settlement pad; its ground 1.5 to 10 m away is 53% of the frame) and a Frontier cockpit dump (eye_054804, a ship at rest), 48 x 48
patches of the decision crop: the game's scene depth (SceneZ), the decisions the shader wrote there (D00: path, flags, motion, the
predicted previous depth) and the motion.csv rows of the dump's frames (the camera's rows and the head's, the tangents, the
projection). The rig (tools\\on_foot_split_test) runs the production shader on WARP over each patch as a virtual eye and compares its
output with D, then runs it again with the on-foot split. This tool writes the file and holds it to the arithmetic:

  python tools\\on_foot_split_fixture.py --extract --out tools\\on_foot_split_test\\fixture_onfoot.bin
         --case steam=<eyes dir>:090359:16401 --case frontier-cockpit=<eyes dir>:054804:16707
  python tools\\on_foot_split_fixture.py --self-test [--fixture PATH]    synthetic geometry first, then every stored pixel re-derived
  python tools\\on_foot_split_fixture.py --info [--fixture PATH]

--extract reads the dumps and writes only --out (--dry-run says what it would do and writes nothing). --self-test re-derives each
stored decision from the stored depth and rows with a numpy port of the shader's pixel path (the world/ship split, the head's delta and
the camera's), so the file cannot drift from what the game ran: a patch whose stored path, flags, motion or predicted depth differ from
the port's is a fixture that no longer says what the dump said.

Layout (little endian), version 1:
  header   8s magic "EDVRFOOT", u32 version, u32 cases
  case     24s name, u32 stamp, frame, eyeW, eyeH, u32 crop[4] (the decision crop in eye pixels), f32 split (what the dump ran: 10),
           u32 rows, u32 patches
  row      u32 frame, f32 tanNow[4], tanPrev[4] (l r bottom top, the shader's order), f32 headR[9], headTv[3], camR[9], camTv[3]
           (3x3 row-major; the motion.csv's rows 0..2, columns 0..2), f32 projA, projB, u32 worldOn (camTv.w), u32 depthMotion (headTv.w)
  patch    u32 x0, y0, w, h (crop coordinates), then f32 depth[w*h] (reversed-Z, as SceneZ), u16 flags[w*h] (D's w),
           s16 motion[w*h*2] (D's xy in 1/256 px), f32 zpred[w*h] (D's z)
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
DEFAULT = HERE / "on_foot_split_test" / "fixture_onfoot.bin"
MAGIC = b"EDVRFOOT"
VERSION = 1
P = 48                      # the patches' side, pixels
SPLIT_DUMP = 10.0           # kTemporalShipMetres: what the dumps' shaders ran with
MOTION_SCALE = 256.0        # D's motion in 1/256 px, signed 16 bits: +-127.99 px
CROP = (308, 274, 1400, 1400)   # both dumps' decision crop in the 2016 x 1949 eye
EYE = (2016, 1949)

# The curated patches, in crop coordinates (x0, y0), by case name: boundaries of the ten-metre split on the ground and in the
# cockpit, the near ground, the mountain, a sky/terrain edge, a stretch of the cockpit's dash, the sky.
PATCHES = {
    "steam": [(368, 592), (992, 608), (600, 1340), (400, 950), (60, 800), (500, 380), (840, 144), (1100, 450)],
    "frontier-cockpit": [(704, 832), (64, 1232), (1072, 848), (592, 1296), (0, 900), (100, 100)],
}

CASE_HEAD = struct.Struct("<24sIIIIIIIIfII")
ROW = struct.Struct("<I4f4f9f3f9f3fffII")
PATCH_HEAD = struct.Struct("<IIII")
assert CASE_HEAD.size == 68 and ROW.size == 148


class Row:
    """One motion.csv row (eye 0) as the shader's constants."""

    def __init__(self, frame, tan_now, tan_prev, head_r, head_tv, cam_r, cam_tv, proj_a, proj_b, world_on, depth_motion):
        self.frame = frame
        self.tan_now = np.array(tan_now, dtype=np.float32)
        self.tan_prev = np.array(tan_prev, dtype=np.float32)
        self.head_r = np.array(head_r, dtype=np.float32).reshape(3, 3)
        self.head_tv = np.array(head_tv, dtype=np.float32)
        self.cam_r = np.array(cam_r, dtype=np.float32).reshape(3, 3)
        self.cam_tv = np.array(cam_tv, dtype=np.float32)
        self.proj_a = np.float32(proj_a)
        self.proj_b = np.float32(proj_b)
        self.world_on = int(world_on)
        self.depth_motion = int(depth_motion)

    def pack(self):
        return ROW.pack(self.frame, *self.tan_now, *self.tan_prev, *self.head_r.ravel(), *self.head_tv, *self.cam_r.ravel(), *self.cam_tv,
                        float(self.proj_a), float(self.proj_b), self.world_on, self.depth_motion)

    @staticmethod
    def unpack(buf):
        v = ROW.unpack(buf)
        return Row(v[0], v[1:5], v[5:9], v[9:18], v[18:21], v[21:30], v[30:33], v[33], v[34], v[35], v[36])


class Patch:
    def __init__(self, x0, y0, w, h, depth, flags, motion, zpred):
        self.x0, self.y0, self.w, self.h = x0, y0, w, h
        self.depth = np.asarray(depth, dtype=np.float32).reshape(h, w)
        self.flags = np.asarray(flags, dtype=np.uint16).reshape(h, w)
        self.motion = np.asarray(motion, dtype=np.int16).reshape(h, w, 2)
        self.zpred = np.asarray(zpred, dtype=np.float32).reshape(h, w)


class Case:
    def __init__(self, name, stamp, frame, split, rows, patches):
        self.name, self.stamp, self.frame, self.split = name, stamp, frame, np.float32(split)
        self.rows, self.patches = rows, patches

    def row(self, frame=None):
        for r in self.rows:
            if r.frame == (self.frame if frame is None else frame):
                return r
        raise KeyError("no row for frame %s in %s" % (frame, self.name))


# ---- the file ----------------------------------------------------------------------------------------------------------------
def write_bytes(cases):
    out = bytearray(struct.pack("<8sII", MAGIC, VERSION, len(cases)))
    for c in cases:
        out += CASE_HEAD.pack(c.name.encode("ascii"), c.stamp, c.frame, EYE[0], EYE[1], *CROP, float(c.split), len(c.rows), len(c.patches))
        for r in c.rows:
            out += r.pack()
        for p in c.patches:
            out += PATCH_HEAD.pack(p.x0, p.y0, p.w, p.h)
            out += p.depth.astype("<f4").tobytes() + p.flags.astype("<u2").tobytes() + p.motion.astype("<i2").tobytes() + p.zpred.astype("<f4").tobytes()
    return bytes(out)


def read_bytes(data):
    magic, version, n = struct.unpack_from("<8sII", data, 0)
    if magic != MAGIC or version != VERSION:
        raise ValueError("not an on-foot split fixture of this version")
    off = 16
    cases = []
    for _ in range(n):
        v = CASE_HEAD.unpack_from(data, off)
        off += CASE_HEAD.size
        name = v[0].rstrip(b"\0").decode("ascii")
        stamp, frame, ew, eh = v[1:5]
        crop = tuple(v[5:9])
        if (ew, eh) != EYE or crop != CROP:
            raise ValueError("a case's eye or crop is not the dumps'")
        split, nrows, npatches = v[9], v[10], v[11]
        rows = []
        for _ in range(nrows):
            rows.append(Row.unpack(data[off:off + ROW.size]))
            off += ROW.size
        patches = []
        for _ in range(npatches):
            x0, y0, w, h = PATCH_HEAD.unpack_from(data, off)
            off += PATCH_HEAD.size
            n = w * h
            depth = np.frombuffer(data, "<f4", n, off); off += 4 * n
            flags = np.frombuffer(data, "<u2", n, off); off += 2 * n
            motion = np.frombuffer(data, "<i2", 2 * n, off); off += 4 * n
            zpred = np.frombuffer(data, "<f4", n, off); off += 4 * n
            patches.append(Patch(x0, y0, w, h, depth, flags, motion, zpred))
        cases.append(Case(name, stamp, frame, split, rows, patches))
    if off != len(data):
        raise ValueError("trailing bytes in the fixture")
    return cases


# ---- the arithmetic, independent of the shader's source ---------------------------------------------------------------------
def decide(row, depth, x0, y0, split):
    """The shader's pixel decision (fetchHistoryT's and mv's, transcribed) for the patch's interior, in float64: (interior slice,
    flags & the bits compared, motion px, predicted depth). `depth` is the patch's reversed-Z scene depth, (h, w)."""
    h, w = depth.shape
    d64 = depth.astype(np.float64)
    zr = np.zeros((h - 4, w - 4))
    for oy in (-1, 0, 1):
        for ox in (-1, 0, 1):
            zr = np.maximum(zr, d64[2 + oy:h - 2 + oy, 2 + ox:w - 2 + ox])
    own = d64[2:h - 2, 2:w - 2]
    den = zr - float(row.proj_a)
    far = (zr <= 0) | (den <= 0)
    z = np.where(far, 0.0, float(row.proj_b) / np.where(den > 0, den, 1.0))
    world_on = row.world_on != 0 and split > 0
    world = world_on & (far | (z > split))
    ys, xs = np.mgrid[2:h - 2, 2:w - 2]
    X = (CROP[0] + x0 + xs).astype(np.float64)
    Y = (CROP[1] + y0 + ys).astype(np.float64)
    tn = row.tan_now.astype(np.float64)
    tp = row.tan_prev.astype(np.float64)
    d = np.stack([tn[0] + (X + 0.5) / EYE[0] * (tn[1] - tn[0]), tn[3] - (Y + 0.5) / EYE[1] * (tn[3] - tn[2]), -np.ones_like(X)], -1)
    out = {}
    for name, R, tv in (("head", row.head_r.astype(np.float64), row.head_tv[:3].astype(np.float64)),
                        ("world", row.cam_r.astype(np.float64), row.cam_tv[:3].astype(np.float64))):
        dp = d @ R.T
        dp = np.where(far[..., None], dp, dp * z[..., None] + tv)
        xt = dp[..., 0] / -dp[..., 2]
        yt = dp[..., 1] / -dp[..., 2]
        px = (xt - tp[0]) / (tp[1] - tp[0]) * EYE[0] - 0.5
        py = (tp[3] - yt) / (tp[3] - tp[2]) * EYE[1] - 0.5
        out[name] = (px - X, py - Y, np.where(far, 0.0, -dp[..., 2]), dp[..., 2] < -1e-6)
    sel = lambda i: np.where(world, out["world"][i], out["head"][i])
    path = np.where(world, 2, 1)
    valid = np.where(world, out["world"][3], out["head"][3])
    flags = path | np.where(world_on, 128, 0) | np.where(own > float(row.proj_a), 256, 0) | np.where(valid, 1024, 0)
    return flags.astype(np.int64), sel(0), sel(1), sel(2)


PATH_BITS = 15 | 128 | 256 | 1024


def self_test_synthetic():
    """The port against geometry worked by hand: a camera that moves sideways by 0.1 m, a head that does not."""
    ident = np.eye(3)
    tv_cam = np.array([0.1, 0.0, 0.0])
    row = Row(1, [-1.0, 1.0, -1.0, 1.0], [-1.0, 1.0, -1.0, 1.0], ident, [0, 0, 0], ident, tv_cam, 0.0, 0.025, 1, 1)
    # 48 x 48 patch at the eye's centre-ish; depth 5 m everywhere (reversed-Z 0.005) except the left half 20 m
    depth = np.full((P, P), 0.025 / 5.0, np.float32)
    depth[:, :P // 2] = 0.025 / 20.0
    flags, mx, my, zp = decide(row, depth, 600, 600, 10.0)
    # the right half is 5 m: nearer than ten, the head path: no motion at all (an identity delta and no head translation)
    assert (flags[:, -6:] & 15 == 1).all() and abs(mx[:, -6:]).max() < 1e-9 and abs(my[:, -6:]).max() < 1e-9
    assert (flags[:, :6] & 15 == 2).all()
    # the left half is 20 m, the world path: dp = d * 20 + (0.1, 0, 0), so xt = d.x + 0.005 and the pixel moves by
    # 0.005 / (tan span 2) * 2016 px = 5.04 px in x at the same y
    assert abs(mx[:, :6] - 0.005 / 2.0 * EYE[0]).max() < 1e-5, mx[0, 0]
    assert abs(my[:, :6]).max() < 1e-5
    # on foot the split is a millimetre: the 5 m half takes the world path too, and moves by 0.1 / 5 / 2 * 2016 = 20.16 px
    flags, mx, my, zp = decide(row, depth, 600, 600, 0.001)
    assert (flags & 15 == 2).all()
    assert abs(mx[:, -6:] - 0.1 / 5.0 / 2.0 * EYE[0]).max() < 1e-5
    # the camera's translation is in the predicted depth: 5 m plus the z term (zero here); with z in it, 5 - 0.2 m
    row2 = Row(1, [-1.0, 1.0, -1.0, 1.0], [-1.0, 1.0, -1.0, 1.0], ident, [0, 0, 0], ident, [0, 0, 0.2], 0.0, 0.025, 1, 1)
    flags, mx, my, zp = decide(row2, depth, 600, 600, 0.001)
    # d.z = -1, so dp.z = -z + 0.2 and the predicted depth is z - 0.2
    assert abs(zp[:, -6:] - 4.8).max() < 1e-5 and abs(zp[:, :6] - 19.8).max() < 1e-5
    # a split of zero is the world path OFF: everything on the head path, flag 128 clear
    flags, mx, my, zp = decide(row, depth, 600, 600, 0.0)
    assert (flags & 15 == 1).all() and (flags & 128 == 0).all()
    # no depth at all (far): the world path takes the direction alone
    flags, mx, my, zp = decide(row, np.zeros((P, P), np.float32), 600, 600, 10.0)
    assert (flags & 15 == 2).all() and abs(mx).max() < 1e-9 and (zp == 0).all() and (flags & 256 == 0).all()


def self_test_roundtrip():
    rng = np.random.default_rng(5)
    row = Row(7, [-1.2, 0.9, -1.0, 1.0], [-1.2, 0.9, -1.0, 1.0], np.eye(3), [0, 0, 1e-4], np.eye(3), [0.1, -0.2, 0.3], 0.0, 0.025, 1, 1)
    patch = Patch(8, 16, P, P, rng.random((P, P)), rng.integers(0, 2048, (P, P)), rng.integers(-3000, 3000, (P, P, 2)), rng.random((P, P)))
    case = Case("synthetic", 1, 7, 10.0, [row], [patch])
    back = read_bytes(write_bytes([case]))[0]
    assert back.name == "synthetic" and back.frame == 7 and back.split == np.float32(10.0)
    assert np.array_equal(back.patches[0].depth, patch.depth) and np.array_equal(back.patches[0].motion, patch.motion)
    assert np.array_equal(back.patches[0].flags, patch.flags) and np.array_equal(back.patches[0].zpred, patch.zpred)
    assert back.rows[0].pack() == row.pack()
    for bad in (b"NOTAFIXT" + write_bytes([case])[8:], write_bytes([case])[:-1], write_bytes([case]) + b"x"):
        try:
            read_bytes(bad)
        except (ValueError, struct.error):
            continue
        raise AssertionError("a damaged fixture was accepted")


def check_stored(cases):
    """Every stored decision re-derived from the stored depth and rows."""
    total = near = 0
    worst_motion = worst_z = 0.0
    for c in cases:
        row = c.row()
        for p in c.patches:
            flags, mx, my, zp = decide(row, p.depth, p.x0, p.y0, float(c.split))
            stored = p.flags[2:-2, 2:-2].astype(np.int64)
            sm = p.motion[2:-2, 2:-2].astype(np.float64) / MOTION_SCALE
            sz = p.zpred[2:-2, 2:-2].astype(np.float64)
            sel = ((stored & 15) == 1) | ((stored & 15) == 2)       # the pixels the engine, the holo and the screen do not own
            assert sel.sum() > 0, "%s %s: no path 1 or 2 pixel" % (c.name, (p.x0, p.y0))
            bad = ((stored & PATH_BITS) != flags) & sel
            assert not bad.any(), "%s patch %s: %d pixels' path or flags differ from the arithmetic (first stored %d, derived %d)" % (
                c.name, (p.x0, p.y0), bad.sum(), stored[bad][0], flags[bad][0])
            em = np.hypot(sm[..., 0] - mx, sm[..., 1] - my)[sel]
            worst_motion = max(worst_motion, em.max())
            assert em.max() < 0.004, "%s patch %s: motion off by %.4f px" % (c.name, (p.x0, p.y0), em.max())
            finite = sel & (zp > 0)
            ez = np.abs(sz - zp)[finite]
            if ez.size:
                rel = (ez / zp[finite]).max()
                worst_z = max(worst_z, rel)
                assert rel < 2e-4, "%s patch %s: predicted depth off by %.2e (relative)" % (c.name, (p.x0, p.y0), rel)
            total += sel.sum()
            near += ((stored & 15) == 1).sum()
    return total, near, worst_motion, worst_z


def self_test(path):
    self_test_synthetic()
    self_test_roundtrip()
    print("on_foot_split_fixture: synthetic geometry and the file's round trip OK")
    if not path.exists():
        print("on_foot_split_fixture: no fixture at %s; --extract writes it (the stored patches are checked when it exists)" % path)
        return
    cases = read_bytes(path.read_bytes())
    names = [c.name for c in cases]
    assert "steam" in names and "frontier-cockpit" in names, "the fixture holds the Steam and the Frontier cases: %s" % names
    total, near, wm, wz = check_stored(cases)
    steam = next(c for c in cases if c.name == "steam")
    # the Steam dump is the bug: its path-1 pixels are the ground nearer than ten metres, with no cockpit stencil anywhere
    s_near = sum(int((p.flags & 15 == 1).sum()) for p in steam.patches)
    s_all = sum(int(((p.flags & 15 == 1) | (p.flags & 15 == 2)).sum()) for p in steam.patches)
    assert s_near > 4000, "the Steam patches hold the near ground (path 1): %d pixels" % s_near
    assert steam.row().world_on == 1 and np.linalg.norm(steam.row().cam_tv[:3]) > 0.01 > np.linalg.norm(steam.row().head_tv[:3]), \
        "the Steam frame's camera moved and its head did not"
    cock = next(c for c in cases if c.name == "frontier-cockpit")
    c_near = sum(int((p.flags & 15 == 1).sum()) for p in cock.patches)
    assert c_near > 3000, "the cockpit patches hold the cockpit (path 1): %d pixels" % c_near
    print("on_foot_split_fixture: self-test passed: %d cases, %d patches, %d pixels re-derived from their stored depth and rows (%d on "
          "path 1), worst motion difference %.5f px, worst predicted-depth difference %.1e" % (
              len(cases), sum(len(c.patches) for c in cases), total, near, wm, wz))
    print("on_foot_split_fixture: Steam: %d of %d stored path 1/2 pixels are nearer than ten metres" % (s_near, s_all))


# ---- extraction -------------------------------------------------------------------------------------------------------------
def read_scenez(path):
    b = path.read_bytes()
    if b[:8] != b"EDVRTEX1":
        raise ValueError("%s is not an EDVR texture dump" % path)
    ver, w, h, fmt, row, scene, eye = struct.unpack_from("<7I", b, 8)
    if (w, h) != EYE or row != w * 8:
        raise ValueError("%s: expected a %dx%d depth/stencil dump" % (path, EYE[0], EYE[1]))
    raw = np.frombuffer(b, np.uint8, w * h * 8, 44).reshape(h, w, 8)
    return raw[:, :, :4].copy().view("<f4").reshape(h, w), scene


def read_decisions(path):
    b = path.read_bytes()
    if b[:8] != b"EDVRTEX1":
        raise ValueError("%s is not an EDVR texture dump" % path)
    ver, w, h, fmt, row, scene, eye = struct.unpack_from("<7I", b, 8)
    if (w, h) != (CROP[2], CROP[3]) or row != w * 16:
        raise ValueError("%s: expected the %dx%d decision crop" % (path, CROP[2], CROP[3]))
    return np.frombuffer(b, "<f4", w * h * 4, 44).reshape(h, w, 4), scene


def read_rows(path):
    rows = {}
    for r in csv.DictReader(open(path, newline="")):
        if r["eye"] != "0":
            continue
        f = lambda n, k: [float(r["%s%d" % (n, i)]) for i in range(k)]
        cam, head = f("cameraR", 12), f("headR", 12)
        ctv, htv = f("cameraTv", 4), f("headTv", 4)
        pick = (0, 1, 2, 4, 5, 6, 8, 9, 10)          # the 3x3 of each 3x4 row-major block
        rows[int(r["frame"])] = Row(int(r["frame"]), f("tanNow", 4), f("tanPrev", 4), [head[i] for i in pick], htv[:3],
                                    [cam[i] for i in pick], ctv[:3], float(r["projectionA"]), float(r["projectionB"]),
                                    int(ctv[3] != 0), int(htv[3] != 0))
    return rows


def extract_case(name, folder, stamp, frame):
    base = Path(folder) / ("eye_%s_" % stamp)
    depth, scene = read_scenez(Path(str(base) + "SceneZ.bin"))
    dec, dscene = read_decisions(Path(str(base) + "D00.bin"))
    if scene != frame or dscene != frame:
        raise ValueError("%s: SceneZ is frame %d and D00 frame %d, not %d" % (name, scene, dscene, frame))
    rows = read_rows(Path(str(base) + "motion.csv"))
    if frame not in rows:
        raise ValueError("%s: motion.csv has no row for frame %d" % (name, frame))
    # the rows of the dump's own frames (Steam: all sixteen, the walking camera at its quick and its quiet), in frame order
    keep = [rows[f] for f in sorted(rows) if f < frame + 16]
    patches = []
    for x0, y0 in PATCHES[name]:
        dz = depth[CROP[1] + y0:CROP[1] + y0 + P, CROP[0] + x0:CROP[0] + x0 + P]
        d = dec[y0:y0 + P, x0:x0 + P]
        motion = np.rint(d[:, :, :2] * MOTION_SCALE)
        if np.abs(motion).max() >= 32767:
            raise ValueError("%s patch %s: motion past +-127 px does not fit the fixture" % (name, (x0, y0)))
        flags = d[:, :, 3]
        if not np.array_equal(flags, np.rint(flags)) or flags.max() >= 65536:
            raise ValueError("%s patch %s: flags are not 16-bit integers" % (name, (x0, y0)))
        patches.append(Patch(x0, y0, P, P, dz, flags.astype(np.uint16), motion.astype(np.int16), d[:, :, 2]))
    return Case(name, int(stamp), frame, SPLIT_DUMP, keep, patches)


def info(path):
    cases = read_bytes(path.read_bytes())
    print("%s: %d bytes, %d cases" % (path, path.stat().st_size, len(cases)))
    for c in cases:
        px = sum(p.w * p.h for p in c.patches)
        print("  %-18s stamp %06d frame %d split %g: %d rows (frames %d..%d), %d patches, %d pixels" % (
            c.name, c.stamp, c.frame, c.split, len(c.rows), c.rows[0].frame, c.rows[-1].frame, len(c.patches), px))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", action="store_true")
    ap.add_argument("--case", action="append", default=[], metavar="NAME=DIR:STAMP:FRAME")
    ap.add_argument("--out", default=str(DEFAULT))
    ap.add_argument("--fixture", default=str(DEFAULT))
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="with --extract: say what would be written and write nothing")
    a = ap.parse_args(argv)
    if a.self_test:
        self_test(Path(a.fixture))
        return 0
    if a.info:
        info(Path(a.fixture))
        return 0
    if a.extract:
        cases = []
        for spec in a.case:
            name, rest = spec.split("=", 1)
            folder, stamp, frame = rest.rsplit(":", 2)
            if name not in PATCHES:
                raise SystemExit("unknown case %s (known: %s)" % (name, ", ".join(sorted(PATCHES))))
            cases.append(extract_case(name, folder, stamp, int(frame)))
        if not cases:
            raise SystemExit("--extract needs at least one --case")
        blob = write_bytes(cases)
        read_bytes(blob)
        if a.dry_run:
            print("would write %d bytes (%d cases) to %s" % (len(blob), len(cases), a.out))
            return 0
        Path(a.out).write_bytes(blob)
        print("on_foot_split_fixture: %d cases, %d patches, %d bytes -> %s" % (len(cases), sum(len(c.patches) for c in cases), len(blob), a.out))
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
