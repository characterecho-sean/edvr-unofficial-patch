// The fake IVRSystem_012's shape and encoding, shared by the stand-in that
// implements it (fakevr.cpp) and the test that calls it (openvr_smoke.cpp).
//
// One header on purpose: the whole point of the cell is that a member-
// convention CALLER reaches a member-convention IMPLEMENTATION through the
// proxy's observation hook with every argument and return byte intact. If
// the two sides declared their own layouts they could drift, and a drifted
// test proves nothing.
//
// The method bodies encode their arguments into their results, so the caller
// can assert that self, every argument -- including IVRSystem_012's fourth
// GetProjectionMatrix parameter, the one later generations dropped -- and
// the by-value struct returns all crossed the hook unchanged. The two
// struct-returning methods are the ones the hook must observe WITHOUT a C
// receiver (EVIDENCE 6bo: member and free conventions disagree about the
// hidden return pointer for those), so a corrupted value here is the test
// catching exactly the failure the asm thunks exist to prevent.
#pragma once

#include <cstdint>

namespace fakevr {

struct M44 { float m[4][4]; };
struct M34 { float m[3][4]; };

// Slot order is IVRSystem_012's first five, which is all the hook touches.
// Three filler methods follow so the executable-prefix probe sees a table
// comfortably longer than the highest hooked slot.
struct ISystem012 {
    virtual void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) = 0; // 0
    virtual M44  GetProjectionMatrix(int32_t eye, float nearZ, float farZ,
                                     int32_t projType) = 0;                    // 1
    virtual void GetProjectionRaw(int32_t eye, float* l, float* r, float* t,
                                  float* b) = 0;                               // 2
    virtual int32_t Filler3() = 0;                                             // 3
    virtual M34  GetEyeToHeadTransform(int32_t eye) = 0;                       // 4
    virtual int32_t Filler5() = 0;                                             // 5
    virtual int32_t Filler6() = 0;                                             // 6
    virtual int32_t Filler7() = 0;                                             // 7
};

constexpr uint32_t kSizeW = 1456;
constexpr uint32_t kSizeH = 1584;

// GetProjectionRaw writes tangents derived from the eye so the two eyes are
// distinguishable: left = {-1.25, 0.75, -1.0, 1.0}, right mirrored in u.
inline void expectedRaw(int32_t eye, float out[4]) {
    out[0] = eye == 0 ? -1.25f : -0.75f;
    out[1] = eye == 0 ? 0.75f : 1.25f;
    out[2] = -1.0f;
    out[3] = 1.0f;
}

// GetProjectionMatrix encodes every argument it received:
//   m[0][0] = 0.78 + eye     m[1][3] = nearZ
//   m[2][0] = farZ           m[2][1] = projType
//   m[3][2] = -1, m[3][3] = 0 (the projection shape the probe validates)
inline M44 expectedMatrix(int32_t eye, float nearZ, float farZ, int32_t projType) {
    M44 r{};
    r.m[0][0] = 0.78f + static_cast<float>(eye);
    r.m[1][1] = 0.79f;
    r.m[1][3] = nearZ;
    r.m[2][0] = farZ;
    r.m[2][1] = static_cast<float>(projType);
    r.m[3][2] = -1.0f;
    return r;
}

// GetEyeToHeadTransform: identity rotation, half-IPD translation by eye, and
// the eye echoed in m[2][3] so a swapped-eye bug cannot cancel out.
inline M34 expectedEyeToHead(int32_t eye) {
    M34 r{};
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    r.m[0][3] = eye == 0 ? -0.032f : 0.032f;
    r.m[2][3] = static_cast<float>(eye);
    return r;
}

}  // namespace fakevr
