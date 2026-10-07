#!/usr/bin/env python3
"""layer_fixtures.py -- the fixtures of the supercruise layer rigs, made from the Frontier install's dumps.

docs/ui-layer-2026-09-23.md, "2026-10-07: orbit lines, supercruise bars and space dust in the layer". The rigs run on files in the
repo, never on the install (a build machine has none); this tool is how those files were made, so they can be made again from a newer
dump. It writes only the file named by --out, and nothing at all with --dry-run.

  census  --log LOG --census N --frame F --out FILE
      The draw rows of one census frame of an edvr_gfx log, as logged (the timestamp dropped), each draw row's target ids resolved to the
      "DC id" lines it names, in one text file: tools/supercruise_census_test/fixtures/census_054801_f1.txt is census 1 (05:48:01), frame 1 of
      edvr_gfx_20261007_054519.log. Format: "# ..." comment lines, "ID @n <text after the id>" lines, "ROW <the row>" lines.

  dust    --eyes DIR --stamp STAMP --out FILE
      EDVRDU01: what the space-dust checks read of an eye dump (eyes\\eye_<stamp>_*): the crop's occluders and stencil from SceneZ (RLE), the
      dust pixels of the first dumped frame, and the luminance of the background under the dust pixels of six frames (8-bit, post-tonemap).
      The dust pixels are the dump's own definition: sky (no depth in the D plane), a bright (> 0.10) and saturated (> 0.40) pixel of the C
      crop; the background is the median luminance of the 9x9 neighbourhood with those pixels left out. Needs numpy and PIL.

  orbit   --pool DIR --eyes DIR --stamp STAMP --frame F --ordinal O --out FILE
      EDVROL01: one orbit-line draw (VS b1 rows 0..332, vertex buffer 0, vertex buffer 1, from the draw state and the aux capture of the same
      eye run) and the crop of the eye's SceneZ the layer's depth-stencil seed copies: tools/orbit_layer_test/fixtures/orbit_050020_29722_312.bin.

  --self-test
      The encoders and decoders on synthetic data, the census parser on a made-up log, and every writer's --dry-run.

EDVROL01 / EDVRDU01 layouts (little endian) are written out at the two writers below; the C++ readers (tools/orbit_layer_test, tools/
supercruise_census_test) are the other half of each, and the rigs fail if a fixture is not what they read.
"""
import argparse
import os
import re
import struct
import sys
import tempfile

# --------------------------------------------------------------------------------------------------------- run-length coding

def rle_pairs(values):
    """[(length, value)] of a flat sequence, runs of at most 2**32-1."""
    out = []
    for v in values:
        if out and out[-1][1] == v:
            out[-1][0] += 1
        else:
            out.append([1, v])
    return [(n, v) for n, v in out]


def depth_tokens(depths):
    """Depth as tokens: (zero_run, literal_count, literals...) until the sequence is spent. A literal run ends at the next zero."""
    tokens = []
    i, n = 0, len(depths)
    while i < n:
        z = 0
        while i < n and depths[i] == 0.0:
            z += 1
            i += 1
        lit = []
        while i < n and depths[i] != 0.0:
            lit.append(depths[i])
            i += 1
        tokens.append((z, lit))
    return tokens


def pack_depth_tokens(depths):
    toks = depth_tokens(depths)
    out = struct.pack("<I", len(toks))
    for z, lit in toks:
        out += struct.pack("<II", z, len(lit)) + struct.pack("<%df" % len(lit), *lit)
    return out


def unpack_depth_tokens(buf, off, total):
    (n,) = struct.unpack_from("<I", buf, off)
    off += 4
    out = []
    for _ in range(n):
        z, c = struct.unpack_from("<II", buf, off)
        off += 8
        out.extend([0.0] * z)
        out.extend(struct.unpack_from("<%df" % c, buf, off))
        off += 4 * c
    if len(out) != total:
        raise ValueError("depth tokens cover %d of %d pixels" % (len(out), total))
    return out, off


