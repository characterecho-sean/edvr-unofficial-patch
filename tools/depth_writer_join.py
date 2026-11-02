#!/usr/bin/env python3
"""Name the draw that disowns a pool record's slot: join an eye run's depth writer map to its stale-slot pixels.

    python tools\\depth_writer_join.py --stamp 180433299003 [--logs <edvr_logs>] [--eye 0] [--top 12]
    python tools\\depth_writer_join.py --self-test

A run armed with advanced.pixel_probe set (src/d3d11/depth_writer_probe.h) writes, beside the object ledger,
edvr_logs\\pool\\depthwriter_<stamp>_e<eye>.bin (EDVRTEX1, DXGI 42 R32_UINT: for every pixel the id of the LAST draw that changed
the eye's scene depth, 0 = none did) and depthwriter_<stamp>.json (one row per id: vs, ps, counts, the depth, stencil, blend and
rasteriser state the draw ran under, and the pixels it owns at the end).

The engine motion compose calls a pixel STALE (kind 4) when a pool record's slot is there but the slot's recorded depth is not the
scene's bit for bit; such a pixel keeps no history. This tool takes the same run's EngineSlots (slot code, slot depth) and Z (the
scene depth) from edvr_logs\\eyes, finds those pixels (marked, and the two depths differ in a bit), and counts the writer ids
over them, grouped by the writer's shader pair and state. The pairs at the top of that table are the draws to look at.

The writer map is of the run's probed frame and EngineSlots/Z are the run's first captured frame; they are the same frame when the
run was armed once and the scene is steady, which the join cannot check (the three files carry different frame counters).
"""
import argparse
import json
import os
import struct
import sys
import tempfile

NO_SLOT = 0xBF800000  # -1.0f: no keyed draw wrote this pixel's slot


