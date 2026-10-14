// The seat memory's pure logic (src/openxr/seat_memory.h, src/common/seat_context.h) and its file (src/openxr/seat_store.h):
// the decision table (mode x where x window x stored), every transition rule, the start and intro plans, the capture (snap, stillness, give-up),
// the LOCAL-change compensation and the persistence round trip through a real scratch folder.
//   seat_memory_test --self-test    runs everything
//   seat_memory_test --dry-run      writes nothing and touches nothing
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/seat_store.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace edvr;
using namespace edvr::openxr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
}
bool closeTo(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
bool nearPose(const SeatPose& a, const SeatPose& b, double eps = 1e-6) {
    return closeTo(a.x, b.x, eps) && closeTo(a.y, b.y, eps) && closeTo(a.z, b.z, eps) && closeTo(seatWrap(a.yaw - b.yaw), 0, eps);
}

SeatContext ctx(bool keep, SeatWhere w, uint32_t kind = 1, uint32_t transitions = 0, bool camera = false, bool published = true) {
    SeatContext c;
    c.published = published;
    c.keep = keep;
    c.gameplay = w != SeatWhere::Unknown;
    if (w == SeatWhere::Aboard) { c.seatedKnown = true; c.seated = true; c.onFootKnown = true; c.onFoot = false; c.seatKind = kind; }
    if (w == SeatWhere::OnFoot) { c.seatedKnown = true; c.seated = false; c.onFootKnown = true; c.onFoot = true; }
    c.cameraOpen = camera;
    c.transitions = transitions;
    return c;
}

// ---- the cross-DLL word
void contextWord() {
    check(!decodeSeatContext(0).published, "a fresh mapping (0) reads as nobody has written");
    check(decodeSeatContext(0).keep == false && decodeSeatContext(0).seatKind == 0, "0 carries nothing else either");
    SeatContext c = ctx(true, SeatWhere::Aboard, 0x15, 0x7FFFF, true);
    const SeatContext d = decodeSeatContext(encodeSeatContext(c));
    check(d.published && d.keep && d.gameplay && d.seatedKnown && d.seated && d.onFootKnown && !d.onFoot && d.cameraOpen &&
          d.seatKind == 0x15 && d.transitions == 0x7FFFF, "every field survives the word");
    c = ctx(false, SeatWhere::OnFoot, 0, 5);
    const SeatContext e = decodeSeatContext(encodeSeatContext(c));
    check(e.published && !e.keep && e.onFoot && !e.seated && e.transitions == 5, "game mode on foot round-trips");
    check(encodeSeatContext(SeatContext{}) == 1u, "an empty but published context is exactly bit 0");
    SeatContext big; big.published = true; big.transitions = 0xFFFFFFFFu; big.seatKind = 0xFFFFFFFFu;
    const SeatContext f = decodeSeatContext(encodeSeatContext(big));
    check(f.transitions == kSeatTransitionMask && f.seatKind == 0x1F, "counts wrap into their fields and never spill");
    check(SeatMemory::whereOf(ctx(true, SeatWhere::Aboard)) == SeatWhere::Aboard, "seated is aboard");
    SeatContext both = ctx(true, SeatWhere::Aboard); both.onFoot = true;
    check(SeatMemory::whereOf(both) == SeatWhere::Aboard, "a sample that says both is read as seated");
    SeatContext unknown = ctx(true, SeatWhere::Unknown);
    check(SeatMemory::whereOf(unknown) == SeatWhere::Unknown, "no flags is unknown (a menu)");
}

// ---- the decision table: mode x where x window x stored
void decisionTable() {
    const SeatPose seat{0.4, 0.1, -1.1, 0.5};
    for (int mode = 0; mode < 3; ++mode)            // 0 unpublished, 1 game, 2 keep
    for (int w = 0; w < 3; ++w)                     // unknown, on foot, aboard
    for (int inWin = 0; inWin < 2; ++inWin)
    for (int stored = 0; stored < 2; ++stored) {
        SeatMemory m;
        if (stored) m.setSeat(seat);
        const SeatWhere where = w == 0 ? SeatWhere::Unknown : w == 1 ? SeatWhere::OnFoot : SeatWhere::Aboard;
        // Get to `where` with the window in the wanted state: baseline unknown, then the state; a window opens only on the aboard flip.
        m.onContext(ctx(mode == 2, SeatWhere::Unknown, 0, 0, false, mode != 0), 1000);
        m.onContext(ctx(mode == 2, where, 1, 0, false, mode != 0), 2000);
        const uint64_t at = inWin ? 2000 + 9000 : 2000 + 10001;   // inside (9 s after the flip) or just past 10 s
        SeatResetDecision expect;
        if (mode != 2) expect = SeatResetDecision::GameMode;
        else if (where == SeatWhere::Unknown) expect = SeatResetDecision::PassThrough;    // a menu or a load; on foot is a seat (kOnFootIsSeated)
        else if (inWin) expect = stored ? SeatResetDecision::ReplaceWithSeat : SeatResetDecision::HonourNoSeat;
        else expect = SeatResetDecision::HonourStore;
        const SeatResetDecision got = m.classifyReset(at);
        char text[160];
        std::snprintf(text, sizeof text, "table: mode=%d where=%d window=%d stored=%d -> %s (got %s)", mode, w, inWin, stored,
                      seatResetDecisionName(expect), seatResetDecisionName(got));
        check(got == expect, text);
    }
    check(kOnFootIsSeated && onFootIsSeated(), "the rule on foot is: everyone sits");
    check(seatHolds(SeatWhere::Aboard) && seatHolds(SeatWhere::OnFoot) && !seatHolds(SeatWhere::Unknown), "the seat applies aboard and on foot, not in a menu");
    // The window is ten seconds from the flip, inclusive, aboard or on foot.
    SeatMemory m; m.setSeat(seat);
    m.onContext(ctx(true, SeatWhere::OnFoot), 0);
    m.onContext(ctx(true, SeatWhere::Aboard), 5000);
    check(m.classifyReset(5000) == SeatResetDecision::ReplaceWithSeat, "a reset at the flip itself is inside the window");
    check(m.classifyReset(15000) == SeatResetDecision::ReplaceWithSeat, "ten seconds after the flip is still inside");
    check(m.classifyReset(15001) == SeatResetDecision::HonourStore, "just past ten seconds is the player's own recentre");
    check(m.classifyReset(4000) == SeatResetDecision::HonourStore, "a clock reading before the window opened is not inside it");
}

// ---- noteReset: what the memory remembers of each outcome
void noteResets() {
    const SeatPose seat{0.4, 0.1, -1.1, 0.5}, fresh{0.1, 0.2, 0.3, -0.4};
    SeatMemory m; m.setSeat(seat);
    m.noteOrigin(true);
    check(!m.noteReset(SeatResetDecision::PassThrough, fresh) && !m.originIsSeat(), "a pass-through moves the origin off the seat and stores nothing");
    check(nearPose(m.seat(), seat), "...and the stored seat is untouched");
    m.noteOrigin(true);
    check(!m.noteReset(SeatResetDecision::GameMode, fresh) && !m.originIsSeat(), "game mode stores nothing");
    check(!m.noteReset(SeatResetDecision::HonourNoSeat, fresh) && nearPose(m.seat(), seat), "an automatic reset with nothing stored stores nothing");
    check(!m.noteReset(SeatResetDecision::ReplaceWithSeat, seat) && m.originIsSeat(), "a replacement leaves the origin on the seat, nothing to write");
    check(!m.noteReset(SeatResetDecision::HonourStore, fresh, 100) && nearPose(m.seat(), fresh) && m.originIsSeat() && m.storePending(),
          "the player's own recentre over a stored seat is held, not written");
    uint64_t waited = 0;
    check(!m.takeCommit(5099, waited) && m.takeCommit(5100, waited) && waited == 5000 && !m.storePending() && !m.takeCommit(9000, waited),
          "...and committed once, five seconds later");
    check(SeatMemory::kRetroMs == 5000, "the backward window is five seconds");
    SeatMemory none;
    check(none.noteReset(SeatResetDecision::HonourStore, fresh) && none.has() && nearPose(none.seat(), fresh), "the first recentre aboard becomes the seat");
    none.noteOrigin(true);
    check(!none.noteReset(SeatResetDecision::HonourNoSeat, fresh), "an automatic reset never overwrites a stored seat");
    SeatMemory gone; gone.noteOrigin(true);
    check(!gone.originIsSeat(), "the origin cannot be the seat when there is none");
}

// ---- transitions: when the stored seat goes back
void transitions() {
    const SeatPose seat{0.4, 0.1, -1.1, 0.5};
    SeatMemory m; m.setSeat(seat);
    SeatEvents ev = m.onContext(ctx(true, SeatWhere::Unknown), 100);
    check(!ev.windowOpened && !ev.applyWanted, "the first sight in a menu does nothing");
    ev = m.onContext(ctx(true, SeatWhere::OnFoot), 200);
    check(ev.windowOpened && ev.applyWanted && std::string(ev.why) == "on_foot" && m.where() == SeatWhere::OnFoot,
          "on foot, from a menu, is a seat transition: window and apply, like boarding");
    check(m.inWindow(200) && m.classifyReset(250) == SeatResetDecision::ReplaceWithSeat, "...and its resets are replaced");
    m.applyDone(); m.noteOrigin(true);
    m.noteReset(SeatResetDecision::PassThrough, SeatPose{});      // walking about, the origin has been moved off the seat
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 300);
    check(ev.windowOpened && ev.applyWanted && std::string(ev.why) == "boarded" && m.applyPending(), "boarding from foot applies the seat");
    check(m.inWindow(300), "...and opens the window");
    m.applyDone(); m.noteOrigin(true);
    check(!m.applyPending(), "applyDone settles the apply");
    // Disembarking is a transition like boarding: a window, and the seat back if the game's reset moved it.
    ev = m.onContext(ctx(true, SeatWhere::OnFoot, 0, 1), 350);
    check(ev.windowOpened && !ev.applyWanted && std::string(ev.why) == "disembarked" && m.inWindow(350),
          "disembarking opens the window (the origin is already the seat: nothing to put back)");
    m.noteReset(SeatResetDecision::PassThrough, SeatPose{});
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 360);
    check(ev.applyWanted && std::string(ev.why) == "boarded", "boarding again puts the seat back");
    m.applyDone(); m.noteOrigin(true);
    m.noteReset(SeatResetDecision::PassThrough, SeatPose{});
    ev = m.onContext(ctx(true, SeatWhere::OnFoot, 0, 1), 370);
    check(ev.applyWanted && std::string(ev.why) == "disembarked" && m.applyPending(), "disembarking with the origin off the seat puts it back");
    m.applyDone(); m.noteOrigin(true);
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 380);
    m.applyDone(); m.noteOrigin(true);
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2), 400);
    check(ev.windowOpened && !ev.applyWanted && std::string(ev.why) == "journal_event", "a journal event aboard opens a window and applies nothing");
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 4, 2), 500);
    check(!ev.applyWanted, "a change of cockpit with the origin already on the seat applies nothing (no needless invalidation)");
    m.noteReset(SeatResetDecision::PassThrough, SeatPose{});      // the game moved it elsewhere
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2), 600);
    check(ev.windowOpened && ev.applyWanted && std::string(ev.why) == "cockpit_changed", "ship to SRV or back puts the seat back");
    check(m.applyPending(), "...pending until it lands");
    // An apply that did not land is retried: still pending on later contexts.
    m.onContext(ctx(true, SeatWhere::Aboard, 1, 2), 700);
    check(m.applyPending(), "an apply that has not landed stays pending");
    m.applyDone(); m.noteOrigin(true);
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2, true), 800);
    check(ev.windowOpened && !ev.applyWanted && std::string(ev.why) == "camera_opened", "Explorer Cam opening opens a window");
    m.noteReset(SeatResetDecision::PassThrough, SeatPose{});
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2, false), 900);
    check(ev.applyWanted && std::string(ev.why) == "camera_closed", "Explorer Cam closing puts the seat back");
    m.applyDone(); m.noteOrigin(true);
    ev = m.onContext(ctx(true, SeatWhere::Unknown, 0, 3), 1000);
    check(!m.inWindow(1000) && !m.applyPending() && std::string(ev.why) == "unknown_state", "into a menu or a load the window closes and a pending apply is cancelled");
    m.noteReset(SeatResetDecision::PassThrough, SeatPose{});
    m.onContext(ctx(true, SeatWhere::Aboard, 1, 3), 1100);
    check(m.applyPending(), "...and coming back re-arms");
    m.onContext(ctx(true, SeatWhere::Unknown, 0, 3), 1200);
    check(!m.applyPending(), "a pending apply does not survive a load");
    // Explorer Cam on foot is the same as aboard
    SeatMemory cam; cam.setSeat(seat); cam.onContext(ctx(true, SeatWhere::OnFoot), 0); cam.applyDone(); cam.noteOrigin(true);
    ev = cam.onContext(ctx(true, SeatWhere::OnFoot, 0, 0, true), 100);
    check(ev.windowOpened && !ev.applyWanted && std::string(ev.why) == "camera_opened", "Explorer Cam opening on foot opens a window");
    cam.noteReset(SeatResetDecision::PassThrough, SeatPose{});
    ev = cam.onContext(ctx(true, SeatWhere::OnFoot, 0, 0, false), 200);
    check(ev.applyWanted && std::string(ev.why) == "camera_closed", "...and closing it puts the seat back");
    // seat kind only means something aboard
    SeatMemory kind; kind.setSeat(seat); kind.onContext(ctx(true, SeatWhere::OnFoot, 0), 0); kind.applyDone(); kind.noteOrigin(true);
    ev = kind.onContext(ctx(true, SeatWhere::OnFoot, 7), 10);
    check(!ev.windowOpened, "a seat-kind word on foot is not a change of cockpit");
    // No seat stored: the window opens, the apply is "no seat".
    SeatMemory none;
    none.onContext(ctx(true, SeatWhere::OnFoot), 0);
    ev = none.onContext(ctx(true, SeatWhere::Aboard), 10);
    check(ev.windowOpened && !ev.applyWanted && ev.noSeat && !none.applyPending(), "nothing stored: boarding opens the window and says so");
    // A game that launches straight into a cockpit
    SeatMemory cold; cold.setSeat(seat);
    ev = cold.onContext(ctx(true, SeatWhere::Aboard), 5);
    check(ev.applyWanted && std::string(ev.why) == "seated_at_start", "first sight aboard applies");
    // unpublished context does nothing and is not keep
    SeatMemory off;
    ev = off.onContext(SeatContext{}, 5);
    check(!off.keep() && !ev.windowOpened && !off.context().published, "an unpublished word is game mode");
    // keep flips live
    SeatMemory live; live.setSeat(seat);
    live.onContext(ctx(true, SeatWhere::Aboard), 0);
    live.onContext(ctx(false, SeatWhere::Aboard), 50000);
    check(!live.keep() && live.classifyReset(50001) == SeatResetDecision::GameMode, "switching the key to game takes effect on the next context");
}

