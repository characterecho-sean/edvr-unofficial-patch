// What each profile pays for the other (docs\design-unified-install-2026-10-12.md section 3.3, "Cost is H8"). An install whose scope is
// both carries the flat profile's hooks into a VR session and the VR profile's hooks into a flat session, and each body gates per call on
// the active profile. This instrument measures the INACTIVE path only: the work a hook does while the other profile is active, counted per
// site. It logs; it changes nothing a hook does.
//
// The cost of measuring: an inactive call bumps one relaxed counter. One call in kSampleEvery also reads the QueryPerformanceCounter at
// its start and at its stop and adds the difference to a relaxed tick sum, and the report scales the sum back up by kSampleEvery. Capture
// sites (creation time, rare) are timed on every call. No lock and no allocation anywhere. A call that is active costs nothing here: the
// caller passes the gate, and a default Probe measures nothing.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>

namespace edvr::cross_profile {

// One call in this many is timed; its ticks stand for this many calls. A power of two, so the sampling test is a mask.
inline constexpr uint64_t kSampleEvery = 64;
static_assert((kSampleEvery & (kSampleEvery - 1)) == 0, "kSampleEvery must be a power of two");

// The profile a site belongs to: the flat hooks run while VR is active, the VR hooks run while flat is active.
enum class Owner : uint8_t { Flat, Vr };

// The timed sites. Each is one per-call hook body, timed from its gate to the start of the forwarded call (the hook's own work, not the
// game's or the driver's).
enum class Site : uint8_t {
    FlatPsConstantBuffers,   // vscreen.cpp hookedPSSetConstantBuffers: the flat discovery observer, on every PS constant-buffer bind
    FlatResizeBuffers,       // device_hook.cpp hookedFlatResizeBuffers and hookedFlatResizeBuffers1
    VrCoverDetour,           // terrain_cull_cover_hook.cpp coverDetour: the cull-plane builder's entry
    VrModeRequest,           // vr_window_trampolines.cpp requestReplacement: the game's mode request
    VrSettingsCopy,          // vr_window_trampolines.cpp copyReplacement: the hold after the copy (the copy is the game's, not timed)
    VrModeWatchFrame,        // mode_watch.cpp modeWatchFrameBoundary: the runtime's live-session query, once a frame
    kSites
};
inline constexpr size_t kSiteCount = static_cast<size_t>(Site::kSites);

constexpr Owner ownerOf(Site s) noexcept {
    return s == Site::FlatPsConstantBuffers || s == Site::FlatResizeBuffers ? Owner::Flat : Owner::Vr;
}

// The short names the lines print.
struct SiteName {
    Site site;
    const char* name;
};
inline constexpr SiteName kSiteNames[] = {
    {Site::FlatPsConstantBuffers, "psConstantBuffers"},
    {Site::FlatResizeBuffers, "resizeBuffers"},
    {Site::VrCoverDetour, "coverDetour"},
    {Site::VrModeRequest, "modeRequest"},
    {Site::VrSettingsCopy, "settingsCopy"},
    {Site::VrModeWatchFrame, "modeWatchFrame"},
};

// Counters. calls counts every inactive call; ticks sums the QPC difference of the timed ones. Captures: the flat profile's creation
// captures while VR is active, every one timed.
struct Counter {
    alignas(64) std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> ticks{0};
};
inline Counter g_sites[kSiteCount];
inline Counter g_captures;

inline uint64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// One measured stretch of one call. A default Probe measures nothing, so an active call holds one at no cost. stop() ends the timed
// stretch and adds it; a second stop() adds nothing. restart() opens another stretch of the same call, for a hook that forwards and then
// does more work of its own.
class Probe {
public:
    Probe() = default;

    // Counts one call of c, and starts timing it when this is one of the sampled calls (every call when everyCall).
    static Probe begin(Counter& c, bool everyCall) noexcept {
        const uint64_t n = c.calls.fetch_add(1, std::memory_order_relaxed);
        Probe p;
        p.counter_ = &c;
        p.timed_ = everyCall || (n & (kSampleEvery - 1)) == 0;
        if (p.timed_) {
            p.start_ = qpcNow();
            p.running_ = true;
        }
        return p;
    }

    void stop() noexcept {
        if (!running_) return;
        running_ = false;
        counter_->ticks.fetch_add(qpcNow() - start_, std::memory_order_relaxed);
    }

    void restart() noexcept {
        if (!timed_ || running_) return;
        running_ = true;
        start_ = qpcNow();
    }

