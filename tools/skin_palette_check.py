#!/usr/bin/env python3
r"""The offline checker for the skin ledger (src\d3d11\skin_ledger.h; docs/kinematic-motion-injection-2026-09-19.md, "F2 study").

An armed eye run writes, beside its crops, in edvr_logs\pool:
  skin_<stamp>.bin                 the palette chain's dispatches (job tables, joint matrices, bind poses) for the run's 20 frames
  bones<p>_<stamp>_<frame>.bin     every learned palette buffer, whole copy at the frame's first pool draw, the first 3 MiB kept
  pool_<stamp>_<frame>.bin         the t33 pool of each frame (the ledger's own)
This reads them and answers the F2 questions with numbers, no judgement in the middle:

  prev      Is the palette the game leaves in the buffer NOT bound at t38 during frame n bit for bit the palette that was
            bound during frame n-1, on every row frame n-1's jobs wrote?  (fRenderSkinningProcessorNode::PrevGpuTransformData)
  recompute Is each palette row the compute's own arithmetic on the captured inputs, palette[dst+i] = joint[src+i] o invBind[bind+i]
            (S o M: M's 3x3 first, then S's)?  Float32, so a tolerance, and the count of rows that are exactly equal too.
  runsum    Do the frame's jobs, by dst, each start where the last ended (dst = running sum of bone counts)?
  bases     Is every nonzero word 0 of the frame's t33 records some job's dst, and which jobs are not drawn?
  list      Between consecutive frames, which jobs persisted, moved (same size and bind pose, another dst), appeared or went;
            and of the persisted dst values, which now belong to a record that jumped more than 0.5 m (a base reused by another
            character), told apart: swaps in a frame whose list changed, and swaps in a frame whose list did NOT (the one
            that would break a join by base).
  views     The views the compute ran through (FirstElement of t0 t1 t2 u0), and whether u0 is the buffer bound at t38.

  python tools\skin_palette_check.py <dir> [stamp]       dir is edvr_logs\pool; the newest skin_*.bin when no stamp
  python tools\skin_palette_check.py --self-test         builds synthetic runs in a temp directory and requires the verdicts
  python tools\skin_palette_check.py --verify-fixture <dir>   reads the skin ledger rig's fixture (C++ writer) and requires them
  --strict                                               exit 1 when a hard check fails (a finding, not a tool error)

Exit 0 unless --strict and a hard check failed, or the files cannot be read (2).
"""
import argparse
import os
import re
import struct
import sys
import tempfile
import shutil

import numpy as np

FRAMES = 20
ROW = 48
JOB = 16
POOL_REC = 336
MAGIC = b"EDVRSKN1"
HEADER = struct.Struct("<8s8I2Q4Q4I")
BIND = struct.Struct("<Q5I")
FRAMEHDR = struct.Struct("<IIiIQ10I")
DISP = struct.Struct("<4I4Q16I2i2I")
assert HEADER.size == 104 and FRAMEHDR.size == 64 and DISP.size == 128 and BIND.size == 28
TOL_ABS = 2e-5
TOL_REL = 1e-5
JUMP_M = 0.5


# ---- reading ----------------------------------------------------------------------------------------------------------------
class Dispatch:
    pass


