#include "menu_panel.h"

#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/supersample_math.h"
#include "../common/timing.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// ---------------------------------------------------------------------------
// The raster

struct Rgb {
    uint8_t r, g, b;
};

constexpr Rgb kBg = {14, 18, 24};
constexpr float kBgAlpha = 0.86f;
constexpr Rgb kTabActive = {255, 150, 40};
constexpr Rgb kTabIdle = {150, 150, 150};
constexpr Rgb kLabel = {232, 232, 226};
constexpr Rgb kValue = {255, 190, 80};
constexpr Rgb kDimText = {150, 150, 150};
constexpr Rgb kHeading = {120, 170, 220};
constexpr Rgb kHighlight = {255, 140, 0};
constexpr float kHighlightAlpha = 0.22f;
constexpr Rgb kBadge = {255, 170, 60};
constexpr Rgb kHint = {190, 190, 190};
constexpr Rgb kFooter = {140, 140, 140};
constexpr Rgb kToastText = {255, 200, 120};

enum class Font { Row, Tab, Small, Hint };

struct Op {
    bool         text = false;
    RECT         rect{};
    Rgb          rgb{};
    float        alpha = 1.0f;   // fills only
    std::wstring str;
    UINT         align = 0;      // DT_LEFT / DT_RIGHT / DT_CENTER
    Font         font = Font::Row;
};

struct LineRect {
    float y0, y1;   // fractions of the bitmap's height
    bool  selectable;
};

struct Raster {
    std::vector<uint8_t> rgba;   // premultiplied
    int      w = 0, h = 0;
    std::vector<LineRect> lines;
    double   ms = 0.0;
};

struct Dib {
    HDC     dc = nullptr;
    HBITMAP bmp = nullptr;
    HBITMAP old = nullptr;
    uint32_t* bits = nullptr;
    int     w = 0, h = 0;
};

bool makeDib(Dib& d, int w, int h) {
    d.dc = CreateCompatibleDC(nullptr);
    if (!d.dc) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    d.bmp = CreateDIBSection(d.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!d.bmp || !bits) return false;
    d.bits = static_cast<uint32_t*>(bits);
    d.old = static_cast<HBITMAP>(SelectObject(d.dc, d.bmp));
    d.w = w;
    d.h = h;
    memset(d.bits, 0, static_cast<size_t>(w) * h * 4);
    SetBkMode(d.dc, TRANSPARENT);
    return true;
}

void freeDib(Dib& d) {
    if (d.dc && d.old) SelectObject(d.dc, d.old);
    if (d.bmp) DeleteObject(d.bmp);
    if (d.dc) DeleteDC(d.dc);
    d = Dib{};
}

// One fill, "over" onto a premultiplied buffer, in code: GDI cannot blend.
void fillOver(Dib& d, const RECT& r, Rgb rgb, float a) {
    const int x0 = r.left < 0 ? 0 : r.left, y0 = r.top < 0 ? 0 : r.top;
    const int x1 = r.right > d.w ? d.w : r.right, y1 = r.bottom > d.h ? d.h : r.bottom;
    const int ia = static_cast<int>(a * 255.0f + 0.5f);
    const int pr = rgb.r * ia / 255, pg = rgb.g * ia / 255, pb = rgb.b * ia / 255;
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = d.bits + static_cast<size_t>(y) * d.w;
        for (int x = x0; x < x1; ++x) {
            const uint32_t px = row[x];
            const int ob = px & 0xFF, og = (px >> 8) & 0xFF, orr = (px >> 16) & 0xFF;
            const int nb = pb + ob * (255 - ia) / 255;
            const int ng = pg + og * (255 - ia) / 255;
            const int nr = pr + orr * (255 - ia) / 255;
            row[x] = static_cast<uint32_t>(nb) | (static_cast<uint32_t>(ng) << 8) |
                     (static_cast<uint32_t>(nr) << 16);
        }
    }
}

