#pragma once
#include <cstdint>

namespace edvr {
// Diagnostic only: no camera, phase, binding or admission decisions.
struct FlatHdrProjectionCapture {
    static constexpr uint32_t kWriterLimit = 64;
    static constexpr uint32_t kAttemptLimit = 4, kCompleteLimit = 2;
    static constexpr uint64_t kMinimumFrameSpacing = 91;
    bool armed = false, closed = false, partial = false, overflow = false;
    bool mutation = false, uncertain = false;
    uint64_t frame = 0;
    const void* target = nullptr;
    const void* depth = nullptr;
    uint32_t frames = 0, completeFrames = 0, writers = 0, payloads = 0, unknown = 0, donors = 0;
    bool exhausted() const { return frames >= kAttemptLimit || completeFrames >= kCompleteLimit; }
    void arm() { *this = {}; armed = true; }
    bool begin(uint64_t f, const void* h, const void* d, uint32_t priorWrites) {
        if (!armed || exhausted() || target || !h ||
            (frames && (f <= frame || f-frame < kMinimumFrameSpacing))) return false;
        frame = f; target = h; depth = d; closed = false;
        partial = priorWrites != 0; overflow = mutation = uncertain = false;
        writers = payloads = unknown = donors = 0;
        return true;
    }
    bool writer(uint64_t f, const void* h) {
        if (!armed || closed || !target || f != frame || h != target) return false;
        ++writers;
        if (writers > kWriterLimit) { overflow = true; return false; }
        return true;
    }
    void written(const void* resource) {
        if (target && !closed && (resource == target || resource == depth)) mutation = true;
    }
    bool complete() const {
        return closed && writers && !partial && !overflow && !mutation && !uncertain &&
            payloads == writers && donors == 1;
    }
    void finishFrame() { if (target) { ++frames; if(complete())++completeFrames; } target = depth = nullptr; }
};
inline bool flatHdrCaptureRange(uint32_t first, uint32_t count, uint32_t row, uint32_t rows) {
    return rows && row <= count && rows <= count-row && row <= UINT32_MAX/16u &&
        rows <= UINT32_MAX/16u-row &&
        first <= UINT32_MAX/16u-row-rows;
}
}
