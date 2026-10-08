// gate_test -- the rig for the pure decisions and file work that sit under the
// per-frame machinery: the clock helpers (src/common/timing.h), the journal
// watcher and its worker thread, which journal is ours, periodic-work timing,
// the notes as they reach a real log, the frame-boundary ticks' structure, and
// the eye-shape recogniser rule.
//
// It is named for the head-offset gate it was written for. That gate -- the
// press-counting decision machine behind the old Explorer Cam -- was deleted on
// 2026-10-07 with the rest of the old route, together with its frame-feed
// scenarios. What is left here is the other half of the rig, which has its own
// reasons (each section says them) and kept the name so the build and the
// docs that point at it stay valid.
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/guard.h"
#include "../../src/common/periodic_work.h"
#include "../../src/common/timing.h"
#include "../../src/common/log.h"
// Header-only as well (guard.h and frame_ticks.h): one fault budget per frame-
// boundary tick, the class device_hook.cpp and vscreen.cpp declare theirs with.
#include "../../src/d3d11/boundary_tick.h"
// Linked into this fixture already; the pure decision it exposes,
// journalPickNewest, had no coverage at all until the review rounds on
// issue #19 found arithmetic bugs in it (and in a sibling decision since
// removed), every one of which is one line of table here.
#include "../../src/d3d11/journal_watch.h"
// Same reason: eyeShapedAtScale is the recogniser rule that decides whether
// the gate is fed at all, and it is inline in the header so this file can
// assert it against the sizes a real rig produced.
#include "../../src/d3d11/vscreen.h"

using namespace edvr;

