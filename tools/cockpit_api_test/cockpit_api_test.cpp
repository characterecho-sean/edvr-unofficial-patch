// Real WARP execution coverage for the concurrent TargetSharp + RemLok
// direct-context API notes. The production helpers and collector are linked;
// tests never synthesize collector notes.
#define EDVR_BINDING_SHADOW_EXTERNAL 1

#include "../../src/d3d11/target_sharp.h"
#include "../../src/d3d11/remlok_fix.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/cockpit_cost_sites.h"
#include "../../src/common/plugin_cost.h"
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/frame_flag.h"
#include "../../src/d3d11/shader_swap.h"
#include "../../src/d3d11/exposure_fix.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;
namespace cost = edvr::cockpit_cost;

namespace {
unsigned g_failed = 0;
unsigned g_checks = 0;
unsigned g_compileCalls = 0;
unsigned g_observedEyeSizeCalls = 0;
bool g_compileFails = false;
bool g_sharp = true;
std::string g_remlokMode = "outer";
float g_remlokScale = 0.75f;
float g_remlokAngle = 0.0f;
void* g_slots[static_cast<size_t>(edvr::BindSlot::Count)] = {};
ID3D11PixelShader* g_replacement = nullptr;
ID3D11VertexShader* g_targetVs = nullptr;
uint32_t g_surfaceWidth = 512;
uint32_t g_surfaceHeight = 256;

bool check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++g_failed;
    }
    return condition;
}

void requireHr(HRESULT hr, const char* label) {
    if (FAILED(hr)) {
        std::printf("FAIL: %s (0x%08lx)\n", label,
                    static_cast<unsigned long>(hr));
        ++g_failed;
    }
}

ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "fixture", nullptr,
                                  nullptr, "main", target, 0, 0,
                                  &code, &errors);
    if (FAILED(hr)) {
        if (errors) std::printf("D3DCompile: %s\n",
                                static_cast<const char*>(errors->GetBufferPointer()));
        requireHr(hr, "compile fixture shader");
    }
    return code;
}

bool hasSite(const EdvrPluginCostOwnerV1& owner, uint16_t site) {
    return (owner.apiSiteMask[site >> 6] & (uint64_t(1) << (site & 63))) != 0;
}

void configureCollector(ID3D11DeviceContext* owner) {
    edvrPluginCostConfigure(0x1u, 1000000u);
    edvrPluginCostSetOwnerContext(owner);
    EdvrPluginCostWindowV1 ignored{};
    // Configuration deliberately drops this first (possibly partial) frame.
    check(edvrPluginCostFrameBoundary(0, 0, 0, 0, 0, &ignored) == 0,
          "collector discards its first post-config boundary");
}

void startWindowFrame(uint32_t frameNo, uint8_t nextApiSample) {
    EdvrPluginCostWindowV1 ignored{};
    check(edvrPluginCostFrameBoundary(frameNo, 0, 0, nextApiSample, 0,
                                      &ignored) == 0,
          "nonterminal API window boundary does not report early");
}

void closeWindowFrame(uint32_t frameNo, uint8_t closedApiSample,
                      uint8_t nextApiSample) {
    EdvrPluginCostWindowV1 ignored{};
    check(edvrPluginCostFrameBoundary(frameNo, 0, closedApiSample,
                                      nextApiSample, 0, &ignored) == 0,
          "collector does not report before its fixed window closes");
}

void runTargetPair(ID3D11DeviceContext* ctx, bool expectedMatch) {
    g_surfaceWidth = 512;
    g_surfaceHeight = 256;
    const bool matched = edvr::targetSharpOnEyeDraw(ctx, 'X', 6, 1);
    check(matched == expectedMatch, "TargetSharp eye candidate follows the actual shader and binding predicates");
    if (!matched) return;
    edvr::targetSharpBegin(ctx);
    edvr::targetSharpEnd(ctx);
}

