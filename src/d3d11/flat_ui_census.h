// The flat profile's interface census (design: docs/design-flat-ui-quality-2026-10-05.md, "Evidence before changes").
//
// OBSERVATION ONLY. It changes no pixel the game outputs and no state: counters, and two read-only D3D state
// queries (OMGetBlendState on a few candidate draws). Always on in the flat profile; no config key. To remove it:
// delete this pair, the three call sites in flat_runtime.cpp (search "flatUiCensus"), the one in ui_surfaces.cpp,
// and the build.bat entry.
//
// It answers, for one flat flight, the three things the VR port of fix.ui_quality needs:
//   1. which interface draws enter flat temporal AA: per (vertex shader, pixel shader, target class, phase) counts,
//      where phase is before the resolve / after the resolve / after the output copy;
//   2. what size the game makes its Scaleform panels in flat (ui_surfaces.cpp's chain/atlas instrument in observe
//      mode, with D, R and Supersampling beside each line);
//   3. whether the raster jitter was nonzero at those draws.
//
// Log lines (prefix "flat ui census:"): "armed" once at startup, then a window header and up to 16 rows every 30 s.
// Their absence means the instrument never ran; "armed" with draws-seen=0 means it ran and the hook was not reached.
//
// Since 2026-10-09 fix.ui_quality's flat layer (flat_ui_layer.h) reads its family rule (flatUiFamilyOf) and rides its
// window: the header names the layer's state (ui-layer=), each row counts the draws the layer took and left
// (layer-taken=, layer-left=), and the layer's own "flat ui layer" lines follow the rows. The census itself still
// changes nothing; removing it now means giving the layer those three things.
#pragma once

#include "ui_layer_math.h"

#include <cstdint>
#include <cstring>

struct ID3D11DeviceContext;

