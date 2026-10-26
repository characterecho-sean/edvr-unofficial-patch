#pragma once
// THE HISTORY DEPTH CHECK'S FOOTPRINT TEST, on the CPU (src/d3d11/flat_mono_shader_source.h, historyDepthMatches; design doc sections 82 and 104).
//
// historyDepthMatches asks whether last frame's depth confirms the surface a pixel shows now. It took the best of the four history-depth texels
// around the reprojected raster position and passed it within 1% (floor 1e-6) of the depth the camera term expects. On a far floor seen at a grazing
// angle that point test has an error floor: the depth of a plane is affine in screen space, so the nearest texel, up to half a pixel of jitter from the
// exact position, is off by about delta / y of the depth (y: rows below the horizon), and a STILL camera failed the 1% test in a band about 33 rows
// below the horizon at 960x600 (flight edvr_gfx_20261022_044106: 1.64% of the frame stale-refused with the camera not moving). The check now also
// passes a footprint of four texels that is planar (the cross difference d00 + d11 - d10 - d01 within the tolerance) and whose bilinear value at the
// exact position is within the tolerance of the expected depth. flat_history_depth_model.h is that decision in float32.
//
// What runs here (the GPU rig, tools\flat_mono_resolve_test\flat_taa_history_depth_gpu_tests.h, runs the same scenes through the real kernel):
//   1. the HLSL's text, pinned to the model statement by statement, and the constants to flat_mono_refusal.h's;
//   2. single footprints, built by hand: a plane at the tolerance's edges (the tolerance, the floor, the cross difference's 1 x tolerance), and each
//      reason to ask nothing (a zero texel, a non-finite one, a footprint outside the image), a checker, a roof corner, and the step that reads as a slope;
//   3. a 960x600 frame under the shipped 32-phase Halton cycle (every consecutive pair): a still floor near its horizon (flat and rolled) kept in full
//      where the point test refused a band of it; the borders, the horizon row and a sloped roof's edge against the sky decided as before; a near object
//      that moved away still refused; a surface 2% deeper last frame (a mover receding 2% a frame) still refused, one 0.9% deeper kept;
//   4. the 16x16 scenes the GPU rig runs, each held to what it is for and to catching each mutant of the rule the rig is held to.
// Every mutant of the rule (a Rule with a field changed) must fail one of 2-4 or one of flat_history_junction_tests.h's (the junction test, the third form of
// the check: its text, the camera math it needs, its single footprints and its 960x600 scenes), and every rule-level mutant the GPU rig runs must change the
// prediction of one of the GPU scenes. The cases here run with the junction test on, as shipped: the scenes of 3 and 4 are the ones the footprint test was
// written for, and it must leave their answers as they were (a still floor is kept in full; the object that moved away, the sky and the movers are not).
//
// Geometry. The frames (3) model the prep's call: the position handed to the check is the previous position of the raster texel's own surface point (its
// true position, q + .5 - c, run through the camera term), and `expected` is that same point's depth, so a still plane's history is exactly bilinear.
// taa() hands the check the previous position of the OUTPUT pixel's centre (uv, not uv - c), with the raster texel's `expected`: one current phase apart
// (more when the render is smaller than the output). At current phase 0 the two coincide, which is the GPU rig's setup (4); at other phases the
// footprint test is exact only to slope * |c|, and the model shows it keeping 79% of the same floor taa() would keep 100% of if it passed the texel's
// own position.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "../../src/d3d11/flat_mono_refusal.h"
#include "../../src/d3d11/flat_mono_shader_source.h"
#include "flat_history_depth_model.h"
#include "flat_history_junction_tests.h"

namespace flat_history_depth {
using histdepth::Image;
using histdepth::Rule;
using histdepth::Verdict;
using histdepth::pointOnly;

constexpr int W = 960, H = 600;
constexpr unsigned kCycle = 32;

struct Sink {
    int failures = 0;
    bool narrate = false;   // print the numbers this run measures
    bool quiet = false;     // a run of a mutant: count the failures, print none
    const char* section = "";
    void expect(bool ok, const char* what) {
        if (!ok) {
            if (!quiet) std::printf("FAIL: history depth %s: %s\n", section, what);
            ++failures;
        }
    }
    void expect(bool ok, const std::string& what) { expect(ok, what.c_str()); }
};

// ---- 1. the shader's text -------------------------------------------------------------------------------------------------------------------
// The statements of historyDepthMatches the model mirrors, in order; each must appear once, after the one before it, and nothing else in the source
// may read HistoryDepth (a second reader would be a second rule).
inline const std::vector<std::string>& pins() {
    static const std::vector<std::string> p = {
        "bool historyDepthMatches(float2 prev,float expected) {",
        "const int2 hi=int2(size.xy)-1;",
        "const float2 raster=prev*float2(size.xy)+jitter.zw-.5;",
        "const int2 base=int2(floor(raster));",
        "const float tol=max(kStaleDepthFloor,expected*kStaleDepthRel);",
        "float best=3.402823e38;",
        "[unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));",
        "if(best<=tol)return true;",
        "if(any(base<0) || any(base>=hi))return false;",
        "const float d00=HistoryDepth.Load(int3(base,0)), d10=HistoryDepth.Load(int3(base+int2(1,0),0));",
        "const float d01=HistoryDepth.Load(int3(base+int2(0,1),0)), d11=HistoryDepth.Load(int3(base+int2(1,1),0));",
        "const float4 footprint=float4(d00,d10,d01,d11);",
        "if(!all(isfinite(footprint)) || !all(footprint>0))return false;",
        "const float2 at=raster-float2(base);",
        "const float planeDepth=lerp(lerp(d00,d10,at.x),lerp(d01,d11,at.x),at.y);",
        "if(abs(planeDepth-expected)<=tol && abs(d00+d11-d10-d01)<=tol)return true;",
        "return junctionPersists(base,raster,footprint,expected,tol);\n}",
    };
    return p;
}
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}
// Empty when the source holds the rule; otherwise the first thing it does not hold.
inline std::string pinFailure(const std::string& source) {
    const size_t first = source.find("bool historyDepthMatches(");
    const std::string tail = "return junctionPersists(base,raster,footprint,expected,tol);\n}";
    const size_t last = source.find(tail);
    if (first == std::string::npos || last == std::string::npos || last < first) return "the function's end is not where the model ends it";
    const std::string fn = source.substr(first, last + tail.size() - first);
    size_t from = 0;
    for (const std::string& pin : pins()) {
        if (countOf(fn, pin) != 1) return "not exactly once in the function: " + pin;
        const size_t at = fn.find(pin);
        if (at < from) return "out of order: " + pin;
        from = at;
    }
    // 1 (the point test's) + 4 (the footprint's) reads inside the function, and the 4 of the junction test's quad (historyQuad, pinned in
    // flat_history_junction_tests.h).
    if (countOf(source, "HistoryDepth.Load(") != 9 || countOf(fn, "HistoryDepth.Load(") != 5) return "HistoryDepth is read somewhere else, or not by every texel";
    return std::string();
}

