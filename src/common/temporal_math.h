// The temporal anti-aliasing pass's arithmetic, header-only and pure, shared
// by the openvr half (which jitters the projection and decides), the d3d11
// half (which filters) and the test that pins both (tools/temporal_test) --
// supersample_math.h's precedent, for the same reason: one definition, every
// consumer, nothing kept in step by hand.
//
// docs/anti-aliasing.md, Feature B. Each frame the eye image is blended with
// a history of the frames before it, each reprojected to where its content
// sits now, so content that flickers on and off the sample grid is averaged
// across frames into a stable value. Four pieces of arithmetic live here:
//
//   * the JITTER: which sub-pixel offset frame n renders through, and how
//     that offset is told to the game as a shift of its projection's
//     tangents (the cull guard's own edit, with a different number);
//   * the MAPPING between a pixel and the view-space direction it looks
//     along, through the frustum the game rendered with;
//   * the ROTATION DELTA between two frames' cameras, from the runtime's
//     head pose or from the game's own view matrix;
//   * the reprojection those three compose into, which the test walks by
//     hand and the shader in src/d3d11/temporal_pass.cpp transcribes.
//
// Conventions, stated once: view space is OpenVR's -- +X right, +Y up, -Z
// forward -- and the projection is the runtime's raw tangents l, r, t, b,
// where texture row 0 looks along the b tangent and the last row along t
// (the guard's cropFractions derived this from the matrix formula and the
// field confirmed it, 2026-08-18). A jitter of (jx, jy) pixels means the
// rendered content sits jx pixels to the right and jy pixels down from
// where the unjittered projection would put it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {

// A populated scene, by the draws into the scene pair's lesser target a
// frame: the main menu's pre-rendered backdrop takes one or two, a scene
// tens to hundreds. Fifty until 2026-09-16, when the scanner's initial
// screen in a sparse system took 49 on four frames in five and the world
// path stood down under the pan (docs/fss-scanner.md).
constexpr uint32_t kTemporalSceneDrawFloor = 8u;

// A scene camera includes both ship and head rotation. Opposing turns can
// cancel, so a small camera delta does not prove a menu/stale camera.
// Trust continuous rows from the bound scene block in a populated scene;
// retain the head-follow detector only for ambiguous auxiliary chains.
inline int temporalCameraFollowScore(int score, uint32_t sceneDraws, bool boundRows,
                                     float headDeg, float rowsDeg) {
    if (sceneDraws >= kTemporalSceneDrawFloor && boundRows) return 30;
    if (headDeg > 0.1f) score += rowsDeg < 0.3f * headDeg ? -4 : 1;
    return score < -30 ? -30 : score > 30 ? 30 : score;
}

// Elite's scene depth is reversed Z with an infinite far plane. OpenVR's
// finite clip planes describe the submitted projection, not this depth
// buffer (14:53 capture: finite fallback displaced station pixels by km).
// A usable scene row takes precedence; missing or unrelated rows must not
// silently reintroduce the runtime's far-plane offset.
inline bool temporalSceneProjection(float rowA, float rowB, float nearZ,
                                     float* a, float* b) {
    const bool haveNear = std::isfinite(nearZ) && nearZ > 0.0f;
    const float rowNear = rowB / (1.0f - rowA);
    const bool measured = std::isfinite(rowA) && std::isfinite(rowB) &&
        rowA <= 0.0f && rowB > 0.0f && std::isfinite(rowNear) &&
        (!haveNear || std::fabs(rowNear - nearZ) <= nearZ * 0.05f);
    *a = measured ? rowA : 0.0f;
    *b = measured ? rowB : haveNear ? nearZ : 0.0f;
    return measured;
}

// How many frames the jitter sequence runs before repeating. Halton (2,3)
// over eight frames covers the pixel evenly; longer sequences converge
// finer detail but take longer to settle after a reset.
constexpr uint32_t kTemporalJitterCount = 8;

