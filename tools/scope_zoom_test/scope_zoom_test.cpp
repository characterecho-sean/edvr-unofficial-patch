// The scope lens, in the build's gate (docs/design-vr-scope-magnification-2026-10-28.md, section 8).
//
//   THE WIRING (source scans, each with a control). The lens at the door right after the shield ripple and before the weapons/HUD composite; the temporal pass clearing and setting
//   the eye's depth around the view it makes for the upscaler; the frame thread's tick after S0's and the publish; the two keys read by their literal names; the ini rows.
//   THE PASSES (WARP, the debug layer on). The production door (src/d3d11/scope_zoom.cpp linked as shipped, the build's bytecode, the production parameter blocks) on a synthetic
//   frame whose depth is a near hexagonal ring about c, far inside and out: inside the ring the frame is sampled at c + d / M (Catmull-Rom, within one code of the reference
//   built from scope_zoom_s1.h's own functions); the ring and everything outside it are not touched, byte for byte; the mask's edge lies on the ring's inner edge to 4 output px; a
//   ring with a two-ray gap still has a lens; an open ring (no frame) and a frame whose format is not R8G8B8A8 or B8G8R8A8 UNORM are left alone and counted; a frame EDVR may
//   not write comes back as the lens's own copy with the original untouched; a stale c, no depth, a picture that is not the upscaler's, a gate verdict that is not a lens and
//   M = 1 are each counted under their own reason and leave the frame alone. The rays read back give the hole the instrument reports, which agrees with the reference march.
//   Mutants of the comparator (M off by 20 %, c off by 4 px, the hole 20 % small) each fail the check that passes the real thing.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/scope_zoom.h"
#include "../../src/d3d11/scope_zoom_s1.h"
#include "../../src/d3d11/scope_zoom_shaders.h"
#include "../../src/d3d11/shader_swap.h"

using namespace edvr;
using Microsoft::WRL::ComPtr;
namespace sz = edvr::scopezoom;

