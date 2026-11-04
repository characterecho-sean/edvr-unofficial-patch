#pragma once
// THE HISTORY DEPTH CHECK'S JUNCTION TEST, on the CPU (src/d3d11/flat_mono_shader_source.h, historyDepthMatches and junctionPersists; design doc
// section 104, "Junctions: the residual band, and the persistence test"; the rule is flat_history_depth_model.h's decide()).
//
// Where a far floor meets a wall, or the floor meets the edge of a parked hull, the four history-depth texels around the reprojected position straddle two
// surfaces. They are not one plane, so the footprint test refuses them, and a still camera showed thin lines there. Refusing is right when the second
// surface left (a disocclusion: the history colour at the edge is the object's) and wrong when it did not. The junction test tells them apart with two
// halves, both required: the pixel's own surface is present in a planar quad of texels edge-adjacent to the footprint, whose plane at the exact position has
// the expected depth; and every footprint texel off that plane is carried into this frame with the camera rows (cameraAfter, the mirror of the camera term)
// to a place where the current depth still has it.
//
// What runs here (the GPU rig, tools\flat_mono_resolve_test\flat_taa_history_depth_gpu_tests.h, runs 16x16 scenes of the same rule through the real kernel):
//   1. the HLSL's text, pinned statement by statement, and cameraAfter pinned as the exact mirror of cameraBefore (derived from it here);
//   2. the camera math: cameraAfter(cameraBefore(x)) is x for rotations and translations, with and without the NDC shift the rows carry, with the
//      game's own rows (Epic frame 71751); cameraBefore agrees with a ray cast; a point behind the camera has no projection;
//   3. single footprints (8x8 depth images, a still camera): a junction that is still there is kept, the object that left it, moved away, or got 5% deeper
//      is not; the quads are asked in the documented order; a quad leaving the image, a zero texel in the quad, a texel whose projection leaves the image
//      and a history depth carrying the SDK foreground scale each have their case;
//   4. a 960x600 frame under every consecutive pair of the shipped 32-phase Halton cycle, rendered by a ray caster (flat_history_junction_scenes.h)
//      with real camera rows: a floor meeting a far wall; a floor under a parked hull; an occluder moving 2 px a frame; a slow camera translation, a
//      turn, and rows that carry the jitter; a mover receding on a plane; the horizon; the image's border. Each says what the rule keeps of the pixels
//      that today's rule (the point and footprint tests) refuses, and how many of them reach each half;
//   5. the mutants of the new code (Rules with fields changed), each of which one of the above must fail.
//
// Geometry. The scenes model the prep's call: the previous position handed to the check is the previous position of the raster texel's own surface point,
// and `expected` is that same point's depth (cameraBefore). taa() hands the check the previous position of the OUTPUT pixel's centre, one current phase
// away (flat_history_depth_tests.h says so); sweeps with that convention are narrated, not asserted.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "../../src/d3d11/flat_mono_refusal.h"
#include "../../src/d3d11/flat_mono_shader_source.h"
#include "flat_history_depth_model.h"
#include "flat_history_junction_scenes.h"

namespace flat_history_junction {
using histdepth::CurrentFrame;
using histdepth::Image;
using histdepth::Rule;
using histdepth::Vec4;
using histdepth::Verdict;

struct Sink {
    int failures = 0;
    bool narrate = false;   // print the numbers this run measures
    bool quiet = false;     // a run of a mutant: count the failures, print none
    const char* section = "";
    void expect(bool ok, const std::string& what) {
        if (!ok) {
            if (!quiet) std::printf("FAIL: history junction %s: %s\n", section, what.c_str());
            ++failures;
        }
    }
};

// ---- 1. the shader's text -------------------------------------------------------------------------------------------------------------------
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}
// From the first occurrence of `first` to the end of the next `tail` (inclusive), or empty.
inline std::string between(const std::string& src, const std::string& first, const std::string& tail) {
    const size_t a = src.find(first);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find(tail, a);
    if (b == std::string::npos) return std::string();
    return src.substr(a, b + tail.size() - a);
}
inline bool isIdent(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
inline std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    return s;
}
// A whole word (not part of a longer identifier) replaced.
inline std::string replaceWord(std::string s, const std::string& from, const std::string& to) {
    size_t at = s.find(from);
    while (at != std::string::npos) {
        const bool left = at == 0 || !isIdent(s[at - 1]), right = at + from.size() >= s.size() || !isIdent(s[at + from.size()]);
        if (left && right) {
            s.replace(at, from.size(), to);
            at = s.find(from, at + to.size());
        } else {
            at = s.find(from, at + from.size());
        }
    }
    return s;
}
// n0..n3 and o0..o3 exchanged, as whole words.
inline std::string swapRows(std::string s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if ((s[i] != 'n' && s[i] != 'o') || s[i + 1] < '0' || s[i + 1] > '3') continue;
        if (i > 0 && isIdent(s[i - 1])) continue;
        if (i + 2 < s.size() && isIdent(s[i + 2])) continue;
        s[i] = s[i] == 'n' ? 'o' : 'n';
    }
    return s;
}
// cameraAfter is cameraBefore with the two cameras exchanged: the same statements with this frame's rows (n) and last frame's (o) swapped after the rows have
// been unjittered, the origin term reversed, and `after` for `before`.
inline std::string mirrorOfBefore(const std::string& before) {
    const size_t cut = before.find("    float3 a=");
    if (cut == std::string::npos) return std::string();
    std::string head = before.substr(0, cut), rest = before.substr(cut);
    head = replaceAll(head, "bool cameraBefore(float2 uv, float depth, out float4 before)", "bool cameraAfter(float2 uv, float depth, out float4 after)");
    rest = replaceAll(replaceWord(swapRows(rest), "before", "after"), "position+=", "position-=");
    return head + rest;
}
inline std::string mirrorFailure(const std::string& source) {
    const std::string before = between(source, "bool cameraBefore(", "return before.w>0 && all(isfinite(before));\n}");
    const std::string after = between(source, "bool cameraAfter(", "return after.w>0 && all(isfinite(after));\n}");
    if (before.empty() || after.empty()) return "cameraBefore or cameraAfter is not in the source";
    if (mirrorOfBefore(before) != after) return "cameraAfter is not the exact mirror of cameraBefore (its rows exchanged, its origin term reversed)";
    return std::string();
}

