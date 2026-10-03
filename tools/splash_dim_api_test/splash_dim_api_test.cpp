// Real System32 WARP coverage for SplashDim's direct-context cost notes and
// the shared binding resolver. The rig invokes the production begin/end path,
// verifies its real D3D state effects, and reads the collector's closed window.
// shaderSwapCreatePs is production code; its internal GetDevice/CreatePixelShader
// calls are intentionally outside the annotated SplashDim and shared-resolver
// slices.

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/guard.h"
#include "../../src/common/log.h"
#include "../../src/common/plugin_cost.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/intro_cost_sites.h"
#include "../../src/d3d11/loader_panel.h"
#include "../../src/d3d11/splash_dim.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;

namespace {

unsigned g_checks = 0;
unsigned g_failures = 0;
std::string g_loadingDim = "screen";
bool g_dimDemand = true;
bool g_eyeSizeAvailable = true;
uint32_t g_eyeWidth = 640;
uint32_t g_eyeHeight = 480;
uint32_t g_faultPsGetCalls = 0;

using GetPixelShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                ID3D11PixelShader**,
                                                ID3D11ClassInstance**, UINT*);
GetPixelShader g_originalPsGetShader = nullptr;

void STDMETHODCALLTYPE faultPsGetShader(ID3D11DeviceContext*,
                                         ID3D11PixelShader**,
                                         ID3D11ClassInstance**, UINT*) {
    ++g_faultPsGetCalls;
    RaiseException(0xE042ED91u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

bool check(bool value, const char* label) {
    ++g_checks;
    if (!value) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
    return value;
}

void checkHr(HRESULT hr, const char* label) {
    check(SUCCEEDED(hr), label);
}

uint64_t siteWord(std::initializer_list<uint16_t> sites, unsigned word) {
    uint64_t bits = 0;
    for (uint16_t site : sites) {
        if (static_cast<unsigned>(site >> 6) == word) bits |= uint64_t(1) << (site & 63);
    }
    return bits;
}

ComPtr<ID3DBlob> compilePixelShader(const char* source) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "splash_dim_api_test",
                                  nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                  &code, &errors);
    if (FAILED(hr) && errors) {
        std::printf("D3DCompile: %s\n",
                    static_cast<const char*>(errors->GetBufferPointer()));
    }
    checkHr(hr, "compile actual stock pixel shader");
    return code;
}

bool createTarget(ID3D11Device* device, uint32_t width, uint32_t height,
                  ComPtr<ID3D11Texture2D>& texture,
                  ComPtr<ID3D11RenderTargetView>& view) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return false;
    return SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &view));
}

bool makeOriginalBlend(ID3D11Device* device,
                       ComPtr<ID3D11BlendState>& state) {
    D3D11_BLEND_DESC desc{};
    desc.RenderTarget[0].BlendEnable = TRUE;
    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateBlendState(&desc, &state));
}

bool closeSampleWindow(uint32_t firstFrame, EdvrPluginCostWindowV1* report) {
    EdvrPluginCostWindowV1 scratch{};
    bool reported = false;
    for (uint32_t i = 1; i <= pc::kWindowFrameCount; ++i) {
        const uint8_t next = i == pc::kWindowFrameCount ? 0 : 1;
        reported = edvrPluginCostFrameBoundary(firstFrame + i, 0, 1,
                                                next, 0, &scratch) != 0;
        if (reported) {
            if (report) *report = scratch;
            break;
        }
        check(i != pc::kWindowFrameCount,
              "collector closes exactly at its configured frame count");
    }
    return reported;
}

void configureCollector(ID3D11DeviceContext* owner, uint32_t firstFrame) {
    edvrPluginCostConfigure(0x1u, 1000000u);
    edvrPluginCostSetOwnerContext(owner);
    EdvrPluginCostWindowV1 ignored{};
    check(edvrPluginCostFrameBoundary(firstFrame, 0, 0, 1, 0, &ignored) == 0,
          "collector drops the configuration boundary before sampling");
    check(edvrPluginCostApiSampleContext(owner) != 0,
          "registered WARP owner context is sample-eligible after boundary publication");
}

