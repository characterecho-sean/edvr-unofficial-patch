// Build gate for the settlement LOD governor (src/d3d11/lod_governor.*,
// fix.settlement_detail, shipped default auto): the policy's steps and
// hysteresis on synthetic frame sequences, auto on the display slot (the
// interval between boundaries; longer than 1.5 periods took two) over the
// latest 30 samples -- up while 3 or more took two slots with the CPU at the
// period (0.25 while the 30's mean caller work ran more than 1.0 ms over,
// else 0.05), two-slot cycles with the CPU under counted apart, the kick to
// k_max once that has held 3 s (held 2 s, one per 30 s), down 0.05 per 5 s
// only after 5 s with no two-slot cycle and a millisecond to spare, the
// lever-spent line at k_max, k_max 6 by default and 8 at most -- and
// reduced's k_max-at-once, never a kick; the cockpit gate (on foot, per the journal
// watcher's Status.json, k is 1 at once and held; one line per transition,
// the frames counted in the summary); the engine arithmetic
// the shadow repeats (FUN_1442B3FC0 / FUN_144308B30's distance, LOD distance,
// LOD pick and the part test's screen-size term); the two observers on
// synthetic engine memory laid out as the decompiles read it -- a render
// context with two eye views and an orthographic one, the builder's
// six-pointer block around a part test, a record with its node and LOD table
// -- including a wrong nibble, an engine reject, a foreign caller and a wild
// pointer; the LOD-scale setter's bracket (FUN_142819D90) against a fake
// context and a fake setter that stores 1 + (1 - x) as the engine does: a
// context the builder never used is never written, the game's value is
// adopted and scaled by k, k back to 1 leaves the engine's own value, a
// slider change is taken as it comes, an implausible value is left alone,
// observe never writes, the table's limit and an unwritable page stand
// acting down, and switching off writes the game's value back; the acting
// counts (an engine reject the game's setting would have kept, with the
// plane test run, failed or missing; a level changed from the game's); the
// per-thread counters under four threads; and the frame boundary end to end
// against a stub native timing feed, a fake engine that runs its tests at
// whatever LOD scale the context holds, and a capturing log: the configure
// line naming the mode and the mechanism, the first-write line, the step
// lines with the LOD scale they will write, the 30-second summaries with the
// setter's counts, NOT ACTING when the setter never ran, "k stayed 1", and
// nothing at all while off. The frame work is the runtime's caller work per
// cycle (timing v5), never the app work beside it; a v3/v4 runtime falls back
// to the app work, named as such in every line; a v5 frame without caller
// work is an invalid sample. Every line fits the real log's line. It then
// times the observers (the cost the design doc quotes). No hooks, no game.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// The implementation under test is compiled INTO this TU, so the rig reaches
// its anonymous-namespace internals (applyConfig, frameBoundaryAt, the
// counter slots, the context table) -- the static_prop_gate_test pattern.
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
bool Config::getBool(const char*, bool def) const { return def; }
NativeTimingSnapshot g_timing;
NativeTimingSnapshot nativeTimingSnapshot() noexcept { return g_timing; }
const char* g_attachResult = "installed";
const char* g_partStatusStub = "hooked";
const char* g_setterStatusStub = "hooked";
LodGovernorBuilderFn g_obsBuilder = nullptr;
LodGovernorPartFn g_obsPart = nullptr;
LodGovernorSetterFn g_obsSetter = nullptr;
int g_attaches = 0, g_detaches = 0;
void kinematicEvalSetLodGovernorObservers(LodGovernorBuilderFn b, LodGovernorPartFn p, LodGovernorSetterFn s) noexcept {
    g_obsBuilder = b;
    g_obsPart = p;
    g_obsSetter = s;
}
const char* kinematicEvalLodGovernorAttach() noexcept { ++g_attaches; return g_attachResult; }
void kinematicEvalLodGovernorDetach() noexcept { ++g_detaches; }
bool kinematicEvalBuilderHooked() noexcept { return true; }
const char* kinematicEvalPartTestStatus() noexcept { return g_partStatusStub; }
const char* kinematicEvalLodSetterStatus() noexcept { return g_setterStatusStub; }
// The engine's plane test, faked: its verdict is whatever the case sets.
uint64_t g_planeVerdict = 1;   // 1 inside every plane, 0xFFFFFFFF outside one
uint64_t __fastcall fakePlanes(uintptr_t, const float*, const float*) { return g_planeVerdict; }
LodGovernorFrustumFn g_frustumStub = &fakePlanes;
LodGovernorFrustumFn kinematicEvalFrustumFn() noexcept { return g_frustumStub; }
// The journal watcher, faked (journal_watch.h): on foot is whatever the case
// sets; with the watcher inactive nothing is known, as in journal_watch.cpp.
bool g_journalActive = true, g_onFoot = false;
bool journalWatchActive() { return g_journalActive; }
bool journalOnFootKnown() { return g_journalActive; }
bool journalOnFoot() { return g_onFoot; }
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
// One policy, fed as the boundary feeds it: each frame's cycle moves the
// clock and the wall time together (the boundary cases below keep 11 ms of
// wall time a frame instead). A cycle of kTwo took two display slots.
constexpr double kP = 1000.0 / 90.0, kTwo = 2.0 * kP;
struct Feed {
    lodgov::Policy p;
    double clock = 1000.0;
    uint64_t now() const { return uint64_t(clock); }
    // The wall time the next frame of this cycle would carry.
    uint64_t next(double cycleMs) const { return uint64_t(clock + cycleMs); }
    lodgov::Step frame(double workMs, double cycleMs, uint32_t records = 679,
                       lodgov::Work work = lodgov::Work::Valid, bool onFoot = false) {
        clock += cycleMs;
        lodgov::FrameSignals s;
        s.records = records;
        s.onFoot = onFoot;
        s.work = work;
        s.workMs = workMs;
        s.periodMs = kP;
        s.nowMs = uint64_t(clock);
        s.clockMs = clock;
        return p.update(s);
    }
};

void casePolicy() {
    using namespace lodgov;
    {
        Policy p;
        // The default ceiling is 6.0: k multiplies the s the game holds -- 1.0
        // at the slider's default, 1.5 at its floor -- and the removal levels
        // off between an effective s x k of 4.5 and 6.
        check(kDefaultMax == 6.0f && kMaxCeiling == 8.0f && p.maxSteps() == 100 && p.kMax() == 6.0f && p.k() == 1.0f,
              "policy: the default k_max is 6.0 (a hundred steps of 0.05, exactly), the ceiling 8; k starts at 1");
        p.configure(2.0f);
        check(p.maxSteps() == 20 && p.k() == 1.0f && p.kMax() == 2.0f,
              "policy: k_max 2.0 is twenty steps of 0.05; k starts at 1");
    }
    Feed f;
    f.p.configure(2.0f);
    // The display slot: the first boundary has no interval; after it every
    // cycle is measured, and one longer than 1.5 periods took two slots.
    f.frame(kP, kP, 150);
    check(!f.p.cycle().measured, "policy: the first boundary after enabling has no interval");
    f.frame(kP, 1.49 * kP, 150);
    check(f.p.cycle().measured && !f.p.cycle().missed && std::fabs(f.p.cycle().ms - 1.49 * kP) < 1e-6,
          "policy: a cycle of 1.49 periods is measured and fits its slot");
    f.frame(kP, 1.51 * kP, 150);
    check(f.p.cycle().missed && !f.p.cycle().cpuUnder,
          "policy: 1.51 periods took two slots, the CPU's (its caller work at the period)");
    f.frame(kP - 1.0, kTwo, 150);
    check(f.p.cycle().missed && f.p.cycle().cpuUnder,
          "policy: two slots with the caller work 1 ms under the period: not the CPU's");
    bool any = false;
    for (int i = 0; i < 300; ++i) any |= f.frame(14.0, kTwo, 150) != Step::None;
    check(!any && f.p.steps() == 0 && !f.p.inSettlement(), "policy: two-slot cycles with 150 records never rise");
    // A bad sample empties the ring, and the next boundary has no interval.
    f.frame(0, kP, 150, Work::Invalid);
    check(f.p.samples() == 0 && f.p.misses() == 0 && !f.p.triggered(), "policy: an invalid sample empties the ring");
    f.frame(12.0, kTwo);
    check(!f.p.cycle().measured && f.p.samples() == 1 && f.p.misses() == 0,
          "policy: ... and the boundary after it has no interval");
    // The ring must hold 30 before a step.
    for (int i = 0; i < 28; ++i) any |= f.frame(12.0, kTwo) != Step::None;
    check(!any && f.p.samples() == 29 && f.p.misses() == 28 && f.p.steps() == 0,
          "policy: 29 samples, 28 of them two slots, do not step: the ring must hold 30");
    check(f.frame(12.0, kTwo) == Step::Up && f.p.steps() == 1 && f.p.k() == 1.05f && f.p.upQuanta() == 1 &&
              !f.p.upHeld() && f.p.stepMisses() == 29,
          "policy: the 30th steps up by 0.05 (29 of 30 took two slots, the CPU's; 12.0 ms is 0.89 over: fine)");
    // At most one step up a second.
    const uint64_t firstUp = f.now();
    int ups = 0;
    while (f.next(kTwo) < firstUp + 1000) ups += f.frame(12.0, kTwo) == Step::Up;
    check(ups == 0, "policy: no second step inside the one-second ramp interval");
    Step s = Step::None;
    while (s == Step::None && f.now() < firstUp + 1100) s = f.frame(12.0, kTwo);
    check(s == Step::Up && f.p.steps() == 2 && f.now() >= firstUp + 1000, "policy: the next step a second later");
    // A frame with no new sample holds the ring; its cycle is still measured.
    f.frame(0, kP, 679, Work::None);
    check(f.p.samples() == 30 && f.p.cycle().measured, "policy: a frame without a sample holds the ring, its cycle measured");
    // Two-slot cycles with the caller work 1 ms under the period: counted
    // apart, never the trigger.
    f.frame(0, kP, 679, Work::Invalid);
    for (int i = 0; i < 60; ++i) any |= f.frame(kP - 1.0, kTwo) != Step::None;
    check(!any && f.p.samples() == 30 && f.p.misses() == 0 && f.p.cpuUnderMisses() == 30 && !f.p.triggered() &&
              f.p.steps() == 2,
          "policy: two-slot cycles with the caller work 1 ms under the period never trigger: not the CPU's");
    // Every cycle in its slot, the caller work 0.29 ms over: hold.
    for (int i = 0; i < 900; ++i) any |= f.frame(11.40, kP) != Step::None;   // 10 s
    check(!any && f.p.steps() == 2 && f.p.misses() == 0 && f.p.samples() == 30,
          "policy: every cycle in its slot and no millisecond to spare: k holds");
    // 2 of the last 30 cycles taking two slots hold; the 3rd steps.
    any |= f.frame(12.0, kTwo) != Step::None;
    any |= f.frame(12.0, kTwo) != Step::None;
    check(!any && f.p.misses() == 2 && f.p.steps() == 2, "policy: 2 of the last 30 cycles taking two slots hold (the dead band)");
    check(f.frame(12.0, kTwo) == Step::Up && f.p.steps() == 3 && f.p.stepMisses() == 3 && f.p.upQuanta() == 1,
          "policy: the 3rd steps up, 0.05 (the 30's mean 0.35 ms over)");
    // 150-199 records: no rise, and the settlement held.
    for (int i = 0; i < 100; ++i) any |= f.frame(12.0, kTwo, 180) != Step::None;
    check(!any && f.p.steps() == 3 && f.p.inSettlement(), "policy: 150-199 records hold k and the settlement");
    // The trigger has held since the 3rd: dense again, one more step, then
    // at 3 s the kick to k_max at once.
    ups = 0;
    s = Step::None;
    for (int i = 0; i < 200 && s != Step::Kick; ++i) {
        s = f.frame(12.0, kTwo);
        ups += s == Step::Up;
    }
    check(s == Step::Kick && ups == 1 && f.p.steps() == 20 && f.p.k() == 2.0f && f.p.kickHeldMs() >= 3000 &&
              f.p.kickHeldMs() < 3030,
          "policy: the trigger held 3 s below k_max: a kick to k_max, at once");
    for (int i = 0; i < 100; ++i) any |= f.frame(12.0, kTwo) != Step::None;
    check(!any && f.p.steps() == 20, "policy: at k_max nothing steps further");
    // Every cycle in its slot, the caller work only 0.61 ms under: hold.
    for (int i = 0; i < 900; ++i) any |= f.frame(10.5, kP) != Step::None;   // 10 s
    check(!any && f.p.steps() == 20, "policy: no cycle taking two slots but no millisecond to spare holds k");
    // 2.11 ms to spare, no two-slot cycle for over 5 s: down 0.05, then no
    // sooner than 5 s again.
    s = Step::None;
    for (int i = 0; i < 60 && s == Step::None; ++i) s = f.frame(9.0, kP);
    check(s == Step::Down && f.p.steps() == 19 && f.p.stepMeanExcessMs() < -1.0 && f.p.stepCleanMs() >= 5000,
          "policy: no cycle taking two slots for 5 s and a millisecond to spare: down 0.05");
    const uint64_t downAt = f.now();
    int downs = 0;
    while (f.next(kP) < downAt + 5000) downs += f.frame(9.0, kP) == Step::Down;
    check(downs == 0, "policy: the next step down no sooner than 5 s later");
    s = Step::None;
    while (s == Step::None && f.now() < downAt + 5100) s = f.frame(9.0, kP);
    check(s == Step::Down && f.p.steps() == 18, "policy: ... and then 0.05 again");
    // A lowered k_max clamps at once.
    f.p.configure(1.5f);
    check(f.frame(11.0, kP) == Step::Clamp && f.p.steps() == 10, "policy: a lowered k_max clamps k at once");
    // Leaving the settlement: 29 sparse frames hold, the 30th resets k to 1,
    // and the boundary after it has no interval.
    Step last = Step::None;
    for (int i = 0; i < 29; ++i) last = f.frame(12.0, kP, 20);
    check(last == Step::None && f.p.steps() == 10 && f.p.inSettlement(), "policy: 29 frames under 150 records hold");
    check(f.frame(12.0, kP, 20) == Step::Reset && f.p.steps() == 0 && !f.p.inSettlement(),
          "policy: the 30th frame under 150 records resets k to 1");
    f.frame(12.0, kP, 20);
    check(!f.p.cycle().measured, "policy: the boundary after leaving the settlement has no interval");
    f.p.configure(std::numeric_limits<float>::quiet_NaN());
    check(f.p.maxSteps() == 0, "policy: a NaN k_max holds k at 1");
    f.p.configure(9.0f);
    check(f.p.maxSteps() == 140 && f.p.kMax() == 8.0f, "policy: k_max is held to 8");
    f.p.configure(6.0f);
    check(f.p.maxSteps() == 100 && f.p.kMax() == 6.0f, "policy: k_max 6.0 is exactly 100 steps");
    f.p.configure(1.26f);
    check(f.p.maxSteps() == 5, "policy: k_max is quantised to the 0.05 step");
}

