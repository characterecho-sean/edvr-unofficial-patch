// Executes the production exposure damper on WARP. A small context proxy
// observes the five D3D entry points used by that function and can fail Map;
// all other resource and collector work uses the real implementation.
#define EDVR_BINDING_SHADOW_EXTERNAL 1
#define EDVR_EXPOSURE_DAMP_TEST 1
#include "../../src/d3d11/exposure_fix.cpp"
#include "../../src/plugins/exposure/exposure_actions.cpp"
#include "../../src/d3d11/shader_registry.cpp"
#include "../../src/common/system_d3d11.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;

namespace {
uint64_t g_now = 10000;
uint64_t g_sunSeen = 0;
void* g_boundStrip = nullptr;
unsigned g_checks = 0;
unsigned g_failures = 0;

uint64_t fixtureClock() { return g_now; }
bool check(bool yes, const char* label) {
    ++g_checks;
    if (!yes) { ++g_failures; std::printf("FAIL: %s\n", label); }
    return yes;
}
bool hrOk(HRESULT hr, const char* label) {
    if (SUCCEEDED(hr)) return true;
    ++g_failures;
    std::printf("FAIL: %s (0x%08lX)\n", label,
                static_cast<unsigned long>(hr));
    return false;
}

struct Calls {
    unsigned getDevice = 0;
    unsigned copy = 0;
    unsigned map = 0;
    unsigned unmap = 0;
    unsigned update = 0;
};
struct ContextProxy {
    void** vtable = nullptr;
    ID3D11DeviceContext* real = nullptr;
    Calls calls{};
    bool failMap = false;
};
ContextProxy& proxyOf(ID3D11DeviceContext* ctx) {
    return *reinterpret_cast<ContextProxy*>(ctx);
}
void STDMETHODCALLTYPE getDevice(ID3D11DeviceContext* self, ID3D11Device** out) {
    auto& p = proxyOf(self);
    ++p.calls.getDevice;
    p.real->GetDevice(out);
}
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* self, ID3D11Resource* resource,
                              UINT subresource, D3D11_MAP type, UINT flags,
                              D3D11_MAPPED_SUBRESOURCE* out) {
    auto& p = proxyOf(self);
    ++p.calls.map;
    return p.failMap ? E_FAIL : p.real->Map(resource, subresource, type, flags, out);
}
void STDMETHODCALLTYPE unmap(ID3D11DeviceContext* self, ID3D11Resource* resource,
                             UINT subresource) {
    auto& p = proxyOf(self);
    ++p.calls.unmap;
    p.real->Unmap(resource, subresource);
}
void STDMETHODCALLTYPE copyResource(ID3D11DeviceContext* self,
                                    ID3D11Resource* dst, ID3D11Resource* src) {
    auto& p = proxyOf(self);
    ++p.calls.copy;
    p.real->CopyResource(dst, src);
}
void STDMETHODCALLTYPE updateSubresource(ID3D11DeviceContext* self,
                                        ID3D11Resource* dst, UINT subresource,
                                        const D3D11_BOX* box, const void* data,
                                        UINT rowPitch, UINT depthPitch) {
    auto& p = proxyOf(self);
    ++p.calls.update;
    p.real->UpdateSubresource(dst, subresource, box, data, rowPitch, depthPitch);
}
constexpr uint16_t kGetDevice = 96;
constexpr uint16_t kCopy = 97;
constexpr uint16_t kMap = 98;
constexpr uint16_t kUnmap = 99;
constexpr uint16_t kWriteA = 100;
constexpr uint16_t kWriteB = 101;

uint64_t mask(std::initializer_list<uint16_t> sites) {
    uint64_t out = 0;
    for (const auto site : sites) out |= uint64_t{1} << (site - 64);
    return out;
}
void openWindow(ID3D11DeviceContext* owner, bool sample) {
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(1, 1000000);
    edvrPluginCostSetOwnerContext(owner);
    EdvrPluginCostWindowV1 discarded{};
    check(edvrPluginCostFrameBoundary(0, 0, 0, sample ? 1 : 0, 0,
                                      &discarded) == 0,
          "first collector boundary is discarded");
}
EdvrPluginCostWindowV1 closeWindow(bool sample) {
    EdvrPluginCostWindowV1 result{};
    uint8_t completed = 0;
    for (uint32_t frame = 1; frame <= 1800; ++frame) {
        completed = edvrPluginCostFrameBoundary(frame, 0, sample ? 1 : 0,
                                                sample ? 1 : 0, 0, &result);
    }
    check(completed != 0 && result.windowFrames == 1800,
          "fixed collector window closes after 1800 frames");
    return result;
}
bool verifyWindow(const EdvrPluginCostWindowV1& window, unsigned reads,
                  unsigned transfers, uint64_t expectedMask, const char* label) {
    const auto& owner = window.owners[static_cast<uint8_t>(pc::Owner::Exposure)];
    const bool okay = owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == reads &&
        owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == transfers &&
        owner.apiSiteMask[0] == 0 && owner.apiSiteMask[1] == expectedMask &&
        owner.apiObserved == (reads + transfers != 0);
    return check(okay, label);
}