namespace {

int g_bad = 0;

// A FAKE CLOCK for the checks that walk durations (src/common/timing.h):
// elapsedMs, dueMs and stampMs read it through g_clockForTest, so a check sets
// g_fakeMs and no check sleeps.
uint64_t g_fakeMs = 0;

uint64_t fakeClock() { return g_fakeMs; }

// Counted here rather than written into the summary by hand. That number has
// been wrong twice already -- it is exactly the kind of thing that drifts
// silently, and a test that misreports how much it checked is halfway to a test
// that checks nothing.
int g_checks = 0;

// ------------------------------------------------- what counts as an eye
//
// Whether a draw is going into an eye texture at all decides what every
// per-draw fix is fed. Its count starves if the recogniser misses the eyes,
// and on 2026-08-19 a rig starved it by rendering the world at a scale --
// 1626x1774 into a 2112x2304 the headset was handed. eyeShapedAtScale is the
// rule that recognises that case, and it is a shape test with a tolerance,
// which is exactly the kind of thing that drifts.
//
// Every size below is from that session's own log, so this is the real list a
// real rig offered: one of them is the world and six are not.
int eyeShapeChecks() {
    struct Case { uint32_t w, h; bool want; const char* what; };
    const uint32_t eyeW = 2112, eyeH = 2304;
    const Case cases[] = {
        {1626, 1774, true,  "the world, rendered at 77% and scaled up (the field case)"},
        {2048, 2048, false, "a square atlas, 9% off the eye's shape"},
        {1024, 1024, false, "a smaller square, same 9%"},
        {1920, 1080, false, "16:9 -- the panel's stock size"},
        {1791, 1007, false, "16:9 again, at an odd size"},
        {1280,  768, false, "5:3"},
        {1024,  512, false, "2:1"},
        // The bounds themselves, which the list above does not reach.
        {2112, 2304, true,  "the published size, which the exact test takes first"},
        // The band's edges, named as edges. 40% and 250% are IN, because a
        // comment that says "40% to 250%" is read as inclusive by whoever
        // changes this next, and a boundary nobody asserts is a boundary that
        // moves.
        { 845,  922, true,  "the eye's shape at exactly 40% -- the bottom of the band"},
        {5280, 5760, true,  "the eye's shape at exactly 250% -- the top of it"},
        { 803,  875, false, "the eye's shape at 38% -- under the band"},
        {5491, 5990, false, "the eye's shape at 260% -- over it"},
        {   0,    0, false, "nothing"},
    };
    int bad = 0;
    for (const Case& c : cases) {
        const bool got = eyeShapedAtScale(c.w, c.h, eyeW, eyeH);
        ++g_checks;
        if (got == c.want) continue;
        printf("  FAIL  %ux%u %s -- eyeShapedAtScale said %s\n", c.w, c.h, c.what,
               got ? "yes" : "no");
        ++bad;
    }
    // A headset that has published nothing yet cannot answer this, and must
    // not answer it by dividing by zero.
    ++g_checks;
    if (eyeShapedAtScale(1626, 1774, 0, 0)) {
        printf("  FAIL  eyeShapedAtScale answered yes with no published eye size\n");
        ++bad;
    }
    if (!bad) printf("  ok    the world at a render scale is an eye; six other shapes are not\n");
    return bad;
}

// WHICH JOURNAL IS OURS, as a table.
//
// The decision that read a crashed session's journal as this session's and
// announced gameplay 0.1s into a process still sitting at the launcher
// (issue #19). Times are FILETIME units, stated relative to notBefore because
// that is what the comparison is against.
int journalPickChecks() {
    int bad = 0;
    constexpr uint64_t kSec = 10000000ull;
    constexpr uint64_t N = 1000000ull * kSec;   // notBefore: our start, less slack

    auto want = [&](const char* what, const uint64_t* cre, const uint64_t* wr,
                    size_t count, int index, bool ours) {
        ++g_checks;
        const JournalPick got = journalPickNewest(cre, wr, count, N);
        if (got.index == index && got.ours == ours) return;
        printf("  FAIL  %s -- got index=%d ours=%d, expected index=%d ours=%d\n",
               what, got.index, got.ours ? 1 : 0, index, ours ? 1 : 0);
        ++bad;
    };

    // Nothing at all, and nothing live: both resolve to "do not know" rather
    // than to a file.
    want("an empty folder picks nothing", nullptr, nullptr, 0, -1, false);
    {
        const uint64_t cre[] = {N - 3600 * kSec};
        const uint64_t wr[] = {N - 60 * kSec};
        want("a journal untouched since we started is not a candidate", cre, wr, 1,
             -1, false);
    }

    // THE ISSUE #19 CASE. [0] is the crashed session's journal: created 27
    // minutes ago, written 40 seconds after notBefore because the crash landed
    // inside the slack. [1] is ours: created moments ago and barely written, so
    // it LOSES on recency and must win anyway.
    {
        const uint64_t cre[] = {N - 1620 * kSec, N + 32 * kSec};
        const uint64_t wr[] = {N + 40 * kSec, N + 33 * kSec};
        want("our journal beats a crashed session's, despite the write times", cre,
             wr, 2, 1, true);

        // The same two in the order the walk might equally have found them.
        // Provenance must not depend on what FindFirstFile returns first.
        const uint64_t creR[] = {N + 32 * kSec, N - 1620 * kSec};
        const uint64_t wrR[] = {N + 33 * kSec, N + 40 * kSec};
        want("...and the same whichever order the walk found them in", creR, wrR, 2,
             0, true);
    }

    // BEFORE OURS EXISTS the foreign one is still adopted -- live events from it
    // are worth having -- but `ours` is false, which is what makes the caller
    // tail it from the end rather than replay its history.
    {
        const uint64_t cre[] = {N - 1620 * kSec};
        const uint64_t wr[] = {N + 40 * kSec};
        want("a foreign journal is adopted, but not as ours", cre, wr, 1, 0, false);
    }

    // THE RUNNING BEST WRITE TIME GOES BACKWARDS when a born file displaces a
    // non-born one, and this is what proves that is harmless: the non-born file
    // carries an enormous write time, and the two born files must still be
    // compared against each other rather than against it.
    {
        const uint64_t cre[] = {N - 1620 * kSec, N + 5 * kSec, N + 6 * kSec};
        const uint64_t wr[] = {N + 9000 * kSec, N + 10 * kSec, N + 20 * kSec};
        want("a stale write time from the other class is never consulted", cre, wr,
             3, 2, true);
    }

    // Equal provenance falls back to recency, and that comparison is strict, so
    // a tie keeps the first -- a stable answer rather than one that flips
    // between reglobs.
    {
        const uint64_t cre[] = {N + 5 * kSec, N + 6 * kSec};
        const uint64_t wr[] = {N + 20 * kSec, N + 20 * kSec};
        want("an exact tie on write time keeps the first, stably", cre, wr, 2, 0,
             true);
    }

    if (!bad) {
        printf("  ok    the journal pick prefers provenance over recency, in any "
               "order, and a crashed session's journal never wins\n");
    }
    return bad;
}

// PHASE-0 TIMING FOR PERIODIC WORK (src/common/periodic_work.h), as a table.
//
// It is tested here because this rig already links the biggest operation it
// times (journal_watch.cpp), the real log and the guard, and is where the
// cadence arithmetic (timing.h) is already asserted -- so no build step changes.
// What is under test is the window and threshold logic, which is pure: the
// clock, the local time of day and the log all arrive as arguments, so a fake
// clock walks whole 30 s windows in microseconds and the rig can count exactly
// how often the local clock was read.
int periodicWorkChecks() {
    int bad = 0;
    auto verify = [&](bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        printf("  FAIL  %s\n", what);
        ++bad;
    };
    auto verifyText = [&](const std::string& got, const char* want, const char* what) {
        ++g_checks;
        if (got == want) return;
        printf("  FAIL  %s\n          got:      %s\n          expected: %s\n", what,
               got.c_str(), want);
        ++bad;
    };

    constexpr int64_t kFreq = 10000000;   // 10 MHz, a usual QueryPerformanceFrequency
    std::vector<std::string> lines;       // everything the operation wrote
    int wallCalls = 0;                    // how often the local clock was read
    PeriodicWallClock wall;               // what the fake local clock answers
    auto setWall = [&](unsigned h, unsigned m, unsigned s, unsigned ms) {
        wall.hour = h;
        wall.minute = m;
        wall.second = s;
        wall.millis = ms;
    };
    auto reset = [&] {
        lines.clear();
        wallCalls = 0;
    };
    // One finished run of `ticks` (10,000 ticks = 1 ms here), completed at t.
    auto run = [&](PeriodicWork& w, int64_t ticks, double atSeconds, uint64_t context = 0) {
        w.record(ticks, static_cast<int64_t>(atSeconds * static_cast<double>(kFreq)), kFreq,
                 context, [&] { ++wallCalls; return wall; },
                 [&](const char* line) { lines.push_back(line); });
    };

    // A QUIET OPERATION writes one summary per 30 s window and nothing before it.
    {
        reset();
        PeriodicWork w("op_quiet");
        setWall(12, 34, 56, 789);
        for (int i = 0; i < 30; ++i) run(w, 5000, i);   // 0.5 ms once a second, t = 0..29 s
        verify(lines.empty(), "nothing is written while the window is still open");
        run(w, 5000, 30);   // the 31st run, exactly 30 s after the window's first
        verify(lines.size() == 1, "the first run at or past 30 s closes the window with one summary");
        if (lines.size() == 1) {
            verifyText(lines[0],
                       "periodic work: op_quiet n=31 total=15.500 max=0.500 at 12:34:56.789 "
                       "slow=0",
                       "the summary's exact form");
        }
        verify(wallCalls == 1,
               "31 unremarkable runs read the local clock once, for the window's first");
        verify(w.runs() == 0, "a closed window starts empty");
    }

    // THE LOCAL CLOCK IS READ ONLY FOR A NEW MAXIMUM OR A SLOW RUN, and once
    // when a run is both. The threshold is exact: 20,000 ticks is 2.0 ms.
    {
        reset();
        PeriodicWork w("op_wall");
        struct Step {
            int64_t ticks;
            int calls;   // total local-clock reads expected after this run
            const char* why;
        };
        const Step steps[] = {
            {10000, 1, "the first run of a window is its maximum, so it reads the clock"},
            {9000, 1, "a shorter run does not"},
            {8000, 1, "nor does another"},
            {15000, 2, "a new maximum does"},
            {15000, 2, "a tie is not a new maximum"},
            {19999, 3, "1.9999 ms is a new maximum, and is not slow"},
            {20000, 4, "2.0000 ms is slow AND a new maximum, and reads the clock once for both"},
            {20000, 5, "a slow run that is not a maximum still needs its time of day"},
        };
        double at = 1.0;
        for (const Step& step : steps) {
            run(w, step.ticks, at);
            at += 1.0;
            verify(wallCalls == step.calls, step.why);
        }
        verify(w.slowRuns() == 2, "2.0000 ms is slow and 1.9999 ms is not");
        verify(lines.size() == 1,
               "the second slow run, a second after the first, is counted but not written");
    }

    // SLOW RUNS: written at once, at most one line per operation per 10 s, each
    // carrying its own context; and the summary's time and context are the
    // SLOWEST run's, not the last one's.
    {
        reset();
        PeriodicWork w("journal_reglob", "files");
        setWall(10, 0, 0, 100);
        run(w, 34120, 100.0, 1893);   // 3.412 ms: the window's maximum
        setWall(10, 0, 5, 100);
        run(w, 25000, 105.0, 9);      // 2.5 ms, five seconds on: counted, not written
        setWall(10, 0, 9, 900);
        run(w, 25000, 109.9, 9);      // 9.9 s after the last WRITTEN line: still not
        setWall(10, 0, 10, 100);
        run(w, 25000, 110.0, 9);      // exactly 10 s after it: written
        verify(lines.size() == 2, "slow lines are limited to one per 10 s, and 10 s is enough");
        if (lines.size() == 2) {
            verifyText(lines[0], "periodic work: journal_reglob SLOW ms=3.412 at 10:00:00.100 files=1893",
                       "the first slow line, with its own context");
            verifyText(lines[1], "periodic work: journal_reglob SLOW ms=2.500 at 10:00:10.100 files=9",
                       "the next one, ten seconds later");
        }
        setWall(10, 0, 30, 0);
        run(w, 1000, 130.0, 9);   // 0.1 ms, thirty seconds after the window opened
        verify(lines.size() == 3, "the window closes on the first run 30 s after it opened");
        if (lines.size() == 3) {
            verifyText(lines[2],
                       "periodic work: journal_reglob n=5 total=11.012 max=3.412 at "
                       "10:00:00.100 slow=4 files=1893",
                       "the summary names the slowest run's time and context, and counts every "
                       "slow run including the withheld ones");
        }

        // ONLY IF IT RAN: the closed window is empty, and the next opens with the
        // next run, so the next summary is 30 s after THAT, not after this one.
        verify(w.runs() == 0, "the window is closed and empty after its summary");
        lines.clear();
        setWall(11, 11, 11, 111);
        run(w, 1000, 131.0, 1);
        run(w, 1000, 160.9, 1);
        verify(lines.empty(), "29.9 s into the next window: nothing");
        run(w, 1000, 161.0, 1);
        verify(lines.size() == 1, "30 s into it: the summary");
        if (lines.size() == 1) {
            verifyText(lines[0],
                       "periodic work: journal_reglob n=3 total=0.300 max=0.100 at "
                       "11:11:11.111 slow=0 files=1",
                       "the second window is its own, not a continuation");
        }
    }

    // STATE IS PER INSTANCE: two operations do not share a slow-line limit or a
    // window.
    {
        reset();
        PeriodicWork a("op_a"), b("op_b");
        setWall(9, 0, 0, 0);
        run(a, 30000, 0.0);   // 3 ms: slow, written
        run(b, 30000, 1.0);   // 3 ms on ANOTHER operation a second later: written too
        verify(lines.size() == 2, "each operation has its own slow-line limit");
        verify(a.runs() == 1 && b.runs() == 1 && a.slowRuns() == 1 && b.slowRuns() == 1,
               "and its own window");
    }

    // A clock that misbehaves must not crash it or manufacture a slow run.
    {
        reset();
        PeriodicWork w("op_odd");
        w.record(-5, 0, 0, 0, [&] { ++wallCalls; return wall; },
                 [&](const char* line) { lines.push_back(line); });
        verify(w.runs() == 1 && w.slowRuns() == 0 && lines.empty(),
               "a negative duration and a zero clock rate are clamped, not believed");
    }

    // THE PRODUCTION BINDING reads the real clock in real milliseconds. With no
    // log open the line it writes goes nowhere, which is fine here: what is
    // asserted is the unit, the mistake a fake clock cannot catch (ticks read
    // as milliseconds looks fine until a flight).
    {
        PeriodicWork real("gate_test_real");
        {
            PeriodicWorkScope scope(real);
            Sleep(20);
        }
        const double gotMs = static_cast<double>(real.maxTicks()) * 1000.0 /
                             static_cast<double>(qpcFrequency());
        verify(real.runs() == 1, "a scope records exactly one run");
        verify(gotMs >= 10.0 && gotMs < 5000.0,
               "a scope around a 20 ms sleep reads about 20, in milliseconds and not ticks");
    }

    if (!bad) {
        printf("  ok    periodic-work timing: a summary per 30 s window and only if it ran, "
               "slow runs written at once and limited to one per 10 s, the local clock read "
               "only for a new maximum or a slow run, and 2.0000 ms exactly slow\n");
    }
    return bad;
}

std::string readWholeFile(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(f);
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        size_t stop = end;
        if (stop > start && text[stop - 1] == '\r') --stop;
        out.push_back(text.substr(start, stop - start));
        start = end + 1;
    }
    return out;
}

