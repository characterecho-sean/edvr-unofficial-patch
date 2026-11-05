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
//   THE EARLY EDGE (srv_turret_sheet_latch.h): the gunner sheet's draw raises the view before Status.json does; Status.json taking over is no edge and says how long it lagged; a latch with no
//   sheet draw for a second expires; a non-Scorpion never raises it; the off edge stays Status-driven (a sheet that stops does not turn a Status-on view off, a Status off edge with the sheet
//   still drawing latches again on its next draw, but a draw from before the off edge does not: leaving the turret turns the view off at the Status.json edge); and the entry fade's hold starts on the early edge.
//   THE PRESS (srv_turret_sheet_latch.h EntryGate): the player's turret key opens the draw gate for kPressWaitMs (it blacks nothing: the hold starts at the edge), and the
//   first sheet draw in the window raises the view with the cap counted from the sheet; no sheet and the window closes with no edge and no black; presses outside the Scorpion's SRV seat, with GuiFocus
//   above 0 or with the view on do nothing; Status.json ends the wait; a key that cannot be observed, or a view with no press seen before it, trips the fallback to the always-open gate,
//   and a change of bindings resets it.
//   THE WIRING: the module is called every frame after alignTick and before the mix, its level is a term of FadeMix's max, the move's publish is told to it, the luma probe's frame-by-frame mode
//   and the Status.json cadence are asked for, and the new source is in the build.
#include "../../src/d3d11/srv_turret_entry_fade.h"
#include "../../src/d3d11/srv_turret_sheet_latch.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace ef = edvr::srv_turret::entryfade;
namespace sl = edvr::srv_turret::sheetlatch;

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
    static_assert(ef::kLeadMs == 1000.0, "the luma window starts a second before the edge, to show the game's own fade against the early edge");
    // A run's window (lead + cap + tail) at 120 Hz is all kept, and the buffer (kPairs, 384 in srv_turret_entry_fade.cpp) holds that window plus the report delay at 144 Hz.
    static_assert((ef::kLeadMs + ef::kCapMs + ef::kTailMs) / (1000.0 / 120.0) <= ef::kMaxSamples, "a run's trace at 120 Hz is kept whole by the sample cap");
    static_assert((ef::kLeadMs + ef::kCapMs + ef::kReportAfterMs) / (1000.0 / 144.0) <= 384.0, "the rolling buffer holds a run's window plus the report delay at 144 Hz");
    check(true, "the constants hold their relations (static)");
}

// ---- the early edge ------------------------------------------------------------------------------------------------------------------------------------------------------------
static sl::Result latchStep(sl::Latch& l, uint64_t now, bool status, bool scorpion, uint64_t seen) {
    sl::Input in;
    in.nowMs = now;
    in.statusOn = status;
    in.scorpion = scorpion;
    in.sheetSeenMs = seen;
    return l.step(in);
}

