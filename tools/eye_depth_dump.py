#!/usr/bin/env python3
"""Read depth_<stamp>_f<frame>_<A|B>.bin from an armed eye run
(src/d3d11/eye_depth_capture.h).

Each file is ONE eye pass's completed depth for ONE frame, paired with the
armed eye run's other captures in edvr_logs\\pool\\:

    magic   'EDVRDEPT' (8 bytes)
    u32     version (1)
    u32     frame            -- the ledger's frame number
    u32     eye              -- 0 = A, 1 = B (scene order: the frame's two
                                depth targets in first-seen order)
    u32     width, height    -- of the depth-stencil texture
    u32     format           -- DXGI_FORMAT of the PAYLOAD: 41 R32_FLOAT
                                (the R32G8X24 family is converted at capture;
                                files from the first builds may carry 39
                                R32_TYPELESS / 40 D32_FLOAT, same payload)
    u32     const_floats     -- 336, or 0 if the constants readback failed
    f32[const_floats]        -- VS b1 floats [256, 592) from the pass's first
                                pool-carrying draw. Index k corresponds to
                                cb1[256 + k]: the view-projection rows are
                                floats[14..29] (cb1[270..273]), the camera-
                                relative origin floats[19..22] (cb1[275]).
    u32     depth_bytes      -- width*height*4, or 0 if the readback failed
    u8[depth_bytes]          -- row-major D32_FLOAT texels, row 0 first
    u32     faults           -- SEH faults hit while writing this file

The copy is staged the moment the pass ends (the first eye draw whose DSV
differs), so the grid holds the depth every submitted draw left behind --
the eval-level culling answer for "which record won here". Joining it to
WHICH record needs the same run's pool_<stamp>_<frame>.bin (instance
positions), inst_<stamp>_<frame>.bin, draws_<stamp>.bin (the draw ledger)
and the dumped VS (vs_*.dxbc): project a record's position through
floats[14..29], subtract the origin floats[19..22], and the surviving depth
at that pixel says the record won the depth test.

Usage:  python tools/eye_depth_dump.py <file-or-dir> [--grid x y]
        python tools/eye_depth_dump.py --self-test
Exit:   0 read ok, 1 error, 2 self-test failure.
"""
import argparse
import io
import struct
import sys
from pathlib import Path

MAGIC = b'EDVRDEPT'
VERSION = 1
CONST_FLOATS = 336
CONST_FIRST_CB1 = 256
HEADER = struct.Struct('<8s7I')   # magic, version, frame, eye, w, h, format, const_floats
GRID_BPP = 4


class DepthDump:
    def __init__(self, path, header, constants, depth):
        self.path = str(path)
        self.header = header
        self.constants = constants   # list of float, [] when unavailable
        self.depth = depth           # list of float row-major, [] when unavailable

    def at(self, x, y):
        w = self.header['width']
        if not self.depth or x >= w or y >= self.header['height']:
            raise IndexError('(%d,%d) outside %dx%d depth grid' %
                             (x, y, w, self.header['height']))
        return self.depth[y * w + x]

    def view_proj(self):
        """cb1[270..273] as four float4 rows: floats[14..29]."""
        if len(self.constants) < 30:
            return None
        return [self.constants[14 + r * 4: 18 + r * 4] for r in range(4)]

    def camera_origin(self):
        """cb1[275] as a float3: floats[19..22]."""
        if len(self.constants) < 23:
            return None
        return self.constants[19:22]


