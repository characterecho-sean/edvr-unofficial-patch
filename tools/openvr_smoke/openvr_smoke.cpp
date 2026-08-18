// openvr_smoke -- checks the openvr proxy's startup path without the game.
//
// The thing under test is that the proxy does NOT load the real openvr_api.dll
// from DllMain. It used to. LoadLibrary of a module nothing else has mapped runs
// that module's DllMain under the loader lock, re-entrantly, which Windows does
// not support -- the same pattern crashed the game for a user running ReShade
// with EDHM on the d3d11 side, and that side was rebuilt to defer.
//
// Three things are asserted, in order:
//
//   1. Loading the proxy returns promptly. On a worker thread with a timeout, so
//      a hang is reported rather than hanging the test.
//
//   2. fakevr is not mapped yet. THIS is the assertion that catches the bug:
//      if the real module is already loaded when LoadLibrary returns, the proxy
//      loaded it from DllMain, which is the thing being fixed. It fails against
//      the old build and passes against this one.
//
//   3. A thunked export forwards, and its arguments arrive intact. The lazy path
//      runs a C initialiser in the middle of a call whose arguments are still
//      live in rcx/rdx/r8/r9 and xmm0-xmm3; the probe checks all of those plus
//      two stack arguments.
//
// Usage: openvr_smoke.exe <dir containing openvr_api.dll and openvr_api_orig.dll>
#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/common/frame_flag.h"
#include "../../src/common/hotkey.h"
#include "../../src/common/guard.h"
#include "../../src/openvr/guard_crop.h"
#include "../../src/openvr/resubmit_shadow.h"
#include "../../src/d3d11/elite_binds.h"
#include "../fakevr/fake_system_012.h"

namespace {

wchar_t g_proxyPath[MAX_PATH];
HMODULE g_loaded = nullptr;

int fail(const char* what) {
    printf("  FAIL  %s\n", what);
    return 1;
}

DWORD WINAPI loadProxy(LPVOID) {
    g_loaded = LoadLibraryW(g_proxyPath);
    return 0;
}

typedef unsigned long long(*PFN_Probe)(unsigned long long, double, unsigned long long,
                                       double, unsigned long long, unsigned long long);

}  // namespace

// A real module whose DllMain faults must degrade, not kill the process.
//
// This started life as a test that the fault stays CATCHABLE, to lock in the
// unwind info the assembly shim was missing. It does not test that: Windows
// catches a faulting DllMain inside LoadLibraryW and returns failure rather than
// propagating an exception, so nothing ever reaches the handler -- measured, the
// child exits with "no exception seen" either way. The unwind info is still
// required, for faults raised in our own initialiser rather than in someone
// else's DllMain, and build.bat asserts it is present instead.
//
// What this DOES pin down is worth keeping: the export still returns, the
// process survives, and resolveProcs has filled the table with the do-nothing
// stub, so the game loses VR instead of dying at startup.
//
// A child process because the fault is arranged by an environment variable read
// in the stand-in's DllMain, and the table resolves once per process.
int faultChild(const char* dir) {
    wchar_t proxy[MAX_PATH];
    _snwprintf_s(proxy, _TRUNCATE, L"%hs\\openvr_api.dll", dir);
    HMODULE m = LoadLibraryW(proxy);
    if (!m) return 3;
    FARPROC p = GetProcAddress(m, "VR_GetInitToken");
    if (!p) return 4;

    unsigned long long v = 12345;
    __try {
        v = reinterpret_cast<unsigned long long(*)()>(p)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;   // also a pass: it returned control to us either way
    }
    // The stub returns 0. Anything else means we forwarded into a module whose
    // DllMain faulted. Not returning at all -- the process being killed -- is
    // the failure, and shows up as an exit code that is neither 0 nor 6.
    return v == 0 ? 0 : 6;
}

// The IVRSystem_012 observation hook, end to end, through the built proxy.
//
// The one real hazard in that hook is the calling-convention split on
// struct-returning methods (EVIDENCE 6bo): the member and free conventions
// disagree about where the hidden return pointer rides, a C receiver of
// either shape corrupts a caller of the other, and the corruption is quiet --
// values still round-trip while the object under them dies. So the two
// struct-returning slots are observed by register-preserving asm thunks, and
// THIS test is what holds that line: fakevr's stand-in is implemented with
// ordinary C++ virtuals, every call below is a genuine member-convention call
// site, and every argument and returned byte is asserted to cross the hook
// unchanged. A convention regression in the thunks fails here, in a build,
// instead of as a vanished headset mid-flight.
//
// Counts are asserted through the edvr_selftest_system_hook export, which
// also proves the asm thunks actually fired rather than the calls having
// slipped past an uninstalled hook.
int systemHookChecks(const char* dir) {
    int bad = 0;

    // A crashed previous run leaves the sentinel armed, and the hook would
    // rightly refuse this session -- in the game. A test must exercise the
    // install every run, so start clean.
    wchar_t armed[MAX_PATH];
    _snwprintf_s(armed, _TRUNCATE, L"%hs\\edvr_logs\\system_hook.armed", dir);
    DeleteFileW(armed);

    typedef void*(__cdecl* PFN_GetGenericInterface)(const char*, int*);
    typedef unsigned int(*PFN_Selftest)(void);
    typedef void*(*PFN_FakePtr)(void);

    auto getIface = reinterpret_cast<PFN_GetGenericInterface>(
        GetProcAddress(g_loaded, "VR_GetGenericInterface"));
    if (!getIface) {
        printf("  FAIL  the proxy does not export VR_GetGenericInterface\n");
        return 1;
    }

    int err = -1;
    void* iface = getIface("IVRSystem_012", &err);
    if (!iface) {
        printf("  FAIL  IVRSystem_012 came back null through the proxy\n");
        return 1;
    }

    HMODULE fake = GetModuleHandleW(L"openvr_api_orig.dll");
    auto fakePtr = fake ? reinterpret_cast<PFN_FakePtr>(
                              GetProcAddress(fake, "VR_FakeSystemPtr"))
                        : nullptr;
    if (!fakePtr) {
        printf("  FAIL  the stand-in does not export VR_FakeSystemPtr\n");
        return 1;
    }
    if (fakePtr() != iface) {
        printf("  FAIL  the proxy returned %p for an interface at %p -- "
               "in-place hooking must hand back the same object\n",
               iface, fakePtr());
        ++bad;
    }

    auto* sys = static_cast<fakevr::ISystem012*>(iface);

    // Slot 0, the C thunk with out-pointers.
    uint32_t w = 0, h = 0;
    sys->GetRecommendedRenderTargetSize(&w, &h);
    if (w != fakevr::kSizeW || h != fakevr::kSizeH) {
        printf("  FAIL  GetRecommendedRenderTargetSize returned %ux%u through "
               "the hook, expected %ux%u\n", w, h, fakevr::kSizeW, fakevr::kSizeH);
        ++bad;
    }

    // Slot 2, the C thunk whose values the investigation lives on.
    for (int32_t eye = 0; eye < 2; ++eye) {
        float l = 0, r = 0, t = 0, b = 0, e[4];
        sys->GetProjectionRaw(eye, &l, &r, &t, &b);
        fakevr::expectedRaw(eye, e);
        if (l != e[0] || r != e[1] || t != e[2] || b != e[3]) {
            printf("  FAIL  GetProjectionRaw(eye %d) tangents %g/%g/%g/%g "
                   "through the hook, expected %g/%g/%g/%g\n",
                   eye, l, r, t, b, e[0], e[1], e[2], e[3]);
            ++bad;
        }
    }

    // Slots 1 and 4, the struct returns through the asm thunks. Distinct
    // arguments per call so the stack positions get exercised too.
    for (int pass = 0; pass < 2; ++pass) {
        for (int32_t eye = 0; eye < 2; ++eye) {
            const float nearZ = 0.1f + static_cast<float>(pass);
            const float farZ = 1000.0f + static_cast<float>(eye) * 500.0f +
                               static_cast<float>(pass);
            const int32_t projType = pass;
            fakevr::M44 got = sys->GetProjectionMatrix(eye, nearZ, farZ, projType);
            fakevr::M44 want = fakevr::expectedMatrix(eye, nearZ, farZ, projType);
            if (memcmp(&got, &want, sizeof(got)) != 0) {
                printf("  FAIL  GetProjectionMatrix(eye %d, %g, %g, %d) came "
                       "back altered through the asm thunk (m00 %g want %g, "
                       "m02 %g want %g, m22 %g want %g, m31 %g want %g)\n",
                       eye, nearZ, farZ, projType, got.m[0][0], want.m[0][0],
                       got.m[0][2], want.m[0][2], got.m[2][2], want.m[2][2],
                       got.m[3][1], want.m[3][1]);
                ++bad;
            }
            fakevr::M34 gotE = sys->GetEyeToHeadTransform(eye);
            fakevr::M34 wantE = fakevr::expectedEyeToHead(eye);
            if (memcmp(&gotE, &wantE, sizeof(gotE)) != 0) {
                printf("  FAIL  GetEyeToHeadTransform(eye %d) came back altered "
                       "through the asm thunk (x %g want %g)\n",
                       eye, gotE.m[0][3], wantE.m[0][3]);
                ++bad;
            }
        }
    }

    // A SECOND object of the same class. In-place patching hooks the class,
    // so these calls reach the same thunks with a self the hook was not
    // installed for -- and must forward untouched all the same.
    auto secondPtr = fake ? reinterpret_cast<PFN_FakePtr>(
                                GetProcAddress(fake, "VR_FakeSecondSystemPtr"))
                          : nullptr;
    if (!secondPtr) {
        printf("  FAIL  the stand-in does not export VR_FakeSecondSystemPtr\n");
        ++bad;
    } else {
        auto* sys2 = static_cast<fakevr::ISystem012*>(secondPtr());
        fakevr::M44 got = sys2->GetProjectionMatrix(1, 0.5f, 750.0f, 1);
        fakevr::M44 want = fakevr::expectedMatrix(1, 0.5f, 750.0f, 1);
        if (memcmp(&got, &want, sizeof(got)) != 0) {
            printf("  FAIL  a second object of the hooked class did not get a "
                   "clean forward (m00 %g want %g)\n", got.m[0][0], want.m[0][0]);
            ++bad;
        }
        float l = 0, r = 0, t = 0, b = 0, e[4];
        sys2->GetProjectionRaw(0, &l, &r, &t, &b);
        fakevr::expectedRaw(0, e);
        if (l != e[0] || r != e[1]) {
            printf("  FAIL  a second object's GetProjectionRaw was disturbed "
                   "(%g/%g want %g/%g)\n", l, r, e[0], e[1]);
            ++bad;
        }
    }

    // The hook's own account of itself: installed, validated by the tangent
    // values above, not inert -- and the exact call counts, which is what
    // proves the thunks fired.
    auto selftest = reinterpret_cast<PFN_Selftest>(
        GetProcAddress(g_loaded, "edvr_selftest_system_hook"));
    if (!selftest) {
        printf("  FAIL  edvr_selftest_system_hook is not exported\n");
        ++bad;
    } else {
        const unsigned int v = selftest();
        if ((v & 1u) == 0) {
            printf("  FAIL  the observation hook reports not installed (0x%08X)\n", v);
            ++bad;
        }
        if ((v & 2u) == 0) {
            printf("  FAIL  the observation hook never validated the tangents "
                   "it was fed (0x%08X)\n", v);
            ++bad;
        }
        if ((v & 4u) != 0) {
            printf("  FAIL  the observation hook went inert on sane values "
                   "(0x%08X)\n", v);
            ++bad;
        }
        const unsigned int matrixCalls = (v >> 8) & 0xFF;
        const unsigned int eyeCalls = (v >> 16) & 0xFF;
        const unsigned int rawCalls = (v >> 24) & 0xFF;
        if (matrixCalls != 5 || eyeCalls != 4 || rawCalls != 3) {
            printf("  FAIL  thunk call counts matrix=%u eyeToHead=%u raw=%u, "
                   "expected 5/4/3 -- a call bypassed or double-counted\n",
                   matrixCalls, eyeCalls, rawCalls);
            ++bad;
        }
    }

    if (bad == 0)
        printf("  ok    IVRSystem_012 observation forwards every call untouched\n");
    return bad;
}

