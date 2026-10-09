// Fixed core shaders: former runtime call contracts remain independently checked below.
#pragma once
static std::vector<Variant> coreVariants(const std::string& core) {
    static const std::string screen = edvr::screenMotionPsSource(core.c_str());
    static const std::string corona = "#define CORONA_MOTION 1\n" + std::string(edvr::kSmokeDepthHlsl);
    return {
        {"kUiLayerCompositeBytecode", "ui_layer_composite_cs", "main", nullptr, {}, false, edvr::kUiLayerCompositeHlsl, "cs_5_0"},
        {"kUiLayerCoverageVsBytecode", "ui_layer_coverage_vs", "main", nullptr, {}, false, edvr::kUiLayerCoverageVsHlsl, "vs_5_0"},
        {"kUiLayerCoveragePsBytecode", "ui_layer_coverage_ps", "main", nullptr, {}, false, edvr::kUiLayerCoveragePsHlsl, "ps_5_0"},
        {"kUiSeedVsBytecode", "ui_layer_seed", "main", nullptr, {}, false, edvr::kUiSeedVs, "vs_5_0"},
        {"kUiSeedDepthStencilBytecode", "ui_layer_seed", "main", nullptr, {}, false, edvr::kUiSeedPsDepthStencil, "ps_5_0"},
        {"kUiSeedDepthOnlyBytecode", "ui_layer_seed", "main", nullptr, {}, false, edvr::kUiSeedPsDepthOnly, "ps_5_0"},
        {"kUiSeedDepthNoStencilBytecode", "ui_layer_seed", "main", nullptr, {}, false, edvr::kUiSeedPsDepthNoStencil, "ps_5_0"},
        {"kUiDepthPanelBytecode", "ui_depth_panel_ps", "main", nullptr, {}, false, edvr::kPanelDepthHlsl, "ps_5_0"},
        {"kUiDepthHudBytecode", "ui_depth_hud_ps", "main", nullptr, {}, false, edvr::kHudDepthHlsl, "ps_5_0"},
        {"kUiDepthScreenBytecode", "ui_depth_screen_ps", "main", nullptr, {}, false, edvr::kScreenDepthHlsl, "ps_5_0"},
        {"kUiDepthHoloBytecode", "ui_depth_holo_ps", "main", nullptr, {}, false, edvr::kHoloDepthHlsl.c_str(), "ps_5_0"},
        {"kUiDepthSmokeBytecode", "ui_depth_smoke_ps", "main", nullptr, {}, false, edvr::kSmokeDepthHlsl, "ps_5_0"},
        {"kUiDepthSpriteBytecode", "ui_depth_sprite_ps", "main", nullptr, {}, false, edvr::kSpriteDepthHlsl, "ps_5_0"},
        {"kUiDepthRingBytecode", "ring_coverage_ps", "main", nullptr, {}, false, edvr::kRingCoverage, "ps_5_0"},
        {"kUiDepthOrbitalBytecode", "orbital_coverage_ps", "main", nullptr, {}, false, edvr::kOrbitalCoveragePs, "ps_5_0"},
        {"kUiDepthHoloUnlitBytecode", "ui_depth_holo_unlit_ps", "main", nullptr, {}, false, edvr::kHoloUnlitDepthHlsl.c_str(), "ps_5_0"},
        {"kUiHoloResolveVsBytecode", "ui_depth_holo_resolve_vs", "main", nullptr, {}, false, edvr::kHoloResolveVsHlsl, "vs_5_0"},
        {"kUiHoloResolvePsBytecode", "ui_depth_holo_resolve_ps", "main", nullptr, {}, false, edvr::kHoloResolvePsHlsl, "ps_5_0"},
        {"kUiHoloNearLightBytecode", "ui_depth_holo_near_light_cs", "main", nullptr, {}, false, edvr::kHoloNearLightCsHlsl, "cs_5_0"},
        {"kUiHoloMarkerReticleBytecode", "ui_depth_holo_marker_reticle_ps", "main", nullptr, {}, false, edvr::kHoloMarkerReticleDepthPsHlsl, "ps_5_0"},
        {"kUiOrbitalCoverageVsBytecode", "orbital coverage", "main", nullptr, {}, false, edvr::kOrbitalCoverageVs, "vs_5_0"},
        {"kUiSmokeCoronaBytecode", "ui_depth_corona_ps", "main", nullptr, {}, false, corona.c_str(), "ps_5_0"},
        {"kNightExteriorBytecode", "night exterior", "main", nullptr, {}, false, edvr::kNightExteriorCs, "cs_5_0"},
        {"kScreenMotionBytecode", "screen motion", "main", nullptr, {}, false, screen.c_str(), "ps_5_0"},
        {"kWeaponMotionVsBytecode", "weapon motion", "main", nullptr, {}, false, edvr::kWeaponMotionVs, "vs_5_0"},
        {"kWeaponMotionPsBytecode", "weapon motion", "main", nullptr, {}, false, edvr::kWeaponMotionPs, "ps_5_0"},
        {"kWeaponIdentityBytecode", "weapon identity", "main", nullptr, {}, false, edvr::kWeaponIdentityCs, "cs_5_0"},
        {"kPlanetCoverageBytecode", "planet coverage", "main", nullptr, {}, false, edvr::kPlanetCoverageHlsl, "ps_5_0"},
        {"kTemporalFoveaBytecode", "temporal_fovea_cs", "fovea", nullptr, {}, false, edvr::kFoveaCsHlsl, "cs_5_0"},
        {"kTemporalDownBytecode", "temporal_down_cs", "down", nullptr, {}, false, edvr::kDownCsHlsl, "cs_5_0"},
    };
}