edvr::RemlokAction runRemlokPair(ID3D11DeviceContext* ctx) {
    g_surfaceWidth = 1024;
    g_surfaceHeight = 512;
    const edvr::RemlokAction action = edvr::remlokOnEyeDraw('N', 3, 1);
    if (action == edvr::RemlokAction::kScissor) {
        edvr::remlokScissorBegin(ctx);
        edvr::remlokScissorEnd(ctx);
    }
    return action;
}

bool liveLeafAndCollectorChecks(ID3D11Device* device,
                                ID3D11DeviceContext* immediate,
                                ID3D11DeviceContext* deferred,
                                ID3D11VertexShader* vs,
                                ID3D11PixelShader* stock,
                                ID3D11PixelShader* replacement,
                                ID3D11RasterizerState* rasterizer) {
    bool ok = true;
    g_targetVs = vs;
    g_replacement = replacement;
    g_slots[static_cast<size_t>(edvr::BindSlot::Vs)] = vs;
    g_slots[static_cast<size_t>(edvr::BindSlot::PsSrv0)] = reinterpret_cast<void*>(0x1010);
    g_slots[static_cast<size_t>(edvr::BindSlot::Dsv0)] = nullptr;
    g_slots[static_cast<size_t>(edvr::BindSlot::PsSrv1)] = nullptr;
    g_slots[static_cast<size_t>(edvr::BindSlot::PsSrv2)] = nullptr;
    g_slots[static_cast<size_t>(edvr::BindSlot::PsSrv3)] = nullptr;

    immediate->VSSetShader(vs, nullptr, 0);
    immediate->PSSetShader(stock, nullptr, 0);
    immediate->RSSetState(rasterizer);
    D3D11_VIEWPORT originalViewport{10.0f, 20.0f, 1000.0f, 800.0f, 0.0f, 1.0f};
    immediate->RSSetViewports(1, &originalViewport);
    D3D11_RECT originalRect{11, 22, 900, 700};
    immediate->RSSetScissorRects(1, &originalRect);

    edvr::Config& cfg = edvr::Config::get();
    g_sharp = true;
    g_remlokMode = "outer";
    g_remlokScale = 0.75f;
    g_remlokAngle = 0.0f;
    edvr::targetSharpConfigure(cfg);
    edvr::remlokConfigure(cfg);
    edvr::remlokFrameBoundary();

    configureCollector(immediate);
    startWindowFrame(1, 1);
    check(edvrPluginCostApiSampleContext(immediate) != 0,
          "owner immediate context becomes sample-eligible at the frame boundary");

    runTargetPair(immediate, true);
    ID3D11PixelShader* observed = nullptr;
    immediate->PSGetShader(&observed, nullptr, nullptr);
    ok &= check(observed == stock, "TargetSharp restores the exact original pixel shader after its real WARP context calls");
    if (observed) observed->Release();

    // A matching shape/binding set with no VS still reaches the final shader
    // query, then declines without engaging the PS replacement wrapper.
    immediate->VSSetShader(nullptr, nullptr, 0);
    runTargetPair(immediate, false);
    immediate->VSSetShader(vs, nullptr, 0);

    ok &= check(runRemlokPair(immediate) == edvr::RemlokAction::kScissor,
                "RemLok's real binding/resource predicates select the scissor wrapper");
    g_slots[static_cast<size_t>(edvr::BindSlot::Dsv0)] = reinterpret_cast<void*>(0x2020);
    ok &= check(runRemlokPair(immediate) == edvr::RemlokAction::kNone,
                "RemLok declines a depth-bound candidate before its context state wrapper");
    g_slots[static_cast<size_t>(edvr::BindSlot::Dsv0)] = nullptr;
    UINT vpCount = 1;
    D3D11_VIEWPORT restoredViewport{};
    immediate->RSGetViewports(&vpCount, &restoredViewport);
    ok &= check(vpCount == 1 && std::memcmp(&restoredViewport, &originalViewport,
                                            sizeof(originalViewport)) == 0,
                "RemLok restores the real WARP viewport after scaled begin/end");
    UINT rectCount = 1;
    D3D11_RECT restoredRect{};
    immediate->RSGetScissorRects(&rectCount, &restoredRect);
    ok &= check(rectCount == 1 && std::memcmp(&restoredRect, &originalRect,
                                              sizeof(originalRect)) == 0,
                "RemLok restores the real WARP scissor rectangle");
    ID3D11RasterizerState* restoredState = nullptr;
    immediate->RSGetState(&restoredState);
    ok &= check(restoredState == rasterizer,
                "RemLok restores the exact original rasterizer state");
    if (restoredState) restoredState->Release();

    // Same helpers and actual D3D calls with a foreign/deferred context. The
    // context-identity check must prevent shared collector writes.
    ok &= check(edvrPluginCostApiSampleContext(deferred) == 0,
                "foreign deferred context is not eligible for owner API sampling");
    runTargetPair(deferred, false); // WARP deferred context begins with no VS.
    edvr::remlokFrameBoundary();
    (void)runRemlokPair(deferred);

    // A temporarily unregistered context cannot write the shared counters,
    // even while the frame sample flag remains enabled.
    edvrPluginCostSetOwnerContext(nullptr);
    check(edvrPluginCostApiSampleContext(immediate) == 0,
          "unregistered immediate context is not sample-eligible");
    runTargetPair(immediate, true);
    edvr::remlokFrameBoundary();
    (void)runRemlokPair(immediate);
    edvrPluginCostSetOwnerContext(immediate);

    // Close the first active frame and keep the next sample active.
    closeWindowFrame(2, 1, 1);

    // In a sampled frame, configured-off plugins visit no leaf, so this is a
    // measured empty sample rather than fabricated zero work.
    g_sharp = false;
    g_remlokMode = "stock";
    edvr::targetSharpConfigure(cfg);
    edvr::remlokConfigure(cfg);
    runTargetPair(immediate, false);
    ok &= check(runRemlokPair(immediate) == edvr::RemlokAction::kNone,
                "RemLok stock mode declines without entering the state wrapper");
    closeWindowFrame(3, 1, 0);

    // Preserve an unsampled real execution: the owner API accessor is false,
    // and both wrappers still restore state without collector calls.
    g_sharp = true;
    g_remlokMode = "outer";
    g_remlokScale = 1.0f;
    edvr::targetSharpConfigure(cfg);
    edvr::remlokConfigure(cfg);
    check(edvrPluginCostApiSampleContext(immediate) == 0,
          "unsampled API frame disables the leaf-local note guards");
    runTargetPair(immediate, true);
    edvr::remlokFrameBoundary();
    ok &= check(runRemlokPair(immediate) == edvr::RemlokAction::kScissor,
                "RemLok scale-one path still executes when API sampling is off");
    closeWindowFrame(4, 0, 1);

    // One sampled scale-one path proves the optional viewport getters/setters
    // disappear while common scissor/rasterizer state work remains.
    edvr::remlokFrameBoundary();
    (void)runRemlokPair(immediate);
    closeWindowFrame(5, 1, 1);

    // Frame 9 is a sample with both features disabled; then fill the fixed
    // collector window with empty API-sample frames.
    g_sharp = false;
    g_remlokMode = "stock";
    edvr::targetSharpConfigure(cfg);
    edvr::remlokConfigure(cfg);
    runTargetPair(immediate, false);
    (void)runRemlokPair(immediate);
    closeWindowFrame(6, 1, 0);
    // Close the intervening unsampled frame, then let the next sampled frame
    // and the remaining empty sample frames complete the fixed window.
    startWindowFrame(7, 1);
    EdvrPluginCostWindowV1 report{};
    bool reported = false;
    for (uint32_t frame = 8; frame <= pc::kWindowFrameCount; ++frame) {
        reported = edvrPluginCostFrameBoundary(frame, 0, 1, 1, 0,
                                               &report) != 0;
        if (reported) {
            ok &= check(frame == pc::kWindowFrameCount,
                        "WARP collector report closes after exactly one full window");
            break;
        }
    }
    ok &= check(reported, "actual WARP leaf execution produces a closed collector report");
    if (reported) {
        const EdvrPluginCostOwnerV1& owner = report.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)];
        ok &= check(report.completedApiSampleFrames == pc::kWindowFrameCount - 3,
                    "API denominator counts only explicitly closed sampled frames, without stride scaling");
        ok &= check(owner.apiObserved == 1 && owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 11 &&
                    owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 12,
                    "real TargetSharp/RemLok execution aggregates to 11 read queries and 12 state calls");
        ok &= check(owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 0 &&
                    owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 0 &&
                    owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0,
                    "leaf context calls remain in their actual read/state categories");
        const uint16_t sites[] = {64,65,66,67,72,73,74,75,76,77,78,79,80,81,82};
        for (uint16_t site : sites) {
            ok &= check(hasSite(owner, site), "real execution publishes each expected TargetSharp/RemLok source-site bit");
        }
        ok &= check(!hasSite(owner, 68) && !hasSite(owner, 71) && !hasSite(owner, 83),
                    "unassigned IDs around the leaf ranges remain absent");
    }

    edvr::targetSharpShutdown();
    edvr::remlokShutdown();
    edvrPluginCostShutdown();
    (void)device;
    return ok;
}

