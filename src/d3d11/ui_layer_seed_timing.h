#pragma once

// Diagnostic subprices of an HDR seed, never inputs to the route totals.
// All seeds of seq % 32 == 0 are attempted; a bounded pool drops pairs under
// pressure. Borrowing the existing application frame adds exactly four
// timestamps to a complete sample and never creates a disjoint scope.
#include "gpu_timing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace edvr {

struct UiHdrSeedTimingMeta {
    uint64_t seq = 0;
    uint32_t reason = 0, sourceW = 0, sourceH = 0, width = 0, height = 0;
    int eye = -1;
    uint8_t stencilMask = 0, passes = 0;
    bool needsDepth = false;
};

class UiHdrSeedGpuProbe {
public:
    static constexpr unsigned kSlots = 32, kSamples = 512, kClasses = 16;
    static constexpr uint64_t kStride = 32, kExpireMs = 2000;
    using Token = uint64_t;
    enum class Cancel { Explicit, RecordingFailed, ExecutionFailed };
    struct Clock {
        void* user = nullptr;
        bool (*counter)(void*, int64_t&) = nullptr;
        uint64_t (*millis)(void*) = nullptr;
        double ticksPerMs = 0;
    };
    struct Health {
        uint64_t seen = 0, disabled = 0, notSelected = 0, selected = 0, busy = 0;
        uint64_t copyBeginFailed = 0, copyEndFailed = 0, workBeginFailed = 0, workEndFailed = 0;
        uint64_t copyStarted = 0, workStarted = 0, paired = 0, invalidCopy = 0, invalidWork = 0;
        uint64_t recordingFailed = 0, executionFailed = 0, cancelled = 0, expired = 0;
        uint64_t resetDropped = 0, lateWindow = 0, recordUnavailable = 0, classOverflow = 0;
    };
private:
    enum class Phase { Empty, CopyOpen, Recording, WorkOpen, Pending };
    struct Slot {
        GpuTimer copy, work;
        Phase phase = Phase::Empty;
        Token token = 0;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* owner = nullptr;
        UiHdrSeedTimingMeta meta;
        uint64_t startedMs = 0, epoch = 0;
        int64_t recordStart = 0;
        bool recordClock = false, copyReady = false, workReady = false;
        double copyMs = 0, workMs = 0, recordMs = -1;
    };
    struct Pair { double copy = 0, work = 0, record = -1; };
    struct Stats {
        std::array<Pair, kSamples> samples{};
        uint64_t n = 0, recordN = 0;
        double copySum = 0, workSum = 0, recordSum = 0;
        uint32_t retained = 0, rng = 0x9e3779b9u;
        void add(const Pair& p) {
            ++n; copySum += p.copy; workSum += p.work;
            if (p.record >= 0) { ++recordN; recordSum += p.record; }
            uint64_t at = retained;
            if (retained < kSamples) ++retained;
            else { rng = rng * 1664525u + 1013904223u; at = (uint64_t(rng) * n) >> 32; }
            if (at < kSamples) samples[size_t(at)] = p;
        }
    };
    struct Classification { bool used = false; UiHdrSeedTimingMeta meta; Stats stats; };
    std::array<Slot, kSlots> slots_{};
    std::array<Classification, kClasses> classes_{};
    Stats all_;
    Health health_;
    Clock clock_;
    uint64_t generation_ = 0, epoch_ = 1;
    unsigned cursor_ = 0, pending_ = 0;

