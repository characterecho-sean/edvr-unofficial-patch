// Exercise the production coverage pass on D3D11 WARP. Only the game's
// depth-probe selection, logging and raw-hook entry are supplied here.
#include "../../src/d3d11/ui_depth.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;
ComPtr<ID3DBlob> compile(const char*,const char*);
namespace edvr {
ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext*,const char*,size_t,const char*,const char*,const SwapMacro*,const char*) { std::abort(); }
ID3D11Texture2D* testScene = nullptr;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
int guardFilter(unsigned long, const char*) { return EXCEPTION_EXECUTE_HANDLER; }
void FaultBudget::charge() { --m_remaining; }
// Classification/configuration are outside these render-pass tests. Abort
// if they become dependencies, rather than silently supplying fake state.
bool Config::getBool(const char*, bool) const { std::abort(); }
float Config::getFloat(const char*, float) const { std::abort(); }
std::string Config::getString(const char*, const char*) const { std::abort(); }
void* bindingGet(BindSlot) { std::abort(); }
uint32_t bindingGeneration(BindSlot) { std::abort(); }
uint64_t bindingShaderHash(BindSlot) { std::abort(); }
bool bindingResolve(void*, ResourceInfo*) { std::abort(); }
bool depthProbeIsSceneDepth(const void*) { std::abort(); }
uint64_t lookupShaderHash(void*) { std::abort(); }
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx, const char* source, size_t,
    const char*, const char*, const SwapMacro*, const char*) {
    auto code=::compile(source,"cs_5_0");ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11ComputeShader* shader=nullptr;
    if(FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))std::abort();return shader;
}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext*, const char*, size_t,
    const char*, const char*, const SwapMacro*, const char*) { std::abort(); }
float temporalPassDepthAt(float metres) { return 0.025f / metres; }
bool temporalPassPlanes(float* nearZ, float* farZ) { *nearZ = .025f; *farZ = 10000; return true; }
bool depthProbeSceneDepthFormat(uint32_t, uint32_t, int, ID3D11Texture2D** tex, uint32_t* fmt) {
    *tex = testScene; *fmt = DXGI_FORMAT_D32_FLOAT; return testScene != nullptr;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* ctx, UINT n,
                               ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* ds) {
    ctx->OMSetRenderTargets(n, rt, ds);
}
}

