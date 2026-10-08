// The Explorer Cam probe's rig (src/d3d11/explorer_cam_probe_core.h, explorer_cam_probe.cpp; advanced.explorer_cam_probe).
//
// Part A drives the pure half the DLL compiles: the snapshot decode, the seqlock (a writer thread against a reader), the
// event ring (overflow and several writers), the state-change detection, the cadence and the 2 s hold, the 5376-byte
// skinned-object fingerprint (the shape the head-look branch used, and the near misses it must refuse), the yaw and pitch
// decode of known rotations, the camera tally, every log line's prefix and 3-decimal text, and the consumer against a
// scripted clock: what each heartbeat looks like when its instrument is alive but idle, the immediate change lines, the 1 Hz
// detail lines while +0x48C != 0 and for 2 s after.
//
// Part B runs the glue (explorer_cam_probe.cpp compiled with EDVR_EXPLORER_CAM_PROBE_TEST) end to end against a SYNTHETIC
// function that begins with the real 28-byte prologue: CodeHook steals 5 bytes of it, the relay and the hook forward four
// registers, the original runs FIRST (the synthetic body writes +0x48C and +0x3E0, and the snapshot must see them on the very
// first call), the return value comes back unchanged, a call from a second thread is seen, a wrong prologue stands down with
// one line and no patch, and the key going off closes the gate and detaches I1 and I2. This is "what would appear in the log
// if the new code never ran" made executable.
//
// --self-test runs it and prints "explorer cam probe: PASS" only when every check holds.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/d3d11/explorer_cam.h"
#include "../../src/d3d11/explorer_cam_f2.h"
#include "../../src/d3d11/explorer_cam_f2_core.h"
#include "../../src/d3d11/explorer_cam_probe.h"
#include "../../src/d3d11/explorer_cam_probe_core.h"
#include "../../src/d3d11/flat_camera_inject.h"
#include "../../src/d3d11/vr_camera_census.h"
#include "../../src/common/code_hook.h"

using namespace edvr;

// ---- stubs for what the glue calls -------------------------------------------------------------------------------------
namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
// explorer_cam.cpp (the shared hook) asks the journal; this rig has none.
bool journalWatchActive() { return false; }
bool journalGameplay() { return true; }
bool journalOnFootKnown() { return false; }
bool journalOnFoot() { return false; }
bool vrCameraCensusWanted() { return true; }
const char* flatCameraInjectObserveStatus() { return "installed"; }
namespace explorercamprobetest {
void setTarget(uintptr_t target);
void boundary(uint32_t frame, uint64_t nowMs, bool want, ecp::SinkFn fn, void* ctx);
ecp::Shared& shared();
ecp::SkinTee& skin();
ecp::CamTee& cam();
size_t stolenBytes();
bool gateOpen();
void reset();
void observe(void* a);
}  // namespace explorercamprobetest
}  // namespace edvr

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_failures; std::printf("  FAIL  %s\n", what); }
    else std::printf("  ok    %s\n", what);
}
bool closeTo(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

struct Capture {
    std::vector<std::string> lines;
    static void add(void* ctx, const char* line) { static_cast<Capture*>(ctx)->lines.emplace_back(line); }
    ecp::Sink sink() { ecp::Sink s; s.fn = &Capture::add; s.ctx = this; return s; }
    size_t count(const char* prefix) const {
        size_t n = 0;
        for (const std::string& l : lines) n += l.compare(0, std::strlen(prefix), prefix) == 0 ? 1 : 0;
        return n;
    }
    // The nth line (0-based) that starts with `prefix`, or an empty string.
    std::string nth(const char* prefix, size_t n) const {
        for (const std::string& l : lines)
            if (l.compare(0, std::strlen(prefix), prefix) == 0 && n-- == 0) return l;
        return std::string();
    }
    void clear() { lines.clear(); }
};
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

ecp::Raw makeRaw(uint8_t relative, uint8_t rotLock, uint8_t preset, uint8_t state, float lx, float ly, float lz) {
    ecp::Raw r;
    std::memset(&r, 0, sizeof(r));
    r.relative = relative; r.rotationLock = rotLock; r.presetPending = preset; r.state = state;
    r.local[0] = 1; r.local[5] = 1; r.local[10] = 1; r.local[12] = lx; r.local[13] = ly; r.local[14] = lz;
    r.world[0] = 1; r.world[5] = 1; r.world[10] = 1; r.world[12] = 100.5f; r.world[13] = -20.25f; r.world[14] = 7.125f;
    r.target = 0xAAAA5555CCCC1111ull;
    return r;
}

// The 5376-byte block the head-look branch fingerprinted: floats 536..538 = the translation at 935/939/943, three unit rows at 932.
std::vector<float> makeBlock(float tx, float ty, float tz) {
    std::vector<float> f(ecp::kSceneBlockFloats, 0.0f);
    f[932] = 1; f[933] = 0; f[934] = 0; f[935] = tx;
    f[936] = 0; f[937] = 1; f[938] = 0; f[939] = ty;
    f[940] = 0; f[941] = 0; f[942] = 1; f[943] = tz;
    f[536] = tx; f[537] = ty; f[538] = tz;
    return f;
}

// A census snapshot (camera+0x20 onwards): kind at +0x264, axes at +0x20, origin at +0x50.
std::vector<uint8_t> makeSnap(uint32_t kind, float ox, float oy, float oz, float axisScale = 1.0f) {
    std::vector<uint8_t> s(0x290, 0);
    std::memcpy(s.data() + (ecp::kCamKind - ecp::kCamSnapFrom), &kind, 4);
    const float axes[12] = {axisScale, 0, 0, 0, 0, axisScale, 0, 0, 0, 0, axisScale, 0};
    std::memcpy(s.data() + (ecp::kCamAxes - ecp::kCamSnapFrom), axes, sizeof(axes));
    const float o[3] = {ox, oy, oz};
    std::memcpy(s.data() + (ecp::kCamOrigin - ecp::kCamSnapFrom), o, sizeof(o));
    return s;
}

// ================================ Part A: the pure half ================================
static_assert(ecp::kOffWorldPose + 0x30 == 0xA0 && ecp::kOffLocalPose + 0x30 == 0x3E0,
              "the origin rows are +0xA0 (world) and +0x3E0 (local), as the log lines name them");
static_assert(ecp::kPrologueBytes == 28 && sizeof(ecp::kPrologue) == 28, "the prologue is 28 bytes");

void testIdentity() {
    std::printf("identity\n");
    // The 28 bytes exactly as Phase 0a printed them, spelled out again here (not copied from the header).
    const uint8_t expected[28] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
                                  0x8D, 0xAC, 0x24, 0x40, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00};
    check(std::memcmp(ecp::kPrologue, expected, 28) == 0, "the prologue is 48 89 5C 24 20 55 57 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00");
    check(ecp::kTargetRva == 0x1071980, "the target is EliteDangerous64.exe+0x1071980");
    // The decoder accepts the first instruction with five stolen bytes and no rip-relative displacement.
    size_t disp = 99;
    check(codeInstructionLength(expected, sizeof(expected), &disp) == 5 && disp == 0,
          "CodeHook's decoder measures the prologue's first instruction as 5 bytes with no displacement (5 stolen)");
}

void testDecode() {
    std::printf("snapshot decode\n");
    const ecp::Raw raw = makeRaw(1, 0, 1, 3, 0.25f, 1.5f, -2.125f);
    const ecp::Sample s = ecp::decodeSample(raw, 0xDEAD0000ull, 77, 9, 33);
    check(s.activity == 0xDEAD0000ull && s.threadId == 77 && s.slotCalls == 9 && s.totalCalls == 33, "the sample carries the activity, thread and both call counters");
    check(ecp::flagRelative(s.flags) == 1 && ecp::flagRotationLock(s.flags) == 0 && ecp::flagPreset(s.flags) == 1 && ecp::flagState(s.flags) == 3,
          "the four bytes unpack as +0x470, +0x471, +0x473, +0x48C");
    check(s.target == 0xAAAA5555CCCC1111ull, "the +0x2C8 qword is carried as a raw value");
    check(closeTo(s.local[12], 0.25f) && closeTo(s.local[13], 1.5f) && closeTo(s.local[14], -2.125f), "the local origin is floats 12..14 of the +0x3B0 block (+0x3E0/+0x3E4/+0x3E8)");
    check(closeTo(s.world[12], 100.5f) && closeTo(s.world[13], -20.25f) && closeTo(s.world[14], 7.125f), "the world origin is floats 12..14 of the +0x70 block (+0xA0/+0xA4/+0xA8)");
    check(ecp::packFlags(1, 2, 3, 4) == 0x04030201u, "packFlags puts +0x470 in byte 0 and +0x48C in byte 3");
}

uint32_t flagStateOf(const ecp::Sample& s) { return ecp::flagState(s.flags); }

