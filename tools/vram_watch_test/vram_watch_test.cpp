// The rig for the graphics memory watch (docs/headset-lock-vdxr-2026-10-02.md, instrument 1): the rules
// (src\common\vram_watch.h), the DXGI read (src\common\vram_query.h) and the machine the Present hook drives
// (VramWatcher), against fake adapters, a fake clock and one real WARP adapter.
//
// What it holds, case by case. Every check's label starts with its case id (V1..V11), which is how
// tools\vram_watch_test\mutants.py tells a mutation that tripped the right check from one that tripped something else:
//
//   V1   the constants and the words the lines are keyed with
//   V2   the predicates: at or over 90% is pressure, over the budget is over, a budget the OS did not report is neither
//   V3   the cadence: the first sample arms and says nothing; a line every 30 s, no sooner
//   V4   pressure: a line the moment the card comes within reach of the limit, then every 5 s, then back to 30 s
//   V5   crossings: one line when usage goes over the budget and one when it comes back, one line per sample at most
//   V6   the cap on the two cadences, and that a crossing is never held to it
//   V7   a sample the OS did not answer, a budget of 0, and a clock that went backwards
//   V8   the text: every field, "-" for a segment with no answer, the armed and unavailable lines, every line fitting
//   V9   the machine end to end on a fake clock: the OS is asked once a second, the armed line, the unavailable line
//        (once, never asked again), a failed read said once, the first line of every kind
//   V10  the DXGI read: a fake adapter's figures and failures, a device with no IDXGIDevice and an adapter with no
//        IDXGIAdapter3 (what a DXVK or Wine without the 1.4 interfaces hands out), and a real WARP adapter
//   V11  the glue, by source text: the Present hook calls the watch once, unconditionally (so the flat profile has it
//        too), after the stall sampler's beat; the glue asks the cheap question first; build.bat compiles it
//
//   vram_watch_test.exe --dry-run          touches nothing
//   vram_watch_test.exe --self-test [root]  root is the repository, for the V11 source pins (skipped without it)
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "system_d3d11.h"
#include "vram_query.h"
#include "vram_watch.h"

using namespace edvr;

namespace {
unsigned g_checks = 0, g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
}

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

constexpr uint64_t kMiB = 1024ull * 1024ull;

VramSegment seg(uint64_t usageMiB, uint64_t budgetMiB) {
    VramSegment s;
    s.valid = true;
    s.usage = usageMiB * kMiB;
    s.budget = budgetMiB * kMiB;
    return s;
}

// Local figures only (the policy looks at nothing else), in tenths of a percent of a 10000 MiB budget.
VramFigures localOnly(uint64_t usageMiB, uint64_t budgetMiB = 10000) {
    VramFigures f;
    f.local = seg(usageMiB, budgetMiB);
    return f;
}

// ---- V1 ----------------------------------------------------------------------------------------------------
void constantCases() {
    check(kVramSampleEveryMs == 1000 && kVramLineEveryMs == 30000 && kVramPressureEveryMs == 5000 && kVramPressurePercent == 90,
          "V1.constants: sampled every second, a line every 30 s, every 5 s from 90% of the budget");
    check(kVramCadenceLineCap == 3000, "V1.constants: the cadence cap is 3000 lines a session");
    check(std::string(vramReasonKey(VramReason::Armed)) == "armed" && std::string(vramReasonKey(VramReason::Periodic)) == "periodic" &&
              std::string(vramReasonKey(VramReason::Pressure)) == "pressure" &&
              std::string(vramReasonKey(VramReason::OverBudget)) == "over_budget" &&
              std::string(vramReasonKey(VramReason::BackUnderBudget)) == "back_under_budget" &&
              std::string(vramReasonKey(VramReason::CapReached)) == "cap_reached",
          "V1.keys: the reason words are fixed: armed periodic pressure over_budget back_under_budget cap_reached");
}

