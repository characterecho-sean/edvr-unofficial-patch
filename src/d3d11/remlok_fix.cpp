#include "remlok_fix.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The overlay's shape, exactly as the census measured and the field
// suppression verified it (2026-08-19): a fullscreen triangle, one instance,
// no depth view bound, PS slot 0 sampling the 1024x512 overlay image. The
// size is the discriminating half -- the tonemap chain is made of otherwise
// identical fullscreen triangles that sample eye-sized textures.
constexpr char     kKind = 'N';   // DrawInstanced, as the census letters it
constexpr uint32_t kVertices = 3;
constexpr uint32_t kInstances = 1;
constexpr uint32_t kSrvW = 1024;
constexpr uint32_t kSrvH = 512;

enum class Mode : uint32_t { kStock, kOuter, kHide };

Mode  g_mode = Mode::kStock;
float g_keep = 0.55f;
bool  g_swap = false;

// Matched draws this frame: 0 is the left eye, 1 the right. Reset at the
// frame boundary so one odd frame cannot invert the pair for the session.
uint32_t g_matchesThisFrame = 0;
bool     g_pendingRight = false;

// The game's rasterizer state, cloned once with ScissorEnable flipped on,
// re-cloned only if the game ever draws the overlay with a different state.
// The original is compared by pointer and never dereferenced after Release.
ID3D11RasterizerState* g_cloneSource = nullptr;
ID3D11RasterizerState* g_clone = nullptr;
bool g_cloneSourceIsDefault = false;

// What begin set, for end to put back. engaged is the contract between the
// two: end restores exactly when begin says it changed something.
bool                   g_engaged = false;
ID3D11RasterizerState* g_savedRS = nullptr;
UINT                   g_savedRectCount = 0;
D3D11_RECT             g_savedRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

uint64_t g_applied = 0;
uint64_t g_hidden = 0;
bool     g_cloneFailedNoted = false;

// The scissor-enabled twin of the game's current rasterizer state. Cached
// against the source pointer; a null source means the default state, whose
// description is spelled out because there is no object to ask.
ID3D11RasterizerState* cloneWithScissor(ID3D11DeviceContext* ctx,
                                        ID3D11RasterizerState* source) {
    if (g_clone && source == g_cloneSource &&
        (source || g_cloneSourceIsDefault)) {
        return g_clone;
    }
    D3D11_RASTERIZER_DESC d{};
    if (source) {
        source->GetDesc(&d);
    } else {
        // The documented defaults of the null rasterizer state.
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.ScissorEnable = TRUE;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11RasterizerState* clone = nullptr;
    const HRESULT hr = dev->CreateRasterizerState(&d, &clone);
    dev->Release();
    if (FAILED(hr) || !clone) {
        if (!g_cloneFailedNoted) {
            g_cloneFailedNoted = true;
            Log::get().note("remlok: could not create the scissor rasterizer "
                            "state (hr=0x%08X); the overlay draws untouched.",
                            static_cast<unsigned>(hr));
        }
        return nullptr;
    }
    if (g_clone) g_clone->Release();
    g_clone = clone;
    g_cloneSource = source;
    g_cloneSourceIsDefault = source == nullptr;
    return g_clone;
}

}  // namespace

void remlokConfigure(Config& cfg) {
    const Mode was = g_mode;
    const std::string m = cfg.getString("fix.remlok_lines", "stock");
    if (m == "stock") {
        g_mode = Mode::kStock;
    } else if (m == "outer") {
        g_mode = Mode::kOuter;
    } else if (m == "hide") {
        g_mode = Mode::kHide;
    } else {
        g_mode = Mode::kStock;
        Log::get().note("remlok_lines \"%s\" is not stock, outer or hide; "
                        "running stock.", m.c_str());
    }

    float keep = cfg.getFloat("advanced.remlok_keep_fraction", 0.55f);
    // Under half and the two eyes' kept regions no longer meet in the
    // middle -- a gap in anything the overlay draws at centre; near 1.0 the
    // nasal line stops being clipped, which is the whole point. Clamp
    // rather than refuse: this is documented as a live-tuned value.
    if (keep < 0.50f) keep = 0.50f;
    if (keep > 0.95f) keep = 0.95f;
    g_keep = keep;
    g_swap = cfg.getBool("advanced.remlok_swap_eyes", false);

    if (was != g_mode) {
        const char* names[] = {"stock", "outer", "hide"};
        Log::get().note("remlok lines: %s. The overlay is recognised by shape "
                        "(3 vertices, 1 instance, no depth, 1024x512 in PS "
                        "slot 0); outer keeps %.0f%% of each eye's width from "
                        "its own temple.",
                        names[static_cast<uint32_t>(g_mode)], g_keep * 100.0f);
    }
}