static void latchCases() {
    {   // the sheet raises the view before Status.json, once; Status.json taking over is not an edge and says the lag
        sl::Latch l;
        sl::Result r = latchStep(l, 1000, false, true, 0);
        check(!r.on && r.event == sl::Event::kNone, "no sheet draw and Status.json off: the view is off");
        r = latchStep(l, 1100, false, true, 1090);
        check(r.on && r.event == sl::Event::kRaised, "a sheet draw 10 ms ago with Status.json off raises the view");
        r = latchStep(l, 1111, false, true, 1105);
        check(r.on && r.event == sl::Event::kNone, "... and the next frame is on with no second raise");
        r = latchStep(l, 1300, true, true, 1295);
        check(r.on && r.event == sl::Event::kStatusAgreed && r.lagMs == 200, "Status.json reporting the turret takes over: still on, no edge, the lag behind the raise said (200 ms)");
        r = latchStep(l, 1311, true, true, 1305);
        check(r.on && r.event == sl::Event::kNone && !l.latched(), "... and then it is Status.json alone");
    }
    {   // a Status.json edge with no sheet (and no latch): no agreed event
        sl::Latch l;
        sl::Result r = latchStep(l, 5000, true, true, 0);
        check(r.on && r.event == sl::Event::kNone, "Status.json on with the sheet never seen: on, and no agreed event (the latch never ran)");
    }
    {   // the latch expires after a second without a sheet draw while Status.json stays off
        sl::Latch l;
        latchStep(l, 1000, false, true, 990);
        sl::Result r = latchStep(l, 1990, false, true, 990);
        check(r.on && r.event == sl::Event::kNone, "a sheet draw 1000 ms ago is still fresh: the latch holds");
        r = latchStep(l, 1991, false, true, 990);
        check(!r.on && r.event == sl::Event::kExpired && r.lagMs == 991, "no sheet draw for more than 1000 ms with Status.json off: the latch expires (a false positive turns off again), the hold said");
        r = latchStep(l, 2100, false, true, 990);
        check(!r.on && r.event == sl::Event::kNone, "... and stays off on the stale sighting");
        r = latchStep(l, 2200, false, true, 2195);
        check(r.on && r.event == sl::Event::kRaised, "... a fresh sheet draw raises it again");
    }
    {   // a sheet that keeps drawing keeps the latch past a second
        sl::Latch l;
        uint64_t seen = 1000;
        bool on = true;
        for (uint64_t now = 1000; now < 4000; now += 11) {
            seen = now - 5;
            on = latchStep(l, now, false, true, seen).on && on;
        }
        check(on, "a sheet drawn every frame holds the latch for three seconds with Status.json off");
    }
    {   // a non-Scorpion SRV never raises it, whatever the sheet does, and Status.json's bit alone does not make the Scarab's view the Scorpion's
        sl::Latch l;
        sl::Result r = latchStep(l, 1000, false, false, 995);
        check(!r.on && r.event == sl::Event::kNone, "a non-Scorpion SRV with a fresh sheet draw is never raised");
        r = latchStep(l, 1100, true, false, 1095);
        check(!r.on && r.event == sl::Event::kNone, "... and Status.json's bit in a non-Scorpion SRV is not the Scorpion's view");
    }
    {   // the journal stops calling the Scorpion while latched
        sl::Latch l;
        latchStep(l, 1000, false, true, 995);
        sl::Result r = latchStep(l, 1100, false, false, 1095);
        check(!r.on && r.event == sl::Event::kDropped && r.lagMs == 100, "the Scorpion unknown while latched drops the latch (said)");
    }
    {   // the off edge stays Status-driven
        sl::Latch l;
        latchStep(l, 1000, false, true, 995);
        latchStep(l, 1200, true, true, 1195);   // Status.json agrees
        // the sheet stops drawing while Status.json still says on: the view stays on
        bool on = true;
        for (uint64_t now = 1300; now < 5000; now += 50) on = latchStep(l, now, true, true, 1195).on && on;
        check(on, "a sheet that stopped drawing does not turn the view off while Status.json says on");
        // Status.json off with the sheet still drawing: the view stays on by the latch, with no edge
        sl::Result r = latchStep(l, 5100, false, true, 5090);
        check(r.on && r.event == sl::Event::kRaised, "a Status.json off edge while the sheet is still drawing latches again (no off edge)");
        // Status.json off with the sheet stopped: off at once
        sl::Latch m;
        latchStep(m, 1000, true, true, 0);
        r = latchStep(m, 1100, false, true, 0);
        check(!r.on && r.event == sl::Event::kNone, "a Status.json off edge with no sheet in the last second turns the view off at once");
        r = latchStep(m, 1200, false, true, 700);   // a sheet last seen before the Status.json on step: stale for a raise (the old rule raised here)
        check(!r.on && r.event == sl::Event::kNone, "... and a sheet draw older than the last step that saw Status.json on does not raise it, however fresh it is");
        r = latchStep(m, 1300, false, true, 1290);
        check(r.on && r.event == sl::Event::kRaised, "... while a draw after the off edge does (a sheet still drawing re-latches on its next draw)");
    }
    {   // the exit: the sheet stops when the game's fade starts, Status.json flips off 600 ms later. The view goes off on that step and nothing raises or expires after it.
        sl::Latch l;
        latchStep(l, 1000, false, true, 990);   // the early edge
        latchStep(l, 1200, true, true, 1190);   // Status.json agrees
        bool onUntil = true;
        for (uint64_t now = 1200; now <= 5400; now += 100) onUntil = latchStep(l, now, true, true, now <= 5000 ? now - 5 : 5000).on && onUntil;
        check(onUntil, "the view is on while Status.json says on");
        // the sheet last drew at 5000; Status.json goes off at 5600 (600 ms later)
        sl::Result r = latchStep(l, 5500, true, true, 5000);
        check(r.on, "... still on at 5500 (the sheet gone 500 ms, Status.json on)");
        r = latchStep(l, 5600, false, true, 5000);
        check(!r.on && r.event == sl::Event::kNone, "the Status.json off step turns the view off at once: no raise from the draw 600 ms old");
        bool quiet = true;
        for (uint64_t now = 5700; now <= 8000; now += 100) {
            r = latchStep(l, now, false, true, 5000);
            quiet = quiet && !r.on && r.event == sl::Event::kNone;
        }
        check(quiet, "... and nothing raises or expires after it (no latch expired line)");
        // a sheet truly still drawing after the off edge re-latches on its next draw
        r = latchStep(l, 8100, false, true, 8095);
        check(r.on && r.event == sl::Event::kRaised, "a draw after the off edge raises it again");
        // the rule is in the source: a mutant that drops the newer-than-Status condition is the old behaviour, which this scenario fails
        const std::string src = readText("src/d3d11/srv_turret_sheet_latch.h");
        check(has(src, "} else if (!latched_ && fresh && newer) {") && has(src, "const bool newer = in.sheetSeenMs > lastStatusOnMs_;") && has(src, "lastStatusOnMs_ = in.nowMs;"),
              "the latch raises only on a sheet draw newer than the last Status.json on step");
        std::string mutant = src;
        const size_t at = mutant.find("fresh && newer");
        mutant.replace(at, 14, "fresh");
        check(!has(mutant, "fresh && newer"), "the mutant that drops the newer-than-Status condition no longer has it, so the source pin above fails on it");
        // and the old behaviour, written out here, raises on the exit step, which the scenario's check above forbids
        const bool oldFresh = (5600 - 5000) <= sl::kExpireMs;
        check(oldFresh, "control: the draw 600 ms old IS fresh, so only the newer-than condition keeps it from raising");
    }
    {   // the entry fade's hold starts on the early edge: the machine is fed the latch's `on`
        sl::Latch l;
        ef::Machine m;
        ef::Events ev;
        bool started = false;
        double at = 0.0;
        for (int f = 0; f < 40; ++f) {
            const double now = 1000.0 + 11.1 * f;
            const bool status = f >= 30;   // Status.json lags the sheet by 20 frames
            const uint64_t seen = f >= 10 ? static_cast<uint64_t>(now) - 3 : 0;
            ef::Input in;
            in.nowMs = now;
            in.frame = 100 + f;
            in.on = latchStep(l, static_cast<uint64_t>(now), status, true, seen).on;
            in.moving = true;
            m.step(in, ev);
            if (ev.started && !started) {
                started = true;
                at = now;
            }
        }
        check(started && at < 1000.0 + 11.1 * 11, "the hold starts on the early edge (frame 10), not on Status.json's (frame 30)");
    }
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
    check(ef::kMaxReports * (1 + ef::kMaxTraceLines) <= 100, "the instrument's lines a session are bounded (entries times a run line and its trace lines)");
}