// The radical inverse of i (i >= 1) in `base`: the Halton sequence's
// members, in [0, 1).
inline float temporalHalton(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

// Frame n's sub-pixel offset, in render pixels, in [-0.5, 0.5).
inline void temporalJitter(uint32_t n, float* jx, float* jy) {
    const uint32_t i = (n % kTemporalJitterCount) + 1;
    *jx = temporalHalton(i, 2) - 0.5f;
    *jy = temporalHalton(i, 3) - 0.5f;
}

// A jitter of (jx, jy) pixels as the shift of every tangent: l and r move
// by dx, t and b by dy. Shifting l and r by dx moves every projected
// point LEFT by dx * w / (r - l) pixels, so content displaced right by jx
// needs dx = -jx * (r - l) / w; rows count downward from the b edge, so
// content displaced down by jy needs dy = +jy * (b - t) / h.
inline void temporalJitterToTangents(float jx, float jy, const float tan[4],
                                     uint32_t w, uint32_t h, float* dx,
                                     float* dy) {
    *dx = w ? -jx * (tan[1] - tan[0]) / static_cast<float>(w) : 0.0f;
    *dy = h ? jy * (tan[3] - tan[2]) / static_cast<float>(h) : 0.0f;
}

// The view-space direction a pixel's CENTRE looks along, through the
// frustum tan = {l, r, t, b} rendered at w x h.
inline void temporalPixelToDir(float px, float py, const float tan[4],
                               uint32_t w, uint32_t h, float d[3]) {
    d[0] = tan[0] + (px + 0.5f) / static_cast<float>(w) * (tan[1] - tan[0]);
    d[1] = tan[3] - (py + 0.5f) / static_cast<float>(h) * (tan[3] - tan[2]);
    d[2] = -1.0f;
}

// ...and back: the pixel (centre-based, so the pixel whose centre a
// direction hits exactly reads as a whole number) a direction lands on.
// False for a direction at or behind the eye.
inline bool temporalDirToPixel(const float d[3], const float tan[4],
                               uint32_t w, uint32_t h, float* px, float* py) {
    if (!(d[2] < -1e-6f)) return false;
    const float xt = d[0] / -d[2];
    const float yt = d[1] / -d[2];
    *px = (xt - tan[0]) / (tan[1] - tan[0]) * static_cast<float>(w) - 0.5f;
    *py = (tan[3] - yt) / (tan[3] - tan[2]) * static_cast<float>(h) - 0.5f;
    return true;
}

// 3x3 helpers, row-major: m[row * 3 + col].
inline void temporalMul3(const float a[9], const float b[9], float out[9]) {
    float t[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            t[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] +
                           a[r * 3 + 2] * b[2 * 3 + c];
        }
    }
    memcpy(out, t, sizeof(t));
}
inline void temporalTranspose3(const float a[9], float out[9]) {
    float t[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) t[r * 3 + c] = a[c * 3 + r];
    }
    memcpy(out, t, sizeof(t));
}
inline void temporalApply3(const float m[9], const float v[3], float out[3]) {
    float t[3];
    for (int r = 0; r < 3; ++r) {
        t[r] = m[r * 3 + 0] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
    }
    memcpy(out, t, sizeof(t));
}
// The 3x3 of a row-major 3x4 (the runtime's HmdMatrix34_t, or the game's
// view rows): m34[row * 4 + col].
inline void temporalRot3Of34(const float m34[12], float out[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = m34[r * 4 + c];
    }
}

// The head's rotation delta from two of the runtime's poses (device ->
// world, row-major 3x4): a fixed world direction seen in head space now
// was seen at R_prev^T R_now of itself the frame before. Head space and
// eye space share their axes on headsets without canted displays; on a
// canted one the delta is off by the cant's conjugation, which the
// neighbourhood clamp absorbs.
inline void temporalHeadDelta(const float prev34[12], const float now34[12],
                              float delta[9]) {
    float rp[9], rn[9], rpT[9];
    temporalRot3Of34(prev34, rp);
    temporalRot3Of34(now34, rn);
    temporalTranspose3(rp, rpT);
    temporalMul3(rpT, rn, delta);
}

// The translation term of a depth reprojection, per eye. A point at
// view-space position P in THIS frame's eye space was, last frame, at
// R_prev^T (R_now (P + e) + t_now - t_prev) - e in that frame's eye space,
// where e is the eye's offset in head space (GetEyeToHeadTransform's
// translation) and [R | t] the runtime's head poses (device -> world).
// That is delta * P + tv with delta = R_prev^T R_now (temporalHeadDelta)
// and tv = delta * e + R_prev^T (t_now - t_prev) - e, which this returns.
// With P known from depth the reprojection is exact for a rigid world;
// without depth (P at infinity) tv vanishes and the rotation-only path
// is what remains.
inline void temporalHeadTranslation(const float prev34[12], const float now34[12],
                                    const float eye[3], float tv[3]) {
    float rp[9], rn[9], rpT[9], delta[9];
    temporalRot3Of34(prev34, rp);
    temporalRot3Of34(now34, rn);
    temporalTranspose3(rp, rpT);
    temporalMul3(rpT, rn, delta);
    const float dt[3] = {now34[3] - prev34[3], now34[7] - prev34[7],
                         now34[11] - prev34[11]};
    float de[3], rdt[3];
    temporalApply3(delta, eye, de);
    temporalApply3(rpT, dt, rdt);
    for (int i = 0; i < 3; ++i) tv[i] = de[i] + rdt[i] - eye[i];
}

