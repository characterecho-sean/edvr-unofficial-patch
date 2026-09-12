#pragma once
#include "gpu_span_state.h"
#include <array>
#include <atomic>

namespace edvr {

enum class DisjointStatus { Pending, Ready, Failed };
enum class DisjointReason { None, InvalidLease, WrongOwner, Incomplete,
                            DriverFailure, Disjoint, ZeroFrequency, Stale };
struct DisjointResult {
    DisjointStatus status = DisjointStatus::Pending;
    uint64_t frequency = 0;
    bool disjoint = false;
    DisjointReason reason = DisjointReason::None;
};
struct DisjointBackend {
    virtual ~DisjointBackend() = default;
    // Native implementations derive thread from GetCurrentThreadId, and
    // validate/retain a canonical immediate context and its device at bind.
    virtual GpuSpanOwner currentOwner() const noexcept = 0;
    virtual bool create(unsigned slot) noexcept = 0; // Cleans partial failure.
    virtual bool begin(unsigned slot) noexcept = 0; // Failure issued no Begin.
    virtual bool end(unsigned slot) noexcept = 0;   // Failure means uncertain closure.
    virtual DisjointResult poll(unsigned slot) noexcept = 0;
    virtual void destroy(unsigned slot) noexcept = 0;
};

// Frequency/validity shared by independently owned timestamp pairs. Tokens
// are inert: copying or destroying one never issues a query or dereferences a
// clock. All lifetime changes are explicit on the verified owner thread.
class DisjointClock final {
public:
    static constexpr unsigned kRecords = 8, kLeases = 32;
    static constexpr uint64_t kMaxAgeMs = 2000;
    struct Lease {
        uint64_t clock = 0, generation = 0, serial = 0;
        unsigned record = 0, index = 0;
        explicit operator bool() const noexcept { return clock != 0; }
    };
private:
    enum class Kind { Borrowed, Standalone, Frame };
    enum class State { Idle, Open, Pending, Complete };
    struct LeaseState { uint64_t serial = 0; Kind kind = Kind::Borrowed;
                        bool occupied = false, ended = false, invalid = false; };
    struct Record {
        State state = State::Idle;
        bool created = false;
        uint64_t generation = 0, nextSerial = 0, closedAt = 0;
        unsigned refs = 0;
        DisjointResult result{};
        std::array<LeaseState, kLeases> leases{};
    };
    DisjointBackend& backend_;
    const GpuSpanOwner owner_;
    const uint64_t id_;
    std::array<Record, kRecords> records_{};
    int active_ = -1;
    bool stopped_ = false, shut_ = false;

