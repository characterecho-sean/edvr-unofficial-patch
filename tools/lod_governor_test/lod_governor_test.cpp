// Build gate for the settlement LOD governor (src/d3d11/lod_governor.*,
// fix.settlement_detail; shadow only in this build): the policy's steps and
// hysteresis on synthetic frame sequences, the engine arithmetic the shadow
// repeats (FUN_1442B3FC0 / FUN_144308B30's distance, LOD distance and LOD
// pick), the two observers on synthetic engine memory laid out as the
// decompiles read it -- a render context with two eye views and an
// orthographic one, the builder's six-pointer block around a part test, a
// record with its node and LOD table -- including a wrong nibble, an engine
// reject, a foreign caller and a wild pointer; the per-thread counters under
// four threads; and the frame boundary end to end against a stub native
// timing feed and a capturing log: the configure line, the step lines, the
// 30-second summaries, "k stayed 1", and nothing at all while off. The frame
// work is the runtime's caller work per cycle (timing v5), never the app work
// beside it -- the first shadow flight's 8.4 ms app / 14.4 ms caller frames
// must step k up -- and a v3/v4 runtime falls back to the app work, named as
// such in every line; a v5 frame without caller work is an invalid sample.
// The summary prints the effective s x k, and every line fits the real log's
// line. It then times both observers (the cost the design doc quotes). No
// hooks, no game.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// The implementation under test is compiled INTO this TU, so the rig reaches
// its anonymous-namespace internals (applyConfig, frameBoundaryAt, the
// counter slots) -- the static_prop_gate_test pattern.
#include "../../src/d3d11/lod_governor.cpp"

namespace edvr {
// Linker stubs: no hooks are attached, no file is read, the log is captured.
std::vector<std::string> g_lines;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_lines.push_back(buf);
}
Config& Config::get() { static Config c; return c; }
std::string Config::getString(const char*, const char* def) const { return def; }
float Config::getFloat(const char*, float def) const { return def; }
NativeTimingSnapshot g_timing;
NativeTimingSnapshot nativeTimingSnapshot() noexcept { return g_timing; }
const char* g_attachResult = "installed";
const char* g_partStatusStub = "hooked";
LodGovernorBuilderFn g_obsBuilder = nullptr;
LodGovernorPartFn g_obsPart = nullptr;
int g_attaches = 0, g_detaches = 0;
void kinematicEvalSetLodGovernorObservers(LodGovernorBuilderFn b, LodGovernorPartFn p) noexcept {
    g_obsBuilder = b;
    g_obsPart = p;
}
const char* kinematicEvalLodGovernorAttach() noexcept { ++g_attaches; return g_attachResult; }
void kinematicEvalLodGovernorDetach() noexcept { ++g_detaches; }
bool kinematicEvalBuilderHooked() noexcept { return true; }
const char* kinematicEvalPartTestStatus() noexcept { return g_partStatusStub; }
}  // namespace edvr

using namespace edvr;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

template <typename T> void put(std::vector<uint8_t>& b, size_t off, const T& v) { std::memcpy(b.data() + off, &v, sizeof(T)); }
uintptr_t addr(const std::vector<uint8_t>& b) { return reinterpret_cast<uintptr_t>(b.data()); }

bool logged(const char* needle, size_t from = 0) {
    for (size_t i = from; i < g_lines.size(); ++i)
        if (g_lines[i].find(needle) != std::string::npos) return true;
    return false;
}
size_t countLogged(const char* needle, size_t from = 0) {
    size_t n = 0;
    for (size_t i = from; i < g_lines.size(); ++i)
        if (g_lines[i].find(needle) != std::string::npos) ++n;
    return n;
}

void table(std::vector<uint8_t>& b, size_t off, std::initializer_list<float> t, uint32_t count) {
    int i = 0;
    for (float v : t) put(b, off + 0x10 * size_t(i++), v);
    put(b, off + 0x70, count);
}

