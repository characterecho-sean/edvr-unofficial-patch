#pragma once
// The overlay's GPU TIME tile (perf_monitor.cpp, 2026-10-14): which number it shows. Pure, so tools/aa_off_test runs the
// decision the overlay makes. The application-render mean (advanced.app_gpu_timing on) wins when it has samples; otherwise
// the frame GPU span from gpuCensusFrame (the census's own frame scope, on with the application clock off) when one has
// completed; otherwise "--" with the reason.
#include <cstddef>
#include <cstdio>

namespace edvr {

// The frame span's timer slots (gpu_census.cpp): a pair is held until it is read back four frames later, so five are
// in flight at most; six leaves one spare. Two timestamps a pair.
constexpr unsigned kFrameSpanSlots = 6;

inline bool gpuTimeTileText(bool appHave, double appMeanMs, bool spanHave, double spanMs, const char* noneReason,
                            char* value, size_t valueLen, char* sub, size_t subLen) {
    if (!value || !sub || !valueLen || !subLen) return false;
    if (appHave) {
        std::snprintf(value, valueLen, "%.1f", appMeanMs);
        std::snprintf(sub, subLen, "ms application render; elapsed");
        return true;
    }
    if (spanHave) {
        std::snprintf(value, valueLen, "%.1f", spanMs);
        std::snprintf(sub, subLen, "ms frame GPU work span (first command to last; includes GPU idle inside the frame)");
        return true;
    }
    std::snprintf(value, valueLen, "--");
    std::snprintf(sub, subLen, "%s", noneReason ? noneReason : "");
    return false;
}

// WHICH number the GPU figure shows, shared by the menu's GPU TIME tile and the in-headset fps overlay line
// (perf_monitor.cpp, 2026-10-14): the two surfaces once chose separately and only the tile learned the work span, so the
// overlay read "gpu -- ms" for a flight with the application clock off. One choice, both callers.
//   clockOn       advanced.app_gpu_timing is on (the application-render clock is live)
//   appReady      the caller's own test that the application-render figure is valid right now (the tile passes clockOn; the
//                 overlay passes nativeApplicationGpuReady, which also tests the snapshot's age and source)
//   historyHas    the application-render series has samples; with the clock off that series holds the work span (noteFrameWork)
//   spanDone/spanLastMs  the census's last completed work-span pair, the fallback before the series has a sample
enum class GpuTimeSource { None, ApplicationRender, WorkSpan };
struct GpuTimeChoice {
    GpuTimeSource source = GpuTimeSource::None;
    double ms = 0.0;
};
inline GpuTimeChoice chooseGpuTime(bool clockOn, bool appReady, bool historyHas, double historyMeanMs, bool spanDone,
                                   double spanLastMs) {
    GpuTimeChoice c;
    if (appReady && historyHas) {
        c.source = GpuTimeSource::ApplicationRender;
        c.ms = historyMeanMs;
    } else if (!clockOn && (historyHas || spanDone)) {
        c.source = GpuTimeSource::WorkSpan;
        c.ms = historyHas ? historyMeanMs : spanLastMs;
    }
    return c;
}

// The overlay's one line, "NN fps   gpu X ms   cpu Y ms": "--" only where the choice is None (or the CPU figure is absent).
inline void overlayLineText(char* buf, size_t len, double fps, const GpuTimeChoice& gpu, const char* cpuLabel, bool haveCpu,
                            double cpuMs) {
    if (!buf || !len) return;
    char gpuValue[24] = "--", cpuValue[24] = "--";
    if (gpu.source != GpuTimeSource::None) std::snprintf(gpuValue, sizeof(gpuValue), "%.1f", gpu.ms);
    if (haveCpu) std::snprintf(cpuValue, sizeof(cpuValue), "%.1f", cpuMs);
    std::snprintf(buf, len, "%.0f fps   gpu %s ms   %s %s ms", fps, gpuValue, cpuLabel ? cpuLabel : "cpu", cpuValue);
}

// The census window line's frame work span field (gpu_census.cpp; tools\edvr_log.py --gpu-work-span parses exactly this text).
// All-time counts of the span pair: completed = pairs read back, skipped = pairs not begun, invalid = pairs the GPU or the
// disjoint scope voided. "last" is "-" until one has completed.
inline void formatFrameWorkSpanField(char* buf, size_t len, unsigned long long completed, unsigned long long skipped,
                                     unsigned long long invalid, double lastMs) {
    if (!buf || !len) return;
    char last[32] = "-";
    if (completed) std::snprintf(last, sizeof(last), "%.3f", lastMs);
    std::snprintf(buf, len, "frame work span: completed %llu, skipped %llu, invalid %llu, last %s ms", completed, skipped, invalid,
                  last);
}

// The work span's arithmetic (gpu_census.cpp's pair): the GPU time from the first command's timestamp to the door end's.
// Clamped at zero: a pair the GPU reports out of order is not a negative frame.
inline double frameWorkSpanMs(double firstCommandMs, double doorEndMs) {
    const double span = doorEndMs - firstCommandMs;
    return span > 0.0 ? span : 0.0;
}

}  // namespace edvr
