// The vehicle rig probe's pure half (advanced.vehicle_rig_probe, TEMPORARY, issue 186): the layout reads, the role table the emit tap range-tests against, and the pose math.
// No game, no Windows: tools\vehicle_rig_test drives it on plain memory, with a fault-injecting memory policy. vehicle_rig_probe.cpp is the glue (the hook, the SEH memory
// policy, the log).
//
// WHAT IT TESTS. docs / the analysis note verify_20261104_vehicle_rig.txt (build 334925): the camera's ICameraTargetShip names the player's ship, its "ShipCockpit" and its
// "Turret" module entities; each has an IKinematicRig interface I; the rig instance is R = *(I+0x2F0) (a POINTER: 0x4327EC0, 0x43280C0 and 0x43265F0 all load it from I+0x2F0
// before reading +0x20, +0x30 or +0x280; the note wrote I+0x2F0, the exe says the pointer stored there); its records are one array of 0x2F0-byte records at *(R+0x280), count at
// *(R+0x298). Whether those records are the ones the emit hook (FUN_144315A20) receives is what the flight decides; this file only reads and classifies.
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

constexpr uint32_t kMaxRigs = 12;            // ship, cockpit, turret, then up to nine module rigs
constexpr uint32_t kFirstModule = 3;
constexpr uint64_t kMaxRecords = 4096;       // a count past this is a wrong read, never walked
constexpr uint64_t kRecordStride = 0x2F0;
constexpr uint32_t kReadyState = 4;          // I+0x328: 0 dead, 1 waiting, 2, 3 creating RT, 4 ready, 5 destroying RT, 6 destroying MT

// The offsets (build 334925). H = a handle block, I = the IKinematicRig interface, R = the rig instance, rec = a rig record.
constexpr uintptr_t kHandleEntity = 0xC8;    // *(H+0xC8) = the entity E (0x647E90); *(H+0xC0) is the state
constexpr uintptr_t kHandleState = 0xC0;
constexpr uintptr_t kIfaceRig = 0x2F0;       // R = *(I+0x2F0)
constexpr uintptr_t kIfaceState = 0x328;
constexpr uintptr_t kRigNamePtr = 0x20;      // name = **(R+0x20)
constexpr uintptr_t kRigWorld = 0x30;        // the rig's world matrix block
constexpr uintptr_t kRigRecords = 0x280;
constexpr uintptr_t kRigCount = 0x298;
constexpr uintptr_t kRecNode = 0x18;         // model = *(*(rec+0x18)+8)
constexpr uintptr_t kNodeModel = 0x08;
constexpr uintptr_t kModelCentre = 0x20;     // float3, node-local
constexpr uintptr_t kModelRadius = 0x2C;
constexpr uintptr_t kRecPos = 0x170;         // f32 x3
constexpr uintptr_t kRecQuat = 0x17C;        // u16 x4
constexpr uintptr_t kRecXform = 0x190;       // a pointer to the transform: local 4x4 at +0, the parent's world 4x4 pointer at +0x50
constexpr uintptr_t kXformParent = 0x50;
constexpr uintptr_t kRecSphereCentre = 0x270;
constexpr uintptr_t kRecSphereRadius = 0x280;
constexpr uintptr_t kTargetShip = 0x60, kTargetCockpit = 0x68, kTargetTurret = 0x70;   // ICameraTargetShip: three handles
constexpr uintptr_t kModsNames = 0x150;      // *(J+0x150): a table, count u32 at +0x10, array pointer at +0x08, entries 0x70 bytes, the name's char* first
constexpr uintptr_t kModsNameCount = 0x10, kModsNameArray = 0x08, kModsNameStride = 0x70;
constexpr uintptr_t kModsCount = 0x1D0;      // the module count
constexpr uintptr_t kModsHandles = 0x1D8;    // a POINTER to the handle array (0x439DEC0: add rdx,[rdi+0x1D8]); entry i at +i*0x30, its first qword the handle block
constexpr uintptr_t kModsHandleStride = 0x30;

// The bins the emit tap counts into.
constexpr uint32_t kShip = 0, kCockpit = 1, kTurret = 2, kModule = 3, kOther = 4, kRaced = 5, kBins = 6;
inline const char* roleName(uint32_t role) {
    switch (role) {
        case kShip: return "ship";
        case kCockpit: return "cockpit";
        case kTurret: return "turret";
        case kModule: return "module";
        case kOther: return "other";
        default: return "raced";
    }
}

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