// --- The policy -----------------------------------------------------------------------
void casePolicy() {
    using namespace lodgov;
    Policy p;
    // The default ceiling is 4.0: k multiplies the s the game holds, the
    // slider's floor is s = 1.5 and its maximum detail s = 1.0, and the LOD
    // note's table needs s x k = 3 from there.
    check(kDefaultMax == 4.0f && p.maxSteps() == 60 && p.kMax() == 4.0f && p.k() == 1.0f,
          "policy: the default k_max is 4.0 (sixty steps); k starts at 1");
    p.configure(2.0f);
    check(p.maxSteps() == 20 && p.k() == 1.0f && p.kMax() == 2.0f, "policy: k_max 2.0 is twenty steps of 0.05; k starts at 1");
    uint64_t t = 1000;
    auto frame = [&](uint32_t records, Work w, double ms) {
        FrameSignals s;
        s.records = records;
        s.work = w;
        s.workMs = ms;
        s.periodMs = 1000.0 / 90.0;
        s.nowMs = t;
        t += 11;
        return p.update(s);
    };
    bool any = false;
    for (int i = 0; i < 300; ++i) any |= frame(150, Work::Valid, 14.0) != Step::None;
    check(!any && p.steps() == 0 && !p.inSettlement(), "policy: over budget with 150 records never rises");
    for (int i = 0; i < 300; ++i) any |= frame(679, Work::Valid, 11.40) != Step::None;
    check(!any && p.steps() == 0 && p.inSettlement(), "policy: 679 records within the 0.3 ms margin holds k at 1");
    for (int i = 0; i < 29; ++i) any |= frame(679, Work::Valid, 12.0) != Step::None;
    check(!any && p.overRun() == 29, "policy: 29 over-budget samples do not step");
    check(frame(679, Work::Valid, 12.0) == Step::Up && p.steps() == 1 && p.k() == 1.05f, "policy: the 30th steps up by 0.05");
    int ups = 0;
    for (int i = 0; i < 90; ++i) ups += frame(679, Work::Valid, 12.0) == Step::Up;   // 990 ms
    check(ups == 0, "policy: no second step inside the one-second ramp interval");
    check(frame(679, Work::Valid, 12.0) == Step::Up && p.steps() == 2, "policy: the next step a second later");
    // A frame with no new sample holds the run; a bad sample breaks it.
    frame(679, Work::None, 0);
    check(p.overRun() >= 30, "policy: a frame without a sample holds the over-budget run");
    frame(679, Work::Invalid, 0);
    check(p.overRun() == 0, "policy: an invalid sample breaks the run");
    for (int i = 0; i < 29; ++i) frame(679, Work::Valid, 12.0);
    check(p.steps() == 2, "policy: the run is counted again from the bad sample");
    // A frame under 200 records blocks a rise, without leaving the settlement.
    for (int i = 0; i < 200; ++i) frame(180, Work::Valid, 12.0);
    check(p.steps() == 2 && p.inSettlement(), "policy: 150-199 records holds k and the settlement");
    // Up to k_max and no further.
    for (int i = 0; i < 4000; ++i) frame(679, Work::Valid, 12.0);
    check(p.steps() == 20 && p.k() == 2.0f, "policy: k stops at k_max");
    // The hysteresis zone holds; under by more than 1.0 ms steps down once a second.
    for (int i = 0; i < 300; ++i) any |= frame(679, Work::Valid, 10.5) != Step::None;
    check(!any && p.steps() == 20, "policy: between period - 1.0 and period + 0.3 ms k holds");
    int downs = 0;
    for (int i = 0; i < 29; ++i) downs += frame(679, Work::Valid, 9.0) == Step::Down;
    check(downs == 0, "policy: 29 under-budget samples do not step down");
    check(frame(679, Work::Valid, 9.0) == Step::Down && p.steps() == 19, "policy: the 30th steps down by 0.05");
    for (int i = 0; i < 90; ++i) downs += frame(679, Work::Valid, 9.0) == Step::Down;
    check(downs == 0, "policy: at most one step down a second");
    // A lowered k_max clamps at once.
    p.configure(1.5f);
    check(frame(679, Work::Valid, 11.0) == Step::Clamp && p.steps() == 10, "policy: a lowered k_max clamps k at once");
    // Leaving the settlement: 29 sparse frames hold, the 30th resets k to 1.
    Step last = Step::None;
    for (int i = 0; i < 29; ++i) last = frame(20, Work::Valid, 12.0);
    check(last == Step::None && p.steps() == 10 && p.inSettlement(), "policy: 29 frames under 150 records hold");
    check(frame(20, Work::Valid, 12.0) == Step::Reset && p.steps() == 0 && !p.inSettlement(),
          "policy: the 30th frame under 150 records resets k to 1");
    p.configure(std::numeric_limits<float>::quiet_NaN());
    check(p.maxSteps() == 0, "policy: a NaN k_max holds k at 1");
    p.configure(9.0f);
    check(p.maxSteps() == 60 && p.kMax() == 4.0f, "policy: k_max is held to 4");
    p.configure(1.26f);
    check(p.maxSteps() == 5, "policy: k_max is quantised to the 0.05 step");
}