// ---- V2 ----------------------------------------------------------------------------------------------------
void predicateCases() {
    VramSegment s;
    s.valid = true;
    s.budget = 10000;
    s.usage = 9000;
    check(vramUnderPressure(s), "V2.pressure: exactly 90% of the budget is under pressure (at or over)");
    s.usage = 8999;
    check(!vramUnderPressure(s), "V2.pressure: 89.99% is not");
    s.usage = 10000;
    check(!vramOverBudget(s) && vramUnderPressure(s), "V2.over: usage equal to the budget is at the limit, not over it");
    s.usage = 10001;
    check(vramOverBudget(s) && vramUnderPressure(s), "V2.over: one byte over the budget is over it");
    s.budget = 0;
    s.usage = 5ull << 30;
    check(!vramOverBudget(s) && !vramUnderPressure(s) && vramPercentTenths(s) == -1,
          "V2.budget: a budget the OS did not report is neither over nor under pressure, and has no percentage");
    VramSegment invalid;
    invalid.usage = 100;
    invalid.budget = 100;
    check(!vramOverBudget(invalid) && !vramUnderPressure(invalid) && vramPercentTenths(invalid) == -1,
          "V2.valid: a segment the OS did not answer for is nothing, whatever its fields hold");
    check(vramPercentTenths(seg(6830, 10000)) == 683 && vramPercentTenths(seg(1, 3)) == 333 && vramPercentTenths(seg(2, 3)) == 667 &&
              vramPercentTenths(seg(0, 10000)) == 0 && vramPercentTenths(seg(12, 12)) == 1000,
          "V2.percent: tenths of a percent, rounded to the nearest (two thirds is 66.7, not 66.6)");
    check(vramPercentTenths(seg(1ull << 20, 2ull << 20)) == 500, "V2.percent: a terabyte against two is 50.0%");
    VramSegment big;
    big.valid = true;
    big.usage = 64ull << 30;
    big.budget = 70ull << 30;
    check(vramUnderPressure(big) && !vramOverBudget(big), "V2.pressure: 64 GB of 70 does not overflow the comparison");
}

// ---- V3 ----------------------------------------------------------------------------------------------------
// The caller's loop: a sample every second from t = 0, offered to the policy; the seconds a line was due at.
std::vector<unsigned> linesAt(VramPolicy& p, unsigned fromSecond, unsigned toSecond, const VramFigures& f, std::vector<VramReason>* kinds = nullptr) {
    std::vector<unsigned> out;
    for (unsigned t = fromSecond; t <= toSecond; ++t) {
        const VramReason r = p.observe(t * 1000ull, f);
        if (r != VramReason::None) {
            out.push_back(t);
            if (kinds) kinds->push_back(r);
        }
    }
    return out;
}

void cadenceCases() {
    VramPolicy p;
    std::vector<VramReason> kinds;
    const std::vector<unsigned> lines = linesAt(p, 0, 100, localOnly(5000), &kinds);
    check(lines == std::vector<unsigned>({30, 60, 90}),
          "V3.cadence: a card at 50% over 100 s of one-second samples gets a line at 30, 60 and 90 s and no other");
    bool allPeriodic = !kinds.empty();
    for (VramReason k : kinds) allPeriodic = allPeriodic && k == VramReason::Periodic;
    check(allPeriodic, "V3.cadence: and each is a periodic line");
    VramPolicy q;
    check(q.observe(100, localOnly(5000)) == VramReason::None && q.armed(),
          "V3.arm: the first sample arms the policy and writes nothing (the glue's armed line is the first line)");
    check(q.observe(100 + 29999, localOnly(5000)) == VramReason::None && q.observe(100 + 30000, localOnly(5000)) == VramReason::Periodic,
          "V3.boundary: 29.999 s after the first line is too soon, 30 s is due");
    VramPolicy r;
    r.observe(0, localOnly(5000));
    check(r.observe(41000, localOnly(5000)) == VramReason::Periodic && r.observe(42000, localOnly(5000)) == VramReason::None &&
              r.observe(71000, localOnly(5000)) == VramReason::Periodic,
          "V3.late: a sample that arrives late (41 s) is the line, and the next is 30 s after it, not after the missed one");
}

// ---- V4 ----------------------------------------------------------------------------------------------------
void pressureCases() {
    VramPolicy p;
    std::vector<unsigned> lines;
    std::vector<VramReason> kinds;
    p.observe(0, localOnly(5000));
    for (unsigned t = 1; t <= 80; ++t) {
        const uint64_t usage = (t >= 10 && t < 36) ? 9500 : 6000;
        const VramReason k = p.observe(t * 1000ull, localOnly(usage));
        if (k != VramReason::None) {
            lines.push_back(t);
            kinds.push_back(k);
        }
    }
    check(lines == std::vector<unsigned>({10, 15, 20, 25, 30, 35, 65}),
          "V4.pressure: a line the second usage reaches 95% (not up to 5 s later), one every 5 s while it stays, none when it "
          "drops, and the next 30 s after the last");
    check(kinds.size() == 7 && kinds[0] == VramReason::Pressure && kinds[5] == VramReason::Pressure && kinds[6] == VramReason::Periodic,
          "V4.kinds: the lines at pressure are pressure lines; the one after it eases is a periodic line");
    VramPolicy edge;
    edge.observe(0, localOnly(5000));
    check(edge.observe(1000, localOnly(9000)) == VramReason::Pressure, "V4.edge: exactly 90% is pressure");
    VramPolicy below;
    below.observe(0, localOnly(5000));
    const std::vector<unsigned> belowLines = linesAt(below, 1, 40, localOnly(8999));
    check(belowLines == std::vector<unsigned>({30}), "V4.edge: 89.99% is not: it keeps the 30 s cadence");
    VramPolicy staying;
    staying.observe(0, localOnly(9500));
    check(staying.observe(1000, localOnly(9500)) == VramReason::None && staying.observe(5000, localOnly(9500)) == VramReason::Pressure,
          "V4.armed-at-pressure: a card already at 95% when the watch arms writes its first pressure line 5 s after the armed line");
}