void testSeqlock() {
    std::printf("seqlock\n");
    ecp::SeqSlot<ecp::Sample> slot;
    ecp::Sample out;
    check(!slot.read(out), "a slot that was never published reads false");
    ecp::Sample a = ecp::decodeSample(makeRaw(0, 0, 0, 3, 1, 2, 3), 1, 2, 3, 4);
    check(slot.tryPublish(a) && slot.read(out) && out.activity == 1 && out.slotCalls == 3 && flagStateOf(out) == 3, "a published sample reads back");
    check(slot.lock() && !slot.lock(), "lock() is a try: a second writer is refused, not queued");
    slot.unlock();
    check(slot.lock(), "...and the slot is free again after unlock");
    slot.unlock();

    // A writer publishing samples whose every word is the same counter against a reader: no torn copy may be returned.
    struct Wide { uint32_t w[48]; };
    ecp::SeqSlot<Wide> wide;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint32_t n = 1;
        while (!stop.load()) {
            Wide v;
            for (uint32_t& x : v.w) x = n;
            wide.tryPublish(v);
            ++n;
        }
    });
    uint64_t good = 0, torn = 0, refused = 0;
    const DWORD until = GetTickCount() + 300;
    while (GetTickCount() < until) {
        Wide got;
        if (wide.read(got, 4)) {
            bool same = true;
            for (uint32_t x : got.w) same = same && x == got.w[0];
            if (same) ++good; else ++torn;
        } else {
            ++refused;
        }
    }
    stop.store(true);
    writer.join();
    check(torn == 0 && good > 1000, "300 ms of a writer against a reader: no torn copy returned, thousands read");
    std::printf("        (%llu consistent reads, %llu refused as in flight)\n", static_cast<unsigned long long>(good), static_cast<unsigned long long>(refused));
}
void testChangeDetection() {
    std::printf("state-change detection\n");
    ecp::ChangeTracker t;
    uint32_t before = 0;
    bool first = false;
    check(t.update(ecp::packFlags(1, 1, 0, 0), &before, &first) && first, "the first observation is reported, as first");
    check(!t.update(ecp::packFlags(1, 1, 0, 0), &before, &first) && !first, "the same four bytes again is no change");
    check(t.update(ecp::packFlags(1, 1, 0, 3), &before, &first) && !first && ecp::flagState(before) == 0 &&
              ecp::changedMask(before, ecp::packFlags(1, 1, 0, 3)) == ecp::kChangedState,
          "+0x48C 0 -> 3 is a change and names only +0x48C");
    check(t.update(ecp::packFlags(0, 1, 0, 3), &before, &first) && ecp::changedMask(before, ecp::packFlags(0, 1, 0, 3)) == ecp::kChangedRelative,
          "+0x470 1 -> 0 is a change and names only +0x470");
    check(t.update(ecp::packFlags(0, 0, 0, 3), &before, &first) && ecp::changedMask(before, ecp::packFlags(0, 0, 0, 3)) == ecp::kChangedRotationLock,
          "+0x471 1 -> 0 is a change and names only +0x471");
    check(t.update(ecp::packFlags(0, 0, 1, 3), &before, &first) && ecp::changedMask(before, ecp::packFlags(0, 0, 1, 3)) == ecp::kChangedPreset,
          "+0x473 0 -> 1 is a change and names only +0x473");
    check(t.update(ecp::packFlags(1, 1, 0, 4), &before, &first) &&
              ecp::changedMask(before, ecp::packFlags(1, 1, 0, 4)) == (ecp::kChangedState | ecp::kChangedRelative | ecp::kChangedRotationLock | ecp::kChangedPreset),
          "several bytes changing in one call are one event naming all of them");

    // Through the hook's own recording function: per activity, in order, with old and new values.
    ecp::Shared sh;
    const ecp::Raw states[] = {makeRaw(1, 1, 0, 0, 0, 0, 0), makeRaw(1, 1, 0, 3, 0, 0, 0), makeRaw(1, 1, 0, 3, 0, 0, 0), makeRaw(1, 1, 0, 4, 0, 0, 0), makeRaw(1, 1, 0, 0, 0, 0, 0)};
    for (const ecp::Raw& r : states) ecp::noteActivityCall(sh, 0x1000, 5, &r);
    ecp::ChangeEvent e;
    int n = 0;
    uint32_t seen[8][2] = {};
    bool firsts[8] = {};
    while (sh.events.take(&e)) { seen[n][0] = ecp::flagState(e.before); seen[n][1] = ecp::flagState(e.after); firsts[n] = e.first != 0; ++n; }
    check(n == 4 && firsts[0] && seen[1][0] == 0 && seen[1][1] == 3 && seen[2][0] == 3 && seen[2][1] == 4 && seen[3][0] == 4 && seen[3][1] == 0,
          "five calls (0, 3, 3, 4, 0) make four events: first, 0>3, 3>4, 4>0 (the repeated 3 is none)");

    // Two activity objects alternating with different states are not a flip-flop.
    ecp::Shared two;
    const ecp::Raw idle = makeRaw(1, 1, 0, 0, 0, 0, 0), live = makeRaw(1, 1, 0, 3, 0, 0, 0);
    for (int i = 0; i < 20; ++i) { ecp::noteActivityCall(two, 0xA000, 5, &idle); ecp::noteActivityCall(two, 0xB000, 5, &live); }
    n = 0;
    while (two.events.take(&e)) ++n;
    check(n == 2, "two activities alternating (state 0 and state 3) give two first-call events and nothing more");
    ecp::Sample s;
    check(two.slots[0].sample.read(s) && s.activity == 0xA000 && ecp::flagState(s.flags) == 0 &&
              two.slots[1].sample.read(s) && s.activity == 0xB000 && ecp::flagState(s.flags) == 3,
          "...each keeps its own slot, so the live one is not overwritten by the idle one");

    // The table, the writer lock and the fault counter.
    ecp::Shared full;
    for (uint64_t k = 1; k <= 9; ++k) ecp::noteActivityCall(full, 0x100 * k, 1, &idle);
    check(full.slotsFull.load() == 1 && full.totalCalls.load() == 9, "a ninth distinct activity is counted (slots_full), not snapshotted");
    ecp::Shared busy;
    ecp::noteActivityCall(busy, 0x2000, 1, &idle);
    busy.slots[0].sample.lock();
    ecp::noteActivityCall(busy, 0x2000, 1, &live);
    busy.slots[0].sample.unlock();
    check(busy.dropped.load() == 1, "a call that finds the slot's writer lock held skips its snapshot and counts it (dropped), never waits");
    ecp::noteActivityCall(busy, 0x2000, 1, nullptr);
    check(busy.faults.load() == 1, "a faulted read is counted (faults)");
}

void testEventRing() {
    std::printf("event ring\n");
    ecp::EventRing<64> ring;
    ecp::ChangeEvent e{};
    for (uint64_t i = 1; i <= 100; ++i) { e.activity = i; e.slotCalls = i * 3; ring.push(e); }
    ecp::ChangeEvent got{};
    uint64_t firstSeen = 0, count = 0, last = 0;
    while (ring.take(&got)) { if (!count) firstSeen = got.activity; ++count; last = got.activity; }
    check(count == 64 && firstSeen == 37 && last == 100 && ring.lost() == 36, "100 events into a 64-slot ring: the newest 64 come out in order and 36 are counted lost");

    // Four writers, one consumer, while it runs: every event delivered is whole.
    ecp::EventRing<64> busy;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (uint64_t w = 1; w <= 4; ++w) {
        writers.emplace_back([&, w] {
            uint64_t n = 0;
            while (!stop.load()) {
                ecp::ChangeEvent v{};
                v.activity = w;
                v.slotCalls = (w << 40) | (++n);
                v.threadId = static_cast<uint32_t>(w);
                v.before = static_cast<uint32_t>(w);
                v.after = static_cast<uint32_t>(w);
                busy.push(v);
            }
        });
    }
    uint64_t delivered = 0, inconsistent = 0;
    const DWORD until = GetTickCount() + 250;
    while (GetTickCount() < until) {
        while (busy.take(&got)) {
            ++delivered;
            const bool whole = got.activity >= 1 && got.activity <= 4 && (got.slotCalls >> 40) == got.activity &&
                               got.threadId == got.activity && got.before == got.activity && got.after == got.activity;
            if (!whole) ++inconsistent;
        }
    }
    stop.store(true);
    for (std::thread& t : writers) t.join();
    check(inconsistent == 0 && delivered > 1000, "four writers against the consumer for 250 ms: no torn event delivered");
    std::printf("        (%llu delivered, %llu counted lost to overwriting)\n", static_cast<unsigned long long>(delivered), static_cast<unsigned long long>(busy.lost()));
}

void testSets() {
    std::printf("distinct sets and tables\n");
    ecp::DistinctSet<8> set;
    for (int i = 0; i < 5; ++i) { set.note(0x10); set.note(0x20); set.note(0x10); }
    uint64_t out[8];
    check(set.drain(out, 8) == 2, "a set notes each value once");
    check(set.drain(out, 8) == 0, "...and a drain empties it");
    for (uint64_t v = 1; v <= 10; ++v) set.note(v);
    check(set.drain(out, 8) == 8 && set.takeOverflow() == 2, "a full set counts what did not fit (10 values into 8 slots: 2)");
    ecp::KeyTable<4> table;
    check(table.findOrInsert(0) == -1 && table.findOrInsert(5) == 0 && table.findOrInsert(6) == 1 && table.findOrInsert(5) == 0, "a key table finds-or-inserts, refusing key 0");
    table.findOrInsert(7); table.findOrInsert(8);
    check(table.findOrInsert(9) == -1 && table.findOrInsert(8) == 3, "...and refuses a fifth key rather than evicting");
}

void testCadence() {
    std::printf("cadence and the 2 s hold\n");
    ecp::Cadence c;
    c.periodMs = 1000;
    check(c.due(0) && !c.due(999) && c.due(1000) && !c.due(1999) && c.due(2000), "a cadence is due at once, then once per period");
    c.reset(5000);
    check(!c.due(5999) && c.due(6000), "reset starts the period from now");
    c.fireNext();
    check(c.due(6001), "fireNext makes it due at the next call");
    check(c.due(60000) && !c.due(60500), "a long stall fires once, not once per missed period");
    ecp::ActiveWindow w;
    check(w.update(0, false) == ecp::ActiveWindow::Idle, "never engaged: idle");
    check(w.update(1000, true) == ecp::ActiveWindow::Active, "state non-zero: active");
    check(w.update(1100, false) == ecp::ActiveWindow::Hold && w.update(3000, false) == ecp::ActiveWindow::Hold, "state back to 0: held for 2 s (inclusive)");
    check(w.update(3001, false) == ecp::ActiveWindow::Idle, "...and idle after");
    check(w.update(4000, true) == ecp::ActiveWindow::Active && w.update(4001, false) == ecp::ActiveWindow::Hold, "a second engagement starts a new hold");
}

