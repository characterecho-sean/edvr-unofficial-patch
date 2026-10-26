// The weapons-fire draws fix.sharp_weapons takes into its own additive HDR layer (docs/dlss-blur-elements-2026-10-14.md, "Design,
// 2026-10-16: weapons fire after the upscale" and its "Built" entry): their shader pairs, the jitter-cancel table, the A/B blend key's
// words and the plan of the tonemap re-issues, in one place, pure, for the family rule (ui_layer_math.h), the draw hook (vscreen.cpp),
// the layer (ui_layer.cpp) and the rigs (tools/sharp_weapons_test, tools/ui_quality_test).
//
//   BEAMS AND BOLTS: vs 1F17BF54DB6EE407 / ps A75C1DB6562B8CA7. DrawInstanced-class 4-vertex strips, SRC_ALPHA / ONE, depth GREATER with
//   no write, stencil off, into the lit R11G11B10 HDR eye target (2016x2016 on the 2026-10-16 flight; the DSV is D32S8).
//   MULTICANNON: vs 939D01F28D1FEEA8 / ps C5DF9CC943476289. Indexed 18-index draws, 13 instances, same blend and depth state.
//   SHIELD STRIKE, additive part: vs 5C1D8EF529324A22 / ps C49F999F7D3C801D. 2880 indices, ONE / ONE, depth GREATER_EQUAL with no write,
//   stencil off. Its refracting partner (820E / 6EAA) samples the pre-DLSS eye HDR copy at the screen UV and stays in the scene (ruled
//   out for the layer).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr {

constexpr uint64_t kUiVsWeaponBeams = 0x1F17BF54DB6EE407ull;
constexpr uint64_t kUiPsWeaponBeams = 0xA75C1DB6562B8CA7ull;
constexpr uint64_t kUiVsMulticannon = 0x939D01F28D1FEEA8ull;
constexpr uint64_t kUiPsMulticannon = 0xC5DF9CC943476289ull;
constexpr uint64_t kUiVsShieldStrike = 0x5C1D8EF529324A22ull;
constexpr uint64_t kUiPsShieldStrike = 0xC49F999F7D3C801Dull;

// One weapons-fire pair, and whether its draw is made through the jitter cancel (the viewport offset that takes the eye's sub-pixel jitter
// out of the layer): valid only where the draw's vertex shader carries the jittered scene camera. The table's default is "cancel", as for
// the space dust. SETTLED (2026-10-16, eye dump 100257, docs "W3"): all three pairs' matrices carry the eye's jitter -- 1F17's cb1[41..43] and 5C1D's
// and 939D's CB0 rows 4-7 equal the jittered scene cb1[270..272] bit for bit apart from translation -- so every entry is true. A pair a later
// build finds unjittered is set false HERE and nowhere else.
struct WeaponPair {
    uint64_t vs, ps;
    const char* name;
    bool cancelJitter;
};
constexpr WeaponPair kWeaponPairs[] = {
    {kUiVsWeaponBeams, kUiPsWeaponBeams, "beams and bolts", true},
    {kUiVsMulticannon, kUiPsMulticannon, "multicannon", true},
    {kUiVsShieldStrike, kUiPsShieldStrike, "shield strike", true},
};
constexpr size_t kWeaponPairCount = sizeof(kWeaponPairs) / sizeof(kWeaponPairs[0]);

// The pair's index, or -1: the exact PAIR is the family, never the vertex shader alone.
inline int weaponPairIndex(uint64_t vs, uint64_t ps) {
    for (size_t i = 0; i < kWeaponPairCount; ++i)
        if (kWeaponPairs[i].vs == vs && kWeaponPairs[i].ps == ps) return static_cast<int>(i);
    return -1;
}
inline int weaponPairIndexByVs(uint64_t vs) {
    for (size_t i = 0; i < kWeaponPairCount; ++i)
        if (kWeaponPairs[i].vs == vs) return static_cast<int>(i);
    return -1;
}
// vscreen.cpp reads the pixel shader's hash for these vertex shaders (the pair is the key).
inline bool weaponsFireVs(uint64_t vs) { return weaponPairIndexByVs(vs) >= 0; }
// The jitter-cancel choice of a weapons-fire draw, from the table. Default (a vertex shader the table does not name): cancel.
inline bool weaponsFireJitterCancel(uint64_t vs) {
    const int i = weaponPairIndexByVs(vs);
    return i < 0 ? true : kWeaponPairs[i].cancelJitter;
}

