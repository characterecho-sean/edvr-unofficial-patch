// explorer_cam_fade_test: the entry fade's wait for the engine's motion (src/d3d11/explorer_cam_fade_core.h, "MOTION"), pure.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention; with no repository root, M11.p (which reads a source) is skipped
//   --self-test  every check; the optional argument is the repository root, where M11.p reads src/d3d11/explorer_cam.cpp (the mutation tool hands it a temp root
//                holding an edited copy)
//
// The comfort fade's own timeline (enter, exit, re-attach, the 3 s cap, the aborts) is held by tools\explorer_cam_test; this rig holds what the F12 flight's
// first F5 entry added to it: once the entry is otherwise ready (placed, locked, steady, the UI settled) the view stays black until the engine's motion is
// live for the eye path -- its views handed to the compose for three consecutive frames and, when the frame has skinned jobs, the second skin's join live for
// them -- for at most one second beyond the moment it was otherwise ready. Cases ("M<case>.<what>"; tools\explorer_cam_fade_test\mutants.py names the case
// that must catch each mutation):
//   M1  ready early: the signals are already live when the entry is otherwise ready: it fades in at once, no hold
//   M2  ready late: it holds black, needs three consecutive frames of views (two are not enough), and fades in the frame the signals are live
//   M3  never ready: the hold ends one second after the entry was otherwise ready, says which condition was missing, and fades in anyway
//   M4  no skinned jobs: a scene with no characters does not wait for the join; with jobs and a join that is not live it does
//   M5  not armed: the engine's motion is not running: nothing is waited for
//   M6  exits are unchanged: an exit never waits
//   M7  a re-attach waits like an entry
//   M8  the hold starts over when the entry stops being otherwise ready
//   M9  the hold is one second beyond the point the view would have faded in, even past the 3 s cap from the press; and the 3 s cap itself is unchanged
//   M10 the lines
//   M11 the clock the timeline is fed: ecm::qpcTicksToUs is exact and never steps back across the tick where ticks * 1000000 wraps a uint64 (about 21 days of
//       counter); run through it across that tick, the placement cap and the motion hold still fire once and on time; and explorer_cam.cpp's two conversions
//       (the frame boundary's input and realNowUs) both call it (read as text)
//   M12 the timeline does not depend on a monotonic clock: a reading below the one before is no time passing and the caps run on from there, also for the
//       entry after the resets that clear one
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "explorer_cam_fade_core.h"

using namespace edvr;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
        std::fflush(stdout);
    }
}
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

// The first QPC tick at which ticks * 1000000 wraps a uint64 (18,446,744,073,710; about 21.35 days of counter at 10 MHz, whatever the frequency).
constexpr uint64_t kWrapTick = UINT64_MAX / 1000000ull + 1;
// explorer_cam.cpp's conversion before the fix: the product wraps before the division.
uint64_t multiplyFirst(uint64_t ticks, uint64_t freq) { return ticks * 1000000ull / freq; }