// This executable's own file name, which is the module a fault inside the rig's
// own code must be attributed to.
std::string ownModuleLeaf() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* leaf = slash ? slash + 1 : path;
    std::string out;
    for (; *leaf; ++leaf) out.push_back(*leaf < 0x80 ? static_cast<char>(*leaf) : '?');
    return out;
}

// " at HH:MM:SS.mmm" in a periodic-work line, zero-padded as the log's own
// prefix is, so the two can be read side by side.
bool hasTimeOfDay(const std::string& line) {
    const size_t at = line.find(" at ");
    if (at == std::string::npos) return false;
    const std::string t = line.substr(at + 4, 12);
    if (t.size() != 12) return false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i == 2 || i == 5) {
            if (t[i] != ':') return false;
        } else if (i == 8) {
            if (t[i] != '.') return false;
        } else if (t[i] < '0' || t[i] > '9') {
            return false;
        }
    }
    return true;
}

// A body that takes a measurable time on the frame clock, so the mark after it is
// recorded and outranks the stretches around it: a mark that does not advance the
// clock records nothing, and only the three slowest stretches are named
// (frame_ticks.h).
void spendMicroseconds(int64_t us) {
    const int64_t start = FrameTicks::now();
    const int64_t ticks = us * qpcFrequency() / 1000000;
    while (FrameTicks::now() - start < ticks) {}
}

// THE NOTES AS THEY REACH A REAL LOG FILE: where a caught fault happened
// (src/common/guard.h), which frame-boundary tick it was (src/d3d11/
// boundary_tick.h), and the periodic-work lines written through the production
// binding (src/common/periodic_work.h).
//
// The fault half is the case R4 / A-9 asked for. A site is a budget's name, and
// deviceHook.frameBoundary covered the whole frame boundary, so a fault anywhere
// in it said only that name -- and eight faulting frames anywhere stopped every
// tick in it. The location half is one fault at one site; the tick half is a tick
// that faults on every frame between two that do not, through the real
// BoundaryTick, the real guarded() and the real filter. The rig closes the log
// and reads the file back, so what is asserted is the text a reporter would paste.
int loggedNotesChecks() {
    int bad = 0;
    auto verify = [&](bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        printf("  FAIL  %s\n", what);
        ++bad;
    };

    wchar_t tmp[MAX_PATH];
    const DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn == 0 || tn >= MAX_PATH) {
        printf("  FAIL  no temp folder to write the notes' log in\n");
        return 1;
    }
    const std::wstring dir =
        std::wstring(tmp) + L"edvr_gate_test_notes_" + std::to_wstring(GetCurrentProcessId());
    // A log.enabled = 0 in whatever ini this rig was pointed at would turn every
    // assertion below into "the file is empty", which reads as a failure of the
    // code and not of the setup.
    Config::get().set("log.enabled", "1");
    if (!Log::get().open(dir, L"gatenotes")) {
        printf("  FAIL  could not open a log in %ls to read the notes back\n", dir.c_str());
        return 1;
    }

    // One pointer per site: sites are compared by address, as they are in the
    // product, where each is a literal at one call.
    static const char kOk[] = "gate_test/no_fault";
    static const char kAv[] = "gate_test/fault_note";
    static const char kPrivate[] = "gate_test/fault_private";
    static const char kLegacy[] = "gate_test/legacy";

    const bool okRan = guarded(kOk, [] {});
    const uintptr_t lowAddress = 0x10;
    auto writeLow = [lowAddress] { *reinterpret_cast<volatile int*>(lowAddress) = 1; };
    const bool av1 = guarded(kAv, writeLow);
    const bool av2 = guarded(kAv, writeLow);
    const bool av3 = guarded(kAv, writeLow);

    // A fault in code that is in no module: a page EDVR-style code would sit in,
    // holding one illegal instruction. Written, then made executable, so this
    // does not ask for a page that is writable and executable at once.
    bool privateRan = true;
    if (void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
        const unsigned char ud2[] = {0x0F, 0x0B};
        memcpy(page, ud2, sizeof(ud2));
        DWORD previous = 0;
        VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &previous);
        privateRan = guarded(kPrivate, [page] { reinterpret_cast<void (*)()>(page)(); });
        VirtualFree(page, 0, MEM_RELEASE);
    }

    // A caller that reaches the filter with only a code, as the three rigs that
    // stub it do not but a future caller might.
    const int legacyVerdict = guardFilter(0xC0000005UL, kLegacy);

    // THE FRAME BOUNDARY, ONE TICK FAULTING ON EVERY FRAME.
    //
    // Three ticks in the order a frame runs them: one before, one that faults each
    // time it runs, one after. The production ticks are declared with the same
    // macro (device_hook.cpp, vscreen.cpp); only these three are run here.
    EDVR_BOUNDARY_TICK(tickBefore, "gate_test_before");
    EDVR_BOUNDARY_TICK(tickBad, "gate_test_bad");
    EDVR_BOUNDARY_TICK(tickAfter, "gate_test_after");
    int ranBefore = 0, ranBad = 0, ranAfter = 0;
    const int boundaryFrames = kBoundaryTickFaults + 12;
    // What the frame-tick chain recorded: the first frame (the bad tick faulting,
    // so every stretch is timed) and the last (the bad tick long since stood down).
    FrameTickSummary firstFrame, lastFrame;
    for (int f = 0; f < boundaryFrames; ++f) {
        g_frameTicks.enter(FrameTicks::now());
        tickBefore.run([&] { ++ranBefore; spendMicroseconds(200); });
        tickBad.run([&] { ++ranBad; writeLow(); });
        tickAfter.run([&] { ++ranAfter; spendMicroseconds(200); });
        const FrameTickSummary sum =
            g_frameTicks.cut("gate_test_rest", FrameTicks::now(), qpcFrequency());
        if (f == 0) firstFrame = sum;
        if (f == boundaryFrames - 1) lastFrame = sum;
    }
    const bool badDisabled = tickBad.disabled();
    const bool neighboursLive = !tickBefore.disabled() && !tickAfter.disabled();

    // The periodic-work line through the production binding: a real clock, the
    // real local time of day, the real log. A 50 ms window instead of 30 s so
    // the summary can be reached without a half-minute sleep; the slow-run line
    // needs no such help.
    PeriodicWorkPolicy quick;
    quick.summaryEveryMs = 50;
    PeriodicWork realWork("gate_test_e2e", "ctx", quick);
    {
        PeriodicWorkScope scope(realWork, 5);
        Sleep(10);   // >= 2 ms: written the moment the scope closes
    }
    Sleep(60);
    {
        PeriodicWorkScope scope(realWork, 6);   // past the window: the summary
    }

    Log::get().close();

    std::string text;
    {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\edvr_gatenotes_*.log").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring path = dir + L"\\" + fd.cFileName;
                text += readWholeFile(path);
                DeleteFileW(path.c_str());
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    RemoveDirectoryW(dir.c_str());
    const std::vector<std::string> log = splitLines(text);
    auto linesWith = [&](const char* a, const char* b) {
        std::vector<const std::string*> found;
        for (const std::string& line : log) {
            if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) {
                found.push_back(&line);
            }
        }
        return found;
    };

    verify(!log.empty(), "the log file was written and read back");
    verify(okRan, "a block that does not fault runs to the end and reports true");
    verify(!av1 && !av2 && !av3, "a faulting block is absorbed and reports false");
    verify(!privateRan, "so is a fault in code that is in no module");
    verify(legacyVerdict == EXCEPTION_EXECUTE_HANDLER, "the filter's verdict is unchanged");

    const std::string exe = ownModuleLeaf();
    verify(!exe.empty(), "this executable's own name could be read");

    // The access violation: three faults at one site.
    const auto absorbed = linesWith("FAULT ABSORBED", "site=gate_test/fault_note");
    verify(absorbed.size() == 1, "three faults at one site write one FAULT ABSORBED line, as ever");
    if (absorbed.size() == 1) {
        const std::string& n = *absorbed[0];
        verify(n.find("exception=0xC0000005 site=gate_test/fault_note at=0x") != std::string::npos,
               "the note gives the faulting address right after the site");
        verify(n.find(exe + "+0x") != std::string::npos,
               "...names the module the fault is in and an offset into it");
        verify(n.find("access=write data=0x0000000000000010 (no module: not mapped)") !=
                   std::string::npos,
               "...and, for an access violation, what was touched and how");
        verify(n.find("[truncated]") == std::string::npos,
               "...without the line being clipped");
        verify(n.find("THIS DID NOT CRASH THE GAME") != std::string::npos,
               "...and the verdict text is still there");
    }
    const auto totals = linesWith("FAULT TOTAL", "site=gate_test/fault_note");
    verify(totals.size() == 1, "the total is restated at 2 and not at 3");
    if (totals.size() == 1) {
        const std::string& n = *totals[0];
        verify(n.find(": 2 absorbed so far") != std::string::npos, "...and it is the count of 2");
        verify(n.find("Latest fault at=0x") != std::string::npos &&
                   n.find(exe + "+0x") != std::string::npos,
               "...carrying the latest fault's location, so a shared budget shows where it moved");
    }

    // The fault in no module.
    const auto priv = linesWith("FAULT ABSORBED", "site=gate_test/fault_private");
    verify(priv.size() == 1, "a fault in a private page is reported once");
    if (priv.size() == 1) {
        const std::string& n = *priv[0];
        verify(n.find("exception=0xC000001D") != std::string::npos, "...as an illegal instruction");
        verify(n.find("no module: private region 0x") != std::string::npos,
               "...saying there is no module and giving the region it is in");
        verify(n.find("access=") == std::string::npos, "...with no data address, it is not a memory fault");
    }

    // A code-only caller keeps the note it always had.
    const auto legacy = linesWith("FAULT ABSORBED", "site=gate_test/legacy");
    verify(legacy.size() == 1 && legacy[0]->find(" at=") == std::string::npos,
           "a caller with only a code gets the note it always did, without a location");

    // The periodic-work lines, through the real binding and the real log.
    const auto slowLine = linesWith("periodic work: gate_test_e2e SLOW ms=", " ctx=5");
    verify(slowLine.size() == 1, "a slow run through the production binding is written at once");
    if (slowLine.size() == 1) {
        verify(hasTimeOfDay(*slowLine[0]), "...with a zero-padded time of day");
    }
    // Only the shape here: which run is the slowest, and so whose context the
    // summary carries, is decided by real sleeps on a loaded machine. The fake
    // clock cases above pin that exactly.
    const auto summary = linesWith("periodic work: gate_test_e2e n=2 total=", " slow=");
    verify(summary.size() == 1, "the window's summary reaches the log through the real binding");
    if (summary.size() == 1) {
        verify(hasTimeOfDay(*summary[0]), "...with a zero-padded time of day");
        verify(summary[0]->find(" ctx=") != std::string::npos, "...and the operation's context");
    }

    if (!bad) {
        printf("  ok    a caught fault's note names its address, module and offset (and what "
               "an access violation touched), once per site and restated with the latest "
               "location; periodic-work lines reach the log through the real binding\n");
    }

    // The frame boundary's ticks: the faulting one ran exactly its budget and then
    // stood down; the ones around it ran on every frame, including the frames it
    // faulted in and every frame after it stopped.
    const int badAtStart = bad;
    verify(ranBad == kBoundaryTickFaults,
           "a tick that faults on every frame runs exactly its budget of eight, then no more");
    verify(badDisabled, "...and is then reported as stood down");
    verify(ranBefore == boundaryFrames && ranAfter == boundaryFrames,
           "the ticks before and after it ran on every frame, faulting frames included");
    verify(neighboursLive, "...and neither of them was touched by its budget");

    // The timing marks kept working. On the first frame all three stretches were
    // timed (a fault takes far longer than a clock tick) and the chain names all
    // three -- the cut's own stretch is a fourth mark, too short to displace any of
    // them; on the last the bad tick's stretch is nothing, and the two that ran are
    // still named.
    auto named = [](const FrameTickSummary& s, const char* name) {
        for (const FrameTick& t : s.top) {
            if (t.name && strcmp(t.name, name) == 0) return true;
        }
        return false;
    };
    verify(firstFrame.marks >= 3 && named(firstFrame, "gate_test_before") &&
               named(firstFrame, "gate_test_bad") && named(firstFrame, "gate_test_after"),
           "the frame-tick chain recorded and named all three ticks on a frame where one faulted");
    verify(lastFrame.marks >= 2 && named(lastFrame, "gate_test_before") &&
               named(lastFrame, "gate_test_after"),
           "...and still names the two that ran once the third has stood down");

    // The notes name the tick that faulted, once, and no other.
    const auto tickFault = linesWith("FAULT ABSORBED", "site=frameBoundary/gate_test_bad");
    verify(tickFault.size() == 1, "eight faults in one tick write one FAULT ABSORBED line, named for that tick");
    if (tickFault.size() == 1) {
        verify(tickFault[0]->find(" at=0x") != std::string::npos &&
                   tickFault[0]->find(exe + "+0x") != std::string::npos,
               "...which also says where in the module it happened");
    }
    verify(!linesWith("FAULT TOTAL", "site=frameBoundary/gate_test_bad").empty(),
           "...and its running total is restated under the same name");
    const auto disabledLine = linesWith("FEATURE-DISABLED",
                                        "frameBoundary/gate_test_bad exhausted its fault budget");
    verify(disabledLine.size() == 1,
           "the tick that stood down is named on a FEATURE-DISABLED line, once");
    verify(linesWith("frameBoundary/gate_test_before", "").empty() &&
               linesWith("frameBoundary/gate_test_after", "").empty(),
           "no note anywhere names either tick that did not fault");
    if (bad == badAtStart) {
        printf("  ok    a frame-boundary tick that faults on every frame stands down alone after "
               "its own eight: its neighbours run on every frame, the timing marks keep naming "
               "them, and the notes name the faulting tick and no other\n");
    }
    return bad;
}

