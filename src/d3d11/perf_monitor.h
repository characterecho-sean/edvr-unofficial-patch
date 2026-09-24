// The Monitor page's numbers (docs/settings-menu.md): fpsVR's readout,
// gathered as cheaply as it can be.
//
// WHAT IT SHOWS AND WHERE EACH NUMBER COMES FROM
//
//   frame rate, frame time, 1% low   the Present-to-Present interval, ringed
//                                    here every frame (one clock read and a
//                                    store; the ring is the only per-frame
//                                    work when the page is not showing)
//   DISPLAY (native path only)       the runtime's predicted display period
//                                    as Hz, its base rate, and a THROTTLED
//                                    flag when the prediction passes 1.5x
//                                    base -- the display side the producer
//                                    cadence cannot show
//   app GPU, compositor GPU,         the compositor's own frame timing, read
//   dropped, reprojected             by the openvr half once a frame and
//                                    published on the channel (frame_timing.h)
//   display rate, eye size           the channel
//   CPU load, RAM                    GetSystemTimes / GetProcessTimes /
//                                    GlobalMemoryStatusEx, once a second
//   VRAM                             IDXGIAdapter3::QueryVideoMemoryInfo,
//                                    once a second
//   GPU load and temperature         NvAPI, once a second, NVIDIA only --
//                                    the same library the foveation loads,
//                                    two entry points; elsewhere "n/a"
//   EDVR's own passes                the temporal, DLSS and sharpen totals
//                                    those passes already keep
//
// THE COST, by design: the once-a-second samplers run ONLY while the page
// is showing (a two-second grace so the first numbers are there when it
// opens), so a closed menu pays the ring and nothing else; the page itself
// re-rasterises at 4 Hz on the worker thread. Nothing here writes a file.
#pragma once

#include <cstdint>

struct ID3D11Device;

