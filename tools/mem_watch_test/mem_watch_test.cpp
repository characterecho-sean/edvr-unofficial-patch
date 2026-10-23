// The rig for the process memory watch (docs/memory-growth-2026-10-22.md): the cadence and the text (src\common\mem_watch.h),
// the plausibility checks on what is read out of Elite's memory, the log's post-cap reserve accounting
// (src\common\log_reserve.h), and by source text the glue that wires them (device_hook.cpp, mem_tick.cpp, log.cpp, build.bat).
//
// Every check's label starts with its case id (M1..M11):
//
//   M1   the constants and the words the lines are keyed with
//   M2   the units: whole megabytes rounded, the percentage test, the machine's commit from GlobalMemoryStatusEx's fields
//   M3   the cadence: the armed line first whatever the figures, then a line every 60 s, woken every 5 s
//   M4   pressure: a line every 5 s while the machine's commit is at or over 90% of its limit, then back to 60 s; wake jitter,
//        an unreadable machine, a clock that went backwards
//   M5   the periodic and pressure text: every field, groups left out, egpu=unread and egpu=implausible, eheap likewise
//   M6   the armed text: it names each group once and why a group is missing
//   M7   every line fits its buffer, and the log's reserved line has room for it
//   M8   the plausibility checks on Elite's own figures, every rejection
//   M9   the runtime process names: case-insensitive, whole name, nothing else
//   M10  the log's post-cap reserve: 1 MiB, one notice, then nothing
//   M11  the glue, by source text: the Present hook calls memWatchTick once after the graphics memory watch's tick, every mem:
//        line is written through Log::noteReserved, the thread is never joined, build.bat compiles it, the log's exit path
//        is the one it was
//
//   mem_watch_test.exe --dry-run          touches nothing
//   mem_watch_test.exe --self-test [root]  root is the repository, for the M11 source pins (skipped without it)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "log_reserve.h"
#include "mem_watch.h"

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

size_t count(const std::string& text, const char* needle) {
    size_t n = 0;
    for (size_t from = text.find(needle); from != std::string::npos; from = text.find(needle, from + 1)) ++n;
    return n;
}

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;

MemMachine machineMiB(uint64_t commitMiB, uint64_t limitMiB, uint64_t availMiB = 12000) {
    MemMachine m;
    m.valid = true;
    m.commit = commitMiB * kMiB;
    m.commitLimit = limitMiB * kMiB;
    m.availPhys = availMiB * kMiB;
    return m;
}

// A machine at `pct` percent of a 40000 MiB commit limit.
MemMachine machineAt(uint64_t pct) { return machineMiB(pct * 400, 40000); }

// Every group present, at figures a session might show.
MemFigures typical() {
    MemFigures f;
    f.process.valid = true;
    f.process.commit = 9123 * kMiB;
    f.process.commitPeak = 9800 * kMiB;
    f.process.workingSet = 8010 * kMiB;
    f.handlesValid = true;
    f.handles = 2345;
    f.threadsValid = true;
    f.threads = 121;
    f.machine = machineMiB(20480, 40960, 12000);
    f.local.valid = true;
    f.local.used = 6000 * kMiB;
    f.local.budget = 11000 * kMiB;
    f.egpu.state = MemRead::Ok;
    const uint64_t mib[kMemGpuKinds] = {1000, 200, 300, 400, 2500, 100, 50, 300, 20, 5, 1, 0};
    for (uint32_t i = 0; i < kMemGpuKinds; ++i) f.egpu.kind[i] = mib[i] * kMiB;
    f.egpu.highWater = 7000 * kMiB;
    f.egpu.heaps = 12;
    f.egpu.descriptorHeaps = 3;
    f.egpu.heapUsed = 5000 * kMiB;
    f.egpu.heapCapacity = 6000 * kMiB;
    f.eheap.state = MemRead::Ok;
    f.eheap.heaps = 5;
    f.eheap.osBytes = 3000 * kMiB;
    f.eheap.mainRead = true;
    f.eheap.mainAccounted = true;
    f.eheap.mainCurrent = 1500 * kMiB;
    f.eheap.mainPeak = 2000 * kMiB;
    f.eheap.mainAllocs = 123456;
    f.runtime[0].running = true;
    f.runtime[0].commit = 900 * kMiB;
    f.runtime[0].handles = 3000;
    f.runtime[2].running = true;
    f.runtime[2].commit = 400 * kMiB;
    f.runtime[2].handles = 800;
    return f;
}

// Every figure at the largest a field holds and every group present, for the buffer.
MemFigures widest() {
    MemFigures f = typical();
    const uint64_t big = ~0ull;
    f.process.commit = f.process.commitPeak = f.process.workingSet = big;
    f.handles = f.threads = ~0u;
    f.machine.commit = f.machine.commitLimit = f.machine.availPhys = big;
    f.local.used = f.local.budget = big;
    for (uint32_t i = 0; i < kMemGpuKinds; ++i) f.egpu.kind[i] = big;
    f.egpu.highWater = big;
    f.egpu.heaps = f.egpu.descriptorHeaps = f.egpu.heapUsed = f.egpu.heapCapacity = big;
    f.eheap.heaps = f.eheap.osBytes = f.eheap.mainCurrent = f.eheap.mainPeak = big;
    f.eheap.mainAllocs = ~0u;
    for (int i = 0; i < kMemRuntimes; ++i) {
        f.runtime[i].running = true;
        f.runtime[i].commit = f.runtime[i].handles = big;
    }
    return f;
}

std::string periodic(const MemFigures& f, MemReason r = MemReason::Periodic) {
    char line[kMemLineCap];
    formatMemLine(line, sizeof(line), r, f);
    return line;
}

std::string armed(const MemFigures& f, const MemUnavailable& missing = MemUnavailable{}) {
    char line[kMemLineCap];
    formatMemArmed(line, sizeof(line), f, missing);
    return line;
}