// Reversed-Z depth to metres along the view axis, for a standard D3D
// projection with the near plane at 1 and the far at 0:
// z = near * far / (d * (far - near) + near). The far plane (d = 0) is
// infinity; the caller treats it as such.
inline float temporalDepthToMetres(float d, float nearZ, float farZ) {
    const float den = d * (farZ - nearZ) + nearZ;
    if (!(den > 0.0f)) return 0.0f;
    return nearZ * farZ / den;
}

// Tier 1 of docs/per-object-motion.md, the mover test for one pixel
// (2026-09-08). The camera-only reprojection predicts that this pixel's
// surface sat at view depth zPred last frame (metres; 0 or less = the far
// plane, or no depth); last frame's depth around the predicted position
// spans [zMin, zMax] metres over its 3x3, with anyFar true when any of
// those texels was the far plane. True when the surface is NOT where the
// camera alone would have put it -- a mover or a disocclusion -- by more
// than tol, a fraction of depth. The range rather than one texel because
// the jitter shifts the grid half a pixel between frames and a single
// compare fires on every silhouette every frame. `thick`: at least six of
// the nine texels around the pixel have a depth now -- a hull, not a text
// stroke or a wire. A thin feature reprojects onto depthless texels on
// every frame the head moves, and "a surface where only sky was" called
// each one a mover (2026-09-08: the interface's text swam with the mask
// on); a thin feature gets the range test alone. The shader's moverAt
// transcribes this; tools/temporal_test pins it.
inline bool temporalMoverTest(float zPred, float zMin, float zMax, bool anyFar,
                              float tol, bool thick = true) {
    if (!(zPred > 0.0f)) return !anyFar;   // sky now: consistent only with sky then
    if (!(zMax > 0.0f)) return thick;      // a surface now where only sky was: a hull's edge, not a stroke's
    return zPred < zMin * (1.0f - tol) || zPred > zMax * (1.0f + tol);
}

// The angle of a rotation, in degrees, from the trace and the skew both:
// cos = (trace - 1) / 2, sin = |skew| / 2, angle = atan2(sin, cos). An acos
// of the trace alone cannot resolve below about 0.02 degrees in float (the
// cosine of a small angle rounds to 1), and the registration line's
// residuals sat on that floor for a day (2026-09-04); the skew is exact
// there. The head's turn between two frames, the chooser's continuity
// test, the rows' residual against the head.
inline float temporalRotationAngleDeg(const float m[9]) {
    const float c = 0.5f * (m[0] + m[4] + m[8] - 1.0f);
    const float sx = m[7] - m[5], sy = m[2] - m[6], sz = m[3] - m[1];
    const float s = 0.5f * sqrtf(sx * sx + sy * sy + sz * sz);
    return atan2f(s, c) * 57.2957795f;
}

// The camera's rotation delta from two frames of the game's own view
// rows (3x4, row-major). Read as world -> view: a fixed world direction
// seen in view space now, d_now = M_now w, was seen at M_prev M_now^T
// d_now the frame before. If the rows are the transpose of that (view ->
// world), the delta is M_prev^T M_now instead; `transposed` picks, and
// the field's acceptance-rate line is what settles which the game keeps.
inline void temporalViewDelta(const float prev12[12], const float now12[12],
                              bool transposed, float delta[9]) {
    float mp[9], mn[9], tmp[9];
    temporalRot3Of34(prev12, mp);
    temporalRot3Of34(now12, mn);
    if (!transposed) {
        temporalTranspose3(mn, tmp);
        temporalMul3(mp, tmp, delta);
    } else {
        temporalTranspose3(mp, tmp);
        temporalMul3(tmp, mn, delta);
    }
}

// Are these three rows a rotation? Near-unit, near-orthogonal -- the
// sun-glare fix's own validation of the game's view rows, shared.
inline bool temporalRowsAreRotation(const float m34[12]) {
    float r[9];
    temporalRot3Of34(m34, r);
    for (int i = 0; i < 3; ++i) {
        const float len = sqrtf(r[i * 3] * r[i * 3] + r[i * 3 + 1] * r[i * 3 + 1] +
                                r[i * 3 + 2] * r[i * 3 + 2]);
        if (!(len > 0.95f && len < 1.05f)) return false;
    }
    const float d01 = r[0] * r[3] + r[1] * r[4] + r[2] * r[5];
    const float d02 = r[0] * r[6] + r[1] * r[7] + r[2] * r[8];
    const float d12 = r[3] * r[6] + r[4] * r[7] + r[5] * r[8];
    return fabsf(d01) < 0.05f && fabsf(d02) < 0.05f && fabsf(d12) < 0.05f;
}

