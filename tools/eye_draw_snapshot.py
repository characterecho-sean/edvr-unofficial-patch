#!/usr/bin/env python3
"""Read drawstate_HHMMSS.bin from an eye run (eye_draw_snapshot.h).

Each draw preserves VS b0/b1/b2 and PS b2 at the draw, before buffer reuse.
Version 2 preserves draw start/base and bounded HUD/sprite VB0/VB1/IB
payloads. Version 3 copies around the draw's base/start and records each
capture_offset separately from its binding offset, for the first three
watched frames. Earlier captures remain readable.
Version 4 adds source mesh records (ordinal UINT32_MAX-1), first-frame
geometry, and references to frame-local t33/t38/VB0 copies. Each buffer's
first_draw states when it was copied; later references are not new copies.
Source effect records (ordinal UINT32_MAX-2) retain b0/b1/b2 and the
bounded VB0/VB1/IB binding windows throughout the run. They have no mesh
references: billboard/flare placement is independent of the instance pool.
Version 5 adds each effect draw's original input layout so its separate
vertex and instance streams can be decoded without guessing their packing.

On-foot source records use ordinal UINT32_MAX. Their DSV is copied when
the screen composite runs, alongside its source colour; depth surfaces
use DSV format IDs 20/40/45/55. Only colour surfaces export to PNG.
The explicit on-foot dump permits 128 MiB per surface / 256 MiB total.
Frames/ordinals join draws_HHMMSS.bin, whose crop map joins the eye images.
Holo t2 surfaces are copied once per resource; their alpha is diagnostic,
not a per-frame history. No resource address is a persistent object ID.
"""
import argparse
from collections import Counter
import io
import json
from pathlib import Path
import struct
import tempfile


