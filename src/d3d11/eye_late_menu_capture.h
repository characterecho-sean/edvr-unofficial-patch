#pragma once
#include "eye_draw_snapshot.h"
#include <cwchar>
#include <cmath>
#include <string>

namespace edvr {
// Diagnostic families shared by several game menus. They are not a menu identity or a picking mesh.
struct LateMenuWindow {
    static constexpr uint32_t kPerKind=4, kMeshBytes=128*1024*1024, kVertexBytes=8*1024*1024;
    uint32_t frame=0,seen[3][2][2]{},kept[3][2][2]{},declined[3][2][2]{};
    uint32_t partial=0,outside=0,firstMenu[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    bool scene[2]{};
    static uint32_t bucket(int eye) noexcept {return eye==0?0u:eye==1?1u:2u;}
    void arm(uint32_t armFrame) noexcept {*this={};frame=armFrame+1;}
    // Fix the frame before observing any menu draw: siblings and an earlier scene cannot be lost waiting for the 360-index anchor.
    bool take(uint32_t ledgerFrame,int eye,uint32_t ordinal,uint64_t vs,uint32_t count) noexcept {
        if(ledgerFrame!=frame){if(ledgerFrame<frame)++partial;else ++outside;return false;}
        const uint32_t e=bucket(eye);
        if(EyeDrawSnapshot::lateMenuSceneReference(vs)) {
            if(e>=2 || scene[e] || firstMenu[e]!=UINT32_MAX)return false;
            scene[e]=true;return true;
        }
        if(!EyeDrawSnapshot::lateMenuFamily(vs))return false;
        if(firstMenu[e]==UINT32_MAX)firstMenu[e]=ordinal;
        const uint32_t family=vs==EyeDrawSnapshot::kLateMenuVs?0u:1u,anchor=count==360?0u:1u;
        ++seen[e][family][anchor];
        if(kept[e][family][anchor]>=kPerKind){++declined[e][family][anchor];return false;}
        ++kept[e][family][anchor];return true;
    }
};

class EyeLateMenuCapture {
public:
    EyeDrawSnapshot evidence;
    LateMenuWindow window;
    struct Binding {uint64_t identity=0;uint32_t first=0,count=0,rangeKnown=0;};
    struct Input {uint64_t view=0,resource=0;uint32_t dimension=0,format=0,width=0,height=0,mips=0,slices=0;uint32_t descriptor[6]{};};
    struct AvatarReference {
        uint32_t status=0,validFlags=0,requestedFrame=0,sourceFrame=0,fpFrame=0,rootReadRenderFrame=0,gGameFrame=0,itemSerial=0;
        uint64_t sampleQpc=0,qpc=0,fpQpc=0,rootQpc=0,ms=0,rootMs=0,qpcFrequency=0;
        float froot[16]{},m3p[16]{},G[16]{},bodyOffset[3]{},pinEyeTarget[3]{},pinTarget[3]{};
        template<class T> void copy(const T& a) {
            status=a.status;validFlags=a.validFlags;requestedFrame=a.requestedFrame;sourceFrame=a.sourceFrame;
            fpFrame=a.fpFrame;rootReadRenderFrame=a.rootReadRenderFrame;gGameFrame=a.gGameFrame;itemSerial=a.itemSerial;
            sampleQpc=a.sampleQpc;qpc=a.qpc;fpQpc=a.fpQpc;rootQpc=a.rootQpc;ms=a.ms;rootMs=a.rootMs;qpcFrequency=a.qpcFrequency;
            std::memcpy(froot,a.froot,sizeof(froot));std::memcpy(m3p,a.m3p,sizeof(m3p));std::memcpy(G,a.G,sizeof(G));
            std::memcpy(bodyOffset,a.bodyOffset,sizeof(bodyOffset));std::memcpy(pinEyeTarget,a.pinEyeTarget,sizeof(pinEyeTarget));std::memcpy(pinTarget,a.pinTarget,sizeof(pinTarget));
        }
    };
    struct Row {uint32_t draw=0,role=0,eyeSource=0;Binding constants[3];uint64_t streams[3]{};Input inputs[16];AvatarReference avatar;std::string inputBridge;};
    std::vector<Row> rows;
    uint32_t meshUsed[3]{},vertexUsed[3]{};
    void release() {evidence.reset();rows.clear();window={};std::memset(meshUsed,0,sizeof(meshUsed));std::memset(vertexUsed,0,sizeof(vertexUsed));}
    void arm(uint32_t frame) {
        evidence.reset();evidence.lateMenuEvidence=true;window.arm(frame);rows.clear();
        std::memset(meshUsed,0,sizeof(meshUsed));std::memset(vertexUsed,0,sizeof(vertexUsed));
    }
    bool wants(uint32_t frame,uint64_t vs) const noexcept {
        return frame==window.frame && (EyeDrawSnapshot::lateMenuFamily(vs)||EyeDrawSnapshot::lateMenuSceneReference(vs));
    }
    void capture(ID3D11DeviceContext* ctx,uint32_t frame,uint32_t renderFrame,int eye,const EyeMeshJoinStamp& join,
                 uint32_t ordinal,uint64_t vs,uint64_t ps,char kind,uint32_t count,uint32_t instances,
                 uint32_t startInstance,uint32_t start,int32_t base,uint32_t eyeSource=0) {
        if(!ctx || (!EyeDrawSnapshot::lateMenuFamily(vs)&&!EyeDrawSnapshot::lateMenuSceneReference(vs)) ||
           !window.take(frame,eye,ordinal,vs,count))return;
        const uint32_t e=LateMenuWindow::bucket(eye),role=EyeDrawSnapshot::lateMenuSceneReference(vs)?2u:1u;
        // Known eyes each own 56 MiB; an unknown eye can consume only its separate 16 MiB. Whole-run limits never reset per frame.
        const uint32_t meshCap=(e==2?16u:56u)*1024*1024,vertexCap=(e==2?2u:3u)*1024*1024;
        evidence.meshBudgetLimit=evidence.meshBytes+meshCap-meshUsed[e];
        evidence.vertexBudgetLimit=evidence.vertexBytes+vertexCap-vertexUsed[e];
        const uint32_t meshBefore=evidence.meshBytes,vertexBefore=evidence.vertexBytes;
        Row row;row.draw=static_cast<uint32_t>(evidence.draws.size());row.role=role;row.eyeSource=eyeSource;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ranged;ctx->QueryInterface(IID_PPV_ARGS(&ranged));
        for(UINT i=0;i<3;++i) {
            Microsoft::WRL::ComPtr<ID3D11Buffer> cb;UINT first=0,n=0;
            if(ranged){ranged->VSGetConstantBuffers1(i,1,&cb,&first,&n);row.constants[i].rangeKnown=1;}
            else ctx->VSGetConstantBuffers(i,1,&cb);
            row.constants[i].identity=reinterpret_cast<uint64_t>(cb.Get());row.constants[i].first=first;row.constants[i].count=n;
        }
        ID3D11Buffer* streams[3]{};UINT stride[2]{},offset[2]{},ibOffset=0;DXGI_FORMAT format{};
        ctx->IAGetVertexBuffers(0,2,streams,stride,offset);ctx->IAGetIndexBuffer(streams+2,&format,&ibOffset);
        for(unsigned i=0;i<3;++i){row.streams[i]=reinterpret_cast<uint64_t>(streams[i]);if(streams[i])streams[i]->Release();}
        static_assert(sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC)==24,"menu input descriptor disk size");
        for(UINT slot=0;slot<16;++slot) {
            auto& input=row.inputs[slot];Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;ctx->PSGetShaderResources(slot,1,&srv);
            input.view=reinterpret_cast<uint64_t>(srv.Get());if(!srv)continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);input.dimension=sd.ViewDimension;input.format=sd.Format;
            std::memcpy(input.descriptor,&sd,sizeof(sd));
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;srv->GetResource(&resource);input.resource=reinterpret_cast<uint64_t>(resource.Get());
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;if(resource && SUCCEEDED(resource.As(&tex))){D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
                input.width=td.Width;input.height=td.Height;input.mips=td.MipLevels;input.slices=td.ArraySize;}
        }
        evidence.capture(ctx,frame,ordinal,vs,ps,kind,count,instances,startInstance,start,base,false,false,true,
                         renderFrame,eye,true,false,nullptr,role);
        meshUsed[e]+=evidence.meshBytes-meshBefore;vertexUsed[e]+=evidence.vertexBytes-vertexBefore;
        if(evidence.draws.size()>row.draw){evidence.draws.back().join=join;evidence.candidateFrames.back().eye=eye;rows.push_back(row);}
    }
    bool write(ID3D11DeviceContext* ctx,const wchar_t* binary,const wchar_t* metadata,const std::string& inputObserver={}) {
        const bool binaryOk=evidence.write(ctx,binary);FILE* f=nullptr;if(_wfopen_s(&f,metadata,L"wb")||!f)return false;
        const wchar_t* leaf=wcsrchr(binary,L'\\');leaf=leaf?leaf+1:binary;
        std::fprintf(f,"{\"schema\":\"edvr_late_menu_v1\",\"binary\":\"%ls\",\"binary_written\":%s,\"selected_frame\":%u,\"mesh_limit\":%u,\"vertex_limit\":%u,\"partial_draws\":%u,\"outside_draws\":%u,\"binding_phase\":\"requested_pre_overrides\",\"ps_input_slots\":[0,15],\"texture_pixels\":\"not_captured\",\"cpu_picking\":\"unavailable\",\"eyes\":[",
                     leaf,binaryOk?"true":"false",window.frame,LateMenuWindow::kMeshBytes,LateMenuWindow::kVertexBytes,window.partial,window.outside);
        for(unsigned e=0;e<3;++e){if(e)std::fputc(',',f);std::fprintf(f,"{\"eye\":%d,\"scene_before_menu\":%s,\"mesh_bytes\":%u,\"vertex_bytes\":%u,\"families\":[",e==2?-1:static_cast<int>(e),e<2&&window.scene[e]?"true":"false",meshUsed[e],vertexUsed[e]);
            for(unsigned family=0;family<2;++family){if(family)std::fputc(',',f);std::fprintf(f,"{\"anchor_seen\":%u,\"siblings_seen\":%u,\"anchor_kept\":%u,\"siblings_kept\":%u,\"anchor_quota\":%u,\"siblings_quota\":%u}",
                window.seen[e][family][0],window.seen[e][family][1],window.kept[e][family][0],window.kept[e][family][1],window.declined[e][family][0],window.declined[e][family][1]);}
            std::fputs("]}",f);}
        std::fputs("],\"rows\":[",f);
        for(size_t n=0;n<rows.size();++n){if(n)std::fputc(',',f);const auto& r=rows[n];std::fprintf(f,"{\"draw\":%u,\"role\":%u,\"eye_source\":%u,\"constants\":[",r.draw,r.role,r.eyeSource);
            for(unsigned i=0;i<3;++i){if(i)std::fputc(',',f);const auto& c=r.constants[i];std::fprintf(f,"{\"identity\":\"%016llX\",\"first\":%u,\"count\":%u,\"range_known\":%u}",static_cast<unsigned long long>(c.identity),c.first,c.count,c.rangeKnown);}
            std::fprintf(f,"],\"streams\":[\"%016llX\",\"%016llX\",\"%016llX\"],\"ps_inputs\":[",static_cast<unsigned long long>(r.streams[0]),static_cast<unsigned long long>(r.streams[1]),static_cast<unsigned long long>(r.streams[2]));
            for(unsigned slot=0;slot<16;++slot){if(slot)std::fputc(',',f);const auto& i=r.inputs[slot];std::fprintf(f,"{\"slot\":%u,\"view\":\"%016llX\",\"resource\":\"%016llX\",\"dimension\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"mips\":%u,\"slices\":%u,\"descriptor\":[%u,%u,%u,%u,%u,%u]}",
                slot,static_cast<unsigned long long>(i.view),static_cast<unsigned long long>(i.resource),i.dimension,i.format,i.width,i.height,i.mips,i.slices,i.descriptor[0],i.descriptor[1],i.descriptor[2],i.descriptor[3],i.descriptor[4],i.descriptor[5]);}
            const auto& a=r.avatar;
            std::fprintf(f,"],\"avatar\":{\"status\":%u,\"valid_flags\":%u,\"requested_frame\":%u,\"source_frame\":%u,\"fp_frame\":%u,\"root_read_render_frame\":%u,\"g_game_frame\":%u,\"item_serial\":%u,\"sample_qpc\":%llu,\"qpc\":%llu,\"fp_qpc\":%llu,\"root_qpc\":%llu,\"ms\":%llu,\"root_ms\":%llu,\"qpc_frequency\":%llu",
                a.status,a.validFlags,a.requestedFrame,a.sourceFrame,a.fpFrame,a.rootReadRenderFrame,a.gGameFrame,a.itemSerial,
                static_cast<unsigned long long>(a.sampleQpc),static_cast<unsigned long long>(a.qpc),static_cast<unsigned long long>(a.fpQpc),static_cast<unsigned long long>(a.rootQpc),
                static_cast<unsigned long long>(a.ms),static_cast<unsigned long long>(a.rootMs),static_cast<unsigned long long>(a.qpcFrequency));
            const auto floats=[&](const char* name,const float* values,unsigned count){std::fprintf(f,",\"%s\":[",name);for(unsigned i=0;i<count;++i){if(i)std::fputc(',',f);if(std::isfinite(values[i]))std::fprintf(f,"%.9g",static_cast<double>(values[i]));else std::fputs("null",f);}std::fputc(']',f);};
            floats("froot",a.froot,16);floats("m3p",a.m3p,16);floats("G",a.G,16);floats("body_offset",a.bodyOffset,3);floats("pin_eye_target",a.pinEyeTarget,3);floats("pin_target",a.pinTarget,3);
            std::fputs("},\"input_bridge\":",f);std::fputs(r.inputBridge.empty()?"null":r.inputBridge.c_str(),f);std::fputc('}',f);}
        std::fputs("],\"input_observer\":",f);std::fputs(inputObserver.empty()?"null":inputObserver.c_str(),f);
        std::fputs("}\n",f);const bool ok=!std::ferror(f);return std::fclose(f)==0&&ok&&binaryOk;
    }
};
} // namespace edvr