// ---- start and intro plans
void plans() {
    check(seatStartPlan(false, true) == SeatStartPlan::TodaysCentre && seatStartPlan(false, false) == SeatStartPlan::TodaysCentre, "game mode keeps today's start");
    check(seatStartPlan(true, true) == SeatStartPlan::ApplySeat, "keep with a seat applies it at the start");
    check(seatStartPlan(true, false) == SeatStartPlan::CaptureLate, "keep with none is today's centre now, capture at the splash");
    check(seatIntroPlan(false, true, true) == SeatIntroPlan::TodaysRecentre && seatIntroPlan(false, false, false) == SeatIntroPlan::TodaysRecentre,
          "game mode keeps today's intro recentre");
    check(seatIntroPlan(true, true, true) == SeatIntroPlan::SkipSeatInPlace, "the seat is already the origin: the intro asks for nothing");
    check(seatIntroPlan(true, true, false) == SeatIntroPlan::ApplySeat, "the seat is stored but the origin is not on it: apply");
    check(seatIntroPlan(true, false, false) == SeatIntroPlan::Capture, "nothing stored: capture");
    check(std::string(seatStartPlanName(SeatStartPlan::ApplySeat)) == "apply_seat" && std::string(seatIntroPlanName(SeatIntroPlan::Capture)) == "capture",
          "plan names are the log's words");
}

