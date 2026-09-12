#!/usr/bin/env python3
r"""Disassemble a DXBC shader from the shader dump: python tools/dxbc_disasm.py file.dxbc [out.txt]

The dump (advanced.glare_shader_dump and its kin) writes each shader the game
creates as edvr_logs\shaders\{vs,ps}_HASH.dxbc. This calls d3dcompiler_47's
D3DDisassemble on one and prints the listing, which is how a family's input
layout, samplers and alpha are read before a coverage shader is written for
it (ui_depth.cpp's kHudDepthHlsl was written from ps 8DEF46452FA459F5's).
Windows only; needs no build.
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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    data = open(path, 'rb').read()
    d3d = ctypes.WinDLL('d3dcompiler_47')
    out = c_void_p()
    # D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING = 4
    hr = d3d.D3DDisassemble(data, c_size_t(len(data)), c_uint(4), c_char_p(None), ctypes.byref(out))
    if hr != 0 or not out:
        print('D3DDisassemble failed: hr=0x%08X' % (hr & 0xFFFFFFFF))
        return 1
    text = blob_bytes(out).decode('utf-8', errors='replace')
    if len(sys.argv) > 2:
        open(sys.argv[2], 'w', encoding='utf-8').write(text)
        print('%s -> %s (%d lines)' % (path, sys.argv[2], text.count('\n')))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
