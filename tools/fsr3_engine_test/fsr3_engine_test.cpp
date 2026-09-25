// A WARP rig for AMD's FSR 3.1 engine (src\d3d11\fsr3_engine.cpp), Track
// C's own desk test (docs\fsr-upscaler-design-2026-09-16.md, section 3.5,
// Phase 0): settles the jitter and motion-vector sign conventions AMD's
// port expects before any flight spends a headset session on a guess.
//
// Modeled on tools\openxr_shared_texture_test\openxr_shared_texture_test.cpp
// for the WARP device (the debug-layer-missing fallback) and the
// --dry-run/--self-test/watchdog shape. The registration table (d, below)
// began as tools\smoke\smoke.cpp's NGX conventions rig (~line 1160: a field
// of soft stripes at infinity, panned by a camera that yaws a degree a
// frame, held against the unjittered truth) carried over to AMD's port, and
// grew a scene of its own when that one turned out not to be able to settle
// a motion sign at all: its finest component has a period of 3.5 px and the
// camera moves the content about 3.5 px a frame, so a wrong sign lands
// nearly a whole period out and reads as nearly right. It survives as (d1).
//
// (a) fsr3Available on WARP.
// (b) two eye contexts create and dispatch FFX_OK at a real per-eye size
//     (2064x2208 -> 2064x2208), and AMD's own ENABLE_DEBUG_CHECKING
//     (advanced.temporal_aa_diagnostics=1) says nothing about this rig's
//     well-formed input (fsr3TestMessageCount, test-only -- deliberately
//     absent from fsr3_engine.h; forward-declared here instead).
// (c) a static (camera-still, jitter-still-cycling) synthetic frame
//     converges within tolerance.
// (d0) the precondition (d) rests on: the motion-vector texture reaches
//     AMD's port at all. The same static scene twice, once with a zero
//     motion field and once with a uniform +40 px in x; the outputs must
//     differ. They did not until fsr3_engine.cpp began handing the port
//     the three caller-owned working surfaces FSR 3.1's dispatch expects
//     (dilatedDepth, dilatedMotionVectors, reconstructedPrevNearest-
//     Depth): a null one registers as the port's NULL resource, so the
//     dilate pass wrote its motion vectors nowhere, the reprojection read
//     zero back, and every sign in (d) tied to the last byte.
// (d) the registration table: jitter sign {as_is, flip_x, flip_y,
//     flip_both} x motion-vector sign {as_is, flip_x, flip_y, flip_both},
//     all sixteen, over a scene translating a whole number of pixels a
//     frame so the true motion vector is known exactly everywhere. The
//     shipped default ("as_is, as_is" -- fsr3_engine.cpp's
//     kFsrJitterScaleX/Y and kFsrMotionVectorScaleX/Y are all 1 as this is
//     written) must be the least-error combination and the only one inside
//     twice the scene's own at-rest floor; if not, those constants want
//     the winning combination and this rig wants re-running.
// (d1) the same signs on a motion field EDVR's own producer made
//     (temporal_math.h's temporalReproject over a yawing camera), rather
//     than a constant this rig wrote by hand.
// (e) reset=true recovers from a hard scene cut markedly better than
//     reset=false carrying the old history forward.
// (f) a size change (1711x1425 -> 3422x3394) recreates the context
//     cleanly.
// (g) WARP's answer to IDXGIAdapter3::QueryVideoMemoryInfo, called
//     directly against this rig's device and reported, not asserted. The
//     engine's create line no longer uses that query (flights 1 to 3 of
//     2026-09-17 showed a create-time delta is noise); it computes the
//     bytes of its own three working surfaces instead.
// WIN32_LEAN_AND_MEAN/NOMINMAX come from the rig's own cl.exe line
// (build.bat's :rig_fsr3_engine_test), not a source-level #define here --
// fsr3_engine.cpp, compiled alongside this file in the same invocation,
// has no such #define of its own and needs them from the command line
// (gpu_timing_test.cpp and native_frame_test.cpp's own convention; only
// openxr_shared_texture_test.cpp uses the source-level form).
#include "../../src/d3d11/fsr3_engine.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/config.h"
#include "../../src/common/temporal_math.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// Test-only, NOT part of fsr3_engine.h's contract (its own comment there
// says so): how many fpMessage lines this session has relayed to the log,
// and the switch that turns fsr3Evaluate's bind-flag front stop off so the
// catch behind it can be proved to work.
namespace edvr {
uint32_t fsr3TestMessageCount();
void fsr3TestSkipBindCheck(bool on);
}

// perf_monitor.cpp is deliberately not linked here -- it pulls in
// device_hook.cpp and the rest of the live hooking machinery, which a desk
// rig has no business starting. fsr3_engine.cpp calls exactly this one
// entry point from perf_monitor.h; a stub is all a WARP rig needs.
namespace edvr { void perfMonitorNoteEvent(uint32_t bits, double ms) { (void)bits; (void)ms; } }

namespace {

unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", why);
        std::fflush(stdout);
    }
}

// The registration table's frame: tools\smoke\smoke.cpp's own size (400 x
// 304), so the pattern's periods below (chosen there for that size) stay
// meaningful. kTan is a plausible symmetric-ish VR eye frustum (l,r,t,b);
// its exact numbers do not matter, only that the same array renders the
// truth and computes the motion field.
constexpr UINT kRegW = 400, kRegH = 304;
constexpr float kThetaDeg = 1.0f;   // the synthetic camera's yaw, per frame
const float kTan[4] = {-1.0f, 1.0f, 0.76f, -0.76f};
const float kFovY = atanf(kTan[2]) + atanf(-kTan[3]);
constexpr UINT kErrX = 100, kErrY = 77, kErrW = 200, kErrH = 150;  // well inside kRegW x kRegH
constexpr int kRegFrames = 8;         // one full period of temporalJitter's own 8-frame sequence
constexpr double kAtRestTolerance = 20.0;  // 1/255 units, mean abs error over kErrW x kErrH

// System32's own d3d11, never EDVR's proxy (common/system_d3d11.h); the
// debug layer is tried first and dropped only when the machine lacks it,
// exactly as tools\openxr_shared_texture_test\openxr_shared_texture_test.cpp's
// own createDevice does. Unlike that rig, this one requests Feature Level
// 11_1 explicitly (11_0 as a fallback): passing a null feature-level array
// to D3D11CreateDevice never negotiates above 11_0, even on a driver that
// supports 11_1, and FSR3's LUMA_PYRAMID pass needs 11_1's compute-UAV-slot
// extension (its compiled shader declares "requires additional
// functionality: 64 UAV slots"; base D3D11 caps a compute shader at 8).
// Confirmed on this machine: an unconstrained request lands WARP at 11_0
// and FFX_ERROR_BACKEND_API_ERROR on that one pass every time; asking for
// 11_1 lands WARP at 11_1 and the identical shader links cleanly.
bool createWarpDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    const auto create = edvr::systemD3D11CreateDevice();
    if (!create) {
        std::printf("FAIL: system d3d11.dll\n");
        return false;
    }
    const D3D_FEATURE_LEVEL wantLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, wantLevels,
                        2, D3D11_SDK_VERSION, &device, &level, &context);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        device.Reset();
        context.Reset();
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, wantLevels, 2, D3D11_SDK_VERSION,
                    &device, &level, &context);
    }
    if (hr == S_OK && level < D3D_FEATURE_LEVEL_11_1) {
        std::printf("WARN: WARP device is feature level 0x%X, not 11_1 -- FSR3's LUMA_PYRAMID\n"
                    "      pass needs 11_1's compute-shader 64-UAV-slot extension and will fail\n"
                    "      FFX_ERROR_BACKEND_API_ERROR at context create.\n",
                    static_cast<unsigned>(level));
    }
    return hr == S_OK;
}

ID3D11Texture2D* makeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    ID3D11Texture2D* tex = nullptr;
    dev->CreateTexture2D(&td, nullptr, &tex);
    return tex;
}

// Read back a region of an R8G8B8A8_UNORM texture: w x h pixels, row-major,
// four bytes a pixel (tools\smoke\smoke.cpp's readRegionRgba8, unchanged).
bool readRegion(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT x, UINT y,
                UINT w, UINT h, std::vector<unsigned char>& out) {
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage)) || !stage) return false;
    D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    bool ok = false;
    if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        out.resize(static_cast<size_t>(w) * h * 4);
        for (UINT r = 0; r < h; ++r) {
            memcpy(&out[static_cast<size_t>(r) * w * 4],
                   static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(r) * m.RowPitch,
                   static_cast<size_t>(w) * 4);
        }
        ok = true;
        ctx->Unmap(stage, 0);
    }
    stage->Release();
    return ok;
}

double regionError(const std::vector<unsigned char>& out, const std::vector<unsigned char>& ref,
                   UINT fullW, UINT rx, UINT ry, UINT rw, UINT rh) {
    double sum = 0.0;
    for (UINT y = 0; y < rh; ++y) {
        for (UINT x = 0; x < rw; ++x) {
            const int o = out[(static_cast<size_t>(y) * rw + x) * 4];
            const int e = ref[(static_cast<size_t>(y + ry) * fullW + x + rx) * 4];
            sum += o > e ? o - e : e - o;
        }
    }
    return sum / (static_cast<double>(rw) * rh);
}

void fillConstant(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT w, UINT h, unsigned char v) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = v; px[i + 1] = v; px[i + 2] = v; px[i + 3] = 255; }
    ctx->UpdateSubresource(tex, 0, nullptr, px.data(), w * 4, 0);
}
void fillDepthConstant(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT w, UINT h, float v) {
    std::vector<float> px(static_cast<size_t>(w) * h, v);
    ctx->UpdateSubresource(tex, 0, nullptr, px.data(), w * 4, 0);
}
void fillZeroMotion(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT w, UINT h) {
    // A half-float zero is the all-zero bit pattern, so a zeroed buffer
    // needs no conversion.
    std::vector<uint16_t> px(static_cast<size_t>(w) * h * 2, 0);
    ctx->UpdateSubresource(tex, 0, nullptr, px.data(), w * 4, 0);
}