bool verifyReportHeader(const EdvrPluginCostWindowV1& report,
                        uint32_t first, uint32_t last) {
    bool ok = true;
    ok &= check(report.version == pc::kWindowVersion && report.profileBit == 0x1u,
                "report carries the configured ABI and profile");
    ok &= check(report.windowFrames == pc::kWindowFrameCount &&
                report.firstFrame == first && report.lastFrame == last,
                "report closes a complete 1800-frame collector window");
    ok &= check(report.completedApiSampleFrames == pc::kWindowFrameCount,
                "report denominator counts exactly 1800 completed API sample frames");
    return ok;
}

bool verifyColdReport(const EdvrPluginCostWindowV1& report,
                      uint32_t first, uint32_t last) {
    bool ok = verifyReportHeader(report, first, last);
    const auto& intro = report.owners[static_cast<uint8_t>(pc::Owner::Intro)];
    const auto& core = report.owners[static_cast<uint8_t>(pc::Owner::Core)];
    ok &= check(intro.apiObserved != 0 && core.apiObserved != 0,
                "cold real WARP execution observes both Intro and Core API notes");
    ok &= check(intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 3 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 1 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 4 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 0 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0,
                "cold SplashDim notes its two saved-state reads, blend GetDevice/Create, and four state calls");
    ok &= check(core.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 4 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 1 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0,
                "cold view resolution and precompiled PS helper count resolver queries plus shader creation");
    ok &= check(intro.apiSiteMask[0] == 0 &&
                intro.apiSiteMask[1] == siteWord({83, 84, 85, 86, 87, 88, 89, 90}, 1),
                "cold Intro coverage mask is exactly the eight annotated direct SplashDim sites");
    ok &= check(core.apiSiteMask[0] == 0 &&
                core.apiSiteMask[1] == siteWord({112, 113, 115, 120, 121}, 1),
                "cold Core mask includes texture resolver and precompiled PS helper sites");
    return ok;
}

bool verifyWarmReport(const EdvrPluginCostWindowV1& report,
                      uint32_t first, uint32_t last) {
    bool ok = verifyReportHeader(report, first, last);
    const auto& intro = report.owners[static_cast<uint8_t>(pc::Owner::Intro)];
    const auto& core = report.owners[static_cast<uint8_t>(pc::Owner::Core)];
    ok &= check(intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 2 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 0 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 4 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 0 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0,
                "warm SplashDim pair counts two saved-state reads and four apply/restore calls only");
    ok &= check(core.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 9 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 0 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0,
                "warm report includes three resolver queries each for success, wrong-size, and missing-eye-size attempts");
    ok &= check(intro.apiSiteMask[0] == 0 &&
                intro.apiSiteMask[1] == siteWord({83, 84, 85, 86, 87, 88}, 1),
                "warm Intro mask excludes the cold-only blend creation sites");
    ok &= check(core.apiSiteMask[0] == 0 &&
                core.apiSiteMask[1] == siteWord({112, 113, 115}, 1),
                "warm Core mask includes GetResource/GetType/Texture2DGetDesc only");
    return ok;
}

bool verifyFaultReport(const EdvrPluginCostWindowV1& report,
                       uint32_t first, uint32_t last) {
    bool ok = verifyReportHeader(report, first, last);
    const auto& intro = report.owners[static_cast<uint8_t>(pc::Owner::Intro)];
    const auto& core = report.owners[static_cast<uint8_t>(pc::Owner::Core)];
    ok &= check(intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 2 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 1 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 0 &&
                intro.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0,
                "guarded fault keeps the completed cold setup and attempted PSGetShader prefix only");
    ok &= check(core.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 4 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 1 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0,
                "guarded fault report includes resolver attempts and the completed cold PS helper creation");
    ok &= check(intro.apiSiteMask[0] == 0 &&
                intro.apiSiteMask[1] == siteWord({83, 89, 90}, 1),
                "fault mask distinguishes blend setup and attempted PSGetShader from later state sites");
    ok &= check(core.apiSiteMask[0] == 0 &&
                core.apiSiteMask[1] == siteWord({112, 113, 115, 120, 121}, 1),
                "fault Core mask preserves resolver prefix and cold precompiled PS helper sites");
    return ok;
}

