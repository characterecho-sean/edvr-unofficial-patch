// flat_sharpen_pass_test -- the REAL sharpening pass on WARP, the way the flat profile calls it.
//
// flat_sharpen_test.cpp checks the wrapper against a stubbed pass. This one links the shipped
// sharpen_pass.cpp (the RCAS compute shader, its precompiled bytecode, its constants, its
// resource handling) and asks what the flat profile asks of it:
//
//  - THE GOLDEN. A 64x48 R8G8B8A8_TYPELESS image built to be hard on RCAS (a smooth
//    gradient, two-pixel stripes, hard diagonal edges, saturated black/white/colour blocks,
//    a flat field, varying alpha) is sharpened at 0.3 and 1.0, and every byte of the result
//    is compared with an independent CPU implementation of AMD's RCAS (FsrRcasF, from
//    src/d3d11/fsr/ffx_fsr1.h) to within one level, alpha exactly. A recorded hash would
//    move with the next WARP; the reference does not.
//  - THE PROPERTIES. A flat field stays itself exactly; stripes get harder, more at 1.0
//    than at 0.3; the result is the source's size and format; the SOURCE IS NEVER WRITTEN
//    (the flat resolve reads its output back as history, so an in-place sharpen would
//    compound every frame).
//  - THROUGH THE WRAPPER: the same golden and properties on what flatSharpenView returns,
//    with an sRGB view in and an sRGB view out.
//  - NEGATIVE CONTROLS. Five deliberately broken twins run through the same checker and
//    must each FAIL it: strength inverted (stops = 2*s, not 2*(1-s)), the region shifted
//    one texel, the result copied back over the source, strength halved, and a wrapper
//    that hands back the resolve's own view. A rig whose mutants pass proves nothing.
//  - TWO DEVICES (RC4 review, F5). The pass keeps its resources on one D3D11 device; a frame from
//    another releases and re-makes them. The real pass, then the wrapper with its state carried
//    across, run on two WARP devices in one process: A, B, A, correct against the RCAS reference
//    and made on the frame's own device, with no stand-down; the changes are said, and capped; the
//    tick's warm compile follows the same rule. The CONTROL switches the reset off
//    (sharpenPassDeviceResetOffForTest) and shows what it prevents: the pass hands B a texture made
//    on A, the wrapper cannot view it and stands the sharpening down.
//  - THE LOG, read back from a real file: the flat wording (frames, the game's output copy),
//    the warm-up line, the first-sharpened-frame line, the totals, the cost line where the
//    GPU timer answers, and the never-ran note that names anti-aliasing or the flat runtime
//    where VR's names a compositor hook. A session that DOES sharpen its frames never says
//    that note: a second run, --self-test-working (the pass latches the note per process).
//   flat_sharpen_pass_test --dry-run | --self-test | --self-test-working
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "flat_sharpen_rig.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/timing.h"
#include "../../src/d3d11/flat_sharpen.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/sharpen_pass.h"

using namespace rig;

// What sharpen_pass.cpp and its neighbours reach for, stubbed the way the other rigs do:
// the crash channel, the census (tools/gpu_census_test covers it), the monitor's event
// sink, and the two questions about a compositor hook, which VR answers and flat never asks.
namespace edvr {
void breadcrumb(const char*) {}
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
void perfMonitorNoteEvent(uint32_t, double) {}
bool glitchConsumerPresent() { return false; }
bool nativeSharpenActive() { return false; }
}  // namespace edvr

