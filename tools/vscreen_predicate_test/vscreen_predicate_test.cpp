#include "vscreen_predicate_test.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <initializer_list>
#include <vector>

#include "../../src/common/system_d3d11.h"
#include "../../src/common/config.h"
#include "../../src/common/plugin_cost.h"
#include "../../src/d3d11/vscreen.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/ui_depth.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/exposure_fix.h"
#include "../../src/d3d11/basic_draw_observation.h"
#include "../../src/d3d11/eye_census_observation.h"
#include "../../src/d3d11/loader_panel_observation.h"
#include "../../src/d3d11/loader_panel.h"
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
bool readLoaderPanel(const edvr::VScreenPredicateTestResult& result,
                     edvr::LoaderPanelObservation* fact) {
    return edvr::draw_ladder_trace::loaderPanelFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readLoaderPanelFactForTest(result.token, 0, fact);
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
bool lpRead(const edvr::LoaderPanelRead<T>& read, U expected) {
    return read.reached && read.known && read.value == static_cast<T>(expected);
}

template <class T>
bool lpSkipped(const edvr::LoaderPanelRead<T>& read) {
    return !read.reached && !read.known && read.value == T{};
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

bool nvForwardActionsMatch(const VScreenPanelDistanceApiTestResult& result,
                           const VScreenPanelDistanceApiTestInput& input,
                           bool replace, bool panel, bool skip) {
    using namespace draw_ladder;
    const std::uint32_t literalCount = panel ? 6u : 240u;
    const bool literalInput = input.kind == 'X' && input.drawCount == literalCount &&
        input.drawInstances == 1 && input.drawArgs.start == 7 &&
        input.drawArgs.base == -3 && input.drawArgs.startInstance == 11;
    const ActionId drawIds[] = {ActionId::kDrawBegin, ActionId::kOriginalDraw,
                                ActionId::kDrawEnd};
    const ActionPhase drawPhases[] = {ActionPhase::Begin, ActionPhase::Issue,
                                      ActionPhase::End};
    const ActionOutcome drawOutcomes[] = {ActionOutcome::Applied,
        ActionOutcome::Applied, ActionOutcome::Applied};
    const ActionId replaceIds[] = {ActionId::kDrawBegin, ActionId::kReplaceDraw,
        ActionId::kOriginalDraw, ActionId::kReplaceDraw, ActionId::kDrawEnd};
    const ActionPhase replacePhases[] = {ActionPhase::Begin, ActionPhase::Begin,
        ActionPhase::Issue, ActionPhase::End, ActionPhase::End};
    const ActionOutcome replaceOutcomes[] = {ActionOutcome::Applied,
        ActionOutcome::Attempted, ActionOutcome::Applied, ActionOutcome::Applied,
        ActionOutcome::Applied};
    const ActionId panelIds[] = {ActionId::kDrawBegin, ActionId::kReplaceDraw,
        ActionId::kOriginalDraw, ActionId::kReplaceDraw,
        ActionId::kPanelConstantBufferRestore, ActionId::kDrawEnd};
    const ActionPhase panelPhases[] = {ActionPhase::Begin, ActionPhase::Begin,
        ActionPhase::Issue, ActionPhase::End, ActionPhase::Restore, ActionPhase::End};
    const ActionOutcome panelOutcomes[] = {ActionOutcome::Applied,
        ActionOutcome::Attempted, ActionOutcome::Applied, ActionOutcome::Applied,
        ActionOutcome::Applied, ActionOutcome::Applied};
    const ActionId skipIds[] = {ActionId::kDrawBegin, ActionId::kSwallowOriginal,
                                ActionId::kDrawEnd};
    const ActionPhase skipPhases[] = {ActionPhase::Begin, ActionPhase::Issue,
                                      ActionPhase::End};
    const ActionOutcome skipOutcomes[] = {ActionOutcome::Applied,
        ActionOutcome::Applied, ActionOutcome::Applied};
    const ActionId* ids = panel ? panelIds : skip ? skipIds : replace ? replaceIds : drawIds;
    const ActionPhase* phases = panel ? panelPhases : skip ? skipPhases : replace ? replacePhases : drawPhases;
    const ActionOutcome* outcomes = panel ? panelOutcomes : skip ? skipOutcomes : replace ? replaceOutcomes : drawOutcomes;
    const std::uint16_t size = panel ? 6 : skip || !replace ? 3 : 5;
    if (!result.token.valid() || draw_ladder_trace::actionCountForTest(result.token) != size)
        return false;
    for (std::uint16_t i = 0; i < size; ++i) {
        std::uint16_t id = 0;
        ActionRecord record{};
        if (!draw_ladder_trace::readActionForTest(result.token, i, &id, &record) ||
            id != static_cast<std::uint16_t>(ids[i]) || record.phase != phases[i] ||
            record.outcome != outcomes[i] ||
            record.call != DrawCallKind::DrawIndexedInstanced ||
            record.count != literalCount || record.instances != 1 ||
            record.start != 7 || record.startInstance != 11 || record.baseVertex != -3 ||
            record.flags != (id == static_cast<std::uint16_t>(ActionId::kReplaceDraw)
                ? static_cast<std::uint16_t>(panel ? 1u : 6u) : 0u)) return false;
        if (id == static_cast<std::uint16_t>(ActionId::kOriginalDraw)) {
            if (record.issueCount != 1) return false;
        } else if (record.issueCount != 0) {
            return false;
        }
        if (id == static_cast<std::uint16_t>(ActionId::kReplaceDraw) &&
            record.flags != (panel ? 1u : 6u)) return false;
    }
    return literalInput;
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

namespace {
struct UiCaseTexture final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};
struct UiCaseRig final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    UiCaseTexture eye, frame, panel;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depthState;
};
constexpr UINT kUiEyeW = 64, kUiEyeH = 64, kUiPanelW = 1920, kUiPanelH = 1080;
// Four cold texture-view inspections precede door admission: the eye target
// in kStateSnapshot, SRV0 in uiDepthOnEyeDraw, the UI target-kind cache, and
// SRV0's panel-size cache. Independent caches resolve each view separately;
// each bindingResolve records Core GetResource(112), GetType(113), and
// Texture2DGetDesc(115). The depth probe's unlearned SRV and zero VS hash
// decline before dsvIsSceneDepth. These twelve reads also occur on refusal.
constexpr unsigned long long kUiSharedDescriptorReads = 4ull * 3ull;
// A captured draw also resolves RTV0 for its replay dimensions, before the
// classifier. The old intro-probe size cache was retired. This inspection
// uses the same Core112/113/115 sites, so it adds reads without new mask bits.
constexpr unsigned long long kCapturedDrawDescriptorReads = 3ull;
constexpr unsigned long long kUiSharedDescriptorMask =
    (1ull << (112 - 64)) | (1ull << (113 - 64)) | (1ull << (115 - 64));
constexpr unsigned long long kUiTransactionMask = ((1ull << 14) - 1) << (83 - 64);
// The collector checks totals and site presence, not runtime note order.
// This is the attributed Core subset, not all D3D calls in the UI route.
bool uiCaseSucceeded(HRESULT value,const char* stage) {
    if (SUCCEEDED(value)) return true;
    std::fprintf(stderr,"UI_REISSUE_SETUP_FAIL stage=%s hr=%08lx\n",stage,
        static_cast<unsigned long>(value));
    return false;
}
bool uiCaseFailed(const char* stage) {
    std::fprintf(stderr,"UI_REISSUE_SETUP_FAIL stage=%s error=%lu\n",stage,
        static_cast<unsigned long>(GetLastError()));
    return false;
}
constexpr char kUiCaseShader[] = R"HLSL(
struct V { float4 pos : SV_Position; };
V vsMain(uint id : SV_VertexID) {
    V o;
    o.pos = float4(id == 2 ? 3.0 : -1.0,
                   id == 1 ? 3.0 : -1.0, 0.5, 1.0);
    return o;
}
float4 psMain(V input) : SV_Target { return float4(0.5, 0.0, 0.0, 0.5); }
)HLSL";

bool makeUiCaseTexture(ID3D11Device* device, UINT w, UINT h, UINT bind,
                       UiCaseTexture* out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    if (!uiCaseSucceeded(device->CreateTexture2D(&d, nullptr, &out->texture),"texture")) return false;
    if ((bind & D3D11_BIND_RENDER_TARGET) &&
        !uiCaseSucceeded(device->CreateRenderTargetView(out->texture.Get(), nullptr, &out->rtv),"texture-rtv")) return false;
    if ((bind & D3D11_BIND_SHADER_RESOURCE) &&
        !uiCaseSucceeded(device->CreateShaderResourceView(out->texture.Get(), nullptr, &out->srv),"texture-srv")) return false;
    return true;
}

bool setupUiCaseRig(UiCaseRig* r) {
    D3D_FEATURE_LEVEL level{};
    const auto create = edvr::systemD3D11CreateDevice();
    if (!create) return uiCaseFailed("system-create-accessor");
    if (!uiCaseSucceeded(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &r->device, &level, &r->context),"warp-device")) return false;
    if (!makeUiCaseTexture(r->device.Get(), kUiEyeW, kUiEyeH,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &r->eye) ||
        !makeUiCaseTexture(r->device.Get(), kUiEyeW, kUiEyeH,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &r->frame) ||
        !makeUiCaseTexture(r->device.Get(), kUiPanelW, kUiPanelH,
            D3D11_BIND_SHADER_RESOURCE, &r->panel)) return false;
    ComPtr<ID3DBlob> vb, pb, errors;
    if (!uiCaseSucceeded(D3DCompile(kUiCaseShader, sizeof(kUiCaseShader)-1, "ui-case", nullptr, nullptr,
            "vsMain", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vb, &errors),"compile-vs")) return false;
    errors.Reset();
    if (!uiCaseSucceeded(D3DCompile(kUiCaseShader, sizeof(kUiCaseShader)-1, "ui-case", nullptr, nullptr,
            "psMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pb, &errors),"compile-ps") ||
        !uiCaseSucceeded(r->device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(),
                                               nullptr, &r->vs),"create-vs") ||
        !uiCaseSucceeded(r->device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(),
                                              nullptr, &r->ps),"create-ps")) return false;
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = kUiEyeW; dd.Height = kUiEyeH; dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (!uiCaseSucceeded(r->device->CreateTexture2D(&dd, nullptr, &r->depth),"depth-texture") ||
        !uiCaseSucceeded(r->device->CreateDepthStencilView(r->depth.Get(), &dsvDesc, &r->dsv),"depth-dsv")) return false;
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE; rs.ScissorEnable = TRUE;
    if (!uiCaseSucceeded(r->device->CreateRasterizerState(&rs, &r->raster),"raster-state")) return false;
    D3D11_BLEND_DESC bd{};
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = TRUE; b.SrcBlend = D3D11_BLEND_ONE;
    b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D11_BLEND_ONE; b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.BlendOpAlpha = D3D11_BLEND_OP_ADD; b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!uiCaseSucceeded(r->device->CreateBlendState(&bd, &r->blend),"blend-state")) return false;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    if (!uiCaseSucceeded(r->device->CreateDepthStencilState(&ds, &r->depthState),"depth-state")) return false;
    const float blue[4] = {0,0,1,1};
    r->context->ClearRenderTargetView(r->eye.rtv.Get(), blue);
    r->context->ClearRenderTargetView(r->frame.rtv.Get(), blue);
    r->context->ClearDepthStencilView(r->dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* target = r->eye.rtv.Get();
    r->context->OMSetRenderTargets(1, &target, r->dsv.Get());
    const D3D11_VIEWPORT vp{0,0,static_cast<float>(kUiEyeW),static_cast<float>(kUiEyeH),0,1};
    const D3D11_RECT sc{0,0,static_cast<LONG>(kUiEyeW),static_cast<LONG>(kUiEyeH)};
    r->context->RSSetViewports(1, &vp); r->context->RSSetScissorRects(1, &sc);
    r->context->RSSetState(r->raster.Get());
    const float factor[4] = {};
    r->context->OMSetBlendState(r->blend.Get(), factor, 0xFFFFFFFFu);
    r->context->OMSetDepthStencilState(r->depthState.Get(), 0);
    r->context->VSSetShader(r->vs.Get(), nullptr, 0);
    r->context->PSSetShader(r->ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* panel = r->panel.srv.Get();
    r->context->PSSetShaderResources(0, 1, &panel);
    r->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    edvr::bindingSet(edvr::BindSlot::Rtv0, r->eye.rtv.Get());
    edvr::bindingSet(edvr::BindSlot::Dsv0, r->dsv.Get());
    edvr::bindingSet(edvr::BindSlot::PsSrv0, r->panel.srv.Get());
    edvr::bindingSetShader(edvr::BindSlot::Vs, r->vs.Get(), 0);
    edvr::bindingSetShader(edvr::BindSlot::Ps, r->ps.Get(), 0);
    const std::wstring systemD3d11 = edvr::systemD3D11Path();
    const std::vector<std::wstring> mappedD3d11 = edvr::mappedD3D11Paths();
    bool systemOnly = !systemD3d11.empty() && !mappedD3d11.empty();
    for (const std::wstring& path : mappedD3d11)
        if (_wcsicmp(path.c_str(), systemD3d11.c_str()) != 0) systemOnly = false;
    if (!systemOnly)
        std::fprintf(stderr, "ui-reissue child: D3D11 is not exclusively mapped from System32\n");
    return systemOnly;
}

struct UiCaseSnapshot {
    void* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend; ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster; ID3D11VertexShader* vs;
    ID3D11PixelShader* ps; ID3D11ShaderResourceView* srv;
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* csCb = nullptr;
    ID3D11ShaderResourceView* csSrvs[3]{};
    ID3D11UnorderedAccessView* csUav = nullptr;
    UINT viewportCount; D3D11_VIEWPORT viewport;
    UINT scissorCount; D3D11_RECT scissor;
    float factor[4]; UINT mask; UINT stencilRef;
};
UiCaseSnapshot takeUiCaseSnapshot(ID3D11DeviceContext* c) {
    UiCaseSnapshot s{};
    ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* ds = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rt, &ds); s.dsv = ds;
    for (UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) {
        s.rtvs[i]=rt[i]; if (rt[i]) rt[i]->Release();
    }
    if (ds) ds->Release();
    c->OMGetBlendState(&s.blend, s.factor, &s.mask);
    c->OMGetDepthStencilState(&s.depth, &s.stencilRef); c->RSGetState(&s.raster);
    c->VSGetShader(&s.vs, nullptr, nullptr); c->PSGetShader(&s.ps, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &s.srv);
    c->CSGetShader(&s.cs,nullptr,nullptr);
    c->CSGetConstantBuffers(0,1,&s.csCb);
    c->CSGetShaderResources(0,3,s.csSrvs);
    c->CSGetUnorderedAccessViews(0,1,&s.csUav);
    s.viewportCount = 1; c->RSGetViewports(&s.viewportCount, &s.viewport);
    s.scissorCount = 1; c->RSGetScissorRects(&s.scissorCount, &s.scissor);
    if (s.blend) s.blend->Release(); if (s.depth) s.depth->Release();
    if (s.raster) s.raster->Release(); if (s.vs) s.vs->Release();
    if (s.ps) s.ps->Release(); if (s.srv) s.srv->Release();
    if (s.cs) s.cs->Release(); if (s.csCb) s.csCb->Release();
    for (ID3D11ShaderResourceView* srv : s.csSrvs) if (srv) srv->Release();
    if (s.csUav) s.csUav->Release();
    return s;
}
bool sameUiCaseSnapshot(const UiCaseSnapshot& a, const UiCaseSnapshot& b) {
    return std::memcmp(a.rtvs,b.rtvs,sizeof(a.rtvs))==0 && a.dsv == b.dsv && a.blend == b.blend &&
        a.depth == b.depth && a.raster == b.raster && a.vs == b.vs &&
        a.ps == b.ps && a.srv == b.srv && a.viewportCount == b.viewportCount &&
        a.cs == b.cs && a.csCb == b.csCb &&
        std::memcmp(a.csSrvs,b.csSrvs,sizeof(a.csSrvs))==0 && a.csUav == b.csUav &&
        std::memcmp(&a.viewport, &b.viewport, sizeof(a.viewport)) == 0 &&
        a.scissorCount == b.scissorCount &&
        std::memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) == 0 &&
        std::memcmp(a.factor, b.factor, sizeof(a.factor)) == 0 &&
        a.mask == b.mask && a.stencilRef == b.stencilRef;
}