def pack_runs(values, fmt="<IB"):
    runs = rle_pairs(values)
    return struct.pack("<I", len(runs)) + b"".join(struct.pack(fmt, n, v) for n, v in runs)


def unpack_runs(buf, off, total, fmt="<IB"):
    size = struct.calcsize(fmt)
    (n,) = struct.unpack_from("<I", buf, off)
    off += 4
    out = []
    for _ in range(n):
        ln, v = struct.unpack_from(fmt, buf, off)
        off += size
        out.extend([v] * ln)
    if len(out) != total:
        raise ValueError("runs cover %d of %d pixels" % (len(out), total))
    return out, off


# --------------------------------------------------------------------------------------------------------- the census text

ROW_RE = re.compile(r"^\[[\d:.]+\] (DC (\d+) #(\d+) [NXDI] .*)$")
ID_RE = re.compile(r"^\[[\d:.]+\] DC id @(\d+) (.*)$")


def census_rows(lines, census, frame):
    """(ids, rows) of one census of a log: the draw rows of `frame` and the id lines they name. `census` counts "DC begin" lines from 1."""
    begin = 0
    ids, rows = {}, []
    for line in lines:
        if "] DC begin" in line:
            begin += 1
            continue
        if begin != census:
            continue
        m = ID_RE.match(line.rstrip())
        if m:
            ids[int(m.group(1))] = m.group(2)
            continue
        m = ROW_RE.match(line.rstrip())
        if m and int(m.group(2)) == frame:
            rows.append(m.group(1))
    if not rows:
        raise ValueError("census %d has no draw rows of frame %d" % (census, frame))
    named = set()
    for r in rows:
        for key in ("r", "d"):
            mm = re.search(r" %s=@(\d+)" % key, r)
            if mm:
                named.add(int(mm.group(1)))
    missing = sorted(i for i in named if i not in ids)
    if missing:
        raise ValueError("the rows name ids the census never described: %s" % missing[:8])
    return {i: ids[i] for i in sorted(named)}, rows


def census_text(source, census, frame, ids, rows):
    out = ["# edvr census fixture (tools/layer_fixtures.py census): %s, census %d, frame %d: %d draw rows, %d target/depth ids" % (source, census, frame, len(rows), len(ids)),
           "# ID @n <description>: the census's own 'DC id' line; ROW <row>: the census's own 'DC <frame> #<ordinal> <kind> ...' line, timestamp dropped"]
    out += ["ID @%d %s" % (i, d) for i, d in ids.items()]
    out += ["ROW " + r for r in rows]
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------------------------------------- SceneZ

def read_scenez(path):
    with open(path, "rb") as f:
        b = f.read()
    ver, w, h, fmt, pitch = struct.unpack_from("<5I", b, 8)
    if b[:8] != b"EDVRTEX1" or len(b) != 44 + w * h * 8:
        raise ValueError("%s is not an EDVRTEX1 R32G8X24 image of %dx%d" % (path, w, h))
    return b, w, h


def scenez_crop(b, w, h, x0, y0, cw, ch):
    """(depth floats, stencil bytes) of the crop, row-major."""
    depth, stencil = [], []
    for y in range(y0, y0 + ch):
        row = b[44 + (y * w + x0) * 8: 44 + (y * w + x0 + cw) * 8]
        for x in range(cw):
            d, s = struct.unpack_from("<fI", row, x * 8)
            depth.append(d)
            stencil.append(s & 0xFF)
    return depth, stencil


# --------------------------------------------------------------------------------------------------------- the writers

def write_out(path, data, dry):
    if dry:
        print("dry-run: would write %d bytes to %s" % (len(data), path))
        return
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    print("wrote %d bytes to %s" % (len(data), path))


ORBIT_MAGIC = b"EDVROL01"
DUST_MAGIC = b"EDVRDU01"
DUST_CROP = (308, 274, 1400, 1400)   # the C/D crops of an eye dump: x, y, width, height in the eye's frame
ORBIT_CROP = (1190, 700, 826, 630)   # the crop of the layer rig: both sizes times 2.5 are whole


