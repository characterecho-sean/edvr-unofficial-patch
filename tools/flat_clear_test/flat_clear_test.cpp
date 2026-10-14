// flat_clear_test -- the headset while flat's policy (src\openxr\flat_clear_state.h, docs\design-unified-install-2026-10-12.md section 12,
// N1) and the word that carries the game's profile across (src\common\frame_flag.h). The run starts only with the graphics half flat,
// no game frame open, the session running and 250 ms of quiet, at the edges 249 and 250. Each stop names its reason in the order the
// runtime's trace prints them. The word reads VR when nobody has written it. Built from the repo root by build.bat's
// :rig_flat_clear_test. Windows-free: it includes only the pure headers. --dry-run writes nothing; --self-test runs the checks.
#include "../../src/openxr/flat_clear_state.h"
#include "../../src/openxr/flat_notice_placement.h"
#include "../../src/common/frame_flag.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using edvr::openxr::FlatClearState;
using Stop = FlatClearState::Stop;

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

// The inputs of one owner pump, named so each case reads as its situation.
struct Pump {
    bool vrProfile = false;      // the graphics half says VR
    bool gameFrameOpen = false;  // the game holds a frame
    bool running = true;         // the session is running
    uint64_t quietMs = 1000;     // ms since the game's last WaitGetPoses or Submit
};

// The flat, quiet, idle situation in which the owner should be clearing.
Pump flatQuiet() { return Pump{}; }

FlatClearState::Step pump(FlatClearState& s, const Pump& p) {
    return s.decide(p.vrProfile, p.gameFrameOpen, p.running, p.quietMs);
}

void quietEdges() {
    FlatClearState s;
    Pump p = flatQuiet();
    p.quietMs = 249;
    check(!pump(s, p).run, "quiet 249 ms does not start the flat clear");
    check(!s.active(), "no stretch after 249 ms of quiet");
    p.quietMs = 250;
    const auto step = pump(s, p);
    check(step.run && step.started && !step.stopped, "quiet 250 ms starts the flat clear, and that pump is its first");
    check(s.active(), "the stretch is active after its first run");
    p.quietMs = 0;
    check(!pump(s, p).run, "quiet 0 ms (a game call just now) runs nothing");
}

void gatesBlockRun() {
    FlatClearState s;
    Pump vr = flatQuiet();
    vr.vrProfile = true;
    check(!pump(s, vr).run, "a VR profile never runs the flat clear, however quiet");
    Pump open = flatQuiet();
    open.gameFrameOpen = true;
    check(!pump(s, open).run, "a game frame held open blocks the flat clear");
    Pump down = flatQuiet();
    down.running = false;
    check(!pump(s, down).run, "a session not running blocks the flat clear");
    check(!s.active(), "blocked pumps leave no stretch");
}

void startAndSteadyRun() {
    FlatClearState s;
    const auto first = pump(s, flatQuiet());
    check(first.run && first.started && !first.stopped, "first clearing pump reports started, not stopped");
    const auto second = pump(s, flatQuiet());
    check(second.run && !second.started && !second.stopped, "a steady clearing pump reports neither start nor stop");
}

void stopReasonVrProfile() {
    FlatClearState s;
    pump(s, flatQuiet());
    Pump vr = flatQuiet();
    vr.vrProfile = true;
    const auto step = pump(s, vr);
    check(!step.run && step.stopped && step.reason == Stop::VrProfile, "the bit going back to VR stops the stretch, reason vr_profile");
    check(!s.active(), "the stretch is inactive after the VR stop");
    const auto again = pump(s, vr);
    check(!again.stopped && again.reason == Stop::None, "a stop is reported once, not on every pump after it");
}

void stopReasonGameFrame() {
    FlatClearState s;
    pump(s, flatQuiet());
    Pump open = flatQuiet();
    open.gameFrameOpen = true;
    const auto held = pump(s, open);
    check(held.stopped && held.reason == Stop::GameFrame, "a game frame opened during the stretch stops it, reason game_frame");

    FlatClearState t;
    pump(t, flatQuiet());
    Pump called = flatQuiet();
    called.quietMs = 0;
    const auto call = pump(t, called);
    check(call.stopped && call.reason == Stop::GameFrame, "a game call inside the quiet window stops it, reason game_frame");
}