// ---- what to remember
void picking() {
    SeatPick p = pickSeatFromHead(SeatPose{0.1, 0.0, 0.1, seatDegToRad(5)});
    check(p.snapped && nearPose(p.seat, SeatPose{}), "a head near LOCAL's origin snaps to it exactly");
    p = pickSeatFromHead(SeatPose{0.0, 0.0, 0.249, 0});
    check(p.snapped, "24.9 cm snaps");
    p = pickSeatFromHead(SeatPose{0.0, 0.0, 0.251, 0});
    check(!p.snapped && closeTo(p.seat.z, 0.251), "25.1 cm does not, and keeps the head");
    p = pickSeatFromHead(SeatPose{0.0, 0.0, 0.0, seatDegToRad(19.9)});
    check(p.snapped, "19.9 degrees snaps");
    p = pickSeatFromHead(SeatPose{0.0, 0.0, 0.0, seatDegToRad(20.1)});
    check(!p.snapped, "20.1 degrees does not");
    p = pickSeatFromHead(SeatPose{0.0, 0.0, 0.0, seatDegToRad(-20.1)});
    check(!p.snapped, "-20.1 degrees does not");
    p = pickSeatFromHead(SeatPose{0.0, 0.45, 0.0, 0});
    check(!p.snapped, "45 cm over LOCAL's origin is not near it (SteamVR's LOCAL sits that far under a seated head)");
    p = pickSeatFromHead(SeatPose{-0.53, 0.45, -1.11, seatDegToRad(-28)});
    check(!p.snapped && closeTo(p.seat.x, -0.53) && closeTo(p.seat.z, -1.11), "the measured Sean seat (1.1 m, 28 degrees off) is kept as measured");
    p = pickSeatFromHead(SeatPose{0, 0, 0, seatDegToRad(350)});
    check(p.snapped, "a heading of 350 degrees is -10 and snaps");
}

