#pragma once

#include <cstdint>

namespace edvr {

struct NativeBenchmarkMetadata {
    uint32_t inputWidth[2]{};
    uint32_t inputHeight[2]{};
    uint32_t outputWidth[2]{};
    uint32_t outputHeight[2]{};
    uint32_t refreshMilliHz = 0;
    float gameFov[2][4]{};
    uint32_t treatments[2]{};
    uint64_t featureEpoch = 0;
    char runtime[64]{};
    char headset[64]{};
    char aaMode[32]{};
    char dlssMode[32]{};
    char build[48]{};
};

// CPU and GPU are separate observations. GPU completion is asynchronous and
// may be older than the CPU publication seen in the same monitor tick.
struct NativeBenchmarkObservation {
    uint64_t scope = 0;
    const NativeBenchmarkMetadata* metadata = nullptr;
    uint64_t cpuSequence = 0;
    uint64_t cpuAtMs = 0;
    double cpuMs = 0.0;
    bool cpuValid = false;
    uint64_t gpuSequence = 0;
    uint64_t gpuAtMs = 0;
    double gpuMs = 0.0;
    bool gpuValid = false;
};

struct NativeBenchmarkDistribution {
    uint64_t valid = 0;
    uint64_t stored = 0;
    uint64_t invalid = 0;
    uint64_t missing = 0;
    bool available = false;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

enum NativeBenchmarkAbortReason : uint32_t {
    kNativeBenchmarkCompleted = 0,
    kNativeBenchmarkScopeChanged = 1,
    kNativeBenchmarkOverflow = 2,
    kNativeBenchmarkClockReversed = 3,
    kNativeBenchmarkTransportLoss = 4,
};

struct NativeBenchmarkReport {
    uint64_t window = 0;
    uint64_t scope = 0;
    uint64_t warmupMs = 0;
    uint64_t sampleMs = 0;
    uint64_t drainMs = 0;
    uint64_t startedAtMs = 0;
    uint64_t sampleEndedAtMs = 0;
    uint64_t endedAtMs = 0;
    uint32_t abortReason = kNativeBenchmarkCompleted;
    bool complete = false;
    bool aborted = false;
    bool overflow = false;
    NativeBenchmarkMetadata metadata{};
    NativeBenchmarkDistribution cpu{};
    NativeBenchmarkDistribution gpu{};
};

// Recurring owner-thread window. observe() performs no allocation, sorting,
// logging or GPU synchronization. 16,384 entries cover a 30-second window at
// 240 Hz with room for delayed asynchronous completions.
class NativeBenchmarkCollector final {
public:
    static constexpr uint64_t kWarmupMs = 2000;
    static constexpr uint64_t kSampleMs = 30000;
    static constexpr uint64_t kDrainMs = 2000;
    static constexpr unsigned kCapacity = 16384;
    static constexpr unsigned kOrphanCapacity = 256;

    NativeBenchmarkCollector() noexcept = default;
    void observe(const NativeBenchmarkObservation& sample, uint64_t nowMs) noexcept;
    // Queue overwrite is a transport failure, not a zero-duration sample.
    // Count it in the affected stream's invalid coverage while sampling.
    void noteDropped(bool cpu, uint64_t count) noexcept;
    bool ready() const noexcept { return phase_ == Phase::Complete; }
    bool takeReport(NativeBenchmarkReport* out) noexcept;
    void reset() noexcept;
    uint64_t window() const noexcept { return window_; }
    bool warming() const noexcept { return phase_ == Phase::Warmup; }
    bool sampling() const noexcept { return phase_ == Phase::Sampling || phase_ == Phase::Drain; }

private:
    enum class Phase : uint8_t { Idle, Warmup, Sampling, Drain, Complete };
    struct Entry {
        uint64_t sequence = 0;
        uint64_t atMs = 0;
        bool used = false;
        bool admitted = false;
        bool cpuSeen = false;
        bool gpuSeen = false;
        bool cpuValid = false;
        bool gpuValid = false;
        double cpuMs = 0.0;
        double gpuMs = 0.0;
    };
    struct OrphanGpu {
        uint64_t sequence = 0;
        uint64_t atMs = 0;
        bool used = false;
        bool valid = false;
        double ms = 0.0;
    };

    uint64_t scope_ = 0;
    NativeBenchmarkMetadata metadata_{};
    uint64_t warmupStartMs_ = 0;
    uint64_t sampleStartMs_ = 0;
    uint64_t sampleEndMs_ = 0;
    uint64_t drainEndMs_ = 0;
    uint64_t lastNowMs_ = 0;
    uint64_t firstSequence_ = 0;
    uint64_t window_ = 1;
    Phase phase_ = Phase::Idle;
    uint32_t abortReason_ = kNativeBenchmarkCompleted;
    bool overflow_ = false;
    bool transportLost_ = false;
    uint64_t droppedCpu_ = 0;
    uint64_t droppedGpu_ = 0;
    Entry entries_[kCapacity]{};
    OrphanGpu orphans_[kOrphanCapacity]{};

    static bool validValue(double value) noexcept;
    static bool elapsed(uint64_t now, uint64_t then, uint64_t duration) noexcept;
    void clearEntries() noexcept;
    void begin(uint64_t scope, uint64_t nowMs,
               const NativeBenchmarkMetadata* metadata) noexcept;
    void startSampling(uint64_t nowMs) noexcept;
    Entry* find(uint64_t sequence, bool create) noexcept;
    OrphanGpu* findOrphan(uint64_t sequence, bool create) noexcept;
    void addCpu(const NativeBenchmarkObservation& sample) noexcept;
    void addGpu(const NativeBenchmarkObservation& sample) noexcept;
    void finish(uint64_t nowMs, uint32_t reason) noexcept;
    static NativeBenchmarkDistribution distribution(Entry* entries,
                                                     bool cpu) noexcept;
};

} // namespace edvr
