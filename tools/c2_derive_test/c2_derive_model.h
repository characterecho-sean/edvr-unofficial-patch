// The C2 derive model: the decompile-cited camera derive shared by
// c2_derive_test (the A-series protocol tests) and c2_warp_test (the B-series
// WARP geometry tests). One transcription, two consumers -- the
// supersample_math.h/temporal_math.h precedent, for the same reason. The
// header comment in c2_derive_test.cpp names the artifacts and conventions.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace c2derive {

struct Cam {
    alignas(16) unsigned char b[0x900];
    float& F(uint32_t helperOff) { return *reinterpret_cast<float*>(b + 0x20 + helperOff); }
    const float& F(uint32_t helperOff) const { return *reinterpret_cast<const float*>(b + 0x20 + helperOff); }
    uint32_t& U(uint32_t helperOff) { return *reinterpret_cast<uint32_t*>(b + 0x20 + helperOff); }
    const uint32_t& U(uint32_t helperOff) const { return *reinterpret_cast<const uint32_t*>(b + 0x20 + helperOff); }
    uint8_t& B(uint32_t helperOff) { return b[0x20 + helperOff]; }
};

constexpr uint32_t kCamAxes = 0x20;        // source 3x4 view axes (+0x20..+0x4C)
constexpr uint32_t kCamOrigin = 0x50;      // source origin (stored negated)
constexpr uint32_t kCamViewRows = 0x190;   // view rows (axes + translation)
constexpr uint32_t kCamProj = 0x1D0;       // projection 4x4
constexpr uint32_t kCamVP = 0x210;         // cached view-projection
constexpr uint32_t kCamFlags = 0x250;      // dirty flag word
constexpr uint32_t kCamNear = 0x254;
constexpr uint32_t kCamFar = 0x258;
constexpr uint32_t kCamCompoundNear = 0x25C;
constexpr uint32_t kCamKind = 0x264;
constexpr uint32_t kCamAngular = 0x280;
constexpr uint32_t kCamBoundX = 0x28C;     // off-center bound pair (proj[8] = 2x)
constexpr uint32_t kCamBoundY = 0x290;     //                        (proj[9] = 2x)
constexpr uint32_t kCamViewportW = 0x2A0;  // refresh-read float pair
constexpr uint32_t kCamViewportH = 0x2A4;
constexpr uint32_t kCamAdjustEnable = 0x2C0;
constexpr uint32_t kCamCustom = 0x2D0;     // kinds 4/5 custom matrix data
constexpr uint32_t kCamRayBasis = 0x870;   // view-matrix snapshot
constexpr uint32_t kCamRayOrigin = 0x8B0;  // origin snapshot

constexpr uint32_t kFlagRay = 1, kFlagView = 2, kFlagProj = 4, kFlagVP = 8;

inline float& camF(Cam& c, uint32_t camOff) { return *reinterpret_cast<float*>(c.b + camOff); }
inline const float& camF(const Cam& c, uint32_t camOff) { return *reinterpret_cast<const float*>(c.b + camOff); }
inline uint32_t& camU(Cam& c, uint32_t camOff) { return *reinterpret_cast<uint32_t*>(c.b + camOff); }
inline const uint32_t& camU(const Cam& c, uint32_t camOff) { return *reinterpret_cast<const uint32_t*>(c.b + camOff); }

inline float negf(float x) { uint32_t u; std::memcpy(&u, &x, 4); u ^= 0x80000000u; std::memcpy(&x, &u, 4); return x; }

// View finalizer: FUN_1404f4910 (camera_cache_helpers.txt:575-617).
inline void finalizeViewRows(Cam& c) {
    const float a0 = c.F(0x00), a1 = c.F(0x04), a2 = c.F(0x08);
    const float a4 = c.F(0x10), a5 = c.F(0x14), a6 = c.F(0x18);
    const float a8 = c.F(0x20), a9 = c.F(0x24), aA = c.F(0x28);
    const float nox = negf(c.F(0x30)), noy = negf(c.F(0x34)), noz = negf(c.F(0x38));
    c.F(0x170) = a0;  c.F(0x174) = a4;  c.F(0x178) = a8;  c.F(0x17C) = 0.0f;
    c.F(0x180) = a1;  c.F(0x184) = a5;  c.F(0x188) = a9;  c.F(0x18C) = 0.0f;
    c.F(0x190) = a2;  c.F(0x194) = a6;  c.F(0x198) = aA;  c.F(0x19C) = 0.0f;
    c.F(0x1A0) = noy * a1 + nox * a0 + noz * a2;
    c.F(0x1A4) = noy * a5 + nox * a4 + noz * a6;
    c.F(0x1A8) = noy * a9 + nox * a8 + noz * aA;
    c.F(0x1AC) = 1.0f;
    c.U(0x230) &= ~2u;
}