bool readUiCaseSurface(ID3D11Device* dev, ID3D11DeviceContext* c, ID3D11Texture2D* src,
                       UINT bytesPerPixel, std::vector<std::uint8_t>* bytes,
                       UINT* width, UINT* height, std::uint64_t* fingerprint) {
    if (!dev || !c || !src || !bytes || !width || !height || !fingerprint ||
        (bytesPerPixel != 4)) return false;
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
    if (!d.Width || !d.Height || d.SampleDesc.Count != 1 ||
        d.Width > 4096 || d.Height > 4096) return false;
    D3D11_TEXTURE2D_DESC stagingDesc=d;
    if (stagingDesc.Format==DXGI_FORMAT_R32_TYPELESS) stagingDesc.Format=DXGI_FORMAT_R32_FLOAT;
    stagingDesc.BindFlags = 0; stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&stagingDesc, nullptr, &staging))) return false;
    c->CopyResource(staging.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const std::size_t rowBytes = static_cast<std::size_t>(d.Width) * bytesPerPixel;
    if (m.RowPitch < rowBytes) { c->Unmap(staging.Get(), 0); return false; }
    bytes->resize(rowBytes * d.Height);
    std::uint64_t hash = 14695981039346656037ull;
    for (UINT y = 0; y < d.Height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(m.pData) +
            static_cast<std::size_t>(y) * m.RowPitch;
        std::memcpy(bytes->data() + static_cast<std::size_t>(y) * rowBytes, row, rowBytes);
        for (std::size_t x = 0; x < rowBytes; ++x) {
            hash ^= row[x]; hash *= 1099511628211ull;
        }
    }
    c->Unmap(staging.Get(), 0);
    *width = d.Width; *height = d.Height; *fingerprint = hash;
    return true;
}

bool literalUiReissueActions(const edvr::VScreenPanelDistanceApiTestResult& result) {
    using namespace edvr::draw_ladder;
    struct E { ActionId id; ActionPhase phase; ActionOutcome outcome; std::uint16_t issues, flags; };
    constexpr E expected[] = {
        {ActionId::kDrawBegin,ActionPhase::Begin,ActionOutcome::Applied,0,0},
        {ActionId::kUiLayerDraw,ActionPhase::Begin,ActionOutcome::Applied,0,0},
        {ActionId::kOriginalDraw,ActionPhase::Issue,ActionOutcome::Applied,1,0},
        {ActionId::kUiLayerDraw,ActionPhase::End,ActionOutcome::Applied,0,0},
        {ActionId::kSecondUiDraw,ActionPhase::Begin,ActionOutcome::Declined,0,1},
        {ActionId::kSecondUiDraw,ActionPhase::Begin,ActionOutcome::Applied,0,2},
        {ActionId::kSecondUiDraw,ActionPhase::Issue,ActionOutcome::Applied,1,2},
        {ActionId::kSecondUiDraw,ActionPhase::End,ActionOutcome::Applied,0,2},
        {ActionId::kDrawEnd,ActionPhase::End,ActionOutcome::Applied,0,0},
    };
    if (!result.token.valid() || draw_ladder_trace::actionCountForTest(result.token) != 9) return false;
    for (std::uint16_t i = 0; i < 9; ++i) {
        std::uint16_t id = 0; ActionRecord a{};
        if (!draw_ladder_trace::readActionForTest(result.token, i, &id, &a) ||
            id != static_cast<std::uint16_t>(expected[i].id) ||
            a.phase != expected[i].phase || a.outcome != expected[i].outcome ||
            a.call != DrawCallKind::Draw || a.issueCount != expected[i].issues ||
            a.flags != expected[i].flags || a.count != 3 || a.instances != 1 ||
            a.start != 0 || a.baseVertex != 0 || a.startInstance != 0) return false;
    }
    return true;
}