void stopReasonSession() {
    FlatClearState s;
    pump(s, flatQuiet());
    Pump down = flatQuiet();
    down.running = false;
    const auto step = pump(s, down);
    check(step.stopped && step.reason == Stop::Session, "the session ending stops the stretch, reason session");
}

void vrBeatsOtherReasons() {
    FlatClearState s;
    pump(s, flatQuiet());
    Pump all = flatQuiet();
    all.vrProfile = true;
    all.gameFrameOpen = true;
    all.running = false;
    check(pump(s, all).reason == Stop::VrProfile, "with every blocker up, the stop names the VR profile first");
}

void restartAfterStop() {
    FlatClearState s;
    pump(s, flatQuiet());
    Pump vr = flatQuiet();
    vr.vrProfile = true;
    pump(s, vr);
    const auto back = pump(s, flatQuiet());
    check(back.run && back.started, "flat again after a VR stop starts a new stretch");
}

void vrFromTheStart() {
    FlatClearState s;
    Pump vr = flatQuiet();
    vr.vrProfile = true;
    const auto step = pump(s, vr);
    check(!step.started && !step.stopped && step.reason == Stop::None, "a session that never went flat has no transitions");
}

void staleEdges() {
    check(!FlatClearState::staleGameFrame(false, true, 999), "a frame open 999 ms after the game's last call is not yet stale");
    check(FlatClearState::staleGameFrame(false, true, 1000), "a frame open 1000 ms after the game's last call is stale");
    check(!FlatClearState::staleGameFrame(true, true, 5000), "VR never ends a game frame, however silent the game");
    check(!FlatClearState::staleGameFrame(false, false, 5000), "no open frame, nothing to end");
}

void failureLatch() {
    FlatClearState s;
    pump(s, flatQuiet());
    s.fail();
    check(!s.active(), "a failed empty frame ends the stretch without an active flag");
    const auto stillFlat = pump(s, flatQuiet());
    check(!stillFlat.run && !stillFlat.started && !stillFlat.stopped,
          "after a failure, still flat: nothing runs and no transition is reported");
    check(!pump(s, flatQuiet()).run, "the failure holds over later flat pumps");
    Pump vr = flatQuiet();
    vr.vrProfile = true;
    check(!pump(s, vr).run, "VR runs nothing while the failure is latched");
    const auto again = pump(s, flatQuiet());
    check(again.run && again.started, "VR then flat again re-arms clearing and runs");
}

// The panel's clock (kPanelTextMs = 10 s of text from a stretch's first panel frame, then kPanelBlackSettleMs = 1 s of black
// that still draws, then no layers): each edge is traced once, and a new stretch starts the clock again.
void panelClock() {
    FlatClearState s;
    pump(s, flatQuiet());
    const uint64_t t0 = 5000;
    const auto first = s.panelFrame(t0);
    check(first.text && first.draw && !first.wentDark && !first.wentEmpty, "the first panel frame of a stretch shows the text and starts the clock");
    check(s.panelFrame(t0 + 9999).text, "9.999 s after the first panel frame the text still shows");
    const auto dark = s.panelFrame(t0 + 10000);
    check(!dark.text && dark.draw && dark.wentDark && !dark.wentEmpty && dark.afterMs == 10000,
          "10 s after it the text is gone and the black still draws, traced once with after_ms 10000");
    check(!s.panelFrame(t0 + 10999).text && s.panelFrame(t0 + 10999).draw, "10.999 s: black, still drawing");
    const auto empty = s.panelFrame(t0 + 11000);
    check(!empty.draw && empty.wentEmpty && !empty.wentDark && empty.afterMs == 11000,
          "11 s after it the frames stop drawing, traced once with after_ms 11000");
    const auto later = s.panelFrame(t0 + 15000);
    check(!later.draw && !later.wentEmpty && !later.wentDark, "the frames after that trace nothing more");
    Pump vr = flatQuiet();
    vr.vrProfile = true;
    pump(s, vr);
    pump(s, flatQuiet());
    const uint64_t t1 = t0 + 130000;
    check(s.panelFrame(t1).text && s.panelFrame(t1 + 9999).text, "a new stretch starts the clock again: the text shows 9.999 s in");
    const auto again = s.panelFrame(t1 + 10000);
    check(!again.text && again.draw && again.wentDark && again.afterMs == 10000, "the new stretch goes dark 10 s after its own first panel frame, traced once");
    const auto againEmpty = s.panelFrame(t1 + 11000);
    check(!againEmpty.draw && againEmpty.wentEmpty && againEmpty.afterMs == 11000, "and stops drawing 11 s in, traced once");
}