// The cull guard's copy mechanism, against a real device (WARP, like the
// resubmit checks): region selection, flip preservation, cache reuse and
// shape-change rebuild. The property under test is CONTENT -- the copy must
// hold exactly the composed crop region -- because the field failure this
// mechanism replaces was a region-interpretation bug in somebody else's
// layer, and the fix must not trade it for one of ours.
int guardCropChecks() {
    int bad = 0;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx)) ||
        !dev || !ctx) {
        printf("  FAIL  could not create a WARP device for the crop checks\n");
        return 1;
    }

    // 64x64, every 16px block stamped with its block id: pixel(x,y) =
    // (x/16)*16 + y/16 in every channel, so any misplaced region shows as
    // the wrong id at the copy's origin.
    constexpr uint32_t kSide = 64;
    std::vector<uint8_t> bytes(kSide * kSide * 4);
    for (uint32_t y = 0; y < kSide; ++y) {
        for (uint32_t x = 0; x < kSide; ++x) {
            const uint8_t v = static_cast<uint8_t>((x / 16) * 16 + y / 16);
            uint8_t* p = &bytes[(y * kSide + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 0xFF;
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kSide;
    d.Height = kSide;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = bytes.data();
    init.SysMemPitch = kSide * 4;
    ID3D11Texture2D* src = nullptr;
    dev->CreateTexture2D(&d, &init, &src);
    if (!src) {
        printf("  FAIL  could not create the crop check's source texture\n");
        ctx->Release();
        dev->Release();
        return 1;
    }

    auto pixelAt = [&](void* tex, uint32_t w, uint32_t h, uint8_t* out) {
        D3D11_TEXTURE2D_DESC sdc{};
        sdc.Width = w;
        sdc.Height = h;
        sdc.MipLevels = 1;
        sdc.ArraySize = 1;
        sdc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sdc.SampleDesc.Count = 1;
        sdc.Usage = D3D11_USAGE_STAGING;
        sdc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(dev->CreateTexture2D(&sdc, nullptr, &staging)) || !staging)
            return false;
        ctx->CopyResource(staging, static_cast<ID3D11Texture2D*>(tex));
        D3D11_MAPPED_SUBRESOURCE map{};
        bool ok = false;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            *out = static_cast<const uint8_t*>(map.pData)[0];
            ctx->Unmap(staging, 0);
            ok = true;
        }
        staging->Release();
        return ok;
    };

    // Null bounds, fractions {0.25, 0.5, 0.75, 1.0}: pixel box x 16..48,
    // y 32..64, so a 32x32 copy whose origin pixel is source(16,32) =
    // block id 16+2 = 18.
    const float fr[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    vr::VRTextureBounds_t ob{};
    void* out = edvr::guardCropCopy(0, src, nullptr, fr, 0, 0, &ob);
    if (!out) {
        printf("  FAIL  guardCropCopy refused a plain crop\n");
        ++bad;
    } else {
        D3D11_TEXTURE2D_DESC od{};
        static_cast<ID3D11Texture2D*>(out)->GetDesc(&od);
        if (od.Width != 32 || od.Height != 32) {
            printf("  FAIL  crop copy is %ux%u, expected 32x32\n", od.Width,
                   od.Height);
            ++bad;
        }
        uint8_t v = 0;
        if (!pixelAt(out, 32, 32, &v) || v != 18) {
            printf("  FAIL  crop origin holds block %u, expected 18 -- the "
                   "region landed in the wrong place\n", v);
            ++bad;
        }
        if (ob.uMin != 0.0f || ob.uMax != 1.0f || ob.vMin != 0.0f ||
            ob.vMax != 1.0f) {
            printf("  FAIL  unflipped source produced flipped-out bounds\n");
            ++bad;
        }
        // Same shape again: the cached texture is reused, not recreated.
        vr::VRTextureBounds_t ob2{};
        void* out2 = edvr::guardCropCopy(0, src, nullptr, fr, 0, 0, &ob2);
        if (out2 != out) {
            printf("  FAIL  an unchanged crop shape was not reused\n");
            ++bad;
        }
    }

    // The snap: the same fractions asked to land on 30x30 nudge the box a
    // pixel inward per side and stay in the same source block; asked to
    // land on 128x128 -- further than the 64px slack -- they refuse, which
    // is the not-the-adopted-target case.
    {
        vr::VRTextureBounds_t obs{};
        void* outs = edvr::guardCropCopy(0, src, nullptr, fr, 30, 30, &obs);
        if (!outs) {
            printf("  FAIL  the snap refused a 2px nudge\n");
            ++bad;
        } else {
            D3D11_TEXTURE2D_DESC od{};
            static_cast<ID3D11Texture2D*>(outs)->GetDesc(&od);
            uint8_t v = 0;
            if (od.Width != 30 || od.Height != 30) {
                printf("  FAIL  snapped crop is %ux%u, expected 30x30\n",
                       od.Width, od.Height);
                ++bad;
            } else if (!pixelAt(outs, 30, 30, &v) || v != 18) {
                printf("  FAIL  snapped crop origin holds block %u, expected "
                       "18\n", v);
                ++bad;
            }
        }
        if (edvr::guardCropCopy(0, src, nullptr, fr, 128, 128, &obs) != nullptr) {
            printf("  FAIL  a snap far beyond the slack was not refused\n");
            ++bad;
        }
    }

    // Flipped v in the game's bounds: composed span runs 1->0, so the same
    // fractions select from the OTHER end of v (top fraction 0.5 measured
    // from v=1 downwards -> pixel y 0..32), and the out bounds must stay
    // flipped. Origin pixel = source(16,0) = block 16.
    {
        vr::VRTextureBounds_t flipped = {0.0f, 1.0f, 1.0f, 0.0f};
        vr::VRTextureBounds_t obf{};
        void* outf = edvr::guardCropCopy(1, src, &flipped, fr, 0, 0, &obf);
        if (!outf) {
            printf("  FAIL  guardCropCopy refused a flipped-v crop\n");
            ++bad;
        } else {
            uint8_t v = 0;
            if (!pixelAt(outf, 32, 32, &v) || v != 16) {
                printf("  FAIL  flipped crop origin holds block %u, expected "
                       "16 -- the flip was not composed\n", v);
                ++bad;
            }
            if (obf.vMin != 1.0f || obf.vMax != 0.0f) {
                printf("  FAIL  a flipped submission lost its flip "
                       "(v %g..%g)\n", obf.vMin, obf.vMax);
                ++bad;
            }
        }
    }

    // A shape change rebuilds rather than reuses: half-width fractions give
    // a 16-wide copy on eye 0.
    {
        const float fr2[4] = {0.25f, 0.5f, 0.5f, 1.0f};
        vr::VRTextureBounds_t ob3{};
        void* out3 = edvr::guardCropCopy(0, src, nullptr, fr2, 0, 0, &ob3);
        if (!out3) {
            printf("  FAIL  guardCropCopy refused the reshaped crop\n");
            ++bad;
        } else {
            D3D11_TEXTURE2D_DESC od{};
            static_cast<ID3D11Texture2D*>(out3)->GetDesc(&od);
            if (od.Width != 16 || od.Height != 32) {
                printf("  FAIL  reshaped crop is %ux%u, expected 16x32\n",
                       od.Width, od.Height);
                ++bad;
            }
        }
    }

    edvr::guardCropShutdown();
    src->Release();
    ctx->Release();
    dev->Release();

    if (bad == 0)
        printf("  ok    the crop copy takes the right region, keeps flips, "
               "and rebuilds on shape changes\n");
    return bad;
}

// The cull guard, end to end, in a CHILD process with the guard armed.
//
// A child because the guard's whole design is install-time and one-way: the
// mode is read when IVRSystem_012 is first requested (that decides whether
// slot 1 gets the member-shaped receiver), the config is cached in the
// installed state, and the parent's own checks above depend on the guard
// being OFF (they assert truth passes through). One process cannot honestly
// test both.
//
// What the child asserts, in order: truth BEFORE go-live; the symmetrized
// tangents and the formula-rebuilt matrix AFTER the boundary-less fallback
// promotes the lie; the crop fractions through the selftest export; and a
// second object of the hooked class still receiving pure truth while the
// lie is live for the game's own interface.
int guardChild(const char* dir) {
    wchar_t proxy[MAX_PATH];
    _snwprintf_s(proxy, _TRUNCATE, L"%hs\\openvr_api.dll", dir);
    HMODULE m = LoadLibraryW(proxy);
    if (!m) { printf("  FAIL  guard child could not load the proxy\n"); return 10; }

    typedef void*(__cdecl* PFN_GetGenericInterface)(const char*, int*);
    typedef unsigned int(*PFN_Crop)(int, float*);
    typedef void*(*PFN_FakePtr)(void);

    auto getIface = reinterpret_cast<PFN_GetGenericInterface>(
        GetProcAddress(m, "VR_GetGenericInterface"));
    if (!getIface) { printf("  FAIL  guard child: no VR_GetGenericInterface\n"); return 11; }
    int err = -1;
    void* iface = getIface("IVRSystem_012", &err);
    if (!iface) { printf("  FAIL  guard child: interface came back null\n"); return 12; }
    auto* sys = static_cast<fakevr::ISystem012*>(iface);

    int bad = 0;
    float l = 0, r = 0, t = 0, b = 0, e[4];

    // Before go-live: pure truth, both eyes (which is also what arms the
    // lie -- it waits for both eyes' true tangents).
    sys->GetProjectionRaw(0, &l, &r, &t, &b);
    fakevr::expectedRaw(0, e);
    if (l != e[0] || r != e[1] || t != e[2] || b != e[3]) {
        printf("  FAIL  guard child: pre-live raw was not the truth "
               "(%g/%g/%g/%g)\n", l, r, t, b);
        ++bad;
    }
    sys->GetProjectionRaw(1, &l, &r, &t, &b);
    fakevr::M44 got = sys->GetProjectionMatrix(0, 0.5f, 100.0f, 0);
    fakevr::M44 want = fakevr::expectedMatrix(0, 0.5f, 100.0f, 0);
    if (memcmp(&got, &want, sizeof(got)) != 0) {
        printf("  FAIL  guard child: the matrix was edited before go-live "
               "(m00 %g want %g)\n", got.m[0][0], want.m[0][0]);
        ++bad;
    }

    // No compositor exists here, so no frame boundary ever fires; the
    // two-second fallback in periodic() is the promoter, and periodic runs
    // at the tail of the observed calls themselves. TWO warmup calls: the
    // first advances to stage 1 (size lie), the next to stage 2 (the
    // projection lie) -- each stage flip happens after the call's own
    // values were already answered.
    Sleep(2300);
    sys->GetProjectionRaw(0, &l, &r, &t, &b);  // -> stage 1 after this call
    sys->GetProjectionRaw(0, &l, &r, &t, &b);  // -> stage 2 after this call

    // Stage 2 implies the size lie too: the fake's 1456x1584 recommendation
    // is answered inflated by the lied spans (u 2.0 -> 2.5 = 1.25x; v at
    // fraction 0.5, 2.0 -> 2.2 = 1.1x).
    {
        uint32_t w = 0, h = 0;
        sys->GetRecommendedRenderTargetSize(&w, &h);
        if (w != 1820 || h != 1742) {
            printf("  FAIL  guard child: inflated target size is %ux%u, "
                   "expected 1820x1742\n", w, h);
            ++bad;
        }
    }

    // After go-live: the symmetric lie at fraction_v = 0.5. Left eye truth
    // is l=-1.25 r=+0.75 t=-1.2 b=+0.8: horizontal fully symmetrized to
    // +/-1.25 (fraction_h defaults to 1), vertical's short side covers HALF
    // its deficit -- b = 0.8 + 0.5*(1.2-0.8) = 1.0 -- and the long side
    // stays put.
    sys->GetProjectionRaw(0, &l, &r, &t, &b);
    if (l != -1.25f || r != 1.25f || t != -1.2f || fabsf(b - 1.0f) > 1e-5f) {
        printf("  FAIL  guard child: left eye post-live raw is %g/%g/%g/%g, "
               "expected -1.25/+1.25/-1.2/+1.0\n", l, r, t, b);
        ++bad;
    }
    sys->GetProjectionRaw(1, &l, &r, &t, &b);
    if (l != -1.25f || r != 1.25f || fabsf(b - 1.0f) > 1e-5f) {
        printf("  FAIL  guard child: right eye post-live raw is %g/%g/%g, "
               "expected -1.25/+1.25/+1.0\n", l, r, b);
        ++bad;
    }

    // The matrix, rebuilt from the lied tangents: m00 = 2/2.5, m02 = 0;
    // the half-covered vertical spans -1.2..+1.0, so m11 = 2/2.2 and
    // m12 = (1.0-1.2)/2.2; the z terms (from THIS call's near/far)
    // untouched.
    got = sys->GetProjectionMatrix(0, 0.5f, 100.0f, 0);
    want = fakevr::expectedMatrix(0, 0.5f, 100.0f, 0);
    if (fabsf(got.m[0][0] - 0.8f) > 1e-5f || fabsf(got.m[0][2]) > 1e-5f) {
        printf("  FAIL  guard child: matrix not rebuilt from the lie "
               "(m00 %g want 0.8, m02 %g want 0)\n", got.m[0][0], got.m[0][2]);
        ++bad;
    }
    if (fabsf(got.m[1][1] - 2.0f / 2.2f) > 1e-4f ||
        fabsf(got.m[1][2] - (-0.2f / 2.2f)) > 1e-4f) {
        printf("  FAIL  guard child: the fractioned vertical is wrong "
               "(m11 %g want %g, m12 %g want %g)\n", got.m[1][1], 2.0f / 2.2f,
               got.m[1][2], -0.2f / 2.2f);
        ++bad;
    }
    if (fabsf(got.m[2][2] - want.m[2][2]) > 1e-5f ||
        fabsf(got.m[2][3] - want.m[2][3]) > 1e-5f ||
        got.m[3][1] != want.m[3][1] || got.m[3][2] != want.m[3][2]) {
        printf("  FAIL  guard child: the z terms or the echo element were "
               "disturbed (m22 %g want %g)\n", got.m[2][2], want.m[2][2]);
        ++bad;
    }

    // The crop fractions the submit side will use. Left eye horizontal: the
    // outer (left) edge is unchanged so u starts at 0 and keeps
    // (0.75+1.25)/2.5 = 0.8. Vertical at fraction 0.5: v=0 is the B edge
    // (the positive tangent -- the direction the field inverted once), so
    // the top fraction is (b'-b)/span = (1.0-0.8)/2.2 and the bottom is
    // (b'-t)/span = 2.2/2.2 = 1. Right eye mirrored in u: 0.2..1.
    auto crop = reinterpret_cast<PFN_Crop>(
        GetProcAddress(m, "edvr_selftest_cull_guard"));
    if (!crop) {
        printf("  FAIL  guard child: edvr_selftest_cull_guard not exported\n");
        ++bad;
    } else {
        const float vTop = 0.2f / 2.2f;
        float f[4] = {};
        if (crop(0, f) != 1u || fabsf(f[0]) > 1e-5f ||
            fabsf(f[2] - 0.8f) > 1e-5f || fabsf(f[1] - vTop) > 1e-4f ||
            fabsf(f[3] - 1.0f) > 1e-4f) {
            printf("  FAIL  guard child: left eye crop fractions %g/%g/%g/%g, "
                   "expected 0/%g/0.8/1 -- if the second value is 0 the "
                   "v axis has been inverted again\n",
                   f[0], f[1], f[2], f[3], vTop);
            ++bad;
        }
        if (crop(1, f) != 1u || fabsf(f[0] - 0.2f) > 1e-5f ||
            fabsf(f[2] - 1.0f) > 1e-5f || fabsf(f[1] - vTop) > 1e-4f) {
            printf("  FAIL  guard child: right eye crop fractions %g/%g/%g, "
                   "expected 0.2/%g/1\n", f[0], f[1], f[2], vTop);
            ++bad;
        }
    }

    // A second object of the hooked class, while the lie is live: pure
    // truth. The lie is for the interface the game was handed, nobody else.
    HMODULE fake = GetModuleHandleW(L"openvr_api_orig.dll");
    auto secondPtr = fake ? reinterpret_cast<PFN_FakePtr>(
                                GetProcAddress(fake, "VR_FakeSecondSystemPtr"))
                          : nullptr;
    if (!secondPtr) {
        printf("  FAIL  guard child: no second fake object to test with\n");
        ++bad;
    } else {
        auto* sys2 = static_cast<fakevr::ISystem012*>(secondPtr());
        sys2->GetProjectionRaw(0, &l, &r, &t, &b);
        fakevr::expectedRaw(0, e);
        if (l != e[0] || r != e[1]) {
            printf("  FAIL  guard child: a foreign object was lied to "
                   "(%g/%g want %g/%g)\n", l, r, e[0], e[1]);
            ++bad;
        }
        fakevr::M44 got2 = sys2->GetProjectionMatrix(1, 0.5f, 100.0f, 1);
        fakevr::M44 want2 = fakevr::expectedMatrix(1, 0.5f, 100.0f, 1);
        if (memcmp(&got2, &want2, sizeof(got2)) != 0) {
            printf("  FAIL  guard child: a foreign object's matrix was edited "
                   "(m00 %g want %g)\n", got2.m[0][0], want2.m[0][0]);
            ++bad;
        }
    }

    return bad == 0 ? 0 : 13;
}

// The crash sentinel's lifecycle, which is shared code with none of its own.
//
// Both of its bugs were lifecycle rather than logic, and both were invisible:
//
//   arm() set its flag whether or not the file was written. The file lives in
//   the log directory, which only Log::open() creates -- and that returns early
//   when log.enabled = 0. So with logging off nothing was ever written, no
//   launch saw a trip, and a hook that really was crashing re-armed every start:
//   the protection was absent in exactly the configuration with no log to
//   diagnose it from.
//
//   A trip was permanent. confirm() runs on validation, on commit failure, and
//   from a shutdown needing FreeLibrary that a closing game never does -- so any
//   session ending early left the file behind and every later launch refused,
//   announcing a crash that never happened.
//
// Asserted here rather than trusted, because neither failure shows up as a
// crash or a wrong pixel; they show up as a fix that quietly is not running.
// The channel the head-offset gate runs on.
//
// d3d11.dll decides which mode the player is in and openvr_api.dll acts on it,
// so the answer crosses a module boundary through a named mapping. A mistake in
// that struct is invisible at compile time in BOTH modules and shows up only as
// a viewpoint that moves in the cockpit, so the round trip is asserted here --
// where the other cross-module channel already is.
int frameFlagChecks() {
    int bad = 0;

    edvr::setExternalCameraOnFoot(false);
    if (edvr::externalCameraOnFoot()) {
        printf("  FAIL  the mode flag reads true after being cleared\n");
        ++bad;
    }
    edvr::setExternalCameraOnFoot(true);
    if (!edvr::externalCameraOnFoot()) {
        printf("  FAIL  the mode flag did not survive a set\n");
        ++bad;
    }

    // It must NOT behave like the glitch mark. That one is cleared every frame
    // by design; this one is a STATE, and clearing it at the frame boundary
    // would drop the player out of the offset one frame after entering it.
    edvr::clearGlitchFrame();
    if (!edvr::externalCameraOnFoot()) {
        printf("  FAIL  the frame boundary cleared the mode state\n");
        ++bad;
    }

    // ...and the two must not share storage.
    edvr::markGlitchFrame();
    edvr::setExternalCameraOnFoot(false);
    if (!edvr::glitchFrameMarked()) {
        printf("  FAIL  clearing the mode flag also cleared the glitch mark\n");
        ++bad;
    }
    edvr::clearGlitchFrame();

    if (bad == 0) printf("  ok    the mode flag round-trips and is independent\n");
    return bad;
}

// Both eyes of a frame get the same verdict, whenever the flag moves.
//
// The mark is read once per eye, at each Submit, and it legitimately changes
// mid-frame -- the detector re-decides on every new furthest camera. A change
// landing between the two Submits would show one eye this frame and the other a
// reprojection of the last one: a one-frame binocular mismatch, which is what a
// flash feels like. The fix for flashes, producing one.
//
// The channel carries no frame identity (EDVR-31), so this is asserted on the
// latch rather than on frame numbers.
int submitPairChecks() {
    int bad = 0;

    // The flag comes up between the two eyes. The first eye decided "show it",
    // so the second must show it too.
    {
        edvr::SubmitPairLatch latch;
        const bool left = latch.verdict(false);
        const bool right = latch.verdict(true);
        if (left || right) {
            printf("  FAIL  the flag rising between eyes split the pair "
                   "(left %s, right %s)\n", left ? "withheld" : "shown",
                   right ? "withheld" : "shown");
            ++bad;
        }
    }

    // And the other direction: the first eye decided "withhold", so the second
    // is withheld even though the mark has since been withdrawn. Consistent-late
    // rather than one eye ahead of the other.
    {
        edvr::SubmitPairLatch latch;
        const bool left = latch.verdict(true);
        const bool right = latch.verdict(false);
        if (!left || !right) {
            printf("  FAIL  the flag falling between eyes split the pair "
                   "(left %s, right %s)\n", left ? "withheld" : "shown",
                   right ? "withheld" : "shown");
            ++bad;
        }
    }

    // A third read -- a runtime that submits more than twice, or a retry --
    // still follows the frame's verdict rather than re-deciding.
    {
        edvr::SubmitPairLatch latch;
        latch.verdict(true);
        latch.verdict(false);
        if (!latch.verdict(false)) {
            printf("  FAIL  a third Submit in one frame re-decided\n");
            ++bad;
        }
    }

    // The boundary releases it, so the next frame is judged on its own merits.
    // Without this one detection would withhold every frame that followed, which
    // is the headset freezing rather than skipping a frame.
    {
        edvr::SubmitPairLatch latch;
        latch.verdict(true);
        latch.reset();
        if (latch.latched() || latch.verdict(false)) {
            printf("  FAIL  the frame boundary did not release the pair verdict\n");
            ++bad;
        }
    }

    if (bad == 0)
        printf("  ok    both eyes of a frame get the same verdict\n");
    return bad;
}

// The resubmit shadow: a withhold hands SteamVR the game's own PREVIOUS
// frame, never the live one and never EDVR-authored pixels (1f).
//
// Driven against a real D3D11 device (WARP, so no GPU and no headset is
// needed) with textures whose bytes are known, because the property under
// test is about CONTENTS: the substitute must hold what was last forwarded,
// a withheld frame's content must never reach it, and a shape change must
// fall back to classic withholding rather than submit a stale-shaped copy.
int resubmitChecks() {
    int bad = 0;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx)) ||
        !dev || !ctx) {
        // WARP ships with Windows 8+; failing to create it is a broken machine,
        // not an acceptable skip -- a skipped cell would report a fix as
        // covered that no test had touched.
        printf("  FAIL  could not create a WARP device to test the resubmit "
               "shadow against\n");
        return 1;
    }

    auto makeTex = [&](uint32_t side, uint8_t fill) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = side;
        d.Height = side;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        std::vector<uint8_t> bytes(static_cast<size_t>(side) * side * 4, fill);
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = bytes.data();
        init.SysMemPitch = side * 4;
        ID3D11Texture2D* t = nullptr;
        dev->CreateTexture2D(&d, &init, &t);
        return t;
    };
    auto firstByte = [&](void* tex, uint32_t side, uint8_t* out) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = side;
        d.Height = side;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)) || !staging)
            return false;
        ctx->CopyResource(staging, static_cast<ID3D11Texture2D*>(tex));
        D3D11_MAPPED_SUBRESOURCE map{};
        bool ok = false;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            *out = static_cast<const uint8_t*>(map.pData)[0];
            ctx->Unmap(staging, 0);
            ok = true;
        }
        staging->Release();
        return ok;
    };

    ID3D11Texture2D* texA = makeTex(64, 0xAA);   // the frame that gets forwarded
    ID3D11Texture2D* texB = makeTex(64, 0xBB);   // the live (withheld) frame
    ID3D11Texture2D* texC = makeTex(128, 0xCC);  // a shape change
    if (!texA || !texB || !texC) {
        printf("  FAIL  could not create the resubmit test textures\n");
        if (texA) texA->Release();
        if (texB) texB->Release();
        if (texC) texC->Release();
        ctx->Release();
        dev->Release();
        return 1;
    }

    edvr::resubmitShadowConfigure();

    // Before anything was forwarded, a withhold has nothing to hand over --
    // classic withholding, which is the never-worse-than-before floor.
    if (edvr::resubmitShadowForWithhold(0, texB) != nullptr) {
        printf("  FAIL  a substitute was offered before any frame had been "
               "forwarded\n");
        ++bad;
    }

    // Pattern A is forwarded; the next frame is withheld while the live
    // texture holds pattern B. The substitute must be a texture that is
    // neither the live one nor the forwarded original, holding A's bytes.
    edvr::resubmitShadowNoteForwarded(0, texA);
    void* sub = edvr::resubmitShadowForWithhold(0, texB);
    if (!sub || sub == texB || sub == texA) {
        printf("  FAIL  the withheld frame was not offered an EDVR-owned copy "
               "(got %p, live %p, forwarded %p)\n", sub,
               static_cast<void*>(texB), static_cast<void*>(texA));
        ++bad;
    } else {
        uint8_t v = 0;
        if (!firstByte(sub, 64, &v) || v != 0xAA) {
            printf("  FAIL  the substitute holds 0x%02X, not the forwarded "
                   "frame's 0xAA\n", v);
            ++bad;
        }
    }

    // A second consecutive withhold forwards the SAME content again: nothing
    // a withheld frame carries may reach the shadow.
    void* sub2 = edvr::resubmitShadowForWithhold(0, texB);
    if (sub2 != sub) {
        printf("  FAIL  consecutive withholds got different substitutes\n");
        ++bad;
    } else if (sub2) {
        uint8_t v = 0;
        if (!firstByte(sub2, 64, &v) || v != 0xAA) {
            printf("  FAIL  the second consecutive withhold's content changed "
                   "to 0x%02X -- a withheld frame reached the shadow\n", v);
            ++bad;
        }
    }

    // The eye texture changes shape mid-flight: this withhold must fall back
    // to classic (nullptr) rather than submit a 64x64 copy where a 128x128
    // frame belongs.
    if (edvr::resubmitShadowForWithhold(0, texC) != nullptr) {
        printf("  FAIL  a shape change mid-flight was handed the stale-shaped "
               "copy\n");
        ++bad;
    }

    // ...and the next FORWARDED frame at the new shape rebuilds the copy.
    edvr::resubmitShadowNoteForwarded(0, texC);
    void* sub3 = edvr::resubmitShadowForWithhold(0, texC);
    if (!sub3 || sub3 == texC) {
        printf("  FAIL  the copy did not rebuild after a shape change\n");
        ++bad;
    } else {
        uint8_t v = 0;
        if (!firstByte(sub3, 128, &v) || v != 0xCC) {
            printf("  FAIL  the rebuilt copy holds 0x%02X, not the newly "
                   "forwarded 0xCC\n", v);
            ++bad;
        }
    }

    // The eyes do not share a shadow: eye 1 never forwarded, so it still
    // withholds classically whatever eye 0 has.
    if (edvr::resubmitShadowForWithhold(1, texB) != nullptr) {
        printf("  FAIL  eye 1 was offered eye 0's frame\n");
        ++bad;
    }

    edvr::resubmitShadowShutdown();
    texA->Release();
    texB->Release();
    texC->Release();
    ctx->Release();
    dev->Release();

    if (bad == 0)
        printf("  ok    a withheld frame resubmits the game's previous frame, "
               "and only that\n");
    return bad;
}