HFONT makeFont(int emPx, bool bold) {
    LOGFONTW lf{};
    lf.lfHeight = -emPx;
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

std::wstring widen(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

// Lay the content out into ops and line rectangles. All sizes derive from
// the cap height in pixels, so the panel reads the same in degrees on any
// headset.
void layout(const MenuContent& c, std::vector<Op>& ops, std::vector<LineRect>& lines, int* outH) {
    const int cap = c.capPx;
    const int W = c.widthPx;
    const int pad = cap * 8 / 10;
    const int rowPitch = cap * 2;
    const int tabH = c.toast ? 0 : cap * 24 / 10;
    const int hintH = c.toast ? 0 : cap * 34 / 10;   // two lines of hint
    const int footH = c.toast ? 0 : cap * 16 / 10;
    const int rows = c.lineCount;
    const int H = pad + tabH + rows * rowPitch + (c.toast ? cap * 2 / 10 : 0) + hintH + footH + pad;
    *outH = H;

    // Background.
    {
        Op o;
        o.rect = {0, 0, W, H};
        o.rgb = kBg;
        o.alpha = kBgAlpha;
        ops.push_back(o);
    }
    int y = pad;
    if (!c.toast) {
        // The tab bar: names spaced across the width.
        int x = pad;
        for (int i = 0; i < c.tabCount; ++i) {
            Op o;
            o.text = true;
            o.str = widen(c.tabs[i]);
            const int wpx = static_cast<int>(o.str.size()) * cap * 6 / 10 + cap;
            o.rect = {x, y, x + wpx, y + tabH};
            o.rgb = i == c.activeTab ? kTabActive : kTabIdle;
            o.align = DT_LEFT;
            o.font = Font::Tab;
            ops.push_back(o);
            if (i == c.activeTab) {
                Op u;
                u.rect = {x, y + tabH - cap / 6, x + wpx - cap / 2, y + tabH};
                u.rgb = kTabActive;
                u.alpha = 0.9f;
                ops.push_back(u);
            }
            x += wpx + cap / 2;
        }
        y += tabH;
    }
    for (int i = 0; i < rows; ++i) {
        const MenuLine& l = c.lines[i];
        const RECT rr = {pad / 2, y, W - pad / 2, y + rowPitch};
        LineRect lr;
        lr.y0 = static_cast<float>(rr.top) / H;
        lr.y1 = static_cast<float>(rr.bottom) / H;
        lr.selectable = l.style == kMenuRow || l.style == kMenuRowHi || l.style == kMenuDim;
        lines.push_back(lr);
        if (l.style == kMenuRowHi) {
            Op h;
            h.rect = rr;
            h.rgb = kHighlight;
            h.alpha = kHighlightAlpha;
            ops.push_back(h);
        }
        Op left;
        left.text = true;
        left.str = widen(l.left);
        left.rect = {pad, y, W * 6 / 10, y + rowPitch};
        left.align = DT_LEFT;
        left.font = l.style == kMenuHeading ? Font::Small : Font::Row;
        left.rgb = l.style == kMenuHeading ? kHeading
                   : l.style == kMenuDim   ? kDimText
                   : c.toast               ? kToastText
                                           : kLabel;
        if (l.style == kMenuHeading) left.rect.left = pad / 2;
        if (c.toast) left.rect.right = W - pad;
        ops.push_back(left);
        if (l.right[0]) {
            Op right;
            right.text = true;
            right.str = widen(l.right);
            right.rect = {W * 6 / 10, y, W - pad, y + rowPitch};
            right.align = DT_RIGHT;
            right.font = Font::Row;
            right.rgb = l.style == kMenuInfo ? kLabel
                        : l.style == kMenuDim ? kDimText
                        : l.badge == kBadgePending ? kBadge
                                                   : kValue;
            ops.push_back(right);
            if (l.badge == kBadgeRestart || l.badge == kBadgeUnknown || l.badge == kBadgePending) {
                Op b;
                b.text = true;
                b.str = l.badge == kBadgeRestart ? L"restart"
                        : l.badge == kBadgePending ? L"at next launch"
                                                   : L"?";
                b.rect = {W * 6 / 10, y + rowPitch * 62 / 100, W - pad, y + rowPitch};
                b.align = DT_RIGHT;
                b.font = Font::Small;
                b.rgb = l.badge == kBadgeUnknown ? kDimText : kBadge;
                ops.push_back(b);
                // The value sits up a little to make room for the badge.
                ops[ops.size() - 2].rect.bottom = y + rowPitch * 70 / 100;
            }
        }
        y += rowPitch;
    }
    if (!c.toast) {
        if (c.hint[0]) {
            Op h;
            h.text = true;
            h.str = widen(c.hint);
            h.rect = {pad, y + cap / 4, W - pad, y + hintH};
            h.align = DT_LEFT | DT_WORDBREAK;
            h.font = Font::Hint;
            h.rgb = kHint;
            ops.push_back(h);
        }
        y += hintH;
        if (c.footer[0]) {
            Op f;
            f.text = true;
            f.str = widen(c.footer);
            f.rect = {pad, y, W - pad, y + footH};
            f.align = DT_LEFT;
            f.font = Font::Small;
            f.rgb = kFooter;
            ops.push_back(f);
        }
    }
}

// Execute the ops into one DIB, in colour or as coverage.
void execute(Dib& d, const std::vector<Op>& ops, bool coverage, HFONT fonts[4]) {
    for (const Op& o : ops) {
        if (!o.text) {
            if (coverage) fillOver(d, o.rect, Rgb{255, 255, 255}, o.alpha);
            else fillOver(d, o.rect, o.rgb, o.alpha);
            continue;
        }
        HFONT f = fonts[static_cast<int>(o.font)];
        HGDIOBJ oldF = SelectObject(d.dc, f);
        SetTextColor(d.dc, coverage ? RGB(255, 255, 255) : RGB(o.rgb.r, o.rgb.g, o.rgb.b));
        RECT r = o.rect;
        UINT fmt = o.align | DT_NOPREFIX;
        if (!(o.align & DT_WORDBREAK)) fmt |= DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
        else fmt |= DT_END_ELLIPSIS;
        DrawTextW(d.dc, o.str.c_str(), -1, &r, fmt);
        SelectObject(d.dc, oldF);
    }
}

bool rasterise(const MenuContent& c, Raster& out) {
    const int64_t t0 = qpcNow();
    std::vector<Op> ops;
    std::vector<LineRect> lines;
    int H = 0;
    layout(c, ops, lines, &H);
    const int W = c.widthPx;
    if (W < 64 || H < 32 || H > 2048) return false;
    Dib colour, cover;
    if (!makeDib(colour, W, H) || !makeDib(cover, W, H)) {
        freeDib(colour);
        freeDib(cover);
        return false;
    }
    const int cap = c.capPx;
    HFONT fonts[4] = {
        makeFont(cap * 10 / 7, false),        // Row
        makeFont(cap * 9 / 7, true),          // Tab
        makeFont(cap * 7 / 7, false),         // Small
        makeFont(cap * 8 / 7, false),         // Hint
    };
    execute(colour, ops, false, fonts);
    execute(cover, ops, true, fonts);
    for (HFONT f : fonts) {
        if (f) DeleteObject(f);
    }
    GdiFlush();
    out.w = W;
    out.h = H;
    out.rgba.resize(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y) {
        const uint32_t* cr = colour.bits + static_cast<size_t>(y) * W;
        const uint32_t* cv = cover.bits + static_cast<size_t>(y) * W;
        uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * W * 4;
        for (int x = 0; x < W; ++x) {
            const uint32_t p = cr[x];
            dst[x * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF);
            dst[x * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);
            dst[x * 4 + 2] = static_cast<uint8_t>(p & 0xFF);
            dst[x * 4 + 3] = static_cast<uint8_t>(cv[x] & 0xFF);
        }
    }
    freeDib(colour);
    freeDib(cover);
    out.lines.swap(lines);
    out.ms = qpcFrequency() > 0
                 ? static_cast<double>(qpcNow() - t0) * 1000.0 / static_cast<double>(qpcFrequency())
                 : 0.0;
    return true;
}

// The worker: one pending content, coalesced; one finished raster, taken
// by the frame thread.
struct Worker {
    std::mutex              m;
    std::condition_variable cv;
    std::thread             thread;
    bool                    started = false;
    bool                    quit = false;
    bool                    hasPending = false;
    MenuContent             pending{};
    bool                    hasReady = false;
    Raster                  ready;
    // The layout the hit test reads, from the raster most recently uploaded.
    std::vector<LineRect>   liveLines;
    int                     liveW = 0, liveH = 0;
    double                  lastMs = 0.0;
};
Worker g_w;

void workerMain() {
    for (;;) {
        MenuContent c;
        {
            std::unique_lock<std::mutex> lock(g_w.m);
            g_w.cv.wait(lock, [] { return g_w.quit || g_w.hasPending; });
            if (g_w.quit) return;
            c = g_w.pending;
            g_w.hasPending = false;
        }
        Raster r;
        bool ok = false;
        guarded("menuPanel/raster", [&] { ok = rasterise(c, r); });
        if (!ok) continue;
        std::lock_guard<std::mutex> lock(g_w.m);
        g_w.ready = std::move(r);
        g_w.hasReady = true;
    }
}

// ---------------------------------------------------------------------------
// The GPU side

constexpr char kCompositeCs[] = R"HLSL(
Texture2D<float4> S : register(t0);
Texture2D<float4> P : register(t1);
SamplerState L : register(s0);
RWTexture2D<float4> O : register(u0);
cbuffer C : register(b0) {
    int4   region;     // the pixels of S this eye owns (x1, y1 exclusive)
    int2   outSize;
    int    flipV;      // the submit's rows run bottom-up
    int    linearOut;  // the frame is linear light: linearise the panel
    float4 tans;       // left, right, top, bottom tangent magnitudes
    float4 m0;         // current-head -> anchor rotation rows; .w = origin
    float4 m1;
    float4 m2;
    float4 geom;       // dist, curve, halfW, halfH
    float4 misc;       // alpha
};
float3 toLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;
    float4 src = S.Load(int3(region.x + id.x, region.y + id.y, 0));
    float u = (id.x + 0.5) / outSize.x;
    float v = (id.y + 0.5) / outSize.y;
    if (flipV) v = 1.0 - v;
    float tx = lerp(-tans.x, tans.y, u);
    float ty = lerp(tans.z, -tans.w, v);
    float3 dv = float3(tx, ty, -1.0);
    float3 df = float3(dot(m0.xyz, dv), dot(m1.xyz, dv), dot(m2.xyz, dv));
    float3 org = float3(m0.w, m1.w, m2.w);
    float dist = geom.x, curve = geom.y, halfW = geom.z, halfH = geom.w;
    float su = -1.0, sv = -1.0;
    if (curve > 0.005) {
        float R = dist / curve;
        float zc = R - dist;
        float a = df.x * df.x + df.z * df.z;
        float b = 2.0 * (org.x * df.x + (org.z - zc) * df.z);
        float c = org.x * org.x + (org.z - zc) * (org.z - zc) - R * R;
        float disc = b * b - 4.0 * a * c;
        if (disc > 0 && a > 1e-8) {
            float t = (-b + sqrt(disc)) / (2.0 * a);
            if (t > 0) {
                float3 hit = org + t * df;
                float th = atan2(hit.x, zc - hit.z);
                su = (th * R + halfW) / (2.0 * halfW);
                sv = (hit.y + halfH) / (2.0 * halfH);
            }
        }
    } else if (df.z < -1e-4) {
        float t = (-dist - org.z) / df.z;
        if (t > 0) {
            float3 hit = org + t * df;
            su = (hit.x + halfW) / (2.0 * halfW);
            sv = (hit.y + halfH) / (2.0 * halfH);
        }
    }
    float4 outc = src;
    if (su >= 0 && su <= 1 && sv >= 0 && sv <= 1) {
        float4 p = P.SampleLevel(L, float2(su, 1.0 - sv), 0);
        if (linearOut) {
            float pa = max(p.a, 1e-4);
            p.rgb = toLinear(p.rgb / pa) * pa;
        }
        float a = p.a * misc.x;
        outc = float4(src.rgb * (1.0 - a) + p.rgb * misc.x, src.a);
    }
    O[id.xy] = outc;
}
)HLSL";

