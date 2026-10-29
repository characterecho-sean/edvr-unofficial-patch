// The HUD's bloom in the headset: the rules, pure (no D3D, no globals). vr_star_glow.cpp drives them (the module that already finds an eye's bloom first level and
// binds H' for it), tools\ui_quality_test drives every function here. docs/cockpit-hud-layer-design-2026-09-27.md, "2026-10-28: the HUD's bloom in the headset".
//
// WHAT. fix.ui_quality takes the cockpit's holo panels, flight HUD and target sprite out of an eye's HDR target H into that eye's HDR HUD layer, and Elite's
// bloom chain reads H, so the HUD lost its halo (flat lost it the same way and got it back on 2026-10-21: flat_ui_layer.h uiLayerGlowFeed). At the eye's bloom
// first level (pixel shader 143AAE05..., reading that eye's H at t0) the draw reads H' = H + the eye's HUD layer's light instead of H, for that one draw.
//
// TONEMAPPED ONCE. H' is read by the first level alone, which thresholds and downsamples: no tonemap reads it. The scene's own tonemap reads the real H plus the
// bloom (which now carries the HUD's halo) and runs once; the HUD's body is in the layer alone, tonemapped once by the re-issue with the scene's bloom zeroed
// (the "M with the scene's bloom zeroed" count of the `crisp hud:` line), and composited over the frame. Nothing is drawn into H, so nothing is tonemapped twice.
//
// WHAT IS HERE. The mode (on, paused for Elite's Bloom being off, off), the window of counts one first-level draw adds to, and the lines that print them. Which
// eye's layer an H is (UiHeadsetHud) is ui_layer_math.h's; the feed is ui_layer.cpp's.
#pragma once

#include "../common/star_glow_bloom.h"
#include "flat_star_glow_math.h"   // FlatStarText
#include "ui_layer_math.h"         // UiHeadsetHud, UiGlowFeed

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

// ---- when it runs --------------------------------------------------------------------------------------------------------------
// ON: the HDR HUD take is armed (the layer's crisp take: fix.ui_quality with a temporal mode, not stood down) and Elite's Bloom is not known to be off (unknown
// runs, as the star glow does: a wrong pause is worse than a wasted compare). PAUSED: armed, but Bloom is known off, so there is no first level to feed. OFF: the
// take is not armed, so no eye's layer holds anything. There is no key: a fix that always helps gets none.
enum class VrHudGlowMode : uint8_t { kOff = 0, kOn, kPaused };
inline VrHudGlowMode vrHudGlowMode(bool takeArmed, sgb::Bloom bloom) {
    if (!takeArmed) return VrHudGlowMode::kOff;
    return bloom == sgb::Bloom::Off ? VrHudGlowMode::kPaused : VrHudGlowMode::kOn;
}

// ---- the window ----------------------------------------------------------------------------------------------------------------
// Why a first-level draw's H got no HUD, in UiHeadsetHud order after kFeed (no-layer, no-content, tone-done, repeat); and how a feed the pass could not make
// came out, by UiGlowFeed after kFed (no-hud, not-ready, binding-not-h, texture, format, fault).
constexpr size_t kVrHudNotFed = 4;
constexpr size_t kVrHudPassFailed = static_cast<size_t>(UiGlowFeed::kCount) - 1;
struct VrHudGlowWindow {
    uint64_t eyeFrames[2] = {};        // eye-frames that took HUD draws into the layer in the window (uiLayerHudEyeFrames, by eye)
    uint64_t firstLevels = 0;          // first-level draws the hook looked at while a HUD waited
    uint64_t fed[2] = {};              // ... that read H' with the eye's HUD in it, by eye
    uint64_t notFed[kVrHudNotFed] = {};
    uint64_t passFailed[kVrHudPassFailed] = {};
};

// One first-level draw the hook looked at: `pick` is the layer's verdict for the H it read, `eye` the eye whose layer it is, `hudFed` the HUD's light is in the H'
// that was bound, `verdict` the pass's UiGlowFeed (as an int). A draw that was picked for a feed and came to none is a failed pass, never a silent miss.
inline void vrHudCount(VrHudGlowWindow& w, UiHeadsetHud pick, int eye, bool hudFed, int verdict) {
    ++w.firstLevels;
    switch (pick) {
        case UiHeadsetHud::kFeed:
            if (hudFed && eye >= 0 && eye < 2) {
                ++w.fed[eye];
            } else {
                const int i = verdict - 1;
                if (i >= 0 && static_cast<size_t>(i) < kVrHudPassFailed) ++w.passFailed[i];
            }
            break;
        case UiHeadsetHud::kNoLayer: ++w.notFed[0]; break;
        case UiHeadsetHud::kNoContent: ++w.notFed[1]; break;
        case UiHeadsetHud::kToneDone: ++w.notFed[2]; break;
        case UiHeadsetHud::kRepeat: ++w.notFed[3]; break;
        default: break;
    }
}

