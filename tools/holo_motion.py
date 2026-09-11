#!/usr/bin/env python3
"""Read eye_HHMMSS_Holo.bin and join HoloCoverage's one-based indices.

Records are the first captured frame's GPU state, not every crop's state.
The shared table also contains ring and orbital coverage (key word 15).
The three map rows take current (clip X, clip Y, clip W, 1) to the previous
clip X/Y/W. Raster jitter is removed by the temporal consumer separately.
"""
import argparse
import json
from pathlib import Path
import struct
import tempfile


def read(path):
    data = Path(path).read_bytes()
    if len(data) < 16 or data[:8] != b'EDVRHLO1':
        raise ValueError('Not an EDVRHLO1 capture')
    count, stride = struct.unpack_from('<2I', data, 8)
    if count > 128 or stride != 240 or len(data) != 16 + count * stride:
        raise ValueError('Invalid count, stride or payload')
    records = []
    for i in range(count):
        at = 16 + i * stride
        values = struct.unpack_from('<28f', data, at + 128)
        key = struct.unpack_from('<32I', data, at)
        records.append(dict(index=i+1, key=key,
                            family={0: 'cockpit', 1: 'ring', 2: 'orbital', 3: 'sprite'}.get(key[15], 'unknown'),
                            clip=[values[j:j+4] for j in range(0, 12, 4)],
                            rows=[values[j:j+4] for j in range(12, 24, 4)],
                            eligible=values[24] == 1, matched=values[27] == 1))
    return records


def self_test():
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)/'holo.bin'
        values = [0.0]*28
        values[15], values[24], values[27] = -.02, 1, 1
        good = b'EDVRHLO1'+struct.pack('<2I', 1, 240)+bytes(128)+struct.pack('<28f', *values)
        path.write_bytes(good)
        r = read(path)[0]
        assert r['index'] == 1 and r['eligible'] and r['matched']
        assert abs(r['rows'][0][3]+.02) < 1e-8
        for bad in (b'', good[:-1], good+b'\0', good[:12]+struct.pack('<I', 272)+good[16:],
                    good[:8]+struct.pack('<I', 129)+good[12:]):
            path.write_bytes(bad)
            try:
                read(path)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid capture accepted')
    print('hologram motion reader self-test passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', nargs='?')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.path:
        print(json.dumps(read(args.path), indent=2))
    else:
        parser.error('Name a capture or use --self-test')
