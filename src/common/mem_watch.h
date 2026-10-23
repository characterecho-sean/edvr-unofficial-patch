// The process memory watch (docs/memory-growth-2026-10-22.md, "Proposed instrument"): how much memory this process holds,
// how full the machine is, what Elite itself says it has allocated, and what the VR runtime processes hold, written
// down on a clock.
//
// WHY. Players say Elite leaks memory over a long session, most of all in VR. A static reading of the exe and of this
// repository found nothing that grows by gigabytes, and the only number EDVR logged was the GPU's (vram_watch.h), which
// stops when the graphics log reaches its size cap. The next flight needs one line that tells three explanations
// apart: Elite's own CPU memory growing, one kind of Elite GPU resource growing, or another process growing (the VR
// runtime, whose growth a player sees in Task Manager as the game's).
//
// WHAT, in groups, each left out of a line when it could not be read:
//   - commit: this process's private bytes, with its peak commit, working set, handle count and thread count;
//   - sys: the machine's commit charge against its limit, and the RAM still available;
//   - local: this process's video memory against the budget the OS gives it (the same DXGI read vram_watch.h makes);
//   - egpu: Elite's own accounting of its GPU resources, by kind, and its GPU heaps. Read from the game's memory, only
//     on the one build the offsets were taken from (src\d3d11\mem_tick.cpp holds the gate);
//   - eheap: a first reading of Elite's CPU heap manager, not yet checked against the process figures;
//   - the VR runtime processes by name (their commit and handle counts), left out when they are not running.
// A group that fails its plausibility check on a sample is written as egpu=implausible (or eheap=implausible) in place of
// its figures, and a game device that does not exist yet as egpu=unread. Every figure is whole megabytes, rounded.
//
// WHEN:
//   - one line, reason=armed, at the first sample, whatever the figures, saying what each group is and why any group
//     is missing. It is the line that proves this code ran;
//   - a line every kMemLineEveryMs;
//   - a line every kMemPressureEveryMs while the machine's commit charge is at or over kMemPressurePercent of its commit
//     limit.
// The caller wakes every kMemWakeEveryMs, reads the machine's commit, and offers it to MemPolicy::observe, which says
// whether a line is due. Only then is the full sample taken.
//
// This header is pure (no OS call, no clock of its own) so tools\mem_watch_test can hold every rule. The OS reads and
// the log are src\d3d11\mem_tick.cpp.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

constexpr uint64_t kMemWakeEveryMs = 5000;
constexpr uint64_t kMemLineEveryMs = 60000;
constexpr uint64_t kMemPressureEveryMs = 5000;
constexpr uint64_t kMemPressurePercent = 90;
// A wake that comes a few milliseconds before the interval is up (a sleep rounded down by the timer, a clock with 15 ms
// steps) is taken as the interval being up: without this a 5 s pressure cadence woken every 5 s would write every
// second wake.
constexpr uint64_t kMemWakeSlackMs = 500;

// The room a formatted line has. The log gives a reserved line (log_reserve.h) more than this plus its stamp.
constexpr size_t kMemLineCap = 2600;

// Elite's own figures are checked against these before they are believed.
constexpr uint64_t kMemSanityBytes = 64ull << 30;
constexpr uint32_t kMemGpuKinds = 12;
constexpr uint64_t kMemGpuHeapMax = 256;
constexpr uint64_t kMemCpuHeapMax = 16;   // the manager's heap table holds sixteen entries

// The order of Elite's twelve GPU byte counters, and the key each is written with.
inline const char* memGpuKindKey(uint32_t i) {
    static const char* const keys[kMemGpuKinds] = {"rt", "ds", "texuav", "imm", "tex", "vb", "ib", "buf", "shader", "desc", "cmd", "unk"};
    return i < kMemGpuKinds ? keys[i] : "?";
}

// Whole megabytes, rounded to the nearest (a half rounds up), without overflowing near 2^64.
inline uint64_t memMb(uint64_t bytes) { return (bytes >> 20) + ((bytes >> 19) & 1); }