def read_texture(path):
    """(meta, ndarray) of one EDVRTEX1 file, through tools/eye_inputs.py (a sibling module)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import eye_inputs
    return eye_inputs.read(path)


def join(slot_code, slot_z, scene_z, writer, draws, near_mask=None):
    """Counts over the stale pixels. Arrays are (h, w) and equal in shape; draws maps id -> row dict.

    Returns {'marked': n, 'stale': n, 'by_writer': [(id, pixels)], 'unwritten': n}, most pixels first.
    """
    import numpy as np
    marked = slot_code.view(np.uint32) != NO_SLOT if slot_code.dtype != np.uint32 else slot_code != NO_SLOT
    if near_mask is not None:
        marked = marked & near_mask
    stale = marked & (slot_z.view(np.uint32) != scene_z.view(np.uint32))
    ids, counts = np.unique(writer[stale], return_counts=True)
    by_writer = sorted(((int(i), int(c)) for i, c in zip(ids, counts)), key=lambda t: -t[1])
    return {'marked': int(marked.sum()), 'stale': int(stale.sum()), 'by_writer': by_writer,
            'unwritten': int(counts[ids == 0].sum()) if (ids == 0).any() else 0}


def describe(row):
    if not row:
        return 'no row'
    d, s, b, r = row['depth'], row['stencil'], row['blend'], row['raster']
    return ('vs %s ps %s count %d inst %d | depth %s func %d write %d | stencil %s ref %d | blend %s mask %d | '
            'bias %d slope %.3g clamp %.3g clip %d cull %d | rtv0 %s' %
            (row['vs'], row['ps'], row['count'], row['instances'], 'on' if d['enable'] else 'off', d['func'], d['write'],
             'on' if s['enable'] else 'off', s['ref'], 'on' if b['enable'] else 'off', b['rt0_mask'],
             r['depth_bias'], r['slope_bias'], r['bias_clamp'], r['depth_clip'], r['cull'],
             '%dx%d fmt %d' % (row['rtv0']['width'], row['rtv0']['height'], row['rtv0']['format']) if row['rtv0']['bound'] else 'none'))


def report(logs, stamp, eye, top):
    import numpy as np
    pool = os.path.join(logs, 'pool')
    eyes = os.path.join(logs, 'eyes')
    _, writer = read_texture(os.path.join(pool, 'depthwriter_%s_e%d.bin' % (stamp, eye)))
    with open(os.path.join(pool, 'depthwriter_%s.json' % stamp), 'r', encoding='utf-8') as f:
        table = {row['id']: row for row in json.load(f)['draws']}
    _, es = read_texture(os.path.join(eyes, 'eye_%s_EngineSlots.bin' % stamp))
    _, z = read_texture(os.path.join(eyes, 'eye_%s_Z.bin' % stamp))
    code = np.ascontiguousarray(es[..., 0])
    zslot = np.ascontiguousarray(es[..., 1])
    scene = np.ascontiguousarray(z, dtype=np.float32)
    if writer.shape != scene.shape:
        raise SystemExit('writer map %s and Z %s differ in size' % (writer.shape, scene.shape))
    near = (0.025 / np.maximum(scene, 1e-9)) < 2000.0   # not sky
    out = join(code, zslot, scene, writer, table, near)
    print('eye %d: %d marked pixels, %d stale (slot depth is not the scene depth bit for bit), %d of those never written after the clear'
          % (eye, out['marked'], out['stale'], out['unwritten']))
    groups = {}
    for i, c in out['by_writer']:
        row = table.get(i)
        key = describe(row) if row else 'id %d (no row)' % i
        groups[key] = groups.get(key, 0) + c
    for key, c in sorted(groups.items(), key=lambda t: -t[1])[:top]:
        print('%8d  %s' % (c, key))
    return out


def write_texture(path, arr, fmt, eye=0):
    h, w = arr.shape[:2]
    with open(path, 'wb') as f:
        f.write(b'EDVRTEX1' + struct.pack('<9I', 1, w, h, fmt, arr.dtype.itemsize * (arr.size // (h * w)) * w, 0, eye, 0, 0))
        f.write(arr.tobytes())


def self_test():
    try:
        import numpy as np
    except ImportError:
        print('depth_writer_join self-test: numpy absent, skipped')
        return
    # four pixels: unmarked, marked and equal, marked and stale written by draw 2, marked and stale never written
    code = np.array([[0xBF800000, 1567, 1567, 1567]], dtype=np.uint32)
    zslot = np.array([[0.5, 0.25, 0.30, 0.31]], dtype=np.float32)
    scene = np.array([[0.5, 0.25, 0.29, 0.31]], dtype=np.float32)
    scene[0, 3] = np.float32(0.3100001)   # one ulp-ish apart: still stale, a bit for bit test
    writer = np.array([[0, 1, 2, 0]], dtype=np.uint32)
    out = join(code, zslot, scene, writer, {})
    assert out['marked'] == 3 and out['stale'] == 2, out
    assert out['by_writer'] == [(0, 1), (2, 1)] or out['by_writer'] == [(2, 1), (0, 1)], out
    assert out['unwritten'] == 1, out
    # the file path: write the five inputs and read the report back through report()
    row = {'id': 2, 'eye': 0, 'vs': 'AAAA000000000002', 'ps': '0000000000000000', 'topology': 4, 'count': 3, 'instances': 1, 'indirect': 0,
           'depth': {'enable': 1, 'func': 8, 'write': 1}, 'stencil': {'enable': 0, 'ref': 0, 'func': 8},
           'blend': {'enable': 0, 'src': 2, 'dest': 1, 'rt0_mask': 15}, 'rtv0': {'bound': 0, 'width': 0, 'height': 0, 'format': 0},
           'raster': {'known': 1, 'depth_bias': -100, 'bias_clamp': 0.0, 'slope_bias': 1.5, 'depth_clip': 1, 'cull': 3, 'fill': 3},
           'final_pixels': 1}
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, 'pool'))
        os.makedirs(os.path.join(tmp, 'eyes'))
        write_texture(os.path.join(tmp, 'pool', 'depthwriter_T9_e0.bin'), writer, 42)
        with open(os.path.join(tmp, 'pool', 'depthwriter_T9.json'), 'w', encoding='utf-8') as f:
            json.dump({'version': 1, 'draws': [row]}, f)
        slots = np.stack([code.view(np.float32), zslot], axis=-1)
        write_texture(os.path.join(tmp, 'eyes', 'eye_T9_EngineSlots.bin'), slots, 16)
        write_texture(os.path.join(tmp, 'eyes', 'eye_T9_Z.bin'), scene, 41)
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            result = report(tmp, 'T9', 0, 5)
        text = buf.getvalue()
        assert result['stale'] == 2 and 'AAAA000000000002' in text and 'bias -100' in text, text
    print('depth_writer_join self-test passed')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--stamp')
    ap.add_argument('--logs', default=None)
    ap.add_argument('--eye', type=int, default=0)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        self_test()
        return 0
    if not a.stamp:
        ap.error('--stamp is required')
    logs = a.logs or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Frontier_Developments', 'Products', 'elite-dangerous-odyssey-64', 'edvr_logs')
    report(logs, a.stamp, a.eye, a.top)
    return 0


if __name__ == '__main__':
    sys.exit(main())
