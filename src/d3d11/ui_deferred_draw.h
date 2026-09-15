#pragma once
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstring>

namespace edvr {
inline const GUID kDeferredBytes={0xa12e44d1,0x3f5a,0x40cb,{0x97,0x2a,0xc1,0x58,0xa0,0x12,0x78,0x6a}};
struct UiDeferredCaptureMask {
    uint32_t cbVs=0,cbPs=0;
    std::array<bool,128> srvVs{},srvPs{};
    bool tone=false;
};
// Reflect actual shader inputs; stale unused bindings are not draw inputs.
inline bool uiDeferredReflect(ID3D11DeviceChild* shader,uint32_t& cb,std::array<bool,128>& srv) {
    cb=0;srv={};UINT n=0;shader->GetPrivateData(kDeferredBytes,&n,nullptr);
    if(!n || n>1024*1024)return false;
    std::vector<uint8_t> bytes(n);
    if(FAILED(shader->GetPrivateData(kDeferredBytes,&n,bytes.data())))return false;
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    if(FAILED(D3DReflect(bytes.data(),n,IID_PPV_ARGS(&reflection))))return false;
    D3D11_SHADER_DESC d{};if(FAILED(reflection->GetDesc(&d)))return false;
    for(UINT i=0;i<d.BoundResources;++i) {
        D3D11_SHADER_INPUT_BIND_DESC r{};if(FAILED(reflection->GetResourceBindingDesc(i,&r)))return false;
        if(r.Type==D3D_SIT_SAMPLER){if(r.BindPoint+r.BindCount>16)return false;continue;}
        if(r.Type==D3D_SIT_CBUFFER){if(r.BindPoint+r.BindCount>14)return false;for(UINT j=0;j<r.BindCount;++j)cb|=1u<<(r.BindPoint+j);continue;}
        if(r.Type!=D3D_SIT_TEXTURE && r.Type!=D3D_SIT_STRUCTURED && r.Type!=D3D_SIT_BYTEADDRESS && r.Type!=D3D_SIT_TBUFFER)return false;
        if(r.BindPoint+r.BindCount>srv.size())return false;
        for(UINT j=0;j<r.BindCount;++j)srv[r.BindPoint+j]=true;
    }
    return true;
}

// One GPU snapshot per source version, shared by all UI packets. A write
// advances the version; older packets retain their old copy. Storage is
// recycled only after the frame ends. There is no readback, wait or flush.
class UiDeferredSnapshots {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Copy { Ptr<ID3D11Resource> resource;uint64_t frame=0,version=0;size_t bytes=0; };
    struct Source { Ptr<ID3D11Resource> original;uint64_t version=0,lastFrame=0;std::vector<Copy> copies; };
    std::unordered_map<ID3D11Resource*,Source> sources_;
    uint64_t frame_=1;
    size_t allocated_=0,copied_=0;
    static constexpr size_t budget_=256u*1024u*1024u;
    static size_t formatSize(DXGI_FORMAT f) {
        switch(f) {
        case DXGI_FORMAT_R8_TYPELESS:case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_UINT:return 1;
        case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_UINT:case DXGI_FORMAT_D16_UNORM:case DXGI_FORMAT_R8G8_UNORM:return 2;
        case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_UINT:case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16G16_UNORM:return 4;
        case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
        default:return 0;
        }
    }
    static size_t imageSize(UINT w,UINT h,UINT z,UINT mips,DXGI_FORMAT format) {
        size_t block=0;
        switch(format){
        case DXGI_FORMAT_BC1_TYPELESS:case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:case DXGI_FORMAT_BC4_UNORM:case DXGI_FORMAT_BC4_SNORM:block=8;break;
        case DXGI_FORMAT_BC2_TYPELESS:case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:case DXGI_FORMAT_BC3_UNORM:case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:case DXGI_FORMAT_BC5_UNORM:case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:case DXGI_FORMAT_BC6H_UF16:case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:block=16;break;
        default:break;
        }
        size_t bpp=formatSize(format),total=0;if(!bpp && !block)return 0;
        for(UINT i=0;i<mips;++i){total+=block?size_t((w+3)/4)*((h+3)/4)*z*block:size_t(w)*h*z*bpp;w=(w>1?w/2:1);h=(h>1?h/2:1);z=(z>1?z/2:1);}return total;
    }
public:
    void written(ID3D11Resource* r){auto i=sources_.find(r);if(i!=sources_.end())++i->second.version;}
    void unknownWrite(){for(auto& i:sources_)++i.second.version;}
    void frameBoundary() {
        ++frame_;copied_=0;
        for(auto i=sources_.begin();i!=sources_.end();) {
            if(frame_-i->second.lastFrame>2){for(auto& c:i->second.copies)allocated_-=c.bytes;i=sources_.erase(i);}else ++i;
        }
    }
    size_t copiedBytes()const{return copied_;}
    size_t allocatedBytes()const{return allocated_;}
    bool capture(ID3D11DeviceContext* ctx,ID3D11Resource* r,Ptr<ID3D11Resource>& out) {
        out.Reset();if(!r)return true;
        D3D11_RESOURCE_DIMENSION kind{};r->GetType(&kind);
        D3D11_BUFFER_DESC bd{};D3D11_TEXTURE2D_DESC td{};D3D11_TEXTURE3D_DESC vd{};
        D3D11_USAGE usage=D3D11_USAGE_STAGING;size_t bytes=0;
        if(kind==D3D11_RESOURCE_DIMENSION_BUFFER){static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);usage=bd.Usage;bytes=bd.ByteWidth;
            if(bd.BindFlags&(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_STREAM_OUTPUT))return false;
        } else if(kind==D3D11_RESOURCE_DIMENSION_TEXTURE2D){static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);usage=td.Usage;
            if(td.SampleDesc.Count!=1)return false;bytes=imageSize(td.Width,td.Height,1,td.MipLevels,td.Format)*td.ArraySize;
        } else if(kind==D3D11_RESOURCE_DIMENSION_TEXTURE3D){static_cast<ID3D11Texture3D*>(r)->GetDesc(&vd);usage=vd.Usage;bytes=imageSize(vd.Width,vd.Height,vd.Depth,vd.MipLevels,vd.Format);
        } else return false;
        if(usage==D3D11_USAGE_IMMUTABLE){out=r;return true;}
        if(!bytes || bytes>budget_ || usage==D3D11_USAGE_STAGING)return false;
        auto& entry=sources_[r];entry.original=r;entry.lastFrame=frame_;
        for(auto& c:entry.copies)if(c.frame==frame_ && c.version==entry.version){out=c.resource;return true;}
        Copy* copy=nullptr;for(auto& c:entry.copies)if(c.frame!=frame_){copy=&c;break;}
        if(!copy) {
            if(bytes>budget_-allocated_)return false;
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);Ptr<ID3D11Resource> resource;
            if(kind==D3D11_RESOURCE_DIMENSION_BUFFER){bd.Usage=D3D11_USAGE_DEFAULT;bd.CPUAccessFlags=0;Ptr<ID3D11Buffer> b;if(FAILED(dev->CreateBuffer(&bd,nullptr,&b)))return false;resource=b;}
            else if(kind==D3D11_RESOURCE_DIMENSION_TEXTURE2D){td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.MiscFlags&=D3D11_RESOURCE_MISC_TEXTURECUBE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;Ptr<ID3D11Texture2D> t;if(FAILED(dev->CreateTexture2D(&td,nullptr,&t)))return false;resource=t;}
            else {vd.Usage=D3D11_USAGE_DEFAULT;vd.CPUAccessFlags=vd.MiscFlags=0;vd.BindFlags=D3D11_BIND_SHADER_RESOURCE;Ptr<ID3D11Texture3D> t;if(FAILED(dev->CreateTexture3D(&vd,nullptr,&t)))return false;resource=t;}
            entry.copies.push_back({resource,0,0,bytes});copy=&entry.copies.back();allocated_+=bytes;
        }
        ctx->CopyResource(copy->resource.Get(),r);copied_+=bytes;copy->frame=frame_;copy->version=entry.version;out=copy->resource;return true;
    }
};