// ---- V5 ----------------------------------------------------------------------------------------------------
void crossingCases() {
    VramPolicy p;
    p.observe(0, localOnly(8000));
    std::vector<unsigned> lines;
    std::vector<VramReason> kinds;
    auto feed = [&](unsigned t, uint64_t usage, uint64_t budget = 10000) {
        const VramReason k = p.observe(t * 1000ull, localOnly(usage, budget));
        if (k != VramReason::None) {
            lines.push_back(t);
            kinds.push_back(k);
        }
    };
    for (unsigned t = 1; t <= 4; ++t) feed(t, 8000);
    feed(5, 10100);                       // over the budget: the crossing, not a pressure line
    for (unsigned t = 6; t <= 9; ++t) feed(t, 10100);
    feed(10, 10100);                      // still over: pressure lines every 5 s from the crossing
    feed(11, 10100);
    feed(12, 8500);                       // back under: the crossing; 85% is not pressure
    feed(13, 10100);                      // flapping: a line per flip
    feed(14, 8500);
    check(lines == std::vector<unsigned>({5, 10, 12, 13, 14}),
          "V5.crossings: a line the second usage goes over the budget, pressure lines 5 s apart while it stays over, a line the "
          "second it comes back, and one per flip after that");
    check(kinds.size() == 5 && kinds[0] == VramReason::OverBudget && kinds[1] == VramReason::Pressure && kinds[2] == VramReason::BackUnderBudget &&
              kinds[3] == VramReason::OverBudget && kinds[4] == VramReason::BackUnderBudget,
          "V5.kinds: over_budget, pressure, back_under_budget, over_budget, back_under_budget");
    check(p.crossings() == 4, "V5.count: four crossings counted");

    // A budget that shrinks under steady usage is a crossing too (another program took the memory).
    VramPolicy shrink;
    shrink.observe(0, localOnly(8000, 10000));
    check(shrink.observe(1000, localOnly(8000, 7000)) == VramReason::OverBudget && shrink.observe(2000, localOnly(8000, 12000)) == VramReason::BackUnderBudget,
          "V5.budget-moves: usage steady while the budget falls below it is over_budget, and back under when the budget returns");

    // One line a sample: a crossing at the moment a periodic line is due is the crossing, and the cadence restarts from it.
    VramPolicy once;
    once.observe(0, localOnly(5000));
    const VramReason atThirty = once.observe(30000, localOnly(10100));
    check(atThirty == VramReason::OverBudget, "V5.one-line: a crossing at the second a periodic line is due writes the crossing only");
    check(once.observe(31000, localOnly(10100)) == VramReason::None, "V5.one-line: and the cadence restarts from it");
}

// ---- V6 ----------------------------------------------------------------------------------------------------
void capCases() {
    VramPolicy p;
    p.observe(0, localOnly(9500));
    unsigned cadence = 0, notices = 0;
    uint64_t now = 0;
    for (unsigned i = 0; i < 3010; ++i) {
        now += kVramPressureEveryMs;
        const VramReason k = p.observe(now, localOnly(9500));
        if (k == VramReason::Pressure) ++cadence;
        if (k == VramReason::CapReached) ++notices;
    }
    check(cadence == kVramCadenceLineCap && notices == 1,
          "V6.cap: 3000 pressure lines are written, then one cap_reached notice, then nothing more of the cadence");
    now += 1000;
    check(p.observe(now, localOnly(10100)) == VramReason::OverBudget && p.observe(now + 1000, localOnly(8000)) == VramReason::BackUnderBudget,
          "V6.cap: a crossing is written after the cap, over and back");
    VramPolicy periodic;
    periodic.observe(0, localOnly(5000));
    unsigned n = 0;
    uint64_t t = 0;
    for (unsigned i = 0; i < 3005; ++i) {
        t += kVramLineEveryMs;
        if (periodic.observe(t, localOnly(5000)) == VramReason::Periodic) ++n;
    }
    check(n == kVramCadenceLineCap, "V6.cap: the periodic lines are held to the same cap");
}

