// The hologram families' vertex-shader hashes. The two modules that must
// agree on what a hologram is now give DIFFERENT answers, and both lists
// live here:
//   - ui_depth.cpp's generic hologram/icon depth pass
//     (the hologram depth pass -- the contribution and
//     element-depth re-issues, whose family lists these seed) covers ALL
//     ELEVEN: kHoloFamiliesBuiltIn's ten cockpit families plus
//     kHoloWorldMarkers, radius-clipping the cockpit ones.
//   - the crisp-HUD half of fix.ui_quality (ui_layer_math.h's
//     uiLayerFamilyFor, Phase 3 of docs/cockpit-hud-layer-design-2026-09-27.md)
//     admits kHoloFamiliesTake's eight proven-safe families, through
//     uiHoloGenericHash below, naming them the ONE family kHoloGeneric on
//     the lit HDR target.
// Restricted families are documented below, including the review's
// citations (reviews/crisp-hud-phase3-review-2026-09-28.md, findings R1/R2).
//
// They lived file-local in ui_depth.cpp until the take needed the same
// names -- spelling both lists here, beside each other, is what keeps the
// take and the depth pass from drifting apart silently about what a
// hologram is. Moved, not copied, in draw_state_describe.h's pattern:
// ui_depth.cpp includes this header, and the per-family commentary below is
// its own, moved with the constants.
#pragma once

#include <cstdint>

