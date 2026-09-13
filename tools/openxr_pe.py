#!/usr/bin/env python3
"""Read-only, bounded PE32+ checks for the experimental Frontier native route.

Only static imports are inspected; runtime GetProcAddress calls need separate
call-site evidence. No DLL or game code is loaded by this tool.
"""
import argparse
import json
from pathlib import Path
import sys

EXPECTED = dict(enumerate((
    'VRControlPanel', 'VRDashboardManager', 'VRTrackedCamera',
    'VR_GetGenericInterface', 'VR_GetInitToken', 'VR_GetStringForHmdError',
    'VR_GetVRInitErrorAsEnglishDescription', 'VR_GetVRInitErrorAsSymbol',
    'VR_InitInternal', 'VR_IsHmdPresent', 'VR_IsInterfaceVersionValid',
    'VR_IsRuntimeInstalled', 'VR_RuntimePath', 'VR_ShutdownInternal',
    'edvrConfigureNativeRuntime', 'edvrGetNativeRuntimeStatus'), 1))
GAME = {'VR_InitInternal', 'VR_ShutdownInternal', 'VR_GetGenericInterface',
        'VR_IsInterfaceVersionValid', 'VR_GetInitToken'}


class PEError(ValueError):
    pass


def integer(data, offset, size):
    if offset < 0 or offset + size > len(data):
        raise PEError('truncated integer')
    return int.from_bytes(data[offset:offset + size], 'little')


class Image:
    def __init__(self, data):
        self.data = data
        if len(data) < 64 or data[:2] != b'MZ':
            raise PEError('bad DOS header')
        nt = integer(data, 60, 4)
        if data[nt:nt + 4] != b'PE\0\0':
            raise PEError('bad NT signature')
        machine = integer(data, nt + 4, 2)
        count = integer(data, nt + 6, 2)
        opt_size = integer(data, nt + 20, 2)
        opt = nt + 24
        if machine != 0x8664 or integer(data, opt, 2) != 0x20b:
            raise PEError('requires AMD64 PE32+')
        if opt_size < 112 or opt + opt_size > len(data):
            raise PEError('truncated optional header')
        dirs = integer(data, opt + 108, 4)
        if dirs > (opt_size - 112) // 8:
            raise PEError('directories exceed optional header')
        self.directories = [(integer(data, opt + 112 + i * 8, 4),
                             integer(data, opt + 116 + i * 8, 4))
                            for i in range(dirs)]
        table = opt + opt_size
        if not count or count > 96 or table + count * 40 > len(data):
            raise PEError('invalid section table')
        self.sections = []
        for i in range(count):
            off = table + i * 40
            virtual_size = integer(data, off + 8, 4)
            rva = integer(data, off + 12, 4)
            raw_size = integer(data, off + 16, 4)
            raw = integer(data, off + 20, 4)
            span = max(virtual_size, raw_size)
            if raw + raw_size > len(data) or rva + span > 0x100000000:
                raise PEError('section exceeds file or RVA space')
            for previous, previous_span, _, _ in self.sections:
                if span and previous_span and rva < previous + previous_span and previous < rva + span:
                    raise PEError('overlapping virtual sections')
            self.sections.append((rva, span, raw, raw_size))

    def mapping(self, rva, size=1):
        if rva <= 0 or size <= 0 or rva + size > 0x100000000:
            raise PEError('invalid RVA range')
        for start, span, raw, raw_size in self.sections:
            if start <= rva and rva + size <= start + span:
                delta = rva - start
                if delta + size > raw_size:
                    raise PEError('RVA enters zero-fill region')
                return raw + delta, raw_size - delta
        raise PEError('RVA outside a section')

    def number(self, rva, size):
        return integer(self.data, self.mapping(rva, size)[0], size)

    def string(self, rva):
        off, available = self.mapping(rva)
        end = self.data.find(b'\0', off, off + min(available, 4096))
        if end < 0:
            raise PEError('unterminated or oversized PE string')
        try:
            return self.data[off:end].decode('ascii')
        except UnicodeError as exc:
            raise PEError('non-ASCII PE string') from exc

    def directory(self, index):
        if index >= len(self.directories):
            return 0, 0
        rva, size = self.directories[index]
        if bool(rva) != bool(size):
            raise PEError('incomplete data directory')
        if rva:
            self.mapping(rva, size)
        return rva, size


