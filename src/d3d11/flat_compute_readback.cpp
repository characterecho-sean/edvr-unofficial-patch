#include "flat_compute_readback.h"
#include "flat_compute_model.h"
#include <array>
#include <new>
#include <utility>
#include <wrl/client.h>

namespace edvr {
thread_local bool g_flatComputeInternal = false;
std::atomic<bool> g_flatComputeReadbackPending{false};
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kBoundsLimit = 4u * 1024u * 1024u;
constexpr uint32_t kCbLimit = 64u * 1024u;
constexpr uint32_t kBoundsJobs = 8, kCbJobs = 20;
constexpr uint64_t kBytesLimit = uint64_t(kBoundsJobs) * kBoundsLimit + uint64_t(kCbJobs) * kCbLimit;
struct Job {
    bool active = false;
    FlatComputeReadbackToken token{};
    FlatComputeReadbackKind kind = FlatComputeReadbackKind::SparseBounds;
    uint32_t bytes = 0;
    uint64_t present = 0, startedMs = 0, lastPollPresent = 0;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> source, staging;
    ComPtr<ID3D11Query> event;
};
struct Queue {
    DWORD owner = 0;
    uint32_t bounds = 0, cbs = 0;
    uint64_t bytes = 0;
    std::array<Job, kBoundsJobs + kCbJobs> jobs{};
};
Queue* state() {
    // No static COM destructor may execute under the loader lock.
    static Queue* const value = new (std::nothrow) Queue;
    return value;
}
void finish(Queue& q, Job& slot, FlatComputeReadbackStatus status, HRESULT hr,
    const uint8_t* bytes, FlatComputeReadbackCallback callback, void* user) {
    Job job = std::move(slot);
    slot = Job{};
    if (job.kind == FlatComputeReadbackKind::ConstantBuffer) --q.cbs;
    else --q.bounds;
    q.bytes -= job.bytes;
    const bool pending = q.cbs + q.bounds != 0;
    if (!pending) q.owner = 0;
    g_flatComputeReadbackPending.store(pending, std::memory_order_release);
    const FlatComputeReadbackResult result{job.token, job.kind, status, hr,
        bytes, bytes ? job.bytes : 0};
    if (callback) callback(result, user);
    if (bytes) job.context->Unmap(job.staging.Get(), 0);
}
}

