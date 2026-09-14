#!/usr/bin/env python3
"""Read draw-local tone-map evidence without treating missing data as black."""
from __future__ import annotations
import argparse
import io
import json
from pathlib import Path
import struct
import tempfile

VS, PS = 0x2D78DC3FD2C0C543, 0x99C21CEB7A699821
IMAGE_CAP, SMALL_CAP = 48 * 1024**2, 8 * 1024**2
BPP = {2: 16, 10: 8, 16: 8, 26: 4, 27: 4, 28: 4, 29: 4,
       41: 4, 34: 4, 35: 4, 54: 2, 56: 2, 49: 2, 61: 1}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def uint(value, cap=0xffffffff):
    return type(value) is int and 0 <= value <= cap


def words(value, count, nullable=False):
    return (nullable and value is None) or (isinstance(value, list) and len(value) == count and all(uint(x) for x in value))


def read(path):
    path = Path(path)
    require(path.stat().st_size <= IMAGE_CAP + SMALL_CAP + 1024**2, 'Snapshot exceeds file cap')
    f = io.BytesIO(path.read_bytes())

    def take(n):
        b = f.read(n)
        require(len(b) == n, 'Truncated tone-map snapshot')
        return b

    def u32():
        return struct.unpack('<I', take(4))[0]

    def metadata():
        n = u32()
        require(n <= 65536, 'Metadata exceeds cap')
        try:
            m = json.loads(take(n))
        except (UnicodeError, json.JSONDecodeError) as e:
            raise ValueError('Invalid tone-map JSON') from e
        require(isinstance(m, dict), 'Metadata is not an object')
        return m

    require(take(8) == b'EDVRTON1', 'Not an EDVRTON1 snapshot')
    version, count, declined, reserved = (u32() for _ in range(4))
    require(version == 1 and count <= 2, 'Unsupported version or draw count')
    require(reserved <= IMAGE_CAP + SMALL_CAP + 4 * 8192, 'Reserved byte cap exceeded')
    draws, totals = [], [0, 0, 0]
    for _ in range(count):
        d = metadata()
        require(d.get('vs') == VS and d.get('ps') == PS, 'Unexpected tone-map shader')
        require(all(uint(d.get(k)) for k in ('frame', 'ordinal', 'start', 'topology', 'vb_stride', 'vb_offset', 'sample_mask', 'stencil_ref')), 'Invalid draw metadata')
        require(d.get('kind') in (ord('D'), ord('N')) and d.get('count') == 3 and d.get('instances') == 1 and d.get('start_instance') == 0, 'Unsupported draw arguments')
        require(0 < d['vb_stride'] <= 256 and type(d.get('complete_inputs')) is bool, 'Invalid vertex stride or completeness')
        layout = d.get('layout')
        require(isinstance(layout, list) and 1 <= len(layout) <= 32, 'Invalid input layout count')
        for e in layout:
            require(isinstance(e, list) and len(e) == 7 and isinstance(e[0], str) and 0 < len(e[0]) < 64 and e[0].isascii() and all(c.isalnum() or c == '_' for c in e[0]), 'Invalid input semantic')
            require(all(uint(x) for x in e[1:]) and e[3] == 0 and e[5] == 0, 'Unsupported input layout stream')
        require(words(d.get('blend'), 66, True) and words(d.get('blend_factor'), 4), 'Invalid blend descriptor')
        require(words(d.get('depth'), 13, True) and words(d.get('raster'), 10, True), 'Invalid depth/raster descriptor')
        require(isinstance(d.get('samplers'), list) and len(d['samplers']) == 3 and all(words(x, 13, True) for x in d['samplers']), 'Invalid sampler descriptors')
        for key, n in (('viewports', 6), ('scissors', 4)):
            require(isinstance(d.get(key), list) and len(d[key]) <= 16 and all(words(x, n) for x in d[key]), 'Invalid viewport/scissor descriptors')
        if draws:
            require(d['frame'] == draws[0]['frame'] and d['ordinal'] > draws[-1]['ordinal'], 'Cross-frame or unordered tone-map draws')
        blobs = []
        for role in range(6):
            m = metadata()
            if m:
                require(uint(m.get('resource'), 0xffffffffffffffff) and m['resource'] != 0, 'Missing resource identity')
                n = m.get('bytes')
                require(uint(n) and n > 0, 'Invalid declared payload size')
                if role < 4:
                    require(m.get('type') == (3 if role == 1 else 2), 'Texture dimension does not match role')
                    require(m.get('format') in BPP and BPP.get(m.get('view_format')) == BPP[m['format']], 'Unsupported format')
                    require(words(m.get('view'), 5 if role == 3 else 6), 'Invalid resource view descriptor')
                    src, origin, size = m.get('source'), m.get('origin'), m.get('size')
                    require(words(src, 3) and words(origin, 3) and words(size, 3) and all(0 < x <= 16384 for x in src + size), 'Invalid texture dimensions')
                    require(all(o + s <= w for o, s, w in zip(origin, size, src)), 'Crop outside source')
                    require(m['view'][0] == m['view_format'], 'Inconsistent view format')
                    if role != 1:
                        require(src[2] == size[2] == 1 and origin[2] == 0, 'Invalid 2D texture depth')
                    if role < 2:
                        require(origin == [0, 0, 0] and size == src, 'Cropped exposure or LUT')
                    else:
                        require(size[:2] == [min(1400, s) for s in src[:2]] and origin[:2] == [(s-c)//2 for s, c in zip(src[:2], size[:2])], 'Invalid central crop')
                    row = size[0] * BPP[m['format']]
                    require(m.get('row') == row and n == row * size[1] * size[2], 'Invalid tight texture size')
                    group = 1 if role < 2 else 0
                else:
                    require(m.get('type') == 1 and uint(m.get('whole')) and uint(m.get('offset')) and n <= 8192 and m['offset'] + n <= m['whole'], 'Invalid buffer range')
                    if role == 4:
                        require(m['offset'] == 0 and 256 <= n <= 8192 and n % 16 == 0, 'Invalid PS b2 range')
                    else:
                        require(n == d['count'] * d['vb_stride'] and m['offset'] == d['vb_offset'] + d['start'] * d['vb_stride'], 'Invalid vertex window')
                    group = 2
                totals[group] += n
                require(totals[group] <= (IMAGE_CAP, SMALL_CAP, 4*8192)[group], 'Payload budget exceeded')
            else:
                n = 0
            size = u32()
            require(size == 0 or size == n, 'Readback payload size mismatch')
            blobs.append(dict(meta=m, data=take(size)))
        d['blobs'] = blobs
        draws.append(d)
    shader_count = u32()
    require(shader_count == (2 if count else 0), 'Invalid shader count')
    shaders = {}
    for _ in range(shader_count):
        h, = struct.unpack('<Q', take(8))
        size = u32()
        require(h in (VS, PS) and h not in shaders and size <= 256*1024, 'Invalid shader record')
        shaders[h] = take(size)
    failures = u32()
    require(not f.read(1), 'Trailing snapshot data')
    require(sum(totals) <= reserved, 'Payloads exceed reserved bytes')
    missing = sum(not b['data'] for d in draws for b in d['blobs']) + sum(not x for x in shaders.values())
    require(failures >= missing, 'Missing readback/shader not reported as failure')
    return dict(version=version, declined=declined, reserved=reserved, failures=failures, draws=draws, shaders=shaders)


def verify_fixture(path):
    x = read(path)
    require(len(x['draws']) == 2 and x['failures'] == 0 and x['declined'] >= 1, 'Incomplete fixture')
    require(all(d['complete_inputs'] and all(b['data'] for b in d['blobs']) for d in x['draws']), 'Missing fixture payload')
    first, second = x['draws']
    pixels = 1400 * 1400
    lut = bytes(c for z in range(4) for y in range(4) for a in range(4)
                for c in (0x10+z, 0x20+y, 0x30+a, 0xe0))
    expected = [struct.pack('<16f', *range(10, 26)), lut, b'\x41' * (4*pixels),
                bytes([51, 76, 102, 128]) * pixels,
                bytes(i ^ 0xa5 for i in range(256)), b'\x51' * 60]
    updated = [b'\x7a' * 64, b'\xcc' * 256, b'\xd2' * (4*pixels),
               bytes([204, 178, 153, 128]) * pixels, b'\xee' * 256, b'\xf1' * 60]
    require([b['data'] for b in first['blobs']] == expected, 'Draw-local data overwritten, cropped incorrectly or sliced incorrectly')
    require([b['data'] for b in second['blobs']] == updated, 'Second draw reused first-draw data')
    for d in x['draws']:
        require(d['frame'] == 42 and d['topology'] == 5 and d['start'] == 2 and d['vb_stride'] == d['vb_offset'] == 20, 'Draw/vertex metadata mismatch')
        require(d['layout'] == [['POSITION', 0, 6, 0, 0, 0, 0], ['TEXCOORD', 0, 16, 0, 12, 0, 0]], 'Layout metadata mismatch')
        require(d['sample_mask'] == 0xa5a5a5a5 and d['stencil_ref'] == 7, 'OM state mismatch')
        require(d['blend_factor'] == list(struct.unpack('<4I', struct.pack('<4f', .1, .2, .3, .4))), 'Blend factors mismatch')
        require(d['depth'][:3] == [1, 0, 5] and d['raster'][0:2] == [3, 1] and d['raster'][7] == 1, 'Depth/raster state mismatch')
        require(d['scissors'] == [[2, 3, 1402, 1403]] and d['viewports'] == [list(struct.unpack('<6I', struct.pack('<6f', 0, 0, 1404, 1406, 0, 1)))], 'Viewport/scissor mismatch')
        require([s[4] for s in d['samplers']] == [0, 0x3f800000, 0xbf800000], 'Samplers not retained separately')
        for b in d['blobs'][2:4]:
            require(b['meta']['origin'] == [2, 3, 0] and b['meta']['source'] == [1404, 1406, 1], 'Crop metadata mismatch')
        require(d['blobs'][2]['meta']['resource'] != d['blobs'][3]['meta']['resource'], 'Input/output identity conflated')
        require(d['blobs'][3]['meta']['format'] == 27 and d['blobs'][3]['meta']['view_format'] == 28, 'Typeless/view format lost')
    require(x['shaders'] == {VS: b'DXBC\x01\x02\x03\x04', PS: b'DXBC\x01\x02\x03\x04'}, 'Shader bytes missing')
    pending = read(str(path) + '.pending')
    require(len(pending['draws']) == 1 and pending['failures'] == 1, 'Pending-output failure not isolated')
    p = pending['draws'][0]['blobs']
    require(not p[3]['data'] and p[3]['meta']['bytes'] == 4*pixels and all(p[i]['data'] == updated[i] for i in (0, 1, 2, 4, 5)), 'Pending output confused with a valid black image')
    return x


def self_test():
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / 'tone.bin'
        good = b'EDVRTON1' + struct.pack('<6I', 1, 0, 0, 0, 0, 0)
        p.write_bytes(good)
        require(read(p)['draws'] == [], 'Empty snapshot failed')
        for bad in (b'', good[:-1], b'INVALID!' + good[8:], good + b'x',
                    b'EDVRTON1' + struct.pack('<4I', 1, 3, 0, 0),
                    b'EDVRTON1' + struct.pack('<5I', 1, 1, 0, 0, 65537)):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Malformed snapshot accepted')
    print('eye tone-map snapshot self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', nargs='?')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--verify-fixture', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return
    if not a.path:
        ap.error('snapshot path required')
    x = verify_fixture(a.path) if a.verify_fixture else read(a.path)
    print(f"EDVRTON1: {len(x['draws'])} draws, {x['reserved']} reserved bytes, {x['declined']} declines, {x['failures']} failures")


if __name__ == '__main__':
    main()