// --- The engine arithmetic ----------------------------------------------------------------
void caseMath() {
    using namespace lodgov;
    bool close = true;
    for (float r = 0.5f; r < 5000.0f; r *= 1.37f) {
        const float c[4] = {r * 0.6f, r * 0.8f, 0.0f, 1.0f}, cam[4] = {0, 0, 0, 0};
        const float d = engineDistance(c, cam);
        close &= std::fabs(d - r) <= r * (1.0f / 1024.0f);
    }
    check(close, "math: the engine's rsqrt(rcp(d^2)) is d to within 2^-10");
    std::vector<uint8_t> raw(0x80, 0);
    table(raw, 0, {10.0f, 2.0f, 5.0f, 8.0f}, 3);
    const LodTable t = LodTable::fromBytes(raw.data());
    uint32_t n = 99;
    check(lodPick(t, 1.0f, &n) && n == 0, "math: f under t1 picks nibble 0");
    check(lodPick(t, 2.0f, &n) && n == 0, "math: f equal to t1 still picks 0 (f <= t[i+1])");
    check(lodPick(t, 3.0f, &n) && n == 1, "math: f between t1 and t2 picks 1");
    check(lodPick(t, 9.0f, &n) && n == 3, "math: f past every level threshold but under t0 picks the count");
    check(!lodPick(t, 11.0f, &n), "math: f over t0 is rejected");
    check(lodPick(t, std::numeric_limits<float>::quiet_NaN(), &n) && n == 0, "math: a NaN f passes at nibble 0, as the engine's compares do");
    table(raw, 0, {10.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, 7);
    const LodTable t7 = LodTable::fromBytes(raw.data());
    check(t7.inRange() && lodPick(t7, 2.0f, &n) && n == 7, "math: a count of 7 reads the count's own bits as t7, as the engine does");
    put<uint32_t>(raw, 0x70, 8);
    check(!LodTable::fromBytes(raw.data()).inRange(), "math: a count past 7 is refused");
    check(angleBucket(1.0f, 1000.0f) == 0 && angleBucket(1.0f, 150.0f) == 1 && angleBucket(1.0f, 80.0f) == 2 &&
          angleBucket(1.0f, 30.0f) == 3 && angleBucket(2.0f, 1.0f) == 3, "math: the angular-radius buckets");
    check(lodDistance(0.001f, 100.0f, 1.0f, 1.0f, 0.0f) == 0.001f * 99.0f, "math: f = A*(d - r)*s + B");
}

// --- Synthetic engine memory ------------------------------------------------------------------
struct Fake {
    std::vector<uint8_t> ctx = std::vector<uint8_t>(0x1A948 + 64, 0);
    std::vector<uint8_t> centre = std::vector<uint8_t>(16, 0), sphere = std::vector<uint8_t>(16, 0);
    std::vector<uint8_t> partTable = std::vector<uint8_t>(0x80, 0);
    std::vector<uint8_t> block = std::vector<uint8_t>(48, 0), out = std::vector<uint8_t>(8, 0);
    std::vector<uint8_t> record = std::vector<uint8_t>(0x2F0, 0), node = std::vector<uint8_t>(0x80, 0);
    std::vector<uint8_t> recTable = std::vector<uint8_t>(0x80, 0);
    uintptr_t view[3] = {};
    Fake() {
        // Views: 0 = bit 1 (eye A) and 1 = bit 22 (eye B), a pixel of 0.001 per
        // metre at the origin; 2 = bit 4, orthographic (A = 0, B = 0.05).
        put(ctx, 0x30, 1.0f);
        put<uint64_t>(ctx, 0x1A940, 3);
        for (uint32_t i = 0; i < 64; ++i) put<uint32_t>(ctx, 0x1A840 + i * 4, 0xFFFFFFFFu);
        const uint32_t bits[3] = {1, 22, 4};
        for (uint32_t v = 0; v < 3; ++v) {
            const size_t at = 0x40 + size_t(v) * 0x6A0;
            put(ctx, at + 0x550, v < 2 ? 0.001f : 0.0f);
            put(ctx, at + 0x560, v < 2 ? 0.0f : 0.05f);
            put<uint64_t>(ctx, at + 0x570, 1ull << bits[v]);
            put<uint32_t>(ctx, at + 0x578, bits[v]);
            put<uint32_t>(ctx, 0x1A840 + bits[v] * 4, v);
            view[v] = addr(ctx) + at;
        }
        // A part at z 100, radius 1: d ~ 100, f = 0.001 * 99 = 0.099. Its
        // table: t0 0.15, t1 0.05, t2 0.12, count 2 -> nibble 1; at k 2,
        // f 0.198 > t0: dropped, r/d 0.57 deg.
        put(centre, 8, 100.0f);
        put(centre, 12, 1.0f);
        put(sphere, 0, 1.0f);
        table(partTable, 0, {0.15f, 0.05f, 0.12f}, 2);
        put<uint64_t>(block, 0x00, addr(centre));
        put<uint64_t>(block, 0x08, addr(sphere));
        put<uint64_t>(block, 0x20, addr(partTable));
        put<uint64_t>(block, 0x28, addr(ctx));
        setOut(1, 1);
        // A record at z 200, radius 2: f = 0.198 in the eyes (nibble 1 of
        // t0 0.25, t1 0.1, t2 0.2); 0.05 in the orthographic view (nibble 0).
        // Node LOD count 3. At k 2 the eyes' f 0.396 > t0: both fail.
        put<uint64_t>(record, 0x18, addr(node));
        put<uint16_t>(node, 0x6A, 3);
        put<uint64_t>(record, 0x20, addr(recTable));
        table(recTable, 0, {0.25f, 0.1f, 0.2f}, 2);
        put(record, 0x248, 200.0f);
        put(record, 0x24C, 1.0f);
        put(record, 0x280, 2.0f);
        setMask((1ull << 1) | (1ull << 22) | (1ull << 4));
        setNibble(1, 1);
        setNibble(22, 1);
        setNibble(4, 0);
    }
    void setOut(uint32_t lod, uint8_t pass) {
        put(out, 0, lod);
        put(out, 4, pass);
    }
    void setMask(uint64_t m) { put(record, 0x208, m); }
    void setNibble(uint32_t bit, uint32_t n) {
        uint64_t w = 0;
        std::memcpy(&w, record.data() + 0x210 + (bit >> 4) * 8, 8);
        w = (w & ~(0xFull << ((bit & 15) * 4))) | (uint64_t(n) << ((bit & 15) * 4));
        put(record, 0x210 + (bit >> 4) * 8, w);
    }
    uintptr_t items() const { return addr(block); }
    uintptr_t outAt() const { return addr(out); }
    uintptr_t nibbles() const { return addr(record) + 0x210; }
    uintptr_t ctxAt() const { return addr(ctx); }
};

void setK(float k) { g_kBits.store(toBits(k)); }

uint32_t g_before[kCounters];
void mark() { sumSlots(g_before); }
uint32_t delta(uint32_t c) {
    uint32_t now[kCounters];
    sumSlots(now);
    return now[c] - g_before[c];
}
uint32_t partField(uint32_t cls, uint32_t f) { return delta(cPartBase + cls * kPartFields + f); }
uint32_t recField(uint32_t eye, uint32_t f) { return delta(cRecBase + eye * kRecFields + f); }

void caseObservers() {
    Fake f;
    g_live.store(true);
    setK(1.0f);
    // The eyes are named from the context the builder observer publishes.
    g_ctx.store(f.ctxAt());
    identifyEyes(g_state);
    check(g_eyes.load() == (1u | (22u << 8)), "observers: the eyes are the two finest perspective views, bits 1 and 22");
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(delta(cParts) == 1 && partField(0, pSeen) == 1 && partField(0, pPassed) == 1 && partField(0, pWouldDrop) == 0 &&
          partField(0, pMismatch) == 0, "part: seen and passed in eye A; nothing differs at k = 1");
    setK(2.0f);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[1], true);
    check(partField(1, pWouldDrop) == 1 && partField(1, pHist2) == 1, "part: at k = 2 eye B's part would drop, 0.57 deg");
    setK(1.10f);   // f 0.109: still under t2 0.12 -> nibble 1, nothing differs
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pPassed) == 1 && partField(0, pWouldChange) == 0 && partField(0, pWouldDrop) == 0,
          "part: at k = 1.1 the nibble holds");
    setK(1.25f);   // f 0.124: past t2 0.12, under t0 0.15 -> nibble 2
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pWouldChange) == 1 && partField(0, pWouldDrop) == 0, "part: at k = 1.25 the nibble moves 1 -> 2");
    f.setOut(0, 1);   // the engine says nibble 0: the recompute disagrees
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pMismatch) == 1 && partField(0, pWouldChange) == 0, "part: a wrong nibble is a disagreement, never shadowed");
    f.setOut(0, 0);   // an engine reject
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[2], true);
    check(partField(2, pSeen) == 1 && partField(2, pPassed) == 0, "part: an engine reject in another view is seen, not passed");
    f.setOut(1, 1);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], false);
    check(delta(cPartForeign) == 1 && partField(0, pSeen) == 0, "part: another caller's test is counted apart");
    void* hole = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    mark();
    lodGovernorPartObserver(reinterpret_cast<uintptr_t>(hole), f.outAt(), f.view[0], true);
    check(delta(cPartFaults) == 1 && delta(cParts) == 1, "part: a wild block is a fault, not a crash");
    // The record test.
    setK(1.0f);
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(delta(cRecords) == 1 && recField(0, rSeen) == 1 && recField(1, rSeen) == 1 && recField(0, rMismatch) == 0 &&
          recField(1, rWouldFail) == 0 && delta(cRecDispatchMismatch) == 0 && delta(cRecNotDispatched) == 0,
          "record: both eyes reproduced at k = 1, dispatched");
    check(fromBits(g_scaleBits.load()) == 1.0f, "record: the LOD scale s is published");
    setK(2.0f);
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(recField(0, rWouldFail) == 1 && recField(1, rWouldFail) == 1 && delta(cRecNotDispatched) == 0,
          "record: at k = 2 both eyes fail; the orthographic view keeps it dispatched");
    f.setMask((1ull << 1) | (1ull << 22));
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(delta(cRecNotDispatched) == 1, "record: eyes only, at k = 2 the builder would not be called");
    f.setNibble(22, 0);
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(recField(1, rMismatch) == 1 && recField(0, rMismatch) == 0, "record: a wrong stored nibble is a disagreement");
    f.setNibble(22, 1);
    f.setMask(1ull << 1);
    put<uint16_t>(f.node, 0x6A, 0);   // nibble 1 past the node's LOD count 0: the engine would not dispatch
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(delta(cRecDispatchMismatch) == 1, "record: a record the dispatch rule would not build is a disagreement");
    put<uint16_t>(f.node, 0x6A, 3);
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, reinterpret_cast<uintptr_t>(hole) + 0x210);
    check(delta(cRecordFaults) == 1 && delta(cRecords) == 1, "record: a wild record is a fault, still a builder call");
    VirtualFree(hole, 0, MEM_RELEASE);
    g_live.store(false);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(delta(cParts) == 0 && delta(cRecords) == 0, "observers: off, they count nothing");
    setK(1.0f);
}

