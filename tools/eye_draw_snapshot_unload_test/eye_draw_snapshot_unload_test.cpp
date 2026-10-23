// Fresh-process regression for the eye-dump crash of 2026-10-23 (native on-foot VR, Elite's minidump 14:08:47).
//
// EyeDrawSnapshot::pointSampleUiTexture compiles its 384x384 point-sample kernel through d3dcompiler_47.dll, loaded by LoadLibraryW. It used
// to call FreeLibrary right after D3DCompile while the ID3DBlob it had just been handed was still alive. The blob's vtable lives in that
// DLL, so code->GetBufferSize() (and the ComPtr destructors) ran code in an unmapped image: 0xC0000005 on the render thread, the crash
// sentinel reading "covered-by-unloaded=d3dcompiler_47.dll". In the game the DLL was only ever loaded by this call (shader_swap.cpp and
// gpu_pass_profile.cpp leak their handles, but only when their features are on), so the FreeLibrary really did unmap it.
//
// eye_draw_snapshot_test cannot stand in for this: it links d3dcompiler.lib and calls D3DCompile itself first, so the DLL is pinned by an import
// for the whole process and the LoadLibrary/FreeLibrary pair never takes the reference count to zero. This rig is its own process, links
// d3d11.lib only, never calls D3DCompile, and refuses to run unless d3dcompiler_47.dll is genuinely absent when the sampler is first called.
//
// On the unfixed header this process dies with an access violation in the first sampler call (the filter below reports it as FAIL and exits 1).
// With the fix the DLL stays resident for the process once the sampler has loaded it, and every blob is released while it is mapped; the rig
// prints which of the two it saw but does not require residency, only that the sampler returns true and the pixels are right on every call.
#include "../../src/d3d11/eye_draw_snapshot.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace edvr { thread_local bool g_vrWorldInternal=false; thread_local bool g_flatComputeInternal=false; }

namespace {
void check(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL: %s\n", why); std::fflush(stdout); std::exit(1); }
}
void hr(HRESULT v, const char* what) {
    if (FAILED(v)) { std::printf("FAIL: %s (0x%08X)\n", what, static_cast<unsigned>(v)); std::fflush(stdout); std::exit(1); }
}
bool compilerLoaded() { return GetModuleHandleW(L"d3dcompiler_47.dll") != nullptr; }

// The crash must fail the build, not hang it on a Windows Error Reporting dialog or exit silently.
LONG WINAPI onCrash(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : nullptr;
    const DWORD code = rec ? rec->ExceptionCode : 0;
    // For an access violation the faulting instruction is usually in this exe (the virtual call), and the address it read is the unmapped
    // vtable slot of the released module.
    const bool access = rec && code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2;
    const void* target = access ? reinterpret_cast<const void*>(rec->ExceptionInformation[1]) : nullptr;
    HMODULE owner = nullptr;
    if (target) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(target), &owner);
    std::printf("FAIL: exception 0x%08lX, instruction %p, accessed address %p (%s); d3dcompiler_47.dll is %s. This is the eye-dump crash "
                "when the accessed address is unmapped and the module is gone: a blob from d3dcompiler_47.dll used after the module was "
                "unloaded.\n", code, rec ? rec->ExceptionAddress : nullptr, target,
                !target ? "none" : owner ? "inside a loaded module" : "not covered by any loaded module",
                compilerLoaded() ? "still loaded" : "not loaded");
    std::fflush(stdout);
    ExitProcess(1);
}

void waitGpu(ID3D11DeviceContext* ctx, ID3D11Device* dev) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query; hr(dev->CreateQuery(&qd, &query), "create event query");
    ctx->End(query.Get()); ctx->Flush();
    const ULONGLONG deadline = GetTickCount64() + 10000;
    HRESULT ready = S_FALSE;
    while (ready == S_FALSE && GetTickCount64() < deadline) ready = ctx->GetData(query.Get(), nullptr, 0, 0);
    hr(ready, "event query"); check(ready == S_OK, "GPU timeout");
}

constexpr UINT kWidth = 1024, kHeight = 768;

