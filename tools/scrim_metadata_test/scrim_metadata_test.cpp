#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../src/common/config.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/scrim_fix.h"

using Microsoft::WRL::ComPtr;

namespace {

using SetShaderResources = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
SetShaderResources g_realSetShaderResources = nullptr;
using GetResource = void(STDMETHODCALLTYPE*)(ID3D11View*, ID3D11Resource**);
GetResource g_realGetResource = nullptr;
bool g_failNextGetResource = false;

void STDMETHODCALLTYPE shadowedSetShaderResources(
    ID3D11DeviceContext* self, UINT start, UINT count,
    ID3D11ShaderResourceView* const* views) {
    if (views) {
        for (UINT i = 0; i < count; ++i) {
            const UINT slot = start + i;
            if (slot >= 4) break;
            edvr::bindingSet(static_cast<edvr::BindSlot>(
                                 static_cast<uint32_t>(edvr::BindSlot::PsSrv0) +
                                 slot),
                             views[i]);
        }
    }
    g_realSetShaderResources(self, start, count, views);
}

void STDMETHODCALLTYPE failOnceGetResource(ID3D11View* self,
                                           ID3D11Resource** resource) {
    if (g_failNextGetResource) {
        g_failNextGetResource = false;
        if (resource) *resource = nullptr;
        return;
    }
    g_realGetResource(self, resource);
}

void check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

void hr(HRESULT result, const char* what) {
    if (FAILED(result)) throw std::runtime_error(what);
}

ComPtr<ID3D11ShaderResourceView> makeTextureSrv(ID3D11Device* device,
                                                UINT width, UINT height,
                                                DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    hr(device->CreateTexture2D(&desc, nullptr, &texture), "create texture");
    ComPtr<ID3D11ShaderResourceView> view;
    hr(device->CreateShaderResourceView(texture.Get(), nullptr, &view),
       "create texture SRV");
    return view;
}

void bindSrv(ID3D11DeviceContext* context, UINT slot,
             ID3D11ShaderResourceView* view) {
    context->PSSetShaderResources(slot, 1, &view);
}

ID3D11ShaderResourceView* boundSrv(ID3D11DeviceContext* context, UINT slot) {
    ID3D11ShaderResourceView* view = nullptr;
    context->PSGetShaderResources(slot, 1, &view);
    return view;
}

uint64_t calls() { return edvr::scrimMetadataResolveCallsForTest(); }

}  // namespace

