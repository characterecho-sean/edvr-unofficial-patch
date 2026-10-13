#include "xess_engine.h"

#include <windows.h>
#include <d3d11.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../common/log.h"
#include "dlaa.h"  // kUpscalerSlots: one XeSS feature per upscaler slot

#ifndef EDVR_HAVE_XESS
#define EDVR_HAVE_XESS 0
#endif

#if EDVR_HAVE_XESS
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <xess/xess_d3d12.h>
#include "gpu_timing.h"
#include "xess_feature_latch.h"
#include "xess_runtime_version.h"
#include "temporal_shader_bytecode.h"
using Microsoft::WRL::ComPtr;
#endif

namespace edvr {
namespace {
char g_reason[320] = "XeSS has not been checked";
bool g_quietFeatureFailure = false;

struct QuietFeatureFailure {
    const bool prior = g_quietFeatureFailure;
    QuietFeatureFailure() { g_quietFeatureFailure = true; }
    ~QuietFeatureFailure() { g_quietFeatureFailure = prior; }
};

bool fail(const char** why, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_reason, sizeof(g_reason), format, args);
    va_end(args);
    g_reason[sizeof(g_reason) - 1] = '\0';
    if (why) *why = g_reason;
    if (!g_quietFeatureFailure) Log::get().note("xess: refused: %s", g_reason);
    return false;
}

#if EDVR_HAVE_XESS
constexpr unsigned kCommandRing = 3;
constexpr DWORD kGpuWaitMs = 5000;
// Only this device ever takes a shared texture's key, always key 0, so an
// acquire is immediate; one that waits at all is a fault, not contention.
constexpr DWORD kKeyWaitMs = 100;

using CreateD3D12DeviceFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

struct XessApi {
    decltype(&xessGetVersion) getVersion = nullptr;
    decltype(&xessGetIntelXeFXVersion) getIntelVersion = nullptr;
    decltype(&xessGetOptimalInputResolution) getOptimalInput = nullptr;
    decltype(&xessD3D12CreateContext) createContext = nullptr;
    decltype(&xessD3D12Init) init = nullptr;
    decltype(&xessD3D12Execute) execute = nullptr;
    decltype(&xessDestroyContext) destroyContext = nullptr;
};

struct SharedTexture {
    ComPtr<ID3D11Texture2D> d11;
    ComPtr<IDXGIKeyedMutex> key;  // null only where the runtime exposes none
    ComPtr<ID3D12Resource> d12;
    HANDLE handle = nullptr;
    SharedTexture() = default;
    SharedTexture(const SharedTexture&) = delete;
    SharedTexture& operator=(const SharedTexture&) = delete;
    ~SharedTexture() { if (handle) CloseHandle(handle); }
};

struct CommandSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t doneValue = 0;
    // Bridge timing: two timestamps around xessD3D12Execute, resolved into this allocator's readback buffer. The buffer is
    // mapped for the slot's life. timingPending is set once a list that resolves into it has been submitted, and cleared
    // only after the fence has passed that submission, so the readback is never read early or read twice.
    ComPtr<ID3D12QueryHeap> timingHeap;
    ComPtr<ID3D12Resource> timingReadback;
    const uint64_t* ticks = nullptr;
    bool timingPending = false;
};

constexpr uint64_t kBridgeReportMs = 30000;   // one line per slot per window
constexpr size_t kBridgeCap = 8192;           // samples kept per window and metric
constexpr unsigned kBridgeSets = 4;           // D3D11 evaluations in flight per slot before timing goes untimed
// D3D11 intervals per evaluation. kBridgeTotal spans all of them; the other three are its parts, in order.
constexpr unsigned kBridgeTotal = 0, kBridgeCopyIn = 1, kBridgeWait = 2, kBridgeCopyOut = 3, kBridgeParts = 4;
constexpr unsigned kBridgeAll = (1u << kBridgeParts) - 1;

// One evaluation's D3D11 timers. Begun inside the temporal pass's own interval they borrow its disjoint scope, so the
// nesting costs no second disjoint query. The set is read a few evaluations later, once every part is ready or invalid.
struct BridgeSet {
    GpuTimer timers[kBridgeParts];
    bool inUse = false;
    unsigned ready = 0;   // bit per part once read
    bool invalid = false;
    double ms[kBridgeParts] = {};
};

// The current window of one slot's bridge timing. Samples are milliseconds; `executed` counts evaluations that ran.
struct BridgeWindow {
    std::vector<double> parts[kBridgeParts];   // D3D11 samples, by part
    std::vector<double> execute;               // D3D12 execute samples
    uint64_t startedAt = 0;                    // GetTickCount64 at the window's first execution
    unsigned executed = 0;
    unsigned untimed = 0;                      // evaluations whose D3D11 set could not begin
    unsigned invalid = 0;                      // timings read back invalid
    // CPU spans per executed evaluation, milliseconds (QueryPerformanceCounter, no GPU queries).
    std::vector<double> cpuEval;               // entry to return
    std::vector<double> cpuAlloc;              // the allocator-reuse wait
    std::vector<double> cpuSubmit;             // the queue Wait, then ExecuteCommandLists through Signal
    unsigned allocBlocked = 0;                 // evaluations whose allocator fence had not passed when reused
};

struct Feature {
    unsigned slot = 0;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
    bool hdr = false;
    xess_quality_settings_t quality = XESS_QUALITY_SETTING_PERFORMANCE;
    xess_context_handle_t context = nullptr;
    SharedTexture color, motion, depth, output;
    ComPtr<ID3D11ShaderResourceView> outputSrv;
    ComPtr<ID3D11ComputeShader> wideningShader;
    std::array<CommandSlot, kCommandRing> commands;
    unsigned nextCommand = 0;
    uint64_t lastUseValue = 0;
};

// One per upscaler slot (dlaa.h, kUpscalerSlots), as fsr3_engine.cpp keeps one context per slot. Each slot keys,
// builds, releases and latches its own feature; the shared textures hang off the feature, so they are per slot too.
struct FeatureSlot {
    XessFeatureFailureLatch failedFeature;
    std::unique_ptr<Feature> feature;
    uint32_t creations = 0;  // test seam: features this slot has built since the backend started
    // The slot's price, as CPU time for each successful evaluate call (submission and its copies; the GPU is not timed).
    uint32_t evaluations = 0, resets = 0;
    double timeSumMs = 0.0, timeMaxMs = 0.0;
    // Bridge timing lives per slot, so it outlives a feature rebuild. The sets are reset on the owner context at shutdown.
    std::array<BridgeSet, kBridgeSets> bridge;
    BridgeWindow window;
    char d12Note[96] = "";   // why this slot's D3D12 execute timestamps are absent, when they are
};

struct State {
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> ownerContext;
    DWORD ownerThread = 0;
    ComPtr<ID3D11Device5> d11v5;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    HMODULE d3d12Module = nullptr;
    HMODULE xessModule = nullptr;
    HANDLE sharedFenceHandle = nullptr;
    HANDLE cpuEvent = nullptr;
    XessApi api;
    // One counter for every slot: all slots signal the one shared fence in this order, so a wait on a slot's own
    // value also covers the other slots' earlier work, and the value never goes backwards.
    uint64_t nextFenceValue = 0;
    uint64_t d12TimestampHz = 0;   // queue ticks per second; 0 when the queue reports none (bridge timing then off)
    uint32_t evaluations = 0, resets = 0, failures = 0;
    bool untrackedSubmission = false;
    bool untrackedRefusalLogged = false;
    std::array<FeatureSlot, kUpscalerSlots> slots;
    ~State() {
        // Normal shutdown has already destroyed the XeSS context after the
        // consumer fence. Release COM objects before unloading the modules
        // that supply their vtables, including on an availability failure.
        for (FeatureSlot& slot : slots) slot.feature.reset();
        fence11.Reset();
        fence12.Reset();
        queue.Reset();
        d12.Reset();
        ownerContext.Reset();
        d11v5.Reset();
        d11.Reset();
        if (cpuEvent) CloseHandle(cpuEvent);
        if (sharedFenceHandle) CloseHandle(sharedFenceHandle);
        if (xessModule) FreeLibrary(xessModule);
        if (d3d12Module) FreeLibrary(d3d12Module);
    }
};