// One call of the production point sampler on a 1024x768 RGBA8 grid whose texel (x,y) is {x, y, 0xA5, 0xFF}, then a spot check of the
// 384x384 output against the shader's centre-point mapping.
void sampleAndVerify(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* which) {
    std::vector<uint8_t> grid(size_t(kWidth) * kHeight * 4);
    for (UINT y = 0; y < kHeight; ++y) for (UINT x = 0; x < kWidth; ++x) {
        const size_t at = (size_t(y) * kWidth + x) * 4;
        grid[at] = uint8_t(x); grid[at + 1] = uint8_t(y); grid[at + 2] = 0xA5; grid[at + 3] = 0xFF;
    }
    D3D11_TEXTURE2D_DESC td{}; td.Width = kWidth; td.Height = kHeight; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{grid.data(), kWidth * 4, 0};
    ComPtr<ID3D11Texture2D> source; hr(dev->CreateTexture2D(&td, &initial, &source), "create grid texture");
    ComPtr<ID3D11ShaderResourceView> sourceView; hr(dev->CreateShaderResourceView(source.Get(), nullptr, &sourceView), "create grid view");

    ComPtr<ID3D11Texture2D> sampled;
    const bool ran = edvr::EyeDrawSnapshot::pointSampleUiTextureForTest(ctx, dev, sourceView.Get(), kWidth, kHeight, &sampled);
    std::printf("%s: sampler returned %s\n", which, ran ? "true" : "false");
    check(ran && sampled, "the point sampler returns true and a staging texture");
    waitGpu(ctx, dev);

    D3D11_MAPPED_SUBRESOURCE mapped{}; hr(ctx->Map(sampled.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map sampler output");
    bool matches = true;
    const std::pair<UINT, UINT> probes[] = {{0, 0}, {1, 0}, {127, 91}, {200, 50}, {383, 0}, {0, 383}, {383, 383}};
    for (const auto& xy : probes) {
        const UINT sx = ((xy.first * 2 + 1) * kWidth) / 768, sy = ((xy.second * 2 + 1) * kHeight) / 768;
        const uint8_t* got = static_cast<const uint8_t*>(mapped.pData) + size_t(xy.second) * mapped.RowPitch + size_t(xy.first) * 4;
        const bool here = got[0] == uint8_t(sx) && got[1] == uint8_t(sy) && got[2] == 0xA5 && got[3] == 0xFF;
        if (!here) std::printf("  probe (%u,%u): got {%u,%u,%u,%u}, want {%u,%u,165,255}\n", xy.first, xy.second, got[0], got[1], got[2], got[3],
                               unsigned(uint8_t(sx)), unsigned(uint8_t(sy)));
        matches = matches && here;
    }
    ctx->Unmap(sampled.Get(), 0);
    check(matches, "the output holds the centre-point samples of the source grid");
}

bool makeWarp(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
}
} // namespace

int wmain() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(onCrash);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    check(makeWarp(dev, ctx), "create a WARP device");
    // The precondition that makes this a regression test: nothing in this process holds d3dcompiler_47.dll (no import, no D3DCompile of
    // our own, and WARP does not load it), so the sampler's own LoadLibraryW is the first and only reference.
    check(!compilerLoaded(), "d3dcompiler_47.dll is already loaded before the sampler runs, so this rig would not exercise the unload path");
    std::puts("before the first call: d3dcompiler_47.dll is not loaded");

    sampleAndVerify(dev.Get(), ctx.Get(), "call 1 (compile path)");
    std::printf("after call 1: d3dcompiler_47.dll is %s\n", compilerLoaded() ? "loaded (resident for the process, as intended)" : "unloaded");

    sampleAndVerify(dev.Get(), ctx.Get(), "call 2 (same device, kernel reuse path)");

    // A different device: the cached kernel belongs to the first one, so the sampler compiles again.
    ComPtr<ID3D11Device> dev2; ComPtr<ID3D11DeviceContext> ctx2;
    check(makeWarp(dev2, ctx2), "create a second WARP device");
    sampleAndVerify(dev2.Get(), ctx2.Get(), "call 3 (second device, recompile path)");
    std::printf("after call 3: d3dcompiler_47.dll is %s\n", compilerLoaded() ? "loaded (resident for the process, as intended)" : "unloaded");

    std::puts("eye_draw_snapshot_unload_test: OK");
    return 0;
}
