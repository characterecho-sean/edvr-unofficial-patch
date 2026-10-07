// Offline diagnostics only. Reads original capture inputs; never loads the proxy DLL.
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <array>
using Microsoft::WRL::ComPtr;
using Bytes=std::vector<unsigned char>;
void check(HRESULT h,const char* label) {
    if(FAILED(h))throw std::runtime_error(std::string(label)+" HRESULT="+std::to_string(uint32_t(h)));
}
struct Reader  {
    std::ifstream f;
    explicit Reader(const std::filesystem::path& p):f(p,std::ios::binary) {
        if(!f)throw std::runtime_error("recipe open");
    }
    void read(void* p,size_t n) {
        if(!f.read(static_cast<char*>(p),n))throw std::runtime_error("truncated recipe");
    }
    uint32_t u() {
        uint32_t v;
        read(&v,4);
        return v;
    }
    std::string str() {
        auto n=u();
        if(n>(1u<<20))throw std::runtime_error("string cap");
        std::string s(n,0);
        read(s.data(),n);
        return s;
    }
};
Bytes hex(const std::string& s) {
    if(s.size()%2)throw std::runtime_error("hex shape");
    Bytes b;
    for(size_t i=0;i<s.size();i+=2)b.push_back(static_cast<unsigned char>(std::stoul(s.substr(i,2),nullptr,16)));
    return b;
}
template<class T>T pod(const std::string& s) {
    auto b=hex(s);
    if(b.size()!=sizeof(T))throw std::runtime_error("POD size "+std::to_string(b.size())+" != "+std::to_string(sizeof(T)));
    T t;
    memcpy(&t,b.data(),sizeof(t));
    return t;
}
struct Part {
    UINT sub,row,plane,rows,depth,bytes;
    std::vector<std::filesystem::path> files;
    Bytes load()const {
        Bytes b(bytes);
        size_t at=0;
        for(auto& p:files) {
            std::ifstream f(p,std::ios::binary);
            if(!f)throw std::runtime_error("payload open");
            while(f) {
                const auto n=std::min<size_t>(1<<20,b.size()-at);
                if(!n) {
                    if(f.peek()!=EOF)throw std::runtime_error("payload excess");
                    break;
                }
                f.read(reinterpret_cast<char*>(b.data()+at),n);
                at+=size_t(f.gcount());
            }
        }
        if(at!=b.size())throw std::runtime_error("payload truncated");
        return b;
    }
};
struct Resource {
    std::string name,type,desc;
    std::vector<Part> parts;
    ComPtr<ID3D11Resource> gpu;
};
struct Recipe {
    UINT count,start;
    INT base;
    UINT instances,startInstance;
    std::map<std::string,std::string> fields;
    std::map<UINT,Resource> resources;
    Bytes vs,ps;
};
Bytes shader(Reader& r) {
    Part p {
    };
    auto n=r.u();
    if(n>64)throw std::runtime_error("shader chunks");
    for(UINT i=0;i<n;++i) {
        auto path=std::filesystem::u8path(r.str());
        auto bytes=std::filesystem::file_size(path);
        if(bytes>(64<<20)-p.bytes)throw std::runtime_error("shader cap");
        p.bytes+=UINT(bytes);
        p.files.push_back(path);
    }
    return p.load();
}
Recipe load(const std::filesystem::path& path) {
    Reader r(path);
    char magic[8];
    r.read(magic,8);
    if(memcmp(magic,"EDVRRP01",8))throw std::runtime_error("recipe version");
    Recipe x {
    };
    x.count=r.u();
    x.start=r.u();
    x.base=INT(r.u());
    x.instances=r.u();
    x.startInstance=r.u();
    auto n=r.u();
    if(n>4096)throw std::runtime_error("field cap");
    for(UINT i=0;i<n;++i) {
        auto k=r.str();
        x.fields[k]=r.str();
    }
    n=r.u();
    if(n>128)throw std::runtime_error("resource cap");
    for(UINT i=0;i<n;++i) {
        auto id=r.u();
        Resource v;
        v.name=r.str();
        v.type=r.str();
        v.desc=r.str();
        auto np=r.u();
        if(np>8192)throw std::runtime_error("part cap");
        for(UINT j=0;j<np;++j) {
            Part p {
            };
            p.sub=r.u();
            p.row=r.u();
            p.plane=r.u();
            p.rows=r.u();
            p.depth=r.u();
            p.bytes=r.u();
            auto nf=r.u();
            if(nf>64||p.bytes>(256u<<20))throw std::runtime_error("payload cap");
            for(UINT k=0;k<nf;++k)p.files.push_back(std::filesystem::u8path(r.str()));
            v.parts.push_back(std::move(p));
        }
        x.resources.emplace(id,std::move(v));
    }
    x.vs=shader(r);
    x.ps=shader(r);
    if(r.f.peek()!=EOF)throw std::runtime_error("recipe trailing bytes");
    return x;
}
template<class T>ComPtr<T> as(Resource& r) {
    ComPtr<T> v;
    check(r.gpu.As(&v),"resource type");
    return v;
}
struct Layout {
    char semantic[64];
    UINT index,format,slot,offset,classification,step;
};
struct Pair {
    UINT a,b;
};
struct Stats {
    uint64_t marked=0;
    UINT left=~0u,top=~0u,right=0,bottom=0;
};
Stats maskStats(const Bytes& b,UINT w,UINT h) {
    if(b.size()!=size_t(w)*h)throw std::runtime_error("mask shape");
    Stats s;
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)if(b[size_t(y)*w+x]) {
        ++s.marked;
        s.left=std::min(s.left,x);
        s.top=std::min(s.top,y);
        s.right=std::max(s.right,x);
        s.bottom=std::max(s.bottom,y);
    }
    return s;
}
double channel(UINT bits,UINT mantissa) {
    auto e=bits>>mantissa,m=bits&((1u<<mantissa)-1);
    if(e==31)return m?NAN:INFINITY;
    return e?std::ldexp(1.0+double(m)/(1u<<mantissa),int(e)-15):std::ldexp(double(m),1-15-int(mantissa));
}
Bytes readback(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* source,UINT rowBytes) {
    D3D11_TEXTURE2D_DESC d {
    };
    source->GetDesc(&d);
    if(d.SampleDesc.Count!=1||d.ArraySize!=1||d.MipLevels!=1)throw std::runtime_error("output shape");
    d.Usage=D3D11_USAGE_STAGING;
    d.BindFlags=d.MiscFlags=0;
    d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    check(dev->CreateTexture2D(&d,nullptr,&stage),"readback create");
    ctx->CopyResource(stage.Get(),source);
    D3D11_MAPPED_SUBRESOURCE m {
    };
    check(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m),"readback map");
    Bytes bytes(size_t(rowBytes)*d.Height);
    for(UINT y=0;y<d.Height;++y)memcpy(bytes.data()+size_t(rowBytes)*y,static_cast<const BYTE*>(m.pData)+size_t(m.RowPitch)*y,rowBytes);
    ctx->Unmap(stage.Get(),0);
    return bytes;
}
void write(const std::filesystem::path& p,const Bytes& b) {
    std::ofstream f(p,std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()),b.size());
    if(!f)throw std::runtime_error("output write");
}
int run(Recipe& x,const std::filesystem::path& out) {
    auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll)throw std::runtime_error("system d3d11");
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    check(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx),"WARP device");
    ComPtr<ID3D11DeviceContext1> ctx1;
    check(ctx.As(&ctx1),"context1");
    for(auto& kv:x.resources) {
        auto& r=kv.second;
        std::vector<Bytes> b;
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        for(auto& p:r.parts) {
            if(p.sub!=b.size())throw std::runtime_error("subresource order");
            b.push_back(p.load());
            data.push_back( {
                b.back().data(),p.row,p.plane
            }
            );
        }
        if(r.type=="D3D11_BUFFER_DESC") {
            auto d=pod<D3D11_BUFFER_DESC>(r.desc);
            if(b.size()!=1 || b[0].size()!=d.ByteWidth)throw std::runtime_error("buffer payload shape");
            d.Usage=D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags=0;
            ComPtr<ID3D11Buffer> v;
            check(dev->CreateBuffer(&d,data.data(),&v),r.name.c_str());
            r.gpu=v;
        }
        else if(r.type=="D3D11_TEXTURE2D_DESC") {
            auto d=pod<D3D11_TEXTURE2D_DESC>(r.desc);
            if(b.size()!=size_t(d.MipLevels)*d.ArraySize)throw std::runtime_error("texture payload count");
            if(d.SampleDesc.Count!=1)throw std::runtime_error("MSAA unsupported");
            d.Usage=D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags=0;
            ComPtr<ID3D11Texture2D> v;
            check(dev->CreateTexture2D(&d,data.data(),&v),r.name.c_str());
            r.gpu=v;
        }
        else throw std::runtime_error("unsupported resource type");
        std::cout<<"created "<<r.name<<"\n";
    }
    auto& f=x.fields;
    for(UINT i=1;i<8;++i)if(f.count("OM.RTV"+std::to_string(i)+".resource"))throw std::runtime_error("multiple original render targets unsupported");
    auto res=[&](const std::string& key)->Resource& {
        return x.resources.at(UINT(std::stoul(f.at(key))));
    };
    for(auto stage: {
        "HS","DS","GS"
    }
    )if(f.at(stage)!= "null")throw std::runtime_error("extra shader stage");
    if(f.at("predicate.object")!="null"&&f.at("predicate.object")!="0")throw std::runtime_error("predication unsupported");
    ComPtr<ID3DBlob> listing;
    check(D3DDisassemble(x.ps.data(),x.ps.size(),0,nullptr,&listing),"PS proof disassembly");
    std::string disasm(static_cast<const char*>(listing->GetBufferPointer()),listing->GetBufferSize());
    for(auto forbidden: {
        "discard","oDepth","oMask","SV_Depth","SV_Coverage"
    }
    )if(disasm.find(forbidden)!=std::string::npos)throw std::runtime_error("PS invalidates solid passing-fragment proof");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    check(dev->CreateVertexShader(x.vs.data(),x.vs.size(),nullptr,&vs),"VS create");
    check(dev->CreatePixelShader(x.ps.data(),x.ps.size(),nullptr,&ps),"PS create");
    std::vector<Layout> layouts;
    for(UINT i=0;f.count("IA.layout"+std::to_string(i));++i)layouts.push_back(pod<Layout>(f.at("IA.layout"+std::to_string(i))));
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    for(auto& l:layouts)elements.push_back( {
        l.semantic,l.index,DXGI_FORMAT(l.format),l.slot,l.offset,D3D11_INPUT_CLASSIFICATION(l.classification),l.step
    }
    );
    ComPtr<ID3D11InputLayout> input;
    check(dev->CreateInputLayout(elements.data(),UINT(elements.size()),x.vs.data(),x.vs.size(),&input),"layout create");
    ctx->IASetInputLayout(input.Get());
    for(UINT i=0;i<32;++i) {
        auto key="IA.VB"+std::to_string(i);
        if(f.at(key)=="null")continue;
        auto b=as<ID3D11Buffer>(res(key));
        auto range=pod<Pair>(f.at(key+".strideOffset"));
        ctx->IASetVertexBuffers(i,1,b.GetAddressOf(),&range.a,&range.b);
    }
    auto ib=as<ID3D11Buffer>(res("IA.IB"));
    auto index=pod<Pair>(f.at("IA.indexFormatOffset"));
    ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT(index.a),index.b);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(pod<UINT>(f.at("IA.topology"))));
    ctx->VSSetShader(vs.Get(),nullptr,0);
    ctx->PSSetShader(ps.Get(),nullptr,0);
    std::vector<ComPtr<ID3D11ShaderResourceView>> srvs;
    std::vector<ComPtr<ID3D11SamplerState>> samplers;
    for(auto stage: {
        "VS","PS"
    }
    ) {
        bool vertex=std::string(stage)=="VS";
        for(UINT i=0;i<14;++i) {
            auto key=std::string(stage)+".CB"+std::to_string(i);
            if(f.at(key)=="null")continue;
            auto cb=as<ID3D11Buffer>(res(key));
            auto range=pod<Pair>(f.at(key+".range"));
            if(vertex)ctx1->VSSetConstantBuffers1(i,1,cb.GetAddressOf(),&range.a,&range.b);
            else ctx1->PSSetConstantBuffers1(i,1,cb.GetAddressOf(),&range.a,&range.b);
        }
        for(UINT i=0;i<128;++i) {
            auto key=std::string(stage)+".SRV"+std::to_string(i);
            if(!f.count(key+".view"))continue;
            auto desc=pod<D3D11_SHADER_RESOURCE_VIEW_DESC>(f.at(key+".view"));
            ComPtr<ID3D11ShaderResourceView> view;
            check(dev->CreateShaderResourceView(res(key+".resource").gpu.Get(),&desc,&view),"SRV create");
            if(vertex)ctx->VSSetShaderResources(i,1,view.GetAddressOf());
            else ctx->PSSetShaderResources(i,1,view.GetAddressOf());
            srvs.push_back(view);
        }
        for(UINT i=0;i<16;++i) {
            auto key=std::string(stage)+".sampler"+std::to_string(i);
            if(f.at(key)=="null")continue;
            auto desc=pod<D3D11_SAMPLER_DESC>(f.at(key));
            ComPtr<ID3D11SamplerState> s;
            check(dev->CreateSamplerState(&desc,&s),"sampler create");
            if(vertex)ctx->VSSetSamplers(i,1,s.GetAddressOf());
            else ctx->PSSetSamplers(i,1,s.GetAddressOf());
            samplers.push_back(s);
        }
    }
    auto color=as<ID3D11Texture2D>(res("OM.RTV0.resource")),depth=as<ID3D11Texture2D>(res("OM.DSV.resource"));
    D3D11_TEXTURE2D_DESC originalColorDesc{};
    color->GetDesc(&originalColorDesc);
    if(originalColorDesc.Format!=DXGI_FORMAT_R11G11B10_FLOAT)throw std::runtime_error("diagnostic requires HDR R11G11B10_FLOAT target");
    auto rtvDesc=pod<D3D11_RENDER_TARGET_VIEW_DESC>(f.at("OM.RTV0.view"));
    auto dsvDesc=pod<D3D11_DEPTH_STENCIL_VIEW_DESC>(f.at("OM.DSV.view"));
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    check(dev->CreateRenderTargetView(color.Get(),&rtvDesc,&rtv),"RTV create");
    check(dev->CreateDepthStencilView(depth.Get(),&dsvDesc,&dsv),"DSV create");
    auto blendDesc=pod<D3D11_BLEND_DESC>(f.at("OM.blend"));
    if(blendDesc.AlphaToCoverageEnable)throw std::runtime_error("A2C invalidates solid footprint");
    ComPtr<ID3D11BlendState> blend;
    check(dev->CreateBlendState(&blendDesc,&blend),"blend create");
    auto ds=pod<D3D11_DEPTH_STENCIL_DESC>(f.at("OM.depthStencil"));
    ComPtr<ID3D11DepthStencilState> dsState;
    check(dev->CreateDepthStencilState(&ds,&dsState),"depth state");
    auto rs=pod<D3D11_RASTERIZER_DESC>(f.at("RS.state"));
    ComPtr<ID3D11RasterizerState> raster;
    check(dev->CreateRasterizerState(&rs,&raster),"raster state");
    ctx->RSSetState(raster.Get());
    std::vector<D3D11_VIEWPORT> vp;
    for(UINT i=0;i<pod<UINT>(f.at("RS.viewportCount"));++i)vp.push_back(pod<D3D11_VIEWPORT>(f.at("RS.viewport"+std::to_string(i))));
    ctx->RSSetViewports(UINT(vp.size()),vp.data());
    std::vector<D3D11_RECT> sc;
    for(UINT i=0;i<pod<UINT>(f.at("RS.scissorCount"));++i)sc.push_back(pod<D3D11_RECT>(f.at("RS.scissor"+std::to_string(i))));
    ctx->RSSetScissorRects(UINT(sc.size()),sc.data());
    auto factors=pod<std::array<float,4>>(f.at("OM.blendFactors"));
    auto sample=pod<UINT>(f.at("OM.sampleMask"));
    auto ref=pod<UINT>(f.at("OM.stencilRef"));
    ctx->OMSetBlendState(blend.Get(),factors.data(),sample);
    ctx->OMSetDepthStencilState(dsState.Get(),ref);
    ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());
    auto beforeColor=res("OM.RTV0.resource").parts[0].load();
    auto beforeDepth=res("OM.DSV.resource").parts[0].load();
    ctx->DrawIndexedInstanced(x.count,x.instances,x.start,x.base,x.startInstance);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    auto actualColor=readback(dev.Get(),ctx.Get(),color.Get(),res("OM.RTV0.resource").parts[0].row);
    auto actualDepth=readback(dev.Get(),ctx.Get(),depth.Get(),res("OM.DSV.resource").parts[0].row);
    Resource *afterColor=nullptr,*afterDepth=nullptr;
    for(auto& kv:x.resources) {
        if(kv.second.name=="OM.RTV0.after")afterColor=&kv.second;
        if(kv.second.name=="OM.DSV.after")afterDepth=&kv.second;
    }
    if(!afterColor||!afterDepth)throw std::runtime_error("after snapshots absent");
    auto expectedColor=afterColor->parts[0].load(),expectedDepth=afterDepth->parts[0].load();
    D3D11_TEXTURE2D_DESC cd {
    };
    color->GetDesc(&cd);
    if(cd.Format!=DXGI_FORMAT_R11G11B10_FLOAT)throw std::runtime_error("color statistics require R11G11B10_FLOAT");
    D3D11_TEXTURE2D_DESC md=cd;
    md.Format=DXGI_FORMAT_R8_UNORM;
    md.BindFlags=D3D11_BIND_RENDER_TARGET;
    md.MiscFlags=0;
    ComPtr<ID3D11Texture2D> mask;
    check(dev->CreateTexture2D(&md,nullptr,&mask),"mask create");
    ComPtr<ID3D11RenderTargetView> maskView;
    check(dev->CreateRenderTargetView(mask.Get(),nullptr,&maskView),"mask view");
    float zero[4] {
    };
    ctx->ClearRenderTargetView(maskView.Get(),zero);
    ctx->UpdateSubresource(depth.Get(),0,nullptr,beforeDepth.data(),res("OM.DSV.resource").parts[0].row,res("OM.DSV.resource").parts[0].plane);
    const char* source="float4 main():SV_Target0{return 1;}";
    ComPtr<ID3DBlob> code,error;
    check(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&code,&error),"solid PS compile");
    ComPtr<ID3D11PixelShader> solid;
    check(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&solid),"solid PS create");
    ctx->PSSetShader(solid.Get(),nullptr,0);
    D3D11_BLEND_DESC mark {
    };
    mark.RenderTarget[0].RenderTargetWriteMask=15;
    ComPtr<ID3D11BlendState> markState;
    check(dev->CreateBlendState(&mark,&markState),"mark blend");
    ctx->OMSetBlendState(markState.Get(),nullptr,sample);
    ctx->OMSetRenderTargets(1,maskView.GetAddressOf(),dsv.Get());
    ctx->DrawIndexedInstanced(x.count,x.instances,x.start,x.base,x.startInstance);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    auto pixels=readback(dev.Get(),ctx.Get(),mask.Get(),cd.Width);
    auto stats=maskStats(pixels,cd.Width,cd.Height);
    uint64_t changed=0,maskedChanged=0,parityChanged=0,outsideMismatch=0,largeError=0,nonfinite=0;
    double maxAbs=0,maxScaled=0;
    UINT maxCode=0;
    for(size_t i=0;i<pixels.size();++i) {
        bool delta=memcmp(beforeColor.data()+i*4,expectedColor.data()+i*4,4)!=0;
        changed+=delta;
        maskedChanged+=delta&&pixels[i];
        bool mismatch=memcmp(actualColor.data()+i*4,expectedColor.data()+i*4,4)!=0;
        parityChanged+=mismatch;
        outsideMismatch+=mismatch&&!pixels[i];
        UINT a,e;
        memcpy(&a,actualColor.data()+i*4,4);
        memcpy(&e,expectedColor.data()+i*4,4);
        bool large=false;
        for(UINT c=0;c<3;++c) {
            UINT shift=c*11,maskBits=c==2?1023:2047;
            auto av=(a>>shift)&maskBits,ev=(e>>shift)&maskBits;
            maxCode=std::max(maxCode,av>ev?av-ev:ev-av);
            double af=channel(av,c==2?5:6),ef=channel(ev,c==2?5:6);
            if(!std::isfinite(af)||!std::isfinite(ef)) {
                ++nonfinite;
                continue;
            }
            double abs=std::abs(af-ef),scaled=abs/(1+std::abs(ef));
            maxAbs=std::max(maxAbs,abs);
            maxScaled=std::max(maxScaled,scaled);
            large|=scaled>0.01;
        }
        largeError+=large;
    }
    std::ofstream numeric(out/"color_parity.json");
    numeric<<"{\"mismatch_outside_mask\":"<<outsideMismatch<<",\"max_channel_code_distance\":"<<maxCode<<",\"max_absolute_HDR_error\":"<<maxAbs<<",\"max_error_divided_by_one_plus_expected\":"<<maxScaled<<",\"pixels_over_1percent_scaled_error\":"<<largeError<<",\"nonfinite_channels\":"<<nonfinite<<"}\n";
    write(out/"passing_mask.r8",pixels);
    std::ofstream report(out/"warp_result.json");
    report<<"{\n\"width\":"<<cd.Width<<",\"height\":"<<cd.Height<<",\"passing_pixels\":"<<stats.marked<<",\"fraction\":"<<double(stats.marked)/pixels.size()<<",\"bounds_inclusive\":["<<stats.left<<","<<stats.top<<","<<stats.right<<","<<stats.bottom<<"],\"captured_changed_pixels\":"<<changed<<",\"masked_captured_changed_pixels\":"<<maskedChanged<<",\"warp_color_mismatch_pixels\":"<<parityChanged<<",\"warp_depth_stencil_exact\":"<<(actualDepth==expectedDepth?"true":"false")<<",\"color_exact\":"<<(actualColor==expectedColor?"true":"false")<<",\"trace_validated\":false,\"mask_method\":\"solid-original-VS-no-discard-assumption\"\n}\n";
    std::cout<<"mask "<<stats.marked<<"/"<<pixels.size()<<" color mismatches "<<parityChanged<<" DSV exact "<<(actualDepth==expectedDepth)<<"\n";
    return 0;
}
void gpuSelfTest() {
    auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    if(!create)throw std::runtime_error("system D3D11 factory");
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL feature;
    check(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&feature,&ctx),"self-test WARP");
    const char* vsText="float4 main(uint id:SV_VertexID):SV_Position {float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],.5,1);}";
    const char* psText="float4 main():SV_Target0{return 1;}";
    ComPtr<ID3DBlob> vsCode,psCode,error;
    check(D3DCompile(vsText,strlen(vsText),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsCode,&error),"self-test VS compile");
    check(D3DCompile(psText,strlen(psText),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psCode,&error),"self-test PS compile");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    check(dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"self-test VS");
    check(dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"self-test PS");
    D3D11_TEXTURE2D_DESC d {
    };
    d.Width=d.Height=8;
    d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R24G8_TYPELESS;
    d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> gameDepth,privateDepth,mask;
    check(dev->CreateTexture2D(&d,nullptr,&gameDepth),"self-test game depth");
    check(dev->CreateTexture2D(&d,nullptr,&privateDepth),"self-test private depth");
    D3D11_DEPTH_STENCIL_VIEW_DESC vd {
    };
    vd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;
    vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> gameView,privateView;
    check(dev->CreateDepthStencilView(gameDepth.Get(),&vd,&gameView),"self-test game DSV");
    check(dev->CreateDepthStencilView(privateDepth.Get(),&vd,&privateView),"self-test private DSV");
    ctx->ClearDepthStencilView(gameView.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.25f,4);
    auto original=readback(dev.Get(),ctx.Get(),gameDepth.Get(),32);
    d.Format=DXGI_FORMAT_R8_UNORM;
    d.BindFlags=D3D11_BIND_RENDER_TARGET;
    check(dev->CreateTexture2D(&d,nullptr,&mask),"self-test mask");
    ComPtr<ID3D11RenderTargetView> rtv;
    check(dev->CreateRenderTargetView(mask.Get(),nullptr,&rtv),"self-test RTV");
    D3D11_DEPTH_STENCIL_DESC ds {
    };
    ds.DepthEnable=TRUE;
    ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    ds.StencilEnable=TRUE;
    ds.StencilReadMask=255;
    ds.StencilWriteMask=4;
    ds.FrontFace= {
        D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_EQUAL
    };
    ds.BackFace=ds.FrontFace;
    ComPtr<ID3D11DepthStencilState> state;
    check(dev->CreateDepthStencilState(&ds,&state),"self-test DS state");
    D3D11_RASTERIZER_DESC rs {
    };
    rs.FillMode=D3D11_FILL_SOLID;
    rs.CullMode=D3D11_CULL_NONE;
    rs.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    check(dev->CreateRasterizerState(&rs,&raster),"self-test raster");
    ctx->RSSetState(raster.Get());
    D3D11_VIEWPORT vp {
        0,0,8,8,0,1
    };
    ctx->RSSetViewports(1,&vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(),nullptr,0);
    ctx->PSSetShader(ps.Get(),nullptr,0);
    for(UINT scenario=0;scenario<3;++scenario) {
        ctx->CopyResource(privateDepth.Get(),gameDepth.Get());
        if(scenario==1)ctx->ClearDepthStencilView(privateView.Get(),D3D11_CLEAR_DEPTH,.75f,4);
        float clear[4] {
        };
        ctx->ClearRenderTargetView(rtv.Get(),clear);
        ctx->OMSetDepthStencilState(state.Get(),scenario==2?0:4);
        ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),privateView.Get());
        ctx->Draw(3,0);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        auto result=readback(dev.Get(),ctx.Get(),mask.Get(),8);
        auto measured=maskStats(result,8,8);
        if(measured.marked!=(scenario==0?64:0))throw std::runtime_error("WARP depth/stencil mask mismatch");
        if(readback(dev.Get(),ctx.Get(),gameDepth.Get(),32)!=original)throw std::runtime_error("WARP replay mutated game DSV");
    }
}
int main(int argc,char** argv) {
    try {
        if(argc==2&&std::string(argv[1])=="--self-test") {
            gpuSelfTest();
            Bytes b(12);
            b[1]=b[10]=255;
            auto s=maskStats(b,4,3);
            if(s.marked!=2||s.left!=1||s.top!=0||s.right!=2||s.bottom!=2)throw std::runtime_error("mask stats");
            try {
                pod<UINT>("00");
                throw std::runtime_error("accepted short POD");
            }
            catch(const std::runtime_error& e) {
                if(std::string(e.what())=="accepted short POD")throw;
            }
            std::cout<<"flat packet replay C++ self-test: PASS\n";
            return 0;
        }
        if(argc!=3 && !(argc==4 && std::string(argv[3])=="--dry-run"))throw std::runtime_error("usage: runner recipe output-directory [--dry-run]");
        auto recipe=load(std::filesystem::u8path(argv[1]));
        if(argc==4) {
            std::cout<<"replay recipe readable; dry-run writes nothing\n";
            return 0;
        }
        return run(recipe,std::filesystem::u8path(argv[2]));
    }
    catch(const std::exception& e) {
        std::cerr<<"replay: "<<e.what()<<"\n";
        return 1;
    }
}
