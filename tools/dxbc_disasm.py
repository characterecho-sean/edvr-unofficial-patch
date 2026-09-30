#!/usr/bin/env python3
r"""Disassemble a DXBC shader from the shader dump: python tools/dxbc_disasm.py file.dxbc [out.txt] [--dry-run]

The dump (advanced.glare_shader_dump and its kin) writes each shader the game
creates as edvr_logs\shaders\{vs,ps}_HASH.dxbc. This calls d3dcompiler_47's
D3DDisassemble on one and prints the listing, which is how a family's input
layout, samplers and alpha are read before a coverage shader is written for
it (ui_depth.cpp's kHudDepthHlsl was written from ps 8DEF46452FA459F5's).
Windows only; needs no build.

With out.txt the listing goes to that file; --dry-run says where and how long
and writes nothing. `python tools/dxbc_disasm.py --self-test` compiles a
one-line shader with d3dcompiler_47 and disassembles it, checking the
wrapper (the blob's vtable slots, the numbering flag, the failure path).
"""
import ctypes
import sys
from ctypes import POINTER, WINFUNCTYPE, c_char_p, c_size_t, c_uint, c_void_p, cast, string_at


def blob_bytes(blob):
    # ID3DBlob: IUnknown (3 slots), GetBufferPointer (3), GetBufferSize (4).
    vtbl = cast(blob, POINTER(c_void_p))[0]
    fns = cast(vtbl, POINTER(c_void_p))
    get_ptr = WINFUNCTYPE(c_void_p, c_void_p)(fns[3])
    get_size = WINFUNCTYPE(c_size_t, c_void_p)(fns[4])
    return string_at(get_ptr(blob), get_size(blob))


def disassemble(data):
    """(hr, listing) for one DXBC blob; the listing is None when D3DDisassemble failed."""
    d3d = ctypes.WinDLL('d3dcompiler_47')
    out = c_void_p()
    # D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING = 4
    hr = d3d.D3DDisassemble(data, c_size_t(len(data)), c_uint(4), c_char_p(None), ctypes.byref(out))
    if hr != 0 or not out:
        return hr, None
    return hr, blob_bytes(out).decode('utf-8', errors='replace')


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if '--self-test' in argv:
        return self_test()
    dry_run = '--dry-run' in argv
    argv = [a for a in argv if a != '--dry-run']
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    data = open(path, 'rb').read()
    hr, text = disassemble(data)
    if text is None:
        print('D3DDisassemble failed: hr=0x%08X' % (hr & 0xFFFFFFFF))
        return 1
    if len(argv) > 1:
        if not dry_run:
            open(argv[1], 'w', encoding='utf-8').write(text)
        print('%s -> %s (%d lines%s)' % (path, argv[1], text.count('\n'),
                                         '; dry run, not written' if dry_run else ''))
    else:
        sys.stdout.write(text)
    return 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def _compile(source, target):
    """DXBC bytes for a shader source, by d3dcompiler_47; None if it did not compile."""
    d3d = ctypes.WinDLL('d3dcompiler_47')
    code = c_void_p()
    errs = c_void_p()
    hr = d3d.D3DCompile(source, len(source), b'fixture', None, None, b'main', target,
                        0, 0, ctypes.byref(code), ctypes.byref(errs))
    return blob_bytes(code) if hr == 0 and code else None


def self_test():
    """Compile a one-line vertex shader and read it back through the wrapper."""
    import contextlib
    import io
    import os
    import re
    import shutil
    import tempfile

    failures = []

    def expect(name, condition, what=''):
        if not condition:
            failures.append('%s: %s' % (name, what))

    def run(*argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = main(list(argv))
        return code, out.getvalue()

    dxbc = _compile(b'float4 main(float4 p : POSITION) : SV_Position { return p; }\n', b'vs_5_0')
    expect('fixture', dxbc is not None and dxbc[:4] == b'DXBC', 'the fixture shader did not compile')
    base = tempfile.mkdtemp(prefix='edvr-dxbc-disasm-')
    try:
        if dxbc:
            # The listing: the profile, an instruction, and the numbering the
            # docs promise (flag 4), which is what lets a line be quoted.
            hr, text = disassemble(dxbc)
            expect('disassemble', hr == 0 and text is not None, 'hr=0x%08X' % (hr & 0xFFFFFFFF))
            if text is not None:
                expect('disassemble', 'vs_5_0' in text and 'ret' in text and 'mov o0.xyzw, v0.xyzw' in text, text)
                expect('numbering', re.search(r'^\s*0: ', text, re.M) is not None, 'no numbered lines:\n' + text)

            # The command line: to stdout, to a file, and a dry run that writes nothing.
            path = os.path.join(base, 'ps_FIXTURE.dxbc')
            with open(path, 'wb') as f:
                f.write(dxbc)
            code, said = run(path)
            expect('stdout', code == 0 and 'vs_5_0' in said, '%r %r' % (code, said))
            listing = os.path.join(base, 'listing.txt')
            code, said = run(path, listing)
            expect('to-file', code == 0 and said.startswith('%s -> %s (' % (path, listing)) and 'dry run' not in said,
                   '%r %r' % (code, said))
            expect('to-file', os.path.isfile(listing) and 'vs_5_0' in open(listing, encoding='utf-8').read(),
                   'the listing file')
            os.remove(listing)
            code, said = run(path, listing, '--dry-run')
            expect('dry-run', code == 0 and 'dry run, not written' in said and not os.path.exists(listing),
                   '%r %r exists=%r' % (code, said, os.path.exists(listing)))

        # Not a shader: a failure code, said plainly, not a traceback.
        hr, text = disassemble(b'this is not a shader' * 4)
        expect('garbage', hr != 0 and text is None, 'hr=0x%08X %r' % (hr & 0xFFFFFFFF, text))
        junk = os.path.join(base, 'junk.dxbc')
        with open(junk, 'wb') as f:
            f.write(b'this is not a shader' * 4)
        code, said = run(junk)
        expect('garbage-cli', code == 1 and said.startswith('D3DDisassemble failed: hr=0x'), '%r %r' % (code, said))

        # No arguments: the usage, and a distinct exit code.
        code, said = run()
        expect('usage', code == 2 and 'Disassemble a DXBC shader' in said, '%r %r' % (code, said[:60]))
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print('dxbc_disasm: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('dxbc_disasm: self-test OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