struct Rig {
    ecm::ComfortTimeline tl;
    ecm::ComfortInputs in;
    uint64_t us = 5000000;
    std::vector<ecm::ComfortLine> events;
    uint32_t releasedEnter = 0, releasedExit = 0;
    float alpha = 0.0f;
    // An optional tick clock (startClock): tick() then feeds the timeline `clock(ticks, clockFreq)` for a counter that started wrapAfterMs before the tick
    // where ticks * 1000000 wraps, as explorer_cam.cpp feeds it QueryPerformanceCounter through the conversion.
    uint64_t (*clock)(uint64_t, uint64_t) = nullptr;
    uint64_t clockFreq = 0, startTicks = 0, elapsedMs = 0, lastNow = 0;
    bool stepped = false;   // the clock handed the timeline a reading below the one before it
    Rig() {
        in.active = true;
        in.ctlCalls = 100;
    }
    void startClock(uint64_t (*fn)(uint64_t, uint64_t), uint64_t freq, uint32_t wrapAfterMs) {
        clock = fn;
        clockFreq = freq;
        startTicks = kWrapTick - uint64_t(wrapAfterMs) * freq / 1000;
        elapsedMs = 0;
        us = lastNow = fn(startTicks, freq);
    }
    ecm::ComfortStep tick(uint32_t ms = 10) {
        if (clock) {
            elapsedMs += ms;
            us = clock(startTicks + elapsedMs * clockFreq / 1000, clockFreq);
        } else {
            us += uint64_t(ms) * 1000;
        }
        if (us < lastNow) stepped = true;
        lastNow = us;
        in.nowUs = us;
        const ecm::ComfortStep s = tl.step(in);
        in.pressEnter = in.pressExit = false;
        for (uint8_t i = 0; i < s.nev; ++i) events.push_back(s.ev[i]);
        releasedEnter += s.releaseEnter ? 1u : 0u;
        releasedExit += s.releaseExit ? 1u : 0u;
        alpha = s.alpha;
        if (in.mode == 0 && in.sessionActive) ++in.ctlCalls;
        return s;
    }
    void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) tick(10); }
    size_t count(ecm::ComfortEv ev) const {
        size_t n = 0;
        for (const auto& e : events) n += e.ev == ev ? 1u : 0u;
        return n;
    }
    const ecm::ComfortLine* find(ecm::ComfortEv ev) const {
        for (const auto& e : events) if (e.ev == ev) return &e;
        return nullptr;
    }
    // F5 until the view is black and the request is out (the session not yet on).
    void pressAndGoBlack() {
        in.pressEnter = true;
        tick(10);
        for (uint32_t ms = 0; !releasedEnter && ms < 1000; ms += 10) tick(10);
    }
    // The controller took the request, the session is on, the placement came: everything the old fade-in waited for.
    void goodEntry() {
        in.sessionActive = true;
        in.requestPending = false;
        in.mode = 4;
        in.placed = true;
        in.state = ecm::kStateRelativeLock;
        in.steady = ecm::kFadeSteadyUpdates;
        in.uiSettled = true;
    }
    void motion(bool armed, uint32_t views, bool jobs, bool live) {
        in.motionArmed = armed;
        in.viewsRun = views;
        in.skinJobs = jobs;
        in.skinLive = live;
    }
    // Ticks until a fade in begins (the phase leaves Black), at most `maxMs`; returns the ms it took.
    uint32_t untilFadeIn(uint32_t maxMs) {
        uint32_t ms = 0;
        while (tl.phase() == ecm::ComfortPhase::Black && ms < maxMs) { tick(10); ms += 10; }
        return ms;
    }
};

void caseConstants() {
    static_assert(ecm::kFadeViewsLiveFrames == 3 && ecm::kFadeMotionHoldMs == 1000, "the signals: views live for 3 frames in a row, the hold at most 1.0 s");
    check(ecm::kFadeViewsLiveFrames == 3 && ecm::kFadeMotionHoldMs == 1000 && ecm::kFadeMaxBlackMs == 3000, "M1.z the constants: 3 frames of views, a hold of 1.0 s beyond today's fade-in point, the 3 s cap from the press unchanged");
}

// ---- M1 ---------------------------------------------------------------------------------------------------------------------------
void caseEarly() {
    Rig r;
    r.motion(true, 3, false, false);
    r.pressAndGoBlack();
    r.goodEntry();
    r.tick();
    check(r.tl.phase() == ecm::ComfortPhase::In && r.count(ecm::ComfortEv::FadeIn) == 1 && r.count(ecm::ComfortEv::MotionTimedOut) == 0,
          "M1.a signals live when the entry is otherwise ready (views 3, no skinned jobs): it fades in on that frame, as before");
    check(r.find(ecm::ComfortEv::FadeIn) && r.find(ecm::ComfortEv::FadeIn)->motionMs == 0, "M1.b ...with no hold (0 ms)");
    {
        char buf[ecm::kLineBytes];
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::FadeIn));
        check(has(buf, "[engine motion: views live for 3 frames in a row, no skinned jobs]") && !has(buf, "held "),
              "M1.d ...and the line reports the engine's motion as it stood (views live for 3 frames, no skinned jobs), so a flight can tell this from a signal that never ran");
    }
    Rig s;
    s.motion(true, 3, true, true);
    s.pressAndGoBlack();
    s.goodEntry();
    s.tick();
    check(s.tl.phase() == ecm::ComfortPhase::In && s.count(ecm::ComfortEv::FadeIn) == 1, "M1.c ...also with skinned jobs whose join is live");
    {
        char buf[ecm::kLineBytes];
        ecm::formatComfort(buf, sizeof(buf), *s.find(ecm::ComfortEv::FadeIn));
        check(has(buf, "skinned jobs, the second skin's join live]"), "M1.e ...and says the join was live for the skinned jobs");
    }
}