bool inspectCurrent(ID3D11DeviceContext* ctx, ID3D11PixelShader* expectedPs,
                    ID3D11BlendState* expectedBlend, bool same) {
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT factors[4]{};
    UINT mask = 0;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->OMGetBlendState(&blend, factors, &mask);
    const bool matches = ps == expectedPs && blend == expectedBlend;
    if (ps) ps->Release();
    if (blend) blend->Release();
    return same ? matches : !matches;
}

bool verifyRestored(ID3D11DeviceContext* ctx, ID3D11PixelShader* stockPs,
                    ID3D11BlendState* stockBlend,
                    const FLOAT expectedFactors[4], UINT expectedMask) {
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT factors[4]{};
    UINT mask = 0;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->OMGetBlendState(&blend, factors, &mask);
    const bool restored = ps == stockPs && blend == stockBlend &&
        std::memcmp(factors, expectedFactors, sizeof(factors)) == 0 &&
        mask == expectedMask;
    if (ps) ps->Release();
    if (blend) blend->Release();
    return restored;
}

bool runColdWindow(ID3D11Device* device, ID3D11DeviceContext* ctx,
                   ID3D11PixelShader* stockPs, ID3D11BlendState* stockBlend,
                   ID3D11RenderTargetView* eyeView,
                   const FLOAT factors[4], UINT mask) {
    bool ok = true;
    ID3D11RenderTargetView* target = eyeView;
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->PSSetShader(stockPs, nullptr, 0);
    ctx->OMSetBlendState(stockBlend, factors, mask);
    edvr::bindingSet(edvr::BindSlot::Rtv0, eyeView);
    configureCollector(ctx, 100);

    check(edvrPluginCostApiSampleOwnerThread() != 0,
          "owner-thread sampler is active during the measured cold frame");
    const bool began = edvr::splashDimBegin(ctx);
    ok &= check(began, "cold production SplashDim Begin succeeds on WARP eye target");
    if (began) {
        ID3D11PixelShader* appliedPs = nullptr;
        ID3D11BlendState* appliedBlend = nullptr;
        FLOAT appliedFactors[4]{};
        UINT appliedMask = 0;
        ctx->PSGetShader(&appliedPs, nullptr, nullptr);
        ctx->OMGetBlendState(&appliedBlend, appliedFactors, &appliedMask);
        ok &= check(appliedPs && appliedPs != stockPs && appliedBlend &&
                    appliedBlend != stockBlend,
                    "cold Begin binds its actual replacement PS and blend state");
        if (appliedPs) appliedPs->Release();
        if (appliedBlend) appliedBlend->Release();
        edvr::splashDimEnd(ctx);
        ok &= check(verifyRestored(ctx, stockPs, stockBlend, factors, mask),
                    "cold End restores the exact original PS, blend, factors, and sample mask");
    }

    EdvrPluginCostWindowV1 report{};
    const bool reported = closeSampleWindow(100, &report);
    ok &= check(reported, "cold WARP run returns a completed collector report");
    if (reported) ok &= verifyColdReport(report, 101, 100 + pc::kWindowFrameCount);
    (void)device;
    return ok;
}