// The world path's delta from two frames of the game's view rows, which
// are the FULL view -- the headset's pose is in them -- stored view->world.
// For rows [R | c] (c the eye's place in the world) a point P now was, last
// frame, at W P + tv, W = R_p^T R_n, tv = R_p^T (c_n - c_p); camMove is
// c_n - c_p, the eye's move in the world. The game's view space runs z
// forward (DirectX), the runtime's eye space z back. A rotation read in
// the one and applied in the other has its pitch and yaw reversed and its
// roll kept, which is exactly what the regression measured: over a dozen
// intervals in space the rows turned -1 times the head about x and y and
// +1 about z (k = -2, -2, 0; 2026-09-04), and the far plane, on this delta
// alone, moved the wrong way by the head's whole turn -- the sky's smear,
// and the station's under a head turn, both gone with the world path off.
// Conjugating by the z flip carries the delta into the eye's frame; the
// translation term takes the same flip (a reflection, not a half turn: the
// still-ship regression on the third line says which).
inline void temporalWorldFromRows(const float prev[12], const float now[12],
                                  float W[9], float tv[3], float camMove[3]) {
    float rp[9], rn[9], rpT[9];
    temporalRot3Of34(prev, rp);
    temporalRot3Of34(now, rn);
    temporalTranspose3(rp, rpT);
    temporalMul3(rpT, rn, W);
    const float dc[3] = {now[3] - prev[3], now[7] - prev[7], now[11] - prev[11]};
    temporalApply3(rpT, dc, tv);
    for (int i = 0; i < 3; ++i) camMove[i] = dc[i];
    W[2] = -W[2];
    W[5] = -W[5];
    W[6] = -W[6];
    W[7] = -W[7];
    tv[2] = -tv[2];
}