inline float* projSlot(Cam& c, int i) { return &c.F(0x1B0 + 4 * i); }
inline const float* projSlot(const Cam& c, int i) { return &c.F(0x1B0 + 4 * i); }

// Projection builder: FUN_1404f2ff0 (camera_cache_helpers.txt:255-493).
inline void buildProjection(Cam& c) {
    const float fVar19 = (c.F(0x23C) + 1.0f) * c.F(0x234); // (compoundNear+1)*near
    const uint32_t kind = c.U(0x244);
    float fVar18 = 1.0f; // DAT_144dde614
    bool bVar5 = false;
    if (kind != 0) {
        if (kind == 1) {
            // Ortho (camera_cache_helpers.txt:296-310). The branch ends with
            // goto LAB_1404f35c7 (:310): it skips the trigonometric block
            // entirely. Falling through would overwrite the ortho matrix with
            // the perspective one (the C2-work review's probe caught exactly
            // that here).
            fVar18 = c.F(0x268);
            *projSlot(c, 0) = c.F(0x264);
            *projSlot(c, 1) = 0.0f; *projSlot(c, 2) = 0.0f; *projSlot(c, 3) = 0.0f;
            *projSlot(c, 4) = 0.0f; *projSlot(c, 6) = 0.0f; *projSlot(c, 7) = 0.0f;
            *projSlot(c, 11) = 0.0f; *projSlot(c, 13) = 0.0f;
            *projSlot(c, 5) = c.F(0x264) * c.F(0x240);
            const float fVar20 = negf(fVar18 / (c.F(0x238) - fVar19));
            *projSlot(c, 10) = fVar20;
            *projSlot(c, 15) = fVar18;
            *projSlot(c, 14) = fVar18 - fVar20 * fVar19;
            goto oblique;
        } else if (kind != 3) {
            if (kind == 4) {
                // Custom matrix copy with per-row w adjustments (:313-340).
                *projSlot(c, 0) = c.F(0x2B0); *projSlot(c, 1) = c.F(0x2B4);
                *projSlot(c, 2) = c.F(0x2BC) - c.F(0x2B8); *projSlot(c, 3) = c.F(0x2BC);
                *projSlot(c, 4) = c.F(0x2C0); *projSlot(c, 5) = c.F(0x2C4);
                *projSlot(c, 6) = c.F(0x2CC) - c.F(0x2C8); *projSlot(c, 7) = c.F(0x2CC);
                *projSlot(c, 8) = c.F(0x2D0); *projSlot(c, 9) = c.F(0x2D4);
                *projSlot(c, 10) = c.F(0x2DC) - c.F(0x2D8); *projSlot(c, 11) = c.F(0x2DC);
                *projSlot(c, 12) = c.F(0x2E0); *projSlot(c, 13) = c.F(0x2E4);
                *projSlot(c, 14) = c.F(0x2EC) - c.F(0x2E8); *projSlot(c, 15) = c.F(0x2EC);
            } else if (kind == 5) {
                // Custom matrix + near/far depth terms (:341-377).
                const float fVar21 = c.F(0x2BC);
                *projSlot(c, 0) = c.F(0x2B0); *projSlot(c, 1) = c.F(0x2B4);
                *projSlot(c, 2) = c.F(0x2B8); *projSlot(c, 3) = fVar21;
                *projSlot(c, 4) = c.F(0x2C0); *projSlot(c, 5) = c.F(0x2C4);
                *projSlot(c, 6) = c.F(0x2C8); *projSlot(c, 7) = c.F(0x2CC);
                *projSlot(c, 8) = c.F(0x2D0); *projSlot(c, 9) = c.F(0x2D4);
                *projSlot(c, 10) = c.F(0x2D8); *projSlot(c, 11) = c.F(0x2DC);
                *projSlot(c, 12) = c.F(0x2E0); *projSlot(c, 13) = c.F(0x2E4);
                *projSlot(c, 14) = c.F(0x2E8); *projSlot(c, 15) = c.F(0x2EC);
                float fVar19b = c.F(0x238), fVar20b = c.F(0x234);
                float fVar22 = (fVar19b * fVar20b) / (fVar20b - fVar19b);
                fVar19b = fVar19b / (fVar19b - fVar20b);
                if (c.B(0x248) != 0) { fVar22 = negf(fVar20b); fVar19b = fVar18; }
                *projSlot(c, 2) = fVar21 - c.F(0x2B8);
                *projSlot(c, 6) = c.F(0x2CC) - c.F(0x2C8);
                *projSlot(c, 10) = c.F(0x2DC) - fVar19b;
                *projSlot(c, 14) = c.F(0x2EC) - fVar22;
            } else {
                // Default identity-ish (:379-399).
                static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                for (int i = 0; i < 16; ++i) *projSlot(c, i) = kIdent[i];
                *projSlot(c, 8) = 0.0f; *projSlot(c, 9) = 0.0f;
                *projSlot(c, 10) = -1.0f;
                *projSlot(c, 14) = 1.0f;
            }
            goto oblique;
        }
        bVar5 = true;
    }
    {
        // Trigonometric perspective (:404-433), FUN_1448b2390 -> std::tan.
        const float fVar21 = c.F(0x260) * 0.5f;
        const float fVar20 = c.F(0x240);
        if (c.B(0x249) == 0) {
            const float fVar22 = c.F(0x238); // far
            const float fVar23 = static_cast<float>(std::tan(static_cast<double>(fVar21)));
            float fVar15 = negf(fVar23);
            float fVar14 = (fVar23 - fVar15) * c.F(0x270);
            float fVar13 = (fVar23 * fVar20 - fVar15 * fVar20) * c.F(0x26C);
            const float fVar16 = fVar15 - fVar14;
            fVar14 = fVar23 - fVar14;
            fVar15 = fVar15 * fVar20 - fVar13;
            fVar13 = fVar23 * fVar20 - fVar13;
            float fv20 = negf(fVar19) * fVar22 / (fVar22 - fVar19);
            float fv22 = fVar22 / (fVar22 - fVar19);
            if (bVar5) { fv20 = negf(fVar19); fv22 = fVar18; }
            fv20 = fv20 * -1.0f;
            fv22 = fv22 * fVar18;
            *projSlot(c, 1) = 0.0f; *projSlot(c, 2) = 0.0f; *projSlot(c, 3) = 0.0f;
            *projSlot(c, 4) = 0.0f; *projSlot(c, 6) = 0.0f; *projSlot(c, 7) = 0.0f;
            *projSlot(c, 14) = fv20;
            *projSlot(c, 0) = 2.0f / (fVar13 - fVar15);
            const float fVar21b = 2.0f / (fVar14 - fVar16);
            *projSlot(c, 8) = (fVar13 + fVar15) / (fVar15 - fVar13);
            *projSlot(c, 9) = (fVar14 + fVar16) / (fVar16 - fVar14);
            *projSlot(c, 15) = 0.0f;
            *projSlot(c, 13) = 0.0f;
            *projSlot(c, 11) = 1.0f; *projSlot(c, 12) = 0.0f;
            *projSlot(c, 10) = fVar18 - fv22;
            *projSlot(c, 5) = fVar21b;
        } else {
            // The +0x249-flag branch (:434-450): the cot-ratio form, with
            // FUN_1448aaf90 -> std::cos and FUN_1448b4d80 -> std::sin.
            const float u3 = c.F(0x250), u4 = c.F(0x254);
            const float fVar22 = c.F(0x258);
            const uint32_t u17 = c.U(0x25C) ^ 0x80000000u;
            const float fVar19c = static_cast<float>(std::cos(static_cast<double>(fVar21)));
            const double d2 = std::sin(static_cast<double>(fVar21));
            *projSlot(c, 1) = 0.0f; *projSlot(c, 7) = 0.0f; *projSlot(c, 9) = 0.0f;
            *projSlot(c, 2) = u3;
            const float fVar21c = static_cast<float>(static_cast<double>(fVar19c) / d2);
            *projSlot(c, 0) = fVar21c / fVar20;
            *projSlot(c, 6) = u4;
            float u17f; std::memcpy(&u17f, &u17, 4);
            *projSlot(c, 14) = u17f;
            *projSlot(c, 15) = 0.0f;
            *projSlot(c, 13) = 0.0f;
            *projSlot(c, 11) = 1.0f; *projSlot(c, 12) = 0.0f;
            *projSlot(c, 10) = fVar18 - fVar22;
            *projSlot(c, 5) = fVar21c;
            *projSlot(c, 4) = 0.0f; *projSlot(c, 3) = 0.0f;
        }
    }
oblique:
    if (c.B(0x2A0) != 0) {
        // Oblique/projection adjust (:458-490), DAT_144e2f890 = 2.0,
        // DAT_144e2f870 = 0.5.
        float fVar18o = 2.0f * (negf(*projSlot(c, 14)) / (*projSlot(c, 10) - 1.0f));
        float fVar19o = fVar18o / *projSlot(c, 5);
        fVar18o = fVar18o / *projSlot(c, 0);
        float fVar20o = *projSlot(c, 8) * negf(fVar18o);
        float fVar22o = *projSlot(c, 9) * negf(fVar19o);
        fVar18o = (fVar20o + fVar18o) * 0.5f;
        fVar19o = (fVar22o + fVar19o) * 0.5f;
        fVar20o = fVar20o - fVar18o;
        fVar22o = fVar22o - fVar19o;
        fVar18o = fVar18o - fVar20o;
        fVar19o = fVar19o - fVar22o;
        const float fVar21o = fVar20o + fVar18o * c.F(0x290);
        fVar20o = fVar20o + fVar18o * c.F(0x280);
        // Decompile :474-475: fVar18 uses +0x284, fVar22 uses +0x294 -- not
        // +0x294/+0x28C (the C2-work review's counterexample caught the swap).
        const float fVar18b = fVar22o + fVar19o * c.F(0x284);
        fVar22o = fVar22o + fVar19o * c.F(0x294);
        *projSlot(c, 0) = *projSlot(c, 0) / (c.F(0x290) - c.F(0x280));
        *projSlot(c, 5) = *projSlot(c, 5) / (c.F(0x294) - c.F(0x284));
        *projSlot(c, 8) = (fVar20o + fVar21o) / (fVar20o - fVar21o);
        *projSlot(c, 9) = (fVar18b + fVar22o) / (fVar18b - fVar22o);
    }
    c.U(0x230) &= ~4u;
}