bool runWarmWindow(ID3D11DeviceContext* ctx, ID3D11DeviceContext* deferred,
                   ID3D11PixelShader* stockPs, ID3D11BlendState* stockBlend,
                   ID3D11RenderTargetView* eyeView,
                   ID3D11RenderTargetView* wrongSizeView,
                   const FLOAT factors[4], UINT mask) {
    bool ok = true;
    ID3D11RenderTargetView* target = eyeView;
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->PSSetShader(stockPs, nullptr, 0);
    ctx->OMSetBlendState(stockBlend, factors, mask);
    edvr::bindingSet(edvr::BindSlot::Rtv0, eyeView);

    // This initial configuration boundary establishes the owner token while
    // leaving sampling off. Reconfigure after the live unsampled pair so that
    // its frame cannot enter the report denominator.
    edvrPluginCostConfigure(0x1u, 1000000u);
    edvrPluginCostSetOwnerContext(ctx);
    EdvrPluginCostWindowV1 ignored{};
    check(edvrPluginCostFrameBoundary(2999, 0, 0, 0, 0, &ignored) == 0,
          "collector drops an unsampled setup boundary");
    ok &= check(edvrPluginCostApiSampleContext(ctx) == 0,
                "unsampled API frame disables direct SplashDim notes");
    const bool unsampledBegin = edvr::splashDimBegin(ctx);
    ok &= check(unsampledBegin, "unsampled warm SplashDim still engages on a real WARP target");
    if (unsampledBegin) {
        ok &= check(inspectCurrent(ctx, stockPs, stockBlend, false),
                    "unsampled Begin still changes the actual PS and blend state");
        edvr::splashDimEnd(ctx);
        ok &= check(verifyRestored(ctx, stockPs, stockBlend, factors, mask),
                    "unsampled End still restores the actual stock state");
    }
    configureCollector(ctx, 3000);
    ok &= check(edvrPluginCostApiSampleContext(ctx) != 0,
                "next frame re-enables owner-context API sampling");
    // Real declines stop at their production gates: disabled config and absent
    // demand before resolution; missing target before COM calls; the wrong-size
    // RTV and unknown eye size after the real shared resolver queries.
    g_loadingDim = "stock";
    edvr::splashDimConfigure(edvr::Config::get());
    ok &= check(!edvr::splashDimBegin(ctx), "configured-off SplashDim declines before API work");
    g_loadingDim = "screen";
    edvr::splashDimConfigure(edvr::Config::get());
    g_dimDemand = false;
    ok &= check(!edvr::splashDimBegin(ctx), "no-demand SplashDim declines before sampling or resolution");
    g_dimDemand = true;
    ok &= check(!edvr::splashDimBegin(nullptr), "null context declines before sampling or resolution");
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    edvr::bindingSet(edvr::BindSlot::Rtv0, nullptr);
    ok &= check(!edvr::splashDimBegin(ctx), "missing shadow target declines without a resolver query");
    ID3D11RenderTargetView* wrongTarget = wrongSizeView;
    ctx->OMSetRenderTargets(1, &wrongTarget, nullptr);
    edvr::bindingSet(edvr::BindSlot::Rtv0, wrongSizeView);
    ok &= check(!edvr::splashDimBegin(ctx), "real non-eye WARP target declines after its resolver sequence");
    ID3D11RenderTargetView* eyeTarget = eyeView;
    ctx->OMSetRenderTargets(1, &eyeTarget, nullptr);
    edvr::bindingSet(edvr::BindSlot::Rtv0, eyeView);
    g_eyeSizeAvailable = false;
    ok &= check(!edvr::splashDimBegin(ctx), "unknown eye dimensions decline after resolving the actual target");
    g_eyeSizeAvailable = true;

    // One real warm pair is the complete Intro contribution in this report.
    const bool began = edvr::splashDimBegin(ctx);
    ok &= check(began, "warm production SplashDim Begin succeeds");
    if (began) {
        ok &= check(inspectCurrent(ctx, stockPs, stockBlend, false),
                    "warm Begin has changed the real PS and blend state");
        ok &= check(!edvr::splashDimBegin(ctx),
                    "nested Begin declines without replacing the saved active sample");
        edvr::splashDimEnd(nullptr);
        ok &= check(inspectCurrent(ctx, stockPs, stockBlend, false),
                    "null End leaves the active WARP state engaged for its valid End");
        edvr::splashDimEnd(ctx);
        ok &= check(verifyRestored(ctx, stockPs, stockBlend, factors, mask),
                    "valid End after nested/null attempts restores exact WARP state");
    }

    // The registered context is owner-thread-only. Ask from a foreign worker
    // using a separate deferred context, with no overlapping D3D calls.
    bool foreignRejected = false;
    std::thread worker([&] {
        foreignRejected = edvrPluginCostApiSampleContext(deferred) == 0 &&
                          edvrPluginCostApiSampleOwnerThread() == 0;
    });
    worker.join();
    ok &= check(foreignRejected,
                "foreign worker/deferred context cannot enter the registered API sample");

    EdvrPluginCostWindowV1 report{};
    const bool reported = closeSampleWindow(3000, &report);
    ok &= check(reported, "warm WARP run returns a completed collector report");
    if (reported) ok &= verifyWarmReport(report, 3001, 3000 + pc::kWindowFrameCount);
    return ok;
}