// ---- M2 ---------------------------------------------------------------------------------------------------------------------------
void caseLate() {
    Rig r;
    r.motion(true, 0, false, false);
    r.pressAndGoBlack();
    r.goodEntry();
    r.run(400);
    check(r.tl.phase() == ecm::ComfortPhase::Black && r.alpha == 1.0f && r.count(ecm::ComfortEv::FadeIn) == 0, "M2.a with the views not live the view stays black after the entry is otherwise ready (400 ms)");
    r.motion(true, 2, false, false);
    r.run(100);
    check(r.tl.phase() == ecm::ComfortPhase::Black && r.count(ecm::ComfortEv::FadeIn) == 0, "M2.b two frames of views in a row are not enough: three are needed");
    r.motion(true, 3, false, false);
    r.tick();
    check(r.tl.phase() == ecm::ComfortPhase::In && r.count(ecm::ComfortEv::FadeIn) == 1 && r.count(ecm::ComfortEv::MotionTimedOut) == 0, "M2.c the frame the third arrives it fades in, as a normal fade in (no timeout)");
    const ecm::ComfortLine* l = r.find(ecm::ComfortEv::FadeIn);
    check(l && l->motionMs >= 480 && l->motionMs <= 540, "M2.d ...and says how long the view was held for the motion (about 500 ms)");
    char buf[ecm::kLineBytes];
    if (l) ecm::formatComfort(buf, sizeof(buf), *l); else buf[0] = 0;
    check(has(buf, "entering: placed, locked, the camera UI hidden and the eye steady for 10 updates") && has(buf, "held 5") && has(buf, "ms of that for the engine's motion"),
          "M2.e the FadeIn line names what was met and the hold");
    Rig s;
    s.motion(true, 5, false, false);
    s.pressAndGoBlack();
    s.goodEntry();
    s.in.viewsRun = 1;
    s.run(300);
    check(s.tl.phase() == ecm::ComfortPhase::Black, "M2.f a run of 1 frame is not enough, however many frames the eyes were given before it: the engine reports the run, the core checks it");
}

// ---- M3 ---------------------------------------------------------------------------------------------------------------------------
void caseNever() {
    Rig r;
    r.motion(true, 1, true, false);
    r.pressAndGoBlack();
    r.goodEntry();
    uint32_t ms = r.untilFadeIn(2000);
    check(r.count(ecm::ComfortEv::MotionTimedOut) == 1 && r.count(ecm::ComfortEv::FadeIn) == 0 && r.count(ecm::ComfortEv::TimedOut) == 0,
          "M3.a signals that never come: one MotionTimedOut, no FadeIn, and not the old 3 s TimedOut");
    check(ms >= 990 && ms <= 1020, "M3.b ...after one second beyond the point the view was otherwise ready (not before, not much later)");
    const ecm::ComfortLine* l = r.find(ecm::ComfortEv::MotionTimedOut);
    check(l && (l->unmet & ecm::kComfortUnmetViews) && (l->unmet & ecm::kComfortUnmetSkin) && !(l->unmet & (ecm::kComfortUnmetPlaced | ecm::kComfortUnmetLock | ecm::kComfortUnmetSteady | ecm::kComfortUnmetUi)),
          "M3.c ...naming the conditions missing: the views, and the second skin's join; none of the placement's");
    char buf[ecm::kLineBytes];
    if (l) ecm::formatComfort(buf, sizeof(buf), *l); else buf[0] = 0;
    check(has(buf, "entering:") && has(buf, "the engine's motion was not live after the 1000 ms hold") && has(buf, "fades in anyway") && has(buf, "the engine's motion views were live for 1 of 3 frames in a row") &&
              has(buf, "the second skin's join is not live for the frame's skinned jobs"),
          "M3.d the line says it fades in anyway, after the 1000 ms hold, and which conditions were missing");
    check(r.tl.phase() == ecm::ComfortPhase::In, "M3.e ...and the fade in begins");
    Rig s;
    s.motion(true, 1, false, false);
    s.pressAndGoBlack();
    s.goodEntry();
    s.untilFadeIn(2000);
    const ecm::ComfortLine* v = s.find(ecm::ComfortEv::MotionTimedOut);
    check(v && (v->unmet & ecm::kComfortUnmetViews) && !(v->unmet & ecm::kComfortUnmetSkin), "M3.f with no skinned jobs only the views are named missing");
}