// ---- V7 ----------------------------------------------------------------------------------------------------
void robustnessCases() {
    VramPolicy p;
    VramFigures none;   // nothing valid
    check(p.observe(0, none) == VramReason::None && !p.armed(), "V7.invalid: a sample with no local figures is ignored and does not arm the policy");
    p.observe(1000, localOnly(5000));
    check(p.observe(31000, none) == VramReason::None && p.observe(31000, localOnly(5000)) == VramReason::Periodic,
          "V7.invalid: a failed sample in the middle changes nothing: the line comes on the next good one");
    VramFigures noBudget;
    noBudget.local.valid = true;
    noBudget.local.usage = 20ull << 30;
    noBudget.local.budget = 0;
    VramPolicy z;
    z.observe(0, noBudget);
    std::vector<unsigned> lines = linesAt(z, 1, 65, noBudget);
    check(lines == std::vector<unsigned>({30, 60}) && z.crossings() == 0,
          "V7.no-budget: with no budget reported there is no pressure and no crossing, only the 30 s cadence");
    VramPolicy c;
    c.observe(100000, localOnly(5000));
    check(c.observe(50000, localOnly(5000)) == VramReason::None && c.observe(79999, localOnly(5000)) == VramReason::None &&
              c.observe(80000, localOnly(5000)) == VramReason::Periodic,
          "V7.clock: a clock that went backwards restarts the interval from the new reading");
}

// ---- V8 ----------------------------------------------------------------------------------------------------
VramFigures sampleFigures() {
    VramFigures f;
    f.local = seg(7421, 10863);
    f.nonLocal = seg(316, 16311);
    return f;
}

void textCases() {
    char figures[256];
    const size_t fn = formatVramFigures(figures, sizeof(figures), sampleFigures());
    check(std::string(figures) == "local_used_mb=7421 local_budget_mb=10863 local_pct=68.3 nonlocal_used_mb=316 nonlocal_budget_mb=16311 nonlocal_pct=1.9" &&
              fn == std::strlen(figures),
          "V8.figures: six key=value fields, megabytes (MiB) and tenths of a percent, in this order");
    formatVramFigures(figures, sizeof(figures), sampleFigures(), ',', "vram_");
    check(std::string(figures) == "vram_local_used_mb=7421,vram_local_budget_mb=10863,vram_local_pct=68.3,vram_nonlocal_used_mb=316,"
                                  "vram_nonlocal_budget_mb=16311,vram_nonlocal_pct=1.9",
          "V8.figures: the runtime's SLOW line asks for the same figures comma-separated with a vram_ prefix on every key");
    char line[640];
    const size_t ln = formatVramLine(line, sizeof(line), VramReason::Periodic, sampleFigures());
    check(std::string(line) == "vram: reason=periodic local_used_mb=7421 local_budget_mb=10863 local_pct=68.3 nonlocal_used_mb=316 "
                               "nonlocal_budget_mb=16311 nonlocal_pct=1.9" &&
              ln == std::strlen(line),
          "V8.line: a periodic line is `vram: reason=periodic` and the figures");
    formatVramLine(line, sizeof(line), VramReason::OverBudget, sampleFigures());
    check(std::string(line).rfind("vram: reason=over_budget local_used_mb=7421", 0) == 0, "V8.line: the reason word is the line's first key");
    VramFigures partial = sampleFigures();
    partial.nonLocal = VramSegment{};
    formatVramFigures(figures, sizeof(figures), partial);
    check(contains(figures, "nonlocal_used_mb=- nonlocal_budget_mb=- nonlocal_pct=-") && contains(figures, "local_pct=68.3"),
          "V8.partial: a segment the OS did not answer for prints '-' for all three, not zero");
    VramFigures unbudgeted = sampleFigures();
    unbudgeted.local.budget = 0;
    formatVramFigures(figures, sizeof(figures), unbudgeted);
    check(contains(figures, "local_used_mb=7421 local_budget_mb=0 local_pct=-"), "V8.no-budget: a budget of 0 prints 0 and no percentage");

    formatVramArmed(line, sizeof(line), "NVIDIA GeForce RTX 4070 Ti", sampleFigures());
    check(std::string(line).rfind("vram: reason=armed local_used_mb=7421 local_budget_mb=10863 local_pct=68.3 ", 0) == 0 &&
              contains(line, "; adapter NVIDIA GeForce RTX 4070 Ti; ") &&
              contains(line, "a line every 30 s, every 5 s from 90% of the local budget, and one at each crossing of the budget.") &&
              !contains(line, "no local budget"),
          "V8.armed: the armed line has the figures, the adapter's name and the whole policy in words");
    formatVramArmed(line, sizeof(line), "", unbudgeted);
    check(contains(line, "adapter (unnamed);") && contains(line, "The OS reports no local budget"),
          "V8.armed: an unnamed adapter and a budget the OS did not report are said");
    formatVramUnavailable(line, sizeof(line), "IDXGIAdapter3 is not offered");
    check(std::string(line) == "vram: unavailable -- IDXGIAdapter3 is not offered. No vram: line will be written this session.",
          "V8.unavailable: the line says why and that no vram: line follows");
    formatVramUnavailable(line, sizeof(line), nullptr);
    check(contains(line, "no reason given"), "V8.unavailable: a missing reason is said, not left blank");
    formatVramLine(line, sizeof(line), VramReason::CapReached, sampleFigures());
    check(std::string(line).rfind("vram: reason=cap_reached local_used_mb=7421", 0) == 0 && contains(line, "3000 periodic and pressure lines") &&
              contains(line, "only a crossing of the budget is written"),
          "V8.cap: the notice says what was capped and what is still written");

    // The widest figures fit, and a buffer that is too small is cut cleanly. The adapter's name is at most the 127
    // bytes the watcher's Opening holds.
    VramFigures widest;
    widest.local.valid = widest.nonLocal.valid = true;
    widest.local.usage = widest.local.budget = widest.nonLocal.usage = widest.nonLocal.budget = ~0ull;
    const std::string longName(127, 'N');
    char big[768];
    const size_t bn = formatVramArmed(big, sizeof(big), longName.c_str(), widest);
    check(bn > 0 && bn < sizeof(big) - 1 && contains(big, "crossing of the budget."),
          "V8.fit: the widest armed line (127-byte adapter name, every figure at 2^64-1) fits the 768 bytes the watcher gives it, tail intact");
    VramFigures widestUnbudgeted = widest;
    widestUnbudgeted.local.budget = 0;
    const size_t un = formatVramArmed(big, sizeof(big), longName.c_str(), widestUnbudgeted);
    check(un > 0 && un < sizeof(big) - 1 && contains(big, "can be judged."),
          "V8.fit: and with the no-budget sentence added");
    char one[400];
    const size_t wn = formatVramLine(one, sizeof(one), VramReason::CapReached, widest);
    check(wn > 0 && wn < sizeof(one) - 1 && contains(one, "is written."), "V8.fit: the widest cap line fits 400 bytes");
    char tiny[12];
    const size_t tn = formatVramLine(tiny, sizeof(tiny), VramReason::Periodic, sampleFigures());
    check(tn == sizeof(tiny) - 1 && tiny[sizeof(tiny) - 1] == 0 && std::strncmp(tiny, "vram: reaso", 11) == 0,
          "V8.fit: a buffer too small gets the start of the line, terminated");
    check(formatVramLine(nullptr, 0, VramReason::Periodic, sampleFigures()) == 0 && formatVramUnavailable(nullptr, 0, "x") == 0,
          "V8.fit: no buffer, no text");
}