static unsigned g_checks = 0;
static void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
}
static void hr(HRESULT r, const char* what) { check(SUCCEEDED(r), what); }
static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ============================================================================================ production's neighbours, stubbed (as the UI layer rigs stub theirs)
namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
bool gpuTimingBind(ID3D11Device*, ID3D11DeviceContext*, const GpuSpanD3D11Ops&) noexcept { return false; }
bool gpuTimingAccepts(ID3D11DeviceContext*) noexcept { return false; }
bool gpuTimingOwns(ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::begin(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::beginBorrowedFrame(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::end(ID3D11DeviceContext*) noexcept { return false; }
GpuTimerPoll GpuTimer::poll(ID3D11DeviceContext*, double&) noexcept { return GpuTimerPoll::Pending; }
void GpuTimer::reset(ID3D11DeviceContext*) noexcept {}
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11ComputeShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreateComputeShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
}  // namespace edvr

// ============================================================================================ the wiring
static bool has(const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; }
static bool before(const std::string& t, const std::string& a, const std::string& b) {
    const size_t pa = t.find(a), pb = t.find(b);
    return pa != std::string::npos && pb != std::string::npos && pa < pb;
}
static std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    check(at != std::string::npos, "a mutation's text is in the source it mutates");
    text.replace(at, from.size(), to);
    return text;
}
// The door: inside uiLayerComposite, the lens (scopeZoomDoor) after the ripple's door and before the weapons/HUD composite's first return, for the right eye only.
static bool wiredDoor(const std::string& layer) {
    const size_t fn = layer.find("ID3D11Texture2D* uiLayerComposite(uint64_t sequence, uint32_t eye");
    if (fn == std::string::npos) return false;
    const std::string body = layer.substr(fn, 9000);
    return before(body, "rippleDoor(e, eye, sequence", "scopeZoomDoor(lensArgs") && before(body, "scopeZoomDoor(lensArgs", "if (!e.srv || !e.rtv || e.compositedSeq == sequence) return handOn();") &&
           before(body, "scopeZoomDoor(lensArgs", "compositeInner(e, eye, frame") && has(body, "if (eye == 1 && scopeZoomWanted())") && has(body, "lensArgs.resolved = e.doorFromPass;");
}
static bool wiredTemporal(const std::string& t) {
    return before(t, "scopeZoomDepthClear(eye);", "depthProbeSceneDepth(sd.Width, sd.Height, eye, &dtex)") && before(t, "depthProbeSceneDepth(sd.Width, sd.Height, eye, &dtex)", "scopeZoomDepthSet(eye, depthSrv") &&
           before(t, "scopeZoomDepthSet(eye, depthSrv", "uiDepthSmokeDepth(sd.Width, sd.Height, eye, &smokeSrv)");
}
static bool wiredFrame(const std::string& n) {
    return before(n, "s0::vrTick(GetTickCount64());", "s1::tick(GetTickCount64());") && has(n, "scopeZoomPublish(out);") && has(n, "scopeZoomPressBegin(s.press);") && has(n, "scopeZoomPressTake(&pl.d);") &&
           has(n, "cfg.getString(\"fix.native_on_foot_ads_scope_zoom\", \"off\")") && has(n, "cfg.getFloat(\"experimental.native_on_foot_ads_scope_zoom_fov\"") &&
           has(n, "sz::lensGate(gi)") && has(n, "nativeTemporalDrawJitter(1, &jseq");
}
static bool wiredIni(const std::string& ini) {
    const size_t fixAt = ini.find("native_on_foot_ads_scope_zoom = off"), expAt = ini.find("native_on_foot_ads_scope_zoom_fov = 56.3");
    return fixAt != std::string::npos && expAt != std::string::npos && fixAt < expAt && has(ini, "# ui: ADS: scope zoom | choices on, off | recommended off | live | menu native_on_foot") &&
           has(ini, "# ui: ADS: scope zoom FOV (experimental) | range 30..90 | step 0.5 | unit deg | recommended 56.3 | live") && before(ini, "[experimental]", "native_on_foot_ads_scope_zoom_fov = 56.3");
}
static void wiringTests() {
    const std::string layer = readFile("src/d3d11/ui_layer.cpp"), temporal = readFile("src/d3d11/temporal_pass.cpp"), native = readFile("src/d3d11/native_on_foot.cpp"), ini = readFile("edvr.ini");
    check(!layer.empty() && !temporal.empty() && !native.empty() && !ini.empty(), "the sources the wiring scans read are readable from the repository root");
    check(wiredDoor(layer), "the lens is at the door, after the ripple, before the weapons/HUD composite, for the right eye, over the upscaler's picture");
    check(!wiredDoor(replaced(layer, "if (eye == 1 && scopeZoomWanted())", "if (eye == 0 && scopeZoomWanted())")), "control: the lens in the left eye is not the wiring");
    check(!wiredDoor(replaced(layer, "lensArgs.resolved = e.doorFromPass;", "lensArgs.resolved = true;")), "control: a lens over a picture that is not the upscaler's is not the wiring");
    check(!wiredDoor(replaced(layer, "scopeZoomDoor(lensArgs, &lensCopy);", "scopeZoomDoorOff(a, &lensCopy);")), "control: a door call to something else is not the wiring");
    check(wiredTemporal(temporal), "the temporal pass clears the eye's depth slot at the top of its depth step and sets it from the view it made for the upscaler");
    check(!wiredTemporal(replaced(temporal, "scopeZoomDepthClear(eye);", "")), "control: no clear is not the wiring");
    check(wiredFrame(native), "the frame tick follows S0's, publishes, takes the door's counters at the press's end, and the two keys are read by their literal names");
    check(!wiredFrame(replaced(native, "s1::tick(GetTickCount64());", "")), "control: no tick is not the wiring");
    check(!wiredFrame(replaced(native, "cfg.getString(\"fix.native_on_foot_ads_scope_zoom\", \"off\")", "cfg.getString(\"fix.native_on_foot_ads_zoom\", \"off\")")), "control: a key read by another name is not the wiring");
    check(wiredIni(ini), "the ini states both keys with their rows: the user key on the Native on foot page, the experimental one in the developer tier");
    check(!wiredIni(replaced(ini, "native_on_foot_ads_scope_zoom = off", "native_on_foot_ads_scope_zoom = on")), "control: the user key on by default is not the shipped default");
}

// ============================================================================================ the scene
namespace {
constexpr uint32_t kRenW = 256, kRenH = 256, kOutW = 512, kOutH = 512;
constexpr float kTan4[4] = {-0.5f, 0.5f, -0.5f, 0.5f};     // 256 render px a unit of tangent: 12 degrees is 54 render px
constexpr double kNearDepth = 0.0417, kNearD = 0.14, kFarD = 0.00122;
constexpr double kCRen[2] = {70.3, 65.6};                   // c in render px, as the game drew it (the jitter in)
constexpr double kJitterPx[2] = {0.25, -0.125};            // the draw's jitter in render px
constexpr double kScale = 2.0;                              // output px per render px

struct Hex {
    double hx, hy, a, w, rot;
    double metric(double x, double y) const {
        double m = -1.0e30;
        for (int i = 0; i < 6; ++i) {
            const double ang = rot + i * (sz::kPi / 3.0);
            m = std::fmax(m, (x - hx) * std::cos(ang) + (y - hy) * std::sin(ang));
        }
        return m;
    }
    bool frame(double x, double y) const { const double m = metric(x, y); return m >= a && m <= a + w; }
    double cast(double cx, double cy, double dx, double dy) const {
        double t = 1.0e30;
        for (int i = 0; i < 6; ++i) {
            const double ang = rot + i * (sz::kPi / 3.0);
            const double nx = std::cos(ang), ny = std::sin(ang);
            const double along = nx * dx + ny * dy;
            if (along <= 1.0e-12) continue;
            t = std::fmin(t, (a - ((cx - hx) * nx + (cy - hy) * ny)) / along);
        }
        return t;
    }
};
const Hex kHex{73.0, 63.0, 32.0, 7.0, 0.2};

struct Rgba { uint8_t r, g, b, a; };
Rgba rampAt(int x, int y) { return Rgba{static_cast<uint8_t>(x & 255), static_cast<uint8_t>(y & 255), static_cast<uint8_t>(((x >> 8) * 80 + (y >> 8) * 40) & 255), 255}; }
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<Rgba> px;   // semantic RGBA, whatever the texture's byte order
    Rgba& at(int x, int y) { return px[static_cast<size_t>(y) * w + x]; }
    const Rgba& at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};
Image makeOrig() {
    Image im;
    im.w = kOutW;
    im.h = kOutH;
    im.px.resize(static_cast<size_t>(kOutW) * kOutH);
    for (uint32_t y = 0; y < kOutH; ++y)
        for (uint32_t x = 0; x < kOutW; ++x) im.at(static_cast<int>(x), static_cast<int>(y)) = rampAt(static_cast<int>(x), static_cast<int>(y));
    return im;
}

struct Scene {
    bool ring = true;
    int gapRay = -1;                           // two rays wide: the frame is cut away between rays gapRay and gapRay + 1
    std::vector<float> depth = std::vector<float>(static_cast<size_t>(kRenW) * kRenH);
    double depthAt(int x, int y) const { return depth[static_cast<size_t>(y) * kRenW + x]; }
};
Scene makeScene(bool ring, int gapRay) {
    Scene s;
    s.ring = ring;
    s.gapRay = gapRay;
    const double gapLo = gapRay >= 0 ? (gapRay * 2.0 * sz::kPi / sz::kLensRays - 1.0 / sz::kDeg) : 0.0, gapHi = gapRay >= 0 ? ((gapRay + 1) * 2.0 * sz::kPi / sz::kLensRays + 1.0 / sz::kDeg) : 0.0;
    for (int y = 0; y < static_cast<int>(kRenH); ++y)
        for (int x = 0; x < static_cast<int>(kRenW); ++x) {
            bool isNear = ring && kHex.frame(x + 0.5, y + 0.5);
            if (isNear && gapRay >= 0) {
                double ang = std::atan2(y + 0.5 - kCRen[1], x + 0.5 - kCRen[0]);
                if (ang < 0.0) ang += 2.0 * sz::kPi;
                if (ang >= gapLo && ang <= gapHi) isNear = false;
            }
            s.depth[static_cast<size_t>(y) * kRenW + x] = static_cast<float>(isNear ? kNearD : kFarD);
        }
    return s;
}

// The reference image of the lens, from scope_zoom_s1.h's own functions over the reference march: the picture in `orig`, magnified by m about c (output px), inside r(theta).
struct Expected {
    Image img;
    std::vector<double> edge;   // output px inside the hole's edge (negative outside), per pixel
};
Rgba catmullAt(const Image& im, double sx, double sy) {
    const double fx = sx - 0.5, fy = sy - 0.5;
    const int bx = static_cast<int>(std::floor(fx)), by = static_cast<int>(std::floor(fy));
    double wx[4], wy[4];
    sz::lensCatmullRom(fx - bx, wx);
    sz::lensCatmullRom(fy - by, wy);
    double acc[3] = {0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            const int x = std::min(std::max(bx - 1 + i, 0), static_cast<int>(im.w) - 1), y = std::min(std::max(by - 1 + j, 0), static_cast<int>(im.h) - 1);
            const Rgba p = im.at(x, y);
            const double w = wx[i] * wy[j];
            acc[0] += w * p.r;
            acc[1] += w * p.g;
            acc[2] += w * p.b;
        }
    auto q = [](double v) { return static_cast<uint8_t>(std::min(std::max(std::lround(v), 0L), 255L)); };
    return Rgba{q(acc[0]), q(acc[1]), q(acc[2]), 255};
}
Expected expectedLens(const Scene& sc, const Image& orig, double m, double cx, double cy, double holeScale, double crOffX) {
    Expected e;
    e.img = orig;
    e.edge.assign(orig.px.size(), -1.0e9);
    sz::LensRay rays[sz::kLensRays];
    for (uint32_t k = 0; k < sz::kLensRays; ++k)
        rays[k] = sz::lensMarchRef([&](int x, int y) { return sc.depthAt(x, y); }, kRenW, kRenH, kCRen[0], kCRen[1], k, kTan4, kNearDepth);
    float r[sz::kLensRays];
    sz::lensClampEscaped(rays, r);
    for (uint32_t k = 0; k < sz::kLensRays; ++k) r[k] = static_cast<float>(r[k] * holeScale);
    for (int y = 0; y < static_cast<int>(orig.h); ++y)
        for (int x = 0; x < static_cast<int>(orig.w); ++x) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            const double dRx = dx / kScale, dRy = dy / kScale;
            const double rr = std::sqrt(dRx * dRx + dRy * dRy);
            if (rr > 120.0) continue;
            const double inside = (sz::lensRadiusAt(r, std::atan2(dRy, dRx)) - rr) * kScale;
            e.edge[static_cast<size_t>(y) * orig.w + x] = inside;
            const double w = sz::lensWeight(inside);
            if (w <= 0.0) continue;
            const double c2[2] = {cx + crOffX, cy}, p[2] = {x + 0.5, y + 0.5};
            double s[2];
            sz::lensSourceOf(c2, m, p, s);
            const Rgba mag = catmullAt(orig, s[0], s[1]), o = orig.at(x, y);
            auto mix = [&](uint8_t a, uint8_t b) { return static_cast<uint8_t>(std::lround(a + (b - a) * w)); };
            e.img.at(x, y) = Rgba{mix(o.r, mag.r), mix(o.g, mag.g), mix(o.b, mag.b), 255};
        }
    return e;
}
// The comparator: pixels well inside the hole within `tol` codes of the reference, pixels outside it (and the ring) identical to the original. Counts the pixels that disagree.
struct Verdict { unsigned inside = 0, outside = 0, insideBad = 0, outsideBad = 0, insideChanged = 0; };
Verdict compare(const Image& got, const Image& orig, const Expected& e, int tol) {
    Verdict v;
    for (size_t i = 0; i < got.px.size(); ++i) {
        const double edge = e.edge[i];
        const Rgba g = got.px[i], o = orig.px[i], x = e.img.px[i];
        if (edge >= 4.0) {
            ++v.inside;
            if (std::abs(g.r - x.r) > tol || std::abs(g.g - x.g) > tol || std::abs(g.b - x.b) > tol) ++v.insideBad;
            if (g.r != o.r || g.g != o.g || g.b != o.b) ++v.insideChanged;
        } else if (edge <= -4.0) {
            ++v.outside;
            if (g.r != o.r || g.g != o.g || g.b != o.b || g.a != o.a) ++v.outsideBad;
        }
    }
    return v;
}