void verifyShaderRegistry() {
    int shader = 0;
    const auto gen0 = edvr::shaderRegistryGeneration();
    edvr::registerShaderHash(&shader, 11);
    check(edvr::lookupShaderHash(&shader) == 0 &&
          edvr::shaderRegistryGeneration() == gen0,
          "registry ignores creation before the exposure lifetime");

    edvr::shaderRegistryBegin();
    check(edvr::shaderRegistryGeneration() == gen0,
          "empty begin leaves the registration generation unchanged");
    edvr::registerShaderHash(&shader, 11);
    const auto gen1 = edvr::shaderRegistryGeneration();
    check(edvr::lookupShaderHash(&shader) == 11 && gen1 == gen0 + 1,
          "live registration publishes the shader hash and generation");
    edvr::registerShaderHash(&shader, 22);
    check(edvr::lookupShaderHash(&shader) == 22 &&
          edvr::shaderRegistryGeneration() == gen1 + 1,
          "replacement at a reused pointer invalidates cached hashes");

    constexpr size_t kThreads = 4;
    constexpr size_t kSlots = 64;
    std::array<int, kThreads * kSlots> shaders{};
    std::array<bool, kThreads> threadOkay{};
    std::array<std::thread, kThreads> workers;
    const auto concurrentStart = edvr::shaderRegistryGeneration();
    for (size_t t = 0; t < kThreads; ++t) {
        workers[t] = std::thread([&, t] {
            bool okay = true;
            for (size_t i = 0; i < kSlots; ++i) {
                const auto hash = static_cast<uint64_t>(1000 + t * kSlots + i);
                void* key = &shaders[t * kSlots + i];
                edvr::registerShaderHash(key, hash);
                okay &= edvr::lookupShaderHash(key) == hash;
            }
            threadOkay[t] = okay;
        });
    }
    for (auto& worker : workers) worker.join();
    bool allFound = true;
    for (size_t t = 0; t < kThreads; ++t) {
        allFound &= threadOkay[t];
        for (size_t i = 0; i < kSlots; ++i) {
            allFound &= edvr::lookupShaderHash(&shaders[t * kSlots + i]) ==
                static_cast<uint64_t>(1000 + t * kSlots + i);
        }
    }
    check(allFound && edvr::shaderRegistryGeneration() ==
          concurrentStart + kThreads * kSlots,
          "concurrent register/lookup retains every hash and publication");

    const auto beforeEnd = edvr::shaderRegistryGeneration();
    edvr::shaderRegistryEnd();
    check(edvr::lookupShaderHash(&shader) == 0 &&
          edvr::lookupShaderHash(&shaders[0]) == 0 &&
          edvr::shaderRegistryGeneration() == beforeEnd,
          "end hides old hashes without publishing a registration");
    const auto dormantGen = edvr::shaderRegistryGeneration();
    edvr::registerShaderHash(&shader, 33);
    check(edvr::lookupShaderHash(&shader) == 0 &&
          edvr::shaderRegistryGeneration() == dormantGen,
          "registry ignores creation after shutdown");
    edvr::shaderRegistryBegin();
    check(edvr::lookupShaderHash(&shader) == 0 &&
          edvr::shaderRegistryGeneration() == dormantGen,
          "new lifetime cannot see a prior pointer assignment");
    edvr::registerShaderHash(&shader, 44);
    check(edvr::lookupShaderHash(&shader) == 44 &&
          edvr::shaderRegistryGeneration() == dormantGen + 1,
          "registration after retry publishes the new pointer assignment");
    edvr::shaderRegistryEnd();
}
} // namespace

