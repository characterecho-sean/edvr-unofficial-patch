#pragma once
// Build-only registry: the scope lens's three compute passes (src/d3d11/scope_zoom_shaders.h; the third is B3's, the magnify pass that samples the left eye's zoomed picture). New, so held to their own contract (symbol, source name, entry, profile, the text each
// compiles) in temporal_shader_build's self-test; tools/scope_zoom_test runs the compiled bytes on a synthetic frame.
#include "../../src/d3d11/scope_zoom_shaders.h"
static std::vector<Variant> scopeZoomVariants() { return {
    {"kScopeZoomRaysBytecode","scope_zoom_rays_cs","main",nullptr,{},false,edvr::kScopeZoomRaysCsHlsl,"cs_5_0"},
    {"kScopeZoomMagnifyBytecode","scope_zoom_magnify_cs","main",nullptr,{},false,edvr::kScopeZoomMagnifyCsHlsl,"cs_5_0"},
    {"kScopeZoomMagnifyB3Bytecode","scope_zoom_magnify_b3_cs","main",nullptr,{},false,edvr::kScopeZoomMagnifyB3CsHlsl,"cs_5_0"},
}; }