// ---- the published role table (a seqlock over relaxed atomics) ----------------------------------------------------------------------------------------------------------------
// One writer at a time (the refresher), any number of readers on job threads, no lock. A reader that overlaps a publish gets kRaced and the caller counts it; a torn read
// can only miscount, and every value read is an integer compared, never dereferenced.
class RoleTable {
public:
    struct Range {
        uint64_t lo = 0, hi = 0;
        uint32_t role = kOther;
    };
    void publish(const Range* ranges, uint32_t n) {
        if (n > kMaxRigs) n = kMaxRigs;
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        n_.store(n, std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            lo_[i].store(ranges[i].lo, std::memory_order_relaxed);
            hi_[i].store(ranges[i].hi, std::memory_order_relaxed);
            role_[i].store(ranges[i].role, std::memory_order_relaxed);
        }
        seq_.store(s + 2, std::memory_order_release);
    }
    // The bin for rec: its rig's role, kOther when no rig holds it, kRaced when a publish overlapped.
    uint32_t classify(uint64_t rec) const {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) return kRaced;
        uint32_t n = n_.load(std::memory_order_relaxed);
        if (n > kMaxRigs) n = kMaxRigs;
        uint32_t found = kOther;
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t lo = lo_[i].load(std::memory_order_relaxed);
            const uint64_t hi = hi_[i].load(std::memory_order_relaxed);
            if (rec >= lo && rec < hi) {
                found = role_[i].load(std::memory_order_relaxed);
                break;
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return kRaced;
        return found < kBins ? found : kOther;
    }
    uint32_t count() const { return n_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> n_{0};
    std::atomic<uint64_t> lo_[kMaxRigs]{};
    std::atomic<uint64_t> hi_[kMaxRigs]{};
    std::atomic<uint32_t> role_[kMaxRigs]{};
};

struct Counts {
    std::atomic<uint64_t> bin[kBins]{};
    void note(uint32_t b) { bin[b < kBins ? b : kOther].fetch_add(1, std::memory_order_relaxed); }
};

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
    float rootSphere[4] = {};     // centre xyz, radius: *(R+0x280)+0x270 / +0x280, the whole rig's sphere in the root's own frame
    bool haveRoot = false;
    float world[16] = {};         // 16 floats at R+0x30
    bool haveWorld = false;
    // The union of the node spheres, c x L (the model's node-local centre through the record's local 4x4, row-vector convention), in the frame of the parent's world matrix
    // (the rig's frame when that parent is R+0x30); the radius is added as is (no scale).
    float boxMin[3] = {}, boxMax[3] = {};
    float maxDist = 0;            // the farthest centre+radius from that frame's origin
    uint32_t spheresRead = 0, spheresSkipped = 0, parentIsRig = 0;
    float scaleMin = 0, scaleMax = 0;   // the records' transform row-0 lengths
};
template <class M>
void describeRig(M& m, const RigRead& rr, RigInfo* out) {
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
    out->haveWorld = m.read(out->world, static_cast<uintptr_t>(rr.R + kRigWorld), sizeof(out->world));
    const uint64_t cap = rr.count < kMaxRecords ? rr.count : kMaxRecords;
    bool first = true;
    for (uint64_t i = 0; i < cap; ++i) {
        const uint64_t rec = rr.lo + i * kRecordStride;
        uint64_t node = 0, model = 0, xf = 0, parent = 0;
        float c[3] = {}, r = 0, L[16] = {};
        if (!m.read(&node, static_cast<uintptr_t>(rec + kRecNode), 8) || !node || !m.read(&model, static_cast<uintptr_t>(node + kNodeModel), 8) || !model ||
            !m.read(c, static_cast<uintptr_t>(model + kModelCentre), 12) || !m.read(&r, static_cast<uintptr_t>(model + kModelRadius), 4) ||
            !m.read(&xf, static_cast<uintptr_t>(rec + kRecXform), 8) || !xf || !m.read(L, static_cast<uintptr_t>(xf), sizeof(L)) ||
            !m.read(&parent, static_cast<uintptr_t>(xf + kXformParent), 8) || !std::isfinite(r) || !std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]) || r < 0) {
            ++out->spheresSkipped;
            continue;
        }
        ++out->spheresRead;
        if (parent == rr.R + kRigWorld) ++out->parentIsRig;
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = c[0] * L[k] + c[1] * L[4 + k] + c[2] * L[8 + k] + L[12 + k];
        const float sc = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        for (int k = 0; k < 3; ++k) {
            const float lo = p[k] - r, hi = p[k] + r;
            if (!std::isfinite(lo) || !std::isfinite(hi)) continue;
            if (first || lo < out->boxMin[k]) out->boxMin[k] = lo;
            if (first || hi > out->boxMax[k]) out->boxMax[k] = hi;
        }
        const float dist = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) + r;
        if (std::isfinite(dist) && dist > out->maxDist) out->maxDist = dist;
        if (std::isfinite(sc)) {
            if (first || sc < out->scaleMin) out->scaleMin = sc;
            if (first || sc > out->scaleMax) out->scaleMax = sc;
        }
        first = false;
    }
}

}  // namespace vrig
}  // namespace edvr
