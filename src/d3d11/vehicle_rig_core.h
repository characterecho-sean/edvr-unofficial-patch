// The vehicle rig's pure half (issue 186): the layout reads of the player vehicle's ship and cockpit rigs, and the pose math the capture lines use. The vehicle frame the flat resolve
// takes (vehicle_frame.h) is the cockpit rig's root record, read at the emit; vehicle_rig.cpp is the glue (the hook, the SEH memory policy, the log). No game, no Windows:
// tools\vehicle_rig_test drives it on plain memory, with a fault-injecting memory policy.
//
// WHAT IT READS. docs / the analysis note verify_20261104_vehicle_rig.txt (build 334925): the camera's ICameraTargetShip names the player's ship and its "ShipCockpit" module entity; each has
// an IKinematicRig interface I; the rig instance is R = *(I+0x2F0) (a POINTER: 0x4327EC0, 0x43280C0 and 0x43265F0 all load it from I+0x2F0 before reading +0x20, +0x30 or +0x280), its records
// are one array of 0x2F0-byte records at *(R+0x280), count at *(R+0x298); the records are the ones the emit hook (FUN_144315A20) receives.
//
// EVERYTHING HERE IS READ-ONLY. A memory policy M supplies `bool read(void* dst, uintptr_t src, size_t n)` (false on a fault); nothing writes the game.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace vrig {

constexpr uint32_t kRigs = 2;                // the ship's rig and the cockpit's
constexpr uint64_t kMaxRecords = 4096;       // a count past this is a wrong read, never walked
constexpr uint64_t kRecordStride = 0x2F0;
constexpr uint32_t kReadyState = 4;          // I+0x328: 0 dead, 1 waiting, 2, 3 creating RT, 4 ready, 5 destroying RT, 6 destroying MT

// The offsets (build 334925). H = a handle block, I = the IKinematicRig interface, R = the rig instance, rec = a rig record.
constexpr uintptr_t kHandleEntity = 0xC8;    // *(H+0xC8) = the entity E (0x647E90); *(H+0xC0) is the state
constexpr uintptr_t kHandleState = 0xC0;
constexpr uintptr_t kIfaceRig = 0x2F0;       // R = *(I+0x2F0)
constexpr uintptr_t kIfaceState = 0x328;
constexpr uintptr_t kRigNamePtr = 0x20;      // name = **(R+0x20)
constexpr uintptr_t kRigRecords = 0x280;
constexpr uintptr_t kRigCount = 0x298;
constexpr uintptr_t kRecPos = 0x170;         // f32 x3
constexpr uintptr_t kRecQuat = 0x17C;        // u16 x4
constexpr uintptr_t kRecXform = 0x190;       // a pointer to the transform: local 4x4 at +0, the parent's world 4x4 pointer at +0x50
constexpr uintptr_t kRecSphereCentre = 0x270;
constexpr uintptr_t kRecSphereRadius = 0x280;
constexpr uintptr_t kTargetShip = 0x60, kTargetCockpit = 0x68;   // ICameraTargetShip: the ship's handle and the cockpit's (a third, the turret's, is at +0x70 and unused)

// The roles a tracked rig has.
constexpr uint32_t kShip = 0, kCockpit = 1;
inline const char* roleName(uint32_t role) { return role == kShip ? "ship" : "cockpit"; }

enum class Status : uint8_t {
    kOk = 0,
    kEmpty,           // ready, zero records: a rig with nothing to match
    kNoHandle,        // no handle / interface was given
    kEntityGone,      // the handle's entity is no longer the one the interface came from: stale
    kFault,           // a read faulted
    kVtableMismatch,  // *(I) is not the IKinematicRig vtable
    kNotReady,        // I+0x328 != 4, or R is null
    kAbsurd,          // a count past kMaxRecords, or an array that wraps the address space: a wrong read
    kBadArray,        // a non-zero count with a null array: torn
};
inline const char* statusName(Status s) {
    switch (s) {
        case Status::kOk: return "ok";
        case Status::kEmpty: return "empty";
        case Status::kNoHandle: return "no-handle";
        case Status::kEntityGone: return "entity-gone";
        case Status::kFault: return "fault";
        case Status::kVtableMismatch: return "vtable-mismatch";
        case Status::kNotReady: return "not-ready";
        case Status::kAbsurd: return "absurd-count";
        default: return "bad-array";
    }
}

