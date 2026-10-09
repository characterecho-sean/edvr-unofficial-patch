// explorer_cam_fade_test: the entry fade's wait for the engine's motion (src/d3d11/explorer_cam_fade_core.h, "MOTION"), pure.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check; the optional argument is the repository root (unused)
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

struct Rig {
    ecm::ComfortTimeline tl;
    ecm::ComfortInputs in;
    uint64_t us = 5000000;
    std::vector<ecm::ComfortLine> events;
    uint32_t releasedEnter = 0, releasedExit = 0;
    float alpha = 0.0f;
    Rig() {
        in.active = true;
        in.ctlCalls = 100;
    }
    ecm::ComfortStep tick(uint32_t ms = 10) {
        us += uint64_t(ms) * 1000;
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
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (i == 2 && argv[1] == std::string("--self-test")) continue;
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
    if (g_failures) {
        std::printf("FAIL: explorer cam fade: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u explorer cam fade checks\n", g_checks);
    return 0;
}