namespace {

constexpr int W = 64, H = 48;

// ---------------------------------------------------------------------------
// AMD's RCAS on the CPU: FsrRcasF (ffx_fsr1.h) with FSR_RCAS_DENOISE off, the loads clamped
// into the image the way the pass clamps them into its region. min/max are the
// number-ignoring ones (D3D's max(NaN, x) is x), so a black or white neighbourhood --
// where a reciprocal is infinite -- lands where the shader lands.
float rcpMedApprox(float a) {  // APrxMedRcpF1
    uint32_t au;
    std::memcpy(&au, &a, 4);
    const uint32_t bu = 0x7ef19fffu - au;
    float b;
    std::memcpy(&b, &bu, 4);
    return b * (-b * a + 2.0f);
}

float min3(float a, float b, float c) { return std::fmin(a, std::fmin(b, c)); }
float max3(float a, float b, float c) { return std::fmax(a, std::fmax(b, c)); }

std::vector<uint8_t> rcasReference(const std::vector<uint8_t>& src, float strength) {
    const float stops = 2.0f * (1.0f - strength);
    const float sharp = std::exp2(-stops);
    const float limit = 0.25f - 1.0f / 16.0f;
    std::vector<uint8_t> out(src.size());
    auto load = [&](int x, int y, int c) {
        x = x < 0 ? 0 : x > W - 1 ? W - 1 : x;
        y = y < 0 ? 0 : y > H - 1 ? H - 1 : y;
        return src[(static_cast<size_t>(y) * W + x) * 4 + c] / 255.0f;
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float b[3], d[3], e[3], f[3], h[3], lobeC[3];
            for (int c = 0; c < 3; ++c) {
                b[c] = load(x, y - 1, c);
                d[c] = load(x - 1, y, c);
                e[c] = load(x, y, c);
                f[c] = load(x + 1, y, c);
                h[c] = load(x, y + 1, c);
                const float mn4 = std::fmin(min3(b[c], d[c], f[c]), h[c]);
                const float mx4 = std::fmax(max3(b[c], d[c], f[c]), h[c]);
                const float hitMin = std::fmin(mn4, e[c]) * (1.0f / (4.0f * mx4));
                const float hitMax = (1.0f - std::fmax(mx4, e[c])) * (1.0f / (4.0f * mn4 - 4.0f));
                lobeC[c] = std::fmax(-hitMin, hitMax);
            }
            const float lobe = std::fmax(-limit, std::fmin(max3(lobeC[0], lobeC[1], lobeC[2]), 0.0f)) * sharp;
            const float rcpL = rcpMedApprox(4.0f * lobe + 1.0f);
            const size_t at = (static_cast<size_t>(y) * W + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float p = (lobe * b[c] + lobe * d[c] + lobe * h[c] + lobe * f[c] + e[c]) * rcpL;
                const float v = std::fmin(std::fmax(p, 0.0f), 1.0f) * 255.0f;
                out[at + c] = static_cast<uint8_t>(std::floor(v + 0.5f));
            }
            out[at + 3] = src[at + 3];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The test image. Nothing in it is smooth by accident.
std::vector<uint8_t> testImage() {
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t r, g, b;
            if (x < 16) {                                  // a smooth gradient
                r = static_cast<uint8_t>(40 + x * 8);
                g = static_cast<uint8_t>(40 + y * 3);
                b = 90;
            } else if (x < 32) {                           // two-pixel stripes, 60 and 180
                r = g = b = (x % 4) < 2 ? 60 : 180;
            } else if (x < 48) {                           // a hard diagonal edge, tinted
                const bool hi = ((x - 32) + y) % 16 < 8;
                r = hi ? 220 : 30;
                g = hi ? 210 : 40;
                b = hi ? 190 : 55;
            } else {                                       // a flat field with three hard blocks
                r = 200; g = 120; b = 60;
                if (y >= 8 && y < 12) {
                    if (x >= 52 && x < 56) r = g = b = 0;
                    else if (x >= 56 && x < 60) r = g = b = 255;
                    else if (x >= 60) { r = 255; g = 0; b = 128; }
                }
            }
            const size_t at = (static_cast<size_t>(y) * W + x) * 4;
            px[at + 0] = r;
            px[at + 1] = g;
            px[at + 2] = b;
            px[at + 3] = static_cast<uint8_t>(255 - (x * 3 + y) % 200);
        }
    }
    return px;
}

struct Env {
    ComPtr<ID3D11Texture2D> src;
    std::vector<uint8_t> bytes;
};

Env makeEnv(ID3D11Device* dev) {
    Env e;
    e.bytes = testImage();
    e.src = makeTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, &e.bytes);
    return e;
}

struct Impl {
    const char* name;
    std::function<ComPtr<ID3D11Texture2D>(Env&, float)> run;
};