namespace edvr {
// No hook is installed by this direct damper fixture. State's RAII member
// still calls uninstall during teardown, so satisfy only that unused edge.
void VTableHook::uninstall() {}
void* bindingGet(BindSlot slot) {
    return slot == BindSlot::CsUav1 ? g_boundStrip : nullptr;
}
uint64_t sunglareLastSeenMs() { return g_sunSeen; }
Log& Log::get() { static Log logger; return logger; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
// Keep configuration IO outside this WARP API-cost fixture, while satisfying
// the production action module's Config service dependency explicitly.
Config& Config::get() { static Config config; return config; }
float Config::getFloat(const char* key, float def) const {
    if (std::strcmp(key, "experimental.exposure_damping") == 0) return 0.5f;
    if (std::strcmp(key, "experimental.exposure_damping_tau") == 0) return 45.0f;
    return def;
}
} // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("dry-run: WARP exposure fixture would run; no files written");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("Usage: exposure_cost_test [--dry-run|--self-test]");
        return 2;
    }
    verifyShaderRegistry();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> real;
    D3D_FEATURE_LEVEL level{};
    const auto create = edvr::systemD3D11CreateDevice();
    if (!check(create != nullptr, "load System32 D3D11CreateDevice")) return 1;
    if (!hrOk(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                nullptr, 0, D3D11_SDK_VERSION, &device,
                                &level, &real), "create WARP device")) return 1;
    if (!check(edvr::reportSystemD3D11Only("exposure_cost_test"),
               "only System32 d3d11.dll is mapped")) return 1;

    const float raw[6] = {1.0f, 1.5f, 2.0f, 2.0f, 0.8f, 0.2f};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 6;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = raw;
    initial.SysMemPitch = sizeof(raw);
    ComPtr<ID3D11Texture2D> texA, texB;
    ComPtr<ID3D11UnorderedAccessView> viewA, viewB;
    if (!hrOk(device->CreateTexture2D(&desc, &initial, &texA), "create first-eye strip") ||
        !hrOk(device->CreateTexture2D(&desc, &initial, &texB), "create second-eye strip") ||
        !hrOk(device->CreateUnorderedAccessView(texA.Get(), nullptr, &viewA), "create first-eye UAV") ||
        !hrOk(device->CreateUnorderedAccessView(texB.Get(), nullptr, &viewB), "create second-eye UAV")) return 1;
    g_boundStrip = viewB.Get();

    // The proxy leaves real WARP resource methods intact. The five overridden
    // context methods are independently counted and forwarded, except the
    // controlled failing Map branch.
    std::array<void*, 49> table{};
    auto** realTable = *reinterpret_cast<void***>(real.Get());
    for (size_t i = 0; i < table.size(); ++i) table[i] = realTable[i];
    table[3] = reinterpret_cast<void*>(&getDevice);
    table[14] = reinterpret_cast<void*>(&map);
    table[15] = reinterpret_cast<void*>(&unmap);
    table[47] = reinterpret_cast<void*>(&copyResource);
    table[48] = reinterpret_cast<void*>(&updateSubresource);
    ContextProxy owner{table.data(), real.Get()};
    ContextProxy foreign{table.data(), real.Get()};
    auto* ownerCtx = reinterpret_cast<ID3D11DeviceContext*>(&owner);
    auto* foreignCtx = reinterpret_cast<ID3D11DeviceContext*>(&foreign);

    edvr::g_clockForTest = &fixtureClock;
    edvr::State state{};
    edvr::plugins::exposure::exposurePluginConfigure(&state, edvr::Config::get());
    check(state.dampK == 0.5f && state.dampTau == 45.0f,
          "production configure reads the WARP fixture's damper settings");
    edvr::g_state = &state;

    ID3D11UnorderedAccessView* firstUavs[4] = {viewA.Get(), nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* secondUavs[4] = {viewB.Get(), nullptr, nullptr, nullptr};
    state.copyMask = 1;
    openWindow(ownerCtx, true);
    edvr::plugins::exposure::exposurePluginShareExposure(
        &state, ownerCtx, firstUavs, secondUavs);
    check(owner.calls.copy == 1 && state.applied == 1 && !state.rejected,
          "production share action copies a compatible resource pair");
    verifyWindow(closeWindow(true), 0, 0, 0,
                 "resource sharing is outside Exposure damper API sites");
    owner.calls = {};

    openWindow(ownerCtx, true);
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get()); // no sun
    check(owner.calls.getDevice == 0 && owner.calls.copy == 0 &&
          owner.calls.map == 0, "sun decline avoids the measured D3D calls");
    g_sunSeen = g_now;
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, nullptr); // missing first eye
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get()); // identity changes
    g_now += 1000;
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get()); // identity not settled
    check(owner.calls.getDevice == 0 && owner.calls.copy == 0 &&
          owner.calls.map == 0, "UAV and settle declines avoid the measured D3D calls");
    verifyWindow(closeWindow(true), 0, 0, 0,
                 "early declines produce no Exposure API notes");

    g_now += 1000;
    owner.calls = {};
    openWindow(ownerCtx, true);
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get()); // allocate staging, queue first copy
    check(owner.calls.getDevice == 1 && owner.calls.copy == 1 &&
          owner.calls.map == 0 && owner.calls.unmap == 0 &&
          owner.calls.update == 0 && state.dampPrevValid,
          "first eligible call makes staging and queues readback without Map");
    verifyWindow(closeWindow(true), 1, 1, mask({kGetDevice, kCopy}),
                 "first eligible call attributes only GetDevice and CopyResource");

    g_now += 16;
    owner.calls = {};
    openWindow(ownerCtx, true);
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get());
    check(owner.calls.getDevice == 0 && owner.calls.copy == 1 &&
          owner.calls.map == 1 && owner.calls.unmap == 1 &&
          owner.calls.update == 2 && state.dampWrites == 1,
          "successful real WARP readback damps and updates both eye strips");
    verifyWindow(closeWindow(true), 1, 4,
                 mask({kCopy, kMap, kUnmap, kWriteA, kWriteB}),
                 "successful path attributes Map, Unmap, and both writes once");

    owner.failMap = true;
    owner.calls = {};
    openWindow(ownerCtx, true);
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get());
    check(owner.calls.copy == 1 && owner.calls.map == 1 &&
          owner.calls.unmap == 0 && owner.calls.update == 0 &&
          state.dampWrites == 1,
          "failed Map is attempted but cannot Unmap or write either strip");
    verifyWindow(closeWindow(true), 1, 1, mask({kCopy, kMap}),
                 "failed Map is counted without a false Unmap or write note");
    owner.failMap = false;

    owner.calls = {};
    openWindow(ownerCtx, false);
    edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get());
    check(owner.calls.copy == 1 && owner.calls.map == 1 &&
          owner.calls.unmap == 1 && owner.calls.update == 2,
          "unsampled owner still executes the production damper");
    verifyWindow(closeWindow(false), 0, 0, 0,
                 "closed sample frame emits no Exposure notes");

    foreign.calls = {};
    openWindow(ownerCtx, true);
    edvr::plugins::exposure::exposurePluginDamp(&state, foreignCtx, viewA.Get());
    check(foreign.calls.copy == 1 && foreign.calls.map == 1 &&
          foreign.calls.unmap == 1 && foreign.calls.update == 2,
          "foreign context still executes the production damper");
    verifyWindow(closeWindow(true), 0, 0, 0,
                 "foreign context cannot claim owner API cost");

    owner.calls = {};
    openWindow(ownerCtx, true);
    std::thread worker([&] {
        edvr::plugins::exposure::exposurePluginDamp(&state, ownerCtx, viewA.Get());
    });
    worker.join();
    check(owner.calls.copy == 1 && owner.calls.map == 1 &&
          owner.calls.unmap == 1 && owner.calls.update == 2,
          "foreign thread still executes the production damper");
    verifyWindow(closeWindow(true), 0, 0, 0,
                 "foreign thread cannot claim owner API cost");

    edvrPluginCostShutdown();
    edvr::g_state = nullptr;
    edvr::g_clockForTest = nullptr;
    g_boundStrip = nullptr;
    check(state.dampStaging[0] != nullptr && state.dampStaging[1] != nullptr,
          "damper lifecycle owns the real WARP staging pair before shutdown");
    // Keep independent resource references to check real texture usability
    // after shutdown, without relying on driver-specific COM refcount values.
    ComPtr<ID3D11Texture2D> stagingAnchors[2];
    stagingAnchors[0] = state.dampStaging[0];
    stagingAnchors[1] = state.dampStaging[1];
    edvr::plugins::exposure::exposurePluginShutdownResources(&state);
    check(state.dampStaging[0] == nullptr && state.dampStaging[1] == nullptr,
          "production shutdown clears both real WARP staging handles");
    edvr::plugins::exposure::exposurePluginShutdownResources(&state);
    check(state.dampStaging[0] == nullptr && state.dampStaging[1] == nullptr,
          "repeated production shutdown keeps the staging pair clear");
    for (const auto& staging : stagingAnchors) {
        D3D11_TEXTURE2D_DESC stagingDesc{};
        if (staging) staging->GetDesc(&stagingDesc);
        check(stagingDesc.Width == 6 && stagingDesc.Height == 1 &&
                  stagingDesc.Format == DXGI_FORMAT_R32_FLOAT &&
                  stagingDesc.Usage == D3D11_USAGE_STAGING &&
                  stagingDesc.CPUAccessFlags == D3D11_CPU_ACCESS_READ,
              "independently retained WARP staging texture survives repeated cleanup");
    }
    std::printf("exposure_cost_test: %s (%u checks)\n",
                g_failures ? "FAILED" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}
