#include "native_benchmark_collector.h"

#include <algorithm>
#include <cmath>

namespace edvr {
namespace {
double percentileSorted(const double* values, uint64_t count, unsigned percent) noexcept {
    if (!values || !count) return 0.0;
    const uint64_t rank = (count * percent + 99u) / 100u;
    return values[rank ? rank - 1 : 0];
}
}

bool NativeBenchmarkCollector::validValue(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 600000.0;
}

bool NativeBenchmarkCollector::elapsed(uint64_t now, uint64_t then,
                                       uint64_t duration) noexcept {
    return now >= then && now - then >= duration;
}

void NativeBenchmarkCollector::noteDropped(bool cpu, uint64_t count) noexcept {
    if (phase_ != Phase::Sampling && phase_ != Phase::Drain) return;
    // A transport loss has no frame identity to pair. Retain the count for
    // coverage, and mark the report aborted rather than presenting a partial
    // queue as a completed benchmark.
    if (count) transportLost_ = true;
    if (cpu) droppedCpu_ += count;
    else droppedGpu_ += count;
}

void NativeBenchmarkCollector::clearEntries() noexcept {
    for (auto& entry : entries_) entry = {};
    for (auto& orphan : orphans_) orphan = {};
}

void NativeBenchmarkCollector::reset() noexcept {
    scope_ = 0;
    metadata_ = {};
    warmupStartMs_ = sampleStartMs_ = sampleEndMs_ = drainEndMs_ = lastNowMs_ = 0;
    firstSequence_ = 0;
    phase_ = Phase::Idle;
    abortReason_ = kNativeBenchmarkCompleted;
    overflow_ = false;
    transportLost_ = false;
    droppedCpu_ = droppedGpu_ = 0;
    clearEntries();
}

void NativeBenchmarkCollector::begin(uint64_t scope, uint64_t nowMs,
                                     const NativeBenchmarkMetadata* metadata) noexcept {
    scope_ = scope;
    metadata_ = metadata ? *metadata : NativeBenchmarkMetadata{};
    warmupStartMs_ = nowMs;
    sampleStartMs_ = sampleEndMs_ = drainEndMs_ = lastNowMs_ = 0;
    firstSequence_ = 0;
    phase_ = Phase::Warmup;
    abortReason_ = kNativeBenchmarkCompleted;
    overflow_ = false;
    transportLost_ = false;
    droppedCpu_ = droppedGpu_ = 0;
    clearEntries();
}

void NativeBenchmarkCollector::startSampling(uint64_t nowMs) noexcept {
    sampleStartMs_ = nowMs;
    sampleEndMs_ = nowMs + kSampleMs;
    drainEndMs_ = sampleEndMs_ + kDrainMs;
    lastNowMs_ = nowMs;
    // CPU publication time owns window membership. A sequence floor inferred
    // from the first completion is unsafe when the queue is delayed or
    // out-of-order, and a metadata-only tick has no sequence at all.
    firstSequence_ = 1;
    phase_ = Phase::Sampling;
    abortReason_ = kNativeBenchmarkCompleted;
    overflow_ = false;
    droppedCpu_ = droppedGpu_ = 0;
    clearEntries();
}

NativeBenchmarkCollector::Entry* NativeBenchmarkCollector::find(uint64_t sequence,
                                                                 bool create) noexcept {
    if (!sequence) return nullptr;
    const unsigned start = static_cast<unsigned>(sequence % kCapacity);
    for (unsigned probe = 0; probe < kCapacity; ++probe) {
        Entry& entry = entries_[(start + probe) % kCapacity];
        if (entry.used) {
            if (entry.sequence == sequence) return &entry;
            continue;
        }
        if (!create) return nullptr;
        entry = {};
        entry.sequence = sequence;
        entry.used = true;
        return &entry;
    }
    overflow_ = true;
    return nullptr;
}

NativeBenchmarkCollector::OrphanGpu* NativeBenchmarkCollector::findOrphan(
    uint64_t sequence, bool create) noexcept {
    if (!sequence) return nullptr;
    const unsigned start = static_cast<unsigned>(sequence % kOrphanCapacity);
    OrphanGpu* freeSlot = nullptr;
    for (unsigned probe = 0; probe < kOrphanCapacity; ++probe) {
        OrphanGpu& orphan = orphans_[(start + probe) % kOrphanCapacity];
        if (orphan.used) {
            if (orphan.sequence == sequence) return &orphan;
            continue;
        }
        if (!freeSlot) freeSlot = &orphan;
    }
    if (create && freeSlot) {
        *freeSlot = {};
        freeSlot->sequence = sequence;
        freeSlot->used = true;
        return freeSlot;
    }
    if (!create) return nullptr;
    overflow_ = true;
    return nullptr;
}

void NativeBenchmarkCollector::addCpu(const NativeBenchmarkObservation& sample) noexcept {
    if (!sample.cpuSequence || sample.cpuSequence < firstSequence_ ||
        sample.cpuAtMs < sampleStartMs_ || sample.cpuAtMs >= sampleEndMs_) return;
    Entry* entry = find(sample.cpuSequence, true);
    if (!entry || entry->cpuSeen) return;
    entry->cpuSeen = true;
    entry->atMs = sample.cpuAtMs;
    entry->cpuValid = sample.cpuValid && validValue(sample.cpuMs);
    entry->cpuMs = entry->cpuValid ? sample.cpuMs : 0.0;
    entry->admitted = true;
    if (OrphanGpu* orphan = findOrphan(sample.cpuSequence, false)) {
        entry->gpuSeen = true;
        entry->gpuValid = orphan->valid;
        entry->gpuMs = orphan->valid ? orphan->ms : 0.0;
        *orphan = {};
    }
}

void NativeBenchmarkCollector::addGpu(const NativeBenchmarkObservation& sample) noexcept {
    if (!sample.gpuSequence || sample.gpuSequence < firstSequence_ || sample.gpuAtMs > drainEndMs_) return;
    Entry* entry = find(sample.gpuSequence, false);
    if (!entry) {
        if (sample.gpuAtMs < sampleStartMs_ || sample.gpuAtMs > drainEndMs_) return;
        OrphanGpu* orphan = findOrphan(sample.gpuSequence, true);
        if (!orphan || orphan->atMs) return;
        orphan->atMs = sample.gpuAtMs;
        orphan->valid = sample.gpuValid && validValue(sample.gpuMs);
        orphan->ms = orphan->valid ? sample.gpuMs : 0.0;
        return;
    }
    if (!entry->admitted || entry->gpuSeen) return;
    entry->gpuSeen = true;
    if (!entry->atMs) entry->atMs = sample.gpuAtMs;
    entry->gpuValid = sample.gpuValid && validValue(sample.gpuMs);
    entry->gpuMs = entry->gpuValid ? sample.gpuMs : 0.0;
}

void NativeBenchmarkCollector::finish(uint64_t nowMs, uint32_t reason) noexcept {
    if (reason == kNativeBenchmarkCompleted && transportLost_)
        reason = kNativeBenchmarkTransportLoss;
    abortReason_ = reason;
    overflow_ = overflow_ || reason == kNativeBenchmarkOverflow;
    lastNowMs_ = nowMs;
    phase_ = Phase::Complete;
}

void NativeBenchmarkCollector::observe(const NativeBenchmarkObservation& sample,
                                       uint64_t nowMs) noexcept {
    if (phase_ == Phase::Complete) return;
    if (!sample.scope) {
        if (scope_ == 0) return;
        if (nowMs < lastNowMs_) {
            if (phase_ == Phase::Sampling || phase_ == Phase::Drain)
                finish(nowMs, kNativeBenchmarkClockReversed);
            return;
        }
        lastNowMs_ = nowMs;
        if (phase_ == Phase::Warmup && elapsed(nowMs, warmupStartMs_, kWarmupMs))
            startSampling(nowMs);
        if (phase_ == Phase::Sampling && elapsed(nowMs, sampleEndMs_, 0))
            phase_ = Phase::Drain;
        if (phase_ == Phase::Drain && elapsed(nowMs, drainEndMs_, 0))
            finish(nowMs, kNativeBenchmarkCompleted);
        return;
    }
    if (scope_ != sample.scope) {
        if (phase_ == Phase::Sampling || phase_ == Phase::Drain) {
            finish(nowMs, kNativeBenchmarkScopeChanged);
            return;
        }
        begin(sample.scope, nowMs, sample.metadata);
    }
    if (nowMs < lastNowMs_) {
        if (phase_ == Phase::Sampling || phase_ == Phase::Drain)
            finish(nowMs, kNativeBenchmarkClockReversed);
        return;
    }
    lastNowMs_ = nowMs;
    if (phase_ == Phase::Warmup) {
        if (!elapsed(nowMs, warmupStartMs_, kWarmupMs)) return;
        startSampling(nowMs);
    }
    if (phase_ == Phase::Sampling) {
        addCpu(sample);
        addGpu(sample);
        if (overflow_) {
            finish(nowMs, kNativeBenchmarkOverflow);
            return;
        }
        if (elapsed(nowMs, sampleEndMs_, 0)) phase_ = Phase::Drain;
    } else if (phase_ == Phase::Drain) {
        addCpu(sample);
        addGpu(sample);
        if (overflow_) {
            finish(nowMs, kNativeBenchmarkOverflow);
            return;
        }
    }
    if (phase_ == Phase::Drain && !sample.cpuSequence && !sample.gpuSequence &&
        elapsed(nowMs, drainEndMs_, 0))
        finish(nowMs, kNativeBenchmarkCompleted);
}

NativeBenchmarkDistribution NativeBenchmarkCollector::distribution(Entry* entries,
                                                                    bool cpu) noexcept {
    NativeBenchmarkDistribution result{};
    if (!entries) return result;
    static double values[kCapacity];
    uint64_t count = 0;
    for (unsigned i = 0; i < kCapacity; ++i) {
        const Entry& entry = entries[i];
        if (!entry.admitted) continue;
        const bool seen = cpu ? entry.cpuSeen : entry.gpuSeen;
        const bool valid = cpu ? entry.cpuValid : entry.gpuValid;
        if (seen) {
            if (valid) {
                ++result.valid;
                if (count < kCapacity) values[count++] = cpu ? entry.cpuMs : entry.gpuMs;
            } else ++result.invalid;
        }
        if (entry.used && !seen) ++result.missing;
    }
    result.stored = count;
    result.available = count != 0;
    if (count) {
        std::sort(values, values + count);
        result.p50 = percentileSorted(values, count, 50);
        result.p95 = percentileSorted(values, count, 95);
        result.p99 = percentileSorted(values, count, 99);
    }
    return result;
}

bool NativeBenchmarkCollector::takeReport(NativeBenchmarkReport* out) noexcept {
    if (!out || phase_ != Phase::Complete) return false;
    *out = {};
    out->window = window_++;
    out->scope = scope_;
    out->startedAtMs = sampleStartMs_;
    out->sampleEndedAtMs = (std::max)(sampleStartMs_, (std::min)(sampleEndMs_,lastNowMs_));
    out->drainMs = lastNowMs_ > out->sampleEndedAtMs ? lastNowMs_ - out->sampleEndedAtMs : 0;
    out->endedAtMs = lastNowMs_;
    out->abortReason = abortReason_;
    out->complete = true;
    out->aborted = abortReason_ != kNativeBenchmarkCompleted;
    out->overflow = overflow_;
    out->metadata = metadata_;
    out->cpu = distribution(entries_, true);
    out->gpu = distribution(entries_, false);
    out->cpu.invalid += droppedCpu_;
    out->gpu.invalid += droppedGpu_;
    reset();
    return true;
}

} // namespace edvr