namespace edvr {

enum class FlatUiPhase : uint8_t { kBeforeResolve = 0, kAfterResolve, kAfterCopy, kCount };
enum class FlatUiTarget : uint8_t { kHdr = 0, kLdr, kBackBuffer, kOffscreen, kOther, kCount };

inline const char* flatUiPhaseName(FlatUiPhase p) {
    return p == FlatUiPhase::kBeforeResolve ? "before-resolve"
         : p == FlatUiPhase::kAfterResolve  ? "after-resolve" : "after-copy";
}
inline const char* flatUiTargetName(FlatUiTarget t) {
    return t == FlatUiTarget::kHdr ? "hdr-scene" : t == FlatUiTarget::kLdr ? "ldr-scene"
         : t == FlatUiTarget::kBackBuffer ? "back-buffer" : t == FlatUiTarget::kOffscreen ? "offscreen" : "other";
}

// What the hook hands over for one draw (everything the flat scope already holds).
struct FlatUiDrawFacts {
    uint64_t vs = 0, ps = 0;
    const void* color = nullptr;      // the bound colour resource
    const void* output = nullptr;     // the back buffer
    uint32_t width = 0, height = 0, format = 0;   // the colour target
    bool hasDepth = false;
    bool tone = false, copy = false;  // the draw is the game's tone or output-copy pass: never counted
    bool resolved = false;            // this frame's resolve has been done (copy route or HDR route)
    bool jittered = false;            // the raster phase was nonzero at this draw
    bool overlayProtected = false;    // the late HDR overlay protection planned this draw
};

// Target class. Scene-sized means the render size R or the output size D; a size that is neither is an offscreen
// target (a Scaleform panel's render-to-texture, a shadow map, a bloom level). Float formats are the HDR scene.
inline bool flatUiIs8bit(uint32_t f) { return f == 28 || f == 29 || f == 87 || f == 91 || f == 88 || f == 93; }
inline bool flatUiIsFloat(uint32_t f) { return f == 26 || f == 10 || f == 11; }
inline FlatUiTarget flatUiTargetClass(bool isBackBuffer, uint32_t w, uint32_t h, uint32_t format, uint32_t dW, uint32_t dH,
                                      uint32_t rW, uint32_t rH) {
    if (isBackBuffer) return FlatUiTarget::kBackBuffer;
    const bool sceneSized = (w == dW && h == dH) || (rW && w == rW && h == rH);
    if (!sceneSized) return FlatUiTarget::kOffscreen;
    if (flatUiIsFloat(format)) return FlatUiTarget::kHdr;
    if (flatUiIs8bit(format)) return FlatUiTarget::kLdr;
    return FlatUiTarget::kOther;
}

// The existing family rule, asked both ways (an HDR eye target, then the post-tonemap one) with no learned surface:
// the shader-pair recognitions of ui_layer_math.h, not a second hash table.
inline UiLayerFamily flatUiFamilyOf(uint64_t vs, uint64_t ps) {
    UiFamilyFacts f;
    f.vs = vs;
    f.ps = ps;
    f.targetKind = 1;
    UiLayerFamily out = uiLayerFamilyFor(f);
    if (out != UiLayerFamily::kNone) return out;
    f.targetKind = 2;
    return uiLayerFamilyFor(f);
}

// A draw is counted when its pair is a known interface family, or (blend known) when it blends into a scene-sized
// target. Offscreen targets count only for known families.
inline bool flatUiCounted(bool known, FlatUiTarget t, bool blended) {
    if (known) return true;
    return blended && (t == FlatUiTarget::kHdr || t == FlatUiTarget::kLdr || t == FlatUiTarget::kBackBuffer);
}

struct FlatUiRow {
    uint64_t vs = 0, ps = 0;
    uint32_t w = 0, h = 0;            // offscreen targets only
    FlatUiTarget target = FlatUiTarget::kOther;
    FlatUiPhase phase = FlatUiPhase::kBeforeResolve;
    UiLayerFamily family = UiLayerFamily::kNone;
    uint64_t draws = 0, jittered = 0, protectedOverlay = 0;
    uint64_t layerTaken = 0, layerRefused = 0;  // fix.ui_quality's flat layer (flat_ui_layer.h): drawn into it, or left
};

// Fixed table, linear probe by key: a window holds a few dozen distinct rows.
struct FlatUiRows {
    static constexpr uint32_t kCap = 192;
    FlatUiRow row[kCap];
    uint32_t used = 0;
    uint64_t overflow = 0;
    // The row's index, or -1 when the table is full (counted as overflow).
    int note(const FlatUiRow& key, bool jittered, bool protectedOverlay) {
        for (uint32_t i = 0; i < used; ++i) {
            FlatUiRow& r = row[i];
            if (r.vs == key.vs && r.ps == key.ps && r.target == key.target && r.phase == key.phase &&
                r.w == key.w && r.h == key.h) {
                ++r.draws;
                r.jittered += jittered ? 1u : 0u;
                r.protectedOverlay += protectedOverlay ? 1u : 0u;
                return static_cast<int>(i);
            }
        }
        if (used >= kCap) { ++overflow; return -1; }
        FlatUiRow& r = row[used++];
        r = key;
        r.draws = 1;
        r.jittered = jittered ? 1u : 0u;
        r.protectedOverlay = protectedOverlay ? 1u : 0u;
        return static_cast<int>(used - 1);
    }
    // The layer's answer for a draw this window's row `index` counted (a stale or -1 index is ignored).
    void noteLayer(int index, bool taken) {
        if (index < 0 || static_cast<uint32_t>(index) >= used) return;
        if (taken) ++row[index].layerTaken;
        else ++row[index].layerRefused;
    }
    void clear() { used = 0; overflow = 0; }
    // Indices of the n busiest rows, busiest first. Returns the count written.
    uint32_t top(uint32_t* out, uint32_t n) const {
        bool taken[kCap] = {};
        uint32_t c = 0;
        for (; c < n; ++c) {
            int best = -1;
            for (uint32_t i = 0; i < used; ++i)
                if (!taken[i] && (best < 0 || row[i].draws > row[best].draws)) best = static_cast<int>(i);
            if (best < 0) break;
            taken[best] = true;
            out[c] = static_cast<uint32_t>(best);
        }
        return c;
    }
};

// ---- the call sites (flat_ui_census.cpp) ----

// Once per frame, at the frame boundary, before the new frame's flags reset. prevHdrRoute / prevCopyRoute name how
// the frame that just ended was resolved (neither: untreated). Prints "armed" on the first call, the window every 30 s.
void flatUiCensusFrame(uint64_t frame, bool paused, uint32_t outW, uint32_t outH, uint32_t renderW, uint32_t renderH,
                       const char* backend, bool injectorOwnsJitter, bool prevHdrRoute, bool prevCopyRoute);
// Once per watched draw, from the flat draw scope after the late-overlay plan is known. Reads the context only for a
// candidate unknown pair (OMGetBlendState). Returns the draw's row in this window, or -1 (not counted, or table full).
int flatUiCensusDraw(ID3D11DeviceContext* ctx, const FlatUiDrawFacts& facts);
// fix.ui_quality's flat layer took the draw counted at `row` (taken), or left it in the game's frame. Same draw, same
// window: the scope that counted it asks, before the frame boundary can start a new window.
void flatUiCensusLayer(int row, bool taken);
// The frame the census last saw (ui_surfaces.cpp's panel lines carry it), and D / R for the same lines.
uint64_t flatUiCensusFrameNo();
void flatUiCensusSizes(uint32_t* outW, uint32_t* outH, uint32_t* renderW, uint32_t* renderH);

}  // namespace edvr
