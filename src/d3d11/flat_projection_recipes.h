#pragma once
#include "flat_projection_runtime.h"
#include "engine_velocity_families.h"

namespace edvr {
// Exact-bytecode recipes, not authorization to jitter a frame. The runtime
// must additionally prove target/camera ownership and complete frame coverage.
// PS companions can contain further consumers: unknown draws remain explicit.
struct FlatProjectionRecipes {
    FlatProjectionRuntimeRequest requests[3]{};
    uint32_t count = 0;
    void add(FlatProjectionStage stage, UINT slot, FlatProjectionPatchLayout layout, uint32_t row) {
        auto& request = requests[count++]; request.stage = stage; request.slot = slot;
        request.patchCount = 1; request.patches[0] = {layout, row * 16, {}};
    }
};
inline FlatProjectionRecipes flatProjectionDrawRecipes(uint64_t vs, uint64_t ps) {
    FlatProjectionRecipes result;
    using S = FlatProjectionStage; using L = FlatProjectionPatchLayout;
    if (engine_velocity_family::supportedPair(vs,ps) || vs == 0x6041FD2D3D0164E1ull || vs == 0xBBAD1CA808E1E292ull)
        result.add(S::Vertex,1,L::ForwardColumns,270);
    else switch (vs) {
    case 0xCFCA8FFC6B058630ull: case 0x0EE43D81E394E70Cull: case 0x2CECEC3065EF0D4Aull:
    case 0x1F3AD1584D7FA3C8ull: case 0x4D516EF05C68FFA5ull: case 0x8BD7C37ABCEE7E45ull:
    case 0x94D5C556DFD6D705ull:
        result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x7E38A6AA1269C901ull:
        result.add(S::Vertex,2,L::ForwardDp4,10);
        result.requests[0].patchCount = 2;
        result.requests[0].patches[1] = {L::InverseUvRay,14*16,{}}; break;
    case 0xF8FA801F2CB1E27Cull: result.add(S::Vertex,2,L::InverseClip,11); break;
    case 0x5453D19B6D362364ull:
        if (ps == 0xF321711CF47EB970ull) result.add(S::Vertex,2,L::ForwardColumns,6); break;
    case 0xA2C2D5510BF1926Dull:
        if (ps == 0x3AD8AABF289A1D8Eull) result.add(S::Vertex,2,L::ForwardColumns,7); break;
    case 0x9B34C331902DC1EDull:
        if (ps == 0x3B3433E4FEBBC37Bull) result.add(S::Vertex,2,L::ForwardColumns,8); break;
    default: break;
    }
    if (ps == 0x7EAC71963E66C5FEull) result.add(S::Pixel,2,L::InverseScreenRay,1);
    return result;
}
inline FlatProjectionRecipes flatProjectionDispatchRecipes(uint64_t cs, uint32_t width, uint32_t height) {
    FlatProjectionRecipes result;
    // Only these two variants have current Epic depth/HDR association evidence.
    if (cs != 0x5998146D464F5C0Eull && cs != 0xEB0245DE0BB23BB6ull) return result;
    result.add(FlatProjectionStage::Compute,0,FlatProjectionPatchLayout::LightingUvRay,10);
    result.requests[0].patches[0].lighting = {.25f,-.25f,width,height,120,(width+119)/120,(height+119)/120,1};
    return result;
}
} // namespace edvr
