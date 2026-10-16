#pragma once
// Build-only registry: the explosion billboards' facing transcriptions (src/d3d11/billboard_facing_vs.h). New, so held to their own
// contract (symbol, source name, entry, profile, the text they compile, the vertex stage and the constant buffers their DXBC reflects)
// rather than a former runtime call's; tools/billboard_facing_test holds their behaviour against the game's own bytecode.
#include "../../src/d3d11/billboard_facing_vs.h"
static std::vector<Variant> billboardFacingVariants() { return {
    {"kFacing9F4BWorldBytecode","facing_9F4B_vs","main",nullptr,{},false,edvr::kFacing9F4BWorldVS,"vs_5_0"},
    {"kFacingBBADWorldBytecode","facing_BBAD_vs","main",nullptr,{},false,edvr::kFacingBBADWorldVS,"vs_5_0"},
    {"kFacing9AECWorldBytecode","facing_9AEC_vs","main",nullptr,{},false,edvr::kFacing9AECWorldVS,"vs_5_0"},
    {"kFacingAFBCWorldBytecode","facing_AFBC_vs","main",nullptr,{},false,edvr::kFacingAFBCWorldVS,"vs_5_0"},
    {"kFacing4BDEWorldBytecode","facing_4BDE_vs","main",nullptr,{},false,edvr::kFacing4BDEWorldVS,"vs_5_0"},
}; }
