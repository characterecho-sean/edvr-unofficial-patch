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
    bool postTone=false;
    bool allowVolatileUavSrv=false;
    bool compactPanelIa=false;
};
inline uint32_t uiDeferredWord(const std::vector<uint8_t>& bytes,size_t offset) {
    uint32_t value=0;if(offset>bytes.size() || bytes.size()-offset<sizeof(value))return 0;std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
}
// Elite's shipped shaders commonly omit RDEF. D3DReflect still succeeds on
// those containers, but reports zero bound resources. Read the authoritative
// SM5 declarations, and accept only the immediate, fixed-slot forms
// that UiDeferredDraw knows how to snapshot and bind.
inline bool uiDeferredDeclaredInputs(const std::vector<uint8_t>& bytes,uint32_t& cb,std::array<bool,128>& srv) {
    if(bytes.size()<36 || std::memcmp(bytes.data(),"DXBC",4) || uiDeferredWord(bytes,24)!=bytes.size())return false;
    const uint32_t chunks=uiDeferredWord(bytes,28);if(!chunks || chunks>128 || size_t(32)+size_t(chunks)*4>bytes.size())return false;
    bool found=false;
    for(uint32_t i=0;i<chunks;++i) {
        const uint32_t offset=uiDeferredWord(bytes,32+size_t(i)*4);if(offset>bytes.size() || bytes.size()-offset<8)return false;
        const uint32_t tag=uiDeferredWord(bytes,offset),size=uiDeferredWord(bytes,offset+4);if(size>bytes.size()-(offset+8))return false;
        if(tag!=0x58454853u && tag!=0x52444853u)continue;
        if(found || size<8 || (size&3))return false;found=true;
        const size_t begin=offset+8,words=size/4;const uint32_t version=uiDeferredWord(bytes,begin);
        if((version&0xffffu)!=0x50u || (version>>16)>1 || uiDeferredWord(bytes,begin+4)!=words)return false;
        for(size_t p=2;p<words;) {
            const uint32_t token=uiDeferredWord(bytes,begin+p*4),op=token&0x7ffu;
            uint32_t length=(token>>24)&0x7fu;if(op==53){if(p+1>=words || (token>>11)!=3)return false;length=uiDeferredWord(bytes,begin+(p+1)*4);if(length<2 || (length-2)%4)return false;}
            if(!length || length>words-p)return false;
            auto wordAt=[&](size_t n){return uiDeferredWord(bytes,begin+(p+n)*4);};
            if(op==88 || op==161 || op==162) {
                const uint32_t expected=op==161?3u:4u;if((token&0x80000000u) || length!=expected || wordAt(1)!=0x00107000u)return false;
                const uint32_t slot=wordAt(2);if(slot>=srv.size() || (op==162 && !wordAt(3)))return false;srv[slot]=true;
            } else if(op==89) {
                if((token&0x80000000u) || length!=4 || wordAt(1)!=0x00208e46u)return false;
                const uint32_t slot=wordAt(2),constants=wordAt(3);if(slot>=14 || constants>4096)return false;cb|=1u<<slot;
            } else if(op==90) {
                if((token&0x80000000u) || length!=3 || wordAt(1)!=0x00106000u || wordAt(2)>=16)return false;
            } else if(op==120 || (op>=144 && op<=146) || (op>=156 && op<=158))return false;
            p+=length;
        }
    }
    return found;
}
// Reflect actual shader inputs; stale unused bindings are not draw inputs.
inline bool uiDeferredReflect(ID3D11DeviceChild* shader,uint32_t& cb,std::array<bool,128>& srv) {
    cb=0;srv={};UINT n=0;shader->GetPrivateData(kDeferredBytes,&n,nullptr);
    if(!n || n>1024*1024)return false;
    std::vector<uint8_t> bytes(n);
    if(FAILED(shader->GetPrivateData(kDeferredBytes,&n,bytes.data())))return false;
    if(!uiDeferredDeclaredInputs(bytes,cb,srv))return false;
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    if(FAILED(D3DReflect(bytes.data(),n,IID_PPV_ARGS(&reflection))))return false;
    D3D11_SHADER_DESC d{};if(FAILED(reflection->GetDesc(&d)))return false;
    for(UINT i=0;i<d.BoundResources;++i) {
        D3D11_SHADER_INPUT_BIND_DESC r{};if(FAILED(reflection->GetResourceBindingDesc(i,&r)))return false;
        if(r.Type==D3D_SIT_SAMPLER){if(r.BindPoint+r.BindCount>16)return false;continue;}
        if(r.Type==D3D_SIT_CBUFFER){if(r.BindPoint+r.BindCount>14)return false;continue;}
        if(r.Type!=D3D_SIT_TEXTURE && r.Type!=D3D_SIT_STRUCTURED && r.Type!=D3D_SIT_BYTEADDRESS && r.Type!=D3D_SIT_TBUFFER)return false;
        if(r.BindPoint+r.BindCount>srv.size())return false;
    }
    return true;
}
inline bool uiDeferredPanelVertexInputs(ID3D11VertexShader* shader) {
    UINT n=0;if(!shader || FAILED(shader->GetPrivateData(kDeferredBytes,&n,nullptr)) || !n || n>1024*1024)return false;
    std::vector<uint8_t> bytes(n);if(FAILED(shader->GetPrivateData(kDeferredBytes,&n,bytes.data())))return false;
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;if(FAILED(D3DReflect(bytes.data(),n,IID_PPV_ARGS(&reflection))))return false;
    D3D11_SHADER_DESC d{};if(FAILED(reflection->GetDesc(&d)) || d.InputParameters!=4)return false;
    const char* semantics[]={"INSTANCEANDMODELDATAINDEX","PACKEDVERTEXDATAA","PACKEDVERTEXDATAB","PACKEDVERTEXDATAC"};
    const BYTE masks[]={3,15,15,15};
    for(UINT i=0;i<4;++i){D3D11_SIGNATURE_PARAMETER_DESC p{};if(FAILED(reflection->GetInputParameterDesc(i,&p)) || !p.SemanticName ||
        std::strcmp(p.SemanticName,semantics[i]) || p.SemanticIndex || p.Register!=i || p.SystemValueType!=D3D_NAME_UNDEFINED ||
        p.ComponentType!=D3D_REGISTER_COMPONENT_UINT32 || p.Mask!=masks[i])return false;}
    return true;
}

