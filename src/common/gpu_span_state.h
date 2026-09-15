#pragma once

// CPU policy for asynchronous frame-associated GPU timestamps.
// All mutable access belongs to one verified immediate-context thread. The
// identities passed below must come from the adapter, not from cached guesses.
// A future pose-wait publisher uses a separate atomic mailbox, never this ring.
#include <array>
#include <cstdint>
#include <limits>

namespace edvr {
struct GpuSpanOwner {
    uintptr_t device = 0, context = 0;
    uint32_t thread = 0;
    bool immediate = true;
};
enum class GpuSpanPoll { Pending, Ready, Failed };
enum class GpuSpanReason {
    Valid, WrongOwner, Stopped, OldSequence, NoOpenFrame, Incomplete,
    BadPair, DriverFailure, Disjoint, ZeroFrequency, BadTimestamps,
    Stale, RingFull, CreateFailed
};
enum class GpuSpanSource { RenderToSubmit, ApplicationRender };
struct GpuSpanRawSample {
    bool timestampsReady = false, disjoint = false;
    uint64_t frequency = 0;
    // Legacy frame mode uses 0/1 for the outer interval and 2/3, 4/5 for
    // ordered eye intervals. Application mode uses four non-overlapping
    // segment pairs (0/1, 2/3, 4/5, 6/7) and sums those pairs.
    std::array<uint64_t, 8> ticks{};
};
struct GpuSpanDriver {
    virtual ~GpuSpanDriver() = default;
    // Failed creation cleans partial resources. Failed begin opened no scope.
    virtual bool create(unsigned slot) noexcept = 0;
    virtual bool begin(unsigned slot) noexcept = 0;
    virtual bool timestamp(unsigned slot, unsigned index) noexcept = 0;
    // A failed end makes closure uncertain: the instrument stops permanently.
    virtual bool end(unsigned slot) noexcept = 0;
    // One nonblocking poll. Ready requires the disjoint result AND every issued
    // timestamp (which can be a partial set for an invalid frame). A D3D11
    // adapter must use DONOTFLUSH and preserve S_FALSE as Pending.
    virtual GpuSpanPoll poll(unsigned slot, GpuSpanRawSample& raw) noexcept = 0;
    virtual void destroy(unsigned slot) noexcept = 0;
};
struct GpuSpanResult {
    uint64_t sequence = 0, sourceFrame = 0, completedAtMs = 0, ageMs = 0;
    double outerMs = 0, leftMs = 0, rightMs = 0;
    GpuSpanReason reason = GpuSpanReason::Incomplete;
    GpuSpanSource source = GpuSpanSource::RenderToSubmit;
};

class GpuSpanState final {
public:
    static constexpr unsigned kSlots = 8;
    static constexpr uint64_t kMaxAgeMs = 2000;
    using Results = std::array<GpuSpanResult, kSlots>;
private:
    enum class State { Free, Open, Pending, Failed };
    struct Slot {
        State state = State::Free;
        bool created = false;
        GpuSpanReason reason = GpuSpanReason::Valid;
        uint64_t sequence = 0, sourceFrame = 0, closedAtMs = 0;
        unsigned begun = 0, ended = 0, order[2]{}, orderCount = 0;
        int activeEye = -1;
        bool application = false, segmentOpen = false;
        unsigned segmentCount = 0;
    };
    GpuSpanDriver& driver_;
    const GpuSpanOwner owner_;
    std::array<Slot, kSlots> slots_{};
    int open_ = -1;
    uint64_t lastSequence_ = 0;
    bool failed_ = false, shut_ = false;

