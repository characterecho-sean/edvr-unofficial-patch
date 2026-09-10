#!/usr/bin/env python3
"""The eye run's ledger, read: which records each draw took from the pool, how far each turned
between consecutive crops, and how the bones moved (object_probe.h, "THE EYE RUN'S LEDGER").

python eye_run_ledger.py <edvr_logs\\pool> <HHMMSS> [--hub-radius 500] [--ring-radius 800]

Reads draws_<stamp>.bin (a 160-byte header: magic, version, frames, frame0, the instance
stream's stride, the palette's stride, the pool's bytes, then 32 crop-to-frame entries; then per
frame the frame, a count and 24-byte rows: the vertex shader hash, the vertex or index count,
the instance count, the start instance, the draw kind, and whether t33 was the pool), and for
each frame pool_<stamp>_<frame>.bin (the pair dumps' format), inst_<stamp>_<frame>.bin (the
instance stream: 8 bytes an instance, the record index first) and bones_<stamp>_<frame>.bin (the
palette's first megabyte, 48-byte rows: three float4 rows of a 3x4 matrix).

Per pair of consecutive crops it prints, for the records the pool draws took: how many were
drawn in each radius band about the station's axis (the axis fitted from the moving records),
the median turn of the drawn ones and of the undrawn, the skinned records' turn with their first
bone composed in, and the big instanced draws that were NOT the pool's, for the next look.
"""
import argparse, glob, math, os, struct, sys
import numpy as np

REC = 336

def load_pool(path):
    b = open(path, 'rb').read()
    magic, ver, frame, poolBytes, sceneBytes, recBytes, records = struct.unpack('<8sIIIIII', b[:32])
    assert magic == b'EDVRPOOL' and recBytes == REC, path
    raw = np.frombuffer(b, dtype=np.uint8, count=records * REC, offset=32 + sceneBytes).reshape(records, REC)
    base = raw[:, :4].copy().view(np.uint32).ravel()
    q = raw[:, 8:16].copy().view(np.uint16).reshape(records, 4).astype(np.float64) * (2.0 / 65535.0) - 1.0
    pos = raw[:, 16:28].copy().view(np.float32).reshape(records, 3).astype(np.float64)
    return dict(frame=frame, base=base, q=q, pos=pos, raw=raw)

def qnorm(q):
    n = np.linalg.norm(q, axis=1, keepdims=True); n[n == 0] = 1
    return q / n

def qmul(a, b):
    x1, y1, z1, w1 = a.T; x2, y2, z2, w2 = b.T
    return np.stack([w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
                     w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2], axis=1)

def qrel(qa, qb):
    """the rotation taking qa to qb: angle (deg) and unit axis per row"""
    qa = qnorm(qa); qb = qnorm(qb)
    d = qmul(qb, qa * np.array([-1, -1, -1, 1.0]))
    s = np.linalg.norm(d[:, :3], axis=1)
    ang = np.degrees(2 * np.arctan2(s, np.abs(d[:, 3])))
    axis = d[:, :3] / np.where(s[:, None] > 0, s[:, None], 1.0)
    axis *= np.sign(d[:, 3])[:, None]
    return ang, axis

def qmat(q):
    """3x3 rotation matrices from (x,y,z,w) quaternions, rows n"""
    q = qnorm(q)
    x, y, z, w = q.T
    return np.stack([np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], -1),
                     np.stack([2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)], -1),
                     np.stack([2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)], -1)], 1)

def rot_angle(Ra, Rb):
    """the angle (deg) of Rb Ra^T per row"""
    D = np.einsum('nij,nkj->nik', Rb, Ra)
    tr = np.clip((D[:, 0, 0] + D[:, 1, 1] + D[:, 2, 2] - 1) / 2, -1, 1)
    return np.degrees(np.arccos(tr))

def bone_mats(bones, base, count):
    """the 3x3 of the first bone at each base (48-byte rows: three float4 rows)"""
    rows = np.frombuffer(bones, dtype=np.float32).reshape(-1, 12)
    M = np.zeros((count, 3, 3)); ok = np.zeros(count, bool)
    for i, b in enumerate(base):
        if b < rows.shape[0]:
            r = rows[b].reshape(3, 4)
            M[i] = r[:, :3]; ok[i] = True
    return M, ok

