#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../src/common/plugin_cost.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/remlok_fix.h"

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;
namespace ro = edvr::remlok_observation;

namespace edvr {
// Linking the production RemLok translation unit also brings its scissor
// sizing code. This rig exercises recognition only, never ScissorBegin/End;
// abort rather than fabricate headset data if that boundary is crossed.
bool eyeTangents(float*, float*) { std::abort(); }
uint32_t cullGuardStatePacked() { std::abort(); }
}  // namespace edvr

namespace {

unsigned g_checks = 0;
unsigned g_failed = 0;
uint32_t g_preDescriptionFaultCalls = 0;
uint32_t g_modeFlipGetResourceCalls = 0;
uint32_t g_releaseFaultCalls = 0;
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
ReleaseFn g_realRelease = nullptr;

bool check(bool pass, const char* label) {
    ++g_checks;
    if (!pass) {
        ++g_failed;
        std::printf("FAIL: %s\n", label);
    }
    return pass;
}

template <class T>
bool readIs(const ro::Read<T>& read, bool reached, bool known, T value) {
    return read.reached == reached && read.known == known && read.value == value;
}

template <class T>
void checkRead(const ro::Read<T>& read, bool reached, bool known, T value,
               const char* label) {
    check(readIs(read, reached, known, value), label);
}

void checkMutation(const ro::Observation& o,
                   uint32_t matchesBefore, uint32_t matchesAfter,
                   uint64_t hiddenBefore, uint64_t hiddenAfter,
                   bool pendingBefore, bool pendingAfter,
                   const char* label) {
    checkRead(o.mutation.matchesBefore, true, true, matchesBefore, label);
    checkRead(o.mutation.matchesAfter, true, true, matchesAfter, label);
    checkRead(o.mutation.hiddenBefore, true, true, hiddenBefore, label);
    checkRead(o.mutation.hiddenAfter, true, true, hiddenAfter, label);
    checkRead(o.mutation.pendingRightBefore, true, true, pendingBefore, label);
    checkRead(o.mutation.pendingRightAfter, true, true, pendingAfter, label);
}

edvr::RemlokAction observe(char kind, uint32_t count, uint32_t instances,
                           uint32_t outerMode, ro::Observation& out) {
    // This is the caller-owned read. The helper must preserve it.
    out.selector.outerMode = {true, true, outerMode};
    const auto action = edvr::remlokOnEyeDrawObserved(kind, count, instances, out);
    checkRead(out.selector.outerMode, true, true, outerMode,
              "helper preserves the caller's outer mode read");
    return action;
}

// Typed ID3D11View fixtures inject a pre-description fault or change the raw
// RemLok mode during GetResource. The mode-change case proves the late hide
// comparison performs its own source read after the actual COM query.
class InjectedView final : public ID3D11View {
public:
    enum class Behavior { Fault, ChangeMode };
    InjectedView(Behavior behavior, ID3D11ShaderResourceView* real = nullptr)
        : behavior_(behavior), real_(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override {
        if (out) *out = nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** out) override {
        if (behavior_ == Behavior::Fault) {
            ++g_preDescriptionFaultCalls;
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
            return;
        }
        ++g_modeFlipGetResourceCalls;
        real_->GetResource(out);
        edvr::remlokPredicateTestSetMode(ro::kModeHide);
    }
private:
    Behavior behavior_;
    ID3D11ShaderResourceView* real_;
    ULONG refs_ = 1;
};

ULONG STDMETHODCALLTYPE faultRelease(IUnknown* self) {
    ++g_releaseFaultCalls;
    const ULONG refs = g_realRelease ? g_realRelease(self) : 0;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return refs;
}

bool createTexture(ID3D11Device* device, UINT width, UINT height,
                   DXGI_FORMAT format, UINT bindFlags,
                   ID3D11Texture2D** out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bindFlags;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, out));
}

bool createSrv(ID3D11Device* device, ID3D11Texture2D* texture,
               ID3D11ShaderResourceView** out) {
    return SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, out));
}

void startCostWindow(ID3D11DeviceContext* context) {
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(1, 1000000);
    edvrPluginCostSetOwnerContext(context);
    EdvrPluginCostWindowV1 discarded{};
    check(edvrPluginCostFrameBoundary(0, 0, 0, 1, 0, &discarded) == 0,
          "owner bootstrap boundary is discarded");
    check(edvrPluginCostApiSampleOwnerThread() != 0,
          "API sample is open on the registered owner thread");
}