// used is at or over pct percent of total, for pct up to 100. A total of 0 is nothing to be over.
inline bool memAtOrOverPercent(uint64_t used, uint64_t total, uint64_t pct) {
    if (total == 0) return false;
    if (used >= total) return true;
    const uint64_t kMax = ~0ull / 100;
    if (used <= kMax && total <= kMax) return used * 100ull >= total * pct;
    return (used >> 10) * 100ull >= (total >> 10) * pct;   // figures this large only need to be right to a thousandth
}

// ---- the machine, and the cadence --------------------------------------------------------------------------------------

struct MemMachine {
    bool valid = false;
    uint64_t commit = 0;        // bytes of commit charge, machine wide
    uint64_t commitLimit = 0;   // the most the machine can commit now (RAM plus page file)
    uint64_t availPhys = 0;     // RAM not in use
};

// From GlobalMemoryStatusEx: ullTotalPageFile is the commit limit and ullAvailPageFile what is left of it.
inline MemMachine memMachineOf(uint64_t totalPageFile, uint64_t availPageFile, uint64_t availPhys) {
    MemMachine m;
    m.valid = true;
    m.commitLimit = totalPageFile;
    m.commit = totalPageFile > availPageFile ? totalPageFile - availPageFile : 0;
    m.availPhys = availPhys;
    return m;
}

inline bool memUnderPressure(const MemMachine& m) {
    return m.valid && m.commitLimit > 0 && memAtOrOverPercent(m.commit, m.commitLimit, kMemPressurePercent);
}

enum class MemReason {
    None,
    Armed,      // the first sample: always a line, whatever the figures
    Periodic,
    Pressure,
};

inline const char* memReasonKey(MemReason r) {
    switch (r) {
        case MemReason::Armed: return "armed";
        case MemReason::Periodic: return "periodic";
        case MemReason::Pressure: return "pressure";
        case MemReason::None: break;
    }
    return "none";
}

class MemPolicy {
public:
    // One wake. `nowMs` is any millisecond clock that only moves forward. The first wake is always the armed line,
    // even when the machine could not be read; afterwards a line is due when the interval for the machine's state
    // (a minute, or five seconds under pressure) has gone by since the last one. A machine that could not be read is
    // not under pressure, so it keeps the slow cadence.
    MemReason observe(uint64_t nowMs, const MemMachine& machine) {
        if (!armed_) {
            armed_ = true;
            lastMs_ = nowMs;
            ++lines_;
            return MemReason::Armed;
        }
        const bool pressure = memUnderPressure(machine);
        const uint64_t every = pressure ? kMemPressureEveryMs : kMemLineEveryMs;
        if (nowMs < lastMs_) {   // a clock that went backwards restarts the interval
            lastMs_ = nowMs;
            return MemReason::None;
        }
        if (nowMs - lastMs_ + kMemWakeSlackMs < every) return MemReason::None;
        lastMs_ = nowMs;
        ++lines_;
        return pressure ? MemReason::Pressure : MemReason::Periodic;
    }

    bool armed() const { return armed_; }
    uint32_t lines() const { return lines_; }

private:
    bool armed_ = false;
    uint64_t lastMs_ = 0;
    uint32_t lines_ = 0;
};

// ---- the figures --------------------------------------------------------------------------------------------------------

struct MemProcess {
    bool valid = false;
    uint64_t commit = 0;       // private bytes
    uint64_t commitPeak = 0;   // the most commit this process has held
    uint64_t workingSet = 0;
};

struct MemVram {
    bool valid = false;
    uint64_t used = 0;
    uint64_t budget = 0;
};

// Whether a group was written, or why it was not, for one sample. Absent is a group that could not be read at all (the
// armed line says why, once) and is left out of every line.
enum class MemRead {
    Absent,
    Ok,
    Unread,        // the game has not made the object yet at this sample
    Implausible,   // read, and failed its checks
};