// A raw owner on purpose. A static destructor would run at DLL_PROCESS_DETACH
// on process exit, releasing D3D12 objects and FreeLibrary-ing libxess.dll and
// d3d12.dll under the loader lock after the game's other threads are gone;
// DllMain forbids FreeLibrary during process termination, and the proxy's rule
// there is "do the minimum and leak the rest" (d3d11_proxy.cpp). The
// FreeLibrary-unload teardown leaks it too: that path can hold the loader lock
// and must issue no context commands (device_hook.cpp, shutdownDeviceHooks),
// while xessShutdown signals, flushes and waits. The render-thread device-
// change path and rigs may call it; loader-lock teardown must not.
State* g_state = nullptr;
bool g_tried = false;
bool g_available = false;

bool returnLatchedFailure(const XessFeatureFailureLatch& latch, const char** why) {
    // The caller may retain this pointer after a device change destroys State.
    std::snprintf(g_reason, sizeof(g_reason), "%s", latch.reason());
    if (why) *why = g_reason;
    return false;
}

template <typename T> bool loadProc(HMODULE module, const char* name, T& target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

bool waitFor(State& state, uint64_t value, const char** why, const char* stage) {
    if (!value) return true;
    uint64_t completed = state.fence12->GetCompletedValue();
    if (completed == UINT64_MAX)
        return fail(why, "%s: D3D12 fence reports device removal (reason 0x%08X)",
                    stage, unsigned(state.d12->GetDeviceRemovedReason()));
    if (completed >= value) return true;
    ResetEvent(state.cpuEvent);
    const HRESULT hr = state.fence12->SetEventOnCompletion(value, state.cpuEvent);
    if (FAILED(hr)) return fail(why, "%s: SetEventOnCompletion failed 0x%08X", stage, unsigned(hr));
    const DWORD result = WaitForSingleObject(state.cpuEvent, kGpuWaitMs);
    completed = state.fence12->GetCompletedValue();
    if (completed == UINT64_MAX)
        return fail(why, "%s: D3D12 device removed while waiting (reason 0x%08X)",
                    stage, unsigned(state.d12->GetDeviceRemovedReason()));
    if (result != WAIT_OBJECT_0 || completed < value)
        return fail(why, "%s: GPU fence %llu did not complete within %lu ms (wait=%lu completed=%llu)",
                    stage, static_cast<unsigned long long>(value), static_cast<unsigned long>(kGpuWaitMs),
                    static_cast<unsigned long>(result), static_cast<unsigned long long>(completed));
    return true;
}

// Bridge timing. Everything below reads results the GPU has already finished and waits on nothing.

bool summarize(const std::vector<double>& samples, double& median, double& p95) {
    if (samples.empty()) return false;
    std::vector<double> sorted(samples);
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    median = (n % 2) ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
    p95 = sorted[(n * 95 + 99) / 100 - 1];   // nearest rank
    return true;
}

void noteSample(std::vector<double>& samples, double ms) {
    if (samples.size() < kBridgeCap) samples.push_back(ms);
}

// Milliseconds between two QueryPerformanceCounter readings.
double qpcSpanMs(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart > 0
        ? static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)
        : 0.0;
}

// One allocator's execute timestamps, read once the fence has passed the submission that wrote them.
void collectExecute(State& state, BridgeWindow& window, CommandSlot& command) {
    if (!command.timingPending) return;
    const uint64_t completed = state.fence12->GetCompletedValue();
    if (completed == UINT64_MAX || command.doneValue > completed) return;
    command.timingPending = false;
    const uint64_t begin = command.ticks[0], end = command.ticks[1];
    if (!state.d12TimestampHz || !begin || end < begin) {
        ++window.invalid;
        return;
    }
    noteSample(window.execute, double(end - begin) * 1000.0 / double(state.d12TimestampHz));
}

// Polls one slot's D3D11 sets without waiting. A set is read once all four parts are ready or invalid; until then it
// stays in flight. Returns the sets still waiting on the GPU.
unsigned pollBridge(ID3D11DeviceContext* context, FeatureSlot& holder) {
    unsigned waiting = 0;
    for (BridgeSet& set : holder.bridge) {
        if (!set.inUse) continue;
        for (unsigned part = 0; part < kBridgeParts; ++part) {
            if (set.ready & (1u << part)) continue;
            double ms = 0.0;
            const GpuTimerPoll result = set.timers[part].poll(context, ms);
            if (result == GpuTimerPoll::Pending) continue;
            set.ready |= 1u << part;
            if (result == GpuTimerPoll::Ready) set.ms[part] = ms;
            else set.invalid = true;
        }
        if (set.ready != kBridgeAll) {
            ++waiting;
            continue;
        }
        set.inUse = false;
        if (set.invalid) ++holder.window.invalid;
        else for (unsigned part = 0; part < kBridgeParts; ++part) noteSample(holder.window.parts[part], set.ms[part]);
    }
    return waiting;
}

void sweepBridge(State& state, unsigned slot, ID3D11DeviceContext* context) {
    FeatureSlot& holder = state.slots[slot];
    if (holder.feature)
        for (CommandSlot& command : holder.feature->commands) collectExecute(state, holder.window, command);
    pollBridge(context, holder);
}

// Evaluations whose timing is still on the GPU: D3D11 sets not yet read, or D3D12 executes whose fence has not passed.
unsigned bridgeUnread(const FeatureSlot& holder) {
    unsigned sets = 0, executes = 0;
    for (const BridgeSet& set : holder.bridge) sets += set.inUse ? 1u : 0u;
    if (holder.feature)
        for (const CommandSlot& command : holder.feature->commands) executes += command.timingPending ? 1u : 0u;
    return sets > executes ? sets : executes;
}

void pairText(char* text, size_t size, bool has, double median, double p95) {
    if (has) std::snprintf(text, size, "%.3f/%.3f", median, p95);
    else std::snprintf(text, size, "n/a");
}