bool literalUiRefusalActions(const edvr::VScreenPanelDistanceApiTestResult& result) {
    using namespace edvr::draw_ladder;
    struct E { ActionId id; ActionPhase phase; ActionOutcome outcome; std::uint16_t issues; };
    constexpr E expected[] = {
        {ActionId::kDrawBegin,ActionPhase::Begin,ActionOutcome::Applied,0},
        {ActionId::kOriginalDraw,ActionPhase::Issue,ActionOutcome::Applied,1},
        {ActionId::kDrawEnd,ActionPhase::End,ActionOutcome::Applied,0},
    };
    if (!result.token.valid() || draw_ladder_trace::actionCountForTest(result.token) != 3) return false;
    for (std::uint16_t i=0;i<3;++i) {
        std::uint16_t id=0; ActionRecord a{};
        if (!draw_ladder_trace::readActionForTest(result.token,i,&id,&a) ||
            id!=static_cast<std::uint16_t>(expected[i].id) ||
            a.phase!=expected[i].phase || a.outcome!=expected[i].outcome ||
            a.call!=DrawCallKind::Draw || a.issueCount!=expected[i].issues || a.flags!=0 ||
            a.count!=3 || a.instances!=1 || a.start!=0 || a.baseVertex!=0 || a.startInstance!=0)
            return false;
    }
    return true;
}

bool runUiReissueChild(bool traceEnabled, bool apiSample, bool refusal) {
    UiCaseRig rig{};
    if (!setupUiCaseRig(&rig)) return uiCaseFailed("rig-setup");
    struct Cleanup final {
        std::wstring tracePath;
        ID3D11DeviceContext* context = nullptr;
        bool done = false;
        void run() {
            if (done) return;
            done = true;
            edvr::uiLayerPredicateTestSetTemporalInput({});
            edvrPluginCostShutdown();
            edvr::uiLayerShutdown();
            edvr::uiDepthShutdown();
            edvr::draw_ladder_trace::shutdown();
            for (std::size_t i = 0; i < static_cast<std::size_t>(edvr::BindSlot::Count); ++i)
                edvr::detail::g_bindingSlots[i] = edvr::detail::BindingSlot{};
            if (context) context->ClearState();
            if (!tracePath.empty()) DeleteFileW(tracePath.c_str());
        }
        ~Cleanup() { run(); }
    } cleanup;
    cleanup.context = rig.context.Get();
    auto& cfg = edvr::Config::get();
    cfg.set("fix.ui_quality", "100"); cfg.set("fix.temporal_aa", "dlss");
    cfg.set("advanced.temporal_aa_jitter_sign", "as_is");
    cfg.set("advanced.temporal_aa_jitter_lag", "0");
    cfg.set("advanced.temporal_aa_debug", "off");
    cfg.set("experimental.on_foot_maps_sharp", "off");
    edvr::uiDepthConfigure(cfg); edvr::uiLayerConfigure(cfg);
    edvr::uiLayerFrameBoundary(rig.context.Get());
    const int registeredEye=edvr::uiDepthEyeOfTarget(rig.eye.texture.Get(), kUiEyeW, kUiEyeH,
            DXGI_FORMAT_R8G8B8A8_UNORM);
    if (registeredEye != 0) {
        std::fprintf(stderr,"UI_REISSUE_SETUP_FAIL stage=register-eye eye=%d depthOn=%u depthStoodDown=%u\n",
            registeredEye,edvr::detail::g_uiDepthOn?1u:0u,edvr::detail::g_uiDepthStoodDown?1u:0u);
        return false;
    }
    constexpr std::uint64_t priorSeq = 7, drawSeq = 8;
    edvr::uiLayerNoteSubmitted(priorSeq, 0, rig.eye.texture.Get());
    edvr::uiLayerNoteTemporal(priorSeq, 0, rig.frame.texture.Get());
    if (!refusal)
        edvr::uiLayerDoorSeen(priorSeq, 0, rig.frame.texture.Get());
    wchar_t temp[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, temp)) return uiCaseFailed("temp-path");
    const std::wstring tracePath = std::wstring(temp) + L"edvr_gfx_ui_reissue_" +
        std::to_wstring(GetCurrentProcessId()) + L".log";
    cleanup.tracePath = tracePath;
    if (traceEnabled) {
        HANDLE traceFile=CreateFileW(tracePath.c_str(),GENERIC_WRITE,0,nullptr,
            CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        if (traceFile==INVALID_HANDLE_VALUE) return uiCaseFailed("create-trace-file");
        CloseHandle(traceFile);
    }
    if (traceEnabled && !armCapture(tracePath)) return uiCaseFailed("arm-trace");
    edvrPluginCostShutdown();
    LARGE_INTEGER hz{};
    if (!QueryPerformanceFrequency(&hz) || hz.QuadPart <= 0) return uiCaseFailed("qpc-frequency");
    edvrPluginCostConfigure(1u, static_cast<std::uint64_t>(hz.QuadPart));
    edvrPluginCostSetOwnerContext(rig.context.Get());
    EdvrPluginCostWindowV1 discard{};
    edvrPluginCostFrameBoundary(0,0,0,apiSample ? 1 : 0,0,&discard);
    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth=16; cbd.Usage=D3D11_USAGE_DEFAULT;
    cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> compositeCb, ourCb;
    if (!uiCaseSucceeded(rig.device->CreateBuffer(&cbd,nullptr,&compositeCb),"host-cb") ||
        !uiCaseSucceeded(rig.device->CreateBuffer(&cbd,nullptr,&ourCb),"override-cb")) return false;
    VScreenPanelDistanceApiTestInput in{};
    in.context=rig.context.Get(); in.ownerContext=rig.context.Get();
    in.traceEnabled=traceEnabled; in.distanceEnabled=false; in.fullClassifier=true;
    // The shared seam validates its CB shadow even with distance disabled.
    in.shadowBytes=cbd.ByteWidth; in.distanceIndex=0;
    in.composedUi=true; in.issueRealDraw=true; in.uiTemporalInput=true;
    in.expectUiRedirect=!refusal; in.panelSrv=rig.panel.srv.Get();
    in.eyeRtv=rig.eye.rtv.Get(); in.hostDsv=rig.dsv.Get();
    in.compositeCb=compositeCb.Get(); in.ourCb=ourCb.Get();
    in.kind='D'; in.drawCount=3; in.drawInstances=1; in.drawArgs={0,0,0};
    in.uiEyeWidth=kUiEyeW; in.uiEyeHeight=kUiEyeH; in.uiSequence=drawSeq;
    const std::size_t shadowCount=static_cast<std::size_t>(edvr::BindSlot::Count);
    std::vector<edvr::detail::BindingSlot> shadowsBefore(shadowCount);
    for (std::size_t i=0;i<shadowCount;++i)
        shadowsBefore[i]=edvr::detail::g_bindingSlots[i];
    const UiCaseSnapshot before=takeUiCaseSnapshot(rig.context.Get());
    VScreenPanelDistanceApiTestResult result{};
    const bool ran=vScreenPanelDistanceApiTransactionTest(in,&result);
    bool shadowsRestored=true;
    for (std::size_t i=0;i<shadowCount;++i) {
        const auto& a=shadowsBefore[i]; const auto& b=edvr::detail::g_bindingSlots[i];
        shadowsRestored=shadowsRestored && a.ptr==b.ptr && a.gen==b.gen && a.hash==b.hash;
    }
    EdvrPluginCostWindowV1 report{}; bool completed=false;
    for (std::uint32_t f=1; f<=1800; ++f)
        completed=edvrPluginCostFrameBoundary(f,0,apiSample?1:0,apiSample?1:0,0,&report)!=0;
    const auto& core=report.owners[static_cast<std::uint8_t>(edvr::plugin_cost::Owner::Core)];
    const auto reads=core.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::ReadQuery)];
    const auto states=core.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::State)];
    const bool expectUiApi=apiSample && !refusal;
    const bool costs=completed && report.version==1 &&
        report.completedApiSampleFrames==(apiSample?1800u:0u) &&
        reads==(apiSample?kUiSharedDescriptorReads+
            (traceEnabled?kCapturedDrawDescriptorReads:0ull)+(expectUiApi?6ull:0ull):0ull) &&
        states==(expectUiApi?8u:0u) && core.apiSiteMask[0]==0 &&
        core.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::Work)]==0 &&
        core.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::Transfer)]==0 &&
        core.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::Instrumentation)]==0 &&
        (core.apiSiteMask[1]&kUiTransactionMask)==(expectUiApi?kUiTransactionMask:0ull) &&
        core.apiSiteMask[1]==((apiSample?kUiSharedDescriptorMask:0ull) |
            (expectUiApi?kUiTransactionMask:0ull));
    const bool actions=traceEnabled ? (refusal ? literalUiRefusalActions(result) :
        literalUiReissueActions(result)) :
        draw_ladder_trace::actionCountForTest(result.token)==0;
    const std::uint32_t region[4]={0,0,kUiEyeW,kUiEyeH};
    const float uv[4]={0,0,1,1};
    ComPtr<ID3D11Texture2D> out;
    out.Attach(edvr::uiLayerComposite(drawSeq,0,rig.frame.texture.Get(),region,uv));
    const UiCaseSnapshot after=takeUiCaseSnapshot(rig.context.Get());
    rig.context->ClearState();
    std::vector<std::uint8_t> hostBytes, composedBytes, depthBytes;
    UINT hostW=0,hostH=0,composedW=0,composedH=0,depthW=0,depthH=0;
    std::uint64_t hostHash=0,composedHash=0,depthHash=0;
    const bool hostRead=readUiCaseSurface(rig.device.Get(),rig.context.Get(),rig.eye.texture.Get(),4,
        &hostBytes,&hostW,&hostH,&hostHash);
    const bool compositeRead=refusal ? !out : out &&
        readUiCaseSurface(rig.device.Get(),rig.context.Get(),out.Get(),4,
            &composedBytes,&composedW,&composedH,&composedHash);
    const bool depthRead=readUiCaseSurface(rig.device.Get(),rig.context.Get(),rig.depth.Get(),4,
        &depthBytes,&depthW,&depthH,&depthHash);
    const bool pixels=hostRead && compositeRead && depthRead;
    bool hostUntouched=pixels && hostW==kUiEyeW && hostH==kUiEyeH &&
        hostBytes.size()==static_cast<std::size_t>(kUiEyeW)*kUiEyeH*4;
    std::size_t composedOverlayPixels=0, hostDrawPixels=0, depthWrittenPixels=0;
    bool hostDrawFull=pixels && hostBytes.size()==static_cast<std::size_t>(kUiEyeW)*kUiEyeH*4;
    for (std::size_t i=0; hostUntouched && i<hostBytes.size(); i+=4)
        hostUntouched=hostBytes[i]==0 && hostBytes[i+1]==0 && hostBytes[i+2]>=250 && hostBytes[i+3]>=250;
    for (std::size_t i=0; hostDrawFull && i<hostBytes.size(); i+=4)
        if (hostBytes[i]>=120 && hostBytes[i]<=136 && hostBytes[i+1]<=2 &&
            hostBytes[i+2]>=120 && hostBytes[i+2]<=136 && hostBytes[i+3]>=250) ++hostDrawPixels;
    bool composedMatches=!refusal && pixels && composedBytes.size()==
        static_cast<std::size_t>(kUiEyeW)*kUiEyeH*4;
    if (composedMatches) {
        for (std::size_t i=0;i<composedBytes.size();i+=4)
            if (composedBytes[i]>=120 && composedBytes[i]<=136 && composedBytes[i+1]<=2 &&
                composedBytes[i+2]>=120 && composedBytes[i+2]<=136 && composedBytes[i+3]>=250)
                ++composedOverlayPixels;
    }
    if (pixels && depthBytes.size()==static_cast<std::size_t>(kUiEyeW)*kUiEyeH*sizeof(float)) {
        for (std::size_t i=0;i<depthBytes.size();i+=sizeof(float)) {
            float sample=1.0f; std::memcpy(&sample,depthBytes.data()+i,sizeof(sample));
            if (std::abs(sample-0.5f)<1.0e-6f) ++depthWrittenPixels;
        }
    }
    const bool pixelsMatch=pixels && depthW==kUiEyeW && depthH==kUiEyeH &&
        (refusal ? hostDrawPixels==static_cast<std::size_t>(kUiEyeW)*kUiEyeH :
            hostUntouched && composedW==kUiEyeW && composedH==kUiEyeH && composedMatches &&
            composedOverlayPixels==static_cast<std::size_t>(kUiEyeW)*kUiEyeH) &&
        depthWrittenPixels==static_cast<std::size_t>(kUiEyeW)*kUiEyeH;
    const bool pass=ran && result.winner==static_cast<std::int16_t>(edvr::draw_ladder::SiteId::kEyeNoDistanceNone) &&
        result.verdict==static_cast<std::int16_t>(edvr::draw_ladder::VerdictOrdinal::kNone) &&
        result.originalDrawCalls==(refusal?1u:2u) && result.realDrawCallbacks &&
        result.realDrawCallbackCount==(refusal?1u:2u) &&
        result.drawArgumentsValid && result.drawTargetsValid &&
        (refusal ? result.drawTargetCount[0]==1 && result.drawTargets[0]==in.eyeRtv &&
            result.drawDepthTargets[0]==in.hostDsv : result.drawTargetCount[0]==1 &&
            result.drawTargets[0]!=in.eyeRtv && result.drawDepthTargets[0]!=nullptr &&
            result.drawDepthTargets[0]!=in.hostDsv && result.drawTargetCount[1]==0 &&
            result.drawDepthTargets[1]==in.hostDsv) &&
        (refusal ? !out : out!=nullptr) &&
        sameUiCaseSnapshot(before,after) && shadowsRestored && actions && costs && pixelsMatch;
    if (!pass) {
        std::fprintf(stderr,"UI_REISSUE_FAIL trace=%u sample=%u refuse=%u ran=%u winner=%d verdict=%d args=%u hoststate=%u shadows=%u actionmatch=%u costmatch=%u pixelmatch=%u calls=%u cb=%u cbstate=%u read=%llu state=%llu mask=%llx host=%llx out=%llx depth=%llx dims=%ux%u/%ux%u/%ux%u overlay=%llu hostdraw=%llu depthhit=%llu actions=%u\n",
            traceEnabled?1u:0u,apiSample?1u:0u,refusal?1u:0u,ran?1u:0u,
            static_cast<int>(result.winner),static_cast<int>(result.verdict),
            result.drawArgumentsValid?1u:0u,sameUiCaseSnapshot(before,after)?1u:0u,
            shadowsRestored?1u:0u,actions?1u:0u,costs?1u:0u,pixelsMatch?1u:0u,
            result.originalDrawCalls,
            result.realDrawCallbackCount,result.drawTargetsValid?1u:0u,
            static_cast<unsigned long long>(reads),static_cast<unsigned long long>(states),
            static_cast<unsigned long long>(core.apiSiteMask[1]),
            static_cast<unsigned long long>(hostHash),static_cast<unsigned long long>(composedHash),
            static_cast<unsigned long long>(depthHash),hostW,hostH,composedW,composedH,depthW,depthH,
            static_cast<unsigned long long>(composedOverlayPixels),
            static_cast<unsigned long long>(hostDrawPixels),
            static_cast<unsigned long long>(depthWrittenPixels),
            result.token.valid()?draw_ladder_trace::actionCountForTest(result.token):0u);
        cleanup.run();
        return false;
    }
    cleanup.run();
    std::printf("UI_REISSUE_RESULT trace=%u sample=%u refusal=%u actions=%u callbacks=%u read=%llu state=%llu mask=%llx host=%llx out=%llx depth=%llx overlay=%llu depthhit=%llu\n",
        traceEnabled?1u:0u,apiSample?1u:0u,refusal?1u:0u,
        traceEnabled?(refusal?3u:9u):0u,result.realDrawCallbackCount,
        static_cast<unsigned long long>(reads),static_cast<unsigned long long>(states),
        static_cast<unsigned long long>(core.apiSiteMask[1]),
        static_cast<unsigned long long>(hostHash),static_cast<unsigned long long>(composedHash),
        static_cast<unsigned long long>(depthHash),
        static_cast<unsigned long long>(composedOverlayPixels),
        static_cast<unsigned long long>(depthWrittenPixels));
    return true;
}