void testFingerprint() {
    std::printf("the 5376-byte skinned-object fingerprint\n");
    ecp::SkinHit hit;
    std::vector<float> f = makeBlock(3.0f, -1.0f, 12.0f);
    check(ecp::classifySkinned(f.data(), f.size(), &hit) == ecp::SkinClass::Skinned && closeTo(hit.t[0], 3) && closeTo(hit.t[1], -1) && closeTo(hit.t[2], 12) &&
              closeTo(hit.dist2, 154.0f) && closeTo(hit.m[0], 1) && closeTo(hit.m[4], 1) && closeTo(hit.m[8], 1),
          "the branch's shape is accepted: twin = translation, unit rows, 12.5 m away; translation and 3x3 are captured");
    f = makeBlock(3, -1, 12); f[536] += 0.06f;
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::NotSkinned, "a twin float 0.06 off is refused");
    f = makeBlock(3, -1, 12); f[538] -= 0.049f;
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::Skinned, "...0.049 off is accepted (the limit is 0.05)");
    f = makeBlock(3, -1, 12); f[537] += 0.051f;
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::NotSkinned, "...0.051 off is refused");
    f = makeBlock(3, -1, 12); f[932] = 0.9f; f[933] = 0; f[934] = 0;   // squared length 0.81
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::NotSkinned, "a row of squared length exactly 0.81 is refused (strict)");
    f[932] = 0.901f;
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::Skinned, "...0.812 is accepted");
    f = makeBlock(3, -1, 12); f[941] = 1.1f; f[940] = 0; f[942] = 0;   // squared length 1.21
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::NotSkinned, "a row of squared length 1.21 is refused (strict)");
    f[941] = 1.099f;
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::Skinned, "...1.208 is accepted");
    f = makeBlock(3, -1, 12); f[937] = 0.5f; f[936] = 0; f[938] = 0;   // squared length 0.25
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::NotSkinned, "a short middle row is refused");
    f = makeBlock(49.9f, 0.0f, 0.0f);
    check(ecp::classifySkinned(f.data(), f.size(), &hit) == ecp::SkinClass::Skinned, "49.9 m away is a root");
    f = makeBlock(50.1f, 0.0f, 0.0f);
    check(ecp::classifySkinned(f.data(), f.size(), &hit) == ecp::SkinClass::Far && hit.cls == ecp::SkinClass::Far, "50.1 m away is counted as far, not lost");
    f = makeBlock(0.0f, 0.0f, 0.0f);
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::NotSkinned, "a zero translation is refused");
    f = makeBlock(3, -1, 12); f[935] = std::nanf("");
    check(ecp::classifySkinned(f.data(), f.size(), nullptr) == ecp::SkinClass::NotSkinned, "a NaN translation is refused");
    f = makeBlock(3, -1, 12);
    check(ecp::classifySkinned(f.data(), 943, nullptr) == ecp::SkinClass::NotSkinned && ecp::classifySkinned(nullptr, 1344, nullptr) == ecp::SkinClass::NotSkinned,
          "a block too short to hold float 943, or a null pointer, is refused");
    std::vector<float> zeros(ecp::kSceneBlockFloats, 0.0f);
    check(ecp::classifySkinned(zeros.data(), zeros.size(), nullptr) == ecp::SkinClass::NotSkinned, "an empty block is refused");

    // The tee: counts, distinct resources, the nearest root, and a take that clears.
    ecp::SkinTee tee;
    const std::vector<float> a = makeBlock(3, 0, 4), b = makeBlock(0, 0, 2), c = makeBlock(60, 0, 0), none = zeros;
    tee.note(0x11, a.data(), a.size());
    tee.note(0x22, b.data(), b.size());
    tee.note(0x11, a.data(), a.size());
    tee.note(0x33, c.data(), c.size());
    tee.note(0x44, none.data(), none.size());
    ecp::SkinSecond s = tee.take();
    check(s.seen == 5 && s.hits == 3 && s.beyond == 1 && s.distinct == 2, "five blocks offered: 3 skinned hits on 2 distinct blocks, 1 far, 1 not skinned");
    check(s.haveNearest && s.nearestResource == 0x22 && closeTo(s.nearest.t[2], 2.0f), "the nearest root is the one with the smallest translation");
    s = tee.take();
    check(s.seen == 0 && s.hits == 0 && !s.haveNearest && s.distinct == 0, "a take clears everything");
}

