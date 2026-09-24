#define EDVR_DEPTH_SCENE_PICK_TEST 1
#include "../../src/d3d11/depth_probe.cpp"
#include "../../src/common/system_d3d11.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
unsigned checks = 0;
std::string temporalMode = "dlss";
bool eyeDepthCaptureOn = false;

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
bool Config::getBool(const char* key, bool def) const {
    if (!std::strcmp(key, "advanced.eye_depth_capture")) return eyeDepthCaptureOn;
    return def;
}

int guardFilter(unsigned long, const char*) { return EXCEPTION_EXECUTE_HANDLER; }
void FaultBudget::charge() { m_remaining.fetch_sub(1, std::memory_order_relaxed); }
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext*, const char*, size_t,
                                         const char*, const char*, const SwapMacro*,
                                         const char*) { return nullptr; }
void temporalPassNoteFirstEyeDraw(ID3D11DeviceContext*) {}
// The layout census reads the bound shaders' hashes (binding_shadow.h's
// inline reader); this rig binds none, so every slot reads zero.
namespace detail { BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)]; }
}  // namespace edvr

namespace {
struct PickState {
    int pair[2];
    edvr::ScenePickCache cache;
    uint32_t scans;
};

struct WorkCounts {
    uint32_t refresh;
    uint32_t order;
    uint32_t formatVisits;
};

struct Selection {
    bool found;
    int eye;
    PickState state;
    WorkCounts work;
};

PickState capturePickState() {
    return {{edvr::g_scenePick[0], edvr::g_scenePick[1]},
            edvr::g_scenePickCache, edvr::g_scenePickScans};
}

void restorePickState(const PickState& state) {
    edvr::g_scenePick[0] = state.pair[0];
    edvr::g_scenePick[1] = state.pair[1];
    edvr::g_scenePickCache = state.cache;
    edvr::g_scenePickScans = state.scans;
}

void clearWorkCounts() {
    edvr::g_scenePickRefreshCalls = 0;
    edvr::g_scenePickOrderCalls = 0;
    edvr::g_scenePickFormatVisits = 0;
}

WorkCounts captureWorkCounts() {
    return {edvr::g_scenePickRefreshCalls, edvr::g_scenePickOrderCalls,
            edvr::g_scenePickFormatVisits};
}

bool samePickState(const PickState& a, const PickState& b) {
    return a.pair[0] == b.pair[0] && a.pair[1] == b.pair[1] &&
           a.cache.w == b.cache.w && a.cache.h == b.cache.h &&
           a.cache.result == b.cache.result && a.cache.valid == b.cache.valid &&
           a.scans == b.scans;
}

// Literal copy of the retired mesh-record capture's former membership +
// two-eye format loop.
bool oldSceneTextureEye(uint32_t w, uint32_t h, const void* resource, int* outEye) {
    *outEye = -1;
    if (!edvr::depthProbeIsSceneDepth(resource)) return false;
    for (int i = 0; i < 2; ++i) {
        ID3D11Texture2D* sceneTexture = nullptr;
        uint32_t format = 0;
        if (edvr::depthProbeSceneDepthFormat(w, h, i, &sceneTexture, &format) &&
            sceneTexture == resource) {
            *outEye = i;
            return true;
        }
    }
    return false;
}

Selection selectFrom(const PickState& initial, bool fused, uint32_t w, uint32_t h,
                     const void* resource) {
    restorePickState(initial);
    clearWorkCounts();
    int eye = 77;
    const bool found = fused
        ? edvr::depthProbeSceneTextureEye(w, h, resource, &eye)
        : oldSceneTextureEye(w, h, resource, &eye);
    return {found, eye, capturePickState(), captureWorkCounts()};
}

Selection checkEquivalent(const char* label, uint32_t w, uint32_t h,
                          const void* resource) {
    const PickState initial = capturePickState();
    const Selection oldResult = selectFrom(initial, false, w, h, resource);
    const Selection fusedResult = selectFrom(initial, true, w, h, resource);
    const std::string prefix = std::string(label) + ": ";
    check(oldResult.found == fusedResult.found,
          (prefix + "return differs").c_str());
    check(oldResult.eye == fusedResult.eye,
          (prefix + "eye differs").c_str());
    check(samePickState(oldResult.state, fusedResult.state),
          (prefix + "pair/cache/scan side effects differ").c_str());
    return fusedResult;
}

struct TargetTableGuard {
    edvr::Target targets[edvr::kMaxTargets];
    int targetCount = edvr::g_targetCount;
    bool wanted = edvr::g_wanted;
    PickState pick = capturePickState();

