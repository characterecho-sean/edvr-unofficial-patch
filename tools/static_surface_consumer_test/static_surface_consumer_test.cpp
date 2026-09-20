#include "../../src/d3d11/temporal_shader_source.h"
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <utility>
#include <fstream>

using Microsoft::WRL::ComPtr;
static unsigned checks = 0;
static void check(bool ok, const char* why) { ++checks; if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); } }
static void hr(HRESULT h) { check(SUCCEEDED(h), "D3D operation"); }
static ComPtr<ID3DBlob> compile(const std::string& s, const char* entry) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT h = D3DCompile(s.data(), s.size(), "static_surface_consumer", nullptr, nullptr,
                           entry, "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(h) && errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    hr(h); return code;
}
static std::vector<float> read(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, unsigned channels) {
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; hr(dev->CreateTexture2D(&d,nullptr,&staging)); ctx->CopyResource(staging.Get(),src);
    D3D11_MAPPED_SUBRESOURCE m{}; hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));
    std::vector<float> out(d.Width*d.Height*channels); for(UINT y=0;y<d.Height;++y)
        std::memcpy(out.data()+y*d.Width*channels, static_cast<const char*>(m.pData)+y*m.RowPitch, d.Width*channels*sizeof(float));
    ctx->Unmap(staging.Get(),0); return out;
}