// ---- V9 ----------------------------------------------------------------------------------------------------
struct Rig {
    // A fake clock: QPC at 10 MHz, and the millisecond clock it implies.
    static constexpr int64_t kPerSecond = 10000000;
    int64_t qpc = 0;
    // What the fake OS answers.
    VramFigures figures = localOnly(5000);
    bool openOk = true;
    const char* openWhy = "IDXGIAdapter3 is not offered";
    bool readOk = true;
    // What the watcher did.
    unsigned opens = 0, reads = 0;
    std::vector<std::string> lines;
    VramWatcher watcher;

    // One Present of the hook: the cheap question, then the tick, as vramWatchTick does.
    void present() {
        if (!watcher.due(qpc)) return;
        watcher.tick(
            qpc, kPerSecond, static_cast<uint64_t>(qpc / (kPerSecond / 1000)),
            [&] {
                ++opens;
                VramWatcher::Opening o;
                o.ok = openOk;
                o.why = openWhy;
                std::snprintf(o.adapter, sizeof(o.adapter), "Fake GPU");
                return o;
            },
            [&](long* hr) {
                ++reads;
                VramFigures f = figures;
                if (!readOk) {
                    f.local.valid = false;
                    f.nonLocal.valid = false;
                    *hr = static_cast<long>(0x887A0005);
                }
                return f;
            },
            [&](const char* line) { lines.push_back(line); });
    }
    // 72 Hz frames for `seconds`.
    void run(unsigned seconds) {
        const int64_t step = kPerSecond / 72;
        const int64_t end = qpc + static_cast<int64_t>(seconds) * kPerSecond;
        for (; qpc < end; qpc += step) present();
    }
    unsigned count(const char* needle) const {
        unsigned n = 0;
        for (const std::string& l : lines)
            if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
};

void machineCases() {
    {
        Rig r;
        r.run(125);
        check(r.opens == 1, "V9.open: the adapter is opened once, at the first frame");
        check(r.reads >= 120 && r.reads <= 126, "V9.rate: nine thousand frames in 125 s ask the OS about once a second (not once a frame)");
        check(r.lines.size() == 5 && r.lines[0].rfind("vram: reason=armed ", 0) == 0 && r.count("reason=periodic") == 4,
              "V9.lines: the armed line, then a periodic one at 30, 60, 90 and 120 s");
        check(r.lines.size() >= 1 && r.lines[0].find("adapter Fake GPU;") != std::string::npos, "V9.armed: the armed line names the adapter the opener gave");
    }
    {
        Rig r;
        r.run(31);
        check(r.lines.size() == 2 && r.count("reason=periodic") == 1,
              "V9.cadence: the armed line counts as the first of the cadence, so the first periodic line is 30 s after the first frame");
    }
    {
        Rig r;
        r.openOk = false;
        r.openWhy = "IDXGIAdapter3 is not offered, and QueryVideoMemoryInfo needs it (a DXVK or Wine without it)";
        r.run(60);
        check(r.opens == 1 && r.reads == 0 && r.lines.size() == 1 && r.lines[0].rfind("vram: unavailable -- IDXGIAdapter3 is not offered", 0) == 0,
              "V9.unavailable: an adapter that cannot be opened is said once, with its reason, and nothing is asked again");
        check(r.watcher.unavailable() && !r.watcher.due(r.qpc + 100 * Rig::kPerSecond),
              "V9.unavailable: and the hook's cheap question says no from then on, however late the clock");
    }
    {
        Rig r;
        r.readOk = false;
        r.run(20);
        check(r.opens == 1 && r.reads == 1 && r.lines.size() == 1 && r.lines[0].find("vram: unavailable -- QueryVideoMemoryInfo failed for the local segment group (hr 0x887A0005)") == 0,
              "V9.first-read: a first reading that fails is the unavailable line, with the HRESULT, and the watch stops there");
    }
    {
        Rig r;
        r.run(3);
        r.readOk = false;
        r.run(10);
        check(r.count("vram: QueryVideoMemoryInfo failed") == 1 && r.watcher.failures() >= 9,
              "V9.later-read: a read that fails after the first is said once and retried every second");
        r.readOk = true;
        r.run(40);
        check(r.count("reason=periodic") == 1 && r.count("vram: QueryVideoMemoryInfo failed") == 1,
              "V9.later-read: when it answers again the cadence goes on; the failure is not repeated");
    }
    {
        Rig r;
        r.run(10);
        r.figures = localOnly(9500);
        r.run(12);
        r.figures = localOnly(10200);
        r.run(3);
        r.figures = localOnly(8000);
        r.run(3);
        check(r.count("reason=pressure") >= 3 && r.count("reason=over_budget") == 1 && r.count("reason=back_under_budget") == 1,
              "V9.kinds: through the machine, pressure lines, one over_budget line and one back_under_budget line are written");
        bool inOrder = false;
        size_t over = std::string::npos, under = std::string::npos, pressure = std::string::npos;
        for (size_t i = 0; i < r.lines.size(); ++i) {
            if (pressure == std::string::npos && r.lines[i].find("reason=pressure") != std::string::npos) pressure = i;
            if (r.lines[i].find("reason=over_budget") != std::string::npos) over = i;
            if (r.lines[i].find("reason=back_under_budget") != std::string::npos) under = i;
        }
        inOrder = pressure != std::string::npos && over != std::string::npos && under != std::string::npos && pressure < over && over < under;
        check(inOrder, "V9.kinds: in that order");
        check(over != std::string::npos && under != std::string::npos && r.lines[over].find("local_used_mb=10200 local_budget_mb=10000") != std::string::npos &&
                  r.lines[under].find("local_used_mb=8000 local_budget_mb=10000") != std::string::npos,
              "V9.figures: each line carries the figures of its own sample (the over_budget line 10200 of 10000, the line after it 8000)");
    }
}

// ---- V10 ---------------------------------------------------------------------------------------------------
struct FakeAdapter {
    HRESULT localHr = S_OK, nonLocalHr = S_OK;
    UINT64 localUsage = 3000 * kMiB, localBudget = 10000 * kMiB, nonLocalUsage = 100 * kMiB, nonLocalBudget = 16000 * kMiB;
    unsigned calls = 0;
    UINT lastNode = 99;
    HRESULT QueryVideoMemoryInfo(UINT node, DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO* out) {
        ++calls;
        lastNode = node;
        const bool local = group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
        const HRESULT hr = local ? localHr : nonLocalHr;
        if (FAILED(hr)) return hr;
        out->CurrentUsage = local ? localUsage : nonLocalUsage;
        out->Budget = local ? localBudget : nonLocalBudget;
        out->AvailableForReservation = 0;
        out->CurrentReservation = 0;
        return S_OK;
    }
};

// What a stack without the DXGI 1.4 interfaces hands out: an IDXGIAdapter that is not an IDXGIAdapter3, on a device
// that is an IDXGIDevice. (Static storage: Release never deletes.)
struct PlainAdapter final : IDXGIAdapter {
    LONG refs = 1;
    std::wstring description = L"Plain Adapter 1.0";
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIAdapter)) {
            *out = static_cast<IDXGIAdapter*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT, IDXGIOutput**) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* d) override {
        std::memset(d, 0, sizeof(*d));
        std::wcsncpy(d->Description, description.c_str(), 127);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER*) override { return E_NOTIMPL; }
};