// Which binding fires, given what is physically held.
//
// This is the rule that decides whether EDVR sees the same camera keypress the
// game sees, and a miss is expensive: external_camera is a TOGGLE, so one
// dropped press inverts the intent for the rest of the session and the offset
// arms in the wrong place or never arms at all. Asserted rather than reasoned
// about, because the failure is invisible -- a press that did nothing.
int hotkeyMatchChecks() {
    int bad = 0;
    const uint32_t C = edvr::kHotkeyCtrl, A = edvr::kHotkeyAlt, S = edvr::kHotkeyShift;

    edvr::hotkeyResetBindings();
    edvr::Hotkey combo, plainOther;
    combo.setBinding("CTRL+ALT+SPACE");        // Elite's default camera bind
    plainOther.setBinding("F9");               // an unrelated single key

    // THE CASE THAT MOTIVATED THIS. Sprinting holds Shift; the player presses
    // their camera key. Under equality this missed, and the intent desynced.
    if (!edvr::hotkeyWouldFire(VK_SPACE, C | A, C | A | S)) {
        printf("  FAIL  CTRL+ALT+SPACE did not fire with Shift also held\n");
        ++bad;
    }
    if (!edvr::hotkeyWouldFire(VK_SPACE, C | A, C | A)) {
        printf("  FAIL  CTRL+ALT+SPACE did not fire with exactly its modifiers\n");
        ++bad;
    }
    // ...but a missing required modifier still must not fire.
    if (edvr::hotkeyWouldFire(VK_SPACE, C | A, C)) {
        printf("  FAIL  CTRL+ALT+SPACE fired with Alt not held\n");
        ++bad;
    }
    if (edvr::hotkeyWouldFire(VK_SPACE, C | A, 0)) {
        printf("  FAIL  CTRL+ALT+SPACE fired on a bare press\n");
        ++bad;
    }

    // LEGACY SEMANTICS. A plain binding with no combo on the same key fires
    // whatever else is held -- which is how Scroll Lock and Pause behaved before
    // modifiers existed here at all. Equality silently changed that.
    if (!edvr::hotkeyWouldFire(VK_F9, 0, S)) {
        printf("  FAIL  a plain binding stopped firing while Shift was held -- "
               "an unannounced change to keys that predate combos\n");
        ++bad;
    }

    // ONE PRESS, ONE BINDING. With both bound on the same key, CTRL+ALT+SPACE
    // must fire the combo and NOT the bare one: firing both would set the
    // camera intent and clear it again in the same frame.
    edvr::Hotkey plainSame;
    plainSame.setBinding("SPACE");
    if (!edvr::hotkeyWouldFire(VK_SPACE, C | A, C | A)) {
        printf("  FAIL  registering a bare SPACE binding broke the combo\n");
        ++bad;
    }
    if (edvr::hotkeyWouldFire(VK_SPACE, 0, C | A)) {
        printf("  FAIL  one press fired BOTH the combo and the bare binding on "
               "the same key\n");
        ++bad;
    }
    // ...and the bare one still fires on its own press.
    if (!edvr::hotkeyWouldFire(VK_SPACE, 0, 0)) {
        printf("  FAIL  the bare binding stopped firing on a bare press\n");
        ++bad;
    }
    // The suppression must be specific to the key, not global.
    if (!edvr::hotkeyWouldFire(VK_F9, 0, C | A)) {
        printf("  FAIL  a combo on SPACE suppressed an unrelated binding on F9\n");
        ++bad;
    }

    edvr::hotkeyResetBindings();
    if (bad == 0)
        printf("  ok    modifier matching: extras allowed, one press one binding\n");
    return bad;
}