// VP finalizer: FUN_1404f49f0 (camera_cache_helpers.txt:57-195): vp = view x
// proj, row-major (:149-192).
inline void finalizeVP(Cam& c) {
    if (c.U(0x230) & 4u) buildProjection(c);
    if (c.U(0x230) & 2u) finalizeViewRows(c);
    const float* v = &c.F(0x170);
    const float* p = &c.F(0x1B0);
    float* vp = &c.F(0x1F0);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            vp[r * 4 + k] = v[r * 4 + 0] * p[0 * 4 + k] + v[r * 4 + 1] * p[1 * 4 + k] +
                            v[r * 4 + 2] * p[2 * 4 + k] + v[r * 4 + 3] * p[3 * 4 + k];
    c.U(0x230) &= ~8u;
}

// Scene CB composer: FUN_140596830 (camera_producer.txt:63-121): out =
// [R | 0; 0 0 0 1] x proj with the source axes rows (lane 3 masked).
inline void composeSceneCb(Cam& c, float out[16]) {
    if (c.U(0x230) & 4u) buildProjection(c);
    const float* s = &c.F(0x00);
    const float p0 = *projSlot(c, 0),  p1 = *projSlot(c, 1),  p2 = *projSlot(c, 2),  p3 = *projSlot(c, 3);
    const float p4 = *projSlot(c, 4),  p5 = *projSlot(c, 5),  p6 = *projSlot(c, 6),  p7 = *projSlot(c, 7);
    const float p8 = *projSlot(c, 8),  p9 = *projSlot(c, 9),  pA = *projSlot(c, 10), pB = *projSlot(c, 11);
    const float pC = *projSlot(c, 12), pD = *projSlot(c, 13), pE = *projSlot(c, 14), pF = *projSlot(c, 15);
    out[0]  = p8 * s[8]  + p0 * s[0] + p4 * s[4];
    out[1]  = p9 * s[8]  + p1 * s[0] + p5 * s[4];
    out[2]  = pA * s[8]  + p2 * s[0] + p6 * s[4];
    out[3]  = pB * s[8]  + p3 * s[0] + p7 * s[4];
    out[4]  = p8 * s[9]  + p0 * s[1] + p4 * s[5];
    out[5]  = p9 * s[9]  + p1 * s[1] + p5 * s[5];
    out[6]  = pA * s[9]  + p2 * s[1] + p6 * s[5];
    out[7]  = pB * s[9]  + p3 * s[1] + p7 * s[5];
    out[8]  = p8 * s[10] + p0 * s[2] + p4 * s[6];
    out[9]  = p9 * s[10] + p1 * s[2] + p5 * s[6];
    out[10] = pA * s[10] + p2 * s[2] + p6 * s[6];
    out[11] = pB * s[10] + p3 * s[2] + p7 * s[6];
    out[12] = pC;
    out[13] = pD;
    out[14] = pE;
    out[15] = pF;
}

