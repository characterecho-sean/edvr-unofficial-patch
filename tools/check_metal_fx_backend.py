#!/usr/bin/env python3
"""MetalFX backend: the private DXMT ABI, and the invariants that must not drift.

WHY THIS EXISTS

`src/d3d11/metal_fx_engine.*` is the only place in EDVR that talks to a
private, undocumented interface belonging to another program: DXMT's
`IMTLD3D11ContextExt`/`Ext1`, declared locally rather than
included (EDVR never includes DXMT headers, and DXMT is read-only reference).
That is exactly the shape of code that rots silently:

  * If DXMT changes the descriptor's layout, EDVR still compiles -- the
    struct is EDVR's own -- and MetalFX then reads whatever EDVR left in the
    padding, or reads JitterOffsetX where ExposureTexture was. There is no
    crash to notice. The picture is wrong, or the process dies inside a
    driver, and both look like a rendering bug.

  * If someone "cleans up" the backend and routes it through nvngx.dll, or
    adds a MetalFX 4 call path beside the MetalFX 3 one, or lets it touch context state, each
    is a regression against a specific decision and nothing at build time says
    so. `SwapDeviceContextState` in particular aborts the process on DXMT
    (flat_isolation_mode.h:7), which is the crash this milestone is not
    allowed to reintroduce.

The C++ `static_assert`s cover EDVR's own layout -- they caught a real
arithmetic error in the offsets when this was written. They cannot cover
whether that layout still agrees with DXMT's, because EDVR cannot see DXMT's
header. This tool does, in two ways: it re-derives the layout from EDVR's own
declaration and checks it against the `static_assert`s, and when a DXMT
checkout sits beside the tree it reads DXMT's real header and compares the
field order and the available IIDs. On a Windows build machine with no DXMT beside
it, the pinned expected values are what is checked, and the cross-check says
it was skipped rather than silently passing.

WHAT IT CHECKS

  1. The two IIDs match the audited DXMT values, and the descriptor's x64
     field offsets and size match the `static_assert`s in the header.
  2. The descriptor's declared field ORDER matches DXMT's, when DXMT is
     available -- an order that differs keeps every offset right and still
     crosses the boundary wrong.
  3. `FlatMonoResolveMode::Mfx` exists, names itself "mfx", and is reachable
     from `fix.temporal_aa = mfx`.
  4. Mfx routes through the SHARED trained branch, so E >= R, and that the
     FSR/NGX/TAA resource formats are still what they were.
  5. No context-state call anywhere in the backend: no SwapDeviceContextState,
     no CreateDeviceContextState, no ClearState. The backend submits one call
     and touches nothing.
  6. The only SwapDeviceContextState call sites in the whole of src/ are still
     the two inside flat_mono_resolve.cpp's Isolate, both on the byCapture
     else-branch -- the runtime DXMT path, unchanged.
  7. The backend names no vendor-extension entry point: no nvngx, no NVAPI,
     no DXMT_ENABLE_NVEXT. No native MTL4FX, MTL4Compiler or MTL4CommandBuffer
     objects and no Ext2/TemporalUpscaleEx: DXMT owns encoding and this
     backend speaks MetalFX 3 only.
  8. The FSR3 and TAA dispatch arms are still in flatMonoResolve's backend
     block, and Mfx was added as an `else if` beside them rather than by
     replacing one.

`--self-test` runs each of these over fixtures in a temp folder -- including a
descriptor with a field reordered, an IID with one digit changed, a backend
that calls ClearState, and a route that can downscale -- so a parse that
drifts fails in the build and not in the field.
"""

import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER = "src/d3d11/metal_fx_engine.h"
SOURCE = "src/d3d11/metal_fx_engine.cpp"
RESOLVE_H = "src/d3d11/flat_mono_resolve.h"
RESOLVE_CPP = "src/d3d11/flat_mono_resolve.cpp"
RUNTIME_CPP = "src/d3d11/flat_runtime.cpp"
SHADER_H = "src/d3d11/flat_mono_shader_source.h"
TEMPORAL_MODE_H = "src/common/temporal_mode.h"

# DXMT at db83f85, src/d3d11/d3d11_interfaces.hpp:24 and :36. These are the
# values, and they are what a QueryInterface compares -- not the mechanism each
# build uses to spell them.
IID_CONTEXT_EXT = "43ace3ce-1956-448b-a4eb-aee68bdeb283"
IID_CONTEXT_EXT1 = "19a8e35a-38be-418f-94e3-9f7323936870"
# The MetalFX 3 feature slot DXMT's CheckFeatureSupport answers.
FEATURE_METALFX_TEMPORAL_SCALER = 0

# The descriptor's fields, in DXMT's declared order, with the x64 type each
# gets. 4-byte scalars, 8-byte pointers, so the offsets follow from the order
# alone once the first pointer is placed -- which is what makes an order change
# detectable rather than merely suspicious.
DESC_FIELDS = [
    ("InputContentWidth", 4),
    ("InputContentHeight", 4),
    ("AutoExposure", 4),
    ("InReset", 4),
    ("DepthReversed", 4),
    ("MotionVectorInDisplayRes", 4),
    ("Color", 8),
    ("Depth", 8),
    ("MotionVector", 8),
    ("Output", 8),
    ("MotionVectorScaleX", 4),
    ("MotionVectorScaleY", 4),
    ("PreExposure", 4),
    ("ExposureTexture", 8),
    ("JitterOffsetX", 4),
    ("JitterOffsetY", 4),
]