inline void textPins(Sink& t) {
    t.section = "text";
    const std::string source = edvr::kFlatMonoShaderSource;
    const std::string failure = pinFailure(source);
    t.expect(failure.empty(), "the shader's historyDepthMatches is the statements the model mirrors: " + failure);
    t.expect(histdepth::kRel == float(edvr::kFlatMonoStaleDepthRelative) && histdepth::kFloor == float(edvr::kFlatMonoStaleDepthFloor),
             "the model's tolerance (1%, floor 1e-6) is flat_mono_refusal.h's");
    t.expect(source.find("if(valid && kind==3)valid=historyDepthMatches(prev,expected);") != std::string::npos &&
                 source.find("if(historyDepthMatches(previous,ExpectedDepth.Load(int3(q,0))))weight=.9;") != std::string::npos,
             "both callers (the prep's stale slot, taa()) ask the one check and take its answer unchanged");
    // The pin has teeth: each statement removed or altered is found.
    const char* const edits[][2] = {
        {"if(any(base<0) || any(base>=hi))return false;", ""},
        {"if(any(base<0) || any(base>=hi))return false;", "if(any(base<0) || any(base>hi))return false;"},
        {"if(!all(isfinite(footprint)) || !all(footprint>0))return false;", "if(!all(isfinite(footprint)))return false;"},
        {"if(!all(isfinite(footprint)) || !all(footprint>0))return false;", "if(!all(footprint>0))return false;"},
        {"lerp(lerp(d00,d10,at.x),lerp(d01,d11,at.x),at.y)", "(d00+d10+d01+d11)*.25"},
        {"lerp(lerp(d00,d10,at.x),lerp(d01,d11,at.x),at.y)", "lerp(lerp(d00,d10,at.y),lerp(d01,d11,at.y),at.x)"},
        {" && abs(d00+d11-d10-d01)<=tol)return true;", ")return true;"},
        {"abs(d00+d11-d10-d01)<=tol", "abs(d00+d11-d10-d01)<=8.0*tol"},
        {"return junctionPersists(base,raster,footprint,expected,tol);", "return false;"},
        {"return junctionPersists(base,raster,footprint,expected,tol);", "return junctionPersists(base,raster,footprint,expected,2.0*tol);"},
        {"abs(planeDepth-expected)<=tol", "abs(planeDepth-expected)<=2.0*tol"},
        {"if(best<=tol)return true;", "if(best<=tol)return true;\n    if(best<=tol)return true;"},
        {"const float2 at=raster-float2(base);", "const float2 at=float2(.5,.5);"},
    };
    for (const auto& e : edits) {
        std::string edited = source;
        const size_t at = edited.find(e[0]);
        t.expect(at != std::string::npos, std::string("the pin's mutation anchor is in the source: ") + e[0]);
        if (at == std::string::npos) continue;
        edited.replace(at, std::strlen(e[0]), e[1]);
        t.expect(!pinFailure(edited).empty(), std::string("the text pins catch: ") + e[0] + " -> " + e[1]);
    }
}

// ---- 2. single footprints -------------------------------------------------------------------------------------------------------------------
// An 8x8 history with no phase: the sample position (rx, ry) is in texel-centre coordinates (texel (3, 2) is at (3, 2)).
inline Verdict at(const Image& m, float rx, float ry, float expected, const Rule& rule) {
    return histdepth::decide(m, (rx + 0.5f) / float(m.w), (ry + 0.5f) / float(m.h), 0, 0, expected, rule);
}
template <class F>
inline Image make(F depth) {
    Image m(8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) m.at(x, y) = depth(float(x), float(y));
    return m;
}
inline bool kept(const Image& m, float rx, float ry, float expected, const Rule& rule) { return at(m, rx, ry, expected, rule).kept; }

