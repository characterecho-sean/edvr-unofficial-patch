#!/usr/bin/env python3
"""Read a pair of instance-pool dumps and say what moved between them.

The object probe (src/d3d11/object_probe.cpp, advanced.object_probe = 1)
writes two consecutive frames of the game's instanced-mesh pool to
edvr_logs\\pool\\pool_HHMMSS_<frame>.bin every 30 s, up to eight pairs a
session: a 32-byte header, the scene block (VS b1) of that frame, then the
pool's records (336 bytes each). This reads two of them and reports, per
record and in aggregate, the things the probe's in-game totals could not
separate on the flights of 2026-09-08:

  - which records are live (non-zero), which changed, and by how much
    (the pose decoded the game's way: scale at 4, unorm16x4 quaternion at
    8, float3 position at 16; the second block at 288/292/312);
  - the frame's own motion, from the scene block: cb1[275] is the camera's
    position in the record's frame, and the rows around it its basis;
  - the rigid motion D = W_prev * W_now^-1 per record at the same slot,
    clustered within a tolerance that scales with distance, and the share
    of records the largest cluster holds -- one cluster of nearly all the
    station's parts is a rigid station in a fixed frame; a cloud is either
    a shuffled pool, stale slots, or a frame that moves;
  - the same records matched by content instead of slot, to tell a shuffle
    from a motion.

Usage:
    python tools/pool_pair.py A.bin B.bin [--verbose]
    python tools/pool_pair.py --self-test
"""
from __future__ import annotations

import math
import struct
import sys

RECORD = 336
HEADER = struct.Struct('<8sIIIIII')
QUAT_SCALE = 0.000031
POS_OFF = 16
QUAT_OFF = 8
SCALE_OFF = 4
POS2_OFF = 292
QUAT2_OFF = 312
CAMERA_ROW = 275          # cb1[275].xyz: the camera's position in the record's frame
SIG_RANGES = ((28, 30), (31, 32), (56, 288), (304, 308), (320, 336))


def read_dump(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic, version, frame, pool_bytes, scene_bytes, record_bytes, records = HEADER.unpack_from(data, 0)
    if magic != b'EDVRPOOL' or version != 1 or record_bytes != RECORD:
        raise SystemExit(f'{path}: not a version-1 pool dump')
    scene = data[HEADER.size:HEADER.size + scene_bytes]
    pool = data[HEADER.size + scene_bytes:HEADER.size + scene_bytes + pool_bytes]
    if len(pool) != pool_bytes or len(scene) != scene_bytes:
        raise SystemExit(f'{path}: short file')
    return {'frame': frame, 'scene': scene, 'pool': pool, 'records': records}


def decode_quat(raw, off):
    xy, zw = struct.unpack_from('<II', raw, off)
    q = [(xy & 0xFFFF) * QUAT_SCALE - 1.0, (xy >> 16) * QUAT_SCALE - 1.0,
         (zw & 0xFFFF) * QUAT_SCALE - 1.0, (zw >> 16) * QUAT_SCALE - 1.0]
    n = math.sqrt(sum(c * c for c in q))
    return [c / n for c in q] if n > 1e-6 else [0.0, 0.0, 0.0, 1.0]


def decode(raw, i):
    """One record's head: (bone base, scale, quaternion xyzw, position), and its second block."""
    o = i * RECORD
    bone = struct.unpack_from('<I', raw, o)[0]
    scale = struct.unpack_from('<f', raw, o + SCALE_OFF)[0]
    q = decode_quat(raw, o + QUAT_OFF)
    p = list(struct.unpack_from('<fff', raw, o + POS_OFF))
    q2 = decode_quat(raw, o + QUAT2_OFF)
    p2 = list(struct.unpack_from('<fff', raw, o + POS2_OFF))
    return {'bone': bone, 'scale': scale, 'q': q, 'p': p, 'q2': q2, 'p2': p2}


def is_live(raw, i):
    o = i * RECORD
    return any(raw[o:o + RECORD])


def signature(raw, i):
    o = i * RECORD
    return b''.join(raw[o + b:o + e] for b, e in SIG_RANGES)


def qmul_conj(a, b):
    """a * conj(b): the rotation taking b's frame to a's (xyzw)."""
    bx, by, bz, bw = -b[0], -b[1], -b[2], b[3]
    return [a[3] * bx + a[0] * bw + a[1] * bz - a[2] * by,
            a[3] * by - a[0] * bz + a[1] * bw + a[2] * bx,
            a[3] * bz + a[0] * by - a[1] * bx + a[2] * bw,
            a[3] * bw - a[0] * bx - a[1] * by - a[2] * bz]


def rotate(q, v):
    c = [q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0]]
    cc = [q[1] * c[2] - q[2] * c[1], q[2] * c[0] - q[0] * c[2], q[0] * c[1] - q[1] * c[0]]
    return [v[i] + 2.0 * (q[3] * c[i] + cc[i]) for i in range(3)]