// One physical press must produce at most one edge, per binding.
//
// The parsing and matching tests above cover which binding SHOULD fire. This
// covers when the edge happens, which is a separate thing and is where the
// harder bug lived: a binding that was correctly suppressed could still mint an
// edge later, from a press that was already over.
int hotkeyEdgeChecks() {
    int bad = 0;
    const uint32_t C = edvr::kHotkeyCtrl, A = edvr::kHotkeyAlt, S = edvr::kHotkeyShift;

    edvr::hotkeyResetBindings();
    edvr::Hotkey combo, bare;
    combo.setBinding("CTRL+ALT+SPACE");
    bare.setBinding("SPACE");

    // THE BUG. The player presses CTRL+ALT+SPACE and lets go of the modifiers
    // slightly before the spacebar, which is how anybody releases a chord.
    //
    //   frame 1  all three down   -> the combo fires, the bare one is suppressed
    //   frame 2  modifiers up, SPACE still down
    //            -> suppression lifts. The bare binding used to fire HERE,
    //               from a press the player made once and had finished with.
    int comboFires = 0, bareFires = 0;
    if (combo.pressedWith(true, C | A, true)) ++comboFires;
    if (bare.pressedWith(true, C | A, true)) ++bareFires;
    if (combo.pressedWith(true, 0, true)) ++comboFires;
    if (bare.pressedWith(true, 0, true)) ++bareFires;      // <- the old edge
    // ...and the key finally comes up.
    combo.pressedWith(false, 0, true);
    bare.pressedWith(false, 0, true);

    if (comboFires != 1) {
        printf("  FAIL  the combo fired %d time(s) for one press, expected 1\n",
               comboFires);
        ++bad;
    }
    if (bareFires != 0) {
        printf("  FAIL  releasing the modifiers minted %d edge(s) on the bare "
               "binding from a press that was already over\n", bareFires);
        ++bad;
    }

    // Holding a key does not repeat.
    edvr::hotkeyResetBindings();
    edvr::Hotkey solo;
    solo.setBinding("F9");
    int fires = 0;
    for (int i = 0; i < 20; ++i) {
        if (solo.pressedWith(true, 0, true)) ++fires;
    }
    if (fires != 1) {
        printf("  FAIL  holding a key fired %d time(s), expected 1\n", fires);
        ++bad;
    }
    // ...and releasing then pressing again does.
    solo.pressedWith(false, 0, true);
    if (!solo.pressedWith(true, 0, true)) {
        printf("  FAIL  a second press after releasing did not fire\n");
        ++bad;
    }

    // A press made while the game does NOT have focus must not fire when focus
    // comes back with the key still held. Same shape as the chord case: the
    // edge would come from a press aimed at another application.
    edvr::hotkeyResetBindings();
    edvr::Hotkey bg;
    bg.setBinding("F10");
    bg.pressedWith(true, 0, false);          // pressed elsewhere
    if (bg.pressedWith(true, 0, true)) {
        printf("  FAIL  a key held from before the game regained focus fired\n");
        ++bad;
    }
    // A fresh press once focused does fire.
    bg.pressedWith(false, 0, true);
    if (!bg.pressedWith(true, 0, true)) {
        printf("  FAIL  a fresh press after focus returned did not fire\n");
        ++bad;
    }

    edvr::hotkeyResetBindings();
    if (bad == 0)
        printf("  ok    one physical press produces at most one edge\n");
    return bad;
}