// ---- M1 ----------------------------------------------------------------------------------------------------
void constantCases() {
    check(kMemWakeEveryMs == 5000 && kMemLineEveryMs == 60000 && kMemPressureEveryMs == 5000 && kMemPressurePercent == 90,
          "M1.constants: woken every 5 s, a line every 60 s, every 5 s from 90% of the commit limit");
    check(kMemSanityBytes == 64 * kGiB && kMemGpuKinds == 12 && kMemGpuHeapMax == 256 && kMemCpuHeapMax == 16,
          "M1.constants: the sanity limits are 64 GiB, twelve counters, 256 GPU heaps, 16 CPU heaps");
    check(std::string(memReasonKey(MemReason::Armed)) == "armed" && std::string(memReasonKey(MemReason::Periodic)) == "periodic" &&
              std::string(memReasonKey(MemReason::Pressure)) == "pressure",
          "M1.keys: the reason words are fixed: armed periodic pressure");
    const char* const kinds[] = {"rt", "ds", "texuav", "imm", "tex", "vb", "ib", "buf", "shader", "desc", "cmd", "unk"};
    bool same = true;
    for (uint32_t i = 0; i < kMemGpuKinds; ++i) same = same && std::string(memGpuKindKey(i)) == kinds[i];
    check(same, "M1.keys: Elite's twelve counters are rt ds texuav imm tex vb ib buf shader desc cmd unk, in that order");
    check(std::string(memRuntimeKey(0)) == "ovrserver" && std::string(memRuntimeKey(1)) == "vrserver" &&
              std::string(memRuntimeKey(2)) == "vrcompositor" && std::string(memRuntimeKey(3)) == "vdstreamer" &&
              std::string(memRuntimeImage(0)) == "OVRServer_x64.exe" && std::string(memRuntimeImage(1)) == "vrserver.exe" &&
              std::string(memRuntimeImage(2)) == "vrcompositor.exe" && std::string(memRuntimeImage(3)) == "VirtualDesktop.Streamer.exe",
          "M1.keys: the four runtimes and their images");
}

// ---- M2 ----------------------------------------------------------------------------------------------------
void unitCases() {
    check(memMb(0) == 0 && memMb(kMiB - 1) == 1 && memMb(kMiB) == 1 && memMb(kMiB / 2 - 1) == 0 && memMb(kMiB / 2) == 1,
          "M2.mb: whole megabytes rounded to the nearest, a half up (512 KiB is 1, 512 KiB less a byte is 0)");
    check(memMb(kMiB + kMiB / 2 - 1) == 1 && memMb(kMiB + kMiB / 2) == 2 && memMb(9123 * kMiB + 300 * 1024) == 9123 &&
              memMb(9123 * kMiB + 600 * 1024) == 9124,
          "M2.mb: 1.5 MiB is 2, a byte less is 1; 9123 MiB and 300 KiB is 9123, and 600 KiB is 9124");
    check(memMb(~0ull) == (1ull << 44), "M2.mb: the largest byte count does not overflow in the rounding (2^44 MB)");
    check(memAtOrOverPercent(90, 100, 90) && !memAtOrOverPercent(89, 100, 90) && memAtOrOverPercent(100, 100, 90) &&
              memAtOrOverPercent(101, 100, 90),
          "M2.percent: exactly 90% is at 90%, 89% is not, over the whole is");
    check(!memAtOrOverPercent(5, 0, 90) && !memAtOrOverPercent(0, 0, 90), "M2.percent: a total of 0 is nothing to be over");
    check(memAtOrOverPercent(90 * kGiB, 100 * kGiB, 90) && !memAtOrOverPercent(90 * kGiB - kMiB, 100 * kGiB, 90),
          "M2.percent: 90 GiB of 100 is at 90%, a megabyte less is not");
    const uint64_t huge = ~0ull / 50;   // 100 * huge overflows
    check(memAtOrOverPercent(huge, huge + 1000, 90) && !memAtOrOverPercent(huge / 2, huge, 90),
          "M2.percent: figures too large to multiply by 100 are still compared");
    const MemMachine m = memMachineOf(40960 * kMiB, 20480 * kMiB, 12000 * kMiB);
    check(m.valid && m.commit == 20480 * kMiB && m.commitLimit == 40960 * kMiB && m.availPhys == 12000 * kMiB,
          "M2.machine: commit is the page file total less what is free of it, the limit is the total, available is RAM");
    const MemMachine odd = memMachineOf(100, 200, 5);
    check(odd.valid && odd.commit == 0, "M2.machine: more free than total (a reading taken between two changes) is a commit of 0, not a wrapped number");
}

// ---- M3 ----------------------------------------------------------------------------------------------------
// The thread's loop: a wake every 5 s from t = 0 with the machine given; the seconds a line was due at.
std::vector<unsigned> wakes(MemPolicy& p, unsigned fromSecond, unsigned toSecond, const MemMachine& m, std::vector<MemReason>* kinds = nullptr,
                            unsigned step = 5) {
    std::vector<unsigned> out;
    for (unsigned t = fromSecond; t <= toSecond; t += step) {
        const MemReason r = p.observe(t * 1000ull, m);
        if (r != MemReason::None) {
            out.push_back(t);
            if (kinds) kinds->push_back(r);
        }
    }
    return out;
}

