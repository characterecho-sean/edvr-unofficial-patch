#pragma once
// Manual F10 evidence for the exact alternate-camera weapon draw. No game
// binding or AA decision depends on this class. The DSV is copied whole to a
// private typeless GPU texture; a fixed compute shader extracts the two planes
// into small, CPU-readable ROI textures. D3D11 forbids boxed depth/stencil
// copies, including copies from a typeless depth/stencil mirror.
#include <d3d11_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "flat_compute_readback.h"
#include "exposure_fix.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "../common/config.h"
#include "../common/log.h"
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

namespace edvr {
class FlatWeaponFootprint {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    static constexpr uint64_t kVs=0x025B4B9FF54622EDull;
    static constexpr uint64_t kPs=0x46F92DC71BF8DFA5ull;
    static constexpr uint64_t kCap=384ull*1024*1024;
    static constexpr unsigned kStages=4;
    static constexpr const char* kNames[kStages]={"before","after","hdr_consumer","end_frame"};
    struct Plane {
        Ptr<ID3D11Texture2D> stage;
        const char* status="partial";
        const char* reason="not-reached";
        std::string file;
        unsigned format=0,rowBytes=0;
        uint64_t bytes=0;
    };
    struct Stage {
        const char* name="";
        const char* status="partial";
        const char* reason="not-reached";
        uint32_t drawSeq=0;
        Plane color,depth,stencil;
    };
    struct Clear { uint32_t sequence=0,flags=0,stencil=0; const void* view=nullptr; };
    struct DrawState {
        const char* kind="unknown";
        bool argumentsKnown=false;
        uint32_t count=0,start=0,instances=0,startInstance=0;
        int32_t base=0;
        const void* indirectBuffer=nullptr;uint32_t indirectOffset=0;
        uint32_t topology=0,sampleMask=~0u,scissorCount=0;
        D3D11_BLEND_DESC blend{};D3D11_RASTERIZER_DESC raster{};
        D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        Ptr<ID3D11Predicate> predicate;bool predicateValue=false;
        const void* effectiveRtv=nullptr,*effectiveDsv=nullptr;
    };
    struct QueryResult {
        Ptr<ID3D11Query> query;
        const char* status="unavailable",*reason="draw-not-bracketed";
        uint32_t hr=0;
        uint64_t samples=0;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    };
    struct Visibility {
        const char* status="unavailable",*reason="draw-not-bracketed";
        bool began=false,ended=false,valid=false;
        uint32_t beginSeq=0,endSeq=0,polls=0;
        QueryResult occlusion,pipeline;
    };
    struct Frame {
        uint64_t number=0,startedMs=0,allocation=0;
        uint32_t drawSeq=0,drawCountExact=0,consumerSeq=0,consumerSlot=~0u,firstBadSeq=0;
        uint64_t consumerVs=0,consumerPs=0;
        const char* consumerVerdict="not-observed";
        uint32_t width=0,height=0,roiX=0,roiY=0,roiW=0,roiH=0;
        uint32_t colorFormat=0,depthFormat=0,depthViewFormat=0;
        uint32_t stencilRef=0,stencilReadMask=0,stencilWriteMask=0,depthWrite=0;
        uint32_t depthEnable=0,depthFunc=0,stencilEnable=0,dsvFlags=0;
        uint32_t frontFunc=0,frontPass=0,frontFail=0,frontDepthFail=0;
        uint32_t backFunc=0,backPass=0,backFail=0,backDepthFail=0;
        uint32_t viewportCount=0;D3D11_VIEWPORT viewport{};
        uint64_t cameraHash=0;
        uint64_t referenceCameraHash=0;
        float camera[6][4]{};
        float referenceCamera[6][4]{};
        const char* firstBadCause="not-observed";
        bool hdrConflict=false,consumerFound=false,afterDone=false,ended=false;
        const char* status="partial",*reason="";
        Ptr<ID3D11Texture2D> color,depth,mirror,outDepth,outStencil;
        Ptr<ID3D11Resource> colorResource,depthResource;
        Ptr<ID3D11RenderTargetView> rtv;
        Ptr<ID3D11DepthStencilView> dsv;
        Ptr<ID3D11ShaderResourceView> depthSrv,stencilSrv;
        Ptr<ID3D11UnorderedAccessView> depthUav,stencilUav;
        Stage stages[kStages];
        DrawState drawState;Visibility visibility;
        std::vector<Clear> clears;
        uint32_t clearOverflow=0;
    };
    std::unique_ptr<Frame> frame_;
    Ptr<ID3D11ComputeShader> shader_;
    Ptr<ID3D11Buffer> regionCb_;
    std::wstring directory_;
    uint64_t armFrame_=0,armMs_=0,diskBudget_=0;
    uint32_t serial_=0,completed_=0,attempts_=0,unsupported_=0;
    bool armed_=false,failed_=false;
#ifdef EDVR_WEAPON_FOOTPRINT_TEST
    std::string fixtureCase_;
    bool forceQueryUnavailable_=false,forceQueryPending_=false;
#endif

