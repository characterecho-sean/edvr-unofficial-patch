#!/usr/bin/env python3
"""Read the exact first-frame DLSS inputs saved beside a paired eye run.

EDVRTEX1: 8-byte magic, nine little-endian uint32 values (version, width,
height, DXGI format, packed row bytes, scene frame, eye, UI-bound, UI-flags),
then tightly packed rows. MV is RG16_FLOAT in input pixels; Z/SceneZ are
reverse device depth. UI's low two bits: 1 floating, 2 attached, 3 smoke;
its upper six bits are optional fixed bias. Bias is DLSS's actual R8 mask.
"""
from pathlib import Path
import json
import struct
import sys


def read(path):
    import numpy as np
    data = Path(path).read_bytes()
    if data[:8] != b'EDVRTEX1' or len(data) < 44:
        raise ValueError('Not an EDVRTEX1 capture')
    keys = ('version', 'width', 'height', 'format', 'row_bytes', 'frame', 'eye', 'ui_bound', 'ui_flags')
    meta = dict(zip(keys, struct.unpack_from('<9I', data, 8)))
    if meta['version'] != 1 or len(data) != 44 + meta['row_bytes'] * meta['height']:
        raise ValueError('Unsupported version or incomplete capture')
    fmt, w, h = meta['format'], meta['width'], meta['height']
    if fmt == 34:
        image = np.frombuffer(data, '<f2', offset=44).reshape(h, w, 2).astype('float32')
    elif fmt in (39, 40, 41):
        image = np.frombuffer(data, '<f4', offset=44).reshape(h, w)
    elif fmt in (19, 20):
        image = np.frombuffer(data, '<f4', offset=44).reshape(h, w, 2)[..., 0]
    elif fmt in (44, 45):
        image = (np.frombuffer(data, '<u4', offset=44).reshape(h, w) & 0xffffff) / 16777215.0
    elif fmt in (53, 55):
        image = np.frombuffer(data, '<u2', offset=44).reshape(h, w) / 65535.0
    elif fmt == 61:
        image = np.frombuffer(data, 'u1', offset=44).reshape(h, w)
    else:
        raise ValueError(f'Unsupported DXGI format {fmt}')
    return meta, image


if __name__ == '__main__':
    import numpy as np
    for path in sys.argv[1:]:
        meta, image = read(path)
        meta.update(path=str(path), minimum=float(np.nanmin(image)), maximum=float(np.nanmax(image)))
        print(json.dumps(meta))
