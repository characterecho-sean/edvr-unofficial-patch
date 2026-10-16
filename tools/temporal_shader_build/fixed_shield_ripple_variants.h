#pragma once
// Build-only registry: the shield ripple's door pass (src/d3d11/shield_ripple_shaders.h). New, so held to its own
// contract (symbol, source name, entry, profile, the text it compiles); tools/shield_ripple_test runs the compiled bytes.
#include "../../src/d3d11/shield_ripple_shaders.h"
static std::vector<Variant> shieldRippleVariants() { return {
    {"kShieldRippleDoorBytecode","shield_ripple_door_cs","main",nullptr,{},false,edvr::kShieldRippleDoorCsHlsl,"cs_5_0"},
}; }
