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

// The angle of a rotation, in degrees: acos((trace - 1) / 2), clamped.
// The head's turn between two frames, for the pass's speed buckets.
inline float temporalRotationAngleDeg(const float m[9]) {
    float c = 0.5f * (m[0] + m[4] + m[8] - 1.0f);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * 57.2957795f;
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
