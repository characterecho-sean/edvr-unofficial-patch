#!/usr/bin/env python3
"""The mutation proof for tools\\panel_curve_test: the rig fails when a rule of the curved screen's module is flipped.

The rig (panel_curve_test.cpp) runs the REAL src\\d3d11\\panel_curve.cpp on a WARP device, in thirteen cases C1..C13 (its header says
which; C12 and C13 are the surface strip of the intro movie and the splash); every check it makes carries a label "C<case>.<what>". A rig that passes proves little until it is seen to FAIL on a module that
breaks the rule it pins. This tool does that: for each mutation below it copies panel_curve.cpp (and panel_curve.h where the rule lives
there) into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong together), compiles the module (and the
rig, when the header changed) against that copy, links the rig with the unmutated common sources, runs it, and requires it to fail on a
check of the case that belongs to the rule (a FAIL label starting with one of the mutation's case ids). Nothing is written inside the
repo; the temp directory is removed at the end.

  python tools\\panel_curve_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it
                                                              is now, every case named is in the rig, and build.bat compiles the rig
                                                              the way this tool does (add --build-bat PATH to check another copy)
  python tools\\panel_curve_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\panel_curve_test\\mutants.py --list
  python tools\\panel_curve_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does); build\\gen is added to the
include path when there is one, as the rig's build.bat line does. It takes about a minute: the four common sources and the rig are
compiled once, then each mutation compiles one file (two for a header) and links. --self-test runs in build.bat's rig and is what
keeps an edit of the module from silently orphaning a mutation: if an anchor stops matching, the build fails and this file says
which.
"""
import argparse
import concurrent.futures
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / "src" / "d3d11"
FILES = {"cpp": SRC / "panel_curve.cpp", "h": SRC / "panel_curve.h"}
MOTION_H = SRC / "screen_motion.h"   # includes panel_curve.h itself, so a mutated header needs this copied beside it
RIG = HERE / "panel_curve_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_panel_curve_test"
COMMON = [ROOT / "src" / "common" / n for n in ("config.cpp", "log.cpp", "guard.cpp", "proxy.cpp")]
LIBS = ["user32.lib", "version.lib"]

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 120.0


class Mutant:
    def __init__(self, name, caught, edits, why, file="cpp"):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # case ids whose checks must report it
        self.edits = list(edits)                                                # (old, new) pairs, applied in order, to `file`
        self.why = why
        self.file = file                                                        # "cpp" or "h"


def M(name, caught, edits, why, file="cpp"):
    return Mutant(name, caught, edits, why, file)


def drop(old):
    return [(old, "")]


# ---- the anchors: text of the production sources, verbatim (the self-test finds each exactly once) ------------------------------
SAVE_VB = "    ctx->IAGetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);\n"
SAVE_IB = "    ctx->IAGetIndexBuffer(&g_savedIb, &g_savedFmt, &g_savedIbOffset);\n"
SAVE_TOPO = "    ctx->IAGetPrimitiveTopology(&g_savedTopo);\n"
BIND_IB = "    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);\n"
BIND_TOPO = "    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);\n"
THE_DRAW = "    draw(ctx, indexCount, 1, 0, 0, 0);\n"
RESTORE_VB = "    ctx->IASetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);\n"
RESTORE_IB = "    ctx->IASetIndexBuffer(g_savedIb, g_savedFmt, g_savedIbOffset);\n"
RESTORE_TOPO = "    ctx->IASetPrimitiveTopology(g_savedTopo);\n"
CURRENT_CURVE = "    return g_vb && g_ib && g_builtCurvature == detail::g_panelCurveCurvature &&\n"
CURRENT_REST = "           g_builtSegments == detail::g_panelCurveSegments && g_builtSign == g_sign && g_builtGain == activeGain();\n"
STAND_DOWN_READY = "    detail::g_panelCurveStoodDown = true;\n    detail::g_panelCurveReady = false;\n"
STAND_DOWN_RESTORE = '    guarded("panelCurve.restore", [&] { restoreSaved(ctx); });\n'
BEND_X = "    *xOut = sinf(theta) / k;\n"
BEND_Z_SIGN = "    *zOut = kTowardViewer * static_cast<float>(sign) * gain *\n"
BEND_Z_COS = "            (1.0f - cosf(theta)) / k;\n"
STRIP_X = "        const float x = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n);\n"
STRIP_U = "        const float u = reverseU ? (1.0f - x) * 0.5f : (x + 1.0f) * 0.5f;\n"
STRIP_BOTTOM = "        vb[i] = Vertex{bx, -1.0f, bz, u, 1.0f};\n"
STRIP_TOP = "        vb[(n + 1) + i] = Vertex{bx, 1.0f, bz, u, 0.0f};\n"
WIND_1 = "        ib[w++] = bl; ib[w++] = tr; ib[w++] = br;\n"
WIND_2 = "        ib[w++] = bl; ib[w++] = tl; ib[w++] = tr;\n"
PUBLISH_GAIN = "    detail::g_panelCurveGain = g_builtGain;\n"
SUB_GUARD = "    if (!ctx || !draw || detail::g_panelCurveStoodDown) { detail::g_panelCurveReady = false; return false; }\n"
SUB_LEARN = "        if (!learnSize(ctx)) return;\n"
SUB_BUILD = "        if (!stripCurrent()) {\n            if (!build(ctx)) return;\n        }\n"
SUB_DRAW_MOTION = "        drawStripHeld(ctx, draw);\n        if (withMotion) {\n"
SUB_WITH_MOTION = "        if (withMotion) {\n"
SUB_MOTION_CALL = "            screenMotionDraw(ctx,draw,g_indexCount,1,0,0,0,shape);\n"
SUB_SHAPE0 = "            const float shape[4]={kPi*g_builtCurvature,float(g_builtSegments),\n"
SUB_SHAPE2 = "                kTowardViewer*float(g_builtSign)*g_builtGain,0.0f};\n"
SUB_RESTORE = "        restoreSaved(ctx);\n\n        substituted = true;\n"
SUB_FAULT = '        standDownAfterFault(ctx, "the substitution");\n        return false;\n'
SUB_READY = "    detail::g_panelCurveReady = substituted;\n    return substituted;\n"
REISSUE_GUARD = "    if (!ctx || !draw || !panelCurveReissueReady()) return false;\n"
REISSUE_BODY = "        drawStripHeld(ctx, draw);\n        restoreSaved(ctx);\n        drawn = true;\n"
REISSUE_FAULT = '        standDownAfterFault(ctx, "the VR world route\'s re-issue of the strip");\n        return false;\n'
REISSUE_COUNT = "    if (++detail::g_panelCurveReissues == 1) {\n"
READY_RETURN = "    return panelCurveWants() && stripCurrent();\n"
CONFIG_SIGN = '    g_sign = cfg.getIntInRange("advanced.panel_curvature_sign", 1, -1, 1) < 0 ? -1 : 1;\n'
CONFIG_GAIN = '    g_zGainCfg = cfg.getFloat("advanced.panel_curvature_z_gain", 0.0f);\n'
LAG = "constexpr uint64_t kReadbackLagMs = 50;\n"
ASPECT = "constexpr float kPanelAspect = 16.0f / 9.0f;\n"
BASIS_Z = "                    const float z0 = f[38], z1 = f[42], z2 = f[46];\n"
BASIS_RATIO = "                        float r = lx / lz;\n"
SIZE_Y = "                g_sizeY = sz[1];\n"
BAD_SIZE_DOWN = "                    sz[0], sz[1]);\n                detail::g_panelCurveStoodDown = true;\n"
NOTHING_BOUND_DOWN = '                "supplies one by hand if this build binds it elsewhere.");\n            detail::g_panelCurveStoodDown = true;\n'
IDENTITY_Z = "        *zOut = 0.0f;\n"
IDENTITY_X = "        *xOut = x;\n"
IDENTITY_GUARD = "    if (c <= 0.0f) {\n"
GAIN_RATIO = "    return g_sizeY * kPanelAspect * (g_basisLearned ? g_basisRatio : 1.0f);\n"
SIZE_LEARNED = "                g_sizeLearned = true;\n"
GAIN_RANGE = "    if (g_zGainCfg < 0.0f || g_zGainCfg > 10000.0f) g_zGainCfg = 0.0f;\n"
GAIN_OVERRIDE = "    if (g_zGainCfg > 0.0f) return g_zGainCfg;\n"
RESTORE_RELEASE ="    if (g_savedVb) { g_savedVb->Release(); g_savedVb = nullptr; }\n    if (g_savedIb) { g_savedIb->Release(); g_savedIb = nullptr; }\n}\n"
REISSUE_RETURN = "            detail::g_panelCurveSegments, detail::g_panelCurveCurvature);\n    }\n    return drawn;\n"
CURVATURE_RANGE = "    if (c < 0.0f || c > kMaxCurvature) {\n"
SEGMENTS_RANGE = "                                   kDefaultSegments, kMinSegments, kMaxSegments);\n"
DEFAULT_MOTION = "bool panelCurveSubstitute(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw, bool withMotion = true);\n"
WANTS_CURVATURE = "    return detail::g_panelCurveCurvature > 0.0f ||\n           detail::g_panelCurveSegments != detail::kDefaultSegments;\n"
WANTS_STOOD_DOWN = "    if (detail::g_panelCurveStoodDown) return false;\n"
INFO_GAIN = "    i.gain = detail::g_panelCurveGain;\n"
INFO_SEGMENTS = "    i.segments = detail::g_panelCurveSegments;\n"
INFO_REISSUES = "    i.reissues = detail::g_panelCurveReissues;\n"
DEFAULT_CURVATURE = '    float c = cfg.getFloat("fix.panel_curvature", 0.0f);\n'
DEFAULT_SEGMENTS = "constexpr int kDefaultSegments = 64;\n"
# the surface strip (C12, C13)
S_WANTED_CFG = "    g_sWanted = c > 0.0f && !g_sStoodDown;   // the surface strip follows the live curvature, and nothing else of the screen's\n"
S_WANTED_RET = "bool panelCurveSurfaceWanted() {\n    return g_sWanted;\n}\n"
S_DRAW_GUARD = "    if (!ctx || !draw || !g_sWanted) return false;\n"
S_ARG_GUARD = "    if (!(gain > 0.0f && gain < 1.0e6f) || (toward != 1 && toward != -1)) return false;\n"
S_BUDGET = "    const bool ok = guardedBudget(g_sBudget, [&] {\n"
S_CURRENT_ALL = ("    return g_sVb && g_sIb && g_sBuiltCurvature == detail::g_panelCurveCurvature && g_sBuiltSegments == detail::g_panelCurveSegments &&\n"
                 "           g_sBuiltGain == gain && g_sBuiltToward == toward && g_sBuiltReverseU == reverseU;\n")