// One GPU snapshot per source version, shared by all UI packets. A write
// advances the version; older packets retain their old copy. Storage is
// recycled only after the frame ends. There is no readback, wait or flush.
class UiDeferredSnapshots {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Copy { Ptr<ID3D11Resource> resource;uint64_t frame=0,version=0;size_t bytes=0;bool volatileSrv=false; };
    struct RangeCopy { Ptr<ID3D11Buffer> resource;uint64_t frame=0,version=0;UINT offset=0,bytes=0,bind=0; };
    struct Source { Ptr<ID3D11Resource> original;uint64_t version=0,lastFrame=0;std::vector<Copy> copies;std::vector<RangeCopy> ranges; };
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
            if(frame_-i->second.lastFrame>2){for(auto& c:i->second.copies)allocated_-=c.bytes;for(auto& c:i->second.ranges)allocated_-=c.bytes;i=sources_.erase(i);}else ++i;
        }
    }
    size_t copiedBytes()const{return copied_;}
    size_t allocatedBytes()const{return allocated_;}
    bool capture(ID3D11DeviceContext* ctx,ID3D11Resource* r,Ptr<ID3D11Resource>& out,bool reflectedSrv=false,bool allowVolatileUavSrv=false,bool allowTrackedUavSrv=false) {
        out.Reset();if(!r)return true;
        D3D11_RESOURCE_DIMENSION kind{};r->GetType(&kind);
        D3D11_BUFFER_DESC bd{};D3D11_TEXTURE2D_DESC td{};D3D11_TEXTURE3D_DESC vd{};
        D3D11_USAGE usage=D3D11_USAGE_STAGING;size_t bytes=0;bool freshSnapshot=false;
        if(kind==D3D11_RESOURCE_DIMENSION_BUFFER){static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);usage=bd.Usage;bytes=bd.ByteWidth;
            if((bd.BindFlags&D3D11_BIND_STREAM_OUTPUT) || ((bd.BindFlags&D3D11_BIND_UNORDERED_ACCESS) && (!reflectedSrv || (!allowVolatileUavSrv && !allowTrackedUavSrv))))return false;
            freshSnapshot=reflectedSrv && allowVolatileUavSrv && (bd.BindFlags&D3D11_BIND_UNORDERED_ACCESS);
        } else if(kind==D3D11_RESOURCE_DIMENSION_TEXTURE2D){static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);usage=td.Usage;
            if(td.SampleDesc.Count!=1)return false;bytes=imageSize(td.Width,td.Height,1,td.MipLevels,td.Format)*td.ArraySize;
        } else if(kind==D3D11_RESOURCE_DIMENSION_TEXTURE3D){static_cast<ID3D11Texture3D*>(r)->GetDesc(&vd);usage=vd.Usage;bytes=imageSize(vd.Width,vd.Height,vd.Depth,vd.MipLevels,vd.Format);
        } else return false;
        if(usage==D3D11_USAGE_IMMUTABLE){out=r;return true;}
        if(!bytes || bytes>budget_ || usage==D3D11_USAGE_STAGING)return false;
        auto& entry=sources_[r];entry.original=r;entry.lastFrame=frame_;
        if(!freshSnapshot)for(auto& c:entry.copies)if(!c.volatileSrv && c.frame==frame_ && c.version==entry.version){out=c.resource;return true;}
        Copy* copy=nullptr;for(auto& c:entry.copies)if(c.frame!=frame_ && c.volatileSrv==freshSnapshot){copy=&c;break;}
        if(!copy) {
            if(bytes>budget_-allocated_)return false;
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);Ptr<ID3D11Resource> resource;
            if(kind==D3D11_RESOURCE_DIMENSION_BUFFER){bd.Usage=D3D11_USAGE_DEFAULT;bd.CPUAccessFlags=0;if(freshSnapshot || allowTrackedUavSrv)bd.BindFlags&=~D3D11_BIND_UNORDERED_ACCESS;Ptr<ID3D11Buffer> b;if(FAILED(dev->CreateBuffer(&bd,nullptr,&b)))return false;resource=b;}
            else if(kind==D3D11_RESOURCE_DIMENSION_TEXTURE2D){td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.MiscFlags&=D3D11_RESOURCE_MISC_TEXTURECUBE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;Ptr<ID3D11Texture2D> t;if(FAILED(dev->CreateTexture2D(&td,nullptr,&t)))return false;resource=t;}
            else {vd.Usage=D3D11_USAGE_DEFAULT;vd.CPUAccessFlags=vd.MiscFlags=0;vd.BindFlags=D3D11_BIND_SHADER_RESOURCE;Ptr<ID3D11Texture3D> t;if(FAILED(dev->CreateTexture3D(&vd,nullptr,&t)))return false;resource=t;}
            entry.copies.push_back({resource,0,0,bytes,freshSnapshot});copy=&entry.copies.back();allocated_+=bytes;
        }
        ctx->CopyResource(copy->resource.Get(),r);copied_+=bytes;copy->frame=frame_;copy->version=entry.version;out=copy->resource;return true;
    }
    bool captureRange(ID3D11DeviceContext* ctx,ID3D11Buffer* source,uint64_t offset,uint64_t bytes,UINT bind,Ptr<ID3D11Buffer>& out) {
        out.Reset();if(!ctx || !source || !bytes || bytes>UINT32_MAX || offset>UINT32_MAX || offset+bytes>UINT32_MAX)return false;
        if(bind!=D3D11_BIND_VERTEX_BUFFER && bind!=D3D11_BIND_INDEX_BUFFER)return false;
        D3D11_BUFFER_DESC sourceDesc{};source->GetDesc(&sourceDesc);
        if(offset+bytes>sourceDesc.ByteWidth || sourceDesc.Usage==D3D11_USAGE_STAGING || (sourceDesc.BindFlags&bind)!=bind)return false;
        auto& entry=sources_[source];entry.original=source;entry.lastFrame=frame_;
        const UINT begin=UINT(offset),length=UINT(bytes);
        for(auto& c:entry.ranges)if(c.frame==frame_ && c.version==entry.version && c.offset==begin && c.bytes==length && c.bind==bind){out=c.resource;return true;}
        RangeCopy* copy=nullptr;for(auto& c:entry.ranges)if(c.frame!=frame_ && c.bytes==length && c.bind==bind){copy=&c;break;}
        if(!copy){if(bytes>budget_-allocated_)return false;D3D11_BUFFER_DESC d{};d.ByteWidth=length;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);Ptr<ID3D11Buffer> b;if(FAILED(dev->CreateBuffer(&d,nullptr,&b)))return false;
            entry.ranges.push_back({b,0,0,0,length,bind});copy=&entry.ranges.back();allocated_+=length;}
        D3D11_BOX box{begin,0,0,begin+length,1,1};ctx->CopySubresourceRegion(copy->resource.Get(),0,0,0,0,source,0,&box);copied_+=length;
        copy->frame=frame_;copy->version=entry.version;copy->offset=begin;out=copy->resource;return true;
    }
};

