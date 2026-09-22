#!/usr/bin/env python3
"""Read the cull gate probe's capture (src/d3d11/cull_gate_probe.*,
advanced.cull_gate_capture) beside its eye run's depth, pool and ledger, and
print the inputs the occlusion-culling recall measurement needs
(docs/design-occlusion-culling-2026-09-22.md, section 9).

    python tools/cull_gate_probe.py <edvr_logs\\pool> <stamp> [<stamp> ...]
           [--frame F] [--radius 1.0] [--occluder-range 120] [--records 40]
           [--parts 40] [--parts-csv FILE]
    python tools/cull_gate_probe.py --self-test
    python tools/cull_gate_probe.py --verify-fixture <gate file>   (build.bat)

Per stamp: the t33 records the eye pass drew in frame F (default: the run's
first complete frame), seen or unseen in both eyes by the depth join
(footprint sphere of --radius metres against the stored reversed-Z depth).
When the stamp has a gate_<stamp>.bin: which engine views are the eyes
(their plane normals against each eye's clip matrix) and the frame offset
between the engine's and the pool's coordinates, the engine record behind
each t33 record (its parts' world positions matched to the pool), and per
engine record the per-eye verdicts of the three per-view tests -- the
distance/LOD mask rec+0x208 (read by each eye view's own +0x570 bits, not its
array index), the traversal gate FUN_14430EFE0, the draw-item builder's
frustum -- beside the ledger's per-eye draws of its parts, with a per-slot
tally (every claimant of a t33 slot kept, so a shadow-only twin at the same
pivot never counts against a test) that says which test admits the pool
draws: the admitting test rejects no drawn slot and admits no record with
nothing drawn. With a version 2 gate file, the same per PART: the builder's
per-(sub-item, view) test FUN_1442B3FC0 -- each part's own sphere (world
centre, radius), view and verdict joined through its builder row, entry and
sub-item to the t33 slots its position claims -- tallied against the
ledger's per-eye draws per (slot, eye), with the slots drawn in one eye
only, and --parts parts listed (one-eye parts first; --parts-csv writes
all). With version 9
geometry in drawstate_<stamp>.eyemesh.bin: the occluder set (seen records
within --occluder-range metres), its meshes' triangle counts and whether
their states are opaque and depth-writing.

gate_<stamp>.bin ('EDVRGATE' version 2; version 1 lacks every [v2] field),
little-endian, written field by field (cull_gate_probe.cpp, write()):
    8s magic, u32 version, u32 first frame, u32 last frame,
    u32 flags (bit 0: the builder's plane test ran; [v2] bit 1: the part
      test FUN_1442B3FC0 was hooked), u32 DAT_145ea3399,
    12 x u32 counters (gate calls/kept/dropped, builder calls/kept/dropped,
      entries dropped, sub-items dropped, faults, record-pose mismatches,
      view dumps, dumps dropped),
    [v2] 6 x u32 part counters (calls, kept, dropped over the cap, kept rows
      with the builder frame unverified, from a foreign caller, outside a
      kept builder row),
    u32 dumps; per dump: u64 ctx, u32 frame, u32 view count, f32 LOD scale
      (ctx+0x30), u32 faults, u32[64] bit table (ctx+0x1A840); per view:
      u32 plane count, f32[4 x count] planes, u8[0x6A0] the raw view record,
    u32 gates; per gate call: u64 record, u64 ctx, u32 frame, u32 LOD,
      u16 view, u8 passed, u8 flags,
    u32 builders; per builder call: u64 record, pose, ctx, node, active mask,
      rec+0x208; u64[4] rec+0x210; u64 bit-ok, frustum-pass, frustum-inside,
      visibility-callback masks; f32[3] +0x170; u8[8] +0x17C; f32[4] +0x240,
      +0x270, +0x280; f32[4] pose quaternion, translation, centre, extents,
      transformed centre; u32 frame, view count, flags, entry count, first
      entry, entries copied,
    u32 entries; per entry: u64 model, u32 sub-items, u32 first, u32 copied,
      [v2] f32[4] model+0x00 (the local centre), f32[4] model+0x10 ([0] the
      radius),
    u32 sub-items; per sub-item f32[4] quaternion, f32[4] position,
    [v2] u32 parts; per FUN_1442B3FC0 call: u64 pose context, u64 sub-item
      address (0 unless verified), u64 view +0x570 bits, f32[3] world centre,
      f32 radius, u32 frame, u32 builder row (index in this file, 0xFFFFFFFF
      none), u32 entry index, u32 sub-item index (0xFFFFFFFF unverified),
      u32 LOD, u16 view, u8 passed, u8 flags (1 view not in the context's
      array, 2 builder frame unverified, 4 foreign caller, 8 builder row's
      pose differs, 16 sphere unreadable),
    4s 'EDVE'.
"""
import argparse
import io
import math
import os
import struct
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

MAGIC = b'EDVRGATE'
VERSION = 2
VERSIONS = (1, 2)
VIEW_BYTES = 0x6A0
COUNTERS = ('gate_calls', 'gate_kept', 'gate_dropped', 'builder_calls', 'builder_kept', 'builder_dropped',
            'entries_dropped', 'subitems_dropped', 'faults', 'record_mismatch', 'dumps', 'dumps_dropped')
PART_COUNTERS = ('part_calls', 'part_kept', 'part_dropped', 'part_unverified', 'part_foreign', 'part_unlinked')
BUILDER_FIELDS = ('record', 'pose', 'ctx', 'node', 'active_mask', 'rec_mask')
NO_ROW = 0xFFFFFFFF
PART_ROW = '<3Q4f5IHBB'   # 64 bytes
PART_VIEW_FOREIGN, PART_UNVERIFIED, PART_FOREIGN_CALLER, PART_OWNER_MISMATCH, PART_SPHERE_FAULT = 1, 2, 4, 8, 16
FLAG_PART_HOOKED = 2
NEAR = 0.025
POOL_RECORD = 336


# --------------------------------------------------------------------------- the file

def decode_view(raw):
    """The fields of a 0x6A0 view record the decompiles name (§9 close-out 1)."""
    return dict(camera=list(struct.unpack_from('<4f', raw, 0x540)),
                lod_scale=struct.unpack_from('<f', raw, 0x550)[0],
                lod_bias=struct.unpack_from('<f', raw, 0x560)[0],
                bits=struct.unpack_from('<Q', raw, 0x570)[0],
                bit_index=struct.unpack_from('<I', raw, 0x578)[0],
                lod_mask=struct.unpack_from('<Q', raw, 0x580)[0],
                flags=struct.unpack_from('<I', raw, 0x688)[0],
                flag68d=raw[0x68D])