struct MemEgpu {
    MemRead state = MemRead::Absent;
    uint64_t kind[kMemGpuKinds] = {};   // live bytes by kind, in memGpuKindKey order
    uint64_t highWater = 0;             // the most their sum has been
    uint64_t heaps = 0;                 // GPU heaps, of every type
    uint64_t descriptorHeaps = 0;       // ...of which count descriptors, not bytes, and are left out of the two sums
    uint64_t heapUsed = 0;              // bytes, over the byte heaps
    uint64_t heapCapacity = 0;
};

struct MemEheap {
    MemRead state = MemRead::Absent;
    uint64_t heaps = 0;
    uint64_t osBytes = 0;               // summed over heaps
    bool mainRead = false;              // the default allocator's statistics were found
    bool mainAccounted = false;         // ...and it is the one that accumulates them (a child block does not)
    uint64_t mainCurrent = 0;
    uint64_t mainPeak = 0;
    uint32_t mainAllocs = 0;
};

constexpr int kMemRuntimes = 4;

struct MemRuntime {
    bool running = false;
    uint64_t commit = 0;      // summed over instances
    uint64_t handles = 0;
};

// The VR runtime processes whose growth a player would blame on the game. These four names are the only other
// processes this watch ever reads, and the only ones it ever writes: the key is what the line carries.
inline const char* memRuntimeKey(int i) {
    static const char* const keys[kMemRuntimes] = {"ovrserver", "vrserver", "vrcompositor", "vdstreamer"};
    return i >= 0 && i < kMemRuntimes ? keys[i] : "?";
}
inline const char* memRuntimeImage(int i) {
    static const char* const images[kMemRuntimes] = {"OVRServer_x64.exe", "vrserver.exe", "vrcompositor.exe", "VirtualDesktop.Streamer.exe"};
    return i >= 0 && i < kMemRuntimes ? images[i] : "";
}

// Which runtime an image name is, or -1: case-insensitive, the whole name, ASCII.
inline int memRuntimeIndexOf(const wchar_t* name, size_t chars) {
    if (!name) return -1;
    for (int i = 0; i < kMemRuntimes; ++i) {
        const char* image = memRuntimeImage(i);
        size_t n = 0;
        while (image[n]) ++n;
        if (n != chars) continue;
        bool same = true;
        for (size_t c = 0; c < n && same; ++c) {
            wchar_t a = name[c];
            char b = image[c];
            if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            same = a == static_cast<wchar_t>(static_cast<unsigned char>(b));
        }
        if (same) return i;
    }
    return -1;
}

struct MemFigures {
    MemProcess process;
    bool handlesValid = false;
    uint32_t handles = 0;
    bool threadsValid = false;
    uint32_t threads = 0;
    MemMachine machine;
    MemVram local;
    MemEgpu egpu;
    MemEheap eheap;
    MemRuntime runtime[kMemRuntimes];
};

// ---- plausibility of what is read out of the game's memory ------------------------------------------------------------

// Elite's twelve live counters, the second set (the same, without flagged resources) and the high water of the first
// set's sum: no counter near the sanity limit's sum, no flagged-out figure above its whole, the high water at least the
// sum. A counter that has gone below zero is a huge unsigned number and fails the first rule.
inline bool memGpuCountersPlausible(const uint64_t first[kMemGpuKinds], const uint64_t second[kMemGpuKinds], uint64_t highWater) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < kMemGpuKinds; ++i) {
        if (first[i] >= kMemSanityBytes) return false;   // so the sum below cannot overflow
        if (second[i] > first[i]) return false;
        sum += first[i];
    }
    if (sum >= kMemSanityBytes) return false;
    if (highWater < sum) return false;
    return highWater < kMemSanityBytes;
}
inline bool memGpuHeapCountPlausible(uint64_t count) { return count <= kMemGpuHeapMax; }
// One GPU heap: no more used than it holds, and a size a heap can have (so that the sums over up to kMemGpuHeapMax of them cannot
// overflow).
inline bool memGpuHeapPlausible(uint64_t used, uint64_t capacity) { return used <= capacity && capacity < kMemSanityBytes; }