// dlaa.cpp's own probeHalf (its desk probes' motion-vector packer),
// unchanged: truncating float32 -> float16, tiny flushed to zero, overflow
// saturated to infinity. Adequate for motion vectors of a few pixels.
uint16_t probeHalf(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((u >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = u & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// A field of soft stripes at infinity (tools\smoke\smoke.cpp, unchanged):
// exact sub-pixel truth at any (u, v), so a registered history converges
// to it and a misregistered one cannot.
float pattern(float u, float v) {
    const float k = 200.0f;
    const float a = sinf(6.2831853f * u * k / 3.5f);
    const float b = sinf(6.2831853f * (0.6f * u + 0.8f * v) * k / 5.0f);
    const float c = sinf(6.2831853f * (-0.7f * u + 0.7f * v) * k / 4.2f);
    const float f = 0.5f + 0.16f * (a + b + c);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

// Frame k's rendered (jittered) image: a camera yawed k * kThetaDeg from
// frame 0's, seen through the jitter (jx, jy) exactly as dlaa.h's own
// convention states it (content sits jx right, jy down of the unjittered
// projection) -- temporal_math.h's temporalPixelToDir/temporalApply3
// transcribe the same chain the shader does.
void renderTruth(int k, float jx, float jy, UINT w, UINT h, const float tan[4],
                 std::vector<unsigned char>& img) {
    const float th = static_cast<float>(k) * kThetaDeg * 3.14159265f / 180.0f;
    const float ck = cosf(th), sk = sinf(th);
    const float R[9] = {ck, 0, sk, 0, 1, 0, -sk, 0, ck};
    img.resize(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            float d[3], wd[3];
            edvr::temporalPixelToDir(static_cast<float>(x) - jx, static_cast<float>(y) - jy, tan, w, h, d);
            edvr::temporalApply3(R, d, wd);
            const float u = wd[0] / -wd[2];
            const float v = wd[1] / -wd[2];
            const unsigned char g = static_cast<unsigned char>(pattern(u, v) * 255.0f + 0.5f);
            unsigned char* px = &img[(static_cast<size_t>(y) * w + x) * 4];
            px[0] = g; px[1] = g; px[2] = g; px[3] = 255;
        }
    }
}

// The per-pixel motion vector, current -> previous, in pixels (dlaa.h's own
// convention, which fsr3Evaluate's contract says this rig's caller-side
// inputs share). The camera yaws by a FIXED kThetaDeg every frame, so the
// delta from now's view into last frame's is the same rotation regardless
// of k (R_prev^T R_now = Ry(theta) always); temporalReproject composes the
// pixel -> direction -> rotate -> direction -> pixel chain the shader
// transcribes. A pixel it refuses (off-frustum) gets zero motion, away
// from the region the error is measured over.
void renderMotionField(UINT w, UINT h, const float tan[4], std::vector<float>& mv) {
    const float th = kThetaDeg * 3.14159265f / 180.0f;
    const float ct = cosf(th), st = sinf(th);
    const float delta[9] = {ct, 0, st, 0, 1, 0, -st, 0, ct};
    mv.assign(static_cast<size_t>(w) * h * 2, 0.0f);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            float ppx = 0.0f, ppy = 0.0f;
            if (edvr::temporalReproject(static_cast<float>(x), static_cast<float>(y), tan, tan, delta, w,
                                        h, &ppx, &ppy)) {
                mv[(static_cast<size_t>(y) * w + x) * 2 + 0] = ppx - static_cast<float>(x);
                mv[(static_cast<size_t>(y) * w + x) * 2 + 1] = ppy - static_cast<float>(y);
            }
        }
    }
}

// The registration table's own scene, translated a whole number of pixels
// a frame so the exact motion vector is known at every pixel and no
// perspective, no frustum refusal and no interpolation enter into it. Two
// scales, because the table settles two conventions of very different size:
//
//   coarse  a 27 x 23 px chequer of sines, which a wrong MOTION sign lands
//           2 * kShiftPx = 6 px out of -- a fifth of a period, at the
//           steepest part of the gradient. The yaw scene above cannot do
//           this: its finest component has a period of 3.5 px and its
//           camera moves the content about 3.5 px a frame, so a wrong sign
//           lands nearly a whole period out and reads as nearly right.
//           Neither 27 nor 23 is a multiple of 6, so a wrong sign cannot
//           drift into alignment inside kRegFrames frames either.
//   ripple  3.5 and 4.2 px, near Nyquist, which is the only thing a wrong
//           JITTER sign can show up in: it misplaces a sample by at most
//           one pixel, and nothing coarse can price that.
//
// Both scales are BAND-LIMITED on purpose. A hard-edged version of this
// scene was written first and measured worse at the job: at render size ==
// upscale size AMD's resolve reconstructs the signal from its jittered
// samples and evaluates it at the output pixel centres, and a
// discontinuity's reconstruction residual is large for EVERY variant,
// which compressed the whole table towards its floor (the winner went from
// 1.4 to 9.7 against a floor of 0.9 to 7.5, and every margin shrank with
// it). Smooth and near-Nyquist is what a resolve can actually converge to,
// so what is left in the numbers is registration, which is what is being
// measured.
constexpr float kShiftPx = 3.0f;  // the content's own step, per frame

float scenePattern(float fx, float fy) {
    const float coarse = 0.40f * sinf(6.2831853f * fx / 27.0f) * sinf(6.2831853f * fy / 23.0f);
    const float ripple = 0.04f * sinf(6.2831853f * fx / 3.5f) +
                         0.04f * sinf(6.2831853f * fy / 4.2f);
    const float g = 0.5f + coarse + ripple;
    return g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
}

// Frame k of the translating scene, point-sampled at the jittered position
// exactly as a renderer samples it (dlaa.h's convention: the content sits
// jx right and jy down of the unjittered projection). The content moves
// +(sx, sy) px a frame, so the motion vector current -> previous is
// -(sx, sy) px at every pixel.
void renderScene(int k, float sx, float sy, float jx, float jy, UINT w, UINT h,
                std::vector<unsigned char>& img) {
    img.resize(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const float g = scenePattern(static_cast<float>(x) - jx - static_cast<float>(k) * sx,
                                        static_cast<float>(y) - jy - static_cast<float>(k) * sy);
            const unsigned char c = static_cast<unsigned char>(g * 255.0f + 0.5f);
            unsigned char* px = &img[(static_cast<size_t>(y) * w + x) * 4];
            px[0] = c; px[1] = c; px[2] = c; px[3] = 255;
        }
    }
}

// The truth this scene is held against is renderScene with no jitter at all,
// which is also what the yaw scene above does. It is a POINT sample, not an
// average over the pixel's footprint, because the inputs are point samples
// too: at render size == upscale size AMD's resolve reconstructs the signal
// its jittered samples came from and evaluates it at each output pixel's
// centre, so what it converges to is the unjittered point sample. Measured
// here, and it matters: an earlier version of this rig held the same scene
// against a 4x4 box average and read an at-rest floor (7.66) WORSE than
// every moving cell in the table, because a misregistered history blurs the
// output towards a box average and was being rewarded for it.

// Diagnostic only (not one of the lettered assertions): the D3D11 debug
// layer's own messages, which a bad shader blob or a bad resource create
// prints in far more detail than FFX's own FFX_ERROR_BACKEND_API_ERROR
// does. tools\openxr_shared_texture_test\openxr_shared_texture_test.cpp's
// own checkDebug, read out rather than asserted on.
void dumpDebugMessages(ID3D11Device* dev, const char* who) {
    ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&queue)))) {
        std::printf("info: %s: no D3D11 debug layer on this device\n", who);
        return;
    }
    const UINT64 n = queue->GetNumStoredMessages();
    std::printf("info: %s: %llu D3D11 debug message(s)\n", who, static_cast<unsigned long long>(n));
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size))) break;
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &size))) break;
        std::printf("info: %s [%d]: %s\n", who, static_cast<int>(message->Severity), message->pDescription);
    }
    queue->ClearStoredMessages();
}

// (b)
// The output texture carries BOTH bind flags, not only UNORDERED_ACCESS:
// AMD's DX11 port's RegisterResourceDX11 (ffx_dx11.cpp) unconditionally
// calls CreateShaderResourceView on every texture it registers, including
// the write-only upscaled-output target -- unlike its UAV path just below,
// which does check the resource's own bind flags first. A UAV-only output
// texture makes that call fail, and this port's response to ANY failed
// D3D11 call at dispatch time is TIF()'s bare `throw 1` (ffx_dx11.cpp
// ~line 228) -- an untyped C++ exception with no place in the otherwise
// all-FfxErrorCode C API, uncatchable by a caller with no reason to expect
// one, fatal (std::terminate) if nothing up the stack happens to catch it.
// Confirmed by isolating the throw site directly; fsr3_engine.h now carries
// this requirement for real callers (temporal_pass.cpp's own output target
// already satisfies it).
void testContextCreateAndSilence(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = 2064, h = 2208;
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out0   = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out1   = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && out0 && out1;
    check(made, "(b) the 2064x2208 textures were created");
    bool ok0 = false, ok1 = false;
    uint32_t before = 0, after = 0;
    if (made) {
        fillConstant(ctx, colour, w, h, 128);
        fillDepthConstant(ctx, depth, w, h, 0.3f);
        fillZeroMotion(ctx, mv, w, h);
        before = edvr::fsr3TestMessageCount();
        const char* why0 = nullptr;
        ok0 = edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out0, w, h, w, h, 0.0f, 0.0f, true,
                                 11.1f, 0.1f, 10000.0f, kFovY, &why0);
        if (!ok0 && why0) std::printf("info: (b) eye 0 refused: %s\n", why0);
        const char* why1 = nullptr;
        ok1 = edvr::fsr3Evaluate(ctx, 1, colour, depth, mv, nullptr, out1, w, h, w, h, 0.0f, 0.0f, true,
                                 11.1f, 0.1f, 10000.0f, kFovY, &why1);
        if (!ok1 && why1) std::printf("info: (b) eye 1 refused: %s\n", why1);
        after = edvr::fsr3TestMessageCount();
    }
    check(ok0, "(b) eye 0 dispatched FFX_OK at 2064x2208");
    check(ok1, "(b) eye 1 dispatched FFX_OK at 2064x2208");
    std::printf("info: (b) fpMessage count before=%u after=%u (advanced.temporal_aa_diagnostics on)\n",
                before, after);
    // `made` is in the test on purpose: with no textures, nothing dispatches,
    // before and after are both 0, and this printed as a clean pass while
    // measuring nothing at all (the delegated rig review, 2026-09-16).
    check(made && ok0 && ok1 && after == before,
          "(b) DEBUG_CHECKING stayed silent on two well-formed dispatches");
    if (colour) colour->Release();
    if (depth) depth->Release();
    if (mv) mv->Release();
    if (out0) out0->Release();
    if (out1) out1->Release();
}