S_REBUILD = "        if (!surfaceCurrent(gain, toward, reverseU)) {\n            if (!buildSurface(ctx, gain, toward, reverseU)) return;\n        }\n"
S_FILL = "    fillStrip(n, detail::g_panelCurveCurvature, -toward, gain, reverseU, vb, ib);\n"
# the way u runs (job 4, the mirror fix)
ONFOOT_FILL = "    fillStrip(n, detail::g_panelCurveCurvature, g_sign, activeGain(), false, vb, ib);   // the screen's u runs with x: its +x runs to the viewer's right\n"
S_BUILT_REVERSE = "    g_sBuiltReverseU = reverseU;\n"
S_LOG_DIR = '        reverseU ? "against x: the placement\'s +x runs to the viewer\'s left" : "with x");\n'
S_INFO_REVERSED = "    i.reversed = g_sVb && g_sBuiltReverseU;   // the direction of the strip in hand: false with none in hand\n"
S_BUILT_COUNT = "    ++g_sBuilt;\n"
S_RS_CULL = "            if (game.CullMode != D3D11_CULL_NONE) {\n"
S_RS_SWAP = "                ctx->RSSetState(off);\n                g_sRsSwapped = true;\n"
S_DRAW_TAIL = "        drawStripHeld(ctx, draw, g_sVb, g_sIb, g_sIndexCount);\n        restoreSaved(ctx);\n        restoreSurfaceRs(ctx);\n"
S_DRAWN = "        drawn = true;\n        ++g_sDrawn;\n"
S_RS_RESTORE = "    if (swapped) ctx->RSSetState(g_sSavedRs);\n    if (g_sSavedRs) { g_sSavedRs->Release(); g_sSavedRs = nullptr; }\n"
S_CULL_OFF = "    d.CullMode = D3D11_CULL_NONE;\n"
S_CACHE_HIT = "        if (s.off && memcmp(&s.game, &game, sizeof(game)) == 0) return s.off;\n"
S_RS_COUNT = "    ++g_sRasterStates;\n"
S_STAND_FLAGS = "    g_sStoodDown = true;\n    g_sWanted = false;\n"
S_STAND_IA = '    guarded("panelCurve.surface.restore", [&] { restoreSaved(ctx); });\n'
S_STAND_RS = '    guarded("panelCurve.surface.restoreRs", [&] { restoreSurfaceRs(ctx); });\n'
S_STAND_LOG = '        "panel curvature: the surface strip (the intro movie and the splash) faulted, so those two are flat for the rest of this session and "\n'
S_INFO_BUILT = "    i.built = g_sBuilt;\n"
S_INFO_STAND = "    i.standDown = g_sStoodDown;\n"
S_INFO_STATES = "    i.rasterStates = g_sRasterStates;\n"
S_SHUT_CULL = "    for (CullOffState& s : g_sCullOff) {\n        if (s.off) { s.off->Release(); s.off = nullptr; }\n"
S_EVICT = "    if (slot.off) slot.off->Release();\n"
S_SHUT_INDEX = "    g_sIndexCount = 0;\n    g_sBuiltCurvature = -1.0f;\n"

