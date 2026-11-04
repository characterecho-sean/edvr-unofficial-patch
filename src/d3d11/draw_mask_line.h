// The may-want byte's counters and window line (vs_may_want.h; perf_monitor.cpp prints the line every 1800 frames beside "draw hook CPU:"). Counts what
// the byte skipped, per subscriber, and the draws that went through the full path, so a window of zeros can be told from a build without the byte:
// the line is written every window. tools\edvr_log.py --perf-windows parses it; its self-test holds the fragments below to this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

enum class MaskSkip : uint8_t { UiFamily, ScreenUi, ScreenRecognize, WeaponMotion, HoloDepth, Count };
constexpr size_t kMaskSkips = static_cast<size_t>(MaskSkip::Count);

inline const char* maskSkipName(size_t k) {
    static const char* const names[kMaskSkips] = {"ui_family", "screen_motion_ui", "screen_motion", "weapon_motion", "hologram_depth"};
    return k < kMaskSkips ? names[k] : "unknown";
}

struct DrawMaskCounts {
    uint32_t draws = 0;              // draws that went through the full path (forwardWithVerdict)
    uint32_t skips[kMaskSkips] = {};   // calls skipped on a clear bit
};

inline void formatDrawMaskWindow(char* out, size_t cap, unsigned frame, const DrawMaskCounts& c) {
    int n = std::snprintf(out, cap, "draw mask: 1800-frame window ending %u; %u draws through the full path; calls skipped by the vertex shader's may-want byte: ", frame, c.draws);
    if (n < 0 || static_cast<size_t>(n) >= cap) return;
    size_t at = static_cast<size_t>(n);
    for (size_t k = 0; k < kMaskSkips; ++k) {
        n = std::snprintf(out + at, cap - at, "%s%s %u", k ? ", " : "", maskSkipName(k), c.skips[k]);
        if (n < 0 || static_cast<size_t>(n) >= cap - at) return;
        at += static_cast<size_t>(n);
    }
    std::snprintf(out + at, cap - at, ".");
}

}  // namespace edvr