struct FnPins { const char* first; const char* tail; std::vector<std::string> pins; };
inline const std::vector<FnPins>& junctionPins() {
    static const std::vector<FnPins> p = {
        {"float4 historyQuad(", "}\n", {
            "float4 historyQuad(int2 at) {",
            "return float4(HistoryDepth.Load(int3(at,0)),HistoryDepth.Load(int3(at+int2(1,0),0)),HistoryDepth.Load(int3(at+int2(0,1),0)),HistoryDepth.Load(int3(at+int2(1,1),0)));",
        }},
        {"float quadDepthAt(", "}", {
            "float quadDepthAt(float4 quad,float2 at) {return lerp(lerp(quad.x,quad.y,at.x),lerp(quad.z,quad.w,at.x),at.y);}",
        }},
        {"bool junctionPersists(", "    return true;\n}", {
            "bool junctionPersists(int2 base,float2 raster,float4 footprint,float expected,float tol) {",
            "const int2 hi=int2(size.xy)-1;",
            "float4 own=0; int2 ownBase=0; bool found=false;",
            "[unroll]for(int i=0;i<4;++i) {",
            "const int2 at=base+(i==0?int2(1,0):(i==1?int2(-1,0):(i==2?int2(0,1):int2(0,-1))));",
            "if(found || any(at<0) || any(at>=hi))continue;",
            "const float4 quad=historyQuad(at);",
            "if(!all(isfinite(quad)) || !all(quad>0))continue;",
            "if(!(abs(quad.x+quad.w-quad.y-quad.z)<=tol) || !(abs(quadDepthAt(quad,raster-float2(at))-expected)<=tol))continue;",
            "own=quad; ownBase=at; found=true;",
            "if(!found)return false;",
            "const float depthScale=(debug.w&2)!=0?foregroundDepth.x:1.0;",
            "[loop]for(int k=0;k<4;++k) {",
            "const int2 texel=base+int2(k&1,k>>1);",
            "const float depthThen=footprint[k];",
            "if(abs(depthThen-quadDepthAt(own,float2(texel-ownBase)))<=tol)continue;",
            "float4 after;",
            "if(!cameraAfter((float2(texel)+.5-jitter.zw)/float2(size.xy),depthThen/depthScale,after))return false;",
            "const float e=after.z/after.w;",
            "const float2 cur=(after.xy/after.w*float2(.5,-.5)+.5)*float2(size.xy)+jitter.xy-.5;",
            "if(!isfinite(e) || !all(cur>=0) || !all(cur<float2(hi)))return false;",
            "const int2 curBase=int2(floor(cur));",
            "float best=3.402823e38;",
            "[unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)best=min(best,abs(SceneDepth.Load(int3(curBase+int2(x,y),0))-e));",
            "if(!(best<=max(kStaleDepthFloor,e*kStaleDepthRel)))return false;",
        }},
    };
    return p;
}
// Empty when the source holds the junction test and cameraAfter as the model mirrors them; otherwise the first thing it does not hold.
inline std::string pinFailure(const std::string& source) {
    for (const FnPins& f : junctionPins()) {
        const std::string fn = between(source, f.first, f.tail);
        if (fn.empty()) return std::string("not in the source: ") + f.first;
        size_t from = 0;
        for (const std::string& pin : f.pins) {
            if (countOf(fn, pin) != 1) return "not exactly once in the function: " + pin;
            const size_t at = fn.find(pin);
            if (at < from) return "out of order: " + pin;
            from = at;
        }
    }
    // The current depth is read by the prep for the pixel (once) and by the junction test (the four texels, once), nowhere else.
    if (countOf(source, "SceneDepth.Load(") != 2) return "SceneDepth is read somewhere else, or not by the junction test";
    return mirrorFailure(source);
}

inline void textPins(Sink& t) {
    t.section = "text";
    const std::string source = edvr::kFlatMonoShaderSource;
    const std::string failure = pinFailure(source);
    t.expect(failure.empty(), "the shader's junction test and cameraAfter are the statements the model mirrors: " + failure);
    t.expect(source.find("if(abs(planeDepth-expected)<=tol && abs(d00+d11-d10-d01)<=tol)return true;\n    return junctionPersists(base,raster,footprint,expected,tol);\n}") !=
                 std::string::npos,
             "historyDepthMatches asks the junction test last, after the point test and the footprint test, and returns its answer unchanged");
    // The pins have teeth: each statement altered is found.
    const char* const edits[][2] = {
        {"if(found || any(at<0) || any(at>=hi))continue;", "if(found || any(at<0) || any(at>hi))continue;"},
        {"(i==0?int2(1,0):(i==1?int2(-1,0):(i==2?int2(0,1):int2(0,-1))))", "(i==0?int2(-1,0):(i==1?int2(1,0):(i==2?int2(0,1):int2(0,-1))))"},
        {"(i==0?int2(1,0):(i==1?int2(-1,0):(i==2?int2(0,1):int2(0,-1))))", "(i==0?int2(1,0):(i==1?int2(-1,0):(i==2?int2(0,-1):int2(0,1))))"},
        {"if(!all(isfinite(quad)) || !all(quad>0))continue;", "if(!all(isfinite(quad)))continue;"},
        {"if(!(abs(quad.x+quad.w-quad.y-quad.z)<=tol) || ", "if("},
        {"|| !(abs(quadDepthAt(quad,raster-float2(at))-expected)<=tol))continue;", ")continue;"},
        {"if(!found)return false;", ""},
        {"depthThen/depthScale", "depthThen"},
        {"const float depthScale=(debug.w&2)!=0?foregroundDepth.x:1.0;", "const float depthScale=1.0;"},
        {"if(abs(depthThen-quadDepthAt(own,float2(texel-ownBase)))<=tol)continue;", ""},
        {"(float2(texel)+.5-jitter.zw)", "(float2(texel)+.5-jitter.xy)"},
        {"if(!isfinite(e) || !all(cur>=0) || !all(cur<float2(hi)))return false;", "if(!isfinite(e))return false;"},
        {"!all(cur<float2(hi))", "!all(cur<=float2(hi))"},
        {"const int2 curBase=int2(floor(cur));", "const int2 curBase=int2(round(cur));"},
        {"if(!(best<=max(kStaleDepthFloor,e*kStaleDepthRel)))return false;", ""},
        {"if(!(best<=max(kStaleDepthFloor,e*kStaleDepthRel)))return false;", "if(!(best<=max(kStaleDepthFloor,e*.05)))return false;"},
        {"abs(SceneDepth.Load(int3(curBase+int2(x,y),0))-e)", "abs(HistoryDepth.Load(int3(curBase+int2(x,y),0))-e)"},
        // cameraAfter, against cameraBefore.
        {"position-=(now[5].xyz-old[5].xyz)*iz;", "position+=(now[5].xyz-old[5].xyz)*iz;"},
        {"iz=depth/o3.z", "iz=depth/n3.z"},
        {"float3 a=float3(o0.x,o1.x,o2.x);", "float3 a=float3(n0.x,n1.x,n2.x);"},
        {"after=position.x*n0+position.y*n1+position.z*n2+iz*n3;", "after=position.x*o0+position.y*o1+position.z*o2+iz*o3;"},
        {"float4 n0=unjitterRow(now[0],rowsJitter.xy), n1=unjitterRow(now[1],rowsJitter.xy);\n    float4 n2=unjitterRow(now[2],rowsJitter.xy), n3=unjitterRow(now[3],rowsJitter.xy);\n"
         "    float4 o0=unjitterRow(old[0],rowsJitter.zw), o1=unjitterRow(old[1],rowsJitter.zw);\n    float4 o2=unjitterRow(old[2],rowsJitter.zw), o3=unjitterRow(old[3],rowsJitter.zw);\n"
         "    float3 a=float3(o0.x",
         "float4 n0=now[0], n1=now[1];\n    float4 n2=now[2], n3=now[3];\n"
         "    float4 o0=unjitterRow(old[0],rowsJitter.zw), o1=unjitterRow(old[1],rowsJitter.zw);\n    float4 o2=unjitterRow(old[2],rowsJitter.zw), o3=unjitterRow(old[3],rowsJitter.zw);\n"
         "    float3 a=float3(o0.x"},
        {"return after.w>0 && all(isfinite(after));", "return all(isfinite(after));"},
    };
    for (const auto& e : edits) {
        std::string edited = source;
        // The anchors of cameraAfter's statements also occur in cameraBefore or earlier: the edit goes to the LAST occurrence that is cameraAfter's.
        const size_t fn = edited.find("bool cameraAfter(");
        size_t at = edited.find(e[0], fn == std::string::npos ? 0 : fn);
        if (at == std::string::npos) at = edited.find(e[0]);
        t.expect(at != std::string::npos, std::string("the pin's mutation anchor is in the source: ") + e[0]);
        if (at == std::string::npos) continue;
        edited.replace(at, std::strlen(e[0]), e[1]);
        t.expect(!pinFailure(edited).empty(), std::string("the text pins catch: ") + e[0] + " -> " + e[1]);
    }
}