// ---- THE PRESS (srv_turret_sheet_latch.h EntryGate; the press opens the window, it blacks nothing) ---------------------------------------------------------------------------------------
// The gate and the machine run as the frame tick and the module run them: the gate first (tick-domain milliseconds), then the machine fed the gate's words. A frame every 10 ms.
struct PSim {
    sl::EntryGate g;
    ef::Machine m;
    uint64_t tick = 100000;
    uint32_t frame = 100, count = 0, serial = 0, applied = 1, epoch = 1, nextSerial = 0;
    bool observable = true, scorpion = true, seat = true, gui0 = true, status = false, moving = true, press = false, publishNext = false;
    uint64_t sheet = 0;
    sl::GateOut go;
    ef::Events ev;
    double level = 0.0;
    static constexpr uint64_t kDt = 10;
    double step() {
        sl::GateIn in;
        in.nowMs = tick;
        in.pressEdge = press;
        in.observable = observable;
        in.epoch = epoch;
        in.scorpion = scorpion;
        in.srvSeat = seat;
        in.guiFocusZero = gui0;
        in.statusOn = status;
        in.sheetSeenMs = sheet;
        go = g.step(in);
        if (publishNext) {
            ++count;
            serial = nextSerial;
            publishNext = false;
        }
        ef::Input mi;
        mi.nowMs = static_cast<double>(tick);
        mi.frame = frame;
        mi.on = go.on;
        mi.moving = moving;
        mi.moveCount = count;
        mi.moveSerial = serial;
        mi.appliedWord = applied;
        mi.pressAgeMs = static_cast<double>(go.pressAgeMs);
        mi.sheetEdge = go.sheetEdge;
        mi.noPress = go.noPress;
        mi.fallback = static_cast<uint8_t>(go.fallback);
        mi.sheetLagMs = static_cast<double>(go.sheetLagMs);
        mi.statusLagMs = static_cast<double>(go.statusLagMs);
        level = m.step(mi, ev);
        press = false;
        tick += kDt;
        ++frame;
        return level;
    }
    // Steps for ms; sheetDrawing: a sheet draw in every frame.
    void run(uint64_t ms, bool sheetDrawing = false) {
        const uint64_t t0 = tick;
        while (tick - t0 < ms) {
            if (sheetDrawing) sheet = tick;
            step();
        }
    }
    uint64_t pressNow() {   // a press edge in this frame; returns its tick
        const uint64_t p = tick;
        press = true;
        step();
        return p;
    }
};

