// Explorer Cam, Phase 3: the pure half of the head-joint eye source and of the camera-suite isolation
// (docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 3: the camera follows the head joint").
//
// THE EYE FOLLOWS THE HEAD. F7 showed the cached +0x58 head joint of the TRUE local skeleton follows stance (standing (0.00, 1.66, 0.03), crouched (0.09, 0.94,
// 0.19), weapon out (0.10, 1.38, 0.19) in the avatar's model space: +x right, +y up, +z forward, the commander-local axes the placement writes). The free camera's
// commander-local pose is written before every update, so the eye is
//     eye = head joint position + head joint rotation x offset,   then + the trims,
// where the offset is the povCamera joint's place relative to the head in the REST pose, expressed in the head joint's own axes: derived ONCE per skeleton
// from the animated pose's local transforms (the pose walk the F5 flight proved: P+0x48 locals, P+0x50 parents), because that pose is the bind pose and never moves.
// Everything here is arithmetic, tables and text; the glue (explorer_cam.cpp) does the guarded reads and the game calls.
//
// ISOLATION. While a session has placed the view, the camera suite's own input actions are cleared before each camera update reads them and put back after,
// so Explorer Cam is an independent mode (F5 is the only way out). The tables below say which action objects each camera object holds and which field of the
// action object the game reads for it.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "explorer_cam_core.h"

