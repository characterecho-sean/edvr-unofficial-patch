#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "../../src/common/config.h"
#include "../../src/common/vtable_hook.h"
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

        // The running lend count (the 2026-10-09 scanner-body instrument): the one lend above counts in the 60 s window the first tick starts; it is said once, 60 s on,
        // as "scanner body fix: lent the other eye's buffer N times in the last 60 s" (the line's text is vertex_resync_test's V4); a window with no lend says nothing.
        check(edvr::resolveBindTick(1000) == 0, "the first tick starts the lend window and says nothing");
        check(edvr::resolveBindTick(60999) == 0, "a tick inside the 60 s says nothing");
        check(edvr::resolveBindTick(61000) == 1, "the tick 60 s after the first says the one lend");
        check(edvr::resolveBindTick(121000) == 0, "the next window, with no lend in it, says nothing");
        edvr::resolveBindBegin(context.Get());   // the slot is empty again: one more lend, then restore
        edvr::resolveBindEnd(context.Get());
        check(edvr::resolveBindTick(181000) == 1, "a later window with a lend in it says it");

        // THE BINDING SHADOW'S RESOLVERS ARE ON FOUR BUDGETS (binding_shadow.h): the
        // fixes' pair and the instruments' pair. A stale pointer faults inside a
        // resolver, the fault is absorbed and charged to that resolver's budget of
        // five, and a spent budget refuses even a live view. One direction per pair,
        // because a budget cannot be refilled and the four are independent by
        // construction: spending the instruments' view budget must leave the fixes'
        // view resolver working, and spending the fixes' resource budget must leave
        // the instruments' resource resolver working.
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
            edvr::ResourceInfo info;
            void* stale = reinterpret_cast<void*>(0x10);   // no object there: the call faults

            check(edvr::bindingResolve(view.Get(), &info) && info.isTexture2D &&
                      info.a == 16 && info.b == 8,
                  "a live view resolves for a fix");
            check(edvr::bindingResolveProbe(view.Get(), &info) && info.isTexture2D &&
                      info.resource == static_cast<ID3D11Resource*>(texture.Get()),
                  "...and for a probe, to the same resource");
            for (int i = 0; i < 6; ++i) {
                check(!edvr::bindingResolveProbe(stale, &info),
                      "a stale view is refused by the probe's resolver, its fault absorbed");
            }
            check(!edvr::bindingResolveProbe(view.Get(), &info),
                  "after its five faults the probe's view budget is spent, and even a live "
                  "view no longer resolves for it");
            check(edvr::bindingResolve(view.Get(), &info) && info.isTexture2D,
                  "the fixes' view resolver shares nothing with it and still resolves");

            check(edvr::bindingResolveResource(texture.Get(), &info) && info.isTexture2D,
                  "a live resource resolves for a fix");
            for (int i = 0; i < 6; ++i) {
                check(!edvr::bindingResolveResource(stale, &info),
                      "a stale resource is refused by the fixes' resolver, its fault absorbed");
            }
            check(!edvr::bindingResolveResource(texture.Get(), &info),
                  "after its five faults the fixes' resource budget is spent");
            check(edvr::bindingResolveResourceProbe(texture.Get(), &info) && info.isTexture2D,
                  "the probe's resource resolver shares nothing with it and still resolves");
        }

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