enum class NvPrecedenceCase : unsigned {
    NightVision, ForeignContext, EyeRange, OffscreenRule,
    OffscreenNone, PanelDistance, ShaderMiss, ModeOff
};

struct NvExpected final {
    std::int16_t winner;
    std::int16_t verdict;
    std::int16_t subsite;
    std::uint32_t claim;
    std::uint32_t claimCalls;
    std::uint32_t beginCalls;
    std::uint32_t endCalls;
    bool replace;
    bool panel;
    bool skip;
};

// Literal selector oracle from the requested case matrix. Keep it independent
// from dispatch helpers, the manifest, and the visitor's returned decision.
constexpr NvExpected kNvExpected[] = {
    {50, 6, 0, 0x1001, 1, 1, 1, true,  false, false},
    {2,  0, 0, 0,      0, 0, 0, false, false, false},
    {49, 2, 0, 0,      0, 0, 0, false, false, true },
    {24, 2, 0, 0,      0, 0, 0, false, false, true },
    {28, 0, 0, 0,      0, 0, 0, false, false, false},
    {66, 1, 0, 0,      0, 0, 0, false, true,  false},
    {67, 0, 0, 0,      0, 0, 0, false, false, false},
    {68, 0, 1, 0,      0, 0, 0, false, false, false},
};

bool nvObservationMatches(unsigned rawCase, const VScreenPanelDistanceApiTestInput& input,
                          const VScreenPanelDistanceApiTestResult& result) {
    if (rawCase >= 8u) return false;
    const NvExpected expected = kNvExpected[rawCase];
    const std::uint32_t literalCount = expected.panel ? 6u : 240u;
    const std::uint64_t literalPs = 0xF786D34B5E118D5Eull +
        (rawCase == static_cast<unsigned>(NvPrecedenceCase::ShaderMiss) ? 1ull : 0ull);
    return input.kind == 'X' && input.drawCount == literalCount &&
        input.drawInstances == 1 && input.drawArgs.start == 7 &&
        input.drawArgs.base == -3 && input.drawArgs.startInstance == 11 &&
        input.cockpitPluginDispatch &&
        input.cockpitVsHash == 0xFCF7BD2896751D96ull && input.cockpitPsHash == literalPs &&
        result.winner == expected.winner && result.verdict == expected.verdict &&
        result.siteResult.subsite == expected.subsite &&
        result.cockpitClaimValue == expected.claim &&
        result.cockpitClaimCalls == expected.claimCalls &&
        result.cockpitBeginCalls == expected.beginCalls &&
        result.cockpitEndCalls == expected.endCalls;
}

bool nvObservationNegativeControls() {
    VScreenPanelDistanceApiTestInput input{};
    VScreenPanelDistanceApiTestResult actual{};
    input.kind = 'X'; input.drawCount = 240; input.drawInstances = 1;
    input.drawArgs = {7, -3, 11}; input.cockpitVsHash = 0xFCF7BD2896751D96ull;
    input.cockpitPsHash = 0xF786D34B5E118D5Eull;
    input.cockpitPluginDispatch = true;
    actual.winner = 50; actual.verdict = 6; actual.siteResult.subsite = 0;
    actual.cockpitClaimValue = 0x1001; actual.cockpitClaimCalls = 1;
    actual.cockpitBeginCalls = 1; actual.cockpitEndCalls = 1;
    if (!nvObservationMatches(0, input, actual)) return false;
    auto wrongWinner = actual; wrongWinner.winner = 49;
    auto missingClaim = actual; missingClaim.cockpitClaimCalls = 0;
    auto missingBegin = actual; missingBegin.cockpitBeginCalls = 0;
    auto missingEnd = actual; missingEnd.cockpitEndCalls = 0;
    if (nvObservationMatches(0, input, wrongWinner) ||
        nvObservationMatches(1, input, actual) ||
        nvObservationMatches(0, input, missingClaim) ||
        nvObservationMatches(0, input, missingBegin) ||
        nvObservationMatches(0, input, missingEnd)) return false;
    input.drawCount = 6;
    actual.winner = 66; actual.verdict = 1; actual.cockpitClaimValue = 0;
    actual.cockpitClaimCalls = actual.cockpitBeginCalls = actual.cockpitEndCalls = 0;
    if (!nvObservationMatches(5, input, actual)) return false;
    input.drawCount = 240; // Wrong shape must fail the literal PanelDistance case.
    if (nvObservationMatches(5, input, actual)) return false;
    input.cockpitPsHash = 0xF786D34B5E118D5Eull + 1ull;
    actual.winner = 67; actual.verdict = 0; actual.siteResult.subsite = 0;
    if (!nvObservationMatches(6, input, actual)) return false;
    input.cockpitPsHash = 0xF786D34B5E118D5Eull; // Missing +1 hash mismatch must fail.
    return !nvObservationMatches(6, input, actual);
}