// The world path's gate on that delta, per frame (the plausibility test of
// src/d3d11/temporal_pass.cpp, here so tools/temporal_test can walk it).
// The rows' delta is the head's plus the ship's turn, and no ship turns 270
// degrees a second: a delta beyond 3 degrees from the head's is another
// camera's rows or a stale latch, and the last accepted delta is carried in
// its place (a far better guess than the head alone, which smeared the
// world on every dropped frame). A jump over 50 m is the floating origin
// moving: only the translation is dropped, and dropped BEFORE the last-good
// store, so a jump never becomes the translation a later drop carries --
// stored first, a jump of hundreds of metres to tens of kilometres was
// carried into the next dropped frame and moved every pixel with a depth on
// the world path by it for one frame (the review of 2026-09-04, F3). A
// carried figure over 50 m is reported so the invariant has a witness on
// the line.
//
// A drop leaves the frame's rows of unknown origin -- another camera's, or
// the view's own after a real jump -- and the next frame's delta is
// measured FROM them. Eye run 050423 (2026-09-25, build c9cab91e, the ship
// rolling about a degree and a half a frame): the chooser resynchronised
// onto an auxiliary pass parked 148 degrees from the view (dropped, carried),
// took that pass's identical write again the next frame as the continuous
// one, and the delta -- zero, which under a still head sits within 3
// degrees of the head's -- was ACCEPTED as the view's and stored as the last
// good; the frame after, back on the view and 148 degrees from the parked
// rows, carried the zero. Two frames of the world standing still under a
// roll, wherever the transition-flash tracker saw a parked camera
// (docs/camera-rows-carry-2026-09-25.md). So the standing of the rows a
// frame measures from is kept: rows a drop left behind are not the view's
// own, and a delta from them that does not turn AT ALL -- the same
// orientation to the last bit, which the view does not hold from one frame
// to the next while the headset is tracked -- is a parked or world-fixed
// camera's: refused, the last good carried, the rows still suspect. A
// delta that turns and passes the 3 degrees restores them (the view resuming
// after a hitch or a resync), so a recovery costs no frame. Measuring from
// the last ACCEPTED rows instead was weighed and declined: after a real
// jump every later frame measures across it from a stale anchor, and none
// is accepted again.
struct TemporalCameraGate {
    float lastGoodC[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the last accepted delta
    float lastGoodTv[3] = {};                            // ...and its translation term
    bool  lastGoodValid = false;
    bool  refOwn = false;     // the rows this frame measures from (last frame's) are the view's own
    bool  measured = false;   // this frame's delta was measured, by either eye...
    bool  refused = false;    // ...and refused by one...
    bool  parked = false;     // ...as a parked camera's
    // Consecutive frames refused as a parked camera's, to the last boundary:
    // a stay carries one delta for its whole length, and nothing else ends
    // a stay on a pass that writes the bound block (the head-follow score
    // trusts that block), so its length is reported.
    uint32_t parkedRun = 0;
};

enum class TemporalCameraVerdict : uint8_t {
    Own,       // the view's own delta: used, and kept as the last good
    Another,   // over 3 degrees from the head's: another camera's rows, or a stale latch
    Parked,    // measured from rows a drop left behind, and not turned at all
};

struct TemporalCameraStep {
    TemporalCameraVerdict verdict = TemporalCameraVerdict::Own;
    bool jump = false;          // the floating origin moved: the translation was dropped
    bool carried = false;       // the last accepted delta stands in for this one
    bool carriedJump = false;   // ...with a translation over 50 m: zero by construction
    bool valid = true;          // a delta is in hand, the view's own or carried
};

// Rows with the same orientation to the last bit: a camera that did not turn.
inline bool temporalRowsSameTurn(const float a[12], const float b[12]) {
    for (int r = 0; r < 3; ++r) {
        if (memcmp(a + r * 4, b + r * 4, sizeof(float) * 3) != 0) return false;
    }
    return true;
}

// One eye's verdict on this frame's delta: W and tv from
// temporalWorldFromRows(prev, now), move the length of its camMove, diffDeg
// the delta's angle from the eye's head delta (0 without one). W and tv come
// back as the delta to use: the rows' own, or the last good carried. Both
// eyes judge a frame against the same standing; it moves on once a frame, in
// temporalCameraGateAdvance.
inline TemporalCameraStep temporalCameraGateStep(TemporalCameraGate& g, const float prev[12],
                                                 const float now[12], double move,
                                                 float diffDeg, float W[9], float tv[3]) {
    TemporalCameraStep s;
    s.jump = move >= 50.0;
    if (s.jump) {
        for (int i = 0; i < 3; ++i) tv[i] = 0.0f;
    }
    if (diffDeg > 3.0f) {
        s.verdict = TemporalCameraVerdict::Another;
    } else if (!g.refOwn && temporalRowsSameTurn(prev, now)) {
        s.verdict = TemporalCameraVerdict::Parked;
        g.parked = true;
    }
    g.measured = true;
    if (s.verdict != TemporalCameraVerdict::Own) {
        g.refused = true;
        if (g.lastGoodValid) {
            memcpy(W, g.lastGoodC, sizeof(g.lastGoodC));
            memcpy(tv, g.lastGoodTv, sizeof(g.lastGoodTv));
            s.carried = true;
            const double carried = std::sqrt(static_cast<double>(tv[0]) * tv[0] +
                                             static_cast<double>(tv[1]) * tv[1] +
                                             static_cast<double>(tv[2]) * tv[2]);
            s.carriedJump = carried >= 50.0;
        } else {
            s.valid = false;
        }
    } else {
        memcpy(g.lastGoodC, W, sizeof(g.lastGoodC));
        if (!s.jump) memcpy(g.lastGoodTv, tv, sizeof(g.lastGoodTv));
        g.lastGoodValid = true;
    }
    return s;
}

// The frame boundary: the rows just chosen become the next frame's
// reference (rowsKept), the view's own when a delta to them was measured
// and no eye refused it. Rows no delta reached -- the first after a frame
// with no write -- are not known to be the view's.
inline void temporalCameraGateAdvance(TemporalCameraGate& g, bool rowsKept) {
    g.refOwn = rowsKept && g.measured && !g.refused;
    g.parkedRun = g.parked ? g.parkedRun + 1 : 0;
    g.measured = false;
    g.refused = false;
    g.parked = false;
}

// The whole reprojection for one pixel, as the shader does it: the pixel's
// direction through this frame's frustum, rotated into last frame's view,
// projected through last frame's frustum. False when it lands behind the
// eye or off the image. The test walks this by hand against known
// rotations; the shader transcribes it.
inline bool temporalReproject(float px, float py, const float tanNow[4],
                              const float tanPrev[4], const float delta[9],
                              uint32_t w, uint32_t h, float* ppx, float* ppy) {
    float d[3], dp[3];
    temporalPixelToDir(px, py, tanNow, w, h, d);
    temporalApply3(delta, d, dp);
    if (!temporalDirToPixel(dp, tanPrev, w, h, ppx, ppy)) return false;
    return *ppx >= 0.0f && *ppy >= 0.0f && *ppx <= static_cast<float>(w) - 1.0f &&
           *ppy <= static_cast<float>(h) - 1.0f;
}

}  // namespace edvr
