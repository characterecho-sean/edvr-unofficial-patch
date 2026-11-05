#include "../../src/common/native_menu.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/parallel_projection_notice.h"
#include "../../src/d3d11/menu_panel.h"
#include "../../src/openxr/native_menu_client.h"
#include "../../src/openxr/eye_capture.h"
#include "../../src/openxr/immediate_executor.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/d3d11/gpu_census.h"
#include <openxr/openxr.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <functional>
#include <cmath>
using Microsoft::WRL::ComPtr;

#pragma comment(linker, "/EXPORT:edvrAcquireNativeMenu")

class InlineExecutor final : public edvr::openxr::ImmediateExecutor {
 public: bool invoke(std::function<void()> callback) override { callback(); return true; }
};

namespace edvr {
void perfMonitorNoteEvent(unsigned, double) {}
// The production definition lives in flat_compute_readback.cpp, which this
// focused WARP rig does not link.
thread_local bool g_flatComputeInternal = false;
// The GPU census (issue #38) is cross-cutting; this rig is about the menu
// panel's own effect, not the census's rotation, so it is stubbed like
// perfMonitorNoteEvent above.
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
}

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
                       ComPtr<ID3D11Texture2D>& out, UINT color, UINT width = 256, UINT height = 256) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
    desc.ArraySize = desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->CreateTexture2D(&desc, nullptr, &out))) return false;
    std::vector<UINT> pixels(size_t(width) * height, color);
    c->UpdateSubresource(out.Get(), 0, nullptr, pixels.data(), width * 4, 0); return true;
}

static bool makeBackbuffer(ID3D11Device* d, ID3D11DeviceContext* c,
                           ComPtr<ID3D11Texture2D>& out, UINT color,
                           UINT width = 256, UINT height = 256) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
    desc.ArraySize = desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(d->CreateTexture2D(&desc, nullptr, &out))) return false;
    std::vector<UINT> pixels(size_t(width) * height, color);
    c->UpdateSubresource(out.Get(), 0, nullptr, pixels.data(), width * 4, 0);
    return true;
}

static ULONG refCount(ID3D11Texture2D* texture) {
    texture->AddRef();
    return texture->Release();
}