bool runNvPrecedenceChild(unsigned rawCase, bool traceEnabled, bool apiSample) {
    if (rawCase >= 8u) return false;
    const auto which = static_cast<NvPrecedenceCase>(rawCase);
    const NvExpected expected = kNvExpected[rawCase];
    UiCaseRig rig{};
    if (!setupUiCaseRig(&rig)) return false;
    struct NvCleanup final {
        ID3D11DeviceContext* context;
        std::wstring tracePath;
        bool done = false;
        void run() {
            if (done) return;
            done = true;
            edvr::draw_ladder_trace::shutdown();
            edvrPluginCostShutdown();
            for (std::size_t i=0;i<static_cast<std::size_t>(edvr::BindSlot::Count);++i)
                edvr::detail::g_bindingSlots[i] = edvr::detail::BindingSlot{};
            context->ClearState();
            if (!tracePath.empty()) DeleteFileW(tracePath.c_str());
        }
        ~NvCleanup() { run(); }
    } cleanup{rig.context.Get()};
    ComPtr<ID3D11RenderTargetView> offscreenRtv;
    if (which == NvPrecedenceCase::OffscreenRule || which == NvPrecedenceCase::OffscreenNone) {
        UiCaseTexture offscreen{};
        if (!makeUiCaseTexture(rig.device.Get(), 256, 128, D3D11_BIND_RENDER_TARGET,
                               &offscreen)) return false;
        offscreenRtv = offscreen.rtv;
        ID3D11RenderTargetView* rt = offscreenRtv.Get();
        rig.context->OMSetRenderTargets(1, &rt, nullptr);
        const D3D11_VIEWPORT vp{0, 0, 256.0f, 128.0f, 0, 1};
        rig.context->RSSetViewports(1, &vp);
        edvr::bindingSet(edvr::BindSlot::Rtv0, offscreenRtv.Get());
    }
    ComPtr<ID3D11DeviceContext> foreignContext;
    if (which == NvPrecedenceCase::ForeignContext) {
        if (FAILED(rig.device->CreateDeferredContext(0, &foreignContext))) return false;
    }
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 16; cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> compositeCb, overrideCb;
    if (!uiCaseSucceeded(rig.device->CreateBuffer(&cbDesc, nullptr, &compositeCb), "nv-host-cb") ||
        !uiCaseSucceeded(rig.device->CreateBuffer(&cbDesc, nullptr, &overrideCb), "nv-override-cb")) return false;

    wchar_t temp[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, temp)) return false;
    const std::wstring tracePath = std::wstring(temp) + L"edvr_gfx_nv_precedence_" +
        std::to_wstring(GetCurrentProcessId()) + L".log";
    cleanup.tracePath = tracePath;
    if (traceEnabled) {
        HANDLE f = CreateFileW(tracePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        CloseHandle(f);
        if (!armCapture(tracePath)) { DeleteFileW(tracePath.c_str()); return false; }
    }
    edvrPluginCostShutdown();
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
    edvrPluginCostConfigure(1u, static_cast<std::uint64_t>(frequency.QuadPart));
    edvrPluginCostSetOwnerContext(rig.context.Get());
    EdvrPluginCostWindowV1 discarded{};
    edvrPluginCostFrameBoundary(0, 0, 0, apiSample ? 1 : 0, 0, &discarded);

    const edvr::RuntimeProfile priorProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    auto& config = edvr::Config::get();
    const std::string priorStability = config.getString("fix.night_vision_stability", "on");
    const std::string priorRealistic = config.getString("experimental.night_vision_realistic", "off");
    config.set("fix.night_vision_stability",
        which == NvPrecedenceCase::ModeOff ? "off" : "on");
    config.set("experimental.night_vision_realistic", "off");

    VScreenPanelDistanceApiTestInput input{};
    input.context = which == NvPrecedenceCase::ForeignContext
        ? foreignContext.Get() : rig.context.Get();
    input.ownerContext = rig.context.Get();
    input.traceEnabled = traceEnabled; input.cpuSample = false;
    input.distanceEnabled = which == NvPrecedenceCase::PanelDistance ||
                            which == NvPrecedenceCase::ModeOff;
    input.fullClassifier = true; input.cockpitPluginDispatch = true;
    input.uiEyeWidth = kUiEyeW; input.uiEyeHeight = kUiEyeH;
    input.cockpitNightVisionModeOff = which == NvPrecedenceCase::ModeOff;
    input.retainDrawGateDemand = which == NvPrecedenceCase::ModeOff;
    input.eyeRangeSkip = which == NvPrecedenceCase::EyeRange;
    input.offscreenSkip = which == NvPrecedenceCase::OffscreenRule;
    // The typed saved-original spy proves X forwarding. The shared GPU
    // callback is D-only; NV replacement pixels belong to night_vision_test.
    input.issueRealDraw = false;
    input.panelSrv = which == NvPrecedenceCase::ModeOff
        ? static_cast<void*>(rig.eye.srv.Get())
        : static_cast<void*>(rig.panel.srv.Get());
    input.eyeRtv = (which == NvPrecedenceCase::OffscreenRule ||
                    which == NvPrecedenceCase::OffscreenNone)
        ? static_cast<void*>(offscreenRtv.Get()) : static_cast<void*>(rig.eye.rtv.Get());
    input.hostDsv = rig.dsv.Get(); input.compositeCb = compositeCb.Get();
    input.ourCb = overrideCb.Get();
    input.cockpitVs = rig.vs.Get(); input.cockpitPs = rig.ps.Get();
    input.cockpitVsHash = 0xFCF7BD2896751D96ull;
    input.cockpitPsHash = 0xF786D34B5E118D5Eull +
        (which == NvPrecedenceCase::ShaderMiss ? 1ull : 0ull);
    input.kind = 'X'; input.drawCount = which == NvPrecedenceCase::PanelDistance ? 6u : 240u;
    input.drawInstances = 1; input.drawArgs = {7, -3, 11};
    input.shadowBytes = 16; input.distanceIndex = 3; input.distanceScale = 1.5f;
    float mapped[4] = {1, 2, 3, 4};
    input.mappedStorage = reinterpret_cast<std::uint8_t*>(mapped);
    input.mappedStorageBytes = sizeof(mapped);
    ID3D11Buffer* hostCb = compositeCb.Get();
    rig.context->VSSetConstantBuffers(0, 1, &hostCb);
    edvr::bindingSet(edvr::BindSlot::VsCb0, hostCb);
    ID3D11ShaderResourceView* hostSrv = static_cast<ID3D11ShaderResourceView*>(input.panelSrv);
    rig.context->PSSetShaderResources(0, 1, &hostSrv);
    if (foreignContext.Get()) {
        ID3D11RenderTargetView* foreignRt = rig.eye.rtv.Get();
        ID3D11DepthStencilView* foreignDs = rig.dsv.Get();
        foreignContext->OMSetRenderTargets(1, &foreignRt, foreignDs);
        foreignContext->VSSetShader(rig.vs.Get(), nullptr, 0);
        foreignContext->PSSetShader(rig.ps.Get(), nullptr, 0);
        foreignContext->PSSetShaderResources(0, 1, &hostSrv);
        foreignContext->VSSetConstantBuffers(0, 1, &hostCb);
        const D3D11_VIEWPORT foreignVp{0, 0, static_cast<float>(kUiEyeW),
                                      static_cast<float>(kUiEyeH), 0, 1};
        foreignContext->RSSetViewports(1, &foreignVp);
    }
    if (which == NvPrecedenceCase::OffscreenRule || which == NvPrecedenceCase::OffscreenNone)
        edvr::bindingSet(edvr::BindSlot::Dsv0, nullptr);
    ID3D11Buffer* beforeNvCb[2]{};
    rig.context->PSGetConstantBuffers(1, 2, beforeNvCb);
    const bool nvCbSlotsInitiallyNull = !beforeNvCb[0] && !beforeNvCb[1];
    for (ID3D11Buffer* cb : beforeNvCb) if (cb) cb->Release();
    const std::size_t bindingCount = static_cast<std::size_t>(edvr::BindSlot::Count);
    std::vector<edvr::detail::BindingSlot> bindingsBefore(bindingCount);
    for (std::size_t i = 0; i < bindingCount; ++i)
        bindingsBefore[i] = edvr::detail::g_bindingSlots[i];
    const UiCaseSnapshot hostBefore = takeUiCaseSnapshot(rig.context.Get());
    VScreenPanelDistanceApiTestResult result{};
    const bool ran = vScreenPanelDistanceApiTransactionTest(input, &result);
    bool bindingsRestored = true;
    for (std::size_t i = 0; i < bindingCount; ++i) {
        const auto& a = bindingsBefore[i]; const auto& b = edvr::detail::g_bindingSlots[i];
        bindingsRestored = bindingsRestored && a.ptr == b.ptr && a.gen == b.gen && a.hash == b.hash;
    }
    const UiCaseSnapshot hostAfter = takeUiCaseSnapshot(rig.context.Get());
    ID3D11Buffer* afterNvCb[2]{}; ID3D11Buffer* afterHostCb = nullptr;
    rig.context->PSGetConstantBuffers(1, 2, afterNvCb);
    rig.context->VSGetConstantBuffers(0, 1, &afterHostCb);
    const bool hostRestored = sameUiCaseSnapshot(hostBefore, hostAfter) &&
        afterNvCb[0] == nullptr && afterNvCb[1] == nullptr && afterHostCb == compositeCb.Get() &&
        nvCbSlotsInitiallyNull;
    for (ID3D11Buffer* cb : afterNvCb) if (cb) cb->Release();
    if (afterHostCb) afterHostCb->Release();
    const bool winnerMatch = nvObservationMatches(rawCase, input, result);
    const bool callbackMatch = winnerMatch;
    const bool actionsMatch = rawCase == static_cast<unsigned>(NvPrecedenceCase::ForeignContext)
        ? draw_ladder_trace::actionCountForTest(result.token) == 0
        : traceEnabled
            ? nvForwardActionsMatch(result, input, expected.replace, expected.panel, expected.skip)
            : draw_ladder_trace::actionCountForTest(result.token) == 0;
    const bool inputTupleLiteral = input.kind == 'X' &&
        input.drawCount == (expected.panel ? 6u : 240u) && input.drawInstances == 1 &&
        input.drawArgs.start == 7 && input.drawArgs.base == -3 &&
        input.drawArgs.startInstance == 11;
    const bool drawMatch = expected.skip
        ? result.originalDrawCalls == 0 && inputTupleLiteral
        : result.drawArgumentsValid && result.originalDrawCalls == 1 && inputTupleLiteral;
    EdvrPluginCostWindowV2 report{}; bool complete = false;
    for (std::uint32_t frame = 1; frame <= 1800; ++frame)
        complete = edvrPluginCostFrameBoundaryV2(frame, 0, apiSample ? 1 : 0,
                                                apiSample ? 1 : 0, 0, &report) != 0;
    const auto& cockpit = report.owners[static_cast<std::uint8_t>(edvr::plugin_cost::Owner::CockpitVisuals)];
    const auto& core = report.owners[static_cast<std::uint8_t>(edvr::plugin_cost::Owner::Core)];
    const auto& panelOwner = report.owners[static_cast<std::uint8_t>(edvr::plugin_cost::Owner::OnFootPanel)];
    const std::uint64_t nvReadMask = (1ull << 18) | (1ull << 19) | (1ull << 20);
    const std::uint64_t panelMask = (1ull << 58) | (1ull << 59) |
                                    (1ull << 60) | (1ull << 61);
    const bool nvSampled = apiSample && rawCase == static_cast<unsigned>(NvPrecedenceCase::NightVision);
    const bool panelSampled = apiSample && rawCase == static_cast<unsigned>(NvPrecedenceCase::PanelDistance);
    // Cold target recognition records Core112/113/115 once. Offscreen rule
    // evaluation and X6 panel eligibility each perform one additional view
    // inspection. Captured owner draws first resolve RTV0 for the replay's
    // dimensions; foreign contexts neither capture nor reach the classifier.
    const std::uint64_t descriptorMask = (1ull << (112-64)) |
        (1ull << (113-64)) | (1ull << (115-64));
    const unsigned descriptorViews = which == NvPrecedenceCase::ForeignContext ? 0u :
        ((which == NvPrecedenceCase::OffscreenRule ||
          which == NvPrecedenceCase::PanelDistance) ? 2u : 1u) +
            (traceEnabled ? 1u : 0u);
    const bool costMatch = complete && report.version == 2 &&
        report.completedApiSampleFrames == (apiSample ? 1800u : 0u) &&
        cockpit.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::ReadQuery)] ==
            (nvSampled ? 3ull : 0ull) &&
        cockpit.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::State)] == 0 &&
        cockpit.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::Transfer)] == 0 &&
        cockpit.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::Work)] == 0 &&
        cockpit.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::Instrumentation)] == 0 &&
        cockpit.apiSiteMask[0] == (nvSampled ? nvReadMask : 0ull) && cockpit.apiSiteMask[1] == 0 &&
        cockpit.apiSiteMask[2] == 0 && cockpit.apiSiteMask[3] == 0 &&
        core.apiCalls[0] == 0 && core.apiCalls[1] == 0 && core.apiCalls[2] == 0 &&
        core.apiCalls[3] == (apiSample ? descriptorViews*3ull : 0ull) && core.apiCalls[4] == 0 &&
        core.apiSiteMask[0] == 0 &&
        core.apiSiteMask[1] == (apiSample && descriptorViews ? descriptorMask : 0ull) &&
        core.apiSiteMask[2] == 0 && core.apiSiteMask[3] == 0 &&
        panelOwner.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::Transfer)] ==
            (panelSampled ? 2ull : 0ull) &&
        panelOwner.apiCalls[static_cast<std::uint8_t>(edvr::plugin_cost::ApiClass::State)] ==
            (panelSampled ? 2ull : 0ull) &&
        panelOwner.apiCalls[0] == 0 && panelOwner.apiCalls[3] == 0 && panelOwner.apiCalls[4] == 0 &&
        panelOwner.apiSiteMask[0] == 0 &&
        panelOwner.apiSiteMask[1] == (panelSampled ? panelMask : 0ull) &&
        panelOwner.apiSiteMask[2] == 0 && panelOwner.apiSiteMask[3] == 0;

    const unsigned actionCount = traceEnabled
        ? static_cast<unsigned>(draw_ladder_trace::actionCountForTest(result.token)) : 0u;
    if (foreignContext.Get()) foreignContext->ClearState();
    cleanup.run();
    config.set("fix.night_vision_stability", priorStability.c_str());
    config.set("experimental.night_vision_realistic", priorRealistic.c_str());
    edvr::g_runtimeProfile = priorProfile;
    const bool okay = ran && winnerMatch && callbackMatch && actionsMatch && drawMatch &&
        hostRestored && bindingsRestored && costMatch;
    if (!okay) {
        std::fprintf(stderr,
            "NV_PRECEDENCE_FAIL case=%u trace=%u sample=%u ran=%u winner=%d/%d verdict=%d/%d sub=%u/%d claim=%x/%x callbacks=%u,%u,%u actions=%u draw=%u host=%u bindings=%u costs=%u reads=%llu mask=%llx corereads=%llu coremask=%llx\n",
            rawCase, traceEnabled?1u:0u, apiSample?1u:0u, ran?1u:0u,
            result.winner, expected.winner, result.verdict, expected.verdict,
            result.siteResult.subsite, expected.subsite, result.cockpitClaimValue,
            expected.claim, result.cockpitClaimCalls, result.cockpitBeginCalls,
            result.cockpitEndCalls, actionsMatch?1u:0u, drawMatch?1u:0u,
            hostRestored?1u:0u, bindingsRestored?1u:0u, costMatch?1u:0u,
            static_cast<unsigned long long>(cockpit.apiCalls[3]),
            static_cast<unsigned long long>(cockpit.apiSiteMask[0]),
            static_cast<unsigned long long>(core.apiCalls[3]),
            static_cast<unsigned long long>(core.apiSiteMask[1]));
        return false;
    }
    std::printf("NV_PRECEDENCE_RESULT case=%u trace=%u sample=%u winner=%d verdict=%d sub=%u claim=%x claimcalls=%u begin=%u end=%u actions=%u original=%u reads=%llu mask=%llx host=1 bindings=1\n",
        rawCase, traceEnabled?1u:0u, apiSample?1u:0u, result.winner, result.verdict,
        result.siteResult.subsite, result.cockpitClaimValue, result.cockpitClaimCalls,
        result.cockpitBeginCalls, result.cockpitEndCalls,
        actionCount,
        result.originalDrawCalls,
        static_cast<unsigned long long>(cockpit.apiCalls[3]),
        static_cast<unsigned long long>(cockpit.apiSiteMask[0]));
    return true;
}

