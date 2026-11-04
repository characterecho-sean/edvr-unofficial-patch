// Test rig for the vehicle rig probe's pure half (src/d3d11/vehicle_rig_core.h, advanced.vehicle_rig_probe, TEMPORARY, issue 186), on plain memory: a fake game address space with
// fault injection, laid out at the offsets the exe showed (R = *(I+0x2F0), the record array at *(R+0x280), count at *(R+0x298), stride 0x2F0, the module handle array at
// *(J+0x1D8)). No DLL and no game. `--self-test` is the only mode; it writes nothing.
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
    for (int i = 0; i < 16; ++i) m.put<float>(r.R + vr::kRigWorld + 4 * i, 100.0f + i);
    // Records: node -> model (centre, radius) ; transform (local 4x4 with a translation, parent = the rig world pointer).
    const uint64_t pool = r.arr + count * vr::kRecordStride + 0x100;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t rec = r.arr + i * vr::kRecordStride;
        const uint64_t node = pool + i * 0x200, model = node + 0x40, xf = node + 0x100;
        m.put<uint64_t>(rec + vr::kRecNode, node);
        m.put<uint64_t>(node + vr::kNodeModel, model);
        const float c[3] = {1.0f * float(i), 0.0f, 0.0f};
        m.putBytes(model + vr::kModelCentre, c, 12);
        m.put<float>(model + vr::kModelRadius, 0.5f);
        m.put<uint64_t>(rec + vr::kRecXform, xf);
        float L[16] = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, 10.0f, 20.0f, 30.0f, 1.0f};
        m.putBytes(xf, L, sizeof(L));
        m.put<uint64_t>(xf + vr::kXformParent, r.R + vr::kRigWorld);
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
    vr::describeRig(m, rr, &info);
    check(info.haveName && std::strcmp(info.name, "Cobra ?Mk? V") == 0, "the name is read through **(R+0x20), quotes made safe for the log");
    check(info.haveRoot && near(info.rootSphere[0], 5) && near(info.rootSphere[3], 8), "the root record's sphere is rec+0x270 / +0x280");
    check(info.haveWorld && near(info.world[12], 112.0f), "the world block is at R+0x30");
    check(info.spheresRead == 4 && info.spheresSkipped == 0 && info.parentIsRig == 4, "every sphere read, every parent is the rig world");
    // c = (i,0,0), L translates by (10,20,30): centres 10..13, y=20, z=30, radius 0.5.
    check(near(info.boxMin[0], 9.5f) && near(info.boxMax[0], 13.5f) && near(info.boxMin[1], 19.5f) && near(info.boxMax[1], 20.5f) && near(info.boxMin[2], 29.5f) &&
              near(info.boxMax[2], 30.5f),
          "the node box is c*L +- r");
    check(near(info.scaleMin, 1.0f) && near(info.scaleMax, 1.0f), "unit transform scale");
    FakeMem ms(1 << 20);
    const Rig s = buildRig(ms, kBase + 0x1000, 2, "Scaled", 2.0f);
    vr::RigRead rs;
    vr::readRig(ms, s.H, s.E, s.I, kVtable, &rs);
    vr::describeRig(ms, rs, &info);
    check(near(info.scaleMin, 2.0f), "a non-1 transform scale is visible");
    // A fault in a record skips it, never throws.
    m.faultAt = {a.arr + 2 * vr::kRecordStride + vr::kRecNode};
    vr::describeRig(m, rr, &info);
    check(info.spheresRead == 3 && info.spheresSkipped == 1, "a faulting record is skipped and counted");
}

void testTable() {
    vr::RoleTable t;
    check(t.classify(kBase) == vr::kOther, "an empty table classifies everything as other");
    const uint64_t shipLo = 0x1000, shipHi = shipLo + 5 * 0x2F0, cockLo = 0x9000, cockHi = cockLo + 3 * 0x2F0;
    vr::RoleTable::Range r[3] = {{shipLo, shipHi, vr::kShip}, {cockLo, cockHi, vr::kCockpit}, {0x20000, 0x20000, vr::kModule}};   // the last is empty (lo == hi)
    t.publish(r, 3);
    check(t.classify(shipLo - 1) == vr::kOther, "one byte before the ship array is other");
    check(t.classify(shipLo) == vr::kShip, "the first byte of the ship array is the ship");
    check(t.classify(shipLo + 0x2F0) == vr::kShip, "the second record is the ship");
    check(t.classify(shipHi - 1) == vr::kShip, "the last byte of the ship array is the ship");
    check(t.classify(shipHi) == vr::kOther, "the array's end is exclusive");
    check(t.classify(cockLo) == vr::kCockpit && t.classify(cockHi - 1) == vr::kCockpit && t.classify(cockHi) == vr::kOther, "the cockpit boundaries");
    check(t.classify(0x20000) == vr::kOther, "a zero-length range matches nothing");
    check(t.classify(0) == vr::kOther && t.classify(UINT64_MAX) == vr::kOther, "the extremes are other");
    t.publish(r, 0);
    check(t.classify(shipLo) == vr::kOther, "a teardown publishes an empty table: the old array no longer matches");
    vr::RoleTable::Range many[vr::kMaxRigs + 3];
    for (uint32_t i = 0; i < vr::kMaxRigs + 3; ++i) many[i] = {0x100000ull + i * 0x10000ull, 0x100000ull + i * 0x10000ull + 0x100, vr::kModule};
    t.publish(many, vr::kMaxRigs + 3);
    check(t.count() == vr::kMaxRigs, "more ranges than slots are clamped, not overrun");
    check(t.classify(0x100000ull + (vr::kMaxRigs - 1) * 0x10000ull) == vr::kModule && t.classify(0x100000ull + vr::kMaxRigs * 0x10000ull) == vr::kOther, "the clamp keeps the first kMaxRigs");

    vr::Counts c;
    c.note(vr::kShip);
    c.note(vr::kShip);
    c.note(vr::kRaced);
    c.note(99);
    check(c.bin[vr::kShip].load() == 2 && c.bin[vr::kRaced].load() == 1 && c.bin[vr::kOther].load() == 1, "counts land in their bins; an unknown bin is other");
}