// ---- 2. the camera math ---------------------------------------------------------------------------------------------------------------------
struct Lcg {
    uint32_t s;
    float next() { s = s * 1664525u + 1013904223u; return float(s >> 8) / float(1u << 24); }
};
struct PosePair { const char* name; histscene::Pose old, now; };
inline std::vector<PosePair> posePairs() {
    std::vector<PosePair> v;
    histscene::Pose a, b;
    v.push_back({"a still camera", a, a});
    b = a; b.o[0] = -0.05f;
    v.push_back({"strafing 0.05 m a frame", b, a});
    b = a; b.o[2] = -0.05f;
    v.push_back({"walking 0.05 m a frame", b, a});
    {
        histscene::Pose o, n;
        o.o[0] = -0.4f; o.o[1] = 0.1f; o.o[2] = -0.7f; o.yaw = -0.05f; o.pitch = 0.02f;
        n.o[0] = 0.0f; n.yaw = 0.03f; n.pitch = -0.01f;
        v.push_back({"a turn and a step", o, n});
    }
    {
        histscene::Pose o, n;
        o.o[0] = 2.0f; o.o[1] = -0.3f; o.o[2] = -3.0f; o.yaw = 0.35f; o.pitch = -0.1f;
        n.o[0] = 0.5f; n.o[2] = 0.2f; n.yaw = -0.2f; n.pitch = 0.08f;
        v.push_back({"a large turn and move", o, n});
    }
    return v;
}
// The NDC shift a jittered frustum puts in a camera's rows for raster phase (jx, jy): the raster is displaced by (jx, jy) pixels, y down.
inline void rowsShift(float jx, float jy, float* ndcX, float* ndcY) {
    *ndcX = 2.0f * jx / float(histscene::kW);
    *ndcY = -2.0f * jy / float(histscene::kH);
}

struct RoundTrip { float maxPx = 0, maxRel = 0; int n = 0, unprojectable = 0; };
// cameraAfter(cameraBefore(x)) is x: pixels in this frame sent to last frame and back.
inline RoundTrip roundTrip(const histdepth::Cam& now, const histdepth::Cam& old, const Rule& rule) {
    RoundTrip r;
    Lcg rng{12345u};
    for (int i = 0; i < 400; ++i) {
        const float u = 0.1f + 0.8f * rng.next(), v = 0.1f + 0.8f * rng.next(), z = 1.0f + 79.0f * rng.next();
        const float depth = histscene::kNear / z;
        Vec4 before, after;
        if (!histdepth::cameraBefore(now, old, u, v, depth, &before)) { ++r.unprojectable; continue; }
        const float pu = before.x / before.w * 0.5f + 0.5f, pv = before.y / before.w * -0.5f + 0.5f, e = before.z / before.w;
        if (!histdepth::cameraAfter(now, old, pu, pv, e, &after, rule.unjitter, rule.reverseOrigin)) { ++r.unprojectable; continue; }
        const float du = (after.x / after.w * 0.5f + 0.5f - u) * float(histscene::kW), dv = (after.y / after.w * -0.5f + 0.5f - v) * float(histscene::kH);
        r.maxPx = std::max(r.maxPx, std::max(std::fabs(du), std::fabs(dv)));
        r.maxRel = std::max(r.maxRel, std::fabs(after.z / after.w - depth) / depth);
        ++r.n;
    }
    return r;
}

// Epic frame 71751's rows (flat_projection_ownership_tests.h): the shape the game composes for a kind-3 camera.
inline histdepth::Cam epicCamera() {
    static const float rows[6][4] = {{.674860716f, -.714774430f, 0, .684166729f}, {-.804393589f, -.069881566f, 0, .664024174f}, {-.240084499f, -1.77504551f, 0, -.301641792f},
                                     {0, 0, .0250000004f, 0}, {.684166729f, .664024174f, -.301641792f, 0}, {-21.0930309f, -24.5114784f, -1.11009693f, 0}};
    histdepth::Cam c;
    std::memcpy(c.rows, rows, sizeof(rows));
    return c;
}