inline void footprints(Sink& t, const Rule& r) {
    t.section = "footprint";
    const Rule old = pointOnly();
    const auto expectKept = [&](const Image& m, float rx, float ry, float expected, bool want, const char* what) {
        t.expect(kept(m, rx, ry, expected, r) == want, what);
    };
    // A steep plane (20% a texel in x, 50% in y): the nearest texel is far from the exact position, the bilinear value is exact.
    const auto plane = [](float x, float y) { return 0.01f * (1.0f + 0.2f * x + 0.5f * y); };
    const Image p = make(plane);
    {
        const float rx = 3.5f, ry = 2.5f, exact = plane(rx, ry);
        t.expect(!kept(p, rx, ry, exact, old), "a plane: the nearest texel is more than 1% off, so the point test alone refuses it");
        expectKept(p, rx, ry, exact, true, "a plane: its depth at the exact position is confirmed by the footprint");
        expectKept(p, rx, ry, exact * 1.009f, true, "a plane, the expected depth 0.9% deeper: inside the tolerance, kept");
        expectKept(p, rx, ry, exact / 1.009f, true, "a plane, 0.9% nearer: kept");
        expectKept(p, rx, ry, exact * 1.011f, false, "a plane, 1.1% deeper: another surface, refused");
        expectKept(p, rx, ry, exact / 1.011f, false, "a plane, 1.1% nearer: refused");
        expectKept(p, rx, ry, exact * 1.02f, false, "a plane, 2% deeper (a mover receding): refused");
        expectKept(p, rx, ry, 0.0f, false, "a plane where the pixel shows the sky: refused");
        // Another position and each corner of the fraction square.
        for (float fx : {0.05f, 0.5f, 0.95f})
            for (float fy : {0.05f, 0.5f, 0.95f}) {
                char what[160];
                std::snprintf(what, sizeof(what), "a plane at fraction (%.2f, %.2f) is confirmed at its exact depth", fx, fy);
                expectKept(p, 2.0f + fx, 4.0f + fy, plane(2.0f + fx, 4.0f + fy), true, what);
            }
    }
    {   // The floor: depth around 1e-5, where 1e-6 is wider than 1%.
        const auto distant = [](float x, float) { return 1e-5f * (1.0f + x); };
        const Image f = make(distant);
        const float rx = 3.5f, exact = distant(rx, 0.0f);
        t.expect(!kept(f, rx, 3.0f, exact, old), "far away: the texels are 5e-6 off, outside the floor, so the point test refuses");
        expectKept(f, rx, 3.0f, exact + 0.9e-6f, true, "far away, 0.9e-6 off the exact depth: inside the 1e-6 floor, kept");
        expectKept(f, rx, 3.0f, exact - 0.9e-6f, true, "far away, 0.9e-6 nearer: kept");
        expectKept(f, rx, 3.0f, exact + 1.2e-6f, false, "far away, 1.2e-6 off: outside the floor, refused");
    }
    {   // The cross difference: a plane plus t * x * y, whose four texels' cross difference is exactly t, the bilinear value exact.
        const float expected = plane(3.5f, 2.5f), tol = expected * 0.01f;
        for (float scale : {0.9f, 1.1f, 3.0f}) {
            const float twist = scale * tol;
            const Image m = make([&](float x, float y) { return plane(x, y) + twist * (x - 3.0f) * (y - 2.0f); });
            // At (3.5, 2.5) the bilinear value of (plane + twist * u * v) is plane + twist / 4.
            const float exactHere = expected + twist * 0.25f;
            char what[200];
            std::snprintf(what, sizeof(what), "a footprint whose cross difference is %.1f x the tolerance, its bilinear value exact: %s", scale,
                          scale < 1 ? "kept" : "refused");
            expectKept(m, 3.5f, 2.5f, exactHere, scale < 1, what);
        }
    }
    {   // A checker (depths 3% apart) and a surface whose depth is its interpolation: cross difference 6 x the tolerance.
        const float a = 0.0100f, b = 0.0103f;
        const Image m = make([&](float x, float y) { return (int(x) + int(y)) & 1 ? b : a; });
        t.expect(!kept(m, 2.5f, 2.5f, 0.5f * (a + b), old), "a checker: the interpolation is more than 1% from both depths, so the point test refuses");
        expectKept(m, 2.5f, 2.5f, 0.5f * (a + b), false, "a checker, a surface at the interpolated depth: not planar, refused");
        expectKept(m, 2.5f, 2.5f, a, true, "a checker, a surface at one of its depths: the point test keeps it");
    }
    {   // A roof's corner: three texels of the roof (near, 10% deeper than the ground) and one of the ground. The bilinear value at the centre is the
        // interpolation; the cross difference is the whole step.
        const float ground = 0.0100f, roof = 0.0110f;
        const Image m = make([&](float x, float y) { return (int(x) == 3 && int(y) == 2) ? ground : roof; });
        expectKept(m, 3.5f, 2.5f, 0.25f * (ground + 3.0f * roof), false, "a roof's corner: the cross difference is the step, refused though the interpolation is exact");
        expectKept(m, 3.5f, 2.5f, roof, true, "a roof's corner, a surface at the roof's depth: the point test keeps it");
    }
    {   // A step along one axis is, over one footprint, indistinguishable from a slope: only a surface at the interpolated depth is confirmed. The
        // depth of either side is the point test's, anything else is refused.
        const Image m = make([](float, float y) { return y >= 3.0f ? 0.02f : 0.01f; });
        expectKept(m, 3.5f, 2.5f, 0.015f, true, "a step along one axis, a surface at the interpolated depth: the slope reading, kept");
        expectKept(m, 3.5f, 2.5f, 0.010f, true, "a step along one axis, a surface at one side's depth: kept by the point test");
        expectKept(m, 3.5f, 2.5f, 0.017f, false, "a step along one axis, a surface at neither side nor between: refused");
        expectKept(m, 3.5f, 2.5f, 0.030f, false, "a step along one axis, a surface nearer than both: refused");
    }
    {   // A zero texel (the sky, a cleared texel) bars the footprint even where the plane is consistent with it: a horizon on a texel row.
        const Image m = make([](float, float y) { return 0.001f * y; });
        t.expect(!kept(m, 3.5f, 0.5f, 0.0005f, old), "a horizon on a texel row: the point test refuses the pixel half a row below it");
        expectKept(m, 3.5f, 0.5f, 0.0005f, false, "a horizon on a texel row: a zero texel is not asked, refused");
        expectKept(m, 3.5f, 1.5f, 0.0015f, true, "the same plane one row lower, every texel non-zero: kept");
        Image sky = p;
        sky.at(4, 3) = 0.0f;
        expectKept(sky, 3.5f, 2.5f, plane(3.5f, 2.5f), false, "a plane with one sky texel in the footprint: refused");
        Image cleared = p;
        cleared.at(3, 2) = -0.0f;
        expectKept(cleared, 3.5f, 2.5f, plane(3.5f, 2.5f), false, "a plane with a negative-zero texel: refused");
    }
    {   // Non-finite texels.
        for (float bad : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            Image m = p;
            m.at(3, 3) = bad;
            expectKept(m, 3.5f, 2.5f, plane(3.5f, 2.5f), false, "a plane with a non-finite texel in the footprint: refused");
        }
    }
    {   // A footprint outside the image: the edge texels the clamp repeats look like a plane in the other axis, and are not asked.
        const auto floorOnly = [](float, float y) { return 0.01f * (1.0f + 0.3f * y); };
        const Image m = make(floorOnly);
        const float expectedY = floorOnly(0, 2.6f);
        expectKept(m, 3.0f, 2.6f, expectedY, true, "a floor, the footprint inside: kept");
        for (float rx : {-0.4f, 7.3f}) {
            t.expect(!kept(m, rx, 2.6f, expectedY, old), "a floor at the image's side: the point test refuses it");
            char what[200];
            std::snprintf(what, sizeof(what), "a floor at the side of the image (x = %.1f): the footprint leaves the image, not asked, refused", rx);
            expectKept(m, rx, 2.6f, expectedY, false, what);
        }
        const auto wall = [](float x, float) { return 0.01f * (1.0f + 0.3f * x); };
        const Image n = make(wall);
        for (float ry : {-0.4f, 7.3f}) {
            t.expect(!kept(n, 2.6f, ry, wall(2.6f, 0), old), "a wall at the image's top or bottom: the point test refuses it");
            char what[200];
            std::snprintf(what, sizeof(what), "a wall at the top or bottom of the image (y = %.1f): not asked, refused", ry);
            expectKept(n, 2.6f, ry, wall(2.6f, 0), false, what);
        }
        // The last footprint that is wholly inside is asked: base = 6 of 8 texels.
        expectKept(m, 6.4f, 2.6f, expectedY, true, "a floor whose footprint is the last two columns (6, 7): inside, kept");
        expectKept(m, 0.0f, 2.6f, expectedY, true, "a floor whose footprint is the first two columns (0, 1): inside, kept");
    }
}