struct Params {
    int32_t region[4];
    int32_t outSize[2];
    int32_t flipV;
    int32_t linearOut;
    float   tans[4];
    float   m0[4];
    float   m1[4];
    float   m2[4];
    float   geom[4];
    float   misc[4];
};

struct EyeState {
    void*                      srcRes = nullptr;
    ID3D11ShaderResourceView*  srcSrv = nullptr;
    ID3D11Texture2D*           copyTex = nullptr;
    ID3D11ShaderResourceView*  copySrv = nullptr;
    uint32_t                   copyW = 0, copyH = 0;
    DXGI_FORMAT                copyFmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D*           outTex = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;
    uint32_t                   outW = 0, outH = 0;
    DXGI_FORMAT                outFmt = DXGI_FORMAT_UNKNOWN;
};
EyeState g_eye[2];

ID3D11Texture2D*          g_panelTex = nullptr;
ID3D11ShaderResourceView* g_panelSrv = nullptr;
int                       g_panelW = 0, g_panelH = 0;
std::atomic<float>        g_panelAspect{0.0f};

ID3D11ComputeShader* g_cs = nullptr;
bool                 g_csTried = false;
ID3D11Buffer*        g_cb = nullptr;
ID3D11SamplerState*  g_samp = nullptr;

std::mutex   g_geomMutex;
MenuGeometry g_geom;