inline void cameraTests(Sink& t, const Rule& rule) {
    t.section = "camera";
    char what[300];
    float worstPx = 0, worstRel = 0;
    for (const PosePair& p : posePairs())
        for (int jittered = 0; jittered < 2; ++jittered) {
            float nx = 0, ny = 0, ox = 0, oy = 0;
            if (jittered) { rowsShift(0.31f, -0.27f, &nx, &ny); rowsShift(-0.18f, 0.44f, &ox, &oy); }
            const histdepth::Cam now = histscene::camOf(p.now, nx, ny), old = histscene::camOf(p.old, ox, oy);
            const RoundTrip r = roundTrip(now, old, rule);
            worstPx = std::max(worstPx, r.maxPx);
            worstRel = std::max(worstRel, r.maxRel);
            std::snprintf(what, sizeof(what), "%s%s: cameraAfter(cameraBefore(x)) is x (worst %.5f px, depth %.2e of itself, %d points)", p.name,
                          jittered ? ", rows carrying the jitter" : "", r.maxPx, r.maxRel, r.n);
            t.expect(r.n > 300 && r.maxPx <= 0.01f && r.maxRel <= 2e-5f, what);
        }
    {   // The game's own rows, a step and a turn away from themselves.
        const histdepth::Cam now = epicCamera();
        histdepth::Cam old = now;
        old.rows[5][0] += 0.35f; old.rows[5][1] -= 0.2f; old.rows[5][2] += 0.12f;
        const float c = std::cos(0.02f), s = std::sin(0.02f);
        for (unsigned col = 0; col < 4; ++col) {
            const float x = old.rows[0][col], y = old.rows[1][col];
            old.rows[0][col] = c * x - s * y;
            old.rows[1][col] = s * x + c * y;
        }
        const RoundTrip r = roundTrip(now, old, rule);
        worstPx = std::max(worstPx, r.maxPx);
        worstRel = std::max(worstRel, r.maxRel);
        std::snprintf(what, sizeof(what), "Epic frame 71751's rows, a step and a turn: the round trip is x (worst %.5f px, depth %.2e of itself, %d points)", r.maxPx, r.maxRel, r.n);
        t.expect(r.n > 100 && r.maxPx <= 0.01f && r.maxRel <= 2e-5f, what);
    }
    if (t.narrate) std::printf("flat history junction: cameraAfter(cameraBefore(x)): worst error over 5 pose pairs, with and without jittered rows, and Epic 71751's rows: %.5f px, depth %.2e of itself\n", worstPx, worstRel);
    // cameraBefore against a ray cast: the world point under a pixel of this frame, projected into last frame's camera in double precision.
    {
        histscene::World w;
        w.prims = {histscene::floorPrim(), histscene::wallPrim(30.0f, 3.0f)};
        float worst = 0, worstDepth = 0;
        int n = 0;
        for (const PosePair& p : posePairs()) {
            const histdepth::Cam now = histscene::camOf(p.now), old = histscene::camOf(p.old);
            const histscene::Basis bo = histscene::basisOf(p.old), bn = histscene::basisOf(p.now);
            Lcg rng{777u};
            for (int i = 0; i < 200; ++i) {
                const float u = 0.05f + 0.9f * rng.next(), v = 0.05f + 0.9f * rng.next();
                const histscene::Hit h = histscene::castUv(w, p.now, u, v);
                if (h.id == histscene::kSurfSky) continue;
                const double nx = double(u) * 2.0 - 1.0, ny = 1.0 - double(v) * 2.0;
                double dir[3], world[3], rel[3];
                for (int k = 0; k < 3; ++k) {
                    dir[k] = double(bn.fwd[k]) + nx * histscene::kTanX * double(bn.right[k]) + ny * histscene::kTanY * double(bn.up[k]);
                    world[k] = double(p.now.o[k]) + dir[k] * double(h.t);
                    rel[k] = world[k] - double(p.old.o[k]);
                }
                const double zo = rel[0] * bo.fwd[0] + rel[1] * bo.fwd[1] + rel[2] * bo.fwd[2];
                const double xo = (rel[0] * bo.right[0] + rel[1] * bo.right[1] + rel[2] * bo.right[2]) / (zo * histscene::kTanX);
                const double yo = (rel[0] * bo.up[0] + rel[1] * bo.up[1] + rel[2] * bo.up[2]) / (zo * histscene::kTanY);
                Vec4 before;
                if (zo <= 0 || !histdepth::cameraBefore(now, old, u, v, histscene::depthOf(h), &before)) continue;
                const double pu = xo * 0.5 + 0.5, pv = -yo * 0.5 + 0.5;
                worst = std::max(worst, float(std::max(std::fabs(double(before.x / before.w * 0.5f + 0.5f) - pu) * histscene::kW,
                                                       std::fabs(double(before.y / before.w * -0.5f + 0.5f) - pv) * histscene::kH)));
                worstDepth = std::max(worstDepth, float(std::fabs(double(before.z / before.w) - double(histscene::kNear) / zo) / (double(histscene::kNear) / zo)));
                ++n;
            }
        }
        std::snprintf(what, sizeof(what), "cameraBefore agrees with a ray cast projected into last frame's camera (the rows are laid out as the shader reads them): %d points, worst %.5f px, depth %.2e of itself", n, worst, worstDepth);
        t.expect(n > 400 && worst <= 0.02f && worstDepth <= 5e-5f, what);
    }
    {   // A point 0.5 m ahead of last frame's camera is behind this frame's, which has stepped 1 m forward: it has no projection.
        histscene::Pose o, n;
        n.o[2] = 1.0f;
        Vec4 after;
        t.expect(!histdepth::cameraAfter(histscene::camOf(n), histscene::camOf(o), 0.55f, 0.5f, histscene::kNear / 0.5f, &after, rule.unjitter, rule.reverseOrigin),
                 "a point that is behind this frame's camera has no projection (cameraAfter refuses it)");
        t.expect(histdepth::cameraAfter(histscene::camOf(n), histscene::camOf(o), 0.55f, 0.5f, histscene::kNear / 3.0f, &after, rule.unjitter, rule.reverseOrigin),
                 "... and one 3 m ahead of last frame's camera is 2 m ahead of this one's");
    }
}

// ---- 3. single footprints -------------------------------------------------------------------------------------------------------------------
// An 8x8 history and an 8x8 current depth, a still camera, the previous phase (pjx, pjy) and the current one (cjx, cjy). The sample position (rx, ry) is in
// texel-centre coordinates.
struct Unit {
    Image hist{8, 8}, cur{8, 8};
    float pjx = 0, pjy = 0, cjx = 0, cjy = 0, scale = 1.0f;
    template <class F> void both(F f) { for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) hist.at(x, y) = cur.at(x, y) = f(x, y); }
    template <class F> void history(F f) { for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) hist.at(x, y) = f(x, y); }
    template <class F> void current(F f) { for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) cur.at(x, y) = f(x, y); }
    Verdict decide(float rx, float ry, float expected, const Rule& rule) const {
        CurrentFrame f;
        f.depth = &cur;
        f.now = f.old = histdepth::stillCamera();
        f.jx = cjx; f.jy = cjy; f.scale = scale;
        return histdepth::decide(hist, (rx + 0.5f - pjx) / 8.0f, (ry + 0.5f - pjy) / 8.0f, pjx, pjy, expected, rule, &f);
    }
};