    uint64_t now() const { return clock_.millis ? clock_.millis(clock_.user) : GetTickCount64(); }
    bool counter(int64_t& value) {
        if (clock_.counter) return clock_.counter(clock_.user, value);
        LARGE_INTEGER q;
        if (!QueryPerformanceCounter(&q)) return false;
        value = q.QuadPart;
        if (!(clock_.ticksPerMs > 0)) {
            LARGE_INTEGER f;
            if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return false;
            clock_.ticksPerMs = double(f.QuadPart) / 1000.0;
        }
        return true;
    }
    Slot* find(ID3D11DeviceContext* ctx, Token token) {
        if (!token || !ctx || !gpuTimingOwns(ctx)) return nullptr;
        const unsigned index = unsigned(token & 0xffu);
        if (!index || index > kSlots) return nullptr;
        Slot& s = slots_[index - 1];
        return s.phase != Phase::Empty && s.token == token && s.owner == ctx ? &s : nullptr;
    }
    void retire(Slot& s, ID3D11DeviceContext* ctx, bool discard) {
        if (discard) { s.copy.reset(ctx); s.work.reset(ctx); }
        if (s.phase != Phase::Empty) --pending_;
        s.phase = Phase::Empty; s.token = 0; s.owner = nullptr; s.device = nullptr;
    }
    static bool sameClass(const UiHdrSeedTimingMeta& a, const UiHdrSeedTimingMeta& b) {
        return a.reason == b.reason && a.stencilMask == b.stencilMask && a.passes == b.passes &&
            a.needsDepth == b.needsDepth && a.sourceW == b.sourceW && a.sourceH == b.sourceH &&
            a.width == b.width && a.height == b.height;
    }
    void accept(const Slot& s) {
        ++health_.paired;
        if (s.epoch != epoch_) ++health_.lateWindow;
        if (s.recordMs < 0) ++health_.recordUnavailable;
        const Pair pair{s.copyMs, s.workMs, s.recordMs};
        all_.add(pair);
        for (auto& c : classes_) {
            if (!c.used) { c.used = true; c.meta = s.meta; }
            if (sameClass(c.meta, s.meta)) { c.stats.add(pair); return; }
        }
        ++health_.classOverflow;
    }
    static double percentile(std::array<double, kSamples>& values, unsigned n, double p) {
        if (!n) return 0;
        std::sort(values.begin(), values.begin() + n);
        const double at = (n - 1) * p;
        const unsigned lo = unsigned(at), hi = (std::min)(lo + 1, n - 1);
        return values[lo] + (values[hi] - values[lo]) * (at - lo);
    }
    static void metric(char* text, size_t size, const Stats& s, unsigned field) {
        std::array<double, kSamples> values{}; unsigned n = 0;
        for (unsigned i = 0; i < s.retained; ++i) {
            const Pair& p = s.samples[i];
            const double value = field == 0 ? p.copy : field == 1 ? p.work : p.record;
            if (value >= 0) values[n++] = value;
        }
        const uint64_t count = field == 2 ? s.recordN : s.n;
        if (!count || !n) { std::snprintf(text, size, "- (n=%llu retained=%u)",
                              static_cast<unsigned long long>(count), n); return; }
        const double sum = field == 0 ? s.copySum : field == 1 ? s.workSum : s.recordSum;
        const double median = percentile(values, n, .5), p95 = percentile(values, n, .95);
        std::snprintf(text, size, "%.4f/%.4f/%.4f (n=%llu retained=%u)", sum / double(count),
                      median, p95, static_cast<unsigned long long>(count), n);
    }
    template<class Emit>
    static void prices(const char* prefix, const Stats& s, Emit& emit) {
        char copy[112], work[112], record[112], text[640];
        metric(copy, sizeof(copy), s, 0); metric(work, sizeof(work), s, 1); metric(record, sizeof(record), s, 2);
        std::snprintf(text, sizeof(text), "%s copy_gpu=%s execute_gpu=%s record_wall_cpu=%s; "
                      "ms_per_seed=mean/median/p95", prefix, copy, work, record);
        emit(text);
    }
public:
    UiHdrSeedGpuProbe() = default;
    explicit UiHdrSeedGpuProbe(const Clock& clock) : clock_(clock) {}
    const Health& health() const { return health_; }
    uint64_t validSamples() const { return all_.n; }
    unsigned pending() const { return pending_; }

