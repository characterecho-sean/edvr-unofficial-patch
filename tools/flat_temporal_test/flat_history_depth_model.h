#pragma once
// THE HISTORY DEPTH CHECK ON THE CPU (src/d3d11/flat_mono_shader_source.h, historyDepthMatches; design doc sections 82 and 104).
//
// The check decides whether last frame's depth confirms that a pixel's history shows the surface the pixel shows now. This is the same decision in
// float32, statement for statement, so a case that is too large for the GPU rig (a 960x600 frame under the shipped jitter cycle) can run here, and so
// the GPU rig (tools\flat_mono_resolve_test\flat_taa_history_depth_gpu_tests.h) can compare the kernel with it pixel for pixel. tools\flat_temporal_test
// pins the HLSL's text to this; the GPU rig pins its numbers.
//
// Rule (the shader's comment above the function has the reasoning):
//   1. The point test: the best of the four history-depth texels around the reprojected raster position is within tol of `expected`,
//      tol = max(floor, expected * rel). This is what the check was.
//   2. When it refuses, the footprint test: the four texels all lie inside the image, are finite and non-zero, are planar (the cross difference
//      |d00 + d11 - d10 - d01| is within tol) and their bilinear value at the exact position is within tol of `expected`.
//   3. When both refuse, the junction test (two halves, both required; the four texels of 2 must be inside the image and solid to get here):
//      a. the pixel's own surface is present: of the four quads edge-adjacent to the footprint (the 2x2 moved by +x, -x, +y, -y, asked in that
//         order, the first that qualifies is the one), one is wholly inside the image, holds four finite non-zero texels, is planar (cross
//         difference within tol) and its bilinear EXTRApolation to the exact previous raster position is within tol of `expected`;
//      b. every other surface persists: each footprint texel more than tol from that quad's plane at the texel is carried into this frame with
//         cameraAfter (the mirror of the camera term; below), must land with its four current depth texels wholly inside the image, and the best
//         of those four must be within max(floor, rel * projected depth) of the projected depth.
//
// A Rule with fields changed is a mutant of the rule; the tests hold each one to failing.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "../../src/common/temporal_math.h"