class UiDeferredDraw {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11VertexShader> vs_;Ptr<ID3D11PixelShader> ps_;Ptr<ID3D11InputLayout> layout_;
    std::array<Ptr<ID3D11Buffer>,14> vcb_,pcb_;
    std::array<Ptr<ID3D11ShaderResourceView>,128> vsr_,psr_;
    std::array<Ptr<ID3D11SamplerState>,16> vss_,pss_;
    std::array<Ptr<ID3D11Buffer>,32> vb_;
    std::array<UINT,32> strides_{},offsets_{};
    Ptr<ID3D11Buffer> ib_;DXGI_FORMAT ibFormat_=DXGI_FORMAT_UNKNOWN;UINT ibOffset_=0;
    Ptr<ID3D11BlendState> blend_;FLOAT factors_[4]{};UINT sampleMask_=~0u;
    Ptr<ID3D11DepthStencilState> depth_;UINT stencil_=0;Ptr<ID3D11RasterizerState> raster_;
    D3D11_VIEWPORT viewport_{};D3D11_RECT scissor_{};UINT scissorCount_=0;
    D3D11_PRIMITIVE_TOPOLOGY topology_=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    char kind_=0;UINT count_=0,instances_=0,start_=0,first_=0;INT base_=0;
    static bool simple(ID3D11DeviceContext* c) {
        Ptr<ID3D11GeometryShader> g;Ptr<ID3D11HullShader> h;Ptr<ID3D11DomainShader> d;
        c->GSGetShader(&g,nullptr,nullptr);c->HSGetShader(&h,nullptr,nullptr);c->DSGetShader(&d,nullptr,nullptr);if(g||h||d)return false;
        ID3D11Buffer* so[4]{};c->SOGetTargets(4,so);bool busy=false;for(auto* p:so)if(p){busy=true;p->Release();}
        ID3D11UnorderedAccessView* u[8]{};c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,u);for(auto* p:u)if(p){busy=true;p->Release();}
        Ptr<ID3D11Predicate> predicate;BOOL value=FALSE;c->GetPredication(&predicate,&value);return !busy && !predicate;
    }
    static bool copyBuffer(ID3D11DeviceContext* ctx,UiDeferredSnapshots& pool,ID3D11Buffer* b,Ptr<ID3D11Buffer>& out) {
        out.Reset();if(!b)return true;Ptr<ID3D11Resource> r;return pool.capture(ctx,b,r) && SUCCEEDED(r.As(&out));
    }
    static bool copyView(ID3D11DeviceContext* ctx,UiDeferredSnapshots& pool,ID3D11ShaderResourceView* v,Ptr<ID3D11ShaderResourceView>& out) {
        out.Reset();if(!v)return true;Ptr<ID3D11Resource> source,copy;v->GetResource(&source);if(!pool.capture(ctx,source.Get(),copy))return false;
        if(source==copy){out=v;return true;}
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};v->GetDesc(&d);Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        return SUCCEEDED(dev->CreateShaderResourceView(copy.Get(),&d,&out));
    }