namespace edvr {

// The generic hologram/icon depth pass's other built-in families (ui_depth.cpp,
// "GENERIC HOLOGRAM/ICON DEPTH COVERAGE"): the radar's star icon core, its
// two stalks, and the sun's corona family, which also paints the icon's
// glow. Unlike kHoloPanel these carry no per-family coverage shader at
// all -- eye dump eye_135907, frame 17847: the radar star icon over sky
// carried the sky's motion at 0% AA depth coverage.
constexpr uint64_t kHoloIconCore     = 0xF8D8A92E96419901ull;
constexpr uint64_t kHoloCoronaFamily = 0xD1281DF454A153ADull;
constexpr uint64_t kHoloIconStalkA   = 0xDF3503CD07F9B10Cull;
constexpr uint64_t kHoloIconStalkB   = 0x5453D19B6D362364ull;
// The target hologram's sphere (two premultiplied quads, ps EA02FAC2BD6C643C
// and E95634B0F61D218F) and the radar's five contact-marker families --
// flight 20260924_155636, eye dump eye_155832: unlisted, the target sphere
// carried the sky's motion at (-3.9,-6.1) px/frame rolling, and the contact
// bars (pool\draws_155832.bin, frame 8548, right after the two stalks in each
// eye's cockpit section) read 0% contribution.
constexpr uint64_t kHoloTargetSphere = 0x5559BD94B6852E83ull;
constexpr uint64_t kHoloContactA     = 0xA2C2D5510BF1926Dull;
constexpr uint64_t kHoloContactB     = 0x9B34C331902DC1EDull;
constexpr uint64_t kHoloContactC     = 0x9611A454527F7FEBull;
constexpr uint64_t kHoloContactD     = 0xB932058F26B76691ull;
constexpr uint64_t kHoloContactE     = 0x94D5C556DFD6D705ull;
// The target reticle's three 3D triangles (72 non-indexed vertices, three
// prisms -- pool\draws_163515.bin, frame 27528, right after the canopy):
// a WORLD MARKER, not a cockpit family. It tracks the targeted ship, which
// can be kilometres out, so the depth pass never radius-clips it like the
// families above -- eye dump eye_163515: sky MV (+0.59,-0.89) against the
// bracketed ship's (+0.14,+0.27) at 1.65 km, going indistinct with speed.
constexpr uint64_t kHoloWorldMarkerReticle = 0x71DD8B8B09060A81ull;

// The built-in cockpit list: the ten short-range panel/icon/hologram
// families above.
constexpr uint64_t kHoloFamiliesBuiltIn[10] = {kHoloIconCore, kHoloCoronaFamily,
                                               kHoloIconStalkA, kHoloIconStalkB,
                                               kHoloTargetSphere, kHoloContactA, kHoloContactB,
                                               kHoloContactC, kHoloContactD, kHoloContactE};
// WORLD MARKERS: a second, separate built-in list for draws that must be
// covered wherever they are, not just inside the cockpit radius (the
// families above are all short-range panel/icon geometry; a world marker
// tracks something that can be kilometres out). Fixed -- see
// holoWorldMarkerList in ui_depth.cpp.
constexpr uint64_t kHoloWorldMarkers[1] = {kHoloWorldMarkerReticle};
constexpr uint32_t kHoloWorldMarkerCount = static_cast<uint32_t>(sizeof(kHoloWorldMarkers) / sizeof(kHoloWorldMarkers[0]));

// The canopy sits in front of the whole sky; covering it would smear the
// stars behind it. The depth pass leaves it out of its
// list (holoBuildFamilyList in ui_depth.cpp), and the crisp take refuses it the same way: it is not one
// of the take's eight uiHoloGenericHash matches, so a canopy draw names no
// family and stays stock.
constexpr uint64_t kHoloCanopy = 0x8C091FFD08644E02ull;

// ---------------------------------------------------------------------
// THE CRISP TAKE's list. The take (ui_layer_math.h's uiLayerFamilyFor,
// through uiHoloGenericHash below) admits ONLY these eight -- the radar's
// star icon core, its two stalks, and the five contact markers: families
// whose PSes sample only material/surface textures through interpolated
// UVs. Two other depth-pass families need separate handling:
//
// PAIR-GATED: the target hologram's sphere (kHoloTargetSphere, R1 of
// reviews/crisp-hud-phase3-review-2026-09-28.md). Only its two verified
// premultiplied-quad PSes (EA02FAC2BD6C643C, E95634B0F61D218F)
// integer-Load scene depth at the SV_Position pixel (ftoi + ld ... t1);
// the take's viewport remap to the larger HUD layer breaks that addressing
// (WARP-reproduced: 84% of the controlled image gone at the flight's 2.5x
// layer scale). uiLayerFamilyFor checks the exact pair separately; the
// production take prepares a DXBC address remap before admission and
// validates the original linear-depth SRV at issue. Unsupported bindings
// retain the stock draw. The VS-only list still refuses all sphere PSes.
//
// REFUSED: the sun's corona family (kHoloCoronaFamily, R2 of the same
// review). One shader pair paints BOTH the radar icon's glow AND the real
// sun's corona -- the captured far-star draw's VS b1[125].yzw carries
// kilometer-scale center and radius. The depth pass radius-clips exactly
// this ambiguity (that test exists to keep the real sun out:
// docs/hologram-depth-2026-09-24.md:536-537); the take has no radius
// concept, and a VS/PS hash alone cannot tell the two uses apart. Until
// per-draw range discrimination exists the take refuses the family, so the
// sun can never be taken as HUD; the radar glow's crisp take is the
// accepted casualty.
//
// The world-marker reticle stays outside the crisp take. Its pre-existing
// depth/motion handling is separate from these cockpit families.
constexpr uint64_t kHoloFamiliesTake[8] = {kHoloIconCore,   kHoloIconStalkA,
                                           kHoloIconStalkB, kHoloContactA,
                                           kHoloContactB,   kHoloContactC,
                                           kHoloContactD,   kHoloContactE};

// The crisp take's VS-only hologram set: the eight families above. Exact
// target-sphere pairs are checked separately by uiLayerFamilyFor.
// (kHoloPanel is NOT one of them: the take has named it kHolo since
// Phase 1.) uiLayerFamilyFor names all eight the ONE family kHoloGeneric
// on the lit HDR target -- one "hologram" row in the 30 s table.
inline bool uiHoloGenericHash(uint64_t vs) {
    for (uint64_t h : kHoloFamiliesTake)
        if (h == vs) return true;
    return false;
}

}  // namespace edvr
