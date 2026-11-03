// The SRV turret's entry fade (issue 140; src/d3d11/srv_turret_entry_fade.h, srv_turret_entry_fade.cpp): the hold to black from the turret-view-on edge until the camera has settled,
// then a short fade back in. No device, no game: the state machine, run frame by frame as the module runs it, and the log lines the instrument writes, plus a scan of the sources that
// connect the module to the game.
//
//   THE MACHINE: the hold starts on the edge, at level 1, in the edge's own frame; the move published in that frame is the entry's (a publish counted before the edge is the last
//   entry's); the applied-serial signal ends the hold kSettleMarginMs after the runtime reports the move's serial applied (an older serial does not count; a later publish retargets);
//   with a runtime that reports nothing the published move plus kNoSignalMs ends it; a signal that never comes ends it at the cap, so the whole run is over by kCapMs; the fade-in is
//   linear over kFadeInMs and never rises; the view going off ends the hold at once; a move that is never published ends it at kPublishWaitMs; no move key means no hold; the level stays
//   in [0, 1] and is exactly 0 outside a run; staying on never re-triggers, a new edge does, one during a fade-in cuts the run short and starts again at 1.
//   THE LINES: the run line, the skipped line, the trace header and a full trace line fit the log's 1200-character line, carry the frames and the signal, say FIRED only when the cap fired,
//   and none contains a phrase tools/edvr_log.py's turret rule reads.
//   THE WIRING: the module is called every frame after alignTick and before the mix, its level is a term of FadeMix's max, the move's publish is told to it, the luma probe's frame-by-frame mode
//   and the Status.json cadence are asked for, and the new source is in the build.
#include "../../src/d3d11/srv_turret_entry_fade.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace ef = edvr::srv_turret::entryfade;

static unsigned g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

static std::string readText(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("FAIL: cannot read %s (the rig runs from the repository root)\n", path);
        std::exit(1);
    }
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
static bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// One scripted run: a frame every 11.1 ms (90 Hz). The caller sets what the world does per frame.
struct Sim {
    ef::Machine m;
    double t = 1000.0;
    uint32_t frame = 100;
    uint32_t count = 0, serial = 0, applied = 1;   // the move's publish count and latest serial; the pose's word (serial + 1 of the alignment it carries: 1 is "none applied")
    bool on = false, moving = true;
    ef::Events last;
    double level = 0.0;
    static constexpr double kDt = 11.1;

    // One frame. publish: the move is published in this frame (counted before the step, as the module's order does: alignTick runs first).
    double tick(bool publishNow = false, uint32_t newSerial = 0) {
        if (publishNow) {
            ++count;
            serial = newSerial;
        }
        ef::Input in;
        in.nowMs = t;
        in.frame = frame;
        in.on = on;
        in.moving = moving;
        in.moveCount = count;
        in.moveSerial = serial;
        in.appliedWord = applied;
        level = m.step(in, last);
        t += kDt;
        ++frame;
        return level;
    }
    // Ticks until pred or ms elapsed; returns the elapsed ms.
    template <class P> double until(P pred, double ms) {
        const double t0 = t;
        while (t - t0 < ms && !pred()) tick();
        return t - t0;
    }
};

