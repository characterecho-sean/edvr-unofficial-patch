#define EDVR_DEPTH_SCENE_PICK_TEST 1
#include "../../src/d3d11/depth_probe.cpp"
#include "../../src/common/system_d3d11.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
unsigned checks = 0;
std::string temporalMode = "dlss";
bool eyeDepthCaptureOn = false;
std::string nativeOnFootKey = "off";   // fix.native_on_foot: off here, so the arming tests below see only the two reasons they always did
unsigned logNotes = 0;                 // F207: the lines the probe writes, counted, and the last one as written
char lastNote[1024] = "";

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
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext*,const void*,size_t,const char*,const char*) { std::abort(); }
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(lastNote, sizeof(lastNote), format, args);
    va_end(args);
    ++logNotes;
}

Config& Config::get() { static Config config; return config; }
std::string Config::getString(const char* key, const char* def) const {
    if (!std::strcmp(key, "fix.temporal_aa")) return temporalMode;
    if (!std::strcmp(key, "fix.native_on_foot")) return nativeOnFootKey;
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

// F196: the probe watches for native on-foot VR alone (depth_probe_wanted.h). A user's log (Quest 2 on WiVRn, fix.temporal_aa = off, the shipped
// default) had F113's row sources decline 157185 draws for want of a scene depth eye and the ADS sight lock on F110's model in every frame: with the
// temporal pass off nothing asked for the scene pair, so it never formed, and the probe was not even watching. The decision, then the arm's work.
void configureRig(edvr::RuntimeProfile profile, const char* mode, bool capture, const char* nativeOnFoot) {
    edvr::runtimeProfileSet(profile);
    temporalMode = mode;
    eyeDepthCaptureOn = capture;
    nativeOnFootKey = nativeOnFoot;
    edvr::depthProbeConfigure(edvr::Config::get());
}

void runNativeOnFootTests(ID3D11DeviceContext* context) {
    using edvr::DepthProbeWhy;
    using edvr::RuntimeProfile;
    edvr::depthProbeShutdown();   // F207: the earlier tests' targets are released and the table is empty before this one's synthetic frames, so no edge below releases a texture that is not real
    TargetTableGuard restore;
    edvr::depthProbeSetNativeOnFoot(true);   // F207: the arm is live on foot only; these tests are the arm's work, so native on foot is active for them (F207's own tests are runOnFootArmTests)

    // The pure decision: (fix.temporal_aa, the eye depth capture, native on-foot VR) -> why the probe watches.
    const struct { const char* mode; bool capture; bool nofVr; DepthProbeWhy why; } decide[] = {
        {"off", false, true, DepthProbeWhy::kNativeOnFoot},   // the shipped default in a headset
        {"off", false, false, DepthProbeWhy::kOff},           // flat, or native on foot off in the VR profile
        {"dlss", false, false, DepthProbeWhy::kTemporal},
        {"dlss", false, true, DepthProbeWhy::kTemporal},      // the pilot's rig: the temporal pass asks, the diagnostics stay
        {"on", false, true, DepthProbeWhy::kTemporal},
        {"banana", false, false, DepthProbeWhy::kTemporal},   // today's rule: any value but off
        {"off", true, false, DepthProbeWhy::kCapture},
        {"off", true, true, DepthProbeWhy::kCapture},         // the capture keeps its diagnostics
        {"", false, true, DepthProbeWhy::kNativeOnFoot},
        {"", false, false, DepthProbeWhy::kOff},
        {"OFF", false, true, DepthProbeWhy::kNativeOnFoot},
    };
    for (const auto& row : decide) {
        const DepthProbeWhy why = edvr::depthProbeWhyFor(row.mode, row.capture, row.nofVr);
        check(why == row.why, "F196: the probe's reason follows (fix.temporal_aa, capture, native on-foot VR)");
        check(edvr::depthProbeWantedFor(row.mode, row.capture, row.nofVr) == (row.why != DepthProbeWhy::kOff),
              "F196: wanted exactly when there is a reason");
        check(edvr::depthProbeDiagnosticsFor(why) == (row.why == DepthProbeWhy::kTemporal || row.why == DepthProbeWhy::kCapture),
              "F196: the diagnostics run for the temporal and capture reasons, never for native on foot alone");
    }
    check(edvr::depthProbeNativeOnFootVr(true, "on") && !edvr::depthProbeNativeOnFootVr(true, "off") &&
          !edvr::depthProbeNativeOnFootVr(false, "on") && !edvr::depthProbeNativeOnFootVr(true, "") &&
          !edvr::depthProbeNativeOnFootVr(true, "ON"),
          "F196: native on-foot VR is the VR profile and fix.native_on_foot exactly 'on' (native_on_foot.cpp readKey's own test)");

    // The decision through depthProbeConfigure: the profile and the key as the config reload reads them.
    const struct { RuntimeProfile profile; const char* mode; bool capture; const char* key; bool wanted; bool pickOnly; } config[] = {
        {RuntimeProfile::Vr, "off", false, "on", true, true},
        {RuntimeProfile::LegacyVr, "off", false, "on", true, true},
        {RuntimeProfile::Vr, "off", false, "off", false, false},      // native on foot disabled in VR, AA off
        {RuntimeProfile::Flat, "off", false, "on", false, false},     // flat: nothing changes
        {RuntimeProfile::Passthrough, "off", false, "on", false, false},
        {RuntimeProfile::Invalid, "off", false, "on", false, false},
        {RuntimeProfile::Vr, "dlss", false, "on", true, false},
        {RuntimeProfile::Vr, "off", true, "on", true, false},
        {RuntimeProfile::Flat, "dlss", false, "on", true, false},     // flat's own arming is as it was
        {RuntimeProfile::Flat, "off", true, "on", true, false},
    };
    for (const auto& row : config) {
        configureRig(row.profile, row.mode, row.capture, row.key);
        check(edvr::g_wanted == row.wanted && edvr::depthProbeWanted() == row.wanted,
              "F196: depthProbeConfigure watches exactly when the decision says");
        check(edvr::g_pickOnly == row.pickOnly, "F196: the scene-pick-only arm is native on-foot VR alone");
    }

    // The arm's work. A synthetic frame: two eye-sized depth targets bound beside eye-sized colour, a third of the eye's size (the HUD's), and a
    // shadow atlas that is the busiest of all and is never beside an eye-sized colour target.
    auto frame = [&]() {
        std::memset(edvr::g_targets, 0, sizeof(edvr::g_targets));
        edvr::g_targetCount = 4;
        const struct { unsigned id; uint32_t w, h, draws, eyeDraws, firstBind; } t[] = {
            {10, 200, 100, 900, 900, 3}, {11, 200, 100, 800, 800, 9}, {12, 100, 100, 2000, 0, 1}, {13, 200, 100, 5, 5, 20},
        };
        for (int i = 0; i < 4; ++i) {
            setSyntheticTarget(i, t[i].id, t[i].w, t[i].h, 0, 0);
            edvr::Target& target = edvr::g_targets[i];
            target.drawsThisFrame = t[i].draws;
            target.eyeRtvDrawsThisFrame = t[i].eyeDraws;
            target.firstBindThisFrame = t[i].firstBind;
            target.lastSeenFrame = edvr::g_frameNo;
        }
        edvr::g_scenePick[0] = edvr::g_scenePick[1] = -1;
        edvr::g_scenePickCache.valid = false;
    };
    auto view = [](int i) { return static_cast<ID3D11DepthStencilView*>(edvr::g_targets[i].dsv); };
    int eye = 77, target = 77;

    configureRig(RuntimeProfile::Vr, "off", false, "on");
    frame();
    check(!edvr::depthProbeCurrentSceneEyeOf(view(0), &eye, &target) && eye == -1,
          "F196: before the first frame boundary there is no scene pair and no eye");
    check(!edvr::depthProbeWantsSample(view(3), view(0)),
          "F196: native on-foot VR alone never unbinds the output merger for a depth sample");
    const uint64_t boundaryReadAt = edvr::g_boundaryLastMs;
    edvr::depthProbeFrameBoundary(context);   // a real context beside targets with no real texture: a frame-boundary read would be begun here and would touch them
    check(edvr::g_boundaryLastMs == boundaryReadAt && !edvr::g_stagingInFlight,
          "F196: native on-foot VR alone begins no frame-boundary read of a depth texture");
    check(edvr::g_layoutTarget == -1, "F196: native on-foot VR alone runs no layout census, though the atlas qualifies for one");
    check(edvr::g_scenePick[0] == 0 && edvr::g_scenePick[1] == 1,
          "F196: the pair forms at the frame boundary by the eye-sized colour target's size, with nobody asking: the two busiest of 200x100, not the 100x100 atlas");
    check(edvr::depthProbeCurrentSceneEyeOf(view(0), &eye, &target) && eye == 0 && target == 0 &&
          edvr::depthProbeCurrentSceneEyeOf(view(1), &eye, &target) && eye == 1 && target == 1,
          "F196: the pair answers each eye by first bind, the way the temporal pass's does");
    check(!edvr::depthProbeCurrentSceneEyeOf(view(2), &eye, &target) && eye == -1 &&
          !edvr::depthProbeCurrentSceneEyeOf(view(3), &eye, &target) && eye == -1,
          "F196: the atlas and the HUD's third target are not scene eyes");
    // F199: the pick-only arm now hands out the scene depth (F85c's reticle march, F119's grid and F123's wall contact never had it with Anti-aliasing off), from the pair the frame
    // boundary formed, for the size that pair is, and never by re-picking.
    {
        ID3D11Texture2D* texture = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{1});
        const uint32_t scansBefore = edvr::g_scenePickScans;
        const int pairBefore[2] = {edvr::g_scenePick[0], edvr::g_scenePick[1]};
        check(edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == fakeTexture(10),
              "F199: the pick-only arm hands out the first eye's scene depth for the pair's size (by first bind: target 0)");
        check(edvr::depthProbeSceneDepth(200, 100, 1, &texture) && texture == fakeTexture(11),
              "F199: ...and the second eye's");
        ID3D11Texture2D* formatted = nullptr;
        uint32_t dsvFormat = 0;
        check(edvr::depthProbeSceneDepthFormat(200, 100, 1, &formatted, &dsvFormat) && formatted == fakeTexture(11) && dsvFormat == static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT),
              "F199: depthProbeSceneDepthFormat rides on it: the eye's texture and the view format the game binds it with");
        // a wrong size is refused: the 100x100 atlas's size (a target is there, busy) and a size nothing has; the pair stays and no scan is made
        check(!edvr::depthProbeSceneDepth(100, 100, 0, &texture) && texture == nullptr && !edvr::depthProbeSceneDepth(201, 100, 1, &texture) && texture == nullptr &&
              !edvr::depthProbeSceneDepth(200, 101, 0, &texture) && texture == nullptr,
              "F199: a wrong size is refused, with no texture");
        check(edvr::g_scenePick[0] == pairBefore[0] && edvr::g_scenePick[1] == pairBefore[1] && edvr::g_scenePickScans == scansBefore,
              "F199: the refusals did not re-pick: the boundary's pair stands and no scan of the targets was made (an asker of another size cannot displace it within the frame)");
        check(!edvr::depthProbeSceneDepth(200, 100, 2, &texture) && texture == nullptr && !edvr::depthProbeSceneDepth(200, 100, -1, &texture) && texture == nullptr &&
              !edvr::depthProbeSceneDepth(200, 100, 0, nullptr),
              "F199: an eye other than 0 or 1, and no output, are refused");
        // the pair's size asked repeatedly agrees with the boundary's pick at every call (it is what refreshScenePick would have returned) and does not move it
        for (int i = 0; i < 4; ++i)
            check(edvr::depthProbeSceneDepth(200, 100, i & 1, &texture) && texture == fakeTexture(i & 1 ? 11 : 10) && edvr::g_scenePick[0] == pairBefore[0] && edvr::g_scenePick[1] == pairBefore[1],
                  "F199: asked again, the same pair");
        // the depth is not handed out from a pair whose targets were not drawn into last frame (refreshScenePick's own rule), nor when a target has left the table
        const edvr::Target saved0 = edvr::g_targets[0], saved1 = edvr::g_targets[1];
        edvr::g_targets[0].drawsLastFrame = 0;
        check(!edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == nullptr, "F199: a pair whose target was not drawn into last frame is refused");
        edvr::g_targets[0] = saved0;
        edvr::g_targets[1].dsv = nullptr;
        check(!edvr::depthProbeSceneDepth(200, 100, 1, &texture) && texture == nullptr, "F199: a pair whose target has left the table is refused");
        edvr::g_targets[1] = saved1;
        check(edvr::depthProbeSceneDepth(200, 100, 1, &texture) && texture == fakeTexture(11), "F199: and handed out again once it is back");
    }
    // The pair is held while the game keeps drawing it (hysteresis): the same frame again, the same pair.
    frame();
    edvr::g_scenePick[0] = 1;
    edvr::g_scenePick[1] = 0;
    edvr::depthProbeFrameBoundary(context);
    check(edvr::g_scenePick[0] == 1 && edvr::g_scenePick[1] == 0 &&
          edvr::depthProbeCurrentSceneEyeOf(view(0), &eye, &target) && eye == 0,
          "F196: a settled pair is kept across frames (the temporal pass's hysteresis), the eye still by first bind");

    // The control: the same frame under the temporal reason keeps the diagnostics -- the unbind-and-sample is asked for and the layout census arms --
    // and does not form the pair itself (the temporal pass is the asker there).
    configureRig(RuntimeProfile::Vr, "dlss", false, "on");
    frame();
    check(!edvr::g_stagingInFlight && edvr::depthProbeWantsSample(view(3), view(0)),
          "F196 control: with a temporal mode on, the unbind-and-sample is still asked for");
    edvr::depthProbeFrameBoundary(nullptr);
    check(edvr::g_layoutTarget == 2, "F196 control: with a temporal mode on, the layout census still arms on the atlas");
    check(edvr::g_scenePick[0] == -1 && edvr::g_scenePick[1] == -1,
          "F196 control: with a temporal mode on, the probe forms no pair of its own at the boundary (depthProbeSceneDepth does)");
    {   // F199 control: with a temporal mode on depthProbeSceneDepth is what it was: it re-picks by the size asked (the pair forms here), and a size with no pair is refused
        ID3D11Texture2D* texture = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{1});
        const uint32_t scansBefore = edvr::g_scenePickScans;
        check(edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == fakeTexture(10) && edvr::g_scenePick[0] == 0 && edvr::g_scenePick[1] == 1 &&
                  edvr::g_scenePickScans == scansBefore + 1,
              "F199 control: with a temporal mode on the asker forms the pair itself, by a scan of the targets (the temporal pass's way)");
        check(edvr::depthProbeSceneDepth(200, 100, 1, &texture) && texture == fakeTexture(11) && edvr::g_scenePickScans == scansBefore + 1,
              "F199 control: ...and the second eye's from the cache");
    }
    edvr::g_layoutTarget = edvr::g_layoutRecord = -1;
    edvr::layoutReset();

    // The probe off in the profile the key does not apply to: no pair, nothing counted.
    configureRig(RuntimeProfile::Flat, "off", false, "on");
    frame();
    edvr::depthProbeFrameBoundary(nullptr);
    check(edvr::g_scenePick[0] == -1 && !edvr::depthProbeCurrentSceneEyeOf(view(0), &eye, &target),
          "F196: in the flat profile with Anti-aliasing off the probe is off and forms nothing");

    configureRig(RuntimeProfile::LegacyVr, "dlss", false, "off");   // the state the rest of this rig runs with
    check(edvr::g_wanted && !edvr::g_pickOnly, "F196: back under the DLSS arming the rest of this rig runs with");
    edvr::depthProbeSetNativeOnFoot(false);
}