// ---- M4 ---------------------------------------------------------------------------------------------------------------------------
void caseNoJobs() {
    Rig r;
    r.motion(true, 5, false, false);
    r.pressAndGoBlack();
    r.goodEntry();
    r.tick();
    check(r.tl.phase() == ecm::ComfortPhase::In && r.count(ecm::ComfortEv::FadeIn) == 1, "M4.a a scene with no characters (no skinned jobs, the join not live because there is nothing to join) does not wait for the join");
    Rig s;
    s.motion(true, 5, true, false);
    s.pressAndGoBlack();
    s.goodEntry();
    s.run(300);
    check(s.tl.phase() == ecm::ComfortPhase::Black, "M4.b a frame with skinned jobs and a join that is not live waits (views alone are not enough)");
    s.motion(true, 5, true, true);
    s.tick();
    check(s.tl.phase() == ecm::ComfortPhase::In && s.count(ecm::ComfortEv::FadeIn) == 1 && s.count(ecm::ComfortEv::MotionTimedOut) == 0, "M4.c ...and fades in the frame the join is live");
    Rig t;
    t.motion(true, 0, false, true);
    t.pressAndGoBlack();
    t.goodEntry();
    t.run(200);
    check(t.tl.phase() == ecm::ComfortPhase::Black, "M4.d a live join does not stand in for the views");
}

// ---- M5 ---------------------------------------------------------------------------------------------------------------------------
void caseUnarmed() {
    Rig r;
    r.motion(false, 0, true, false);
    r.pressAndGoBlack();
    r.goodEntry();
    r.tick();
    check(r.tl.phase() == ecm::ComfortPhase::In && r.count(ecm::ComfortEv::FadeIn) == 1 && r.count(ecm::ComfortEv::MotionTimedOut) == 0,
          "M5.a with the engine's motion not running nothing is waited for, whatever the other signals say");
    char buf[ecm::kLineBytes];
    ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::FadeIn));
    check(has(buf, "[engine motion: not running, so nothing was waited for]") && !has(buf, "views live for"), "M5.b ...and the line says so (an engine signal that never ran is told apart from one that was ready)");
}

// ---- M6 ---------------------------------------------------------------------------------------------------------------------------
void caseExit() {
    Rig r;
    r.motion(true, 5, false, false);
    r.pressAndGoBlack();
    r.goodEntry();
    r.run(1000);   // the entry is through
    check(r.tl.phase() == ecm::ComfortPhase::Clear && r.count(ecm::ComfortEv::Cleared) == 1, "M6.pre the entry completed");
    r.motion(true, 0, true, false);
    r.in.pressExit = true;
    r.tick();
    for (uint32_t ms = 0; !r.releasedExit && ms < 1000; ms += 10) r.tick();
    r.in.mode = 0;
    uint32_t ms = 0;
    while (r.tl.phase() == ecm::ComfortPhase::Black && ms < 3000) { r.tick(); ms += 10; }
    const size_t fadeIns = r.count(ecm::ComfortEv::FadeIn);
    check(fadeIns == 2 && r.count(ecm::ComfortEv::MotionTimedOut) == 0 && ms <= 300, "M6.a an exit fades in as soon as the camera reads closed and 10 updates have passed, whatever the motion signals say");
    const ecm::ComfortLine* l = nullptr;
    for (const auto& e : r.events) if (e.ev == ecm::ComfortEv::FadeIn && e.kind == ecm::ComfortKind::Exit) l = &e;
    check(l && l->motionMs == 0 && l->unmet == 0, "M6.b ...with no hold in it");
}

