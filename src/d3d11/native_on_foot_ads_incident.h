#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {

// Frame-thread history for an ADS fault. It samples at 10 Hz and prints only on a fault, at most three histories a minute.
// Fields retain their own stamps: a fault before this tick's map runs must not present the previous map as current.
struct AdsIncidentSample {
    uint64_t ms = 0, revision = 0, contentAgeMs = 0, actionAgeUs = 0;
    uint64_t headSeq = 0, selectionEpoch = 0, configGeneration = 0, rootAgeMs = 0;
    uint32_t tick = 0, mapTick = 0, rowsFrame = 0, polls = 0, itemSerial = 0, press = 0;
    char item[64] = {};
    uint32_t clean = 0, dirty = 0, unsplit = 0, foreign = 0, blocked = 0, writes = 0, procs = 0, refusals = 0;
    // rootKnown/age describe the stamp read separately from rootPitch, not a proven pitch sample age.
    bool request = false, requestFresh = false, journalKnown = false, journal = false, veto = false, rowsKnown = false, residualKnown = false, headKnown = false, rootKnown = false;
    int state = 0;
    double weight = 0.0, rootPitch = 0.0;
    double head[2] = {}, asked[2] = {}, queued[2] = {}, applied[2] = {}, player[2] = {}, cancel[2] = {}, ahead[2] = {}, carried[2] = {}, residual[2] = {};
    double gain[2] = {}, gainEnds[2] = {};
};

struct AdsIncidentHistory {
    static constexpr size_t kCapacity = 16;
    static constexpr uint64_t kSampleMs = 100, kWindowMs = 60000;
    static constexpr uint32_t kDumpsPerWindow = 3;
    AdsIncidentSample samples[kCapacity] = {};
    size_t next = 0, count = 0;
    uint64_t lastSampleMs = 0, dumpMs[kDumpsPerWindow] = {};
    uint32_t dumps = 0, suppressed = 0;
    void clearSamples() noexcept { next = count = 0; lastSampleMs = 0; }
    bool push(const AdsIncidentSample& sample, bool force = false) noexcept {
        if (count && (at(count - 1).itemSerial != sample.itemSerial || at(count - 1).selectionEpoch != sample.selectionEpoch ||
                      at(count - 1).configGeneration != sample.configGeneration ||
                      at(count - 1).press != sample.press || std::memcmp(at(count - 1).item, sample.item, sizeof(sample.item)) != 0)) clearSamples();
        if (!force && count && sample.ms >= lastSampleMs && sample.ms - lastSampleMs < kSampleMs) return false;
        samples[next] = sample;
        next = (next + 1) % kCapacity;
        if (count < kCapacity) ++count;
        lastSampleMs = sample.ms;
        return true;
    }
    const AdsIncidentSample& at(size_t index) const noexcept { return samples[(next + kCapacity - count + index) % kCapacity]; }
    bool beginDump(uint64_t now) noexcept {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < dumps; ++i)
            if (now >= dumpMs[i] && now - dumpMs[i] < kWindowMs) dumpMs[kept++] = dumpMs[i];
        dumps = kept;
        if (dumps >= kDumpsPerWindow) { ++suppressed; return false; }
        dumpMs[dumps++] = now;
        return true;
    }
};

} // namespace nofoot
} // namespace edvr
