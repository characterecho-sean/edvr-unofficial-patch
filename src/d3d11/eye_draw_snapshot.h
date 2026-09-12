#pragma once

// Draw-time evidence for an explicitly armed eye run. Celestial draws use
// cb0[4..7] for clip position, outside the instance pool; dim holo strokes
// need the original surface alpha to distinguish them from the background.
// Keep each draw (including both eyes and repeated VS hashes). Buffer
// addresses identify bindings within a run, never objects across frames.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <utility>
#include <map>
#include <mutex>
#include <cstring>
#include "holo_material.h"

namespace edvr {
class EyeDrawSnapshot {
    using Buffer = Microsoft::WRL::ComPtr<ID3D11Buffer>;
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
public:
    static constexpr uint32_t kMaxDraws = 4096;
    static constexpr uint32_t kMaxTextures = 24;
    static constexpr uint32_t kMaxTextureBytes = 16 * 1024 * 1024;
    static constexpr uint32_t kTotalTextureBytes = 64 * 1024 * 1024;
    static constexpr uint32_t kVscreenTextureBytes = 128 * 1024 * 1024;
    static constexpr uint32_t kVscreenTotalBytes = 256 * 1024 * 1024;
    static constexpr uint64_t kHolo = 0x81216C77F90DEDD6ull;
    static constexpr uint64_t kHud = 0xB7790CBFC6554097ull, kSprite=0xE508648660A352B2ull;
    static constexpr uint64_t kPanel=0xA888D51024D9798Eull,kScreen=0x4EF6DDB075A927FAull;
    static constexpr uint64_t kVscreen=0x5C36AF051B98B9F1ull,kVscreenPs=0xCFE84157BC76E921ull;
    static constexpr uint64_t kScene=0x4435F2E50020E7F3ull;
    static constexpr uint64_t kNight=0xFCF7BD2896751D96ull,kNightPs=0xF786D34B5E118D5Eull;
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
    static void rememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* e,UINT n,uint64_t vs) {
        if((!sourceEffect(vs) && !sourceMesh(vs) && vs!=kNight) || !layout || !e || !n || n>32)return;
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
        case 0x88DCF1164C640EC3ull:return true;
        default:return false;
        }
    }
    static bool watches(uint64_t vs) {
        switch (vs) {
        case kHolo:
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
        case 0xB12F7A618E1BDE98ull:
        case 0x203DF51758AADC4Dull:
        case 0xC7FA0C0F5DD49180ull: // six 8194-vertex instances beside the bodies
        case 0xA47A3315FFF5E2E4ull: // line candidate; identity is not yet proven
            return true;
        default: return false;
        }
    }
    struct Draw {
        uint64_t vs = 0, ps = 0, target = 0;
        uint32_t frame = 0, ordinal = 0, kind = 0, count = 0, instances = 0, startInstance = 0;
        uint32_t width = 0, height = 0, texture = UINT32_MAX;
        uint64_t source[4] = {}; // VS b0,b1,b2; PS b2 (night vision: PS b1 replaces unused VS b1)
        uint32_t whole[4] = {}, copied[4] = {};
        Buffer stage[4];
        uint32_t start=0;int32_t base=0;
        struct Stream {uint32_t offset=0,stride=0,whole=0,copied=0,captureOffset=0;Buffer stage;};
        Stream streams[3]; // VB0 instance IDs, VB1 packed vertices, index buffer
        uint32_t mesh[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX}; // t33, t38, VB0
        std::vector<Layout> layout; // source effects; version 7 also night vision
    };
    struct MeshBuffer {
        Buffer source,stage;
        uint32_t frame=0,firstDraw=0,bytes=0,stride=0;
        uint64_t scope=0; // separate eye targets may reuse and rewrite a pool
    };
    std::vector<MeshBuffer> meshBuffers;
    uint32_t meshBytes=0,meshDeclined=0,meshDraws=0;
    static constexpr uint32_t kMeshBudget=256*1024*1024;
    struct Surface {
        Texture source, stage;
        uint32_t frame = 0, width = 0, height = 0, format = 0, bytes = 0;
    };
    std::vector<Draw> draws;
    std::vector<Surface> surfaces;
    uint32_t dropped = 0, failures = 0, textureBytes = 0;
    uint32_t firstFrame=0,vertexBytes=0,vertexDraws=0,vertexDeclined=0;
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
    struct NightSampling {
        uint32_t draw=0,mask=0,viewportCount=0;
        D3D11_SAMPLER_DESC sampler[2]{};
        D3D11_VIEWPORT viewport{};
    };
    std::vector<NightSampling> nightSampling;

    void captureEffectEnd(ID3D11DeviceContext* ctx) {
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
        if ((!watches(hash) && !sourceMesh(hash) && !sourceEffect(hash) && hash!=kVscreenPs && hash!=kNightPs) || !bytes || !size || size > 256*1024) return;
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
            const uint64_t pixel=d.vs==kVscreen?kVscreenPs:(d.vs==kNight && d.ps==kNightPs?kNightPs:0);
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
        effectImages.clear();effectImageBytes=effectImageDeclined=0;pendingEffectImage=UINT32_MAX;
        nightSampling.clear();
        meshBuffers.clear();meshBytes=meshDeclined=meshDraws=0;
        sourceDepth.Reset();sourceDepthFormat=DXGI_FORMAT_UNKNOWN;sourceFrame=0;
    }

    uint32_t captureMeshBuffer(ID3D11DeviceContext* ctx,ID3D11Device* dev,
                               ID3D11Buffer* src,uint32_t frame,uint32_t stride,uint64_t scope=0) {
        if(!src)return UINT32_MAX;
        for(uint32_t i=0;i<meshBuffers.size();++i)
            if(meshBuffers[i].frame==frame && meshBuffers[i].scope==scope && meshBuffers[i].source.Get()==src)return i;
        D3D11_BUFFER_DESC bd{};src->GetDesc(&bd);
        // Full palette, not the older ledger's first MiB: the weapon may
        // address bones beyond that prefix. Record the first draw that saw
        // each resource this frame; deduplication is explicitly visible.
        if(!bd.ByteWidth || bd.ByteWidth>16*1024*1024 || bd.ByteWidth>kMeshBudget-meshBytes){++meshDeclined;return UINT32_MAX;}
        MeshBuffer b;b.source=src;b.frame=frame;b.firstDraw=uint32_t(draws.size());b.bytes=bd.ByteWidth;b.stride=stride;b.scope=scope;
        bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&b.stage))){++failures;return UINT32_MAX;}
        ctx->CopyResource(b.stage.Get(),src);meshBytes+=b.bytes;
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
    void captureEyeMesh(ID3D11DeviceContext* ctx,uint32_t frame,uint32_t ordinal,uint64_t vs,uint64_t ps,
                        char kind,uint32_t count,uint32_t instances,uint32_t startInstance,uint32_t start,int32_t base) {
        if(!sourceMesh(vs) || (firstFrame && (frame<firstFrame || frame-firstFrame>=3)))return;
        capture(ctx,frame,ordinal,vs,ps,kind,count,instances,startInstance,start,base,true,true);
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

    void capture(ID3D11DeviceContext* ctx, uint32_t frame, uint32_t ordinal,
                 uint64_t vs, uint64_t ps, char kind, uint32_t count,
                 uint32_t instances, uint32_t startInstance,uint32_t start=0,int32_t base=0,bool source=false,bool eyeMesh=false) {
        if (!ctx || (!watches(vs) && !(source && (sourceMesh(vs) || sourceEffect(vs))))) return;
        if (draws.size() >= kMaxDraws) { ++dropped; return; }
        Draw d;
        d.frame = frame; d.ordinal = ordinal; d.vs = vs; d.ps = ps;
        d.kind = static_cast<uint8_t>(kind); d.count = count;
        d.instances = instances; d.startInstance = startInstance;
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
        const bool night=vs==kNight && ps==kNightPs;
        const bool unpacked=effect || night;
        if(unpacked || mesh) {
            Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;ctx->IAGetInputLayout(&layout);
            Layout elements[32];UINT bytes=sizeof(elements);
            if(layout && SUCCEEDED(layout->GetPrivateData(effectLayoutKey(),&bytes,elements)) && bytes &&
                bytes<=sizeof(elements) && bytes%sizeof(Layout)==0)d.layout.assign(elements,elements+bytes/sizeof(Layout));
        }
        if(mesh) {
            ++meshDraws;
            for(unsigned i=0;i<2;++i) {
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;ctx->VSGetShaderResources(i?38:33,1,&srv);
                if(!srv)continue;
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);
                Microsoft::WRL::ComPtr<ID3D11Resource> res;srv->GetResource(&res);Buffer b;
                if(sd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || sd.Buffer.FirstElement!=0 || FAILED(res.As(&b))){++meshDeclined;continue;}
                D3D11_BUFFER_DESC bd{};b->GetDesc(&bd);
                if(bd.StructureByteStride!=(i?48u:336u)){++meshDeclined;continue;}
                d.mesh[i]=captureMeshBuffer(ctx,dev.Get(),b.Get(),frame,bd.StructureByteStride,eyeMesh?d.target:0);
            }
            Buffer ids;UINT stride=0,offset=0;ctx->IAGetVertexBuffers(0,1,&ids,&stride,&offset);
            if(stride==8 && offset==0)d.mesh[2]=captureMeshBuffer(ctx,dev.Get(),ids.Get(),frame,stride,eyeMesh?d.target:0);
            else ++meshDeclined;
        }
        if (vs==kHolo || vs==kSprite || vs==kPanel || vs==kScreen || vs==kVscreen)
            d.texture=captureSurface(ctx,dev.Get(),frame,vs==kHolo?holoSurfaceSlot(ps):vs==kPanel?1:0,vs==kVscreen);
        if(vs==kVscreen && sourceFrame==frame && sourceDepth && d.texture!=UINT32_MAX) {
            D3D11_TEXTURE2D_DESC depth{};sourceDepth->GetDesc(&depth);
            const auto& colour=surfaces[d.texture];
            if(colour.frame==frame && depth.Width==colour.width && depth.Height==colour.height)
                copySurface(ctx,dev.Get(),frame,sourceDepth.Get(),sourceDepthFormat,true);
        }
        // Target labels and vector widgets can move inside their dynamic
        // vertex streams. Preserve each draw, not the first binding of a VS.
        // Three frames and 32 MiB bound this explicit diagnostic's cost.
        if(((vs==kHud || vs==kSprite || vs==kVscreen) && frame-firstFrame<3) || ((mesh || night) && frame==firstFrame) || effect) {
            ID3D11Buffer* raw[3]{};UINT strides[2]{},offsets[2]{},ibOffset=0;DXGI_FORMAT fmt{};
            ctx->IAGetVertexBuffers(0,2,raw,strides,offsets);ctx->IAGetIndexBuffer(raw+2,&fmt,&ibOffset);
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
                if(!unpacked && i==(vs==kVscreen?0:1))begin+=uint64_t(base>0?base:0)*s.stride;
                if(vs==kVscreen && i==1)begin+=uint64_t(startInstance)*s.stride;
                if(begin>=s.whole){++vertexDeclined;continue;}
                s.captureOffset=static_cast<uint32_t>(begin);
                UINT bytes=s.whole-s.captureOffset;if(bytes>256*1024)bytes=256*1024;
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
                if(vertexBytes+bytes>32*1024*1024){++vertexDeclined;continue;}
                bd.ByteWidth=bytes;bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(dev->CreateBuffer(&bd,nullptr,&s.stage))){++failures;continue;}
                D3D11_BOX box{s.captureOffset,0,0,s.captureOffset+bytes,1,1};ctx->CopySubresourceRegion(s.stage.Get(),0,0,0,0,src.Get(),0,&box);
                s.copied=bytes;vertexBytes+=bytes;copied=true;
            }
            vertexDraws+=copied?1u:0u;
        }
        draws.push_back(std::move(d));
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
        u32(7); u32(static_cast<uint32_t>(draws.size())); u32(static_cast<uint32_t>(surfaces.size())); u32(dropped);
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
        };
        for (Draw& d : draws) {
            u64(d.vs); u64(d.ps); u64(d.target);
            u32(d.frame); u32(d.ordinal); u32(d.kind); u32(d.count); u32(d.instances); u32(d.startInstance);
            u32(d.width); u32(d.height); u32(d.texture);
            for (int i = 0; i < 4; ++i) {
                u64(d.source[i]); u32(d.whole[i]);
                payload(d.stage[i].Get(), d.copied[i], 0, 0);
            }
            u32(d.start);u32(static_cast<uint32_t>(d.base));
            for(auto& s:d.streams){u32(s.offset);u32(s.stride);u32(s.whole);u32(s.captureOffset);payload(s.stage.Get(),s.copied,0,0);}
            for(auto id:d.mesh)u32(id);
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
            u32(b.frame);u32(b.firstDraw);u32(b.bytes);u32(b.stride);
            payload(b.stage.Get(),b.bytes,0,0);
        }
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
        u32(failures);
        ok = !ferror(f) && ok;
        return fclose(f) == 0 && ok;
    }

