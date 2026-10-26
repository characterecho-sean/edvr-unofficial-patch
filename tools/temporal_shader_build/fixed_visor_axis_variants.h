#pragma once
// Build-only registry: the on-foot helmet visor's axis-centred vertex shader (src/d3d11/visor_axis_vs.h; F121). New, so held to its own contract (symbol,
// source name, entry, profile, the text it compiles, the vertex stage and the constant buffers and buffer its DXBC reflects); tools/visor_axis_test holds
// its behaviour against the game's own bytecode (VS B553BB479B7C0B97).
#include "../../src/d3d11/visor_axis_vs.h"
static std::vector<Variant> visorAxisVariants() { return {
    {"kVisorAxisVsBytecode","visor_axis_vs","main",nullptr,{},false,edvr::kVisorAxisVS,"vs_5_0"},
}; }