void caseThreads() {
    Fake f;
    g_live.store(true);
    setK(2.0f);
    mark();
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&f] {
            for (int k = 0; k < 50000; ++k) lodGovernorPartObserver(f.items(), f.outAt(), f.view[2], true);
        });
    for (auto& t : ts) t.join();
    check(delta(cParts) == 200000 && partField(2, pPassed) == 200000,
          "threads: four workers' owner-only counters sum exactly at the boundary");
    g_live.store(false);
    setK(1.0f);
}

// A runtime frame as the graphics half's snapshot holds it, with frame work
// ms. Version 5 (the default): caller work per cycle ms, beside an app work
// 6 ms lower -- the first shadow flight's shape (8.4 app, 14.4 caller) -- so a
// check that reads ms proves the governor read the caller work. Versions 3
// and 4 carry no caller work (publishCpu zeroes what an older frame lacks):
// the app work is ms; version 3 has no base rate either.
void setTiming(uint64_t seq, uint64_t now, double ms, uint32_t version = EDVR_NATIVE_TIMING_VERSION_5) {
    g_timing = NativeTimingSnapshot{};
    g_timing.active = g_timing.haveCpu = g_timing.applicationValid = true;
    g_timing.generation = 1;
    g_timing.firstSequence = 1;
    g_timing.sequence = seq;
    g_timing.capturedAtMs = now;
    g_timing.predictedPeriodMs = 1000.0 / 90.0;
    g_timing.cpu.version = version;
    g_timing.cpu.sequence = seq;
    if (version >= EDVR_NATIVE_TIMING_VERSION_5) {
        g_timing.cpu.size = sizeof(g_timing.cpu);
        g_timing.cpu.callerWorkMs = ms;
        g_timing.cpu.callerWorkValid = 1;
        g_timing.applicationMs = ms > 6.0 ? ms - 6.0 : 0.0;
    } else {
        g_timing.cpu.size = version >= EDVR_NATIVE_TIMING_VERSION_4 ? EDVR_NATIVE_TIMING_FRAME_SIZE_4
                                                                    : EDVR_NATIVE_TIMING_FRAME_SIZE_3;
        g_timing.applicationMs = ms;
    }
    if (version >= EDVR_NATIVE_TIMING_VERSION_4) g_timing.cpu.baseDisplayHz = 90.0f;
}