// ---- 3. a 960x600 frame under the shipped cycle ----------------------------------------------------------------------------------------------
using Surface = float (*)(float, float);

struct Tally {
    long pixels = 0;          // pixels examined (the current pixel shows a surface)
    long solid = 0;           // ... whose footprint is inside the image with four non-zero texels
    long oldKept = 0, newKept = 0;
    long solidOldKept = 0, solidNewKept = 0;
    long open = 0;            // ... whose footprint is not solid
    long openDiffers = 0;     // ... where the rule and the point test disagree
    long openOldRefused = 0;
    long differs = 0;         // ... of any footprint, where the rule and the point test disagree
    long byFootprint = 0;     // kept by the footprint test
};

// Every consecutive pair of the cycle: last frame (phase p, the previous index) and this frame (phase c). `before` is the surface last frame, `now` this
// frame's; the camera is still, so a pixel's previous position is itself and its expected depth is `now` there. visit(shipped, point) per pixel that shows
// a surface now.
template <class Before, class Now, class Visit>
inline void sweep(Before before, Now now, int x0, int y0, int x1, int y1, const Rule& rule, Visit visit) {
    const Rule old = pointOnly();
    const int fx0 = std::max(0, x0 - 2), fx1 = std::min(W, x1 + 3), fy0 = std::max(0, y0 - 2), fy1 = std::min(H, y1 + 3);
    Image hist(W, H), cur(W, H);   // each phase rewrites the region the pixels read; nothing outside it is read
    for (unsigned n = 0; n < kCycle; ++n) {
        float cx, cy, px, py;
        histdepth::rig16::phaseOf(n, &cx, &cy);
        histdepth::rig16::phaseOf(n + kCycle - 1, &px, &py);
        for (int y = fy0; y < fy1; ++y)
            for (int x = fx0; x < fx1; ++x) {
                hist.at(x, y) = before(float(x) + 0.5f - px, float(y) + 0.5f - py);
                cur.at(x, y) = now(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
            }
        // The junction test reads this frame's depth and the cameras: the camera is still.
        histdepth::CurrentFrame frame;
        frame.depth = &cur;
        frame.now = frame.old = histdepth::stillCamera();
        frame.jx = cx; frame.jy = cy;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const float tx = float(x) + 0.5f - cx, ty = float(y) + 0.5f - cy;
                const float expected = now(tx, ty);
                if (!(expected > 0)) continue;
                const float u = tx / float(W), v = ty / float(H);
                visit(y, histdepth::decide(hist, u, v, px, py, expected, rule, &frame), histdepth::decide(hist, u, v, px, py, expected, old));
            }
    }
}