struct RigRead {
    uint64_t R = 0;
    uint64_t lo = 0, hi = 0;    // [lo, hi): the record array; lo == hi when empty
    uint64_t count = 0;
};

// One rig, re-read from scratch (never trusted from an earlier frame: teardown 0x43342E0 returns the arrays to pools and zeroes R+0x280..0x2A0, and an address can be reused).
template <class M>
Status readRig(M& m, uint64_t h, uint64_t entity, uint64_t iface, uint64_t expectedVtable, RigRead* out) {
    *out = RigRead{};
    if (!h || !entity || !iface) return Status::kNoHandle;
    uint64_t e = 0;
    if (!m.read(&e, static_cast<uintptr_t>(h + kHandleEntity), 8)) return Status::kFault;
    if (e != entity) return Status::kEntityGone;
    uint64_t vt = 0;
    if (!m.read(&vt, static_cast<uintptr_t>(iface), 8)) return Status::kFault;
    if (vt != expectedVtable) return Status::kVtableMismatch;
    uint32_t state = 0;
    if (!m.read(&state, static_cast<uintptr_t>(iface + kIfaceState), 4)) return Status::kFault;
    if (state != kReadyState) return Status::kNotReady;
    uint64_t R = 0;
    if (!m.read(&R, static_cast<uintptr_t>(iface + kIfaceRig), 8)) return Status::kFault;
    if (!R) return Status::kNotReady;
    uint64_t arr = 0, count = 0;
    if (!m.read(&arr, static_cast<uintptr_t>(R + kRigRecords), 8) || !m.read(&count, static_cast<uintptr_t>(R + kRigCount), 8)) return Status::kFault;
    out->R = R;
    out->count = count;
    if (count > kMaxRecords) return Status::kAbsurd;
    if (count == 0) return Status::kEmpty;
    if (!arr) return Status::kBadArray;
    const uint64_t bytes = count * kRecordStride;
    if (arr > UINT64_MAX - bytes) return Status::kAbsurd;
    out->lo = arr;
    out->hi = arr + bytes;
    return Status::kOk;
}

// ---- the pose math, as tools\flat_pixels_engine.py does it ---------------------------------------------------------------------------------------------------------------------
inline void unpackQuat(const uint32_t bits[2], float q[4]) {
    const uint32_t p[4] = {bits[0] & 0xFFFFu, bits[0] >> 16, bits[1] & 0xFFFFu, bits[1] >> 16};
    for (int i = 0; i < 4; ++i) q[i] = static_cast<float>(p[i]) * (1.0f / 32767.0f) - 1.0f;
}
// turn(q, v) of flat_pixels_engine.py: (2w^2 - 1) v + 2 (q.v) q + 2 w (q x v), w = q[3].
inline void turn(const float q[4], const float v[3], float out[3]) {
    const float w = q[3];
    const float d = q[0] * v[0] + q[1] * v[1] + q[2] * v[2];
    const float cx = q[1] * v[2] - q[2] * v[1], cy = q[2] * v[0] - q[0] * v[2], cz = q[0] * v[1] - q[1] * v[0];
    const float s = 2.0f * w * w - 1.0f;
    out[0] = s * v[0] + 2.0f * d * q[0] + 2.0f * w * cx;
    out[1] = s * v[1] + 2.0f * d * q[1] + 2.0f * w * cy;
    out[2] = s * v[2] + 2.0f * d * q[2] + 2.0f * w * cz;
}
// The eye in the record's frame: the inverse of the record's rotation applied to (eye - position), scale taken as 1 (the producer fixes the emitted item's scale words to 1.0).
inline void eyeInRec(const float pos[3], const uint32_t quatBits[2], const float eye[3], float out[3]) {
    float q[4];
    unpackQuat(quatBits, q);
    q[0] = -q[0];
    q[1] = -q[1];
    q[2] = -q[2];
    const float d[3] = {eye[0] - pos[0], eye[1] - pos[1], eye[2] - pos[2]};
    turn(q, d, out);
}

