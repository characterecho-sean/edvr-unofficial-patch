// The config refresh window line (perf_monitor.cpp, every 1800 frames): how many times the settings file was re-read and every module reconfigured on the frame thread in
// the window, what that cost, and how many menu edits were queued. Written every window so a window with no refresh can be told from a build without the line; the closing
// sentence therefore belongs to that case alone (F16's first build printed it after counts that were not zero).
#pragma once
#include <cstddef>
#include <cstdio>

namespace edvr {

inline void formatConfigRefreshWindow(char* out, size_t cap, unsigned frame, unsigned reloads, double reloadMs, double reloadMaxMs, unsigned edits) {
    std::snprintf(out, cap,
                  "config refresh: 1800-frame window ending %u; %u refreshes on the frame thread (the settings file re-read and every module reconfigured), %.1f ms in all, "
                  "%.2f ms mean, %.2f ms max; %u menu edits queued for the settings file.%s",
                  frame, reloads, reloadMs, reloads ? reloadMs / reloads : 0.0, reloadMaxMs, edits,
                  reloads == 0 && edits == 0 ? " No refresh and no menu edit in this window." : "");
}

}  // namespace edvr