void testYawPitch() {
    std::printf("yaw and pitch decode\n");
    float yaw = 0, pitch = 0;
    ecp::axisYawPitch(0, 0, 1, &yaw, &pitch);
    check(closeTo(yaw, 0) && closeTo(pitch, 0), "straight ahead (+z): yaw 0, pitch 0");
    ecp::axisYawPitch(1, 0, 0, &yaw, &pitch);
    check(closeTo(yaw, 90) && closeTo(pitch, 0), "to the right (+x): yaw 90");
    ecp::axisYawPitch(-1, 0, 0, &yaw, &pitch);
    check(closeTo(yaw, -90), "to the left (-x): yaw -90");
    ecp::axisYawPitch(0, 0, -1, &yaw, &pitch);
    check(closeTo(std::fabs(yaw), 180), "behind (-z): yaw +-180");
    ecp::axisYawPitch(0, 1, 0, &yaw, &pitch);
    check(closeTo(pitch, 90), "straight up (+y): pitch 90");
    ecp::axisYawPitch(0, -1, 0, &yaw, &pitch);
    check(closeTo(pitch, -90), "straight down: pitch -90");
    ecp::axisYawPitch(0, 0.5f, 0.8660254f, &yaw, &pitch);
    check(closeTo(yaw, 0) && closeTo(pitch, 30), "30 degrees up: pitch 30");
    ecp::axisYawPitch(0, 0, 0, &yaw, &pitch);
    check(closeTo(yaw, 0) && closeTo(pitch, 0), "a zero vector decodes to 0, 0 and does not divide");
    ecp::axisYawPitch(0, 2, 0, &yaw, &pitch);
    check(closeTo(pitch, 90), "an unnormalised vector is normalised for the pitch");

    // A rotation of +30 degrees about the view's up axis, row-major, view = M x model: the model's +Z axis is column 2.
    const float c = 0.8660254f, s = 0.5f;
    const float ry[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    ecp::modelAxisYawPitch(ry, 2, &yaw, &pitch);
    check(closeTo(yaw, 30) && closeTo(pitch, 0), "Ry(30): the model's +Z axis is at yaw 30, pitch 0");
    ecp::modelAxisYawPitch(ry, 0, &yaw, &pitch);
    check(closeTo(yaw, 120) && closeTo(pitch, 0), "...and its +X axis at yaw 120 (90 + 30)");
    ecp::modelAxisYawPitch(ry, 1, &yaw, &pitch);
    check(closeTo(pitch, 90), "...and its +Y axis straight up");
    // +20 degrees about the view's x axis tips the model's +Z axis down (view y is up).
    const float cx = 0.9396926f, sx = 0.3420201f;
    const float rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
    ecp::modelAxisYawPitch(rx, 2, &yaw, &pitch);
    check(closeTo(yaw, 0) && closeTo(pitch, -20), "Rx(20): the model's +Z axis is at pitch -20 (the convention the log line states)");
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    ecp::modelAxisYawPitch(identity, 2, &yaw, &pitch);
    check(closeTo(yaw, 0) && closeTo(pitch, 0), "identity: +Z is straight ahead");
}

void testCamTee() {
    std::printf("the camera tally (I1)\n");
    ecp::CamTee tee;
    const std::vector<uint8_t> k5 = makeSnap(5, 1, 2, 3), k3 = makeSnap(3, 10, 20, 30, 2.0f), k0 = makeSnap(0, 0, 0, 0);
    tee.note(7, 0x594FE1, 0xC5, k5.data());
    tee.note(7, 0x594FE1, 0xC6, k5.data());
    tee.note(8, 0x594FE1, 0xC5, k5.data());
    tee.note(8, 0x5A0000, 0xC3, k3.data());
    tee.note(8, 0x5A0000, 0xC3, k3.data());
    tee.note(8, 0x5A0000, 0xC3, k3.data());
    tee.note(8, 0x594FE1, 0xC0, k0.data());   // the same call site, a different kind: its own row
    ecp::CamSecond s = tee.take();
    check(s.used == 3 && s.calls == 7 && s.overflow == 0, "three (kind, site) rows, seven calls");
    const ecp::CamRow* r5 = nullptr; const ecp::CamRow* r3 = nullptr; const ecp::CamRow* r0 = nullptr;
    for (uint32_t i = 0; i < s.used; ++i) {
        if (s.rows[i].kind == 5) r5 = &s.rows[i];
        if (s.rows[i].kind == 3) r3 = &s.rows[i];
        if (s.rows[i].kind == 0) r0 = &s.rows[i];
    }
    check(r5 && r5->site == 0x594FE1 && r5->calls == 3 && r5->frames == 2 && r5->cameras == 2 && closeTo(r5->origin[0], 1) && closeTo(r5->origin[2], 3) && closeTo(r5->axes[0], 1),
          "kind 5 at +0x594FE1: 3 calls over 2 frames, 2 camera objects, origin (1,2,3) and its axes read from the snapshot");
    check(r3 && r3->site == 0x5A0000 && r3->calls == 3 && r3->frames == 1 && r3->cameras == 1 && closeTo(r3->origin[1], 20) && closeTo(r3->axes[0], 2) && closeTo(r3->axes[5], 2),
          "kind 3 at +0x5A0000: 3 calls in one frame (3 per frame), origin (10,20,30), axes scaled by 2");
    check(r0 && r0->calls == 1, "the same site with another kind is its own row");
    check(s.kindCalls[5] == 3 && s.kindCalls[3] == 3 && s.kindCalls[0] == 1, "calls are tallied per kind too");
    check(tee.take().calls == 0, "a take clears the tally");
    for (uint32_t site = 0; site < 20; ++site) tee.note(1, 0x1000 + site, 0xC0 + site, k5.data());
    s = tee.take();
    check(s.used == ecp::kCamRows && s.overflow == 4 && s.calls == 20, "a 17th..20th distinct (kind, site) is counted as overflow, not lost silently");
    const std::vector<uint8_t> wild = makeSnap(4000, 0, 0, 0);
    tee.note(1, 1, 1, wild.data());
    check(tee.take().kindOverflow == 1, "a kind beyond the table is counted too");
}

void testText() {
    std::printf("log lines\n");
    char line[ecp::kLineBytes];
    ecp::ChangeEvent e{};
    e.activity = 0x1234ABCD; e.slotCalls = 42; e.threadId = 9; e.before = ecp::packFlags(1, 1, 0, 0); e.after = ecp::packFlags(1, 1, 0, 3); e.first = 0;
    ecp::formatChange(line, sizeof(line), e, 1234);
    check(std::strncmp(line, "explorer cam probe I3 change:", 29) == 0 && has(line, "+0x48C 0->3") && has(line, "+0x470 1->1") && has(line, "+0x471 1->1") &&
              has(line, "+0x473 0->0") && has(line, "changed=48C") && has(line, "act=0x1234ABCD") && has(line, "frame=1234"),
          "a change line names the old and new value of all four bytes, which changed, the activity and the frame");
    e.after = ecp::packFlags(0, 1, 1, 3);
    ecp::formatChange(line, sizeof(line), e, 1);
    check(has(line, "changed=48C,470,473"), "several changed bytes are listed in order");
    e.first = 1; e.before = e.after;
    ecp::formatChange(line, sizeof(line), e, 1);
    check(has(line, "first-call") && has(line, "changed=-"), "a first observation says so");

    const ecp::Raw raw = makeRaw(1, 0, 0, 3, 0.25f, 1.5f, -2.125f);
    ecp::Sample s = ecp::decodeSample(raw, 0xFEED, 4, 100, 100);
    ecp::formatPose(line, sizeof(line), s, ecp::ActiveWindow::Active, 17);
    check(std::strncmp(line, "explorer cam probe I3 pose:", 27) == 0 && has(line, "local_origin(+0x3E0)=(0.250,1.500,-2.125)") && has(line, "world_origin(+0xA0)=(100.500,-20.250,7.125)") &&
              has(line, "local_basis(+0x3B0 rows)=[(1.000,0.000,0.000)(0.000,1.000,0.000)(0.000,0.000,1.000)]") && has(line, "phase=active") && has(line, "state=3"),
          "the pose line prints the local origin, the local basis rows and the world origin to 3 decimals");

    ecp::CamRow row;
    row.used = true; row.kind = 5; row.site = 0x594FE1; row.calls = 6; row.frames = 3; row.cameras = 2;
    row.origin[0] = 1; row.origin[1] = 2; row.origin[2] = 3; row.axes[0] = 1; row.axes[5] = 1; row.axes[10] = 1;
    ecp::formatCamRow(line, sizeof(line), row, ecp::ActiveWindow::Hold);
    check(std::strncmp(line, "explorer cam probe I1 cam:", 26) == 0 && has(line, "kind=5") && has(line, "site=+0x594FE1") && has(line, "calls_per_frame=2.00") && has(line, "origin(+0x50)=(1.000,2.000,3.000)") &&
              has(line, "axes(+0x20 rows)=[(1.000,0.000,0.000,0.000)") && has(line, "phase=hold"),
          "the camera line prints kind, site, calls per frame, origin and the axes rows");

    ecp::SkinSecond root;
    root.seen = 40; root.hits = 3; root.beyond = 1; root.distinct = 2; root.haveNearest = true;
    root.nearestResource = 0xABC;
    root.nearest.t[0] = 0.5f; root.nearest.t[1] = -1.7f; root.nearest.t[2] = 2.0f; root.nearest.dist2 = 0.25f + 2.89f + 4.0f;
    const float c = 0.8660254f, sn = 0.5f;
    const float m[9] = {c, 0, sn, 0, 1, 0, -sn, 0, c};
    std::memcpy(root.nearest.m, m, sizeof(m));
    ecp::formatRoot(line, sizeof(line), root, ecp::ActiveWindow::Active);
    check(std::strncmp(line, "explorer cam probe I2 root:", 27) == 0 && has(line, "skinned_blocks=2") && has(line, "nearest_view_m=(0.500,-1.700,2.000)") && has(line, "dist_m=2.672") &&
              has(line, "+Z yaw=30.0 pitch=0.0") && has(line, "convention:") && has(line, "yaw = atan2(x,z)"),
          "the root line prints the count, the nearest root in view space, the decoded yaw and pitch and the convention");
    root.haveNearest = false;
    ecp::formatRoot(line, sizeof(line), root, ecp::ActiveWindow::Active);
    check(has(line, "nearest=none"), "with no root in the second it says so");

    // The worst case still fits one log line.
    ecp::Sample big = ecp::decodeSample(makeRaw(1, 1, 1, 6, -99999.999f, 99999.999f, -99999.999f), 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFF, 0xFFFFFFFFFFFFull, 0xFFFFFFFFFFFFull);
    for (float& v : big.world) v = -99999.999f;
    for (float& v : big.local) v = -99999.999f;
    ecp::formatPose(line, sizeof(line), big, ecp::ActiveWindow::Hold, 99999999);
    check(std::strlen(line) < ecp::kLineBytes - 1 && has(line, "world_basis"), "the pose line with six-digit values still fits one log line whole");
}

// ---- the consumer against a scripted clock ------------------------------------------------------------------------------------
void testConsumerIdle() {
    std::printf("consumer: every heartbeat when its instrument is alive but idle\n");
    ecp::Shared sh;
    ecp::SkinTee skin;
    ecp::CamTee cam;
    ecp::Consumer c;
    Capture cap;
    ecp::TickIn in;
    in.i3Armed = true; in.i3Status = "armed"; in.censusWanted = true; in.injectStatus = "installed"; in.i2Armed = true;
    c.start(0);
    in.nowMs = 1000; c.tick(sh, skin, cam, in, cap.sink());
    check(cap.lines.empty(), "nothing prints before the first 5 s is up");
    in.nowMs = 5000; c.tick(sh, skin, cam, in, cap.sink());
    check(cap.lines.size() == 3 && cap.count("explorer cam probe I3 heartbeat:") == 1 && cap.count("explorer cam probe I1 heartbeat:") == 1 && cap.count("explorer cam probe I2 heartbeat:") == 1,
          "at 5 s: exactly one heartbeat per instrument, in the order I3, I1, I2, and no detail line");
    check(cap.lines.size() == 3 && std::strncmp(cap.lines[0].c_str(), "explorer cam probe I3", 21) == 0 && std::strncmp(cap.lines[1].c_str(), "explorer cam probe I1", 21) == 0 &&
              std::strncmp(cap.lines[2].c_str(), "explorer cam probe I2", 21) == 0, "...in that order");
    const std::string i3 = cap.nth("explorer cam probe I3 heartbeat:", 0), i1 = cap.nth("explorer cam probe I1 heartbeat:", 0), i2 = cap.nth("explorer cam probe I2 heartbeat:", 0);
    check(has(i3, "hook=armed") && has(i3, "calls=0") && has(i3, "total_calls=0") && has(i3, "activities=0") && has(i3, "threads=0") && has(i3, "idle=no-call-yet") && has(i3, "window=5.0s"),
          "I3 idle: calls=0 total_calls=0 activities=0 threads=0 idle=no-call-yet");
    check(has(i1, "census=running") && has(i1, "inject_hook=installed") && has(i1, "census_calls=0") && has(i1, "idle=no-refresh-call-reached-the-probe") && has(i1, "kinds=[]"),
          "I1 idle: census=running census_calls=0 kinds=[] idle=no-refresh-call-reached-the-probe");
    check(has(i2, "tee=armed") && has(i2, "blocks_offered=0") && has(i2, "skinned=0") && has(i2, "idle=no-5376-byte-block-offered"),
          "I2 idle: tee=armed blocks_offered=0 skinned=0 idle=no-5376-byte-block-offered");

    // The hook stood down: the heartbeat says why nothing can be seen.
    cap.clear();
    ecp::Consumer down;
    in.i3Armed = false; in.i3Status = "stood down";
    down.start(0);
    in.nowMs = 5000; down.tick(sh, skin, cam, in, cap.sink());
    check(has(cap.nth("explorer cam probe I3 heartbeat:", 0), "hook=stood down") && has(cap.nth("explorer cam probe I3 heartbeat:", 0), "the hook is stood down") &&
              has(cap.nth("explorer cam probe I1 heartbeat:", 0), "gate=I3 down"),
          "with I3 stood down the heartbeats still print and say that the detail lines are gated on it");
}

struct Script {
    ecp::Shared sh;
    ecp::SkinTee skin;
    ecp::CamTee cam;
    ecp::Consumer c;
    Capture cap;
    ecp::TickIn in;
    uint64_t now = 0;
    Script() {
        in.i3Armed = true; in.i3Status = "armed"; in.censusWanted = true; in.injectStatus = "installed"; in.i2Armed = true;
    }
    void tick() { in.nowMs = now; in.frame = static_cast<uint32_t>(now / 11); c.tick(sh, skin, cam, in, cap.sink()); }
};

void testConsumerEngaged() {
    std::printf("consumer: change lines at once, detail lines at 1 Hz while +0x48C != 0 and for 2 s after\n");
    Script t;
    t.c.start(0);
    const std::vector<uint8_t> k5 = makeSnap(5, 1, 2, 3), k3 = makeSnap(3, 10, 20, 30);
    const std::vector<float> blk = makeBlock(0.5f, -1.7f, 2.0f);
    // Idle until 10 s, the activity alive in state 0 (one call per 100 ms tick).
    for (t.now = 100; t.now < 10000; t.now += 100) {
        const ecp::Raw r = makeRaw(1, 1, 0, 0, 0.1f, 1.5f, 0.2f);
        ecp::noteActivityCall(t.sh, 0xA11CE, 1234, &r);
        t.tick();
    }
    check(t.cap.count("explorer cam probe I3 change:") == 1 && has(t.cap.nth("explorer cam probe I3 change:", 0), "first-call"), "9.9 s of state 0: one line, the first call");
    check(t.cap.count("explorer cam probe I3 pose:") == 0 && t.cap.count("explorer cam probe I2 root:") == 0 && t.cap.count("explorer cam probe I1 cam:") == 0, "...and no detail line");
    check(t.cap.count("explorer cam probe I3 heartbeat:") == 1, "...and one I3 heartbeat (at 5 s)");
    const std::string beat = t.cap.nth("explorer cam probe I3 heartbeat:", 0);
    check(has(beat, "calls=50") && has(beat, "activities=1") && has(beat, "activity_ptrs=[0xA11CE]") && has(beat, "threads=1") && has(beat, "thread_ids=[1234]") &&
              has(beat, "state: act=0xA11CE +0x48C=0 +0x470=1 +0x471=1 +0x473=0") && has(beat, "phase=idle"),
          "the heartbeat counts the window's calls, the distinct activity pointers and thread ids, and shows the current +0x48C");
    t.cap.clear();

    // At 10.0 s the state flips to 3. Ticks every 100 ms; the camera and the scene blocks are fed every tick.
    size_t poseLines = 0;
    std::vector<std::string> poseTimes;
    for (t.now = 10000; t.now <= 15000; t.now += 100) {
        const uint8_t state = t.now >= 10000 && t.now <= 11100 ? 3 : 0;   // the free camera is up for 1.1 s, then off
        const ecp::Raw r = makeRaw(1, 1, 0, state, 0.1f, 1.5f, 0.2f);
        ecp::noteActivityCall(t.sh, 0xA11CE, 1234, &r);
        t.cam.note(t.now / 11, 0x594FE1, 0xC5, k5.data());
        t.cam.note(t.now / 11, 0x5A0000, 0xC3, k3.data());
        t.skin.note(0x77, blk.data(), blk.size());
        t.tick();
        const size_t now = t.cap.count("explorer cam probe I3 pose:");
        if (now != poseLines) { poseTimes.push_back(std::to_string(t.now) + ":" + (has(t.cap.nth("explorer cam probe I3 pose:", now - 1), "phase=active") ? "active" : "hold")); poseLines = now; }
    }
    check(t.cap.count("explorer cam probe I3 change:") == 2, "the two flips (0>3 at 10.0 s, 3>0 at 11.2 s) are two change lines");
    const std::string up = t.cap.nth("explorer cam probe I3 change:", 0), down = t.cap.nth("explorer cam probe I3 change:", 1);
    check(has(up, "+0x48C 0->3") && has(up, "changed=48C") && has(down, "+0x48C 3->0") && has(down, "changed=48C"), "...with old and new values");
    // The change line comes out in the same tick as the flip: it precedes the first pose line.
    size_t changeAt = 0, poseAt = 0;
    for (size_t i = 0; i < t.cap.lines.size(); ++i) {
        if (!changeAt && t.cap.lines[i].compare(0, 29, "explorer cam probe I3 change:") == 0) changeAt = i + 1;
        if (!poseAt && t.cap.lines[i].compare(0, 27, "explorer cam probe I3 pose:") == 0) poseAt = i + 1;
    }
    check(changeAt && poseAt && changeAt < poseAt, "the change line precedes the first detail line");
    check(poseTimes.size() == 4 && poseTimes[0] == "10000:active" && poseTimes[1] == "11000:active" && poseTimes[2] == "12000:hold" && poseTimes[3] == "13000:hold",
          "pose lines at 1 Hz: the moment it engages, 1 s later (active), then two held seconds (hold), and none after 13.0 s");
    if (poseTimes.size() != 4) { for (const std::string& s : poseTimes) std::printf("        pose at %s\n", s.c_str()); }
    check(t.cap.count("explorer cam probe I1 cam:") == 8 && t.cap.count("explorer cam probe I2 root:") == 4, "I1 prints one line per (kind, site) (2) and I2 one line, on the same four seconds");
    const std::string cam0 = t.cap.nth("explorer cam probe I1 cam:", 0);
    check(has(cam0, "kind=5") && has(cam0, "site=+0x594FE1") && has(cam0, "origin(+0x50)=(1.000,2.000,3.000)") && has(cam0, "calls_per_frame=") , "an I1 line carries kind, site, calls per frame and the origin");
    const std::string root0 = t.cap.nth("explorer cam probe I2 root:", 0);
    check(has(root0, "skinned_blocks=1") && has(root0, "nearest_view_m=(0.500,-1.700,2.000)"), "an I2 line carries the block count and the nearest root's view-space translation");
    const std::string pose0 = t.cap.nth("explorer cam probe I3 pose:", 0);
    check(has(pose0, "local_origin(+0x3E0)=(0.100,1.500,0.200)") && has(pose0, "state=3"), "an I3 line carries the local origin and the state byte");
    check(t.cap.count("explorer cam probe I3 heartbeat:") == 2 && t.cap.count("explorer cam probe I1 heartbeat:") == 2 && t.cap.count("explorer cam probe I2 heartbeat:") == 2,
          "heartbeats at 10 s and 15 s for all three instruments");
    const std::string beat2 = t.cap.nth("explorer cam probe I2 heartbeat:", 0);
    check(has(beat2, "blocks_offered=") && !has(beat2, "idle="), "an I2 heartbeat with blocks offered and skinned hits carries no idle tag");
}

void testConsumerStale() {
    std::printf("consumer: an activity that stops being called is not treated as engaged\n");
    Script t;
    t.c.start(0);
    for (t.now = 100; t.now <= 2000; t.now += 100) {
        const ecp::Raw r = makeRaw(1, 1, 0, 3, 0, 0, 0);
        ecp::noteActivityCall(t.sh, 0xBEE, 1, &r);
        t.tick();
    }
    const size_t poseBefore = t.cap.count("explorer cam probe I3 pose:");
    // The activity is destroyed with its last state still 3: no more calls.
    for (t.now = 2100; t.now <= 10000; t.now += 100) t.tick();
    check(t.cap.count("explorer cam probe I3 stale:") == 1 && has(t.cap.nth("explorer cam probe I3 stale:", 0), "stopped being called"), "one stale line when its calls stop with +0x48C != 0");
    const size_t poseAfter = t.cap.count("explorer cam probe I3 pose:");
    check(poseAfter >= poseBefore && poseAfter <= poseBefore + 4, "...and the 1 Hz lines end after the 1.5 s freshness and the 2 s hold, not at the end of the session");
    check(has(t.cap.nth("explorer cam probe I3 heartbeat:", 1), "idle=no-call-in-window"), "the next heartbeat says no call in the window");
}

// ================================ Part B: the glue, end to end ================================
// A function that begins with the real prologue: rcx = the activity. Its body writes +0x48C = 3 and +0x3E0 = 1.0f and returns
// 0x1122334455667788, then restores what the prologue saved. Anything the hook does not forward or preserve breaks this call.
size_t buildSynthetic(uint8_t* code, bool corrupt) {
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) code[n++] = b; };
    for (uint8_t b : ecp::kPrologue) code[n++] = b;
    put({0xC6, 0x81, 0x8C, 0x04, 0x00, 0x00, 0x03});                      // mov byte ptr [rcx+48Ch], 3
    put({0xC7, 0x81, 0xE0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F});    // mov dword ptr [rcx+3E0h], 1.0f
    put({0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11});    // mov rax, 1122334455667788h
    put({0x48, 0x81, 0xC4, 0xC0, 0x03, 0x00, 0x00});                      // add rsp, 3C0h
    put({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x5F, 0x5D});                // pop r15; pop r14; pop r13; pop rdi; pop rbp
    put({0x48, 0x8B, 0x5C, 0x24, 0x20});                                  // mov rbx, [rsp+20h]
    put({0xC3});                                                          // ret
    if (corrupt) code[14] ^= 0x01;   // the byte of `lea rbp,[rsp-2C0h]`: no longer build 332841's prologue
    return n;
}
uint8_t* makeExecutable(const uint8_t* code, size_t n) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return nullptr;
    std::memcpy(page, code, n);
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, n);
    return page;
}
using ActivityFn = uint64_t (__fastcall*)(void*);