MUTANTS = [
    # ---- C1: nothing is wanted at curvature 0 -------------------------------------------------------------------------------
    M("wants-at-curvature-zero", "C1", [(WANTS_CURVATURE, "    return detail::g_panelCurveCurvature >= 0.0f ||\n           detail::g_panelCurveSegments != detail::kDefaultSegments;\n")],
      "curvature 0 at the default columns is wanted", "h"),
    M("default-curvature-on", "C1", [(DEFAULT_CURVATURE, '    float c = cfg.getFloat("fix.panel_curvature", 0.3f);\n')], "with no key set the screen is curved"),
    M("default-columns-32", "C1", [(DEFAULT_SEGMENTS, "constexpr int kDefaultSegments = 32;\n")], "the default column count is 32, so a 64-column flat strip is the identity test", "h"),
    M("reissue-ready-always", ("C1", "C5"), [(READY_RETURN, "    return true;\n")], "the re-issue is always ready, wanted or not, current or not"),
    # ---- C2: the substitution -----------------------------------------------------------------------------------------------
    M("bend-sign-flipped", "C2", [(BEND_Z_SIGN, "    *zOut = -kTowardViewer * static_cast<float>(sign) * gain *\n")], "the screen bends away from the viewer"),
    M("bend-x-uses-cos", "C2", [(BEND_X, "    *xOut = cosf(theta) / k;\n")], "x' is cos(theta)/k"),
    M("bend-ignores-gain", "C2", [(BEND_Z_SIGN, "    *zOut = kTowardViewer * static_cast<float>(sign) * 1.0f *\n")], "the gain does not reach the bend"),
    M("learned-beats-override", "C2", [(GAIN_OVERRIDE, "")], "a gain read from the game wins over the override (what C8 learned is used from then on)"),
    M("bend-z-uses-sin", "C2", [(BEND_Z_COS, "            (1.0f - sinf(theta)) / k;\n")], "z' is 1 - sin(theta)"),
    M("uv-from-bent-x", "C2", [(STRIP_U, "        const float u = reverseU ? (1.0f - bx) * 0.5f : (bx + 1.0f) * 0.5f;\n")], "the UV follows the bent x, so the bend moves texels"),
    M("winding-first-flipped", ("C2", "C7"), [(WIND_1, "        ib[w++] = bl; ib[w++] = br; ib[w++] = tr;\n")], "the first triangle of each quad is wound the other way"),
    M("winding-second-flipped", ("C2", "C7"), [(WIND_2, "        ib[w++] = bl; ib[w++] = tr; ib[w++] = tl;\n")], "the second triangle of each quad is wound the other way"),
    M("index-format-r32", "C2", [(BIND_IB, "    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);\n")], "the strip's indices are bound as 32-bit"),
    M("topology-strip", "C2", [(BIND_TOPO, "    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);\n")], "the strip is drawn as a triangle strip"),
    M("draw-through-the-context", "C2", [(THE_DRAW, "    ctx->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);\n")],
      "the draw goes through the context's own entry, not the original pointer it is handed (our hook would recurse)"),
    M("draw-start-instance-one", "C2", [(THE_DRAW, "    draw(ctx, indexCount, 1, 0, 0, 1);\n")], "the draw starts at instance 1"),
    M("restore-skips-topology", "C2", [(RESTORE_TOPO, "")], "the game's topology is not put back"),
    M("restore-skips-index-buffer", "C2", [(RESTORE_IB, "")], "the game's index buffer is not put back"),
    M("restore-forgets-index-offset", "C2", [(RESTORE_IB, "    ctx->IASetIndexBuffer(g_savedIb, g_savedFmt, 0);\n")], "the game's index buffer comes back at offset 0"),
    M("restore-swaps-stride-and-offset", "C2", [(RESTORE_VB, "    ctx->IASetVertexBuffers(0, 1, &g_savedVb, &g_savedOffset, &g_savedStride);\n")], "the game's vertex stride and offset come back swapped"),
    M("save-skips-topology", "C2", [(SAVE_TOPO, "")], "the game's topology is never saved"),
    M("save-skips-index-buffer", "C2", [(SAVE_IB, "")], "the game's index buffer is never saved"),
    M("substitute-skips-the-restore", "C2", [(SUB_RESTORE, "\n        substituted = true;\n")], "the substitution leaves the strip bound"),
    M("motion-after-the-restore", "C2", [(SUB_DRAW_MOTION, "        drawStripHeld(ctx, draw);\n        restoreSaved(ctx);\n        if (withMotion) {\n")],
      "the motion pass is issued after the game's state is back, so it does not see the strip"),
    M("motion-count-wrong", "C2", [(SUB_MOTION_CALL, "            screenMotionDraw(ctx,draw,6,1,0,0,0,shape);\n")], "the motion pass is told 6 indices"),
    M("motion-curve-without-pi", "C2", [(SUB_SHAPE0, "            const float shape[4]={g_builtCurvature,float(g_builtSegments),\n")], "the motion curve carries c, not pi*c"),
    M("motion-curve-sign-dropped", "C2", [(SUB_SHAPE2, "                float(g_builtSign)*g_builtGain,0.0f};\n")], "the motion curve's depth has the wrong sign"),
    M("config-ignores-gain-override", "C2", [(CONFIG_GAIN, "    g_zGainCfg = 0.0f;\n")], "the z-gain override is never read"),
    M("restore-leaks-the-saved-buffers", "C2", [(RESTORE_RELEASE, "}\n")], "the references taken on the game's buffers are never given back"),
    M("substitute-returns-false-on-success", "C2", [(SUB_READY, "    detail::g_panelCurveReady = substituted;\n    return false;\n")], "the substitution drew but says it did not (the game's own draw would follow it)"),
    # ---- C3: no motion pass -------------------------------------------------------------------------------------------------
    M("motion-ignores-withMotion", "C3", [(SUB_WITH_MOTION, "        if (true) {\n")], "the motion pass is issued whether asked for or not"),
    # ---- C4: the re-issue ---------------------------------------------------------------------------------------------------
    M("reissue-binds-the-games-ia", "C4", [(REISSUE_BODY, "        draw(ctx, g_indexCount, 1, 0, 0, 0);\n        drawn = true;\n")],
      "the re-issue draws with whatever is bound, the game's quad buffers, not the strip"),
    M("reissue-issues-the-motion-pass", "C4", [(REISSUE_BODY, "        drawStripHeld(ctx, draw);\n        screenMotionDraw(ctx, draw, g_indexCount, 1, 0, 0, 0, nullptr);\n        restoreSaved(ctx);\n        drawn = true;\n")],
      "the re-issue also issues the motion pass"),
    M("reissue-skips-the-restore", "C4", [(REISSUE_BODY, "        drawStripHeld(ctx, draw);\n        drawn = true;\n")], "the re-issue leaves the strip bound"),
    M("reissue-returns-false-on-success", "C4", [(REISSUE_RETURN, "            detail::g_panelCurveSegments, detail::g_panelCurveCurvature);\n    }\n    return false;\n")],
      "the re-issue drew but says it did not (the route's bracket would be closed without the eye)"),
    M("reissue-counter-never-counts", "C4", [(REISSUE_COUNT, "    if (detail::g_panelCurveReissues == 1) {\n")], "the re-issue counter never moves"),
    M("reissue-note-every-time", "C4", [(REISSUE_COUNT, "    if (++detail::g_panelCurveReissues >= 1) {\n")], "the first-call line is written at every re-issue"),
    # ---- C5: a live change ---------------------------------------------------------------------------------------------------
    M("current-ignores-gain", "C5", [(CURRENT_REST, "           g_builtSegments == detail::g_panelCurveSegments && g_builtSign == g_sign;\n")], "a changed gain does not make the strip stale"),
    M("current-ignores-sign", "C5", [(CURRENT_REST, "           g_builtSegments == detail::g_panelCurveSegments && g_builtGain == activeGain();\n")], "a changed sign does not make the strip stale"),
    M("current-ignores-segments", "C5", [(CURRENT_REST, "           g_builtSign == g_sign && g_builtGain == activeGain();\n")], "a changed column count does not make the strip stale"),
    M("current-ignores-curvature", "C5", [(CURRENT_CURVE, "    return g_vb && g_ib &&\n")], "a changed curvature does not make the strip stale"),
    M("always-rebuilds", "C5", [(SUB_BUILD, "        if (!build(ctx)) return;\n")], "the strip is rebuilt at every substitution"),
    M("reissue-ignores-readiness", "C5", [(REISSUE_GUARD, "    if (!ctx || !draw) return false;\n")], "the re-issue draws a stale strip"),
    M("config-ignores-sign", ("C5", "C6"), [(CONFIG_SIGN, "    g_sign = 1;\n")], "the depth sign is never read"),
    M("bend-ignores-sign", ("C5", "C6"), [(BEND_Z_SIGN, "    *zOut = kTowardViewer * gain *\n")], "the sign does not reach the bend"),
    # ---- C6: the table -------------------------------------------------------------------------------------------------------
    M("index-count-hard-coded", "C6", [(THE_DRAW, "    draw(ctx, 384, 1, 0, 0, 0);\n")], "the draw is always 384 indices, the default's"),
    M("curvature-one-refused", "C6", [(CURVATURE_RANGE, "    if (c < 0.0f || c >= kMaxCurvature) {\n")], "a curvature of exactly 1 (a closed cylinder) is treated as off"),
    # ---- C7: the identity ----------------------------------------------------------------------------------------------------
    M("identity-width-short", "C7", [(STRIP_X, "        const float x = -1.0f + 1.99f * static_cast<float>(i) / static_cast<float>(n);\n")], "the strip stops short of the quad's right edge"),
    M("identity-v-flipped", "C7", [(STRIP_BOTTOM, "        vb[i] = Vertex{bx, -1.0f, bz, u, 0.0f};\n"), (STRIP_TOP, "        vb[(n + 1) + i] = Vertex{bx, 1.0f, bz, u, 1.0f};\n")],
      "V runs the other way: the screen is upside down"),
    M("identity-rows-swapped", "C7", [(STRIP_BOTTOM, "        vb[i] = Vertex{bx, 1.0f, bz, u, 0.0f};\n"), (STRIP_TOP, "        vb[(n + 1) + i] = Vertex{bx, -1.0f, bz, u, 1.0f};\n")],
      "the top row is numbered first, so the strip is not the game's quad in the game's order"),
    M("identity-bend-z-offset", "C7", [(IDENTITY_Z, "        *zOut = 0.5f;\n")], "the flat strip sits half a unit off the quad's plane"),
    M("identity-bend-x-scaled", "C7", [(IDENTITY_X, "        *xOut = x * 0.5f;\n")], "the flat strip is half as wide as the quad"),
    M("identity-guard-off-by-one", "C7", [(IDENTITY_GUARD, "    if (c < 0.0f) {\n")], "curvature 0 reaches the bend's division by pi*c"),
    M("wants-ignores-segments", ("C7", "C10"), [(WANTS_CURVATURE, "    return detail::g_panelCurveCurvature > 0.0f;\n")], "the identity test's column count does not ask for a substitution", "h"),
    # ---- C8: not ready, and what is learned ------------------------------------------------------------------------------------
    M("ready-flag-true-when-not-drawn", "C8", [(SUB_READY, "    detail::g_panelCurveReady = true;\n    return substituted;\n")], "the ready flag is set although the call drew nothing"),
    M("null-arguments-not-refused", "C8", [(SUB_GUARD, "    if (detail::g_panelCurveStoodDown) { detail::g_panelCurveReady = false; return false; }\n")], "a null context or draw function is not refused up front"),
    M("draws-before-the-gain-is-known", "C8", [(SUB_LEARN, "        learnSize(ctx);\n")], "the strip is drawn while the gain is still unknown"),
    M("gain-from-size-x", "C8", [(SIZE_Y, "                g_sizeY = sz[0];\n")], "the gain takes SIZE.x for the visual half-width"),
    M("basis-ratio-inverted", "C8", [(BASIS_RATIO, "                        float r = lz / lx;\n")], "the world basis ratio is z over x"),
    M("basis-reads-the-wrong-float", "C8", [(BASIS_Z, "                    const float z0 = f[37], z1 = f[42], z2 = f[46];\n")], "the z basis reads float 37 instead of 38"),
    M("aspect-dropped", "C8", [(ASPECT, "constexpr float kPanelAspect = 1.0f;\n")], "the 16:9 content aspect is left out of the gain"),
    M("ratio-never-used", "C8", [(GAIN_RATIO, "    return g_sizeY * kPanelAspect;\n")], "the world basis ratio is read but never reaches the gain"),
    M("size-never-learned", "C8", [(SIZE_LEARNED, "")], "a SIZE that reads back fine is never taken as learned"),
    M("readback-lag-zero", "C8", [(LAG, "constexpr uint64_t kReadbackLagMs = 0;\n")], "the SIZE copy is mapped at once, stalling the render thread"),
    M("readback-lag-long", "C8", [(LAG, "constexpr uint64_t kReadbackLagMs = 5000;\n")], "the SIZE copy is not read for five seconds"),
    M("default-motion-off", "C8", [(DEFAULT_MOTION, "bool panelCurveSubstitute(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw, bool withMotion = false);\n")],
      "the substitution's motion pass is off unless asked for", "h"),
    M("nothing-bound-does-not-stand-down", "C8", [(NOTHING_BOUND_DOWN, '                "supplies one by hand if this build binds it elsewhere.");\n')], "no SIZE bound is not a reason to stand down"),
    M("bad-size-does-not-stand-down", "C8", [(BAD_SIZE_DOWN, "                    sz[0], sz[1]);\n")], "a SIZE that cannot be a panel's is not a reason to stand down"),
    # ---- C9: faults ----------------------------------------------------------------------------------------------------------
    M("reissue-fault-does-not-stand-down", "C9", [(REISSUE_FAULT, "        restoreSaved(ctx);\n        return false;\n")], "a fault in the re-issue puts the state back but leaves the feature on"),
    M("substitute-fault-does-not-stand-down", "C9", [(SUB_FAULT, "        restoreSaved(ctx);\n        return false;\n")], "a fault in the substitution puts the state back but leaves the feature on"),
    M("stand-down-skips-the-restore", "C9", [(STAND_DOWN_RESTORE, "")], "a fault leaves the strip bound in the game's input assembler"),
    M("stand-down-leaves-ready", "C9", [(STAND_DOWN_READY, "    detail::g_panelCurveStoodDown = true;\n")], "a fault leaves the ready flag set"),
    M("substitute-ignores-stand-down", "C9", [(SUB_GUARD, "    if (!ctx || !draw) { detail::g_panelCurveReady = false; return false; }\n")], "a stood-down feature still substitutes"),
    M("reissue-counter-counts-attempts", "C9", [(REISSUE_BODY, "        ++detail::g_panelCurveReissues;\n        drawStripHeld(ctx, draw);\n        restoreSaved(ctx);\n        drawn = true;\n"),
                                                (REISSUE_COUNT, "    if (detail::g_panelCurveReissues == 1) {\n")], "the counter counts a re-issue before it is known to have drawn"),
    M("wants-ignores-stand-down", "C9", [(WANTS_STOOD_DOWN, "")], "a stood-down feature is still wanted", "h"),
    # ---- C10: the numbers ------------------------------------------------------------------------------------------------------
    M("build-does-not-publish-the-gain", "C10", [(PUBLISH_GAIN, "")], "the gain the strip was built with is not published"),
    M("curvature-range-not-enforced", "C10", [(CURVATURE_RANGE, "    if (false) {\n")], "a curvature above 1 or below 0 is used as written"),
    M("gain-override-range-not-enforced", "C10", [(GAIN_RANGE, "")], "a z-gain override of 20000, or a negative one, is used as written"),
    M("segments-not-clamped", "C10", [(SEGMENTS_RANGE, "                                   kDefaultSegments, kMinSegments, 100000);\n")], "a column count past 256 is used as written (the strip is built on a 256-column stack)"),
    M("info-gain-reads-curvature", "C10", [(INFO_GAIN, "    i.gain = detail::g_panelCurveCurvature;\n")], "panelCurveInfo() reports the curvature as the gain", "h"),
    M("info-segments-constant", "C10", [(INFO_SEGMENTS, "    i.segments = detail::kDefaultSegments;\n")], "panelCurveInfo() always reports the default column count", "h"),
    M("info-reissues-zero", "C10", [(INFO_REISSUES, "    i.reissues = 0;\n")], "panelCurveInfo() never reports a re-issue", "h"),
    # ---- C11: the rest of the pipeline -----------------------------------------------------------------------------------------
    M("restore-clears-vertex-slot-1", "C11", [(RESTORE_TOPO, RESTORE_TOPO + "    { ID3D11Buffer* none = nullptr; UINT zero = 0; ctx->IASetVertexBuffers(1, 1, &none, &zero, &zero); }\n")],
      "putting the game's state back also unbinds the SIZE record in slot 1"),
    M("draw-clears-the-ps-resource", "C11", [(BIND_TOPO, BIND_TOPO + "    { ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(0, 1, &none); }\n")],
      "the strip's draw unbinds the pixel shader's resource"),
    M("draw-clears-the-rasterizer", "C11", [(THE_DRAW, THE_DRAW + "    ctx->RSSetState(nullptr);\n")], "the strip's draw leaves the default rasterizer state bound"),
    # ---- C12: the surface strip -----------------------------------------------------------------------------------------------
    M("surface-wanted-at-curvature-zero", "C12", [(S_WANTED_CFG, S_WANTED_CFG.replace("c > 0.0f", "c >= 0.0f"))], "the surface strip is wanted at curvature 0"),
    M("surface-wanted-is-the-screens", "C12", [(S_WANTED_RET, "bool panelCurveSurfaceWanted() {\n    return panelCurveWants() && !g_sStoodDown;\n}\n")],
      "the surface is wanted whenever the screen is, the identity test's one column at curvature 0 included"),
    M("surface-draws-when-not-wanted", "C12", [(S_DRAW_GUARD, "    if (!ctx || !draw) return false;\n")], "the draw does not check that the surface is wanted: it draws at curvature 0"),
    M("surface-null-context-not-refused", "C12", [(S_DRAW_GUARD, "    if (!draw || !g_sWanted) return false;\n")], "a null context reaches the draw (and stands the surface down)"),
    M("surface-null-draw-not-refused", "C12", [(S_DRAW_GUARD, "    if (!ctx || !g_sWanted) return false;\n")], "a null draw function is called (and stands the surface down)"),
    M("surface-gain-not-checked", "C12", [(S_ARG_GUARD, "    if (toward != 1 && toward != -1) return false;\n")], "a gain of 0, a negative gain, NaN or infinity is built and drawn"),
    M("surface-gain-no-upper-bound", "C12", [(S_ARG_GUARD, "    if (!(gain > 0.0f) || (toward != 1 && toward != -1)) return false;\n")], "an infinite gain is built and drawn"),
    M("surface-direction-not-checked", "C12", [(S_ARG_GUARD, "    if (!(gain > 0.0f && gain < 1.0e6f)) return false;\n")], "a direction other than +-1 is built and drawn"),
    M("surface-key-ignores-curvature", "C12", [(S_CURRENT_ALL, S_CURRENT_ALL.replace(" g_sBuiltCurvature == detail::g_panelCurveCurvature &&", ""))], "a changed curvature does not make the surface strip stale"),
    M("surface-key-ignores-columns", "C12", [(S_CURRENT_ALL, S_CURRENT_ALL.replace(" g_sBuiltSegments == detail::g_panelCurveSegments &&", ""))], "a changed column count does not make the surface strip stale"),
    M("surface-key-ignores-gain", "C12", [(S_CURRENT_ALL, S_CURRENT_ALL.replace("g_sBuiltGain == gain && ", ""))], "a changed gain does not make the surface strip stale"),
    M("surface-key-ignores-direction", "C12", [(S_CURRENT_ALL, S_CURRENT_ALL.replace(" && g_sBuiltToward == toward", ""))], "a changed direction does not make the surface strip stale"),
    M("surface-key-ignores-the-way-u-runs", "C12", [(S_CURRENT_ALL, S_CURRENT_ALL.replace(" && g_sBuiltReverseU == reverseU", ""))], "a flip of reverseU does not make the surface strip stale"),
    M("surface-always-rebuilds", "C12", [(S_REBUILD, "        if (true) {\n            if (!buildSurface(ctx, gain, toward, reverseU)) return;\n        }\n")], "the surface strip is rebuilt at every draw"),
    M("surface-direction-inverted", "C12", [(S_FILL, "    fillStrip(n, detail::g_panelCurveCurvature, toward, gain, reverseU, vb, ib);\n")], "toward +1 bends away from the viewer (the sign is not flipped)"),
    M("surface-gain-ignored", "C12", [(S_FILL, "    fillStrip(n, detail::g_panelCurveCurvature, -toward, 1.0f, reverseU, vb, ib);\n")], "the caller's gain does not reach the strip"),
    # which way u runs (the mirror fix): the flag reaches the strip, rightly oriented, is part of the key, and never touches the on-foot strip
    M("surface-u-ignores-the-flag", "C12", [(STRIP_U, "        const float u = (x + 1.0f) * 0.5f;\n")], "u always runs with x: a placement whose +x runs left is mirrored"),
    M("surface-u-flag-inverted", ("C12", "C2"), [(STRIP_U, "        const float u = reverseU ? (x + 1.0f) * 0.5f : (1.0f - x) * 0.5f;\n")], "the generator runs u against x when it is told to run it with x"),
    M("surface-u-reversed-wrongly", "C12", [(STRIP_U, "        const float u = reverseU ? (1.0f - x) : (x + 1.0f) * 0.5f;\n")], "the reversed u is 1 - x, not (1 - x)/2: it leaves 0..1"),
    M("surface-flag-inverted-at-the-build", "C12", [(S_FILL, "    fillStrip(n, detail::g_panelCurveCurvature, -toward, gain, !reverseU, vb, ib);\n")], "the surface builds the strip the other way round from the one asked for"),
    M("surface-flag-never-reaches-the-build", "C12", [(S_FILL, "    fillStrip(n, detail::g_panelCurveCurvature, -toward, gain, false, vb, ib);\n")], "the surface strip always runs u with x"),
    M("surface-key-flag-never-recorded", "C12", [(S_BUILT_REVERSE, "")], "the key never learns which way u runs: a reversed strip is rebuilt at every draw"),
    M("onfoot-u-reversed", "C2", [(ONFOOT_FILL, ONFOOT_FILL.replace("activeGain(), false,", "activeGain(), true,"))], "the screen's own strip runs u against x"),
    M("onfoot-follows-the-surfaces-flag", "C13", [(ONFOOT_FILL, ONFOOT_FILL.replace("activeGain(), false,", "activeGain(), g_sBuiltReverseU,"))], "the screen's strip picks up the way the surface strip in hand runs u"),
    M("surface-log-direction-swapped", "C13", [(S_LOG_DIR, '        reverseU ? "with x" : "against x: the placement\'s +x runs to the viewer\'s left");\n')], "the build line says u runs the way it does not"),
    M("info-reversed-never", "C12", [(S_INFO_REVERSED, "    i.reversed = false;\n")], "the info never says the strip in hand runs u against x"),
    M("info-reversed-inverted", "C12", [(S_INFO_REVERSED, "    i.reversed = g_sVb && !g_sBuiltReverseU;\n")], "the info says the opposite of the way the strip in hand runs u"),
    M("surface-build-uncounted", "C12", [(S_BUILT_COUNT, "")], "a built strip is not counted"),
    M("surface-draw-uncounted", "C12", [(S_DRAWN, "        drawn = true;\n")], "a drawn strip is not counted"),
    M("surface-draws-the-screens-strip", "C12", [(S_DRAW_TAIL, S_DRAW_TAIL.replace("drawStripHeld(ctx, draw, g_sVb, g_sIb, g_sIndexCount);", "drawStripHeld(ctx, draw);"))],
      "the surface is drawn with the screen's own strip"),
    M("surface-draws-the-screens-index-count", "C12", [(S_DRAW_TAIL, S_DRAW_TAIL.replace("g_sVb, g_sIb, g_sIndexCount);", "g_sVb, g_sIb, g_indexCount);"))], "the surface's draw takes the screen's index count"),
    M("surface-keeps-the-games-cull", "C12", [(S_RS_CULL, "            if (false) {\n")], "the strip is drawn with the game's own cull mode"),
    M("surface-swaps-a-state-that-culls-nothing", "C12", [(S_RS_CULL, "            if (true) {\n")], "a state that already culls nothing is replaced by a derived copy too"),
    M("cull-off-keeps-the-cull", "C12", [(S_CULL_OFF, "")], "the derived state is the game's, cull and all"),
    M("cull-off-drops-a-field", "C12", [(S_CULL_OFF, S_CULL_OFF + "    d.DepthBias = 0;\n")], "the derived state loses the game's depth bias"),
    M("cull-off-drops-the-winding", "C12", [(S_CULL_OFF, S_CULL_OFF + "    d.FrontCounterClockwise = FALSE;\n")], "the derived state loses the game's winding convention"),
    M("cull-off-cache-never-hits", "C12", [(S_CACHE_HIT, "        if (false) return s.off;\n")], "a derived state is created for every draw"),
    M("cull-off-cache-keyed-by-cull-only", "C12", [(S_CACHE_HIT, "        if (s.off && s.game.CullMode == game.CullMode) return s.off;\n")], "two states of the game's with the same cull share one derived copy"),
    M("cull-off-cache-keyed-by-nothing", "C12", [(S_CACHE_HIT, "        if (s.off) return s.off;\n")], "every state of the game's is served the first derived copy"),
    M("cull-off-uncounted", "C12", [(S_RS_COUNT, "")], "a derived state is not counted when it is created"),
    M("rs-not-rebound", "C12", [(S_RS_RESTORE, "    if (g_sSavedRs) { g_sSavedRs->Release(); g_sSavedRs = nullptr; }\n")], "the game's rasterizer state is not bound again after the draw"),
    M("rs-reference-leaked", "C12", [(S_RS_RESTORE, "    if (swapped) ctx->RSSetState(g_sSavedRs);\n")], "the reference taken on the game's rasterizer state is never given back"),
    M("rs-swap-not-flagged", "C12", [(S_RS_SWAP, "                ctx->RSSetState(off);\n")], "the swap is not recorded, so nothing is owed a rebind"),
    M("rs-restore-skipped", "C12", [(S_DRAW_TAIL, "        drawStripHeld(ctx, draw, g_sVb, g_sIb, g_sIndexCount);\n        restoreSaved(ctx);\n")], "the surface's draw never puts the game's rasterizer state back"),
    M("shutdown-keeps-the-cull-off-states", "C12", [(S_SHUT_CULL, "    for (CullOffState& s : g_sCullOff) {\n")], "the shutdown keeps the derived rasterizer states"),
    M("cull-off-evicted-state-leaked", "C12", [(S_EVICT, "")], "a derived state pushed out of the cache is never released"),
    M("surface-info-built-is-drawn", "C12", [(S_INFO_BUILT, "    i.built = g_sDrawn;\n")], "the info reports draws as builds"),
    M("surface-info-states-zero", "C12", [(S_INFO_STATES, "    i.rasterStates = 0;\n")], "the info never reports a derived state"),
    # ---- C13: the surface strip beside the screen's ------------------------------------------------------------------------------
    M("surface-fault-does-not-stand-down", "C13", [(S_STAND_FLAGS, "    g_sWanted = false;\n")], "a fault in the surface's draw does not stand it down for the session"),
    M("surface-fault-leaves-it-wanted", "C13", [(S_STAND_FLAGS, "    g_sStoodDown = true;\n")], "a fault stands the surface down but leaves it wanted until the next configure"),
    M("surface-fault-skips-the-restore", "C13", [(S_STAND_IA, "")], "a fault in the surface's draw leaves its strip bound in the input assembler"),
    M("surface-fault-skips-the-rs-restore", "C13", [(S_STAND_RS, "")], "a fault in the surface's draw leaves the cull-off state bound"),
    M("surface-fault-stands-the-screen-down", "C13", [(S_STAND_FLAGS, S_STAND_FLAGS + "    detail::g_panelCurveStoodDown = true;\n")], "a fault of the surface's stands the on-foot screen down too"),
    M("surface-fault-clears-the-screens-ready", "C13", [(S_STAND_FLAGS, S_STAND_FLAGS + "    detail::g_panelCurveReady = false;\n")], "a fault of the surface's clears the on-foot screen's ready flag"),
    M("screen-fault-stands-the-surface-down", "C13", [(STAND_DOWN_READY, STAND_DOWN_READY + "    g_sStoodDown = true;\n    g_sWanted = false;\n")], "a fault of the screen's stands the surface down too"),
    M("surface-fault-uses-the-screens-budget", "C13", [(S_BUDGET, "    const bool ok = guardedBudget(g_budget, [&] {\n")], "the surface's faults are charged to the screen's fault budget"),
    M("surface-fault-line-is-the-screens", "C13", [(S_STAND_LOG, '        "panel curvature: the substitution faulted, so it is off for the rest of this session and "\n')],
      "the surface's fault is logged in the screen's words"),
    M("surface-info-standdown-false", "C13", [(S_INFO_STAND, "    i.standDown = false;\n")], "the info never says the surface stood down"),
    M("shutdown-clears-the-surface-stand-down", "C13", [(S_SHUT_INDEX, "    g_sStoodDown = false;\n" + S_SHUT_INDEX)], "the shutdown brings a stood-down surface back"),
]