inline bool memCpuHeapCountPlausible(uint64_t count) { return count >= 1 && count <= kMemCpuHeapMax; }
inline bool memCpuOsBytesPlausible(uint64_t bytes) { return bytes < kMemSanityBytes; }
inline bool memCpuMainPlausible(uint64_t current, uint64_t peak) { return current <= peak; }

// ---- the text -----------------------------------------------------------------------------------------------------------

namespace mem_detail {

// A line being built in a fixed buffer: once it does not fit, the rest is dropped and `cut` says so.
struct Out {
    char* buf;
    size_t cap;
    size_t len = 0;
    bool cut = false;
    Out(char* b, size_t c) : buf(b), cap(c) {
        if (buf && cap) buf[0] = 0;
    }
    void add(const char* fmt, ...) {
        if (!buf || cap == 0 || cut) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n < 0 || static_cast<size_t>(n) >= cap - len) {
            cut = true;
            len = cap - 1;
            buf[len] = 0;
            return;
        }
        len += static_cast<size_t>(n);
    }
};

inline unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }

inline void figures(Out& o, const MemFigures& f) {
    if (f.process.valid) {
        o.add(" commit_mb=%llu commit_peak_mb=%llu ws_mb=%llu", ull(memMb(f.process.commit)), ull(memMb(f.process.commitPeak)),
              ull(memMb(f.process.workingSet)));
    }
    if (f.handlesValid) o.add(" handles=%u", static_cast<unsigned>(f.handles));
    if (f.threadsValid) o.add(" threads=%u", static_cast<unsigned>(f.threads));
    if (f.machine.valid) {
        o.add(" sys_commit_mb=%llu sys_commit_limit_mb=%llu sys_avail_mb=%llu", ull(memMb(f.machine.commit)),
              ull(memMb(f.machine.commitLimit)), ull(memMb(f.machine.availPhys)));
    }
    if (f.local.valid) o.add(" local_used_mb=%llu local_budget_mb=%llu", ull(memMb(f.local.used)), ull(memMb(f.local.budget)));
    switch (f.egpu.state) {
        case MemRead::Unread: o.add(" egpu=unread"); break;
        case MemRead::Implausible: o.add(" egpu=implausible"); break;
        case MemRead::Ok: {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < kMemGpuKinds; ++i) sum += f.egpu.kind[i];
            o.add(" egpu_mb=%llu egpu_peak_mb=%llu", ull(memMb(sum)), ull(memMb(f.egpu.highWater)));
            for (uint32_t i = 0; i < kMemGpuKinds; ++i) o.add(" egpu_%s_mb=%llu", memGpuKindKey(i), ull(memMb(f.egpu.kind[i])));
            o.add(" egpu_heaps=%llu egpu_desc_heaps=%llu egpu_heap_used_mb=%llu egpu_heap_cap_mb=%llu", ull(f.egpu.heaps),
                  ull(f.egpu.descriptorHeaps), ull(memMb(f.egpu.heapUsed)), ull(memMb(f.egpu.heapCapacity)));
            break;
        }
        case MemRead::Absent: break;
    }
    switch (f.eheap.state) {
        case MemRead::Unread: o.add(" eheap=unread"); break;
        case MemRead::Implausible: o.add(" eheap=implausible"); break;
        case MemRead::Ok:
            o.add(" eheap_heaps=%llu eheap_os_mb=%llu", ull(f.eheap.heaps), ull(memMb(f.eheap.osBytes)));
            if (f.eheap.mainRead && f.eheap.mainAccounted) {
                o.add(" eheap_main_mb=%llu eheap_main_peak_mb=%llu eheap_main_allocs=%u", ull(memMb(f.eheap.mainCurrent)),
                      ull(memMb(f.eheap.mainPeak)), static_cast<unsigned>(f.eheap.mainAllocs));
            } else {
                o.add(" eheap_main=%s", f.eheap.mainRead ? "untracked" : "unread");
            }
            break;
        case MemRead::Absent: break;
    }
    for (int i = 0; i < kMemRuntimes; ++i) {
        const MemRuntime& r = f.runtime[i];
        if (!r.running) continue;
        o.add(" %s_commit_mb=%llu %s_handles=%llu", memRuntimeKey(i), ull(memMb(r.commit)), memRuntimeKey(i), ull(r.handles));
    }
}

}  // namespace mem_detail