void testGlue() {
    std::printf("glue, end to end (a synthetic FreeCameraActivity update)\n");
    namespace t = edvr::explorercamprobetest;
    uint8_t good[128] = {}, bad[128] = {};
    const size_t nGood = buildSynthetic(good, false);
    buildSynthetic(bad, true);
    uint8_t* goodPage = makeExecutable(good, nGood);
    uint8_t* badPage = makeExecutable(bad, nGood);
    check(goodPage && badPage, "two executable pages for the synthetic functions");
    if (!goodPage || !badPage) return;

    alignas(16) static uint8_t activity[0x600];
    std::memset(activity, 0, sizeof(activity));
    const float oy = 2.0f, oz = 3.0f, wx = 55.5f;
    std::memcpy(activity + 0x3E4, &oy, 4);
    std::memcpy(activity + 0x3E8, &oz, 4);
    std::memcpy(activity + 0xA0, &wx, 4);
    activity[0x470] = 1; activity[0x471] = 1; activity[0x473] = 0;
    const uint64_t rawTarget = 0x00000123456789A0ull;
    std::memcpy(activity + 0x2C8, &rawTarget, 8);   // a pointer-looking value that points nowhere: it must never be dereferenced
    auto callGood = reinterpret_cast<ActivityFn>(goodPage);
    check(callGood(activity) == 0x1122334455667788ull && activity[0x48C] == 3, "baseline: the synthetic function works unhooked");
    activity[0x48C] = 0;
    const float zero = 0.0f;
    std::memcpy(activity + 0x3E0, &zero, 4);

    // ---- key off from the start: nothing is installed ------------------------------------------------------------------------
    Capture cap;
    t::setTarget(reinterpret_cast<uintptr_t>(goodPage));
    uint8_t before[40];
    std::memcpy(before, goodPage, sizeof(before));
    t::boundary(1, 1000, false, &Capture::add, &cap);
    t::boundary(2, 1016, false, &Capture::add, &cap);
    t::boundary(3, 7000, false, &Capture::add, &cap);
    check(cap.lines.size() == 1 && std::strncmp(cap.lines[0].c_str(), "explorer cam probe: off", 23) == 0, "KEY OFF: three boundaries print one 'off' line");
    check(std::memcmp(before, goodPage, sizeof(before)) == 0 && !t::gateOpen() && !explorerCamProbeWantsSceneBlocks() && detail::g_vrCensusCameraNote == nullptr &&
              t::stolenBytes() == 0,
          "KEY OFF: no hook is installed (the function's bytes are untouched), the gate is closed, the scene tee is off, the census pointer is null");

    // ---- a wrong prologue: one line, no patch --------------------------------------------------------------------------------
    cap.clear();
    uint8_t badBefore[40];
    std::memcpy(badBefore, badPage, sizeof(badBefore));
    t::setTarget(reinterpret_cast<uintptr_t>(badPage));
    t::boundary(4, 8000, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 stood down:") == 1 && has(cap.nth("explorer cam probe I3 stood down:", 0), "the game build differs") &&
              has(cap.nth("explorer cam probe I3 stood down:", 0), "nothing was patched"),
          "WRONG PROLOGUE: one 'stood down' line saying the game build differs");
    check(std::memcmp(badBefore, badPage, sizeof(badBefore)) == 0 && !t::gateOpen(), "...and not one byte of the function was written, the gate stays closed");
    check(cap.count("explorer cam probe I3 armed:") == 0 && cap.count("explorer cam probe I1 armed:") == 1 && cap.count("explorer cam probe I2 armed:") == 1 && cap.count("explorer cam probe: on") == 1,
          "...while I1 and I2 still announce themselves");
    cap.clear();
    t::boundary(5, 14000, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 heartbeat:") >= 1 && has(cap.nth("explorer cam probe I3 heartbeat:", 0), "hook=stood down"), "...and the I3 heartbeat says the hook is stood down");
    t::boundary(6, 14100, false, &Capture::add, &cap);
    t::reset();

    // ---- the real path ---------------------------------------------------------------------------------------------------------
    cap.clear();
    t::setTarget(reinterpret_cast<uintptr_t>(goodPage));
    t::boundary(10, 20000, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 armed:") == 1 && has(cap.nth("explorer cam probe I3 armed:", 0), "stolen=5 bytes") && has(cap.nth("explorer cam probe I3 armed:", 0), "28/28 bytes verified"),
          "ARMED: one line, 5 stolen bytes, 28/28 prologue bytes verified");
    check(cap.count("explorer cam probe I1 armed:") == 1 && cap.count("explorer cam probe I2 armed:") == 1 && cap.count("explorer cam probe: on") == 1, "...and the I1, I2 and key-on lines");
    check(t::stolenBytes() == 5 && t::gateOpen() && explorerCamProbeWantsSceneBlocks() && detail::g_vrCensusCameraNote != nullptr, "the hook stole 5 bytes; the gate, the scene tee and the census pointer are open");
    check(goodPage[0] == 0xE9, "the function now begins with E9 (the relay jump)");

    const uint64_t totalBefore = t::shared().totalCalls.load();
    const uint64_t r1 = callGood(activity);
    check(r1 == 0x1122334455667788ull, "the hooked function returns the original's value unchanged");
    check(t::shared().totalCalls.load() == totalBefore + 1, "...and the call was counted");
    cap.clear();
    t::boundary(11, 20100, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 change:") == 1 && has(cap.nth("explorer cam probe I3 change:", 0), "+0x48C 3->3"),
          "THE ORIGINAL RUNS FIRST: the very first snapshot already sees +0x48C = 3, which only the original's body wrote (a snapshot before it would read 0)");
    check(cap.count("explorer cam probe I3 pose:") == 1 && has(cap.nth("explorer cam probe I3 pose:", 0), "state=3") && has(cap.nth("explorer cam probe I3 pose:", 0), "local_origin(+0x3E0)=(1.000,2.000,3.000)") &&
              has(cap.nth("explorer cam probe I3 pose:", 0), "world_origin(+0xA0)=(55.500,0.000,0.000)") && has(cap.nth("explorer cam probe I3 pose:", 0), "target=0x123456789A0"),
          "the pose line shows +0x3E0 written by the original, +0x3E4/+0x3E8 and +0xA0 as the activity held them, and the raw +0x2C8 qword");
    check(has(cap.nth("explorer cam probe I3 pose:", 0), "act=0x") && has(cap.nth("explorer cam probe I3 pose:", 0), "thread="), "...with the activity pointer and thread id");
    uint8_t unchanged[0x600];
    std::memcpy(unchanged, activity, sizeof(unchanged));

    // A call from a second thread, the activity unchanged.
    uint64_t r2 = 0;
    std::thread other([&] { r2 = callGood(activity); });
    other.join();
    check(r2 == 0x1122334455667788ull, "a call from a worker thread returns the original's value too");
    for (int i = 0; i < 8; ++i) callGood(activity);
    cap.clear();
    t::boundary(12, 25100, true, &Capture::add, &cap);
    const std::string beat = cap.nth("explorer cam probe I3 heartbeat:", 0);
    check(has(beat, "hook=armed") && has(beat, "calls=10") && has(beat, "activities=1") && has(beat, "threads=2") && has(beat, "+0x48C=3"),
          "HEARTBEAT: 10 calls in the window, one activity pointer, two thread ids (the render thread and the worker), +0x48C = 3");
    check(has(cap.nth("explorer cam probe I1 heartbeat:", 0), "census_calls=0") && has(cap.nth("explorer cam probe I2 heartbeat:", 0), "blocks_offered=0"),
          "...and I1 and I2 print their own heartbeats with zero counts");

    // I1 and I2 through their real feeds.
    const std::vector<uint8_t> snap = makeSnap(5, 4, 5, 6);
    detail::g_vrCensusCameraNote(777, 0xC5, 0x594FE1, snap.data());
    detail::g_vrCensusCameraNote(777, 0xC5, 0x594FE1, snap.data());
    const std::vector<float> blk = makeBlock(1.0f, 0.0f, 3.0f);
    explorerCamProbeNoteSceneBlock(reinterpret_cast<const void*>(0x5150), blk.data(), 5376);
    explorerCamProbeNoteSceneBlock(reinterpret_cast<const void*>(0x5150), blk.data(), 100);   // too short: refused
    cap.clear();
    t::boundary(13, 26200, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I1 cam:") == 1 && has(cap.nth("explorer cam probe I1 cam:", 0), "kind=5") && has(cap.nth("explorer cam probe I1 cam:", 0), "calls=2") &&
              has(cap.nth("explorer cam probe I1 cam:", 0), "origin(+0x50)=(4.000,5.000,6.000)"),
          "I1: a census call fed through the hook pointer comes out as a per-kind line with the origin");
    check(cap.count("explorer cam probe I2 root:") == 1 && has(cap.nth("explorer cam probe I2 root:", 0), "skinned_blocks=1") && has(cap.nth("explorer cam probe I2 root:", 0), "nearest_view_m=(1.000,0.000,3.000)") &&
              has(cap.nth("explorer cam probe I2 root:", 0), "offered=1"),
          "I2: a scene block fed through the tee comes out as a root line (the too-short block was refused)");

    // A guarded read of a bad activity pointer: counted, no crash.
    const uint32_t faultsBefore = t::shared().faults.load();
    t::observe(reinterpret_cast<void*>(0x10));
    check(t::shared().faults.load() == faultsBefore + 1, "a faulting activity pointer is a counted fault, not a crash (the SEH guard)");

    // The game's memory was only read.
    check(std::memcmp(unchanged, activity, sizeof(unchanged)) == 0, "NOTHING WRITTEN: the activity's memory is byte for byte what the original left");

    // The key goes off while running.
    cap.clear();
    const uint64_t callsAtOff = t::shared().totalCalls.load();
    t::boundary(14, 30000, false, &Capture::add, &cap);
    check(cap.count("explorer cam probe: off") == 1 && !t::gateOpen() && !explorerCamProbeWantsSceneBlocks() && detail::g_vrCensusCameraNote == nullptr,
          "KEY OFF while running: one line, the gate closes, the tee and the census pointer detach");
    check(callGood(activity) == 0x1122334455667788ull && t::shared().totalCalls.load() == callsAtOff, "...the function still returns the original's value and the callback no longer runs (no call counted)");
    cap.clear();
    t::boundary(15, 31000, true, &Capture::add, &cap);
    check(t::gateOpen() && cap.count("explorer cam probe: on again") == 1 && cap.count("explorer cam probe I3 armed:") == 0, "turned back on: the gate reopens with a short line, the hook is not installed twice");
    t::boundary(16, 31100, false, &Capture::add, &cap);

    // Restore: the hook removes itself, the bytes come back.
    t::reset();
    uint8_t after[5];
    std::memcpy(after, goodPage, 5);
    check(std::memcmp(after, ecp::kPrologue, 5) == 0, "uninstall puts the original five bytes back");
}


// ================================ Part C: the F2 instruments ================================
// I3 pressed (the activity's three action ints), I4 (the camera controller) and N (the commander's eye: a vtable slot swap whose
// replacement notes its callers' return addresses). Pure math first, then the glue on a synthetic vtable.

void testNeckMath() {
    std::printf("the neck: the commander's frame and the eye in commander-local axes\n");
    // A commander facing +x at world (10, 0, 20): F's rows are right (0,0,-1), up (0,1,0), forward (1,0,0).
    const float F[9] = {0, 0, -1, 0, 1, 0, 1, 0, 0};
    const float root[3] = {10, 0, 20};
    // The free camera under the relative lock: local L (a rotation, origin = the eye placement), world W = L x F, origin = lo x F + root.
    auto build = [&](const float L[9], const float lo[3], float local[16], float world[16]) {
        std::memset(local, 0, 64);
        std::memset(world, 0, 64);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) local[r * 4 + c] = L[r * 3 + c];
        for (int c = 0; c < 3; ++c) local[12 + c] = lo[c];
        local[15] = 1;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                float sum = 0;
                for (int k = 0; k < 3; ++k) sum += L[r * 3 + k] * F[k * 3 + c];   // (L x F)[r][c]
                world[r * 4 + c] = sum;
            }
        for (int c = 0; c < 3; ++c) {
            float sum = root[c];
            for (int k = 0; k < 3; ++k) sum += lo[k] * F[k * 3 + c];
            world[12 + c] = sum;
        }
        world[15] = 1;
    };
    const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float back[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};   // the selfie preset's facing
    const float lo[3] = {0.0f, 1.68f, 0.10f};
    const float eyeLocal[3] = {0.02f, 1.55f, 0.12f};
    // The eye in world space: e x F + root.
    float eyeWorld[3];
    for (int c = 0; c < 3; ++c) {
        float sum = root[c];
        for (int k = 0; k < 3; ++k) sum += eyeLocal[k] * F[k * 3 + c];
        eyeWorld[c] = sum;
    }
    check(closeTo(eyeWorld[0], 10.12f) && closeTo(eyeWorld[1], 1.55f) && closeTo(eyeWorld[2], 19.98f), "(the known frame puts the local eye (0.02, 1.55, 0.12) at world (10.12, 1.55, 19.98))");
    for (int variant = 0; variant < 2; ++variant) {
        float local[16], world[16];
        build(variant == 0 ? ident : back, lo, local, world);
        const f2::CommanderFrame c = f2::commanderFrame(local, world);
        bool frameOk = c.valid;
        for (int i = 0; i < 9; ++i) frameOk = frameOk && closeTo(c.f[i], F[i]);
        check(frameOk, variant == 0 ? "F = L^T x W recovers the commander's frame (identity local rotation)" : "F = L^T x W recovers the commander's frame (local rotation facing back)");
        check(closeTo(c.root[0], 10.0f) && closeTo(c.root[1], 0.0f) && closeTo(c.root[2], 20.0f), "...and the root = W.origin - L.origin x F is (10, 0, 20)");
        float out[3];
        f2::worldToCommanderLocal(c, eyeWorld, out);
        check(closeTo(out[0], 0.02f) && closeTo(out[1], 1.55f) && closeTo(out[2], 0.12f), "...the eye comes back in commander-local axes as (right 0.02, up 1.55, forward 0.12): the stance test's figure");
    }
    float local[16], world[16];
    build(ident, lo, local, world);
    local[0] = 5.0f;   // not a rotation
    check(!f2::commanderFrame(local, world).valid, "rows that are not near unit length give no frame (no figure rather than a wrong one)");
    local[0] = std::nanf("");
    check(!f2::commanderFrame(local, world).valid, "...and neither does a NaN");
}