    static std::string ptr(const void* p) { char b[32]{};std::snprintf(b,sizeof(b),"0x%llX",static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));return b; }
    static std::string hash(uint64_t v) { char b[20]{};std::snprintf(b,sizeof(b),"%016llX",static_cast<unsigned long long>(v));return b; }
    static const char* drawKind(char kind) {
        switch(kind){case 'D':return "Draw";case 'I':return "DrawIndexed";
        case 'N':return "DrawInstanced";case 'X':return "DrawIndexedInstanced";
        case 'A':return "DrawAuto";case 'Y':return "DrawInstancedIndirect";
        case 'Z':return "DrawIndexedInstancedIndirect";default:return "unknown";}
    }
    static bool write(const std::wstring& path,const void* data,size_t n) {
        FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"wb")||!f)return false;
        const bool ok=std::fwrite(data,1,n,f)==n;return std::fclose(f)==0&&ok;
    }
    static unsigned colorBpp(DXGI_FORMAT f) {
        switch(f){
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
        default:return 0;
        }
    }
    static void captureDrawState(ID3D11DeviceContext* ctx,DrawState& d,char kind,
                                 uint32_t count,uint32_t start,int32_t base,uint32_t instances,
                                 uint32_t startInstance,ID3D11Buffer* indirectBuffer,uint32_t indirectOffset) {
        d.kind=drawKind(kind);d.argumentsKnown=kind=='D'||kind=='I'||kind=='N'||kind=='X';
        d.count=count;d.start=start;d.base=base;d.instances=instances;d.startInstance=startInstance;
        d.indirectBuffer=indirectBuffer;d.indirectOffset=indirectOffset;
        D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);d.topology=topology;
        Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=~0u;
        ctx->OMGetBlendState(&blend,factors,&mask);d.sampleMask=mask;
        if(blend)blend->GetDesc(&d.blend);
        else for(auto& target:d.blend.RenderTarget)target.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        Ptr<ID3D11RasterizerState> raster;ctx->RSGetState(&raster);
        if(raster)raster->GetDesc(&d.raster);
        else {d.raster.FillMode=D3D11_FILL_SOLID;d.raster.CullMode=D3D11_CULL_BACK;
              d.raster.DepthClipEnable=TRUE;}
        UINT n=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetScissorRects(&n,d.scissors);d.scissorCount=n;
        BOOL value=FALSE;ctx->GetPredication(&d.predicate,&value);d.predicateValue=value!=FALSE;
        Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;
        ctx->OMGetRenderTargets(1,&rt,&ds);d.effectiveRtv=rt.Get();d.effectiveDsv=ds.Get();
    }
    static void queryBegin(ID3D11DeviceContext* ctx,QueryResult& q,D3D11_QUERY kind) {
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!dev){q.status="unavailable";q.reason="device-unavailable";return;}
        D3D11_QUERY_DESC desc{};desc.Query=kind;
        const HRESULT hr=dev->CreateQuery(&desc,&q.query);q.hr=static_cast<uint32_t>(hr);
        if(FAILED(hr)||!q.query){q.status="unavailable";q.reason="create-query-failed";return;}
        ctx->Begin(q.query.Get());q.status="pending";q.reason="query-pending";
    }
    static void queryPoll(ID3D11DeviceContext* ctx,QueryResult& q,bool pipeline,bool forcePending) {
        if(std::strcmp(q.status,"pending")||!q.query)return;
        if(forcePending){q.hr=S_FALSE;return;}
        const HRESULT hr=pipeline?
            ctx->GetData(q.query.Get(),&q.stats,sizeof(q.stats),D3D11_ASYNC_GETDATA_DONOTFLUSH):
            ctx->GetData(q.query.Get(),&q.samples,sizeof(q.samples),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        q.hr=static_cast<uint32_t>(hr);
        if(hr==S_OK){q.status="complete";q.reason="";}
        else if(hr!=S_FALSE){q.status="failed";q.reason="get-data-failed";}
    }
    bool reserve(Frame& f,uint64_t n) {
        if(n>kCap||f.allocation>kCap-n)return false;
        f.allocation+=n;return true;
    }
    static bool makeTexture(ID3D11Device* dev,UINT w,UINT h,DXGI_FORMAT format,
                            D3D11_USAGE usage,UINT bind,UINT cpu,Ptr<ID3D11Texture2D>& out) {
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.Usage=usage;d.BindFlags=bind;d.CPUAccessFlags=cpu;
        return SUCCEEDED(dev->CreateTexture2D(&d,nullptr,&out))&&out;
    }
    bool resources(ID3D11DeviceContext* ctx,Frame& f) {
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(!dev)return false;
        D3D11_TEXTURE2D_DESC dd{};f.depth->GetDesc(&dd);
        const uint64_t mirrorBytes=uint64_t(dd.Width)*dd.Height*8;
        const uint64_t smallBytes=uint64_t(f.roiW)*f.roiH*4;
        if(!reserve(f,mirrorBytes+2*smallBytes))return false;
        dd.Usage=D3D11_USAGE_DEFAULT;dd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        dd.CPUAccessFlags=dd.MiscFlags=0;
        if(FAILED(dev->CreateTexture2D(&dd,nullptr,&f.mirror))||!f.mirror||
           !makeTexture(dev.Get(),f.roiW,f.roiH,DXGI_FORMAT_R32_FLOAT,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,f.outDepth)||
           !makeTexture(dev.Get(),f.roiW,f.roiH,DXGI_FORMAT_R32_UINT,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,f.outStencil))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        sv.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        if(FAILED(dev->CreateShaderResourceView(f.mirror.Get(),&sv,&f.depthSrv)))return false;
        sv.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        if(FAILED(dev->CreateShaderResourceView(f.mirror.Get(),&sv,&f.stencilSrv)))return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};uv.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
        uv.Format=DXGI_FORMAT_R32_FLOAT;
        if(FAILED(dev->CreateUnorderedAccessView(f.outDepth.Get(),&uv,&f.depthUav)))return false;
        uv.Format=DXGI_FORMAT_R32_UINT;
        if(FAILED(dev->CreateUnorderedAccessView(f.outStencil.Get(),&uv,&f.stencilUav)))return false;
        if(!shader_)shader_.Attach(shaderSwapCreateCs(ctx,kWeaponFootprintBytecode,sizeof(kWeaponFootprintBytecode),
                                                     "weapon footprint extraction","flat weapon footprint"));
        if(!shader_)return false;
        if(!regionCb_){D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(dev->CreateBuffer(&bd,nullptr,&regionCb_)))return false;}
        return true;
    }
    bool preparePlane(ID3D11Device* dev,Frame& f,Plane& p,DXGI_FORMAT format,unsigned bpp) {
        const uint64_t bytes=uint64_t(f.roiW)*f.roiH*bpp;
        if(!reserve(f,bytes))return false;
        if(!makeTexture(dev,f.roiW,f.roiH,format,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,p.stage))return false;
        p.format=static_cast<unsigned>(format);p.rowBytes=f.roiW*bpp;p.bytes=bytes;p.status="queued";p.reason="";
        return true;
    }
    bool extract(ID3D11DeviceContext* ctx,Frame& f) {
        // The private mirror has no DSV binding. The game retains its OM state;
        // only CS slots 0..1 and CB0 are changed, then restored exactly.
        Ptr<ID3D11Predicate> predicate;BOOL prediction=FALSE;ctx->GetPredication(&predicate,&prediction);
        if(predicate)return false;
        ID3D11UnorderedAccessView* om[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,om);
        bool busy=false;for(auto*& p:om){if(p){busy=true;p->Release();p=nullptr;}}
        if(busy)return false;
        Ptr<ID3D11ComputeShader> prior;
        ID3D11ClassInstance* classes[256]{};UINT classCount=256;
        ctx->CSGetShader(&prior,classes,&classCount);
        ID3D11ShaderResourceView* savedSrv[2]{};ctx->CSGetShaderResources(0,2,savedSrv);
        ID3D11UnorderedAccessView* savedUav[2]{};ctx->CSGetUnorderedAccessViews(0,2,savedUav);
        Ptr<ID3D11Buffer> savedCb;ctx->CSGetConstantBuffers(0,1,&savedCb);
        const UINT region[4]={f.roiX,f.roiY,f.roiW,f.roiH};ctx->UpdateSubresource(regionCb_.Get(),0,nullptr,region,0,0);
        ID3D11ShaderResourceView* inputs[2]={f.depthSrv.Get(),f.stencilSrv.Get()};
        ID3D11UnorderedAccessView* outputs[2]={f.depthUav.Get(),f.stencilUav.Get()};
        ID3D11Buffer* cb=regionCb_.Get();
        ctx->CSSetShader(shader_.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,1,&cb);
        ctx->CSSetShaderResources(0,2,inputs);ctx->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
        ctx->Dispatch((f.roiW+15)/16,(f.roiH+15)/16,1);
        ID3D11ShaderResourceView* nullSrv[2]{};ID3D11UnorderedAccessView* nullUav[2]{};
        ctx->CSSetShaderResources(0,2,nullSrv);ctx->CSSetUnorderedAccessViews(0,2,nullUav,nullptr);
        ctx->CSSetShader(prior.Get(),classes,classCount);
        ID3D11Buffer* oldCb=savedCb.Get();ctx->CSSetConstantBuffers(0,1,&oldCb);
        ctx->CSSetShaderResources(0,2,savedSrv);
        UINT keep[2]={~0u,~0u};ctx->CSSetUnorderedAccessViews(0,2,savedUav,keep);
        for(UINT i=0;i<classCount;++i)if(classes[i])classes[i]->Release();
        for(auto* p:savedSrv)if(p)p->Release();for(auto* p:savedUav)if(p)p->Release();
        return true;
    }
    void snapshot(ID3D11DeviceContext* ctx,Frame& f,unsigned index,uint32_t sequence) {
        Stage& s=f.stages[index];s.name=kNames[index];s.drawSeq=sequence;
        if(std::strcmp(s.status,"partial"))return;
        if(!ctx){s.reason="no-context";return;}
        FlatComputeInternalScope internal;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        const unsigned bpp=colorBpp(static_cast<DXGI_FORMAT>(f.colorFormat));
        if(!dev||!bpp||!preparePlane(dev.Get(),f,s.color,static_cast<DXGI_FORMAT>(f.colorFormat),bpp)||
           !preparePlane(dev.Get(),f,s.depth,DXGI_FORMAT_R32_FLOAT,4)||
           !preparePlane(dev.Get(),f,s.stencil,DXGI_FORMAT_R32_UINT,4)){
            s.status="failed";s.reason="staging-or-budget";failed_=true;return;
        }
        s.stencil.format=DXGI_FORMAT_R8_UINT;s.stencil.rowBytes=f.roiW;s.stencil.bytes=uint64_t(f.roiW)*f.roiH;
        D3D11_BOX box{f.roiX,f.roiY,0,f.roiX+f.roiW,f.roiY+f.roiH,1};
        ctx->CopySubresourceRegion(s.color.stage.Get(),0,0,0,0,f.color.Get(),0,&box);
        // Depth/stencil copy is whole-subresource. Never supply a box here.
        ctx->CopyResource(f.mirror.Get(),f.depth.Get());
        if(!extract(ctx,f)){s.status="failed";s.reason="unsafe-or-failed-cs-state";failed_=true;return;}
        ctx->CopyResource(s.depth.stage.Get(),f.outDepth.Get());
        ctx->CopyResource(s.stencil.stage.Get(),f.outStencil.Get());
        s.status="queued";s.reason="";
    }
    static bool readPlane(ID3D11DeviceContext* ctx,Plane& p,const std::wstring& path,
                          unsigned w,unsigned h,bool stencil) {
        if(std::strcmp(p.status,"queued"))return false;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr=ctx->Map(p.stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return false;
        if(FAILED(hr)){p.status="failed";p.reason="map-failed";return true;}
        FILE* file=nullptr;const bool opened=_wfopen_s(&file,path.c_str(),L"wb")==0&&file;
        bool ok=opened;
        std::vector<uint8_t> compact(stencil?w:0);
        for(unsigned y=0;y<h&&ok;++y){const auto* row=static_cast<const uint8_t*>(m.pData)+size_t(y)*m.RowPitch;
            if(stencil){for(unsigned x=0;x<w;++x)compact[x]=row[4*x];
                ok=std::fwrite(compact.data(),1,w,file)==w;}
            else ok=std::fwrite(row,1,p.rowBytes,file)==p.rowBytes;
        }
        ctx->Unmap(p.stage.Get(),0);
        if(opened && std::fclose(file)!=0)ok=false;
        p.status=ok?"complete":"failed";p.reason=ok?"":"write-failed";
        if(stencil){p.format=DXGI_FORMAT_R8_UINT;p.rowBytes=w;p.bytes=uint64_t(w)*h;}
        return true;
    }
    std::string json(const Frame& f) const {
        std::ostringstream o;o<<"{\"schema\":2,\"build\":\""<<EDVR_VERSION_STRING<<"\",\"frame\":"<<f.number
#ifdef EDVR_WEAPON_FOOTPRINT_TEST
         <<",\"fixture_kind\":\""<<(fixtureCase_.empty()?"flat_weapon_gpu_v1":"flat_weapon_gpu_v2")
         <<"\",\"fixture_case\":\""<<fixtureCase_<<"\""
#endif
         <<",\"status\":\""<<f.status<<"\",\"reason\":\""<<f.reason<<"\",\"vs\":\""<<hash(kVs)
         <<"\",\"ps\":\""<<hash(kPs)<<"\",\"camera_hash\":\""<<hash(f.cameraHash)
         <<"\",\"reference_camera_hash\":\""<<hash(f.referenceCameraHash)
         <<"\",\"draw_seq\":"<<f.drawSeq<<",\"draw_count_exact\":"<<f.drawCountExact
         <<",\"hdr_conflict\":"<<(f.hdrConflict?"true":"false")
         <<",\"first_bad_seq\":"<<f.firstBadSeq<<",\"first_bad_cause\":\""<<f.firstBadCause<<"\""
         <<",\"color_resource\":\""<<ptr(f.colorResource.Get())<<"\",\"depth_resource\":\""<<ptr(f.depthResource.Get())
         <<"\",\"rtv\":\""<<ptr(f.rtv.Get())<<"\",\"dsv\":\""<<ptr(f.dsv.Get())
         <<"\",\"source_width\":"<<f.width<<",\"source_height\":"<<f.height
         <<",\"color_format\":"<<f.colorFormat<<",\"depth_format\":"<<f.depthFormat
         <<",\"depth_view_format\":"<<f.depthViewFormat<<",\"stencil_ref\":"<<f.stencilRef
         <<",\"stencil_read_mask\":"<<f.stencilReadMask<<",\"stencil_write_mask\":"<<f.stencilWriteMask
         <<",\"depth_write\":"<<(f.depthWrite?"true":"false")
         <<",\"depth_enable\":"<<(f.depthEnable?"true":"false")<<",\"depth_func\":"<<f.depthFunc
         <<",\"stencil_enable\":"<<(f.stencilEnable?"true":"false")<<",\"dsv_flags\":"<<f.dsvFlags
         <<",\"front_stencil\":{\"func\":"<<f.frontFunc<<",\"pass\":"<<f.frontPass
         <<",\"fail\":"<<f.frontFail<<",\"depth_fail\":"<<f.frontDepthFail<<"}"
         <<",\"back_stencil\":{\"func\":"<<f.backFunc<<",\"pass\":"<<f.backPass
         <<",\"fail\":"<<f.backFail<<",\"depth_fail\":"<<f.backDepthFail<<"}"
         <<",\"viewport_count\":"<<f.viewportCount<<",\"viewport\":["<<f.viewport.TopLeftX<<","<<f.viewport.TopLeftY
         <<","<<f.viewport.Width<<","<<f.viewport.Height<<","<<f.viewport.MinDepth<<","<<f.viewport.MaxDepth<<"]"
         <<",\"roi\":{\"x\":"<<f.roiX<<",\"y\":"<<f.roiY
         <<",\"width\":"<<f.roiW<<",\"height\":"<<f.roiH<<"},\"camera_rows\":[";
        o<<std::setprecision(9);
        for(unsigned i=0;i<6;++i){if(i)o<<",";o<<"[";for(unsigned j=0;j<4;++j){if(j)o<<",";o<<f.camera[i][j];}o<<"]";}o<<"],\"reference_camera_rows\":[";
        for(unsigned i=0;i<6;++i){if(i)o<<",";o<<"[";for(unsigned j=0;j<4;++j){if(j)o<<",";o<<f.referenceCamera[i][j];}o<<"]";}o<<"]";
        o<<",\"consumer\":{\"draw_seq\":"<<f.consumerSeq<<",\"source_resource\":\""<<ptr(f.colorResource.Get())
         <<"\",\"found\":"<<(f.consumerFound?"true":"false")<<",\"phase\":\"before_draw\",\"vs\":\""
         <<hash(f.consumerVs)<<"\",\"ps\":\""<<hash(f.consumerPs)<<"\",\"srv_slot\":"<<f.consumerSlot
         <<",\"verdict\":\""<<f.consumerVerdict<<"\",\"first_bad_seq\":"<<f.firstBadSeq<<"}";
        const auto& d=f.drawState;const auto& v=f.visibility;
        o<<",\"draw_state\":{\"kind\":\""<<d.kind<<"\",\"arguments_known\":"<<(d.argumentsKnown?"true":"false");
        const auto arg=[&](const char* name,int64_t value){o<<",\""<<name<<"\":";if(d.argumentsKnown)o<<value;else o<<"null";};
        arg("count",d.count);arg("start",d.start);arg("base",d.base);arg("instances",d.instances);arg("start_instance",d.startInstance);
        o<<",\"indirect_buffer\":";
        if(d.indirectBuffer)o<<"\""<<ptr(d.indirectBuffer)<<"\"";else o<<"null";
        o<<",\"indirect_offset\":";if(d.indirectBuffer)o<<d.indirectOffset;else o<<"null";
        o<<",\"topology\":"<<d.topology<<",\"effective_rtv\":\""<<ptr(d.effectiveRtv)
         <<"\",\"effective_dsv\":\""<<ptr(d.effectiveDsv)<<"\",\"blend\":{\"alpha_to_coverage\":"
         <<(d.blend.AlphaToCoverageEnable?"true":"false")<<",\"independent\":"
         <<(d.blend.IndependentBlendEnable?"true":"false")<<",\"sample_mask\":"<<d.sampleMask<<",\"write_masks\":[";
        for(unsigned i=0;i<8;++i){if(i)o<<",";o<<static_cast<unsigned>(d.blend.RenderTarget[i].RenderTargetWriteMask);}o<<"],\"blend_enable\":[";
        for(unsigned i=0;i<8;++i){if(i)o<<",";o<<(d.blend.RenderTarget[i].BlendEnable?"true":"false");}o<<"]}";
        o<<",\"raster\":{\"cull\":"<<d.raster.CullMode<<",\"scissor_enable\":"
         <<(d.raster.ScissorEnable?"true":"false")<<",\"depth_bias\":"<<d.raster.DepthBias
         <<",\"slope_bias\":"<<d.raster.SlopeScaledDepthBias<<",\"scissors\":[";
        for(unsigned i=0;i<d.scissorCount;++i){if(i)o<<",";const auto& r=d.scissors[i];o<<"{\"left\":"<<r.left
            <<",\"top\":"<<r.top<<",\"right\":"<<r.right<<",\"bottom\":"<<r.bottom<<"}";}o<<"]}";
        o<<",\"predication\":{\"bound\":"<<(d.predicate?"true":"false")<<",\"pointer\":\""
         <<ptr(d.predicate.Get())<<"\",\"value\":"<<(d.predicateValue?"true":"false")<<"}}";
        o<<",\"visibility\":{\"scope\":\"original_exact_draw\",\"status\":\""<<v.status
         <<"\",\"reason\":\""<<v.reason<<"\",\"draw_seq\":"<<f.drawSeq<<",\"begin_seq\":";
        if(v.began)o<<v.beginSeq;else o<<"null";
        o<<",\"end_seq\":";if(v.ended)o<<v.endSeq;else o<<"null";
        const auto& q=v.occlusion;o<<",\"occlusion\":{\"status\":\""<<q.status<<"\",\"reason\":\""
            <<q.reason<<"\",\"hr\":"<<q.hr<<",\"samples_passed\":";
        if(!std::strcmp(q.status,"complete"))o<<q.samples;else o<<"null";
        const auto& p=v.pipeline;o<<"},\"pipeline_statistics\":{\"status\":\""<<p.status
            <<"\",\"reason\":\""<<p.reason<<"\",\"hr\":"<<p.hr;
        const char* statNames[]={"ia_vertices","ia_primitives","vs_invocations","gs_invocations","gs_primitives",
            "c_invocations","c_primitives","ps_invocations","hs_invocations","ds_invocations","cs_invocations"};
        const uint64_t statValues[]={p.stats.IAVertices,p.stats.IAPrimitives,p.stats.VSInvocations,p.stats.GSInvocations,
            p.stats.GSPrimitives,p.stats.CInvocations,p.stats.CPrimitives,p.stats.PSInvocations,p.stats.HSInvocations,
            p.stats.DSInvocations,p.stats.CSInvocations};
        for(unsigned i=0;i<11;++i){o<<",\""<<statNames[i]<<"\":";
            if(!std::strcmp(p.status,"complete"))o<<statValues[i];else o<<"null";}
        o<<"}},\"clears\":[";
        for(size_t i=0;i<f.clears.size();++i){if(i)o<<",";const auto& c=f.clears[i];o<<"{\"draw_seq\":"<<c.sequence
            <<",\"flags\":"<<c.flags<<",\"stencil\":"<<c.stencil<<",\"view\":\""<<ptr(c.view)<<"\"}";}
        o<<"],\"clear_overflow\":"<<f.clearOverflow<<",\"stages\":[";
        for(unsigned i=0;i<kStages;++i){if(i)o<<",";const auto& s=f.stages[i];o<<"{\"name\":\""<<kNames[i]
            <<"\",\"phase\":\""<<(i==0||i==2?"before_draw":i==1?"after_draw":"before_present")
            <<"\",\"frame\":"<<f.number<<",\"draw_seq\":"<<s.drawSeq
            <<",\"color_resource\":\""<<ptr(f.colorResource.Get())<<"\",\"depth_resource\":\""<<ptr(f.depthResource.Get())
            <<"\",\"status\":\""<<(std::strcmp(s.status,"queued")?s.status:"partial")
            <<"\",\"reason\":\""<<s.reason<<"\",\"planes\":{";
            const Plane* planes[]={&s.color,&s.depth,&s.stencil};const char* names[]={"color","depth","stencil"};
            for(unsigned p=0;p<3;++p){if(p)o<<",";const Plane& a=*planes[p];o<<"\""<<names[p]<<"\":{\"file\":\""
                <<a.file<<"\",\"format\":"<<a.format<<",\"row_bytes\":"<<a.rowBytes
                <<",\"bytes\":"<<a.bytes<<",\"status\":\""<<(std::strcmp(a.status,"queued")?a.status:"partial")
                <<"\",\"reason\":\""<<a.reason<<"\"}";}
            o<<"}}";}
        o<<"]}";return o.str();
    }
    void finish(Frame& f,const char* forced=nullptr) {
        for(QueryResult* q:{&f.visibility.occlusion,&f.visibility.pipeline})
            if(!std::strcmp(q->status,"pending")){
                q->status=forced&&std::strcmp(forced,"readback-timeout")?"failed":"timeout";
                q->reason=forced&&std::strcmp(forced,"readback-timeout")?forced:"get-data-timeout";
            }
        for(auto& s:f.stages){
            if(!std::strcmp(s.status,"queued")){s.status="partial";s.reason=forced?forced:"readback-incomplete";}
            Plane* planes[]={&s.color,&s.depth,&s.stencil};
            for(auto* p:planes)if(std::strcmp(p->status,"complete")){
                if(!std::strcmp(p->status,"queued")){p->status="partial";p->reason=forced?forced:"readback-incomplete";}
                p->file.clear();p->bytes=0;
            }
        }
        bool complete=!forced&&f.hdrConflict&&f.afterDone&&f.consumerFound&&f.ended;
        for(auto& s:f.stages){if(std::strcmp(s.status,"complete"))complete=false;}
        f.status=complete?"complete":f.afterDone?"partial":"failed";
        f.reason=forced?forced:complete?"":!f.consumerFound?"consumer-not-observed":!f.ended?"frame-end-not-observed":
            !f.afterDone?"after-draw-not-observed":"stage-incomplete";
        const std::wstring stem=directory_+L"\\frame_"+std::to_wstring(f.number);
        const std::string body=json(f);
        const std::wstring temporary=stem+L".json.tmp",target=stem+L".json";
        const bool files=write(temporary,body.data(),body.size())&&
            MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
        Log::get().note("flat weapon footprint: frame=%llu status=%s reason=%s draw-seq=%u exact-draws=%u consumer=%u stages=%s/%s/%s/%s bytes=%llu files=%s directory=%ls",
            static_cast<unsigned long long>(f.number),f.status,f.reason,f.drawSeq,f.drawCountExact,f.consumerFound?1u:0u,
            f.stages[0].status,f.stages[1].status,f.stages[2].status,f.stages[3].status,
            static_cast<unsigned long long>(f.allocation),files?"ok":"failed",directory_.c_str());
        if(!files)failed_=true;
        const uint64_t outputPerPixel=colorBpp(static_cast<DXGI_FORMAT>(f.colorFormat))+5;
        diskBudget_+=uint64_t(f.roiW)*f.roiH*outputPerPixel*kStages+body.size();
        ++completed_;
    }
public:
    bool active() const {return armed_;}
    const std::wstring& directory() const {return directory_;}
    bool tracking(uint64_t frame) const {return armed_&&frame_&&frame_->number==frame;}
    const void* colorResource() const {return frame_?frame_->colorResource.Get():nullptr;}
    void cancel(const char* reason) {
        if(frame_){finish(*frame_,reason);frame_.reset();}
        if(armed_)Log::get().note("flat weapon footprint: summary status=%s attempts=%u completed=%u unsupported=%u directory=%ls",
            reason,attempts_,completed_,unsupported_,directory_.c_str());
        armed_=false;
    }
    void arm(uint64_t frame) {
        cancel("rearmed");shader_.Reset();regionCb_.Reset();directory_.clear();
        attempts_=completed_=unsupported_=0;diskBudget_=0;failed_=false;armed_=true;armFrame_=frame;armMs_=GetTickCount64();
        SYSTEMTIME t{};GetSystemTime(&t);wchar_t leaf[128]{};
        _snwprintf_s(leaf,_TRUNCATE,L"%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u",t.wYear,t.wMonth,t.wDay,
            t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),++serial_);
        const auto root=Config::get().logDir()+L"\\flat_weapon_footprint";directory_=root+L"\\"+leaf;
        if(!ensureDirectory(Config::get().logDir())||!ensureDirectory(root)||!ensureDirectory(directory_)){
            armed_=false;Log::get().note("flat weapon footprint: arm refused directory");return;}
        Log::get().note("flat weapon footprint: armed frame=%llu exact-VS=%016llX exact-PS=%016llX conflict-only=1 frames=2 gpu-cap=384MiB cumulative-disk-cap=384MiB expiry=900frames/30s roi=full-width-lower-1152 directory=%ls",
            static_cast<unsigned long long>(frame),static_cast<unsigned long long>(kVs),static_cast<unsigned long long>(kPs),directory_.c_str());
    }
