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
    python tools/compile_variants.py --self-test

With no arguments it checks the sun-glare vertex shader and its three
variants, which is what it was written for. Give it a header path to check a
different one, and define names after that to replace the variant list; the
unmodified compile is always done first.

--self-test compiles fixture headers in the temp folder -- a shader that
builds in every variant, one that fails only under a define, one with a
syntax error -- and reads the argument forms, so the wrapper around
d3dcompiler_47 is itself checked. It writes nothing but that temp folder.
"""
import ctypes
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SRC = os.path.join(REPO, "src", "d3d11", "sunglare_vs.h")
DEFAULT_DEFINES = []


class Macro(ctypes.Structure):
    _fields_ = [("n", ctypes.c_char_p), ("d", ctypes.c_char_p)]


def parse_args(argv):
    """(header, [define, ...], target) from the command line.

    --target=cs_5_0 (or ps_5_0) desk-compiles a compute or pixel shader the
    same way; the default is the vertex target the tool was written for."""
    args = [a for a in argv if not a.startswith("--target=")]
    target = next((a[len("--target="):] for a in argv if a.startswith("--target=")), "vs_5_0")
    src = args[0] if len(args) > 0 else DEFAULT_SRC
    defines = args[1:] if len(args) > 1 else DEFAULT_DEFINES
    return src, defines, target


def extract_hlsl(text):
    """The bytes between R"HLSL( and )HLSL", or None when there is no literal."""
    match = re.search(r'R"HLSL\((.*)\)HLSL"', text, re.S)
    return match.group(1).encode() if match else None


def compile_variant(d3d, hlsl, defname, target):
    """Compile once, with `defname` defined to 1 (None: unmodified).
    Returns (ok, hr, compiler's error text)."""
    code = ctypes.c_void_p()
    errs = ctypes.c_void_p()
    if defname:
        arr = (Macro * 2)(Macro(defname.encode(), b"1"), Macro(None, None))
        pdef = arr
    else:
        pdef = None
    hr = d3d.D3DCompile(hlsl, len(hlsl), b"vs", pdef, None, b"main",
                        target.encode(), 0, 0, ctypes.byref(code), ctypes.byref(errs))
    ok = hr == 0 and code.value
    text = ""
    if errs.value:
        vt = ctypes.cast(errs, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
        getp = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
        gets = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
        text = ctypes.string_at(getp(errs), gets(errs)).decode("utf-8", "replace")[:400]
    return bool(ok), hr, text


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if "--self-test" in argv:
        return self_test()
    src, defines, target = parse_args(argv)

    text = open(src, encoding="utf-8").read()
    hlsl = extract_hlsl(text)
    if hlsl is None:
        sys.exit(f"no R\"HLSL(...)HLSL\" literal in {src}")
    d3d = ctypes.WinDLL("d3dcompiler_47.dll")

    print(f"{os.path.relpath(src, REPO)}: {len(hlsl)} bytes of HLSL")
    failed = 0
    for d in [None] + defines:
        ok, hr, errors = compile_variant(d3d, hlsl, d, target)
        print(f"{d or 'NORMAL'}: hr=0x{hr & 0xFFFFFFFF:08X} {'OK' if ok else 'FAIL'}")
        if errors:
            print(errors)
        if not ok:
            failed += 1
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def self_test():
    """The argument forms, the literal extraction and the compile wrapper,
    against fixture headers in the temp folder."""
    import contextlib
    import io
    import shutil
    import tempfile

    failures = []

    def expect(name, condition, what=""):
        if not condition:
            failures.append("%s: %s" % (name, what))

    # The command line.
    expect("args-default", parse_args([]) == (DEFAULT_SRC, DEFAULT_DEFINES, "vs_5_0"), repr(parse_args([])))
    expect("args-header-only", parse_args(["x.h"]) == ("x.h", DEFAULT_DEFINES, "vs_5_0"),
           "a header alone keeps the default variants: %r" % (parse_args(["x.h"]),))
    expect("args-defines", parse_args(["--target=cs_5_0", "x.h", "A", "B"]) == ("x.h", ["A", "B"], "cs_5_0"),
           repr(parse_args(["--target=cs_5_0", "x.h", "A", "B"])))
    expect("args-target-anywhere", parse_args(["x.h", "A", "--target=ps_5_0"]) == ("x.h", ["A"], "ps_5_0"),
           repr(parse_args(["x.h", "A", "--target=ps_5_0"])))

    # The literal: multi-line, with parentheses of its own, and absent.
    body = "float4 main(float4 p : POSITION) : SV_Position {\n    return (p);\n}\n"
    expect("extract", extract_hlsl('static const char* k = R"HLSL(%s)HLSL";\n' % body) == body.encode(),
           "the literal was not read back whole")
    expect("extract-none", extract_hlsl('static const char* k = "float4 main()";\n') is None,
           "a header with no literal")

    base = tempfile.mkdtemp(prefix="edvr-compile-variants-")

    def header(name, hlsl):
        path = os.path.join(base, name + ".h")
        with open(path, "w", encoding="utf-8") as f:
            f.write('#pragma once\nstatic const char* kSrc = R"HLSL(%s)HLSL";\n' % hlsl)
        return path

    def run(*argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            try:
                code = main(list(argv))
            except SystemExit as exc:
                code = "SystemExit: %s" % exc
        return code, out.getvalue()

    try:
        d3d = ctypes.WinDLL("d3dcompiler_47.dll")

        # The wrapper itself: a good shader compiles and says nothing, a bad one
        # fails with the compiler's own code, and a define is honoured.
        good = (body.replace("return (p);", "\n#ifdef FLIP\n    return -p;\n#else\n    return p;\n#endif\n")).encode()
        ok, hr, text = compile_variant(d3d, good, None, "vs_5_0")
        expect("compile-good", ok and hr == 0 and text == "", "%r %r %r" % (ok, hr, text))
        ok, hr, text = compile_variant(d3d, good, "FLIP", "vs_5_0")
        expect("compile-define", ok and hr == 0, "%r %r %r" % (ok, hr, text))
        bad = b"float4 main(float4 p : POSITION) : SV_Position { return q; }\n"
        ok, hr, text = compile_variant(d3d, bad, None, "vs_5_0")
        expect("compile-bad", not ok and hr & 0xFFFFFFFF != 0 and "X3004" in text and "'q'" in text,
               "%r %r %r" % (ok, hr, text))
        # The target is honoured: a vertex shader is not a compute shader.
        ok, hr, text = compile_variant(d3d, good, None, "cs_5_0")
        expect("compile-target", not ok, "a vertex shader compiled as cs_5_0")

        # End to end, through the command line the docs show.
        conditional = ("float4 main(float4 p : POSITION) : SV_Position {\n"
                       "#ifdef BROKEN\n    return q;\n#else\n    return p;\n#endif\n}\n")
        code, said = run(header("good", body), "FLIP", "OTHER")
        expect("run-good", code == 0 and said.count(": hr=0x00000000 OK") == 3
               and "NORMAL: hr=0x00000000 OK" in said and "FLIP: hr=0x00000000 OK" in said
               and "OTHER: hr=0x00000000 OK" in said and " bytes of HLSL" in said, "%r\n%s" % (code, said))
        code, said = run(header("broken-variant", conditional), "BROKEN", "FINE")
        expect("run-one-variant-fails", code == 1 and "BROKEN: hr=0x" in said and "FAIL" in said
               and "NORMAL: hr=0x00000000 OK" in said and "FINE: hr=0x00000000 OK" in said, "%r\n%s" % (code, said))
        code, said = run(header("broken", bad.decode()))
        expect("run-syntax-error", code == 1 and "NORMAL: hr=0x" in said and "FAIL" in said
               and "undeclared identifier" in said, "%r\n%s" % (code, said))
        code, said = run("--target=ps_5_0", header("pixel", "float4 main(float4 p : SV_Position) : SV_Target { return p; }\n"))
        expect("run-target", code == 0 and "NORMAL: hr=0x00000000 OK" in said, "%r\n%s" % (code, said))
        path = os.path.join(base, "none.h")
        with open(path, "w", encoding="utf-8") as f:
            f.write("// no literal here\n")
        code, said = run(path)
        expect("run-no-literal", str(code).startswith("SystemExit: no R\"HLSL(...)HLSL\" literal in"), "%r" % (code,))
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print("compile_variants: self-test FAILED")
        for f in failures:
            print("  " + f.replace("\n", "\n    "))
        return 1
    print("compile_variants: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
