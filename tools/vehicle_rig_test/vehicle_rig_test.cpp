// Test rig for the vehicle rig's pure half (src/d3d11/vehicle_rig_core.h, issue 186), on plain memory: a fake game address space with fault injection, laid out at the offsets the exe
// showed (R = *(I+0x2F0), the record array at *(R+0x280), count at *(R+0x298), stride 0x2F0). No DLL and no game. `--self-test` is the only mode; it writes nothing.
#include "../../src/d3d11/vehicle_rig_core.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vr = edvr::vrig;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// A fake address space: [kBase, kBase + size), virtual addresses stored inside as plain u64s.
constexpr uint64_t kBase = 0x100000000ull;
struct FakeMem {
    std::vector<uint8_t> bytes;
    std::set<uint64_t> faultAt;      // a read that touches one of these addresses faults
    uint64_t reads = 0;
    explicit FakeMem(size_t n) : bytes(n, 0) {}
    bool read(void* dst, uintptr_t src, size_t n) {
        ++reads;
        for (uint64_t f : faultAt)
            if (f >= src && f < src + n) return false;
        if (src < kBase || src + n > kBase + bytes.size() || src + n < src) return false;
        std::memcpy(dst, &bytes[src - kBase], n);
        return true;
    }
    template <class T>
    void put(uint64_t addr, T v) {
        std::memcpy(&bytes[addr - kBase], &v, sizeof(T));
    }
    void putBytes(uint64_t addr, const void* p, size_t n) { std::memcpy(&bytes[addr - kBase], p, n); }
};

// One rig laid out in a FakeMem: handle block H (E at +0xC8), interface I (vtable, state, R pointer), rig R (name, world, array, count), records, nodes, models, transforms.
constexpr uint64_t kVtable = 0x140000000ull + 0x55956B0;
struct Rig {
    uint64_t H, E, I, R, arr, count, nameStr;
};
Rig buildRig(FakeMem& m, uint64_t at, uint64_t count, const char* name, float scale = 1.0f) {
    Rig r{};
    r.H = at;                 // 0x100 bytes
    r.E = at + 0x100;         // opaque
    r.I = at + 0x200;         // 0x400 bytes
    r.R = at + 0x600;         // 0x400 bytes
    r.nameStr = at + 0xA00;   // 0x40
    const uint64_t namePtrCell = at + 0xA40;
    r.arr = at + 0x1000;
    r.count = count;
    m.put<uint64_t>(r.H + vr::kHandleEntity, r.E);
    m.put<int32_t>(r.H + vr::kHandleState, 5);
    m.put<uint64_t>(r.I, kVtable);
    m.put<uint32_t>(r.I + vr::kIfaceState, 4);
    m.put<uint64_t>(r.I + vr::kIfaceRig, r.R);              // THE POINTER: R is not I+0x2F0
    m.put<uint64_t>(r.R + vr::kRigNamePtr, namePtrCell);
    m.put<uint64_t>(namePtrCell, r.nameStr);
    m.putBytes(r.nameStr, name, std::strlen(name) + 1);
    m.put<uint64_t>(r.R + vr::kRigRecords, r.arr);
    m.put<uint64_t>(r.R + vr::kRigCount, count);
    // Records: a transform (local 4x4, its scale is what readPose shows beside the pose), a position and a rotation, and a sphere radius that grows with the index.
    const uint64_t pool = r.arr + count * vr::kRecordStride + 0x100;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t rec = r.arr + i * vr::kRecordStride;
        const uint64_t xf = pool + i * 0x200;
        m.put<uint64_t>(rec + vr::kRecXform, xf);
        float L[16] = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, 10.0f, 20.0f, 30.0f, 1.0f};
        m.putBytes(xf, L, sizeof(L));
        m.put<float>(rec + vr::kRecSphereRadius, 1.0f + float(i));
        const float p[3] = {1.0f, 2.0f, 3.0f};
        m.putBytes(rec + vr::kRecPos, p, 12);
        const uint32_t q[2] = {0x7FFF7FFFu, 0xFFFE7FFFu};   // identity: x=y=z=0, w=1
        m.putBytes(rec + vr::kRecQuat, q, 8);
    }
    if (count) {
        const float rc[4] = {5.0f, 6.0f, 7.0f, 8.0f};
        m.putBytes(r.arr + vr::kRecSphereCentre, rc, 12);
        m.put<float>(r.arr + vr::kRecSphereRadius, rc[3]);
    }
    return r;
}

