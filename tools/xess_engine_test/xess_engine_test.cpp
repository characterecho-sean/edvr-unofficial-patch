// Hardware integration rig for the exact EDVR XeSS bridge. --gpu-test is
// strict; --gpu-test-if-available skips only absent required capabilities.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../src/common/log.h"
#include "../../src/d3d11/xess_engine.h"
#include "../../src/d3d11/xess_feature_latch.h"
#include "../../src/d3d11/xess_runtime_version.h"

using Microsoft::WRL::ComPtr;

namespace edvr {
Log& Log::get() { static auto* log = new Log; return *log; }
void Log::note(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}
}  // namespace edvr

namespace {
bool check(bool okay, const char* stage) {
    if (!okay) std::fprintf(stderr, "xess_engine_test: FAILED %s\n", stage);
    return okay;
}

bool latchSelfTest() {
    edvr::XessFeatureFailureLatch latch;
    const edvr::XessFeatureKey original{640, 360, 1280, 720, false};
    if (!check(!latch.matches(original), "new key is not failed")) return false;
    latch.record(original, "xessD3D12Init returned 7");
    if (!check(latch.matches(original) &&
               std::strcmp(latch.reason(), "xessD3D12Init returned 7") == 0,
               "failed key keeps its precise reason")) return false;
    latch.clearIfDifferent(original);
    if (!check(latch.matches(original), "same key stays failed without retry")) return false;
    const edvr::XessFeatureKey changes[] = {
        {641, 360, 1280, 720, false}, {640, 361, 1280, 720, false},
        {640, 360, 1281, 720, false}, {640, 360, 1280, 721, false},
        {640, 360, 1280, 720, true},
    };
    for (const auto& changed : changes) {
        latch.record(original, "unavailable quality");
        latch.clearIfDifferent(changed);
        if (!check(!latch.matches(original) && !latch.matches(changed),
                   "size or HDR change clears failed key")) return false;
    }
    latch.record(original, "shared texture failed");
    latch.clear();
    return check(!latch.matches(original), "mode reset clears failed key");
}

bool missingCapability(HRESULT hr) {
    return hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_ERROR_NOT_FOUND;
}

bool xessUnsupportedDeviceOrDriver(const char* reason) {
    if (!reason) return false;
    return std::strcmp(reason, "xessD3D12CreateContext returned -1") == 0 ||
           std::strcmp(reason, "xessD3D12CreateContext returned -2") == 0 ||
           std::strcmp(reason, "xessD3D12Init returned -1") == 0 ||
           std::strcmp(reason, "xessD3D12Init returned -2") == 0;
}

bool gateSelfTest() {
    return check(edvr::xessSrRuntimeCompatible(2, 0, 2) &&
                 edvr::xessSrRuntimeCompatible(2, 0, 89) &&
                 !edvr::xessSrRuntimeCompatible(2, 1, 0) &&
                 !edvr::xessSrRuntimeCompatible(3, 0, 2) &&
                 !edvr::xessSrRuntimeCompatible(2, 0, 90),
                 "XeSS-SR version gate rejects incompatible ABI") &&
           check(missingCapability(DXGI_ERROR_UNSUPPORTED) &&
                 missingCapability(DXGI_ERROR_NOT_FOUND) &&
                 !missingCapability(E_FAIL) &&
                 !missingCapability(DXGI_ERROR_DEVICE_REMOVED),
                 "GPU gate skips only explicit missing capabilities") &&
           check(xessUnsupportedDeviceOrDriver("xessD3D12CreateContext returned -1") &&
                 xessUnsupportedDeviceOrDriver("xessD3D12Init returned -2") &&
                 !xessUnsupportedDeviceOrDriver("xessD3D12Init returned -6") &&
                 !xessUnsupportedDeviceOrDriver("D3D11 output Wait failed"),
                 "GPU gate fails real bridge and runtime errors");
}

// The slot check runs before any device use, so a null context proves the
// range without a GPU: slot 3 is refused by range, and slot 2 (the last of
// kUpscalerSlots = 3) passes it and stops at the context check.
bool slotRangeSelfTest() {
    const char* why = nullptr;
    const bool refusedFourth = !edvr::xessEvaluate(nullptr, 3, nullptr, nullptr, nullptr, nullptr,
                                                   1, 1, 1, 1, 0.0f, 0.0f, false, false, &why);
    if (!check(refusedFourth && why && std::strstr(why, "slot 3") != nullptr,
               "upscaler slot 3 is refused by range")) return false;
    why = nullptr;
    const bool acceptedLast = !edvr::xessEvaluate(nullptr, 2, nullptr, nullptr, nullptr, nullptr,
                                                  1, 1, 1, 1, 0.0f, 0.0f, false, false, &why);
    return check(acceptedLast && why && std::strstr(why, "invalid XeSS") != nullptr,
                 "upscaler slot 2 passes the range check");
}

enum class Probe { Ready, Skip, Fail };

Probe hardwareAdapterPresent() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        std::fprintf(stderr, "xess_engine_test: FAILED DXGI factory 0x%08X\n", unsigned(hr));
        return Probe::Fail;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) {
            std::fprintf(stderr, "xess_engine_test: FAILED DXGI adapter enumeration 0x%08X\n", unsigned(hr));
            return Probe::Fail;
        }
        DXGI_ADAPTER_DESC1 desc{};
        hr = adapter->GetDesc1(&desc);
        if (FAILED(hr)) {
            std::fprintf(stderr, "xess_engine_test: FAILED DXGI adapter description 0x%08X\n", unsigned(hr));
            return Probe::Fail;
        }
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return Probe::Ready;
    }
    std::puts("xess_engine_test: SKIP no hardware DXGI adapter");
    return Probe::Skip;
}