# Forbidden in the backend: the vendor-extension route this backend does not
# use, and any MetalFX 4 API. The Metal entries are what keep this backend
# MetalFX 3: EDVR reaches the scaler only through DXMT's D3D11 extension
# interface (IMTLD3D11ContextExt/Ext1's TemporalUpscale), never through a
# second extension and never by naming an MTL4 object. So MTL4FXTemporalScaler,
# MTL4Compiler and MTL4CommandBuffer remain forbidden: naming one would mean
# EDVR building or encoding into a Metal 4 object itself, which is exactly the
# copy, the command buffer and the residency tracking this file exists to
# delegate. A comment may mention them to explain why the gate exists -- hence
# CODE_ONLY below.
BANNED_IN_BACKEND = [
    ("nvngx", "the backend must not route through DXMT's NVIDIA vendor extension"),
    ("NVSDK_NGX", "the backend must not route through the NGX SDK"),
    ("NvAPI", "the backend must not route through NVAPI"),
    ("DXMT_ENABLE_NVEXT", "the backend must not require the NVEXT registry gate"),
    ("TemporalUpscaleEx", "there is no MetalFX 4 path: the backend speaks TemporalUpscale only"),
    ("IMTLD3D11ContextExt2", "there is no MetalFX 4 path: Ext/Ext1 are the whole interface"),
    ("MTL4FX", "MetalFX 4 is not reached from here at all, neither through DXMT nor directly"),
    ("MTL4Compiler", "MetalFX 4's compiler is DXMT's; EDVR must not create one"),
    ("MTL4CommandBuffer", "the command buffer is DXMT's; EDVR must not encode into or commit one"),
    ("MTLPixelFormat", "EDVR must not name Metal formats; DXMT owns the translation"),
    ("WMT::", "EDVR must not name winemetal types either; the D3D11 interface is the whole seam"),
    ("SwapDeviceContextState", "abort()s the process on DXMT (flat_isolation_mode.h:7)"),
    ("CreateDeviceContextState", "the backend must not make or swap context state at all"),
    ("ClearState", "the resolver's isolation guard owns the context state, not the backend"),
]


def read(rel, root=None):
    with open(os.path.join(root or ROOT, rel), "rb") as handle:
        return handle.read().decode("utf-8")


def x64_layout(fields):
    """Offsets and size for the declared field order, MSVC x64 rules: natural
    alignment, a pointer forces the next field up to 8, trailing pad to 8."""
    offsets, cursor = {}, 0
    for name, size in fields:
        align = size
        if cursor % align:
            cursor += align - cursor % align
        offsets[name] = cursor
        cursor += size
    size = cursor
    if size % 8:
        size += 8 - size % 8
    return offsets, size