class UiDeferredDraw {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct PanelLayout {
        char semantic[64]{};
        uint32_t index=0,format=0,slot=0,offset=0,classification=0,step=0;
    };
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
    const char* failure_=nullptr;
    bool fail(const char* why){failure_=why;return false;}
    static const GUID& panelLayoutKey(){static const GUID g={0x0db0ca71,0x6a19,0x4ca8,{0x9b,0x44,0x51,0x22,0x17,0x0e,0x4e,0x91}};return g;}
    static bool panelLayout(ID3D11InputLayout* layout) {
        PanelLayout actual[4]{};UINT bytes=sizeof(actual);if(!layout || FAILED(layout->GetPrivateData(panelLayoutKey(),&bytes,actual)) || bytes!=sizeof(actual))return false;
        const PanelLayout expected[]={
            {{'I','N','S','T','A','N','C','E','A','N','D','M','O','D','E','L','D','A','T','A','I','N','D','E','X',0},0,DXGI_FORMAT_R32G32_UINT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {{'P','A','C','K','E','D','V','E','R','T','E','X','D','A','T','A','A',0},0,DXGI_FORMAT_R32G32B32A32_UINT,1,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {{'P','A','C','K','E','D','V','E','R','T','E','X','D','A','T','A','B',0},0,DXGI_FORMAT_R32G32B32A32_UINT,1,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {{'P','A','C','K','E','D','V','E','R','T','E','X','D','A','T','A','C',0},0,DXGI_FORMAT_R32G32_UINT,1,32,D3D11_INPUT_PER_VERTEX_DATA,0}};
        return std::memcmp(actual,expected,sizeof(actual))==0;
    }
    static bool exactBuffer(ID3D11Buffer* buffer,UINT bytes,D3D11_USAGE usage,UINT bind,UINT cpu) {
        if(!buffer)return false;D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);return d.ByteWidth==bytes && d.Usage==usage && d.BindFlags==bind && d.CPUAccessFlags==cpu && !d.MiscFlags && !d.StructureByteStride;
    }
    bool capturePanelIa(ID3D11DeviceContext* c,UiDeferredSnapshots& pool,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
        if(kind!='X' || !count || instances!=1 || base<0 || topology_!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || !panelLayout(layout_.Get()))return fail("panel IA layout or draw shape changed");
        ID3D11Buffer* raw[32]{};c->IAGetVertexBuffers(0,32,raw,strides_.data(),offsets_.data());std::array<Ptr<ID3D11Buffer>,32> source;for(UINT i=0;i<32;++i)source[i].Attach(raw[i]);
        if(strides_[0]!=8 || offsets_[0] || strides_[1]!=40 || offsets_[1] || !exactBuffer(source[0].Get(),32768,D3D11_USAGE_DYNAMIC,D3D11_BIND_VERTEX_BUFFER,D3D11_CPU_ACCESS_WRITE) ||
            !exactBuffer(source[1].Get(),130023424,D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER,0))return fail("panel vertex buffer contract changed");
        Ptr<ID3D11Buffer> index;c->IAGetIndexBuffer(&index,&ibFormat_,&ibOffset_);
        if(ibFormat_!=DXGI_FORMAT_R16_UINT || ibOffset_ || !exactBuffer(index.Get(),33554432,D3D11_USAGE_DEFAULT,D3D11_BIND_INDEX_BUFFER,0))return fail("panel index buffer contract changed");
        const uint64_t instanceBegin=uint64_t(first)*8,indexBegin=uint64_t(start)*2,indexBytes=uint64_t(count)*2;
        const uint64_t vertexBegin=uint64_t(base)*40,vertexBytes=uint64_t(65536)*40;
        if(instanceBegin+8>32768 || indexBegin+indexBytes>33554432 || vertexBegin+vertexBytes>130023424)return fail("panel IA range outside retained buffers");
        if(!pool.captureRange(c,source[0].Get(),instanceBegin,8,D3D11_BIND_VERTEX_BUFFER,vb_[0]) ||
            !pool.captureRange(c,source[1].Get(),vertexBegin,vertexBytes,D3D11_BIND_VERTEX_BUFFER,vb_[1]) ||
            !pool.captureRange(c,index.Get(),indexBegin,indexBytes,D3D11_BIND_INDEX_BUFFER,ib_))return fail("panel IA range snapshot unavailable");
        offsets_={};ibOffset_=0;start_=0;base_=0;first_=0;return true;
    }
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
    static bool copyView(ID3D11DeviceContext* ctx,UiDeferredSnapshots& pool,ID3D11ShaderResourceView* v,Ptr<ID3D11ShaderResourceView>& out,bool allowVolatileUavSrv,bool trackedPanelT38=false) {
        out.Reset();if(!v)return true;Ptr<ID3D11Resource> source,copy;v->GetResource(&source);
        if(trackedPanelT38){Ptr<ID3D11Buffer> b;if(FAILED(source.As(&b)))return false;D3D11_BUFFER_DESC bd{};b->GetDesc(&bd);D3D11_SHADER_RESOURCE_VIEW_DESC sd{};v->GetDesc(&sd);
            if(bd.ByteWidth!=8388624 || bd.Usage!=D3D11_USAGE_DEFAULT || bd.BindFlags!=(D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS) || bd.CPUAccessFlags || bd.MiscFlags!=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED || bd.StructureByteStride!=48 ||
               sd.Format!=DXGI_FORMAT_UNKNOWN || sd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || sd.Buffer.FirstElement || sd.Buffer.NumElements!=174763)return false;}
        if(!pool.capture(ctx,source.Get(),copy,true,allowVolatileUavSrv,trackedPanelT38))return false;
        if(source==copy){out=v;return true;}
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};v->GetDesc(&d);Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        return SUCCEEDED(dev->CreateShaderResourceView(copy.Get(),&d,&out));
    }
public:
    bool ready()const{return kind_!=0;}
    const char* failureReason()const{return failure_?failure_:"unspecified capture failure";}
    ID3D11VertexShader* originalVertexShader()const{return vs_.Get();}
    ID3D11PixelShader* originalPixelShader()const{return ps_.Get();}
    ID3D11BlendState* blendState()const{return blend_.Get();}
    ID3D11DepthStencilState* depthState()const{return depth_.Get();}
    UINT stencilRef()const{return stencil_;}
    D3D11_VIEWPORT originalViewport()const{return viewport_;}
    D3D11_RECT originalScissor()const{return scissor_;}
    ID3D11Buffer* psBuffer(UINT i)const{return pcb_[i].Get();}
    ID3D11ShaderResourceView* psResource(UINT i)const{return psr_[i].Get();}
    ID3D11ShaderResourceView* vsResource(UINT i)const{return vsr_[i].Get();}
    ID3D11SamplerState* psSampler(UINT i)const{return pss_[i].Get();}
    ID3D11SamplerState* vsSampler(UINT i)const{return vss_[i].Get();}
    void setBlend(ID3D11DeviceContext* c,ID3D11BlendState* b)const{c->OMSetBlendState(b,factors_,sampleMask_);}
    bool capture(ID3D11DeviceContext* c,UiDeferredSnapshots& pool,char kind,UINT count,UINT instances,UINT start,INT base,UINT first,const UiDeferredCaptureMask& mask) {
        *this={};if(!c || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !simple(c) || (kind!='D' && kind!='I' && kind!='N' && kind!='X'))return fail("draw context or pipeline state unsupported");
        ID3D11ClassInstance* classes[256]{};UINT n=256;c->VSGetShader(&vs_,classes,&n);for(UINT i=0;i<n;++i)classes[i]->Release();if(n)return fail("vertex shader class instances unsupported");
        n=256;c->PSGetShader(&ps_,classes,&n);for(UINT i=0;i<n;++i)classes[i]->Release();if(n || !vs_ || !ps_)return fail("pixel shader or class instances unsupported");
        c->IAGetInputLayout(&layout_);c->IAGetPrimitiveTopology(&topology_);if(topology_==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)return fail("input topology unavailable");
        if(mask.compactPanelIa){if(!capturePanelIa(c,pool,kind,count,instances,start,base,first))return false;}
        else {
            // Input registers are not IA slots: the planetary HUD can carry
            // several attributes in a stream. Keep every bound stream intact.
            ID3D11Buffer* vb[32]{};c->IAGetVertexBuffers(0,32,vb,strides_.data(),offsets_.data());
            bool ok=true;for(UINT i=0;i<32;++i){Ptr<ID3D11Buffer> original;original.Attach(vb[i]);if(ok)ok=copyBuffer(c,pool,original.Get(),vb_[i]);}if(!ok)return fail("vertex buffer snapshot unavailable");
            Ptr<ID3D11Buffer> ib;c->IAGetIndexBuffer(&ib,&ibFormat_,&ibOffset_);
            if(kind=='I' || kind=='X'){if(!ib || !copyBuffer(c,pool,ib.Get(),ib_))return fail("index buffer snapshot unavailable");}
            start_=start;base_=base;first_=first;
        }
        c->OMGetBlendState(&blend_,factors_,&sampleMask_);c->OMGetDepthStencilState(&depth_,&stencil_);c->RSGetState(&raster_);
        UINT nv=1;c->RSGetViewports(&nv,&viewport_);scissorCount_=1;c->RSGetScissorRects(&scissorCount_,&scissor_);
        if(nv!=1 || scissorCount_>1)return fail("viewport or scissor count unsupported");
        auto constants=[&](bool vertex,uint32_t bits,auto& output){ID3D11Buffer* b[14]{};if(vertex)c->VSGetConstantBuffers(0,14,b);else c->PSGetConstantBuffers(0,14,b);
            bool good=true;for(UINT i=0;i<14;++i){Ptr<ID3D11Buffer> original;original.Attach(b[i]);if(good && (bits&(1u<<i)))good=original && copyBuffer(c,pool,original.Get(),output[i]);}return good;};
        if(!constants(true,mask.cbVs,vcb_) || !constants(false,mask.cbPs,pcb_))return fail("constant buffer snapshot unavailable");
        for(UINT i=0;i<128;++i){
            if(mask.srvVs[i]){Ptr<ID3D11ShaderResourceView> v;c->VSGetShaderResources(i,1,&v);if(!copyView(c,pool,v.Get(),vsr_[i],mask.allowVolatileUavSrv,mask.compactPanelIa && i==38))return fail(mask.compactPanelIa && i==38?"panel t38 descriptor or tracked snapshot unsupported":"vertex shader resource snapshot unavailable");}
            if(mask.srvPs[i] && !(mask.tone && i==1) && !(mask.postTone && i==0)){Ptr<ID3D11ShaderResourceView> v;c->PSGetShaderResources(i,1,&v);if(!copyView(c,pool,v.Get(),psr_[i],mask.allowVolatileUavSrv))return fail("pixel shader resource snapshot unavailable");}
        }
        ID3D11SamplerState* vss[16]{},*pss[16]{};c->VSGetSamplers(0,16,vss);c->PSGetSamplers(0,16,pss);for(UINT i=0;i<16;++i){vss_[i].Attach(vss[i]);pss_[i].Attach(pss[i]);}
        kind_=kind;count_=count;instances_=instances;failure_=nullptr;return true;
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