def read(path):
    data = Path(path).read_bytes()
    stream = io.BytesIO(data)

    def take(n):
        b = stream.read(n)
        if len(b) != n:
            raise ValueError('Truncated draw snapshot')
        return b

    def unpack(fmt):
        return struct.unpack(fmt, take(struct.calcsize(fmt)))

    if take(8) != b'EDVRDRW1':
        raise ValueError('Not an EDVRDRW1 snapshot')
    version, nd, ns, dropped = unpack('<4I')
    if version not in (1, 2, 3, 4, 5) or nd > 4096 or ns > 24:
        raise ValueError('Unsupported version or invalid counts')
    draws, surfaces = [], []
    vertex_bytes = 0
    for _ in range(nd):
        d = dict(zip(('vs', 'ps', 'target'), unpack('<3Q')))
        d.update(zip(('frame', 'ordinal', 'kind', 'count', 'instances', 'start_instance',
                      'width', 'height', 'texture'), unpack('<9I')))
        if d['texture'] != 0xffffffff and d['texture'] >= ns:
            raise ValueError('Invalid surface index')
        d['buffers'] = []
        for slot in range(4):
            identity, whole, size = unpack('<QII')
            if size > min(whole, 1024 if slot == 0 else 8192) or size % 16:
                raise ValueError('Invalid constant-buffer payload')
            d['buffers'].append(dict(identity=identity, whole=whole, data=take(size)))
        d['streams'] = []
        if version >= 2:
            d['start'], d['base'] = unpack('<Ii')
            for slot in range(3):
                offset, stride, whole = unpack('<3I')
                capture_offset = unpack('<I')[0] if version >= 3 else offset
                size, = unpack('<I')
                if size > min(max(0, whole-capture_offset), 256*1024):
                    raise ValueError('Invalid vertex payload')
                vertex_bytes += size
                if vertex_bytes > 32*1024*1024:
                    raise ValueError('Vertex payload budget exceeded')
                d['streams'].append(dict(offset=offset, stride=stride, whole=whole, capture_offset=capture_offset, data=take(size)))
        d['mesh'] = list(unpack('<3I')) if version >= 4 else [0xffffffff]*3
        d['layout'] = []
        if version >= 5:
            elements, = unpack('<I')
            if elements > 32:
                raise ValueError('Invalid input layout size')
            for _ in range(elements):
                semantic = take(64)
                if b'\0' not in semantic:
                    raise ValueError('Unterminated input semantic')
                e = dict(zip(('index', 'format', 'slot', 'offset', 'classification', 'step'), unpack('<6I')))
                e['semantic'] = semantic.split(b'\0', 1)[0].decode('ascii')
                if e['slot'] >= 32 or e['classification'] > 1:
                    raise ValueError('Invalid input layout element')
                d['layout'].append(e)
        draws.append(d)
    total = 0
    for _ in range(ns):
        s = dict(zip(('frame', 'width', 'height', 'format', 'size'), unpack('<5I')))
        bpp = {28: 4, 29: 4, 87: 4, 91: 4, 10: 8, 20: 8, 40: 4, 45: 4, 55: 2}.get(s['format'])
        expected = s['width'] * s['height'] * (bpp or 0)
        total += expected
        if not bpp or not s['width'] or not s['height'] or expected > 128*1024*1024 or total > 256*1024*1024:
            raise ValueError('Invalid surface descriptor')
        if s['size'] not in (0, expected):
            raise ValueError('Invalid surface payload')
        s['data'] = take(s['size'])
        surfaces.append(s)
    mesh_buffers, mesh_declined = [], 0
    if version >= 4:
        nb, mesh_declined = unpack('<2I')
        if nb > nd*3:
            raise ValueError('Invalid source buffer count')
        total = 0
        for _ in range(nb):
            b = dict(zip(('frame', 'first_draw', 'whole', 'stride', 'size'), unpack('<5I')))
            total += b['whole']
            if not 0 < b['whole'] <= 16*1024*1024 or total > 256*1024*1024 or b['size'] not in (0,b['whole']) or b['first_draw'] >= nd or b['stride'] not in (8,48,336):
                raise ValueError('Invalid source buffer descriptor')
            b['data'] = take(b['size'])
            mesh_buffers.append(b)
        for i,d in enumerate(draws):
            for role,ref in enumerate(d['mesh']):
                if ref == 0xffffffff:
                    continue
                if ref >= nb:
                    raise ValueError('Invalid source buffer reference')
                b = mesh_buffers[ref]
                if b['frame'] != d['frame'] or b['first_draw'] > i or b['stride'] != (336,48,8)[role]:
                    raise ValueError('Source buffer belongs to a different frame, role, or later draw')
    failures, = unpack('<I')
    if stream.read(1):
        raise ValueError('Trailing snapshot data')
    return dict(version=version, dropped=dropped, failures=failures, draws=draws, surfaces=surfaces,
                mesh_buffers=mesh_buffers, mesh_declined=mesh_declined)


def export_surfaces(capture, directory, dry_run=False):
    directory = Path(directory)
    colour_formats = (28, 29, 87, 91, 10)
    paths = [directory / f'surface_{i:02d}.png' for i, s in enumerate(capture['surfaces']) if s['data'] and s['format'] in colour_formats]
    if dry_run:
        return paths
    from PIL import Image
    if paths:
        directory.mkdir(parents=True, exist_ok=True)
    for i, s in enumerate(capture['surfaces']):
        if not s['data'] or s['format'] not in colour_formats:
            continue
        if s['format'] == 10:
            values = struct.iter_unpack('<e', s['data'])
            rgba = bytes(max(0, min(255, round(v[0] * 255))) for v in values)
        else:
            rgba = s['data']
        decoder = 'BGRA' if s['format'] in (87, 91) else 'RGBA'
        im = Image.frombytes('RGBA', (s['width'], s['height']), rgba, 'raw', decoder)
        im.save(directory / f'surface_{i:02d}.png')
    return paths