bool remlokWantsDraws() { return g_mode != Mode::kStock; }

RemlokAction remlokOnEyeDraw(char kind, uint32_t count, uint32_t instances) {
    if (g_mode == Mode::kStock) return RemlokAction::kNone;
    if (kind != kKind || count != kVertices || instances != kInstances) {
        return RemlokAction::kNone;
    }
    // The overlay binds no depth; scene and HUD draws do. Checked before the
    // SRV resolve so the resolve only runs for depthless fullscreen
    // triangles, which are a handful a frame.
    if (bindingGet(BindSlot::Dsv0) != nullptr) return RemlokAction::kNone;
    ResourceInfo info;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &info) ||
        !info.isTexture2D || info.a != kSrvW || info.b != kSrvH) {
        return RemlokAction::kNone;
    }

    const uint32_t match = g_matchesThisFrame++;
    if (g_mode == Mode::kHide) {
        if (++g_hidden == 1) {
            Log::get().note("remlok lines: hidden (first overlay draw "
                            "suppressed this session).");
        }
        return RemlokAction::kHide;
    }
    g_pendingRight = ((match & 1u) != 0u) != g_swap;
    return RemlokAction::kScissor;
}

void remlokScissorBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;

    ID3D11RasterizerState* current = nullptr;
    ctx->RSGetState(&current);   // AddRef'd when non-null
    ID3D11RasterizerState* scissored = cloneWithScissor(ctx, current);
    if (!scissored) {
        if (current) current->Release();
        return;   // the draw runs untouched, which stock already survives
    }

    UINT vpCount = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&vpCount, &vp);
    if (vpCount == 0 || vp.Width <= 0.0f) {
        if (current) current->Release();
        return;
    }

    g_savedRS = current;   // keep the reference until end restores it
    g_savedRectCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&g_savedRectCount, g_savedRects);

    // Each eye keeps the g_keep fraction nearest its own temple: the left
    // eye's rectangle starts at the viewport's left edge, the right eye's
    // ends at its right edge. What is clipped is the nasal remainder, where
    // the wrong-side line sat.
    const LONG x0 = static_cast<LONG>(vp.TopLeftX);
    const LONG y0 = static_cast<LONG>(vp.TopLeftY);
    const LONG w = static_cast<LONG>(vp.Width);
    const LONG h = static_cast<LONG>(vp.Height);
    const LONG keep = static_cast<LONG>(vp.Width * g_keep);
    D3D11_RECT r{};
    r.top = y0;
    r.bottom = y0 + h;
    if (g_pendingRight) {
        r.left = x0 + w - keep;
        r.right = x0 + w;
    } else {
        r.left = x0;
        r.right = x0 + keep;
    }
    ctx->RSSetScissorRects(1, &r);
    ctx->RSSetState(scissored);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("remlok lines: outer mode engaged -- each eye now "
                        "keeps the line at its own temple and loses the one "
                        "along the nose. First applied to the %s eye, %ld of "
                        "%ld px kept.",
                        g_pendingRight ? "right" : "left", keep, w);
    }
}

void remlokScissorEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ctx->RSSetState(g_savedRS);
    if (g_savedRS) {
        g_savedRS->Release();
        g_savedRS = nullptr;
    }
    ctx->RSSetScissorRects(g_savedRectCount,
                           g_savedRectCount ? g_savedRects : nullptr);
}

void remlokFrameBoundary() { g_matchesThisFrame = 0; }

void remlokShutdown() {
    if (g_clone) {
        g_clone->Release();
        g_clone = nullptr;
    }
    g_cloneSource = nullptr;
    if (g_savedRS) {
        g_savedRS->Release();
        g_savedRS = nullptr;
    }
}

}  // namespace edvr