// auto on the display slot (refinement 4, after the 05:05 flight): the
// trigger is 3 or more of the last 30 cycles taking two slots, the CPU's;
// up at most once a second, 0.25 while the 30's mean caller work ran more
// than 1.0 ms over, else 0.05; the kick to k_max once the trigger has held
// 3 s below it (held 2 s, one per 30 s); down 0.05 only after 5 s with no
// cycle taking two slots and 1 ms to spare, then no sooner than 5 s; the
// lever spent at k_max with the trigger for 5 s.
void caseStepPolicy() {
    using namespace lodgov;
    // 90 Hz, 30% of the cycles taking two slots (spread evenly: 9 in any 30),
    // the caller work at the period: up once a second, 0.05 each -- and once
    // the trigger has held 3 s, the kick; at k_max 5 s more, the lever spent.
    {
        Feed f;
        std::vector<uint64_t> upsAt;
        int fineUps = 0, otherUps = 0, downs = 0;
        uint64_t kickAt = 0, triggerAt = 0;
        float kickFrom = 0;
        for (int i = 0; i < 700; ++i) {
            const bool two = std::floor((i + 1) * 0.3) > std::floor(i * 0.3);
            const float k0 = f.p.k();
            const Step s = f.frame(kP, two ? kTwo : kP);
            if (!triggerAt && f.p.triggered()) triggerAt = f.now();
            if (s == Step::Up) {
                upsAt.push_back(f.now());
                (f.p.upQuanta() == 1 ? fineUps : otherUps)++;
            }
            if (s == Step::Kick && !kickAt) {
                kickAt = f.now();
                kickFrom = k0;
            }
            if (s == Step::Down) ++downs;
        }
        bool paced = upsAt.size() >= 2;
        for (size_t i = 1; i < upsAt.size(); ++i)
            paced &= upsAt[i] - upsAt[i - 1] >= 1000 && upsAt[i] - upsAt[i - 1] <= 1025;
        check(upsAt.size() == 3 && fineUps == 3 && otherUps == 0 && paced && downs == 0,
              "steps: 30% of cycles taking two slots, the caller work at the period: up 0.05 once a second");
        check(kickAt && kickAt - triggerAt >= 3000 && kickAt - triggerAt < 3030 && kickFrom == 1.15f && f.p.k() == 6.0f,
              "steps: ... and the trigger held 3 s: the kick, from 1.15 to k_max 6.00 at once");
        check(f.p.ceilingMissing(f.now()), "steps: ... and at k_max with the trigger for 5 s the lever is spent");
        std::printf("lod_governor_test: 30%% of 90 Hz cycles taking two slots, the caller work at the period: %zu steps "
                    "of 0.05 a second apart, then the kick from %.2f to %.2f at %.2f s of the trigger\n",
                    upsAt.size(), double(kickFrom), double(f.p.k()), double(kickAt - triggerAt) / 1000.0);
    }
    // Two-slot cycles with the caller work 1 ms under the period: counted
    // as the CPU under, never the trigger.
    {
        Feed f;
        bool any = false;
        for (int i = 0; i < 1000; ++i) any |= f.frame(kP - 1.0, i % 3 == 0 ? kTwo : kP) != Step::None;
        check(!any && f.p.steps() == 0 && f.p.misses() == 0 && f.p.cpuUnderMisses() == 10 && !f.p.triggered(),
              "steps: two-slot cycles with the caller work 1 ms under the period do not trigger (the CPU under)");
    }
    // Every cycle two slots at 12.9 ms (1.79 over): 0.25 once a second, and
    // at 3 s the kick. Then with 1.2 ms to spare: 4.9 s with no cycle taking
    // two slots holds, 5.0 s steps down once, the next not before 5 s more;
    // only 0.5 ms to spare holds.
    {
        Feed f;
        const uint64_t t0 = f.now();
        int coarseUps = 0;
        uint64_t kickAt = 0;
        float kickFrom = 0;
        for (int i = 0; i < 300; ++i) {
            const float k0 = f.p.k();
            const Step s = f.frame(12.9, kTwo);
            if (s == Step::Up && f.p.upQuanta() == kCoarseQuanta) ++coarseUps;
            if (s == Step::Kick && !kickAt) {
                kickAt = f.now();
                kickFrom = k0;
            }
        }
        check(coarseUps == 3 && kickFrom == 1.75f && f.p.steps() == 100,
              "steps: every cycle two slots at 12.9 ms: 1.25, 1.50, 1.75 a second apart, then the kick to 6.00");
        std::printf("lod_governor_test: every 90 Hz cycle two slots at 12.9 ms: %d steps of 0.25, then the kick from %.2f "
                    "to 6.00 at %.2f s\n", coarseUps, double(kickFrom), double(kickAt - t0) / 1000.0);
        const uint64_t lastMiss = f.now();
        bool any = false;
        while (f.next(kP) < lastMiss + 4900) any |= f.frame(kP - 1.2, kP) != Step::None;
        check(!any, "steps: 4.9 s with no cycle taking two slots and 1.2 ms to spare holds");
        Step s = Step::None;
        while (s == Step::None && f.now() < lastMiss + 5100) s = f.frame(kP - 1.2, kP);
        check(s == Step::Down && f.p.steps() == 99 && f.now() >= lastMiss + 5000 && f.p.stepCleanMs() >= 5000 &&
                  std::fabs(f.p.stepMeanExcessMs() + 1.2) < 1e-9,
              "steps: at 5.0 s with 1.2 ms to spare, one step down, 0.05");
        const uint64_t downAt = f.now();
        int downs = 0;
        while (f.next(kP) < downAt + 5000) downs += f.frame(kP - 1.2, kP) == Step::Down;
        check(downs == 0, "steps: the next step down not before 5 s more");
        s = Step::None;
        while (s == Step::None && f.now() < downAt + 5100) s = f.frame(kP - 1.2, kP);
        check(s == Step::Down && f.p.steps() == 98, "steps: ... and then it comes");
        f.frame(0, kP, 679, Work::Invalid);
        bool hold = false;
        for (int i = 0; i < 1000; ++i) hold |= f.frame(kP - 0.5, kP) != Step::None;
        check(!hold && f.p.steps() == 98, "steps: no cycle taking two slots but only 0.5 ms to spare holds");
    }
    // The kick from k 2.75, its hold, and one kick per 30 s.
    {
        Feed f;
        // Bursts of 3 two-slot cycles every 90 frames at 12.9 ms: a 0.25 step
        // a burst, the trigger never held 3 s, so no kick.
        int kicks = 0;
        for (int i = 0; f.p.k() < 2.75f && i < 5000; ++i)
            kicks += f.frame(12.9, i % 90 < 3 ? kTwo : kP) == Step::Kick;
        check(f.p.k() == 2.75f && kicks == 0, "steps: bursts of cycles taking two slots step a burst at a time, no kick");
        // Every cycle in its slot at the period, then every cycle two slots:
        // the trigger holds; 0.05 a second while it does, and at 3 s the kick.
        for (int i = 0; i < 40; ++i) f.frame(kP, kP);
        uint64_t triggerAt = 0, kickAt = 0;
        float kickFrom = 0;
        for (int i = 0; i < 400 && !kickAt; ++i) {
            const float k0 = f.p.k();
            const Step s = f.frame(kP, kTwo);
            if (!triggerAt && f.p.triggered()) triggerAt = f.now();
            if (s == Step::Kick) {
                kickAt = f.now();
                kickFrom = k0;
            }
        }
        check(kickAt && kickAt - triggerAt >= 3000 && kickFrom == 2.9f && f.p.k() == 6.0f &&
                  f.p.kickHeldMs() >= 3000 && f.p.stepMisses() >= 3,
              "steps: the trigger held 3 s from k 2.75 (0.05 a second meanwhile, to 2.90): the kick to k_max 6.00");
        // Held: every cycle in its slot with 1.2 ms to spare, no step down for
        // 2 s (nor for 5 s after the last cycle taking two slots).
        bool down = false;
        while (f.next(kP) < kickAt + 2000) down |= f.frame(kP - 1.2, kP) == Step::Down;
        check(!down && f.p.k() == 6.0f, "steps: k_max held 2 s after the kick");
        // A higher k_max and cycles taking two slots again: 0.05 a second, and
        // no second kick until 30 s after the first.
        f.p.configure(8.0f);
        uint64_t secondKick = 0;
        int upsBefore = 0;
        for (int i = 0; i < 3000 && !secondKick; ++i) {
            const Step s = f.frame(kP, kTwo);
            if (s == Step::Kick) secondKick = f.now();
            else if (s == Step::Up) ++upsBefore;
        }
        check(secondKick >= kickAt + 30000 && secondKick < kickAt + 30030 && upsBefore >= 20 && f.p.k() == 8.0f,
              "steps: a second kick within 30 s is refused (0.05 a second meanwhile); at 30 s it comes");
    }
    // The lever spent: at k_max with the trigger, not before 5 s there.
    {
        Feed f;
        f.p.configure(1.5f);
        uint64_t atMax = 0;
        for (int i = 0; i < 400 && !atMax; ++i)
            if (f.frame(12.9, kTwo) != Step::None && f.p.steps() == f.p.maxSteps()) atMax = f.now();
        bool early = false;
        while (f.next(kTwo) < atMax + 4900) {
            f.frame(12.9, kTwo);
            early |= f.p.ceilingMissing(f.now());
        }
        bool spent = false;
        while (!spent && f.now() < atMax + 5100) {
            f.frame(12.9, kTwo);
            spent = f.p.ceilingMissing(f.now());
        }
        check(atMax && !early && spent && f.now() >= atMax + 5000,
              "steps: at k_max with the trigger held, the lever is spent at 5 s there, not before");
    }
    // Every cycle two slots with the 30's mean 1.5 ms over: 0.25, at the 30th
    // sample and not before (the first has no interval: 29 of 30).
    {
        Feed f;
        bool early = false;
        for (int i = 0; i < 29; ++i) early |= f.frame(kP + 1.5, kTwo) != Step::None;
        check(!early && f.frame(kP + 1.5, kTwo) == Step::Up && f.p.upQuanta() == kCoarseQuanta &&
                  f.p.stepMisses() == 29 && std::fabs(f.p.stepMeanExcessMs() - 1.5) < 1e-9 && f.p.k() == 1.25f,
              "steps: every cycle two slots with the mean 1.5 ms over: up 0.25 at the 30th sample and not before");
    }
    // The size reads the 30 samples behind the step, not older ones: just
    // after a step, 10 at 20 ms, then 11.6 ms until the next, whose 30 are
    // all at 11.6: 0.05.
    {
        Feed f;
        Step last = Step::None;
        for (int i = 0; i < 200 && last == Step::None; ++i) last = f.frame(11.6, kTwo);
        const int before = f.p.steps();
        bool early = false;
        for (int i = 0; i < 10; ++i) early |= f.frame(20.0, kTwo) != Step::None;
        Step next = Step::None;
        for (int i = 0; i < 200 && next == Step::None; ++i) next = f.frame(11.6, kTwo);
        check(last == Step::Up && !early && next == Step::Up && f.p.upQuanta() == 1 && f.p.steps() == before + 1 &&
                  f.p.stepMeanExcessMs() < 1.0,
              "steps: the size reads the 30 samples behind the step, not older ones");
    }
    // The cap: k_max 2.1 with bursts at 12.9 ms (a 0.25 step a burst): 1.25,
    // 1.5, 1.75, 2.0, then 2.1 (the rule's 0.25, held to k_max); the clamp.
    {
        Feed f;
        f.p.configure(2.1f);
        int ups = 0;
        bool heldLast = false;
        for (int i = 0; i < 1000; ++i)
            if (f.frame(12.9, i % 90 < 3 ? kTwo : kP) == Step::Up) {
                ++ups;
                heldLast = f.p.upHeld();
            }
        check(ups == 5 && heldLast && f.p.upQuanta() == kCoarseQuanta && f.p.steps() == 22 && f.p.k() == 2.1f,
              "steps: a 0.25 step is held to k_max (2.0 -> 2.1), and k stops there");
        f.p.configure(1.5f);
        check(f.frame(12.9, kP) == Step::Clamp && f.p.k() == 1.5f, "steps: a lowered k_max still clamps k at once");
    }
    // The first acting flight's slope (12.9 ms at k 1, 10.6 at 2.70, a line):
    // a cycle takes two slots while the caller work runs more than 0.3 ms past
    // the period. The ramp is kicked at 3 s, then k comes back 0.05 per 5 s
    // to where the work has a millisecond to spare, with no cycle taking two
    // slots on the way.
    {
        Feed f;
        int coarse = 0, fine = 0, kicks = 0, downs = 0, missesAfterKick = 0;
        uint64_t kickAt = 0, lastDown = 0;
        const uint64_t t0 = f.now();
        for (int i = 0; i < 40000; ++i) {   // ~450 s
            const double work = 12.9 - (12.9 - 10.6) / 1.7 * (double(f.p.k()) - 1.0);
            const Step s = f.frame(work, work > kP + 0.3 ? kTwo : kP);
            if (s == Step::Up) (f.p.upQuanta() == kCoarseQuanta ? coarse : fine)++;
            if (s == Step::Kick) {
                ++kicks;
                kickAt = f.now();
            }
            if (s == Step::Down) {
                ++downs;
                lastDown = f.now();
            }
            if (kickAt && s != Step::Kick && f.p.cycle().missed) ++missesAfterKick;   // after the kick's own frame
        }
        const double settled = 12.9 - (12.9 - 10.6) / 1.7 * (double(f.p.k()) - 1.0);
        check(kicks == 1 && coarse == 3 && fine == 0 && missesAfterKick == 0 && settled >= kP - 1.0 &&
                  settled <= kP + 0.3,
              "steps: on the flight's slope 3 steps of 0.25, the kick at 3 s, then back 0.05 per 5 s with no miss");
        std::printf("lod_governor_test: the flight's slope (12.9 ms at k 1, 10.6 at 2.70): %d steps of 0.25, the kick "
                    "at %.1f s, then %d steps down to k %.2f (work %.2f ms) by %.0f s\n", coarse,
                    double(kickAt - t0) / 1000.0, downs, double(f.p.k()), settled, double(lastDown - t0) / 1000.0);
    }
    // Cycles taking two slots at random at a fixed rate (a fixed-seed
    // generator, 90 Hz, the caller work at the period, 60 s from k 1): how
    // often the trigger fires below a tenth, and whether it kicks. Printed;
    // pinned only in order.
    {
        uint64_t rng = 0x9E3779B97F4A7C15ull;
        auto uniform = [&]() {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            return double(rng >> 11) / 9007199254740992.0;
        };
        const double rates[5] = {0.10, 0.05, 0.03, 0.02, 0.01};
        int ups[5] = {}, kicks[5] = {};
        for (int r = 0; r < 5; ++r) {
            Feed f;
            while (f.now() < 61000) {
                const Step s = f.frame(kP, uniform() < rates[r] ? kTwo : kP);
                ups[r] += s == Step::Up;
                kicks[r] += s == Step::Kick;
            }
        }
        check(ups[0] + kicks[0] > ups[2] + kicks[2] && ups[1] + kicks[1] > ups[4] + kicks[4],
              "steps: rarer cycles taking two slots, fewer steps");
        std::printf("lod_governor_test: two-slot cycles at random, 60 s at 90 Hz from k 1: up steps (kicks) at 10%% %d "
                    "(%d), 5%% %d (%d), 3%% %d (%d), 2%% %d (%d), 1%% %d (%d)\n", ups[0], kicks[0], ups[1], kicks[1],
                    ups[2], kicks[2], ups[3], kicks[3], ups[4], kicks[4]);
    }
}