inline void account(Tally& t, const Verdict& shipped, const Verdict& point) {
    ++t.pixels;
    t.oldKept += point.kept;
    t.newKept += shipped.kept;
    t.byFootprint += shipped.byFootprint;
    t.differs += shipped.kept != point.kept;
    if (shipped.solid) {
        ++t.solid;
        t.solidOldKept += point.kept;
        t.solidNewKept += shipped.kept;
    } else {
        ++t.open;
        t.openDiffers += shipped.kept != point.kept;
        t.openOldRefused += !point.kept;
    }
}

// The floors: d = 2e-4 per row below the horizon, so one row below it is a slope of 100% a pixel, 20 rows 5%, 60 rows 1.7%.
inline float floorFlat(float, float yt) { return yt > 250.0f ? 2e-4f * (yt - 250.0f) : 0.0f; }
inline float floorRolled(float xt, float yt) {
    const float d = (yt - 250.0f) + 0.25f * (xt - 480.0f);
    return d > 0 ? 2e-4f * d : 0.0f;
}
// A plane with a slope toward each of two corners (about 5% a pixel at the corner): the borders' test.
inline float cornerTopLeft(float xt, float yt) { return 2e-4f * (xt + yt + 20.0f); }
inline float cornerBottomRight(float xt, float yt) { return 2e-4f * ((float(W) - xt) + (float(H) - yt) + 20.0f); }
// A sloped roof (the same 2e-4 a row, from row 80) against the sky, its edge the line a * x + b * y = e.
inline float roofEdge(float xt, float yt, float a, float b, float e) { return (a * xt + b * yt >= e && yt > 80.0f) ? 2e-4f * (yt - 80.0f) : 0.0f; }
constexpr float kNearObject = 0.05f;   // 3-5 times the floor's depth where it stands: a near object
inline float floorWithObject(float xt, float yt) {
    if (xt >= 400.0f && xt < 460.0f && yt >= 290.0f && yt < 320.0f) return kNearObject;
    return floorFlat(xt, yt);
}
template <int Permille>
inline float floorScaled(float xt, float yt) { return floorFlat(xt, yt) * (1.0f + 0.001f * float(Permille)); }