static void machineCases() {
    // ---- the nominal entry: the edge, the publish in the edge's frame, the runtime applies it a few frames later ----------------------------------------------------------------
    {
        Sim s;
        for (int i = 0; i < 20; ++i) s.tick();
        check(s.level == 0.0 && !s.last.started, "off the view the level is exactly 0 and nothing starts");
        s.on = true;
        s.applied = 4;   // an older alignment (serial 3) is what the pose carries
        const double edgeT = s.t;
        const uint32_t edgeFrame = s.frame;
        s.tick(true, 7);   // the edge's frame: alignTick published serial 7 before the machine ran
        check(s.last.started && s.level == 1.0, "the hold starts on the edge, at level 1, in the edge's own frame");
        check(s.last.published && s.m.run().published && s.m.run().serial == 7 && s.m.run().publishFrame == edgeFrame, "the move published in the edge's frame is the entry's");
        check(s.m.run().startFrame == edgeFrame && s.m.run().entry == 1, "the run is numbered and remembers the frame it began in");
        for (int i = 0; i < 6; ++i) s.tick();
        check(s.level == 1.0 && !s.m.run().appliedSeen && s.m.phase() == ef::Phase::kHold, "an older applied serial does not end the hold (serial 3 carried, 7 published)");
        s.applied = 8;   // serial 7 + 1: the pose carries the move
        s.tick();
        check(s.last.appliedSeen && s.m.run().appliedSeen, "the runtime reporting the move's serial applied is seen in that frame");
        const double appliedT = s.m.run().appliedMs;
        const double held = s.until([&] { return s.m.phase() != ef::Phase::kHold; }, 400.0);
        check(s.m.run().settle == ef::Settle::kApplied, "the hold ends by the applied serial");
        check(s.m.run().settleMs >= appliedT + ef::kSettleMarginMs && s.m.run().settleMs < appliedT + ef::kSettleMarginMs + 2 * Sim::kDt + 0.01,
              "... kSettleMarginMs after it, to within a frame");
        (void)held;
        check(s.last.settled && s.m.phase() == ef::Phase::kFadeIn && s.level == 1.0, "the fade-in begins at 1 in the frame the hold ends");
        // The fade-in is linear and never rises.
        double prev = 1.0;
        bool monotone = true, inRange = true;
        const double fadeStart = s.m.run().settleMs;
        double mid = -1.0;
        while (s.m.phase() == ef::Phase::kFadeIn) {
            s.tick();
            if (s.level > prev + 1e-12) monotone = false;
            if (s.level < 0.0 || s.level > 1.0) inRange = false;
            if (mid < 0.0 && s.t - fadeStart >= ef::kFadeInMs * 0.5) mid = s.level;
            prev = s.level;
        }
        check(monotone && inRange, "the fade-in never rises and stays in [0, 1]");
        check(mid > 0.35 && mid < 0.6, "the fade-in is linear: about half at half its time");
        check(s.last.ended && s.level == 0.0 && s.m.run().fadeEnded && s.m.phase() == ef::Phase::kIdle, "the level reaches exactly 0 and the run ends");
        check(s.m.run().endMs - fadeStart >= ef::kFadeInMs && s.m.run().endMs - fadeStart < ef::kFadeInMs + Sim::kDt + 0.01, "... after kFadeInMs");
        check(s.m.run().endMs - edgeT < ef::kCapMs, "the whole run is inside the cap");
        check(!s.m.run().capFired, "the cap did not fire on a run that got its signal");
        // Staying on does not re-trigger.
        const uint32_t entries = s.m.entries();
        bool any = false;
        for (int i = 0; i < 400; ++i) {
            s.tick();
            if (s.last.started || s.last.skipped || s.last.settled || s.last.ended || s.level != 0.0) any = true;
        }
        check(!any && s.m.entries() == entries && s.m.phase() == ef::Phase::kIdle, "the view staying on does not re-trigger the hold, however long it stays");
        // Off and on again: a new entry.
        s.on = false;
        for (int i = 0; i < 5; ++i) s.tick();
        check(s.level == 0.0 && s.m.entries() == entries, "the view going off after a finished run starts nothing");
        s.on = true;
        s.tick(true, 9);
        check(s.last.started && s.level == 1.0 && s.m.entries() == entries + 1 && s.m.run().serial == 9, "the next edge is a new entry with its own serial");
        check(s.m.havePrev() && s.m.prev().entry == entries && s.m.prev().fadeEnded, "... and the run before it is kept whole for its report");
    }
    // ---- a publish counted before the edge is the last entry's ----------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.count = 3;   // an earlier entry published three times
        s.serial = 40;
        s.applied = 41;   // and the pose carries serial 40
        for (int i = 0; i < 3; ++i) s.tick();   // frames off the view: the machine has seen the count
        s.on = true;
        s.tick();   // an edge with no publish in its frame
        check(s.last.started && !s.m.run().published, "an old publish is not this entry's move");
        s.until([&] { return s.m.phase() != ef::Phase::kHold; }, 2000.0);
        check(s.m.run().settle == ef::Settle::kNoPublish && !s.m.run().capFired, "a move never published ends the hold at kPublishWaitMs, not at the cap");
        check(s.m.run().settleMs - s.m.run().startMs < ef::kPublishWaitMs + 2 * Sim::kDt + 0.01, "... after kPublishWaitMs");
    }
    // ---- no applied-serial signal: the published move plus the margin -----------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.applied = 0;   // the runtime reports nothing
        s.on = true;
        s.tick(true, 5);
        s.until([&] { return s.m.phase() != ef::Phase::kHold; }, 2000.0);
        check(s.m.run().settle == ef::Settle::kMargin && !s.m.run().sawReport && !s.m.run().appliedSeen, "with no report the hold ends by the published move plus the margin");
        check(s.m.run().settleMs - s.m.run().publishMs >= ef::kNoSignalMs && s.m.run().settleMs - s.m.run().publishMs < ef::kNoSignalMs + 2 * Sim::kDt + 0.01, "... kNoSignalMs after the publish");
        check(!s.m.run().capFired, "... and the cap is not what ended it");
    }
    // ---- a signal that never comes: the cap ---------------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.applied = 3;   // the runtime reports, but never the move's serial
        s.on = true;
        s.tick(true, 5);
        const double t0 = s.m.run().startMs;
        s.until([&] { return s.m.phase() == ef::Phase::kIdle && s.m.run().fadeEnded; }, 5000.0);
        check(s.m.run().sawReport && !s.m.run().appliedSeen, "a runtime that reports but never the move's serial is told from one that does not report");
        check(s.m.run().settle == ef::Settle::kCap && s.m.run().capFired, "the cap fires when the signal never comes");
        check(s.m.run().settleMs - t0 >= ef::kCapMs - ef::kFadeInMs && s.m.run().settleMs - t0 < ef::kCapMs - ef::kFadeInMs + 2 * Sim::kDt + 0.01, "... so the hold ends at the cap less the fade-in");
        check(s.m.run().endMs - t0 >= ef::kCapMs - 1.0 && s.m.run().endMs - t0 < ef::kCapMs + 3 * Sim::kDt, "... and the whole run, fade-in included, ends at the cap (a frame or two over)");
        check(s.level == 0.0, "the headset is not left black");
    }
    // ---- the view going off in the hold ---------------------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.applied = 1;
        s.on = true;
        s.tick(true, 5);
        for (int i = 0; i < 5; ++i) s.tick();
        s.on = false;
        s.tick();
        check(s.last.settled && s.m.run().settle == ef::Settle::kViewOff && s.m.phase() == ef::Phase::kFadeIn, "the view going off in the hold ends it at once, into the fade-in");
        s.until([&] { return s.m.phase() == ef::Phase::kIdle; }, 1000.0);
        check(s.level == 0.0 && s.m.run().fadeEnded && !s.m.run().capFired, "... which finishes at 0 without the cap");
    }
    // ---- no move key: nothing to wait for ---------------------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.moving = false;
        s.on = true;
        s.tick();
        check(s.last.skipped && !s.last.started && s.level == 0.0 && s.m.run().skip == ef::Skip::kNoMove, "with the camera move off the edge starts no hold and says so");
        for (int i = 0; i < 50; ++i) s.tick();
        check(s.level == 0.0 && s.m.phase() == ef::Phase::kIdle && s.m.entries() == 1, "... and stays quiet while the view stays on");
    }
    // ---- a new edge during a fade-in cuts the run short -----------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.applied = 0;
        s.on = true;
        s.tick(true, 5);
        s.until([&] { return s.m.phase() == ef::Phase::kFadeIn; }, 2000.0);
        s.tick();
        s.tick();
        s.on = false;
        s.tick();
        s.on = true;
        s.tick(true, 6);
        check(s.last.started && s.level == 1.0 && s.m.entries() == 2, "an edge during a fade-in starts a new hold at 1");
        check(s.m.havePrev() && s.m.prev().cutShort && !s.m.prev().fadeEnded && s.m.prev().entry == 1, "... and the run it cut short is kept, marked, for its report");
    }
    // ---- a later publish of the move retargets the signal ------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.applied = 6;
        s.on = true;
        s.tick(true, 5);
        s.tick();
        check(s.m.run().appliedSeen, "the first publish is seen applied");
        s.tick(true, 9);   // the key changed in the view: the move is published again
        s.tick();
        check(s.m.run().serial == 9 && !s.m.run().appliedSeen, "a later publish retargets the signal");
        s.applied = 10;
        s.tick();
        check(s.m.run().appliedSeen && s.m.run().appliedFrame == s.frame - 1, "... which then needs its own report");
    }
    // ---- reset ------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        Sim s;
        s.on = true;
        s.tick(true, 5);
        s.m.reset();
        check(s.m.level() == 0.0 && s.m.phase() == ef::Phase::kIdle, "a stood-down machine is idle at 0");
    }
    // ---- the constants ---------------------------------------------------------------------------------------------------------------------------------------------------------
    static_assert(ef::kCapMs - ef::kFadeInMs > ef::kNoSignalMs + 100.0, "the margin fallback must be able to fire before the cap");
    static_assert(ef::kCapMs - ef::kFadeInMs > ef::kPublishWaitMs, "the no-publish end must come before the cap");
    static_assert(ef::kSettleMarginMs < ef::kNoSignalMs, "the unconfirmed margin is the longer");
    static_assert(ef::kCapMs <= 1100.0, "the whole run is held to about a second");
    static_assert(ef::kMaxSamples == ef::kMaxTraceLines * ef::kSamplesPerLine, "the trace cap is lines times samples");
    check(true, "the constants hold their relations (static)");
}