// One implementation through the whole checker. Returns how many things it got wrong;
// `why` names the first. `verbose` prints them (the real implementations); the mutants run
// silent, because failing is their job.
int checkImpl(Warp& w, const Impl& impl, bool verbose, std::string* why) {
    int bad = 0;
    auto fail = [&](const std::string& what) {
        ++bad;
        if (why && why->empty()) *why = what;
        if (verbose) {
            ++g_checks;
            ++g_failures;
            std::printf("FAIL: %s: %s\n", impl.name, what.c_str());
        }
    };
    auto pass = [&] { if (verbose) ++g_checks; };
    Env env = makeEnv(w.device.Get());
    if (!env.src) {
        fail("the source texture could not be made");
        return bad;
    }
    int darkAt[2] = {0, 0}, brightAt[2] = {0, 0};
    const float strengths[2] = {0.3f, 1.0f};
    for (int i = 0; i < 2; ++i) {
        const float s = strengths[i];
        char label[96];
        std::snprintf(label, sizeof(label), "strength %.1f", static_cast<double>(s));
        ComPtr<ID3D11Texture2D> res = impl.run(env, s);
        if (!res) {
            fail(std::string(label) + ": no result");
            continue;
        }
        D3D11_TEXTURE2D_DESC d{};
        res->GetDesc(&d);
        if (d.Width != static_cast<UINT>(W) || d.Height != static_cast<UINT>(H) ||
            d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
            fail(std::string(label) + ": the result is not the source's size and format");
            continue;
        }
        pass();
        const std::vector<uint8_t> got = readBytes(w.device.Get(), w.context.Get(), res.Get());
        const std::vector<uint8_t> want = rcasReference(env.bytes, s);
        int worst = 0, badPixels = 0, alphaBad = 0;
        size_t firstBad = 0;
        for (size_t k = 0; k < want.size(); k += 4) {
            int here = 0;
            for (int c = 0; c < 3; ++c) here = std::max(here, std::abs(int(got[k + c]) - int(want[k + c])));
            worst = std::max(worst, here);
            if (here > 1) { if (!badPixels) firstBad = k / 4; ++badPixels; }
            if (got[k + 3] != want[k + 3]) ++alphaBad;
        }
        if (badPixels || alphaBad) {
            char m[200];
            std::snprintf(m, sizeof(m),
                          "%s: %d pixels differ from the RCAS reference by more than one level (worst %d, first at x=%zu y=%zu), %d alpha",
                          label, badPixels, worst, firstBad % W, firstBad / W, alphaBad);
            fail(m);
        } else {
            pass();
        }
        // A flat field stays itself (rows 20..39, columns 50..63: clear of the blocks): to within
        // one level, the reciprocal RCAS resolves with being an approximation by design, and the
        // alpha exactly.
        bool flatHeld = true;
        for (int y = 20; y < 40; ++y) {
            for (int x = 50; x < 64; ++x) {
                const size_t at = (static_cast<size_t>(y) * W + x) * 4;
                for (int c = 0; c < 3; ++c) flatHeld = flatHeld && std::abs(int(got[at + c]) - int(env.bytes[at + c])) <= 1;
                flatHeld = flatHeld && got[at + 3] == env.bytes[at + 3];
            }
        }
        if (!flatHeld) fail(std::string(label) + ": a flat field did not stay itself");
        else pass();
        // The stripes: column 20 is a dark one (60), 22 a bright one (180), row 24.
        darkAt[i] = got[(static_cast<size_t>(24) * W + 20) * 4];
        brightAt[i] = got[(static_cast<size_t>(24) * W + 22) * 4];
        // The source is only read.
        const std::vector<uint8_t> after = readBytes(w.device.Get(), w.context.Get(), env.src.Get());
        if (after != env.bytes) fail(std::string(label) + ": the SOURCE was written");
        else pass();
    }
    // Stripes get harder, and more at 1.0 than at 0.3 (which sits between the source and 1.0).
    if (darkAt[1] > 52 || brightAt[1] < 188) {
        char m[160];
        std::snprintf(m, sizeof(m), "full strength left a dark stripe at %d and a bright one at %d (want under 53 and over 187)",
                      darkAt[1], brightAt[1]);
        fail(m);
    } else if (!(darkAt[1] < darkAt[0] && darkAt[0] < 60) || !(brightAt[1] > brightAt[0] && brightAt[0] > 180)) {
        char m[160];
        std::snprintf(m, sizeof(m), "0.3 gave %d/%d, 1.0 gave %d/%d: not in between the source's 60/180 and full strength",
                      darkAt[0], brightAt[0], darkAt[1], brightAt[1]);
        fail(m);
    } else {
        pass();
    }
    return bad;
}

// ---------------------------------------------------------------------------
// The implementations under test.

ComPtr<ID3D11Texture2D> asTexture(void* p) {
    return ComPtr<ID3D11Texture2D>(static_cast<ID3D11Texture2D*>(p));   // the pass's own, borrowed: AddRef'd here
}

bool g_wrapperFormatKept = true;