// Builds the slot's line from its window and starts a new window. Reads still outstanding count as unread and land in
// the next window. An unavailable part is named, never printed as zero.
void reportSlot(State& state, unsigned slot, XessBridgeTiming& out) {
    FeatureSlot& holder = state.slots[slot];
    BridgeWindow& window = holder.window;
    double median[kBridgeParts] = {}, p95[kBridgeParts] = {};
    bool has[kBridgeParts] = {};
    for (unsigned part = 0; part < kBridgeParts; ++part)
        has[part] = summarize(window.parts[part], median[part], p95[part]);
    double executeMedian = 0.0, executeP95 = 0.0;
    const bool hasExecute = summarize(window.execute, executeMedian, executeP95);

    out = XessBridgeTiming{};
    out.d11Samples = unsigned(window.parts[kBridgeTotal].size());
    out.d12Samples = unsigned(window.execute.size());
    out.evals = out.d11Samples > out.d12Samples ? out.d11Samples : out.d12Samples;
    out.unread = bridgeUnread(holder);
    out.copyInMedian = median[kBridgeCopyIn]; out.copyInP95 = p95[kBridgeCopyIn];
    out.waitMedian = median[kBridgeWait];     out.waitP95 = p95[kBridgeWait];
    out.executeMedian = executeMedian;        out.executeP95 = executeP95;
    out.copyOutMedian = median[kBridgeCopyOut]; out.copyOutP95 = p95[kBridgeCopyOut];
    out.totalMedian = median[kBridgeTotal];   out.totalP95 = p95[kBridgeTotal];
    double evalMedian = 0.0, evalP95 = 0.0, allocMedian = 0.0, allocP95 = 0.0, submitMedian = 0.0, submitP95 = 0.0;
    const bool hasEval = summarize(window.cpuEval, evalMedian, evalP95);
    const bool hasAlloc = summarize(window.cpuAlloc, allocMedian, allocP95);
    const bool hasSubmit = summarize(window.cpuSubmit, submitMedian, submitP95);
    out.cpuEvals = unsigned(window.cpuEval.size());
    out.blocked = window.allocBlocked;
    out.evalMedian = evalMedian; out.evalP95 = evalP95;
    out.allocMedian = allocMedian; out.allocP95 = allocP95;
    out.submitMedian = submitMedian; out.submitP95 = submitP95;
    out.submitTimed = hasSubmit;

    char copyIn[32], wait[32], execute[32], copyOut[32], total[32];
    char evalText[32], allocText[32], submitText[32];
    pairText(evalText, sizeof evalText, hasEval, evalMedian, evalP95);
    pairText(allocText, sizeof allocText, hasAlloc, allocMedian, allocP95);
    pairText(submitText, sizeof submitText, hasSubmit, submitMedian, submitP95);
    const std::string submitPart = hasSubmit ? std::string(", submit ") + submitText : std::string();
    pairText(copyIn, sizeof copyIn, has[kBridgeCopyIn], median[kBridgeCopyIn], p95[kBridgeCopyIn]);
    pairText(wait, sizeof wait, has[kBridgeWait], median[kBridgeWait], p95[kBridgeWait]);
    pairText(execute, sizeof execute, hasExecute, executeMedian, executeP95);
    pairText(copyOut, sizeof copyOut, has[kBridgeCopyOut], median[kBridgeCopyOut], p95[kBridgeCopyOut]);
    pairText(total, sizeof total, has[kBridgeTotal], median[kBridgeTotal], p95[kBridgeTotal]);
    std::string reasons;
    if (window.untimed) reasons += ", d3d11 untimed " + std::to_string(window.untimed);
    if (window.invalid) reasons += ", invalid " + std::to_string(window.invalid);
    if (!hasExecute && holder.d12Note[0]) reasons += std::string(", d3d12 ") + holder.d12Note;
    std::snprintf(out.line, sizeof out.line,
                  "xess: slot=%u bridge timing over %u evals, ms median/p95: copy-in %s, wait %s, "
                  "d3d12 execute %s, copy-out %s, d3d11 total %s (unread %u%s); "
                  "cpu ms median/p95: eval %s, alloc-wait %s (blocked %u of %u)%s",
                  slot, out.evals, copyIn, wait, execute, copyOut, total, out.unread, reasons.c_str(),
                  evalText, allocText, window.allocBlocked, window.executed, submitPart.c_str());
    Log::get().note("%s", out.line);

    for (std::vector<double>& samples : window.parts) samples.clear();
    window.execute.clear();
    window.cpuEval.clear();
    window.cpuAlloc.clear();
    window.cpuSubmit.clear();
    window.startedAt = 0;
    window.executed = 0;
    window.untimed = 0;
    window.invalid = 0;
    window.allocBlocked = 0;
}

// Starts an evaluation's D3D11 intervals: the whole bridge, then copy-in. A set that cannot begin counts as untimed and the
// evaluation runs without one. Nothing here waits or flushes.
BridgeSet* beginBridge(FeatureSlot& holder, ID3D11Device* device, ID3D11DeviceContext* context) {
    for (BridgeSet& set : holder.bridge) {
        if (set.inUse) continue;
        if (!set.timers[kBridgeTotal].begin(device, context)) break;
        set.inUse = true;
        set.ready = 0;
        set.invalid = false;
        set.timers[kBridgeCopyIn].begin(device, context);   // a refused begin reads back Invalid
        return &set;
    }
    ++holder.window.untimed;
    return nullptr;
}

// Cancels an evaluation's D3D11 intervals when it fails before the whole bridge has ended, so no interval stays open on
// the owner context. Disarmed once the whole bridge has ended; the set is then read by pollBridge.
class BridgeGuard {
    BridgeSet* set_;
    ID3D11DeviceContext* context_;
public:
    BridgeGuard(BridgeSet* set, ID3D11DeviceContext* context) noexcept : set_(set), context_(context) {}
    ~BridgeGuard() {
        if (!set_) return;
        for (unsigned part = kBridgeParts; part-- > 0;) set_->timers[part].reset(context_);
        set_->inUse = false;
    }
    BridgeGuard(const BridgeGuard&) = delete;
    BridgeGuard& operator=(const BridgeGuard&) = delete;
    void disarm() noexcept { set_ = nullptr; }
};

// Resets every bridge timer on the owner context. Only a drained backend gets here, so no timer holds an open interval.
void cancelBridgeTimers(State& state, ID3D11DeviceContext* context) {
    for (FeatureSlot& holder : state.slots)
        for (BridgeSet& set : holder.bridge)
            for (unsigned part = kBridgeParts; part-- > 0;) set.timers[part].reset(context);
}

bool releaseSlot(State& state, unsigned slot, const char** why) {
    std::unique_ptr<Feature>& feature = state.slots[slot].feature;
    if (!feature) return true;
    if (state.untrackedSubmission)
        return fail(why, "a submitted D3D12 command list has no completed fence; retaining its resources");
    if (!waitFor(state, feature->lastUseValue, why, "release")) return false;
    if (feature->context) {
        const xess_result_t rc = state.api.destroyContext(feature->context);
        Log::get().note("xess: slot=%u destroy context rc=%d after %u evaluations", slot, int(rc), state.evaluations);
        feature->context = nullptr;
    }
    // The fence has passed every submission, so their execute timestamps are final; read them before the readback goes.
    for (CommandSlot& command : feature->commands) collectExecute(state, state.slots[slot].window, command);
    feature.reset();
    return true;
}

bool makeShared(State& state, unsigned slot, SharedTexture& target, const char* role,
                uint32_t width, uint32_t height, DXGI_FORMAT format,
                bool writable, const char** why) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    // Both flags are required for the NT handle path used by the measured
    // DXVK/vkd3d-proton bridge. Render-target bind is valid on all four tested
    // formats; output also needs D3D12 unordered access.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET |
                     (writable ? D3D11_BIND_UNORDERED_ACCESS : 0);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = state.d11->CreateTexture2D(&desc, nullptr, target.d11.GetAddressOf());
    if (FAILED(hr)) return fail(why, "shared %s D3D11 texture %ux%u format=%u failed 0x%08X",
                                role, width, height, unsigned(format), unsigned(hr));
    // The native D3D11 runtime enforces the keyed mutex: it drops any copy or
    // dispatch that touches the texture while this device holds no key, and
    // XeSS then reads and returns zeros (xess_engine_test --gpu-test failed
    // so on an RTX 5090 under Windows 11; DXVK does not enforce it). Every D3D11
    // access takes key 0 (takeKeys); D3D12 never takes it, and the shared
    // fences alone order the two APIs.
    if (FAILED(target.d11.As(&target.key))) target.key.Reset();
    ComPtr<IDXGIResource1> resource;
    hr = target.d11.As(&resource);
    if (FAILED(hr)) return fail(why, "shared %s IDXGIResource1 unavailable 0x%08X", role, unsigned(hr));
    hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                      nullptr, &target.handle);
    if (FAILED(hr) || !target.handle)
        return fail(why, "shared %s NT handle failed 0x%08X", role, unsigned(hr));
    hr = state.d12->OpenSharedHandle(target.handle, IID_PPV_ARGS(target.d12.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "shared %s D3D12 open failed 0x%08X", role, unsigned(hr));
    Log::get().note("xess: slot=%u shared %s %ux%u format=%u D3D11->D3D12 open ok",
                    slot, role, width, height, unsigned(format));
    return true;
}