def _native_exports(im):
    rva, size = im.directory(0)
    if not rva or size < 40:
        raise PEError('missing export directory')
    base = im.number(rva + 16, 4)
    functions = im.number(rva + 20, 4)
    names = im.number(rva + 24, 4)
    if base != 1 or functions != len(EXPECTED) or names != functions:
        raise PEError('export count/base mismatch')
    eat = im.number(rva + 28, 4)
    name_table = im.number(rva + 32, 4)
    ordinals = im.number(rva + 36, 4)
    im.mapping(eat, functions * 4)
    im.mapping(name_table, names * 4)
    im.mapping(ordinals, names * 2)
    result = {}
    for i in range(names):
        ordinal = base + im.number(ordinals + i * 2, 2)
        if ordinal not in EXPECTED or ordinal in result:
            raise PEError('invalid or aliased export ordinal')
        name = im.string(im.number(name_table + i * 4, 4))
        function = im.number(eat + (ordinal - base) * 4, 4)
        if rva <= function < rva + size:
            raise PEError('forwarded export: ' + name)
        im.mapping(function)
        result[ordinal] = name
    if result != EXPECTED:
        raise PEError('export name/ordinal contract mismatch: ' + repr(result))
    return result


def native_exports(path):
    return _native_exports(Image(Path(path).read_bytes()))


def _game_imports(im):
    rows = []
    for index, kind, width in ((1, 'import', 20), (13, 'delay', 32)):
        rva, size = im.directory(index)
        if not rva:
            continue
        terminated = False
        for off in range(0, size - width + 1, width):
            values = [im.number(rva + off + i, 4) for i in range(0, width, 4)]
            if not any(values):
                terminated = True
                break
            if kind == 'delay':
                if values[0] != 1:
                    raise PEError('delay import VA attributes unsupported')
                name_rva, first, thunk = values[1], values[3], values[4]
            else:
                thunk, _, _, name_rva, first = values
                thunk = thunk or first
            dll = im.string(name_rva)
            if dll.replace('\\', '/').rsplit('/', 1)[-1].lower() != 'openvr_api.dll':
                continue
            im.mapping(first, 8)
            # Each entry is mapped independently, including the terminator;
            # a malformed table cannot run into unmapped/zero-fill memory.
            for i in range(65536):
                value = im.number(thunk + i * 8, 8)
                if not value:
                    break
                row = {'kind': kind, 'dll': dll}
                if value & (1 << 63):
                    if value & ~((1 << 63) | 0xffff):
                        raise PEError('invalid ordinal thunk bits')
                    row['ordinal'] = value & 0xffff
                else:
                    im.mapping(value, 2)
                    row['name'] = im.string(value + 2)
                rows.append(row)
            else:
                raise PEError('import thunk limit exceeded')
        if not terminated:
            raise PEError('import descriptors lack bounded terminator')
    return rows


def game_openvr_imports(path):
    return _game_imports(Image(Path(path).read_bytes()))


def _validate_rows(rows):
    for row in rows:
        if 'ordinal' in row or row.get('name') not in GAME:
            raise PEError('unrecognized or ordinal OpenVR import: ' + repr(row))
    if {row['name'] for row in rows} != GAME:
        raise PEError('required Frontier OpenVR imports differ')
    return rows


def validate_frontier_imports(game_path, native_path):
    native_exports(native_path)
    return _validate_rows(game_openvr_imports(game_path))