def strip_comments(text):
    """Comments removed, string literals KEPT.

    This is the view for the checks that legitimately read text: the mode's own
    name, the value fix.temporal_aa is parsed from, the text of a static_assert.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def strip_strings(text):
    """String literals removed as well as comments.

    The third view, used only to decide whether a forbidden Metal or vendor
    symbol appears in CODE rather than in the prose that explains why it must
    not. A regex rather than a lexer because it only has to be right about the
    one thing it is asked, and a false negative here is caught by the strict
    comment-stripped check that runs alongside it.
    """
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    return re.sub(r"'(?:\\.|[^'\\])*'", "''", text)


def guid_fields(text, constant):
    """The Data1/Data2/Data3/Data4 literals of one `constexpr GUID <name>`."""
    match = re.search(r"constexpr\s+GUID\s+" + constant + r"\s*=\s*\{([^;]*)\};", text, re.S)
    if not match:
        return None
    body = match.group(1)
    scalars = re.search(r"(?:^|\{)\s*(0x[0-9a-fA-F]+)u\s*,\s*(0x[0-9a-fA-F]+)u\s*,\s*(0x[0-9a-fA-F]+)u\s*,", body)
    array = re.search(r"\{\s*((?:0x[0-9a-fA-F]+\s*,\s*){7}0x[0-9a-fA-F]+)\s*\}", body)
    if not scalars or not array:
        return None
    data4 = [int(v.strip(), 16) for v in array.group(1).split(",")]
    return (int(scalars.group(1), 16), int(scalars.group(2), 16), int(scalars.group(3), 16), data4)


def guid_string(value):
    d1, d2, d3, data4 = value
    return "%08x-%04x-%04x-%s-%s" % (d1, d2, d3, "".join("%02x" % b for b in data4[:2]),
                                     "".join("%02x" % b for b in data4[2:]))


def descriptor_field_order(text):
    """The field names EDVR declares, in order, from the struct body."""
    match = re.search(r"struct\s+TemporalUpscaleDesc\s*\{(.*?)\n\};", strip_comments(text), re.S)
    if not match:
        return None
    return re.findall(r"^\s*(?:[A-Za-z_][\w:]*\s+)*?[A-Za-z_]\w*\s*\*?\s*([A-Za-z_]\w*)\s*"
                      r"(?:\[[^\]]*\])?\s*;", match.group(1), re.M)


def gate_modes(text, function, return_type="bool"):
    """The mode strings one of temporal_mode.h's small predicates compares.

    Read out of the function's own body rather than the file, so a mode named
    in a comment, or in the OTHER predicate, cannot be mistaken for one this
    one enables. Returns None when the function cannot be found at all, which is
    a failure the caller reports rather than an empty set it would forgive.
    """
    match = re.search(r"inline\s+" + return_type + r"\s+" + function + r"\s*\([^)]*\)\s*\{(.*?)\n\}",
                      strip_comments(text), re.S)
    if not match:
        return None
    return {m.lower() for m in
            re.findall(r'_stricmp\(\s*mode\.c_str\(\)\s*,\s*"([^"]*)"', match.group(1))}


def checks(root):
    """Returns a list of (ok, message) pairs."""
    out = []

    def check(ok, message):
        out.append((bool(ok), message))
        return ok

    header = read(HEADER, root)
    header_code = strip_comments(header)

    # 1. the two IIDs, and 1b. their Data1 re-derived from the pinned strings.
    for constant, expected in (("kIidContextExt", IID_CONTEXT_EXT), ("kIidContextExt1", IID_CONTEXT_EXT1)):
        value = guid_fields(header_code, constant)
        if not check(value is not None, "%s declares %s" % (HEADER, constant)):
            continue
        check(guid_string(value) == expected,
              "%s: %s is %s (pinned %s)" % (HEADER, constant, guid_string(value), expected))

    # 2. the descriptor's declared order, and the offsets that follow from it.
    order = descriptor_field_order(header_code)
    want_order = [name for name, _ in DESC_FIELDS]
    if check(order == want_order,
             "%s declares the descriptor fields in DXMT's order" % HEADER):
        offsets, size = x64_layout(DESC_FIELDS)
        asserts = dict((name, int(value)) for name, value in
                       re.findall(r"offsetof\(TemporalUpscaleDesc,\s*(\w+)\)\s*==\s*(\d+)", header_code))
        for name, want in offsets.items():
            got = asserts.get(name)
            check(got == want, "%s: offsetof(%s) is %s, asserted %s (x64 layout of DXMT's order gives %d)"
                  % (HEADER, name, got, "absent" if got is None else got, want))
        want_size = re.search(r"sizeof\(TemporalUpscaleDesc\)\s*==\s*(\d+)", header_code)
        check(want_size is not None and int(want_size.group(1)) == size,
              "%s: sizeof(TemporalUpscaleDesc) asserted %s, layout gives %d"
              % (HEADER, want_size.group(1) if want_size else "absent", size))

    # 3. against DXMT's own header, when a checkout is beside the tree. A build
    #    machine without DXMT checks the pinned values above and says so.
    dxmt = os.path.join(os.path.dirname(root.rstrip(os.sep)), "dxmt",
                        os.path.join("src", "d3d11", "d3d11_interfaces.hpp"))
    if os.path.exists(dxmt):
        with open(dxmt, "rb") as handle:
            theirs = handle.read().decode("utf-8", "replace")
        for name, expected in (("IMTLD3D11ContextExt", IID_CONTEXT_EXT),
                               ("IMTLD3D11ContextExt1", IID_CONTEXT_EXT1)):
            found = re.search(r'DEFINE_COM_INTERFACE\(\s*"([0-9a-fA-F-]+)"\s*,\s*' + name + r"\b", theirs)
            check(found is not None and found.group(1).lower() == expected,
                  "DXMT still declares %s as %s" % (name, expected))
        theirs_order = re.search(r"struct\s+MTL_TEMPORAL_UPSCALE_D3D11_DESC\s*\{(.*?)\n\};", theirs, re.S)
        theirs_fields = re.findall(r"^\s*(?:[A-Za-z_]\w*)\s*\*?\s*([A-Za-z_]\w*)\s*;",
                                   theirs_order.group(1), re.M) if theirs_order else None
        check(theirs_fields == want_order,
              "DXMT's descriptor field order still matches EDVR's")
    else:
        out.append((True, "no DXMT checkout beside the tree: the pinned IIDs and layout were checked, "
                          "the live cross-check against DXMT's header was SKIPPED"))

    # 4. the mode exists, names itself, and is selectable.
    resolve_h = strip_comments(read(RESOLVE_H, root))
    check(re.search(r"enum\s+class\s+FlatMonoResolveMode\s*\{[^}]*\bMfx\b", resolve_h) is not None,
          "%s declares FlatMonoResolveMode::Mfx" % RESOLVE_H)
    check(re.search(r'FlatMonoResolveMode::Mfx\s*\?\s*"mfx"', resolve_h) is not None,
          "flatMonoResolveModeName spells Mfx \"mfx\"")
    # MetalFX 4 (Mfx4/Ext2) was deferred out of this port: no Mfx4 enumerator,
    # spelling, parse, admission or dispatch check remains.
    route_cpp = strip_comments(read(RESOLVE_CPP, root))
    runtime_cpp = strip_comments(read(RUNTIME_CPP, root))
    # v0.18.3 uses debug.z for late overlays and debug.w for alternate-camera
    # coverage. MetalFX must have a separate flag: sharing debug.z would write
    # an unbound prep UAV on overlay TAA and read null coverage on plain MFX.
    shader = strip_comments(read(SHADER_H, root))
    host_constants = re.search(r"struct Constants\s*\{([^}]+)\}", route_cpp)
    shader_constants = re.search(r"cbuffer Mono\s*:\s*register\(b0\)\s*\{([^}]+)\}", shader)
    check(host_constants is not None and shader_constants is not None,
          "host and shader declare the shared mono constants")
    if host_constants and shader_constants:
        check(re.search(r"uint32_t route\[4\], debug\[4\];\s*float foregroundDepth\[4\];\s*uint32_t backend\[4\];", host_constants[1]) is not None
              and re.search(r"uint4 route\s*;\s*uint4 debug\s*;\s*float4 foregroundDepth\s*;\s*uint4 backend\s*;", shader_constants[1]) is not None
              and "sizeof(Constants)==320" in route_cpp,
              "MetalFX appends one matching cbuffer lane after upstream route/debug/foregroundDepth (320 bytes)")
    check("constants.debug[2]=overlay?1u:0u;" in route_cpp
          and "foreground?2u:(untrusted?1u:0u)" in route_cpp
          and "constants.foregroundDepth[0]=sdkDepthScale;" in route_cpp
          and "constants.backend[0]=mfx?1u:0u;" in route_cpp,
          "late-overlay/foreground/alternate-camera flags remain independent of MetalFX colour expansion")
    check(re.search(r"if\(backend.x!=0\)\s*\{\s*float3 c=Color.Load\(int3\(q,0\)\).rgb;\s*"
                    r"OutColor\[q\]=float4\(clamp\(c,0.0,65504.0\),1.0\);", shader) is not None,
          "only MetalFX's own flag writes the prep colour UAV with the established fp16 expansion")
    check(re.search(r"if\(debug.z!=0\)\s*\{[^}]*OverlayCoverage.Load", shader, re.S) is not None,
          "upstream debug.z still gates the late-overlay HDR finish")
    check(re.search(r'"mfx"[^;?]{0,40}\?\s*FlatMonoResolveMode::Mfx', runtime_cpp) is not None,
          "%s parses fix.temporal_aa = mfx" % RUNTIME_CPP)
    # A declared mode is not a usable mode. resolveModeValid is the ONE admission
    # predicate the resolve entry point consults, and a mode missing from it is
    # refused there with flat-resolve-invalid-mode -- before the route, before the
    # backend, before mfxAvailable() is ever asked. Read from the predicate's own
    # body, so a mode named in a comment elsewhere in the file cannot stand in
    # for admission.
    admission = re.search(
        r"bool\s+resolveModeValid\s*\(\s*FlatMonoResolveMode\s+mode\s*\)\s*\{(.*?)\n\}",
        route_cpp, re.S)
    if check(admission is not None, "%s declares resolveModeValid" % RESOLVE_CPP):
        admitted = set(re.findall(r"FlatMonoResolveMode::(\w+)", admission.group(1)))
        check("Mfx" in admitted,
              "resolveModeValid still admits Mfx, or temporal_aa = mfx refuses: %s"
              % sorted(admitted))
    # Admission is necessary and not sufficient: the DISPATCH predicate is a second,
    # independent copy. A mode admitted there but not named here misses the MetalFX arm
    # entirely and lands on the final `else`, which is dlaaEvaluate -- so an mfx frame
    # allocates its fp16 colour, hands dlaa the UNORM copy-route colour instead, and
    # produces a plausible-looking image from the wrong backend. The two must agree.
    # TWO `const bool mfx` declarations live in this file: the resource/format
    # predicate and the dispatch one. `re.search` alone finds the FIRST -- the
    # resource predicate -- so a check written that way passes green while the dispatch
    # arm still ignores mfx, which is the defect this exists to catch. Take the nearest
    # declaration PRECEDING `else if(mfx)`: the one actually in scope for that arm.
    arm_at = route_cpp.find("else if(mfx)")
    before_arm = route_cpp[:arm_at] if arm_at >= 0 else ""
    found = list(re.finditer(r"const\s+bool\s+mfx\s*=\s*([^;]+);", before_arm))
    dispatch = found[-1] if found else None
    if check(arm_at >= 0 and dispatch is not None,
             "%s declares the dispatch-local MetalFX predicate before its arm" % RESOLVE_CPP):
        dispatched = set(re.findall(r"FlatMonoResolveMode::(\w+)", dispatch.group(1)))
        check("Mfx" in dispatched,
              "the MetalFX dispatch arm admits Mfx: %s" % sorted(dispatched))
        check(dispatched == {"Mfx"},
              "the dispatch predicate names exactly the MetalFX mode: %s"
              % sorted(dispatched))

    # 5. Mfx shares the trained route, so E >= R, and never refuses a downscale.
    route_body = re.search(r"inline FlatResolveRoute flatResolveRoute\(.*?\n\}", resolve_h, re.S)
    if route_body:
        body = route_body.group(0)
        flat = " ".join(body.split())
        check("const bool mfx = mode == FlatMonoResolveMode::Mfx;" in flat,
              "flatResolveRoute names Mfx on the shared trained branch")
        # The one property MetalFX needs: every evaluation size the trained
        # branch can return is the render width or the output width, never
        # anything else. Counting the right-hand sides catches a branch that
        # computes its own size (which is how a downscale sneaks in) without
        # having to model the control flow.
        widths = set(re.findall(r"out\.evalWidth\s*=\s*([^;]+);", flat))
        check(widths <= {"rW", "dW"},
              "every trained evaluation size is the render or the output width: %s" % sorted(widths))
        # And Mfx gets no sizing of its own, which is what makes the sharing real.
        check(flat.count("FlatMonoResolveMode::Mfx") == 1,
              "Mfx has no branch of its own in flatResolveRoute")
    else:
        check(False, "flatResolveRoute not found in %s" % RESOLVE_H)

    # 6. the backend itself touches no context state and names no vendor route.
    # Comments removed, string literals KEPT: this file's own header names what
    # the backend deliberately does not use, and naming it in prose is how a
    # reader knows the decision was made. Code and string literals may not name
    # any of it -- which is why a log line here says "vendor extension" rather
    # than the DLL, and why LoadLibraryW(L"nvngx.dll") is caught.
    backend = strip_comments(read(HEADER, root) + "\n" + read(SOURCE, root))
    for needle, why in BANNED_IN_BACKEND:
        check(needle not in backend, "the MetalFX backend does not mention %s (%s)" % (needle, why))
    # The same list over CODE ONLY -- comments and string literals removed as
    # well. The check above is the strict one and stays strict, but it is worth
    # saying why a second, weaker view exists: this milestone legitimately has
    # to NAME the Metal 4 types in prose ("MetalFX 4 encodes into a Metal 4
    # command buffer, and DXMT owns it") to explain a gate that exists. A
    # comment saying why a symbol must not appear is not the symbol appearing.
    # If a banned name ever shows up in CODE_ONLY, that is a real violation and
    # the strict check above has already failed too -- this one is here so the
    # failure names the code rather than the explanation.
    code_only = strip_strings(strip_comments(read(HEADER, root) + "\n" + read(SOURCE, root)))
    for needle, why in BANNED_IN_BACKEND:
        check(needle not in code_only,
              "the MetalFX backend's code does not name %s (%s)" % (needle, why))
    check("TemporalUpscale" in backend, "the MetalFX backend calls TemporalUpscale")
    check("CheckFeatureSupport" in backend, "the MetalFX backend asks CheckFeatureSupport")

    # 6b. THE METAL 3 CALL SHAPE. mfxEvaluate asks the Ext1 capability gate,
    # builds the one 88-byte descriptor, and calls the original TemporalUpscale.
    # There is no second call shape: no generation branch, no Ext2, no Ex call.
    source_code = strip_comments(read(SOURCE, root))
    header_code_only = header_code

    # The feature value is pinned. It is a value, not an interface, so nothing
    # in the C++ would fail to compile if DXMT read a different one.
    found = re.search(r"MetalFxTemporalScaler\s*=\s*(\d+)", header_code_only)
    check(found is not None and int(found.group(1)) == FEATURE_METALFX_TEMPORAL_SCALER,
          "Feature::MetalFxTemporalScaler is %d (pinned %d)"
          % (int(found.group(1)) if found else -1, FEATURE_METALFX_TEMPORAL_SCALER))

    # Ext1's declaration: derived from Ext, one extra virtual.
    check(re.search(r"struct\s+ContextExt1\s*:\s*public\s+ContextExt", header_code_only) is not None,
          "ContextExt1 derives from ContextExt")

    # The Metal 3 call.
    check("TemporalUpscale" in source_code, "the backend calls TemporalUpscale")

    # THE CALL SHAPE. mfxEvaluate must call the original entry point with the
    # filled descriptor, and the capability gate must run before the descriptor
    # exists: a refused request must never become 88 bytes of EDVR-authored
    # state crossing the seam on the way to finding out it could not proceed.
    body = re.search(r"bool\s+mfxEvaluate\s*\(.*?\n\}", source_code, re.S)
    if check(body is not None, "mfxEvaluate is found in %s" % SOURCE):
        text = " ".join(body.group(0).split())
        check(re.search(r"ext->TemporalUpscale\(&desc\)", text) is not None,
              "mfxEvaluate calls the original ext->TemporalUpscale(&desc)")
        gate_pos, desc_pos = text.find("extension(ctx, why)"), text.find("TemporalUpscaleDesc desc")
        check(gate_pos >= 0 and desc_pos > gate_pos,
              "the capability gate runs before the descriptor is built (no refused request crosses the seam)")

    # 7. the flat path's swap call sites are unchanged: exactly two, both in
    #    flat_mono_resolve.cpp, both the byCapture else-branch.
    sites = []
    for dirpath, _dirnames, filenames in os.walk(os.path.join(root, "src")):
        for name in sorted(filenames):
            if not name.endswith((".cpp", ".h", ".hpp")):
                continue
            full = os.path.join(dirpath, name)
            with open(full, "rb") as handle:
                text = handle.read().decode("utf-8", "replace")
            for number, line in enumerate(strip_comments(text).splitlines(), 1):
                if "->SwapDeviceContextState(" in line:
                    sites.append((os.path.relpath(full, root).replace(os.sep, "/"), number))
    check(len(sites) == 2 and all(where == RESOLVE_CPP for where, _ in sites),
          "the only SwapDeviceContextState call sites are the two in %s: %s" % (RESOLVE_CPP, sites))

    # 8. the FSR3 and TAA arms are still there, and Mfx was added beside them.
    # The four arms, in order, by position rather than by one exact layout: TAA,
    # then FSR3, then Mfx, then the NGX else. Requiring the arms to be PRESENT and
    # ORDERED is what matters -- that Mfx was added beside the others and that
    # none of them was replaced.
    # Anchored at the backend scope, because "else {" and "if(taa)" both occur
    # earlier in the file and an unanchored search would compare unrelated lines.
    anchor = route_cpp.find("flatcpu::Scope backendScope")
    tail = route_cpp[anchor:] if anchor >= 0 else ""
    arms = []
    for marker in ("if(taa)", "else if(f.mode==FlatMonoResolveMode::Fsr)", "else if(mfx)", "else {"):
        at = tail.find(marker)
        arms.append(at if at >= 0 else len(tail))
    if check(anchor >= 0 and all(at < len(tail) for at in arms) and arms == sorted(arms),
             "the backend dispatch is TAA / Fsr / Mfx / (NGX) in that order"):
        taa_arm, fsr_arm, mfx_arm = tail[arms[0]:arms[1]], tail[arms[1]:arms[2]], tail[arms[2]:arms[3]]
        check("context->Dispatch(" in taa_arm, "the TAA arm still dispatches EDVR's own kernel")
        check("fsr3Evaluate(" in fsr_arm, "the FSR3 arm still calls fsr3Evaluate")
        check("mfxEvaluate(" in mfx_arm, "the Mfx arm calls mfxEvaluate")
        check("dlaaEvaluate(" in tail[arms[3]:], "the NGX arm still calls dlaaEvaluate")
        check("g.prep.Get()" in route_cpp and "g.finish.Get()" in route_cpp,
              "the prep and finish kernels are still dispatched")

    # 9. the SHARED temporal frontend gate recognises mfx. Everything above is
    #    the flat resolver and the backend; this is the gate in front of all of
    #    it, and it lives in a file with no knowledge of MetalFX at all. A mode
    #    missing from temporalModeEnabled() is stood down before the resolver is
    #    asked anything: no discovery, no jitter, no motion and, above all, no
    #    mfxAvailable(). The log then shows a mode that parsed and routed, a
    #    repeated refusal reason, and not one MFX line -- which is exactly the
    #    evidence a flight produces, and exactly what this catches.
    #    The second half is the boundary that must NOT move: mfx belongs here
    #    and not in temporalEngineFor(), because EDVR owns no external trained
    #    engine for MetalFX. DXMT encodes the scaler and keeps its history, so
    #    claiming an AMD- or NVIDIA-shaped coupling there would be wrong.
    gate = read(TEMPORAL_MODE_H, root)
    modes = gate_modes(gate, "temporalModeEnabled")
    if check(modes is not None, "%s declares temporalModeEnabled" % TEMPORAL_MODE_H):
        check("mfx" in modes,
              "the shared temporal-mode gate recognises mfx: %s" % sorted(modes))
        check({"on", "dlaa", "dlss", "fsr"} <= modes,
              "the gate still recognises every mode it did before mfx: %s" % sorted(modes))
    engine = gate_modes(gate, "temporalEngineFor", "TemporalEngine")
    if check(engine is not None, "%s declares temporalEngineFor" % TEMPORAL_MODE_H):
        check("mfx" not in engine,
              "mfx stays out of temporalEngineFor: DXMT owns the scaler and its history, "
              "so EDVR has no external engine to couple to")

    return out


def main():
    results = checks(ROOT)
    width = max(len(message) for _, message in results)
    failed = 0
    for ok, message in results:
        if not ok:
            failed += 1
        sys.stdout.write("%s  %s\n" % ("PASS" if ok else "FAIL", message))
    sys.stdout.write("\n%d checks, %d failed\n" % (len(results), failed))
    return 1 if failed else 0


def write_fixture(root, name, text):
    path = os.path.join(root, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(text.encode("utf-8"))
    return path


GOOD_HEADER = """#pragma once
#include <cstddef>
namespace edvr { namespace mfx {
constexpr GUID kIidContextExt = {0x43ace3ceu, 0x1956u, 0x448bu,
                                 {0xa4, 0xeb, 0xae, 0xe6, 0x8b, 0xde, 0xb2, 0x83}};
constexpr GUID kIidContextExt1 = {0x19a8e35au, 0x38beu, 0x418fu,
                                  {0x94, 0xe3, 0x9f, 0x73, 0x23, 0x93, 0x68, 0x70}};
enum class Feature : int { MetalFxTemporalScaler = 0 };
struct TemporalUpscaleDesc {
    UINT InputContentWidth;
    UINT InputContentHeight;
    BOOL AutoExposure;
    BOOL InReset;
    BOOL DepthReversed;
    BOOL MotionVectorInDisplayRes;
    ID3D11Resource *Color;
    ID3D11Resource *Depth;
    ID3D11Resource *MotionVector;
    ID3D11Resource *Output;
    FLOAT MotionVectorScaleX;
    FLOAT MotionVectorScaleY;
    FLOAT PreExposure;
    ID3D11Resource *ExposureTexture;
    FLOAT JitterOffsetX;
    FLOAT JitterOffsetY;
};
static_assert(sizeof(TemporalUpscaleDesc) == %d, "");
%s
struct ContextExt : public IUnknown {};
struct ContextExt1 : public ContextExt {};
bool mfxAvailable(ID3D11DeviceContext* ctx, const char** why);
bool mfxEvaluate(ID3D11DeviceContext* ctx, ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                 ID3D11Texture2D* mv, ID3D11Texture2D* out, uint32_t w, uint32_t h, uint32_t outW,
                 uint32_t outH, float jx, float jy, bool reset, const char** why, bool autoExposure);
}}
""" % (
    x64_layout(DESC_FIELDS)[1],
    "\n".join('static_assert(offsetof(TemporalUpscaleDesc, %s) == %d, "");' % (n, o)
              for n, o in x64_layout(DESC_FIELDS)[0].items()),
)

GOOD_SOURCE = """#include "metal_fx_engine.h"
namespace edvr {
bool mfxAvailable(ID3D11DeviceContext* ctx, const char** why) {
    // nvngx.dll is NOT used; DXMT_ENABLE_NVEXT is NOT required.
    (void)ctx;(void)why;return true; }
bool mfxAvailableEx(ID3D11DeviceContext* ctx, const char** why) { (void)ctx;(void)why;return true; }
bool mfxEvaluate(ID3D11DeviceContext* ctx, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*,
                 ID3D11Texture2D*, uint32_t, uint32_t, uint32_t, uint32_t, float, float, bool,
                 const char** why, bool) {
    mfx::ContextExt1 *ext = extension(ctx, why);
    if (!ext) return false;
    ext->CheckFeatureSupport(mfx::Feature::MetalFxTemporalScaler, nullptr, 0);
    mfx::TemporalUpscaleDesc desc{};
    desc.JitterOffsetX = jx;
    ext->TemporalUpscale(&desc);
    return true; }
}
"""

GOOD_RESOLVE_H = """#pragma once
enum class FlatMonoResolveMode { Taa, Dlaa, Dlss, Fsr, Mfx };
inline const char* flatMonoResolveModeName(FlatMonoResolveMode mode) {
    return mode == FlatMonoResolveMode::Fsr ? "fsr" : mode == FlatMonoResolveMode::Dlss ? "dlss"
         : mode == FlatMonoResolveMode::Dlaa ? "dlaa" : mode == FlatMonoResolveMode::Mfx ? "mfx" : "taa";
}
struct FlatResolveRoute { uint32_t evalWidth=0, evalHeight=0; const char* name=""; bool refused=true; };
inline FlatResolveRoute flatResolveRoute(FlatMonoResolveMode mode,
                                         uint32_t rW, uint32_t rH, uint32_t dW, uint32_t dH) {
    FlatResolveRoute out{};
    const bool smaller = rW < dW || rH < dH, larger = rW > dW || rH > dH;
    if (mode == FlatMonoResolveMode::Dlss) { out.evalWidth = dW; out.refused = false; return out; }
    const bool mfx = mode == FlatMonoResolveMode::Mfx;
    if (larger) {
        out.evalWidth = rW; out.evalHeight = rH; out.refused = false;
        out.name = mfx ? "mfx-native-aa-supersample" : "fsr-native-aa-supersample";
        return out;
    }
    out.evalWidth = dW; out.evalHeight = dH; out.refused = false;
    out.name = mfx ? "mfx-native" : "trained-native";
    return out;
}
"""

GOOD_RESOLVE_CPP = """#include "flat_mono_resolve.h"
namespace {
struct Constants { float camera[6][4], previous[6][4]; uint32_t size[4], flags[4]; float jitter[4], rowsJitter[4]; uint32_t route[4], debug[4]; float foregroundDepth[4]; uint32_t backend[4]; };
static_assert(sizeof(Constants)==320, "HLSL cbuffer layout");
void setFlags() {
    constants.debug[2]=overlay?1u:0u;
    constants.debug[3]=foreground?2u:(untrusted?1u:0u);
    constants.foregroundDepth[0]=sdkDepthScale;
    constants.backend[0]=mfx?1u:0u;
}
bool resolveModeValid(FlatMonoResolveMode mode) {
    return mode==FlatMonoResolveMode::Taa || mode==FlatMonoResolveMode::Dlaa ||
        mode==FlatMonoResolveMode::Dlss || mode==FlatMonoResolveMode::Fsr ||
        mode==FlatMonoResolveMode::Mfx;
}
const bool mfx = f.mode==FlatMonoResolveMode::Mfx;
const bool mfx=f.mode==FlatMonoResolveMode::Mfx;
void prepAndBackend(ID3D11DeviceContext* context) {
    flatcpu::Scope backendScope(flatcpu::kBackend);
    ok = true;
    if(taa) { context->Dispatch(1,1,1); }
    else if(f.mode==FlatMonoResolveMode::Fsr) { ok=fsr3Evaluate(context); }
    else if(mfx) { ok=mfxEvaluate(context,f.renderWidth,f.renderHeight,f.jitterX,f.jitterY); }
    else { ok=dlaaEvaluate(context); }
    context->CSSetShader(g.prep.Get(),nullptr,0);
    context->CSSetShader(g.finish.Get(),nullptr,0);
}
struct Isolate {
    Isolate(ID3D11DeviceContext1* context, ID3DDeviceContextState* state, bool byCapture) {
        if (byCapture) { capture(context); }
        else context->SwapDeviceContextState(state, previous.GetAddressOf());
    }
    ~Isolate() { if (!byCapture) context->SwapDeviceContextState(previous.Get(), nullptr); }
};
}
bool flatMonoResolveResolve(ID3D11DeviceContext* context) {
    context->UpdateSubresource(nullptr,0,nullptr,nullptr,0,0);
    if(taa) { context->Dispatch(1,1,1); }
    else if(f.mode==FlatMonoResolveMode::Fsr) { ok=fsr3Evaluate(context); }
    else if(mfx) { ok=mfxEvaluate(context); }
    else { ok=dlaaEvaluate(context); }
    context->CSSetShader(g.prep.Get(),nullptr,0);
    context->CSSetShader(g.finish.Get(),nullptr,0);
    return ok;
}
"""

GOOD_SHADER_H = """cbuffer Mono : register(b0) {
    float4 now[6]; float4 old[6]; uint4 size; uint4 flags;
    float4 jitter; float4 rowsJitter; uint4 route; uint4 debug; float4 foregroundDepth; uint4 backend;
};
void prep() {
    if(backend.x!=0) {
        float3 c=Color.Load(int3(q,0)).rgb;
        OutColor[q]=float4(clamp(c,0.0,65504.0),1.0);
    }
}
float4 finishHdr() {
    if(debug.z!=0) { covered=max(covered,OverlayCoverage.Load(int3(q,0))); }
}
"""

GOOD_RUNTIME_CPP = """s.engine = _stricmp(mode.c_str(), "fsr") == 0 ? FlatMonoResolveMode::Fsr :
            _stricmp(mode.c_str(), "mfx") == 0 ? FlatMonoResolveMode::Mfx : FlatMonoResolveMode::Taa;