// AcquireSync reports WAIT_TIMEOUT and WAIT_ABANDONED as success HRESULTs, so
// only S_OK means the key is held. A failed take gives back what it took.
template <size_t N>
bool takeKeys(SharedTexture* const (&textures)[N], const char** why, const char* stage) {
    for (size_t i = 0; i < N; ++i) {
        IDXGIKeyedMutex* key = textures[i]->key.Get();
        if (!key) continue;
        const HRESULT hr = key->AcquireSync(0, kKeyWaitMs);
        if (hr == S_OK) continue;
        if (hr == static_cast<HRESULT>(WAIT_ABANDONED)) key->ReleaseSync(0);
        while (i--) if (textures[i]->key) textures[i]->key->ReleaseSync(0);
        return fail(why, "%s: keyed mutex AcquireSync(0) returned 0x%08X", stage, unsigned(hr));
    }
    return true;
}

template <size_t N>
void giveKeys(SharedTexture* const (&textures)[N]) {
    for (SharedTexture* texture : textures)
        if (texture->key) texture->key->ReleaseSync(0);
}

bool qualityFor(State& state, Feature& feature, const char** why) {
    const xess_2d_t output{feature.outW, feature.outH};
    struct Choice { xess_quality_settings_t setting; const char* name; };
    constexpr Choice choices[] = {
        {XESS_QUALITY_SETTING_AA, "AA"},
        {XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, "Ultra Quality Plus"},
        {XESS_QUALITY_SETTING_ULTRA_QUALITY, "Ultra Quality"},
        {XESS_QUALITY_SETTING_QUALITY, "Quality"},
        {XESS_QUALITY_SETTING_BALANCED, "Balanced"},
        {XESS_QUALITY_SETTING_PERFORMANCE, "Performance"},
        {XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, "Ultra Performance"},
    };
    double best = std::numeric_limits<double>::infinity();
    const Choice* selected = nullptr;
    xess_2d_t selectedOptimal{}, selectedMin{}, selectedMax{};
    for (const Choice& candidate : choices) {
        if (candidate.setting == XESS_QUALITY_SETTING_AA &&
            (feature.inW != feature.outW || feature.inH != feature.outH)) continue;
        xess_2d_t optimal{}, minimum{}, maximum{};
        const xess_result_t rc = state.api.getOptimalInput(feature.context, &output,
                                                            candidate.setting, &optimal,
                                                            &minimum, &maximum);
        if (rc != XESS_RESULT_SUCCESS) continue;
        if (feature.inW < minimum.x || feature.inW > maximum.x ||
            feature.inH < minimum.y || feature.inH > maximum.y) continue;
        const double dx = (double(feature.inW) - optimal.x) / feature.outW;
        const double dy = (double(feature.inH) - optimal.y) / feature.outH;
        const double distance = dx * dx + dy * dy;
        if (distance < best) {
            best = distance; selected = &candidate;
            selectedOptimal = optimal; selectedMin = minimum; selectedMax = maximum;
        }
    }
    if (!selected) return fail(why, "no XeSS quality accepts input %ux%u for output %ux%u",
                               feature.inW, feature.inH, feature.outW, feature.outH);
    feature.quality = selected->setting;
    Log::get().note("xess: slot=%u quality=%s input=%ux%u output=%ux%u optimal=%ux%u bounds=%ux%u..%ux%u",
                    feature.slot, selected->name, feature.inW, feature.inH, feature.outW, feature.outH,
                    selectedOptimal.x, selectedOptimal.y, selectedMin.x, selectedMin.y,
                    selectedMax.x, selectedMax.y);
    return true;
}

// The bridge's D3D12 timestamps for one allocator: a two-entry timestamp heap and a readback buffer for its resolve. A
// failure leaves the allocator untimed and names the reason in the slot's note. It never fails the feature.
void makeTiming(State& state, FeatureSlot& holder, CommandSlot& command) {
    if (!state.d12TimestampHz) {
        std::snprintf(holder.d12Note, sizeof(holder.d12Note), "queue reports no timestamp frequency");
        return;
    }
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = 2;
    ComPtr<ID3D12QueryHeap> heap;
    HRESULT hr = state.d12->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(heap.GetAddressOf()));
    if (FAILED(hr)) {
        std::snprintf(holder.d12Note, sizeof(holder.d12Note), "query heap failed 0x%08X", unsigned(hr));
        return;
    }
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = 2 * sizeof(uint64_t);
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    hr = state.d12->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(readback.GetAddressOf()));
    if (FAILED(hr)) {
        std::snprintf(holder.d12Note, sizeof(holder.d12Note), "readback buffer failed 0x%08X", unsigned(hr));
        return;
    }
    void* mapped = nullptr;
    hr = readback->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        std::snprintf(holder.d12Note, sizeof(holder.d12Note), "readback map failed 0x%08X", unsigned(hr));
        return;
    }
    command.timingHeap = heap;
    command.timingReadback = readback;
    command.ticks = static_cast<const uint64_t*>(mapped);
}

