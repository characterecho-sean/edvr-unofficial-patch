#define EDVR_DEPTH_SCENE_PICK_TEST 1
#include "../../src/d3d11/depth_probe.cpp"
#include "../../src/common/system_d3d11.h"

#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
unsigned checks = 0;
std::string temporalMode = "dlss";

void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

struct DepthTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> view;
};

DepthTarget makeTarget(ID3D11Device* device, UINT width, UINT height) {
    DepthTarget target;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    check(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target.texture)),
          "create depth texture");
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    check(SUCCEEDED(device->CreateDepthStencilView(target.texture.Get(), &dd, &target.view)),
          "create depth view");
    return target;
}

void draws(const DepthTarget& target, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        edvr::depthProbeNoteDraw(nullptr, target.view.Get(), true, false);
    }
}

ID3D11Texture2D* scene(uint32_t width, uint32_t height, int eye, bool expected) {
    ID3D11Texture2D* texture = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{1});
    const bool found = edvr::depthProbeSceneDepth(width, height, eye, &texture);
    check(found == expected, expected ? "scene query succeeds" : "scene query fails");
    if (!expected) check(texture == nullptr, "failed scene query clears output");
    return texture;
}
}  // namespace

namespace edvr {
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) {}

Config& Config::get() { static Config config; return config; }
std::string Config::getString(const char* key, const char* def) const {
    if (!std::strcmp(key, "fix.temporal_aa")) return temporalMode;
    if (!std::strcmp(key, "fix.eye_mask")) return "off";
    return def;
}

int guardFilter(unsigned long, const char*) { return EXCEPTION_EXECUTE_HANDLER; }
void FaultBudget::charge() { m_remaining.fetch_sub(1, std::memory_order_relaxed); }
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext*, const char*, size_t,
                                         const char*, const char*, const SwapMacro*,
                                         const char*) { return nullptr; }