// Readers hammer classify while a writer alternates two tables; every answer must be a valid bin, a record that is in neither table is never anything but other or raced, and the
// stable answers (the same in both tables) are never raced-away wrongly. No fault, no out-of-range bin.
void testConcurrent() {
    vr::RoleTable t;
    vr::RoleTable::Range a[2] = {{0x1000, 0x2000, vr::kShip}, {0x5000, 0x6000, vr::kCockpit}};
    vr::RoleTable::Range b[3] = {{0x1000, 0x2000, vr::kShip}, {0x8000, 0x9000, vr::kTurret}, {0xA000, 0xB000, vr::kModule}};
    t.publish(a, 2);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad{0}, shipOk{0}, raced{0}, total{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const uint32_t s = t.classify(0x1800);   // a ship in both tables
                const uint32_t x = t.classify(0x5800);   // a cockpit only in A
                const uint32_t y = t.classify(0x8800);   // a turret only in B
                const uint32_t o = t.classify(0xF000);   // in neither
                total.fetch_add(4, std::memory_order_relaxed);
                if (s >= vr::kBins || x >= vr::kBins || y >= vr::kBins || o >= vr::kBins) bad.fetch_add(1, std::memory_order_relaxed);
                if (s == vr::kShip) shipOk.fetch_add(1, std::memory_order_relaxed);
                if (s != vr::kShip && s != vr::kRaced) bad.fetch_add(1, std::memory_order_relaxed);
                if (x != vr::kCockpit && x != vr::kOther && x != vr::kRaced) bad.fetch_add(1, std::memory_order_relaxed);
                if (y != vr::kTurret && y != vr::kOther && y != vr::kRaced) bad.fetch_add(1, std::memory_order_relaxed);
                if (o != vr::kOther && o != vr::kRaced) bad.fetch_add(1, std::memory_order_relaxed);
                if (s == vr::kRaced) raced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    bool flip = false;
    while (std::chrono::steady_clock::now() < until) {
        if (flip) t.publish(a, 2);
        else t.publish(b, 3);
        flip = !flip;
    }
    stop.store(true);
    for (auto& th : readers) th.join();
    check(bad.load() == 0, "concurrent classify: every answer is a valid bin and only miscounts (raced), never garbage");
    check(shipOk.load() > 0, "concurrent classify: stable answers still come through");
    std::printf("vehicle_rig_test: concurrent: %llu classifications, %llu raced\n", static_cast<unsigned long long>(total.load()), static_cast<unsigned long long>(raced.load()));
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

// What the emit tap costs per call on this machine: one classify and one counter increment, against a four-rig table.
void benchTap() {
    vr::RoleTable t;
    vr::RoleTable::Range r[4] = {{0x10000, 0x20000, vr::kShip}, {0x30000, 0x40000, vr::kCockpit}, {0x50000, 0x60000, vr::kTurret}, {0x70000, 0x80000, vr::kModule}};
    t.publish(r, 4);
    vr::Counts c;
    const int n = 20000000;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t rec = 0x10000;
    for (int i = 0; i < n; ++i) {
        c.note(t.classify(rec));
        rec = rec * 6364136223846793005ull + 1442695040888963407ull;
        rec &= 0xFFFFFull;
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
    uint64_t sum = 0;
    for (auto& b : c.bin) sum += b.load();
    check(sum == uint64_t(n), "the bench counted every call");
    std::printf("vehicle_rig_test: emit tap (classify + count, four rigs): %.1f ns per call\n", ns);
}

int selfTest() {
    testReadRig();
    testDescribe();
    testTable();
    testConcurrent();
    testPose();
    benchTap();
    std::printf("vehicle_rig_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return selfTest();
    std::printf("usage: vehicle_rig_test --self-test\n");
    return 2;
}