# ---- applying an edit ----------------------------------------------------------------------------------------------------
def apply_edits(text, edits, name="?"):
    """The text with each (old, new) applied in order; each `old` must occur exactly once in the text as it stands then."""
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise ValueError("mutation %s: an anchor occurs %d times (want 1): %r" % (name, count, old[:90]))
        if old == new:
            raise ValueError("mutation %s: an edit changes nothing: %r" % (name, old[:90]))
        text = text.replace(old, new)
    return text


def read_source(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def fail_labels(output):
    """Every check label the rig printed on a 'FAIL: C<case>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the
    rig's closing 'FAIL: panel curve: n of m checks failed' line is not a label."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(r"C\d+\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text):
    return set(re.findall(r'\{"(C\d+)", case\d+\}', rig_text))


def rig_case_has_labels(rig_text, case_id):
    return ('"%s.' % case_id) in rig_text or ('"%s"' % case_id) in rig_text


def label_block(bat_text, label):
    """The lines of one build.bat subroutine, continuation lines (a trailing ^) joined; None when the label is absent."""
    lines = bat_text.replace("\r\n", "\n").split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == label.lower() or l.lower().startswith(label.lower() + " ")), None)
    if start is None:
        return None
    out, joined = [], ""
    for line in lines[start + 1:]:
        if line.startswith(":") and not line.startswith("::"):
            break
        if line.rstrip().endswith("^"):
            joined += line.rstrip()[:-1] + " "
            continue
        out.append(joined + line)
        joined = ""
    return out


def find_gen():
    """The generated-headers directory: this checkout's build\\gen, else the main checkout's (a worktree is .claude\\worktrees\\<name>)."""
    for candidate in (ROOT / "build" / "gen", ROOT.parents[2] / "build" / "gen" if len(ROOT.parents) > 2 else None):
        if candidate and candidate.is_dir():
            return candidate
    return None


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path (Windows looks a bare name up on THIS process's PATH, not on the env passed), with the
    environment they need: this one if cl is on PATH, else the one vcvars64.bat makes, found with vswhere as build.bat does."""

    def __init__(self):
        found = shutil.which("cl.exe")
        if found:
            self.env = os.environ.copy()
        else:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
            bat = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if not vs or not bat.is_file():
                raise RuntimeError("cl.exe is not on PATH and no Visual Studio with the x64 C++ tools was found")
            dump = subprocess.run('cmd.exe /s /c ""%s" >nul && set"' % bat, capture_output=True, text=True).stdout
            self.env = {}
            for line in dump.splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    self.env[key] = value
            path = next((v for k, v in self.env.items() if k.upper() == "PATH"), "")
            found = shutil.which("cl.exe", path=path)
            if not found:
                raise RuntimeError("vcvars64.bat did not put cl.exe on PATH")
        self.cl = str(found)
        self.link = str(Path(found).with_name("link.exe"))


def compile_obj(tc, source, outdir, includes, obj_name=None):
    """Compile one source to outdir\\<obj_name, else the source's stem>.obj; (exit code, output, obj path)."""
    obj = outdir / ((obj_name or Path(source).stem) + ".obj")
    cmd = [tc.cl] + CL_FLAGS + ["/c"] + ["/I" + str(i) for i in includes] + ["/Fo" + str(obj), str(source)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr), obj


def link_exe(tc, objs, exe, outdir):
    cmd = [tc.link, "/nologo", "/INCREMENTAL:NO", "/OUT:" + str(exe)] + [str(o) for o in objs] + LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr)