static bool noReadPhrase(const char* line) {
    // The phrases tools/edvr_log.py's turret rule (H_SRV_TURRET_LABELS) matches a line on: none of these lines may contain one.
    static const char* labels[] = {"srv turret view", "srv turret camera", "srv turret settings", "srv turret hud", "srv turret anchor", "srv turret:"};
    for (const char* l : labels)
        if (std::strstr(line, l)) return false;
    return true;
}

static void lineCases() {
    ef::Run r;
    r.entry = 3;
    r.startMs = 5000.0;
    r.startFrame = 9541;
    r.published = true;
    r.publishMs = 5000.0;
    r.publishFrame = 9541;
    r.serial = 7;
    r.sawReport = true;
    r.appliedSeen = true;
    r.appliedMs = 5033.0;
    r.appliedFrame = 9544;
    r.settle = ef::Settle::kApplied;
    r.settleMs = 5183.0;
    r.settleFrame = 9558;
    r.fadeEnded = true;
    r.endMs = 5383.0;
    r.endFrame = 9576;
    char line[1200];
    int n = ef::formatRun(line, sizeof(line), r);
    check(n > 0 && static_cast<size_t>(n) < sizeof(line), "the run line fits the log's line");
    check(has(line, "srv turret entry fade: entry 3: hold started at frame 9541") && has(line, "move published at frame 9541 (serial 7)") && has(line, "applied at frame 9544 (+33 ms)") &&
              has(line, "settled at frame 9558 (+183 ms) by the runtime's applied serial plus the margin") && has(line, "fade-in ended at frame 9576 (+383 ms)") && has(line, "cap (1000 ms) not fired"),
          "the run line carries the frames, the times from the edge, the signal and the cap");
    check(!has(line, "FIRED") && noReadPhrase(line), "a run whose cap did not fire does not say FIRED, and the line holds no phrase the turret health rule reads");
    r.capFired = true;
    r.settle = ef::Settle::kCap;
    r.appliedSeen = false;
    ef::formatRun(line, sizeof(line), r);
    check(has(line, "cap (1000 ms) FIRED") && has(line, "by the cap") && has(line, "reports an applied serial and never reported this one"), "a fired cap and a missing signal are said");
    r.sawReport = false;
    r.settle = ef::Settle::kMargin;
    r.capFired = false;
    ef::formatRun(line, sizeof(line), r);
    check(has(line, "reported no applied serial") && has(line, "published move plus the margin"), "a runtime that reports nothing is said, and the margin that ended the hold");
    r.fadeEnded = false;
    r.cutShort = true;
    ef::formatRun(line, sizeof(line), r);
    check(has(line, "cut short by the next entry"), "a run cut short says so");

    ef::Run k;
    k.entry = 4;
    k.startFrame = 12752;
    k.skip = ef::Skip::kNoMove;
    n = ef::formatSkipped(line, sizeof(line), k);
    check(n > 0 && has(line, "entry 4 at frame 12752: no hold ran: the camera move is off") && noReadPhrase(line), "the skipped line says no hold ran and why");
    check(!has(line, "hold started") && !has(line, "luma"), "... and is told apart from a run's lines");

    // The trace: a full line fits, the phases are named, an unread eye is '-'.
    ef::Run t = r;
    t.settle = ef::Settle::kApplied;
    t.fadeEnded = true;
    t.cutShort = false;
    check(ef::phaseAt(t, -100.0) == ' ' && ef::phaseAt(t, 50.0) == 'H' && ef::phaseAt(t, 250.0) == 'F' && ef::phaseAt(t, 500.0) == 'a', "the phases: before, hold, fade-in, after");
    n = ef::formatTraceHeader(line, sizeof(line), t, 190, 5);
    check(has(line, "entry 3 luma") && has(line, "190 samples, thinned") && noReadPhrase(line), "the trace header names the entry, the count and the thinning");
    size_t len = static_cast<size_t>(n);
    for (uint32_t i = 0; i < ef::kSamplesPerLine; ++i) {
        ef::Sample s;
        s.tMs = -299.0 + 11.1 * i + 0.4;
        s.mean[0] = 0.1234f;
        s.mean[1] = i == 3 ? -1.0f : 0.9994f;
        s.phase = i % 3 == 0 ? 'H' : 'F';
        len += static_cast<size_t>(ef::appendSample(line, sizeof(line), len, s));
    }
    check(len < 1100, "a full trace line is inside the log's 1200 characters with room to spare");
    check(has(line, " H-299:0.123/0.999") && has(line, "F") && has(line, "/-"), "samples carry the phase, the time from the edge and both eyes, '-' for an eye that never read back");
    n = ef::formatNoLuma(line, sizeof(line), t);
    check(has(line, "entry 3 luma: no readings") && noReadPhrase(line), "a window with no readings is said");
    n = ef::formatCapLine(line, sizeof(line));
    check(has(line, "12 entries reported") && n > 0, "the session cap says so once it is reached");
    check(ef::kMaxReports * (1 + ef::kMaxTraceLines) <= 80, "the instrument's lines a session are bounded (entries times a run line and its trace lines)");
}

