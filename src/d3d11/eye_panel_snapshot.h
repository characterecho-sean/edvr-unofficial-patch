#pragma once
// Diagnostic only: snapshot the actual holo-panel draw after substitutions.
// No binding changes, draw replay, Flush, or draw-time CPU readback. Resources
// are copied separately for every draw, even when their COM pointers repeat.
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace edvr {
class EyePanelSnapshot {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Layout {
        char semantic[64]{};
        uint32_t index=0,format=0,slot=0,offset=0,classification=0,step=0;
    };
    struct Mip {
        uint32_t level=0,width=0,height=0,row=0,rows=0,offset=0,size=0;
    };
    struct Blob {
        std::string name,meta,reason;
        Ptr<ID3D11Resource> stage,source;
        std::vector<uint8_t> cpu;
        std::vector<Mip> mips;
        uint32_t bytes=0,x=0,y=0;
        bool ready=false;
    };
    struct Draw {
        std::string meta;
        std::vector<std::string> reasons;
        std::vector<Blob> blobs;
    };
    std::vector<Draw> draws_;
    Ptr<ID3D11Resource> target_;
    Ptr<ID3D11Texture2D> depthScratch_;
    uint32_t firstFrame_=0,pending_=UINT32_MAX;
    static constexpr uint64_t kPayloadCap=768ull*1024*1024;
    static constexpr uint64_t kScratchCap=128ull*1024*1024;
    static constexpr uint32_t kBufferCap=32*1024*1024,kVertexCap=256*1024,kCrop=1400;

    static const GUID& shaderKey() {
        static const GUID g={0x4356eea2,0x4a49,0x4094,{0xb4,0x94,0x3a,0x51,0xf1,0x71,0x36,0x84}};
        return g;
    }
    static const GUID& layoutKey() {
        static const GUID g={0x0db0ca71,0x6a19,0x4ca8,{0x9b,0x44,0x51,0x22,0x17,0x0e,0x4e,0x91}};
        return g;
    }
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::map<uint64_t,std::vector<uint8_t>>& shaders() {
        static std::map<uint64_t,std::vector<uint8_t>> m; return m;
    }
    static std::string quote(const std::string& value) {
        std::string out="\"";
        for (unsigned char ch:value) {
            if(ch=='"' || ch=='\\')out+='\\';
            if(ch<32) { char escape[7];sprintf_s(escape,"\\u%04x",unsigned(ch));out+=escape; }
            else out+=char(ch);
        }
        return out+'"';
    }
    template<class T> static std::string words(const T& desc) {
        static_assert(sizeof(T)%4==0,"word descriptor");
        std::ostringstream s;s<<'[';
        for(size_t i=0;i<sizeof(T)/4;++i) {
            uint32_t v=0;memcpy(&v,reinterpret_cast<const uint8_t*>(&desc)+i*4,4);
            if(i)s<<',';s<<v;
        }
        s<<']';return s.str();
    }
    static uint64_t identity(const void* p) { return uint64_t(reinterpret_cast<uintptr_t>(p)); }
    static uint64_t actualShader(ID3D11DeviceChild* shader) {
        uint64_t hash=0;UINT size=sizeof(hash);
        return shader && SUCCEEDED(shader->GetPrivateData(shaderKey(),&size,&hash)) && size==sizeof(hash)?hash:0;
    }
    static std::string resourceMeta(ID3D11Resource* resource) {
        std::string out=",\"resource\":"+std::to_string(identity(resource));
        if(!resource)return out;
        Ptr<ID3D11Buffer> buffer;Ptr<ID3D11Texture2D> texture;
        if(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) {
            D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);out+=",\"resource_desc\":"+words(d);
        } else if(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
            D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);out+=",\"resource_desc\":"+words(d);
        }
        return out;
    }
    bool reserve(uint64_t n,Blob& blob) {
        if(!n || n>kPayloadCap-bytes || n>UINT32_MAX) {blob.reason="payload_budget";return false;}
        blob.bytes=uint32_t(n);return true;
    }
    bool allocation(HRESULT hr,Blob& blob) {
        if(FAILED(hr)) {blob.reason="allocation_failed";++failures;return false;}
        bytes+=blob.bytes;return true;
    }
    // Strict storage/view pairs. Unexpected formats retain their descriptors
    // but are explicitly incomplete rather than pretending to be RGBA8.
    static bool textureFormat(DXGI_FORMAT storage,DXGI_FORMAT view,
                              DXGI_FORMAT& typed,uint32_t& unit,bool& blocks) {
        typed=view;blocks=false;
        switch(view) {
        case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R16G16B16A16_FLOAT:
            unit=8;return storage==view || storage==DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            unit=4;return storage==view || storage==DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:
            blocks=true;unit=8;return storage==view || storage==DXGI_FORMAT_BC1_TYPELESS;
        case DXGI_FORMAT_R11G11B10_FLOAT:unit=4;return storage==view;
        case DXGI_FORMAT_R8_UNORM:unit=1;return storage==view || storage==DXGI_FORMAT_R8_TYPELESS;
        case DXGI_FORMAT_R32_FLOAT:unit=4;return storage==view || storage==DXGI_FORMAT_R32_TYPELESS;
        default:return false;
        }
    }
    static std::string mipMeta(const std::vector<Mip>& mips) {
        std::ostringstream s;s<<",\"mips\":[";
        for(size_t i=0;i<mips.size();++i) {
            const auto& m=mips[i];if(i)s<<',';
            s<<"{\"level\":"<<m.level<<",\"width\":"<<m.width<<",\"height\":"<<m.height
             <<",\"row_bytes\":"<<m.row<<",\"rows\":"<<m.rows<<",\"offset\":"<<m.offset
             <<",\"size\":"<<m.size<<'}';
        }
        s<<']';return s.str();
    }
    void buffer(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11Buffer* source,
                uint64_t offset,uint64_t size,uint32_t cap,Blob& b) {
        b.meta="\"type\":\"buffer\""+resourceMeta(source);
        D3D11_BUFFER_DESC original{};if(source)source->GetDesc(&original);
        b.meta+=",\"whole\":"+std::to_string(original.ByteWidth)+",\"offset\":"+std::to_string(offset);
        if(!source) {b.reason="absent";return;}
        if(!size || size>cap || offset>original.ByteWidth || size>original.ByteWidth-offset) {
            b.reason="buffer_range_or_cap";return;
        }
        if(!reserve(size,b))return;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=uint32_t(size);desc.Usage=D3D11_USAGE_STAGING;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Buffer> stage;
        if(!allocation(dev->CreateBuffer(&desc,nullptr,&stage),b))return;
        b.stage=stage;
        D3D11_BOX box{uint32_t(offset),0,0,uint32_t(offset+size),1,1};
        ctx->CopySubresourceRegion(stage.Get(),0,0,0,0,source,0,&box);b.ready=true;
    }
    void constant(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11Buffer* src,
                  uint32_t minimum,Blob& b) {
        D3D11_BUFFER_DESC d{};if(src)src->GetDesc(&d);
        buffer(ctx,dev,src,0,d.ByteWidth,8192,b);
        b.meta+=",\"minimum\":"+std::to_string(minimum);
        if(d.ByteWidth<minimum)b.reason="constant_buffer_too_short";
    }
    void structured(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11ShaderResourceView* view,
                    uint32_t stride,Blob& b) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};Ptr<ID3D11Resource> r;Ptr<ID3D11Buffer> src;
        if(view) {view->GetDesc(&vd);view->GetResource(&r);r.As(&src);}
        D3D11_BUFFER_DESC d{};if(src)src->GetDesc(&d);
        const bool valid=src && vd.ViewDimension==D3D11_SRV_DIMENSION_BUFFER &&
            vd.Format==DXGI_FORMAT_UNKNOWN && d.StructureByteStride==stride &&
            (d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && vd.Buffer.NumElements &&
            (uint64_t(vd.Buffer.FirstElement)+vd.Buffer.NumElements)*stride<=d.ByteWidth;
        if(valid)buffer(ctx,dev,src.Get(),0,d.ByteWidth,kBufferCap,b);
        else {b.meta="\"type\":\"buffer\""+resourceMeta(r.Get());b.reason="structured_view_or_stride";}
        b.meta+=",\"view\":"+words(vd)+",\"stride\":"+std::to_string(d.StructureByteStride);
    }
    void sampledTexture(ID3D11DeviceContext* ctx,ID3D11Device* dev,
                        ID3D11ShaderResourceView* view,Blob& b) {
        Ptr<ID3D11Resource> r;Ptr<ID3D11Texture2D> src;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};D3D11_TEXTURE2D_DESC original{};
        if(view) {view->GetDesc(&vd);view->GetResource(&r);r.As(&src);}
        if(src)src->GetDesc(&original);
        b.meta="\"type\":\"texture\""+resourceMeta(r.Get())+",\"view\":"+words(vd);
        if(!src || vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || original.ArraySize!=1 ||
           original.SampleDesc.Count!=1 || (original.BindFlags&D3D11_BIND_DEPTH_STENCIL)) {
            b.reason="texture_shape_or_view";return;
        }
        const uint32_t first=vd.Texture2D.MostDetailedMip;
        const uint32_t levels=vd.Texture2D.MipLevels==UINT32_MAX && first<original.MipLevels?
            original.MipLevels-first:vd.Texture2D.MipLevels;
        if(!levels || first>=original.MipLevels || levels>original.MipLevels-first) {
            b.reason="texture_mip_view";return;
        }
        DXGI_FORMAT typed=DXGI_FORMAT_UNKNOWN;uint32_t unit=0;bool blocks=false;
        if(!textureFormat(original.Format,vd.Format,typed,unit,blocks)) {
            b.reason="texture_format_pair";return;
        }
        uint64_t total=0;
        for(uint32_t level=0;level<original.MipLevels;++level) {
            const uint32_t w=std::max(1u,original.Width>>level),h=std::max(1u,original.Height>>level);
            const uint32_t row=(blocks?(w+3)/4:w)*unit,rows=blocks?(h+3)/4:h;
            const uint64_t n=uint64_t(row)*rows;
            if(total+n>64ull*1024*1024) {b.reason="texture_resource_cap";return;}
            b.mips.push_back({level,w,h,row,rows,uint32_t(total),uint32_t(n)});total+=n;
        }
        b.meta+=",\"storage_format\":"+std::to_string(typed)+",\"origin\":[0,0],\"view_first_mip\":"+
            std::to_string(first)+",\"view_mips\":"+std::to_string(levels)+mipMeta(b.mips);
        if(!reserve(total,b))return;
        auto d=original;d.Format=typed;d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;
        d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Texture2D> stage;
        if(!allocation(dev->CreateTexture2D(&d,nullptr,&stage),b))return;
        b.stage=stage;ctx->CopyResource(stage.Get(),src.Get());b.ready=true;
    }
    void crop(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11Texture2D* src,
              bool depth,bool after,Blob& b) {
        D3D11_TEXTURE2D_DESC original{};if(src)src->GetDesc(&original);
        b.meta="\"type\":\"texture\""+resourceMeta(src);
        const uint32_t unit=depth?8:4;
        const bool format=depth?(original.Format==DXGI_FORMAT_R32G8X24_TYPELESS ||
            original.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT):original.Format==DXGI_FORMAT_R11G11B10_FLOAT;
        if(!src || !format || original.ArraySize!=1 || original.MipLevels!=1 || original.SampleDesc.Count!=1) {
            b.reason="crop_shape_or_format";return;
        }
        const uint32_t w=std::min(kCrop,original.Width),h=std::min(kCrop,original.Height);
        b.x=(original.Width-w)/2;b.y=(original.Height-h)/2;
        const uint64_t n=uint64_t(w)*h*unit;
        b.mips.push_back({0,w,h,w*unit,h,0,uint32_t(n)});
        b.meta+=",\"storage_format\":"+std::to_string(original.Format)+",\"origin\":["+
            std::to_string(b.x)+","+std::to_string(b.y)+"]"+mipMeta(b.mips);
        if(!reserve(n,b))return;
        Ptr<ID3D11Texture2D> copySource=src;
        if(depth) {
            // D3D11 forbids partial source depth copies. Copy the whole
            // subresource to an unbound default texture, then crop that.
            if(uint64_t(original.Width)*original.Height*unit>kScratchCap) {
                b.reason="depth_scratch_cap";return;
            }
            D3D11_TEXTURE2D_DESC scratch{};if(depthScratch_)depthScratch_->GetDesc(&scratch);
            if(!depthScratch_ || scratch.Width!=original.Width || scratch.Height!=original.Height || scratch.Format!=original.Format) {
                depthScratch_.Reset();scratch=original;scratch.Usage=D3D11_USAGE_DEFAULT;
                scratch.BindFlags=scratch.MiscFlags=scratch.CPUAccessFlags=0;
                if(FAILED(dev->CreateTexture2D(&scratch,nullptr,&depthScratch_))) {
                    b.reason="depth_scratch_allocation";++failures;return;
                }
            }
            ctx->CopyResource(depthScratch_.Get(),src);copySource=depthScratch_;
        }
        auto d=original;d.Width=w;d.Height=h;d.Usage=D3D11_USAGE_STAGING;
        d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Texture2D> stage;
        if(!allocation(dev->CreateTexture2D(&d,nullptr,&stage),b))return;
        b.stage=stage;
        if(after)b.source=src;
        else {
            D3D11_BOX box{b.x,b.y,0,b.x+w,b.y+h,1};
            ctx->CopySubresourceRegion(stage.Get(),0,0,0,0,copySource.Get(),0,&box);b.ready=true;
        }
    }
    static std::string layoutMeta(const Layout* layout,uint32_t count) {
        std::ostringstream s;s<<",\"layout\":[";
        for(uint32_t i=0;i<count;++i) {
            const auto& e=layout[i];if(i)s<<',';
            s<<'['<<quote(e.semantic)<<','<<e.index<<','<<e.format<<','<<e.slot<<','<<e.offset
             <<','<<e.classification<<','<<e.step<<']';
        }
        s<<']';return s.str();
    }
    void geometry(ID3D11DeviceContext* ctx,ID3D11Device* dev,Draw& draw,char kind,
                  uint32_t n,uint32_t instances,uint32_t startInstance,uint32_t start,int32_t base) {
        Ptr<ID3D11InputLayout> il;ctx->IAGetInputLayout(&il);
        Layout layout[32]{};UINT size=sizeof(layout);
        if(!il || FAILED(il->GetPrivateData(layoutKey(),&size,layout)) || !size || size%sizeof(Layout) || size>sizeof(layout)) {
            draw.reasons.push_back("missing_input_layout");return;
        }
        const uint32_t elements=size/sizeof(Layout);draw.meta+=layoutMeta(layout,elements);
        const bool indexed=kind=='X' || kind=='I';
        if(indexed) {
            Ptr<ID3D11Buffer> ib;DXGI_FORMAT format{};UINT offset=0;ctx->IAGetIndexBuffer(&ib,&format,&offset);
            const uint32_t stride=format==DXGI_FORMAT_R16_UINT?2:format==DXGI_FORMAT_R32_UINT?4:0;
            Blob b;b.name="ib";
            buffer(ctx,dev,ib.Get(),uint64_t(offset)+uint64_t(start)*stride,uint64_t(n)*stride,kBufferCap,b);
            b.meta+=",\"format\":"+std::to_string(format)+",\"binding_offset\":"+std::to_string(offset);
            if(!stride)b.reason="index_format";draw.blobs.push_back(std::move(b));
        }
        ID3D11Buffer* raw[32]{};UINT strides[32]{},offsets[32]{};
        ctx->IAGetVertexBuffers(0,32,raw,strides,offsets);
        Ptr<ID3D11Buffer> vbs[32];for(uint32_t i=0;i<32;++i)vbs[i].Attach(raw[i]);
        bool seen[32]{};
        for(uint32_t i=0;i<elements;++i) {
            const auto& e=layout[i];
            if(e.slot>=32) {draw.reasons.push_back("layout_slot_range");continue;}
            if(seen[e.slot])continue;seen[e.slot]=true;
            const uint32_t slot=e.slot,stride=strides[slot];
            bool valid=e.classification==D3D11_INPUT_PER_VERTEX_DATA ||
                (e.classification==D3D11_INPUT_PER_INSTANCE_DATA && e.step>0);
            for(uint32_t j=i+1;j<elements;++j)if(layout[j].slot==slot &&
                (layout[j].classification!=e.classification || layout[j].step!=e.step))valid=false;
            const uint64_t first=e.classification==D3D11_INPUT_PER_INSTANCE_DATA?startInstance:
                indexed?uint64_t(std::max(0,base)):start;
            const uint64_t off=uint64_t(offsets[slot])+first*stride;
            D3D11_BUFFER_DESC d{};if(vbs[slot])vbs[slot]->GetDesc(&d);
            const uint64_t available=off<d.ByteWidth?d.ByteWidth-off:0;
            Blob b;b.name="vb"+std::to_string(slot);
            buffer(ctx,dev,vbs[slot].Get(),off,std::min<uint64_t>(available,kVertexCap),kVertexCap,b);
            b.meta+=",\"stride\":"+std::to_string(stride)+",\"binding_offset\":"+std::to_string(offsets[slot])+
                ",\"inputclass\":"+std::to_string(e.classification)+",\"step\":"+std::to_string(e.step);
            if(!valid || !stride)b.reason="vertex_class_or_stride";
            draw.blobs.push_back(std::move(b));
        }
        if(!n || !instances || (kind!='X' && kind!='I' && kind!='N' && kind!='V'))draw.reasons.push_back("draw_shape");
        // Index values are checked by the reader after nonblocking readback.
        // No draw-time Map or guessed maximum index is permitted here.
    }
    void shaderBlob(uint64_t hash,const char* name,Draw& draw) {
        Blob b;b.name=name;b.meta="\"type\":\"shader\",\"hash\":"+std::to_string(hash);
        std::lock_guard<std::mutex> guard(mutex());const auto it=shaders().find(hash);
        if(it==shaders().end())b.reason="missing_shader_bytecode";
        else if(reserve(it->second.size(),b)) {b.cpu=it->second;b.ready=true;bytes+=b.bytes;}
        draw.blobs.push_back(std::move(b));
    }