// The cockpit gate: on foot (Status.json via the journal watcher) k is 1 at
// once and held there, exactly as outside a settlement; aboard again the ramp
// starts over. reduced alike.
void caseFootPolicy() {
    using namespace lodgov;
    Feed f;
    for (int i = 0; i < 150; ++i) f.frame(12.9, kTwo);
    check(f.p.k() > 1.0f && f.p.inSettlement(), "foot: in the cockpit at a busy settlement k has risen");
    check(f.frame(12.9, kTwo, 679, Work::Valid, true) == Step::Foot && f.p.k() == 1.0f && !f.p.inSettlement() &&
              f.p.samples() == 0 && f.p.misses() == 0 && f.p.meanExcessMs() == 0.0 && !f.p.triggered(),
          "foot: on foot k is 1 at once, the settlement and the samples forgotten");
    bool any = false;
    for (int i = 0; i < 1000; ++i) any |= f.frame(14.0, kTwo, 679, Work::Valid, true) != Step::None;
    check(!any && f.p.k() == 1.0f && !f.p.inSettlement(), "foot: held at 1 on foot, however long the frame and however dense");
    f.p.configure(1.5f);
    check(f.frame(14.0, kTwo, 679, Work::Valid, true) == Step::None, "foot: a lowered k_max on foot has nothing to clamp");
    f.p.configure(6.0f);
    // Aboard: the first boundary has no interval, so the 30th sample holds 29
    // cycles taking two slots, and steps.
    Step last = Step::None;
    for (int i = 0; i < 29; ++i) last = f.frame(12.9, kTwo);
    check(last == Step::None && f.p.k() == 1.0f && f.p.inSettlement(), "foot: aboard again, 29 samples do not step");
    check(f.frame(12.9, kTwo) == Step::Up && f.p.k() == 1.25f && f.p.stepMisses() == 29,
          "foot: the 30th steps, as from a fresh start");
    // reduced: 1 on foot, k_max again aboard.
    Feed r;
    r.p.setFixed(true);
    r.p.configure(3.0f);
    check(r.frame(9.0, kP, 250) == Step::Enter && r.p.k() == 3.0f, "foot: reduced puts k at k_max in the cockpit");
    check(r.frame(9.0, kP, 250, Work::Valid, true) == Step::Foot && r.p.k() == 1.0f, "foot: reduced puts k at 1 on foot at once");
    any = false;
    for (int i = 0; i < 100; ++i) any |= r.frame(9.0, kP, 250, Work::Valid, true) != Step::None;
    check(!any && r.p.k() == 1.0f, "foot: reduced holds 1 on foot at a dense settlement");
    check(r.frame(9.0, kP, 250) == Step::Enter && r.p.k() == 3.0f, "foot: reduced is k_max again aboard");
}