struct PlainDevice final : IDXGIDevice {
    LONG refs = 1;
    PlainAdapter adapter;
    bool failGetAdapter = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIDevice)) {
            *out = static_cast<IDXGIDevice*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** out) override {
        if (failGetAdapter) {
            *out = nullptr;
            return E_FAIL;
        }
        *out = &adapter;
        adapter.AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*, IDXGISurface**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const*, DXGI_RESIDENCY*, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT*) override { return E_NOTIMPL; }
};

struct BareUnknown final : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

void queryCases() {
    FakeAdapter a;
    const VramFigures f = vramRead(&a);
    check(f.local.valid && f.nonLocal.valid && f.local.usage == 3000 * kMiB && f.local.budget == 10000 * kMiB &&
              f.nonLocal.usage == 100 * kMiB && f.nonLocal.budget == 16000 * kMiB && a.calls == 2 && a.lastNode == 0,
          "V10.read: both segment groups of node 0 are read, usage and budget of each in its own field");
    FakeAdapter bad;
    bad.localHr = static_cast<HRESULT>(0x887A0001);
    HRESULT hr = S_OK;
    const VramFigures g = vramRead(&bad, &hr);
    check(!g.local.valid && g.nonLocal.valid && hr == static_cast<HRESULT>(0x887A0001),
          "V10.read: a group the OS refuses is invalid, the other is still read, and the HRESULT is handed back");
    check(!vramReadSegment(static_cast<FakeAdapter*>(nullptr), DXGI_MEMORY_SEGMENT_GROUP_LOCAL).valid, "V10.read: no adapter, no figures");

    const char* why = nullptr;
    char name[64] = "unchanged";
    check(vramAdapterOf(nullptr, &why, name, sizeof(name)) == nullptr && why && std::string(why) == "no device to ask" && name[0] == 0,
          "V10.open: no device is said, and the name is cleared");
    BareUnknown bare;
    why = nullptr;
    check(vramAdapterOf(&bare, &why, nullptr, 0) == nullptr && why && contains(why, "no IDXGIDevice"), "V10.open: a device that is not an IDXGIDevice is said");
    PlainDevice noAdapter;
    noAdapter.failGetAdapter = true;
    why = nullptr;
    check(vramAdapterOf(&noAdapter, &why, nullptr, 0) == nullptr && why && contains(why, "no DXGI adapter"), "V10.open: a device with no adapter is said");
    PlainDevice plain;
    why = nullptr;
    name[0] = 'x';
    IDXGIAdapter3* none = vramAdapterOf(&plain, &why, name, sizeof(name));
    check(none == nullptr && why && contains(why, "IDXGIAdapter3 is not offered") && std::string(name) == "Plain Adapter 1.0",
          "V10.open: an adapter without IDXGIAdapter3 (a DXVK or Wine that lacks it) is said by its interface, and still named");
    check(plain.adapter.refs == 1 && plain.refs == 1, "V10.open: every reference taken on the way was given back");
    char tinyName[8];
    std::memset(tinyName, 'z', sizeof(tinyName));
    vramAdapterOf(&plain, &why, tinyName, sizeof(tinyName));
    check(std::string(tinyName) == "Plain A" && tinyName[sizeof(tinyName) - 1] == 0, "V10.open: a name that does not fit is cut to the buffer and terminated");
    // A cut never lands inside a character: "Grün Karte" in 4 bytes is "Gr" (the u-umlaut is two bytes in UTF-8).
    plain.adapter.description = L"Grün Karte";
    char umlaut[4];
    std::memset(umlaut, 'z', sizeof(umlaut));
    vramAdapterOf(&plain, &why, umlaut, sizeof(umlaut));
    check(std::string(umlaut) == "Gr", "V10.open: and the cut is not made inside a multi-byte character");
    plain.adapter.description = L"Plain Adapter 1.0";

    // The real thing: a WARP device from System32's d3d11 (never this build's proxy) and its adapter.
    PFN_D3D11_CREATE_DEVICE create = systemD3D11CreateDevice();
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT made = create ? create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context) : E_FAIL;
    if (FAILED(made) || !device) {
        std::printf("[vram_watch_test] SKIPPED the real-adapter checks: no WARP device (hr 0x%08lX)\n", static_cast<unsigned long>(made));
        return;
    }
    char real[128];
    why = nullptr;
    IDXGIAdapter3* adapter3 = vramAdapterOf(device, &why, real, sizeof(real));
    check(adapter3 != nullptr, "V10.real: a real D3D11 device's adapter answers for IDXGIAdapter3 (Windows 10 1607 or later)");
    if (adapter3) {
        HRESULT rhr = S_OK;
        const VramFigures rf = vramRead(adapter3, &rhr);
        check(rf.local.valid, "V10.real: QueryVideoMemoryInfo answers for the local segment group of a real adapter");
        // What the call costs, for the record (the tick makes two a second): not a check, a number.
        LARGE_INTEGER t0{}, t1{}, freq{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const int reps = 400;
        for (int i = 0; i < reps; ++i) vramRead(adapter3);
        QueryPerformanceCounter(&t1);
        std::printf("[vram_watch_test] adapter \"%s\": local %llu of %llu MiB, non-local %s; both segment groups read in %.1f us\n", real,
                    static_cast<unsigned long long>(rf.local.usage / kMiB), static_cast<unsigned long long>(rf.local.budget / kMiB),
                    rf.nonLocal.valid ? "answered" : "not answered",
                    1e6 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart) / reps);
        adapter3->Release();
    }
    if (context) context->Release();
    device->Release();
}