// (c)
void testAtRest(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = kRegW, h = kRegH;
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out    = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && out;
    check(made, "(c) the at-rest textures were created");
    if (made) {
        fillDepthConstant(ctx, depth, w, h, 0.3f);
        fillZeroMotion(ctx, mv, w, h);
        std::vector<unsigned char> img, ref, readback;
        bool okAll = true;
        for (int k = 0; k < kRegFrames; ++k) {
            float jx = 0.0f, jy = 0.0f;
            edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
            renderTruth(0, jx, jy, w, h, kTan, img);   // world fixed at k=0: the camera does not move
            ctx->UpdateSubresource(colour, 0, nullptr, img.data(), w * 4, 0);
            const char* why = nullptr;
            const bool ok = edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, w, h, w, h, jx, jy,
                                               k == 0, 11.1f, 0.1f, 10000.0f, kFovY, &why);
            if (!ok) {
                okAll = false;
                std::printf("info: (c) frame %d refused: %s\n", k, why ? why : "?");
                break;
            }
        }
        check(okAll, "(c) every at-rest frame dispatched");
        if (okAll) {
            std::vector<unsigned char> readback2;
            if (readRegion(dev, ctx, out, kErrX, kErrY, kErrW, kErrH, readback2)) {
                renderTruth(0, 0.0f, 0.0f, w, h, kTan, ref);
                const double err = regionError(readback2, ref, w, kErrX, kErrY, kErrW, kErrH);
                std::printf("info: (c) at-rest mean abs error over the region = %.2f/255\n", err);
                check(err < kAtRestTolerance, "(c) at-rest output converged within tolerance");
            } else {
                check(false, "(c) could not read the at-rest output back");
            }
        }
    }
    if (colour) colour->Release();
    if (depth) depth->Release();
    if (mv) mv->Release();
    if (out) out->Release();
}