void cadenceCases() {
    MemPolicy p;
    std::vector<MemReason> kinds;
    const std::vector<unsigned> lines = wakes(p, 0, 200, machineAt(50), &kinds);
    check(lines == std::vector<unsigned>({0, 60, 120, 180}),
          "M3.cadence: a machine at 50% woken every 5 s over 200 s: the armed line at 0, then 60, 120 and 180 s and no other");
    check(kinds.size() == 4 && kinds[0] == MemReason::Armed && kinds[1] == MemReason::Periodic && kinds[2] == MemReason::Periodic &&
              kinds[3] == MemReason::Periodic,
          "M3.kinds: the first is the armed line and each after it is periodic");
    check(p.armed() && p.lines() == 4, "M3.count: four lines counted");

    MemPolicy blind;
    check(blind.observe(7000, MemMachine{}) == MemReason::Armed && blind.armed(),
          "M3.arm: the first wake is the armed line even when the machine could not be read");
    check(blind.observe(7000 + 59000, MemMachine{}) == MemReason::None && blind.observe(7000 + 60000, MemMachine{}) == MemReason::Periodic,
          "M3.arm: and an unreadable machine keeps the 60 s cadence");

    MemPolicy atLimit;
    check(atLimit.observe(0, machineAt(99)) == MemReason::Armed, "M3.arm: a machine already under pressure still arms with the armed line, not a pressure line");

    MemPolicy late;
    late.observe(0, machineAt(50));
    check(late.observe(70000, machineAt(50)) == MemReason::Periodic && late.observe(75000, machineAt(50)) == MemReason::None &&
              late.observe(130000, machineAt(50)) == MemReason::Periodic,
          "M3.late: a wake that arrives late (70 s) is the line, and the next is 60 s after it, not after the missed one");

    MemPolicy slack;
    slack.observe(0, machineAt(50));
    check(slack.observe(59499, machineAt(50)) == MemReason::None && slack.observe(59500, machineAt(50)) == MemReason::Periodic,
          "M3.slack: a wake half a second early (a timer rounding down) takes the line; 501 ms early does not");
}

// ---- M4 ----------------------------------------------------------------------------------------------------
void pressureCases() {
    MemPolicy p;
    p.observe(0, machineAt(50));
    std::vector<unsigned> lines;
    std::vector<MemReason> kinds;
    for (unsigned t = 5; t <= 140; t += 5) {
        const MemMachine m = (t >= 25 && t < 50) ? machineAt(95) : machineAt(50);
        const MemReason k = p.observe(t * 1000ull, m);
        if (k != MemReason::None) {
            lines.push_back(t);
            kinds.push_back(k);
        }
    }
    check(lines == std::vector<unsigned>({25, 30, 35, 40, 45, 105}),
          "M4.pressure: a line at the first wake that sees 95% (25 s), one every 5 s while it stays, none once it eases, and the "
          "next 60 s after the last (105 s)");
    check(kinds.size() == 6 && kinds[0] == MemReason::Pressure && kinds[4] == MemReason::Pressure && kinds[5] == MemReason::Periodic,
          "M4.kinds: the lines under pressure are pressure lines; the one after it eases is periodic");

    MemPolicy edge;
    edge.observe(0, machineAt(50));
    check(edge.observe(5000, machineAt(90)) == MemReason::Pressure, "M4.edge: exactly 90% is pressure");
    MemPolicy below;
    below.observe(0, machineMiB(35999, 40000));
    check(below.observe(5000, machineMiB(35999, 40000)) == MemReason::None && below.observe(60000, machineMiB(35999, 40000)) == MemReason::Periodic,
          "M4.edge: 89.9975% is not: it keeps the 60 s cadence");
    check(machineAt(90).valid && memUnderPressure(machineAt(90)) && !memUnderPressure(machineAt(89)) && !memUnderPressure(MemMachine{}) &&
              !memUnderPressure(machineMiB(5000, 0)),
          "M4.edge: a machine that could not be read, or reports no limit, is not under pressure");

    MemPolicy staying;
    staying.observe(0, machineAt(95));
    check(staying.observe(1000, machineAt(95)) == MemReason::None && staying.observe(5000, machineAt(95)) == MemReason::Pressure,
          "M4.armed-at-pressure: a machine already at 95% when the watch arms writes its first pressure line 5 s after the armed line");

    // A thread woken every 5 s by Sleep gets 5001 or 4990 ms on a clock with 15 ms steps; the 5 s cadence must not skip a wake.
    MemPolicy jitter;
    jitter.observe(0, machineAt(95));
    uint64_t t = 0;
    unsigned fired = 0;
    const int jitterMs[] = {5001, 4990, 5016, 4985, 5000, 5031, 4969, 5000};
    for (int step : jitterMs) {
        t += static_cast<uint64_t>(step);
        if (jitter.observe(t, machineAt(95)) == MemReason::Pressure) ++fired;
    }
    check(fired == 8, "M4.jitter: eight wakes within 30 ms of 5 s under pressure are eight lines, none skipped");

    MemPolicy backwards;
    backwards.observe(100000, machineAt(50));
    check(backwards.observe(50000, machineAt(50)) == MemReason::None && backwards.observe(109000, machineAt(50)) == MemReason::None &&
              backwards.observe(110000, machineAt(50)) == MemReason::Periodic,
          "M4.backwards: a clock that went backwards restarts the interval from the new reading");
}