// Refresh context copy: FUN_1405921f0's VP copy (camera_producer.txt:191-221)
// -- the cached VP transposed.
inline void refreshCopyVp(Cam& c, float ctx[16]) {
    if (c.U(0x230) & 8u) finalizeVP(c);
    const float* vp = &c.F(0x1F0);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) ctx[r * 4 + k] = vp[k * 4 + r];
}

// Ray snapshot: FUN_1406be790 (camera_ray_writers2.txt:13-30).
inline void snapshotRay(Cam& c) {
    if (c.U(0x230) & 2u) finalizeViewRows(c);
    for (int i = 0; i < 18; ++i) camF(c, kCamRayBasis + 4 * i) = c.F(0x170 + 4 * i);
    camF(c, kCamRayOrigin + 0) = c.F(0x30);
    camF(c, kCamRayOrigin + 4) = c.F(0x34);
    camF(c, kCamRayOrigin + 8) = c.F(0x38);
    camF(c, kCamRayOrigin + 12) = 0.0f;
}

// ---------------------------------------------------------------------------
// The 3x4 view inverse, as FUN_1405e9ed0's output is consumed
// (camera_ray_consumer.txt:142-158). SUBSTITUTION, named like the trig one:
// the helper computes a general inverse; the view rows the rig (and the
// game) produce are orthonormal, where the exact inverse is [R^T | -R^T t]
// with the bottom row (0,0,0,1). A non-orthonormal input here would need the
// real helper -- and would be a finding of its own.
// ---------------------------------------------------------------------------
inline void inverseView34(const float v[12], float inv[16]) {
    inv[0] = v[0]; inv[1] = v[4]; inv[2] = v[8];  inv[3] = 0.0f;
    inv[4] = v[1]; inv[5] = v[5]; inv[6] = v[9];  inv[7] = 0.0f;
    inv[8] = v[2]; inv[9] = v[6]; inv[10] = v[10]; inv[11] = 0.0f;
    inv[12] = -(inv[0] * v[12] + inv[4] * v[13] + inv[8] * v[14]);
    inv[13] = -(inv[1] * v[12] + inv[5] * v[13] + inv[9] * v[14]);
    inv[14] = -(inv[2] * v[12] + inv[6] * v[13] + inv[10] * v[14]);
    inv[15] = 1.0f;
}