struct Fx {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    bool debugLayer = false;
    bool bgraStore = false;
    explicit Fx(bool hardware) {
        D3D_FEATURE_LEVEL level;
        const D3D_DRIVER_TYPE type = hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        UINT flags = hardware ? 0 : D3D11_CREATE_DEVICE_DEBUG;
        HRESULT r = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        if (FAILED(r)) {
            flags = 0;
            r = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        }
        hr(r, "a WARP device");
        debugLayer = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
        if (debugLayer) dev.As(&info);
        UINT s = 0;
        bgraStore = SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &s)) && (s & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
        UINT s2 = 0;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2{DXGI_FORMAT_B8G8R8A8_UNORM, 0};
        if (bgraStore && SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof(fs2)))) {
            s2 = fs2.OutFormatSupport2;
            bgraStore = (s2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        }
    }
    ComPtr<ID3D11Texture2D> frameTex(DXGI_FORMAT fmt, const Image& im, UINT bind) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kOutW; d.Height = kOutH; d.MipLevels = d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        ComPtr<ID3D11Texture2D> t;
        hr(dev->CreateTexture2D(&d, nullptr, &t), "the rig's frame");
        upload(t.Get(), fmt, im);
        return t;
    }
    void upload(ID3D11Texture2D* t, DXGI_FORMAT fmt, const Image& im) {
        std::vector<uint8_t> bytes(static_cast<size_t>(im.w) * im.h * 4);
        for (size_t i = 0; i < im.px.size(); ++i) {
            const Rgba p = im.px[i];
            const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
            bytes[4 * i + 0] = bgra ? p.b : p.r;
            bytes[4 * i + 1] = p.g;
            bytes[4 * i + 2] = bgra ? p.r : p.b;
            bytes[4 * i + 3] = p.a;
        }
        ctx->UpdateSubresource(t, 0, nullptr, bytes.data(), im.w * 4, 0);
    }
    Image read(ID3D11Texture2D* t, DXGI_FORMAT fmt) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> stage;
        hr(dev->CreateTexture2D(&d, nullptr, &stage), "staging");
        ctx->CopyResource(stage.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map");
        Image im;
        im.w = d.Width;
        im.h = d.Height;
        im.px.resize(static_cast<size_t>(im.w) * im.h);
        for (uint32_t y = 0; y < im.h; ++y)
            for (uint32_t x = 0; x < im.w; ++x) {
                const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + 4 * x;
                const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
                im.at(static_cast<int>(x), static_cast<int>(y)) = Rgba{bgra ? p[2] : p[0], p[1], bgra ? p[0] : p[2], p[3]};
            }
        ctx->Unmap(stage.Get(), 0);
        return im;
    }
    // The scene's depth as the game's own texture: the depth channel of R32G8X24_TYPELESS, viewed R32_FLOAT_X8X24_TYPELESS (as temporal_pass.cpp makes it).
    ComPtr<ID3D11ShaderResourceView> depthSrv(const Scene& sc, ComPtr<ID3D11Texture2D>* keep) {
        std::vector<uint32_t> raw(static_cast<size_t>(kRenW) * kRenH * 2, 0);
        for (size_t i = 0; i < sc.depth.size(); ++i) std::memcpy(&raw[2 * i], &sc.depth[i], 4);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kRenW; d.Height = kRenH; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G8X24_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{raw.data(), kRenW * 8, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateTexture2D(&d, &sd, keep->ReleaseAndGetAddressOf()))) {   // the plain float depth view reads the same bytes the shader sees
            d.Format = DXGI_FORMAT_R32_FLOAT;
            v.Format = DXGI_FORMAT_R32_FLOAT;
            D3D11_SUBRESOURCE_DATA sd2{sc.depth.data(), kRenW * 4, 0};
            hr(dev->CreateTexture2D(&d, &sd2, keep->ReleaseAndGetAddressOf()), "the depth texture (R32_FLOAT)");
            std::puts("scope_zoom_test: this adapter would not make an R32G8X24_TYPELESS texture with initial data: the depth is R32_FLOAT here");
        }
        ComPtr<ID3D11ShaderResourceView> s;
        hr(dev->CreateShaderResourceView(keep->Get(), &v, &s), "the depth view");
        return s;
    }
};

