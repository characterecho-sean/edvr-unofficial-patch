#pragma once
// Build-only registry: visual.desktop_window's draw (src/d3d11/desktop_window_shaders.h). New, so held to its own contract (symbol, source name, entry, profile,
// the text it compiles) rather than a former runtime call's; tools/desktop_window_test reads the wiring and temporal_shader_build reflects the bytes.
#include "../../src/d3d11/desktop_window_shaders.h"
static std::vector<Variant> desktopWindowVariants() { return {
    {"kDesktopWindowVsBytecode","desktop_window_vs","main",nullptr,{},false,edvr::kDesktopWindowVsHlsl,"vs_5_0"},
    {"kDesktopWindowPsBytecode","desktop_window_ps","main",nullptr,{},false,edvr::kDesktopWindowPsHlsl,"ps_5_0"},
}; }