// ---- M5 ----------------------------------------------------------------------------------------------------
void textCases() {
    const std::string full =
        "mem: reason=periodic commit_mb=9123 commit_peak_mb=9800 ws_mb=8010 handles=2345 threads=121 sys_commit_mb=20480 "
        "sys_commit_limit_mb=40960 sys_avail_mb=12000 local_used_mb=6000 local_budget_mb=11000 egpu_mb=4876 egpu_peak_mb=7000 "
        "egpu_rt_mb=1000 egpu_ds_mb=200 egpu_texuav_mb=300 egpu_imm_mb=400 egpu_tex_mb=2500 egpu_vb_mb=100 egpu_ib_mb=50 egpu_buf_mb=300 "
        "egpu_shader_mb=20 egpu_desc_mb=5 egpu_cmd_mb=1 egpu_unk_mb=0 egpu_heaps=12 egpu_desc_heaps=3 egpu_heap_used_mb=5000 "
        "egpu_heap_cap_mb=6000 eheap_heaps=5 eheap_os_mb=3000 eheap_main_mb=1500 eheap_main_peak_mb=2000 eheap_main_allocs=123456 "
        "ovrserver_commit_mb=900 ovrserver_handles=3000 vrcompositor_commit_mb=400 vrcompositor_handles=800";
    const std::string got = periodic(typical());
    if (got != full) std::printf("  got:      %s\n  expected: %s\n", got.c_str(), full.c_str());
    check(got == full, "M5.line: a periodic line with every group, in the documented order, as key=value pairs");
    check(periodic(typical(), MemReason::Pressure).rfind("mem: reason=pressure commit_mb=9123 ", 0) == 0,
          "M5.reason: a pressure line starts the same way with its own reason");
    check(!contains(got, "\n") && !contains(got, "\r") && !contains(got, ";"), "M5.line: one line, no newline, none of the armed line's prose");

    MemFigures f = typical();
    f.process.valid = false;
    check(!contains(periodic(f), " commit_mb=") && !contains(periodic(f), "commit_peak_mb=") && !contains(periodic(f), "ws_mb=") &&
              contains(periodic(f), " handles=2345 threads=121 "),
          "M5.omit: a process group that could not be read leaves out commit, peak and working set, and keeps the counts that were read");
    f = typical();
    f.handlesValid = false;
    f.threadsValid = false;
    check(!contains(periodic(f), " handles=") && !contains(periodic(f), "threads=") && contains(periodic(f), "commit_mb=9123 commit_peak_mb=9800 ws_mb=8010 sys_commit"),
          "M5.omit: counts that were not read are left out, one by one");
    f = typical();
    f.machine.valid = false;
    check(!contains(periodic(f), "sys_"), "M5.omit: an unreadable machine leaves out the three sys_ fields");
    f = typical();
    f.local.valid = false;
    check(!contains(periodic(f), "local_"), "M5.omit: an adapter without video memory figures leaves out the local fields");
    f = typical();
    f.egpu.state = MemRead::Absent;
    f.eheap.state = MemRead::Absent;
    check(!contains(periodic(f), "egpu") && !contains(periodic(f), "eheap"), "M5.omit: groups of Elite's books that were never available are not mentioned");
    f = typical();
    for (int i = 0; i < kMemRuntimes; ++i) f.runtime[i].running = false;
    check(!contains(periodic(f), "ovrserver") && !contains(periodic(f), "vrserver") && !contains(periodic(f), "vrcompositor") &&
              !contains(periodic(f), "vdstreamer") && periodic(f).size() > 100,
          "M5.omit: runtimes that are not running are not written");
    f = typical();
    f.runtime[1].running = true;
    f.runtime[1].commit = 77 * kMiB;
    f.runtime[1].handles = 12;
    f.runtime[3].running = true;
    f.runtime[3].commit = 5 * kMiB;
    f.runtime[3].handles = 1;
    const std::string four = periodic(f);
    check(contains(four, "ovrserver_commit_mb=900 ovrserver_handles=3000 vrserver_commit_mb=77 vrserver_handles=12 vrcompositor_commit_mb=400 "
                         "vrcompositor_handles=800 vdstreamer_commit_mb=5 vdstreamer_handles=1"),
          "M5.runtime: all four, in the order ovrserver vrserver vrcompositor vdstreamer, commit then handles");

    f = typical();
    f.egpu.state = MemRead::Unread;
    check(contains(periodic(f), " egpu=unread ") && !contains(periodic(f), "egpu_"), "M5.egpu: a game with no device yet is egpu=unread, with none of its numbers");
    f.egpu.state = MemRead::Implausible;
    check(contains(periodic(f), " egpu=implausible ") && !contains(periodic(f), "egpu_") && contains(periodic(f), "eheap_heaps=5"),
          "M5.egpu: values that failed their checks are egpu=implausible, with none of its numbers, and the other groups still written");
    f = typical();
    f.eheap.state = MemRead::Implausible;
    check(contains(periodic(f), " eheap=implausible ") && !contains(periodic(f), "eheap_") && contains(periodic(f), "egpu_mb=4876"),
          "M5.eheap: eheap=implausible likewise");
    f.eheap.state = MemRead::Unread;
    check(contains(periodic(f), " eheap=unread "), "M5.eheap: a manager that is not there yet is eheap=unread");
    f = typical();
    f.eheap.mainAccounted = false;
    check(contains(periodic(f), "eheap_os_mb=3000 eheap_main=untracked ") && !contains(periodic(f), "eheap_main_mb"),
          "M5.eheap: a default allocator whose statistics block does not accumulate is said, not written as zeros");
    f.eheap.mainRead = false;
    check(contains(periodic(f), "eheap_os_mb=3000 eheap_main=unread ") && !contains(periodic(f), "eheap_main_mb"),
          "M5.eheap: a default heap that was not found is said, and the heap table's figures are still written");

    f = typical();
    f.egpu.kind[0] = 5 * kMiB + kMiB / 2;
    f.egpu.kind[1] = 5 * kMiB + kMiB / 2 - 1;
    f.egpu.kind[2] = 0;
    f.egpu.kind[3] = 0;
    f.egpu.kind[4] = 0;
    check(contains(periodic(f), "egpu_rt_mb=6 egpu_ds_mb=5 egpu_texuav_mb=0"), "M5.round: 5.5 MiB is 6 and 5.5 MiB less a byte is 5, in the kinds as everywhere");
    f = typical();
    for (uint32_t i = 0; i < kMemGpuKinds; ++i) f.egpu.kind[i] = kMiB / 3;   // each rounds to 0, the sum to 4
    check(contains(periodic(f), "egpu_mb=4 ") && contains(periodic(f), "egpu_rt_mb=0 "), "M5.round: the total is of the bytes, not of the rounded kinds");
}

