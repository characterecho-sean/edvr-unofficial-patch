#pragma once
// The terrain-culling arc's automatic probe cycle (src/d3d11/cull_cycle.h; docs\terrain-culling.md): the schedule, the window
// boundaries, the discard of each window's first frames, the per-eye accumulation, the head speed, the paired lines at the end of
// a cycle, the status the driver reports for a request, and the fixture tools\cull_cycle_fixture.log the log tool's --self-test
// reads, held here to exactly what the writer writes.
#include "../../src/d3d11/cull_cycle.h"
#include "../../src/d3d11/mono_camera_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace cull_cases {
using namespace edvr::cullcycle;
namespace monocam = edvr::monocam;

struct Lines {
    std::vector<std::string> v;
    void operator()(const char* s) { v.emplace_back(s); }
    size_t count(const std::string& prefix) const {
        size_t n = 0;
        for (const auto& l : v) if (l.rfind(prefix, 0) == 0) ++n;
        return n;
    }
    bool has(const std::string& text) const {
        for (const auto& l : v) if (l == text) return true;
        return false;
    }
    std::string join() const {
        std::string out;
        for (const auto& l : v) out += l + "\n";
        return out;
    }
};

// counts(group, frameIndex, draws, indices): what the frame that just ended counted. frameIndex is 1 for the first frame of a window.
using Counts = std::function<void(uint32_t group, unsigned frameIndex, uint32_t draws[2], uint64_t indices[2])>;

// A scripted flight: frame boundaries `periodUs` apart, the head yawing at yawDegPerSecond(window) deg/s (-1: no pose).
struct Session {
    Cycle cycle;
    Lines lines;
    uint64_t t = 1000000, periodUs = 10000;
    uint32_t group = 0;
    unsigned frameIndex = 0;
    double yaw = 0.0;
    std::function<double(uint64_t window)> yawRate = [](uint64_t) { return -1.0; };
    bool opened = false;
    uint32_t step(const Counts& counts) {
        uint32_t d[2] = {};
        uint64_t ix[2] = {};
        if (opened) {
            ++frameIndex;
            counts(group, frameIndex, d, ix);   // the first boundary only opens window 1: no frame has ended
        }
        opened = true;
        const double rate = yawRate(cycle.windowsClosed());
        float pose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (rate >= 0.0) {
            yaw += rate * static_cast<double>(periodUs) * 1e-6;
            const double a = yaw * 3.14159265358979323846 / 180.0;
            pose[0] = static_cast<float>(std::cos(a)); pose[2] = static_cast<float>(std::sin(a));
            pose[8] = static_cast<float>(-std::sin(a)); pose[10] = static_cast<float>(std::cos(a));
        }
        const uint32_t next = cycle.beginFrame(t, rate >= 0.0 ? pose : nullptr, d, ix, lines);
        t += periodUs;
        if (next != group) { group = next; frameIndex = 0; }
        return next;
    }
    void stepWindows(unsigned windows, const Counts& counts) {
        const uint64_t target = cycle.windowsClosed() + windows;
        for (unsigned guard = 0; cycle.windowsClosed() < target && guard < 100000; ++guard) step(counts);
    }
};

// The scripted counts the fixture and the pair cells use. Off: 10 / 9 draws a frame (left / right); the groups differ from it by a
// known amount (mono, group 7, is 13 / 10); every draw is 2304 indices. The second cycle's `all` is two draws up on the first's.
inline void scriptCounts(uint32_t group, uint64_t cycleIndex, uint32_t d[2], uint64_t ix[2]) {
    static const uint32_t left[8] = {10, 16, 10, 12, 10, 14, 11, 13}, right[8] = {9, 13, 9, 9, 9, 12, 9, 10};
    d[0] = left[group] + (group == 1 && cycleIndex >= 1 ? 2u : 0u);
    d[1] = right[group];
    ix[0] = static_cast<uint64_t>(d[0]) * 2304u;
    ix[1] = static_cast<uint64_t>(d[1]) * 2304u;
}

