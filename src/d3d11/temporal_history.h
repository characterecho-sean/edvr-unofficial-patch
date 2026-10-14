#pragma once
#include <array>
#include <cstdint>

#include "../common/camera_target_tag.h"

namespace edvr {
// CPU-only evidence retained between diagnostic key presses. No GPU queries
// or readbacks; the caller serializes access and logs a copy outside its lock.
struct TemporalHistoryEntry {
    uint64_t qpc = 0;
    uint32_t frame = 0, eye = 0, flags = 0, inputs = 0, events = 0;
    uint32_t width = 0, height = 0, outputWidth = 0, outputHeight = 0;
    // 0 refused, 1 native TAA, 2 full NVIDIA, 3 native + NVIDIA fovea.
    uint32_t output = 0;
    float jitterX = 0, jitterY = 0;
    float worldTranslation[3] = {};
    // Retrospective camera-row provenance. The temporal path still uses one
    // shared selection; these fields only preserve which write it selected and
    // the first recognised rigid scene draw tagged for this entry's eye. `c`
    // is reported only as the scene-row origin; whether it is an eye pose,
    // camera centre, or another render origin is evidence for the flight.
    uint32_t cameraChoiceFlags = 0, cameraDrawFlags = 0;
    uint32_t selectedSeq = 0, boundLatchSeq = 0, twinSeq = 0, drawSeq = 0;
    uint32_t writesAtChoice = 0, writesAtDraw = 0;
    uint32_t observedWritesAtChoice = 0, observedWritesAtDraw = 0;
    uint32_t observedEvictionsAtChoice = 0, observedEvictionsAtDraw = 0;
    uint32_t candidates = 0, boundCandidates = 0;
    uint32_t continuousCandidates = 0, twinCandidates = 0;
    uint64_t selectedResource = 0, boundResource = 0, drawResource = 0, drawVsHash = 0;
    float selectedRows[12] = {}, drawRows[12] = {};
    float selectedProj[2] = {}, drawProj[2] = {};
    float selectedDrawOffset[3] = {};  // selected c minus this eye draw's c, world coordinates
    float selectedDrawRotationDeg = 0;
    float drawTranslation[3] = {};     // this eye's draw rows, current -> previous view
    float drawRotationDeg = 0;
    // The render target the selected write was drawn into (the first draw after the write), and the tagged eye draw's own
    // (docs/sky-flicker-2026-10-13.md). Unknown (src 0) when no draw followed the write.
    camtag::Target selectedTarget, drawTarget;
    uint32_t selectedDraws = 0;        // the owner-context draws made between the selected write and the next write
    uint32_t rejectedWrites = 0;      // writes this frame's chooser rejected as not drawn into an eye-sized target or drawn into the submitted texture (the eye-target rule)
};

template<uint32_t Capacity = 4096> class TemporalHistory {
    static_assert(Capacity > 0, "history must retain an entry");
    std::array<TemporalHistoryEntry, Capacity> entries_{};
    uint32_t next_ = 0, count_ = 0;
public:
    void clear() { next_ = count_ = 0; }
    void record(const TemporalHistoryEntry& entry) {
        entries_[next_] = entry;
        next_ = (next_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
    }
    uint32_t size() const { return count_; }
    const TemporalHistoryEntry& oldest(uint32_t index) const {
        return entries_[(next_ + Capacity - count_ + index) % Capacity];
    }
};
} // namespace edvr
