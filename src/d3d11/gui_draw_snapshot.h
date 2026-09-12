#pragma once
// One explicitly requested source-rendering frame. This observes the GUI
// before it becomes an eye texture; it never changes rendering or settings.
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
class GuiDrawSnapshot {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Blob {Ptr<ID3D11Resource> stage;uint32_t bytes=0,row=0,rows=0,subresource=0;std::string meta;};
    struct Draw {uint64_t vs=0,ps=0;std::string meta;};
    std::vector<Blob> blobs;
    std::vector<Draw> draws;
    std::map<ID3D11Resource*,std::vector<unsigned>> textures;
    std::vector<Ptr<ID3D11Resource>> retained;
    uint32_t frame=0,bytes=0;
    static constexpr uint32_t budget=96*1024*1024;
    static std::map<uint64_t,std::vector<uint8_t>>& shaders(){static std::map<uint64_t,std::vector<uint8_t>> m;return m;}
    static std::mutex& mutex(){static std::mutex m;return m;}
    static const GUID& layoutKey(){static const GUID k={0x3fb76dd8,0x86df,0x4128,{0x93,0x91,0x23,0x95,0x9d,0x46,0x14,0xb2}};return k;}
    template<class T>static std::string words(const T& value){
        static_assert(sizeof(T)%4==0,"word-aligned state");std::ostringstream s;s<<'[';
        for(size_t i=0;i<sizeof(T)/4;++i){uint32_t u;std::memcpy(&u,reinterpret_cast<const char*>(&value)+i*4,4);if(i)s<<',';s<<u;}s<<']';return s.str();
    }
    int buffer(ID3D11DeviceContext* c,ID3D11Device* d,ID3D11Buffer* src,unsigned offset,unsigned limit,unsigned stride,const char* role){
        if(!src)return -1;D3D11_BUFFER_DESC bd{};src->GetDesc(&bd);
        unsigned whole=bd.ByteWidth;if(offset>=whole){++declined;return -1;}
        unsigned n=whole-offset;if(n>limit)n=limit;if(!n || bytes+n>budget){++declined;return -1;}
        bd.ByteWidth=n;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Buffer> stage;if(FAILED(d->CreateBuffer(&bd,nullptr,&stage))){++failures;return -1;}
        D3D11_BOX box{offset,0,0,offset+n,1,1};c->CopySubresourceRegion(stage.Get(),0,0,0,0,src,0,&box);
        Blob b;b.stage=stage;b.bytes=n;std::ostringstream s;s<<"{\"role\":\""<<role<<"\",\"whole\":"<<whole<<",\"offset\":"<<offset<<",\"stride\":"<<stride<<'}';b.meta=s.str();
        blobs.push_back(std::move(b));bytes+=n;return int(blobs.size()-1);
    }
    std::string texture(ID3D11DeviceContext* c,ID3D11Device* d,ID3D11ShaderResourceView* view,ID3D11Texture2D* direct=nullptr,DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN){
        if(!view && !direct)return "null";D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        Ptr<ID3D11Resource> r;
        if(view){view->GetDesc(&sd);view->GetResource(&r);}else{r=direct;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Format=format;sd.Texture2D.MipLevels=1;}
        Ptr<ID3D11Texture2D> t;
        if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || FAILED(r.As(&t))){++declined;return "null";}
        D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);unsigned block=1,bpp=0;
        switch(sd.Format){
        case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_A8_UNORM:bpp=1;break;
        case DXGI_FORMAT_R8G8_UNORM:bpp=2;break;
        case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_D16_UNORM:bpp=2;break;
        case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_D24_UNORM_S8_UINT:bpp=4;break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:bpp=8;break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:bpp=4;break;
        case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:case DXGI_FORMAT_BC4_UNORM:block=4;bpp=8;break;
        case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC2_UNORM_SRGB:case DXGI_FORMAT_BC3_UNORM:case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:block=4;bpp=16;break;
        default:break;
        }
        if(!bpp || td.SampleDesc.Count!=1 || td.ArraySize!=1 || td.MipLevels>16){++declined;return "null";}
        auto found=textures.find(r.Get());
        if(found==textures.end()){
            uint64_t total=0;
            for(UINT i=0;i<td.MipLevels;++i){UINT w=td.Width>>i,h=td.Height>>i;if(!w)w=1;if(!h)h=1;total+=uint64_t((w+block-1)/block)*((h+block-1)/block)*bpp;}
            if(total>16*1024*1024 || bytes+total>budget){++declined;return "null";}
            auto desc=td;desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Ptr<ID3D11Texture2D> stage;if(FAILED(d->CreateTexture2D(&desc,nullptr,&stage))){++failures;return "null";}
            c->CopyResource(stage.Get(),t.Get());
            std::vector<unsigned> ids;
            for(unsigned mip=0;mip<td.MipLevels;++mip){
                unsigned w=td.Width>>mip,h=td.Height>>mip;if(!w)w=1;if(!h)h=1;
                unsigned row=(w+block-1)/block*bpp,rows=(h+block-1)/block,n=row*rows;
                if(uint64_t(row)*rows>16*1024*1024 || bytes+n>budget){++declined;break;}
                Blob b;b.stage=stage;b.bytes=n;b.row=row;b.rows=rows;b.subresource=mip;
                std::ostringstream s;s<<"{\"role\":\"texture\",\"width\":"<<w<<",\"height\":"<<h<<",\"format\":"<<unsigned(sd.Format)<<",\"resource_format\":"<<unsigned(td.Format)<<",\"mip\":"<<mip<<",\"row\":"<<row<<",\"rows\":"<<rows<<'}';b.meta=s.str();
                ids.push_back(unsigned(blobs.size()));blobs.push_back(std::move(b));bytes+=n;
            }
            found=textures.emplace(r.Get(),ids).first;retained.push_back(r);
        }
        std::ostringstream s;s<<"{\"view\":"<<words(sd)<<",\"mips\":[";
        for(size_t i=0;i<found->second.size();++i){if(i)s<<',';s<<found->second[i];}s<<"]}";return s.str();
    }
