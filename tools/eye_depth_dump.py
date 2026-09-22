#!/usr/bin/env python3
"""Read depth_<stamp>_f<frame>_<A|B>.bin from an armed eye run
(src/d3d11/eye_depth_capture.h).

Each file is ONE eye pass's completed depth for ONE frame, paired with the
armed eye run's other captures in edvr_logs\\pool\\:

    magic   'EDVRDEPT' (8 bytes)
    u32     version          -- 2 (1 for files written before 2026-09-22)
    u32     frame            -- the ledger's frame number
    u32     eye              -- 0 = A, 1 = B (the depth probe's scene pair,
                                first-bound pass = A)
    u32     width, height    -- of the depth-stencil texture
    u32     format           -- DXGI_FORMAT of the PAYLOAD: 41 R32_FLOAT
                                (the R32G8X24 family is converted at capture;
                                files from the first builds may carry 39
                                R32_TYPELESS / 40 D32_FLOAT, same payload)
    u32     const_first_float  -- VERSION 2 ONLY: the cb1 float index of the
                                block's first float; 1024 = float4 register 256
    u32     const_floats     -- the block's length in floats; 0 if the
                                readback failed or (v2) the buffer stops short
                                of register 275
    f32[const_floats]        -- VS b1 from the pass's first pool-carrying draw.
                                Version 2: float4 registers [256, 336), the end
                                clamped to the buffer. The game's pool VS reads
                                the view-projection's COLUMNS from registers
                                270..273 -- clip = x*c270 + y*c271 + z*c272 +
                                c273 for p = (record position - register 275),
                                register 275 being the eye origin in the record
                                frame (vs_EB5234DB6ADB491D).
                                Version 1: cb1 FLOATS [256, 592), i.e. float4
                                registers 64..147 -- the window was counted in
                                floats, not registers, and holds neither the
                                view-projection nor the origin. view_proj() and
                                camera_origin() return None for version 1.
    u32     depth_bytes      -- width*height*4, or 0 if the readback failed
    u8[depth_bytes]          -- row-major R32_FLOAT texels, reversed-Z, row 0
                                first
    u32     faults           -- SEH faults hit while writing this file

The copy is staged the moment the pass ends (the first eye draw whose DSV
differs), so the grid holds the depth every submitted draw left behind --
the eval-level culling answer for "which record won here". Joining it to
WHICH record needs the same run's pool_<stamp>_<frame>.bin (instance
positions at +16), inst_<stamp>_<frame>.bin and draws_<stamp>.bin (the draw
ledger): project a record's position with project() (version 2; for a
version 1 file take the camera from the eyemesh snapshot's VS b1 instead),
divide by w, and the surviving depth at that pixel says whether the record
won the depth test (reversed-Z: depth = near / w, near 0.025 m).

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
VERSIONS = (1, 2)
V1_FIRST_FLOAT = 256          # version 1: cb1 floats [256, 592) = registers 64..147
V1_CONST_FLOATS = 336
V2_MAX_FLOATS = (336 - 256) * 4   # version 2: registers [256, 336)
VP_REGISTER = 270             # the clip matrix's columns: registers 270..273
ORIGIN_REGISTER = 275         # the eye origin in the record frame
HEADER1 = struct.Struct('<8s7I')   # magic, version, frame, eye, w, h, format, const_floats
HEADER2 = struct.Struct('<8s8I')   # ... format, const_first_float, const_floats
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

    def register(self, r):
        """cb1 float4 register r as 4 floats, or None when the block does not
        hold it. Version 1 blocks are float-counted registers 64..147, so the
        camera registers are never inside them."""
        first = self.header['const_first_float']
        k = r * 4 - first
        if k < 0 or k % 4 or k + 4 > len(self.constants):
            return None
        return self.constants[k:k + 4]

    def view_proj(self):
        """cb1[270..273] as four float4: the clip matrix's COLUMNS, clip =
        x*c[0] + y*c[1] + z*c[2] + c[3]. None when unavailable."""
        cols = [self.register(VP_REGISTER + i) for i in range(4)]
        return None if any(c is None for c in cols) else cols

    def camera_origin(self):
        """cb1[275].xyz: the eye origin in the record frame. None when unavailable."""
        o = self.register(ORIGIN_REGISTER)
        return None if o is None else o[:3]

    def project(self, pos):
        """clip (x, y, z, w) of a record-frame position, or None without a camera."""
        vp = self.view_proj()
        org = self.camera_origin()
        if vp is None or org is None:
            return None
        p = [pos[i] - org[i] for i in range(3)]
        return [p[0] * vp[0][j] + p[1] * vp[1][j] + p[2] * vp[2][j] + vp[3][j] for j in range(4)]


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

    magic, version, frame, eye, width, height, fmt = struct.unpack(
        '<8s6I', take(32, 'header'))
    if magic != MAGIC:
        raise ValueError('Not an EDVRDEPT depth dump')
    if version not in VERSIONS:
        raise ValueError('Unsupported version %d' % version)
    first_float = u32('const_first_float') if version >= 2 else V1_FIRST_FLOAT
    const_floats = u32('const_floats')
    if eye > 1:
        raise ValueError('Invalid eye index %d' % eye)
    if not width or not height or width > 16384 or height > 16384:
        raise ValueError('Invalid dimensions %dx%d' % (width, height))
    if fmt not in (39, 40, 41):   # R32_TYPELESS / D32_FLOAT (legacy) / R32_FLOAT
        raise ValueError('Unexpected depth payload format %d' % fmt)
    limit = V1_CONST_FLOATS if version == 1 else V2_MAX_FLOATS
    if const_floats > limit or (version >= 2 and (first_float % 4 or const_floats % 4)):
        raise ValueError('Invalid constants block: %d floats from cb1 float %d' % (const_floats, first_float))
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
                  height=height, format=fmt, const_first_float=first_float,
                  const_floats=const_floats, depth_bytes=depth_bytes)
    return DepthDump(path, header, constants, depth)


def describe(d):
    h = d.header
    lines = ['%s: version %u frame %u eye %s %dx%d format %d' %
             (d.path, h['version'], h['frame'], 'AB'[h['eye']], h['width'], h['height'], h['format'])]
    if d.constants:
        first = h['const_first_float']
        if h['version'] == 1:
            lines.append('  constants: %u floats, cb1 floats [%d..%d) = registers 64..147 '
                         '(version 1: no camera in the block)' %
                         (len(d.constants), first, first + len(d.constants)))
        else:
            lines.append('  constants: %u floats, cb1 registers [%d..%d)' %
                         (len(d.constants), first // 4, (first + len(d.constants)) // 4))
        vp = d.view_proj()
        org = d.camera_origin()
        if vp:
            lines.append('  view-proj columns cb1[270..273]: ' +
                         ', '.join('[%g %g %g %g]' % tuple(c) for c in vp))
        if org:
            lines.append('  eye origin cb1[275]: [%g %g %g]' % tuple(org))
    else:
        lines.append('  constants: unavailable (readback failed or buffer too short)')
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

    def build(version=2, frame=7, eye=1, width=8, height=8, fmt=41, constants=None,
              depth=None, faults=0, first_float=1024):
        if constants is None:
            n = V1_CONST_FLOATS if version == 1 else V2_MAX_FLOATS
            base = V1_FIRST_FLOAT if version == 1 else first_float
            constants = [float(base + i) + 0.5 for i in range(n)]   # value = cb1 float index + 0.5
        depth = [(x + y * 0.01) for y in range(height) for x in range(width)] if depth is None else depth
        out = io.BytesIO()
        if version == 1:
            out.write(HEADER1.pack(MAGIC, 1, frame, eye, width, height, fmt, len(constants)))
        else:
            out.write(HEADER2.pack(MAGIC, version, frame, eye, width, height, fmt, first_float, len(constants)))
        if constants:
            out.write(_struct.pack('<%df' % len(constants), *constants))
        payload = _struct.pack('<%df' % len(depth), *depth) if depth else b''
        out.write(_struct.pack('<I', len(payload)))
        out.write(payload)
        out.write(_struct.pack('<I', faults))
        return out.getvalue()

    with tempfile.TemporaryDirectory() as tmp:
        # Version 2: the camera registers are inside the block, by register.
        good = Path(tmp) / 'depth_TEST_f7_B.bin'
        good.write_bytes(build())
        d = read(good)
        assert d.header['version'] == 2 and d.header['const_first_float'] == 1024, 'v2 header'
        assert d.header['frame'] == 7 and d.header['eye'] == 1, 'header round-trip'
        assert d.header['width'] == 8 and d.header['height'] == 8, 'grid size round-trip'
        assert len(d.constants) == V2_MAX_FLOATS, 'constants count'
        assert d.constants[0] == 1024.5, 'block starts at cb1 float 1024 = register 256'
        vp = d.view_proj()
        assert vp[0] == [1080.5, 1081.5, 1082.5, 1083.5], 'column c270 = cb1 floats 1080..1083'
        assert vp[3] == [1092.5, 1093.5, 1094.5, 1095.5], 'column c273 = cb1 floats 1092..1095'
        assert d.camera_origin() == [1100.5, 1101.5, 1102.5], 'eye origin = cb1 floats 1100..1102'
        clip = d.project([1100.5 + 1.0, 1101.5, 1102.5])   # p = (1, 0, 0): clip = c270 + c273
        assert clip == [1080.5 + 1092.5, 1081.5 + 1093.5, 1082.5 + 1094.5, 1083.5 + 1095.5], 'projection convention'
        assert len(d.depth) == 64, 'depth texel count'
        # f32 round-trip: compare within a float epsilon, not bit-exact
        assert abs(d.at(3, 4) - (3 + 4 * 0.01)) < 1e-6, 'depth grid access'
        assert describe(d).count('\n') == 4, 'summary shape'
        # A block clamped to a CB1[276] buffer: registers 256..275, camera intact.
        tight = Path(tmp) / 'tight.bin'
        tight.write_bytes(build(constants=[float(1024 + i) + 0.5 for i in range(80)]))
        d = read(tight)
        assert d.view_proj() is not None and d.camera_origin() == [1100.5, 1101.5, 1102.5], 'clamped block keeps the camera'
        # Version 1: registers 64..147, no camera -- never the old floats[14..29].
        old = Path(tmp) / 'old.bin'
        old.write_bytes(build(version=1, fmt=40))
        d = read(old)
        assert d.header['version'] == 1 and d.header['const_first_float'] == V1_FIRST_FLOAT, 'v1 implied window'
        assert len(d.constants) == V1_CONST_FLOATS and d.view_proj() is None and d.camera_origin() is None, \
            'a version 1 block never yields a camera'
        assert 'no camera' in describe(d), 'v1 summary says so'

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
        head[8:12] = _struct.pack('<I', 3)   # version field
        expect_value_error(bytes(head), 'unknown version')
        dims = bytearray(build())
        dims[20:24] = _struct.pack('<I', 0)   # width
        expect_value_error(bytes(dims), 'zero width')
        expect_value_error(build(constants=[0.0] * (V2_MAX_FLOATS + 4)), 'v2 block longer than registers 256..335')
        expect_value_error(build(first_float=1026), 'v2 block not register-aligned')
        nod = Path(tmp) / 'nod.bin'
        nod.write_bytes(build(depth=[]))
        d = read(nod)
        assert d.depth == [] and d.header['depth_bytes'] == 0, 'missing depth is explicit'
        noc = Path(tmp) / 'noc.bin'
        noc.write_bytes(build(constants=[]))
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
