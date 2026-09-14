#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/d3d11_stereo.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/openxr/render_thread_dispatcher.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <array>

using namespace edvr::openxr;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
constexpr unsigned extent = 128;
const auto session = reinterpret_cast<XrSession>(1);
const auto space = reinterpret_cast<XrSpace>(2);
struct Runtime {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> images[2];
    unsigned creates = 0, releases = 0;
    bool acquired[2]{}, waited[2]{};
    DWORD xrThread = 0;
    uint64_t graphicsCallbacks = 0, graphicsAtRelease = 0;
    D3D11Stereo* drainSubject = nullptr;
} runtime;
void xrOwner() {
    if (runtime.xrThread) check(GetCurrentThreadId() == runtime.xrThread, "XR swapchain calls stay on owner thread");
}
unsigned eyeIndex(XrSwapchain chain) {
    const auto i = reinterpret_cast<uintptr_t>(chain) - 10;
    check(i < 2, "valid fake swapchain");
    return i < 2 ? unsigned(i) : 0;
}
XrResult XRAPI_PTR formats(XrSession, uint32_t capacity, uint32_t* count, int64_t* out) {
    xrOwner();
    *count = 1;
    if (capacity) out[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
    xrOwner();
    const unsigned eye = runtime.creates++;
    if (eye >= 2) return XR_ERROR_LIMIT_REACHED;
    check(info->width == extent && info->height == extent, "swapchain extent");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = extent;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(runtime.device->CreateTexture2D(&desc, nullptr, &runtime.images[eye])))
        return XR_ERROR_RUNTIME_FAILURE;
    *out = reinterpret_cast<XrSwapchain>(uintptr_t(10 + eye));
    return XR_SUCCESS;
}
XrResult XRAPI_PTR destroy(XrSwapchain chain) {
    xrOwner();
    if (runtime.drainSubject) check(!runtime.drainSubject->needsGpuDrain(), "GPU completion precedes swapchain destruction");
    runtime.images[eyeIndex(chain)].Reset();
    return XR_SUCCESS;
}
XrResult XRAPI_PTR images(XrSwapchain chain, uint32_t capacity, uint32_t* count,
                         XrSwapchainImageBaseHeader* out) {
    xrOwner();
    *count = 1;
    if (capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(out)->texture = runtime.images[eyeIndex(chain)].Get();
    return XR_SUCCESS;
}
XrResult XRAPI_PTR acquire(XrSwapchain chain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    xrOwner();
    const unsigned eye = eyeIndex(chain);
    check(!runtime.acquired[eye], "acquire once");
    runtime.acquired[eye] = true; *index = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR wait(XrSwapchain chain, const XrSwapchainImageWaitInfo*) {
    xrOwner();
    const unsigned eye = eyeIndex(chain);
    check(runtime.acquired[eye] && !runtime.waited[eye], "wait after acquire");
    runtime.waited[eye] = true;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR release(XrSwapchain chain, const XrSwapchainImageReleaseInfo*) {
    xrOwner();
    if (runtime.xrThread) {
        check(runtime.graphicsCallbacks > runtime.graphicsAtRelease, "immediate submission finishes before XR image release");
        runtime.graphicsAtRelease = runtime.graphicsCallbacks;
    }
    const unsigned eye = eyeIndex(chain);
    check(runtime.waited[eye], "release after wait");
    runtime.waited[eye] = runtime.acquired[eye] = false;
    ++runtime.releases;
    return XR_SUCCESS;
}
uint32_t pixel(ID3D11Texture2D* source, unsigned x, unsigned y) {
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(runtime.device->CreateTexture2D(&desc, nullptr, &staging))) {
        check(false, "staging allocation"); return 0;
    }
    runtime.context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(runtime.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        check(false, "pixel readback"); return 0;
    }
    uint32_t result = 0;
    std::memcpy(&result, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch + x * 4, 4);
    runtime.context->Unmap(staging.Get(), 0);
    return result;
}

using BridgeCounts = void (*)(uint64_t*, uint64_t*);
// Fault-inject only the pre-submission GetDevice call. This object must never
// reach the D3D driver. It models a chained COM implementation reentering and
// throwing so that the provider's admission/permit cleanup is exercised.
struct ListProbe final : ID3D11CommandList {
    GraphicsBridgeClient* client = nullptr;
    ID3D11CommandList* nested = nullptr;
    unsigned calls = 0;
    HRESULT nestedResult = S_OK;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override {
        ++calls;
        if (out) *out = nullptr;
        nestedResult = client->execute(nested);
        throw 1;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetContextFlags() override { return 0; }
};

void bridgeContracts(HMODULE proxy, BridgeCounts counts) {
    auto acquireBridge = reinterpret_cast<EdvrAcquireGraphicsBridge>(
        GetProcAddress(proxy, "edvrAcquireGraphicsBridge"));
    check(acquireBridge != nullptr, "paired bridge export exists");
    if (!acquireBridge) return;
    const EdvrGraphicsBridgeRequest request{sizeof(request), EDVR_GRAPHICS_BRIDGE_VERSION_1,
                                            runtime.device.Get(), runtime.context.Get()};
    auto emptyTable = [] {
        return EdvrGraphicsBridgeTable{sizeof(EdvrGraphicsBridgeTable), EDVR_GRAPHICS_BRIDGE_VERSION_1};
    };
    auto rejected = [&](EdvrGraphicsBridgeRequest invalid) {
        auto table = emptyTable();
        check(FAILED(acquireBridge(&invalid, &table)) && !table.size && !table.version &&
              !table.lease && !table.execute && !table.release, "invalid request clears output");
    };
    auto invalid = request; invalid.version = 2; rejected(invalid);
    invalid = request; invalid.size = sizeof(request) + 8; rejected(invalid);
    invalid = request; invalid.size = 4; rejected(invalid);
    invalid = request; invalid.device = nullptr; rejected(invalid);
    invalid = request; invalid.context = nullptr; rejected(invalid);
    auto table = emptyTable(); table.version = 2;
    check(FAILED(acquireBridge(&request, &table)) && !table.lease && !table.execute && !table.release,
          "unknown output ABI rejected");
    table = emptyTable();
    check(acquireBridge(nullptr, &table) == E_INVALIDARG && !table.lease, "null request rejected");
    check(acquireBridge(&request, nullptr) == E_INVALIDARG, "null output rejected");
    alignas(EdvrGraphicsBridgeTable) std::array<unsigned char, sizeof(EdvrGraphicsBridgeTable) + 16> bytes;
    bytes.fill(0xcc);
    const uint32_t shortSize = 4;
    std::memcpy(bytes.data(), &shortSize, sizeof(shortSize));
    check(acquireBridge(&request, reinterpret_cast<EdvrGraphicsBridgeTable*>(bytes.data())) == E_INVALIDARG,
          "short output table rejected");
    bool canary = true;
    for (size_t i = 4; i < bytes.size(); ++i) canary = canary && bytes[i] == 0xcc;
    check(canary, "short output writes stay inside declared size");

    ComPtr<ID3D11Device> otherDevice;
    ComPtr<ID3D11DeviceContext> otherContext, deferred, otherDeferred;
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &otherDevice, nullptr, &otherContext)), "independent same-adapter device");
    check(SUCCEEDED(runtime.device->CreateDeferredContext(0, &deferred)), "private recording context");
    if (!otherDevice || !deferred) return;
    invalid = request; invalid.device = otherDevice.Get(); invalid.context = otherContext.Get(); rejected(invalid);
    invalid = request; invalid.context = otherContext.Get(); rejected(invalid);
    invalid = request; invalid.context = deferred.Get(); rejected(invalid);

    ComPtr<ID3D11Texture2D> privateTarget;
    ComPtr<ID3D11RenderTargetView> privateRtv;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(SUCCEEDED(runtime.device->CreateTexture2D(&desc, nullptr, &privateTarget)) &&
        SUCCEEDED(runtime.device->CreateRenderTargetView(privateTarget.Get(), nullptr, &privateRtv)),
        "private bridge target");
    if (!privateRtv) return;
    const float cyan[]{0, 1, 1, 1};
    deferred->ClearRenderTargetView(privateRtv.Get(), cyan);
    ComPtr<ID3D11CommandList> list, otherList;
    check(SUCCEEDED(deferred->FinishCommandList(FALSE, &list)) && list, "private bridge list");
    check(SUCCEEDED(otherDevice->CreateDeferredContext(0, &otherDeferred)), "foreign recording context");
    if (!otherDeferred || !list) return;
    otherDeferred->ClearState();
    check(SUCCEEDED(otherDeferred->FinishCommandList(FALSE, &otherList)) && otherList, "foreign device list");
    if (!otherList) return;

    uint64_t privateBefore = 0, unknownBefore = 0;
    counts(&privateBefore, &unknownBefore);
    GraphicsBridgeClient client;
    check(FAILED(client.acquire(GetModuleHandleW(L"kernel32.dll"), runtime.device.Get(), runtime.context.Get())) &&
        !client.active(), "missing paired export fails without fallback");
    check(client.acquire(proxy, runtime.device.Get(), runtime.context.Get()) == S_OK && client.active(),
          "client acquires exact device/context bridge");
    if (!client.active()) return;
    table = emptyTable();
    check(acquireBridge(&request, &table) == E_PENDING && !table.lease && !table.execute && !table.release,
          "second lease rejected without replacing owner");
    HRESULT duplicateThread = S_OK;
    std::thread duplicate([&] {
        auto second = emptyTable();
        duplicateThread = acquireBridge(&request, &second);
        if (second.lease) second.release(second.lease);
    });
    duplicate.join();
    check(duplicateThread == E_PENDING, "second thread cannot designate another lease owner");
    check(FAILED(client.execute(nullptr)), "null list rejected");
    check(FAILED(client.execute(otherList.Get())), "same-adapter foreign list rejected");
    ListProbe probe; probe.client = &client; probe.nested = list.Get();
    HRESULT wrongThread = S_OK;
    std::thread worker([&] { wrongThread = client.execute(&probe); });
    worker.join();
    check(wrongThread == E_ACCESSDENIED && probe.calls == 0,
          "worker submission rejected before any list/device access");
    check(client.execute(&probe) == E_FAIL && probe.calls == 1 && probe.nestedResult == E_PENDING,
          "reentrant submission rejected and COM exception contained");
    uint64_t privateNow = 0, unknownNow = 0;
    counts(&privateNow, &unknownNow);
    check(privateNow == privateBefore && unknownNow == unknownBefore, "rejected calls never reach execute hook");
    check(client.execute(list.Get()) == S_OK, "owner still executes after rejection");
    counts(&privateNow, &unknownNow);
    check(privateNow == privateBefore + 1 && unknownNow == unknownBefore, "exact list classified private once");
    check(pixel(privateTarget.Get(), 0, 0) == 0xffffff00, "private bridge submits actual GPU work");
    client.reset();
    check(!client.active() && FAILED(client.execute(list.Get())), "released client cannot submit");
    // A list's identity is not a permanent tag: replay outside the bridge is
    // still unknown, even if this exact object was previously permitted.
    runtime.context->ExecuteCommandList(list.Get(), TRUE);
    counts(&privateNow, &unknownNow);
    check(privateNow == privateBefore + 1 && unknownNow == unknownBefore + 1,
          "private permit cannot escape its call or mark a list permanently");
    check(client.acquire(proxy, runtime.device.Get(), runtime.context.Get()) == S_OK,
          "new lease allowed after release");
    client.reset();
}