def fit_axis(p0, p1, ang, axis, mask):
    """the station's axis (unit) and a point on it, from records that turned: p1 - p0 = (R - I)(p0 - c)"""
    m = mask & (ang > 0.005) & (ang < 1.0)
    if m.sum() < 20: return None, None
    a = axis[m]
    # the common axis: the principal direction of the per-record axes (sign-aligned)
    ref = a[np.argmax(np.abs(a).sum(1))]
    a = a * np.sign(a @ ref)[:, None]
    u = a.mean(0); u /= np.linalg.norm(u)
    th = np.radians(np.median(ang[m]))
    K = np.array([[0, -u[2], u[1]], [u[2], 0, -u[0]], [-u[1], u[0], 0]])
    R = np.eye(3) + math.sin(th) * K + (1 - math.cos(th)) * (K @ K)
    A = R - np.eye(3)
    # least squares for c: (R - I) c = (R - I) p0 - (p1 - p0)  summed over records; the axis direction is unobservable, so solve in the plane
    P0 = p0[m]; P1 = p1[m]
    rhs = (P0 @ A.T) - (P1 - P0)
    # solve A c = rhs_i for all i (stack)
    AA = np.vstack([A] * 1)
    # normal equations over all records
    M = A.T @ A * m.sum()
    v = (A.T @ rhs.T).sum(1)
    # remove the axis direction from the solve
    Pp = np.eye(3) - np.outer(u, u)
    M2 = Pp @ M @ Pp + np.outer(u, u)
    c = np.linalg.lstsq(M2, Pp @ v, rcond=None)[0]
    # place c at the records' mean along the axis
    c = c + u * ((P0.mean(0) - c) @ u)
    return u, c

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir'); ap.add_argument('stamp')
    ap.add_argument('--hub-radius', type=float, default=500.0)
    ap.add_argument('--ring-radius', type=float, default=800.0)
    ap.add_argument('--big', type=int, default=50, help='a non-pool instanced draw with at least this many instances is listed')
    a = ap.parse_args()
    dp = os.path.join(a.dir, f'draws_{a.stamp}.bin')
    b = open(dp, 'rb').read()
    magic, ver, frames, frame0, instStride, bonesStride, poolBytes = struct.unpack('<8sIIIIII', b[:32])
    assert magic == b'EDVRLDGR', dp
    crop = struct.unpack('<32i', b[32:160])
    off = 160
    draws = {}
    for i in range(frames):
        frame, n = struct.unpack('<II', b[off:off + 8]); off += 8
        rows = np.frombuffer(b, dtype=np.dtype([('vs', '<u8'), ('count', '<u4'), ('inst', '<u4'), ('start', '<u4'), ('kind', 'u1'), ('pool', 'u1'), ('pad', '<u2')]), count=n, offset=off)
        off += n * 24
        draws[frame] = rows
    cropFrames = [(k, f) for k, f in enumerate(crop) if f >= 0]
    print(f"ledger {a.stamp}: {frames} frames from {frame0}; instance stride {instStride}, palette stride {bonesStride}, pool {poolBytes} bytes")
    print("crops: " + " ".join(f"C{k:02d}=f{f}" for k, f in cropFrames))
    pools = {}; insts = {}; bones = {}
    for frame in range(frame0, frame0 + frames):
        p = os.path.join(a.dir, f'pool_{a.stamp}_{frame}.bin')
        if os.path.exists(p): pools[frame] = load_pool(p)
        p = os.path.join(a.dir, f'inst_{a.stamp}_{frame}.bin')
        if os.path.exists(p): insts[frame] = np.frombuffer(open(p, 'rb').read(), dtype=np.uint32)
        p = os.path.join(a.dir, f'bones_{a.stamp}_{frame}.bin')
        if os.path.exists(p): bones[frame] = open(p, 'rb').read()
    print(f"on disk: {len(pools)} pool frames, {len(insts)} instance streams, {len(bones)} palette copies, {sum(len(v) for v in draws.values())} draw rows")
    stride_u32 = max(1, instStride // 4)

    def drawn_records(frame):
        """record index -> (vs, draws) for the pool draws of this frame"""
        out = {}
        if frame not in insts or frame not in draws: return out
        st = insts[frame]
        for r in draws[frame]:
            if not r['pool'] or r['inst'] == 0: continue
            s = int(r['start']); n = int(r['inst'])
            idx = st[s * stride_u32:(s + n) * stride_u32:stride_u32]
            for ri in idx:
                if ri < 2048: out.setdefault(int(ri), []).append(int(r['vs']))
        return out

    seq = [f for k, f in cropFrames]
    seq = sorted(set(seq))
    pairs = [(seq[i], seq[i + 1]) for i in range(len(seq) - 1)] if len(seq) > 1 else []
    if not pairs:
        print("no consecutive crops in the header; using consecutive frames on disk instead")
        fs = sorted(pools); pairs = [(fs[i], fs[i + 1]) for i in range(len(fs) - 1)]
    print()
    print("per pair of consecutive crops: the station's records (5-15 km) by radius band about the fitted axis --")
    print("   drawn = taken by a pool draw in BOTH frames; turn = the median turn of those records between the frames (deg)")
    hdr = f"{'frames':>13s} {'axis fit':>9s} | {'hub: drawn/all':>15s} {'turn':>7s} {'undrawn':>8s} | {'mid: drawn/all':>15s} {'turn':>7s} {'undrawn':>8s} | {'ring: drawn/all':>16s} {'turn':>7s} {'undrawn':>8s} | {'skinned drawn':>13s} {'rec':>6s} {'bone':>6s} {'both':>6s}"
    print(hdr)
    for fa, fb in pairs:
        if fa not in pools or fb not in pools:
            print(f"{fa:>6d}->{fb:<6d} (a pool frame is missing)"); continue
        A, B = pools[fa], pools[fb]
        d0 = np.linalg.norm(A.pos if False else A['pos'], axis=1)
        station = (d0 > 5000) & (d0 < 15000) & np.isfinite(d0)
        same = station & (A['base'] == B['base']) & (np.linalg.norm(B['pos'] - A['pos'], axis=1) < 50)
        ang, axis = qrel(A['q'], B['q'])
        u, c = fit_axis(A['pos'], B['pos'], ang, axis, same & (A['base'] == 0))
        if u is None:
            print(f"{fa:>6d}->{fb:<6d} (no axis: too few records turned)"); continue
        rel = A['pos'] - c
        r_axis = np.linalg.norm(rel - np.outer(rel @ u, u), axis=1)
        drawnA = drawn_records(fa); drawnB = drawn_records(fb)
        drawn = np.zeros(2048, bool)
        for ri in drawnA:
            if ri in drawnB: drawn[ri] = True
        bands = [("hub", r_axis < a.hub_radius), ("mid", (r_axis >= a.hub_radius) & (r_axis < a.ring_radius)), ("ring", r_axis >= a.ring_radius)]
        cells = []
        for name, bm in bands:
            m = same & bm
            md = m & drawn; mu = m & ~drawn
            t_d = np.median(ang[md]) if md.sum() else float('nan')
            t_u = np.median(ang[mu]) if mu.sum() else float('nan')
            cells.append(f"{md.sum():>7d}/{m.sum():<7d} {t_d:>7.4f} {t_u:>8.4f}")
        # the skinned records: the record's turn, the first bone's turn, and both composed
        sk = same & (A['base'] != 0) & drawn
        skin_txt = f"{sk.sum():>13d} {'-':>6s} {'-':>6s} {'-':>6s}"
        if sk.sum() and fa in bones and fb in bones:
            idx = np.nonzero(sk)[0]
            Ra = qmat(A['q'][idx]); Rb = qmat(B['q'][idx])
            Ma, oka = bone_mats(bones[fa], A['base'][idx], len(idx)); Mb, okb = bone_mats(bones[fb], B['base'][idx], len(idx))
            ok = oka & okb
            if ok.any():
                t_rec = np.median(rot_angle(Ra[ok], Rb[ok]))
                t_bone = np.median(rot_angle(Ma[ok], Mb[ok]))
                t_both = np.median(rot_angle(np.einsum('nij,njk->nik', Ra[ok], Ma[ok]), np.einsum('nij,njk->nik', Rb[ok], Mb[ok])))
                skin_txt = f"{sk.sum():>13d} {t_rec:>6.4f} {t_bone:>6.4f} {t_both:>6.4f}"
        print(f"{fa:>6d}->{fb:<6d} {np.degrees(0):>9.0f} | " + " | ".join(cells) + f" | {skin_txt}")
    # the draws by shader, first pair's first frame
    if pairs and pairs[0][0] in draws:
        f = pairs[0][0]
        rows = draws[f]
        print()
        print(f"frame {f}: {len(rows)} eye draws; by vertex shader (pool draws first):")
        by = {}
        for r in rows:
            k = (int(r['vs']), int(r['pool']))
            e = by.setdefault(k, [0, 0]); e[0] += 1; e[1] += int(r['inst'])
        for (vs, pool), (n, inst) in sorted(by.items(), key=lambda kv: (-kv[0][1], -kv[1][1]))[:24]:
            print(f"   {'POOL' if pool else '    '} {vs:016X} {n:5d} draws {inst:6d} instances")
        big = [r for r in rows if not r['pool'] and r['inst'] >= a.big]
        if big:
            print(f"   non-pool instanced draws with >= {a.big} instances (the next look): " +
                  ", ".join(f"{int(r['vs']):016X} x{int(r['inst'])} @{int(r['start'])} n={int(r['count'])}" for r in big[:12]))

if __name__ == '__main__':
    sys.exit(main())