def read_skin(path):
    b = open(path, "rb").read()
    h = HEADER.unpack_from(b, 0)
    if h[0] != MAGIC:
        raise ValueError("%s: not a skin ledger file (magic %r)" % (path, h[0]))
    (_, version, frames, frame0, npal, keep, row, job, nbinds, chain, clear) = h[:11]
    ids = list(h[11:15])
    pbytes = list(h[15:19])
    if version != 1 or row != ROW or job != JOB:
        raise ValueError("%s: version %d, row %d, job %d: not the layout this reads" % (path, version, row, job))
    at = HEADER.size
    binds = []
    for _ in range(nbinds):
        (bid, first, num, stride, valid, n) = BIND.unpack_from(b, at)
        at += BIND.size
        binds.append(dict(id=bid, first=first, num=num, stride=stride, valid=valid, data=b[at:at + n]))
        at += n
    frames_out = []
    for _ in range(frames):
        f = FRAMEHDR.unpack_from(b, at)
        at += FRAMEHDR.size
        # frame, flags, bound, t38First, boundId, t38Num, t38Valid, dispatches, clears, chainSeen, chainOver, palIssued, palGot, nKept, lost
        fr = dict(frame=f[0], pool=bool(f[1] & 1), bound=f[2], boundId=f[4], t38=(f[3], f[5], f[6]), dispatches=f[7], clears=f[8], chainSeen=f[9],
                  chainOver=f[10], palIssued=f[11], palGot=f[12], lost=f[14], d=[])
        for _k in range(f[13]):
            v = DISP.unpack_from(b, at)
            at += DISP.size
            d = Dispatch()
            d.x, d.y, d.z, d.groups = v[0:4]
            d.t0, d.t1, d.t2, d.u0 = v[4:8]
            views = [v[8 + 4 * i:12 + 4 * i] for i in range(4)]
            d.v0, d.v1, d.v2, d.vu = views
            d.bind, d.jointsFrom, jb, tb = v[24], v[25], v[26], v[27]
            d.jobs = b[at:at + jb]
            at += jb
            d.joints = b[at:at + tb]
            at += tb
            fr["d"].append(d)
        frames_out.append(fr)
    if at != len(b):
        raise ValueError("%s: %d bytes left over after the last frame" % (path, len(b) - at))
    return dict(version=version, frame0=frame0, npal=npal, keep=keep, nbinds=nbinds, chain=chain, clear=clear, ids=ids, pbytes=pbytes,
                binds=binds, frames=frames_out)


def read_pool(path):
    b = open(path, "rb").read()
    magic, ver, frame, pool_bytes, scene_bytes, rec_bytes, records = struct.unpack_from("<8sIIIIII", b, 0)
    if magic != b"EDVRPOOL" or rec_bytes != POOL_REC:
        raise ValueError("%s: not a pool copy" % path)
    raw = np.frombuffer(b, dtype=np.uint8, count=records * POOL_REC, offset=32 + scene_bytes).reshape(records, POOL_REC)
    return raw