bool makeFeature(State& state, unsigned slot, uint32_t w, uint32_t h, uint32_t outW,
                 uint32_t outH, bool hdr, const char** why) {
    auto feature = std::make_unique<Feature>();
    feature->slot = slot;
    feature->inW = w; feature->inH = h; feature->outW = outW;
    feature->outH = outH; feature->hdr = hdr;
    const xess_result_t created = state.api.createContext(state.d12.Get(), &feature->context);
    if (created != XESS_RESULT_SUCCESS || !feature->context)
        return fail(why, "xessD3D12CreateContext returned %d", int(created));
    Log::get().note("xess: slot=%u D3D12 context create rc=%d ptr=%p", slot, int(created), feature->context);
    // From this point every failure destroys the XeSS context before the
    // temporary shared textures and D3D12 objects leave scope.
    auto abandon = [&]() -> bool {
        state.api.destroyContext(feature->context);
        feature->context = nullptr;
        return false;
    };
    if (!qualityFor(state, *feature, why)) return abandon();
    xess_d3d12_init_params_t init{};
    init.outputResolution = xess_2d_t{outW, outH};
    init.qualitySetting = feature->quality;
    // EDVR's depth is reversed infinite-Z and its RG16F motion is low-res,
    // current->previous in pixels. XeSS needs no NDC/JITTERED_MV/HIGH_RES_MV
    // flags for these measured inputs. HDR has no external exposure image.
    init.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH |
                     (hdr ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : XESS_INIT_FLAG_LDR_INPUT_COLOR);
    const xess_result_t initialized = state.api.init(feature->context, &init);
    if (initialized != XESS_RESULT_SUCCESS) {
        fail(why, "xessD3D12Init returned %d", int(initialized));
        return abandon();
    }
    Log::get().note("xess: slot=%u D3D12 init rc=%d flags=0x%X quality=%u output=%ux%u",
                    slot, int(initialized), init.initFlags, unsigned(feature->quality), outW, outH);
    xess_version_t intel{};
    const xess_result_t intelRc = state.api.getIntelVersion(feature->context, &intel);
    Log::get().note("xess: slot=%u Intel XeFX query rc=%d version=%u.%u.%u.%u (diagnostic only)",
                    slot, int(intelRc), intel.major, intel.minor, intel.patch, intel.reserved);
    const DXGI_FORMAT colorFormat = hdr ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!makeShared(state, slot, feature->color, "color", w, h, colorFormat, false, why) ||
        !makeShared(state, slot, feature->motion, "motion", w, h, DXGI_FORMAT_R16G16_FLOAT, false, why) ||
        !makeShared(state, slot, feature->depth, "depth", w, h, DXGI_FORMAT_R32_FLOAT, false, why) ||
        !makeShared(state, slot, feature->output, "output", outW, outH, colorFormat, true, why))
        return abandon();
    if (hdr) {
        HRESULT hr = state.d11->CreateShaderResourceView(feature->output.d11.Get(), nullptr,
                                                          feature->outputSrv.GetAddressOf());
        if (FAILED(hr)) {
            fail(why, "R11 XeSS output SRV failed 0x%08X", unsigned(hr));
            return abandon();
        }
        hr = state.d11->CreateComputeShader(kXessR11ToFp16Bytecode,
                                             sizeof(kXessR11ToFp16Bytecode), nullptr,
                                             feature->wideningShader.GetAddressOf());
        if (FAILED(hr)) {
            fail(why, "precompiled R11->FP16 copy shader failed 0x%08X", unsigned(hr));
            return abandon();
        }
        Log::get().note("xess: slot=%u HDR R11 matched SDK output -> FP16 target via precompiled copy shader", slot);
    }
    state.slots[slot].d12Note[0] = '\0';
    for (CommandSlot& command : feature->commands) {
        HRESULT hr = state.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(command.allocator.GetAddressOf()));
        if (FAILED(hr)) {
            fail(why, "D3D12 command allocator failed 0x%08X", unsigned(hr));
            return abandon();
        }
        hr = state.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           command.allocator.Get(), nullptr,
                                           IID_PPV_ARGS(command.list.GetAddressOf()));
        if (FAILED(hr)) {
            fail(why, "D3D12 command list failed 0x%08X", unsigned(hr));
            return abandon();
        }
        hr = command.list->Close();
        if (FAILED(hr)) {
            fail(why, "D3D12 initial command close failed 0x%08X", unsigned(hr));
            return abandon();
        }
        makeTiming(state, state.slots[slot], command);
    }
    Log::get().note("xess: slot=%u three command allocators ready; backend feature established", slot);
    state.slots[slot].feature = std::move(feature);
    ++state.slots[slot].creations;
    return true;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

bool validateTexture(ID3D11Texture2D* texture, uint32_t width, uint32_t height,
                     DXGI_FORMAT format, const char* role, const char** why) {
    if (!texture) return fail(why, "%s texture is null", role);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height || desc.Format != format ||
        desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1)
        return fail(why, "%s texture contract: got %ux%u format=%u samples=%u mips=%u, need %ux%u format=%u",
                    role, desc.Width, desc.Height, unsigned(desc.Format), desc.SampleDesc.Count,
                    desc.MipLevels, width, height, unsigned(format));
    return true;
}

bool widenHdr(ID3D11DeviceContext* context, Feature& feature,
              ID3D11Texture2D* output, const char** why) {
    ComPtr<ID3D11Device> dev;
    context->GetDevice(dev.GetAddressOf());
    ComPtr<ID3D11UnorderedAccessView> target;
    HRESULT hr = dev->CreateUnorderedAccessView(output, nullptr, target.GetAddressOf());
    if (FAILED(hr)) return fail(why, "FP16 output UAV failed 0x%08X", unsigned(hr));
    ComPtr<ID3D11ComputeShader> oldShader;
    ComPtr<ID3D11ShaderResourceView> oldSrv;
    ComPtr<ID3D11UnorderedAccessView> oldUav;
    ID3D11ComputeShader* prior = nullptr;
    ID3D11ShaderResourceView* priorSrv = nullptr;
    ID3D11UnorderedAccessView* priorUav = nullptr;
    context->CSGetShader(&prior, nullptr, nullptr); oldShader.Attach(prior);
    context->CSGetShaderResources(0, 1, &priorSrv); oldSrv.Attach(priorSrv);
    context->CSGetUnorderedAccessViews(0, 1, &priorUav); oldUav.Attach(priorUav);
    ID3D11ShaderResourceView* source = feature.outputSrv.Get();
    ID3D11UnorderedAccessView* destination = target.Get();
    context->CSSetShaderResources(0, 1, &source);
    context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
    context->CSSetShader(feature.wideningShader.Get(), nullptr, 0);
    context->Dispatch((feature.outW + 7) / 8, (feature.outH + 7) / 8, 1);
    ID3D11ShaderResourceView* restoreSrv = oldSrv.Get();
    ID3D11UnorderedAccessView* restoreUav = oldUav.Get();
    context->CSSetShaderResources(0, 1, &restoreSrv);
    context->CSSetUnorderedAccessViews(0, 1, &restoreUav, nullptr);
    context->CSSetShader(oldShader.Get(), nullptr, 0);
    return true;
}

#endif  // EDVR_HAVE_XESS
}  // namespace

bool xessBuiltIn() { return EDVR_HAVE_XESS != 0; }