def angle_deg(q):
    w = min(1.0, abs(q[3]))
    return 2.0 * math.degrees(math.acos(w))


def rigid_delta(prev, now):
    """D = W_prev * W_now^-1: (qd, t, angle, |t|, |p_now|)."""
    qd = qmul_conj(prev['q'], now['q'])
    if qd[3] < 0.0:
        qd = [-c for c in qd]
    rp = rotate(qd, now['p'])
    t = [prev['p'][i] - rp[i] for i in range(3)]
    return qd, t, angle_deg(qd), math.sqrt(sum(c * c for c in t)), math.sqrt(sum(c * c for c in now['p']))


def cluster(deltas, angle_tol=0.03, pos_tol=0.03, pos_per_m=2e-4, limit=256):
    """Greedy clustering of (qd, t, angle, |t|, dist) tuples; returns clusters as index lists."""
    clusters = []
    for idx, (qd, t, _a, _tm, dist) in enumerate(deltas):
        tol = pos_tol + pos_per_m * dist
        for c in clusters:
            cq, ct = c['q'], c['t']
            dot = min(1.0, abs(sum(qd[i] * cq[i] for i in range(4))))
            if 2.0 * math.degrees(math.acos(dot)) > angle_tol:
                continue
            if math.sqrt(sum((t[i] - ct[i]) ** 2 for i in range(3))) > tol:
                continue
            c['members'].append(idx)
            break
        else:
            if len(clusters) < limit:
                clusters.append({'q': qd, 't': t, 'members': [idx]})
    clusters.sort(key=lambda c: -len(c['members']))
    return clusters


def camera(scene):
    """cb1[275].xyz and the three rows after it, if the block reaches them."""
    need = (CAMERA_ROW + 4) * 16
    if len(scene) < need:
        return None
    rows = []
    for r in range(CAMERA_ROW, CAMERA_ROW + 4):
        rows.append(list(struct.unpack_from('<ffff', scene, r * 16)))
    return rows


