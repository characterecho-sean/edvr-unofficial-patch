#pragma once
namespace edvr {
// Null-PS depth passes retain every original color target: these shaders
// export only EDVR's private MRT6. They do not discard or export depth,
// coverage, or stencil, so the original fixed-function tests still decide
// precisely which fragments update ownership.
constexpr char kFlatNullWorldMarkerPs[]=R"HLSL(
float2 main(float4 position:SV_Position):SV_Target6 {
    return float2(0,position.z);
}
)HLSL";

// The flat-only VS export copies actual v0.x, the original t33 pool index.
// A foreign encoded owner must remain an exact odd integer in binary32.
constexpr char kFlatNullForeignMarkerPs[]=R"HLSL(
struct Input {
    float4 position:SV_Position;
    nointerpolation uint slot:EDVRPOOLSLOT;
};
float2 main(Input input):SV_Target6 {
    float owner=input.slot<=0x7ffffe?-(float)(2*input.slot+3):-2;
    return float2(owner,input.position.z);
}
)HLSL";
// No original PS exists on this path, so b13 is available for the bounded
// frame-local draw token. Native primitive identity distinguishes coincident
// triangles within that actual draw.
constexpr char kFlatNullForeignProvenanceMarkerPs[]=R"HLSL(
cbuffer Writer : register(b13) { float4 writer; };
struct Input {
    float4 position:SV_Position;
    nointerpolation uint slot:EDVRPOOLSLOT;
    uint primitive:SV_PrimitiveID;
};
float4 main(Input input):SV_Target6 {
    float owner=input.slot<=0x7ffffe?-(float)(2*input.slot+3):-2;
    return float4(owner,input.position.z,float(input.primitive),writer.x);
}
)HLSL";
constexpr char kFlatNullPoolMarkerPs[]=R"HLSL(
struct Input {
    float4 position:SV_Position;
    nointerpolation uint slot:EDVRPOOLSLOT;
};
float2 main(Input input):SV_Target6 {
    float owner=input.slot<=0x7ffffe?(float)(2*input.slot+1):-2;
    return float2(owner,input.position.z);
}
)HLSL";
} // namespace edvr