def verify_fixture(capture):
    assert len(capture['draws']) == 7 and len(capture['surfaces']) == 1
    assert capture['dropped'] == 0 and capture['failures'] == 0
    for i, d in enumerate(capture['draws'][:3]):
        assert (d['frame'], d['ordinal'], d['width'], d['height']) == (100+i, i*2, 8, 8)
        assert d['buffers'][0]['whole'] == 192
        vals = struct.unpack('<48f', d['buffers'][0]['data'])
        assert all(v == 12+i*3 for v in vals), 'Buffer was captured after a later write'
        assert not d['buffers'][1]['data'] and not d['buffers'][2]['data']
        assert d['buffers'][3]['data'] == d['buffers'][0]['data']
    s = capture['surfaces'][0]
    assert (s['width'], s['height'], s['format']) == (3, 2, 28)
    assert s['data'] == bytes(range(24)), 'Texture rows or first-draw timing differ'
    for i, d in enumerate(capture['draws'][3:5]):
        assert (d['start'], d['base']) == (2, -3)
        v = d['streams'][0]
        assert (v['offset'], v['stride'], v['whole']) == (8, 16, 64)
        assert v['data'] == bytes(range(8+i*64, 64+i*64)), 'Vertex buffer captured after reuse'
    for i, d in enumerate(capture['draws'][5:]):
        assert (d['start'], d['base']) == (150000, 8000)
        for s, binding, start, length in [(d['streams'][1], 16, 320016, 184), (d['streams'][2], 4, 300004, 12)]:
            assert (s['offset'], s['capture_offset']) == (binding, start)
            assert s['data'] == bytes((k+i*37) & 255 for k in range(start, start+length)), 'Shared-buffer draw window lost or overwritten'