// The positive control's scripted counts: what the planet-terrain draws read per frame in each cull guard stage. Off and waiting
// are the same scene (10 / 9 draws); adopting is the game rebuilding at the new size (12 / 10); live is wider (16 or 18 / 14, the
// left eye alternating by live window so the windows have a spread).
inline void measureCounts(uint32_t stage, uint64_t liveWindow, uint32_t d[2], uint64_t ix[2]) {
    d[0] = stage <= 1 ? 10u : stage == 2 ? 12u : 16u + 2u * static_cast<uint32_t>(liveWindow % 2);
    d[1] = stage <= 1 ? 9u : stage == 2 ? 10u : 14u;
    ix[0] = static_cast<uint64_t>(d[0]) * 2304u;
    ix[1] = static_cast<uint64_t>(d[1]) * 2304u;
}
// The scripted guard: off for 8 windows, waiting from 1650 frames in (a window dropped), adopting from 2000, live from 2300 for 8.
inline uint32_t measureStage(unsigned f) { return f < 1650 ? 0u : f < 2000 ? 1u : f < 2300 ? 2u : 3u; }

// tools\cull_cycle_fixture.log: first what the mono camera hook writes (its lazy install, the callers and the writer calls it
// observed, a steady `mono` run that starts and stops); then the driver, run through the counters the draw path feeds, on a 10 ms
// clock, for two cycles and the first window of a third (29 windows), the head turning 3 deg/s except 40 deg/s through the first ui
// window, the mono hook live (2 reads a frame, 3 in a mono window); then a measure run (the positive control): the cull guard off,
// waiting, adopting and live (18 windows, 3 dropped by a stage change), with the hook not live (no ", mono reads" field).
inline std::string fixtureLog() {
    Driver driver;
    Lines lines;
    {
        // The hook as it lands: live, with the writer's observe live, then three distinct callers (the second is the mono filler,
        // the first repeated) and two writer calls (the second's *out unreadable), drained in order.
        constexpr uintptr_t base = 0x7FF600000000ull;
        monocam::Lazy lazy;
        lazy.frame(true, [] { return static_cast<const char*>(nullptr); }, [] { return monocam::InstallResult{}; }, lines);
        monocam::Observer observer;
        observer.reset(base, base + monocam::kFillerReturnRva);
        observer.noteGetterCaller(base + 0x28719A4);
        observer.noteGetterCaller(base + monocam::kFillerReturnRva);
        observer.noteGetterCaller(base + 0x28719A4);
        observer.noteGetterCaller(base + 0x2A03F51);
        observer.noteWriter(3840, 2160, 1.0f, true, 1.7777778f, 1.7777778f, base + 0x2871D10);
        observer.noteWriter(0, 0, 0.0f, false, 0.0f, 0.0f, base + 0x2871D10);
        observer.drain(lines);
        SteadyMono steady;
        steady.frame(true, false, true, lines);
        steady.frame(false, false, true, lines);
    }
    g_monoHookLive.store(true, std::memory_order_relaxed);
    uint64_t t = 1000000;
    double yaw = 0.0;
    uint32_t group = 0;
    for (unsigned guard = 0; guard < 100000; ++guard) {
        const uint64_t closed = driver.cycle().windowsClosed();
        if (closed >= 29) break;
        // What the frame that just ended drew: the counters the draw path adds to.
        if (guard) {
            uint32_t d[2];
            uint64_t ix[2];
            scriptCounts(group, closed / 14, d, ix);
            for (int e = 0; e < 2; ++e)
                for (uint32_t i = 0; i < d[e]; ++i) noteTerrainDraw(e, 2304u, 1u);
            for (uint32_t i = 0; i < (group == kMonoGroup ? 3u : 2u); ++i) noteMonoRead();
        }
        const double rate = closed == 5 ? 40.0 : 3.0;
        yaw += rate * 0.01;
        const double a = yaw * 3.14159265358979323846 / 180.0;
        float pose[12] = {static_cast<float>(std::cos(a)), 0, static_cast<float>(std::sin(a)), 0, 0, 1, 0, 0,
                          static_cast<float>(-std::sin(a)), 0, static_cast<float>(std::cos(a)), 0};
        group = driver.frame(true, false, false, true, 3, t, pose, lines);
        t += 10000;
    }
    driver.frame(false, false, false, true, 3, t, nullptr, lines);   // the key goes back to off: "stopped after"
    t += 10000;
    g_monoHookLive.store(false, std::memory_order_relaxed);          // the measure run below has no mono hook: no ", mono reads" field
    double yaw2 = 0.0;
    for (unsigned f = 0; f <= 3900; ++f) {
        const uint32_t stage = measureStage(f);
        if (f) {
            uint32_t d[2];
            uint64_t ix[2];
            measureCounts(measureStage(f - 1), driver.cycle().windowsClosed() >= 10 ? driver.cycle().windowsClosed() - 10 : 0, d, ix);
            for (int e = 0; e < 2; ++e)
                for (uint32_t i = 0; i < d[e]; ++i) noteTerrainDraw(e, 2304u, 1u);
        }
        yaw2 += 0.03;
        const double a = yaw2 * 3.14159265358979323846 / 180.0;
        float pose[12] = {static_cast<float>(std::cos(a)), 0, static_cast<float>(std::sin(a)), 0, 0, 1, 0, 0,
                          static_cast<float>(-std::sin(a)), 0, static_cast<float>(std::cos(a)), 0};
        driver.frame(false, true, stage != 0, true, stage, t, pose, lines);   // the guard is configured from the second stage on
        t += 10000;
    }
    driver.frame(false, false, false, true, 0, t, nullptr, lines);
    std::string out;
    for (const auto& l : lines.v) out += "[00:00:00.000] " + l + "\n";
    return out;
}

