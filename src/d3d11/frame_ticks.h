// Where the time went inside EDVR's own Present hook, per frame.
//
// WHY. A native OpenXR flight (2026-09-29, phase 0) put the largest phase of 47
// of 54 long cycles between Elite's second Submit and its next WaitGetPoses. That
// window holds Elite's Present call, which d3d11.dll hooks: about fifty module
// ticks, the hotkeys, the menu, the config reload. Nothing said which of it was
// EDVR's. The LONG FRAME line printed no share at all -- and the boundary figure
// it was meant to carry (perf_monitor's cpuBoundaryMs) is stamped AFTER that line
// is written, so it could only ever read 0.00.
//
// WHAT IT KEEPS. One chain of clock reads on the render thread. The Present hook
// starts the chain (enter), then each tick names the stretch since the previous
// mark (mark). Three kinds of time never mix:
//
//   ticks      EDVR's own work in the hook, each one named. Kept per frame:
//              the sum, the frame boundary's part of the sum, and the three
//              slowest with their names.
//   real       the driver's Present call itself (external): not EDVR's, so it
//              is timed but kept out of the ticks and out of the top three.
//   outside    everything between one hook's return and the next one's entry:
//              Elite, and the OpenXR runtime's Submit and WaitGetPoses. Not
//              measured here; it is what is left of the frame.
//
// A FRAME IS THE MONITOR'S FRAME. perf_monitor's clock (perfMonitorFrame, run
// from inside the boundary) is the frame's edge, and cut() takes the same clock
// reading, so the ticks in one frame's summary are exactly the ticks measured
// since the previous frame's edge: the tail of the previous Present hook, the
// game's own time, and the head of this hook up to the edge. A tick that spans
// the edge (the menu's, which contains perfMonitorFrame) is split at it. The
// three parts of a frame -- hook ticks, real Present, outside -- add up to the
// frame's length with nothing left over.
//
// THE COST. One QueryPerformanceCounter read per mark and a compare against the
// third-slowest tick; a name is a literal whose pointer is kept. About sixty
// marks a frame is a microsecond or two. No allocation, no lock, no log write.
//
// THREADING. The render thread only: the Present hook and everything it calls.
// The state is plain, not atomic; a second thread marking would corrupt it, so
// nothing else may.
//
// A rig that runs this file alone (native_perf_history_test) is why it depends on
// nothing but the Windows clock.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace edvr {

// One named span of EDVR's time. The name is a literal; the recorder keeps the
// pointer, never a copy.
struct FrameTick {
    const char* name = nullptr;
    float ms = 0.0f;
};

// One frame's worth of the chain, handed over by FrameTicks::cut.
struct FrameTickSummary {
    float hookMs = 0.0f;       // every tick: EDVR's work in the Present hook, the real Present excluded
    float boundaryMs = 0.0f;   // of hookMs, the frame-boundary block's ticks
    float realMs = 0.0f;       // the driver's own Present call
    uint32_t marks = 0;        // ticks recorded (0: the chain never ran in this frame)
    uint32_t hooks = 0;        // Present hooks entered in this frame (normally 1)
    FrameTick top[3];          // the three slowest ticks, slowest first; name null when unused
};

