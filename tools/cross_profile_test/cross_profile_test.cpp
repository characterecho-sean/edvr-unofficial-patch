// cross_profile_test -- src\d3d11\cross_profile_cost.h: the 1-in-64 scaling, the per-frame division, the sampling of timed calls, the
// capture scope, and the report's windows and session totals with the profile gate. Built from the repo root by build.bat's
// :rig_cross_profile_test. Windows API only through the header (QueryPerformanceCounter). --dry-run writes nothing; --self-test runs the
// checks and prints the two sample lines the edvr_log.py fixture copies.
#include "../../src/d3d11/cross_profile_cost.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using namespace edvr::cross_profile;

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

bool closeTo(double a, double b) { return std::fabs(a - b) < 1e-9; }

void resetCounters() {
    for (Counter& c : g_sites) {
        c.calls.store(0);
        c.ticks.store(0);
    }
    g_captures.calls.store(0);
    g_captures.ticks.store(0);
}

bool startsWith(const char* text, const char* prefix) { return std::strncmp(text, prefix, std::strlen(prefix)) == 0; }
bool contains(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun) {
        std::printf("cross_profile_test: --dry-run writes nothing; the checks run under --self-test\n");
        return 0;
    }
    if (!selfTest) {
        std::printf("usage: cross_profile_test --self-test | --dry-run\n");
        return 1;
    }

    // Arithmetic: a sampled tick stands for kSampleEvery calls; a per-frame figure divides the scaled sum by the frames.
    check(kSampleEvery == 64, "the sampling rate is one call in 64");
    check(closeTo(scaledMicros(1, 1000000), 64.0), "one sampled tick at 1 MHz is 64 us over all calls");
    check(closeTo(scaledMicros(10, 1000000), 640.0), "ten sampled ticks at 1 MHz scale to 640 us");
    check(closeTo(scaledMicros(1, 2000000), 32.0), "the scaling follows the QPC frequency (2 MHz gives 32 us)");
    check(closeTo(scaledMicros(5, 0), 0.0), "a zero QPC frequency reads as zero, not a division fault");
    check(closeTo(perFrameMicros(10, 1000000, 4), 160.0), "10 sampled ticks (640 us) over 4 frames is 160 us per frame");
    check(closeTo(perFrameMicros(640, 1000000, 0), 0.0), "a window with no frames reads as zero per frame");
    check(closeTo(captureMillis(1500, 1000000), 1.5), "1500 ticks at 1 MHz is 1.5 ms (captures are not scaled)");
    check(closeTo(captureMillis(1500, 0), 0.0), "a zero QPC frequency gives zero milliseconds");

    // Sampling: every call is counted; one in 64 is timed, starting with the first (calls 0, 64, 128 of 130).
    resetCounters();
    int timed = 0;
    for (int i = 0; i < 130; ++i) {
        Probe p = Probe::begin(g_sites[static_cast<size_t>(Site::FlatPsConstantBuffers)], false);
        if (p.timed()) ++timed;
        p.stop();
    }
    check(g_sites[static_cast<size_t>(Site::FlatPsConstantBuffers)].calls.load() == 130, "every one of 130 calls is counted");
    check(timed == 3, "three of 130 calls are timed: the first and every 64th after it");

    // A default probe (an active call) counts nothing and times nothing.
    resetCounters();
    Probe idle = enterSite(Site::VrCoverDetour, false);
    idle.stop();
    check(g_sites[static_cast<size_t>(Site::VrCoverDetour)].calls.load() == 0 && !idle.timed(),
          "an active call's default probe counts nothing");

    // stop() is idempotent; restart() opens a second stretch of the same call and does not count it again.
    resetCounters();
    Probe capture = Probe::begin(g_captures, true);
    capture.stop();
    const uint64_t afterFirst = g_captures.ticks.load();
    capture.stop();
    check(g_captures.ticks.load() == afterFirst, "a second stop() adds nothing");
    capture.restart();
    capture.stop();
    check(g_captures.calls.load() == 1, "restart() does not count the call again");
    check(g_captures.ticks.load() >= afterFirst, "restart() adds a second stretch's ticks");

    // Captures: a scope counts and times while its gate is open, and nothing while it is closed.
    resetCounters();
    { CaptureScope s(true); }
    { CaptureScope s(false); }
    { CaptureScope s(true); }
    check(g_captures.calls.load() == 2, "a capture scope counts only while its gate is open");

    // Report: 30 s windows read the inactive owner; the 5-minute session totals name both. Frames: the first call starts the clock.
    resetCounters();
    Reporter r;
    Reporter::Lines lines;
    const uint64_t freq = 1000000;
    r.frame(0, freq, false, true, lines);
    check(lines.count == 0, "the first frame starts the clock and logs nothing");
    for (int i = 0; i < 130; ++i) enterSite(Site::FlatPsConstantBuffers, true).stop();
    r.frame(30000, freq, false, true, lines);
    check(lines.count == 1, "at 30 s with VR active, one window line is logged");
    check(startsWith(lines.text[0], "cross-profile cost: flat hooks while vr:"), "VR active names the flat hooks");
    check(contains(lines.text[0], "psConstantBuffers=130/"), "the window line carries the 130 calls");
    check(contains(lines.text[0], "over 1 frames"), "the window line counts its own frame");
    check(contains(lines.text[0], "; captures 0 in 0.00 ms (session 0 in 0.00 ms)"), "the window line carries the captures");
    std::printf("sample: %s\n", lines.text[0]);

    r.frame(60000, freq, false, false, lines);
    check(lines.count == 0, "with neither profile active, nothing is logged");

    r.frame(300000, freq, true, false, lines);
    check(lines.count == 3, "at 5 minutes: the window line (flat active) and both session lines");
    check(startsWith(lines.text[0], "cross-profile cost: vr hooks while flat:"), "flat active names the VR hooks in the window line");
    check(contains(lines.text[0], "over 1 frames"), "the VR window line divides by the one flat frame");
    check(startsWith(lines.text[1], "cross-profile cost session: flat hooks while vr:"), "the session names the flat hooks");
    check(contains(lines.text[1], "psConstantBuffers=130/"), "the session total keeps the 130 calls");
    check(contains(lines.text[1], "over 2 frames"), "the flat session divides by the two VR frames, not all four");
    check(startsWith(lines.text[2], "cross-profile cost session: vr hooks while flat:"), "the session names the VR hooks");
    check(contains(lines.text[2], "coverDetour=0/0.00"), "a VR site with no calls reads zero");
    check(contains(lines.text[2], "over 1 frames"), "the VR session divides by the one flat frame");

    // Mixed: the review's case. 28 VR frames, then one flat frame carrying 64 cover-detour calls with 10 sampled ticks (1 MHz: 640 us
    // over the calls it stands for). Before the fix the VR-hooks line divided by all 29 frames and read 22 us. Each owner now divides by
    // the frames its hooks ran in: the flat hooks by the 28 VR frames, the cover detour by the one flat frame.
    resetCounters();
    Reporter m;
    m.frame(0, freq, false, true, lines);
    for (int k = 1; k <= 27; ++k) m.frame(static_cast<uint64_t>(k) * 1000, freq, false, true, lines);
    m.frame(28000, freq, true, false, lines);
    g_sites[static_cast<size_t>(Site::FlatPsConstantBuffers)].calls.store(28);
    g_sites[static_cast<size_t>(Site::VrCoverDetour)].calls.store(64);
    g_sites[static_cast<size_t>(Site::VrCoverDetour)].ticks.store(10);
    m.frame(30000, freq, false, true, lines);
    check(lines.count == 2, "a window spanning a switch prints a line for both owners");
    check(startsWith(lines.text[0], "cross-profile cost: flat hooks while vr:"), "the flat hooks' line comes first");
    check(contains(lines.text[0], "psConstantBuffers=28/0.00"), "the flat hooks divide by the 28 VR frames");
    check(contains(lines.text[0], "over 28 frames"), "the flat window line says it covers 28 frames");
    check(startsWith(lines.text[1], "cross-profile cost: vr hooks while flat:"), "the VR hooks' line follows");
    check(contains(lines.text[1], "coverDetour=64/640.00"), "the cover detour reads 640 us per flat frame, not per 29");
    check(contains(lines.text[1], "over 1 frames"), "the VR-hooks window line says it covers the one flat frame");
    std::printf("sample: %s\n", lines.text[0]);
    std::printf("sample: %s\n", lines.text[1]);

    // The mixed window's session: 30 VR frames and one flat frame in all; each owner's session line divides by its own total.
    m.frame(300000, freq, false, true, lines);
    check(lines.count == 3, "the mixed run's 5-minute point: a window line and both session lines");
    check(startsWith(lines.text[1], "cross-profile cost session: flat hooks while vr:"), "the mixed session names the flat hooks");
    check(contains(lines.text[1], "over 30 frames"), "the flat session divides by all 30 VR frames");
    check(startsWith(lines.text[2], "cross-profile cost session: vr hooks while flat:"), "the mixed session names the VR hooks");
    check(contains(lines.text[2], "coverDetour=64/640.00"), "the VR session divides its 10 ticks by the one flat frame");
    check(contains(lines.text[2], "over 1 frames"), "the VR session says it covers the one flat frame");
    std::printf("sample: %s\n", lines.text[2]);

    // The gate: a site is measured only while the profile its report divides by is active. Passthrough and an invalid profile have
    // neither flag set, so neither owner's sites are measured there.
    check(inactiveFor(Owner::Vr, true, false) && !inactiveFor(Owner::Vr, false, true), "VR sites are measured while flat runs, not while VR does");
    check(inactiveFor(Owner::Flat, false, true) && !inactiveFor(Owner::Flat, true, false), "flat sites are measured while VR runs, not while flat does");
    check(!inactiveFor(Owner::Vr, false, false) && !inactiveFor(Owner::Flat, false, false), "neither owner is measured in Passthrough");

    // Review finding 8: the mode watch's query ran in Passthrough (neither flag set), was sampled as VR-site cost, and a window that then
    // reached one Flat frame divided it by that frame (modeWatchFrame=2/640.00). Under the gate the Passthrough sample is not taken and
    // the Flat frame reads zero; a sample taken in the Flat frame itself still counts.
    resetCounters();
    Reporter p;
    p.frame(0, freq, false, false, lines);                                   // Passthrough frames: counted toward neither profile
    for (int i = 0; i < 128; ++i) {
        Probe q = enterSite(Site::VrModeWatchFrame, inactiveFor(Owner::Vr, false, false));
        q.stop();
    }
    check(g_sites[static_cast<size_t>(Site::VrModeWatchFrame)].calls.load() == 0, "a mode-watch query in Passthrough is not counted");
    p.frame(15000, freq, true, false, lines);                                // one Flat frame
    {
        Probe q = enterSite(Site::VrModeWatchFrame, inactiveFor(Owner::Vr, true, false));
        q.stop();
    }
    check(g_sites[static_cast<size_t>(Site::VrModeWatchFrame)].calls.load() == 1, "one taken while Flat is active is counted");
    g_sites[static_cast<size_t>(Site::VrModeWatchFrame)].calls.store(0);   // that one call is not the window under test
    g_sites[static_cast<size_t>(Site::VrModeWatchFrame)].ticks.store(0);
    p.frame(30000, freq, false, false, lines);
    bool vrLine = false;
    for (int i = 0; i < lines.count; ++i)
        if (startsWith(lines.text[i], "cross-profile cost: vr hooks while flat:")) {
            vrLine = true;
            check(contains(lines.text[i], "modeWatchFrame=0/0.00") && contains(lines.text[i], "over 1 frames"),
                  "the Flat frame carries no Passthrough cost");
        }
    check(vrLine, "the window prints the VR hooks' line for its one Flat frame");

    std::printf("cross_profile_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
