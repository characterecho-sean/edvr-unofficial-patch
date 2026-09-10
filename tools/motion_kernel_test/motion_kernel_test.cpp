#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
using Microsoft::WRL::ComPtr;
void require(bool ok,const char* why){if(!ok){std::printf("FAIL %s\n",why);std::exit(1);}}
void hr(HRESULT h){if(FAILED(h)){std::printf("HRESULT %08x\n",unsigned(h));std::exit(1);}}
std::vector<char> load(const char* p){std::ifstream f(p,std::ios::binary);require(bool(f),p);return {std::istreambuf_iterator<char>(f),{}};}
struct Tex{ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11ShaderResourceView> s;ComPtr<ID3D11UnorderedAccessView> u;UINT bpp;};
ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
Tex tex(UINT w,UINT h,DXGI_FORMAT fmt,UINT bpp,const char* path=nullptr,DXGI_FORMAT view=DXGI_FORMAT_UNKNOWN){
    Tex x;x.bpp=bpp;D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=fmt;
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    std::vector<char> bytes;if(path){bytes=load(path);require(bytes.size()==size_t(w)*h*bpp,"texture file length");}
    D3D11_SUBRESOURCE_DATA data{bytes.data(),w*bpp,0};hr(dev->CreateTexture2D(&d,path?&data:nullptr,&x.t));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=view==DXGI_FORMAT_UNKNOWN?fmt:view;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    hr(dev->CreateShaderResourceView(x.t.Get(),&sd,&x.s));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=sd.Format;ud.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
    hr(dev->CreateUnorderedAccessView(x.t.Get(),&ud,&x.u));return x;
}
std::vector<char> read(const Tex& t){
    D3D11_TEXTURE2D_DESC d{};t.t->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;hr(dev->CreateTexture2D(&d,nullptr,&stage));ctx->CopyResource(stage.Get(),t.t.Get());
    D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));std::vector<char> b(size_t(d.Width)*d.Height*t.bpp);
    for(UINT y=0;y<d.Height;++y)std::memcpy(b.data()+size_t(y)*d.Width*t.bpp,static_cast<char*>(m.pData)+y*m.RowPitch,d.Width*t.bpp);
    ctx->Unmap(stage.Get(),0);return b;
}

int main(){
    D3D_FEATURE_LEVEL level;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx));
    const char* names[]={"reference","diagnostic","normal"};
    ComPtr<ID3D11ComputeShader> shaders[3];
    for(int i=0;i<3;++i){
        auto src=load((std::string(names[i])+".hlsl").c_str());ComPtr<ID3DBlob> code,errors;
        HRESULT result=D3DCompile(src.data(),src.size(),names[i],nullptr,nullptr,"mv","cs_5_0",0,0,&code,&errors);
        if(errors)std::puts(static_cast<const char*>(errors->GetBufferPointer()));hr(result);
        hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i]));
    }
    for(int fixture=0;fixture<20;++fixture){
        char folder[16];sprintf_s(folder,"%02d",fixture);require(SetCurrentDirectoryA(folder),"fixture directory");
        auto dim=load("size.bin");require(dim.size()==24,"dimensions");UINT v[6];memcpy(v,dim.data(),24);
        const UINT w=v[0],h=v[1],ow=v[2],oh=v[3],tw=v[4],th=v[5];
        auto colour=tex(tw,th,DXGI_FORMAT_R8G8B8A8_UNORM,4,"colour.bin");
        auto history=tex(ow,oh,DXGI_FORMAT_R8G8B8A8_UNORM,4,"history.bin");
        auto depth=tex(tw,th,DXGI_FORMAT_R32_FLOAT,4,"depth.bin");
        auto smoke=tex(tw,th,DXGI_FORMAT_R32_FLOAT,4,"smoke.bin");
        auto uiDepth=tex(tw,th,DXGI_FORMAT_R32_FLOAT,4,"ui_depth.bin");
        auto prevDepth=tex(w,h,DXGI_FORMAT_R32_FLOAT,4,"previous_depth.bin");
        auto mask=tex(w,h,DXGI_FORMAT_R8_UNORM,1,"mask.bin");
        auto previous=tex(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4,"ui_previous.bin");
        Tex outputs[]={tex(w,h,DXGI_FORMAT_R16G16_FLOAT,4),tex(w,h,DXGI_FORMAT_R32_FLOAT,4),
                       tex(w,h,DXGI_FORMAT_R8_UNORM,1),tex(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4)};
        D3D11_TEXTURE3D_DESC gd{};gd.Width=gd.Height=gd.Depth=gd.MipLevels=1;gd.Format=DXGI_FORMAT_R8_UNORM;
        gd.BindFlags=D3D11_BIND_SHADER_RESOURCE;unsigned char occupied=fixture%2?128:255;D3D11_SUBRESOURCE_DATA init{&occupied,1,1};
        ComPtr<ID3D11Texture3D> grid;ComPtr<ID3D11ShaderResourceView> gridView;
        hr(dev->CreateTexture3D(&gd,&init,&grid));hr(dev->CreateShaderResourceView(grid.Get(),nullptr,&gridView));
        auto parameters=load("params.bin");D3D11_BUFFER_DESC bd{};bd.ByteWidth=UINT(parameters.size());
        bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA ci{parameters.data(),0,0};
        ComPtr<ID3D11Buffer> cb;hr(dev->CreateBuffer(&bd,&ci,&cb));
        bd={};bd.ByteWidth=64*4;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;
        ComPtr<ID3D11Buffer> stats;ComPtr<ID3D11UnorderedAccessView> statsView;
        hr(dev->CreateBuffer(&bd,nullptr,&stats));hr(dev->CreateUnorderedAccessView(stats.Get(),nullptr,&statsView));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;hr(dev->CreateSamplerState(&sd,&sampler));
        std::vector<char> reference[4];
        for(int variant=0;variant<3;++variant){
            ID3D11ShaderResourceView* srvs[]={colour.s.Get(),history.s.Get(),depth.s.Get(),prevDepth.s.Get(),mask.s.Get(),
                                            gridView.Get(),smoke.s.Get(),uiDepth.s.Get(),previous.s.Get()};
            ID3D11UnorderedAccessView* uavs[]={nullptr,nullptr,variant==2?nullptr:statsView.Get(),outputs[0].u.Get(),
                                            outputs[1].u.Get(),outputs[2].u.Get(),outputs[3].u.Get()};
            ctx->CSSetShader(shaders[variant].Get(),nullptr,0);ctx->CSSetShaderResources(0,9,srvs);
            ctx->CSSetUnorderedAccessViews(0,7,uavs,nullptr);ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
            ctx->CSSetSamplers(0,1,sampler.GetAddressOf());ctx->Dispatch((w+7)/8,(h+7)/8,1);ctx->ClearState();
            for(int i=0;i<4;++i){auto bytes=read(outputs[i]);if(variant==0)reference[i]=std::move(bytes);
                else { if(bytes!=reference[i])std::printf("fixture %d variant %s output %d\n",fixture,names[variant],i);
                       require(bytes==reference[i],"image output differs from scalar reference");}}
        }
        std::printf("PASS: fixture %02d, %ux%u; all four outputs identical, both diagnostic modes.\n",fixture,w,h);
        require(SetCurrentDirectoryA(".."),"return to root");
    }
    std::puts("PASS: 20 GPU fixtures, three shader variants.");
}