public:
    static constexpr uint64_t kVs=0x81216C77F90DEDD6ull,kPs=0xA2965EC2931A39C8ull;
    uint32_t declined=0,failures=0,ignoredOtherEye=0,ignoredOtherFrame=0;
    uint64_t bytes=0;
    uint32_t count() const { return uint32_t(draws_.size()); }
    uint32_t firstFrame() const { return firstFrame_; }
    uint64_t target() const { return identity(target_.Get()); }
    void reset() {
        draws_.clear();target_.Reset();depthScratch_.Reset();firstFrame_=0;pending_=UINT32_MAX;
        declined=failures=ignoredOtherEye=ignoredOtherFrame=0;bytes=0;
    }
    static void rememberShader(uint64_t hash,const void* data,size_t n,ID3D11DeviceChild* shader=nullptr) {
        if((hash!=kVs && hash!=kPs) || !data || !n || n>256*1024)return;
        if(shader)shader->SetPrivateData(shaderKey(),sizeof(hash),&hash);
        std::lock_guard<std::mutex> guard(mutex());
        const auto* p=static_cast<const uint8_t*>(data);shaders()[hash]=std::vector<uint8_t>(p,p+n);
    }
    static void rememberLayout(ID3D11InputLayout* il,const D3D11_INPUT_ELEMENT_DESC* e,UINT n,uint64_t hash) {
        if(hash!=kVs || !il || !e || !n || n>32)return;
        Layout saved[32]{};
        for(UINT i=0;i<n;++i) {
            if(!e[i].SemanticName || strlen(e[i].SemanticName)>=sizeof(saved[i].semantic))return;
            strcpy_s(saved[i].semantic,e[i].SemanticName);saved[i].index=e[i].SemanticIndex;
            saved[i].format=e[i].Format;saved[i].slot=e[i].InputSlot;saved[i].offset=e[i].AlignedByteOffset;
            saved[i].classification=e[i].InputSlotClass;saved[i].step=e[i].InstanceDataStepRate;
        }
        il->SetPrivateData(layoutKey(),n*sizeof(Layout),saved);
    }
    void captureRequested(ID3D11DeviceContext* ctx,uint32_t lo,uint32_t hi,uint32_t frame,uint32_t ordinal,
                          uint64_t vs,uint64_t ps,char kind,uint32_t n,uint32_t instances,
                          uint32_t startInstance,uint32_t start,int32_t base) {
        if(frame<lo || frame>hi || !ctx || vs!=kVs || ps!=kPs)return;
        if(firstFrame_ && frame!=firstFrame_) {++ignoredOtherFrame;return;}
        ID3D11RenderTargetView* raw[8]{};Ptr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(8,raw,&dsv);
        Ptr<ID3D11RenderTargetView> rt[8];for(uint32_t i=0;i<8;++i)rt[i].Attach(raw[i]);
        Ptr<ID3D11Resource> r;Ptr<ID3D11Texture2D> targetTexture;
        if(rt[0]) {rt[0]->GetResource(&r);r.As(&targetTexture);}
        if(target_ && r.Get()!=target_.Get()) {++ignoredOtherEye;return;}
        if(count()>=16 || pending_!=UINT32_MAX) {++declined;return;}
        D3D11_RENDER_TARGET_VIEW_DESC rv{};if(rt[0])rt[0]->GetDesc(&rv);
        D3D11_TEXTURE2D_DESC targetDesc{};if(targetTexture)targetTexture->GetDesc(&targetDesc);
        const bool validRT=rv.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && !rv.Texture2D.MipSlice;
        const bool eligible=validRT && targetDesc.Format==DXGI_FORMAT_R11G11B10_FLOAT &&
            targetDesc.ArraySize==1 && targetDesc.MipLevels==1 && targetDesc.SampleDesc.Count==1;
        // Preserve invalid candidate evidence without allowing it to choose
        // the capture frame/eye before the first actual HDR panel draw.
        if(!firstFrame_ && eligible)firstFrame_=frame;
        if(!target_ && eligible)target_=r;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        Draw draw;std::ostringstream s;
        s<<"\"frame\":"<<frame<<",\"ordinal\":"<<ordinal<<",\"vs\":"<<vs<<",\"ps\":"<<ps
         <<",\"kind\":"<<unsigned(uint8_t(kind))<<",\"count\":"<<n<<",\"instances\":"<<instances
         <<",\"start_instance\":"<<startInstance<<",\"start\":"<<start<<",\"base\":"<<base;
        Ptr<ID3D11VertexShader> vshader;Ptr<ID3D11PixelShader> pshader;UINT vc=0,pc=0;
        ctx->VSGetShader(&vshader,nullptr,&vc);ctx->PSGetShader(&pshader,nullptr,&pc);
        if(actualShader(vshader.Get())!=vs || actualShader(pshader.Get())!=ps)
            draw.reasons.push_back("substituted_or_unregistered_shader");
        if(vc || pc)draw.reasons.push_back("shader_class_instances");
        Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> ds;
        ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
        if(gs || hs || ds)draw.reasons.push_back("unsupported_shader_stage");
        Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);
        if(predicate)draw.reasons.push_back("predication");
        for(uint32_t i=1;i<8;++i)if(rt[i])draw.reasons.push_back("extra_render_target");
        s<<",\"rtv_view\":"<<words(rv);
        if(!validRT)draw.reasons.push_back("render_target_view");
        for(uint32_t phase=0;phase<2;++phase) {
            Blob b;b.name=phase?"rtv_after":"rtv_before";
            if(validRT)crop(ctx,dev.Get(),targetTexture.Get(),false,phase!=0,b);
            else {b.meta="\"type\":\"texture\""+resourceMeta(r.Get());b.reason="render_target_view";}
            draw.blobs.push_back(std::move(b));
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};Ptr<ID3D11Resource> dr;Ptr<ID3D11Texture2D> depthTexture;
        if(dsv) {dsv->GetDesc(&dv);dsv->GetResource(&dr);dr.As(&depthTexture);}
        s<<",\"dsv_view\":"<<(dsv?words(dv):"null");
        if(dsv) {
            Blob b;b.name="dsv_roi";
            const bool valid=dv.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D && !dv.Texture2D.MipSlice &&
                dv.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            if(valid)crop(ctx,dev.Get(),depthTexture.Get(),true,false,b);
            else {b.meta="\"type\":\"texture\""+resourceMeta(dr.Get());b.reason="depth_stencil_view";}
            draw.blobs.push_back(std::move(b));
        }
        Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;
        ctx->OMGetBlendState(&blend,factors,&mask);D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);
        Ptr<ID3D11DepthStencilState> depth;UINT ref=0;
        ctx->OMGetDepthStencilState(&depth,&ref);D3D11_DEPTH_STENCIL_DESC dd{};if(depth)depth->GetDesc(&dd);
        Ptr<ID3D11RasterizerState> raster;ctx->RSGetState(&raster);D3D11_RASTERIZER_DESC rd{};if(raster)raster->GetDesc(&rd);
        s<<",\"blend\":"<<(blend?words(bd):"null")<<",\"blend_factor\":"<<words(factors)<<",\"sample_mask\":"<<mask
         <<",\"depth\":"<<(depth?words(dd):"null")<<",\"stencil_ref\":"<<ref<<",\"raster\":"<<(raster?words(rd):"null");
        D3D11_VIEWPORT vp[16]{};D3D11_RECT sc[16]{};UINT vcount=16,scount=16;
        ctx->RSGetViewports(&vcount,vp);ctx->RSGetScissorRects(&scount,sc);
        s<<",\"viewports\":[";for(UINT i=0;i<vcount && i<16;++i) {if(i)s<<',';s<<words(vp[i]);}
        s<<"],\"scissors\":[";for(UINT i=0;i<scount && i<16;++i) {if(i)s<<',';s<<words(sc[i]);}
        s<<"],\"samplers\":[";
        ID3D11SamplerState* samplers[2]{};ctx->PSGetSamplers(0,2,samplers);
        for(uint32_t i=0;i<2;++i) {
            Ptr<ID3D11SamplerState> q;q.Attach(samplers[i]);D3D11_SAMPLER_DESC sd{};if(q)q->GetDesc(&sd);
            if(i)s<<',';s<<(q?words(sd):"null");
        }
        D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);
        s<<"],\"topology\":"<<topology;draw.meta=s.str();
        ID3D11ShaderResourceView* psRaw[3]{};ctx->PSGetShaderResources(0,3,psRaw);
        for(uint32_t i=0;i<3;++i) {
            Ptr<ID3D11ShaderResourceView> q;q.Attach(psRaw[i]);Blob b;b.name="ps_t"+std::to_string(i);
            sampledTexture(ctx,dev.Get(),q.Get(),b);draw.blobs.push_back(std::move(b));
        }
        ID3D11Buffer* cbRaw[3]{};ctx->VSGetConstantBuffers(0,3,cbRaw);
        const uint32_t minimums[3]={12*16,276*16,4*16};
        for(uint32_t i=0;i<3;++i) {
            Ptr<ID3D11Buffer> q;q.Attach(cbRaw[i]);Blob b;b.name="vs_b"+std::to_string(i);
            constant(ctx,dev.Get(),q.Get(),minimums[i],b);draw.blobs.push_back(std::move(b));
        }
        ID3D11Buffer* psCb[2]{};ctx->PSGetConstantBuffers(1,2,psCb);
        for(uint32_t i=0;i<2;++i) {
            Ptr<ID3D11Buffer> q;q.Attach(psCb[i]);Blob b;b.name="ps_b"+std::to_string(i+1);
            constant(ctx,dev.Get(),q.Get(),i?13*16:327*16,b);draw.blobs.push_back(std::move(b));
        }
        for(uint32_t i=0;i<2;++i) {
            Ptr<ID3D11ShaderResourceView> q;ctx->VSGetShaderResources(i?38:33,1,&q);
            Blob b;b.name=i?"vs_t38":"vs_t33";structured(ctx,dev.Get(),q.Get(),i?48:336,b);
            draw.blobs.push_back(std::move(b));
        }
        geometry(ctx,dev.Get(),draw,kind,n,instances,startInstance,start,base);
        shaderBlob(vs,"shader_vs",draw);shaderBlob(ps,"shader_ps",draw);
        bool complete=draw.reasons.empty();for(const auto& b:draw.blobs)complete=complete && b.reason.empty();
        if(!complete)++declined;
        pending_=count();draws_.push_back(std::move(draw));
    }
    void end(ID3D11DeviceContext* ctx) {
        if(!ctx || pending_==UINT32_MAX)return;
        for(auto& b:draws_[pending_].blobs)if(b.name=="rtv_after" && b.source && b.stage) {
            const auto& m=b.mips[0];D3D11_BOX box{b.x,b.y,0,b.x+m.width,b.y+m.height,1};
            ctx->CopySubresourceRegion(b.stage.Get(),0,0,0,0,b.source.Get(),0,&box);
            b.ready=true;b.source.Reset();
        }
        pending_=UINT32_MAX;
    }
    bool write(ID3D11DeviceContext* ctx,const wchar_t* path) {
        if(!ctx || !path)return false;
        FILE* f=nullptr;if(_wfopen_s(&f,path,L"wb") || !f)return false;
        bool ok=fwrite("EDVRPNL1",1,8,f)==8;
        auto u32=[&](uint32_t v) {ok=fwrite(&v,4,1,f)==1 && ok;};
        auto u64=[&](uint64_t v) {ok=fwrite(&v,8,1,f)==1 && ok;};
        auto text=[&](const std::string& value) {u32(uint32_t(value.size()));ok=fwrite(value.data(),1,value.size(),f)==value.size() && ok;};
        u32(1);u32(count());u32(firstFrame_);u32(declined);u32(failures);u32(ignoredOtherEye);u32(ignoredOtherFrame);
        u64(target());u64(bytes);
        for(auto& draw:draws_) {
            // Hold maps only for this draw, so completeness is known before
            // its metadata is written. A missing mip invalidates the blob.
            std::vector<std::vector<D3D11_MAPPED_SUBRESOURCE>> maps(draw.blobs.size());
            std::vector<std::string> reasons(draw.blobs.size());
            bool complete=draw.reasons.empty();
            for(size_t i=0;i<draw.blobs.size();++i) {
                auto& b=draw.blobs[i];auto& reason=reasons[i];reason=b.reason;
                if(reason.empty() && !b.ready)reason="end_not_called";
                if(reason.empty() && b.cpu.empty()) {
                    if(!b.stage)reason="missing_staging";
                    else for(uint32_t level=0;level<std::max<size_t>(1,b.mips.size());++level) {
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        const HRESULT hr=ctx->Map(b.stage.Get(),level,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                        if(FAILED(hr)) {reason=hr==DXGI_ERROR_WAS_STILL_DRAWING?"readback_pending":"readback_failed";break;}
                        maps[i].push_back(mapped);
                        if(!mapped.pData || (!b.mips.empty() && mapped.RowPitch<b.mips[level].row)) {
                            reason="mapped_row_pitch";break;
                        }
                    }
                }
                if(!reason.empty()) {complete=false;if(b.reason.empty())++failures;}
            }
            std::ostringstream dm;dm<<'{'<<draw.meta<<",\"complete\":"<<(complete?"true":"false")<<",\"reasons\":[";
            for(size_t i=0;i<draw.reasons.size();++i) {if(i)dm<<',';dm<<quote(draw.reasons[i]);}
            dm<<"]}";text(dm.str());u32(uint32_t(draw.blobs.size()));
            for(size_t i=0;i<draw.blobs.size();++i) {
                const auto& b=draw.blobs[i];const auto& reason=reasons[i];const bool valid=reason.empty();
                text(b.name);text("{"+b.meta+",\"bytes\":"+std::to_string(b.bytes)+",\"complete\":"+
                    (valid?"true":"false")+",\"reason\":"+quote(reason)+"}");
                u32(valid?b.bytes:0);
                if(valid) {
                    if(!b.cpu.empty())ok=fwrite(b.cpu.data(),1,b.cpu.size(),f)==b.cpu.size() && ok;
                    else if(b.mips.empty())ok=fwrite(maps[i][0].pData,1,b.bytes,f)==b.bytes && ok;
                    else for(size_t level=0;level<b.mips.size();++level) {
                        const auto& mip=b.mips[level];const auto& mapped=maps[i][level];
                        const auto* data=static_cast<const uint8_t*>(mapped.pData);
                        for(uint32_t row=0;row<mip.rows;++row)
                            ok=fwrite(data+size_t(row)*mapped.RowPitch,1,mip.row,f)==mip.row && ok;
                    }
                }
                for(uint32_t level=0;level<maps[i].size();++level)ctx->Unmap(b.stage.Get(),level);
            }
        }
        u32(failures);return fclose(f)==0 && ok;
    }
};
} // namespace edvr