static void wiringCases() {
    const std::string nof = readText("src/d3d11/native_on_foot.cpp");
    const size_t tick = nof.find("g_fadeMix.turret = srvTurretEntryFadeTick(frameNo);");
    const size_t align = nof.find("alignTick(GetTickCount64());   // F33: the body align");
    const size_t next = nof.find("const double fade = g_fadeMix.next();");
    check(tick != std::string::npos && align != std::string::npos && next != std::string::npos && align < tick && tick < next,
          "native_on_foot.cpp calls the entry fade after alignTick and before the mix is read, and puts its level in the mix's turret term");
    const std::string f123 = readText("src/d3d11/native_on_foot_f123.h");
    check(has(f123, "double snap = 0.0, contain = 0.0, turret = 0.0;") && has(f123, "turret > 0.0 ? turret : 0.0"), "FadeMix's max has the turret term");
    const std::string st = readText("src/d3d11/srv_turret.cpp");
    const size_t note = st.find("void srvTurretNotePublished(");
    check(note != std::string::npos && st.find("srvTurretEntryFadeNoteMove(serial)", note) != std::string::npos && has(st, "bool srvTurretViewOn() { return g_on; }"),
          "srv_turret.cpp tells the entry fade the serial of the camera move and says whether the view is on");
    const std::string mod = readText("src/d3d11/srv_turret_entry_fade.cpp");
    check(has(mod, "lumaProbeBurst(inSrv)") && has(mod, "journalWatchSetEagerStatusSrv(inSrv)") && has(mod, "np.inputVersion >= 3") && has(mod, "ef::formatSkipped") && has(mod, "ef::formatRun"),
          "the module asks for the frame-by-frame luma and the 100 ms Status.json in the Scorpion, reads the applied word only from input version 3, and writes both the run and the skipped line");
    check(has(mod, "if (!vr) {") && has(mod, "g_machine.reset();"), "the module stands down outside VR (the comfort fade is the runtime's)");
    const std::string luma = readText("src/d3d11/luma_probe.cpp");
    check(has(luma, "if (stage == 0 && g_burstOn.load(std::memory_order_relaxed)) burstSample(ctx, tex, eye);") && has(luma, "burstPoll(ctx, eye);"),
          "the luma probe samples stage 0 every pass only while the mode is on, and polls it every pass");
    const std::string jw = readText("src/d3d11/journal_watch.cpp");
    check(has(jw, "g_eagerSrv.load(std::memory_order_relaxed)") && has(jw, "void journalWatchSetEagerStatusSrv(bool eager)"), "the journal watcher has the SRV cadence request and reads it in its wake-up");
    const std::string bat = readText("build.bat");
    check(has(bat, "\"src\\d3d11\\srv_turret_entry_fade.cpp\""), "build.bat compiles the module into d3d11.dll");
}

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("srv_turret_entry_fade_test: dry-run (no files, no device)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test")) {
        std::puts("usage: srv_turret_entry_fade_test --self-test | --dry-run");
        return 2;
    }
    machineCases();
    lineCases();
    wiringCases();
    std::printf("PASS srv_turret_entry_fade_test --self-test: %u checks\n", g_checks);
    return 0;
}