inline void frames(Sink& t, const Rule& r) {
    t.section = "frame";
    char what[256];
    // 3a. The still floor near its horizon, flat and rolled.
    {
        Tally all, band;
        sweep(floorFlat, floorFlat, 0, 252, W, 340, r, [&](int y, const Verdict& s, const Verdict& p) {
            account(all, s, p);
            if (y >= 265 && y <= 275) account(band, s, p);   // 15 to 25 rows below the horizon: 6.7% to 4% a pixel
        });
        t.expect(all.solid > 0 && all.solidNewKept == all.solid, "a still floor, rows 252-339, every pixel whose footprint is inside the frame and non-zero is kept");
        t.expect(band.solid > 0 && band.solidNewKept == band.solid, "a still floor, the 4%-6.7% band, every pixel with a solid footprint is kept");
        t.expect(all.solidOldKept < all.solid && band.solidOldKept < band.solid, "a still floor: the point test alone refuses some of it (the shimmer)");
        t.expect(all.openDiffers == 0, "a still floor: a pixel whose footprint is not solid is decided by the point test alone");
        t.expect(all.openOldRefused > 0, "a still floor: the frame's borders are part of the test (some are refused by the point test)");
        if (t.narrate)
            std::printf("flat history depth: still floor 960x600, 32 phase pairs, rows 252-339: the point test kept %.2f%% of the pixels, with the footprint test %.2f%% "
                        "(the rest are the first and last columns, whose footprint leaves the frame, decided as before); of the pixels with a solid footprint "
                        "%.2f%% -> %.2f%%; in the 15-25 rows below the horizon (6.7%%-4%% a pixel) %.2f%% -> %.2f%%\n",
                        100.0 * all.oldKept / all.pixels, 100.0 * all.newKept / all.pixels, 100.0 * all.solidOldKept / all.solid,
                        100.0 * all.solidNewKept / all.solid, 100.0 * band.solidOldKept / band.solid, 100.0 * band.solidNewKept / band.solid);
    }
    {
        Tally all;
        sweep(floorRolled, floorRolled, 0, 130, W, 400, r, [&](int, const Verdict& s, const Verdict& p) { account(all, s, p); });
        t.expect(all.solid > 0 && all.solidNewKept == all.solid, "a still floor with a rolled horizon (slope along both axes), every pixel with a solid footprint is kept");
        t.expect(all.solidOldKept < all.solid, "a rolled floor: the point test alone refuses some of it");
        t.expect(all.openDiffers == 0 && all.openOldRefused > 0, "a rolled floor: footprints holding the sky or leaving the frame are decided by the point test alone");
        if (t.narrate)
            std::printf("flat history depth: rolled floor, rows 130-399: of the pixels with a solid footprint the point test kept %.2f%%, with the footprint test %.2f%%\n",
                        100.0 * all.solidOldKept / all.solid, 100.0 * all.solidNewKept / all.solid);
    }
    // 3b. The borders: a plane that slopes toward two corners.
    {
        Tally tl, br;
        sweep(cornerTopLeft, cornerTopLeft, 0, 0, 48, 48, r, [&](int, const Verdict& s, const Verdict& p) { account(tl, s, p); });
        sweep(cornerBottomRight, cornerBottomRight, W - 48, H - 48, W, H, r, [&](int, const Verdict& s, const Verdict& p) { account(br, s, p); });
        t.expect(tl.open > 0 && tl.openOldRefused > 0 && tl.openDiffers == 0, "the top-left corner: a pixel whose footprint leaves the frame is decided as before (the point test refused some)");
        t.expect(br.open > 0 && br.openOldRefused > 0 && br.openDiffers == 0, "the bottom-right corner: a pixel whose footprint leaves the frame is decided as before");
        t.expect(tl.solid > 0 && tl.solidNewKept == tl.solid && br.solid > 0 && br.solidNewKept == br.solid, "the corners: every pixel with a footprint inside the frame is kept");
    }
    // 3c. A sloped roof's edge against the sky: a vertical edge, a diagonal one and the roof's top edge. Last frame's edge was 2.5 units further along
    // its normal, so the pixels between show the roof where last frame held the sky.
    {
        struct Edge { const char* name; float a, b, e; int x0, y0, x1, y1; };
        static const Edge edges[] = {
            {"a vertical edge", 1.0f, 0.0f, 500.0f, 480, 120, 540, 200},
            {"a diagonal edge", 1.0f, 0.5f, 600.0f, 480, 120, 560, 200},
            {"the roof's top edge", 0.0f, 1.0f, 130.0f, 560, 118, 640, 150},
        };
        for (const Edge& e : edges) {
            Tally all;
            long edgeRefused = 0;
            const auto now = [&](float x, float y) { return roofEdge(x, y, e.a, e.b, e.e); };
            const auto then = [&](float x, float y) { return roofEdge(x, y, e.a, e.b, e.e + 2.5f); };
            sweep(then, now, e.x0, e.y0, e.x1, e.y1, r, [&](int, const Verdict& s, const Verdict& p) {
                account(all, s, p);
                if (!s.solid) edgeRefused += !s.kept;
            });
            std::snprintf(what, sizeof(what), "%s against the sky: a pixel whose footprint holds the sky is decided by the point test alone", e.name);
            t.expect(all.open > 0 && all.openDiffers == 0, what);
            std::snprintf(what, sizeof(what), "%s against the sky: the pixels whose last-frame footprint is sky are refused (some)", e.name);
            t.expect(edgeRefused > 0, what);
            std::snprintf(what, sizeof(what), "%s against the sky: every pixel with a solid footprint is kept", e.name);
            t.expect(all.solid > 0 && all.solidNewKept == all.solid, what);
        }
    }
    // 3d. A near object that moved away, revealing the floor behind it: a pixel whose footprint reaches the object keeps what the point test decided.
    {
        Tally all;
        long reach = 0, reachKept = 0, reachDiffers = 0;
        sweep(floorWithObject, floorFlat, 380, 270, 480, 340, r, [&](int, const Verdict& s, const Verdict& p) {
            account(all, s, p);
            if (s.highest >= kNearObject) {
                ++reach;
                reachKept += s.kept;
                reachDiffers += s.kept != p.kept;
            }
        });
        t.expect(reach > 0 && reachKept < reach, "a near object that moved away: the pixels over where it was are refused (some)");
        t.expect(reachDiffers == 0, "a near object that moved away: a pixel whose footprint holds the object is decided as the point test did");
        t.expect(all.solid > reach && all.byFootprint > 0, "a near object that moved away: the floor around it is kept by the footprint test");
        if (t.narrate)
            std::printf("flat history depth: near object that moved away: %ld pixel-frames reach the object, %ld kept (all by the point test, as before), %ld differ\n",
                        reach, reachKept, reachDiffers);
    }
    // 3e. A mover receding 2% a frame (last frame the floor was 2% deeper there), approaching 2%, and ones either side of the tolerance.
    {
        struct Mover { const char* name; Surface then; bool kept; };
        static const Mover movers[] = {
            {"2% deeper last frame (a mover receding 2% a frame)", [](float x, float y) { return floorScaled<20>(x, y); }, false},
            {"2% nearer last frame (a mover approaching 2% a frame)", [](float x, float y) { return floorScaled<-20>(x, y); }, false},
            {"1.1% deeper last frame", [](float x, float y) { return floorScaled<11>(x, y); }, false},
            {"0.9% deeper last frame (inside the tolerance)", [](float x, float y) { return floorScaled<9>(x, y); }, true},
            {"0.9% nearer last frame (inside the tolerance)", [](float x, float y) { return floorScaled<-9>(x, y); }, true},
        };
        for (const Mover& m : movers) {
            Tally all;
            sweep(m.then, floorFlat, 252, 252, 700, 340, r, [&](int, const Verdict& s, const Verdict& p) { account(all, s, p); });
            if (!m.kept) {
                std::snprintf(what, sizeof(what), "a floor %s: the footprint test keeps no pixel the point test refused", m.name);
                t.expect(all.pixels > 0 && all.differs == 0, what);
                std::snprintf(what, sizeof(what), "a floor %s: the point test refuses some (it is a test)", m.name);
                t.expect(all.newKept < all.pixels, what);
            } else {
                std::snprintf(what, sizeof(what), "a floor %s: every pixel with a solid footprint is kept", m.name);
                t.expect(all.solid > 0 && all.solidNewKept == all.solid, what);
            }
        }
    }
}