// THE BOUNDARY'S UNGUARDED SURFACE, AS SOURCE TEXT.
//
// hookedPresent's frame boundary used to sit inside one guarded lambda, so
// anything added to it was under SEH for free. It is presentFrameBoundary() now,
// a list of ticks each on a budget of its own (src/d3d11/boundary_tick.h), and a
// statement added to it OUTSIDE a tick is under nothing at all: the first fault
// in it is the game's crash, not a FEATURE-DISABLED line. The function may hold
// exactly three such statements, documented there -- the frame counter, the
// graphics-off test, and whether the config poll is due -- so this reads the
// function, removes every tick call, and requires that what is left is those three
// and nothing else. It also requires that every tick declared in the two files that
// declare them is run exactly once, that no two share a mark (the mark is the
// budget's name and the LONG FRAME line's), and that the old shared budgets are gone.
std::string withoutLineComments(const std::string& src) {
    std::string out;
    bool inString = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < src.size()) out += src[++i];
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
            out += c;
        } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            out += '\n';
        } else {
            out += c;
        }
    }
    return out;
}

// The index of the ')' that closes the '(' at `open`, skipping string literals.
size_t closingParen(const std::string& t, size_t open) {
    int depth = 0;
    bool inString = false;
    for (size_t i = open; i < t.size(); ++i) {
        const char c = t[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')' && --depth == 0) {
            return i;
        }
    }
    return std::string::npos;
}

std::string withoutSpaces(const std::string& t) {
    std::string out;
    for (char c : t) {
        if (!isspace(static_cast<unsigned char>(c))) out += c;
    }
    return out;
}

