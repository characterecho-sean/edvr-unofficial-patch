// The map temporal-off frames' EASU constants (2026-10-10). The shader is map_easu_shader.h, precompiled; these are the numbers its
// cbuffer reads, computed by AMD's FsrEasuCon from the vendored ffx_fsr1.h (map_easu.cpp).
#pragma once

#include <cstdint>

namespace edvr {

// out[0..3] = con0, out[4..7] = con1, out[8..11] = con2, out[12..15] = con3 (FsrEasuCon, input viewport and input size = the render
// size, output size = the evaluation size); out[16..17] = the output size, out[18..19] = the input size (mapCompress's bounds).
void mapEasuConstants(uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, uint32_t out[20]);

}  // namespace edvr
