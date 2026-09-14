#pragma once
// Draw-local evidence for the game's HDR -> display-colour conversion.
// No draws, binding changes, flushes or blocking readbacks. The caller arms
// only the first requested eye frame; end() copies the original output once.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace edvr {
class EyeTonemapSnapshot {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Layout { char semantic[64]{}; uint32_t index=0,format=0,slot=0,offset=0,classification=0,step=0; };
    struct Blob {
        Ptr<ID3D11Resource> stage,source;
        std::string meta="{}";
        uint32_t bytes=0,row=0,rows=0,slices=0,x=0,y=0,width=0,height=0;
        bool ready=false;
    };
    struct Draw { std::string meta; Blob blobs[6]; }; // exposure, LUT, HDR, output, PS b2, VB0
    std::vector<Draw> draws_;
    uint32_t firstFrame_=0,pending_=UINT32_MAX,imageBytes_=0,smallBytes_=0;
    static constexpr uint32_t kImageBudget=48*1024*1024,kSmallBudget=8*1024*1024,kBufferBudget=8192,kCrop=1400;
    static const GUID& layoutKey() { static const GUID g={0x52b4e581,0x97d2,0x4bb1,{0x8c,0xc8,0x21,0x64,0x9d,0x5c,0x4e,0x11}};return g; }
    static std::mutex& mutex() { static std::mutex m;return m; }
    static std::map<uint64_t,std::vector<uint8_t>>& shaders() { static std::map<uint64_t,std::vector<uint8_t>> m;return m; }
    template<class T> static std::string words(const T& t) {
        static_assert(sizeof(T)%4==0,"word-aligned descriptor");std::ostringstream s;s<<'[';
        for(size_t i=0;i<sizeof(T)/4;++i) { uint32_t v;std::memcpy(&v,reinterpret_cast<const char*>(&t)+i*4,4);if(i)s<<',';s<<v; }
        s<<']';return s.str();
    }
    static uint32_t pixelBytes(DXGI_FORMAT f) {
        switch(f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R32G32_FLOAT:return 8;
        case DXGI_FORMAT_R11G11B10_FLOAT:case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:return 4;
        case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R8G8_UNORM:return 2;
        case DXGI_FORMAT_R8_UNORM:return 1;
        default:return 0;
        }
    }
    bool texture(ID3D11DeviceContext* c,ID3D11Device* dev,ID3D11Resource* src,
                 DXGI_FORMAT viewFormat,const std::string& view,bool volume,bool crop,bool defer,Blob& b) {
        if(!src)return false;
        Ptr<ID3D11Texture2D> t2;Ptr<ID3D11Texture3D> t3;
        D3D11_TEXTURE2D_DESC d2{};D3D11_TEXTURE3D_DESC d3{};
        uint32_t w=0,h=0,z=1,mips=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        if(volume) {
            if(FAILED(src->QueryInterface(IID_PPV_ARGS(&t3))))return false;
            t3->GetDesc(&d3);w=d3.Width;h=d3.Height;z=d3.Depth;mips=d3.MipLevels;format=d3.Format;
        } else {
            if(FAILED(src->QueryInterface(IID_PPV_ARGS(&t2))))return false;
            t2->GetDesc(&d2);w=d2.Width;h=d2.Height;mips=d2.MipLevels;format=d2.Format;
            if(d2.ArraySize!=1 || d2.SampleDesc.Count!=1 || (d2.BindFlags&D3D11_BIND_DEPTH_STENCIL))return false;
        }
        const uint32_t stride=pixelBytes(format);
        if(!stride || pixelBytes(viewFormat)!=stride || !w || !h || !z || mips!=1)return false;
        const uint32_t cw=crop && w>kCrop?kCrop:w,ch=crop && h>kCrop?kCrop:h;
        const uint64_t n=uint64_t(cw)*ch*z*stride;
        uint32_t& reserved=crop?imageBytes_:smallBytes_;const uint32_t cap=crop?kImageBudget:kSmallBudget;
        if(!n || n>cap-reserved)return false;
        b.x=(w-cw)/2;b.y=(h-ch)/2;b.width=cw;b.height=ch;
        b.bytes=static_cast<uint32_t>(n);b.row=cw*stride;b.rows=ch;b.slices=z;
        HRESULT hr=E_FAIL;
        if(volume) {
            d3.Usage=D3D11_USAGE_STAGING;d3.BindFlags=d3.MiscFlags=0;d3.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Ptr<ID3D11Texture3D> stage;hr=dev->CreateTexture3D(&d3,nullptr,&stage);b.stage=stage;
        } else {
            d2.Width=cw;d2.Height=ch;d2.Usage=D3D11_USAGE_STAGING;d2.BindFlags=d2.MiscFlags=0;
            d2.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d2.SampleDesc.Quality=0;
            Ptr<ID3D11Texture2D> stage;hr=dev->CreateTexture2D(&d2,nullptr,&stage);b.stage=stage;
        }
        if(FAILED(hr)) { ++failures;return false; }
        std::ostringstream m;m<<"{\"type\":"<<(volume?3:2)<<",\"resource\":"<<reinterpret_cast<uint64_t>(src)
            <<",\"format\":"<<format<<",\"view_format\":"<<viewFormat<<",\"view\":"<<view
            <<",\"source\":["<<w<<','<<h<<','<<z<<"],\"origin\":["<<b.x<<','<<b.y<<",0],\"size\":["
            <<cw<<','<<ch<<','<<z<<"],\"row\":"<<b.row<<",\"bytes\":"<<b.bytes<<'}';b.meta=m.str();
        reserved+=b.bytes;bytes+=b.bytes;
        if(defer)b.source=src;
        else { D3D11_BOX box{b.x,b.y,0,b.x+cw,b.y+ch,z};c->CopySubresourceRegion(b.stage.Get(),0,0,0,0,src,0,&box);b.ready=true; }
        return true;
    }
    bool srv(ID3D11DeviceContext* c,ID3D11Device* dev,ID3D11ShaderResourceView* v,bool volume,bool crop,Blob& b) {
        if(!v)return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};v->GetDesc(&vd);Ptr<ID3D11Resource> r;v->GetResource(&r);
        if(volume) {
            if(vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE3D || vd.Texture3D.MostDetailedMip || vd.Texture3D.MipLevels!=1)return false;
        } else if(vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || vd.Texture2D.MostDetailedMip || vd.Texture2D.MipLevels!=1)return false;
        return texture(c,dev,r.Get(),vd.Format,words(vd),volume,crop,false,b);
    }
    bool buffer(ID3D11DeviceContext* c,ID3D11Device* dev,ID3D11Buffer* src,uint32_t off,uint32_t n,Blob& b) {
        if(!src || !n || n>kBufferBudget)return false;
        D3D11_BUFFER_DESC original{};src->GetDesc(&original);
        if(off>original.ByteWidth || n>original.ByteWidth-off)return false;
        D3D11_BUFFER_DESC d{};d.ByteWidth=n;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Buffer> stage;if(FAILED(dev->CreateBuffer(&d,nullptr,&stage))) { ++failures;return false; }
        D3D11_BOX box{off,0,0,off+n,1,1};c->CopySubresourceRegion(stage.Get(),0,0,0,0,src,0,&box);
        b.stage=stage;b.bytes=n;b.ready=true;bytes+=n;
        std::ostringstream m;m<<"{\"type\":1,\"resource\":"<<reinterpret_cast<uint64_t>(src)<<",\"whole\":"<<original.ByteWidth
            <<",\"offset\":"<<off<<",\"bytes\":"<<n<<'}';b.meta=m.str();return true;
    }