inline void unitCases(Sink& t, const Rule& r) {
    t.section = "single footprints";
    const Rule today = histdepth::beforeJunction();
    // A junction along x of a steep floor (30% a row) and a wall: the wall fills rows 0..3 at depth .02, the floor continues the wall's depth at row 3.5.
    const auto floorAt = [](float y) { return 0.02f + 0.006f * (y - 3.5f); };
    const auto crease = [&](int, int y) { return y <= 3 ? 0.02f : floorAt(float(y)); };
    {
        Unit u;
        u.both(crease);
        const float rx = 3.5f, ry = 3.6f, expected = floorAt(ry);
        t.expect(!u.decide(rx, ry, expected, today).kept, "a floor pixel at the foot of a wall: today's rule (the point and footprint tests) refuses it");
        const Verdict v = u.decide(rx, ry, expected, r);
        t.expect(v.kept && v.byJunction && v.own == 2 && v.offPlane > 0, "... a wall that is still there: the plane of the floor below the footprint is its own surface (the +y quad) and the wall persists: kept");
        Unit gone;   // the wall left: the floor shows where it was
        gone.history(crease);
        gone.current([&](int, int y) { return floorAt(float(y)); });
        const Verdict g = gone.decide(rx, ry, expected, r);
        t.expect(!g.kept && g.own == 2, "... a wall that left (the floor now shows where it was): its own surface is found and the other one is not there, refused");
        Unit moved;   // the wall retreated two rows
        moved.history(crease);
        moved.current([&](int, int y) { return y <= 1 ? 0.02f : floorAt(float(y)); });
        t.expect(!moved.decide(rx, ry, expected, r).kept, "... a wall that moved two rows: refused");
        Unit deeper;   // the wall is 5% deeper
        deeper.history(crease);
        deeper.current([&](int x, int y) { return y <= 3 ? 0.021f : crease(x, y); });
        t.expect(!deeper.decide(rx, ry, expected, r).kept, "... a wall 5% deeper than it was: another surface, refused");
        Unit slight;     // 0.5% deeper is inside the tolerance
        slight.history(crease);
        slight.current([&](int x, int y) { return y <= 3 ? 0.0201f : crease(x, y); });
        t.expect(slight.decide(rx, ry, expected, r).kept, "... a wall 0.5% deeper: inside the tolerance, kept");
        // The surfaces are not interchangeable: the pixel shows the floor; one that shows something else at the junction has no own surface beside the footprint.
        t.expect(!u.decide(rx, ry, 0.0259f, r).kept, "a pixel that shows neither surface (an expected depth no quad has there): refused");
        t.expect(!u.decide(rx, ry, 0.0f, r).kept, "a pixel that shows the sky: refused");
    }
    // The documented order: right, left, down, up; the first quad that qualifies is the pixel's own surface. A crease in x whose two sides both have the
    // expected depth at the sample position (3.5): the right side qualifies first. The right-hand surface is gone from this frame, the left-hand one is not:
    // the pixel is kept only if the right-hand side is the one chosen as its own.
    {
        const auto sides = [](int x, int) { return x <= 3 ? 0.02f + 0.001f * (float(x) - 3.5f) : 0.02f + 0.004f * (float(x) - 3.5f); };
        Unit u;
        u.history(sides);
        u.current([&](int x, int y) { return x <= 3 ? sides(x, y) : 0.03f; });
        const Verdict v = u.decide(3.5f, 3.2f, 0.02f, r);
        t.expect(!u.decide(3.5f, 3.2f, 0.02f, today).kept, "two surfaces crossing at the sample: today's rule refuses");
        t.expect(v.kept && v.own == 0, "the quads are asked right, left, down, up: the right-hand quad is the pixel's own surface, the left-hand surface persists: kept");
        Unit both;     // both persist: kept whichever is chosen
        both.both(sides);
        t.expect(both.decide(3.5f, 3.2f, 0.02f, r).kept, "... both sides still there: kept");
        Unit leftGone;
        leftGone.history(sides);
        leftGone.current([&](int x, int y) { return x <= 3 ? 0.03f : sides(x, y); });
        t.expect(!leftGone.decide(3.5f, 3.2f, 0.02f, r).kept, "... the left-hand surface gone, the right-hand one is the pixel's own and the one that must persist: refused");
    }
    // A quad that leaves the image is not asked: the pixel's own surface is the bottom row alone, whose edge texels a clamped quad would repeat.
    {
        Unit u;
        const auto sliver = [](int x, int y) { return y <= 6 ? 0.03f : 0.02f + 0.002f * (float(x) - 3.5f); };
        u.both(sliver);
        const float expected = 0.02f + 0.002f * (3.6f - 3.5f);
        t.expect(!u.decide(3.6f, 6.3f, expected, today).kept, "a sliver one texel deep at the image's bottom: today's rule refuses");
        t.expect(!u.decide(3.6f, 6.3f, expected, r).kept, "... the only quad of its own surface leaves the image and is not asked: refused");
    }
    // A zero texel (the sky) bars the adjacent quad even where the quad is a plane through it: a ramp that reaches zero at column 6.
    {
        Unit u;
        const auto ramp = [](int x, int) { return x <= 4 ? 0.03f : (x >= 6 ? 0.0f : 0.01f * float(6 - x)); };
        u.both(ramp);
        const float expected = 0.01f * (6.0f - 4.6f);
        t.expect(!u.decide(4.6f, 3.5f, expected, today).kept, "a ramp to the horizon beside a wall: today's rule refuses");
        t.expect(!u.decide(4.6f, 3.5f, expected, r).kept, "... the quad whose plane is the ramp holds the zero texel and is not asked: refused");
    }
    // A footprint texel whose projection leaves the image refuses: the junction at each edge of the image, the wall in the edge row or column, with a previous
    // phase that carries it outside (refused) and the other way (inside, kept).
    {
        for (int side = 0; side < 4; ++side) {
            // c is the distance, in texels, from the edge: the wall is at c == 0, the surface the pixel shows is a plane in c from c == 1 on.
            const auto cOf = [side](int x, int y) { return side == 0 ? x : (side == 1 ? 7 - x : (side == 2 ? y : 7 - y)); };
            const auto edge = [&](int x, int y) { const int c = cOf(x, y); return c == 0 ? 0.02f : 0.03f + 0.004f * float(c - 1); };
            const float cr = 0.6f;                                   // the sample's distance from the edge column
            const float expected = 0.03f + 0.004f * (cr - 1.0f);
            const float along = 3.4f;                                // the other axis
            const float rx = side == 0 ? cr : (side == 1 ? 7.0f - cr : along), ry = side == 2 ? cr : (side == 3 ? 7.0f - cr : along);
            for (int out = 0; out < 2; ++out) {
                Unit u;
                u.both(edge);
                // The wall texel sits at c == 0 and projects to c + (current phase - previous phase) along the normal: outside when the previous phase is toward the edge.
                const float shift = out ? 0.25f : -0.25f;           // toward the wall, in c
                const float sx = (side == 0 ? 1.0f : (side == 1 ? -1.0f : 0.0f)) * shift, sy = (side == 2 ? 1.0f : (side == 3 ? -1.0f : 0.0f)) * shift;
                u.pjx = sx; u.pjy = sy;
                const Verdict v = u.decide(rx, ry, expected, r);
                char what[200];
                std::snprintf(what, sizeof(what), "a wall in the image's %s: %s", side == 0 ? "left column" : (side == 1 ? "right column" : (side == 2 ? "top row" : "bottom row")),
                              out ? "its texel's projection leaves the image: refused" : "its texel's projection stays inside: kept");
                t.expect(v.own >= 0 && v.kept == !out, what);
            }
        }
    }
    // The SDK foreground map scales the history depth (foregroundDepth.x) and not the current one: the scale is divided out before the texel is projected.
    {
        Unit u;
        u.history([&](int x, int y) { return 1.6f * crease(x, y); });
        u.current(crease);
        u.scale = 1.6f;
        const float rx = 3.5f, ry = 3.6f, expected = 1.6f * floorAt(ry);
        t.expect(u.decide(rx, ry, expected, r).kept, "a history carrying the SDK foreground scale (1.6) and a current depth that does not: the scale is divided out, kept");
        u.scale = 1.0f;
        t.expect(!u.decide(rx, ry, expected, r).kept, "... without the scale divided out, the wall is 60% off where it is projected: refused");
    }
}