SeatPose head(double x, double y, double z, double yawDeg) { return SeatPose{x, y, z, seatWrap(seatDegToRad(yawDeg))}; }

// ---- "worn and still"
void stillness() {
    SeatCapture c;
    check(c.state() == SeatCapture::State::Idle, "idle until begun");
    c.begin(1000);
    // 90 Hz of a head that tremors by a few millimetres
    SeatCapture::State s = SeatCapture::State::Sampling;
    unsigned doneAt = 0;
    for (unsigned i = 0; i < 90 && s == SeatCapture::State::Sampling; ++i) {
        const double jitter = (i % 2 ? 0.002 : -0.002);
        s = c.add(1000 + i * 11, head(-0.53 + jitter, 0.45, -1.11 - jitter, -28 + jitter * 100));
        if (s == SeatCapture::State::Done) doneAt = i;
    }
    check(s == SeatCapture::State::Done, "a head still to a few millimetres is captured");
    check(doneAt >= SeatStillness::kMinSamples - 1, "...not before enough samples");
    check(c.last().spanMs >= SeatStillness::kMinSpanMs, "...nor before 0.4 s");
    check(closeTo(c.result().x, -0.53, 0.003) && closeTo(c.result().z, -1.11, 0.003), "the result is the window's mean");
    check(closeTo(seatRadToDeg(c.result().yaw), -28, 0.5), "...with the circular mean heading");
    check(c.add(9999, head(5, 5, 5, 0)) == SeatCapture::State::Done, "a finished capture stays finished");

    // walking about never settles; gives up at four seconds
    SeatCapture w; w.begin(0);
    SeatCapture::State ws = SeatCapture::State::Sampling;
    for (unsigned i = 0; i < 1000 && ws == SeatCapture::State::Sampling; ++i)
        ws = w.add(i * 11, head(0.3 * std::sin(i * 0.05), 0.45, 0.3 * std::cos(i * 0.05), 3.0 * i));
    check(ws == SeatCapture::State::GaveUp, "a head that keeps moving gives up");
    check(w.elapsed(4000) >= SeatCapture::kTimeoutMs, "...after the timeout");
    check(w.last().spreadCm > SeatStillness::kMaxSpreadM * 100.0, "...and the statistics say why");

    // yaw alone breaks stillness
    SeatCapture y; y.begin(0);
    SeatCapture::State ys = SeatCapture::State::Sampling;
    for (unsigned i = 0; i < 200 && ys == SeatCapture::State::Sampling; ++i)
        ys = y.add(i * 11, head(0, 0.45, 0, (i % 2) ? 8.0 : -8.0));
    check(ys != SeatCapture::State::Done, "a head swinging 8 degrees each way is not still");

    // too few samples inside the span
    SeatStillness win;
    for (unsigned i = 0; i < 10; ++i) win.add(i * 50, head(0, 0.45, 0, 0));
    check(!win.stats().still, "ten samples over half a second is too few");
    win.add(600, head(0, 0.45, 0, 0));
    SeatStillness sparse;
    for (unsigned i = 0; i < 40; ++i) sparse.add(i * 5, head(0, 0.45, 0, 0));
    check(!sparse.stats().still, "forty samples within 0.2 s is too short a span");
    // the window slides: an old excursion ages out
    SeatStillness slide;
    slide.add(0, head(2, 0.45, 2, 90));
    for (unsigned i = 0; i < 120; ++i) slide.add(100 + i * 11, head(0, 0.45, 0, 0));
    check(slide.stats().still, "an excursion older than the window no longer counts");
    // NaN and a clock that steps back start the window again
    SeatStillness bad;
    for (unsigned i = 0; i < 50; ++i) bad.add(i * 11, head(0, 0.45, 0, 0));
    SeatPose nan = head(0, 0.45, 0, 0); nan.x = NAN;
    bad.add(600, nan);
    check(bad.stats().n == 0, "a NaN sample clears the window");
    for (unsigned i = 0; i < 50; ++i) bad.add(1000 + i * 11, head(0, 0.45, 0, 0));
    bad.add(500, head(0, 0.45, 0, 0));
    check(bad.stats().n == 1, "a clock that steps back clears the window");
}