def run_rig(exe, tc):
    """(outcome, labels, tail) of one run of the rig: 'pass', 'fail', 'crash', 'timeout'."""
    try:
        done = subprocess.run([str(exe), "--self-test"], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % RUN_TIMEOUT
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


# ---- the run ---------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each is compiled with the rig in a temp directory outside the repo and must fail on a check of the cases named:" % len(mutants)]
    for m in mutants:
        lines.append("  %-38s %-3s caught by %-9s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    gen = find_gen()
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in FILES.items()}
    work = Path(tempfile.mkdtemp(prefix="pcm_"))
    workers = jobs or min(8, os.cpu_count() or 2)
    try:
        # The control: the unmutated module and the rig, built the way every mutation is, and every common source compiled once.
        base_inc = ([gen] if gen else []) + [SRC]
        common_dir = work / "c"
        common_dir.mkdir()
        control_dir = work / "k"
        control_dir.mkdir()
        shutil.copyfile(FILES["h"], control_dir / "panel_curve.h")
        shutil.copyfile(MOTION_H, control_dir / "screen_motion.h")
        (control_dir / "panel_curve.cpp").write_text(sources["cpp"], encoding="utf-8", newline="\n")
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(compile_obj, tc, c, common_dir, base_inc) for c in COMMON]
            futures.append(pool.submit(compile_obj, tc, control_dir / "panel_curve.cpp", control_dir, [control_dir] + base_inc))
            futures.append(pool.submit(compile_obj, tc, RIG, common_dir, [control_dir] + base_inc, "rig"))
            built = [f.result() for f in futures]
        for code, text, obj in built:
            if code != 0:
                print("control: nocompile %s\n%s" % (obj.name, text.strip()[-1500:]), file=out)
                return 1
        common_objs = [b[2] for b in built[:len(COMMON)]]
        module_obj = built[len(COMMON)][2]
        rig_obj = common_dir / "rig.obj"
        code, text = link_exe(tc, [rig_obj, module_obj] + common_objs, control_dir / "rig.exe", control_dir)
        if code != 0:
            print("control: nolink\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(control_dir / "rig.exe", tc)
        print("control (the unmutated module, every case): %s %s" % (outcome, tail or " ".join(labels[:3])), file=out)
        if outcome != "pass":
            print("the rig does not pass on the unmutated module when built this way; nothing below means anything", file=out)
            return 1

        def one(index, m):
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            d = work / ("m%02d" % index)
            d.mkdir()
            for k in FILES:
                text = mutated if k == m.file else sources[k]
                (d / FILES[k].name).write_text(text, encoding="utf-8", newline="\n")
            shutil.copyfile(MOTION_H, d / "screen_motion.h")
            inc = [d] + base_inc
            code, text, mobj = compile_obj(tc, d / "panel_curve.cpp", d, inc)
            if code != 0:
                return m, "nocompile", (text.strip().splitlines() or [""])[-1]
            robj = rig_obj
            if m.file == "h":   # the rig compiles the header too: rebuild it against the mutated copy
                code, text, robj = compile_obj(tc, RIG, d, inc, "rig")
                if code != 0:
                    return m, "nocompile", (text.strip().splitlines() or [""])[-1]
            exe = d / "rig.exe"
            code, text = link_exe(tc, [robj, mobj] + common_objs, exe, d)
            if code != 0:
                return m, "nolink", (text.strip().splitlines() or [""])[-1]
            outcome, labels, tail = run_rig(exe, tc)
            if outcome == "fail":
                if any(l.startswith(c + ".") for l in labels for c in m.caught):
                    if verbose:
                        return m, "caught", "%d failing label(s): %s" % (len(labels), ", ".join(labels))
                    return m, "caught", "%d failing label(s), first %s" % (len(labels), labels[0])
                return m, "OTHER", "failed on %s only" % ", ".join(sorted({l.split(".")[0] for l in labels}))
            return m, {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT"}[outcome], tail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for m, verdict, detail in pool.map(lambda im: one(*im), list(enumerate(mutants))):
                results.append((m, verdict, detail))
                print("%-9s %-38s %-9s %s" % (verdict, m.name, "/".join(m.caught), detail if verbose else detail[:150]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own case, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test(build_bat=BUILD_BAT):
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(fail_labels("a\nFAIL: C4.reissue1-identical -- x [y]\nFAIL: C2.sub-bytes\nFAIL: panel curve: 3 of 9\n") == ["C4.reissue1-identical", "C2.sub-bytes"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 panel curve checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('{"C1", case1}, {"C10", case10}') == {"C1", "C10"}, "rig_cases reads the case table")

    # every mutation against the sources as they are now, and against the rig
    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 14, "the mutation list did not shrink below 14 (%d)" % len(MUTANTS))
    for m in MUTANTS:
        check(m.file in FILES, "%s edits panel_curve.%s, which this tool does not know" % (m.name, m.file))
        if m.file in FILES:
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
                check(mutated != sources[m.file], "%s changes the source" % m.name)
            except ValueError as error:
                failures.append(str(error))
        for c in m.caught:
            check(c in cases, "%s: the rig has no case %s" % (m.name, c))
            check(rig_case_has_labels(rig, c), "%s: the rig has no check labelled %s." % (m.name, c))
    covered = {c for m in MUTANTS for c in m.caught}
    check(covered == cases, "every case of the rig has a mutation, and only cases of the rig: %s vs %s" % (sorted(covered), sorted(cases)))

    # the module still has the pieces the rig stubs and the tool copies
    check(MOTION_H.is_file(), "src\\d3d11\\screen_motion.h exists (copied beside a mutated header)")
    check(find_gen() is None or find_gen().is_dir(), "the generated-headers directory, when there is one, is a directory")

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        for src in ("tools\\panel_curve_test\\panel_curve_test.cpp", "src\\d3d11\\panel_curve.cpp", "src\\common\\config.cpp", "src\\common\\log.cpp",
                    "src\\common\\guard.cpp", "src\\common\\proxy.cpp"):
            check(src in cl, "build.bat's rig compile has %s (the sources this tool links)" % src)
        check('/I"src\\d3d11"' in cl, "build.bat's rig compile finds the headers through /I src\\d3d11 (the mutated copy is found the same way)")
        for lib in LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("d3d11.lib" not in cl, "build.bat's rig does not link d3d11.lib (it takes System32's device through src\\common\\system_d3d11.h)")
        check('panel_curve_test.exe" --dry-run' in text and 'panel_curve_test.exe" --self-test' in text, "build.bat runs the rig's --dry-run and --self-test")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's --self-test")

    # --dry-run starts nothing and writes nothing
    calls = []
    real_mkdtemp, real_run = tempfile.mkdtemp, subprocess.run
    tempfile.mkdtemp = lambda *a, **k: calls.append("mkdtemp") or real_mkdtemp(*a, **k)
    subprocess.run = lambda *a, **k: calls.append("subprocess") or real_run(*a, **k)
    try:
        sink = io.StringIO()
        code = run_all(dry_run=True, out=sink)
    finally:
        tempfile.mkdtemp, subprocess.run = real_mkdtemp, real_run
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select("no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: panel_curve_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    parser.add_argument("--verbose", action="store_true", help="print every label a mutant made the rig fail on, not just the first")
    parser.add_argument("--build-bat", default=str(BUILD_BAT), help="the build.bat the self-test reads the rig's label from")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test(args.build_bat)
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        try:
            return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
        except (ValueError, RuntimeError) as error:
            print("error: %s" % error, file=sys.stderr)
            return 2
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