void caseBoundary() {
    Fake f;
    uint64_t t = 50000, seq = 10;
    auto frames = [&](uint32_t n, uint32_t records, uint32_t parts, double ms,
                      uint32_t version = EDVR_NATIVE_TIMING_VERSION_5) {
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t r = 0; r < records; ++r)
                if (g_obsBuilder) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
            for (uint32_t p = 0; p < parts; ++p)
                if (g_obsPart) g_obsPart(f.items(), f.outAt(), f.view[p & 1], true);
            setTiming(++seq, t, ms, version);
            frameBoundaryAt(t);
            t += 11;
        }
    };
    // game: one off line, and then nothing at all.
    size_t at = g_lines.size();
    applyConfig("game", 2.0f, t);
    check(logged("settlement detail: off (fix.settlement_detail = game", at) && !g_live.load() && g_attaches == 0,
          "boundary: game logs one off line and attaches nothing");
    at = g_lines.size();
    frames(3000, 250, 20, 12.5);
    check(g_lines.size() == at, "boundary: while off, 33 s of frames log nothing (never ran reads as silence)");
    applyConfig("game", 2.0f, t);
    check(g_lines.size() == at, "boundary: the same value again is not re-logged");
    // auto: the configure line, then the ramp. No runtime frame is published
    // yet, so the configure line cannot name the signal and says who will.
    g_timing = NativeTimingSnapshot{};
    at = g_lines.size();
    applyConfig("auto", 2.0f, t);
    check(g_live.load() && g_attaches == 1 && g_obsBuilder && g_obsPart, "boundary: auto attaches the observers");
    check(logged("settlement detail: shadow governor on (auto: shadow, never acts)", at) &&
              logged("per-part test FUN_1442B3FC0 hooked", at),
          "boundary: the configure line says shadow, never acts, and the hooks");
    check(logged("frame work = caller work per cycle if the runtime sends it (timing v5), else app work (pre-submit "
                 "only; host older); the first runtime frame decides and a line names it", at),
          "boundary: before any runtime frame the configure line says the first frame names the signal");
    at = g_lines.size();
    frames(2800, 250, 20, 12.5);   // 30.8 s, 250 records a frame; caller work 12.5 ms over budget, app work 6.5 under
    check(countLogged("settlement detail: frame work = caller work per cycle (runtime timing v5: the caller thread "
                      "from one pose wait's return to the next one's entry", at) == 1,
          "boundary: the first version 5 frame names the signal, once");
    check(countLogged("settlement detail (shadow, never acts): k 1.00 -> 1.05, up", at) == 1,
          "boundary: the first step is logged");
    check(countLogged("settlement detail (shadow, never acts): k ", at) >= 4 &&
              countLogged("settlement detail (shadow, never acts): k ", at) <= 7,
          "boundary: step lines are rate-limited to one per 5 s");
    check(logged("steps since the last line", at), "boundary: a rate-limited line counts the steps it skipped");
    check(logged("250 builder records, frame work = caller work per cycle: 12.50 ms vs period 11.11 ms", at),
          "boundary: a step line names the signal it stepped on");
    check(logged("k now 2.00, effective s x k 2.000 (window 1.00..2.00 of max 2.00; 20 up", at),
          "boundary: the summary's k, effective s x k, range and steps");
    check(logged("builder records/frame 250.0 (max 250", at) && logged("part tests/frame 20.0", at),
          "boundary: the summary's density means");
    check(logged("frame work = caller work per cycle: 12.50 ms mean vs period 11.11 ms", at) &&
              logged("invalid 0, caller work absent 0)", at),
          "boundary: the summary's frame work is the caller work (not the 6.50 ms app work beside it) vs the period");
    check(logged("eye views bits 1 / 22", at), "boundary: the summary names the eyes");
    check(logged("settlement detail (shadow) eye A (view bit 1 now): parts tested 10.0/frame, engine passed 10.0", at) &&
              logged("settlement detail (shadow) eye B (view bit 22 now)", at),
          "boundary: one line per eye with its part counts");
    check(logged("records passed 250.0/frame", at) && logged("disagreements with the engine at k = 1: parts 0, records 0", at),
          "boundary: the eye lines carry the record test and the disagreements");
    check(logged("settlement detail (shadow) other views:", at) && logged("would not be called for at all", at),
          "boundary: the other-views line");
    check(!logged("k stayed 1", at), "boundary: a window that stepped does not say k stayed 1");
    // The effective LOD scale is the game's s times k, not k: at the slider's
    // floor the engine holds s = 1.5, and k 2 there is s x k 3 (the LOD note's
    // table is keyed by it). The k = 1 recompute disagrees with the fake's
    // verdicts at this s; only the header is read here.
    put(f.ctx, 0x30, 1.5f);
    at = g_lines.size();
    frames(2800, 250, 20, 12.5);
    check(logged("k now 2.00, effective s x k 3.000 (window 2.00..2.00", at) &&
              logged("LOD scale s (ctx+0x30) 1.500", at),
          "boundary: the summary prints the effective s x k beside k, from the engine's s");
    check(countLogged("settlement detail: frame work = ", at) == 0,
          "boundary: the signal line is not repeated while the runtime's version holds");
    put(f.ctx, 0x30, 1.0f);
    // A lowered k_max: logged again, clamps at the next frame.
    at = g_lines.size();
    applyConfig("auto", 1.5f, t);
    frames(1, 250, 0, 12.5);
    check(logged("k in [1, 1.50]", at) && logged("clamped to the new advanced.settlement_detail_max", at),
          "boundary: a lowered k_max re-logs the configure line and clamps");
    check(logged("; frame work = caller work per cycle (runtime timing v5: the caller thread", at),
          "boundary: once a runtime frame has said which, the configure line names the signal");
    // reduced: reserved, behaves as auto, says so.
    at = g_lines.size();
    applyConfig("reduced", 1.5f, t);
    check(logged("reduced is reserved and behaves as auto in this build: shadow, never acts", at) && g_attaches == 1,
          "boundary: reduced is logged as reserved and behaves as auto");
    // Off mid-window: the partial summary, then the off line.
    at = g_lines.size();
    applyConfig("game", 1.5f, t);
    check(!g_live.load() && g_detaches == 1 && !g_obsBuilder && logged("settlement detail (shadow, never acts): ", at) &&
              logged("settlement detail: off", at),
          "boundary: switching off logs the partial window and detaches");
    // On again with nothing built: k stuck at 1, and the summary says why --
    // and, with no LOD scale ever read, that s x k is unknown.
    g_scaleBits.store(0);
    at = g_lines.size();
    applyConfig("auto", 2.0f, t);
    frames(2800, 0, 0, 12.5);
    check(logged("k stayed 1: no draw-item builder calls", at) && !logged("eye A (view bit", at),
          "boundary: no builder calls -> the header says k stayed 1 and no eye lines");
    check(logged("k now 1.00, effective s x k unknown (no LOD scale read yet) (window", at),
          "boundary: no LOD scale read -> the effective s x k says unknown, not 0");
    at = g_lines.size();
    frames(2800, 250, 20, 10.5);   // dense, inside the margin
    check(logged("k stayed 1: the frame work never ran 0.30 ms over the period", at), "boundary: within budget -> why k stayed 1");
    applyConfig("game", 2.0f, t);
    // A runtime whose newest frame is invalid: every boundary breaks the runs.
    applyConfig("auto", 2.0f, t);
    at = g_lines.size();
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        setTiming(++seq, t, 12.5);
        g_timing.invalid = true;
        frameBoundaryAt(t);
        t += 11;
    }
    check(logged("over 0 samples", at) && logged("invalid 2729, caller work absent 0)", at) &&
              logged("k stayed 1: no valid frame-work sample from the native runtime", at),
          "boundary: invalid runtime frames are counted and hold k at 1");
    applyConfig("game", 2.0f, t);
    // THE DEFECT the caller work fixes -- the first shadow flight (2026-09-23
    // 07:25 UTC, parked at the settlement, 45 fps): app work 8.37-8.67 ms,
    // caller work 14.4 ms (cycle 21.99 - next wait 7.57), period 11.11. On the
    // app figure k never left 1; on the caller work, the same frames step up.
    applyConfig("auto", 2.0f, t);
    at = g_lines.size();
    frames(2800, 250, 20, 14.4);   // version 5: caller 14.4 ms, app 8.4 ms beside it
    check(logged("frame work = caller work per cycle: 14.40 ms mean vs period 11.11 ms", at) &&
              logged("settlement detail (shadow, never acts): k 1.00 -> 1.05, up", at) && !logged("k stayed 1", at),
          "boundary: the first flight's frames (caller 14.4 ms, app 8.4 ms) step k up on the caller work");
    applyConfig("game", 2.0f, t);
    // A version 5 frame whose runtime could not close the cycle before it
    // carries no caller work: an invalid sample, never the app work beside it
    // (the two figures are never mixed in one run).
    applyConfig("auto", 2.0f, t);
    at = g_lines.size();
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        setTiming(++seq, t, 14.4);
        g_timing.cpu.callerWorkValid = 0;
        g_timing.cpu.callerWorkMs = 0;
        frameBoundaryAt(t);
        t += 11;
    }
    check(logged("over 0 samples (over by > 0.30 ms: 0, under by > 1.00 ms: 0, invalid 2729, caller work absent 2729)",
                 at) &&
              logged("k stayed 1: no valid frame-work sample from the native runtime", at),
          "boundary: a version 5 frame without caller work is an invalid sample, counted as such");
    applyConfig("game", 2.0f, t);
    // An older runtime (timing v4): no caller work crosses, the app work
    // stands in, and every line says so. At the first flight's 8.4 ms app
    // work it holds k at 1 -- the defect, reproduced on the fallback.
    applyConfig("auto", 2.0f, t);
    at = g_lines.size();
    frames(2800, 250, 20, 8.4, EDVR_NATIVE_TIMING_VERSION_4);
    check(countLogged("settlement detail: frame work = app work (pre-submit only; host older: runtime timing v4 sends "
                      "no caller work", at) == 1,
          "boundary: a version 4 runtime is named once, as the fallback");
    check(logged("frame work = app work (pre-submit only; host older): 8.40 ms mean vs period 11.11 ms", at) &&
              logged("k stayed 1: the frame work never ran 0.30 ms over the period", at),
          "boundary: the fallback's summary names the app work; at the first flight's 8.4 ms it holds k at 1");
    at = g_lines.size();
    applyConfig("auto", 1.5f, t);
    check(logged("; frame work = app work (pre-submit only; host older: runtime timing v4 sends no caller work", at),
          "boundary: the configure line names the fallback once a runtime frame has said which");
    applyConfig("game", 2.0f, t);
    // Version 3: no base rate either -- the session's first predicted period
    // is the budget -- and an over-budget app work still steps.
    applyConfig("auto", 2.0f, t);
    at = g_lines.size();
    frames(2800, 250, 20, 12.5, EDVR_NATIVE_TIMING_VERSION_3);
    check(countLogged("runtime timing v3 sends no caller work", at) == 1 &&
              logged("frame work = app work (pre-submit only; host older): 12.50 ms mean vs period 11.11 ms", at) &&
              logged("frame work = app work (pre-submit only; host older): 12.50 ms vs period 11.11 ms", at),
          "boundary: a version 3 runtime falls back to the app work against the first predicted period, and steps");
    applyConfig("game", 2.0f, t);
    // An attach refusal: off, said so, nothing counted.
    at = g_lines.size();
    g_attachResult = "opcode_mismatch";
    applyConfig("auto", 2.0f, t);
    check(!g_live.load() && logged("could not attach its engine hooks (opcode_mismatch)", at),
          "boundary: a refused attach is logged and stays off");
    g_attachResult = "installed";
    at = g_lines.size();
    applyConfig("fast", 2.0f, t);
    check(logged("fix.settlement_detail = \"fast\" is not game, auto or reduced", at) && !g_live.load(),
          "boundary: an unknown value is named and treated as game");
    applyConfig("game", 2.0f, t);
    // The configure sweep with both keys unset (the stub Config answers every
    // default): off, and the ceiling is the compiled 4.0.
    at = g_lines.size();
    lodGovernorConfigure(Config::get());
    check(!g_live.load() && g_state.kMaxCfg == 4.0f && g_state.policy.kMax() == 4.0f && g_lines.size() == at,
          "boundary: with the keys unset the governor stays off and k_max is the compiled default 4.0");
    applyConfig("auto", lodgov::kDefaultMax, t);
    check(logged("k in [1, 4.00]", at), "boundary: the default ceiling reads k in [1, 4.00] in the configure line");
    applyConfig("game", lodgov::kDefaultMax, t);
}