struct Run {
    Image image;                  // the frame the lens left (or the copy it handed back)
    Image original;               // the frame texture after the lens (to see it was not written when it must not be)
    bool gotCopy = false;
    sz::LensDoorStats stats;
};
struct RunIn {
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool inPlace = true, resolved = true, hasDepth = true;
    sz::LensVerdict verdict = sz::LensVerdict::kQualifies;
    double m = 2.5;
    uint32_t sleepMs = 0;          // between the publish and the door: a c this old is stale
};
// One press as the production pieces see it: publish, door (with the frame re-uploaded before each call), the counters taken. A lens that runs is called until its rays have been
// read back (they come a few calls later); the picture compared is the last call's, which starts from the original like every other.
Run runDoor(Fx& f, const Scene& sc, const Image& orig, const RunIn& in) {
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11ShaderResourceView> srv = f.depthSrv(sc, &depthTex);
    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> frame = f.frameTex(in.fmt, orig, bind);
    scopeZoomDepthClear(1);
    if (in.hasDepth) scopeZoomDepthSet(1, srv.Get(), kRenW, kRenH, static_cast<float>(kNearDepth));
    static uint32_t press = 0;   // a press of its own each run: the rays still in flight from the run before belong to that one and are dropped
    ++press;
    scopeZoomPressBegin(press);
    ScopeZoomFrame rec;
    rec.verdict = static_cast<uint32_t>(in.verdict);
    rec.press = press;
    rec.m = static_cast<float>(in.m);
    rec.mTarget = static_cast<float>(in.m);
    rec.uv[0] = static_cast<float>(kCRen[0] / kRenW);
    rec.uv[1] = static_cast<float>(kCRen[1] / kRenH);
    rec.jitterUv[0] = static_cast<float>(kJitterPx[0] / kRenW);
    rec.jitterUv[1] = static_cast<float>(kJitterPx[1] / kRenH);
    std::memcpy(rec.tan4, kTan4, sizeof(kTan4));
    scopeZoomPublish(rec);
    if (in.sleepMs) Sleep(in.sleepMs);
    const bool wait = in.hasDepth && in.verdict == sz::LensVerdict::kQualifies && in.m >= 1.001 && !in.sleepMs && in.resolved;
    const uint32_t region[4] = {0, 0, kOutW, kOutH};
    ComPtr<ID3D11Texture2D> copy;
    Run out;
    for (uint32_t i = 0; i < (wait ? 400u : 1u); ++i) {
        if (i) scopeZoomPublish(rec);
        f.upload(frame.Get(), in.fmt, orig);
        ScopeZoomDoorArgs a;
        a.eye = 1;
        a.sequence = 100 + i;
        a.frame = frame.Get();
        a.region = region;
        a.inPlace = in.inPlace;
        a.resolved = in.resolved;
        copy.Reset();
        scopeZoomDoor(a, &copy);
        f.ctx->Flush();
        if (!wait) break;
        sz::LensDoorStats peek;
        scopeZoomPressTake(&peek);
        if (peek.raysRead >= 1) break;
        Sleep(1);
    }
    out.gotCopy = copy != nullptr;
    out.image = f.read(copy ? copy.Get() : frame.Get(), in.fmt);
    out.original = f.read(frame.Get(), in.fmt);
    scopeZoomPressTake(&out.stats);
    scopeZoomDepthClear(1);
    return out;
}
double cOutX() { return (kCRen[0] / kRenW - kJitterPx[0] / kRenW) * kOutW; }
double cOutY() { return (kCRen[1] / kRenH - kJitterPx[1] / kRenH) * kOutH; }
bool sameImage(const Image& a, const Image& b) { return a.w == b.w && a.h == b.h && a.px.size() == b.px.size() && std::memcmp(a.px.data(), b.px.data(), a.px.size() * sizeof(Rgba)) == 0; }