// reduced: k = k_max at once on entering a settlement, 1 on leaving it, no
// ramp, no frame-work steps and no kick; the lever-spent line still speaks.
void caseReducedPolicy() {
    using namespace lodgov;
    Feed f;
    f.p.setFixed(true);
    f.p.configure(3.0f);
    bool any = false;
    for (int i = 0; i < 300; ++i) any |= f.frame(14.0, kTwo, 150) != Step::None;
    check(!any && f.p.steps() == 0, "reduced: outside a settlement k stays 1, however long the frame");
    check(f.frame(9.0, kP, 250) == Step::Enter && f.p.k() == 3.0f,
          "reduced: the first frame with 200 records puts k at k_max at once");
    for (int i = 0; i < 300; ++i) any |= f.frame(14.0, kTwo, 250) != Step::None;
    for (int i = 0; i < 700; ++i) any |= f.frame(8.0, kP, 250) != Step::None;
    check(!any && f.p.k() == 3.0f, "reduced: no frame-work step up or down in the settlement");
    f.p.configure(2.0f);
    check(f.frame(11.0, kP, 250) == Step::Clamp && f.p.k() == 2.0f, "reduced: a lowered k_max clamps at once");
    f.p.configure(4.0f);
    check(f.frame(11.0, kP, 250) == Step::Enter && f.p.k() == 4.0f, "reduced: a raised k_max is the factor at once");
    // Every cycle taking two slots for 10 s at k_max: never a kick, and the
    // lever is spent.
    int kicks = 0;
    for (int i = 0; i < 450; ++i) kicks += f.frame(14.0, kTwo, 250) == Step::Kick;
    check(kicks == 0 && f.p.k() == 4.0f && f.p.ceilingMissing(f.now()),
          "reduced: never kicks; at k_max with the trigger for 5 s the lever is spent");
    Step last = Step::None;
    for (int i = 0; i < 29; ++i) last = f.frame(11.0, kP, 100);
    check(last == Step::None && f.p.k() == 4.0f, "reduced: 29 sparse frames hold k_max");
    check(f.frame(11.0, kP, 100) == Step::Reset && f.p.k() == 1.0f, "reduced: the 30th frame under 150 records puts k at 1");
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
    // The part test's first term: half the pixel footprint at d against r.
    check(screenSizePasses(0.001f, 100.0f, 0.0f, 0.05f) && !screenSizePasses(0.001f, 100.0f, 0.0f, 0.049f) &&
          !screenSizePasses(std::numeric_limits<float>::quiet_NaN(), 100.0f, 0.0f, 1.0f),
          "math: the screen-size term is 0.5*(A*d + B) <= r, and a NaN fails it");
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
    float scale() const {
        float s = 0;
        std::memcpy(&s, ctx.data() + 0x30, 4);
        return s;
    }
    // The engine's setter, FUN_142819D90's last store (decomp_2819D90.txt:108):
    // s = 1 + (1 - x), x the settings' LOD distance scale.
    void engineSetsScale(float x) { put(ctx, 0x30, 1.0f + (1.0f - x)); }
    float viewA(uint32_t v) const { float a; std::memcpy(&a, ctx.data() + 0x40 + v * 0x6A0 + 0x550, 4); return a; }
    float viewB(uint32_t v) const { float b; std::memcpy(&b, ctx.data() + 0x40 + v * 0x6A0 + 0x560, 4); return b; }
    // The engine's part test in view v at the scale the context holds now:
    // screen size, the (fake) plane test, then the LOD distance and pick.
    void enginePart(uint32_t v) {
        const float cam[4] = {0, 0, 0, 0};
        float c[4], sph[4];
        std::memcpy(c, centre.data(), 16);
        std::memcpy(sph, sphere.data(), 16);
        const float d = lodgov::engineDistance(c, cam);
        uint32_t n = 0;
        const bool pass = lodgov::screenSizePasses(viewA(v), d, viewB(v), sph[0]) &&
                          static_cast<uint32_t>(g_planeVerdict) != 0xFFFFFFFFu &&
                          lodgov::lodPick(lodgov::LodTable::fromBytes(partTable.data()),
                                          lodgov::lodDistance(viewA(v), d, sph[0], scale(), viewB(v)), &n);
        setOut(pass ? n : 0, pass ? 1 : 0);
    }
    // The traversal's record test at the scale the context holds now: views
    // 1, 22 and 4 tested; a failing view's bit cleared, a passing one's
    // nibble stored.
    void engineRecord() {
        const float cam[4] = {0, 0, 0, 0};
        float c[4], r = 0;
        std::memcpy(c, record.data() + 0x240, 16);
        std::memcpy(&r, record.data() + 0x280, 4);
        const uint32_t bits[3] = {1, 22, 4};
        uint64_t m = 0;
        for (uint32_t v = 0; v < 3; ++v) {
            uint32_t n = 0;
            const float d = lodgov::engineDistance(c, cam);
            if (lodgov::lodPick(lodgov::LodTable::fromBytes(recTable.data()),
                                lodgov::lodDistance(viewA(v), d, r, scale(), viewB(v)), &n)) {
                m |= 1ull << bits[v];
                setNibble(bits[v], n);
            }
        }
        setMask(m);
    }
};

void setK(float k) { g_kBits.store(toBits(k)); }
// A clean context table: no builder context remembered either (a new Fake
// can land at an old one's address).
void freshTable() {
    g_ctx.store(0);
    resetContexts();
}

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
    freshTable();
    g_live.store(true);
    setK(1.0f);
    // The eyes are named from the context the builder observer publishes.
    g_ctx.store(f.ctxAt());
    identifyEyes(g_state);
    check(g_eyes.load() == (1u | (22u << 8)), "observers: the eyes are the two finest perspective views, bits 1 and 22");
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(delta(cParts) == 1 && partField(0, pSeen) == 1 && partField(0, pPassed) == 1 && partField(0, pDrop) == 0 &&
          partField(0, pMismatch) == 0 && partField(0, pActing) == 0, "part: seen and passed in eye A; nothing differs at k = 1");
    setK(2.0f);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[1], true);
    check(partField(1, pDrop) == 1 && partField(1, pHist2) == 1 && partField(1, pActing) == 0,
          "part: observing, at k = 2 eye B's part would drop, 0.57 deg");
    setK(1.10f);   // f 0.109: still under t2 0.12 -> nibble 1, nothing differs
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pPassed) == 1 && partField(0, pChange) == 0 && partField(0, pDrop) == 0,
          "part: at k = 1.1 the nibble holds");
    setK(1.25f);   // f 0.124: past t2 0.12, under t0 0.15 -> nibble 2
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pChange) == 1 && partField(0, pDrop) == 0, "part: at k = 1.25 the nibble moves 1 -> 2");
    f.setOut(0, 1);   // the engine says nibble 0: the recompute disagrees
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pMismatch) == 1 && partField(0, pChange) == 0, "part: a wrong nibble is a disagreement, never shadowed");
    f.setOut(0, 0);   // an engine reject
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[2], true);
    check(partField(2, pSeen) == 1 && partField(2, pPassed) == 0 && partField(2, pDrop) == 0,
          "part: observing, an engine reject in another view is seen, not passed, never counted");
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
    freshTable();
}

// --- The LOD-scale setter's bracket: the one write --------------------------------------------
// The fake setter stores 1 + (1 - x) at ctx+0x30, as FUN_142819D90 does last,
// and the bracket then hands the context to the governor.
void setterCall(Fake& f, float x) {
    f.engineSetsScale(x);
    lodGovernorSetterObserver(f.ctxAt());
}

void caseSetter() {
    Fake f, other;
    freshTable();
    g_standDown.store(nullptr);
    g_live.store(true);
    g_acting.store(true);
    const uint32_t calls0 = g_setterCalls.load(), scaled0 = g_setterScaled.load();
    setK(2.0f);
    // A context the draw-item builder has not used is never written.
    setterCall(f, 0.5f);
    check(f.scale() == 1.5f && g_setterCalls.load() - calls0 == 1 && g_setterScaled.load() == scaled0,
          "setter: a context the builder has not used is never written (the engine's 1.5 stands)");
    // The builder uses f's context: from the next rebuild its scale is the game's x k.
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    setterCall(f, 0.5f);
    CtxSlot* slot = findContext(f.ctxAt());
    if (!slot) {
        check(false, "setter: the builder observer registers its context");
        return;
    }
    check(f.scale() == 3.0f && fromBits(slot->gameBits.load()) == 1.5f && fromBits(slot->heldBits.load()) == 3.0f &&
              g_setterScaled.load() - scaled0 == 1 && slot->firstState.load() == 2,
          "setter: the builder's context is scaled right after the engine's store: game 1.5 -> 3.0 at k 2");
    // The second context the engine rebuilds each frame, never the builder's.
    setterCall(other, 0.5f);
    check(other.scale() == 1.5f, "setter: the engine's other context is never written while acting");
    // A k step: the next rebuild writes the game's value times the new k.
    setK(2.5f);
    setterCall(f, 0.5f);
    check(f.scale() == 3.75f, "setter: a k step lands at the next rebuild (1.5 x 2.5)");
    // k back to 1: the engine's own store is left as it is.
    setK(1.0f);
    setterCall(f, 0.5f);
    check(f.scale() == 1.5f && slot->heldBits.load() == 0 && fromBits(slot->gameBits.load()) == 1.5f,
          "setter: k back to 1 leaves the engine's own value at the next rebuild");
    // A slider change while acting: the game's new value is scaled, no adoption step.
    setK(2.0f);
    setterCall(f, 1.0f);
    check(f.scale() == 2.0f && fromBits(slot->gameBits.load()) == 1.0f,
          "setter: a slider change (x 1.0 -> s 1.0) is taken as the engine stores it and scaled (2.0)");
    // An implausible value is counted and left alone.
    const uint32_t implausible0 = g_setterImplausible.load();
    setterCall(f, -10.0f);   // s = 12
    check(f.scale() == 12.0f && g_setterImplausible.load() - implausible0 == 1 && slot->heldBits.load() == 0,
          "setter: an implausible game value (12) is counted and left alone");
    put(f.ctx, 0x30, std::numeric_limits<float>::quiet_NaN());
    lodGovernorSetterObserver(f.ctxAt());
    check(std::isnan(f.scale()) && g_setterImplausible.load() - implausible0 == 2, "setter: a NaN is left alone too");
    // Observe (not acting): computes, never writes.
    g_acting.store(false);
    setterCall(f, 0.5f);
    check(f.scale() == 1.5f && fromBits(slot->gameBits.load()) == 1.5f,
          "setter: observing, the game's value is read and never written");
    g_acting.store(true);
    // gameScaleFor: the tests' scale is the game's unless EDVR's value is in force.
    setterCall(f, 0.5f);
    check(f.scale() == 3.0f && gameScaleFor(f.ctxAt(), 3.0f) == 1.5f && gameScaleFor(f.ctxAt(), 1.5f) == 1.5f &&
              gameScaleFor(other.ctxAt(), 3.0f) == 3.0f,
          "setter: the game's scale for a test is 1.5 exactly while EDVR's 3.0 is in force, else the value read");
    // Restore: the game's value goes back into every context still holding EDVR's.
    check(restoreScaled() == 1 && f.scale() == 1.5f && slot->heldBits.load() == 0,
          "setter: restoring writes the game's 1.5 back over EDVR's 3.0");
    setterCall(f, 0.5f);
    put(f.ctx, 0x30, 1.25f);   // the engine stored its own since
    check(restoreScaled() == 0 && f.scale() == 1.25f, "setter: a restore never overwrites a value the engine stored since");
    // The table's limit: a ninth builder context stands acting down.
    for (uintptr_t i = 1; i < lodgov::kContexts; ++i) registerContext(0x100000 + i * 0x1000);
    check(!g_contextOverflow.load() && actingNow(), "setter: eight builder contexts fit the table");
    registerContext(0x900000);
    check(g_contextOverflow.load() && !actingNow(), "setter: a ninth stands acting down");
    setterCall(f, 0.5f);
    check(f.scale() == 1.5f, "setter: stood down by the table's limit, nothing is written");
    freshTable();
    // An unwritable page: VirtualQuery refuses it before any write, and acting stands down for the process.
    void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const uintptr_t ro = reinterpret_cast<uintptr_t>(page);
    float gameS = 1.5f;
    std::memcpy(reinterpret_cast<void*>(ro + 0x30), &gameS, 4);
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_READONLY, &old);
    registerContext(ro);
    lodGovernorSetterObserver(ro);
    float after = 0;
    std::memcpy(&after, reinterpret_cast<const void*>(ro + 0x30), 4);
    check(after == 1.5f && g_standDown.load() != nullptr && std::strstr(g_standDown.load(), "VirtualQuery") &&
              g_standDownCtx.load() == ro && !actingNow(),
          "setter: a read-only context is refused by VirtualQuery before any write, and acting stands down");
    check(!writeScale(ro, 2.0f), "setter: a write that faults is caught by the guard, not the process");
    registerContext(f.ctxAt());
    setterCall(f, 0.5f);
    check(f.scale() == 1.5f, "setter: after a stand-down nothing is written, observing goes on");
    VirtualFree(page, 0, MEM_RELEASE);
    g_standDown.store(nullptr);
    g_standDownCtx.store(0);
    // Off: the bracket does nothing at all.
    g_live.store(false);
    const uint32_t calls1 = g_setterCalls.load();
    setterCall(f, 0.5f);
    check(g_setterCalls.load() == calls1 && f.scale() == 1.5f, "setter: off, the bracket counts and writes nothing");
    g_acting.store(false);
    setK(1.0f);
    freshTable();
}