// The cost the design doc quotes: both observers on hot synthetic memory.
void caseCost() {
    Fake f;
    g_live.store(true);
    g_ctx.store(f.ctxAt());
    identifyEyes(g_state);
    setK(1.5f);
    const int n = 2000000;
    LARGE_INTEGER q0, q1, fr;
    QueryPerformanceFrequency(&fr);
    for (int i = 0; i < 10000; ++i) lodGovernorPartObserver(f.items(), f.outAt(), f.view[i & 1], true);
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < n; ++i) lodGovernorPartObserver(f.items(), f.outAt(), f.view[i & 1], true);
    QueryPerformanceCounter(&q1);
    const double partNs = double(q1.QuadPart - q0.QuadPart) * 1e9 / double(fr.QuadPart) / n;
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < n / 10; ++i) lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    QueryPerformanceCounter(&q1);
    const double recordNs = double(q1.QuadPart - q0.QuadPart) * 1e9 / double(fr.QuadPart) / (n / 10);
    uint32_t sums[kCounters];
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < 1000; ++i) sumSlots(sums);
    QueryPerformanceCounter(&q1);
    const double boundaryUs = double(q1.QuadPart - q0.QuadPart) * 1e6 / double(fr.QuadPart) / 1000;
    std::printf("lod_governor_test: cost on this machine, hot synthetic memory: part observer %.1f ns/call, record "
                "observer %.1f ns/call (3 views), the boundary's slot sum %.2f us (%u slots claimed)\n",
                partNs, recordNs, boundaryUs, g_slotNext.load());
    check(partNs < 500.0 && recordNs < 2000.0, "cost: the observers are sub-microsecond");
    g_live.store(false);
    setK(1.0f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("lod_governor_test: dry-run (no hooks, no game, no files)\n");
        return 0;
    }
    if (argc < 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: lod_governor_test.exe --self-test | --dry-run\n");
        return 2;
    }
    casePolicy();
    caseMath();
    caseObservers();
    caseThreads();
    caseBoundary();
    caseCost();
    // Log::note (src/common/log.cpp) formats into 1200 bytes after a 15-byte
    // timestamp and reserves 18 more (the truncation marker and the line end):
    // 1166 characters is the longest message that reaches the log whole. The
    // stub above has room for more, so check here.
    size_t longest = 0;
    for (const std::string& line : g_lines)
        if (line.size() > longest) longest = line.size();
    std::printf("lod_governor_test: longest log line %zu characters (the real log keeps 1166)\n", longest);
    check(longest <= 1166, "log: every line fits the real log's line whole (no truncation marker in flight)");
    // --print-log: the captured log lines, for reading what a flight will print.
    if (argc > 2 && std::strcmp(argv[2], "--print-log") == 0)
        for (const std::string& line : g_lines) std::printf("  log | %s\n", line.c_str());
    std::printf("lod_governor_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