void testF2Core() {
    std::printf("the neck: identity, the slot and the return-address filter\n");
    const uint8_t code[8] = {0x48, 0x8D, 0x81, 0x68, 0x02, 0x00, 0x00, 0xC3};
    check(std::memcmp(f2::kEyeGetterCode, code, 8) == 0 && f2::neckCodeOk(code), "the accessor is `lea rax,[rcx+268h]; ret` (48 8D 81 68 02 00 00 C3)");
    uint8_t other[8];
    std::memcpy(other, code, 8);
    other[3] ^= 1;
    check(!f2::neckCodeOk(other), "...one byte different is not it");
    check(f2::kEyeVtableRva == 0x51FCE98 && f2::kEyeGetterSlot == 4 && f2::kEyeGetterRva == 0xF88330 && f2::kLocalEyeSiteRva == 0x1073946 && f2::kOffEyeMatrix == 0x268 &&
              f2::kOffEyeB == 0x1A8 && f2::kOffEyeC == 0x1E8 && f2::kOffEyeD == 0x228 && f2::kOffEyeVec == 0x2A8,
          "the vtable is +0x51FCE98, slot 4 holds +0xF88330, the local site returns to +0x1073946, the matrices are at +0x268 +0x1A8 +0x1E8 +0x228 and the vec4 at +0x2A8");
    const f2::NeckTargets t = f2::neckTargetsFromBase(0x140000000ull);
    check(t.vtable == 0x140000000ull + 0x51FCE98 && t.slot == t.vtable + 0x20 && t.getter == 0x140000000ull + 0xF88330 && t.localSite == 0x140000000ull + 0x1073946,
          "the targets derive from the image base: slot = vtable + 0x20");
    check(f2::neckSlotOk(t.getter, t) && !f2::neckSlotOk(t.getter + 8, t), "the slot must hold the build's accessor, nothing else");
    check(f2::isLocalEyeSite(t.localSite, t) && !f2::isLocalEyeSite(t.localSite + 1, t) && !f2::isLocalEyeSite(0, t), "only the first-person camera activity's return address is the local site");
    check(f2::neckInterfaceOk(t.vtable, t) && !f2::neckInterfaceOk(t.vtable + 8, t) && !f2::neckInterfaceOk(0, t), "an interface is live only while its first qword is the eye vtable");
}