void temporalPassNoteFirstEyeDraw(ID3D11DeviceContext*) {}
}  // namespace edvr

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    try {
        if (argc != 2) return 2;
        if (!std::strcmp(argv[1], "--dry-run")) {
            std::puts("depth_scene_pick_test: dry-run");
            return 0;
        }
        if (std::strcmp(argv[1], "--self-test")) return 2;

        const auto createDevice = edvr::systemD3D11CreateDevice();
        check(createDevice != nullptr, "load system D3D11");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        check(createDevice && SUCCEEDED(createDevice(
                  nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                  D3D11_SDK_VERSION, &device, &level, &context)),
              "create WARP device");

        edvr::depthProbeConfigure(edvr::Config::get());
        check(edvr::g_wanted, "DLSS enables the depth probe");

        const uint32_t negativeScan = edvr::g_scenePickScans;
        scene(100, 100, 0, false);
        check(edvr::g_scenePickScans == negativeScan + 1,
              "first negative query scans the target table");
        scene(100, 100, 1, false);
        check(edvr::g_scenePickScans == negativeScan + 1,
              "repeated negative query uses the cache");

        const DepthTarget a = makeTarget(device.Get(), 100, 100);
        const DepthTarget b = makeTarget(device.Get(), 100, 100);
        draws(b, 8);  // First bound is the left eye even though A is busier.
        draws(a, 10);
        const uint32_t creationScan = edvr::g_scenePickScans;
        scene(100, 100, 0, false);
        check(edvr::g_scenePickScans == creationScan + 1,
              "target creation invalidates a cached negative result");
        scene(100, 100, 1, false);
        check(edvr::g_scenePickScans == creationScan + 1,
              "unchanged pre-roll negative remains cached");

        int diagnosticEye = 99, diagnosticTarget = 99;
        const uint32_t readOnlyPrePickScan = edvr::g_scenePickScans;
        check(!edvr::depthProbeCurrentSceneEyeOf(b.view.Get(), &diagnosticEye,
                                                 &diagnosticTarget) &&
                  diagnosticEye == -1 && diagnosticTarget == -1,
              "read-only eye lookup does not form an unsettled scene pair");
        check(edvr::g_scenePickScans == readOnlyPrePickScan &&
                  edvr::g_scenePick[0] < 0 && edvr::g_scenePick[1] < 0,
              "read-only eye lookup never scans or changes the pair");

        edvr::depthProbeFrameBoundary(nullptr);
        const uint32_t firstPairScan = edvr::g_scenePickScans;
        check(scene(100, 100, 0, true) == b.texture.Get(),
              "first-bind order chooses the first eye");
        check(scene(100, 100, 1, true) == a.texture.Get(),
              "first-bind order chooses the second eye");
        check(edvr::g_scenePickScans == firstPairScan + 1,
              "both eyes share one scene-pick scan");
        const uint32_t readOnlySettledScan = edvr::g_scenePickScans;
        check(edvr::depthProbeCurrentSceneEyeOf(b.view.Get(), &diagnosticEye,
                                                &diagnosticTarget) &&
                  diagnosticEye == 0 && diagnosticTarget >= 0,
              "read-only eye lookup identifies the settled first eye");
        check(edvr::depthProbeCurrentSceneEyeOf(a.view.Get(), &diagnosticEye,
                                                &diagnosticTarget) &&
                  diagnosticEye == 1 && diagnosticTarget >= 0 &&
                  edvr::g_scenePickScans == readOnlySettledScan,
              "read-only eye lookup identifies the second eye without scanning");
        scene(100, 100, 0, true);
        check(edvr::g_scenePickScans == firstPairScan + 1,
              "repeated positive query uses the cache");

        const DepthTarget c = makeTarget(device.Get(), 100, 100);
        draws(a, 7);
        draws(b, 6);
        draws(c, 12);
        const uint32_t readOnlyNonPairScan = edvr::g_scenePickScans;
        const int readOnlyPair0 = edvr::g_scenePick[0], readOnlyPair1 = edvr::g_scenePick[1];
        const edvr::ScenePickCache readOnlyCache = edvr::g_scenePickCache;
        check(!edvr::depthProbeCurrentSceneEyeOf(c.view.Get(), &diagnosticEye,
                                                 &diagnosticTarget) &&
                  diagnosticEye == -1 && diagnosticTarget == -1,
              "read-only eye lookup rejects a known target outside the settled pair");
        check(edvr::g_scenePickScans == readOnlyNonPairScan &&
                  edvr::g_scenePick[0] == readOnlyPair0 && edvr::g_scenePick[1] == readOnlyPair1 &&
                  edvr::g_scenePickCache.w == readOnlyCache.w &&
                  edvr::g_scenePickCache.h == readOnlyCache.h &&
                  edvr::g_scenePickCache.result == readOnlyCache.result &&
                  edvr::g_scenePickCache.valid == readOnlyCache.valid,
              "read-only non-pair lookup preserves pair and cache state");
        const uint32_t addedScan = edvr::g_scenePickScans;
        check(scene(100, 100, 0, true) == b.texture.Get(),
              "new target revalidation keeps the prior-frame ordering");
        check(edvr::g_scenePickScans == addedScan + 1,
              "new live target invalidates the positive cache");
        edvr::depthProbeFrameBoundary(nullptr);
        const uint32_t hysteresisScan = edvr::g_scenePickScans;
        check(scene(100, 100, 0, true) == a.texture.Get(),
              "eye order is recomputed after a frame roll");
        check(scene(100, 100, 1, true) == b.texture.Get(),
              "half-busiest hysteresis keeps the prior pair");
        check(edvr::g_scenePickScans == hysteresisScan + 1,
              "new frame counts force one new scan");

        draws(c, 20);
        draws(a, 5);
        draws(b, 4);
        edvr::depthProbeFrameBoundary(nullptr);
        check(scene(100, 100, 0, true) == c.texture.Get(),
              "a pair below the half-busiest threshold is replaced");
        check(scene(100, 100, 1, true) == a.texture.Get(),
              "replacement retains the second-busiest target");

        const DepthTarget d = makeTarget(device.Get(), 200, 100);
        const DepthTarget e = makeTarget(device.Get(), 200, 100);
        draws(c, 20);
        draws(a, 10);
        draws(e, 8);
        draws(d, 10);
        edvr::depthProbeFrameBoundary(nullptr);
        const uint32_t sizeScan = edvr::g_scenePickScans;
        check(scene(200, 100, 0, true) == e.texture.Get(),
              "alternate size forms its own ordered pair");
        check(scene(100, 100, 0, true) == c.texture.Get(),
              "switching back revalidates the global pick");
        check(edvr::g_scenePickScans == sizeScan + 2,
              "each size switch performs the normal scan");
        scene(100, 100, 1, true);
        check(edvr::g_scenePickScans == sizeScan + 2,
              "unchanged switched-back size is cached");

        const int stale0 = edvr::g_scenePick[0], stale1 = edvr::g_scenePick[1];
        const DepthTarget lone = makeTarget(device.Get(), 300, 100);
        draws(c, 4);
        draws(a, 3);
        draws(lone, 9);
        edvr::depthProbeFrameBoundary(nullptr);
        const uint32_t loneScan = edvr::g_scenePickScans;
        scene(300, 100, 0, false);
        check(edvr::g_scenePick[0] == stale0 && edvr::g_scenePick[1] == stale1,
              "failed one-candidate query leaves the stale pick untouched");
        scene(300, 100, 1, false);
        check(edvr::g_scenePickScans == loneScan + 1,
              "failed one-candidate result is cached without becoming success");

        for (unsigned i = 0; i < 122; ++i) edvr::depthProbeFrameBoundary(nullptr);
        bool anyLive = false;
        for (int i = 0; i < edvr::g_targetCount; ++i) anyLive |= edvr::g_targets[i].dsv != nullptr;
        check(!anyLive, "inactive target slots are evicted through frame rollover");
        const uint32_t evictionScan = edvr::g_scenePickScans;
        scene(100, 100, 0, false);
        check(edvr::g_scenePickScans == evictionScan + 1,
              "eviction cannot reuse a cached successful pair");

        const int slotsBeforeReuse = edvr::g_targetCount;
        const DepthTarget reusedLeft = makeTarget(device.Get(), 100, 100);
        const DepthTarget reusedRight = makeTarget(device.Get(), 100, 100);
        draws(reusedRight, 7);
        scene(100, 100, 0, false);
        draws(reusedLeft, 9);
        scene(100, 100, 0, false);
        edvr::depthProbeFrameBoundary(nullptr);
        check(edvr::g_targetCount == slotsBeforeReuse,
              "new targets reuse evicted slots instead of growing the table");
        check(scene(100, 100, 0, true) == reusedRight.texture.Get() &&
              scene(100, 100, 1, true) == reusedLeft.texture.Get(),
              "reused slots form a fresh ordered pair after rollover");

        const uint32_t configScan = edvr::g_scenePickScans;
        temporalMode = "off";
        edvr::depthProbeConfigure(edvr::Config::get());
        scene(100, 100, 0, false);
        check(edvr::g_scenePickScans == configScan,
              "disabled config returns before scanning");
        temporalMode = "dlss";
        edvr::depthProbeConfigure(edvr::Config::get());
        check(scene(100, 100, 0, true) == reusedRight.texture.Get(),
              "re-enabled config revalidates the pair");
        check(edvr::g_scenePickScans == configScan + 1,
              "reconfigure invalidates the last query");

        const uint32_t shutdownScan = edvr::g_scenePickScans;
        edvr::depthProbeShutdown();
        scene(100, 100, 0, false);
        check(edvr::g_scenePickScans == shutdownScan + 1 && edvr::g_targetCount == 0,
              "shutdown invalidates cached identities and empties the table");

        std::printf("depth_scene_pick_test: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