// A periodic or pressure line, no newline. Returns its length.
inline size_t formatMemLine(char* buf, size_t cap, MemReason reason, const MemFigures& f) {
    if (!buf || cap == 0) return 0;
    mem_detail::Out o(buf, cap);
    o.add("mem: reason=%s", memReasonKey(reason));
    mem_detail::figures(o, f);
    return o.len;
}

// Why each group that could not be read could not, for the armed line. A null is a group that was read.
enum class MemGroup {
    Process,
    Machine,
    Vram,
    Scan,     // the process list: the thread count and the runtimes
    Egpu,
    Eheap,
};
constexpr int kMemGroups = 6;

struct MemUnavailable {
    const char* why[kMemGroups] = {};
};

inline const char* memGroupName(int g) {
    static const char* const names[kMemGroups] = {"commit", "sys", "local", "processes", "egpu", "eheap"};
    return g >= 0 && g < kMemGroups ? names[g] : "?";
}

// The line written once, at the first sample. It carries the first sample's figures and says what each group is.
inline size_t formatMemArmed(char* buf, size_t cap, const MemFigures& f, const MemUnavailable& missing) {
    if (!buf || cap == 0) return 0;
    mem_detail::Out o(buf, cap);
    o.add("mem: reason=armed");
    mem_detail::figures(o, f);
    bool any = false;
    for (int g = 0; g < kMemGroups; ++g) {
        if (!missing.why[g]) continue;
        if (any) o.add("; %s (%.160s)", memGroupName(g), missing.why[g]);
        else o.add("; unreadable, so left out of every line: %s (%.160s)", memGroupName(g), missing.why[g]);
        any = true;
    }
    o.add("; sizes are whole MB. commit is this process's private bytes, commit_peak its highest, ws its working set, with its "
          "handle and thread counts (under Wine commit is anonymous resident memory plus swap, and handles are not counted, so "
          "left out). sys is the whole machine's commit charge against its limit, and the RAM still free. local is "
          "this process's video memory against the budget the OS gives it. egpu is Elite's own accounting of its GPU resources "
          "by kind (rt ds texuav imm tex vb ib buf shader desc cmd unk), read only on build 334925: its heaps are counted in "
          "bytes, with descriptor heaps counted apart; egpu=unread is a game with no device yet and egpu=implausible values that "
          "failed their checks. eheap is a first reading of Elite's heap manager, not yet checked against the process figures: "
          "os is the bytes its heaps took from the C runtime, main the default allocator's current and peak bytes and the "
          "allocations it has made. The runtime fields cover named VR runtime processes (ovrserver is OVRServer_x64.exe, "
          "vrserver, vrcompositor, vdstreamer is VirtualDesktop.Streamer.exe), their commit and handles summed over instances, "
          "left out when not running. A line every %llu s, every %llu s while the machine's commit is at or over %llu%% of "
          "its limit.",
          mem_detail::ull(kMemLineEveryMs / 1000), mem_detail::ull(kMemPressureEveryMs / 1000), mem_detail::ull(kMemPressurePercent));
    return o.len;
}

// The line written once when the thread cannot be started.
inline size_t formatMemUnavailable(char* buf, size_t cap, const char* why) {
    if (!buf || cap == 0) return 0;
    mem_detail::Out o(buf, cap);
    o.add("mem: unavailable -- %.200s. No mem: line will be written this session.", why && why[0] ? why : "no reason given");
    return o.len;
}

}  // namespace edvr
