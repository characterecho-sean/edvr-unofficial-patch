#pragma once
// Build-only registry: the SRV turret HUD's two pass-through geometry shaders (src/d3d11/srv_turret_hud_shader.h; issue 140, fix.srv_turret_hud_gap), one for each hologram
// family's output signature. New, so held to their own contract (symbol, source name, entry, profile, the text each compiles, and the stage, signature and constant buffer its
// DXBC reflects) in temporal_shader_build's self-test; tools/srv_turret_hud_test runs the compiled bytes on WARP.
#include "../../src/d3d11/srv_turret_hud_shader.h"
static std::vector<Variant> srvTurretHudVariants() { return {
    {"kSrvTurretHudGsABytecode","srv turret hud pass-through A","main",nullptr,{},false,edvr::kSrvTurretHudGsA,"gs_5_0"},
    {"kSrvTurretHudGsBBytecode","srv turret hud pass-through B","main",nullptr,{},false,edvr::kSrvTurretHudGsB,"gs_5_0"},
}; }
