#pragma once

// Draw-time evidence for an explicitly armed eye run. Celestial draws use
// cb0[4..7] for clip position, outside the instance pool; dim holo strokes
// need the original surface alpha to distinguish them from the background.
// Keep each draw (including both eyes and repeated VS hashes). Buffer
// addresses identify bindings within a run, never objects across frames.
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <utility>
#include <map>
#include <mutex>
#include "vr_world_route.h" // VrWorldInternalScope keeps helper dispatches out of the game's census/hooks
#include <cstring>
#include "holo_material.h"

namespace edvr {
inline uint32_t eyeMeshVb0BindingStatus(bool present,uint32_t stride,uint32_t offset) noexcept {
    if(!present)return 0; // absent binding
    if(stride!=8)return 4; // unsupported instance-id stride
    if(offset!=0)return 3; // unsupported binding offset
    return 1; // supported binding; copy/eye/quota checks still follow
}
struct EyeMeshJoinStamp {
    uint32_t sceneFrame=0,epoch=0,scheduledIndex=UINT32_MAX,scheduledRenderFrame=0;
    uint32_t fpFrame=0,fpSequence=0,gGameFrame=0,itemSerial=0,rigGeneration=0;
    uint16_t armJoints[2][3]={};
    uint64_t rig=0,poseQpc=0,drawQpc=0,captureSequence=0;
    uint32_t validFlags=0; // scene, epoch, scheduled index, pose row found, pose valid, draw QPC, capture sequence
    uint32_t sourceJobLookupStatus=1; // 1 = resolve offline from exact t33 record and skin-ledger dispatches
};
class EyeDrawSnapshot {
    using Buffer = Microsoft::WRL::ComPtr<ID3D11Buffer>;
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
public:
    static constexpr uint32_t kMaxDraws = 4096;
    static constexpr uint32_t kMaxTextures = 24;
    static constexpr uint32_t kMaxTextureBytes = 16 * 1024 * 1024;
    // The targeting sprite's source surface is 2613x2286 RGBA8 (22.79 MiB).
    // Keep this exception tied to the exact original colour pair; other
    // surfaces retain the normal 16 MiB diagnostic cap.
    static constexpr uint32_t kSpriteTextureBytes = 32 * 1024 * 1024;
    static constexpr uint32_t kTotalTextureBytes = 64 * 1024 * 1024;
    static constexpr uint32_t kVscreenTextureBytes = 128 * 1024 * 1024;
    static constexpr uint32_t kVscreenTotalBytes = 256 * 1024 * 1024;
    // The source sprite diagnostic is deliberately separate from ordinary
    // effect images.  A pair is reserved before the draw, so a failed second
    // allocation can never leave a misleading before-only record behind.
    static constexpr uint32_t kSpriteImageBytes = 160 * 1024 * 1024;
    static constexpr uint32_t kSpriteDepthBytes = 160 * 1024 * 1024;
    static constexpr uint32_t kSpriteImageCount = 20;
    static constexpr uint32_t kSpriteCrop = 1400;
    static constexpr uint64_t kHolo = 0x81216C77F90DEDD6ull;
    // The same panels with Elite's Disable GUI effects on (holo_material.h): the pair a run on such a
    // rig would otherwise miss, vertex shader bytes and surface included.
    static constexpr uint64_t kHoloGuiFxOff = kHoloGuiFxOffVs;
    static constexpr uint64_t kHud = 0xB7790CBFC6554097ull, kSprite=0xE508648660A352B2ull;
    static constexpr uint64_t kSpritePs=0x63ABD86359B57D01ull;
    static constexpr uint64_t kPanel=0xA888D51024D9798Eull,kScreen=0x4EF6DDB075A927FAull;
    static constexpr uint64_t kVscreen=0x5C36AF051B98B9F1ull,kVscreenPs=0xCFE84157BC76E921ull;
    static constexpr uint64_t kScene=0x4435F2E50020E7F3ull;
    static constexpr uint64_t kNight=0xFCF7BD2896751D96ull,kNightPs=0xF786D34B5E118D5Eull;
    // Exact rifle bolt pair from the armed eye dump. Retain its existing
    // draw-time streams only; this is visual shader geometry, not collision.
    static constexpr uint64_t kBoltVs=0x1F17BF54DB6EE407ull,kBoltPs=0xA75C1DB6562B8CA7ull;
    // F77 bounded candidate capture: these are observed shader families,
    // not proven weapon identity. Keep the optic pair exact.
    static constexpr uint64_t kF77HeldCandidateVs=0x154FB5A453F3D2E9ull;
    static constexpr uint64_t kF77OpticVs=0x025B4B9FF54622EDull,kF77OpticPs=0x46F92DC71BF8DFA5ull;
    static constexpr uint64_t kWeaponUiVs=0xB10B032BDFD46700ull,kWeaponUiPs=0xDB899F4BD577F2E5ull;
    static bool f77Candidate(uint64_t vs,uint64_t ps) noexcept {
        return vs==kF77HeldCandidateVs || (vs==kF77OpticVs && ps==kF77OpticPs) || (vs==kWeaponUiVs && ps==kWeaponUiPs);
    }
    static bool weaponUiCandidate(uint64_t vs,uint64_t ps) noexcept {
        return (vs==kF77OpticVs&&ps==kF77OpticPs)||(vs==kWeaponUiVs&&ps==kWeaponUiPs);
    }
    static bool fireGeometry(uint64_t vs,uint64_t ps) noexcept { return vs==kBoltVs && ps==kBoltPs; }
    // Census unknown-A/B settlement prop batches, 2026-09-21. PS hashes are
    // listed explicitly: PS bytecode is only retained when a creation-time
    // gate names the hash (device_hook hookedCreatePS), unlike VS which
    // always reaches rememberShader.
    static constexpr uint64_t kUnknownA=0x8056C9D5F22007F9ull,kUnknownAPs=0x669CC896CA4AA988ull;
    static constexpr uint64_t kUnknownB=0x2684F02B9B0BB0DEull,kUnknownBPs=0x2376A8D9AA874372ull;
    // 05:10:17 supercruise Earth-like world: opaque surface and cloud layer.
    // Their affine VS placement is outside the rigid t33 pool. Capture the
    // actual draw's constants/vertices, never substitute atmosphere matrices.
    static bool planetSurface(uint64_t vs) {
        return vs==0x71DD9863DCFC0986ull || vs==0x3530A6FD15EDE145ull;
    }
    // 13:07:14 / 13:10:25: solar surface, corona and arc draws. These are
    // not the planet shader or the rigid instance pool. Keep their actual
    // placement and both shaders before extending either motion path.
    static bool solarDraw(uint64_t vs) {
        switch(vs) {
        case 0x0EE43D81E394E70Cull:case 0x4D516EF05C68FFA5ull:
        case 0xD95905C18B7FAD93ull:case 0x8BD7C37ABCEE7E45ull:
        case 0xD1281DF454A153ADull:case 0x5E417E9DF2E7F9E6ull:
        case 0x1F3AD1584D7FA3C8ull:return true;
        default:return false;
        }
    }
    static bool solarPixel(uint64_t ps) {
        switch(ps) {
        case 0x147E748F4CD3AE9Aull:case 0x5BCB6B95BE7C0700ull:
        case 0x94676B1FD0DF150Full:case 0x97DBC87FCAA429C4ull:
        case 0xBD801F2FB02522EBull:case 0xBA65C50BBA1ECCBBull:return true;
        default:return false;
        }
    }
    // 20:02:56 crouching rifle: these source passes use independent
    // billboard/flare vertices, not the weapon's t33 instance records.
    // Capture their placement; do not infer attachment from proximity.
    static bool sourceEffect(uint64_t vs) {
        return vs==0x9AEC596A2B036EA6ull || vs==0x3D05E7CF11AC9BEEull ||
               vs==0x0357BBB2DEE43C1Full || vs==0x963B52C73B4143ACull ||
               vs==0xE904D334BC8B11EAull || vs==0x359BF8FF5CFAA4C3ull;
    }
    static bool effectImage(uint64_t vs) {
        // 04:54:29: point/spot lighting, streaks, beams and local particles
        // are separate from the corrected weapon mesh. Preserve the pixels
        // each pass adds, without changing or suppressing any of those draws.
        return sourceEffect(vs) && vs!=0x3D05E7CF11AC9BEEull;
    }
    struct Layout {char semantic[64]{};uint32_t index=0,format=0,slot=0,offset=0,classification=0,step=0;};
    static const GUID& effectLayoutKey() {
        static const GUID key={0x10e33b44,0x1cf4,0x4fa2,{0x82,0x1f,0x63,0x51,0xda,0xeb,0x43,0x99}};return key;
    }
    // Any layout that reads the instance pool's per-instance record index is
    // a pool family (EB52 and its material layers, the unknown-A/B pairs,
    // 5B4D/BBE5 among them): the cull gate probe's geometry capture needs
    // their vertex layouts to decode the occluders' positions.
    static bool poolLayout(const D3D11_INPUT_ELEMENT_DESC* e,UINT n) {
        for(UINT i=0;e && i<n;++i)
            if(e[i].SemanticName && std::strcmp(e[i].SemanticName,"INSTANCEANDMODELDATAINDEX")==0)return true;
        return false;
    }
    static void rememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* e,UINT n,uint64_t vs) {
        if((!sourceEffect(vs) && !sourceMesh(vs) && !planetSurface(vs) && !solarDraw(vs) && vs!=kNight && vs!=kSprite && vs!=kBoltVs &&
            !poolLayout(e,n)) || !layout || !e || !n || n>32)return;
        std::vector<Layout> items(n);
        for(UINT i=0;i<n;++i) {
            if(!e[i].SemanticName || strlen(e[i].SemanticName)>=64)return;
            std::strcpy(items[i].semantic,e[i].SemanticName);items[i].index=e[i].SemanticIndex;
            items[i].format=e[i].Format;items[i].slot=e[i].InputSlot;items[i].offset=e[i].AlignedByteOffset;
            items[i].classification=e[i].InputSlotClass;items[i].step=e[i].InstanceDataStepRate;
        }
        layout->SetPrivateData(effectLayoutKey(),UINT(items.size()*sizeof(Layout)),items.data());
    }
    // Source mesh families in the 17:09:53 on-foot capture. These include
    // scenery: proximity/weapon identity must be proved from their records.
    static bool sourceMesh(uint64_t vs) {
        switch(vs) {
        case 0xF516BF0201303B87ull:case 0x7B0DC42D383F694Cull:
        case 0x8B589D25B2A0ADDCull:case 0xEB5234DB6ADB491Dull:
        case 0xDE545DC8EE4FBB87ull:case 0x889A5279E68F0672ull:
        case 0x9D800F45B12A6AACull:
        case kScene:case 0x154FB5A453F3D2E9ull:
        case 0x8106B439CD518CFCull:case 0x39CC20727A27FD17ull:
        case 0xA4A19FAF8D08E1D6ull:case 0x114AF608F86D9ED8ull:
        // Complete the weapon/tool material evidence, including optics and
        // additional surfaces drawn separately from the opaque mesh.
        case 0xAACFDCF2FB9AD809ull:case 0x34CCFAAB1EAD90BEull:
        // Multi-UV hull materials share the rigid pool transform but have
        // distinct output registers; retain their draw evidence as well.
        case 0x61AE8EB05FDC18DDull:case 0x66DE2CADB1F4AE6Bull:
        case 0x174E8D76363BE337ull:case 0x025B4B9FF54622EDull:
        case 0x7F9B650EC1A1E570ull:
        // The laser rifle's glow strips and the root triangle (2026-09-17).
        case 0xBB31244E30265F2Dull:case 0xCFCA8FFC6B058630ull:
        case 0x88DCF1164C640EC3ull:return true;
        // Census unknown-A/B settlement prop batches, 2026-09-21.
        case kUnknownA:case kUnknownB:return true;
        default:return false;
        }
    }
    static bool bodyMeshSelector(uint64_t vs,uint64_t ps,uint32_t stencilRef,uint32_t count) noexcept {
        const bool bodyCount=count==13260 || count==30216 || count==23784 || count==7692 || count==37080;
        return stencilRef==21 && bodyCount &&
            ((vs==0x9D800F45B12A6AACull && ps==0x194DD6A677084A74ull) ||
             (vs==0x7B0DC42D383F694Cull && ps==0x0DF03E64DF9DBEF1ull));
    }
    static int otherTrackedPaletteIndex(uint32_t count,ID3D11Buffer* const* palettes,ID3D11Buffer* bound) noexcept {
        if(count!=2 || !palettes || !bound || !palettes[0] || !palettes[1] || palettes[0]==palettes[1])return -1;
        if(palettes[0]==bound)return 1;
        if(palettes[1]==bound)return 0;
        return -1;
    }
    static bool watches(uint64_t vs) {
        if(solarDraw(vs))return true;
        switch (vs) {
        case kHolo:
        case kHoloGuiFxOff:
        case kHud:
        case kSprite:
        case kPanel:
        case kScreen:
        case kVscreen:
        case kNight: // 05:23:39 stationary night-vision terrain blur
        case 0xACE405F428C17EF6ull: // matching 2304/104448-index depth/colour draws
        case 0x72BDD292154158ADull:
        case 0x19F70CE80DA3242Bull: // sphere draw using cb0[9..11], cb1[270..273]
        case 0x26FC402B1274EE7Bull: // planetary pipeline, including the surface
        case 0x9FFA5D5E79F04873ull:
        case 0x71DD9863DCFC0986ull:
        case 0x3530A6FD15EDE145ull:
        case 0xB12F7A618E1BDE98ull:
        case 0x203DF51758AADC4Dull:
        case 0xC7FA0C0F5DD49180ull: // six 8194-vertex instances beside the bodies
        // The weapons fire (weapons_fire.h; fix.sharp_weapons, docs/dlss-blur-elements-2026-10-14.md W3/A1): an eye dump holds each pair's VS
        // b0/b1/b2, so 1F17's CB1[41..48] and 939D's and 5C1D's CB0 rows 4-7 can be compared with the jittered scene camera's matrix of the
        // SAME frame (done 2026-10-16: all three carry the jitter; the jitter-cancel table says cancel for each).
        case 0x1F17BF54DB6EE407ull: // beams and bolts
        case 0x939D01F28D1FEEA8ull: // multicannon rounds
        case 0x5C1D8EF529324A22ull: // the shield strike's additive part
        // The shield-strike refraction (shield_ripple.h, R1): VS b0/b1/b2 here; the pixel shader's b1 row 90 (the gain) and s0's filter are
        // read at run time on the first draw and logged. PS b1 is the same buffer as VS b1 when the log's first-draw line says so.
        case 0x820E5C131B99361Dull: // the shield's refracting bubble
        case 0xA47A3315FFF5E2E4ull: // the supercruise bars' line list (supercruise_lines.h); identity is evidence, proved by the aux vertex capture
        case 0x9BFC7FD232328391ull: // the supercruise space dust's 300 ribbon quads (supercruise_lines.h): its b0/b1/b2 here, its vertices in the aux capture
            return true;
        default: return false;
        }
    }
    struct Draw {
        uint64_t vs = 0, ps = 0, target = 0;
        uint32_t frame = 0, ordinal = 0, kind = 0, count = 0, instances = 0, startInstance = 0;
        uint32_t width = 0, height = 0, texture = UINT32_MAX;
        // F82: the source draw's ledger frame is the submit-side clock; the
        // runtime frame and eye are recorded independently for offline joins.
        uint32_t renderFrame=0; int32_t eye=-1;
        EyeMeshJoinStamp join{};
        uint64_t source[4] = {}; // VS b0,b1,b2; PS b2 (night vision: PS b1 replaces unused VS b1)
        uint32_t whole[4] = {}, copied[4] = {};
        Buffer stage[4];
        uint32_t start=0;int32_t base=0;
        struct Stream {uint32_t offset=0,stride=0,whole=0,copied=0,captureOffset=0;Buffer stage;};
        Stream streams[3]; // VB0 instance IDs, VB1 packed vertices, index buffer
        uint32_t mesh[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX}; // t33, t38, VB0, other palette candidate
        uint64_t meshSource[4]={};
        uint64_t meshView[4]={}; // bound SRV identities where one exists (t33/t38)
        // t33/t38 use SRV elements; VB0 uses bytes from its IA binding.
        uint32_t meshRangeStart[4]={},meshRangeCount[4]={};
        // 0 absent, 1 staged copy issued, 2 unsupported view, 3 nonzero
        // first element/binding offset, 4 unexpected stride, 5 size/budget refusal, 6 stage
        // creation failure, 7 body snapshot quota already used, 8 not selected,
        // 9 no unambiguous other-palette candidate was tracked, 10 unknown eye.
        uint32_t meshStatus[4]={};
        uint32_t meshSelected=0;
        std::vector<Layout> layout; // source effects; version 7 also night vision
    };
    struct SpriteDiagnostic {
        Texture target, before, after, depthImage;
        Buffer psB1;
        uint32_t frame=0, draw=0, beforeBytes=0, afterBytes=0, psB1Bytes=0;
        uint32_t mesh[2]={UINT32_MAX,UINT32_MAX};
        uint32_t x=0,y=0,width=0,height=0,sourceWidth=0,sourceHeight=0;
        uint32_t rtFormat=0, depthFormat=0, depthResourceFormat=0, depthBytes=0, srvFormat=0, srvMostDetailedMip=0, srvMipLevels=0;
        uint32_t srvWidth=0,srvHeight=0,srvArraySize=0,srvMipCount=0;
        uint64_t psB1Identity=0,srvIdentity=0,rtIdentity=0,depthIdentity=0;
        uint32_t psB1Whole=0, blendFactor[4]={}, sampleMask=0, stencilRef=0,stateMask=0;
        uint32_t rasterFill=0,rasterCull=0,topology=0,dsvFlags=0;
        uint32_t scissorCount=0,viewportCount=0;
        D3D11_BLEND_DESC blend{};
        D3D11_DEPTH_STENCIL_DESC depth{};
        D3D11_RASTERIZER_DESC raster{};
        D3D11_RECT scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        D3D11_SAMPLER_DESC sampler{};
        std::vector<Layout> layout;
        bool postPending=false;
    };
    struct MeshBuffer {
        Buffer source,stage;
        uint32_t frame=0,firstDraw=0,bytes=0,stride=0;
        uint64_t scope=0; // separate eye targets may reuse and rewrite a pool
    };
    std::vector<MeshBuffer> meshBuffers;
    uint32_t meshBytes=0,meshDeclined=0,meshDraws=0;
    static constexpr uint32_t kMeshBudget=256*1024*1024;
    static constexpr uint32_t kEyeMeshBudget=768*1024*1024;
    static constexpr uint32_t kEyeMeshSnapshotBudget=24*1024*1024;
    uint32_t meshBudgetLimit=kMeshBudget;
    uint32_t meshCaptureFirstFrame=0,meshCaptureDeclined=0,meshNonBodyDraws=0;
    uint32_t meshCaptureSnapshots=0;
    static constexpr uint32_t kEyeMeshFrames=16;
    static constexpr uint32_t kEyeMeshSnapshots=kEyeMeshFrames*2;
    uint32_t meshSnapshotRenderFrame[kEyeMeshSnapshots]={};
    uint32_t meshSnapshotLedgerFrame[kEyeMeshSnapshots]={};
    bool meshSnapshotNativeValid[kEyeMeshSnapshots]={};
    int32_t meshSnapshotEye[kEyeMeshSnapshots]={};
    struct Surface {
        Texture source, stage;
        uint32_t frame = 0, width = 0, height = 0, format = 0, bytes = 0;
    };
    std::vector<Draw> draws;
    std::vector<Surface> surfaces;
    uint32_t dropped = 0, failures = 0, textureBytes = 0;
    uint32_t firstFrame=0,vertexBytes=0,vertexDraws=0,vertexDeclined=0;
    uint32_t fireGeometrySeen=0,fireGeometryCaptured=0,fireGeometryMissingLayout=0,fireGeometryRefusedBytes=0;
    Texture sourceDepth;
    DXGI_FORMAT sourceDepthFormat=DXGI_FORMAT_UNKNOWN;
    uint32_t sourceFrame=0;
    struct EffectImage {
        Texture source,stage;
        // 0/1: before/after colour; v7 2..6: night-vision PS t0..t4.
        uint32_t draw=0,after=0,x=0,y=0,width=0,height=0,sourceWidth=0,sourceHeight=0,format=0,bytes=0;
    };
    std::vector<EffectImage> effectImages;
    uint32_t effectImageBytes=0,effectImageDeclined=0,pendingEffectImage=UINT32_MAX;
    static constexpr uint32_t kEffectImageBudget=64*1024*1024;
    uint32_t effectImageBudgetLimit=kEffectImageBudget;
    struct CandidateFrame {uint32_t draw=0,ledgerFrame=0,renderFrame=0;int32_t eye=-1;uint32_t issued=2,roiCaptured=0,roiComplete=0,issueStatus=0,b0Bytes=0,viewportCount=0,scissorCount=0,width=0,height=0,rtvFormat=0,b0FirstConstant=0,b0NumConstants=0,b0RangeKnown=0,cameraCbSlot=UINT32_MAX;uint64_t actualVs=0,actualPs=0,b0Identity=0,targetIdentity=0;Buffer b0Stage;D3D11_VIEWPORT viewports[16]{};D3D11_RECT scissors[16]{};D3D11_RENDER_TARGET_VIEW_DESC rtv{};};
    std::vector<CandidateFrame> candidateFrames;
    struct CandidateInput {
        Buffer bufferStage;
        Texture textureStage;
        uint64_t sourceResource=0,sourceView=0;
        uint32_t draw=0,slot=0,kind=0,viewDimension=0,format=0;
        uint32_t width=0,height=0,mip=0,requestedMip=0,mipLevels=0,requestedMipLevels=0,sourceWidth=0,sourceHeight=0,sourceMipLevels=0,sourceArraySize=0,sourceFormat=0;
        uint32_t firstSlice=0,slices=0;
        uint32_t firstElement=0,elements=0,stride=0,rowBytes=0,rows=0,bytes=0;
        uint32_t sampleMode=0;
    };
    struct CandidateRefusal {
        uint32_t draw=0,slot=0,kind=0,viewDimension=0,format=0,sourceWidth=0,sourceHeight=0;
        uint32_t sourceMipLevels=0,sourceArraySize=0,requestedMip=0,requestedMipLevels=0,firstSlice=0,slices=0,reason=0;
        uint64_t sourceResource=0,sourceView=0;
    };
    std::vector<CandidateInput> candidateInputs;
    std::vector<CandidateRefusal> candidateRefusals;
    struct CandidateSampler {uint32_t draw=0,present=0;D3D11_SAMPLER_DESC desc{};};
    std::vector<CandidateSampler> candidateSamplers;
    uint32_t candidateDropped=0,candidateMissingRenderFrame=0,candidateOutsideFrameWindow=0,candidateInputDeclined=0,candidateMipDeclined=0,candidateRoiIncomplete=0;
    uint32_t candidateUiFirstLedgerFrame=0,candidateUiCounts[16][3]{},candidateUiDropped[16][3]{},candidateUiInputBytes[16]{},candidateNonUiDraws=0;
    uint32_t candidateInputBytes=0,candidateOtherInputBytes=0,candidateOpticDraws=0;
    // Reserve 16 rows for every frame/eye bucket; UI activity in one frame
    // cannot consume later frames' candidate capacity.
    // Scene/optic candidates cannot consume that reservation.
    static constexpr uint32_t kCandidateUiPerFrameEye=16,kCandidateUiReserve=16*3*kCandidateUiPerFrameEye,kCandidateNonUiMax=384;
    static constexpr uint32_t kCandidateMaxDraws=kCandidateNonUiMax+kCandidateUiReserve,kCandidateMaxOpticDraws=2;
    static bool candidateCapacityAvailable(bool ui,uint32_t uiCount,uint32_t nonUiCount,uint32_t totalCount) noexcept {
        return totalCount<kCandidateMaxDraws && (ui?uiCount<kCandidateUiPerFrameEye:nonUiCount<kCandidateNonUiMax);
    }
    static constexpr uint32_t kCandidateMeshBudget=64*1024*1024,kCandidateInputBudget=128*1024*1024;
    static constexpr uint32_t kCandidateOtherInputBudget=24*1024*1024,kCandidateUiInputPerFrameBudget=6*1024*1024;
    static constexpr uint32_t kCandidateRoiBudget=8*1024*1024;
    static constexpr uint32_t kCandidateRoiEdge=512;
    static constexpr uint32_t kCandidateUiTextureEdge=384;
    uint32_t candidateRenderFirstFrame=0,candidateRoiDraw=UINT32_MAX,pendingCandidateRoi=UINT32_MAX,pendingCandidateDraw=UINT32_MAX;
    std::vector<SpriteDiagnostic> spriteDiagnostics;
    uint32_t spriteImageBytes=0,spriteDepthBytes=0,spriteImageDeclined=0,pendingSpriteDiagnostic=UINT32_MAX;
    Texture spriteDepthScratch;
    uint32_t spriteDepthScratchWidth=0,spriteDepthScratchHeight=0,spriteDepthScratchFormat=0;
    struct NightSampling {
        uint32_t draw=0,mask=0,viewportCount=0;
        D3D11_SAMPLER_DESC sampler[2]{};
        D3D11_VIEWPORT viewport{};
    };
    std::vector<NightSampling> nightSampling;