// ---- the runtime recentred LOCAL: always the player's own, in any state
void runtimeRecentre() {
    const SeatPose seat{-0.53, 0.45, -1.11, seatDegToRad(-28)};
    const SeatPose head{0.2, 0.3, -0.4, seatDegToRad(15)};
    for (int w = 0; w < 3; ++w) {
        SeatMemory m; m.setSeat(seat);
        const SeatWhere where = w == 0 ? SeatWhere::Unknown : w == 1 ? SeatWhere::OnFoot : SeatWhere::Aboard;
        m.onContext(ctx(true, SeatWhere::Unknown), 0);
        m.onContext(ctx(true, where), 100);
        // inside the window the game's reset would be replaced; the runtime's recentre is stored whatever the window says
        check(m.noteRuntimeRecentre(head), "a runtime recentre says write now");
        check(nearPose(m.seat(), head) && m.originIsSeat() && !m.applyPending(), "a runtime recentre stores the sampled head as the seat, in any state, in or out of a window");
    }
    SeatMemory none;
    none.noteRuntimeRecentre(head);
    check(none.has() && nearPose(none.seat(), head), "...even with no seat stored");
    // never held: a held manual recentre is overtaken by the runtime's, and nothing is left pending to be taken back
    SeatMemory held; held.setSeat(seat); held.onContext(ctx(true, SeatWhere::Aboard), 0); held.applyDone(); held.noteOrigin(true);
    held.noteReset(SeatResetDecision::HonourStore, SeatPose{0.1, 0, 0, 0}, 20000);
    check(held.storePending(), "(a manual recentre is held)");
    held.noteRuntimeRecentre(head);
    uint64_t w2 = 0;
    check(!held.storePending() && !held.takeCommit(99999, w2) && nearPose(held.seat(), head), "...the runtime's recentre stores at once and cancels the hold");
    // the head could not be sampled: LOCAL's own origin, applied when a frame allows
    SeatMemory fb; fb.setSeat(seat); fb.onContext(ctx(true, SeatWhere::Aboard), 0); fb.applyDone(); fb.noteOrigin(true);
    fb.storeAndApplyLater(SeatPose{});
    check(nearPose(fb.seat(), SeatPose{}) && fb.applyPending() && !fb.originIsSeat(), "untracked: LOCAL identity is the seat and is pending");
    fb.applyDone(); fb.noteOrigin(true);
    check(fb.originIsSeat() && !fb.applyPending(), "...until it lands");
    SeatMemory off;
    off.storeAndApplyLater(SeatPose{});
    check(off.has() && off.applyPending(), "...with nothing stored before");
    // next boarding uses it
    SeatMemory nb; nb.setSeat(seat);
    nb.onContext(ctx(true, SeatWhere::OnFoot), 0); nb.applyDone(); nb.noteOrigin(true);
    nb.noteRuntimeRecentre(head);
    nb.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 5000);
    check(nb.classifyReset(5100) == SeatResetDecision::ReplaceWithSeat && nearPose(nb.seat(), head), "the next boarding's replacement is the runtime-recentred seat");
}

void meter() {
    const SeatMeter m = seatMeter(head(0.3, 0.1, 0.4, 25), head(0, 0, 0, 0));
    check(closeTo(m.offsetCm, 50, 1e-9) && closeTo(m.heightCm, 10, 1e-9) && closeTo(m.yawDeg, 25, 1e-9), "offset, height and heading are read off in centimetres and degrees");
    const SeatMeter z = seatMeter(head(1, 2, 3, 40), head(1, 2, 3, 40));
    check(closeTo(z.offsetCm, 0) && closeTo(z.heightCm, 0) && closeTo(z.yawDeg, 0), "a head on the origin reads zero");
    const SeatMeter w = seatMeter(head(0, 0, 0, 170), head(0, 0, 0, -170));
    check(closeTo(w.yawDeg, -20, 1e-9), "heading differences wrap");
}