int main() {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL level{};
    HRESULT h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,
                                D3D11_SDK_VERSION,&dev,&level,&ctx);
    if(h==DXGI_ERROR_SDK_COMPONENT_MISSING) h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
                                                                  D3D11_SDK_VERSION,&dev,&level,&ctx);
    hr(h); ComPtr<ID3D11InfoQueue> info; dev.As(&info);
    std::string hlsl = "#define EDVR_TEMPORAL_TRACE 1\n";
    hlsl += edvr::kTemporalCsHlsl;
    auto code=compile(hlsl,"mv"); ComPtr<ID3D11ComputeShader> shader;
    hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    ComPtr<ID3D11ShaderReflection> reflect; hr(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&reflect));
    auto* cbDesc=reflect->GetConstantBufferByName("P"); D3D11_SHADER_BUFFER_DESC pd{}; hr(cbDesc->GetDesc(&pd));
    std::vector<unsigned char> cbData(pd.Size,0);
    auto set=[&](const char* n,const void* p,unsigned bytes){D3D11_SHADER_VARIABLE_DESC v{};hr(cbDesc->GetVariableByName(n)->GetDesc(&v));check(v.StartOffset+bytes<=cbData.size(),"reflected field extent");std::memcpy(cbData.data()+v.StartOffset,p,bytes);};
    auto f4=[&](const char* n,float a,float b,float c,float d){float v[4]={a,b,c,d};set(n,v,16);};
    int region[4]={0,0,8,8}, size[2]={8,8}; set("region",region,16); set("size",size,8); set("texSize",size,8);
    f4("tanNow",-1,1,-1,1); f4("tanPrev",-1,1,-1,1);
    f4("dR0",1,0,0,0); f4("dR1",0,1,0,0); f4("dR2",0,0,1,0);
    f4("c2R0",1,0,0,0); f4("c2R1",0,1,0,0); f4("c2R2",0,0,1,0);
    f4("stR0",1,0,0,0); f4("stR1",0,1,0,0); f4("stR2",0,0,1,0); f4("tvSt",1,0,0,1);
    f4("tvCam",0,0,0,1); f4("split",10,0,0,0); f4("knobs",0,1,.025f,0);
    f4("ships",0,0,0,0); f4("objects",1500,0,10000,0); f4("shBox0",-100,-100,-100,1); f4("shBox1",100,100,100,0); f4("shDir",0,0,0,0); f4("shParts",0,0,0,0);
    float shipRows[12]={1,0,0,0,0,1,0,0,0,0,1,0}; set("shR",shipRows,sizeof(shipRows)); float shipTv[4]={1,0,0,0}; set("shTv",shipTv,sizeof(shipTv));
    f4("holoJitter",0,0,1,1); f4("probe",1,0,0,256); f4("wR0",1,0,0,0); f4("wR1",0,1,0,0); f4("wR2",0,0,1,0);
    // The centre ray is 100 m deep. Keep it strictly inside the ownership
    // volume (insideBody excludes its high face), so the unmodified shader
    // first takes the dominant body's path 4.
    f4("box0",-1000,-1000,-1000,0); f4("box1",1000,1000,1000,0); int have=1; set("haveHistory",&have,4);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=pd.Size; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; ComPtr<ID3D11Buffer> cb; hr(dev->CreateBuffer(&bd,nullptr,&cb));
    auto tex=[&](DXGI_FORMAT fmt,UINT bind,UINT w=8,UINT h=8){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=fmt;d.BindFlags=bind;ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&d,nullptr,&t));return t;};
    auto colour=tex(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto scene=tex(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto ownerNow=tex(DXGI_FORMAT_R32G32B32A32_UINT,D3D11_BIND_SHADER_RESOURCE), ownerPrev=tex(DXGI_FORMAT_R32G32B32A32_UINT,D3D11_BIND_SHADER_RESOURCE);
    auto motion=tex(DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_UNORDERED_ACCESS), depth=tex(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_UNORDERED_ACCESS), mask=tex(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    auto output=tex(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS), decision=tex(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    auto stats=tex(DXGI_FORMAT_R32_UINT,D3D11_BIND_UNORDERED_ACCESS,64,1);
    D3D11_TEXTURE3D_DESC gd{};gd.Width=gd.Height=gd.Depth=128;gd.MipLevels=1;gd.Format=DXGI_FORMAT_R32_FLOAT;gd.BindFlags=D3D11_BIND_SHADER_RESOURCE;std::vector<float> gridData(128*128*128,1.0f);D3D11_SUBRESOURCE_DATA gi{gridData.data(),128*4,128*128*4};ComPtr<ID3D11Texture3D> grid;hr(dev->CreateTexture3D(&gd,&gi,&grid));
    ComPtr<ID3D11ShaderResourceView> srvHold[19]; ID3D11ShaderResourceView* srvs[19] = {};
    hr(dev->CreateShaderResourceView(colour.Get(),nullptr,&srvHold[0]));hr(dev->CreateShaderResourceView(scene.Get(),nullptr,&srvHold[2]));hr(dev->CreateShaderResourceView(grid.Get(),nullptr,&srvHold[5]));hr(dev->CreateShaderResourceView(ownerNow.Get(),nullptr,&srvHold[17]));hr(dev->CreateShaderResourceView(ownerPrev.Get(),nullptr,&srvHold[18]));for(int i=0;i<19;++i)srvs[i]=srvHold[i].Get();
    ComPtr<ID3D11UnorderedAccessView> uav[8]; auto makeUav=[&](ID3D11Texture2D* t,ComPtr<ID3D11UnorderedAccessView>& v){hr(dev->CreateUnorderedAccessView(t,nullptr,&v));}; makeUav(output.Get(),uav[0]); makeUav(motion.Get(),uav[3]);makeUav(depth.Get(),uav[4]);makeUav(mask.Get(),uav[5]);makeUav(decision.Get(),uav[7]);makeUav(stats.Get(),uav[2]); ID3D11UnorderedAccessView* uavs[8] = {}; for(int i=0;i<8;++i)uavs[i]=uav[i].Get();
    std::vector<float> colourData(8*8*4,0), sceneData(64,.00025f); ctx->UpdateSubresource(colour.Get(),0,nullptr,colourData.data(),8*16,0); ctx->UpdateSubresource(scene.Get(),0,nullptr,sceneData.data(),8*4,0);
    std::vector<unsigned> owners(64*4,0), previous(64*4,0); const unsigned key[3]={0x1234u,0x5678u,0x9abcu}; const size_t centre=(4*8+4)*4; std::memcpy(owners.data()+centre,key,12); float raw=.00025f; std::memcpy(&owners[centre+3],&raw,4); std::memcpy(previous.data()+centre,key,12); std::memcpy(&previous[centre+3],&raw,4); ctx->UpdateSubresource(ownerNow.Get(),0,nullptr,owners.data(),8*16,0);ctx->UpdateSubresource(ownerPrev.Get(),0,nullptr,previous.data(),8*16,0);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;ComPtr<ID3D11SamplerState> samp;hr(dev->CreateSamplerState(&sd,&samp));
    auto run=[&](bool bindFlag,bool history,bool keys,bool currentDepthMatch,int previousX,float jitterX){std::fill(previous.begin(),previous.end(),0);size_t prevIndex=(4*8+size_t(previousX))*4;if(keys){std::memcpy(previous.data()+prevIndex,key,12);std::memcpy(&previous[prevIndex+3],&raw,4);}ctx->UpdateSubresource(ownerPrev.Get(),0,nullptr,previous.data(),8*16,0);float currentRaw=currentDepthMatch?raw:.0002f;std::memcpy(owners.data()+centre,key,12);std::memcpy(&owners[centre+3],&currentRaw,4);ctx->UpdateSubresource(ownerNow.Get(),0,nullptr,owners.data(),8*16,0);f4("probe",1,0,0,bindFlag?256.f:0);int hv=history?1:0;set("haveHistory",&hv,4);f4("holoJitter",jitterX,0,1,1);ctx->UpdateSubresource(cb.Get(),0,nullptr,cbData.data(),0,0);ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());ctx->CSSetShaderResources(0,19,srvs);ctx->CSSetUnorderedAccessViews(0,8,uavs,nullptr);ctx->CSSetSamplers(0,1,samp.GetAddressOf());ctx->CSSetShader(shader.Get(),nullptr,0);ctx->Dispatch(1,1,1);auto m=read(dev.Get(),ctx.Get(),motion.Get(),2),d=read(dev.Get(),ctx.Get(),decision.Get(),4);return std::pair<float,unsigned>{m[2*centre/4],unsigned(d[4*(centre/4)+3]+.5f)};};
    auto body=[&](const std::pair<float,unsigned>& result,const char* why){check((result.second&15u)==4u && (result.second&2048u)==0u,why);check(std::fabs(result.first)>.01f,"unpromoted body path retains its nonzero motion");};
    const auto base=run(false,true,true,true,3,1); body(base,"unmodified production shader enters body path 4");
    const auto good=run(true,true,true,true,3,1); std::printf("body motion=%g flags=%u; promoted motion=%g flags=%u\n",base.first,base.second,good.first,good.second); check(std::fabs(good.first)<.01f,"static owner restores zero world motion"); check((good.second&15u)==2u && (good.second&2048u)!=0,"static owner promotes body path 4 to confirmed path 2");
    body(run(true,true,false,true,3,1),"mismatched key retains body path 4");
    body(run(true,true,true,false,3,1),"depth mismatch retains body path 4");
    body(run(false,true,true,true,3,1),"feature flag off retains body path 4");
    body(run(true,false,true,true,3,1),"missing history retains body path 4");
    body(run(true,true,true,true,3,0),"wrong jitter sample retains body path 4");
    ctx->Flush();
    if (info) {
        const UINT64 count = info->GetNumStoredMessages();
        bool errors = false;
        for (UINT64 i=0; i<count; ++i) {
            SIZE_T bytes = 0; info->GetMessage(i,nullptr,&bytes); std::vector<unsigned char> msg(bytes);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(msg.data()); info->GetMessage(i,m,&bytes);
            errors |= m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION || m->Severity == D3D11_MESSAGE_SEVERITY_ERROR;
        }
        check(!errors,"WARP temporal dispatch emits no D3D11 debug errors");
    }
    std::printf("PASS: %u static-surface consumer checks\n",checks); return 0;
}
