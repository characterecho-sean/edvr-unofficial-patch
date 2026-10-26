// The local avatar's skeleton for native on foot's census (native_on_foot_hooks.cpp): the skeleton interface's vtables and slots on build 334925,
// the pose walk (the animated pose's local transforms and parents), the joint names it looks up, and the plausibility test of a joint matrix.
// Arithmetic, tables and text only; the glue does the guarded reads and the game calls.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "avatar_hooks_core.h"

namespace edvr {
namespace avh {

// ---- A. the skeleton interface, the pose walk and the slot checks (build 334925) -----------------------------------------------------------------------
// The verified addresses of the build the census reads (found by the flights' skeleton instruments, since retired; the arc doc has the evidence).
constexpr uintptr_t kRrVtableRva = 0x55A0180, kAoVtableRva = 0x5180D00;
constexpr uint32_t kHeadSlots = 4;                                                    // the slots used: +0x18, +0x30, +0x48, +0x58
constexpr uint32_t kHeadSlotIndex[kHeadSlots] = {3, 6, 9, 11};
constexpr uintptr_t kRrFunctions[kHeadSlots] = {0x43FE5E0, 0xFDEBB0, 0x43F9E60, 0x43F9B90};
constexpr uintptr_t kAoFunctions[kHeadSlots] = {0xFDF3B0, 0xFDEBB0, 0xFDEF50, 0xFDEDB0};
constexpr uint32_t kRrCachedFlag = 0x2C0, kAoCachedFlag = 0xF8;                      // iface+: the joint matrices are cached when it is non-zero
constexpr uintptr_t kHeadNameRva = 0x5552000, kFootLNameRva = 0x5567308, kFootRNameRva = 0x5567320;
inline constexpr char kHeadName[] = "def_c_head_joint";
inline constexpr char kPovName[] = "def_c_povCamera_joint";
inline constexpr char kFootLName[] = "def_l_foot_joint";
inline constexpr char kFootRName[] = "def_r_foot_joint";
constexpr uint32_t kNoJoint = 0xFFFF;

enum class HeadKind : uint8_t { None = 0, Runtime = 1, Animated = 2 };

// ---- the joints by WALKING THE ANIMATED POSE ---------------------------------------------------------------------------------------------------
// F5: the cached +0x58 head did not drop when the commander crouched, and +0x48 drifted 12 m (a frame that is not the free camera's). The cached matrices
// are only maintained for joints the game asked for (SetAttachJoint marks a joint and its ancestors), so the walk composes the joints from the animated pose
// itself, exactly as the game's uncached path does (FUN 0xFDF170, read and its quaternion constants evaluated against the exe's own data):
//   P = GetPoseData(): the u16 at P+0 is the joint count; *(P+0x48) -> the local transforms, 32 bytes a joint: position vec4 at +0 (x,y,z,w), rotation
//   quaternion (x,y,z,w) at +0x10; *(P+0x50) -> the u16 parent of each joint (0xFFFF = root); *(P+0x18) -> the u16 name hashes.
//   Row-vector convention: the joint's matrix is R(q) with its position as the fourth row, and the model-space matrix is joint x parent x grandparent ...
//   R(q) is DirectXMath's XMMatrixRotationQuaternion for row vectors. So a point p in joint j is carried up with  p = p x R(q_a) + pos_a  for each ancestor a.
// This reads game memory and calls nothing: the arrays are copied (under SEH) and walked here.
constexpr uint32_t kPoseJointCountOff = 0, kPoseNamesOff = 0x18, kPoseLocalsOff = 0x48, kPoseParentsOff = 0x50;
constexpr uint32_t kJointBytes = 32, kJointFloats = 8, kMaxWalkJoints = 512;
// The rotation rows of a unit quaternion q = (x, y, z, w), laid out as the exe's code lays them out (w*w2 - 1 + x*x2 on the diagonal).
inline void quatRows(const float q[4], float r[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float d = 2.0f * w * w - 1.0f;
    r[0] = d + 2.0f * x * x;
    r[1] = 2.0f * w * z + 2.0f * x * y;
    r[2] = -2.0f * w * y + 2.0f * x * z;
    r[3] = -2.0f * w * z + 2.0f * y * x;
    r[4] = d + 2.0f * y * y;
    r[5] = 2.0f * w * x + 2.0f * y * z;
    r[6] = 2.0f * w * y + 2.0f * z * x;
    r[7] = -2.0f * w * x + 2.0f * z * y;
    r[8] = d + 2.0f * z * z;
}
// The model-space position of joint `idx`: its local position carried up through its ancestors. False for an index out of range, a parent out of range, or
// a chain longer than the joint count (a cycle). `depth` = the number of ancestors walked.
inline bool walkJointToModel(const float* locals, const uint16_t* parents, uint32_t joints, uint32_t idx, float out[3], uint32_t* depth) {
    if (idx >= joints) return false;
    float p[3] = {locals[idx * kJointFloats + 0], locals[idx * kJointFloats + 1], locals[idx * kJointFloats + 2]};
    uint32_t steps = 0;
    for (uint32_t a = parents[idx]; a != 0xFFFFu; a = parents[a]) {
        if (a >= joints || ++steps > joints) return false;
        float r[9];
        quatRows(locals + a * kJointFloats + 4, r);
        const float x = p[0], y = p[1], z = p[2];
        p[0] = x * r[0] + y * r[3] + z * r[6] + locals[a * kJointFloats + 0];
        p[1] = x * r[1] + y * r[4] + z * r[7] + locals[a * kJointFloats + 1];
        p[2] = x * r[2] + y * r[5] + z * r[8] + locals[a * kJointFloats + 2];
    }
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    *depth = steps;
    return true;
}

struct HeadTargets {
    uintptr_t base = 0;
    size_t imageSize = 0;
    uintptr_t rrVtable = 0, aoVtable = 0, headName = 0, povName = 0, footLName = 0, footRName = 0;
    uintptr_t rrFn[kHeadSlots] = {}, aoFn[kHeadSlots] = {};
    bool inImage(uintptr_t p) const { return base != 0 && p >= base && p < base + imageSize; }
};
inline HeadTargets headTargetsFromBase(uintptr_t base, size_t imageSize) {
    HeadTargets t;
    t.base = base;
    t.imageSize = imageSize;
    t.rrVtable = base + kRrVtableRva;
    t.aoVtable = base + kAoVtableRva;
    t.headName = base + kHeadNameRva;
    t.povName = base + kPovNameRva;
    t.footLName = base + kFootLNameRva;
    t.footRName = base + kFootRNameRva;
    for (uint32_t i = 0; i < kHeadSlots; ++i) {
        t.rrFn[i] = base + kRrFunctions[i];
        t.aoFn[i] = base + kAoFunctions[i];
    }
    return t;
}
inline HeadKind headKindOfVtable(uint64_t vptr, const HeadTargets& t) {
    if (vptr == 0) return HeadKind::None;
    if (vptr == t.rrVtable) return HeadKind::Runtime;
    if (vptr == t.aoVtable) return HeadKind::Animated;
    return HeadKind::None;
}
// The function slot i (0 pose, 1 find, 2 world, 3 model) must hold for this kind.
inline uintptr_t headExpectedFunction(HeadKind k, uint32_t i, const HeadTargets& t) {
    if (i >= kHeadSlots) return 0;
    return k == HeadKind::Runtime ? t.rrFn[i] : k == HeadKind::Animated ? t.aoFn[i] : 0;
}
inline uintptr_t headVtableOf(HeadKind k, const HeadTargets& t) { return k == HeadKind::Runtime ? t.rrVtable : k == HeadKind::Animated ? t.aoVtable : 0; }
inline uint32_t headCachedFlagOffset(HeadKind k) { return k == HeadKind::Runtime ? kRrCachedFlag : kAoCachedFlag; }
// The checks that come before a call: the vtable pointer is the kind's and every slot used holds exactly the expected function. 0 = fine,
// 1 = the vtable differs, 2 = slot i differs (slotOut = its index, foundOut = what it holds).
inline int headVerifySlots(uint64_t vptr, const uint64_t found[kHeadSlots], HeadKind k, const HeadTargets& t, uint32_t* slotOut, uint64_t* foundOut) {
    if (k == HeadKind::None || vptr != headVtableOf(k, t)) return 1;
    for (uint32_t i = 0; i < kHeadSlots; ++i)
        if (found[i] != headExpectedFunction(k, i, t)) {
            *slotOut = i;
            *foundOut = found[i];
            return 2;
        }
    return 0;
}

// The product of two 3x3 matrices in the row-vector convention (out = a x b; row-major).
inline void mulRows3(const float a[9], const float b[9], float out[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += a[i * 3 + k] * b[k * 3 + j];
            out[i * 3 + j] = s;
        }
}
// The rotation of joint `idx` in model space, as three rows: R = R_idx x R_parent x R_grandparent ... (a vector in the joint's own axes, times R, is the
// same vector in model space). The positions' walk applies each ancestor the same way. False for an index or a parent out of range, or a cycle.
inline bool walkJointRotationToModel(const float* locals, const uint16_t* parents, uint32_t joints, uint32_t idx, float out[9]) {
    if (idx >= joints) return false;
    float r[9];
    quatRows(locals + idx * kJointFloats + 4, r);
    uint32_t steps = 0;
    for (uint32_t a = parents[idx]; a != 0xFFFFu; a = parents[a]) {
        if (a >= joints || ++steps > joints) return false;
        float q[9], next[9];
        quatRows(locals + a * kJointFloats + 4, q);
        mulRows3(r, q, next);
        std::memcpy(r, next, sizeof(r));
    }
    std::memcpy(out, r, sizeof(r));
    return true;
}

// Rows 0-2 of a 4x4 row-vector matrix are near unit length (a rotation, possibly with a modest scale).
inline bool rowsNearUnit(const float m16[16]) {
    for (int r = 0; r < 3; ++r) {
        const float x = m16[r * 4], y = m16[r * 4 + 1], z = m16[r * 4 + 2];
        const float len2 = x * x + y * y + z * z;
        if (!(len2 > 0.25f && len2 < 4.0f)) return false;   // also false for NaN
    }
    return true;
}
// A live +0x58 matrix of a joint (avatar-root relative, row vectors, translation in the fourth row) the census may trust: finite, rows near unit
// length, and a head that is inside the avatar's own surroundings (x and z within 1.5 m of its root, y between 0.2 and 2.6 m).
inline bool headMatrixPlausible(const float m[16]) {
    for (int i = 0; i < 16; ++i)
        if (!(std::fabs(m[i]) < 100.0f)) return false;   // false for NaN too
    if (!rowsNearUnit(m)) return false;
    return std::fabs(m[12]) <= 1.5f && std::fabs(m[14]) <= 1.5f && m[13] >= 0.2f && m[13] <= 2.6f;
}
// The skeleton interface's kind, for the log.
inline const char* hkindText(uint32_t k) { return k == static_cast<uint32_t>(HeadKind::Runtime) ? "RR (RuntimeRigComponent)" : k == static_cast<uint32_t>(HeadKind::Animated) ? "AO (AnimatedObject)" : "unknown kind"; }

}  // namespace avh
}  // namespace edvr