// ---- M6 ----------------------------------------------------------------------------------------------------
void armedCases() {
    const std::string a = armed(typical());
    check(a.rfind("mem: reason=armed commit_mb=9123 ", 0) == 0 && contains(a, "ovrserver_handles=3000"),
          "M6.armed: the armed line starts with its reason and carries the first sample's figures");
    check(contains(a, "commit is this process's private bytes") && contains(a, "egpu is Elite's own accounting") &&
              contains(a, "read only on build 334473") && contains(a, "eheap is a first reading of Elite's heap manager, not yet checked against the process figures") &&
              contains(a, "The runtime fields cover named VR runtime processes"),
          "M6.explain: each field group is explained once: commit, egpu (and the build it is read on), eheap (not yet checked), the runtimes");
    check(count(a, "commit is this process's") == 1 && count(a, "egpu is Elite's own accounting") == 1 && count(a, "eheap is a first reading") == 1 &&
              count(a, "The runtime fields cover") == 1,
          "M6.explain: and only once");
    check(contains(a, "A line every 60 s, every 5 s while the machine's commit is at or over 90% of its limit."),
          "M6.cadence: the cadence is said, from the constants");
    check(contains(a, "egpu=unread is a game with no device yet and egpu=implausible values that failed their checks"),
          "M6.explain: what egpu=unread and egpu=implausible mean");
    check(!contains(a, "unreadable, so left out") && !contains(a, "\n"), "M6.missing: with every group read, no group is said to be missing; one line");

    MemFigures f = typical();
    f.egpu.state = MemRead::Absent;
    f.eheap.state = MemRead::Absent;
    f.local.valid = false;
    f.threadsValid = false;
    for (int i = 0; i < kMemRuntimes; ++i) f.runtime[i].running = false;
    MemUnavailable missing;
    missing.why[static_cast<int>(MemGroup::Egpu)] = "this is not Elite build 334473 (PE timestamp or image size differs)";
    missing.why[static_cast<int>(MemGroup::Eheap)] = "the instructions at +0x5373A0 are not those of build 334473";
    missing.why[static_cast<int>(MemGroup::Vram)] = "IDXGIAdapter3 is not offered";
    missing.why[static_cast<int>(MemGroup::Scan)] = "ntdll does not export NtQuerySystemInformation";
    const std::string b = armed(f, missing);
    check(contains(b, "; unreadable, so left out of every line: local (IDXGIAdapter3 is not offered); processes (ntdll does not export NtQuerySystemInformation); "
                      "egpu (this is not Elite build 334473 (PE timestamp or image size differs)); eheap (the instructions at +0x5373A0 are not those of build 334473); "),
          "M6.missing: every group that could not be read is named once with its reason, in group order, in the armed line");
    check(!contains(b, " egpu_") && !contains(b, " eheap_") && !contains(b, "local_used") && !contains(b, "threads="),
          "M6.missing: ...and none of their fields is written");
    check(count(b, "unreadable, so left out") == 1, "M6.missing: the preface is said once");

    char tiny[40];
    const size_t n = formatMemArmed(tiny, sizeof(tiny), typical(), MemUnavailable{});
    check(n == sizeof(tiny) - 1 && tiny[sizeof(tiny) - 1] == 0 && std::strlen(tiny) == n, "M6.cut: a buffer too small is filled and terminated, never overrun");
    check(formatMemArmed(nullptr, 10, typical(), MemUnavailable{}) == 0 && formatMemLine(tiny, 0, MemReason::Periodic, typical()) == 0,
          "M6.cut: no buffer, no line");
    char unavailable[300];
    formatMemUnavailable(unavailable, sizeof(unavailable), "the sampling thread could not be started");
    check(std::string(unavailable) == "mem: unavailable -- the sampling thread could not be started. No mem: line will be written this session.",
          "M6.unavailable: the line written once when the thread cannot start");
}

// ---- M7 ----------------------------------------------------------------------------------------------------
void fitCases() {
    const std::string w = periodic(widest());
    check(w.size() + 1 < kMemLineCap && !contains(w, "...") && contains(w, "vdstreamer_handles=18446744073709551615"),
          "M7.fit: the widest periodic line (every group, every figure at 2^64-1) fits its buffer whole");
    check(count(w, " ") > 38, "M7.fit: ...and is all there (every field)");

    const std::string wa = armed(widest());
    check(wa.size() + 1 < kMemLineCap && wa.size() > 2000 && wa.compare(wa.size() - 15, 15, "% of its limit.") == 0,
          "M7.fit: the widest armed line (every group present, every figure at 2^64-1) fits its buffer whole, ending with the cadence sentence");

    // Groups that are missing have no figures: the widest armed line with all six missing and a reason of 400 characters each.
    MemFigures none;
    MemUnavailable all;
    const std::string longWhy(400, 'x');
    for (int g = 0; g < kMemGroups; ++g) all.why[g] = longWhy.c_str();
    const std::string a = armed(none, all);
    check(a.size() + 1 < kMemLineCap && a.size() > 1500 && a.compare(a.size() - 15, 15, "% of its limit.") == 0,
          "M7.fit: the widest armed line with six missing groups, each with a 400-character reason, fits its buffer whole");
    check(contains(a, std::string(160, 'x').c_str()) && !contains(a, std::string(161, 'x').c_str()),
          "M7.fit: a reason is held to 160 characters");

    // Gate failures of the two Elite groups with the rest present at the widest.
    MemFigures mixed = widest();
    mixed.egpu.state = MemRead::Absent;
    mixed.eheap.state = MemRead::Absent;
    MemUnavailable gate;
    gate.why[static_cast<int>(MemGroup::Egpu)] = longWhy.c_str();
    gate.why[static_cast<int>(MemGroup::Eheap)] = longWhy.c_str();
    const std::string m = armed(mixed, gate);
    check(m.size() + 1 < kMemLineCap && m.compare(m.size() - 15, 15, "% of its limit.") == 0,
          "M7.fit: the widest armed line with the two Elite groups missing fits its buffer whole");

    const std::string empty = armed(none, MemUnavailable{});
    check(empty.size() + 1 < kMemLineCap && empty.rfind("mem: reason=armed; sizes are whole MB", 0) == 0,
          "M7.fit: an armed line with no figures at all is still the armed line");
    // The log gives a reserved line this much room: the stamp ("[hh:mm:ss.mmm] ", 15), the line end (2) and the truncation marker
    // (15, and 3 more) go with the text.
    check(kMemLineCap + 15 + 2 + 18 <= kLogReservedLineBytes, "M7.log: a line of the buffer's size, stamped and ended, fits the log's reserved line without its truncation marker");
    check(kLogReserveBytes == 1024ull * 1024ull, "M7.log: the reserve is 1 MiB");
}

