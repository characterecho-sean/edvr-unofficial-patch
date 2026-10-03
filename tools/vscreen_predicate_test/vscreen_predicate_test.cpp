#include "vscreen_predicate_test.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <initializer_list>

#include "../../src/common/system_d3d11.h"
#include "../../src/common/config.h"
#include "../../src/d3d11/vscreen.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/resolve_bind_fix.h"
#include "../../src/d3d11/exposure_fix.h"
#include "../../src/d3d11/basic_draw_observation.h"
#include "../../src/d3d11/eye_census_observation.h"
#include "../../src/d3d11/loader_panel_observation.h"
#include "../../src/d3d11/loader_panel.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/vtable_hook.h"

namespace {
using LoaderGetIndexFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer**,
                                                 DXGI_FORMAT*, UINT*);
LoaderGetIndexFn g_loaderGetIndex = nullptr;
std::uint32_t g_loaderQueryAttempts = 0;
std::uint32_t g_loaderInjectedFaults = 0;
bool g_loaderFault = false;
bool g_loaderRetired = false;
void STDMETHODCALLTYPE loaderReentryGetIndex(ID3D11DeviceContext* context,
    ID3D11Buffer** buffer, DXGI_FORMAT* format, UINT* offset) {
    ++g_loaderQueryAttempts;
    edvr::loaderPanelPredicateTestReentry(!g_loaderRetired, false, g_loaderRetired,
                                         23, UINT32_MAX);
    if (g_loaderFault) {
        ++g_loaderInjectedFaults;
        RaiseException(0xE042ED94u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    g_loaderGetIndex(context, buffer, format, offset);
}

using Microsoft::WRL::ComPtr;
using GetPixelShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11PixelShader**, ID3D11ClassInstance**, UINT*);
GetPixelShaderFn g_realGetPixelShader = nullptr;
std::uint32_t g_getPixelShaderCalls = 0;
bool g_faultGetPixelShader = false;
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(ID3D11PixelShader*);
ReleaseFn g_realShaderRelease = nullptr;
std::uint32_t g_releaseCalls = 0;

void STDMETHODCALLTYPE testGetPixelShader(ID3D11DeviceContext* self,
    ID3D11PixelShader** shader, ID3D11ClassInstance** instances, UINT* count) {
    ++g_getPixelShaderCalls;
    if (g_faultGetPixelShader)
        RaiseException(0xE042ED94u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    g_realGetPixelShader(self, shader, instances, count);
}

ULONG STDMETHODCALLTYPE testShaderRelease(ID3D11PixelShader* self) {
    ++g_releaseCalls;
    const ULONG refs = g_realShaderRelease(self);
    RaiseException(0xE042ED95u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return refs;
}

bool readResolveBind(const edvr::VScreenPredicateTestResult& result,
                     edvr::ResolveBindObservation* fact) {
    return edvr::draw_ladder_trace::resolveBindFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readResolveBindFactForTest(result.token, 0, fact);
}

bool readLoaderPanel(const edvr::VScreenPredicateTestResult& result,
                     edvr::LoaderPanelObservation* fact) {
    return edvr::draw_ladder_trace::loaderPanelFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readLoaderPanelFactForTest(result.token, 0, fact);
}

template <class T, class U>
bool lpRead(const edvr::LoaderPanelRead<T>& read, U expected) {
    return read.reached && read.known && read.value == static_cast<T>(expected);
}

template <class T>
bool lpSkipped(const edvr::LoaderPanelRead<T>& read) {
    return !read.reached && !read.known && read.value == T{};
}

template <class T, class U>
bool rbRead(const edvr::ResolveBindRead<T>& read, U expected) {
    return read.reached && read.known && read.value == static_cast<T>(expected);
}

template <class T>
bool rbSkipped(const edvr::ResolveBindRead<T>& read) {
    return !read.reached && !read.known && read.value == T{};
}

ComPtr<ID3DBlob> compileTestPixelShader() {
    constexpr char source[] = "float4 main():SV_Target{return float4(1,0,0,1);}";
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, sizeof(source) - 1, "vscreen-resolve-bind",
        nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
        &code, &errors);
    if (FAILED(hr) && errors)
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    return SUCCEEDED(hr) ? code : ComPtr<ID3DBlob>{};
}

using GetResourceFn = void(STDMETHODCALLTYPE*)(ID3D11ShaderResourceView*, ID3D11Resource**);
GetResourceFn g_originalGetResource = nullptr;
std::uint32_t g_resourceAttempts = 0;
bool g_injectResourceFault = false;
bool g_mutationAccepted = false;
const edvr::VScreenEyeCensusTestMutation* g_mutation = nullptr;
void STDMETHODCALLTYPE observedGetResource(ID3D11ShaderResourceView* view,
                                          ID3D11Resource** resource) {
    ++g_resourceAttempts;
    if (g_mutation)
        g_mutationAccepted = edvr::vScreenEyeCensusPredicateTestMutate(*g_mutation);
    if (g_injectResourceFault)
        RaiseException(0xE042ED93u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    g_originalGetResource(view, resource);
}

bool hookResource(edvr::VTableHook& hook, ID3D11ShaderResourceView* view) {
    g_resourceAttempts = 0;
    g_mutationAccepted = false;
    return hook.attach(view, 9) && hook.setMode(edvr::HookMode::CopyVptr) &&
        hook.replace(7, reinterpret_cast<void*>(&observedGetResource),
                     reinterpret_cast<void**>(&g_originalGetResource)) && hook.commit();
}

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool readFact(const edvr::VScreenPredicateTestResult& result,
              edvr::BasicDrawObservation* fact) {
    return edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readBasicFactForTest(result.token, 0, fact);
}

bool checkRead(const edvr::BasicDrawRead<std::uintptr_t>& read,
               std::uintptr_t expected) {
    return read.reached && read.known && read.value == expected;
}

bool checkRead(const edvr::BasicDrawRead<std::uint32_t>& read,
               std::uint32_t expected) {
    return read.reached && read.known && read.value == expected;
}

bool checkRead(const edvr::BasicDrawRead<bool>& read, bool expected) {
    return read.reached && read.known && read.value == expected;
}

bool checkSkipped(const edvr::BasicDrawRead<std::uintptr_t>& read) {
    return !read.reached && !read.known && read.value == 0;
}

bool checkSkipped(const edvr::BasicDrawRead<std::uint32_t>& read) {
    return !read.reached && !read.known && read.value == 0;
}

bool checkSkipped(const edvr::BasicDrawRead<bool>& read) {
    return !read.reached && !read.known && !read.value;
}

bool armCapture(const std::wstring& path) {
    edvr::draw_ladder_trace::configure(true, path.c_str());
    if (!edvr::draw_ladder_trace::configured()) return false;
    edvr::draw_ladder_trace::armManual();
    edvr::draw_ladder_trace::frameBegin({});
    return edvr::draw_ladder_trace::capturing();
}

bool readEyeCensus(const edvr::VScreenPredicateTestResult& result,
                   edvr::EyeCensusObservation* fact) {
    return edvr::draw_ladder_trace::eyeCensusFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readEyeCensusFactForTest(result.token, 0, fact);
}

bool eyeRead(const edvr::EyeCensusRead<std::uint32_t>& value,
             std::uint32_t expected) {
    return value.reached && value.known && value.value == expected;
}

bool eyeRead(const edvr::EyeCensusRead<std::uint64_t>& value,
             std::uint64_t expected) {
    return value.reached && value.known && value.value == expected;
}

bool eyeRead(const edvr::EyeCensusRead<std::uint8_t>& value,
             std::uint8_t expected) {
    return value.reached && value.known && value.value == expected;
}

bool eyeRead(const edvr::EyeCensusRead<bool>& value, bool expected) {
    return value.reached && value.known && value.value == expected;
}

template <class T>
bool eyeSkipped(const edvr::EyeCensusRead<T>& value) {
    return !value.reached && !value.known && value.value == T{};
}

bool visitCensus(ID3D11DeviceContext* context, char kind, std::uint32_t count,
                 const edvr::VScreenEyeCensusTestRule* rules,
                 std::uint32_t ruleCount, void* const srvs[4],
                 std::uint64_t vsHash, std::uint64_t seed, bool traced,
                 edvr::VScreenPredicateTestResult* result) {
    void* priorSrvs[4]{};
    for (std::uint32_t i = 0; i < 4; ++i)
        priorSrvs[i] = edvr::bindingGet(static_cast<edvr::BindSlot>(
            static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + i));
    void* const priorVs = edvr::bindingGet(edvr::BindSlot::Vs);
    const std::uint64_t priorHash = edvr::bindingShaderHash(edvr::BindSlot::Vs);
    bool okay = edvr::vScreenEyeCensusPredicateTestVisit(
        context, kind, count, rules, ruleCount, srvs, vsHash, seed, traced, result);
    for (std::uint32_t i = 0; i < 4; ++i)
        okay &= check(edvr::bindingGet(static_cast<edvr::BindSlot>(
                          static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + i)) == priorSrvs[i],
                      "EyeCensus seam restores prior SRV binding");
    okay &= check(edvr::bindingGet(edvr::BindSlot::Vs) == priorVs &&
                      edvr::bindingShaderHash(edvr::BindSlot::Vs) == priorHash,
                  "EyeCensus seam restores prior VS identity and hash");
    return okay;
}

bool visitResolveBind(ID3D11DeviceContext* context, bool traced,
                      edvr::VScreenPredicateTestResult* result) {
    return edvr::vScreenResolveBindPredicateTestVisit(context, traced, result);
}

bool hookGetPixelShader(edvr::VTableHook& hook, ID3D11DeviceContext* context) {
    g_getPixelShaderCalls = 0;
    void* original = nullptr;
    if (!hook.attach(context, 128) || !hook.setMode(edvr::HookMode::CopyVptr) ||
        !hook.replace(74, reinterpret_cast<void*>(&testGetPixelShader), &original) ||
        !hook.commit()) return false;
    g_realGetPixelShader = reinterpret_cast<GetPixelShaderFn>(original);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        else if (std::strcmp(argv[i], "--self-test") != 0) {
            std::fprintf(stderr, "unsupported argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (dryRun) {
        std::puts("vscreen_predicate_test dry-run: no work performed");
        return 0;
    }
    wchar_t tempPath[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) {
        std::fprintf(stderr, "vscreen_predicate_test: cannot locate temp directory\n");
        return 1;
    }
    const std::wstring logPath = std::wstring(tempPath) + L"edvr_gfx_vscreen_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64()) + L".log";
    HANDLE logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (logFile == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "vscreen_predicate_test: cannot create trace path\n");
        return 1;
    }
    CloseHandle(logFile);

    bool okay = armCapture(logPath);
    if (!okay) {
        std::fprintf(stderr, "vscreen_predicate_test: trace capture did not arm\n");
        edvr::draw_ladder_trace::shutdown();
        DeleteFileW(logPath.c_str());
        return 1;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    ID3D11DeviceContext* deferred = nullptr;
    D3D_FEATURE_LEVEL level{};
    const auto createDevice = edvr::systemD3D11CreateDevice();
    HRESULT hr = createDevice ? createDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &immediate) : E_FAIL;
    okay &= check(SUCCEEDED(hr) && device && immediate,
                  "WARP supplies real immediate context for owner identity");
    if (SUCCEEDED(hr) && device && immediate) {
        okay &= check(edvr::reportSystemD3D11Only("vscreen_predicate_test"),
                      "WARP uses only System32 d3d11");
        hr = device->CreateDeferredContext(0, &deferred);
        okay &= check(SUCCEEDED(hr) && deferred,
                      "WARP supplies a real deferred context for foreign path");
    }

    if (immediate && deferred) {
        using namespace edvr::draw_ladder;
        ID3D11Texture2D* loaderTarget = nullptr;
        ID3D11RenderTargetView* loaderRtv = nullptr;
        D3D11_TEXTURE2D_DESC loaderTargetDesc{};
        loaderTargetDesc.Width = 1024;
        loaderTargetDesc.Height = 512;
        loaderTargetDesc.MipLevels = 1;
        loaderTargetDesc.ArraySize = 1;
        loaderTargetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        loaderTargetDesc.SampleDesc.Count = 1;
        loaderTargetDesc.Usage = D3D11_USAGE_DEFAULT;
        loaderTargetDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&loaderTargetDesc, nullptr, &loaderTarget);
        okay &= check(SUCCEEDED(hr) && loaderTarget,
                      "WARP creates a real 1024x512 loader target texture");
        if (loaderTarget) {
            hr = device->CreateRenderTargetView(loaderTarget, nullptr, &loaderRtv);
            okay &= check(SUCCEEDED(hr) && loaderRtv,
                          "WARP creates a real RTV for the loader target");
        }
        if (loaderRtv) {
            const std::uint32_t chainOrdinals[] = {0};
            edvr::loaderPanelPredicateTestSeed(true, true, true, false,
                1024, 512, chainOrdinals, 1);
            edvr::VScreenPredicateTestResult loaderResult{};
            okay &= check(edvr::vScreenLoaderPanelPredicateTestVisit(
                immediate, loaderRtv, 0, 7, -3, 'X', 30, true, &loaderResult),
                "site 25 trace visitor resolves a real WARP loader RTV");
            okay &= check(loaderResult.siteResult.flow == Flow::Stop &&
                              loaderResult.siteResult.outcome == SiteOutcome::Claimed,
                          "verified chain ordinal produces the actual loader-panel claim");
            edvr::LoaderPanelObservation loaderFact{};
            okay &= check(readLoaderPanel(loaderResult, &loaderFact),
                          "site 25 appends one fact through the real cold trace pool");
            okay &= check(loaderFact.siteId == 25 && loaderFact.kind == 19 &&
                              lpRead(loaderFact.outer.wants, true) &&
                              lpRead(loaderFact.outer.eyeDrawsLastFrame, 0) &&
                              lpRead(loaderFact.outer.rtvPresent, true) &&
                              lpRead(loaderFact.outer.resolved, true) &&
                              lpRead(loaderFact.outer.isTexture2D, true) &&
                              lpRead(loaderFact.outer.targetWidth, 1024) &&
                              lpRead(loaderFact.outer.targetHeight, 512) &&
                              lpRead(loaderFact.outer.qsStartIndex, 7) &&
                              lpRead(loaderFact.outer.qsBaseVertex, -3) &&
                              lpRead(loaderFact.helper.withhold.chainOrdCountGate, 1) &&
                              lpRead(loaderFact.helper.withhold.chainScan[0].chainOrd, 0) &&
                              lpRead(loaderFact.helper.withhold.frameWithheldAfter, true) &&
                              lpRead(loaderFact.helper.withhold.dimLiveAfter, true),
                          "fact records actual resolver, ordinal scan, and withhold writes");

            edvr::loaderPanelPredicateTestSeed(true, true, true, false,
                2048, 1024, chainOrdinals, 1);
            edvr::VScreenPredicateTestResult loaderNegative{};
            okay &= check(edvr::vScreenLoaderPanelPredicateTestVisit(
                immediate, loaderRtv, 0, 0, 0, 'X', 30, true, &loaderNegative) &&
                    loaderNegative.siteResult.flow == Flow::Continue &&
                    loaderNegative.siteResult.outcome == SiteOutcome::Declined,
                "real target with nonmatching chain dimensions declines");
            okay &= check(readLoaderPanel(loaderNegative, &loaderFact) &&
                              lpRead(loaderFact.helper.panel.chainWidth, 2048) &&
                              lpRead(loaderFact.helper.withhold.specDoneGate, true) &&
                              lpSkipped(loaderFact.helper.withhold.chainOnSpecGate),
                          "negative fact preserves dimension failure and lazy speculation gates");

            edvr::VScreenPredicateTestResult loaderOuterDecline{};
            okay &= check(edvr::vScreenLoaderPanelPredicateTestVisit(
                immediate, loaderRtv, 100, 0, 0, 'X', 30, true, &loaderOuterDecline) &&
                    loaderOuterDecline.siteResult.flow == Flow::Continue &&
                    loaderOuterDecline.siteResult.outcome == SiteOutcome::Declined,
                "scene threshold declines before the real RTV resolver");
            okay &= check(readLoaderPanel(loaderOuterDecline, &loaderFact) &&
                              lpRead(loaderFact.outer.wants, true) &&
                              lpRead(loaderFact.outer.eyeDrawsLastFrame, 100) &&
                              lpSkipped(loaderFact.outer.rtvPresent) &&
                              lpSkipped(loaderFact.outer.resolved) &&
                              lpSkipped(loaderFact.helper.wants),
                          "threshold fact leaves resolver and helper inputs unread");

            edvr::loaderPanelPredicateTestSeed(true, true, true, false,
                1024, 512, chainOrdinals, 1);
            edvr::VScreenPredicateTestResult loaderNoTrace{};
            okay &= check(edvr::vScreenLoaderPanelPredicateTestVisit(
                immediate, loaderRtv, 0, 7, -3, 'X', 30, false, &loaderNoTrace) &&
                    loaderNoTrace.siteResult.flow == Flow::Stop &&
                    loaderNoTrace.siteResult.outcome == SiteOutcome::Claimed &&
                    edvr::draw_ladder_trace::loaderPanelFactCountForTest(loaderNoTrace.token) == 0,
                "NoTrace site 25 preserves the claim without capturing a fact");
            edvr::loaderPanelPredicateTestSeed(false, false, false, false,
                0, 0, nullptr, 0);

            const auto loaderVisit = [&](void* target, bool traced = true) {
                return edvr::vScreenLoaderPanelPredicateTestVisit(immediate, target,
                    0, 0, 0, 'X', 30, traced, &loaderResult) &&
                    (!traced || readLoaderPanel(loaderResult, &loaderFact));
            };
            const auto seedSpeculative = [&]() {
                edvr::loaderPanelShutdown();
                edvr::loaderPanelPredicateTestSeed(true, false, false, false,
                    0, 0, nullptr, 0);
            };
            seedSpeculative();
            okay &= check(loaderVisit(loaderRtv) &&
                              loaderResult.siteResult.outcome == SiteOutcome::Claimed &&
                              lpRead(loaderFact.helper.panel.frameFirstPanelDoneBefore, false) &&
                              lpRead(loaderFact.helper.withhold.specDoneGate, false) &&
                              lpRead(loaderFact.helper.withhold.chainOnSpecGate, false) &&
                              lpRead(loaderFact.helper.withhold.retiredGate, false),
                          "actual first panel speculates without a seeded learned chain");
            okay &= check(loaderVisit(loaderRtv) &&
                              loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                              lpRead(loaderFact.helper.panel.frameFirstPanelDoneBefore, true) &&
                              lpRead(loaderFact.helper.withhold.subArmAfterClear, false) &&
                              lpSkipped(loaderFact.helper.withhold.frameWithheldAfter),
                          "second actual panel in the frame does not speculate");

            seedSpeculative();
            okay &= check(loaderVisit(nullptr) &&
                              loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                              lpRead(loaderFact.outer.rtvPresent, false) &&
                              lpRead(loaderFact.outer.resolved, false) &&
                              lpSkipped(loaderFact.outer.isTexture2D) &&
                              lpSkipped(loaderFact.helper.wants),
                          "null RTV consumes the real resolver failure and skips type/helper");
            Microsoft::WRL::ComPtr<ID3D11Buffer> renderBuffer;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bufferTarget;
            D3D11_BUFFER_DESC renderBufferDesc{};
            renderBufferDesc.ByteWidth = 64;
            renderBufferDesc.Usage = D3D11_USAGE_DEFAULT;
            renderBufferDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            D3D11_RENDER_TARGET_VIEW_DESC bufferTargetDesc{};
            bufferTargetDesc.Format = DXGI_FORMAT_R32_FLOAT;
            bufferTargetDesc.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
            bufferTargetDesc.Buffer.NumElements = 16;
            const bool madeBufferTarget = SUCCEEDED(device->CreateBuffer(&renderBufferDesc, nullptr, &renderBuffer)) &&
                SUCCEEDED(device->CreateRenderTargetView(renderBuffer.Get(), &bufferTargetDesc, &bufferTarget));
            okay &= check(madeBufferTarget && loaderVisit(bufferTarget.Get()) &&
                              loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                              lpRead(loaderFact.outer.resolved, true) &&
                              lpRead(loaderFact.outer.isTexture2D, false) &&
                              lpSkipped(loaderFact.outer.targetWidth) && lpSkipped(loaderFact.helper.wants),
                          "real buffer RTV resolves but short-circuits the Texture2D gate");
            for (const auto dims : {std::pair<UINT, UINT>{1023, 512}, {1024, 511}}) {
                D3D11_TEXTURE2D_DESC smallDesc = loaderTargetDesc;
                smallDesc.Width = dims.first; smallDesc.Height = dims.second;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> smallTexture;
                Microsoft::WRL::ComPtr<ID3D11RenderTargetView> smallRtv;
                const bool made = SUCCEEDED(device->CreateTexture2D(&smallDesc, nullptr, &smallTexture)) &&
                    SUCCEEDED(device->CreateRenderTargetView(smallTexture.Get(), nullptr, &smallRtv));
                okay &= check(made && loaderVisit(smallRtv.Get()) &&
                                  loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                                  lpRead(loaderFact.outer.targetWidth, dims.first) &&
                                  (dims.first < 1024 ? lpSkipped(loaderFact.outer.targetHeight) :
                                      lpRead(loaderFact.outer.targetHeight, dims.second)) &&
                                  lpSkipped(loaderFact.helper.wants),
                              "actual undersized RTV preserves lazy width/height gates");
            }
            for (const std::uint32_t position : {47u, 48u}) {
                seedSpeculative();
                edvr::loaderPanelPredicateTestProgress(position, 0, false, 0, 0);
                okay &= check(loaderVisit(loaderRtv) &&
                                  loaderResult.siteResult.outcome == SiteOutcome::Claimed &&
                                  lpRead(loaderFact.helper.sequence.position, position) &&
                                  (position == 47 ? lpRead(loaderFact.helper.sequence.lenAfter, 48) :
                                      lpSkipped(loaderFact.helper.sequence.lenAfter)),
                              "sequence physical boundary controls append without changing speculation");
            }
            for (const auto shape : {std::pair<char, UINT>{'N', 30}, {'X', 0}}) {
                seedSpeculative();
                okay &= check(edvr::vScreenLoaderPanelPredicateTestVisit(immediate, loaderRtv,
                                  0, 0, 0, shape.first, shape.second, true, &loaderResult) &&
                                  readLoaderPanel(loaderResult, &loaderFact) &&
                                  loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                                  lpRead(loaderFact.helper.wants, true) &&
                                  lpRead(loaderFact.helper.contextNonNull, true) &&
                                  lpSkipped(loaderFact.helper.sequence.position),
                              "actual helper kind/count guard skips composition and mutation");
            }
            const std::uint32_t lateOrdinals[4] = {9, 10, 11, 2};
            edvr::loaderPanelPredicateTestSeed(true, true, true, false, 1024, 512, lateOrdinals, 4);
            edvr::loaderPanelPredicateTestProgress(0, 2, false, 0, 0);
            okay &= check(loaderVisit(loaderRtv) &&
                              loaderResult.siteResult.outcome == SiteOutcome::Claimed &&
                              lpRead(loaderFact.helper.withhold.chainScan[3].chainOrd, 2) &&
                              lpRead(loaderFact.helper.withhold.chainScan[0].loopCount, 4) &&
                              lpRead(loaderFact.helper.withhold.chainOrdCountGate, 4) &&
                              lpSkipped(loaderFact.helper.withhold.terminalChainOrdCount),
                          "fourth raw learned-cache ordinal produces the actual claim");
            edvr::loaderPanelPredicateTestSeed(true, true, true, false, 1024, 512, lateOrdinals, 4);
            edvr::loaderPanelPredicateTestProgress(0, UINT32_MAX, false, 0, 0);
            okay &= check(loaderVisit(loaderRtv) &&
                              loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                              lpRead(loaderFact.helper.panel.panelOrdinalBefore, UINT32_MAX) &&
                              lpRead(loaderFact.helper.panel.panelOrdinalAfter, 0) &&
                              lpRead(loaderFact.helper.panel.localOrdinal, UINT32_MAX) &&
                              lpSkipped(loaderFact.helper.withhold.chainOrdCountGate),
                          "ordinal wraps but its consumed sentinel suppresses the chain scan");

            // Real IA buffers make the forwarded collection callback complete;
            // this exercises capture checkpoints, not the historical learner.
            Microsoft::WRL::ComPtr<ID3D11Buffer> loaderIb, loaderVb;
            D3D11_BUFFER_DESC inputDesc{};
            inputDesc.ByteWidth = 64; inputDesc.Usage = D3D11_USAGE_DEFAULT;
            inputDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            bool madeInputs = SUCCEEDED(device->CreateBuffer(&inputDesc, nullptr, &loaderIb));
            inputDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            madeInputs &= SUCCEEDED(device->CreateBuffer(&inputDesc, nullptr, &loaderVb));
            okay &= check(madeInputs, "real loader collection IA buffers exist");
            Microsoft::WRL::ComPtr<ID3D11Buffer> priorLoaderIb, priorLoaderVb;
            DXGI_FORMAT priorLoaderFormat = DXGI_FORMAT_UNKNOWN;
            UINT priorLoaderIndexOffset = 0, priorLoaderStride = 0, priorLoaderVertexOffset = 0;
            immediate->IAGetIndexBuffer(&priorLoaderIb, &priorLoaderFormat, &priorLoaderIndexOffset);
            immediate->IAGetVertexBuffers(0, 1, &priorLoaderVb, &priorLoaderStride, &priorLoaderVertexOffset);
            immediate->IASetIndexBuffer(loaderIb.Get(), DXGI_FORMAT_R16_UINT, 0);
            ID3D11Buffer* loaderVertex = loaderVb.Get();
            const UINT loaderStride = 8, loaderOffset = 0;
            immediate->IASetVertexBuffers(0, 1, &loaderVertex, &loaderStride, &loaderOffset);
            for (const bool fault : {false, true}) {
                seedSpeculative();
                edvr::loaderPanelPredicateTestProgress(0, 0, true, 0, 0);
                edvr::VTableHook queryHook;
                g_loaderQueryAttempts = 0;
                g_loaderFault = fault; g_loaderRetired = fault;
                const bool hooked = queryHook.attach(immediate, 128) &&
                    queryHook.setMode(edvr::HookMode::CopyVptr) &&
                    queryHook.replace(80, reinterpret_cast<void*>(&loaderReentryGetIndex),
                        reinterpret_cast<void**>(&g_loaderGetIndex)) && queryHook.commit();
                okay &= check(hooked, "typed actual IAGetIndexBuffer slot80 callback installed");
                const bool visited = loaderVisit(loaderRtv);
                queryHook.uninstall();
                okay &= check(visited && g_loaderQueryAttempts == 1 &&
                                  loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                                  lpRead(loaderFact.helper.collection.capCountGate, 0) &&
                                  lpRead(loaderFact.helper.collection.guardEntered, true) &&
                                  lpRead(loaderFact.helper.collection.guardReturned, !fault) &&
                                  loaderFact.helper.collection.collectionMutationUnobserved &&
                                  lpRead(loaderFact.helper.withhold.chainOnSpecGate, !fault) &&
                                  (fault ? lpRead(loaderFact.helper.withhold.retiredGate, true) :
                                      lpSkipped(loaderFact.helper.withhold.retiredGate)),
                              "actual collection reentry/fault controls freshly consumed classification flags");
                okay &= check(fault ?
                    (lpRead(loaderFact.helper.collection.capDroppedBeforeWrite, UINT32_MAX) &&
                     lpRead(loaderFact.helper.collection.capDroppedAfterWrite, 0) &&
                     lpSkipped(loaderFact.helper.collection.capCountBeforeWrite)) :
                    (lpRead(loaderFact.helper.collection.capCountBeforeWrite, 23) &&
                     lpRead(loaderFact.helper.collection.capCountAfterWrite, 24) &&
                     lpRead(loaderFact.helper.collection.ibFillBeforeWrite, 0) &&
                     lpRead(loaderFact.helper.collection.ibFillAfterWrite, 60) &&
                     lpSkipped(loaderFact.helper.collection.capDroppedBeforeWrite)),
                    "capture ledger observes immediate write-local values after actual COM callback");
            }
            seedSpeculative();
            edvr::loaderPanelPredicateTestProgress(0, 0, true, 0, 0);
            {
                edvr::VTableHook faultHook;
                g_loaderQueryAttempts = 0;
                g_loaderFault = true; g_loaderRetired = true;
                const bool hooked = faultHook.attach(immediate, 128) &&
                    faultHook.setMode(edvr::HookMode::CopyVptr) &&
                    faultHook.replace(80, reinterpret_cast<void*>(&loaderReentryGetIndex),
                        reinterpret_cast<void**>(&g_loaderGetIndex)) && faultHook.commit();
                okay &= check(hooked, "fresh actual IA hook for NoTrace collection fault");
                const bool visited = loaderVisit(loaderRtv, false);
                faultHook.uninstall();
                okay &= check(visited && g_loaderQueryAttempts == 1 &&
                                  loaderResult.siteResult.outcome == SiteOutcome::Declined &&
                                  edvr::draw_ladder_trace::loaderPanelFactCountForTest(loaderResult.token) == 0,
                              "NoTrace actual collection fault preserves decline without observation");
            }
            g_loaderFault = false;
            seedSpeculative();
            edvr::loaderPanelPredicateTestProgress(0, 0, true, 24, UINT32_MAX);
            okay &= check(loaderVisit(loaderRtv) &&
                              lpRead(loaderFact.helper.collection.capCountGate, 24) &&
                              lpSkipped(loaderFact.helper.collection.guardEntered) &&
                              lpRead(loaderFact.helper.collection.capDroppedAfterWrite, 0) &&
                              !loaderFact.helper.collection.collectionMutationUnobserved,
                          "capture capacity rejects before the callback and wraps the real drop counter");
            seedSpeculative();
            edvr::loaderPanelPredicateTestProgress(48, 0, true, 0, 0);
            okay &= check(loaderVisit(loaderRtv) &&
                              lpSkipped(loaderFact.helper.collection.capCountGate) &&
                              lpSkipped(loaderFact.helper.collection.guardEntered) &&
                              lpRead(loaderFact.helper.collection.capDroppedAfterWrite, 1),
                          "overlong sequence excludes collection before capacity read");
            seedSpeculative();
            edvr::loaderPanelPredicateTestProgress(0, 0, true, 0, 0);
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> loaderTextureView;
            const bool madeTextureView = SUCCEEDED(device->CreateShaderResourceView(
                loaderTarget, nullptr, &loaderTextureView));
            okay &= check(madeTextureView, "real typed loader texture view exists");
            void* const priorTextureBinding = edvr::bindingGet(edvr::BindSlot::PsSrv0);
            edvr::bindingSet(edvr::BindSlot::PsSrv0, loaderTextureView.Get());
            const bool texturedVisit = loaderVisit(loaderRtv);
            edvr::bindingSet(edvr::BindSlot::PsSrv0, priorTextureBinding);
            okay &= check(texturedVisit && loaderResult.siteResult.outcome == SiteOutcome::Claimed &&
                              lpRead(loaderFact.outer.textured, true) &&
                              lpSkipped(loaderFact.helper.collection.capCountGate) &&
                              lpSkipped(loaderFact.helper.collection.guardEntered) &&
                              lpSkipped(loaderFact.helper.collection.capDroppedAfterWrite),
                          "raw nonnull texture binding excludes collection but preserves first-panel speculation");
            // Last LoaderPanel scenario: exhaust its real six-fault lifetime
            // budget, without resetting or modifying the budget in the seam.
            while (g_loaderInjectedFaults < 6) {
                seedSpeculative();
                edvr::loaderPanelPredicateTestProgress(0, 0, true, 0, 0);
                edvr::VTableHook exhaustionHook;
                g_loaderQueryAttempts = 0;
                g_loaderFault = true; g_loaderRetired = true;
                const bool hooked = exhaustionHook.attach(immediate, 128) &&
                    exhaustionHook.setMode(edvr::HookMode::CopyVptr) &&
                    exhaustionHook.replace(80, reinterpret_cast<void*>(&loaderReentryGetIndex),
                        reinterpret_cast<void**>(&g_loaderGetIndex)) && exhaustionHook.commit();
                okay &= check(hooked, "fresh real IA hook for lifetime budget exhaustion");
                const bool visited = loaderVisit(loaderRtv);
                exhaustionHook.uninstall();
                const bool injected = visited && g_loaderQueryAttempts == 1 &&
                    lpRead(loaderFact.helper.collection.guardEntered, true) &&
                    lpRead(loaderFact.helper.collection.guardReturned, false);
                okay &= check(injected, "actual typed collection faults consume the real budget");
                if (!injected) break; // fail boundedly if admission was lost early
            }
            seedSpeculative();
            edvr::loaderPanelPredicateTestProgress(0, 0, true, 0, 0);
            {
                edvr::VTableHook skippedHook;
                g_loaderQueryAttempts = 0;
                const bool hooked = skippedHook.attach(immediate, 128) &&
                    skippedHook.setMode(edvr::HookMode::CopyVptr) &&
                    skippedHook.replace(80, reinterpret_cast<void*>(&loaderReentryGetIndex),
                        reinterpret_cast<void**>(&g_loaderGetIndex)) && skippedHook.commit();
                okay &= check(hooked, "real IA hook observes exhausted-budget skip");
                const bool visited = loaderVisit(loaderRtv);
                skippedHook.uninstall();
                okay &= check(visited && g_loaderInjectedFaults == 6 && g_loaderQueryAttempts == 0 &&
                                  lpRead(loaderFact.helper.collection.guardEntered, false) &&
                                  lpRead(loaderFact.helper.collection.guardReturned, false) &&
                                  lpRead(loaderFact.helper.collection.capDroppedBeforeWrite, 0) &&
                                  lpRead(loaderFact.helper.collection.capDroppedAfterWrite, 1) &&
                                  !loaderFact.helper.collection.collectionMutationUnobserved,
                              "six real faults exclude seventh callback but retain outer drop mutation");
            }
            g_loaderFault = false;
            edvr::loaderPanelShutdown();
            edvr::loaderPanelPredicateTestSeed(false, false, false, false, 0, 0, nullptr, 0);
            immediate->IASetIndexBuffer(priorLoaderIb.Get(), priorLoaderFormat, priorLoaderIndexOffset);
            ID3D11Buffer* priorLoaderVertex = priorLoaderVb.Get();
            immediate->IASetVertexBuffers(0, 1, &priorLoaderVertex, &priorLoaderStride, &priorLoaderVertexOffset);
        }
        if (loaderRtv) loaderRtv->Release();
        if (loaderTarget) loaderTarget->Release();
        constexpr auto contextSite = static_cast<std::uint16_t>(SiteId::kForeignContextNone);
        constexpr auto distanceSite = static_cast<std::uint16_t>(SiteId::kEyeNoDistanceNone);

        edvr::VScreenPredicateTestResult result{};
        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, immediate, immediate, 23, false, true, &result),
                      "trace-enabled owner-context site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Continue &&
                          result.siteResult.outcome == SiteOutcome::Observed &&
                          result.glareClampAfter == 0,
                      "owner compare observes the draw and resets glare clamp");
        edvr::BasicDrawObservation fact{};
        okay &= check(readFact(result, &fact),
                      "owner site emits one fact through the real trace pool");
        okay &= check(fact.siteId == contextSite &&
                          fact.kind == edvr::BasicDrawFactKind::kContext &&
                          checkRead(fact.context.contextIdentity,
                                    reinterpret_cast<std::uintptr_t>(immediate)) &&
                          checkRead(fact.context.ownerContextIdentity,
                                    reinterpret_cast<std::uintptr_t>(immediate)) &&
                          checkRead(fact.context.glareClampBefore, 23) &&
                          checkRead(fact.context.glareClampAfter, 0) &&
                          checkSkipped(fact.distance.distanceEnabled),
                      "context fact contains actual identities and before/after reset reads");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, deferred, immediate, 19, false, true, &result),
                      "trace-enabled foreign-context site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Stop &&
                          result.siteResult.outcome == SiteOutcome::Exited &&
                          result.glareClampAfter == 0,
                      "foreign context exits after the reset");
        okay &= check(readFact(result, &fact) && fact.siteId == contextSite &&
                          checkRead(fact.context.contextIdentity,
                                    reinterpret_cast<std::uintptr_t>(deferred)) &&
                          checkRead(fact.context.ownerContextIdentity,
                                    reinterpret_cast<std::uintptr_t>(immediate)) &&
                          checkRead(fact.context.glareClampBefore, 19) &&
                          checkRead(fact.context.glareClampAfter, 0),
                      "foreign fact records the real deferred and owner context identities");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, false, true, &result),
                      "trace-enabled disabled-distance site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Stop &&
                          result.siteResult.outcome == SiteOutcome::Exited,
                      "disabled distance setting exits at the real site");
        okay &= check(readFact(result, &fact) && fact.siteId == distanceSite &&
                          fact.kind == edvr::BasicDrawFactKind::kDistance &&
                          checkRead(fact.distance.distanceEnabled, false) &&
                          checkSkipped(fact.context.contextIdentity) &&
                          checkSkipped(fact.context.ownerContextIdentity) &&
                          checkSkipped(fact.context.glareClampBefore) &&
                          checkSkipped(fact.context.glareClampAfter),
                      "distance fact contains only the reached raw gate");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, true, true, &result),
                      "trace-enabled enabled-distance site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Continue &&
                          result.siteResult.outcome == SiteOutcome::Declined,
                      "enabled distance setting continues past the negative gate");
        okay &= check(readFact(result, &fact) &&
                          checkRead(fact.distance.distanceEnabled, true),
                      "enabled distance fact records the true raw gate");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, immediate, immediate, 31, false, false, &result),
                      "NoTrace owner specialization executes the real visitor");
        okay &= check(result.siteResult.flow == Flow::Continue &&
                          result.glareClampAfter == 0 &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace preserves reset behavior and emits no cold-pool fact");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, false, false, &result),
                      "NoTrace distance specialization executes the real visitor");
        okay &= check(result.siteResult.flow == Flow::Stop &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace preserves the distance exit without facts");
        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, deferred, immediate, 17, false, false, &result) &&
                          result.siteResult.flow == Flow::Stop &&
                          result.siteResult.outcome == SiteOutcome::Exited &&
                          result.glareClampAfter == 0 &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace foreign specialization resets and exits without facts");
        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, true, false, &result) &&
                          result.siteResult.flow == Flow::Continue &&
                          result.siteResult.outcome == SiteOutcome::Declined &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace enabled-distance specialization declines without facts");

        constexpr std::uint8_t kFilterOff = 0;
        constexpr std::uint8_t kFilterSize = 1;
        constexpr std::uint8_t kFilterEye = 2;
        constexpr std::uint8_t kFilterNone = 3;
        constexpr std::uint8_t kFilterAny = 4;
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Buffer* buffer = nullptr;
        ID3D11ShaderResourceView* bufferSrv = nullptr;
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = 64;
        textureDesc.Height = 32;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&textureDesc, nullptr, &texture);
        okay &= check(SUCCEEDED(hr) && texture, "WARP creates real census SRV texture");
        if (texture) {
            hr = device->CreateShaderResourceView(texture, nullptr, &srv);
            okay &= check(SUCCEEDED(hr) && srv, "WARP creates real census SRV view");
        }
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = 256;
        bufferDesc.Usage = D3D11_USAGE_DEFAULT;
        bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufferDesc.StructureByteStride = 16;
        hr = device->CreateBuffer(&bufferDesc, nullptr, &buffer);
        okay &= check(SUCCEEDED(hr) && buffer, "WARP creates structured census buffer");
        if (buffer) {
            D3D11_SHADER_RESOURCE_VIEW_DESC bufferViewDesc{};
            bufferViewDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            bufferViewDesc.Buffer.FirstElement = 0;
            bufferViewDesc.Buffer.NumElements = 16;
            hr = device->CreateShaderResourceView(buffer, &bufferViewDesc, &bufferSrv);
            okay &= check(SUCCEEDED(hr) && bufferSrv,
                          "WARP creates typed structured-buffer SRV");
        }
        void* srvs[4] = {srv, nullptr, nullptr, nullptr};
        edvr::VScreenPredicateTestResult eyeResult{};
        edvr::VScreenEyeCensusTestRule rule{};
        edvr::EyeCensusObservation eyeFact{};

        okay &= check(visitCensus(immediate, 'N', 3, nullptr, 0, srvs, 0, 8, true,
                                  &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Continue &&
                          eyeResult.siteResult.outcome == SiteOutcome::Declined &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.skipCountGate, 0) &&
                          eyeSkipped(eyeFact.terminalLoopCount) &&
                          eyeSkipped(eyeFact.censusSkippedBefore) &&
                          eyeResult.censusSkippedAfter == 8,
                      "empty EyeCensus config records only its initial raw gate");

        rule.kind = 'N';
        rule.count = 2;
        rule.countHigh = 4;
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0,
                                  UINT64_MAX, true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          eyeResult.siteResult.outcome == SiteOutcome::Exited &&
                          eyeResult.siteResult.subsite == 0 &&
                          eyeResult.censusSkippedAfter == 0 &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].countHighGate, 4) &&
                          eyeRead(eyeFact.rules[0].countMinimum, 2) &&
                          eyeRead(eyeFact.rules[0].countHighBound, 4) &&
                          eyeRead(eyeFact.rules[0].ruleKind, 'N') &&
                          eyeRead(eyeFact.rules[0].filters[0].modeOffGate, kFilterOff) &&
                          eyeRead(eyeFact.censusSkippedBefore, UINT64_MAX) &&
                          eyeRead(eyeFact.censusSkippedAfter, 0),
                      "count range match exits at subsite zero and records wrapping counter write");

        rule = {};
        rule.kind = 'N';
        rule.count = 3;
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 20,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].exactCount, 3) &&
                          eyeSkipped(eyeFact.rules[0].countMinimum) &&
                          eyeSkipped(eyeFact.rules[0].countHighBound),
                      "exact count path consumes only its exact-count operand");

        rule = {};
        rule.kind = 'X';
        rule.count = 3;
        edvr::VScreenEyeCensusTestRule secondRule{};
        secondRule.kind = 'N';
        secondRule.count = 3;
        edvr::VScreenEyeCensusTestRule orderedRules[2] = {rule, secondRule};
        okay &= check(visitCensus(immediate, 'N', 3, orderedRules, 2, srvs, 0, 30,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          eyeResult.siteResult.subsite == 1 &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].ruleKind, 'X') &&
                          eyeRead(eyeFact.rules[1].ruleKind, 'N') &&
                          eyeSkipped(eyeFact.rules[2].loopCount),
                      "rule order chooses the first matching entry and stops later reads");

        rule = {};
        rule.kind = 'N';
        rule.count = 3;
        rule.filters[0] = {kFilterSize, 64, 32};
        okay &= check(srv && visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                         true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].resolved, true) &&
                          eyeRead(eyeFact.rules[0].filters[0].isTexture2D, true) &&
                          eyeRead(eyeFact.rules[0].filters[0].width, 64) &&
                          eyeRead(eyeFact.rules[0].filters[0].configuredWidth, 64) &&
                          eyeRead(eyeFact.rules[0].filters[0].height, 32) &&
                          eyeRead(eyeFact.rules[0].filters[0].configuredHeight, 32),
                      "size filter uses real WARP view dimensions and lazy operands");

        rule.filters[0] = {kFilterSize, 63, 32};
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Continue &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].width, 64) &&
                          eyeRead(eyeFact.rules[0].filters[0].configuredWidth, 63) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].height) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].configuredHeight),
                      "size mismatch short-circuits the height operands");

        srvs[0] = bufferSrv;
        rule.filters[0] = {kFilterSize, 256, 16};
        okay &= check(bufferSrv && visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                               true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Continue &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].resolved, true) &&
                          eyeRead(eyeFact.rules[0].filters[0].isTexture2D, false) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].width),
                      "real structured-buffer SRV resolves but fails the Texture2D filter");
        srvs[0] = srv;

        rule.filters[0] = {kFilterEye, 0, 0};
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Continue &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].eyeSizeAvailable, false) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].eyeWidth) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].width),
                      "eye filter reports an unavailable eye-size answer as a known decline");
        edvr::announceEyeTextureSize(128, 64);
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Continue &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].eyeSizeAvailable, true) &&
                          eyeRead(eyeFact.rules[0].filters[0].eyeWidth, 128) &&
                          eyeRead(eyeFact.rules[0].filters[0].width, 64) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].height),
                      "eye-size mismatch short-circuits the height comparison");
        edvr::announceEyeTextureSize(64, 32);
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].eyeSizeAvailable, true) &&
                          eyeRead(eyeFact.rules[0].filters[0].eyeWidth, 64) &&
                          eyeRead(eyeFact.rules[0].filters[0].eyeHeight, 32),
                      "eye filter matches the published dimensions of the real WARP texture");

        rule.filters[0] = {kFilterAny, 0, 0};
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].modeOffGate, kFilterAny) &&
                          eyeRead(eyeFact.rules[0].filters[0].modeAnyGate, kFilterAny) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].boundNonNull),
                      "Any filter short-circuits before binding access");

        srvs[0] = nullptr;
        rule.filters[0] = {kFilterNone, 0, 0};
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 1,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].boundNonNull, false) &&
                          eyeRead(eyeFact.rules[0].filters[0].modeAfterBound, kFilterNone) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].resolved),
                      "None filter matches an absent view without resolver calls");

        srvs[0] = srv;
        rule = {};
        rule.kind = 'N';
        rule.count = 3;
        rule.vsHash = 0x1122334455667788ull;
        rule.filters[0] = {kFilterSize, 999, 999};
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs,
                                  rule.vsHash, 1, true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].heldVsHash, rule.vsHash) &&
                          eyeRead(eyeFact.rules[0].vsHashCompareExpected, rule.vsHash) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].modeOffGate),
                      "matching VS hash bypasses every SRV filter");

        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs,
                                  0x8877665544332211ull, 1, true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Continue &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].heldVsHash, 0x8877665544332211ull) &&
                          eyeSkipped(eyeFact.rules[0].countHighGate) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].modeOffGate),
                      "VS hash mismatch declines that rule without falling through to SRV tests");

        rule = {};
        rule.kind = 'N';
        rule.count = 3;
        rule.filters[0] = {kFilterOff, 0, 0};
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 9,
                                  false, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Stop &&
                          eyeResult.censusSkippedAfter == 10 &&
                          edvr::draw_ladder_trace::eyeCensusFactCountForTest(eyeResult.token) == 0,
                      "NoTrace EyeCensus keeps the real skip and emits no cold-pool fact");

        // Negative count paths consume kind after count, but no filter operands.
        rule.count = 4;
        rule.countHigh = 8;
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4, true, &eyeResult) &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].countMinimum, 4) &&
                          eyeSkipped(eyeFact.rules[0].countHighBound) &&
                          eyeRead(eyeFact.rules[0].ruleKind, 'N') &&
                          eyeSkipped(eyeFact.rules[0].filters[0].modeOffGate) &&
                          eyeRead(eyeFact.terminalLoopCount, 1),
                      "range lower miss skips upper bound and all filters");
        rule.count = 1;
        rule.countHigh = 2;
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4, true, &eyeResult) &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].countHighBound, 2) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].modeOffGate),
                      "range upper miss consumes upper bound");
        rule.countHigh = 0;
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4, true, &eyeResult) &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].exactCount, 1) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].modeOffGate),
                      "exact count miss skips filters");
        edvr::VScreenEyeCensusTestRule eightRules[8]{};
        for (auto& entry : eightRules) { entry.kind = 'N'; entry.count = 1; }
        eightRules[7].count = 3;
        okay &= check(visitCensus(immediate, 'N', 3, eightRules, 8, srvs, 0, 4, true, &eyeResult) &&
                          eyeResult.siteResult.subsite == 7 &&
                          eyeResult.siteResult.outcome == SiteOutcome::Exited &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[7].loopCount, 8) &&
                          eyeSkipped(eyeFact.terminalLoopCount),
                      "eighth rule wins without a terminal loop read");
        eightRules[7].count = 1;
        okay &= check(visitCensus(immediate, 'N', 3, eightRules, 8, srvs, 0, 4, true, &eyeResult) &&
                          eyeResult.siteResult.outcome == SiteOutcome::Declined &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[7].loopCount, 8) &&
                          eyeRead(eyeFact.terminalLoopCount, 8),
                      "all eight misses preserve final consumed loop bound");

        rule = {};
        rule.kind = 'N'; rule.count = 3;
        rule.filters[0] = {kFilterNone, 0, 0};
        edvr::VScreenEyeCensusTestMutation mutation{};
        {
            edvr::VTableHook resourceHook;
            okay &= check(srv && hookResource(resourceHook, srv), "hook real SRV GetResource slot seven");
            okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4, true, &eyeResult) &&
                              eyeResult.siteResult.outcome == SiteOutcome::Declined &&
                              readEyeCensus(eyeResult, &eyeFact) &&
                              eyeRead(eyeFact.rules[0].filters[0].boundNonNull, true) &&
                              eyeSkipped(eyeFact.rules[0].filters[0].resolved) &&
                              g_resourceAttempts == 0,
                          "None rejects a present real view without a query");

            mutation.changeFilter = true;
            mutation.filter = {kFilterSize, 64, 32};
            mutation.changeCounter = true;
            mutation.counter = UINT64_MAX;
            g_mutation = &mutation;
            rule.filters[0] = {kFilterEye, 999, 999};
            okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4, true, &eyeResult) &&
                              g_resourceAttempts == 1 && g_mutationAccepted &&
                              eyeResult.siteResult.outcome == SiteOutcome::Exited &&
                              eyeResult.censusSkippedAfter == 0 && readEyeCensus(eyeResult, &eyeFact) &&
                              eyeRead(eyeFact.rules[0].filters[0].modeOffGate, kFilterEye) &&
                              eyeRead(eyeFact.rules[0].filters[0].modeAfterBound, kFilterEye) &&
                              eyeRead(eyeFact.rules[0].filters[0].modeAfterResolve, kFilterSize) &&
                              eyeRead(eyeFact.rules[0].filters[0].configuredWidth, 64) &&
                              eyeRead(eyeFact.rules[0].filters[0].configuredHeight, 32) &&
                              eyeSkipped(eyeFact.rules[0].filters[0].eyeSizeAvailable) &&
                              eyeRead(eyeFact.censusSkippedBefore, UINT64_MAX) &&
                              eyeRead(eyeFact.censusSkippedAfter, 0),
                          "real COM reentry changes later mode dimensions and write-local counter");
            resourceHook.uninstall();
            g_mutation = nullptr;
        }

        {
            edvr::VTableHook resourceHook;
            okay &= check(hookResource(resourceHook, srv), "rehook real SRV for loop-count reentry");
            mutation = {};
            mutation.changeFilter = true; mutation.filter = {kFilterSize, 63, 32};
            mutation.changeLoopCount = true; mutation.loopCount = 1;
            g_mutation = &mutation;
            orderedRules[0] = rule;
            orderedRules[0].filters[0] = {kFilterSize, 64, 32};
            orderedRules[1] = secondRule;
            okay &= check(visitCensus(immediate, 'N', 3, orderedRules, 2, srvs, 0, 7, true, &eyeResult) &&
                              g_resourceAttempts == 1 && g_mutationAccepted &&
                              eyeResult.siteResult.outcome == SiteOutcome::Declined &&
                              eyeResult.censusSkippedAfter == 7 && readEyeCensus(eyeResult, &eyeFact) &&
                              eyeRead(eyeFact.skipCountGate, 2) && eyeRead(eyeFact.rules[0].loopCount, 2) &&
                              eyeRead(eyeFact.rules[0].filters[0].configuredWidth, 63) &&
                              eyeSkipped(eyeFact.rules[0].filters[0].height) &&
                              eyeRead(eyeFact.terminalLoopCount, 1) && eyeSkipped(eyeFact.rules[1].loopCount),
                          "COM reentry mutates exact width and next loop bound before next rule");
            resourceHook.uninstall();
            g_mutation = nullptr;
        }

        {
            edvr::VTableHook resourceHook;
            okay &= check(hookResource(resourceHook, srv), "rehook real SRV for fault parity");
            g_injectResourceFault = true;
            rule.filters[0] = {kFilterSize, 64, 32};
            okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4, true, &eyeResult) &&
                              g_resourceAttempts == 1 && eyeResult.censusSkippedAfter == 4 &&
                              eyeResult.siteResult.outcome == SiteOutcome::Declined &&
                              readEyeCensus(eyeResult, &eyeFact) &&
                              eyeRead(eyeFact.rules[0].filters[0].resolved, false) &&
                              eyeSkipped(eyeFact.rules[0].filters[0].isTexture2D),
                          "typed real-view fault records attempted query and known-false prefix");
            okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4, false, &eyeResult) &&
                              g_resourceAttempts == 2 && eyeResult.censusSkippedAfter == 4 &&
                              eyeResult.siteResult.outcome == SiteOutcome::Declined &&
                              edvr::draw_ladder_trace::eyeCensusFactCountForTest(eyeResult.token) == 0,
                          "NoTrace real-view fault has same query result and no observation");
            resourceHook.uninstall();
            g_injectResourceFault = false;
        }

        // One invalid view exercises the production guarded resolver's known-false prefix.
        srvs[0] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
        rule.filters[0] = {kFilterSize, 64, 32};
        okay &= check(visitCensus(immediate, 'N', 3, &rule, 1, srvs, 0, 4,
                                  true, &eyeResult) &&
                          eyeResult.siteResult.flow == Flow::Continue &&
                          readEyeCensus(eyeResult, &eyeFact) &&
                          eyeRead(eyeFact.rules[0].filters[0].resolved, false) &&
                          eyeSkipped(eyeFact.rules[0].filters[0].isTexture2D),
                      "guarded real resolver fault is a known-false result with later reads skipped");

        // Drive site 60 through the real visitor and helper. Hash lookups use
        // the production registry, populated with real WARP shaders.
        edvr::installExposureFix(device, edvr::HookMode::CopyVptr);
        const auto shaderCode = compileTestPixelShader();
        ComPtr<ID3D11PixelShader> resolveShader;
        ComPtr<ID3D11PixelShader> unknownShader;
        okay &= check(shaderCode && SUCCEEDED(device->CreatePixelShader(
                          shaderCode->GetBufferPointer(), shaderCode->GetBufferSize(),
                          nullptr, &resolveShader)) && resolveShader,
                      "WARP creates the real resolve-claim shader");
        okay &= check(shaderCode && SUCCEEDED(device->CreatePixelShader(
                          shaderCode->GetBufferPointer(), shaderCode->GetBufferSize(),
                          nullptr, &unknownShader)) && unknownShader,
                      "WARP creates an unregistered fallback shader");
        if (resolveShader && unknownShader) {
            edvr::registerShaderHash(resolveShader.Get(), edvr::detail::kResolveBindPs);
            okay &= check(edvr::lookupShaderHash(resolveShader.Get()) ==
                              edvr::detail::kResolveBindPs,
                          "production shader registry returns the registered WARP hash");
            edvr::Config& config = edvr::Config::get();
            const std::string previousScannerBody = config.getString("fix.scanner_body", "on");
            void* const previousPsShadow = edvr::bindingGet(edvr::BindSlot::Ps);
            const std::uint64_t previousPsHash =
                edvr::bindingShaderHash(edvr::BindSlot::Ps);
            ID3D11PixelShader* previousActualPs = nullptr;
            immediate->PSGetShader(&previousActualPs, nullptr, nullptr);
            config.set("fix.scanner_body", "off");
            edvr::resolveBindConfigure(config);
            edvr::bindingSetShader(edvr::BindSlot::Ps, nullptr, 0);
            immediate->PSSetShader(nullptr, nullptr, 0);

            edvr::VScreenPredicateTestResult resolveResult{};
            edvr::ResolveBindObservation resolveFact{};
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              resolveResult.siteResult.flow == Flow::Continue &&
                              resolveResult.siteResult.outcome == SiteOutcome::Declined &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.outer.wants, false) &&
                              rbSkipped(resolveFact.outer.psPresent) &&
                              rbSkipped(resolveFact.helper.wants),
                          "fix-off claim records only the consumed outer gate");

            config.set("fix.scanner_body", "on");
            edvr::resolveBindConfigure(config);
            edvr::VTableHook psGetHook;
            okay &= check(hookGetPixelShader(psGetHook, immediate),
                          "typed context hook counts real PSGetShader calls");
            immediate->PSSetShader(unknownShader.Get(), nullptr, 0);
            edvr::bindingSetShader(edvr::BindSlot::Ps, unknownShader.Get(), 1);
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 0 &&
                              resolveResult.siteResult.outcome == SiteOutcome::Declined &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.outer.psPresent, true) &&
                              rbRead(resolveFact.outer.psHash, 1) &&
                              rbSkipped(resolveFact.helper.wants),
                          "known non-resolve shadow skips helper and COM fallback");

            immediate->PSSetShader(resolveShader.Get(), nullptr, 0);
            edvr::bindingSetShader(edvr::BindSlot::Ps, resolveShader.Get(),
                                   edvr::detail::kResolveBindPs);
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 0 &&
                              resolveResult.siteResult.flow == Flow::Stop &&
                              resolveResult.siteResult.outcome == SiteOutcome::Claimed &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.wants, true) &&
                              rbRead(resolveFact.helper.psHash, edvr::detail::kResolveBindPs) &&
                              rbSkipped(resolveFact.helper.lambdaEntered),
                          "known resolve hash claims through the actual site without getter");
            okay &= check(visitResolveBind(immediate, false, &resolveResult) &&
                              resolveResult.siteResult.outcome == SiteOutcome::Claimed &&
                              edvr::draw_ladder_trace::resolveBindFactCountForTest(
                                  resolveResult.token) == 0,
                          "NoTrace specialization makes the same cached claim without a fact");

            immediate->PSSetShader(nullptr, nullptr, 0);
            edvr::bindingSetShader(edvr::BindSlot::Ps, nullptr, 0);
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 1 &&
                              resolveResult.siteResult.outcome == SiteOutcome::Declined &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.lambdaEntered, true) &&
                              rbRead(resolveFact.helper.psGetReached, true) &&
                              rbRead(resolveFact.helper.psGetCompleted, true) &&
                              rbRead(resolveFact.helper.shaderNonNull, false) &&
                              rbSkipped(resolveFact.helper.lookupReached) &&
                              rbRead(resolveFact.helper.guardReturned, true),
                          "unknown null binding completes real PSGetShader and declines");

            immediate->PSSetShader(unknownShader.Get(), nullptr, 0);
            edvr::bindingSetShader(edvr::BindSlot::Ps, unknownShader.Get(), 0);
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 2 &&
                              resolveResult.siteResult.outcome == SiteOutcome::Declined &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.shaderNonNull, true) &&
                              rbRead(resolveFact.helper.lookupCompleted, true) &&
                              rbRead(resolveFact.helper.lookupHash, 0) &&
                              rbRead(resolveFact.helper.releaseCompleted, true) &&
                              rbSkipped(resolveFact.helper.cacheBeforePresent),
                          "unregistered real shader has completed zero lookup and no cache repair");

            // The zero-shadow fallback learns the registered shader through
            // PSGetShader, updates the actual shadow, and claims this draw.
            immediate->PSSetShader(resolveShader.Get(), nullptr, 0);
            edvr::bindingSetShader(edvr::BindSlot::Ps, resolveShader.Get(), 0);
            g_getPixelShaderCalls = 0;
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 1 &&
                              resolveResult.siteResult.outcome == SiteOutcome::Claimed &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.psGetCompleted, true) &&
                              rbRead(resolveFact.helper.lookupHash,
                                     edvr::detail::kResolveBindPs) &&
                              rbRead(resolveFact.helper.cacheBeforePresent, true) &&
                              rbRead(resolveFact.helper.cacheBeforeHash, 0) &&
                              rbRead(resolveFact.helper.cacheAfterPresent, true) &&
                              rbRead(resolveFact.helper.cacheAfterHash,
                                     edvr::detail::kResolveBindPs),
                          "real fallback lookup repairs the shadow and claims");
            edvr::bindingSetShader(edvr::BindSlot::Ps, resolveShader.Get(), 0);
            g_getPixelShaderCalls = 0;
            g_faultGetPixelShader = true;
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 1 &&
                              resolveResult.siteResult.outcome == SiteOutcome::Declined &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.lambdaEntered, true) &&
                              rbRead(resolveFact.helper.psGetReached, true) &&
                              rbSkipped(resolveFact.helper.psGetCompleted) &&
                              rbRead(resolveFact.helper.guardReturned, false),
                          "typed PSGetShader fault records its attempted prefix");
            g_faultGetPixelShader = false;
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 2 &&
                              resolveResult.siteResult.outcome == SiteOutcome::Claimed &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.lookupHash,
                                     edvr::detail::kResolveBindPs),
                          "claim recovers after the real getter fault");

            // Make the shadow unknown again. The helper assigns its positive
            // match before Release, so this real post-query fault preserves it.
            edvr::bindingSetShader(edvr::BindSlot::Ps, resolveShader.Get(), 0);
            edvr::VTableHook releaseHook;
            void* releaseOriginal = nullptr;
            okay &= check(releaseHook.attach(resolveShader.Get(), 16) &&
                              releaseHook.setMode(edvr::HookMode::CopyVptr) &&
                              releaseHook.replace(2,
                                  reinterpret_cast<void*>(&testShaderRelease),
                                  &releaseOriginal) && releaseHook.commit(),
                          "typed shader Release hook injects the post-match fault");
            g_realShaderRelease = reinterpret_cast<ReleaseFn>(releaseOriginal);
            g_releaseCalls = 0;
            g_getPixelShaderCalls = 0;
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              g_getPixelShaderCalls == 1 &&
                              g_releaseCalls == 1 &&
                              resolveResult.siteResult.outcome == SiteOutcome::Claimed &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.lookupHash,
                                     edvr::detail::kResolveBindPs) &&
                              rbRead(resolveFact.helper.releaseReached, true) &&
                              rbSkipped(resolveFact.helper.releaseCompleted) &&
                              rbRead(resolveFact.helper.guardReturned, false),
                          "Release fault after lookup preserves the positive claim");
            releaseHook.uninstall();
            g_realShaderRelease = nullptr;
            okay &= check(visitResolveBind(immediate, true, &resolveResult) &&
                              resolveResult.siteResult.outcome == SiteOutcome::Claimed &&
                              readResolveBind(resolveResult, &resolveFact) &&
                              rbRead(resolveFact.helper.wants, true) &&
                              rbSkipped(resolveFact.helper.lambdaEntered),
                          "repaired positive shadow recovers after Release fault");

            // A non-null invalid COM context consumes the remaining helper
            // fault budget. The first six calls enter the guarded callback;
            // the next is denied before PSGetShader.
            auto* const invalidContext = reinterpret_cast<ID3D11DeviceContext*>(
                static_cast<std::uintptr_t>(1));
            edvr::bindingSetShader(edvr::BindSlot::Ps, nullptr, 0);
            for (int i = 0; i < 7; ++i) {
                const bool invoked = visitResolveBind(invalidContext, true, &resolveResult);
                bool haveFact = invoked && readResolveBind(resolveResult, &resolveFact);
                if (i < 6) {
                    okay &= check(haveFact &&
                                      rbRead(resolveFact.helper.lambdaEntered, true) &&
                                      rbRead(resolveFact.helper.psGetReached, true) &&
                                      rbSkipped(resolveFact.helper.psGetCompleted) &&
                                      rbRead(resolveFact.helper.guardReturned, false),
                                  "invalid typed context faults within admitted budget");
                } else {
                    okay &= check(haveFact &&
                                      rbRead(resolveFact.helper.lambdaEntered, false) &&
                                      rbSkipped(resolveFact.helper.psGetReached) &&
                                      rbRead(resolveFact.helper.guardReturned, false),
                                  "exhausted helper budget records a denied callback");
                }
            }
            psGetHook.uninstall();
            immediate->PSSetShader(previousActualPs, nullptr, 0);
            edvr::bindingSetShader(edvr::BindSlot::Ps, previousPsShadow, previousPsHash);
            if (previousActualPs) previousActualPs->Release();
            config.set("fix.scanner_body", previousScannerBody.c_str());
            edvr::resolveBindConfigure(config);
            edvr::bindingSetShader(edvr::BindSlot::Ps, nullptr, 0);
        }

        for (std::uint32_t i = 0; i < 4; ++i)
            edvr::bindingSet(static_cast<edvr::BindSlot>(
                                 static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + i), nullptr);
        edvr::bindingSetShader(edvr::BindSlot::Vs, nullptr, 0);

        if (srv) srv->Release();
        if (bufferSrv) bufferSrv->Release();
        if (buffer) buffer->Release();
        if (texture) texture->Release();
    }

    if (deferred) deferred->Release();
    if (immediate) immediate->Release();
    if (device) device->Release();
    edvr::draw_ladder_trace::shutdown();
    DeleteFileW(logPath.c_str());
    std::puts(okay ? "vscreen_predicate_test: PASS" : "vscreen_predicate_test: FAILED");
    return okay ? 0 : 1;
}
