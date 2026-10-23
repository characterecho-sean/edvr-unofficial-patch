#include "fsr4_engine.h"

#include <windows.h>
#include <d3d11.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../common/log.h"
#include "dlaa.h"          // kUpscalerSlots: one FSR 4 context per upscaler slot
#include "fsr4_version.h"  // the pure decisions: version pick, refusal text, formats, flags, camera planes

// Bare `/D EDVR_HAVE_FSR4` (no value) would make `#if EDVR_HAVE_FSR4` below a syntax error rather than a clean 0. Pin it, as
// fsr3_engine.cpp pins EDVR_HAVE_FSR3.
#ifndef EDVR_HAVE_FSR4
#define EDVR_HAVE_FSR4 0
#endif

#if EDVR_HAVE_FSR4
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "gpu_timing.h"
// AMD's FidelityFX SDK v2.3.0 headers (the ffx-api: C descriptors over five exported functions), staged by the build under
// <sdk>\include\FidelityFX\. ffx_api_loader.h is not used: it is a header of inline GetProcAddress calls that this file does by
// hand, and it #pragma errors off Windows. The headers declare their functions dllexport; nothing here names those functions, it
// takes the Pfn typedefs and resolves the exports from AMD's loader at run time.
#pragma warning(push, 0)
#include <FidelityFX/api/include/ffx_api.h>
#include <FidelityFX/api/include/dx12/ffx_api_dx12.h>
#include <FidelityFX/upscalers/include/ffx_upscale.h>
#pragma warning(pop)
using Microsoft::WRL::ComPtr;
#endif

namespace edvr {

#if EDVR_HAVE_FSR4
// hdr_backend_flags.h and fsr4_version.h mirror AMD's creation and dispatch flags so a rig can pin the flag set without the SDK;
// every constant is checked against the real enum here, so a drift in either fails this compile and not a flight. AMD's debug
// checking bit is not mirrored (it is 1<<7 here, 1<<8 in the FSR 3.1 DX11 port's header) and is never set.
static_assert(kFsrFlagHighDynamicRange == static_cast<uint32_t>(FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE), "HDR bit");
static_assert(kFsrFlagDepthInverted == static_cast<uint32_t>(FFX_UPSCALE_ENABLE_DEPTH_INVERTED), "inverted depth bit");
static_assert(kFsrFlagDepthInfinite == static_cast<uint32_t>(FFX_UPSCALE_ENABLE_DEPTH_INFINITE), "infinite depth bit");
static_assert(kFsrFlagAutoExposure == static_cast<uint32_t>(FFX_UPSCALE_ENABLE_AUTO_EXPOSURE), "auto exposure bit");
static_assert(kFsr4FlagNonLinearColourspace == static_cast<uint32_t>(FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE), "non-linear colour space bit");
static_assert(kFsr4DispatchNonLinearSrgb == static_cast<uint32_t>(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB), "dispatch sRGB bit");
#endif

namespace {

constexpr size_t kReasonBytes = 512;

// "fsr 4.1.1" once a context has reported its provider (fsr4VersionLabel); "fsr4" before. Plain storage, no static destructor.
char g_label[40] = "fsr4";

// One refusal line per distinct reason: the same text again is counted and said only every 600th time, so a refusal the caller
// meets every frame cannot flood the log. Function-local trivially-initialised storage: no guard, no destructor.
void logRefusal(const char* text) {
    static char last[kReasonBytes];
    static unsigned repeats = 0;
    if (std::strcmp(last, text) == 0) {
        if (++repeats % 600u != 0u) return;
        Log::get().note("fsr4: refused: %s (the same refusal again, %u times)", text, repeats + 1u);
        return;
    }
    std::snprintf(last, sizeof(last), "%s", text);
    repeats = 0;
    Log::get().note("fsr4: refused: %s", text);
}

#if !EDVR_HAVE_FSR4
// Every entry of a build without AMD's SDK answers this, so the log says once that FSR 4 was asked for and is not there.
bool failNotBuilt(const char** why) {
    constexpr const char* reason = "this build was made without AMD's FSR 4 SDK (EDVR_HAVE_FSR4)";
    if (why) *why = reason;
    logRefusal(reason);
    return false;
}
#endif

#if EDVR_HAVE_FSR4
char g_reason[kReasonBytes] = "FSR 4 has not been checked";          // evaluate and release refusals
char g_probeReason[kReasonBytes] = "FSR 4 has not been checked";     // the availability probe's, kept apart so a later evaluate cannot overwrite it
bool g_quietFeatureFailure = false;
bool g_fovFallbackNoted = false;

struct QuietFeatureFailure {
    const bool prior = g_quietFeatureFailure;
    QuietFeatureFailure() { g_quietFeatureFailure = true; }
    ~QuietFeatureFailure() { g_quietFeatureFailure = prior; }
};

// Every message is formatted into static storage, so the pointer handed back through why stays valid after the caller's frame
// and after the backend that produced it is gone. The text is always the format's own: pass user text as "%s", never as the
// format (the Proton hint carries a literal %command%).
bool fail(const char** why, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_reason, sizeof(g_reason), format, args);
    va_end(args);
    g_reason[sizeof(g_reason) - 1] = '\0';
    if (why) *why = g_reason;
    if (!g_quietFeatureFailure) logRefusal(g_reason);
    return false;
}

bool failProbe(const char** why, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_probeReason, sizeof(g_probeReason), format, args);
    va_end(args);
    g_probeReason[sizeof(g_probeReason) - 1] = '\0';
    if (why) *why = g_probeReason;
    logRefusal(g_probeReason);
    return false;
}

constexpr unsigned kCommandRing = 3;
constexpr DWORD kGpuWaitMs = 5000;
// Only this device ever takes a shared texture's key, always key 0, so an acquire is immediate; one that waits at all is a
// fault, not contention.
constexpr DWORD kKeyWaitMs = 100;
constexpr size_t kMaxOffered = 32;       // versions read from AMD's runtime; it offers a handful
constexpr unsigned kDispatchStrikes = 3; // consecutive failed dispatches before the key is latched as failed

using CreateD3D12DeviceFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