bool targetFailureCheck(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs,
                        ID3D11PixelShader* stock) {
    bool ok = true;
    g_targetVs = vs;
    g_sharp = true;
    g_remlokMode = "stock";
    g_compileFails = true;
    g_compileCalls = 0;
    g_slots[static_cast<size_t>(edvr::BindSlot::Vs)] = vs;
    g_slots[static_cast<size_t>(edvr::BindSlot::PsSrv0)] = reinterpret_cast<void*>(0x1010);
    edvr::Config& cfg = edvr::Config::get();
    edvr::targetSharpConfigure(cfg);
    edvr::remlokConfigure(cfg);

    configureCollector(ctx);
    startWindowFrame(100, 1);
    runTargetPair(ctx, true);
    ID3D11PixelShader* current = nullptr;
    ctx->PSGetShader(&current, nullptr, nullptr);
    ok &= check(current == stock,
                "failed shader replacement leaves the WARP context's original PS bound");
    if (current) current->Release();
    ok &= check(g_compileCalls == 2,
                "TargetSharp actually attempts its documented EASU then cubic fallback");
    closeWindowFrame(101, 1, 1);
    EdvrPluginCostWindowV1 report{};
    bool reported = false;
    for (uint32_t frame = 102; frame <= 99 + pc::kWindowFrameCount; ++frame) {
        reported = edvrPluginCostFrameBoundary(frame, 0, 1, 1, 0, &report) != 0;
        if (reported) break;
    }
    ok &= check(reported, "failed-replacement WARP path closes its collector report");
    if (reported) {
        const EdvrPluginCostOwnerV1& owner = report.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)];
        ok &= check(owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 1 &&
                    owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0 &&
                    hasSite(owner, 64) && !hasSite(owner, 65) && !hasSite(owner, 66) && !hasSite(owner, 67),
                    "failed replacement records only the real candidate VS query, not PS apply/restore calls");
    }
    edvr::targetSharpShutdown();
    edvrPluginCostShutdown();
    g_compileFails = false;
    return ok;
}

} // namespace

