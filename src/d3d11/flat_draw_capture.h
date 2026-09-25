#pragma once
// F10-only, passive draw chronology for a few flat scene pixels. No shader or
// game binding is changed. All copies are queued on the owner context and
// read with DO_NOT_WAIT at later Presents.
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#include "../common/config.h"
#include "../common/log.h"

namespace edvr {
bool captureFlatProbeShader(char stage,uint64_t hash);
class FlatDrawCapture {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    static constexpr unsigned kPoints=8, kWindow=16, kDrawCap=512, kFrames=2;
    static constexpr unsigned kChunkSlots=8192, kChunkCols=64;
    static constexpr uint64_t kByteCap=256ull*1024*1024;
    inline static constexpr float uv_[kPoints][2]={{.08f,.70f},{.91f,.84f},{.50f,.37f},{.62f,.60f},
                                             {.82f,.67f},{.35f,.40f},{.60f,.45f},{.90f,.74f}};
    struct Copy {
        unsigned rt=0, point=0, chunk=0, slot=0, x0=0,y0=0, format=0,bpp=0;
        bool after=false; const char* status="queued"; uint64_t offset=0; unsigned length=0;
    };
    struct Cb {
        unsigned slot=0, bytes=0, first=0, count=0; const void* resource=nullptr;
        Ptr<ID3D11Buffer> stage; const char* status="unavailable"; uint64_t offset=0; unsigned length=0;
    };
    struct Pool {
        Ptr<ID3D11Buffer> stage; const void* resource=nullptr; const void* view=nullptr;
        unsigned bytes=0,stride=0,first=0,elements=0; const char* status="not-requested",*reason="";
        uint64_t offset=0; unsigned length=0;
    };
    struct Rt {
        Ptr<ID3D11Texture2D> texture; const void* resource=nullptr; const void* view=nullptr;
        unsigned format=0, viewFormat=0, width=0,height=0; bool valid=false;
    };
    struct Draw {
        unsigned q=0, instances=0, topology=0, indexFormat=0, indexOffset=0;
        char kind='?'; unsigned count=0,start=0,startInstance=0;int32_t base=0;
        uint64_t vs=0,ps=0; const void* dsv=nullptr; const void* vsObject=nullptr; const void* psObject=nullptr;
        const void* depthResource=nullptr;unsigned depthFormat=0,depthViewFormat=0;
        bool vsBytes=false,psBytes=false;
        const void* layout=nullptr; const void* ib=nullptr;
        const void* vb[4]{}; unsigned vbStride[4]{},vbOffset[4]{};
        unsigned depthEnable=0,depthWrite=0,depthFunc=0,stencilRef=0;
        int depthBias=0; float slopeBias=0,biasClamp=0;
        unsigned viewportCount=0; D3D11_VIEWPORT viewport{};
        Rt rt[4], motionSlot, motionDepth; Cb cb[3]; Pool pool; std::vector<Copy> copies;
        bool motionCandidate=false,motionExpected=false, motionBefore=false; const char* motionStatus="not-requested";
        const void* motionPool=nullptr; const void* motionPoolView=nullptr;
        bool before=false; const char* reason="";
    };
    struct Chunk { Ptr<ID3D11Texture2D> stage; unsigned format=0,bpp=0,used=0; bool mapped=false;
        uint64_t allocation=0; };
    struct Frame {
        uint64_t number=0, startMs=0, bytesAllocated=0; unsigned width=0,height=0, drawsSeen=0, overflows=0, refusals=0,motionRefusals=0;
        const void* priorDepth=nullptr; const void* selectedDepth=nullptr; const void* selectedHdr=nullptr;
        bool qualified=false, identity=false; const char* reason="not-qualified";
        std::vector<Draw> draws; std::vector<Chunk> chunks;
        Ptr<ID3D11Texture2D> depthMirror; unsigned depthMirrorFormat=0;
        FILE* pixelFile=nullptr;FILE* cbFile=nullptr;FILE* poolFile=nullptr;
        uint64_t pixelWritten=0,cbWritten=0,poolWritten=0;
        unsigned poolSnapshots=0,poolByFamily[2]{};
        unsigned readbackFrames=0; bool readbackError=false,motionReadbackError=false;
    };
    std::unique_ptr<Frame> current_;
    std::deque<std::unique_ptr<Frame>> pending_;
    std::wstring directory_;
    uint64_t armMs_=0,armFrame_=0, allocated_=0; unsigned accepted_=0,serial_=0;
    unsigned unqualified_=0;
    std::map<std::pair<char,uint64_t>,bool> shaderBytes_;
    bool armed_=false,failed_=false;
    static unsigned bpp(DXGI_FORMAT f) {
        switch(f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS: return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS: return 8;
        default:return 0;
        }
    }
    static std::string hex(const void* p) { char s[32]{};std::snprintf(s,sizeof(s),"0x%llX",(unsigned long long)(uintptr_t)p);return s; }
    static std::string hash(uint64_t h) { char s[24]{};std::snprintf(s,sizeof(s),"%016llX",(unsigned long long)h);return s; }
    static bool write(const std::wstring& path,const void* p,size_t n) {
        FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"wb")||!f)return false;
        const bool ok=!n || std::fwrite(p,1,n,f)==n;return std::fclose(f)==0&&ok;
    }
    static unsigned center(float u,unsigned n) {return std::min(n-1u,static_cast<unsigned>(u*n));}
    static unsigned origin(unsigned c,unsigned n) {return std::min(n-kWindow,c<kWindow/2?0:c-kWindow/2);}
    bool reserve(Frame& f,uint64_t bytes,bool motion=false) {
        if(bytes>kByteCap || allocated_>kByteCap-bytes){if(motion)++f.motionRefusals;else ++f.overflows;return false;}
        allocated_+=bytes;f.bytesAllocated+=bytes;return true;
    }
    bool stageCopy(ID3D11DeviceContext* ctx,Frame& f,Draw& d,unsigned rt,unsigned pt,bool after) {
        const Rt& r=rt==6?d.motionSlot:rt==7?d.motionDepth:d.rt[rt];
        Copy c;c.rt=rt;c.point=pt;c.after=after;c.format=r.format;c.bpp=bpp(static_cast<DXGI_FORMAT>(r.format));
        if(!r.valid){c.status="unavailable";d.copies.push_back(c);if(rt>=6)++f.motionRefusals;else ++f.refusals;return false;}
        c.x0=origin(center(uv_[pt][0],r.width),r.width);c.y0=origin(center(uv_[pt][1],r.height),r.height);
        unsigned idx=0;
        for(;idx<f.chunks.size();++idx)if(f.chunks[idx].format==r.format && f.chunks[idx].used<kChunkSlots)break;
        if(idx==f.chunks.size()) {
            const uint64_t n=uint64_t(kChunkCols*kWindow)*(kChunkSlots/kChunkCols*kWindow)*c.bpp;
            if(!reserve(f,n,rt>=6)){c.status="unavailable";d.copies.push_back(c);return false;}
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            D3D11_TEXTURE2D_DESC td{};td.Width=kChunkCols*kWindow;td.Height=kChunkSlots/kChunkCols*kWindow;
            td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=static_cast<DXGI_FORMAT>(r.format);
            td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Chunk ch;ch.format=r.format;ch.bpp=c.bpp;ch.allocation=n;
            if(FAILED(dev->CreateTexture2D(&td,nullptr,&ch.stage))) {allocated_-=n;f.bytesAllocated-=n;c.status="unavailable";d.copies.push_back(c);if(rt>=6)++f.motionRefusals;else ++f.refusals;return false;}
            f.chunks.push_back(std::move(ch));
        }
        Chunk& ch=f.chunks[idx];c.chunk=idx;c.slot=ch.used++;
        const D3D11_BOX box{c.x0,c.y0,0,c.x0+kWindow,c.y0+kWindow,1};
        ID3D11Texture2D* source=rt==7?f.depthMirror.Get():r.texture.Get();
        if(!source){c.status="unavailable";d.copies.push_back(c);if(rt>=6)++f.motionRefusals;else ++f.refusals;return false;}
        ctx->CopySubresourceRegion(ch.stage.Get(),0,(c.slot%kChunkCols)*kWindow,(c.slot/kChunkCols)*kWindow,0,source,0,&box);
        d.copies.push_back(c);return true;
    }
    void stageCb(ID3D11DeviceContext* ctx,Frame& f,Draw& d) {
        ID3D11Buffer* raw[3]{};ctx->VSGetConstantBuffers(0,3,raw);
        Ptr<ID3D11DeviceContext1> c1;ctx->QueryInterface(IID_PPV_ARGS(&c1));
        UINT first[3]{},count[3]{};
        if(c1) {ID3D11Buffer* again[3]{};c1->VSGetConstantBuffers1(0,3,again,first,count);for(auto* b:again)if(b)b->Release();}
        for(unsigned i=0;i<3;++i) {
            Cb& c=d.cb[i];c.slot=i;c.resource=raw[i];c.first=first[i];c.count=count[i];
            Ptr<ID3D11Buffer> source;source.Attach(raw[i]);if(!source)continue;
            D3D11_BUFFER_DESC bd{};source->GetDesc(&bd);c.bytes=bd.ByteWidth;
            if(!c1)c.count=(bd.ByteWidth+15)/16;
            if(!bd.ByteWidth || bd.ByteWidth>65536 || !reserve(f,bd.ByteWidth)){++f.refusals;continue;}
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC sd=bd;sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=sd.MiscFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;sd.StructureByteStride=0;
            if(FAILED(dev->CreateBuffer(&sd,nullptr,&c.stage))){allocated_-=bd.ByteWidth;f.bytesAllocated-=bd.ByteWidth;++f.refusals;continue;}
            ctx->CopyResource(c.stage.Get(),source.Get());c.status="queued";
        }
    }
    void stagePool(ID3D11DeviceContext* ctx,Frame& f,Draw& d,ID3D11ShaderResourceView* srv) {
        Pool& p=d.pool;p.status="unavailable";
        if(!srv){p.reason="t33-unbound";++f.motionRefusals;return;}
        p.view=srv;Ptr<ID3D11Resource> resource;srv->GetResource(&resource);p.resource=resource.Get();
        Ptr<ID3D11Buffer> buffer;if(!resource||FAILED(resource.As(&buffer))){p.reason="t33-not-buffer";++f.motionRefusals;return;}
        D3D11_BUFFER_DESC bd{};buffer->GetDesc(&bd);p.bytes=bd.ByteWidth;p.stride=bd.StructureByteStride;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};srv->GetDesc(&vd);
        if(vd.ViewDimension==D3D11_SRV_DIMENSION_BUFFER){p.first=vd.Buffer.FirstElement;p.elements=vd.Buffer.NumElements;}
        else if(vd.ViewDimension==D3D11_SRV_DIMENSION_BUFFEREX){p.first=vd.BufferEx.FirstElement;p.elements=vd.BufferEx.NumElements;}
        else {p.reason="t33-not-buffer-view";++f.motionRefusals;return;}
        if(!p.bytes||p.bytes>4u*1024*1024||p.stride!=336||
           (uint64_t(p.first)+p.elements)*p.stride>p.bytes){p.reason="t33-range-or-size";++f.motionRefusals;return;}
        const unsigned family=d.vs==0x66DE2CADB1F4AE6Bull?0:1;
        if(f.poolSnapshots>=16||f.poolByFamily[family]>=8){p.reason="pool-snapshot-cap";++f.motionRefusals;return;}
        if(!reserve(f,p.bytes,true)){p.reason="byte-cap";return;}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);D3D11_BUFFER_DESC sd=bd;
        sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=sd.MiscFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;sd.StructureByteStride=0;
        if(FAILED(dev->CreateBuffer(&sd,nullptr,&p.stage))){allocated_-=p.bytes;f.bytesAllocated-=p.bytes;p.reason="create-staging";++f.motionRefusals;return;}
        ctx->CopyResource(p.stage.Get(),buffer.Get());p.status="queued";++f.poolSnapshots;++f.poolByFamily[family];
    }
    bool stageDepthMirror(ID3D11DeviceContext* ctx,Frame& f,Draw& d) {
        if(!d.motionDepth.valid||d.motionDepth.resource!=f.priorDepth)return false;
        if(!f.depthMirror){
            D3D11_TEXTURE2D_DESC td{};d.motionDepth.texture->GetDesc(&td);
            const uint64_t bytes=uint64_t(td.Width)*td.Height*bpp(td.Format);
            if(!reserve(f,bytes,true))return false;
            td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=td.CPUAccessFlags=td.MiscFlags=0;
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            if(FAILED(dev->CreateTexture2D(&td,nullptr,&f.depthMirror))){allocated_-=bytes;f.bytesAllocated-=bytes;return false;}
            f.depthMirrorFormat=td.Format;
        }
        if(f.depthMirrorFormat!=d.motionDepth.format)return false;
        // D3D11 forbids a boxed copy directly from a depth-stencil resource.
        // Copy its whole subresource first, then box-copy the mirror's pixels.
        ctx->CopyResource(f.depthMirror.Get(),d.motionDepth.texture.Get());
        return true;
    }
    static void metadata(ID3D11DeviceContext* ctx,Draw& d) {
        Ptr<ID3D11InputLayout> il;ctx->IAGetInputLayout(&il);d.layout=il.Get();
        ID3D11Buffer* vb[4]{};UINT stride[4]{},offset[4]{};ctx->IAGetVertexBuffers(0,4,vb,stride,offset);
        for(unsigned i=0;i<4;++i){d.vb[i]=vb[i];d.vbStride[i]=stride[i];d.vbOffset[i]=offset[i];if(vb[i])vb[i]->Release();}
        Ptr<ID3D11Buffer> ib;DXGI_FORMAT indexFormat=DXGI_FORMAT_UNKNOWN;UINT indexOffset=0;
        ctx->IAGetIndexBuffer(&ib,&indexFormat,&indexOffset);d.ib=ib.Get();d.indexFormat=indexFormat;d.indexOffset=indexOffset;
        D3D11_PRIMITIVE_TOPOLOGY topo{};ctx->IAGetPrimitiveTopology(&topo);d.topology=topo;
        d.depthEnable=1;d.depthWrite=D3D11_DEPTH_WRITE_MASK_ALL;d.depthFunc=D3D11_COMPARISON_LESS;
        Ptr<ID3D11DepthStencilState> ds;UINT ref=0;ctx->OMGetDepthStencilState(&ds,&ref);d.stencilRef=ref;
        if(ds){D3D11_DEPTH_STENCIL_DESC x{};ds->GetDesc(&x);d.depthEnable=x.DepthEnable;d.depthWrite=x.DepthWriteMask;d.depthFunc=x.DepthFunc;}
        Ptr<ID3D11RasterizerState> rs;ctx->RSGetState(&rs);
        if(rs){D3D11_RASTERIZER_DESC x{};rs->GetDesc(&x);d.depthBias=x.DepthBias;d.slopeBias=x.SlopeScaledDepthBias;d.biasClamp=x.DepthBiasClamp;}
        UINT n=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;D3D11_VIEWPORT all[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        ctx->RSGetViewports(&n,all);d.viewportCount=n;if(n)d.viewport=all[0];
    }
    bool readback(ID3D11DeviceContext* ctx,Frame& f) {
        const std::wstring stem=directory_+L"\\frame_"+std::to_wstring(f.number);
        if(!f.pixelFile&&_wfopen_s(&f.pixelFile,(stem+L"_pixels.bin").c_str(),L"wb")){f.readbackError=true;return true;}
        if(!f.cbFile&&_wfopen_s(&f.cbFile,(stem+L"_cb.bin").c_str(),L"wb")){f.readbackError=true;return true;}
        if(!f.poolFile&&_wfopen_s(&f.poolFile,(stem+L"_pool.bin").c_str(),L"wb")){
            f.motionReadbackError=true;
            for(auto& d:f.draws)if(!std::strcmp(d.pool.status,"queued")){d.pool.status="gpu_error";d.pool.reason="open-failed";}
        }
        for(unsigned j=0;j<f.chunks.size();++j) {
            Chunk& ch=f.chunks[j];if(ch.mapped)continue;
            D3D11_MAPPED_SUBRESOURCE map{};const HRESULT hr=ctx->Map(ch.stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return false;
            if(FAILED(hr)){
                for(auto& d:f.draws)for(auto& c:d.copies)if(c.chunk==j&&!std::strcmp(c.status,"queued")){
                    c.status="gpu_error";if(c.rt>=6)f.motionReadbackError=true;else f.readbackError=true;}
                ch.mapped=true;continue;
            }
            for(auto& d:f.draws)for(auto& c:d.copies)if(c.chunk==j&&!std::strcmp(c.status,"queued")) {
                c.offset=f.pixelWritten;c.length=kWindow*kWindow*c.bpp;
                const unsigned sx=(c.slot%kChunkCols)*kWindow,sy=(c.slot/kChunkCols)*kWindow;
                bool ok=true;
                for(unsigned row=0;row<kWindow;++row) {
                    const auto* p=static_cast<const uint8_t*>(map.pData)+size_t(sy+row)*map.RowPitch+sx*c.bpp;
                    if(std::fwrite(p,1,kWindow*c.bpp,f.pixelFile)!=kWindow*c.bpp){ok=false;break;}
                }
                if(ok){f.pixelWritten+=c.length;c.status="ok";}else{c.status="gpu_error";c.length=0;
                    if(c.rt>=6)f.motionReadbackError=true;else f.readbackError=true;}
            }
            ctx->Unmap(ch.stage.Get(),0);ch.mapped=true;ch.stage.Reset();
            allocated_-=ch.allocation;f.bytesAllocated-=ch.allocation;ch.allocation=0;
        }
        for(auto& d:f.draws)for(auto& c:d.cb)if(c.stage && !std::strcmp(c.status,"queued")) {
            D3D11_MAPPED_SUBRESOURCE map{};const HRESULT hr=ctx->Map(c.stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return false;
            if(FAILED(hr)){c.status="gpu_error";f.readbackError=true;continue;}
            c.offset=f.cbWritten;c.length=c.bytes;auto* p=static_cast<const uint8_t*>(map.pData);
            const bool ok=std::fwrite(p,1,c.bytes,f.cbFile)==c.bytes;
            ctx->Unmap(c.stage.Get(),0);c.stage.Reset();
            if(ok){f.cbWritten+=c.bytes;c.status="ok";}else{c.status="gpu_error";c.length=0;f.readbackError=true;}
            allocated_-=c.bytes;f.bytesAllocated-=c.bytes;
        }
        for(auto& d:f.draws){Pool& p=d.pool;if(!f.poolFile||!p.stage||std::strcmp(p.status,"queued"))continue;
            D3D11_MAPPED_SUBRESOURCE map{};const HRESULT hr=ctx->Map(p.stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return false;
            if(FAILED(hr)){p.status="gpu_error";p.reason="map-failed";f.motionReadbackError=true;continue;}
            p.offset=f.poolWritten;p.length=p.bytes;
            const bool ok=std::fwrite(map.pData,1,p.bytes,f.poolFile)==p.bytes;
            ctx->Unmap(p.stage.Get(),0);p.stage.Reset();
            if(ok){f.poolWritten+=p.bytes;p.status="ok";}else{p.status="gpu_error";p.reason="write-failed";p.length=0;f.motionReadbackError=true;}
            allocated_-=p.bytes;f.bytesAllocated-=p.bytes;
        }
        return true;
    }
    static std::string json(Frame& f,const char* status,bool motionComplete,unsigned motionDraws) {
        std::ostringstream o;o<<"{\"schema\":2,\"status\":\""<<status<<"\",\"reason\":\""<<f.reason
          <<"\",\"frame\":"<<f.number<<",\"render_width\":"<<f.width<<",\"render_height\":"<<f.height
          <<",\"prior_depth\":\""<<hex(f.priorDepth)<<"\",\"selected_depth\":\""<<hex(f.selectedDepth)
          <<"\",\"selected_hdr\":\""<<hex(f.selectedHdr)<<"\",\"qualified\":"<<(f.qualified?"true":"false")
          <<",\"identity_match\":"<<(f.identity?"true":"false")<<",\"draw_cap\":"<<kDrawCap
           <<",\"draws_seen\":"<<f.drawsSeen<<",\"draws_recorded\":"<<f.draws.size()
           <<",\"overflow\":"<<f.overflows<<",\"refusals\":"<<f.refusals
           <<",\"motion_draws\":"<<motionDraws<<",\"motion_refusals\":"<<f.motionRefusals
           <<",\"motion_complete\":"<<(motionComplete?"true":"false")
          <<",\"pixels_file\":\"frame_"<<f.number<<"_pixels.bin\",\"pixels_bytes\":"<<f.pixelWritten
           <<",\"cb_file\":\"frame_"<<f.number<<"_cb.bin\",\"cb_bytes\":"<<f.cbWritten
           <<",\"pool_file\":\"frame_"<<f.number<<"_pool.bin\",\"pool_bytes\":"<<f.poolWritten<<",\"points\":[";
        for(unsigned i=0;i<kPoints;++i){if(i)o<<',';o<<"{\"u\":"<<uv_[i][0]<<",\"v\":"<<uv_[i][1]<<",\"x\":"<<center(uv_[i][0],f.width)<<",\"y\":"<<center(uv_[i][1],f.height)<<"}";}
        o<<"],\"draws\":[";
        for(size_t i=0;i<f.draws.size();++i){const Draw& d=f.draws[i];if(i)o<<',';
            o<<"{\"q\":"<<d.q<<",\"instances\":"<<d.instances<<",\"args_available\":"<<(d.kind=='?'?"false":"true")
             <<",\"kind\":\""<<d.kind<<"\",\"count\":"<<d.count<<",\"start\":"<<d.start<<",\"base\":"<<d.base<<",\"start_instance\":"<<d.startInstance
             <<",\"vs\":\""<<hash(d.vs)
             <<"\",\"ps\":\""<<hash(d.ps)<<"\",\"vs_object\":\""<<hex(d.vsObject)<<"\",\"ps_object\":\""<<hex(d.psObject)
             <<"\",\"shader_bytes\":{\"vs\":"<<(d.vsBytes?"true":"false")<<",\"ps\":"<<(d.psBytes?"true":"false")
             <<"},\"dsv\":\""<<hex(d.dsv)<<"\",\"depth_resource\":\""<<hex(d.depthResource)<<"\",\"depth_format\":"<<d.depthFormat
             <<",\"depth_view_format\":"<<d.depthViewFormat<<",\"topology\":"<<d.topology<<",\"depth_state\":{\"enable\":"<<d.depthEnable
             <<",\"write_mask\":"<<d.depthWrite<<",\"func\":"<<d.depthFunc<<",\"stencil_ref\":"<<d.stencilRef
             <<"},\"raster\":{\"depth_bias\":"<<d.depthBias<<",\"slope_bias\":"<<d.slopeBias<<",\"bias_clamp\":"<<d.biasClamp
             <<"},\"viewport\":{\"count\":"<<d.viewportCount<<",\"x\":"<<d.viewport.TopLeftX<<",\"y\":"<<d.viewport.TopLeftY
             <<",\"w\":"<<d.viewport.Width<<",\"h\":"<<d.viewport.Height<<",\"min\":"<<d.viewport.MinDepth<<",\"max\":"<<d.viewport.MaxDepth
             <<"},\"ia\":{\"layout\":\""<<hex(d.layout)<<"\",\"ib\":\""<<hex(d.ib)<<"\",\"index_format\":"<<d.indexFormat
             <<",\"index_offset\":"<<d.indexOffset<<",\"vb\":[";
            for(unsigned v=0;v<4;++v){if(v)o<<',';o<<"{\"resource\":\""<<hex(d.vb[v])<<"\",\"stride\":"<<d.vbStride[v]<<",\"offset\":"<<d.vbOffset[v]<<"}";}o<<"]},\"rt\":[";
            for(unsigned r=0;r<4;++r){if(r)o<<',';o<<"{\"index\":"<<r<<",\"resource\":\""<<hex(d.rt[r].resource)<<"\",\"view\":\""<<hex(d.rt[r].view)
                 <<"\",\"format\":"<<d.rt[r].format<<",\"view_format\":"<<d.rt[r].viewFormat<<",\"width\":"<<d.rt[r].width<<",\"height\":"<<d.rt[r].height
                 <<",\"valid\":"<<(d.rt[r].valid?"true":"false")<<"}";}
            const auto motionTarget=[&](const Rt& r){o<<"{\"resource\":\""<<hex(r.resource)<<"\",\"view\":\""<<hex(r.view)
                <<"\",\"format\":"<<r.format<<",\"view_format\":"<<r.viewFormat<<",\"width\":"<<r.width
                <<",\"height\":"<<r.height<<",\"valid\":"<<(r.valid?"true":"false")<<"}";};
            o<<"],\"motion\":{\"candidate\":"<<(d.motionCandidate?"true":"false")
             <<",\"expected\":"<<(d.motionExpected?"true":"false")<<",\"status\":\""<<d.motionStatus
             <<"\",\"pool_resource\":\""<<hex(d.motionPool)<<"\",\"pool_view\":\""<<hex(d.motionPoolView)<<"\",\"slot\":";
            motionTarget(d.motionSlot);o<<",\"depth\":";motionTarget(d.motionDepth);
            const Pool& pool=d.pool;
            o<<",\"pool\":{\"status\":\""<<pool.status<<"\",\"reason\":\""<<pool.reason
             <<"\",\"resource\":\""<<hex(pool.resource)<<"\",\"view\":\""<<hex(pool.view)
             <<"\",\"byte_width\":"<<pool.bytes<<",\"stride\":"<<pool.stride<<",\"first_element\":"<<pool.first
             <<",\"num_elements\":"<<pool.elements<<",\"offset\":"<<pool.offset<<",\"length\":"<<pool.length
             <<"}},\"cb\":[";
            for(unsigned b=0;b<3;++b){if(b)o<<',';const Cb& c=d.cb[b];o<<"{\"slot\":"<<b<<",\"resource\":\""<<hex(c.resource)<<"\",\"byte_width\":"<<c.bytes
                <<",\"first_constant\":"<<c.first<<",\"constant_count\":"<<c.count<<",\"status\":\""<<c.status<<"\",\"offset\":"<<c.offset<<",\"length\":"<<c.length<<"}";}
            o<<"],\"copies\":[";for(size_t j=0;j<d.copies.size();++j){const Copy& c=d.copies[j];if(j)o<<',';
                o<<"{\"rt_index\":"<<c.rt<<",\"point\":"<<c.point<<",\"phase\":\""<<(c.after?"after":"before")
                 <<"\",\"status\":\""<<c.status<<"\",\"format\":"<<c.format<<",\"bpp\":"<<c.bpp<<",\"x0\":"<<c.x0<<",\"y0\":"<<c.y0
                 <<",\"offset\":"<<c.offset<<",\"length\":"<<c.length<<"}";}o<<"]}";
        }o<<"]}";return o.str();
    }
    void finish(Frame& f,bool timedOut) {
        if(f.pixelFile){if(std::fclose(f.pixelFile)!=0)f.readbackError=true;f.pixelFile=nullptr;}
        if(f.cbFile){if(std::fclose(f.cbFile)!=0)f.readbackError=true;f.cbFile=nullptr;}
        if(f.poolFile){if(std::fclose(f.poolFile)!=0)f.motionReadbackError=true;f.poolFile=nullptr;}
        const char* unfinished=timedOut?(!f.qualified?"skipped":"timeout"):"gpu_error";
        for(auto& d:f.draws){for(auto& c:d.copies)if(!std::strcmp(c.status,"queued")){c.status=unfinished;c.length=0;}
            for(auto& c:d.cb)if(!std::strcmp(c.status,"queued")){c.status=unfinished;c.length=0;}
            if(!std::strcmp(d.pool.status,"queued")){d.pool.status=unfinished;d.pool.length=0;}}
        bool all=f.qualified&&f.identity&&!f.draws.empty()&&!f.overflows&&!f.refusals&&!f.readbackError&&!timedOut&&f.drawsSeen<=kDrawCap;
        bool motionComplete=f.qualified&&f.identity&&!f.motionRefusals&&!f.motionReadbackError&&!timedOut;unsigned motionDraws=0;
        for(auto& d:f.draws){for(auto& c:d.copies)if(c.rt<4&&std::strcmp(c.status,"ok"))all=false;
            for(auto& c:d.cb)if(c.resource&&std::strcmp(c.status,"ok"))all=false;
            if(d.motionCandidate){++motionDraws;if(std::strcmp(d.motionStatus,"captured")||std::strcmp(d.pool.status,"ok"))motionComplete=false;
                for(auto& c:d.copies)if(c.rt>=6&&std::strcmp(c.status,"ok"))motionComplete=false;}}
        if(!motionDraws)motionComplete=false;
        const char* status=all?"complete":f.qualified?"partial":"failed";
        if(!std::strcmp(f.reason,"qualified"))f.reason=timedOut?"readback-timeout":all?"qualified":"copy-or-cap-refused";
        const std::wstring stem=directory_+L"\\frame_"+std::to_wstring(f.number);
        const std::string body=json(f,status,motionComplete,motionDraws);
        FILE* empty=nullptr;
        if(_wfopen_s(&empty,(stem+L"_pixels.bin").c_str(),L"ab")==0&&empty)std::fclose(empty);
        empty=nullptr;if(_wfopen_s(&empty,(stem+L"_cb.bin").c_str(),L"ab")==0&&empty)std::fclose(empty);
        empty=nullptr;if(_wfopen_s(&empty,(stem+L"_pool.bin").c_str(),L"ab")==0&&empty)std::fclose(empty);
        const std::wstring temporary=stem+L".json.tmp",manifest=stem+L".json";
        const bool files=write(temporary,body.data(),body.size())&&
            MoveFileExW(temporary.c_str(),manifest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
        Log::get().note("flat draw pixels: frame=%llu status=%s reason=%s draws=%u/%u copies=%llu cb-bytes=%llu motion=%u/%u refusals=%u pool-bytes=%llu files=%s directory=%ls",
            (unsigned long long)f.number,status,f.reason,(unsigned)f.draws.size(),f.drawsSeen,
            (unsigned long long)f.pixelWritten,(unsigned long long)f.cbWritten,motionComplete?1u:0u,motionDraws,
            f.motionRefusals,(unsigned long long)f.poolWritten,files?"ok":"failed",directory_.c_str());
        if(!files)failed_=true;allocated_-=f.bytesAllocated;
    }
public:
    bool active() const {return armed_&&current_!=nullptr;}
    const std::wstring& directory() const {return directory_;}
    void cancel(const char* reason) {if(armed_&&!current_&&pending_.empty())
            Log::get().note("flat draw pixels: summary status=failed reason=%s qualified=%u unqualified=%u directory=%ls",reason,accepted_,unqualified_,directory_.c_str());
        if(current_){current_->reason=reason;finish(*current_,true);current_.reset();}
        while(!pending_.empty()){pending_.front()->reason=reason;finish(*pending_.front(),true);pending_.pop_front();}armed_=false;}
    void arm(uint64_t frame) {
        cancel("rearmed");directory_.clear();accepted_=0;unqualified_=0;failed_=false;shaderBytes_.clear();armed_=true;armFrame_=frame;armMs_=GetTickCount64();
        SYSTEMTIME t{};GetSystemTime(&t);wchar_t leaf[128]{};
        _snwprintf_s(leaf,_TRUNCATE,L"%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),++serial_);
        const auto root=Config::get().logDir()+L"\\flat_draw_pixels";directory_=root+L"\\"+leaf;
        if(!ensureDirectory(Config::get().logDir())||!ensureDirectory(root)||!ensureDirectory(directory_)){armed_=false;Log::get().note("flat draw pixels: arm refused directory");return;}
        Log::get().note("flat draw pixels: armed frame=%llu next-two-qualified-frames points=8 window=16 cap=512-draws/frame memory=256MiB expiry=900frames/30s directory=%ls",
            (unsigned long long)frame,directory_.c_str());
    }
    void begin(uint64_t frame,const void* priorDepth,unsigned width,unsigned height) {
        if(!armed_||accepted_>=kFrames||current_)return;
        if(frame-armFrame_>900 || GetTickCount64()-armMs_>30000){cancel("arm-expired-no-qualified-frame");return;}
        if(!priorDepth||width<kWindow||height<kWindow){if(accepted_)cancel("second-frame-prior-source-unavailable");return;}
        current_.reset(new Frame);current_->number=frame;current_->priorDepth=priorDepth;
        current_->width=width;current_->height=height;current_->startMs=GetTickCount64();current_->draws.reserve(kDrawCap);
    }
    bool before(ID3D11DeviceContext* ctx,unsigned instances,char kind,uint32_t count,uint32_t start,int32_t base,uint32_t startInstance,
                uint64_t vs,uint64_t ps,const void* vsObject,const void* psObject) {
        if(!active()||!ctx)return false;Frame& f=*current_;
        Ptr<ID3D11RenderTargetView> views[4];Ptr<ID3D11DepthStencilView> dsv;
        ID3D11RenderTargetView* raw[4]{};ctx->OMGetRenderTargets(4,raw,&dsv);
        bool matched=false;Rt rt[4];
        for(unsigned i=0;i<4;++i){views[i].Attach(raw[i]);if(!views[i])continue;
            Ptr<ID3D11Resource> res;views[i]->GetResource(&res);Ptr<ID3D11Texture2D> tex;if(!res||FAILED(res.As(&tex)))continue;
            D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);D3D11_RENDER_TARGET_VIEW_DESC vd{};views[i]->GetDesc(&vd);
            if(td.Width!=f.width||td.Height!=f.height)continue;
            matched=true;rt[i].texture=tex;rt[i].resource=res.Get();rt[i].view=views[i].Get();rt[i].width=td.Width;rt[i].height=td.Height;
            rt[i].format=td.Format;rt[i].viewFormat=vd.Format;
            rt[i].valid=td.MipLevels>=1&&td.ArraySize==1&&td.SampleDesc.Count==1&&
                vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D&&vd.Texture2D.MipSlice==0&&bpp(td.Format)!=0;
        }
        if(!matched)return false;++f.drawsSeen;
        if(f.draws.size()>=kDrawCap){++f.overflows;return false;}
        Draw d;d.q=f.drawsSeen;d.instances=instances;d.kind=kind;d.count=count;d.start=start;d.base=base;d.startInstance=startInstance;
        d.vs=vs;d.ps=ps;d.vsObject=vsObject;d.psObject=psObject;
        d.dsv=dsv.Get();if(dsv){Ptr<ID3D11Resource> dr;dsv->GetResource(&dr);d.depthResource=dr.Get();
            Ptr<ID3D11Texture2D> dt;if(dr&&SUCCEEDED(dr.As(&dt))){D3D11_TEXTURE2D_DESC td{};dt->GetDesc(&td);d.depthFormat=td.Format;}
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};dsv->GetDesc(&vd);d.depthViewFormat=vd.Format;}
        for(unsigned i=0;i<4;++i)d.rt[i]=rt[i];
        metadata(ctx,d);stageCb(ctx,f,d);
        const auto captureStage=[&](char stage,uint64_t h) {
            if(!h)return false;const auto key=std::make_pair(stage,h);auto it=shaderBytes_.find(key);if(it!=shaderBytes_.end())return it->second;
            const bool saved=shaderBytes_.size()<256&&captureFlatProbeShader(stage,h);shaderBytes_.emplace(key,saved);return saved;
        };
        d.vsBytes=captureStage('v',d.vs);d.psBytes=captureStage('p',d.ps);
        for(unsigned i=0;i<4;++i)if(d.rt[i].texture)for(unsigned p=0;p<kPoints;++p)stageCopy(ctx,f,d,i,p,false);
        d.before=true;f.draws.push_back(std::move(d));return true;
    }
    // Called after the engine's private MRT6 substitution, but before the
    // game's raster draw. The first before() above retains original CB/IA.
    void motionBefore(ID3D11DeviceContext* ctx,bool expectedEngineSlot,bool sourceCandidate=false) {
        if(!active()||!ctx||current_->draws.empty())return;
        Frame& f=*current_;Draw& d=f.draws.back();if(!d.before)return;
        const bool targetPair=(d.vs==0x66DE2CADB1F4AE6Bull&&d.ps==0x864F1F949851B8DEull)||
                              (d.vs==0xBBE58E40FE88EC80ull&&d.ps==0xDB3E8D20CF53FBC0ull);
        if(!targetPair)return; // Bound diagnostic: the underlay and decal only.
        d.motionCandidate=sourceCandidate;
        if(sourceCandidate&&!expectedEngineSlot){d.motionStatus="producer-declined";++f.motionRefusals;return;}
        if(!expectedEngineSlot)return;
        d.motionExpected=true;d.motionStatus="unavailable";
        ID3D11RenderTargetView* raw[7]{};Ptr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(7,raw,&dsv);
        Ptr<ID3D11RenderTargetView> views[7];for(unsigned i=0;i<7;++i)views[i].Attach(raw[i]);
        Ptr<ID3D11ShaderResourceView> pool;ID3D11ShaderResourceView* poolRaw=nullptr;
        ctx->VSGetShaderResources(33,1,&poolRaw);pool.Attach(poolRaw);
        d.motionPoolView=pool.Get();if(pool){Ptr<ID3D11Resource> pr;pool->GetResource(&pr);d.motionPool=pr.Get();}
        stagePool(ctx,f,d,pool.Get());
        if(!views[6]||!dsv||dsv.Get()!=d.dsv){++f.motionRefusals;return;}
        Ptr<ID3D11Resource> slotResource;views[6]->GetResource(&slotResource);
        Ptr<ID3D11Texture2D> slot;if(!slotResource||FAILED(slotResource.As(&slot))){++f.motionRefusals;return;}
        D3D11_TEXTURE2D_DESC sd{};slot->GetDesc(&sd);D3D11_RENDER_TARGET_VIEW_DESC sv{};views[6]->GetDesc(&sv);
        d.motionSlot.texture=slot;d.motionSlot.resource=slotResource.Get();d.motionSlot.view=views[6].Get();
        d.motionSlot.format=sd.Format;d.motionSlot.viewFormat=sv.Format;d.motionSlot.width=sd.Width;d.motionSlot.height=sd.Height;
        d.motionSlot.valid=sd.Width==f.width&&sd.Height==f.height&&sd.MipLevels>=1&&sd.ArraySize==1&&
            sd.SampleDesc.Count==1&&sv.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D&&sv.Texture2D.MipSlice==0&&
            (sd.Format==DXGI_FORMAT_R32G32_FLOAT||sd.Format==DXGI_FORMAT_R32G32_TYPELESS)&&
            sv.Format==DXGI_FORMAT_R32G32_FLOAT;
        Ptr<ID3D11Resource> depthResource;dsv->GetResource(&depthResource);
        Ptr<ID3D11Texture2D> depth;if(!depthResource||FAILED(depthResource.As(&depth))){++f.motionRefusals;return;}
        D3D11_TEXTURE2D_DESC dd{};depth->GetDesc(&dd);D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dsv->GetDesc(&dv);
        d.motionDepth.texture=depth;d.motionDepth.resource=depthResource.Get();d.motionDepth.view=dsv.Get();
        d.motionDepth.format=dd.Format;d.motionDepth.viewFormat=dv.Format;d.motionDepth.width=dd.Width;d.motionDepth.height=dd.Height;
        d.motionDepth.valid=dd.Width==f.width&&dd.Height==f.height&&dd.MipLevels>=1&&dd.ArraySize==1&&
            dd.SampleDesc.Count==1&&dv.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D&&dv.Texture2D.MipSlice==0&&
            ((dd.Format==DXGI_FORMAT_R32G8X24_TYPELESS&&dv.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)||
             (dd.Format==DXGI_FORMAT_R32_TYPELESS&&dv.Format==DXGI_FORMAT_D32_FLOAT))&&
            depthResource.Get()==d.depthResource;
        if(!d.motionSlot.valid||!d.motionDepth.valid){++f.motionRefusals;return;}
        if(!stageDepthMirror(ctx,f,d)){d.motionStatus="depth-mirror-unavailable";++f.motionRefusals;return;}
        d.motionStatus="captured";d.motionBefore=true;
        for(unsigned p=0;p<kPoints;++p){stageCopy(ctx,f,d,6,p,false);stageCopy(ctx,f,d,7,p,false);}
    }
    void after(ID3D11DeviceContext* ctx) {
        if(!active()||!ctx)return;Frame& f=*current_;if(f.draws.empty())return;
        Draw& d=f.draws.back();if(!d.before)return;d.before=false;
        Ptr<ID3D11RenderTargetView> views[4];ID3D11RenderTargetView* raw[4]{};ctx->OMGetRenderTargets(4,raw,nullptr);
        for(unsigned i=0;i<4;++i){views[i].Attach(raw[i]);if(!d.rt[i].texture)continue;
            if(views[i].Get()!=d.rt[i].view){++f.refusals;d.reason="rt-changed-during-draw";continue;}
            for(unsigned p=0;p<kPoints;++p)stageCopy(ctx,f,d,i,p,true);
        }
        if(d.motionBefore){
            ID3D11RenderTargetView* bound[7]{};Ptr<ID3D11DepthStencilView> mdsv;
            ctx->OMGetRenderTargets(7,bound,&mdsv);
            Ptr<ID3D11RenderTargetView> owned[7];for(unsigned i=0;i<7;++i)owned[i].Attach(bound[i]);
            if(owned[6].Get()==d.motionSlot.view&&mdsv.Get()==d.motionDepth.view&&stageDepthMirror(ctx,f,d)){
                for(unsigned p=0;p<kPoints;++p){stageCopy(ctx,f,d,6,p,true);stageCopy(ctx,f,d,7,p,true);}
            }else{d.motionStatus="view-or-mirror-changed";++f.motionRefusals;
                for(unsigned p=0;p<kPoints;++p)for(unsigned rt=6;rt<=7;++rt){Copy c;c.rt=rt;c.point=p;c.after=true;c.status="skipped";d.copies.push_back(c);}}
        }
    }
    void qualify(uint64_t frame,const void* selectedDepth,const void* selectedHdr,unsigned w,unsigned h) {
        if(!active()||current_->number!=frame)return;Frame& f=*current_;
        f.qualified=true;f.selectedDepth=selectedDepth;f.selectedHdr=selectedHdr;
        f.identity=(selectedDepth==f.priorDepth&&w==f.width&&h==f.height);
        f.reason=f.identity?"qualified":"prior-depth-or-extent-mismatch";
    }
    void present(ID3D11DeviceContext* ctx,uint64_t completedFrame) {
        if(current_&&current_->number==completedFrame) {
            if(current_->qualified&&current_->identity){++accepted_;pending_.push_back(std::move(current_));}
            else {++unqualified_;if(accepted_){current_->reason="second-frame-unqualified";finish(*current_,true);armed_=false;}
                else {allocated_-=current_->bytesAllocated;
                    if(unqualified_<=4||unqualified_%90==0)Log::get().note("flat draw pixels: unqualified frame=%llu reason=%s draws=%u attempts=%u; retrying while armed",
                    (unsigned long long)current_->number,current_->reason,current_->drawsSeen,unqualified_);}current_.reset();}
        }
        for(auto it=pending_.begin();it!=pending_.end();) {
            Frame& f=**it;++f.readbackFrames;
            const bool timeout=f.readbackFrames>120||GetTickCount64()-f.startMs>5000;
            if(timeout||readback(ctx,f)){finish(f,timeout);it=pending_.erase(it);}else ++it;
        }
        if(accepted_>=kFrames)armed_=false;
        if(armed_&&(completedFrame-armFrame_>900||GetTickCount64()-armMs_>30000))cancel("arm-expired");
    }
};
constexpr float FlatDrawCapture::uv_[FlatDrawCapture::kPoints][2];
} // namespace edvr
