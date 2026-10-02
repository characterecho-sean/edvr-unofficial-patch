// The graphics memory watch (docs/headset-lock-vdxr-2026-10-02.md, instrument 1): how much of the GPU's memory this
// process holds, against how much the OS lets it hold, written down on a clock, while it is close to the limit, and
// at each crossing of it.
//
// WHY. A Quest 3 user's headset locked on foot while the game carried on at 10 fps, and one of the three open
// explanations was memory pressure on a 12 GB card in a heavy scene. Nothing in either log said how full the card
// was, so that explanation could be neither held nor ruled out. This is the number.
//
// WHAT. DXGI's IDXGIAdapter3::QueryVideoMemoryInfo, for the adapter the game's device sits on, in two segment
// groups: LOCAL (the card's own memory) and NON_LOCAL (system memory the card uses). `usage` is this process's, not
// the machine's; `budget` is what the OS will let this process have right now, and it moves (another program
// starting takes some of it). Usage over the budget is when the OS starts demoting this process's allocations to
// system memory, which is where a game that was fine a minute ago stops being.
//
// WHEN (all of it from the local group, which is the one that matters):
//   - a line every kVramLineEveryMs;
//   - a line every kVramPressureEveryMs while usage is at or over kVramPressurePercent of the budget;
//   - one line at each crossing of the budget, over and back under, the moment the sample sees it. A crossing is
//     never held to the cap below.
// The caller samples the OS about once a second and offers every sample to VramPolicy::observe, which says whether a
// line is due and which kind. The sampling is silent; only the policy writes.
//
// This header is pure (no OS call, no clock of its own) so tools\vram_watch_test can hold every rule; the one
// place that calls DXGI is vram_query.h, and the glue that logs is src\d3d11\vram_tick.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

constexpr uint64_t kVramSampleEveryMs = 1000;
constexpr uint64_t kVramLineEveryMs = 30000;
constexpr uint64_t kVramPressureEveryMs = 5000;
constexpr uint64_t kVramPressurePercent = 90;
// Lines of the two cadences (periodic and pressure) a session may write: a card that sits at 95% for four hours
// would otherwise write a line every five seconds all that time. 3000 is about four hours of nothing but pressure
// lines, a few hundred KB. Crossings are exempt, and the first line past the cap says so.
constexpr uint32_t kVramCadenceLineCap = 3000;

struct VramSegment {
    bool valid = false;     // the OS answered for this segment group
    uint64_t usage = 0;     // bytes this process uses in it
    uint64_t budget = 0;    // bytes the OS lets this process use in it now (0: not reported)
};

struct VramFigures {
    VramSegment local;
    VramSegment nonLocal;
};

enum class VramReason {
    None,
    Armed,             // the first reading; written by the glue with the adapter's name, never returned by the policy
    Periodic,
    Pressure,
    OverBudget,
    BackUnderBudget,
    CapReached,        // the cadence cap was reached: one notice, then only crossings
};

inline const char* vramReasonKey(VramReason r) {
    switch (r) {
        case VramReason::Armed: return "armed";
        case VramReason::Periodic: return "periodic";
        case VramReason::Pressure: return "pressure";
        case VramReason::OverBudget: return "over_budget";
        case VramReason::BackUnderBudget: return "back_under_budget";
        case VramReason::CapReached: return "cap_reached";
        case VramReason::None: break;
    }
    return "none";
}

// Usage as a percentage of the budget, in tenths (683 is 68.3%), or -1 when either is unknown.
inline int vramPercentTenths(const VramSegment& s) {
    if (!s.valid || s.budget == 0) return -1;
    const double tenths = 1000.0 * static_cast<double>(s.usage) / static_cast<double>(s.budget);
    if (tenths > 2000000.0) return 2000000;   // a budget of a few bytes; keeps the figure printable
    return static_cast<int>(tenths + 0.5);
}

inline bool vramOverBudget(const VramSegment& s) { return s.valid && s.budget > 0 && s.usage > s.budget; }

inline bool vramUnderPressure(const VramSegment& s) {
    return s.valid && s.budget > 0 && s.usage * 100ull >= s.budget * kVramPressurePercent;
}