namespace edvr {

Config& Config::get() { static Config cfg; return cfg; }
bool Config::getBool(const char* key, bool def) const {
    if (std::strcmp(key, "advanced.target_indicator_scale_probe") == 0) return false;
    if (std::strcmp(key, "advanced.remlok_swap_eyes") == 0) return false;
    return def;
}
int Config::getInt(const char*, int def) const { return def; }
float Config::getFloat(const char* key, float def) const {
    if (std::strcmp(key, "advanced.remlok_scale") == 0) return g_remlokScale;
    if (std::strcmp(key, "fix.remlok_line_angle") == 0) return g_remlokAngle;
    if (std::strcmp(key, "advanced.remlok_keep_fraction") == 0) return 0.55f;
    return def;
}
std::string Config::getString(const char* key, const char* def) const {
    if (std::strcmp(key, "experimental.target_indicator") == 0) return g_sharp ? "sharp" : "stock";
    if (std::strcmp(key, "advanced.target_indicator_sharpen") == 0) return "off";
    if (std::strcmp(key, "advanced.target_indicator_vs") == 0) return "";
    if (std::strcmp(key, "fix.remlok_lines") == 0) return g_remlokMode;
    return def ? def : "";
}
int Config::getIntInRange(const char*, int def, int, int) const { return def; }
std::string Config::requestedTemporalMode() const { return ""; }
void Config::set(const char*, const char*) {}

Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) {}