// ---------------------------------------------------------------------------
// Ray CB composer: FUN_1405964c0 (camera_ray_consumer.txt:96-188), with the
// constant blocks resolved: the 1450c8090..0xbc block is the identity 4x4, so
// the four pre-vectors are the snapshot rows and the origin delta as a point:
//   V0..V2 = snapshot rows (basis), V3 = delta x basis + (0,0,0,1)
// and the CB is the 4x4 product [V0..V3] x inverseView(current view rows),
// read back as 12 floats (3x4) + the w column (:172-175).
// origin = camera+0x50, snapOrigin = camera+0x8B0, viewRows = helper+0x170,
// basis = camera+0x870.
// ---------------------------------------------------------------------------
inline void composeRayCb(const float origin[3], const float snapOrigin[3],
                         const float viewRows[12], const float basis[12], float out[16]) {
    const float dx = origin[0] - snapOrigin[0];
    const float dy = origin[1] - snapOrigin[1];
    const float dz = origin[2] - snapOrigin[2];
    float v[16];
    v[0] = basis[0];  v[1] = basis[1];  v[2] = basis[2];  v[3] = basis[3];
    v[4] = basis[4];  v[5] = basis[5];  v[6] = basis[6];  v[7] = basis[7];
    v[8] = basis[8];  v[9] = basis[9];  v[10] = basis[10]; v[11] = basis[11];
    v[12] = dz * basis[8] + dy * basis[4] + dx * basis[0];
    v[13] = dz * basis[9] + dy * basis[5] + dx * basis[1];
    v[14] = dz * basis[10] + dy * basis[6] + dx * basis[2];
    v[15] = 1.0f + dz * basis[11] + dy * basis[7] + dx * basis[3];
    float inv[16];
    inverseView34(viewRows, inv);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            out[r * 4 + k] = v[r * 4 + 0] * inv[0 * 4 + k] + v[r * 4 + 1] * inv[1 * 4 + k] +
                             v[r * 4 + 2] * inv[2 * 4 + k] + v[r * 4 + 3] * inv[3 * 4 + k];
}

