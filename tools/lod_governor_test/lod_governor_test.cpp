// Build gate for the settlement LOD governor (src/d3d11/lod_governor.*,
// fix.settlement_detail, shipped default auto): the policy on synthetic frame
// sequences -- auto decided once a second of wall time over the cycles
// completed in it, on the display slot (the interval between boundaries;
// longer than 1.5 periods took two): a second of 20 valid cycles or more
// triggers when a tenth of them took two slots and were not GPU-bound (the
// application's GPU render at the period - 0.5 ms or more; with no GPU sample
// a miss is ours), fewer than 20 decide nothing; up 0.25 while its mean
// caller work ran more than 1.0 ms over, else 0.05; ten triggering seconds in
// a row below k_max kick to k_max (one per 30 s), and five clean seconds at a
// time bring k back 0.25 to the pre-kick k + 0.25, then 0.05; down 0.05 after
// five clean seconds, five more before the next; the lever-spent line at
// k_max; the inert lever (two up steps that change the dropped parts by under
// 0.5% of those passed hold k -- no step, no kick -- until the parts tested
// move 20% or 30 s pass); a runtime without caller work holds; random misses
// at 3% and 10%; k_max 6 by default and 8 at most -- and reduced's
// k_max-at-once, never a kick; the cockpit gate (on foot, per the journal
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
// to the app work, named as such in every line, and auto holds; a v5 frame
// without caller work is an invalid sample; the GPU-bound misses, the
// lever-spent line with the GPU beside the caller work, and the inert line.
// Every line fits the real log's line. It then times the observers (the cost
// the design doc quotes). No hooks, no game.
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
// The application-render GPU instrument, faked: off unless a case sets it.
GpuFrameSnapshot g_gpu;
GpuFrameSnapshot gpuFrameSnapshot() noexcept { return g_gpu; }
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
    lodgov::WorkSource source = lodgov::WorkSource::Caller;
    bool gpuValid = false;
    double gpuMs = 0;
    uint32_t tested = 0, passed = 0, dropped = 0;
    bool camValid = true;
    float cam[3] = {0.0f, 0.0f, 0.0f};
    bool expireNext = false;
    uint64_t now() const { return uint64_t(clock); }
    uint64_t next(double cycleMs) const { return uint64_t(clock + cycleMs); }
    lodgov::Step frame(double workMs, double cycleMs, uint32_t records = 679,
                       lodgov::Work work = lodgov::Work::Valid, bool onFoot = false) {
        clock += cycleMs;
        lodgov::FrameSignals s;
        s.records = records;
        s.onFoot = onFoot;
        s.work = work;
        s.source = source;
        s.expired = expireNext;
        expireNext = false;
        s.workMs = workMs;
        s.periodMs = kP;
        s.nowMs = uint64_t(clock);
        s.clockMs = clock;
        s.gpuValid = gpuValid;
        s.gpuMs = gpuMs;
        s.tested = tested;
        s.passed = passed;
        s.dropped = dropped;
        s.camValid = camValid;
        std::memcpy(s.cam, cam, sizeof(s.cam));
        return p.update(s);
    }
    // Frames until a window closes: the first `twoSlots` of them take two
    // slots, the rest one. The step the closing frame returned.
    lodgov::Step window(double workMs, int twoSlots, uint32_t records = 679) {
        lodgov::Step s = lodgov::Step::None;
        for (int i = 0; i < 400; ++i) {
            s = frame(workMs, i < twoSlots ? kTwo : kP, records);
            if (p.windowClosed()) return s;
        }
        return s;
    }
    // Windows until one returns a step (at most n): how many it took.
    int until(lodgov::Step want, double workMs, int twoSlots, int n = 100) {
        for (int i = 1; i <= n; ++i)
            if (window(workMs, twoSlots) == want) return i;
        return 0;
    }
};
constexpr int kAll = 1000;   // every cycle of a window two slots
constexpr int kFine = 12;    // 12 of ~79 cycles two slots (15%): triggers, a fine step
constexpr double kClean = kP - 1.2;   // a clean second's caller work: 1.2 ms to spare

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
    // The display slot, and whose a miss is.
    {
        Feed f;
        f.frame(kP, kP, 150);
        check(!f.p.cycle().measured, "policy: the first boundary after enabling has no interval");
        f.frame(kP, 1.49 * kP, 150);
        check(f.p.cycle().measured && !f.p.cycle().missed && std::fabs(f.p.cycle().ms - 1.49 * kP) < 1e-6,
              "policy: a cycle of 1.49 periods is measured and fits its slot");
        f.frame(kP, 1.51 * kP, 150);
        check(f.p.cycle().missed && !f.p.cycle().gpuBound && !f.p.cycle().unexplained,
              "policy: 1.51 periods took two slots; the caller work at the period, no GPU sample: the CPU's");
        f.frame(kP - 1.0, kTwo, 150);
        check(f.p.cycle().missed && f.p.cycle().unexplained,
              "policy: two slots with the caller work 1 ms under and no GPU sample: unexplained, not the CPU's");
        f.gpuValid = true;
        f.gpuMs = 11.5;
        f.frame(kP + 1.0, kTwo, 150);
        check(f.p.cycle().missed && f.p.cycle().gpuBound,
              "policy: two slots with the application's GPU render at 11.5 ms (period - 0.5 or more): GPU-bound");
        f.gpuMs = 6.0;
        f.frame(kP + 1.0, kTwo, 150);
        check(f.p.cycle().missed && !f.p.cycle().gpuBound && !f.p.cycle().unexplained,
              "policy: two slots with the GPU at 6 ms and the caller work over: the CPU's");
        f.frame(kP - 1.0, kTwo, 150);
        check(f.p.cycle().unexplained, "policy: two slots with the GPU at 6 ms and the caller work under: unexplained");
    }
    // A decision a second; 150 records never rise; a second of 15 fresh
    // cycles decides nothing and breaks no run.
    {
        Feed f;
        f.p.configure(2.0f);
        bool any = false;
        for (int i = 0; i < 300; ++i) any |= f.frame(14.0, kTwo, 150) != Step::None;   // 6.7 s
        check(!any && f.p.steps() == 0 && !f.p.inSettlement() && f.p.windows() >= 6 && f.p.windows() <= 7 &&
                  f.p.lastWindow().triggered && f.p.triggerRun() >= 6,
              "policy: a decision a second; seconds of two-slot cycles with 150 records trigger but never rise");
        while (!f.p.windowClosed()) f.frame(14.0, kTwo, 150);
        const uint32_t run = f.p.triggerRun();
        int n = 0;
        Step s = Step::None;
        do {
            s = f.frame(14.0, kTwo, 679, n++ < 15 ? Work::Valid : Work::None);
        } while (!f.p.windowClosed());
        check(s == Step::None && !f.p.lastWindow().decided && f.p.lastWindow().cycles == 15 &&
                  f.p.triggerRun() == run && f.p.steps() == 0,
              "policy: a second of 15 fresh cycles decides nothing and breaks no run");
        // The evidence expires: the ring, the second in progress and the runs
        // go, and k holds.
        f.frame(14.0, kTwo);
        f.expireNext = true;
        const Step e = f.frame(14.0, kTwo);
        check(e == Step::None && f.p.samples() == 1 && f.p.triggerRun() == 0 && !f.p.cycle().measured &&
                  f.p.expiries() == 1 && f.p.k() == 1.0f,
              "policy: an expiry empties the ring, drops the second in progress and the runs, and holds k");
    }
    // The trigger's line, the step's size, the density gate, k_max, down.
    {
        Feed f;
        f.p.configure(2.0f);
        f.window(kP, 0);   // a first second, every cycle in its slot: nothing
        const Step s8 = f.window(kP, 8);
        const WindowResult w8 = f.p.lastWindow();
        const Step s9 = f.window(kP, 9);
        const WindowResult w9 = f.p.lastWindow();
        check(s8 == Step::None && w8.decided && !w8.triggered && w8.misses == 8 && s9 == Step::Up && w9.triggered &&
                  w9.misses == 9 && f.p.k() == 1.05f && f.p.upQuanta() == 1,
              "policy: 8 of ~82 cycles taking two slots hold, 9 of ~81 trigger: up 0.05 (under a quarter, the mean at the period)");
        std::printf("lod_governor_test: the trigger's line at 90 Hz: %u of %u two-slot cycles hold, %u of %u step\n",
                    w8.misses, w8.cycles, w9.misses, w9.cycles);
        check(f.window(12.9, kAll) == Step::Up && f.p.upQuanta() == kCoarseQuanta && f.p.k() == 1.30f,
              "policy: a triggering second, every cycle two slots and 1.79 ms over: up 0.25");
        check(f.window(12.9, kAll, 180) == Step::None && f.p.k() == 1.30f && f.p.inSettlement(),
              "policy: 150-199 records hold k and the settlement");
        for (int i = 0; i < 3; ++i) f.window(12.9, kAll);
        check(f.p.k() == 2.0f && f.p.upHeld(), "policy: k stops at k_max, the last 0.25 held to it");
        check(f.window(12.9, kAll) == Step::None && f.p.k() == 2.0f, "policy: at k_max nothing steps further");
        check(f.until(Step::Down, kClean, 0) == 5 && f.p.k() == 1.95f && f.p.downQuanta() == 1 && !f.p.relaxStep() &&
                  f.p.haveGood() && f.p.goodK() == 2.0f,
              "policy: five seconds with headroom in a row: down 0.05, a trial against the working point 2.00");
        check(f.until(Step::Down, kClean, 0) == 5 && f.p.k() == 1.90f, "policy: ... then five more before the next");
        f.window(kClean, 0);
        f.window(kClean, 0);
        f.window(kP - 0.5, 0);   // clean, but only 0.5 ms under
        check(f.until(Step::Down, kClean, 0) == 5, "policy: a clean second without the millisecond breaks the run: five more");
        f.p.configure(1.5f);
        check(f.frame(kP, kP) == Step::Clamp && f.p.k() == 1.5f, "policy: a lowered k_max clamps k at once");
        Step last = Step::None;
        for (int i = 0; i < 29; ++i) last = f.frame(kP, kP, 20);
        check(last == Step::None && f.p.k() == 1.5f && f.p.inSettlement(), "policy: 29 frames under 150 records hold");
        check(f.frame(kP, kP, 20) == Step::Reset && f.p.k() == 1.0f && !f.p.inSettlement() && !f.p.haveGood(),
              "policy: the 30th frame under 150 records resets k to 1 and forgets the working point");
        f.frame(kP, kP, 20);
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
}