static std::vector<Variant> foregroundVariants() {
    return {
        {"kFlatForegroundMotionVsBytecode", "flat foreground motion", "main", nullptr, {}, false, edvr::kFlatForegroundMotionVs, "vs_5_0"},
        {"kFlatForegroundMotionPsBytecode", "flat foreground motion", "main", nullptr, {}, false, edvr::kFlatForegroundMotionPs, "ps_5_0"},
        {"kFlatNullWorldMarkerPsBytecode", "flat null world marker", "main", nullptr, {}, false, edvr::kFlatNullWorldMarkerPs, "ps_5_0"},
        {"kFlatNullForeignMarkerPsBytecode", "flat null foreign marker", "main", nullptr, {}, false, edvr::kFlatNullForeignMarkerPs, "ps_5_0"},
        {"kFlatNullForeignProvenanceMarkerPsBytecode", "flat null foreign provenance marker", "main", nullptr, {}, false, edvr::kFlatNullForeignProvenanceMarkerPs, "ps_5_0"},
        {"kFlatNullPoolMarkerPsBytecode", "flat null pool marker", "main", nullptr, {}, false, edvr::kFlatNullPoolMarkerPs, "ps_5_0"},
    };
}

// The supercruise bars' strip shader (docs/ui-layer-2026-09-23.md, "2026-10-07"): the proxy's first geometry shader. No former
// runtime compile exists to hold it to (it is new), so the self-test checks the symbol, source name, entry, profile, source identity
// and the stage its DXBC reflects instead (the foreground group's checks).
static std::vector<Variant> supercruiseVariants() {
    return {
        {"kSupercruiseBarsGsBytecode", "supercruise bars strip", "main", nullptr, {}, false, edvr::kSupercruiseBarsGs, "gs_5_0"},
    };
}

// The sibling pass of the flat foreground map (src/d3d11/flat_foreground_motion_shader.h, design doc section 104): four compute shaders, new, held to
// their own contract (symbol, source name, entry, profile, the text they compile and the thread groups and bindings their DXBC reflects).
static std::vector<Variant> siblingVariants() {
    return {
        {"kFlatForegroundDonorBytecode", "flat foreground donor", "main", nullptr, {}, false, edvr::kFlatForegroundDonorCs, "cs_5_0"},
        {"kFlatForegroundFitBytecode", "flat foreground fit", "main", nullptr, {}, false, edvr::kFlatForegroundFitCs, "cs_5_0"},
        // The sibling model's shadow (flat_foreground_shadow.h): the moments of each draw that matched, then each draw's leave-one-out measurement.
        {"kFlatForegroundShadowMomentsBytecode", "flat foreground shadow moments", "main", nullptr, {}, false, edvr::kFlatForegroundShadowMomentsCs, "cs_5_0"},
        {"kFlatForegroundShadowEvalBytecode", "flat foreground shadow evaluation", "main", nullptr, {}, false, edvr::kFlatForegroundShadowEvalCs, "cs_5_0"},
    };
}

