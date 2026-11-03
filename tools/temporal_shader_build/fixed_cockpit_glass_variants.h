#pragma once
// Build-only registry: visual.cockpit_glass = legacy's canopy pixel shader (src/d3d11/cockpit_glass_shader.h, docs\cockpit-glass-legacy-port.md). New, so held to its own
// contract (symbol, source name, entry, profile, flags, the text it compiles, and what its DXBC reflects: the game's own input signature, element for element) in
// temporal_shader_build's self-test; tools/cockpit_glass_test runs the compiled bytes on a synthetic canopy pixel. Optimization level 3: the canopy shades every frame.
#include "../../src/d3d11/cockpit_glass_shader.h"
static std::vector<Variant> cockpitGlassVariants() { return {
    {"kCockpitGlassLegacyPsBytecode","cockpit_glass_legacy_ps","main",nullptr,{},false,edvr::kCockpitGlassLegacyPsHlsl,"ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0},
}; }
