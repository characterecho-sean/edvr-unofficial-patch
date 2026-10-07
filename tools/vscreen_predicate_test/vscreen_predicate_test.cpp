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
#include "../../src/common/plugin_cost.h"
#include "../../src/d3d11/vscreen.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/resolve_bind_fix.h"
#include "../../src/d3d11/exposure_fix.h"
#include "../../src/d3d11/basic_draw_observation.h"
#include "../../src/d3d11/eye_census_observation.h"
#include "../../src/d3d11/loader_panel_observation.h"
#include "../../src/d3d11/loader_panel.h"
#include "../../src/d3d11/fss_dump.h"
#include "../../src/d3d11/target_sharp.h"
#include "../../src/d3d11/sunglare_fix.h"
#include "../../src/d3d11/sunglare_nomination_observation.h"
#include "../../src/d3d11/plugin_registry.h"
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

bool readFssDump(const edvr::VScreenPredicateTestResult& result,
                 edvr::FssDumpObservation* fact) {
    return edvr::draw_ladder_trace::fssDumpFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readFssDumpFactForTest(result.token, 0, fact);
}

bool readTargetSharp(const edvr::VScreenPredicateTestResult& result,
                     edvr::TargetSharpObservation* fact) {
    return edvr::draw_ladder_trace::targetSharpFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readTargetSharpFactForTest(result.token, 0, fact);
}

bool readSunglareNomination(const edvr::VScreenPredicateTestResult& result,
                            edvr::SunglareNominationObservation* fact) {
    return edvr::draw_ladder_trace::sunglareNominationFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readSunglareNominationFactForTest(
               result.token, 0, fact);
}

bool readForwarding(const edvr::VScreenForwardingTestResult& result,
                    edvr::ForwardingObservation* fact) {
    return edvr::draw_ladder_trace::forwardingFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readForwardingFactForTest(result.token, 0, fact);
}

template <class T, class U>
bool fwRead(const edvr::BasicDrawRead<T>& read, U expected) {
    return read.reached && read.known && read.value == static_cast<T>(expected);
}

template <class T>
bool fwSkipped(const edvr::BasicDrawRead<T>& read) {
    return !read.reached && !read.known && read.value == T{};
}

bool checkForwardAction(const edvr::VScreenForwardingTestResult& result,
                        const edvr::VScreenForwardingTestInput& input,
                        std::uint16_t expectedId,
                        edvr::draw_ladder::ActionOutcome expectedOutcome,
                        std::uint16_t expectedIssueCount) {
    using namespace edvr::draw_ladder;
    if (edvr::draw_ladder_trace::actionCountForTest(result.token) != 1) return false;
    std::uint16_t actionId = 0;
    ActionRecord actual{};
    if (!edvr::draw_ladder_trace::readActionForTest(result.token, 0, &actionId, &actual)) return false;
    DrawCallKind call{};
    switch (input.kind) {
    case 'D': call = DrawCallKind::Draw; break;
    case 'I': call = DrawCallKind::DrawIndexed; break;
    case 'N': call = DrawCallKind::DrawInstanced; break;
    case 'X': call = DrawCallKind::DrawIndexedInstanced; break;
    default: return false;
    }
    std::uint32_t start = input.args.start;
    std::int32_t base = input.args.base;
    if (input.kind == 'D' || input.kind == 'N') {
        start = static_cast<std::uint32_t>(input.args.base);
        base = 0;
    }
    return actionId == expectedId && actual.phase == ActionPhase::Issue &&
        actual.outcome == expectedOutcome && actual.call == call && actual.flags == 0 &&
        actual.issueCount == expectedIssueCount && actual.count == input.count &&
        actual.instances == input.instances && actual.start == start &&
        actual.startInstance == input.args.startInstance && actual.baseVertex == base;
}

template <class T, class U>
bool fdRead(const edvr::FssDumpRead<T>& read, U expected) {
    return read.reached && read.known && read.value == static_cast<T>(expected);
}

template <class T>
bool fdSkipped(const edvr::FssDumpRead<T>& read) {
    return !read.reached && !read.known && read.value == T{};
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

ComPtr<ID3DBlob> compileTestVertexShader(const char* source, const char* name) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), name,
        nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
        &code, &errors);
    if (FAILED(hr) && errors)
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    return SUCCEEDED(hr) ? code : ComPtr<ID3DBlob>{};
}

using GetVertexShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11VertexShader**, ID3D11ClassInstance**, UINT*);
GetVertexShaderFn g_realGetVertexShader = nullptr;
std::uint32_t g_getVertexShaderCalls = 0;
bool g_faultGetVertexShader = false;
void STDMETHODCALLTYPE testGetVertexShader(ID3D11DeviceContext* self,
    ID3D11VertexShader** shader, ID3D11ClassInstance** instances, UINT* count) {
    ++g_getVertexShaderCalls;
    if (g_faultGetVertexShader)
        RaiseException(0xE042ED94u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    g_realGetVertexShader(self, shader, instances, count);
}

using VertexReleaseFn = ULONG(STDMETHODCALLTYPE*)(ID3D11VertexShader*);
VertexReleaseFn g_realVertexRelease = nullptr;
std::uint32_t g_vertexReleaseCalls = 0;
ULONG STDMETHODCALLTYPE testVertexRelease(ID3D11VertexShader* self) {
    ++g_vertexReleaseCalls;
    const ULONG refs = g_realVertexRelease(self);
    RaiseException(0xE042ED95u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return refs;
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

using ResourceGetTypeFn = void(STDMETHODCALLTYPE*)(ID3D11Resource*,
                                                   D3D11_RESOURCE_DIMENSION*);
using BufferGetDescFn = void(STDMETHODCALLTYPE*)(ID3D11Buffer*, D3D11_BUFFER_DESC*);
using TextureGetDescFn = void(STDMETHODCALLTYPE*)(ID3D11Texture2D*, D3D11_TEXTURE2D_DESC*);
ResourceGetTypeFn g_realResourceGetType = nullptr;
BufferGetDescFn g_realBufferGetDesc = nullptr;
TextureGetDescFn g_realTextureGetDesc = nullptr;
std::uint32_t g_nominationGetTypeCalls = 0;
std::uint32_t g_nominationGetDescCalls = 0;
bool g_nominationTypeFault = false;

void STDMETHODCALLTYPE nominationGetType(ID3D11Resource* self,
                                           D3D11_RESOURCE_DIMENSION* dimension) {
    ++g_nominationGetTypeCalls;
    if (g_nominationTypeFault)
        RaiseException(0xE042ED96u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    g_realResourceGetType(self, dimension);
}

void STDMETHODCALLTYPE nominationGetBufferDesc(ID3D11Buffer* self,
                                                D3D11_BUFFER_DESC* desc) {
    ++g_nominationGetDescCalls;
    g_realBufferGetDesc(self, desc);
}

void STDMETHODCALLTYPE nominationGetTextureDesc(ID3D11Texture2D* self,
                                                  D3D11_TEXTURE2D_DESC* desc) {
    ++g_nominationGetDescCalls;
    g_realTextureGetDesc(self, desc);
}

bool hookNominationResource(edvr::VTableHook& hook, ID3D11Resource* resource,
                            bool texture) {
    g_nominationGetTypeCalls = 0;
    g_nominationGetDescCalls = 0;
    // ID3D11Resource contributes GetType(7), SetEvictionPriority(8) and
    // GetEvictionPriority(9); Buffer/Texture2D GetDesc is therefore slot 10.
    if (!hook.attach(resource, 11) || !hook.setMode(edvr::HookMode::CopyVptr) ||
        !hook.replace(7, reinterpret_cast<void*>(&nominationGetType),
                      reinterpret_cast<void**>(&g_realResourceGetType)))
        return false;
    if (texture) {
        if (!hook.replace(10, reinterpret_cast<void*>(&nominationGetTextureDesc),
                          reinterpret_cast<void**>(&g_realTextureGetDesc)))
            return false;
    } else if (!hook.replace(10, reinterpret_cast<void*>(&nominationGetBufferDesc),
                             reinterpret_cast<void**>(&g_realBufferGetDesc))) {
        return false;
    }
    return hook.commit();
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

bool checkRead(const edvr::BasicDrawRead<std::int32_t>& read,
               std::int32_t expected) {
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

bool testFssDumpPredicate(ID3D11Device* device, ID3D11DeviceContext* context) {
    constexpr std::uint64_t ringHash = 0x7E38A6AA1269C901ull;
    constexpr std::uint64_t compositeHash = 0x953C8123AD8DC13Bull;
    constexpr std::uint64_t tonemapHash = 0x2D78DC3FD2C0C543ull;
    constexpr char ringSource[] =
        "float4 main(uint id:SV_VertexID):SV_Position {"
        "return float4((id==0)?-1:1,(id==1)?1:-1,0,1);}";
    constexpr char compositeSource[] =
        "float4 main(uint id:SV_VertexID):SV_Position {"
        "return float4((id==0)?-1:1,(id==1)?1:-1,0.125,1);}";
    constexpr char tonemapSource[] =
        "float4 main(uint id:SV_VertexID):SV_Position {"
        "return float4((id==0)?-1:1,(id==1)?1:-1,0.25,1);}";
    const auto code = compileTestVertexShader(ringSource, "fss-dump-ring");
    const auto compositeCode = compileTestVertexShader(compositeSource, "fss-dump-composite");
    const auto tonemapCode = compileTestVertexShader(tonemapSource, "fss-dump-tonemap");
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11VertexShader> compositeShader;
    ComPtr<ID3D11VertexShader> tonemapShader;
    bool okay = check(code && SUCCEEDED(device->CreateVertexShader(
                           code->GetBufferPointer(), code->GetBufferSize(),
                           nullptr, &shader)) && shader,
                     "WARP creates registered FSS dump vertex shader");
    if (!shader) return false;
    okay &= check(compositeCode && tonemapCode &&
                      SUCCEEDED(device->CreateVertexShader(
                         compositeCode->GetBufferPointer(), compositeCode->GetBufferSize(), nullptr,
                         &compositeShader)) && compositeShader &&
                      SUCCEEDED(device->CreateVertexShader(
                         tonemapCode->GetBufferPointer(), tonemapCode->GetBufferSize(), nullptr,
                         &tonemapShader)) && tonemapShader,
                  "WARP creates real composite and tonemap FSS shaders");
    edvr::registerShaderHash(shader.Get(), ringHash);
    if (compositeShader) edvr::registerShaderHash(compositeShader.Get(), compositeHash);
    if (tonemapShader) edvr::registerShaderHash(tonemapShader.Get(), tonemapHash);
    context->VSSetShader(shader.Get(), nullptr, 0);
    okay &= check(edvr::lookupShaderHash(shader.Get()) == ringHash,
                  "production registry resolves the WARP FSS dump shader");

    auto saved = edvr::fssDumpPredicateTestState();
    const auto savedInterestMask = edvr::pluginRegistryDrawInterestMask();
    edvr::FssDumpPredicateTestState seed{};
    seed.frame = 1;
    seed.pendingKind = 8;
    seed.pendingEye = 9;
    const auto mask = edvr::draw_interest::bit(
        edvr::draw_interest::InterestId::FssDump);
    edvr::pluginRegistryConfigureDrawInterests(mask, nullptr, 0);
    edvr::VScreenPredicateTestResult result{};
    edvr::FssDumpObservation fact{};

    // uint32 frame subtraction and the repeated wants groups are source inputs.
    edvr::fssDumpPredicateTestSetState(seed);
    okay &= check(edvr::vScreenFssDumpPredicateTestVisit(
                      context, 'N', 4, 1, 0, UINT32_MAX - 1, true, &result) &&
                      result.siteResult.outcome == edvr::draw_ladder::SiteOutcome::Declined &&
                      readFssDump(result, &fact) && fact.handlerInvoked &&
                      fdRead(fact.outer.frameNo, 0u) &&
                      fdRead(fact.helper.lookupHash, ringHash) &&
                      fdRead(fact.helper.counters.ringBefore, 0u) &&
                      fdRead(fact.helper.counters.ringAfter, 1u) &&
                      fdRead(fact.helper.dumping, false),
                  "actual WARP visitor preserves age wrap and non-dump counter increment");
    edvr::FssDumpPredicateTestState inactive{};
    edvr::fssDumpPredicateTestSetState(inactive);
    okay &= check(edvr::vScreenFssDumpPredicateTestVisit(
                      context, 'N', 4, 1, 12, 10, true, &result) &&
                      readFssDump(result, &fact) &&
                      fdRead(fact.outer.wants.frame, 0u) &&
                      fdRead(fact.outer.wants.seriesWant, 0u) &&
                      fdSkipped(fact.outer.wants.done) &&
                      fdSkipped(fact.outer.bodyFrame) &&
                      fdSkipped(fact.helper.wants.frame),
                  "site 59 preserves short-circuit outer gate and skips helper wants");
    edvr::fssDumpPredicateTestSetState(seed);
    okay &= check(edvr::vScreenFssDumpPredicateTestVisit(
                      nullptr, 'N', 4, 1, 12, 10, true, &result) &&
                      readFssDump(result, &fact) &&
                      fdRead(fact.helper.contextNonNull, false) &&
                      fdSkipped(fact.helper.guardCallReached),
                  "null context stops before shader query");
    edvr::pluginRegistryConfigureDrawInterests(0, nullptr, 0);
    okay &= check(edvr::vScreenFssDumpPredicateTestVisit(
                      context, 'N', 4, 1, 12, 10, true, &result) &&
                      result.siteResult.outcome == edvr::draw_ladder::SiteOutcome::NotEligible &&
                      readFssDump(result, &fact) && !fact.handlerInvoked &&
                      fdSkipped(fact.outer.wants.frame) &&
                      fdSkipped(fact.helper.wants.frame),
                  "uninterested site emits one source-read-free fact");
    edvr::pluginRegistryConfigureDrawInterests(mask, nullptr, 0);

    // Each comparison starts from the identical scalar state and real WARP VS.
    const auto compare = [&](ID3D11VertexShader* currentShader,
                             std::uint32_t vertices,
                             edvr::FssDumpPredicateTestState current,
                             bool expectedClaim, const char* label) {
        context->VSSetShader(currentShader, nullptr, 0);
        edvr::fssDumpPredicateTestSetState(current);
        edvr::VScreenPredicateTestResult traced{};
        edvr::FssDumpObservation observed{};
        const bool a = edvr::vScreenFssDumpPredicateTestVisit(
            context, 'N', vertices, 1, 12, 10, true, &traced) && readFssDump(traced, &observed);
        const auto afterA = edvr::fssDumpPredicateTestState();
        edvr::fssDumpPredicateTestSetState(current);
        edvr::VScreenPredicateTestResult plain{};
        const bool b = edvr::vScreenFssDumpPredicateTestVisit(
            context, 'N', vertices, 1, 12, 10, false, &plain);
        const auto afterB = edvr::fssDumpPredicateTestState();
        okay &= check(a && b &&
                          ((traced.siteResult.outcome == edvr::draw_ladder::SiteOutcome::Claimed) == expectedClaim) &&
                          traced.siteResult.outcome == plain.siteResult.outcome &&
                          afterA.frame == afterB.frame && afterA.done == afterB.done &&
                          afterA.seriesWant == afterB.seriesWant &&
                          afterA.seriesDone == afterB.seriesDone &&
                          afterA.ring == afterB.ring &&
                          afterA.composite == afterB.composite &&
                          afterA.tonemap == afterB.tonemap &&
                          afterA.pendingKind == afterB.pendingKind &&
                          afterA.pendingEye == afterB.pendingEye &&
                          afterA.dumping == afterB.dumping &&
                          edvr::draw_ladder_trace::fssDumpFactCountForTest(plain.token) == 0,
                      label);
    };
    seed.dumping = true;
    compare(shader.Get(), 4, seed, true,
            "trace/no-trace positive ring claim and pending writes agree");
    seed.ring = 254;
    compare(shader.Get(), 4, seed, false,
            "trace/no-trace occurrence 255 declines identically");
    seed.ring = 255;
    compare(shader.Get(), 4, seed, true,
            "trace/no-trace uint8 wrap claim agrees identically");
    edvr::fssDumpPredicateTestSetState(seed);
    okay &= check(edvr::vScreenFssDumpPredicateTestVisit(
                      context, 'N', 4, 1, 12, 10, true, &result) &&
                      readFssDump(result, &fact) &&
                      fdRead(fact.helper.counters.ringAfter, 0u) &&
                      fdRead(fact.helper.pendingEyeAfter, UINT32_MAX),
                  "wrapped occurrence zero preserves the UINT32_MAX pending-eye value");
    seed.ring = 0;
    seed.composite = 0;
    compare(compositeShader.Get(), 6, seed, true,
            "trace/no-trace composite family claim agrees");
    seed.composite = 0;
    seed.tonemap = 0;
    compare(tonemapShader.Get(), 3, seed, true,
            "trace/no-trace tonemap family claim agrees");
    seed.ring = 0;
    seed.dumping = false;
    compare(shader.Get(), 4, seed, false,
            "trace/no-trace non-dump counter increment agrees");
    context->VSSetShader(shader.Get(), nullptr, 0);

    context->VSSetShader(nullptr, nullptr, 0);
    seed.dumping = false;
    seed.ring = 0;
    edvr::fssDumpPredicateTestSetState(seed);
    okay &= check(edvr::vScreenFssDumpPredicateTestVisit(
                      context, 'N', 4, 1, 12, 10, true, &result) &&
                      result.siteResult.outcome == edvr::draw_ladder::SiteOutcome::Declined &&
                      readFssDump(result, &fact) &&
                      fdRead(fact.helper.shaderNonNull, false) &&
                      fdRead(fact.helper.lookupReached, true) &&
                      fdRead(fact.helper.lookupCompleted, true) &&
                      fdRead(fact.helper.lookupHash, 0ull) &&
                      fdSkipped(fact.helper.releaseReached) &&
                      fdRead(fact.helper.guardReturned, true),
                  "successful null VS getter still performs the raw zero-hash lookup");
    context->VSSetShader(shader.Get(), nullptr, 0);

    // Fault-budget state is deliberately not seeded. The post-hash Release
    // fault consumes one production slot; seven real query faults consume the
    // rest before the next call skips its lambda.
    edvr::VTableHook releaseHook;
    void* releaseOriginal = nullptr;
    okay &= check(releaseHook.attach(shader.Get(), 16) &&
                      releaseHook.setMode(edvr::HookMode::CopyVptr) &&
                      releaseHook.replace(2,
                          reinterpret_cast<void*>(&testVertexRelease),
                          &releaseOriginal) && releaseHook.commit(),
                  "typed WARP vertex shader Release hook installs");
    g_realVertexRelease = reinterpret_cast<VertexReleaseFn>(releaseOriginal);
    g_vertexReleaseCalls = 0;
    seed.dumping = true;
    seed.ring = 0;
    edvr::fssDumpPredicateTestSetState(seed);
    okay &= check(edvr::vScreenFssDumpPredicateTestVisit(
                      context, 'N', 4, 1, 12, 10, true, &result) &&
                      g_vertexReleaseCalls == 1 &&
                      result.siteResult.outcome == edvr::draw_ladder::SiteOutcome::Claimed &&
                      readFssDump(result, &fact) &&
                      fdRead(fact.helper.lookupHash, ringHash) &&
                      fdRead(fact.helper.releaseCompleted, false) &&
                      fdRead(fact.helper.hashAfterGuard, ringHash) &&
                      fdRead(fact.helper.guardReturned, false),
                  "Release fault after real hash lookup preserves site 59 claim");
    releaseHook.uninstall();
    g_realVertexRelease = nullptr;

    edvr::VTableHook hook;
    void* original = nullptr;
    okay &= check(hook.attach(context, 128) &&
                      hook.setMode(edvr::HookMode::CopyVptr) &&
                      hook.replace(76, reinterpret_cast<void*>(&testGetVertexShader),
                                   &original) && hook.commit(),
                  "typed WARP VSGetShader hook installs at verified slot 76");
    g_realGetVertexShader = reinterpret_cast<GetVertexShaderFn>(original);
    g_faultGetVertexShader = true;
    for (int i = 0; i < 8; ++i) {
        edvr::fssDumpPredicateTestSetState(seed);
        const bool visited = edvr::vScreenFssDumpPredicateTestVisit(
            context, 'N', 4, 1, 12, 10, true, &result);
        const bool have = visited && readFssDump(result, &fact);
        if (i < 7) {
            okay &= check(have && fdRead(fact.helper.callbackEntered, true) &&
                              fdRead(fact.helper.vsGetShaderCompleted, false) &&
                              fdRead(fact.helper.guardReturned, false),
                          "actual getter fault records attempted callback prefix");
        } else {
            okay &= check(have && fdRead(fact.helper.callbackEntered, false) &&
                              fdSkipped(fact.helper.vsGetShaderReached) &&
                              fdRead(fact.helper.guardReturned, false),
                          "eighth query records denied callback after eight combined faults");
        }
    }
    g_faultGetVertexShader = false;
    hook.uninstall();
    g_realGetVertexShader = nullptr;
    edvr::fssDumpPredicateTestSetState(saved);
    edvr::pluginRegistryConfigureDrawInterests(savedInterestMask, nullptr, 0);
    context->VSSetShader(nullptr, nullptr, 0);
    return okay;
}

bool testSunglareNomination(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace edvr;
    bool okay = true;
    const int savedWorld = detail::g_sunglareWorld;
    const auto savedTypeCalls = g_nominationGetTypeCalls;
    const auto savedDescCalls = g_nominationGetDescCalls;
    const bool savedTypeFault = g_nominationTypeFault;
    const auto savedGetType = g_realResourceGetType;
    const auto savedBufferDesc = g_realBufferGetDesc;
    const auto savedTextureDesc = g_realTextureGetDesc;
    void* const savedTarget = sunglareSceneCbTargetRaw();
    void* const savedShadow = bindingGet(BindSlot::VsCb0);
    ID3D11Buffer* savedActual = nullptr;
    context->VSGetConstantBuffers(0, 1, &savedActual);
    const auto visit = [&](ID3D11Buffer* buffer, std::uint32_t count,
                           void* nominatedBefore, bool traced,
                           VScreenPredicateTestResult* result) {
        context->VSSetConstantBuffers(0, 1, &buffer);
        bindingSet(BindSlot::VsCb0, buffer);
        return vScreenSunglareNominationPredicateTestVisit(
            context, 'X', count, nominatedBefore, traced, result);
    };
    const auto makeBuffer = [&](UINT bytes, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(device->CreateBuffer(&desc, nullptr, &out)) && out;
    };
    ComPtr<ID3D11Buffer> cb208, cb192, cb224;
    okay &= check(makeBuffer(208, cb208), "WARP creates a 208-byte scene constant buffer");
    okay &= check(makeBuffer(192, cb192), "WARP creates a 192-byte negative constant buffer");
    okay &= check(makeBuffer(224, cb224), "WARP creates a 224-byte negative constant buffer");
    if (cb208 && cb192 && cb224) {
        VTableHook hook208, hook192, hook224;
        const bool hooked208 = hookNominationResource(
            hook208, static_cast<ID3D11Resource*>(cb208.Get()), false);
        const bool hooked192 = hookNominationResource(
            hook192, static_cast<ID3D11Resource*>(cb192.Get()), false);
        const bool hooked224 = hookNominationResource(
            hook224, static_cast<ID3D11Resource*>(cb224.Get()), false);
        okay &= check(hooked208 && hooked192 && hooked224,
                      "WARP hooks actual CB GetType/GetDesc resolver calls");
        const auto runPair = [&](ID3D11Buffer* buffer, std::uint32_t count,
                                 int world, void* nominated,
                                 bool expectedCallback, std::uint32_t expectedBytes,
                                 const char* label) {
            detail::g_sunglareWorld = world;
            void* const callbackBefore = sunglareSceneCbTargetRaw();
            VScreenPredicateTestResult traced{}, plain{};
            SunglareNominationObservation fact{};
            g_nominationGetTypeCalls = g_nominationGetDescCalls = 0;
            const bool tracedOk = visit(buffer, count, nominated, true, &traced) &&
                                  readSunglareNomination(traced, &fact);
            const auto tracedTypeCalls = g_nominationGetTypeCalls;
            const auto tracedDescCalls = g_nominationGetDescCalls;
            void* const callbackAfterTrace = sunglareSceneCbTargetRaw();
            sunglareSceneCb(callbackBefore);
            g_nominationGetTypeCalls = g_nominationGetDescCalls = 0;
            const bool plainOk = visit(buffer, count, nominated, false, &plain);
            const auto plainTypeCalls = g_nominationGetTypeCalls;
            const auto plainDescCalls = g_nominationGetDescCalls;
            void* const callbackAfterPlain = sunglareSceneCbTargetRaw();
            const std::uint32_t expectedResolverCalls =
                world && count > 10000 && buffer && buffer != nominated ? 1u : 0u;
            const bool pairOkay = tracedOk && plainOk &&
                              traced.siteResult.outcome == plain.siteResult.outcome &&
                              traced.siteResult.outcome == draw_ladder::SiteOutcome::Observed &&
                              fact.siteId == 45 && fact.kind == 22 &&
                              checkRead(fact.worldMode, static_cast<std::int32_t>(world)) &&
                              checkRead(fact.callbackInvoked, expectedCallback) &&
                              tracedTypeCalls == expectedResolverCalls &&
                              plainTypeCalls == expectedResolverCalls &&
                              tracedDescCalls == (expectedBytes ? 1u : 0u) &&
                              plainDescCalls == (expectedBytes ? 1u : 0u) &&
                              callbackAfterTrace == (expectedCallback ? buffer : callbackBefore) &&
                              callbackAfterPlain == (expectedCallback ? buffer : callbackBefore) &&
                              traced.sceneCbNominatedAfter ==
                                  (expectedCallback ? buffer : nominated) &&
                              plain.sceneCbNominatedAfter ==
                                  (expectedCallback ? buffer : nominated) &&
                              draw_ladder_trace::sunglareNominationFactCountForTest(
                                  plain.token) == 0;
            okay &= check(pairOkay, label);
            if (!pairOkay) {
                std::fprintf(stderr,
                    "nomination query prefix: trace=%d plain=%d; GetType=%u/%u expected=%u; GetDesc=%u/%u expected=%u\n",
                    tracedOk, plainOk, tracedTypeCalls, plainTypeCalls,
                    expectedResolverCalls, tracedDescCalls, plainDescCalls,
                    expectedBytes ? 1u : 0u);
                std::fprintf(stderr,
                    "nomination mutation: nominated=%p/%p expected=%p; target=%p/%p expected=%p\n",
                    traced.sceneCbNominatedAfter, plain.sceneCbNominatedAfter,
                    expectedCallback ? static_cast<void*>(buffer) : nominated,
                    callbackAfterTrace, callbackAfterPlain,
                    expectedCallback ? static_cast<void*>(buffer) : callbackBefore);
            }
            if (world && count > 10000) {
                okay &= check(checkRead(fact.boundCbIdentity,
                                        reinterpret_cast<std::uintptr_t>(buffer)) &&
                                  bindingGet(BindSlot::VsCb0) == buffer,
                              "site 45 records the actual bound CB identity");
                if (buffer) {
                    okay &= check(checkRead(fact.nominatedBeforeIdentity,
                                            reinterpret_cast<std::uintptr_t>(nominated)),
                                  "macro-seeded nomination history is read only for a nonnull CB");
                } else {
                    okay &= check(!fact.nominatedBeforeIdentity.reached &&
                                      !fact.resourceResolved.reached &&
                                      !fact.isBuffer.reached && !fact.byteWidth.reached,
                                  "null CB stops before prior nomination and resolver reads");
                }
                if (expectedBytes) {
                    okay &= check(checkRead(fact.resourceResolved, true) &&
                                      checkRead(fact.isBuffer, true) &&
                                      checkRead(fact.byteWidth, expectedBytes),
                                  "site 45 records the resolver's actual buffer type and size");
                }
            }
            if (!world || count <= 10000) {
                okay &= check(!fact.boundCbIdentity.reached &&
                                  !fact.nominatedBeforeIdentity.reached &&
                                  !fact.resourceResolved.reached &&
                                  !fact.isBuffer.reached && !fact.byteWidth.reached,
                              "world/count prefix preserves downstream lazy reads");
            }
            if (buffer == nominated && world && count > 10000) {
                okay &= check(!fact.resourceResolved.reached && !fact.isBuffer.reached &&
                                  !fact.byteWidth.reached,
                              "same identity preserves resolver and size laziness");
            }
            if (expectedCallback) {
                okay &= check(checkRead(fact.nominatedAfterIdentity,
                                        reinterpret_cast<std::uintptr_t>(buffer)) &&
                                  checkRead(fact.callbackTargetAfterIdentity,
                                        reinterpret_cast<std::uintptr_t>(buffer)),
                              "successful nomination records actual callback mutation state");
            } else {
                okay &= check(!fact.nominatedAfterIdentity.reached &&
                                  !fact.callbackTargetAfterIdentity.reached,
                              "non-mutating paths leave callback mutation suffix unread");
            }
        };

        // The accepted path uses the actual bound WARP CB and production resolver.
        runPair(cb208.Get(), 10001, 1, nullptr, true, 208,
                "Trace and NoTrace agree on the actual 208-byte nomination");
        runPair(cb208.Get(), 10001, -1, nullptr, true, 208,
                "negative nonzero signed world mode still admits nomination");
        // Count/world gates, same-pointer identity and wrong sizes stop at their source prefix.
        runPair(cb208.Get(), 10000, 1, nullptr, false, 0,
                "count 10000 leaves the binding and resolver suffix lazy");
        runPair(cb208.Get(), 10001, 0, nullptr, false, 0,
                "world mode zero leaves all downstream source reads lazy");
        runPair(cb208.Get(), 10001, 1, cb208.Get(), false, 0,
                "already nominated CB skips resource resolution");
        runPair(nullptr, 10001, 1, nullptr, false, 0,
                "null actual CB records presence and skips nomination/resolution suffix");
        runPair(cb192.Get(), 10001, 1, nullptr, false, 192,
                "192-byte WARP CB resolves but does not nominate");
        runPair(cb224.Get(), 10001, 1, nullptr, false, 224,
                "224-byte WARP CB resolves but does not nominate");

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = textureDesc.Height = 32;
        textureDesc.MipLevels = textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> trackedTexture;
        const bool haveTrackedTexture = SUCCEEDED(device->CreateTexture2D(
            &textureDesc, nullptr, &trackedTexture)) && trackedTexture;
        okay &= check(haveTrackedTexture,
                      "WARP creates a texture for the manually seeded shadow-type negative");
        if (haveTrackedTexture) {
            VTableHook textureHook;
            const bool textureHooked = hookNominationResource(
                textureHook, static_cast<ID3D11Resource*>(trackedTexture.Get()), true);
            ID3D11Buffer* const nullBuffer = nullptr;
            context->VSSetConstantBuffers(0, 1, &nullBuffer);
            bindingSet(BindSlot::VsCb0, trackedTexture.Get());
            detail::g_sunglareWorld = 1;
            VScreenPredicateTestResult textureTrace{}, texturePlain{};
            SunglareNominationObservation textureFact{};
            const auto typeCalls = g_nominationGetTypeCalls;
            const auto descCalls = g_nominationGetDescCalls;
            g_nominationGetTypeCalls = g_nominationGetDescCalls = 0;
            const bool textureTraceOk = vScreenSunglareNominationPredicateTestVisit(
                context, 'X', 10001, nullptr, true, &textureTrace) &&
                readSunglareNomination(textureTrace, &textureFact);
            const auto traceTypeCalls = g_nominationGetTypeCalls;
            const auto traceDescCalls = g_nominationGetDescCalls;
            void* const textureTargetBeforePlain = sunglareSceneCbTargetRaw();
            g_nominationGetTypeCalls = g_nominationGetDescCalls = 0;
            const bool texturePlainOk = vScreenSunglareNominationPredicateTestVisit(
                context, 'X', 10001, nullptr, false, &texturePlain);
            const auto plainTypeCalls = g_nominationGetTypeCalls;
            const auto plainDescCalls = g_nominationGetDescCalls;
            okay &= check(textureHooked && textureTraceOk && texturePlainOk &&
                              checkRead(textureFact.boundCbIdentity,
                                  reinterpret_cast<std::uintptr_t>(trackedTexture.Get())) &&
                              checkRead(textureFact.resourceResolved, true) &&
                              checkRead(textureFact.isBuffer, false) &&
                              !textureFact.byteWidth.reached &&
                              checkRead(textureFact.callbackInvoked, false) &&
                              !textureFact.nominatedAfterIdentity.reached &&
                              traceTypeCalls == 1 && plainTypeCalls == 1 &&
                              traceDescCalls == 1 && plainDescCalls == 1 &&
                              textureTrace.sceneCbNominatedAfter == nullptr &&
                              texturePlain.sceneCbNominatedAfter == nullptr &&
                              sunglareSceneCbTargetRaw() == textureTargetBeforePlain &&
                              draw_ladder_trace::sunglareNominationFactCountForTest(
                                  texturePlain.token) == 0,
                          "manually seeded tracked texture shadow is a non-buffer negative, not an actual CB binding");
            textureHook.uninstall();
            g_nominationGetTypeCalls = typeCalls;
            g_nominationGetDescCalls = descCalls;
        }
        detail::g_sunglareWorld = 1;
        VScreenPredicateTestResult unresolved{};
        SunglareNominationObservation unresolvedFact{};
        sunglareSceneCb(savedTarget);
        g_nominationTypeFault = true;
        g_nominationGetTypeCalls = g_nominationGetDescCalls = 0;
        const bool unresolvedVisited = visit(cb208.Get(), 10001, nullptr, true, &unresolved) &&
            readSunglareNomination(unresolved, &unresolvedFact);
        const auto unresolvedTraceTypeCalls = g_nominationGetTypeCalls;
        const auto unresolvedTraceDescCalls = g_nominationGetDescCalls;
        void* const unresolvedTargetAfterTrace = sunglareSceneCbTargetRaw();
        sunglareSceneCb(savedTarget);
        g_nominationGetTypeCalls = g_nominationGetDescCalls = 0;
        VScreenPredicateTestResult unresolvedPlain{};
        const bool unresolvedPlainVisited = visit(
            cb208.Get(), 10001, nullptr, false, &unresolvedPlain);
        const auto unresolvedPlainTypeCalls = g_nominationGetTypeCalls;
        const auto unresolvedPlainDescCalls = g_nominationGetDescCalls;
        void* const unresolvedTargetAfterPlain = sunglareSceneCbTargetRaw();
        g_nominationTypeFault = false;
        okay &= check(unresolvedVisited && unresolvedPlainVisited &&
                          checkRead(unresolvedFact.resourceResolved, false) &&
                          !unresolvedFact.isBuffer.reached &&
                          !unresolvedFact.byteWidth.reached &&
                          checkRead(unresolvedFact.callbackInvoked, false) &&
                          unresolvedTraceTypeCalls == 1 &&
                          unresolvedPlainTypeCalls == 1 &&
                          unresolvedTraceDescCalls == 0 &&
                          unresolvedPlainDescCalls == 0 &&
                          unresolved.sceneCbNominatedAfter == nullptr &&
                          unresolvedPlain.sceneCbNominatedAfter == nullptr &&
                          unresolvedTargetAfterTrace == savedTarget &&
                          unresolvedTargetAfterPlain == savedTarget &&
                          draw_ladder_trace::sunglareNominationFactCountForTest(
                              unresolvedPlain.token) == 0,
                      "Trace/NoTrace GetType fault preserves prefix, query count, and non-mutation");
        hook208.uninstall();
        hook192.uninstall();
        hook224.uninstall();
    }
    detail::g_sunglareWorld = savedWorld;
    g_nominationGetTypeCalls = savedTypeCalls;
    g_nominationGetDescCalls = savedDescCalls;
    g_nominationTypeFault = savedTypeFault;
    g_realResourceGetType = savedGetType;
    g_realBufferGetDesc = savedBufferDesc;
    g_realTextureGetDesc = savedTextureDesc;
    sunglareSceneCb(savedTarget);
    context->VSSetConstantBuffers(0, 1, &savedActual);
    bindingSet(BindSlot::VsCb0, savedShadow);
    if (savedActual) savedActual->Release();
    return okay;
}

using edvr::VScreenPanelDistanceApiTestEvent;
using edvr::VScreenPanelDistanceApiTestInput;
using edvr::VScreenPanelDistanceApiTestResult;
using edvr::VScreenClassifierSiteEvent;
namespace draw_ladder = edvr::draw_ladder;
namespace draw_ladder_trace = edvr::draw_ladder_trace;
namespace plugin_cost = edvr::plugin_cost;

constexpr std::uint16_t kPanelDistanceClaimSite = 66;
constexpr std::uint16_t kPanelDistanceMapApiSite = 122;
constexpr std::uint16_t kPanelDistanceUnmapApiSite = 123;
constexpr std::uint16_t kPanelDistanceOverrideBindApiSite = 124;
constexpr std::uint16_t kPanelDistanceRestoreBindApiSite = 125;

bool panelApiSite(const EdvrPluginCostOwnerV1& owner, std::uint16_t site) {
    return (owner.apiSiteMask[site / 64] & (std::uint64_t{1} << (site % 64))) != 0;
}

bool panelCpuSite(const EdvrPluginCostOwnerV1& owner, std::uint16_t site) {
    return (owner.cpuSiteMask[site / 64] & (std::uint64_t{1} << (site % 64))) != 0;
}

bool panelEventSequence(const VScreenPanelDistanceApiTestResult& result,
                        std::initializer_list<VScreenPanelDistanceApiTestEvent> expected) {
    if (result.eventOverflow || result.eventCount != expected.size()) return false;
    std::size_t i = 0;
    for (const auto event : expected) {
        if (result.events[i++] != static_cast<std::uint8_t>(event)) return false;
    }
    return true;
}

bool configurePanelCostWindow(ID3D11DeviceContext* ownerContext, bool apiSample) {
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(0x2u, static_cast<std::uint64_t>(frequency.QuadPart));
    edvrPluginCostSetOwnerContext(ownerContext);
    EdvrPluginCostWindowV1 discarded{};
    // The first boundary publishes the render-owner thread and starts the
    // requested API sample frame; its partial window is intentionally skipped.
    (void)edvrPluginCostFrameBoundary(1, 0, 0, apiSample ? 1u : 0u, 0, &discarded);
    return true;
}

bool completePanelCostWindow(bool apiSample, bool cpuSample,
                             EdvrPluginCostWindowV1* window) {
    if (!window) return false;
    bool ready = false;
    // Frame 1 is the collector's configure/reload discard. Frames 2..1801
    // close one complete 1800-frame window while retaining the chosen axes.
    for (std::uint32_t frame = 2; frame <= 1801; ++frame) {
        const std::uint8_t nextApi = apiSample && frame < 1801 ? 1u : 0u;
        ready = edvrPluginCostFrameBoundary(
            frame, cpuSample ? 1u : 0u, apiSample ? 1u : 0u, nextApi, 0, window) != 0;
    }
    return ready;
}

bool testPanelDistanceApiTransaction(ID3D11DeviceContext* immediate,
                                     ID3D11DeviceContext* deferred) {
    bool okay = true;
    std::uint64_t compositeIdentity = 0x50414E454Cull;
    std::uint64_t overrideIdentity = 0x4F55524342ull;
    alignas(float) std::uint8_t mappedStorage[256]{};
    VScreenPanelDistanceApiTestInput base{};
    base.context = immediate;
    base.ownerContext = immediate;
    base.shadowBytes = 16;
    base.distanceIndex = 2;
    base.distanceScale = 0.5f;
    const float originalConstants[4] = {2.0f, 4.0f, 6.0f, 8.0f};
    std::memcpy(base.shadow, originalConstants, sizeof(originalConstants));
    base.compositeCb = &compositeIdentity;
    base.ourCb = &overrideIdentity;
    base.mappedStorage = mappedStorage;
    base.mappedStorageBytes = base.shadowBytes;
    base.kind = 'I';
    base.drawCount = 6;
    base.drawInstances = 1;
    // DrawIndexed uses only start/base; startInstance belongs to the
    // instanced families and must remain zero for this fixture.
    base.drawArgs = {2, -3, 0};

    const auto run = [&](const VScreenPanelDistanceApiTestInput& input,
                         bool apiSample, bool cpuSample,
                         VScreenPanelDistanceApiTestResult* result,
                         EdvrPluginCostWindowV1* window) {
        const bool configured = configurePanelCostWindow(input.ownerContext, apiSample);
        const bool visited = configured &&
            vScreenPanelDistanceApiTransactionTest(input, result);
        const bool completed = visited && completePanelCostWindow(apiSample, cpuSample, window);
        edvrPluginCostShutdown();
        return configured && visited && completed;
    };
    const auto ownerAt = [](const EdvrPluginCostWindowV1& window)
        -> const EdvrPluginCostOwnerV1& {
        return window.owners[static_cast<std::uint8_t>(plugin_cost::Owner::OnFootPanel)];
    };
    const auto apiCallsAre = [&](const EdvrPluginCostWindowV1& window,
                                 std::uint64_t transfer, std::uint64_t state) {
        const auto& owner = ownerAt(window);
        return owner.owner == static_cast<std::uint8_t>(plugin_cost::Owner::OnFootPanel) &&
            owner.apiCalls[static_cast<std::uint8_t>(plugin_cost::ApiClass::Transfer)] == transfer &&
            owner.apiCalls[static_cast<std::uint8_t>(plugin_cost::ApiClass::State)] == state;
    };
    const auto siteMaskIs = [&](const EdvrPluginCostWindowV1& window,
                                bool map, bool unmap, bool apply, bool restore) {
        const auto& owner = ownerAt(window);
        return panelApiSite(owner, kPanelDistanceMapApiSite) == map &&
            panelApiSite(owner, kPanelDistanceUnmapApiSite) == unmap &&
            panelApiSite(owner, kPanelDistanceOverrideBindApiSite) == apply &&
            panelApiSite(owner, kPanelDistanceRestoreBindApiSite) == restore;
    };

    VScreenPanelDistanceApiTestResult result{};
    EdvrPluginCostWindowV1 window{};

    // The production path calls Map, Unmap, binds the replacement, forwards
    // the original draw, then restores the original constant buffer.
    std::memset(mappedStorage, 0, sizeof(mappedStorage));
    okay &= check(run(base, true, false, &result, &window),
                  "PanelDistance successful path completes a sampled API window");
    const float scaledConstants[4] = {2.0f, 4.0f, 3.0f, 8.0f};
    const auto expectedEvents = {VScreenPanelDistanceApiTestEvent::Map,
        VScreenPanelDistanceApiTestEvent::Unmap,
        VScreenPanelDistanceApiTestEvent::OverrideBind,
        VScreenPanelDistanceApiTestEvent::OriginalDraw,
        VScreenPanelDistanceApiTestEvent::RestoreBind};
    okay &= check(result.siteResult.outcome == draw_ladder::SiteOutcome::Claimed &&
                      result.siteResult.verdict == static_cast<std::int16_t>(
                          edvr::draw_ladder::VerdictOrdinal::kPanel) &&
                      result.mapCalls == 1 && result.unmapCalls == 1 &&
                      result.constantBufferCalls == 2 && result.originalDrawCalls == 1 &&
                      result.mapArgumentsValid && result.unmapArgumentsValid &&
                      result.overrideBindArgumentsValid && result.restoreBindArgumentsValid &&
                      result.drawArgumentsValid && result.finalBoundCb == base.compositeCb &&
                      panelEventSequence(result, expectedEvents),
                  "PanelDistance actual visitor and forwarder preserve exact saved-original order and arguments");
    okay &= check(result.mapSubresource == 0 &&
                      result.mapType == static_cast<std::uint32_t>(D3D11_MAP_WRITE_DISCARD) &&
                      result.mapFlags == 0 &&
                      result.unmapSubresource == 0 && result.bindStartSlots[0] == 0 &&
                      result.bindStartSlots[1] == 0 && result.bindCounts[0] == 1 &&
                      result.bindCounts[1] == 1 && result.bindBuffers[0] == base.ourCb &&
                      result.bindBuffers[1] == base.compositeCb &&
                      result.mappedBytes == base.shadowBytes &&
                      std::memcmp(result.mappedSnapshot, scaledConstants, sizeof(scaledConstants)) == 0,
                  "PanelDistance scales only the selected shadow constant and restores the saved binding");
    okay &= check(apiCallsAre(window, 2, 2) &&
                      siteMaskIs(window, true, true, true, true),
                  "SampledApi attributes successful Map/Unmap and both binds to OnFootPanel");

    // A Map HRESULT failure still counts the actual Map call, but no later
    // transfer or binding exists to attribute.
    auto mapFailure = base;
    mapFailure.mapHresult = static_cast<std::int32_t>(E_FAIL);
    result = {};
    okay &= check(run(mapFailure, true, false, &result, &window) &&
                      result.mapCalls == 1 && result.unmapCalls == 0 &&
                      result.constantBufferCalls == 0 && result.originalDrawCalls == 1 &&
                      result.mapArgumentsValid && result.finalBoundCb == base.compositeCb &&
                      panelEventSequence(result, {VScreenPanelDistanceApiTestEvent::Map,
                                                  VScreenPanelDistanceApiTestEvent::OriginalDraw}) &&
                      apiCallsAre(window, 1, 0) &&
                      siteMaskIs(window, true, false, false, false),
                  "failed Map records only the attempted transfer and preserves the original draw/binding");

    // S_OK with a null mapped pointer is also a refused transaction and must
    // not invent an Unmap or either state bind.
    auto nullMap = base;
    nullMap.mapReturnsNull = true;
    result = {};
    okay &= check(run(nullMap, true, false, &result, &window) &&
                      result.mapCalls == 1 && result.unmapCalls == 0 &&
                      result.constantBufferCalls == 0 && result.originalDrawCalls == 1 &&
                      result.mapArgumentsValid &&
                      panelEventSequence(result, {VScreenPanelDistanceApiTestEvent::Map,
                                                  VScreenPanelDistanceApiTestEvent::OriginalDraw}) &&
                      apiCallsAre(window, 1, 0) &&
                      siteMaskIs(window, true, false, false, false),
                  "null Map data records the attempted Map and no synthetic cleanup/binds");

    // The API sample axis stays active while CPU sampling is independently off
    // or on. A live trace capture must also force NoCpu without muting API data.
    auto cpuSampled = base;
    cpuSampled.cpuSample = true;
    result = {};
    okay &= check(run(cpuSampled, true, true, &result, &window) &&
                      apiCallsAre(window, 2, 2) &&
                      ownerAt(window).cpuTimedScopes > 0 &&
                      panelCpuSite(ownerAt(window), kPanelDistanceClaimSite),
                  "SampledApi remains independent when the actual claim also uses SampledCpu");
    auto replay = base;
    replay.traceEnabled = true;
    replay.cpuSample = true;
    result = {};
    okay &= check(run(replay, true, true, &result, &window) &&
                      result.token.valid() &&
                      draw_ladder_trace::actionCountForTest(result.token) > 0 &&
                      apiCallsAre(window, 2, 2) && ownerAt(window).cpuTimedScopes == 0,
                  "active DrawLadderTrace selects NoCpu while retaining sampled PanelDistance API attribution");

    // An unselected API frame still runs the rendering transaction; notes are
    // erased. The warm-site seam deliberately bypasses the ladder's foreign
    // context exit: a mismatched context selects NoApi while this prequalified
    // claim still renders. An unclaimed draw performs no panel transaction.
    auto unsampled = base;
    result = {};
    okay &= check(run(unsampled, false, false, &result, &window) &&
                      result.siteResult.outcome == draw_ladder::SiteOutcome::Claimed &&
                      result.mapCalls == 1 && result.unmapCalls == 1 &&
                      result.constantBufferCalls == 2 && result.originalDrawCalls == 1 &&
                      apiCallsAre(window, 0, 0) && siteMaskIs(window, false, false, false, false),
                  "NoApi erases notes without changing the actual PanelDistance transaction");

    auto foreignContext = base;
    foreignContext.context = deferred;
    foreignContext.ownerContext = immediate;
    result = {};
    okay &= check(run(foreignContext, true, false, &result, &window) &&
                      result.siteResult.outcome == draw_ladder::SiteOutcome::Claimed &&
                      result.siteResult.verdict == static_cast<std::int16_t>(
                          edvr::draw_ladder::VerdictOrdinal::kPanel) &&
                      result.mapCalls == 1 && result.unmapCalls == 1 &&
                      result.constantBufferCalls == 2 && result.originalDrawCalls == 1 &&
                      result.mapArgumentsValid && result.unmapArgumentsValid &&
                      result.overrideBindArgumentsValid && result.restoreBindArgumentsValid &&
                      result.drawArgumentsValid && result.finalBoundCb == base.compositeCb &&
                      panelEventSequence(result, expectedEvents) &&
                      result.mappedBytes == base.shadowBytes &&
                      std::memcmp(result.mappedSnapshot, scaledConstants, sizeof(scaledConstants)) == 0 &&
                      apiCallsAre(window, 0, 0) &&
                      siteMaskIs(window, false, false, false, false),
                  "foreign context selects NoApi without altering the prequalified warm-site transaction");

    auto noClaim = base;
    noClaim.distanceEnabled = false;
    result = {};
    okay &= check(run(noClaim, true, false, &result, &window) &&
                      result.mapCalls == 0 && result.unmapCalls == 0 &&
                      result.constantBufferCalls == 0 && result.originalDrawCalls == 1 &&
                      apiCallsAre(window, 0, 0) &&
                      siteMaskIs(window, false, false, false, false),
                  "a draw with no PanelDistance claim leaves API counters and site mask empty");

    edvrPluginCostShutdown();
    return okay;
}

struct ExpectedClassifierSite final {
    draw_ladder::SiteId id;
    draw_ladder::SiteKind kind;
    draw_ladder::SiteOutcome outcome;
    std::uint16_t subsite = 0;
    std::int16_t verdict = -1;
};

bool classifierSiteSequenceIs(const VScreenPanelDistanceApiTestResult& result,
                              const ExpectedClassifierSite* expected,
                              std::size_t expectedCount) {
    if (result.classifierSiteOverflow || result.classifierSiteCount != expectedCount) {
        std::fprintf(stderr, "Classifier sites: actual=%u expected=%u overflow=%u\n",
            static_cast<unsigned>(result.classifierSiteCount), static_cast<unsigned>(expectedCount),
            static_cast<unsigned>(result.classifierSiteOverflow));
        return false;
    }
    for (std::size_t i = 0; i < expectedCount; ++i) {
        const auto& actual = result.classifierSites[i];
        const auto& want = expected[i];
        if (actual.siteId != static_cast<std::uint16_t>(want.id) ||
            actual.kind != static_cast<std::uint8_t>(want.kind) ||
            actual.outcome != static_cast<std::uint8_t>(want.outcome) ||
            actual.subsite != want.subsite || actual.verdict != want.verdict) {
            std::fprintf(stderr,
                "Classifier site[%u]: id=%u/%u kind=%u/%u outcome=%u/%u subsite=%u/%u verdict=%d/%d\n",
                static_cast<unsigned>(i), static_cast<unsigned>(actual.siteId), static_cast<unsigned>(want.id),
                static_cast<unsigned>(actual.kind), static_cast<unsigned>(want.kind),
                static_cast<unsigned>(actual.outcome), static_cast<unsigned>(want.outcome),
                static_cast<unsigned>(actual.subsite), static_cast<unsigned>(want.subsite),
                static_cast<int>(actual.verdict), static_cast<int>(want.verdict));
            return false;
        }
    }
    return true;
}

bool noneDrawActionsMatch(const VScreenPanelDistanceApiTestResult& result,
                          const VScreenPanelDistanceApiTestInput& input) {
    using namespace draw_ladder;
    if (!result.token.valid() || draw_ladder_trace::actionCountForTest(result.token) != 3)
        return false;
    const DrawCallKind call = input.kind == 'D' ? DrawCallKind::Draw
        : input.kind == 'I' ? DrawCallKind::DrawIndexed
        : input.kind == 'N' ? DrawCallKind::DrawInstanced
                            : DrawCallKind::DrawIndexedInstanced;
    const std::uint32_t start = (input.kind == 'D' || input.kind == 'N')
        ? static_cast<std::uint32_t>(input.drawArgs.base) : input.drawArgs.start;
    const std::uint32_t startInstance = (input.kind == 'N' || input.kind == 'X')
        ? input.drawArgs.startInstance : 0;
    const std::int32_t baseVertex = (input.kind == 'I' || input.kind == 'X')
        ? input.drawArgs.base : 0;
    const ActionId expectedIds[] = {ActionId::kDrawBegin, ActionId::kOriginalDraw,
                                    ActionId::kDrawEnd};
    const ActionPhase expectedPhases[] = {ActionPhase::Begin, ActionPhase::Issue,
                                          ActionPhase::End};
    for (std::uint16_t i = 0; i < 3; ++i) {
        std::uint16_t id = 0;
        ActionRecord record{};
        if (!draw_ladder_trace::readActionForTest(result.token, i, &id, &record) ||
            id != static_cast<std::uint16_t>(expectedIds[i]) ||
            record.phase != expectedPhases[i] || record.outcome != ActionOutcome::Applied)
            return false;
        if (i == 1 && (record.call != call || record.issueCount != 1 ||
                       record.count != input.drawCount ||
                       record.instances != input.drawInstances || record.start != start ||
                       record.startInstance != startInstance ||
                       record.baseVertex != baseVertex)) return false;
    }
    return true;
}

bool panelDrawActionsMatch(const VScreenPanelDistanceApiTestResult& result,
                           const VScreenPanelDistanceApiTestInput& input) {
    using namespace draw_ladder;
    // Every non-None verdict also records the forwarder's replace bracket,
    // including Panel, whose actual CB restore belongs to the caller.
    if (!result.token.valid() || draw_ladder_trace::actionCountForTest(result.token) != 6) {
        std::fprintf(stderr, "Panel %c trace action count: actual=%u expected=6 token=%u\n",
            input.kind, static_cast<unsigned>(draw_ladder_trace::actionCountForTest(result.token)),
            static_cast<unsigned>(result.token.valid()));
        return false;
    }
    const DrawCallKind call = input.kind == 'D' ? DrawCallKind::Draw
        : input.kind == 'I' ? DrawCallKind::DrawIndexed
        : input.kind == 'N' ? DrawCallKind::DrawInstanced
                            : DrawCallKind::DrawIndexedInstanced;
    const std::uint32_t start = (input.kind == 'D' || input.kind == 'N')
        ? static_cast<std::uint32_t>(input.drawArgs.base) : input.drawArgs.start;
    const std::uint32_t startInstance = (input.kind == 'N' || input.kind == 'X')
        ? input.drawArgs.startInstance : 0;
    const std::int32_t baseVertex = (input.kind == 'I' || input.kind == 'X')
        ? input.drawArgs.base : 0;
    const ActionId expectedIds[] = {ActionId::kDrawBegin, ActionId::kReplaceDraw,
        ActionId::kOriginalDraw, ActionId::kReplaceDraw,
        ActionId::kPanelConstantBufferRestore, ActionId::kDrawEnd};
    const ActionPhase expectedPhases[] = {ActionPhase::Begin, ActionPhase::Begin,
        ActionPhase::Issue, ActionPhase::End, ActionPhase::Restore, ActionPhase::End};
    const ActionOutcome expectedOutcomes[] = {ActionOutcome::Applied, ActionOutcome::Attempted,
        ActionOutcome::Applied, ActionOutcome::Applied, ActionOutcome::Applied,
        ActionOutcome::Applied};
    const std::uint16_t expectedFlags[] = {0, 1, 0, 1, 0, 0}; // Panel verdict ordinal.
    const std::uint16_t expectedIssueCounts[] = {0, 0, 1, 0, 0, 0};
    for (std::uint16_t i = 0; i < 6; ++i) {
        std::uint16_t id = 0;
        ActionRecord record{};
        const bool read = draw_ladder_trace::readActionForTest(result.token, i, &id, &record);
        if (!read ||
            id != static_cast<std::uint16_t>(expectedIds[i]) ||
            record.phase != expectedPhases[i] || record.outcome != expectedOutcomes[i] ||
            record.flags != expectedFlags[i] || record.issueCount != expectedIssueCounts[i] ||
            record.call != call || record.count != input.drawCount ||
            record.instances != input.drawInstances || record.start != start ||
            record.startInstance != startInstance || record.baseVertex != baseVertex) {
            std::fprintf(stderr,
                "Panel %c action[%u]: read=%u id=%u/%u phase=%u/%u outcome=%u/%u "
                "flags=%u/%u issues=%u/%u call=%u/%u count=%u/%u instances=%u/%u "
                "start=%u/%u startInstance=%u/%u base=%d/%d\n",
                input.kind, static_cast<unsigned>(i), static_cast<unsigned>(read),
                static_cast<unsigned>(id), static_cast<unsigned>(expectedIds[i]),
                static_cast<unsigned>(record.phase), static_cast<unsigned>(expectedPhases[i]),
                static_cast<unsigned>(record.outcome), static_cast<unsigned>(expectedOutcomes[i]),
                static_cast<unsigned>(record.flags), static_cast<unsigned>(expectedFlags[i]),
                static_cast<unsigned>(record.issueCount), static_cast<unsigned>(expectedIssueCounts[i]),
                static_cast<unsigned>(record.call), static_cast<unsigned>(call),
                record.count, input.drawCount, record.instances, input.drawInstances,
                record.start, start, record.startInstance, startInstance,
                record.baseVertex, baseVertex);
            return false;
        }
    }
    return true;
}

bool testFullClassifierTerminalPath(ID3D11Device* device,
                                    ID3D11DeviceContext* immediate) {
    using draw_ladder::SiteId;
    using draw_ladder::SiteKind;
    using draw_ladder::SiteOutcome;
    bool okay = true;

    // Literal expected production order and outcomes. The two interest gates
    // are forced off by the seam; plugin dispatch is disabled. The matching
    // WARP SRV/CB lets panel eligibility pass, and injected Map failure is the
    // only reason the enabled case reaches PanelTailNone.
    constexpr ExpectedClassifierSite full[] = {
        {SiteId::kFssChromeSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kForeignContextNone, SiteKind::Exit, SiteOutcome::Observed},
        {SiteId::kDrawGateDisabledNone, SiteKind::Exit, SiteOutcome::Observed},
        {SiteId::kParticleProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kParticleSubstitute, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kWitchspaceStarsSkip, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kStateSnapshot, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kRouteSelected, SiteKind::Observe, SiteOutcome::Observed, 6},
        {SiteId::kEyeDepthAndCount, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeUiDepthProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeHoloDepthProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kIntroPanelClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kIntroCurveObserve, SiteKind::Observe, SiteOutcome::NotEligible},
        {SiteId::kSunglareNomination, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeCensusSubmitted, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kUiCrispProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kObjectProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeCensusSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kEyeRangeSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kNightVisionClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kRemlokHideSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kRemlokScissorClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kHoloClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kTargetSharpClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kScrimClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kEyeBackdropComposite, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kFssPanelClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kFssRevealClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kFssDumpClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kResolveBindClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kSunglareSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kSunglareSteadyClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kGlareClampClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kHeadOffsetObserve, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kPanelCurveObserve, SiteKind::Observe, SiteOutcome::NotEligible},
        {SiteId::kEyeNoDistanceNone, SiteKind::Exit, SiteOutcome::Declined},
        {SiteId::kPanelEligibilityNone, SiteKind::Exit, SiteOutcome::Declined},
        {SiteId::kPanelDistanceClaim, SiteKind::Claim, SiteOutcome::Declined, 1},
        {SiteId::kPanelTailNone, SiteKind::Exit, SiteOutcome::Exited, 1, 0},
    };
    constexpr ExpectedClassifierSite distanceDisabled[] = {
        {SiteId::kFssChromeSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kForeignContextNone, SiteKind::Exit, SiteOutcome::Observed},
        {SiteId::kDrawGateDisabledNone, SiteKind::Exit, SiteOutcome::Observed},
        {SiteId::kParticleProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kParticleSubstitute, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kWitchspaceStarsSkip, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kStateSnapshot, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kRouteSelected, SiteKind::Observe, SiteOutcome::Observed, 6},
        {SiteId::kEyeDepthAndCount, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeUiDepthProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeHoloDepthProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kIntroPanelClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kIntroCurveObserve, SiteKind::Observe, SiteOutcome::NotEligible},
        {SiteId::kSunglareNomination, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeCensusSubmitted, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kUiCrispProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kObjectProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeCensusSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kEyeRangeSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kNightVisionClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kRemlokHideSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kRemlokScissorClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kHoloClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kTargetSharpClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kScrimClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kEyeBackdropComposite, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kFssPanelClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kFssRevealClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kFssDumpClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kResolveBindClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kSunglareSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kSunglareSteadyClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kGlareClampClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kHeadOffsetObserve, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kPanelCurveObserve, SiteKind::Observe, SiteOutcome::NotEligible},
        {SiteId::kEyeNoDistanceNone, SiteKind::Exit, SiteOutcome::Exited, 0, 0},
    };
    constexpr ExpectedClassifierSite successful[] = {
        {SiteId::kFssChromeSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kForeignContextNone, SiteKind::Exit, SiteOutcome::Observed},
        {SiteId::kDrawGateDisabledNone, SiteKind::Exit, SiteOutcome::Observed},
        {SiteId::kParticleProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kParticleSubstitute, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kWitchspaceStarsSkip, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kStateSnapshot, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kRouteSelected, SiteKind::Observe, SiteOutcome::Observed, 6},
        {SiteId::kEyeDepthAndCount, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeUiDepthProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeHoloDepthProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kIntroPanelClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kIntroCurveObserve, SiteKind::Observe, SiteOutcome::NotEligible},
        {SiteId::kSunglareNomination, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeCensusSubmitted, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kUiCrispProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kObjectProbe, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kEyeCensusSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kEyeRangeSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kNightVisionClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kRemlokHideSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kRemlokScissorClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kHoloClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kTargetSharpClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kScrimClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kEyeBackdropComposite, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kFssPanelClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kFssRevealClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kFssDumpClaim, SiteKind::Claim, SiteOutcome::NotEligible},
        {SiteId::kResolveBindClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kSunglareSkip, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kSunglareSteadyClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kGlareClampClaim, SiteKind::Claim, SiteOutcome::Declined},
        {SiteId::kHeadOffsetObserve, SiteKind::Observe, SiteOutcome::Observed},
        {SiteId::kPanelCurveObserve, SiteKind::Observe, SiteOutcome::NotEligible},
        {SiteId::kEyeNoDistanceNone, SiteKind::Exit, SiteOutcome::Declined},
        {SiteId::kPanelEligibilityNone, SiteKind::Exit, SiteOutcome::Declined},
        {SiteId::kPanelDistanceClaim, SiteKind::Claim, SiteOutcome::Claimed, 0, 1},
    };

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = 1920;
    textureDesc.Height = 1080;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> panelTexture;
    ComPtr<ID3D11ShaderResourceView> panelSrv;
    D3D11_TEXTURE2D_DESC eyeDesc = textureDesc;
    eyeDesc.Width = 2048;
    eyeDesc.Height = 2048;
    eyeDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> eyeTexture;
    ComPtr<ID3D11RenderTargetView> eyeRtv;
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 16;
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> compositeCb;
    ComPtr<ID3D11Buffer> overrideCb;
    okay &= check(device && SUCCEEDED(device->CreateTexture2D(&textureDesc, nullptr,
                                                            &panelTexture)) &&
                      SUCCEEDED(device->CreateShaderResourceView(panelTexture.Get(), nullptr,
                                                                  &panelSrv)) &&
                      SUCCEEDED(device->CreateTexture2D(&eyeDesc, nullptr, &eyeTexture)) &&
                      SUCCEEDED(device->CreateRenderTargetView(eyeTexture.Get(), nullptr,
                                                                &eyeRtv)) &&
                      SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, &compositeCb)) &&
                      SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, &overrideCb)),
                  "WARP creates actual panel-sized SRV and matching constant buffers for full ladder");
    if (!device || !immediate || !panelSrv || !eyeRtv || !compositeCb || !overrideCb) return false;

    alignas(float) std::uint8_t shadow[16]{};
    const float shadowValues[4] = {2.0f, 4.0f, 6.0f, 8.0f};
    std::memcpy(shadow, shadowValues, sizeof(shadowValues));
    auto makeInput = [&](bool distanceEnabled, bool traceEnabled) {
        VScreenPanelDistanceApiTestInput input{};
        input.context = immediate;
        input.ownerContext = immediate;
        input.traceEnabled = traceEnabled;
        input.distanceEnabled = distanceEnabled;
        input.shadowBytes = sizeof(shadow);
        std::memcpy(input.shadow, shadow, sizeof(shadow));
        input.distanceIndex = 2;
        input.distanceScale = 0.5f;
        input.compositeCb = compositeCb.Get();
        input.ourCb = overrideCb.Get();
        input.mapHresult = static_cast<std::int32_t>(E_FAIL);
        input.fullClassifier = true;
        input.panelSrv = panelSrv.Get();
        input.eyeRtv = eyeRtv.Get();
        input.kind = 'I';
        input.drawCount = 6;
        input.drawArgs = {2, -3, 0};
        return input;
    };

    struct DrawFixture final {
        char kind;
        std::uint32_t instances;
        edvr::DrawArgs args;
    };
    // Match each real hook's populated fields; unused fields remain zero.
    constexpr DrawFixture drawFixtures[] = {
        {'D', 1, {0, -3, 0}}, {'I', 1, {2, -3, 0}},
        {'N', 3, {0, -3, 7}}, {'X', 3, {2, -3, 7}},
    };
    for (const bool traceEnabled : {false, true}) {
      for (const auto& draw : drawFixtures) {
        VScreenPanelDistanceApiTestResult result{};
        auto input = makeInput(true, traceEnabled);
        input.kind = draw.kind;
        input.drawInstances = draw.instances;
        input.drawArgs = draw.args;
        const bool ran = edvr::vScreenPanelDistanceApiTransactionTest(input, &result);
        okay &= check(ran && result.winner == static_cast<std::int16_t>(SiteId::kPanelTailNone) &&
                          result.verdict == static_cast<std::int16_t>(draw_ladder::VerdictOrdinal::kNone) &&
                          result.mapCalls == 1 &&
                          result.unmapCalls == 0 && result.constantBufferCalls == 0 &&
                          result.originalDrawCalls == 1 &&
                          result.finalBoundCb == input.compositeCb &&
                          result.mapArgumentsValid && result.drawArgumentsValid &&
                          panelEventSequence(result, {VScreenPanelDistanceApiTestEvent::Map,
                                                      VScreenPanelDistanceApiTestEvent::OriginalDraw}),
                          traceEnabled
                          ? "Trace full VR-eye classifier reaches PanelTailNone after injected Map failure and forwards one original draw"
                          : "NoTrace full VR-eye classifier reaches PanelTailNone after injected Map failure and forwards one original draw");
        if (traceEnabled) {
            okay &= check(result.siteResult.outcome == SiteOutcome::Exited &&
                              result.siteResult.subsite == 1 &&
                              classifierSiteSequenceIs(result, full, sizeof(full) / sizeof(full[0])),
                          "Trace full VR-eye classifier matches the literal ordered all-decline site/outcome sequence");
            okay &= check(noneDrawActionsMatch(result, input),
                          "None verdict forwards exactly one typed original draw between DrawBegin/DrawEnd");
        } else {
            okay &= check(result.classifierSiteCount == 0 && !result.classifierSiteOverflow,
                          "NoTrace full classifier emits no ordered-site observations");
        }

        VScreenPanelDistanceApiTestResult disabledResult{};
        auto disabled = makeInput(false, traceEnabled);
        disabled.kind = draw.kind;
        disabled.drawInstances = draw.instances;
        disabled.drawArgs = draw.args;
        const bool disabledRan = edvr::vScreenPanelDistanceApiTransactionTest(
            disabled, &disabledResult);
        okay &= check(disabledRan &&
                          disabledResult.winner == static_cast<std::int16_t>(SiteId::kEyeNoDistanceNone) &&
                          disabledResult.verdict == static_cast<std::int16_t>(draw_ladder::VerdictOrdinal::kNone) &&
                          disabledResult.mapCalls == 0 && disabledResult.unmapCalls == 0 &&
                          disabledResult.constantBufferCalls == 0 &&
                          disabledResult.originalDrawCalls == 1 &&
                          disabledResult.finalBoundCb == disabled.compositeCb &&
                          disabledResult.drawArgumentsValid &&
                          panelEventSequence(disabledResult,
                              {VScreenPanelDistanceApiTestEvent::OriginalDraw}),
                      traceEnabled
                          ? "Trace distance-disabled full classifier exits at EyeNoDistanceNone and forwards one original draw"
                          : "NoTrace distance-disabled full classifier exits at EyeNoDistanceNone and forwards one original draw");
        if (traceEnabled) {
            okay &= check(disabledResult.siteResult.outcome == SiteOutcome::Exited &&
                              disabledResult.siteResult.subsite == 0 &&
                              classifierSiteSequenceIs(disabledResult, distanceDisabled,
                                                   sizeof(distanceDisabled) / sizeof(distanceDisabled[0])),
                          "Trace distance-disabled route matches the independent literal prefix and omits later Panel sites");
            okay &= check(noneDrawActionsMatch(disabledResult, disabled),
                          "distance-disabled None verdict records exactly one typed original draw");
        } else {
            okay &= check(disabledResult.classifierSiteCount == 0 &&
                              !disabledResult.classifierSiteOverflow,
                          "NoTrace distance-disabled classifier emits no ordered-site observations");
        }

        alignas(float) std::uint8_t mappedStorage[16]{};
        auto claimed = makeInput(true, traceEnabled);
        claimed.kind = draw.kind;
        claimed.drawInstances = draw.instances;
        claimed.drawArgs = draw.args;
        claimed.mapHresult = static_cast<std::int32_t>(S_OK);
        claimed.mappedStorage = mappedStorage;
        claimed.mappedStorageBytes = sizeof(mappedStorage);
        VScreenPanelDistanceApiTestResult claimedResult{};
        const bool claimRan = edvr::vScreenPanelDistanceApiTransactionTest(
            claimed, &claimedResult);
        const float scaledConstants[4] = {2.0f, 4.0f, 3.0f, 8.0f};
        okay &= check(claimRan &&
                          claimedResult.winner == static_cast<std::int16_t>(SiteId::kPanelDistanceClaim) &&
                          claimedResult.verdict == static_cast<std::int16_t>(
                              draw_ladder::VerdictOrdinal::kPanel) &&
                          claimedResult.mapCalls == 1 && claimedResult.unmapCalls == 1 &&
                          claimedResult.constantBufferCalls == 2 &&
                          claimedResult.originalDrawCalls == 1 &&
                          claimedResult.mapArgumentsValid && claimedResult.unmapArgumentsValid &&
                          claimedResult.overrideBindArgumentsValid &&
                          claimedResult.restoreBindArgumentsValid &&
                          claimedResult.drawArgumentsValid &&
                          claimedResult.finalBoundCb == claimed.compositeCb &&
                          panelEventSequence(claimedResult, {
                              VScreenPanelDistanceApiTestEvent::Map,
                              VScreenPanelDistanceApiTestEvent::Unmap,
                              VScreenPanelDistanceApiTestEvent::OverrideBind,
                              VScreenPanelDistanceApiTestEvent::OriginalDraw,
                              VScreenPanelDistanceApiTestEvent::RestoreBind}),
                      traceEnabled
                          ? "Trace full VR-eye classifier claims PanelDistance and preserves exact API/draw/restore order"
                          : "NoTrace full VR-eye classifier claims PanelDistance and preserves exact API/draw/restore order");
        okay &= check(claimedResult.bindStartSlots[0] == 0 &&
                          claimedResult.bindStartSlots[1] == 0 &&
                          claimedResult.bindCounts[0] == 1 && claimedResult.bindCounts[1] == 1 &&
                          claimedResult.bindBuffers[0] == claimed.ourCb &&
                          claimedResult.bindBuffers[1] == claimed.compositeCb &&
                          claimedResult.mappedBytes == claimed.shadowBytes &&
                          std::memcmp(claimedResult.mappedSnapshot, scaledConstants,
                                      sizeof(scaledConstants)) == 0,
                      "full classifier claim copies shadow constants, scales only the selected value, and restores CB0");
        if (traceEnabled) {
            const bool claimPayloadMatches = claimedResult.siteResult.outcome == SiteOutcome::Claimed &&
                claimedResult.siteResult.verdict == static_cast<std::int16_t>(
                    draw_ladder::VerdictOrdinal::kPanel) && claimedResult.siteResult.subsite == 0;
            if (!claimPayloadMatches) {
                std::fprintf(stderr, "Panel %c claim payload: outcome=%u/%u verdict=%d/1 subsite=%u/0\n",
                    claimed.kind, static_cast<unsigned>(claimedResult.siteResult.outcome),
                    static_cast<unsigned>(SiteOutcome::Claimed),
                    static_cast<int>(claimedResult.siteResult.verdict),
                    static_cast<unsigned>(claimedResult.siteResult.subsite));
            }
            const bool sitesMatch = classifierSiteSequenceIs(claimedResult, successful,
                sizeof(successful) / sizeof(successful[0]));
            const bool actionsMatch = panelDrawActionsMatch(claimedResult, claimed);
            okay &= check(claimPayloadMatches && sitesMatch && actionsMatch,
                          "Trace claim matches the independent 38-site prefix and exact six-action original-draw/restore ledger");
        } else {
            okay &= check(claimedResult.classifierSiteCount == 0 &&
                              !claimedResult.classifierSiteOverflow &&
                              claimedResult.winner == static_cast<std::int16_t>(
                                  SiteId::kPanelDistanceClaim) &&
                              claimedResult.verdict == static_cast<std::int16_t>(
                                  draw_ladder::VerdictOrdinal::kPanel),
                          "NoTrace full classifier claims with zero ordered-site observations");
        }
      }
    }
    return okay;
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
        okay &= testPanelDistanceApiTransaction(immediate, deferred);
        okay &= testFullClassifierTerminalPath(device, immediate);
        // Local action-scope check only: the production forwardWithVerdict
        // receives a real WARP context and an injected thunk boundary. The
        // external caller's real draw implementation is intentionally not run.
        const auto runForwardingPair = [&](const edvr::VScreenForwardingTestInput& input,
                                           const char* label,
                                           std::uint32_t expectedCalls,
                                           std::uint8_t expectedClass) {
            edvr::VScreenForwardingTestResult traced{}, plain{};
            const bool tracedOk = edvr::vScreenForwardingPredicateTestVisit(
                immediate, true, input, &traced);
            const bool plainOk = edvr::vScreenForwardingPredicateTestVisit(
                immediate, false, input, &plain);
            okay &= check(tracedOk && plainOk, label);
            okay &= check(traced.originalCalls == expectedCalls &&
                              plain.originalCalls == expectedCalls &&
                              traced.alteredClass == expectedClass &&
                              plain.alteredClass == expectedClass &&
                              traced.callbackReturned == plain.callbackReturned,
                          "actual WARP forwarder preserves injected callback/class between Trace and NoTrace");
            okay &= check(edvr::draw_ladder_trace::forwardingFactCountForTest(plain.token) == 0,
                          "NoTrace forwarder appends no forwarding fact");
            okay &= check(traced.issueBlockedAfter == plain.issueBlockedAfter &&
                              traced.crispPendingAfter == plain.crispPendingAfter &&
                              traced.planetPendingAfter == plain.planetPendingAfter &&
                              traced.planetSolarPendingAfter == plain.planetSolarPendingAfter &&
                              traced.curveThisDrawAfter == plain.curveThisDrawAfter &&
                              traced.engineVelocityCacheFamilyAfter == plain.engineVelocityCacheFamilyAfter,
                          "actual WARP forwarder restores identical seeded scalar state in Trace and NoTrace");
            return traced;
        };

        edvr::VScreenForwardingTestInput forwardNone{};
        forwardNone.kind = 'X';
        forwardNone.count = 37;
        forwardNone.instances = 4;
        forwardNone.args = {12, -7, 11};
        forwardNone.engineVelocityCacheFamily = -1;
        auto forwardNoneResult = runForwardingPair(forwardNone,
            "actual WARP kNone forwarder executes with injected thunk", 1, 0);
        okay &= check(checkForwardAction(forwardNoneResult, forwardNone,
                              static_cast<std::uint16_t>(ActionId::kOriginalDraw),
                              ActionOutcome::Applied, 1),
                      "kNone action has exact X tuple, Applied outcome, and one known issue");
        edvr::ForwardingObservation forwardFact{};
        okay &= check(readForwarding(forwardNoneResult, &forwardFact),
                      "kNone appends one actual forwardInputs fact to the cold trace pool");
        okay &= check(forwardFact.kind == 1 && forwardFact.version == 1 &&
                          fwRead(forwardFact.verdictOrdinal, 0) &&
                          fwRead(forwardFact.issueBlockedEntry, false) &&
                          fwRead(forwardFact.objectProbeLedgerOn, false) &&
                          fwRead(forwardFact.uiDepthThisDraw, false) &&
                          fwRead(forwardFact.holoDepthThisDraw, false) &&
                          fwRead(forwardFact.compositeThisDraw, false) &&
                          fwSkipped(forwardFact.curveThisDrawBeforeSkipClear) &&
                          fwRead(forwardFact.seedDiagnostics, false) &&
                          fwRead(forwardFact.uiLayerLiveEyeGate, false) &&
                          fwRead(forwardFact.uiLayerLiveFallbackGate, false) &&
                          fwRead(forwardFact.uiLayerWatchingGate, false) &&
                          fwRead(forwardFact.curveThisDrawCurveGate, false) &&
                          fwRead(forwardFact.engineVelocityCacheFamily, -1) &&
                          fwRead(forwardFact.introCurveThisDrawStripGate, false) &&
                          fwRead(forwardFact.issueBlockedBeforeOriginal, false) &&
                          fwRead(forwardFact.originalCallReturned, true) &&
                          fwRead(forwardFact.crispPendingAfterOriginal, false) &&
                          fwRead(forwardFact.issueBlockedAfterOriginal, false) &&
                          fwRead(forwardFact.planetPending, false) &&
                          fwRead(forwardFact.planetSolarPending, false),
                      "kNone fact is the hand-expected lazy raw checkpoint sequence");

        edvr::VScreenForwardingTestInput forwardNoneDeclined = forwardNone;
        forwardNoneDeclined.engineVelocityCacheFamily = 0;
        forwardNoneDeclined.callbackReturns = false;
        auto declinedResult = runForwardingPair(forwardNoneDeclined,
            "actual WARP kNone records a declined injected thunk", 1, 1);
        okay &= check(checkForwardAction(declinedResult, forwardNoneDeclined,
                              static_cast<std::uint16_t>(ActionId::kOriginalDraw),
                              ActionOutcome::Declined, 0),
                      "callback-false action preserves exact X tuple with zero issues");
        edvr::ForwardingObservation declinedFact{};
        okay &= check(readForwarding(declinedResult, &declinedFact) &&
                          fwRead(declinedFact.engineVelocityCacheFamily, 0) &&
                          fwRead(declinedFact.originalCallReturned, false) &&
                          fwSkipped(declinedFact.crispPendingAfterOriginal) &&
                          fwRead(declinedFact.issueBlockedAfterOriginal, false) &&
                          fwRead(declinedFact.planetPending, false) &&
                          fwRead(declinedFact.planetSolarPending, false),
                      "declined callback lazily skips crisp but continues to blocked and planet gates");

        edvr::VScreenForwardingTestInput forwardSkip{};
        forwardSkip.verdictOrdinal = 2;
        forwardSkip.kind = 'I';
        forwardSkip.count = 23;
        forwardSkip.instances = 1;
        forwardSkip.args = {13, -8, 0};
        auto forwardSkipResult = runForwardingPair(forwardSkip,
            "actual WARP kSkip forwards no callback", 0, 0);
        okay &= check(checkForwardAction(forwardSkipResult, forwardSkip,
                              static_cast<std::uint16_t>(ActionId::kSwallowOriginal),
                              ActionOutcome::Applied, 0),
                      "kSkip action has exact I tuple and no original issue");
        edvr::ForwardingObservation skipFact{};
        okay &= check(readForwarding(forwardSkipResult, &skipFact) &&
                          fwRead(skipFact.verdictOrdinal, 2) &&
                          fwRead(skipFact.issueBlockedEntry, false) &&
                          fwRead(skipFact.objectProbeLedgerOn, false) &&
                          fwRead(skipFact.curveThisDrawBeforeSkipClear, false) &&
                          fwSkipped(skipFact.seedDiagnostics) &&
                          fwSkipped(skipFact.uiLayerLiveEyeGate) &&
                          fwSkipped(skipFact.originalCallReturned) &&
                          !forwardSkipResult.curveThisDrawAfter,
                      "kSkip records pre-clear mutation input then preserves every later lazy checkpoint");

        const auto checkKindTuple = [&](char kind, std::uint32_t count,
                                        std::uint32_t instances, edvr::DrawArgs args,
                                        const char* label) {
            edvr::VScreenForwardingTestInput input{};
            input.kind = kind;
            input.count = count;
            input.instances = instances;
            input.args = args;
            const auto result = runForwardingPair(input, label, 1, 0);
            okay &= check(checkForwardAction(result, input,
                                  static_cast<std::uint16_t>(ActionId::kOriginalDraw),
                                  ActionOutcome::Applied, 1),
                          "production original action serializes the expected draw tuple");
        };
        checkKindTuple('D', 29, 1, {91, 27, 5},
                       "actual WARP kNone Draw thunk boundary is injected");
        checkKindTuple('I', 31, 1, {13, -7, 9},
                       "actual WARP kNone DrawIndexed thunk boundary is injected");
        checkKindTuple('N', 35, 4, {89, 12, 11},
                       "actual WARP kNone DrawInstanced tuple is injected");
        const auto checkUnsignedBaseEdge = [&](char kind, std::int32_t base,
                                                std::uint32_t expectedStart,
                                                std::uint32_t instances,
                                                const char* label) {
            edvr::VScreenForwardingTestInput input{};
            input.kind = kind;
            input.count = 17;
            input.instances = instances;
            input.args = {0, base, 6};
            const auto result = runForwardingPair(input, label, 1, 0);
            using namespace edvr::draw_ladder;
            std::uint16_t actionId = 0;
            ActionRecord actual{};
            const bool read = edvr::draw_ladder_trace::readActionForTest(
                result.token, 0, &actionId, &actual);
            const DrawCallKind call = kind == 'D' ? DrawCallKind::Draw
                                                   : DrawCallKind::DrawInstanced;
            okay &= check(edvr::draw_ladder_trace::actionCountForTest(result.token) == 1 &&
                              read && actionId == static_cast<std::uint16_t>(ActionId::kOriginalDraw) &&
                              actual.phase == ActionPhase::Issue &&
                              actual.outcome == ActionOutcome::Applied &&
                              actual.issueCount == 1 && actual.call == call &&
                              actual.flags == 0 && actual.count == 17 &&
                              actual.instances == instances &&
                              actual.start == expectedStart &&
                              actual.startInstance == 6 && actual.baseVertex == 0,
                          "literal D/N action record preserves modulo-2^32 base conversion");
        };
        checkUnsignedBaseEdge('D', (-2147483647 - 1), 0x80000000u, 1,
                              "actual WARP Draw records INT32_MIN base as unsigned start");
        checkUnsignedBaseEdge('D', -1, 0xFFFFFFFFu, 1,
                              "actual WARP Draw records -1 base as unsigned start");
        checkUnsignedBaseEdge('N', (-2147483647 - 1), 0x80000000u, 4,
                              "actual WARP DrawInstanced records INT32_MIN base as unsigned start");
        checkUnsignedBaseEdge('N', -1, 0xFFFFFFFFu, 4,
                              "actual WARP DrawInstanced records -1 base as unsigned start");

        edvr::VScreenForwardingTestInput forwardSkipWithCurve = forwardSkip;
        forwardSkipWithCurve.curveThisDraw = true;
        auto skipCurveResult = runForwardingPair(forwardSkipWithCurve,
            "actual WARP kSkip clears an armed curve marker", 0, 0);
        edvr::ForwardingObservation skipCurveFact{};
        okay &= check(readForwarding(skipCurveResult, &skipCurveFact) &&
                          fwRead(skipCurveFact.curveThisDrawBeforeSkipClear, true) &&
                          !skipCurveResult.curveThisDrawAfter &&
                          fwSkipped(skipCurveFact.seedDiagnostics),
                      "skip curve mutation is observed but outside the supported replay domain");

        edvr::VScreenForwardingTestInput forwardBlocked{};
        forwardBlocked.issueBlockedEntry = true;
        auto blockedResult = runForwardingPair(forwardBlocked,
            "actual WARP blocked entry takes early-return path", 0, 0);
        okay &= check(checkForwardAction(blockedResult, forwardBlocked,
                              static_cast<std::uint16_t>(ActionId::kSwallowOriginal),
                              ActionOutcome::Declined, 0),
                      "blocked early return emits one declined swallow action with exact tuple");
        edvr::ForwardingObservation blockedFact{};
        okay &= check(readForwarding(blockedResult, &blockedFact) &&
                          fwRead(blockedFact.issueBlockedEntry, true) &&
                          fwSkipped(blockedFact.objectProbeLedgerOn) &&
                          fwSkipped(blockedFact.uiDepthThisDraw) &&
                          fwSkipped(blockedFact.seedDiagnostics) &&
                          fwSkipped(blockedFact.originalCallReturned),
                      "blocked entry leaves scope inputs genuinely unreached");

        edvr::VScreenForwardingTestInput forwardLedger{};
        forwardLedger.objectProbeLedgerOn = true;
        auto ledgerResult = runForwardingPair(forwardLedger,
            "actual WARP active ledger remains outside the supported plan", 1, 0);
        edvr::ForwardingObservation ledgerFact{};
        okay &= check(readForwarding(ledgerResult, &ledgerFact) &&
                          fwRead(ledgerFact.objectProbeLedgerOn, true) &&
                          fwRead(ledgerFact.originalCallReturned, true),
                      "active owner ledger is recorded as an unsupported raw gate");

        edvr::VScreenForwardingTestInput forwardCrisp{};
        forwardCrisp.changeCrispPendingAfter = true;
        forwardCrisp.crispPendingAfter = true;
        auto crispResult = runForwardingPair(forwardCrisp,
            "actual WARP callback mutates crisp-pending checkpoint", 1, 0);
        edvr::ForwardingObservation crispFact{};
        okay &= check(readForwarding(crispResult, &crispFact) &&
                          fwRead(crispFact.crispPendingAfterOriginal, true) &&
                          !crispResult.crispPendingAfter,
                      "crisp gate records true before its safe no-pending decline clears it");

        edvr::VScreenForwardingTestInput forwardPostBlocked{};
        forwardPostBlocked.changeIssueBlockedAfter = true;
        forwardPostBlocked.issueBlockedAfter = true;
        forwardPostBlocked.changePlanetPendingAfter = true;
        forwardPostBlocked.planetPendingAfter = true;
        auto postBlockedResult = runForwardingPair(forwardPostBlocked,
            "actual WARP callback mutates post-original issue block", 1, 0);
        edvr::ForwardingObservation postBlockedFact{};
        okay &= check(readForwarding(postBlockedResult, &postBlockedFact) &&
                          fwRead(postBlockedFact.issueBlockedAfterOriginal, true) &&
                          fwSkipped(postBlockedFact.planetPending) &&
                          fwSkipped(postBlockedFact.planetSolarPending),
                      "post-original block ends lazily before planet checkpoints");

        edvr::VScreenForwardingTestInput forwardPlanetPrimary{};
        forwardPlanetPrimary.changePlanetPendingAfter = true;
        forwardPlanetPrimary.planetPendingAfter = true;
        forwardPlanetPrimary.planetSolarPendingAfter = true;
        auto primaryPlanetResult = runForwardingPair(forwardPlanetPrimary,
            "actual WARP callback mutates primary planet pending checkpoint", 1, 0);
        edvr::ForwardingObservation primaryPlanetFact{};
        okay &= check(readForwarding(primaryPlanetResult, &primaryPlanetFact) &&
                          fwRead(primaryPlanetFact.planetPending, true) &&
                          fwSkipped(primaryPlanetFact.planetSolarPending) &&
                          !primaryPlanetResult.planetPendingAfter &&
                          !primaryPlanetResult.planetSolarPendingAfter,
                      "primary pending short-circuits solar read and inactive-depth helper clears both");

        edvr::VScreenForwardingTestInput forwardPlanet{};
        forwardPlanet.changePlanetPendingAfter = true;
        forwardPlanet.planetSolarPendingAfter = true;
        auto planetResult = runForwardingPair(forwardPlanet,
            "actual WARP callback mutates solar pending checkpoint", 1, 0);
        edvr::ForwardingObservation planetFact{};
        okay &= check(readForwarding(planetResult, &planetFact) &&
                          fwRead(planetFact.planetPending, false) &&
                          fwRead(planetFact.planetSolarPending, true) &&
                          !planetResult.planetPendingAfter &&
                          !planetResult.planetSolarPendingAfter,
                      "solar pending is lazily consumed and cleared by inactive-depth decline");

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

        // The exposure installation owns the production shader registry. It
        // must exist before TargetSharp, resolve, and FSS register WARP shaders.
        edvr::installExposureFix(device, edvr::HookMode::CopyVptr);

        // Drive TargetSharp site 54 through the production visitor/helper on
        // WARP resources. The visitor receives the actual published interest
        // mask; these bounded visits do not claim full-ladder coverage.
        {
            constexpr std::uint64_t kTargetHash = 0x5453484152500001ull;
            constexpr std::uint64_t kWrongHash = 0x5453484152500002ull;
            const auto savedInterest = edvr::pluginRegistryDrawInterestMask();
            const bool savedSharp = edvr::detail::g_targetSharpSharp;
            const bool savedFailed = edvr::detail::g_targetSharpFailed;
            const auto savedHash = edvr::targetSharpPredicateTestConfiguredHash();
            const auto savedResourceAttempts = g_resourceAttempts;
            const auto savedVertexShaderCalls = g_getVertexShaderCalls;
            const bool savedResourceFault = g_injectResourceFault;
            const auto savedSrvMask = edvr::draw_interest::bit(
                edvr::draw_interest::InterestId::TargetSharp);
            void* priorShadow[4]{};
            ID3D11ShaderResourceView* priorActualSrvs[4]{};
            for (std::uint32_t i = 0; i < 4; ++i) {
                priorShadow[i] = edvr::bindingGet(static_cast<edvr::BindSlot>(
                    static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + i));
            }
            immediate->PSGetShaderResources(0, 4, priorActualSrvs);
            ID3D11VertexShader* priorActualVs = nullptr;
            immediate->VSGetShader(&priorActualVs, nullptr, nullptr);

            const char* vsSource =
                "struct O { float4 p : SV_Position; };\n"
                "O main(uint id : SV_VertexID) { O o; "
                "o.p=float4((id==0)?0.0:1.0,0.0,0.0,1.0); return o; }\n";
            const auto vsCode = compileTestVertexShader(vsSource, "target-sharp-warp");
            const char* unknownVsSource =
                "struct O { float4 p : SV_Position; };\n"
                "O main(uint id : SV_VertexID) { O o; "
                "o.p=float4(0.0,(id==0)?0.0:1.0,0.0,1.0); return o; }\n";
            const auto unknownVsCode = compileTestVertexShader(
                unknownVsSource, "target-sharp-warp-unknown");
            ComPtr<ID3D11VertexShader> targetVs;
            ComPtr<ID3D11VertexShader> unknownVs;
            okay &= check(vsCode && SUCCEEDED(device->CreateVertexShader(
                              vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                              nullptr, &targetVs)) && targetVs,
                          "WARP creates real TargetSharp vertex shader");
            okay &= check(unknownVsCode && SUCCEEDED(device->CreateVertexShader(
                              unknownVsCode->GetBufferPointer(), unknownVsCode->GetBufferSize(),
                              nullptr, &unknownVs)) && unknownVs,
                          "WARP creates unregistered TargetSharp vertex shader");
            if (targetVs) {
                edvr::registerShaderHash(targetVs.Get(), kTargetHash);
                okay &= check(edvr::lookupShaderHash(targetVs.Get()) == kTargetHash,
                              "production registry returns TargetSharp WARP hash");
            }

            edvr::VTableHook vsGetHook;
            void* originalVsGet = nullptr;
            bool hookReady = vsGetHook.attach(immediate, 128) &&
                vsGetHook.setMode(edvr::HookMode::CopyVptr) &&
                vsGetHook.replace(76, reinterpret_cast<void*>(&testGetVertexShader),
                                  &originalVsGet) && vsGetHook.commit();
            okay &= check(hookReady, "TargetSharp WARP VSGetShader counter installs");
            if (hookReady) g_realGetVertexShader =
                reinterpret_cast<GetVertexShaderFn>(originalVsGet);
            edvr::VTableHook targetResourceHook;
            const bool resourceHookReady = srv && hookResource(targetResourceHook, srv);
            okay &= check(resourceHookReady,
                          "TargetSharp WARP GetResource query counter installs");

            if (srv) immediate->PSSetShaderResources(0, 1, &srv);
            for (std::uint32_t i = 0; i < 4; ++i)
                edvr::bindingSet(static_cast<edvr::BindSlot>(
                    static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + i),
                    i == 0 ? srv : nullptr);
            if (targetVs) immediate->VSSetShader(targetVs.Get(), nullptr, 0);
            {
                ComPtr<ID3D11VertexShader> actualTargetVs;
                immediate->VSGetShader(&actualTargetVs, nullptr, nullptr);
                okay &= check(actualTargetVs && actualTargetVs.Get() == targetVs.Get() &&
                                  edvr::lookupShaderHash(actualTargetVs.Get()) == kTargetHash,
                              "actual WARP VSGetShader returns the registered TargetSharp shader");
            }
            edvr::pluginRegistryConfigureDrawInterests(savedSrvMask, nullptr, 0);
            edvr::targetSharpPredicateTestSeed(true, false, kTargetHash);

            auto visitTarget = [&](char kind, std::uint32_t count,
                                   std::uint32_t eyeW, std::uint32_t eyeH,
                                   std::uint32_t renderW, std::uint32_t renderH,
                                   bool traced,
                                   edvr::VScreenPredicateTestResult* out) {
                return edvr::vScreenTargetSharpPredicateTestVisit(
                    immediate, kind, count, 1, eyeW, eyeH, renderW, renderH,
                    traced, out);
            };
            edvr::VScreenPredicateTestResult targetResult{};
            edvr::TargetSharpObservation targetFact{};
            if (hookReady) g_getVertexShaderCalls = 0;
            g_resourceAttempts = 0;
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              targetResult.siteResult.outcome == SiteOutcome::Claimed &&
                              readTargetSharp(targetResult, &targetFact) &&
                              targetFact.siteId == 54 && targetFact.kind == 21 &&
                              targetFact.handlerInvoked &&
                              fwRead(targetFact.outerSharp, true) &&
                              fwRead(targetFact.outerFailed, false) &&
                              fwRead(targetFact.helperSharp, true) &&
                              fwRead(targetFact.helperFailed, false) &&
                              fwRead(targetFact.srv0Present, true) &&
                              fwRead(targetFact.srv0Resolved, true) &&
                              fwRead(targetFact.srv0Texture2D, true) &&
                              fwRead(targetFact.srv0Width, 64) &&
                              fwRead(targetFact.srv0Height, 32) &&
                              targetFact.eyeSize.result ==
                                  edvr::holo_scrim_observation::Tri::No &&
                              fwRead(targetFact.aux1Present, false) &&
                              fwRead(targetFact.aux2Present, false) &&
                              fwRead(targetFact.aux3Present, false) &&
                              fwRead(targetFact.vsPresent, true) &&
                              fwRead(targetFact.queriedShaderHash, kTargetHash) &&
                              fwRead(targetFact.configuredShaderHash, kTargetHash) &&
                              (!hookReady || g_getVertexShaderCalls == 1) &&
                              (!resourceHookReady || g_resourceAttempts == 1),
                          "WARP TargetSharp success records actual SRV, eye, and registered VS inputs");
            if (hookReady) g_getVertexShaderCalls = 0;
            g_resourceAttempts = 0;
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, false,
                                      &targetResult) &&
                              targetResult.siteResult.outcome == SiteOutcome::Claimed &&
                              (!hookReady || g_getVertexShaderCalls == 1) &&
                              (!resourceHookReady || g_resourceAttempts == 1),
                          "TargetSharp NoTrace shares the same WARP selector and query counts");

            edvr::pluginRegistryConfigureDrawInterests(0, nullptr, 0);
            if (hookReady) g_getVertexShaderCalls = 0;
            g_resourceAttempts = 0;
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              targetResult.siteResult.outcome == SiteOutcome::NotEligible &&
                              (!hookReady || g_getVertexShaderCalls == 0) &&
                              (!resourceHookReady || g_resourceAttempts == 0) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              targetFact.siteId == 54 && targetFact.kind == 21 &&
                              !targetFact.handlerInvoked &&
                              fwSkipped(targetFact.outerSharp) &&
                              fwSkipped(targetFact.outerFailed) &&
                              fwSkipped(targetFact.helperSharp) &&
                              fwSkipped(targetFact.helperFailed) &&
                              fwSkipped(targetFact.srv0Present) &&
                              fwSkipped(targetFact.srv0Resolved) &&
                              targetFact.eyeSize.reached ==
                                  edvr::holo_scrim_observation::Tri::Unknown,
                          "published mask off records a default NotEligible fact without input reads");
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, false,
                                      &targetResult) &&
                              targetResult.siteResult.outcome == SiteOutcome::NotEligible &&
                              edvr::draw_ladder_trace::targetSharpFactCountForTest(
                                  targetResult.token) == 0,
                          "NoTrace mask-off visit remains unrecorded");

            edvr::pluginRegistryConfigureDrawInterests(savedSrvMask, nullptr, 0);
            edvr::targetSharpPredicateTestSeed(false, false, kTargetHash);
            if (hookReady) g_getVertexShaderCalls = 0;
            g_resourceAttempts = 0;
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              targetResult.siteResult.outcome == SiteOutcome::Declined &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.outerSharp, false) &&
                              fwSkipped(targetFact.outerFailed) &&
                              fwSkipped(targetFact.helperSharp) &&
                              fwSkipped(targetFact.srv0Present) &&
                              (!hookReady || g_getVertexShaderCalls == 0) &&
                              (!resourceHookReady || g_resourceAttempts == 0),
                          "outer stock gate short-circuits all helper and D3D reads");

            edvr::targetSharpPredicateTestSeed(true, true, kTargetHash);
            if (hookReady) g_getVertexShaderCalls = 0;
            g_resourceAttempts = 0;
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.outerSharp, true) &&
                              fwRead(targetFact.outerFailed, true) &&
                              fwSkipped(targetFact.helperSharp) &&
                              fwSkipped(targetFact.srv0Present) &&
                              (!resourceHookReady || g_resourceAttempts == 0),
                          "outer failed gate keeps the helper suffix lazy");

            edvr::targetSharpPredicateTestSeed(true, false, kTargetHash);
            if (hookReady) g_getVertexShaderCalls = 0;
            g_resourceAttempts = 0;
            okay &= check(visitTarget('X', 5, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.helperSharp, true) &&
                              fwSkipped(targetFact.srv0Present),
                          "draw-shape rejection leaves all binding reads skipped");

            edvr::bindingSet(edvr::BindSlot::PsSrv0, nullptr);
            ID3D11ShaderResourceView* nullTargetSrv = nullptr;
            immediate->PSSetShaderResources(0, 1, &nullTargetSrv);
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.srv0Present, false) &&
                              fwRead(targetFact.srv0Resolved, false) &&
                              fwSkipped(targetFact.srv0Texture2D) &&
                              targetFact.eyeSize.reached ==
                                  edvr::holo_scrim_observation::Tri::Unknown &&
                              fwSkipped(targetFact.aux1Present) &&
                              fwSkipped(targetFact.vsPresent),
                          "null slot zero still reaches production resolver and stops suffix");

            if (resourceHookReady) {
                immediate->PSSetShaderResources(0, 1, &srv);
                edvr::bindingSet(edvr::BindSlot::PsSrv0, srv);
                g_resourceAttempts = 0;
                g_injectResourceFault = true;
                okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                          &targetResult) &&
                                  g_resourceAttempts == 1 &&
                                  readTargetSharp(targetResult, &targetFact) &&
                                  fwRead(targetFact.srv0Present, true) &&
                                  fwRead(targetFact.srv0Resolved, false) &&
                                  fwSkipped(targetFact.srv0Texture2D) &&
                                  fwSkipped(targetFact.aux1Present) &&
                                  fwSkipped(targetFact.vsPresent),
                              "guarded WARP GetResource fault records unresolved prefix and skips suffix");
                g_resourceAttempts = 0;
                okay &= check(visitTarget('X', 6, 100, 80, 0, 0, false,
                                          &targetResult) &&
                                  g_resourceAttempts == 1 &&
                                  targetResult.siteResult.outcome == SiteOutcome::Declined &&
                                  edvr::draw_ladder_trace::targetSharpFactCountForTest(
                                      targetResult.token) == 0,
                              "NoTrace resolver fault preserves one query and emits no fact");
                g_injectResourceFault = false;
            }

            if (bufferSrv) {
                immediate->PSSetShaderResources(0, 1, &bufferSrv);
                edvr::bindingSet(edvr::BindSlot::PsSrv0, bufferSrv);
                okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                          &targetResult) &&
                                  readTargetSharp(targetResult, &targetFact) &&
                                  fwRead(targetFact.srv0Resolved, true) &&
                                  fwRead(targetFact.srv0Texture2D, false) &&
                                  fwSkipped(targetFact.srv0Width) &&
                                  targetFact.eyeSize.reached ==
                                      edvr::holo_scrim_observation::Tri::Unknown &&
                                  targetFact.eyeSize.statePresent ==
                                      edvr::holo_scrim_observation::Tri::Unknown &&
                                  fwSkipped(targetFact.aux1Present),
                              "real WARP buffer SRV stops before Texture2D dimensions and eye state");
            }
            if (srv) {
                immediate->PSSetShaderResources(0, 1, &srv);
                edvr::bindingSet(edvr::BindSlot::PsSrv0, srv);
                okay &= check(visitTarget('X', 6, 64, 32, 0, 0, true,
                                          &targetResult) &&
                                  readTargetSharp(targetResult, &targetFact) &&
                                  targetFact.eyeSize.result ==
                                      edvr::holo_scrim_observation::Tri::Yes &&
                                  fwSkipped(targetFact.aux1Present) &&
                                  fwSkipped(targetFact.vsPresent),
                              "eye-sized WARP surface exits before auxiliary slots and shader query");
                okay &= check(visitTarget('X', 6, 100, 80, 64, 32, true,
                                          &targetResult) &&
                                  readTargetSharp(targetResult, &targetFact) &&
                                  targetFact.eyeSize.result ==
                                      edvr::holo_scrim_observation::Tri::Yes,
                              "render-sized WARP surface uses the observed render dimension path");
            }

            if (srv && hookReady) {
                for (std::uint32_t slot = 1; slot <= 3; ++slot) {
                    immediate->PSSetShaderResources(slot, 1, &srv);
                    edvr::bindingSet(static_cast<edvr::BindSlot>(
                        static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + slot), srv);
                    g_getVertexShaderCalls = 0;
                    okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                              &targetResult) &&
                                      readTargetSharp(targetResult, &targetFact) &&
                                      targetFact.eyeSize.result ==
                                          edvr::holo_scrim_observation::Tri::No &&
                                      (!hookReady || g_getVertexShaderCalls == 0) &&
                                      (slot == 1
                                          ? fwRead(targetFact.aux1Present, true) &&
                                            fwSkipped(targetFact.aux2Present)
                                          : slot == 2
                                              ? fwRead(targetFact.aux1Present, false) &&
                                                fwRead(targetFact.aux2Present, true) &&
                                                fwSkipped(targetFact.aux3Present)
                                              : fwRead(targetFact.aux1Present, false) &&
                                                fwRead(targetFact.aux2Present, false) &&
                                                fwRead(targetFact.aux3Present, true)),
                                  "each occupied auxiliary SRV exits at its lazy slot read");
                    immediate->PSSetShaderResources(slot, 1, &nullTargetSrv);
                    edvr::bindingSet(static_cast<edvr::BindSlot>(
                        static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + slot), nullptr);
                }
            }

            immediate->VSSetShader(nullptr, nullptr, 0);
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.vsPresent, false) &&
                              fwSkipped(targetFact.queriedShaderHash) &&
                              fwSkipped(targetFact.configuredShaderHash),
                          "null actual vertex shader leaves hash reads skipped");
            if (unknownVs) immediate->VSSetShader(unknownVs.Get(), nullptr, 0);
            edvr::targetSharpPredicateTestSeed(true, false, kWrongHash);
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.vsPresent, true) &&
                              fwRead(targetFact.queriedShaderHash, 0) &&
                              fwRead(targetFact.configuredShaderHash, kWrongHash) &&
                              targetResult.siteResult.outcome == SiteOutcome::Declined,
                          "unregistered real WARP VS resolves to raw zero and mismatches configured pin");
            if (targetVs) immediate->VSSetShader(targetVs.Get(), nullptr, 0);
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.vsPresent, true) &&
                              fwRead(targetFact.queriedShaderHash, kTargetHash) &&
                              fwRead(targetFact.configuredShaderHash, kWrongHash) &&
                              targetResult.siteResult.outcome == SiteOutcome::Declined,
                          "registered real WARP hash mismatches the configured pin");

            if (hookReady) {
                vsGetHook.uninstall();
                g_realGetVertexShader = nullptr;
            }
            if (resourceHookReady) targetResourceHook.uninstall();
            immediate->VSSetShader(priorActualVs, nullptr, 0);
            if (priorActualVs) priorActualVs->Release();
            immediate->PSSetShaderResources(0, 4, priorActualSrvs);
            for (std::uint32_t i = 0; i < 4; ++i) {
                edvr::bindingSet(static_cast<edvr::BindSlot>(
                    static_cast<std::uint32_t>(edvr::BindSlot::PsSrv0) + i), priorShadow[i]);
                if (priorActualSrvs[i]) priorActualSrvs[i]->Release();
            }
            edvr::pluginRegistryConfigureDrawInterests(savedInterest, nullptr, 0);
            edvr::targetSharpPredicateTestSeed(savedSharp, savedFailed, savedHash);
            g_resourceAttempts = savedResourceAttempts;
            g_getVertexShaderCalls = savedVertexShaderCalls;
            g_injectResourceFault = savedResourceFault;
        }

        // Drive site 60 through the real visitor and helper. Hash lookups use
        // the production registry, populated with real WARP shaders.
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

        okay &= testFssDumpPredicate(device, immediate);
        okay &= testSunglareNomination(device, immediate);
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