// probeHalf's inverse, for reading back what was actually uploaded: the
// table below is only as good as the bits the port sees, and a packer that
// flushed every motion vector to zero would tie every sign silently.
float halfToFloat(uint16_t hbits) {
    const uint32_t sign = static_cast<uint32_t>(hbits & 0x8000u) << 16;
    const int32_t exp = static_cast<int32_t>((hbits >> 10) & 0x1F);
    const uint32_t mant = static_cast<uint32_t>(hbits & 0x3FFu);
    uint32_t u;
    if (exp == 0) {
        u = sign;  // zero or subnormal, which probeHalf never emits
    } else if (exp == 31) {
        u = sign | 0x7F800000u | (mant << 13);
    } else {
        u = sign | (static_cast<uint32_t>(exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// The motion field's own statistics over the region the error is measured
// over, and the count of pixels temporalReproject refused (a refusal leaves
// zero motion, which would quietly weaken the table).
void printMotionStats(const char* who, UINT w, UINT h, const float tan[4], const float delta[9],
                      const std::vector<float>& mv) {
    double sx = 0.0, sy = 0.0;
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
    long refused = 0, n = 0;
    for (UINT y = kErrY; y < kErrY + kErrH; ++y) {
        for (UINT x = kErrX; x < kErrX + kErrW; ++x) {
            float ppx = 0.0f, ppy = 0.0f;
            if (tan && delta &&
                !edvr::temporalReproject(static_cast<float>(x), static_cast<float>(y), tan, tan,
                                         delta, w, h, &ppx, &ppy)) {
                ++refused;
            }
            const float vx = mv[(static_cast<size_t>(y) * w + x) * 2 + 0];
            const float vy = mv[(static_cast<size_t>(y) * w + x) * 2 + 1];
            sx += vx; sy += vy;
            if (vx < mnx) mnx = vx;
            if (vx > mxx) mxx = vx;
            if (vy < mny) mny = vy;
            if (vy > mxy) mxy = vy;
            ++n;
        }
    }
    const size_t mid = ((static_cast<size_t>(kErrY + kErrH / 2) * w) + kErrX + kErrW / 2) * 2;
    std::printf(
        "info: %s motion over the region: x mean %+.3f min %+.3f max %+.3f | y mean %+.3f min "
        "%+.3f max %+.3f | refused %ld/%ld px | centre px (%+.3f, %+.3f) -> half (%+.3f, %+.3f)\n",
        who, n ? sx / n : 0.0, mnx, mxx, n ? sy / n : 0.0, mny, mxy, refused, n, mv[mid],
        mv[mid + 1], halfToFloat(probeHalf(mv[mid])), halfToFloat(probeHalf(mv[mid + 1])));
}

// Runs one kRegFrames-long scene through the engine under one sign variant.
// `frames[k]` is frame k's rendered (jittered) colour; `mvHalf` the motion
// field, constant in every scene this rig uses and so uploaded once; `jsx`
// and `jsy` the jitter signs under test. When `truth` is given, the mean
// abs error of the last three frames against truth[k] is returned through
// `errOut`; when `readback` is given it receives the final frame's region.
// One loop, one refusal message: an error path that reported success on a
// dispatch that never ran would be worse than no rig at all.
bool runSequence(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* colour,
                 ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out, UINT w, UINT h,
                 const std::vector<std::vector<unsigned char>>& frames,
                 const std::vector<uint16_t>& mvHalf, float jsx, float jsy, const char* who,
                 const std::vector<std::vector<unsigned char>>* truth, double* errOut,
                 std::vector<unsigned char>* readback) {
    ctx->UpdateSubresource(mv, 0, nullptr, mvHalf.data(), w * 4, 0);
    std::vector<unsigned char> rb;
    double sum = 0.0;
    long n = 0;
    for (int k = 0; k < kRegFrames; ++k) {
        float jx = 0.0f, jy = 0.0f;
        edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
        ctx->UpdateSubresource(colour, 0, nullptr, frames[static_cast<size_t>(k)].data(), w * 4, 0);
        const char* why = nullptr;
        if (!edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, w, h, w, h, jsx * jx,
                                jsy * jy, k == 0, 11.1f, 0.1f, 10000.0f, kFovY, &why)) {
            std::printf("info: %s frame %d refused: %s\n", who, k, why ? why : "?");
            return false;
        }
        if (k < kRegFrames - 3) continue;
        if (!readRegion(dev, ctx, out, kErrX, kErrY, kErrW, kErrH, rb)) {
            std::printf("info: %s frame %d could not be read back\n", who, k);
            return false;
        }
        if (truth) {
            sum += regionError(rb, (*truth)[static_cast<size_t>(k)], w, kErrX, kErrY, kErrW, kErrH) *
                   (static_cast<double>(kErrW) * kErrH);
            n += static_cast<long>(kErrW) * kErrH;
        }
    }
    if (errOut) *errOut = n ? sum / n : 1e9;
    if (readback) *readback = rb;
    return true;
}

// (d0) The precondition every sign in (d) rests on: does the motion-vector
// texture EDVR registers as dd.motionVectors reach AMD's port at all? The
// same static scene is run twice, once with a zero motion field and once
// with a uniform +40 px in x. 40 px is far outside anything a still scene's
// history could legitimately be sampled at, so the port either samples the
// history 40 px away or rejects it -- either way the two outputs must
// differ. If they do not, the port is reading no motion at all, and every
// sign in the table below would be a tie measured over nothing.
void testMotionReaches(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = kRegW, h = kRegH;
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out    = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && out;
    check(made, "(d0) the motion-probe textures were created");
    if (made) {
        fillDepthConstant(ctx, depth, w, h, 0.3f);
        std::vector<std::vector<unsigned char>> frames(static_cast<size_t>(kRegFrames));
        for (int k = 0; k < kRegFrames; ++k) {
            float jx = 0.0f, jy = 0.0f;
            edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
            renderTruth(0, jx, jy, w, h, kTan, frames[static_cast<size_t>(k)]);
        }
        std::vector<uint16_t> zero(static_cast<size_t>(w) * h * 2, 0);
        std::vector<uint16_t> big(static_cast<size_t>(w) * h * 2, 0);
        for (size_t i = 0; i < big.size(); i += 2) big[i] = probeHalf(40.0f);
        std::vector<unsigned char> a, b;
        const bool ok = runSequence(dev, ctx, colour, depth, mv, out, w, h, frames, zero, 1.0f, 1.0f,
                                    "(d0) zero-motion", nullptr, nullptr, &a) &&
                        runSequence(dev, ctx, colour, depth, mv, out, w, h, frames, big, 1.0f, 1.0f,
                                    "(d0) 40px-motion", nullptr, nullptr, &b);
        check(ok, "(d0) both motion-probe sequences dispatched and read back");
        if (ok) {
            double sum = 0.0;
            int worst = 0;
            for (size_t i = 0; i < a.size(); i += 4) {
                const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
                const int ad = d < 0 ? -d : d;
                sum += ad;
                if (ad > worst) worst = ad;
            }
            const double mean = sum / (static_cast<double>(kErrW) * kErrH);
            std::printf(
                "info: (d0) zero motion vs a uniform +40 px in x, same static scene: mean abs "
                "difference %.2f/255, worst pixel %d/255\n",
                mean, worst);
            check(mean > 1.0,
                  "(d0) the motion-vector texture reaches AMD's port (a 40 px vector changes the "
                  "output)");
        }
    }
    if (colour) colour->Release();
    if (depth) depth->Release();
    if (mv) mv->Release();
    if (out) out->Release();
}

// (d) design doc 3.5 item 3, settled on the translating scene above:
// jitter sign {as_is, flip_x, flip_y, flip_both} x motion-vector sign
// {as_is, flip_x, flip_y, flip_both}, all sixteen, since nothing says the
// two motion axes must be wrong together. Each cell is the mean of two
// scenes, one translating in x and one in y: a scene that moves in x alone
// cannot see the y sign at all (its y motion is zero), and vice versa.
void testRegistrationTable(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    struct Variant { const char* name; float jsx, jsy, msx, msy; };
    static const Variant kVariants[16] = {
        {"jitter as_is,     motion as_is    ",  1.0f,  1.0f,  1.0f,  1.0f},
        {"jitter as_is,     motion flip_x   ",  1.0f,  1.0f, -1.0f,  1.0f},
        {"jitter as_is,     motion flip_y   ",  1.0f,  1.0f,  1.0f, -1.0f},
        {"jitter as_is,     motion flip_both",  1.0f,  1.0f, -1.0f, -1.0f},
        {"jitter flip_x,    motion as_is    ", -1.0f,  1.0f,  1.0f,  1.0f},
        {"jitter flip_x,    motion flip_x   ", -1.0f,  1.0f, -1.0f,  1.0f},
        {"jitter flip_x,    motion flip_y   ", -1.0f,  1.0f,  1.0f, -1.0f},
        {"jitter flip_x,    motion flip_both", -1.0f,  1.0f, -1.0f, -1.0f},
        {"jitter flip_y,    motion as_is    ",  1.0f, -1.0f,  1.0f,  1.0f},
        {"jitter flip_y,    motion flip_x   ",  1.0f, -1.0f, -1.0f,  1.0f},
        {"jitter flip_y,    motion flip_y   ",  1.0f, -1.0f,  1.0f, -1.0f},
        {"jitter flip_y,    motion flip_both",  1.0f, -1.0f, -1.0f, -1.0f},
        {"jitter flip_both, motion as_is    ", -1.0f, -1.0f,  1.0f,  1.0f},
        {"jitter flip_both, motion flip_x   ", -1.0f, -1.0f, -1.0f,  1.0f},
        {"jitter flip_both, motion flip_y   ", -1.0f, -1.0f,  1.0f, -1.0f},
        {"jitter flip_both, motion flip_both", -1.0f, -1.0f, -1.0f, -1.0f},
    };
    // The engine's own shipped constants, as the table indexes them: what
    // fsr3_engine.cpp's kFsrJitterScaleX/Y and kFsrMotionVectorScaleX/Y are
    // set to must be the winner, or those constants (and this index) want
    // changing together.
    const int kShipped = 0;

    const UINT w = kRegW, h = kRegH;
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out    = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && out;
    check(made, "(d) the registration-table textures were created");
    if (!made) {
        if (colour) colour->Release();
        if (depth) depth->Release();
        if (mv) mv->Release();
        if (out) out->Release();
        return;
    }
    fillDepthConstant(ctx, depth, w, h, 0.3f);

    struct Scene { const char* name; float sx, sy; };
    const Scene kScenes[2] = {{"x", kShiftPx, 0.0f}, {"y", 0.0f, kShiftPx}};
    std::vector<std::vector<unsigned char>> frames[2], truth[2], restFrames, restTruth;
    for (int si = 0; si < 2; ++si) {
        frames[si].resize(static_cast<size_t>(kRegFrames));
        truth[si].resize(static_cast<size_t>(kRegFrames));
        for (int k = 0; k < kRegFrames; ++k) {
            float jx = 0.0f, jy = 0.0f;
            edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
            renderScene(k, kScenes[si].sx, kScenes[si].sy, jx, jy, w, h,
                       frames[si][static_cast<size_t>(k)]);
            renderScene(k, kScenes[si].sx, kScenes[si].sy, 0.0f, 0.0f, w, h,
                       truth[si][static_cast<size_t>(k)]);
        }
    }
    // The floor this scene's own resolve reaches with nothing moving: the
    // same pattern, the same jitter sequence, a zero motion field. The
    // winner below is judged against THIS, not against the smooth yaw
    // scene's at-rest number -- a hard edge costs the resolve something
    // even when every sign is right.
    restFrames.resize(static_cast<size_t>(kRegFrames));
    restTruth.resize(static_cast<size_t>(kRegFrames));
    for (int k = 0; k < kRegFrames; ++k) {
        float jx = 0.0f, jy = 0.0f;
        edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
        renderScene(0, 0.0f, 0.0f, jx, jy, w, h, restFrames[static_cast<size_t>(k)]);
        renderScene(0, 0.0f, 0.0f, 0.0f, 0.0f, w, h, restTruth[static_cast<size_t>(k)]);
    }

    // The four jitter signs with NOTHING moving and a zero motion field.
    // This settles the jitter convention on its own, where no motion can
    // confound it, and the first of the four is this scene's floor: what
    // its resolve reaches when there is nothing to register. The table
    // below is judged against that number.
    std::vector<uint16_t> mvHalf(static_cast<size_t>(w) * h * 2, 0);
    // The four cells that carry the shipped motion signs, found by matching
    // on those signs rather than by index arithmetic, so that reordering
    // the table cannot point this at the wrong four.
    int restIdx[4] = {-1, -1, -1, -1};
    int nRest = 0;
    for (int vi = 0; vi < 16 && nRest < 4; ++vi) {
        if (kVariants[vi].msx == kVariants[kShipped].msx &&
            kVariants[vi].msy == kVariants[kShipped].msy) {
            restIdx[nRest++] = vi;
        }
    }
    check(nRest == 4, "(d) the table carries four jitter signs at the shipped motion signs");
    double rest[4] = {};
    double atRest = 0.0;
    bool rigOk = nRest == 4;
    for (int ji = 0; ji < nRest && rigOk; ++ji) {
        rigOk = runSequence(dev, ctx, colour, depth, mv, out, w, h, restFrames, mvHalf,
                            kVariants[restIdx[ji]].jsx, kVariants[restIdx[ji]].jsy,
                            "(d) scene at rest", &restTruth, &rest[ji], nullptr);
        if (rigOk && restIdx[ji] == kShipped) atRest = rest[ji];
    }
    check(rigOk, "(d) the scene's at-rest floor was measured under all four jitter signs");
    if (rigOk) {
        std::printf("info: (d) at rest (no motion at all): jitter");
        for (int ji = 0; ji < 4; ++ji) {
            std::printf(" %s%s %.2f", kVariants[restIdx[ji]].jsx < 0.0f ? "-x" : "+x",
                        kVariants[restIdx[ji]].jsy < 0.0f ? "-y" : "+y", rest[ji]);
        }
        std::printf("/255; the shipped signs (+x+y) are the floor the table is judged against\n");
        bool haveShipped = false, leastAtRest = true;
        for (int ji = 0; ji < 4; ++ji) {
            if (restIdx[ji] == kShipped) haveShipped = true;
            else if (rest[ji] <= atRest) leastAtRest = false;
        }
        leastAtRest = leastAtRest && haveShipped;
        // With nothing moving, the motion signs cannot matter and only the
        // jitter signs can: this settles that convention on its own, clear
        // of the table's coupling.
        check(leastAtRest,
              "(d) with nothing moving, the shipped jitter signs are the least-error ones");
    }

    double err[16] = {};
    for (int vi = 0; vi < 16 && rigOk; ++vi) {
        double both = 0.0;
        for (int si = 0; si < 2 && rigOk; ++si) {
            const float mvx = kVariants[vi].msx * -kScenes[si].sx;
            const float mvy = kVariants[vi].msy * -kScenes[si].sy;
            for (size_t i = 0; i < mvHalf.size(); i += 2) {
                mvHalf[i] = probeHalf(mvx);
                mvHalf[i + 1] = probeHalf(mvy);
            }
            double e = 0.0;
            if (!runSequence(dev, ctx, colour, depth, mv, out, w, h, frames[si], mvHalf,
                             kVariants[vi].jsx, kVariants[vi].jsy, kVariants[vi].name, &truth[si],
                             &e, nullptr)) {
                rigOk = false;
                break;
            }
            both += e;
        }
        err[vi] = both / 2.0;
    }
    check(rigOk, "(d) every registration-table dispatch and readback succeeded");
    if (rigOk) {
        int best = 0;
        for (int vi = 1; vi < 16; ++vi) if (err[vi] < err[best]) best = vi;
        int tied = 0;
        double runnerUp = 1e18;
        for (int vi = 0; vi < 16; ++vi) {
            if (err[vi] <= err[best]) ++tied;
            if (vi != best && err[vi] < runnerUp) runnerUp = err[vi];
        }
        for (int vi = 0; vi < 16; ++vi) {
            std::printf("info: (d) %s error %6.2f%s\n", kVariants[vi].name, err[vi],
                        vi == best ? "  <- least" : "");
        }
        std::printf(
            "info: (d) least %.2f, runner-up %.2f (%.2fx the least), at-rest floor %.2f, "
            "the band %.2f\n",
            err[best], runnerUp, err[best] > 0.0 ? runnerUp / err[best] : 0.0, atRest,
            2.0 * atRest);
        // A motion-vector texture that never reached AMD's port would tie
        // all four motion columns exactly, which is how this rig read
        // before fsr3_engine.cpp began handing the port its three
        // caller-owned working surfaces. That must fail here, not pass
        // quietly: a table of sixteen equal numbers has a minimum too.
        check(tied == 1, "(d) exactly one sign combination is the least-error one");
        // One threshold, twice the floor this same scene reaches with
        // nothing moving, has to separate the table into the one
        // combination that CONVERGED and the fifteen that did not. A ratio
        // between the best and the runner-up would be the wrong bar: the
        // two conventions are not the same size of error -- a motion sign
        // lands the history 2 * kShiftPx = 6 px out, a jitter sign
        // misplaces a sample by at most one -- so any single ratio would
        // be generous to one axis and fitted to the other. This is a
        // physical threshold instead: inside it the registration is as
        // good as no registration needed at all, outside it, it is not.
        check(err[best] <= 2.0 * atRest,
              "(d) the winning combination converges to within twice this scene's at-rest floor");
        check(runnerUp > 2.0 * atRest,
              "(d) no other sign combination is inside that band");
        // The motion axes get a ratio of their own as well, because THIS
        // is the failure that happened: motion vectors that reach nothing
        // cost the table its whole motion signal while leaving the jitter
        // columns intact and every number plausible. Found by matching on
        // the signs rather than by index arithmetic off kShipped, so that
        // reordering the table above cannot quietly point this at the
        // wrong two cells.
        double motionFlipX = 0.0, motionFlipY = 0.0;
        for (int vi = 0; vi < 16; ++vi) {
            const Variant& v = kVariants[vi];
            if (v.jsx != kVariants[best].jsx || v.jsy != kVariants[best].jsy) continue;
            if (v.msx == -kVariants[best].msx && v.msy == kVariants[best].msy) motionFlipX = err[vi];
            if (v.msx == kVariants[best].msx && v.msy == -kVariants[best].msy) motionFlipY = err[vi];
        }
        check(motionFlipX >= 2.0 * err[best] && motionFlipY >= 2.0 * err[best],
              "(d) flipping either motion axis alone at least doubles the error");
        check(best == kShipped, "(d) the shipped default ('jitter as_is, motion as_is') converges best");
        if (best != kShipped) {
            std::printf(
                "info: (d) AMD's port wants '%s' -- set kFsrJitterScaleX=%.0f, kFsrJitterScaleY=%.0f, "
                "kFsrMotionVectorScaleX=%.0f, kFsrMotionVectorScaleY=%.0f in fsr3_engine.cpp and "
                "re-run this rig.\n",
                kVariants[best].name, kVariants[best].jsx, kVariants[best].jsy, kVariants[best].msx,
                kVariants[best].msy);
        }

        // (d1) The same signs, but on a motion field EDVR's own producer
        // made: temporal_math.h's temporalReproject over a yawing camera,
        // the very chain temporal_pass.cpp builds dlMv with, spatially
        // varying and perspective-correct rather than a constant this rig
        // wrote by hand. Two runs only -- the settled signs against their
        // motion-flipped twin -- since (d) has already chosen; this only
        // has to show the choice survives a real field.
        std::vector<std::vector<unsigned char>> yawFrames(static_cast<size_t>(kRegFrames)),
            yawTruth(static_cast<size_t>(kRegFrames));
        for (int k = 0; k < kRegFrames; ++k) {
            float jx = 0.0f, jy = 0.0f;
            edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
            renderTruth(k, jx, jy, w, h, kTan, yawFrames[static_cast<size_t>(k)]);
            renderTruth(k, 0.0f, 0.0f, w, h, kTan, yawTruth[static_cast<size_t>(k)]);
        }
        std::vector<float> mvTrue;
        renderMotionField(w, h, kTan, mvTrue);
        {
            const float th = kThetaDeg * 3.14159265f / 180.0f;
            const float ct = cosf(th), st = sinf(th);
            const float delta[9] = {ct, 0, st, 0, 1, 0, -st, 0, ct};
            printMotionStats("(d1) yaw scene", w, h, kTan, delta, mvTrue);
        }
        double yawSettled = 0.0, yawFlipped = 0.0;
        bool yawOk = true;
        for (int pass = 0; pass < 2 && yawOk; ++pass) {
            const float ms = pass == 0 ? 1.0f : -1.0f;
            for (size_t i = 0; i < mvHalf.size(); i += 2) {
                mvHalf[i] = probeHalf(ms * kVariants[kShipped].msx * mvTrue[i]);
                mvHalf[i + 1] = probeHalf(ms * kVariants[kShipped].msy * mvTrue[i + 1]);
            }
            yawOk = runSequence(dev, ctx, colour, depth, mv, out, w, h, yawFrames, mvHalf,
                                kVariants[kShipped].jsx, kVariants[kShipped].jsy,
                                pass == 0 ? "(d1) settled signs" : "(d1) motion flipped", &yawTruth,
                                pass == 0 ? &yawSettled : &yawFlipped, nullptr);
        }
        check(yawOk, "(d1) both yaw-scene dispatches succeeded");
        if (yawOk) {
            std::printf(
                "info: (d1) EDVR's own reprojected field under the settled signs: error %.2f, "
                "with the motion flipped %.2f\n",
                yawSettled, yawFlipped);
            check(yawSettled < yawFlipped,
                  "(d1) the settled signs beat their motion-flipped twin on EDVR's own "
                  "reprojected motion field");
        }
    }
    colour->Release();
    depth->Release();
    mv->Release();
    out->Release();
}

// (e)
void testReset(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = kRegW, h = kRegH;
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out    = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && out;
    check(made, "(e) the reset-test textures were created");
    if (!made) {
        if (colour) colour->Release();
        if (depth) depth->Release();
        if (mv) mv->Release();
        if (out) out->Release();
        return;
    }
    fillDepthConstant(ctx, depth, w, h, 0.3f);
    std::vector<float> mvTrue;
    renderMotionField(w, h, kTan, mvTrue);
    std::vector<uint16_t> mvHalf(mvTrue.size());
    for (size_t i = 0; i < mvTrue.size(); ++i) mvHalf[i] = probeHalf(mvTrue[i]);
    ctx->UpdateSubresource(mv, 0, nullptr, mvHalf.data(), w * 4, 0);

    std::vector<unsigned char> img, ref, readback;
    auto buildHistory = [&](bool& ok) {
        for (int k = 0; k < 4 && ok; ++k) {
            float jx = 0.0f, jy = 0.0f;
            edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
            renderTruth(k, jx, jy, w, h, kTan, img);
            ctx->UpdateSubresource(colour, 0, nullptr, img.data(), w * 4, 0);
            const char* why = nullptr;
            ok = edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, w, h, w, h, jx, jy, k == 0,
                                    11.1f, 0.1f, 10000.0f, kFovY, &why);
            if (!ok) std::printf("info: (e) history frame %d refused: %s\n", k, why ? why : "?");
        }
    };
    auto cutFrame = [&](bool reset, double& err, bool& ok) {
        const int k = 20;   // a hard cut: twenty degrees at once, not one more step of the pan
        float jx = 0.0f, jy = 0.0f;
        edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
        renderTruth(k, jx, jy, w, h, kTan, img);
        ctx->UpdateSubresource(colour, 0, nullptr, img.data(), w * 4, 0);
        const char* why = nullptr;
        ok = edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, w, h, w, h, jx, jy, reset, 11.1f,
                                0.1f, 10000.0f, kFovY, &why);
        if (!ok) {
            std::printf("info: (e) cut frame refused: %s\n", why ? why : "?");
            return;
        }
        renderTruth(k, 0.0f, 0.0f, w, h, kTan, ref);
        if (!readRegion(dev, ctx, out, kErrX, kErrY, kErrW, kErrH, readback)) { ok = false; return; }
        err = regionError(readback, ref, w, kErrX, kErrY, kErrW, kErrH);
    };

    bool ok = true;
    buildHistory(ok);
    double errReset = -1.0;
    if (ok) cutFrame(true, errReset, ok);
    check(ok, "(e) the reset-branch dispatches succeeded");

    bool ok2 = true;
    buildHistory(ok2);
    double errNoReset = -1.0;
    if (ok2) cutFrame(false, errNoReset, ok2);
    check(ok2, "(e) the no-reset-branch dispatches succeeded");

    if (ok && ok2) {
        std::printf("info: (e) error after a cut -- reset %.2f/255, no reset %.2f/255\n", errReset,
                    errNoReset);
        check(errReset < 0.7 * errNoReset,
             "(e) reset recovers markedly better than carrying stale history through a cut");
    }
    colour->Release();
    depth->Release();
    mv->Release();
    out->Release();
}

// (f)
void testSizeChange(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    // Three keys in a row on the same eye, so each one rekeys the previous:
    // 1:1 at the Pimax Crystal Super's half-quality render size, 1:1 at its
    // full per-eye output, and then the upscale BETWEEN them. The design doc
    // claimed 1711x1425 -> 3422x3394 was exercised; it was not -- both of
    // those were 1:1 dispatches at two different sizes (the review of
    // 2026-09-16, F3). The third row below is the claim made real. Its axes
    // scale differently (x doubles, y does not), which FSR allows and Elite
    // would not produce: what it proves is that renderSize and upscaleSize
    // are carried independently all the way through, at the largest output
    // this rig ever asks for.
    struct Size { UINT w, h, oW, oH; };
    const Size sizes[3] = {{1711, 1425, 1711, 1425},
                           {3422, 3394, 3422, 3394},
                           {1711, 1425, 3422, 3394}};
    for (const Size& s : sizes) {
        ID3D11Texture2D* colour = makeTexture(dev, s.w, s.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D* depth  = makeTexture(dev, s.w, s.h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D* mv     = makeTexture(dev, s.w, s.h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D* out    = makeTexture(dev, s.oW, s.oH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        const bool made = colour && depth && mv && out;
        char label[80];
        snprintf(label, sizeof(label), "(f) %ux%u -> %ux%u textures were created", s.w, s.h, s.oW,
                 s.oH);
        check(made, label);
        bool ok = false;
        if (made) {
            fillConstant(ctx, colour, s.w, s.h, 128);
            fillDepthConstant(ctx, depth, s.w, s.h, 0.3f);
            fillZeroMotion(ctx, mv, s.w, s.h);
            const char* why = nullptr;
            ok = edvr::fsr3Evaluate(ctx, 1, colour, depth, mv, nullptr, out, s.w, s.h, s.oW, s.oH,
                                    0.0f, 0.0f, true, 11.1f, 0.1f, 10000.0f, kFovY, &why);
            if (!ok && why) {
                std::printf("info: (f) %ux%u -> %ux%u refused: %s\n", s.w, s.h, s.oW, s.oH, why);
            }
        }
        snprintf(label, sizeof(label), "(f) %ux%u -> %ux%u dispatched FFX_OK", s.w, s.h, s.oW, s.oH);
        check(ok, label);
        if (colour) colour->Release();
        if (depth) depth->Release();
        if (mv) mv->Release();
        if (out) out->Release();
    }
}

// The upscale case (the review of 2026-09-16, F3). Until now EVERY
// fsr3Evaluate in this rig passed outW = w: the whole point of fix.temporal_aa
// = fsr -- native_temporal.cpp sets s.upscale for it -- had never run
// anywhere, and anyone at HMD Quality below 1 (the Quest 3, and every
// FOV-trim configuration) meets that path on frame 1 of flight 1.
//
// Two sizes, for two different questions:
//   (h)  the Quest 3's own numbers, 1376x1472 -> 2064x2208 (two thirds):
//        does a context at a real VR upscale ratio create and dispatch, and
//        does anything structured come out of it?
//   (h2) the registration table's scene at the SAME ratio, 400x304 ->
//        600x456 (exactly 1.5x on both axes, as 2064/1376 and 2208/1472 are):
//        are the engine's shipped jitter and motion-vector signs still the
//        unique minimum once the output grid is no longer the render grid?
//        The Quest 3's own frame is not used for this: 4.5 MP x 8 frames x 7
//        sign cells on a software rasteriser does not fit the rig's budget,
//        and the convention is a property of the RATIO, not of the size.
constexpr UINT kUpRenderW = 400, kUpRenderH = 304;
constexpr UINT kUpOutW = 600, kUpOutH = 456;
constexpr UINT kUpErrX = 150, kUpErrY = 120, kUpErrW = 300, kUpErrH = 225;

// The truth at OUTPUT size: the same continuous scene, sampled where each
// output pixel's centre lands in render-pixel coordinates. That is what
// AMD's upsample converges to -- it reconstructs the signal its jittered
// render-size samples came from and evaluates it at (pixel + 0.5) - jitter,
// so an output pixel X reads the signal at (X + 0.5) * w/oW - 0.5 render
// pixels. Anything else here would measure the mapping, not the signs.
void renderSceneUpscaledTruth(int k, float sx, float sy, UINT w, UINT h, UINT oW, UINT oH,
                              std::vector<unsigned char>& img) {
    const float rx = static_cast<float>(w) / static_cast<float>(oW);
    const float ry = static_cast<float>(h) / static_cast<float>(oH);
    img.resize(static_cast<size_t>(oW) * oH * 4);
    for (UINT y = 0; y < oH; ++y) {
        for (UINT x = 0; x < oW; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * rx - 0.5f;
            const float fy = (static_cast<float>(y) + 0.5f) * ry - 0.5f;
            const float g = scenePattern(fx - static_cast<float>(k) * sx,
                                         fy - static_cast<float>(k) * sy);
            const unsigned char c = static_cast<unsigned char>(g * 255.0f + 0.5f);
            unsigned char* px = &img[(static_cast<size_t>(y) * oW + x) * 4];
            px[0] = c; px[1] = c; px[2] = c; px[3] = 255;
        }
    }
}

// runSequence's upscaling twin: render-size inputs, an output-size target,
// and the error measured over an output-size region against an output-size
// truth. Kept separate rather than bolted onto runSequence with five more
// parameters, so the 1:1 table that settled the shipped signs is not
// disturbed by this.
bool runUpscaleSequence(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* colour,
                        ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out,
                        const std::vector<std::vector<unsigned char>>& frames,
                        const std::vector<uint16_t>& mvHalf, float jsx, float jsy, const char* who,
                        const std::vector<std::vector<unsigned char>>& truth, double* errOut) {
    ctx->UpdateSubresource(mv, 0, nullptr, mvHalf.data(), kUpRenderW * 4, 0);
    std::vector<unsigned char> rb;
    double sum = 0.0;
    long n = 0;
    for (int k = 0; k < kRegFrames; ++k) {
        float jx = 0.0f, jy = 0.0f;
        edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
        ctx->UpdateSubresource(colour, 0, nullptr, frames[static_cast<size_t>(k)].data(),
                               kUpRenderW * 4, 0);
        const char* why = nullptr;
        if (!edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, kUpRenderW, kUpRenderH,
                                kUpOutW, kUpOutH, jsx * jx, jsy * jy, k == 0, 11.1f, 0.1f, 10000.0f,
                                kFovY, &why)) {
            std::printf("info: %s frame %d refused: %s\n", who, k, why ? why : "?");
            return false;
        }
        if (k < kRegFrames - 3) continue;
        if (!readRegion(dev, ctx, out, kUpErrX, kUpErrY, kUpErrW, kUpErrH, rb)) {
            std::printf("info: %s frame %d could not be read back\n", who, k);
            return false;
        }
        sum += regionError(rb, truth[static_cast<size_t>(k)], kUpOutW, kUpErrX, kUpErrY, kUpErrW,
                           kUpErrH) *
               (static_cast<double>(kUpErrW) * kUpErrH);
        n += static_cast<long>(kUpErrW) * kUpErrH;
    }
    if (errOut) *errOut = n ? sum / n : 1e9;
    return true;
}

// (h)
void testUpscaleQuest3(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = 1376, h = 1472, oW = 2064, oH = 2208;   // Quest 3, HMD Quality 2/3
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out    = makeTexture(dev, oW, oH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && out;
    check(made, "(h) the 1376x1472 -> 2064x2208 textures were created");
    bool ok = false;
    bool structured = false;
    if (made) {
        // A real image, not a constant: a flat input upscales to a flat
        // output, which would pass a "not blank" test while proving nothing.
        std::vector<unsigned char> img;
        renderScene(0, 0.0f, 0.0f, 0.0f, 0.0f, w, h, img);
        ctx->UpdateSubresource(colour, 0, nullptr, img.data(), w * 4, 0);
        fillDepthConstant(ctx, depth, w, h, 0.3f);
        fillZeroMotion(ctx, mv, w, h);
        const char* why = nullptr;
        ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            float jx = 0.0f, jy = 0.0f;
            edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
            ok = edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, w, h, oW, oH, jx, jy,
                                    k == 0, 11.1f, 0.1f, 10000.0f, kFovY, &why);
            if (!ok) std::printf("info: (h) frame %d refused: %s\n", k, why ? why : "?");
        }
        std::vector<unsigned char> rb;
        if (ok && readRegion(dev, ctx, out, oW / 4, oH / 4, 64, 64, rb)) {
            int lo = 255, hi = 0;
            double sum = 0.0;
            for (size_t i = 0; i < rb.size(); i += 4) {
                const int v = rb[i];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                sum += v;
            }
            const double mean = sum / (static_cast<double>(rb.size()) / 4.0);
            std::printf("info: (h) a 64x64 patch of the 2064x2208 output: min %d max %d mean %.1f\n",
                        lo, hi, mean);
            // The scene's own contrast is the coarse chequer's +-0.40 plus
            // the ripple, so a correctly upscaled patch spans well over 20
            // levels. Blank (all one value) or black (mean near 0) fails.
            structured = (hi - lo) > 20 && mean > 20.0 && mean < 235.0;
        }
    }
    check(ok, "(h) the Quest 3 upscale dispatched FFX_OK at 1376x1472 -> 2064x2208");
    check(structured, "(h) its 2064x2208 output carries the scene, not a blank or black frame");
    if (colour) colour->Release();
    if (depth) depth->Release();
    if (mv) mv->Release();
    if (out) out->Release();
}

// (h2)
void testUpscaleRegistration(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    // Seven cells, not sixteen: the 1:1 table (d) already measured all
    // sixteen and put every two-axis-wrong combination far outside the band,
    // and each convention is priced independently here, so what the ratio
    // could plausibly disturb is a SINGLE axis of either. One diagonal scene
    // instead of (d)'s two, because a diagonal moves in x and y at once and
    // so sees both motion axes in one run -- which (d)'s x-only and y-only
    // pair cannot do, and which halves a table that costs 2.25x as many
    // pixels per dispatch as (d)'s does.
    struct Cell { const char* name; float jsx, jsy, msx, msy; };
    static const Cell kCells[7] = {
        {"jitter as_is,     motion as_is    ",  1.0f,  1.0f,  1.0f,  1.0f},
        {"jitter flip_x,    motion as_is    ", -1.0f,  1.0f,  1.0f,  1.0f},
        {"jitter flip_y,    motion as_is    ",  1.0f, -1.0f,  1.0f,  1.0f},
        {"jitter flip_both, motion as_is    ", -1.0f, -1.0f,  1.0f,  1.0f},
        {"jitter as_is,     motion flip_x   ",  1.0f,  1.0f, -1.0f,  1.0f},
        {"jitter as_is,     motion flip_y   ",  1.0f,  1.0f,  1.0f, -1.0f},
        {"jitter as_is,     motion flip_both",  1.0f,  1.0f, -1.0f, -1.0f},
    };
    const int kShipped = 0;   // fsr3_engine.cpp's kFsrJitterScale*/kFsrMotionVectorScale* = +1

    ID3D11Texture2D* colour = makeTexture(dev, kUpRenderW, kUpRenderH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, kUpRenderW, kUpRenderH, DXGI_FORMAT_R32_FLOAT,
                                          D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, kUpRenderW, kUpRenderH, DXGI_FORMAT_R16G16_FLOAT,
                                          D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out    = makeTexture(dev, kUpOutW, kUpOutH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && out;
    check(made, "(h2) the 400x304 -> 600x456 registration textures were created");
    if (!made) {
        if (colour) colour->Release();
        if (depth) depth->Release();
        if (mv) mv->Release();
        if (out) out->Release();
        return;
    }
    fillDepthConstant(ctx, depth, kUpRenderW, kUpRenderH, 0.3f);

    std::vector<std::vector<unsigned char>> frames(static_cast<size_t>(kRegFrames));
    std::vector<std::vector<unsigned char>> truth(static_cast<size_t>(kRegFrames));
    std::vector<std::vector<unsigned char>> restFrames(static_cast<size_t>(kRegFrames));
    std::vector<std::vector<unsigned char>> restTruth(static_cast<size_t>(kRegFrames));
    for (int k = 0; k < kRegFrames; ++k) {
        float jx = 0.0f, jy = 0.0f;
        edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
        renderScene(k, kShiftPx, kShiftPx, jx, jy, kUpRenderW, kUpRenderH,
                    frames[static_cast<size_t>(k)]);
        renderSceneUpscaledTruth(k, kShiftPx, kShiftPx, kUpRenderW, kUpRenderH, kUpOutW, kUpOutH,
                                 truth[static_cast<size_t>(k)]);
        renderScene(0, 0.0f, 0.0f, jx, jy, kUpRenderW, kUpRenderH,
                    restFrames[static_cast<size_t>(k)]);
        renderSceneUpscaledTruth(0, 0.0f, 0.0f, kUpRenderW, kUpRenderH, kUpOutW, kUpOutH,
                                 restTruth[static_cast<size_t>(k)]);
    }

    // The floor at THIS ratio, with nothing moving and a zero motion field,
    // under the shipped jitter signs. It is a different number from (d)'s: an
    // upscale cannot reconstruct what the render grid never sampled, so the
    // band below is measured here rather than borrowed from the 1:1 table.
    std::vector<uint16_t> mvHalf(static_cast<size_t>(kUpRenderW) * kUpRenderH * 2, 0);
    double atRest = 0.0;
    bool rigOk = runUpscaleSequence(dev, ctx, colour, depth, mv, out, restFrames, mvHalf,
                                    kCells[kShipped].jsx, kCells[kShipped].jsy,
                                    "(h2) scene at rest", restTruth, &atRest);
    check(rigOk, "(h2) the scene's at-rest floor was measured at the upscale ratio");

    double err[7] = {};
    for (int ci = 0; ci < 7 && rigOk; ++ci) {
        // The content moves +(kShiftPx, kShiftPx) render pixels a frame, so
        // the motion vector current -> previous is -(kShiftPx, kShiftPx),
        // in RENDER pixels: the port divides motionVectorScale by the render
        // size itself, which is what makes a scale of 1 right at any ratio
        // (fsr3_engine.cpp's own note on kFsrMotionVectorScaleX).
        const float mvx = kCells[ci].msx * -kShiftPx;
        const float mvy = kCells[ci].msy * -kShiftPx;
        for (size_t i = 0; i < mvHalf.size(); i += 2) {
            mvHalf[i] = probeHalf(mvx);
            mvHalf[i + 1] = probeHalf(mvy);
        }
        rigOk = runUpscaleSequence(dev, ctx, colour, depth, mv, out, frames, mvHalf, kCells[ci].jsx,
                                   kCells[ci].jsy, kCells[ci].name, truth, &err[ci]);
    }
    check(rigOk, "(h2) every upscaled registration dispatch and readback succeeded");
    if (rigOk) {
        int best = 0;
        for (int ci = 1; ci < 7; ++ci) if (err[ci] < err[best]) best = ci;
        int tied = 0;
        double runnerUp = 1e18;
        for (int ci = 0; ci < 7; ++ci) {
            if (err[ci] <= err[best]) ++tied;
            if (ci != best && err[ci] < runnerUp) runnerUp = err[ci];
        }
        for (int ci = 0; ci < 7; ++ci) {
            std::printf("info: (h2) %s error %6.2f%s\n", kCells[ci].name, err[ci],
                        ci == best ? "  <- least" : "");
        }
        std::printf("info: (h2) at 400x304 -> 600x456: least %.2f, runner-up %.2f, at-rest floor "
                    "%.2f, the band %.2f; above the floor: least %+.2f, runner-up %+.2f\n",
                    err[best], runnerUp, atRest, 2.0 * atRest, err[best] - atRest,
                    runnerUp - atRest);
        check(tied == 1, "(h2) exactly one sign combination is the least-error one at the upscale ratio");
        check(best == kShipped,
              "(h2) the shipped signs still converge best when the output grid is not the render grid");
        // (d)'s band -- twice the floor this same scene reaches with nothing
        // to register -- still holds for the WINNER, and is kept here.
        check(err[best] <= 2.0 * atRest,
              "(h2) the winning combination converges to within twice the upscaled at-rest floor");
        // (d)'s other half, "and nothing else is inside that band", CANNOT be
        // reused at an upscale ratio, and this is measured, not assumed. The
        // floor itself is what changes: at 1:1 it was 0.93/255 and a wrong
        // jitter sign cost 2.52, nearly three times it; at 3:2 the floor is
        // 2.05 -- it now carries the upscaler's own reconstruction residual,
        // which no sign can improve -- and a wrong jitter sign costs 3.38,
        // well inside twice 2.05. A band of 2x the floor admits it. What
        // separates the conventions at this ratio is the DISTANCE ABOVE the
        // floor: the winner is +0.01 (it lands on the floor -- once the
        // registration is right, moving content costs nothing the resolve was
        // not already paying), and the next best is +1.33. So the bar is that
        // the winner is far nearer the floor than the runner-up is, which is
        // scale-free and had a factor of about thirty in hand when it was
        // measured here.
        check(err[best] - atRest <= 0.25 * (runnerUp - atRest),
              "(h2) the winner sits on the upscaled at-rest floor and every other combination is "
              "well above it");
    }
    colour->Release();
    depth->Release();
    mv->Release();
    out->Release();
}

// (i) fsr3Warm, which nothing exercised: the loading-screen path that
// temporal_pass.cpp calls before the first submitted frame. Measured through
// createMs rather than by scraping the log -- a warm that MADE a context
// reports the time it took, and one that found it already made reports zero.
// The same instrument proves the two things the engine now promises about
// the context key: a live advanced.temporal_aa_diagnostics flip remakes it
// (F8), and fsr3ReleaseFeatures really frees it (F7).
void testWarmAndRelease(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = 960, h = 800, oW = 1440, oH = 1200;   // a key no other case uses
    double ms = -1.0;
    const char* why = nullptr;
    const bool warmed = edvr::fsr3Warm(ctx, w, h, oW, oH, &ms, &why);
    if (!warmed && why) std::printf("info: (i) fsr3Warm refused: %s\n", why);
    check(warmed, "(i) fsr3Warm made both eyes' contexts at 960x800 -> 1440x1200");
    std::printf("info: (i) the warm-up's own create cost %.0f ms for the pair\n", ms);
    check(warmed && ms > 0.0, "(i) the warm-up reports the time its creates took");

    double again = -1.0;
    const bool second = edvr::fsr3Warm(ctx, w, h, oW, oH, &again, &why);
    check(second && again == 0.0, "(i) a second warm at the same key creates nothing");

    // What the warm-up is FOR: the first evaluate at that key finds the
    // context made. It must succeed, and a warm straight after it must still
    // cost nothing -- which is what says the evaluate used the warmed context
    // rather than quietly rekeying over it.
    {
        ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D* out    = makeTexture(dev, oW, oH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        const bool made = colour && depth && mv && out;
        check(made, "(i) the textures for a dispatch at the warmed key were created");
        bool ok = false, stillWarm = false;
        if (made) {
            fillConstant(ctx, colour, w, h, 128);
            fillDepthConstant(ctx, depth, w, h, 0.3f);
            fillZeroMotion(ctx, mv, w, h);
            const char* whyEval = nullptr;
            ok = edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, w, h, oW, oH, 0.0f,
                                    0.0f, true, 11.1f, 0.1f, 10000.0f, kFovY, &whyEval);
            if (!ok && whyEval) std::printf("info: (i) the warmed-key dispatch refused: %s\n", whyEval);
            double after = -1.0;
            stillWarm = edvr::fsr3Warm(ctx, w, h, oW, oH, &after, &why) && after == 0.0;
            double infiniteMs = -1.0;
            check(edvr::fsr3Warm(ctx, w, h, oW, oH, &infiniteMs, &why, true) && infiniteMs > 0,
                  "(i) infinite reversed depth creates a distinct context key");
            check(edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, out, w, h, oW, oH,
                  0, 0, true, 11.1f, .025f, 0, kFovY, &whyEval, true),
                  "(i) explicit infinite depth dispatch accepts the actual near plane");
            infiniteMs = -1.0;
            check(edvr::fsr3Warm(ctx, w, h, oW, oH, &infiniteMs, &why, true) && infiniteMs == 0,
                  "(i) infinite depth dispatch reuses its warm context");
            double finiteMs = -1.0;
            check(edvr::fsr3Warm(ctx, w, h, oW, oH, &finiteMs, &why) && finiteMs > 0,
                  "(i) default finite VR depth recreates its original context key");
        }
        check(ok, "(i) a dispatch at the warmed key succeeded");
        check(stillWarm, "(i) and it found the warm-up's own context, creating nothing");
        if (colour) colour->Release();
        if (depth) depth->Release();
        if (mv) mv->Release();
        if (out) out->Release();
    }

    // advanced.temporal_aa_diagnostics is part of the key now: flipping it
    // live must remake the context instead of doing nothing until a size
    // moves. The rig runs with it ON (run(), below), so turn it off here and
    // put it back afterwards.
    edvr::Config::get().set("advanced.temporal_aa_diagnostics", "0");
    double flipped = -1.0;
    const bool afterFlip = edvr::fsr3Warm(ctx, w, h, oW, oH, &flipped, &why);
    edvr::Config::get().set("advanced.temporal_aa_diagnostics", "1");
    std::printf("info: (i) after flipping advanced.temporal_aa_diagnostics the warm cost %.0f ms\n",
                flipped);
    check(afterFlip && flipped > 0.0,
          "(i) flipping advanced.temporal_aa_diagnostics live remakes the context");

    double back = -1.0;
    const bool restored = edvr::fsr3Warm(ctx, w, h, oW, oH, &back, &why);
    check(restored && back > 0.0, "(i) flipping it back remakes the context again");

    edvr::fsr3ReleaseFeatures();
    double afterRelease = -1.0;
    const bool remade = edvr::fsr3Warm(ctx, w, h, oW, oH, &afterRelease, &why);
    std::printf("info: (i) after fsr3ReleaseFeatures the same key cost %.0f ms again\n",
                afterRelease);
    check(remade && afterRelease > 0.0,
          "(i) fsr3ReleaseFeatures frees the contexts, so the same key is made afresh");
    edvr::fsr3ReleaseFeatures();
}

// (j) The reactive mask, null at every call site until now, so
// advanced.temporal_aa_fsr_reactive was untested end to end. The seam hands
// FSR an R8_UNORM mask at render size (temporal_pass.cpp's e.dlMask); the
// shape is what is checked here, not the picture.
void testReactiveMask(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = kRegW, h = kRegH;
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mask   = makeTexture(dev, w, h, DXGI_FORMAT_R8_UNORM,       D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out    = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    const bool made = colour && depth && mv && mask && out;
    check(made, "(j) the reactive-mask textures were created");
    bool ok = false;
    if (made) {
        std::vector<unsigned char> img;
        renderScene(0, 0.0f, 0.0f, 0.0f, 0.0f, w, h, img);
        ctx->UpdateSubresource(colour, 0, nullptr, img.data(), w * 4, 0);
        fillDepthConstant(ctx, depth, w, h, 0.3f);
        fillZeroMotion(ctx, mv, w, h);
        // Half the frame reactive, half not: a uniform mask could be ignored
        // wholesale without changing the answer.
        std::vector<unsigned char> m(static_cast<size_t>(w) * h, 0);
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = w / 2; x < w; ++x) m[static_cast<size_t>(y) * w + x] = 200;
        }
        ctx->UpdateSubresource(mask, 0, nullptr, m.data(), w, 0);
        const char* why = nullptr;
        ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            float jx = 0.0f, jy = 0.0f;
            edvr::temporalJitter(static_cast<uint32_t>(k), &jx, &jy);
            ok = edvr::fsr3Evaluate(ctx, 1, colour, depth, mv, mask, out, w, h, w, h, jx, jy,
                                    k == 0, 11.1f, 0.1f, 10000.0f, kFovY, &why);
            if (!ok) std::printf("info: (j) frame %d refused: %s\n", k, why ? why : "?");
        }
    }
    check(ok, "(j) a dispatch with an R8_UNORM reactive mask succeeded");
    if (colour) colour->Release();
    if (depth) depth->Release();
    if (mv) mv->Release();
    if (mask) mask->Release();
    if (out) out->Release();
}

// (k) The bind-flag contract, both halves (the review of 2026-09-16, F2):
//   k1  fsr3Evaluate REFUSES a write-only output before registering
//       anything, with a reason naming the texture and the flag.
//   k2  and behind that front stop, the catch really does catch: with the
//       check switched off (a test-only hook), the same texture reaches
//       AMD's RegisterResourceDX11, its CreateShaderResourceView fails, its
//       TIF helper throws `1` out of an extern "C" function -- and this must
//       come back as false plus a reason rather than taking the process
//       down. That is only true because build.bat compiles both this rig and
//       the d3d11 half with /EHs; under /EHsc the compiler is entitled to
//       assume that exception cannot exist.
void testBindFlagContract(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = 512, h = 384;   // a key no other case uses
    ID3D11Texture2D* colour = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depth  = makeTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT,      D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* mv     = makeTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,   D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* uavOnly = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D11_BIND_UNORDERED_ACCESS);
    const bool made = colour && depth && mv && uavOnly;
    check(made, "(k) the write-only-output textures were created");
    if (made) {
        fillConstant(ctx, colour, w, h, 128);
        fillDepthConstant(ctx, depth, w, h, 0.3f);
        fillZeroMotion(ctx, mv, w, h);

        const char* why = nullptr;
        const bool refused = !edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, uavOnly, w, h,
                                                 w, h, 0.0f, 0.0f, true, 11.1f, 0.1f, 10000.0f,
                                                 kFovY, &why);
        std::printf("info: (k1) a UAV-only output was refused with: %s\n", why ? why : "(no reason)");
        check(refused, "(k1) fsr3Evaluate refuses a write-only output instead of registering it");
        check(refused && why && std::strstr(why, "D3D11_BIND_SHADER_RESOURCE") != nullptr &&
                  std::strstr(why, "output") != nullptr,
              "(k1) and its reason names the output texture and the missing flag");

        // k2: the same call with the front stop off. A throw that escaped
        // would take this process down, so reaching the line after it at all
        // is half the result; the other half is that it came back false.
        edvr::fsr3TestSkipBindCheck(true);
        const char* why2 = nullptr;
        const bool threwCleanly = !edvr::fsr3Evaluate(ctx, 0, colour, depth, mv, nullptr, uavOnly, w,
                                                      h, w, h, 0.0f, 0.0f, true, 11.1f, 0.1f,
                                                      10000.0f, kFovY, &why2);
        edvr::fsr3TestSkipBindCheck(false);
        std::printf("info: (k2) with the check off, AMD's port answered: %s\n",
                    why2 ? why2 : "(no reason)");
        check(threwCleanly,
              "(k2) the port's own failure on that texture comes back as false, not as a crash");
        check(threwCleanly && why2 && std::strstr(why2, "dispatch") != nullptr,
              "(k2) and it is reported as a failed dispatch with a reason");
        // Whatever state that half-run dispatch left in the context, it is
        // not to be measured by anything after this.
        edvr::fsr3ReleaseFeatures();
    }
    if (colour) colour->Release();
    if (depth) depth->Release();
    if (mv) mv->Release();
    if (uavOnly) uavOnly->Release();
}

