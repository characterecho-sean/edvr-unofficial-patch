#include "pixel_probe.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/log.h"

namespace edvr {

namespace detail {
uint32_t g_pixelProbePointCount = 0;
bool g_pixelProbeActive = false;
}  // namespace detail

namespace {

constexpr uint32_t kWindow = 16;
constexpr uint32_t kMaxDrawSlots = 2048;              // the design's cap
constexpr uint32_t kBaselineSlots = kPixelProbeMaxPoints * 2;   // one per point per eye
constexpr uint32_t kTotalSlots = kMaxDrawSlots + kBaselineSlots;
constexpr uint32_t kAtlasCols = 64;
constexpr uint32_t kAtlasRows = (kTotalSlots + kAtlasCols - 1) / kAtlasCols;
constexpr uint32_t kMaxLoggedChanges = 256;

PixelProbePoint g_points[kPixelProbeMaxPoints];

bool     g_armed = false;
uint32_t g_frame = 0;          // this module's own count, the object_probe.cpp pattern
uint32_t g_targetFrame = 0;    // the one frame a run watches

// ONE staging atlas: CopySubresourceRegion's destination during the frame
// (D3D11 allows a sub-rectangle copy into a STAGING resource, the same
// idiom eye_draw_snapshot.h uses for a whole-resource copy) and the Map
// target at the closing boundary -- no intermediate DEFAULT texture, no
// second copy.
ID3D11Texture2D* g_atlas = nullptr;
DXGI_FORMAT      g_atlasFormat = DXGI_FORMAT_UNKNOWN;
uint32_t         g_bpp = 0;

// Baseline slot per point per eye (UINT32_MAX: not yet taken this run) and
// the slot each point/eye should next be diffed against -- the baseline
// until a draw's copy replaces it.
uint32_t g_baselineSlot[kPixelProbeMaxPoints][2];
uint32_t g_lastSlot[kPixelProbeMaxPoints][2];
bool     g_baselineTaken[2] = {false, false};

uint32_t g_nextDrawSlot = 0;    // cursor into the draw region, 0..kMaxDrawSlots

struct DrawRecord {
    int      eye = -1;
    uint32_t slot[kPixelProbeMaxPoints];   // UINT32_MAX where dropped
    DrawInfo info;
};
std::vector<DrawRecord> g_draws;

uint32_t g_eyesSeenMask = 0;
uint32_t g_drawsIntoEye = 0;
uint32_t g_copied = 0;
uint32_t g_dropped = 0;
uint32_t g_declined = 0;

// The handful of formats an eye render target realistically carries
// (eye_draw_snapshot.h's copySurface uses the same set for the same
// reason). Unrecognised is a decline, never a guess at the stride.
uint32_t bytesPerTexel(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
        return 8;
    default:
        return 0;
    }
}

void slotOrigin(uint32_t slot, uint32_t& x, uint32_t& y) {
    x = (slot % kAtlasCols) * kWindow;
    y = (slot / kAtlasCols) * kWindow;
}

// The 16x16 window around a normalised point, shifted (never shrunk) to
// stay inside the target -- callers have already declined anything under
// kWindow in either dimension, so a fit always exists.
void windowOrigin(float nx, float ny, uint32_t w, uint32_t h, uint32_t& x0, uint32_t& y0) {
    const int32_t cx = static_cast<int32_t>(nx * static_cast<float>(w));
    const int32_t cy = static_cast<int32_t>(ny * static_cast<float>(h));
    const int32_t maxX = static_cast<int32_t>(w) - static_cast<int32_t>(kWindow);
    const int32_t maxY = static_cast<int32_t>(h) - static_cast<int32_t>(kWindow);
    int32_t x = cx - static_cast<int32_t>(kWindow / 2);
    int32_t y = cy - static_cast<int32_t>(kWindow / 2);
    if (x < 0) x = 0; if (x > maxX) x = maxX;
    if (y < 0) y = 0; if (y > maxY) y = maxY;
    x0 = static_cast<uint32_t>(x);
    y0 = static_cast<uint32_t>(y);
}

void releaseAtlas() {
    if (g_atlas) { g_atlas->Release(); g_atlas = nullptr; }
    g_atlasFormat = DXGI_FORMAT_UNKNOWN;
    g_bpp = 0;
}

// Declines an array target, a multisampled one, or one too small for even
// one window -- the caller counts the decline. w/h/fmt are the resource's
// own, queried fresh: ResourceInfo (binding_shadow.h) carries width, height
// and format but not ArraySize, and this is the one place that needs it.
bool validate(ID3D11Resource* resource, uint32_t* w, uint32_t* h, DXGI_FORMAT* fmt) {
    if (!resource) return false;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex)
        return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    tex->Release();
    if (td.ArraySize != 1 || td.SampleDesc.Count != 1 || td.Width < kWindow || td.Height < kWindow)
        return false;
    *w = td.Width; *h = td.Height; *fmt = td.Format;
    return true;
}