// ---------------------------------------------------------------------------
// Depth-parameter CB row: the refresh's slots +0x60/+0x68 upload
// (camera_producer.txt:277-294). far = camera+0x258, near = camera+0x254,
// viewport pair camera+0x2A0/+0x2A4, counters camera+0x2B0/+0x2B4.
// row0 = {1/(far-near), 1/far - 1/near, 1/near, near}; row1 the viewport
// pair and the two counters. For the infinite-far path (proj[10] == 0,
// proj[14] == near) the shaders' z is 1/(d*row0[2]) == near/d, which the W2
// consumption check exercises; the finite-far constants are transcribed but
// not consumed here.
// ---------------------------------------------------------------------------
inline void composeDepthCb(const Cam& c, float row0[4], float row1[4]) {
    const float farZ = camF(c, kCamFar), nearZ = camF(c, kCamNear);
    const float fVar28 = 1.0f / (farZ * nearZ);
    row0[0] = 1.0f / (farZ - nearZ);
    row0[1] = (nearZ - farZ) * fVar28;
    row0[2] = farZ * fVar28;
    row0[3] = nearZ;
    row1[0] = camF(c, kCamViewportW);
    row1[1] = camF(c, kCamViewportH);
    row1[2] = camF(c, 0x2B0);
    row1[3] = camF(c, 0x2B4);
}

// A plausible perspective camera (z-forward DirectX view space, reversed-Z
// infinite-far depth, per temporal_math.h's notes).
inline void makeCamera(Cam& c) {
    std::memset(c.b, 0, sizeof(c.b));
    const float cy = std::cos(0.35f), sy = std::sin(0.35f);
    const float cp = std::cos(-0.12f), sp = std::sin(-0.12f);
    float* a = &camF(c, kCamAxes);
    a[0] = cy;      a[1] = 0.0f;  a[2] = -sy;     a[3] = 0.0f;
    a[4] = sy * sp; a[5] = cp;    a[6] = cy * sp; a[7] = 0.0f;
    a[8] = sy * cp; a[9] = -sp;   a[10] = cy * cp;
    camF(c, kCamOrigin + 0) = 120.0f;
    camF(c, kCamOrigin + 4) = -45.0f;
    camF(c, kCamOrigin + 8) = 900.0f;
    camF(c, kCamNear) = 0.5f;
    camF(c, kCamFar) = 100000.0f;
    camF(c, kCamCompoundNear) = 0.0f;
    camU(c, kCamKind) = 3;
    camF(c, kCamAngular) = 1.1f;
    camF(c, 0x260) = 1.6f;
    camF(c, kCamBoundX) = 0.0f;
    camF(c, kCamBoundY) = 0.0f;
    camF(c, kCamViewportW) = 3840.0f;
    camF(c, kCamViewportH) = 2160.0f;
    camU(c, kCamFlags) = kFlagView | kFlagProj | kFlagVP;
}

inline void derive(Cam& c) {
    finalizeVP(c);
}

// Project a view-space point with the projection matrix (z-forward, w = z).
inline void projectPoint(const Cam& c, const float v[3], float outXYW[3]) {
    const float* p = &c.F(0x1B0);
    const float x = v[0], y = v[1], z = v[2];
    outXYW[0] = p[0] * x + p[4] * y + p[8] * z + p[12];
    outXYW[1] = p[1] * x + p[5] * y + p[9] * z + p[13];
    outXYW[2] = p[3] * x + p[7] * y + p[11] * z + p[15];
}

} // namespace c2derive