    TargetTableGuard() { std::memcpy(targets, edvr::g_targets, sizeof(targets)); }
    ~TargetTableGuard() {
        std::memcpy(edvr::g_targets, targets, sizeof(targets));
        edvr::g_targetCount = targetCount;
        edvr::g_wanted = wanted;
        restorePickState(pick);
    }
};

ID3D11Texture2D* fakeTexture(unsigned id) {
    return reinterpret_cast<ID3D11Texture2D*>(uintptr_t{0x10000u + id * 0x100u});
}

void setSyntheticTarget(int index, unsigned textureId, uint32_t w, uint32_t h,
                        uint32_t draws, uint32_t firstBind,
                        DXGI_FORMAT format = DXGI_FORMAT_D32_FLOAT) {
    edvr::Target target{};
    target.dsv = reinterpret_cast<void*>(uintptr_t{0x20000u + unsigned(index) * 0x100u});
    target.tex = fakeTexture(textureId);
    target.w = w;
    target.h = h;
    target.samples = 1;
    target.drawsLastFrame = draws;
    target.firstBindLastFrame = firstBind;
    target.dsvFmt = format;
    edvr::g_targets[index] = target;
}

void baseSyntheticTable() {
    std::memset(edvr::g_targets, 0, sizeof(edvr::g_targets));
    edvr::g_targetCount = 6;
    setSyntheticTarget(0, 10, 200, 100, 10, 2);
    setSyntheticTarget(1, 11, 200, 100, 8, 4);
    setSyntheticTarget(2, 20, 100, 100, 10, 10);
    setSyntheticTarget(3, 21, 100, 100, 8, 20);
    setSyntheticTarget(4, 22, 400, 100, 7, 30);
    setSyntheticTarget(5, 23, 500, 100, 6, 40);
    edvr::g_wanted = true;
    edvr::g_scenePick[0] = 2;
    edvr::g_scenePick[1] = 3;
    edvr::g_scenePickCache = edvr::ScenePickCache{100, 100, true, true};
    edvr::g_scenePickScans = 41;
}

void runEquivalenceTests() {
    TargetTableGuard restore;

    baseSyntheticTable();
    edvr::g_wanted = false;
    Selection result = checkEquivalent("disabled", 100, 100, fakeTexture(20));
    check(!result.found && result.eye == -1, "disabled selection rejects and clears eye");

    baseSyntheticTable();
    result = checkEquivalent("null resource", 100, 100, nullptr);
    check(!result.found && result.eye == -1, "null resource rejects and clears eye");

    baseSyntheticTable();
    edvr::g_scenePick[0] = edvr::g_scenePick[1] = -1;
    result = checkEquivalent("no pair", 100, 100, fakeTexture(20));
    check(!result.found && result.eye == -1, "missing pair rejects and clears eye");

    baseSyntheticTable();
    result = checkEquivalent("settled first eye", 100, 100, fakeTexture(20));
    check(result.found && result.eye == 0, "settled first eye retained");
    baseSyntheticTable();
    result = checkEquivalent("settled second eye", 100, 100, fakeTexture(21));
    check(result.found && result.eye == 1, "settled second eye retained");

    baseSyntheticTable();
    setSyntheticTarget(4, 22, 100, 100, 12, 30);
    edvr::g_scenePickCache.valid = false;
    result = checkEquivalent("invalid cache hysteresis", 100, 100, fakeTexture(20));
    check(result.found && result.eye == 0 && result.state.pair[0] == 2 &&
          result.state.pair[1] == 3 && result.state.scans == 42,
          "invalid cache refresh keeps pair within hysteresis");

    baseSyntheticTable();
    setSyntheticTarget(4, 22, 100, 100, 30, 30);
    setSyntheticTarget(5, 23, 100, 100, 20, 40);
    edvr::g_scenePickCache.valid = false;
    result = checkEquivalent("busiest replacement", 100, 100, fakeTexture(21));
    check(!result.found && result.eye == -1 && result.state.pair[0] == 4 &&
          result.state.pair[1] == 5,
          "refresh rejects a formerly selected texture after replacement");

    baseSyntheticTable();
    edvr::g_scenePickCache.valid = false;
    result = checkEquivalent("alternate size", 200, 100, fakeTexture(20));
    check(!result.found && result.state.pair[0] == 0 && result.state.pair[1] == 1,
          "alternate-size refresh displaces the old pair");

    baseSyntheticTable();
    setSyntheticTarget(0, 10, 300, 100, 10, 2);
    edvr::g_scenePickCache.valid = false;
    result = checkEquivalent("one candidate", 300, 100, fakeTexture(20));
    check(!result.found && result.state.pair[0] == 2 && result.state.pair[1] == 3 &&
          result.state.cache.valid && !result.state.cache.result,
          "failed one-candidate refresh preserves the stale pair");

    baseSyntheticTable();
    setSyntheticTarget(4, 22, 100, 100, 30, 30);
    edvr::g_scenePickCache.valid = false;
    result = checkEquivalent("third same-size target", 100, 100, fakeTexture(22));
    check(!result.found && result.state.scans == 41 && result.state.pair[0] == 2,
          "non-pair target rejects before refresh");

    baseSyntheticTable();
    edvr::g_targets[3].tex = fakeTexture(20);
    edvr::g_targets[3].firstBindLastFrame = 5;
    result = checkEquivalent("same-texture pair aliases", 100, 100, fakeTexture(20));
    check(result.found && result.eye == 0,
          "same-texture pair chooses the first ordered eye");

    baseSyntheticTable();
    edvr::g_targets[0].tex = fakeTexture(20);
    edvr::g_targets[0].dsvFmt = DXGI_FORMAT_UNKNOWN;
    result = checkEquivalent("zero-format first alias", 100, 100, fakeTexture(20));
    check(!result.found && result.eye == -1,
          "first target-table alias controls format validity");

    baseSyntheticTable();
    edvr::g_targets[2].dsvFmt = DXGI_FORMAT_UNKNOWN;
    result = checkEquivalent("invalid other-eye format", 100, 100, fakeTexture(21));
    check(result.found && result.eye == 1,
          "invalid first-eye format does not reject the valid second eye");

    baseSyntheticTable();
    ID3D11Texture2D* evictedTexture = edvr::g_targets[2].tex;
    setSyntheticTarget(2, 25, 100, 100, 11, 9);
    edvr::g_scenePickCache.valid = false;
    result = checkEquivalent("evicted identity", 100, 100, evictedTexture);
    check(!result.found && result.state.scans == 41,
          "evicted identity rejects before a reused-slot refresh");
    result = checkEquivalent("reused slot", 100, 100, fakeTexture(25));
    check(result.found && result.eye == 0,
          "reused scene slot selects its new texture identity");

    baseSyntheticTable();
    const PickState counterStart = capturePickState();
    restorePickState(counterStart);
    clearWorkCounts();
    int eye = -1;
    check(oldSceneTextureEye(100, 100, fakeTexture(20), &eye) && eye == 0,
          "old counter sequence selects first eye");
    check(oldSceneTextureEye(100, 100, fakeTexture(21), &eye) && eye == 1,
          "old counter sequence selects second eye");
    const WorkCounts oldWork = captureWorkCounts();
    restorePickState(counterStart);
    clearWorkCounts();
    check(edvr::depthProbeSceneTextureEye(100, 100, fakeTexture(20), &eye) && eye == 0,
          "fused counter sequence selects first eye");
    check(edvr::depthProbeSceneTextureEye(100, 100, fakeTexture(21), &eye) && eye == 1,
          "fused counter sequence selects second eye");
    const WorkCounts fusedWork = captureWorkCounts();
    check(oldWork.formatVisits == 10 && fusedWork.formatVisits == 7,
          "observed indices 2/3 reduce alternating-eye format visits from 10 to 7");
    check(oldWork.refresh == 3 && fusedWork.refresh == 2 &&
          oldWork.order == 3 && fusedWork.order == 2,
          "fused lookup performs one refresh and order operation per resource");
}

bool oldSceneTextureEyeUninstrumented(uint32_t w, uint32_t h, const void* resource,
                                      int* outEye) {
    *outEye = -1;
    if (!edvr::depthProbeIsSceneDepth(resource)) return false;
    for (int eye = 0; eye < 2; ++eye) {
        if (!edvr::refreshScenePickImpl<false>(w, h)) continue;
        int first, second;
        edvr::sceneOrderFirstSecondImpl<false>(&first, &second);
        ID3D11Texture2D* texture = edvr::g_targets[eye == 0 ? first : second].tex;
        if (edvr::sceneTextureHasFormat<false>(texture) && texture == resource) {
            *outEye = eye;
            return true;
        }
    }
    return false;
}

bool fusedSceneTextureEyeUninstrumented(uint32_t w, uint32_t h, const void* resource,
                                        int* outEye) {
    return edvr::sceneTextureEyeImpl<false>(w, h, resource, outEye);
}

using SelectionFn = bool (*)(uint32_t, uint32_t, const void*, int*);
volatile uintptr_t benchmarkChecksum = 0;

double benchmarkOne(SelectionFn function, const void* first, const void* second,
                    unsigned iterations, LARGE_INTEGER frequency) {
    SelectionFn volatile dispatch = function;
    uintptr_t checksum = 0;
    LARGE_INTEGER begin{}, end{};
    QueryPerformanceCounter(&begin);
    for (unsigned i = 0; i < iterations; ++i) {
        int eye = -1;
        const void* resource = (i & 1) ? second : first;
        const bool found = dispatch(100, 100, resource, &eye);
        checksum = checksum * 33u + uintptr_t(found ? eye + 3 : 1);
    }
    QueryPerformanceCounter(&end);
    benchmarkChecksum += checksum | uintptr_t{1};
    return double(end.QuadPart - begin.QuadPart) * 1.0e9 /
           double(frequency.QuadPart) / double(iterations);
}

double median(std::array<double, 9> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void benchmarkTable(int count, int firstIndex, int secondIndex,
                    const char* label, LARGE_INTEGER frequency) {
    std::memset(edvr::g_targets, 0, sizeof(edvr::g_targets));
    edvr::g_targetCount = count;
    for (int i = 0; i < count; ++i)
        setSyntheticTarget(i, unsigned(i + 1), 100, 100, 1, unsigned(i + 1));
    edvr::g_wanted = true;
    edvr::g_scenePick[0] = firstIndex;
    edvr::g_scenePick[1] = secondIndex;
    edvr::g_scenePickCache = edvr::ScenePickCache{100, 100, true, true};
    const void* first = edvr::g_targets[firstIndex].tex;
    const void* second = edvr::g_targets[secondIndex].tex;
    constexpr unsigned iterations = 300000;
    std::array<double, 9> oldTimes{}, fusedTimes{};
    // Warm code and data before collecting the rotating-order trials.
    benchmarkOne(oldSceneTextureEyeUninstrumented, first, second, 50000, frequency);
    benchmarkOne(fusedSceneTextureEyeUninstrumented, first, second, 50000, frequency);
    for (size_t trial = 0; trial < oldTimes.size(); ++trial) {
        if ((trial & 1) == 0) {
            oldTimes[trial] = benchmarkOne(oldSceneTextureEyeUninstrumented, first, second,
                                           iterations, frequency);
            fusedTimes[trial] = benchmarkOne(fusedSceneTextureEyeUninstrumented, first, second,
                                             iterations, frequency);
        } else {
            fusedTimes[trial] = benchmarkOne(fusedSceneTextureEyeUninstrumented, first, second,
                                             iterations, frequency);
            oldTimes[trial] = benchmarkOne(oldSceneTextureEyeUninstrumented, first, second,
                                           iterations, frequency);
        }
    }
    const double oldNs = median(oldTimes), fusedNs = median(fusedTimes);
    const auto oldRange = std::minmax_element(oldTimes.begin(), oldTimes.end());
    const auto fusedRange = std::minmax_element(fusedTimes.begin(), fusedTimes.end());
    unsigned fusedWins = 0;
    for (size_t i = 0; i < oldTimes.size(); ++i) fusedWins += fusedTimes[i] < oldTimes[i];
    const int oldVisits = (firstIndex + 1) * 2 + secondIndex + 1;
    const int fusedVisits = firstIndex + secondIndex + 2;
    std::printf("depth_scene_pick_test: benchmark %s: old %.2f ns/call [%.2f, %.2f], fused %.2f ns/call [%.2f, %.2f], %.2fx, fused wins %u/9; two-call format visits %d -> %d; iterations %u x 9, checksum 0x%llx\n",
                label, oldNs, *oldRange.first, *oldRange.second,
                fusedNs, *fusedRange.first, *fusedRange.second,
                oldNs / fusedNs, fusedWins, oldVisits, fusedVisits, iterations,
                static_cast<unsigned long long>(benchmarkChecksum));
}

int runBenchmark() {
    TargetTableGuard restore;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 1;
    benchmarkTable(4, 2, 3, "indices-2-3", frequency);
    benchmarkTable(edvr::kMaxTargets, edvr::kMaxTargets - 2,
                   edvr::kMaxTargets - 1, "indices-30-31", frequency);
    return benchmarkChecksum == 0 ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    try {
        if (argc != 2) return 2;
        if (!std::strcmp(argv[1], "--dry-run")) {
            std::puts("depth_scene_pick_test: dry-run");
            return 0;
        }
        if (!std::strcmp(argv[1], "--benchmark")) return runBenchmark();
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
        runEquivalenceTests();

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

        // advanced.eye_depth_capture lights the probe on its own: the flight
        // rig runs fix.temporal_aa AND fix.eye_mask both off, and the eye-run
        // depth capture's scene-pair verdict needs the probe watching anyway.
        temporalMode = "off";
        edvr::depthProbeConfigure(edvr::Config::get());
        check(!edvr::g_wanted, "temporal and eye mask both off leaves the probe unwatched");
        eyeDepthCaptureOn = true;
        edvr::depthProbeConfigure(edvr::Config::get());
        check(edvr::g_wanted, "the eye depth capture enables the depth probe on its own");
        eyeDepthCaptureOn = false;
        edvr::depthProbeConfigure(edvr::Config::get());
        check(!edvr::g_wanted, "the capture off restores the unwatched state");
        temporalMode = "dlss";
        edvr::depthProbeConfigure(edvr::Config::get());
        check(edvr::g_wanted, "temporal AA still enables the probe on its own");

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

        // The draw path's inline eye-draw pre-check (depth_probe.h) against
        // the callee's own common case: a frame's first eye draw is always
        // noted, the same view again needs no call, and a different view or
        // a new frame does. (A null view keeps the callee away from COM.)
        {
            int token = 0;
            void* const other = &token;   // only ever handed to the predicate
            edvr::depthProbeFrameBoundary(nullptr);
            check(edvr::depthProbeEyeDrawNeedsNote(nullptr),
                  "eye-draw pre-check: a frame's first eye draw reaches the note");
            edvr::depthProbeNoteEyeDraw(context.Get(), nullptr, 1);
            check(edvr::g_eyeDrawThisFrame && !edvr::depthProbeEyeDrawNeedsNote(nullptr),
                  "eye-draw pre-check: the same view again is the note's one-compare return");
            check(edvr::depthProbeEyeDrawNeedsNote(other),
                  "eye-draw pre-check: a different view reaches the note");
            edvr::depthProbeFrameBoundary(nullptr);
            check(edvr::depthProbeEyeDrawNeedsNote(nullptr),
                  "eye-draw pre-check: a new frame reaches the note again");
        }

        // The layout census (flight 6, 153446: "now #0 2048x1024" beside a
        // 5088x2862 viewport). A screen-sized target and a shadow atlas that
        // take turns as the busiest each keep their own record -- the atlas
        // never shows the screen's viewport -- and a sample drawn with a
        // viewport outside its own target is counted as such. The views the
        // probe tracks are held while tracked and released when evicted.
        {
            DepthTarget screen = makeTarget(device.Get(), 100, 60), atlas = makeTarget(device.Get(), 64, 32);
            auto refs = [](ID3D11DepthStencilView* v) { v->AddRef(); return v->Release(); };
            const ULONG screenRefs = refs(screen.view.Get());
            auto noted = [&](const DepthTarget& t, unsigned n, std::initializer_list<D3D11_VIEWPORT> vps) {
                context->RSSetViewports(static_cast<UINT>(vps.size()), vps.begin());
                for (unsigned i = 0; i < n; ++i) edvr::depthProbeNoteDraw(context.Get(), t.view.Get(), false, false);
            };
            const D3D11_VIEWPORT full{0, 0, 100, 60, 0, 1}, halfA{0, 0, 32, 32, 0, 1}, halfB{32, 0, 32, 32, 0, 1};
            edvr::layoutReset();
            noted(screen, 1100, {full});
            noted(atlas, 10, {halfA, halfB});
            edvr::depthProbeFrameBoundary(nullptr);                     // the screen is the busiest
            check(refs(screen.view.Get()) == screenRefs + 1, "census: a tracked depth view is held (its address cannot be reused)");
            noted(screen, 1100, {full});                                // sampled into the screen's record
            noted(atlas, 1500, {halfA, halfB});
            edvr::depthProbeFrameBoundary(nullptr);                     // now the atlas is
            noted(atlas, 1500, {halfA, halfB});                         // sampled into the atlas's record
            noted(screen, 10, {full});
            edvr::depthProbeFrameBoundary(nullptr);                     // the atlas again
            noted(atlas, 1, {full});                                    // a viewport the atlas cannot hold
            const int si = edvr::findTarget(screen.view.Get()), ai = edvr::findTarget(atlas.view.Get());
            const edvr::LayoutRecord* rs = nullptr;
            const edvr::LayoutRecord* ra = nullptr;
            for (const auto& r : edvr::g_layoutRecords) {
                if (r.target == si) rs = &r;
                if (r.target == ai) ra = &r;
            }
            check(si >= 0 && ai >= 0 && rs && ra, "census: each target that was the busiest has its own record");
            check(rs->w == 100 && rs->h == 60 && rs->samples == 2 && rs->viewports[0].w == 100 && rs->viewports[0].h == 60 &&
                  !rs->viewports[1].count && rs->outside == 0,
                  "census: the screen's record holds only the screen's own viewport");
            check(ra->w == 64 && ra->h == 32 && ra->samples == 3 && ra->viewports[0].w == 32 && ra->viewports[1].x == 32 &&
                  ra->outside == 1,
                  "census: the atlas's record holds its two half viewports, and the one draw outside it is counted as such");
            for (unsigned i = 0; i < edvr::kReleaseAfterFrames + 2; ++i) edvr::depthProbeFrameBoundary(nullptr);
            check(refs(screen.view.Get()) == screenRefs, "census: an evicted target's view is released");
        }

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