def jobs_of(d):
    n = min(d.groups, len(d.jobs) // JOB)
    return np.frombuffer(d.jobs, dtype="<u4", count=n * 4).reshape(n, 4) if n else np.zeros((0, 4), dtype="<u4")


def frame_jobs(fr):
    """All jobs of a frame, rows (src, dst, bind, count, dispatch index)."""
    rows = []
    for i, d in enumerate(fr["d"]):
        j = jobs_of(d)
        if len(j):
            rows.append(np.concatenate([j, np.full((len(j), 1), i, dtype="<u4")], axis=1))
    return np.concatenate(rows, axis=0) if rows else np.zeros((0, 5), dtype="<u4")


def bones(dirpath, stamp, p, frame):
    path = os.path.join(dirpath, "bones%d_%s_%d.bin" % (p, stamp, frame))
    return np.fromfile(path, dtype=np.uint8) if os.path.exists(path) else None


def rows_f32(raw):
    n = len(raw) // ROW
    return raw[:n * ROW].view("<f4").reshape(n, 3, 4)


# ---- the checks --------------------------------------------------------------------------------------------------------------
def recompute_rows(joints_raw, bind_raw, j, d, bind_rec):
    """Rows for one job: S o M, S the joint matrices at src (t2, 48 B), M the bind pose at bindBase + 3i (t1, 16 B x 3)."""
    src, dst, bb, count = (int(x) for x in j[:4])
    J = np.frombuffer(joints_raw, dtype="<f4")
    B = np.frombuffer(bind_raw, dtype="<f4")
    j0 = (d.v2[0] + src) * 12                 # t2 is 48-byte elements, a bone each
    m0 = (bind_rec["first"] + bb) * 4         # t1 is 16-byte elements; bone i's three float4s start at bindBase + 3 i
    out = np.zeros((count, 3, 4), dtype=np.float32)
    ok = np.zeros(count, dtype=bool)
    n = max(0, min(count, (len(J) - j0) // 12, (len(B) - m0) // 12)) if j0 >= 0 and m0 >= 0 else 0
    if n:
        S = J[j0:j0 + 12 * n].reshape(n, 3, 4)
        M = B[m0:m0 + 12 * n].reshape(n, 3, 4)
        out[:n, :, :3] = np.matmul(S[:, :, :3], M[:, :, :3])
        out[:n, :, 3] = np.einsum("nij,nj->ni", S[:, :, :3], M[:, :, 3]) + S[:, :, 3]
        ok[:n] = True
    return out, ok


def run_checks(dirpath, stamp):
    s = read_skin(os.path.join(dirpath, "skin_%s.bin" % stamp))
    frames = s["frames"]
    f0 = s["frame0"]
    res = {"stamp": stamp, "frames": len(frames), "findings": [], "checks": {}}
    fails = []

    def check(name, ok, detail, hard=True):
        res["checks"][name] = dict(ok=bool(ok), detail=detail, hard=hard)
        if hard and not ok:
            fails.append(name)

    # ---- recompute
    rows_cmp = rows_exact = rows_bad = rows_skipped = 0
    worst = 0.0
    for fr in frames:
        if fr["bound"] < 0:
            continue
        pal = bones(dirpath, stamp, fr["bound"], fr["frame"])
        if pal is None:
            continue
        P = rows_f32(pal)
        for i, d in enumerate(fr["d"]):
            jf = d.jointsFrom
            if jf < 0 or d.bind < 0 or d.bind >= len(s["binds"]):
                continue
            joints = fr["d"][jf].joints
            bind = s["binds"][d.bind]
            if not joints or not bind["data"] or not d.jobs:
                continue
            for j in jobs_of(d):
                out, ok = recompute_rows(joints, bind["data"], j, d, bind)
                dst = int(j[1]) + d.vu[0]
                n = len(out)
                avail = np.zeros(n, dtype=bool)
                hi = max(0, min(n, len(P) - dst)) if dst >= 0 else 0
                avail[:hi] = True
                use = ok & avail
                rows_skipped += int(n - use.sum())
                if not use.any():
                    continue
                got = P[dst:dst + hi][use[:hi]]
                want = out[:hi][use[:hi]]
                diff = np.max(np.abs(got - want), axis=(1, 2))
                tol = TOL_ABS + TOL_REL * np.max(np.abs(want), axis=(1, 2))
                rows_cmp += len(diff)
                worst = max(worst, float(diff.max()))
                rows_exact += int(np.sum(np.all(got == want, axis=(1, 2))))
                rows_bad += int(np.sum(diff > tol))
    check("recompute", rows_cmp > 0 and rows_bad == 0,
          "%d rows recomputed from t0/t1/t2, %d exactly equal, %d off by more than the tolerance, %d not checkable (beyond the kept prefix or inputs short); "
          "largest difference %.3g" % (rows_cmp, rows_exact, rows_bad, rows_skipped, worst))

    # ---- prev
    pairs = pairs_ok = rows_total = rows_equal = 0
    alt_ok = alt_total = 0
    id_ok = id_total = 0
    nonwritten_equal = nonwritten_total = 0
    bad_pairs = []
    for n in range(1, len(frames)):
        fn, fp = frames[n], frames[n - 1]
        if fp["bound"] < 0 or fn["bound"] < 0 or s["npal"] < 2:
            continue
        other = [p for p in range(s["npal"]) if p != fn["bound"]]
        if not other:
            continue
        o = other[0]
        alt_total += 1
        alt_ok += (fp["bound"] == o)
        if s["ids"][o] and fp["boundId"]:
            id_total += 1
            id_ok += (s["ids"][o] == fp["boundId"])
        A = bones(dirpath, stamp, o, fn["frame"])
        B = bones(dirpath, stamp, fp["bound"], fp["frame"])
        if A is None or B is None:
            continue
        PA, PB = rows_f32(A), rows_f32(B)
        jp = frame_jobs(fp)
        written = np.zeros(min(len(PA), len(PB)), dtype=bool)
        for j in jp:
            lo = int(j[1]) + fp["d"][int(j[4])].vu[0]   # rows are addressed through the UAV's view: FirstElement is 0 unless the log says otherwise
            hi = min(lo + int(j[3]), len(written))
            if lo < hi:
                written[lo:hi] = True
        pairs += 1
        wr = int(written.sum())
        eq = int(np.sum(np.all(PA[:len(written)][written].view(np.uint32).reshape(-1, 12) == PB[:len(written)][written].view(np.uint32).reshape(-1, 12), axis=1))) if wr else 0
        rows_total += wr
        rows_equal += eq
        full = min(len(PA), len(PB))
        nonw = ~written
        nonwritten_total += int(nonw.sum())
        nonwritten_equal += int(np.sum(np.all(PA[:full][nonw].view(np.uint32).reshape(-1, 12) == PB[:full][nonw].view(np.uint32).reshape(-1, 12), axis=1)))
        if wr and eq == wr:
            pairs_ok += 1
        elif wr:
            bad_pairs.append((fn["frame"], wr - eq, wr))
    check("prev", pairs > 0 and pairs_ok == pairs,
          "other buffer during frame n == the buffer bound in frame n-1, bit for bit on the rows frame n-1's jobs wrote: %d of %d frame pairs equal on every such row "
          "(%d of %d rows)%s" % (pairs_ok, pairs, rows_equal, rows_total, "" if not bad_pairs else "; differing pairs (frame, rows off, rows): %s" % bad_pairs[:6]))
    check("prev_alternation", alt_total > 0 and alt_ok == alt_total,
          "the buffer not bound in frame n is the one that was bound in frame n-1 (by palette index) in %d of %d frames; by buffer identity in %d of %d" %
          (alt_ok, alt_total, id_ok, id_total))
    res["checks"]["prev_other_rows"] = dict(ok=True, hard=False,
                                           detail="rows no job of frame n-1 wrote: %d of %d identical between the two buffers (informational: stale rows are not the question)" %
                                           (nonwritten_equal, nonwritten_total))

    # ---- running sum
    gaps = overlaps = frames_run = firsts = 0
    first_dst = set()
    for fr in frames:
        j = frame_jobs(fr)
        j = j[j[:, 3] > 0] if len(j) else j
        if not len(j):
            continue
        frames_run += 1
        order = np.argsort(j[:, 1], kind="stable")
        js = j[order]
        first_dst.add(int(js[0, 1]))
        for k in range(1, len(js)):
            end = int(js[k - 1, 1]) + int(js[k - 1, 3])
            if int(js[k, 1]) > end:
                gaps += 1
            elif int(js[k, 1]) < end:
                overlaps += 1
    check("runsum", frames_run > 0 and gaps == 0 and overlaps == 0,
          "jobs by dst, each starting where the last ended: %d gaps and %d overlaps over %d frames; first dst values seen %s" %
          (gaps, overlaps, frames_run, sorted(first_dst)[:6]))

    # ---- bases vs dst
    pool = {}
    for fr in frames:
        path = os.path.join(dirpath, "pool_%s_%d.bin" % (stamp, fr["frame"]))
        if os.path.exists(path):
            pool[fr["frame"]] = read_pool(path)
    not_dst = skinned = undrawn = frames_pool = 0
    for fr in frames:
        raw = pool.get(fr["frame"])
        if raw is None:
            continue
        frames_pool += 1
        w0 = np.ascontiguousarray(raw[:, :4]).view("<u4").ravel()
        bases = set(int(x) for x in w0 if x)
        jf = frame_jobs(fr)
        dsts = set(int(x) for x in jf[:, 1]) if len(jf) else set()
        skinned += len(bases)
        not_dst += len(bases - dsts)
        undrawn += len(dsts - bases)
    check("bases", frames_pool > 0 and not_dst == 0,
          "t33 records' nonzero word 0 that is not a job's dst: %d of %d distinct bases over %d frames (jobs with no record in the pool: %d, expected for characters not drawn)" %
          (not_dst, skinned, frames_pool, undrawn))

    # ---- list changes and identity swaps
    changed_frames = []
    swaps_explained = swaps_unexplained = pairs_l = 0
    detail_swaps = []
    for n in range(1, len(frames)):
        a, b = frame_jobs(frames[n - 1]), frame_jobs(frames[n])
        if not len(a) or not len(b):
            continue
        pairs_l += 1
        ka = {}
        for r in a:
            ka.setdefault((int(r[2]), int(r[3])), []).append(int(r[1]))
        moved = new = same = 0
        for r in b:
            key = (int(r[2]), int(r[3]))
            lst = ka.get(key, [])
            if int(r[1]) in lst:
                lst.remove(int(r[1]))
                same += 1
            elif lst:
                lst.pop(0)
                moved += 1
            else:
                new += 1
        gone = sum(len(v) for v in ka.values())
        changed = bool(moved or new or gone)
        if changed:
            changed_frames.append((frames[n]["frame"], same, moved, new, gone))
        # persisted dst values whose record jumped
        ra, rb = pool.get(frames[n - 1]["frame"]), pool.get(frames[n]["frame"])
        if ra is None or rb is None:
            continue

        def first_pos(raw):
            w0 = np.ascontiguousarray(raw[:, :4]).view("<u4").ravel()
            pos = np.ascontiguousarray(raw[:, 16:28]).view("<f4").reshape(-1, 3)
            out = {}
            for i in range(len(w0) - 1, -1, -1):
                if w0[i]:
                    out[int(w0[i])] = pos[i]
            return out

        pa, pb = first_pos(ra), first_pos(rb)
        sw = [bse for bse in set(pa) & set(pb) if float(np.linalg.norm(pa[bse].astype(np.float64) - pb[bse])) > JUMP_M]
        if sw:
            if changed:
                swaps_explained += len(sw)
            else:
                swaps_unexplained += len(sw)
            detail_swaps.append((frames[n]["frame"], len(sw), "list changed" if changed else "LIST UNCHANGED"))
    check("list_identity", pairs_l > 0 and swaps_unexplained == 0,
          "%d frame pairs: list changed in %d (frame, same, moved, new, gone): %s; persisted bases whose record jumped over %.1f m: %d in a changed-list frame, "
          "%d with the list UNCHANGED (the case a join by base cannot survive)%s" %
          (pairs_l, len(changed_frames), changed_frames[:8], JUMP_M, swaps_explained, swaps_unexplained,
           "" if not detail_swaps else "; at %s" % detail_swaps[:6]))
    res["changed_frames"] = changed_frames
    res["swaps"] = dict(explained=swaps_explained, unexplained=swaps_unexplained)

    # ---- views and u0
    firsts = {}
    u0_is_bound = u0_total = 0
    for fr in frames:
        for d in fr["d"]:
            firsts[(d.v0[0], d.v1[0], d.v2[0], d.vu[0])] = firsts.get((d.v0[0], d.v1[0], d.v2[0], d.vu[0]), 0) + 1
            if fr["boundId"]:
                u0_total += 1
                u0_is_bound += (d.u0 == fr["boundId"])
    t38_firsts = sorted({fr["t38"][0] for fr in frames if fr["pool"] and fr["t38"][2]})
    check("views", u0_total > 0 and u0_is_bound == u0_total,
          "FirstElement of (t0, t1, t2, u0) over the chain dispatches (view -> dispatches): %s; the pool draws' t38 view starts at element(s) %s; "
          "the compute's u0 is the buffer bound at t38 in %d of %d dispatches" % (dict(list(firsts.items())[:4]), t38_firsts, u0_is_bound, u0_total))
    res["fails"] = fails
    return res


def print_report(res, out=sys.stdout):
    print("skin palette check, run %s (%d frames)" % (res["stamp"], res["frames"]), file=out)
    for name, c in res["checks"].items():
        tag = "INFO" if not c["hard"] else ("PASS" if c["ok"] else "FAIL")
        print("  %-18s %s  %s" % (name, tag, c["detail"]), file=out)
    print("RESULT: %s" % ("every hard check passed" if not res["fails"] else "FAILED: " + ", ".join(res["fails"])), file=out)


def newest_stamp(dirpath):
    cands = [f for f in os.listdir(dirpath) if re.match(r"skin_\d+\.bin$", f)]
    if not cands:
        raise ValueError("no skin_<stamp>.bin in %s" % dirpath)
    cands.sort(key=lambda f: os.path.getmtime(os.path.join(dirpath, f)))
    return cands[-1][len("skin_"):-len(".bin")]


# ---- the synthetic run (an independent writer of the same layout: the checker is tested on it, and the C++ writer's fixture is
# ---- read by the same reader, so a layout that drifts on either side fails in the build) --------------------------------------------
def compose(S, M):
    out = np.zeros((3, 4), dtype=np.float32)
    out[:, :3] = S[:, :3] @ M[:, :3]
    out[:, 3] = S[:, :3] @ M[:, 3] + S[:, 3]
    return out


def rot(angle, t):
    c, s = np.cos(angle), np.sin(angle)
    return np.array([[c, -s, 0, t[0]], [s, c, 0, t[1]], [0, 0, 1, t[2]]], dtype=np.float32)


def write_skin(path, frame0, runs, binds, ids, paletteBytes):
    """runs: per frame dict(bound, boundId, dispatches=[dict(jobs bytes, joints bytes, bind index, u0)], pool)"""
    o = bytearray()
    o += HEADER.pack(MAGIC, 1, FRAMES, frame0, 2, 3 << 20, ROW, JOB, len(binds), 0x6FE04AF836BB1DBA, 0x7B2A531B72941109, *ids, *[0] * (4 - len(ids)),
                     *paletteBytes, *[0] * (4 - len(paletteBytes)))
    for b in binds:
        o += BIND.pack(b["id"], 0, len(b["data"]) // 16, 16, 1, len(b["data"]))
        o += b["data"]
    for k in range(FRAMES):
        fr = runs[k]
        o += FRAMEHDR.pack(frame0 + k, 1 if fr["pool"] else 0, fr["bound"], 0, fr["boundId"], 64, 1, 2, 1, len(fr["dispatches"]), 0, fr["palIssued"], fr["palGot"],
                           len(fr["dispatches"]), 0)
        for d in fr["dispatches"]:
            groups = len(d["jobs"]) // JOB
            o += DISP.pack(groups, 1, 1, groups, 0x1000, 0x2000, 0x3000, d["u0"], 0, 0, 16, 1, 0, 0, 16, 1, 0, 0, 48, 1, 0, 0, 48, 1,
                           d["bind"], 0 if d["joints"] else d["jointsFrom"], len(d["jobs"]), len(d["joints"]))
            o += d["jobs"] + d["joints"]
    open(path, "wb").write(bytes(o))


def synth_run(dirpath, stamp="777777", frame0=300, mutate=None):
    """A 20-frame run: three characters (5, 4, 6 bones; the middle one leaves at frame 12 so the list shifts, a fourth joins at 15);
    two palette buffers alternating by frame, the one not bound holding last frame's rows. `mutate` names a fault to inject."""
    rng = np.random.RandomState(7)
    bind = np.array([rot(0.1 * b, (0.01 * b, -0.02 * b, 0.5)) for b in range(21)], dtype=np.float32)
    bind_bytes = bind.tobytes()   # bone b's three float4s are t1 elements 3b..3b+2: a job's bindBase is 3 x its first bone
    rows = 64
    pal = [np.zeros((rows, 3, 4), dtype=np.float32), np.zeros((rows, 3, 4), dtype=np.float32)]
    runs = []
    prev_jobs = None
    for k in range(FRAMES):
        f = frame0 + k
        ents = [(0, 5, 0, True), (1, 4, 15, k < 12), (2, 6, 27, True)] + ([(3, 6, 45, True)] if k >= 15 else [])
        jobs, ids_, src, dst = [], [], 0, 1
        for eid, cnt, bb, alive in ents:
            if not alive:
                continue
            jobs.append((src, dst, bb, cnt))
            ids_.append(eid)
            src += cnt
            dst += cnt
        if mutate == "gap" and k == 6:
            jobs[2] = (jobs[2][0], jobs[2][1] + 3, jobs[2][2], jobs[2][3])
        joints = np.array([rot(0.05 * k + 0.02 * r, (0.1 * r, 0.03 * k, 0.2)) for r in range(src)], dtype=np.float32)
        b = k % 2
        cur = pal[b].copy()
        for (s_, d_, bb, cnt) in jobs:
            for i in range(cnt):
                cur[d_ + i] = compose(joints[s_ + i], bind[bb // 3 + i])
        if mutate == "recompute" and k == 9:
            cur[jobs[0][1]][0, 0] += 0.5
        pal[b] = cur
        recs = []
        bases = [0, 0, 0]
        pos = [(0, 0, 0)] * 3
        for (s_, d_, bb, cnt), eid in zip(jobs, ids_):
            bases.append(d_)
            pos.append((10 + 3 * eid + 0.01 * k, 2, -5))
            if cnt == 5:
                bases.append(d_)
                pos.append((10 + 3 * eid + 0.01 * k, 2, -5))
        if mutate == "swap" and k == 8:   # a record at an unchanged list jumps: identity swap with the list UNCHANGED
            pos[3] = (40.0, 2, -5)
        if mutate == "notdst" and k == 4:
            bases.append(9999)
            pos.append((1, 1, 1))
        poolrec = bytearray(len(bases) * POOL_REC)
        for r, (bs, ps) in enumerate(zip(bases, pos)):
            struct.pack_into("<I", poolrec, r * POOL_REC, bs)
            struct.pack_into("<3f", poolrec, r * POOL_REC + 16, *ps)
        hdr = struct.pack("<8sIIIIII", b"EDVRPOOL", 1, f, len(poolrec), 0, POOL_REC, len(bases))
        open(os.path.join(dirpath, "pool_%s_%d.bin" % (stamp, f)), "wb").write(hdr + bytes(poolrec))
        jb = b"".join(struct.pack("<4I", *j) for j in jobs)
        runs.append(dict(pool=True, bound=b, boundId=0xA0A0 if b == 0 else 0xB0B0, palIssued=2 if k else 1, palGot=2 if k else 1,
                         dispatches=[dict(jobs=jb, joints=joints.tobytes(), bind=0, jointsFrom=0, u0=0xA0A0 if b == 0 else 0xB0B0)]))
        for p in range(2):
            if k == 0 and p != b:
                continue
            data = pal[p].tobytes()
            if mutate == "prev" and k == 11 and p != b:
                a = bytearray(data)
                struct.pack_into("<f", a, jobs[0][1] * ROW, 123.0)   # corrupt one written row of last frame in the other buffer
                data = bytes(a)
            if mutate == "nofile" and k == 3 and p != b:
                continue
            open(os.path.join(dirpath, "bones%d_%s_%d.bin" % (p, stamp, f)), "wb").write(data)
    write_skin(os.path.join(dirpath, "skin_%s.bin" % stamp), frame0, runs, [dict(id=0x2000, data=bind_bytes)], [0xA0A0, 0xB0B0], [rows * ROW, rows * ROW])
    return stamp


# ---- the self-test and the fixture gate ----------------------------------------------------------------------------------------
def self_test():
    failures = []

    def expect(ok, what):
        if not ok:
            failures.append(what)

    def verdict(mutate):
        d = tempfile.mkdtemp(prefix="skinchk_")
        try:
            stamp = synth_run(d, mutate=mutate)
            return run_checks(d, stamp)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    healthy = verdict(None)
    expect(not healthy["fails"], "a healthy synthetic run passes every hard check: %s" % healthy["fails"])
    c = healthy["checks"]
    expect("19 of 19 frame pairs" in c["prev"]["detail"], "the healthy run's Prev check covers 19 frame pairs: %s" % c["prev"]["detail"])
    expect(healthy["swaps"]["explained"] >= 1 and healthy["swaps"]["unexplained"] == 0, "the healthy run's character swap is explained by its list change: %s" % healthy["swaps"])
    expect(any(x[0] == 312 for x in healthy["changed_frames"]) and any(x[0] == 315 for x in healthy["changed_frames"]),
           "the list changes at frames 312 (a character left) and 315 (one joined): %s" % healthy["changed_frames"])
    expect("exactly equal" in c["recompute"]["detail"] and " 0 off by more" in c["recompute"]["detail"], "the healthy recompute matches every row: %s" % c["recompute"]["detail"])
    for name, want, why in (("prev", "prev", "one corrupted row of the other buffer fails Prev"),
                            ("recompute", "recompute", "one corrupted palette row fails the recompute"),
                            ("gap", "runsum", "a job that starts late fails the running sum"),
                            ("swap", "list_identity", "a record that jumps with the list unchanged is an UNEXPLAINED swap"),
                            ("notdst", "bases", "a t33 base that is no job's dst fails the bases check")):
        v = verdict(name)
        expect(want in v["fails"], "%s (fails: %s)" % (why, v["fails"]))
        others = [f for f in v["fails"] if f != want]
        expect(not others, "%s trips nothing else (also failed: %s)" % (name, others))
    v = verdict("nofile")
    expect("prev" not in v["fails"], "a missing bones file skips its pair, it does not fail the run: %s" % v["fails"])
    expect("18 of 18 frame pairs" in v["checks"]["prev"]["detail"], "...and the pair count shows it: %s" % v["checks"]["prev"]["detail"])
    # the reader refuses what is not its layout
    d = tempfile.mkdtemp(prefix="skinchk_")
    try:
        p = os.path.join(d, "skin_1.bin")
        open(p, "wb").write(b"NOTASKIN" + bytes(200))
        try:
            read_skin(p)
            expect(False, "read_skin refuses a file with the wrong magic")
        except ValueError:
            pass
        stamp = synth_run(d)
        b = open(os.path.join(d, "skin_%s.bin" % stamp), "rb").read()
        open(os.path.join(d, "skin_%s.bin" % stamp), "wb").write(b + b"x")
        try:
            read_skin(os.path.join(d, "skin_%s.bin" % stamp))
            expect(False, "read_skin refuses a file with bytes left over")
        except ValueError:
            pass
        expect(newest_stamp(d) == stamp, "newest_stamp finds the run")
    finally:
        shutil.rmtree(d, ignore_errors=True)
    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: skin_palette_check self-test (a healthy synthetic run passes; six injected faults are each caught by their own check and no other)")
    return 0


def verify_fixture(dirpath):
    """The C++ rig's fixture (skin_ledger_test.exe --fixture): written by the production serialiser, read by this reader."""
    stamp = "090909"
    res = run_checks(dirpath, stamp)
    print_report(res)
    failures = []
    if res["fails"]:
        failures.append("hard checks failed on the rig's fixture: %s" % res["fails"])
    c = res["checks"]
    if "19 of 19 frame pairs" not in c["prev"]["detail"]:
        failures.append("the fixture's Prev check did not cover 19 frame pairs: %s" % c["prev"]["detail"])
    if not any(x[0] == 7012 for x in res["changed_frames"]) or not any(x[0] == 7015 for x in res["changed_frames"]):
        failures.append("the fixture's list changes at frames 7012 and 7015 were not seen: %s" % res["changed_frames"])
    if res["swaps"]["explained"] < 1 or res["swaps"]["unexplained"] != 0:
        failures.append("the fixture's swap at frame 7012 should be explained by its list change: %s" % res["swaps"])
    if " 0 off by more" not in c["recompute"]["detail"]:
        failures.append("the fixture's recompute is off: %s" % c["recompute"]["detail"])
    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: skin_palette_check read the skin ledger rig's fixture end to end")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir", nargs="?")
    ap.add_argument("stamp", nargs="?")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--verify-fixture", metavar="DIR")
    ap.add_argument("--strict", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.verify_fixture:
        return verify_fixture(a.verify_fixture)
    if not a.dir:
        ap.print_help()
        return 2
    try:
        stamp = a.stamp or newest_stamp(a.dir)
        res = run_checks(a.dir, stamp)
    except (ValueError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    print_report(res)
    return 1 if (a.strict and res["fails"]) else 0


if __name__ == "__main__":
    sys.exit(main())
