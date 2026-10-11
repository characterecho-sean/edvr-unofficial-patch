// Hardware integration rig for the exact EDVR XeSS bridge. Run --gpu-test
// inside the disposable Proton prefix with the selected Intel Vulkan ICD.
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../src/common/log.h"
#include "../../src/d3d11/xess_engine.h"

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
            UINT width, UINT height, UINT outW, UINT outH, bool hdr) {
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
        if (!edvr::xessEvaluate(context, 0, color.Get(), depth.Get(), motion.Get(),
                                output.Get(), width, height, outW, outH,
                                0.0f, 0.0f, frame == 0, hdr, &why)) {
            std::fprintf(stderr, "xess_engine_test: frame %u refused: %s\n", frame,
                         why ? why : "unspecified");
            return false;
        }
    }
    if (!check(hasPixel(device, context, output.Get()), "D3D11 readback nonzero center pixel")) return false;
    std::fprintf(stderr, "xess_engine_test: PASS %s %ux%u -> %ux%u, 5 frames\n",
                 hdr ? "HDR" : "LDR", width, height, outW, outH);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("xess_engine_test: hardware flights planned; no files or device created");
        return 0;
    }
    if (std::strcmp(argv[1], "--self-test") == 0)
        return check(edvr::xessBuiltIn(), "backend compiled into rig") ? 0 : 1;
    if (std::strcmp(argv[1], "--gpu-test") != 0) return 2;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                    nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(),
                                    &level, context.GetAddressOf());
    if (!check(SUCCEEDED(hr) && level >= D3D_FEATURE_LEVEL_11_0, "hardware D3D11 device")) return 1;
    const char* why = nullptr;
    if (!edvr::xessAvailable(device.Get(), &why)) {
        std::fprintf(stderr, "xess_engine_test: unavailable: %s\n", why ? why : "unspecified");
        return 1;
    }
    const bool good = flight(device.Get(), context.Get(), 640, 360, 640, 360, false) &&
                      flight(device.Get(), context.Get(), 640, 360, 1280, 720, false) &&
                      flight(device.Get(), context.Get(), 640, 360, 1280, 720, true);
    edvr::xessReleaseFeatures();
    const bool recreated = good && flight(device.Get(), context.Get(), 640, 360, 1280, 720, true);
    edvr::xessShutdown();
    return recreated ? 0 : 1;
}
