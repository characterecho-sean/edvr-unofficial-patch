#include "native_timing.h"
#include "../common/gpu_frame_protocol.h"
#include "../common/log.h"
#include "../common/timing.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <wrl/client.h>

extern "C" uint64_t WINAPI edvrGpuFrameEvent(unsigned, unsigned, uint64_t,
                                               unsigned, unsigned, void*);

namespace {
constexpr unsigned kCapacity = 16;
constexpr int64_t kMaxPeriodNs = 1000000000LL * 10; // 10 seconds
constexpr double kMaxFieldMs = 600000.0;            // malformed input guard

struct Context {
    bool active = false;
    ID3D11Device* device = nullptr;
    DWORD producer = 0;
    uint64_t generation = 0;
    uint64_t firstSequence = 0;
    uint64_t waitSequence = 0;
    int64_t waitQpc = 0;
    bool waitOutstanding = false;
    bool waitValid = false;
    bool published = false;
    double pendingWaitMs = 0, pendingPeriodMs = 0;
    unsigned attempted = 0, opened = 0, ended = 0;
    uint64_t validCount = 0, invalidCount = 0, publishedCount = 0;
    uint64_t lastLogMs = 0;
};

Context pool[kCapacity];
unsigned used = 0;
Context* current = nullptr;
std::mutex lifetime;
edvr::NativeTimingSnapshot snapshot;

Context* identify(void* p) noexcept {
    for (unsigned i = 0; i < used; ++i) if (p == &pool[i]) return &pool[i];
    return nullptr;
}

uint64_t nowMs() noexcept { return GetTickCount64(); }
int64_t qpcNow() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}

bool qpcMs(int64_t start, int64_t end, double& ms) noexcept {
    static const int64_t f = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? value.QuadPart : int64_t(0);
    }();
    if (start <= 0 || end < start || f <= 0) return false;
    ms = static_cast<double>(end - start) * 1000.0 / static_cast<double>(f);
    return std::isfinite(ms) && ms <= kMaxFieldMs;
}

bool finiteNonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= kMaxFieldMs;
}

void clearCpu() noexcept {
    snapshot.haveCpu = false;
    snapshot.invalid = false;
    snapshot.cpu = {};
    snapshot.capturedAtMs = 0;
    snapshot.waitMs = 0;
    snapshot.predictedPeriodMs = 0;
    snapshot.sequence = 0;
}

void poison(uint64_t sequence) noexcept {
    if (sequence) edvrGpuFrameEvent(edvr::kGpuFrameProtocol,
        static_cast<unsigned>(edvr::GpuFrameEvent::Cancel), sequence, 0, 0, nullptr);
}

void logCounts(Context& c, const char* reason) noexcept {
    const auto now = nowMs();
    if (!c.lastLogMs || now - c.lastLogMs >= 5000) {
        c.lastLogMs = now;
        edvr::Log::get().note("native timing: %s; waits valid %llu invalid %llu; "
            "wait/submit/temporal/menu/transfer/compose are wall elapsed, not GPU or exclusive CPU.",
            reason, static_cast<unsigned long long>(c.validCount),
            static_cast<unsigned long long>(c.invalidCount));
    }
}

uint64_t WINAPI waitBegin(void* p) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c || !c->active || c != current) return 0;
    // An unfinished prior wait cannot leave its CPU sample fresh.
    if (c->waitSequence && !c->published) {
        poison(c->waitSequence);
        clearCpu();
    }
    const uint64_t sequence = edvrGpuFrameEvent(edvr::kGpuFrameProtocol,
        static_cast<unsigned>(edvr::GpuFrameEvent::WaitBegin), 0, 0, 0, nullptr);
    c->waitSequence = sequence;
    c->waitQpc = sequence ? qpcNow() : 0;
    c->waitOutstanding = sequence != 0;
    c->waitValid = false;
    c->published = false;
    c->attempted = c->opened = c->ended = 0;
    if (!sequence) { clearCpu(); snapshot.invalid = true; }
    if (sequence && !c->firstSequence) {
        c->firstSequence = sequence;
        snapshot.firstSequence = sequence;
    }
    return sequence;
}

HRESULT WINAPI waitEnd(void* p, uint64_t sequence, uint32_t valid, int64_t periodNs) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c || !c->active || c != current || !c->waitOutstanding ||
        sequence != c->waitSequence) return E_INVALIDARG;
    const int64_t end = qpcNow();
    const bool sensible = periodNs >= 0 && periodNs <= kMaxPeriodNs;
    double elapsedMs = 0;
    const bool ok = valid == 1 && sensible && qpcMs(c->waitQpc, end, elapsedMs) &&
        edvrGpuFrameEvent(edvr::kGpuFrameProtocol,
            static_cast<unsigned>(edvr::GpuFrameEvent::WaitEnd), sequence, 0, 1, nullptr);
    c->waitOutstanding = false;
    c->waitValid = ok;
    c->published = false;
    ++(ok ? c->validCount : c->invalidCount);
    if (!ok) {
        poison(sequence);
        clearCpu();
        snapshot.invalid = true;
        logCounts(*c, "invalid wait sample");
        return E_INVALIDARG;
    }
    // Preserve the complete previous snapshot, including its own wait time
    // and period. New wait metadata is published only with its matching pair.
    c->pendingWaitMs = elapsedMs;
    c->pendingPeriodMs = static_cast<double>(periodNs) / 1000000.0;
    return S_OK;
}