// ---- 4. a 960x600 frame under the shipped cycle ---------------------------------------------------------------------------------------------
using histscene::kH;
using histscene::kW;

struct Case {
    histscene::World then, now;
    histscene::Pose thenPose, nowPose;
    bool rowsJitter = false;     // the rows carry each frame's raster phase (rowsJitter in the constants)
    bool taaPosition = false;    // the previous position is the output pixel's centre, one current phase from the texel's own (taa()'s call)
    int x0, y0, x1, y1;          // the current pixels examined
};
struct PixelInfo { int x, y, idNow, idThen; };

// The last two of the rules a sweep is given are today's rule (the point and footprint tests) and the point test alone; every other rule is tallied against them.
struct Tally {
    long pixels = 0;            // pixel-frames with a camera term
    long kept = 0;              // ... kept by this rule
    long solid = 0;             // footprint inside the image, four finite non-zero texels
    long todayRefused = 0;      // ... and no compared value within 0.1% of a threshold, and today's rule refuses it
    long keptOfThose = 0;       // ... of which this rule keeps
    long reached = 0;           // ... of which an own surface (half a) was found
    long reachedRefused = 0;    // ... and half b refused it
    long open = 0;              // footprint not solid
    long openDiffers = 0;       // ... where this rule and the point test disagree
    long differs = 0;           // where this rule and today's rule disagree
    long ambiguous = 0;         // solid pixels with a compared value within 0.1% of a threshold
};
using Select = std::function<bool(const PixelInfo&)>;
struct Set { const char* name; Select in; };
using Grid = std::vector<std::vector<Tally>>;   // [set][rule]

inline Grid sweep(const Case& c, const std::vector<Rule>& rules, const std::vector<Set>& sets) {
    const size_t today = rules.size() - 2, point = rules.size() - 1;
    Grid grid(sets.size(), std::vector<Tally>(rules.size()));
    const int fx0 = std::max(0, c.x0 - 4), fx1 = std::min(kW, c.x1 + 5), fy0 = std::max(0, c.y0 - 4), fy1 = std::min(kH, c.y1 + 5);
    std::vector<int> idsNow(size_t(kW) * kH, histscene::kSurfSky);
    std::vector<Verdict> out(rules.size());
    Image hist(kW, kH), cur(kW, kH);   // each phase rewrites the region the pixels read; nothing outside it is read
    for (unsigned n = 0; n < histdepth::rig16::kCycle; ++n) {
        float cx, cy, px, py;
        histdepth::rig16::phaseOf(n, &cx, &cy);
        histdepth::rig16::phaseOf(n + histdepth::rig16::kCycle - 1, &px, &py);
        histscene::render(hist, nullptr, c.then, c.thenPose, px, py, fx0, fy0, fx1, fy1);
        histscene::render(cur, &idsNow, c.now, c.nowPose, cx, cy, fx0, fy0, fx1, fy1);
        float nx = 0, ny = 0, ox = 0, oy = 0;
        if (c.rowsJitter) { rowsShift(cx, cy, &nx, &ny); rowsShift(px, py, &ox, &oy); }
        CurrentFrame frame;
        frame.depth = &cur;
        frame.now = histscene::camOf(c.nowPose, nx, ny);
        frame.old = histscene::camOf(c.thenPose, ox, oy);
        frame.jx = cx; frame.jy = cy;
        for (int y = c.y0; y < c.y1; ++y)
            for (int x = c.x0; x < c.x1; ++x) {
                const float d = cur.at(x, y);
                if (!(d > 0)) continue;
                const float u = (float(x) + 0.5f - cx) / float(kW), v = (float(y) + 0.5f - cy) / float(kH);
                Vec4 before;
                if (!histdepth::cameraBefore(frame.now, frame.old, u, v, d, &before)) continue;
                float pu = before.x / before.w * 0.5f + 0.5f, pv = before.y / before.w * -0.5f + 0.5f;
                const float expected = before.z / before.w;
                if (c.taaPosition) { pu += cx / float(kW); pv += cy / float(kH); }
                // The prep's own range checks.
                if (!(pu >= 0 && pu <= 1 && pv >= 0 && pv <= 1 && std::isfinite(expected) && expected >= 0 && expected <= 1)) continue;
                PixelInfo info{x, y, idsNow[size_t(y) * size_t(kW) + size_t(x)], histscene::castUv(c.then, c.thenPose, pu, pv).id};
                bool any = false;
                for (const Set& s : sets) any = any || s.in(info);
                if (!any) continue;
                for (size_t r = 0; r < rules.size(); ++r) out[r] = histdepth::decide(hist, pu, pv, px, py, expected, rules[r], &frame);
                for (size_t s = 0; s < sets.size(); ++s) {
                    if (!sets[s].in(info)) continue;
                    for (size_t r = 0; r < rules.size(); ++r) {
                        Tally& t = grid[s][r];
                        const Verdict& m = out[r];
                        ++t.pixels;
                        t.kept += m.kept;
                        t.differs += m.kept != out[today].kept;
                        if (!m.solid) { ++t.open; t.openDiffers += m.kept != out[point].kept; continue; }
                        ++t.solid;
                        if (m.margin < 1e-3f || out[today].margin < 1e-3f) { ++t.ambiguous; continue; }
                        if (out[today].kept) continue;
                        ++t.todayRefused;
                        t.keptOfThose += m.kept;
                        if (m.own >= 0) { ++t.reached; t.reachedRefused += !m.kept; }
                    }
                }
            }
    }
    return grid;
}

// The scenes. The eye is 1.7 m above the floor, looking along +z; 960x600 at 90 degrees across.
constexpr float kFarWall = 100.0f;
inline float hullDistance() { return 0.7f * (histscene::kEye / (0.15f * histscene::kTanY)); }   // a hull 30% nearer than the floor under its bottom edge at row 345
inline Case creaseCase() {
    Case c;
    c.then.prims = {histscene::floorPrim(), histscene::wallPrim(kFarWall, 3.0f)};
    c.now = c.then;
    c.x0 = 16; c.x1 = kW - 16; c.y0 = 296; c.y1 = 324;
    return c;
}
inline Case hullCase(float movePx, float recede) {
    const float z = hullDistance();
    Case c;
    c.then.prims = {histscene::floorPrim(), histscene::hullPrim(z, -1.5f, 1.5f, -1.2f, 1.0f)};
    const float dx = movePx * (2.0f * histscene::kTanX * z / float(kW));   // movePx pixels at the hull's distance
    c.now.prims = {histscene::floorPrim(), histscene::hullPrim(z * recede, -1.5f + dx, 1.5f + dx, -1.2f, 1.0f)};
    c.x0 = 380; c.x1 = 620; c.y0 = 300; c.y1 = 370;
    return c;
}
inline bool isFloor(const PixelInfo& i) { return i.idNow == histscene::kSurfFloor; }
inline bool isWall(const PixelInfo& i) { return i.idNow == histscene::kSurfWall; }
inline bool isHull(const PixelInfo& i) { return i.idNow == histscene::kSurfHull; }