bool runGuardedFaultWindow(ID3D11DeviceContext* ctx,
                           ID3D11PixelShader* stockPs,
                           ID3D11BlendState* stockBlend,
                           ID3D11RenderTargetView* eyeView,
                           const FLOAT factors[4], UINT mask) {
    bool ok = true;
    edvr::splashDimShutdown();
    ID3D11RenderTargetView* target = eyeView;
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->PSSetShader(stockPs, nullptr, 0);
    ctx->OMSetBlendState(stockBlend, factors, mask);
    edvr::bindingSet(edvr::BindSlot::Rtv0, eyeView);
    configureCollector(ctx, 6000);

    // Slot 74 is the SDK ID3D11DeviceContext::PSGetShader vtable entry. The
    // repository's resolve_bind_test independently pins the same slot while
    // compiling against the typed C++ interface.
    constexpr size_t kPsGetShaderSlot = 74;
    edvr::VTableHook hook;
    const bool attached = hook.attach(ctx, 128);
    ok &= check(attached, "attach isolated WARP context vtable hook");
    bool committed = false;
    if (attached) {
        void* original = nullptr;
        const bool mode = hook.setMode(edvr::HookMode::CopyVptr);
        ok &= check(mode, "choose private vtable copy for isolated PSGetShader fault injection");
        const bool staged = mode && hook.replace(kPsGetShaderSlot,
            reinterpret_cast<void*>(&faultPsGetShader), &original);
        ok &= check(staged, "stage the typed PSGetShader fault callback at SDK slot 74");
        g_originalPsGetShader = reinterpret_cast<GetPixelShader>(original);
        ok &= check(g_originalPsGetShader != nullptr,
                    "vtable hook captures the real typed WARP PSGetShader target");
        committed = staged && hook.commit();
        ok &= check(committed, "commit the private PSGetShader fault hook");
    }

    if (committed) {
        g_faultPsGetCalls = 0;
        const bool began = edvr::splashDimBegin(ctx);
        ok &= check(!began,
                    "production SplashDim catches one PSGetShader fault and declines without engagement");
        ok &= check(g_faultPsGetCalls == 1,
                    "real WARP vtable callback proves exactly one guarded getter attempt");
        hook.uninstall();
        ok &= check(verifyRestored(ctx, stockPs, stockBlend, factors, mask),
                    "fault before the first state mutation leaves stock WARP state intact");
    }

    EdvrPluginCostWindowV1 report{};
    const bool reported = closeSampleWindow(6000, &report);
    ok &= check(reported, "guarded-fault WARP run returns a completed collector report");
    if (reported) ok &= verifyFaultReport(report, 6001, 6000 + pc::kWindowFrameCount);

    // A single caught fault must leave SplashDim's four-attempt budget usable.
    // Sampling has closed, so this recovery pair proves behavior without
    // contaminating the fault-prefix report.
    edvrPluginCostShutdown();
    const bool recovered = edvr::splashDimBegin(ctx);
    ok &= check(recovered, "one caught getter fault leaves the production path available");
    if (recovered) {
        edvr::splashDimEnd(ctx);
        ok &= check(verifyRestored(ctx, stockPs, stockBlend, factors, mask),
                    "unsampled recovery End restores state after the caught prefix");
    }
    edvr::splashDimShutdown();
    g_originalPsGetShader = nullptr;
    return ok;
}

} // namespace