// ---- 4. the GPU rig's scenes ----------------------------------------------------------------------------------------------------------------
// A pixel that shows the sky now has no camera term in the kernel (the prep refuses it), so the rig compares only the pixels that show a surface.
inline size_t differences(const histdepth::rig16::Scene& s, const Rule& a, const Rule& b) {
    const auto va = histdepth::rig16::predict(s, a), vb = histdepth::rig16::predict(s, b);
    size_t d = 0;
    for (size_t i = 0; i < va.size(); ++i)
        if (s.now[i] > 0 && va[i].margin >= 2e-3f && vb[i].margin >= 2e-3f && va[i].kept != vb[i].kept) ++d;
    return d;
}

inline void rigScenes(Sink& t, const Rule& r) {
    t.section = "rig scenes";
    size_t count = 0;
    const histdepth::rig16::Plan* plan = histdepth::rig16::plan(&count);
    char what[256];
    const Rule old = pointOnly();
    for (size_t i = 0; i < count; ++i) {
        const histdepth::rig16::Scene s = histdepth::rig16::build(plan[i]);
        const auto v = histdepth::rig16::predict(s, r), p = histdepth::rig16::predict(s, old);
        size_t shown = 0, ambiguous = 0, solid = 0, solidKept = 0, openDiffers = 0, reachDiffers = 0, byFootprint = 0, pointKept = 0, newKept = 0;
        size_t byJunction = 0, ghosts = 0, ghostsKept = 0, reachedRefused = 0;
        for (size_t k = 0; k < v.size(); ++k) {
            if (!(s.now[k] > 0)) continue;
            ++shown;
            ambiguous += v[k].margin < 2e-3f;
            pointKept += p[k].kept;
            newKept += v[k].kept;
            byFootprint += v[k].byFootprint;
            byJunction += v[k].byJunction;
            reachedRefused += v[k].own >= 0 && !v[k].kept;
            // The hull that moved 2 columns right: the pixels whose centre it covered and no longer covers (columns 5 and 6, rows 5..9) show the floor now.
            if (plan[i].kind == 'g' && (k % size_t(histdepth::rig16::N) == 5 || k % size_t(histdepth::rig16::N) == 6) && k / size_t(histdepth::rig16::N) >= 5 && k / size_t(histdepth::rig16::N) <= 9) { ++ghosts; ghostsKept += v[k].kept; }
            const bool reachesObject = plan[i].kind == 'o' && v[k].highest >= 0.04f;
            if (reachesObject) reachDiffers += v[k].kept != p[k].kept;
            else if (v[k].solid) { ++solid; solidKept += v[k].kept; }
            else openDiffers += v[k].kept != p[k].kept;
        }
        std::snprintf(what, sizeof(what), "GPU scene %zu (%s, previous phase index %u, (%+.3f, %+.3f)): no more than 8%% of its pixels are within 0.2%% of a threshold", i, s.name, plan[i].n, s.px, s.py);
        t.expect(ambiguous * 12 < shown, what);
        std::snprintf(what, sizeof(what), "GPU scene %zu (%s): the previous phase is not within 0.03 px of a texel centre", i, s.name);
        t.expect(std::fabs(s.px) >= 0.03f && std::fabs(s.py) >= 0.03f, what);
        switch (plan[i].kind) {
            case 'f': case 't':
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): every pixel with a solid footprint is kept, and the point test alone refuses some", i, s.name);
                t.expect(solid > 0 && solidKept == solid && pointKept + 8 <= newKept, what);
                break;
            case 'm':
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): the footprint test keeps nothing, and the point test refuses most of it", i, s.name);
                t.expect(byFootprint == 0 && pointKept * 2 < shown, what);
                break;
            case 'o': case 'e': case 'h':
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): a pixel whose footprint holds the sky or the object is decided as the point test did; the rest is kept, "
                                                  "and the point test alone refuses more than the rule", i, s.name);
                t.expect(openDiffers == 0 && reachDiffers == 0 && solid > 0 && solidKept == solid && pointKept < newKept, what);
                break;
            case 'w':
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): the junction test keeps a row of pixels the footprint test refused (%zu)", i, s.name, byJunction);
                t.expect(byJunction >= 8, what);
                break;
            case 'p':
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): the junction test keeps the floor at the hull's edges the footprint test refused (%zu)", i, s.name, byJunction);
                t.expect(byJunction >= 3, what);
                break;
            case 'g':
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): the %zu pixels the hull uncovered are all refused (%zu kept), and the ghost guard is reached (%zu pixels have their own surface and are refused by half b)",
                              i, s.name, ghosts, ghostsKept, reachedRefused);
                t.expect(ghosts == 10 && ghostsKept == 0 && reachedRefused >= 4, what);
                break;
            case 'i': {
                const auto nf = histdepth::rig16::predict(s, [] { Rule x; x.footprint = false; return x; }());
                size_t withoutFootprint = 0;
                for (size_t k = 0; k < nf.size(); ++k) withoutFootprint += nf[k].kept && s.now[k] > 0;
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): the footprint test keeps a pixel no quad beside it can (%zu kept by the footprint test, %zu kept without it)", i, s.name, byFootprint, withoutFootprint);
                t.expect(byFootprint >= 1 && withoutFootprint < newKept, what);
                break;
            }
            default:   // 'c'
                std::snprintf(what, sizeof(what), "GPU scene %zu (%s): the footprint test keeps nothing (not planar)", i, s.name);
                t.expect(byFootprint == 0, what);
                break;
        }
    }
}