Probe optionalHardwareProbe(ID3D11Device* device) {
    ComPtr<ID3D11Device5> d11v5;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(d11v5.GetAddressOf()));
    if (hr == E_NOINTERFACE) {
        std::puts("xess_engine_test: SKIP no ID3D11Device5/shared fences");
        return Probe::Skip;
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "xess_engine_test: FAILED ID3D11Device5 query 0x%08X\n", unsigned(hr));
        return Probe::Fail;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(hr = device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()))) ||
        FAILED(hr = dxgi->GetAdapter(adapter.GetAddressOf()))) {
        std::fprintf(stderr, "xess_engine_test: FAILED D3D11 adapter query 0x%08X\n", unsigned(hr));
        return Probe::Fail;
    }
    struct Module {
        HMODULE handle = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ~Module() { if (handle) FreeLibrary(handle); }
    } module;
    if (!module.handle) {
        std::fprintf(stderr, "xess_engine_test: FAILED System32 d3d12.dll load Win32=%lu\n", GetLastError());
        return Probe::Fail;
    }
    using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create = reinterpret_cast<CreateDevice>(GetProcAddress(module.handle, "D3D12CreateDevice"));
    if (!create) {
        std::puts("xess_engine_test: FAILED D3D12CreateDevice export missing");
        return Probe::Fail;
    }
    ComPtr<ID3D12Device> d12;
    hr = create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d12.GetAddressOf()));
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        std::puts("xess_engine_test: SKIP adapter has no D3D12 device");
        return Probe::Skip;
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "xess_engine_test: FAILED D3D12CreateDevice 0x%08X\n", unsigned(hr));
        return Probe::Fail;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_4};
    hr = d12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
    if (FAILED(hr)) {
        std::fprintf(stderr, "xess_engine_test: FAILED D3D12 shader model query 0x%08X\n", unsigned(hr));
        return Probe::Fail;
    }
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_4) {
        std::fprintf(stdout, "xess_engine_test: SKIP Shader Model %u below XeSS minimum 6.4\n",
                     unsigned(model.HighestShaderModel));
        return Probe::Skip;
    }
    std::puts("xess_engine_test: GPU capability present (D3D11 shared fences, D3D12 Shader Model >=6.4)");
    return Probe::Ready;
}

ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, UINT width, UINT height,
                                 DXGI_FORMAT format, UINT binds) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = binds;
    ComPtr<ID3D11Texture2D> result;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, result.GetAddressOf()))) return {};
    return result;
}