struct ChildCompletion final {
    DWORD processId = 0;
    DWORD wait = WAIT_TIMEOUT, finalWait = WAIT_TIMEOUT, exitCode = STILL_ACTIVE;
    DWORD pipeError = 0, waitError = 0, terminationError = 0, exitError = 0;
    bool overflow = false, unexpectedPipeError = false, exitKnown = false;
    bool eofWhileRunning = false;
    ULONGLONG elapsed = 0;
    std::string output;
    bool succeeded() const {
        return wait == WAIT_OBJECT_0 && finalWait == WAIT_OBJECT_0 && exitKnown &&
            exitCode == 0 && !overflow && !unexpectedPipeError &&
            !waitError && !terminationError && !exitError;
    }
    const char* failureReason() const {
        if (overflow) return "output-overflow";
        if (unexpectedPipeError) return "pipe-error";
        if (finalWait != WAIT_OBJECT_0 || terminationError) return "termination-failed";
        if (wait == WAIT_FAILED || waitError) return "process-wait";
        if (wait == WAIT_TIMEOUT) return "deadline";
        if (!exitKnown || exitError) return "exit-query";
        if (exitCode != 0) return "child-exit";
        return "framing";
    }
};

ChildCompletion collectChild(HANDLE pipe, HANDLE process, DWORD budgetMs,
                             std::size_t maxBytes, HANDLE releaseAtEof = nullptr) {
    ChildCompletion result{};
    result.processId = GetProcessId(process);
    const ULONGLONG started = GetTickCount64();
    const ULONGLONG deadline = started + budgetMs;
    bool eof = false;
    char chunk[512];
    auto drain = [&] {
        while (!eof && !result.overflow && !result.unexpectedPipeError) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                result.pipeError = GetLastError();
                eof = result.pipeError == ERROR_BROKEN_PIPE;
                result.unexpectedPipeError = !eof;
                break;
            }
            if (!available) break;
            DWORD got = 0;
            const DWORD take = (std::min)(available, static_cast<DWORD>(sizeof(chunk)));
            if (!ReadFile(pipe, chunk, take, &got, nullptr)) {
                result.pipeError = GetLastError();
                eof = result.pipeError == ERROR_BROKEN_PIPE;
                result.unexpectedPipeError = !eof;
                break;
            }
            if (!got) { eof = true; break; }
            if (result.output.size() + got > maxBytes) { result.overflow = true; break; }
            result.output.append(chunk, got);
        }
    };
    while (GetTickCount64() < deadline) {
        drain();
        if (result.overflow || result.unexpectedPipeError) break;
        if (eof && releaseAtEof) {
            result.eofWhileRunning = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            if (!SetEvent(releaseAtEof)) { result.waitError = GetLastError(); break; }
            releaseAtEof = nullptr;
        }
        // EOF closes the output stream, not the process. CRT teardown can
        // close stdout before the process handle becomes signalled.
        const ULONGLONG remaining = deadline - (std::min)(deadline, GetTickCount64());
        result.wait = WaitForSingleObject(process,
            static_cast<DWORD>((std::min)(remaining, 10ull)));
        if (result.wait == WAIT_OBJECT_0) break;
        if (result.wait == WAIT_FAILED) { result.waitError = GetLastError(); break; }
    }
    result.finalWait = WaitForSingleObject(process, 0);
    if (result.finalWait != WAIT_OBJECT_0) {
        if (!TerminateProcess(process, 1)) result.terminationError = GetLastError();
        result.finalWait = WaitForSingleObject(process, 5000);
        if (result.finalWait == WAIT_FAILED) result.waitError = GetLastError();
    }
    result.exitKnown = GetExitCodeProcess(process, &result.exitCode) != FALSE;
    if (!result.exitKnown) result.exitError = GetLastError();
    drain();
    result.elapsed = GetTickCount64() - started;
    return result;
}

