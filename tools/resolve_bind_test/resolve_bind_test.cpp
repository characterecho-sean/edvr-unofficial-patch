#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "../../src/common/config.h"
#include "../../src/common/plugin_cost.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/binding_cost_sites.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/resolve_bind_fix.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint64_t kResolveHash = 0x7CECABDE34FFBE9EULL;
ID3D11PixelShader* g_resolveShader = nullptr;
ID3D11PixelShader* g_otherShader = nullptr;

using GetPixelShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                ID3D11PixelShader**,
                                                ID3D11ClassInstance**, UINT*);
GetPixelShader g_realGetPixelShader = nullptr;
uint32_t g_getPixelShaderCalls = 0;

void STDMETHODCALLTYPE returnNullResource(ID3D11View*, ID3D11Resource** out) {
    *out = nullptr;
}

using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
ReleaseFn g_realRelease = nullptr;
uint32_t g_releaseFaultCalls = 0;
uint32_t g_descFaultCalls = 0;
void STDMETHODCALLTYPE faultTextureDesc(ID3D11Texture2D*, D3D11_TEXTURE2D_DESC*) {
    ++g_descFaultCalls;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
}

ULONG STDMETHODCALLTYPE faultRelease(IUnknown* self) {
    if (g_realRelease) g_realRelease(self);
    ++g_releaseFaultCalls;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return 0;
}

void STDMETHODCALLTYPE countGetPixelShader(ID3D11DeviceContext* self,
                                            ID3D11PixelShader** shader,
                                            ID3D11ClassInstance** instances,
                                            UINT* count) {
    ++g_getPixelShaderCalls;
    g_realGetPixelShader(self, shader, instances, count);
}

void check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

void hr(HRESULT result, const char* what) {
    if (FAILED(result)) throw std::runtime_error(what);
}

uint64_t costMask(std::initializer_list<edvr::binding_cost::Site> sites) {
    uint64_t mask = 0;
    for (const auto site : sites) {
        const uint16_t id = edvr::binding_cost::id(site);
        mask |= uint64_t{1} << (id - 64);
    }
    return mask;
}

EdvrPluginCostWindowV1 closeCostWindow() {
    EdvrPluginCostWindowV1 window{};
    uint8_t completed = 0;
    // Frame 1 (closed by the caller) contains the only API sample. These 1799
    // boundaries finish the same report window without adding API samples.
    for (uint32_t frame = 2; frame <= edvr::plugin_cost::kWindowFrameCount; ++frame) {
        completed = edvrPluginCostFrameBoundary(frame, 0, 0, 0, 0, &window);
        check((completed != 0) == (frame == edvr::plugin_cost::kWindowFrameCount),
              "resolver cost window reports only at its last boundary");
    }
    check(completed != 0 && window.windowFrames == 1800,
          "resolver query costs stay in the existing 1800-frame window");
    return window;
}

void checkResolverCost(const EdvrPluginCostWindowV1& window, uint64_t getResource,
                       uint64_t getType, uint64_t bufferDesc, uint64_t textureDesc) {
    constexpr uint8_t core = static_cast<uint8_t>(edvr::plugin_cost::Owner::Core);
    constexpr uint8_t readQuery = static_cast<uint8_t>(edvr::plugin_cost::ApiClass::ReadQuery);
    const auto& owner = window.owners[core];
    const uint64_t expectedMask = costMask({edvr::binding_cost::Site::GetResource,
                                            edvr::binding_cost::Site::GetType,
                                            edvr::binding_cost::Site::BufferGetDesc,
                                            edvr::binding_cost::Site::Texture2DGetDesc});
    check(window.completedApiSampleFrames == 1,
          "resolver queries do not change the completed API-frame denominator");
    check(getResource == 16 && getType == 14 && bufferDesc == 2 && textureDesc == 6,
          "scenario expectations enumerate the exact COM query attempts by site");
    check(owner.apiCalls[readQuery] == getResource + getType + bufferDesc + textureDesc,
          "Core ReadQuery count equals the 38 resolver COM query attempts");
    check(owner.apiCalls[static_cast<uint8_t>(edvr::plugin_cost::ApiClass::Work)] == 0 &&
              owner.apiCalls[static_cast<uint8_t>(edvr::plugin_cost::ApiClass::Transfer)] == 0 &&
              owner.apiCalls[static_cast<uint8_t>(edvr::plugin_cost::ApiClass::State)] == 0 &&
              owner.apiCalls[static_cast<uint8_t>(edvr::plugin_cost::ApiClass::Instrumentation)] == 0,
          "resolver COM queries are attributed only to Core ReadQuery");
    check(owner.apiSiteMask[0] == 0 && owner.apiSiteMask[1] == expectedMask,
          "Core coverage mask contains only resolver query sites 112 through 115");
}