def self_test():
    """Synthetic PE contracts exercise real decoding without loading binaries."""
    checks = 0

    def check(value):
        nonlocal checks
        checks += 1
        assert value, 'check %d' % checks

    def refused(fn):
        try:
            fn()
        except PEError:
            check(True)
        else:
            check(False)

    def fixture(delay=False, fallback=False):
        d = bytearray(0x4200)

        def put(off, value, size=4):
            d[off:off + size] = value.to_bytes(size, 'little')

        def off(rva):
            return rva - 0x1000 + 0x200

        def rv(rva, value, size=4):
            put(off(rva), value, size)

        def string(rva, value):
            b = value.encode('ascii') + b'\0'
            d[off(rva):off(rva) + len(b)] = b

        d[:2] = b'MZ'
        put(60, 0x80)
        d[0x80:0x84] = b'PE\0\0'
        put(0x84, 0x8664, 2)
        put(0x86, 1, 2)
        put(0x94, 240, 2)
        put(0x98, 0x20b, 2)
        put(0x98 + 108, 16)
        section = 0x98 + 240
        for pos, value in ((8, 0x4000), (12, 0x1000), (16, 0x4000), (20, 0x200)):
            put(section + pos, value)
        dirs = 0x98 + 112
        put(dirs, 0x1000)
        put(dirs + 4, 0x600)
        for pos, value in ((16, 1), (20, 16), (24, 16), (28, 0x1040),
                           (32, 0x1080), (36, 0x10c0)):
            rv(0x1000 + pos, value)
        next_name = 0x1100
        for i, name in enumerate(EXPECTED.values()):
            rv(0x1040 + i * 4, 0x4000 + i)
            rv(0x1080 + i * 4, next_name)
            rv(0x10c0 + i * 2, i, 2)
            string(next_name, name)
            next_name += len(name) + 1
        kind = 13 if delay else 1
        width = 32 if delay else 20
        put(dirs + 8 * kind, 0x2000)
        put(dirs + 8 * kind + 4, width * 3)
        string(0x2200, 'KERNEL32.dll')
        string(0x2240, 'OpenVR_API.DLL')
        for i, name_rva in enumerate((0x2200, 0x2240)):
            descriptor = 0x2000 + i * width
            if delay:
                values = (1, name_rva, 0, 0x2400, 0x2400, 0, 0, 0)
            else:
                values = (0 if fallback else 0x2400, 0, 0, name_rva, 0x2400)
            for j, value in enumerate(values):
                rv(descriptor + j * 4, value)
        for i, name in enumerate(sorted(GAME)):
            rv(0x2400 + i * 8, 0x2600 + i * 128, 8)
            string(0x2602 + i * 128, name)
        return d, put, rv

    for data in (b'', b'MZ' + bytes(62)):
        refused(lambda: Image(data))
    for delay, fallback in ((False, False), (False, True), (True, False)):
        d, put, rv = fixture(delay, fallback)
        check(_native_exports(Image(d)) == EXPECTED)
        rows = _validate_rows(_game_imports(Image(d)))
        check(len(rows) == 5 and all(r['kind'] == ('delay' if delay else 'import') for r in rows))
    # Mutations isolate boundaries and the exact export/import contract.
    mutations = (
        (lambda p, r: p(0x84, 0x14c, 2), lambda im: im),
        (lambda p, r: p(0x94, 100, 2), lambda im: im),
        (lambda p, r: p(0x98 + 108, 17), lambda im: im),
        (lambda p, r: r(0x1000 + 20, 17), _native_exports),
        (lambda p, r: r(0x10c2, 0, 2), _native_exports),
        (lambda p, r: r(0x1040, 0), _native_exports),
        (lambda p, r: r(0x1040, 0x1100), _native_exports),
        (lambda p, r: r(0x1080, 0x4fff), _native_exports),
        (lambda p, r: p(0x98 + 112 + 12, 40), _game_imports),
        (lambda p, r: r(0x2014, 0x4ff8), _game_imports),
        (lambda p, r: r(0x2400, 1 << 63 | 9, 8), lambda im: _validate_rows(_game_imports(im))),
        (lambda p, r: r(0x2400, 0, 8), lambda im: _validate_rows(_game_imports(im))),
        (lambda p, r: r(0x2400, 0x100000000, 8), _game_imports),
    )
    for mutation, parse in mutations:
        d, put, rv = fixture()
        d[-1] = 0x41  # no terminator at the last mapped byte
        rv(0x4ff8, 0x2600, 8)
        mutation(put, rv)
        refused(lambda: parse(Image(d)))
    d, put, rv = fixture(True)
    rv(0x2000, 0)
    refused(lambda: _game_imports(Image(d)))
    print('openxr_pe: %d checks, 0 failures' % checks)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game')
    ap.add_argument('--native')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.native:
        ap.error('--native is required; --game additionally checks Frontier imports')
    try:
        result = {'exports': native_exports(args.native)}
        if args.game:
            result['imports'] = validate_frontier_imports(args.game, args.native)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, PEError) as exc:
        print('openxr_pe: ' + str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