def orbit_bytes(cb1, vb0, vb1, target, crop, depth, stencil):
    """EDVROL01: header, the draw, the crop's stencil runs, the crop's depth tokens.
       "EDVROL01", u32 version 1, u32 cb1Rows, vertices, instances, floatsPerInstance, targetW, targetH, cropX, cropY, cropW, cropH,
       f32 cb1[rows*4], f32 vb0[vertices*4], f32 vb1[instances*floats], stencil runs (u32 n, n x (u32 len, u8 value)),
       depth tokens (u32 n, n x (u32 zeroRun, u32 literalCount, f32 literals...))."""
    rows, verts, inst, per = len(cb1) // 4, len(vb0) // 4, len(vb1) // 15, 15
    cx, cy, cw, ch = crop
    if len(depth) != cw * ch or len(stencil) != cw * ch:
        raise ValueError("the crop planes are %d / %d pixels, the crop is %d" % (len(depth), len(stencil), cw * ch))
    out = ORBIT_MAGIC + struct.pack("<11I", 1, rows, verts, inst, per, target[0], target[1], cx, cy, cw, ch)
    out += struct.pack("<%df" % len(cb1), *cb1) + struct.pack("<%df" % len(vb0), *vb0) + struct.pack("<%df" % len(vb1), *vb1)
    out += pack_runs(stencil) + pack_depth_tokens(depth)
    return out


def read_orbit(buf):
    if buf[:8] != ORBIT_MAGIC:
        raise ValueError("not EDVROL01")
    ver, rows, verts, inst, per, tw, th, cx, cy, cw, ch = struct.unpack_from("<11I", buf, 8)
    off = 8 + 44
    cb1 = list(struct.unpack_from("<%df" % (rows * 4), buf, off)); off += rows * 16
    vb0 = list(struct.unpack_from("<%df" % (verts * 4), buf, off)); off += verts * 16
    vb1 = list(struct.unpack_from("<%df" % (inst * per), buf, off)); off += inst * per * 4
    stencil, off = unpack_runs(buf, off, cw * ch)
    depth, off = unpack_depth_tokens(buf, off, cw * ch)
    if off != len(buf):
        raise ValueError("%d trailing bytes" % (len(buf) - off))
    return dict(rows=rows, verts=verts, inst=inst, per=per, target=(tw, th), crop=(cx, cy, cw, ch), cb1=cb1, vb0=vb0, vb1=vb1, stencil=stencil, depth=depth)


def dust_bytes(frame, crop, stencil, occluder, points, background):
    """EDVRDU01: "EDVRDU01", u32 version 1, u32 frameW, frameH, cropX, cropY, cropW, cropH, runs of (u32 len, u8 stencil, u8 occluder)
       over the crop (u32 n first), u32 nPoints then (u16 x, u16 y) in crop coordinates, u32 nBackground then that many u8."""
    cx, cy, cw, ch = crop
    if len(stencil) != cw * ch or len(occluder) != cw * ch:
        raise ValueError("the crop planes do not fit the crop")
    out = DUST_MAGIC + struct.pack("<7I", 1, frame[0], frame[1], cx, cy, cw, ch)
    pairs = [(s << 8) | o for s, o in zip(stencil, occluder)]
    runs = rle_pairs(pairs)
    out += struct.pack("<I", len(runs)) + b"".join(struct.pack("<IBB", n, v >> 8, v & 0xFF) for n, v in runs)
    out += struct.pack("<I", len(points)) + b"".join(struct.pack("<HH", x, y) for x, y in points)
    out += struct.pack("<I", len(background)) + bytes(background)
    return out