EdvrPluginCostWindowV1 closeCostWindow() {
    EdvrPluginCostWindowV1 window{};
    check(edvrPluginCostFrameBoundary(1, 0, 1, 0, 0, &window) == 0,
          "one sampled API frame stays inside the report window");
    uint8_t completed = 0;
    for (uint32_t frame = 2; frame <= 1800; ++frame) {
        completed = edvrPluginCostFrameBoundary(frame, 0, 0, 0, 0, &window);
    }
    check(completed != 0 && window.windowFrames == 1800,
          "real resolver notes close in the standard 1800-frame window");
    return window;
}

void checkCostWindow(const EdvrPluginCostWindowV1& window) {
    const auto& core = window.owners[static_cast<uint8_t>(pc::Owner::Core)];
    const auto readQuery = static_cast<uint8_t>(pc::ApiClass::ReadQuery);
    const uint64_t queryMask = (uint64_t{1} << 48) | (uint64_t{1} << 49) |
                               (uint64_t{1} << 50) | (uint64_t{1} << 51);
    check(window.completedApiSampleFrames == 1,
          "RemLok recognition leaves the completed API-frame denominator at one");
    check(core.apiCalls[readQuery] == 43,
          "Core ReadQuery count matches the real resolver's 43 COM query attempts");
    // Buffer, narrow and short views: 9 queries. Ten matching texture draws:
    // 30 queries (including mode-change and Release-fault paths). One injected
    // pre-description fault plus three stale views: 4 GetResource attempts.
    check(core.apiSiteMask[0] == 0 && core.apiSiteMask[1] == queryMask,
          "resolver query coverage is exactly sites 112 through 115");
    check(core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 0 &&
              core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 0 &&
              core.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0 &&
              core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0,
          "RemLok recognition adds only Core ReadQuery observations");
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        else if (std::strcmp(argv[i], "--self-test") != 0) {
            std::fprintf(stderr, "unsupported argument: %s\n", argv[i]);
            return 2;
        }
    }
    // No device, hooks, resolver calls or logger work on the dry-run path.
    if (dryRun) {
        std::puts("remlok-predicate-test dry-run: no work performed");
        return 0;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const auto createDevice = edvr::systemD3D11CreateDevice();
    if (!check(createDevice != nullptr, "resolve System32 D3D11CreateDevice")) return 1;
    const HRESULT created = createDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context);
    if (!check(SUCCEEDED(created), "create WARP device")) return 1;
    if (!check(edvr::reportSystemD3D11Only("remlok_predicate_test"),
               "WARP uses only System32 d3d11")) return 1;

    ComPtr<ID3D11Texture2D> goodTexture, narrowTexture, shortTexture, depthTexture;
    ComPtr<ID3D11ShaderResourceView> goodView, narrowView, shortView;
    ComPtr<ID3D11DepthStencilView> depthView;
    if (!check(createTexture(device.Get(), 1024, 512, DXGI_FORMAT_R8G8B8A8_UNORM,
                             D3D11_BIND_SHADER_RESOURCE, &goodTexture),
               "create matching 1024x512 texture") ||
        !check(createTexture(device.Get(), 1023, 512, DXGI_FORMAT_R8G8B8A8_UNORM,
                             D3D11_BIND_SHADER_RESOURCE, &narrowTexture),
               "create wrong-width texture") ||
        !check(createTexture(device.Get(), 1024, 511, DXGI_FORMAT_R8G8B8A8_UNORM,
                             D3D11_BIND_SHADER_RESOURCE, &shortTexture),
               "create wrong-height texture") ||
        !check(createSrv(device.Get(), goodTexture.Get(), &goodView), "create matching SRV") ||
        !check(createSrv(device.Get(), narrowTexture.Get(), &narrowView), "create narrow SRV") ||
        !check(createSrv(device.Get(), shortTexture.Get(), &shortView), "create short SRV")) return 1;

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = 1024;
    depthDesc.Height = 512;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (!check(SUCCEEDED(device->CreateTexture2D(&depthDesc, nullptr, &depthTexture)),
               "create DSV backing texture") ||
        !check(SUCCEEDED(device->CreateDepthStencilView(depthTexture.Get(), nullptr,
                                                        &depthView)),
               "create real DSV")) return 1;

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 256;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = 16;
    ComPtr<ID3D11Buffer> buffer;
    D3D11_SHADER_RESOURCE_VIEW_DESC bufferSrvDesc{};
    bufferSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    bufferSrvDesc.Buffer.NumElements = bufferDesc.ByteWidth / bufferDesc.StructureByteStride;
    ComPtr<ID3D11ShaderResourceView> bufferView;
    if (!check(SUCCEEDED(device->CreateBuffer(&bufferDesc, nullptr, &buffer)),
               "create structured buffer") ||
        !check(SUCCEEDED(device->CreateShaderResourceView(buffer.Get(), &bufferSrvDesc,
                                                          &bufferView)),
               "create typed structured-buffer SRV")) return 1;

    startCostWindow(context.Get());
    edvr::bindingSet(edvr::BindSlot::Dsv0, nullptr);
    edvr::bindingSet(edvr::BindSlot::PsSrv0, goodView.Get());

    ro::Observation observation{};
    edvr::remlokPredicateTestSeed(ro::kModeStock, false, 10, 7, true);
    check(observe('N', 3, 1, ro::kModeStock, observation) == edvr::RemlokAction::kNone,
          "stock mode declines a matching shape");
    checkRead(observation.helper.modeBeforeGate, true, true,
              static_cast<uint32_t>(ro::kModeStock),
              "stock mode read is captured before the helper gate");
    checkRead(observation.helper.dsvNonNull, false, false, false,
              "stock mode skips the DSV read");
    checkRead(observation.helper.resolved, false, false, false,
              "stock mode skips the resource resolver");
    checkMutation(observation, 10, 10, 7, 7, true, true,
                  "stock mode leaves mutation state unchanged");

    edvr::remlokPredicateTestSeed(ro::kModeOuter, false, 0, 0, false);
    observation = {};
    check(observe('D', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "wrong draw kind declines before binding reads");
    checkRead(observation.helper.dsvNonNull, false, false, false,
              "wrong shape skips DSV inspection");
    checkMutation(observation, 0, 0, 0, 0, false, false,
                  "wrong shape leaves all mutation reads unchanged");

    edvr::bindingSet(edvr::BindSlot::Dsv0, depthView.Get());
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "a real bound DSV rejects an otherwise matching shape");
    checkRead(observation.helper.dsvNonNull, true, true, true,
              "DSV presence is recorded as the rejection");
    checkRead(observation.helper.resolved, false, false, false,
              "DSV rejection skips resource resolution");
    checkMutation(observation, 0, 0, 0, 0, false, false,
                  "DSV decline leaves all mutation reads unchanged");
    edvr::bindingSet(edvr::BindSlot::Dsv0, nullptr);

    edvr::bindingSet(edvr::BindSlot::PsSrv0, nullptr);
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "null SRV is a known resolver negative");
    checkRead(observation.helper.resolved, true, true, false,
              "null SRV resolver result is recorded as known false");
    checkRead(observation.helper.isTexture2D, false, false, false,
              "failed resolution skips resource type and dimensions");
    checkMutation(observation, 0, 0, 0, 0, false, false,
                  "null SRV decline leaves all mutation reads unchanged");

    edvr::bindingSet(edvr::BindSlot::PsSrv0, bufferView.Get());
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "real buffer SRV is rejected by the Texture2D predicate");
    checkRead(observation.helper.isTexture2D, true, true, false,
              "buffer type is a known false predicate");
    checkRead(observation.helper.width, false, false, uint32_t{0},
              "buffer path skips dimensions");
    checkMutation(observation, 0, 0, 0, 0, false, false,
                  "wrong resource type leaves all mutation reads unchanged");

    edvr::bindingSet(edvr::BindSlot::PsSrv0, narrowView.Get());
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "wrong width rejects matching kind and resource type");
    checkRead(observation.helper.width, true, true, uint32_t{1023},
              "width is the consumed rejection value");
    checkRead(observation.helper.height, false, false, uint32_t{0},
              "width mismatch short-circuits height");
    checkMutation(observation, 0, 0, 0, 0, false, false,
                  "wrong width leaves all mutation reads unchanged");

    edvr::bindingSet(edvr::BindSlot::PsSrv0, shortView.Get());
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "wrong height rejects the matching width");
    checkRead(observation.helper.width, true, true, uint32_t{1024},
              "matching width is recorded");
    checkRead(observation.helper.height, true, true, uint32_t{511},
              "height is read after matching width");
    checkMutation(observation, 0, 0, 0, 0, false, false,
                  "wrong height leaves all mutation reads unchanged");

    edvr::bindingSet(edvr::BindSlot::PsSrv0, goodView.Get());
    edvr::remlokPredicateTestSeed(ro::kModeOuter, false, 0, 0, false);
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kScissor,
          "first real overlay match selects scissor");
    checkMutation(observation, 0, 1, 0, 0, false, false,
                  "first match increments the actual match counter");
    checkRead(observation.helper.hideMode, true, true,
              static_cast<uint32_t>(ro::kModeOuter),
              "late hide-mode read occurs after a successful match");
    checkRead(observation.helper.swap, true, true, false,
              "outer mode reads swap only after the match");

    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kScissor,
          "second real overlay match selects scissor");
    checkMutation(observation, 1, 2, 0, 0, false, true,
                  "second arrival is assigned to the right eye");

    const auto beforeBoundary = edvr::remlokPredicateTestSnapshot();
    edvr::remlokFrameBoundary();
    const auto afterBoundary = edvr::remlokPredicateTestSnapshot();
    check(beforeBoundary.matchesAfter.value == 2 && afterBoundary.matchesAfter.value == 0 &&
              beforeBoundary.hiddenAfter.value == afterBoundary.hiddenAfter.value &&
              beforeBoundary.pendingRightAfter.value && afterBoundary.pendingRightAfter.value,
          "frame boundary resets only match parity and leaves other state intact");

    edvr::remlokPredicateTestSeed(ro::kModeOuter, true, 0, 0, false);
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kScissor,
          "swap mode still selects the outer scissor action");
    checkMutation(observation, 0, 1, 0, 0, false, true,
                  "swap exchanges first-arrival eye parity");
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kScissor,
          "second swapped match still selects scissor");
    checkMutation(observation, 1, 2, 0, 0, true, false,
                  "swap exchanges second-arrival eye parity");

    edvr::remlokPredicateTestSeed(ro::kModeHide, true, 4, 8, true);
    observation = {};
    check(observe('N', 3, 1, ro::kModeHide, observation) == edvr::RemlokAction::kHide,
          "hide mode suppresses a matching overlay");
    checkMutation(observation, 4, 5, 8, 9, true, true,
                  "hide advances match and hidden counters without changing pending eye");
    checkRead(observation.helper.hideMode, true, true,
              static_cast<uint32_t>(ro::kModeHide),
              "hide comparison uses the separate late mode read");
    checkRead(observation.helper.swap, false, false, false,
              "hide path skips swap read");

    edvr::remlokPredicateTestSeed(7, true, 0, 0, false);
    observation = {};
    check(observe('N', 3, 1, 91, observation) == edvr::RemlokAction::kScissor,
          "unknown raw mode follows frozen non-stock, non-hide behavior");
    checkRead(observation.helper.modeBeforeGate, true, true, uint32_t{7},
              "unknown raw mode is preserved");
    checkRead(observation.helper.hideMode, true, true, uint32_t{7},
              "late mode is read independently and preserves unknown values");

    edvr::remlokPredicateTestSeed(ro::kModeOuter, false, 0, 0, false);
    InjectedView changingView(InjectedView::Behavior::ChangeMode, goodView.Get());
    edvr::bindingSet(edvr::BindSlot::PsSrv0, &changingView);
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kHide,
          "a mode change during GetResource is seen by the late hide read");
    check(g_modeFlipGetResourceCalls == 1,
          "mode-changing wrapper forwards one real view GetResource call");
    checkRead(observation.helper.modeBeforeGate, true, true,
              static_cast<uint32_t>(ro::kModeOuter),
              "pre-query mode read remains outer");
    checkRead(observation.helper.hideMode, true, true,
              static_cast<uint32_t>(ro::kModeHide),
              "post-query mode read sees hide independently");
    checkMutation(observation, 0, 1, 0, 1, false, false,
                  "mode-only change preserves consumed match and hidden counter snapshots");

    // End the mode-change scenario: its injected SRV otherwise changes this
    // parity-only case to hide during GetResource as well.
    edvr::bindingSet(edvr::BindSlot::PsSrv0, goodView.Get());
    edvr::remlokPredicateTestSeed(ro::kModeOuter, false, UINT32_MAX, 3, false);
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kScissor,
          "match parity remains defined at UINT32_MAX");
    checkMutation(observation, UINT32_MAX, 0, 3, 3, false, true,
                  "unsigned match counter wraps and retains low-bit eye parity");

    edvr::remlokPredicateTestSeed(ro::kModeHide, false, 10, UINT64_MAX, false);
    observation = {};
    check(observe('N', 3, 1, ro::kModeHide, observation) == edvr::RemlokAction::kHide,
          "hide path remains active at UINT64_MAX hidden count");
    checkMutation(observation, 10, 11, UINT64_MAX, 0, false, false,
                  "unsigned hidden counter wraps without changing pending eye");

    // A typed SDK-interface test double injects an exception from GetResource,
    // before describeResource can call GetType.
    edvr::remlokPredicateTestSeed(ro::kModeOuter, false, 20, 5, false);
    InjectedView faultingView(InjectedView::Behavior::Fault);
    edvr::bindingSet(edvr::BindSlot::PsSrv0, &faultingView);
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "pre-description COM fault is absorbed as a known resolver negative");
    check(g_preDescriptionFaultCalls == 1,
          "injected GetResource fault callback ran exactly once");
    checkRead(observation.helper.resolved, true, true, false,
              "guarded bindingResolve false is retained as a known negative");
    checkRead(observation.helper.isTexture2D, false, false, false,
              "pre-description fault skips type and dimensions");
    checkMutation(observation, 20, 20, 5, 5, false, false,
                  "pre-description fault does not mutate match state");

    // IUnknown::Release is vtable slot 2 by the typed COM ABI. The hook forwards
    // the real WARP Release and then raises SEH, exercising the resolver's
    // post-description return-value behavior without replacing its COM object.
    edvr::VTableHook releaseHook;
    check(releaseHook.attach(goodTexture.Get(), 16), "attach typed-resource Release hook");
    check(releaseHook.setMode(edvr::HookMode::CopyVptr), "select private resource vtable copy");
    void* realRelease = nullptr;
    check(releaseHook.replace(2, reinterpret_cast<void*>(&faultRelease), &realRelease),
          "stage the typed IUnknown::Release fault");
    g_realRelease = reinterpret_cast<ReleaseFn>(realRelease);
    check(releaseHook.commit(), "commit Release fault hook");
    edvr::remlokPredicateTestSeed(ro::kModeOuter, false, 30, 5, false);
    edvr::bindingSet(edvr::BindSlot::PsSrv0, goodView.Get());
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kScissor,
          "successful description remains matched when Release faults afterwards");
    releaseHook.uninstall();
    g_realRelease = nullptr;
    check(g_releaseFaultCalls == 1,
          "post-description Release callback ran exactly once");
    checkRead(observation.helper.resolved, true, true, true,
              "Release fault preserves bindingResolve's completed describe result");
    checkMutation(observation, 30, 31, 5, 5, false, false,
                  "Release fault follows the successful match mutation");

    // The two caught faults above spend two of the real view resolver's five
    // credits. Three stale calls consume the rest; the next live view is a
    // known negative because its exhausted budget refuses the callback.
    void* stale = reinterpret_cast<void*>(0x10);
    edvr::bindingSet(edvr::BindSlot::PsSrv0, stale);
    for (uint32_t i = 0; i < 3; ++i) {
        observation = {};
        check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
              "remaining invalid-view faults are contained by the real resolver budget");
        checkRead(observation.helper.resolved, true, true, false,
                  "invalid view resolver result is known false");
        checkMutation(observation, 31, 31, 5, 5, false, false,
                      "invalid view decline leaves mutation state unchanged");
    }
    edvr::bindingSet(edvr::BindSlot::PsSrv0, goodView.Get());
    observation = {};
    check(observe('N', 3, 1, ro::kModeOuter, observation) == edvr::RemlokAction::kNone,
          "exhausted resolver budget rejects a live matching view");
    checkRead(observation.helper.resolved, true, true, false,
              "budget refusal remains a known-negative resolver result");
    checkRead(observation.helper.isTexture2D, false, false, false,
              "budget refusal skips type and dimensions");
    checkMutation(observation, 31, 31, 5, 5, false, false,
                  "budget decline leaves mutation state unchanged");

    const auto window = closeCostWindow();
    checkCostWindow(window);
    edvrPluginCostShutdown();
    edvr::remlokShutdown();
    edvr::bindingForgetAll();

    if (g_failed == 0) {
        std::printf("remlok_predicate_test PASS (%u checks)\n", g_checks);
        return 0;
    }
    std::printf("remlok_predicate_test FAIL (%u/%u failed)\n", g_failed, g_checks);
    return 1;
}
