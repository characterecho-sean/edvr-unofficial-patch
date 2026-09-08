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
#include "perf_monitor.h"   // the upload is an event for the drop attribution
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

enum class Font { Row, Tab, Small, Hint, Big, Caption, TileSub };
constexpr int kFontCount = 7;

struct Op {
    bool         text = false;
    RECT         rect{};
    Rgb          rgb{};
    float        alpha = 1.0f;   // fills only
    std::wstring str;
    UINT         align = 0;      // DT_LEFT / DT_RIGHT / DT_CENTER, DT_WORDBREAK for wrapping
    Font         font = Font::Row;
    bool         top = false;    // draw from the rect's top, unclipped, rather than centred
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
    const int rowPitch = c.compact ? cap * 17 / 10 : cap * 2;
    const int tabH = c.toast ? 0 : cap * 24 / 10;
    const int hintH = c.toast ? 0 : cap * 34 / 10;   // two lines of hint
    const int footH = c.toast ? 0 : cap * 16 / 10;
    const int graphH = c.graphCount > 0 ? cap * 40 / 10 : 0;
    const int rows = c.lineCount;
    // The tile grid: caption, big value, a two-line sub, in a box five
    // tile-units tall, the unit being nine tenths of a cap. Each box is
    // sized to its font's LINE height, not its cap height -- the first
    // build gave the big number 1.7 caps for a face whose line box is
    // 2.8, and the digits clipped top and bottom.
    const int columns = c.tileColumns > 0 ? c.tileColumns : 4;
    const int tileRows = c.tileCount > 0 ? (c.tileCount + columns - 1) / columns : 0;
    const int u = cap * 9 / 10;
    const int tileH = u * 50 / 10;
    const int tileGap = cap / 4;
    const int tilesH = tileRows > 0 ? tileRows * (tileH + tileGap) + cap / 2 : 0;
    const int hintHUsed = c.hint[0] ? hintH : 0;
    const int H = pad + tabH + tilesH + rows * rowPitch + (c.toast ? cap * 2 / 10 : 0) + graphH +
                  hintHUsed + footH + pad;
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
    if (tileRows > 0) {
        const int gridW = W - 2 * pad;
        const int tileW = (gridW - (columns - 1) * tileGap) / columns;
        for (int i = 0; i < c.tileCount; ++i) {
            const MenuTile& t = c.tiles[i];
            const int col = i % columns, row = i / columns;
            const int x0 = pad + col * (tileW + tileGap);
            const int y0 = y + row * (tileH + tileGap);
            Op box;
            box.rect = {x0, y0, x0 + tileW, y0 + tileH};
            box.rgb = Rgb{255, 255, 255};
            box.alpha = 0.06f;
            ops.push_back(box);
            const int inset = u / 3;
            Op cap1;
            cap1.text = true;
            cap1.str = widen(t.caption);
            cap1.rect = {x0 + inset, y0 + u / 8, x0 + tileW - inset, y0 + u * 12 / 10};
            cap1.align = DT_LEFT;
            cap1.font = Font::Caption;
            cap1.rgb = kDimText;
            ops.push_back(cap1);
            Op val;
            val.text = true;
            val.str = widen(t.value);
            val.rect = {x0 + inset, y0 + u * 10 / 10, x0 + tileW - inset, y0 + u * 31 / 10};
            val.align = DT_LEFT;
            val.font = Font::Big;
            val.rgb = kLabel;
            val.top = true;
            ops.push_back(val);
            if (t.sub[0]) {
                Op sub;
                sub.text = true;
                sub.str = widen(t.sub);
                sub.rect = {x0 + inset, y0 + u * 29 / 10, x0 + tileW - inset, y0 + tileH - u / 10};
                sub.align = DT_LEFT | DT_WORDBREAK;
                sub.font = Font::TileSub;
                sub.rgb = kHint;
                ops.push_back(sub);
            }
        }
        y += tilesH;
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
        // Information rows carry long values, so the split sits further
        // left for them than for a setting's label and value; a note has
        // the whole width.
        const int split = l.style == kMenuNote ? W - pad : l.style == kMenuInfo ? W * 26 / 100 : W * 6 / 10;
        Op left;
        left.text = true;
        left.str = widen(l.left);
        left.rect = {pad, y, split, y + rowPitch};
        left.align = DT_LEFT;
        left.font = l.style == kMenuHeading ? Font::Small : l.style == kMenuNote ? Font::Hint : Font::Row;
        left.rgb = l.style == kMenuHeading ? kHeading
                   : l.style == kMenuDim   ? kDimText
                   : l.style == kMenuNote  ? kHint
                   : c.toast               ? kToastText
                                           : kLabel;
        if (l.style == kMenuHeading) left.rect.left = pad / 2;
        if (c.toast) left.rect.right = W - pad;
        ops.push_back(left);
        if (l.right[0]) {
            Op right;
            right.text = true;
            right.str = widen(l.right);
            right.rect = {split, y, W - pad, y + rowPitch};
            right.align = l.style == kMenuInfo ? DT_LEFT : DT_RIGHT;
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
                b.rect = {split, y + rowPitch * 62 / 100, W - pad, y + rowPitch};
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
    if (c.graphCount > 0) {
        // The frame-time strip: one bar per frame, oldest left, against a
        // scale of twice the display's budget, with the budget drawn as a
        // line. Green within budget, amber over it, red at twice it.
        const int gx0 = pad, gx1 = W - pad;
        const int gy0 = y + cap / 2, gy1 = y + graphH - cap / 4;
        {
            Op bg;
            bg.rect = {gx0, gy0, gx1, gy1};
            bg.rgb = Rgb{0, 0, 0};
            bg.alpha = 0.35f;
            ops.push_back(bg);
        }
        const float scale = c.graphBudgetMs > 0.0f ? 2.0f * c.graphBudgetMs : 22.2f;
        const int   plotH = gy1 - gy0;
        const float barW = static_cast<float>(gx1 - gx0) / static_cast<float>(c.graphCount);
        for (int i = 0; i < c.graphCount; ++i) {
            const float ms = c.graph[i];
            if (!(ms > 0.0f)) continue;
            float frac = ms / scale;
            if (frac > 1.0f) frac = 1.0f;
            const int h = static_cast<int>(frac * plotH + 0.5f);
            Op b;
            b.rect = {gx0 + static_cast<int>(i * barW), gy1 - h,
                      gx0 + static_cast<int>((i + 1) * barW) - 1, gy1};
            if (b.rect.right <= b.rect.left) b.rect.right = b.rect.left + 1;
            b.rgb = ms > 2.0f * c.graphBudgetMs   ? Rgb{255, 90, 70}
                    : ms > c.graphBudgetMs * 1.02f ? Rgb{255, 170, 60}
                                                   : Rgb{110, 200, 120};
            b.alpha = 0.9f;
            ops.push_back(b);
        }
        {
            Op line;
            const int ly = gy1 - static_cast<int>(0.5f * plotH + 0.5f);
            line.rect = {gx0, ly, gx1, ly + (cap / 12 > 0 ? cap / 12 : 1)};
            line.rgb = kLabel;
            line.alpha = 0.5f;
            ops.push_back(line);
        }
        if (c.graphLabel[0]) {
            Op t;
            t.text = true;
            t.str = widen(c.graphLabel);
            t.rect = {gx0 + cap / 3, gy0, gx1, gy0 + cap * 14 / 10};
            t.align = DT_LEFT;
            t.font = Font::Small;
            t.rgb = kFooter;
            ops.push_back(t);
        }
        y += graphH;
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
        y += hintHUsed;
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
void execute(Dib& d, const std::vector<Op>& ops, bool coverage, HFONT fonts[kFontCount]) {
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
        if (!(o.align & DT_WORDBREAK)) {
            fmt |= DT_SINGLELINE | DT_END_ELLIPSIS;
            // Centred in the box, or set from its top and never clipped: a
            // big number's descender room may reach past its box.
            fmt |= o.top ? DT_NOCLIP : DT_VCENTER;
        } else {
            fmt |= DT_END_ELLIPSIS;
        }
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
    // Em sizes from the cap height: Segoe UI's cap height is about 0.7 em,
    // and its line box about 1.33 em. The tile faces are sized from the
    // tile unit (0.9 cap): the number 1.5 units, the caption 0.8, the sub
    // 0.72 -- about sixteen characters a line in a four-across tile.
    const int u = cap * 9 / 10;
    HFONT fonts[kFontCount] = {
        makeFont(cap * 10 / 7, false),        // Row
        makeFont(cap * 9 / 7, true),          // Tab
        makeFont(cap * 7 / 7, false),         // Small
        makeFont(cap * 8 / 7, false),         // Hint
        makeFont(u * 15 / 10, true),          // Big: the tiles' numbers
        makeFont(u * 8 / 10, true),           // Caption
        makeFont(u * 72 / 100, false),        // TileSub
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
    int4   box;        // the output pixels this dispatch covers (x1, y1 exclusive)
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
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 id = uint2(box.x + tid.x, box.y + tid.y);
    if (id.x >= (uint)box.z || id.y >= (uint)box.w) return;
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
    int32_t box[4];
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

// The GPU price, the sharpen pass's ring: a timestamp pair around the copy
// and the dispatch, never awaited, averaged and said once after enough of
// them -- so the overlay's cost is a number in the log, not a belief.
struct QuerySlot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool         inUse = false;
};
constexpr int kQueryRing = 8;
QuerySlot g_qring[kQueryRing];
uint32_t  g_timeCount = 0;
double    g_timeSum = 0.0;
double    g_timeMax = 0.0;
bool      g_timeLogged = false;
std::atomic<float> g_gpuMsAvg{0.0f};
uint32_t  g_lastBoxW = 0, g_lastBoxH = 0;

FaultBudget g_budget("menuPanel", 8);

void releaseQueries() {
    for (QuerySlot& q : g_qring) {
        if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
        if (q.begin) { q.begin->Release(); q.begin = nullptr; }
        if (q.end) { q.end->Release(); q.end = nullptr; }
        q.inUse = false;
    }
}

void pollQueries(ID3D11DeviceContext* ctx) {
    for (QuerySlot& q : g_qring) {
        if (!q.inUse) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        const HRESULT h0 = ctx->GetData(q.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT h1 = ctx->GetData(q.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        q.inUse = false;
        if (dj.Disjoint || h0 != S_OK || h1 != S_OK || dj.Frequency == 0) continue;
        const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        ++g_timeCount;
        g_timeSum += ms;
        if (ms > g_timeMax) g_timeMax = ms;
        g_gpuMsAvg.store(static_cast<float>(g_timeSum / g_timeCount));
    }
    if (!g_timeLogged && g_timeCount >= 240) {
        g_timeLogged = true;
        Log::get().note("menu panel: measured %.3f ms per eye on average (max %.3f) -- the region "
                        "copy plus the composite over the panel's %ux%u pixel box. That is the "
                        "overlay's whole GPU price while it is up.",
                        g_timeSum / g_timeCount, g_timeMax, g_lastBoxW, g_lastBoxH);
    }
}

int acquireQuery(ID3D11Device* dev) {
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_qring[i];
        if (q.inUse) continue;
        if (!q.disjoint) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qt{};
            qt.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(dev->CreateQuery(&qd, &q.disjoint)) || FAILED(dev->CreateQuery(&qt, &q.begin)) ||
                FAILED(dev->CreateQuery(&qt, &q.end))) {
                if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
                if (q.begin) { q.begin->Release(); q.begin = nullptr; }
                if (q.end) { q.end->Release(); q.end = nullptr; }
                return -1;
            }
        }
        return i;
    }
    return -1;
}

// The panel's footprint in this eye, in region pixels, from its corners
// (nine points along the top and bottom edges, so a curved panel's bulge
// is inside it) projected through the eye's frustum. False when the panel
// is behind the eye or entirely outside it: nothing to draw here.
bool panelBox(const float* xf, const float* tans, float dist, float curve, float halfW, float halfH,
              uint32_t regionW, uint32_t regionH, bool flipV, int32_t box[4]) {
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    const float lt = tans[0], rt = tans[1], top = tans[2], bot = tans[3];
    for (int i = 0; i <= 8; ++i) {
        const float t = -1.0f + 2.0f * static_cast<float>(i) / 8.0f;
        float qx, qz;
        if (curve > 0.005f) {
            const float R = dist / curve;
            const float th = t * halfW / R;
            qx = R * sinf(th);
            qz = (R - dist) - R * cosf(th);
        } else {
            qx = t * halfW;
            qz = -dist;
        }
        for (int s = -1; s <= 1; s += 2) {
            const float q[3] = {qx - xf[9], static_cast<float>(s) * halfH - xf[10], qz - xf[11]};
            // Anchor -> eye: D transposed.
            const float vx = xf[0] * q[0] + xf[3] * q[1] + xf[6] * q[2];
            const float vy = xf[1] * q[0] + xf[4] * q[1] + xf[7] * q[2];
            const float vz = xf[2] * q[0] + xf[5] * q[1] + xf[8] * q[2];
            if (vz > -1e-3f) return false;   // at or behind the eye: draw the whole region instead
            const float tx = vx / -vz, ty = vy / -vz;
            const float u = (tx + lt) / (lt + rt);
            float v = (top - ty) / (top + bot);
            if (flipV) v = 1.0f - v;
            if (u < minU) minU = u;
            if (u > maxU) maxU = u;
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
    }
    const float W = static_cast<float>(regionW), H = static_cast<float>(regionH);
    int32_t x0 = static_cast<int32_t>(floorf(minU * W)) - 2;
    int32_t x1 = static_cast<int32_t>(ceilf(maxU * W)) + 2;
    int32_t y0 = static_cast<int32_t>(floorf(minV * H)) - 2;
    int32_t y1 = static_cast<int32_t>(ceilf(maxV * H)) + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<int32_t>(regionW)) x1 = static_cast<int32_t>(regionW);
    if (y1 > static_cast<int32_t>(regionH)) y1 = static_cast<int32_t>(regionH);
    box[0] = x0;
    box[1] = y0;
    box[2] = x1;
    box[3] = y1;
    return true;
}

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
        p.tans[0] = eye == 0 ? outer : inner;
        p.tans[1] = eye == 0 ? inner : outer;
        p.tans[2] = top;
        p.tans[3] = bot;
        // Where the panel lands in this eye. Outside it entirely: forward
        // the frame untouched, nothing copied, nothing dispatched. Behind
        // the eye (a look away from a world-anchored panel): the whole
        // region, which the shader answers pixel by pixel.
        int32_t box[4] = {0, 0, static_cast<int32_t>(regionW), static_cast<int32_t>(regionH)};
        const float halfH = g.halfW * g_panelAspect.load();
        if (panelBox(xf, p.tans, g.dist, g.curve, g.halfW, halfH, regionW, regionH, flipV, box) &&
            (box[2] <= box[0] || box[3] <= box[1])) {
            if (ctx) ctx->Release();
            if (dev) dev->Release();
            src->Release();
            return nullptr;
        }
        pollQueries(ctx);
        const int qs = acquireQuery(dev);
        if (qs >= 0) {
            ctx->Begin(g_qring[qs].disjoint);
            ctx->End(g_qring[qs].begin);
        }
        D3D11_BOX rb{};
        rb.left = region[0];
        rb.top = region[1];
        rb.right = region[2];
        rb.bottom = region[3];
        rb.back = 1;
        if (viaCopy) {
            ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &rb);
            p.region[2] = static_cast<int32_t>(regionW);
            p.region[3] = static_cast<int32_t>(regionH);
            // The dispatch fills the whole output from the copy.
            box[0] = 0;
            box[1] = 0;
            box[2] = static_cast<int32_t>(regionW);
            box[3] = static_cast<int32_t>(regionH);
        } else {
            // The region lands in the output by copy; only the panel's box
            // is then composited, reading the source through its view.
            ctx->CopySubresourceRegion(e.outTex, 0, 0, 0, 0, src, 0, &rb);
            for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
        }
        for (int i = 0; i < 4; ++i) p.box[i] = box[i];
        g_lastBoxW = static_cast<uint32_t>(box[2] - box[0]);
        g_lastBoxH = static_cast<uint32_t>(box[3] - box[1]);
        p.outSize[0] = static_cast<int32_t>(regionW);
        p.outSize[1] = static_cast<int32_t>(regionH);
        p.flipV = flipV ? 1 : 0;
        p.linearOut = linear ? 1 : 0;
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
            ctx->Dispatch((static_cast<UINT>(box[2] - box[0]) + 7) / 8,
                          (static_cast<UINT>(box[3] - box[1]) + 7) / 8, 1);
            if (qs >= 0) {
                ctx->End(g_qring[qs].end);
                ctx->End(g_qring[qs].disjoint);
                g_qring[qs].inUse = true;
            }

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
            if (qs >= 0) {
                // A query begun and never ended would wedge its slot.
                ctx->End(g_qring[qs].end);
                ctx->End(g_qring[qs].disjoint);
                g_qring[qs].inUse = true;
            }
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
        perfMonitorNoteEvent(kEvRaster);
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

bool menuPanelStats(int* w, int* h, double* lastMs, float* gpuMs) {
    std::lock_guard<std::mutex> lock(g_w.m);
    if (g_w.liveW == 0) return false;
    if (w) *w = g_w.liveW;
    if (h) *h = g_w.liveH;
    if (lastMs) *lastMs = g_w.lastMs;
    if (gpuMs) *gpuMs = g_gpuMsAvg.load();
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
    releaseQueries();
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