HRESULT flatComputeReadbackSchedule(ID3D11DeviceContext* context,
    const FlatComputeReadbackRequest& request) {
    if (!context || !request.source) return E_INVALIDARG;
    Queue* q = state();
    if (!q) return E_OUTOFMEMORY;
    const DWORD thread = GetCurrentThreadId();
    if (q->owner && q->owner != thread) return E_ACCESSDENIED;
    FlatComputeInternalScope internal;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
    D3D11_BUFFER_DESC sourceDesc{};
    request.source->GetDesc(&sourceDesc);
    const bool sparse = request.kind == FlatComputeReadbackKind::SparseBounds;
    const bool cb = request.kind == FlatComputeReadbackKind::ConstantBuffer;
    if (!sparse && !cb && request.kind != FlatComputeReadbackKind::WholeBounds)
        return E_INVALIDARG;
    uint32_t bytes = 0;
    if (sparse) {
        if (!request.offsets || !request.offsetCount || request.offsetCount > 12 ||
            request.wholeBytes) return E_INVALIDARG;
        for (uint32_t i = 0; i < request.offsetCount; ++i) {
            const uint64_t offset = request.offsets[i];
            if (offset % 4 || offset > sourceDesc.ByteWidth ||
                sourceDesc.ByteWidth - offset < 32) return E_INVALIDARG;
        }
        if (sourceDesc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) return E_INVALIDARG;
        bytes = request.offsetCount * 32;
    } else {
        const uint32_t limit = cb ? kCbLimit : kBoundsLimit;
        if (request.offsets || request.offsetCount || !request.wholeBytes ||
            request.wholeBytes != sourceDesc.ByteWidth || request.wholeBytes > limit)
            return E_INVALIDARG;
        if (cb && !(sourceDesc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return E_INVALIDARG;
        bytes = request.wholeBytes;
    }
    if ((cb ? q->cbs >= kCbJobs : q->bounds >= kBoundsJobs) || q->bytes + bytes > kBytesLimit)
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    Job* slot = nullptr;
    for (Job& job : q->jobs) if (!job.active) { slot = &job; break; }
    if (!slot) return HRESULT_FROM_WIN32(ERROR_BUSY);
    ComPtr<ID3D11Device> device, sourceDevice;
    context->GetDevice(&device);
    request.source->GetDevice(&sourceDevice);
    if (!device || device.Get() != sourceDevice.Get()) return E_INVALIDARG;
    Job job;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &job.staging);
    if (FAILED(hr)) return hr;
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    hr = device->CreateQuery(&query, &job.event);
    if (FAILED(hr)) return hr;
    job.token = request.token;
    job.kind = request.kind;
    job.bytes = bytes;
    job.context = context;
    job.source = request.source;
    job.present = job.lastPollPresent = request.presentOrdinal;
    job.startedMs = GetTickCount64();
    if (sparse) {
        for (uint32_t i = 0; i < request.offsetCount; ++i) {
            const UINT offset = static_cast<UINT>(request.offsets[i]);
            const D3D11_BOX box{offset, 0, 0, offset + 32, 1, 1};
            context->CopySubresourceRegion(job.staging.Get(), 0, i * 32, 0, 0,
                request.source, 0, &box);
        }
    } else context->CopyResource(job.staging.Get(), request.source);
    context->End(job.event.Get());
    job.active = true;
    *slot = std::move(job);
    q->owner = thread;
    if (cb) ++q->cbs; else ++q->bounds;
    q->bytes += bytes;
    g_flatComputeReadbackPending.store(true, std::memory_order_relaxed);
    return S_OK;
}

void flatComputeReadbackPoll(ID3D11DeviceContext* context, uint64_t presentOrdinal,
    FlatComputeReadbackCallback callback, void* user) {
    if (!flatComputeReadbackPending() || !context) return;
    Queue* q = state();
    if (!q || q->owner != GetCurrentThreadId()) return;
    FlatComputeInternalScope internal;
    const uint64_t now = GetTickCount64();
    for (Job& job : q->jobs) {
        if (!job.active) continue;
        if (flatComputeDeadline(job.present, presentOrdinal, job.startedMs, now)) {
            finish(*q, job, FlatComputeReadbackStatus::Timeout,
                HRESULT_FROM_WIN32(WAIT_TIMEOUT), nullptr, callback, user);
            continue;
        }
        if (presentOrdinal <= job.present || presentOrdinal == job.lastPollPresent) continue;
        job.lastPollPresent = presentOrdinal;
        if (job.context.Get() != context) continue;
        BOOL ready = FALSE;
        HRESULT hr = context->GetData(job.event.Get(), &ready, sizeof(ready),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(hr)) {
            finish(*q, job, FlatComputeReadbackStatus::DeviceFailure, hr, nullptr, callback, user);
            continue;
        }
        if (hr != S_OK || !ready) continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(job.staging.Get(), 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr)) {
            finish(*q, job, FlatComputeReadbackStatus::DeviceFailure, hr, nullptr, callback, user);
            continue;
        }
        finish(*q, job, FlatComputeReadbackStatus::Complete, S_OK,
            static_cast<const uint8_t*>(mapped.pData), callback, user);
    }
}

void flatComputeReadbackCancel(FlatComputeReadbackCallback callback, void* user) {
    if (!flatComputeReadbackPending()) return;
    Queue* q = state();
    if (!q || q->owner != GetCurrentThreadId()) return;
    FlatComputeInternalScope internal;
    for (Job& job : q->jobs) if (job.active)
        finish(*q, job, FlatComputeReadbackStatus::Cancelled,
            HRESULT_FROM_WIN32(ERROR_CANCELLED), nullptr, callback, user);
}
}