bool xessAvailable(ID3D11Device* device, const char** why) {
#if !EDVR_HAVE_XESS
    (void)device;
    return fail(why, "this build has no XeSS SDK (EDVR_HAVE_XESS=0)");
#else
    // flatRuntimeResize can clear its device pointer before it sees the new
    // one. The backend itself must detect a changed owner on every entry.
    if (device && g_available && g_state && g_state->d11.Get() != device) {
        Log::get().note("xess: D3D11 device changed; releasing old backend before rebind");
        xessShutdown();
    }
    if (g_tried) {
        if (g_available && g_state && g_state->d11.Get() == device) {
            if (why) *why = nullptr;
            return true;
        }
        if (g_available) return fail(why, "XeSS backend is bound to a different D3D11 device");
        if (why) *why = g_reason;
        return false;
    }
    g_tried = true;
    if (!device) return fail(why, "D3D11 device is null");
    auto state = std::make_unique<State>();
    state->d11 = device;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(state->d11v5.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "ID3D11Device5/shared fences unavailable 0x%08X", unsigned(hr));
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "IDXGIDevice unavailable 0x%08X", unsigned(hr));
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) return fail(why, "D3D11 adapter query failed 0x%08X", unsigned(hr));
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) return fail(why, "D3D11 adapter description failed 0x%08X", unsigned(hr));
    state->d3d12Module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!state->d3d12Module) return fail(why, "d3d12.dll unavailable (Win32=%lu)", GetLastError());
    CreateD3D12DeviceFn createD3D12 = nullptr;
    if (!loadProc(state->d3d12Module, "D3D12CreateDevice", createD3D12))
        return fail(why, "D3D12CreateDevice export unavailable");
    hr = createD3D12(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(state->d12.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "D3D12CreateDevice on D3D11 adapter failed 0x%08X", unsigned(hr));
    const LUID d12Luid = state->d12->GetAdapterLuid();
    if (d12Luid.HighPart != desc.AdapterLuid.HighPart || d12Luid.LowPart != desc.AdapterLuid.LowPart)
        return fail(why, "D3D11/D3D12 adapter LUID mismatch");
    Log::get().note("xess: D3D11/D3D12 adapter matched LUID=%08X:%08X vendor=0x%04X device=0x%04X",
                    unsigned(desc.AdapterLuid.HighPart), unsigned(desc.AdapterLuid.LowPart),
                    desc.VendorId, desc.DeviceId);
    // Resolve from this module, not the process working directory or an
    // already-loaded foreign copy. The rig resolves beside its executable.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&xessBuiltIn), &self) || !self)
        return fail(why, "EDVR module for libxess.dll could not be located (Win32=%lu)", GetLastError());
    wchar_t xessDll[MAX_PATH]{};
    const DWORD selfLength = GetModuleFileNameW(self, xessDll, MAX_PATH);
    if (!selfLength || selfLength >= MAX_PATH)
        return fail(why, "EDVR module path for libxess.dll could not be read (Win32=%lu)", GetLastError());
    wchar_t* const slash = wcsrchr(xessDll, L'\\');
    constexpr wchar_t kXessDllName[] = L"libxess.dll";
    if (!slash || size_t(slash + 1 - xessDll) + (sizeof(kXessDllName) / sizeof(wchar_t)) > MAX_PATH)
        return fail(why, "EDVR module directory path is too long for libxess.dll");
    wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - xessDll), kXessDllName);
    const DWORD attributes = GetFileAttributesW(xessDll);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return fail(why, "libxess.dll is missing beside EDVR");
        return fail(why, "libxess.dll beside EDVR cannot be inspected (Win32=%lu)", code);
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return fail(why, "libxess.dll beside EDVR is a directory");
    state->xessModule = LoadLibraryExW(xessDll, nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!state->xessModule) {
        const DWORD code = GetLastError();
        if (code == ERROR_MOD_NOT_FOUND)
            return fail(why, "libxess.dll exists beside EDVR but a dependency could not be loaded (Win32=126; check the VC++ 2015+ runtime)");
        return fail(why, "libxess.dll beside EDVR failed to load (Win32=%lu)", code);
    }
    wchar_t loadedXessDll[MAX_PATH]{};
    const DWORD loadedLength = GetModuleFileNameW(state->xessModule, loadedXessDll, MAX_PATH);
    if (!loadedLength || loadedLength >= MAX_PATH)
        return fail(why, "loaded XeSS DLL path could not be verified");
    if (_wcsicmp(loadedXessDll, xessDll) != 0)
        return fail(why, "libxess.dll resolved to another module (expected %ls, loaded %ls)",
                    xessDll, loadedXessDll);
    Log::get().note("xess: runtime module %ls", loadedXessDll);
    XessApi& api = state->api;
    if (!loadProc(state->xessModule, "xessGetVersion", api.getVersion) ||
        !loadProc(state->xessModule, "xessGetIntelXeFXVersion", api.getIntelVersion) ||
        !loadProc(state->xessModule, "xessGetOptimalInputResolution", api.getOptimalInput) ||
        !loadProc(state->xessModule, "xessD3D12CreateContext", api.createContext) ||
        !loadProc(state->xessModule, "xessD3D12Init", api.init) ||
        !loadProc(state->xessModule, "xessD3D12Execute", api.execute) ||
        !loadProc(state->xessModule, "xessDestroyContext", api.destroyContext))
        return fail(why, "libxess.dll lacks a required XeSS-SR D3D12 export");
    xess_version_t version{};
    const xess_result_t versionRc = api.getVersion(&version);
    if (versionRc != XESS_RESULT_SUCCESS)
        return fail(why, "xessGetVersion returned %d", int(versionRc));
    // The pinned SDK package is 3.0.2, but its SR runtime ABI is 2.0.2.
    // Development-stage patches (90+) may change that ABI too.
    if (!xessSrRuntimeCompatible(version.major, version.minor, version.patch))
        return fail(why, "libxess.dll has incompatible XeSS-SR runtime %u.%u.%u.%u (expected 2.0.x)",
                    version.major, version.minor, version.patch, version.reserved);
    Log::get().note("xess: SDK runtime version %u.%u.%u.%u loaded; D3D12 SR exports present",
                    version.major, version.minor, version.patch, version.reserved);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = state->d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(state->queue.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "D3D12 queue failed 0x%08X", unsigned(hr));
    // The queue's timestamp ticks convert to milliseconds for bridge timing. A zero frequency switches that timing off.
    if (FAILED(state->queue->GetTimestampFrequency(&state->d12TimestampHz))) state->d12TimestampHz = 0;
    hr = state->d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                IID_PPV_ARGS(state->fence12.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "D3D12 shared fence failed 0x%08X", unsigned(hr));
    hr = state->d12->CreateSharedHandle(state->fence12.Get(), nullptr, GENERIC_ALL,
                                        nullptr, &state->sharedFenceHandle);
    if (FAILED(hr)) return fail(why, "D3D12 fence NT handle failed 0x%08X", unsigned(hr));
    hr = state->d11v5->OpenSharedFence(state->sharedFenceHandle,
                                       IID_PPV_ARGS(state->fence11.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "D3D11 shared fence open failed 0x%08X", unsigned(hr));
    state->cpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->cpuEvent) return fail(why, "GPU fence CPU event failed (Win32=%lu)", GetLastError());
    Log::get().note("xess: D3D11/D3D12 shared queue and fence ready");
    g_state = state.release();
    g_available = true;
    if (why) *why = nullptr;
    return true;
#endif
}