// ---- M7 ---------------------------------------------------------------------------------------------------------------------------
void caseReattach() {
    Rig r;
    r.motion(true, 5, false, false);
    r.pressAndGoBlack();
    r.goodEntry();
    r.run(1000);
    check(r.tl.phase() == ecm::ComfortPhase::Clear, "M7.pre the entry completed");
    r.in.placed = false;   // the placement released with the session still on
    r.tick();
    check(r.tl.kind() == ecm::ComfortKind::Reattach, "M7.a the detach starts a re-attach");
    r.motion(true, 0, false, false);
    r.run(300);   // black by now (100 ms out)
    r.goodEntry();
    r.run(300);
    check(r.tl.phase() == ecm::ComfortPhase::Black, "M7.b a re-attach that is otherwise ready waits for the engine's motion like an entry");
    uint32_t ms = r.untilFadeIn(2000);
    const ecm::ComfortLine* l = r.find(ecm::ComfortEv::MotionTimedOut);
    check(l && l->kind == ecm::ComfortKind::Reattach && ms >= 690 && ms <= 740, "M7.c ...and gives up after the same one second from the moment it was otherwise ready, naming the re-attach");
}

// ---- M8 ---------------------------------------------------------------------------------------------------------------------------
void caseRestart() {
    Rig r;
    r.motion(true, 0, false, false);
    r.pressAndGoBlack();
    r.goodEntry();
    r.run(600);
    r.in.placed = false;       // the view stops being otherwise ready
    r.run(100);
    check(r.tl.phase() == ecm::ComfortPhase::Black, "M8.a the entry stops being otherwise ready: still black");
    r.in.placed = true;
    uint32_t ms = r.untilFadeIn(2000);
    const ecm::ComfortLine* l = r.find(ecm::ComfortEv::MotionTimedOut);
    check(l && ms >= 990 && ms <= 1020, "M8.b the hold starts over when it is otherwise ready again: one second from THEN, not from the first time");
}

// ---- M9 ---------------------------------------------------------------------------------------------------------------------------
void caseCap() {
    // otherwise ready at 2.9 s from the press, the motion never live: faded in 1.0 s later (3.9 s), past the 3 s cap from the press
    Rig r;
    r.motion(true, 0, false, false);
    r.pressAndGoBlack();
    r.in.sessionActive = true;
    r.in.mode = 1;
    r.run(2600);   // nothing placed yet
    r.goodEntry();
    uint32_t ms = r.untilFadeIn(3000);
    const ecm::ComfortLine* l = r.find(ecm::ComfortEv::MotionTimedOut);
    check(l && l->heldMs >= 3700 && ms >= 990 && ms <= 1020 && r.count(ecm::ComfortEv::TimedOut) == 0,
          "M9.a otherwise ready at about 2.9 s and the motion never live: the hold runs one second past that, beyond the 3 s cap from the press, then fades in");
    // never otherwise ready, the motion live: the old 3 s cap, unchanged
    Rig s;
    s.motion(true, 5, false, false);
    s.pressAndGoBlack();
    s.in.sessionActive = true;
    s.in.mode = 1;
    uint32_t held = 0;
    while (s.tl.phase() == ecm::ComfortPhase::Black && held < 5000) { s.tick(); held += 10; }
    const ecm::ComfortLine* t = s.find(ecm::ComfortEv::TimedOut);
    check(t && s.count(ecm::ComfortEv::MotionTimedOut) == 0 && t->heldMs >= 2990 && t->heldMs <= 3030, "M9.b an entry that is never otherwise ready still times out at 3 s from the press, whatever the motion signals say");
    check(t && !(t->unmet & (ecm::kComfortUnmetViews | ecm::kComfortUnmetSkin)), "M9.c ...and names only its own unmet conditions");
}

// ---- M10 --------------------------------------------------------------------------------------------------------------------------
void caseLines() {
    Rig r;
    r.motion(true, 0, false, false);
    r.pressAndGoBlack();
    char buf[ecm::kLineBytes];
    ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::Start));
    check(has(buf, "BEFORE the camera opens") && has(buf, "3 s at most") && has(buf, "up to 1000 ms more"), "M10.a the Start line of an entry says the motion hold exists and how long it can last");
    ecm::ComfortLine line;
    line.ev = ecm::ComfortEv::MotionTimedOut;
    line.kind = ecm::ComfortKind::Enter;
    line.unmet = ecm::kComfortUnmetViews;
    line.viewsRun = 2;
    line.heldMs = 2500;
    ecm::formatComfort(buf, sizeof(buf), line);
    check(has(buf, "the engine's motion views were live for 2 of 3 frames in a row") && !has(buf, "second skin"), "M10.b the unmet text names the views and the frames, and only the conditions that were missing");
}