void lensTests(Fx& f) {
    const Image orig = makeOrig();
    const Scene ring = makeScene(true, -1);
    const double cx = cOutX(), cy = cOutY();
    check(std::fabs(cx - 140.1) < 1e-3 && std::fabs(cy - 131.45) < 1e-3, "the rig's c in output px is c less the jitter, scaled (140.10, 131.45)");

    // ---- the real thing, in place, RGBA and BGRA ----
    for (int pass = 0; pass < 2; ++pass) {
        const DXGI_FORMAT fmt = pass == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (pass == 1 && !f.bgraStore) {
            std::puts("scope_zoom_test: this adapter has no typed UAV store for B8G8R8A8_UNORM: that format's cases are skipped");
            continue;
        }
        RunIn in;
        in.fmt = fmt;
        const Run r = runDoor(f, ring, orig, in);
        const Expected e = expectedLens(ring, orig, in.m, cx, cy, 1.0, 0.0);
        const Verdict v = compare(r.image, orig, e, 1);
        std::printf("lens %s in place: %u px inside the hole (%u bad, %u changed), %u px outside it (%u bad)\n", pass == 0 ? "RGBA" : "BGRA", v.inside, v.insideBad, v.insideChanged, v.outside, v.outsideBad);
        check(!r.gotCopy && v.inside > 2000 && v.insideBad == 0 && v.outsideBad == 0, "inside the ring the frame is sampled at c + d / M (Catmull-Rom, within one code of the reference); the ring and everything outside are untouched");
        check(v.insideChanged * 10 > v.inside * 9, "and the lens really changed the picture over (nearly) all of the hole");
        check(r.stats.dispatched >= 1 && r.stats.noop == 0 && r.stats.staleC == 0 && r.stats.depthUnavailable == 0 && r.stats.formatRefused == 0 && r.stats.noS == 0, "the qualifying frames were dispatched and none was declined");
        // the mask's edge on the ring's inner edge: scan inward from outside along 24 directions for the first pixel the lens changed
        double worst = 0.0;
        for (int k = 0; k < 24; ++k) {
            const double phi = k * 2.0 * sz::kPi / 24.0 + 0.04, dx = std::cos(phi), dy = std::sin(phi);
            double found = -1.0;
            for (double t = 160.0; t > 6.0; t -= 0.5) {
                const int x = static_cast<int>(std::floor(cx + dx * t)), y = static_cast<int>(std::floor(cy + dy * t));
                if (x < 0 || y < 0 || x >= static_cast<int>(kOutW) || y >= static_cast<int>(kOutH)) continue;
                const Rgba g = r.image.at(x, y), o = orig.at(x, y);
                if (std::abs(g.r - o.r) + std::abs(g.g - o.g) >= 6) { found = t; break; }
            }
            const double exact = kHex.cast(kCRen[0], kCRen[1], dx, dy) * kScale;   // render px to output px
            check(found > 0.0, "the scan found the lens's edge");
            worst = std::fmax(worst, std::fabs(found - exact));
        }
        std::printf("lens edge: worst distance from the ring's inner edge %.2f output px\n", worst);
        check(worst <= 4.0, "the lens ends at the ring's inner edge, to 4 output px, in every direction scanned");
        if (pass == 0) {
            // the comparator's mutants: each must fail the check that passes the real thing
            const Verdict wrongM = compare(r.image, orig, expectedLens(ring, orig, in.m * 1.2, cx, cy, 1.0, 0.0), 1);
            const Verdict wrongC = compare(r.image, orig, expectedLens(ring, orig, in.m, cx, cy, 1.0, 4.0), 1);
            const Verdict tiny = compare(r.image, orig, expectedLens(ring, orig, in.m, cx, cy, 0.8, 0.0), 1);
            check(wrongM.insideBad > 100, "mutant: M off by 20 percent is caught");
            check(wrongC.insideBad > 100, "mutant: the source taken 4 px off c is caught");
            check(tiny.insideBad + tiny.outsideBad > 100, "mutant: the hole 20 percent too small is caught");
            // the rays read back: the hole the instrument reports against the reference march
            check(r.stats.raysRead >= 1, "the rays were read back");
            sz::LensRay rays[sz::kLensRays];
            sz::LensReadback ref;
            for (uint32_t k = 0; k < sz::kLensRays; ++k) {
                rays[k] = sz::lensMarchRef([&](int x, int y) { return ring.depthAt(x, y); }, kRenW, kRenH, kCRen[0], kCRen[1], k, kTan4, kNearDepth);
                ref.rDeg[k] = static_cast<float>(rays[k].rDeg);
            }
            ref.escaped = 0;
            double rmin, rmed, rmax;
            check(sz::lensHoleStats(ref, &rmin, &rmed, &rmax), "the reference march has a hole");
            std::printf("hole: GPU r min %.3f median %.3f max %.3f deg, %u escaped; reference %.3f %.3f %.3f\n", r.stats.rMinDeg, r.stats.rMedianDeg, r.stats.rMaxDeg, r.stats.holeEscaped, rmin, rmed, rmax);
            check(r.stats.holeKnown && r.stats.holeEscaped == 0 && std::fabs(r.stats.rMinDeg - rmin) < 0.02 && std::fabs(r.stats.rMedianDeg - rmed) < 0.02 && std::fabs(r.stats.rMaxDeg - rmax) < 0.02,
                  "the hole the GPU's rays give (min, median, max in degrees, none escaped) is the reference march's, to 0.02 degrees");
            check(std::fabs(r.stats.depthAtC - kFarD) < 1e-6 && std::fabs(r.stats.depthNear - kNearDepth) < 1e-6, "the depth at c (far, 0.00122) and the near line are what the line reports");
            check(r.stats.cN >= 1 && std::fabs(r.stats.cSumX / r.stats.cN - cx) < 1e-3 && std::fabs(r.stats.cSumY / r.stats.cN - cy) < 1e-3, "c's counted place is c in output px");
        }
    }

    // ---- a frame EDVR may not write: the lens's own copy comes back, the original is untouched ----
    {
        RunIn in;
        in.inPlace = false;
        const Run r = runDoor(f, ring, orig, in);
        const Verdict v = compare(r.image, orig, expectedLens(ring, orig, in.m, cx, cy, 1.0, 0.0), 1);
        check(r.gotCopy && v.insideBad == 0 && v.outsideBad == 0 && v.insideChanged * 10 > v.inside * 9, "a frame that is not EDVR's to rewrite comes back as the lens's copy, magnified as in place");
        check(sameImage(r.original, orig), "and the original frame is not written");
    }

    // ---- a ring with a two-ray gap still has a lens, the gap clamped to its neighbours ----
    {
        const Scene gap = makeScene(true, 20);
        uint32_t esc = 0;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) esc += sz::lensMarchRef([&](int x, int y) { return gap.depthAt(x, y); }, kRenW, kRenH, kCRen[0], kCRen[1], k, kTan4, kNearDepth).escaped ? 1u : 0u;
        check(esc >= 1 && esc <= 2, "the reference march sees the gap as one or two escaped rays, not an open sight");
        RunIn in;
        const Run r = runDoor(f, gap, orig, in);
        const Verdict v = compare(r.image, orig, expectedLens(gap, orig, in.m, cx, cy, 1.0, 0.0), 1);
        std::printf("lens with a gap (%u rays escaped): %u px inside (%u bad), %u outside (%u bad); GPU escaped %u\n", esc, v.inside, v.insideBad, v.outside, v.outsideBad, r.stats.holeEscaped);
        check(v.insideBad == 0 && v.outsideBad == 0 && v.insideChanged * 10 > v.inside * 9, "a gap in the ring still has its lens, to the reference built with the escaped rays clamped to their neighbours");
        check(r.stats.holeKnown && r.stats.holeEscaped == esc && r.stats.openSight == 0 && r.stats.applied >= 1, "and the instrument counts the escaped rays and no open sight");
    }

    // ---- an open ring (no frame): the frame is left alone and the sight is counted open ----
    {
        const Scene open = makeScene(false, -1);
        RunIn in;
        const Run r = runDoor(f, open, orig, in);
        check(sameImage(r.image, orig) && !r.gotCopy, "an open sight (no frame in the depth) leaves the picture as it was");
        check(r.stats.dispatched >= 1 && r.stats.raysRead >= 1 && r.stats.openSight == r.stats.raysRead && r.stats.applied + r.stats.openSight >= r.stats.dispatched && !r.stats.holeKnown,
              "and the rays read back say open sight, with no hole to report");
    }

    // ---- the reasons that decline a frame, each counted and each leaving the frame alone ----
    {
        RunIn in;
        in.fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // a format the lens refuses: the frame is built by hand (the rig's pixel helpers are 8-bit)
        ComPtr<ID3D11Texture2D> depthTex;
        ComPtr<ID3D11ShaderResourceView> srv = f.depthSrv(ring, &depthTex);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kOutW; d.Height = kOutH; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> half;
        hr(f.dev->CreateTexture2D(&d, nullptr, &half), "a half-float frame");
        scopeZoomDepthSet(1, srv.Get(), kRenW, kRenH, static_cast<float>(kNearDepth));
        scopeZoomPressBegin(1);
        ScopeZoomFrame rec;
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kQualifies);
        rec.m = rec.mTarget = 2.5f;
        rec.uv[0] = static_cast<float>(kCRen[0] / kRenW);
        rec.uv[1] = static_cast<float>(kCRen[1] / kRenH);
        std::memcpy(rec.tan4, kTan4, sizeof(kTan4));
        scopeZoomPublish(rec);
        const uint32_t region[4] = {0, 0, kOutW, kOutH};
        ScopeZoomDoorArgs a;
        a.eye = 1;
        a.frame = half.Get();
        a.region = region;
        a.inPlace = true;
        a.resolved = true;
        ComPtr<ID3D11Texture2D> copy;
        scopeZoomDoor(a, &copy);
        sz::LensDoorStats s;
        scopeZoomPressTake(&s);
        check(!copy && s.formatRefused == 1 && s.dispatched == 0 && s.qualifying == 1, "a frame that is not R8G8B8A8 or B8G8R8A8 UNORM is refused, counted as format refused and handed back as it was");
        scopeZoomDepthClear(1);
    }
    {
        const struct { const char* what; RunIn in; } cases[] = {
            {"a c older than 50 ms", [] { RunIn i; i.sleepMs = 150; return i; }()},
            {"no depth for the eye", [] { RunIn i; i.hasDepth = false; return i; }()},
            {"a picture that is not the upscaler's", [] { RunIn i; i.resolved = false; return i; }()},
            {"the gate's verdict no S", [] { RunIn i; i.verdict = sz::LensVerdict::kNoS; return i; }()},
            {"the gate's verdict not in the table", [] { RunIn i; i.verdict = sz::LensVerdict::kNotInTable; return i; }()},
            {"the idle verdict", [] { RunIn i; i.verdict = sz::LensVerdict::kIdle; return i; }()},
            {"M at 1 (the first frame of the ease)", [] { RunIn i; i.m = 1.0; return i; }()},
        };
        for (const auto& c : cases) {
            const Run r = runDoor(f, ring, orig, c.in);
            check(sameImage(r.image, orig) && !r.gotCopy, c.what);
            const sz::LensDoorStats& s = r.stats;
            const std::string what = c.what;
            if (what == "a c older than 50 ms") check(s.staleC == 1 && s.dispatched == 0, "a c older than 50 ms is counted stale and leaves the frame alone");
            else if (what == "no depth for the eye") check(s.depthUnavailable == 1 && s.dispatched == 0, "no depth is counted depth unavailable and leaves the frame alone");
            else if (what == "a picture that is not the upscaler's") check(s.formatRefused == 1 && s.dispatched == 0, "a picture that is not the upscaler's is refused and left alone");
            else if (what == "the gate's verdict no S") check(s.noS == 1 && s.qualifying == 0, "the gate's no S is counted and asks nothing of the passes");
            else if (what == "the gate's verdict not in the table") check(s.notInTable == 1 && s.qualifying == 0, "the gate's not in the table is counted and asks nothing of the passes");
            else if (what == "the idle verdict") check(s.noS + s.notInTable + s.staleC + s.dispatched + s.noop + s.formatRefused + s.depthUnavailable == 0, "the idle verdict counts nothing");
            else check(s.noop == 1 && s.dispatched == 0 && s.applied == 1 && s.qualifying == 1, "M at 1 is counted as applied with nothing issued");
        }
    }
    // the eye that is not the aiming one is not called, and a call with no frame does nothing
    {
        ScopeZoomDoorArgs a;
        a.eye = 0;
        ComPtr<ID3D11Texture2D> copy;
        scopeZoomDoor(a, &copy);
        a.eye = 1;
        scopeZoomDoor(a, &copy);
        check(!copy, "a door call for the left eye, or with no frame, does nothing");
    }
}