bool clear(ID3D11Device* device, ID3D11DeviceContext* context,
           ID3D11Texture2D* source, const FLOAT color[4]) {
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(device->CreateRenderTargetView(source, nullptr, view.GetAddressOf()))) return false;
    context->ClearRenderTargetView(view.Get(), color);
    return true;
}

bool hasPixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto* pixel = static_cast<const unsigned char*>(mapped.pData) +
                        size_t(desc.Height / 2) * mapped.RowPitch + size_t(desc.Width / 2) *
                        (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
    bool any = false;
    for (unsigned byte = 0; byte < (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u); ++byte)
        any |= pixel[byte] != 0;
    context->Unmap(staging.Get(), 0);
    return any;
}

bool flight(ID3D11Device* device, ID3D11DeviceContext* context,
            UINT width, UINT height, UINT outW, UINT outH, bool hdr,
            const char** failureReason = nullptr, unsigned slot = 0) {
    if (failureReason) *failureReason = nullptr;
    const DXGI_FORMAT colorFormat = hdr ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT outFormat = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : colorFormat;
    const UINT inBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const UINT outBinds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    auto color = texture(device, width, height, colorFormat, inBinds);
    auto depth = texture(device, width, height, DXGI_FORMAT_R32_FLOAT, inBinds);
    auto motion = texture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, inBinds);
    auto output = texture(device, outW, outH, outFormat, outBinds);
    if (!check(color && depth && motion && output, "create input/output textures")) return false;
    constexpr FLOAT lit[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    constexpr FLOAT farDepth[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!check(clear(device, context, color.Get(), lit) &&
               clear(device, context, depth.Get(), farDepth) &&
               clear(device, context, motion.Get(), farDepth), "clear render-grid inputs")) return false;
    for (unsigned frame = 0; frame < 5; ++frame) {
        const char* why = nullptr;
        if (!edvr::xessEvaluate(context, slot, color.Get(), depth.Get(), motion.Get(),
                                output.Get(), width, height, outW, outH,
                                0.0f, 0.0f, frame == 0, hdr, &why)) {
            if (failureReason) *failureReason = why;
            std::fprintf(stderr, "xess_engine_test: frame %u refused: %s\n", frame,
                         why ? why : "unspecified");
            return false;
        }
    }
    if (!check(hasPixel(device, context, output.Get()), "D3D11 readback nonzero center pixel")) return false;
    std::fprintf(stderr, "xess_engine_test: PASS %s %ux%u -> %ux%u, 5 frames slot=%u\n",
                 hdr ? "HDR" : "LDR", width, height, outW, outH, slot);
    return true;
}

bool rebindAfterFailedFeature(ID3D11Device* device, ID3D11DeviceContext* context) {
    constexpr UINT tiny = 1;
    constexpr UINT outputSize = 1280;
    constexpr UINT inBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    auto color = texture(device, tiny, tiny, DXGI_FORMAT_R8G8B8A8_UNORM, inBinds);
    auto depth = texture(device, tiny, tiny, DXGI_FORMAT_R32_FLOAT, inBinds);
    auto motion = texture(device, tiny, tiny, DXGI_FORMAT_R16G16_FLOAT, inBinds);
    auto output = texture(device, outputSize, outputSize, DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS);
    if (!check(color && depth && motion && output, "create out-of-range feature inputs")) return false;
    const char* reason = nullptr;
    const auto evaluate = [&]() {
        return edvr::xessEvaluate(context, 0, color.Get(), depth.Get(), motion.Get(),
                                  output.Get(), tiny, tiny, outputSize, outputSize,
                                  0.0f, 0.0f, false, false, &reason);
    };
    if (!check(!evaluate() && reason && std::strstr(reason, "no XeSS quality accepts input"),
               "out-of-range feature is latched with its precise reason")) return false;
    const char* retainedReason = reason;
    if (!check(!evaluate() && reason == retainedReason,
               "failed feature repeats through stable reason storage")) return false;

    ComPtr<ID3D11Device> replacement;
    ComPtr<ID3D11DeviceContext> replacementContext;
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                         nullptr, 0, D3D11_SDK_VERSION,
                                         replacement.GetAddressOf(), &level,
                                         replacementContext.GetAddressOf());
    if (!check(SUCCEEDED(hr) && replacement.Get() != device, "create replacement D3D11 device")) return false;
    const char* availableWhy = nullptr;
    if (!check(edvr::xessAvailable(replacement.Get(), &availableWhy),
               "XeSS rebinds directly when the D3D11 device changes")) return false;
    if (!check(std::strstr(retainedReason, "no XeSS quality accepts input") != nullptr,
               "failure reason remains readable after old State is destroyed")) return false;
    return flight(replacement.Get(), replacementContext.Get(), 640, 360, 640, 360, false);
}