// ---- M11 --------------------------------------------------------------------------------------------------------------------------
size_t occurrences(const std::string& text, const char* needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
bool readText(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char chunk[4096];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) out->append(chunk, got);
    std::fclose(f);
    return true;
}
const char* labelf(const char* fmt, unsigned long long a, unsigned b) {
    static char buf[160];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
}

void caseConversion() {
    // Exact at the frequencies where microseconds are ticks over a whole number (f = m MHz: us = ticks / m), either side of the tick where the product wraps,
    // and at a tick far beyond it.
    struct Exact { uint64_t freq, m; };
    const Exact exact[] = {{1000000ull, 1}, {10000000ull, 10}, {24000000ull, 24}, {1000000000ull, 1000}};
    bool ok = true;
    for (const Exact& e : exact) {
        for (uint64_t t = kWrapTick - 2; t <= kWrapTick + 2; ++t) ok = ok && ecm::qpcTicksToUs(t, e.freq) == t / e.m;
        ok = ok && ecm::qpcTicksToUs(1ull << 62, e.freq) == (1ull << 62) / e.m;
    }
    check(ok, "M11.a the conversion is exact (ticks / m at m MHz) at 1, 10, 24 and 1000 MHz, from two ticks before the wrap point to two after it, and at 2^62 ticks");
    // Every frequency a machine reports, across the wrap point: never backwards, and within a microsecond of the arithmetic done in floating point.
    const uint64_t freqs[] = {1000ull, 3579545ull, 10000000ull, 19200000ull, 24000000ull, 1000000000ull};
    bool monotone = true, agrees = true;
    for (uint64_t f : freqs) {
        uint64_t prev = 0;
        const uint64_t step = f / 50 ? f / 50 : 1;
        for (uint64_t t = kWrapTick - 3 * f; t <= kWrapTick + 3 * f; t += step) {
            const uint64_t us = ecm::qpcTicksToUs(t, f);
            monotone = monotone && us >= prev;
            prev = us;
            const double ref = double(t) * 1e6 / double(f);
            agrees = agrees && std::fabs(double(us) - ref) <= 1.0 + ref * 1e-14;
        }
    }
    check(monotone, "M11.b across the tick where ticks * 1000000 wraps, at 1 kHz .. 1 GHz, the converted clock never goes backwards");
    check(agrees, "M11.c ...and agrees with the real quotient to a microsecond (to the precision of a double at the largest)");
    check(ecm::qpcTicksToUs(12345678, 0) == 12345678ull * 1000000ull, "M11.d a zero frequency is read as 1 Hz, not divided by");
}

// A tick clock of `freq` Hz whose counter reaches the wrap point `wrapAfterMs` after it starts. Run through ecm::qpcTicksToUs, the way explorer_cam.cpp feeds
// the timeline: the placement cap (an entry never placed, 3 s from the press) and the motion hold (an entry otherwise ready, 1 s) fire on time, once, with
// the clock never stepping back.
void caseWrapTimeline() {
    const uint64_t freqs[] = {10000000ull, 3579545ull, 24000000ull, 1000000000ull};
    const uint32_t wraps[] = {5, 100, 400, 900, 1200, 2900};
    for (uint64_t f : freqs) {
        for (uint32_t w : wraps) {
            Rig r;
            r.startClock(&ecm::qpcTicksToUs, f, w);
            r.motion(true, 5, false, false);
            r.pressAndGoBlack();
            r.in.sessionActive = true;
            r.in.mode = 1;   // the camera opened and never placed the view
            uint32_t held = 0;
            while (r.tl.phase() == ecm::ComfortPhase::Black && held < 5000) { r.tick(); held += 10; }
            const ecm::ComfortLine* t = r.find(ecm::ComfortEv::TimedOut);
            check(t && r.count(ecm::ComfortEv::TimedOut) == 1 && t->heldMs >= 2990 && t->heldMs <= 3030 && !r.stepped,
                  labelf("M11.e the placement cap fires once at 3 s from the press on a %llu Hz clock that wraps %u ms in, and the clock never steps back", f, w));
            r.run(500);
            check(r.tl.phase() == ecm::ComfortPhase::Clear && r.alpha == 0.0f, labelf("M11.f ...and the view is clear after it (%llu Hz, wrap at %u ms)", f, w));

            Rig m;
            m.startClock(&ecm::qpcTicksToUs, f, w);
            m.motion(true, 0, false, false);
            m.pressAndGoBlack();
            m.goodEntry();
            const uint32_t ms = m.untilFadeIn(3000);
            const ecm::ComfortLine* l = m.find(ecm::ComfortEv::MotionTimedOut);
            check(l && m.count(ecm::ComfortEv::MotionTimedOut) == 1 && m.count(ecm::ComfortEv::TimedOut) == 0 && ms >= 990 && ms <= 1020 && !m.stepped,
                  labelf("M11.g the motion hold gives up once, 1 s after the entry was ready, on a %llu Hz clock that wraps %u ms in, and the clock never steps back", f, w));
        }
    }
    // The scenario does cross the wrap: the multiply-first conversion steps the clock back in it, so the checks above are not vacuous.
    Rig s;
    s.startClock(&multiplyFirst, 10000000ull, 400);
    s.motion(true, 5, false, false);
    s.pressAndGoBlack();
    s.run(500);
    check(s.stepped, "M11.h the multiply-first conversion does step the clock back in this scenario (the rig crosses the wrap point)");
}