def read_gate(path):
    data = open(path, 'rb').read()
    s = io.BytesIO(data)

    def take(n, what):
        b = s.read(n)
        if len(b) != n:
            raise ValueError('Truncated gate file: %s' % what)
        return b

    def unpack(fmt, what):
        return struct.unpack(fmt, take(struct.calcsize(fmt), what))
    if take(8, 'magic') != MAGIC:
        raise ValueError('Not an EDVRGATE file')
    version, = unpack('<I', 'version')
    if version not in VERSIONS:
        raise ValueError('Unsupported gate file version %d' % version)
    first, last, flags, vis = unpack('<4I', 'header')
    counts = dict(zip(COUNTERS, unpack('<12I', 'counters')))
    if version >= 2:
        counts.update(zip(PART_COUNTERS, unpack('<6I', 'part counters')))
    ndumps, = unpack('<I', 'dump count')
    if ndumps > 16:
        raise ValueError('Invalid dump count %d' % ndumps)
    dumps = []
    for _ in range(ndumps):
        ctx, frame, count = unpack('<QII', 'dump head')
        lod, = unpack('<f', 'dump lod')
        faults, = unpack('<I', 'dump faults')
        table = list(unpack('<64I', 'bit table'))
        if count > 64:
            raise ValueError('Invalid view count %d' % count)
        views = []
        for _ in range(count):
            pc, = unpack('<I', 'plane count')
            if pc > 32:
                raise ValueError('Invalid plane count %d' % pc)
            planes = [list(unpack('<4f', 'plane')) for _ in range(pc)]
            raw = take(VIEW_BYTES, 'view record')
            v = decode_view(raw)
            v.update(planes=planes, raw=raw)
            views.append(v)
        dumps.append(dict(ctx=ctx, frame=frame, lod_scale=lod, faults=faults, bit_table=table, views=views))
    ngates, = unpack('<I', 'gate count')
    gates = []
    for _ in range(ngates):
        record, ctx, frame, lod, view, passed, gflags = unpack('<QQIIHBB', 'gate')
        gates.append(dict(record=record, ctx=ctx, frame=frame, lod=lod, view=view, passed=passed, flags=gflags))
    nb, = unpack('<I', 'builder count')
    builders = []
    for index in range(nb):
        b = dict(zip(BUILDER_FIELDS, unpack('<6Q', 'builder head')))
        b['index'] = index
        b['nibbles'] = list(unpack('<4Q', 'nibbles'))
        b['bit_ok'], b['frustum_pass'], b['frustum_inside'], b['vis_applies'] = unpack('<4Q', 'view masks')
        b['position'] = list(unpack('<3f', 'position'))
        b['quat'] = take(8, 'quat')
        for name in ('world_centre', 'local_centre', 'radius', 'pose_quat', 'pose_t', 'pose_centre',
                     'pose_extents', 'centre'):
            b[name] = list(unpack('<4f', name))
        (b['frame'], b['view_count'], b['flags'], b['entry_count'], b['entry_first'],
         b['entries_copied']) = unpack('<6I', 'builder tail')
        builders.append(b)
    ne, = unpack('<I', 'entry count')
    entries = []
    for _ in range(ne):
        model, sub_count, sub_first, sub_copied = unpack('<Q3I', 'entry')
        e = dict(model=model, sub_count=sub_count, sub_first=sub_first, sub_copied=sub_copied)
        if version >= 2:
            e['model_centre'] = list(unpack('<4f', 'model centre'))
            e['model_sphere'] = list(unpack('<4f', 'model sphere'))
        entries.append(e)
    ns, = unpack('<I', 'sub-item count')
    subs = [unpack('<8f', 'sub-item') for _ in range(ns)]
    parts = []
    if version >= 2:
        np_, = unpack('<I', 'part count')
        blob = take(np_ * struct.calcsize(PART_ROW), 'part rows')
        for row in struct.iter_unpack(PART_ROW, blob):
            (pose, sub_item, view_bits, cx, cy, cz, radius, frame, builder, entry, sub, lod,
             view, passed, pflags) = row
            parts.append(dict(pose=pose, sub_item=sub_item, view_bits=view_bits, centre=[cx, cy, cz],
                              radius=radius, frame=frame, builder=builder, entry=entry, sub=sub, lod=lod,
                              view=view, passed=passed, flags=pflags))
    if take(4, 'end marker') != b'EDVE':
        raise ValueError('Missing end marker')
    if s.read(1):
        raise ValueError('Trailing bytes after the end marker')
    for b in builders:
        if b['entries_copied'] and b['entry_first'] + b['entries_copied'] > ne:
            raise ValueError('Builder entries out of range')
    for e in entries:
        if e['sub_copied'] and e['sub_first'] + e['sub_copied'] > ns:
            raise ValueError('Entry sub-items out of range')
    for p in parts:
        if p['builder'] != NO_ROW and p['builder'] >= nb:
            raise ValueError('Part row names builder row %d of %d' % (p['builder'], nb))
    return dict(version=version, first=first, last=last, flags=flags, vis_global=vis, counts=counts, dumps=dumps,
                gates=gates, builders=builders, entries=entries, subs=subs, parts=parts)


def write_gate(g):
    """The Python mirror of CullGateProbe::write, for the self-test; writes
    g['version'] (default the current one), so version 1 stays testable."""
    version = g.get('version', VERSION)
    out = io.BytesIO()
    out.write(MAGIC + struct.pack('<5I', version, g['first'], g['last'], g['flags'], g['vis_global']))
    out.write(struct.pack('<12I', *[g['counts'][k] for k in COUNTERS]))
    if version >= 2:
        out.write(struct.pack('<6I', *[g['counts'][k] for k in PART_COUNTERS]))
    out.write(struct.pack('<I', len(g['dumps'])))
    for d in g['dumps']:
        out.write(struct.pack('<QIIfI', d['ctx'], d['frame'], len(d['views']), d['lod_scale'], d['faults']))
        out.write(struct.pack('<64I', *d['bit_table']))
        for v in d['views']:
            out.write(struct.pack('<I', len(v['planes'])))
            for p in v['planes']:
                out.write(struct.pack('<4f', *p))
            out.write(v['raw'])
    out.write(struct.pack('<I', len(g['gates'])))
    for x in g['gates']:
        out.write(struct.pack('<QQIIHBB', x['record'], x['ctx'], x['frame'], x['lod'], x['view'], x['passed'], x['flags']))
    out.write(struct.pack('<I', len(g['builders'])))
    for b in g['builders']:
        out.write(struct.pack('<6Q', *[b[k] for k in BUILDER_FIELDS]))
        out.write(struct.pack('<4Q', *b['nibbles']))
        out.write(struct.pack('<4Q', b['bit_ok'], b['frustum_pass'], b['frustum_inside'], b['vis_applies']))
        out.write(struct.pack('<3f', *b['position']) + b['quat'])
        for name in ('world_centre', 'local_centre', 'radius', 'pose_quat', 'pose_t', 'pose_centre',
                     'pose_extents', 'centre'):
            out.write(struct.pack('<4f', *b[name]))
        out.write(struct.pack('<6I', b['frame'], b['view_count'], b['flags'], b['entry_count'], b['entry_first'],
                              b['entries_copied']))
    out.write(struct.pack('<I', len(g['entries'])))
    for e in g['entries']:
        out.write(struct.pack('<Q3I', e['model'], e['sub_count'], e['sub_first'], e['sub_copied']))
        if version >= 2:
            out.write(struct.pack('<4f', *e['model_centre']) + struct.pack('<4f', *e['model_sphere']))
    out.write(struct.pack('<I', len(g['subs'])))
    for s in g['subs']:
        out.write(struct.pack('<8f', *s))
    if version >= 2:
        out.write(struct.pack('<I', len(g['parts'])))
        for p in g['parts']:
            out.write(struct.pack(PART_ROW, p['pose'], p['sub_item'], p['view_bits'], *p['centre'], p['radius'],
                                  p['frame'], p['builder'], p['entry'], p['sub'], p['lod'], p['view'], p['passed'],
                                  p['flags']))
    out.write(b'EDVE')
    return out.getvalue()


# --------------------------------------------------------------------------- geometry helpers

def unit(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v] if n > 1e-12 else [0.0, 0.0, 0.0]


def side_planes(cols):
    """The four side-plane normals of a clip matrix given as its columns
    (clip = x*c0 + y*c1 + z*c2 + c3, cb1[270..273]): rows r0, r1, r3."""
    r = [[cols[0][j], cols[1][j], cols[2][j]] for j in range(4)]
    return [unit([r[3][k] + s * r[i][k] for k in range(3)]) for i in (0, 1) for s in (1.0, -1.0)]


def plane_match(eye_normals, planes):
    """Mean over the eye's side planes of the best |cos| against the view's
    planes: 1.0 when the view's frustum has the eye's orientation."""
    ns = [unit(p[:3]) for p in planes]
    if not ns:
        return 0.0
    return sum(max(abs(sum(a * b for a, b in zip(e, n))) for n in ns) for e in eye_normals) / len(eye_normals)


def decode_quat16(raw):
    """The pool's and the record's packed quaternion: unorm16 x4, q = u*2/65535-1."""
    xy, zw = struct.unpack('<II', raw)
    q = [(xy & 0xFFFF) * (2.0 / 65535) - 1, (xy >> 16) * (2.0 / 65535) - 1,
         (zw & 0xFFFF) * (2.0 / 65535) - 1, (zw >> 16) * (2.0 / 65535) - 1]
    n = math.sqrt(sum(c * c for c in q))
    return [c / n for c in q] if n > 1e-6 else [0.0, 0.0, 0.0, 1.0]


def rotate(q, v, conjugate=False):
    x, y, z, w = q
    if conjugate:
        x, y, z = -x, -y, -z
    tx = 2 * (y * v[2] - z * v[1])
    ty = 2 * (z * v[0] - x * v[2])
    tz = 2 * (x * v[1] - y * v[0])
    return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)]