// Lazily creates the shared atlas from the first target's format; a later
// mismatched format declines rather than resizing mid-run (ONE atlas, the
// design's own word for it). Cheap once created: no device fetch at all
// once g_atlas is set and the format still matches.
bool ensureAtlas(ID3D11DeviceContext* ctx, DXGI_FORMAT fmt) {
    if (g_atlas) return fmt == g_atlasFormat;
    const uint32_t bpp = bytesPerTexel(fmt);
    if (!bpp) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kAtlasCols * kWindow;
    td.Height = kAtlasRows * kWindow;
    td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Format = fmt;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const bool atlasOk = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_atlas));
    dev->Release();
    if (!atlasOk) { releaseAtlas(); return false; }
    g_atlasFormat = fmt;
    g_bpp = bpp;
    return true;
}

void resetRun() {
    g_nextDrawSlot = 0;
    g_draws.clear();
    g_eyesSeenMask = g_drawsIntoEye = g_copied = g_dropped = g_declined = 0;
    g_baselineTaken[0] = g_baselineTaken[1] = false;
    for (auto& row : g_baselineSlot) for (auto& s : row) s = UINT32_MAX;
    for (auto& row : g_lastSlot) for (auto& s : row) s = UINT32_MAX;
}

void logChange(uint32_t frame, int eye, uint32_t point, uint32_t ordinal, const DrawInfo& info,
               uint32_t changed, uint32_t maxDelta) {
    Log::get().note(
        "pixel probe: frame %u eye %d point %u draw #%u vs %016llX ps %016llX topology %u "
        "count %u inst %u blend %s %u %u depth %s %u %u%s changed %u/%u max byte delta %u",
        frame, eye, point, ordinal,
        static_cast<unsigned long long>(info.vsHash), static_cast<unsigned long long>(info.psHash),
        info.topology, info.count, info.instances,
        info.blendEnable ? "on" : "off", info.blendSrc, info.blendDest,
        info.depthEnable ? "on" : "off", info.depthWriteMask, info.depthFunc,
        info.indirect ? " indirect" : "",
        changed, kWindow * kWindow, maxDelta);
}