// The mutants of the rule. rigRun: the GPU rig mutates the shader the same way, and the scenes it runs must tell the rule from the mutant.
struct Mutant {
    const char* name;
    Rule rule;
    bool rigRun;
};
inline std::vector<Mutant> mutants() {
    std::vector<Mutant> m;
    const auto add = [&](const char* name, bool rigRun, auto edit) { Rule r; edit(r); m.push_back({name, r, rigRun}); };
    // The footprint test (the second form).
    add("the point test alone (the check as it was before the footprint test)", true, [](Rule& r) { r.footprint = false; r.junction = false; });
    add("no footprint test (the junction test stays)", true, [](Rule& r) { r.footprint = false; });
    add("no cross difference test", true, [](Rule& r) { r.planar = false; });
    add("the cross difference is allowed 8 tolerances", true, [](Rule& r) { r.planarScale = 8.0f; });
    add("the mean of the four texels, not the value at the exact position", true, [](Rule& r) { r.exactPosition = false; });
    add("a footprint outside the frame is asked, its edge texels repeated", true, [](Rule& r) { r.wholeFootprint = false; });
    add("a zero or non-finite texel does not bar the footprint", true, [](Rule& r) { r.solid = false; });
    add("the horizontal fraction weights the rows and the vertical one the columns", true, [](Rule& r) { r.swappedWeights = true; });
    add("the tolerance is 5%", false, [](Rule& r) { r.rel = 0.05f; });
    add("the tolerance is 0.5%", false, [](Rule& r) { r.rel = 0.005f; });
    add("the floor is zero", false, [](Rule& r) { r.floor = 0.0f; });
    add("the floor is 1e-5", false, [](Rule& r) { r.floor = 1e-5f; });
    // The junction test (the third form). The GPU rig's camera is still and its rows carry no jitter, so the two camera mutants are the CPU cases' alone.
    add("no junction test (the check as it was before it)", true, [](Rule& r) { r.junction = false; });
    add("no persistence half: the pixel's own surface beside the footprint is enough", true, [](Rule& r) { r.persist = false; });
    add("a footprint texel that lands outside the image reads the current depth's edge texels", true, [](Rule& r) { r.insideCheck = false; });
    add("the camera-origin term is not reversed (cameraAfter keeps cameraBefore's sign)", false, [](Rule& r) { r.reverseOrigin = true; });
    add("cameraAfter keeps the NDC shift the rows carry", false, [](Rule& r) { r.unjitter = false; });
    add("the adjacent quad's cross difference is not asked", true, [](Rule& r) { r.ownPlanar = false; });
    add("the adjacent quad's plane at the exact position is not compared with the expected depth", true, [](Rule& r) { r.ownExtrapolation = false; });
    add("an adjacent quad partly outside the image is asked, its edge texels repeated", false, [](Rule& r) { r.ownWhole = false; });
    add("a zero or non-finite texel does not bar the adjacent quad", false, [](Rule& r) { r.ownSolid = false; });
    add("the adjacent quads are asked up, down, left, right", false, [](Rule& r) { r.reversedOrder = true; });
    add("a footprint texel is another surface only 8 tolerances off the plane", true, [](Rule& r) { r.offPlaneScale = 8.0f; });
    return m;
}

inline int run(const Rule& r, bool narrate) {
    Sink t;
    t.narrate = narrate;
    t.quiet = !narrate;
    footprints(t, r);
    frames(t, r);
    rigScenes(t, r);
    return t.failures;
}
}  // namespace flat_history_depth

inline int flatHistoryDepthTests() {
    using namespace flat_history_depth;
    int failures = 0;
    {
        Sink t;
        textPins(t);
        failures += t.failures;
    }
    failures += flat_history_junction::runText(true);
    failures += run(Rule(), true);
    failures += flat_history_junction::run(Rule(), true);
    // Mutation controls: the tests fail every mutant of the rule.
    int caught = 0;
    const std::vector<Mutant> list = mutants();
    std::vector<Rule> mutantRules;
    for (const Mutant& m : list) mutantRules.push_back(m.rule);
    const std::vector<int> junctionFailures = flat_history_junction::runAll(mutantRules, false);   // one pass of the 960x600 scenes serves every mutant
    for (size_t index = 0; index < list.size(); ++index) {
        const Mutant& m = list[index];
        const int failed = run(m.rule, false) + junctionFailures[index];
        if (failed > 0) ++caught;
        else { std::printf("FAIL: history depth mutation \"%s\" is not caught by the CPU cases\n", m.name); ++failures; }
        if (m.rigRun) {
            size_t count = 0, differing = 0;
            const histdepth::rig16::Plan* plan = histdepth::rig16::plan(&count);
            for (size_t i = 0; i < count; ++i) differing += differences(histdepth::rig16::build(plan[i]), Rule(), m.rule);
            if (differing == 0) { std::printf("FAIL: history depth mutation \"%s\" changes no pixel of the GPU rig's scenes\n", m.name); ++failures; }
            std::printf("flat history depth: mutant \"%s\": %d CPU checks fail, %zu pixels of the GPU rig's scenes change\n", m.name, failed, differing);
        } else {
            std::printf("flat history depth: mutant \"%s\": %d CPU checks fail\n", m.name, failed);
        }
    }
    std::printf("flat history depth: %d of %zu mutants of the rule caught by the CPU cases\n", caught, list.size());
    return failures;
}
