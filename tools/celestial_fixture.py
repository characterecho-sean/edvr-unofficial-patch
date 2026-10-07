#!/usr/bin/env python3
"""The celestial-motion rig's fixture: real patch constants from eye dump 180540, and the references the rig checks.

Eye dump 180540 (build v0.18.2-56-geccfce7a, Frontier install, a supercruise approach to a moon at 3.9 Mm,
docs/terrain-motion-dispatch-cost-2026-09-17.md 2026-10-06) holds 19 frames of the planet patch draws in
drawstate_180540.bin (VS 72BDD292154158AD, the colour pass: 36 draws per eye per frame = 6 bodies x 6 faces) and the
camera rows, tangents and jitter of 16 of those frames in eye_180540_motion.csv. Drawstate frame N is motion.csv
frame N-1 (the dump's frame counter is one ahead; cb0[9..11] of drawstate frame N equals motion.csv's `now`
rotation of frame N-1).

  python tools\\celestial_fixture.py --extract <drawstate_180540.bin> <eye_180540_motion.csv> [--out tools\\celestial_motion_test\\fixture_180540.bin]
  python tools\\celestial_fixture.py --self-test          re-derives every stored reference from the stored raw
                                                          constants (independent of the C++ in src\\common\\celestial_math.h)
  python tools\\celestial_fixture.py --info [fixture]     what is in it

THE FIXTURE (little-endian, about 200 KB). Header 'EDVRCEL1', version, eye-frames, references. Per eye-frame: frame,
eye, patches, hasMotion, A (cb0[9..11].xyz), cb1[270..273] (the per-eye clip columns the game projects with), the
pass's tanNow, tanPrev and jitter for that frame (motion.csv), then the patches: c, q, rows (cb2[4..7]), o, q2, body
(cb2[12]). Then the references, per eye, per frame with a motion row on it and the one before, per body (the near
moon, radius 679444.9, and the far body of radius 835951.4 at 6.4e8 m):
  dT        the body's rigid delta translation, the mean over its faces of t = tp - R tc in world-aligned axes
            (each face matched to last frame's by its static rows; the doc's D references are these);
  p1, p1m   the nearest patch's centre in head axes, and its jitter-free true motion;
  p2, p2m   the body centre (cb2[12].xyz, back in head axes by A^T), and its jitter-free true motion.
TRUE MOTION is the game's own chain, no EDVR arithmetic in it: a point's world-aligned camera-relative position in
each frame (the patch's A c and its last-frame counterpart's; the centre from cb2[12] of each frame) goes through that
frame's cb1 columns to a pixel; previous minus current is the motion with both frames' jitter in it, and the pass's
jitter step (jitter of the previous frame minus this frame's, motion.csv) comes off.
"""
import argparse
import csv
import math
import struct
import sys
from pathlib import Path

MAGIC = b'EDVRCEL1'
VERSION = 1
VS = 0x72BDD292154158AD
W, H = 2016, 1949                     # the eye dump's render size
MOON = 679444.875                     # cb2[12].w of the near body, as a float32
FAR = 835951.4375                     # a far body, 6.4e8 m
BODIES = (MOON, FAR)
HEAD_FMT = '<8sIII'
FRAME_FMT = '<IIII9f16f4f4f2f'        # frame eye patches hasMotion A cb1 tanNow tanPrev jit
PATCH_FMT = '<34f'                    # c(3) q(4) rows(16) o(3) q2(4) body(4)
REF_FMT = '<IIfI13d8f4f'              # frame eye radius flags dT p1 p1m p2 p2m tanNow tanPrev jit jitPrev
# The doc's D references (eye 0, the moon), metres, world-aligned: drawstate frame -> mean face translation.
DOC_D = {23651: (-226.7, -365.4, -2788.1), 23652: (-204.4, -336.1, -2555.8), 23653: (-2017.6, -3255.4, -24794.2),
         23654: (-2225.2, -3585.9, -27316.3), 23655: (-858.2, -1383.9, -10535.0), 23656: (-596.1, -961.7, -7323.2),
         23657: (-530.6, -857.0, -6514.7)}