// (g)
void testVramQuery(ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    uint64_t bytes = 0;
    bool ok = SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) &&
             SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
             SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)));
    if (ok) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        ok = SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
        if (ok) bytes = info.CurrentUsage;
    }
    // Informational only. The engine's create line no longer measures this
    // (flights 1 to 3 showed the create-time delta is noise); it computes
    // its three surfaces' bytes instead. This probe stays so a future
    // measured figure has WARP's answer on record.
    if (ok) {
        std::printf("info: (g) IDXGIAdapter3::QueryVideoMemoryInfo answered on WARP: %llu bytes local "
                    "usage (the engine does not use it: its create line computes its own surfaces' bytes)\n",
                    static_cast<unsigned long long>(bytes));
    } else {
        std::printf(
            "info: (g) IDXGIAdapter3::QueryVideoMemoryInfo is not available on WARP (expected for a "
            "software adapter; the engine does not use it either).\n");
    }
}

int run() {
    // Enables AMD's own ENABLE_DEBUG_CHECKING context flag (fsr3_engine.cpp's
    // ensureContext reads this at every context creation, not through
    // Fsr3Settings) for the whole rig, not only test (b) -- Config::get().set
    // is the sanctioned in-memory test-injection mechanism used elsewhere
    // (e.g. native_frame_test.cpp), no file I/O.
    edvr::Config::get().set("advanced.temporal_aa_diagnostics", "1");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!createWarpDevice(device, context)) {
        std::printf("FAIL: could not create a WARP device\n");
        std::printf("fsr3_engine_test: %u checks, %u failures\n", g_checks, g_failures + 1);
        return 1;
    }

    const char* why = nullptr;
    const bool avail = edvr::fsr3Available(device.Get(), &why);
    check(avail, "(a) fsr3Available on WARP");
    std::printf("info: (a) fsr3Available = %s (%s)\n", avail ? "true" : "false", why ? why : "?");
    if (avail) {
        testContextCreateAndSilence(device.Get(), context.Get());
        dumpDebugMessages(device.Get(), "after (b)");
        testAtRest(device.Get(), context.Get());
        testMotionReaches(device.Get(), context.Get());
        testRegistrationTable(device.Get(), context.Get());
        testReset(device.Get(), context.Get());
        testSizeChange(device.Get(), context.Get());
        testUpscaleQuest3(device.Get(), context.Get());
        testUpscaleRegistration(device.Get(), context.Get());
        testWarmAndRelease(device.Get(), context.Get());
        testReactiveMask(device.Get(), context.Get());
        // Last of the dispatching cases: (k2) deliberately drives AMD's port
        // into its own throw, and nothing after it should be measuring a
        // context that went through that.
        testBindFlagContract(device.Get(), context.Get());
        dumpDebugMessages(device.Get(), "after (k)");
        testVramQuery(device.Get());
        edvr::fsr3Shutdown();
    }

    std::printf("fsr3_engine_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered, like every other rig under tools\ (e.g.
    // openxr_shared_texture_test.cpp's benchmark()): run_jobs.py captures
    // this process's own stdout, and a crash or a watchdog ExitProcess must
    // not take a block-buffered partial run with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts(
            "Would test AMD's FSR3 engine on a WARP device: availability, context creation, "
            "DEBUG_CHECKING silence, the jitter/motion-vector sign registration table, reset, a "
            "size change, the Quest 3's own upscale and the sign table at that ratio, the "
            "loading-screen warm-up and the release that frees it, a reactive mask, the "
            "bind-flag refusal and the caught throw behind it, and WARP's answer to the video "
            "memory query; no devices "
            "or files.");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test")) return 2;
    // A hang here must not hang the whole build; run_jobs.py would wait on
    // this process forever otherwise.
    std::thread watchdog([] { Sleep(240000); ExitProcess(124); });
    watchdog.detach();
    // fsr3_engine.cpp's ensureContext/fsr3Evaluate now catch AMD's port's own
    // TIF-guarded `throw 1` (ffx_dx11.cpp) at both call sites that can reach
    // it and fold it into their normal false+why return, so this should
    // never fire from a call through fsr3_engine.h. Kept anyway, the same
    // belt-and-suspenders spirit as the watchdog thread above: a rig that
    // takes down the whole build with an unreadable native crash on a path
    // nobody has thought of yet is worse than one that prints a message and
    // fails cleanly.
    try {
        return run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: uncaught std::exception reached main(): %s\n", e.what());
        std::fflush(stderr);
        return 3;
    } catch (...) {
        std::fprintf(stderr, "FAIL: uncaught non-std::exception reached main()\n");
        std::fflush(stderr);
        return 3;
    }
}