// CPU-only process controls use an inherited event to guarantee the child
// closes output while still alive. The parent releases it only after EOF.
bool launchPipeLifecycleControl(unsigned mode, ChildCompletion* completion) {
    wchar_t exe[MAX_PATH + 1]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!completion || !length || length >= MAX_PATH) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE release = CreateEventW(&sa, TRUE, FALSE, nullptr);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::uint8_t> storage(bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    const bool initialized = bytes &&
        InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    HANDLE inherited[] = {writePipe, input, release};
    const bool ready = input != INVALID_HANDLE_VALUE && release && initialized &&
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0) &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writePipe;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(exe, length) +
        L"\" --pipe-lifecycle-child " + std::to_wstring(mode) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(release));
    const bool started = ready && CreateProcessW(exe, &command[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &process) != FALSE;
    if (initialized) DeleteProcThreadAttributeList(attributes);
    CloseHandle(writePipe);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (started) {
        *completion = collectChild(readPipe, process.hProcess, mode == 2 ? 1500u : 5000u,
            512, release);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    if (release) CloseHandle(release);
    CloseHandle(readPipe);
    return started;
}

bool testChildPipeLifecycle() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        ChildCompletion result{};
        if (!launchPipeLifecycleControl(mode, &result)) return false;
        const bool common = result.output == "PIPE_LIFECYCLE_RESULT\n" &&
            result.pipeError == ERROR_BROKEN_PIPE && result.eofWhileRunning &&
            !result.unexpectedPipeError && !result.overflow && result.exitKnown &&
            result.finalWait == WAIT_OBJECT_0 && !result.waitError &&
            !result.terminationError && !result.exitError;
        const bool expected = mode == 0 ? result.succeeded() : mode == 1 ?
            (!result.succeeded() && result.wait == WAIT_OBJECT_0 && result.exitCode == 7 &&
             std::strcmp(result.failureReason(), "child-exit") == 0) :
            (!result.succeeded() && result.wait == WAIT_TIMEOUT && result.exitCode == 1 &&
             result.elapsed >= 1500u && std::strcmp(result.failureReason(), "deadline") == 0);
        if (!common || !expected) {
            std::fprintf(stderr,"PIPE_LIFECYCLE_FAIL mode=%u wait=%lu finalwait=%lu exit=%lu elapsed=%llu pipeerror=%lu eofrunning=%u waiterror=%lu killerror=%lu exiterror=%lu pid=%lu\n",
                mode,result.wait,result.finalWait,result.exitCode,
                static_cast<unsigned long long>(result.elapsed),result.pipeError,
                result.eofWhileRunning?1u:0u,result.waitError,result.terminationError,result.exitError,
                result.processId);
            return false;
        }
    }
    return true;
}

struct UiChildReport final {
    unsigned trace = 0, sample = 0, refusal = 0, actions = 0, callbacks = 0;
    unsigned long long reads = 0, states = 0, mask = 0;
    unsigned long long host = 0, output = 0, depth = 0;
    unsigned long long overlayPixels = 0, depthWrittenPixels = 0;
};

