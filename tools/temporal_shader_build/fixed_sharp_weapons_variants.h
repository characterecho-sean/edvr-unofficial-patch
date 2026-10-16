#pragma once
// Build-only registry: fix.sharp_weapons' source pass (src/d3d11/sharp_weapons_shaders.h). New, so held to its own contract (symbol, source
// name, entry, profile, the text it compiles) rather than a former runtime call's; tools/sharp_weapons_test runs the compiled bytes.
#include "../../src/d3d11/sharp_weapons_shaders.h"
static std::vector<Variant> sharpWeaponsVariants() { return {
    {"kSharpWeaponsSourceVsBytecode","sharp_weapons_source_vs","main",nullptr,{},false,edvr::kSharpWeaponsSourceVsHlsl,"vs_5_0"},
    {"kSharpWeaponsSourcePsBytecode","sharp_weapons_source_ps","main",nullptr,{},false,edvr::kSharpWeaponsSourcePsHlsl,"ps_5_0"},
}; }