void* bindingGet(BindSlot slot) { return g_slots[static_cast<size_t>(slot)]; }
uint32_t bindingGeneration(BindSlot) { return 1; }
bool bindingResolve(void* view, ResourceInfo* out) {
    if (!view || !out) return false;
    *out = {};
    out->isTexture2D = true;
    out->a = g_surfaceWidth;
    out->b = g_surfaceHeight;
    return true;
}

uint64_t lookupShaderHash(void* shader) {
    return shader == g_targetVs ? 0xE508648660A352B2ull : 0;
}
bool vScreenIsEyeSized(uint32_t width, uint32_t height) {
    return width >= 3000 && height >= 2000;
}
bool vScreenIsEyeSizedObserved(
    uint32_t width, uint32_t height,
    holo_scrim_observation::EyeSizeObservation* observation) {
    ++g_observedEyeSizeCalls;
    const bool result = vScreenIsEyeSized(width, height);
    if (observation) {
        // This cost fixture has only a coarse cutoff, not vScreen's state
        // and near-two size predicate. Leave selector inputs unavailable;
        // the real observed helper is covered by the vScreen WARP rig.
        *observation = {};
    }
    return result;
}
bool eyeTangents(float*, float*) { return false; }
uint32_t cullGuardStatePacked() { return 0; }

ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext*, const char*, size_t,
                                       const char*, const char*, const SwapMacro*,
                                       const char*) {
    ++g_compileCalls;
    if (g_compileFails || !g_replacement) return nullptr;
    g_replacement->AddRef();
    return g_replacement;
}

} // namespace edvr

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("Usage: cockpit_api_test --self-test");
        return 2;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &level, &immediate);
    requireHr(hr, "create WARP device");
    if (FAILED(hr)) return 1;

    ComPtr<ID3D11DeviceContext> deferred;
    requireHr(device->CreateDeferredContext(0, &deferred), "create deferred context");

    const char* vsSource = "float4 main(uint id:SV_VertexID):SV_Position { return float4(id,0,0,1); }";
    const char* psSource = "float4 main():SV_Target { return float4(1,0,0,1); }";
    ComPtr<ID3DBlob> vsCode = compile(vsSource, "vs_5_0");
    ComPtr<ID3DBlob> psCode = compile(psSource, "ps_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> stock;
    ComPtr<ID3D11PixelShader> replacement;
    requireHr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                        nullptr, &vs), "create fixture VS");
    requireHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                        nullptr, &stock), "create fixture stock PS");
    requireHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                        nullptr, &replacement), "create fixture replacement PS");

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterizer;
    requireHr(device->CreateRasterizerState(&rd, &rasterizer), "create fixture rasterizer state");
    if (g_failed) return 1;

    g_replacement = replacement.Get();
    bool ok = liveLeafAndCollectorChecks(device.Get(), immediate.Get(), deferred.Get(),
                                         vs.Get(), stock.Get(), replacement.Get(),
                                         rasterizer.Get());
    ok &= targetFailureCheck(immediate.Get(), vs.Get(), stock.Get());
    ok &= check(g_observedEyeSizeCalls == 0,
                "default-path API-cost checks never invoke the trace-only eye-size bridge");
    edvr::targetSharpShutdown();
    edvr::remlokShutdown();
    edvrPluginCostShutdown();

    std::printf("cockpit_api_test: %s (%u checks, %u failures)\n",
                (ok && g_failed == 0) ? "PASS" : "FAILED", g_checks, g_failed);
    return (ok && g_failed == 0) ? 0 : 1;
}
