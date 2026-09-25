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
// Verified inert pairs from the Epic f1ea02fe outcome census. This is an
// explicit classification, not a projection patch or a jitter admission.
inline bool flatProjectionDrawUnchanged(uint64_t vs, uint64_t ps) {
    return (vs == 0xFC1193AFFC596F74ull && ps == 0x258B95AC99520C1Full) ||
           (vs == 0xE8FDC0D92EEBA6D7ull && ps == 0x258B95AC99520C1Full) ||
           (vs == 0x53211E8C072CD02Eull && ps == 0xB403F48CB35D9739ull) ||
           // Epic 6e9bde74: VS passes v0 straight to SV_Position; PS has
           // only CB2[7] UV/filter constants and t0/t1/t2 samples.
           (vs == 0xCFA91824129ECBBCull && ps == 0xFCFAD73924BF45B9ull) ||
           // Epic 85bf7652: VS maps packed UV directly to clip space without
           // a CB; PS filters texture neighbors with no projection consumer.
           (vs == 0x525D47E3D5E2EFF4ull && ps == 0xF0BAE053476F8730ull);
}
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
    // Additional exact companions observed in the Epic f1ea02fe flight.
    // The VS bytecode confirms both the starting row and multiplication
    // convention; no VS-only fallback admits an unobserved material.
    if (result.count == 0) switch (vs) {
    // b1[270..273]: scalar-weighted rows accumulated into clip position.
    case 0xBFE51414CC3024B4ull: if (ps == 0xDB79AE788E049DFDull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xEB5234DB6ADB491Dull: if (ps == 0xB7D50283329322C3ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x7B0DC42D383F694Cull: if (ps == 0x0DF03E64DF9DBEF1ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xC53F124D7D591509ull: if (ps == 0x41DDAD26FE2034A7ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x87FCE198053AA4B9ull: if (ps == 0x50A516DA6DFE2A7Cull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xDE545DC8EE4FBB87ull: if (ps == 0x91F8937EDA723663ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x5DA53D8B0133341Eull:
        if (ps == 0xE23C45251B7ECDFEull || ps == 0xBF0CE0DA543D491Full) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x4A0748B67A27F71Eull:
        if (ps == 0x1E1E004BD5442A7Aull || ps == 0x4411D5EF62CDC66Aull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x39CC20727A27FD17ull: if (ps == 0xBFD75730622BB17Cull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x68DDDEF04D9894AFull: if (ps == 0x06332CA168B6DA63ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xF7A6E916F14A3B1Aull: if (ps == 0x06332CA168B6DA63ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xD95905C18B7FAD93ull: if (ps == 0x5BCB6B95BE7C0700ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xD1281DF454A153ADull: if (ps == 0x97DBC87FCAA429C4ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x025B4B9FF54622EDull: if (ps == 0xC5A5C7E8216CB9AFull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x6DB587D29F43A9A6ull: if (ps == 0xB2DE0A41A4C2B4F5ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x0B5981F2AEF7D80Aull: if (ps == 0xC5A5C7E8216CB9AFull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x8C091FFD08644E02ull: if (ps == 0x4E4FF61E8A08FC7Eull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x5559BD94B6852E83ull:
        if (ps == 0xEA02FAC2BD6C643Cull || ps == 0xE95634B0F61D218Full) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x9F4BBCFCD3B68BC9ull: if (ps == 0x2BAE3742FEB916D9ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    // b0[4..7] and b2[10..13]: four dot products form clip xyzw.
    case 0x5E417E9DF2E7F9E6ull: if (ps == 0xBD801F2FB02522EBull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x88DCF1164C640EC3ull: if (ps == 0x494506A63091DF8Cull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0xE904D334BC8B11EAull: if (ps == 0x095030F27D2C362Aull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0xB7790CBFC6554097ull: if (ps == 0x8DEF46452FA459F5ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x81216C77F90DEDD6ull: if (ps == 0xA2965EC2931A39C8ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x0357BBB2DEE43C1Full: if (ps == 0x81812EF97FB4A361ull) result.add(S::Vertex,2,L::ForwardDp4,10); break;
    case 0x8289669D93A18C1Dull: if (ps == 0xC6E6E419DA9F6FADull) result.add(S::Vertex,2,L::ForwardDp4,10); break;
    case 0x963B52C73B4143ACull: if (ps == 0x50364C9D994141D5ull) result.add(S::Vertex,2,L::ForwardDp4,10); break;
    // b2[6..9] and [7..10]: clip is a scalar-weighted row sum.
    case 0xDF3503CD07F9B10Cull: if (ps == 0x8C08EB252B0F6095ull) result.add(S::Vertex,2,L::ForwardColumns,6); break;
    case 0xB932058F26B76691ull: if (ps == 0x65861AC394D51526ull) result.add(S::Vertex,2,L::ForwardColumns,6); break;
    case 0x9611A454527F7FEBull: if (ps == 0x1E1C49DC51C0E509ull) result.add(S::Vertex,2,L::ForwardColumns,7); break;
    default: break;
    }
    // Epic ce715126, 2026-09-24: exact companions in the scene draw audit.
    // These offsets follow the instructions that write SV_Position, not other
    // view-space varyings. A PS screen/depth lookup alone is not an inverse
    // projection: none of these PS blobs consumes a patchable inverse matrix.
    if (result.count == 0) switch (vs) {
    // CB1 rows 270..273 are scalar-weighted clip columns, including the
    // depth-only draw with no PS. Local/object transforms before these rows
    // do not require equality with the scene camera.
    case 0x84F6596FAF22CCFAull: if (ps == 0) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xDE545DC8EE4FBB87ull: if (ps == 0x03B17F89B31C4788ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x361CD4B7FF213A01ull: if (ps == 0xFA7411BF7E4C4088ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x124D7F3F649138D4ull: if (ps == 0x8085AE8DD1906CDCull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x3064D7F445192FDDull: if (ps == 0xCCDB2A91490F8755ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x9AEC596A2B036EA6ull: if (ps == 0x3789CA2062E196FBull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xEB787F983BC1F5A3ull: if (ps == 0x8591A46B10497299ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x8106B439CD518CFCull: if (ps == 0x3BC6B5B66B852BB5ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xAFBCD3ADB9092F78ull: if (ps == 0x2BAE3742FEB916D9ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    // Earlier 0150638a pair: Epic F10 now includes the exact CAB... PS.
    // Its CB1[277..279] terms rotate directions; SV_Position remains the
    // VS CB1[270..273] column sum, with no PS inverse projection.
    case 0x98397963AAEC45D3ull: if (ps == 0xCAB49794BB439D03ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    // This VS selects between CB1[41..44] and CB1[45..48] before o5
    // (SV_Position); jitter both possible clip matrices atomically.
    case 0x1F17BF54DB6EE407ull:
        if (ps == 0xA75C1DB6562B8CA7ull) {
            result.add(S::Vertex,1,L::ForwardColumns,41);
            result.requests[0].patchCount = 2;
            result.requests[0].patches[1] = {L::ForwardColumns,45*16,{}};
        } break;
    // Four DP4 clip rows; partial-z variants still use rows 4,5,7 for
    // xyw, so the homogeneous xy += jitter*W identity is unchanged.
    case 0xCFC9094F7EEE21E2ull:
        if (ps == 0xFD77C2EBFFAC7D9Cull || ps == 0xAE3D10F3D40D688Cull ||
            ps == 0x4DBE9258D3C4D3DAull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x41E245D488BFE83Eull: if (ps == 0x6EF82262EB12A037ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0xB12F7A618E1BDE98ull: if (ps == 0x42AC0CACC9CDF72Bull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x203DF51758AADC4Dull: if (ps == 0xEEAAC839A9F09448ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x5EAFFCD01B97D0C4ull: if (ps == 0xDD371C57C9093BB8ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x46546443FD3C3F88ull: if (ps == 0xAD050E528C0E8B17ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x95D01BA609BF7500ull: if (ps == 0x067CBE05E7EF7F32ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0xE508648660A352B2ull: if (ps == 0x63ABD86359B57D01ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x381D80284FE236F8ull: if (ps == 0x72BBDA3D4CD3E39Bull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x71DD8B8B09060A81ull: if (ps == 0x2D037A047171BF3Bull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x939D01F28D1FEEA8ull: if (ps == 0xC5DF9CC943476289ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x5C1D8EF529324A22ull: if (ps == 0xC49F999F7D3C801Dull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x820E5C131B99361Dull: if (ps == 0x6EAA86EFE135B2D4ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    // Billboard VS reads CB0 only at instructions 5-7 for anchor xyw.
    // Its subsequent divide and depth occlusion samples must see the same
    // jittered camera anchor; the final screen quad is intentionally nonlinear.
    case 0xB75A6FF2CA9FA5D6ull: if (ps == 0xD56F859BE4781431ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    // The direct depthless effect emits o2 = sum(coord * CB2[8..10])
    // + CB2[11], the same column convention at a different offset.
    case 0x24214E7C45496BE0ull: if (ps == 0x0C8FCDB6A3BECCE6ull) result.add(S::Vertex,2,L::ForwardColumns,8); break;
    case 0xA1B7CFCD0BE7493Eull: if (ps == 0x2DB678B6B558B604ull) result.add(S::Vertex,2,L::ForwardDp4,10); break;
    case 0xCE24A73943632F55ull: if (ps == 0x1F64463B15189104ull) result.add(S::Vertex,2,L::ForwardDp4,10); break;
    default: break;
    }
    // Epic 6e9bde74 F10 capture. The first 18 VS blobs below form SV_Position from a
    // scalar-weighted sum of CB1[270..273]. Their exact PS companions do not
    // read those projection rows or a patchable inverse projection. Several
    // read CB1[277..279] for view direction; those rows are not clip matrices.
    if (result.count == 0) switch (vs) {
    case 0xF516BF0201303B87ull: if (ps == 0xB40B0462256E31C2ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x318693134643131Aull: if (ps == 0x4EBA794D13603735ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xD99AFDC250D19A3Full: if (ps == 0xE86271E464CCDC1Dull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x114AF608F86D9ED8ull: if (ps == 0xA17504A2627767F2ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x01DA82F8D0EA0C99ull: if (ps == 0x2C3ADCD2FCD47298ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x6D2158CFE759A3BEull: if (ps == 0xBD59C4EF0DA3B1E2ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xA4A19FAF8D08E1D6ull: if (ps == 0xBE3EA29C554ABFF3ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x33A5025C48FC8259ull: if (ps == 0x6B8C26FAC5558C6Cull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xA8E4D93B8B294505ull: if (ps == 0x75D12D8561BA8525ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xA6A39338C06E03A1ull: if (ps == 0xED91F94EC94FA5C2ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x617C6E44A034E0C2ull: if (ps == 0x627FEC646836683Full) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xB43A856E285815E3ull: if (ps == 0x702C3974A260DE14ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xF512712C40D93C12ull: if (ps == 0xD0B9213C1F248335ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x359BF8FF5CFAA4C3ull: if (ps == 0x92FF8499ED345759ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xEFE42AC6142C1815ull: if (ps == 0x29D8624FD690277Full) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x01A029C7DBD48554ull: if (ps == 0x130FC0A72CE36CDBull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0xA1CE8A95F0D23260ull: if (ps == 0xC6CD9AEBDEA803EEull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x2684F02B9B0BB0DEull: if (ps == 0x2376A8D9AA874372ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    // Screen quad o3 is copied from input XY. The PS samples scene depth at
    // that coordinate and reconstructs a ray from VS o2. CB1[144,145] are
    // the screen basis and [147].xyz is the adjustable ray origin; row 146
    // and all W components remain intact under InverseScreenRay.
    case 0x4AEC439CEC7FFDCEull: if (ps == 0x87EF79B19297B8C4ull) result.add(S::Vertex,1,L::InverseScreenRay,144); break;
    default: break;
    }
    // Epic 5c78c34d menu capture: complete VS/PS creation blobs. Each PS
    // companion was checked for an additional projection consumer. The
    // clustered material reads CB1[277..279] only for orientation, the
    // deferred lighting PS reconstructs from interpolated rays and depth,
    // the depth/alpha PS samples UV, and the reflective material reads
    // CB1[287..289] only for cube-map direction.
    if (result.count == 0) switch (vs) {
    case 0x61AE8EB05FDC18DDull: if (ps == 0x4504BC268E109C31ull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x0357BBB2DEE43C1Full: if (ps == 0x222188632125D14Bull) result.add(S::Vertex,2,L::ForwardDp4,10); break;
    case 0x4EF6DDB075A927FAull: if (ps == 0x098C0764D28FC42Cull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    case 0x95D01BA609BF7500ull: if (ps == 0xF10792B40AE3ED42ull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    default: break;
    }
    // Epic 85bf7652 automatic capture: the decal VS writes the same
    // CB1[270..273] column sum to SV_Position and the clip-XYW varying
    // used by its PS to sample depth. The PS reconstructs from interpolated
    // world position and view-axis CB1[279], not an inverse projection.
    // The skinned material VS instead writes four CB0[4..7] dot products;
    // its PS consumes only material UVs and orientation CB1[277..279].
    if (result.count == 0) switch (vs) {
    case 0x0C4E76889907B963ull: if (ps == 0xA90825082F36756Eull) result.add(S::Vertex,1,L::ForwardColumns,270); break;
    case 0x989E043933A369ABull: if (ps == 0xCE844D87026C684Cull) result.add(S::Vertex,0,L::ForwardDp4,4); break;
    default: break;
    }
    // Epic 85d590e0: projected effect. VS CB0[4..7] feeds both
    // SV_Position and clip XYW used by the PS depth-occlusion sample.
    // The PS compares sampled depth against that unchanged W; it has no
    // projection matrix of its own.
    if (result.count == 0 && vs == 0x2D8263CC54D55398ull && ps == 0x89B662E266E5D73Eull)
        result.add(S::Vertex,0,L::ForwardDp4,4);
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