using namespace edvr;
int checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", label); std::exit(1); }
}
void hr(HRESULT result) { check(SUCCEEDED(result), "D3D operation"); }
ComPtr<ID3DBlob> compile(const char* hlsl, const char* profile) {
    ComPtr<ID3DBlob> blob, errors;
    HRESULT result = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr,
                                "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(result) && errors) std::puts(static_cast<const char*>(errors->GetBufferPointer()));
    hr(result); return blob;
}
struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11DepthStencilView> dsv;
};
Target depth(ID3D11Device* dev, DXGI_FORMAT format, DXGI_FORMAT view, UINT w = 8, UINT samples = 1) {
    Target t;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = 8; td.MipLevels = td.ArraySize = 1;
    td.Format = format; td.SampleDesc.Count = samples;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    hr(dev->CreateTexture2D(&td, nullptr, &t.tex));
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = view;
    dd.ViewDimension = samples == 1 ? D3D11_DSV_DIMENSION_TEXTURE2D : D3D11_DSV_DIMENSION_TEXTURE2DMS;
    hr(dev->CreateDepthStencilView(t.tex.Get(), &dd, &t.dsv));
    return t;
}
std::vector<float> read(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT channel=0) {
    ComPtr<ID3D11Texture2D> tex; hr(res->QueryInterface(IID_PPV_ARGS(&tex)));
    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; hr(dev->CreateTexture2D(&td, nullptr, &stage));
    ctx->CopyResource(stage.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map));
    UINT stride = td.Format == DXGI_FORMAT_R8_UNORM ? 1 : (td.Format == DXGI_FORMAT_R32G8X24_TYPELESS || td.Format==DXGI_FORMAT_R32G32_FLOAT) ? 8 : td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16 : 4;
    std::vector<float> values(td.Width * td.Height);
    for (UINT y = 0; y < td.Height; ++y) for (UINT x = 0; x < td.Width; ++x) {
        const auto* p = static_cast<const unsigned char*>(map.pData) + y * map.RowPitch + x * stride;
        if (td.Format == DXGI_FORMAT_R8_UNORM) values[y * td.Width + x] = *p / 255.0f;
        else if (td.Format == DXGI_FORMAT_R24G8_TYPELESS)
            values[y * td.Width + x] = static_cast<float>(*reinterpret_cast<const UINT*>(p) & 0xffffff) / 16777215.0f;
        else values[y * td.Width + x] = reinterpret_cast<const float*>(p)[channel];
    }
    ctx->Unmap(stage.Get(), 0); return values;
}
int main(int argc, char** argv) {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    const auto driver = argc>2 && std::strcmp(argv[2],"hardware")==0 ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
    HRESULT created = D3D11CreateDevice(nullptr, driver, nullptr,
        D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
    if (created == DXGI_ERROR_SDK_COMPONENT_MISSING)
        created = D3D11CreateDevice(nullptr, driver, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
    hr(created);
    ComPtr<ID3D11InfoQueue> info; dev.As(&info);
    // Compile every actual coverage shader, not a test transcription.
    for (DepthShader& entry : g_depthShaders) {
        auto code = compile(entry.hlsl, "ps_5_0");
        hr(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &entry.shader));
    }
    auto vsCode = compile("cbuffer C:register(b0){float4 v;} struct O{float2 uv:TEXCOORD0;float4 p:SV_Position;}; O main(uint id:SV_VertexID){O o;float2 p=float2((id<<1)&2,id&2);o.p=float4(p*float2(2,-2)+float2(-1,1),v.x,1);o.uv=p;return o;}", "vs_5_0");
    auto psCode = compile("float main():SV_Target{return 1;}", "ps_5_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    hr(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
    hr(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> position, sentinel;
    hr(dev->CreateBuffer(&bd, nullptr, &position)); hr(dev->CreateBuffer(&bd, nullptr, &sentinel));
    D3D11_TEXTURE2D_DESC td{}; td.Width = 2; td.Height = td.ArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    float surface[] = {1,1,1,1, 1,1,1,0}; D3D11_SUBRESOURCE_DATA data{surface, sizeof(surface), 0};
    ComPtr<ID3D11Texture2D> surf; ComPtr<ID3D11ShaderResourceView> surfSrv;
    hr(dev->CreateTexture2D(&td, &data, &surf)); hr(dev->CreateShaderResourceView(surf.Get(), nullptr, &surfSrv));
    td.Width = td.Height = 8; td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color; ComPtr<ID3D11RenderTargetView> rtv;
    hr(dev->CreateTexture2D(&td, nullptr, &color)); hr(dev->CreateRenderTargetView(color.Get(), nullptr, &rtv));
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler; hr(dev->CreateSamplerState(&sd, &sampler));
    D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> nonwriting; hr(dev->CreateDepthStencilState(&ds, &nonwriting));
    D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster; hr(dev->CreateRasterizerState(&rs, &raster));
    auto setZ = [&](float z) { float v[4] = {z}; ctx->UpdateSubresource(position.Get(), 0, nullptr, v, 0, 0); };
    auto bind = [&](ID3D11DepthStencilView* dsv) {
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv);
        ctx->OMSetDepthStencilState(nonwriting.Get(), 7);
        ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, position.GetAddressOf());
        ctx->PSSetConstantBuffers(13, 1, sentinel.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, surfSrv.GetAddressOf());
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->RSSetState(raster.Get()); D3D11_VIEWPORT vp{0,0,8,8,0,1}; ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    auto coverage = [&](bool menu, bool mark) {
        g_on = true; g_mode = menu ? Mode::kReissue : Mode::kReissueScene;
        g_reissueShader = &g_depthShaders[2]; g_drawEye = 0; g_reissueMaskSlot = 1;
        g_rebindW = g_rebindH = 8; g_wantRebind = menu; g_rebindEye = 0;
        g_wantMask = mark; g_reactive = .5f;
        check(uiDepthReissueBegin(ctx.Get()), "production coverage begins");
        ctx->Draw(3, 0); uiDepthReissueEnd(ctx.Get());
        ComPtr<ID3D11Buffer> cb; ctx->PSGetConstantBuffers(13, 1, &cb);
        check(cb.Get() == sentinel.Get(), "PS b13 restored");
        ComPtr<ID3D11PixelShader> p; ctx->PSGetShader(&p, nullptr, nullptr);
        check(p.Get() == ps.Get(), "pixel shader restored");
        ComPtr<ID3D11DepthStencilState> d; UINT ref = 0; ctx->OMGetDepthStencilState(&d, &ref);
        check(d.Get() == nonwriting.Get() && ref == 7, "depth state and stencil reference restored");
    };
    auto scene = depth(dev.Get(), DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT);
    testScene = scene.tex.Get();
    const float zero[4] = {};
    ctx->ClearDepthStencilView(scene.dsv.Get(), D3D11_CLEAR_DEPTH, .2f, 0);
    bind(scene.dsv.Get()); setZ(.6f); coverage(false, true);
    auto original = read(dev.Get(), ctx.Get(), scene.tex.Get());
    for (float z : original) check(std::fabs(z - .2f) < 1e-5f, "coverage leaves game depth unchanged");
    ID3D11ShaderResourceView* ui = nullptr;
    check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui), "AA receives private UI depth");
    ComPtr<ID3D11Resource> privateRes; ui->GetResource(&privateRes);
    auto values = read(dev.Get(), ctx.Get(), privateRes.Get());
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        check(std::fabs(values[y*8+x] - (x < 4 ? .6f : .2f)) < 1e-5f, "private depth follows alpha coverage");
    ctx->ClearRenderTargetView(rtv.Get(), zero); setZ(.3f); ctx->Draw(3,0);
    for (float v : read(dev.Get(), ctx.Get(), color.Get())) check(v == 1, "later smoke has no rectangular hole");
    // Positive control: reproduce the defect with coverage on the live DSV.
    ctx->OMSetDepthStencilState(reissueState(ctx.Get()), 0); ctx->PSSetShader(g_depthShaders[2].shader,nullptr,0);
    ID3D11Buffer* floor = floorBuffer(ctx.Get(),1,0); ctx->PSSetConstantBuffers(13,1,&floor);
    setZ(.6f); ctx->Draw(3,0); bind(scene.dsv.Get()); setZ(.3f);
    ctx->ClearRenderTargetView(rtv.Get(), zero); ctx->Draw(3,0);
    values = read(dev.Get(), ctx.Get(), color.Get());
    check(values[1] == 0 && values[6] == 1, "live depth control reproduces rectangular smoke hole");
    uiDepthFrameBoundary(ctx.Get());
    check(!uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui), "no stale UI in a frame without coverage");
    ctx->ClearDepthStencilView(scene.dsv.Get(), D3D11_CLEAR_DEPTH, .8f,0);
    bind(scene.dsv.Get()); setZ(.6f); coverage(false,true);
    check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui), "new frame seeded"); ui->GetResource(privateRes.ReleaseAndGetAddressOf());
    for (float z : read(dev.Get(),ctx.Get(),privateRes.Get())) check(std::fabs(z-.8f)<1e-5f,"scene geometry occludes UI");
    check(!uiDepthTemporalDepth(8,8,1,scene.tex.Get(),&ui),"eye isolation");
    g_on = false; check(!uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"disabled feature does not publish"); g_on = true;
    // A menu uses another DSV and converts its projection into scene depth.
    auto menu = depth(dev.Get(), DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT);
    uiDepthFrameBoundary(ctx.Get()); ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,.1f,0);
    ctx->ClearDepthStencilView(menu.dsv.Get(),D3D11_CLEAR_DEPTH,.05f,0);
    bind(menu.dsv.Get()); setZ(.96f); coverage(true,false);
    for(float z:read(dev.Get(),ctx.Get(),menu.tex.Get())) check(std::fabs(z-.05f)<1e-5f,"menu DSV unchanged");
    for(float z:read(dev.Get(),ctx.Get(),scene.tex.Get())) check(std::fabs(z-.1f)<1e-5f,"rebound scene DSV unchanged");
    check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"menu publishes private depth"); ui->GetResource(privateRes.ReleaseAndGetAddressOf());
    values=read(dev.Get(),ctx.Get(),privateRes.Get()); check(std::fabs(values[1]-.24f)<1e-5f,"menu depth encoding preserved");
    D3D11_VIEWPORT vp{}; UINT count=1; ctx->RSGetViewports(&count,&vp); check(vp.MaxDepth==1,"viewport restored");
    ComPtr<ID3D11DepthStencilView> bound; ctx->OMGetRenderTargets(0,nullptr,&bound); check(bound.Get()==menu.dsv.Get(),"menu target restored");
    // Dim comms icons must keep panel depth instead of falling through to
    // sky motion. Their screen material has no glow; transparent pixels
    // must still leave the private and original depths alone.
    for (float alpha : {0.18f, 0.02f, 1.0f/255.0f, 0.001f, 0.0f}) {
        surface[3]=alpha;
        ctx->UpdateSubresource(surf.Get(),0,nullptr,surface,sizeof(surface),0);
        uiDepthFrameBoundary(ctx.Get());
        ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        bind(menu.dsv.Get()); setZ(.96f); coverage(true,true);
        check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"comms depth published");
        ui->GetResource(privateRes.ReleaseAndGetAddressOf());
        const auto d=read(dev.Get(),ctx.Get(),privateRes.Get());
        const auto m=read(dev.Get(),ctx.Get(),g_mask[0].tex);
        const bool visible=alpha>=1.0f/255.0f;
        check(std::fabs(d[1]-(visible?.24f:0.0f))<1e-5f,"dim comms stroke has physical panel depth");
        check((m[1]>0)==visible && m[6]==0,"comms mask covers dim icons but not transparency");
        check(d[6]==0,"transparent comms pixel preserves sky depth");
        check(read(dev.Get(),ctx.Get(),scene.tex.Get())[1]==0,"comms coverage leaves original depth untouched");
    }
    // The rank panel's dim glyphs peak at 124/255 alpha. Exercise the
    // actual holo shader at cockpit and target distances, including fully
    // transparent texels and sub-quantum source/glow values.
    auto holoVsCode=compile("cbuffer C:register(b0){float4 v;} struct O{float4 tc0:TEXCOORD0;float3 tc4:TEXCOORD4;float3 view:TEXCOORD6;float3 tc7:TEXCOORD7;float2 uv:TEXCOORD8;float4 p:SV_Position;}; O main(uint id:SV_VertexID){O o=(O)0;float2 p=float2((id<<1)&2,id&2);o.p=float4(p*float2(2,-2)+float2(-1,1),.025/v.x,1);o.uv=p;o.view=float3(0,0,-v.x);return o;}","vs_5_0");
    ComPtr<ID3D11VertexShader> holoVs;
    hr(dev->CreateVertexShader(holoVsCode->GetBufferPointer(),holoVsCode->GetBufferSize(),nullptr,&holoVs));
    for (float distance : {.6f, 16000.0f}) for (float alpha : {124.0f/255,33.0f/255,1.0f/255,.001f,0.0f,.8f}) {
        surface[3]=alpha; ctx->UpdateSubresource(surf.Get(),0,nullptr,surface,sizeof(surface),0);
        uiDepthFrameBoundary(ctx.Get()); ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        bind(scene.dsv.Get()); setZ(distance); ctx->VSSetShader(holoVs.Get(),nullptr,0);
        ctx->PSSetShaderResources(2,1,surfSrv.GetAddressOf()); ctx->PSSetSamplers(1,1,sampler.GetAddressOf());
        g_on=true; g_mode=Mode::kReissueScene; g_reissueShader=&g_depthShaders[3];
        g_drawEye=0; g_reissueMaskSlot=1; g_rebindW=g_rebindH=8; g_wantMask=true;
        check(uiDepthReissueBegin(ctx.Get()),"holo coverage begins"); ctx->Draw(3,0); uiDepthReissueEnd(ctx.Get());
        check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"holo private depth published");
        ui->GetResource(privateRes.ReleaseAndGetAddressOf());
        const auto d=read(dev.Get(),ctx.Get(),privateRes.Get());
        const bool visible=alpha >= (distance<10 ? 1.0f/255 : .5f);
        check(std::fabs(d[1]-(visible?.025f/distance:0))<1e-8f,"dim cockpit text retains depth; distant marker fringe excluded");
        check(d[6]==0,"transparent holo texel never stamps depth");
        check((read(dev.Get(),ctx.Get(),g_mask[0].tex)[1]>0)==visible,"holo mask follows physical coverage");
        check(read(dev.Get(),ctx.Get(),scene.tex.Get())[1]==0,"holo coverage leaves game depth untouched");
    }
    surface[3]=1;
    ctx->UpdateSubresource(surf.Get(),0,nullptr,surface,sizeof(surface),0);
    uiDepthFrameBoundary(ctx.Get());
    ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,.1f,0);
    bind(menu.dsv.Get()); setZ(.96f); coverage(true,false);
    // Run the actual temporal depth accessor on the GPU. Its register
    // declarations and function are read unchanged from production source.
    std::ifstream source("src/d3d11/temporal_pass.cpp");
    std::string temporal((std::istreambuf_iterator<char>(source)), {});
    std::string merge;
    for(const char* start : {"Texture2D<float> Z :", "Texture2D<float> ZS :", "Texture2D<float> ZUI :", "float zSceneAt("}) {
        auto begin=temporal.find(start); check(begin!=std::string::npos,"temporal depth source found");
        merge += temporal.substr(begin,temporal.find('\n',begin)-begin)+"\n";
    }
    merge += "RWTexture2D<float> Result:register(u0);[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){Result[id.xy]=zSceneAt(id.xy);}";
    auto mergeCode=compile(merge.c_str(),"cs_5_0"); ComPtr<ID3D11ComputeShader> mergeCs;
    hr(dev->CreateComputeShader(mergeCode->GetBufferPointer(),mergeCode->GetBufferSize(),nullptr,&mergeCs));
    D3D11_SHADER_RESOURCE_VIEW_DESC zd{}; zd.Format=DXGI_FORMAT_R32_FLOAT; zd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; zd.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> sceneRead; hr(dev->CreateShaderResourceView(scene.tex.Get(),&zd,&sceneRead));
    td.BindFlags=D3D11_BIND_UNORDERED_ACCESS; ComPtr<ID3D11Texture2D> merged; ComPtr<ID3D11UnorderedAccessView> mergeUav;
    hr(dev->CreateTexture2D(&td,nullptr,&merged)); hr(dev->CreateUnorderedAccessView(merged.Get(),nullptr,&mergeUav));
    ctx->OMSetRenderTargets(0,nullptr,nullptr); ctx->CSSetShader(mergeCs.Get(),nullptr,0);
    ctx->CSSetShaderResources(2,1,sceneRead.GetAddressOf()); ctx->CSSetShaderResources(7,1,&ui);
    ctx->CSSetUnorderedAccessViews(0,1,mergeUav.GetAddressOf(),nullptr);ctx->Dispatch(1,1,1);
    values=read(dev.Get(),ctx.Get(),merged.Get());
    check(std::fabs(values[1]-.24f)<1e-5f && std::fabs(values[6]-.1f)<1e-5f,"temporal accessor merges UI with scene");
    ID3D11ShaderResourceView* nullRead=nullptr; ctx->CSSetShaderResources(2,1,&nullRead);
    ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,.5f,0);
    ctx->CSSetShaderResources(2,1,sceneRead.GetAddressOf());ctx->Dispatch(1,1,1);
    for(float z:read(dev.Get(),ctx.Get(),merged.Get())) check(std::fabs(z-.5f)<1e-5f,"later nearer scene geometry wins temporal merge");
    ctx->CSSetShaderResources(7,1,&nullRead);ctx->Dispatch(1,1,1);
    for(float z:read(dev.Get(),ctx.Get(),merged.Get())) check(std::fabs(z-.5f)<1e-5f,"unbound UI layer leaves scene depth unchanged");
    ctx->ClearState();
    // Render the actual flight-HUD coverage over a distant station. The
    // game's VS carries clip w (linear distance) in tc1.z, and its depth
    // resolve is linear too. It must not be compared to SV_Position.z.
    {
        auto hudCode = compile(R"(
cbuffer C:register(b0){float4 v;}
struct O {float4 tc0:TEXCOORD0;float4 tc1:TEXCOORD1;float4 tc2:TEXCOORD2;
float4 tc5:TEXCOORD5;float4 tc9:TEXCOORD9;float4 tc10:TEXCOORD10;
float4 tc13:TEXCOORD13;float4 tc16:TEXCOORD16;float2 tc17:TEXCOORD17;
float3 tc18:TEXCOORD18;float4 pos:SV_Position;};
O main(uint id:SV_VertexID){O o=(O)0;float2 p=float2((id<<1)&2,id&2)*float2(2,-2)+float2(-1,1);
o.pos=float4(p,v.x,1);o.tc1=float4(p*v.y,v.y,100);o.tc2.w=1;o.tc5=float4(1,0,0,1);o.tc0=float4(0,0,-5,1);
o.tc9.w=1;o.tc13.w=1;o.tc17.x=v.z;o.tc18=float3(.5,0,0);return o;}
)", "vs_5_0");
        ComPtr<ID3D11VertexShader> hudVs;
        hr(dev->CreateVertexShader(hudCode->GetBufferPointer(),hudCode->GetBufferSize(),nullptr,&hudVs));
        float cb1[205*4] = {}; cb1[204*4] = 1; cb1[202*4+3]=cb1[203*4]=12;
        const int marchSteps=16;std::memcpy(&cb1[203*4+3],&marchSteps,sizeof(marchSteps));
        bd.ByteWidth=sizeof(cb1); D3D11_SUBRESOURCE_DATA cbData{cb1,0,0}; ComPtr<ID3D11Buffer> hudCb;
        hr(dev->CreateBuffer(&bd,&cbData,&hudCb));
        td.Width=td.Height=1; td.Format=DXGI_FORMAT_R32_FLOAT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        float linearDepth=15000; D3D11_SUBRESOURCE_DATA linearData{&linearDepth,4,0};
        ComPtr<ID3D11Texture2D> linear; ComPtr<ID3D11ShaderResourceView> linearSrv;
        hr(dev->CreateTexture2D(&td,&linearData,&linear)); hr(dev->CreateShaderResourceView(linear.Get(),nullptr,&linearSrv));
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        ComPtr<ID3D11Texture2D> noise;ComPtr<ID3D11ShaderResourceView> noiseSrv;
        hr(dev->CreateTexture2D(&td,nullptr,&noise));hr(dev->CreateShaderResourceView(noise.Get(),nullptr,&noiseSrv));
        ComPtr<ID3D11PixelShader> gameHud;
        ComPtr<ID3D11Texture2D> reference;ComPtr<ID3D11RenderTargetView> referenceRtv;
        if(argc>1) {
            // Optional differential check against the user's installed shader,
            // never distributed with this repository.
            std::ifstream input(argv[1],std::ios::binary);std::vector<char> binary((std::istreambuf_iterator<char>(input)),{});
            check(!binary.empty(),"reference game shader read");hr(dev->CreatePixelShader(binary.data(),binary.size(),nullptr,&gameHud));
            td.Width=td.Height=8;td.BindFlags=D3D11_BIND_RENDER_TARGET;
            hr(dev->CreateTexture2D(&td,nullptr,&reference));hr(dev->CreateRenderTargetView(reference.Get(),nullptr,&referenceRtv));
        }
        // Native TAA and NVIDIA both need classification at zero reactivity.
        for (bool trained : {false,true}) for (int kind=0;kind<13;++kind) {
            ctx->ClearState(); uiDepthFrameBoundary(ctx.Get());
            ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,.025f/linearDepth,0);
            bind(scene.dsv.Get()); ctx->VSSetShader(hudVs.Get(),nullptr,0);
            ctx->PSSetShaderResources(0,1,linearSrv.GetAddressOf());
            ctx->PSSetShaderResources(1,1,noiseSrv.GetAddressOf());
            ctx->PSSetShaderResources(2,1,surfSrv.GetAddressOf()); // caller binding must survive the scene read
            ctx->PSSetSamplers(1,1,sampler.GetAddressOf()); ctx->PSSetConstantBuffers(1,1,hudCb.GetAddressOf());
            // 0 floating core, 1 attached core, 2 glow, 3 behind the scene.
            float distance=kind==1?14000.0f:kind==3?16000.0f:30.0f;
            // A resolve/clip-W scale need not be the temporal metre scale.
            // The former re-encoding saturated this floating core to Z=1,
            // despite the real scene behind it remaining 15 km away.
            const float resolved=kind==12?.01f:linearDepth;
            if(kind==12)distance=.001f;
            ctx->UpdateSubresource(linear.Get(),0,nullptr,&resolved,4,0);
            float vertex[4]={.025f/distance,distance,kind==2?1.0f:kind==11?.7f:.5f,0};
            if(kind==12)vertex[0]=.025f/30.0f;
            ctx->UpdateSubresource(position.Get(),0,nullptr,vertex,0,0);
            cb1[202*4+3]=cb1[203*4]=kind==4?0.0f:kind==10?.1f:12.0f;
            ctx->UpdateSubresource(hudCb.Get(),0,nullptr,cb1,0,0);
            float noiseValue=kind==5?0:kind==6?.25f:kind==7?.5f:1;
            float noiseChannels[4]={noiseValue,noiseValue,noiseValue,noiseValue};
            if(kind==8)noiseChannels[0]=0;
            if(kind==9)noiseChannels[1]=0;
            ctx->UpdateSubresource(noise.Get(),0,nullptr,noiseChannels,16,0);
            std::vector<float> actualAlpha;
            if(gameHud) {
                const float clear[4]={};ctx->ClearRenderTargetView(referenceRtv.Get(),clear);
                ctx->OMSetRenderTargets(1,referenceRtv.GetAddressOf(),scene.dsv.Get());ctx->PSSetShader(gameHud.Get(),nullptr,0);
                ctx->Draw(3,0);ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),scene.dsv.Get());
                actualAlpha=read(dev.Get(),ctx.Get(),reference.Get(),3);
                std::printf("HUD reference case %d: alpha %.6f\n",kind,actualAlpha[0]);
            }
            g_on=true;g_trained=trained;g_reactive=0;g_alphaFloor=.5f;
            g_mode=Mode::kReissueScene;g_reissueShader=&g_depthShaders[1];g_drawEye=0;
            g_reissueMaskSlot=2;g_reissueMaskOffset=0;g_rebindW=g_rebindH=8;g_wantMask=true;
            check(uiDepthReissueBegin(ctx.Get()),"HUD coverage at zero reactivity begins");
            ctx->Draw(3,0);uiDepthReissueEnd(ctx.Get());ctx->OMSetRenderTargets(0,nullptr,nullptr);
            ComPtr<ID3D11ShaderResourceView> restoredHudSrv;
            ctx->PSGetShaderResources(2,1,&restoredHudSrv);
            check(restoredHudSrv.Get()==surfSrv.Get(),"HUD scene-depth read restores caller PS slot 2");
            ID3D11Texture2D* mask=nullptr;
            check(uiDepthCoverageMask(8,8,0,&mask)&&mask,"motion coverage remains available at zero reactivity");
            auto maskValues=read(dev.Get(),ctx.Get(),mask);
            check(!uiDepthReactiveMask(8,8,0,&mask)&&!mask,"zero reactivity supplies no NVIDIA bias texture");
            for(size_t j=0;j<maskValues.size();++j) {
                if(kind<6)check(std::lround(maskValues[j]*255)==(kind==0?1:kind==1?2:0),"HUD coverage ignores transparent strokes and preserves opaque motion classification");
                if(gameHud)check((maskValues[j]>0)==(actualAlpha[j]>=.7f),"coverage matches installed shader opacity, including noise and density");
            }
            check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"HUD private depth available");
            ui->GetResource(privateRes.ReleaseAndGetAddressOf());
            float expected=kind==1?.025f/distance:.025f/linearDepth;
            for(float z:read(dev.Get(),ctx.Get(),privateRes.Get())) check(std::fabs(z-expected)<1e-10f,"HUD coverage writes device depth in the correct units");
            for(float z:read(dev.Get(),ctx.Get(),scene.tex.Get())) check(std::fabs(z-.025f/linearDepth)<1e-10f,"HUD fix preserves live scene depth");
        }
        ctx->ClearState();uiDepthFrameBoundary(ctx.Get());
    }
    // The sprite VS deliberately writes clip Z=abs(W). Reusing the screen
    // coverage shader reproduces depth 1 over a distant station. Exercise
    // the dedicated production sprite path with that exact VS contract,
    // including near UI, sky, nearer geometry, alpha and caller t2 state.
    {
        auto code=compile(R"(
cbuffer C:register(b0){float4 v;}
struct O{float2 tc0:TEXCOORD0;float4 pos:SV_Position;};
O main(uint id:SV_VertexID){O o;float2 p=float2((id<<1)&2,id&2);
o.pos=float4((p*float2(2,-2)+float2(-1,1))*v.x,abs(v.x),v.x);o.tc0=p;return o;}
)","vs_5_0");
        ComPtr<ID3D11VertexShader> spriteVs;
        hr(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&spriteVs));
        const DXGI_FORMAT formats[]={DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R32G8X24_TYPELESS};
        const DXGI_FORMAT views[]={DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
        for(int format=0;format<3;++format) for(bool trained:{false,true})
        for(float metres:{1.0f,16587.594f}) for(float behind:{0.0f,.025f/15000.0f}) {
            ctx->ClearState();uiDepthFrameBoundary(ctx.Get());
            auto target=depth(dev.Get(),formats[format],views[format]);
            testScene=target.tex.Get();ctx->ClearDepthStencilView(target.dsv.Get(),D3D11_CLEAR_DEPTH,behind,0);
            const float actualScene=read(dev.Get(),ctx.Get(),target.tex.Get())[1];
            bind(target.dsv.Get());setZ(metres);ctx->VSSetShader(spriteVs.Get(),nullptr,0);
            ctx->PSSetShaderResources(2,1,surfSrv.GetAddressOf());
            g_on=true;g_trained=trained;g_reactive=0;g_mode=Mode::kReissueScene;
            g_reissueShader=&g_depthShaders[5];g_drawEye=0;g_reissueMaskSlot=1;g_reissueMaskOffset=0;
            g_rebindW=g_rebindH=8;g_wantRebind=false;g_wantMask=true;
            check(uiDepthReissueBegin(ctx.Get()),"sprite coverage begins");ctx->Draw(3,0);uiDepthReissueEnd(ctx.Get());
            ComPtr<ID3D11ShaderResourceView> restored;ctx->PSGetShaderResources(2,1,&restored);
            check(restored.Get()==surfSrv.Get(),"sprite restores caller t2");
            check(uiDepthTemporalDepth(8,8,0,target.tex.Get(),&ui),"sprite publishes temporal depth");
            ui->GetResource(privateRes.ReleaseAndGetAddressOf());values=read(dev.Get(),ctx.Get(),privateRes.Get());
            const float expected=(std::max)(actualScene,.025f/metres);
            const float tolerance=format==1?1.0f/16777215.0f:expected*1e-5f+1e-10f;
            if(std::fabs(values[1]-expected)>tolerance) {
                std::printf("sprite format %d trained %d metres %.7g scene %.9g got %.9g expected %.9g\n",
                            format,trained,metres,actualScene,values[1],expected);
            }
            check(std::fabs(values[1]-expected)<=tolerance,"sprite recovers physical depth instead of forced Z=1");
            check(values[6]==actualScene,"sprite transparent fringe preserves scene depth");
            check(read(dev.Get(),ctx.Get(),target.tex.Get())[1]==actualScene,"sprite leaves original scene depth unchanged");
            auto mask=read(dev.Get(),ctx.Get(),g_mask[0].tex);
            check(std::lround(mask[1]*255)==1 && mask[6]==0,"sprite coverage survives zero bias and nearer station");
        }
        testScene=scene.tex.Get();ctx->ClearState();uiDepthFrameBoundary(ctx.Get());
    }
    // Exercise the orbital reissue through the actual private-depth pass,
    // including multi-instance indices and restoration of both shader stages.
    {
        ctx->ClearState();uiDepthFrameBoundary(ctx.Get());g_holoMotion[0]=HoloMotion{};
        auto code=compile(kOrbitalCoverageVs,"vs_5_0");hr(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&g_orbitalVs));
        D3D11_INPUT_ELEMENT_DESC elements[]={{"POSTANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"OSTOWST",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1},{"OSTOWSR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,16,D3D11_INPUT_PER_INSTANCE_DATA,1},{"OSTOWSS",0,DXGI_FORMAT_R32G32B32_FLOAT,1,32,D3D11_INPUT_PER_INSTANCE_DATA,1},{"COLOUR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,44,D3D11_INPUT_PER_INSTANCE_DATA,1}};
        ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(elements,5,code->GetBufferPointer(),code->GetBufferSize(),&layout));
        auto make=[&](UINT bytes,UINT bind,const void* data){D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.BindFlags=bind;D3D11_SUBRESOURCE_DATA init{data,0,0};ComPtr<ID3D11Buffer> b;hr(dev->CreateBuffer(&desc,&init,&b));return b;};
        float sc[333*4]{};sc[270*4]=sc[271*4+1]=sc[272*4+3]=sc[277*4]=sc[278*4+1]=sc[279*4+2]=1;sc[273*4+2]=.025f;sc[332*4+2]=sc[332*4+3]=.125f;
        float material[8]{};material[6]=1;UINT orbitalSentinel[4]={91,0,0,0};auto sceneCb=make(sizeof(sc),D3D11_BIND_CONSTANT_BUFFER,sc),materialCb=make(sizeof(material),D3D11_BIND_CONSTANT_BUFFER,material),savedCb=make(sizeof(orbitalSentinel),D3D11_BIND_CONSTANT_BUFFER,orbitalSentinel);
        float vertices[16]={-.5f,0,1,0,-.5f,0,1,0,.5f,0,1,0,.5f,0,1,0},instances[30]{};
        for(int i=0;i<2;++i){float* p=instances+i*15;p[1]=i?.5f:-.5f;p[2]=1;p[3]=1;p[7]=1;p[8]=p[9]=p[10]=p[11]=p[12]=p[13]=p[14]=1;}
        auto vb0=make(sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,vertices),vb1=make(sizeof(instances),D3D11_BIND_VERTEX_BUFFER,instances);
        ID3D11Buffer* vb[]={vb0.Get(),vb1.Get()};UINT strides[]={16,60},offsets[]={0,0};ctx->IASetVertexBuffers(0,2,vb,strides,offsets);ctx->IASetInputLayout(layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->VSSetConstantBuffers(1,1,sceneCb.GetAddressOf());ctx->PSSetConstantBuffers(2,1,materialCb.GetAddressOf());ctx->VSSetConstantBuffers(12,1,savedCb.GetAddressOf());ctx->PSSetConstantBuffers(12,1,savedCb.GetAddressOf());
        ctx->RSSetState(raster.Get());D3D11_VIEWPORT orbitalVp{0,0,8,8,0,1};ctx->RSSetViewports(1,&orbitalVp);ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);testScene=scene.tex.Get();ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),scene.dsv.Get());
        g_on=true;g_mode=Mode::kReissueScene;g_reissueShader=&g_depthShaders[7];g_drawEye=0;g_reissueMaskSlot=0;g_rebindW=g_rebindH=8;g_wantMask=true;g_holoDraw={'N',4,2,0,0,0};
        check(uiDepthReissueBegin(ctx.Get()),"orbital private reissue begins");ctx->DrawInstanced(4,2,0,0);uiDepthReissueEnd(ctx.Get());
        ComPtr<ID3D11VertexShader> afterVs;ctx->VSGetShader(&afterVs,nullptr,nullptr);check(afterVs.Get()==vs.Get(),"orbital reissue restores original vertex shader");
        ComPtr<ID3D11Buffer> afterCb;ctx->VSGetConstantBuffers(12,1,&afterCb);check(afterCb.Get()==savedCb.Get(),"orbital reissue restores VS constants");afterCb.Reset();ctx->PSGetConstantBuffers(12,1,&afterCb);check(afterCb.Get()==savedCb.Get(),"orbital reissue restores PS constants");
        ID3D11ShaderResourceView* views[2]{};g_holoMotion[0].views(scene.tex.Get(),views);ComPtr<ID3D11Resource> orbitalCoverage;views[0]->GetResource(&orbitalCoverage);auto indices=read(dev.Get(),ctx.Get(),orbitalCoverage.Get());unsigned counts[3]{};for(float v:indices)if(v>=0&&v<=2)++counts[unsigned(v)];
        std::printf("orbital indices %u/%u/%u\n",counts[0],counts[1],counts[2]);check(counts[1]>0&&counts[2]>0,"orbital pixels carry their actual instance index");for(float v:read(dev.Get(),ctx.Get(),scene.tex.Get()))check(v==0,"orbital coverage preserves live game depth");
        ctx->ClearState();uiDepthFrameBoundary(ctx.Get());g_holoDraw={};
    }
    // Execute the actual adaptive UI helper, including its production t8/u6
    // bindings. Previous evidence is raw UI colour, not temporal output.
    {
        ctx->ClearState();
        auto first=temporal.find("float4 uiEvidence(");
        auto last=temporal.find("bool uiCovered(",first);
        check(first!=std::string::npos && last!=std::string::npos,"adaptive UI source found");
        std::string shader=R"(
Texture2D<float4> S:register(t0); Texture2D<float> UM:register(t4);
Texture2D<float4> UP:register(t8); RWTexture2D<float4> UN:register(u6);
RWTexture2D<float> Result:register(u0); SamplerState L:register(s0);
cbuffer C:register(b0){int4 region;int2 size;int2 texSize;float4 jit;float4 probe;float4 offset;};
)"+temporal.substr(first,last-first)+R"(
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
UN[id.xy]=uiEvidence(id.xy);Result[id.xy]=adaptiveUiReactive(id.xy,float2(id.xy)+offset.xy);}
)";
        auto code=compile(shader.c_str(),"cs_5_0"); ComPtr<ID3D11ComputeShader> cs;
        hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs));
        D3D11_TEXTURE2D_DESC image{}; image.Width=image.Height=8;image.MipLevels=image.ArraySize=1;
        image.SampleDesc.Count=1;image.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        image.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> colour,previous,next,mark,result;
        hr(dev->CreateTexture2D(&image,nullptr,&colour));hr(dev->CreateTexture2D(&image,nullptr,&previous));
        hr(dev->CreateTexture2D(&image,nullptr,&next));image.Format=DXGI_FORMAT_R8_UNORM;
        hr(dev->CreateTexture2D(&image,nullptr,&mark));image.Format=DXGI_FORMAT_R32_FLOAT;
        hr(dev->CreateTexture2D(&image,nullptr,&result));
        ComPtr<ID3D11ShaderResourceView> colourView,previousView,markView;
        hr(dev->CreateShaderResourceView(colour.Get(),nullptr,&colourView));
        hr(dev->CreateShaderResourceView(previous.Get(),nullptr,&previousView));
        hr(dev->CreateShaderResourceView(mark.Get(),nullptr,&markView));
        ComPtr<ID3D11UnorderedAccessView> resultView,nextView;
        hr(dev->CreateUnorderedAccessView(result.Get(),nullptr,&resultView));
        hr(dev->CreateUnorderedAccessView(next.Get(),nullptr,&nextView));
        struct Params {int region[4]={0,0,8,8};int size[2]={8,8};int texSize[2]={8,8};
            float jit[4]={};float probe[4]={0,0,1,6};float offset[4]={};} params;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(params);desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> constants;hr(dev->CreateBuffer(&desc,nullptr,&constants));
        D3D11_SAMPLER_DESC samp{};samp.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU=samp.AddressV=samp.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;samp.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> linear;hr(dev->CreateSamplerState(&samp,&linear));
        std::vector<unsigned char> now(256),old(256),mask(64);
        auto stroke=[&](int x,int y,bool current,unsigned char r=25,unsigned char g=204,unsigned char b=51) {
            int i=y*8+x;auto& pixels=current?now:old;
            pixels[i*4]=r;pixels[i*4+1]=g;pixels[i*4+2]=b;pixels[i*4+3]=255;
            if(current)mask[i]=1;
        };
        auto reset=[&]() {now.assign(256,0);old.assign(256,0);mask.assign(64,0);params=Params{};};
        auto run=[&]() {
            ctx->UpdateSubresource(colour.Get(),0,nullptr,now.data(),32,0);
            ctx->UpdateSubresource(previous.Get(),0,nullptr,old.data(),32,0);
            ctx->UpdateSubresource(mark.Get(),0,nullptr,mask.data(),8,0);
            ctx->UpdateSubresource(constants.Get(),0,nullptr,&params,0,0);
            ID3D11ShaderResourceView* views[9]={colourView.Get(),nullptr,nullptr,nullptr,markView.Get(),nullptr,nullptr,nullptr,previousView.Get()};
            ID3D11UnorderedAccessView* outputs[7]={resultView.Get(),nullptr,nullptr,nullptr,nullptr,nullptr,nextView.Get()};
            ctx->CSSetShader(cs.Get(),nullptr,0);ctx->CSSetShaderResources(0,9,views);
            ctx->CSSetUnorderedAccessViews(0,7,outputs,nullptr);ctx->CSSetConstantBuffers(0,1,constants.GetAddressOf());
            ctx->CSSetSamplers(0,1,linear.GetAddressOf());ctx->Dispatch(1,1,1);ctx->ClearState();
            return read(dev.Get(),ctx.Get(),result.Get());
        };
        reset();stroke(3,3,true);stroke(3,3,false);
        for(float v:run())check(v<.001f,"stable UI retains temporal smoothing");
        ctx->CopyResource(previous.Get(),next.Get());
        ctx->UpdateSubresource(colour.Get(),0,nullptr,now.data(),32,0);
        // Read the evidence actually emitted at u6 on the preceding dispatch.
        ID3D11ShaderResourceView* carried[9]={colourView.Get(),nullptr,nullptr,nullptr,markView.Get(),nullptr,nullptr,nullptr,previousView.Get()};
        ID3D11UnorderedAccessView* targets[7]={resultView.Get(),nullptr,nullptr,nullptr,nullptr,nullptr,nextView.Get()};
        ctx->CSSetShader(cs.Get(),nullptr,0);ctx->CSSetShaderResources(0,9,carried);
        ctx->CSSetUnorderedAccessViews(0,7,targets,nullptr);ctx->CSSetConstantBuffers(0,1,constants.GetAddressOf());
        ctx->CSSetSamplers(0,1,linear.GetAddressOf());ctx->Dispatch(1,1,1);ctx->ClearState();
        for(float v:read(dev.Get(),ctx.Get(),result.Get()))check(v<.001f,"GPU-produced evidence remains stable on the following frame");
        params.probe[3]=4;
        auto reactivity=run();check(reactivity[27]>.999f,"new history starts with fresh UI");
        params.probe[3]=0;
        for(float v:run())check(v<.001f,"disabled adaptation does not reject history");
        reset();stroke(3,3,true,204,25,51);stroke(3,3,false);
        reactivity=run();check(reactivity[27]>.999f,"changed UI colour rejects stale UI");
        reset();stroke(3,3,true);reactivity=run();check(reactivity[27]>.999f,"appearing UI is fresh");
        reset();stroke(3,3,false);reactivity=run();check(reactivity[27]>.999f,"erased isolated stroke clears its history");
        check(reactivity[35]>.999f,"transparent centre rejects a departed glyph in the history footprint");
        check(reactivity[36]>.999f,"diagonal reconstruction footprint rejects departing UI");
        check(reactivity[63]<.001f,"unrelated sky retains history");
        reset();stroke(4,3,true);stroke(3,3,false);
        reactivity=run();check(reactivity[28]<.001f,"one pixel raster shift tolerates matching UI colour at the stroke");
        params.offset[0]=-1;
        reactivity=run();check(reactivity[28]<.001f,"motion aligns UI evidence");
        reset();stroke(3,3,true);stroke(3,3,false);
        for(int i=0;i<64;++i)if(!mask[i])now[i*4]=255;
        for(float v:run())check(v<.001f,"background change outside UI preserves UI history");
        reset();stroke(3,3,true);stroke(3,3,false);mask[27]=2;
        for(float v:run())check(v<.001f,"attached UI uses adaptation as well as floating UI");
        params.offset[0]=20;reactivity=run();check(reactivity[27]>.999f,"offscreen history cannot blur current UI");
        reset();stroke(3,3,true);mask[27]=3;
        for(float v:run())check(v<.001f,"smoke coverage cannot trigger adaptive UI rejection");
        reset();stroke(3,3,true);mask[27]=255;
        for(float v:run())check(v<.001f,"reactive smoke is distinct from UI at every bias strength");
        reset();stroke(3,3,true);params.probe[2]=0;
        for(float v:run())check(v<.001f,"unbound coverage cannot mark scene content as UI");
        std::puts("PASS: production adaptive UI shader keeps stable strokes and rejects changed, new and erased UI.");
    }
    // Copy/view compatibility, identity, resize and frame rollover for all
    // depth encodings supported by the temporal pass.
    for(auto formats : {std::pair{DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT},
                        std::pair{DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_D24_UNORM_S8_UINT},
                        std::pair{DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT}}) {
        UiDepthLayer layer; auto t=depth(dev.Get(),formats.first,formats.second);
        ctx->ClearDepthStencilView(t.dsv.Get(),D3D11_CLEAR_DEPTH,.25f,0);
        check(layer.acquire(ctx.Get(),t.tex.Get())!=nullptr,"private allocation for depth format");
        auto* liveRead=layer.sourceView(ctx.Get());
        check(liveRead!=nullptr,"floating HUD can read original scene depth in each supported format");
        ComPtr<ID3D11Resource> liveRes;liveRead->GetResource(&liveRes);
        check(liveRes.Get()==t.tex.Get(),"HUD reads the original scene, never its writable private target");
        check(layer.view(scene.tex.Get(),8,8)==nullptr,"wrong source identity rejected");
        check(layer.view(t.tex.Get(),9,8)==nullptr,"wrong size rejected");
        layer.view(t.tex.Get(),8,8)->GetResource(privateRes.ReleaseAndGetAddressOf());
        for(float z:read(dev.Get(),ctx.Get(),privateRes.Get())) check(std::fabs(z-.25f)<1e-5f,"copied depth encoding");
        ctx->ClearDepthStencilView(t.dsv.Get(),D3D11_CLEAR_DEPTH,.75f,0);
        check(std::fabs(read(dev.Get(),ctx.Get(),liveRes.Get())[0]-.75f)<1e-5f,"HUD sees current scene depth without an extra copy");
        layer.acquire(ctx.Get(),t.tex.Get());
        check(std::fabs(read(dev.Get(),ctx.Get(),privateRes.Get())[0]-.25f)<1e-5f,"one seed per frame preserves earlier UI");
        layer.frameBoundary(); check(layer.view(t.tex.Get(),8,8)==nullptr,"frame invalidation");
        check(layer.sourceView(ctx.Get())==nullptr,"old-frame HUD scene view is not published");
        layer.acquire(ctx.Get(),t.tex.Get()); check(std::fabs(read(dev.Get(),ctx.Get(),privateRes.Get())[0]-.75f)<1e-5f,"next frame reseeded");
        auto resized=depth(dev.Get(),formats.first,formats.second,16);
        check(layer.acquire(ctx.Get(),resized.tex.Get())!=nullptr,"resize supported");
        check(layer.view(t.tex.Get(),8,8)==nullptr,"old resource rejected after resize");
    }
    auto msaa=depth(dev.Get(),DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT,8,4);
    UiDepthLayer declined; check(!declined.acquire(ctx.Get(),msaa.tex.Get()),"MSAA safely declined");
    if(info) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<unsigned char> storage(size);
        auto* m=reinterpret_cast<D3D11_MESSAGE*>(storage.data()); hr(info->GetMessage(i,m,&size));
        if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
            std::puts(m->pDescription); check(false,"D3D debug-layer warning/error");
        }
    }
    ctx->ClearState(); uiDepthShutdown();
    std::printf("PASS: %d checks; production UI coverage isolates smoke, preserves depth/alpha/occlusion/state, and handles menus and frame/eye/format changes.\n",checks);
}