// ---- V11 ---------------------------------------------------------------------------------------------------
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

size_t at(const std::string& text, const char* needle, size_t from = 0) { return text.find(needle, from); }

void sourcePins(const std::string& root) {
    const std::string hook = slurp(root + "\\src\\d3d11\\device_hook.cpp");
    const std::string glue = slurp(root + "\\src\\d3d11\\vram_tick.cpp");
    const std::string bat = slurp(root + "\\build.bat");
    check(!hook.empty() && !glue.empty() && !bat.empty(), "V11.read: the three sources the glue lives in were read");
    if (hook.empty() || glue.empty() || bat.empty()) return;
    const size_t none = std::string::npos;
    const size_t beat = at(hook, "    stallWatchBeat(presentT1, g_state->frameCounter);\n");
    const size_t tick = at(hook, "    vramWatchTick(presentT1, g_state->device);\n");
    const size_t ticks = at(hook, "g_frameTicks.markAt(\"present_pre\", presentT0);");
    check(beat != none && tick != none && ticks != none && beat < tick && tick < ticks,
          "V11.hook: hookedPresent ticks the watch right after the stall sampler's beat, before the frame ticks");
    check(tick != none && at(hook, "vramWatchTick(", tick + 10) == none && at(hook, "vramWatchTick(") == tick + 4,
          "V11.hook: it is the hook's only call, so a frame is not sampled twice");
    // Between the beat and the call there are comment lines and nothing else: no `if` can hold the call back, in
    // either profile.
    bool onlyComments = beat != none && tick != none && beat < tick;
    if (onlyComments) {
        std::istringstream between(hook.substr(beat + 1, tick - beat - 1));
        std::string row;
        std::getline(between, row);   // the rest of the beat's own line
        while (std::getline(between, row)) {
            const size_t first = row.find_first_not_of(' ');
            if (first == std::string::npos) continue;
            if (row.compare(first, 2, "//") != 0) onlyComments = false;
        }
    }
    check(onlyComments && at(hook, "if (runtimeFlatProfile()) vramWatchTick") == none && at(hook, "if (!runtimeFlatProfile()) vramWatchTick") == none,
          "V11.hook: the call is unconditional (nothing but comments between the beat and it), so the flat profile has the watch too");
    check(at(hook, "#include \"vram_tick.h\"") != none, "V11.hook: device_hook.cpp includes the watch's header");
    const size_t due = at(glue, "if (!g_watcher.due(qpc)) return;");
    const size_t budget = at(glue, "guardedBudget(g_budget, [&] {");
    const size_t open = at(glue, "vramAdapterOf(device, &why, opening.adapter, sizeof(opening.adapter))");
    const size_t read = at(glue, "vramRead(g_adapter, &result)");
    const size_t write = at(glue, "Log::get().note(\"%s\", line)");
    check(due != none && budget != none && open != none && read != none && write != none && due < budget && budget < open && open < read && read < write,
          "V11.glue: the tick asks the cheap question first, then runs under the fault budget: open the device's adapter, read, write the log");
    check(at(bat, "\"src\\d3d11\\vram_tick.cpp\"") != none, "V11.build: build.bat compiles the glue into d3d11.dll");
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dryRun = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test") selfTest = true;
        else if (a == "--dry-run") dryRun = true;
        else if (selfTest && root.empty() && a.rfind("--", 0) != 0) root = a;
    }
    if (dryRun) {
        std::printf("[vram_watch_test] dry-run: touches nothing (no log, no file, no thread)\n");
        return 0;
    }
    if (!selfTest) {
        std::printf("usage: vram_watch_test.exe --dry-run | --self-test [root]\n");
        return 2;
    }
    constantCases();
    predicateCases();
    cadenceCases();
    pressureCases();
    crossingCases();
    capCases();
    robustnessCases();
    textCases();
    machineCases();
    queryCases();
    if (root.empty()) std::printf("[vram_watch_test] SKIPPED the V11 source pins: no repository root given\n");
    else sourcePins(root);
    if (g_failures) {
        std::printf("FAIL: vram watch: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u vram watch checks\n", g_checks);
    return 0;
}
