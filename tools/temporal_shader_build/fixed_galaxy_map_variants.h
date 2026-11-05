#pragma once
// Build-only registry: the VR Galaxy Map's replacement shaders (src/d3d11/galaxy_map_vs.h; docs/galaxy-map-vr-2026-11-03.md Fix 3). New, so held to their own contract
// (symbol, source name, entry, profile, the text they compile, the stage and the resources their DXBC reflects); tools/galaxy_map_vs_test holds the five transcriptions'
// behaviour against the formulae in that header, and by hand against the game's own bytecode.
#include "../../src/d3d11/galaxy_map_vs.h"
static std::vector<Variant> galaxyMapVariants() { return {
    {"kMapIconFacingVsBytecode","map_icon_facing_vs","main",nullptr,{},false,edvr::kMapIconFacingVS,"vs_5_0"},
    {"kMapIconUvFacingVsBytecode","map_icon_uv_facing_vs","main",nullptr,{},false,edvr::kMapIconUvFacingVS,"vs_5_0"},
    {"kMapArrowDepthVsBytecode","map_arrow_depth_vs","main",nullptr,{},false,edvr::kMapArrowDepthVS,"vs_5_0"},
    {"kMapProbeVsBytecode","map_probe_vs","main",nullptr,{},false,edvr::kMapProbeVS,"vs_5_0"},
    {"kMapProbePsBytecode","map_probe_ps","main",nullptr,{},false,edvr::kMapProbePS,"ps_5_0"},
    {"kMapRingVsBytecode","map_ring_vs","main",nullptr,{},false,edvr::kMapRingVS,"vs_5_0"},
    {"kMapDepthSpritePsBytecode","map_depth_sprite_ps","main",nullptr,{},false,edvr::kMapDepthSpritePS,"ps_5_0"},
    {"kMapDepthCanvasPsBytecode","map_depth_canvas_ps","main",nullptr,{},false,edvr::kMapDepthCanvasPS,"ps_5_0"},
    {"kMapStemVsBytecode","map_stem_vs","main",nullptr,{},false,edvr::kMapStemVS,"vs_5_0"},
    {"kMapMarkerVsBytecode","map_marker_vs","main",nullptr,{},false,edvr::kMapMarkerVS,"vs_5_0"},
    {"kMapPunchVsBytecode","map_punch_vs","main",nullptr,{},false,edvr::kMapPunchVS,"vs_5_0"},
    {"kMapPunchPsBytecode","map_punch_ps","main",nullptr,{},false,edvr::kMapPunchPS,"ps_5_0"},
    {"kMapReticuleVsBytecode","map_reticule_vs","main",nullptr,{},false,edvr::kMapReticuleVS,"vs_5_0"},
    {"kMapProbeStickyPsBytecode","map_probe_sticky_ps","main",nullptr,{},false,edvr::kMapProbeStickyPS,"ps_5_0"},
}; }