// Upscaler slots keep separate features. Slot 1 changing size builds slot 1
// alone and leaves slot 0 unbuilt; slot 0 keeps its feature while slot 1 runs;
// slot 3 is refused; the profile switch's release frees every slot, and a freed
// slot rebuilds on its next use. Builds are counted from the backend's start,
// so every check is against a baseline taken after the release.
bool slotIsolation(ID3D11Device* device, ID3D11DeviceContext* context) {
    edvr::xessReleaseFeatures();
    const uint32_t base0 = edvr::xessTestFeatureCreations(0);
    const uint32_t base1 = edvr::xessTestFeatureCreations(1);
    if (!check(!edvr::xessTestFeatureLive(0) && !edvr::xessTestFeatureLive(1),
               "release leaves no slot live")) return false;
    if (!flight(device, context, 640, 360, 1280, 720, false, nullptr, 0)) return false;
    if (!check(edvr::xessTestFeatureLive(0) && !edvr::xessTestFeatureLive(1) &&
               edvr::xessTestFeatureCreations(0) == base0 + 1 && edvr::xessTestFeatureCreations(1) == base1,
               "slot 0 builds alone")) return false;
    if (!flight(device, context, 800, 450, 1600, 900, false, nullptr, 1)) return false;
    if (!check(edvr::xessTestFeatureLive(0) && edvr::xessTestFeatureLive(1) &&
               edvr::xessTestFeatureCreations(0) == base0 + 1 && edvr::xessTestFeatureCreations(1) == base1 + 1,
               "slot 1 at another size builds without rebuilding slot 0")) return false;
    if (!flight(device, context, 640, 360, 1280, 720, false, nullptr, 0)) return false;
    if (!check(edvr::xessTestFeatureCreations(0) == base0 + 1,
               "slot 0 keeps its feature while slot 1 runs")) return false;
    const char* refusal = nullptr;
    if (!check(!flight(device, context, 640, 360, 1280, 720, false, &refusal, 3) &&
               refusal && std::strstr(refusal, "slot 3") != nullptr,
               "upscaler slot 3 is refused on the GPU path")) return false;
    edvr::xessReleaseFeatures();
    if (!check(!edvr::xessTestFeatureLive(0) && !edvr::xessTestFeatureLive(1) &&
               edvr::xessTestFeatureCreations(0) == base0 + 1 && edvr::xessTestFeatureCreations(1) == base1 + 1,
               "release frees every slot without counting a rebuild")) return false;
    if (!flight(device, context, 800, 450, 1600, 900, false, nullptr, 1)) return false;
    return check(edvr::xessTestFeatureLive(1) && edvr::xessTestFeatureCreations(1) == base1 + 2,
                 "a released slot rebuilds on its next use");
}

