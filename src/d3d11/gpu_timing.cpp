#include "gpu_timing.h"
#include "gpu_disjoint_d3d11.h"
#include "../common/log.h"
#include <atomic>
#include <memory>
#include <new>
#include <utility>

namespace edvr {
namespace {
HRESULT createQuery(void*, ID3D11Device* d, const D3D11_QUERY_DESC* desc, ID3D11Query** q) {
    return d->CreateQuery(desc, q);
}
HRESULT beginQuery(void*, ID3D11DeviceContext* c, ID3D11Asynchronous* q) { c->Begin(q); return S_OK; }
HRESULT endQuery(void*, ID3D11DeviceContext* c, ID3D11Asynchronous* q) { c->End(q); return S_OK; }
HRESULT queryData(void*, ID3D11DeviceContext* c, ID3D11Asynchronous* q, void* p, UINT n, UINT flags) {
    return c->GetData(q, p, n, flags);
}
void releaseQuery(void*, ID3D11Query* q) noexcept { q->Release(); }
GpuSpanD3D11Ops nativeOrCustom(GpuSpanD3D11Ops ops) noexcept {
    if (!ops.createQuery && !ops.begin && !ops.end && !ops.getData && !ops.releaseQuery)
        ops = {ops.user, createQuery, beginQuery, endQuery, queryData, releaseQuery};
    return ops;
}

struct TimerRegistration {
    TimerRegistration* previous = nullptr;
    TimerRegistration* next = nullptr;
    virtual void expire(uint64_t now) noexcept = 0;
};
struct Domain {
    const GpuSpanD3D11Ops ops;
    DisjointD3D11Backend backend;
    DisjointClock clock;
    const GpuSpanOwner owner;
    std::atomic<bool> unavailable{false};
    TimerRegistration* timers = nullptr; // Owner-thread use; quiescent reset/unload.
    uint64_t lastSweep = 0;
    bool firstReady = false;
    Domain(ID3D11Device* d, ID3D11DeviceContext* c, const GpuSpanD3D11Ops& custom)
        : ops(nativeOrCustom(custom)), backend(d, c, ops), clock(backend), owner(backend.currentOwner()) {}
    bool owns(ID3D11DeviceContext* c) const noexcept {
        return owner.immediate && owner.context == reinterpret_cast<uintptr_t>(c) &&
            owner.thread == GetCurrentThreadId();
    }
    bool accepts(ID3D11DeviceContext* c) const noexcept {
        return owns(c) && !unavailable.load(std::memory_order_acquire);
    }
    ID3D11Device* device() const noexcept { return reinterpret_cast<ID3D11Device*>(owner.device); }
    void collectExpired(uint64_t now) noexcept {
        if (now >= lastSweep && now - lastSweep < 100) return;
        lastSweep = now;
        // Disabled producers no longer poll their own rings. Retire their
        // expired leases as well, or they could hold every shared record.
        for (auto* t = timers; t; t = t->next) t->expire(now);
    }
};

// Never run shared_ptr/COM destruction during process termination. Explicit
// quiescent unload detaches this registry before owners reset their timers.
std::shared_ptr<Domain>& registry() {
    alignas(std::shared_ptr<Domain>) static unsigned char storage[sizeof(std::shared_ptr<Domain>)];
    static auto* value = new (storage) std::shared_ptr<Domain>;
    return *value;
}
std::shared_ptr<Domain> current() { return std::atomic_load(&registry()); }
void stop(const std::shared_ptr<Domain>& d) noexcept {
    if (!d->unavailable.exchange(true, std::memory_order_acq_rel))
        Log::get().note("GPU timing: an unfinished sample was abandoned without its owner context; "
                        "timing is unavailable until explicit owner shutdown. Rendering continues.");
}
} // namespace

bool gpuTimingBind(ID3D11Device* dev, ID3D11DeviceContext* ctx, const GpuSpanD3D11Ops& ops) noexcept {
    if (!dev || !ctx) return false;
    try {
        auto d = current();
        if (d) return d->accepts(ctx) && d->device() == dev;
        auto candidate = std::make_shared<Domain>(dev, ctx, ops);
        if (!candidate->owner.immediate) return false;
        if (!std::atomic_compare_exchange_strong(&registry(), &d, candidate))
            return d && d->accepts(ctx) && d->device() == dev;
        Log::get().note("GPU timing: shared disjoint clock bound to device %p, immediate context %p, "
                        "thread %lu; existing timer pairs borrow one frequency scope.",
                        static_cast<void*>(dev), static_cast<void*>(ctx), GetCurrentThreadId());
        return true;
    } catch (...) { return false; }
}

bool gpuTimingAccepts(ID3D11DeviceContext* ctx) noexcept {
    const auto d = current();
    return d && d->accepts(ctx);
}
bool gpuTimingOwns(ID3D11DeviceContext* ctx) noexcept {
    const auto d = current();
    return d && d->owns(ctx);
}
bool gpuTimingShutdown(ID3D11DeviceContext* ctx) noexcept {
    auto d = current();
    if (!d || !d->owns(ctx)) return false;
    d->unavailable.store(true, std::memory_order_release);
    const bool result = d->clock.shutdown(GetTickCount64());
    auto expected = d;
    std::atomic_compare_exchange_strong(&registry(), &expected, std::shared_ptr<Domain>{});
    return result;
}
void gpuTimingAbandon() noexcept {
    auto d = std::atomic_exchange(&registry(), std::shared_ptr<Domain>{});
    if (d) d->unavailable.store(true, std::memory_order_release);
    // The clock's destructor issues nothing; native backend destruction only
    // Releases. Outstanding timer states retain this domain until reset.
}

struct GpuTimer::State : TimerRegistration {
    enum class Phase { Idle, Open, Pending } phase = Phase::Idle;
    std::shared_ptr<Domain> domain;
    ID3D11Query* stamps[2]{};
    DisjointClock::Lease lease{};
    uint64_t ticks[2]{}, startedAt = 0;
    unsigned ready = 0;
    bool invalid = false;
    explicit State(std::shared_ptr<Domain> d) : domain(std::move(d)) {
        next = domain->timers;
        if (next) next->previous = this;
        domain->timers = this;
    }
    void dropQueries() noexcept {
        for (auto*& q : stamps) if (q) { domain->ops.releaseQuery(domain->ops.user, q); q = nullptr; }
    }
    ~State() { // Explicit reset only; never GpuTimer destruction.
        if (previous) previous->next = next;
        else domain->timers = next;
        if (next) next->previous = previous;
        dropQueries();
    }
    bool allocate() noexcept {
        if (stamps[0] && stamps[1]) return true;
        const D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
        for (auto*& q : stamps) {
            if (domain->ops.createQuery(domain->ops.user, domain->device(), &desc, &q) != S_OK || !q) {
                dropQueries(); return false;
            }
        }
        return true;
    }
    void retire(bool keepQueries) noexcept {
        if (lease && !domain->unavailable.load(std::memory_order_acquire))
            domain->clock.release(lease, GetTickCount64());
        lease = {};
        phase = Phase::Idle;
        if (!keepQueries) dropQueries();
    }
    void expire(uint64_t now) noexcept override {
        if (phase != Phase::Idle && now >= startedAt && now - startedAt > DisjointClock::kMaxAgeMs)
            retire(false);
    }
};

GpuTimer::GpuTimer(GpuTimer&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }
GpuTimer& GpuTimer::operator=(GpuTimer&& other) noexcept {
    if (this != &other) { reset(); state_ = other.state_; other.state_ = nullptr; }
    return *this;
}

bool GpuTimer::begin(ID3D11Device* dev, ID3D11DeviceContext* ctx) noexcept {
    if (state_ && !state_->domain->owns(ctx)) return false;
    if (!gpuTimingBind(dev, ctx)) return false;
    const auto d = current();
    if (!d || !d->accepts(ctx) || d->device() != dev) return false;
    const uint64_t now = GetTickCount64();
    d->collectExpired(now);
    if (state_ && state_->domain != d) reset(ctx);
    if (!state_) state_ = new (std::nothrow) State(d);
    if (!state_) return false;
    auto& s = *state_;
    if (s.phase != State::Phase::Idle || !s.allocate()) return false;
    s.startedAt = now;
    s.lease = d->clock.acquireInterval(s.startedAt);
    if (!s.lease) return false;
    s.phase = State::Phase::Open;
    s.ready = 0;
    s.invalid = false;
    s.ticks[0] = s.ticks[1] = 0;
    if (d->ops.end(d->ops.user, ctx, s.stamps[0]) != S_OK) {
        s.retire(false); // Explicit owner cancellation closes a standalone scope once.
        return false;
    }
    return true;
}
bool GpuTimer::end(ID3D11DeviceContext* ctx) noexcept {
    if (!state_ || !state_->domain->accepts(ctx)) return false;
    auto& s = *state_;
    if (s.phase != State::Phase::Open) return false;
    const auto& d = s.domain;
    // A parent may already have invalidated this unfinished lease. Do not
    // issue a timestamp outside its disjoint scope in that case.
    if (d->clock.poll(s.lease, GetTickCount64()).status == DisjointStatus::Failed) {
        s.invalid = true;
        s.phase = State::Phase::Pending;
        return false;
    }
    const bool stamped = d->ops.end(d->ops.user, ctx, s.stamps[1]) == S_OK;
    const bool ended = d->clock.endInterval(s.lease, GetTickCount64());
    s.invalid = !stamped || !ended;
    s.phase = State::Phase::Pending;
    return !s.invalid;
}
GpuTimerPoll GpuTimer::poll(ID3D11DeviceContext* ctx, double& ms) noexcept {
    if (!state_) return GpuTimerPoll::Invalid;
    if (!state_->domain->owns(ctx)) return GpuTimerPoll::Pending;
    auto& s = *state_;
    if (s.domain->unavailable.load(std::memory_order_acquire)) {
        s.retire(false); return GpuTimerPoll::Invalid;
    }
    if (s.phase == State::Phase::Idle) return GpuTimerPoll::Invalid;
    const uint64_t now = GetTickCount64();
    if (s.invalid || (now >= s.startedAt && now - s.startedAt > DisjointClock::kMaxAgeMs)) {
        s.retire(false); return GpuTimerPoll::Invalid;
    }
    const auto result = s.domain->clock.poll(s.lease, now);
    if (result.status == DisjointStatus::Pending) return GpuTimerPoll::Pending;
    if (result.status != DisjointStatus::Ready) { s.retire(false); return GpuTimerPoll::Invalid; }
    for (unsigned i = 0; i < 2; ++i) {
        if (s.ready & (1u << i)) continue;
        const auto& ops = s.domain->ops;
        const HRESULT hr = ops.getData(ops.user, ctx, s.stamps[i], &s.ticks[i], sizeof(uint64_t),
                                       D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) continue;
        if (hr != S_OK) { s.retire(false); return GpuTimerPoll::Invalid; }
        s.ready |= 1u << i;
    }
    if (s.ready != 3) return GpuTimerPoll::Pending;
    if (s.ticks[1] < s.ticks[0]) { s.retire(false); return GpuTimerPoll::Invalid; }
    ms = double(s.ticks[1] - s.ticks[0]) * 1000.0 / double(result.frequency);
    if (!s.domain->firstReady) {
        s.domain->firstReady = true;
        Log::get().note("GPU timing: first shared-clock interval ready: %.4f ms, frequency %llu, "
                        "context %p, thread %lu. This is an existing pass timer, not the outer frame span.",
                        ms, static_cast<unsigned long long>(result.frequency),
                        static_cast<void*>(ctx), GetCurrentThreadId());
    }
    s.retire(true);
    return GpuTimerPoll::Ready;
}
void GpuTimer::reset(ID3D11DeviceContext* ctx) noexcept {
    if (!state_) return;
    auto* s = state_;
    const auto& d = s->domain;
    if (ctx && !d->owns(ctx)) return;
    if (s->lease && !d->unavailable.load(std::memory_order_acquire)) {
        if (d->owns(ctx) || (d->owner.thread == GetCurrentThreadId() && s->phase != State::Phase::Open))
            d->clock.release(s->lease, GetTickCount64());
        else stop(d); // Do not End an open query from unload or an unverified caller.
    }
    state_ = nullptr;
    delete s;
}

struct GpuTimingFrameDriver::State {
    enum class Phase { Empty, Idle, Open, Pending, Failed };
    struct Slot {
        Phase phase = Phase::Empty;
        ID3D11Query* stamps[6]{};
        DisjointClock::Lease lease{};
        unsigned issued = 0, ready = 0;
        bool invalid = false;
        GpuSpanRawSample raw{};
    } slots[GpuSpanState::kSlots];
    const std::shared_ptr<Domain> domain;
    const GpuSpanOwner owner;
    int open = -1;
    bool stopped = false;
    explicit State(std::shared_ptr<Domain> d) : domain(std::move(d)), owner(domain->owner) {}
    ID3D11DeviceContext* context() const noexcept {
        return reinterpret_cast<ID3D11DeviceContext*>(owner.context);
    }
    void drop(unsigned i) noexcept {
        for (auto*& q : slots[i].stamps) if (q) {
            domain->ops.releaseQuery(domain->ops.user, q);
            q = nullptr;
        }
    }
    void release(Slot& q) noexcept {
        if (q.lease && !domain->unavailable.load(std::memory_order_acquire))
            domain->clock.release(q.lease, GetTickCount64());
        q.lease = {};
    }
    ~State() { // Explicit reset only; no context calls.
        for (unsigned i = 0; i < GpuSpanState::kSlots; ++i) drop(i);
    }
};

bool GpuTimingFrameDriver::bind(ID3D11Device* dev, ID3D11DeviceContext* ctx) noexcept {
    if (state_ && !state_->domain->owns(ctx)) return false;
    if (!gpuTimingBind(dev, ctx)) return false;
    auto d = current();
    if (!d || !d->accepts(ctx) || d->device() != dev) return false;
    if (state_ && state_->domain != d) reset(ctx);
    if (!state_) state_ = new (std::nothrow) State(d);
    return state_ != nullptr;
}
GpuSpanOwner GpuTimingFrameDriver::currentOwner() const noexcept {
    if (!state_) return {};
    auto owner = state_->owner;
    owner.thread = GetCurrentThreadId(); // Never return a cached thread as the caller.
    return owner;
}
bool GpuTimingFrameDriver::create(unsigned i) noexcept {
    auto* s = state_;
    if (!s || !s->domain->accepts(s->context()) || i >= GpuSpanState::kSlots) return false;
    auto& q = s->slots[i];
    if (q.phase != State::Phase::Empty || s->stopped) return false;
    const D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
    for (auto*& stamp : q.stamps) {
        if (s->domain->ops.createQuery(s->domain->ops.user, s->domain->device(), &desc, &stamp) != S_OK || !stamp) {
            s->drop(i);
            return false;
        }
    }
    q.phase = State::Phase::Idle;
    return true;
}
bool GpuTimingFrameDriver::begin(unsigned i) noexcept {
    auto* s = state_;
    if (!s || !s->domain->accepts(s->context()) || i >= GpuSpanState::kSlots) return false;
    if (s->open >= 0 || s->stopped || s->slots[i].phase != State::Phase::Idle) return false;
    const auto now = GetTickCount64();
    s->domain->collectExpired(now);
    auto& q = s->slots[i];
    q.raw = {};
    q.issued = q.ready = 0;
    q.invalid = false;
    q.lease = s->domain->clock.startFrame(now);
    if (!q.lease) return false; // An active standalone scope is never borrowed as a frame.
    q.phase = State::Phase::Open;
    s->open = static_cast<int>(i);
    return true;
}
bool GpuTimingFrameDriver::timestamp(unsigned i, unsigned n) noexcept {
    auto* s = state_;
    if (!s || !s->domain->accepts(s->context()) || i >= GpuSpanState::kSlots || n >= 6) return false;
    if (s->open != static_cast<int>(i)) return false;
    auto& q = s->slots[i];
    if (!q.stamps[n] || (q.issued & (1u << n))) return false;
    q.issued |= 1u << n; // A failed callback may still have issued; never retry it.
    const bool ok = s->domain->ops.end(s->domain->ops.user, s->context(), q.stamps[n]) == S_OK;
    q.invalid |= !ok;
    return ok;
}
bool GpuTimingFrameDriver::end(unsigned i) noexcept {
    auto* s = state_;
    if (!s || !s->domain->accepts(s->context()) || i >= GpuSpanState::kSlots) return false;
    if (s->open != static_cast<int>(i)) return false;
    auto& q = s->slots[i];
    const bool ok = s->domain->clock.finishFrame(q.lease, GetTickCount64());
    q.phase = ok ? State::Phase::Pending : State::Phase::Failed;
    s->open = -1; // End failure is uncertain and is never retried.
    if (!ok) s->stopped = true;
    return ok;
}
GpuSpanPoll GpuTimingFrameDriver::poll(unsigned i, GpuSpanRawSample& out) noexcept {
    out = {};
    auto* s = state_;
    if (!s || !s->domain->owns(s->context())) return GpuSpanPoll::Pending;
    if (i >= GpuSpanState::kSlots || s->domain->unavailable.load(std::memory_order_acquire))
        return GpuSpanPoll::Failed;
    auto& q = s->slots[i];
    if (q.phase != State::Phase::Pending || q.invalid) return GpuSpanPoll::Failed;
    const auto result = s->domain->clock.poll(q.lease, GetTickCount64());
    if (result.status == DisjointStatus::Pending) return GpuSpanPoll::Pending;
    if (result.status != DisjointStatus::Ready && result.reason != DisjointReason::Disjoint &&
        result.reason != DisjointReason::ZeroFrequency) {
        q.phase = State::Phase::Failed;
        return GpuSpanPoll::Failed;
    }
    q.raw.frequency = result.frequency;
    q.raw.disjoint = result.reason == DisjointReason::Disjoint || result.disjoint;
    for (unsigned n = 0; n < 6; ++n) {
        if (!(q.issued & (1u << n)) || (q.ready & (1u << n))) continue;
        const HRESULT hr = s->domain->ops.getData(s->domain->ops.user, s->context(), q.stamps[n],
            &q.raw.ticks[n], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) continue;
        if (hr != S_OK) { q.phase = State::Phase::Failed; return GpuSpanPoll::Failed; }
        q.ready |= 1u << n;
    }
    if (q.ready != q.issued) return GpuSpanPoll::Pending;
    q.raw.timestampsReady = true;
    out = q.raw;
    s->release(q);
    q.phase = State::Phase::Idle;
    return GpuSpanPoll::Ready;
}
void GpuTimingFrameDriver::destroy(unsigned i) noexcept {
    auto* s = state_;
    if (!s || !s->domain->owns(s->context()) || i >= GpuSpanState::kSlots) return;
    if (s->open == static_cast<int>(i)) {
        if (!s->domain->unavailable.load(std::memory_order_acquire)) end(i);
        s->open = -1;
    }
    s->release(s->slots[i]);
    s->drop(i);
    s->slots[i] = {};
}
void GpuTimingFrameDriver::reset(ID3D11DeviceContext* ctx) noexcept {
    auto* s = state_;
    if (!s || (ctx && !s->domain->owns(ctx))) return;
    if (!ctx) {
        bool retained = false;
        for (const auto& q : s->slots) retained |= static_cast<bool>(q.lease);
        if (retained) stop(s->domain); // No clock calls at all during quiescent unload.
    } else {
        if (s->open >= 0 && !s->domain->unavailable.load(std::memory_order_acquire))
            end(static_cast<unsigned>(s->open));
        for (auto& q : s->slots) s->release(q);
    }
    state_ = nullptr;
    delete s;
}
} // namespace edvr