class VramPolicy {
public:
    // One sample. `nowMs` is any millisecond clock that only moves forward; a sample without local figures is
    // ignored. Returns the kind of line due now, or None. The first sample arms the policy and returns None: the
    // glue writes its own armed line from it, which counts as the first line of the cadence.
    VramReason observe(uint64_t nowMs, const VramFigures& f) {
        if (!f.local.valid) return VramReason::None;
        const bool over = vramOverBudget(f.local);
        const bool pressure = vramUnderPressure(f.local);
        if (!armed_) {
            armed_ = true;
            over_ = over;
            pressure_ = pressure;
            lastMs_ = nowMs;
            return VramReason::None;
        }
        VramReason r = VramReason::None;
        if (over != over_) {
            r = over ? VramReason::OverBudget : VramReason::BackUnderBudget;
            ++crossings_;
        } else if (pressure && !pressure_) {
            r = VramReason::Pressure;   // the moment the card comes within reach of the limit, not up to five seconds later
        } else {
            const uint64_t every = pressure ? kVramPressureEveryMs : kVramLineEveryMs;
            if (nowMs < lastMs_) lastMs_ = nowMs;   // a clock that went backwards restarts the interval
            else if (nowMs - lastMs_ >= every) r = pressure ? VramReason::Pressure : VramReason::Periodic;
        }
        over_ = over;
        pressure_ = pressure;
        if (r == VramReason::None) return r;
        if (r == VramReason::Periodic || r == VramReason::Pressure) {
            if (cadenceLines_ >= kVramCadenceLineCap) {
                lastMs_ = nowMs;
                if (capNoted_) return VramReason::None;
                capNoted_ = true;
                return VramReason::CapReached;
            }
            ++cadenceLines_;
        }
        lastMs_ = nowMs;
        return r;
    }

    bool armed() const { return armed_; }
    uint32_t cadenceLines() const { return cadenceLines_; }
    uint32_t crossings() const { return crossings_; }

private:
    bool armed_ = false;
    bool over_ = false;
    bool pressure_ = false;
    bool capNoted_ = false;
    uint64_t lastMs_ = 0;
    uint32_t cadenceLines_ = 0;
    uint32_t crossings_ = 0;
};

namespace vram_detail {
inline void megabytes(char* out, size_t size, const VramSegment& s, bool budget) {
    if (!s.valid) {
        std::snprintf(out, size, "-");
        return;
    }
    const uint64_t bytes = budget ? s.budget : s.usage;
    std::snprintf(out, size, "%llu", static_cast<unsigned long long>(bytes / (1024ull * 1024ull)));
}
inline void percent(char* out, size_t size, const VramSegment& s) {
    const int tenths = vramPercentTenths(s);
    if (tenths < 0) std::snprintf(out, size, "-");
    else std::snprintf(out, size, "%d.%d", tenths / 10, tenths % 10);
}
inline size_t finish(char* buf, size_t cap, int n) {
    if (n < 0) {
        buf[0] = 0;
        return 0;
    }
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}
}  // namespace vram_detail

// The six figures every vram line carries after its reason, as key=value pairs. A segment the OS did not answer for
// prints "-" for its three, so a reader can tell "no answer" from "zero". The graphics log's lines separate them with
// a space; the runtime's SLOW line (slow_regime.h) is comma-separated and prefixes the keys with "vram_".
inline size_t formatVramFigures(char* buf, size_t cap, const VramFigures& f, char sep = ' ', const char* prefix = "") {
    if (!buf || cap == 0) return 0;
    char lu[24], lb[24], lp[16], nu[24], nb[24], np[16];
    vram_detail::megabytes(lu, sizeof(lu), f.local, false);
    vram_detail::megabytes(lb, sizeof(lb), f.local, true);
    vram_detail::percent(lp, sizeof(lp), f.local);
    vram_detail::megabytes(nu, sizeof(nu), f.nonLocal, false);
    vram_detail::megabytes(nb, sizeof(nb), f.nonLocal, true);
    vram_detail::percent(np, sizeof(np), f.nonLocal);
    if (!prefix) prefix = "";
    return vram_detail::finish(
        buf, cap,
        std::snprintf(buf, cap, "%slocal_used_mb=%s%c%slocal_budget_mb=%s%c%slocal_pct=%s%c%snonlocal_used_mb=%s%c%snonlocal_budget_mb=%s%c%snonlocal_pct=%s",
                      prefix, lu, sep, prefix, lb, sep, prefix, lp, sep, prefix, nu, sep, prefix, nb, sep, prefix, np));
}

// A line the policy asked for. `Armed` is the glue's own (formatVramArmed): it carries the adapter's name and what
// the watch will do.
inline size_t formatVramLine(char* buf, size_t cap, VramReason reason, const VramFigures& f) {
    if (!buf || cap == 0) return 0;
    char figures[200];
    formatVramFigures(figures, sizeof(figures), f);
    if (reason == VramReason::CapReached) {
        return vram_detail::finish(
            buf, cap,
            std::snprintf(buf, cap,
                          "vram: reason=cap_reached %s; %u periodic and pressure lines have been written this session, which is the "
                          "cap; from here only a crossing of the budget is written.",
                          figures, static_cast<unsigned>(kVramCadenceLineCap)));
    }
    return vram_detail::finish(buf, cap, std::snprintf(buf, cap, "vram: reason=%s %s", vramReasonKey(reason), figures));
}