bool launchUiReissueChild(bool trace, bool sample, bool refusal, UiChildReport* report) {
    wchar_t exe[MAX_PATH * 2]{};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exe,
        static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    if (!exeLen || exeLen >= sizeof(exe) / sizeof(exe[0])) return false;
    HANDLE readPipe = nullptr, writePipe = nullptr, nullInput = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (!attributeBytes) {
        CloseHandle(nullInput); CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    std::vector<std::uint8_t> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    bool attributesInitialized = InitializeProcThreadAttributeList(
        attributes, 1, 0, &attributeBytes) != FALSE;
    HANDLE inheritedHandles[2] = {writePipe, nullInput};
    if (!attributesInitialized ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
        if (attributesInitialized) DeleteProcThreadAttributeList(attributes);
        CloseHandle(nullInput); CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = writePipe;
    startup.StartupInfo.hStdError = writePipe;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(exe, exeLen) +
        L"\" --ui-reissue-child " + (trace ? L"1 " : L"0 ") +
        (sample ? L"1 " : L"0 ") + (refusal ? L"1" : L"0");
    const BOOL started = CreateProcessW(exe, &command[0], nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
        &startup.StartupInfo, &process);
    const DWORD startError = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(writePipe);
    CloseHandle(nullInput);
    if (!started) { CloseHandle(readPipe); SetLastError(startError); return false; }
    const ChildCompletion completion = collectChild(readPipe, process.hProcess, 120000, 4096);
    const auto& output = completion.output;
    const DWORD wait = completion.wait, exitCode = completion.exitCode;
    const bool overflow = completion.overflow;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    auto failedChild = [&](const char* reason) {
        std::fprintf(stderr,
            "UI_REISSUE_CHILD_FAIL trace=%u sample=%u refusal=%u reason=%s wait=%lu exit=%lu overflow=%u bytes=%llu elapsed=%llu pipeerror=%lu finalwait=%lu waiterror=%lu killerror=%lu exiterror=%lu pid=%lu\n",
            trace?1u:0u, sample?1u:0u, refusal?1u:0u, reason,
            static_cast<unsigned long>(wait), static_cast<unsigned long>(exitCode),
            overflow?1u:0u, static_cast<unsigned long long>(output.size()),
            static_cast<unsigned long long>(completion.elapsed),
            completion.pipeError,completion.finalWait,completion.waitError,
            completion.terminationError,completion.exitError,completion.processId);
        if (!output.empty()) {
            std::fwrite(output.data(), 1, output.size(), stderr);
            if (output.back()!='\n') std::fputc('\n',stderr);
        }
        return false;
    };
    if (!completion.succeeded() || output.empty() ||
        output.back() != '\n' || output.find('\n') != output.size() - 1 ||
        output.rfind("UI_REISSUE_RESULT ", 0) != 0 || !report)
        return failedChild(completion.failureReason());
    int consumed = -1;
    const int parsed = std::sscanf(output.c_str(),
        "UI_REISSUE_RESULT trace=%u sample=%u refusal=%u actions=%u callbacks=%u read=%llu state=%llu mask=%llx host=%llx out=%llx depth=%llx overlay=%llu depthhit=%llu%n",
        &report->trace, &report->sample, &report->refusal, &report->actions, &report->callbacks,
        &report->reads, &report->states, &report->mask, &report->host,
        &report->output, &report->depth, &report->overlayPixels,
        &report->depthWrittenPixels, &consumed);
    // Windows text-mode stdout emits CRLF. Accept that or LF, but require
    // the entire captured record to end immediately after its one terminator.
    const bool terminator = consumed >= 0 &&
        ((output.size() == static_cast<std::size_t>(consumed) + 1 &&
          output[consumed] == '\n') ||
         (output.size() == static_cast<std::size_t>(consumed) + 2 &&
          output[consumed] == '\r' && output[consumed + 1] == '\n'));
    const bool expectUiApi = sample && !refusal;
    const bool accepted = parsed == 13 && terminator &&
        report->trace == (trace ? 1u : 0u) &&
        report->sample == (sample ? 1u : 0u) &&
        report->refusal == (refusal ? 1u : 0u) &&
        report->actions == (trace ? (refusal ? 3u : 9u) : 0u) &&
        report->callbacks == (refusal ? 1u : 2u) &&
        report->reads == (sample ? kUiSharedDescriptorReads+
            (trace?kCapturedDrawDescriptorReads:0ull)+(expectUiApi?6ull:0ull) : 0ull) &&
        report->states == (expectUiApi ? 8ull : 0ull) &&
        (report->mask & kUiTransactionMask) == (expectUiApi ? kUiTransactionMask : 0ull) &&
        report->mask == ((sample ? kUiSharedDescriptorMask : 0ull) |
            (expectUiApi ? kUiTransactionMask : 0ull)) &&
        report->overlayPixels == (refusal ? 0ull :
            static_cast<unsigned long long>(kUiEyeW)*kUiEyeH) &&
        report->depthWrittenPixels == static_cast<unsigned long long>(kUiEyeW)*kUiEyeH;
    return accepted ? true : failedChild("result-contract");
}

bool testUiComposedActionSubprocesses() {
    UiChildReport reports[6]{};
    constexpr bool traceCases[6] = {true, false, true, false, true, true};
    constexpr bool sampleCases[6] = {true, true, false, false, true, true};
    constexpr bool refusalCases[6] = {false, false, false, false, false, true};
    for (unsigned i = 0; i < 6; ++i) {
        if (!launchUiReissueChild(traceCases[i], sampleCases[i], refusalCases[i], &reports[i])) return false;
    }
    return reports[0].host == reports[1].host &&
        reports[0].host == reports[2].host && reports[0].host == reports[3].host &&
        reports[0].host == reports[4].host &&
        reports[0].output == reports[1].output &&
        reports[0].output == reports[2].output &&
        reports[0].output == reports[3].output &&
        reports[0].output == reports[4].output &&
        reports[0].depth == reports[1].depth &&
        reports[0].depth == reports[2].depth &&
        reports[0].depth == reports[3].depth &&
        reports[0].depth == reports[4].depth && reports[5].host != reports[0].host &&
        reports[5].output == 0;
}

struct NvChildReport final {
    unsigned caseId = 0, trace = 0, sample = 0;
    int winner = -1, verdict = -1;
    unsigned subsite = 0, claim = 0, claimCalls = 0, beginCalls = 0, endCalls = 0;
    unsigned actions = 0, original = 0;
    unsigned long long reads = 0, mask = 0;
};

bool launchNvPrecedenceChild(unsigned caseId, bool trace, bool sample,
                             NvChildReport* report) {
    if (!report) return false;
    wchar_t exe[MAX_PATH + 1]{};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!exeLen || exeLen >= MAX_PATH) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::uint8_t> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    const bool attributesInitialized = attributeBytes &&
        InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) != FALSE;
    HANDLE inherited[2] = {writePipe, nullInput};
    if (!attributesInitialized || !UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        if (attributesInitialized) DeleteProcThreadAttributeList(attributes);
        CloseHandle(nullInput); CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writePipe;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(exe, exeLen) +
        L"\" --nv-precedence-child " + std::to_wstring(caseId) +
        (trace ? L" 1 " : L" 0 ") + (sample ? L"1" : L"0");
    const BOOL started = CreateProcessW(exe, &command[0], nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
        &startup.StartupInfo, &process);
    const DWORD startError = GetLastError();
    DeleteProcThreadAttributeList(attributes); CloseHandle(writePipe); CloseHandle(nullInput);
    if (!started) { CloseHandle(readPipe); SetLastError(startError); return false; }
    const ChildCompletion completion = collectChild(readPipe, process.hProcess, 120000, 512);
    const auto& output = completion.output;
    const DWORD wait = completion.wait, exitCode = completion.exitCode;
    const bool overflow = completion.overflow;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    auto failedChild = [&](const char* reason) {
        std::fprintf(stderr,"NV_PRECEDENCE_CHILD_DETAIL case=%u trace=%u sample=%u reason=%s wait=%lu exit=%lu overflow=%u bytes=%llu elapsed=%llu pipeerror=%lu finalwait=%lu waiterror=%lu killerror=%lu exiterror=%lu pid=%lu\n",
            caseId,trace?1u:0u,sample?1u:0u,reason,
            static_cast<unsigned long>(wait),static_cast<unsigned long>(exitCode),
            overflow?1u:0u,static_cast<unsigned long long>(output.size()),
            static_cast<unsigned long long>(completion.elapsed),
            completion.pipeError,completion.finalWait,completion.waitError,
            completion.terminationError,completion.exitError,completion.processId);
        if (!output.empty()) {
            std::fwrite(output.data(),1,output.size(),stderr);
            if (output.back()!='\n') std::fputc('\n',stderr);
        }
        return false;
    };
    if (!completion.succeeded() || output.empty() ||
        output.back() != '\n' || output.find('\n') != output.size() - 1 ||
        output.rfind("NV_PRECEDENCE_RESULT ", 0) != 0) return failedChild(completion.failureReason());
    int consumed = -1;
    const int parsed = std::sscanf(output.c_str(),
        "NV_PRECEDENCE_RESULT case=%u trace=%u sample=%u winner=%d verdict=%d sub=%u claim=%x claimcalls=%u begin=%u end=%u actions=%u original=%u reads=%llu mask=%llx host=1 bindings=1%n",
        &report->caseId, &report->trace, &report->sample, &report->winner,
        &report->verdict, &report->subsite, &report->claim, &report->claimCalls,
        &report->beginCalls, &report->endCalls, &report->actions, &report->original,
        &report->reads, &report->mask, &consumed);
    const bool terminator = consumed >= 0 &&
        ((output.size() == static_cast<std::size_t>(consumed) + 1 && output[consumed] == '\n') ||
         (output.size() == static_cast<std::size_t>(consumed) + 2 && output[consumed] == '\r' &&
          output[consumed + 1] == '\n'));
    const NvExpected expected = kNvExpected[caseId];
    const unsigned expectedActions = !trace || caseId == static_cast<unsigned>(NvPrecedenceCase::ForeignContext)
        ? 0u : expected.panel ? 6u : expected.skip ? 3u :
        expected.replace ? 5u : 3u;
    const bool nvSampled = sample && caseId == static_cast<unsigned>(NvPrecedenceCase::NightVision);
    const bool accepted = parsed == 14 && terminator && report->caseId == caseId &&
        report->trace == (trace ? 1u : 0u) && report->sample == (sample ? 1u : 0u) &&
        report->winner == expected.winner && report->verdict == expected.verdict &&
        report->subsite == static_cast<unsigned>(expected.subsite) &&
        report->claim == expected.claim && report->claimCalls == expected.claimCalls &&
        report->beginCalls == expected.beginCalls && report->endCalls == expected.endCalls &&
        report->actions == expectedActions && report->original == (expected.skip ? 0u : 1u) &&
        report->reads == (nvSampled ? 3ull : 0ull) &&
        report->mask == (nvSampled ? ((1ull << 18) | (1ull << 19) | (1ull << 20)) : 0ull);
    return accepted ? true : failedChild("result-contract");
}

bool testNvPrecedenceSubprocesses() {
    if (!nvObservationNegativeControls()) return false;
    for (unsigned caseId = 0; caseId < 8; ++caseId) {
        for (unsigned trace = 0; trace < 2; ++trace) {
            for (unsigned sample = 0; sample < 2; ++sample) {
                NvChildReport report{};
                if (!launchNvPrecedenceChild(caseId, trace != 0, sample != 0, &report)) {
                    std::fprintf(stderr, "NV_PRECEDENCE_CHILD_FAIL case=%u trace=%u sample=%u\n",
                        caseId, trace, sample);
                    return false;
                }
            }
        }
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--pipe-lifecycle-child") == 0) {
        if (std::strcmp(argv[2],"0") && std::strcmp(argv[2],"1") &&
            std::strcmp(argv[2],"2")) return 2;
        char* end = nullptr;
        const unsigned long long rawHandle = std::strtoull(argv[3], &end, 10);
        if (!end || end == argv[3] || *end || !rawHandle) return 2;
        HANDLE release = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(rawHandle));
        constexpr char frame[] = "PIPE_LIFECYCLE_RESULT\n";
        constexpr DWORD frameBytes = static_cast<DWORD>(sizeof(frame)-1);
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), frame, frameBytes,
                &written, nullptr) || written != frameBytes) return 2;
        std::fclose(stdout);
        std::fclose(stderr);
        const DWORD released = WaitForSingleObject(release, 5000);
        CloseHandle(release);
        if (released != WAIT_OBJECT_0) return 2;
        // Parent has proved EOF while this process is still alive.
        Sleep(std::strcmp(argv[2],"2") == 0 ? 10000u : 100u);
        return std::strcmp(argv[2],"1") == 0 ? 7 : 0;
    }
    if (argc == 5 && std::strcmp(argv[1], "--nv-precedence-child") == 0) {
        char* end = nullptr;
        const unsigned long caseId = std::strtoul(argv[2], &end, 10);
        if (!end || *end || caseId >= 8 ||
            (std::strcmp(argv[3], "0") != 0 && std::strcmp(argv[3], "1") != 0) ||
            (std::strcmp(argv[4], "0") != 0 && std::strcmp(argv[4], "1") != 0)) return 2;
        return runNvPrecedenceChild(static_cast<unsigned>(caseId),
            std::strcmp(argv[3], "1") == 0, std::strcmp(argv[4], "1") == 0) ? 0 : 1;
    }
    if (argc == 5 && std::strcmp(argv[1], "--ui-reissue-child") == 0) {
        const bool trace = std::strcmp(argv[2], "1") == 0;
        const bool sample = std::strcmp(argv[3], "1") == 0;
        const bool refusal = std::strcmp(argv[4], "1") == 0;
        if ((std::strcmp(argv[2], "0") != 0 && !trace) ||
            (std::strcmp(argv[3], "0") != 0 && !sample) ||
            (std::strcmp(argv[4], "0") != 0 && !refusal)) return 2;
        return runUiReissueChild(trace, sample, refusal) ? 0 : 1;
    }
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
            constexpr std::uint64_t kTargetHash = 0xE508648660A352B2ull;
            constexpr std::uint64_t kWrongHash = 0x5453484152500002ull;
            const auto savedInterest = edvr::pluginRegistryDrawInterestMask();
            const bool savedSharp = edvr::detail::g_targetSharpSharp;
            const bool savedFailed = edvr::detail::g_targetSharpFailed;
            okay &= check(edvr::targetSharpPredicateTestConfiguredHash() == kTargetHash,
                          "TargetSharp retains its immutable production shader hash");
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
            edvr::targetSharpPredicateTestSeed(true, false);

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
            edvr::targetSharpPredicateTestSeed(false, false);
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

            edvr::targetSharpPredicateTestSeed(true, true);
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

            edvr::targetSharpPredicateTestSeed(true, false);
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
            edvr::targetSharpPredicateTestSeed(true, false);
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.vsPresent, true) &&
                              fwRead(targetFact.queriedShaderHash, 0) &&
                              fwRead(targetFact.configuredShaderHash, kTargetHash) &&
                              targetResult.siteResult.outcome == SiteOutcome::Declined,
                          "unregistered real WARP VS resolves to raw zero and mismatches the fixed shader hash");
            if (targetVs) edvr::registerShaderHash(targetVs.Get(), kWrongHash);
            okay &= check(targetVs && edvr::lookupShaderHash(targetVs.Get()) == kWrongHash &&
                              edvr::targetSharpPredicateTestConfiguredHash() == kTargetHash,
                          "TargetSharp hash mismatch changes the queried shader while its fixed hash stays unchanged");
            if (targetVs) immediate->VSSetShader(targetVs.Get(), nullptr, 0);
            okay &= check(visitTarget('X', 6, 100, 80, 0, 0, true,
                                      &targetResult) &&
                              readTargetSharp(targetResult, &targetFact) &&
                              fwRead(targetFact.vsPresent, true) &&
                              fwRead(targetFact.queriedShaderHash, kWrongHash) &&
                              fwRead(targetFact.configuredShaderHash, kTargetHash) &&
                              targetResult.siteResult.outcome == SiteOutcome::Declined,
                          "registered real WARP hash mismatches the fixed shader hash");
            if (targetVs) edvr::registerShaderHash(targetVs.Get(), kTargetHash);

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
            edvr::targetSharpPredicateTestSeed(savedSharp, savedFailed);
            g_resourceAttempts = savedResourceAttempts;
            g_getVertexShaderCalls = savedVertexShaderCalls;
            g_injectResourceFault = savedResourceFault;
        }

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

    okay &= check(testChildPipeLifecycle(),
                  "CPU child EOF precedes natural exit; nonzero exit and actual short deadline are rejected and reaped");
    okay &= check(testUiComposedActionSubprocesses(),
                  "isolated WARP children exercise production UI family routing, composed reissues, sampled API notes, and NoTrace parity");
    okay &= check(testNvPrecedenceSubprocesses(),
                  "fresh WARP children verify literal Night Vision precedence, real plugin forwarding callbacks, typed arguments, restoration, and sampled null-CB reads");
    if (deferred) deferred->Release();
    if (immediate) immediate->Release();
    if (device) device->Release();
    edvr::draw_ladder_trace::shutdown();
    DeleteFileW(logPath.c_str());
    std::puts(okay ? "vscreen_predicate_test: PASS" : "vscreen_predicate_test: FAILED");
    return okay ? 0 : 1;
}
