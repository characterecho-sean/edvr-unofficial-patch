#pragma once
// Build-only registry: the flat HUD's bloom feed pass (src/d3d11/fixed_shader_source.h kUiGlowFeedPsHlsl, ui_layer.cpp uiLayerGlowFeed) and the star
// light's pass that follows it (kStarGlowAddPsHlsl, visual.star_glow). New, so held to their own contracts (symbol, source name, entry, profile, the
// text they compile) rather than a former runtime call's. Both reuse the coverage pass's fullscreen vertex shader (kUiLayerCoverageVsBytecode).
static std::vector<Variant> glowFeedVariants() { return {
    {"kUiGlowFeedPsBytecode","ui_glow_feed_ps","main",nullptr,{},false,edvr::kUiGlowFeedPsHlsl,"ps_5_0"},
    {"kStarGlowAddPsBytecode","star_glow_add_ps","main",nullptr,{},false,edvr::kStarGlowAddPsHlsl,"ps_5_0"},
}; }