// The glue (src/d3d11/explorer_cam.cpp) is read as text: both of its conversions of QueryPerformanceCounter go through ecm::qpcTicksToUs.
void caseGlue(const std::string& root) {
    if (root.empty()) return;   // --dry-run has no repository root: the --self-test run reads the file
    std::string text;
    const bool read = readText(root + "/src/d3d11/explorer_cam.cpp", &text);
    check(read, "M11.p0 src/d3d11/explorer_cam.cpp can be read under the repository root");
    if (!read) return;
    check(occurrences(text, "return ecm::qpcTicksToUs(qpcNow(), freq);") == 1, "M11.p1 realNowUs converts the counter through ecm::qpcTicksToUs");
    check(occurrences(text, "in.nowUs = ecm::qpcTicksToUs(static_cast<uint64_t>(t.QuadPart), freq);") == 1,
          "M11.p2 the frame boundary's clock input (ComfortInputs::nowUs) converts the counter through ecm::qpcTicksToUs");
    check(!has(text, "qpcNow() * 1000000") && !has(text, "t.QuadPart) * 1000000") && !has(text, "in.nowUs = static_cast<uint64_t>"),
          "M11.p3 neither of them multiplies the counter by 1000000 itself");
}

// ---- M12 --------------------------------------------------------------------------------------------------------------------------
// The timeline does not depend on its caller's clock being monotonic: a reading below the one before is no time passing, and the caps run on from there.
// The step back used is the one the wrap made (1,844,674,407,371 us at 10 MHz).
constexpr uint64_t kBackUs = 1844674407371ull;
void caseBackwards() {
    // The rigs' clock starts at 5 s: lift it by the step so the step back does not wrap below zero.
    Rig r;
    r.us += kBackUs;
    r.motion(true, 5, false, false);
    r.pressAndGoBlack();
    r.in.sessionActive = true;
    r.in.mode = 1;
    r.run(1000);
    check(r.tl.phase() == ecm::ComfortPhase::Black && r.count(ecm::ComfortEv::TimedOut) == 0, "M12.pre black 1.2 s in, nothing placed");
    r.us -= kBackUs;
    uint32_t held = 0;
    while (r.tl.phase() == ecm::ComfortPhase::Black && held < 5000) { r.tick(); held += 10; }
    const ecm::ComfortLine* t = r.find(ecm::ComfortEv::TimedOut);
    check(t && r.count(ecm::ComfortEv::TimedOut) == 1 && t->heldMs >= 2990 && t->heldMs <= 3030 && held < 2000,
          "M12.a the clock steps back mid-wait: the 3 s cap still fires, at 3 s from the press by the time that really passed (not never)");

    Rig m;
    m.us += kBackUs;
    m.motion(true, 0, false, false);
    m.pressAndGoBlack();
    m.goodEntry();
    m.run(400);
    m.us -= kBackUs;
    const uint32_t ms = m.untilFadeIn(3000);
    const ecm::ComfortLine* l = m.find(ecm::ComfortEv::MotionTimedOut);
    check(l && m.count(ecm::ComfortEv::MotionTimedOut) == 1 && l->motionMs >= 990 && l->motionMs <= 1020 && ms >= 580 && ms <= 640,
          "M12.b the clock steps back during the motion hold: the hold still ends 1 s after the entry was ready (about 600 ms more)");

    Rig a;
    a.us += kBackUs;
    a.motion(true, 5, false, false);
    a.in.pressEnter = true;
    for (int i = 0; i < 5; ++i) a.tick();
    const float before = a.alpha;
    a.us -= kBackUs;
    a.tick();
    const float atStep = a.alpha;
    a.tick();
    check(before > 0.0f && atStep == before && a.alpha > atStep, "M12.c a step back during the fade out stands the ramp still for that frame (no jump, no reversal) and it carries on");

    // The resets that clear an entry (Cleared, stood down) wipe the shift but keep the last reading on the shifted clock, so the next step finds the shift
    // again; the next entry's caps run on the real time, not on the caller's clock catching up with the step it took. (No mutation separates these two checks
    // from the rebase's own: a reset that lost the last reading would still pass, because a fresh timeline starts its press on whatever clock it is given.)
    Rig c;
    c.us += kBackUs;
    c.motion(true, 5, false, false);
    c.pressAndGoBlack();
    c.goodEntry();
    c.us -= kBackUs;
    c.run(1000);
    check(c.count(ecm::ComfortEv::Cleared) == 1 && c.tl.phase() == ecm::ComfortPhase::Clear, "M12.d.pre an entry completes across a step back");
    c.in.sessionActive = false;
    c.in.placed = false;
    c.in.requestPending = true;   // the next F5's request is never taken
    c.in.pressEnter = true;
    c.tick();
    uint32_t heldC = 0;
    while (c.tl.phase() != ecm::ComfortPhase::Clear && c.count(ecm::ComfortEv::TimedOut) == 0 && heldC < 5000) { c.tick(); heldC += 10; }
    const ecm::ComfortLine* tc = c.find(ecm::ComfortEv::TimedOut);
    check(tc && tc->heldMs >= 2990 && tc->heldMs <= 3030, "M12.d after the reset that clears a completed entry the next entry's 3 s cap fires on time");

    Rig d;
    d.us += kBackUs;
    d.motion(true, 5, false, false);
    d.pressAndGoBlack();
    d.in.sessionActive = true;
    d.in.mode = 1;
    d.run(300);
    d.us -= kBackUs;
    d.tick();
    d.in.active = false;   // Explorer Cam stands down: the view is clear at once, the timeline restarts
    d.tick();
    check(d.count(ecm::ComfortEv::Dropped) == 1 && d.tl.phase() == ecm::ComfortPhase::Clear, "M12.e.pre standing down drops the entry");
    d.in.active = true;
    d.in.sessionActive = false;
    d.in.requestPending = true;
    d.in.pressEnter = true;
    d.tick();
    uint32_t heldD = 0;
    while (d.tl.phase() != ecm::ComfortPhase::Clear && d.count(ecm::ComfortEv::TimedOut) == 0 && heldD < 5000) { d.tick(); heldD += 10; }
    const ecm::ComfortLine* td = d.find(ecm::ComfortEv::TimedOut);
    check(td && td->heldMs >= 2990 && td->heldMs <= 3030, "M12.e after the stand-down reset the next entry's 3 s cap fires on time");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (i == 2 && argv[1] == std::string("--self-test")) root = a;
        else {
            std::fprintf(stderr, "usage: explorer_cam_fade_test --self-test [repository root] | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: explorer_cam_fade_test --self-test [repository root] | --dry-run\n");
        return 2;
    }
    caseConstants();
    caseEarly();
    caseLate();
    caseNever();
    caseNoJobs();
    caseUnarmed();
    caseExit();
    caseReattach();
    caseRestart();
    caseCap();
    caseLines();
    caseConversion();
    caseWrapTimeline();
    caseGlue(root);
    caseBackwards();
    if (g_failures) {
        std::printf("FAIL: explorer cam fade: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u explorer cam fade checks\n", g_checks);
    return 0;
}
