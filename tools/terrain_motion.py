#!/usr/bin/env python3
"""Read the draw-time terrain history saved with the first eye-run inputs.

EDVRTRN1, uint32 count/stride, then count records (272 bytes each):
12 uint4 patch/texture keys, float4 quaternion, float4 translation (w is
history-valid), three float4 current-to-previous view transform rows.
The frame/eye are those of the matching TerrainIndex/TerrainZ EDVRTEX1 files.
Read-only; requires only the Python standard library.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import tempfile


def read(path):
    data = Path(path).read_bytes()
    if len(data) < 16 or data[:8] != b'EDVRTRN1':
        raise ValueError('Not an EDVRTRN1 terrain capture')
    count, stride = struct.unpack_from('<2I', data, 8)
    if count > 512 or stride != 272 or len(data) != 16 + count * stride:
        raise ValueError('Invalid count/stride or incomplete terrain capture')
    records = []
    for i in range(count):
        at = 16 + i * stride
        key = struct.unpack_from('<48I', data, at)
        f = struct.unpack_from('<20f', data, at + 192)
        if f[7] not in (0, 1):
            raise ValueError('Invalid terrain history flag')
        if f[7] and not all(math.isfinite(v) for v in f):
            raise ValueError('Non-finite valid terrain transform')
        records.append(dict(index=i + 1, key=key, q=f[:4], position=f[4:7],
                            valid=bool(f[7]), rows=[f[8:12], f[12:16], f[16:20]]))
    return records


def verify_fixture(records):
    assert len(records) == 1 and records[0]['valid']
    t = [r[3] for r in records[0]['rows']]
    assert all(abs(a - b) < 1e-5 for a, b in zip(t, (2, -1, 10))), t


def self_test():
    values = [0.0] * 20
    values[3] = values[7] = values[8] = values[13] = values[18] = 1.0
    values[11], values[15], values[19] = 2, -1, 10
    blob = b'EDVRTRN1' + struct.pack('<2I48I20f', 1, 272, *range(48), *values)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'terrain.bin'
        path.write_bytes(blob)
        records = read(path)
        verify_fixture(records)
        assert records[0]['key'] == tuple(range(48))
        for bad in (blob[:15], blob[:-1], blob + b'x', b'badmagic' + blob[8:],
                    blob[:8] + struct.pack('<2I', 513, 272) + blob[16:],
                    blob[:12] + struct.pack('<I', 256) + blob[16:]):
            path.write_bytes(bad)
            try:
                read(path)
            except ValueError:
                pass
            else:
                raise AssertionError('Malformed terrain capture accepted')
        values[7] = float('nan')
        path.write_bytes(blob[:208] + struct.pack('<20f', *values))
        try:
            read(path)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid history flag accepted')
    print('Terrain motion reader self-test passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', nargs='?')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--verify-fixture', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.path:
        records = read(args.path)
        if args.verify_fixture:
            verify_fixture(records)
            print('GPU terrain dump fixture verified')
        else:
            print(json.dumps(dict(path=args.path, count=len(records),
                matched=sum(r['valid'] for r in records),
                unmatched=[r['index'] for r in records if not r['valid']],
                translations=[dict(index=r['index'], xyz=[v[3] for v in r['rows']])
                              for r in records if r['valid']]), indent=2))
    else:
        parser.error('path or --self-test required')