#ifdef EDVR_WEAPON_FOOTPRINT_TEST
    void armForTest(uint64_t frame,const std::wstring& directory,const char* fixtureCase="") {
        arm(frame);directory_=directory;fixtureCase_=fixtureCase?fixtureCase:"";
        if(!ensureDirectory(directory_)){armed_=false;Log::get().note("flat weapon footprint: test directory unavailable");}
    }
    void setQueryTestMode(bool unavailable,bool pending) {
        forceQueryUnavailable_=unavailable;forceQueryPending_=pending;
    }
#endif
    bool before(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t sequence,uint64_t vs,uint64_t ps,
                bool hdrConflict,uint64_t cameraHash,const unsigned char* cameraBytes,
                uint64_t referenceCameraHash=0,const unsigned char* referenceCameraBytes=nullptr) {
        if(!armed_||vs!=kVs||ps!=kPs||!ctx)return false;
        if(frame_){if(frame_->number==frame)++frame_->drawCountExact;return false;}
        if(completed_>=2)return false;
        ++attempts_;if(!hdrConflict||!cameraBytes)return false;
        FlatComputeInternalScope internal;
        Ptr<ID3D11VertexShader> actualVs;Ptr<ID3D11PixelShader> actualPs;
        ctx->VSGetShader(&actualVs,nullptr,nullptr);ctx->PSGetShader(&actualPs,nullptr,nullptr);
#ifndef EDVR_WEAPON_FOOTPRINT_TEST
        if(lookupShaderHash(actualVs.Get())!=kVs||lookupShaderHash(actualPs.Get())!=kPs){++unsupported_;return false;}
#endif
        Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(1,&rtv,&dsv);if(!rtv||!dsv){++unsupported_;return false;}
        Ptr<ID3D11Resource> cr,dr;rtv->GetResource(&cr);dsv->GetResource(&dr);
        Ptr<ID3D11Texture2D> color,depth;
        if(!cr||!dr||FAILED(cr.As(&color))||FAILED(dr.As(&depth))){++unsupported_;return false;}
        D3D11_TEXTURE2D_DESC cd{},dd{};color->GetDesc(&cd);depth->GetDesc(&dd);
        D3D11_RENDER_TARGET_VIEW_DESC rv{};rtv->GetDesc(&rv);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dsv->GetDesc(&dv);
        Ptr<ID3D11DepthStencilState> state;UINT ref=0;ctx->OMGetDepthStencilState(&state,&ref);
        D3D11_DEPTH_STENCIL_DESC sd{};if(state)state->GetDesc(&sd);
        if(!colorBpp(cd.Format)||cd.Width<64||cd.Height<64||cd.Width!=dd.Width||cd.Height!=dd.Height||
           cd.ArraySize!=1||dd.ArraySize!=1||cd.MipLevels!=1||dd.MipLevels!=1||
           cd.SampleDesc.Count!=1||dd.SampleDesc.Count!=1||
           rv.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||rv.Texture2D.MipSlice!=0||
           dv.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||dv.Texture2D.MipSlice!=0||
           dd.Format!=DXGI_FORMAT_R32G8X24_TYPELESS||dv.Format!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT||
           !state||sd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO||!sd.StencilEnable||
           sd.StencilWriteMask!=4||((ref&sd.StencilWriteMask)!=4)||
           sd.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE){++unsupported_;return false;}
        // One frame owns all staging and scratch resources. A second frame is
        // selected only after this frame has been read back and released.
        frame_.reset(new Frame);Frame& f=*frame_;
        f.allocation=1024*1024; // conservative shader, views, CB and metadata overhead
        f.number=frame;f.startedMs=GetTickCount64();f.drawSeq=sequence;f.drawCountExact=1;
        f.width=cd.Width;f.height=cd.Height;f.roiW=cd.Width;f.roiH=(std::min)(1152u,cd.Height);
        f.roiX=0;f.roiY=cd.Height-f.roiH;
        const uint64_t pixels=uint64_t(f.roiW)*f.roiH;
        const uint64_t estimatedGpu=f.allocation+uint64_t(dd.Width)*dd.Height*8+
            pixels*8+kStages*pixels*(colorBpp(cd.Format)+8);
        if(estimatedGpu>kCap){
            Log::get().note("flat weapon footprint: frame=%llu refused gpu-cap estimated=%llu limit=%llu source=%ux%u color-bpp=%u",
                static_cast<unsigned long long>(frame),static_cast<unsigned long long>(estimatedGpu),
                static_cast<unsigned long long>(kCap),cd.Width,cd.Height,colorBpp(cd.Format));
            frame_.reset();failed_=true;cancel("gpu-cap");return false;
        }
        const uint64_t estimatedOutput=uint64_t(f.roiW)*f.roiH*(colorBpp(cd.Format)+5)*kStages+16384;
        if(estimatedOutput>kCap||diskBudget_>kCap-estimatedOutput){
            Log::get().note("flat weapon footprint: frame=%llu refused disk-cap estimated=%llu previous=%llu",
                static_cast<unsigned long long>(frame),static_cast<unsigned long long>(estimatedOutput),static_cast<unsigned long long>(diskBudget_));
            frame_.reset();failed_=true;cancel("disk-cap");return false;
        }
        f.colorFormat=cd.Format;f.depthFormat=dd.Format;f.depthViewFormat=dv.Format;
        for(unsigned i=0;i<kStages;++i){
            Stage& stage=f.stages[i];stage.name=kNames[i];
            stage.color.format=cd.Format;stage.color.rowBytes=f.roiW*colorBpp(cd.Format);
            stage.depth.format=DXGI_FORMAT_R32_FLOAT;stage.depth.rowBytes=f.roiW*4;
            stage.stencil.format=DXGI_FORMAT_R8_UINT;stage.stencil.rowBytes=f.roiW;
        }
        f.stencilRef=ref;f.stencilReadMask=sd.StencilReadMask;f.stencilWriteMask=sd.StencilWriteMask;f.depthWrite=sd.DepthWriteMask;
        f.depthEnable=sd.DepthEnable;f.depthFunc=sd.DepthFunc;f.stencilEnable=sd.StencilEnable;f.dsvFlags=dv.Flags;
        f.frontFunc=sd.FrontFace.StencilFunc;f.frontPass=sd.FrontFace.StencilPassOp;
        f.frontFail=sd.FrontFace.StencilFailOp;f.frontDepthFail=sd.FrontFace.StencilDepthFailOp;
        f.backFunc=sd.BackFace.StencilFunc;f.backPass=sd.BackFace.StencilPassOp;
        f.backFail=sd.BackFace.StencilFailOp;f.backDepthFail=sd.BackFace.StencilDepthFailOp;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&viewportCount,viewports);f.viewportCount=viewportCount;
        if(viewportCount)f.viewport=viewports[0];
        f.cameraHash=cameraHash;std::memcpy(f.camera,cameraBytes,sizeof(f.camera));f.hdrConflict=true;
        f.referenceCameraHash=referenceCameraHash;
        if(referenceCameraBytes)std::memcpy(f.referenceCamera,referenceCameraBytes,sizeof(f.referenceCamera));
        f.color=color;f.depth=depth;f.colorResource=cr;f.depthResource=dr;f.rtv=rtv;f.dsv=dsv;
        if(!resources(ctx,f)){
            Log::get().note("flat weapon footprint: frame=%llu status=failed reason=resource-or-shader-unavailable allocated=%llu; game draw unchanged",
                static_cast<unsigned long long>(frame),static_cast<unsigned long long>(f.allocation));
            frame_.reset();++unsupported_;failed_=true;return false;
        }
        snapshot(ctx,f,0,sequence);
        return true;
    }
    void after(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t sequence) {
        if(!tracking(frame)||frame_->drawSeq!=sequence||frame_->afterDone)return;
        frame_->afterDone=true;snapshot(ctx,*frame_,1,sequence);
    }
    void beginActualDraw(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t sequence,char kind,
                         uint32_t count,uint32_t start,int32_t base,uint32_t instances,uint32_t startInstance,
                         ID3D11Buffer* indirectBuffer=nullptr,uint32_t indirectOffset=0) {
        if(!tracking(frame)||!ctx||frame_->drawSeq!=sequence||frame_->visibility.began||frame_->ended)return;
        FlatComputeInternalScope internal;
        Frame& f=*frame_;auto& v=f.visibility;
        captureDrawState(ctx,f.drawState,kind,count,start,base,instances,startInstance,indirectBuffer,indirectOffset);
        v.began=true;v.beginSeq=sequence;v.status="unavailable";v.reason="end-not-seen";
#ifdef EDVR_WEAPON_FOOTPRINT_TEST
        if(forceQueryUnavailable_){v.occlusion.reason=v.pipeline.reason="test-forced-unavailable";return;}
#endif
        queryBegin(ctx,v.occlusion,D3D11_QUERY_OCCLUSION);
        queryBegin(ctx,v.pipeline,D3D11_QUERY_PIPELINE_STATISTICS);
    }
    void endActualDraw(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t sequence,bool explicitEnd=true) {
        if(!tracking(frame)||!ctx||frame_->drawSeq!=sequence)return;
        auto& v=frame_->visibility;if(!v.began||v.ended)return;
        FlatComputeInternalScope internal;
        if(v.occlusion.query)ctx->End(v.occlusion.query.Get());
        if(v.pipeline.query)ctx->End(v.pipeline.query.Get());
        v.ended=true;v.endSeq=sequence;v.valid=explicitEnd&&v.beginSeq==sequence;
        v.status=v.valid?"complete":"failed";v.reason=v.valid?"":"destructor-fallback-or-sequence-mismatch";
    }
    void confirm(uint64_t frame,uint32_t sequence,uint32_t firstBadSeq,const char* cause) {
        if(!tracking(frame)||frame_->drawSeq!=sequence)return;
        frame_->firstBadSeq=firstBadSeq;frame_->firstBadCause=cause?cause:"unavailable";
    }
    void consumer(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t sequence,const void* source,
                  uint64_t vs=0,uint64_t ps=0,uint32_t slot=~0u,const char* verdict="unknown",uint32_t firstBadSeq=0) {
        if(!tracking(frame)||frame_->consumerFound||source!=frame_->colorResource.Get())return;
        frame_->consumerFound=true;frame_->consumerSeq=sequence;frame_->consumerVs=vs;frame_->consumerPs=ps;
        frame_->consumerSlot=slot;frame_->consumerVerdict=verdict;frame_->firstBadSeq=firstBadSeq;
        snapshot(ctx,*frame_,2,sequence);
    }
    void clear(ID3D11DepthStencilView* dsv,UINT flags,UINT stencil,uint32_t sequence) {
        if(!armed_||!frame_||frame_->ended||!dsv||!(flags&D3D11_CLEAR_STENCIL))return;
        if(dsv!=frame_->dsv.Get()){
            Ptr<ID3D11Resource> resource;dsv->GetResource(&resource);
            if(resource.Get()!=frame_->depthResource.Get())return;
        }
        if(frame_->clears.size()<64)frame_->clears.push_back({sequence,flags,stencil,dsv});else ++frame_->clearOverflow;
    }
    void beforePresent(ID3D11DeviceContext* ctx,uint64_t completedFrame,uint32_t sequence) {
        if(!tracking(completedFrame)||frame_->ended)return;
        frame_->ended=true;snapshot(ctx,*frame_,3,sequence);
    }
    void present(ID3D11DeviceContext* ctx,uint64_t completedFrame,uint32_t sequence) {
        if(!armed_)return;
        const uint64_t now=GetTickCount64();
        if(frame_&&frame_->ended&&!ctx){cancel("readback-context-unavailable");return;}
        if(frame_&&frame_->ended){
            bool waiting=false;
            for(unsigned i=0;i<kStages;++i){auto& s=frame_->stages[i];if(std::strcmp(s.status,"queued"))continue;
                bool stageWaiting=false;
                Plane* planes[]={&s.color,&s.depth,&s.stencil};const char* names[]={"color","depth","stencil"};
                for(unsigned j=0;j<3;++j){Plane& p=*planes[j];if(std::strcmp(p.status,"queued"))continue;
                    p.file="frame_"+std::to_string(frame_->number)+"_"+kNames[i]+"_"+names[j]+".bin";
                    const std::wstring path=directory_+L"\\"+std::wstring(p.file.begin(),p.file.end());
                    if(!readPlane(ctx,p,path,frame_->roiW,frame_->roiH,j==2)){waiting=true;stageWaiting=true;}
                }
                if(!stageWaiting){s.status=std::strcmp(s.color.status,"complete")||std::strcmp(s.depth.status,"complete")||
                    std::strcmp(s.stencil.status,"complete")?"failed":"complete";s.reason=std::strcmp(s.status,"complete")?"readback-failed":"";}
            }
            auto& v=frame_->visibility;
            if(v.ended){
                ++v.polls;
                bool forcePending=false;
#ifdef EDVR_WEAPON_FOOTPRINT_TEST
                forcePending=forceQueryPending_;
#endif
                queryPoll(ctx,v.occlusion,false,forcePending);
                queryPoll(ctx,v.pipeline,true,forcePending);
                if(v.polls>=120||now-frame_->startedMs>=5000){
                    for(QueryResult* q:{&v.occlusion,&v.pipeline})if(!std::strcmp(q->status,"pending")){
                        q->status="timeout";q->reason="get-data-timeout";
                    }
                }
            }
            const bool queryWaiting=!std::strcmp(v.occlusion.status,"pending")||!std::strcmp(v.pipeline.status,"pending");
            const bool timedOut=now-frame_->startedMs>=5000||v.polls>=120;
            if((!waiting&&!queryWaiting)||timedOut){finish(*frame_,waiting?"readback-timeout":nullptr);frame_.reset();}
        }
        if(completed_>=2){cancel(failed_?"complete-with-failures":"complete");return;}
        if(completedFrame>=armFrame_+900||now-armMs_>=30000)cancel(completed_?"arm-expired-after-sample":"arm-expired-no-match");
    }
};
} // namespace edvr