def keyed_part_positions(builder, entries, subs, conjugate):
    """(entry index, sub-item index, world position) of an engine record's
    parts: its pose (+0x170 position, +0x17C quaternion) applied to each
    sub-item's local position. The indices are the ones a version 2 part row
    carries: the entry's place in the pose context's array, the sub-item's in
    the entry's (the probe copies both from the start, in order)."""
    q = decode_quat16(builder['quat'])
    t = builder['position']
    out = []
    for ei, e in enumerate(entries[builder['entry_first']:builder['entry_first'] + builder['entries_copied']]):
        for si, s in enumerate(subs[e['sub_first']:e['sub_first'] + e['sub_copied']]):
            p = rotate(q, s[4:7], conjugate)
            out.append((ei, si, [p[0] + t[0], p[1] + t[1], p[2] + t[2]]))
    return out


def part_positions(builder, entries, subs, conjugate):
    """World positions of an engine record's parts (keyed_part_positions without the keys)."""
    return [p for _, _, p in keyed_part_positions(builder, entries, subs, conjugate)]


def mask_admits(mask, view_bits):
    """A view mask (rec+0x208, the builder's active mask) admits a view when
    it shares a bit with the view's own bits (+0x570). The bits are NOT the
    view's array index: on 152632 eye A is view 0 with bit 1 and eye B is
    view 5 with bit 22 (decomp_42B4420.txt:250-275 tests view+0x570 & mask)."""
    return bool(mask & view_bits)


def tally(claims, drawn, verdict):
    """Per test, over (t33 slot, eye) and (engine record, eye) pairs:
    'rejected, drawn' -- a slot drawn in an eye whose every claiming engine
    record the test rejects there (a reject that did not remove the draw);
    'admitted, undrawn' -- a record the test admits in an eye none of whose
    claimed slots is drawn there; 'admitted, drawn'. A slot can be claimed by
    two records at one pivot (a shadow-only twin beside the eye record, same
    parts): judged per slot, a twin never counts against a test. The test
    that predicts the pool draws has zero in both violation columns."""
    parts_of = {}
    for k, rids in claims.items():
        for rid in rids:
            parts_of.setdefault(rid, []).append(k)
    out = {}
    for name in ('mask', 'gate', 'builder'):
        c = {'rejected, drawn': 0, 'admitted, undrawn': 0, 'admitted, drawn': 0}
        for e in 'AB':
            for k in drawn[e]:
                rids = claims.get(k)
                if rids and not any(verdict[r][name][e] for r in rids if r in verdict):
                    c['rejected, drawn'] += 1
            for rid, v in verdict.items():
                if v[name][e] and parts_of.get(rid):
                    c['admitted, drawn' if any(k in drawn[e] for k in parts_of[rid]) else 'admitted, undrawn'] += 1
        out[name] = c
    return out


def part_views(gate, frame):
    """Version 2 part rows of `frame`, per part (engine record, entry index,
    sub-item index): {view: (passed, LOD)}, a view passing if any of its rows
    passed (a record built twice in a frame tests its parts twice). Rows with
    no verified identity, no builder row, a foreign caller or a builder row
    whose pose differs are left out and counted."""
    builders = gate['builders']
    views, skipped = {}, 0
    for p in gate.get('parts', ()):
        if p['frame'] != frame:
            continue
        if p['builder'] == NO_ROW or p['flags'] & (PART_UNVERIFIED | PART_FOREIGN_CALLER | PART_OWNER_MISMATCH):
            skipped += 1
            continue
        key = (builders[p['builder']]['record'], p['entry'], p['sub'])
        v = views.setdefault(key, {})
        was = v.get(p['view'], (False, None))
        v[p['view']] = (True, p['lod']) if p['passed'] else was
    return views, skipped


def part_eye_verdicts(keys, views, eye_views, record_builder):
    """Per part and eye, the effective per-part admission: the part test's
    verdict in the eye's view; False when the record's own view loop rejected
    the eye (the builder never reaches the part test for it); None --
    untested -- when the record admitted the eye but no row tested the part
    there (a view the builder routes to its type-2 path, or a lost row)."""
    out = {}
    for key in keys:
        v = views.get(key, {})
        out[key] = {}
        for e in 'AB':
            if eye_views[e] in v:
                out[key][e] = v[eye_views[e]][0]
            elif not record_builder.get(key[0], {}).get(e, False):
                out[key][e] = False
            else:
                out[key][e] = None
    return out


def part_tally(pclaims, drawn, pverdict):
    """The per-part analogue of tally(): over (t33 slot, eye) and (part, eye)
    pairs, 'rejected, drawn' -- a slot drawn in an eye every claiming part
    rejects there; 'untested, drawn' -- no claimant admits it, one is
    untested; 'admitted, undrawn' -- a part admitted in an eye none of whose
    slots is drawn there; 'admitted, drawn'; 'rejected, undrawn' (agreement
    on a reject); 'untested' (part, eye) pairs. A slot claimed by two parts
    at one pivot (a shadow-only twin) never counts against the test. The
    per-part admission has zero in both violation columns."""
    slots_of = {}
    for k, keys in pclaims.items():
        for key in keys:
            slots_of.setdefault(key, []).append(k)
    c = dict.fromkeys(('rejected, drawn', 'untested, drawn', 'admitted, undrawn', 'admitted, drawn',
                       'rejected, undrawn', 'untested'), 0)
    for e in 'AB':
        for k in drawn[e]:
            keys = pclaims.get(k)
            if not keys:
                continue
            vs = [pverdict.get(key, {}).get(e) for key in keys]
            if any(v is True for v in vs):
                continue
            c['rejected, drawn' if all(v is False for v in vs) else 'untested, drawn'] += 1
        for key, slots in slots_of.items():
            v = pverdict.get(key, {}).get(e)
            hit = any(k in drawn[e] for k in slots)
            if v is True:
                c['admitted, drawn' if hit else 'admitted, undrawn'] += 1
            elif v is False:
                if not hit:
                    c['rejected, undrawn'] += 1
            else:
                c['untested'] += 1
    return c


# --------------------------------------------------------------------------- the run join

def eye_cameras(pool_dir, stamp, frame, depth, pool_pos):
    """(cols, origin) per eye 'A'/'B': version 2 depth files carry them;
    otherwise the eyemesh snapshot's VS b1 per eye target, paired by record
    origins landing exactly on their own depth."""
    import numpy as np
    cams = {}
    for e in 'AB':
        if depth[e].view_proj() is not None:
            cams[e] = (np.array(depth[e].view_proj(), float), np.array(depth[e].camera_origin(), float))
    if len(cams) == 2:
        return cams
    import eye_draw_snapshot as snap
    path = os.path.join(pool_dir, 'drawstate_%s.eyemesh.bin' % stamp)
    if not os.path.exists(path):
        return {}
    cap = snap.read(path)
    per_target = {}
    for d in cap['draws']:
        if d['frame'] != frame or d['target'] in per_target:
            continue
        f = np.frombuffer(d['buffers'][1]['data'], dtype='<f4')
        if len(f) >= 276 * 4:
            per_target[d['target']] = (f[1080:1096].reshape(4, 4).astype(float), f[1100:1103].astype(float))
    best, score = {}, {}
    for t, (cols, org) in per_target.items():
        for e in 'AB':
            score[(t, e)] = exact_hits(cols, org, pool_pos, depth[e])
    targets = list(per_target)
    pairs = [(a, b) for a in targets for b in targets if a != b]
    if not pairs:
        return {}
    a, b = max(pairs, key=lambda p: score[(p[0], 'A')] + score[(p[1], 'B')])
    best['A'], best['B'] = per_target[a], per_target[b]
    return best


def project(cols, org, pos):
    import numpy as np
    p = pos - org[None, :3]
    return p[:, 0:1] * cols[0][None, :] + p[:, 1:2] * cols[1][None, :] + p[:, 2:3] * cols[2][None, :] + cols[3][None, :]


def exact_hits(cols, org, pos, dd):
    import numpy as np
    z = np.array(dd.depth, dtype=np.float64).reshape(dd.header['height'], dd.header['width'])
    clip = project(cols, org, pos)
    w = clip[:, 3]
    ok = w > 0.05
    H, W = z.shape
    px = ((clip[:, 0] / np.where(ok, w, 1) + 1) * 0.5 * W).astype(np.int64)
    py = ((1 - clip[:, 1] / np.where(ok, w, 1)) * 0.5 * H).astype(np.int64)
    inb = ok & (px >= 0) & (px < W) & (py >= 0) & (py < H)
    zr = NEAR / w[inb]
    zs = z[py[inb], px[inb]]
    return int((np.abs(zs - zr) / zr < 1e-3).sum())


