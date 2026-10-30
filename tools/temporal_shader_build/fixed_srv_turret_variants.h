#pragma once
// Build-only registry: the SRV turret HUD's pass-through geometry shader (src/d3d11/srv_turret_hud_shader.h; issue 140, fix.srv_turret_hud_gap). New, so held to its own
// contract (symbol, source name, entry, profile, the text it compiles, and the stage, signature and constant buffer its DXBC reflects) in temporal_shader_build's self-test;
// tools/srv_turret_hud_test runs the compiled bytes on WARP.
#include "../../src/d3d11/srv_turret_hud_shader.h"
static std::vector<Variant> srvTurretHudVariants() { return {
    {"kSrvTurretHudGsBytecode","srv turret hud pass-through","main",nullptr,{},false,edvr::kSrvTurretHudGs,"gs_5_0"},
}; }
