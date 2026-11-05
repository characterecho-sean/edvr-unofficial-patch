#pragma once
// Build-only registry: visual.cockpit_glass = legacy's canopy pixel shaders (src/d3d11/cockpit_glass_shader.h, docs\cockpit-glass-legacy-port.md): one text, three compiles. The
// intact variant replaces the game's intact canopy PS; the ice and condensation variants are overlays drawn over the game's own ice and condensation draws, each with that
// draw's vertex shader's output signature (four mask components, the dirt and the imperfection in different ones) and its crack and blemish texture slots. New, so held to their
// own contract (symbol, source name, entry, profile, flags, macros, the text, and what each DXBC reflects) in temporal_shader_build's self-test; tools/cockpit_glass_test runs the
// compiled bytes on a synthetic canopy pixel. Optimization level 3: the canopy shades every frame.
#include "../../src/d3d11/cockpit_glass_shader.h"
static const D3D_SHADER_MACRO kCockpitGlassIntactMacros[] = {
    {"EDVR_OVERLAY","0"},{"EDVR_MASKS4","0"},{"EDVR_MASK_DIRT","1"},{"EDVR_MASK_IMP","2"},{"EDVR_T_CRACK","t6"},{"EDVR_T_BLEMISH","t9"},{nullptr,nullptr}};
static const D3D_SHADER_MACRO kCockpitGlassIceMacros[] = {
    {"EDVR_OVERLAY","1"},{"EDVR_MASKS4","1"},{"EDVR_MASK_DIRT","2"},{"EDVR_MASK_IMP","3"},{"EDVR_T_CRACK","t7"},{"EDVR_T_BLEMISH","t11"},{nullptr,nullptr}};
static const D3D_SHADER_MACRO kCockpitGlassCondensationMacros[] = {
    {"EDVR_OVERLAY","1"},{"EDVR_MASKS4","1"},{"EDVR_MASK_DIRT","1"},{"EDVR_MASK_IMP","2"},{"EDVR_T_CRACK","t7"},{"EDVR_T_BLEMISH","t11"},{nullptr,nullptr}};
static std::vector<Variant> cockpitGlassVariants() { return {
    {"kCockpitGlassLegacyPsBytecode","cockpit_glass_legacy_ps","main",kCockpitGlassIntactMacros,{},false,edvr::kCockpitGlassLegacyPsHlsl,"ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0},
    {"kCockpitGlassIceOverlayPsBytecode","cockpit_glass_ice_overlay_ps","main",kCockpitGlassIceMacros,{},false,edvr::kCockpitGlassLegacyPsHlsl,"ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0},
    {"kCockpitGlassCondensationOverlayPsBytecode","cockpit_glass_condensation_overlay_ps","main",kCockpitGlassCondensationMacros,{},false,edvr::kCockpitGlassLegacyPsHlsl,"ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0},
}; }