// The line the watch writes once, at its first reading.
inline size_t formatVramArmed(char* buf, size_t cap, const char* adapterName, const VramFigures& f) {
    if (!buf || cap == 0) return 0;
    char figures[200];
    formatVramFigures(figures, sizeof(figures), f);
    return vram_detail::finish(
        buf, cap,
        std::snprintf(
            buf, cap,
            "vram: reason=armed %s; adapter %s; this process's graphics memory against the budget the OS gives it "
            "(DXGI QueryVideoMemoryInfo, local = the card's own memory, non-local = system memory it uses), sampled once a "
            "second; a line every %llu s, every %llu s from %llu%% of the local budget, and one at each crossing of the "
            "budget.%s",
            figures, adapterName && adapterName[0] ? adapterName : "(unnamed)",
            static_cast<unsigned long long>(kVramLineEveryMs / 1000), static_cast<unsigned long long>(kVramPressureEveryMs / 1000),
            static_cast<unsigned long long>(kVramPressurePercent),
            f.local.valid && f.local.budget == 0 ? " The OS reports no local budget, so no percentage and no crossing can be judged." : ""));
}

// The line the watch writes once when it cannot read anything.
inline size_t formatVramUnavailable(char* buf, size_t cap, const char* why) {
    if (!buf || cap == 0) return 0;
    return vram_detail::finish(
        buf, cap,
        std::snprintf(buf, cap, "vram: unavailable -- %s. No vram: line will be written this session.",
                      why && why[0] ? why : "no reason given"));
}

// The machine the Present hook drives: when a sample is due, how the adapter is opened at the first one, what is
// said when it cannot be, and what is written afterwards. The OS and the log reach it only through the three callables
// tick() is handed, so a rig drives it with a fake adapter and a fake clock and sees every line it would write.
//
//   open()      -> Opening   the first time a sample is due: the adapter's name, or why there is none
//   read(&hr)   -> VramFigures   every sample; `local.valid` false when the OS did not answer (hr says why)
//   write(line)                 a finished log line, no newline
//
// The first reading either writes the armed line or the unavailable line, once; an adapter that cannot be opened (or
// whose first reading fails) is never asked again, so a DXVK without IDXGIAdapter3 costs one line and one compare of
// two integers a frame. A later failed read is said once and retried every second.
class VramWatcher {
public:
    struct Opening {
        bool ok = false;
        const char* why = "";    // when !ok
        char adapter[128] = {};  // when ok: the adapter's name
    };

    // The cheap question the hook asks every frame: is a sample due at this clock reading?
    bool due(int64_t qpc) const { return !unavailable_ && qpc >= nextQpc_; }

    template <class Open, class Read, class Write>
    void tick(int64_t qpc, int64_t qpcPerSecond, uint64_t nowMs, Open&& open, Read&& read, Write&& write) {
        if (!due(qpc) || qpcPerSecond <= 0) return;
        nextQpc_ = qpc + qpcPerSecond * static_cast<int64_t>(kVramSampleEveryMs) / 1000;
        char line[768];   // the widest armed line (a 127-byte adapter name, every figure at 2^64-1) is under 700 bytes
        if (!opened_) {
            opened_ = true;
            const Opening opening = open();
            if (!opening.ok) {
                formatVramUnavailable(line, sizeof(line), opening.why);
                write(static_cast<const char*>(line));
                unavailable_ = true;
                return;
            }
            std::snprintf(adapter_, sizeof(adapter_), "%s", opening.adapter);
            long hr = 0;
            const VramFigures figures = read(&hr);
            if (!figures.local.valid) {
                char reason[112];
                std::snprintf(reason, sizeof(reason), "QueryVideoMemoryInfo failed for the local segment group (hr 0x%08lX)",
                              static_cast<unsigned long>(hr));
                formatVramUnavailable(line, sizeof(line), reason);
                write(static_cast<const char*>(line));
                unavailable_ = true;
                return;
            }
            policy_.observe(nowMs, figures);   // arms the policy; the line below is the first of its cadence
            formatVramArmed(line, sizeof(line), adapter_, figures);
            write(static_cast<const char*>(line));
            return;
        }
        long hr = 0;
        const VramFigures figures = read(&hr);
        if (!figures.local.valid) {
            ++failures_;
            if (!failureNoted_) {
                failureNoted_ = true;
                std::snprintf(line, sizeof(line),
                              "vram: QueryVideoMemoryInfo failed for the local segment group (hr 0x%08lX); no line is written until "
                              "it answers again.",
                              static_cast<unsigned long>(hr));
                write(static_cast<const char*>(line));
            }
            return;
        }
        const VramReason reason = policy_.observe(nowMs, figures);
        if (reason == VramReason::None) return;
        formatVramLine(line, sizeof(line), reason, figures);
        write(static_cast<const char*>(line));
    }

    bool unavailable() const { return unavailable_; }
    uint64_t failures() const { return failures_; }
    const VramPolicy& policy() const { return policy_; }

private:
    VramPolicy policy_;
    char adapter_[128] = {};
    bool opened_ = false;
    bool unavailable_ = false;
    bool failureNoted_ = false;
    int64_t nextQpc_ = 0;
    uint64_t failures_ = 0;
};

}  // namespace edvr