// ---- M8 ----------------------------------------------------------------------------------------------------
void plausibilityCases() {
    uint64_t first[kMemGpuKinds] = {}, second[kMemGpuKinds] = {};
    for (uint32_t i = 0; i < kMemGpuKinds; ++i) first[i] = second[i] = (i + 1) * kMiB;   // 78 MiB
    uint64_t sum = 0;
    for (uint32_t i = 0; i < kMemGpuKinds; ++i) sum += first[i];
    check(memGpuCountersPlausible(first, second, sum), "M8.gpu: counters that agree, with the high water equal to their sum, are plausible");
    check(memGpuCountersPlausible(first, second, sum + 1000 * kGiB / 1000), "M8.gpu: a high water above the sum is the normal case");
    uint64_t zero[kMemGpuKinds] = {};
    check(memGpuCountersPlausible(zero, zero, 0), "M8.gpu: a game that has allocated nothing is plausible");
    check(!memGpuCountersPlausible(first, second, sum - 1), "M8.gpu: a high water below the sum is refused");
    uint64_t flagged[kMemGpuKinds];
    std::memcpy(flagged, second, sizeof(flagged));
    flagged[4] = first[4] + 1;
    check(!memGpuCountersPlausible(first, flagged, sum), "M8.gpu: a flagged-out figure above its whole is refused");
    flagged[4] = first[4];
    check(memGpuCountersPlausible(first, flagged, sum), "M8.gpu: ...and equal to it is not");
    uint64_t big[kMemGpuKinds];
    std::memcpy(big, first, sizeof(big));
    big[2] = kMemSanityBytes;
    check(!memGpuCountersPlausible(big, zero, ~0ull), "M8.gpu: a counter of 64 GiB is refused");
    uint64_t eight[kMemGpuKinds] = {};
    for (uint32_t i = 0; i < 8; ++i) eight[i] = 8 * kGiB;   // no counter near the limit, 64 GiB in all
    check(!memGpuCountersPlausible(eight, zero, 64 * kGiB) && !memGpuCountersPlausible(eight, zero, 100 * kGiB),
          "M8.gpu: counters that add to 64 GiB are refused, whatever the high water says");
    uint64_t under[kMemGpuKinds] = {};
    for (uint32_t i = 0; i < 8; ++i) under[i] = 8 * kGiB - kMiB;   // 64 GiB less 8 MiB
    check(memGpuCountersPlausible(under, zero, 8 * (8 * kGiB - kMiB)) && memGpuCountersPlausible(under, under, 64 * kGiB - 4 * kMiB),
          "M8.gpu: counters that add to just under 64 GiB are plausible");
    check(!memGpuCountersPlausible(under, zero, 64 * kGiB), "M8.gpu: a high water of 64 GiB is refused");
    uint64_t wrapped[kMemGpuKinds];
    std::memcpy(wrapped, first, sizeof(wrapped));
    wrapped[7] = 0 - 4096ull;   // a counter that was taken below zero
    check(!memGpuCountersPlausible(wrapped, zero, ~0ull - 1), "M8.gpu: a counter that went below zero (2^64 - 4096) is refused, with no overflow in the sum");
    check(!memGpuCountersPlausible(first, second, ~0ull), "M8.gpu: a high water of 2^64-1 is refused");

    check(memGpuHeapCountPlausible(0) && memGpuHeapCountPlausible(256) && !memGpuHeapCountPlausible(257) && !memGpuHeapCountPlausible(~0ull),
          "M8.heaps: 0 to 256 GPU heaps; 257 is refused");
    check(memGpuHeapPlausible(0, 0) && memGpuHeapPlausible(5, 5) && !memGpuHeapPlausible(6, 5) && !memGpuHeapPlausible(1, kMemSanityBytes) &&
              memGpuHeapPlausible(1, kMemSanityBytes - 1),
          "M8.heaps: a heap uses no more than it holds, and holds under 64 GiB");

    check(!memCpuHeapCountPlausible(0) && memCpuHeapCountPlausible(1) && memCpuHeapCountPlausible(16) && !memCpuHeapCountPlausible(17) &&
              !memCpuHeapCountPlausible(~0ull),
          "M8.cpu: 1 to 16 heaps; 0 and 17 are refused");
    check(memCpuOsBytesPlausible(0) && memCpuOsBytesPlausible(kMemSanityBytes - 1) && !memCpuOsBytesPlausible(kMemSanityBytes),
          "M8.cpu: bytes from the OS under 64 GiB");
    check(memCpuMainPlausible(0, 0) && memCpuMainPlausible(5, 5) && memCpuMainPlausible(4, 5) && !memCpuMainPlausible(6, 5),
          "M8.cpu: the default allocator's current bytes do not pass its peak");
}

// ---- M9 ----------------------------------------------------------------------------------------------------
int indexOf(const wchar_t* name) {
    size_t n = 0;
    while (name[n]) ++n;
    return memRuntimeIndexOf(name, n);
}

void nameCases() {
    check(indexOf(L"OVRServer_x64.exe") == 0 && indexOf(L"ovrserver_x64.exe") == 0 && indexOf(L"OVRSERVER_X64.EXE") == 0,
          "M9.name: OVRServer_x64.exe in any case");
    check(indexOf(L"vrserver.exe") == 1 && indexOf(L"VRServer.exe") == 1 && indexOf(L"vrcompositor.exe") == 2 && indexOf(L"VRCompositor.EXE") == 2 &&
              indexOf(L"VirtualDesktop.Streamer.exe") == 3 && indexOf(L"virtualdesktop.streamer.exe") == 3,
          "M9.name: vrserver, vrcompositor and VirtualDesktop.Streamer.exe in any case");
    check(indexOf(L"vrserver") == -1 && indexOf(L"vrserver.exe2") == -1 && indexOf(L"xvrserver.exe") == -1 && indexOf(L"vrserver.ex") == -1 &&
              indexOf(L"") == -1 && indexOf(L"EliteDangerous64.exe") == -1 && indexOf(L"vrmonitor.exe") == -1 && indexOf(L"OVRServer_x86.exe") == -1,
          "M9.name: the whole name or nothing: a prefix, a suffix, another process, an empty name");
    check(memRuntimeIndexOf(nullptr, 5) == -1, "M9.name: no name is no match");
    const wchar_t lengthOnly[] = L"vrserver.exe-and-more";
    check(memRuntimeIndexOf(lengthOnly, 12) == 1 && memRuntimeIndexOf(lengthOnly, 13) == -1,
          "M9.name: the length given is the length compared (a name in a buffer is not NUL-terminated)");
    const wchar_t extended[] = L"vrserverı.exe";   // a dotless i is not an ASCII letter
    check(memRuntimeIndexOf(extended, 12) == -1, "M9.name: only ASCII folds");
}