def analyse(a, b, verbose=False, out=print):
    n = min(a['records'], b['records'])
    pa, pb = a['pool'], b['pool']
    live_a = [is_live(pa, i) for i in range(n)]
    live_b = [is_live(pb, i) for i in range(n)]
    out(f"frames {a['frame']} -> {b['frame']}: {n} slots, live {sum(live_a)} -> {sum(live_b)}")
    cam_a, cam_b = camera(a['scene']), camera(b['scene'])
    if cam_a and cam_b:
        d = [cam_b[0][i] - cam_a[0][i] for i in range(3)]
        out(f"camera cb1[{CAMERA_ROW}]: {cam_a[0][:3]} -> {cam_b[0][:3]}, moved {math.sqrt(sum(c * c for c in d)):.4f}")
        for k in range(1, 4):
            out(f"  row {CAMERA_ROW + k}: {cam_a[k]} -> {cam_b[k]}")
    else:
        out('camera: the scene block does not reach the camera rows (or was not captured)')

    same = changed = freed = allocated = 0
    deltas, delta_slots = [], []
    pos_step, quat_step = [], []
    second_is_own = second_is_prev = 0
    for i in range(n):
        ra, rb = pa[i * RECORD:(i + 1) * RECORD], pb[i * RECORD:(i + 1) * RECORD]
        if ra == rb:
            same += live_a[i]
            continue
        if not live_b[i]:
            freed += 1
            continue
        if not live_a[i]:
            allocated += 1
            continue
        changed += 1
        da, db = decode(pa, i), decode(pb, i)
        if db['q2'] == db['q'] and db['p2'] == db['p']:
            second_is_own += 1
        if db['q2'] == da['q'] and db['p2'] == da['p']:
            second_is_prev += 1
        step = math.sqrt(sum((db['p'][k] - da['p'][k]) ** 2 for k in range(3)))
        pos_step.append(step)
        quat_step.append(angle_deg(qmul_conj(da['q'], db['q'])))
        deltas.append(rigid_delta(da, db))
        delta_slots.append(i)
    out(f"unchanged live {same}, changed {changed}, freed {freed}, allocated {allocated}")
    if changed:
        pos_step.sort()
        quat_step.sort()
        mid = changed // 2
        out(f"per-slot step: position median {pos_step[mid]:.4f} m (90th {pos_step[int(changed * 0.9)]:.3f}, "
            f"max {pos_step[-1]:.2f}); turn median {quat_step[mid]:.4f} deg (90th {quat_step[int(changed * 0.9)]:.3f}, "
            f"max {quat_step[-1]:.2f})")
        out(f"second block equals the record's own pose on {second_is_own} of {changed}, last frame's on {second_is_prev}")
        cl = cluster(deltas)
        big = cl[0]['members']
        out(f"rigid motions by slot: {len(cl)} clusters; largest {len(big)} of {changed} "
            f"({100.0 * len(big) / changed:.0f}%), turn {angle_deg(cl[0]['q']):.4f} deg, "
            f"translation {math.sqrt(sum(c * c for c in cl[0]['t'])):.4f} m; next {[len(c['members']) for c in cl[1:6]]}")

    # The same records by content: a shuffle puts last frame's bytes at a
    # new slot; a motion changes them in place.
    prev_by_bytes = {}
    for i in range(n):
        if live_a[i]:
            prev_by_bytes.setdefault(pa[i * RECORD:(i + 1) * RECORD], []).append(i)
    moved_whole = 0
    for i in range(n):
        if not live_b[i]:
            continue
        rb = pb[i * RECORD:(i + 1) * RECORD]
        if rb == pa[i * RECORD:(i + 1) * RECORD]:
            continue
        js = prev_by_bytes.get(rb)
        if js and any(j != i for j in js):
            moved_whole += 1
    prev_sig = {}
    for i in range(n):
        if live_a[i]:
            prev_sig.setdefault(signature(pa, i), []).append(i)
    # Nearest same-signature record by position: the instance identity a
    # classifier would use. How far the nearest is says whether it is safe.
    near = []
    for i in range(n):
        if not (live_b[i] and live_a[i]):
            continue
        cands = prev_sig.get(signature(pb, i))
        if not cands:
            continue
        db = decode(pb, i)
        best = None
        for j in cands:
            da = decode(pa, j)
            dist = math.sqrt(sum((db['p'][k] - da['p'][k]) ** 2 for k in range(3)))
            if best is None or dist < best[0]:
                best = (dist, j)
        near.append((best[0], i, best[1]))
    at_slot = sum(1 for d, i, j in near if i == j)
    near_d = sorted(d for d, _i, _j in near)
    out(f"by content: {moved_whole} records are byte-for-byte at a new slot; nearest same-signature record "
        f"is at the same slot for {at_slot} of {len(near)}"
        + (f", median distance {near_d[len(near_d) // 2]:.4f} m, 90th {near_d[int(len(near_d) * 0.9)]:.3f}" if near_d else ''))
    if verbose:
        for k, (qd, t, ang, tm, dist) in enumerate(deltas[:40]):
            out(f"  slot {delta_slots[k]}: turn {ang:.4f} deg, t {tm:.4f} m, at {dist:.1f} m")