class FrameTicks {
public:
    static int64_t now() noexcept {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    // The owned swapchain's Present hook was entered at `at`. Whatever came
    // since the last mark was the game's, not a tick, so the chain restarts.
    //
    // In a profile that never reaches perfMonitorFrame (the flat one) nothing
    // ever cuts, so the sums would grow for the whole session and the slowest
    // three would freeze on the first slow frame. Every few hooks without a cut
    // drops what has piled up.
    void enter(int64_t at) noexcept {
        if (++hooksSinceCut_ > kMaxUncutHooks) {
            clearFrame();
            hooksSinceCut_ = 1;
        }
        ++hooks_;
        last_ = at;
    }

    // Inside the frame-boundary block, ticks also count toward boundaryMs.
    void boundary(bool inside) noexcept { inBoundary_ = inside; }

    // The stretch since the previous mark was `name`'s. A mark before any chain
    // has started only starts one; a clock that stepped back records nothing.
    void markAt(const char* name, int64_t at) noexcept {
        if (last_ <= 0) {
            last_ = at;
            return;
        }
        if (at <= last_) return;
        const int64_t d = at - last_;
        last_ = at;
        ticks_ += d;
        if (inBoundary_) boundaryTicks_ += d;
        ++marks_;
        if (d > top_[2].ticks) insert(name, d);
    }
    void mark(const char* name) noexcept { markAt(name, now()); }

    // The stretch since the previous mark, ended at `at`, was the driver's
    // Present. Timed, not a tick.
    void external(int64_t at) noexcept {
        if (last_ <= 0 || at <= last_) return;
        realTicks_ += at - last_;
        last_ = at;
    }

    // The frame's edge, read once by the caller (`at`, the same reading the
    // monitor's frame length uses): the stretch since the last mark is `name`'s,
    // the chain closes there, and this frame's numbers are handed over. The
    // chain keeps running from `at`, so a tick in progress is split at the edge.
    FrameTickSummary cut(const char* name, int64_t at, int64_t frequency) noexcept {
        markAt(name, at);
        FrameTickSummary s;
        const double toMs = frequency > 0 ? 1000.0 / static_cast<double>(frequency) : 0.0;
        s.hookMs = static_cast<float>(static_cast<double>(ticks_) * toMs);
        s.boundaryMs = static_cast<float>(static_cast<double>(boundaryTicks_) * toMs);
        s.realMs = static_cast<float>(static_cast<double>(realTicks_) * toMs);
        s.marks = marks_;
        s.hooks = hooks_;
        for (int i = 0; i < 3; ++i) {
            s.top[i].name = top_[i].name;
            s.top[i].ms = static_cast<float>(static_cast<double>(top_[i].ticks) * toMs);
        }
        clearFrame();
        hooksSinceCut_ = 0;
        return s;
    }

private:
    static constexpr unsigned kMaxUncutHooks = 8;
    struct Top {
        const char* name = nullptr;
        int64_t ticks = 0;
    };

    void insert(const char* name, int64_t d) noexcept {
        int i = 2;
        while (i > 0 && d > top_[i - 1].ticks) {
            top_[i] = top_[i - 1];
            --i;
        }
        top_[i] = {name, d};
    }
    void clearFrame() noexcept {
        ticks_ = boundaryTicks_ = realTicks_ = 0;
        marks_ = hooks_ = 0;
        top_[0] = top_[1] = top_[2] = {};
    }

    int64_t last_ = 0;
    int64_t ticks_ = 0, boundaryTicks_ = 0, realTicks_ = 0;
    uint32_t marks_ = 0, hooks_ = 0, hooksSinceCut_ = 0;
    bool inBoundary_ = false;
    Top top_[3];
};

// The one recorder: the Present hook's, and so the render thread's.
inline FrameTicks g_frameTicks;
inline void frameTick(const char* name) noexcept { g_frameTicks.mark(name); }

// ---- the two LONG FRAME clauses, as pure text ---------------------------------

// snprintf that keeps count instead of walking off the end.
inline void frameTickAppend(char* buf, size_t cap, size_t& len, const char* fmt, ...) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    len = static_cast<size_t>(n) < cap - len ? len + static_cast<size_t>(n) : cap - 1;
}

// Engine motion's CPU time in one frame (engine_motion_cpu.h): EDVR's own work
// in its hooks on the game's code, on the thread that called this Present,
// every call clocked and the frame's own -- exact, not sampled and not held over.
// It is part of "outside the hook" like the draw hooks, not of the partition.
struct EngineMotionFrame {
    bool measured = false;          // false: the priming frame, or no clock rate; no clause
    double renderMs = 0.0;
    unsigned long long calls = 0;   // clocked scopes on this thread this frame; 0 = the code never ran
};

