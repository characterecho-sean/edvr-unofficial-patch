#pragma once
// The GPU pass profiler's front-loaded delay (TEMPORARY, gpu_pass_profile.cpp): one thread running a chain of dependent multiply-adds,
// `iterations` long, into a one-element UAV. Compiled at the first capture that asks for a delay (d3dcompiler_47.dll); the iteration
// count is calibrated against a timestamp-measured trial (calibrateDelay). The loop is dependent so the compiler cannot fold it, and
// the result is written so it cannot be removed.
namespace edvr {
constexpr char kGpuPassDelayHlsl[] =
    "cbuffer DelayCB : register(b0) { uint iterations; uint3 unused; }\n"
    "RWBuffer<float> sink : register(u0);\n"
    "[numthreads(1, 1, 1)] void main() {\n"
    "    float x = 1.0001f;\n"
    "    [loop] for (uint i = 0; i < iterations; ++i) { x = x * 1.0000001f + 0.0000001f; }\n"
    "    sink[0] = x;\n"
    "}\n";
}  // namespace edvr