namespace histdepth {

constexpr float kRel = 0.01f;     // flat_mono_refusal.h: kFlatMonoStaleDepthRelative
constexpr float kFloor = 1e-6f;   // flat_mono_refusal.h: kFlatMonoStaleDepthFloor

struct Image {
    int w = 0, h = 0;
    std::vector<float> d;
    Image() = default;
    Image(int width, int height) : w(width), h(height), d(size_t(width) * size_t(height), 0.0f) {}
    float& at(int x, int y) { return d[size_t(y) * size_t(w) + size_t(x)]; }
    // HistoryDepth.Load(clamp(...)): a coordinate outside the image reads the edge texel.
    float load(int x, int y) const {
        x = std::min(std::max(x, 0), w - 1);
        y = std::min(std::max(y, 0), h - 1);
        return d[size_t(y) * size_t(w) + size_t(x)];
    }
};

struct Rule {
    float rel = kRel, floor = kFloor;
    bool footprint = true;        // false: the point test alone, the check as it was before the footprint test
    bool planar = true;           // false: the cross difference is not asked
    float planarScale = 1.0f;     // the cross difference's tolerance, in tol
    bool exactPosition = true;    // false: the mean of the four texels stands for their value at the exact position
    bool swappedWeights = false;  // true: the horizontal fraction weights the rows and the vertical one the columns
    bool wholeFootprint = true;   // false: a footprint partly outside the image is asked, its edge texels clamped
    bool solid = true;            // false: a zero (sky, cleared) or non-finite texel does not bar the footprint
    // The junction test (3).
    bool junction = true;         // false: the third test is not asked (the check as it was before it)
    bool persist = true;          // false: half b is not asked, the pixel's own surface beside the footprint is enough
    bool insideCheck = true;      // false: a footprint texel that lands outside the image reads the edge texels of the current depth
    bool reverseOrigin = false;   // true: cameraAfter adds the camera-origin term as cameraBefore does, instead of reversing it
    bool unjitter = true;         // false: cameraAfter keeps the NDC shift the rows carry
    bool ownPlanar = true;        // false: the adjacent quad's cross difference is not asked
    bool ownExtrapolation = true; // false: the adjacent quad's plane at the exact position is not compared with `expected`
    bool ownWhole = true;         // false: a quad partly outside the image is asked, its edge texels repeated
    bool ownSolid = true;         // false: a zero or non-finite texel does not bar the adjacent quad
    bool reversedOrder = false;   // true: the quads are asked up, down, left, right
    float offPlaneScale = 1.0f;   // the distance from the quad's plane, in tol, at which a footprint texel is another surface
};

// The check as it was before the footprint test: the point test alone.
inline Rule pointOnly() { Rule r; r.footprint = false; r.junction = false; return r; }
// The check as it was before the junction test: the point test and the footprint test.
inline Rule beforeJunction() { Rule r; r.junction = false; return r; }

struct Verdict {
    bool kept = false;
    bool byFootprint = false;     // kept by the footprint test after the point test refused
    bool byJunction = false;      // kept by the junction test after the point and footprint tests refused
    bool inside = false;          // the 2x2 footprint lies wholly inside the image
    bool solid = false;           // ... and all four texels are finite and non-zero
    int own = -1;                 // the junction test's adjacent quad: 0 +x, 1 -x, 2 +y, 3 -y, or -1 (none qualified, or the test was not asked)
    int offPlane = 0;             // footprint texels it found off that quad's plane
    float margin = 1e30f;         // the smallest relative distance of a compared value from its threshold (an arithmetic ulp's worth of safety)
    float lowest = 0, highest = 0;   // the smallest and the largest of the four texels around the position (edge texels repeated)
};

// ---- the camera term and its inverse, in float32 ---------------------------------------------------------------------------------------------
// The rows are b1[270..275] as the shader's `now` and `old` hold them: rows 0..2 the clip image of a camera-relative world axis (x and y in
// components 0 and 1, the view direction's depth in 3, nothing in 2: reversed-Z with no far plane), row 3 the near term in component 2, row 4 the
// view direction and row 5 the camera origin. ndcX and ndcY are the NDC shift the rows themselves carry (rowsJitter): the shader removes it.
struct Vec3 { float x, y, z; };
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Cam {
    float rows[6][4] = {};
    float ndcX = 0, ndcY = 0;
};
inline Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec4 unjitterRow(const float* r, float ndcX, float ndcY) {
    Vec4 o;
    o.x = r[0]; o.y = r[1]; o.z = r[2]; o.w = r[3];
    if (ndcX == 0 && ndcY == 0) return o;
    o.x -= ndcX * r[3];
    o.y -= ndcY * r[3];
    return o;
}
inline Vec4 madd4(Vec3 p, const Vec4& r0, const Vec4& r1, const Vec4& r2, float iz, const Vec4& r3) {
    Vec4 o;
    o.x = p.x * r0.x + p.y * r1.x + p.z * r2.x + iz * r3.x;
    o.y = p.x * r0.y + p.y * r1.y + p.z * r2.y + iz * r3.y;
    o.z = p.x * r0.z + p.y * r1.z + p.z * r2.z + iz * r3.z;
    o.w = p.x * r0.w + p.y * r1.w + p.z * r2.w + iz * r3.w;
    return o;
}
// cameraBefore and cameraAfter are one function with the two cameras exchanged: the point under (u, v) at `depth` under `from`'s rows is solved with those
// rows, moved to `to`'s camera origin (the position is relative to its camera's origin and scaled by 1/z: the difference of origins is scaled by iz) and
// projected with `to`'s rows. originSign = -1 keeps the sign cameraBefore moves with, which is wrong going the other way (the mutant).
inline bool transfer(const Cam& from, const Cam& to, float u, float v, float depth, Vec4* out, bool unjitter = true, float originSign = 1.0f) {
    const float fj[2] = {unjitter ? from.ndcX : 0.0f, unjitter ? from.ndcY : 0.0f};
    const float tj[2] = {unjitter ? to.ndcX : 0.0f, unjitter ? to.ndcY : 0.0f};
    const Vec4 f0 = unjitterRow(from.rows[0], fj[0], fj[1]), f1 = unjitterRow(from.rows[1], fj[0], fj[1]);
    const Vec4 f2 = unjitterRow(from.rows[2], fj[0], fj[1]), f3 = unjitterRow(from.rows[3], fj[0], fj[1]);
    const Vec4 t0 = unjitterRow(to.rows[0], tj[0], tj[1]), t1 = unjitterRow(to.rows[1], tj[0], tj[1]);
    const Vec4 t2 = unjitterRow(to.rows[2], tj[0], tj[1]), t3 = unjitterRow(to.rows[3], tj[0], tj[1]);
    const Vec3 a{f0.x, f1.x, f2.x}, b{f0.y, f1.y, f2.y}, c{f0.w, f1.w, f2.w};
    const Vec3 ca = cross3(b, c), cb = cross3(c, a), cc = cross3(a, b);
    const float det = dot3(a, ca), iz = depth / f3.z;
    const Vec3 rhs{(u * 2.0f + (-1.0f)) - f3.x * iz, (v * (-2.0f) + 1.0f) - f3.y * iz, 1.0f - f3.w * iz};
    Vec3 p{(ca.x * rhs.x + cb.x * rhs.y + cc.x * rhs.z) / det, (ca.y * rhs.x + cb.y * rhs.y + cc.y * rhs.z) / det,
           (ca.z * rhs.x + cb.z * rhs.y + cc.z * rhs.z) / det};
    p.x += originSign * ((from.rows[5][0] - to.rows[5][0]) * iz);
    p.y += originSign * ((from.rows[5][1] - to.rows[5][1]) * iz);
    p.z += originSign * ((from.rows[5][2] - to.rows[5][2]) * iz);
    *out = madd4(p, t0, t1, t2, iz, t3);
    return out->w > 0 && std::isfinite(out->x) && std::isfinite(out->y) && std::isfinite(out->z) && std::isfinite(out->w);
}
// The shader's cameraBefore: this frame's raster pixel at (u, v), unjittered, to last frame. before.xy / before.w is the NDC position, z / w the depth.
inline bool cameraBefore(const Cam& now, const Cam& old, float u, float v, float depth, Vec4* before) {
    return transfer(now, old, u, v, depth, before);
}
// The shader's cameraAfter: last frame's (u, v) at last frame's `depth` to this frame. Its origin term has the opposite sign of cameraBefore's.
inline bool cameraAfter(const Cam& now, const Cam& old, float u, float v, float depth, Vec4* after, bool unjitter = true, bool reverseOrigin = false) {
    return transfer(old, now, u, v, depth, after, unjitter, reverseOrigin ? -1.0f : 1.0f);
}

// What the junction test reads besides the history: this frame's depth (SceneDepth), the two cameras and this frame's raster phase (jitter.xy; the
// previous phase, jitter.zw, is decide's jx, jy), and the scale the history depth carries (foregroundDepth.x with the SDK foreground map, else 1).
struct CurrentFrame {
    const Image* depth = nullptr;
    Cam now, old;
    float jx = 0, jy = 0;
    float scale = 1.0f;
};
// The camera of the GPU rig and of the still-camera scenes: a unit-tangent perspective at the origin looking along +z (tools\flat_mono_resolve_test's camera()).
inline Cam stillCamera() {
    Cam c;
    c.rows[0][0] = c.rows[1][1] = c.rows[2][3] = 1.0f;
    c.rows[3][2] = 0.025f;
    c.rows[4][2] = 1.0f;
    return c;
}
inline float lerpf(float a, float b, float s) { return a + s * (b - a); }
// lerp(lerp(d00, d10, fx), lerp(d01, d11, fx), fy): the shader's quadDepthAt. fx and fy outside [0, 1] extrapolate.
inline float quadDepth(const float* q, float fx, float fy) { return lerpf(lerpf(q[0], q[1], fx), lerpf(q[2], q[3], fx), fy); }

// prevU, prevV: the camera term's previous position in [0,1] (unjittered); jx, jy: the previous frame's raster phase in render pixels.
// `cur` is what the junction test reads of this frame; without it (or without the rule's junction) the third test refuses.
inline Verdict decide(const Image& hist, float prevU, float prevV, float jx, float jy, float expected, const Rule& rule = Rule(), const CurrentFrame* cur = nullptr) {
    Verdict v;
    const float rx = prevU * float(hist.w) + jx - 0.5f, ry = prevV * float(hist.h) + jy - 0.5f;
    const int bx = int(std::floor(rx)), by = int(std::floor(ry));
    const float tol = std::max(rule.floor, expected * rule.rel);
    const auto note = [&](float value, float threshold) {
        const float t = std::max(std::fabs(threshold), 1e-30f);
        v.margin = std::min(v.margin, std::fabs(value - threshold) / t);
    };
    float best = 3.402823e38f;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) best = std::min(best, std::fabs(hist.load(bx + x, by + y) - expected));
    note(best, tol);
    v.inside = bx >= 0 && by >= 0 && bx < hist.w - 1 && by < hist.h - 1;
    {
        const float a = hist.load(bx, by), b = hist.load(bx + 1, by), c = hist.load(bx, by + 1), e = hist.load(bx + 1, by + 1);
        v.lowest = std::min(std::min(a, b), std::min(c, e));
        v.highest = std::max(std::max(a, b), std::max(c, e));
        v.solid = v.inside && std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(e) && a > 0 && b > 0 && c > 0 && e > 0;
    }
    if (best <= tol) { v.kept = true; return v; }
    if (!rule.footprint && !rule.junction) return v;
    if (rule.wholeFootprint && !v.inside) return v;
    const float d00 = hist.load(bx, by), d10 = hist.load(bx + 1, by), d01 = hist.load(bx, by + 1), d11 = hist.load(bx + 1, by + 1);
    if (rule.solid) {
        const bool finite = std::isfinite(d00) && std::isfinite(d10) && std::isfinite(d01) && std::isfinite(d11);
        if (!finite || !(d00 > 0 && d10 > 0 && d01 > 0 && d11 > 0)) return v;
    }
    const float fx = rx - float(bx), fy = ry - float(by);
    if (rule.footprint) {
        const float wx = rule.swappedWeights ? fy : fx, wy = rule.swappedWeights ? fx : fy;
        const float top = d00 + wx * (d10 - d00), bottom = d01 + wx * (d11 - d01);
        const float value = rule.exactPosition ? top + wy * (bottom - top) : 0.25f * (d00 + d10 + d01 + d11);
        note(std::fabs(value - expected), tol);
        const float cross = std::fabs(d00 + d11 - d10 - d01);
        if (rule.planar) note(cross, tol * rule.planarScale);
        const bool depthOk = std::fabs(value - expected) <= tol;
        const bool planarOk = !rule.planar || cross <= tol * rule.planarScale;
        if (depthOk && planarOk) { v.kept = true; v.byFootprint = true; return v; }
    }
    if (!rule.junction || !cur || !cur->depth) return v;