// "EDVR in this frame: ..." -- EDVR's share of one frame, from its summary.
// frameMs is the frame's length on the same clock the summary was cut with.
// Draw-hook time is a separate, estimated quantity (perf_monitor.h): it is inside
// "outside the hook", not part of the partition, and on a frame that was not
// sampled it is the last sampled frame's figure held over, which the text says.
// The engine-motion clause is exact for this frame and is the render thread's.
// No calls reads "none this frame", never 0.00 ms: a 0.00 is code that ran and
// rounds to nothing. (The call count is not in the clause: the LONG FRAME line
// is at its size, and the 30 s report carries the rates.)
inline size_t formatEdvrShare(char* buf, size_t cap, double frameMs, const FrameTickSummary& t,
                              double drawsMs, bool drawsFresh, const EngineMotionFrame& em = EngineMotionFrame{}) {
    size_t len = 0;
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
    double outside = frameMs - static_cast<double>(t.hookMs) - static_cast<double>(t.realMs);
    if (outside < 0.0) outside = 0.0;
    frameTickAppend(buf, cap, len,
                    "EDVR in this frame: %.2f ms in the Present hook (frame boundary %.2f ms), "
                    "%.2f ms in the real Present, draw hooks ~%.2f ms (%s), %.2f ms outside the hook; "
                    "slowest EDVR ticks: ",
                    static_cast<double>(t.hookMs), static_cast<double>(t.boundaryMs),
                    static_cast<double>(t.realMs), drawsMs,
                    drawsFresh ? "sampled this frame" : "held over", outside);
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        if (!t.top[i].name) continue;
        frameTickAppend(buf, cap, len, "%s%s=%.2f ms", any ? ", " : "", t.top[i].name,
                        static_cast<double>(t.top[i].ms));
        any = true;
    }
    if (!any) frameTickAppend(buf, cap, len, "none recorded");
    frameTickAppend(buf, cap, len, ";");
    if (em.measured) {
        if (em.calls) frameTickAppend(buf, cap, len, " engine motion %.2f ms;", em.renderMs);
        else frameTickAppend(buf, cap, len, " engine motion none this frame;");
    }
    return len;
}

// Everything the native LONG FRAME line says, in one place so a rig can hold it
// to its size: Log::note cuts a line at about 1170 bytes and marks the cut, and
// the tail is the runtime sequence and game work -- the only key that lays this
// line against the OpenXR half's native_long_cycle. The share goes before the
// events and the frame stamp, which are the parts that can grow.
struct NativeLongFrame {
    double frameMs = 0.0;
    const char* reference = "";     // "runtime predicted period 11.1 ms"
    uint32_t textures = 0, buffers = 0, shaders = 0;
    double creationMb = 0.0;
    const char* share = "";         // formatEdvrShare's text
    const char* events = "";        // "none", or the event list
    const char* stamp = "";         // " This is frame N ..."
    unsigned long long sequence = 0;
    const char* gameWork = "";      // "7.11 ms" or "unavailable"
};
inline size_t formatNativeLongFrame(char* buf, size_t cap, const NativeLongFrame& l) {
    size_t len = 0;
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
    frameTickAppend(buf, cap, len,
                    "monitor: LONG FRAME -- %.1f ms between Presents (%s), no WaitGetPoses, CPU busy, "
                    "compositor, reprojection, or door samples; game creations: %u textures, %u buffers, "
                    "%u shaders (%.1f MB); %s EDVR events: %s.%s runtime sequence %llu, game work %s.",
                    l.frameMs, l.reference, l.textures, l.buffers, l.shaders, l.creationMb, l.share,
                    l.events, l.stamp, l.sequence, l.gameWork);
    return len;
}

}  // namespace edvr
