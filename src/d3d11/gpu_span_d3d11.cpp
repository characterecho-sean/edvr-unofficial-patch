#include "gpu_span_d3d11.h"
#include <windows.h>

namespace edvr {
namespace {
HRESULT createQuery(void*, ID3D11Device* dev, const D3D11_QUERY_DESC* desc, ID3D11Query** out) {
    return dev->CreateQuery(desc, out);
}
HRESULT beginQuery(void*, ID3D11DeviceContext* ctx, ID3D11Asynchronous* query) {
    ctx->Begin(query);
    return S_OK;
}
HRESULT endQuery(void*, ID3D11DeviceContext* ctx, ID3D11Asynchronous* query) {
    ctx->End(query);
    return S_OK;
}
HRESULT getData(void*, ID3D11DeviceContext* ctx, ID3D11Asynchronous* query,
                void* data, UINT size, UINT flags) {
    return ctx->GetData(query, data, size, flags);
}
void releaseQuery(void*, ID3D11Query* query) noexcept { query->Release(); }
}

GpuSpanD3D11Driver::GpuSpanD3D11Driver(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                                     const GpuSpanD3D11Ops& ops, unsigned markerCount) noexcept
    : device_(dev), context_(ctx), ops_(ops), markerCount_(markerCount) {
    owner_.device = reinterpret_cast<uintptr_t>(dev);
    owner_.context = reinterpret_cast<uintptr_t>(ctx);
    owner_.thread = GetCurrentThreadId();
    owner_.immediate = false;
    if (dev) dev->AddRef();
    if (ctx) ctx->AddRef();
    if (!ops_.createQuery && !ops_.begin && !ops_.end && !ops_.getData && !ops_.releaseQuery)
        ops_ = {ops_.user, createQuery, beginQuery, endQuery, getData, releaseQuery};
    if (!dev || !ctx || markerCount_ > 6 || !ops_.createQuery || !ops_.begin || !ops_.end ||
        !ops_.getData || !ops_.releaseQuery || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    ID3D11Device* contextDevice = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    ctx->GetDevice(&contextDevice);
    dev->GetImmediateContext(&immediate);
    bound_ = contextDevice == dev && immediate == ctx;
    if (contextDevice) contextDevice->Release();
    if (immediate) immediate->Release();
    owner_.immediate = bound_;
}

GpuSpanD3D11Driver::~GpuSpanD3D11Driver() {
    // Release only. A live scope must be closed through owner-thread shutdown
    // before destroying the driver; destructors never issue context commands.
    for (unsigned i = 0; i < kSlots; ++i) release(i);
    if (context_) context_->Release();
    if (device_) device_->Release();
}
GpuSpanOwner GpuSpanD3D11Driver::currentOwner() const noexcept {
    auto owner = owner_;
    owner.thread = GetCurrentThreadId();
    return owner;
}
bool GpuSpanD3D11Driver::ownerThread() const noexcept {
    return GetCurrentThreadId() == owner_.thread && bound_;
}
bool GpuSpanD3D11Driver::removed() const noexcept {
    return device_->GetDeviceRemovedReason() != S_OK;
}
void GpuSpanD3D11Driver::release(unsigned i) noexcept {
    auto& s = slots_[i];
    if (s.disjoint) ops_.releaseQuery(ops_.user, s.disjoint);
    for (auto* q : s.stamps) if (q) ops_.releaseQuery(ops_.user, q);
    s = {};
}
bool GpuSpanD3D11Driver::create(unsigned i) noexcept {
    if (!ownerThread() || stopped_ || shut_ || i >= kSlots ||
        slots_[i].state != State::Empty || removed()) return false;
    auto& s = slots_[i];
    const D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
    if (ops_.createQuery(ops_.user, device_, &dj, &s.disjoint) != S_OK || !s.disjoint) {
        release(i);
        return false;
    }
    for (unsigned n = 0; n < markerCount_; ++n) {
        auto*& q = s.stamps[n];
        if (ops_.createQuery(ops_.user, device_, &ts, &q) != S_OK || !q) {
            release(i);
            return false;
        }
    }
    s.state = State::Idle;
    return true;
}
bool GpuSpanD3D11Driver::begin(unsigned i) noexcept {
    if (!ownerThread() || stopped_ || shut_ || i >= kSlots || open_ >= 0 ||
        slots_[i].state != State::Idle || removed()) return false;
    auto& s = slots_[i];
    if (ops_.begin(ops_.user, context_, s.disjoint) != S_OK) {
        s.state = State::Failed;
        return false;
    }
    s.state = State::Open;
    s.issued = s.ready = 0;
    s.disjointReady = false;
    s.raw = {};
    open_ = static_cast<int>(i);
    return true;
}
bool GpuSpanD3D11Driver::timestamp(unsigned i, unsigned n) noexcept {
    if (!ownerThread() || stopped_ || shut_ || i >= kSlots || n >= markerCount_ ||
        open_ != static_cast<int>(i) || (slots_[i].issued & (1u << n))) return false;
    auto& s = slots_[i];
    // End is void in D3D11. Preserve issuance even if removal is detected
    // afterward; the policy still closes the disjoint scope and rejects it.
    s.issued |= 1u << n;
    return ops_.end(ops_.user, context_, s.stamps[n]) == S_OK && !removed();
}
bool GpuSpanD3D11Driver::end(unsigned i) noexcept {
    if (!ownerThread() || shut_ || i >= kSlots || open_ != static_cast<int>(i)) return false;
    auto& s = slots_[i];
    const bool ok = ops_.end(ops_.user, context_, s.disjoint) == S_OK && !removed();
    open_ = -1;
    s.state = ok ? State::Pending : State::Failed;
    if (!ok) stopped_ = true;
    return ok;
}
GpuSpanPoll GpuSpanD3D11Driver::poll(unsigned i, GpuSpanRawSample& raw) noexcept {
    raw = {};
    if (!ownerThread() || stopped_ || shut_ || i >= kSlots ||
        slots_[i].state != State::Pending) return GpuSpanPoll::Failed;
    auto& s = slots_[i];
    if (removed()) { s.state = State::Failed; return GpuSpanPoll::Failed; }
    constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if (!s.disjointReady) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        const HRESULT hr = ops_.getData(ops_.user, context_, s.disjoint, &dj, sizeof(dj), flags);
        if (hr == S_FALSE) return GpuSpanPoll::Pending;
        if (hr != S_OK) { s.state = State::Failed; return GpuSpanPoll::Failed; }
        s.raw.frequency = dj.Frequency;
        s.raw.disjoint = dj.Disjoint != FALSE;
        s.disjointReady = true;
    }
    for (unsigned n = 0; n < 6; ++n) {
        const unsigned mask = 1u << n;
        if (!(s.issued & mask) || (s.ready & mask)) continue;
        UINT64 tick = 0;
        const HRESULT hr = ops_.getData(ops_.user, context_, s.stamps[n], &tick, sizeof(tick), flags);
        if (hr == S_FALSE) return GpuSpanPoll::Pending;
        if (hr != S_OK) { s.state = State::Failed; return GpuSpanPoll::Failed; }
        s.raw.ticks[n] = tick;
        s.ready |= mask;
    }
    s.raw.timestampsReady = true;
    raw = s.raw;
    s.state = State::Idle;
    return GpuSpanPoll::Ready;
}
void GpuSpanD3D11Driver::destroy(unsigned i) noexcept {
    if (!ownerThread() || shut_ || i >= kSlots) return;
    if (open_ == static_cast<int>(i)) end(i);
    release(i);
}
bool GpuSpanD3D11Driver::shutdown() noexcept {
    if (!ownerThread() || shut_) return false;
    if (open_ >= 0) end(static_cast<unsigned>(open_));
    for (unsigned i = 0; i < kSlots; ++i) release(i);
    stopped_ = shut_ = true;
    return true;
}
} // namespace edvr
