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
    rows = np.frombuffer(bones, dtype=np.float32, count=(len(bones) // 48) * 12).reshape(-1, 12)   # whole rows only: a megabyte is not a multiple of 48
    M = np.zeros((count, 3, 3)); ok = np.zeros(count, bool)
    for i, b in enumerate(base):
        if b < rows.shape[0]:
            r = rows[b].reshape(3, 4)
            M[i] = r[:, :3]; ok[i] = True
    return M, ok

def kabsch(p0, p1):
    """the rigid motion p1 = R p0 + t of a point cloud, by SVD"""
    c0 = p0.mean(0); c1 = p1.mean(0)
    H = (p0 - c0).T @ (p1 - c1)
    U, S, Vt = np.linalg.svd(H)
    d = np.sign(np.linalg.det(Vt.T @ U.T))
    R = Vt.T @ np.diag([1, 1, d]) @ U.T
    return R, c1 - R @ c0

def fit_axis(p0, p1, ang, axis, mask):
    """the station's turn between the frames from the records' POSITIONS: a rigid fit (Kabsch) of the
    masked records, refined three times by dropping the records that do not move with the rest (the
    traffic, a record freed and reused) -- the quaternions' per-record axes are too coarse at a
    twentieth of a degree (unorm16). Returns the unit axis, a point on it, the turn in degrees and the
    mask of the records that fitted."""
    m = mask & (ang < 1.0)
    if m.sum() < 20: return None, None, 0.0, m
    for _ in range(4):
        R, t = kabsch(p0[m], p1[m])
        res = np.linalg.norm(p0 @ R.T + t - p1, axis=1)
        cut = max(0.05, 3.0 * np.median(res[m]))
        m2 = mask & (ang < 1.0) & (res < cut)
        if m2.sum() < 20 or m2.sum() == m.sum(): m = m2 if m2.sum() >= 20 else m; break
        m = m2
    R, t = kabsch(p0[m], p1[m])
    th = math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(R) - 1) / 2))))
    w = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]])
    if np.linalg.norm(w) < 1e-12: return None, None, th, m
    u = w / np.linalg.norm(w)
    # p1 = R(p0 - c) + c: t = (I - R) c, singular along the axis, which the pseudo-inverse leaves out
    c = np.linalg.pinv(np.eye(3) - R, rcond=1e-6) @ t
    c = c + u * ((p0[m].mean(0) - c) @ u)
    return u, c, th, m