// Hotkey bindings, including combinations.
//
// Elite's own default for the external camera is CTRL + ALT + SPACE, so chords
// are the normal case rather than an extra. A parser that dropped the modifiers
// would leave a binding watching bare SPACE -- firing constantly, in a build
// whose whole job is to know which mode the player asked for. Silent, and
// exactly backwards, so it is asserted.
int hotkeyChecks() {
    int bad = 0;
    uint32_t m = 0xFFFFFFFFu;

    if (edvr::virtualKeyFromName("F9", &m) != VK_F9 || m != 0) {
        printf("  FAIL  a plain key did not parse, or invented modifiers\n");
        ++bad;
    }
    const int space = edvr::virtualKeyFromName("CTRL+ALT+SPACE", &m);
    if (space != VK_SPACE || m != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  CTRL+ALT+SPACE parsed as vk=%d mods=%u\n", space, m);
        ++bad;
    }
    // Elite's default written the other ways people write it.
    if (edvr::virtualKeyFromName("ctrl + alt + space", &m) != VK_SPACE ||
        m != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  lower case and spaces did not parse the same\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("CONTROL-MENU-SPACE", &m) != VK_SPACE ||
        m != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  the CONTROL/MENU spellings or '-' did not parse\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("SHIFT+F11", &m) != VK_F11 ||
        m != edvr::kHotkeyShift) {
        printf("  FAIL  SHIFT+F11 did not parse\n");
        ++bad;
    }
    // A component that is not a modifier must make the WHOLE binding
    // unrecognised. Taking the last part and ignoring the rest would turn a
    // typo into a different, live binding.
    if (edvr::virtualKeyFromName("CTRL+WOMBAT+SPACE", &m) != 0) {
        printf("  FAIL  an unknown modifier did not reject the binding\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("CTRL+", &m) != 0) {
        printf("  FAIL  a binding with no key was accepted\n");
        ++bad;
    }

    // The punctuation row (6az: a field user bound '\\' and '[' and got
    // "not a key name EDVR knows"). Characters resolve through the keyboard
    // layout, names resolve to the same key as their character, and '-' is a
    // KEY unless it follows a modifier word -- the old parser split on it
    // anywhere, which made the minus key unbindable.
    const int backslash = edvr::virtualKeyFromName("\\", &m);
    if (backslash == 0 || m != 0) {
        printf("  FAIL  '\\' did not bind (vk=%d mods=%u)\n", backslash, m);
        ++bad;
    }
    if (edvr::virtualKeyFromName("BACKSLASH", &m) != backslash) {
        printf("  FAIL  BACKSLASH and '\\' bound different keys\n");
        ++bad;
    }
    const int lbracket = edvr::virtualKeyFromName("[", &m);
    if (lbracket == 0) {
        printf("  FAIL  '[' did not bind\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("LEFTBRACKET", &m) != lbracket) {
        printf("  FAIL  LEFTBRACKET and '[' bound different keys\n");
        ++bad;
    }
    const int minus = edvr::virtualKeyFromName("-", &m);
    if (minus == 0 || m != 0) {
        printf("  FAIL  '-' alone did not bind as the minus key\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("SHIFT+-", &m) != minus ||
        m != edvr::kHotkeyShift) {
        printf("  FAIL  SHIFT+- did not parse as shift and the minus key\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName(";", &m) == 0) {
        printf("  FAIL  ';' did not bind\n");
        ++bad;
    }
    // A shifted character names the same physical key as its base: the high
    // half of the layout lookup is deliberately ignored.
    if (edvr::virtualKeyFromName("|", &m) != backslash) {
        printf("  FAIL  '|' and '\\' are the same physical key and did not "
               "bind alike\n");
        ++bad;
    }
    // HASH exists because a bare '#' after "= " is eaten by the ini's own
    // trailing-comment rule -- the name is the reliable spelling there.
    const int hash = edvr::virtualKeyFromName("HASH", &m);
    if (hash == 0 || edvr::virtualKeyFromName("#", &m) != hash) {
        printf("  FAIL  HASH and '#' did not bind the same key\n");
        ++bad;
    }

    // Elite's Key_ names translate to bindings this parser accepts -- the
    // bridge that lets bindings be adopted from the game's own files.
    char t[32];
    if (!edvr::eliteBindsTranslateKey("Key_F11", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) != VK_F11) {
        printf("  FAIL  Key_F11 did not translate to F11\n");
        ++bad;
    }
    if (!edvr::eliteBindsTranslateKey("Key_RightArrow", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) != VK_RIGHT) {
        printf("  FAIL  Key_RightArrow did not translate to RIGHT\n");
        ++bad;
    }
    if (!edvr::eliteBindsTranslateKey("Key_LeftBracket", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) == 0) {
        printf("  FAIL  Key_LeftBracket did not translate to a bindable key\n");
        ++bad;
    }
    if (!edvr::eliteBindsTranslateKey("Key_SemiColon", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) == 0) {
        printf("  FAIL  Key_SemiColon did not translate to a bindable key\n");
        ++bad;
    }
    if (edvr::eliteBindsTranslateKey("Key_LeftShift", t, sizeof(t))) {
        printf("  FAIL  a bare modifier translated as a main key\n");
        ++bad;
    }

    // The bindings-change fingerprint: stable across reads when nothing
    // changed, moved by a file growing (a rebind rewrites the file), and 0
    // for a directory that is not there -- the trio live re-adoption needs.
    {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        wchar_t dir[MAX_PATH] = {};
        swprintf_s(dir, L"%sedvr_binds_fp_test", tmp);
        CreateDirectoryW(dir, nullptr);
        wchar_t file[MAX_PATH] = {};
        swprintf_s(file, L"%s\\Custom.4.0.binds", dir);
        DWORD wrote = 0;
        HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        WriteFile(f, "<Root/>", 7, &wrote, nullptr);
        CloseHandle(f);
        const unsigned long long fpA = edvr::eliteBindsFingerprintDir(dir);
        const unsigned long long fpB = edvr::eliteBindsFingerprintDir(dir);
        f = CreateFileW(file, FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        WriteFile(f, "<!-- rebound -->", 16, &wrote, nullptr);
        CloseHandle(f);
        const unsigned long long fpC = edvr::eliteBindsFingerprintDir(dir);
        DeleteFileW(file);
        RemoveDirectoryW(dir);
        const unsigned long long fpGone = edvr::eliteBindsFingerprintDir(dir);
        if (fpA == 0 || fpA != fpB) {
            printf("  FAIL  bindings fingerprint not stable across unchanged "
                   "reads\n");
            ++bad;
        }
        if (fpC == fpA) {
            printf("  FAIL  bindings fingerprint did not move when a file "
                   "changed\n");
            ++bad;
        }
        if (fpGone != 0) {
            printf("  FAIL  a missing bindings directory did not read as 0\n");
            ++bad;
        }
    }

    // The lookup's file and element selection, against a fixture directory
    // reproducing the 12:14 field failure: Elite keeps a stale
    // previous-format preset (Custom.4.1.binds, untouched since 2025)
    // beside the file it actually maintains (Custom.4.2.binds, rewritten
    // on every Apply), and on foot it acts on PhotoCameraToggle_Humanoid,
    // not the ship's PhotoCameraToggle.
    {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        wchar_t dir[MAX_PATH] = {};
        swprintf_s(dir, L"%sedvr_binds_lookup_test", tmp);
        CreateDirectoryW(dir, nullptr);
        auto writeFile = [&](const wchar_t* name, const char* body,
                             int yearsOld) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\%s", dir, name);
            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            DWORD wrote = 0;
            WriteFile(f, body, (DWORD)strlen(body), &wrote, nullptr);
            if (yearsOld > 0) {
                FILETIME now{};
                GetSystemTimeAsFileTime(&now);
                ULARGE_INTEGER u{};
                u.LowPart = now.dwLowDateTime;
                u.HighPart = now.dwHighDateTime;
                u.QuadPart -= 365ull * 24 * 3600 * 10000000ull * yearsOld;
                FILETIME old{};
                old.dwLowDateTime = u.LowPart;
                old.dwHighDateTime = u.HighPart;
                SetFileTime(f, nullptr, nullptr, &old);
            }
            CloseHandle(f);
        };
        auto cleanup = [&]() {
            for (const wchar_t* n :
                 {L"StartPreset.4.start", L"Custom.4.1.binds",
                  L"Custom.4.2.binds"}) {
                wchar_t path[MAX_PATH];
                swprintf_s(path, L"%s\\%s", dir, n);
                DeleteFileW(path);
            }
            RemoveDirectoryW(dir);
        };
        writeFile(L"StartPreset.4.start", "Custom\nCustom\n", 0);
        // The stale relic: on-foot camera F11, next-view RightArrow.
        writeFile(L"Custom.4.1.binds",
                  "<Root>\n"
                  "<PhotoCameraToggle><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_F11\" /></PhotoCameraToggle>\n"
                  "<PhotoCameraToggle_Humanoid><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_F11\" /></PhotoCameraToggle_Humanoid>\n"
                  "<VanityCameraScrollRight><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_RightArrow\" /></VanityCameraScrollRight>\n"
                  "</Root>\n",
                  1);
        // The maintained file, after the field rebind: on-foot camera moved
        // to backslash, next-view to right-bracket; the SHIP camera stays
        // F11, exactly as on the reporting rig.
        writeFile(L"Custom.4.2.binds",
                  "<Root>\n"
                  "<PhotoCameraToggle><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_F11\" /></PhotoCameraToggle>\n"
                  "<PhotoCameraToggle_Humanoid><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_BackSlash\" /></PhotoCameraToggle_Humanoid>\n"
                  "<VanityCameraScrollRight><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_RightBracket\" /></VanityCameraScrollRight>\n"
                  "</Root>\n",
                  0);
        char v[48] = {};
        if (!edvr::eliteBindsLookupDir(dir, "VanityCameraScrollRight", v,
                                       sizeof(v)) ||
            edvr::virtualKeyFromName(v, &m) !=
                edvr::virtualKeyFromName("RIGHTBRACKET", &m)) {
            printf("  FAIL  a stale previous-format preset file outranked "
                   "the maintained one (got %s)\n", v);
            ++bad;
        }
        if (!edvr::eliteBindsLookupDir(dir, "PhotoCameraToggle_Humanoid", v,
                                       sizeof(v), "PhotoCameraToggle") ||
            edvr::virtualKeyFromName(v, &m) !=
                edvr::virtualKeyFromName("BACKSLASH", &m)) {
            printf("  FAIL  the on-foot camera element did not answer with "
                   "the maintained file's key (got %s)\n", v);
            ++bad;
        }
        // A Humanoid entry moved OFF the keyboard must refuse rather than
        // fall through to the ship element's keyboard key -- that key does
        // nothing on foot. The GamePad primary sits right before the ship
        // element so a span that bleeds across elements is caught too.
        writeFile(L"Custom.4.2.binds",
                  "<Root>\n"
                  "<PhotoCameraToggle_Humanoid><Primary Device=\"GamePad\" "
                  "Key=\"GamePad_DPadRight\" /></PhotoCameraToggle_Humanoid>\n"
                  "<PhotoCameraToggle><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_F11\" /></PhotoCameraToggle>\n"
                  "</Root>\n",
                  0);
        if (edvr::eliteBindsLookupDir(dir, "PhotoCameraToggle_Humanoid", v,
                                      sizeof(v), "PhotoCameraToggle")) {
            printf("  FAIL  an on-foot camera on a controller fell through "
                   "to the ship element's keyboard key (%s)\n", v);
            ++bad;
        }
        // Names Elite writes that used to be reported as "not on a keyboard
        // key" (issue #8): a HOTAS user with a keyboard secondary was told
        // there was no keyboard binding at all.
        if (!edvr::eliteBindsTranslateKey("Key_PrintScreen", t, sizeof(t)) ||
            edvr::virtualKeyFromName(t, &m) != VK_SNAPSHOT) {
            printf("  FAIL  Key_PrintScreen did not translate\n");
            ++bad;
        }
        if (!edvr::eliteBindsTranslateKey("Key_Numpad_Enter", t, sizeof(t)) ||
            edvr::virtualKeyFromName(t, &m) != VK_RETURN) {
            printf("  FAIL  Key_Numpad_Enter did not translate to ENTER\n");
            ++bad;
        }
        if (!edvr::eliteBindsTranslateKey("Key_Oem102", t, sizeof(t)) ||
            edvr::virtualKeyFromName(t, &m) != 0xE2) {
            printf("  FAIL  Key_Oem102 did not translate to a raw VK\n");
            ++bad;
        }

        // With the Humanoid element entirely ABSENT (older formats), the
        // fallback element is the right answer.
        writeFile(L"Custom.4.2.binds",
                  "<Root>\n"
                  "<PhotoCameraToggle><Secondary Device=\"Keyboard\" "
                  "Key=\"Key_F11\" /></PhotoCameraToggle>\n"
                  "</Root>\n",
                  0);
        if (!edvr::eliteBindsLookupDir(dir, "PhotoCameraToggle_Humanoid", v,
                                       sizeof(v), "PhotoCameraToggle") ||
            edvr::virtualKeyFromName(v, &m) != VK_F11) {
            printf("  FAIL  an absent on-foot element did not fall back to "
                   "the ship element\n");
            ++bad;
        }
        cleanup();
    }

    // THE FOCUS SPLIT (2026-08-16). EDVR's own keys are filtered by which
    // window has focus; bindings mirroring the game's own keys are not,
    // because Elite acts on them unfocused and a swallowed press inverts a
    // toggle for the rest of the session.
    {
        edvr::Hotkey mine;
        mine.setBinding("F11");
        if (mine.pressedWith(true, 0, /*focused=*/false)) {
            printf("  FAIL  an EDVR-owned key fired while unfocused\n");
            ++bad;
        }
        if (!mine.takeMissedWhileUnfocused()) {
            printf("  FAIL  the discarded press was not recorded for report\n");
            ++bad;
        }
        edvr::Hotkey theirs;
        theirs.setBinding("F11");
        theirs.setGameMirrored(true);
        if (!theirs.pressed() && false) { /* pressed() reads real hardware */ }
        // pressedWith is the testable seam; a mirrored binding is handed
        // focused=true by pressed(), so assert the seam accepts it and that
        // the flag itself is what distinguishes the two.
        if (!theirs.gameMirrored() || mine.gameMirrored()) {
            printf("  FAIL  gameMirrored did not distinguish the two kinds\n");
            ++bad;
        }
        if (!theirs.pressedWith(true, 0, /*focused=*/true)) {
            printf("  FAIL  a game-mirrored key did not fire\n");
            ++bad;
        }
        if (theirs.takeMissedWhileUnfocused()) {
            printf("  FAIL  a game-mirrored key recorded a focus miss\n");
            ++bad;
        }
    }

    // setBinding must carry BOTH halves. This is the regression that matters:
    // setKey(virtualKeyFromName(s)) compiles and drops the modifiers.
    edvr::Hotkey k;
    k.setBinding("CTRL+ALT+SPACE");
    if (k.key() != VK_SPACE || k.mods() != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  setBinding dropped the modifiers\n");
        ++bad;
    }
    // ...and setting a plain key afterwards must clear them, or the chord's
    // modifiers would linger on a binding that no longer wants any.
    k.setKey(VK_F9);
    if (k.mods() != 0) {
        printf("  FAIL  setKey left stale modifiers behind\n");
        ++bad;
    }

    if (bad == 0) printf("  ok    hotkey bindings parse, including combinations\n");
    return bad;
}

// The heartbeat, which is the part that decides whether a player's viewpoint
// stays moved after the gate stops running.
//
// Written as a test because the failure is invisible in every log: a frozen
// gate publishes nothing, so nothing says the flag went stale, and the symptom
// is "the offset is applied in the cockpit" several minutes later.
int liveFlagChecks() {
    int bad = 0;
    const uint32_t kMaxAge = 5;

    // FIRST, before anything in this process has published: the d3d11-absent
    // case, where openvr_api.dll is installed and its partner is not. There is
    // nobody to ask, which is not the same as being told no, and guessing yes
    // would apply the offset in every mode with no gate at all.
    //
    // This assertion is why liveFlagChecks runs before frameFlagChecks: once
    // anything has called setExternalCameraOnFoot, the "never published" state
    // is unreachable for the rest of the process and the case goes untested.
    if (edvr::externalCameraOnFootLive(kMaxAge)) {
        printf("  FAIL  the live flag read true with nothing ever published\n");
        ++bad;
    }

    // A writer that keeps publishing keeps it live, including across a run of
    // frames longer than the staleness window -- the heartbeat is the WRITES,
    // not the changes.
    bool heldLive = true;
    for (uint32_t i = 0; i < kMaxAge * 4; ++i) {
        edvr::setExternalCameraOnFoot(true);
        if (!edvr::externalCameraOnFootLive(kMaxAge)) heldLive = false;
    }
    if (!heldLive) {
        printf("  FAIL  an actively refreshed flag went stale while being written\n");
        ++bad;
    }

    // Now the writer stops. The raw flag still says yes -- that is the whole
    // hazard -- and the live one must give up within the window.
    uint32_t agedOutAt = 0;
    for (uint32_t i = 1; i <= kMaxAge * 3; ++i) {
        if (!edvr::externalCameraOnFootLive(kMaxAge)) { agedOutAt = i; break; }
    }
    if (!edvr::externalCameraOnFoot()) {
        printf("  FAIL  the raw flag did not stay set -- the test is not testing "
               "the stuck case\n");
        ++bad;
    }
    if (agedOutAt == 0) {
        printf("  FAIL  a frozen gate never aged out; the offset would stay "
               "applied for the session\n");
        ++bad;
    } else if (agedOutAt <= kMaxAge) {
        printf("  FAIL  aged out after %u frames, inside the %u-frame window\n",
               agedOutAt, kMaxAge);
        ++bad;
    }

    // And it recovers: a gate that starts publishing again is believed again,
    // so a single fault-and-recover does not disable the feature for good.
    edvr::setExternalCameraOnFoot(true);
    if (!edvr::externalCameraOnFootLive(kMaxAge)) {
        printf("  FAIL  a gate that resumed publishing was not believed again\n");
        ++bad;
    }

    // A live NO is still a no. The stamp moving must not be mistaken for the
    // answer being yes.
    edvr::setExternalCameraOnFoot(false);
    if (edvr::externalCameraOnFootLive(kMaxAge)) {
        printf("  FAIL  a refreshed 'off' read as on\n");
        ++bad;
    }
    edvr::setExternalCameraOnFoot(false);

    if (bad == 0)
        printf("  ok    the mode flag ages out when the gate stops publishing\n");
    return bad;
}

int sentinelChecks() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, _TRUNCATE, L"%sedvr_sentinel_test_%lu", base, GetCurrentProcessId());
    wchar_t file[MAX_PATH];
    _snwprintf_s(file, _TRUNCATE, L"%s\\probe.armed", dir);

    RemoveDirectoryW(dir);   // from a previous run, if any
    int rc = 0;

    {
        // The directory does NOT exist yet. This is the log.enabled = 0 case.
        edvr::Sentinel s(dir, L"probe");
        if (s.trippedOnStartup()) {
            printf("  FAIL  a fresh sentinel reports a trip with no file present\n");
            rc = 1;
        }
        if (!s.arm()) {
            printf("  FAIL  arm() failed with no log directory -- it must create it\n");
            rc = 1;
        } else if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  arm() reported success but wrote no file\n");
            rc = 1;
        }
    }
    {
        // A new sentinel over the same path is the next launch.
        edvr::Sentinel s(dir, L"probe");
        if (!s.trippedOnStartup()) {
            printf("  FAIL  the armed file did not trip the next sentinel\n");
            rc = 1;
        }
        s.clearTrip();
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  clearTrip() left the file in place -- a false trip would\n");
            printf("        then refuse every launch forever\n");
            rc = 1;
        }
        if (s.trippedOnStartup()) {
            printf("  FAIL  clearTrip() left the in-memory trip set, so a second request\n");
            printf("        this session would install after being refused\n");
            rc = 1;
        }
    }
    {
        // The normal success path: armed, then confirmed.
        edvr::Sentinel s(dir, L"probe");
        s.arm();
        s.confirm();
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  confirm() did not remove the armed file\n");
            rc = 1;
        }
    }
    {
        // Somewhere it cannot possibly write. arm() must say so rather than
        // claiming protection it does not have.
        edvr::Sentinel s(L"\\\\?\\Z:\\nonexistent-volume\\edvr", L"probe");
        if (s.arm()) {
            printf("  FAIL  arm() returned true for a path it cannot write\n");
            rc = 1;
        }
    }

    RemoveDirectoryW(dir);
    if (rc == 0) printf("  ok    crash sentinel arms, trips, clears and confirms\n");
    return rc;
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[2], "--fault-child") == 0) return faultChild(argv[1]);
    if (argc >= 3 && strcmp(argv[2], "--guard-child") == 0) return guardChild(argv[1]);

    printf("edvr openvr smoke\n");
    if (argc < 2) {
        printf("usage: openvr_smoke.exe <dir with openvr_api.dll + openvr_api_orig.dll>\n");
        return 2;
    }
    _snwprintf_s(g_proxyPath, _TRUNCATE, L"%hs\\openvr_api.dll", argv[1]);
    printf("proxy: %ls\n\n", g_proxyPath);

    // 1. The load must not deadlock.
    HANDLE t = CreateThread(nullptr, 0, loadProxy, nullptr, 0, nullptr);
    if (!t) return fail("could not create the loader thread");
    const DWORD waited = WaitForSingleObject(t, 15000);
    if (waited == WAIT_TIMEOUT) {
        printf("  FAIL  loading the proxy did not finish in 15 seconds\n");
        printf("        Something in the load path is blocking under the loader lock.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;   // the thread is stuck; process exit takes it with us
    }
    CloseHandle(t);
    if (!g_loaded) return fail("the proxy did not load at all");
    printf("  ok    proxy loaded without deadlocking\n");

    // 2. ...and it did so without pulling in the real module.
    if (GetModuleHandleW(L"openvr_api_orig.dll")) {
        printf("  FAIL  openvr_api_orig.dll was mapped by DllMain\n");
        printf("        Deferral is not happening; assertion 1 passed by luck.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    printf("  ok    real module not loaded yet -- deferred as intended\n");

    // 3. The first export call resolves it and forwards correctly.
    PFN_Probe probe =
        reinterpret_cast<PFN_Probe>(GetProcAddress(g_loaded, "VR_IsHmdPresent"));
    if (!probe) return fail("VR_IsHmdPresent is not exported by the proxy");

    const unsigned long long got = probe(7ull, 6.0, 5ull, 4.0, 3ull, 2ull);
    const unsigned long long want = 7ull + 60ull + 500ull + 4000ull + 30000ull + 200000ull;
    if (got != want) {
        printf("  FAIL  forwarded call returned %llu, expected %llu\n", got, want);
        printf("        An argument was corrupted across the lazy-load path.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    printf("  ok    first call resolved the table and passed all six arguments\n");

    if (!GetModuleHandleW(L"openvr_api_orig.dll")) {
        return fail("the real module still is not loaded after a forwarded call");
    }
    printf("  ok    real module loaded on demand\n");

    // 5. A real module whose DllMain faults degrades instead of killing us.
    {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t cmd[MAX_PATH * 2];
        _snwprintf_s(cmd, _TRUNCATE, L"\"%s\" \"%hs\" --fault-child", self, argv[1]);

        SetEnvironmentVariableW(L"EDVR_FAKEVR_FAULT", L"1");
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr,
                                       nullptr, &si, &pi);
        SetEnvironmentVariableW(L"EDVR_FAKEVR_FAULT", nullptr);
        if (!ok) return fail("could not start the fault child");

        WaitForSingleObject(pi.hProcess, 20000);
        DWORD code = 0xFFFFFFFF;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (code != 0) {
            printf("  FAIL  a faulting real-module DllMain did not degrade cleanly "
                   "(child exit 0x%08lX)\n", code);
            printf("        Expected the export to return the stub's zero and the\n");
            printf("        process to survive. It did not return at all.\n");
            printf("\nOPENVR SMOKE FAILED\n");
            return 1;
        }
        printf("  ok    a faulting real module degrades to stubs, process survives\n");
    }

    // After the fault child, which must see a process where the interface was
    // never requested; this one requests it and drives the observation hook.
    if (systemHookChecks(argv[1]) != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }

    // The cull guard, in a child with the guard armed -- this parent's own
    // proxy already installed with the guard off and cannot honestly re-run
    // the install. The child directory is built fresh here: the same two
    // DLLs, plus an edvr.ini that arms symmetric mode.
    {
        char dir2[MAX_PATH * 2];
        snprintf(dir2, sizeof(dir2), "%s2", argv[1]);
        CreateDirectoryA(dir2, nullptr);
        char src[MAX_PATH * 2], dst[MAX_PATH * 2];
        snprintf(src, sizeof(src), "%s\\openvr_api.dll", argv[1]);
        snprintf(dst, sizeof(dst), "%s\\openvr_api.dll", dir2);
        if (!CopyFileA(src, dst, FALSE)) return fail("could not stage the guard child's proxy");
        snprintf(src, sizeof(src), "%s\\openvr_api_orig.dll", argv[1]);
        snprintf(dst, sizeof(dst), "%s\\openvr_api_orig.dll", dir2);
        if (!CopyFileA(src, dst, FALSE)) return fail("could not stage the guard child's stand-in");
        snprintf(dst, sizeof(dst), "%s\\edvr.ini", dir2);
        {
            FILE* f = nullptr;
            if (fopen_s(&f, dst, "w") != 0 || !f) {
                return fail("could not write the guard child's edvr.ini");
            }
            // fraction_v = 0.5 exercises the partial-margin math end to end
            // against the fixture's asymmetric vertical, and the headset
            // gate names the fake's signature (l/r 51.3+36.9 = 88, t/b
            // 50.2+38.7 = 89) so the gate's match path runs too -- a broken
            // fovSignature keeps the child at truth and fails every
            // post-live assertion.
            fputs("[fix]\ncull_guard = symmetric\ncull_guard_fraction_v = 0.5\n"
                  "cull_guard_headsets = 88x89\n",
                  f);
            fclose(f);
        }
        // A crashed previous child leaves its sentinel armed; the install
        // must run every time in a test.
        wchar_t armed[MAX_PATH];
        _snwprintf_s(armed, _TRUNCATE, L"%hs\\edvr_logs\\system_hook.armed", dir2);
        DeleteFileW(armed);

        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t cmd[MAX_PATH * 2];
        _snwprintf_s(cmd, _TRUNCATE, L"\"%s\" \"%hs\" --guard-child", self, dir2);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr,
                            nullptr, &si, &pi)) {
            return fail("could not start the guard child");
        }
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD code = 0xFFFFFFFF;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code != 0) {
            printf("  FAIL  the guard child exited 0x%08lX -- the cull guard "
                   "lied wrong, cropped wrong, or crashed (its FAIL lines are "
                   "above)\n", code);
            printf("\nOPENVR SMOKE FAILED\n");
            return 1;
        }
        printf("  ok    cull guard: true first, symmetric lie after go-live, "
               "matrix rebuilt, crop correct, strangers untouched\n");
    }

    // Shared code with no other coverage. Runs last because it touches nothing
    // the assertions above depend on.
    //
    // liveFlagChecks BEFORE frameFlagChecks, and the order is load-bearing: its
    // first assertion is the "d3d11 never published" case, which stops existing
    // the moment anything calls setExternalCameraOnFoot.
    if (hotkeyMatchChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (hotkeyEdgeChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (hotkeyChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (liveFlagChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (frameFlagChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (submitPairChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (resubmitChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (guardCropChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (sentinelChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }

    printf("\nOPENVR SMOKE PASSED\n");
    return 0;
}