// AMD's ffx-api: five exports of amd_fidelityfx_loader_dx12.dll.
struct FfxApi {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
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
    // Bridge timing: two timestamps around ffxDispatch, resolved into this allocator's readback buffer. The buffer is mapped for
    // the slot's life. timingPending is set once a list that resolves into it has been submitted, and cleared only after the
    // fence has passed that submission, so the readback is never read early or read twice.
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

// One evaluation's D3D11 timers. Begun inside the temporal pass's own interval they borrow its disjoint scope, so the nesting
// costs no second disjoint query. The set is read a few evaluations later, once every part is ready or invalid.
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
    std::vector<double> execute;               // D3D12 dispatch samples
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

// The create descriptor chain. ffx_api.h asks that what a create descriptor points at stay live until ffxDestroyContext, so the
// chain lives and dies with the context rather than on the creating frame's stack.
struct ContextDescs {
    ffxCreateContextDescUpscale upscale{};
    ffxCreateBackendDX12Desc backend{};
    ffxCreateContextDescUpscaleVersion version{};
    ffxOverrideVersion versionOverride{};
};

struct Feature {
    unsigned slot = 0;
    Fsr4ContextKey key;
    ffxContext context = nullptr;
    std::unique_ptr<ContextDescs> descs;
    SharedTexture color, motion, depth, output;
    std::array<CommandSlot, kCommandRing> commands;
    unsigned nextCommand = 0;
    uint64_t lastUseValue = 0;
    bool firstDispatch = true;     // a fresh context's first dispatch always breaks the (empty) history
    unsigned dispatchStrikes = 0;  // consecutive failed dispatches
};

// One per upscaler slot (dlaa.h, kUpscalerSlots), as fsr3_engine.cpp keeps one context per slot. Each slot keys, builds,
// releases and latches its own context; the shared textures hang off the Feature, so they are per slot too.
struct FeatureSlot {
    Fsr4FailureLatch failedFeature;
    std::unique_ptr<Feature> feature;
    uint32_t creations = 0;  // test seam: contexts this slot has built since the backend started
    // The slot's price, as CPU time for each successful evaluate call (submission and its copies; the GPU is not timed).
    uint32_t evaluations = 0, resets = 0;
    double timeSumMs = 0.0, timeMaxMs = 0.0;
    // Bridge timing lives per slot, so it outlives a context rebuild. The sets are reset on the owner context at shutdown.
    std::array<BridgeSet, kBridgeSets> bridge;
    BridgeWindow window;
    char d12Note[96] = "";   // why this slot's D3D12 dispatch timestamps are absent, when they are
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
    HMODULE loaderModule = nullptr;
    HANDLE sharedFenceHandle = nullptr;
    HANDLE cpuEvent = nullptr;
    FfxApi api;
    // The provider AMD's runtime was asked for: the highest 4.x it offers for this device (D7), as the id ffxOverrideVersion
    // takes and the name it reported, copied out of AMD's memory at once.
    uint64_t versionId = 0;
    std::string versionName;
    // One counter for every slot: all slots signal the one shared fence in this order, so a wait on a slot's own value also
    // covers the other slots' earlier work, and the value never goes backwards.
    uint64_t nextFenceValue = 0;
    uint64_t d12TimestampHz = 0;   // queue ticks per second; 0 when the queue reports none (bridge timing then off)
    uint32_t evaluations = 0, resets = 0, failures = 0;
    bool untrackedSubmission = false;
    bool untrackedRefusalLogged = false;
    std::array<FeatureSlot, kUpscalerSlots> slots;
    ~State() {
        // Normal shutdown has already destroyed every ffx context after the consumer fence. Release COM objects, which
        // include nothing of AMD's, and close the handles. AMD's loader is deliberately left mapped: it has run a signed ML
        // runtime on this process, its detach path has never been flown, and an unloaded-and-reloaded copy buys nothing a
        // second LoadLibrary of the mapped one does not. d3d12.dll is the system's and is released as XeSS releases it.
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
        if (d3d12Module) FreeLibrary(d3d12Module);
    }
};

// A raw owner on purpose. A static destructor would run at DLL_PROCESS_DETACH on process exit, releasing D3D12 objects under the
// loader lock after the game's other threads are gone; DllMain forbids that, and the proxy's rule there is "do the minimum and
// leak the rest" (d3d11_proxy.cpp). The FreeLibrary-unload teardown leaks it too: that path can hold the loader lock and must
// issue no context commands (device_hook.cpp, shutdownDeviceHooks), while fsr4Shutdown signals, flushes and waits. The
// render-thread device-change path and rigs may call it; loader-lock teardown must not.
State* g_state = nullptr;
bool g_tried = false;
bool g_available = false;
// The device a failed availability probe ran on. A probe failure is the device's (no shared fences, no D3D12 on its adapter, an
// AMD runtime that offers no 4.x for its GPU), so it is kept only for that device: a different one is probed afresh. Held with
// its own reference so a replacement device that lands at a freed device's address cannot pass for it; released by
// fsr4Shutdown and by the next probe. A raw pointer, like g_state: no static destructor.
ID3D11Device* g_failedProbeDevice = nullptr;
std::atomic<uint32_t> g_messageCount{0};   // AMD's runtime may call back from its own threads
constexpr uint32_t kMessageCap = 20;

void forgetFailedProbeDevice() {
    if (g_failedProbeDevice) g_failedProbeDevice->Release();
    g_failedProbeDevice = nullptr;
}

bool returnLatchedFailure(const Fsr4FailureLatch& latch, const char** why) {
    // The caller may retain this pointer after a device change destroys State.
    std::snprintf(g_reason, sizeof(g_reason), "%s", latch.reason());
    if (why) *why = g_reason;
    return false;
}

template <typename T> bool loadProc(HMODULE module, const char* name, T& target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

bool underWine() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

const char* ffxReturnName(ffxReturnCode_t rc) {
    switch (rc) {
        case FFX_API_RETURN_OK: return "ok";
        case FFX_API_RETURN_ERROR: return "error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown descriptor type";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
        case FFX_API_RETURN_NO_PROVIDER: return "no provider";
        case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER: return "bad parameter";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "provider too old for this descriptor";
        default: return "unrecognised code";
    }
}

// AMD's runtime messages, relayed to the log. fpMessage is called from whichever thread the runtime is on, so the cap counts
// atomically; a runtime that chatters every frame cannot fill the log.
void ffxMessage(uint32_t type, const wchar_t* message) {
    const uint32_t n = g_messageCount.fetch_add(1) + 1;
    if (n > kMessageCap) {
        if (n == kMessageCap + 1) Log::get().note("fsr4: further messages from AMD's runtime are suppressed.");
        return;
    }
    // Zero-initialised and checked: WideCharToMultiByte returns 0 without a terminator when the text does not fit.
    char narrow[256] = {};
    const int written = WideCharToMultiByte(CP_UTF8, 0, message ? message : L"", -1, narrow,
                                            static_cast<int>(sizeof(narrow)), nullptr, nullptr);
    if (written <= 0) {
        Log::get().note("fsr4: a message from AMD's runtime was too long for this log line (over %u bytes) or could not be converted.",
                        static_cast<unsigned>(sizeof(narrow) - 1));
        return;
    }
    Log::get().note("fsr4: amd %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", narrow);
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

// One allocator's dispatch timestamps, read once the fence has passed the submission that wrote them.
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

// Polls one slot's D3D11 sets without waiting. A set is read once all four parts are ready or invalid; until then it stays in
// flight. Returns the sets still waiting on the GPU.
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

// Evaluations whose timing is still on the GPU: D3D11 sets not yet read, or D3D12 dispatches whose fence has not passed.
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

// Logs the slot's line from its window and starts a new window. Reads still outstanding count as unread and land in the next
// window. An unavailable part is named, never printed as zero.
void reportSlot(State& state, unsigned slot) {
    FeatureSlot& holder = state.slots[slot];
    BridgeWindow& window = holder.window;
    double median[kBridgeParts] = {}, p95[kBridgeParts] = {};
    bool has[kBridgeParts] = {};
    for (unsigned part = 0; part < kBridgeParts; ++part)
        has[part] = summarize(window.parts[part], median[part], p95[part]);
    double executeMedian = 0.0, executeP95 = 0.0;
    const bool hasExecute = summarize(window.execute, executeMedian, executeP95);
    const unsigned d11Samples = unsigned(window.parts[kBridgeTotal].size());
    const unsigned d12Samples = unsigned(window.execute.size());
    const unsigned evals = d11Samples > d12Samples ? d11Samples : d12Samples;
    double evalMedian = 0.0, evalP95 = 0.0, allocMedian = 0.0, allocP95 = 0.0, submitMedian = 0.0, submitP95 = 0.0;
    const bool hasEval = summarize(window.cpuEval, evalMedian, evalP95);
    const bool hasAlloc = summarize(window.cpuAlloc, allocMedian, allocP95);
    const bool hasSubmit = summarize(window.cpuSubmit, submitMedian, submitP95);

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
    Log::get().note("fsr4: slot=%u bridge timing over %u evals, ms median/p95: copy-in %s, wait %s, "
                    "d3d12 dispatch %s, copy-out %s, d3d11 total %s (unread %u%s); "
                    "cpu ms median/p95: eval %s, alloc-wait %s (blocked %u of %u)%s",
                    slot, evals, copyIn, wait, execute, copyOut, total, bridgeUnread(holder), reasons.c_str(),
                    evalText, allocText, window.allocBlocked, window.executed, submitPart.c_str());

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

// Cancels an evaluation's D3D11 intervals when it fails before the whole bridge has ended, so no interval stays open on the
// owner context. Disarmed once the whole bridge has ended; the set is then read by pollBridge.
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

// Destroys the slot's ffx context and frees its shared textures, after the bounded GPU wait for everything it last submitted. A
// wait that fails leaves the context standing (the GPU still refers to it) and says why.
bool releaseSlot(State& state, unsigned slot, const char** why) {
    std::unique_ptr<Feature>& feature = state.slots[slot].feature;
    if (!feature) return true;
    if (state.untrackedSubmission)
        return fail(why, "a submitted D3D12 command list has no completed fence; retaining its resources");
    if (!waitFor(state, feature->lastUseValue, why, "release")) return false;
    if (feature->context) {
        const ffxReturnCode_t rc = state.api.destroy(&feature->context, nullptr);
        Log::get().note("fsr4: slot=%u destroy context rc=%u after %u evaluations", slot, unsigned(rc), state.evaluations);
        feature->context = nullptr;
    }
    // The fence has passed every submission, so their dispatch timestamps are final; read them before the readback goes.
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
    // Both misc flags are required for the NT handle path of the measured DXVK/vkd3d-proton bridge (xess_engine.cpp). Render-
    // target bind is valid on every format this engine shares; the output also needs D3D12 unordered access, which FSR 4 writes
    // it through.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET |
                     (writable ? D3D11_BIND_UNORDERED_ACCESS : 0);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = state.d11->CreateTexture2D(&desc, nullptr, target.d11.GetAddressOf());
    if (FAILED(hr)) return fail(why, "shared %s D3D11 texture %ux%u format=%u failed 0x%08X",
                                role, width, height, unsigned(format), unsigned(hr));
    // The native D3D11 runtime enforces the keyed mutex: it drops any copy or dispatch that touches the texture while this
    // device holds no key, and the consumer then reads and returns zeros (xess_engine_test failed so on an RTX 5090 under
    // Windows 11; DXVK does not enforce it). Every D3D11 access takes key 0 (takeKeys); D3D12 never takes it, and the shared
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
    const D3D12_RESOURCE_DESC opened = target.d12->GetDesc();
    Log::get().note("fsr4: slot=%u shared %s %ux%u format=%u D3D11->D3D12 open ok, D3D12 flags=0x%X format=%u",
                    slot, role, width, height, unsigned(format), unsigned(opened.Flags), unsigned(opened.Format));
    return true;
}

// AcquireSync reports WAIT_TIMEOUT and WAIT_ABANDONED as success HRESULTs, so only S_OK means the key is held. A failed take
// gives back what it took.
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

// The bridge's D3D12 timestamps for one allocator: a two-entry timestamp heap and a readback buffer for its resolve. A failure
// leaves the allocator untimed and names the reason in the slot's note. It never fails the context.
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

// The ffx context for a feature whose shared textures and command ring already stand. The last step of makeFeature, so the one
// thing that can fail after it (a provider below 4.x) destroys the context itself.
bool makeContext(State& state, Feature& feature, const char** why) {
    const Fsr4ContextKey& key = feature.key;
    feature.descs = std::make_unique<ContextDescs>();
    ContextDescs& d = *feature.descs;
    d.upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.upscale.flags = fsr4CreateFlags(key.infiniteDepth, key.hdr);
    d.upscale.maxRenderSize = FfxApiDimensions2D{key.inW, key.inH};
    d.upscale.maxUpscaleSize = FfxApiDimensions2D{key.outW, key.outH};
    d.upscale.fpMessage = &ffxMessage;
    d.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    d.backend.device = state.d12.Get();
    // Required since SDK 2.1 (ml.md, "New APIs"): the API version this was built against, chained on the upscale descriptor.
    d.version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    d.version.version = FFX_UPSCALER_VERSION;
    // The provider the availability probe picked: a 4.x one, by id, so AMD cannot choose its default (3.1.5 on a GPU it does not
    // route to FSR 4, an older 4.x where a newer is offered).
    d.versionOverride.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    d.versionOverride.versionId = state.versionId;
    d.upscale.header.pNext = &d.backend.header;
    d.backend.header.pNext = &d.version.header;
    d.version.header.pNext = &d.versionOverride.header;
    d.versionOverride.header.pNext = nullptr;

    LARGE_INTEGER began{}, ended{};
    QueryPerformanceCounter(&began);
    ffxContext context = nullptr;
    const ffxReturnCode_t rc = state.api.create(&context, &d.upscale.header, nullptr);
    QueryPerformanceCounter(&ended);
    if (rc != FFX_API_RETURN_OK || !context) {
        if (context) state.api.destroy(&context, nullptr);
        return fail(why, "ffxCreateContext returned %u (%s) for provider %s at in=%ux%u out=%ux%u flags=0x%X",
                    unsigned(rc), ffxReturnName(rc), state.versionName.c_str(), key.inW, key.inH, key.outW, key.outH,
                    unsigned(d.upscale.flags));
    }
    feature.context = context;
    Log::get().note("fsr4: slot=%u context created in %.1f ms: in=%ux%u out=%ux%u flags=0x%X infinite=%u hdr=%u",
                    feature.slot, qpcSpanMs(began, ended), key.inW, key.inH, key.outW, key.outH,
                    unsigned(d.upscale.flags), key.infiniteDepth ? 1u : 0u, key.hdr ? 1u : 0u);

    // Which provider AMD actually bound. D7 belt and braces: the override above asked for a 4.x one; a context on anything older
    // is destroyed, never run in fsr4's name.
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t queried = state.api.query(&feature.context, &provider.header);
    char name[48] = "";
    if (queried == FFX_API_RETURN_OK && provider.versionName)
        std::snprintf(name, sizeof(name), "%s", provider.versionName);   // copied: AMD's pointer is not ours to keep
    Log::get().note("fsr4: slot %u provider %s (id 0x%llX) query rc=%u", feature.slot, name[0] ? name : "unknown",
                    static_cast<unsigned long long>(queried == FFX_API_RETURN_OK ? provider.versionId : 0), unsigned(queried));
    if (queried != FFX_API_RETURN_OK || !fsr4ProviderIsFour(name, provider.versionId, state.versionId)) {
        state.api.destroy(&feature.context, nullptr);
        feature.context = nullptr;
        return fail(why, "AMD's runtime bound %s%s to the context, not a 4.x provider; fsr4 never runs an older one in its name",
                    name[0] ? "FSR " : "an unnamed provider", name);
    }
    Fsr4Version provided;
    if (fsr4ParseVersion(name, &provided)) std::snprintf(g_label, sizeof(g_label), "fsr %s", fsr4FormatVersion(provided).c_str());
    else std::snprintf(g_label, sizeof(g_label), "fsr %s", name);

    // Diagnostic only: which inputs this provider says it requires and which it takes if given (FfxApiQueryResourceIdentifiers:
    // colour 1, depth 2, motion 4, exposure 8, reactive 16, transparency 32). Route 1 gives exposure none and runs on AMD's
    // default; if this provider requires one, the first dispatch refuses and this line says why.
    ffxQueryDescUpscaleGetResourceRequirements requirements{};
    requirements.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    const ffxReturnCode_t required = state.api.query(&feature.context, &requirements.header);
    Log::get().note("fsr4: slot=%u resource requirements rc=%u required=0x%llX optional=0x%llX", feature.slot, unsigned(required),
                    static_cast<unsigned long long>(requirements.required_resources),
                    static_cast<unsigned long long>(requirements.optional_resources));
    return true;
}

bool makeFeature(State& state, unsigned slot, const Fsr4ContextKey& key, const char** why) {
    auto feature = std::make_unique<Feature>();
    feature->slot = slot;
    feature->key = key;
    const DXGI_FORMAT colourFormat = static_cast<DXGI_FORMAT>(key.colourFormat);
    const DXGI_FORMAT depthFormat = static_cast<DXGI_FORMAT>(key.depthFormat);
    const DXGI_FORMAT motionFormat = static_cast<DXGI_FORMAT>(key.motionFormat);
    const DXGI_FORMAT outFormat = static_cast<DXGI_FORMAT>(key.outFormat);
    if (!makeShared(state, slot, feature->color, "color", key.inW, key.inH, colourFormat, false, why) ||
        !makeShared(state, slot, feature->motion, "motion", key.inW, key.inH, motionFormat, false, why) ||
        !makeShared(state, slot, feature->depth, "depth", key.inW, key.inH, depthFormat, false, why) ||
        !makeShared(state, slot, feature->output, "output", key.outW, key.outH, outFormat, true, why))
        return false;
    state.slots[slot].d12Note[0] = '\0';
    for (CommandSlot& command : feature->commands) {
        HRESULT hr = state.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(command.allocator.GetAddressOf()));
        if (FAILED(hr)) return fail(why, "D3D12 command allocator failed 0x%08X", unsigned(hr));
        hr = state.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           command.allocator.Get(), nullptr,
                                           IID_PPV_ARGS(command.list.GetAddressOf()));
        if (FAILED(hr)) return fail(why, "D3D12 command list failed 0x%08X", unsigned(hr));
        hr = command.list->Close();
        if (FAILED(hr)) return fail(why, "D3D12 initial command close failed 0x%08X", unsigned(hr));
        makeTiming(state, state.slots[slot], command);
    }
    Log::get().note("fsr4: slot=%u three command allocators ready", slot);
    if (!makeContext(state, *feature, why)) return false;
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

// What a caller's texture needs to become one of the bridge's shared textures: the typed format it maps to, and whether a plain
// CopyResource does it (the texture is exactly the render size) or a box copy must take its top-left render-size corner.
struct SourcePlan {
    DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
    bool exact = false;
};

// A render-grid input (colour, depth or motion). It may be larger than w x h: the engine's frame grids can sit in bigger
// textures, and FSR 4's auto exposure reads the WHOLE colour texture (OptiScaler's finding), so the shared texture is always
// exactly w x h and a larger source is box-copied into it. A box copy is illegal from a depth-stencil-bound resource (and from a
// multisampled one), so those must already be exactly w x h, where CopyResource takes them whole.
bool planInput(ID3D11Texture2D* texture, Fsr4Role role, uint32_t w, uint32_t h, const char* name, SourcePlan& plan,
               const char** why) {
    if (!texture) return fail(why, "%s texture is null", name);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    plan.typed = fsr4TypedFormat(role, desc.Format);
    if (plan.typed == DXGI_FORMAT_UNKNOWN)
        return fail(why, "%s texture format=%u has no typed FSR 4 equivalent", name, unsigned(desc.Format));
    if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1)
        return fail(why, "%s texture contract: got %ux%u format=%u samples=%u mips=%u slices=%u, need one sample, one mip and one slice",
                    name, desc.Width, desc.Height, unsigned(desc.Format), desc.SampleDesc.Count, desc.MipLevels, desc.ArraySize);
    if (desc.Width < w || desc.Height < h)
        return fail(why, "%s texture is %ux%u, smaller than the %ux%u render size", name, desc.Width, desc.Height, w, h);
    plan.exact = desc.Width == w && desc.Height == h;
    if (!plan.exact && (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL))
        return fail(why, "%s texture is %ux%u and depth-stencil bound; a box copy is illegal from it, so it must be exactly the %ux%u render size",
                    name, desc.Width, desc.Height, w, h);
    return true;
}

// The output. Exactly the output size: the engine copies the shared texture over all of it.
bool planOutput(ID3D11Texture2D* texture, uint32_t outW, uint32_t outH, SourcePlan& plan, const char** why) {
    if (!texture) return fail(why, "output texture is null");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    plan.typed = fsr4TypedFormat(Fsr4Role::Output, desc.Format);
    if (plan.typed == DXGI_FORMAT_UNKNOWN)
        return fail(why, "output texture format=%u has no typed FSR 4 equivalent", unsigned(desc.Format));
    if (desc.Width != outW || desc.Height != outH || desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1)
        return fail(why, "output texture contract: got %ux%u format=%u samples=%u mips=%u slices=%u, need %ux%u, one sample, one mip and one slice",
                    desc.Width, desc.Height, unsigned(desc.Format), desc.SampleDesc.Count, desc.MipLevels, desc.ArraySize,
                    outW, outH);
    plan.exact = true;
    return true;
}

void copyInput(ID3D11DeviceContext* context, ID3D11Texture2D* shared, ID3D11Texture2D* source, uint32_t w, uint32_t h,
               bool exact) {
    if (exact) {
        context->CopyResource(shared, source);
        return;
    }
    D3D11_BOX box{};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = w; box.bottom = h; box.back = 1;
    context->CopySubresourceRegion(shared, 0, 0, 0, 0, source, 0, &box);
}

// Reads AMD's versions for the upscaler on this device (decision D7) through ffxQuery(nullptr, ...): once for the count, once to
// fill. The device is a field of ffxQueryDescGetVersions, so no backend descriptor is chained (ffxapi.md, "Query"). Ids and names
// come from the one fill call, never two (their order may differ between calls), and every name is copied at once because AMD
// keeps some in global memory that a later query overwrites.
bool queryOffered(State& state, std::vector<std::string>& names, std::vector<uint64_t>& ids, const char** why) {
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = state.d12.Get();
    uint64_t count = 0;
    query.outputCount = &count;
    ffxReturnCode_t rc = state.api.query(nullptr, &query.header);
    if (rc != FFX_API_RETURN_OK)
        return failProbe(why, "AMD's ffxQuery for the upscaler versions returned %u (%s)%s", unsigned(rc), ffxReturnName(rc),
                         rc == FFX_API_RETURN_NO_PROVIDER ? "; no provider answered (is amd_fidelityfx_upscaler_dx12.dll loadable beside EDVR?)" : "");
    if (count > kMaxOffered) count = kMaxOffered;
    if (count == 0) return true;   // offered nothing: the caller refuses
    std::vector<uint64_t> versionIds(static_cast<size_t>(count), 0);
    std::vector<const char*> versionNames(static_cast<size_t>(count), nullptr);
    uint64_t capacity = count;
    query.outputCount = &capacity;
    query.versionIds = versionIds.data();
    query.versionNames = versionNames.data();
    rc = state.api.query(nullptr, &query.header);
    if (rc != FFX_API_RETURN_OK)
        return failProbe(why, "AMD's ffxQuery filling the upscaler versions returned %u (%s)", unsigned(rc), ffxReturnName(rc));
    const size_t returned = static_cast<size_t>(capacity < count ? capacity : count);
    for (size_t i = 0; i < returned; ++i) {
        names.emplace_back(versionNames[i] ? versionNames[i] : "");
        ids.push_back(versionIds[i]);
    }
    return true;
}

// The rest of an availability probe from the loaded D3D12 device: AMD's loader, its exports, the version offer, and the queue and
// shared fence. Split from fsr4Available so the order reads top to bottom; every refusal here is the probe's.
bool probeRuntime(State& state, const char** why) {
    // Resolve from this module, not the process working directory or an already-loaded foreign copy. The rig resolves beside
    // its executable.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&fsr4BuiltIn), &self) || !self)
        return failProbe(why, "EDVR module for AMD's FSR 4 DLLs could not be located (Win32=%lu)", GetLastError());
    wchar_t directory[MAX_PATH]{};
    const DWORD selfLength = GetModuleFileNameW(self, directory, MAX_PATH);
    if (!selfLength || selfLength >= MAX_PATH)
        return failProbe(why, "EDVR module path for AMD's FSR 4 DLLs could not be read (Win32=%lu)", GetLastError());
    wchar_t* const slash = wcsrchr(directory, L'\\');
    if (!slash) return failProbe(why, "EDVR module path has no directory for AMD's FSR 4 DLLs");
    slash[1] = L'\0';   // directory now ends in a backslash
    const size_t directoryLength = wcslen(directory);

    // Both of AMD's DLLs must sit beside EDVR: the loader finds the upscaler next to itself. A missing one is named.
    struct Beside {
        const wchar_t* wideName;
        const char* name;
        const char* because;
        wchar_t path[MAX_PATH];
    } files[2] = {
        {L"amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_loader_dx12.dll", "AMD's FSR 4 runtime is not installed there", {}},
        {L"amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll", "AMD's loader needs it for the upscaler", {}},
    };
    for (Beside& file : files) {
        const size_t nameLength = wcslen(file.wideName);
        if (directoryLength + nameLength >= MAX_PATH)
            return failProbe(why, "EDVR module directory path is too long for %s", file.name);
        wcscpy_s(file.path, MAX_PATH, directory);
        wcscat_s(file.path, MAX_PATH, file.wideName);
        const DWORD attributes = GetFileAttributesW(file.path);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                return failProbe(why, "%s is missing beside EDVR (%s)", file.name, file.because);
            return failProbe(why, "%s beside EDVR cannot be inspected (Win32=%lu)", file.name, code);
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return failProbe(why, "%s beside EDVR is a directory", file.name);
    }
    const wchar_t* const loaderPath = files[0].path;
    state.loaderModule = LoadLibraryExW(loaderPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!state.loaderModule) {
        const DWORD code = GetLastError();
        if (code == ERROR_MOD_NOT_FOUND)
            return failProbe(why, "amd_fidelityfx_loader_dx12.dll exists beside EDVR but a dependency could not be loaded (Win32=126; check the VC++ 2015+ runtime)");
        return failProbe(why, "amd_fidelityfx_loader_dx12.dll beside EDVR failed to load (Win32=%lu)", code);
    }
    wchar_t loadedPath[MAX_PATH]{};
    const DWORD loadedLength = GetModuleFileNameW(state.loaderModule, loadedPath, MAX_PATH);
    if (!loadedLength || loadedLength >= MAX_PATH)
        return failProbe(why, "loaded AMD loader path could not be verified");
    if (_wcsicmp(loadedPath, loaderPath) != 0)
        return failProbe(why, "amd_fidelityfx_loader_dx12.dll resolved to another module (expected %ls, loaded %ls)", loaderPath, loadedPath);
    Log::get().note("fsr4: runtime module %ls", loadedPath);
    FfxApi& api = state.api;
    if (!loadProc(state.loaderModule, "ffxCreateContext", api.create) ||
        !loadProc(state.loaderModule, "ffxDestroyContext", api.destroy) ||
        !loadProc(state.loaderModule, "ffxConfigure", api.configure) ||
        !loadProc(state.loaderModule, "ffxQuery", api.query) ||
        !loadProc(state.loaderModule, "ffxDispatch", api.dispatch))
        return failProbe(why, "amd_fidelityfx_loader_dx12.dll lacks a required ffx-api export");

    // Route AMD's runtime errors into the log from here on, so a refusal inside it has its own words beside ours. Best effort:
    // a runtime that declines this changes nothing else.
    ffxConfigureDescGlobalDebug1 debug{};
    debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
    debug.fpMessage = &ffxMessage;
    debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
    const ffxReturnCode_t configured = api.configure(nullptr, &debug.header);
    Log::get().note("fsr4: runtime error messages routed to this log, ffxConfigure rc=%u", unsigned(configured));

    // D7: which versions does AMD's runtime offer for this device? Only a 4.x one lets fsr4 run.
    std::vector<std::string> names;
    std::vector<uint64_t> ids;
    if (!queryOffered(state, names, ids, why)) return false;
    const std::string offered = names.empty() ? std::string("no FSR version") : "FSR " + fsr4FormatVersionList(names);
    Log::get().note("fsr4: AMD's runtime offers %s for this device (%u version%s)", offered.c_str(), unsigned(names.size()),
                    names.size() == 1 ? "" : "s");
    Fsr4Version picked;
    const int pick = fsr4PickVersion(names, &picked);
    if (pick < 0) {
        const std::string text = fsr4RefusalText(names, underWine());
        return failProbe(why, "%s", text.c_str());
    }
    state.versionId = ids[static_cast<size_t>(pick)];
    state.versionName = names[static_cast<size_t>(pick)];
    Log::get().note("fsr4: using AMD's provider %s (version id 0x%llX), the highest 4.x offered", fsr4FormatVersion(picked).c_str(),
                    static_cast<unsigned long long>(state.versionId));

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = state.d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(state.queue.GetAddressOf()));
    if (FAILED(hr)) return failProbe(why, "D3D12 queue failed 0x%08X", unsigned(hr));
    // The queue's timestamp ticks convert to milliseconds for bridge timing. A zero frequency switches that timing off.
    if (FAILED(state.queue->GetTimestampFrequency(&state.d12TimestampHz))) state.d12TimestampHz = 0;
    hr = state.d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(state.fence12.GetAddressOf()));
    if (FAILED(hr)) return failProbe(why, "D3D12 shared fence failed 0x%08X", unsigned(hr));
    hr = state.d12->CreateSharedHandle(state.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &state.sharedFenceHandle);
    if (FAILED(hr)) return failProbe(why, "D3D12 fence NT handle failed 0x%08X", unsigned(hr));
    hr = state.d11v5->OpenSharedFence(state.sharedFenceHandle, IID_PPV_ARGS(state.fence11.GetAddressOf()));
    if (FAILED(hr)) return failProbe(why, "D3D11 shared fence open failed 0x%08X", unsigned(hr));
    state.cpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.cpuEvent) return failProbe(why, "GPU fence CPU event failed (Win32=%lu)", GetLastError());
    Log::get().note("fsr4: D3D11/D3D12 shared queue and fence ready");
    return true;
}
#endif  // EDVR_HAVE_FSR4
}  // namespace