def self_test():
    head = b'EDVRDRW1' + struct.pack('<4I', 1, 0, 0, 0)
    with tempfile.TemporaryDirectory() as td:
        p = Path(td)/'sample.bin'
        p.write_bytes(head + struct.pack('<I', 0))
        c = read(p)
        assert c['draws'] == [] and c['surfaces'] == []
        h4 = b'EDVRDRW1' + struct.pack('<4I',4,1,0,0)
        draw = struct.pack('<3Q9I',1,2,3,7,0xfffffffe,ord('X'),6,1,0,8,8,0xffffffff)
        draw += struct.pack('<QII',0,0,0)*4 + struct.pack('<Ii',0,0) + struct.pack('<5I',0,0,0,0,0)*3
        refs = struct.pack('<3I',0,0xffffffff,0xffffffff)
        table = struct.pack('<2I',1,0)
        blob = struct.pack('<5I',7,0,336,336,336) + bytes(336) + struct.pack('<I',0)
        good = h4+draw+refs+table+blob
        p.write_bytes(good)
        assert len(read(p)['mesh_buffers'][0]['data']) == 336
        h5 = b'EDVRDRW1' + struct.pack('<4I',5,1,0,0)
        layout = b'POSITION'.ljust(64,b'\0')+struct.pack('<6I',0,2,1,16,1,1)
        v5 = h5+draw+refs+struct.pack('<I',1)+layout+table+blob
        p.write_bytes(v5)
        assert read(p)['draws'][0]['layout'] == [dict(semantic='POSITION',index=0,format=2,slot=1,offset=16,classification=1,step=1)]
        for bad in (v5[:-1],h5+draw+refs+struct.pack('<I',33),h5+draw+refs+struct.pack('<I',1)+b'X'*64+layout[64:]+table+blob):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid effect layout accepted')
        malformed_mesh = [good[:-1], h4+draw+struct.pack('<3I',1,0xffffffff,0xffffffff)+table+blob]
        for frame,first,whole,stride,size in ((8,0,336,336,336),(7,1,336,336,336),(7,0,336,48,336),(7,0,336,336,335),(7,0,17*1024*1024,336,0)):
            malformed_mesh.append(h4+draw+refs+table+struct.pack('<5I',frame,first,whole,stride,size)+bytes(336)+struct.pack('<I',0))
        for bad in malformed_mesh:
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid source buffer accepted')
        for bad in (b'', head, head+b'\0'*5, head.replace(b'EDVRDRW1', b'EDVRBAD1'),
                    b'EDVRDRW1'+struct.pack('<4I', 5, 0, 0, 0),
                    b'EDVRDRW1'+struct.pack('<4I', 1, 4097, 0, 0)):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid snapshot accepted')
        c['surfaces'] = [dict(data=bytes(range(24)), format=28, width=3, height=2)]
        before = sorted(Path(td).rglob('*'))
        assert export_surfaces(c, Path(td)/'absent', True) == [Path(td)/'absent/surface_00.png']
        assert before == sorted(Path(td).rglob('*')), '--dry-run wrote files'
    print('eye draw snapshot self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', nargs='?')
    ap.add_argument('--surfaces', help='Export original RGBA panel textures to this directory')
    ap.add_argument('--dry-run', action='store_true', help='Report exports without writing anything')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--verify-fixture', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return
    if not a.path:
        ap.error('Name a drawstate capture')
    c = read(a.path)
    if a.verify_fixture:
        verify_fixture(c)
        effects = read(str(a.path)+'.effects')
        assert effects['version'] == 5 and len(effects['draws']) == 5 and effects['failures'] == 0
        for i,d in enumerate(effects['draws']):
            assert d['ordinal'] == 0xfffffffd and d['mesh'] == [0xffffffff]*3
            assert d['layout'][1] == dict(semantic='TEXCOORD',index=0,format=2,slot=1,offset=16,classification=1,step=1)
            assert bool(d['streams'][0]['data']) == (i < 4) and bool(d['streams'][1]['data']) == (i < 4)
        mesh = read(str(a.path)+'.mesh')
        assert mesh['failures'] == mesh['mesh_declined'] == 0 and len(mesh['draws']) == 4 and len(mesh['mesh_buffers']) == 6
        for i,d in enumerate(mesh['draws']):
            assert d['ordinal'] == 0xfffffffe and d['mesh'] == [i//2*3+j for j in range(3)]
            for role,ref in enumerate(d['mesh']):
                b = mesh['mesh_buffers'][ref]
                assert b['first_draw'] == i//2*2 and b['frame'] == 300+i//2
                assert b['data'] == bytes([21+role*10+i//2])*b['whole'], 'Source buffer copied after reuse or truncated'
        assert mesh['mesh_buffers'][1]['whole'] > 1024*1024, 'High bone indices lost'
        v = read(str(a.path)+'.vscreen')
        assert len(v['draws']) == 3 and len(v['surfaces']) == 2 and v['failures'] == 0
        assert v['draws'][0]['ordinal'] == 0xffffffff and v['draws'][0]['texture'] == 0xffffffff
        assert v['draws'][1]['texture'] == 0 and v['draws'][2]['texture'] == 0
        assert all(z == .75 for z in struct.unpack('<64f', v['surfaces'][1]['data'])), 'Source depth was copied before scene completion or after reuse'
        assert export_surfaces(v, Path('unused'), True) == [Path('unused/surface_00.png')], 'Depth was treated as colour'
        print('GPU draw snapshot fixture passed')
        return
    print(json.dumps(dict(version=c['version'], draws=len(c['draws']), surfaces=len(c['surfaces']), dropped=c['dropped'],
                          source_buffers=len(c['mesh_buffers']), source_bytes=sum(b['size'] for b in c['mesh_buffers']), source_declined=c['mesh_declined'],
                          vertex_draws=sum(any(s['data'] for s in d['streams']) for d in c['draws']),
                          vertex_bytes=sum(len(s['data']) for d in c['draws'] for s in d['streams']),
                          failures=c['failures'], shaders=Counter(f"{d['vs']:016X}" for d in c['draws'])), indent=2))
    if a.surfaces:
        for path in export_surfaces(c, a.surfaces, a.dry_run):
            print(('Would write ' if a.dry_run else 'Wrote ') + str(path))


if __name__ == '__main__':
    main()
