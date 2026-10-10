// The map temporal-off frames' upscale (2026-10-10, the maintainer's decision: EASU for the maps below native). This is the text AFTER
// AMD's vendored FSR: tools/temporal_shader_build prepends kGpuPrologue, ffx_a.h and ffx_fsr1.h (fsr_hlsl_gen.h, generated from
// src/d3d11/fsr at build time, MIT, AMD's notices kept in those files) and "#define FSR_EASU_F 1", then compiles the two entries
// below to bytecode. The runtime carries no HLSL compiler (shader_swap.h), so nothing here is compiled on the render thread.
//
// Pass 1 (mapCompress) runs on the copy route's colour (render size): c / (1 + max c), the compression hdrCompress applies in
// flat_mono_shader_source.h, so EASU's edge test sees a bounded signal and not HDR radiance. Pass 2 (mapEasu) runs EASU over that
// texture at the evaluation size, then inverts the compression as hdrExpand does, kept finite and inside the half-float range.
// No sharpening pass follows (no RCAS): the map's text and markers are the UI layer's, and EASU is the only filter here.
//
// Constants: mapCon0..mapCon3 are AMD's FsrEasuCon output for render size -> evaluation size (map_easu.cpp computes them with the
// same call intro_upscale.cpp makes); mapSize.xy is the output size, mapSize.zw the input size (mapCompress's bounds).
#pragma once

namespace edvr {

inline constexpr char kMapEasuHlsl[] = R"HLSL(
Texture2D<float4> MapRaw : register(t1);      // mapCompress's input: the copy route's colour, render size
Texture2D<float4> MapBounded : register(t0);  // mapEasu's input: mapCompress's output, render size
SamplerState MapSmp : register(s0);
cbuffer MapCon : register(b0) { uint4 mapCon0; uint4 mapCon1; uint4 mapCon2; uint4 mapCon3; uint4 mapSize; };
RWTexture2D<float4> MapOut : register(u0);

AF4 FsrEasuRF(AF2 p) { return MapBounded.GatherRed(MapSmp, p); }
AF4 FsrEasuGF(AF2 p) { return MapBounded.GatherGreen(MapSmp, p); }
AF4 FsrEasuBF(AF2 p) { return MapBounded.GatherBlue(MapSmp, p); }

[numthreads(8,8,1)]
void mapCompress(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= mapSize.zw)) return;
    float3 c = max(MapRaw.Load(int3(id.xy, 0)).rgb, 0.0);
    MapOut[id.xy] = float4(c / (1.0 + max(c.r, max(c.g, c.b))), 1.0);
}

[numthreads(8,8,1)]
void mapEasu(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= mapSize.xy)) return;
    AF3 c;
    FsrEasuF(c, AU2(id.xy), mapCon0, mapCon1, mapCon2, mapCon3);
    // EASU's ringing can overshoot the bounded range, and a NaN must not reach the expand: both fall to a finite bounded value.
    c = all(isfinite(c)) ? clamp(c, 0.0, 0.9999) : float3(0.0, 0.0, 0.0);
    float m = max(c.r, max(c.g, c.b));
    MapOut[id.xy] = float4(min(c / max(1.0 - m, 1.0 / 65504.0), 65504.0), 1.0);
}
)HLSL";

}  // namespace edvr