static bool nearly(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

static void pressCases() {
    {   // the press opens the gate for the window and does nothing else: the level stays 0 and no entry begins
        PSim s;
        s.run(100);
        check(!s.go.gateOpen && !s.go.waiting && s.level == 0.0, "in the Scorpion's SRV seat with the view off and no press the draw gate is CLOSED (the always-open gate is gone) and the level is 0");
        const uint64_t p = s.pressNow();
        check(s.go.event == sl::GateEvent::kCounted && s.go.waiting && s.go.gateOpen && s.m.phase() == ef::Phase::kIdle, "a press with the Scorpion known, the SRV seat, the view off and GuiFocus 0 counts: the gate opens and the machine stays idle");
        check(sl::kPressWaitMs == 2000, "kPressWaitMs is 2000 ms");
        bool open = true, black = false;
        while (s.tick - p < 1900) {
            s.step();
            open = open && s.go.gateOpen && s.go.waiting;
            black = black || s.level != 0.0;
        }
        check(open && !black, "the gate stays open to 1900 ms and the view is never blacked by the press");
        check(s.m.entries() == 0 && !s.m.run().published && s.m.phase() == ef::Phase::kIdle, "no entry has begun and the camera move is not published: the camera is not moved by the press");
        uint64_t closedAt = 0;
        for (int i = 0; i < 40 && !closedAt; ++i) {
            s.step();
            black = black || s.level != 0.0;
            if (!s.go.gateOpen) closedAt = s.tick - PSim::kDt - p;
        }
        check(closedAt > 2000 && closedAt <= 2000 + 2 * PSim::kDt, "the gate closes kPressWaitMs (2000 ms) after the press and no sooner");
        s.run(300);
        check(!black && s.level == 0.0 && !s.go.waiting && s.m.phase() == ef::Phase::kIdle && s.m.entries() == 0 && !s.go.gateOpen, "... the press did nothing: no black at any point, idle, no entry, no view edge and the gate closed");
    }
    {   // the press with no camera move wanted does not black the view
        PSim s;
        s.moving = false;
        s.run(50);
        s.pressNow();
        s.run(300);
        check(s.level == 0.0 && s.m.phase() == ef::Phase::kIdle && s.go.waiting, "with no camera move to wait for the press does not black the view (the gate still waits for the sheet)");
    }
    {   // the sheet path: nothing from the press to the sheet, then the hold from the sheet edge to applied plus the margin, the fade-in; the cap counts from the sheet
        PSim s;
        s.run(100);
        const uint64_t p = s.pressNow();
        double minLevel = 1.0;
        bool blackBeforeEdge = false;
        while (s.tick - p < 310) {   // from the press to the sheet: the game's own fade, EDVR adds nothing
            s.step();
            blackBeforeEdge = blackBeforeEdge || s.level != 0.0;
        }
        check(!blackBeforeEdge, "from the press to the sheet draw EDVR's level stays 0");
        const uint64_t sheetTick = s.tick;
        s.sheet = sheetTick;
        s.publishNext = true;
        s.nextSerial = 7;
        s.step();
        check(s.go.on && s.go.sheetEdge && !s.go.noPress && s.ev.started && s.level == 1.0 && s.m.run().entry == 1 && s.m.run().pressed && s.m.run().viaSheet, "the first gunner sheet draw in the window raises the view; the entry follows the press");
        check(s.m.run().startMs == static_cast<double>(sheetTick) && s.m.run().pressMs == static_cast<double>(p), "the run starts at the sheet edge (the cap's zero), and remembers the press");
        check(s.ev.published && s.m.run().published && s.m.run().serial == 7, "the camera move is published at the edge and is the entry's");
        s.applied = 4;
        for (int i = 0; i < 10; ++i) {
            s.sheet = s.tick;
            s.status = s.tick >= p + 422;
            s.step();
            minLevel = std::fmin(minLevel, s.level);
        }
        check(s.m.phase() == ef::Phase::kHold, "an older applied serial does not end the hold");
        s.applied = 8;
        s.sheet = s.tick;
        s.step();
        const double appliedMs = s.m.run().appliedMs;
        check(s.ev.appliedSeen, "the runtime reporting the move applied is seen");
        while (s.m.phase() == ef::Phase::kHold) {
            s.sheet = s.tick;
            s.status = s.tick >= p + 422;
            s.step();
            minLevel = std::fmin(minLevel, s.level);
        }
        check(minLevel == 1.0 && s.m.run().settle == ef::Settle::kApplied && s.m.run().settleMs >= appliedMs + ef::kSettleMarginMs, "the black holds from the edge to the applied serial plus the margin");
        while (s.m.phase() == ef::Phase::kFadeIn) {
            s.sheet = s.tick;
            s.status = true;
            s.step();
        }
        check(s.level == 0.0 && s.m.run().fadeEnded && s.m.run().endMs - s.m.run().settleMs >= ef::kFadeInMs && s.m.run().endMs - s.m.run().settleMs <= ef::kFadeInMs + 2.0 * PSim::kDt, "... then the existing 200 ms fade-in to 0");
        check(s.m.run().sheetLagMs == static_cast<double>(sheetTick - p) && s.m.run().statusLagMs == 430.0, "the run carries the press-to-sheet time (310 ms) and the press-to-Status.json time (430 ms)");
        char line[400];
        ef::formatPressEntry(line, sizeof(line), s.m.run());
        check(has(line, "srv turret entry fade: entry 1: press at frame 110; sheet +310 ms after the press; Status.json +430 ms after the press"), "the entry's press line carries the press frame, the sheet time and the Status.json time");
        // a press while the view is on does nothing in this change
        const double before = s.level;
        s.press = true;
        s.step();
        check(s.go.event == sl::GateEvent::kNone && s.level == before && s.m.entries() == 1, "a press while the view is on (leaving the turret) does nothing");
    }
    {   // the cap counts from the sheet edge, not from the press: a signal that never comes ends the hold 800 ms after the sheet
        PSim s;
        s.run(100);
        const uint64_t p = s.pressNow();
        s.run(300);
        s.sheet = s.tick;
        s.publishNext = true;
        s.nextSerial = 9;
        s.step();
        while (s.m.phase() == ef::Phase::kHold) {
            s.sheet = s.tick;
            s.step();
        }
        const ef::Run& r = s.m.run();
        check(r.settle == ef::Settle::kCap && r.capFired, "a runtime that never reports the move applied ends the hold at the cap");
        check(r.settleMs - r.startMs >= ef::kCapMs - ef::kFadeInMs && r.settleMs - r.startMs <= ef::kCapMs - ef::kFadeInMs + 2.0 * PSim::kDt, "... 800 ms after the sheet edge (the existing whole-run cap of 1000 ms counts from the sheet)");
        check(r.settleMs - static_cast<double>(p) > ef::kCapMs, "... which is more than 1000 ms after the press, so the cap is not counted from the press");
    }
    {   // presses in any other state do nothing
        struct Case { const char* name; bool scorpion, seat, gui0; };
        const Case cases[] = {{"the journal does not call the vehicle the Scorpion", false, true, true}, {"the seat is not the SRV", true, false, true}, {"GuiFocus is above 0 (a panel is open)", true, true, false}};
        for (const Case& c : cases) {
            PSim s;
            s.run(100);
            s.scorpion = c.scorpion;
            s.seat = c.seat;
            s.gui0 = c.gui0;
            s.pressNow();
            check(s.go.event != sl::GateEvent::kCounted && !s.go.waiting && !s.go.gateOpen && s.level == 0.0, c.name);
            s.run(400);
            check(s.level == 0.0 && !s.go.gateOpen && s.m.phase() == ef::Phase::kIdle, "... and nothing follows: no black, the gate stays closed");
        }
    }
    {   // Status.json reporting the turret without a sheet ends the wait and raises the view
        PSim s;
        s.run(100);
        s.pressNow();
        s.run(300);
        s.status = true;
        s.publishNext = true;
        s.nextSerial = 3;
        s.step();
        check(s.go.on && !s.go.sheetEdge && !s.go.noPress && s.ev.started && s.level == 1.0 && !s.go.waiting && s.m.run().pressed, "Status.json on without a sheet ends the wait and raises the view; the press stands");
        check(s.m.run().sheetLagMs < 0.0 && s.m.run().statusLagMs == 310.0 && s.go.fallback == sl::FallbackWhy::kNone, "... the run has no sheet time, Status.json's time (310 ms), and no fallback");
        char line[400];
        ef::formatPressEntry(line, sizeof(line), s.m.run());
        check(has(line, "entry 1: press at frame 110; no sheet draw seen; Status.json +310 ms after the press") && noReadPhrase(line), "the press line says no sheet draw was seen");
    }
    {   // a press seen and not counted precedes a Status.json raise: not blind. The same raise 2.5 s after it is.
        PSim s;
        s.run(100);
        s.gui0 = false;
        s.pressNow();
        s.gui0 = true;
        s.run(500);
        s.status = true;
        s.step();
        check(s.go.on && s.go.noPress && s.go.event != sl::GateEvent::kFellBack && s.go.fallback == sl::FallbackWhy::kNone, "a view raised 500 ms after a press the watcher saw is not a blind watcher");
        PSim t;
        t.run(100);
        t.gui0 = false;
        t.pressNow();
        t.gui0 = true;
        t.run(sl::kBlindWindowMs + 500);
        t.status = true;
        t.step();
        check(t.go.event == sl::GateEvent::kFellBack && t.go.fallback == sl::FallbackWhy::kBlind, "... and the same raise kBlindWindowMs plus 500 ms after it is");
    }
    {   // a slow entry: the counted press times out at kPressWaitMs, and the view that arrives after it still finds the press recent
        static_assert(sl::kBlindWindowMs > sl::kPressWaitMs, "the blind window outlasts the press window");
        check(sl::kBlindWindowMs == 5000, "kBlindWindowMs is 5000 ms");
        PSim s;
        s.run(100);
        s.pressNow();
        s.run(sl::kPressWaitMs + 200);   // the window ends with no view
        check(!s.go.waiting && !s.go.gateOpen && s.m.entries() == 0, "a press whose view has not come in kPressWaitMs times out first");
        s.run(800);                      // the view arrives 3 s after the press
        s.status = true;
        s.step();
        check(s.go.on && s.go.noPress && s.go.event != sl::GateEvent::kFellBack && s.go.fallback == sl::FallbackWhy::kNone, "... and a view 3 s after the press is not a blind watcher");
        s.run(100);
        check(s.go.on && s.go.fallback == sl::FallbackWhy::kNone, "... and the watcher stays trusted afterwards");
        PSim n;   // no press at all
        n.run(100);
        n.status = true;
        n.step();
        check(n.go.event == sl::GateEvent::kFellBack && n.go.fallback == sl::FallbackWhy::kBlind, "a view with no press ever seen is still a blind watcher");
    }
    {   // the blind fallback trips, behaves as a4ebcbec, and resets on a bindings change
        PSim s;
        s.run(100);
        s.status = true;
        s.step();   // the view goes on by Status.json with no press ever seen
        check(s.go.event == sl::GateEvent::kFellBack && s.go.fallback == sl::FallbackWhy::kBlind && s.go.on && s.go.noPress && s.ev.started, "a view that goes on by Status.json with no press seen trips the fallback (blind watcher), and the entry still holds");
        check(s.m.run().noPress && s.m.run().fallback == 2 && !s.m.run().pressed, "... the run says no press was seen and why");
        char line[400];
        ef::formatPressEntry(line, sizeof(line), s.m.run());
        check(has(line, "entry 1: no press was seen (the press watcher saw nothing before the view went on: fallback); the view was raised by Status.json") && noReadPhrase(line), "the press line says no press was seen, the fallback and what raised the view");
        s.status = false;
        s.run(400);
        check(!s.go.on && s.go.gateOpen && !s.go.waiting, "in the fallback the gate is open while the Scorpion's view is off, with no press");
        s.sheet = s.tick;
        s.step();
        check(s.go.on && s.go.sheetEdge && s.go.noPress && s.ev.started && s.level == 1.0 && s.m.run().entry == 2 && s.m.run().viaSheet, "... and the sheet raises the view unfiltered, the hold starting at the sheet (a4ebcbec)");
        ef::formatPressEntry(line, sizeof(line), s.m.run());
        check(has(line, "entry 2: no press was seen") && has(line, "raised by the gunner sheet's draw"), "... the line says the sheet raised it");
        s.run(1100);   // the sheet stops: the latch expires and the view goes off
        check(!s.go.on, "the sheet stopping turns the latch off after a second");
        s.epoch = 2;
        s.step();
        check(s.go.event == sl::GateEvent::kFallbackOff && s.go.fallback == sl::FallbackWhy::kNone && !s.go.gateOpen, "a change of bindings resets the fallback: the gate is closed again");
        s.pressNow();
        check(s.go.event == sl::GateEvent::kCounted && s.go.waiting, "... and a press counts again");
    }
    {   // no observable slot: fallback from the first step, a press does nothing
        PSim s;
        s.observable = false;
        s.step();
        check(s.go.event == sl::GateEvent::kFellBack && s.go.fallback == sl::FallbackWhy::kUnobservable && s.go.gateOpen, "no observable slot (a mouse button past the fifth, an axis, unbound, no .binds): fallback at once, the gate open in the Scorpion");
        s.run(50);
        s.pressNow();
        check(s.go.event != sl::GateEvent::kCounted && !s.go.waiting && s.level == 0.0, "... and a press (which cannot be trusted) starts nothing");
        s.sheet = s.tick;
        s.step();
        check(s.go.on && s.go.sheetEdge && s.go.noPress && s.m.run().fallback == 1, "... the sheet raises the view and holds from there");
        char line[400];
        ef::formatPressEntry(line, sizeof(line), s.m.run());
        check(has(line, "the press watcher cannot see the key: fallback") && noReadPhrase(line), "... the line says the key cannot be seen");
        s.observable = true;
        s.epoch = 2;
        s.status = false;
        s.run(1100);
        s.step();
        check(s.go.fallback == sl::FallbackWhy::kNone, "a re-read that finds the key observable ends the fallback");
    }
    {   // a sheet draw from before the press does not raise the view (the window filters it), and a press after a view that just ended
        PSim s;
        s.run(100);
        s.sheet = s.tick - 5;   // a stale draw, before any press
        s.run(50);
        check(!s.go.on, "a sheet draw with no press does not raise the view while the press is trusted");
        const uint64_t p = s.pressNow();
        s.step();
        check(!s.go.on && p > s.sheet, "... and a draw from before the press still does not");
    }
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
    check(has(st, "g_sheetLatch.step(gateIn)") && has(st, "gateIn.statusOn = turret;") && has(st, "gateIn.scorpion = scorpion;") && has(st, "const bool on = gate.on;") &&
              has(st, "srv turret view: Status.json agreed %llu ms after the sheet") && has(st, "(the gunner sheet drew; Status.json not yet)") && has(st, "the gunner sheet latch expired after"),
          "the frame tick takes `on` from the press gate fed Status.json's bit and the Scorpion, and says the raise, the agreement and the expiry");
    check(has(st, "srvTurretPressPoll()") && has(st, "gateIn.pressEdge = press.pressed;") && has(st, "gateIn.observable = press.observable;") && has(st, "gateIn.epoch = press.epoch;") &&
              has(st, "gateIn.srvSeat = (journalSeatKind() & 4u) != 0;") && has(st, "if (!journalGuiFocus(&guiFocus)) guiFocus = 0;") && has(st, "gateIn.guiFocusZero = guiFocus == 0;") &&
              has(st, "srvTurretEntryFadeNotePress(w);") && has(st, "srv turret press: falling back to the gunner-sheet watch") && has(st, "srv turret press: the bindings changed; the press is trusted again"),
          "the frame tick polls the turret key's press once a frame, tells the gate the Scorpion, the SRV seat, Status.json's GuiFocus (unknown counts as 0) and the bindings epoch, tells the entry fade the gate's words, and logs the fallback");
    check(has(st, "if (st::isGunnerSheetSignature(sig) && (eye == 0 || eye == 1)) detail::g_srvTurretSheetSeenMs.store(GetTickCount64(), std::memory_order_relaxed);") &&
              has(st, "const bool watching = gate.gateOpen && !on;") && has(st, "const bool drawGate = wanted || watching || (on && gate.gateOpen);") && !has(st, "const bool watching = scorpion && (journalSeatKind() & 4u) != 0 && !on;"),
          "the sheet's draw is timed on the draw path, and the draw gate follows the press gate, not the Scorpion's seat alone (the always-open gate is gone)");
    const std::string pm = readText("src/d3d11/srv_turret_press.cpp");
    check(has(pm, "eliteBindsActionSrv(\"ToggleBuggyTurretButton\", &w.bind)") && has(pm, "setGameMirrored(true)") && has(pm, "eliteBindsFingerprint()") && has(pm, "hotkey.read_game_bindings") && has(pm, "CANNOT be seen") &&
              has(pm, "srv turret press: Elite's ToggleBuggyTurretButton in %s"),
          "the press watcher reads ToggleBuggyTurretButton through the shared action parser, watches each slot as a game-mirrored hotkey, re-reads on a bindings change and says which slots are watched and which cannot be seen");
    const std::string eb = readText("src/d3d11/elite_binds.cpp");
    check(has(eb, "return eliteBindsActionIn(\"HMDReset\", text, len, out);") && has(eb, "return eliteBindsActionDir(\"HMDReset\", dirC, out);") && !has(eb, "<HMDReset>\") != std::string::npos"),
          "HMD reset's functions are the shared action parser with the name fixed (one parser, not a copy)");
    const std::string vsc = readText("src/d3d11/vscreen.cpp");
    const size_t gun = vsc.find("return srvTurretGunnerBegin(self, vs, kind, indexCount, srvTurretDrawEye(s));");
    const size_t watchOnly = vsc.find("if (srvTurretWatchOnly()) return false;");
    const size_t famVs = vsc.find("if (!srv_turret::isFamilyVs(vs)) return false;");
    check(gun != std::string::npos && watchOnly != std::string::npos && famVs != std::string::npos && gun < watchOnly && watchOnly < famVs,
          "vscreen.cpp reaches the gunner sheet first, then leaves every other draw to the game while only the sheet is watched, before the HUD families");
    const std::string math = readText("src/d3d11/srv_turret_math.h");
    check(has(math, "if (indexCount != kGunnerSheetIndices && indexCount != kGunnerCardsIndices) return -1;"), "gunnerSignature tests the index count before the shader's hash");
    const std::string mod = readText("src/d3d11/srv_turret_entry_fade.cpp");
    check(has(mod, "constexpr size_t kPairs = 384;"), "the luma window keeps 384 pairs (a 1000 ms lead, the run and the report delay at 144 Hz)");
    check(has(mod, "lumaProbeBurst(inSrv)") && has(mod, "journalWatchSetEagerStatusSrv(inSrv)") && has(mod, "np.inputVersion >= 3") && has(mod, "ef::formatSkipped") && has(mod, "ef::formatRun"),
          "the module asks for the frame-by-frame luma and the 100 ms Status.json in the Scorpion, reads the applied word only from input version 3, and writes both the run and the skipped line");
    check(has(mod, "if (!vr) {") && has(mod, "g_machine.reset();"), "the module stands down outside VR (the comfort fade is the runtime's)");
    const std::string luma = readText("src/d3d11/luma_probe.cpp");
    check(has(luma, "if (stage == 0 && g_burstOn.load(std::memory_order_relaxed)) burstSample(ctx, tex, eye);") && has(luma, "burstPoll(ctx, eye);"),
          "the luma probe samples stage 0 every pass only while the mode is on, and polls it every pass");
    const std::string jw = readText("src/d3d11/journal_watch.cpp");
    check(has(jw, "g_eagerSrv.load(std::memory_order_relaxed)") && has(jw, "void journalWatchSetEagerStatusSrv(bool eager)"), "the journal watcher has the SRV cadence request and reads it in its wake-up");
    const std::string bat = readText("build.bat");
    check(has(bat, "\"src\\d3d11\\srv_turret_entry_fade.cpp\"") && has(bat, "\"src\\d3d11\\srv_turret_press.cpp\""), "build.bat compiles the module and the press watcher into d3d11.dll");
    check(has(mod, "in.pressAgeMs = g_press.ageMs;") && has(mod, "ef::formatPressEntry") && has(mod, "void srvTurretEntryFadeNotePress("),
          "the module feeds the machine the press gate's words and writes the press line of each entry");
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
    latchCases();
    pressCases();
    lineCases();
    wiringCases();
    std::printf("PASS srv_turret_entry_fade_test --self-test: %u checks\n", g_checks);
    return 0;
}