// ---- persistence
void bookText() {
    std::vector<SeatRecord> book;
    seatBookPut(book, "pimax-openxr/pimax-crystal", SeatPose{-0.53, 0.0, -1.11, seatDegToRad(-28)});
    seatBookPut(book, "steamvr-openxr/steamvr-openxr-aapvr", SeatPose{-0.53, 0.45, -1.11, seatDegToRad(-28.5)});
    std::vector<SeatRecord> back;
    unsigned skipped = 9;
    check(seatBookParse(seatBookSerialize(book), back, &skipped) && back.size() == 2 && skipped == 0, "a book parses back, nothing skipped");
    SeatPose p;
    check(seatBookFind(back, "pimax-openxr/pimax-crystal", p) && nearPose(p, SeatPose{-0.53, 0.0, -1.11, seatDegToRad(-28)}, 1e-7), "each key finds its own seat");
    check(seatBookFind(back, "steamvr-openxr/steamvr-openxr-aapvr", p) && closeTo(p.y, 0.45, 1e-7), "...the other one too");
    check(!seatBookFind(back, "oculus/quest", p), "an unknown headset has no seat");
    seatBookPut(book, "pimax-openxr/pimax-crystal", SeatPose{1, 2, 3, 0.25});
    check(book.size() == 2 && seatBookFind(book, "pimax-openxr/pimax-crystal", p) && closeTo(p.x, 1), "putting an existing key replaces it");
    seatBookErase(book, "pimax-openxr/pimax-crystal");
    check(book.size() == 1 && !seatBookFind(book, "pimax-openxr/pimax-crystal", p), "erase removes it");
    // hostile and damaged files
    check(!seatBookParse("", back), "an empty file has no header");
    check(!seatBookParse("edvr-seat 2\nk\t1\t2\t3\t4\n", back), "another version is refused, not guessed at");
    check(seatBookParse("edvr-seat 1\n", back) && back.empty(), "a header alone is an empty book");
    check(seatBookParse("edvr-seat 1\r\nk\t1\t2\t3\t4\r\n", back, &skipped) && back.size() == 1 && skipped == 0, "CRLF lines parse");
    check(seatBookParse("edvr-seat 1\nbad\nk\t1\t2\t3\nk2\t1\t2\t3\tx\nk3\tnan\t0\t0\t0\nk4\t1e9\t0\t0\t0\nk5\t1\t2\t3\t4\n", back, &skipped) &&
          back.size() == 1 && back[0].key == "k5" && skipped == 5, "damaged, short, non-numeric, NaN and absurd lines are skipped one by one");
    check(seatBookParse("edvr-seat 1\n\n\nk\t1\t2\t3\t4", back) && back.size() == 1, "blank lines and a missing final newline are fine");
    SeatRecord nasty; nasty.key = std::string("a\tb\nc") + char(1) + "d"; nasty.pose = SeatPose{1, 1, 1, 1};
    std::vector<SeatRecord> nb{nasty};
    check(seatBookParse(seatBookSerialize(nb), back) && back.size() == 1 && back[0].key == "a_b_c_d", "tabs, newlines and control bytes in a key are made safe");
    std::vector<SeatRecord> many;
    for (unsigned i = 0; i < 40; ++i) seatBookPut(many, "k" + std::to_string(i), SeatPose{});
    check(many.size() == kSeatBookMaxRecords, "the book holds at most sixteen headsets (the oldest go)");
    check(seatBookFind(many, "k39", p) && !seatBookFind(many, "k0", p), "...and it is the oldest that go");
    std::string huge = "edvr-seat 1\n";
    for (unsigned i = 0; i < 100; ++i) huge += "key" + std::to_string(i) + "\t1\t2\t3\t4\n";
    check(seatBookParse(huge, back) && back.size() == kSeatBookMaxRecords, "a long file parses to the cap");
    check(seatPoseFinite(SeatPose{}) && !seatPoseFinite(SeatPose{NAN, 0, 0, 0}) && !seatPoseFinite(SeatPose{0, 99, 0, 0}) && !seatPoseFinite(SeatPose{0, 0, 0, 100}), "pose sanity bounds");
}

std::wstring scratchRoot() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[64];
    std::swprintf(name, 64, L"edvr_seat_test_%lu_%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    return std::wstring(tmp) + name;
}
void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring full = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(full); else DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

void files() {
    const std::wstring root = scratchRoot();
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring path = root + L"\\seat.txt";
    std::vector<SeatRecord> book;
    bool existed = true;
    check(seatBookLoadFile(path, book, nullptr, &existed) && book.empty() && !existed, "an absent file is an empty book, not an error");
    seatBookPut(book, "steamvr-openxr/steamvr-openxr-aapvr", SeatPose{-0.53, 0.45, -1.11, seatDegToRad(-28)});
    check(seatBookSaveFile(path, book), "a book saves");
    check(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "...leaving no temp file behind");
    std::vector<SeatRecord> back;
    unsigned skipped = 0;
    check(seatBookLoadFile(path, back, &skipped, &existed) && existed && back.size() == 1, "...and loads back");
    SeatPose p;
    check(seatBookFind(back, "steamvr-openxr/steamvr-openxr-aapvr", p) && nearPose(p, SeatPose{-0.53, 0.45, -1.11, seatDegToRad(-28)}, 1e-7), "...to the same seat");
    seatBookPut(book, "steamvr-openxr/steamvr-openxr-aapvr", SeatPose{0.1, 0.2, 0.3, 0.4});
    check(seatBookSaveFile(path, book) && seatBookLoadFile(path, back) && seatBookFind(back, "steamvr-openxr/steamvr-openxr-aapvr", p) && closeTo(p.x, 0.1),
          "a second save replaces the first");
    // a damaged file
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD put = 0;
    WriteFile(f, "garbage\nmore", 12, &put, nullptr);
    CloseHandle(f);
    check(!seatBookLoadFile(path, back) && back.empty(), "a file with the wrong header fails to load and yields no seat");
    // a directory where the file should be
    const std::wstring dirAsFile = root + L"\\dir.txt";
    CreateDirectoryW(dirAsFile.c_str(), nullptr);
    check(!seatBookLoadFile(dirAsFile, back), "a folder in the file's place is a failed load");
    check(!seatBookSaveFile(root + L"\\no_such_folder\\seat.txt", book), "a save into a folder that does not exist fails cleanly");

    // the mirror folder, resolved the installer's way, against a scratch LOCALAPPDATA
    const std::wstring exeSteam = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64";
    check(seatMirrorDirFor(root, exeSteam, false).empty(), "no mirror folder and no create: nothing, and nothing made");
    check(GetFileAttributesW((root + L"\\EDVR").c_str()) == INVALID_FILE_ATTRIBUTES, "...not even the EDVR folder");
    const std::wstring made = seatMirrorDirFor(root, exeSteam, true);
    check(made == root + L"\\EDVR\\elite-dangerous-odyssey-64-steam" && seatDirExists(made), "create makes <leaf>-steam for a Steam install");
    check(seatMirrorDirFor(root, exeSteam, false) == made, "...and finds it again without create");
    check(seatMirrorDirFor(root, L"D:\\Epic Games\\EliteDangerous\\Products\\elite-dangerous-odyssey-64", false) == made,
          "another store's install of the same leaf finds the one folder that carries the leaf (the installer made it)");
    const std::wstring other = seatMirrorDirFor(root, L"E:\\Games\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64", false);
    check(other == made, "...so does a folder you chose");
    check(seatMirrorDirFor(L"", exeSteam, true).empty() && seatMirrorDirFor(root, L"", true).empty(), "no LOCALAPPDATA or no exe folder: nothing");
    CreateDirectoryW((root + L"\\EDVR\\elite-dangerous-odyssey-64-epic").c_str(), nullptr);
    check(seatMirrorDirFor(root, L"D:\\Epic Games\\EliteDangerous\\Products\\elite-dangerous-odyssey-64", false) == root + L"\\EDVR\\elite-dangerous-odyssey-64-epic",
          "with two folders, the store slug the path implies picks the right one");
    check(seatMirrorDirFor(root, L"E:\\Games\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64", false).empty(),
          "...and with two and no slug, there is no guess");
    // a seat in the mirror folder survives as the real thing would: save, drop the book, load
    const std::wstring seatPath = made + L"\\" + kSeatFileName;
    std::vector<SeatRecord> real;
    seatBookPut(real, "k", SeatPose{1, 2, 3, 0.5});
    check(seatBookSaveFile(seatPath, real) && seatBookLoadFile(seatPath, back) && back.size() == 1, "a seat written into the mirror folder reads back");
    removeTree(root);
    check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "the scratch folder is gone");
}

