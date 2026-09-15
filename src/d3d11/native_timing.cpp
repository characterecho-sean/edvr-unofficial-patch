#include "native_timing.h"
#include "../common/gpu_frame_protocol.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/timing.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <limits>

extern "C" uint64_t WINAPI edvrGpuFrameEvent(unsigned, unsigned, uint64_t,
                                               unsigned, unsigned, void*);

namespace {
constexpr unsigned kCapacity = 16;
constexpr unsigned kApplicationSegments = 4;
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
    uint64_t gpuRejectFloor = 0, lastGpuSequence = 0, lastGpuLogMs = 0;
    bool gpuLoggedValid = false;
    int64_t applicationQpc = 0;
    double applicationMs = 0;
    unsigned applicationSegments = 0;
    bool applicationOpen = false, applicationValid = true;
    bool completionQueued = false;
};

Context pool[kCapacity];
unsigned used = 0;
Context* current = nullptr;
std::mutex lifetime;
edvr::NativeTimingSnapshot snapshot;
constexpr unsigned kCompletionCapacity = 256;
struct Completion { uint64_t ordinal = 0; edvr::NativeTimingSnapshot value{}; };
std::array<Completion,kCompletionCapacity> completions{};
uint64_t nextCompletion = 1;

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
    snapshot.applicationMs = 0;
    snapshot.applicationValid = false;
}

void queueCompletion(Context& c, uint64_t sequence, bool invalid) noexcept {
    if (!sequence || c.completionQueued) return;
    auto value = snapshot;
    value.active = true;
    value.generation = c.generation;
    value.sequence = sequence;
    value.capturedAtMs = nowMs();
    if (invalid) {
        value.haveCpu = false;
        value.invalid = true;
        value.cpu = {};
        value.waitMs = 0;
        value.predictedPeriodMs = 0;
        value.applicationMs = 0;
        value.applicationValid = false;
    }
    const auto ordinal = nextCompletion++;
    completions[ordinal % kCompletionCapacity] = {ordinal, value};
    c.completionQueued = true;
}

void clearDeviceGpu() noexcept {
    snapshot.haveDeviceGpu = false;
    snapshot.deviceGpu = {};
}

void rejectGpu(Context& c, uint64_t sequence) noexcept {
    if (sequence > c.gpuRejectFloor) c.gpuRejectFloor = sequence;
    clearDeviceGpu();
}

