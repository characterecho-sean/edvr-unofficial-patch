#!/usr/bin/env python3
"""Desk-compile a replacement shader's HLSL, once per variant, before it ships.

WHY THIS EXISTS. The replacement shaders live as R"HLSL(...)HLSL" literals in
headers next to the fixes that use them (src/d3d11/sunglare_vs.h,
particle_vs.h). They are compiled at runtime by shaderSwapCompileVs, on the
render thread, at a matched draw -- which is the worst possible place to
discover a typo. src/d3d11/shader_swap.h puts it plainly: the game is never
the compiler's first audience. This is that first audience.

It compiles the literal once per preprocessor variant, because a fix whose
variants are selected by #define has as many programs as it has defines and
only the one you happened to run gets checked otherwise. Failures print the
compiler's own error text.

Usage:
    python tools/compile_variants.py [--target=cs_5_0] [header] [define ...]

With no arguments it checks the sun-glare vertex shader and its three
variants, which is what it was written for. Give it a header path to check a
different one, and define names after that to replace the variant list; the
unmodified compile is always done first.
"""
import ctypes
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SRC = os.path.join(REPO, "src", "d3d11", "sunglare_vs.h")
DEFAULT_DEFINES = ["NOGATE", "ALLWORLD", "ALLFLAT"]

# --target=cs_5_0 (or ps_5_0) desk-compiles a compute or pixel shader the
# same way; the default is the vertex target the tool was written for. The
# supersample resolve's filter (src/d3d11/supersample_pass.cpp) is the first
# compute shader checked here.
ARGS = [a for a in sys.argv[1:] if not a.startswith("--target=")]
TARGET = next((a[len("--target="):] for a in sys.argv[1:]
               if a.startswith("--target=")), "vs_5_0").encode()
SRC = ARGS[0] if len(ARGS) > 0 else DEFAULT_SRC
DEFINES = ARGS[1:] if len(ARGS) > 1 else DEFAULT_DEFINES

text = open(SRC, encoding="utf-8").read()
match = re.search(r'R"HLSL\((.*)\)HLSL"', text, re.S)
if not match:
    sys.exit(f"no R\"HLSL(...)HLSL\" literal in {SRC}")
hlsl = match.group(1).encode()
d3d = ctypes.WinDLL("d3dcompiler_47.dll")


class Macro(ctypes.Structure):
    _fields_ = [("n", ctypes.c_char_p), ("d", ctypes.c_char_p)]


def compile_variant(defname):
    code = ctypes.c_void_p()
    errs = ctypes.c_void_p()
    if defname:
        arr = (Macro * 2)(Macro(defname.encode(), b"1"), Macro(None, None))
        pdef = arr
    else:
        pdef = None
    hr = d3d.D3DCompile(hlsl, len(hlsl), b"vs", pdef, None, b"main",
                        TARGET, 0, 0, ctypes.byref(code), ctypes.byref(errs))
    ok = hr == 0 and code.value
    print(f"{defname or 'NORMAL'}: hr=0x{hr & 0xFFFFFFFF:08X} {'OK' if ok else 'FAIL'}")
    if errs.value:
        vt = ctypes.cast(errs, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
        getp = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
        gets = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
        print(ctypes.string_at(getp(errs), gets(errs)).decode("utf-8", "replace")[:400])
    return ok


print(f"{os.path.relpath(SRC, REPO)}: {len(hlsl)} bytes of HLSL")
failed = 0
for d in [None] + DEFINES:
    if not compile_variant(d):
        failed += 1
sys.exit(1 if failed else 0)