// The notice's placement (flat_notice_placement.h): the panel's centre is the eye's optical centre from its own tangents.
void noticeGeometry() {
    using namespace edvr::openxr;
    check(noticePanelWidth(512) == 205, "the notice panel is 40% of a 512 px eye image");
    check(noticeHeadlineHeight(512) == 18 && noticeDetailHeight(512) == 14, "the font cells are the eye height over 28 and over 36");
    const double t = std::tan(0.785398163);
    NoticeRect r{};
    check(flatNoticeRect(512, 512, 205, 60, -t, t, t, -t, r) && r.x == 154 && r.y == 226 && r.width == 205 && r.height == 60,
          "a symmetric fov centres the panel on the image (x 154, y 226)");
    // A canted fov: the left edge reaches less far than the right, so the straight-ahead point sits left of the image middle; the top
    // edge reaches further than the bottom, so it sits below the middle (image y grows downward).
    const double tanL = std::tan(-0.6), tanR = std::tan(0.9), tanU = std::tan(0.7), tanD = std::tan(-0.5);
    check(flatNoticeRect(512, 512, 205, 60, tanL, tanR, tanU, tanD, r) && std::abs(r.x + 102 - 180) <= 1,
          "a canted fov moves the centre to its own optical centre (x about 180, not 256)");
    check(r.y + 30 > 256, "a top edge that reaches further than the bottom puts the centre below the image middle");
    check(flatNoticeRect(512, 512, 205, 60, -2.0, 0.1, 0.5, -0.5, r) && r.x == 512 - 205 && r.y == 256 - 30,
          "a centre past the right edge clamps the panel inside the image");
    check(flatNoticeRect(512, 512, 205, 60, 0.0, 0.0, 0.5, -0.5, r) == false, "a zero horizontal span is refused");
    check(flatNoticeRect(512, 512, 205, 60, -0.5, 0.5, 0.5, 0.5, r) == false, "a zero vertical span is refused");
    check(flatNoticeRect(512, 512, 205, 60, std::nan(""), 0.5, 0.5, -0.5, r) == false, "a non-finite tangent is refused");
    check(flatNoticeRect(0, 512, 205, 60, -0.5, 0.5, 0.5, -0.5, r) == false, "an empty image is refused");
}

void defaultWordReadsVr() {
    check(edvr::kVrProfileWordVr == 1 && edvr::kVrProfileWordNotVr == 2, "the word's values are 1 for VR and 2 for not VR");
    check(edvr::vrProfileWordIsVr(0), "a fresh word (nobody wrote) reads as VR, so an absent graphics half changes nothing");
    check(edvr::vrProfileWordIsVr(edvr::kVrProfileWordVr), "the word written as VR reads as VR");
    check(!edvr::vrProfileWordIsVr(edvr::kVrProfileWordNotVr), "the word written as not VR reads as not VR");
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun && !selfTest) {
        std::printf("flat_clear_test: dry run, nothing written\n");
        return 0;
    }
    quietEdges();
    gatesBlockRun();
    startAndSteadyRun();
    stopReasonVrProfile();
    stopReasonGameFrame();
    stopReasonSession();
    vrBeatsOtherReasons();
    restartAfterStop();
    vrFromTheStart();
    staleEdges();
    failureLatch();
    panelClock();
    noticeGeometry();
    defaultWordReadsVr();
    if (g_fails) {
        std::printf("FAIL: flat_clear_test %d of %d checks failed\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: flat_clear_test %d checks (quiet edges 249/250 ms, gates, start and stop transitions with reasons, "
                "restart, stale game frame edges 999/1000 ms, failure latch re-armed by VR, panel text 9.999 s, dark at 10 s, empty at 11 s, "
                "default word reads VR)\n", g_checks);
    return 0;
}