// ---- a reset outside any window is held for five seconds before it is believed
void retroWindow() {
    const SeatPose seat{-0.53, 0.45, -1.11, seatDegToRad(-28)}, ramp{-0.20, 0.05, -0.40, seatDegToRad(20)};
    // the ramp's reset 3 s before the boarding flip is dropped and the old seat applied
    {
        SeatMemory m; m.setSeat(seat);
        m.onContext(ctx(true, SeatWhere::Unknown), 0);
        m.onContext(ctx(true, SeatWhere::OnFoot, 0, 1), 1000);
        m.applyDone(); m.noteOrigin(true);
        const uint64_t t = 30000;                 // long after the on-foot window
        check(m.classifyReset(t) == SeatResetDecision::HonourStore, "the ramp's reset is outside a window");
        check(!m.noteReset(SeatResetDecision::HonourStore, ramp, t) && nearPose(m.seat(), ramp) && m.storePending() && nearPose(m.previousSeat(), seat),
              "...honoured, held, with the old seat kept beside it");
        uint64_t w = 0;
        check(!m.takeCommit(t + 2999, w), "...not written yet");
        const SeatEvents ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2), t + 3000);
        check(ev.discardedPending && ev.discardAgeMs == 3000, "boarding 3 s later discards it, saying how long it had waited");
        check(nearPose(ev.discardedSeat, ramp) && nearPose(ev.restoredSeat, seat) && nearPose(m.seat(), seat), "...the old seat is back");
        check(ev.applyWanted && m.applyPending() && !m.originIsSeat() && std::string(ev.why) == "boarded", "...and applied, as a transition (the origin is on the ramp pose)");
        check(!m.storePending() && !m.takeCommit(t + 99999, w), "...and nothing is left to commit");
        m.applyDone(); m.noteOrigin(true);
    }
    // a reset 6 s before boarding was committed a second ago
    {
        SeatMemory m; m.setSeat(seat);
        m.onContext(ctx(true, SeatWhere::Unknown), 0);
        m.onContext(ctx(true, SeatWhere::OnFoot, 0, 1), 1000);
        m.applyDone(); m.noteOrigin(true);
        const uint64_t t = 30000;
        m.noteReset(SeatResetDecision::HonourStore, ramp, t);
        uint64_t w = 0;
        check(m.takeCommit(t + 5000, w) && w == 5000, "five seconds on, it commits");
        const SeatEvents ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2), t + 6000);
        check(!ev.discardedPending && nearPose(m.seat(), ramp), "boarding 6 s after is too late to take it back: it is the seat");
        check(ev.windowOpened && !ev.applyWanted, "...and the origin is already on it");
    }
    // aboard, no transition: commits at exactly five seconds, once
    {
        SeatMemory m; m.setSeat(seat);
        m.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 0);
        m.applyDone(); m.noteOrigin(true);
        const uint64_t t = 60000;
        check(m.classifyReset(t) == SeatResetDecision::HonourStore, "aboard long after the window: the player's recentre");
        const SeatPose mine{-0.50, 0.46, -1.05, seatDegToRad(-25)};
        check(!m.noteReset(SeatResetDecision::HonourStore, mine, t) && m.storePending(), "held");
        uint64_t w = 0;
        check(!m.takeCommit(t + 4999, w), "not at 4.999 s");
        check(m.takeCommit(t + 5000, w) && w == 5000 && nearPose(m.seat(), mine) && !m.storePending(), "committed at exactly 5 s, saying how long it waited");
        check(!m.takeCommit(t + 20000, w), "...once");
        const SeatEvents ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2), t + 30000);
        check(!ev.discardedPending && nearPose(m.seat(), mine), "a transition after the commit keeps it");
    }
    // a second recentre inside the wait keeps the first previous seat, and the wait starts again
    {
        SeatMemory m; m.setSeat(seat);
        m.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 0);
        m.applyDone(); m.noteOrigin(true);
        const SeatPose a{0.1, 0.1, 0.1, 0.1}, b{0.2, 0.2, 0.2, 0.2};
        m.noteReset(SeatResetDecision::HonourStore, a, 60000);
        m.noteReset(SeatResetDecision::HonourStore, b, 63000);
        uint64_t w = 0;
        check(nearPose(m.previousSeat(), seat) && nearPose(m.seat(), b), "two recentres in a row: the previous seat is still the stored one");
        check(!m.takeCommit(67999, w) && m.takeCommit(68000, w) && w == 5000, "...and the wait counts from the last");
        SeatMemory n; n.setSeat(seat);
        n.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 0); n.applyDone(); n.noteOrigin(true);
        n.noteReset(SeatResetDecision::HonourStore, a, 60000);
        n.noteReset(SeatResetDecision::HonourStore, b, 63000);
        const SeatEvents ev = n.onContext(ctx(true, SeatWhere::Aboard, 1, 3), 65000);
        check(ev.discardedPending && nearPose(n.seat(), seat) && ev.discardAgeMs == 2000, "a journal event inside the wait takes both back");
    }
    // nothing stored: nothing to go back to, so it is written at once
    {
        SeatMemory m;
        m.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 0);
        check(m.noteReset(SeatResetDecision::HonourStore, ramp, 60000) && !m.storePending(), "with no seat stored the first recentre is written at once");
    }
    // game mode and replaced resets hold nothing
    {
        SeatMemory m; m.setSeat(seat);
        m.onContext(ctx(true, SeatWhere::Aboard, 1, 1), 0);
        m.noteReset(SeatResetDecision::ReplaceWithSeat, seat, 100);
        check(!m.storePending(), "a replaced reset holds nothing");
    }
}

