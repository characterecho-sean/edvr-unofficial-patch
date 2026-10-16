// What the Unmap tee reads FROM THE MAPPED POINTER, counted and timed (docs/particle-billboards.md,
// 2026-10-16). Log only: nothing here changes what any reader does.
//
// Mapped dynamic constant-buffer memory may be write-combined, which makes every CPU read from it slow
// (issue 65, DXVK: 18-54 us a 5 KB block). hookedUnmap reads the game's pointer for several consumers
// before the real Unmap. This prices each one: calls a frame, bytes a call, microseconds a call and a frame.
//
// Every call is counted; one in kSampleEvery is bracketed with QueryPerformanceCounter, the clock's own
// cost (measured once) taken off, and the microseconds a call is the sampled mean. The readers are
// themselves the cost being measured, so the sample is thin on purpose: the bracket costs two clock reads
// on one call in eight.
//
// Single-threaded by construction: the Unmap tee runs on the owner context's thread, and the frame
// boundary that prints the line runs on the same one.
#pragma once

#include <cstdint>
#include <cstdio>

#include <windows.h>

namespace edvr {
namespace unmapprobe {

enum Reader : int {
    kPanel = 0,      // the composite constants' shadow: memcpy of the whole block (State::shadow, 512 bytes at most)
    kCamGlitch,      // glitchFrameObserve: 3 floats, only inside an engine-fix event window
    kCamSunglare,    // sunglareSceneRows on the camera block: 12 floats at float 932, fix.sunglare world shader only
    kCamTemporal,    // temporalPassNoteSceneWrite: 12 floats at 932 and 2 at 792, only while the temporal pass wants them
    kSceneCb,        // sunglareSceneRows on the world shader's scene block (a 208-byte block never qualifies)
    kParticle,       // particleCapture: the plume shader's slot-1 block
    kBillboard,      // billboardCapture: the sun glare train's block, at most 512 bytes
    kReaderCount
};

inline const char* readerName(int r) {
    static const char* const names[kReaderCount] = {"panel", "camera-glitch", "camera-sunglare", "camera-temporal",
                                                    "scene-cb", "particle", "billboard"};
    return (r >= 0 && r < kReaderCount) ? names[r] : "?";
}

constexpr uint32_t kSampleEvery = 8;          // a power of two
constexpr uint64_t kWindowMs = 30000;

// A clock a rig can replace. Null reads QueryPerformanceCounter.
inline int64_t (*g_clockForTest)() = nullptr;
inline int64_t clockNow() {
    if (g_clockForTest) return g_clockForTest();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline int64_t g_frequencyForTest = 0;
inline int64_t clockFrequency() {
    if (g_frequencyForTest > 0) return g_frequencyForTest;
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? f.QuadPart : 1;
}

struct Window {
    uint64_t calls[kReaderCount] = {};
    uint64_t bytes[kReaderCount] = {};
    uint64_t sampled[kReaderCount] = {};
    int64_t  ticks[kReaderCount] = {};
    uint64_t frames = 0;
    uint64_t particleCaptures = 0;     // particle Unmaps, and of them the ones whose block the camera tee also read
    uint64_t particleSharedWithCamera = 0;
};

struct Probe {
    Window   win;
    uint64_t seq[kReaderCount] = {};   // never reset: keeps the sampling phase across windows
    int64_t  overheadTicks = -1;       // a back-to-back clock pair, measured at first use
    uint64_t windowStartMs = 0;

    void measureOverhead() {
        int64_t best = 0;
        for (int i = 0; i < 64; ++i) {
            const int64_t a = clockNow();
            const int64_t b = clockNow();
            const int64_t d = b - a;
            if (i == 0 || d < best) best = d;     // the minimum: a pair never costs less than its own two reads
        }
        overheadTicks = best > 0 ? best : 0;
    }

    // f() does the read and returns the bytes it read.
    template <class F>
    void timed(Reader r, F&& f) {
        ++win.calls[r];
        if ((++seq[r] & (kSampleEvery - 1)) != 0) {
            win.bytes[r] += f();
            return;
        }
        if (overheadTicks < 0) measureOverhead();
        const int64_t t0 = clockNow();
        const uint64_t b = f();
        const int64_t dt = clockNow() - t0;
        win.bytes[r] += b;
        ++win.sampled[r];
        win.ticks[r] += dt > overheadTicks ? dt - overheadTicks : 0;
    }

    // A reader whose own early-out is known to the caller: when it will not read, run it uncounted.
    template <class F>
    void timedIf(bool reads, Reader r, F&& f) {
        if (reads) timed(r, static_cast<F&&>(f));
        else f();
    }

    void noteParticleCapture(bool cameraAlsoReadThisBlock) {
        ++win.particleCaptures;
        if (cameraAlsoReadThisBlock) ++win.particleSharedWithCamera;
    }
};

inline Probe g_probe;

// The window's line. `extra` is appended verbatim (the particle target's state). A reader with no call
// reads "not exercised".
inline size_t formatLine(char* out, size_t cap, const Window& w, int64_t overheadTicks, int64_t ticksPerSecond,
                         const char* extra) {
    if (!out || cap == 0) return 0;
    size_t n = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (n >= cap) return;
        const int k = std::snprintf(out + n, cap - n, fmt, a...);
        if (k > 0) n += static_cast<size_t>(k) < cap - n ? static_cast<size_t>(k) : cap - n - 1;
    };
    put("unmap readers (%llu frame(s), 1 call in %u timed, clock pair %.0f ns):",
        static_cast<unsigned long long>(w.frames), kSampleEvery,
        overheadTicks > 0 ? 1e9 * static_cast<double>(overheadTicks) / static_cast<double>(ticksPerSecond) : 0.0);
    double totalUsFrame = 0.0;
    bool anyTimed = false;
    for (int r = 0; r < kReaderCount; ++r) {
        put("%s %s", r ? ";" : "", readerName(r));
        if (w.calls[r] == 0) { put(" not exercised"); continue; }
        const double calls = static_cast<double>(w.calls[r]);
        const double perFrame = w.frames ? calls / static_cast<double>(w.frames) : 0.0;
        put(" %.1f/frame %.0f B/call", perFrame, static_cast<double>(w.bytes[r]) / calls);
        if (w.sampled[r] == 0) { put(" no timed sample"); continue; }
        const double usCall = 1e6 * static_cast<double>(w.ticks[r]) / static_cast<double>(ticksPerSecond) /
                              static_cast<double>(w.sampled[r]);
        const double usFrame = usCall * perFrame;
        put(" %.2f us/call %.1f us/frame", usCall, usFrame);
        totalUsFrame += usFrame;
        anyTimed = true;
    }
    if (anyTimed) put("; total %.1f us/frame", totalUsFrame);
    if (w.particleCaptures)
        put("; particle block is the camera block on %llu of %llu capture(s)",
            static_cast<unsigned long long>(w.particleSharedWithCamera), static_cast<unsigned long long>(w.particleCaptures));
    if (extra && extra[0]) put("; %s", extra);
    return n;
}

// Once per frame, from the owner thread's frame boundary. Returns true when `out` holds a line to log.
// extraFn(char*, size_t) fills the tail of the line and runs only when a line is due.
template <class ExtraFn>
inline bool frameTick(Probe& p, uint64_t nowMs, char* out, size_t cap, ExtraFn&& extraFn) {
    ++p.win.frames;
    if (p.windowStartMs == 0) { p.windowStartMs = nowMs ? nowMs : 1; return false; }
    if (nowMs - p.windowStartMs < kWindowMs) return false;
    if (p.overheadTicks < 0) p.measureOverhead();
    char extra[192] = {};
    extraFn(extra, sizeof extra);
    formatLine(out, cap, p.win, p.overheadTicks, clockFrequency(), extra);
    p.win = Window{};
    p.windowStartMs = nowMs ? nowMs : 1;
    return true;
}

}  // namespace unmapprobe
}  // namespace edvr