public:
    bool ready()const{return kind_!=0;}
    ID3D11VertexShader* originalVertexShader()const{return vs_.Get();}
    ID3D11PixelShader* originalPixelShader()const{return ps_.Get();}
    ID3D11BlendState* blendState()const{return blend_.Get();}
    ID3D11DepthStencilState* depthState()const{return depth_.Get();}
    D3D11_VIEWPORT originalViewport()const{return viewport_;}
    D3D11_RECT originalScissor()const{return scissor_;}
    ID3D11Buffer* psBuffer(UINT i)const{return pcb_[i].Get();}
    ID3D11ShaderResourceView* psResource(UINT i)const{return psr_[i].Get();}
    ID3D11ShaderResourceView* vsResource(UINT i)const{return vsr_[i].Get();}
    ID3D11SamplerState* psSampler(UINT i)const{return pss_[i].Get();}
    ID3D11SamplerState* vsSampler(UINT i)const{return vss_[i].Get();}
    void setBlend(ID3D11DeviceContext* c,ID3D11BlendState* b)const{c->OMSetBlendState(b,factors_,sampleMask_);}
    bool capture(ID3D11DeviceContext* c,UiDeferredSnapshots& pool,char kind,UINT count,UINT instances,UINT start,INT base,UINT first,const UiDeferredCaptureMask& mask) {
        *this={};if(!c || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !simple(c) || (kind!='D' && kind!='I' && kind!='N' && kind!='X'))return false;
        ID3D11ClassInstance* classes[256]{};UINT n=256;c->VSGetShader(&vs_,classes,&n);for(UINT i=0;i<n;++i)classes[i]->Release();if(n)return false;
        n=256;c->PSGetShader(&ps_,classes,&n);for(UINT i=0;i<n;++i)classes[i]->Release();if(n || !vs_ || !ps_)return false;
        c->IAGetInputLayout(&layout_);c->IAGetPrimitiveTopology(&topology_);if(topology_==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)return false;
        // Input registers are not IA slots: the planetary HUD can carry
        // several attributes in a stream. Keep every bound stream intact.
        ID3D11Buffer* vb[32]{};c->IAGetVertexBuffers(0,32,vb,strides_.data(),offsets_.data());
        bool ok=true;for(UINT i=0;i<32;++i){Ptr<ID3D11Buffer> original;original.Attach(vb[i]);if(ok)ok=copyBuffer(c,pool,original.Get(),vb_[i]);}if(!ok)return false;
        Ptr<ID3D11Buffer> ib;c->IAGetIndexBuffer(&ib,&ibFormat_,&ibOffset_);
        if(kind=='I' || kind=='X'){if(!ib || !copyBuffer(c,pool,ib.Get(),ib_))return false;}
        c->OMGetBlendState(&blend_,factors_,&sampleMask_);c->OMGetDepthStencilState(&depth_,&stencil_);c->RSGetState(&raster_);
        UINT nv=1;c->RSGetViewports(&nv,&viewport_);scissorCount_=1;c->RSGetScissorRects(&scissorCount_,&scissor_);
        if(nv!=1 || scissorCount_>1)return false;
        auto constants=[&](bool vertex,uint32_t bits,auto& output){ID3D11Buffer* b[14]{};if(vertex)c->VSGetConstantBuffers(0,14,b);else c->PSGetConstantBuffers(0,14,b);
            bool good=true;for(UINT i=0;i<14;++i){Ptr<ID3D11Buffer> original;original.Attach(b[i]);if(good && (bits&(1u<<i)))good=original && copyBuffer(c,pool,original.Get(),output[i]);}return good;};
        if(!constants(true,mask.cbVs,vcb_) || !constants(false,mask.cbPs,pcb_))return false;
        for(UINT i=0;i<128;++i){
            if(mask.srvVs[i]){Ptr<ID3D11ShaderResourceView> v;c->VSGetShaderResources(i,1,&v);if(!copyView(c,pool,v.Get(),vsr_[i]))return false;}
            if(mask.srvPs[i] && !(mask.tone && i==1)){Ptr<ID3D11ShaderResourceView> v;c->PSGetShaderResources(i,1,&v);if(!copyView(c,pool,v.Get(),psr_[i]))return false;}
        }
        ID3D11SamplerState* vss[16]{},*pss[16]{};c->VSGetSamplers(0,16,vss);c->PSGetSamplers(0,16,pss);for(UINT i=0;i<16;++i){vss_[i].Attach(vss[i]);pss_[i].Attach(pss[i]);}
        kind_=kind;count_=count;instances_=instances;start_=start;first_=first;base_=base;return true;
    }
    bool bind(ID3D11DeviceContext* c,ID3D11PixelShader* ps,ID3D11RenderTargetView* r0,ID3D11RenderTargetView* r2,ID3D11DepthStencilView* depth,const D3D11_VIEWPORT& vp,const D3D11_RECT* scissor=nullptr)const {
        if(!ready() || !c || c->GetType()!=D3D11_DEVICE_CONTEXT_DEFERRED || !ps || !r0)return false;
        ID3D11RenderTargetView* rt[3]={r0,nullptr,r2};c->OMSetRenderTargets(r2?3:1,rt,depth);setBlend(c,blend_.Get());c->OMSetDepthStencilState(depth_.Get(),stencil_);
        c->RSSetState(raster_.Get());c->RSSetViewports(1,&vp);c->RSSetScissorRects(scissorCount_,scissor?scissor:&scissor_);
        c->IASetInputLayout(layout_.Get());c->IASetPrimitiveTopology(topology_);ID3D11Buffer* vb[32]{};for(UINT i=0;i<32;++i)vb[i]=vb_[i].Get();c->IASetVertexBuffers(0,32,vb,strides_.data(),offsets_.data());c->IASetIndexBuffer(ib_.Get(),ibFormat_,ibOffset_);
        c->VSSetShader(vs_.Get(),nullptr,0);c->PSSetShader(ps,nullptr,0);
        ID3D11Buffer* vc[14]{},*pc[14]{};for(UINT i=0;i<14;++i){vc[i]=vcb_[i].Get();pc[i]=pcb_[i].Get();}c->VSSetConstantBuffers(0,14,vc);c->PSSetConstantBuffers(0,14,pc);
        ID3D11ShaderResourceView* vr[128]{},*pr[128]{};for(UINT i=0;i<128;++i){vr[i]=vsr_[i].Get();pr[i]=psr_[i].Get();}c->VSSetShaderResources(0,128,vr);c->PSSetShaderResources(0,128,pr);
        ID3D11SamplerState* vs[16]{},*pss[16]{};for(UINT i=0;i<16;++i){vs[i]=vss_[i].Get();pss[i]=pss_[i].Get();}c->VSSetSamplers(0,16,vs);c->PSSetSamplers(0,16,pss);return true;
    }
    void draw(ID3D11DeviceContext* c)const {
        switch(kind_){case 'D':c->Draw(count_,UINT(base_));break;case 'I':c->DrawIndexed(count_,start_,base_);break;case 'N':c->DrawInstanced(count_,instances_,UINT(base_),first_);break;case 'X':c->DrawIndexedInstanced(count_,instances_,start_,base_,first_);break;}
    }
};
}
