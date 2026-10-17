#pragma once

#include <cstdint>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace edvr::nofoot::optic_capture {

constexpr uint32_t kCount = 16;

enum class SourceState : uint8_t { Unsampled, Matched, Inactive, Missing, Stale, Future, Invalid, Unmatched };

struct RowDescriptor {
    bool scheduled = false;
    uint32_t sceneFrame = 0, epoch = 0;
    uint64_t sequence = 0;
    char imageStatus[32] = "unscheduled";
};

inline bool formatJsonNumber(char* out, size_t capacity, float value) noexcept {
    if (!std::isfinite(value)) {
        if (capacity) std::snprintf(out, capacity, "null");
        return false;
    }
    if (capacity) std::snprintf(out, capacity, "%.9g", static_cast<double>(value));
    return true;
}

struct Record {
    bool sampled = false;
    uint32_t index = 0, sceneFrame = 0, epoch = 0;
    uint64_t sequence = 0;
    char imageStatus[32] = "unscheduled";
    SourceState state = SourceState::Unsampled;
    bool p14BasisValid = false, p14PositionValid = false;
    uint32_t adsFrame = ~0u, fpFrame = ~0u, gGameFrame = ~0u, rootReadFrame = ~0u, eyeFrame = ~0u;
    uint32_t fpGeneration = 0;
    uint64_t sampleQpc = 0, adsQpc = 0, fpQpc = 0, gQpc = 0, rootReadQpc = 0, eyeQpc = 0;
    bool adsActive = false, fpRead = false, gRead = false, eyeRead = false;
    bool itemRead = false;
    uint32_t itemSerial = 0;
    char itemName[64] = "";
    float p14Model[3] = {}, p14World[3] = {}, p14Basis[9] = {}, rootForward[3] = {};
    float eyeRows[16] = {}, eyeOrigin[4] = {};
};

struct Ledger {
    bool armed = false;
    uint32_t duplicates = 0;
    uint64_t stamp = 0;
    Record rows[kCount] = {};
    RowDescriptor requested[kCount] = {};

    void begin(uint64_t runStamp) noexcept {
        armed = true;
        duplicates = 0;
        stamp = runStamp;
        for (uint32_t i = 0; i < kCount; ++i) { rows[i] = Record{}; requested[i] = RowDescriptor{}; }
    }

    bool sample(uint32_t index, const Record& row) noexcept {
        if (!armed || index >= kCount) return false;
        if (rows[index].sampled) { ++duplicates; return false; }
        rows[index] = row;
        rows[index].sampled = true;
        rows[index].index = index;
        return true;
    }

    void end(const RowDescriptor* descriptors, uint32_t count) noexcept {
        for (uint32_t i = 0; i < kCount; ++i) requested[i] = (descriptors && i < count) ? descriptors[i] : RowDescriptor{};
        armed = false;
    }
};

} // namespace edvr::nofoot::optic_capture