// Read the actual composite bounds, rather than asserting only the input
// geometry. A resolution-dependent draw/crop would change these fractions.
static bool changedBounds(const std::vector<UINT>& pixels, UINT width, UINT height,
                          UINT background, float bounds[4]) {
    if (pixels.size() != size_t(width) * height) return false;
    UINT x0 = width, y0 = height, x1 = 0, y1 = 0;
    bool found = false;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        if (pixels[size_t(y) * width + x] == background) continue;
        found = true;
        x0 = (std::min)(x0, x); y0 = (std::min)(y0, y);
        x1 = (std::max)(x1, x + 1); y1 = (std::max)(y1, y + 1);
    }
    if (!found || !x0 || !y0 || x1 == width || y1 == height) return false;
    bounds[0] = float(x0) / width; bounds[1] = float(y0) / height;
    bounds[2] = float(x1) / width; bounds[3] = float(y1) / height;
    return true;
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(3);
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("native_menu_test: dry-run (no WARP device)"); return 0;
    }
    if (!std::wcscmp(argv[1], L"--layout")) {
        // The panel's bitmap height is the on-screen card's shape: the
        // raster stores its aspect and halfH = halfW * aspect, so a height
        // that changes between pages changes the card's size. An entries
        // page therefore reserves its hint box (reserveHint, menu_panel.cpp)
        // and holds whatever the highlighted row's hint holds, or scrolling
        // over a row that does (and one that does not) carry a hint makes
        // the card grow and shrink. A page built without a hint line on
        // purpose -- the Monitor -- sets no reserveHint and pays nothing
        // for a box it never fills: the box is part of the bitmap's height,
        // and carrying it for no reason pushes the page nearer the
        // raster's 2048-px bound and makes it stop updating sooner. The
        // Debug tab's heading is measured against the rect layout() draws
        // it into: the single-line draw path is DT_END_ELLIPSIS, so a
        // heading wider than its rect reads cut.
        unsigned checks = 0, fails = 0;
        auto check = [&](bool ok, const char* name) {
            ++checks; if (!ok) { ++fails; std::printf("FAIL: %s\n", name); }
        };
        edvr::MenuContent page{};
        page.widthPx = page.cardPx = 256;
        page.capPx = 34;
        page.tabCount = 3;
        for (int i = 0; i < 3; ++i) std::snprintf(page.tabs[i], sizeof(page.tabs[i]), "Tab %d", i);
        page.activeTab = 1;
        page.lineCount = 5;
        for (int i = 0; i < 5; ++i) std::snprintf(page.lines[i].left, sizeof(page.lines[i].left), "Row %d", i);
        std::strcpy(page.footer, "keys");
        // The entries-page property: the hint box is row-dependent (the
        // highlighted row's hint, the rest's none), so a padded entries
        // page must reserve it -- and keep its height -- whatever the
        // highlight's hint holds, or the card changes size on a scroll.
        page.reserveHint = true;
        const int plain = edvr::menuPanelLayoutHeightForTest(page);
        check(plain > 0, "a plain page has a layout height");
        const int hintH = page.capPx * 34 / 10;
        check(hintH > 0, "the hint box has a height at this cap");
        const char* hints[] = {
            "Choose DLSS to change its model preset.",
            "No runtime width on record yet; the panel stays at the game's own 1920x1080 until a session with VR running has completed once.",
            "Turn anti-aliasing on to sharpen.",
            "Currently resolves to 3840x2160.",
        };
        for (const char* hint : hints) {
            std::strcpy(page.hint, hint);
            check(edvr::menuPanelLayoutHeightForTest(page) == plain,
                  "a hinted page that reserves its hint box is the same height as an unhinted one");
        }
        std::strcpy(page.hint, "");
        // The Monitor-page guard (dadf5304 regressed it): a page built
        // without a hint line on purpose sets no reserveHint, so it
        // reserves the box for nothing and pays hintH for an empty card
        // region it never shows -- pushing the page toward the raster's
        // 2048-px bound. Its height must be exactly what it was before the
        // box was always reserved: a plain, unhinted page is one hint box
        // shorter than the same page that reserves the box.
        page.reserveHint = false;
        const int monitorH = edvr::menuPanelLayoutHeightForTest(page);
        check(monitorH == plain - hintH,
              "a page that builds no hint line reserves none and is a hint box shorter than one that does");
        // The Debug tab's heading, against the rect layout() draws it into
        // (menuPanelLineRectForTest reads it from layout()'s own text op).
        // Flat: cardPx 860, capPx 34 -- sizeContent's flat path, fixed. The
        // heading takes the whole line: its left is pad / 2 and its right is
        // cardW - pad, so the rect is 820 px here; DT_END_ELLIPSIS cuts text
        // at its right edge. Without the full-width arm in layout() the rect
        // is the setting split's 503 px and the 805 px sentence is cut.
        edvr::MenuContent dbg{};
        dbg.widthPx = dbg.cardPx = 860;
        dbg.capPx = 34;
        dbg.tabCount = 1;
        std::snprintf(dbg.tabs[0], sizeof(dbg.tabs[0]), "Debug");
        dbg.lineCount = 1;
        std::snprintf(dbg.lines[0].left, sizeof(dbg.lines[0].left), "For bug reports. Both are off unless you turn them on.");
        dbg.lines[0].style = edvr::kMenuHeading;
        std::strcpy(dbg.footer, "keys");
        int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
        edvr::menuPanelLineRectForTest(dbg, 0, &hx0, &hy0, &hx1, &hy1);
        const int headingBoxW = hx1 - hx0;
        const int headingW = edvr::menuPanelMeasureLine(dbg.lines[0].left, dbg.capPx);
        check(headingBoxW == 860 - dbg.capPx * 8 / 10 - dbg.capPx * 8 / 10 / 2,
              "the heading's rect spans the line from its half-pad left to cardW - pad");
        check(headingW > 0 && headingW <= headingBoxW,
              "the Debug heading fits the rect it is drawn into (no ellipsis)");
        // The wiring that makes a padded page reserve its hint box is in
        // menu.cpp's build, which this rig does not link -- so the height
        // checks above set reserveHint themselves and cannot see it. Read
        // the source and pin that menu.cpp sets c.reserveHint = true inside
        // the pad block, before its pad loop: that block is the only place
        // that runs, so pinning it there (and nowhere else) means a page
        // that pads its page reserves the box and a page that does not
        // (the Monitor, the prompt page) does not. Deleting the line, or
        // moving it outside the pad block, fails here.
        {
            std::ifstream in("src/d3d11/menu.cpp");
            std::string menu((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::string guard = "if (!p.prompt && c.lineCount > 0) {";
            const std::string padLoop = "for (int i = c.lineCount; i < visibleRows && i < kMenuMaxLines; ++i)";
            const size_t g = menu.find(guard);
            const size_t set = menu.find("c.reserveHint = true;");
            const size_t loop = menu.find(padLoop);
            check(!menu.empty() && g != std::string::npos && loop != std::string::npos && g < loop,
                  "menu.cpp's pad block and pad loop can be found in the source");
            check(set != std::string::npos && g < set && set < loop,
                  "menu.cpp sets reserveHint inside the pad block, ahead of its pad loop");
            check(menu.find("c.reserveHint = true;", set + 1) == std::string::npos,
                  "menu.cpp sets reserveHint in exactly one place (the pad block; the Monitor and prompt pages reserve none)");
        }
        std::printf("native_menu_test: layout %u checks, %u failures (heading %dpx in a %dpx rect)\n",
                    checks, fails, headingW, headingBoxW);
        return fails ? 1 : 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    unsigned checks = 0, fails = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks; if (!ok) { ++fails; std::printf("FAIL: %s\n", name); }
    };
    // System32's d3d11 through common/system_d3d11.h, never an import: EDVR's proxy sits beside this exe.
    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "system d3d11.dll");
    if (!createDevice) return 1;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL feature{};
    check(SUCCEEDED(createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context)), "WARP device");
    if (!device) return 1;

    EdvrNativeMenuRequest request{sizeof(request), EDVR_NATIVE_MENU_VERSION_1, device.Get(), 17};
    EdvrNativeMenuTable table{sizeof(table), EDVR_NATIVE_MENU_VERSION_1};
    check(edvrAcquireNativeMenu(&request, &table) == S_OK && table.context && nativeMenuActive() && !nativeMenuAvailable(), "provider acquire; session active before pose");
    // The eye poses the Status-page notice for parallel projection reads (nativeMenuEyePoses): none before a pose, and the caller's array is left alone.
    float heldEyes[2][12]; std::memset(heldEyes, 0x7f, sizeof(heldEyes)); float untouched[2][12]; std::memcpy(untouched, heldEyes, sizeof(untouched));
    check(!nativeMenuEyePoses(heldEyes) && std::memcmp(heldEyes, untouched, sizeof(heldEyes)) == 0 && !nativeMenuEyePoses(nullptr), "eye poses: none before the first publish, the array untouched");
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
    check(table.publishPose(table.context, head, eyes, frusta, 17, 3) == S_OK && nativeMenuAvailable() && nativeMenuActive(), "publish valid pose; session active");
    check(nativeMenuEyePoses(heldEyes) && std::memcmp(heldEyes, eyes, sizeof(heldEyes)) == 0, "eye poses: the published pair, both 3x4 rows intact");

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

    // Head-locked monitor: change the actual source texture dimensions and
    // the published sizing channel together, then compare what was rendered.
    // Both asymmetric eyes must retain the same angular rectangle, including
    // a flipped input and an aspect-ratio change (not just uniform scaling).
    {
        const UINT dimensions[][2] = {{256,256}, {512,512}, {1024,512}, {512,1024}};
        constexpr UINT background = 0xff202020u;
        const char* readout = "90 fps   gpu 13.3 ms   cpu 1.4 ms";
        edvr::MenuContent reference{};
        float referenceWidth = 0.f;
        check(edvr::menuPanelBuildOverlayContent(reference, readout, 1.1f, &referenceWidth),
              "overlay production content builder");
        edvr::setMenuHeadLock(true, 9.f, -8.f);
        edvr::MenuGeometry pendingGeometry{};
        pendingGeometry.alpha = 1.f;
        pendingGeometry.overlayRaster = true;
        edvr::menuPanelSetGeometry(pendingGeometry);
        ComPtr<ID3D11Texture2D> pendingOutput;
        float pendingBounds[4]{};
        check(table.treatEye(table.context, 0, source.Get(), normal,
              pendingOutput.GetAddressOf(), pendingBounds) == S_FALSE && !pendingOutput,
              "overlay handover suppresses the previous settings raster");
        float expected[2][4]{};
        bool haveExpected[2]{};
        for (const auto& dim : dimensions) {
            edvr::announceEyeTextureSize(dim[0], dim[1]);
            edvr::announceEyeTangents(1.1f, .9f);
            edvr::MenuContent overlay{};
            float width = 0.f;
            check(edvr::menuPanelBuildOverlayContent(overlay, readout, 1.1f, &width) &&
                  width == referenceWidth && overlay.widthPx == reference.widthPx &&
                  overlay.capPx == reference.capPx && overlay.lineCount == reference.lineCount,
                  "overlay raster and angular width ignore render resolution");
            edvr::menuPanelSubmit(overlay);
            bool uploaded = false;
            for (unsigned ms = 0; ms < 3000 && !uploaded; ms += 5) {
                uploaded = edvr::menuPanelWorkerReadyForTest();
                if (!uploaded) Sleep(5);
            }
            check(uploaded, "overlay worker completes");
            edvr::menuPanelTick(device.Get());
            edvr::MenuGeometry overlayGeometry{};
            overlayGeometry.alpha = 1.f;
            overlayGeometry.overlayRaster = true;
            // Deliberately stale model width: an upload can land after the
            // menu tick has set its geometry. The compositor must use the
            // angular width associated with the newly uploaded bitmap.
            overlayGeometry.halfW = 100.f;
            edvr::menuPanelSetGeometry(overlayGeometry);
            ComPtr<ID3D11Texture2D> resized;
            check(makeSource(device.Get(), context.Get(), resized, background, dim[0], dim[1]),
                  "overlay resized source");
            if (!resized) continue;
            for (unsigned eye = 0; eye < 2; ++eye) {
                ComPtr<ID3D11Texture2D> output;
                float bounds[4]{};
                check(table.treatEye(table.context, eye, resized.Get(), eye ? flipped : normal,
                      output.GetAddressOf(), bounds) == S_OK && output,
                      "overlay composite in each asymmetric eye");
                std::vector<UINT> pixels;
                float actual[4]{};
                const bool haveBounds = output && readPixels(device.Get(), context.Get(), output.Get(), pixels) &&
                    changedBounds(pixels, dim[0], dim[1], background, actual);
                check(haveBounds, "overlay bounds are visible and do not touch eye edges");
                if (!haveBounds) continue;
                if (!haveExpected[eye]) {
                    std::memcpy(expected[eye], actual, sizeof(actual));
                    haveExpected[eye] = true;
                } else {
                    bool same = true;
                    for (unsigned axis = 0; axis < 4; ++axis)
                        same = same && std::fabs(actual[axis] - expected[eye][axis]) <= 2.f / 256.f;
                    check(same, "overlay rendered bounds remain stable within pixel rounding");
                }
            }
        }
        edvr::setMenuHeadLock(false, 0.f, 0.f);
        edvr::menuPanelSetGeometry(geometry);
    }

    std::atomic<long> wrongThread{0}; std::thread worker([&] { ID3D11Texture2D* o = nullptr; float b[4]{}; wrongThread = table.treatEye(table.context, 0, source.Get(), normal, &o, b); if (o) o->Release(); }); worker.join();
    check(wrongThread == E_INVALIDARG, "wrong producer thread rejected");
    ComPtr<ID3D11Device> foreign; ComPtr<ID3D11DeviceContext> foreignContext; D3D_FEATURE_LEVEL foreignFeature{};
    check(SUCCEEDED(createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &foreign, &foreignFeature, &foreignContext)), "foreign WARP device");
    ComPtr<ID3D11Texture2D> foreignSource; check(foreign && makeSource(foreign.Get(), foreignContext.Get(), foreignSource, 0xff303030u), "foreign source");
    ID3D11Texture2D* rejected = nullptr; float rejectBounds[4]{};
    check(table.treatEye(table.context, 0, foreignSource.Get(), normal, &rejected, rejectBounds) == E_INVALIDARG && !rejected, "foreign device rejected");
    float badBounds[4] = {-.1f,0,1,1}; check(table.treatEye(table.context, 0, source.Get(), badBounds, &rejected, rejectBounds) == E_INVALIDARG && !rejected, "bad bounds rejected");
    check(table.publishPose(table.context, nullptr, nullptr, nullptr, 17, 0) == S_OK && !nativeMenuAvailable() && nativeMenuActive(), "CPU invalidation keeps session active");
    std::memcpy(heldEyes, untouched, sizeof(heldEyes));
    check(!nativeMenuEyePoses(heldEyes) && std::memcmp(heldEyes, untouched, sizeof(heldEyes)) == 0, "eye poses: none after a CPU invalidation");
    check(table.close(table.context) == S_OK && !nativeMenuAvailable() && !nativeMenuActive(), "CPU close retires session");
    check(!nativeMenuEyePoses(heldEyes), "eye poses: none after the close");
    EdvrNativeMenuTable second{sizeof(second), EDVR_NATIVE_MENU_VERSION_1}; EdvrNativeMenuRequest secondRequest{sizeof(secondRequest), EDVR_NATIVE_MENU_VERSION_1, device.Get(), 29};
    check(edvrAcquireNativeMenu(&secondRequest, &second) == S_OK && second.context && nativeMenuActive(), "reacquire after close; session active");
    check(second.publishPose(second.context, head, eyes, frusta, 29, 4) == S_OK && nativeMenuAvailable(), "second publish");
    check(nativeMenuEyePoses(heldEyes) && std::memcmp(heldEyes, eyes, sizeof(heldEyes)) == 0, "eye poses: the second session's pair");
    check(table.publishPose(table.context, nullptr, nullptr, nullptr, 17, 0) == E_INVALIDARG && nativeMenuAvailable() && nativeMenuActive(), "stale context cannot invalidate active generation");
    check(table.close(table.context) == S_FALSE && nativeMenuActive(), "stale context cannot retire active generation");
    check(second.close(second.context) == S_OK && !nativeMenuAvailable() && !nativeMenuActive(), "second close");
    check(!nativeMenuEyePoses(heldEyes), "eye poses: none once the last session closed");
    edvr::openxr::NativeMenuClient client;
    check(client.acquire(GetModuleHandleW(nullptr), device.Get(), 31) == S_OK && nativeMenuActive(), "client acquire; session active");
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

    // Parallel-projection latch across menu sessions (parallel_projection_notice.h, SessionLatch), driven by the same session identity and
    // held poses the graphics tick uses. Four cycles, each an acquire, its poses and a close. A checked or suppressed state must not reach
    // the next session, a notice must not outlive its session, and a close followed by the same verdict says it again.
    {
        namespace ppn = edvr::ppn;
        const char* pimax8kx = "Pimax Vision 8K X";
        const char* pimaxXr = "PimaxXR (Unofficial)";
        const char* pimaxPlay = "Pimax OpenXR";
        const float c10 = std::cos(10.f * 3.14159265f / 180.f), s10 = std::sin(10.f * 3.14159265f / 180.f);
        const float parallelPair[2][12] = {{1,0,0,-.03f, 0,1,0,0, 0,0,1,0}, {1,0,0,.03f, 0,1,0,0, 0,0,1,0}};
        const float cantedPair[2][12] = {{1,0,0,-.03f, 0,1,0,0, 0,0,1,0}, {c10,0,s10,.03f, 0,1,0,0, -s10,0,c10,0}};
        float lcHead[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
        float lcFrusta[2][4] = {{-.9f,1.1f,-1.2f,.8f}, {-1.1f,.9f,-.8f,1.2f}};
        ppn::SessionLatch latch;   // the graphics tick's latch, one for the rig
        // One graphics tick as menu.cpp runs it: the identity first, then the held poses.
        auto lcTick = [&](const char* system, const char* runtime) {
            const uint64_t id = nativeMenuSessionId();
            float held[2][12]{};
            const bool poseHeld = nativeMenuEyePoses(held);
            return ppn::latchTick(latch, id, poseHeld ? held : nullptr, system, runtime);
        };
        EdvrNativeMenuTable lcTable{};
        auto lcAcquire = [&](uint64_t gen) {
            EdvrNativeMenuRequest r{sizeof(r), EDVR_NATIVE_MENU_VERSION_1, device.Get(), gen};
            lcTable = EdvrNativeMenuTable{sizeof(EdvrNativeMenuTable), EDVR_NATIVE_MENU_VERSION_1};
            return edvrAcquireNativeMenu(&r, &lcTable) == S_OK && lcTable.context && nativeMenuActive() && !nativeMenuAvailable();
        };
        auto lcPublish = [&](const float (*eyes)[12], uint64_t gen) {
            return lcTable.publishPose(lcTable.context, lcHead, eyes, lcFrusta, gen, 5) == S_OK && nativeMenuAvailable();
        };
        auto lcClose = [&]() { return lcTable.close(lcTable.context) == S_OK && !nativeMenuActive() && !nativeMenuAvailable(); };

        // Cycle 1: canted views on a canted Pimax, ProjectionOff. Decided, nothing raised: the suppressed state cycle 2 must not inherit.
        const uint64_t id0 = nativeMenuSessionId();
        check(lcAcquire(201) && nativeMenuSessionId() != id0, "latch 1: acquire changes the session identity");
        check(lcPublish(cantedPair, 201), "latch 1: publish canted views");
        check(!lcTick(pimax8kx, pimaxXr) && latch.checked && !latch.notice, "latch 1: canted views decided, no notice");
        check(!lcTick(pimax8kx, pimaxXr) && latch.checked && !latch.notice, "latch 1: the same session does not decide again");
        const uint64_t id1 = nativeMenuSessionId();
        check(lcClose() && nativeMenuSessionId() != id1, "latch 1: close; the close changes the session identity");
        check(!lcTick(pimax8kx, pimaxXr) && !latch.checked && !latch.notice, "latch 1: the close clears the verdict");

        // Cycle 2: parallel views, PimaxXR. The notice must show here, not be suppressed by cycle 1's checked state.
        check(lcAcquire(202) && lcPublish(parallelPair, 202), "latch 2: acquire and publish parallel views");
        check(lcTick(pimax8kx, pimaxXr) && latch.checked && latch.notice, "latch 2: its own notice is raised after the canted cycle");
        check(!lcTick(pimax8kx, pimaxXr) && latch.notice, "latch 2: a second tick of the session does not say it again");
        // A CPU invalidation and a re-publish inside the session are not a new session: the one notice stays, no second toast.
        const uint64_t id2 = nativeMenuSessionId();
        check(lcTable.publishPose(lcTable.context, nullptr, nullptr, nullptr, 202, 0) == S_OK && !nativeMenuAvailable() &&
              nativeMenuSessionId() == id2, "latch 2: CPU invalidation keeps the session identity");
        check(!lcTick(pimax8kx, pimaxXr) && latch.notice, "latch 2: invalidated, the notice stays for the session");
        check(lcPublish(parallelPair, 202) && !lcTick(pimax8kx, pimaxXr) && latch.notice && nativeMenuSessionId() == id2,
              "latch 2: re-publish inside the session says nothing new");
        check(lcClose() && nativeMenuSessionId() != id2, "latch 2: close");
        check(!lcTick(pimax8kx, pimaxXr) && !latch.checked && !latch.notice, "latch 2: the close clears the notice");

        // Cycle 3: a close followed by a publish of the same verdict. A new session, so the notice is said again.
        check(lcAcquire(203) && lcPublish(parallelPair, 203), "latch 3: acquire and publish the same parallel views");
        check(lcTick(pimax8kx, pimaxXr) && latch.notice, "latch 3: the same verdict after a close is said again, as a new session");
        check(lcClose(), "latch 3: close");

        // Cycle 4: parallel views on a runtime not known to apply the toggle: LogOnly. Decided, no notice, and nothing carried from cycle 3.
        check(lcAcquire(204) && lcPublish(parallelPair, 204), "latch 4: acquire and publish parallel views on Pimax OpenXR");
        check(!lcTick(pimax8kx, pimaxPlay) && latch.checked && !latch.notice, "latch 4: log-only runtime decided, no notice");
        float lcNone[2][12]{};
        check(lcClose() && !nativeMenuActive() && !nativeMenuEyePoses(lcNone), "latch 4: close leaves no session and no poses");
    }

    // Exercise the desktop Present compositor against a backbuffer-shaped
    // render target. With no SRV bind it must take the source-copy path.
    {
        edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
        constexpr UINT background = 0xff203040u;
        ComPtr<ID3D11Texture2D> back;
        check(makeBackbuffer(device.Get(), context.Get(), back, background),
              "flat backbuffer render target");
        if (back) {
            std::vector<UINT> before, after;
            check(readPixels(device.Get(), context.Get(), back.Get(), before),
                  "flat baseline readback");
            edvr::MenuGeometry hidden = geometry;
            hidden.alpha = 0.f;
            edvr::menuPanelSetGeometry(hidden);
            const uint32_t hiddenHeartbeat = edvr::menuDrawnValue();
            check(!edvr::menuPanelCompositeFlatTexture(back.Get()) &&
                  edvr::menuDrawnValue() == hiddenHeartbeat,
                  "flat hidden menu skips draw and heartbeat");
            check(readPixels(device.Get(), context.Get(), back.Get(), after) &&
                  after == before, "flat hidden menu preserves pixels");

            edvr::menuPanelSetGeometry(geometry);
            ComPtr<ID3D11RenderTargetView> sentinelRtv;
            check(SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &sentinelRtv)),
                  "flat sentinel render target view");
            ID3D11RenderTargetView* boundRtv = sentinelRtv.Get();
            context->OMSetRenderTargets(1, &boundRtv, nullptr);
            D3D11_VIEWPORT viewport{13.f, 17.f, 111.f, 93.f, 0.25f, 0.75f};
            context->RSSetViewports(1, &viewport);
            context->CSSetShaderResources(0, 1, &boundSrv);
            context->CSSetUnorderedAccessViews(0, 1, &boundUav, nullptr);
            const ULONG refsBefore = refCount(back.Get());
            const uint32_t drawnBefore = edvr::menuDrawnValue();
            check(edvr::menuPanelCompositeFlatTexture(back.Get()), "flat visible composite");
            check(refCount(back.Get()) == refsBefore,
                  "flat composite retains no backbuffer reference");
            check(edvr::menuDrawnValue() == drawnBefore + 1,
                  "flat visible composite advances heartbeat once");
            ID3D11ShaderResourceView* flatSrv = nullptr;
            ID3D11UnorderedAccessView* flatUav = nullptr;
            ID3D11RenderTargetView* flatRtv = nullptr;
            UINT viewportCount = 1;
            D3D11_VIEWPORT flatViewport{};
            context->CSGetShaderResources(0, 1, &flatSrv);
            context->CSGetUnorderedAccessViews(0, 1, &flatUav);
            context->OMGetRenderTargets(1, &flatRtv, nullptr);
            context->RSGetViewports(&viewportCount, &flatViewport);
            check(flatSrv == boundSrv && flatUav == boundUav &&
                  flatRtv == boundRtv && viewportCount == 1 &&
                  std::memcmp(&flatViewport, &viewport, sizeof(viewport)) == 0,
                  "flat composite restores compute, OM and viewport state");
            if (flatSrv) flatSrv->Release();
            if (flatUav) flatUav->Release();
            if (flatRtv) flatRtv->Release();
            ID3D11RenderTargetView* noRtv = nullptr;
            ID3D11ShaderResourceView* noSrv = nullptr;
            ID3D11UnorderedAccessView* noUav = nullptr;
            context->OMSetRenderTargets(0, &noRtv, nullptr);
            context->CSSetShaderResources(0, 1, &noSrv);
            context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
            sentinelRtv.Reset();
            check(readPixels(device.Get(), context.Get(), back.Get(), after),
                  "flat composite readback");
            if (after.size() == before.size()) {
                size_t changed = 0;
                for (size_t i = 0; i < after.size(); ++i) changed += after[i] != before[i];
                check(changed > 0 && changed < after.size(),
                      "flat panel visible with outside pixels preserved");
            } else check(false, "flat readback dimensions");

            edvr::menuPanelFlatResize();
            check(!edvr::menuPanelFlatRasterReady() &&
                  !edvr::menuPanelCompositeFlatTexture(back.Get()),
                  "flat resize retires old raster and composite");
            edvr::menuPanelSubmit(content);
            bool fresh = false;
            for (unsigned ms = 0; ms < 3000 && !fresh; ms += 5) {
                fresh = edvr::menuPanelWorkerReadyForTest();
                if (!fresh) Sleep(5);
            }
            check(fresh, "flat resize fresh raster worker ready");
            edvr::menuPanelTick(device.Get());
            check(edvr::menuPanelFlatRasterReady(), "flat resize fresh raster uploaded");
            ComPtr<ID3D11Texture2D> resizedBack;
            check(makeBackbuffer(device.Get(), context.Get(), resizedBack,
                                 background, 320, 240), "flat resized backbuffer");
            if (resizedBack) {
                std::vector<UINT> resizedPixels;
                check(edvr::menuPanelCompositeFlatTexture(resizedBack.Get()) &&
                      readPixels(device.Get(), context.Get(), resizedBack.Get(), resizedPixels),
                      "flat resized backbuffer composes");
                float visibleBounds[4]{};
                check(changedBounds(resizedPixels, 320, 240, background, visibleBounds),
                      "flat resize draws bounded panel pixels");
                check(visibleBounds[0] >= 0.01f && visibleBounds[0] < 0.05f &&
                      visibleBounds[1] >= 0.01f && visibleBounds[1] < 0.05f &&
                      visibleBounds[2] < 0.55f && visibleBounds[3] < 0.55f,
                      "flat panel stays small and anchored near the upper-left corner");
            }
            // A replacement graphics device must build its own raster and
            // context state; no resource from the first device may be reused.
            edvr::menuPanelFlatResize();
            edvr::menuPanelSubmit(content);
            bool foreignReady = false;
            for (unsigned ms = 0; ms < 3000 && !foreignReady; ms += 5) {
                foreignReady = edvr::menuPanelWorkerReadyForTest();
                if (!foreignReady) Sleep(5);
            }
            check(foreignReady, "flat replacement device raster worker ready");
            if (foreignReady && foreign) {
                edvr::menuPanelTick(foreign.Get());
                ComPtr<ID3D11Texture2D> foreignBack;
                check(makeBackbuffer(foreign.Get(), foreignContext.Get(),
                                     foreignBack, background),
                      "flat replacement device backbuffer");
                std::vector<UINT> foreignPixels;
                float foreignBounds[4]{};
                check(foreignBack &&
                      edvr::menuPanelCompositeFlatTexture(foreignBack.Get()) &&
                      readPixels(foreign.Get(), foreignContext.Get(),
                                 foreignBack.Get(), foreignPixels) &&
                      changedBounds(foreignPixels, 256, 256, background,
                                    foreignBounds),
                      "flat replacement device composites fresh raster");
            }
        }
        edvr::menuPanelFlatResize();
        edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    }
    edvr::menuPanelShutdown(); std::printf("native_menu_test: %u checks, %u failures\n", checks, fails); return fails ? 1 : 0;
}