    void captureEffectEnd(ID3D11DeviceContext* ctx) {
        const uint32_t sprite=pendingSpriteDiagnostic;pendingSpriteDiagnostic=UINT32_MAX;
        if(ctx && sprite<spriteDiagnostics.size()) {
            auto& d=spriteDiagnostics[sprite];
            if(d.postPending && d.after) {
                D3D11_BOX box{d.x,d.y,0,d.x+d.width,d.y+d.height,1};
                ctx->CopySubresourceRegion(d.after.Get(),0,0,0,0,d.target.Get(),0,&box);
                d.postPending=false;
            }
        }
        const uint32_t before=pendingEffectImage;pendingEffectImage=UINT32_MAX;
        if(!ctx || before>=effectImages.size())return;
        // Copy the original target even if a wrapped draw restored bindings.
        // The index identifies the exact draw, not merely its shader family.
        const auto& b=effectImages[before];
        captureEffectImage(ctx,b.source.Get(),static_cast<DXGI_FORMAT>(b.format),b.draw,true);
    }

    // The game may create these before a dump is armed. Retain only this
    // bounded VS set, then write only shaders seen in the requested run.
    // No broad shader-dump setting or startup disk writes are necessary.
    static void rememberShader(uint64_t hash, const void* bytes, size_t size) {
        if ((!watches(hash) && !sourceMesh(hash) && !sourceEffect(hash) && !solarPixel(hash) && hash!=kF77HeldCandidateVs && hash!=kVscreenPs && hash!=kNightPs && hash!=kSpritePs && hash!=kUnknownAPs && hash!=kUnknownBPs && hash!=kF77OpticPs && hash!=kWeaponUiVs && hash!=kWeaponUiPs) || !bytes || !size || size > 256*1024) return;
        std::lock_guard<std::mutex> lock(shaderMutex());
        auto& shaders = shaderBytes();
        if (shaders.count(hash)) return;
        const auto* p = static_cast<const uint8_t*>(bytes);
        shaders[hash] = std::vector<uint8_t>(p, p + size);
    }
    uint32_t writeShaders(const wchar_t* directory) const {
        std::lock_guard<std::mutex> lock(shaderMutex());
        const auto& shaders=shaderBytes();
        uint32_t missing = 0;
        std::map<uint64_t, bool> seen;
        for (const Draw& d : draws) {
            const uint64_t pixel=d.vs==kVscreen?kVscreenPs:(d.vs==kNight && d.ps==kNightPs?kNightPs:((d.vs==kSprite && d.ps==kSpritePs)?kSpritePs:((d.vs==kUnknownA || d.vs==kUnknownB || (d.vs==kF77OpticVs && d.ps==kF77OpticPs) || (d.vs==kWeaponUiVs&&d.ps==kWeaponUiPs))?d.ps:(solarDraw(d.vs)?d.ps:0))));
            if(pixel && seen.emplace(pixel,true).second) {
                const auto ps=shaders.find(pixel);
                if(ps==shaders.end())++missing;
                else {
                    wchar_t path[MAX_PATH];_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\ps_%016llX.dxbc",directory,static_cast<unsigned long long>(pixel));
                    FILE* f=nullptr;
                    if(_wfopen_s(&f,path,L"wb") || !f)++missing;
                    else {bool ok=fwrite(ps->second.data(),1,ps->second.size(),f)==ps->second.size();if(fclose(f)!=0 || !ok)++missing;}
                }
            }
            if (!seen.emplace(d.vs, true).second) continue;
            const auto it = shaderBytes().find(d.vs);
            if (it == shaderBytes().end()) { ++missing; continue; }
            wchar_t path[MAX_PATH];
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\vs_%016llX.dxbc", directory,
                          static_cast<unsigned long long>(d.vs));
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"wb") || !f) { ++missing; continue; }
            const bool ok = fwrite(it->second.data(), 1, it->second.size(), f) == it->second.size();
            const int closed = fclose(f);
            if (!ok || closed) ++missing;
        }
        return missing;
    }

    void reset() {
        draws.clear(); surfaces.clear(); dropped = failures = textureBytes = 0;
        firstFrame=vertexBytes=vertexDraws=vertexDeclined=0;
        fireGeometrySeen=fireGeometryCaptured=fireGeometryMissingLayout=fireGeometryRefusedBytes=0;
        effectImages.clear();effectImageBytes=effectImageDeclined=0;pendingEffectImage=UINT32_MAX;
        spriteDiagnostics.clear();spriteImageBytes=spriteDepthBytes=spriteImageDeclined=0;pendingSpriteDiagnostic=UINT32_MAX;
        spriteDepthScratch.Reset();spriteDepthScratchWidth=spriteDepthScratchHeight=spriteDepthScratchFormat=0;
        nightSampling.clear();
        meshBuffers.clear();meshBytes=meshDeclined=meshDraws=0;meshBudgetLimit=kMeshBudget;
        meshCaptureFirstFrame=meshCaptureDeclined=meshCaptureSnapshots=meshNonBodyDraws=0;
        for(uint32_t i=0;i<kEyeMeshSnapshots;++i){meshSnapshotRenderFrame[i]=meshSnapshotLedgerFrame[i]=0;meshSnapshotNativeValid[i]=false;meshSnapshotEye[i]=-1;}
        candidateFrames.clear();candidateInputs.clear();candidateRefusals.clear();candidateSamplers.clear();candidateDropped=candidateMissingRenderFrame=candidateOutsideFrameWindow=candidateInputDeclined=candidateMipDeclined=candidateRoiIncomplete=0;
        candidateInputBytes=candidateOtherInputBytes=candidateOpticDraws=candidateRenderFirstFrame=candidateUiFirstLedgerFrame=candidateNonUiDraws=0;std::memset(candidateUiCounts,0,sizeof(candidateUiCounts));std::memset(candidateUiDropped,0,sizeof(candidateUiDropped));std::memset(candidateUiInputBytes,0,sizeof(candidateUiInputBytes));candidateRoiDraw=pendingCandidateRoi=pendingCandidateDraw=UINT32_MAX;
        effectImageBudgetLimit=kEffectImageBudget;
        sourceDepth.Reset();sourceDepthFormat=DXGI_FORMAT_UNKNOWN;sourceFrame=0;
        armGeometry(0);
    }

    uint32_t captureMeshBuffer(ID3D11DeviceContext* ctx,ID3D11Device* dev,
                               ID3D11Buffer* src,uint32_t frame,uint32_t stride,uint64_t scope=0,bool perDraw=false,uint32_t* status=nullptr) {
        if(!src)return UINT32_MAX;
        if(!perDraw)for(uint32_t i=0;i<meshBuffers.size();++i)
            if(meshBuffers[i].frame==frame && meshBuffers[i].scope==scope && meshBuffers[i].source.Get()==src)return i;
        D3D11_BUFFER_DESC bd{};src->GetDesc(&bd);
        // The F82 eye capture passes perDraw: a shared resource can be
        // rewritten between draws, so identity alone cannot deduplicate it.
        if(!bd.ByteWidth || bd.ByteWidth>16*1024*1024 || bd.ByteWidth>meshBudgetLimit-meshBytes){++meshDeclined;if(status)*status=5;return UINT32_MAX;}
        MeshBuffer b;b.source=src;b.frame=frame;b.firstDraw=uint32_t(draws.size());b.bytes=bd.ByteWidth;b.stride=stride;b.scope=scope;
        bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&b.stage))){++failures;if(status)*status=6;return UINT32_MAX;}
        ctx->CopyResource(b.stage.Get(),src);meshBytes+=b.bytes;if(status)*status=1;
        meshBuffers.push_back(std::move(b));return uint32_t(meshBuffers.size()-1);
    }

    void captureSourceMesh(ID3D11DeviceContext* ctx,uint32_t frame,uint64_t vs,uint64_t ps,
                           char kind,uint32_t count,uint32_t instances,uint32_t startInstance,uint32_t start,int32_t base) {
        pendingEffectImage=UINT32_MAX;
        if(sourceEffect(vs)) {
            capture(ctx,frame,UINT32_MAX-2,vs,ps,kind,count,instances,startInstance,start,base,true);
            return;
        }
        if(!sourceMesh(vs))return;
        // Reserved ordinal distinct from the source camera and eye ledger.
        capture(ctx,frame,UINT32_MAX-1,vs,ps,kind,count,instances,startInstance,start,base,true);
    }

    // The 12:02:29 exterior hull was absent from the UI/terrain snapshot;
    // boundary pool copies were not synchronized with its draw camera.
    // Use a separate snapshot/cap so these mesh draws cannot crowd out UI.
    // The caller must already have established an eye draw and armed ledger.
    void captureEyeMesh(ID3D11DeviceContext* ctx,uint32_t frame,uint32_t renderFrame,int32_t eye,const EyeMeshJoinStamp& join,uint32_t ordinal,uint64_t vs,uint64_t ps,uint32_t stencilRef,ID3D11Buffer* otherPaletteCandidate,
                        char kind,uint32_t count,uint32_t instances,uint32_t startInstance,uint32_t start,int32_t base) {
        if(!sourceMesh(vs))return;
        const bool selected=bodyMeshSelector(vs,ps,stencilRef,count);
        if(!selected){++meshNonBodyDraws;return;}
        if(!meshCaptureFirstFrame)meshCaptureFirstFrame=frame;
        if(frame<meshCaptureFirstFrame || frame-meshCaptureFirstFrame>=kEyeMeshFrames) {
            ++meshCaptureDeclined; return;
        }
        bool snapshotPermit=false,seenSnapshot=false;
        if(selected&&eye>=0)for(uint32_t i=0;i<meshCaptureSnapshots;++i) {
            if(meshSnapshotNativeValid[i]!=(renderFrame!=0) || meshSnapshotEye[i]!=eye)continue;
            if(renderFrame ? meshSnapshotRenderFrame[i]==renderFrame : meshSnapshotLedgerFrame[i]==frame){seenSnapshot=true;break;}
        }
        if(selected&&eye>=0&&!seenSnapshot&&meshCaptureSnapshots<kEyeMeshSnapshots){
            meshSnapshotRenderFrame[meshCaptureSnapshots]=renderFrame;
            meshSnapshotLedgerFrame[meshCaptureSnapshots]=frame;
            meshSnapshotNativeValid[meshCaptureSnapshots]=renderFrame!=0;
            meshSnapshotEye[meshCaptureSnapshots]=eye;
            ++meshCaptureSnapshots;snapshotPermit=true;
        }
        if(snapshotPermit)meshBudgetLimit=std::min(kEyeMeshBudget,meshBytes+kEyeMeshSnapshotBudget);
        const uint32_t before=uint32_t(draws.size());
        capture(ctx,frame,ordinal,vs,ps,kind,count,instances,startInstance,start,base,true,true,false,renderFrame,eye,snapshotPermit,selected,otherPaletteCandidate);
        if(draws.size()==before)++meshCaptureDeclined;
        else draws.back().join=join;
    }

    void armF77Candidates(uint32_t firstLedgerFrame=0) {
        reset();meshBudgetLimit=kCandidateMeshBudget;effectImageBudgetLimit=kCandidateRoiBudget;candidateUiFirstLedgerFrame=firstLedgerFrame;
    }
    static bool f77RenderFrameEligible(uint32_t first,uint32_t frame) noexcept {
        return first && frame>=first && frame-first<3;
    }
    bool wantsEyeMeshCapture(uint32_t frame,uint64_t vs,uint64_t ps,uint32_t stencilRef,uint32_t count) const noexcept {
        return bodyMeshSelector(vs,ps,stencilRef,count) && draws.size()<kMaxDraws &&
               (!meshCaptureFirstFrame || (frame>=meshCaptureFirstFrame && frame-meshCaptureFirstFrame<kEyeMeshFrames));
    }
    void captureF77Candidate(ID3D11DeviceContext* ctx,uint32_t ledgerFrame,uint32_t renderFrame,int32_t eye,uint32_t ordinal,
                             uint64_t vs,uint64_t ps,char kind,uint32_t count,uint32_t instances,
                             uint32_t startInstance,uint32_t start,int32_t base) {
        if(!ctx || !f77Candidate(vs,ps))return;
        unsigned uiIndex=16,uiBucket=3;
        if(weaponUiCandidate(vs,ps)){
            if(!candidateUiFirstLedgerFrame||ledgerFrame<candidateUiFirstLedgerFrame||ledgerFrame-candidateUiFirstLedgerFrame>=16){++candidateOutsideFrameWindow;return;}
            uiBucket=eye==0?0u:eye==1?1u:2u;uiIndex=ledgerFrame-candidateUiFirstLedgerFrame;
            if(!candidateCapacityAvailable(true,candidateUiCounts[uiIndex][uiBucket],candidateNonUiDraws,uint32_t(candidateFrames.size()))) {
                ++candidateDropped;++candidateUiDropped[uiIndex][uiBucket];return;
            }
            ++candidateUiCounts[uiIndex][uiBucket];
        } else {
            if(!renderFrame){++candidateMissingRenderFrame;return;}
            if(!candidateRenderFirstFrame)candidateRenderFirstFrame=renderFrame;
            if(!f77RenderFrameEligible(candidateRenderFirstFrame,renderFrame)){++candidateOutsideFrameWindow;return;}
            if(!candidateCapacityAvailable(false,0,candidateNonUiDraws,uint32_t(candidateFrames.size()))) {++candidateDropped;return;}
        }
        const uint32_t before=uint32_t(draws.size());
        capture(ctx,ledgerFrame,ordinal,vs,ps,kind,count,instances,startInstance,start,base,true,true,true,renderFrame,eye);
        if(draws.size()==before)++candidateDropped;
        else {candidateFrames.back().eye=eye;if(!weaponUiCandidate(vs,ps))++candidateNonUiDraws;}
    }
    void candidateDrawIssued(ID3D11DeviceContext* ctx,bool issued,uint64_t actualVs,uint64_t actualPs) {
        if(pendingCandidateDraw>=candidateFrames.size())return;
        CandidateFrame& c=candidateFrames[pendingCandidateDraw];c.issued=issued?1u:0u;c.actualVs=actualVs;c.actualPs=actualPs;
        if(!issued)c.issueStatus=1; // original candidate was not issued
        else if(c.draw>=draws.size() || !weaponUiCandidate(draws[c.draw].vs,draws[c.draw].ps))c.issueStatus=4; // outside measured UI shader families
        else if((actualVs!=kWeaponUiVs||actualPs!=kWeaponUiPs) && (actualVs!=kF77OpticVs||actualPs!=kF77OpticPs))c.issueStatus=5; // effective shader changed
        else if(!ctx)c.issueStatus=3;
        else {
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rt;ctx->OMGetRenderTargets(1,&rt,nullptr);
            if(rt){rt->GetDesc(&c.rtv);Microsoft::WRL::ComPtr<ID3D11Resource> resource;rt->GetResource(&resource);
                c.targetIdentity=reinterpret_cast<uint64_t>(resource.Get());Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                if(SUCCEEDED(resource.As(&texture))){D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);c.width=td.Width;c.height=td.Height;c.rtvFormat=td.Format;}}
            UINT nv=16,ns=16;ctx->RSGetViewports(&nv,c.viewports);ctx->RSGetScissorRects(&ns,c.scissors);c.viewportCount=(std::min)(nv,16u);c.scissorCount=(std::min)(ns,16u);
            const UINT cameraSlot=actualVs==kF77OpticVs?1u:0u;c.cameraCbSlot=cameraSlot;
            Microsoft::WRL::ComPtr<ID3D11Buffer> b0;Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
            if(SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))){UINT first=0,count=0;ctx1->VSGetConstantBuffers1(cameraSlot,1,&b0,&first,&count);c.b0FirstConstant=first;c.b0NumConstants=count;c.b0RangeKnown=1;}
            else ctx->VSGetConstantBuffers(cameraSlot,1,&b0);
            if(b0){D3D11_BUFFER_DESC bd{};b0->GetDesc(&bd);c.b0Identity=reinterpret_cast<uint64_t>(b0.Get());
                const bool rangeValid=(!c.b0RangeKnown || (c.b0FirstConstant<=4096&&c.b0NumConstants<=4096)) &&
                    (cameraSlot==0 || (c.b0RangeKnown?(c.b0NumConstants>=276&&uint64_t(c.b0FirstConstant+276u)*16u<=bd.ByteWidth):bd.ByteWidth>=276u*16u));
                if(bd.ByteWidth && bd.ByteWidth<=64*1024 && rangeValid){bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    Microsoft::WRL::ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
                    if(SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&c.b0Stage))){ctx->CopyResource(c.b0Stage.Get(),b0.Get());c.b0Bytes=bd.ByteWidth;
                        if(c.targetIdentity&&c.width&&c.height)c.issueStatus=2;else {c.b0Stage.Reset();c.b0Bytes=0;c.issueStatus=3;}}else c.issueStatus=3;
                }else c.issueStatus=3;
            }else c.issueStatus=3;
        }
        if(pendingCandidateRoi!=UINT32_MAX && pendingCandidateDraw==c.draw) {
            if(ctx && issued && pendingCandidateRoi<effectImages.size()) {
                auto& after=effectImages[pendingCandidateRoi];
                D3D11_BOX box{after.x,after.y,0,after.x+after.width,after.y+after.height,1};
                ctx->CopySubresourceRegion(after.stage.Get(),0,0,0,0,after.source.Get(),0,&box);
                c.roiComplete=1;
            } else ++candidateRoiIncomplete;
            pendingCandidateRoi=UINT32_MAX;
        }
        pendingCandidateDraw=UINT32_MAX;
    }
    void candidateDrawEnd() {
        if(pendingCandidateDraw>=candidateFrames.size())return;
        CandidateFrame& c=candidateFrames[pendingCandidateDraw];c.issued=2;
        if(pendingCandidateRoi!=UINT32_MAX && pendingCandidateDraw==c.draw) {
            ++candidateRoiIncomplete;pendingCandidateRoi=UINT32_MAX;
        }
        pendingCandidateDraw=UINT32_MAX;
    }

    // --- Version 9: one armed frame's pool-draw geometry -------------------
    // The cull gate probe's occluder inventory (advanced.cull_gate_capture,
    // design-occlusion-culling-2026-09-22.md §9): EVERY instanced eye draw
    // of the armed frame whose t33 held the pool gets a map row (frame, the
    // ledger's own ordinal, mesh, state, VS/PS, instance range), so the
    // reader joins any ledger row to its geometry and chooses the occluders
    // offline from the pool positions and the depth. Each distinct mesh --
    // (vertex buffer, slot, offset, stride, index buffer, offset, format,
    // start, base, count, kind, topology) -- is copied once: the index
    // window [start, start+count) and a vertex window from the base vertex
    // for `count` vertices (a mesh whose indices reach past it is flagged by
    // the reader, not guessed), with the input layout that decodes it. Each
    // distinct blend/depth-stencil/raster state is kept once. Separate
    // budget, explicit declines; off (geoFrame 0) unless the probe armed it.
    struct GeoMesh {
        uint64_t vb=0,ib=0;
        uint32_t vbSlot=0,vbOffset=0,vbStride=0,vbWhole=0,ibOffset=0,ibFormat=0,ibWhole=0;
        uint32_t start=0;int32_t base=0;uint32_t count=0,kind=0,topology=0;
        uint32_t vCapture=0,vBytes=0,iCapture=0,iBytes=0;
        Buffer vStage,iStage;
        std::vector<Layout> layout;
    };
    struct GeoState {
        uint32_t flags=0;   // 1 blend state bound, 2 depth-stencil state bound, 4 rasterizer state bound
        D3D11_BLEND_DESC blend{};float factor[4]{};uint32_t sampleMask=0;
        D3D11_DEPTH_STENCIL_DESC depth{};uint32_t stencilRef=0;
        D3D11_RASTERIZER_DESC raster{};
    };
    struct GeoDraw {
        uint32_t frame=0,ordinal=0,mesh=UINT32_MAX,state=UINT32_MAX;
        uint64_t vs=0,ps=0;uint32_t instances=0,startInstance=0;
    };
    std::vector<GeoMesh> geoMeshes;
    std::vector<GeoState> geoStates;
    std::vector<GeoDraw> geoDraws;
    std::map<std::vector<uint64_t>,uint32_t> geoMeshIndex,geoStateIndex;
    uint32_t geoFrame=0,geoBytes=0,geoDeclined=0,geoDrawsDropped=0;
    static constexpr uint32_t kGeoBudget=96*1024*1024,kGeoMaxDraws=40000,kGeoMaxMeshes=20000;

    void armGeometry(uint32_t frame) {
        geoMeshes.clear();geoStates.clear();geoDraws.clear();geoMeshIndex.clear();geoStateIndex.clear();
        geoFrame=frame;geoBytes=geoDeclined=geoDrawsDropped=0;
    }
    // The arm across a reset (design-occlusion-culling-2026-09-22.md §10).
    // reset() clears the arm with the rest of the snapshot: on run 152632
    // the pool's first sighting, 60 ms after the gate probe armed frame 2,
    // ran releasePool -> ledgerRelease -> reset() and the section came back
    // empty. A caller releasing mid-run calls this instead. The arm comes
    // back only when the release cost it nothing -- `now` (the ledger frame
    // whose draws are being submitted) is not past the armed frame and none
    // of its draws has been mapped; otherwise it stays disarmed and the loss
    // is kept for the run's log line, never re-armed as a partial frame. Only
    // geo* state is touched: no other capture's arm can be cleared by it.
    // The fate outlives reset(); clearGeometryFate() starts a run.
    enum GeoRelease : uint32_t { kGeoNotArmed=0, kGeoRearmed=1, kGeoLost=2 };
    uint32_t geoRearms=0,geoLostFrame=0,geoLostAt=0,geoLostDraws=0;
    void clearGeometryFate() { geoRearms=geoLostFrame=geoLostAt=geoLostDraws=0; }
    GeoRelease resetKeepingGeometry(uint32_t now) {
        const uint32_t armed=geoFrame,mapped=uint32_t(geoDraws.size());
        reset();
        if(!armed)return kGeoNotArmed;
        if(now<=armed && !mapped){armGeometry(armed);++geoRearms;return kGeoRearmed;}
        geoLostFrame=armed;geoLostAt=now;geoLostDraws=mapped;
        return kGeoLost;
    }
    // One ledger row of the armed frame; the caller has already decided
    // the draw is instanced and that t33 held the pool.
    void captureGeometry(ID3D11DeviceContext* ctx,uint32_t frame,uint32_t ordinal,uint64_t vs,uint64_t ps,
                         char kind,uint32_t count,uint32_t instances,uint32_t startInstance,uint32_t start,int32_t base) {
        if(!ctx || !geoFrame || frame!=geoFrame)return;
        if(geoDraws.size()>=kGeoMaxDraws){++geoDrawsDropped;return;}
        GeoDraw g;g.frame=frame;g.ordinal=ordinal;g.vs=vs;g.ps=ps;g.instances=instances;g.startInstance=startInstance;
        g.state=geoCaptureState(ctx);
        g.mesh=geoCaptureMesh(ctx,kind,count,start,base);
        geoDraws.push_back(g);
    }

    uint32_t geoCaptureState(ID3D11DeviceContext* ctx) {
        ID3D11BlendState* bs=nullptr;FLOAT factor[4]{};UINT mask=0;ctx->OMGetBlendState(&bs,factor,&mask);
        ID3D11DepthStencilState* ds=nullptr;UINT ref=0;ctx->OMGetDepthStencilState(&ds,&ref);
        ID3D11RasterizerState* rs=nullptr;ctx->RSGetState(&rs);
        uint32_t f[4];std::memcpy(f,factor,sizeof(f));
        const std::vector<uint64_t> key={reinterpret_cast<uint64_t>(bs),reinterpret_cast<uint64_t>(ds),reinterpret_cast<uint64_t>(rs),
                                         (uint64_t(f[0])<<32)|f[1],(uint64_t(f[2])<<32)|f[3],(uint64_t(mask)<<32)|ref};
        uint32_t index=UINT32_MAX;
        const auto it=geoStateIndex.find(key);
        if(it!=geoStateIndex.end())index=it->second;
        else {
            GeoState s;
            if(bs){bs->GetDesc(&s.blend);s.flags|=1u;}
            if(ds){ds->GetDesc(&s.depth);s.flags|=2u;}
            if(rs){rs->GetDesc(&s.raster);s.flags|=4u;}
            std::memcpy(s.factor,factor,sizeof(s.factor));s.sampleMask=mask;s.stencilRef=ref;
            index=uint32_t(geoStates.size());geoStates.push_back(s);geoStateIndex.emplace(key,index);
        }
        if(bs)bs->Release();if(ds)ds->Release();if(rs)rs->Release();
        return index;
    }

    uint32_t geoCaptureMesh(ID3D11DeviceContext* ctx,char kind,uint32_t count,uint32_t start,int32_t base) {
        // The vertex slot: the one the layout reads per vertex; without a
        // remembered layout, slot 1 when slot 0 is the 8-byte instance stream.
        std::vector<Layout> layout;
        {
            Microsoft::WRL::ComPtr<ID3D11InputLayout> il;ctx->IAGetInputLayout(&il);
            Layout elements[32];UINT bytes=sizeof(elements);
            if(il && SUCCEEDED(il->GetPrivateData(effectLayoutKey(),&bytes,elements)) && bytes &&
               bytes<=sizeof(elements) && bytes%sizeof(Layout)==0)layout.assign(elements,elements+bytes/sizeof(Layout));
        }
        ID3D11Buffer* raw[2]{};UINT strides[2]{},offsets[2]{};ctx->IAGetVertexBuffers(0,2,raw,strides,offsets);
        Buffer vbs[2];vbs[0].Attach(raw[0]);vbs[1].Attach(raw[1]);
        uint32_t slot=UINT32_MAX;
        for(const auto& e:layout)if(e.classification==D3D11_INPUT_PER_VERTEX_DATA && e.slot<2){slot=e.slot;break;}
        if(slot==UINT32_MAX)slot=(strides[0]==8 && vbs[1])?1u:0u;
        ID3D11Buffer* ibRaw=nullptr;DXGI_FORMAT fmt=DXGI_FORMAT_UNKNOWN;UINT ibOffset=0;ctx->IAGetIndexBuffer(&ibRaw,&fmt,&ibOffset);
        Buffer ib;ib.Attach(ibRaw);
        D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;ctx->IAGetPrimitiveTopology(&topology);
        const bool indexed=kind=='X'||kind=='I';
        const std::vector<uint64_t> key={reinterpret_cast<uint64_t>(vbs[slot].Get()),(uint64_t(slot)<<32)|offsets[slot],
                                         strides[slot],indexed?reinterpret_cast<uint64_t>(ib.Get()):0,
                                         indexed?((uint64_t(ibOffset)<<32)|uint32_t(fmt)):0,
                                         (uint64_t(start)<<32)|uint32_t(base),(uint64_t(count)<<32)|uint32_t(kind),uint64_t(topology)};
        const auto it=geoMeshIndex.find(key);
        if(it!=geoMeshIndex.end())return it->second;
        if(geoMeshes.size()>=kGeoMaxMeshes || !vbs[slot] || !strides[slot]){++geoDeclined;return UINT32_MAX;}
        GeoMesh m;m.vb=reinterpret_cast<uint64_t>(vbs[slot].Get());m.vbSlot=slot;m.vbOffset=offsets[slot];m.vbStride=strides[slot];
        m.start=start;m.base=base;m.count=count;m.kind=uint32_t(uint8_t(kind));m.topology=uint32_t(topology);m.layout=std::move(layout);
        D3D11_BUFFER_DESC vd{};vbs[slot]->GetDesc(&vd);m.vbWhole=vd.ByteWidth;
        // Vertex window: indexed draws from the base vertex for `count`
        // vertices (a mesh's distinct vertices never outnumber its indices
        // unless its indices skip ahead -- the reader checks), non-indexed
        // draws exactly [start, start+count).
        const uint64_t firstVertex=indexed?uint64_t(base>0?base:0):uint64_t(start);
        const uint64_t vBegin=uint64_t(m.vbOffset)+firstVertex*m.vbStride;
        uint64_t vBytes=uint64_t(count<65536u?count:65536u)*m.vbStride;
        if(vBegin<m.vbWhole && vBegin+vBytes>m.vbWhole)vBytes=m.vbWhole-vBegin;
        if(vBegin>=m.vbWhole)vBytes=0;
        uint64_t iBegin=0,iBytes=0;
        if(indexed && ib) {
            const uint32_t is=fmt==DXGI_FORMAT_R16_UINT?2u:fmt==DXGI_FORMAT_R32_UINT?4u:0u;
            D3D11_BUFFER_DESC id{};ib->GetDesc(&id);
            m.ib=reinterpret_cast<uint64_t>(ib.Get());m.ibOffset=ibOffset;m.ibFormat=uint32_t(fmt);m.ibWhole=id.ByteWidth;
            iBegin=uint64_t(ibOffset)+uint64_t(start)*is;iBytes=uint64_t(count)*is;
            if(!is || iBegin>=m.ibWhole)iBytes=0;
            else if(iBegin+iBytes>m.ibWhole)iBytes=m.ibWhole-iBegin;
        }
        if((!vBytes && !iBytes) || uint64_t(geoBytes)+vBytes+iBytes>kGeoBudget){++geoDeclined;return UINT32_MAX;}
        Microsoft::WRL::ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
        auto stage=[&](ID3D11Buffer* src,uint64_t begin,uint64_t bytes,Buffer& out,uint32_t& capture,uint32_t& copied)->bool{
            if(!bytes)return true;
            D3D11_BUFFER_DESC sd{};sd.ByteWidth=uint32_t(bytes);sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(!dev || FAILED(dev->CreateBuffer(&sd,nullptr,&out))){++failures;return false;}
            D3D11_BOX box{uint32_t(begin),0,0,uint32_t(begin+bytes),1,1};
            ctx->CopySubresourceRegion(out.Get(),0,0,0,0,src,0,&box);
            capture=uint32_t(begin);copied=uint32_t(bytes);geoBytes+=uint32_t(bytes);return true;
        };
        stage(vbs[slot].Get(),vBegin,vBytes,m.vStage,m.vCapture,m.vBytes);
        if(ib)stage(ib.Get(),iBegin,iBytes,m.iStage,m.iCapture,m.iBytes);
        const uint32_t index=uint32_t(geoMeshes.size());
        geoMeshes.push_back(std::move(m));geoMeshIndex.emplace(key,index);
        return index;
    }

    // First world/terrain draw per source frame, not every offscreen draw.
    // Retain the scene DSV now, copy it at the final screen composite after
    // the scene is complete. This diagnostic never selects temporal inputs.
    void captureSource(ID3D11DeviceContext* ctx,uint32_t frame,uint64_t vs,uint64_t ps,
                       char kind,uint32_t count,uint32_t instances,uint32_t startInstance,uint32_t start,int32_t base) {
        if(!ctx || (vs!=kScene && vs!=0xACE405F428C17EF6ull))return;
        if(sourceFrame==frame)return;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(0,nullptr,&dsv);
        if(!dsv)return;
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;dsv->GetResource(&resource);Texture tex;
        if(FAILED(resource.As(&tex)))return;
        D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dsv->GetDesc(&dd);
        if(td.SampleDesc.Count!=1 || td.ArraySize!=1 || dd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D)return;
        sourceDepth=tex;sourceDepthFormat=dd.Format;sourceFrame=frame;
        capture(ctx,frame,UINT32_MAX,vs,ps,kind,count,instances,startInstance,start,base,true);
    }

    uint32_t captureSpriteDiagnostic(ID3D11DeviceContext* ctx,ID3D11Device* dev,
                                     const Draw& draw,ID3D11RenderTargetView* rtv) {
        if(!ctx || !dev || !rtv)return UINT32_MAX;
        if(spriteDiagnostics.size()>=kSpriteImageCount/2){++spriteImageDeclined;return UINT32_MAX;}
        Microsoft::WRL::ComPtr<ID3D11Resource> rawTarget;rtv->GetResource(&rawTarget);
        Texture target;if(!rawTarget || FAILED(rawTarget.As(&target)))return UINT32_MAX;
        D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
        if(td.SampleDesc.Count!=1 || td.ArraySize!=1 || !td.Width || !td.Height)return UINT32_MAX;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};rtv->GetDesc(&rd);
        if(rd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice!=0)return UINT32_MAX;
        uint32_t bpp=0;switch(td.Format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R11G11B10_FLOAT:bpp=4;break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:bpp=8;break;
        default:return UINT32_MAX;
        }
        SpriteDiagnostic d;d.frame=draw.frame;d.draw=static_cast<uint32_t>(draws.size());
        d.target=target;d.rtIdentity=reinterpret_cast<uint64_t>(target.Get());
        d.rtFormat=static_cast<uint32_t>(rd.Format);d.sourceWidth=td.Width;d.sourceHeight=td.Height;
        d.width=td.Width<kSpriteCrop?td.Width:kSpriteCrop;d.height=td.Height<kSpriteCrop?td.Height:kSpriteCrop;
        d.x=(td.Width-d.width)/2;d.y=(td.Height-d.height)/2;
        d.beforeBytes=d.afterBytes=d.width*d.height*bpp;
        const uint64_t pair=uint64_t(d.beforeBytes)*2;
        if(!pair || pair>kSpriteImageBytes-spriteImageBytes) {++spriteImageDeclined;return UINT32_MAX;}
        td.Width=d.width;td.Height=d.height;td.MipLevels=1;td.Usage=D3D11_USAGE_STAGING;
        td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&d.before)) ||
           FAILED(dev->CreateTexture2D(&td,nullptr,&d.after))) {++spriteImageDeclined;return UINT32_MAX;}
        D3D11_BOX box{d.x,d.y,0,d.x+d.width,d.y+d.height,1};
        ctx->CopySubresourceRegion(d.before.Get(),0,0,0,0,target.Get(),0,&box);
        d.postPending=true;

        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(0,nullptr,&dsv);
        if(!dsv) {++spriteImageDeclined;return UINT32_MAX;}
        Microsoft::WRL::ComPtr<ID3D11Resource> rawDepth;dsv->GetResource(&rawDepth);Texture depthSrc;
        if(!rawDepth || FAILED(rawDepth.As(&depthSrc))) {++spriteImageDeclined;return UINT32_MAX;}
        D3D11_TEXTURE2D_DESC dd{};depthSrc->GetDesc(&dd);D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};dsv->GetDesc(&dsvd);
        d.dsvFlags=dsvd.Flags;
        if(dsvd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || dsvd.Texture2D.MipSlice!=0 ||
           dd.SampleDesc.Count!=1 || dd.ArraySize!=1 || dd.MipLevels!=1 || !dd.Width || !dd.Height || d.x+d.width>dd.Width || d.y+d.height>dd.Height) {++spriteImageDeclined;return UINT32_MAX;}
        uint32_t dbpp=0;switch(dd.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:dbpp=8;break;
        case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:dbpp=4;break;
        case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:dbpp=2;break;
        default:++spriteImageDeclined;return UINT32_MAX;
        }
        // Full depth copy is required before cropping a non-DSV resource.
        // Bound this reusable intermediate independently of the crop budget.
        if(uint64_t(dd.Width)*dd.Height*dbpp>128u*1024u*1024u) {++spriteImageDeclined;return UINT32_MAX;}
        d.depthImage=depthSrc;d.depthIdentity=reinterpret_cast<uint64_t>(depthSrc.Get());d.depthFormat=static_cast<uint32_t>(dsvd.Format);d.depthResourceFormat=static_cast<uint32_t>(dd.Format);d.depthBytes=d.width*d.height*dbpp;d.stateMask|=1u<<3;
        if(!d.depthBytes || uint64_t(d.depthBytes)>kSpriteDepthBytes-spriteDepthBytes) {++spriteImageDeclined;return UINT32_MAX;}
        D3D11_TEXTURE2D_DESC dcopy=dd;dcopy.Width=d.width;dcopy.Height=d.height;dcopy.MipLevels=1;dcopy.Usage=D3D11_USAGE_STAGING;dcopy.BindFlags=dcopy.MiscFlags=0;dcopy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(!spriteDepthScratch || spriteDepthScratchWidth!=dd.Width || spriteDepthScratchHeight!=dd.Height || spriteDepthScratchFormat!=static_cast<uint32_t>(dd.Format)) {
            spriteDepthScratch.Reset();D3D11_TEXTURE2D_DESC scratch=dd;scratch.Usage=D3D11_USAGE_DEFAULT;scratch.BindFlags=scratch.MiscFlags=scratch.CPUAccessFlags=0;
            if(FAILED(dev->CreateTexture2D(&scratch,nullptr,&spriteDepthScratch))) {++spriteImageDeclined;return UINT32_MAX;}
            spriteDepthScratchWidth=dd.Width;spriteDepthScratchHeight=dd.Height;spriteDepthScratchFormat=static_cast<uint32_t>(dd.Format);
        }
        if(FAILED(dev->CreateTexture2D(&dcopy,nullptr,&d.depthImage))) {++spriteImageDeclined;return UINT32_MAX;}
        ctx->CopyResource(spriteDepthScratch.Get(),depthSrc.Get());
        ctx->CopySubresourceRegion(d.depthImage.Get(),0,0,0,0,spriteDepthScratch.Get(),0,&box);

        const uint64_t drawScope=(uint64_t(draw.frame)<<32)|uint64_t(d.draw);
        for(unsigned i=0;i<2;++i) {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> meshSrv;
            ctx->VSGetShaderResources(i?38:33,1,&meshSrv);
            if(!meshSrv) {++meshDeclined;continue;}
            D3D11_SHADER_RESOURCE_VIEW_DESC msd{};meshSrv->GetDesc(&msd);
            Microsoft::WRL::ComPtr<ID3D11Resource> meshRes;meshSrv->GetResource(&meshRes);Buffer meshBuffer;
            if(msd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || msd.Format!=DXGI_FORMAT_UNKNOWN || msd.Buffer.FirstElement!=0 ||
               !meshRes || FAILED(meshRes.As(&meshBuffer))) {++meshDeclined;continue;}
            D3D11_BUFFER_DESC mbd{};meshBuffer->GetDesc(&mbd);const uint32_t expected=i?48u:336u;
            if(mbd.StructureByteStride!=expected || !mbd.ByteWidth || uint64_t(msd.Buffer.NumElements)*expected!=mbd.ByteWidth) {++meshDeclined;continue;}
            d.mesh[i]=captureMeshBuffer(ctx,dev,meshBuffer.Get(),draw.frame,expected,drawScope);
            if(d.mesh[i]==UINT32_MAX)++meshDeclined;
        }

        ID3D11Buffer* b1=nullptr;ctx->PSGetConstantBuffers(1,1,&b1);
        if(b1) {
            d.stateMask|=1u<<6;
            d.psB1Identity=reinterpret_cast<uint64_t>(b1);d.psB1Whole=0;
            D3D11_BUFFER_DESC bd{};b1->GetDesc(&bd);d.psB1Whole=bd.ByteWidth;
            d.psB1Bytes=bd.ByteWidth<8192?bd.ByteWidth:8192;
            D3D11_BUFFER_DESC sd=bd;sd.ByteWidth=d.psB1Bytes;sd.Usage=D3D11_USAGE_STAGING;
            sd.BindFlags=sd.MiscFlags=sd.StructureByteStride=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(d.psB1Bytes && SUCCEEDED(dev->CreateBuffer(&sd,nullptr,&d.psB1))) {
                D3D11_BOX cb{0,0,0,d.psB1Bytes,1,1};ctx->CopySubresourceRegion(d.psB1.Get(),0,0,0,0,b1,0,&cb);
            } else {d.psB1Bytes=0;++failures;}
            b1->Release();
        }
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;ctx->PSGetSamplers(0,1,&sampler);
        if(sampler){sampler->GetDesc(&d.sampler);d.stateMask|=1u<<0;}
        ID3D11BlendState* blend=nullptr;FLOAT factor[4]{};UINT mask=0;
        ctx->OMGetBlendState(&blend,factor,&mask);if(blend){blend->GetDesc(&d.blend);d.stateMask|=1u<<1;blend->Release();}
        std::memcpy(d.blendFactor,factor,sizeof(factor));d.sampleMask=mask;
        ID3D11DepthStencilState* depth=nullptr;ctx->OMGetDepthStencilState(&depth,&d.stencilRef);
        if(depth){depth->GetDesc(&d.depth);d.stateMask|=1u<<2;depth->Release();}
        if(dsv){d.stateMask|=1u<<3;}
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;ctx->RSGetState(&raster);
        if(raster){raster->GetDesc(&d.raster);d.rasterFill=d.raster.FillMode;d.rasterCull=d.raster.CullMode;d.stateMask|=1u<<4;}
        d.scissorCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetScissorRects(&d.scissorCount,d.scissor);
        d.viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&d.viewportCount,d.viewport);
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;ctx->PSGetShaderResources(0,1,&srv);
        if(srv) {
            d.srvIdentity=reinterpret_cast<uint64_t>(srv.Get());d.stateMask|=1u<<5;D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);
            d.srvFormat=static_cast<uint32_t>(sd.Format);
            if(sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D) {
                d.srvMostDetailedMip=sd.Texture2D.MostDetailedMip;d.srvMipLevels=sd.Texture2D.MipLevels;
            }
            Microsoft::WRL::ComPtr<ID3D11Resource> res;srv->GetResource(&res);Texture tex;
            if(res && SUCCEEDED(res.As(&tex))) {D3D11_TEXTURE2D_DESC sdesc{};tex->GetDesc(&sdesc);d.srvWidth=sdesc.Width;d.srvHeight=sdesc.Height;d.srvArraySize=sdesc.ArraySize;d.srvMipCount=sdesc.MipLevels;}
        }
        d.layout=draw.layout;
        D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);d.topology=static_cast<uint32_t>(topology);
        spriteImageBytes+=static_cast<uint32_t>(pair);spriteDepthBytes+=d.depthBytes;spriteDiagnostics.push_back(std::move(d));
        return static_cast<uint32_t>(spriteDiagnostics.size()-1);
    }

    void capture(ID3D11DeviceContext* ctx, uint32_t frame, uint32_t ordinal,
                 uint64_t vs, uint64_t ps, char kind, uint32_t count,
                 uint32_t instances, uint32_t startInstance,uint32_t start=0,int32_t base=0,bool source=false,bool eyeMesh=false,
                 bool f77CandidateDraw=false,uint32_t renderFrame=0,int32_t eye=-1,bool meshSnapshotPermit=true,bool meshSelected=false,ID3D11Buffer* otherPaletteCandidate=nullptr) {
        const bool fireGeometryPair=fireGeometry(vs,ps);
        if (!ctx || (!watches(vs) && !(source && (sourceMesh(vs) || sourceEffect(vs))) && !fireGeometryPair && !f77CandidateDraw)) return;
        const bool candidateOptic=f77CandidateDraw && vs==kF77OpticVs && ps==kF77OpticPs;
        const bool candidateUi=f77CandidateDraw&&vs==kWeaponUiVs&&ps==kWeaponUiPs;
        const bool captureOpticEvidence=candidateOptic && candidateOpticDraws<kCandidateMaxOpticDraws;
        if(fireGeometryPair && fireGeometrySeen<kMaxDraws)++fireGeometrySeen;
        if (draws.size() >= kMaxDraws) { ++dropped; return; }
        Draw d;
        d.frame = frame; d.ordinal = ordinal; d.vs = vs; d.ps = ps;
        d.kind = static_cast<uint8_t>(kind); d.count = count;
        d.instances = instances; d.startInstance = startInstance;
        d.renderFrame=renderFrame;d.eye=eye;
        d.meshSelected=meshSelected?1u:0u;
        d.start=start;d.base=base;if(!firstFrame)firstFrame=frame;
        Microsoft::WRL::ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rt;
        ctx->OMGetRenderTargets(1, &rt, nullptr);
        if (rt) {
            Microsoft::WRL::ComPtr<ID3D11Resource> res; rt->GetResource(&res);
            Texture tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
                d.target = reinterpret_cast<uint64_t>(tex.Get());
                d.width = td.Width; d.height = td.Height;
            }
        }
        ID3D11Buffer* buffers[4] = {};
        ctx->VSGetConstantBuffers(0, 3, buffers);
        ctx->PSGetConstantBuffers(2, 1, buffers + 3);
        if(vs==kNight && ps==kNightPs) {
            if(buffers[1])buffers[1]->Release();buffers[1]=nullptr;
            ctx->PSGetConstantBuffers(1,1,buffers+1);
        }
        for (int i = 0; i < 4; ++i) {
            Buffer src; src.Attach(buffers[i]);
            if (!src) continue;
            d.source[i] = reinterpret_cast<uint64_t>(src.Get());
            D3D11_BUFFER_DESC bd{}; src->GetDesc(&bd);
            d.whole[i] = bd.ByteWidth;
            const uint32_t cap = i == 0 ? 1024u : 8192u;
            bd.ByteWidth = bd.ByteWidth < cap ? bd.ByteWidth : cap;
            bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, &d.stage[i]))) { ++failures; continue; }
            D3D11_BOX box{0, 0, 0, bd.ByteWidth, 1, 1};
            ctx->CopySubresourceRegion(d.stage[i].Get(), 0, 0, 0, 0, src.Get(), 0, &box);
            d.copied[i] = bd.ByteWidth;
        }
        const bool mesh=source && (eyeMesh || ordinal==UINT32_MAX-1) && sourceMesh(vs);
        const bool effect=source && ordinal==UINT32_MAX-2 && sourceEffect(vs);
        const bool sprite=vs==kSprite && ps==kSpritePs;
        const bool night=vs==kNight && ps==kNightPs;
        const bool planet=planetSurface(vs),solar=solarDraw(vs);
        const bool unpacked=effect || night || planet || solar || fireGeometryPair || f77CandidateDraw;
        if(unpacked || sprite || mesh || fireGeometryPair || f77CandidateDraw) {
            Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;ctx->IAGetInputLayout(&layout);
            Layout elements[32];UINT bytes=sizeof(elements);
            if(layout && SUCCEEDED(layout->GetPrivateData(effectLayoutKey(),&bytes,elements)) && bytes &&
                bytes<=sizeof(elements) && bytes%sizeof(Layout)==0)d.layout.assign(elements,elements+bytes/sizeof(Layout));
        }
        if(fireGeometryPair && d.layout.empty() && fireGeometryMissingLayout<kMaxDraws)++fireGeometryMissingLayout;
        if(mesh || f77CandidateDraw) {
            ++meshDraws;
            for(unsigned i=0;i<2;++i) {
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;ctx->VSGetShaderResources(i?38:33,1,&srv);
                if(!srv)continue;
                d.meshView[i]=reinterpret_cast<uint64_t>(srv.Get());
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);
                if(sd.ViewDimension==D3D11_SRV_DIMENSION_BUFFER){d.meshRangeStart[i]=sd.Buffer.FirstElement;d.meshRangeCount[i]=sd.Buffer.NumElements;}
                Microsoft::WRL::ComPtr<ID3D11Resource> res;srv->GetResource(&res);
                d.meshSource[i]=reinterpret_cast<uint64_t>(res.Get());Buffer b;
                if(sd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || FAILED(res.As(&b))){++meshDeclined;d.meshStatus[i]=2;continue;}
                if(sd.Buffer.FirstElement!=0){++meshDeclined;d.meshStatus[i]=3;continue;}
                D3D11_BUFFER_DESC bd{};b->GetDesc(&bd);
                if(bd.StructureByteStride!=(i?48u:336u)){++meshDeclined;d.meshStatus[i]=4;continue;}
                d.meshSource[i]=reinterpret_cast<uint64_t>(b.Get());
                if(eyeMesh&&eye<0){d.meshStatus[i]=10;continue;}
                if(eyeMesh&&!meshSelected){d.meshStatus[i]=8;continue;}
                if(eyeMesh&&!meshSnapshotPermit){d.meshStatus[i]=7;continue;}
                d.mesh[i]=captureMeshBuffer(ctx,dev.Get(),b.Get(),frame,bd.StructureByteStride,eyeMesh?d.target:0,eyeMesh&&!f77CandidateDraw,&d.meshStatus[i]);
            }
            Buffer ids;UINT stride=0,offset=0;ctx->IAGetVertexBuffers(0,1,&ids,&stride,&offset);
            d.meshSource[2]=reinterpret_cast<uint64_t>(ids.Get());
            if(ids){D3D11_BUFFER_DESC idDesc{};ids->GetDesc(&idDesc);d.meshRangeStart[2]=offset;d.meshRangeCount[2]=offset<idDesc.ByteWidth?idDesc.ByteWidth-offset:0;}
            const uint32_t vb0Status=eyeMeshVb0BindingStatus(ids.Get()!=nullptr,stride,offset);
            if(vb0Status==4 || vb0Status==3){d.meshStatus[2]=vb0Status;++meshDeclined;}
            else if(vb0Status==1){if(eyeMesh&&eye<0)d.meshStatus[2]=10;else if(eyeMesh&&!meshSelected)d.meshStatus[2]=8;else if(eyeMesh&&!meshSnapshotPermit)d.meshStatus[2]=7;else d.mesh[2]=captureMeshBuffer(ctx,dev.Get(),ids.Get(),frame,stride,eyeMesh?d.target:0,eyeMesh&&!f77CandidateDraw,&d.meshStatus[2]);}
            if(eyeMesh) {
                if(eye<0)d.meshStatus[3]=10;
                else if(!otherPaletteCandidate)d.meshStatus[3]=9;
                else {
                    D3D11_BUFFER_DESC otherDesc{};otherPaletteCandidate->GetDesc(&otherDesc);
                    d.meshSource[3]=reinterpret_cast<uint64_t>(otherPaletteCandidate);
                    d.meshRangeCount[3]=otherDesc.StructureByteStride?otherDesc.ByteWidth/otherDesc.StructureByteStride:0;
                    if(otherDesc.StructureByteStride!=48)d.meshStatus[3]=4;
                    else if(!meshSnapshotPermit)d.meshStatus[3]=7;
                    else d.mesh[3]=captureMeshBuffer(ctx,dev.Get(),otherPaletteCandidate,frame,48,d.target,true,&d.meshStatus[3]);
                }
            }
        }
        if (vs==kHolo || vs==kHoloGuiFxOff || vs==kSprite || vs==kPanel || vs==kScreen || vs==kVscreen) {
            const bool spriteSource = vs==kSprite && ps==kSpritePs;
            d.texture=captureSurface(ctx,dev.Get(),frame,
                                     (vs==kHolo || vs==kHoloGuiFxOff)?holoSurfaceSlot(ps):vs==kPanel?1:0,
                                     vs==kVscreen,spriteSource?kSpriteTextureBytes:0);
        }
        if(vs==kVscreen && sourceFrame==frame && sourceDepth && d.texture!=UINT32_MAX) {
            D3D11_TEXTURE2D_DESC depth{};sourceDepth->GetDesc(&depth);
            const auto& colour=surfaces[d.texture];
            if(colour.frame==frame && depth.Width==colour.width && depth.Height==colour.height)
                copySurface(ctx,dev.Get(),frame,sourceDepth.Get(),sourceDepthFormat,true);
        }
        // Target labels and vector widgets can move inside their dynamic
        // vertex streams. Preserve each draw, not the first binding of a VS.
        // Three frames and 32 MiB bound this explicit diagnostic's cost.
        if(((vs==kHud || vs==kSprite || vs==kVscreen || solar) && frame-firstFrame<3) || f77CandidateDraw ||
           ((mesh || night || planet) && (f77CandidateDraw || frame==firstFrame)) || effect || fireGeometryPair) {
            ID3D11Buffer* raw[3]{};UINT strides[2]{},offsets[2]{},ibOffset=0;DXGI_FORMAT fmt{};
            ctx->IAGetVertexBuffers(0,2,raw,strides,offsets);ctx->IAGetIndexBuffer(raw+2,&fmt,&ibOffset);
            uint32_t opticMinIndex=0,opticMaxIndex=0;
            bool opticIndexRangeValid=false;
            const bool indexedCandidate=f77CandidateDraw && (kind=='X'||kind=='I') && raw[2] && count &&
                (fmt==DXGI_FORMAT_R16_UINT||fmt==DXGI_FORMAT_R32_UINT);
            if(indexedCandidate) {
                const uint32_t indexStride=fmt==DXGI_FORMAT_R16_UINT?2u:4u;
                const uint64_t indexBegin=uint64_t(ibOffset)+uint64_t(start)*indexStride;
                D3D11_BUFFER_DESC indexDesc{};raw[2]->GetDesc(&indexDesc);
                const uint64_t indexBytes=uint64_t(count)*indexStride;
                if(indexBegin<=indexDesc.ByteWidth && indexBytes<=indexDesc.ByteWidth-indexBegin && indexBytes<=256*1024) {
                    D3D11_BUFFER_DESC stageDesc{};stageDesc.ByteWidth=static_cast<UINT>(indexBytes);
                    stageDesc.Usage=D3D11_USAGE_STAGING;stageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    Buffer probe;
                    if(SUCCEEDED(dev->CreateBuffer(&stageDesc,nullptr,&probe))) {
                        D3D11_BOX box{static_cast<UINT>(indexBegin),0,0,static_cast<UINT>(indexBegin+indexBytes),1,1};
                        ctx->CopySubresourceRegion(probe.Get(),0,0,0,0,raw[2],0,&box);
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        const HRESULT mapHr=ctx->Map(probe.Get(),0,D3D11_MAP_READ,0,&mapped);
                        if(SUCCEEDED(mapHr)) {
                            if(mapped.pData) {
                                opticMinIndex=UINT32_MAX;opticMaxIndex=0;
                                for(uint32_t n=0;n<count;++n) {
                                    uint32_t value=0;
                                    if(indexStride==2)value=reinterpret_cast<const uint16_t*>(mapped.pData)[n];
                                    else value=reinterpret_cast<const uint32_t*>(mapped.pData)[n];
                                    opticMinIndex=(std::min)(opticMinIndex,value);opticMaxIndex=(std::max)(opticMaxIndex,value);
                                }
                                opticIndexRangeValid=true;
                            }
                            ctx->Unmap(probe.Get(),0);
                        }
                    }
                }
            }
            bool copied=false;
            for(int i=0;i<3;++i) {
                Buffer src;src.Attach(raw[i]);if(!src)continue;
                if(unpacked) {
                    if(night && i==1)continue; // exact night VS reads only POSITION in VB0
                    if(i==2 && kind!='X' && kind!='I')continue;
                    if(i<2 && !d.layout.empty()) {
                        bool used=false;for(const auto& e:d.layout)used=used || e.slot==uint32_t(i);
                        if(!used)continue;
                    }
                }
                auto& s=d.streams[i];s.offset=i==2?ibOffset:offsets[i];s.stride=i==2?(fmt==DXGI_FORMAT_R16_UINT?2u:fmt==DXGI_FORMAT_R32_UINT?4u:0u):strides[i];
                D3D11_BUFFER_DESC bd{};src->GetDesc(&bd);s.whole=bd.ByteWidth;
                if(s.offset>=s.whole || !s.stride)continue;
                // Mesh VB0 is already retained in full by the frame-local
                // table. Recopying it for every material starved effect VBs.
                if(mesh && i==0 && d.mesh[2]!=UINT32_MAX)continue;
                // These watched packed families use VB0 for per-instance
                // IDs and VB1 for vertices. Shared buffers can place a
                // six-index sprite megabytes beyond the binding offset.
                // Keep original bindings separately from the copied window.
                const bool indexed=kind=='X'||kind=='I';
                uint64_t begin=s.offset;
                if(i==2 && indexed)begin+=uint64_t(start)*s.stride;
                // Effects use unpacked VB0 vertices / VB1 instances, unlike
                // packed mesh IDs. Keep the bounded binding windows for both
                // streams so no guessed input classification drops a sprite.
                // F77's optic pair is indexed packed geometry. The input
                // layout alone does not make it "unpacked": VB1 is addressed
                // from BaseVertexLocation, which can place the tiny quad far
                // beyond the binding origin. Keep the original binding in
                // s.offset and retain the draw's vertex window separately.
                bool indexedVertexWindow=false;
                if(indexedCandidate && i<2)
                    for(const auto& e:d.layout)indexedVertexWindow=indexedVertexWindow ||
                        (e.slot==uint32_t(i) && e.classification==D3D11_INPUT_PER_VERTEX_DATA);
                uint32_t indexedOpticBytes=0;
                if(indexedVertexWindow) {
                    const int64_t firstVertex=int64_t(base)+opticMinIndex,lastVertex=int64_t(base)+opticMaxIndex;
                    const uint64_t elements=lastVertex>=firstVertex?uint64_t(lastVertex-firstVertex+1):0;
                    const uint64_t span=elements && elements<=UINT64_MAX/s.stride?elements*s.stride:UINT64_MAX;
                    const uint64_t endElement=lastVertex>=0?uint64_t(lastVertex)+1:UINT64_MAX;
                    const bool endInBuffer=endElement<= (UINT64_MAX-offsets[i])/s.stride &&
                        uint64_t(offsets[i])+endElement*s.stride<=s.whole;
                    if(!opticIndexRangeValid || firstVertex<0 || lastVertex<firstVertex || span>256*1024 ||
                       !endInBuffer) {++vertexDeclined;continue;}
                    begin=uint64_t(offsets[i])+uint64_t(firstVertex)*s.stride;
                    indexedOpticBytes=static_cast<uint32_t>(span);
                } else if((!unpacked || indexedVertexWindow) && i==(vs==kVscreen?0:1) && base>0) {
                    begin+=uint64_t(base)*s.stride;
                }
                if(vs==kVscreen && i==1)begin+=uint64_t(startInstance)*s.stride;
                if(begin>=s.whole){++vertexDeclined;continue;}
                s.captureOffset=static_cast<uint32_t>(begin);
                UINT bytes=indexedOpticBytes?indexedOpticBytes:s.whole-s.captureOffset;if(bytes>256*1024)bytes=256*1024;
                if(i==2 && indexed && uint64_t(count)*s.stride<bytes)bytes=count*s.stride;
                if(unpacked && i<2 && !d.layout.empty()) {
                    bool used=false,perInstance=true,knownStep=true;uint64_t instanceElements=0;
                    for(const auto& e:d.layout)if(e.slot==uint32_t(i)) {
                        used=true;perInstance=perInstance && e.classification==D3D11_INPUT_PER_INSTANCE_DATA;
                        knownStep=knownStep && e.step>0;
                        if(e.step) {
                            const uint64_t end=uint64_t(startInstance)+(uint64_t(instances)+e.step-1)/e.step;
                            if(end>instanceElements)instanceElements=end;
                        }
                    }
                    // Keep the original binding origin. Only trim its tail
                    // when the layout/draw proves the last accessed element.
                    uint64_t elements=0;
                    if(used && perInstance && knownStep)elements=instanceElements;
                    else if(used && !perInstance && !indexed)elements=uint64_t(base>0?base:0)+count;
                    if(elements && elements*s.stride<bytes)bytes=static_cast<UINT>(elements*s.stride);
                }
                if(!bytes)continue;
                if(vertexBytes+bytes>32*1024*1024){++vertexDeclined;if(fireGeometryPair)fireGeometryRefusedBytes=bytes>UINT32_MAX-fireGeometryRefusedBytes?UINT32_MAX:fireGeometryRefusedBytes+bytes;continue;}
                bd.ByteWidth=bytes;bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(dev->CreateBuffer(&bd,nullptr,&s.stage))){++failures;if(fireGeometryPair)fireGeometryRefusedBytes=bytes>UINT32_MAX-fireGeometryRefusedBytes?UINT32_MAX:fireGeometryRefusedBytes+bytes;continue;}
                D3D11_BOX box{s.captureOffset,0,0,s.captureOffset+bytes,1,1};ctx->CopySubresourceRegion(s.stage.Get(),0,0,0,0,src.Get(),0,&box);
                s.copied=bytes;vertexBytes+=bytes;copied=true;
            }
            vertexDraws+=copied?1u:0u;
            if(fireGeometryPair) {
                bool position0=false,position1=false;
                for(const auto& e:d.layout)if(std::strcmp(e.semantic,"POSITION")==0 && e.slot<2) {
                    if(e.index==0 && d.streams[e.slot].copied)position0=true;
                    if(e.index==1 && d.streams[e.slot].copied)position1=true;
                }
                if(position0 && position1 && fireGeometryCaptured<kMaxDraws)++fireGeometryCaptured;
            }
        }
        if(sprite && frame==firstFrame && rt) {
            pendingSpriteDiagnostic=captureSpriteDiagnostic(ctx,dev.Get(),d,rt.Get());
        }
        const uint32_t candidateDrawIndex=uint32_t(draws.size());
        if(candidateOptic)++candidateOpticDraws;
        uint32_t candidateRoiAfter=UINT32_MAX;
        if(captureOpticEvidence || candidateUi) {
            captureCandidateSampler(ctx,candidateDrawIndex);
            captureCandidateInputs(ctx,dev.Get(),candidateDrawIndex,candidateUi,frame,vs==kF77OpticVs?2u:0u);
            if(rt)candidateRoiAfter=captureCandidateRoiPair(ctx,dev.Get(),rt.Get(),candidateDrawIndex);
            if(candidateRoiAfter!=UINT32_MAX) {
                pendingCandidateRoi=candidateRoiAfter;
                if(candidateRoiDraw==UINT32_MAX)candidateRoiDraw=candidateDrawIndex;
            }
        }
        draws.push_back(std::move(d));
        if(f77CandidateDraw) {
            CandidateFrame c;c.draw=candidateDrawIndex;c.ledgerFrame=frame;c.renderFrame=renderFrame;
            c.roiCaptured=candidateRoiAfter!=UINT32_MAX?1u:0u;c.cameraCbSlot=vs==kF77OpticVs?1u:0u;
            candidateFrames.push_back(c);pendingCandidateDraw=uint32_t(candidateFrames.size()-1);
        }
        if(((effect && effectImage(vs)) || night) && frame==firstFrame && rt) {
            Microsoft::WRL::ComPtr<ID3D11Resource> res;rt->GetResource(&res);Texture tex;
            D3D11_RENDER_TARGET_VIEW_DESC rd{};rt->GetDesc(&rd);
            if(rd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && rd.Texture2D.MipSlice==0 && SUCCEEDED(res.As(&tex)))
                pendingEffectImage=captureEffectImage(ctx,tex.Get(),rd.Format,uint32_t(draws.size()-1),false);
        }
        if(night && frame==firstFrame)captureNightSampling(ctx,uint32_t(draws.size()-1));
    }

    // Called after the ledger's readback grace period. No waits/flushes:
    // unavailable copies get zero payload and an explicit failure count.
    // On-disk integers are little-endian, individually written (no ABI padding).
    bool write(ID3D11DeviceContext* ctx, const wchar_t* path) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") || !f) return false;
        bool ok = fwrite("EDVRDRW1", 1, 8, f) == 8;
        auto u32 = [&](uint32_t v) { ok = fwrite(&v, 4, 1, f) == 1 && ok; };
        auto u64 = [&](uint64_t v) { ok = fwrite(&v, 8, 1, f) == 1 && ok; };
        u32(15); u32(static_cast<uint32_t>(draws.size())); u32(static_cast<uint32_t>(surfaces.size())); u32(dropped);
        auto payload = [&](ID3D11Resource* resource, uint32_t bytes, uint32_t row, uint32_t height) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const bool mapped = resource && SUCCEEDED(ctx->Map(resource, 0, D3D11_MAP_READ,
                                                 D3D11_MAP_FLAG_DO_NOT_WAIT, &m));
            const bool valid = mapped && m.pData && (!row || m.RowPitch >= row);
            u32(valid ? bytes : 0);
            if (valid) {
                if (!row) ok = fwrite(m.pData, 1, bytes, f) == bytes && ok;
                else for (uint32_t y = 0; y < height; ++y)
                    ok = fwrite(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                                1, row, f) == row && ok;
            } else if (bytes) ++failures;
            if (mapped) ctx->Unmap(resource, 0);
            return valid;
        };
        for (Draw& d : draws) {
            u64(d.vs); u64(d.ps); u64(d.target);
            u32(d.frame); u32(d.ordinal); u32(d.kind); u32(d.count); u32(d.instances); u32(d.startInstance);
            u32(d.renderFrame);u32(static_cast<uint32_t>(d.eye));
            u32(d.meshSelected);
            u32(d.join.sceneFrame);u32(d.join.epoch);u32(d.join.scheduledIndex);u32(d.join.scheduledRenderFrame);
            u32(d.join.fpFrame);u32(d.join.fpSequence);u32(d.join.gGameFrame);u32(d.join.itemSerial);u32(d.join.rigGeneration);
            ok=fwrite(d.join.armJoints,sizeof(d.join.armJoints),1,f)==1&&ok;
            u64(d.join.rig);u64(d.join.poseQpc);u64(d.join.drawQpc);u64(d.join.captureSequence);u32(d.join.validFlags);u32(d.join.sourceJobLookupStatus);
            u32(d.width); u32(d.height); u32(d.texture);
            for (int i = 0; i < 4; ++i) {
                u64(d.source[i]); u32(d.whole[i]);
                payload(d.stage[i].Get(), d.copied[i], 0, 0);
            }
            u32(d.start);u32(static_cast<uint32_t>(d.base));
            for(auto& s:d.streams){u32(s.offset);u32(s.stride);u32(s.whole);u32(s.captureOffset);payload(s.stage.Get(),s.copied,0,0);}
            for(unsigned i=0;i<4;++i){u32(d.mesh[i]);u64(d.meshSource[i]);u64(d.meshView[i]);u32(d.meshRangeStart[i]);u32(d.meshRangeCount[i]);u32(d.meshStatus[i]);}
            u32(uint32_t(d.layout.size()));
            for(const auto& e:d.layout) {
                ok=fwrite(e.semantic,1,64,f)==64 && ok;
                u32(e.index);u32(e.format);u32(e.slot);u32(e.offset);u32(e.classification);u32(e.step);
            }
        }
        for (Surface& s : surfaces) {
            u32(s.frame); u32(s.width); u32(s.height); u32(s.format);
            payload(s.stage.Get(), s.bytes, s.height ? s.bytes / s.height : 0, s.height);
        }
        u32(uint32_t(meshBuffers.size()));u32(meshDeclined);
        for(auto& b:meshBuffers) {
            u32(b.frame);u32(b.firstDraw);u32(b.bytes);u32(b.stride);u64(reinterpret_cast<uint64_t>(b.source.Get()));u64(b.scope);
            u32(payload(b.stage.Get(),b.bytes,0,0)?1u:0u);
        }
        u32(meshCaptureFirstFrame);u32(kEyeMeshFrames);u32(meshCaptureDeclined);u32(meshCaptureSnapshots);u32(meshNonBodyDraws);
        u32(uint32_t(effectImages.size()));u32(effectImageDeclined);
        for(auto& e:effectImages) {
            u32(e.draw);u32(e.after);u32(e.x);u32(e.y);u32(e.width);u32(e.height);
            u32(e.sourceWidth);u32(e.sourceHeight);u32(e.format);
            const uint32_t rows=e.format==DXGI_FORMAT_BC4_UNORM?(e.height+3)/4:e.height;
            payload(e.stage.Get(),e.bytes,e.bytes/rows,rows);
        }
        u32(uint32_t(nightSampling.size()));
        static_assert(sizeof(D3D11_SAMPLER_DESC)==52 && sizeof(D3D11_VIEWPORT)==24,"night sampling disk layout");
        for(const auto& n:nightSampling) {
            u32(n.draw);u32(n.mask);u32(n.viewportCount);
            uint32_t words[26];std::memcpy(words,n.sampler,sizeof(words));for(auto v:words)u32(v);
            uint32_t viewport[6];std::memcpy(viewport,&n.viewport,sizeof(viewport));for(auto v:viewport)u32(v);
        }
        static_assert(sizeof(D3D11_SAMPLER_DESC)==52 && sizeof(D3D11_BLEND_DESC)==264 &&
                      sizeof(D3D11_DEPTH_STENCIL_DESC)==52 && sizeof(D3D11_RASTERIZER_DESC)==40,
                      "sprite diagnostic disk descriptor layout");
        // Version 8: exact E508/63ABD sprite boundary records. Descriptor
        // blobs carry the native D3D11 layouts; their byte lengths are part
        // of the format so a parser can reject a future ABI drift rather than
        // silently treating a different state as equivalent.
        u32(uint32_t(spriteDiagnostics.size()));u32(spriteImageDeclined);
        auto blob=[&](const void* p,uint32_t n){u32(n);if(n)ok=fwrite(p,1,n,f)==n&&ok;};
        for(auto& s:spriteDiagnostics) {
            u32(s.frame);u32(s.draw);u32(s.x);u32(s.y);u32(s.width);u32(s.height);u32(s.sourceWidth);u32(s.sourceHeight);
            u32(s.rtFormat);u32(s.srvFormat);u32(s.srvMostDetailedMip);u32(s.srvMipLevels);u32(s.srvWidth);u32(s.srvHeight);u32(s.srvArraySize);u32(s.srvMipCount);
            u64(s.psB1Identity);u64(s.srvIdentity);u64(s.rtIdentity);u64(s.depthIdentity);u32(s.psB1Whole);u32(s.depthFormat);u32(s.depthResourceFormat);
            u32(s.mesh[0]);u32(s.mesh[1]);
            u32(s.blendFactor[0]);u32(s.blendFactor[1]);u32(s.blendFactor[2]);u32(s.blendFactor[3]);u32(s.sampleMask);u32(s.stencilRef);u32(s.depthFormat);u32(s.rasterFill);u32(s.rasterCull);u32(s.stateMask);
            u32(s.topology);u32(s.dsvFlags);
            u32(s.scissorCount);for(uint32_t i=0;i<s.scissorCount && i<D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;++i){u32(static_cast<uint32_t>(s.scissor[i].left));u32(static_cast<uint32_t>(s.scissor[i].top));u32(static_cast<uint32_t>(s.scissor[i].right));u32(static_cast<uint32_t>(s.scissor[i].bottom));}
            u32(s.viewportCount);for(uint32_t i=0;i<s.viewportCount && i<D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;++i)blob(&s.viewport[i],sizeof(D3D11_VIEWPORT));
            blob(&s.sampler,sizeof(s.sampler));blob(&s.blend,sizeof(s.blend));blob(&s.depth,sizeof(s.depth));blob(&s.raster,sizeof(s.raster));
            u32(uint32_t(s.layout.size()));for(const auto& e:s.layout){blob(e.semantic,64);u32(e.index);u32(e.format);u32(e.slot);u32(e.offset);u32(e.classification);u32(e.step);}
            u32(s.beforeBytes);payload(s.before.Get(),s.beforeBytes,s.height?s.beforeBytes/s.height:0,s.height);
            u32(s.afterBytes);if(s.postPending){u32(0);++failures;}else payload(s.after.Get(),s.afterBytes,s.height?s.afterBytes/s.height:0,s.height);
            u32(s.depthBytes);payload(s.depthImage.Get(),s.depthBytes,s.height?s.depthBytes/s.height:0,s.height);
            payload(s.psB1.Get(),s.psB1Bytes,0,0);
        }
        // Version 9: the armed frame's pool-draw geometry (geoFrame 0 and
        // three empty tables when the cull gate probe did not arm it).
        u32(geoFrame);u32(uint32_t(geoMeshes.size()));u32(geoDeclined);u32(geoDrawsDropped);
        for(auto& m:geoMeshes) {
            u64(m.vb);u32(m.vbSlot);u32(m.vbOffset);u32(m.vbStride);u32(m.vbWhole);
            u64(m.ib);u32(m.ibOffset);u32(m.ibFormat);u32(m.ibWhole);
            u32(m.start);u32(static_cast<uint32_t>(m.base));u32(m.count);u32(m.kind);u32(m.topology);
            u32(m.vCapture);payload(m.vStage.Get(),m.vBytes,0,0);
            u32(m.iCapture);payload(m.iStage.Get(),m.iBytes,0,0);
            u32(uint32_t(m.layout.size()));
            for(const auto& e:m.layout){ok=fwrite(e.semantic,1,64,f)==64 && ok;u32(e.index);u32(e.format);u32(e.slot);u32(e.offset);u32(e.classification);u32(e.step);}
        }
        u32(uint32_t(geoStates.size()));
        for(const auto& s:geoStates) {
            u32(s.flags);blob(&s.blend,sizeof(s.blend));
            uint32_t factor[4];std::memcpy(factor,s.factor,sizeof(factor));for(auto v:factor)u32(v);
            u32(s.sampleMask);blob(&s.depth,sizeof(s.depth));u32(s.stencilRef);blob(&s.raster,sizeof(s.raster));
        }
        u32(uint32_t(geoDraws.size()));
        for(const auto& g:geoDraws) {
            u32(g.frame);u32(g.ordinal);u32(g.mesh);u32(g.state);u64(g.vs);u64(g.ps);u32(g.instances);u32(g.startInstance);
        }
        u32(candidateDropped);u32(candidateMissingRenderFrame);u32(candidateOutsideFrameWindow);u32(candidateInputDeclined);u32(candidateMipDeclined);u32(candidateInputBytes);
        u32(candidateOpticDraws);u32(candidateRoiIncomplete);u32(candidateRoiDraw);
        for(unsigned frameIndex=0;frameIndex<16;++frameIndex)for(unsigned e=0;e<3;++e)u32(candidateUiCounts[frameIndex][e]);
        for(unsigned frameIndex=0;frameIndex<16;++frameIndex)for(unsigned e=0;e<3;++e)u32(candidateUiDropped[frameIndex][e]);
        for(unsigned frameIndex=0;frameIndex<16;++frameIndex)u32(candidateUiInputBytes[frameIndex]);
        u32(uint32_t(candidateFrames.size()));
        for(auto& c:candidateFrames){u32(c.draw);u32(c.ledgerFrame);u32(c.renderFrame);u32(static_cast<uint32_t>(c.eye));u32(c.issued);u32(c.roiCaptured);u32(c.roiComplete);
            u32(c.issueStatus);u64(c.actualVs);u64(c.actualPs);u64(c.b0Identity);u64(c.targetIdentity);u32(c.width);u32(c.height);u32(c.rtvFormat);u32(c.b0FirstConstant);u32(c.b0NumConstants);u32(c.b0RangeKnown);u32(c.cameraCbSlot);blob(&c.rtv,sizeof(c.rtv));u32(c.viewportCount);u32(c.scissorCount);
            if(c.viewportCount)blob(c.viewports,sizeof(D3D11_VIEWPORT)*c.viewportCount);if(c.scissorCount)blob(c.scissors,sizeof(D3D11_RECT)*c.scissorCount);
            u32(c.b0Bytes);payload(c.b0Stage.Get(),c.b0Bytes,0,0);}
        u32(uint32_t(candidateInputs.size()));
        for(auto& c:candidateInputs) {
            u32(c.draw);u32(c.slot);u32(c.kind);u32(c.viewDimension);u32(c.format);
            u32(c.width);u32(c.height);u32(c.sourceWidth);u32(c.sourceHeight);u32(c.sourceMipLevels);u32(c.sourceArraySize);
            u32(c.mip);u32(c.requestedMip);u32(c.mipLevels);u32(c.requestedMipLevels);u32(c.firstSlice);u32(c.slices);
            u32(c.firstElement);u32(c.elements);u32(c.stride);u32(c.rowBytes);u32(c.rows);u32(c.bytes);
            u64(c.sourceResource);u64(c.sourceView);u32(c.sampleMode);u32(c.sourceFormat);
            if(c.kind==0)payload(c.bufferStage.Get(),c.bytes,0,0);
            else for(uint32_t mip=0;mip<c.mipLevels;++mip)for(uint32_t slice=0;slice<c.slices;++slice) {
                const uint32_t sub=D3D11CalcSubresource(mip,slice,c.mipLevels);
                D3D11_MAPPED_SUBRESOURCE m{};
                const bool mapped=c.textureStage && SUCCEEDED(ctx->Map(c.textureStage.Get(),sub,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m));
                uint32_t rowBytes=0,rows=0;const uint32_t width=c.width>>mip?c.width>>mip:1,height=c.height>>mip?c.height>>mip:1;
                const bool layout=candidateTextureLayout(static_cast<DXGI_FORMAT>(c.format),width,height,rowBytes,rows);
                const bool valid=mapped && m.pData && layout && m.RowPitch>=rowBytes;
                const uint32_t subBytes=layout?rowBytes*rows:0;u32(valid?subBytes:0);
                if(valid)for(uint32_t row=0;row<rows;++row)
                    ok=fwrite(static_cast<const uint8_t*>(m.pData)+size_t(row)*m.RowPitch,1,rowBytes,f)==rowBytes&&ok;
                else if(subBytes)++failures;
                if(mapped)ctx->Unmap(c.textureStage.Get(),sub);
            }
        }
        u32(uint32_t(candidateRefusals.size()));
        for(const auto& c:candidateRefusals) {
            u32(c.draw);u32(c.slot);u32(c.kind);u32(c.viewDimension);u32(c.format);
            u32(c.sourceWidth);u32(c.sourceHeight);u32(c.sourceMipLevels);u32(c.sourceArraySize);
            u32(c.requestedMip);u32(c.requestedMipLevels);u32(c.firstSlice);u32(c.slices);u32(c.reason);
            u64(c.sourceResource);u64(c.sourceView);
        }
        u32(uint32_t(candidateSamplers.size()));
        for(const auto& c:candidateSamplers){u32(c.draw);u32(c.present);blob(&c.desc,sizeof(c.desc));}
        u32(failures);
        ok = !ferror(f) && ok;
        return fclose(f) == 0 && ok;
    }