bool     g_failNoted = false;
bool     g_fmtNoted = false;
bool     g_firstNoted = false;
uint32_t g_draws = 0;

FaultBudget g_budget("menuPanel", 8);

void failOnce(const char* what) {
    if (g_failNoted) return;
    g_failNoted = true;
    Log::get().note("menu panel: %s; the panel is not drawn and the keyboard stays the game's.",
                    what);
}

DXGI_FORMAT viewFormatOf(DXGI_FORMAT f, bool* linear) {
    *linear = false;
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            *linear = true;
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

void releaseEye(EyeState& e) {
    if (e.srcSrv) { e.srcSrv->Release(); e.srcSrv = nullptr; }
    e.srcRes = nullptr;
    if (e.copySrv) { e.copySrv->Release(); e.copySrv = nullptr; }
    if (e.copyTex) { e.copyTex->Release(); e.copyTex = nullptr; }
    e.copyW = e.copyH = 0;
    if (e.outUav) { e.outUav->Release(); e.outUav = nullptr; }
    if (e.outTex) { e.outTex->Release(); e.outTex = nullptr; }
    e.outW = e.outH = 0;
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT texFmt, DXGI_FORMAT viewFmt,
             UINT bind, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv,
             ID3D11UnorderedAccessView** uav) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex)) || !*tex) return false;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(*tex, &sd, srv))) return false;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = viewFmt;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(*tex, &ud, uav))) return false;
    }
    return true;
}