static std::vector<LegacyContract> coreLegacyContracts() {
    return {
        {"ui_layer_composite_cs", "main", "cs_5_0", nullptr, 0x237EEBB920BC9015ull},
        {"ui_layer_coverage_vs", "main", "vs_5_0", nullptr, 0xC0A9BDF65E71A8DCull},
        {"ui_layer_coverage_ps", "main", "ps_5_0", nullptr, 0xA12E6D166D50BDA1ull},
        {"ui_layer_seed", "main", "vs_5_0", nullptr, 0xCC059AB4FCFA26BCull},
        {"ui_layer_seed", "main", "ps_5_0", nullptr, 0x6204DC7B89FEB995ull},
        {"ui_layer_seed", "main", "ps_5_0", nullptr, 0xB4E15991FFB22394ull},
        {"ui_layer_seed", "main", "ps_5_0", nullptr, 0xD5CC09085C1E6153ull},
        {"ui_depth_panel_ps", "main", "ps_5_0", nullptr, 0x1D917A3C0D1713B4ull},
        {"ui_depth_hud_ps", "main", "ps_5_0", nullptr, 0xBF9B75865E9F9E09ull},
        {"ui_depth_screen_ps", "main", "ps_5_0", nullptr, 0xF5002C2A63664430ull},
        {"ui_depth_holo_ps", "main", "ps_5_0", nullptr, 0x5F77A92009D3B66Cull},
        {"ui_depth_smoke_ps", "main", "ps_5_0", nullptr, 0xC85041994740A190ull},
        {"ui_depth_sprite_ps", "main", "ps_5_0", nullptr, 0x6C9C1A2DEE91E117ull},
        {"ring_coverage_ps", "main", "ps_5_0", nullptr, 0xF5BF48ECBB489351ull},
        {"orbital_coverage_ps", "main", "ps_5_0", nullptr, 0x8BF4DF22F42F0472ull},
        {"ui_depth_holo_unlit_ps", "main", "ps_5_0", nullptr, 0xBF9BF9DF94D733DBull},
        {"ui_depth_holo_resolve_vs", "main", "vs_5_0", nullptr, 0x20645D8AD5F63813ull},
        {"ui_depth_holo_resolve_ps", "main", "ps_5_0", nullptr, 0xA0BA9F47CC37A5BAull},
        {"ui_depth_holo_near_light_cs", "main", "cs_5_0", nullptr, 0xB1BF8BF2F8942B44ull},
        {"ui_depth_holo_marker_reticle_ps", "main", "ps_5_0", nullptr, 0x6EE982850EC312BBull},
        {"orbital coverage", "main", "vs_5_0", nullptr, 0xE791DFD3733D1EBAull},
        {"ui_depth_corona_ps", "main", "ps_5_0", nullptr, 0x0372DA1389F36CE0ull},
        {"night exterior", "main", "cs_5_0", nullptr, 0x28453D6A0BAB0138ull},
        // Re-pinned 2026-10-08 on purpose: the character rule (an uncovered stencil texel beyond kFirstPersonReachDepth is a world
        // pixel), the same rule as the flat prep's. Was 0x422615E32B4FE096.
        // Re-pinned again 2026-10-08 on purpose (F2): this pixel shader is the engine core text plus its own tail, and the core gained the
        // second skin's reprojection (engineReprojectRowsE, the skinned branch of enginePixelZ, the gCount bins). Nothing in this shader
        // calls them; the pin moves because the text it is assembled from did. Was 0x8C2F828BE3751ABB.
        // Re-pinned 2026-10-09 on purpose (F2 on foot, F15): the on-foot eye-route fallback now takes a skinned character's exact motion from the source's
        // target 7 (SourceSkin at t15, engine.w, kind 7 counted as joined and in its own slot, painted as joined). Was 0xAA505601226395B7.
        {"screen motion", "main", "ps_5_0", nullptr, 0x47B44C452FE3B59Bull},
        {"weapon motion", "main", "vs_5_0", nullptr, 0x7232767DAC4ADBD4ull},
        {"weapon motion", "main", "ps_5_0", nullptr, 0xF948E51A2E036952ull},
        {"weapon identity", "main", "cs_5_0", nullptr, 0x10E05DE79500471Dull},
        {"planet coverage", "main", "ps_5_0", nullptr, 0xAC1BF855670403D4ull},
        {"temporal_fovea_cs", "fovea", "cs_5_0", nullptr, 0xE05D6C08CF233167ull},
        {"temporal_down_cs", "down", "cs_5_0", nullptr, 0x446A3A4BBB585FFAull},
    };
}