int boundarySourceChecks(const std::string& root) {
    int bad = 0;
    auto verify = [&](bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        printf("  FAIL  %s\n", what);
        ++bad;
    };
    const std::wstring base = std::wstring(root.begin(), root.end()) + L"\\src\\d3d11\\";
    const std::string hookText = withoutLineComments(readWholeFile(base + L"device_hook.cpp"));
    const std::string screenText = withoutLineComments(readWholeFile(base + L"vscreen.cpp"));
    if (hookText.empty() || screenText.empty()) {
        printf("  FAIL  could not read src\\d3d11\\device_hook.cpp and vscreen.cpp under %s\n",
               root.c_str());
        return 1;
    }

    // The function, without its braces.
    const size_t head = hookText.find("void presentFrameBoundary() {");
    verify(head != std::string::npos, "device_hook.cpp defines presentFrameBoundary()");
    std::string body;
    if (head != std::string::npos) {
        size_t i = hookText.find('{', head);
        const size_t begin = i + 1;
        int depth = 0;
        bool inString = false;
        for (; i < hookText.size(); ++i) {
            const char c = hookText[i];
            if (inString) {
                if (c == '\\') ++i;
                else if (c == '"') inString = false;
            } else if (c == '"') {
                inString = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}' && --depth == 0) {
                break;
            }
        }
        body = hookText.substr(begin, i - begin);
    }
    const std::string tickBody = body;   // the ticks, before they are taken out below
    // Every tick call out, then what is left.
    size_t pos = 0;
    int tickCalls = 0;
    while ((pos = body.find(".run(", pos)) != std::string::npos) {
        size_t start = pos;
        while (start > 0 && (isalnum(static_cast<unsigned char>(body[start - 1])) ||
                             body[start - 1] == '_')) {
            --start;
        }
        if (body.compare(start, 2, "tk") != 0) {
            ++pos;
            continue;
        }
        const size_t close = closingParen(body, pos + 4);
        if (close == std::string::npos) break;
        size_t end = close + 1;
        if (end < body.size() && body[end] == ';') ++end;
        body.erase(start, end - start);
        pos = start;
        ++tickCalls;
    }
    verify(tickCalls >= 20, "presentFrameBoundary() runs its work as ticks");
    verify(withoutSpaces(body) ==
               "++g_state->frameCounter;if(graphicsRuntimeDisabled())return;"
               "if(menuTakeConfigPollRequest()||dueMs(g_state->configPollMs,kConfigPollMs))"
               "{g_state->configPollMs=stampMs();}",
           "outside its ticks presentFrameBoundary() holds only the frame counter, the "
           "graphics-off test and the config-poll decision -- anything else added there "
           "runs under no fault budget at all (wrap it in a BoundaryTick)");

    // Every declared tick is run once, and no mark is used twice.
    std::set<std::string> marks;
    int declared = 0;
    for (const std::string* text : {&hookText, &screenText}) {
        size_t at = 0;
        while ((at = text->find("EDVR_BOUNDARY_TICK(", at)) != std::string::npos) {
            const size_t open = at + std::strlen("EDVR_BOUNDARY_TICK");
            const size_t close = closingParen(*text, open);
            at = open;
            if (close == std::string::npos) break;
            const std::string args = text->substr(open + 1, close - open - 1);
            const size_t comma = args.find(',');
            const size_t q1 = args.find('"');
            const size_t q2 = args.rfind('"');
            if (comma == std::string::npos || q1 == std::string::npos || q2 <= q1) continue;
            const std::string id = withoutSpaces(args.substr(0, comma));
            const std::string mark = args.substr(q1 + 1, q2 - q1 - 1);
            ++declared;
            verify(marks.insert(mark).second, "no two frame-boundary ticks share a mark");
            size_t uses = 0, from = 0;
            while ((from = text->find(id + ".run(", from)) != std::string::npos) {
                ++uses;
                from += id.size();
            }
            if (uses != 1) {
                printf("        tick %s (\"%s\") is run %zu times\n", id.c_str(), mark.c_str(), uses);
            }
            verify(uses == 1, "every declared frame-boundary tick is run exactly once");
        }
    }
    verify(declared >= 50, "device_hook.cpp and vscreen.cpp declare their boundary ticks");

    // EXPLORER CAM'S TICK IS THE DEVICE HOOK'S, NOT vSCREEN'S (review 2026-10-08, finding 3). It used to be run from vScreenFrameBoundary, which only exists when vScreen
    // installs a State; with fix.black_void off, fix.panel_distance 1, fix.transition_flash 0, advanced.app_gpu_timing off and advanced.panel_hooks_always off vScreen installs
    // transport-only, that function never runs, and F5 did nothing -- whatever hotkey.explorer_cam held. Moving it back under vScreen (or behind a condition) makes this fail.
    // (tools\test_openxr_transport.py drives the same minimal config through the real d3d11.dll and asks for the boundary's lines: the behaviour, not just this text.)
    const std::string flatTicks = withoutSpaces(tickBody);
    verify(withoutSpaces(hookText).find("EDVR_BOUNDARY_TICK(tkExplorerCam,\"explorer_cam\");") != std::string::npos &&
               flatTicks.find("tkExplorerCam.run([]{explorerCamFrameBoundary(static_cast<uint32_t>(g_state->frameCounter));});") != std::string::npos,
           "Explorer Cam's frame tick is declared in device_hook.cpp and run at the top level of presentFrameBoundary(), with the frame counter");
    verify(withoutSpaces(hookText).find("EDVR_BOUNDARY_TICK(tkExplorerCamProbe,\"explorer_cam_probe\");") != std::string::npos &&
               flatTicks.find("tkExplorerCamProbe.run([]{explorerCamProbeFrameBoundary(static_cast<uint32_t>(g_state->frameCounter));});") != std::string::npos,
           "...and so is the probe's, with a budget of its own");
    verify(flatTicks.find("tkExplorerCam.run(") < flatTicks.find("tkExplorerCamProbe.run(") && flatTicks.find("tkEliteBinds.run(") < flatTicks.find("tkExplorerCam.run(") &&
               flatTicks.find("tkExplorerCam.run(") < flatTicks.find("tkMenu.run(") && flatTicks.find("tkExplorerCam.run(") < flatTicks.find("tkVscreenRest.run("),
           "...after the Elite binds it reads, before the probe that attaches to its hook, and before the menu and vScreen's boundary");
    verify(screenText.find("explorerCamFrameBoundary") == std::string::npos && screenText.find("explorerCamProbeFrameBoundary") == std::string::npos &&
               screenText.find("tkExplorerCam") == std::string::npos,
           "vscreen.cpp does not carry either of them: nothing about Explorer Cam depends on vScreen having installed");
    {
        // The ini's reload lives in vScreenRefreshConfig, the only place it is re-read. It must come BEFORE the State check: with vScreen transport-only there is no State, and a
        // reload that waited for one never happened -- a hotkey.explorer_cam bound while the game runs (or any live edit, the F8 menu's writes) would never apply.
        const size_t at = screenText.find("void vScreenRefreshConfig() {");
        std::string flat;
        if (at != std::string::npos) {
            const size_t end = screenText.find("\n}", at);
            if (end != std::string::npos) flat = withoutSpaces(screenText.substr(at, end - at));
        }
        const size_t reload = flat.find("cfg.reloadIfChanged()"), state = flat.find("if(!s)return;");
        verify(reload != std::string::npos && state != std::string::npos && reload < state,
               "vScreenRefreshConfig() re-reads the ini BEFORE it looks for vScreen's State, so a live edit applies when vScreen is transport-only");
    }
    verify(hookText.find("g_frameBudget") == std::string::npos &&
               hookText.find("deviceHook.frameBoundary") == std::string::npos,
           "the boundary's one shared budget is gone");
    verify(screenText.find("g_cameraBudget") == std::string::npos,
           "and so is the camera readers' one shared budget");

    if (!bad) {
        printf("  ok    the frame boundary runs only as ticks, each declared once and run once, "
               "with %d marks unique across both files; outside them there is only the frame "
               "counter, the graphics-off test and the poll decision\n", declared);
    }
    return bad;
}

// THE SENTINEL, PINNED.
//
// elapsedMs and dueMs differ only in what they answer for a stamp of 0, and
// that difference silently disabled four subsystems in one review: the
// journal watcher, config reloading in both DLLs, and the camera-view
// candidate poll. Each had been a countdown initialised to 0 meaning "due
// now"; each became elapsedMs, which answers false for 0; and because the
// only write to each stamp was inside the branch it gated, none of them ever
// ran again. Nothing crashed and no log line changed -- the features just
// were not there.
//
// So the two are asserted apart here. Anyone who "simplifies" one into the
// other, or makes elapsedMs treat 0 as the epoch, fails this rather than
// shipping four dead subsystems.
int timingChecks() {
    int bad = 0;
    const uint64_t saveMs = g_fakeMs;
    edvr::g_clockForTest = &fakeClock;
    g_fakeMs = 5000;

    if (edvr::elapsedMs(0, 100)) {
        printf("  FAIL  elapsedMs(0, ...) must be false\n");
        ++bad;
    }
    if (!edvr::dueMs(0, 100)) {
        printf("  FAIL  dueMs(0, ...) must be true\n");
        ++bad;
    }
    if (edvr::elapsedMs(4950, 100) || edvr::dueMs(4950, 100)) {
        printf("  FAIL  50 ms into a 100 ms window, neither should fire\n");
        ++bad;
    }
    if (!edvr::elapsedMs(4900, 100) || !edvr::dueMs(4900, 100)) {
        printf("  FAIL  exactly at the window, both should fire\n");
        ++bad;
    }
    // stampMs never hands back the sentinel, or the run it marks would read
    // as one that never started.
    g_fakeMs = 0;
    if (edvr::stampMs() == 0) {
        printf("  FAIL  stampMs() returned 0, which means never started\n");
        ++bad;
    }

    g_fakeMs = saveMs;
    if (bad == 0)
        printf("  ok    elapsedMs and dueMs disagree about 0, as they must\n");
    return bad;
}