public:
    uint32_t declined=0,failures=0,missingLayouts=0;
    struct Layout {char semantic[64]{};UINT index=0,format=0,slot=0,offset=0,classification=0,step=0;};
    static bool gui(uint64_t vs){return vs==0x666EF0C4C616F67Eull || vs==0x1012E00B3CB44469ull || vs==0xA3E5D3FCBC1165F8ull;}
    static bool guiPs(uint64_t ps){return ps==0xC0C4E6413DF14E9Aull || ps==0x5A887A688EA10D50ull || ps==0xDE0E1C56AAE678C5ull;}
    static void rememberShader(uint64_t hash,const void* data,size_t n){
        if((!gui(hash)&&!guiPs(hash)) || !data || !n || n>256*1024)return;
        std::lock_guard<std::mutex> lock(mutex());if(shaders().count(hash))return;
        auto* p=static_cast<const uint8_t*>(data);shaders()[hash]=std::vector<uint8_t>(p,p+n);
    }
    static void rememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* e,UINT n,uint64_t shader){
        if(!layout || !e || !n || n>32 || !gui(shader))return;std::vector<Layout> items(n);
        for(UINT i=0;i<n;++i){if(!e[i].SemanticName || strlen(e[i].SemanticName)>=64)return;
            std::strcpy(items[i].semantic,e[i].SemanticName);items[i].index=e[i].SemanticIndex;items[i].format=e[i].Format;items[i].slot=e[i].InputSlot;items[i].offset=e[i].AlignedByteOffset;items[i].classification=e[i].InputSlotClass;items[i].step=e[i].InstanceDataStepRate;}
        layout->SetPrivateData(layoutKey(),UINT(items.size()*sizeof(Layout)),items.data());
    }
    void reset(){*this=GuiDrawSnapshot{};}
    size_t count()const{return draws.size();}
    void capture(ID3D11DeviceContext* c,uint32_t f,uint64_t vs,uint64_t ps,char kind,UINT count,UINT instances,UINT start,int base,UINT firstInstance){
        if(!gui(vs) || (frame && frame!=f) || (kind!='X' && kind!='I') || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(1,&rt,&depth);if(!rt)return;
        Ptr<ID3D11Resource> res;rt->GetResource(&res);Ptr<ID3D11Texture2D> target;if(FAILED(res.As(&target)))return;
        D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
        // The square flight ladder and wide animation strip, recognized
        // together with the actual GUI VS. Capture-only shape selection.
        if(td.Width>2048 || td.Height>2048 || !(td.Width==td.Height || (td.Width>td.Height*3 && td.Width<td.Height*6)))return;
        if(draws.size()>=512 || bytes>=budget){++declined;return;}frame=f;
        Ptr<ID3D11Device> dev;c->GetDevice(&dev);std::ostringstream s;
        s<<"{\"frame\":"<<f<<",\"vs\":\""<<std::hex<<vs<<"\",\"ps\":\""<<ps<<"\",\"target\":\""<<reinterpret_cast<uint64_t>(res.Get())<<std::dec<<"\",\"width\":"<<td.Width<<",\"height\":"<<td.Height<<",\"kind\":\""<<kind<<"\",\"count\":"<<count<<",\"instances\":"<<instances<<",\"start\":"<<start<<",\"base\":"<<base<<",\"first_instance\":"<<firstInstance;
        D3D11_RENDER_TARGET_VIEW_DESC targetView{};rt->GetDesc(&targetView);
        s<<",\"target_view\":"<<words(targetView)<<",\"target_before\":"<<texture(c,dev.Get(),nullptr,target.Get(),targetView.Format);
        UINT nv=16,nr=16;D3D11_VIEWPORT vp[16]{};D3D11_RECT rect[16]{};c->RSGetViewports(&nv,vp);c->RSGetScissorRects(&nr,rect);
        s<<",\"viewports\":[";for(UINT i=0;i<nv && i<16;++i){if(i)s<<',';s<<words(vp[i]);}s<<"],\"scissors\":[";for(UINT i=0;i<nr && i<16;++i){if(i)s<<',';s<<words(rect[i]);}s<<']';
        Ptr<ID3D11RasterizerState> rs;c->RSGetState(&rs);D3D11_RASTERIZER_DESC rd{};if(rs)rs->GetDesc(&rd);s<<",\"rasterizer\":"<<(rs?words(rd):"null");
        Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask;c->OMGetBlendState(&blend,factors,&mask);D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);s<<",\"blend\":"<<(blend?words(bd):"null")<<",\"blend_factors\":"<<words(factors)<<",\"sample_mask\":"<<mask;
        Ptr<ID3D11DepthStencilState> ds;UINT ref;c->OMGetDepthStencilState(&ds,&ref);D3D11_DEPTH_STENCIL_DESC dd{};if(ds)ds->GetDesc(&dd);D3D11_DEPTH_STENCIL_VIEW_DESC dv{};if(depth)depth->GetDesc(&dv);s<<",\"depth_state\":"<<(ds?words(dd):"null")<<",\"stencil_ref\":"<<ref<<",\"depth_view\":"<<(depth?words(dv):"null");
        Ptr<ID3D11Resource> depthRes;Ptr<ID3D11Texture2D> depthTex;if(depth){depth->GetResource(&depthRes);depthRes.As(&depthTex);}
        s<<",\"depth_before\":"<<texture(c,dev.Get(),nullptr,depthTex.Get(),dv.Format);
        D3D11_PRIMITIVE_TOPOLOGY topology;c->IAGetPrimitiveTopology(&topology);s<<",\"topology\":"<<unsigned(topology);
        Ptr<ID3D11InputLayout> layout;c->IAGetInputLayout(&layout);Layout fields[32]{};UINT layoutBytes=sizeof(fields);
        bool got=layout && SUCCEEDED(layout->GetPrivateData(layoutKey(),&layoutBytes,fields)) && layoutBytes%sizeof(Layout)==0 && layoutBytes<=sizeof(fields);
        s<<",\"layout\":[";if(got)for(UINT i=0;i<layoutBytes/sizeof(Layout);++i){if(i)s<<',';s<<words(fields[i]);}else ++missingLayouts;s<<']';
        Ptr<ID3D11Buffer> cb; c->VSGetConstantBuffers(2,1,&cb);s<<",\"vs_cb2\":"<<buffer(c,dev.Get(),cb.Get(),0,8192,0,"vs_cb2");cb.Reset();c->PSGetConstantBuffers(2,1,&cb);s<<",\"ps_cb2\":"<<buffer(c,dev.Get(),cb.Get(),0,8192,0,"ps_cb2");
        Ptr<ID3D11ShaderResourceView> pool;c->VSGetShaderResources(0,1,&pool);Ptr<ID3D11Resource> pr;Ptr<ID3D11Buffer> pb;D3D11_SHADER_RESOURCE_VIEW_DESC pd{};
        if(pool){pool->GetResource(&pr);pr.As(&pb);pool->GetDesc(&pd);}s<<",\"pool_view\":"<<(pool?words(pd):"null")<<",\"pool\":"<<buffer(c,dev.Get(),pb.Get(),0,1024*1024,160,"vs_t0");
        s<<",\"streams\":[";
        for(UINT i=0;i<3;++i){Ptr<ID3D11Buffer> v;UINT stride=0,offset=0;DXGI_FORMAT fmt{};if(i==2){c->IAGetIndexBuffer(&v,&fmt,&offset);stride=fmt==DXGI_FORMAT_R16_UINT?2:fmt==DXGI_FORMAT_R32_UINT?4:0;}else c->IAGetVertexBuffers(i,1,&v,&stride,&offset);
            uint64_t begin=offset;if(i==2)begin+=uint64_t(start)*stride;else if(i==0 && base>0)begin+=uint64_t(base)*stride;
            uint64_t limit=i==2?uint64_t(count)*stride:256*1024;
            int id=-1;
            if(begin<=UINT32_MAX && limit<=16*1024*1024)
                id=buffer(c,dev.Get(),v.Get(),UINT(begin),UINT(limit),stride,i==2?"indices":i==0?"vertices":"instances");
            else ++declined;
            if(i)s<<',';s<<"{\"blob\":"<<id<<",\"offset\":"<<offset<<",\"stride\":"<<stride<<",\"format\":"<<unsigned(fmt)<<'}';
        }s<<"],\"textures\":[";
        for(UINT i=0;i<2;++i){Ptr<ID3D11ShaderResourceView> view;c->PSGetShaderResources(i,1,&view);if(i)s<<',';s<<texture(c,dev.Get(),view.Get());}s<<"],\"samplers\":[";
        for(UINT i=0;i<2;++i){Ptr<ID3D11SamplerState> sampler;c->PSGetSamplers(i,1,&sampler);D3D11_SAMPLER_DESC desc{};if(sampler)sampler->GetDesc(&desc);if(i)s<<',';s<<(sampler?words(desc):"null");}s<<"]}";
        draws.push_back({vs,ps,s.str()});
    }
    bool write(ID3D11DeviceContext* c,const wchar_t* path,const wchar_t* directory){
        FILE* file=nullptr;if(_wfopen_s(&file,path,L"wb") || !file)return false;bool ok=fwrite("EDVRGUI1",1,8,file)==8;
        auto u32=[&](uint32_t v){ok=fwrite(&v,4,1,file)==1 && ok;};auto text=[&](const std::string& s){u32(UINT(s.size()));ok=fwrite(s.data(),1,s.size(),file)==s.size()&&ok;};
        u32(1);u32(UINT(draws.size()));u32(UINT(blobs.size()));u32(declined);u32(missingLayouts);
        for(const auto& d:draws)text(d.meta);
        for(auto& b:blobs){text(b.meta);D3D11_MAPPED_SUBRESOURCE m{};bool mapped=SUCCEEDED(c->Map(b.stage.Get(),b.subresource,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m));bool valid=mapped && m.pData && (!b.row || m.RowPitch>=b.row);u32(valid?b.bytes:0);
            if(valid){if(!b.row)ok=fwrite(m.pData,1,b.bytes,file)==b.bytes&&ok;else for(UINT row=0;row<b.rows;++row)ok=fwrite(static_cast<const char*>(m.pData)+row*m.RowPitch,1,b.row,file)==b.row&&ok;}else ++failures;
            if(mapped)c->Unmap(b.stage.Get(),b.subresource);
        }
        std::lock_guard<std::mutex> lock(mutex());std::map<uint64_t,bool> seen;
        for(const auto& d:draws)for(uint64_t hash:{d.vs,d.ps})if(seen.emplace(hash,true).second){auto it=shaders().find(hash);if(it==shaders().end()){++failures;continue;}
            wchar_t name[MAX_PATH];_snwprintf_s(name,MAX_PATH,_TRUNCATE,L"%s\\%s_%016llX.dxbc",directory,gui(hash)?L"vs":L"ps",static_cast<unsigned long long>(hash));FILE* shader=nullptr;
            if(_wfopen_s(&shader,name,L"wb") || !shader){++failures;continue;}bool wrote=fwrite(it->second.data(),1,it->second.size(),shader)==it->second.size();if(fclose(shader)!=0 || !wrote)++failures;
        }
        u32(failures);ok=!ferror(file)&&ok;return fclose(file)==0&&ok;
    }
};
}