void infoCheck(Fx& f) {
    if (!f.info) {
        std::puts("scope_zoom_test: no debug layer on this machine (the passes ran without it)");
        return;
    }
    const UINT64 count = f.info->GetNumStoredMessages();
    unsigned bad = 0;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(f.info->GetMessage(i, nullptr, &size)) || !size) continue;
        std::vector<uint8_t> raw(size);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
        if (FAILED(f.info->GetMessage(i, msg, &size))) continue;
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR || msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
            ++bad;
            std::fprintf(stderr, "debug layer: %s\n", msg->pDescription);
        }
    }
    check(bad == 0, "the debug layer reports no error or corruption");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("scope_zoom_test: dry-run (no device, no files)");
        return 0;
    }
    const bool hardware = argc > 1 && std::strcmp(argv[1], "--hardware-self-test") == 0;
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0 && !hardware) {
        std::fprintf(stderr, "usage: --self-test | --dry-run | --hardware-self-test (the machine's own adapter; not a gate)\n");
        return 2;
    }
    try {
        wiringTests();
        Fx f(hardware);
        lensTests(f);
        infoCheck(f);
        scopeZoomRelease();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scope_zoom_test: FAIL: %s (after %u checks)\n", e.what(), g_checks);
        return 1;
    }
    std::printf("scope_zoom_test: %u checks passed.\n", g_checks);
    return 0;
}