// The 10 s line, printed while the mode is ON. A cockpit with Bloom on, one first level an eye a frame, reads hud-eye-frames[l,r] = fed[l,r] and first-levels = their
// sum, with every not-fed count zero. fed below hud-eye-frames is a HUD whose eye-frame no first level took (not-fed says why); first-levels = 0 with hud-eye-frames above zero
// is a hook that never saw the draw (the pixel shader differs, or Bloom is off, or the draw is not one of the four thunks').
inline int vrHudGlowWindowLine(char* out, size_t n, const VrHudGlowWindow& w) {
    if (!out || n < 16) return 0;
    FlatStarText t(out, n);
    t.put("vr hud glow: window=10s hud-eye-frames[left,right]=");
    t.putList(w.eyeFrames, 2);
    t.put(" first-levels=");
    t.putUnsigned(w.firstLevels);
    t.put(" fed[left,right]=");
    t.putList(w.fed, 2);
    t.put(" not-fed[");
    for (size_t i = 0; i < kVrHudNotFed; ++i) {
        if (i) t.put(",");
        t.put(uiHeadsetHudName(static_cast<UiHeadsetHud>(i + 1)));
    }
    t.put("]=");
    t.putList(w.notFed, kVrHudNotFed);
    t.put(" pass-failed[");
    for (size_t i = 0; i < kVrHudPassFailed; ++i) {
        if (i) t.put(",");
        t.put(uiGlowFeedName(static_cast<UiGlowFeed>(i + 1)));
    }
    t.put("]=");
    t.putList(w.passFailed, kVrHudPassFailed);
    return static_cast<int>(t.used);
}

// ---- the lines that say what the mode did ---------------------------------------------------------------------------------------
inline int vrHudGlowOnLine(char* out, size_t n, uint64_t frame) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n,
                                "vr hud glow: on from frame %llu: fix.ui_quality takes the HUD out of H, so the bloom chain's first level (ps %016llX) of an eye reads H' = H + the "
                                "eye's HUD layer for that one draw; a window line every 10 s names the HUD eye-frames, the first levels and the feeds (first-levels=0 with "
                                "hud-eye-frames above zero: the hook never saw a first level); Elite's Bloom is not known to be off",
                                static_cast<unsigned long long>(frame), 0x143AAE0597E2F7BFull);
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}
inline int vrHudGlowPausedLine(char* out, size_t n) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n, "vr hud glow: paused: Elite's Bloom is known to be off, so the bloom chain has no first level to feed the HUD's light to; nothing runs until Bloom is on");
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}
inline int vrHudGlowOffLine(char* out, size_t n) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n, "vr hud glow: off: the HDR HUD take is not armed (fix.ui_quality or the temporal mode is off, or the layer stood down); nothing runs and nothing is created");
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}
// Said once, at the first feed that put an eye's HUD into H'.
inline int vrHudGlowFedLine(char* out, size_t n, int eye, uint32_t hW, uint32_t hH, uint64_t frame) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n,
                                "vr hud glow: the HUD's light joins the game's bloom -- the first-level draw (ps %016llX) of the %s eye reads H' = H + that eye's HUD layer at t0 "
                                "from frame %llu on (H %ux%u); the HUD's body stays in the layer, tonemapped once by the re-issue with the scene's bloom zeroed, and H' reaches "
                                "no tonemap: the exposure reduction, the composite and every other reader keep H; said once",
                                0x143AAE0597E2F7BFull, eye == 0 ? "left" : "right", static_cast<unsigned long long>(frame), hW, hH);
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}
// Said once, at the first first-level draw picked for a feed whose pass came to nothing.
inline int vrHudGlowFailLine(char* out, size_t n, int eye, int verdict, bool gameViewHeld) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n,
                                "vr hud glow: H' could not be built for the %s eye's first level (the feed's verdict %d = %s; game's view of t0 %s); the draw reads H as the game made "
                                "it, so that eye's HUD has no halo this frame; said once",
                                eye == 0 ? "left" : "right", verdict, uiGlowFeedName(static_cast<UiGlowFeed>(verdict >= 0 && verdict < static_cast<int>(UiGlowFeed::kCount) ? verdict : 99)),
                                gameViewHeld ? "held" : "missing");
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}

}  // namespace edvr