def load_aux(path):
    b = open(path, 'rb').read()
    magic, ver, frame, entries, pad = struct.unpack('<8sIIII', b[:24])
    assert magic == b'EDVRLAUX', path
    off = 24; out = []
    for _ in range(entries):
        vs, instances, count = struct.unpack('<QII', b[off:off + 16]); off += 16
        sizes = struct.unpack('<4I', b[off:off + 16]); off += 16
        strides = struct.unpack('<4I', b[off:off + 16]); off += 16
        blobs = []
        for k in range(4):
            blobs.append(b[off:off + sizes[k]]); off += sizes[k]
        out.append(dict(vs=vs, instances=instances, count=count, stride=strides, bytes=blobs))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir'); ap.add_argument('stamp')
    ap.add_argument('--hub-radius', type=float, default=500.0)
    ap.add_argument('--ring-radius', type=float, default=800.0)
    ap.add_argument('--big', type=int, default=50, help='a non-pool instanced draw with at least this many instances is listed')
    ap.add_argument('--pool-vs', help='a file of vertex shader hashes (hex, one a line) that read t33: the pool draws. Without it the '
                                     'ledger flag decides, which only says t33 HELD the pool at the draw -- true of every draw after a '
                                     'pool draw, the game never unbinding it (the run of 2026-09-10 05:37)')
    a = ap.parse_args()
    readers = None
    if a.pool_vs:
        readers = set(int(l.strip(), 16) for l in open(a.pool_vs) if l.strip())
    dp = os.path.join(a.dir, f'draws_{a.stamp}.bin')
    b = open(dp, 'rb').read()
    magic, ver, frames, frame0, instStride, bonesStride, poolBytes = struct.unpack('<8sIIIIII', b[:32])
    assert magic == b'EDVRLDGR', dp
    crop = struct.unpack('<32i', b[32:160])
    off = 160
    draws = {}
    for i in range(frames):
        frame, n = struct.unpack('<II', b[off:off + 8]); off += 8
        rows = np.frombuffer(b, dtype=np.dtype([('vs', '<u8'), ('count', '<u4'), ('inst', '<u4'), ('start', '<u4'), ('kind', 'u1'), ('pool', 'u1'), ('pad', '<u2')]), count=n, offset=off).copy()
        off += n * 24
        if readers is not None:
            rows['pool'] = np.array([1 if int(v) in readers else 0 for v in rows['vs']], dtype=np.uint8)
        draws[frame] = rows
    cropFrames = [(k, f) for k, f in enumerate(crop) if f >= 0]
    print(f"ledger {a.stamp}: {frames} frames from {frame0}; instance stride {instStride}, palette stride {bonesStride}, pool {poolBytes} bytes")
    print("crops: " + " ".join(f"C{k:02d}=f{f}" for k, f in cropFrames))
    pools = {}; insts = {}; bones = {}; auxes = {}
    for frame in range(frame0, frame0 + frames):
        p = os.path.join(a.dir, f'pool_{a.stamp}_{frame}.bin')
        if os.path.exists(p): pools[frame] = load_pool(p)
        p = os.path.join(a.dir, f'inst_{a.stamp}_{frame}.bin')
        if os.path.exists(p): insts[frame] = np.frombuffer(open(p, 'rb').read(), dtype=np.uint32)
        for pi in range(4):
            p = os.path.join(a.dir, f'bones{pi}_{a.stamp}_{frame}.bin')
            if os.path.exists(p): bones.setdefault(frame, {})[pi] = open(p, 'rb').read()
        p = os.path.join(a.dir, f'bones_{a.stamp}_{frame}.bin')   # the first build's single palette
        if os.path.exists(p): bones.setdefault(frame, {})[0] = open(p, 'rb').read()
        p = os.path.join(a.dir, f'aux_{a.stamp}_{frame}.bin')
        if os.path.exists(p): auxes[frame] = load_aux(p)
    print(f"on disk: {len(pools)} pool frames, {len(insts)} instance streams, {len(bones)} frames of palettes, {len(auxes)} aux files, {sum(len(v) for v in draws.values())} draw rows")
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
    hdr = f"{'frames':>13s} {'body turn/n':>11s}| {'hub: drawn/all':>15s} {'turn':>7s} {'undrawn':>8s} | {'mid: drawn/all':>15s} {'turn':>7s} {'undrawn':>8s} | {'ring: drawn/all':>16s} {'turn':>7s} {'undrawn':>8s} | {'skinned drawn':>13s} {'rec':>6s} {'bone':>6s} {'both':>6s}"
    print(hdr)
    for fa, fb in pairs:
        if fa not in pools or fb not in pools:
            print(f"{fa:>6d}->{fb:<6d} (a pool frame is missing)"); continue
        A, B = pools[fa], pools[fb]
        d0 = np.linalg.norm(A.pos if False else A['pos'], axis=1)
        station = (d0 > 5000) & (d0 < 15000) & np.isfinite(d0)
        same = station & (A['base'] == B['base']) & (np.linalg.norm(B['pos'] - A['pos'], axis=1) < 50)
        ang, axis = qrel(A['q'], B['q'])
        u, c, turn, body = fit_axis(A['pos'], B['pos'], ang, axis, same & (A['base'] == 0))
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
            def pick(frame, bases):
                # the palette whose rows at the bases are rotations (orthonormal), chosen per frame: the
                # game binds two and alternates
                best = None
                for pi in sorted(bones[frame]):
                    M, ok = bone_mats(bones[frame][pi], bases, len(bases))
                    orth = ok & (np.abs(np.einsum('nij,nkj->nik', M, M) - np.eye(3)).max(axis=(1, 2)) < 0.05)
                    if best is None or orth.sum() > best[2].sum(): best = (pi, M, orth)
                return best
            pa = pick(fa, A['base'][idx]); pb = pick(fb, B['base'][idx])
            if pa and pb:
                ok = pa[2] & pb[2]
                if ok.sum() >= 3:
                    Ma, Mb = pa[1], pb[1]
                    t_rec = np.median(rot_angle(Ra[ok], Rb[ok]))
                    t_bone = np.median(rot_angle(Ma[ok], Mb[ok]))
                    t_both = np.median(rot_angle(np.einsum('nij,njk->nik', Ra[ok], Ma[ok]), np.einsum('nij,njk->nik', Rb[ok], Mb[ok])))
                    skin_txt = f"{sk.sum():>13d} {t_rec:>6.4f} {t_bone:>6.4f} {t_both:>6.4f} (palettes {pa[0]}/{pb[0]}, {ok.sum()} rotations)"
                else:
                    skin_txt = f"{sk.sum():>13d} {'-':>6s} {'-':>6s} {'-':>6s} (no rotations at the bases in any palette: {pa[2].sum()}/{pb[2].sum()})"
        print(f"{fa:>6d}->{fb:<6d} {turn:>6.4f}/{body.sum():<3d}| " + " | ".join(cells) + f" | {skin_txt}")
    # THE AUX: the big non-pool instanced draws -- per consecutive frames, the turn of cb2's world rows
    # (rows 2-4, the ones the dumped shaders multiply positions by) and what changed in t0 / the streams
    if auxes:
        print()
        print("the big instanced draws that read no pool -- cb2's rows 2-4 as a rotation, its turn between consecutive crops; t0 and the streams: dwords changed")
        vss = sorted(set(e['vs'] for fr in auxes.values() for e in fr))
        for vs in vss:
            line = []
            for fa, fb in pairs:
                ea = next((e for e in auxes.get(fa, []) if e['vs'] == vs), None)
                eb = next((e for e in auxes.get(fb, []) if e['vs'] == vs), None)
                if not ea or not eb: line.append("   -   "); continue
                ca, cb = ea['bytes'][0], eb['bytes'][0]
                if len(ca) >= 80 and len(cb) >= 80:
                    # rows 2-4 are floats 8..19 (a row is four floats)
                    Ma = np.frombuffer(ca, dtype=np.float32, count=20)[8:20].reshape(3, 4)[:, :3].astype(np.float64)
                    Mb = np.frombuffer(cb, dtype=np.float32, count=20)[8:20].reshape(3, 4)[:, :3].astype(np.float64)
                    # normalise the rows (a scale may ride along)
                    na = np.linalg.norm(Ma, axis=1, keepdims=True); nb = np.linalg.norm(Mb, axis=1, keepdims=True)
                    if na.min() > 1e-6 and nb.min() > 1e-6:
                        D = (Mb / nb) @ (Ma / na).T
                        tr = np.clip((np.trace(D) - 1) / 2, -1, 1)
                        line.append(f"{math.degrees(math.acos(tr)):.4f}")
                    else:
                        line.append(" zero ")
                else:
                    line.append(" none ")
            e0 = next((e for fr in auxes.values() for e in fr if e['vs'] == vs), None)
            print(f"   {vs:016X} x{e0['instances']} n={e0['count']} cb2 {len(e0['bytes'][0])} B, t0 {len(e0['bytes'][1])} B stride {e0['stride'][1]}, vb0 {len(e0['bytes'][2])} B stride {e0['stride'][2]}, vb1 {len(e0['bytes'][3])} B stride {e0['stride'][3]}")
            print("      cb2 rows 2-4 turn/pair: " + " ".join(line))
            for what, name in ((1, "t0"), (2, "vb0"), (3, "vb1")):
                ch = []
                for fa, fb in pairs:
                    ea = next((e for e in auxes.get(fa, []) if e['vs'] == vs), None)
                    eb = next((e for e in auxes.get(fb, []) if e['vs'] == vs), None)
                    if not ea or not eb or not ea['bytes'][what] or len(ea['bytes'][what]) != len(eb['bytes'][what]): ch.append("-"); continue
                    da = np.frombuffer(ea['bytes'][what], dtype=np.uint32); db = np.frombuffer(eb['bytes'][what], dtype=np.uint32)
                    ch.append(str(int((da != db).sum())))
                if any(c != "-" for c in ch): print(f"      {name} dwords changed/pair: " + " ".join(ch))
            # a per-instance stream (vb0/vb1/t0 with a stride of 12 or more): its elements' first three floats as a
            # position -- the median displacement per pair, and the turn of the cloud about its own centroid (Kabsch)
            for what, name in ((1, "t0"), (2, "vb0"), (3, "vb1")):
                st = e0['stride'][what]
                if st < 12 or not e0['bytes'][what]: continue
                n = min(len(e0['bytes'][what]) // st, max(e0['instances'], 1))
                if n < 8: continue
                disp = []; turns = []
                for fa, fb in pairs:
                    ea = next((e for e in auxes.get(fa, []) if e['vs'] == vs), None); eb = next((e for e in auxes.get(fb, []) if e['vs'] == vs), None)
                    if not ea or not eb or len(ea['bytes'][what]) < n * st or len(eb['bytes'][what]) < n * st: disp.append(float('nan')); turns.append(float('nan')); continue
                    Pa = np.frombuffer(ea['bytes'][what], dtype=np.uint8, count=n * st).reshape(n, st)[:, :12].copy().view(np.float32).reshape(n, 3).astype(np.float64)
                    Pb = np.frombuffer(eb['bytes'][what], dtype=np.uint8, count=n * st).reshape(n, st)[:, :12].copy().view(np.float32).reshape(n, 3).astype(np.float64)
                    okp = np.isfinite(Pa).all(1) & np.isfinite(Pb).all(1) & (np.abs(Pa).max(1) < 1e7) & (np.abs(Pb).max(1) < 1e7)
                    if okp.sum() < 8: disp.append(float('nan')); turns.append(float('nan')); continue
                    disp.append(float(np.median(np.linalg.norm(Pb[okp] - Pa[okp], axis=1))))
                    R, t = kabsch(Pa[okp], Pb[okp])
                    turns.append(math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(R) - 1) / 2)))))
                if all(np.isnan(disp)): continue
                print(f"      {name} as {n} positions (first 3 floats of {st}-byte elements): median |d|/pair " + " ".join(f"{d:.3f}" for d in disp))
                print(f"      {name} cloud turn/pair (Kabsch): " + " ".join(f"{t:.4f}" for t in turns) + f"; centroid radius {np.linalg.norm(Pa[okp].mean(0)):.0f} m, spread {np.linalg.norm(Pa[okp] - Pa[okp].mean(0), axis=1).mean():.0f} m")
            # t0 as 48-byte matrices: the per-instance rotation between the first pair
            if e0['stride'][1] == 48 and pairs:
                fa, fb = pairs[0]
                ea = next((e for e in auxes.get(fa, []) if e['vs'] == vs), None); eb = next((e for e in auxes.get(fb, []) if e['vs'] == vs), None)
                if ea and eb and ea['bytes'][1] and len(ea['bytes'][1]) == len(eb['bytes'][1]):
                    n = min(len(ea['bytes'][1]) // 48, e0['instances'])
                    Ma = np.frombuffer(ea['bytes'][1], dtype=np.float32, count=n * 12).reshape(n, 3, 4)[:, :, :3].astype(np.float64)
                    Mb = np.frombuffer(eb['bytes'][1], dtype=np.float32, count=n * 12).reshape(n, 3, 4)[:, :, :3].astype(np.float64)
                    print(f"      t0 as {n} 3x4 matrices: per-instance turn {fa}->{fb} median {np.median(rot_angle(Ma, Mb)):.4f} deg; translation column median |d| {np.median(np.linalg.norm(np.frombuffer(eb['bytes'][1], dtype=np.float32, count=n*12).reshape(n,3,4)[:,:,3] - np.frombuffer(ea['bytes'][1], dtype=np.float32, count=n*12).reshape(n,3,4)[:,:,3], axis=1)):.3f}")
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