    // 3a. The pixel's own surface: the first quad edge-adjacent to the footprint that is a plane at `expected`.
    static const int kShift[2][4][2] = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}, {{0, -1}, {0, 1}, {-1, 0}, {1, 0}}};
    const float footprint[4] = {d00, d10, d01, d11};
    float own[4] = {0, 0, 0, 0};
    int ownX = 0, ownY = 0;
    for (int i = 0; i < 4 && v.own < 0; ++i) {
        const int ax = bx + kShift[rule.reversedOrder ? 1 : 0][i][0], ay = by + kShift[rule.reversedOrder ? 1 : 0][i][1];
        if (rule.ownWhole && (ax < 0 || ay < 0 || ax >= hist.w - 1 || ay >= hist.h - 1)) continue;
        const float q[4] = {hist.load(ax, ay), hist.load(ax + 1, ay), hist.load(ax, ay + 1), hist.load(ax + 1, ay + 1)};
        if (rule.ownSolid) {
            bool good = true;
            for (float t : q) good = good && std::isfinite(t) && t > 0;
            if (!good) continue;
        }
        const float cross = std::fabs(q[0] + q[3] - q[1] - q[2]);
        if (rule.ownPlanar) {
            note(cross, tol);
            if (!(cross <= tol)) continue;
        }
        const float at = quadDepth(q, rx - float(ax), ry - float(ay));
        if (rule.ownExtrapolation) {
            note(std::fabs(at - expected), tol);
            if (!(std::fabs(at - expected) <= tol)) continue;
        }
        v.own = i;
        ownX = ax; ownY = ay;
        for (int k = 0; k < 4; ++k) own[k] = q[k];
    }
    if (v.own < 0) return v;
    if (!rule.persist) { v.kept = true; v.byJunction = true; return v; }

    // 3b. Every footprint texel off that plane is another surface, and must still be there.
    for (int k = 0; k < 4; ++k) {
        const int tx = bx + (k & 1), ty = by + (k >> 1);
        const float then = footprint[k];
        const float onPlane = std::fabs(then - quadDepth(own, float(tx - ownX), float(ty - ownY)));
        note(onPlane, tol * rule.offPlaneScale);
        if (onPlane <= tol * rule.offPlaneScale) continue;
        ++v.offPlane;
        Vec4 after;
        if (!cameraAfter(cur->now, cur->old, (float(tx) + 0.5f - jx) / float(hist.w), (float(ty) + 0.5f - jy) / float(hist.h), then / cur->scale, &after,
                         rule.unjitter, rule.reverseOrigin))
            return v;
        const float e = after.z / after.w;
        const float cx = (after.x / after.w * 0.5f + 0.5f) * float(hist.w) + cur->jx - 0.5f;
        const float cy = (after.y / after.w * -0.5f + 0.5f) * float(hist.h) + cur->jy - 0.5f;
        int cbx = 0, cby = 0;
        if (rule.insideCheck) {
            if (!std::isfinite(e) || !(cx >= 0 && cy >= 0 && cx < float(hist.w - 1) && cy < float(hist.h - 1))) return v;
            cbx = int(std::floor(cx));
            cby = int(std::floor(cy));
        } else {
            if (!std::isfinite(e) || !std::isfinite(cx) || !std::isfinite(cy)) return v;
            cbx = std::min(std::max(int(std::floor(cx)), 0), hist.w - 2);
            cby = std::min(std::max(int(std::floor(cy)), 0), hist.h - 2);
        }
        float seen = 3.402823e38f;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) seen = std::min(seen, std::fabs(cur->depth->load(cbx + x, cby + y) - e));
        const float tolNow = std::max(rule.floor, e * rule.rel);
        note(seen, tolNow);
        if (!(seen <= tolNow)) return v;
    }
    v.kept = true;
    v.byJunction = true;
    return v;
}