// ---- one record's pose, for the capture lines ----------------------------------------------------------------------------------------------------------------------------------
struct PoseRead {
    uint32_t posBits[3] = {};
    uint32_t quatBits[2] = {};
    float xfScale = 0;       // the length of the record's transform's first row: the scale the rig's own matrix carries, to show a non-1 scale beside eye_in_rec
    bool haveScale = false;
};
template <class M>
bool readPose(M& m, uint64_t rec, PoseRead* out) {
    *out = PoseRead{};
    if (!m.read(out->posBits, static_cast<uintptr_t>(rec + kRecPos), 12) || !m.read(out->quatBits, static_cast<uintptr_t>(rec + kRecQuat), 8)) return false;
    uint64_t xf = 0;
    float row[3] = {};
    if (m.read(&xf, static_cast<uintptr_t>(rec + kRecXform), 8) && xf && m.read(row, static_cast<uintptr_t>(xf), 12)) {
        out->xfScale = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        out->haveScale = std::isfinite(out->xfScale);
    }
    return true;
}
inline void bitsToFloats(const uint32_t bits[3], float out[3]) {
    for (int i = 0; i < 3; ++i) std::memcpy(&out[i], &bits[i], 4);
}

// ---- what a rig says about itself, once, when it becomes ready ---------------------------------------------------------------------------------------------------------------
struct RigInfo {
    char name[48] = {};
    bool haveName = false;
    float rootSphere[4] = {};     // centre xyz, radius: the first record's rec+0x270 / +0x280, the whole rig's sphere in the root's own frame
    bool haveRoot = false;
    // The two largest record radii (rec+0x280), with their record index: whether a loose root sphere comes from one mesh. Read only when asked (withRadii).
    float topRadius[2] = {};
    uint64_t topIndex[2] = {};
    uint32_t topCount = 0;        // how many of the two were found
    uint64_t radiiRead = 0, radiiSkipped = 0;
};
template <class M>
void describeRig(M& m, const RigRead& rr, bool withRadii, RigInfo* out) {
    *out = RigInfo{};
    uint64_t np = 0, sp = 0;
    if (m.read(&np, static_cast<uintptr_t>(rr.R + kRigNamePtr), 8) && np && m.read(&sp, static_cast<uintptr_t>(np), 8) && sp) {
        char buf[sizeof(out->name)] = {};
        // A guarded read can fail past the end of a short string's page; read what is there, a byte run at a time.
        size_t got = 0;
        while (got + 1 < sizeof(buf) && m.read(&buf[got], static_cast<uintptr_t>(sp + got), 1) && buf[got]) ++got;
        for (size_t i = 0; i < got; ++i) {
            const unsigned char c = static_cast<unsigned char>(buf[i]);
            out->name[i] = (c >= 0x20 && c < 0x7F && c != '"') ? static_cast<char>(c) : '?';
        }
        out->haveName = got != 0;
    }
    if (rr.count) {
        // The root record is the first element of the array.
        float c[3] = {}, r = 0;
        if (m.read(c, static_cast<uintptr_t>(rr.lo + kRecSphereCentre), 12) && m.read(&r, static_cast<uintptr_t>(rr.lo + kRecSphereRadius), 4)) {
            out->rootSphere[0] = c[0];
            out->rootSphere[1] = c[1];
            out->rootSphere[2] = c[2];
            out->rootSphere[3] = r;
            out->haveRoot = true;
        }
    }
    if (!withRadii) return;
    const uint64_t cap = rr.count < kMaxRecords ? rr.count : kMaxRecords;
    for (uint64_t i = 0; i < cap; ++i) {
        float r = 0;
        if (!m.read(&r, static_cast<uintptr_t>(rr.lo + i * kRecordStride + kRecSphereRadius), 4) || !std::isfinite(r) || r < 0) {
            ++out->radiiSkipped;
            continue;
        }
        ++out->radiiRead;
        if (out->topCount == 0 || r > out->topRadius[0]) {
            out->topRadius[1] = out->topRadius[0];
            out->topIndex[1] = out->topIndex[0];
            out->topRadius[0] = r;
            out->topIndex[0] = i;
            if (out->topCount < 2) ++out->topCount;
        } else if (out->topCount < 2 || r > out->topRadius[1]) {
            out->topRadius[1] = r;
            out->topIndex[1] = i;
            out->topCount = 2;
        }
    }
}

}  // namespace vrig
}  // namespace edvr