    Token beginCopy(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool armed,
                    const UiHdrSeedTimingMeta& meta) {
        ++health_.seen;
        if (!armed) { ++health_.disabled; return 0; }
        if (!meta.seq || meta.seq % kStride || meta.eye < 0 || meta.eye > 1 || !dev || !ctx) {
            ++health_.notSelected; return 0;
        }
        ++health_.selected;
        Slot* chosen = nullptr; unsigned index = 0;
        for (unsigned i = 0; i < kSlots; ++i) {
            index = (cursor_ + i) % kSlots;
            if (slots_[index].phase == Phase::Empty) { chosen = &slots_[index]; break; }
        }
        if (!chosen) { ++health_.busy; return 0; }
        Slot& s = *chosen;
        if (!s.copy.beginBorrowedFrame(dev, ctx)) { ++health_.copyBeginFailed; return 0; }
        ++health_.copyStarted;
        ++pending_;
        cursor_ = (index + 1) % kSlots;
        s.phase = Phase::CopyOpen; s.meta = meta; s.device = dev; s.owner = ctx;
        s.token = (++generation_ << 8) | (index + 1); s.epoch = epoch_; s.startedMs = now();
        s.copyReady = s.workReady = s.recordClock = false; s.copyMs = s.workMs = 0; s.recordMs = -1;
        return s.token;
    }
    bool endCopy(ID3D11DeviceContext* ctx, Token token) {
        Slot* s = find(ctx, token);
        if (!s || s->phase != Phase::CopyOpen) return false;
        if (!s->copy.end(ctx)) { ++health_.copyEndFailed; retire(*s, ctx, true); return false; }
        s->phase = Phase::Recording; s->recordClock = counter(s->recordStart);
        return true;
    }
    bool beginWork(ID3D11DeviceContext* ctx, Token token) {
        Slot* s = find(ctx, token);
        if (!s || s->phase != Phase::Recording) return false;
        int64_t end = 0;
        if (s->recordClock && counter(end) && end >= s->recordStart && clock_.ticksPerMs > 0)
            s->recordMs = double(end - s->recordStart) / clock_.ticksPerMs;
        if (!s->work.beginBorrowedFrame(s->device, ctx)) {
            ++health_.workBeginFailed; retire(*s, ctx, true); return false;
        }
        ++health_.workStarted; s->phase = Phase::WorkOpen;
        return true;
    }
    bool endWork(ID3D11DeviceContext* ctx, Token token, bool success = true) {
        Slot* s = find(ctx, token);
        if (!s || s->phase != Phase::WorkOpen) return false;
        if (!success) { cancel(ctx, token, Cancel::ExecutionFailed); return false; }
        if (!s->work.end(ctx)) { ++health_.workEndFailed; retire(*s, ctx, true); return false; }
        s->phase = Phase::Pending;
        return true;
    }
    void cancel(ID3D11DeviceContext* ctx, Token token, Cancel reason = Cancel::Explicit) {
        Slot* s = find(ctx, token); if (!s) return;
        if (reason == Cancel::RecordingFailed) ++health_.recordingFailed;
        else if (reason == Cancel::ExecutionFailed) ++health_.executionFailed;
        else ++health_.cancelled;
        retire(*s, ctx, true);
    }
    void poll(ID3D11DeviceContext* ctx) {
        if (!pending_) return;  // steady diagnostics-off: no clock, owner lookup or ring scan
        if (!ctx || !gpuTimingOwns(ctx)) return;
        const uint64_t time = now();
        for (auto& s : slots_) {
            if (s.phase == Phase::Empty || s.owner != ctx) continue;
            if (time >= s.startedMs && time - s.startedMs >= kExpireMs) {
                ++health_.expired; retire(s, ctx, true); continue;
            }
            if (s.phase != Phase::Pending) continue;
            if (!s.copyReady) {
                const auto r = s.copy.poll(ctx, s.copyMs);
                if (r == GpuTimerPoll::Invalid) { ++health_.invalidCopy; retire(s, ctx, true); continue; }
                s.copyReady = r == GpuTimerPoll::Ready;
            }
            if (!s.workReady) {
                const auto r = s.work.poll(ctx, s.workMs);
                if (r == GpuTimerPoll::Invalid) { ++health_.invalidWork; retire(s, ctx, true); continue; }
                s.workReady = r == GpuTimerPoll::Ready;
            }
            if (s.copyReady && s.workReady) {
                if (std::isfinite(s.copyMs) && std::isfinite(s.workMs) && s.copyMs >= 0 && s.workMs >= 0)
                    accept(s);
                else ++health_.invalidWork;
                retire(s, ctx, false);
            }
        }
    }
    bool reset(ID3D11DeviceContext* ctx = nullptr) {
        if (ctx && !gpuTimingOwns(ctx)) return false;
        for (const auto& s : slots_) if (s.phase != Phase::Empty && ctx && s.owner != ctx) return false;
        for (auto& s : slots_) {
            if (s.phase != Phase::Empty) ++health_.resetDropped;
            retire(s, ctx, true);
        }
        return true;
    }
    template<class Emit>
    void report(bool diagnosticsCurrent, Emit&& emit) {
        char text[1000];
        std::snprintf(text, sizeof(text), "ui quality: HDR seed subprice: diagnostics_current=%u "
                      "stride=32 pool=32 sample=all_seeds_of_selected_stereo_frame "
                      "markers_per_complete_seed=4 borrowed_application_scope=1 subset_of_HDRseed=1 "
                      "not_additive=1 copy=CopyResource execute=ExecuteCommandList "
                      "record_wall_cpu=deferred_record_and_Finish omitted=resource_prep_and_state_restore "
                      "retained_samples_may_predate_toggle=1 pending=%u", diagnosticsCurrent ? 1u : 0u, pending());
        emit(text);
        const auto u = [](uint64_t n) { return static_cast<unsigned long long>(n); };
        std::snprintf(text, sizeof(text), "ui quality: HDR seed subprice health: seen=%llu disabled=%llu "
                      "not_selected=%llu selected=%llu busy=%llu copy_started=%llu execute_started=%llu "
                      "paired=%llu copy_begin_failed=%llu copy_end_failed=%llu execute_begin_failed=%llu "
                      "execute_end_failed=%llu invalid_copy=%llu invalid_execute=%llu recording_failed=%llu "
                      "execution_failed=%llu cancelled=%llu expired=%llu reset_dropped=%llu "
                      "late_window_pairs=%llu record_clock_unavailable=%llu classification_overflow=%llu",
                      u(health_.seen), u(health_.disabled), u(health_.notSelected), u(health_.selected),
                      u(health_.busy), u(health_.copyStarted), u(health_.workStarted), u(health_.paired),
                      u(health_.copyBeginFailed), u(health_.copyEndFailed), u(health_.workBeginFailed),
                      u(health_.workEndFailed), u(health_.invalidCopy), u(health_.invalidWork),
                      u(health_.recordingFailed), u(health_.executionFailed), u(health_.cancelled),
                      u(health_.expired), u(health_.resetDropped), u(health_.lateWindow),
                      u(health_.recordUnavailable), u(health_.classOverflow));
        emit(text);
        prices("ui quality: HDR seed subprice totals:", all_, emit);
        for (const auto& c : classes_) if (c.used) {
            const auto& m = c.meta; char prefix[224];
            std::snprintf(prefix, sizeof(prefix), "ui quality: HDR seed subprice class: reason=0x%x "
                          "need_depth=%u stencil_mask=0x%02x passes=%u source=%ux%u target=%ux%u",
                          m.reason, m.needsDepth ? 1u : 0u, unsigned(m.stencilMask), unsigned(m.passes),
                          m.sourceW, m.sourceH, m.width, m.height);
            prices(prefix, c.stats, emit);
        }
        health_ = Health{};
        if (all_.n) all_ = Stats{};
        for (auto& c : classes_) if (c.used) c = Classification{};
        ++epoch_;
    }
};

} // namespace edvr