// Two eyes at an eye-like size, evaluated alternately (slot 0, slot 1, slot 0, ...) for five frames each. Both pass, and
// each eye builds its feature once: the other eye's evaluations never rebuild it, and the outputs hold their own pixels.
bool eyeAlternation(ID3D11Device* device, ID3D11DeviceContext* context) {
    edvr::xessReleaseFeatures();
    const uint32_t base0 = edvr::xessTestFeatureCreations(0);
    const uint32_t base1 = edvr::xessTestFeatureCreations(1);
    constexpr UINT size = 1000;
    constexpr UINT outSize = 2000;
    const UINT inBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const UINT outBinds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> color[2], depth[2], motion[2], output[2];
    constexpr FLOAT lit[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    constexpr FLOAT farDepth[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (unsigned eye = 0; eye < 2; ++eye) {
        color[eye] = texture(device, size, size, DXGI_FORMAT_R8G8B8A8_UNORM, inBinds);
        depth[eye] = texture(device, size, size, DXGI_FORMAT_R32_FLOAT, inBinds);
        motion[eye] = texture(device, size, size, DXGI_FORMAT_R16G16_FLOAT, inBinds);
        output[eye] = texture(device, outSize, outSize, DXGI_FORMAT_R8G8B8A8_UNORM, outBinds);
        if (!check(color[eye] && depth[eye] && motion[eye] && output[eye], "create eye textures")) return false;
        if (!check(clear(device, context, color[eye].Get(), lit) &&
                   clear(device, context, depth[eye].Get(), farDepth) &&
                   clear(device, context, motion[eye].Get(), farDepth), "clear eye inputs")) return false;
    }
    for (unsigned frame = 0; frame < 5; ++frame) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            const char* why = nullptr;
            if (!edvr::xessEvaluate(context, eye, color[eye].Get(), depth[eye].Get(), motion[eye].Get(),
                                    output[eye].Get(), size, size, outSize, outSize,
                                    0.0f, 0.0f, frame == 0, false, &why)) {
                std::fprintf(stderr, "xess_engine_test: eye %u frame %u refused: %s\n", eye, frame,
                             why ? why : "unspecified");
                return check(false, "alternating eye evaluation passes");
            }
        }
    }
    for (unsigned eye = 0; eye < 2; ++eye)
        if (!check(hasPixel(device, context, output[eye].Get()), "alternating eye output has a nonzero pixel")) return false;
    if (!check(edvr::xessTestFeatureCreations(0) == base0 + 1 && edvr::xessTestFeatureCreations(1) == base1 + 1,
               "each eye builds its feature once; alternation rebuilds neither")) return false;
    std::fprintf(stderr, "xess_engine_test: PASS alternating eyes %ux%u -> %ux%u, 5 frames each slot=0,1\n",
                 size, size, outSize, outSize);
    return true;
}
// Forty evaluations alternate over slots 0 and 1 at the eye size. Timing is read back without waiting, so the rig sweeps
// (bounded) until nothing is unread, then reports each slot. Each slot must show both D3D11 and D3D12 samples, with
// nonzero execute and wait medians.
bool bridgeTiming(ID3D11Device* device, ID3D11DeviceContext* context) {
    // A fresh window: the lines below cover only the evaluations of this case.
    edvr::XessBridgeTiming discard;
    for (unsigned slot = 0; slot < 2; ++slot) edvr::xessBridgeTimingReport(context, slot, &discard);
    constexpr UINT size = 1000;
    constexpr UINT outSize = 2000;
    const UINT inBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const UINT outBinds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> color[2], depth[2], motion[2], output[2];
    constexpr FLOAT lit[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    constexpr FLOAT farDepth[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (unsigned eye = 0; eye < 2; ++eye) {
        color[eye] = texture(device, size, size, DXGI_FORMAT_R8G8B8A8_UNORM, inBinds);
        depth[eye] = texture(device, size, size, DXGI_FORMAT_R32_FLOAT, inBinds);
        motion[eye] = texture(device, size, size, DXGI_FORMAT_R16G16_FLOAT, inBinds);
        output[eye] = texture(device, outSize, outSize, DXGI_FORMAT_R8G8B8A8_UNORM, outBinds);
        if (!check(color[eye] && depth[eye] && motion[eye] && output[eye], "create bridge timing textures")) return false;
        if (!check(clear(device, context, color[eye].Get(), lit) &&
                   clear(device, context, depth[eye].Get(), farDepth) &&
                   clear(device, context, motion[eye].Get(), farDepth), "clear bridge timing inputs")) return false;
    }
    for (unsigned n = 0; n < 40; ++n) {
        const unsigned eye = n % 2;
        const char* why = nullptr;
        if (!edvr::xessEvaluate(context, eye, color[eye].Get(), depth[eye].Get(), motion[eye].Get(),
                                output[eye].Get(), size, size, outSize, outSize,
                                0.0f, 0.0f, n < 2, false, &why)) {
            std::fprintf(stderr, "xess_engine_test: bridge timing evaluation %u refused: %s\n", n,
                         why ? why : "unspecified");
            return check(false, "bridge timing evaluations pass");
        }
    }
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (edvr::xessBridgeTimingSweep(context) != 0 && GetTickCount64() < deadline) Sleep(2);
    for (unsigned slot = 0; slot < 2; ++slot) {
        edvr::XessBridgeTiming timing;
        // The engine logs the line itself (timing.line holds the same text), so the build log carries the measured numbers.
        if (!check(edvr::xessBridgeTimingReport(context, slot, &timing), "bridge timing reports slot")) return false;
        if (!check(timing.d11Samples > 0 && timing.d12Samples > 0 &&
                   timing.executeMedian > 0.0 && timing.waitMedian > 0.0,
                   "bridge timing reads D3D11 and D3D12 with nonzero execute and wait")) return false;
        if (!check(timing.cpuEvals > 0 && timing.evalMedian > 0.0 && timing.allocMedian >= 0.0 &&
                   timing.blocked <= timing.cpuEvals && timing.submitTimed,
                   "bridge timing carries CPU spans with nonzero eval time")) return false;
    }
    std::fprintf(stderr, "xess_engine_test: PASS bridge timing, 40 evals on slots 0,1\n");
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("xess_engine_test: strict and capability-gated GPU flights planned; no files or device created");
        return 0;
    }
    if (std::strcmp(argv[1], "--self-test") == 0)
        return check(edvr::xessBuiltIn(), "backend compiled into rig") &&
               latchSelfTest() && gateSelfTest() && slotRangeSelfTest() ? 0 : 1;
    const bool optional = std::strcmp(argv[1], "--gpu-test-if-available") == 0;
    if (!optional && std::strcmp(argv[1], "--gpu-test") != 0) return 2;
    if (optional) {
        const Probe adapters = hardwareAdapterPresent();
        if (adapters != Probe::Ready) return adapters == Probe::Skip ? 0 : 1;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                    nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(),
                                    &level, context.GetAddressOf());
    if (optional && missingCapability(hr)) {
        std::fprintf(stdout, "xess_engine_test: SKIP hardware D3D11 device unavailable 0x%08X\n", unsigned(hr));
        return 0;
    }
    if (!check(SUCCEEDED(hr), "hardware D3D11 device")) return 1;
    if (optional && level < D3D_FEATURE_LEVEL_11_0) {
        std::puts("xess_engine_test: SKIP hardware D3D11 feature level below 11_0");
        return 0;
    }
    if (!check(level >= D3D_FEATURE_LEVEL_11_0, "D3D11 feature level 11_0")) return 1;
    if (optional) {
        const Probe capability = optionalHardwareProbe(device.Get());
        if (capability != Probe::Ready) return capability == Probe::Skip ? 0 : 1;
    }
    const char* why = nullptr;
    if (!edvr::xessAvailable(device.Get(), &why)) {
        std::fprintf(stderr, "xess_engine_test: unavailable: %s\n", why ? why : "unspecified");
        return 1;
    }
    const char* firstFailure = nullptr;
    const bool first = flight(device.Get(), context.Get(), 640, 360, 640, 360, false, &firstFailure);
    if (!first && optional && xessUnsupportedDeviceOrDriver(firstFailure)) {
        std::fprintf(stdout, "xess_engine_test: SKIP XeSS reported unsupported GPU/driver: %s\n", firstFailure);
        edvr::xessShutdown();
        return 0;
    }
    const bool good = first &&
                      flight(device.Get(), context.Get(), 640, 360, 1280, 720, false) &&
                      flight(device.Get(), context.Get(), 640, 360, 1280, 720, true);
    edvr::xessReleaseFeatures();
    const bool recreated = good && flight(device.Get(), context.Get(), 640, 360, 1280, 720, true);
    const bool slots = recreated && slotIsolation(device.Get(), context.Get());
    const bool eyes = slots && eyeAlternation(device.Get(), context.Get());
    const bool bridge = eyes && bridgeTiming(device.Get(), context.Get());
    const bool rebound = bridge && rebindAfterFailedFeature(device.Get(), context.Get());
    edvr::xessShutdown();
    return rebound ? 0 : 1;
}