def self_test():
    """Two synthetic frames: a rigid body of 20 parts turning 0.05 deg about y and
    moving 0.2 m, one part elsewhere moving on its own, one slot freed, one stale."""
    import random
    rnd = random.Random(7)

    def enc_quat(q):
        n = math.sqrt(sum(c * c for c in q))
        q = [c / n for c in q]
        u = [max(0, min(65535, int(round((c + 1.0) / QUAT_SCALE)))) for c in q]
        return struct.pack('<II', u[0] | (u[1] << 16), u[2] | (u[3] << 16))

    def record(q, p, sig_byte):
        r = bytearray(RECORD)
        r[SCALE_OFF:SCALE_OFF + 4] = struct.pack('<f', 1.0)
        r[QUAT_OFF:QUAT_OFF + 8] = enc_quat(q)
        r[POS_OFF:POS_OFF + 12] = struct.pack('<fff', *p)
        r[QUAT2_OFF:QUAT2_OFF + 8] = enc_quat(q)
        r[POS2_OFF:POS2_OFF + 12] = struct.pack('<fff', *p)
        r[100] = sig_byte
        return bytes(r)

    half = math.radians(0.05) / 2.0
    qd = [0.0, math.sin(half), 0.0, math.cos(half)]
    tmove = [0.2, 0.0, 0.0]
    n = 32
    a, b = bytearray(RECORD * n), bytearray(RECORD * n)
    q0 = [0.1, 0.2, 0.3, 0.9]
    for i in range(20):
        p = [rnd.uniform(-500, 500) for _ in range(3)]
        ra = record(q0, p, 1)
        # now = D^-1 applied: W_now = D^-1 W_prev  ->  q_now = conj(qd) q_prev, p_now = R^-1 (p - t)
        qinv = [-qd[0], -qd[1], -qd[2], qd[3]]
        pn = rotate(qinv, [p[k] - tmove[k] for k in range(3)])
        qn = qmul_conj(qinv, [-q0[0], -q0[1], -q0[2], q0[3]])   # qinv * q0
        rb = record(qn, pn, 1)
        a[i * RECORD:(i + 1) * RECORD] = ra
        b[i * RECORD:(i + 1) * RECORD] = rb
    # a lone mover
    a[20 * RECORD:21 * RECORD] = record(q0, [10.0, 0.0, 0.0], 2)
    b[20 * RECORD:21 * RECORD] = record(q0, [10.0, 3.0, 0.0], 2)
    # a slot freed, one allocated
    a[21 * RECORD:22 * RECORD] = record(q0, [1.0, 1.0, 1.0], 3)
    b[22 * RECORD:23 * RECORD] = record(q0, [2.0, 2.0, 2.0], 3)
    scene = bytearray((CAMERA_ROW + 4) * 16)
    scene_b = bytearray(scene)
    struct.pack_into('<ffff', scene, CAMERA_ROW * 16, 0.0, 0.0, 0.0, 1.0)
    struct.pack_into('<ffff', scene_b, CAMERA_ROW * 16, 0.5, 0.0, 0.0, 1.0)
    fa = {'frame': 100, 'scene': bytes(scene), 'pool': bytes(a), 'records': n}
    fb = {'frame': 101, 'scene': bytes(scene_b), 'pool': bytes(b), 'records': n}
    lines = []
    analyse(fa, fb, out=lines.append)
    text = '\n'.join(lines)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(f"  {'ok  ' if cond else 'FAIL'}  {what}")
        ok = ok and cond

    check('live 22 -> 22' in text, 'live counts')
    check('changed 21, freed 1, allocated 1' in text, 'changed / freed / allocated')
    check('largest 20 of 21 (95%)' in text, 'the rigid body is one cluster of twenty')
    check('turn 0.0500 deg' in text, 'the cluster turn is the body turn')
    check('moved 0.5000' in text, 'the camera delta reads from the scene block')
    check('second block equals the record\'s own pose on 21 of 21' in text, 'the second block test')
    check('is at the same slot for 21 of 21' in text, 'nearest same-signature record is the slot itself')
    # The header round-trips.
    hd = HEADER.pack(b'EDVRPOOL', 1, 7, RECORD * 2, 16, RECORD, 2)
    check(len(hd) == 32, 'the header is 32 bytes')
    return ok


def main(argv):
    if '--self-test' in argv:
        return 0 if self_test() else 1
    args = [x for x in argv[1:] if not x.startswith('--')]
    if len(args) != 2:
        print(__doc__)
        return 2
    a, b = read_dump(args[0]), read_dump(args[1])
    if b['frame'] < a['frame']:
        a, b = b, a
    analyse(a, b, verbose='--verbose' in argv)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