// ---- the whole story, as the first flight will tell it
void story() {
    const SeatPose seat{-0.53, 0.45, -1.11, seatDegToRad(-28)};
    SeatMemory m;
    // 1. a first launch: nothing stored, keep. The start is today's; the splash captures.
    m.onContext(ctx(true, SeatWhere::Unknown), 0);
    check(seatStartPlan(m.keep(), m.has()) == SeatStartPlan::CaptureLate, "launch 1: start as today");
    check(seatIntroPlan(m.keep(), m.has(), m.originIsSeat()) == SeatIntroPlan::Capture, "launch 1: the splash captures");
    m.setSeat(seat); m.noteOrigin(true);
    // 2. on foot in the station: everyone sits, so the spawn reset is replaced by the seat
    SeatEvents ev = m.onContext(ctx(true, SeatWhere::OnFoot, 0, 1), 20000);
    check(ev.windowOpened && !ev.applyWanted, "on foot: a window opens (the origin is already the seat)");
    check(m.classifyReset(20500) == SeatResetDecision::ReplaceWithSeat, "on foot: the spawn reset is replaced");
    m.noteReset(SeatResetDecision::ReplaceWithSeat, seat);
    check(m.has() && nearPose(m.seat(), seat) && m.originIsSeat(), "...and the seat stands");
    // 3. boarding by the ramp: a reset lands BEFORE the flip (4.4 s early), outside any window: read as the player's own, and stored
    check(m.classifyReset(60000) == SeatResetDecision::HonourStore, "boarding: an early reset, outside a window, is read as the player's own");
    const SeatPose early{-0.40, 0.10, -1.00, seatDegToRad(-10)};
    check(!m.noteReset(SeatResetDecision::HonourStore, early, 60000) && m.storePending(), "...but held, not written");
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 1, 2), 64400);
    check(ev.discardedPending && ev.discardAgeMs == 4400 && nearPose(m.seat(), seat), "boarding 4.4 s later takes the ramp's reset back: the old seat stands");
    check(ev.applyWanted, "boarding: the flip puts the seat back");
    m.applyDone(); m.noteOrigin(true);
    // 4. the game resets again 5 s after the flip (the interface route): replaced
    check(m.classifyReset(69400) == SeatResetDecision::ReplaceWithSeat, "boarding: the late reset is replaced");
    m.noteReset(SeatResetDecision::ReplaceWithSeat, seat);
    check(m.originIsSeat(), "...and the origin is on the seat");
    // 5. a quarter of an hour later the player presses recentre
    check(m.classifyReset(900000) == SeatResetDecision::HonourStore, "later: the player's own recentre");
    const SeatPose mine{-0.50, 0.46, -1.05, seatDegToRad(-25)};
    uint64_t held = 0;
    check(!m.noteReset(SeatResetDecision::HonourStore, mine, 900000) && !m.takeCommit(904999, held) && m.takeCommit(905000, held) && nearPose(m.seat(), mine),
          "...becomes the seat after five quiet seconds");
    // 6. docking, disembarking, boarding again: the new seat
    ev = m.onContext(ctx(true, SeatWhere::OnFoot, 0, 3), 1000000);
    check(ev.windowOpened && std::string(ev.why) == "disembarked" && m.classifyReset(1003000) == SeatResetDecision::ReplaceWithSeat,
          "disembarking: the game's reset in the window is replaced by the player's seat");
    ev = m.onContext(ctx(true, SeatWhere::Aboard, 4, 4), 1060000);
    check(m.classifyReset(1060001) == SeatResetDecision::ReplaceWithSeat && nearPose(m.seat(), mine), "boarding again uses the recentre the player made");
    // 7. back in game mode nothing of this applies
    m.onContext(ctx(false, SeatWhere::Aboard, 4, 4), 1100000);
    check(m.classifyReset(1100001) == SeatResetDecision::GameMode, "game mode: honoured as always");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("seat_memory_test: dry-run (no files, no runtime)");
        return 0;
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    contextWord();
    decisionTable();
    noteResets();
    transitions();
    plans();
    picking();
    stillness();
    runtimeRecentre();
    retroWindow();
    meter();
    bookText();
    files();
    story();
    std::printf("seat_memory_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
