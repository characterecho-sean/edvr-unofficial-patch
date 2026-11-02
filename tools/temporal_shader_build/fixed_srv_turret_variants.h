#pragma once
// Build-only registry: the SRV turret HUD's nine pass-through geometry shaders (src/d3d11/srv_turret_hud_shader.h; issue 140, experimental.srv_turret_panels_back, experimental.srv_turret_radar_back and experimental.srv_turret_hud_gap), one for each distinct
// vertex-shader output signature (src/d3d11/srv_turret_math.h kSignatures). New, so held to their own contract (symbol, source name, entry, profile, the text each compiles, and the stage, signature and constant buffer its
// DXBC reflects) in temporal_shader_build's self-test; tools/srv_turret_hud_test runs the compiled bytes on WARP.
#include "../../src/d3d11/srv_turret_hud_shader.h"
static std::vector<Variant> srvTurretHudVariants() { return {
    {"kSrvTurretHudGsABytecode","srv turret hud pass-through A","main",nullptr,{},false,edvr::kSrvTurretHudGsA,"gs_5_0"},
    {"kSrvTurretHudGsBBytecode","srv turret hud pass-through B","main",nullptr,{},false,edvr::kSrvTurretHudGsB,"gs_5_0"},
    {"kSrvTurretHudGsCBytecode","srv turret hud pass-through C","main",nullptr,{},false,edvr::kSrvTurretHudGsC,"gs_5_0"},
    {"kSrvTurretHudGsDBytecode","srv turret hud pass-through D","main",nullptr,{},false,edvr::kSrvTurretHudGsD,"gs_5_0"},
    {"kSrvTurretHudGsEBytecode","srv turret hud pass-through E","main",nullptr,{},false,edvr::kSrvTurretHudGsE,"gs_5_0"},
    {"kSrvTurretHudGsFBytecode","srv turret hud pass-through F","main",nullptr,{},false,edvr::kSrvTurretHudGsF,"gs_5_0"},
    {"kSrvTurretHudGsGBytecode","srv turret hud pass-through G","main",nullptr,{},false,edvr::kSrvTurretHudGsG,"gs_5_0"},
    {"kSrvTurretHudGsHBytecode","srv turret hud pass-through H","main",nullptr,{},false,edvr::kSrvTurretHudGsH,"gs_5_0"},
    {"kSrvTurretHudGsIBytecode","srv turret hud pass-through I","main",nullptr,{},false,edvr::kSrvTurretHudGsI,"gs_5_0"},
}; }
