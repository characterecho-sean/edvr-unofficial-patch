// The flat profile's interface census. flat_ui_census.h says what and why; this is the state and the log lines.
// Everything here runs on the render thread (the flat scope admits the owner thread only, the frame boundary is that
// thread's), except the two atomics the panel lines read from the game's creating threads.
#include "flat_ui_census.h"

#include "flat_compute_readback.h"
#include "ui_surfaces.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>

namespace edvr {

namespace {

constexpr uint64_t kWindowMs = 30000;
constexpr uint32_t kRowsLogged = 16;

struct Census {
    bool armed = false;
    uint64_t windowStartMs = 0;
    // The frame, for the phase.
    bool copyDone = false;
    uint32_t outW = 0, outH = 0, renderW = 0, renderH = 0;
    const char* backend = "taa";
    bool injectorOwns = false;
    // The window.
    uint32_t frames = 0, pausedFrames = 0, hdrRouteFrames = 0, copyRouteFrames = 0, untreatedFrames = 0;
    uint64_t drawsSeen = 0, counted = 0, known = 0, unknownBlended = 0, blendQueries = 0, blendSkippedHdrDepth = 0;
    FlatUiRows rows;
};
Census g_c;
std::atomic<uint64_t> g_frameNo{0};
std::atomic<uint32_t> g_outW{0}, g_outH{0}, g_renderW{0}, g_renderH{0};

void report(Census& c, uint64_t nowMs) {
    const double frames = c.frames ? static_cast<double>(c.frames) : 1.0;
    Log::get().note(
        "flat ui census: window=%llus frames=%u (paused %u) D=%ux%u R=%ux%u backend=%s resolve-route hdr=%u copy=%u "
        "untreated=%u jitter-owner=%s draws-seen=%llu counted=%llu (known-family %llu, unknown "
        "blended %llu) blend-queries=%llu hdr-depth-unknown-skipped=%llu rows=%u (overflow %llu); rows below: busiest "
        "%u, per-frame averages",
        static_cast<unsigned long long>((nowMs - c.windowStartMs) / 1000), c.frames, c.pausedFrames, c.outW, c.outH,
        c.renderW, c.renderH, c.backend, c.hdrRouteFrames, c.copyRouteFrames, c.untreatedFrames,
        c.injectorOwns ? "camera-injector" : "phase-machine",
        static_cast<unsigned long long>(c.drawsSeen), static_cast<unsigned long long>(c.counted),
        static_cast<unsigned long long>(c.known), static_cast<unsigned long long>(c.unknownBlended),
        static_cast<unsigned long long>(c.blendQueries), static_cast<unsigned long long>(c.blendSkippedHdrDepth),
        c.rows.used, static_cast<unsigned long long>(c.rows.overflow), kRowsLogged);
    uint32_t order[kRowsLogged];
    const uint32_t n = c.rows.top(order, kRowsLogged);
    for (uint32_t i = 0; i < n; ++i) {
        const FlatUiRow& r = c.rows.row[order[i]];
        char size[24] = "";
        if (r.target == FlatUiTarget::kOffscreen) std::snprintf(size, sizeof(size), " %ux%u", r.w, r.h);
        Log::get().note(
            "flat ui census row %u: vs=%016llX ps=%016llX family=%s target=%s%s phase=%s draws=%llu per-frame=%.2f "
            "jittered=%llu unjittered=%llu late-overlay-protected=%llu",
            i + 1, static_cast<unsigned long long>(r.vs), static_cast<unsigned long long>(r.ps),
            r.family == UiLayerFamily::kNone ? "unknown-blended" : uiLayerFamilyName(r.family),
            flatUiTargetName(r.target), size, flatUiPhaseName(r.phase), static_cast<unsigned long long>(r.draws),
            static_cast<double>(r.draws) / frames, static_cast<unsigned long long>(r.jittered),
            static_cast<unsigned long long>(r.draws - r.jittered), static_cast<unsigned long long>(r.protectedOverlay));
    }
    if (n == 0) Log::get().note("flat ui census row: none counted this window");
    c.windowStartMs = nowMs;
    c.frames = c.pausedFrames = c.hdrRouteFrames = c.copyRouteFrames = c.untreatedFrames = 0;
    c.drawsSeen = c.counted = c.known = c.unknownBlended = c.blendQueries = c.blendSkippedHdrDepth = 0;
    c.rows.clear();
}

// Alpha-blended means a blend state with blending on in slot 0 that keeps some of the destination.
bool blendedNow(ID3D11DeviceContext* ctx) {
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
    FLOAT factor[4] = {};
    UINT mask = 0;
    ctx->OMGetBlendState(&blend, factor, &mask);
    if (!blend) return false;
    D3D11_BLEND_DESC desc{};
    blend->GetDesc(&desc);
    const D3D11_RENDER_TARGET_BLEND_DESC& t = desc.RenderTarget[0];
    return t.BlendEnable && t.DestBlend != D3D11_BLEND_ZERO;
}

}  // namespace

void flatUiCensusFrame(uint64_t frame, bool paused, uint32_t outW, uint32_t outH, uint32_t renderW, uint32_t renderH,
                       const char* backend, bool injectorOwnsJitter, bool prevHdrRoute, bool prevCopyRoute) {
    Census& c = g_c;
    const uint64_t now = GetTickCount64();
    if (!c.armed) {
        uiSurfacesObserveTick();
        c.armed = true;
        c.windowStartMs = now;
        Log::get().note("flat ui census: armed (observation only: counts interface draws by shader pair, target and "
                        "phase against the resolve, panel sizes, and jitter at those draws; a summary every 30 s)");
    } else {
        // The frame that just ended: how it was resolved.
        if (prevHdrRoute) ++c.hdrRouteFrames;
        else if (prevCopyRoute) ++c.copyRouteFrames;
        else ++c.untreatedFrames;
    }
    g_frameNo.store(frame, std::memory_order_relaxed);
    g_outW.store(outW, std::memory_order_relaxed);
    g_outH.store(outH, std::memory_order_relaxed);
    g_renderW.store(renderW, std::memory_order_relaxed);
    g_renderH.store(renderH, std::memory_order_relaxed);
    c.outW = outW;
    c.outH = outH;
    c.renderW = renderW;
    c.renderH = renderH;
    c.backend = backend ? backend : "taa";
    c.injectorOwns = injectorOwnsJitter;
    c.copyDone = false;
    ++c.frames;
    if (paused) ++c.pausedFrames;
    if (now - c.windowStartMs >= kWindowMs) {
        report(c, now);
        uiSurfacesObserveTick();   // Supersampling's refresh and the glyph atlas's write counts, with the window
    }
}

void flatUiCensusDraw(ID3D11DeviceContext* ctx, const FlatUiDrawFacts& f) {
    Census& c = g_c;
    ++c.drawsSeen;
    if (f.copy) {
        c.copyDone = true;
        return;
    }
    if (f.tone) return;
    const FlatUiTarget target = flatUiTargetClass(f.color && f.color == f.output, f.width, f.height, f.format, c.outW,
                                                  c.outH, c.renderW, c.renderH);
    const UiLayerFamily family = flatUiFamilyOf(f.vs, f.ps);
    const bool known = family != UiLayerFamily::kNone;
    bool blended = false;
    if (!known) {
        const bool scene = target == FlatUiTarget::kHdr || target == FlatUiTarget::kLdr ||
                           target == FlatUiTarget::kBackBuffer;
        if (!scene) return;
        // World geometry draws into H with a depth buffer by the thousand; an unknown pair there is not asked.
        if (target == FlatUiTarget::kHdr && f.hasDepth) {
            ++c.blendSkippedHdrDepth;
            return;
        }
        FlatComputeInternalScope guard;
        ++c.blendQueries;
        blended = blendedNow(ctx);
        if (!blended) return;
    }
    if (!flatUiCounted(known, target, blended)) return;
    const FlatUiPhase phase = c.copyDone ? FlatUiPhase::kAfterCopy
                              : f.resolved ? FlatUiPhase::kAfterResolve : FlatUiPhase::kBeforeResolve;
    FlatUiRow key;
    key.vs = f.vs;
    key.ps = f.ps;
    key.target = target;
    key.phase = phase;
    key.family = family;
    if (target == FlatUiTarget::kOffscreen) {
        key.w = f.width;
        key.h = f.height;
    }
    ++c.counted;
    if (known) ++c.known;
    else ++c.unknownBlended;
    c.rows.note(key, f.jittered, f.overlayProtected);
}

uint64_t flatUiCensusFrameNo() { return g_frameNo.load(std::memory_order_relaxed); }

void flatUiCensusSizes(uint32_t* outW, uint32_t* outH, uint32_t* renderW, uint32_t* renderH) {
    if (outW) *outW = g_outW.load(std::memory_order_relaxed);
    if (outH) *outH = g_outH.load(std::memory_order_relaxed);
    if (renderW) *renderW = g_renderW.load(std::memory_order_relaxed);
    if (renderH) *renderH = g_renderH.load(std::memory_order_relaxed);
}

}  // namespace edvr