template <class Check>
void runCullCycleCases(Check&& check) {
    const auto closeTo = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
    // ---- the schedule and the windows ----------------------------------------------------------------------------------------
    {
        Session s;
        std::vector<uint32_t> sequence{0};
        const Counts counts = [](uint32_t g, unsigned, uint32_t d[2], uint64_t ix[2]) { scriptCounts(g, 0, d, ix); };
        for (unsigned guard = 0; s.cycle.windowsClosed() < 15 && guard < 100000; ++guard) {
            const uint32_t before = s.group;
            const uint32_t next = s.step(counts);
            if (next != before) sequence.push_back(next);
        }
        check(s.cycle.windowsClosed() == 15 && s.cycle.cyclesDone() == 1, "15 windows closed: one full cycle and the next cycle's first window");
        const std::vector<uint32_t> want = {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 1};
        check(sequence == want, "the schedule is off, all, off, camera, off, ui, off, sky, off, sizes, off, other, off, mono, and then off, all again");
        check(kSchedule[0] == 0 && kSchedule[1] == 1 && kSchedule[3] == 2 && kSchedule[5] == 3 && kSchedule[7] == 4 && kSchedule[9] == 5 &&
                  kSchedule[11] == 6 && kSchedule[13] == 7 && kMonoGroup == 7 && kWindows == 14 && kDiscardFrames == 30 && kWindowUs == 2000000u &&
                  std::strcmp(groupName(0), "off") == 0 && std::strcmp(groupName(2), "camera") == 0 && std::strcmp(groupName(6), "other") == 0 &&
                  std::strcmp(groupName(7), "mono") == 0 && std::strcmp(groupName(8), "?") == 0,
              "the schedule's codes are the probe's (1 all, 2 camera, 3 ui, 4 sky, 5 sizes, 6 other) and this half's 7 (mono), 14 windows of 2.0 s, 30 frames discarded");
        bool offBetween = true;
        for (unsigned i = 0; i < kWindows; ++i) offBetween = offBetween && ((i % 2 == 0) == (kSchedule[i] == 0));
        check(offBetween, "...every group has an off window on each side: off at every even slot, a group at every odd one, mono last");
        check(s.lines.count("cull cycle: ") == 15 + 7, "15 window lines, and the seven pair lines once the 15th window has closed");
    }
    {
        // The boundary: a window closes at the first frame boundary at least 2.0 s after it opened, not before.
        Session s;
        const Counts counts = [](uint32_t g, unsigned, uint32_t d[2], uint64_t ix[2]) { scriptCounts(g, 0, d, ix); };
        s.step(counts);   // the first boundary opens window 1
        for (unsigned i = 0; i < 199; ++i) s.step(counts);
        check(s.cycle.windowsClosed() == 0 && s.lines.v.empty(), "10 ms frames: 199 frames after the opening (1.99 s) have closed nothing");
        s.step(counts);
        check(s.cycle.windowsClosed() == 1 && s.lines.v.size() == 1, "...the 200th (2.00 s) closes window 1");
        Session q;
        q.periodUs = 16600;
        q.step(counts);
        unsigned frames = 0;
        while (q.cycle.windowsClosed() < 1 && frames < 1000) { q.step(counts); ++frames; }
        check(frames == 121, "16.6 ms frames: the window closes on the first boundary past 2.0 s (the 121st, 2.009 s), not the 120th (1.992 s)");
        const uint64_t secondStart = q.t - q.periodUs;
        unsigned again = 0;
        while (q.cycle.windowsClosed() < 2 && again < 1000) { q.step(counts); ++again; }
        check(again == 121 && q.t - q.periodUs - secondStart >= kWindowUs, "...and the next window is timed from that boundary, not from the first one's opening");
    }
    // ---- the discard, and per-eye accumulation ----------------------------------------------------------------------------------
    {
        Session s;
        // The first 30 frames of a window count 999 / 7 draws; the 31st 40 / 9; the rest 10 / 9. Only the discard decides the mean.
        const Counts counts = [](uint32_t, unsigned frame, uint32_t d[2], uint64_t ix[2]) {
            d[0] = frame <= 30 ? 999u : frame == 31 ? 40u : 10u;
            d[1] = frame <= 30 ? 7u : 9u;
            ix[0] = static_cast<uint64_t>(d[0]) * 2304u;
            ix[1] = static_cast<uint64_t>(d[1]) * 2304u;
        };
        s.stepWindows(1, counts);
        check(s.lines.v.size() == 1 &&
                  s.lines.v[0] == "cull cycle: off window 1: frames 170, terrain draws L/R mean 10.18/9.00, indices L/R mean 23446.6/20736.0, head 0.0 deg/s",
              "the first 30 frames of a window are discarded: 170 of 200 counted, the 31st in, and each eye's means are its own (the exact line)");
        s.stepWindows(1, counts);
        check(s.lines.v.size() == 2 && s.lines.v[1].find("all window 2: frames 170, terrain draws L/R mean 10.18/9.00") != std::string::npos,
              "...every window discards its own first 30 frames");
    }
    // ---- head speed ----------------------------------------------------------------------------------------------------------------
    {
        Session s;
        s.yawRate = [](uint64_t window) { return window == 0 ? 30.0 : window == 1 ? 90.0 : -1.0; };
        const Counts counts = [](uint32_t g, unsigned, uint32_t d[2], uint64_t ix[2]) { scriptCounts(g, 0, d, ix); };
        s.stepWindows(3, counts);
        check(s.lines.v.size() == 3 && s.lines.v[0].find(", head 30.0 deg/s") != std::string::npos && s.lines.v[1].find(", head 90.0 deg/s") != std::string::npos &&
                  s.lines.v[2].find(", head 0.0 deg/s") != std::string::npos,
              "the head speed is the mean turn between frames over the counted frames: 30 and 90 deg/s read as such, no pose reads 0");
        const float a[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        float b[12] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};   // 90 degrees about y
        check(closeTo(rotationDegrees(a, b), 90.0, 1e-9) && closeTo(rotationDegrees(a, a), 0.0, 1e-12), "rotationDegrees: 90 degrees about y reads 90, no turn reads 0");
        const double tiny = 0.03 * 3.14159265358979323846 / 180.0;
        const float c[12] = {static_cast<float>(std::cos(tiny)), 0, static_cast<float>(std::sin(tiny)), 0, 0, 1, 0, 0,
                             static_cast<float>(-std::sin(tiny)), 0, static_cast<float>(std::cos(tiny)), 0};
        check(closeTo(rotationDegrees(a, c), 0.03, 0.001), "...and a turn of 0.03 degrees, a frame's worth, survives float poses (the sine, not the cosine)");
    }
    // ---- the pairs at the end of a cycle -----------------------------------------------------------------------------------------
    {
        Session s;
        const Counts counts = [](uint32_t g, unsigned, uint32_t d[2], uint64_t ix[2]) { scriptCounts(g, 0, d, ix); };
        s.stepWindows(14, counts);
        check(s.lines.count("cull cycle: cycle ") == 0, "after the 14th window nothing is paired yet: 'mono' waits for the next cycle's first off window");
        s.stepWindows(1, counts);
        check(s.lines.has("cull cycle: cycle 1, all vs off: draws L/R +6.00/+4.00, indices L/R +13824.0/+9216.0 (windows 1, 2, 3)") &&
                  s.lines.has("cull cycle: cycle 1, camera vs off: draws L/R +0.00/+0.00, indices L/R +0.0/+0.0 (windows 3, 4, 5)") &&
                  s.lines.has("cull cycle: cycle 1, ui vs off: draws L/R +2.00/+0.00, indices L/R +4608.0/+0.0 (windows 5, 6, 7)") &&
                  s.lines.has("cull cycle: cycle 1, sky vs off: draws L/R +0.00/+0.00, indices L/R +0.0/+0.0 (windows 7, 8, 9)") &&
                  s.lines.has("cull cycle: cycle 1, sizes vs off: draws L/R +4.00/+3.00, indices L/R +9216.0/+6912.0 (windows 9, 10, 11)") &&
                  s.lines.has("cull cycle: cycle 1, other vs off: draws L/R +1.00/+0.00, indices L/R +2304.0/+0.0 (windows 11, 12, 13)") &&
                  s.lines.has("cull cycle: cycle 1, mono vs off: draws L/R +3.00/+1.00, indices L/R +6912.0/+2304.0 (windows 13, 14, 15)"),
              "the 15th window closes the cycle: each group's mean less its two neighbouring off windows, per eye, for draws and indices; 'mono' pairs with the next off");
        check(s.lines.count("cull cycle: cycle 1, ") == 7 && s.lines.count("cull cycle: cycle 1 counted no") == 0, "...seven lines, once");
        s.stepWindows(14, counts);
        check(s.lines.count("cull cycle: cycle 2, ") == 7, "the next cycle is paired when the cycle after it opens");
    }
    {
        // Frames too long for a window to reach its 31st: nothing is counted and the pairs say so, instead of writing zeros.
        Session s;
        s.periodUs = 100000;
        const Counts counts = [](uint32_t g, unsigned, uint32_t d[2], uint64_t ix[2]) { scriptCounts(g, 0, d, ix); };
        s.stepWindows(15, counts);
        check(s.lines.v[0].find("frames 0,") != std::string::npos && s.lines.has("cull cycle: cycle 1, all vs off: n/a (a window had no counted frames)") &&
                  s.lines.count("cull cycle: cycle 1, ") >= 6,
              "windows with no counted frames pair as n/a");
        Session z;
        const Counts none = [](uint32_t, unsigned, uint32_t d[2], uint64_t ix[2]) { d[0] = d[1] = 0; ix[0] = ix[1] = 0; };
        z.stepWindows(15, none);
        check(z.lines.has("cull cycle: cycle 1 counted no planet-terrain draws in any window (not over terrain, or no scene depth pair yet)"),
              "a cycle that counted no terrain draw at all says so, rather than reporting a flat zero as a result");
    }
    // ---- the driver: what a request comes to --------------------------------------------------------------------------------------
    {
        check(statusFor(false, false, true) == Status::Idle && statusFor(false, true, false) == Status::Idle && statusFor(true, true, true) == Status::IgnoredGuard &&
                  statusFor(true, true, false) == Status::IgnoredGuard && statusFor(true, false, false) == Status::StoodDownBuild && statusFor(true, false, true) == Status::Running,
              "the request: the guard wins over a wrong build, off is idle");
        Driver d;
        Lines lines;
        float pose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        check(d.frame(false, false, false, true, 3, 1000000, pose, lines) == 0 && lines.v.empty() && !counting(), "no request: group 0, nothing logged, nothing counted");
        check(d.frame(true, false, true, true, 3, 1010000, pose, lines) == 0 && lines.v.size() == 1 && lines.v[0] == "cull cycle: ignored while the cull guard runs" && !counting(),
              "requested with a cull guard configured: group 0, one line saying it is ignored, nothing counted");
        d.frame(true, false, true, true, 3, 1020000, pose, lines);
        check(lines.v.size() == 1, "...said once");
        check(d.frame(true, false, false, false, 3, 1030000, pose, lines) == 0 && lines.v.size() == 2 && lines.v[1] == "cull cycle: standing down -- not build 332841" && !counting() &&
                  d.status() == Status::StoodDownBuild,
              "requested on another build: group 0, one line saying it stands down, nothing counted");
        check(d.frame(true, false, false, true, 3, 1040000, pose, lines) == 0 && counting() && d.status() == Status::Running && lines.v.size() == 3 &&
                  lines.v[2].rfind("cull cycle: running -- 2.0 s windows, off, all, off, camera, off, ui, off, sky, off, sizes, off, other", 0) == 0,
              "requested on build 332841 with no guard: it runs, says so, and counts; the first frame is off");
        // The render thread's counts reach the window through the frame boundary and nowhere else.
        for (int i = 0; i < 40; ++i) { noteTerrainDraw(0, 2304u, 1u); }
        for (int i = 0; i < 30; ++i) { noteTerrainDraw(1, 1000u, 3u); }
        noteTerrainDraw(2, 5u, 1u); noteTerrainDraw(-1, 5u, 1u);
        check(g_draws[0].load() == 40 && g_draws[1].load() == 30 && g_indices[0].load() == 40u * 2304u && g_indices[1].load() == 30u * 3000u,
              "noteTerrainDraw counts a draw and its index count times its instances per eye, and ignores a draw that is on neither eye");
        d.frame(true, false, false, true, 3, 1050000, pose, lines);
        check(g_draws[0].load() == 0 && g_draws[1].load() == 0 && g_indices[0].load() == 0, "...and the boundary takes them");
        d.frame(false, false, false, true, 3, 1060000, pose, lines);
        check(lines.v.back() == "cull cycle: stopped after 0 windows" && !counting() && d.status() == Status::Idle, "the key going back to off logs the stop and stops counting");
    }
    // ---- measure: the same counting with no lies, labelled by the cull guard's stage (the positive control) ------------------------
    {
        Driver d;
        Lines lines;
        float pose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        uint64_t t = 1000000;
        const auto frames = [&](unsigned n, uint32_t stage, uint32_t left, uint32_t right, bool* lied) {
            for (unsigned i = 0; i < n; ++i) {
                for (uint32_t k = 0; k < left; ++k) noteTerrainDraw(0, 2304u, 1u);
                for (uint32_t k = 0; k < right; ++k) noteTerrainDraw(1, 2304u, 1u);
                const uint32_t group = d.frame(false, true, true, false, stage, t, pose, lines);
                if (lied && group != 0) *lied = true;
                t += 10000;
            }
        };
        bool lied = false;
        frames(1, 3, 0, 0, &lied);
        check(d.status() == Status::Measuring && counting() && lines.v.size() == 1 && lines.v[0].rfind("cull cycle: measuring -- 2.0 s windows labelled by the cull guard's stage", 0) == 0 &&
                  d.cycle().measuring(),
              "measure runs with a cull guard configured and on another build: it tells no lie, so it has nothing to stand down for, and it says that it is measuring");
        frames(200, 3, 16, 14, &lied);
        check(lines.has("cull cycle: measure[guard live] window 1: frames 170, terrain draws L/R mean 16.00/14.00, indices L/R mean 36864.0/32256.0, head 0.0 deg/s") && d.cycle().windowsClosed() == 1,
              "a window is labelled by the cull guard's stage, and counted as a cycle's is (the exact line)");
        // The stage changes under a window: that window is dropped, and the change is said.
        frames(100, 3, 16, 14, &lied);
        frames(1, 2, 12, 10, &lied);
        check(lines.has("cull cycle: the cull guard went from live to adopting (the window in progress is dropped)") && d.cycle().windowsClosed() == 1 && d.cycle().stage() == 2,
              "a stage change under a window drops it and says so, and does not close it");
        frames(199, 2, 12, 10, &lied);
        check(d.cycle().windowsClosed() == 1, "...the new stage's window runs its full 2.0 s from the change");
        frames(1, 2, 12, 10, &lied);
        check(lines.has("cull cycle: measure[guard adopting] window 2: frames 170, terrain draws L/R mean 12.00/10.00, indices L/R mean 27648.0/23040.0, head 0.0 deg/s"),
              "...and is numbered after the windows that closed");
        frames(201, 1, 10, 9, &lied);   // the first call of each is the change itself, which drops the window
        frames(201, 0, 10, 9, &lied);
        check(lines.has("cull cycle: measure[guard waiting] window 3: frames 170, terrain draws L/R mean 10.00/9.00, indices L/R mean 23040.0/20736.0, head 0.0 deg/s") &&
                  lines.has("cull cycle: measure[guard off] window 4: frames 170, terrain draws L/R mean 10.00/9.00, indices L/R mean 23040.0/20736.0, head 0.0 deg/s"),
              "off, waiting, adopting and live each label their windows");
        check(!lied, "...and measure never tells the runtime anything but group 0, not at a window boundary either");
        check(lines.count("cull cycle: cycle ") == 0, "...and writes no pairs");
        const size_t before = lines.v.size();
        d.frame(false, false, false, true, 0, t, pose, lines);
        check(lines.v.size() == before + 1 && lines.v.back() == "cull cycle: stopped after 4 windows" && !counting() && d.status() == Status::Idle, "the key going back to off stops it, counting the windows");
        // A cycle after a measure run starts clean, and a measure after a cycle too.
        check(d.frame(true, false, false, true, 0, t + 10000, pose, lines) == 0 && d.status() == Status::Running && !d.cycle().measuring() && d.cycle().windowsClosed() == 0,
              "a cycle after a measure run starts from window 1 as a cycle");
        d.frame(false, true, false, true, 0, t + 20000, pose, lines);
        check(d.status() == Status::Measuring && d.cycle().measuring() && lines.v[lines.v.size() - 2] == "cull cycle: stopped after 0 windows",
              "...and a measure run after a cycle as a measure run");
        d.frame(false, false, false, true, 0, t + 30000, pose, lines);
    }
    // ---- the mono camera's windows: the read count, the steady mode ----------------------------------------------------------------
    {
        // The count rides the same discard as the draws: 7 reads a frame in the 30 discarded, 2 in the 170 counted -> 340, not 400.
        Cycle c;
        Lines lines;
        uint64_t t = 1000000;
        float pose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const uint32_t d[2] = {10, 9};
        const uint64_t ix[2] = {23040, 20736};
        unsigned frame = 0;
        while (c.windowsClosed() < 1 && frame < 1000) {
            ++frame;
            c.beginFrame(t, pose, d, ix, lines, 0, frame <= 31 ? 7u : 2u);
            t += 10000;
        }
        check(lines.v.size() == 1 && lines.v[0] == "cull cycle: off window 1: frames 170, terrain draws L/R mean 10.00/9.00, indices L/R mean 23040.0/20736.0, head 0.0 deg/s, mono reads 340",
              "a window with the mono hook live writes ', mono reads N', the total over its counted frames only (the discarded ones do not add)");
        Cycle plain;
        Lines plainLines;
        t = 1000000;
        frame = 0;
        while (plain.windowsClosed() < 1 && frame < 1000) {
            ++frame;
            plain.beginFrame(t, pose, d, ix, plainLines);
            t += 10000;
        }
        check(plainLines.v.size() == 1 && plainLines.v[0].find("mono") == std::string::npos && kNoMonoReads == 0xFFFFFFFFu,
              "...and without the hook (no count passed) the line has no such field");
        // Through the driver: live reads come from the counter, and the hook not being live hides them.
        Driver d1;
        Lines l1;
        g_monoHookLive.store(true);
        t = 5000000;
        for (unsigned i = 0; i < 202; ++i) {
            for (int k = 0; k < 5; ++k) noteMonoRead();
            d1.frame(true, false, false, true, 3, t, pose, l1);
            t += 10000;
        }
        check(l1.count("cull cycle: off window 1: frames 170,") == 1 && l1.has("cull cycle: off window 1: frames 170, terrain draws L/R mean 0.00/0.00, indices L/R mean 0.0/0.0, head 0.0 deg/s, mono reads 850"),
              "the driver takes the mono read counter at each frame boundary: 5 a frame over 170 counted frames reads 850");
        g_monoHookLive.store(false);
        for (int k = 0; k < 5; ++k) noteMonoRead();
        d1.frame(false, false, false, true, 3, t, pose, l1);
        check(g_monoReads.load() == 0, "a change of status clears the counter, so a run never starts with the last run's reads");
        for (int k = 0; k < 3; ++k) noteMonoRead();
        d1.frame(true, false, false, true, 3, t + 10000, pose, l1);
        check(g_monoReads.load() == 0, "...and a run's first boundary takes what was counted");
        d1.frame(false, false, false, true, 3, t + 20000, pose, l1);
        g_monoReads.store(0);
        // The steady form: `advanced.cull_probe = mono` on every frame, with the cycle's gates and no windows.
        SteadyMono s;
        Lines sl;
        check(!s.frame(false, false, true, sl) && sl.v.empty() && s.status() == Status::Idle, "mono steady, not requested: no lie, nothing logged");
        check(s.frame(true, false, true, sl) && sl.v.size() == 1 && sl.v[0] == "cull probe: mono -- the mono camera's aspect is multiplied by 1.30 on every frame" && s.status() == Status::Running,
              "requested on build 332841 with no cull guard: the lie is on, and the log says so once");
        s.frame(true, false, true, sl);
        check(sl.v.size() == 1, "...said once");
        check(!s.frame(true, true, true, sl) && sl.v.back() == "cull probe: mono ignored while the cull guard runs" && s.status() == Status::IgnoredGuard,
              "with a cull guard configured the lie is off, and it says it is ignored (a guard wins over a wrong build too)");
        check(!s.frame(true, true, false, sl) && sl.v.size() == 2 && s.status() == Status::IgnoredGuard, "...a guard and another build: still the guard's line, not a second one");
        check(!s.frame(true, false, false, sl) && sl.v.back() == "cull probe: mono standing down -- not build 332841" && s.status() == Status::StoodDownBuild,
              "on another build with no guard the lie is off and it stands down");
        check(s.frame(true, false, true, sl) && sl.v.size() == 4 && s.status() == Status::Running, "...and back on build 332841 it runs again, saying so");
        check(!s.frame(false, false, true, sl) && sl.v.back() == "cull probe: mono stopped" && s.status() == Status::Idle,
              "the key going back to off stops the lie and logs it");
        s.frame(false, false, true, sl);
        check(sl.v.back() == "cull probe: mono stopped" && sl.v.size() == 5, "...once");
        SteadyMono quiet;
        Lines ql;
        quiet.frame(true, true, true, ql);
        quiet.frame(false, true, true, ql);
        check(ql.v.size() == 1, "a request that never ran says nothing when it goes away (no 'stopped' for a lie that was never on)");
    }
    // ---- the fixture the log tool reads ----------------------------------------------------------------------------------------------
    {
        const std::string built = fixtureLog();
        std::string fixture;
        if (FILE* f = std::fopen("tools/cull_cycle_fixture.log", "rb")) {
            char buffer[4096];
            size_t n;
            while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) fixture.append(buffer, n);
            std::fclose(f);
        }
        std::string normalised;
        for (char ch : fixture) if (ch != '\r') normalised += ch;
        check(!fixture.empty(), "tools/cull_cycle_fixture.log is readable from the repo root");
        check(normalised == built,
              "the log tool's fixture file is what the writer writes for the scripted flight, byte for byte (python tools\\edvr_log.py --tally cull reads it; regenerate with --print-cull-fixture)");
        check(built.find("cull cycle: running --") != std::string::npos && built.find("cull cycle: all window 2: frames 170, terrain draws L/R mean 16.00/13.00") != std::string::npos &&
                  built.find("cull cycle: ui window 6: frames 170") != std::string::npos && built.find("cull cycle: cycle 2, all vs off: draws L/R +8.00/+4.00") != std::string::npos &&
                  built.find("cull cycle: stopped after 29 windows") != std::string::npos &&
                  built.find("cull cycle: mono window 14: frames 170, terrain draws L/R mean 13.00/10.00") != std::string::npos &&
                  built.find(", head 3.0 deg/s, mono reads 510") != std::string::npos && built.find(", head 3.0 deg/s, mono reads 340") != std::string::npos &&
                  built.find("cull probe: mono windows multiply the mono camera's aspect by 1.30 (hook live)") != std::string::npos &&
                  built.find("mono camera: aspect getter called from exe+0x2871D89 (caller 2) -- the mono filler") != std::string::npos &&
                  built.find("mono camera: aspect writer call 2: width 0, height 0, min aspect 0.000000, out unreadable, return exe+0x2871D10") != std::string::npos &&
                  built.find("cull probe: mono stopped") != std::string::npos,
              "...and it carries the lines of every class: running, windows, both cycles' pairs, stopped");
    }
}
}  // namespace cull_cases