uint32_t packQ(float x, float y, float z, float w, uint32_t out[2]) {
    auto enc = [](float v) { return static_cast<uint32_t>(std::lround((v + 1.0f) * 32767.0f)) & 0xFFFFu; };
    out[0] = enc(x) | (enc(y) << 16);
    out[1] = enc(z) | (enc(w) << 16);
    return 0;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void testReadRig() {
    FakeMem m(1 << 20);
    const Rig a = buildRig(m, kBase + 0x1000, 5, "Cobra Mk V");
    vr::RigRead rr;
    check(vr::readRig(m, a.H, a.E, a.I, kVtable, &rr) == vr::Status::kOk, "a ready rig reads ok");
    check(rr.R == a.R && rr.lo == a.arr && rr.hi == a.arr + 5 * 0x2F0 && rr.count == 5, "R is the pointer at I+0x2F0, the array and its end are read through it");
    // Reading R as I+0x2F0 itself (the note's literal text) would land elsewhere: the layout above proves the pointer is what is read.
    check(rr.R != a.I + vr::kIfaceRig, "R is not the address I+0x2F0");

    // Zero records: ready, empty, no range.
    FakeMem m0(1 << 20);
    const Rig z = buildRig(m0, kBase + 0x1000, 0, "Empty");
    check(vr::readRig(m0, z.H, z.E, z.I, kVtable, &rr) == vr::Status::kEmpty && rr.lo == 0 && rr.hi == 0, "a rig with a zero count is empty and has no range");

    // Torn down between frames: the arrays zeroed (0x43342E0), then the count nonzero with a null array (torn), then the entity replaced.
    FakeMem mt(1 << 20);
    const Rig t = buildRig(mt, kBase + 0x1000, 4, "Torn");
    check(vr::readRig(mt, t.H, t.E, t.I, kVtable, &rr) == vr::Status::kOk, "before the teardown it reads ok");
    mt.put<uint64_t>(t.R + vr::kRigRecords, 0);
    mt.put<uint64_t>(t.R + vr::kRigCount, 0);
    check(vr::readRig(mt, t.H, t.E, t.I, kVtable, &rr) == vr::Status::kEmpty, "after the teardown the zeroed fields read empty, not the old array");
    mt.put<uint64_t>(t.R + vr::kRigCount, 4);
    check(vr::readRig(mt, t.H, t.E, t.I, kVtable, &rr) == vr::Status::kBadArray, "a count with a null array is torn, refused");
    mt.put<uint64_t>(t.R + vr::kRigRecords, t.arr);
    mt.put<uint64_t>(t.H + vr::kHandleEntity, t.E + 8);
    check(vr::readRig(mt, t.H, t.E, t.I, kVtable, &rr) == vr::Status::kEntityGone, "a handle whose entity changed is stale");
    mt.put<uint64_t>(t.H + vr::kHandleEntity, t.E);
    mt.put<uint32_t>(t.I + vr::kIfaceState, 5);
    check(vr::readRig(mt, t.H, t.E, t.I, kVtable, &rr) == vr::Status::kNotReady, "I+0x328 != 4 is not ready");
    mt.put<uint32_t>(t.I + vr::kIfaceState, 4);
    mt.put<uint64_t>(t.I + vr::kIfaceRig, 0);
    check(vr::readRig(mt, t.H, t.E, t.I, kVtable, &rr) == vr::Status::kNotReady, "a null R is not ready");

    // A vtable mismatch: nothing past the vtable is trusted.
    FakeMem mv(1 << 20);
    const Rig v = buildRig(mv, kBase + 0x1000, 3, "Wrong");
    mv.put<uint64_t>(v.I, kVtable + 0x10);
    mv.reads = 0;
    check(vr::readRig(mv, v.H, v.E, v.I, kVtable, &rr) == vr::Status::kVtableMismatch && rr.lo == 0, "a vtable mismatch is refused with no range");
    check(mv.reads == 2, "a vtable mismatch reads only the handle's entity and the vtable (no further trust)");

    // A fault in each guarded read.
    const uint64_t spots[] = {a.H + vr::kHandleEntity, a.I, a.I + vr::kIfaceState, a.I + vr::kIfaceRig, a.R + vr::kRigRecords, a.R + vr::kRigCount};
    for (uint64_t f : spots) {
        m.faultAt = {f};
        check(vr::readRig(m, a.H, a.E, a.I, kVtable, &rr) == vr::Status::kFault && rr.lo == 0, "a fault in a guarded read is a Fault status with no range");
    }
    m.faultAt.clear();
    check(vr::readRig(m, 0, a.E, a.I, kVtable, &rr) == vr::Status::kNoHandle, "no handle");
    check(vr::readRig(m, a.H, 0, a.I, kVtable, &rr) == vr::Status::kNoHandle, "no entity");
    check(vr::readRig(m, a.H, a.E, 0, kVtable, &rr) == vr::Status::kNoHandle, "no interface");

    // An absurd count is a wrong read: logged as one, nothing walked.
    m.put<uint64_t>(a.R + vr::kRigCount, vr::kMaxRecords + 1);
    check(vr::readRig(m, a.H, a.E, a.I, kVtable, &rr) == vr::Status::kAbsurd && rr.lo == 0 && rr.count == vr::kMaxRecords + 1, "a count past 4096 is Absurd, with no range");
    m.put<uint64_t>(a.R + vr::kRigCount, vr::kMaxRecords);
    check(vr::readRig(m, a.H, a.E, a.I, kVtable, &rr) == vr::Status::kOk && rr.count == vr::kMaxRecords, "4096 records is the largest accepted count");
    m.put<uint64_t>(a.R + vr::kRigCount, 0xFFFFFFFFFFFFFFFFull);
    check(vr::readRig(m, a.H, a.E, a.I, kVtable, &rr) == vr::Status::kAbsurd, "an all-ones count is Absurd");
    m.put<uint64_t>(a.R + vr::kRigCount, 5);
    m.put<uint64_t>(a.R + vr::kRigRecords, 0xFFFFFFFFFFFFFFF0ull);
    check(vr::readRig(m, a.H, a.E, a.I, kVtable, &rr) == vr::Status::kAbsurd, "an array that wraps the address space is Absurd");
}

void testDescribe() {
    FakeMem m(1 << 20);
    const Rig a = buildRig(m, kBase + 0x1000, 4, "Cobra \"Mk\" V");
    vr::RigRead rr;
    check(vr::readRig(m, a.H, a.E, a.I, kVtable, &rr) == vr::Status::kOk, "describe: reads");
    vr::RigInfo info;
    vr::describeRig(m, rr, false, &info);
    check(info.haveName && std::strcmp(info.name, "Cobra ?Mk? V") == 0, "the name is read through **(R+0x20), quotes made safe for the log");
    check(info.haveRoot && near(info.rootSphere[0], 5) && near(info.rootSphere[3], 8), "the root record's sphere is rec+0x270 / +0x280");
    check(info.topCount == 0 && info.radiiRead == 0, "without withRadii no record radius is read");
    // The records' radii are 8 (the root, whose sphere was set above), 2, 3, 4: the two largest are the root and index 3.
    vr::describeRig(m, rr, true, &info);
    check(info.radiiRead == 4 && info.radiiSkipped == 0 && info.topCount == 2 && info.topIndex[0] == 0 && near(info.topRadius[0], 8) && info.topIndex[1] == 3 && near(info.topRadius[1], 4),
          "the two largest record radii and their indices are found");
    // One record: one radius. No records: none, and no root.
    FakeMem m1(1 << 20);
    const Rig one = buildRig(m1, kBase + 0x1000, 1, "One");
    vr::RigRead r1;
    vr::readRig(m1, one.H, one.E, one.I, kVtable, &r1);
    vr::describeRig(m1, r1, true, &info);
    check(info.topCount == 1 && info.topIndex[0] == 0 && near(info.topRadius[0], 8), "a rig with one record has one largest radius");
    // A fault in a record's radius skips it, never throws; the order of the remaining two holds.
    m.faultAt = {a.arr + 3 * vr::kRecordStride + vr::kRecSphereRadius};
    vr::describeRig(m, rr, true, &info);
    check(info.radiiRead == 3 && info.radiiSkipped == 1 && info.topCount == 2 && info.topIndex[0] == 0 && info.topIndex[1] == 2 && near(info.topRadius[1], 3),
          "a faulting record is skipped and counted, and the next largest takes its place");
    // A non-finite or negative radius is skipped.
    m.faultAt.clear();
    m.put<float>(a.arr + 2 * vr::kRecordStride + vr::kRecSphereRadius, -1.0f);
    m.put<float>(a.arr + 1 * vr::kRecordStride + vr::kRecSphereRadius, std::nanf(""));
    vr::describeRig(m, rr, true, &info);
    check(info.radiiRead == 2 && info.radiiSkipped == 2 && info.topIndex[0] == 0 && info.topIndex[1] == 3, "a negative or NaN radius is skipped");
}

void testPose() {
    uint32_t q[2];
    float out[3];
    const float pos[3] = {100.0f, 200.0f, 300.0f};
    // Identity quaternion: eye in rec = eye - pos.
    const uint32_t ident[2] = {0x7FFF7FFFu, 0xFFFE7FFFu};
    const float eye[3] = {101.0f, 198.0f, 303.0f};
    vr::eyeInRec(pos, ident, eye, out);
    check(near(out[0], 1) && near(out[1], -2) && near(out[2], 3), "identity rotation: eye_in_rec is eye - pos");
    // 90 degrees about +z (flat_pixels_engine.turn rotates (1,0,0) to (0,1,0)); the eye at pos + (0,1,0) is at (1,0,0) in the record's frame.
    const float s = std::sqrt(0.5f);
    packQ(0, 0, s, s, q);
    float qf[4];
    vr::unpackQuat(q, qf);
    float t[3];
    const float ex[3] = {1, 0, 0};
    vr::turn(qf, ex, t);
    check(near(t[0], 0, 2e-3f) && near(t[1], 1, 2e-3f) && near(t[2], 0, 2e-3f), "turn() matches flat_pixels_engine.turn: +90 about z sends x to y");
    const float eye2[3] = {100.0f, 201.0f, 300.0f};
    vr::eyeInRec(pos, q, eye2, out);
    check(near(out[0], 1, 2e-3f) && near(out[1], 0, 2e-3f) && near(out[2], 0, 2e-3f), "the inverse rotation puts the eye on +x of the record");
    // Round trip through a rotation about a skew axis: turn(q, eyeInRec) == eye - pos.
    packQ(0.3f, -0.5f, 0.2f, std::sqrt(1 - 0.09f - 0.25f - 0.04f), q);
    const float eye3[3] = {103.0f, 197.0f, 305.5f};
    vr::eyeInRec(pos, q, eye3, out);
    vr::unpackQuat(q, qf);
    vr::turn(qf, out, t);
    check(near(t[0], 3, 5e-3f) && near(t[1], -3, 5e-3f) && near(t[2], 5.5f, 5e-3f), "turn(q, eyeInRec) returns eye - pos (skew axis)");

    // readPose through memory: bits exact, transform scale visible.
    FakeMem m(1 << 20);
    const Rig a = buildRig(m, kBase + 0x1000, 2, "Pose", 1.5f);
    vr::PoseRead p;
    check(vr::readPose(m, a.arr, &p), "readPose reads");
    float pf[3];
    vr::bitsToFloats(p.posBits, pf);
    check(near(pf[0], 1) && near(pf[1], 2) && near(pf[2], 3) && p.quatBits[0] == 0x7FFF7FFFu && p.quatBits[1] == 0xFFFE7FFFu, "pose bits are the record's own, exactly");
    check(p.haveScale && near(p.xfScale, 1.5f), "the record's transform scale is read beside the pose");
    m.faultAt = {a.arr + vr::kRecQuat};
    check(!vr::readPose(m, a.arr, &p), "a faulting pose read is false");
    m.faultAt.clear();
    m.faultAt = {a.arr + vr::kRecXform};
    check(vr::readPose(m, a.arr, &p) && !p.haveScale, "a faulting transform read keeps the pose and drops the scale");
}

int selfTest() {
    testReadRig();
    testDescribe();
    testPose();
    std::printf("vehicle_rig_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return selfTest();
    std::printf("usage: vehicle_rig_test --self-test\n");
    return 2;
}