inline double pct(long a, long b) { return b ? 100.0 * double(a) / double(b) : 0.0; }

// The scenes against every rule in `candidates` at once (one render of each frame serves all of them); sinks[r] collects what candidate r fails. Candidate 0 is
// the one that narrates.
inline void frames(std::vector<Sink>& sinks, const std::vector<Rule>& candidates) {
    std::vector<Rule> rules = candidates;
    rules.push_back(histdepth::beforeJunction());
    rules.push_back(histdepth::pointOnly());
    const size_t today = rules.size() - 2, point = rules.size() - 1;
    for (Sink& t : sinks) t.section = "frames";
    char what[400];
    const auto each = [&](auto&& check) { for (size_t r = 0; r < candidates.size(); ++r) check(sinks[r], r); };
    // 4a. A floor meeting a far wall, a still camera.
    {
        const Grid g = sweep(creaseCase(), rules, {{"floor", isFloor}, {"wall", isWall}});
        each([&](Sink& t, size_t r) {
            const Tally& f = g[0][r];
            std::snprintf(what, sizeof(what), "a floor meeting a wall 100 m away, a still camera: of the %ld floor pixel-frames with a solid footprint today's rule refuses, %ld are kept (every one)", f.todayRefused, f.keptOfThose);
            t.expect(f.todayRefused > 5000 && f.keptOfThose == f.todayRefused, what);
            t.expect(f.reached == f.todayRefused, "a floor meeting a wall: every one of them had its own surface beside the footprint (half a), so half b is what kept them");
            t.expect(g[1][r].differs == 0, "a floor meeting a wall: the wall's own pixels decide as they did");
            t.expect(f.ambiguous * 200 < f.pixels, "a floor meeting a wall: almost no pixel is within 0.1% of a threshold");
        });
        const Tally& f = g[0][0];
        if (sinks[0].narrate)
            std::printf("flat history junction: floor meeting a wall 100 m away, still camera, 32 phase pairs: of %ld floor pixel-frames the point test kept %.2f%%, today's rule %.2f%%; today's rule refused %ld with a "
                        "solid footprint, the junction test keeps %ld of them (%.2f%%); wall pixels: %ld differ from today's\n",
                        f.pixels, pct(g[0][point].kept, g[0][point].pixels), pct(g[0][today].kept, g[0][today].pixels), f.todayRefused, f.keptOfThose, pct(f.keptOfThose, f.todayRefused), g[1][0].differs);
    }
    // 4b. A floor under a parked hull (30% nearer), a still camera.
    {
        const Grid g = sweep(hullCase(0.0f, 1.0f), rules, {{"floor", isFloor}, {"hull", isHull}});
        each([&](Sink& t, size_t r) {
            const Tally& f = g[0][r];
            std::snprintf(what, sizeof(what), "a parked hull 30%% nearer than the floor: of the %ld floor pixel-frames at its edges that today's rule refuses, %ld are kept (every one)", f.todayRefused, f.keptOfThose);
            t.expect(f.todayRefused > 500 && f.keptOfThose == f.todayRefused, what);
            t.expect(g[1][r].differs == 0, "a parked hull: its own pixels decide as they did");
        });
        const Tally& f = g[0][0];
        if (sinks[0].narrate)
            std::printf("flat history junction: floor under a parked hull 30%% nearer, still camera: today's rule refused %ld solid floor pixel-frames at the hull's edges, the junction test keeps %ld (%.2f%%)\n",
                        f.todayRefused, f.keptOfThose, pct(f.keptOfThose, f.todayRefused));
    }
    // 4c. The ghost guard: the hull moves 2 px a frame; the floor it uncovers at its trailing edge shows what the history holds the hull's colour of.
    {
        const Set uncovered{"uncovered", [](const PixelInfo& i) { return isFloor(i) && i.idThen == histscene::kSurfHull; }};
        const Grid g = sweep(hullCase(2.0f, 1.0f), rules, {uncovered, {"covered", [](const PixelInfo& i) { return isHull(i) && i.idThen != histscene::kSurfHull; }}});
        each([&](Sink& t, size_t r) {
            const Tally& d = g[0][r];
            std::snprintf(what, sizeof(what), "an occluder moving 2 px a frame: the floor it uncovers (%ld pixel-frames with a solid footprint, which today's rule refuses) is refused, %ld kept", d.todayRefused, d.keptOfThose);
            t.expect(d.todayRefused > 1000 && d.keptOfThose == 0, what);
            std::snprintf(what, sizeof(what), "an occluder moving 2 px a frame: the case is reached, %ld of those pixels have their own surface beside the footprint and half b refuses %ld (the test is not vacuous)", d.reached, d.reachedRefused);
            t.expect(d.reached > 200 && d.reachedRefused == d.reached, what);
            t.expect(g[1][r].differs == 0, "an occluder moving 2 px a frame: the pixels it newly covers decide as today's rule did");
        });
        const Tally& d = g[0][0];
        if (sinks[0].narrate) {
            std::printf("flat history junction: hull moving 2 px a frame, still camera: %ld uncovered floor pixel-frames (history holds the hull), today's rule refuses %ld with a solid footprint; the junction test keeps %ld; "
                        "%ld reach half b, which refuses %ld\n", d.pixels, d.todayRefused, d.keptOfThose, d.reached, d.reachedRefused);
            for (float px : {1.0f, 0.5f}) {
                const Grid s1 = sweep(hullCase(px, 1.0f), rules, {uncovered});
                std::printf("flat history junction: the same hull at %.1f px a frame (a mover this slow cannot be told from a still surface, here as in the point test): %ld of %ld refused pixel-frames kept "
                            "(today's rule keeps %ld of %ld of all)\n", px, s1[0][0].keptOfThose, s1[0][0].todayRefused, s1[0][today].kept, s1[0][today].pixels);
            }
        }
    }
    // 4d. A slow camera over a static junction: strafing and walking 0.05 m a frame, and a turn; the rows may carry the jitter.
    {
        struct Move { const char* name; float dx, dz, dyaw; bool rows; };
        static const Move moves[] = {{"strafing +0.05 m a frame", 0.05f, 0, 0, false}, {"strafing -0.05 m a frame", -0.05f, 0, 0, false}, {"walking 0.05 m a frame", 0, 0.05f, 0, false},
                                     {"walking back 0.05 m a frame", 0, -0.05f, 0, false}, {"turning 0.1 degrees a frame", 0, 0, 0.0017453f, false},
                                     {"strafing +0.05 m a frame, rows carrying the jitter", 0.05f, 0, 0, true}, {"a still camera, rows carrying the jitter", 0, 0, 0, true}};
        for (const Move& m : moves) {
            Case c = creaseCase();
            c.thenPose.o[0] = -m.dx; c.thenPose.o[2] = -m.dz; c.thenPose.yaw = -m.dyaw;
            c.rowsJitter = m.rows;
            c.x0 = 24; c.x1 = kW - 24;
            const Grid g = sweep(c, rules, {{"floor", isFloor}});
            each([&](Sink& t, size_t r) {
                const Tally& f = g[0][r];
                std::snprintf(what, sizeof(what), "a floor meeting a wall, %s: of the %ld solid floor pixel-frames today's rule refuses, %ld are kept (every one)", m.name, f.todayRefused, f.keptOfThose);
                t.expect(f.todayRefused > 5000 && f.keptOfThose == f.todayRefused, what);
            });
            const Tally& f = g[0][0];
            if (sinks[0].narrate) std::printf("flat history junction: floor meeting a wall, %s: today's rule refused %ld, kept %ld (%.2f%%)\n", m.name, f.todayRefused, f.keptOfThose, pct(f.keptOfThose, f.todayRefused));
        }
        // The same with a parked hull, whose edge a step sideways uncovers by about half a pixel: the floor whose exact previous position was the floor is kept.
        for (int rows = 0; rows < 2; ++rows)
            for (float dx : {0.05f, -0.05f}) {
                Case c = hullCase(0.0f, 1.0f);
                c.thenPose.o[0] = -dx;
                c.rowsJitter = rows != 0;
                const Grid g = sweep(c, rules, {{"floor, floor", [](const PixelInfo& i) { return isFloor(i) && i.idThen == histscene::kSurfFloor; }}});
                each([&](Sink& t, size_t r) {
                    const Tally& f = g[0][r];
                    std::snprintf(what, sizeof(what), "a floor under a parked hull, strafing %+.2f m a frame%s: of the %ld solid pixel-frames of floor that was floor, today's rule refuses, %ld are kept (every one)", dx,
                                  rows ? ", rows carrying the jitter" : "", f.todayRefused, f.keptOfThose);
                    t.expect(f.todayRefused > 400 && f.keptOfThose == f.todayRefused, what);
                });
            }
    }
    // 4e. A mover receding on a plane: a plate 2% (and 1.2%) deeper than it was, a still camera. Nothing it covers, nothing at its edge, is kept that was not.
    for (float recede : {1.02f, 1.012f}) {
        const Grid g = sweep(hullCase(0.0f, recede), rules, {{"plate", isHull}, {"floor", isFloor}});
        each([&](Sink& t, size_t r) {
            const Tally& h = g[0][r];
            std::snprintf(what, sizeof(what), "a plate receding %.1f%% a frame: %ld solid pixel-frames of it are refused by today's rule, %ld are kept", (recede - 1.0f) * 100.0f, h.todayRefused, h.keptOfThose);
            t.expect(h.todayRefused > 100000 && h.keptOfThose == 0 && h.differs == 0, what);
            std::snprintf(what, sizeof(what), "a plate receding %.1f%% a frame: the floor around it keeps no pixel today's rule refused (%ld refused, %ld kept)", (recede - 1.0f) * 100.0f, g[1][r].todayRefused, g[1][r].keptOfThose);
            t.expect(g[1][r].todayRefused > 1000 && g[1][r].keptOfThose == 0, what);
        });
    }
    // 4f. The horizon: a footprint that holds the sky is decided by the point test alone, as before.
    {
        Case c;
        c.then.prims = {histscene::floorPrim()};
        c.now = c.then;
        c.x0 = 16; c.x1 = kW - 16; c.y0 = 296; c.y1 = 340;
        const Grid g = sweep(c, rules, {{"all", [](const PixelInfo&) { return true; }}});
        each([&](Sink& t, size_t r) {
            const Tally& a = g[0][r];
            std::snprintf(what, sizeof(what), "the horizon: %ld pixel-frames whose footprint holds the sky or leaves the frame, %ld of them decided other than by the point test", a.open, a.openDiffers);
            t.expect(a.open > 1000 && a.openDiffers == 0, what);
            t.expect(a.solid > 10000, "the horizon: and the pixels below it, whose footprint is solid, are in the sweep");
        });
    }
    // 4g. The image's border: the wall texel of a junction at the first or last column projects outside the image and refuses; one column in, it does not.
    {
        std::vector<long> edgeRefused(candidates.size(), 0), edgeKept(candidates.size(), 0), innerKept(candidates.size(), 0), innerRefused(candidates.size(), 0);
        for (int side = 0; side < 2; ++side) {
            Case c = creaseCase();
            c.x0 = side ? kW - 8 : 0; c.x1 = side ? kW : 8;
            const Grid g = sweep(c, rules, {{"edge", [side](const PixelInfo& i) { return isFloor(i) && i.x == (side ? kW - 1 : 0); }},
                                            {"inner", [side](const PixelInfo& i) { return isFloor(i) && i.x != (side ? kW - 1 : 0); }}});
            for (size_t r = 0; r < candidates.size(); ++r) {
                edgeRefused[r] += g[0][r].todayRefused - g[0][r].keptOfThose;
                edgeKept[r] += g[0][r].keptOfThose;
                innerKept[r] += g[1][r].keptOfThose;
                innerRefused[r] += g[1][r].todayRefused - g[1][r].keptOfThose;
            }
        }
        each([&](Sink& t, size_t r) {
            std::snprintf(what, sizeof(what), "the image's first and last columns: %ld junction pixel-frames whose texel projects outside the image are refused, %ld kept; one column in, %ld kept and %ld refused", edgeRefused[r], edgeKept[r], innerKept[r], innerRefused[r]);
            t.expect(edgeRefused[r] > 5 && edgeKept[r] == 0 && innerKept[r] > 50 && innerRefused[r] == 0, what);
        });
    }
    // 4h. taa() hands the check the previous position of the output pixel's centre: narrated.
    if (sinks[0].narrate) {
        Case c = creaseCase();
        c.taaPosition = true;
        const Grid g = sweep(c, rules, {{"floor", isFloor}});
        std::printf("flat history junction: the same floor and wall with taa()'s call (the output pixel's previous position, one current phase from the texel's own): today's rule keeps %.2f%% of %ld floor "
                    "pixel-frames, of the %ld it refuses with a solid footprint the junction test keeps %ld\n", pct(g[0][today].kept, g[0][today].pixels), g[0][today].pixels, g[0][0].todayRefused, g[0][0].keptOfThose);
    }
}

// ---- 5. the rule, and its mutants -----------------------------------------------------------------------------------------------------------
// Everything above against every rule given, in one pass; the failures of each. The first rule narrates what it measures.
inline std::vector<int> runAll(const std::vector<Rule>& rules, bool narrate) {
    std::vector<Sink> sinks(rules.size());
    for (size_t r = 0; r < rules.size(); ++r) {
        sinks[r].narrate = narrate && r == 0;
        sinks[r].quiet = !sinks[r].narrate;
        cameraTests(sinks[r], rules[r]);
        unitCases(sinks[r], rules[r]);
    }
    frames(sinks, rules);
    std::vector<int> failures;
    for (const Sink& t : sinks) failures.push_back(t.failures);
    return failures;
}
inline int run(const Rule& rule, bool narrate) { return runAll({rule}, narrate)[0]; }
inline int runText(bool) {
    Sink t;
    textPins(t);
    return t.failures;
}

}  // namespace flat_history_junction