// --- The acting counts: the price, the same quantity as observing's -----------------------
void caseActingCounts() {
    Fake f;
    freshTable();
    g_standDown.store(nullptr);
    g_live.store(true);
    g_acting.store(true);
    g_frustumStub = &fakePlanes;
    g_frustum.store(&fakePlanes);
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());   // registers the builder's context
    identifyEyes(g_state);
    check(findContext(f.ctxAt()) != nullptr && g_eyes.load() == (1u | (22u << 8)),
          "acting: the builder's first call registers its context; the eyes are named from it");
    // The game holds 1.0; k 2 is in force: the part's f 0.198 > t0 0.15, an
    // engine reject, which the game's own setting (f 0.099) would have passed.
    setK(2.0f);
    setterCall(f, 1.0f);
    check(f.scale() == 2.0f, "acting: EDVR's 2.0 is in force");
    g_planeVerdict = 1;
    f.enginePart(0);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pActing) == 1 && partField(0, pPassed) == 0 && partField(0, pDrop) == 1 && partField(0, pHist2) == 1 &&
              partField(0, pMismatch) == 0,
          "acting: an engine reject the game's setting would have kept is dropped (plane test run and passed), 0.57 deg");
    // Outside a plane: the engine's plane test rejects it at any scale.
    g_planeVerdict = 0xFFFFFFFFull;
    f.enginePart(0);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pDrop) == 0 && partField(0, pUnverified) == 0,
          "acting: a reject outside a plane is not dropped by EDVR");
    g_planeVerdict = 1;
    // No matched plane test: counted apart, never as dropped.
    g_frustum.store(nullptr);
    f.enginePart(0);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pDrop) == 0 && partField(0, pUnverified) == 1,
          "acting: without a matched plane test the reject is counted as unverified, not dropped");
    g_frustum.store(&fakePlanes);
    // Under a pixel either way: the screen-size term rejected it.
    put(f.sphere, 0, 0.01f);
    f.enginePart(0);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    check(partField(0, pDrop) == 0 && partField(0, pUnverified) == 0 && partField(0, pActing) == 1,
          "acting: a reject under a pixel is not dropped by EDVR");
    put(f.sphere, 0, 1.0f);
    // A pass at EDVR's scale whose level differs from the game's: k 1.25 ->
    // f 0.124 (nibble 2) against the game's 0.099 (nibble 1).
    setK(1.25f);
    setterCall(f, 1.0f);
    f.enginePart(1);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[1], true);
    check(partField(1, pPassed) == 1 && partField(1, pChange) == 1 && partField(1, pMismatch) == 0 && partField(1, pDrop) == 0,
          "acting: a pass at EDVR's scale one level coarser than the game's is a level change");
    // The disagreement gate reads the scale the engine held.
    f.setOut(0, 1);
    mark();
    lodGovernorPartObserver(f.items(), f.outAt(), f.view[1], true);
    check(partField(1, pMismatch) == 1, "acting: a nibble the recompute at EDVR's scale does not reproduce is a disagreement");
    // The record test at EDVR's scale: stored nibbles 2 (f 0.2475), the game's 1.
    f.engineRecord();
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(recField(0, rSeen) == 1 && recField(0, rActing) == 1 && recField(0, rChange) == 1 && recField(0, rMismatch) == 0 &&
              recField(1, rChange) == 1 && delta(cRecDispatchMismatch) == 0 && delta(cRecNotDispatched) == 0,
          "acting: the record's eyes reproduce at EDVR's scale and are one level coarser than the game's");
    // At k 2 both eyes fail the record test: their bits are gone, not seen.
    setK(2.0f);
    setterCall(f, 1.0f);
    f.engineRecord();
    mark();
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());
    check(recField(0, rSeen) == 0 && recField(1, rSeen) == 0 && delta(cRecDispatchMismatch) == 0 && delta(cRecords) == 1,
          "acting: a record that lost both eyes at EDVR's scale shows no eye (the traversal keeps no pre-test mask)");
    restoreScaled();
    g_live.store(false);
    g_acting.store(false);
    setK(1.0f);
    freshTable();
    f.engineSetsScale(0.0f);
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

uint64_t g_t = 50000, g_seq = 10;

// The boundary's clock (QueryPerformanceCounter ms in the DLL). The runtime
// these cases fly against is a simple one: a frame whose caller work ran more
// than 0.3 ms past the period missed its slot and took two; any other took
// one. Wall time (the t the cases pass) still moves 11 ms a frame, so the
// windows keep their frame counts.
const double kPeriodMs = 1000.0 / 90.0;
double g_clock = 1000.0;
void boundaryCycle(uint64_t t, double cycleMs) {
    g_clock += cycleMs;
    frameBoundaryAt(t, g_clock);
}
void boundaryAt(uint64_t t, double workMs) {
    boundaryCycle(t, workMs > kPeriodMs + 0.3 ? 2.0 * kPeriodMs : kPeriodMs);
}