    bool timed() const noexcept { return timed_; }

private:
    Counter* counter_ = nullptr;
    uint64_t start_ = 0;
    bool timed_ = false;
    bool running_ = false;
};

// The caller's gate for a site of `owner`: true only when the profile whose frames the report divides that owner by is the active one (the
// flat hooks' cost divides by VR frames, the VR hooks' by flat frames; framesOf). In Passthrough or an invalid profile neither is active,
// the reporter counts no frame, so a sample taken there would be divided into the next window's frames of another profile. It is not taken.
// A hook that must still run in those profiles (the mode watch's live query) keeps running; it just is not measured there.
constexpr bool inactiveFor(Owner owner, bool flatActive, bool vrActive) noexcept {
    return owner == Owner::Flat ? vrActive : flatActive;
}

// The probe for one call of a hot site. inactive is the caller's gate: true when the other profile is active, which is the only time
// the site's own work is counted.
inline Probe enterSite(Site s, bool inactive) noexcept {
    return inactive ? Probe::begin(g_sites[static_cast<size_t>(s)], false) : Probe();
}

// Times one creation-time capture for the scope's life. inactive is the caller's gate, as above. Every call is timed.
class CaptureScope {
public:
    explicit CaptureScope(bool inactive) noexcept : probe_(inactive ? Probe::begin(g_captures, true) : Probe()) {}
    ~CaptureScope() { probe_.stop(); }
    CaptureScope(const CaptureScope&) = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;

private:
    Probe probe_;
};

// One reading of the counters, and a window as the difference of two readings. The frames are counted per profile: a frame belongs to
// the profile that was active in it, so an owner's counters divide by the frames of the other profile (framesOf).
struct Totals {
    uint64_t calls[kSiteCount] = {};
    uint64_t ticks[kSiteCount] = {};
    uint64_t captures = 0;
    uint64_t captureTicks = 0;
    uint64_t vrFrames = 0;
    uint64_t flatFrames = 0;
};

// The frames an owner's counters divide by: its hooks run while the other profile is active, so a flat hook divides by the VR frames
// and a VR hook by the flat frames. A window spanning a switch then gives each owner the frames its own cost was paid in.
inline uint64_t framesOf(const Totals& t, Owner owner) noexcept {
    return owner == Owner::Flat ? t.vrFrames : t.flatFrames;
}

inline Totals readTotals(uint64_t vrFrames, uint64_t flatFrames) noexcept {
    Totals t;
    for (size_t i = 0; i < kSiteCount; ++i) {
        t.calls[i] = g_sites[i].calls.load(std::memory_order_relaxed);
        t.ticks[i] = g_sites[i].ticks.load(std::memory_order_relaxed);
    }
    t.captures = g_captures.calls.load(std::memory_order_relaxed);
    t.captureTicks = g_captures.ticks.load(std::memory_order_relaxed);
    t.vrFrames = vrFrames;
    t.flatFrames = flatFrames;
    return t;
}

inline Totals minus(const Totals& now, const Totals& base) noexcept {
    Totals d;
    for (size_t i = 0; i < kSiteCount; ++i) {
        d.calls[i] = now.calls[i] - base.calls[i];
        d.ticks[i] = now.ticks[i] - base.ticks[i];
    }
    d.captures = now.captures - base.captures;
    d.captureTicks = now.captureTicks - base.captureTicks;
    d.vrFrames = now.vrFrames - base.vrFrames;
    d.flatFrames = now.flatFrames - base.flatFrames;
    return d;
}

// ---- Arithmetic (the rig pins it) ------------------------------------------------------------------------------------------------------

// Microseconds the sampled ticks stand for, over all the calls they sampled: each sampled call stands for kSampleEvery calls.
inline double scaledMicros(uint64_t sampledTicks, uint64_t freq) noexcept {
    if (!freq) return 0.0;
    return static_cast<double>(sampledTicks) * static_cast<double>(kSampleEvery) * 1e6 / static_cast<double>(freq);
}

// Microseconds per frame over a window of frames; zero for a window with none.
inline double perFrameMicros(uint64_t sampledTicks, uint64_t freq, uint64_t frames) noexcept {
    return frames ? scaledMicros(sampledTicks, freq) / static_cast<double>(frames) : 0.0;
}

// Milliseconds of an unsampled sum (every capture is timed, so nothing scales).
inline double captureMillis(uint64_t ticks, uint64_t freq) noexcept {
    if (!freq) return 0.0;
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

// ---- Lines -----------------------------------------------------------------------------------------------------------------------------

// Appends formatted text to a buffer without overrunning it.
struct Writer {
    char* out;
    size_t size;
    size_t len = 0;

    void add(const char* fmt, ...) noexcept {
        if (!out || len + 1 >= size) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(out + len, size - len, fmt, ap);
        va_end(ap);
        if (n > 0) len += static_cast<size_t>(n) < size - len ? static_cast<size_t>(n) : size - len - 1;
    }
};

// One owner's line: its sites per frame over the frames its hooks ran in (framesOf), and, on the flat line only, the captures (the flat
// profile's creation captures, which run while VR is active). The window line adds the session's captures in brackets; the session lines
// pass nullptr. title is "cross-profile cost" for a window and "cross-profile cost session" for the running total.
inline size_t formatLine(char* out, size_t size, const char* title, Owner owner, const Totals& t, const Totals* session,
                         uint64_t freq) noexcept {
    Writer w{out, size};
    w.add("%s: %s:", title, owner == Owner::Flat ? "flat hooks while vr" : "vr hooks while flat");
    const uint64_t frames = framesOf(t, owner);
    const char* sep = " ";
    for (const SiteName& s : kSiteNames) {
        if (ownerOf(s.site) != owner) continue;
        const size_t i = static_cast<size_t>(s.site);
        w.add("%s%s=%llu/%.2f", sep, s.name, static_cast<unsigned long long>(t.calls[i]),
              perFrameMicros(t.ticks[i], freq, frames));
        sep = ", ";
    }
    if (owner == Owner::Flat) {
        w.add("; captures %llu in %.2f ms", static_cast<unsigned long long>(t.captures), captureMillis(t.captureTicks, freq));
        if (session) {
            w.add(" (session %llu in %.2f ms)", static_cast<unsigned long long>(session->captures),
                  captureMillis(session->captureTicks, freq));
        }
    }
    w.add(" over %llu frames", static_cast<unsigned long long>(frames));
    return w.len;
}

// ---- The report -------------------------------------------------------------------------------------------------------------------------

// Read once a frame at the Present edge, on the render thread (perf_monitor.cpp). Every 30 s it emits a window line for each owner that had
// frames in which its hooks ran: a window that spans a switch prints both, each divided by its own profile's frames. Every 5 minutes it
// emits the session totals for both owners, since the process started. No flush is needed at a switch: the frames are counted per
// profile, so a switch splits nothing. Nothing is locked: the counters are read with relaxed loads, and the frame counts and the windows
// are the render thread's alone.
class Reporter {
public:
    static constexpr uint64_t kWindowMs = 30000;
    static constexpr uint64_t kSessionMs = 300000;

    struct Lines {
        char text[4][400];
        int count = 0;
    };

    // flatActive / vrActive: the runtime profile now (runtimeFlatProfile(), runtimeVrProfile()). The two are exclusive; a frame with
    // neither is counted toward neither profile, and nothing is logged for it, but the window still rolls over.
    void frame(uint64_t nowMs, uint64_t freq, bool flatActive, bool vrActive, Lines& out) noexcept {
        if (flatActive) {
            ++flatFrames_;
        } else if (vrActive) {
            ++vrFrames_;
        }
        out.count = 0;
        if (!started_) {
            started_ = true;
            windowStartMs_ = nowMs;
            sessionDueMs_ = nowMs + kSessionMs;
            windowBase_ = readTotals(vrFrames_, flatFrames_);
            return;
        }
        if (nowMs - windowStartMs_ >= kWindowMs) {
            const Totals now = readTotals(vrFrames_, flatFrames_);
            const Totals window = minus(now, windowBase_);
            for (Owner owner : {Owner::Flat, Owner::Vr}) {
                if (framesOf(window, owner) == 0) continue;   // no frames in which this owner's hooks ran
                formatLine(out.text[out.count++], sizeof(out.text[0]), "cross-profile cost", owner, window, &now, freq);
            }
            windowBase_ = now;
            windowStartMs_ = nowMs;
        }
        if (nowMs >= sessionDueMs_) {
            sessionDueMs_ = nowMs + kSessionMs;
            const Totals now = readTotals(vrFrames_, flatFrames_);
            for (Owner owner : {Owner::Flat, Owner::Vr}) {
                formatLine(out.text[out.count++], sizeof(out.text[0]), "cross-profile cost session", owner, now, nullptr, freq);
            }
        }
    }

private:
    bool started_ = false;
    uint64_t vrFrames_ = 0;
    uint64_t flatFrames_ = 0;
    uint64_t windowStartMs_ = 0;
    uint64_t sessionDueMs_ = 0;
    Totals windowBase_{};
};

}  // namespace edvr::cross_profile
