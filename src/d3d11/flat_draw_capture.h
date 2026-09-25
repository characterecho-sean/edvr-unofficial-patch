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
        Rt rt[4]; Cb cb[3]; std::vector<Copy> copies;
        bool before=false; const char* reason="";
    };
    struct Chunk { Ptr<ID3D11Texture2D> stage; unsigned format=0,bpp=0,used=0; bool mapped=false;
        uint64_t allocation=0; };
    struct Frame {
        uint64_t number=0, startMs=0, bytesAllocated=0; unsigned width=0,height=0, drawsSeen=0, overflows=0, refusals=0;
        const void* priorDepth=nullptr; const void* selectedDepth=nullptr; const void* selectedHdr=nullptr;
        bool qualified=false, identity=false; const char* reason="not-qualified";
        std::vector<Draw> draws; std::vector<Chunk> chunks;
        FILE* pixelFile=nullptr;FILE* cbFile=nullptr;
        uint64_t pixelWritten=0,cbWritten=0;
        unsigned readbackFrames=0; bool readbackError=false;
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
        case DXGI_FORMAT_R32G32_TYPELESS: return 8;
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
    bool reserve(Frame& f,uint64_t bytes) {
        if(bytes>kByteCap || allocated_>kByteCap-bytes){++f.overflows;return false;}
        allocated_+=bytes;f.bytesAllocated+=bytes;return true;
    }
    bool stageCopy(ID3D11DeviceContext* ctx,Frame& f,Draw& d,unsigned rt,unsigned pt,bool after) {
        const Rt& r=d.rt[rt]; Copy c;c.rt=rt;c.point=pt;c.after=after;c.format=r.format;c.bpp=bpp(static_cast<DXGI_FORMAT>(r.format));
        if(!r.valid){c.status="unavailable";d.copies.push_back(c);++f.refusals;return false;}
        c.x0=origin(center(uv_[pt][0],r.width),r.width);c.y0=origin(center(uv_[pt][1],r.height),r.height);
        unsigned idx=0;
        for(;idx<f.chunks.size();++idx)if(f.chunks[idx].format==r.format && f.chunks[idx].used<kChunkSlots)break;
        if(idx==f.chunks.size()) {
            const uint64_t n=uint64_t(kChunkCols*kWindow)*(kChunkSlots/kChunkCols*kWindow)*c.bpp;
            if(!reserve(f,n)){c.status="unavailable";d.copies.push_back(c);return false;}
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            D3D11_TEXTURE2D_DESC td{};td.Width=kChunkCols*kWindow;td.Height=kChunkSlots/kChunkCols*kWindow;
            td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=static_cast<DXGI_FORMAT>(r.format);
            td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Chunk ch;ch.format=r.format;ch.bpp=c.bpp;ch.allocation=n;
            if(FAILED(dev->CreateTexture2D(&td,nullptr,&ch.stage))) {allocated_-=n;f.bytesAllocated-=n;c.status="unavailable";d.copies.push_back(c);++f.refusals;return false;}
            f.chunks.push_back(std::move(ch));
        }
        Chunk& ch=f.chunks[idx];c.chunk=idx;c.slot=ch.used++;
        const D3D11_BOX box{c.x0,c.y0,0,c.x0+kWindow,c.y0+kWindow,1};
        ctx->CopySubresourceRegion(ch.stage.Get(),0,(c.slot%kChunkCols)*kWindow,(c.slot/kChunkCols)*kWindow,0,r.texture.Get(),0,&box);
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
        for(unsigned j=0;j<f.chunks.size();++j) {
            Chunk& ch=f.chunks[j];if(ch.mapped)continue;
            D3D11_MAPPED_SUBRESOURCE map{};const HRESULT hr=ctx->Map(ch.stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return false;
            if(FAILED(hr)){f.readbackError=true;ch.mapped=true;continue;}
            for(auto& d:f.draws)for(auto& c:d.copies)if(c.chunk==j&&!std::strcmp(c.status,"queued")) {
                c.offset=f.pixelWritten;c.length=kWindow*kWindow*c.bpp;
                const unsigned sx=(c.slot%kChunkCols)*kWindow,sy=(c.slot/kChunkCols)*kWindow;
                bool ok=true;
                for(unsigned row=0;row<kWindow;++row) {
                    const auto* p=static_cast<const uint8_t*>(map.pData)+size_t(sy+row)*map.RowPitch+sx*c.bpp;
                    if(std::fwrite(p,1,kWindow*c.bpp,f.pixelFile)!=kWindow*c.bpp){ok=false;break;}
                }
                if(ok){f.pixelWritten+=c.length;c.status="ok";}else{c.status="gpu_error";c.length=0;f.readbackError=true;}
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
        return true;
    }
    static std::string json(Frame& f,const char* status) {
        std::ostringstream o;o<<"{\"schema\":1,\"status\":\""<<status<<"\",\"reason\":\""<<f.reason
          <<"\",\"frame\":"<<f.number<<",\"render_width\":"<<f.width<<",\"render_height\":"<<f.height
          <<",\"prior_depth\":\""<<hex(f.priorDepth)<<"\",\"selected_depth\":\""<<hex(f.selectedDepth)
          <<"\",\"selected_hdr\":\""<<hex(f.selectedHdr)<<"\",\"qualified\":"<<(f.qualified?"true":"false")
          <<",\"identity_match\":"<<(f.identity?"true":"false")<<",\"draw_cap\":"<<kDrawCap
          <<",\"draws_seen\":"<<f.drawsSeen<<",\"draws_recorded\":"<<f.draws.size()
          <<",\"overflow\":"<<f.overflows<<",\"refusals\":"<<f.refusals
          <<",\"pixels_file\":\"frame_"<<f.number<<"_pixels.bin\",\"pixels_bytes\":"<<f.pixelWritten
          <<",\"cb_file\":\"frame_"<<f.number<<"_cb.bin\",\"cb_bytes\":"<<f.cbWritten<<",\"points\":[";
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
                <<",\"valid\":"<<(d.rt[r].valid?"true":"false")<<"}";}o<<"],\"cb\":[";
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
        const char* unfinished=timedOut?(!f.qualified?"skipped":"timeout"):"gpu_error";
        for(auto& d:f.draws){for(auto& c:d.copies)if(!std::strcmp(c.status,"queued")){c.status=unfinished;c.length=0;}
            for(auto& c:d.cb)if(!std::strcmp(c.status,"queued")){c.status=unfinished;c.length=0;}}
        bool all=f.qualified&&f.identity&&!f.draws.empty()&&!f.overflows&&!f.refusals&&!f.readbackError&&!timedOut&&f.drawsSeen<=kDrawCap;
        for(auto& d:f.draws){for(auto& c:d.copies)if(std::strcmp(c.status,"ok"))all=false;
            for(auto& c:d.cb)if(c.resource&&std::strcmp(c.status,"ok"))all=false;}
        const char* status=all?"complete":f.qualified?"partial":"failed";
        if(!std::strcmp(f.reason,"qualified"))f.reason=timedOut?"readback-timeout":all?"qualified":"copy-or-cap-refused";
        const std::wstring stem=directory_+L"\\frame_"+std::to_wstring(f.number);
        const std::string body=json(f,status);
        FILE* empty=nullptr;
        if(_wfopen_s(&empty,(stem+L"_pixels.bin").c_str(),L"ab")==0&&empty)std::fclose(empty);
        empty=nullptr;if(_wfopen_s(&empty,(stem+L"_cb.bin").c_str(),L"ab")==0&&empty)std::fclose(empty);
        const std::wstring temporary=stem+L".json.tmp",manifest=stem+L".json";
        const bool files=write(temporary,body.data(),body.size())&&
            MoveFileExW(temporary.c_str(),manifest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
        Log::get().note("flat draw pixels: frame=%llu status=%s reason=%s draws=%u/%u copies=%llu cb-bytes=%llu files=%s directory=%ls",
            (unsigned long long)f.number,status,f.reason,(unsigned)f.draws.size(),f.drawsSeen,
            (unsigned long long)f.pixelWritten,(unsigned long long)f.cbWritten,files?"ok":"failed",directory_.c_str());
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
    void after(ID3D11DeviceContext* ctx) {
        if(!active()||!ctx)return;Frame& f=*current_;if(f.draws.empty())return;
        Draw& d=f.draws.back();if(!d.before)return;d.before=false;
        Ptr<ID3D11RenderTargetView> views[4];ID3D11RenderTargetView* raw[4]{};ctx->OMGetRenderTargets(4,raw,nullptr);
        for(unsigned i=0;i<4;++i){views[i].Attach(raw[i]);if(!d.rt[i].texture)continue;
            if(views[i].Get()!=d.rt[i].view){++f.refusals;d.reason="rt-changed-during-draw";continue;}
            for(unsigned p=0;p<kPoints;++p)stageCopy(ctx,f,d,i,p,true);
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