ComPtr<ID3D11Texture2D> throughWrapper(Warp& w, Env& e, float s) {
    auto& cfg = edvr::Config::get();
    char value[32];
    std::snprintf(value, sizeof(value), "%g", static_cast<double>(s));
    cfg.set("fix.render_sharpness", value);
    edvr::flatSharpenReset();
    ComPtr<ID3D11ShaderResourceView> in = makeSrv(w.device.Get(), e.src.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    ID3D11ShaderResourceView* out = edvr::flatSharpenView(w.context.Get(), in.Get());
    if (!out || out == in.Get()) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    out->GetDesc(&vd);
    if (vd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) g_wrapperFormatKept = false;
    ComPtr<ID3D11Resource> r;
    out->GetResource(&r);
    ComPtr<ID3D11Texture2D> t;
    r.As(&t);
    return t;
}

// ---------------------------------------------------------------------------
// The scenarios. One real log stays open across all of them.

uint64_t g_now = 1000;
uint64_t fakeClock() { return g_now; }

void neverRan(Warp& w) {
    // The setting is on, anti-aliasing is off, and nothing has been sharpened: after 30 s the
    // tick says so once, in the flat profile's own words, and never mentions a compositor hook.
    auto& cfg = edvr::Config::get();
    cfg.set("fix.render_sharpness", "0.3");
    cfg.set("fix.temporal_aa", "off");
    edvr::sharpenPassConfigure(cfg);
    edvr::g_clockForTest = fakeClock;
    g_now = 1000;
    edvr::sharpenPassTick(w.context.Get());   // the first tick: the warm compile
    g_now += 31000;
    edvr::sharpenPassTick(w.context.Get());   // 31 s on: the note
    edvr::sharpenPassTick(w.context.Get());   // said once
    edvr::g_clockForTest = nullptr;
}

void neverRanWording() {
    char vr[700], off[700], on[700];
    edvr::sharpenPassNeverRanText(vr, sizeof(vr), false, true, 0.3f);
    edvr::sharpenPassNeverRanText(off, sizeof(off), true, false, 0.3f);
    edvr::sharpenPassNeverRanText(on, sizeof(on), true, true, 0.3f);
    check(std::strstr(vr, "compositor hook") && std::strstr(vr, "openvr_api.dll"),
          "VR's never-ran note still names the compositor hook and openvr_api.dll");
    check(std::strstr(off, "anti-aliasing is off") && !std::strstr(off, "openvr_api.dll") &&
              !std::strstr(off, "compositor hook"),
          "flat, anti-aliasing off: names the anti-aliasing, not a compositor hook");
    check(std::strstr(on, "no resolved frame") && std::strstr(on, "'flat runtime:' lines") &&
              !std::strstr(on, "openvr_api.dll") && !std::strstr(on, "compositor hook"),
          "flat, anti-aliasing on: names the flat runtime's lines, not a compositor hook");
}

// The other half of the never-ran note: a session whose frames ARE sharpened must not say
// it, however long it runs. The note is for a pass with nothing to work on; said over a
// working one it would send a reader hunting a fault that is not there. The pass latches
// the note for the process (it is a session's), so the two halves are two runs of this
// rig: `--self-test` (nothing sharpened) and `--self-test-working` (this).
void workingSession(Warp& w) {
    auto& cfg = edvr::Config::get();
    cfg.set("fix.render_sharpness", "0.3");
    cfg.set("fix.temporal_aa", "on");
    edvr::sharpenPassConfigure(cfg);
    edvr::flatSharpenReset();
    edvr::g_clockForTest = fakeClock;
    g_now = 1000;
    edvr::sharpenPassTick(w.context.Get());   // the first tick: the warm compile
    Env e = makeEnv(w.device.Get());
    ComPtr<ID3D11ShaderResourceView> in = makeSrv(w.device.Get(), e.src.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    int sharpened = 0;
    for (int i = 0; i < 4; ++i) sharpened += edvr::flatSharpenView(w.context.Get(), in.Get()) != in.Get();
    check(sharpened == 4, "the working session sharpens its frames");
    g_now += 31000;
    edvr::sharpenPassTick(w.context.Get());   // 31 s on, frames sharpened: nothing to say
    g_now += 31000;
    edvr::sharpenPassTick(w.context.Get());   // and a minute on
    edvr::g_clockForTest = nullptr;
}

void workingAssertions(const std::string& log) {
    check(countLines(log, "render sharpening: first sharpened frame") == 1,
          "log: the working session's first sharpened frame is said once");
    check(countLines(log, "no frame has been sharpened") == 0 && countLines(log, "compositor hook") == 0 &&
              countLines(log, "openvr_api.dll") == 0,
          "log: a session that sharpens its frames never says the never-ran note");
    for (const std::string& line : linesWith(log, "render sharpening")) std::printf("  log: %s\n", line.c_str());
}

void firstFrames(Warp& w) {
    // A run of frames through the wrapper and the real pass, the way the flat runtime
    // drives them: enough that the pass's GPU timer has samples to report.
    auto& cfg = edvr::Config::get();
    cfg.set("fix.render_sharpness", "0.3");
    edvr::flatSharpenReset();
    Env e = makeEnv(w.device.Get());
    ComPtr<ID3D11ShaderResourceView> in = makeSrv(w.device.Get(), e.src.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    int sharpened = 0;
    for (int i = 0; i < 600; ++i) {
        ID3D11ShaderResourceView* out = edvr::flatSharpenView(w.context.Get(), in.Get());
        sharpened += out != in.Get();
        // A full sync (a copy and a map) every frame: the pass's GPU timer reads its queries a
        // frame behind, and WARP finishes them on its own thread. Without this the 120th timed
        // pass lands whenever WARP gets to it, in a later scenario, at a later strength, and the
        // cost line's figures (and the run's verdict) move from run to run.
        readBytes(w.device.Get(), w.context.Get(), e.src.Get());
    }
    check(sharpened == 600, "600 frames through the wrapper and the real pass are all sharpened");
    uint32_t treated = 0;
    double avg = 0, mx = 0;
    check(edvr::sharpenPassTotals(&treated, &avg, &mx) && treated >= 600, "the pass counted them");
    std::printf("  the pass's own timer: %u frames, %.3f ms average, %.3f ms max (WARP, %dx%d)\n",
                treated, avg, mx, W, H);
}

void golden(Warp& w) {
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    const Impl real{"the real pass", [](Env& e, float s) { return asTexture(edvrSharpen(e.src.Get(), 0, nullptr, s)); }};
    std::string why;
    checkImpl(w, real, true, &why);
    g_wrapperFormatKept = true;
    const Impl viaWrapper{"through flatSharpenView", [&w](Env& e, float s) { return throughWrapper(w, e, s); }};
    checkImpl(w, viaWrapper, true, &why);
    check(g_wrapperFormatKept, "through flatSharpenView: an sRGB view in is an sRGB view out");
}

void mutants(Warp& w) {
    struct M { Impl impl; const char* named; };
    std::vector<M> ms;
    ms.push_back({{"strength inverted", [](Env& e, float s) { return asTexture(edvrSharpen(e.src.Get(), 0, nullptr, 1.0f - s)); }},
                  "stops = 2*strength instead of 2*(1-strength)"});
    ms.push_back({{"region shifted one texel", [](Env& e, float s) {
                       const float bounds[4] = {1.0f / W, 0.0f, 1.0f, 1.0f};
                       return asTexture(edvrSharpen(e.src.Get(), 0, bounds, s));
                   }}, "the region starts one texel in"});
    ms.push_back({{"result copied back over the source", [&w](Env& e, float s) {
                       ComPtr<ID3D11Texture2D> r = asTexture(edvrSharpen(e.src.Get(), 0, nullptr, s));
                       if (r) w.context->CopyResource(e.src.Get(), r.Get());
                       return r;
                   }}, "the sharpen written in place"});
    ms.push_back({{"strength halved", [](Env& e, float s) { return asTexture(edvrSharpen(e.src.Get(), 0, nullptr, s * 0.5f)); }},
                  "half the strength asked for"});
    ms.push_back({{"a wrapper that returns the resolve's own view", [](Env& e, float) { return e.src; }},
                  "the sharpening never applied"});
    for (const M& m : ms) {
        std::string why;
        const int bad = checkImpl(w, m.impl, false, &why);
        check(bad > 0, (std::string("control: ") + m.impl.name + " (" + m.named + ") FAILS the checker").c_str());
        std::printf("  control: %-46s -> %d checks failed; first: %s\n", m.impl.name, bad, why.c_str());
    }
}

// ---------------------------------------------------------------------------
// TWO DEVICES. The game can recreate its D3D11 device in a running process, and VR's native
// provider acquires again on the new one (RC4 review, F5). What the pass made on the first device
// cannot be used on the second: a view over a texture another device made is refused, which stood
// the flat sharpening down for the rest of the session, and a shader, buffer or UAV from one device
// bound on another's context is no better. The pass now releases everything it holds and makes it
// again when a frame arrives from another device. The wrapper-only two-device case in
// flat_sharpen_test stubs the pass out and cannot see this; these run the real one.
//
// Every scenario opens its own log, so the line counts are exact, and starts the pass fresh:
// sharpenPassDeviceResetOffForTest(false) drops whatever it holds.

ID3D11Device* deviceOfTexture(void* texture) {
    ComPtr<ID3D11Device> d;
    if (texture) static_cast<ID3D11Texture2D*>(texture)->GetDevice(&d);
    return d.Get();
}

ID3D11Device* deviceOfView(ID3D11ShaderResourceView* view) {
    ComPtr<ID3D11Device> d;
    if (view) view->GetDevice(&d);
    return d.Get();
}

void freshPass() {
    auto& cfg = edvr::Config::get();
    cfg.set("fix.render_sharpness", "0.6");
    cfg.set("fix.temporal_aa", "taa");
    edvr::sharpenPassDeviceResetOffForTest(false);
    edvr::flatSharpenReset();
}

// A, then B, then A again, through the real pass and through the wrapper with its own state carried
// from one device to the other the way the flat runtime carries it (no reset between). Every result is
// correct against the RCAS reference, is made on the device its frame came from, and nothing stands
// down. Four device changes in all (A to B, B to A, A to B, B to A), each one said.
void deviceChange(Warp& a, Warp& b) {
    freshPass();
    auto devA = a.device.Get();
    auto devB = b.device.Get();
    const Impl real{"the real pass", [](Env& e, float s) { return asTexture(edvrSharpen(e.src.Get(), 0, nullptr, s)); }};
    std::string why;
    checkImpl(a, real, true, &why);
    // Something in every kind of slot A can hold: eye 1 through the copy-through path (a source that
    // refuses a shader view), on top of eye 0's direct view and result.
    Env copySource;
    copySource.bytes = testImage();
    copySource.src = makeTexture(devA, W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, 0, &copySource.bytes);
    check(copySource.src && edvrSharpen(copySource.src.Get(), 1, nullptr, 0.6f) != nullptr,
          "setup: eye 1 sharpens a source that refuses a shader view, through a copy");
    edvr::SharpenPassHeld onA;
    edvr::sharpenPassHeldForTest(&onA);
    check(onA.owner == devA && onA.shader == devA && onA.buffer == devA && onA.eyeOut[0] == devA &&
              onA.eyeSrcView[0] == devA && onA.eyeOut[1] == devA && onA.eyeCopy[1] == devA,
          "setup: everything the pass holds -- shader, buffer, both eyes' results, eye 0's view, eye 1's copy -- was made on A");
    check(edvr::gpuTimingOwns(a.context.Get()) && !edvr::gpuTimingOwns(b.context.Get()) && onA.queriesInFlight >= 1,
          "setup: the timing domain is A's, and the price ring holds a query in flight on A");
    {
        Env eb = makeEnv(devB);
        void* r = edvrSharpen(eb.src.Get(), 0, nullptr, 0.6f);
        check(r != nullptr, "device B: the pass sharpens a frame from a second device");
        const bool onB = r && deviceOfTexture(r) == devB;
        check(onB, "...into a texture made on that device, not on the first one");
        // Every kind of thing the pass held on A is gone or made again on B, none left behind.
        edvr::SharpenPassHeld held;
        edvr::sharpenPassHeldForTest(&held);
        check(held.owner == devB, "after B's first frame the pass works on B");
        check(held.shader == devB && held.shaderTried, "...its shader was made again, on B");
        check(held.buffer == devB, "...its parameter buffer was made again, on B");
        check(held.eyeOut[0] == devB && held.eyeSrcView[0] == devB, "...eye 0's result and its view over the source are B's");
        check(!held.eyeOut[1] && !held.eyeSrcView[1] && !held.eyeCopy[1],
              "...eye 1's result, view and copy-through texture, unused on B, are released and not left on A");
        check(held.formatSupportAsks == onA.formatSupportAsks + 1, "...B was asked what formats it can store");
        check(held.queriesInFlight == 0, "...and the price ring's queries on A are released");
        if (onB) checkImpl(b, real, true, &why);   // against the reference, read back on B
    }
    checkImpl(a, real, true, &why);                // and back: A's own state is made again
    {
        edvr::SharpenPassHeld held;
        edvr::sharpenPassHeldForTest(&held);
        check(held.owner == devA && held.shader == devA && held.buffer == devA && held.eyeOut[0] == devA &&
                  held.eyeSrcView[0] == devA && !held.eyeOut[1] && !held.eyeCopy[1],
              "back on A: everything the pass holds was made on A, and nothing is left from B");
    }

    Env ea = makeEnv(devA);
    Env eb = makeEnv(devB);
    ComPtr<ID3D11ShaderResourceView> aIn = makeSrv(devA, ea.src.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    ComPtr<ID3D11ShaderResourceView> bIn = makeSrv(devB, eb.src.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    edvr::flatSharpenReset();
    // A returned view is the wrapper's own and good until its next call (a device change releases the
    // old device's), so each is read where it is returned.
    ID3D11ShaderResourceView* aOut = edvr::flatSharpenView(a.context.Get(), aIn.Get());
    const bool aSharpened = aOut != aIn.Get() && deviceOfView(aOut) == devA;
    ID3D11ShaderResourceView* bOut = edvr::flatSharpenView(b.context.Get(), bIn.Get());
    const bool bSharpened = bOut != bIn.Get() && deviceOfView(bOut) == devB;
    ID3D11ShaderResourceView* aBack = edvr::flatSharpenView(a.context.Get(), aIn.Get());
    const bool aBackSharpened = aBack != aIn.Get() && deviceOfView(aBack) == devA;
    check(aSharpened, "wrapper, device A: a sharpened view, made on A");
    check(bSharpened, "wrapper, device B after A: a sharpened view, made on B (it stood down here before the fix)");
    check(aBackSharpened, "wrapper, back on device A: sharpened on A again");
    const edvr::FlatSharpenCounts c = edvr::flatSharpenCounts();
    check(c.sharpened == 3 && c.refusals == 0 && !c.stoodDown && c.passedStoodDown == 0,
          "the wrapper counted three sharpened frames across the changes: no refusal, no stand-down");
    edvr::sharpenPassDeviceResetOffForTest(false);
}

void deviceChangeAssertions(const std::string& log) {
    check(countLines(log, "the D3D device changed (change ") == 4,
          "log: the four device changes are each said");
    check(countLines(log, "render_sharpen_cs created") == 5,
          "log: the shader is made once on the first device and once per change, and not again while the device stays");
    check(countLines(log, "stands down") == 0 && countLines(log, "standing down") == 0,
          "log: nothing stood down");
    for (const std::string& line : linesWith(log, "the D3D device changed")) std::printf("  log: %s\n", line.c_str());
}

// Two devices trading frames must not fill a log: the first four changes are said and the fourth
// says the rest are not. Six frames from B, A, B, A, B, A: the first adopts, five changes follow.
void deviceChangeNotes(Warp& a, Warp& b) {
    freshPass();
    Env ea = makeEnv(a.device.Get());
    Env eb = makeEnv(b.device.Get());
    int made = 0, wrong = 0;
    for (int i = 0; i < 6; ++i) {
        const bool onA = (i & 1) != 0;
        void* r = edvrSharpen((onA ? ea : eb).src.Get(), 0, nullptr, 0.6f);
        made += r != nullptr;
        wrong += r && deviceOfTexture(r) != (onA ? a.device.Get() : b.device.Get());
    }
    check(made == 6 && wrong == 0, "six alternating frames are each sharpened on their own device");
    edvr::sharpenPassDeviceResetOffForTest(false);
}

void deviceChangeNotesAssertions(const std::string& log) {
    check(countLines(log, "the D3D device changed (change ") == 4 && countLines(log, "Further changes are not said") == 1,
          "log: five changes say four lines, the fourth ending the notes");
}

// The tick's warm compile is on a device too. A tick on A warms the shader on A and adopts A, so a
// frame from B is a device change: released, said, and the shader made again on B. (A pass that did
// not adopt A at the tick would take B's first frame for the first device it saw, and run A's warm
// shader on it.)
void tickAdoptsItsDevice(Warp& a, Warp& b) {
    freshPass();
    edvr::sharpenPassConfigure(edvr::Config::get());
    edvr::sharpenPassTick(a.context.Get());
    Env eb = makeEnv(b.device.Get());
    void* r = edvrSharpen(eb.src.Get(), 0, nullptr, 0.6f);
    check(r != nullptr && deviceOfTexture(r) == b.device.Get(), "a frame from B after a tick on A is sharpened on B");
    edvr::sharpenPassDeviceResetOffForTest(false);
}

void tickAssertions(const std::string& log) {
    check(countLines(log, "the D3D device changed (change 1)") == 1 && countLines(log, "render_sharpen_cs created") == 2,
          "log: the tick's device is the pass's, so B's frame is a change and the shader is made again on B");
}

// CONTROL: the reset switched off is the pass as it was before it existed. The pass keeps A's
// resources for B's frame, hands B a texture made on A, the wrapper cannot make a view over it on B,
// and stands the flat sharpening down for the session: the review's probe, line for line. If this
// ever passes with the reset on, the checks above prove nothing.
void deviceResetControl(Warp& a, Warp& b) {
    freshPass();
    edvr::sharpenPassDeviceResetOffForTest(true);
    Env ea = makeEnv(a.device.Get());
    Env eb = makeEnv(b.device.Get());
    ComPtr<ID3D11ShaderResourceView> aIn = makeSrv(a.device.Get(), ea.src.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    ComPtr<ID3D11ShaderResourceView> bIn = makeSrv(b.device.Get(), eb.src.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    ID3D11ShaderResourceView* aOut = edvr::flatSharpenView(a.context.Get(), aIn.Get());
    check(aOut != aIn.Get() && deviceOfView(aOut) == a.device.Get(), "control setup: the first device is sharpened as ever");
    void* direct = edvrSharpen(eb.src.Get(), 0, nullptr, 0.6f);
    check(direct != nullptr && deviceOfTexture(direct) == a.device.Get(),
          "control: without the reset the pass hands device B a texture made on device A");
    ID3D11ShaderResourceView* bOut = edvr::flatSharpenView(b.context.Get(), bIn.Get());
    check(bOut == bIn.Get(), "control: ...so device B's frame goes through unsharpened");
    const edvr::FlatSharpenCounts c = edvr::flatSharpenCounts();
    check(c.stoodDown && c.refusals == 1 && c.sharpened == 1,
          "control: ...and the wrapper stands the sharpening down for the session (sharpened 1, refusals 1, stood down)");
    std::printf("  control: reset off -> device B: %s; counts: sharpened=%llu refusals=%llu stoodDown=%u\n",
                bOut == bIn.Get() ? "PASS_THROUGH" : "SHARPENED_VIEW",
                static_cast<unsigned long long>(c.sharpened), static_cast<unsigned long long>(c.refusals),
                c.stoodDown ? 1u : 0u);
    edvr::sharpenPassDeviceResetOffForTest(false);   // back on, from a clean start
    edvr::flatSharpenReset();
}

// The shutdown at the end of the main log's scenario let go of everything: the shader, the buffer,
// both eyes' textures and views, the price ring, and the reference on the device it worked on.
void shutdownReleasedEverything() {
    edvr::SharpenPassHeld held;
    edvr::sharpenPassHeldForTest(&held);
    check(!held.owner && !held.shader && !held.buffer && !held.eyeOut[0] && !held.eyeOut[1] && !held.eyeSrcView[0] &&
              !held.eyeSrcView[1] && !held.eyeCopy[0] && !held.eyeCopy[1] && held.queriesInFlight == 0,
          "shutdown released everything the pass held, and its reference on the device");
}

void logAssertions(const std::string& log) {
    check(countLines(log, "render sharpening: first sharpened frame") == 1 &&
              countLines(log, "before the game's own output copy") >= 1 &&
              countLines(log, "the last pass before the frame leaves") == 0,
          "log: the first sharpened frame is said once, in the flat profile's words");
    check(countLines(log, "first sharpened frame pays no compile") == 1,
          "log: the warm-up line says frame, not eye");
    check(countLines(log, "anti-aliasing is off (fix.temporal_aa)") == 1 && countLines(log, "openvr_api.dll") == 0 &&
              countLines(log, "compositor hook") == 0,
          "log: never-ran, said once, names the anti-aliasing and no compositor hook");
    check(countLines(log, "render sharpening totals:") >= 1 && countLines(log, "frames sharpened this session") >= 1 &&
              countLines(log, "eye-submits") == 0,
          "log: the totals count frames");
    // The cost line: once mid-run, after 120 timed passes (with the size and strength), and
    // again at shutdown; both say per frame.
    check(countLines(log, "render sharpening: measured") >= 1 && countLines(log, "ms per frame on average") >= 1 &&
              countLines(log, "ms per eye") == 0,
          "log: the cost line says per frame");
    check(countLines(log, "at 64x48 -- one dispatch of AMD's RCAS at strength 0.30") == 1,
          "log: the cost line names the size and the strength after 120 timed passes");
    for (const char* needle : {"render sharpening:", "render sharpening totals:", "flat sharpen:"}) {
        for (const std::string& line : linesWith(log, needle)) std::printf("  log: %s\n", line.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(3);
    // Unbuffered, so a crash does not take the output with it: the scenarios that mix two devices
    // are the ones most likely to fault, and a run that dies should say how far it got.
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("flat_sharpen_pass_test: dry-run (no device, no files)");
        return 0;
    }
    const bool working = argc == 2 && !std::strcmp(argv[1], "--self-test-working");
    if (argc != 2 || (!working && std::strcmp(argv[1], "--self-test"))) {
        std::puts("usage: flat_sharpen_pass_test --dry-run | --self-test | --self-test-working");
        return 2;
    }
    Warp w = makeWarp();
    if (!w.ok) return 1;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    if (working) {
        const std::string log = withLog(L"flatsharpenwork", [&] { workingSession(w); });
        workingAssertions(log);
        std::printf("flat_sharpen_pass_test (working session): %u checks, %u failures\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    neverRanWording();
    const std::string log = withLog(L"flatsharpenpass", [&] {
        neverRan(w);      // first: nothing has been sharpened yet, which is the point
        firstFrames(w);
        golden(w);
        mutants(w);
        edvr::sharpenPassNoteTotals();
        edvr::sharpenPassShutdown();
    });
    logAssertions(log);
    shutdownReleasedEverything();

    // Two devices in one process: the real pass on each in turn, the wrapper carried across, the
    // tick's warm compile, the cap on the notes, and the control with the reset switched off.
    Warp b = makeWarp();
    const bool twoDevices = b.ok && b.device.Get() != w.device.Get();
    check(twoDevices, "a second, distinct WARP device");
    if (twoDevices) {
        std::puts("  two devices: A, B, A through the real pass and through the wrapper");
        deviceChangeAssertions(withLog(L"flatsharpendev", [&] { deviceChange(w, b); }));
        std::puts("  two devices: the cap on the change notes");
        deviceChangeNotesAssertions(withLog(L"flatsharpennotes", [&] { deviceChangeNotes(w, b); }));
        std::puts("  two devices: the tick's warm compile");
        tickAssertions(withLog(L"flatsharpentick", [&] { tickAdoptsItsDevice(w, b); }));
        std::puts("  two devices: the control, reset off");
        const std::string controlLog = withLog(L"flatsharpencontrol", [&] { deviceResetControl(w, b); });
        for (const std::string& line : linesWith(controlLog, "flat sharpen:")) std::printf("  log (control): %s\n", line.c_str());
    }
    std::printf("flat_sharpen_pass_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