# --- small double-precision helpers -------------------------------------------------------------------------------
def f32(x):
    return struct.unpack('<f', struct.pack('<f', float(x)))[0]


def qmat(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def mm(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mv(a, v):
    return [sum(a[i][k] * v[k] for k in range(3)) for i in range(3)]


def tr(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]


def mat3(flat):
    return [list(flat[0:3]), list(flat[3:6]), list(flat[6:9])]


def world(patch, A):
    """T = [A R(q) | A c] of one patch."""
    return mm(A, qmat(patch['q'])), mv(A, patch['c'])


def clip_pixel(cb1, X):
    """The game's projection: world-aligned camera-relative X through cb1[270..273] to a pixel (px, py)."""
    c = [X[0] * cb1[0][k] + X[1] * cb1[1][k] + X[2] * cb1[2][k] + cb1[3][k] for k in range(4)]
    return ((c[0] / c[3] + 1) / 2 * W, (1 - c[1] / c[3]) / 2 * H)


def distance(p):
    return math.sqrt(sum(x * x for x in p['c']))


# --- the references, from raw -------------------------------------------------------------------------------------
def body_patches(frame_patches, radius):
    return [p for p in frame_patches if p['body'][3] == radius]


def match(cur, prev):
    """Patches of `cur` with exactly one counterpart in `prev` by their static rows, and no other patch claiming it."""
    pairs = []
    claimed = {}
    for p in cur:
        m = [q for q in prev if q['rows'] == p['rows']]
        if len(m) == 1:
            pairs.append((p, m[0]))
            claimed[id(m[0])] = claimed.get(id(m[0]), 0) + 1
    return [(p, q) for p, q in pairs if claimed[id(q)] == 1]


def reference(cur_ef, prev_ef, radius):
    """One body, one eye, one frame: the references, or None when there is nothing to match."""
    cur = body_patches(cur_ef['patches'], radius)
    prev = body_patches(prev_ef['patches'], radius)
    pairs = match(cur, prev)
    if not pairs:
        return None
    A, Ap = mat3(cur_ef['A']), mat3(prev_ef['A'])
    cb1 = [cur_ef['cb1'][i * 4:i * 4 + 4] for i in range(4)]
    cb1p = [prev_ef['cb1'][i * 4:i * 4 + 4] for i in range(4)]
    ts = []
    for p, q in pairs:
        Rc, tc = world(p, A)
        Rp, tp = world(q, Ap)
        R = mm(Rp, tr(Rc))
        Rt = mv(R, tc)
        ts.append([tp[i] - Rt[i] for i in range(3)])
    dT = [sum(t[i] for t in ts) / len(ts) for i in range(3)]
    jit, jitp = cur_ef['jit'], prev_ef['jit']
    js = (jitp[0] - jit[0], jitp[1] - jit[1])

    def motion(Xc, Xp):
        a, b = clip_pixel(cb1, Xc), clip_pixel(cb1p, Xp)
        return (b[0] - a[0] - js[0], b[1] - a[1] - js[1])

    # p1: the nearest patch's centre, in head axes, and the point's own previous place
    near = min(pairs, key=lambda pq: distance(pq[0]))
    p1 = list(near[0]['c'])
    m1 = motion(mv(A, p1), mv(Ap, near[1]['c']))
    # p2: the body centre, from cb2[12] of each frame
    Bc, Bp = cur[0]['body'][:3], prev[0]['body'][:3]
    p2 = mv(tr(A), Bc)
    m2 = motion(Bc, Bp)
    return dict(dT=dT, p1=p1, p1m=m1, p2=p2, p2m=m2, matched=len(pairs))


def read_motion(path):
    rows = {}
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            rows[(int(r['frame']), int(r['eye']))] = r
    return rows


def extract(drawstate, motion_csv, out):
    sys.path.insert(0, str(Path(__file__).parent))
    import eye_draw_snapshot as eds
    cap = eds.read(drawstate)
    motion = read_motion(motion_csv)
    draws = [d for d in cap['draws'] if d['vs'] == VS]
    targets = sorted({d['target'] for d in draws})
    if len(targets) != 2:
        raise SystemExit(f'expected two eye targets, found {len(targets)}')

    def row(d, slot, i):
        return struct.unpack_from('<4f', d['buffers'][slot]['data'], i * 16)

    frames = {}
    for d in draws:
        eye = targets.index(d['target'])
        ef = frames.setdefault((d['frame'], eye), dict(patches=[]))
        if 'A' not in ef:
            ef['A'] = [row(d, 0, 9 + i)[j] for i in range(3) for j in range(3)]
            ef['cb1'] = [v for i in range(4) for v in row(d, 1, 270 + i)]
        ef['patches'].append(dict(c=row(d, 2, 2)[:3], q=row(d, 2, 10),
                                  rows=tuple(v for i in (4, 5, 6, 7) for v in row(d, 2, i)),
                                  o=row(d, 2, 8)[:3], q2=row(d, 2, 23), body=row(d, 2, 12)))
    for (f, eye), ef in frames.items():
        m = motion.get((f - 1, eye))     # drawstate frame N is motion.csv frame N-1
        ef['hasMotion'] = 1 if m else 0
        if m:
            # float32, as the pass holds them and as the fixture stores them, so a re-derivation matches to the bit
            ef['tanNow'] = [f32(m[f'tanNow{i}']) for i in range(4)]
            ef['tanPrev'] = [f32(m[f'tanPrev{i}']) for i in range(4)]
            ef['jit'] = [f32(m['jitter0']), f32(m['jitter1'])]
        else:
            ef['tanNow'] = ef['tanPrev'] = [0.0] * 4
            ef['jit'] = [0.0, 0.0]
    write(frames, out)


def refs_of(frames):
    refs = []
    for (f, eye) in sorted(frames):
        cur, prev = frames[(f, eye)], frames.get((f - 1, eye))
        if prev is None or not cur['hasMotion'] or not prev['hasMotion']:
            continue
        for radius in BODIES:
            r = reference(cur, prev, radius)
            if r:
                r.update(frame=f, eye=eye, radius=radius, tanNow=cur['tanNow'], tanPrev=cur['tanPrev'],
                         jit=cur['jit'], jitPrev=prev['jit'])
                refs.append(r)
    return refs


def write(frames, out):
    refs = refs_of(frames)
    blob = bytearray(struct.pack(HEAD_FMT, MAGIC, VERSION, len(frames), len(refs)))
    for (f, eye) in sorted(frames):
        ef = frames[(f, eye)]
        blob += struct.pack(FRAME_FMT, f, eye, len(ef['patches']), ef['hasMotion'], *ef['A'], *ef['cb1'],
                            *ef['tanNow'], *ef['tanPrev'], *ef['jit'])
        for p in ef['patches']:
            blob += struct.pack(PATCH_FMT, *p['c'], *p['q'], *p['rows'], *p['o'], *p['q2'], *p['body'])
    for r in refs:
        blob += struct.pack(REF_FMT, r['frame'], r['eye'], r['radius'], 0, *r['dT'], *r['p1'], *r['p1m'], *r['p2'],
                            *r['p2m'], *r['tanNow'], *r['tanPrev'], *r['jit'], *r['jitPrev'])
    Path(out).write_bytes(bytes(blob))
    print(f'celestial_fixture: {len(frames)} eye-frames, {len(refs)} references, {len(blob)} bytes -> {out}')


def read(path):
    data = Path(path).read_bytes()
    magic, version, n, nrefs = struct.unpack_from(HEAD_FMT, data, 0)
    if magic != MAGIC or version != VERSION:
        raise ValueError('not a celestial fixture of this version')
    off = struct.calcsize(HEAD_FMT)
    frames = {}
    for _ in range(n):
        v = struct.unpack_from(FRAME_FMT, data, off)
        off += struct.calcsize(FRAME_FMT)
        ef = dict(frame=v[0], eye=v[1], hasMotion=v[3], A=list(v[4:13]), cb1=list(v[13:29]), tanNow=list(v[29:33]),
                  tanPrev=list(v[33:37]), jit=list(v[37:39]), patches=[])
        for _ in range(v[2]):
            p = struct.unpack_from(PATCH_FMT, data, off)
            off += struct.calcsize(PATCH_FMT)
            ef['patches'].append(dict(c=p[0:3], q=p[3:7], rows=p[7:23], o=p[23:26], q2=p[26:30], body=p[30:34]))
        frames[(v[0], v[1])] = ef
    refs = []
    for _ in range(nrefs):
        v = struct.unpack_from(REF_FMT, data, off)
        off += struct.calcsize(REF_FMT)
        refs.append(dict(frame=v[0], eye=v[1], radius=v[2], dT=list(v[4:7]), p1=list(v[7:10]), p1m=list(v[10:12]),
                         p2=list(v[12:15]), p2m=list(v[15:17]), tanNow=list(v[17:21]), tanPrev=list(v[21:25]),
                         jit=list(v[25:27]), jitPrev=list(v[27:29])))
    if off != len(data):
        raise ValueError('trailing bytes in the fixture')
    return frames, refs


def check(ok, why):
    if not ok:
        raise AssertionError(why)


def self_test(path):
    frames, refs = read(path)
    check(len(frames) == 38 and len(refs) == 60, f'38 eye-frames and 60 references expected, got {len(frames)}, {len(refs)}')
    check(all(len(ef['patches']) == 36 for ef in frames.values()), 'every eye-frame holds 36 patches (6 bodies x 6 faces)')
    # every reference re-derives from the stored raw constants, to the stored float64 bits
    again = refs_of(frames)
    check(len(again) == len(refs), 'the same references come out')
    worst = 0.0
    for a, b in zip(again, refs):
        check((a['frame'], a['eye'], a['radius']) == (b['frame'], b['eye'], b['radius']), 'reference order')
        for key in ('dT', 'p1', 'p1m', 'p2', 'p2m'):
            for x, y in zip(a[key], b[key]):
                worst = max(worst, abs(x - y))
                check(abs(x - y) <= 1e-9 * max(1.0, abs(y)), f'{key} of frame {b["frame"]} eye {b["eye"]} re-derives')
    # the doc's D references (eye 0, the moon) agree with the stored ones. They were made from the unnormalised
    # quaternions' matrices, whose 1e-7 scale shows as a metre along the patch's distance; the stored ones normalise.
    for r in refs:
        if r['eye'] == 0 and r['radius'] == MOON and r['frame'] in DOC_D:
            err = math.dist(r['dT'], DOC_D[r['frame']])
            check(err < 2.0, f'frame {r["frame"]}: D {r["dT"]} against the doc {DOC_D[r["frame"]]} is off by {err:.2f} m')
    # sanity of the physics the references rest on: the world-path motion the camera term would give is far from the truth
    moon0 = [r for r in refs if r['eye'] == 0 and r['radius'] == MOON]
    check(len(moon0) == 15, '15 frames of the moon on eye 0')
    check(max(math.hypot(*r['p2m']) for r in moon0) > 2.0, 'the moon moves by pixels in these frames')
    print(f'celestial_fixture: self-test passed ({len(refs)} references re-derived, worst difference {worst:.2e})')


def info(path):
    frames, refs = read(path)
    print(f'{path}: {len(frames)} eye-frames, {len(refs)} references, {Path(path).stat().st_size} bytes')
    for (f, eye), ef in sorted(frames.items())[:4]:
        print(f'  frame {f} eye {eye}: {len(ef["patches"])} patches, motion {ef["hasMotion"]}, jit {ef["jit"]}')


def main():
    default = Path(__file__).parent / 'celestial_motion_test' / 'fixture_180540.bin'
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--info', nargs='?', const=str(default))
    ap.add_argument('--extract', nargs=2, metavar=('DRAWSTATE', 'MOTION_CSV'))
    ap.add_argument('--out', default=str(default))
    ap.add_argument('--fixture', default=str(default))
    a = ap.parse_args()
    if a.extract:
        extract(a.extract[0], a.extract[1], a.out)
    elif a.info:
        info(a.info)
    elif a.self_test:
        self_test(a.fixture)
    else:
        ap.error('one of --extract, --info, --self-test')


if __name__ == '__main__':
    main()