// THE JOURNAL WATCHER'S WORKER THREAD (src/d3d11/journal_watch.cpp).
//
// The watcher's file work -- the Status.json read, the journal open and tail, the
// directory walk -- used to run on Elite's render thread from the frame boundary.
// The phase-0 timing of 2026-09-29 (Frontier, v0.18.0-rc.3-94-g8fee57c2) priced
// it: the re-glob took 2.6 to 4.3 ms on every one of 97 runs against 1,992
// journals, and one Status.json read took 39.2 ms inside a 46-50 ms frame. It
// runs on a worker now, and journalWatchTick() only hands results across.
//
// This drives the real worker against real files in a temp folder, from a test
// thread that plays the Present thread. What it pins:
//
//   - events written to a journal, and a changed Status.json, still reach every
//     accessor a consumer reads, and arrive together;
//   - the tick never waits on file work and never does any: a worker held inside
//     a file call does not stop 200,000 ticks, and no file call is ever made on
//     the ticking thread;
//   - shutdown returns while the worker is still inside a file call (it must
//     not join: it can run under the loader lock), wakes a worker that would
//     have slept for ten seconds, and the worker closes the journal on its way
//     out; a frame after it starts nothing;
//   - an eager-Status request reaches a worker asleep on the slower cadence;
//   - eight consecutive file errors retire the watcher, and the consumers are
//     told;
//   - a journal that cannot be proved ours is read from its end, and is replaced
//     by one that is, at a re-glob run by the worker;
//   - the phase-0 timing lines and the journal's own lines are written from the
//     worker, through the real log.
//
// The worker reads the real clock, so this runs with no fake one installed.
namespace journalrig {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr,
                            nullptr);
    }
    return out;
}

bool writeWhole(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == text.size();
}

bool appendTo(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == text.size();
}

// One journal line, in the game's own shape: `"event":"NAME"` with no space,
// which is what the scanner looks for.
std::string ev(const char* name) {
    return std::string("{ \"timestamp\":\"2026-09-29T10:00:00Z\", \"event\":\"") + name + "\" }\n";
}

// A Status.json. flags2 or gui below zero leaves the field out, which is what the
// game does in a menu.
std::string statusText(uint32_t flags, int flags2, int gui) {
    std::string s = "{ \"timestamp\":\"2026-09-29T10:00:00Z\", \"event\":\"Status\", \"Flags\":" +
                    std::to_string(flags);
    if (flags2 >= 0) s += ", \"Flags2\":" + std::to_string(flags2);
    if (gui >= 0) s += ", \"GuiFocus\":" + std::to_string(gui);
    return s + " }\n";
}

// The test thread is the Present thread: it makes the same call, at about the
// same rate, until the condition holds.
template <class Cond>
bool tickUntil(Cond cond, DWORD timeoutMs) {
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        journalWatchTick();
        if (cond()) return true;
        if (GetTickCount64() - t0 >= timeoutMs) return false;
        Sleep(1);
    }
}

bool workerGone(DWORD timeoutMs) {
    const ULONGLONG t0 = GetTickCount64();
    while (journalWatchTestWorkerRunning()) {
        if (GetTickCount64() - t0 >= timeoutMs) return false;
        Sleep(1);
    }
    return true;
}