def footprint_visible(cols, org, pos, z, radius):
    """Per record: does any texel of its footprint sphere's disc hold a depth
    at or behind the sphere's front (reversed-Z: stored <= near/(w - r))?
    Records whose sphere reaches the eye count visible (conservative)."""
    import numpy as np
    H, W = z.shape
    cf = np.array([[cols[0][j], cols[1][j], cols[2][j]] for j in range(4)])
    f = cf[3] / np.linalg.norm(cf[3])
    sx = np.linalg.norm(cf[0] - (cf[0] @ f) * f) * W / 2.0
    sy = np.linalg.norm(cf[1] - (cf[1] @ f) * f) * H / 2.0
    clip = project(cols, org, pos)
    w = clip[:, 3]
    sw = np.where(np.abs(w) > 1e-6, w, 1e-6)
    cx = (clip[:, 0] / sw + 1) * 0.5 * W
    cy = (1 - clip[:, 1] / sw) * 0.5 * H
    vis = np.zeros(len(pos), bool)
    for i in range(len(pos)):
        wc = w[i]
        if wc <= radius + NEAR:
            vis[i] = wc > -radius
            continue
        rx = radius * sx / wc * 1.05 + 1.0
        ry = radius * sy / wc * 1.05 + 1.0
        x0, x1 = int(math.floor(cx[i] - rx)), int(math.ceil(cx[i] + rx))
        y0, y1 = int(math.floor(cy[i] - ry)), int(math.ceil(cy[i] + ry))
        if x1 < 0 or y1 < 0 or x0 >= W or y0 >= H:
            continue
        X0, X1, Y0, Y1 = max(x0, 0), min(x1, W - 1), max(y0, 0), min(y1, H - 1)
        step = 1
        while (X1 - X0 + 1) * (Y1 - Y0 + 1) // (step * step) > 250000:
            step *= 2
        sub = z[Y0:Y1 + 1:step, X0:X1 + 1:step]
        gx = (np.arange(X0, X1 + 1, step) + 0.5 - cx[i]) / (rx + step)
        gy = (np.arange(Y0, Y1 + 1, step) + 0.5 - cy[i]) / (ry + step)
        m = (gx[None, :] ** 2 + gy[:, None] ** 2) <= 1.0
        if not m.any():
            m[m.shape[0] // 2, m.shape[1] // 2] = True
        vis[i] = bool((sub[m] <= NEAR / (wc - radius)).any())
    return vis, w


def run(pool_dir, stamp, args):
    import numpy as np
    import eye_run_ledger as ledger
    import eye_depth_dump as dd
    print('== run %s' % stamp)
    ver, frames, frame0, inst_stride, _, _, _, draws = ledger.load_draws(os.path.join(pool_dir, 'draws_%s.bin' % stamp))
    gate_path = os.path.join(pool_dir, 'gate_%s.bin' % stamp)
    gate = read_gate(gate_path) if os.path.exists(gate_path) else None
    frame = args.frame or (gate['first'] if gate else frame0 + 1)
    rows = draws.get(frame)
    if rows is None or not len(rows):
        print('  no ledger rows for frame %d' % frame)
        return 1
    pool = ledger.load_pool(os.path.join(pool_dir, 'pool_%s_%d.bin' % (stamp, frame)))
    inst = np.frombuffer(open(os.path.join(pool_dir, 'inst_%s_%d.bin' % (stamp, frame)), 'rb').read(), dtype=np.uint32)
    stride = max(1, inst_stride // 4)
    depth = {}
    for e in 'AB':
        p = os.path.join(pool_dir, 'depth_%s_f%d_%s.bin' % (stamp, frame, e))
        if not os.path.exists(p):
            print('  no depth file %s: seen/unseen needs both eyes' % os.path.basename(p))
            return 1
        depth[e] = dd.read(p)
    # Eye RT tokens: the two busiest, the first-bound one is A.
    rts = [int(x) for x in rows['rt'] if int(x) >= 0]
    top = sorted(set(rts), key=lambda r: -rts.count(r))[:2]
    first_row = {rt: int(np.argmax(rows['rt'] == rt)) for rt in top}
    rt_a, rt_b = sorted(top, key=lambda rt: first_row[rt])
    eye_of_rt = {rt_a: 'A', rt_b: 'B'}
    nrec = len(pool['pos'])
    drawn = {'A': set(), 'B': set()}
    rows_of = {}
    for i, r in enumerate(rows):
        e = eye_of_rt.get(int(r['rt']))
        if not e or int(r['inst']) == 0 or (int(r['start']) == 0 and int(r['inst']) == 1):
            continue
        s, n = int(r['start']), int(r['inst'])
        for x in inst[s * stride:(s + n) * stride:stride]:
            x = int(x)
            if x < nrec:
                drawn[e].add(x)
                rows_of.setdefault(x, []).append(i)
    recs = np.array(sorted(drawn['A'] | drawn['B']), dtype=np.int64)
    pos = pool['pos'][recs].astype(np.float64)
    cams = eye_cameras(pool_dir, stamp, frame, depth, pos)
    if len(cams) != 2:
        print('  no eye camera: a version 2 depth file or the eyemesh snapshot is needed')
        return 1
    seen = np.zeros(len(recs), bool)
    for e in 'AB':
        z = np.array(depth[e].depth, dtype=np.float64).reshape(depth[e].header['height'], depth[e].header['width'])
        v, _ = footprint_visible(cams[e][0], cams[e][1], pos, z, args.radius)
        seen |= v
    print('  frame %d: %d t33 records drawn (A %d, B %d); unseen in both eyes at R = %.1f m: %d (%.1f%%)' % (
        frame, len(recs), len(drawn['A']), len(drawn['B']), args.radius, int((~seen).sum()),
        100.0 * (~seen).mean() if len(recs) else 0))
    if not gate:
        print('  no gate_%s.bin: seen/unseen only' % stamp)
        return 0
    c = gate['counts']
    print('  gate file: frames %d..%d, %d gate calls (%d kept), %d builder calls (%d kept), %d dumps, %d faults, %d '
          'mismatches, builder plane test %s' % (gate['first'], gate['last'], c['gate_calls'], c['gate_kept'],
                                                 c['builder_calls'], c['builder_kept'], c['dumps'], c['faults'],
                                                 c['record_mismatch'], 'ran' if gate['flags'] & 1 else 'ABSENT'))
    # Name the eye views: the view whose planes share each eye's orientation.
    eye_views, offsets, eye_bits = {}, {}, {}
    for d in gate['dumps']:
        scores = {}
        for e in 'AB':
            normals = side_planes(cams[e][0])
            ranked = sorted(((plane_match(normals, v['planes']), k) for k, v in enumerate(d['views'])), reverse=True)
            scores[e] = ranked[0] if ranked else (0.0, None)
        print('  dump ctx %#x frame %d: %d views; eye A = view %s (|cos| %.5f), eye B = view %s (|cos| %.5f)' % (
            d['ctx'], d['frame'], len(d['views']), scores['A'][1], scores['A'][0], scores['B'][1], scores['B'][0]))
        named = all(scores[e][1] is not None and scores[e][0] > 0.99 for e in 'AB') and scores['A'][1] != scores['B'][1]
        if named and (d['frame'] == frame or not eye_views):
            for e in 'AB':
                eye_views[e] = scores[e][1]
                eye_bits[e] = d['views'][scores[e][1]]['bits']
                offsets[e] = [a - b for a, b in zip(d['views'][scores[e][1]]['camera'][:3], cams[e][1][:3])]
    if len(eye_views) != 2:
        print('  the eyes are not among the engine views (no two distinct views match above 0.99): '
              'no per-eye verdicts')
        return 0
    off = [(offsets['A'][k] + offsets['B'][k]) / 2 for k in range(3)]
    spread = math.sqrt(sum((offsets['A'][k] - offsets['B'][k]) ** 2 for k in range(3)))
    print('  engine -> pool frame offset %s (the two eyes agree to %.3f m); eye view bits A %#x, B %#x' % (
        ['%.3f' % v for v in off], spread, eye_bits['A'], eye_bits['B']))
    # Join engine records to t33 records by their parts' world positions.
    # The jobs are stamped with the frame current when they ran: use this
    # frame's observations, else the window frame nearest it.
    stamped = sorted({b['frame'] for b in gate['builders']} | {x['frame'] for x in gate['gates']})
    obs_frame = min(stamped, key=lambda f: (abs(f - frame), f)) if stamped else frame
    if obs_frame != frame:
        print('  no observations stamped %d; using frame %d\'s' % (frame, obs_frame))
    builders = [b for b in gate['builders'] if b['frame'] == obs_frame]
    by_record = {}
    for b in builders:
        by_record.setdefault(b['record'], b)
    tree_pos = pool['pos'].astype(np.float64)
    best = None
    for conj in (False, True):
        claims, pclaims = {}, {}
        for rid, b in by_record.items():
            for ei, si, p in keyed_part_positions(b, gate['entries'], gate['subs'], conj):
                q = np.array([p[0] - off[0], p[1] - off[1], p[2] - off[2]])
                d2 = ((tree_pos - q[None, :]) ** 2).sum(axis=1)
                # Every pool record at the part's position (5 cm): parts that
                # share a pivot and a pose are one object to a cull. Every
                # claimant is kept: two engine records at one pivot (a
                # shadow-only twin) both claim the slot.
                for k in np.nonzero(d2 < 0.05 ** 2)[0]:
                    claims.setdefault(int(k), set()).add(rid)
                    pclaims.setdefault(int(k), set()).add((rid, ei, si))
        if best is None or len(claims) > len(best[1]):
            best = (conj, claims, pclaims)
    conj, claims, pclaims = best
    parts_of = {}
    for k, rids in claims.items():
        for rid in rids:
            parts_of.setdefault(rid, []).append(k)
    joined = sum(1 for x in recs if int(x) in claims)
    print('  t33 records joined to an engine record: %d of %d (parts rotated by %s; %d slots claimed by two or more '
          'engine records)' % (joined, len(recs), 'the conjugate' if conj else 'the quaternion',
                               sum(1 for v in claims.values() if len(v) > 1)))
    gate_verdict = {}
    for gc in gate['gates']:
        if gc['frame'] == obs_frame:
            gate_verdict.setdefault(gc['record'], {})[gc['view']] = gc['passed']
    # Per engine record, beside the ledger's per-eye draws of its parts.
    seen_of = {int(r): bool(s) for r, s in zip(recs, seen)}
    verdicts = {}
    table = []
    for rid, b in by_record.items():
        verdicts[rid] = {
            'mask': {e: mask_admits(b['rec_mask'], eye_bits[e]) for e in 'AB'},
            'gate': {e: bool(gate_verdict.get(rid, {}).get(eye_views[e], 0)) for e in 'AB'},
            'builder': {e: bool(b['bit_ok'] >> eye_views[e] & 1 and b['frustum_pass'] >> eye_views[e] & 1)
                        for e in 'AB'},
        }
        parts = [k for k in parts_of.get(rid, []) if k in seen_of]
        if parts:
            ledger_eyes = {e: any(k in drawn[e] for k in parts) for e in 'AB'}
            table.append((len(parts), rid, sum(seen_of[k] for k in parts), verdicts[rid], ledger_eyes))
    for name, c in tally(claims, drawn, verdicts).items():
        print('  per-eye admission vs the ledger\'s per-eye draws: %-7s %d (slot, eye) draws it rejects, %d admitted '
              '(record, eye) pairs with nothing drawn, %d admitted and drawn' % (
                  name, c['rejected, drawn'], c['admitted, undrawn'], c['admitted, drawn']))
    print('  top engine records by parts (parts seen/all; mask, gate, builder and ledger per eye):')
    for parts, rid, nseen, v, led in sorted(table, key=lambda x: -x[0])[:args.records]:
        fmt = lambda d: ''.join(e if d[e] else '.' for e in 'AB')
        print('    %#x  %3d/%-3d  mask %s  gate %s  builder %s  ledger %s' % (
            rid, nseen, parts, fmt(v['mask']), fmt(v['gate']), fmt(v['builder']), fmt(led)))
    report_parts(gate, obs_frame, eye_views, pclaims, drawn, seen_of, verdicts, off, pool, args)
    occluders(pool_dir, stamp, frame, recs, seen, cams, rows_of, args)
    return 0


def report_parts(gate, obs_frame, eye_views, pclaims, drawn, seen_of, verdicts, off, pool, args):
    """The per-part site (version 2): FUN_1442B3FC0's verdicts per part and
    eye against the ledger's per-eye draws, the one-eye slots, and a listing."""
    if gate.get('version', 1) < 2:
        print('  gate file version 1: no per-part rows (FUN_1442B3FC0 was not captured before version 2)')
        return
    c = gate['counts']
    print('  per-part test FUN_1442B3FC0: %s; %d calls in the window, %d rows kept, %d dropped over the cap; of '
          'the kept rows %d with the builder frame unverified, %d from a foreign caller, %d outside a kept builder '
          'row' % ('hooked' if gate['flags'] & FLAG_PART_HOOKED else 'STOOD DOWN (no rows by construction)',
                   c['part_calls'], c['part_kept'], c['part_dropped'], c['part_unverified'], c['part_foreign'],
                   c['part_unlinked']))
    views, skipped = part_views(gate, obs_frame)
    if not views:
        print('  no part rows with a verified identity in frame %d (%d left out): no per-part tally' % (
            obs_frame, skipped))
        return
    record_builder = {rid: v['builder'] for rid, v in verdicts.items()}
    keys = {key for ks in pclaims.values() for key in ks}
    pverdict = part_eye_verdicts(keys, views, eye_views, record_builder)
    tested = sum(1 for key in keys if key in views)
    print('  frame %d: %d parts carry part rows (%d rows left out), %d of the %d builder parts joined to a t33 slot '
          'were tested' % (obs_frame, len(views), skipped, tested, len(keys)))
    t = part_tally(pclaims, drawn, pverdict)
    print('  per-part admission vs the ledger\'s per-eye draws: part    %d (slot, eye) draws every claiming part '
          'rejects, %d admitted (part, eye) pairs with nothing drawn, %d admitted and drawn; %d rejected and '
          'undrawn; untested: %d drawn slots, %d (part, eye) pairs' % (
              t['rejected, drawn'], t['admitted, undrawn'], t['admitted, drawn'], t['rejected, undrawn'],
              t['untested, drawn'], t['untested']))
    # The slots drawn in one eye only (§10: 217, each outside the other eye).
    one = {'A': drawn['A'] - drawn['B'], 'B': drawn['B'] - drawn['A']}
    other = {'A': 'B', 'B': 'A'}
    verdict_count = {'rejected': 0, 'admitted': 0, 'untested': 0, 'unjoined': 0}
    for e in 'AB':
        for k in one[e]:
            ks = pclaims.get(k)
            if not ks:
                verdict_count['unjoined'] += 1
                continue
            vs = [pverdict[key][other[e]] for key in ks]
            verdict_count['admitted' if any(v is True for v in vs) else
                          'rejected' if all(v is False for v in vs) else 'untested'] += 1
    print('  slots drawn in one eye only: %d (A %d, B %d); in the eye that did not draw them the part test '
          'rejects %d, admits %d, never tested %d; %d have no joined builder part' % (
              len(one['A']) + len(one['B']), len(one['A']), len(one['B']), verdict_count['rejected'],
              verdict_count['admitted'], verdict_count['untested'], verdict_count['unjoined']))
    # The sphere each row carries against the slot its part claims, and
    # against the model's own radius (the builder copies model+0x10).
    import numpy as np
    by_part_row = {}
    for p in gate['parts']:
        if (p['frame'] == obs_frame and p['builder'] != NO_ROW and
                not p['flags'] & (PART_UNVERIFIED | PART_FOREIGN_CALLER | PART_OWNER_MISMATCH | PART_SPHERE_FAULT)):
            by_part_row.setdefault((gate['builders'][p['builder']]['record'], p['entry'], p['sub']), p)
    model_radius = {}
    for b in gate['builders']:
        if b['frame'] != obs_frame:
            continue
        for ei, e in enumerate(gate['entries'][b['entry_first']:b['entry_first'] + b['entries_copied']]):
            model_radius[(b['record'], ei)] = e['model_sphere'][0]
    dists, radius_off = [], 0
    for k, ks in pclaims.items():
        for key in ks:
            p = by_part_row.get(key)
            if p is None:
                continue
            c3 = [p['centre'][j] - off[j] for j in range(3)]
            dists.append(float(np.linalg.norm(np.array(c3) - pool['pos'][k][:3])))
            mr = model_radius.get((key[0], key[1]))
            if mr is not None and abs(mr - p['radius']) > 1e-6 * max(1.0, abs(mr)):
                radius_off += 1
    if dists:
        d = np.array(dists)
        print('  part spheres: centre to the claimed slot\'s origin median %.3f m, 90th %.3f m, max %.3f m over %d '
              '(part, slot) pairs; radius p50 %.2f m, p90 %.2f m; %d radii differ from the model\'s +0x10' % (
                  float(np.median(d)), float(np.percentile(d, 90)), float(d.max()), len(d),
                  float(np.median([p['radius'] for p in by_part_row.values()])),
                  float(np.percentile([p['radius'] for p in by_part_row.values()], 90)), radius_off))
    # The listing: one-eye parts first, then the rest by record and index.
    slots_of = {}
    for k, ks in pclaims.items():
        for key in ks:
            slots_of.setdefault(key, []).append(k)

    def eye_text(key, e):
        v = pverdict[key][e]
        pv = views.get(key, {}).get(eye_views[e])
        state = 'untested' if v is None else ('pass L%d' % pv[1] if v and pv else 'pass') if v else (
            'FAIL' if pv else 'record rejects')
        return '%s: view %d %s %s' % (e, eye_views[e], state,
                                      'drawn' if any(k in drawn[e] for k in slots_of.get(key, ())) else 'undrawn')

    def one_eye(key):
        return any((k in drawn['A']) != (k in drawn['B']) for k in slots_of.get(key, ()))

    order = sorted(slots_of, key=lambda key: (not one_eye(key), key))
    if args.parts:
        print('  parts (one-eye parts first; record, entry/sub-item, slots, sphere, per eye; every view P/F):')
    for key in order[:args.parts]:
        p = by_part_row.get(key)
        sphere = '(%.2f, %.2f, %.2f) r %.2f' % (p['centre'][0], p['centre'][1], p['centre'][2], p['radius']) \
            if p else '(no row)'
        allv = ' '.join('%d:%s' % (v, ('P%d' % lod) if ok else 'F') for v, (ok, lod) in
                        sorted(views.get(key, {}).items()))
        print('    %#x e%d/s%d slot %s  %s  %s  %s  views %s' % (
            key[0], key[1], key[2], ','.join(str(k) for k in sorted(slots_of[key])[:3]), sphere,
            eye_text(key, 'A'), eye_text(key, 'B'), allv or '-'))
    if args.parts_csv:
        with open(args.parts_csv, 'w', encoding='utf-8', newline='') as f:
            f.write('record,entry,sub,slots,cx,cy,cz,radius,A_view,A_verdict,A_lod,A_drawn,B_view,B_verdict,'
                    'B_lod,B_drawn,views\n')
            for key in order:
                p = by_part_row.get(key)
                row = ['%#x' % key[0], str(key[1]), str(key[2]), ' '.join(str(k) for k in sorted(slots_of[key]))]
                row += ['%.4f' % p['centre'][j] for j in range(3)] + ['%.4f' % p['radius']] if p else [''] * 4
                for e in 'AB':
                    v = pverdict[key][e]
                    pv = views.get(key, {}).get(eye_views[e])
                    row += [str(eye_views[e]), 'untested' if v is None else ('pass' if v else 'reject'),
                            str(pv[1]) if pv and pv[0] else '',
                            '1' if any(k in drawn[e] for k in slots_of[key]) else '0']
                row.append(' '.join('%d:%s' % (v, ('P%d' % lod) if ok else 'F')
                                    for v, (ok, lod) in sorted(views.get(key, {}).items())))
                f.write(','.join(row) + '\n')
        print('  %d parts written to %s' % (len(order), args.parts_csv))


def occluders(pool_dir, stamp, frame, recs, seen, cams, rows_of, args):
    """Seen t33 records within --occluder-range of eye A: their draws' meshes
    from the version 9 geometry, triangle counts, and whether the state is
    opaque and depth-writing."""
    import numpy as np
    import eye_draw_snapshot as snap
    path = os.path.join(pool_dir, 'drawstate_%s.eyemesh.bin' % stamp)
    if not os.path.exists(path):
        print('  no eyemesh snapshot: no occluder geometry')
        return
    geo = snap.read(path).get('geometry') or snap.empty_geometry()
    if not geo['draws'] and not geo['meshes']:
        print('  the eyemesh snapshot\'s version 9 geometry section is EMPTY (frame %d, 0 draws mapped, %d declined): '
              'no occluder geometry or state was captured' % (geo['frame'], geo['declined']))
        return
    if geo['frame'] != frame:
        print('  the eyemesh snapshot has no version 9 geometry for frame %d (it has frame %d)' % (frame, geo['frame']))
        return
    import eye_run_ledger as ledger
    pool = ledger.load_pool(os.path.join(pool_dir, 'pool_%s_%d.bin' % (stamp, frame)))
    eye = cams['A'][1][:3]
    by_ordinal = {g['ordinal']: g for g in geo['draws'] if g['frame'] == frame}
    occ, tris, missing, soft, meshes = 0, 0, 0, 0, set()
    for r, s in zip(recs, seen):
        if not s or np.linalg.norm(pool['pos'][int(r)] - eye) > args.occluder_range:
            continue
        occ += 1
        for row in rows_of.get(int(r), []):
            g = by_ordinal.get(row)
            if g is None or g['mesh'] == 0xffffffff:
                missing += 1
                continue
            if g['mesh'] not in meshes:
                meshes.add(g['mesh'])
                tris += snap.triangles(geo['meshes'][g['mesh']])
            st = geo['states'][g['state']] if g['state'] != 0xffffffff else None
            if st is not None and not (snap.blend_opaque(st) and snap.depth_writes(st)):
                soft += 1
    print('  occluder set (seen, within %.0f m): %d t33 records, %d distinct meshes, %d triangles; %d draws without '
          'geometry, %d draws blended or not depth-writing (not solid occluders)' % (
              args.occluder_range, occ, len(meshes), tris, missing, soft))


# --------------------------------------------------------------------------- tests

def fixture_expectations(g):
    """The rig's file (tools/cull_gate_probe_test): what the probe must have written."""
    c = g['counts']
    assert g['version'] == VERSION, 'the probe writes the current version'
    assert (g['first'], g['last'], g['flags'], g['vis_global']) == (100, 102, 1 | FLAG_PART_HOOKED, 1), 'header'
    assert (c['gate_calls'], c['gate_kept'], c['builder_calls'], c['builder_kept']) == (5, 4, 2, 2), 'counters'
    assert c['record_mismatch'] == 1 and c['faults'] >= 2 and c['dumps'] == 2, 'faults, mismatch, dumps'
    assert [c[k] for k in PART_COUNTERS] == [9, 8, 0, 3, 1, 1], 'part counters %s' % [c[k] for k in PART_COUNTERS]
    assert [d['frame'] for d in g['dumps']] == [100, 101], 'one dump per (context, frame)'
    d = g['dumps'][0]
    assert abs(d['lod_scale'] - 0.75) < 1e-6 and d['bit_table'][:3] == [0, 1, 2] and len(d['views']) == 3
    for k, v in enumerate(d['views']):
        assert v['planes'] == [[1.0, 0.0, 0.0, float(k)], [0.0, 1.0, 0.0, 2.0]], 'plane array'
        assert v['camera'] == [10.0 * k, 20.0, 30.0, 1.0] and v['bits'] == 1 << k and v['bit_index'] == k
        assert abs(v['lod_scale'] - (0.5 + k)) < 1e-6 and v['flags'] == 0x10 * k and v['flag68d'] == (1 if k == 2 else 0)
    gates = g['gates']
    assert [(x['frame'], x['view'], x['passed'], x['lod'], x['flags']) for x in gates] == [
        (100, 0, 1, 2, 0), (100, 1, 0, 0, 0), (100, 0xFFFF, 1, 2, 1), (101, 0, 1, 2, 0)], 'gate rows'
    assert len({x['record'] for x in gates}) == 1 and len({x['ctx'] for x in gates}) == 1
    b = g['builders'][0]
    assert b['record'] == gates[0]['record'] and b['ctx'] == gates[0]['ctx'] and b['node'] == 0x5150
    assert b['rec_mask'] == 0b101 and b['nibbles'][0] == 0x21 and b['active_mask'] == 0b111
    assert (b['bit_ok'], b['frustum_pass'], b['frustum_inside'], b['vis_applies']) == (0b111, 0b101, 0b001, 0b100)
    assert b['position'] == [100.0, 200.0, 300.0] and b['quat'] == bytes(range(1, 9))
    assert b['world_centre'] == [101.0, 202.0, 303.0, 1.0] and b['radius'][0] == 7.5
    assert all(abs(a - x) < 1e-5 for a, x in zip(b['centre'], [10.0, 21.0, 30.0, 1.0])), 'the builder centre formula'
    assert (b['frame'], b['view_count'], b['flags'], b['entry_count'], b['entries_copied']) == (100, 3, 0, 2, 2)
    e0, e1 = g['entries'][b['entry_first']:b['entry_first'] + 2]
    assert e0['model'] and e1['model'] and e0['model'] != e1['model'], 'two models'
    assert (e0['sub_count'], e0['sub_copied'], e1['sub_copied']) == (3, 3, 0), 'sub-item counts'
    assert [g['subs'][e0['sub_first'] + k][4:7] for k in range(3)] == [(float(k), 2.0 * k, 3.0 * k) for k in range(3)]
    assert e0['model_centre'] == [0.5, 0.25, 0.125, 1.0] and e0['model_sphere'] == [4.5, 0.5, 0.0, 0.0], \
        'the model\'s +0x00 centre and +0x10 sphere'
    assert e1['model_sphere'][0] == 9.0, 'the second model\'s radius'
    m = g['builders'][1]
    assert m['flags'] & 4 and m['active_mask'] == 0b001, 'the pose mismatch is flagged'
    # The part tests, in call order (the rig's P1..P6, P8, P9; P7 faulted, P10 fell outside).
    parts = g['parts']
    got = [(p['frame'], p['builder'], p['entry'], p['sub'], p['lod'], p['view'], p['passed'], p['flags'])
           for p in parts]
    un = PART_UNVERIFIED
    assert got == [(100, 0, 0, 1, 2, 0, 1, 0), (100, 0, 0, 2, 0, 1, 0, 0), (100, 0, NO_ROW, NO_ROW, 2, 0, 1, un),
                   (100, 0, NO_ROW, NO_ROW, 2, 0, 1, un | PART_FOREIGN_CALLER), (100, 1, 0, 0, 2, 0, 1, PART_OWNER_MISMATCH),
                   (100, NO_ROW, 0, 0, 2, 0, 1, 0), (100, 0, NO_ROW, NO_ROW, 2, 0, 1, un),
                   (100, 0, 0, 0, 2, 0, 1, 0)], 'part rows %s' % got
    assert all(p['pose'] == b['pose'] for p in parts), 'every part names param_1[2], the pose context'
    assert parts[0]['centre'] == [11.0, 22.0, 33.0] and parts[0]['radius'] == 4.5, 'the sphere as tested'
    assert parts[0]['view_bits'] == 1 and parts[1]['view_bits'] == 2, 'the view bits a pass sets'
    assert parts[0]['sub_item'] and parts[1]['sub_item'] - parts[0]['sub_item'] == 32, 'the sub-item addresses'
    assert all(p['sub_item'] == 0 for p in parts if p['flags'] & PART_UNVERIFIED), 'no address without identity'


def self_test():
    import numpy as np
    raw = bytearray(VIEW_BYTES)
    struct.pack_into('<4f', raw, 0x540, 1, 2, 3, 1)
    struct.pack_into('<ffQI', raw, 0x550, 0.5, 0, 0, 0)
    struct.pack_into('<f', raw, 0x560, 0.25)
    struct.pack_into('<QI', raw, 0x570, 4, 2)
    raw[0x68D] = 1
    view = dict(planes=[[1.0, 0.0, 0.0, 0.0]], raw=bytes(raw))
    g = dict(first=5, last=7, flags=1 | FLAG_PART_HOOKED, vis_global=0,
             counts={k: i for i, k in enumerate(COUNTERS + PART_COUNTERS)},
             dumps=[dict(ctx=0x1000, frame=5, lod_scale=1.0, faults=0, bit_table=list(range(64)), views=[view])],
             gates=[dict(record=0x2000, ctx=0x1000, frame=5, lod=3, view=0, passed=1, flags=0)],
             builders=[dict(record=0x2000, pose=0x3000, ctx=0x1000, node=0x4000, active_mask=1, rec_mask=1,
                            nibbles=[0, 0, 0, 0], bit_ok=1, frustum_pass=1, frustum_inside=0, vis_applies=0,
                            position=[10.0, 0.0, 0.0], quat=struct.pack('<4H', 32767, 32767, 32767, 65535),
                            world_centre=[0.0] * 4, local_centre=[0.0] * 4, radius=[1.0, 0, 0, 0],
                            pose_quat=[0, 0, 0, 1.0], pose_t=[0.0] * 4, pose_centre=[0.0] * 4, pose_extents=[1.0, 0, 0, 0],
                            centre=[0.0] * 4, frame=5, view_count=1, flags=0, entry_count=1, entry_first=0,
                            entries_copied=1)],
             entries=[dict(model=0x5000, sub_count=1, sub_first=0, sub_copied=1,
                           model_centre=[0.5, 0.0, 0.0, 1.0], model_sphere=[2.5, 0.0, 0.0, 0.0])],
             subs=[(0.0, 0.0, 0.0, 1.0, 1.0, 2.0, 3.0, 1.0)],
             parts=[dict(pose=0x3000, sub_item=0x6000, view_bits=4, centre=[11.5, 2.0, 3.0], radius=2.5, frame=5,
                         builder=0, entry=0, sub=0, lod=1, view=0, passed=1, flags=0),
                    dict(pose=0x3000, sub_item=0, view_bits=4, centre=[0.0, 0.0, 0.0], radius=0.0, frame=5,
                         builder=NO_ROW, entry=NO_ROW, sub=NO_ROW, lod=0, view=0xFFFF, passed=0,
                         flags=PART_UNVERIFIED | PART_VIEW_FOREIGN)])
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, 'gate_TEST.bin')
        blob = write_gate(g)
        open(p, 'wb').write(blob)
        r = read_gate(p)
        assert r['version'] == 2 and r['flags'] == 1 | FLAG_PART_HOOKED
        assert r['counts']['faults'] == COUNTERS.index('faults') and r['gates'][0]['lod'] == 3
        assert r['counts']['part_unlinked'] == len(COUNTERS) + PART_COUNTERS.index('part_unlinked'), 'part counters'
        v = r['dumps'][0]['views'][0]
        assert v['camera'] == [1.0, 2.0, 3.0, 1.0] and v['bits'] == 4 and v['bit_index'] == 2 and v['flag68d'] == 1
        b = r['builders'][0]
        assert b['quat'] == g['builders'][0]['quat'] and r['subs'][0][4:7] == (1.0, 2.0, 3.0) and b['index'] == 0
        assert r['entries'][0]['model_sphere'][0] == 2.5 and r['entries'][0]['model_centre'][0] == 0.5
        assert r['parts'] == g['parts'], 'part rows round-trip'
        # The record's quaternion decodes to ~identity; its part sits at +(1,2,3).
        pts = part_positions(b, r['entries'], r['subs'], False)
        assert all(abs(a - x) < 1e-3 for a, x in zip(pts[0], [11.0, 2.0, 3.0])), 'part world position'
        assert [k[:2] for k in keyed_part_positions(b, r['entries'], r['subs'], False)] == [(0, 0)], 'part keys'
        # Version 1 (no part counters, model spheres or part rows) still reads.
        g1 = dict(g, version=1, flags=1)
        open(p, 'wb').write(write_gate(g1))
        r1 = read_gate(p)
        assert r1['version'] == 1 and r1['parts'] == [] and 'part_calls' not in r1['counts'], 'version 1'
        assert 'model_sphere' not in r1['entries'][0] and r1['builders'][0]['record'] == 0x2000
        for bad, why in ((b'EDVRDRW1' + blob[8:], 'magic'), (blob[:-1], 'truncated'), (blob + b'\0', 'trailing'),
                         (blob[:8] + struct.pack('<I', 3) + blob[12:], 'version'),
                         (blob[:8] + struct.pack('<I', 1) + blob[12:], 'a version 2 body read as version 1')):
            open(p, 'wb').write(bad)
            try:
                read_gate(p)
            except ValueError:
                continue
            raise AssertionError('accepted a bad file: %s' % why)
        # A part row naming a builder row the file does not have.
        open(p, 'wb').write(write_gate(dict(g, parts=[dict(g['parts'][0], builder=5)])))
        try:
            read_gate(p)
            raise AssertionError('accepted a part row with no such builder row')
        except ValueError:
            pass
    # Eye naming: a clip matrix's side planes match themselves, not a rotated frustum.
    cols = np.array([[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0.3, 0.1, 0, 1.0], [0, 0, 0.025, 0]])
    normals = side_planes(cols)
    planes = [n + [0.0] for n in normals]
    assert plane_match(normals, planes) > 0.9999, 'a frustum matches its own planes'
    turned = [[n[1], -n[0], n[2], 0.0] for n in normals]
    assert plane_match(normals, turned) < 0.99, 'a turned frustum does not match'
    # A mask admits a view by the view's own bits, not its array index (152632: eye B is view 5, bit 22).
    assert mask_admits(0x8006400082, 1 << 22) and not mask_admits(0x8006400082, 1 << 5), 'mask by view bits'
    # The tally: slot 1 is claimed by an admitted record and its rejected shadow-only twin, slot 2 by a
    # record every test rejects and that is not drawn, slot 3 by a record admitted but not drawn in B.
    both = {'A': True, 'B': True}
    none = {'A': False, 'B': False}
    v = {10: {'mask': both, 'gate': none, 'builder': both}, 11: {'mask': none, 'gate': none, 'builder': none},
         12: {'mask': none, 'gate': none, 'builder': none}, 13: {'mask': both, 'gate': both, 'builder': both}}
    t = tally({1: {10, 11}, 2: {12}, 3: {13}}, {'A': {1, 3}, 'B': {1}}, v)
    assert t['builder'] == {'rejected, drawn': 0, 'admitted, undrawn': 1, 'admitted, drawn': 3}, t['builder']
    assert t['gate'] == {'rejected, drawn': 2, 'admitted, undrawn': 1, 'admitted, drawn': 1}, t['gate']
    # Quaternion rotation: +90 degrees about z turns x into y.
    s = math.sqrt(0.5)
    assert all(abs(a - x) < 1e-9 for a, x in zip(rotate([0, 0, s, s], [1, 0, 0]), [0, 1, 0]))
    assert all(abs(a - x) < 1e-9 for a, x in zip(rotate([0, 0, s, s], [1, 0, 0], True), [0, -1, 0]))
    # The per-part site. Record 10 (builder row 0) has parts (0,0), (0,1), (1,0); record 11 (row 1) is a
    # shadow-only twin at (0,0)'s pivot whose view loop rejects both eyes. Eye A is view 0, eye B view 5.
    # Part (0,0) passes both eyes; (0,1) passes A, fails B (a one-eye part); (1,0) has no rows: untested.
    gp = dict(builders=[dict(record=10), dict(record=11)], parts=[
        dict(frame=5, builder=0, entry=0, sub=0, view=0, passed=1, lod=2, flags=0),
        dict(frame=5, builder=0, entry=0, sub=0, view=5, passed=0, lod=0, flags=0),
        dict(frame=5, builder=0, entry=0, sub=0, view=5, passed=1, lod=1, flags=0),   # twice built: any pass
        dict(frame=5, builder=0, entry=0, sub=1, view=0, passed=1, lod=0, flags=0),
        dict(frame=5, builder=0, entry=0, sub=1, view=5, passed=0, lod=0, flags=0),
        dict(frame=5, builder=0, entry=0, sub=1, view=7, passed=1, lod=3, flags=0),   # a shadow view
        dict(frame=5, builder=0, entry=1, sub=0, view=0, passed=0, lod=0, flags=PART_UNVERIFIED),   # left out
        dict(frame=5, builder=NO_ROW, entry=1, sub=0, view=0, passed=1, lod=0, flags=0),           # left out
        dict(frame=6, builder=0, entry=1, sub=0, view=0, passed=1, lod=0, flags=0)])               # other frame
    pv, skipped = part_views(gp, 5)
    assert skipped == 2 and set(pv) == {(10, 0, 0), (10, 0, 1)}, (skipped, sorted(pv))
    assert pv[(10, 0, 0)] == {0: (True, 2), 5: (True, 1)} and pv[(10, 0, 1)][7] == (True, 3), pv
    eyes = {'A': 0, 'B': 5}
    rb = {10: {'A': True, 'B': True}, 11: {'A': False, 'B': False}}
    keys = [(10, 0, 0), (10, 0, 1), (10, 1, 0), (11, 0, 0)]
    ev = part_eye_verdicts(keys, pv, eyes, rb)
    assert ev == {(10, 0, 0): {'A': True, 'B': True}, (10, 0, 1): {'A': True, 'B': False},
                  (10, 1, 0): {'A': None, 'B': None}, (11, 0, 0): {'A': False, 'B': False}}, ev
    # Slots: 1 = (10,0,0) + twin (11,0,0), drawn in both; 2 = (10,0,1), drawn in A only; 3 = (10,1,0), drawn
    # in A (untested); 4 = (11,0,0) alone, undrawn.
    pc = {1: {(10, 0, 0), (11, 0, 0)}, 2: {(10, 0, 1)}, 3: {(10, 1, 0)}, 4: {(11, 0, 0)}}
    t = part_tally(pc, {'A': {1, 2, 3}, 'B': {1}}, ev)
    assert t == {'rejected, drawn': 0, 'untested, drawn': 1, 'admitted, undrawn': 0, 'admitted, drawn': 3,
                 'rejected, undrawn': 1, 'untested': 2}, t
    # A part the test rejects in the eye that drew it is a violation; one it admits undrawn is the other.
    t = part_tally(pc, {'A': {1, 2, 3}, 'B': {1, 2}}, ev)
    assert t['rejected, drawn'] == 1 and t['admitted, undrawn'] == 0, t
    t = part_tally(pc, {'A': {1, 3}, 'B': {1}}, ev)
    assert t['rejected, drawn'] == 0 and t['admitted, undrawn'] == 1, t
    print('cull_gate_probe self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('pool_dir', nargs='?')
    ap.add_argument('stamps', nargs='*')
    ap.add_argument('--frame', type=int, default=0, help='the ledger frame (default: the gate window\'s first)')
    ap.add_argument('--radius', type=float, default=1.0, help='footprint sphere radius, metres')
    ap.add_argument('--occluder-range', type=float, default=120.0, help='occluder set: seen records within this many metres')
    ap.add_argument('--records', type=int, default=40, help='engine records to list')
    ap.add_argument('--parts', type=int, default=40, help='parts to list (version 2; one-eye parts first)')
    ap.add_argument('--parts-csv', metavar='FILE', help='write every joined part (version 2) to this CSV')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--verify-fixture', metavar='GATE_FILE', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.self_test:
        try:
            self_test()
        except (AssertionError, ValueError) as e:
            print('cull_gate_probe self-test FAILED: %s' % e, file=sys.stderr)
            return 2
        return 0
    if a.verify_fixture:
        try:
            fixture_expectations(read_gate(a.verify_fixture))
        except (AssertionError, ValueError, OSError) as e:
            print('cull_gate_probe fixture FAILED: %s' % e, file=sys.stderr)
            return 2
        print('cull_gate_probe fixture passed: %s' % a.verify_fixture)
        return 0
    if not a.pool_dir or not a.stamps:
        ap.error('a pool directory and at least one run stamp are required (or --self-test)')
    status = 0
    for stamp in a.stamps:
        try:
            status = max(status, run(a.pool_dir, stamp, a))
        except (OSError, ValueError, KeyError) as e:
            print('  run %s: %s' % (stamp, e), file=sys.stderr)
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