int main() {
    try {
        const auto createDevice = edvr::systemD3D11CreateDevice();
        check(createDevice != nullptr, "load System32 D3D11CreateDevice");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        hr(createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                        D3D11_SDK_VERSION, &device, &level, &context),
           "create WARP device");

        edvr::VTableHook hook;
        check(hook.attach(context.Get(), 128), "attach context vtable");
        check(hook.setMode(edvr::HookMode::CopyVptr), "select private vtable copy");
        void* original = nullptr;
        check(hook.replace(8, reinterpret_cast<void*>(&shadowedSetShaderResources),
                           &original),
              "replace PSSetShaderResources");
        g_realSetShaderResources = reinterpret_cast<SetShaderResources>(original);
        check(hook.commit(), "commit PSSetShaderResources shadow hook");

        const auto wash = makeTextureSrv(device.Get(), 16, 16,
                                         DXGI_FORMAT_BC1_UNORM);
        const auto retryWash = makeTextureSrv(device.Get(), 16, 16,
                                              DXGI_FORMAT_BC1_UNORM);
        const auto ui = makeTextureSrv(device.Get(), 2048, 1024,
                                       DXGI_FORMAT_R8G8B8A8_UNORM);
        const auto other = makeTextureSrv(device.Get(), 32, 32,
                                          DXGI_FORMAT_R8G8B8A8_UNORM);

        edvr::Config& config = edvr::Config::get();
        config.set("fix.loading_dim", "screen");
        config.set("advanced.loading_dim_level", "0");
        edvr::scrimConfigure(config);
        check(edvr::scrimWantsDraws(), "scrim enabled");

        // A failed non-null resolve must stay unknown at the same generation.
        // The view is real; its first GetResource is withheld, then the exact
        // same binding succeeds without any setter repairing the generation.
        edvr::VTableHook retryHook;
        check(retryHook.attach(retryWash.Get(), 16), "attach retry SRV vtable");
        check(retryHook.setMode(edvr::HookMode::CopyVptr),
              "select retry SRV private vtable copy");
        original = nullptr;
        check(retryHook.replace(7, reinterpret_cast<void*>(&failOnceGetResource),
                                &original),
              "replace retry SRV GetResource");
        g_realGetResource = reinterpret_cast<GetResource>(original);
        check(retryHook.commit(), "commit retry SRV GetResource shim");
        bindSrv(context.Get(), 0, retryWash.Get());
        bindSrv(context.Get(), 1, ui.Get());
        const uint32_t retryGeneration =
            edvr::bindingGeneration(edvr::BindSlot::PsSrv0);
        edvr::scrimMetadataResetResolveCallsForTest();
        g_failNextGetResource = true;
        check(!edvr::scrimOnEyeDraw('X', 5760, 1),
              "failed non-null resolve declines");
        check(calls() == 1, "failed non-null resolve attempted once");
        check(edvr::bindingGeneration(edvr::BindSlot::PsSrv0) == retryGeneration,
              "failed resolve did not change generation");
        check(edvr::scrimOnEyeDraw('X', 5760, 1),
              "same-generation resolve retries and recovers");
        check(calls() == 3, "retry resolves slot0 then slot1");
        check(edvr::scrimOnEyeDraw('X', 5760, 1),
              "recovered metadata is cached");
        check(calls() == 3, "recovered metadata avoids later queries");

        edvr::scrimMetadataResetResolveCallsForTest();
        bindSrv(context.Get(), 0, other.Get());
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "known slot0 miss");
        check(calls() == 1, "slot0 miss resolves once");
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "cached slot0 miss");
        check(calls() == 1, "cached slot0 miss avoids query");

        // Even the identical pointer is a new binding identity. This is also
        // the pointer-recycling guard: the generation, not the address alone,
        // decides whether immutable metadata can be reused.
        bindSrv(context.Get(), 0, other.Get());
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "same pointer rebound");
        check(calls() == 2, "same pointer rebind invalidates slot0");

        bindSrv(context.Get(), 0, wash.Get());
        bindSrv(context.Get(), 1, ui.Get());
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "real scrim recognised");
        check(calls() == 4, "both real SRVs resolved");
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "real scrim cached");
        check(calls() == 4, "recognition cache avoids both queries");
        // The draw path asks scrimWashShape inline before the call; with the
        // real wash bound, the two must agree shape by shape, and a shape the
        // pre-check turns away must be one the match declines untouched.
        check(edvr::scrimWashShape('X', 5760, 1) && edvr::scrimWashShape('X', 100, 1),
              "inline shape pre-check accepts the wash mesh");
        check(!edvr::scrimWashShape('X', 6, 1) && !edvr::scrimOnEyeDraw('X', 6, 1) &&
                  !edvr::scrimWashShape('X', 5760, 2) && !edvr::scrimOnEyeDraw('X', 5760, 2) &&
                  !edvr::scrimWashShape('N', 5760, 1) && !edvr::scrimOnEyeDraw('N', 5760, 1),
              "inline shape pre-check rejects exactly what the match rejects on shape");
        check(calls() == 4, "shape rejections make no query");

        bindSrv(context.Get(), 1, other.Get());
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "slot1-only miss");
        check(calls() == 5, "slot1-only change preserves slot0 cache");
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "slot1 miss cached");
        check(calls() == 5, "cached slot1 miss avoids query");
        bindSrv(context.Get(), 1, ui.Get());
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "slot1 restored");
        check(calls() == 6, "slot1 restore resolves only slot1");

        bindSrv(context.Get(), 0, other.Get());
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "slot0-only miss");
        check(calls() == 7, "slot0-only change resolves only slot0");
        bindSrv(context.Get(), 0, wash.Get());
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "slot0 restored");
        check(calls() == 8, "slot0 restore resolves only slot0");

        edvr::bindingFrameBoundary();
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "next-frame recognition");
        check(calls() == 10, "frame boundary bounds both cache entries");

        // ClearState and ExecuteCommandList(false) both use this production
        // invalidation boundary. Null remains unknown/do-nothing; a later bind
        // increments the generation and recovers through real resolution.
        context->ClearState();
        edvr::bindingForgetAll();
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "forgotten null declines");
        check(calls() == 10, "null state issues no resource query");
        bindSrv(context.Get(), 0, wash.Get());
        bindSrv(context.Get(), 1, ui.Get());
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "unknown state recovers");
        check(calls() == 12, "recovery resolves both rebound SRVs");

        ComPtr<ID3D11DeviceContext> deferred;
        hr(device->CreateDeferredContext(0, &deferred), "create deferred context");
        ID3D11ShaderResourceView* otherRaw = other.Get();
        deferred->PSSetShaderResources(0, 1, &otherRaw);
        ComPtr<ID3D11CommandList> commands;
        hr(deferred->FinishCommandList(FALSE, &commands), "finish command list");
        context->ExecuteCommandList(commands.Get(), TRUE);
        check(edvr::scrimOnEyeDraw('X', 5760, 1),
              "restore-state command list preserves classification");
        check(calls() == 12, "restore-state command list preserves cache");
        context->ExecuteCommandList(commands.Get(), FALSE);
        edvr::bindingForgetAll();
        check(!edvr::scrimOnEyeDraw('X', 5760, 1),
              "non-restoring command list forgets classification");
        check(calls() == 12, "forgotten command-list state remains unknown");

        bindSrv(context.Get(), 0, wash.Get());
        bindSrv(context.Get(), 1, ui.Get());
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "restore test classified");
        check(calls() == 14, "restore test resolves bindings");
        const uint32_t beforeOverride =
            edvr::bindingGeneration(edvr::BindSlot::PsSrv0);
        edvr::scrimBegin(context.Get());
        ID3D11ShaderResourceView* during = boundSrv(context.Get(), 0);
        check(during && during != wash.Get(), "begin substitutes uniform SRV");
        during->Release();
        edvr::scrimEnd(context.Get());
        ID3D11ShaderResourceView* after = boundSrv(context.Get(), 0);
        check(after == wash.Get(), "end restores original SRV");
        if (after) after->Release();
        check(edvr::bindingGeneration(edvr::BindSlot::PsSrv0) ==
                  beforeOverride + 2,
              "begin/end hook invalidates slot0 twice");
        check(edvr::scrimOnEyeDraw('X', 5760, 1),
              "post-override classification recovers");
        check(calls() == 15, "post-override resolves only invalidated slot0");

        config.set("fix.loading_dim", "stock");
        edvr::scrimConfigure(config);
        check(!edvr::scrimOnEyeDraw('X', 5760, 1), "disabled scrim declines");
        check(calls() == 15, "disabled scrim makes no query");
        config.set("fix.loading_dim", "screen");
        edvr::scrimConfigure(config);
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "re-enabled scrim recovers");
        check(calls() == 17, "re-enable clears metadata cache");

        edvr::scrimShutdown();
        check(edvr::scrimOnEyeDraw('X', 5760, 1), "post-shutdown metadata recovers");
        check(calls() == 19, "shutdown clears metadata cache");
        edvr::scrimShutdown();
        edvr::bindingForgetAll();
        std::puts("scrim_metadata_test PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scrim_metadata_test FAIL: %s\n", e.what());
        return 1;
    }
}