// ---- M10 ---------------------------------------------------------------------------------------------------
void reserveCases() {
    LogReserve r;
    check(!r.spent() && r.used() == 0, "M10.start: a fresh reserve is unspent");
    check(r.admit(100) == ReserveVerdict::Write && r.used() == 100 && !r.spent(), "M10.write: a line within the reserve is written");
    check(r.admit(0) == ReserveVerdict::Write && r.used() == 100, "M10.write: an empty line costs nothing");
    check(r.admit(kLogReserveBytes - 100) == ReserveVerdict::Write && r.used() == kLogReserveBytes && !r.spent(),
          "M10.fit: a line that brings the reserve to exactly 1 MiB is written");
    check(r.admit(1) == ReserveVerdict::Spent && r.spent(), "M10.spent: the first line over says Spent");
    check(r.admit(1) == ReserveVerdict::Skip && r.admit(0) == ReserveVerdict::Skip && r.admit(kLogReserveBytes) == ReserveVerdict::Skip && r.used() == kLogReserveBytes,
          "M10.spent: and every line after it, whatever its size, is skipped, without taking more of the reserve");
    r.reset();
    check(!r.spent() && r.used() == 0 && r.admit(kLogReserveBytes) == ReserveVerdict::Write, "M10.reset: a new log file starts a new reserve");

    LogReserve lines;
    unsigned written = 0, notices = 0, skipped = 0;
    for (unsigned i = 0; i < 5000; ++i) {
        switch (lines.admit(600)) {
            case ReserveVerdict::Write: ++written; break;
            case ReserveVerdict::Spent: ++notices; break;
            case ReserveVerdict::Skip: ++skipped; break;
        }
    }
    check(written == kLogReserveBytes / 600 && notices == 1 && skipped == 5000 - written - 1,
          "M10.session: 600-byte lines until the reserve cannot take another: 1747 are written, one notice, the rest skipped");
    LogReserve small;
    check(small.admit(kLogReserveBytes + 1) == ReserveVerdict::Spent && small.admit(10) == ReserveVerdict::Skip,
          "M10.spent: a line larger than the whole reserve spends it at once, and nothing after it is written (the notice stands for the rest)");
    LogReserve edge;
    edge.admit(kLogReserveBytes - 10);
    check(edge.admit(11) == ReserveVerdict::Spent && edge.admit(1) == ReserveVerdict::Skip,
          "M10.spent: a line that does not fit spends the reserve even though a shorter one would have: the order of the lines is kept");
}

// ---- M11 ---------------------------------------------------------------------------------------------------
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

size_t at(const std::string& text, const char* needle, size_t from = 0) { return text.find(needle, from); }

std::string between(const std::string& text, const char* from, const char* to) {
    const size_t a = text.find(from);
    if (a == std::string::npos) return "";
    const size_t b = text.find(to, a + 1);
    return b == std::string::npos ? "" : text.substr(a, b - a);
}