public:
    static constexpr uint64_t kVs=0x2D78DC3FD2C0C543ull,kPs=0x99C21CEB7A699821ull;
    uint32_t declined=0,failures=0,bytes=0;
    uint32_t count() const { return static_cast<uint32_t>(draws_.size()); }
    void reset() { draws_.clear();firstFrame_=imageBytes_=smallBytes_=declined=failures=bytes=0;pending_=UINT32_MAX; }
    static void rememberShader(uint64_t hash,const void* data,size_t n) {
        if((hash!=kVs && hash!=kPs) || !data || !n || n>256*1024)return;
        std::lock_guard<std::mutex> lock(mutex());if(shaders().count(hash))return;
        auto p=static_cast<const uint8_t*>(data);shaders()[hash]=std::vector<uint8_t>(p,p+n);
    }
    static void rememberLayout(ID3D11InputLayout* l,const D3D11_INPUT_ELEMENT_DESC* e,UINT n,uint64_t hash) {
        if(hash!=kVs || !l || !e || !n || n>32)return;Layout a[32]{};
        for(UINT i=0;i<n;++i) {
            if(!e[i].SemanticName || strlen(e[i].SemanticName)>=64)return;
            // Shader semantics are identifiers; reject rather than emit unescaped JSON.
            for(const char* s=e[i].SemanticName;*s;++s)if(!((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'))return;
            strcpy_s(a[i].semantic,e[i].SemanticName);a[i].index=e[i].SemanticIndex;a[i].format=e[i].Format;
            a[i].slot=e[i].InputSlot;a[i].offset=e[i].AlignedByteOffset;a[i].classification=e[i].InputSlotClass;a[i].step=e[i].InstanceDataStepRate;
        }
        l->SetPrivateData(layoutKey(),n*sizeof(Layout),a);
    }
    void capture(ID3D11DeviceContext* c,uint32_t frame,uint32_t ordinal,uint64_t vs,uint64_t ps,
                 char kind,uint32_t n,uint32_t startVertex,uint32_t instances=1,uint32_t startInstance=0) {
        if(!c || vs!=kVs || ps!=kPs)return;
        if(!firstFrame_)firstFrame_=frame;
        if(frame!=firstFrame_)return;
        if(draws_.size()>=2 || pending_!=UINT32_MAX || (kind!='D' && kind!='N') || n!=3 || instances!=1 || startInstance) { ++declined;return; }
        Ptr<ID3D11Device> dev;c->GetDevice(&dev);
        Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> dsv;c->OMGetRenderTargets(1,&rt,&dsv);
        // This exact pass replaces an LDR colour target without a DSV or blending.
        if(!rt || dsv) { ++declined;return; }
        D3D11_RENDER_TARGET_VIEW_DESC rd{};rt->GetDesc(&rd);
        if(rd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice) { ++declined;return; }
        Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;c->OMGetBlendState(&blend,factors,&mask);D3D11_BLEND_DESC bd{};
        if(blend)blend->GetDesc(&bd);
        if(bd.RenderTarget[0].BlendEnable) { ++declined;return; }
        Ptr<ID3D11InputLayout> layout;c->IAGetInputLayout(&layout);Layout elems[32]{};UINT layoutBytes=sizeof(elems);
        if(!layout || FAILED(layout->GetPrivateData(layoutKey(),&layoutBytes,elems)) || !layoutBytes || layoutBytes>sizeof(elems) || layoutBytes%sizeof(Layout)) { ++declined;return; }
        for(UINT i=0;i<layoutBytes/sizeof(Layout);++i)if(elems[i].slot || elems[i].classification!=D3D11_INPUT_PER_VERTEX_DATA) { ++declined;return; }
        Ptr<ID3D11Buffer> vb,cb;UINT stride=0,offset=0;c->IAGetVertexBuffers(0,1,&vb,&stride,&offset);c->PSGetConstantBuffers(2,1,&cb);
        if(!stride || stride>256 || !vb || !cb || uint64_t(startVertex)*stride+offset>UINT32_MAX) { ++declined;return; }
        D3D11_BUFFER_DESC cbd{};cb->GetDesc(&cbd);const uint32_t cbBytes=cbd.ByteWidth<kBufferBudget?cbd.ByteWidth:kBufferBudget;
        if(cbBytes<256) { ++declined;return; }
        Draw d;bool complete=true;
        Ptr<ID3D11ShaderResourceView> exposure,lut,hdr;c->VSGetShaderResources(0,1,&exposure);c->PSGetShaderResources(0,1,&lut);c->PSGetShaderResources(1,1,&hdr);
        complete=srv(c,dev.Get(),exposure.Get(),false,false,d.blobs[0]) && complete;
        complete=srv(c,dev.Get(),lut.Get(),true,false,d.blobs[1]) && complete;
        complete=srv(c,dev.Get(),hdr.Get(),false,true,d.blobs[2]) && complete;
        Ptr<ID3D11Resource> target;rt->GetResource(&target);
        complete=texture(c,dev.Get(),target.Get(),rd.Format,words(rd),false,true,true,d.blobs[3]) && complete;
        complete=buffer(c,dev.Get(),cb.Get(),0,cbBytes,d.blobs[4]) && complete;
        complete=buffer(c,dev.Get(),vb.Get(),offset+startVertex*stride,n*stride,d.blobs[5]) && complete;
        if(!complete)++declined;
        D3D11_PRIMITIVE_TOPOLOGY topology{};c->IAGetPrimitiveTopology(&topology);
        Ptr<ID3D11DepthStencilState> depth;UINT stencil=0;c->OMGetDepthStencilState(&depth,&stencil);D3D11_DEPTH_STENCIL_DESC dd{};if(depth)depth->GetDesc(&dd);
        Ptr<ID3D11RasterizerState> raster;c->RSGetState(&raster);D3D11_RASTERIZER_DESC rs{};if(raster)raster->GetDesc(&rs);
        D3D11_VIEWPORT views[16]{};UINT nv=16;c->RSGetViewports(&nv,views);D3D11_RECT rects[16]{};UINT nr=16;c->RSGetScissorRects(&nr,rects);
        Ptr<ID3D11SamplerState> samplers[3];c->VSGetSamplers(0,1,&samplers[0]);c->PSGetSamplers(0,1,&samplers[1]);c->PSGetSamplers(1,1,&samplers[2]);
        std::ostringstream m;m<<"{\"frame\":"<<frame<<",\"ordinal\":"<<ordinal<<",\"vs\":"<<vs<<",\"ps\":"<<ps
            <<",\"kind\":"<<unsigned(kind)<<",\"count\":"<<n<<",\"start\":"<<startVertex<<",\"instances\":"<<instances
            <<",\"start_instance\":"<<startInstance<<",\"topology\":"<<topology<<",\"vb_stride\":"<<stride<<",\"vb_offset\":"<<offset
            <<",\"complete_inputs\":"<<(complete?"true":"false")<<",\"layout\":[";
        for(UINT i=0;i<layoutBytes/sizeof(Layout);++i) { const auto& e=elems[i];if(i)m<<',';m<<"[\""<<e.semantic<<"\","<<e.index<<','<<e.format<<','<<e.slot<<','<<e.offset<<','<<e.classification<<','<<e.step<<']'; }
        m<<"],\"blend\":"<<(blend?words(bd):"null")<<",\"blend_factor\":"<<words(factors)<<",\"sample_mask\":"<<mask
            <<",\"depth\":"<<(depth?words(dd):"null")<<",\"stencil_ref\":"<<stencil<<",\"raster\":"<<(raster?words(rs):"null")<<",\"viewports\":[";
        for(UINT i=0;i<nv;++i) { if(i)m<<',';m<<words(views[i]); }m<<"],\"scissors\":[";
        for(UINT i=0;i<nr;++i) { if(i)m<<',';m<<words(rects[i]); }m<<"],\"samplers\":[";
        for(unsigned i=0;i<3;++i) { if(i)m<<',';D3D11_SAMPLER_DESC sd{};if(samplers[i])samplers[i]->GetDesc(&sd);m<<(samplers[i]?words(sd):"null"); }
        m<<"]}";d.meta=m.str();draws_.push_back(std::move(d));pending_=count()-1;
    }
    void end(ID3D11DeviceContext* c) {
        const uint32_t index=pending_;pending_=UINT32_MAX;
        if(!c || index>=draws_.size())return;auto& b=draws_[index].blobs[3];
        if(!b.source || !b.stage)return;
        D3D11_BOX box{b.x,b.y,0,b.x+b.width,b.y+b.height,1};c->CopySubresourceRegion(b.stage.Get(),0,0,0,0,b.source.Get(),0,&box);
        b.ready=true;b.source.Reset();
    }
    bool write(ID3D11DeviceContext* c,const wchar_t* path,const wchar_t* shaderDir=nullptr) {
        (void)shaderDir;if(!c || !path)return false;FILE* f=nullptr;if(_wfopen_s(&f,path,L"wb") || !f)return false;
        bool ok=fwrite("EDVRTON1",1,8,f)==8;
        auto u32=[&](uint32_t v){ok=fwrite(&v,4,1,f)==1 && ok;};
        auto text=[&](const std::string& s){u32(static_cast<uint32_t>(s.size()));ok=fwrite(s.data(),1,s.size(),f)==s.size() && ok;};
        u32(1);u32(count());u32(declined);u32(bytes);
        for(auto& d:draws_) {
            text(d.meta);
            for(auto& b:d.blobs) {
                text(b.meta);D3D11_MAPPED_SUBRESOURCE mapped{};
                if(!b.ready || !b.stage || FAILED(c->Map(b.stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))) { u32(0);++failures;continue; }
                const bool pitchOk=mapped.pData && (!b.row || (mapped.RowPitch>=b.row && (b.slices<=1 || uint64_t(mapped.DepthPitch)>=uint64_t(mapped.RowPitch)*b.rows)));
                if(!pitchOk) { c->Unmap(b.stage.Get(),0);u32(0);++failures;continue; }
                u32(b.bytes);const auto p=static_cast<const uint8_t*>(mapped.pData);
                if(!b.row)ok=fwrite(p,1,b.bytes,f)==b.bytes && ok;
                else for(uint32_t z=0;z<b.slices;++z)for(uint32_t y=0;y<b.rows;++y)
                    ok=fwrite(p+size_t(z)*mapped.DepthPitch+size_t(y)*mapped.RowPitch,1,b.row,f)==b.row && ok;
                c->Unmap(b.stage.Get(),0);
            }
        }
        std::lock_guard<std::mutex> lock(mutex());u32(count()?2u:0u);
        if(count())for(uint64_t hash:{kVs,kPs}) {
            ok=fwrite(&hash,8,1,f)==1 && ok;auto it=shaders().find(hash);
            if(it==shaders().end()) { u32(0);++failures; }
            else { u32(static_cast<uint32_t>(it->second.size()));ok=fwrite(it->second.data(),1,it->second.size(),f)==it->second.size() && ok; }
        }
        u32(failures);ok=!ferror(f) && ok;return fclose(f)==0 && ok;
    }
};
}
