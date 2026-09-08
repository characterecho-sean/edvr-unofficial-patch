// The Monitor page's numbers (docs/settings-menu.md): fpsVR's readout,
// gathered as cheaply as it can be.
//
// WHAT IT SHOWS AND WHERE EACH NUMBER COMES FROM
//
//   frame rate, frame time, 1% low   the Present-to-Present interval, ringed
//                                    here every frame (one clock read and a
//                                    store; the ring is the only per-frame
//                                    work when the page is not showing)
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

// The page's rows, label and value, up to `max`. Returns how many.
struct PerfLine {
    char left[48];
    char right[80];
};
int perfMonitorLines(PerfLine* out, int max);

// The last `max` frame intervals in milliseconds, oldest first, and the
// display's frame budget (1000 / Hz, or 11.1 when the rate is unknown).
// Returns how many.
int perfMonitorGraph(float* out, int max, float* budgetMs);

// The one-line readout for the head-locked overlay (menu.fps_overlay):
// frame rate and time over the last second, the app's GPU time, and the
// frames dropped in the last ten seconds. Needs nothing the slow samplers
// gather.
void perfMonitorOverlayLine(char* buf, size_t bufLen);

void perfMonitorShutdown();

}  // namespace edvr