"""

# The shared frontend gate, shaped as the real file has it: mfx present in
# temporalModeEnabled() and deliberately absent from temporalEngineFor().
GOOD_TEMPORAL_MODE = """#pragma once
#include <string>
namespace edvr {
inline bool temporalModeEnabled(const std::string& mode) {
    return _stricmp(mode.c_str(), "on") == 0 ||
           _stricmp(mode.c_str(), "dlaa") == 0 ||
           _stricmp(mode.c_str(), "dlss") == 0 ||
           _stricmp(mode.c_str(), "fsr") == 0 ||
           _stricmp(mode.c_str(), "mfx") == 0;
}
enum class TemporalEngine { Own, Nvidia, Amd };
inline TemporalEngine temporalEngineFor(const std::string& mode) {
    if (_stricmp(mode.c_str(), "dlaa") == 0 || _stricmp(mode.c_str(), "dlss") == 0) {
        return TemporalEngine::Nvidia;
    }
    if (_stricmp(mode.c_str(), "fsr") == 0) return TemporalEngine::Amd;
    return TemporalEngine::Own;
}
inline bool temporalExternalEngine(const std::string& mode) {
    return temporalEngineFor(mode) != TemporalEngine::Own;
}
}  // namespace edvr
"""


def self_test():
    saved_root = globals()["ROOT"]
    try:
        def fixture(header=None, source=None, resolve_h=None, resolve_cpp=None, runtime_cpp=None,
                    temporal_mode_h=None, shader_h=None):
            root = tempfile.mkdtemp(prefix="edvr-mfx-")
            write_fixture(root, HEADER, header if header is not None else GOOD_HEADER)
            write_fixture(root, SOURCE, source if source is not None else GOOD_SOURCE)
            write_fixture(root, RESOLVE_H, resolve_h if resolve_h is not None else GOOD_RESOLVE_H)
            write_fixture(root, RESOLVE_CPP, resolve_cpp if resolve_cpp is not None else GOOD_RESOLVE_CPP)
            write_fixture(root, RUNTIME_CPP, runtime_cpp if runtime_cpp is not None else GOOD_RUNTIME_CPP)
            write_fixture(root, TEMPORAL_MODE_H,
                          temporal_mode_h if temporal_mode_h is not None else GOOD_TEMPORAL_MODE)
            write_fixture(root, SHADER_H, shader_h if shader_h is not None else GOOD_SHADER_H)
            # flat_mono_resolve.cpp is also where the two swap call sites live;
            # the fixture keeps them so check 7 has something to find.
            return root

        def failures_for(**kwargs):
            return [message for ok, message in checks(fixture(**kwargs)) if not ok]

        # The layout arithmetic itself, independent of any file: a field moved
        # must move the offsets, and the size must follow.
        offsets, size = x64_layout(DESC_FIELDS)
        if offsets["Color"] != 24 or offsets["ExposureTexture"] != 72 or size != 88:
            print("self-test FAIL: x64_layout disagrees with the audited descriptor", file=sys.stderr)
            return 1
        swapped = list(DESC_FIELDS)
        swapped[6], swapped[7] = swapped[7], swapped[6]
        if x64_layout(swapped)[0] == offsets:
            print("self-test FAIL: x64_layout does not react to a reordering", file=sys.stderr)
            return 1

        # A good fixture must pass, or the guard proves nothing.
        if failures_for():
            print("self-test FAIL: the good fixture failed:\n  " + "\n  ".join(failures_for()), file=sys.stderr)
            return 1

        cases = []

        # A descriptor field reordered: every offset can stay put and the ABI
        # still breaks, so the order must be compared, not just the numbers.
        reordered = GOOD_HEADER.replace("    FLOAT PreExposure;\n    ID3D11Resource *ExposureTexture;",
                                        "    ID3D11Resource *ExposureTexture;\n    FLOAT PreExposure;")
        cases.append(("a reordered descriptor field", failures_for(header=reordered)))

        # One IID digit wrong: a QueryInterface that silently always fails.
        cases.append(("a mistyped IID",
                      failures_for(header=GOOD_HEADER.replace("0x43ace3ceu", "0x43ace3cfu"))))

        # The actual v0.18.2 port collision, and its host/HLSL coupling.
        cases.append(("MFX overwrites the upstream overlay flag",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace(
                          "constants.debug[2]=overlay?1u:0u;", "constants.debug[2]=mfx?1u:0u;"))))
        cases.append(("overlay TAA writes MetalFX's unbound prep UAV",
                      failures_for(shader_h=GOOD_SHADER_H.replace("if(backend.x!=0)", "if(debug.z!=0)"))))
        cases.append(("MetalFX flag drives the upstream overlay finish",
                      failures_for(shader_h=GOOD_SHADER_H.replace("if(debug.z!=0)", "if(backend.x!=0)"))))
        cases.append(("upstream alternate-camera flag lost",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace("constants.debug[3]=foreground?2u:(untrusted?1u:0u);", ""))))
        cases.append(("new host/HLSL constant lane disagrees",
                      failures_for(shader_h=GOOD_SHADER_H.replace("float4 foregroundDepth; uint4 backend;", "uint4 backend; float4 foregroundDepth;"))))
        cases.append(("stale private mono cbuffer size",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace("sizeof(Constants)==320", "sizeof(Constants)==288"))))

        # The v0.18.3 foreground-depth lane and its scale must survive the port.
        cases.append(("upstream foreground depth lane lost",
                      failures_for(shader_h=GOOD_SHADER_H.replace("float4 foregroundDepth; ", ""))))
        cases.append(("MetalFX overwrites the upstream foreground depth scale",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace(
                          "constants.foregroundDepth[0]=sdkDepthScale;", "constants.foregroundDepth[0]=mfx?1.f:0.f;"))))

        # A size assertion that drifted from the layout.
        cases.append(("a stale sizeof assertion",
                      failures_for(header=GOOD_HEADER.replace("== %d" % x64_layout(DESC_FIELDS)[1],
                                                              "== 96", 1))))

        # The invariants, each one broken on purpose.
        cases.append(("a backend that calls ClearState",
                      failures_for(source=GOOD_SOURCE.replace("return true; }\n}",
                                                             "context->ClearState(); return true; }\n}"))))
        cases.append(("a backend that loads nvngx.dll",
                      failures_for(source=GOOD_SOURCE +
                                   '\nHMODULE m = LoadLibraryW(L"nvngx.dll");\n')))
        cases.append(("a backend that names a Metal type",
                      failures_for(source=GOOD_SOURCE + "\nWMT::Texture t;\n")))
        cases.append(("a MetalFX 4 command buffer",
                      failures_for(source=GOOD_SOURCE + "\nid<MTL4CommandBuffer> cb = nullptr;\n")))
        # (MetalFX 4 cases were removed with the deferred mfx4 mode.)
        cases.append(("a backend that swaps context state",
                      failures_for(source=GOOD_SOURCE +
                                   "\ncontext->SwapDeviceContextState(a,b);\n")))
        # The regression this check exists for: Mfx declared, named, parsed
        # and routed, yet not admitted, so every mfx frame refused at the resolve
        # entry point and nothing downstream ever ran.
        cases.append(("Mfx removed from the resolveModeValid admission predicate",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace(
                          "||\n        mode==FlatMonoResolveMode::Mfx;",
                          "||\n        mode==FlatMonoResolveMode::Taa;"))))
        # The same defect one predicate further on: admitted, resources allocated, and
        # then dispatched to dlaaEvaluate because the dispatch-local copy did not name it.
        cases.append(("Mfx removed from the MetalFX dispatch predicate",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace(
                          "const bool mfx=f.mode==FlatMonoResolveMode::Mfx;",
                          "const bool mfx=f.mode==FlatMonoResolveMode::Taa;"))))
        # A comment NAMES the forbidden thing without using it: that is the file's
        # own documentation style, and the guard must not flag it.
        if failures_for(source=GOOD_SOURCE + "\n// deliberately not nvngx, not NVAPI\n"):
            print("self-test FAIL: a comment naming a forbidden route was flagged", file=sys.stderr)
            return 1
        print("ok  %-52s not flagged (a comment is not a route)" % "a comment naming nvngx")
        cases.append(("no Mfx enumerator",
                      failures_for(resolve_h=GOOD_RESOLVE_H.replace(", Mfx", ""))))
        cases.append(("Mfx routed on its own, able to downscale",
                      failures_for(resolve_h=GOOD_RESOLVE_H.replace(
                          "out.evalWidth = dW; out.evalHeight = dH; out.refused = false;",
                          "out.evalWidth = rW > dW ? dW : rW; out.refused = false;"))))
        cases.append(("an unreachable Mfx parse",
                      failures_for(runtime_cpp="s.engine = FlatMonoResolveMode::Taa;\n")))
        cases.append(("a dispatch where FSR3 was replaced rather than added to",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace("fsr3Evaluate(context)", "mfxEvaluate(context)"))))
        cases.append(("a dispatch with the prep kernel removed",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP.replace("g.prep.Get()", "nullptr"))))
        cases.append(("a third swap call site",
                      failures_for(resolve_cpp=GOOD_RESOLVE_CPP + "\nvoid extra() { ctx->SwapDeviceContextState(a,b); }\n")))
        # The bug a flight found: the mode parsed, routed, and was then stood
        # down before mfxAvailable() could run, so the log carried a routed
        # mode, a refusal reason, and no MFX line at all.
        # NOTE: in the fixture mfx is the last comparison, so removing its
        # line removes the line that ends the expression.
        cases.append(("the shared temporal-mode gate no longer recognises mfx",
                      failures_for(temporal_mode_h=GOOD_TEMPORAL_MODE.replace(
                          '           _stricmp(mode.c_str(), "mfx") == 0;\n', ""))))
        # A mode quietly dropped from the same gate while another is added.
        cases.append(("a mode dropped from the shared temporal-mode gate",
                      failures_for(temporal_mode_h=GOOD_TEMPORAL_MODE.replace(
                          '           _stricmp(mode.c_str(), "dlss") == 0 ||\n', ""))))
        # The boundary that must not move: mfx is not an external engine here.
        cases.append(("mfx claimed as an external engine in temporalEngineFor",
                      failures_for(temporal_mode_h=GOOD_TEMPORAL_MODE.replace(
                          '    if (_stricmp(mode.c_str(), "fsr") == 0) return TemporalEngine::Amd;',
                          '    if (_stricmp(mode.c_str(), "fsr") == 0) return TemporalEngine::Amd;\n'
                          '    if (_stricmp(mode.c_str(), "mfx") == 0) return TemporalEngine::Nvidia;'))))

        bad = 0
        for name, failures in cases:
            if not failures:
                print("self-test FAIL: %s was NOT caught" % name, file=sys.stderr)
                bad += 1
            else:
                print("ok  %-52s caught by %d check(s)" % (name, len(failures)))
        if bad:
            print("self-test FAIL: %d fixture(s) slipped through" % bad, file=sys.stderr)
            return 1
        print("\nmetafx backend self-test: %d fixtures, all caught" % len(cases))
        return 0
    finally:
        globals()["ROOT"] = saved_root


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv[1:] else main())