private:
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
        if(draw<draws.size() && draws[draw].vs==kNight) {
            e.x=(td.Width-e.width)/2;e.y=(td.Height-e.height)/3;
        }
        if(bc4){e.x&=~3u;e.y&=~3u;e.bytes=((e.width+3)/4)*((e.height+3)/4)*8;}
        if((!bpp && !bc4) || !e.bytes || td.SampleDesc.Count!=1 || td.ArraySize!=1 ||
           effectImages.size()>=32 || e.bytes>kEffectImageBudget-effectImageBytes) {
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
    static std::mutex& shaderMutex() { static std::mutex m; return m; }
    static std::map<uint64_t, std::vector<uint8_t>>& shaderBytes() {
        static std::map<uint64_t, std::vector<uint8_t>> s; return s;
    }
    uint32_t captureSurface(ID3D11DeviceContext* ctx, ID3D11Device* dev, uint32_t frame,UINT slot,bool large=false) {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ctx->PSGetShaderResources(slot, 1, &srv);
        if (!srv) return UINT32_MAX;
        Microsoft::WRL::ComPtr<ID3D11Resource> res; srv->GetResource(&res);
        Texture tex;
        if (FAILED(res.As(&tex))) { ++failures; return UINT32_MAX; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; srv->GetDesc(&sd);
        if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || sd.Texture2D.MostDetailedMip!=0) {++failures;return UINT32_MAX;}
        return copySurface(ctx,dev,frame,tex.Get(),sd.Format,large);
    }
    uint32_t copySurface(ID3D11DeviceContext* ctx,ID3D11Device* dev,uint32_t frame,ID3D11Texture2D* tex,DXGI_FORMAT format,bool large) {
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
        if (!bpp || td.SampleDesc.Count != 1 || td.ArraySize != 1 ||
            bytes > (large?kVscreenTextureBytes:kMaxTextureBytes) ||
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