// A function the rig calls the way the game calls the accessor: rcx = the interface, rdx = its vtable, `call [rdx+20h]`. The return
// address (thunk + 7) identifies the caller.
struct Thunks {
    uint8_t* page = nullptr;
    uintptr_t localReturn() const { return reinterpret_cast<uintptr_t>(page) + 7; }
    using Call = void* (__fastcall*)(void* self, void* vtable);
    Call local() const { return reinterpret_cast<Call>(page); }
    Call other() const { return reinterpret_cast<Call>(page + 0x40); }
};
Thunks makeThunks() {
    Thunks t;
    t.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!t.page) return t;
    const uint8_t thunk[12] = {0x48, 0x83, 0xEC, 0x28, 0xFF, 0x52, 0x20, 0x48, 0x83, 0xC4, 0x28, 0xC3};   // sub rsp,28h; call [rdx+20h]; add rsp,28h; ret
    std::memcpy(t.page, thunk, sizeof(thunk));
    std::memcpy(t.page + 0x40, thunk, sizeof(thunk));
    DWORD old = 0;
    VirtualProtect(t.page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), t.page, 4096);
    return t;
}

uint8_t* makeCode(const std::vector<uint8_t>& bytes) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return nullptr;
    std::memcpy(page, bytes.data(), bytes.size());
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, bytes.size());
    return page;
}

// The camera controller's update: the real 15-byte prologue, then what restores what it saved.
uint8_t* makeControllerPage() {
    std::vector<uint8_t> b(ecm::kControllerPrologue, ecm::kControllerPrologue + sizeof(ecm::kControllerPrologue));
    const uint8_t tail[] = {0x48, 0x8B, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x6C, 0x24, 0x10, 0x48, 0x8B, 0x74, 0x24, 0x18, 0x31, 0xC0, 0xC3};
    b.insert(b.end(), tail, tail + sizeof(tail));
    return makeCode(b);
}

struct EyeInterface {
    alignas(16) uint8_t bytes[0x2C0] = {};
    void setMatrix(uint32_t off, float ox, float oy, float oz) {
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, ox, oy, oz, 1};
        std::memcpy(bytes + off, m, 64);
    }
};