    static uint64_t nextId() noexcept {
        static std::atomic<uint64_t> id{0};
        return id.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    static DisjointResult failure(DisjointReason why) noexcept {
        return {DisjointStatus::Failed, 0, false, why};
    }
    bool ownerOk() const noexcept {
        const auto actual = backend_.currentOwner();
        return owner_.device && owner_.context && owner_.thread && owner_.immediate &&
            actual.device == owner_.device && actual.context == owner_.context &&
            actual.thread == owner_.thread && actual.immediate;
    }
    // Every caller gates ownership before reading these mutable records.
    bool valid(const Lease& t) const noexcept {
        if (t.clock != id_ || t.record >= kRecords || t.index >= kLeases) return false;
        const auto& r = records_[t.record];
        const auto& l = r.leases[t.index];
        return r.state != State::Idle && r.generation == t.generation &&
               l.occupied && l.serial == t.serial;
    }
    Lease allocate(unsigned i, Kind kind) noexcept {
        auto& r = records_[i];
        for (unsigned j = 0; j < kLeases; ++j) if (!r.leases[j].occupied) {
            // Exhaustion cannot wrap and make an old token live again.
            if (r.nextSerial == UINT64_MAX) return {};
            auto& l = r.leases[j];
            l = {++r.nextSerial, kind, true, false, false};
            ++r.refs;
            return {id_, r.generation, l.serial, i, j};
        }
        return {};
    }
    Lease open(Kind kind) noexcept {
        for (unsigned i = 0; i < kRecords; ++i) {
            auto& r = records_[i];
            if (r.state != State::Idle || r.generation == UINT64_MAX) continue;
            if (!r.created) {
                if (!backend_.create(i)) return {};
                r.created = true;
            }
            if (!backend_.begin(i)) {
                backend_.destroy(i);
                r.created = false;
                return {};
            }
            ++r.generation;
            r.state = State::Open;
            r.result = {};
            active_ = static_cast<int>(i);
            // Idle records have no leases and nextSerial resets at retirement.
            return allocate(i, kind);
        }
        return {};
    }
    bool close(unsigned i, uint64_t now) noexcept {
        auto& r = records_[i];
        const bool ok = backend_.end(i);
        active_ = -1;
        r.closedAt = now;
        r.state = ok ? State::Pending : State::Complete;
        if (!ok) { stopped_ = true; r.result = failure(DisjointReason::DriverFailure); }
        for (auto& l : r.leases) if (l.occupied && !l.ended) l.invalid = true;
        return ok;
    }
    void retire(unsigned i) noexcept {
        auto& r = records_[i];
        if (r.refs || r.state != State::Complete) return;
        const bool keep = r.created && r.result.status == DisjointStatus::Ready;
        if (r.created && !keep) backend_.destroy(i);
        const auto generation = r.generation;
        r = {};
        r.created = keep;
        r.generation = generation;
    }
    void update(unsigned i, uint64_t now) noexcept {
        auto& r = records_[i];
        if (r.state != State::Pending) return;
        if (stopped_) r.result = failure(DisjointReason::DriverFailure);
        else if (now >= r.closedAt && now - r.closedAt > kMaxAgeMs)
            r.result = failure(DisjointReason::Stale);
        else {
            r.result = backend_.poll(i);
            if (r.result.status == DisjointStatus::Ready) {
                if (r.result.disjoint) r.result = failure(DisjointReason::Disjoint);
                else if (!r.result.frequency) r.result = failure(DisjointReason::ZeroFrequency);
            } else if (r.result.status == DisjointStatus::Failed)
                r.result = failure(DisjointReason::DriverFailure);
        }
        if (r.result.status != DisjointStatus::Pending) r.state = State::Complete;
        retire(i);
    }
public:
    explicit DisjointClock(DisjointBackend& backend) noexcept
        : backend_(backend), owner_(backend.currentOwner()), id_(nextId()) {}
    DisjointClock(const DisjointClock&) = delete;
    DisjointClock& operator=(const DisjointClock&) = delete;
    // No backend calls in destruction. Owner-thread shutdown invalidates all
    // tokens; a process-detach caller must not try to join or use its context.
    Lease startFrame(uint64_t now) noexcept {
        if (!ownerOk() || stopped_ || shut_ || active_ >= 0) return {};
        collect(now);
        return open(Kind::Frame);
    }
    Lease acquireInterval(uint64_t now) noexcept {
        if (!ownerOk() || stopped_ || shut_) return {};
        if (active_ >= 0) return allocate(static_cast<unsigned>(active_), Kind::Borrowed);
        collect(now);
        return open(Kind::Standalone);
    }
    bool endInterval(Lease t, uint64_t now) noexcept {
        if (!ownerOk() || shut_ || !valid(t)) return false;
        auto& r = records_[t.record];
        auto& l = r.leases[t.index];
        if (r.state != State::Open || l.kind == Kind::Frame || l.ended || l.invalid) return false;
        l.ended = true;
        return l.kind == Kind::Standalone ? close(t.record, now) : true;
    }
    bool finishFrame(Lease t, uint64_t now) noexcept {
        if (!ownerOk() || shut_ || !valid(t)) return false;
        auto& r = records_[t.record];
        auto& l = r.leases[t.index];
        if (r.state != State::Open || l.kind != Kind::Frame || l.ended) return false;
        l.ended = true;
        return close(t.record, now);
    }
    DisjointResult poll(Lease t, uint64_t now) noexcept {
        if (!ownerOk()) return failure(DisjointReason::WrongOwner);
        if (shut_ || !valid(t)) return failure(DisjointReason::InvalidLease);
        auto& r = records_[t.record];
        auto& l = r.leases[t.index];
        if (l.invalid) return failure(DisjointReason::Incomplete);
        if (!l.ended) return {};
        update(t.record, now); // An occupied lease prevents retirement.
        return r.result;
    }
    bool release(Lease t, uint64_t now) noexcept {
        if (!ownerOk() || shut_ || !valid(t)) return false;
        auto& r = records_[t.record];
        auto& l = r.leases[t.index];
        if (r.state == State::Open && l.kind != Kind::Borrowed) close(t.record, now);
        l.occupied = false;
        --r.refs;
        retire(t.record);
        return true;
    }
    bool collect(uint64_t now) noexcept {
        if (!ownerOk() || shut_) return false;
        for (unsigned i = 0; i < kRecords; ++i) update(i, now);
        return true;
    }
    bool shutdown(uint64_t now) noexcept {
        if (!ownerOk() || shut_) return false;
        if (active_ >= 0) close(static_cast<unsigned>(active_), now);
        for (unsigned i = 0; i < kRecords; ++i) {
            if (records_[i].created) backend_.destroy(i);
            records_[i] = {};
        }
        shut_ = true;
        return true;
    }
};
} // namespace edvr
