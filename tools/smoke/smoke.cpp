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
// Optional second argument: --recovery-device, --foreign-device, or
// --fixes-off-device (with advanced.d3d11_fixes=0 in a fixture ini). These
// focused hardware checks require the default log directory beside smoke.exe.
// --warp runs the normal smoke checks on the software D3D11 device.
#include <windows.h>

#include <d3d11.h>

#include <cstdio>
#include <vector>

#include "../../src/common/temporal_math.h"
#include <cmath>
#include <cstring>

namespace {

typedef HRESULT(WINAPI* PFN_Create)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

int fail(const char* what) {
    printf("  FAIL  %s\n", what);
    return 1;
}

// Clear a freshly made w*h target to Elite's grey and read one pixel back.
//
// Everything is created and destroyed inside, which is the point of the second
// test below: it hands the runtime a steady supply of freed view addresses to
// hand back out.
bool clearGreyReadBack(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h,
                       unsigned char out[3]) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &tex)) || !tex) return false;
    if (FAILED(device->CreateRenderTargetView(tex, nullptr, &rtv)) || !rtv) {
        tex->Release();
        return false;
    }

    const float grey[4] = {32.0f / 255.0f, 32.0f / 255.0f, 32.0f / 255.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, grey);

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Width = 1; sd.Height = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = false;
    ID3D11Texture2D* stage = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &stage)) && stage) {
        D3D11_BOX box{0, 0, 0, 1, 1, 1};
        ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, tex, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
            const unsigned char* px = static_cast<const unsigned char*>(m.pData);
            out[0] = px[0]; out[1] = px[1]; out[2] = px[2];
            ok = true;
            ctx->Unmap(stage, 0);
        }
        stage->Release();
    }
    rtv->Release();
    tex->Release();
    return ok;
}

// A solid-colour source standing in for a submitted eye texture. RENDER_
// TARGET so it can be cleared; SHADER_RESOURCE when asked, because a real
// eye texture is one -- and NOT when asked, to reach the resolve's
// copy-through path for a source that refuses a shader view.
bool makeSolidSrc(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h,
                  const float rgba[4], bool shaderResource, ID3D11Texture2D** outTex) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET |
                   (shaderResource ? D3D11_BIND_SHADER_RESOURCE : 0);
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &tex)) || !tex) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(device->CreateRenderTargetView(tex, nullptr, &rtv)) || !rtv) {
        tex->Release();
        return false;
    }
    ctx->ClearRenderTargetView(rtv, rgba);
    rtv->Release();
    *outTex = tex;
    return true;
}

// Read back one pixel of an R8G8B8A8_UNORM texture.
bool readPixelRgba8(ID3D11Device* device, ID3D11DeviceContext* ctx,
                    ID3D11Texture2D* tex, UINT x, UINT y, unsigned char out[4]) {
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &stage)) || !stage) return false;
    D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
    ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    bool ok = false;
    if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        const unsigned char* px = static_cast<const unsigned char*>(m.pData);
        out[0] = px[0]; out[1] = px[1]; out[2] = px[2]; out[3] = px[3];
        ok = true;
        ctx->Unmap(stage, 0);
    }
    stage->Release();
    return ok;
}

// The resolve's result, checked: an ID3D11Texture2D of the expected size
// and format whose pixel at (x, y) holds the expected colour within +/-tol.
// Prints its own ok/FAIL line naming `label`.
bool checkResolved(ID3D11Device* device, ID3D11DeviceContext* ctx, void* result,
                   const char* label, UINT expectW, UINT expectH, UINT x, UINT y,
                   unsigned char er, unsigned char eg, unsigned char eb, int tol) {
    if (!result) {
        printf("  FAIL  %s: the pass returned null\n", label);
        return false;
    }
    ID3D11Texture2D* rtex = nullptr;
    static_cast<IUnknown*>(result)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rtex));
    if (!rtex) {
        printf("  FAIL  %s: the result is not an ID3D11Texture2D\n", label);
        return false;
    }
    D3D11_TEXTURE2D_DESC rd{};
    rtex->GetDesc(&rd);
    if (rd.Width != expectW || rd.Height != expectH ||
        rd.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        printf("  FAIL  %s: the result is %ux%u format %d, expected %ux%u "
               "R8G8B8A8_UNORM (the source's own format)\n",
               label, rd.Width, rd.Height, static_cast<int>(rd.Format), expectW,
               expectH);
        rtex->Release();
        return false;
    }
    unsigned char px[4] = {0, 0, 0, 0};
    const bool got = readPixelRgba8(device, ctx, rtex, x, y, px);
    rtex->Release();
    if (!got) {
        printf("  FAIL  %s: could not read the result back\n", label);
        return false;
    }
    const int dr = static_cast<int>(px[0]) - er;
    const int dg = static_cast<int>(px[1]) - eg;
    const int db = static_cast<int>(px[2]) - eb;
    if (dr < -tol || dr > tol || dg < -tol || dg > tol || db < -tol || db > tol) {
        printf("  FAIL  %s: pixel (%u,%u) is (%u,%u,%u), expected within %d of "
               "(%u,%u,%u)\n",
               label, x, y, px[0], px[1], px[2], tol, er, eg, eb);
        return false;
    }
    printf("  ok    %s: %ux%u, pixel (%u,%u) held at (%u,%u,%u)\n", label,
           expectW, expectH, x, y, px[0], px[1], px[2]);
    return true;
}