// Was a weapons layer's content DROPPED: the layer holds `draws` draws of frame `contentSeq`, and the last frame a re-issue CARRIED the weapons' light
// into the picture (`publishedSeq`, stamped only by a re-issue whose bracket included the layer's content) is not that frame. Asked at the eye's next
// weapons draw, which may be many frames after the content (a burst's last draw, then quiet). Not the latest TONEMAP's frame: the re-issue runs every
// frame for the HUD, so comparing against it called every burst's last draw lost when the next burst began (the 2026-10-16 10:36 flight's "lost layer"
// lines, one per burst end; docs/dlss-blur-elements-2026-10-14.md, "The never-tonemapped frames: found").
inline bool weaponsLayerDropped(uint64_t contentSeq, uint32_t draws, uint64_t publishedSeq) {
    return contentSeq != 0 && draws != 0 && publishedSeq != contentSeq;
}

// fix.sharp_weapons = on | plain | off. One key for the whole feature (the weapons fire, the shield strike's additive part and the shield ripple):
//   on:    the layer's weapons light reaches the picture by tonemap difference, T(F_up + L) - T(F_up), added onto the upscaled picture: the light is
//          tonemapped WITH the scene it lands on, so it is as bright as the game would have drawn it. The old spellings of on (true, yes, 1) read as on.
//   plain: T(L) added: exact over black space, too bright over a lit background (a planet, a star), where the tonemap compresses.
//   off:   stock, everything drawn in the scene as the game draws it (false, no, 0 read as off).
// Anything else reads as on, and says so.
enum class SharpBlend : uint8_t { kMatched = 0, kPlain };
enum class SharpWeapons : uint8_t { kOn = 0, kPlain, kOff };
inline SharpWeapons sharpWeaponsParse(const char* text, bool* recognized) {
    if (recognized) *recognized = true;
    if (text) {
        char low[16] = {};
        for (size_t i = 0; i + 1 < sizeof(low) && text[i]; ++i) low[i] = static_cast<char>(text[i] >= 'A' && text[i] <= 'Z' ? text[i] - 'A' + 'a' : text[i]);
        if (std::strcmp(low, "on") == 0 || std::strcmp(low, "true") == 0 || std::strcmp(low, "yes") == 0 || std::strcmp(low, "1") == 0) return SharpWeapons::kOn;
        if (std::strcmp(low, "plain") == 0) return SharpWeapons::kPlain;
        if (std::strcmp(low, "off") == 0 || std::strcmp(low, "false") == 0 || std::strcmp(low, "no") == 0 || std::strcmp(low, "0") == 0) return SharpWeapons::kOff;
    }
    if (recognized) *recognized = false;
    return SharpWeapons::kOn;
}
inline bool sharpWeaponsTakes(SharpWeapons m) { return m != SharpWeapons::kOff; }
inline SharpBlend sharpWeaponsBlend(SharpWeapons m) { return m == SharpWeapons::kPlain ? SharpBlend::kPlain : SharpBlend::kMatched; }
inline const char* sharpBlendName(SharpBlend b) { return b == SharpBlend::kPlain ? "plain" : "matched"; }

// ----------------------------------------------------------------- the tonemap re-issues
//
// Per eye-frame the layer re-issues the game's tonemap draw (its exposure, LUT and b2 untouched) over the 8-bit layer E once for every
// step below, the HDR source at the admitted slot and the blend named by the step. Weapons first, the HUD after, so the HUD's
// premultiplied light is ADDED on top of the weapons' contribution (the HUD's replace-draw would erase it):
//
//   HUD alone (the layer as it always was):   [HudReplace]                   E = T(H)                      coverage pass: E.a = H.a
//   weapons, plain:                            clear E, [WeaponsReplace, (HudAdd)]   E = T(W) (+ T(H))
//   weapons, matched:                          clear E, source pass (S = F_up, W += F_up), [WeaponsBase, WeaponsLit, (HudAdd)]
//                                              E = T(S) ; E = T(W) - E = T(F_up + L) - T(F_up) ; (+ T(H))
//
// The source pass is one fullscreen pass with two targets: S (a scratch) takes the game's HDR input upsampled to the layer's size
// (bilinear, the same function for both terms), and W (the weapons layer) takes the same light added on top of what the weapons drew.
enum class ToneStep : uint8_t { kHudReplace = 0, kHudAdd, kWeaponsReplace, kWeaponsBase, kWeaponsLit };