// F207 (issue 152): the native on-foot arm is live only while native on-foot VR is active. A user's profile read the arm's cost in the cockpit, the stations and the menus, where nothing reads
// its pair: the per-draw note, the eye-draw note and the boundary pick, and the draw gate held open. Off foot it is not wanted at all; the edge forms the pair from one full frame's counts;
// nothing from before an edge is answered; the temporal and capture arms do not care.
void runOnFootArmTests(ID3D11Device* device) {
    using edvr::DepthProbeWhy;
    using edvr::RuntimeProfile;
    edvr::depthProbeShutdown();   // an empty table; every reference the earlier tests' targets held is released
    edvr::g_scenePick[0] = edvr::g_scenePick[1] = -1;   // and no pair from them (shutdown leaves the indices; no reader answers off foot, and an edge on clears them)
    auto refs = [](IUnknown* p) { p->AddRef(); return p->Release(); };
    int eye = 77, target = 77;
    ID3D11Texture2D* texture = nullptr;
    uint32_t drawsAt = 0;

    // the pure decision: the on-foot arm follows the foot, the others do not
    check(!edvr::depthProbeLiveFor(DepthProbeWhy::kNativeOnFoot, false) && edvr::depthProbeLiveFor(DepthProbeWhy::kNativeOnFoot, true) &&
          edvr::depthProbeLiveFor(DepthProbeWhy::kTemporal, false) && edvr::depthProbeLiveFor(DepthProbeWhy::kTemporal, true) &&
          edvr::depthProbeLiveFor(DepthProbeWhy::kCapture, false) && edvr::depthProbeLiveFor(DepthProbeWhy::kCapture, true) &&
          !edvr::depthProbeLiveFor(DepthProbeWhy::kOff, false) && !edvr::depthProbeLiveFor(DepthProbeWhy::kOff, true),
          "F207: the native on-foot arm is live exactly on foot; the temporal and capture arms are live whenever configured; off is never live");

    // OFF FOOT: configured for the on-foot arm, and nothing is wanted
    edvr::depthProbeSetNativeOnFoot(false);
    configureRig(RuntimeProfile::Vr, "off", false, "on");
    check(edvr::g_why == DepthProbeWhy::kNativeOnFoot && !edvr::g_wanted && !edvr::depthProbeWanted() && !edvr::g_pickOnly,
          "F207: configured for native on-foot VR but not on foot: the reason stands, nothing is wanted, and the draw gate's term (depthProbeWanted) is false");
    const DepthTarget l = makeTarget(device, 200, 100), r = makeTarget(device, 200, 100), hud = makeTarget(device, 100, 100);
    const ULONG lTex = refs(l.texture.Get()), rTex = refs(r.texture.Get()), lView = refs(l.view.Get());
    draws(l, 20);
    draws(r, 18);
    check(edvr::g_drawsThisFrame == 0 && edvr::g_targetCount == 0 && !edvr::depthProbeEyeDrawNeedsNote(nullptr) && !edvr::depthProbeEyeDrawNeedsNote(l.view.Get()),
          "F207: off foot the per-draw note counts nothing and finds no target, and the eye-draw pre-check never reaches the note");
    edvr::depthProbeFrameBoundary(nullptr);
    check(edvr::g_scenePick[0] == -1 && edvr::g_scenePick[1] == -1 && !edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target) && eye == -1 &&
          !edvr::depthProbeSceneEyeOf(l.view.Get(), &eye, &target) && !edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == nullptr &&
          !edvr::depthProbeDrawsAtSize(200, 100, &drawsAt) && refs(l.texture.Get()) == lTex && refs(l.view.Get()) == lView,
          "F207: off foot the boundary does no pick, no reader is answered, and the probe holds no reference to the game's depth");

    // THE EDGE ON: counted from that boundary, the pair from the next one, rows from the second on-foot frame
    const unsigned notes0 = logNotes;
    edvr::depthProbeSetNativeOnFoot(true);
    check(edvr::g_wanted && edvr::depthProbeWanted() && edvr::g_pickOnly && !edvr::depthProbeWantsSample(l.view.Get(), r.view.Get()) && logNotes == notes0 + 1 &&
          std::strstr(lastNote, "depth probe (F207): the on-foot arm is on (native on foot went active") == lastNote,
          "F207: on foot the arm is wanted and pick-only, and the edge writes one line: depth probe (F207): the on-foot arm is on (native on foot went active ...)");
    edvr::depthProbeFrameBoundary(nullptr);   // the edge's own boundary: it rolls nothing
    check(edvr::g_scenePick[0] == -1 && edvr::g_scenePick[1] == -1, "F207: the edge's own boundary forms no pair (no frame has been counted yet)");
    draws(l, 20);   // the first on-foot frame
    draws(r, 18);
    for (unsigned i = 0; i < 40; ++i) edvr::depthProbeNoteDraw(nullptr, hud.view.Get(), false, false);
    check(edvr::g_drawsThisFrame == 78 && !edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target) && eye == -1 && !edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == nullptr,
          "F207: the first on-foot frame is counted, and nothing is answered during it: no pair yet");
    edvr::depthProbeSetNativeOnFoot(true);
    edvr::depthProbeFrameBoundary(nullptr);   // the first on-foot frame's counts roll, and the pair forms from them
    check(edvr::g_scenePick[0] >= 0 && edvr::g_scenePick[1] >= 0 && edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target) && eye == 0 &&
          edvr::depthProbeCurrentSceneEyeOf(r.view.Get(), &eye, &target) && eye == 1 && !edvr::depthProbeCurrentSceneEyeOf(hud.view.Get(), &eye, &target) && eye == -1,
          "F207: the pair forms at the second boundary after the edge, from one full frame's counts: the second on-foot frame has rows, left by first bind");
    check(edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == l.texture.Get() && edvr::depthProbeSceneDepth(200, 100, 1, &texture) && texture == r.texture.Get() &&
          refs(l.texture.Get()) == lTex + 1 && refs(r.texture.Get()) == rTex + 1,
          "F207: and F199's scene depth is handed out for the pair's size; the table holds one reference to each texture while on foot");
    unsigned flap = logNotes;
    for (unsigned i = 0; i < 1000; ++i) { edvr::depthProbeSetNativeOnFoot(true); }
    check(logNotes == flap, "F207: a frame boundary that tells the same value writes nothing (one compare)");

    // THE EDGE OFF: the pair, the counts and everything the probe holds are dropped
    edvr::depthProbeSetNativeOnFoot(false);
    check(!edvr::g_wanted && !edvr::depthProbeWanted() && !edvr::g_pickOnly && edvr::g_targetCount == 0 && edvr::g_scenePick[0] == -1 && edvr::g_scenePick[1] == -1 && edvr::g_drawsThisFrame == 0 &&
          !edvr::g_scenePickCache.valid && refs(l.texture.Get()) == lTex && refs(r.texture.Get()) == rTex && refs(l.view.Get()) == lView &&
          std::strstr(lastNote, "depth probe (F207): the on-foot arm is off (native on foot went inactive") == lastNote,
          "F207: going off foot drops the pair, the table and the counts, releases every reference the probe held (the textures and views), and writes the off line");
    check(!edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target) && !edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == nullptr,
          "F207: nothing is answered after the edge off");
    draws(l, 5);
    check(edvr::g_drawsThisFrame == 0 && edvr::g_targetCount == 0, "F207: off foot again the note counts nothing");

    // A NEW on-foot session with other textures (a hangar load): the old pair is not answered, the new one forms the same way
    const DepthTarget l2 = makeTarget(device, 200, 100), r2 = makeTarget(device, 200, 100);
    edvr::depthProbeSetNativeOnFoot(true);
    edvr::depthProbeFrameBoundary(nullptr);
    check(!edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target) && !edvr::depthProbeCurrentSceneEyeOf(r.view.Get(), &eye, &target),
          "F207: after a second edge on the first session's views are not answered");
    draws(l2, 12);
    draws(r2, 10);
    edvr::depthProbeSetNativeOnFoot(true);
    edvr::depthProbeFrameBoundary(nullptr);
    check(edvr::depthProbeSceneDepth(200, 100, 0, &texture) && texture == l2.texture.Get() && edvr::depthProbeSceneDepth(200, 100, 1, &texture) && texture == r2.texture.Get() &&
          !edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target),
          "F207: the second session's pair is its own targets, formed at the second boundary, and the first session's textures are never answered");
    edvr::depthProbeSetNativeOnFoot(false);

    // A PAIR KEPT FROM BEFORE: the temporal arm formed one; the config moves to the on-foot arm off foot (nothing answers: not wanted), and the edge on clears it before anything can
    configureRig(RuntimeProfile::Vr, "dlss", false, "on");
    draws(l, 20);
    draws(r, 18);
    edvr::depthProbeFrameBoundary(nullptr);
    check(scene(200, 100, 0, true) == l.texture.Get() && edvr::g_pickOnly == false, "F207 control: with a temporal mode the pair forms for its asker, off foot or on");
    configureRig(RuntimeProfile::Vr, "off", false, "on");
    check(!edvr::g_wanted && edvr::g_targetCount == 2 && !edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target) && !edvr::depthProbeSceneDepth(200, 100, 0, &texture),
          "F207: moved to the on-foot arm off foot, the temporal state is not answered (not wanted)");
    edvr::depthProbeSetNativeOnFoot(true);
    check(edvr::g_targetCount == 0 && edvr::g_scenePick[0] == -1 && refs(l.texture.Get()) == lTex && !edvr::depthProbeCurrentSceneEyeOf(l.view.Get(), &eye, &target) &&
          !edvr::depthProbeSceneDepth(200, 100, 0, &texture),
          "F207: the edge on clears a pair and a table kept from before: none of it is answered, none of it is held");
    edvr::depthProbeSetNativeOnFoot(false);

    // THE CONTROLS: the temporal and capture arms do not follow the foot, and an edge changes none of their state
    const struct { const char* mode; bool capture; DepthProbeWhy why; } controls[] = {{"dlss", false, DepthProbeWhy::kTemporal}, {"off", true, DepthProbeWhy::kCapture}};
    for (const auto& c : controls) {
        configureRig(RuntimeProfile::Vr, c.mode, c.capture, "on");
        draws(l, 3);
        draws(r, 3);
        const int count = edvr::g_targetCount;
        const unsigned notesBefore = logNotes;
        bool same = edvr::g_why == c.why && edvr::g_wanted && !edvr::g_pickOnly && edvr::g_drawsThisFrame == 6 && count == 2;
        for (const bool foot : {true, false, true, false}) {
            edvr::depthProbeSetNativeOnFoot(foot);
            same = same && edvr::g_wanted && edvr::depthProbeWanted() && !edvr::g_pickOnly && edvr::g_targetCount == count && edvr::g_drawsThisFrame == 6 && refs(l.texture.Get()) == lTex + 1;
        }
        check(same && logNotes == notesBefore, "F207 control: the temporal and the capture arms are wanted with or without the foot, keep their diagnostics and their table, and an edge writes nothing");
        edvr::depthProbeFrameBoundary(nullptr);
        edvr::depthProbeShutdown();
    }

    // NO FLICKER: the predicate's flaps are written at most kOnFootEdgeLines times, then once as a count, and never a line a frame
    configureRig(RuntimeProfile::Vr, "off", false, "on");
    edvr::depthProbeSetNativeOnFoot(false);
    edvr::g_onFootEdges = 0;
    const unsigned flapStart = logNotes;
    for (unsigned i = 0; i < 100; ++i) edvr::depthProbeSetNativeOnFoot(i % 2 == 0);
    check(edvr::g_onFootEdges == 100 && logNotes - flapStart == edvr::kOnFootEdgeLines + 1 && std::strstr(lastNote, "further edges are counted, not logged") != nullptr,
          "F207: a predicate that flapped on every frame writes the first 24 edges and one line saying the rest are counted, never a line a frame");
    edvr::depthProbeSetNativeOnFoot(false);

    // the reason line says the arm runs on foot only, and the boundary tells the probe before it rolls the counts, and before the draw gate is re-sampled
    {
        std::ifstream probe("src/d3d11/depth_probe.cpp"), vscreen("src/d3d11/vscreen.cpp");
        std::stringstream p, v;
        p << probe.rdbuf();
        v << vscreen.rdbuf();
        const std::string ps = p.str(), vs = v.str();
        const size_t tell = vs.find("depthProbeSetNativeOnFoot(nativeOnFootActive());\n            depthProbeFrameBoundary(g_state->ownerCtx);"), tick = vs.find("tkDepthProbe.run([&] {"),
                     gate = vs.find("drawGateSet(drawGateSubscribed(s));", tell == std::string::npos ? 0 : tell);
        check(!ps.empty() && !vs.empty() && ps.find("depth probe: scene pair on for native on-foot VR, on foot only (Anti-aliasing off):") != std::string::npos && tick != std::string::npos && tell != std::string::npos &&
              tell > tick && tell - tick < 200 && gate != std::string::npos && vs.find("witchspaceStarsHidden() || depthProbeWanted() ||") != std::string::npos,
              "F207: the reason line says on foot only; vScreenFrameBoundary tells the probe nativeOnFootActive() just before the boundary rolls the counts and before it re-samples the draw gate, whose probe term is depthProbeWanted()");
    }
    edvr::depthProbeShutdown();
    configureRig(RuntimeProfile::LegacyVr, "dlss", false, "off");
    check(edvr::g_wanted && !edvr::g_pickOnly, "F207: back under the DLSS arming the rest of this rig runs with");
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

        // Printing must not spend the probe's functional sample budget or couple two target records.
        edvr::Target quiet, secondTarget;
        quiet.sampleLines = 17;
        std::vector<uint64_t> printed;
        for (uint64_t i = 1; i <= 91; ++i) if (edvr::depthSampleLogDue(++quiet.sampleLogCandidates)) printed.push_back(i);
        check(printed == std::vector<uint64_t>({1, 2, 3, 4, 5, 6, 30, 60, 90}) && quiet.sampleLines == 17,
              "depth sample log: the first six then every thirtieth candidate, without changing the sampling counter");
        check(!edvr::depthSampleLogDue(0) && edvr::depthSampleLogDue(++secondTarget.sampleLogCandidates) && secondTarget.sampleLogCandidates == 1,
              "depth sample log: a new target starts its own cadence; candidate zero is not a sample");
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
        // rig runs fix.temporal_aa off, and the eye-run depth capture's
        // scene-pair verdict needs the probe watching anyway.
        temporalMode = "off";
        edvr::depthProbeConfigure(edvr::Config::get());
        check(!edvr::g_wanted, "temporal off leaves the probe unwatched");
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

        // The draw gate (draw_gate.h) is opened for this probe by depthProbeWanted() alone
        // (vscreen.cpp drawGateSubscribed), and both rungs below it lean on that one term:
        // depthProbeNoteDraw behind depthProbeWanted() itself, depthProbeNoteEyeDraw behind
        // depthProbeEyeDrawNeedsNote(). So the second must imply the first, for every view and
        // frame state, or a config that arms only the probe leaves its eye rung behind a shut
        // gate. And the flag must follow depthProbeConfigure's arming rule for the two configs
        // no other gate subscriber covers: the eye depth capture on its own (TAA off), and a
        // fix.temporal_aa value that is not a recognised mode (today's rule: any value but off
        // arms the probe; the gate follows the flag whatever the rule is). No rig covers
        // drawGateSubscribed itself; this is the probe's half.
        {
            int gateToken = 0;
            void* const gateViews[] = {nullptr, &gateToken};
            const bool gateSavedThisFrame = edvr::detail::g_depthProbeEyeDrawThisFrame;
            void* const gateSavedLast = edvr::detail::g_depthProbeLastDsv;
            const struct { const char* mode; bool capture; bool wanted; } gateArming[] = {
                {"off", false, false}, {"off", true, true}, {"dlss", false, true},
                {"on", false, true}, {"banana", false, true},
            };
            for (const auto& arm : gateArming) {
                temporalMode = arm.mode;
                eyeDepthCaptureOn = arm.capture;
                edvr::depthProbeConfigure(edvr::Config::get());
                check(edvr::depthProbeWanted() == arm.wanted,
                      "gate: depthProbeWanted() is true exactly when fix.temporal_aa is not off or the eye depth capture is on");
                for (const bool gateThisFrame : {false, true}) {
                    for (void* const gateLast : gateViews) {
                        edvr::detail::g_depthProbeEyeDrawThisFrame = gateThisFrame;
                        edvr::detail::g_depthProbeLastDsv = gateLast;
                        for (void* const gateView : gateViews) {
                            check(!edvr::depthProbeEyeDrawNeedsNote(gateView) || edvr::depthProbeWanted(),
                                  "gate: the eye rung's pre-check never passes while depthProbeWanted() is false, so one gate term covers both rungs");
                        }
                    }
                }
            }
            temporalMode = "dlss";
            eyeDepthCaptureOn = false;
            edvr::depthProbeConfigure(edvr::Config::get());
            check(edvr::depthProbeWanted(), "gate: back under the DLSS arming the rest of this rig runs with");
            edvr::detail::g_depthProbeEyeDrawThisFrame = gateSavedThisFrame;
            edvr::detail::g_depthProbeLastDsv = gateSavedLast;
        }

        // F196: native on-foot VR alone is a reason to watch, and the scene pair it forms at the frame boundary answers the eyes.
        runNativeOnFootTests(context.Get());
        check(edvr::depthProbeWanted(), "F196: the DLSS arming is back for the rest of this rig");
        // F207: and the arm is live only on foot.
        runOnFootArmTests(device.Get());
        check(edvr::depthProbeWanted(), "F207: the DLSS arming is back for the rest of this rig");

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