private:
    static bool candidateTextureLayout(DXGI_FORMAT f,uint32_t w,uint32_t h,uint32_t& rowBytes,uint32_t& rows) {
        uint32_t bpp=0;
        switch(f) {
        case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_A8_UNORM:bpp=1;break;
        case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:bpp=2;break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R11G11B10_FLOAT:bpp=4;break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R32G32_FLOAT:bpp=8;break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:bpp=16;break;
        default:break;
        }
        if(bpp){rowBytes=w*bpp;rows=h;return w&&h;}
        switch(f) {
        case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:case DXGI_FORMAT_BC4_UNORM:
            rowBytes=((w+3)/4)*8;rows=(h+3)/4;return w&&h;
        case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC2_UNORM_SRGB:case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:case DXGI_FORMAT_BC5_UNORM:case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:
            rowBytes=((w+3)/4)*16;rows=(h+3)/4;return w&&h;
        default:return false;
        }
    }
    static bool pointSampleUiTexture(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11ShaderResourceView* source,
                                     uint32_t sourceWidth,uint32_t sourceHeight,Texture& staging) {
        if(!ctx||!dev||!source||!sourceWidth||!sourceHeight)return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};source->GetDesc(&srvDesc);
        if(srvDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
           (srvDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&srvDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))return false;
        struct Kernel { Microsoft::WRL::ComPtr<ID3D11Device> device;Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader; };
        static Kernel kernel;static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        if(kernel.device.Get()!=dev || !kernel.shader) {
            kernel.shader.Reset();kernel.device=dev;
            static const char sourceCode[]="Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);"
                "[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x>=384||p.y>=384)return;"
                "uint w,h;src.GetDimensions(w,h);uint2 q=min(uint2(((p.x*2+1)*w)/768,((p.y*2+1)*h)/768),uint2(w-1,h-1));"
                "dst[p.xy]=src.Load(int3(q,0));}";
            using CompileFn=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);
            HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");if(!compiler)return false;
            auto compile=reinterpret_cast<CompileFn>(GetProcAddress(compiler,"D3DCompile"));
            Microsoft::WRL::ComPtr<ID3DBlob> code,errors;
            const HRESULT compiled=compile?compile(sourceCode,sizeof(sourceCode)-1,"EDVR UI point sample",nullptr,nullptr,"main","cs_5_0",0,0,&code,&errors):E_FAIL;
            FreeLibrary(compiler);if(FAILED(compiled)||!code)return false;
            if(FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&kernel.shader)))return false;
        }
        D3D11_TEXTURE2D_DESC outDesc{};outDesc.Width=outDesc.Height=384;outDesc.MipLevels=outDesc.ArraySize=outDesc.SampleDesc.Count=1;
        outDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;outDesc.Usage=D3D11_USAGE_DEFAULT;outDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        Texture sampled;if(FAILED(dev->CreateTexture2D(&outDesc,nullptr,&sampled)))return false;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;if(FAILED(dev->CreateUnorderedAccessView(sampled.Get(),nullptr,&uav)))return false;
        D3D11_TEXTURE2D_DESC stageDesc=outDesc;stageDesc.Usage=D3D11_USAGE_STAGING;stageDesc.BindFlags=0;stageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateTexture2D(&stageDesc,nullptr,&staging)))return false;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> oldShader;ID3D11ClassInstance* classes[256]{};UINT classCount=256;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> oldSrv;Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> oldUav;
        ctx->CSGetShader(&oldShader,classes,&classCount);ctx->CSGetShaderResources(0,1,&oldSrv);ctx->CSGetUnorderedAccessViews(0,1,&oldUav);
        {
            VrWorldInternalScope internal;
            ID3D11ShaderResourceView* input=source;ID3D11UnorderedAccessView* output=uav.Get();UINT initialCount=UINT_MAX;
            ctx->CSSetShaderResources(0,1,&input);ctx->CSSetUnorderedAccessViews(0,1,&output,&initialCount);
            ctx->CSSetShader(kernel.shader.Get(),nullptr,0);ctx->Dispatch(48,48,1);
            ID3D11ShaderResourceView* noneSrv=nullptr;ID3D11UnorderedAccessView* noneUav=nullptr;
            ctx->CSSetShaderResources(0,1,&noneSrv);ctx->CSSetUnorderedAccessViews(0,1,&noneUav,nullptr);
            ctx->CSSetShader(oldShader.Get(),classes,classCount);ctx->CSSetShaderResources(0,1,oldSrv.GetAddressOf());
            ctx->CSSetUnorderedAccessViews(0,1,oldUav.GetAddressOf(),nullptr);
            ctx->CopyResource(staging.Get(),sampled.Get());
        }
        for(UINT i=0;i<classCount;++i)if(classes[i])classes[i]->Release();
        return true;
    }
