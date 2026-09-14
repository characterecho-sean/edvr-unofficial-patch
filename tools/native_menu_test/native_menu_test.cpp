#include "../../src/common/native_menu.h"
#include "../../src/common/frame_flag.h"
#include "../../src/d3d11/menu_panel.h"
#include "../../src/openxr/native_menu_client.h"
#include "../../src/openxr/eye_capture.h"
#include "../../src/openxr/immediate_executor.h"
#include <openxr/openxr.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <cmath>
using Microsoft::WRL::ComPtr;

#pragma comment(linker, "/EXPORT:edvrAcquireNativeMenu")

class InlineExecutor final : public edvr::openxr::ImmediateExecutor {
 public: bool invoke(std::function<void()> callback) override { callback(); return true; }
};

namespace edvr { void perfMonitorNoteEvent(unsigned, double) {} }

static bool readPixels(ID3D11Device* d, ID3D11DeviceContext* c,
                       ID3D11Texture2D* src, std::vector<UINT>& out) {
    D3D11_TEXTURE2D_DESC desc{}; src->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d->CreateTexture2D(&desc, nullptr, &staging))) return false;
    c->CopyResource(staging.Get(), src);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    out.resize(size_t(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(out.data() + size_t(y) * desc.Width,
                    static_cast<const char*>(map.pData) + y * map.RowPitch,
                    size_t(desc.Width) * 4);
    c->Unmap(staging.Get(), 0); return true;
}

static bool makeSource(ID3D11Device* d, ID3D11DeviceContext* c,
                       ComPtr<ID3D11Texture2D>& out, UINT color) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 256;
    desc.ArraySize = desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->CreateTexture2D(&desc, nullptr, &out))) return false;
    std::vector<UINT> pixels(256u * 256u, color);
    c->UpdateSubresource(out.Get(), 0, nullptr, pixels.data(), 256u * 4, 0); return true;
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(3);
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("native_menu_test: dry-run (no WARP device)"); return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    unsigned checks = 0, fails = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks; if (!ok) { ++fails; std::printf("FAIL: %s\n", name); }
    };
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL feature{};
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                      nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context)), "WARP device");
    if (!device) return 1;

    EdvrNativeMenuRequest request{sizeof(request), EDVR_NATIVE_MENU_VERSION_1, device.Get(), 17};
    EdvrNativeMenuTable table{sizeof(table), EDVR_NATIVE_MENU_VERSION_1};
    check(edvrAcquireNativeMenu(&request, &table) == S_OK && table.context, "provider acquire");
    float head[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    float eyes[2][12] = {{1,0,0,-.03f,0,1,0,0,0,0,1,0}, {1,0,0,.03f,0,1,0,0,0,0,1,0}};
    float frusta[2][4] = {{-.9f,1.1f,-1.2f,.8f}, {-1.1f,.9f,-.8f,1.2f}};
    edvr::publishMenuAnchor(head); edvr::setMenuVisible(1.f);
    edvr::MenuGeometry geometry{}; geometry.alpha = 1.f; edvr::menuPanelSetGeometry(geometry);
    edvr::MenuContent content{}; content.widthPx = content.cardPx = 256; content.capPx = 18; content.lineCount = 1;
    std::strcpy(content.lines[0].left, "Native OpenXR"); std::strcpy(content.lines[0].right, "WARP");
    edvr::menuPanelSubmit(content);
    bool ready = false;
    for (unsigned ms = 0; ms < 3000 && !ready; ms += 5) { ready = edvr::menuPanelWorkerReadyForTest(); if (!ready) Sleep(5); }
    check(ready, "bounded raster worker ready"); edvr::menuPanelTick(device.Get());
    check(table.publishPose(table.context, head, eyes, frusta, 17, 3) == S_OK && nativeMenuAvailable(), "publish valid pose");

    ComPtr<ID3D11Texture2D> source; check(makeSource(device.Get(), context.Get(), source, 0xff202020u), "source texture");
    ComPtr<ID3D11ShaderResourceView> sentinelSrv;
    check(SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, &sentinelSrv)), "sentinel SRV");
    D3D11_TEXTURE2D_DESC uavDesc{}; source->GetDesc(&uavDesc); uavDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> uavTexture; ComPtr<ID3D11UnorderedAccessView> sentinelUav;
    check(SUCCEEDED(device->CreateTexture2D(&uavDesc, nullptr, &uavTexture)) &&
          SUCCEEDED(device->CreateUnorderedAccessView(uavTexture.Get(), nullptr, &sentinelUav)), "sentinel UAV");
    ID3D11ShaderResourceView* boundSrv = sentinelSrv.Get(); ID3D11UnorderedAccessView* boundUav = sentinelUav.Get();
    context->CSSetShaderResources(0, 1, &boundSrv); context->CSSetUnorderedAccessViews(0, 1, &boundUav, nullptr);
    float normal[4] = {0,0,1,1}, outBounds[4]{}; ID3D11Texture2D* left = nullptr;
    check(table.treatEye(table.context, 0, source.Get(), normal, &left, outBounds) == S_OK && left, "left real output");
    ID3D11ShaderResourceView* gotSrv = nullptr; ID3D11UnorderedAccessView* gotUav = nullptr;
    context->CSGetShaderResources(0, 1, &gotSrv); context->CSGetUnorderedAccessViews(0, 1, &gotUav);
    check(gotSrv == boundSrv && gotUav == boundUav, "compute bindings restored"); if (gotSrv) gotSrv->Release(); if (gotUav) gotUav->Release();
    float flipped[4] = {1,0,0,1}, flippedOut[4]{}; ID3D11Texture2D* right = nullptr;
    check(table.treatEye(table.context, 1, source.Get(), flipped, &right, flippedOut) == S_OK && right && flippedOut[0] == 1 && flippedOut[2] == 0, "right output preserves flip");
    std::vector<UINT> baseline, leftPixels, rightPixels;
    check(readPixels(device.Get(), context.Get(), source.Get(), baseline), "read source pixels");
    check(left && readPixels(device.Get(), context.Get(), left, leftPixels), "read left pixels");
    check(right && readPixels(device.Get(), context.Get(), right, rightPixels), "read right pixels");
    if (leftPixels.size() == baseline.size() && rightPixels.size() == baseline.size()) {
        unsigned leftChanged = 0, rightChanged = 0;
        for (size_t i = 0; i < baseline.size(); ++i) { leftChanged += leftPixels[i] != baseline[i]; rightChanged += rightPixels[i] != baseline[i]; }
        check(leftChanged > 0 && rightChanged > 0, "both eyes contain panel pixels");
        check(leftChanged < baseline.size() && rightChanged < baseline.size(), "color pass remains outside panel");
    } else check(false, "readback sizes");
    if (left) left->Release(); if (right) right->Release();

    std::atomic<long> wrongThread{0}; std::thread worker([&] { ID3D11Texture2D* o = nullptr; float b[4]{}; wrongThread = table.treatEye(table.context, 0, source.Get(), normal, &o, b); if (o) o->Release(); }); worker.join();
    check(wrongThread == E_INVALIDARG, "wrong producer thread rejected");
    ComPtr<ID3D11Device> foreign; ComPtr<ID3D11DeviceContext> foreignContext; D3D_FEATURE_LEVEL foreignFeature{};
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &foreign, &foreignFeature, &foreignContext)), "foreign WARP device");
    ComPtr<ID3D11Texture2D> foreignSource; check(foreign && makeSource(foreign.Get(), foreignContext.Get(), foreignSource, 0xff303030u), "foreign source");
    ID3D11Texture2D* rejected = nullptr; float rejectBounds[4]{};
    check(table.treatEye(table.context, 0, foreignSource.Get(), normal, &rejected, rejectBounds) == E_INVALIDARG && !rejected, "foreign device rejected");
    float badBounds[4] = {-.1f,0,1,1}; check(table.treatEye(table.context, 0, source.Get(), badBounds, &rejected, rejectBounds) == E_INVALIDARG && !rejected, "bad bounds rejected");
    check(table.publishPose(table.context, nullptr, nullptr, nullptr, 17, 0) == S_OK && !nativeMenuAvailable(), "CPU invalidation");
    check(table.close(table.context) == S_OK && !nativeMenuAvailable(), "CPU close");
    EdvrNativeMenuTable second{sizeof(second), EDVR_NATIVE_MENU_VERSION_1}; EdvrNativeMenuRequest secondRequest{sizeof(secondRequest), EDVR_NATIVE_MENU_VERSION_1, device.Get(), 29};
    check(edvrAcquireNativeMenu(&secondRequest, &second) == S_OK && second.context, "reacquire after close");
    check(second.publishPose(second.context, head, eyes, frusta, 29, 4) == S_OK && nativeMenuAvailable(), "second publish");
    check(table.publishPose(table.context, nullptr, nullptr, nullptr, 17, 0) == E_INVALIDARG && nativeMenuAvailable(), "stale context cannot invalidate active generation");
    check(second.close(second.context) == S_OK && !nativeMenuAvailable(), "second close");
    edvr::openxr::NativeMenuClient client;
    check(client.acquire(GetModuleHandleW(nullptr), device.Get(), 31) == S_OK, "client acquire");
    edvr::openxr::GeometryInput gi{}; gi.generation = 31; gi.sequence = 1;
    gi.viewFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    gi.headFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    gi.headPose.orientation.w = 1.f; gi.width[0] = gi.width[1] = 256; gi.height[0] = gi.height[1] = 256;
    gi.views[0].pose.orientation.w = 1.f; gi.views[0].pose.position.x = -.03f;
    gi.views[1].pose.orientation.w = 1.f; gi.views[1].pose.position.x = .03f;
    gi.views[0].fov = {-atanf(.9f), atanf(1.1f), atanf(.8f), -atanf(1.2f)};
    gi.views[1].fov = {-atanf(1.1f), atanf(.9f), atanf(1.2f), -atanf(.8f)};
    edvr::publishMenuAnchor(head); edvr::setMenuVisible(1.f); edvr::menuPanelSetGeometry(geometry);
    check(client.publish(gi, 1) == S_OK && nativeMenuAvailable(), "client publish geometry");
    ComPtr<ID3D11Texture2D> clientLeft, clientRight; vr::VRTextureBounds_t leftBounds{}, rightBounds{};
    check(client.treat(0, source.Get(), nullptr, clientLeft, leftBounds) == S_OK && clientLeft, "client treat left");
    check(client.treat(1, source.Get(), nullptr, clientRight, rightBounds) == S_OK && clientRight, "client treat right");
    InlineExecutor executor; edvr::openxr::EyeCapture capture;
    check(capture.initializeShared(device.Get(), foreign.Get(), &executor) == S_OK, "shared capture initialize");
    vr::Texture_t clientTexture{clientLeft.Get(), vr::API_DirectX, vr::ColorSpace_Auto};
    vr::VRTextureBounds_t captureLeftBounds{leftBounds.uMin,leftBounds.vMin,leftBounds.uMax,leftBounds.vMax};
    vr::VRTextureBounds_t captureRightBounds{rightBounds.uMin,rightBounds.vMin,rightBounds.uMax,rightBounds.vMax};
    check(clientLeft && capture.capture(vr::Eye_Left, &clientTexture, &captureLeftBounds) == vr::VRCompositorError_None, "shared capture left");
    clientTexture.handle = clientRight.Get();
    check(clientRight && capture.capture(vr::Eye_Right, &clientTexture, &captureRightBounds) == vr::VRCompositorError_None, "shared capture right");
    std::vector<UINT> consumerLeft, consumerRight;
    check(capture.texture(vr::Eye_Left) && readPixels(foreign.Get(), foreignContext.Get(), capture.texture(vr::Eye_Left), consumerLeft), "read shared left");
    check(capture.texture(vr::Eye_Right) && readPixels(foreign.Get(), foreignContext.Get(), capture.texture(vr::Eye_Right), consumerRight), "read shared right");
    std::vector<UINT> expectedLeft, expectedRight;
    check(readPixels(device.Get(), context.Get(), clientLeft.Get(), expectedLeft), "read client left");
    check(readPixels(device.Get(), context.Get(), clientRight.Get(), expectedRight), "read client right");
    check(consumerLeft == expectedLeft && consumerRight == expectedRight, "shared pixels exact");
    check(capture.shutdownShared() == S_OK, "shared capture shutdown"); capture.shutdown();
    check(client.close() == S_OK, "client close");
    edvr::menuPanelShutdown(); std::printf("native_menu_test: %u checks, %u failures\n", checks, fails); return fails ? 1 : 0;
}