bool xessEvaluate(ID3D11DeviceContext* context, unsigned slot,
                  ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                  ID3D11Texture2D* motion, ID3D11Texture2D* output,
                  uint32_t width, uint32_t height, uint32_t outputWidth,
                  uint32_t outputHeight, float jitterX, float jitterY,
                  bool reset, bool hdr, const char** why) {
#if !EDVR_HAVE_XESS
    (void)context; (void)slot; (void)colour; (void)depth; (void)motion; (void)output;
    (void)width; (void)height; (void)outputWidth; (void)outputHeight;
    (void)jitterX; (void)jitterY; (void)reset; (void)hdr;
    return fail(why, "this build has no XeSS SDK (EDVR_HAVE_XESS=0)");
#else
    if (slot >= kUpscalerSlots)
        return fail(why, "XeSS-SR upscaler slot %u is outside 0..%u", slot, kUpscalerSlots - 1);
    if (!context || !width || !height || !outputWidth || !outputHeight ||
        !std::isfinite(jitterX) || !std::isfinite(jitterY))
        return fail(why, "invalid XeSS context, dimensions, or jitter");
    LARGE_INTEGER began{};
    QueryPerformanceCounter(&began);
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!xessAvailable(device.Get(), why)) return false;
    State& state = *g_state;
    FeatureSlot& holder = state.slots[slot];
    if (state.untrackedSubmission) {
        constexpr const char* reason = "an unfenced GPU submission is still in flight; XeSS resources are retained until process exit";
        if (!state.untrackedRefusalLogged) {
            Log::get().note("xess: refused: %s", reason);
            state.untrackedRefusalLogged = true;
        }
        if (why) *why = reason;
        return false;
    }
    const DWORD thread = GetCurrentThreadId();
    if (state.ownerThread && (state.ownerThread != thread || state.ownerContext.Get() != context))
        return fail(why, "XeSS backend must use its original D3D11 immediate context/render thread");
    if (!state.ownerThread) { state.ownerThread = thread; state.ownerContext = context; }
    ComPtr<ID3D11DeviceContext4> ctx4;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(ctx4.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "ID3D11DeviceContext4/Signal/Wait unavailable 0x%08X", unsigned(hr));
    // Bridge timing is read here, between evaluations, from results the GPU has finished. Its 30 s line is due here too.
    sweepBridge(state, slot, context);
    if (holder.window.executed && GetTickCount64() - holder.window.startedAt >= kBridgeReportMs) {
        XessBridgeTiming line;
        reportSlot(state, slot, line);
    }
    const DXGI_FORMAT colorFormat = hdr ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!validateTexture(colour, width, height, colorFormat, "color", why) ||
        !validateTexture(depth, width, height, DXGI_FORMAT_R32_FLOAT, "depth", why) ||
        !validateTexture(motion, width, height, DXGI_FORMAT_R16G16_FLOAT, "motion", why) ||
        !validateTexture(output, outputWidth, outputHeight,
                         hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : colorFormat, "output", why))
        return false;
    const XessFeatureKey key{width, height, outputWidth, outputHeight, hdr};
    holder.failedFeature.clearIfDifferent(key);
    if (holder.failedFeature.matches(key)) return returnLatchedFailure(holder.failedFeature, why);
    if (!holder.feature || holder.feature->inW != width || holder.feature->inH != height ||
        holder.feature->outW != outputWidth || holder.feature->outH != outputHeight ||
        holder.feature->hdr != hdr) {
        // A fence-drain refusal is transient; only a failed create is latched.
        if (!releaseSlot(state, slot, why)) return false;
        const char* createWhy = nullptr;
        bool created = false;
        {
            QuietFeatureFailure quiet;
            created = makeFeature(state, slot, width, height, outputWidth, outputHeight, hdr, &createWhy);
        }
        if (!created) {
            holder.failedFeature.record(key, createWhy);
            Log::get().note("xess: slot=%u feature create failed in=%ux%u out=%ux%u hdr=%u: %s; this key will not retry until the key or mode changes",
                            slot, width, height, outputWidth, outputHeight, hdr ? 1u : 0u,
                            holder.failedFeature.reason());
            return returnLatchedFailure(holder.failedFeature, why);
        }
    }
    Feature& feature = *holder.feature;
    CommandSlot& command = feature.commands[feature.nextCommand++ % kCommandRing];
    // The wait happens only when a command allocator is about to be reused;
    // normal producer/consumer ordering is entirely GPU-fence based.
    // CPU spans for the bridge line. The fence is read before the wait, so "blocked" means the wait really had to wait.
    const bool allocBlocked = command.doneValue > state.fence12->GetCompletedValue();
    LARGE_INTEGER allocBegan{}, allocEnded{};
    QueryPerformanceCounter(&allocBegan);
    if (!waitFor(state, command.doneValue, why, "allocator reuse")) return false;
    QueryPerformanceCounter(&allocEnded);
    const double allocMs = qpcSpanMs(allocBegan, allocEnded);
    double submitMs = 0.0;
    // The wait above has passed this allocator's last submission, so its timestamps are final before the reset reuses them.
    collectExecute(state, holder.window, command);
    hr = command.allocator->Reset();
    if (FAILED(hr)) return fail(why, "D3D12 command allocator reset failed 0x%08X", unsigned(hr));
    hr = command.list->Reset(command.allocator.Get(), nullptr);
    if (FAILED(hr)) return fail(why, "D3D12 command list reset failed 0x%08X", unsigned(hr));
    SharedTexture* const inputs[] = {&feature.color, &feature.motion, &feature.depth};
    const char* keyWhy = nullptr;
    if (!takeKeys(inputs, &keyWhy, "D3D11 input copy")) {
        command.list->Close(); // Reset opened it; no D3D12 work was submitted.
        holder.failedFeature.record(key, keyWhy);
        return returnLatchedFailure(holder.failedFeature, why);
    }
    // Bridge timing opens before the input copies: total, then copy-in. The guard cancels them on any failure below.
    BridgeSet* bridge = beginBridge(holder, device.Get(), context);
    BridgeGuard guard(bridge, context);
    context->CopyResource(feature.color.d11.Get(), colour);
    context->CopyResource(feature.motion.d11.Get(), motion);
    context->CopyResource(feature.depth.d11.Get(), depth);
    giveKeys(inputs);
    const uint64_t producer = ++state.nextFenceValue;
    hr = ctx4->Signal(state.fence11.Get(), producer);
    if (FAILED(hr)) {
        command.list->Close(); // Reset opened it; no D3D12 work was submitted.
        // The three D3D11 input copies may already be queued. Without a
        // fence, releasing their shared textures would race those copies.
        state.untrackedSubmission = true;
        return fail(why, "D3D11 producer Signal(%llu) failed 0x%08X; resources retained",
                    static_cast<unsigned long long>(producer), unsigned(hr));
    }
    if (bridge) {   // copy-in ends after the Signal is enqueued; the wait starts there
        bridge->timers[kBridgeCopyIn].end(context);
        bridge->timers[kBridgeWait].begin(device.Get(), context);
    }
    context->Flush();
    feature.lastUseValue = producer;
    LARGE_INTEGER queueWaitBegan{}, queueWaitEnded{};
    QueryPerformanceCounter(&queueWaitBegan);
    hr = state.queue->Wait(state.fence12.Get(), producer);
    QueryPerformanceCounter(&queueWaitEnded);
    submitMs += qpcSpanMs(queueWaitBegan, queueWaitEnded);
    if (FAILED(hr)) {
        command.list->Close(); // No command list was submitted.
        return fail(why, "D3D12 producer Wait(%llu) failed 0x%08X",
                    static_cast<unsigned long long>(producer), unsigned(hr));
    }
    ID3D12GraphicsCommandList* list = command.list.Get();
    transition(list, feature.color.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, feature.motion.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, feature.depth.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, feature.output.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    xess_d3d12_execute_params_t params{};
    params.pColorTexture = feature.color.d12.Get();
    params.pVelocityTexture = feature.motion.d12.Get();
    params.pDepthTexture = feature.depth.d12.Get();
    params.pOutputTexture = feature.output.d12.Get();
    // flatProjectionJitter shifts clip X by +2*jitterX/width and clip Y by
    // -2*jitterY/height (positive pixel coordinates right/down). XeSS takes
    // the projection's subpixel sample offset in those same pixel axes.
    params.jitterOffsetX = jitterX;
    params.jitterOffsetY = jitterY;
    params.exposureScale = 1.0f;
    params.resetHistory = reset ? 1u : 0u;
    params.inputWidth = width;
    params.inputHeight = height;
    // The two timestamps bracket xessD3D12Execute alone; the barriers around it are outside the measured span.
    if (command.timingHeap.Get()) list->EndQuery(command.timingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const xess_result_t rc = state.api.execute(feature.context, list, &params);
    if (rc != XESS_RESULT_SUCCESS) {
        ++state.failures;
        // The command list was never submitted; it is safe to close/reset on
        // the next frame, while the producer signal remains a no-op handoff.
        list->Close();
        if (state.failures != 1 && state.failures % 300 != 0) {
            QuietFeatureFailure quiet;
            return fail(why, "xessD3D12Execute returned %d (frame %u)", int(rc), state.evaluations + 1);
        }
        return fail(why, "xessD3D12Execute returned %d (frame %u)", int(rc), state.evaluations + 1);
    }
    if (command.timingHeap.Get()) {
        list->EndQuery(command.timingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(command.timingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                               command.timingReadback.Get(), 0);
    }
    transition(list, feature.color.d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(list, feature.motion.d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(list, feature.depth.d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(list, feature.output.d12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    hr = list->Close();
    if (FAILED(hr)) return fail(why, "D3D12 XeSS command close failed 0x%08X", unsigned(hr));
    ID3D12CommandList* lists[] = {list};
    LARGE_INTEGER submitBegan{}, submitEnded{};
    QueryPerformanceCounter(&submitBegan);
    state.queue->ExecuteCommandLists(1, lists);
    command.timingPending = command.timingHeap.Get() != nullptr;
    const uint64_t done = ++state.nextFenceValue;
    hr = state.queue->Signal(state.fence12.Get(), done);
    QueryPerformanceCounter(&submitEnded);
    submitMs += qpcSpanMs(submitBegan, submitEnded);
    if (FAILED(hr)) {
        state.untrackedSubmission = true;
        return fail(why, "D3D12 execute Signal(%llu) failed 0x%08X; submitted work retained",
                    static_cast<unsigned long long>(done), unsigned(hr));
    }
    command.doneValue = done;
    feature.lastUseValue = done;
    hr = ctx4->Wait(state.fence11.Get(), done);
    if (FAILED(hr)) return fail(why, "D3D11 output Wait(%llu) failed 0x%08X",
                                static_cast<unsigned long long>(done), unsigned(hr));
    if (bridge) {   // the wait ends once the D3D12 signal has passed; copy-out starts there
        bridge->timers[kBridgeWait].end(context);
        bridge->timers[kBridgeCopyOut].begin(device.Get(), context);
    }
    SharedTexture* const result[] = {&feature.output};
    if (!takeKeys(result, &keyWhy, "D3D11 output copy")) {
        holder.failedFeature.record(key, keyWhy);
        return returnLatchedFailure(holder.failedFeature, why);
    }
    if (hdr) {
        const bool widened = widenHdr(context, feature, output, why);
        giveKeys(result);
        if (!widened) return false;
    } else {
        context->CopyResource(output, feature.output.d11.Get());
        giveKeys(result);
    }
    if (bridge) {   // copy-out and the whole bridge end after the output copy
        bridge->timers[kBridgeCopyOut].end(context);
        bridge->timers[kBridgeTotal].end(context);
        guard.disarm();
    }
    const uint64_t consumer = ++state.nextFenceValue;
    hr = ctx4->Signal(state.fence11.Get(), consumer);
    if (FAILED(hr)) {
        // The copy/widening work can still be queued on D3D11. Without its
        // completion signal we cannot safely recycle shared textures.
        state.untrackedSubmission = true;
        return fail(why, "D3D11 consumer Signal(%llu) failed 0x%08X; resources retained",
                    static_cast<unsigned long long>(consumer), unsigned(hr));
    }
    context->Flush();
    feature.lastUseValue = consumer;
    LARGE_INTEGER ended{}, frequency{};
    QueryPerformanceCounter(&ended);
    QueryPerformanceFrequency(&frequency);
    const double ms = frequency.QuadPart > 0
        ? static_cast<double>(ended.QuadPart - began.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)
        : 0.0;
    ++holder.evaluations;
    if (reset) ++holder.resets;
    holder.timeSumMs += ms;
    if (ms > holder.timeMaxMs) holder.timeMaxMs = ms;
    if (!holder.window.executed) holder.window.startedAt = GetTickCount64();
    ++holder.window.executed;
    // Stored only for evaluations that executed, so every CPU sample and the blocked count describe the same set.
    noteSample(holder.window.cpuEval, ms);
    noteSample(holder.window.cpuAlloc, allocMs);
    noteSample(holder.window.cpuSubmit, submitMs);
    if (allocBlocked) ++holder.window.allocBlocked;
    ++state.evaluations;
    if (reset) ++state.resets;
    if (state.evaluations <= 3 || state.evaluations % 300 == 0)
        Log::get().note("xess: slot=%u execute #%u rc=0 in=%ux%u out=%ux%u hdr=%u jitter=(%.3f,%.3f) reset=%u fences=%llu/%llu/%llu copy=%s",
                        slot, state.evaluations, width, height, outputWidth, outputHeight,
                        hdr ? 1u : 0u, jitterX, jitterY, reset ? 1u : 0u,
                        static_cast<unsigned long long>(producer),
                        static_cast<unsigned long long>(done),
                        static_cast<unsigned long long>(consumer), hdr ? "R11->FP16" : "RGBA8");
    if (why) *why = nullptr;
    return true;
#endif
}

void xessReleaseFeatures() {
#if EDVR_HAVE_XESS
    if (!g_state) return;
    // The profile switch releases every slot. Each release has its own bounded GPU
    // wait, so one slot's refusal does not keep the others from being freed.
    for (unsigned slot = 0; slot < kUpscalerSlots; ++slot) {
        g_state->slots[slot].failedFeature.clear();
        if (!releaseSlot(*g_state, slot, nullptr))
            Log::get().note("xess: slot=%u retained feature after bounded GPU wait refusal; it cannot be freed in flight", slot);
    }
#endif
}

void xessShutdown() {
#if EDVR_HAVE_XESS
    if (g_state) {
        Log::get().note("xess: session evaluations=%u resets=%u failures=%u",
                        g_state->evaluations, g_state->resets, g_state->failures);
        bool drained = true;
        for (unsigned slot = 0; slot < kUpscalerSlots; ++slot)
            if (!releaseSlot(*g_state, slot, nullptr)) drained = false;
        if (!drained) {
            // The GPU still refers to the feature's command allocators or
            // shared textures. Process teardown will reclaim them; releasing
            // them here would be a use-after-free on a hung device.
            Log::get().note("xess: GPU did not drain; retaining backend objects until process exit");
        } else {
            // A GpuTimer holds its queries and lease until its owner resets it; the owner context is the only one allowed to.
            if (g_state->ownerContext.Get()) cancelBridgeTimers(*g_state, g_state->ownerContext.Get());
            delete g_state;
        }
        g_state = nullptr;
    }
    g_tried = false;
    g_available = false;
#endif
}

// Rig seams, as fsr3TestContextCreations is for FSR: the features a slot has built since the backend started, and whether
// the slot holds one now. Both are zero or false in a build without the XeSS SDK.
uint32_t xessTestFeatureCreations(unsigned slot) {
#if EDVR_HAVE_XESS
    return g_state && slot < kUpscalerSlots ? g_state->slots[slot].creations : 0u;
#else
    (void)slot;
    return 0u;
#endif
}

bool xessTestFeatureLive(unsigned slot) {
#if EDVR_HAVE_XESS
    return g_state && slot < kUpscalerSlots && g_state->slots[slot].feature != nullptr;
#else
    (void)slot;
    return false;
#endif
}

bool xessSlotTotals(unsigned slot, uint32_t* evaluations, uint32_t* resets, double* avgMs, double* maxMs) {
#if EDVR_HAVE_XESS
    if (!g_state || slot >= kUpscalerSlots) return false;
    const FeatureSlot& holder = g_state->slots[slot];
    if (holder.evaluations == 0) return false;
    if (evaluations) *evaluations = holder.evaluations;
    if (resets) *resets = holder.resets;
    if (avgMs) *avgMs = holder.timeSumMs / holder.evaluations;
    if (maxMs) *maxMs = holder.timeMaxMs;
    return true;
#else
    (void)slot; (void)evaluations; (void)resets; (void)avgMs; (void)maxMs;
    return false;
#endif
}

unsigned xessBridgeTimingSweep(ID3D11DeviceContext* context) {
#if EDVR_HAVE_XESS
    if (!g_state || !context) return 0;
    unsigned unread = 0;
    for (unsigned slot = 0; slot < kUpscalerSlots; ++slot) {
        sweepBridge(*g_state, slot, context);
        unread += bridgeUnread(g_state->slots[slot]);
    }
    return unread;
#else
    (void)context;
    return 0;
#endif
}

bool xessBridgeTimingReport(ID3D11DeviceContext* context, unsigned slot, XessBridgeTiming* out) {
#if EDVR_HAVE_XESS
    if (!g_state || !context || !out || slot >= kUpscalerSlots) return false;
    sweepBridge(*g_state, slot, context);
    if (!g_state->slots[slot].window.executed) return false;
    reportSlot(*g_state, slot, *out);
    return true;
#else
    (void)context; (void)slot; (void)out;
    return false;
#endif
}

}  // namespace edvr