void* compositeInner(void* srcTex, int eye, const float* bounds, const float* xf) {
    MenuGeometry g;
    {
        std::lock_guard<std::mutex> lock(g_geomMutex);
        g = g_geom;
    }
    if (!(g.alpha > 0.0f) || !g_panelSrv || g_panelW <= 0 || g_panelH <= 0) return nullptr;

    ID3D11Texture2D* src = nullptr;
    static_cast<IUnknown*>(srcTex)->QueryInterface(__uuidof(ID3D11Texture2D),
                                                   reinterpret_cast<void**>(&src));
    if (!src) return nullptr;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    bool ok = sd.SampleDesc.Count == 1 && sd.ArraySize == 1 && sd.MipLevels == 1;
    if (!ok) failOnce("the outgoing texture is multisampled or arrayed, a kind this pass does not handle");

    uint32_t region[4] = {};
    bool flipU = false, flipV = false;
    if (ok && !supersampleRegionFromBounds(sd.Width, sd.Height, bounds, region, &flipU, &flipV)) {
        ok = false;
        failOnce("the Submit bounds name no usable eye region");
    }
    const uint32_t regionW = ok ? region[2] - region[0] : 0;
    const uint32_t regionH = ok ? region[3] - region[1] : 0;

    bool linear = false;
    DXGI_FORMAT viewFmt = ok ? viewFormatOf(sd.Format, &linear) : DXGI_FORMAT_UNKNOWN;
    if (ok && viewFmt == DXGI_FORMAT_UNKNOWN) {
        ok = false;
        if (!g_fmtNoted) {
            g_fmtNoted = true;
            Log::get().note("menu panel: the outgoing texture's format (DXGI_FORMAT %d) is one "
                            "this pass does not handle; the panel is not drawn. Please report "
                            "this log.",
                            static_cast<int>(sd.Format));
        }
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (ok) {
        src->GetDevice(&dev);
        if (dev) dev->GetImmediateContext(&ctx);
        ok = dev && ctx;
    }
    if (ok && !g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = shaderSwapCompileCs(ctx, kCompositeCs, sizeof(kCompositeCs) - 1, "main",
                                   "menu_panel_cs", nullptr, "menu panel");
    }
    ok = ok && g_cs != nullptr;
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Params);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the parameter buffer could not be created");
    }
    if (ok && !g_samp) {
        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&smp, &g_samp));
        if (!ok) failOnce("the sampler could not be created");
    }

    EyeState& e = g_eye[eye];
    ID3D11ShaderResourceView* inSrv = nullptr;
    bool viaCopy = false;
    if (ok) {
        if (sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            if (e.srcRes != static_cast<void*>(src) || !e.srcSrv) {
                if (e.srcSrv) { e.srcSrv->Release(); e.srcSrv = nullptr; }
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = viewFmt;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (SUCCEEDED(dev->CreateShaderResourceView(src, &vd, &e.srcSrv)) && e.srcSrv) {
                    e.srcRes = src;
                } else {
                    e.srcSrv = nullptr;
                }
            }
            inSrv = e.srcSrv;
        }
        if (!inSrv) {
            viaCopy = true;
            if (!e.copyTex || e.copyW != regionW || e.copyH != regionH || e.copyFmt != sd.Format) {
                if (e.copySrv) { e.copySrv->Release(); e.copySrv = nullptr; }
                if (e.copyTex) { e.copyTex->Release(); e.copyTex = nullptr; }
                if (makeTex(dev, regionW, regionH, sd.Format, viewFmt, D3D11_BIND_SHADER_RESOURCE,
                            &e.copyTex, &e.copySrv, nullptr)) {
                    e.copyW = regionW;
                    e.copyH = regionH;
                    e.copyFmt = sd.Format;
                } else {
                    if (e.copySrv) { e.copySrv->Release(); e.copySrv = nullptr; }
                    if (e.copyTex) { e.copyTex->Release(); e.copyTex = nullptr; }
                }
            }
            inSrv = e.copySrv;
        }
        if (!inSrv) {
            ok = false;
            failOnce("the outgoing texture refuses a shader view and could not be copied");
        }
    }
    if (ok && (!e.outTex || e.outW != regionW || e.outH != regionH || e.outFmt != sd.Format)) {
        if (e.outUav) { e.outUav->Release(); e.outUav = nullptr; }
        if (e.outTex) { e.outTex->Release(); e.outTex = nullptr; }
        if (makeTex(dev, regionW, regionH, sd.Format, viewFmt,
                    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &e.outTex, nullptr,
                    &e.outUav)) {
            e.outW = regionW;
            e.outH = regionH;
            e.outFmt = sd.Format;
        } else {
            ok = false;
            failOnce("the output texture could not be created");
        }
    }

    void* result = nullptr;
    if (ok) {
        // The eye's frustum: the outer tangent is temporal (left edge of the
        // left eye, right edge of the right); the vertical pair as measured,
        // or derived symmetric from the region's shape when unpublished.
        float outer = 0.0f, inner = 0.0f, top = 0.0f, bot = 0.0f;
        if (!eyeTangents(&outer, &inner)) {
            outer = inner = 1.0f;
        }
        if (!eyeTangentsVertical(&top, &bot)) {
            top = bot = (outer + inner) * 0.5f * (static_cast<float>(regionH) / static_cast<float>(regionW));
        }
        Params p{};
        if (viaCopy) {
            D3D11_BOX box{};
            box.left = region[0];
            box.top = region[1];
            box.right = region[2];
            box.bottom = region[3];
            box.back = 1;
            ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &box);
            p.region[2] = static_cast<int32_t>(regionW);
            p.region[3] = static_cast<int32_t>(regionH);
        } else {
            for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
        }
        p.outSize[0] = static_cast<int32_t>(regionW);
        p.outSize[1] = static_cast<int32_t>(regionH);
        p.flipV = flipV ? 1 : 0;
        p.linearOut = linear ? 1 : 0;
        p.tans[0] = eye == 0 ? outer : inner;
        p.tans[1] = eye == 0 ? inner : outer;
        p.tans[2] = top;
        p.tans[3] = bot;
        p.m0[0] = xf[0]; p.m0[1] = xf[1]; p.m0[2] = xf[2]; p.m0[3] = xf[9];
        p.m1[0] = xf[3]; p.m1[1] = xf[4]; p.m1[2] = xf[5]; p.m1[3] = xf[10];
        p.m2[0] = xf[6]; p.m2[1] = xf[7]; p.m2[2] = xf[8]; p.m2[3] = xf[11];
        p.geom[0] = g.dist;
        p.geom[1] = g.curve;
        p.geom[2] = g.halfW;
        p.geom[3] = g.halfW * g_panelAspect.load();
        p.misc[0] = g.alpha;
        D3D11_MAPPED_SUBRESOURCE m{};
        bool ran = false;
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData) {
            memcpy(m.pData, &p, sizeof(p));
            ctx->Unmap(g_cb, 0);
            ran = true;
        }
        if (ran) {
            ID3D11ComputeShader* savedCs = nullptr;
            ID3D11ShaderResourceView* savedSrv[2] = {};
            ID3D11UnorderedAccessView* savedUav = nullptr;
            ID3D11Buffer* savedCb = nullptr;
            ID3D11SamplerState* savedSamp = nullptr;
            ctx->CSGetShader(&savedCs, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 2, savedSrv);
            ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
            ctx->CSGetConstantBuffers(0, 1, &savedCb);
            ctx->CSGetSamplers(0, 1, &savedSamp);

            ID3D11ShaderResourceView* nullSrv[2] = {};
            ID3D11UnorderedAccessView* nullUav = nullptr;
            ID3D11ShaderResourceView* setSrv[2] = {inSrv, g_panelSrv};
            ctx->CSSetShaderResources(0, 2, nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            ctx->CSSetShader(g_cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &g_cb);
            ctx->CSSetSamplers(0, 1, &g_samp);
            ctx->CSSetShaderResources(0, 2, setSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &e.outUav, nullptr);
            ctx->Dispatch((regionW + 7) / 8, (regionH + 7) / 8, 1);

            ctx->CSSetShaderResources(0, 2, nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            ctx->CSSetShader(savedCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 2, savedSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &savedCb);
            ctx->CSSetSamplers(0, 1, &savedSamp);
            if (savedCs) savedCs->Release();
            for (ID3D11ShaderResourceView* v : savedSrv) if (v) v->Release();
            if (savedUav) savedUav->Release();
            if (savedCb) savedCb->Release();
            if (savedSamp) savedSamp->Release();

            result = e.outTex;
            ++g_draws;
            bumpMenuDrawn();
            if (!g_firstNoted) {
                g_firstNoted = true;
                Log::get().note(
                    "menu panel: first composite -- a %dx%d bitmap on a %ux%u %s eye region "
                    "(DXGI_FORMAT %d%s), %.2f m out, curve %.2f, half-width %.2f m%s.",
                    g_panelW, g_panelH, regionW, regionH, eye == 0 ? "left" : "right",
                    static_cast<int>(sd.Format), linear ? ", linear light" : "",
                    static_cast<double>(g.dist), static_cast<double>(g.curve),
                    static_cast<double>(g.halfW),
                    viaCopy ? " (the source was copied out first: it refuses a shader view)" : "");
            }
        } else {
            failOnce("the parameter buffer could not be written");
        }
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    src->Release();
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------

bool menuPanelHit(const float org[3], const float dir[3], float dist, float curve, float halfW,
                  float halfH, float* su, float* sv) {
    float u = -1.0f, v = -1.0f;
    if (curve > 0.005f) {
        const float R = dist / curve;
        const float zc = R - dist;
        const float a = dir[0] * dir[0] + dir[2] * dir[2];
        const float b = 2.0f * (org[0] * dir[0] + (org[2] - zc) * dir[2]);
        const float c = org[0] * org[0] + (org[2] - zc) * (org[2] - zc) - R * R;
        const float disc = b * b - 4.0f * a * c;
        if (disc > 0.0f && a > 1e-8f) {
            const float t = (-b + sqrtf(disc)) / (2.0f * a);
            if (t > 0.0f) {
                const float hx = org[0] + t * dir[0];
                const float hy = org[1] + t * dir[1];
                const float hz = org[2] + t * dir[2];
                const float th = atan2f(hx, zc - hz);
                u = (th * R + halfW) / (2.0f * halfW);
                v = (hy + halfH) / (2.0f * halfH);
            }
        }
    } else if (dir[2] < -1e-4f) {
        const float t = (-dist - org[2]) / dir[2];
        if (t > 0.0f) {
            const float hx = org[0] + t * dir[0];
            const float hy = org[1] + t * dir[1];
            u = (hx + halfW) / (2.0f * halfW);
            v = (hy + halfH) / (2.0f * halfH);
        }
    }
    if (su) *su = u;
    if (sv) *sv = v;
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

void menuPanelSubmit(const MenuContent& c) {
    std::lock_guard<std::mutex> lock(g_w.m);
    if (!g_w.started) {
        g_w.started = true;
        g_w.quit = false;
        g_w.thread = std::thread(workerMain);
    }
    g_w.pending = c;
    g_w.hasPending = true;
    g_w.cv.notify_one();
}

void menuPanelTick(ID3D11Device* dev) {
    if (!dev) return;
    Raster r;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(g_w.m);
        if (g_w.hasReady) {
            r = std::move(g_w.ready);
            g_w.hasReady = false;
            have = true;
        }
    }
    if (!have) return;
    guardedBudget(g_budget, [&] {
        if (!g_panelTex || g_panelW != r.w || g_panelH != r.h) {
            if (g_panelSrv) { g_panelSrv->Release(); g_panelSrv = nullptr; }
            if (g_panelTex) { g_panelTex->Release(); g_panelTex = nullptr; }
            if (!makeTex(dev, static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h),
                         DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                         D3D11_BIND_SHADER_RESOURCE, &g_panelTex, &g_panelSrv, nullptr)) {
                if (g_panelSrv) { g_panelSrv->Release(); g_panelSrv = nullptr; }
                if (g_panelTex) { g_panelTex->Release(); g_panelTex = nullptr; }
                failOnce("the panel texture could not be created");
                return;
            }
            g_panelW = r.w;
            g_panelH = r.h;
        }
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (!ctx) return;
        ctx->UpdateSubresource(g_panelTex, 0, nullptr, r.rgba.data(),
                               static_cast<UINT>(r.w) * 4, 0);
        ctx->Release();
        g_panelAspect.store(static_cast<float>(r.h) / static_cast<float>(r.w));
        std::lock_guard<std::mutex> lock(g_w.m);
        g_w.liveLines = r.lines;
        g_w.liveW = r.w;
        g_w.liveH = r.h;
        g_w.lastMs = r.ms;
    });
}

void menuPanelSetGeometry(const MenuGeometry& g) {
    std::lock_guard<std::mutex> lock(g_geomMutex);
    g_geom = g;
}

float menuPanelAspect() { return g_panelAspect.load(); }

int menuPanelLineAt(float u, float v) {
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return -1;
    std::lock_guard<std::mutex> lock(g_w.m);
    for (size_t i = 0; i < g_w.liveLines.size(); ++i) {
        const LineRect& l = g_w.liveLines[i];
        if (v >= l.y0 && v < l.y1) return l.selectable ? static_cast<int>(i) : -1;
    }
    return -1;
}

bool menuPanelStats(int* w, int* h, double* lastMs) {
    std::lock_guard<std::mutex> lock(g_w.m);
    if (g_w.liveW == 0) return false;
    if (w) *w = g_w.liveW;
    if (h) *h = g_w.liveH;
    if (lastMs) *lastMs = g_w.lastMs;
    return true;
}

void menuPanelShutdown() {
    {
        std::lock_guard<std::mutex> lock(g_w.m);
        g_w.quit = true;
        g_w.cv.notify_one();
    }
    if (g_w.started && g_w.thread.joinable()) g_w.thread.join();
    g_w.started = false;
    for (EyeState& e : g_eye) releaseEye(e);
    if (g_panelSrv) { g_panelSrv->Release(); g_panelSrv = nullptr; }
    if (g_panelTex) { g_panelTex->Release(); g_panelTex = nullptr; }
    if (g_samp) { g_samp->Release(); g_samp = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    g_panelW = g_panelH = 0;
    g_panelAspect.store(0.0f);
    if (g_draws) Log::get().note("menu panel: %u eye composites this session.", g_draws);
}

}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrMenuPanel(void* srcTex, int eye, const float* bounds,
                                                     const float* xf) {
    if (!srcTex || !xf || eye < 0 || eye > 1) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::compositeInner(srcTex, eye, bounds, xf);
    });
    return out;
}