namespace edvr {
namespace ecm {

// ---- A. the skeleton interface, the pose walk and the slot checks (build 332841) -----------------------------------------------------------------------
// The verified addresses of the build the head-joint eye reads (found by the flights' skeleton instruments, since retired; the arc doc has the evidence).
constexpr uintptr_t kRrVtableRva = 0x559CF90, kAoVtableRva = 0x517DC20;
constexpr uint32_t kHeadSlots = 4;                                                    // the slots used: +0x18, +0x30, +0x48, +0x58
constexpr uint32_t kHeadSlotIndex[kHeadSlots] = {3, 6, 9, 11};
constexpr uintptr_t kRrFunctions[kHeadSlots] = {0x43FB900, 0xFDDB10, 0x43F7180, 0x43F6EB0};
constexpr uintptr_t kAoFunctions[kHeadSlots] = {0xFDE310, 0xFDDB10, 0xFDDEB0, 0xFDDD10};
constexpr uint32_t kRrCachedFlag = 0x2C0, kAoCachedFlag = 0xF8;                      // iface+: the joint matrices are cached when it is non-zero
constexpr uintptr_t kHeadNameRva = 0x554EE10, kFootLNameRva = 0x5564118, kFootRNameRva = 0x5564130;
inline constexpr char kHeadName[] = "def_c_head_joint";
inline constexpr char kPovName[] = "def_c_povCamera_joint";
inline constexpr char kFootLName[] = "def_l_foot_joint";
inline constexpr char kFootRName[] = "def_r_foot_joint";
constexpr uint32_t kNoJoint = 0xFFFF;

enum class HeadKind : uint8_t { None = 0, Runtime = 1, Animated = 2 };

// ---- the joints by WALKING THE ANIMATED POSE ---------------------------------------------------------------------------------------------------
// F5: the cached +0x58 head did not drop when the commander crouched, and +0x48 drifted 12 m (a frame that is not the free camera's). The cached matrices
// are only maintained for joints the game asked for (SetAttachJoint marks a joint and its ancestors), so the walk composes the joints from the animated pose
// itself, exactly as the game's uncached path does (FUN 0xFDE0D0, read and its quaternion constants evaluated against the exe's own data):
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

// ---- B. the eye from the head joint ---------------------------------------------------------------------------------------------------------------
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

enum class RestWhy : uint32_t {
    Ok = 0, NoJoints, TooManyJoints, HeadMissing, PovMissing, HeadWalk, PovWalk, RotationWalk, NotRotation, ImplausibleOffset, ArraysUnreadable, NoPose, FindFailed
};
inline const char* restWhyText(RestWhy w) {
    switch (w) {
        case RestWhy::Ok: return "ok";
        case RestWhy::NoJoints: return "the skeleton's pose has no joints";
        case RestWhy::TooManyJoints: return "the skeleton has more joints than the walk's buffer";
        case RestWhy::HeadMissing: return "FindJoint(\"def_c_head_joint\") found no head joint (or an index past the joint count)";
        case RestWhy::PovMissing: return "the povCamera joint index is missing (or past the joint count)";
        case RestWhy::HeadWalk: return "the head joint's parent chain is broken (a parent out of range, or a cycle)";
        case RestWhy::PovWalk: return "the povCamera joint's parent chain is broken";
        case RestWhy::RotationWalk: return "the head joint's rotation could not be composed";
        case RestWhy::NotRotation: return "the head joint's rest rotation rows are not unit length (a quaternion that is not a rotation)";
        case RestWhy::ImplausibleOffset: return "the povCamera joint is not within half a metre of the head joint";
        case RestWhy::ArraysUnreadable: return "the pose's local transforms or parents could not be read";
        case RestWhy::NoPose: return "GetPoseData returned no pose";
        case RestWhy::FindFailed: return "the FindJoint call faulted";
        default: return "an unnamed reason";
    }
}
// What the rest pose says about where the eye is relative to the head joint.
struct RestOffset {
    float headRest[3] = {}, povRest[3] = {};   // model space
    float headRot[9] = {};                     // the head joint's rest rotation rows
    float delta[3] = {};                       // pov - head, model space
    float local[3] = {};                       // delta in the head joint's own axes: local[i] = dot(delta, row i of headRot)
    uint32_t headDepth = 0, povDepth = 0;
};
constexpr float kMaxRestOffset = 0.5f;   // metres: the eye is a few centimetres from the head joint
inline RestWhy deriveRestOffset(const float* locals, const uint16_t* parents, uint32_t joints, uint32_t headIdx, uint32_t povIdx, RestOffset* o) {
    if (joints == 0) return RestWhy::NoJoints;
    if (joints > kMaxWalkJoints) return RestWhy::TooManyJoints;
    if (headIdx == kNoJoint || headIdx >= joints) return RestWhy::HeadMissing;
    if (povIdx == kNoJoint || povIdx >= joints) return RestWhy::PovMissing;
    RestOffset r;
    if (!walkJointToModel(locals, parents, joints, headIdx, r.headRest, &r.headDepth)) return RestWhy::HeadWalk;
    if (!walkJointToModel(locals, parents, joints, povIdx, r.povRest, &r.povDepth)) return RestWhy::PovWalk;
    if (!walkJointRotationToModel(locals, parents, joints, headIdx, r.headRot)) return RestWhy::RotationWalk;
    for (int i = 0; i < 3; ++i) {
        const float len2 = r.headRot[i * 3] * r.headRot[i * 3] + r.headRot[i * 3 + 1] * r.headRot[i * 3 + 1] + r.headRot[i * 3 + 2] * r.headRot[i * 3 + 2];
        if (!(len2 > 0.9f && len2 < 1.1f)) return RestWhy::NotRotation;   // also false for NaN
    }
    for (int k = 0; k < 3; ++k) r.delta[k] = r.povRest[k] - r.headRest[k];
    for (int i = 0; i < 3; ++i) r.local[i] = r.delta[0] * r.headRot[i * 3] + r.delta[1] * r.headRot[i * 3 + 1] + r.delta[2] * r.headRot[i * 3 + 2];
    if (!(std::fabs(r.local[0]) < kMaxRestOffset && std::fabs(r.local[1]) < kMaxRestOffset && std::fabs(r.local[2]) < kMaxRestOffset)) return RestWhy::ImplausibleOffset;
    *o = r;
    return RestWhy::Ok;
}

// A live +0x58 matrix of the head joint (avatar-root relative, row vectors, translation in the fourth row) the placement may trust: finite, rows near unit
// length, and a head that is inside the avatar's own surroundings (x and z within 1.5 m of its root, y between 0.2 and 2.6 m).
inline bool headMatrixPlausible(const float m[16]) {
    for (int i = 0; i < 16; ++i)
        if (!(std::fabs(m[i]) < 100.0f)) return false;   // false for NaN too
    if (!rowsNearUnit(m)) return false;
    return std::fabs(m[12]) <= 1.5f && std::fabs(m[14]) <= 1.5f && m[13] >= 0.2f && m[13] <= 2.6f;
}
// The eye in the avatar's model space: the head joint's position plus the rest offset (in the head's own axes) turned by the head's live rotation.
inline void headEyeModel(const float m[16], const float local[3], float out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = m[12 + k] + local[0] * m[0 + k] + local[1] * m[4 + k] + local[2] * m[8 + k];
}
// The largest element-wise difference between a live matrix's rotation rows and the rest rotation rows (a self-check on the quaternion convention: standing
// still it is small, a transposed or mirrored convention would not be).
inline float rotationDiff(const float m[16], const float rest[9]) {
    float worst = 0.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float d = std::fabs(m[i * 4 + j] - rest[i * 3 + j]);
            if (d > worst) worst = d;
        }
    return worst;
}

// The trims: commander-local metres added after the joint (right, up, forward). PERMANENT user settings since 2026-10-08 (Sean flew them and kept them):
// fix.explorer_cam_eye_trim_right, _up and _forward, personal preference for where the eye sits in the head, tuned live from the F8 menu's Explorer Cam page.
struct Trim {
    float right = 0.0f, up = 0.0f, forward = 0.0f;   // the arithmetic's neutral element; the SHIPPED values are the defaults below
};
// What the trims ship as: Sean's own tuning (2026-10-08). The shipped edvr.ini carries the same three numbers, the code falls back to them for an ini that
// lacks the keys, and tools/config_test holds the three places to one value.
constexpr float kTrimRightDefault = 0.0f, kTrimUpDefault = 0.15f, kTrimForwardDefault = -0.08f;
// Held to +-0.5 m on every axis (it was +-0.3 while they were test keys): 0.15 up is half of the old range, and a taller or shorter commander, or a wider
// helmet, can want more. Past half a metre the eye is outside the head whichever way it goes.
constexpr float kTrimLimit = 0.5f;
inline float clampTrim(float v) {
    if (!(v == v)) return 0.0f;
    return v < -kTrimLimit ? -kTrimLimit : (v > kTrimLimit ? kTrimLimit : v);
}
// Model axes are the commander-local axes: +x right, +y up, +z forward.
inline Eye eyeFromModelPoint(const float p[3], const Trim& t) {
    Eye e;
    e.right = p[0] + t.right;
    e.up = p[1] + t.up;
    e.forward = p[2] + t.forward;
    return e;
}
// The follow smoothing (fix.explorer_cam_follow_smoothing_ms, a permanent user setting since 2026-10-08; milliseconds; 0 = exact follow, the default).
// An exponential approach with time constant `tau`; at 0 the target is returned unchanged, bit for bit, and the state is forgotten. The F8 menu offers
// 0..200 in steps of 10; the file may say up to 1000.
constexpr int kSmoothingMsDefault = 0;
constexpr float kSmoothingMsMax = 1000.0f;
inline float clampSmoothingMs(float ms) {
    if (!(ms == ms) || ms <= 0.0f) return 0.0f;
    return ms > kSmoothingMsMax ? kSmoothingMsMax : ms;
}
struct EyeSmoother {
    bool have = false;
    Eye prev;
};
inline Eye smoothEye(EyeSmoother& s, const Eye& target, double dtMs, float tauMs) {
    if (!(tauMs > 0.0f)) {
        s.have = false;
        return target;
    }
    if (!s.have) {
        s.have = true;
        s.prev = target;
        return target;
    }
    if (!(dtMs > 0.0)) return s.prev;
    const float a = static_cast<float>(1.0 - std::exp(-dtMs / static_cast<double>(tauMs)));
    Eye e;
    e.up = s.prev.up + (target.up - s.prev.up) * a;
    e.forward = s.prev.forward + (target.forward - s.prev.forward) * a;
    e.right = s.prev.right + (target.right - s.prev.right) * a;
    s.prev = e;
    return e;
}

// Why the eye came from the fixed keys instead of the head joint (the latest reason, for the log and the heartbeat).
enum class FixedWhy : uint32_t { None = 0, NotArmed, NothingLatched, Unverified, Stale, Fault, Implausible, StoodDown };
inline const char* fixedWhyText(FixedWhy w) {
    switch (w) {
        case FixedWhy::None: return "none";
        case FixedWhy::NotArmed: return "the head-joint source is not armed (unknown game build, or the joint-name literals differ)";
        case FixedWhy::NothingLatched: return "no local skeleton pair is latched yet";
        case FixedWhy::Unverified: return "this skeleton has not been verified (its rest offset could not be derived)";
        case FixedWhy::Stale: return "the latched interface is no longer a skeleton interface (the avatar was destroyed)";
        case FixedWhy::Fault: return "a read or call of the head joint faulted";
        case FixedWhy::Implausible: return "the head joint's matrix looked wrong";
        case FixedWhy::StoodDown: return "the head-joint source stood down for the session";
        default: return "an unnamed reason";
    }
}
constexpr uint32_t kMaxFollowFaults = 8;   // faults of the head read before the head source (not the placement) stands down
constexpr uint32_t kFollowRetryUpdates = 60;   // a skeleton whose pose is not built yet (no pose, or no joints) is tried again this many placing updates later

enum class FollowNoteKind : uint32_t { None = 0, Rest, RestFailed, FirstLive, Switched, StoodDown };
// One thing the camera-job thread says about the head source; the frame thread writes the line.
struct FollowNote {
    uint32_t kind = 0;
    uint32_t why = 0;             // RestFailed: RestWhy; Switched: FixedWhy (None = now the head joint); StoodDown: 0 = faults, 1 = a vtable slot differs
    uint64_t iface = 0;
    uint32_t hkind = 0;           // HeadKind
    uint32_t joints = 0;
    uint32_t headIdx = 0, povIdx = 0;
    uint32_t headDepth = 0, povDepth = 0;
    uint32_t faults = 0;
    uint32_t pad = 0;
    float headRest[3] = {}, povRest[3] = {}, delta[3] = {}, local[3] = {};
    float live[3] = {};           // FirstLive: the live head position
    float eye[3] = {};            // FirstLive: the eye (up, forward, right)
    float rotDiff = 0.0f;         // FirstLive: largest element difference between the live and the rest rotation rows
    float pad2 = 0.0f;
    uint64_t a = 0, b = 0, c = 0; // StoodDown by a slot: the slot's byte offset, what it holds, what it should hold
};
static_assert(std::is_trivially_copyable<FollowNote>::value, "notes are copied through the ring");

inline const char* prefixFollow() { return "explorer cam: head follow:"; }
inline const char* hkindText(uint32_t k) { return k == static_cast<uint32_t>(HeadKind::Runtime) ? "RR (RuntimeRigComponent)" : k == static_cast<uint32_t>(HeadKind::Animated) ? "AO (AnimatedObject)" : "unknown kind"; }
inline void formatFollowNote(char* out, size_t cap, const FollowNote& n) {
    Line o(out, cap);
    const unsigned long long iface = static_cast<unsigned long long>(n.iface);
    switch (static_cast<FollowNoteKind>(n.kind)) {
        case FollowNoteKind::Rest:
            o.put("%s head joint of skeleton 0x%llX (%s, %u joints) verified: head idx %u rest (%.3f,%.3f,%.3f) depth %u, povCamera idx %u rest (%.3f,%.3f,%.3f) depth %u; "
                  "the eye sits at (%.3f,%.3f,%.3f) from the head in model space, which is (%.3f,%.3f,%.3f) in the head joint's own axes; from now on every placing "
                  "update reads the head with +0x58 and puts the eye at head + head rotation x that offset + the trims (no smoothing unless "
                  "fix.explorer_cam_follow_smoothing_ms is set)",
                  prefixFollow(), iface, hkindText(n.hkind), n.joints, n.headIdx, n.headRest[0], n.headRest[1], n.headRest[2], n.headDepth, n.povIdx, n.povRest[0],
                  n.povRest[1], n.povRest[2], n.povDepth, n.delta[0], n.delta[1], n.delta[2], n.local[0], n.local[1], n.local[2]);
            break;
        case FollowNoteKind::RestFailed:
            o.put("%s the head joint of skeleton 0x%llX (%s, %u joints) cannot be used: %s (head idx %u, povCamera idx %u); the fixed eye keys place the view for this "
                  "skeleton",
                  prefixFollow(), iface, hkindText(n.hkind), n.joints, restWhyText(static_cast<RestWhy>(n.why)), n.headIdx, n.povIdx);
            break;
        case FollowNoteKind::FirstLive:
            o.put("%s first live head joint (+0x58) of skeleton 0x%llX: head (%.3f,%.3f,%.3f) in the avatar's model space; its rotation rows differ from the rest pose's by "
                  "at most %.3f per element (small standing still; large would mean the quaternion convention is wrong); the eye is up=%.3f forward=%.3f right=%.3f "
                  "in the commander's frame",
                  prefixFollow(), iface, n.live[0], n.live[1], n.live[2], n.rotDiff, n.eye[0], n.eye[1], n.eye[2]);
            break;
        case FollowNoteKind::Switched:
            if (n.why == static_cast<uint32_t>(FixedWhy::None))
                o.put("%s the eye now comes from the head joint of skeleton 0x%llX", prefixFollow(), iface);
            else
                o.put("%s the eye now comes from the FIXED keys (fix.explorer_cam_eye_up/_forward/_right): %s", prefixFollow(), fixedWhyText(static_cast<FixedWhy>(n.why)));
            break;
        case FollowNoteKind::StoodDown:
            if (n.why == 1)
                o.put("%s the head-joint source stood down for the session: vtable slot +0x%llX of skeleton 0x%llX holds 0x%llX, not the build-332841 function 0x%llX; the "
                      "fixed eye keys place the view from now on",
                      prefixFollow(), static_cast<unsigned long long>(n.a), iface, static_cast<unsigned long long>(n.b), static_cast<unsigned long long>(n.c));
            else
                o.put("%s the head-joint source stood down for the session: %u reads or calls of the head joint faulted; the fixed eye keys place the view from now on, "
                      "and the placement goes on",
                      prefixFollow(), n.faults);
            break;
        default:
            o.put("%s note %u (unnamed)", prefixFollow(), n.kind);
            break;
    }
}

// ---- C. isolating the camera suite ----------------------------------------------------------------------------------------------------------------
// Every action the game reads is an object with three fields that matter: a float axis at +0x18, an int "pressed" at +0x1C (edge buttons) and a byte "held" at
// +0x24 (hold buttons; the game derives edges from +0x24 against its previous-held byte at +0x34). A camera object keeps a pointer to each of its actions
// ("handle") at a fixed offset of its own. The clear saves the field, writes zero, and the restore writes the saved value back after the update, so a held
// byte is never left zeroed (it would read as a fresh press). Handles are read again on every call: the binder re-resolves them on a context change.
enum class IsoKind : uint8_t { Axis = 0, Pressed = 1, Held = 2 };
struct IsoField {
    uint16_t handle;   // the offset in the holder of the pointer to the action object
    IsoKind kind;      // the field of the action object the game reads
};
constexpr uint32_t kOffActionAxis = 0x18, kOffActionHeld = 0x24;   // (+0x1C is kOffActionPressed)
constexpr uint32_t isoFieldOffset(IsoKind k) { return k == IsoKind::Axis ? kOffActionAxis : k == IsoKind::Pressed ? kOffActionPressed : kOffActionHeld; }
constexpr uint32_t isoFieldWidth(IsoKind k) { return k == IsoKind::Held ? 1u : 4u; }
enum IsoHolder : int { kIsoFreeCamera = 0, kIsoController, kIsoCameraUi, kIsoZoomDof, kIsoHolderCount };

// FreeCameraActivity (update 0x1071980): FreeCamSpeedInc/Dec held, the move/look axes, ToggleRotationLock, FixCameraWorldToggle, FixCameraRelativeToggle.
// EDVR's own lock press goes in on +0x508 after the clear.
inline constexpr IsoField kIsoTabFree[] = {
    {0x4B8, IsoKind::Held}, {0x4C0, IsoKind::Held},
    {0x4C8, IsoKind::Axis}, {0x4D0, IsoKind::Axis}, {0x4F0, IsoKind::Axis}, {0x4D8, IsoKind::Axis}, {0x4E0, IsoKind::Axis}, {0x4E8, IsoKind::Axis},
    {0x4F8, IsoKind::Pressed}, {0x500, IsoKind::Pressed}, {0x508, IsoKind::Pressed}};
// VesselCameraMountControl (update 0x2DF14C0): PhotoCameraToggle, the two scrolls, ToggleFreeCam, QuitCamera and VanityCameraOne..Ten. EDVR's own F5 presses
// (+0x310, +0x328) go in after the clear.
inline constexpr IsoField kIsoTabController[] = {
    {0x310, IsoKind::Pressed}, {0x318, IsoKind::Pressed}, {0x320, IsoKind::Pressed}, {0x328, IsoKind::Pressed}, {0x340, IsoKind::Pressed},
    {0x350, IsoKind::Pressed}, {0x358, IsoKind::Pressed}, {0x360, IsoKind::Pressed}, {0x368, IsoKind::Pressed}, {0x370, IsoKind::Pressed},
    {0x378, IsoKind::Pressed}, {0x380, IsoKind::Pressed}, {0x388, IsoKind::Pressed}, {0x390, IsoKind::Pressed}, {0x398, IsoKind::Pressed}};
// VanityCameraUIActivity (update 0x47C7640): FreeCamToggleHud. EDVR's own hide/unhide press goes in after the clear.
inline constexpr IsoField kIsoTabCameraUi[] = {{0x1D8, IsoKind::Pressed}};
// VanityCameraDofAndZoomControls (update 0x1078990; names are data, any handle may be null): +0x260 toggles the advance mode, the rest are held (zoom, aperture, focus).
inline constexpr IsoField kIsoTabZoomDof[] = {
    {0x250, IsoKind::Held}, {0x258, IsoKind::Held}, {0x260, IsoKind::Pressed}, {0x268, IsoKind::Held}, {0x270, IsoKind::Held}, {0x278, IsoKind::Held}, {0x280, IsoKind::Held}};
constexpr size_t kIsoMaxFields = 16;
constexpr size_t kIsoCounts[kIsoHolderCount] = {sizeof(kIsoTabFree) / sizeof(kIsoTabFree[0]), sizeof(kIsoTabController) / sizeof(kIsoTabController[0]),
                                                sizeof(kIsoTabCameraUi) / sizeof(kIsoTabCameraUi[0]), sizeof(kIsoTabZoomDof) / sizeof(kIsoTabZoomDof[0])};
inline const IsoField* isoFields(int holder) {
    switch (holder) {
        case kIsoFreeCamera: return kIsoTabFree;
        case kIsoController: return kIsoTabController;
        case kIsoCameraUi: return kIsoTabCameraUi;
        case kIsoZoomDof: return kIsoTabZoomDof;
        default: return nullptr;
    }
}
inline const char* isoHolderName(int holder) {
    switch (holder) {
        case kIsoFreeCamera: return "free_camera";
        case kIsoController: return "controller";
        case kIsoCameraUi: return "camera_ui";
        case kIsoZoomDof: return "zoom";
        default: return "?";
    }
}
constexpr uint32_t kMaxIsoFaults = 3;   // faults of the isolation's guarded reads and writes before it stands down for the session (placement goes on)

// What a clear saved: only the fields that were non-zero, each with the address, the value and the width, so the restore can write it back bit for bit.
struct IsoSaved {
    uint64_t addr[kIsoMaxFields] = {};
    uint32_t value[kIsoMaxFields] = {};
    uint8_t width[kIsoMaxFields] = {};
    uint32_t count = 0;   // entries saved (= presses blocked in this call)
};

// The once-per-session line.
inline void formatIsolation(char* out, size_t cap, bool zoomArmed, const char* hotkey) {
    Line o(out, cap);
    size_t actions = 0, holders = 0;
    for (int h = 0; h < kIsoHolderCount; ++h) {
        if (h == kIsoZoomDof && !zoomArmed) continue;
        actions += kIsoCounts[h];
        ++holders;
    }
    o.put("%s camera suite isolated: %u actions blocked across %u holders (free camera %u, camera controller %u, camera UI %u", prefix(), static_cast<unsigned>(actions),
          static_cast<unsigned>(holders), static_cast<unsigned>(kIsoCounts[kIsoFreeCamera]), static_cast<unsigned>(kIsoCounts[kIsoController]),
          static_cast<unsigned>(kIsoCounts[kIsoCameraUi]));
    if (zoomArmed) o.put(", zoom/DOF %u", static_cast<unsigned>(kIsoCounts[kIsoZoomDof]));
    o.put(") from the moment the view is placed until you press %s again: each is cleared before the camera's own update reads it and put back after it, so the keys, "
          "the mouse and the pad cannot move or detach the camera, change preset, zoom, or close the suite; EDVR's own presses (F5's open and TAB, the relative lock, "
          "the UI hide) go in after the clear. Walking and turning are not touched (they are the commander's own actions, another object)",
          hotkey && hotkey[0] ? hotkey : "the hotkey");
    if (!zoomArmed) o.put("; the zoom/DOF hook is not in place, so its zoom, aperture and focus keys are not blocked");
}

// ---- D. the second heartbeat line: the eye source and the isolation ------------------------------------------------------------------------------------
struct FollowBeatIn {
    double windowSeconds = 0;
    const char* source = "fixed";          // "head-joint" | "fixed"
    FixedWhy why = FixedWhy::None;         // the latest reason for the fixed keys
    Eye lastEye;                           // the last eye written
    uint64_t headUpdates = 0, headWindow = 0, fixedUpdates = 0, fixedWindow = 0;
    uint32_t headFaults = 0;
    bool headDown = false;
    uint32_t t58n = 0, t58min = 0, t58max = 0, t58allMax = 0;   // microseconds, the +0x58 call: this window (allMax over the session)
    Trim trim;
    float smoothingMs = 0.0f;
    bool isoDown = false;
    uint32_t isoFaults = 0;
    uint64_t blocked[kIsoHolderCount] = {}, blockedWindow[kIsoHolderCount] = {};
    uint64_t zoomCalls = 0, zoomCallsWindow = 0;
    bool zoomArmed = false;
    const char* fadePhase = "clear";       // the comfort fade (explorer_cam_fade_core.h)
    const char* fadeKind = "idle";
    float fadeAlpha = 0.0f;
};
inline void formatFollowBeat(char* out, size_t cap, const FollowBeatIn& h) {
    Line o(out, cap);
    o.put("%s heartbeat (follow, isolation): window=%.1fs eye_source=%s last_eye(up=%.3f forward=%.3f right=%.3f) head_joint_updates=%llu(+%llu) fixed_updates=%llu(+%llu) "
          "fixed_reason=\"%s\" head_source=%s head_faults=%u",
          prefix(), h.windowSeconds, h.source, h.lastEye.up, h.lastEye.forward, h.lastEye.right, static_cast<unsigned long long>(h.headUpdates),
          static_cast<unsigned long long>(h.headWindow), static_cast<unsigned long long>(h.fixedUpdates), static_cast<unsigned long long>(h.fixedWindow),
          fixedWhyText(h.why), h.headDown ? "stood down" : "on", h.headFaults);
    if (h.t58n == 0) o.put(" read58_us(n/min/max/session_max)=0/-/-/%u", h.t58allMax);
    else o.put(" read58_us(n/min/max/session_max)=%u/%u/%u/%u", h.t58n, h.t58min, h.t58max, h.t58allMax);
    o.put(" trims(right=%.3f up=%.3f forward=%.3f) follow_smoothing_ms=%.0f isolation=%s isolation_faults=%u blocked_presses(", h.trim.right, h.trim.up, h.trim.forward,
          h.smoothingMs, h.isoDown ? "stood down" : "on", h.isoFaults);
    for (int i = 0; i < kIsoHolderCount; ++i)
        o.put("%s%s=%llu(+%llu)", i ? " " : "", isoHolderName(i), static_cast<unsigned long long>(h.blocked[i]), static_cast<unsigned long long>(h.blockedWindow[i]));
    o.put(") zoom_hook=%s zoom_hook_calls=%llu(+%llu) comfort_fade(phase=%s kind=%s alpha=%.3f)", h.zoomArmed ? "armed" : "not installed",
          static_cast<unsigned long long>(h.zoomCalls), static_cast<unsigned long long>(h.zoomCallsWindow), h.fadePhase, h.fadeKind, h.fadeAlpha);
}

}  // namespace ecm
}  // namespace edvr
