// Canonical source for dxbc_static_surface.h's embedded instruction tail.
// The injector relocates r0..r2, identity input 0 and position input 1, then
// inserts the executable instructions that produce SV_Target7 before RET.
cbuffer StaticSurfaceGeometry : register(b13) { uint4 GeometryKey; }

struct ModelRecord {
    uint4 row0;
    uint4 row1;
    uint4 unused[19];
};
StructuredBuffer<ModelRecord> ModelPool : register(t127);

struct Input {
    nointerpolation uint3 identity : __USER_MATERIALMODULATION_DATAID;
    float4 position : SV_Position;
};
struct Output {
    float4 retained : SV_Target0;
    uint4 owner : SV_Target7;
};

Output main(Input input) {
    Output output;
    output.retained = 0;
    const uint poolIndex = input.identity.y & 0x7fffffffu;
    uint count, stride;
    ModelPool.GetDimensions(count, stride);
    uint3 key = 0;
    if (poolIndex < count) {
        const uint4 row0 = ModelPool[poolIndex].row0;
        const uint3 row1 = ModelPool[poolIndex].row1.xyz;
        key = (row0.y ^ GeometryKey.xyz) ^ uint3(0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u);
        key *= uint3(0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu);
        key = (key ^ row0.z) * uint3(0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu);
        key = (key ^ row0.w) * uint3(0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu);
        key = (key ^ row1.x) * uint3(0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu);
        key = (key ^ row1.y) * uint3(0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu);
        key = (key ^ row1.z) * uint3(0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu);
        key ^= key >> uint3(16u, 13u, 15u);
        if (row0.x != 0) key = 0;
    }
    output.owner = uint4(key, asuint(input.position.z));
    return output;
}