const char* gpuStatusName(uint32_t status) noexcept {
    switch (status) {
    case EdvrNativeGpuPending: return "pending";
    case EdvrNativeGpuValid: return "valid";
    case EdvrNativeGpuDisabled: return "disabled";
    case EdvrNativeGpuIncomplete: return "incomplete";
    case EdvrNativeGpuQueryFailure: return "query-failure";
    case EdvrNativeGpuStale: return "stale";
    case EdvrNativeGpuNotSeparate: return "not-separate";
    default: return "invalid";
    }
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
        queueCompletion(*c, c->waitSequence, true);
        poison(c->waitSequence);
        rejectGpu(*c, c->waitSequence);
        clearCpu();
    }
    const uint64_t sequence = edvrGpuFrameEvent(edvr::kGpuFrameProtocol,
        static_cast<unsigned>(edvr::GpuFrameEvent::WaitBegin), 0, 0, 0, nullptr);
    c->waitSequence = sequence;
    c->waitQpc = sequence ? qpcNow() : 0;
    c->waitOutstanding = sequence != 0;
    c->waitValid = false;
    c->published = false;
    c->completionQueued = false;
    c->attempted = c->opened = c->ended = 0;
    c->applicationQpc = 0; c->applicationMs = 0;
    c->applicationSegments = 0;
    c->applicationOpen = false; c->applicationValid = true;
    if (!sequence) { clearCpu(); rejectGpu(*c, c->waitSequence); snapshot.invalid = true; }
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
            static_cast<unsigned>(edvr::GpuFrameEvent::WaitEnd), sequence, 0, edvr::kGpuFrameNativeWait, nullptr);
    c->waitOutstanding = false;
    c->waitValid = ok;
    c->published = false;
    ++(ok ? c->validCount : c->invalidCount);
    if (!ok) {
        queueCompletion(*c, sequence, true);
        poison(sequence);
        rejectGpu(*c, sequence);
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
    if (eye == 2u) {
        // Reserved CPU wall marker. All intervals are measured on the bound
        // producer thread; the route caller is the producer in native mode.
        if (begin > 1 || accepted > 1 || texture || !c->waitValid || c->published ||
            !c->applicationValid || GetCurrentThreadId() != c->producer) return 0;
        if (begin) {
            if (c->applicationOpen || !(c->applicationQpc = qpcNow())) return 0;
            c->applicationOpen = true;
        } else {
            if (!c->applicationOpen) return 0;
            const int64_t end = qpcNow(); double elapsed = 0;
            if (!qpcMs(c->applicationQpc, end, elapsed)) {
                c->applicationValid = false; c->applicationOpen = false; return 0;
            }
            c->applicationMs += elapsed;
            ++c->applicationSegments;
            c->applicationQpc = 0; c->applicationOpen = false;
        }
        return 1;
    }
    if (eye == 3u || eye == 4u || (eye >= 5u && eye <= 6u)) {
        if (GetCurrentThreadId() != c->producer) return 0;
        if (eye >= 5u) {
            if (begin != 0 || accepted != 1 || !texture || !c->waitValid || c->published) return 0;
            return edvrGpuFrameEvent(edvr::kGpuFrameProtocol,
                static_cast<unsigned>(edvr::GpuFrameEvent::SegmentEnd), sequence,
                eye - 5u, 1u, texture) ? 1u : 0u;
        }
        if (begin != (eye == 3u ? 1u : 0u) || accepted != 1 || texture ||
            !c->waitValid || c->published) return 0;
        const auto event = eye == 3u ? edvr::GpuFrameEvent::SegmentResume :
                                      edvr::GpuFrameEvent::SegmentPause;
        return edvrGpuFrameEvent(edvr::kGpuFrameProtocol,
            static_cast<unsigned>(event), sequence, 0, 0, nullptr) ? 1u : 0u;
    }
    if (GetCurrentThreadId() != c->producer || eye > 1 || begin > 1 || accepted > 1 || !texture) {
        poison(sequence); rejectGpu(*c, sequence); return 0;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
    Microsoft::WRL::ComPtr<IUnknown> a, b;
    texture->GetDevice(&sourceDevice);
    const bool same = sourceDevice && SUCCEEDED(c->device->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(sourceDevice->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
    if (!same) { poison(sequence); rejectGpu(*c, sequence); return 0; }
    const unsigned bit = 1u << eye;
    if (begin) {
        if ((c->attempted & bit) || c->opened) { poison(sequence); rejectGpu(*c, sequence); return 0; }
        c->attempted |= bit;
    } else {
        if (!(c->opened & bit) || (c->ended & bit)) { poison(sequence); rejectGpu(*c, sequence); return 0; }
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
        frame->size != sizeof(*frame) || frame->version != EDVR_NATIVE_TIMING_VERSION_3 ||
        !c->waitValid || c->published || frame->sequence != c->waitSequence) return E_INVALIDARG;
    bool fieldsValid = finiteNonnegative(frame->composeMs);
    for (unsigned eye = 0; eye < 2; ++eye) fieldsValid = fieldsValid &&
        finiteNonnegative(frame->submitMs[eye]) && finiteNonnegative(frame->temporalMs[eye]) &&
        finiteNonnegative(frame->menuMs[eye]) && finiteNonnegative(frame->transferMs[eye]);
    if (!fieldsValid) {
        queueCompletion(*c, frame->sequence, true);
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
    snapshot.applicationMs = c->applicationMs;
    // Native routing has four required wall intervals: initial game work,
    // left treatment, between-eye route/game work, and right treatment.
    // Direct-owner calls and failed route returns cannot publish a plausible
    // partial application measurement.
    snapshot.applicationValid = c->applicationValid && !c->applicationOpen &&
        c->applicationSegments == kApplicationSegments;
    c->published = true;
    queueCompletion(*c, frame->sequence, false);
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
    if (!c->published) queueCompletion(*c, c->waitSequence, true);
    poison(c->waitSequence);
    rejectGpu(*c, c->waitSequence);
    c->waitOutstanding = c->waitValid = c->published = false;
    c->applicationQpc = 0; c->applicationOpen = false; c->applicationValid = false;
    clearCpu(); snapshot.active = true; snapshot.generation = c->generation; snapshot.invalid = true;
    return S_OK;
}

uint32_t WINAPI gpuEnabled(void* p) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c || !c->active || c != current) return 0u;
    const bool enabled = edvr::Config::get().getBool("advanced.app_gpu_timing", true);
    if (!enabled) {
        clearDeviceGpu();
        // Older asynchronous work must not reappear after a disable/re-enable.
        if (c->waitSequence && c->waitSequence-1 > c->gpuRejectFloor)
            c->gpuRejectFloor = c->waitSequence-1;
        snapshot.deviceGpu = {sizeof(EdvrNativeDeviceGpuSample), EDVR_NATIVE_TIMING_VERSION_3,
            c->waitSequence, nowMs(), EdvrNativeGpuDisabled};
        snapshot.haveDeviceGpu = true;
    }
    return enabled ? 1u : 0u;
}

HRESULT WINAPI publishDeviceGpu(void* p, const EdvrNativeDeviceGpuSample* sample) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    const auto now = nowMs();
    if (!c || !c->active || c != current || !sample || sample->size != sizeof(*sample) ||
        sample->version != EDVR_NATIVE_TIMING_VERSION_3 || !sample->sequence || !c->firstSequence ||
        sample->sequence < c->firstSequence || sample->sequence > c->waitSequence ||
        sample->sequence <= c->gpuRejectFloor ||
        sample->sequence < c->lastGpuSequence ||
        (sample->sequence == c->lastGpuSequence &&
         !(snapshot.haveDeviceGpu && snapshot.deviceGpu.status == EdvrNativeGpuPending &&
           sample->status == EdvrNativeGpuValid)) ||
        !sample->completedAtMs || sample->completedAtMs > now ||
        (now - sample->completedAtMs > 2000 && sample->status != EdvrNativeGpuStale) ||
        (!edvr::Config::get().getBool("advanced.app_gpu_timing", true) && sample->status != EdvrNativeGpuDisabled) ||
        sample->status > EdvrNativeGpuNotSeparate)
        return E_INVALIDARG;
    for (double value : sample->transferMs)
        if (!finiteNonnegative(value)) return E_INVALIDARG;
    for (double value : sample->composeMs)
        if (!finiteNonnegative(value)) return E_INVALIDARG;
    c->lastGpuSequence = sample->sequence;
    if (sample->status != EdvrNativeGpuPending &&
        (!c->lastGpuLogMs || now - c->lastGpuLogMs >= 5000 ||
         (sample->status == EdvrNativeGpuValid && !c->gpuLoggedValid))) {
        c->lastGpuLogMs = now;
        if (sample->status == EdvrNativeGpuValid) c->gpuLoggedValid = true;
        edvr::Log::get().note("native device GPU: %s, seq %llu, age %llu ms, transfer %.3f/%.3f ms, compose %.3f/%.3f ms; spans exclude producer and runtime compositor.",
            gpuStatusName(sample->status), (unsigned long long)sample->sequence,
            (unsigned long long)(now - sample->completedAtMs), sample->transferMs[0], sample->transferMs[1],
            sample->composeMs[0], sample->composeMs[1]);
    }
    if (sample->status == EdvrNativeGpuDisabled) {
        snapshot.deviceGpu = *sample;
        snapshot.haveDeviceGpu = true;
        return S_OK;
    }
    snapshot.deviceGpu = *sample;
    snapshot.haveDeviceGpu = true;
    return S_OK;
}

HRESULT WINAPI close(void* p) {
    std::lock_guard<std::mutex> lock(lifetime);
    Context* c = identify(p);
    if (!c) return E_INVALIDARG;
    if (!c->active) return S_FALSE;
    if (!c->published) queueCompletion(*c, c->waitSequence, true);
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
unsigned nativeTimingReadCompletions(uint64_t& cursor, NativeTimingSnapshot* out,
                                     unsigned capacity, uint64_t& dropped) noexcept {
    if (!out || !capacity) return 0;
    std::lock_guard<std::mutex> lock(lifetime);
    const uint64_t latest = nextCompletion ? nextCompletion - 1 : 0;
    const uint64_t oldest = latest >= kCompletionCapacity ?
        latest - kCompletionCapacity + 1 : 1;
    const uint64_t next = cursor == (std::numeric_limits<uint64_t>::max)() ?
        (std::numeric_limits<uint64_t>::max)() : cursor + 1;
    if (next < oldest) { dropped += oldest - next; cursor = oldest - 1; }
    const uint64_t start = (std::max)(cursor == (std::numeric_limits<uint64_t>::max)() ?
        (std::numeric_limits<uint64_t>::max)() : cursor + 1, oldest);
    const uint64_t available = latest >= start ? latest - start + 1 : 0;
    const unsigned count = static_cast<unsigned>((std::min)(uint64_t(capacity), available));
    for (unsigned i = 0; i < count; ++i) {
        const auto& item = completions[(start + i) % kCompletionCapacity];
        if (item.ordinal != start + i) { ++dropped; cursor = start + i; continue; }
        out[i] = item.value;
        cursor = item.ordinal;
    }
    return count;
}
}

extern "C" HRESULT WINAPI edvrAcquireNativeTiming(const EdvrNativeTimingRequest* request,
                                                    EdvrNativeTimingTable* table) {
    if (!table || table->size != sizeof(*table) || table->version != EDVR_NATIVE_TIMING_VERSION_3)
        return E_INVALIDARG;
    *table = {sizeof(*table), EDVR_NATIVE_TIMING_VERSION_3};
    if (!request || request->size != sizeof(*request) || request->version != EDVR_NATIVE_TIMING_VERSION_3 ||
        !request->device || !request->generation) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(lifetime);
    if (current || used == kCapacity) return E_PENDING;
    Context& c = pool[used++];
    c = {}; c.active = true; c.device = request->device; c.producer = GetCurrentThreadId();
    c.generation = request->generation; current = &c;
    snapshot = {}; snapshot.active = true; snapshot.generation = c.generation;
    table->context = &c; table->waitBegin = waitBegin; table->waitEnd = waitEnd;
    table->gpuEye = gpuEye; table->publishCpu = publishCpu; table->invalidate = invalidate; table->close = close;
    table->gpuEnabled = gpuEnabled; table->publishDeviceGpu = publishDeviceGpu;
    return S_OK;
}
