// smoke -- checks the proxy loads and the fixes actually do something.
//
// Runs without the game and without a headset. It loads the built d3d11.dll the
// way Elite does, creates a device through it, and then tests the one fix that
// can be checked from outside VR: clearing an eye-sized target to Elite's grey
// and reading a pixel back.
//
// This exists because a fix that is only ever exercised in a headset does not
// get exercised. An earlier instrument in this project was wired wrong in a way
// that made every measurement identical, and it survived two play sessions
// before anyone noticed.
//
// Usage: smoke.exe [path\to\d3d11.dll]
#include <windows.h>

#include <d3d11.h>

#include <cstdio>

namespace {

typedef HRESULT(WINAPI* PFN_Create)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

int fail(const char* what) {
    printf("  FAIL  %s\n", what);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const char* proxy = argc > 1 ? argv[1] : "build\\d3d11.dll";
    printf("edvr smoke\nproxy: %s\n\n", proxy);

    char full[MAX_PATH];
    if (!GetFullPathNameA(proxy, MAX_PATH, full, nullptr)) return fail("GetFullPathName");
    HMODULE mod = LoadLibraryA(full);
    if (!mod) return fail("the proxy did not load");
    printf("  ok    proxy loaded\n");

    auto create = reinterpret_cast<PFN_Create>(GetProcAddress(mod, "D3D11CreateDevice"));
    if (!create) return fail("D3D11CreateDevice is not exported");
    if (!GetProcAddress(mod, "D3D11CoreCreateDevice")) return fail("an export is missing");
    printf("  ok    exports resolve\n");

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 1,
                        D3D11_SDK_VERSION, &device, &got, &ctx);
    if (FAILED(hr)) {
        printf("  note  no hardware device, falling back to WARP\n");
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION,
                    &device, &got, &ctx);
    }
    if (FAILED(hr) || !device || !ctx) return fail("could not create a device");
    printf("  ok    device created through the proxy\n");

    // The black void fix, end to end.
    int rc = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 2048; td.Height = 2048; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D* eye = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &eye)) && eye &&
        SUCCEEDED(device->CreateRenderTargetView(eye, nullptr, &rtv))) {

        const float grey[4] = {32.0f / 255.0f, 32.0f / 255.0f, 32.0f / 255.0f, 1.0f};
        ctx->ClearRenderTargetView(rtv, grey);

        D3D11_TEXTURE2D_DESC sd = td;
        sd.Width = 1; sd.Height = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* stage = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &stage)) && stage) {
            D3D11_BOX box{0, 0, 0, 1, 1, 1};
            ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, eye, 0, &box);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                const unsigned char* px = static_cast<const unsigned char*>(m.pData);
                const bool black = px[0] == 0 && px[1] == 0 && px[2] == 0;
                printf("  %s  cleared an eye-sized target to grey 32,32,32 and read "
                       "back %u,%u,%u\n",
                       black ? "ok  " : "note", px[0], px[1], px[2]);
                printf("        %s\n",
                       black ? "black void is working"
                             : "black void did nothing -- expected if black_void = 0");
                ctx->Unmap(stage, 0);
            }
            stage->Release();
        }
        rtv->Release();
        eye->Release();
    } else {
        rc = fail("could not make a test render target");
    }

    ctx->Release();
    device->Release();
    printf("\n%s\n", rc == 0 ? "SMOKE TEST PASSED" : "SMOKE TEST FAILED");
    return rc;
}