def read_dust(buf):
    if buf[:8] != DUST_MAGIC:
        raise ValueError("not EDVRDU01")
    ver, fw, fh, cx, cy, cw, ch = struct.unpack_from("<7I", buf, 8)
    off = 8 + 28
    (n,) = struct.unpack_from("<I", buf, off)
    off += 4
    stencil, occ = [], []
    for _ in range(n):
        ln, s, o = struct.unpack_from("<IBB", buf, off)
        off += 6
        stencil.extend([s] * ln)
        occ.extend([o] * ln)
    if len(stencil) != cw * ch:
        raise ValueError("runs cover %d of %d" % (len(stencil), cw * ch))
    (n,) = struct.unpack_from("<I", buf, off)
    off += 4
    points = [struct.unpack_from("<HH", buf, off + 4 * i) for i in range(n)]
    off += 4 * n
    (n,) = struct.unpack_from("<I", buf, off)
    off += 4
    bg = list(buf[off:off + n])
    off += n
    if off != len(buf):
        raise ValueError("%d trailing bytes" % (len(buf) - off))
    return dict(frame=(fw, fh), crop=(cx, cy, cw, ch), stencil=stencil, occluder=occ, points=points, background=bg)


# --------------------------------------------------------------------------------------------------------- the subcommands

def cmd_census(a):
    with open(a.log, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    ids, rows = census_rows(lines, a.census, a.frame)
    write_out(a.out, census_text(os.path.basename(a.log), a.census, a.frame, ids, rows).encode("utf-8"), a.dry_run)


def cmd_orbit(a):
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import eye_draw_snapshot as eds   # the same reader every drawstate tool uses
    state = eds.read(os.path.join(a.pool, "drawstate_%s.bin" % a.stamp))
    draw = [d for d in state["draws"] if d["vs"] == 0xC7FA0C0F5DD49180 and d["frame"] == a.frame and d["ordinal"] == a.ordinal]
    if len(draw) != 1:
        raise SystemExit("the draw state holds %d orbit-line draws at frame %d ordinal %d" % (len(draw), a.frame, a.ordinal))
    d = draw[0]
    cb1_raw = d["buffers"][1]["data"]
    rows = 333
    cb1 = list(struct.unpack_from("<%df" % (rows * 4), cb1_raw, 0))
    # the aux capture of the same frame: entry of vs C7FA0C0F5DD49180, buffers cb2, t0, vb0, vb1
    with open(os.path.join(a.pool, "aux_%s_%d.bin" % (a.stamp, a.frame)), "rb") as f:
        aux = f.read()
    magic, ver, fr, entries, pad = struct.unpack_from("<8sIIII", aux, 0)
    off = 24
    vb0 = vb1 = None
    for _ in range(entries):
        vs, inst, count = struct.unpack_from("<QII", aux, off)
        off += 16
        sizes = struct.unpack_from("<4I", aux, off); off += 16
        strides = struct.unpack_from("<4I", aux, off); off += 16
        bufs = []
        for k in range(4):
            bufs.append(aux[off:off + sizes[k]]); off += sizes[k]
        if vs == 0xC7FA0C0F5DD49180:
            vb0 = list(struct.unpack("<%df" % (len(bufs[2]) // 4), bufs[2]))[:d["count"] * 4]
            vb1 = list(struct.unpack("<%df" % (len(bufs[3]) // 4), bufs[3]))[:d["instances"] * 15]
    if vb0 is None or len(vb0) != d["count"] * 4 or len(vb1) != d["instances"] * 15:
        raise SystemExit("the aux capture does not hold the draw's vertex buffers")
    b, w, h = read_scenez(os.path.join(a.eyes, "eye_%s_SceneZ.bin" % a.stamp))
    cx, cy, cw, ch = ORBIT_CROP
    depth, stencil = scenez_crop(b, w, h, cx, cy, cw, ch)
    write_out(a.out, orbit_bytes(cb1, vb0, vb1, (d["width"], d["height"]), ORBIT_CROP, depth, stencil), a.dry_run)


def cmd_dust(a):
    import numpy as np
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    cx, cy, cw, ch = DUST_CROP
    b, w, h = read_scenez(os.path.join(a.eyes, "eye_%s_SceneZ.bin" % a.stamp))
    arr = np.frombuffer(b, dtype=np.uint8, offset=44, count=w * h * 8).reshape(h, w, 8)
    z = arr[..., :4].copy().view("<f4").reshape(h, w)
    st = arr[..., 4]
    zc, sc = z[cy:cy + ch, cx:cx + cw], st[cy:cy + ch, cx:cx + cw]
    occluder = (zc > 0).astype(np.uint8)

    def load_c(i):
        return np.asarray(Image.open(os.path.join(a.eyes, "eye_%s_C%02d.bmp" % (a.stamp, i))).convert("RGB")).astype(np.float32) / 255.0

    def load_d(i):
        data = open(os.path.join(a.eyes, "eye_%s_D%02d.bin" % (a.stamp, i)), "rb").read()
        if data[:8] != b"EDVRTEX1":
            raise SystemExit("D%02d is not EDVRTEX1" % i)
        return np.frombuffer(data, dtype="<f4", offset=44).reshape(ch, cw, 4)

    DEPTH_VALID = 256

    def candidates(c, d):
        sky = (d[..., 3].astype(np.int64) & DEPTH_VALID) == 0
        mx, mn = c.max(axis=2), c.min(axis=2)
        sat = (mx - mn) / np.maximum(mx, 1e-6)
        return sky & (mx > 0.10) & (sat > 0.40)

    c0, d0 = load_c(0), load_d(0)
    cand0 = candidates(c0, d0)
    ys, xs = np.nonzero(cand0)
    points = list(zip(xs.tolist(), ys.tolist()))
    background = []
    for t in range(0, 16, 3):
        c, d = load_c(t), load_d(t)
        cand = candidates(c, d)
        lum = c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722
        lum = lum.copy()
        lum[cand] = np.nan
        yy, xx = np.nonzero(cand)
        for y, x in list(zip(yy, xx))[::3]:
            if y < 5 or x < 5 or y > ch - 6 or x > cw - 6:
                continue
            win = lum[y - 4:y + 5, x - 4:x + 5]
            v = win[~np.isnan(win)]
            if len(v) >= 20:
                background.append(int(round(float(np.median(v)) * 255)))
    write_out(a.out, dust_bytes((w, h), DUST_CROP, sc.reshape(-1).tolist(), occluder.reshape(-1).tolist(), points, background), a.dry_run)


# --------------------------------------------------------------------------------------------------------- the self-test

def self_test():
    # run-length coding
    vals = [5, 5, 5, 1, 1, 7, 7, 7, 7, 0]
    assert rle_pairs(vals) == [(3, 5), (2, 1), (4, 7), (1, 0)]
    buf = pack_runs(vals)
    back, off = unpack_runs(buf, 0, len(vals))
    assert back == vals and off == len(buf)
    try:
        unpack_runs(buf, 0, len(vals) + 1)
        raise AssertionError("a short run table must fail")
    except ValueError:
        pass
    depth = [0.0, 0.0, 1.5, 2.5, 0.0, 3.5, 0.0, 0.0, 0.0]
    packed = pack_depth_tokens(depth)
    back, off = unpack_depth_tokens(packed, 0, len(depth))
    assert back == depth and off == len(packed)
    assert unpack_depth_tokens(pack_depth_tokens([0.0] * 5), 0, 5)[0] == [0.0] * 5
    assert unpack_depth_tokens(pack_depth_tokens([1.0] * 4), 0, 4)[0] == [1.0] * 4
    # the orbit fixture's layout round-trips, and a trailing byte or a wrong magic fails
    cb1 = [float(i) for i in range(333 * 4)]
    vb0 = [float(i % 7) for i in range(8 * 4)]
    vb1 = [float(i) for i in range(2 * 15)]
    cw, ch = 4, 3
    stencil = [5, 5, 5, 5, 21, 21, 144, 5, 5, 5, 5, 5]
    dep = [0.0, 0.0, 0.0, 0.0, 1.4e-11, 1.4e-11, 0.01, 0.0, 0.0, 0.0, 0.0, 0.0]
    ob = orbit_bytes(cb1, vb0, vb1, (2016, 1949), (10, 20, cw, ch), dep, stencil)
    r = read_orbit(ob)
    assert r["rows"] == 333 and r["verts"] == 8 and r["inst"] == 2 and r["crop"] == (10, 20, cw, ch) and r["target"] == (2016, 1949)
    assert r["cb1"] == cb1 and r["vb0"] == vb0 and r["vb1"] == vb1 and r["stencil"] == stencil
    assert all(abs(a - b) <= 1e-15 + 1e-7 * abs(b) for a, b in zip(r["depth"], dep))
    for bad in (ob + b"\0", b"EDVROL00" + ob[8:]):
        try:
            read_orbit(bad)
            raise AssertionError("a bad fixture must fail")
        except ValueError:
            pass
    # the dust fixture's layout
    st = [5, 5, 5, 144, 144, 5]
    oc = [0, 0, 0, 1, 1, 0]
    db = dust_bytes((2016, 1949), (308, 274, 3, 2), st, oc, [(1, 0), (2, 1)], [0, 1, 7, 200])
    r = read_dust(db)
    assert r["stencil"] == st and r["occluder"] == oc and r["points"] == [(1, 0), (2, 1)] and r["background"] == [0, 1, 7, 200] and r["crop"] == (308, 274, 3, 2)
    for bad in (db + b"\0", b"EDVRDU00" + db[8:]):
        try:
            read_dust(bad)
            raise AssertionError("a bad fixture must fail")
        except ValueError:
            pass
    # the census parser, on a made-up log: two censuses, rows of two frames, ids named and not
    log = [
        "[05:00:00.000] DC begin census=1 frames=2 frame=10 offscreen=no",
        "[05:00:00.001] DC id @1 tex 2016x1949 fmt=26 res=00000001 vf=26",
        "[05:00:00.001] DC id @2 tex 2016x1949 fmt=19 res=00000002 vf=20",
        "[05:00:00.002] DC 0 #1 N n=3 i=1 r=@1 d=@2 vh=AAAA ph=BBBB",
        "[05:00:00.003] DC 1 #1 N n=3 i=1 r=@1 d=@2 vh=AAAA ph=BBBB",
        "[05:00:00.004] DC 1 #2 X n=6 i=1 r=@1 d=- vh=CCCC ph=DDDD",
        "[05:00:00.005] DCL 1 #0 B a=@9",
        "[05:00:01.000] DC begin census=2 frames=2 frame=20 offscreen=yes",
        "[05:00:01.001] DC id @1 tex 100x100 fmt=26 res=00000009 vf=26",
        "[05:00:01.002] DC 1 #7 N n=3 i=1 r=@1 d=- vh=EEEE ph=FFFF",
    ]
    ids, rows = census_rows(log, 1, 1)
    assert sorted(ids) == [1, 2] and len(rows) == 2 and rows[0].startswith("DC 1 #1 N ") and rows[1].startswith("DC 1 #2 X ")
    ids2, rows2 = census_rows(log, 2, 1)
    assert ids2 == {1: "tex 100x100 fmt=26 res=00000009 vf=26"} and len(rows2) == 1
    for bad in ((log, 1, 5), (log, 3, 1), ([l for l in log if "DC id @2" not in l], 1, 1)):
        try:
            census_rows(*bad)
            raise AssertionError("a census with no rows of the frame, no such census, or an undescribed id must fail")
        except ValueError:
            pass
    text = census_text("made-up.log", 1, 1, ids, rows)
    assert text.count("\nROW ") == 2 and text.count("\nID @") == 2 and text.startswith("# edvr census fixture")
    # --dry-run writes nothing at all, and a real write is atomic and leaves no temporary file
    with tempfile.TemporaryDirectory() as d:
        target = os.path.join(d, "sub", "x.bin")
        write_out(target, b"abc", True)
        assert not os.path.exists(os.path.join(d, "sub")), "--dry-run created a directory"
        write_out(target, b"abc", False)
        assert open(target, "rb").read() == b"abc" and os.listdir(os.path.join(d, "sub")) == ["x.bin"]
    # the crop of a SceneZ image: read_scenez checks size, scenez_crop reads depth and the stencil byte
    w, h = 4, 3
    body = b"".join(struct.pack("<fI", float(i), i & 0xFF) for i in range(w * h))
    img = b"EDVRTEX1" + struct.pack("<9I", 1, w, h, 19, w * 8, 0, 0, 0, 0) + body
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "z.bin")
        open(p, "wb").write(img)
        b, ww, hh = read_scenez(p)
        dd, ss = scenez_crop(b, ww, hh, 1, 1, 2, 2)
        assert (ww, hh) == (4, 3) and dd == [5.0, 6.0, 9.0, 10.0] and ss == [5, 6, 9, 10]
        open(p, "wb").write(img + b"\0")
        try:
            read_scenez(p)
            raise AssertionError("a SceneZ of the wrong size must fail")
        except ValueError:
            pass
    # the three fixtures the rigs read are in the repo and are what these readers (and the rigs' C++ ones) say they are
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "orbit_layer_test", "fixtures", "orbit_050020_29722_312.bin"), "rb") as f:
        r = read_orbit(f.read())
    assert (r["rows"], r["verts"], r["inst"], r["per"], r["target"], r["crop"]) == (333, 8194, 6, 15, (2016, 1949), ORBIT_CROP), "the orbit fixture's shape"
    assert r["vb1"][3] == 2.0 and all(r["vb1"][15 * i + 3] == 1.5 for i in range(1, 6)), "the orbit fixture's half-widths: the orbit 2.0, five rings 1.5"
    with open(os.path.join(here, "supercruise_census_test", "fixtures", "dust_054804.bin"), "rb") as f:
        r = read_dust(f.read())
    assert r["frame"] == (2016, 1949) and r["crop"] == DUST_CROP and len(r["points"]) == 2043 and len(r["background"]) == 4143, "the dust fixture's shape"
    with open(os.path.join(here, "supercruise_census_test", "fixtures", "census_054801_f1.txt"), "rb") as f:
        text = f.read().decode("utf-8")
    assert text.count("\nROW DC 1 #") == 244 and text.count("\nID @") == 18, "the census fixture: 244 draw rows of frame 1 and 18 ids"
    assert " vh=9BFC7FD232328391 " in text and " vh=A47A3315FFF5E2E4 " in text and " vh=C7FA0C0F5DD49180 " in text, "the census fixture holds the three supercruise vertex shaders"
    print("layer_fixtures self-test OK")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--self-test", action="store_true")
    sub = p.add_subparsers(dest="cmd")   # each writer takes --dry-run (writes nothing at all)
    c = sub.add_parser("census"); c.add_argument("--log", required=True); c.add_argument("--census", type=int, required=True); c.add_argument("--frame", type=int, required=True); c.add_argument("--out", required=True)
    d = sub.add_parser("dust"); d.add_argument("--eyes", required=True); d.add_argument("--stamp", required=True); d.add_argument("--out", required=True)
    o = sub.add_parser("orbit"); o.add_argument("--pool", required=True); o.add_argument("--eyes", required=True); o.add_argument("--stamp", required=True)
    o.add_argument("--frame", type=int, required=True); o.add_argument("--ordinal", type=int, required=True); o.add_argument("--out", required=True)
    for sp in (c, d, o):
        sp.add_argument("--dry-run", action="store_true", dest="dry_run", help="write nothing at all")
    a = p.parse_args(argv)
    if a.self_test:
        self_test()
        return 0
    if a.cmd == "census":
        cmd_census(a)
    elif a.cmd == "dust":
        cmd_dust(a)
    elif a.cmd == "orbit":
        cmd_orbit(a)
    else:
        p.print_help()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