ComPtr<ID3DBlob> compilePixelShader(const char* colour) {
    char source[160]{};
    std::snprintf(source, sizeof(source),
                  "float4 main():SV_Target{return float4(%s);}", colour);
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "resolve-bind-test",
                                      nullptr, nullptr, "main", "ps_5_0",
                                      D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result) && errors) {
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    }
    hr(result, "compile pixel shader");
    return code;
}

void bind(ID3D11DeviceContext* context, ID3D11PixelShader* shader, uint64_t hash) {
    context->PSSetShader(shader, nullptr, 0);
    edvr::bindingSetShader(edvr::BindSlot::Ps, shader, hash);
}

}  // namespace

namespace edvr {

uint64_t lookupShaderHash(void* shader) {
    if (shader == g_resolveShader) return kResolveHash;
    if (shader == g_otherShader) return 1;
    return 0;
}

}  // namespace edvr

int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                             D3D11_SDK_VERSION, &device, &level, &context),
           "create WARP device");

        const auto resolveCode = compilePixelShader("1,0,0,1");
        const auto otherCode = compilePixelShader("0,1,0,1");
        ComPtr<ID3D11PixelShader> resolveShader;
        ComPtr<ID3D11PixelShader> otherShader;
        hr(device->CreatePixelShader(resolveCode->GetBufferPointer(),
                                     resolveCode->GetBufferSize(), nullptr,
                                     &resolveShader),
           "create resolve shader");
        hr(device->CreatePixelShader(otherCode->GetBufferPointer(),
                                     otherCode->GetBufferSize(), nullptr, &otherShader),
           "create other shader");
        g_resolveShader = resolveShader.Get();
        g_otherShader = otherShader.Get();

        edvr::VTableHook hook;
        check(hook.attach(context.Get(), 128), "attach context vtable");
        check(hook.setMode(edvr::HookMode::CopyVptr), "select private vtable copy");
        void* original = nullptr;
        constexpr size_t kPsGetShaderSlot = 74;
        check(hook.replace(kPsGetShaderSlot,
                           reinterpret_cast<void*>(&countGetPixelShader), &original),
              "replace PSGetShader");
        g_realGetPixelShader = reinterpret_cast<GetPixelShader>(original);
        check(hook.commit(), "commit PSGetShader counter");

        edvr::Config& config = edvr::Config::get();
        config.set("fix.scanner_body", "on");
        edvr::resolveBindConfigure(config);

        bind(context.Get(), resolveShader.Get(), kResolveHash);
        check(edvr::resolveBindOnEyeDraw(context.Get()), "known resolve matches");
        check(g_getPixelShaderCalls == 0, "known resolve avoids PSGetShader");

        bind(context.Get(), otherShader.Get(), 1);
        check(!edvr::resolveBindOnEyeDraw(context.Get()), "known non-resolve declines");
        check(g_getPixelShaderCalls == 0, "known non-resolve avoids PSGetShader");

        // The draw path's inline pre-check (resolve_bind_fix.h) may skip the
        // call only where the shadow alone already says no: a held shader
        // with a known hash that is not the resolve's. Unknown (no pointer,
        // or a zero hash) and the resolve itself must still reach the call.
        check(edvr::resolveBindShadowSaysNo(true, 1), "inline pre-check: known non-resolve");
        check(!edvr::resolveBindShadowSaysNo(true, kResolveHash), "inline pre-check: known resolve reaches the call");
        check(!edvr::resolveBindShadowSaysNo(true, 0) && !edvr::resolveBindShadowSaysNo(false, 1),
              "inline pre-check: an unknown shadow reaches the call");
        check(edvr::detail::kResolveBindPs == kResolveHash, "inline pre-check names the resolve's hash");

        // A shader can be bound before the registry learns its hash. The real
        // getter remains authoritative, and a successful lookup repairs the
        // shadow so the following draw takes the fast path.
        bind(context.Get(), resolveShader.Get(), 0);
        check(edvr::resolveBindOnEyeDraw(context.Get()), "zero hash falls back");
        check(g_getPixelShaderCalls == 1, "zero hash calls PSGetShader once");
        check(edvr::bindingGet(edvr::BindSlot::Ps) == resolveShader.Get() &&
                  edvr::bindingShaderHash(edvr::BindSlot::Ps) == kResolveHash,
              "fallback relearns resolve shader");
        check(edvr::resolveBindOnEyeDraw(context.Get()), "relearned resolve matches");
        check(g_getPixelShaderCalls == 1, "relearned resolve uses fast path");

        // ExecuteCommandList(FALSE) invalidates the shadow after applying the
        // list's state. Forgetting here models that production hook boundary.
        context->PSSetShader(otherShader.Get(), nullptr, 0);
        edvr::bindingForgetAll();
        check(!edvr::resolveBindOnEyeDraw(context.Get()),
              "forgotten shadow reads actual non-resolve");
        check(g_getPixelShaderCalls == 2, "forgotten shadow calls PSGetShader");
        check(edvr::bindingGet(edvr::BindSlot::Ps) == otherShader.Get() &&
                  edvr::bindingShaderHash(edvr::BindSlot::Ps) == 1,
              "fallback relearns command-list shader");
        check(!edvr::resolveBindOnEyeDraw(context.Get()),
              "relearned non-resolve declines");
        check(g_getPixelShaderCalls == 2, "relearned non-resolve uses fast path");

        bind(context.Get(), resolveShader.Get(), kResolveHash);
        check(edvr::resolveBindOnEyeDraw(context.Get()), "rebind updates classification");
        check(g_getPixelShaderCalls == 2, "rebind stays on fast path");

        // ClearState makes null real state and a null shadow. Null is kept as
        // unknown, so it must still consult the real context rather than claim
        // a cached non-match.
        context->ClearState();
        edvr::bindingForgetAll();
        check(!edvr::resolveBindOnEyeDraw(context.Get()), "null state declines");
        check(g_getPixelShaderCalls == 3, "null state uses PSGetShader fallback");

        // The cache change must leave the repair around a matching draw intact:
        // cache a healthy vertex buffer, lend it to the empty draw, then restore
        // the empty binding after that draw.
        bind(context.Get(), resolveShader.Get(), kResolveHash);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 20;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ComPtr<ID3D11Buffer> vertexBuffer;
        hr(device->CreateBuffer(&desc, nullptr, &vertexBuffer), "create vertex buffer");
        ID3D11Buffer* bound = vertexBuffer.Get();
        UINT stride = 20, offset = 4;
        context->IASetVertexBuffers(0, 1, &bound, &stride, &offset);
        check(edvr::resolveBindOnEyeDraw(context.Get()), "matching repair draw classified");
        edvr::resolveBindBegin(context.Get());
        edvr::resolveBindEnd(context.Get());

        bound = nullptr;
        UINT zero = 0;
        context->IASetVertexBuffers(0, 1, &bound, &zero, &zero);
        edvr::resolveBindBegin(context.Get());
        ComPtr<ID3D11Buffer> lent;
        UINT lentStride = 0, lentOffset = 0;
        context->IAGetVertexBuffers(0, 1, &lent, &lentStride, &lentOffset);
        check(lent.Get() == vertexBuffer.Get() && lentStride == stride &&
                  lentOffset == offset,
              "matching draw receives cached vertex buffer");
        edvr::resolveBindEnd(context.Get());
        lent.Reset();
        context->IAGetVertexBuffers(0, 1, &lent, &lentStride, &lentOffset);
        check(!lent, "repair restores empty vertex binding");

        // Exercise the production COM resolvers with real WARP views and resources.
        // The vtable hooks below only inject a null GetResource result and two SEH
        // faults; the measured resolver and collector remain the production code.
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 16;
            td.Height = 8;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture;
            hr(device->CreateTexture2D(&td, nullptr, &texture), "create texture");
            ComPtr<ID3D11ShaderResourceView> view;
            hr(device->CreateShaderResourceView(texture.Get(), nullptr, &view),
               "create view");

            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 256;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 16;
            ComPtr<ID3D11Buffer> buffer;
            hr(device->CreateBuffer(&bd, nullptr, &buffer), "create resolver buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC bufferSrvDesc{};
            bufferSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            bufferSrvDesc.Buffer.NumElements = bd.ByteWidth / bd.StructureByteStride;
            ComPtr<ID3D11ShaderResourceView> bufferView;
            hr(device->CreateShaderResourceView(buffer.Get(), &bufferSrvDesc, &bufferView),
               "create buffer view");

            D3D11_TEXTURE1D_DESC t1d{};
            t1d.Width = 16;
            t1d.MipLevels = 1;
            t1d.ArraySize = 1;
            t1d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            t1d.Usage = D3D11_USAGE_DEFAULT;
            t1d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture1D> texture1d;
            hr(device->CreateTexture1D(&t1d, nullptr, &texture1d), "create unsupported 1D resource");

            edvrPluginCostShutdown();
            edvrPluginCostConfigure(1, 1000000);
            edvrPluginCostSetOwnerContext(context.Get());
            check(edvrPluginCostApiSampleOwnerThread() == 0,
                  "API cost sampling is closed before the owner boundary");
            EdvrPluginCostWindowV1 discarded{};
            check(edvrPluginCostFrameBoundary(0, 0, 0, 1, 0, &discarded) == 0,
                  "owner bootstrap boundary is discarded");
            check(edvrPluginCostApiSampleOwnerThread() != 0,
                  "registered owner thread enters the open API sample frame");
            check(edvrPluginCostApiSampleContext(context.Get()) != 0,
                  "registered immediate context is eligible in the API sample frame");

            edvr::ResourceInfo info;
            void* stale = reinterpret_cast<void*>(0x10);   // no object there: the call faults

            check(edvr::bindingResolve(view.Get(), &info) && info.isTexture2D &&
                      info.a == 16 && info.b == 8,
                  "a real texture view resolves for a fix");
            check(edvr::bindingResolveProbe(view.Get(), &info) && info.isTexture2D &&
                      info.resource == static_cast<ID3D11Resource*>(texture.Get()),
                  "the real texture view resolves for a probe to the same resource");

            check(edvr::bindingResolve(bufferView.Get(), &info) && info.isBuffer &&
                      info.a == bd.ByteWidth,
                  "a real buffer view uses the typed buffer descriptor path");
            check(edvr::bindingResolveProbe(bufferView.Get(), &info) && info.isBuffer,
                  "the probe resolves the real buffer view");

            check(!edvr::bindingResolveResource(texture1d.Get(), &info),
                  "unsupported resource dimension stops after GetType");

            // ID3D11View::GetResource is slot 7 after IUnknown and
            // ID3D11DeviceChild; this typed callback models a live view that
            // returns no resource.
            edvr::VTableHook nullResourceHook;
            check(nullResourceHook.attach(view.Get(), 16), "attach null-resource view hook");
            check(nullResourceHook.setMode(edvr::HookMode::CopyVptr), "select view copy hook");
            check(nullResourceHook.replace(7, reinterpret_cast<void*>(&returnNullResource), nullptr),
                  "stage null-resource GetResource hook");
            check(nullResourceHook.commit(), "commit null-resource GetResource hook");
            check(!edvr::bindingResolve(view.Get(), &info),
                  "null GetResource output is not described or released");
            nullResourceHook.uninstall();

            check(edvr::bindingResolveResourceProbe(texture.Get(), &info) &&
                      info.isTexture2D && info.resource == texture.Get(),
                  "raw-resource resolver gets type and texture description without GetResource");

            // ID3D11Texture2D::GetDesc is slot 10 after IUnknown,
            // ID3D11DeviceChild, and ID3D11Resource. Raise a real SEH exception
            // from that typed COM entry to verify the guarded query-fault path.
            edvr::VTableHook queryFaultHook;
            check(queryFaultHook.attach(texture.Get(), 16), "attach texture query-fault hook");
            check(queryFaultHook.setMode(edvr::HookMode::CopyVptr), "select texture copy hook");
            check(queryFaultHook.replace(10, reinterpret_cast<void*>(&faultTextureDesc), nullptr),
                  "stage GetDesc fault injection");
            check(queryFaultHook.commit(), "commit GetDesc fault injection");
            check(!edvr::bindingResolveResourceProbe(texture.Get(), &info),
                  "GetDesc SEH fault is absorbed by the resource resolver");
            queryFaultHook.uninstall();
            check(g_descFaultCalls == 1,
                  "raw-resource resolver reached the injected GetDesc fault exactly once");

            for (int i = 0; i < 6; ++i) {
                check(!edvr::bindingResolveProbe(stale, &info),
                      "probe view resolver absorbs invalid-pointer faults");
            }
            check(!edvr::bindingResolveProbe(view.Get(), &info),
                  "probe view budget refuses a live view after five faults");
            check(edvr::bindingResolve(view.Get(), &info) && info.isTexture2D,
                  "fix view resolver keeps working after probe budget exhaustion");

            // Release is deliberately outside the measured set. Preserve the
            // resolver's established result semantics: a successful describe
            // leaves ok=true even if the subsequent Release faults.
            edvr::VTableHook releaseFaultHook;
            check(releaseFaultHook.attach(texture.Get(), 16), "attach Release fault hook");
            check(releaseFaultHook.setMode(edvr::HookMode::CopyVptr), "select Release copy hook");
            void* realRelease = nullptr;
            check(releaseFaultHook.replace(2, reinterpret_cast<void*>(&faultRelease), &realRelease),
                  "stage Release fault injection");
            g_realRelease = reinterpret_cast<ReleaseFn>(realRelease);
            check(releaseFaultHook.commit(), "commit Release fault injection");
            check(edvr::bindingResolve(view.Get(), &info) && info.isTexture2D,
                  "successful description remains resolved when Release faults");
            releaseFaultHook.uninstall();
            g_realRelease = nullptr;
            check(g_releaseFaultCalls == 1,
                  "view resolver reached the injected Release fault exactly once");

            // The fixes and probes own separate fault budgets. Each absorbs five
            // invalid calls; exhausted budgets then refuse a live object without
            // adding query cost.
            for (int i = 0; i < 6; ++i) {
                check(!edvr::bindingResolve(stale, &info),
                      "fix view resolver absorbs remaining faults after the Release fault");
            }
            check(!edvr::bindingResolve(view.Get(), &info),
                  "spent fix view budget refuses a live view without querying it");
            for (int i = 0; i < 6; ++i) {
                check(!edvr::bindingResolveProbe(stale, &info),
                      "spent probe view budget refuses invalid pointers without querying");
            }
            check(!edvr::bindingResolveProbe(view.Get(), &info),
                  "spent probe view budget refuses a live view without querying it");
            for (int i = 0; i < 6; ++i) {
                check(!edvr::bindingResolveResource(stale, &info),
                      "fix resource resolver absorbs faults and exhausts its remaining budget");
            }
            check(!edvr::bindingResolveResource(texture.Get(), &info),
                  "spent fix resource budget refuses a live resource without querying it");

            // Close one API sample, then make one more real query in an open
            // report window with API sampling disabled. Its results remain
            // correct and it cannot inflate the completed API sample count.
            check(edvrPluginCostFrameBoundary(1, 0, 1, 0, 0, &discarded) == 0,
                  "one sampled frame is accumulated without closing the report window");
            check(edvrPluginCostApiSampleOwnerThread() == 0,
                  "owner-thread sampling closes at the API frame boundary");
            // The view budget is spent above, so use the still-live resource
            // probe path only after the resource-fault assertions are complete.
            check(edvr::bindingResolveResourceProbe(texture.Get(), &info) && info.isTexture2D,
                  "unsampled probe resource query survives fix resource budget exhaustion");

            const auto window = closeCostWindow();
            // Initial texture+buffer view pairs: GR4/GT4/BD2/TD2.
            // Unsupported raw resource: GT1; null view result: GR1.
            // Raw texture and injected GetDesc fault: GT2/TD2.
            // Probe stale faults: GR5. Fix survivor and Release-fault views:
            // GR2/GT2/TD2. Remaining fix view faults: GR4.
            // Fix raw-resource faults: GT5. Total GR16/GT14/BD2/TD6 = 38.
            // Exhausted-budget and unsampled calls contribute no attempts.
            checkResolverCost(window, 16, 14, 2, 6);
        }

        edvrPluginCostShutdown();
        edvr::resolveBindShutdown();
        edvr::bindingForgetAll();
        g_resolveShader = nullptr;
        g_otherShader = nullptr;
        std::puts("resolve_bind_test PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "resolve_bind_test FAIL: %s\n", e.what());
        return 1;
    }
}