void threadedRenderer(HMODULE proxy, BridgeCounts counts) {
    runtime.creates = runtime.releases = 0;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> targetView;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = extent; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(SUCCEEDED(runtime.device->CreateTexture2D(&desc, nullptr, &target)) &&
        SUCCEEDED(runtime.device->CreateRenderTargetView(target.Get(), nullptr, &targetView)), "threaded game sentinel target");
    if (!targetView) return;
    const float magenta[]{1, 0, 1, 1};
    runtime.context->ClearRenderTargetView(targetView.Get(), magenta);
    runtime.context->OMSetRenderTargets(1, targetView.GetAddressOf(), nullptr);
    using Binding = void* (*)(unsigned, uint32_t*);
    const auto binding = reinterpret_cast<Binding>(GetProcAddress(proxy, "edvr_selftest_binding"));
    void* expected[unsigned(edvr::BindSlot::Count)]{};
    uint32_t generations[unsigned(edvr::BindSlot::Count)]{};
    for (unsigned i = 0; i < unsigned(edvr::BindSlot::Count); ++i) expected[i] = binding(i, &generations[i]);
    auto preserved = [&] {
        for (unsigned i = 0; i < unsigned(edvr::BindSlot::Count); ++i) {
            uint32_t generation = 0;
            check(binding(i, &generation) == expected[i] && generation == generations[i], "threaded render preserves actual binding shadow");
        }
        check(pixel(target.Get(), 64, 60) == 0xffff00ff, "threaded render preserves game pixels");
    };
    OwnerService owner;
    RenderThreadDispatcher render(owner);
    check(owner.start() && render.bindCurrentThread(), "threaded renderer owner/caller setup");
    struct Executor final : ImmediateExecutor {
        RenderThreadDispatcher& render;
        explicit Executor(RenderThreadDispatcher& value) : render(value) {}
        bool invoke(std::function<void()> callback) override {
            return render.invoke([&] {
                check(render.isRenderThread() && GetCurrentThreadId() != runtime.xrThread, "actual graphics work runs on render caller");
                callback(); ++runtime.graphicsCallbacks;
            });
        }
    } executor(render);
    D3D11Stereo renderer;
    runtime.drainSubject = &renderer;
    EyeCapture capture;
    SkyboxCapture skybox;
    XrViewConfigurationView sizes[2]{}; XrView views[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        sizes[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        sizes[i].recommendedImageRectWidth = sizes[i].recommendedImageRectHeight = extent;
        sizes[i].maxImageRectWidth = sizes[i].maxImageRectHeight = extent;
        sizes[i].maxSwapchainSampleCount = 1;
        views[i] = {XR_TYPE_VIEW}; views[i].pose.orientation.w = 1;
        views[i].fov = {-.785398163f, .785398163f, .785398163f, -.785398163f};
    }
    uint64_t privateBefore = 0, unknownBefore = 0;
    counts(&privateBefore, &unknownBefore);
    bool initialized = false;
    check(render.invokeOwner([&] {
        runtime.xrThread = GetCurrentThreadId();
        initialized = renderer.initialize({formats, create, destroy, images, acquire, wait, release},
            session, runtime.device.Get(), sizes, proxy, &executor) == XR_SUCCESS;
    }) && initialized, "renderer acquires paired bridge on caller while initializing on XR owner");
    if (initialized) {
        preserved();
        XrCompositionLayerProjection layer{};
        check(render.invokeOwner([&] { check(renderer.render(views, space, layer) == XR_SUCCESS, "threaded direct stereo"); }), "direct boundary");
        preserved();
        const uint32_t triangle = pixel(runtime.images[0].Get(), 64, 60);
        check((triangle >> 24) == 255 && (triangle & 255) > 60 && ((triangle >> 8) & 255) > 60 &&
            ((triangle >> 16) & 255) > 60 && triangle != 0xffff00ff, "threaded renderer produces actual mixed triangle pixels");
        check(render.invokeOwner([&] {
            check(executor.invoke([&] {
                check(SUCCEEDED(capture.initialize(runtime.device.Get())) && SUCCEEDED(skybox.initialize(runtime.device.Get())), "threaded capture initialize");
            }), "capture initialization boundary");
            ID3D11Texture2D* texture = nullptr;
            for (unsigned eye = 0; eye < 2; ++eye) {
                check(renderer.drawEye(eye, views[eye], texture) == XR_SUCCESS && texture, "threaded diagnostic source");
                if (!texture) return;
                check(executor.invoke([&] {
                    const vr::Texture_t source{texture, vr::API_DirectX, vr::ColorSpace_Linear};
                    check(capture.capture(vr::EVREye(eye), &source) == vr::VRCompositorError_None, "copy eye pixels on render caller");
                }), "eye capture boundary");
            }
            check(renderer.renderCaptured(views, space, capture, layer) == XR_SUCCESS, "threaded copied-eye stereo");
            check(executor.invoke([&] {
                vr::Texture_t faces[6];
                for (auto& face : faces) face = {texture, vr::API_DirectX, vr::ColorSpace_Linear};
                check(skybox.set(faces, 6) == vr::VRCompositorError_None, "copy skybox pixels on render caller");
            }), "skybox copy boundary");
            check(renderer.renderSkybox(views, space, skybox, layer) == XR_SUCCESS, "threaded skybox stereo");
        }), "copied renderer boundary");
        preserved();
        check(pixel(runtime.images[0].Get(), 64, 60) != 0xffff00ff, "threaded private output differs from untouched game target");
        uint64_t privateNow = 0, unknownNow = 0;
        counts(&privateNow, &unknownNow);
        check(privateNow == privateBefore + 8 && unknownNow == unknownBefore, "threaded private lists preserve history classification");
        const auto callbacks = runtime.graphicsCallbacks;
        check(owner.invoke([&] { check(!executor.invoke([]{}), "idle owner cannot use render caller"); }), "idle owner probe completes");
        check(runtime.graphicsCallbacks == callbacks, "idle probe performs no graphics work");
        // A render attempt outside a boundary must fail before any queued list
        // can escape. A later valid boundary must not replay that failed list.
        const unsigned released = runtime.releases;
        check(owner.invoke([&] { check(renderer.render(views, space, layer) == XR_ERROR_RUNTIME_FAILURE, "renderer fails closed without active render caller"); }), "unowned render attempt returns");
        check(render.invokeOwner([&] { check(renderer.render(views, space, layer) == XR_ERROR_RUNTIME_FAILURE, "failed unowned pass cannot replay later"); }), "sticky failure boundary");
        check(runtime.graphicsCallbacks == callbacks && runtime.releases == released, "failed pass performs no execution or image release");
        check(owner.invoke([&] {
            check(renderer.needsGpuDrain(), "submitted work still needs explicit GPU completion");
            check(renderer.shutdown() == XR_ERROR_RUNTIME_FAILURE && renderer.needsGpuDrain(), "shutdown without caller retains pending resources");
            check(runtime.images[0] && runtime.images[1], "failed drain does not destroy swapchain images");
        }), "unowned shutdown returns without graphics access");
        check(runtime.graphicsCallbacks == callbacks, "rejected shutdown does not use immediate context");
        check(render.invokeOwner([&] {
            check(renderer.drain() == XR_SUCCESS && !renderer.needsGpuDrain(), "real GPU event completes on caller after rejected shutdown");
        }), "GPU drain boundary");
        check(runtime.graphicsCallbacks == callbacks + 1, "one synchronous completion callback");
        preserved();
    }
    const auto beforeCleanup = runtime.graphicsCallbacks;
    check(owner.invoke([&] {
        skybox.shutdown(); capture.shutdown();
        check(renderer.shutdown() == XR_SUCCESS, "owner cleanup needs no render callback");
    }), "threaded cleanup finishes on owner");
    check(runtime.graphicsCallbacks == beforeCleanup, "cleanup after drain needs no immediate context access");
    runtime.drainSubject = nullptr;
    // The deliberately failed pass may retain an acquired fake image until
    // destruction. Retire fake bookkeeping with those destroyed handles.
    runtime.acquired[0] = runtime.acquired[1] = runtime.waited[0] = runtime.waited[1] = false;
    preserved();
    render.close(); check(owner.stop(), "threaded owner joins");
    runtime.xrThread = 0;
}

int selfTest(const wchar_t* path) {
    // The actual hook-owning DLL stays loaded until this isolated child exits.
    HMODULE proxy = LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(proxy != nullptr, "load built graphics proxy");
    if (!proxy) return 1;
    auto createDevice = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(proxy, "D3D11CreateDevice"));
    using Binding = void* (*)(unsigned, uint32_t*);
    auto binding = reinterpret_cast<Binding>(GetProcAddress(proxy, "edvr_selftest_binding"));
    auto hooks = reinterpret_cast<unsigned (*)()>(GetProcAddress(proxy, "edvr_selftest_hooks"));
    auto bridgeCounts = reinterpret_cast<BridgeCounts>(GetProcAddress(proxy, "edvr_selftest_graphics_bridge"));
    check(createDevice && binding && hooks && bridgeCounts, "graphics fixture exports available");
    if (!createDevice || !binding || !hooks || !bridgeCounts) return 1;
    check(SUCCEEDED(createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &runtime.device, nullptr, &runtime.context)), "create exact proxy device/context");
    if (!runtime.device || !runtime.context) return 1;
    bridgeContracts(proxy, bridgeCounts);
    if (failures) return 1;
    uint64_t privateBefore = 0, unknownBefore = 0;
    bridgeCounts(&privateBefore, &unknownBefore);

    ComPtr<ID3D11Texture2D> target, source, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = extent;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(SUCCEEDED(runtime.device->CreateTexture2D(&td, nullptr, &target)) &&
        SUCCEEDED(runtime.device->CreateRenderTargetView(target.Get(), nullptr, &rtv)), "game sentinel target");
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check(SUCCEEDED(runtime.device->CreateTexture2D(&td, nullptr, &source)) &&
        SUCCEEDED(runtime.device->CreateShaderResourceView(source.Get(), nullptr, &srv)), "game sentinel source");
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    check(SUCCEEDED(runtime.device->CreateTexture2D(&td, nullptr, &depth)) &&
        SUCCEEDED(runtime.device->CreateDepthStencilView(depth.Get(), nullptr, &dsv)), "game sentinel depth");
    ComPtr<ID3D11Buffer> cb, storage[4];
    ComPtr<ID3D11UnorderedAccessView> uavs[4];
    D3D11_BUFFER_DESC bd{64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    check(SUCCEEDED(runtime.device->CreateBuffer(&bd, nullptr, &cb)), "game sentinel constant buffer");
    bd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
    for (unsigned i = 0; i < 4; ++i)
        check(SUCCEEDED(runtime.device->CreateBuffer(&bd, nullptr, &storage[i])) &&
            SUCCEEDED(runtime.device->CreateUnorderedAccessView(storage[i].Get(), nullptr, &uavs[i])), "game sentinel UAV");
    const char* shader =
        "float4 vs(uint id:SV_VertexID):SV_POSITION{return float4(0,0,0,1);}"
        "float4 ps():SV_TARGET{return float4(1,1,1,1);}"
        "[numthreads(1,1,1)]void cs(uint3 id:SV_DispatchThreadID){}";
    ComPtr<ID3DBlob> vsCode, psCode, csCode;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    auto compile = [&](const char* entry, const char* profile, ID3DBlob** code) {
        return SUCCEEDED(D3DCompile(shader, std::strlen(shader), "game sentinel", nullptr,
            nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, code, nullptr));
    };
    check(compile("vs", "vs_5_0", &vsCode) && compile("ps", "ps_5_0", &psCode) &&
        compile("cs", "cs_5_0", &csCode), "compile game sentinel shaders");
    if (failures) return 1;
    check(SUCCEEDED(runtime.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
        SUCCEEDED(runtime.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)) &&
        SUCCEEDED(runtime.device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, &cs)), "create game sentinel shaders");
    if (failures) return 1;
    const float magenta[]{1, 0, 1, 1};
    runtime.context->ClearRenderTargetView(rtv.Get(), magenta);
    runtime.context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    ID3D11ShaderResourceView* srvs[]{srv.Get(), srv.Get(), srv.Get(), srv.Get()};
    runtime.context->PSSetShaderResources(0, 4, srvs);
    runtime.context->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    runtime.context->VSSetShader(vs.Get(), nullptr, 0);
    runtime.context->PSSetShader(ps.Get(), nullptr, 0);
    runtime.context->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* rawUavs[]{uavs[0].Get(), uavs[1].Get(), uavs[2].Get(), uavs[3].Get()};
    runtime.context->CSSetUnorderedAccessViews(0, 4, rawUavs, nullptr);
    constexpr unsigned slots = unsigned(edvr::BindSlot::Count);
    void* expected[slots]{rtv.Get(), dsv.Get(), srv.Get(), srv.Get(), srv.Get(), srv.Get(), cb.Get(),
        cs.Get(), uavs[0].Get(), uavs[1].Get(), uavs[2].Get(), uavs[3].Get(), vs.Get(), ps.Get()};
    uint32_t generations[slots]{};
    for (unsigned i = 0; i < slots; ++i)
        check(binding(i, &generations[i]) == expected[i], "real proxy setter populated shadow");
    auto preserved = [&] {
        uint64_t privateNow = 0, unknownNow = 0;
        bridgeCounts(&privateNow, &unknownNow);
        check(unknownNow == unknownBefore, "renderer private lists do not enter unknown history invalidation");
        for (unsigned i = 0; i < slots; ++i) {
            uint32_t generation = 0;
            check(binding(i, &generation) == expected[i] && generation == generations[i], "actual DLL shadow identity/generation preserved");
        }
        ComPtr<ID3D11RenderTargetView> current;
        runtime.context->OMGetRenderTargets(1, &current, nullptr);
        check(current.Get() == rtv.Get(), "game target remains bound");
        check(pixel(target.Get(), 64, 60) == 0xffff00ff, "game target pixels unchanged");
    };
    D3D11Stereo renderer;
    XrViewConfigurationView sizes[2]{}; XrView views[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        sizes[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        sizes[i].recommendedImageRectWidth = sizes[i].recommendedImageRectHeight = extent;
        sizes[i].maxImageRectWidth = sizes[i].maxImageRectHeight = extent;
        sizes[i].maxSwapchainSampleCount = 1;
        views[i] = {XR_TYPE_VIEW}; views[i].pose.orientation.w = 1;
        views[i].fov = {-.785398163f, .785398163f, .785398163f, -.785398163f};
    }
    check(renderer.initialize({formats, create, destroy, images, acquire, wait, release},
        session, runtime.device.Get(), sizes, GetModuleHandleW(L"kernel32.dll")) == XR_ERROR_INITIALIZATION_FAILED &&
        runtime.creates == 0, "missing bridge fails renderer before XR swapchains");
    preserved();
    check(renderer.initialize({formats, create, destroy, images, acquire, wait, release},
        session, runtime.device.Get(), sizes, proxy) == XR_SUCCESS, "renderer uses paired hooked game device");
    preserved();
    XrCompositionLayerProjection layer{};
    check(renderer.render(views, space, layer) == XR_SUCCESS, "direct render through hooked context"); preserved();
    const uint32_t drawn = pixel(runtime.images[0].Get(), 64, 60);
    check((drawn >> 24) == 255 && (drawn & 255) > 60 &&
        ((drawn >> 8) & 255) > 60 && ((drawn >> 16) & 255) > 60 &&
        drawn != 0xffff00ff, "direct renderer produced mixed triangle color");
    EyeCapture captured; check(SUCCEEDED(captured.initialize(runtime.device.Get())), "initialize eye capture");
    SkyboxCapture skybox; check(SUCCEEDED(skybox.initialize(runtime.device.Get())), "initialize skybox capture");
    ID3D11Texture2D* eyeTexture = nullptr;
    for (unsigned eye = 0; eye < 2; ++eye) {
        check(renderer.drawEye(eye, views[eye], eyeTexture) == XR_SUCCESS && eyeTexture, "offscreen render through hooked context");
        preserved();
        vr::Texture_t texture{eyeTexture, vr::API_DirectX, vr::ColorSpace_Linear};
        check(captured.capture(vr::EVREye(eye), &texture) == vr::VRCompositorError_None, "capture on hooked context");
    }
    check(renderer.renderCaptured(views, space, captured, layer) == XR_SUCCESS, "copied-eye render through hooked context"); preserved();
    vr::Texture_t faces[6];
    for (auto& face : faces) face = {eyeTexture, vr::API_DirectX, vr::ColorSpace_Linear};
    check(skybox.set(faces, 6) == vr::VRCompositorError_None, "skybox capture through hooked context");
    check(renderer.renderSkybox(views, space, skybox, layer) == XR_SUCCESS, "skybox render through hooked context"); preserved();
    check((hooks() & 2u) != 0 && (hooks() & 1u) == 0, "actual ExecuteCommandList hook ran without immediate ClearState");
    check(runtime.releases == 6, "all composed eyes released");
    check(renderer.shutdown() == XR_SUCCESS, "renderer shutdown"); preserved();
    uint64_t privateNow = 0, unknownNow = 0;
    bridgeCounts(&privateNow, &unknownNow);
    check(privateNow == privateBefore + 8 && unknownNow == unknownBefore,
          "all eight renderer lists use private submission, with no history invalidation");

    ComPtr<ID3D11DeviceContext> gameRecorder;
    ComPtr<ID3D11CommandList> gameList;
    check(SUCCEEDED(runtime.device->CreateDeferredContext(0, &gameRecorder)), "unknown game recorder");
    if (!gameRecorder) return 1;
    const float green[]{0, 1, 0, 1};
    gameRecorder->ClearRenderTargetView(rtv.Get(), green);
    check(SUCCEEDED(gameRecorder->FinishCommandList(FALSE, &gameList)) && gameList, "unknown game target write list");
    if (!gameList) return 1;
    runtime.context->ExecuteCommandList(gameList.Get(), TRUE);
    bridgeCounts(&privateNow, &unknownNow);
    check(unknownNow == unknownBefore + 1 && privateNow == privateBefore + 8,
          "ordinary game list retains conservative history invalidation");
    check(pixel(target.Get(), 64, 60) == 0xff00ff00, "ordinary game list actually writes game target");
    for (unsigned i = 0; i < slots; ++i) {
        uint32_t generation = 0;
        check(binding(i, &generation) == expected[i] && generation == generations[i],
              "unknown restore TRUE preserves bindings while invalidating content history");
    }
    runtime.context->ExecuteCommandList(gameList.Get(), FALSE);
    bridgeCounts(&privateNow, &unknownNow);
    check(unknownNow == unknownBefore + 2 && privateNow == privateBefore + 8,
          "restore FALSE remains unknown");
    for (unsigned i = 0; i < slots; ++i)
        check(binding(i, nullptr) == nullptr, "restore FALSE clears binding shadow");
    threadedRenderer(proxy, bridgeCounts);
    runtime.context->ClearState();
    // Teardown the fixture's own bindings only after checking renderer cleanup.
    std::printf("openxr_proxy_state_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("Would test renderer state through the adjacent built graphics proxy using WARP/fake XR; no files, device or runtime created.");
        return 0;
    }
    if (std::strcmp(argv[1], "--self-test")) return 2;
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 2;
    std::wstring path(executable, length);
    path.resize(path.find_last_of(L"\\/") + 1); path += L"d3d11.dll";
    return selfTest(path.c_str());
}