// Observe mode (advanced.settlement_detail_observe = 1): everything the
// shadow governor did, now with the observe tag -- it never writes.
void caseBoundary() {
    Fake f;
    uint64_t& t = g_t;
    uint64_t& seq = g_seq;
    auto frames = [&](uint32_t n, uint32_t records, uint32_t parts, double ms,
                      uint32_t version = EDVR_NATIVE_TIMING_VERSION_5) {
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t r = 0; r < records; ++r)
                if (g_obsBuilder) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
            for (uint32_t p = 0; p < parts; ++p)
                if (g_obsPart) g_obsPart(f.items(), f.outAt(), f.view[p & 1], true);
            setTiming(++seq, t, ms, version);
            boundaryAt(t, ms);
            t += 11;
        }
    };
    // The builder ran before the governor was switched on: its last context
    // names the eyes from the first frame.
    g_ctx.store(f.ctxAt());
    // game: one off line, and then nothing at all.
    size_t at = g_lines.size();
    applyConfig("game", 2.0f, true, t);
    check(logged("settlement detail: off (fix.settlement_detail = game", at) && !g_live.load() && g_attaches == 0,
          "boundary: game logs one off line and attaches nothing");
    at = g_lines.size();
    frames(3000, 250, 20, 12.5);
    check(g_lines.size() == at, "boundary: while off, 33 s of frames log nothing (never ran reads as silence)");
    applyConfig("game", 2.0f, true, t);
    check(g_lines.size() == at, "boundary: the same value again is not re-logged");
    // auto, observe only: the configure line, then the ramp. No runtime frame
    // is published yet, so the configure line cannot name the signal and says
    // who will.
    g_timing = NativeTimingSnapshot{};
    at = g_lines.size();
    applyConfig("auto", 2.0f, true, t);
    check(g_live.load() && g_attaches == 1 && g_obsBuilder && g_obsPart && g_obsSetter && !g_acting.load(),
          "boundary: auto with observe attaches the three observers and does not act");
    check(logged("settlement detail: on (auto, observe only: never writes (advanced.settlement_detail_observe = 1)) -- "
                 "k in [1, 2.00]", at) &&
              logged("part test FUN_1442B3FC0 hooked, LOD-scale setter FUN_142819D90 hooked, plane test FUN_1404F4E10 "
                     "matched", at),
          "boundary: the configure line says observe only, never writes, and the hooks");
    check(logged("frame work = caller work per cycle if the runtime sends it (timing v5), else app work (pre-submit "
                 "only; host older); the first runtime frame decides and a line names it", at),
          "boundary: before any runtime frame the configure line says the first frame names the signal");
    check(logged("settlement detail: auto's policy, on the last 30 cycles (one took two display slots when longer "
                 "than 1.5 x the period;", at) &&
              logged("to k_max 2.00 at once if that holds 3 s below it (a kick: held 2 s, at most one per 30 s)", at),
          "boundary: auto's policy follows on a line of its own");
    at = g_lines.size();
    // 30.8 s, 250 records a frame; caller work 11.6 ms, 0.49 over budget (the
    // fine steps), app work 5.6 under; a burst of 3 cycles taking two slots
    // every 91 frames: a step a second, the trigger never held 3 s, no kick.
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        for (uint32_t p = 0; p < 20; ++p) g_obsPart(f.items(), f.outAt(), f.view[p & 1], true);
        setTiming(++seq, t, 11.6);
        boundaryCycle(t, i % 91 < 3 ? 2.0 * kPeriodMs : kPeriodMs);
        t += 11;
    }
    check(countLogged("settlement detail: frame work = caller work per cycle (runtime timing v5: the caller thread "
                      "from one pose wait's return to the next one's entry", at) == 1,
          "boundary: the first version 5 frame names the signal, once");
    check(countLogged("settlement detail (observe only, never writes): k 1.00 -> 1.05, up 0.05: 3 of the last 30 "
                      "cycles took two display slots with the caller work at the period; the 30's mean caller work "
                      "0.49 ms over (1.00 ms over or less: the fine step); 250 builder records", at) == 1,
          "boundary: the first step is logged, 0.05, with the two-slot cycles and the mean excess behind it");
    check(logged("slots missed 89 of 2728 (CPU under 0); kicks 0; at the ceiling with misses ", at) &&
              !logged("kick:", at),
          "boundary: the summary counts the cycles that took two slots, and bursts never kick");
    check(countLogged("settlement detail (observe only, never writes): k ", at) >= 4 &&
              countLogged("settlement detail (observe only, never writes): k ", at) <= 7,
          "boundary: step lines are rate-limited to one per 5 s");
    check(logged("steps since the last line", at), "boundary: a rate-limited line counts the steps it skipped");
    check(logged("250 builder records, frame work = caller work per cycle: 11.60 ms vs period 11.11 ms.", at) &&
              !logged("-> LOD scale s x k", at),
          "boundary: a step line names the signal it stepped on; observing, it names no scale to write");
    check(logged("k now 2.00, effective s x k 2.000 (window 1.00..2.00 of max 2.00; 20 up (0 by 0.25), 0 down, 0 "
                 "resets, 0 clamps; held on foot 0 frames)", at),
          "boundary: the summary's k, effective s x k, range, steps and their size, and the frames held on foot");
    check(logged("builder records/frame 250.0 (max 250", at) && logged("part tests/frame 20.0", at),
          "boundary: the summary's density means");
    check(logged("frame work = caller work per cycle: 11.60 ms mean vs period 11.11 ms", at) &&
              logged("invalid 0, caller work absent 0)", at),
          "boundary: the summary's frame work is the caller work (not the 5.60 ms app work beside it) vs the period");
    check(logged("LOD scale: game s 1.000 (the builder's read: no setter call yet), held 1.000 (k 2.00); setter calls 0 "
                 "(scaled 0) on 0 pointers", at) && !logged("NOT ACTING", at),
          "boundary: observing, the summary's LOD scale and a setter that never ran, without NOT ACTING");
    check(logged("eye views bits 1 / 22", at), "boundary: the summary names the eyes");
    check(logged("settlement detail (observe only, never writes) eye A (view bit 1 now): parts tested 10.0/frame, engine "
                 "passed 10.0; at the shadow k would drop", at) &&
              logged("settlement detail (observe only, never writes) eye B (view bit 22 now)", at),
          "boundary: one line per eye with its part counts");
    check(logged("records passed 250.0/frame", at) &&
              logged("disagreements with the engine at the LOD scale it held: parts 0, records 0", at),
          "boundary: the eye lines carry the record test and the disagreements");
    check(logged("settlement detail (observe only, never writes) other views:", at) &&
              logged("would not be called for at all", at),
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
              logged("LOD scale: game s 1.500 (the builder's read: no setter call yet), held 1.500", at),
          "boundary: the summary prints the effective s x k beside k, from the engine's s");
    check(countLogged("settlement detail: frame work = ", at) == 0,
          "boundary: the signal line is not repeated while the runtime's version holds");
    // Every cycle taking two slots at k_max: after 5 s at the ceiling, the
    // lever is spent, said once a window -- these frames end one window and
    // begin the next, where it still holds.
    check(countLogged("settlement detail: at the ceiling (k 2.00, s x k 3.000) and still missing 30 of the last 30 "
                      "display slots: the remaining caller work (12.50 ms mean, 1.39 ms over the period) is not "
                      "LOD-elastic; 0 of those misses had the CPU under the period (not ours).", at) == 2,
          "boundary: at k_max with every cycle taking two slots for 5 s, the ceiling line, once a window");
    put(f.ctx, 0x30, 1.0f);
    // A lowered k_max: logged again, clamps at the next frame.
    at = g_lines.size();
    applyConfig("auto", 1.5f, true, t);
    frames(1, 250, 0, 12.5);
    check(logged("k in [1, 1.50]", at) && logged("clamped to the new advanced.settlement_detail_max", at),
          "boundary: a lowered k_max re-logs the configure line and clamps");
    check(logged("; frame work = caller work per cycle (runtime timing v5: the caller thread", at),
          "boundary: once a runtime frame has said which, the configure line names the signal");
    // reduced, observe only: k_max at once in the settlement, never written.
    at = g_lines.size();
    applyConfig("reduced", 1.5f, true, t);
    frames(1, 250, 0, 12.5);
    check(logged("settlement detail: on (reduced, observe only: never writes (advanced.settlement_detail_observe = 1)) "
                 "-- k = 1.50 "
                 "(advanced.settlement_detail_max) at once from a frame with >= 200 draw-builder records, 1 after 30 "
                 "frames under 150 or on foot, no ramp", at) && g_attaches == 1,
          "boundary: reduced's configure line says k_max at once, no ramp, 1 on foot");
    // Off mid-window: the partial summary, then the off line.
    at = g_lines.size();
    applyConfig("game", 1.5f, true, t);
    check(!g_live.load() && g_detaches == 1 && !g_obsBuilder && !g_obsSetter &&
              logged("settlement detail (observe only, never writes): ", at) &&
              logged("settlement detail: off (fix.settlement_detail = game: the game's own detail; nothing is observed "
                     "or changed; the game's LOD scale written back to 0 context(s))", at),
          "boundary: switching off logs the partial window and detaches");
    // On again with nothing built: k stuck at 1, and the summary says why --
    // and, with no LOD scale ever read, that s x k is unknown.
    g_scaleBits.store(0);
    g_ctx.store(0);
    at = g_lines.size();
    applyConfig("auto", 2.0f, true, t);
    frames(2800, 0, 0, 12.5);
    check(logged("k stayed 1: no draw-item builder calls", at) && !logged("eye A (view bit", at),
          "boundary: no builder calls -> the header says k stayed 1 and no eye lines");
    check(logged("k now 1.00, effective s x k unknown (no LOD scale read yet) (window", at) &&
              logged("LOD scale: game s unknown, held unknown (k 1.00)", at),
          "boundary: no LOD scale read -> the effective s x k and the game's s say unknown, not 0");
    at = g_lines.size();
    // The ring still holds the 12.5 ms samples, and frame work counts whatever
    // the density: 40 sparse frames within budget replace them first.
    frames(40, 0, 0, 10.5);
    frames(2800, 250, 20, 10.5);   // dense, every cycle one slot
    check(logged("k stayed 1: no cycle took two display slots with the caller work at the period in a frame with 200 "
                 "records", at),
          "boundary: every cycle in its slot -> why k stayed 1");
    // 2 of every 30 cycles taking two slots: the dead band, and the summary says so.
    at = g_lines.size();
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        const double ms = i % 15 == 0 ? 12.2 : 10.9;
        setTiming(++seq, t, ms);
        boundaryAt(t, ms);
        t += 11;
    }
    check(logged("k stayed 1: never 3 of the last 30 cycles took two display slots with the caller work at the period "
                 "(1-2 is the dead band)", at) &&
              !logged("up 0.", at),
          "boundary: 2 of every 30 cycles taking two slots -> no step, and the summary names the dead band");
    // Every cycle taking two slots: 0.25 steps a second, and after 3 s of it
    // below k_max the kick, said whole. Then headroom: 5 s with no cycle
    // taking two and the mean 2.11 ms under -- one step down, and the line
    // says why.
    at = g_lines.size();
    frames(400, 250, 20, 12.9);
    const float raised = g_state.policy.k();
    // (1.05 first: the ring still held the dead band's samples; then 1.30 and
    // 1.55 at 0.25; the kick at 3.0 s.)
    check(raised == 2.0f &&
              logged("kick: 30 of the last 30 cycles took two slots for 3.0 s at k 1.55: to k_max 2.00 so consecutive "
                     "frames fit and the runtime returns to full rate; 250 builder records", at),
          "boundary: the trigger held 3 s below k_max kicks to k_max, and the kick line is never rate-limited");
    frames(700, 250, 20, 9.0);
    check(g_state.policy.k() == 1.95f &&
              countLogged("down 0.05: no cycle took two display slots for 5.0 s and the last 30's mean caller work is "
                          "2.11 ms under the period (more than 1.00 ms to spare); 250 builder records, frame work = "
                          "caller work per cycle: 9.00 ms vs period 11.11 ms", at) == 1,
          "boundary: one step down after 5 s with no cycle taking two slots; its line says how long and the margin");
    applyConfig("game", 2.0f, true, t);
    // A runtime whose newest frame is invalid: every boundary breaks the runs.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        setTiming(++seq, t, 12.5);
        g_timing.invalid = true;
        boundaryAt(t, 12.5);
        t += 11;
    }
    check(logged("over 0 samples", at) && logged("invalid 2729, caller work absent 0)", at) &&
              logged("k stayed 1: no valid frame-work sample from the native runtime", at),
          "boundary: invalid runtime frames are counted and hold k at 1");
    applyConfig("game", 2.0f, true, t);
    // THE DEFECT the caller work fixes -- the first shadow flight (2026-09-23
    // 07:25 UTC, parked at the settlement, 45 fps): app work 8.37-8.67 ms,
    // caller work 14.4 ms (cycle 21.99 - next wait 7.57), period 11.11. On the
    // app figure k never left 1; on the caller work, the same frames step up.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    frames(2800, 250, 20, 14.4);   // version 5: caller 14.4 ms, app 8.4 ms beside it
    check(logged("frame work = caller work per cycle: 14.40 ms mean vs period 11.11 ms", at) &&
              logged("settlement detail (observe only, never writes): k 1.00 -> 1.25, up 0.25: 29 of the last 30 "
                     "cycles took two display slots with the caller work at the period; the 30's mean caller work "
                     "3.29 ms over (more than 1.00 ms over: the coarse step)", at) &&
              !logged("k stayed 1", at),
          "boundary: the first flight's frames (caller 14.4 ms, app 8.4 ms) step k up on the caller work, by 0.25");
    applyConfig("game", 2.0f, true, t);
    // A version 5 frame whose runtime could not close the cycle before it
    // carries no caller work: an invalid sample, never the app work beside it
    // (the two figures are never mixed in one run).
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        setTiming(++seq, t, 14.4);
        g_timing.cpu.callerWorkValid = 0;
        g_timing.cpu.callerWorkMs = 0;
        boundaryAt(t, 14.4);
        t += 11;
    }
    check(logged("over 0 samples (over by > 0.30 ms: 0, under by > 1.00 ms: 0, invalid 2729, caller work absent 2729)",
                 at) &&
              logged("k stayed 1: no valid frame-work sample from the native runtime", at),
          "boundary: a version 5 frame without caller work is an invalid sample, counted as such");
    applyConfig("game", 2.0f, true, t);
    // An older runtime (timing v4): no caller work crosses, the app work
    // stands in, and every line says so. At the first flight's 8.4 ms app
    // work it holds k at 1 -- the defect, reproduced on the fallback.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    frames(2800, 250, 20, 8.4, EDVR_NATIVE_TIMING_VERSION_4);
    check(countLogged("settlement detail: frame work = app work (pre-submit only; host older: runtime timing v4 sends "
                      "no caller work", at) == 1,
          "boundary: a version 4 runtime is named once, as the fallback");
    check(logged("frame work = app work (pre-submit only; host older): 8.40 ms mean vs period 11.11 ms", at) &&
              logged("k stayed 1: no cycle took two display slots with the caller work at the period", at),
          "boundary: the fallback's summary names the app work; with every cycle in its slot k holds at 1");
    at = g_lines.size();
    applyConfig("auto", 1.5f, true, t);
    check(logged("; frame work = app work (pre-submit only; host older: runtime timing v4 sends no caller work", at),
          "boundary: the configure line names the fallback once a runtime frame has said which");
    applyConfig("game", 2.0f, true, t);
    // Version 3: no base rate either -- the session's first predicted period
    // is the budget -- and an over-budget app work still steps.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    frames(2800, 250, 20, 12.5, EDVR_NATIVE_TIMING_VERSION_3);
    check(countLogged("runtime timing v3 sends no caller work", at) == 1 &&
              logged("frame work = app work (pre-submit only; host older): 12.50 ms mean vs period 11.11 ms", at) &&
              logged("frame work = app work (pre-submit only; host older): 12.50 ms vs period 11.11 ms", at),
          "boundary: a version 3 runtime falls back to the app work against the first predicted period, and steps");
    applyConfig("game", 2.0f, true, t);
    // An attach refusal: off, said so, nothing counted.
    at = g_lines.size();
    g_attachResult = "opcode_mismatch";
    applyConfig("auto", 2.0f, true, t);
    check(!g_live.load() && !g_acting.load() && logged("could not attach its engine hooks (opcode_mismatch)", at),
          "boundary: a refused attach is logged and stays off");
    g_attachResult = "installed";
    at = g_lines.size();
    applyConfig("fast", 2.0f, true, t);
    check(logged("fix.settlement_detail = \"fast\" is not game, auto or reduced", at) && !g_live.load(),
          "boundary: an unknown value is named and treated as game");
    applyConfig("game", 2.0f, true, t);
    // The configure sweep with every key unset (the stub Config answers every
    // default): the shipped fix -- auto, acting, the compiled k_max 6.0,
    // observe 0.
    at = g_lines.size();
    lodGovernorConfigure(Config::get());
    check(g_live.load() && g_acting.load() && g_state.mode == Mode::Auto && g_state.kMaxCfg == 6.0f &&
              g_state.policy.kMax() == 6.0f && !g_state.observe &&
              logged("settlement detail: on (auto: acts by scaling the game's LOD scale", at) &&
              logged("-- k in [1, 6.00], auto's policy on the next line", at) &&
              logged("settlement detail: auto's policy, on the last 30 cycles", at),
          "boundary: with the keys unset the governor is on in auto and acts, k_max the compiled 6.0, observe 0");
    applyConfig("game", lodgov::kDefaultMax, false, t);
    at = g_lines.size();
    applyConfig("", lodgov::kDefaultMax, false, t);
    check(g_live.load() && g_state.mode == Mode::Auto && logged("settlement detail: on (auto:", at),
          "boundary: an empty fix.settlement_detail is the compiled default, auto");
    applyConfig("game", lodgov::kDefaultMax, false, t);
}