def read(path):
    data = Path(path).read_bytes()
    stream = io.BytesIO(data)

    def take(n, what):
        b = stream.read(n)
        if len(b) != n:
            raise ValueError('Truncated depth dump: %s' % what)
        return b

    def u32(what):
        return struct.unpack('<I', take(4, what))[0]

    magic, version, frame, eye, width, height, fmt, const_floats = HEADER.unpack(
        take(HEADER.size, 'header'))
    if magic != MAGIC:
        raise ValueError('Not an EDVRDEPT depth dump')
    if version != VERSION:
        raise ValueError('Unsupported version %d' % version)
    if eye > 1:
        raise ValueError('Invalid eye index %d' % eye)
    if not width or not height or width > 16384 or height > 16384:
        raise ValueError('Invalid dimensions %dx%d' % (width, height))
    if fmt not in (39, 40, 41):   # R32_TYPELESS / D32_FLOAT (legacy) / R32_FLOAT
        raise ValueError('Unexpected depth payload format %d' % fmt)
    if const_floats > CONST_FLOATS:
        raise ValueError('Invalid constants count %d' % const_floats)
    constants = list(struct.unpack('<%df' % const_floats,
                                   take(const_floats * 4, 'constants'))) if const_floats else []
    depth_bytes = u32('depth byte count')
    expected = width * height * GRID_BPP
    if depth_bytes not in (0, expected):
        raise ValueError('Depth payload %d bytes, expected 0 or %d' %
                         (depth_bytes, expected))
    depth = list(struct.unpack('<%df' % (depth_bytes // GRID_BPP),
                               take(depth_bytes, 'depth'))) if depth_bytes else []
    faults = u32('fault count')
    if faults:
        raise ValueError('%d SEH faults while writing this file' % faults)
    if stream.read(1):
        raise ValueError('Trailing bytes after the depth payload')
    header = dict(version=version, frame=frame, eye=eye, width=width,
                  height=height, format=fmt, const_floats=const_floats,
                  depth_bytes=depth_bytes)
    return DepthDump(path, header, constants, depth)


def describe(d):
    h = d.header
    lines = ['%s: frame %u eye %s %dx%d format %d' %
             (d.path, h['frame'], 'AB'[h['eye']], h['width'], h['height'], h['format'])]
    if d.constants:
        vp = d.view_proj()
        org = d.camera_origin()
        lines.append('  constants: %u floats (cb1[%d..%d))' %
                     (len(d.constants), CONST_FIRST_CB1, CONST_FIRST_CB1 + len(d.constants)))
        if vp:
            lines.append('  view-proj cb1[270..273]: ' +
                         ', '.join('[%g %g %g %g]' % tuple(r) for r in vp))
        if org:
            lines.append('  camera-relative origin cb1[275]: [%g %g %g]' % tuple(org))
    else:
        lines.append('  constants: unavailable (readback failed)')
    if d.depth:
        finite = [v for v in d.depth if v == v and v not in (float('inf'), float('-inf'))]
        lines.append('  depth: %u texels, min %g, max %g' %
                     (len(d.depth), min(finite), max(finite)))
    else:
        lines.append('  depth: unavailable (readback failed)')
    return '\n'.join(lines)


def self_test():
    import struct as _struct
    import tempfile

    def build(frame=7, eye=1, width=8, height=8, fmt=40, constants=None,
              depth=None, faults=0):
        constants = [0.25 * (i + 1) for i in range(CONST_FLOATS)] if constants is None else constants
        depth = [(x + y * 0.01) for y in range(height) for x in range(width)] if depth is None else depth
        out = io.BytesIO()
        out.write(HEADER.pack(MAGIC, VERSION, frame, eye, width, height, fmt, len(constants)))
        if constants:
            out.write(_struct.pack('<%df' % len(constants), *constants))
        payload = _struct.pack('<%df' % len(depth), *depth) if depth else b''
        out.write(_struct.pack('<I', len(payload)))
        out.write(payload)
        out.write(_struct.pack('<I', faults))
        return out.getvalue()

    with tempfile.TemporaryDirectory() as tmp:
        good = Path(tmp) / 'depth_TEST_f7_B.bin'
        good.write_bytes(build())
        d = read(good)
        assert d.header['frame'] == 7 and d.header['eye'] == 1, 'header round-trip'
        assert d.header['width'] == 8 and d.header['height'] == 8, 'grid size round-trip'
        assert len(d.constants) == CONST_FLOATS, 'constants count'
        assert d.constants[0] == 0.25 and d.constants[14] == 3.75, 'constants values'
        assert d.view_proj()[0] == d.constants[14:18], 'view-proj slice'
        assert d.camera_origin() == d.constants[19:22], 'camera origin slice'
        assert len(d.depth) == 64, 'depth texel count'
        # f32 round-trip: compare within a float epsilon, not bit-exact
        assert abs(d.at(3, 4) - (3 + 4 * 0.01)) < 1e-6, 'depth grid access'
        assert describe(d).count('\n') == 4, 'summary shape'

        def expect_value_error(blob, why):
            bad = Path(tmp) / 'bad.bin'
            bad.write_bytes(blob)
            try:
                read(bad)
            except ValueError:
                return
            raise AssertionError('no rejection: %s' % why)

        expect_value_error(b'EDVRDRW1' + build()[8:], 'wrong magic')
        expect_value_error(build(faults=1), 'nonzero fault count')
        expect_value_error(build()[:-2], 'truncated tail')
        head = bytearray(build())
        head[8:12] = _struct.pack('<I', 2)   # version field
        expect_value_error(bytes(head), 'wrong version')
        dims = bytearray(build())
        dims[20:24] = _struct.pack('<I', 0)   # width
        expect_value_error(bytes(dims), 'zero width')
        no_depth = build(depth=[])
        d = read(Path(tmp) / 'good2.bin') if False else None
        nod = Path(tmp) / 'nod.bin'
        nod.write_bytes(no_depth)
        d = read(nod)
        assert d.depth == [] and d.header['depth_bytes'] == 0, 'missing depth is explicit'
        no_const = build(constants=[])
        noc = Path(tmp) / 'noc.bin'
        noc.write_bytes(no_const)
        d = read(noc)
        assert d.constants == [] and d.view_proj() is None, 'missing constants explicit'
    print('eye_depth_dump self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('path', nargs='?', help='a depth .bin file or a directory of them')
    ap.add_argument('--grid', nargs=2, type=int, metavar=('X', 'Y'),
                    help='print the depth value at one pixel')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        try:
            self_test()
        except (AssertionError, ValueError) as e:
            print('eye_depth_dump self-test FAILED: %s' % e, file=sys.stderr)
            return 2
        return 0
    if not args.path:
        ap.error('a file or directory is required (or --self-test)')
    p = Path(args.path)
    files = sorted(p.glob('depth_*.bin')) if p.is_dir() else [p]
    if not files:
        print('no depth_*.bin under %s' % p, file=sys.stderr)
        return 1
    try:
        for f in files:
            d = read(f)
            print(describe(d))
            if args.grid:
                print('  depth[%d,%d] = %g' % (args.grid[0], args.grid[1],
                                               d.at(args.grid[0], args.grid[1])))
    except (OSError, ValueError, IndexError) as e:
        print('eye_depth_dump: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