// auto, decided once a second (refinement 4b and the review of 2026-09-23):
// the step's size, the kick and its trial, the relaxation, the working point,
// the GPU's and the unexplained misses, a runtime without caller work, the
// lever's benefit, random misses, and a density the lever itself cuts.
void caseStepPolicy() {
    using namespace lodgov;
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto uniform = [&]() {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return double(rng >> 11) / 9007199254740992.0;
    };
    // The size: a quarter of the cycles the CPU's is the coarse step even with
    // the mean at the period -- the review's finding 2: 31% misses at a mean
    // near the period took eleven fine steps.
    {
        Feed f;
        f.window(kP, 0);
        const Step s17 = f.window(kP, 17);
        const WindowResult w17 = f.p.lastWindow();
        const int q17 = f.p.upQuanta();
        const Step s21 = f.window(kP, 21);
        const WindowResult w21 = f.p.lastWindow();
        check(s17 == Step::Up && q17 == 1 && s21 == Step::Up && f.p.upQuanta() == kCoarseQuanta &&
                  w17.misses * 4 < w17.cycles && w21.misses * 4 >= w21.cycles,
              "steps: at a mean at the period, 17 of ~74 cycles the CPU's take 0.05, 21 of ~69 (a quarter) take 0.25");
        std::printf("lod_governor_test: the coarse line at 90 Hz, the mean at the period: %u of %u step 0.05, %u of %u "
                    "step 0.25\n", w17.misses, w17.cycles, w21.misses, w21.cycles);
    }
    // 30% of 90 Hz cycles two slots at the period, whatever k: 0.25 a second
    // (a quarter or more), the kick at the 10th second from where the steps
    // have got to -- a trial the fifth second at k_max fails, so the pre-kick
    // k comes back at once and no kick follows for 60 s; the steps climb
    // again to k_max, where five triggering seconds say the lever is spent.
    {
        Feed f;
        int ups = 0, coarse = 0, kicks = 0, restores = 0;
        float kickFrom = 0, restoredTo = 0;
        uint32_t kickWindow = 0, restoreWindow = 0, maxWindow = 0, spentWindow = 0;
        for (int i = 0; i < 3000 && !spentWindow; ++i) {   // ~43 s at most
            const bool two = std::floor((i + 1) * 0.3) > std::floor(i * 0.3);
            const float k0 = f.p.k();
            const Step s = f.frame(kP, two ? kTwo : kP);
            if (s == Step::Up) {
                ++ups;
                coarse += f.p.upQuanta() == kCoarseQuanta;
            }
            if (s == Step::Kick) {
                ++kicks;
                kickFrom = k0;
                kickWindow = f.p.windows();
            }
            if (s == Step::Restore) {
                ++restores;
                restoredTo = f.p.k();
                restoreWindow = f.p.windows();
            }
            if (!maxWindow && restores && f.p.k() == 6.0f) maxWindow = f.p.windows();
            if (f.p.ceilingMissing()) spentWindow = f.p.windows();
        }
        check(kicks == 1 && kickWindow == 10 && kickFrom == 3.25f && restores == 1 && restoreWindow == 15 &&
                  restoredTo == 3.25f && f.p.restoredAfterKick() && ups == coarse && ups == 20 && maxWindow == 26 &&
                  spentWindow == 31 && f.p.ceilingOutcome() == Outcome::Unknown,
              "steps: 30% at the period: 0.25 a second, the kick at the 10th from 3.25, restored at the 15th, back up "
              "to 6.00 by the 26th, spent at the 31st; no parts measured: the outcome unknown");
        std::printf("lod_governor_test: 30%% two-slot cycles at the period: %d steps of 0.25, the kick at second %u from "
                    "%.2f, restored to %.2f at second %u, k_max again at second %u, the lever spent at second %u\n",
                    ups, kickWindow, double(kickFrom), double(restoredTo), restoreWindow, maxWindow, spentWindow);
    }
    // A failed kick blocks the next for 60 s: 15% misses at the period (fine
    // steps) with k_max 8 -- the kick at the 10th second, restored at the
    // 15th; ten more triggering seconds below k_max kick again only once 60 s
    // have passed since the restore.
    {
        Feed f;
        f.p.configure(8.0f);
        uint32_t kickAt[3] = {}, restoreAt[3] = {};
        int kicks = 0, restores = 0;
        while (f.now() < 90000 && kicks < 3) {
            const Step s = f.window(kP, kFine);
            if (s == Step::Kick) kickAt[kicks++] = f.p.windows();
            if (s == Step::Restore && restores < 3) restoreAt[restores++] = f.p.windows();
        }
        check(kicks == 2 && kickAt[0] == 10 && restoreAt[0] == 15 && kickAt[1] >= restoreAt[0] + 60 &&
                  kickAt[1] <= restoreAt[0] + 61 && restores >= 1,
              "steps: a failed kick's restore blocks the next kick for 60 s");
        std::printf("lod_governor_test: 15%% at the period, k_max 8: kicks at seconds %u and %u, the first restored at "
                    "second %u\n", kickAt[0], kickAt[1], restoreAt[0]);
    }
    // Every cycle two slots at 12.9 ms until k reaches 2.50: six steps of
    // 0.25 clear it, no kick.
    {
        Feed f;
        int coarse = 0, kicks = 0;
        for (int i = 0; i < 1000; ++i) {
            const bool two = f.p.k() < 2.5f;
            const Step s = f.frame(two ? 12.9 : 10.9, two ? kTwo : kP);
            coarse += s == Step::Up && f.p.upQuanta() == kCoarseQuanta;
            kicks += s == Step::Kick;
        }
        check(coarse == 6 && kicks == 0 && f.p.k() == 2.5f,
              "steps: two-slot cycles at 12.9 ms until k 2.50: six steps of 0.25 clear them, no kick");
    }
    // A successful kick from 2.90 and its relaxation: the fifth second at
    // k_max clean, then 0.25 per five clean seconds to 3.15 (the pre-kick k
    // + 0.25) in about 60 s, then 0.05.
    {
        Feed f;
        for (int i = 0; i < 5; ++i) {   // 1.25 .. 2.25, a quiet second between: never ten in a row
            f.window(12.9, kAll);
            f.window(kP, 0);
        }
        for (int i = 0; i < 4; ++i) {   // 2.30 .. 2.45
            f.window(kP, kFine);
            f.window(kP, 0);
        }
        check(f.p.k() == 2.45f && !f.p.relaxing(), "steps: triggering seconds with quiet ones between: 2.45, no kick");
        int ups = 0;
        Step s = Step::None;
        for (int i = 0; i < 10; ++i) {
            s = f.window(kP, kFine);
            ups += s == Step::Up;
        }
        check(s == Step::Kick && ups == 9 && f.p.preKickK() == 2.9f && f.p.k() == 6.0f && f.p.relaxing() &&
                  f.p.kickTrial() && f.p.relaxTargetK() == 3.15f,
              "steps: ten triggering seconds in a row: 0.05 a second to 2.90, then the kick from 2.90 to 6.00, a trial");
        const uint64_t cleanFrom = f.now();
        int relax = 0, other = 0;
        uint64_t reached = 0;
        bool trialPassed = false;
        for (int i = 0; i < 100 && !reached; ++i) {
            s = f.window(kClean, 0);
            if (i == 4) trialPassed = !f.p.kickTrial() && s == Step::Down;
            if (s == Step::Down) (f.p.relaxStep() ? relax : other)++;
            if (f.p.k() == 3.15f) reached = f.now();
        }
        check(trialPassed && relax == 12 && other == 0 && reached && reached - cleanFrom >= 59000 &&
                  reached - cleanFrom <= 62000 && !f.p.relaxing(),
              "steps: the fifth second at k_max clean passes the trial; 0.25 per five seconds (the last 0.10) to 3.15 in ~60 s");
        std::printf("lod_governor_test: the kick from 2.90 to 6.00, then %d relaxing steps to 3.15 in %.1f s\n", relax,
                    double(reached - cleanFrom) / 1000.0);
        check(f.until(Step::Down, kClean, 0) == 5 && f.p.downQuanta() == 1 && !f.p.relaxStep() && f.p.k() == 3.1f,
              "steps: ... then the ordinary 0.05");
    }
    // A recovery trial that fails: a second triggering within 10 s of a step
    // down restores the working point at once and doubles the wait before the
    // next trial -- 5, 10, 20, 40, 60 clean seconds; back to 5 after 60 s
    // without a trigger. The same after a relaxing step.
    {
        Feed f;
        for (int i = 0; i < 9; ++i) f.window(12.9, kAll);   // 1.25 .. 3.25, 0.25 a second
        const bool at325 = f.p.k() == 3.25f;
        uint32_t waits[6] = {};
        bool restoresOk = true;
        for (int t = 0; t < 6; ++t) {
            const int w = f.until(Step::Down, kClean, 0, 70);
            waits[t] = uint32_t(w);
            restoresOk &= f.p.k() == 3.2f && f.p.goodK() == 3.25f;
            const Step r = f.window(kP, kFine);   // the next second triggers: the trial failed
            restoresOk &= r == Step::Restore && f.p.k() == 3.25f && !f.p.restoredAfterKick();
        }
        check(at325 && restoresOk && waits[0] == 5 && waits[1] == 10 && waits[2] == 20 && waits[3] == 40 &&
                  waits[4] == 60 && waits[5] == 60 && f.p.downWait() == 60,
              "steps: each failed recovery trial restores 3.25 at once; the waits 5, 10, 20, 40, 60, 60 clean seconds");
        // A step down that holds 60 s without a trigger (quiet seconds, no
        // headroom): the wait is 5 again. The waits themselves never reset
        // it -- 60 clean seconds are 60 s without a trigger too.
        const int w60 = f.until(Step::Down, kClean, 0, 70);
        for (int i = 0; i < 61; ++i) f.window(kP, 0);
        check(w60 == 60 && f.p.k() == 3.2f && f.p.downWait() == 5 && f.until(Step::Down, kClean, 0) == 5 &&
                  f.p.k() == 3.15f,
              "steps: a step down that holds 60 s without a trigger resets the wait to 5 clean seconds");
        // Near the working point the step is fine, a quarter of misses or not:
        // 3.15 after the trial, 12 quiet seconds (past its 10 s), then 36% of
        // the cycles two slots: 0.05, back to the working point 3.20.
        for (int i = 0; i < 12; ++i) f.window(kP, 0);
        check(f.window(kP, 24) == Step::Up && f.p.upQuanta() == 1 && f.p.upNearGood() && f.p.k() == 3.2f,
              "steps: within 0.25 below the working point the step is 0.05, even at a quarter of misses");
        // At the working point it triggers again: forgotten, and the size
        // rule is the ordinary one.
        check(f.window(kP, 24) == Step::Up && f.p.upQuanta() == kCoarseQuanta && !f.p.haveGood() &&
                  f.p.k() == 3.45f,
              "steps: triggering at the working point forgets it: 0.25 again");
        // A relaxing step that fails: the k before it at once, the
        // relaxation over, 0.05 after ten clean seconds.
        Feed g;
        for (int i = 0; i < 10; ++i) g.window(kP, kAll);    // 0.25 a second to 3.25, the kick at the 10th
        for (int i = 0; i < 10; ++i) g.window(kClean, 0);   // the trial passes; relaxing: 5.75, 5.50
        const bool relaxed = g.p.k() == 5.5f && g.p.relaxing();
        const Step r = g.window(kP, kAll);
        check(relaxed && r == Step::Restore && g.p.k() == 5.75f && !g.p.relaxing() && g.p.downWait() == 10,
              "steps: a second triggering within 10 s of a relaxing step restores 5.75 and ends the relaxation");
        check(g.until(Step::Down, kClean, 0) == 10 && g.p.downQuanta() == 1 && !g.p.relaxStep(),
              "steps: ... then 0.05 after ten clean seconds");
    }
    // The GPU's misses and the unexplained ones: two-slot cycles with the
    // application's GPU render at 11.5 ms never trigger and are counted
    // GPU-bound; at 6 ms with the caller work over, they trigger; with no
    // GPU sample and the caller work over, they trigger; with the caller work
    // under the period - 0.3 ms and the GPU under or unknown, they are
    // unexplained and never trigger.
    {
        Feed f;
        f.gpuValid = true;
        f.gpuMs = 11.5;
        bool any = false;
        for (int i = 0; i < 20; ++i) any |= f.window(kP + 0.8, kAll) != Step::None;
        check(!any && f.p.k() == 1.0f && f.p.lastWindow().misses == 0 && f.p.lastWindow().gpuBound > 40 &&
                  f.p.triggerRun() == 0,
              "steps: two-slot cycles with the GPU at 11.5 ms never trigger: GPU-bound, counted apart");
        f.gpuMs = 6.0;
        check(f.window(kP + 0.8, kAll) == Step::Up && f.p.k() == 1.25f,
              "steps: with the GPU at 6 ms and the caller work over, they trigger (every cycle: 0.25)");
        f.gpuValid = false;
        check(f.window(kP + 0.8, kAll) == Step::Up && f.p.k() == 1.5f, "steps: with no GPU sample, they trigger");
        any = false;
        for (int i = 0; i < 10; ++i) any |= f.window(kP - 0.5, kAll) != Step::None;
        f.gpuValid = true;
        for (int i = 0; i < 10; ++i) any |= f.window(kP - 0.5, kAll) != Step::None;
        check(!any && f.p.k() == 1.5f && f.p.lastWindow().unexplained > 40 && f.p.lastWindow().misses == 0,
              "steps: the caller work 0.5 ms under and the GPU under or unknown: unexplained, never a trigger");
    }
    // A runtime without caller work (timing v3/v4): nothing triggers.
    {
        Feed f;
        f.source = WorkSource::App;
        bool any = false;
        for (int i = 0; i < 20; ++i) any |= f.window(12.9, kAll) != Step::None;
        check(!any && f.p.k() == 1.0f && f.p.holding() && !f.p.lastWindow().triggered,
              "steps: without caller work auto never steps up");
    }
    // The lever's benefit (the review's finding on the inert rule): the parts
    // passed at EDVR's scale and the caller work across each up step.
    {
        // 4,800 parts passed a frame and the caller work unchanged through
        // two 0.25 steps: inert -- no step and no kick; the down rule still
        // applies.
        Feed f;
        f.tested = 5500;
        f.passed = 4800;
        f.dropped = 4;
        int ups = 0, kicks = 0;
        uint64_t heldAt = 0;
        for (int i = 0; i < 25; ++i) {
            const Step s = f.window(12.9, kAll);
            ups += s == Step::Up;
            kicks += s == Step::Kick;
            if (!heldAt && f.p.inert()) heldAt = f.now();
        }
        check(ups == 2 && kicks == 0 && f.p.inert() && f.p.inertHolds() == 1 && f.p.k() == 1.5f &&
                  f.p.lastEffect() == Effect::NoBenefit && f.p.runCoarse() == 2 && f.p.runFine() == 0 &&
                  f.p.runFigures().tested[0] == 5500.0 && f.p.runFigures().tested[1] == 5500.0 &&
                  f.p.runFigures().passed[0] == 4800.0 && f.p.runFigures().passed[1] == 4800.0 &&
                  std::fabs(f.p.runFigures().caller[0] - 12.9) < 1e-9 && f.p.triggerRun() == 0 &&
                  f.p.judgedNone() == 2,
              "steps: two 0.25 steps moving neither the parts tested and passed nor the caller work: inert, no step, "
              "no kick");
        // 30 s after the hold one step is retried; without a benefit the hold goes on.
        int w = 0;
        while (f.p.retries() == 0 && w < 12) {
            f.window(12.9, kAll);
            ++w;
        }
        const uint64_t retriedAt = f.now();
        check(f.p.retries() == 1 && f.p.k() == 1.75f && retriedAt - heldAt >= 29000 && retriedAt - heldAt <= 32000,
              "steps: 30 s into the hold one step is retried");
        for (int i = 0; i < 25; ++i) f.window(12.9, kAll);
        check(f.p.inert() && f.p.retries() == 1 && f.p.k() == 1.75f && f.p.rearms() == 0,
              "steps: a retry without a benefit keeps the hold, and the next retry waits 30 s");
        // The view changes -- the parts tested move 25% -- and a step is
        // retried at once; this one crosses a threshold past the plateau (the
        // passed parts fall 17%): a benefit, re-armed.
        f.tested = 6875;
        check(f.window(12.9, kAll) == Step::Up && f.p.retries() == 2 && f.p.k() == 2.0f && f.p.retrying(),
              "steps: a 25% change in the parts tested retries a step at once");
        f.passed = 4000;   // what 2.00 passes here: a threshold past the plateau
        f.window(12.9, kAll);
        check(!f.p.inert() && f.p.rearms() == 1 && f.p.lastEffect() == Effect::Benefit && f.p.k() == 2.25f,
              "steps: a retried step past the plateau shows a benefit and re-arms; the steps go on");
        // Inert, the down rule still applies.
        Feed g;
        g.tested = 5500;
        g.passed = 4800;
        for (int i = 0; i < 4; ++i) g.window(12.9, kAll);
        const float held = g.p.k();
        check(g.p.inert() && held == 1.5f && g.until(Step::Down, kClean, 0) == 5 && g.p.k() == 1.45f,
              "steps: inert, clean seconds still give detail back");
        // Four 0.05 steps without a benefit: inert too.
        Feed h;
        h.tested = 5500;
        h.passed = 4800;
        ups = 0;
        for (int i = 0; i < 8; ++i) ups += h.window(12.9 - 1.8, kFine) == Step::Up;
        check(ups == 4 && h.p.inert() && h.p.runFine() == 4 && h.p.runCoarse() == 0,
              "steps: four 0.05 steps without a benefit: inert");
        // Each step passes 3% fewer parts: a benefit every time, it keeps stepping.
        Feed m;
        m.tested = 5500;
        ups = 0;
        for (int i = 0; i < 8; ++i) {
            m.passed = uint32_t(4800.0 * (1.0 - 0.03 * (double(m.p.k()) - 1.0) / 0.25));
            ups += m.window(12.9, kAll) == Step::Up;
        }
        check(ups == 8 && !m.p.inert() && m.p.inertHolds() == 0 && m.p.lastEffect() == Effect::Benefit,
              "steps: each step passing 3% fewer parts keeps stepping");
        // The passed parts unchanged but the caller work 0.25 ms lower a
        // step: a benefit (6 steps keep it over the period - 0.3 ms, where a
        // two-slot cycle would be unexplained).
        Feed c;
        c.tested = 5500;
        c.passed = 4800;
        ups = 0;
        for (int i = 0; i < 6; ++i) ups += c.window(12.9 - 0.25 * (double(c.p.k()) - 1.0) / 0.25, kAll) == Step::Up;
        check(ups == 6 && !c.p.inert() && c.p.lastEffect() == Effect::Benefit,
              "steps: the caller work falling 0.25 ms a step is a benefit with the passed parts unchanged");
        // No part tests at all: not judged, never a reason to hold.
        Feed u;
        ups = 0;
        for (int i = 0; i < 6; ++i) ups += u.window(12.9, kAll) == Step::Up;
        check(ups == 6 && !u.p.inert() && u.p.lastEffect() == Effect::NotJudged,
              "steps: without part tests the benefit is not judged and the steps go on");
        // A loading scene (07:15: the inert line during the approach): the
        // records rising 400 -> 680 across each second and the parts with
        // them -- the steps are not judged, never a hold.
        Feed ld;
        ups = 0;
        for (int wnd = 0; wnd < 6; ++wnd) {
            int i = 0;
            Step s = Step::None;
            do {
                const double rise = i < 44 ? i / 44.0 : 1.0;
                const uint32_t records = uint32_t(400.0 + 280.0 * rise);
                ld.tested = 8 * records;
                ld.passed = 7 * records;
                s = ld.frame(12.9, kTwo, records);
                ++i;
            } while (!ld.p.windowClosed());
            ups += s == Step::Up;
        }
        check(ups == 6 && !ld.p.inert() && ld.p.lastEffect() == Effect::NotJudged && ld.p.judgedNone() == 0 &&
                  ld.p.judgedBenefit() == 0 && ld.p.notJudged() >= 5,
              "steps: a loading scene (records rising 400 -> 680 across each second): not judged, never a hold");
        // A stable scene where a step takes 10% of the parts tested (whole
        // records culled before the builder) with the passed parts unchanged:
        // a benefit.
        Feed tt;
        tt.passed = 4800;
        for (int i = 0; i < 2; ++i) {
            tt.tested = uint32_t(5500.0 * (1.0 - 0.1 * (double(tt.p.k()) - 1.0) / 0.25) + 0.5);
            tt.window(12.9, kAll);
        }
        const EffectFigures tf = tt.p.lastFigures();
        check(tt.p.lastEffect() == Effect::Benefit && tf.tested[0] == 5500.0 && tf.tested[1] == 4950.0 &&
                  tf.passed[0] == 4800.0 && tf.passed[1] == 4800.0,
              "steps: a stable scene where the parts tested fall 10% with the passed unchanged: a benefit");
        // The outcome at the ceiling against the k = 1 baseline of the same
        // view (k_max 1.50, every cycle two slots whatever k): each step
        // takes 10% of the parts tested -- residual benefit, both ends given.
        Feed br;
        br.p.configure(1.5f);
        br.passed = 4000;
        for (int i = 0; i < 12 && !br.p.ceilingMissing(); ++i) {
            br.tested = uint32_t(5500.0 * (1.0 - 0.1 * (double(br.p.k()) - 1.0) / 0.25) + 0.5);
            br.window(12.9, kAll);
        }
        EffectFigures bf;
        const Outcome bo = br.p.ceilingOutcome(&bf);
        check(br.p.ceilingMissing() && br.p.haveBaseline() && bo == Outcome::Residual && bf.baseline &&
                  bf.tested[0] == 5500.0 && bf.tested[1] == 4400.0 && bf.passed[0] == 4000.0 &&
                  bf.passed[1] == 4000.0,
              "steps: at the ceiling against the k 1 baseline of this view, tested 5,500 -> 4,400: residual benefit");
        // Nothing moved: no observed benefit, against the same baseline.
        Feed bn;
        bn.p.configure(1.5f);
        bn.tested = 5500;
        bn.passed = 4000;
        for (int i = 0; i < 12 && !bn.p.ceilingMissing(); ++i) bn.window(12.9, kAll);
        const Outcome no = bn.p.ceilingOutcome(&bf);
        check(bn.p.ceilingMissing() && no == Outcome::NoBenefit && bf.baseline && bf.tested[1] == 5500.0 &&
                  bf.passed[1] == 4000.0,
              "steps: at the ceiling with nothing moved against the baseline: no observed benefit");
        // Another view -- the eye camera 5 m away -- and the baseline does not
        // answer; the last judged step does.
        bn.cam[0] = 5.0f;
        bn.window(12.9, kAll);
        bn.p.ceilingOutcome(&bf);
        check(!bf.baseline, "steps: with the eye camera 5 m from the baseline's, the last judged step answers instead");
    }
    // Cycles taking two slots at random at a fixed rate (a fixed-seed
    // generator, 90 Hz, the caller work at the period): 3% for 300 s steps at
    // most once; 10% for 60 s steps on a good part of the seconds.
    {
        Feed f;
        int ups = 0, kicks = 0;
        while (f.now() < 301000) {
            const Step s = f.frame(kP, uniform() < 0.03 ? kTwo : kP);
            ups += s == Step::Up;
            kicks += s == Step::Kick;
        }
        check(ups <= 1 && kicks == 0, "steps: two-slot cycles at random at 3% for 300 s: at most one step up");
        Feed g;
        int ups10 = 0, kicks10 = 0;
        uint32_t decided = 0;
        while (g.now() < 61000) {
            const Step s = g.frame(kP, uniform() < 0.10 ? kTwo : kP);
            ups10 += s == Step::Up;
            kicks10 += s == Step::Kick;
            decided += g.p.windowClosed() && g.p.lastWindow().decided;
        }
        check(ups10 + kicks10 >= int(decided) / 3, "steps: at 10% for 60 s it steps on a good part of the seconds");
        std::printf("lod_governor_test: two-slot cycles at random, 90 Hz: 3%% for 300 s -> %d up steps; 10%% for 60 s -> "
                    "%d up steps and %d kicks in %u seconds\n", ups, ups10, kicks10, decided);
    }
    // The first acting flight's slope (12.9 ms at k 1, 10.6 at 2.70, a line),
    // a cycle taking two slots while the caller work runs more than 0.3 ms
    // past the period. Printed: where the ramp ends and what it costs.
    {
        Feed f;
        int coarse = 0, fine = 0, kicks = 0, downs = 0, restores = 0;
        uint64_t lastUp = 0;
        const uint64_t t0 = f.now();
        for (int i = 0; i < 20000; ++i) {   // ~220 s
            const double work = 12.9 - (12.9 - 10.6) / 1.7 * (double(f.p.k()) - 1.0);
            const Step s = f.frame(work, work > kP + 0.3 ? kTwo : kP);
            if (s == Step::Up) {
                (f.p.upQuanta() == kCoarseQuanta ? coarse : fine)++;
                lastUp = f.now();
            }
            kicks += s == Step::Kick;
            downs += s == Step::Down;
            restores += s == Step::Restore;
        }
        const double settled = 12.9 - (12.9 - 10.6) / 1.7 * (double(f.p.k()) - 1.0);
        check(settled <= kP + 0.3 && kicks == 0 && f.p.k() == 2.25f,
              "steps: on the flight's slope 0.25 steps end at 2.25 with every cycle in its slot, no kick");
        std::printf("lod_governor_test: the flight's slope (12.9 ms at k 1, 10.6 at 2.70): %d steps of 0.25, %d of 0.05 "
                    "by %.1f s, %d kicks, %d down, %d restores, settling at k %.2f (%.2f ms)\n",
                    coarse, fine, double(lastUp - t0) / 1000.0, kicks, downs, restores, double(f.p.k()), settled);
    }
    // A density the lever itself cuts (the review's conditional risk): the
    // builder's records fall from 680 at k 1 to 140 at k 2 as whole records
    // drop out. The current behaviour, pinned until a flight shows it: at
    // 140 the settlement ends after 30 frames and k resets to 1, and the
    // ramp starts over.
    {
        Feed f;
        int resets = 0, ups = 0;
        while (f.now() < 31000) {
            const double k = f.p.k();
            const double fall = 680.0 - 540.0 * (k - 1.0);
            const uint32_t records = uint32_t(fall > 140.0 ? fall : 140.0);
            const Step s = f.frame(12.9, kTwo, records);
            resets += s == Step::Reset;
            ups += s == Step::Up;
        }
        check(resets >= 2 && ups >= 8, "steps: records falling to 140 as k rises: k resets to 1 and the ramp repeats");
        std::printf("lod_governor_test: records 680 -> 140 as k rises 1 -> 2: %d resets and %d up steps in 30 s\n",
                    resets, ups);
    }
}