public:
    static bool pointSampleUiTextureForTest(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11ShaderResourceView* source,
                                             uint32_t sourceWidth,uint32_t sourceHeight,ID3D11Texture2D** output) {
        if(!output)return false;*output=nullptr;Texture staging;
        if(!pointSampleUiTexture(ctx,dev,source,sourceWidth,sourceHeight,staging))return false;
        *output=staging.Detach();return true;
    }
private:
    void captureCandidateInputs(ID3D11DeviceContext* ctx,ID3D11Device* dev,uint32_t draw,bool weaponUi=false,uint32_t uiFrame=UINT32_MAX,uint32_t uiTextureSlot=0) {
        // Preserve the material that selects the optic before optional
        // alpha/colour arrays can consume the bounded input budget.
        const UINT slots[4]={weaponUi?uiTextureSlot:2u,1u,3u,weaponUi?(uiTextureSlot==0?2u:0u):0u};
        const uint32_t slotCount=weaponUi?4u:3u;
        const uint32_t drawLimit=weaponUi?1024u*1024u:20u*1024u*1024u;
        const uint32_t uiFrameIndex=weaponUi&&candidateUiFirstLedgerFrame&&uiFrame>=candidateUiFirstLedgerFrame&&uiFrame-candidateUiFirstLedgerFrame<16
            ?uiFrame-candidateUiFirstLedgerFrame:UINT32_MAX;
        uint32_t drawBytes=0;
        for(UINT slotIndex=0;slotIndex<slotCount;++slotIndex) {
            const UINT slot=slots[slotIndex];
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;ctx->PSGetShaderResources(slot,1,&srv);
            if(!srv){++candidateInputDeclined;continue;}
            CandidateInput input;input.draw=draw;input.slot=slot;
            input.sourceView=reinterpret_cast<uint64_t>(srv.Get());
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);input.viewDimension=sd.ViewDimension;input.format=sd.Format;
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;srv->GetResource(&resource);
            if(!resource){++candidateInputDeclined;continue;}
            input.sourceResource=reinterpret_cast<uint64_t>(resource.Get());
            if(sd.ViewDimension==D3D11_SRV_DIMENSION_BUFFER || sd.ViewDimension==D3D11_SRV_DIMENSION_BUFFEREX) {
                Microsoft::WRL::ComPtr<ID3D11Buffer> source;if(FAILED(resource.As(&source))){++candidateInputDeclined;continue;}
                D3D11_BUFFER_DESC bd{};source->GetDesc(&bd);input.kind=0;input.width=bd.ByteWidth;input.sourceWidth=bd.ByteWidth;
                if(sd.ViewDimension==D3D11_SRV_DIMENSION_BUFFER){input.firstElement=sd.Buffer.FirstElement;input.elements=sd.Buffer.NumElements;}
                else {input.firstElement=sd.BufferEx.FirstElement;input.elements=sd.BufferEx.NumElements;}
                input.stride=bd.StructureByteStride;input.bytes=bd.ByteWidth;
                if(!input.bytes || input.bytes>drawLimit || input.bytes>drawLimit-drawBytes || input.bytes>kCandidateInputBudget-candidateInputBytes ||
                   input.bytes>kCandidateOtherInputBudget-candidateOtherInputBytes) {++candidateInputDeclined;continue;}
                D3D11_BUFFER_DESC stage=bd;stage.Usage=D3D11_USAGE_STAGING;stage.BindFlags=stage.MiscFlags=stage.StructureByteStride=0;stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(dev->CreateBuffer(&stage,nullptr,&input.bufferStage))){++failures;++candidateInputDeclined;continue;}
                ctx->CopyResource(input.bufferStage.Get(),source.Get());
            } else if(sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D || sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
                Texture source;if(FAILED(resource.As(&source))){++candidateInputDeclined;continue;}
                D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);input.kind=1;input.sourceFormat=sd.Format;input.format=sd.Format;
                input.mip=sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?sd.Texture2D.MostDetailedMip:sd.Texture2DArray.MostDetailedMip;
                input.requestedMip=input.mip;
                const uint32_t viewMips=sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?sd.Texture2D.MipLevels:sd.Texture2DArray.MipLevels;
                input.firstSlice=sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?0:sd.Texture2DArray.FirstArraySlice;
                input.slices=sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?1:(slot==3?1:sd.Texture2DArray.ArraySize);
                if(input.mip>=td.MipLevels || !input.slices || input.firstSlice+input.slices>td.ArraySize || td.SampleDesc.Count!=1) {++candidateInputDeclined;continue;}
                input.sourceWidth=td.Width;input.sourceHeight=td.Height;input.sourceMipLevels=td.MipLevels;input.sourceArraySize=td.ArraySize;
                const uint32_t remainingMips=td.MipLevels-input.mip;
                input.requestedMipLevels=(!viewMips || viewMips==UINT_MAX || viewMips>remainingMips)?remainingMips:viewMips;
                input.width=td.Width>>input.mip;if(!input.width)input.width=1;
                input.height=td.Height>>input.mip;if(!input.height)input.height=1;
                if(!candidateTextureLayout(static_cast<DXGI_FORMAT>(sd.Format),input.width,input.height,input.rowBytes,input.rows)) {
                    ++candidateInputDeclined;
                    candidateRefusals.push_back({draw,slot,1,static_cast<uint32_t>(sd.ViewDimension),static_cast<uint32_t>(sd.Format),input.sourceWidth,input.sourceHeight,
                        input.sourceMipLevels,input.sourceArraySize,input.requestedMip,input.requestedMipLevels,input.firstSlice,input.slices,3,input.sourceResource,input.sourceView});
                    continue;
                }
                const bool uiImage=weaponUi&&slot==uiTextureSlot&&uiFrameIndex<16;
                const uint32_t frameImageRemaining=uiImage?(candidateUiInputBytes[uiFrameIndex]<kCandidateUiInputPerFrameBudget?kCandidateUiInputPerFrameBudget-candidateUiInputBytes[uiFrameIndex]:0):UINT32_MAX;
                const uint32_t otherRemaining=candidateOtherInputBytes<kCandidateOtherInputBudget?kCandidateOtherInputBudget-candidateOtherInputBytes:0;
                const uint32_t available=(std::min)((std::min)(drawLimit-drawBytes,kCandidateInputBudget-candidateInputBytes),uiImage?frameImageRemaining:otherRemaining);
                uint64_t bytes=0;uint32_t skippedMips=0;
                bool pointSample=false;
                if(uiImage&&(input.width>kCandidateUiTextureEdge||input.height>kCandidateUiTextureEdge)) {
                    uint32_t offset=0;
                    while(offset<input.requestedMipLevels&&(input.width>>offset>kCandidateUiTextureEdge||input.height>>offset>kCandidateUiTextureEdge))++offset;
                    if(offset<input.requestedMipLevels) {
                        input.mip+=offset;input.width=input.width>>offset?input.width>>offset:1;input.height=input.height>>offset?input.height>>offset:1;
                        input.mipLevels=1;input.slices=1;input.sampleMode=1;skippedMips=offset;
                        candidateTextureLayout(static_cast<DXGI_FORMAT>(sd.Format),input.width,input.height,input.rowBytes,input.rows);
                        bytes=uint64_t(input.rowBytes)*input.rows;
                    } else if(sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM||sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                        pointSample=true;input.width=input.height=kCandidateUiTextureEdge;input.format=DXGI_FORMAT_R8G8B8A8_UNORM;
                        input.mipLevels=input.slices=1;input.sampleMode=2;candidateTextureLayout(DXGI_FORMAT_R8G8B8A8_UNORM,input.width,input.height,input.rowBytes,input.rows);
                        bytes=uint64_t(input.rowBytes)*input.rows;
                    }
                }
                if(!bytes) for(;skippedMips<input.requestedMipLevels;++skippedMips) {
                    uint64_t chainBytes=0;uint32_t levels=0;
                    for(uint32_t mip=skippedMips;mip<input.requestedMipLevels;++mip) {
                        uint32_t rowBytes=0,rows=0;
                        const uint32_t w=input.width>>mip?input.width>>mip:1;
                        const uint32_t h=input.height>>mip?input.height>>mip:1;
                        if(!candidateTextureLayout(static_cast<DXGI_FORMAT>(sd.Format),w,h,rowBytes,rows))break;
                        const uint64_t mipBytes=uint64_t(rowBytes)*rows*input.slices;
                        if(!mipBytes || mipBytes>available-chainBytes)break;
                        chainBytes+=mipBytes;++levels;
                    }
                    if(levels){input.mip+=skippedMips;input.width=input.width>>skippedMips?input.width>>skippedMips:1;
                        input.height=input.height>>skippedMips?input.height>>skippedMips:1;input.mipLevels=levels;bytes=chainBytes;
                        candidateTextureLayout(static_cast<DXGI_FORMAT>(sd.Format),input.width,input.height,input.rowBytes,input.rows);break;}
                }
                candidateMipDeclined+=skippedMips+(input.requestedMipLevels-skippedMips-input.mipLevels);
                if(!bytes || bytes>UINT32_MAX) {
                    ++candidateInputDeclined;
                    candidateRefusals.push_back({draw,slot,1,static_cast<uint32_t>(sd.ViewDimension),static_cast<uint32_t>(sd.Format),input.sourceWidth,input.sourceHeight,
                        input.sourceMipLevels,input.sourceArraySize,input.requestedMip,input.requestedMipLevels,input.firstSlice,input.slices,uiImage?4u:1u,input.sourceResource,input.sourceView});
                    continue;
                }
                input.bytes=static_cast<uint32_t>(bytes);
                if(bytes>available) {++candidateInputDeclined;candidateRefusals.push_back({draw,slot,1,static_cast<uint32_t>(sd.ViewDimension),static_cast<uint32_t>(sd.Format),input.sourceWidth,input.sourceHeight,
                    input.sourceMipLevels,input.sourceArraySize,input.requestedMip,input.requestedMipLevels,input.firstSlice,input.slices,1,input.sourceResource,input.sourceView});continue;}
                if(pointSample) {
                    if(!pointSampleUiTexture(ctx,dev,srv.Get(),td.Width>>input.requestedMip,td.Height>>input.requestedMip,input.textureStage)) {++candidateInputDeclined;
                        candidateRefusals.push_back({draw,slot,1,static_cast<uint32_t>(sd.ViewDimension),static_cast<uint32_t>(sd.Format),input.sourceWidth,input.sourceHeight,
                            input.sourceMipLevels,input.sourceArraySize,input.requestedMip,input.requestedMipLevels,input.firstSlice,input.slices,4,input.sourceResource,input.sourceView});continue;}
                } else {
                    D3D11_TEXTURE2D_DESC stage{};stage.Width=input.width;stage.Height=input.height;stage.MipLevels=input.mipLevels;stage.ArraySize=input.slices;
                    stage.Format=static_cast<DXGI_FORMAT>(input.format);stage.SampleDesc.Count=1;stage.Usage=D3D11_USAGE_STAGING;stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    if(FAILED(dev->CreateTexture2D(&stage,nullptr,&input.textureStage))){++failures;++candidateInputDeclined;
                        candidateRefusals.push_back({draw,slot,1,static_cast<uint32_t>(sd.ViewDimension),static_cast<uint32_t>(sd.Format),input.sourceWidth,input.sourceHeight,
                            input.sourceMipLevels,input.sourceArraySize,input.requestedMip,input.requestedMipLevels,input.firstSlice,input.slices,2,input.sourceResource,input.sourceView});continue;}
                    for(uint32_t mip=0;mip<input.mipLevels;++mip)for(uint32_t slice=0;slice<input.slices;++slice) {
                        const uint32_t srcSub=D3D11CalcSubresource(input.mip+mip,input.firstSlice+slice,td.MipLevels);
                        const uint32_t dstSub=D3D11CalcSubresource(mip,slice,input.mipLevels);
                        ctx->CopySubresourceRegion(input.textureStage.Get(),dstSub,0,0,0,source.Get(),srcSub,nullptr);
                    }
                }
            } else {++candidateInputDeclined;continue;}
            candidateInputBytes+=input.bytes;drawBytes+=input.bytes;
            if(weaponUi&&slot==uiTextureSlot&&uiFrameIndex<16)candidateUiInputBytes[uiFrameIndex]+=input.bytes;else candidateOtherInputBytes+=input.bytes;
            candidateInputs.push_back(std::move(input));
        }
    }
    void captureCandidateSampler(ID3D11DeviceContext* ctx,uint32_t draw) {
        CandidateSampler record;record.draw=draw;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;ctx->PSGetSamplers(0,1,&sampler);
        if(sampler){record.present=1;sampler->GetDesc(&record.desc);}
        candidateSamplers.push_back(record);
    }
    void captureNightSampling(ID3D11DeviceContext* ctx,uint32_t draw) {
        if(nightSampling.size()>=8){++effectImageDeclined;return;}
        NightSampling n;n.draw=draw;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        n.viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&n.viewportCount,viewports);if(n.viewportCount==1)n.viewport=viewports[0];
        for(UINT slot=0;slot<2;++slot) {
            Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;ctx->PSGetSamplers(slot,1,&sampler);
            if(sampler){n.mask|=1u<<slot;sampler->GetDesc(n.sampler+slot);}
        }
        nightSampling.push_back(n);
        for(UINT slot=0;slot<5;++slot) {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;ctx->PSGetShaderResources(slot,1,&srv);
            if(!srv){++effectImageDeclined;continue;}
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);
            Microsoft::WRL::ComPtr<ID3D11Resource> res;srv->GetResource(&res);Texture tex;
            if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || sd.Texture2D.MostDetailedMip!=0 || FAILED(res.As(&tex))) {
                ++effectImageDeclined;continue;
            }
            // Copy each eye at this draw, even when it reuses a resource.
            // Surface deduplication would silently retain the first eye.
            captureEffectImage(ctx,tex.Get(),sd.Format,draw,2+slot);
        }
    }
    uint32_t captureEffectImage(ID3D11DeviceContext* ctx,ID3D11Texture2D* tex,DXGI_FORMAT format,uint32_t draw,uint32_t after) {
        D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
        uint32_t bpp=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:
            (format==DXGI_FORMAT_R11G11B10_FLOAT || format==DXGI_FORMAT_R8G8B8A8_UNORM ||
             format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)?4:0;
        const bool input=after>=2,bc4=input && format==DXGI_FORMAT_BC4_UNORM;
        if(input && (format==DXGI_FORMAT_R32_FLOAT || format==DXGI_FORMAT_R10G10B10A2_UNORM))bpp=4;
        EffectImage e;e.source=tex;e.draw=draw;e.after=after;e.format=format;
        e.sourceWidth=td.Width;e.sourceHeight=td.Height;
        // Native pixels at the lower right, where the reported rifle fleck
        // lies. Store the crop origin; never pass this off as a whole image.
        e.width=td.Width<1024?td.Width:1024;e.height=td.Height<1024?td.Height:1024;
        e.x=td.Width-e.width;e.y=td.Height-e.height;e.bytes=e.width*e.height*bpp;
        // Night vision is a stereo scene pass. Its terrain is near the
        // centre of the view; keep the rifle's lower-right crop unchanged.
        if(draw<draws.size() && (draws[draw].vs==kNight ||
           (draws[draw].vs==kF77OpticVs && draws[draw].ps==kF77OpticPs))) {
            e.x=(td.Width-e.width)/2;e.y=(td.Height-e.height)/3;
        }
        if(bc4){e.x&=~3u;e.y&=~3u;e.bytes=((e.width+3)/4)*((e.height+3)/4)*8;}
        if((!bpp && !bc4) || !e.bytes || td.SampleDesc.Count!=1 || td.ArraySize!=1 ||
           effectImages.size()>=32 || e.bytes>effectImageBudgetLimit-effectImageBytes) {
            ++effectImageDeclined;return UINT32_MAX;
        }
        td.Width=e.width;td.Height=e.height;td.MipLevels=1;td.Usage=D3D11_USAGE_STAGING;
        td.Format=format; // retain the actual typed SRV/RTV interpretation
        td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&e.stage))){++failures;return UINT32_MAX;}
        D3D11_BOX box{e.x,e.y,0,e.x+e.width,e.y+e.height,1};
        ctx->CopySubresourceRegion(e.stage.Get(),0,0,0,0,tex,0,&box);
        effectImageBytes+=e.bytes;effectImages.push_back(std::move(e));return uint32_t(effectImages.size()-1);
    }
    uint32_t captureCandidateRoiPair(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11RenderTargetView* rtv,uint32_t draw) {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;rtv->GetResource(&resource);Texture source;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};rtv->GetDesc(&rd);
        if(rd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice!=0 || !resource || FAILED(resource.As(&source))) {
            ++effectImageDeclined;return UINT32_MAX;
        }
        D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);
        const DXGI_FORMAT format=rd.Format;
        const uint32_t bpp=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8u:
            (format==DXGI_FORMAT_R8G8B8A8_UNORM || format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format==DXGI_FORMAT_R11G11B10_FLOAT)?4u:0u;
        const uint32_t width=td.Width<kCandidateRoiEdge?td.Width:kCandidateRoiEdge;
        const uint32_t height=td.Height<kCandidateRoiEdge?td.Height:kCandidateRoiEdge;
        const uint32_t bytes=width*height*bpp;
        if(!bpp || !width || !height || td.ArraySize!=1 || td.SampleDesc.Count!=1 ||
           effectImages.size()+2>32 || uint64_t(bytes)*2>effectImageBudgetLimit-effectImageBytes) {
            ++effectImageDeclined;return UINT32_MAX;
        }
        D3D11_TEXTURE2D_DESC stageDesc=td;stageDesc.Width=width;stageDesc.Height=height;stageDesc.MipLevels=1;
        stageDesc.Usage=D3D11_USAGE_STAGING;stageDesc.BindFlags=stageDesc.MiscFlags=0;stageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        EffectImage before,after;before.source=after.source=source;before.draw=after.draw=draw;
        before.after=0;after.after=1;before.x=after.x=(td.Width-width)/2;before.y=after.y=(td.Height-height)/2;
        before.width=after.width=width;before.height=after.height=height;
        before.sourceWidth=after.sourceWidth=td.Width;before.sourceHeight=after.sourceHeight=td.Height;
        before.format=after.format=format;before.bytes=after.bytes=bytes;
        if(FAILED(dev->CreateTexture2D(&stageDesc,nullptr,&before.stage)) || FAILED(dev->CreateTexture2D(&stageDesc,nullptr,&after.stage))) {
            ++failures;return UINT32_MAX;
        }
        D3D11_BOX box{before.x,before.y,0,before.x+width,before.y+height,1};
        ctx->CopySubresourceRegion(before.stage.Get(),0,0,0,0,source.Get(),0,&box);
        effectImages.push_back(std::move(before));effectImages.push_back(std::move(after));effectImageBytes+=bytes*2;
        return uint32_t(effectImages.size()-1);
    }
    static std::mutex& shaderMutex() { static std::mutex m; return m; }
    static std::map<uint64_t, std::vector<uint8_t>>& shaderBytes() {
        static std::map<uint64_t, std::vector<uint8_t>> s; return s;
    }
    uint32_t captureSurface(ID3D11DeviceContext* ctx, ID3D11Device* dev, uint32_t frame,
                            UINT slot,bool large=false,uint32_t perTextureCap=0) {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ctx->PSGetShaderResources(slot, 1, &srv);
        if (!srv) return UINT32_MAX;
        Microsoft::WRL::ComPtr<ID3D11Resource> res; srv->GetResource(&res);
        Texture tex;
        if (FAILED(res.As(&tex))) { ++failures; return UINT32_MAX; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; srv->GetDesc(&sd);
        if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || sd.Texture2D.MostDetailedMip!=0) {++failures;return UINT32_MAX;}
        return copySurface(ctx,dev,frame,tex.Get(),sd.Format,large,perTextureCap);
    }
    uint32_t copySurface(ID3D11DeviceContext* ctx,ID3D11Device* dev,uint32_t frame,
                         ID3D11Texture2D* tex,DXGI_FORMAT format,bool large,
                         uint32_t perTextureCap=0) {
        for (uint32_t i = 0; i < surfaces.size(); ++i)
            if (surfaces[i].source.Get() == tex) return i;
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        uint32_t bpp = 0;
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: bpp = 4; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: bpp = 8; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:bpp=large?8:0;break;
        case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_D24_UNORM_S8_UINT:bpp=large?4:0;break;
        case DXGI_FORMAT_D16_UNORM:bpp=large?2:0;break;
        default: break;
        }
        const uint64_t bytes = static_cast<uint64_t>(td.Width) * td.Height * bpp;
        const uint64_t textureCap=perTextureCap?perTextureCap:(large?kVscreenTextureBytes:kMaxTextureBytes);
        if (!bpp || td.SampleDesc.Count != 1 || td.ArraySize != 1 ||
            bytes > textureCap ||
            bytes + textureBytes > (large?kVscreenTotalBytes:kTotalTextureBytes) || surfaces.size() >= kMaxTextures) {
            ++failures; return UINT32_MAX;
        }
        Surface s; s.source = tex; s.frame = frame; s.width = td.Width; s.height = td.Height;
        s.format = format; s.bytes = static_cast<uint32_t>(bytes);
        td.MipLevels = 1; td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = td.MiscFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &s.stage))) { ++failures; return UINT32_MAX; }
        ctx->CopySubresourceRegion(s.stage.Get(), 0, 0, 0, 0, tex, 0, nullptr);
        textureBytes += s.bytes;
        surfaces.push_back(std::move(s));
        return static_cast<uint32_t>(surfaces.size() - 1);
    }
};
} // namespace edvr
