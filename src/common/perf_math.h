// The Monitor page's arithmetic, header-only so tools/menu_test can pin it.
//
// A frame-time ring is summarised the way fpsVR summarises one: the mean,
// the worst, and the "1% low" -- the mean of the SLOWEST one percent of
// frames (one frame in a hundred, ten in a thousand), which is the number
// that says whether the judder you felt was real when the mean says
// everything is fine. Selection on a copy, not a sort of the ring in
// place, because the ring keeps arriving.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace edvr {

struct PerfStats {
    float avgMs = 0.0f;
    float maxMs = 0.0f;
    float p99Ms = 0.0f;   // the 1% low, as a frame time: the slowest 1% averaged
    float p50Ms = 0.0f;   // the median
    int   count = 0;
};

// Over `n` samples in milliseconds; samples that are not positive or are
// over `capMs` (a loading-screen stall, a debugger) are left out so one
// pause does not own the mean for ten seconds.
inline PerfStats perfStatsOf(const float* ms, int n, float capMs = 500.0f) {
    PerfStats s;
    if (!ms || n <= 0) return s;
    std::vector<float> v;
    v.reserve(static_cast<size_t>(n));
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const float x = ms[i];
        if (!(x > 0.0f) || x > capMs) continue;
        v.push_back(x);
        sum += x;
        if (x > s.maxMs) s.maxMs = x;
    }
    if (v.empty()) return s;
    s.count = static_cast<int>(v.size());
    s.avgMs = static_cast<float>(sum / static_cast<double>(v.size()));
    // The median: the middle element by selection.
    {
        const size_t k = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        s.p50Ms = v[k];
    }
    // The 1% low: the slowest ceil(n / 100) frames, averaged. With fewer
    // than a hundred frames that is the single worst one.
    {
        size_t worst = (v.size() + 99) / 100;
        if (worst < 1) worst = 1;
        if (worst > v.size()) worst = v.size();
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() - worst), v.end());
        double acc = 0.0;
        for (size_t i = v.size() - worst; i < v.size(); ++i) acc += v[i];
        s.p99Ms = static_cast<float>(acc / static_cast<double>(worst));
    }
    return s;
}

// Frames per second for a frame time, 0 for nothing.
inline float perfFpsOf(float ms) { return ms > 0.0f ? 1000.0f / ms : 0.0f; }

}  // namespace edvr
