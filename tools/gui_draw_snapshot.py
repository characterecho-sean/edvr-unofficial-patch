#!/usr/bin/env python3
"""Read gui_HHMMSS.bin from an explicit eye dump.

EDVRGUI1 v1: bounded JSON draw/state records followed by JSON-described
GPU blobs. D3D11 descriptors are arrays of little-endian uint32 words;
floats retain their bit representation. Layout entries are 64 semantic
bytes then six uint32 fields (index, format, slot, offset, class, step).
Buffer captures retain binding and copied offsets separately. Texture
blobs hold tightly packed rows for each original mip, including BC blocks.
target_before/depth_before are the initial contents before the first
captured draw using that resource. No capture content is an instruction.
"""
import argparse
import io
import json
from pathlib import Path
import struct
import tempfile


def read(path):
    path = Path(path)
    if path.stat().st_size > 128 * 1024 * 1024:
        raise ValueError('GUI capture exceeds file budget')
    stream = io.BytesIO(path.read_bytes())

    def take(n):
        data = stream.read(n)
        if len(data) != n:
            raise ValueError('Truncated GUI capture')
        return data

    def u32():
        return struct.unpack('<I', take(4))[0]

    def meta():
        n = u32()
        if n > 65536:
            raise ValueError('Oversize GUI metadata')
        item = json.loads(take(n))
        if not isinstance(item, dict):
            raise ValueError('GUI metadata must be an object')
        return item

    if take(8) != b'EDVRGUI1' or u32() != 1:
        raise ValueError('Unsupported GUI snapshot')
    nd, nb, declined, missing_layouts = (u32() for _ in range(4))
    if nd > 512 or nb > 8192:
        raise ValueError('GUI capture count exceeds budget')
    draws = [meta() for _ in range(nd)]
    blobs, total = [], 0
    for _ in range(nb):
        b = meta()
        n = u32()
        total += n
        if n > 16 * 1024 * 1024 or total > 96 * 1024 * 1024:
            raise ValueError('GUI payload exceeds budget')
        if b.get('role') == 'texture':
            expected = b['row'] * b['rows']
        else:
            expected = b['whole'] - b['offset']
        if expected < 0 or (n and n > expected):
            raise ValueError('Invalid GUI payload extent')
        b['data'] = take(n)
        blobs.append(b)
    failures = u32()
    if stream.read(1):
        raise ValueError('Trailing GUI capture data')

    def index(value):
        if not isinstance(value, int) or not -1 <= value < nb:
            raise ValueError('Invalid GUI blob index')

    for d in draws:
        for key in ('vs_cb2', 'ps_cb2', 'pool'):
            index(d[key])
        for s in d['streams']:
            index(s['blob'])
        for t in [d['target_before'], d['depth_before'], *d['textures']]:
            if t is not None:
                for i in t['mips']:
                    index(i)
                    if i < 0 or blobs[i]['role'] != 'texture':
                        raise ValueError('Invalid GUI texture reference')
        if len(d['layout']) > 32 or any(len(e) != 22 for e in d['layout']):
            raise ValueError('Invalid GUI input layout')
    return dict(draws=draws, blobs=blobs, declined=declined,
                missing_layouts=missing_layouts, failures=failures)


def verify_fixture(c):
    assert len(c['draws']) == 2 and c['missing_layouts'] == 0
    assert c['failures'] == 0 and c['declined'] == 0
    d = c['draws'][0]
    assert d['frame'] == 200 and d['width'] == 8
    assert len(d['layout']) == 1
    baseline = c['blobs'][d['target_before']['mips'][0]]['data']
    assert baseline == bytes([51, 102, 153, 255]) * 64
    assert c['blobs'][d['depth_before']['mips'][0]]['data'] == struct.pack('<f', .75) * 64
    atlas = d['textures'][1]['mips']
    assert [len(c['blobs'][i]['data']) for i in atlas] == [64, 16, 16]
    assert all(c['blobs'][i]['data'] == bytes([17 + n]) * len(c['blobs'][i]['data']) for n, i in enumerate(atlas))
    assert c['blobs'][d['vs_cb2']]['data'] == struct.pack('<f', 7) * 48
    assert c['blobs'][c['draws'][1]['vs_cb2']]['data'] == struct.pack('<f', 8) * 48


def self_test():
    empty = b'EDVRGUI1' + struct.pack('<6I', 1, 0, 0, 0, 0, 0)
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / 'test.bin'
        p.write_bytes(empty)
        assert read(p)['draws'] == []
        for data in (b'', empty[:-1], empty + b'x', empty[:12] + struct.pack('<I', 513) + empty[16:]):
            p.write_bytes(data)
            try:
                read(p)
            except (ValueError, struct.error):
                pass
            else:
                raise AssertionError('Invalid GUI capture accepted')
    print('GUI snapshot reader self-test passed')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('file', nargs='?')
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--verify-fixture', action='store_true')
    args = p.parse_args()
    if args.self_test:
        self_test()
    if args.file:
        c = read(args.file)
        if args.verify_fixture:
            verify_fixture(c)
            print('GPU GUI snapshot fixture verified')
        else:
            print(json.dumps({k: len(v) if k in ('draws', 'blobs') else v for k, v in c.items()}, indent=2))
    elif not args.self_test:
        p.error('file or --self-test is required')


if __name__ == '__main__':
    main()