// The depth the footprint holds at the exact position: what the footprint test compares with `expected`. Tests build a surface "whose depth is
// the interpolation" from it.
inline float bilinearAt(const Image& hist, float prevU, float prevV, float jx, float jy) {
    const float rx = prevU * float(hist.w) + jx - 0.5f, ry = prevV * float(hist.h) + jy - 0.5f;
    const int bx = int(std::floor(rx)), by = int(std::floor(ry));
    const float fx = rx - float(bx), fy = ry - float(by);
    const float d00 = hist.load(bx, by), d10 = hist.load(bx + 1, by), d01 = hist.load(bx, by + 1), d11 = hist.load(bx + 1, by + 1);
    const float top = d00 + fx * (d10 - d00), bottom = d01 + fx * (d11 - d01);
    return top + fy * (bottom - top);
}


// ---- the GPU rig's scenes ---------------------------------------------------------------------------------------------------------------------
// tools\flat_mono_resolve_test runs the taa kernel on a 16 x 16 frame whose current phase is 0 (so the colour a pixel samples is exactly its texel's) and
// whose previous phase is (px, py). A scene here is the pair of depth images that rig is handed, plus what this model says each pixel does; the rig
// compares the kernel with it, and tools\flat_temporal_test holds each scene to what it is for.
namespace rig16 {
constexpr int N = 16;

struct Scene {
    const char* name;
    std::vector<float> before, now;   // last frame's depth (rendered at phase (px, py)) and this frame's (phase 0), row-major
    float px, py;
};

// A surface is the depth it has at a true (unjittered) position, 0 for sky. A texel (x, y) rendered at phase (jx, jy) holds the surface at (x + .5 - jx, y + .5 - jy).
// floorAt falls 5% per pixel at the top row to 2.8% at the bottom one (d = .01 * (1 + y / 20): 1 / (20 + y) of itself per row) and has no slope in x;
// tiltAt also falls along x.
inline float floorAt(float, float yt) { return 0.01f * (1.0f + yt / 20.0f); }
inline float tiltAt(float xt, float yt) { return 0.01f * (1.0f + (yt + 0.5f * xt) / 20.0f); }

template <class F>
inline std::vector<float> render(F surface, float jx, float jy) {
    std::vector<float> z(size_t(N) * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) z[size_t(y) * N + x] = surface(float(x) + 0.5f - jx, float(y) + 0.5f - jy);
    return z;
}
inline Image image(const std::vector<float>& z) {
    Image m(N, N);
    m.d = z;
    return m;
}

// A still plane: the same surface now and last frame.
inline Scene slope(const char* name, float px, float py, bool tilted) {
    Scene s{name, {}, {}, px, py};
    if (tilted) { s.before = render(tiltAt, px, py); s.now = render(tiltAt, 0, 0); }
    else { s.before = render(floorAt, px, py); s.now = render(floorAt, 0, 0); }
    return s;
}
// The plane last frame was `factor` times as deep (2% a frame is a surface moving 2% of its range between frames).
inline Scene mover(const char* name, float px, float py, float factor) {
    Scene s{name, {}, {}, px, py};
    s.before = render(floorAt, px, py);
    for (float& v : s.before) v *= factor;
    s.now = render(floorAt, 0, 0);
    return s;
}
// A near object last frame (columns 5..9, rows 5..9), the floor behind it now.
inline Scene object(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    s.before = render(floorAt, px, py);
    for (int y = 5; y <= 9; ++y)
        for (int x = 5; x <= 9; ++x) s.before[size_t(y) * N + x] = 0.04f;
    s.now = render(floorAt, 0, 0);
    return s;
}
// A sloped roof against the sky whose edge was 1.5 pixels farther right last frame.
inline Scene skyEdge(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    const auto roofNow = [](float xt, float yt) { return xt >= 6.0f ? floorAt(xt, yt) : 0.0f; };
    const auto roofThen = [](float xt, float yt) { return xt >= 7.5f ? floorAt(xt, yt) : 0.0f; };
    s.before = render(roofThen, px, py);
    s.now = render(roofNow, 0, 0);
    return s;
}
// A plane that reaches depth 0 exactly on the centre of texel row 7 last frame (its horizon), the sky above it: the zero texel is consistent with the plane,
// and the footprints that hold it are still not asked.
inline Scene horizon(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    const float h0 = 7.5f - py;   // texel row 7, rendered at phase py, is centred on true row 7.5 - py
    const auto plane = [h0](float, float yt) { return yt > h0 ? 0.01f * (yt - h0) / 20.0f : 0.0f; };
    s.before = render(plane, px, py);
    s.now = render(plane, 0, 0);
    return s;
}
// A checker of two depths 3% apart last frame, and a surface this frame whose depth at each pixel is the checker's interpolation at the exact previous position.
inline Scene interpolated(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    s.before.resize(size_t(N) * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) s.before[size_t(y) * N + x] = ((x + y) & 1) ? 0.0103f : 0.0100f;
    const Image m = image(s.before);
    s.now.resize(size_t(N) * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) s.now[size_t(y) * N + x] = bilinearAt(m, (float(x) + 0.5f) / N, (float(y) + 0.5f) / N, px, py);
    return s;
}

// A static junction: a steep floor (d = .01 * (1 + y / 4): 1 / (4 + y) of itself per row, 10% at the junction) that meets a wall, the wall as deep as the
// floor is where they meet and flat above it, the same surface in both frames. The junction is half way between the last frame's samples of rows 5 and 6 (true
// row 6 - py), where the footprint test, which reads the kink as a slope, is furthest from the surface.
inline float steepAt(float, float yt) { return 0.01f * (1.0f + yt / 4.0f); }
inline Scene crease(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    const float yc = 6.0f - py;
    const auto surface = [yc](float xt, float yt) { return yt >= yc ? steepAt(xt, yt) : steepAt(xt, yc); };
    s.before = render(surface, px, py);
    s.now = render(surface, 0, 0);
    return s;
}
// A hull in front of the floor (4 times as shallow in z: depth 0.04), columns 5..9 and rows 5..9 in true coordinates, shifted `dx` columns between the frames.
inline float hullAt(float xt, float yt, float shift) {
    return (xt >= 5.0f + shift && xt < 10.0f + shift && yt >= 5.0f && yt < 10.0f) ? 0.04f : floorAt(xt, yt);
}
// A parked hull: the same in both frames.
inline Scene parked(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    s.before = render([](float x, float y) { return hullAt(x, y, 0.0f); }, px, py);
    s.now = render([](float x, float y) { return hullAt(x, y, 0.0f); }, 0, 0);
    return s;
}
// A hull that moved 2 pixels to the right between the frames (the floor at its trailing edge is disoccluded).
inline Scene moved(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    s.before = render([](float x, float y) { return hullAt(x, y, 0.0f); }, px, py);
    s.now = render([](float x, float y) { return hullAt(x, y, 2.0f); }, 0, 0);
    return s;
}
// A tilted patch two texels wide and high on the floor, still: the one pixel whose footprint is the whole patch is kept by the footprint test alone, no
// quad beside it is a plane.
inline float islandAt(float xt, float yt) {
    return (xt >= 6.0f && xt < 8.0f && yt >= 6.0f && yt < 8.0f) ? 0.01f * (1.0f + 0.1f * xt + 1.0f * yt) : floorAt(xt, yt);
}
inline Scene island(const char* name, float px, float py) {
    Scene s{name, {}, {}, px, py};
    s.before = render(islandAt, px, py);
    s.now = render(islandAt, 0, 0);
    return s;
}

// The cases the GPU rig runs, by kind and by the index of the previous frame's phase in the 32-phase cycle (temporalJitter(n, count 32)): the phases
// are the shipped Halton points, picked where the point test alone refuses most of the scene and the previous phase is not within a few hundredths of
// a pixel of a texel centre (where one ulp of the camera term's arithmetic could move the footprint).
//   f a floor (no slope in x), t a tilted plane, m a floor 2% deeper last frame, o a near object that moved away, e a roof against the sky,
//   c a checker whose interpolation the surface is, h a horizon that ends on a texel row, w a floor meeting a wall (a crease), p a parked hull in
//   front of the floor, g a hull that moved 2 pixels (the floor at its trailing edge is disoccluded), i a tilted patch two texels wide.
struct Plan { char kind; unsigned n; };
inline const Plan* plan(size_t* count) {
    static const Plan p[] = {{'f', 2}, {'f', 7}, {'f', 4}, {'f', 5}, {'t', 6}, {'m', 1}, {'m', 28}, {'o', 2}, {'e', 7},
                             {'c', 9}, {'c', 10}, {'h', 2}, {'h', 4}, {'w', 2}, {'w', 14}, {'p', 2}, {'p', 4}, {'g', 2}, {'g', 7}, {'i', 1}, {'i', 19}};
    *count = sizeof(p) / sizeof(p[0]);
    return p;
}
inline const char* kindName(char k) {
    switch (k) {
        case 'f': return "a still floor";
        case 't': return "a still tilted plane";
        case 'm': return "a floor 2% deeper last frame";
        case 'o': return "a near object that moved away";
        case 'e': return "a roof against the sky";
        case 'c': return "a surface that is a checker's interpolation";
        case 'h': return "a horizon on a texel row";
        case 'w': return "a floor meeting a wall";
        case 'p': return "a parked hull";
        case 'g': return "a hull that moved 2 pixels";
        case 'i': return "a two-texel tilted patch";
        default: return "?";
    }
}
constexpr unsigned kCycle = 32;
inline void phaseOf(unsigned n, float* x, float* y) { edvr::temporalJitter(n % kCycle, x, y, kCycle); }
inline Scene build(const Plan& p) {
    float px = 0, py = 0;
    phaseOf(p.n, &px, &py);
    const char* name = kindName(p.kind);
    switch (p.kind) {
        case 'f': return slope(name, px, py, false);
        case 't': return slope(name, px, py, true);
        case 'm': return mover(name, px, py, 1.02f);
        case 'o': return object(name, px, py);
        case 'e': return skyEdge(name, px, py);
        case 'c': return interpolated(name, px, py);
        case 'w': return crease(name, px, py);
        case 'p': return parked(name, px, py);
        case 'g': return moved(name, px, py);
        case 'i': return island(name, px, py);
        default: return horizon(name, px, py);
    }
}

// What `rule` says of every pixel of a scene (the camera is still, so a pixel's previous position is itself and its expected depth is its own).
inline std::vector<Verdict> predict(const Scene& s, const Rule& rule) {
    const Image m = image(s.before), cur = image(s.now);
    CurrentFrame frame;   // the rig's camera is still, its current phase 0
    frame.depth = &cur;
    frame.now = frame.old = stillCamera();
    std::vector<Verdict> v(size_t(N) * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            v[size_t(y) * N + x] = decide(m, (float(x) + 0.5f) / N, (float(y) + 0.5f) / N, s.px, s.py, s.now[size_t(y) * N + x], rule, &frame);
    return v;
}
}  // namespace rig16

}  // namespace histdepth
