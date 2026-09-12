#!/usr/bin/env python3
"""Read the exact first-frame DLSS inputs saved beside a paired eye run.

EDVRTEX1: 8-byte magic, nine little-endian uint32 values (version, width,
height, DXGI format, packed row bytes, scene frame, eye, UI-bound, UI-flags),
then tightly packed rows. MV is RG16_FLOAT in input pixels; Z/SceneZ are
reverse device depth. UI's low two bits: 1 floating, 2 attached, 3 smoke;
its upper six bits are optional fixed bias. Bias is DLSS's actual R8 mask.
WeaponMotion is RGBA16_FLOAT at source-screen size: original post-VS
previous-minus-current motion XY, source depth Z, validity W (1 valid,
2 new/rejected, 0 uncovered). ScreenMotion composes it into each eye.
ScreenMotion is RGBA16_FLOAT: previous-minus-current raster motion XY,
source reversed Z, validity (0 outside, 1 valid, 2 source disocclusion,
3 source UI requiring current reconstruction).
UI-flags bit 32 means this map was supplied to temporal reconstruction.
"""
from pathlib import Path
import json
import struct
import sys


def _load(path):
    data = Path(path).read_bytes()
    if data[:8] != b'EDVRTEX1' or len(data) < 44:
        raise ValueError('Not an EDVRTEX1 capture')
    keys = ('version', 'width', 'height', 'format', 'row_bytes', 'frame', 'eye', 'ui_bound', 'ui_flags')
    meta = dict(zip(keys, struct.unpack_from('<9I', data, 8)))
    if meta['version'] != 1 or len(data) != 44 + meta['row_bytes'] * meta['height']:
        raise ValueError('Unsupported version or incomplete capture')
    fmt, w, h = meta['format'], meta['width'], meta['height']
    pixel_bytes = {10: 8, 16: 8, 34: 4, 39: 4, 40: 4, 41: 4,
                   42: 4, 19: 8, 20: 8, 44: 4, 45: 4, 53: 2, 55: 2, 61: 1}
    if not w or not h or fmt not in pixel_bytes or meta['row_bytes'] != w * pixel_bytes[fmt]:
        raise ValueError('Unsupported format or invalid packed row extent')
    return meta, data


def read(path):
    import numpy as np
    meta, data = _load(path)
    fmt, w, h = meta['format'], meta['width'], meta['height']
    if fmt == 10:  # ScreenMotion: raster motion XY, source Z, validity (1/2/3).
        image = np.frombuffer(data, '<f2', offset=44).reshape(h, w, 4).astype('float32')
    elif fmt == 16:  # R32G32_FLOAT: hologram record index and exact raster depth
        image = np.frombuffer(data, '<f4', offset=44).reshape(h, w, 2)
    elif fmt == 34:
        image = np.frombuffer(data, '<f2', offset=44).reshape(h, w, 2).astype('float32')
    elif fmt in (39, 40, 41):
        image = np.frombuffer(data, '<f4', offset=44).reshape(h, w)
    elif fmt == 42:  # R32_UINT: exact terrain patch index, zero = no coverage
        image = np.frombuffer(data, '<u4', offset=44).reshape(h, w)
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


def self_test():
    import tempfile
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / 'screen.bin'
        pixels = struct.pack('<16e', 1.25, -.5, .005, 1, 0, 0, 0, 2,
                             0, 0, 0, 0, -2, 3, .025, 1)
        payload = b'EDVRTEX1' + struct.pack('<9I', 1, 2, 2, 10, 16, 17, 0, 0, 32) + pixels
        path.write_bytes(payload)
        meta, data = _load(path)
        assert meta['ui_flags'] == 32 and meta['frame'] == 17
        assert data[44:] == pixels
        # Analysis returns NumPy arrays, but validating the file contract
        # must not add a NumPy dependency to the normal DLL build.
        try:
            import numpy as np
        except ImportError:
            pass
        else:
            _, image = read(path)
            expected = np.array(struct.unpack('<16e', pixels), dtype='float32').reshape(2, 2, 4)
            np.testing.assert_array_equal(image, expected)
        for invalid in (b'', payload[:-1], payload + b'x',
                        payload[:24] + struct.pack('<I', 8) + payload[28:],
                        payload[:8] + struct.pack('<I', 2) + payload[12:]):
            path.write_bytes(invalid)
            try:
                _load(path)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid eye capture accepted')
    print('Eye input reader self-test passed')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        if path == '--self-test':
            self_test()
            continue
        import numpy as np
        meta, image = read(path)
        meta.update(path=str(path), minimum=float(np.nanmin(image)), maximum=float(np.nanmax(image)))
        print(json.dumps(meta))