namespace edvr {

Config& Config::get() { static Config cfg; return cfg; }
std::string Config::getString(const char*, const char*) const { return g_loadingDim; }

Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
// Guard/hook diagnostics use the same no-file boundary as the logger here.
// The rig keeps real COM calls, SEH containment and hook execution intact.
void breadcrumb(const char*) {}

bool loaderPanelDimWanted() { return g_dimDemand; }
bool eyeTextureSize(uint32_t* width, uint32_t* height) {
    if (!g_eyeSizeAvailable || !width || !height) return false;
    *width = g_eyeWidth;
    *height = g_eyeHeight;
    return true;
}

// shader_swap.cpp's cold helper uses these production diagnostics, but the rig
// does not initialize a logger or write any files.
int64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
int64_t qpcFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}
void perfMonitorNoteEvent(uint32_t, double) {}

} // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("splash_dim_api_test: dry-run (no D3D, logger, or file work)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("Usage: splash_dim_api_test --dry-run | --self-test");
        return 2;
    }

    const auto createDevice = edvr::systemD3D11CreateDevice();
    if (!check(createDevice != nullptr, "load System32 D3D11CreateDevice")) return 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                              nullptr, 0, D3D11_SDK_VERSION,
                              &device, &level, &immediate);
    if (!check(SUCCEEDED(hr), "create System32 WARP device")) return 1;
    ComPtr<ID3D11DeviceContext> deferred;
    checkHr(device->CreateDeferredContext(0, &deferred), "create WARP deferred context");

    const char* stockSource =
        "float4 main():SV_Target { return float4(0.25,0.5,0.75,1.0); }";
    ComPtr<ID3DBlob> stockCode = compilePixelShader(stockSource);
    if (!stockCode) return 1;
    ComPtr<ID3D11PixelShader> stockPs;
    checkHr(device->CreatePixelShader(stockCode->GetBufferPointer(),
                                      stockCode->GetBufferSize(), nullptr,
                                      &stockPs), "create real stock pixel shader");
    ComPtr<ID3D11BlendState> stockBlend;
    check(makeOriginalBlend(device.Get(), stockBlend), "create real original blend state");
    ComPtr<ID3D11Texture2D> eyeTexture;
    ComPtr<ID3D11RenderTargetView> eyeView;
    ComPtr<ID3D11Texture2D> wrongTexture;
    ComPtr<ID3D11RenderTargetView> wrongView;
    check(createTarget(device.Get(), g_eyeWidth, g_eyeHeight, eyeTexture, eyeView),
          "create actual eye-sized WARP render target");
    check(createTarget(device.Get(), 320, 240, wrongTexture, wrongView),
          "create actual non-eye WARP render target");
    if (g_failures) return 1;

    const FLOAT factors[4] = {0.2f, 0.3f, 0.4f, 0.5f};
    constexpr UINT sampleMask = 0x12345678u;
    edvr::splashDimConfigure(edvr::Config::get());
    bool ok = runColdWindow(device.Get(), immediate.Get(), stockPs.Get(),
                            stockBlend.Get(), eyeView.Get(), factors, sampleMask);

    // Dispose of cold products, rebuild them on the real path without the API
    // sample enabled, then verify that the next complete sampled pair is warm.
    edvrPluginCostShutdown();
    edvr::splashDimShutdown();
    edvr::bindingSet(edvr::BindSlot::Rtv0, eyeView.Get());
    ID3D11RenderTargetView* target = eyeView.Get();
    immediate->OMSetRenderTargets(1, &target, nullptr);
    immediate->PSSetShader(stockPs.Get(), nullptr, 0);
    immediate->OMSetBlendState(stockBlend.Get(), factors, sampleMask);
    const bool primed = edvr::splashDimBegin(immediate.Get());
    ok &= check(primed, "unmeasured production path primes the actual WARP replacement resources");
    if (primed) edvr::splashDimEnd(immediate.Get());

    ok &= runWarmWindow(immediate.Get(), deferred.Get(), stockPs.Get(),
                        stockBlend.Get(), eyeView.Get(), wrongView.Get(),
                        factors, sampleMask);

    ok &= runGuardedFaultWindow(immediate.Get(), stockPs.Get(), stockBlend.Get(),
                                eyeView.Get(), factors, sampleMask);

    edvr::splashDimShutdown();
    edvr::bindingSet(edvr::BindSlot::Rtv0, nullptr);
    edvr::bindingForgetAll();
    edvrPluginCostShutdown();
    std::printf("splash_dim_api_test: %s (%u checks, %u failures)\n",
                (ok && g_failures == 0) ? "PASS" : "FAILED", g_checks, g_failures);
    return ok && g_failures == 0 ? 0 : 1;
}
