#pragma once
#include <array>
#include <cstdint>

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
    float worldTranslation[3] = {}, bodyTranslation[3] = {};
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