// The cockpit gate end to end, acting: on foot (the journal watcher's
// Status.json) k is 1 at once and the engine's next rebuild keeps the game's
// value; one line per transition; the summary counts the frames held; a
// window wholly on foot says why k stayed 1; and with the watcher off the log
// says once that on foot cannot be told, and the governor runs.
void caseFootBoundary() {
    Fake f;
    table(f.recTable, 0, {0.9f, 0.1f, 0.2f}, 2);
    uint64_t& t = g_t;
    uint64_t& seq = g_seq;
    g_standDown.store(nullptr);
    auto frames = [&](uint32_t n, double ms) {
        for (uint32_t i = 0; i < n; ++i) {
            f.engineSetsScale(0.5f);   // the game's 1.5
            if (g_obsSetter) g_obsSetter(f.ctxAt());
            f.engineRecord();
            for (uint32_t r = 0; r < 250; ++r)
                if (g_obsBuilder) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
            setTiming(++seq, t, ms);
            boundaryAt(t, ms);
            t += 11;
        }
    };
    g_onFoot = false;
    g_journalActive = true;
    size_t at = g_lines.size();
    applyConfig("auto", 2.0f, false, t);
    frames(400, 12.9);   // 4.4 s at 12.9 ms, every cycle two slots: 1.25, 1.5, 1.75, then the kick to 2.0
    check(g_acting.load() && g_state.policy.k() == 2.0f && f.scale() == 3.0f,
          "foot: in the cockpit the ramp and its kick reach k_max 2.0, and 1.5 x 2 is in force");
    const uint32_t scaled0 = g_setterScaled.load();
    const size_t atFoot = g_lines.size();
    g_onFoot = true;
    frames(1000, 12.9);   // 11 s on foot, far over budget, dense
    check(countLogged("settlement detail (acting): on foot (the game's Status.json, the flag the on-foot frame pacing "
                      "reads): k 2.00 -> 1.00, held at 1 while on foot -- the governor is for the cockpit only", at) == 1 &&
              g_state.policy.k() == 1.0f && f.scale() == 1.5f,
          "foot: on foot, one line and k at 1 at once; the engine's rebuild keeps the game's 1.5");
    check(g_setterScaled.load() - scaled0 == 1 && !logged("settlement detail (acting): k ", atFoot),
          "foot: one rebuild's lag (the one before the boundary saw the flag), then nothing written and no step");
    g_onFoot = false;
    frames(1, 12.9);
    check(countLogged("settlement detail (acting): no longer on foot (Status.json) after 11.0 s, 1000 frames held at "
                      "k 1; the governor resumes", at) == 1,
          "foot: aboard again, one line saying how long");
    frames(1400, 12.9);   // to the 30 s summary; the ramp starts over
    check(logged("held on foot 1000 frames); ", at) && g_state.policy.k() == 2.0f && f.scale() == 3.0f,
          "foot: the summary counts the frames held on foot, and aboard the ramp has run again");
    // A window wholly on foot: k stayed 1, and why. Switched on while on foot,
    // the first boundary says so.
    applyConfig("game", 2.0f, false, t);
    g_onFoot = true;
    at = g_lines.size();
    applyConfig("auto", 2.0f, false, t);
    frames(2800, 12.9);
    check(countLogged("on foot (the game's Status.json", at) == 1 && logged("k 1.00 -> 1.00, held at 1 while on foot", at) &&
              logged("held on foot 2729 frames); ", at) &&
              logged("k stayed 1: on foot the whole window (the governor is for the cockpit only)", at) &&
              !logged("up 0.", at) && f.scale() == 1.5f,
          "foot: a window wholly on foot holds k at 1, writes nothing, and says why k stayed 1");
    // The watcher off: nothing is known, the log says so once, the governor runs.
    applyConfig("game", 2.0f, false, t);
    g_journalActive = false;   // g_onFoot is still set: unknown is not on foot
    at = g_lines.size();
    applyConfig("auto", 2.0f, false, t);
    frames(400, 12.9);
    check(countLogged("settlement detail: the journal watcher is not reading the game's Status.json", at) == 1 &&
              !logged("on foot (the game's Status.json", at) && g_state.policy.k() == 2.0f,
          "foot: with the journal watcher off the log says once that on foot cannot be told, and the governor runs");
    applyConfig("game", 2.0f, false, t);
    g_journalActive = true;
    g_onFoot = false;
}