    bool identity(const GpuSpanOwner& o) const noexcept {
        return owner_.device && owner_.context && owner_.thread && owner_.immediate &&
            o.device == owner_.device && o.context == owner_.context &&
            o.thread == owner_.thread && o.immediate;
    }
    GpuSpanReason gate(const GpuSpanOwner& o) const noexcept {
        // Wrong-thread calls must not even read fields written by the owner.
        if (!identity(o)) return GpuSpanReason::WrongOwner;
        return failed_ || shut_ ? GpuSpanReason::Stopped : GpuSpanReason::Valid;
    }
    GpuSpanReason close(GpuSpanReason reason, uint64_t now) noexcept {
        const unsigned i = static_cast<unsigned>(open_);
        Slot& s = slots_[i];
        const bool stamp = driver_.timestamp(i, 1);
        const bool ended = driver_.end(i); // Always attempt, even if stamp failed.
        if (!stamp || !ended) reason = GpuSpanReason::DriverFailure;
        if (!ended) failed_ = true; // No later begin on an uncertain live scope.
        s.reason = reason;
        s.closedAtMs = now;
        s.state = State::Pending;
        open_ = -1;
        return reason;
    }
    GpuSpanReason closeApplication(GpuSpanReason reason, uint64_t now) noexcept {
        const unsigned i = static_cast<unsigned>(open_);
        Slot& s = slots_[i];
        if (s.segmentOpen) {
            const unsigned end = 2 * s.segmentCount - 1;
            const bool stamp = end < 8 && driver_.timestamp(i, end);
            s.segmentOpen = false;
            if (!stamp) reason = GpuSpanReason::DriverFailure;
        }
        const bool ended = driver_.end(i);
        if (!ended) { reason = GpuSpanReason::DriverFailure; failed_ = true; }
        s.reason = reason;
        s.closedAtMs = now;
        s.state = State::Pending;
        open_ = -1;
        return reason;
    }
    void retire(unsigned i, bool destroy) noexcept {
        Slot& s = slots_[i];
        const bool keep = s.created && !destroy;
        if (s.created && destroy) driver_.destroy(i);
        s = Slot{};
        s.created = keep;
    }
    static uint64_t age(uint64_t now, uint64_t then) noexcept {
        return now >= then ? now - then : 0;
    }
    static GpuSpanReason validate(const Slot& s, const GpuSpanRawSample& raw) noexcept {
        if (raw.disjoint) return GpuSpanReason::Disjoint;
        if (!raw.frequency) return GpuSpanReason::ZeroFrequency;
        const auto& t = raw.ticks;
        if (s.ended != 3 || s.orderCount != 2 || t[0] > t[1])
            return GpuSpanReason::BadTimestamps;
        for (unsigned eye = 0; eye < 2; ++eye) {
            const unsigned k = 2 + 2 * eye;
            if (t[k] > t[k + 1] || t[k] < t[0] || t[k + 1] > t[1])
                return GpuSpanReason::BadTimestamps;
        }
        if (t[3 + 2 * s.order[0]] > t[2 + 2 * s.order[1]])
            return GpuSpanReason::BadTimestamps;
        return GpuSpanReason::Valid;
    }
    static GpuSpanReason validateApplication(const Slot& s, const GpuSpanRawSample& raw,
                                             double& elapsed) noexcept {
        if (raw.disjoint) return GpuSpanReason::Disjoint;
        if (!raw.frequency || s.segmentCount < 3 || s.segmentCount > 4)
            return !raw.frequency ? GpuSpanReason::ZeroFrequency : GpuSpanReason::BadTimestamps;
        uint64_t ticks = 0, previousEnd = 0;
        for (unsigned n = 0; n < s.segmentCount; ++n) {
            const unsigned k = 2 * n;
            if (raw.ticks[k] > raw.ticks[k + 1] ||
                (n && previousEnd > raw.ticks[k]))
                return GpuSpanReason::BadTimestamps;
            const uint64_t width = raw.ticks[k + 1] - raw.ticks[k];
            if (ticks > (std::numeric_limits<uint64_t>::max)() - width)
                return GpuSpanReason::BadTimestamps;
            ticks += width;
            previousEnd = raw.ticks[k + 1];
        }
        elapsed = static_cast<double>(ticks) * 1000.0 /
            static_cast<double>(raw.frequency);
        return (elapsed >= 0.0) ? GpuSpanReason::Valid : GpuSpanReason::BadTimestamps;
    }
public:
    explicit GpuSpanState(GpuSpanDriver& driver, GpuSpanOwner owner) noexcept
        : driver_(driver), owner_(owner) {}
    GpuSpanState(const GpuSpanState&) = delete;
    GpuSpanState& operator=(const GpuSpanState&) = delete;
    // No destructor driver calls: its thread/device validity is unknown.