namespace edvr {

// Every frame, from the frame boundary: the interval ring, and the
// compositor sample if a new one was published.
void perfMonitorFrame(ID3D11Device* dev);

// The page is showing (or not): starts and stops the once-a-second samplers.
void perfMonitorSetActive(bool active);

// The page's gauges: a caption, a big value and one small line each, in
// the order they are laid out four across. Returns how many.
struct PerfTile {
    char caption[24];
    char value[24];
    char sub[40];
};
int perfMonitorTiles(PerfTile* out, int max);

// The most recent dropped or long frame, with EDVR's events in it, as one
// line for under the gauges.
void perfMonitorLastDropLine(char* buf, size_t bufLen);

// One strip of the last `max` frames in milliseconds, oldest first, and the
// display's frame budget (1000 / Hz, or 11.1 when the rate is unknown).
// Returns how many. `which` picks what is plotted: the GPU frame the
// compositor measured, or its app CPU time (render-thread fallback) -- fpsVR draws
// the two as separate strips, and a frame over budget on one of them is a
// different problem from a frame over budget on the other.
enum PerfGraph { kGraphGpu = 0, kGraphCpu = 1, kGraphPeriod = 2 };
// Native mode uses independent unique completion samples for kGraphGpu
// (producer span) and kGraphCpu (the CPU figure: the runtime's caller work per
// cycle, or an older runtime's pre-submit application time --
// NativePerfHistory::cpuFigure), not compositor CPU/GPU frames.
// The reference is the fresh runtime-predicted period, or 0 if unavailable.
int perfMonitorGraph(int which, float* out, int max, float* budgetMs);

// The one-line readout for the head-locked overlay (menu.fps_overlay):
// frame rate over the last second, CPU/GPU over the menu's recent window,
// and frames dropped in the last ten seconds. Without app timing the CPU
// fallback is labelled "thread". Needs nothing the slow samplers
// gather.
// Native gpu/cpu labels show independent 200ms means of the producer GPU and
// the CPU figure: "cpu" is the caller work per cycle (timing v5: the game's
// thread from one pose wait's return to the next, submits and their waits
// included -- wall time, not exclusive CPU execution); "cpu (pre-submit)" is
// an older runtime's application time, pose wait end to submit only.
void perfMonitorOverlayLine(char* buf, size_t bufLen);

// The local D3D11 render-to-submit diagnostic, kept separate from SteamVR's
// compositor numbers. The line reports disabled, pending, invalid, or stale
// state explicitly rather than treating it as a zero.
void perfMonitorLocalGpuLine(char* buf, size_t bufLen);
// Native OpenXR timing is wall time on the serialized owner/producer path:
// submit spans include blocking and rendezvous, while producer spans include
// queued GPU work and exclude the XR device. These are never compositor times.
void perfMonitorNativeTimingLine(char* buf, size_t bufLen);

// DROP ATTRIBUTION (docs/settings-menu.md, "diagnosing drops caused by the
// mod"). Every frame's ring entry carries what EDVR did in it -- the events
// below, ORed in from wherever they happen (both halves: the openvr one
// crosses on the channel) -- and what EDVR's own work cost: the frame
// boundary's CPU time, the door's CPU time, the draw hooks' CPU time on
// sampled frames, and the door's GPU time from a timestamp pair. A dropped
// or long frame is then a row with EDVR's part of it written down, the
// Monitor page counts the drops that coincided with EDVR activity against
// the ones that did not, and a rate-limited log line carries the same
// evidence into a field report.
enum PerfEvent : uint32_t {
    kEvReload   = 1u << 0,   // edvr.ini re-read and every module reconfigured
    kEvIniWrite = 1u << 1,   // the menu wrote edvr.ini (the I/O is off-thread; the reload follows)
    kEvCompile  = 1u << 2,   // a shader compiled through shader_swap
    kEvWithhold = 1u << 3,   // the transition-flash fix withheld a submit
    kEvResubmit = 1u << 4,   // ...and handed the compositor the previous frame's copy
    kEvCensus   = 1u << 5,   // a draw census or quad probe was requested
    kEvRaster   = 1u << 6,   // the menu uploaded a fresh bitmap
    kEvBinds    = 1u << 7,   // Elite's bindings were re-read
    kEvNgx      = 1u << 8,   // NVIDIA's DLSS feature was (re)created
    kEvMenu     = 1u << 9,   // the menu opened or closed
    kEvFsr      = 1u << 10,  // AMD's FSR context was (re)created
    kEvEyeDump  = 1u << 11,  // a diagnostic eye dump was requested
};
// Note an event in the frame in progress; `ms` is the event's own duration
// where one is known (a compile, a reload), 0 otherwise. Any thread.
void perfMonitorNoteEvent(uint32_t bits, double ms = 0.0);

// EDVR's CPU time, credited to the frame most recently ringed: the frame
// boundary's body, or the door's (the latter arrives over the channel).
enum PerfCpu { kCpuBoundary = 0, kCpuDoor = 1 };
void perfMonitorNoteCpu(int which, double ms);

// The time the game's thread was blocked inside the real Present, noted by
// the swapchain hook before the frame is ringed; with the WaitGetPoses
// block (over the channel) it is subtracted from the frame period to give
// the render thread's own time, on the CPU TIME tile's sub-line. It is a
// larger window than the compositor's own poses-to-submit figure, by the
// work the game does after its second submit.
void perfMonitorNotePresentWait(double ms);

// Draw-hook sampling: on one frame in sixteen the draw thunks time
// themselves and the real call they forward; the difference is EDVR's own
// cost in the hooks, credited to that frame and shown as the running
// figure. The thunks pay two clock reads per draw on a sample frame and
// one branch otherwise.
// Is this a draw-sample frame (one in sixteen)? Inline, because every draw
// thunk's DrawClock asks, so it was a cross-TU call about 18k times a frame
// in a build with no /GL -- and on fifteen frames in sixteen the answer is no
// and the call was all of the cost.
//
// There is NO config key behind this: the draw-hook CPU attribution runs for
// one frame in sixteen in every session (perf_monitor.cpp's only two Config
// reads are fix.temporal_aa and fix.temporal_aa_model, neither related), and
// its consumers -- the 1800-frame "draw hook CPU" window line and cpuDrawsMs
// on the drop line -- are always present. So this cannot be armed away; it
// can only be made cheap to ask.
namespace detail { extern bool g_perfMonitorSampleDraws; }
inline bool perfMonitorSampleDraws() { return detail::g_perfMonitorSampleDraws; }

// ...and on a sample frame, only every this-many-th draw is clocked; the
// frame's figure is scaled back up by the same factor (perf_monitor.cpp).
//
// Clocking every draw of a sampled frame took four QueryPerformanceCounter
// reads a draw -- about 72k at a settlement, ~1.8 ms spent on ONE frame in
// sixteen: a periodic hitch the flown profile of 2026-09-22 averaged to 0.11
// ms a frame. At one draw in 64 the sampled frame pays ~1.1k clock reads,
// under 0.03 ms. The line keeps its meaning -- EDVR's own time in the draw
// hooks on a sampled frame, forwarded draw excluded -- and now says that it
// is an estimate. The selection is by draw ordinal on the drawing thread, so
// a scene whose costly draws recur with a period of exactly 64 would bias it;
// nothing in a frame's draw order is known to.
constexpr uint32_t kPerfMonitorDrawTimeStride = 64;
void perfMonitorDrawTicks(int64_t wholeTicks, int64_t realTicks);

void perfMonitorShutdown();

}  // namespace edvr

extern "C" {
// The door's GPU bracket, called by the openvr half around every pass it
// runs at the door for one eye (frame_timing.h): a timestamp pair on the
// game's immediate context, never awaited, polled on later calls. `tex`
// is any ID3D11Texture2D* on the game's device. The measured time per
// frame (both eyes) is the Monitor page's "EDVR at the door" GPU figure.
__declspec(dllexport) void edvrDoorGpuBegin(void* tex, int eye);
__declspec(dllexport) void edvrDoorGpuEnd(void* tex, int eye);
}