// The cockpit gate: on foot (Status.json via the journal watcher) k is 1 at
// once and held there, exactly as outside a settlement; aboard again the ramp
// starts over. reduced alike.
void caseFootPolicy() {
    using namespace lodgov;
    Feed f;
    for (int i = 0; i < 3; ++i) f.window(12.9, kAll);
    check(f.p.k() == 1.75f && f.p.inSettlement(), "foot: in the cockpit at a busy settlement k has risen");
    check(f.frame(12.9, kTwo, 679, Work::Valid, true) == Step::Foot && f.p.k() == 1.0f && !f.p.inSettlement() &&
              f.p.samples() == 0 && f.p.triggerRun() == 0 && !f.p.triggered() && !f.p.inert() && !f.p.haveGood(),
          "foot: on foot k is 1 at once, the settlement, the samples, the runs and the working point forgotten");
    bool any = false;
    for (int i = 0; i < 1000; ++i) any |= f.frame(14.0, kTwo, 679, Work::Valid, true) != Step::None;
    check(!any && f.p.k() == 1.0f && !f.p.inSettlement(), "foot: held at 1 on foot, however long the frame and however dense");
    f.p.configure(1.5f);
    check(f.frame(14.0, kTwo, 679, Work::Valid, true) == Step::None, "foot: a lowered k_max on foot has nothing to clamp");
    f.p.configure(6.0f);
    // Aboard at the settlement: the first boundary has no interval, and the
    // k from before the hold comes back in one step.
    const Step back = f.frame(12.9, kTwo);
    check(!f.p.cycle().measured && back == Step::Aboard && f.p.k() == 1.75f,
          "foot: aboard again at the settlement, no interval, and the k from before the hold at once");
    // The 06:53 case: 3.45 held, 2000 frames on foot, 680 records again from
    // the first frame aboard -- k 3.45 in one step, no ramp.
    Feed g;
    for (int i = 0; i < 9; ++i) g.window(12.9, kAll);   // 0.25 a second to 3.25
    g.window(kP, 0);                                     // a quiet second: no kick
    for (int i = 0; i < 4; ++i) g.window(kP, kFine);     // 0.05 a second to 3.45
    const bool at345 = g.p.k() == 3.45f;
    Step s = g.frame(12.9, kTwo, 680, Work::Valid, true);
    bool quiet = true;
    for (int i = 1; i < 2000; ++i) quiet &= g.frame(12.9, kTwo, 680, Work::Valid, true) == Step::None;
    const bool held1 = g.p.k() == 1.0f;
    int others = 0, aboard = 0;
    float firstK = 0;
    for (int i = 0; i < 450; ++i) {   // 5 s aboard, 680 records from the first frame, every cycle in its slot
        const Step a = g.frame(kP, kP, 680);
        if (i == 0) firstK = g.p.k();
        aboard += a == Step::Aboard;
        others += a != Step::Aboard && a != Step::None;
    }
    check(at345 && s == Step::Foot && quiet && held1 && firstK == 3.45f && aboard == 1 && others == 0 &&
              g.p.k() == 3.45f,
          "foot: 3.45 held, 2000 frames on foot, 680 records within 5 s aboard: k 3.45 in one step, no ramp");
    // On foot again, then aboard where the records stay under 200 for 6 s:
    // k 1, and then the ordinary ramp.
    g.frame(12.9, kTwo, 680, Work::Valid, true);
    for (int i = 1; i < 100; ++i) g.frame(12.9, kTwo, 680, Work::Valid, true);
    const uint64_t boarded = g.now();
    bool none = true;
    while (g.now() < boarded + 6000) none &= g.frame(kP, kP, 150) == Step::None;
    do {
        none &= g.frame(kP, kP, 150) == Step::None;   // to the end of the second in progress
    } while (!g.p.windowClosed());
    const bool stillOne = g.p.k() == 1.0f;
    s = g.window(12.9, kAll);
    check(none && stillOne && s == Step::Up && g.p.k() == 1.25f && g.p.upQuanta() == kCoarseQuanta,
          "foot: aboard with the records under 200 for 6 s: k 1, then the ordinary ramp");
    // A hold that begins at k 1 keeps the pending k: aboard, on foot again
    // within 5 s, aboard at the settlement -- the k from before the first hold.
    Feed h;
    for (int i = 0; i < 3; ++i) h.window(12.9, kAll);   // 1.75
    h.frame(12.9, kTwo, 680, Work::Valid, true);         // on foot: 1.00, 1.75 kept
    h.frame(kP, kP, 100);                                // aboard, no settlement yet
    h.frame(kP, kP, 100, Work::Valid, true);             // on foot again, at k 1
    h.frame(kP, kP, 100, Work::Valid, true);
    const Step b = h.frame(kP, kP, 680);                 // aboard at the settlement
    check(b == Step::Aboard && h.p.k() == 1.75f,
          "foot: a hold that begins at k 1 keeps the k pending from the hold before");
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
    int kicks = 0;
    for (int i = 0; i < 700; ++i) kicks += f.frame(14.0, kTwo, 250) == Step::Kick;   // 15 s, every cycle two slots
    check(kicks == 0 && f.p.k() == 4.0f && f.p.ceilingMissing(),
          "reduced: never kicks; at k_max with five triggering seconds the lever is spent");
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

// The application-render GPU instrument's newest sample as the boundary reads
// it (gpu_frame_timing.h), 5 ms old at `now`; ms < 0 switches it off.
void setGpu(uint64_t now, double ms) {
    g_gpu = GpuFrameSnapshot{};
    if (ms < 0.0) return;
    g_gpu.enabled = g_gpu.haveResult = true;
    g_gpu.result.source = GpuSpanSource::ApplicationRender;
    g_gpu.result.reason = GpuSpanReason::Valid;
    g_gpu.result.ageMs = 5;
    g_gpu.result.outerMs = ms;
    g_gpu.capturedAtMs = now;
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
    check(logged("settlement detail: auto's policy, decided once a second on the cycles completed in it with a fresh "
                 "timing sample", at) &&
              logged("to k_max 2.00 at once after 10 such seconds in a row below it (a kick, at most one per 30 s)", at) &&
              logged("settlement detail: auto's trials: a kick is judged on its fifth second at k_max", at),
          "boundary: auto's policy and its trials follow on lines of their own");
    at = g_lines.size();
    // 30.8 s, 250 records a frame; caller work 11.6 ms, 0.49 over budget, app
    // work 5.6 under; one cycle in seven two slots whatever k (14%, under a
    // quarter: the fine steps): 0.05 a second to 1.45, the kick at the tenth
    // second, its trial failed on the fifth second at k_max -- 1.45 again, no
    // kick for 60 s -- and 0.05 a second to k_max 2.00 at the 26th.
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        for (uint32_t p = 0; p < 20; ++p) g_obsPart(f.items(), f.outAt(), f.view[p & 1], true);
        setTiming(++seq, t, 11.6);
        boundaryCycle(t, i % 7 == 0 ? 2.0 * kPeriodMs : kPeriodMs);
        t += 11;
    }
    check(countLogged("settlement detail: frame work = caller work per cycle (runtime timing v5: the caller thread "
                      "from one pose wait's return to the next one's entry", at) == 1,
          "boundary: the first version 5 frame names the signal, once");
    check(countLogged("settlement detail (observe only, never writes): k 1.00 -> 1.05, up 0.05: 13 of 91 cycles in the "
                      "last second took two display slots as the CPU's (0 GPU-bound, 0 unexplained), their mean caller "
                      "work 0.49 ms over (under a quarter the CPU's and 1.00 ms over or less: the fine step); 250 "
                      "builder records", at) == 1,
          "boundary: the first step is logged, 0.05, with the second's two-slot cycles, whose, and their mean excess");
    check(countLogged("settlement detail (observe only, never writes): k 1.45 -> 2.00, kick: 10 seconds in a row with a "
                      "tenth or more of the cycles taking two slots as the CPU's (the last 13 of 91), the steps not "
                      "clearing it: from the pre-kick k 1.45 to k_max 2.00 so consecutive frames fit and the runtime "
                      "returns to full rate; a trial, judged on the fifth second at k_max; 250 builder records", at) == 1,
          "boundary: ten triggering seconds below k_max kick, a trial, and the kick line names the pre-kick k");
    check(countLogged("settlement detail (observe only, never writes): k 2.00 -> 1.45, restored k 1.45 after a failed "
                      "kick: the fifth second at k_max still had a tenth or more of its cycles take two display slots "
                      "as the CPU's (13 of 91); no kick for 60 s; 250 builder records", at) == 1,
          "boundary: the kick's fifth second at k_max still triggering: the pre-kick k back at once, said whole");
    check(countLogged("settlement detail (observe only, never writes): k ", at) == 6 &&
              logged("(+4 steps since the last line)", at) && logged("(+3 steps since the last line)", at),
          "boundary: step lines are rate-limited to one per 5 s, never a kick or a restore, and count the steps skipped");
    check(logged("decisions: slots missed 389 of 2728 (the CPU's 389, GPU-bound 0, unexplained 0); kicks 1; restores 1 "
                 "(1 after a failed kick); the next recovery trial after 5 clean seconds; up steps' benefit 0 yes, 0 "
                 "no, ", at) &&
              logged("not judged (the scene changing, or too few samples); inert holds 0 (retries 0, re-armed 0); parts "
                     "a frame: tested 20.0, passed at EDVR's scale 0.0", at),
          "boundary: the decisions line counts the slots and whose, the kick, the restore, the judgements and the holds");
    check(logged("(at k_max now: unknown (no fresh evidence))", at) && !logged("settlement detail: at the ceiling", at),
          "boundary: at k_max the summary gives the outcome (20 parts a frame: not judged); not yet spent");
    check(logged("250 builder records, frame work = caller work per cycle: 11.60 ms vs period 11.11 ms.", at) &&
              !logged("-> LOD scale s x k", at),
          "boundary: a step line names the signal it stepped on; observing, it names no scale to write");
    check(logged("k now 2.00, effective s x k 2.000 (window 1.00..2.00 of max 2.00; 20 up (0 by 0.25), 0 down, 0 "
                 "resets, 0 clamps; held on foot 0 frames)", at),
          "boundary: the summary's k, effective s x k, range, steps and their size, and the frames held on foot");
    check(logged("builder records/frame 250.0 (max 250, >= 200 on 2729 frames, 150-199 in a settlement on 0)", at) &&
              logged("part tests/frame 20.0", at),
          "boundary: the summary's density means, and the frames a settlement spent at 150-199 records");
    check(logged("frame work = caller work per cycle: 11.60 ms mean vs period 11.11 ms", at) &&
              logged("invalid 0, caller work absent 0; no fresh timing on 0 frames, 0 expiries)", at),
          "boundary: the summary's frame work is the caller work (not the 5.60 ms app work beside it), all fresh");
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
    // verdicts at this s; only the header is read here. Every cycle takes two
    // slots now: the fifth triggering second at k_max says the lever is spent
    // (the ring still half the 11.6 ms samples), and again once the next
    // summary window begins.
    put(f.ctx, 0x30, 1.5f);
    at = g_lines.size();
    frames(2800, 250, 20, 12.5);
    check(logged("k now 2.00, effective s x k 3.000 (window 2.00..2.00", at) &&
              logged("LOD scale: game s 1.500 (the builder's read: no setter call yet), held 1.500", at),
          "boundary: the summary prints the effective s x k beside k, from the engine's s");
    check(countLogged("settlement detail: frame work = ", at) == 0,
          "boundary: the signal line is not repeated while the runtime's version holds");
    check(countLogged("settlement detail: at the ceiling (k 2.00, s x k 3.000) and still missing ", at) == 2 &&
              countLogged("settlement detail: at the ceiling (k 2.00, s x k 3.000) and still missing 30 of the last 30 "
                          "display slots (30 the CPU's, 0 GPU-bound, 0 unexplained): outcome unknown (no fresh "
                          "evidence) (against k 1 at this view: tested 20 -> 20, passed 20 -> 20 parts a frame, caller "
                          "work 11.60 -> 12.50 ms); caller work 12.50 ms mean, GPU unknown (no application-render "
                          "sample).", at) == 1,
          "boundary: the lever-spent line names k, s x k, whose misses, the outcome with both ends against k 1 "
          "(20 parts a frame, the recompute disagreeing at s 1.5: not judged), the caller work and the GPU");
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
    // The second in progress still holds 12.5 ms cycles that took two slots:
    // 40 sparse frames within budget close it where nothing can rise (no
    // settlement), before the dense frames.
    frames(40, 0, 0, 10.5);
    frames(2800, 250, 20, 10.5);   // dense, every cycle one slot
    check(logged("k stayed 1: no cycle took two display slots as the CPU's in a frame with 200 records", at) &&
              !logged("only GPU-bound or unexplained ones", at),
          "boundary: every cycle in its slot -> why k stayed 1");
    // One cycle in 15 taking two slots (6-7 a second, under a tenth): the
    // dead band, and the summary says so.
    at = g_lines.size();
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        const double ms = i % 15 == 0 ? 12.2 : 10.9;
        setTiming(++seq, t, ms);
        boundaryAt(t, ms);
        t += 11;
    }
    check(logged("k stayed 1: no second had a tenth of its cycles take two display slots as the CPU's (under a tenth "
                 "is the dead band)", at) &&
              !logged("up 0.", at),
          "boundary: one cycle in 15 taking two slots -> no step, and the summary names the dead band");
    // Every cycle taking two slots at 12.9 ms: the second in progress, a
    // quarter of it already two slots, steps 0.25; 0.25 a second to k_max
    // 2.00 at the fourth -- clear of the ten seconds a kick needs. Then
    // headroom: five clean seconds with the caller work 2.11 ms under -- one
    // step down, a trial against 2.00, and the line says so.
    at = g_lines.size();
    frames(400, 250, 20, 12.9);
    check(g_state.policy.k() == 2.0f && logged("k 1.00 -> 1.25, up 0.25: ", at) && !logged("kick:", at),
          "boundary: every cycle taking two slots at 12.9 ms: 0.25 a second reaches k_max before a kick could");
    frames(700, 250, 20, 9.0);
    check(g_state.policy.k() == 1.95f &&
              countLogged("k 2.00 -> 1.95, down 0.05: 5 clean seconds in a row (no cycle took two display slots), the "
                          "last one's caller work 2.11 ms under the period (more than 1.00 ms to spare); a trial: k 2.00 "
                          "comes back if a second triggers within 10 s; 250 builder records, frame work = caller work "
                          "per cycle: 9.00 ms vs period 11.11 ms", at) == 1,
          "boundary: one step down after five clean seconds, a trial; its line says so, the margin and the k it risks");
    applyConfig("game", 2.0f, true, t);
    // A runtime whose newest frame is invalid: every boundary expires the evidence.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    for (uint32_t i = 0; i < 2800; ++i) {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        setTiming(++seq, t, 12.5);
        g_timing.invalid = true;
        boundaryAt(t, 12.5);
        t += 11;
    }
    check(logged("over 0 samples", at) &&
              logged("invalid 2729, caller work absent 0; no fresh timing on 2729 frames, 0 expiries)", at) &&
              logged("k stayed 1: no valid frame-work sample from the native runtime", at),
          "boundary: invalid runtime frames are counted, never fresh, and hold k at 1");
    applyConfig("game", 2.0f, true, t);
    // THE DEFECT the caller work fixes -- the first shadow flight (2026-09-23
    // 07:25 UTC, parked at the settlement, 45 fps): app work 8.37-8.67 ms,
    // caller work 14.4 ms (cycle 21.99 - next wait 7.57), period 11.11. On the
    // app figure k never left 1; on the caller work, the same frames step up.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    frames(2800, 250, 20, 14.4);   // version 5: caller 14.4 ms, app 8.4 ms beside it
    check(logged("frame work = caller work per cycle: 14.40 ms mean vs period 11.11 ms", at) &&
              logged("settlement detail (observe only, never writes): k 1.00 -> 1.25, up 0.25: 91 of 91 cycles in the "
                     "last second took two display slots as the CPU's (0 GPU-bound, 0 unexplained), their mean caller "
                     "work 3.29 ms over (a quarter or more the CPU's: the coarse step)", at) &&
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
    check(logged("over 0 samples (over by > 0.30 ms: 0, under by > 1.00 ms: 0, invalid 2729, caller work absent 2729; "
                 "no fresh timing on 2729 frames", at) &&
              logged("k stayed 1: no valid frame-work sample from the native runtime", at),
          "boundary: a version 5 frame without caller work is an invalid sample, counted as such");
    applyConfig("game", 2.0f, true, t);
    // An older runtime (timing v4): no caller work crosses, the app work
    // stands in, and every line says so. Without caller work no miss can be
    // put on the CPU: auto holds k at 1, says so once, and every summary
    // carries it.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    frames(2800, 250, 20, 8.4, EDVR_NATIVE_TIMING_VERSION_4);
    check(countLogged("settlement detail: frame work = app work (pre-submit only; host older: runtime timing v4 sends "
                      "no caller work", at) == 1,
          "boundary: a version 4 runtime is named once, as the fallback");
    check(logged("frame work = app work (pre-submit only; host older): 8.40 ms mean vs period 11.11 ms", at) &&
              countLogged("settlement detail: the runtime sends no caller work (timing v4), so misses cannot be "
                          "attributed to the CPU: auto holds k at 1", at) == 1 &&
              logged("unexplained 0) (no caller work: holding); kicks 0", at) &&
              logged("k stayed 1: no caller work from the runtime, so no miss can be attributed to the CPU: holding",
                     at),
          "boundary: the fallback's summary names the app work; auto holds, said once and in the summary");
    at = g_lines.size();
    applyConfig("auto", 1.5f, true, t);
    check(logged("; frame work = app work (pre-submit only; host older: runtime timing v4 sends no caller work", at),
          "boundary: the configure line names the fallback once a runtime frame has said which");
    applyConfig("game", 2.0f, true, t);
    // Version 3: no base rate either -- the session's first predicted period
    // is the budget. App work over it and every cycle taking two slots: still
    // no step, auto holds.
    applyConfig("auto", 2.0f, true, t);
    at = g_lines.size();
    frames(2800, 250, 20, 12.5, EDVR_NATIVE_TIMING_VERSION_3);
    check(countLogged("runtime timing v3 sends no caller work", at) == 1 &&
              logged("frame work = app work (pre-submit only; host older): 12.50 ms mean vs period 11.11 ms", at) &&
              countLogged("settlement detail: the runtime sends no caller work (timing v3)", at) == 1 &&
              logged("slots missed 2728 of 2728 (the CPU's 2728, GPU-bound 0, unexplained 0) (no caller work: holding)",
                     at) &&
              !logged("up 0.", at) && g_state.policy.k() == 1.0f,
          "boundary: a version 3 runtime falls back to the app work against the first predicted period, and holds");
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
              logged("settlement detail: auto's policy, decided once a second", at),
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
    frames(400, 12.9);   // 4.4 s at 12.9 ms, every cycle two slots: 0.25 a second to k_max 2.0 at the fourth
    check(g_acting.load() && g_state.policy.k() == 2.0f && f.scale() == 3.0f && !logged("kick:", at),
          "foot: in the cockpit the ramp reaches k_max 2.0, and 1.5 x 2 is in force");
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
    const size_t atAboard = g_lines.size();
    frames(1, 12.9);
    check(countLogged("settlement detail (acting): no longer on foot (Status.json) after 11.0 s, 1000 frames held at "
                      "k 1; the governor resumes", at) == 1 &&
              countLogged("settlement detail (acting): k 1.00 -> 2.00, back aboard: k restored to 2.00 (held on foot "
                          "1000 frames)", atAboard) == 1 &&
              g_state.policy.k() == 2.0f,
          "foot: aboard again at the settlement, one line saying how long, and k 2.00 back in one step");
    frames(1400, 12.9);   // to the 30 s summary
    check(logged("1 restored aboard; held on foot 1000 frames); ", at) && g_state.policy.k() == 2.0f &&
              f.scale() == 3.0f && !logged("k 1.00 -> 1.25", atAboard),
          "foot: the summary counts the frames held on foot and the return aboard; no ramp from 1");
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
    frames(2800, 250, 20, 12.5);   // 1.39 ms over, every cycle two slots: 0.25 a second to k_max, no kick
    check(countLogged("settlement detail: LOD scale scaled: game s 1.500 -> 1.875 (k 1.25), ctx 0x", at) == 1,
          "acting: the first write to the builder's context is logged once, with the game's s and k");
    check(f.scale() == 3.0f && other.scale() == 1.5f,
          "acting: at k 2 the builder's context holds 3.0 after the rebuild; the other context keeps the game's 1.5");
    check(logged("settlement detail (acting): k 1.00 -> 1.25, up 0.25: 91 of 91 cycles in the last second took two "
                 "display slots as the CPU's (0 GPU-bound, 0 unexplained), their mean caller work 1.39 ms over (a "
                 "quarter or more the CPU's: the coarse step); 250 builder records, frame work = caller work per "
                 "cycle: 12.50 ms vs period 11.11 ms -> LOD scale s x k 1.875.", at),
          "acting: a step line names its size and why, the two-slot cycles and whose, and the scale it will write");
    check(!logged("kick:", at), "acting: four steps of 0.25 reach k_max 2.00 in four seconds: no kick");
    // (Once a summary window: these 2800 frames end the first and begin the
    // second, where it still holds.)
    check(countLogged("settlement detail: at the ceiling (k 2.00, s x k 3.000) and still missing 30 of the last 30 "
                      "display slots (30 the CPU's, 0 GPU-bound, 0 unexplained): outcome unknown (no fresh evidence) "
                      "(against k 1 at this view: tested 20 -> 20, passed 20 -> 0 parts a frame, caller work 12.50 -> "
                      "12.50 ms); caller work 12.50 ms mean, GPU unknown (no application-render sample).", at) == 2,
          "acting: at k_max with five triggering seconds in a row, the lever-spent line with both ends, once a "
          "summary window");
    check(logged("(window 1.00..2.00 of max 2.00; 4 up (4 by 0.25), 0 down, 0 resets, 0 clamps; held on foot 0 frames)",
                 at) &&
              logged("; kicks 0; restores 0 (0 after a failed kick);", at),
          "acting: the summary counts four steps of 0.25, no kick and no restore");
    // The first window: 2729 frames of 11 ms, two rebuilds a frame, the first
    // 92 at k 1 (the first second decides at its 91st cycle).
    check(logged("settlement detail (acting): 30.0 s, 2729 frames: k now 2.00, effective s x k 3.000", at) &&
              logged("LOD scale: game s 1.500, held 3.000 (k 2.00); setter calls 5458 (scaled 2637) on 1 pointers "
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

// The signals beside the caller work, end to end, observing: the
// application's GPU render time and the caller work class every two-slot
// cycle (GPU-bound, unexplained, the CPU's; the lever-spent line gives the GPU
// beside the caller work), the lever's benefit (two up steps that move
// neither the parts passed at EDVR's scale nor the caller work: inert; a
// retried step past the plateau re-arms), and the density a settlement spends
// at 150-199 records.
void caseSignalsBoundary() {
    Fake f;
    freshTable();
    g_ctx.store(f.ctxAt());
    uint64_t& t = g_t;
    uint64_t& seq = g_seq;
    // Every cycle takes two slots; the GPU sample is gpuMs (< 0: none), or 9.0
    // ms -- well under the period, so that miss is the CPU's -- on every
    // oursEvery-th frame.
    auto frames = [&](uint32_t n, uint32_t parts, double ms, double gpuMs, uint32_t oursEvery = 0,
                      uint32_t records = 250) {
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t r = 0; r < records; ++r)
                if (g_obsBuilder) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
            for (uint32_t p = 0; p < parts; ++p)
                if (g_obsPart) g_obsPart(f.items(), f.outAt(), f.view[p & 1], true);
            setTiming(++seq, t, ms);
            setGpu(t, oursEvery && i % oursEvery == 0 ? 9.0 : gpuMs);
            boundaryCycle(t, 2.0 * kPeriodMs);
            t += 11;
        }
    };
    // The GPU at 11.5 ms (the period - 0.5 or more) and the caller work 12.9
    // (it spans Present, where a GPU-bound game blocks): never a step; the
    // summary counts every miss GPU-bound and says why k stayed 1.
    size_t at = g_lines.size();
    applyConfig("auto", 2.0f, true, t);
    frames(2800, 20, 12.9, 11.5);
    check(g_state.policy.k() == 1.0f && !logged("up 0.", at) &&
              logged("slots missed 2728 of 2728 (the CPU's 0, GPU-bound 2728, unexplained 0); kicks 0;", at) &&
              logged("k stayed 1: no cycle took two display slots as the CPU's in a frame with 200 records (only "
                     "GPU-bound or unexplained ones)", at),
          "signals: two-slot cycles with the GPU at 11.5 ms never step; the summary counts them GPU-bound");
    applyConfig("game", 2.0f, true, t);
    // The GPU at 6 ms and the caller work 10.5 (under the period - 0.3):
    // neither's -- unexplained, counted, never a step.
    at = g_lines.size();
    applyConfig("auto", 2.0f, true, t);
    frames(2800, 20, 10.5, 6.0);
    check(g_state.policy.k() == 1.0f && !logged("up 0.", at) &&
              logged("slots missed 2728 of 2728 (the CPU's 0, GPU-bound 0, unexplained 2728); kicks 0;", at),
          "signals: two-slot cycles with the GPU and the caller work both under: unexplained, never a step");
    applyConfig("game", 2.0f, true, t);
    // At the ceiling (k_max 1.05: one fine step) with four cycles in five
    // GPU-bound at 12.5 ms and the fifth the CPU's (9.0): each second
    // triggers on the fifth, and after five at k_max the lever-spent line
    // gives the classes and both figures -- the GPU's mean over the caller
    // work's: the GPU is the wall.
    at = g_lines.size();
    applyConfig("auto", 1.05f, true, t);
    frames(1000, 20, 11.5, 12.5, 5);
    check(countLogged("settlement detail: at the ceiling (k 1.05, s x k 1.050) and still missing 30 of the last 30 "
                      "display slots (6 the CPU's, 24 GPU-bound, 0 unexplained): outcome unknown (no fresh evidence) "
                      "(against k 1 at this view: tested 20 -> 20, passed 20 -> 20 parts a frame, caller work 11.50 -> "
                      "11.50 ms); caller work 11.50 ms mean, GPU 11.80 ms: the GPU is the wall.", at) == 1 &&
              !logged("settlement detail: the LOD lever is inert", at),
          "signals: the lever-spent line gives whose misses, the GPU beside the caller work, and when the GPU is the wall");
    applyConfig("game", 2.0f, true, t);
    // The lever inert at this view: 500 part tests a frame (250 an eye), each
    // passed, none dropped until s x k passes 1.52 here (f = 0.099 s k
    // against t0 0.15), the caller work unchanged: the two 0.25 steps to 1.25
    // and 1.50 move nothing -- inert, said once with the figures; no step and
    // no kick while it holds. 30 s on one step is retried: 1.75 is past the
    // plateau (every part would drop), a benefit, and the lever re-arms.
    at = g_lines.size();
    applyConfig("auto", 2.0f, true, t);
    frames(2800, 500, 12.9, -1.0);
    check(countLogged("settlement detail: the LOD lever is inert at this view: no observed benefit: tested 500 -> 500, "
                      "passed 500 -> 500 parts a frame, caller work 12.90 -> 12.90 ms across two 0.25 steps; holding k "
                      "1.50 (s x k 1.500), no step and no kick; one step is retried every 30 s or when the parts tested "
                      "a frame move 20%.", at) == 1 &&
              g_state.policy.k() == 1.5f && !logged("kick:", at) && !logged("settlement detail: at the ceiling", at) &&
              logged("; kicks 0; restores 0 (0 after a failed kick); the next recovery trial after 5 clean seconds; "
                     "up steps' benefit 0 yes, 2 no, 0 not judged (the scene changing, or too few samples); inert holds "
                     "1 (retries 0, re-armed 0); parts a frame: tested 500.0, passed at EDVR's scale 500.0, dropped "
                     "0.0", at),
          "signals: two up steps that move nothing: the lever is inert, said once; no step and no kick while it holds");
    const size_t atRetry = g_lines.size();
    frames(300, 500, 12.9, -1.0);
    check(countLogged("settlement detail: the LOD lever responds again at this view: the retried step moved tested 500 "
                      "-> 500, passed 500 -> 0 parts a frame, caller work 12.90 -> 12.90 ms; stepping resumes at k "
                      "1.75.", atRetry) == 1 &&
              logged("; a retry while the lever is inert here;", atRetry) && g_state.policy.k() == 2.0f,
          "signals: 30 s into the hold a retried step past the plateau shows a benefit: re-armed, stepping resumes");
    // At k_max 2.00 with every cycle still two slots: the lever-spent line's
    // outcome against the k 1 baseline of this view -- every part would drop
    // now (passed 500 -> 0): residual benefit, both ends given.
    const size_t atCeiling = g_lines.size();
    frames(600, 500, 12.9, -1.0);
    check(countLogged("settlement detail: at the ceiling (k 2.00, s x k 2.000) and still missing 30 of the last 30 "
                      "display slots (30 the CPU's, 0 GPU-bound, 0 unexplained): outcome residual benefit (against k 1 "
                      "at this view: tested 500 -> 500, passed 500 -> 0 parts a frame, caller work 12.90 -> 12.90 ms); "
                      "caller work 12.90 ms mean, GPU unknown (no application-render sample).", atCeiling) == 1,
          "signals: the ceiling's outcome against the k 1 baseline of this view, tested and passed at both ends");
    applyConfig("game", 2.0f, true, t);
    // A settlement's frames at 150-199 records hold k and are counted: the
    // density the lever itself may cut (the review's conditional risk).
    at = g_lines.size();
    applyConfig("auto", 2.0f, true, t);
    frames(100, 0, 10.5, 6.0, 0, 250);
    frames(2700, 0, 10.5, 6.0, 0, 180);
    check(logged("150-199 in a settlement on 2629)", at) && g_state.policy.inSettlement(),
          "signals: the summary counts a settlement's frames at 150-199 records");
    applyConfig("game", 2.0f, true, t);
    setGpu(t, -1.0);
    freshTable();
}

// The review of 2026-09-23 (reviews/lod-governor-review-2026-09-23.md,
// finding 1, the table at lines 235-240): a governor primed on overloaded
// fresh samples, then 6.6 s of frames without new timing. At 6964c31 the
// frozen sequence and the inactive source climbed k 1.25 -> 2.75 on the same
// 30 samples. Every row must leave k where the priming put it: a decision
// counts only cycles with a fresh sample, and the evidence expires.
void caseFreshEvidence() {
    Fake f;
    freshTable();
    g_ctx.store(f.ctxAt());
    uint64_t& t = g_t;
    uint64_t& seq = g_seq;
    auto frame = [&]() {
        for (uint32_t r = 0; r < 250; ++r) g_obsBuilder(0, f.ctxAt(), 0, f.nibbles());
        boundaryCycle(t, 2.0 * kPeriodMs);   // overloaded: every cycle two slots
        t += 11;
    };
    // The rows: what the timing does after the priming, where the review has
    // it, and the readWork branch that meets it now.
    struct Row {
        const char* what;
        const char* review;
        const char* code;
    };
    const Row rows[4] = {
        {"frozen sequence", "reviews/lod-governor-review-2026-09-23.md:237",
         "src/d3d11/lod_governor.cpp:1146 (readWork, the unchanged sequence: expired after 2 s)"},
        {"inactive timing source", "reviews/lod-governor-review-2026-09-23.md:238",
         "src/d3d11/lod_governor.cpp:1137 (readWork, no lease: expired at once)"},
        {"new sequence already 3 s old", "reviews/lod-governor-review-2026-09-23.md:239",
         "src/d3d11/lod_governor.cpp:1158 (readWork, not fresh: invalid, expired)"},
        {"explicit invalid source", "reviews/lod-governor-review-2026-09-23.md:240",
         "src/d3d11/lod_governor.cpp:1141 (readWork, an invalid frame: expired)"},
    };
    for (int r = 0; r < 4; ++r) {
        // The priming: a second of fresh overloaded samples -- the first
        // decision, k 1.00 -> 1.25.
        applyConfig("game", 6.0f, true, t);
        applyConfig("auto", 6.0f, true, t);
        for (int i = 0; i < 92; ++i) {
            setTiming(++seq, t, 14.0);
            frame();
        }
        const float before = g_state.policy.k();
        int n = 600;
        if (r == 1) g_timing.active = false;
        if (r == 2) {
            ++g_timing.sequence;
            g_timing.capturedAtMs = t - 3000;
            ++n;
        }
        if (r == 3) g_timing.invalid = true;
        for (int i = 0; i < n; ++i) frame();
        const float after = g_state.policy.k();
        const Window& w = g_state.w;
        std::printf("lod_governor_test: review row %d, %s (%s; now %s): k %.2f -> %.2f; valid %u, invalid %u, no "
                    "fresh timing on %u frames, %u expiries\n", r + 1, rows[r].what, rows[r].review, rows[r].code,
                    double(before), double(after), w.workSamples, w.workInvalid, w.staleFrames, w.expiries);
        const bool rowOk = r == 0   ? w.expiries == 1 && w.staleFrames > 400
                         : r == 1 ? w.expiries == 1 && w.staleFrames == 600
                         : r == 2 ? w.expiries == 1 && w.workInvalid == 1
                                  : w.expiries == 1 && w.workInvalid == 600;
        char what[160];
        std::snprintf(what, sizeof(what), "fresh: review row %d (%s): k stays 1.25 through 6.6 s without new timing, "
                      "the evidence expired once", r + 1, rows[r].what);
        check(before == 1.25f && after == before && rowOk, what);
        g_timing = NativeTimingSnapshot{};
    }
    applyConfig("game", 6.0f, true, t);
    freshTable();
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
    run("signals boundary", caseSignalsBoundary);
    run("fresh evidence", caseFreshEvidence);
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
