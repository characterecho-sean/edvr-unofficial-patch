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
