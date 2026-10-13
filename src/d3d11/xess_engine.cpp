#include "xess_engine.h"

#include <windows.h>
#include <d3d11.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../common/log.h"

#ifndef EDVR_HAVE_XESS
#define EDVR_HAVE_XESS 0
#endif

#if EDVR_HAVE_XESS
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <xess/xess_d3d12.h>
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
constexpr unsigned kFlatSlot = 0;
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
};

struct Feature {
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
    uint64_t nextFenceValue = 0;
    uint32_t evaluations = 0, resets = 0, failures = 0;
    bool untrackedSubmission = false;
    bool untrackedRefusalLogged = false;
    XessFeatureFailureLatch failedFeature;
    std::unique_ptr<Feature> feature;
    ~State() {
        // Normal shutdown has already destroyed the XeSS context after the
        // consumer fence. Release COM objects before unloading the modules
        // that supply their vtables, including on an availability failure.
        feature.reset();
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

bool releaseFeature(State& state, const char** why) {
    if (!state.feature) return true;
    if (state.untrackedSubmission)
        return fail(why, "a submitted D3D12 command list has no completed fence; retaining its resources");
    if (!waitFor(state, state.feature->lastUseValue, why, "release")) return false;
    if (state.feature->context) {
        const xess_result_t rc = state.api.destroyContext(state.feature->context);
        Log::get().note("xess: destroy context rc=%d after %u evaluations", int(rc), state.evaluations);
        state.feature->context = nullptr;
    }
    state.feature.reset();
    return true;
}

bool makeShared(State& state, SharedTexture& target, const char* role,
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
    Log::get().note("xess: shared %s %ux%u format=%u D3D11->D3D12 open ok",
                    role, width, height, unsigned(format));
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
    Log::get().note("xess: quality=%s input=%ux%u output=%ux%u optimal=%ux%u bounds=%ux%u..%ux%u",
                    selected->name, feature.inW, feature.inH, feature.outW, feature.outH,
                    selectedOptimal.x, selectedOptimal.y, selectedMin.x, selectedMin.y,
                    selectedMax.x, selectedMax.y);
    return true;
}

bool makeFeature(State& state, uint32_t w, uint32_t h, uint32_t outW,
                 uint32_t outH, bool hdr, const char** why) {
    auto feature = std::make_unique<Feature>();
    feature->inW = w; feature->inH = h; feature->outW = outW;
    feature->outH = outH; feature->hdr = hdr;
    const xess_result_t created = state.api.createContext(state.d12.Get(), &feature->context);
    if (created != XESS_RESULT_SUCCESS || !feature->context)
        return fail(why, "xessD3D12CreateContext returned %d", int(created));
    Log::get().note("xess: D3D12 context create rc=%d ptr=%p", int(created), feature->context);
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
    Log::get().note("xess: D3D12 init rc=%d flags=0x%X quality=%u output=%ux%u",
                    int(initialized), init.initFlags, unsigned(feature->quality), outW, outH);
    xess_version_t intel{};
    const xess_result_t intelRc = state.api.getIntelVersion(feature->context, &intel);
    Log::get().note("xess: Intel XeFX query rc=%d version=%u.%u.%u.%u (diagnostic only)",
                    int(intelRc), intel.major, intel.minor, intel.patch, intel.reserved);
    const DXGI_FORMAT colorFormat = hdr ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!makeShared(state, feature->color, "color", w, h, colorFormat, false, why) ||
        !makeShared(state, feature->motion, "motion", w, h, DXGI_FORMAT_R16G16_FLOAT, false, why) ||
        !makeShared(state, feature->depth, "depth", w, h, DXGI_FORMAT_R32_FLOAT, false, why) ||
        !makeShared(state, feature->output, "output", outW, outH, colorFormat, true, why))
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
        Log::get().note("xess: HDR R11 matched SDK output -> FP16 flat target via precompiled copy shader");
    }
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
    }
    Log::get().note("xess: three command allocators ready; backend feature established");
    state.feature = std::move(feature);
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
    if (slot != kFlatSlot) return fail(why, "XeSS-SR is flat slot 0 only (got %u)", slot);
    if (!context || !width || !height || !outputWidth || !outputHeight ||
        !std::isfinite(jitterX) || !std::isfinite(jitterY))
        return fail(why, "invalid XeSS flat context, dimensions, or jitter");
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!xessAvailable(device.Get(), why)) return false;
    State& state = *g_state;
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
        return fail(why, "XeSS flat backend must use its original D3D11 immediate context/render thread");
    if (!state.ownerThread) { state.ownerThread = thread; state.ownerContext = context; }
    ComPtr<ID3D11DeviceContext4> ctx4;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(ctx4.GetAddressOf()));
    if (FAILED(hr)) return fail(why, "ID3D11DeviceContext4/Signal/Wait unavailable 0x%08X", unsigned(hr));
    const DXGI_FORMAT colorFormat = hdr ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!validateTexture(colour, width, height, colorFormat, "color", why) ||
        !validateTexture(depth, width, height, DXGI_FORMAT_R32_FLOAT, "depth", why) ||
        !validateTexture(motion, width, height, DXGI_FORMAT_R16G16_FLOAT, "motion", why) ||
        !validateTexture(output, outputWidth, outputHeight,
                         hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : colorFormat, "output", why))
        return false;
    const XessFeatureKey key{width, height, outputWidth, outputHeight, hdr};
    state.failedFeature.clearIfDifferent(key);
    if (state.failedFeature.matches(key)) return returnLatchedFailure(state.failedFeature, why);
    if (!state.feature || state.feature->inW != width || state.feature->inH != height ||
        state.feature->outW != outputWidth || state.feature->outH != outputHeight ||
        state.feature->hdr != hdr) {
        // A fence-drain refusal is transient; only a failed create is latched.
        if (!releaseFeature(state, why)) return false;
        const char* createWhy = nullptr;
        bool created = false;
        {
            QuietFeatureFailure quiet;
            created = makeFeature(state, width, height, outputWidth, outputHeight, hdr, &createWhy);
        }
        if (!created) {
            state.failedFeature.record(key, createWhy);
            Log::get().note("xess: feature create failed in=%ux%u out=%ux%u hdr=%u: %s; this key will not retry until the key or mode changes",
                            width, height, outputWidth, outputHeight, hdr ? 1u : 0u,
                            state.failedFeature.reason());
            return returnLatchedFailure(state.failedFeature, why);
        }
    }
    Feature& feature = *state.feature;
    CommandSlot& command = feature.commands[feature.nextCommand++ % kCommandRing];
    // The wait happens only when a command allocator is about to be reused;
    // normal producer/consumer ordering is entirely GPU-fence based.
    if (!waitFor(state, command.doneValue, why, "allocator reuse")) return false;
    hr = command.allocator->Reset();
    if (FAILED(hr)) return fail(why, "D3D12 command allocator reset failed 0x%08X", unsigned(hr));
    hr = command.list->Reset(command.allocator.Get(), nullptr);
    if (FAILED(hr)) return fail(why, "D3D12 command list reset failed 0x%08X", unsigned(hr));
    SharedTexture* const inputs[] = {&feature.color, &feature.motion, &feature.depth};
    const char* keyWhy = nullptr;
    if (!takeKeys(inputs, &keyWhy, "D3D11 input copy")) {
        command.list->Close(); // Reset opened it; no D3D12 work was submitted.
        state.failedFeature.record(key, keyWhy);
        return returnLatchedFailure(state.failedFeature, why);
    }
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
    context->Flush();
    feature.lastUseValue = producer;
    hr = state.queue->Wait(state.fence12.Get(), producer);
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
    state.queue->ExecuteCommandLists(1, lists);
    const uint64_t done = ++state.nextFenceValue;
    hr = state.queue->Signal(state.fence12.Get(), done);
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
    SharedTexture* const result[] = {&feature.output};
    if (!takeKeys(result, &keyWhy, "D3D11 output copy")) {
        state.failedFeature.record(key, keyWhy);
        return returnLatchedFailure(state.failedFeature, why);
    }
    if (hdr) {
        const bool widened = widenHdr(context, feature, output, why);
        giveKeys(result);
        if (!widened) return false;
    } else {
        context->CopyResource(output, feature.output.d11.Get());
        giveKeys(result);
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
    ++state.evaluations;
    if (reset) ++state.resets;
    if (state.evaluations <= 3 || state.evaluations % 300 == 0)
        Log::get().note("xess: execute #%u rc=0 in=%ux%u out=%ux%u hdr=%u jitter=(%.3f,%.3f) reset=%u fences=%llu/%llu/%llu copy=%s",
                        state.evaluations, width, height, outputWidth, outputHeight,
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
    if (g_state) g_state->failedFeature.clear();
    if (g_state && !releaseFeature(*g_state, nullptr))
        Log::get().note("xess: retained feature after bounded GPU wait refusal; it cannot be freed in flight");
#endif
}

void xessShutdown() {
#if EDVR_HAVE_XESS
    if (g_state) {
        Log::get().note("xess: session evaluations=%u resets=%u failures=%u",
                        g_state->evaluations, g_state->resets, g_state->failures);
        if (!releaseFeature(*g_state, nullptr)) {
            // The GPU still refers to the feature's command allocators or
            // shared textures. Process teardown will reclaim them; releasing
            // them here would be a use-after-free on a hung device.
            Log::get().note("xess: GPU did not drain; retaining backend objects until process exit");
        } else {
            delete g_state;
        }
        g_state = nullptr;
    }
    g_tried = false;
    g_available = false;
#endif
}

}  // namespace edvr