void testF2Glue() {
    std::printf("the F2 instruments end to end (a synthetic vtable, synthetic callers, a synthetic controller)\n");
    namespace t = edvr::explorercamprobetest;
    const Thunks th = makeThunks();
    const std::vector<uint8_t> getterCode(f2::kEyeGetterCode, f2::kEyeGetterCode + 8);
    uint8_t* getterPage = makeCode(getterCode);
    uint8_t* controllerPage = makeControllerPage();
    uint8_t good[128] = {};
    const size_t nGood = buildSynthetic(good, false);
    uint8_t* freePage = makeExecutable(good, nGood);
    auto* vtable = static_cast<uintptr_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(th.page && getterPage && controllerPage && freePage && vtable, "(pages for the thunks, the accessor, the controller, the free-camera update and a vtable)");
    if (!th.page || !getterPage || !controllerPage || !freePage || !vtable) return;
    for (int i = 0; i < 8; ++i) vtable[i] = 0xDEAD0000u + i;
    vtable[4] = reinterpret_cast<uintptr_t>(getterPage);

    static EyeInterface ifaceA, ifaceB;
    ifaceA = EyeInterface();
    ifaceB = EyeInterface();
    const uint64_t vt = reinterpret_cast<uint64_t>(vtable);
    std::memcpy(ifaceA.bytes, &vt, 8);
    std::memcpy(ifaceB.bytes, &vt, 8);
    // The known frame of testNeckMath: the eye at world (10.12, 1.55, 19.98); the other matrices carry marker origins.
    ifaceA.setMatrix(f2::kOffEyeMatrix, 10.12f, 1.55f, 19.98f);
    ifaceA.setMatrix(f2::kOffEyeB, 1, 2, 3);
    ifaceA.setMatrix(f2::kOffEyeC, 4, 5, 6);
    ifaceA.setMatrix(f2::kOffEyeD, 7, 8, 9);
    const float vec4[4] = {0.5f, 0.25f, 0.125f, 1.0f};
    std::memcpy(ifaceA.bytes + f2::kOffEyeVec, vec4, 16);

    // The free-camera activity: local pose (identity, origin (0,1.68,0.10)), world pose = local x F + root, and three action objects.
    static uint8_t activity[0x600];
    static uint8_t actRot[0x40], actWorld[0x40], actRel[0x40];
    std::memset(activity, 0, sizeof(activity));
    std::memset(actRot, 0, sizeof(actRot));
    std::memset(actWorld, 0, sizeof(actWorld));
    std::memset(actRel, 0, sizeof(actRel));
    const uint64_t pRot = reinterpret_cast<uint64_t>(actRot), pWorld = reinterpret_cast<uint64_t>(actWorld), pRel = reinterpret_cast<uint64_t>(actRel);
    std::memcpy(activity + 0x4F8, &pRot, 8);
    std::memcpy(activity + 0x500, &pWorld, 8);
    std::memcpy(activity + 0x508, &pRel, 8);
    const float local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 1.68f, 0.10f, 1};
    const float world[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 10.10f, 1.68f, 20.0f, 1};   // L x F, and lo x F + root
    std::memcpy(activity + 0x3B0, local, 64);
    std::memcpy(activity + 0x70, world, 64);
    activity[0x48C] = 4;
    auto pressedInt = [](uint8_t* action) -> int32_t& { return *reinterpret_cast<int32_t*>(action + 0x1C); };

    // The controller: a mode byte and three action objects.
    static uint8_t controller[0x600];
    static uint8_t actPhoto[0x40], actFree[0x40], actQuit[0x40];
    std::memset(controller, 0, sizeof(controller));
    std::memset(actPhoto, 0, sizeof(actPhoto));
    std::memset(actFree, 0, sizeof(actFree));
    std::memset(actQuit, 0, sizeof(actQuit));
    const uint64_t pPhoto = reinterpret_cast<uint64_t>(actPhoto), pFree = reinterpret_cast<uint64_t>(actFree), pQuit = reinterpret_cast<uint64_t>(actQuit);
    std::memcpy(controller + 0x310, &pPhoto, 8);
    std::memcpy(controller + 0x328, &pFree, 8);
    std::memcpy(controller + 0x340, &pQuit, 8);
    auto callController = reinterpret_cast<uint64_t (__fastcall*)(void*)>(controllerPage);

    ExplorerCamTestTargets targets;
    targets.freeCamera = reinterpret_cast<uintptr_t>(freePage);
    targets.controller = reinterpret_cast<uintptr_t>(controllerPage);
    explorercamtest::setTargets(targets);
    explorercamf2test::NeckSeam seam;
    seam.vtable = reinterpret_cast<uintptr_t>(vtable);
    seam.slot = reinterpret_cast<uintptr_t>(&vtable[4]);
    seam.getter = reinterpret_cast<uintptr_t>(getterPage);
    seam.localSite = th.localReturn();
    explorercamf2test::setNeckTargets(seam);

    Capture cap;
    // ---- armed -----------------------------------------------------------------------------------------------------------------------
    t::boundary(1, 1000, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I4 armed:") == 1 && has(cap.nth("explorer cam probe I4 armed:", 0), "stolen=5 bytes"), "I4 ARMED: one line (the controller hook, 5 stolen bytes)");
    check(cap.count("explorer cam probe N armed:") == 1 && has(cap.nth("explorer cam probe N armed:", 0), "is the first-person camera activity's own read") &&
              has(cap.nth("explorer cam probe N armed:", 0), "SLOT is replaced and the code is not hooked"),
          "N ARMED: one line, naming the slot swap, why the code is not hooked, and the local site");
    check(explorercamf2test::neckInstalled() && vtable[4] == explorercamf2test::neckReplacement() && vtable[3] == 0xDEAD0003u && vtable[5] == 0xDEAD0005u,
          "the swap wrote ONE slot: vtable[4] is the replacement, its neighbours are untouched");
    check(*reinterpret_cast<uint64_t*>(getterPage) == *reinterpret_cast<const uint64_t*>(f2::kEyeGetterCode), "...and the accessor's own code was not touched");

    // ---- the replacement: returns exactly what the original returns, and the return-address filter -----------------------------------
    void* r = th.other()(ifaceA.bytes, vtable);
    check(r == ifaceA.bytes + 0x268 && explorercamf2test::neckCalls() == 1 && explorercamf2test::neckLocalSiteCalls() == 0 && explorercamf2test::neckLocalEye() == 0 &&
              explorercamf2test::neckDistinct() == 1,
          "a call from any other return address returns self+0x268 (what the original returns), is counted, and does NOT set the local eye");
    r = th.local()(ifaceA.bytes, vtable);
    check(r == ifaceA.bytes + 0x268 && explorercamf2test::neckCalls() == 2 && explorercamf2test::neckLocalSiteCalls() == 1 &&
              explorercamf2test::neckLocalEye() == reinterpret_cast<uintptr_t>(ifaceA.bytes),
          "a call that returns to the first-person camera activity's site returns the same value, counts as a local-site call, and KEEPS the interface");
    r = th.other()(ifaceB.bytes, vtable);
    check(r == ifaceB.bytes + 0x268 && explorercamf2test::neckDistinct() == 2 && explorercamf2test::neckLocalEye() == reinterpret_cast<uintptr_t>(ifaceA.bytes),
          "another humanoid's interface is a second distinct pointer and does not replace the local eye");
    th.other()(ifaceB.bytes, vtable);
    check(explorercamf2test::neckDistinct() == 2 && explorercamf2test::neckCalls() == 4, "...and a repeat is counted, not re-counted as distinct");

    // ---- the first 1 Hz tick: the local site was called, no free-camera update has paired a sample yet --------------------------------------
    cap.clear();
    t::boundary(2, 2050, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe N matrix:") == 5 && has(cap.nth("explorer cam probe N matrix:", 0), "phase=local-site") && cap.count("explorer cam probe N local:") == 1 &&
              has(cap.nth("explorer cam probe N local:", 0), "no free-camera update has paired a sample"),
          "the local site called and no free-camera sample yet: the matrices print (phase=local-site) and ONE notice says the commander-local figure waits for the free camera");

    // ---- I3 pressed ------------------------------------------------------------------------------------------------------------------------
    cap.clear();
    t::observe(activity);
    t::boundary(2, 2100, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 pressed:") == 1 && has(cap.nth("explorer cam probe I3 pressed:", 0), "first-sight") && has(cap.nth("explorer cam probe I3 pressed:", 0), "rotation_lock(+0x4F8)=0") &&
              has(cap.nth("explorer cam probe I3 pressed:", 0), "world_fix(+0x500)=0") && has(cap.nth("explorer cam probe I3 pressed:", 0), "relative_fix(+0x508)=0"),
          "I3 PRESSED: the first sight prints the three ints (0 0 0)");
    cap.clear();
    t::observe(activity);
    t::boundary(3, 2150, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 pressed:") == 0, "...nothing while they do not change");
    pressedInt(actWorld) = 1;
    t::observe(activity);
    t::boundary(4, 2200, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 pressed:") == 1 && has(cap.nth("explorer cam probe I3 pressed:", 0), "world_fix(+0x500)=1") && has(cap.nth("explorer cam probe I3 pressed:", 0), "(was 0/0/0)"),
          "the player's FixCameraWorldToggle (+0x500) going to 1 is a change line, 'was 0/0/0'");
    pressedInt(actWorld) = 0;
    const uint64_t nothing = 0;
    std::memcpy(activity + 0x508, &nothing, 8);
    cap.clear();
    t::observe(activity);
    t::boundary(5, 2250, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I3 pressed:") == 1 && has(cap.nth("explorer cam probe I3 pressed:", 0), "relative_fix(+0x508)=unreadable"), "a NULL action pointer reads 'unreadable', not a fault");
    std::memcpy(activity + 0x508, &pRel, 8);

    // ---- the neck's paired sample, 1 Hz, in commander-local axes ----------------------------------------------------------------------
    cap.clear();
    t::observe(activity);                                    // publishes the paired sample (the local eye is known)
    t::boundary(6, 3100, true, &Capture::add, &cap);         // due (a second after the last 1 Hz tick)
    check(cap.count("explorer cam probe N matrix:") == 5, "1 Hz: five N matrix lines (the four matrices and the vec4)");
    check(has(cap.nth("explorer cam probe N matrix:", 0), "+0x268(eye) origin=(10.120,1.550,19.980)") && has(cap.nth("explorer cam probe N matrix:", 1), "+0x1A8 origin=(1.000,2.000,3.000)") &&
              has(cap.nth("explorer cam probe N matrix:", 2), "+0x1E8 origin=(4.000,5.000,6.000)") && has(cap.nth("explorer cam probe N matrix:", 3), "+0x228 origin=(7.000,8.000,9.000)") &&
              has(cap.nth("explorer cam probe N matrix:", 4), "vec4(+0x2A8)=(0.500,0.250,0.125,1.000)"),
          "...each with its origin and rows, and the vec4");
    const std::string local1 = cap.nth("explorer cam probe N local:", 0);
    check(!local1.empty() && has(local1, "eye_in_commander_local(right,up,forward)=(0.020,1.550,0.120)") && has(local1, "root=(10.000,0.000,20.000)") &&
              has(local1, "eye_world(+0x268)=(10.120,1.550,19.980)") && has(local1, "activity_world_origin(+0xA0)=(10.100,1.680,20.000)") &&
              has(local1, "ASSUMPTION: +0x268 is in the same world frame as the activity's +0x70"),
          "N LOCAL: the eye in the commander's axes (0.020, 1.550, 0.120), the root, both raw world origins, and the ASSUMPTION said in the line");
    // Local-site-only phase: no free-camera sample for a while, but the first-person camera activity reads the eye.
    cap.clear();
    th.local()(ifaceA.bytes, vtable);
    t::boundary(7, 4700, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe N matrix:") == 5 && has(cap.nth("explorer cam probe N matrix:", 0), "phase=local-site") && cap.count("explorer cam probe N local:") == 0,
          "with no free-camera update for 1.5 s but the local site called: the matrices print (phase=local-site) and there is no commander-local line (no fresh pair)");
    // Nothing at all happening: silence at 1 Hz.
    cap.clear();
    t::boundary(8, 5800, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe N matrix:") == 0 && cap.count("explorer cam probe N local:") == 0, "neither the free camera nor the local site in the last second: no matrix lines");

    // ---- the heartbeats (5 s) -------------------------------------------------------------------------------------------------------------
    cap.clear();
    t::boundary(9, 6100, true, &Capture::add, &cap);
    const std::string nbeat = cap.nth("explorer cam probe N heartbeat:", 0);
    check(!nbeat.empty() && has(nbeat, "hook=armed") && has(nbeat, "calls=5(") && has(nbeat, "local_site_calls=2(") && has(nbeat, "distinct_interfaces=2") && has(nbeat, "local_eye=set (0x") &&
              has(nbeat, "local_site_last_seen="),
          "N HEARTBEAT: calls, local-site calls, distinct interfaces, whether the local eye is set, when the local site was last seen");
    const std::string ibeat = cap.nth("explorer cam probe I4 heartbeat:", 0);
    check(!ibeat.empty() && has(ibeat, "hook=armed") && has(ibeat, "total_calls=0") && has(ibeat, "idle=no-call-yet"), "I4 HEARTBEAT before any controller call: 'idle=no-call-yet'");

    // ---- I4: the controller, through the hook ------------------------------------------------------------------------------------------------
    cap.clear();
    callController(controller);
    t::boundary(10, 6200, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I4 change:") == 1 && has(cap.nth("explorer cam probe I4 change:", 0), "first-sight") && has(cap.nth("explorer cam probe I4 change:", 0), "mode(+0x3E0) 0->0 (closed)"),
          "I4: the first controller call is a first-sight line (mode 0, closed): whether it runs with the camera closed is answered in the log");
    controller[0x3E0] = 1;
    pressedInt(actPhoto) = 1;
    cap.clear();
    callController(controller);
    t::boundary(11, 6300, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I4 change:") == 1 && has(cap.nth("explorer cam probe I4 change:", 0), "mode(+0x3E0) 0->1") && has(cap.nth("explorer cam probe I4 change:", 0), "photo(+0x310)=1") &&
              has(cap.nth("explorer cam probe I4 change:", 0), "(suite open on a preset)"),
          "I4: mode 0 -> 1 with PhotoCameraToggle's int at 1 is one change line naming both");
    pressedInt(actPhoto) = 0;
    cap.clear();
    callController(controller);
    callController(controller);
    t::boundary(12, 6400, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe I4 change:") == 1 && has(cap.nth("explorer cam probe I4 change:", 0), "photo(+0x310)=0"), "...and the int going back to 0 is one more; calls that change nothing print nothing");
    cap.clear();
    t::boundary(13, 11200, true, &Capture::add, &cap);
    const std::string ibeat2 = cap.nth("explorer cam probe I4 heartbeat:", 0);
    check(has(ibeat2, "calls=4") && has(ibeat2, "total_calls=4") && has(ibeat2, "+0x3E0=1") && !has(ibeat2, "idle="), "I4 HEARTBEAT after calls: the window's calls, the total and the mode, no 'idle'");

    // ---- stale: the interface's vtable pointer no longer matches -------------------------------------------------------------------------
    cap.clear();
    const uint64_t garbage = 0x1234;
    std::memcpy(ifaceA.bytes, &garbage, 8);
    t::observe(activity);
    t::boundary(14, 11300, true, &Capture::add, &cap);
    check(explorercamf2test::neckLocalEye() == 0 && cap.count("explorer cam probe N stale:") == 1 && has(cap.nth("explorer cam probe N stale:", 0), "was stale"),
          "STALE: an interface whose vtable is not the eye vtable is cleared, and said once");
    cap.clear();
    t::observe(activity);
    t::boundary(15, 11400, true, &Capture::add, &cap);
    check(cap.count("explorer cam probe N stale:") == 0, "...and not said again");
    std::memcpy(ifaceA.bytes, &vt, 8);
    th.local()(ifaceA.bytes, vtable);
    check(explorercamf2test::neckLocalEye() == reinterpret_cast<uintptr_t>(ifaceA.bytes), "...the next local-site call re-learns it");

    // ---- key off: the observers detach, the swap stays (a faithful getter) --------------------------------------------------------------------------
    cap.clear();
    t::boundary(16, 12000, false, &Capture::add, &cap);
    const uint64_t callsOff = explorercamf2test::neckCalls();
    void* rr = th.other()(ifaceB.bytes, vtable);
    check(cap.count("explorer cam probe: off") == 1 && rr == ifaceB.bytes + 0x268 && explorercamf2test::neckCalls() == callsOff + 1 && vtable[4] == explorercamf2test::neckReplacement(),
          "KEY OFF: one line; the swapped getter still returns exactly what the original returns");
    callController(controller);
    pressedInt(actPhoto) = 1;
    cap.clear();
    callController(controller);
    t::boundary(17, 12100, false, &Capture::add, &cap);
    check(cap.count("explorer cam probe I4 change:") == 0, "...and the controller observer is detached: a change prints nothing");
    pressedInt(actPhoto) = 0;

    // ---- unload: the original goes back --------------------------------------------------------------------------------------------------------
    t::reset();
    check(!explorercamf2test::neckInstalled() && vtable[4] == reinterpret_cast<uintptr_t>(getterPage), "RESET: the original accessor is back in the slot");

    // ---- stand-downs: a slot or code that is not build 332841's ----------------------------------------------------------------------------------
    for (int variant = 0; variant < 2; ++variant) {
        vtable[4] = reinterpret_cast<uintptr_t>(getterPage);
        explorercamtest::setTargets(targets);
        explorercamf2test::NeckSeam s2 = seam;
        if (variant == 0) {
            vtable[4] = 0xFEEDF00Dull;
        } else {
            const uint8_t wrong[8] = {0x48, 0x8D, 0x81, 0x78, 0x02, 0x00, 0x00, 0xC3};
            uint8_t* badGetter = makeCode(std::vector<uint8_t>(wrong, wrong + 8));
            vtable[4] = reinterpret_cast<uintptr_t>(badGetter);
            s2.getter = reinterpret_cast<uintptr_t>(badGetter);
        }
        explorercamf2test::setNeckTargets(s2);
        cap.clear();
        t::boundary(20, 20000, true, &Capture::add, &cap);
        check(cap.count("explorer cam probe N stood down:") == 1 && has(cap.nth("explorer cam probe N stood down:", 0), "the game build differs") &&
                  has(cap.nth("explorer cam probe N stood down:", 0), "nothing was patched") && !explorercamf2test::neckInstalled() && vtable[4] != explorercamf2test::neckReplacement(),
              variant == 0 ? "WRONG SLOT VALUE: one 'N stood down' line, nothing patched" : "WRONG ACCESSOR CODE: one 'N stood down' line, nothing patched");
        t::boundary(21, 26000, true, &Capture::add, &cap);
        check(cap.count("explorer cam probe N heartbeat:") >= 1 && has(cap.nth("explorer cam probe N heartbeat:", 0), "hook=stood down") &&
                  has(cap.nth("explorer cam probe N heartbeat:", 0), "idle=the hook is stood down"),
              "...and the heartbeat says the hook is stood down (never ran, distinguishable from ran and saw nothing)");
        t::boundary(22, 26100, false, &Capture::add, &cap);
        t::reset();
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            std::printf("explorer cam probe test: dry run, nothing to do\n");
            return 0;
        }
    }
    testIdentity();
    testDecode();
    testSeqlock();
    testChangeDetection();
    testEventRing();
    testSets();
    testCadence();
    testFingerprint();
    testYawPitch();
    testCamTee();
    testText();
    testConsumerIdle();
    testConsumerEngaged();
    testConsumerStale();
    testGlue();
    testNeckMath();
    testF2Core();
    testF2Glue();
    if (g_failures) {
        std::printf("explorer cam probe: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("explorer cam probe: PASS\n");
    return 0;
}