bool fsr4BuiltIn() { return EDVR_HAVE_FSR4 != 0; }

const char* fsr4VersionLabel() { return g_label; }

bool fsr4Available(ID3D11Device* dev, const char** why) {
#if !EDVR_HAVE_FSR4
    (void)dev;
    return failNotBuilt(why);
#else
    // flatRuntimeResize can clear its device pointer before it sees the new one. The backend itself must detect a changed owner
    // on every entry.
    if (dev && g_available && g_state && g_state->d11.Get() != dev) {
        Log::get().note("fsr4: D3D11 device changed; releasing old backend before rebind");
        fsr4Shutdown();
    }
    // An availability probe that failed belongs to the device it ran on. Offered a different one, probe again.
    if (dev && g_tried && !g_available && g_failedProbeDevice != dev) {
        Log::get().note("fsr4: D3D11 device changed after a failed availability probe; probing the new device");
        fsr4Shutdown();
    }
    if (g_tried) {
        if (g_available && g_state && g_state->d11.Get() == dev) {
            if (why) *why = nullptr;
            return true;
        }
        if (g_available) return fail(why, "FSR 4 backend is bound to a different D3D11 device");
        if (why) *why = g_probeReason;   // the cached refusal: said once when it was made, not again every frame
        return false;
    }
    g_tried = true;
    if (!dev) return failProbe(why, "D3D11 device is null");
    dev->AddRef();
    g_failedProbeDevice = dev;   // dropped again below once the probe succeeds
    auto state = std::make_unique<State>();
    state->d11 = dev;
    HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(state->d11v5.GetAddressOf()));
    if (FAILED(hr)) return failProbe(why, "ID3D11Device5/shared fences unavailable 0x%08X", unsigned(hr));
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = dev->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) return failProbe(why, "IDXGIDevice unavailable 0x%08X", unsigned(hr));
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) return failProbe(why, "D3D11 adapter query failed 0x%08X", unsigned(hr));
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) return failProbe(why, "D3D11 adapter description failed 0x%08X", unsigned(hr));
    state->d3d12Module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!state->d3d12Module) return failProbe(why, "d3d12.dll unavailable (Win32=%lu)", GetLastError());
    CreateD3D12DeviceFn createD3D12 = nullptr;
    if (!loadProc(state->d3d12Module, "D3D12CreateDevice", createD3D12))
        return failProbe(why, "D3D12CreateDevice export unavailable");
    // Feature level 12_0, where XeSS's bridge asks 11_0: AMD's runtime filters the versions it offers by this device (FSR 4 needs
    // shader model 6.6), and a GPU that cannot make a 12_0 device cannot run FSR 4 whatever else it offers.
    hr = createD3D12(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(state->d12.GetAddressOf()));
    if (FAILED(hr)) return failProbe(why, "D3D12CreateDevice (feature level 12_0) on the D3D11 adapter failed 0x%08X", unsigned(hr));
    const LUID d12Luid = state->d12->GetAdapterLuid();
    if (d12Luid.HighPart != desc.AdapterLuid.HighPart || d12Luid.LowPart != desc.AdapterLuid.LowPart)
        return failProbe(why, "D3D11/D3D12 adapter LUID mismatch");
    Log::get().note("fsr4: D3D11/D3D12 adapter matched LUID=%08X:%08X vendor=0x%04X device=0x%04X wine=%u",
                    unsigned(desc.AdapterLuid.HighPart), unsigned(desc.AdapterLuid.LowPart),
                    desc.VendorId, desc.DeviceId, underWine() ? 1u : 0u);
    if (!probeRuntime(*state, why)) return false;
    g_state = state.release();
    g_available = true;
    forgetFailedProbeDevice();   // the probe passed; g_state->d11 holds the device now
    if (why) *why = nullptr;
    return true;
