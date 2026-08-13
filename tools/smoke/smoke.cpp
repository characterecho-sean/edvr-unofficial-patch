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