struct TonePlan {
    ToneStep steps[3] = {};
    uint8_t count = 0;
    bool clearEye = false;    // E is cleared to (0,0,0,1) first: no step replaces its alpha
    bool sourcePass = false;  // the F_up pass runs before the first step
    bool coverage = false;    // the HUD's coverage pass runs after the last step (E.a = H.a)
};

// hud: the HDR HUD layer holds this frame's draws; weapons: the weapons layer does; mode: the key; matchedReady: the scratch and the
// source pass exist (when they do not, the frame falls back to the plain add, counted).
inline TonePlan sharpTonePlan(bool hud, bool weapons, SharpBlend mode, bool matchedReady) {
    TonePlan p;
    if (!weapons) {
        if (hud) {
            p.steps[p.count++] = ToneStep::kHudReplace;
            p.coverage = true;
        }
        return p;
    }
    p.clearEye = true;
    if (mode == SharpBlend::kMatched && matchedReady) {
        p.sourcePass = true;
        p.steps[p.count++] = ToneStep::kWeaponsBase;
        p.steps[p.count++] = ToneStep::kWeaponsLit;
    } else {
        p.steps[p.count++] = ToneStep::kWeaponsReplace;
    }
    if (hud) {
        p.steps[p.count++] = ToneStep::kHudAdd;
        p.coverage = true;
    }
    return p;
}

inline const char* toneStepName(ToneStep s) {
    switch (s) {
        case ToneStep::kHudReplace: return "hud-replace";
        case ToneStep::kHudAdd: return "hud-add";
        case ToneStep::kWeaponsReplace: return "weapons-replace";
        case ToneStep::kWeaponsBase: return "weapons-base";
        case ToneStep::kWeaponsLit: return "weapons-lit";
        default: return "?";
    }
}

// Which layer feeds a step: the HUD's H, the weapons' W, or the scratch S.
enum class ToneSource : uint8_t { kHud = 0, kWeapons, kScratch };
inline ToneSource toneStepSource(ToneStep s) {
    switch (s) {
        case ToneStep::kHudReplace:
        case ToneStep::kHudAdd: return ToneSource::kHud;
        case ToneStep::kWeaponsBase: return ToneSource::kScratch;
        default: return ToneSource::kWeapons;
    }
}

// Whether a step keeps the tone draw's scene images (ui_layer_math.h toneInputIsSceneImage: the scene's bloom and the like) and the
// DoF composite's blur bound as the game bound them, or reads zero at the images and its own source at the blur. The HUD's steps and
// the plain weapons replace tonemap the layer's own light, which the frame does not hold, so nothing of the scene may come with it:
// T(H + bloom) added onto a frame that holds T(scene + bloom) is the bloom twice, and T((1-f) H + f blur) the blur twice. The matched
// pair keeps them: T(F_up + L) - T(F_up) is the light tonemapped on the scene it lands on, and that scene includes its bloom and
// blur -- both terms carry them, so the difference holds neither.
inline bool toneStepKeepsSceneImages(ToneStep s) {
    return s == ToneStep::kWeaponsBase || s == ToneStep::kWeaponsLit;
}

// How a step writes E's colour: replace, add (ONE, ONE) or subtract (source minus what E holds). The numbers are D3D11's blend
// operations (ADD 1, SUBTRACT 2), asserted where they meet the API (sharp_weapons.h).
enum class ToneWrite : uint8_t { kReplace = 0, kAdd, kSubtract };
inline ToneWrite toneStepWrite(ToneStep s) {
    switch (s) {
        case ToneStep::kHudAdd: return ToneWrite::kAdd;
        case ToneStep::kWeaponsLit: return ToneWrite::kSubtract;
        default: return ToneWrite::kReplace;
    }
}

}  // namespace edvr