// Maps the atlas once, diffs every stored slot against the slot before it
// (per point, per eye), logs the draws that changed a window (capped), then
// releases everything. Runs whether or not anything was ever copied, so
// the summary line always exists.
void closeProbedFrame(ID3D11DeviceContext* ctx) {
    uint32_t changedDraws = 0;
    uint32_t logged = 0;
    if (g_atlas && g_copied > 0 && ctx) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(ctx->Map(g_atlas, 0, D3D11_MAP_READ, 0, &map))) {
            for (size_t i = 0; i < g_draws.size(); ++i) {
                DrawRecord& rec = g_draws[i];
                bool any = false;
                for (uint32_t p = 0; p < detail::g_pixelProbePointCount; ++p) {
                    const uint32_t slot = rec.slot[p];
                    if (slot == UINT32_MAX) continue;
                    const uint32_t prev = g_lastSlot[p][rec.eye];
                    g_lastSlot[p][rec.eye] = slot;
                    if (prev == UINT32_MAX) continue;   // nothing to diff the first copy against
                    uint32_t sx, sy, px, py;
                    slotOrigin(slot, sx, sy);
                    slotOrigin(prev, px, py);
                    uint32_t changed = 0, maxDelta = 0;
                    for (uint32_t ty = 0; ty < kWindow; ++ty) {
                        const uint8_t* a = static_cast<const uint8_t*>(map.pData) +
                                           static_cast<size_t>(sy + ty) * map.RowPitch + sx * g_bpp;
                        const uint8_t* b = static_cast<const uint8_t*>(map.pData) +
                                           static_cast<size_t>(py + ty) * map.RowPitch + px * g_bpp;
                        for (uint32_t tx = 0; tx < kWindow; ++tx) {
                            bool texelChanged = false;
                            for (uint32_t bi = 0; bi < g_bpp; ++bi) {
                                const uint32_t delta = static_cast<uint32_t>(std::abs(
                                    static_cast<int>(a[tx * g_bpp + bi]) - static_cast<int>(b[tx * g_bpp + bi])));
                                if (delta) texelChanged = true;
                                if (delta > maxDelta) maxDelta = delta;
                            }
                            if (texelChanged) ++changed;
                        }
                    }
                    if (changed) {
                        any = true;
                        if (logged < kMaxLoggedChanges) {
                            ++logged;
                            logChange(g_targetFrame, rec.eye, p, static_cast<uint32_t>(i) + 1,
                                     rec.info, changed, maxDelta);
                        }
                    }
                }
                if (any) ++changedDraws;
            }
            ctx->Unmap(g_atlas, 0);
        }
    }
    const uint32_t eyesSeen = (g_eyesSeenMask & 1u ? 1u : 0u) + (g_eyesSeenMask & 2u ? 1u : 0u);
    Log::get().note(
        "pixel probe: frame %u done -- eyes seen %u, draws into eye targets %u, copied %u, "
        "dropped %u, declined %u (array/format), draws that changed a window %u.",
        g_targetFrame, eyesSeen, g_drawsIntoEye, g_copied, g_dropped, g_declined, changedDraws);
    releaseAtlas();
    g_draws.clear();
}

}  // namespace

void pixelProbeConfigure(const PixelProbePoint* points, uint32_t count) {
    detail::g_pixelProbePointCount = 0;
    detail::g_pixelProbeActive = false;
    g_armed = false;   // a run in flight never outlives the points it was armed with
    if (!points || !count) return;
    const uint32_t n = count > kPixelProbeMaxPoints ? kPixelProbeMaxPoints : count;
    for (uint32_t i = 0; i < n; ++i) g_points[i] = points[i];
    detail::g_pixelProbePointCount = n;
}

void pixelProbeConfigure(Config& cfg) {
    const std::string spec = cfg.getString("advanced.pixel_probe", "");
    if (spec.empty()) { pixelProbeConfigure(nullptr, 0); return; }
    PixelProbePoint pts[kPixelProbeMaxPoints];
    uint32_t n = 0;
    bool bad = false;
    const char* p = spec.c_str();
    for (;;) {
        const char* semi = std::strchr(p, ';');
        const size_t len = semi ? static_cast<size_t>(semi - p) : std::strlen(p);
        if (len == 0 || n >= kPixelProbeMaxPoints) { bad = true; break; }
        char tok[64];
        if (len >= sizeof(tok)) { bad = true; break; }
        std::memcpy(tok, p, len);
        tok[len] = 0;
        char* end = nullptr;
        const float x = std::strtof(tok, &end);
        if (end == tok || *end != ',') { bad = true; break; }
        char* end2 = nullptr;
        const float y = std::strtof(end + 1, &end2);
        if (end2 == end + 1 || *end2 || !std::isfinite(x) || !std::isfinite(y) ||
            x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) {
            bad = true; break;
        }
        pts[n].x = x; pts[n].y = y; ++n;
        if (!semi) break;
        p = semi + 1;
    }
    if (bad || n == 0) {
        Log::get().note(
            "pixel probe: \"%s\" is not up to %u \"x,y\" points in 0..1 separated by ';'; "
            "refused rather than half-applied.",
            spec.c_str(), static_cast<unsigned>(kPixelProbeMaxPoints));
        return;
    }
    pixelProbeConfigure(pts, n);
}

