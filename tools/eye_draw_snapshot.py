#!/usr/bin/env python3
"""Read drawstate_HHMMSS.bin from an eye run (eye_draw_snapshot.h).

Each draw preserves VS b0/b1/b2 and PS b2 at the draw, before buffer reuse.
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
    if version != 1 or nd > 2048 or ns > 24:
        raise ValueError('Unsupported version or invalid counts')
    draws, surfaces = [], []
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
        draws.append(d)
    total = 0
    for _ in range(ns):
        s = dict(zip(('frame', 'width', 'height', 'format', 'size'), unpack('<5I')))
        bpp = {28: 4, 29: 4, 87: 4, 91: 4, 10: 8}.get(s['format'])
        expected = s['width'] * s['height'] * (bpp or 0)
        total += expected
        if not bpp or not s['width'] or not s['height'] or expected > 16*1024*1024 or total > 64*1024*1024:
            raise ValueError('Invalid surface descriptor')
        if s['size'] not in (0, expected):
            raise ValueError('Invalid surface payload')
        s['data'] = take(s['size'])
        surfaces.append(s)
    failures, = unpack('<I')
    if stream.read(1):
        raise ValueError('Trailing snapshot data')
    return dict(version=version, dropped=dropped, failures=failures, draws=draws, surfaces=surfaces)


def export_surfaces(capture, directory, dry_run=False):
    directory = Path(directory)
    paths = [directory / f'surface_{i:02d}.png' for i, s in enumerate(capture['surfaces']) if s['data']]
    if dry_run:
        return paths
    from PIL import Image
    if paths:
        directory.mkdir(parents=True, exist_ok=True)
    for i, s in enumerate(capture['surfaces']):
        if not s['data']:
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
    assert len(capture['draws']) == 3 and len(capture['surfaces']) == 1
    assert capture['dropped'] == 0 and capture['failures'] == 0
    for i, d in enumerate(capture['draws']):
        assert (d['frame'], d['ordinal'], d['width'], d['height']) == (100+i, i*2, 8, 8)
        assert d['buffers'][0]['whole'] == 192
        vals = struct.unpack('<48f', d['buffers'][0]['data'])
        assert all(v == 12+i*3 for v in vals), 'Buffer was captured after a later write'
        assert not d['buffers'][1]['data'] and not d['buffers'][2]['data']
        assert d['buffers'][3]['data'] == d['buffers'][0]['data']
    s = capture['surfaces'][0]
    assert (s['width'], s['height'], s['format']) == (3, 2, 28)
    assert s['data'] == bytes(range(24)), 'Texture rows or first-draw timing differ'


def self_test():
    head = b'EDVRDRW1' + struct.pack('<4I', 1, 0, 0, 0)
    with tempfile.TemporaryDirectory() as td:
        p = Path(td)/'sample.bin'
        p.write_bytes(head + struct.pack('<I', 0))
        c = read(p)
        assert c['draws'] == [] and c['surfaces'] == []
        for bad in (b'', head, head+b'\0'*5, head.replace(b'EDVRDRW1', b'EDVRBAD1'),
                    b'EDVRDRW1'+struct.pack('<4I', 2, 0, 0, 0),
                    b'EDVRDRW1'+struct.pack('<4I', 1, 2049, 0, 0)):
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
        print('GPU draw snapshot fixture passed')
        return
    print(json.dumps(dict(draws=len(c['draws']), surfaces=len(c['surfaces']), dropped=c['dropped'],
                          failures=c['failures'], shaders=Counter(f"{d['vs']:016X}" for d in c['draws'])), indent=2))
    if a.surfaces:
        for path in export_surfaces(c, a.surfaces, a.dry_run):
            print(('Would write ' if a.dry_run else 'Wrote ') + str(path))


if __name__ == '__main__':
    main()