uint32_t WINAPI gpuEye(void* p, uint64_t sequence, uint32_t eye, uint32_t begin,
                       uint32_t accepted, ID3D11Texture2D* texture) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c || !c->active || c != current || !sequence || !c->waitValid || c->published || sequence != c->waitSequence) return 0;
    if (GetCurrentThreadId() != c->producer || eye > 1 || begin > 1 || accepted > 1 || !texture) {
        poison(sequence); return 0;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
    Microsoft::WRL::ComPtr<IUnknown> a, b;
    texture->GetDevice(&sourceDevice);
    const bool same = sourceDevice && SUCCEEDED(c->device->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(sourceDevice->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
    if (!same) { poison(sequence); return 0; }
    const unsigned bit = 1u << eye;
    if (begin) {
        if ((c->attempted & bit) || c->opened) { poison(sequence); return 0; }
        c->attempted |= bit;
    } else {
        if (!(c->opened & bit) || (c->ended & bit)) { poison(sequence); return 0; }
        c->opened &= ~bit; c->ended |= bit;
    }
    const auto event = begin ? edvr::GpuFrameEvent::SubmitBegin : edvr::GpuFrameEvent::SubmitEnd;
    const uint64_t result = edvrGpuFrameEvent(edvr::kGpuFrameProtocol,
        static_cast<unsigned>(event), sequence, eye, begin ? 0u : (accepted ? 1u : 0u), texture);
    if (result && begin) c->opened |= bit;
    // GPU timing can be disabled or lack a covered render command. Its own
    // controller handles validity; CPU wall measurements remain independent.
    return result ? 1u : 0u;
}

HRESULT WINAPI publishCpu(void* p, const EdvrNativeTimingFrame* frame) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c || !c->active || c != current || !frame ||
        frame->size != sizeof(*frame) || frame->version != EDVR_NATIVE_TIMING_VERSION_1 ||
        !c->waitValid || c->published || frame->sequence != c->waitSequence) return E_INVALIDARG;
    bool fieldsValid = finiteNonnegative(frame->composeMs);
    for (unsigned eye = 0; eye < 2; ++eye) fieldsValid = fieldsValid &&
        finiteNonnegative(frame->submitMs[eye]) && finiteNonnegative(frame->temporalMs[eye]) &&
        finiteNonnegative(frame->menuMs[eye]) && finiteNonnegative(frame->transferMs[eye]);
    if (!fieldsValid) {
        poison(frame->sequence); c->waitValid = false;
        clearCpu(); snapshot.invalid = true; return E_INVALIDARG;
    }
    snapshot.cpu = *frame;
    snapshot.haveCpu = true;
    snapshot.invalid = false;
    snapshot.active = true;
    snapshot.generation = c->generation;
    snapshot.sequence = frame->sequence;
    snapshot.capturedAtMs = nowMs();
    snapshot.waitMs = c->pendingWaitMs;
    snapshot.predictedPeriodMs = c->pendingPeriodMs;
    c->published = true;
    if (++c->publishedCount == 1 || !c->lastLogMs || snapshot.capturedAtMs - c->lastLogMs >= 5000) {
        c->lastLogMs = snapshot.capturedAtMs;
        edvr::Log::get().note("native timing CPU: seq %llu, wait %.3f ms, submits %.3f ms, "
            "temporal %.3f menu %.3f transfer %.3f compose %.3f ms; wall elapsed includes waits, "
            "not exclusive CPU or GPU; valid waits %llu invalid waits %llu.",
            (unsigned long long)frame->sequence,snapshot.waitMs,frame->submitMs[0]+frame->submitMs[1],
            frame->temporalMs[0]+frame->temporalMs[1],frame->menuMs[0]+frame->menuMs[1],
            frame->transferMs[0]+frame->transferMs[1],frame->composeMs,
            (unsigned long long)c->validCount,(unsigned long long)c->invalidCount);
    }
    return S_OK;
}

HRESULT WINAPI invalidate(void* p) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c || !c->active || c != current) return E_INVALIDARG;
    poison(c->waitSequence);
    c->waitOutstanding = c->waitValid = c->published = false;
    clearCpu(); snapshot.active = true; snapshot.generation = c->generation; snapshot.invalid = true;
    return S_OK;
}

HRESULT WINAPI close(void* p) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c) return E_INVALIDARG;
    if (!c->active) return S_FALSE;
    poison(c->waitSequence);
    c->active = false; c->device = nullptr; c->waitOutstanding = c->waitValid = false;
    c->published = false;
    if (current == c) current = nullptr;
    snapshot = {};
    return S_OK;
}
}

namespace edvr {
NativeTimingSnapshot nativeTimingSnapshot() noexcept {
    std::lock_guard<std::mutex> lock(lifetime);
    return snapshot;
}
}

extern "C" HRESULT WINAPI edvrAcquireNativeTiming(const EdvrNativeTimingRequest* request,
                                                    EdvrNativeTimingTable* table) {
    if (!table || table->size != sizeof(*table) || table->version != EDVR_NATIVE_TIMING_VERSION_1)
        return E_INVALIDARG;
    *table = {sizeof(*table), EDVR_NATIVE_TIMING_VERSION_1};
    if (!request || request->size != sizeof(*request) || request->version != EDVR_NATIVE_TIMING_VERSION_1 ||
        !request->device || !request->generation) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(lifetime);
    if (current || used == kCapacity) return E_PENDING;
    Context& c = pool[used++];
    c = {}; c.active = true; c.device = request->device; c.producer = GetCurrentThreadId();
    c.generation = request->generation; current = &c;
    snapshot = {}; snapshot.active = true; snapshot.generation = c.generation;
    table->context = &c; table->waitBegin = waitBegin; table->waitEnd = waitEnd;
    table->gpuEye = gpuEye; table->publishCpu = publishCpu; table->invalidate = invalidate; table->close = close;
    return S_OK;
}