void pixelProbeArm() {
    if (!detail::g_pixelProbePointCount || g_armed) return;
    g_armed = true;
    g_targetFrame = g_frame + 2;   // the whole next frame; see pixelProbeArm in the header
    resetRun();
    char list[256] = {0};
    for (uint32_t i = 0; i < detail::g_pixelProbePointCount; ++i) {
        char one[32];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%.3f,%.3f", i ? ";" : "",
                   static_cast<double>(g_points[i].x), static_cast<double>(g_points[i].y));
        strcat_s(list, one);
    }
    Log::get().note("pixel probe: armed for frame %u, points %s, window %ux%u.",
                    g_targetFrame, list, kWindow, kWindow);
}

void pixelProbeFrameBoundary(ID3D11DeviceContext* ctx) {
    if (g_armed && g_frame + 1 >= g_targetFrame) {
        if (ctx) closeProbedFrame(ctx);
        g_armed = false;
    }
    ++g_frame;
    detail::g_pixelProbeActive = g_armed && g_frame + 1 == g_targetFrame;
}

void pixelProbeBeforeDraw(ID3D11DeviceContext* ctx, ID3D11Resource* resource, int eye) {
    if (!g_armed || eye < 0 || !ctx || g_frame + 1 != g_targetFrame) return;
    if (g_baselineTaken[eye]) return;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (!validate(resource, &w, &h, &fmt)) { ++g_declined; return; }
    if (!ensureAtlas(ctx, fmt)) { ++g_declined; return; }
    for (uint32_t p = 0; p < detail::g_pixelProbePointCount; ++p) {
        const uint32_t slot = p * 2 + static_cast<uint32_t>(eye);
        uint32_t sx, sy, wx, wy;
        slotOrigin(slot, sx, sy);
        windowOrigin(g_points[p].x, g_points[p].y, w, h, wx, wy);
        const D3D11_BOX box{wx, wy, 0, wx + kWindow, wy + kWindow, 1};
        ctx->CopySubresourceRegion(g_atlas, 0, sx, sy, 0, resource, 0, &box);
        g_baselineSlot[p][eye] = slot;
        g_lastSlot[p][eye] = slot;
        ++g_copied;
    }
    g_baselineTaken[eye] = true;
    g_eyesSeenMask |= (1u << eye);
}

void pixelProbeAfterDraw(ID3D11DeviceContext* ctx, ID3D11Resource* resource, int eye,
                         const DrawInfo& info) {
    if (!g_armed || eye < 0 || !ctx || g_frame + 1 != g_targetFrame) return;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (!validate(resource, &w, &h, &fmt)) { ++g_declined; return; }
    if (!ensureAtlas(ctx, fmt)) { ++g_declined; return; }
    g_eyesSeenMask |= (1u << eye);
    DrawRecord rec;
    rec.eye = eye;
    rec.info = info;
    for (uint32_t p = 0; p < kPixelProbeMaxPoints; ++p) rec.slot[p] = UINT32_MAX;
    for (uint32_t p = 0; p < detail::g_pixelProbePointCount; ++p) {
        if (g_nextDrawSlot >= kMaxDrawSlots) { ++g_dropped; continue; }
        const uint32_t slot = kBaselineSlots + g_nextDrawSlot++;
        uint32_t sx, sy, wx, wy;
        slotOrigin(slot, sx, sy);
        windowOrigin(g_points[p].x, g_points[p].y, w, h, wx, wy);
        const D3D11_BOX box{wx, wy, 0, wx + kWindow, wy + kWindow, 1};
        ctx->CopySubresourceRegion(g_atlas, 0, sx, sy, 0, resource, 0, &box);
        rec.slot[p] = slot;
        ++g_copied;
    }
    g_draws.push_back(rec);
    ++g_drawsIntoEye;
}

void pixelProbeShutdown() {
    releaseAtlas();
    g_draws.clear();
    g_armed = false;
    detail::g_pixelProbeActive = false;
    detail::g_pixelProbePointCount = 0;
}

}  // namespace edvr