// Can the file be opened with no sharing at all? Not while any other handle to
// it is open, which is what tells whether the worker still holds the journal.
bool openExclusive(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

// The hook the worker calls before each piece of file work. It records which
// thread made the call, sleeps if asked (a slow disk), and can hold the worker
// once (a stuck one) until the rig lets it go.
struct Witness {
    std::atomic<DWORD> presenterTid{0};   // the thread that ticks; no file call may come from it
    std::atomic<int>   calls{0};
    std::atomic<int>   onPresenter{0};
    std::atomic<bool>  holdNext{false};
    std::atomic<DWORD> sleepMs{0};
    HANDLE             entered = nullptr;   // manual-reset: the worker is now held
    HANDLE             release = nullptr;   // manual-reset: let it go
};
Witness g_w;

void witnessHook(const char*) {
    ++g_w.calls;
    if (GetCurrentThreadId() == g_w.presenterTid.load()) ++g_w.onPresenter;
    if (const DWORD ms = g_w.sleepMs.load()) Sleep(ms);
    if (g_w.holdNext.exchange(false)) {
        SetEvent(g_w.entered);
        // Bounded, so a rig that failed cannot hang the build.
        WaitForSingleObject(g_w.release, 15000);
    }
}

struct Presenter {
    HANDLE   done = nullptr;
    uint32_t ticks = 0;
};

DWORD WINAPI presenterProc(LPVOID arg) {
    Presenter* p = static_cast<Presenter*>(arg);
    g_w.presenterTid.store(GetCurrentThreadId());
    for (uint32_t i = 0; i < p->ticks; ++i) journalWatchTick();
    SetEvent(p->done);
    return 0;
}

DWORD WINAPI shutdownProc(LPVOID arg) {
    journalWatchShutdown();
    SetEvent(static_cast<HANDLE>(arg));
    return 0;
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(path);
            } else {
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

}  // namespace journalrig

int journalWorkerChecks() {
    using namespace journalrig;
    int bad = 0;
    auto verify = [&](bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        printf("  FAIL  %s\n", what);
        ++bad;
    };
    if (edvr::g_clockForTest) {
        printf("  FAIL  the journal worker reads the real clock, and a fake one is installed\n");
        return 1;
    }

    wchar_t tmp[MAX_PATH];
    const DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn == 0 || tn >= MAX_PATH) {
        printf("  FAIL  no temp folder for the journal worker's files\n");
        return 1;
    }
    const std::wstring base =
        std::wstring(tmp) + L"edvr_gate_test_journal_" + std::to_wstring(GetCurrentProcessId());
    removeTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    Config::get().set("log.enabled", "1");
    if (!Log::get().open(base + L"\\logs", L"gatejournal")) {
        printf("  FAIL  could not open a log in %ls to read the worker's lines back\n", base.c_str());
        removeTree(base);
        return 1;
    }
    g_w.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_w.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_w.entered || !g_w.release) {
        printf("  FAIL  could not create the rig's events\n");
        Log::get().close();
        removeTree(base);
        return 1;
    }

    // A folder of its own for each scenario, so a journal one leaves behind is
    // never a candidate in the next.
    auto makeDir = [&](const wchar_t* leaf) {
        const std::wstring d = base + L"\\" + leaf;
        CreateDirectoryW(d.c_str(), nullptr);
        Config::get().set("d3d11.journal_watch", "1");
        Config::get().set("d3d11.journal_dir", toUtf8(d).c_str());
        journalWatchTestSetWorkHook(nullptr);
        journalWatchSetEagerStatus(false);
        return d;
    };
    auto stopWorker = [&] {
        journalWatchShutdown();
        verify(workerGone(10000), "the worker leaves after a shutdown");
    };

    // ---------------------------------------------------- 1. what reaches the consumers
    // 5% is a poll every 25 ms and a re-glob every 200 ms: the scenarios that
    // follow each wait on one, and at 500 ms they would take half a minute.
    {
        const std::wstring d = makeDir(L"events");
        journalWatchTestSetCadencePercent(5);
        const std::wstring journal = d + L"\\Journal.2026-09-29T100000.01.log";
        const std::wstring statusFile = d + L"\\Status.json";
        writeWhole(journal, ev("Fileheader") + ev("Music") + ev("LoadGame"));
        writeWhole(statusFile, statusText(0x10, 0, 0));
        journalWatchConfigure();
        verify(journalWatchActive(), "a folder that exists is watched");
        verify(!journalWatchTestWorkerRunning(), "configuring starts no thread: the first tick does");
        verify(!journalGameplay() && journalStatusSamples() == 0,
               "nothing is known until the worker has read something");

        verify(tickUntil([] { return journalGameplay(); }, 10000),
               "a LoadGame written to the journal reaches journalGameplay() through the worker");
        verify(journalWatchTestWorkerRunning(), "the worker is a thread of its own, still running");
        // The whole first pass arrives as one: Status was read in the same pass
        // as the journal, so both are here on the tick that shows LoadGame.
        uint32_t focus = 99;
        verify(journalStatusSamples() >= 1 && journalOnFootKnown() && !journalOnFoot() &&
                   journalSupercruiseKnown() && journalSupercruise() && journalFssFocusKnown() &&
                   !journalFssFocus() && journalGuiFocus(&focus) && focus == 0,
               "the Status.json read in the same pass arrives with it, all together");

        // A change is what the consumers compare (device_hook.cpp), so a change
        // is what is asserted. Not "== 1": scanEvents scans the head of a chunk
        // twice when a carry precedes it, so a Disembark in a later read counts
        // twice. That is how it was before the worker, and it is not this
        // change's to alter.
        appendTo(journal, ev("Disembark"));
        verify(tickUntil([] { return journalDisembarks() > 0; }, 10000),
               "an event appended to the journal later is tailed: Disembark");
        verify(journalEmbarks() == 0, "...and counted as a Disembark, not an Embark");
        appendTo(journal, ev("Embark"));
        verify(tickUntil([] { return journalEmbarks() > 0; }, 10000), "Embark");

        // 0x40000010: the FSD-jump bit and the supercruise bit; Flags2 1: on foot.
        writeWhole(statusFile, statusText(0x40000010u, 1, 9));
        verify(tickUntil([] { return journalFssFocus(); }, 10000),
               "a changed Status.json reaches its consumer: GuiFocus 9 is the scanner");
        verify(journalGuiFocus(&focus) && focus == 9 && journalOnFootKnown() && journalOnFoot() &&
                   journalSupercruise(),
               "...and the rest of that sample arrives with it");
        appendTo(journal, ev("StartJump"));
        verify(tickUntil([] { return journalInJumpTunnel(); }, 10000),
               "StartJump, with Status' FSD-jump flag up, is a jump tunnel");
        appendTo(journal, ev("FSDJump"));
        verify(tickUntil([] { return !journalInJumpTunnel(); }, 10000), "FSDJump ends it");

        // The seat (journalSeatedFromFlags): the same read's Flags bits 24-26 (main ship, fighter, SRV) and Flags2 bits 1-2
        // (taxi, multicrew). On foot is Flags2 bit 0 with none of them, and a sample that says both is read as seated: the
        // temporal pass's ship split (src/common/temporal_mode.h) turns off only for a clear yes. Each step differs from the
        // one before in on-foot or seated, so a tick that answers is a tick that read the new file.
        struct Seat { uint32_t flags; int flags2; bool onFoot; bool seated; const char* what; };
        const Seat seats[] = {
            {0x01000000u, 0, false, true, "Flags bit 24 (the main ship) seats the commander"},
            {0x00000000u, 1, true, false, "Flags2 bit 0 alone is on foot, and nothing seats it"},
            {0x04000000u, 0, false, true, "Flags bit 26 (an SRV) seats the commander"},
            {0x00000000u, 1, true, false, "on foot again"},
            {0x02000000u, 0, false, true, "Flags bit 25 (a fighter) seats the commander"},
            {0x00000000u, 3, true, true, "Flags2 bit 1 (a taxi) beside OnFoot reads as seated"},
            {0x00000000u, 1, true, false, "on foot once more"},
            {0x00000000u, 2, false, true, "Flags2 bit 1 (a taxi) alone seats the commander"},
            {0x00000000u, 1, true, false, "on foot a third time"},
            {0x00000000u, 4, false, true, "Flags2 bit 2 (multicrew) seats the commander"},
            {0x00000000u, 1, true, false, "on foot a fourth time"},
            {0x40000010u, 0, false, false, "the FSD-jump and supercruise bits are not seats"},
        };
        for (const Seat& st : seats) {
            writeWhole(statusFile, statusText(st.flags, st.flags2, 0));
            verify(tickUntil([&] { return journalOnFoot() == st.onFoot && journalSeated() == st.seated; }, 10000), st.what);
            verify(journalOnFootKnown() && journalSeatedKnown(), "...and both answers are known with it");
        }

        // The menu: Flags and nothing else.
        writeWhole(statusFile, statusText(0, -1, -1));
        verify(tickUntil([] { return !journalOnFootKnown() && !journalFssFocusKnown(); }, 10000),
               "a Status.json without Flags2 or GuiFocus answers not-known");
        verify(!journalFssFocus() && !journalGuiFocus(nullptr),
               "...and the scanner is over");
        verify(journalSeatedKnown() && !journalSeated(),
               "...while Flags itself, in the file, still says nothing seats the commander");
        DeleteFileW(statusFile.c_str());
        verify(tickUntil([] { return !journalSupercruiseKnown(); }, 10000),
               "a Status.json that goes missing drops what was known, after three misses");
        verify(!journalSeatedKnown(), "...the seat included");

        appendTo(journal, ev("Shutdown"));
        verify(tickUntil([] { return !journalGameplay(); }, 10000), "a Shutdown event ends gameplay");
        stopWorker();
    }

    // ------------------------------------- 2. the tick neither waits on file work nor does it
    {
        const std::wstring d = makeDir(L"nowait");
        journalWatchTestSetCadencePercent(100);
        writeWhole(d + L"\\Journal.2026-09-29T100000.01.log", ev("LoadGame"));
        writeWhole(d + L"\\Status.json", statusText(0x10, 1, 9));
        g_w.calls = 0;
        g_w.onPresenter = 0;
        g_w.sleepMs = 0;
        ResetEvent(g_w.entered);
        ResetEvent(g_w.release);
        g_w.holdNext = true;
        journalWatchTestSetWorkHook(&witnessHook);
        journalWatchConfigure();

        // A thread of its own plays the Present thread, so a tick that did wait
        // shows up as a timeout here rather than a hung build.
        Presenter p;
        p.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        p.ticks = 200000;
        HANDLE presenter = CreateThread(nullptr, 0, &presenterProc, &p, 0, nullptr);
        verify(presenter != nullptr && p.done != nullptr, "the rig's Present-thread stand-in started");
        verify(WaitForSingleObject(g_w.entered, 10000) == WAIT_OBJECT_0,
               "the worker reached its first file call and is held inside it");
        verify(WaitForSingleObject(p.done, 10000) == WAIT_OBJECT_0,
               "200,000 ticks completed while the worker was held inside a file call");
        verify(!journalGameplay() && journalStatusSamples() == 0 && journalWatchActive(),
               "...and answered from what was known, which was nothing new");
        verify(g_w.calls.load() == 1 && g_w.onPresenter.load() == 0,
               "the only file call so far is the worker's, and none was made on the ticking thread");

        SetEvent(g_w.release);
        // From here this thread ticks, and the check below covers it too.
        g_w.presenterTid = GetCurrentThreadId();
        verify(tickUntil([] { return journalGameplay(); }, 10000),
               "let go, the worker finishes the call and its results arrive");
        verify(g_w.onPresenter.load() == 0 && g_w.calls.load() >= 3,
               "every file call it made (status, walk, open, tail) came from the worker's thread");

        // The worker is held again inside its next file call, and shutdown is
        // asked for from a third thread: it must return, not wait for the worker.
        ResetEvent(g_w.entered);
        ResetEvent(g_w.release);
        g_w.holdNext = true;
        verify(WaitForSingleObject(g_w.entered, 10000) == WAIT_OBJECT_0,
               "the worker reached its next file call and is held inside it");
        HANDLE shutdownDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE stopper = CreateThread(nullptr, 0, &shutdownProc, shutdownDone, 0, nullptr);
        verify(stopper != nullptr &&
                   WaitForSingleObject(shutdownDone, 10000) == WAIT_OBJECT_0,
               "journalWatchShutdown() returns while the worker is still inside a file call");
        verify(journalWatchTestWorkerRunning(), "...which has not left yet, and shutdown did not wait for it");
        SetEvent(g_w.release);
        verify(workerGone(10000), "released, the worker finishes the call and leaves");
        verify(g_w.onPresenter.load() == 0, "and still no file call came from the ticking thread");
        journalWatchTestSetWorkHook(nullptr);
        for (HANDLE h : {presenter, stopper, p.done, shutdownDone}) {
            if (h) CloseHandle(h);
        }
    }

    // ---------------------------------------------------- 3. stop, and stay stopped
    {
        const std::wstring d = makeDir(L"stop");
        // A ten-second poll: a worker that did not wake for the stop would sleep
        // through the wait below.
        journalWatchTestSetCadencePercent(2000);
        const std::wstring journal = d + L"\\Journal.2026-09-29T100000.01.log";
        writeWhole(journal, ev("LoadGame"));
        writeWhole(d + L"\\Status.json", statusText(0x10, 1, 0));
        journalWatchConfigure();
        verify(tickUntil([] { return journalGameplay(); }, 10000), "the worker's first pass arrives");
        verify(!openExclusive(journal), "while it runs, the worker holds the journal open");
        const ULONGLONG t0 = GetTickCount64();
        journalWatchShutdown();
        verify(workerGone(4000),
               "a worker asleep for ten seconds leaves within four of a shutdown: the stop wakes it");
        const ULONGLONG took = GetTickCount64() - t0;
        printf("        (the worker was gone %llu ms after the shutdown; its poll was 10 s away)\n",
               static_cast<unsigned long long>(took));
        verify(openExclusive(journal), "and it closed the journal on its way out");
        verify(journalWatchActive() && journalGameplay(),
               "shutdown leaves the last answers standing, as closing the file always did");
        for (int i = 0; i < 200; ++i) journalWatchTick();
        Sleep(50);
        verify(!journalWatchTestWorkerRunning(), "a frame after shutdown starts nothing");
    }

    // ------------------------------------------------------------ 4. eager Status reads
    {
        const std::wstring d = makeDir(L"eager");
        // A 5 s poll, and a 1 s eager Status read.
        journalWatchTestSetCadencePercent(1000);
        writeWhole(d + L"\\Journal.2026-09-29T100000.01.log", ev("LoadGame"));
        writeWhole(d + L"\\Status.json", statusText(0x10, 1, 0));
        journalWatchConfigure();
        verify(tickUntil([] { return journalStatusSamples() >= 1; }, 10000),
               "the first Status sample arrives");
        const ULONGLONG t0 = GetTickCount64();
        journalWatchSetEagerStatus(true);
        verify(tickUntil([] { return journalStatusSamples() >= 2; }, 3500),
               "asking for eager reads makes the next sample come at the eager period, not "
               "the five-second poll: the request woke a worker asleep until its old due time");
        printf("        (the second sample came %llu ms after the request; a 5 s poll would be 5000)\n",
               static_cast<unsigned long long>(GetTickCount64() - t0));
        journalWatchSetEagerStatus(false);
        stopWorker();
    }

    // ------------------------------------------------------ 5. eight file errors retire it
    {
        const std::wstring d = makeDir(L"retire");
        journalWatchTestSetCadencePercent(5);
        // The journal is held with no sharing, so every open the worker attempts
        // fails with a sharing violation.
        const std::wstring journal = d + L"\\Journal.2026-09-29T100000.01.log";
        HANDLE lock = CreateFileW(journal.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        verify(lock != INVALID_HANDLE_VALUE, "the rig could hold a journal exclusively");
        journalWatchConfigure();
        verify(journalWatchActive(), "the watcher starts out active");
        verify(tickUntil([] { return !journalWatchActive(); }, 15000),
               "eight consecutive file errors retire it, and the tick tells the consumers");
        verify(!journalFssFocusKnown() && !journalOnFootKnown() && !journalSupercruiseKnown(),
               "a retired watcher knows nothing");
        verify(workerGone(10000), "and the worker has left");
        for (int i = 0; i < 200; ++i) journalWatchTick();
        Sleep(50);
        verify(!journalWatchTestWorkerRunning(), "a retired watcher is not restarted by a later frame");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    }

    // ------------------------------- 6. a journal we cannot prove ours, then one we can
    {
        const std::wstring d = makeDir(L"provenance");
        journalWatchTestSetCadencePercent(5);
        // Written now, created ten minutes ago: what a crashed session's journal
        // looks like to a relaunch (issue #19).
        const std::wstring foreign = d + L"\\Journal.2026-09-29T090000.01.log";
        {
            HANDLE f = CreateFileW(foreign.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                const std::string history = ev("LoadGame");
                DWORD wrote = 0;
                WriteFile(f, history.data(), static_cast<DWORD>(history.size()), &wrote, nullptr);
                FILETIME now;
                GetSystemTimeAsFileTime(&now);
                ULARGE_INTEGER t;
                t.LowPart = now.dwLowDateTime;
                t.HighPart = now.dwHighDateTime;
                t.QuadPart -= 10ull * 60ull * 10000000ull;
                FILETIME old;
                old.dwLowDateTime = t.LowPart;
                old.dwHighDateTime = t.HighPart;
                SetFileTime(f, &old, nullptr, nullptr);
                CloseHandle(f);
            }
        }
        writeWhole(d + L"\\Status.json", statusText(0x10, 1, 0));
        journalWatchConfigure();
        // A Status sample arrives only with the pass that adopted the journal, so
        // by then the worker has the foreign file's end, and nothing written
        // after this point can be mistaken for history.
        verify(tickUntil([] { return journalStatusSamples() >= 1; }, 10000), "the first pass arrives");
        verify(!journalGameplay(),
               "a journal that cannot be proved ours is read from its end: its LoadGame is history");
        appendTo(foreign, ev("Embark"));
        verify(tickUntil([] { return journalEmbarks() > 0; }, 10000),
               "what is written to it from then on is read");
        verify(!journalGameplay(), "and the history was not replayed to get there");
        // The game's own journal appears.
        Sleep(30);
        writeWhole(d + L"\\Journal.2026-09-29T100000.01.log", ev("LoadGame"));
        verify(tickUntil([] { return journalGameplay(); }, 10000),
               "a journal created after we started replaces it at the worker's next re-glob, "
               "and is read from the top");
        stopWorker();
    }

    // ------------------------------------ 7. the phase-0 timing is written from the worker
    {
        const std::wstring d = makeDir(L"timing");
        journalWatchTestSetCadencePercent(5);
        writeWhole(d + L"\\Journal.2026-09-29T100000.01.log", ev("LoadGame"));
        writeWhole(d + L"\\Status.json", statusText(0x10, 1, 0));
        // Every file call takes 25 ms, well over the 2 ms a run must reach to be
        // written as slow at once.
        g_w.calls = 0;
        g_w.onPresenter = 0;
        g_w.sleepMs = 25;
        g_w.presenterTid = GetCurrentThreadId();
        journalWatchTestSetWorkHook(&witnessHook);
        journalWatchConfigure();
        verify(tickUntil([] { return journalGameplay(); }, 10000), "a slow disk delays the arrival, not the tick");
        stopWorker();
        journalWatchTestSetWorkHook(nullptr);
        g_w.sleepMs = 0;
        verify(g_w.calls.load() >= 3, "the slow file calls were made");
        // The ticking thread here is this one, and it made none of them.
        verify(g_w.onPresenter.load() == 0, "and none of them on the thread that ticks");
    }

    // ------------------------------------------------------------------- done
    journalWatchTestSetCadencePercent(100);
    Config::get().set("d3d11.journal_watch", "0");
    journalWatchConfigure();   // off: the next fixtures see a watcher that is not there
    verify(!journalWatchActive() && !journalGameplay(), "a disabled watcher answers nothing");
    Log::get().close();

    std::string text;
    {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((base + L"\\logs\\edvr_gatejournal_*.log").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                text += readWholeFile(base + L"\\logs\\" + fd.cFileName);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    removeTree(base);
    for (HANDLE h : {g_w.entered, g_w.release}) {
        if (h) CloseHandle(h);
    }
    g_w.entered = g_w.release = nullptr;

    const std::vector<std::string> log = splitLines(text);
    auto linesWith = [&](const char* a, const char* b = "") {
        size_t n = 0;
        for (const std::string& line : log) {
            if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) ++n;
        }
        return n;
    };
    verify(!log.empty(), "the log file was written and read back");
    verify(linesWith("journal: watching the game's own event stream") >= 1,
           "the configure line is in the log");
    verify(linesWith("journal: reading on its own thread (") >= 1,
           "the worker says when it starts, so a log without the line means it never ran");
    verify(linesWith("journal: LoadGame -- gameplay has started") >= 1,
           "LoadGame's line, written from the worker");
    verify(linesWith("status: GuiFocus 9 -- the game says the player is in the Full System Scanner.") >= 1 &&
               linesWith("status: the game says FSS focus ended.") >= 1,
           "the scanner's entry and exit lines, written from the worker");
    verify(linesWith("journal: 9 file errors in a row, so the journal is not being read") == 1,
           "the retirement is said once, with the count that tripped it");
    verify(linesWith("was written since this process started but created before it") == 1,
           "the foreign journal's line is written once");
    verify(linesWith("was created after this process started, so it IS this session's") == 1,
           "and so is the line that supersedes it");
    verify(linesWith("periodic work: journal_status SLOW ms=") >= 1,
           "the Status.json read's phase-0 timing is written from the worker");
    verify(linesWith("periodic work: journal_reglob SLOW ms=", " files=1") >= 1,
           "the walk's is, with the number of journals it found");
    verify(linesWith("periodic work: journal_tail SLOW ms=", " bytes=") >= 1,
           "the tail read's is, with the bytes it read");

    if (!bad) {
        printf("  ok    the journal watcher's file work runs on a worker thread of its own: events "
               "and Status.json changes reach every accessor through it, the tick neither waits on "
               "file work nor does any, shutdown returns without joining and wakes a sleeping "
               "worker, eager reads wake it, eight file errors retire it, a journal we cannot "
               "prove ours is read from its end, and its lines and phase-0 timing come from the "
               "worker\n");
    }
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    // The real ini, so the checks that read the shipped configuration run
    // against the numbers that ship rather than against numbers this file made
    // up.
    const std::string dir = argc > 1 ? argv[1] : ".";
    Config::get().init(std::wstring(dir.begin(), dir.end()));

    printf("edvr gate_test -- timing, journal, periodic work, logged notes, boundary ticks\n");

    edvr::g_clockForTest = &fakeClock;
    g_bad += timingChecks();
    // Pure decisions over numbers, so they run once here.
    g_bad += eyeShapeChecks();
    g_bad += journalPickChecks();
    g_bad += periodicWorkChecks();
    g_bad += loggedNotesChecks();
    g_bad += boundarySourceChecks(dir);
    edvr::g_clockForTest = nullptr;
    // Last, and with the real clock the worker reads: it configures the watcher
    // and leaves it off, which the fixtures above never see.
    g_bad += journalWorkerChecks();

    if (g_bad) {
        printf("\nGATE TEST FAILED (%d)\n", g_bad);
        return 1;
    }
    printf("  ok    %d assertion(s) total\n", g_checks);
    printf("\nGATE TEST PASSED\n");
    return 0;
}