void sourcePins(const std::string& root) {
    const std::string hook = slurp(root + "\\src\\d3d11\\device_hook.cpp");
    const std::string glue = slurp(root + "\\src\\d3d11\\mem_tick.cpp");
    const std::string glueHeader = slurp(root + "\\src\\d3d11\\mem_tick.h");
    const std::string bat = slurp(root + "\\build.bat");
    const std::string logCpp = slurp(root + "\\src\\common\\log.cpp");
    const std::string logH = slurp(root + "\\src\\common\\log.h");
    check(!hook.empty() && !glue.empty() && !glueHeader.empty() && !bat.empty() && !logCpp.empty() && !logH.empty(),
          "M11.read: the six sources the glue and the log live in were read");
    if (hook.empty() || glue.empty() || glueHeader.empty() || bat.empty() || logCpp.empty() || logH.empty()) return;
    const size_t none = std::string::npos;

    const std::string pair = "    vramWatchTick(presentT1, dev);\n    memWatchTick(dev);";
    const size_t both = at(hook, pair.c_str());
    const size_t ticks = at(hook, "g_frameTicks.markAt(\"present_pre\", presentT0);");
    check(both != none && ticks != none && both < ticks && count(hook, "    vramWatchTick(presentT1, dev);\n") == 1,
          "M11.hook: hookedPresent calls memWatchTick(dev) on the line directly after the graphics memory watch's tick, before the frame ticks");
    check(both != none && count(hook, "memWatchTick(") == 1,
          "M11.hook: it is the hook's only call, and a statement of its own at the top of the function (no if before it), so the thread is "
          "started once and the flat profile has the watch too");
    check(at(hook, "#include \"mem_tick.h\"") != none, "M11.hook: device_hook.cpp includes the glue's header");
    check(at(glueHeader, "void memWatchTick(ID3D11Device* device);") != none, "M11.hook: the header declares memWatchTick(ID3D11Device*)");

    // The glue: the one atomic read first, then the start under a fault guard; the thread is a detached thread with a name
    // that is never joined; every mem: line goes through noteReserved and none through note.
    const size_t relaxed = at(glue, "g_started.load(std::memory_order_relaxed)");
    const size_t cas = at(glue, "g_started.compare_exchange_strong(expected, true)");
    const size_t guard = at(glue, "guarded(\"memWatch/start\"");
    check(relaxed != none && cas != none && guard != none && relaxed < cas && cas < guard,
          "M11.glue: memWatchTick reads one atomic first, then claims the start once, then starts under a fault guard");
    check(at(glue, "std::thread(&samplerMain).detach()") != none && at(glue, "SetThreadDescription(GetCurrentThread(), L\"edvr-mem\")") != none &&
              at(glue, ".join(") == none && at(glue, "TerminateThread") == none && at(glue, "WaitForSingleObject") == none,
          "M11.glue: one detached, named thread that is never joined or killed");
    check(at(glue, "Log::get().noteReserved(") != none && at(glue, "Log::get().note(") == none && at(glue, ".note(") == none,
          "M11.glue: every line the glue writes is written through noteReserved, none through note");
    check(count(glue, "noteReserved(") >= 3 && at(glue, "Log::get().noteReserved(\"%s\", line)") != none,
          "M11.glue: the sample, the thread's failure and the start's failure are all written that way");
    check(at(glue, "kEliteStamp = 1791840573u") != none && at(glue, "kEliteImageSize = 104906752u") != none,
          "M11.glue: the build identity is the pair every build-bound hook here checks (timestamp 1791840573, image size 104906752)");
    check(at(glue, "__declspec(noinline) bool sehRead(") != none && at(glue, "__declspec(noinline) bool sehMatches(") != none,
          "M11.glue: the reads of the game's memory are noinline SEH helpers");
    // Every dereference of the game's memory goes through sehRead: no raw pointer cast of a game address in the file.
    check(at(glue, "reinterpret_cast<const uint64_t*>") == none && at(glue, "*reinterpret_cast<uint64_t*>") == none,
          "M11.glue: no raw read of the game's memory outside the SEH helpers");
    check(at(glue, "GetProcAddress(ntdll, \"NtQuerySystemInformation\")") != none && at(glue, "GetProcessMemoryInfo(") != none &&
              at(glue, "psapi.lib") == none,
          "M11.glue: the process list comes from ntdll by GetProcAddress, the process counters from GetProcessMemoryInfo (no psapi.lib)");
    check(at(glue, "kSystemProcessInformation = 5") != none && at(glue, "kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004)") != none &&
              at(glue, "HeapReAlloc(") != none,
          "M11.glue: one process-list buffer, grown on a length mismatch");
    // Nothing but the four runtimes' own keys is ever written about another process.
    check(at(glue, "ImageNameBuffer") != none && count(glue, "memRuntimeIndexOf(") == 1,
          "M11.glue: a process's name is compared with the four runtimes' and goes no further");

    // The pins: every one of the two groups' tables is there with the RVAs the offsets were read at.
    const char* const rvas[] = {"0x51FB32", "0x51FB9C", "0x51FBA9", "0x51D746", "0x51D755", "0x519D52", "0x519D5C", "0x519D88", "0x519E0B", "0x51A0EB",
                                "0x537385", "0x5373A0", "0x543FED", "0x5536B3", "0x5404AE", "0x5404D1", "0x54C9DF", "0x55B695"};
    bool allPins = true;
    for (const char* rva : rvas) allPins = allPins && count(glue, (std::string("{") + rva + ",").c_str()) == 1;
    check(allPins, "M11.pins: the eighteen byte pins of the two groups are each in the table once, at the RVA they were read at");
    check(at(glue, "kRendererGlobalRva = 0x5F1DCA8") != none && at(glue, "kManagerGlobalRva = 0x5F1DE58") != none && at(glue, "kManagerObjectRva = 0x5F28120") != none,
          "M11.pins: the two globals and the manager's static object");

    // build.bat: the glue is on a line of its own in d3d11.dll's source list, and the rig has its label.
    bool glueLine = false, rigLabel = false;
    {
        std::istringstream rows(bat);
        std::string row;
        while (std::getline(rows, row)) {
            if (!row.empty() && row.back() == '\r') row.pop_back();
            if (row == "    \"src\\d3d11\\mem_tick.cpp\" ^") glueLine = true;
            if (row == ":rig_mem_watch_test") rigLabel = true;
        }
    }
    check(glueLine, "M11.build: build.bat compiles the glue into d3d11.dll, on a line of its own");
    check(rigLabel && at(bat, "tools\\mem_watch_test\\mem_watch_test.cpp") != none, "M11.build: build.bat builds and runs this rig");

    // The log: a reserved line is noted through noteReserved, written when the cap has been reached, and the process-exit path is
    // not touched by any of it.
    check(at(logH, "void noteReserved(const char* fmt, ...);") != none && at(logCpp, "void Log::noteReserved(") != none && at(logCpp, "writeReserved(index);") != none,
          "M11.log: Log::noteReserved exists and writeBuffer writes the reserve once the cap is reached");
    const std::string detach = between(logCpp, "void Log::detachDuringProcessExit() {", "void Log::close() {");
    check(!detach.empty() && !contains(detach, "eserve") && !contains(detach, "spans"),
          "M11.log: detachDuringProcessExit does not use the reserve");
    check(contains(logCpp, "log size cap reached; only lines noted as reserved") && !contains(logCpp, "log size cap reached; nothing further"),
          "M11.log: the cap notice says the reserved lines go on");
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
        std::printf("[mem_watch_test] dry-run: touches nothing (no log, no file, no thread, no process)\n");
        return 0;
    }
    if (!selfTest) {
        std::printf("usage: mem_watch_test.exe --dry-run | --self-test [root]\n");
        return 2;
    }
    constantCases();
    unitCases();
    cadenceCases();
    pressureCases();
    textCases();
    armedCases();
    fitCases();
    plausibilityCases();
    nameCases();
    reserveCases();
    // What the lines look like, for the record: not checks.
    std::printf("[mem_watch_test] armed:    %s\n", armed(typical()).c_str());
    std::printf("[mem_watch_test] periodic: %s\n", periodic(typical()).c_str());
    std::printf("[mem_watch_test] widest line %zu of %zu bytes\n", periodic(widest()).size(), kMemLineCap);
    if (root.empty()) std::printf("[mem_watch_test] SKIPPED the M11 source pins: no repository root given\n");
    else sourcePins(root);
    if (g_failures) {
        std::printf("FAIL: mem watch: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u mem watch checks\n", g_checks);
    return 0;
}