// The refusals want the opposite verdict: null is the pass.
bool expectRefused(void* result, const char* label) {
    if (result) {
        printf("  FAIL  %s: the pass did not refuse\n", label);
        return false;
    }
    printf("  ok    %s: refused as it should\n", label);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char* proxy = argc > 1 ? argv[1] : "build\\d3d11.dll";
    const bool recoveryCheck = argc > 2 && strcmp(argv[2], "--recovery-device") == 0;
    const bool foreignCheck = argc > 2 && strcmp(argv[2], "--foreign-device") == 0;
    const bool offCheck = argc > 2 && strcmp(argv[2], "--fixes-off-device") == 0;
    const bool warpCheck = offCheck || (argc > 2 && strcmp(argv[2], "--warp") == 0);
    const bool disabledCheck = recoveryCheck || offCheck;
    printf("edvr smoke\nproxy: %s\n\n", proxy);

    char full[MAX_PATH];
    if (!GetFullPathNameA(proxy, MAX_PATH, full, nullptr)) return fail("GetFullPathName");

    // Clear the crash sentinel before loading, or this gate tests nothing
    // every other run.
    //
    // The DLL arms a sentinel before its first vtable write and confirms it
    // after about six seconds of frames. smoke presents no frames and exits,
    // so it always leaves the sentinel armed -- and the NEXT run reads that
    // as "the previous run crashed", disables every fix, and reports the
    // hooks missing. Measured: a run that passed, then an identical run that
    // failed with "slot 110 is not ClearState". A release gate that
    // alternates is worse than no gate, because package.bat runs this one.
    {
        char armed[MAX_PATH];
        // Config's default log directory is beside the executable, even
        // when the proxy being tested is loaded from another directory.
        if (!GetModuleFileNameA(nullptr, armed, MAX_PATH)) return fail("executable path");
        if (char* slash = strrchr(armed, '\\')) {
            *slash = '\0';
            strncat_s(armed, "\\edvr_logs\\d3d11_hooks.armed", _TRUNCATE);
            DeleteFileA(armed);
            if (recoveryCheck) {
                char dir[MAX_PATH];
                strcpy_s(dir, armed);
                *strrchr(dir, '\\') = '\0';
                CreateDirectoryA(dir, nullptr);
                HANDLE marker = CreateFileA(armed, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
                if (marker == INVALID_HANDLE_VALUE) return fail("could not arm recovery fixture");
                CloseHandle(marker);
            }
        }
    }

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
    const D3D_DRIVER_TYPE driver = warpCheck ? D3D_DRIVER_TYPE_WARP
                                            : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = create(nullptr, driver, nullptr, 0, want, 1,
                        D3D11_SDK_VERSION, &device, &got, &ctx);
    if (FAILED(hr)) {
        printf("  note  no hardware device, falling back to WARP\n");
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION,
                    &device, &got, &ctx);
    }
    if (FAILED(hr) || !device || !ctx) return fail("could not create a device");
    printf("  ok    device created through the proxy\n");

    if (disabledCheck || foreignCheck) {
        ID3D11Device* second = nullptr;
        ID3D11DeviceContext* secondCtx = nullptr;
        hr = create(nullptr, driver, nullptr, 0, want, 1,
                    D3D11_SDK_VERSION, &second, &got, &secondCtx);
        if (FAILED(hr) || !second || !secondCtx) return fail("second hardware device unavailable");
        bool pass = true;
        if (disabledCheck) {
            unsigned char a[3] = {}, b[3] = {};
            pass = clearGreyReadBack(device, ctx, 2048, 2048, a) &&
                   clearGreyReadBack(second, secondCtx, 2048, 2048, b) &&
                   a[0] == 32 && a[1] == 32 && a[2] == 32 &&
                   b[0] == 32 && b[1] == 32 && b[2] == 32;
            printf("  %s  recovery forwards clears on BOTH devices: %u,%u,%u / %u,%u,%u\n",
                   pass ? "ok  " : "FAIL", a[0],a[1],a[2],b[0],b[1],b[2]);
            if (!pass) return fail("recovery guard re-armed on a later device");
        }
        typedef void* (*PFN_Taa)(void*,int,const float*,const float*,const float*,float,float,
                                const float*,const float*,const float*,float,float,float,int,
                                float,float,unsigned,unsigned,unsigned);
        auto taa = reinterpret_cast<PFN_Taa>(GetProcAddress(mod, "edvrTemporalAa"));
        const float colour[4] = {0.25f,0.5f,0.75f,1};
        const float tan[4] = {-1,1,-1,1}, ident[9] = {1,0,0,0,1,0,0,0,1};
        ID3D11Texture2D *a = nullptr, *b = nullptr;
        if (!taa || !makeSolidSrc(device,ctx,64,64,colour,true,&a) ||
            !makeSolidSrc(second,secondCtx,64,64,colour,true,&b)) return fail("device fixture inputs");
        auto treat = [&](ID3D11Texture2D* src) {
            return taa(src,0,nullptr,tan,tan,0,0,ident,nullptr,nullptr,0,0,0,1,0.9f,1,0,0,1);
        };
        void* firstOut = treat(a);
        pass &= disabledCheck ? firstOut == nullptr : firstOut != nullptr;
        pass &= treat(b) == nullptr;
        if (offCheck) {
            using Sharpen = void* (*)(void*, int, const float*, float);
            auto sharpen = reinterpret_cast<Sharpen>(GetProcAddress(mod, "edvrSharpen"));
            const bool aaOff = firstOut == nullptr;
            const bool sharpenOff = sharpen && sharpen(a, 0, nullptr, 0.5f) == nullptr;
            printf("  %s  graphics off blocks submit-time AA/sharpen: %d/%d\n",
                   aaOff && sharpenOff ? "ok  " : "FAIL",
                   aaOff, sharpenOff);
            pass &= aaOff && sharpenOff;
        }
        if (foreignCheck) {
            pass &= checkResolved(device,ctx,treat(a),"original device remains usable",64,64,
                                   32,32,64,128,191,2);
        }
        pass &= SUCCEEDED(device->GetDeviceRemovedReason()) && SUCCEEDED(second->GetDeviceRemovedReason());
        a->Release(); b->Release(); secondCtx->Release(); second->Release();
        ctx->Release(); device->Release();
        printf("%s: %s\n", offCheck ? "FIXES OFF DEVICE" : recoveryCheck ? "RECOVERY DEVICE" : "FOREIGN DEVICE", pass ? "PASSED" : "FAILED");
        return pass ? 0 : 1;
    }

    // The black void fix, end to end.
    int rc = 0;
    unsigned char px[3] = {0, 0, 0};
    bool blackVoidOn = false;
    if (!clearGreyReadBack(device, ctx, 2048, 2048, px)) {
        rc = fail("could not make a test render target");
    } else {
        blackVoidOn = px[0] == 0 && px[1] == 0 && px[2] == 0;
        printf("  %s  cleared an eye-sized target to grey 32,32,32 and read back "
               "%u,%u,%u\n",
               blackVoidOn ? "ok  " : "note", px[0], px[1], px[2]);
        printf("        %s\n",
               blackVoidOn ? "black void is working"
                           : "black void did nothing -- expected if black_void = 0");
    }

    // The same fix again, after the runtime has been given every chance to hand
    // back a view address it already used.
    //
    // This is the shape of the bug that shipped twice. Both view tests used to
    // cache their answer keyed by the view pointer, and D3D reissues freed
    // addresses -- so an answer recorded for a small target could later be
    // returned for an eye-sized one that happened to land on the same address,
    // and that eye kept its grey void. In the game the trigger was switching
    // between the external camera and on foot, which destroys and recreates the
    // eye textures; here it is just churn.
    //
    // Alternating sizes matters. A small target teaches the wrong answer, and
    // the eye-sized target that follows it is the one that has to come back
    // black anyway.
    // Skipping this must never read as passing it.
    //
    // The loop below used to be guarded by "&& blackVoidOn" and nothing else, so
    // a run with black_void = 0 skipped the entire regression check and still
    // printed SMOKE TEST PASSED. The one condition the test exists to catch was
    // indistinguishable from a configuration choice -- and so was a vtable
    // rejection, or a chained proxy swallowing the clear.
    bool regressionRan = false;
    if (rc == 0 && blackVoidOn) {
        const int kRounds = 16;
        int black = 0, measured = 0, greyKept = 0, greyMeasured = 0;
        for (int i = 0; i < kRounds; ++i) {
            // Not named "small": windows.h defines that as char, via rpcndr.h.
            unsigned char tiny[3] = {0, 0, 0};
            if (clearGreyReadBack(device, ctx, 256, 256, tiny)) {  // freed before the next
                ++greyMeasured;
                // A 256x256 target is not an eye texture and must come back
                // UNCHANGED. This is the other direction the bug fails in: a
                // stale "yes, eye-sized" attached to a recycled small view would
                // blacken something the fix has no business touching, and the
                // all-black assertion below cannot see that at all.
                if (tiny[0] == 32 && tiny[1] == 32 && tiny[2] == 32) ++greyKept;
            }
            unsigned char big[3] = {0, 0, 0};
            if (!clearGreyReadBack(device, ctx, 2048, 2048, big)) continue;
            ++measured;
            if (big[0] == 0 && big[1] == 0 && big[2] == 0) ++black;
        }
        if (measured == 0 || greyMeasured == 0) {
            rc = fail("could not repeat the clears");
        } else {
            regressionRan = true;
            if (black == measured) {
                printf("  ok    %d eye-sized clears across %d view create/destroy rounds, "
                       "all black\n",
                       measured, kRounds);
            } else {
                printf("  FAIL  %d of %d eye-sized clears came back black\n", black,
                       measured);
                printf("        A view test is remembering an answer past the life of "
                       "the view it came from.\n");
                rc = 1;
            }
            if (greyKept == greyMeasured) {
                printf("  ok    %d non-eye clears left alone\n", greyMeasured);
            } else {
                printf("  FAIL  %d of %d non-eye clears were altered\n",
                       greyMeasured - greyKept, greyMeasured);
                printf("        The fix is acting on targets that are not eye "
                       "textures.\n");
                rc = 1;
            }
        }
    }

    // The two hooks whose vtable slots were counted rather than measured.
    //
    // Calling them proves the slot: if 58 or 110 named some other method, this
    // is where the process falls over or the clear below stops working, at
    // build time, instead of on somebody's headset. A miscount that lands on a
    // neighbour -- FinishCommandList sits beside ClearState -- would otherwise
    // be invisible until it mattered.
    if (rc == 0) {
        ID3D11DeviceContext* deferred = nullptr;
        if (SUCCEEDED(device->CreateDeferredContext(0, &deferred)) && deferred) {
            ID3D11CommandList* list = nullptr;
            if (SUCCEEDED(deferred->FinishCommandList(FALSE, &list)) && list) {
                ctx->ExecuteCommandList(list, FALSE);
                list->Release();
            }
            deferred->Release();
        }
        ctx->ClearState();

        // Ask whether the hooks RAN, rather than whether rendering survived.
        //
        // Checking that a clear still works afterwards cannot catch a miscount:
        // every plausible off-by-one lands on a method this test never calls, so
        // rendering carries on regardless and the gate passes for exactly the
        // regression it exists to catch.
        typedef unsigned int (*PFN_Selftest)();
        PFN_Selftest flags =
            reinterpret_cast<PFN_Selftest>(GetProcAddress(mod, "edvr_selftest_hooks"));
        const unsigned int bits = flags ? flags() : 0u;
        if (!flags) {
            printf("  FAIL  edvr_selftest_hooks is not exported\n");
            rc = 1;
        } else if ((bits & 1u) == 0) {
            printf("  FAIL  calling ClearState did not reach our hook\n");
            printf("        Slot %d is not ClearState in this runtime.\n", 110);
            rc = 1;
        } else if ((bits & 2u) == 0) {
            printf("  FAIL  calling ExecuteCommandList did not reach our hook\n");
            printf("        Slot %d is not ExecuteCommandList in this runtime.\n", 58);
            rc = 1;
        } else {
            printf("  ok    ClearState and ExecuteCommandList both reached their hooks\n");
        }

        // ...and that rendering still works afterwards.
        unsigned char after[3] = {0, 0, 0};
        if (rc == 0 && (!clearGreyReadBack(device, ctx, 2048, 2048, after) ||
                        after[0] != 0 || after[1] != 0 || after[2] != 0)) {
            printf("  FAIL  the black void stopped working after ClearState/"
                   "ExecuteCommandList\n");
            rc = 1;
        }
    }

    // The temporal pass (docs/anti-aliasing.md Feature B), called the way
    // the openvr half calls it at submit.
    //
    // BEFORE the scene-counter block, and the order is load-bearing: that
    // block issues 150 pipeline-less draws, which this rig's driver survives
    // only until the next GPU sync -- the first Map afterwards came back
    // DEVICE_HUNG (0x887A0006), measured 2026-09-01 while the render-scale
    // tests were being written. These tests map results back, so they run
    // first; the counter reads a CPU-side count and does not care.
    {
        typedef void* (*PFN_Taa)(void*, int, const float*, const float*,
                                 const float*, float, float, const float*,
                                 const float*, const float*, float, float,
                                 float, int, float, float, unsigned, unsigned,
                                 unsigned);
        PFN_Taa taa = reinterpret_cast<PFN_Taa>(GetProcAddress(mod, "edvrTemporalAa"));
        if (!taa) {
            printf("  FAIL  edvrTemporalAa is not exported\n");
            rc = 1;
        } else {
            printf("  ok    edvrTemporalAa resolves\n");
            const float tan[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
            const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            const unsigned char ar = 200, ag = 120, ab = 60;
            const float colourA[4] = {ar / 255.0f, ag / 255.0f, ab / 255.0f, 1.0f};
            const unsigned char br = 30, bg = 180, bb = 220;
            const float colourB[4] = {br / 255.0f, bg / 255.0f, bb / 255.0f, 1.0f};
            ID3D11Texture2D* srcA = nullptr;
            ID3D11Texture2D* srcB = nullptr;
            if (!makeSolidSrc(device, ctx, 400, 304, colourA, true, &srcA) ||
                !makeSolidSrc(device, ctx, 400, 304, colourB, true, &srcB)) {
                printf("  FAIL  could not make the temporal test sources\n");
                rc = 1;
            } else {
                // The first frame after a reset goes out as it came in.
                void* r1 = taa(srcA, 0, nullptr, tan, tan, 0.0f, 0.0f, ident, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 1,
                               0.9f, 1.0f, 0u, 0u, 1u);
                if (!checkResolved(device, ctx, r1, "temporal: the first frame is the game's own",
                                   400, 304, 200, 152, ar, ag, ab, 2)) rc = 1;
                // The same frame again, with history and no motion: the
                // blend of a colour with itself.
                void* r2 = taa(srcA, 0, nullptr, tan, tan, 0.0f, 0.0f, ident, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 1,
                               0.9f, 1.0f, 0u, 0u, 0u);
                if (!checkResolved(device, ctx, r2, "temporal: a steady scene stays itself",
                                   400, 304, 200, 152, ar, ag, ab, 2)) rc = 1;
                // A cut: the history (A) disagrees with everything around
                // the pixel now (B, zero variance), so the clip pulls it
                // to B and the output is the new scene, not a fade.
                void* r3 = taa(srcB, 0, nullptr, tan, tan, 0.0f, 0.0f, ident, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 1,
                               0.9f, 1.0f, 0u, 0u, 0u);
                if (!checkResolved(device, ctx, r3, "temporal: a cut is not ghosted",
                                   400, 304, 200, 152, br, bg, bb, 2)) rc = 1;
                // Motion source none, and no delta at all: still the frame.
                void* r4 = taa(srcB, 0, nullptr, tan, tan, 0.0f, 0.0f, nullptr, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 0,
                               0.9f, 1.0f, 0u, 0u, 0u);
                if (!checkResolved(device, ctx, r4, "temporal: motion 'none' blends in place",
                                   400, 304, 200, 152, br, bg, bb, 2)) rc = 1;
                // The other eye is its own history: eye 1 starts afresh.
                void* r5 = taa(srcA, 1, nullptr, tan, tan, 0.0f, 0.0f, ident, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 1,
                               0.9f, 1.0f, 0u, 0u, 0u);
                if (!checkResolved(device, ctx, r5, "temporal: the other eye has its own history",
                                   400, 304, 200, 152, ar, ag, ab, 2)) rc = 1;
            }
            // A double-wide source: the right eye's region resolved must
            // not read the left's, at its leftmost column, history or not.
            {
                const unsigned char lr = 10, lg = 10, lb = 10;
                const float left[4] = {lr / 255.0f, lg / 255.0f, lb / 255.0f, 1.0f};
                ID3D11Texture2D* wide = nullptr;
                ID3D11Texture2D* half = nullptr;
                if (!makeSolidSrc(device, ctx, 800, 304, left, true, &wide) ||
                    !makeSolidSrc(device, ctx, 400, 304, colourB, true, &half)) {
                    printf("  FAIL  could not make the temporal double-wide sources\n");
                    rc = 1;
                } else {
                    D3D11_BOX box{0, 0, 0, 400, 304, 1};
                    ctx->CopySubresourceRegion(wide, 0, 400, 0, 0, half, 0, &box);
                    const float bR[4] = {0.5f, 0.0f, 1.0f, 1.0f};
                    void* r = taa(wide, 1, bR, tan, tan, 0.0f, 0.0f, ident, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 1, 0.9f, 1.0f, 0u, 0u, 1u);
                    if (!checkResolved(device, ctx, r, "temporal: right eye of a double-wide, leftmost column (no bleed)",
                                       400, 304, 0, 152, br, bg, bb, 2)) rc = 1;
                    r = taa(wide, 1, bR, tan, tan, 0.0f, 0.0f, ident, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 1, 0.9f, 1.0f, 0u, 0u, 0u);
                    if (!checkResolved(device, ctx, r, "temporal: ...and with history",
                                       400, 304, 0, 152, br, bg, bb, 2)) rc = 1;
                }
                if (half) half->Release();
                if (wide) wide->Release();
            }
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = 400; td.Height = 304; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 4;
                td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
                ID3D11Texture2D* msaa = nullptr;
                if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &msaa)) && msaa) {
                    if (!expectRefused(taa(msaa, 0, nullptr, tan, tan, 0.0f, 0.0f, ident, nullptr, nullptr, 0.0f, 0.0f, 0.0f, 1, 0.9f, 1.0f, 0u, 0u, 1u),
                                       "temporal: an MSAA source")) rc = 1;
                    msaa->Release();
                }
            }
            if (srcA) srcA->Release();
            if (srcB) srcB->Release();
        }
    }

    // The intro skip's desk half (intro_skip.h): the path predicate against
    // the shapes the game could hand it, and the refusal through the hook
    // bodies. A wrong predicate would not crash a flight, it would waste
    // one: the log would say "no ident was asked for" and mean nothing.
    {
        typedef unsigned (*PFN_IntroSkip)();
        PFN_IntroSkip introSkip =
            reinterpret_cast<PFN_IntroSkip>(GetProcAddress(mod, "edvrIntroSkipSelftest"));
        if (!introSkip) {
            printf("  FAIL  edvrIntroSkipSelftest is not exported\n");
            rc = 1;
        } else {
            const unsigned bits = introSkip();
            if (bits == 7u) {
                printf("  ok    intro skip: idents under Movies\\ are told apart from the "
                       "front end's loops and from everything else, in either separator "
                       "and case; an armed hook answers 'file not found', a disarmed "
                       "one refuses nothing, and the probe's watch counts without "
                       "refusing\n");
            } else {
                printf("  FAIL  intro skip self-test returned %u (want 7: 1 predicate, 2 "
                       "refusal, 4 watch)\n", bits);
                rc = 1;
            }
        }
    }

    // The depth probe's read path (depth_probe.h): a depth texture of the
    // game's own family, cleared through its depth view, read back through
    // a shader view and through a copy. The fourth flight read zeros from
    // the game's targets both ways it had tried; this says whether the
    // mechanism can read a depth texture at all.
    {
        typedef unsigned (*PFN_DepthSelftest)(void*);
        PFN_DepthSelftest depthTest = reinterpret_cast<PFN_DepthSelftest>(
            GetProcAddress(mod, "edvrDepthProbeSelftest"));
        if (!depthTest) {
            printf("  FAIL  edvrDepthProbeSelftest is not exported\n");
            rc = 1;
        } else {
            const unsigned bits = depthTest(device);
            if (bits == 7u) {
                printf("  ok    depth probe: a cleared R32G8X24 depth texture reads 0.5 "
                       "through a direct view and through a copy\n");
            } else {
                printf("  FAIL  depth probe self-test returned %u (want 7: 1 setup, 2 "
                       "direct view, 4 copy)\n", bits);
                rc = 1;
            }
        }
    }

    // The render sharpening (docs/anti-aliasing.md's "sharpen" at the
    // door), called the way the openvr half calls it: AMD's RCAS on the
    // eye's region, the source's own size and format, the taps clamped
    // inside the region. Same placement rule as the passes above.
    {
        typedef void* (*PFN_Sharpen)(void*, int, const float*, float);
        PFN_Sharpen sharpen =
            reinterpret_cast<PFN_Sharpen>(GetProcAddress(mod, "edvrSharpen"));
        if (!sharpen) {
            printf("  FAIL  edvrSharpen is not exported\n");
            rc = 1;
        } else {
            printf("  ok    edvrSharpen resolves\n");
            auto readResult = [&](void* r, UINT x, UINT y, unsigned char out[4]) {
                if (!r) return false;
                ID3D11Texture2D* rtex = nullptr;
                static_cast<IUnknown*>(r)->QueryInterface(
                    __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rtex));
                if (!rtex) return false;
                const bool read = readPixelRgba8(device, ctx, rtex, x, y, out);
                rtex->Release();
                return read;
            };
            // A flat field has nothing to sharpen and must stay itself;
            // strength 0 is nothing to do and is refused.
            const unsigned char fr = 200, fg = 120, fb = 60;
            const float flat[4] = {fr / 255.0f, fg / 255.0f, fb / 255.0f, 1.0f};
            ID3D11Texture2D* flatTex = nullptr;
            if (!makeSolidSrc(device, ctx, 400, 304, flat, true, &flatTex)) {
                printf("  FAIL  could not make the sharpen test source\n");
                rc = 1;
            } else {
                if (!checkResolved(device, ctx, sharpen(flatTex, 0, nullptr, 1.0f),
                                   "sharpen: a flat field stays itself", 400, 304,
                                   200, 152, fr, fg, fb, 1)) rc = 1;
                if (!expectRefused(sharpen(flatTex, 0, nullptr, 0.0f),
                                   "sharpen: strength 0")) rc = 1;
                flatTex->Release();
            }
            // Vertical stripes two pixels wide, 60 and 180: RCAS must pull
            // the dark ones darker and the bright ones brighter, and by
            // more at full strength than at a quarter.
            {
                const UINT sw = 400, sh = 304;
                std::vector<unsigned char> bytes(static_cast<size_t>(sw) * sh * 4);
                for (UINT y = 0; y < sh; ++y) {
                    for (UINT x = 0; x < sw; ++x) {
                        const unsigned char v = (x % 4) < 2 ? 60 : 180;
                        unsigned char* bp = &bytes[(static_cast<size_t>(y) * sw + x) * 4];
                        bp[0] = bp[1] = bp[2] = v;
                        bp[3] = 0xFF;
                    }
                }
                D3D11_TEXTURE2D_DESC td{};
                td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA init{};
                init.pSysMem = bytes.data();
                init.SysMemPitch = sw * 4;
                ID3D11Texture2D* striped = nullptr;
                if (FAILED(device->CreateTexture2D(&td, &init, &striped)) || !striped) {
                    printf("  FAIL  could not make the striped sharpen source\n");
                    rc = 1;
                } else {
                    // x = 201 is a dark column, x = 202 a bright one.
                    unsigned char fullD[4] = {}, fullB[4] = {}, quD[4] = {}, quB[4] = {};
                    void* rFull = sharpen(striped, 0, nullptr, 1.0f);
                    const bool gotFull = readResult(rFull, 201, 152, fullD) &&
                                         readResult(rFull, 202, 152, fullB);
                    void* rQu = sharpen(striped, 0, nullptr, 0.25f);
                    const bool gotQu = readResult(rQu, 201, 152, quD) &&
                                       readResult(rQu, 202, 152, quB);
                    if (!gotFull || !gotQu) {
                        printf("  FAIL  sharpen: the striped result could not be read back\n");
                        rc = 1;
                    } else if (fullD[0] > 52 || fullB[0] < 188) {
                        printf("  FAIL  sharpen: full strength left a dark stripe at %u and a "
                               "bright one at %u (expected under 53 and over 187)\n",
                               fullD[0], fullB[0]);
                        rc = 1;
                    } else if (!(fullD[0] < quD[0] && quD[0] < 60) ||
                               !(fullB[0] > quB[0] && quB[0] > 180)) {
                        printf("  FAIL  sharpen: a quarter strength gave %u/%u against full "
                               "strength's %u/%u and the source's 60/180 -- not in between\n",
                               quD[0], quB[0], fullD[0], fullB[0]);
                        rc = 1;
                    } else {
                        printf("  ok    sharpen: stripes 60/180 became %u/%u at full strength, "
                               "%u/%u at a quarter\n",
                               fullD[0], fullB[0], quD[0], quB[0]);
                    }
                    striped->Release();
                }
            }
            // A double-wide source, the left half black: the right eye's
            // leftmost column is flat and must stay flat -- a tap that
            // reached into the other eye would sharpen it against black.
            {
                const unsigned char rr = 30, rg = 180, rb = 220;
                const float right[4] = {rr / 255.0f, rg / 255.0f, rb / 255.0f, 1.0f};
                const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                ID3D11Texture2D* wide = nullptr;
                ID3D11Texture2D* half = nullptr;
                if (!makeSolidSrc(device, ctx, 800, 304, black, true, &wide) ||
                    !makeSolidSrc(device, ctx, 400, 304, right, true, &half)) {
                    printf("  FAIL  could not make the sharpen double-wide sources\n");
                    rc = 1;
                } else {
                    D3D11_BOX box{0, 0, 0, 400, 304, 1};
                    ctx->CopySubresourceRegion(wide, 0, 400, 0, 0, half, 0, &box);
                    const float bR[4] = {0.5f, 0.0f, 1.0f, 1.0f};
                    if (!checkResolved(device, ctx, sharpen(wide, 1, bR, 1.0f),
                                       "sharpen: right eye of a double-wide, leftmost column (no bleed)",
                                       400, 304, 0, 152, rr, rg, rb, 1)) rc = 1;
                }
                if (half) half->Release();
                if (wide) wide->Release();
            }
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = 400; td.Height = 304; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 4;
                td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
                ID3D11Texture2D* msaa = nullptr;
                if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &msaa)) && msaa) {
                    if (!expectRefused(sharpen(msaa, 0, nullptr, 1.0f),
                                       "sharpen: an MSAA source")) rc = 1;
                    msaa->Release();
                }
            }
        }
    }

    // THE SCENE COUNTER COUNTS DRAWS, NOT RENDER-TARGET REBINDS.
    //
    // The recogniser's verdict is cached against the binding generation, so a
    // counter written inside it sees one call per rebind however many draws
    // follow. That shipped, and the field found it: the rig it was written
    // for named the right target in its own log and promoted nothing, because
    // a bar written in draws was being tested against a number of rebinds.
    // The difference is invisible in a log and obvious here -- bind once,
    // draw a known number of times, ask for the number back.
    //
    // 1626x1774 is that rig's real scene target. Nothing about the test needs
    // it to be that size, but a number from a session beats an invented one.
    {
        const UINT kW = 1626, kH = 1774, kDraws = 150;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &tex)) && tex &&
            SUCCEEDED(device->CreateRenderTargetView(tex, nullptr, &rtv)) && rtv) {
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            // Empty draws: the hook runs before the call is forwarded, so the
            // runtime rejecting them for want of a pipeline costs nothing.
            for (UINT i = 0; i < kDraws; ++i) ctx->Draw(0, 0);
            ID3D11RenderTargetView* none = nullptr;
            ctx->OMSetRenderTargets(1, &none, nullptr);

            typedef unsigned int (*PFN_SceneDraws)(unsigned int, unsigned int);
            PFN_SceneDraws sceneDraws = reinterpret_cast<PFN_SceneDraws>(
                GetProcAddress(mod, "edvr_selftest_scene_draws"));
            if (!sceneDraws) {
                printf("  FAIL  edvr_selftest_scene_draws is not exported\n");
                rc = 1;
            } else {
                const unsigned int got = sceneDraws(kW, kH);
                if (got == kDraws) {
                    printf("  ok    the scene counter counted %u draws into one "
                           "target, not the rebinds\n", got);
                } else if (got <= 2) {
                    printf("  FAIL  the scene counter read %u after %u draws into one "
                           "target\n", got, kDraws);
                    printf("        That is the rebind count. A promotion bar written "
                           "in draws can never be reached.\n");
                    rc = 1;
                } else {
                    printf("  FAIL  the scene counter read %u after %u draws\n", got,
                           kDraws);
                    rc = 1;
                }
            }
        } else {
            printf("  FAIL  could not make a render target for the scene counter\n");
            rc = 1;
        }
        if (rtv) rtv->Release();
        if (tex) tex->Release();
    }

    ctx->Release();
    device->Release();

    if (rc != 0) {
        printf("\nSMOKE TEST FAILED\n");
        return rc;
    }
    if (!regressionRan) {
        // A failure, not a caveat.
        //
        // Renaming the outcome was not enough: build.bat offers this command as
        // the way to check the build, and it exited 0 against
        // C:\Windows\System32\d3d11.dll -- a DLL containing none of these fixes.
        // A check that could not run has verified nothing, and saying so in
        // words while returning success puts the burden on somebody reading
        // scrollback. black_void = 0 is a legitimate setting and this is a
        // legitimate way to find out that it is on.
        printf("\nSMOKE TEST FAILED: THE VIEW-RECYCLING CHECK COULD NOT RUN\n");
        printf("        The first eye-sized clear did not come back black, so the\n");
        printf("        black void fix is not acting. Either this DLL is not an EDVR\n");
        printf("        build, or black_void = 0 in an edvr.ini beside it.\n");
        return 1;
    }
    printf("\nSMOKE TEST PASSED\n");
    return 0;
}