#endif
}

bool fsr4Evaluate(ID3D11DeviceContext* ctx, unsigned slot, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out,
                  uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, float jx, float jy,
                  bool reset, float frameMs, float nearZ, float farZ, float fovY,
                  const char** why, bool infiniteDepth, bool hdr) {
#if !EDVR_HAVE_FSR4
    (void)ctx; (void)slot; (void)colour; (void)depth; (void)mv; (void)out; (void)w; (void)h; (void)outW; (void)outH;
    (void)jx; (void)jy; (void)reset; (void)frameMs; (void)nearZ; (void)farZ; (void)fovY; (void)infiniteDepth; (void)hdr;
    return failNotBuilt(why);
#else
    if (slot >= kUpscalerSlots)
        return fail(why, "FSR 4 upscaler slot %u is outside 0..%u", slot, kUpscalerSlots - 1);
    if (!ctx || !w || !h || !outW || !outH || !std::isfinite(jx) || !std::isfinite(jy))
        return fail(why, "invalid FSR 4 context, dimensions, or jitter");
    LARGE_INTEGER began{};
    QueryPerformanceCounter(&began);
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(device.GetAddressOf());
    if (!fsr4Available(device.Get(), why)) return false;
    State& state = *g_state;
    FeatureSlot& holder = state.slots[slot];
    if (state.untrackedSubmission) {
        constexpr const char* reason = "an unfenced GPU submission is still in flight; FSR 4 resources are retained until process exit";
        if (!state.untrackedRefusalLogged) {
            Log::get().note("fsr4: refused: %s", reason);
            state.untrackedRefusalLogged = true;
        }
        if (why) *why = reason;
        return false;
    }
    const DWORD thread = GetCurrentThreadId();
    if (state.ownerThread && (state.ownerThread != thread || state.ownerContext.Get() != ctx))
        return fail(why, "FSR 4 backend must use its original D3D11 immediate context/render thread");
    if (!state.ownerThread) { state.ownerThread = thread; state.ownerContext = ctx; }
    ComPtr<ID3D11DeviceContext4> ctx4;
    HRESULT hr = ctx->QueryInterface(IID_PPV_ARGS(ctx4.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "ID3D11DeviceContext4/Signal/Wait unavailable 0x%08X", unsigned(hr));
    // Bridge timing is read here, between evaluations, from results the GPU has finished. Its 30 s line is due here too.
    sweepBridge(state, slot, ctx);
    if (holder.window.executed && GetTickCount64() - holder.window.startedAt >= kBridgeReportMs) reportSlot(state, slot);

    // Everything that can refuse for a reason of the caller's inputs does so before any D3D11 work is queued, so a refusal here
    // leaves nothing in flight and latches nothing.
    SourcePlan colourPlan, depthPlan, motionPlan, outPlan;
    if (!planInput(colour, Fsr4Role::Colour, w, h, "color", colourPlan, why) ||
        !planInput(depth, Fsr4Role::Depth, w, h, "depth", depthPlan, why) ||
        !planInput(mv, Fsr4Role::Motion, w, h, "motion", motionPlan, why) ||
        !planOutput(out, outW, outH, outPlan, why))
        return false;
    float camNear = 0.0f, camFar = 0.0f;
    const char* planesWhy = nullptr;
    if (!fsr4CameraPlanes(nearZ, farZ, infiniteDepth, &camNear, &camFar, &planesWhy)) return fail(why, "%s", planesWhy);
    if (!(fovY > 0.0f) || !std::isfinite(fovY)) {
        if (!g_fovFallbackNoted) {
            g_fovFallbackNoted = true;
            Log::get().note("fsr4: no vertical field of view reached the engine (the seam computes it from the eye's tangents), so "
                            "AMD's reprojection runs at the documented 90-degree stand-in. This line prints once.");
        }
        fovY = kFsr4DefaultFovYRadians;
    }

    Fsr4ContextKey key;
    key.inW = w; key.inH = h; key.outW = outW; key.outH = outH;
    key.infiniteDepth = infiniteDepth; key.hdr = hdr;
    key.colourFormat = static_cast<uint32_t>(colourPlan.typed);
    key.depthFormat = static_cast<uint32_t>(depthPlan.typed);
    key.motionFormat = static_cast<uint32_t>(motionPlan.typed);
    key.outFormat = static_cast<uint32_t>(outPlan.typed);
    holder.failedFeature.clearIfDifferent(key);
    if (holder.failedFeature.matches(key)) return returnLatchedFailure(holder.failedFeature, why);
    if (!holder.feature || !(holder.feature->key == key)) {
        // A fence-drain refusal is transient; only a failed create is latched.
        if (!releaseSlot(state, slot, why)) return false;
        const char* createWhy = nullptr;
        bool created = false;
        {
            QuietFeatureFailure quiet;
            created = makeFeature(state, slot, key, &createWhy);
        }
        if (!created) {
            holder.failedFeature.record(key, createWhy);
            Log::get().note("fsr4: slot=%u context create failed in=%ux%u out=%ux%u infinite=%u hdr=%u: %s; this key will not retry until the key or mode changes",
                            slot, w, h, outW, outH, infiniteDepth ? 1u : 0u, hdr ? 1u : 0u, holder.failedFeature.reason());
            return returnLatchedFailure(holder.failedFeature, why);
        }
        // The price is the evaluation's, not the create's: makeFeature logs its own duration, and AMD's model setup would
        // otherwise own the slot's maximum for the session.
        QueryPerformanceCounter(&began);
    }
    Feature& feature = *holder.feature;
    CommandSlot& command = feature.commands[feature.nextCommand++ % kCommandRing];
    // The wait happens only when a command allocator is about to be reused; normal producer/consumer ordering is entirely
    // GPU-fence based. CPU spans for the bridge line: the fence is read before the wait, so "blocked" means the wait really had
    // to wait.
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
        command.list->Close();   // Reset opened it; no D3D12 work was submitted.
        holder.failedFeature.record(key, keyWhy);
        return returnLatchedFailure(holder.failedFeature, why);
    }
    // Bridge timing opens before the input copies: total, then copy-in. The guard cancels them on any failure below.
    BridgeSet* bridge = beginBridge(holder, device.Get(), ctx);
    BridgeGuard guard(bridge, ctx);
    copyInput(ctx, feature.color.d11.Get(), colour, w, h, colourPlan.exact);
    copyInput(ctx, feature.motion.d11.Get(), mv, w, h, motionPlan.exact);
    copyInput(ctx, feature.depth.d11.Get(), depth, w, h, depthPlan.exact);
    giveKeys(inputs);
    const uint64_t producer = ++state.nextFenceValue;
    hr = ctx4->Signal(state.fence11.Get(), producer);
    if (FAILED(hr)) {
        command.list->Close();   // Reset opened it; no D3D12 work was submitted.
        // The three D3D11 input copies may already be queued. Without a fence, releasing their shared textures would race those
        // copies.
        state.untrackedSubmission = true;
        return fail(why, "D3D11 producer Signal(%llu) failed 0x%08X; resources retained",
                    static_cast<unsigned long long>(producer), unsigned(hr));
    }
    if (bridge) {   // copy-in ends after the Signal is enqueued; the wait starts there
        bridge->timers[kBridgeCopyIn].end(ctx);
        bridge->timers[kBridgeWait].begin(device.Get(), ctx);
    }
    ctx->Flush();
    feature.lastUseValue = producer;
    LARGE_INTEGER queueWaitBegan{}, queueWaitEnded{};
    QueryPerformanceCounter(&queueWaitBegan);
    hr = state.queue->Wait(state.fence12.Get(), producer);
    QueryPerformanceCounter(&queueWaitEnded);
    submitMs += qpcSpanMs(queueWaitBegan, queueWaitEnded);
    if (FAILED(hr)) {
        command.list->Close();   // No command list was submitted.
        return fail(why, "D3D12 producer Wait(%llu) failed 0x%08X",
                    static_cast<unsigned long long>(producer), unsigned(hr));
    }
    ID3D12GraphicsCommandList* list = command.list.Get();
    // Inputs go to NON_PIXEL_SHADER_RESOURCE and the output to UNORDERED_ACCESS, which is what AMD's contract asks of a DX12
    // caller (ml.md, "Input resources") and what the FfxApiResource states below declare. The shared textures are in COMMON when
    // D3D12 gets them from D3D11 and go back to COMMON before the D3D11 side touches them again.
    transition(list, feature.color.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, feature.motion.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, feature.depth.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, feature.output.d12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = list;
    dispatch.color = ffxApiGetResourceDX12(feature.color.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ffxApiGetResourceDX12(feature.depth.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(feature.motion.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    // exposure, reactive and transparencyAndComposition stay empty, as route 1 leaves them: preExposure is 1, both routes have
    // AMD's automatic exposure (fsr4CreateFlags), and EDVR has no reactive or transparency mask in this pass.
    // The UAV usage is stated rather than left to the helper, which derives it from the opened resource's own flags: a shared
    // resource opened through vkd3d-proton is not known to carry ALLOW_UNORDERED_ACCESS (makeShared logs what it does carry).
    dispatch.output = ffxApiGetResourceDX12(feature.output.d12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
                                            FFX_API_RESOURCE_USAGE_UAV);
    // dlaa.h's jitter is positive right/down in render pixels, the same axes AMD's contract takes (ml.md, "Camera jitter": the
    // projection is shifted by +2*jx/width and -2*jy/height), and route 1 measured both signs and the motion scale as identities
    // (fsr3_engine.cpp). A motion vector in render pixels needs scale 1: AMD's range is [-width, width] in render pixels.
    dispatch.jitterOffset = FfxApiFloatCoords2D{jx, jy};
    dispatch.motionVectorScale = FfxApiFloatCoords2D{1.0f, 1.0f};
    dispatch.renderSize = FfxApiDimensions2D{w, h};
    dispatch.upscaleSize = FfxApiDimensions2D{outW, outH};
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = fsr4FrameTimeMs(frameMs);
    dispatch.preExposure = 1.0f;
    dispatch.reset = reset || feature.firstDispatch;
    dispatch.cameraNear = camNear;
    dispatch.cameraFar = camFar;
    dispatch.cameraFovAngleVertical = fovY;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = fsr4DispatchFlags(hdr);

    // The two timestamps bracket ffxDispatch alone; the barriers around it are outside the measured span.
    if (command.timingHeap.Get()) list->EndQuery(command.timingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const ffxReturnCode_t rc = state.api.dispatch(&feature.context, &dispatch.header);
    if (rc != FFX_API_RETURN_OK) {
        ++state.failures;
        ++feature.dispatchStrikes;
        // AMD may have recorded part of its work before failing; the next attempt starts its history afresh.
        feature.firstDispatch = true;
        // The command list was never submitted; it is safe to close/reset on the next frame, while the producer signal remains a
        // no-op handoff.
        list->Close();
        char text[160];
        std::snprintf(text, sizeof(text), "ffxDispatch returned %u (%s) (frame %u)", unsigned(rc), ffxReturnName(rc),
                      state.evaluations + 1);
        if (feature.dispatchStrikes >= kDispatchStrikes) {
            // A dispatch that fails every time will keep failing; stop paying the copies for it until the key or mode changes.
            holder.failedFeature.record(key, text);
            Log::get().note("fsr4: slot=%u %u consecutive dispatch failures; this key will not retry until the key or mode changes: %s",
                            slot, feature.dispatchStrikes, text);
            return returnLatchedFailure(holder.failedFeature, why);
        }
        if (state.failures != 1 && state.failures % 300 != 0) {
            QuietFeatureFailure quiet;
            return fail(why, "%s", text);
        }
        return fail(why, "%s", text);
    }
    feature.dispatchStrikes = 0;
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
    if (FAILED(hr)) return fail(why, "D3D12 FSR 4 command close failed 0x%08X", unsigned(hr));
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
    feature.firstDispatch = false;
    hr = ctx4->Wait(state.fence11.Get(), done);
    if (FAILED(hr)) return fail(why, "D3D11 output Wait(%llu) failed 0x%08X",
                                static_cast<unsigned long long>(done), unsigned(hr));
    if (bridge) {   // the wait ends once the D3D12 signal has passed; copy-out starts there
        bridge->timers[kBridgeWait].end(ctx);
        bridge->timers[kBridgeCopyOut].begin(device.Get(), ctx);
    }
    SharedTexture* const result[] = {&feature.output};
    if (!takeKeys(result, &keyWhy, "D3D11 output copy")) {
        holder.failedFeature.record(key, keyWhy);
        return returnLatchedFailure(holder.failedFeature, why);
    }
    // FSR 4 wrote the output in the format the caller's texture has (the HDR route's R16G16B16A16_FLOAT, or the LDR route's
    // RGBA8): a plain copy, no widening.
    ctx->CopyResource(out, feature.output.d11.Get());
    giveKeys(result);
    if (bridge) {   // copy-out and the whole bridge end after the output copy
        bridge->timers[kBridgeCopyOut].end(ctx);
        bridge->timers[kBridgeTotal].end(ctx);
        guard.disarm();
    }
    const uint64_t consumer = ++state.nextFenceValue;
    hr = ctx4->Signal(state.fence11.Get(), consumer);
    if (FAILED(hr)) {
        // The copy work can still be queued on D3D11. Without its completion signal we cannot safely recycle shared textures.
        state.untrackedSubmission = true;
        return fail(why, "D3D11 consumer Signal(%llu) failed 0x%08X; resources retained",
                    static_cast<unsigned long long>(consumer), unsigned(hr));
    }
    ctx->Flush();
    feature.lastUseValue = consumer;
    LARGE_INTEGER ended{};
    QueryPerformanceCounter(&ended);
    const double ms = qpcSpanMs(began, ended);
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
    if (state.evaluations <= 3 || state.evaluations % 3000 == 0)
        Log::get().note("fsr4: slot=%u dispatch #%u rc=0 in=%ux%u out=%ux%u hdr=%u infinite=%u jitter=(%.3f,%.3f) reset=%u fences=%llu/%llu/%llu copy-in=%s/%s/%s provider=%s",
                        slot, state.evaluations, w, h, outW, outH, hdr ? 1u : 0u, infiniteDepth ? 1u : 0u, jx, jy,
                        reset ? 1u : 0u, static_cast<unsigned long long>(producer),
                        static_cast<unsigned long long>(done), static_cast<unsigned long long>(consumer),
                        colourPlan.exact ? "whole" : "box", depthPlan.exact ? "whole" : "box",
                        motionPlan.exact ? "whole" : "box", g_label);
    if (why) *why = nullptr;
    return true;
#endif
}

void fsr4ReleaseFeatures() {
#if EDVR_HAVE_FSR4
    if (!g_state) return;
    // The profile switch releases every slot. Each release has its own bounded GPU wait, so one slot's refusal does not keep the
    // others from being freed.
    for (unsigned slot = 0; slot < kUpscalerSlots; ++slot) {
        g_state->slots[slot].failedFeature.clear();
        if (!releaseSlot(*g_state, slot, nullptr))
            Log::get().note("fsr4: slot=%u retained context after bounded GPU wait refusal; it cannot be freed in flight", slot);
    }
#endif
}

void fsr4Shutdown() {
#if EDVR_HAVE_FSR4
    if (g_state) {
        Log::get().note("fsr4: session evaluations=%u resets=%u failures=%u",
                        g_state->evaluations, g_state->resets, g_state->failures);
        bool drained = true;
        for (unsigned slot = 0; slot < kUpscalerSlots; ++slot)
            if (!releaseSlot(*g_state, slot, nullptr)) drained = false;
        if (!drained) {
            // The GPU still refers to a context's command allocators or shared textures. Process teardown will reclaim them;
            // releasing them here would be a use-after-free on a hung device.
            Log::get().note("fsr4: GPU did not drain; retaining backend objects until process exit");
        } else {
            // A GpuTimer holds its queries and lease until its owner resets it; the owner context is the only one allowed to.
            if (g_state->ownerContext.Get()) cancelBridgeTimers(*g_state, g_state->ownerContext.Get());
            delete g_state;
        }
        g_state = nullptr;
    }
    forgetFailedProbeDevice();
    g_tried = false;
    g_available = false;
    g_fovFallbackNoted = false;   // a re-initialised session says it again
    std::snprintf(g_label, sizeof(g_label), "fsr4");
#endif
}

bool fsr4SlotTotals(unsigned slot, uint32_t* evaluations, uint32_t* resets, double* avgMs, double* maxMs) {
#if EDVR_HAVE_FSR4
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

// Rig seams, as xessTestFeatureCreations is for XeSS: the contexts a slot has built since the backend started, and whether the
// slot holds one now. Both are zero or false in a build without the SDK.
uint32_t fsr4TestContextCreations(unsigned slot) {
#if EDVR_HAVE_FSR4
    return g_state && slot < kUpscalerSlots ? g_state->slots[slot].creations : 0u;
#else
    (void)slot;
    return 0u;
#endif
}

bool fsr4TestContextLive(unsigned slot) {
#if EDVR_HAVE_FSR4
    return g_state && slot < kUpscalerSlots && g_state->slots[slot].feature != nullptr;
#else
    (void)slot;
    return false;
#endif
}

}  // namespace edvr