// Acting end to end: auto and reduced write the game's LOD scale x k at the
// engine's rebuild, the fake engine runs its tests at whatever the context
// holds, and the lines say so.
void caseActingBoundary() {
    Fake f, other;
    // The record's table passes the eyes at every k here (t0 0.9), so the
    // record lines have eyes to report.
    table(f.recTable, 0, {0.9f, 0.1f, 0.2f}, 2);
    uint64_t& t = g_t;
    uint64_t& seq = g_seq;
    g_standDown.store(nullptr);
    g_state.standDownLogged = false;
    g_planeVerdict = 1;
    g_frustumStub = &fakePlanes;
    // One engine frame: the rebuild of both contexts (the setter stores the
    // game's 1.5, then the bracket runs -- unless the case says the hook
    // never fires), the traversal's record test, the builder and its part
    // tests, all at the scale then in force.
    auto frames = [&](uint32_t n, uint32_t records, uint32_t parts, double ms, bool bracket = true) {
        for (uint32_t i = 0; i < n; ++i) {
            f.engineSetsScale(0.5f);
            if (bracket && g_obsSetter) g_obsSetter(f.ctxAt());
            other.engineSetsScale(0.5f);
            if (bracket && g_obsSetter) g_obsSetter(other.ctxAt());
            f.engineRecord();
            for (uint32_t r = 0; r < records; ++r)
                if (g_obsBuilder) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
            for (uint32_t p = 0; p < parts; ++p) {
                f.enginePart(p & 1);
                if (g_obsPart) g_obsPart(f.items(), f.outAt(), f.view[p & 1], true);
            }
            setTiming(++seq, t, ms);
            boundaryAt(t, ms);
            t += 11;
        }
    };
    f.engineSetsScale(0.5f);
    size_t at = g_lines.size();
    applyConfig("auto", 2.0f, false, t);
    check(g_live.load() && g_acting.load(), "acting: auto with observe 0 and the setter hooked acts");
    check(logged("settlement detail: on (auto: acts by scaling the game's LOD scale right after the engine sets it each "
                 "frame (FUN_142819D90): the game's value x k) -- k in [1, 2.00]", at),
          "acting: the configure line names the mode and the mechanism in one clause");
    at = g_lines.size();
    frames(2800, 250, 20, 12.5);   // 1.39 ms over, every cycle two slots: steps of 0.25, then the kick
    check(countLogged("settlement detail: LOD scale scaled: game s 1.500 -> 1.875 (k 1.25), ctx 0x", at) == 1,
          "acting: the first write to the builder's context is logged once, with the game's s and k");
    check(f.scale() == 3.0f && other.scale() == 1.5f,
          "acting: at k 2 the builder's context holds 3.0 after the rebuild; the other context keeps the game's 1.5");
    check(logged("settlement detail (acting): k 1.00 -> 1.25, up 0.25: 29 of the last 30 cycles took two display slots "
                 "with the caller work at the period; the 30's mean caller work 1.39 ms over (more than 1.00 ms over: "
                 "the coarse step); 250 builder records, frame work = caller work per cycle: 12.50 ms vs period 11.11 "
                 "ms -> LOD scale s x k 1.875.", at),
          "acting: a step line names its size, the two-slot cycles, the mean excess, and the scale it will write");
    check(logged("settlement detail (acting): k 1.75 -> 2.00, kick: 30 of the last 30 cycles took two slots for 3.0 s "
                 "at k 1.75: to k_max 2.00 so consecutive frames fit and the runtime returns to full rate; 250 builder "
                 "records, frame work = caller work per cycle: 12.50 ms vs period 11.11 ms -> LOD scale s x k 3.000", at),
          "acting: the trigger held 3 s below k_max: the kick, logged whole with the scale it will write");
    // (Once a window: these 2800 frames end the first window and begin the
    // second, where it still holds.)
    check(countLogged("settlement detail: at the ceiling (k 2.00, s x k 3.000) and still missing 30 of the last 30 "
                      "display slots: the remaining caller work (12.50 ms mean, 1.39 ms over the period) is not "
                      "LOD-elastic; 0 of those misses had the CPU under the period (not ours).", at) == 2,
          "acting: at k_max with every cycle taking two slots for 5 s, the ceiling line, once a window");
    check(logged("(window 1.00..2.00 of max 2.00; 3 up (3 by 0.25), 0 down, 0 resets, 0 clamps; held on foot 0 frames)",
                 at) &&
              logged("; kicks 1; at the ceiling with misses ", at),
          "acting: the summary counts three steps of 0.25 and the kick");
    // The first window: 2729 frames of 11 ms, two rebuilds a frame, the
    // first 30 at k 1 (nothing to scale).
    check(logged("settlement detail (acting): 30.0 s, 2729 frames: k now 2.00, effective s x k 3.000", at) &&
              logged("LOD scale: game s 1.500, held 3.000 (k 2.00); setter calls 5458 (scaled 2699) on 1 pointers "
                     "(called with 2; builder contexts 1); implausible 0; faults 0;", at) &&
              !logged("NOT ACTING", at),
          "acting: the summary's LOD scale follows k, and the setter's counts name one scaled context of the two");
    check(logged("settlement detail (acting) eye A (view bit 1 now): parts tested 10.0/frame (", at) &&
              logged("dropped (the game's setting would have kept it)", at) &&
              logged("disagreements with the engine at the LOD scale it held: parts 0, records 0", at),
          "acting: the eye lines say dropped, and the disagreement gate reads 0 at EDVR's scale");
    at = g_lines.size();
    frames(2800, 250, 20, 12.5);
    check(logged("dropped (the game's setting would have kept it) 10.0/frame (max 10)", at) &&
              logged("engine passed 0.0; dropped", at),
          "acting: every part the game's 1.5 passes and 3.0 fails is dropped, 10 an eye a frame");
    // A setter hook that never fires: the engine's own 1.5 stands, k is above
    // 1, and the summary says NOT ACTING (two windows, so one is whole).
    at = g_lines.size();
    frames(5600, 250, 20, 12.5, false);
    check(f.scale() == 1.5f && logged("held 1.500 (k 2.00); setter calls 0 (scaled 0) on 0 pointers", at) &&
              logged("NOT ACTING: k rose above 1 but FUN_142819D90's hook ran 0 times, so nothing was written", at),
          "acting: a setter hook that never fires reads as setter calls 0 and NOT ACTING while k > 1");
    // Observe switched on while acting: the game's value goes back at once, then never written.
    frames(1, 250, 20, 12.5);
    check(f.scale() == 3.0f, "acting: EDVR's value is in force before observe is switched on");
    at = g_lines.size();
    applyConfig("auto", 2.0f, true, t);
    check(f.scale() == 1.5f && !g_acting.load() && logged("on (auto, observe only: never writes", at),
          "acting: observe on writes the game's value back and stops writing");
    frames(10, 250, 20, 12.5);
    check(f.scale() == 1.5f, "acting: observing, the rebuild's value is never scaled");
    // Off while acting: the game's value is written back, and the off line counts it.
    applyConfig("auto", 2.0f, false, t);
    frames(1, 250, 20, 12.5);
    check(f.scale() == 3.0f, "acting: acting again from the next rebuild");
    at = g_lines.size();
    applyConfig("game", 2.0f, false, t);
    check(f.scale() == 1.5f && !g_live.load() &&
              logged("settlement detail: off (fix.settlement_detail = game: the game's own detail; nothing is observed "
                     "or changed; the game's LOD scale written back to 1 context(s))", at),
          "acting: switching off writes the game's value back and says so");
    frames(10, 250, 20, 12.5);
    check(f.scale() == 1.5f, "acting: off, the rebuild's value is never touched");
    // reduced: k_max at once in the settlement, 1 when it ends.
    at = g_lines.size();
    applyConfig("reduced", 3.0f, false, t);
    check(logged("settlement detail: on (reduced: acts by scaling the game's LOD scale right after the engine sets it "
                 "each frame (FUN_142819D90): the game's value x k) -- k = 3.00 (advanced.settlement_detail_max) at "
                 "once", at),
          "acting: reduced's configure line");
    frames(3, 250, 20, 9.0);
    check(logged("settlement detail (acting): k 1.00 -> 3.00, reduced: in a settlement (>= 200 builder records), k = "
                 "k_max at once; 250 builder records, frame work = caller work per cycle: 9.00 ms vs period 11.11 ms "
                 "-> LOD scale s x k 4.500.", at) && f.scale() == 4.5f,
          "acting: reduced puts k at k_max on the first settlement frame; the next rebuild writes 4.5");
    frames(29, 20, 0, 9.0);
    check(f.scale() == 4.5f, "acting: reduced holds k_max through 29 sparse frames");
    frames(2, 20, 0, 9.0);   // the reset's step line falls inside the 5 s rate limit
    check(g_state.policy.k() == 1.0f && f.scale() == 1.5f,
          "acting: reduced returns k to 1 when the settlement ends; the next rebuild keeps the game's 1.5");
    applyConfig("game", 3.0f, false, t);
    // The setter hook stood down: the governor says it cannot act and never writes.
    g_setterStatusStub = "prologue mismatch at RVA 0x2819D90";
    at = g_lines.size();
    applyConfig("auto", 2.0f, false, t);
    check(!g_acting.load() &&
              logged("on (auto, but it cannot act: the LOD-scale setter hook stood down; observing, never writes)", at) &&
              logged("LOD-scale setter FUN_142819D90 STOOD DOWN (prologue mismatch at RVA 0x2819D90)", at),
          "acting: a refused setter hook is named, and the governor only observes");
    frames(2800, 250, 20, 12.5);
    check(f.scale() == 1.5f && logged("settlement detail (cannot act, observing): 30.0 s", at) &&
              logged("settlement detail (cannot act, observing): k 1.00 -> 1.25, up 0.25", at),
          "acting: without the setter hook nothing is written, and every line says why");
    applyConfig("game", 2.0f, false, t);
    g_setterStatusStub = "hooked";
    // The longest configure line this build can print: auto's policy clause,
    // the frame-work clause before any runtime frame, every hook refused with
    // its longest reason (kinematic_eval_hook.cpp's strings), the plane test
    // mismatched, and an earlier stand-down. main() holds it to 1166.
    g_setterStatusStub = "setter mismatch at RVA 0x2819F47 (s = 1 + (1 - *param_5))";
    g_partStatusStub = "builder call-site mismatch at RVA 0x42B4B85 (mov r8,rax; mov [rsp+30h],rax; lea rcx,[rbp+70h]; "
                       "call FUN_1442B3FC0)";
    g_frustumStub = nullptr;
    g_standDown.store("ctx+0x30 is not committed, writable memory (VirtualQuery)");
    const lodgov::WorkSource source = g_state.source;
    g_state.source = lodgov::WorkSource::None;
    g_timing = NativeTimingSnapshot{};
    at = g_lines.size();
    applyConfig("auto", 4.0f, false, t);
    check(logged("part test FUN_1442B3FC0 STOOD DOWN (builder call-site mismatch at RVA 0x42B4B85 (mov r8,rax; mov "
                 "[rsp+30h],rax; lea rcx,[rbp+70h]; call FUN_1442B3FC0))", at) &&
              logged("plane test FUN_1404F4E10 MISMATCHED (acting drops unverified)", at) &&
              logged("acting STOOD DOWN earlier in this process", at),
          "acting: the longest configure line names every refusal whole");
    applyConfig("game", 4.0f, false, t);
    g_state.source = source;
    g_standDown.store(nullptr);
    g_setterStatusStub = "hooked";
    g_partStatusStub = "hooked";
    g_frustumStub = &fakePlanes;
}

// The cost the design doc quotes: both observers on hot synthetic memory,
// observing and acting (an acting reject runs the plane test).
void caseCost() {
    Fake f;
    freshTable();
    g_live.store(true);
    lodGovernorBuilderObserver(0, f.ctxAt(), 0, f.nibbles());   // registers the context, names it g_ctx
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
    // Acting: EDVR's 2.0 over the game's 1.0, the part an engine reject the
    // game's setting keeps -- the dearest path, with the plane test.
    g_acting.store(true);
    g_frustum.store(&fakePlanes);
    setK(2.0f);
    setterCall(f, 1.0f);
    f.enginePart(0);
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < n; ++i) lodGovernorPartObserver(f.items(), f.outAt(), f.view[0], true);
    QueryPerformanceCounter(&q1);
    const double dropNs = double(q1.QuadPart - q0.QuadPart) * 1e9 / double(fr.QuadPart) / n;
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < n / 10; ++i) setterCall(f, 1.0f);
    QueryPerformanceCounter(&q1);
    const double setterNs = double(q1.QuadPart - q0.QuadPart) * 1e9 / double(fr.QuadPart) / (n / 10);
    uint32_t sums[kCounters];
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < 1000; ++i) sumSlots(sums);
    QueryPerformanceCounter(&q1);
    const double boundaryUs = double(q1.QuadPart - q0.QuadPart) * 1e6 / double(fr.QuadPart) / 1000;
    std::printf("lod_governor_test: cost on this machine, hot synthetic memory: part observer %.1f ns/call (acting, a "
                "dropped reject with the plane test %.1f), record observer %.1f ns/call (3 views), setter observer "
                "%.1f ns/call, the boundary's slot sum %.2f us (%u slots claimed)\n",
                partNs, dropNs, recordNs, setterNs, boundaryUs, g_slotNext.load());
    check(partNs < 500.0 && dropNs < 1000.0 && recordNs < 2000.0 && setterNs < 2000.0,
          "cost: the observers are sub-microsecond");
    restoreScaled();
    g_acting.store(false);
    g_live.store(false);
    setK(1.0f);
    freshTable();
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered, so a crash does not take the FAIL lines before it with it.
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("lod_governor_test: dry-run (no hooks, no game, no files)\n");
        return 0;
    }
    if (argc < 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: lod_governor_test.exe --self-test [--print-log] | --dry-run\n");
        return 2;
    }
    const char* trace = std::getenv("LODGOV_TRACE");
    auto run = [&](const char* name, void (*fn)()) {
        if (trace) std::printf("lod_governor_test: case %s\n", name);
        fn();
    };
    run("policy", casePolicy);
    run("step policy", caseStepPolicy);
    run("foot policy", caseFootPolicy);
    run("reduced policy", caseReducedPolicy);
    run("math", caseMath);
    run("observers", caseObservers);
    run("setter", caseSetter);
    run("acting counts", caseActingCounts);
    run("threads", caseThreads);
    run("boundary", caseBoundary);
    run("foot boundary", caseFootBoundary);
    run("acting boundary", caseActingBoundary);
    run("cost", caseCost);
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
