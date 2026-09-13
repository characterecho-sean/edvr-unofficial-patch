#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/d3d11_stereo.h"
#include "../../src/d3d11/binding_shadow.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>

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
} runtime;
unsigned eyeIndex(XrSwapchain chain) {
    const auto i = reinterpret_cast<uintptr_t>(chain) - 10;
    check(i < 2, "valid fake swapchain");
    return i < 2 ? unsigned(i) : 0;
}
XrResult XRAPI_PTR formats(XrSession, uint32_t capacity, uint32_t* count, int64_t* out) {
    *count = 1;
    if (capacity) out[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
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
    runtime.images[eyeIndex(chain)].Reset();
    return XR_SUCCESS;
}
XrResult XRAPI_PTR images(XrSwapchain chain, uint32_t capacity, uint32_t* count,
                         XrSwapchainImageBaseHeader* out) {
    *count = 1;
    if (capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(out)->texture = runtime.images[eyeIndex(chain)].Get();
    return XR_SUCCESS;
}
XrResult XRAPI_PTR acquire(XrSwapchain chain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    const unsigned eye = eyeIndex(chain);
    check(!runtime.acquired[eye], "acquire once");
    runtime.acquired[eye] = true; *index = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR wait(XrSwapchain chain, const XrSwapchainImageWaitInfo*) {
    const unsigned eye = eyeIndex(chain);
    check(runtime.acquired[eye] && !runtime.waited[eye], "wait after acquire");
    runtime.waited[eye] = true;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR release(XrSwapchain chain, const XrSwapchainImageReleaseInfo*) {
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
    check(createDevice && binding && hooks, "graphics fixture exports available");
    if (!createDevice || !binding || !hooks) return 1;
    check(SUCCEEDED(createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &runtime.device, nullptr, &runtime.context)), "create exact proxy device/context");
    if (!runtime.device || !runtime.context) return 1;

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
        session, runtime.device.Get(), sizes) == XR_SUCCESS, "renderer uses hooked game device");
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