    GpuSpanReason beginFrame(uint64_t sequence, uint64_t sourceFrame,
                             uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (!sequence || sequence <= lastSequence_) return GpuSpanReason::OldSequence;
        lastSequence_ = sequence; // Includes skipped/failed measurements.
        if (open_ >= 0) {
            if (slots_[static_cast<unsigned>(open_)].application)
                closeApplication(GpuSpanReason::Incomplete, now);
            else
                close(GpuSpanReason::Incomplete, now);
        }
        if (failed_) return GpuSpanReason::DriverFailure;
        unsigned i = 0;
        while (i < kSlots && slots_[i].state != State::Free) ++i;
        if (i == kSlots) return GpuSpanReason::RingFull;
        Slot& s = slots_[i];
        if (!s.created) {
            if (!driver_.create(i)) return GpuSpanReason::CreateFailed;
            s.created = true;
        }
        s.sequence = sequence;
        s.sourceFrame = sourceFrame;
        if (!driver_.begin(i)) {
            s.state = State::Failed;
            s.reason = GpuSpanReason::DriverFailure;
            s.closedAtMs = now;
            return s.reason;
        }
        s.state = State::Open;
        open_ = static_cast<int>(i);
        if (!driver_.timestamp(i, 0)) return close(GpuSpanReason::DriverFailure, now);
        return GpuSpanReason::Valid;
    }
    // Application rendering mode brackets only producer GPU work. The
    // disjoint scope stays open while keyed transfers/runtime calls are out
    // of scope; each actual producer run is a separate timestamp pair.
    GpuSpanReason beginApplicationFrame(uint64_t sequence, uint64_t sourceFrame,
                                        uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (!sequence || sequence <= lastSequence_) return GpuSpanReason::OldSequence;
        lastSequence_ = sequence;
        if (open_ >= 0) {
            if (slots_[static_cast<unsigned>(open_)].application)
                closeApplication(GpuSpanReason::Incomplete, now);
            else
                close(GpuSpanReason::Incomplete, now);
        }
        if (failed_) return GpuSpanReason::DriverFailure;
        unsigned i = 0;
        while (i < kSlots && slots_[i].state != State::Free) ++i;
        if (i == kSlots) return GpuSpanReason::RingFull;
        Slot& s = slots_[i];
        if (!s.created) {
            if (!driver_.create(i)) return GpuSpanReason::CreateFailed;
            s.created = true;
        }
        s = Slot{};
        s.created = true;
        s.application = true;
        s.sequence = sequence;
        s.sourceFrame = sourceFrame;
        if (!driver_.begin(i)) {
            s.state = State::Failed;
            s.reason = GpuSpanReason::DriverFailure;
            s.closedAtMs = now;
            return s.reason;
        }
        s.state = State::Open;
        open_ = static_cast<int>(i);
        s.segmentCount = 1;
        s.segmentOpen = driver_.timestamp(i, 0);
        if (!s.segmentOpen) return closeApplication(GpuSpanReason::DriverFailure, now);
        return GpuSpanReason::Valid;
    }
    GpuSpanReason endApplicationSegment(uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (open_ < 0) return GpuSpanReason::NoOpenFrame;
        Slot& s = slots_[static_cast<unsigned>(open_)];
        if (!s.application || !s.segmentOpen || !s.segmentCount || s.segmentCount > 4)
            return closeApplication(GpuSpanReason::Incomplete, now);
        const unsigned marker = 2 * s.segmentCount - 1;
        if (!driver_.timestamp(static_cast<unsigned>(open_), marker))
            return closeApplication(GpuSpanReason::DriverFailure, now);
        s.segmentOpen = false;
        return GpuSpanReason::Valid;
    }
    GpuSpanReason resumeApplicationSegment(uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (open_ < 0) return GpuSpanReason::NoOpenFrame;
        Slot& s = slots_[static_cast<unsigned>(open_)];
        if (!s.application || s.segmentOpen || s.segmentCount >= 4)
            return s.application ? GpuSpanReason::RingFull : GpuSpanReason::Incomplete;
        const unsigned marker = 2 * s.segmentCount;
        if (!driver_.timestamp(static_cast<unsigned>(open_), marker))
            return closeApplication(GpuSpanReason::DriverFailure, now);
        ++s.segmentCount;
        s.segmentOpen = true;
        return GpuSpanReason::Valid;
    }
    GpuSpanReason finishApplicationFrame(uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (open_ < 0) return GpuSpanReason::NoOpenFrame;
        Slot& s = slots_[static_cast<unsigned>(open_)];
        if (!s.application) return GpuSpanReason::Incomplete;
        if (s.segmentOpen || !s.segmentCount || s.segmentCount < 3)
            return closeApplication(GpuSpanReason::Incomplete, now);
        const bool ended = driver_.end(static_cast<unsigned>(open_));
        if (!ended) { failed_ = true; s.reason = GpuSpanReason::DriverFailure; }
        s.reason = ended ? GpuSpanReason::Valid : GpuSpanReason::DriverFailure;
        s.closedAtMs = now;
        s.state = State::Pending;
        open_ = -1;
        return s.reason;
    }
    GpuSpanReason beginEye(unsigned eye, uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (open_ < 0) return GpuSpanReason::NoOpenFrame;
        Slot& s = slots_[static_cast<unsigned>(open_)];
        if (eye > 1 || s.activeEye >= 0 || (s.begun & (1u << eye)))
            return close(GpuSpanReason::BadPair, now);
        if (!driver_.timestamp(static_cast<unsigned>(open_), 2 + 2 * eye))
            return close(GpuSpanReason::DriverFailure, now);
        s.begun |= 1u << eye;
        s.activeEye = static_cast<int>(eye);
        s.order[s.orderCount++] = eye;
        return GpuSpanReason::Valid;
    }
    GpuSpanReason endEye(unsigned eye, uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (open_ < 0) return GpuSpanReason::NoOpenFrame;
        Slot& s = slots_[static_cast<unsigned>(open_)];
        if (eye > 1 || s.activeEye != static_cast<int>(eye))
            return close(GpuSpanReason::BadPair, now);
        if (!driver_.timestamp(static_cast<unsigned>(open_), 3 + 2 * eye))
            return close(GpuSpanReason::DriverFailure, now);
        s.ended |= 1u << eye;
        s.activeEye = -1;
        return GpuSpanReason::Valid;
    }
    // Eye intervals cover the measured submit path. The shipping controller
    // includes runtime Submit plus post-submit copies and labels it accordingly;
    // it must not advertise those intervals as EDVR-only cost.
    GpuSpanReason finishFrame(uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (open_ < 0) return GpuSpanReason::NoOpenFrame;
        const Slot& s = slots_[static_cast<unsigned>(open_)];
        return close(s.ended == 3 && s.activeEye < 0 ? GpuSpanReason::Valid :
                     GpuSpanReason::Incomplete, now);
    }
    GpuSpanReason invalidateFrame(uint64_t now, const GpuSpanOwner& owner) noexcept {
        const auto checked = gate(owner);
        if (checked != GpuSpanReason::Valid) return checked;
        if (open_ < 0) return GpuSpanReason::NoOpenFrame;
        return slots_[static_cast<unsigned>(open_)].application ?
            closeApplication(GpuSpanReason::Incomplete, now) :
            close(GpuSpanReason::Incomplete, now);
    }
    unsigned poll(uint64_t now, const GpuSpanOwner& owner, Results& out) noexcept {
        if (!identity(owner)) return 0;
        if (shut_) return 0;
        unsigned count = 0;
        for (unsigned i = 0; i < kSlots; ++i) {
            Slot& s = slots_[i];
            if (s.state != State::Pending && s.state != State::Failed) continue;
            const uint64_t elapsed = age(now, s.closedAtMs);
            GpuSpanReason reason = s.reason;
            GpuSpanRawSample raw{};
            if (failed_ || s.state == State::Failed) reason = GpuSpanReason::DriverFailure;
            else if (elapsed > kMaxAgeMs) reason = GpuSpanReason::Stale;
            else {
                const auto status = driver_.poll(i, raw);
                if (status == GpuSpanPoll::Pending ||
                    (status == GpuSpanPoll::Ready && !raw.timestampsReady)) continue;
                if (status == GpuSpanPoll::Failed) reason = GpuSpanReason::DriverFailure;
                else if (reason == GpuSpanReason::Valid && !s.application)
                    reason = validate(s, raw);
            }
            GpuSpanResult& result = out[count++];
            result = GpuSpanResult{}; // Invalid results never retain old values.
            result.sequence = s.sequence;
            result.sourceFrame = s.sourceFrame;
            result.completedAtMs = now;
            result.ageMs = elapsed;
            if (s.application) {
                result.source = GpuSpanSource::ApplicationRender;
                if (reason == GpuSpanReason::Valid)
                    reason = validateApplication(s, raw, result.outerMs);
            } else if (reason == GpuSpanReason::Valid) {
                const double scale = 1000.0 / static_cast<double>(raw.frequency);
                const auto& t = raw.ticks;
                result.outerMs = static_cast<double>(t[1] - t[0]) * scale;
                result.leftMs = static_cast<double>(t[3] - t[2]) * scale;
                result.rightMs = static_cast<double>(t[5] - t[4]) * scale;
            }
            result.reason = reason;
            retire(i, reason != GpuSpanReason::Valid && reason != GpuSpanReason::BadPair &&
                      reason != GpuSpanReason::Incomplete);
        }
        return count;
    }
    // Discards outstanding samples and releases every created slot, including
    // after end failure. A failed end is not retried on an uncertain scope.
    bool shutdown(uint64_t now, const GpuSpanOwner& owner) noexcept {
        if (!identity(owner)) return false;
        if (shut_) return false;
        if (open_ >= 0) {
            if (slots_[static_cast<unsigned>(open_)].application)
                closeApplication(GpuSpanReason::Incomplete, now);
            else
                close(GpuSpanReason::Incomplete, now);
        }
        for (unsigned i = 0; i < kSlots; ++i) retire(i, true);
        shut_ = true;
        return true;
    }
};
} // namespace edvr
