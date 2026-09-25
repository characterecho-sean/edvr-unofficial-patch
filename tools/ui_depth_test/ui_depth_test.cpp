// Exercise the production coverage pass on D3D11 WARP. Only the game's
// depth-probe selection, logging and raw-hook entry are supplied here.
// This rig supplies its own binding shadow readers (below), so it asks the
// header for declarations rather than the inline production ones.
#define EDVR_BINDING_SHADOW_EXTERNAL 1
#include "../../src/d3d11/ui_depth.cpp"
#include "../../src/d3d11/ui_resolve.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <cmath>
#include <algorithm>
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
// ui_depth.cpp's UI content census reads this (objectProbeLedgerActive,
// object_probe.h) to gate its per-draw logging; driven directly below,
// the way g_holoDepthOn and the rest of detail:: already are on this page.
namespace detail { bool g_objectProbeOn = false; bool g_objectProbeLedgerOn = false; }
int64_t qpcNow() { LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER t;QueryPerformanceFrequency(&t);return t.QuadPart; }
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
bool bindingResolveResource(void*, ResourceInfo*) { std::abort(); }
bool depthProbeIsSceneDepth(const void*) { std::abort(); }
uint64_t lookupShaderHash(void*) { std::abort(); }
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx, const char* source, size_t,
    const char*, const char*, const SwapMacro*, const char*) {
    auto code=::compile(source,"cs_5_0");ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11ComputeShader* shader=nullptr;
    if(FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))std::abort();return shader;
}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* ctx, const char* source, size_t,
    const char*, const char*, const SwapMacro*, const char*) {
    auto code=::compile(source,"ps_5_0");ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11PixelShader* shader=nullptr;
    if(FAILED(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))std::abort();return shader;
}
float temporalPassDepthAt(float metres) { return 0.025f / metres; }
bool temporalPassPlanes(float* nearZ, float* farZ) { *nearZ = .025f; *farZ = 10000; return true; }
bool depthProbeSceneDepthFormat(uint32_t, uint32_t, int, ID3D11Texture2D** tex, uint32_t* fmt) {
    *tex = testScene; *fmt = DXGI_FORMAT_D32_FLOAT; return testScene != nullptr;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* ctx, UINT n,
                               ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* ds) {
    ctx->OMSetRenderTargets(n, rt, ds);
}
// Pass-through: this rig drives the coverage passes directly, so the hook
// these bypass in production (the draw census, eye-draw gate, foveation,
// probe) never needs to see them here either.
void vScreenDrawRaw(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertex) {
    ctx->Draw(vertexCount, startVertex);
}
void vScreenVSSetShaderRaw(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs,
                           ID3D11ClassInstance* const* classInstances, UINT numClassInstances) {
    ctx->VSSetShader(vs, classInstances, numClassInstances);
}
void vScreenPSSetShaderRaw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
                           ID3D11ClassInstance* const* classInstances, UINT numClassInstances) {
    ctx->PSSetShader(ps, classInstances, numClassInstances);
}
void vScreenOMSetBlendStateRaw(ID3D11DeviceContext* ctx, ID3D11BlendState* state,
                               const float blendFactor[4], UINT sampleMask) {
    ctx->OMSetBlendState(state, blendFactor, sampleMask);
}
void vScreenUpdateSubresourceRaw(ID3D11DeviceContext* ctx, ID3D11Resource* dstResource,
                                 UINT dstSubresource, const D3D11_BOX* dstBox,
                                 const void* srcData, UINT srcRowPitch, UINT srcDepthPitch) {
    ctx->UpdateSubresource(dstResource, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
}
void vScreenRSSetViewportsRaw(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* vps) {
    ctx->RSSetViewports(n, vps);
}
void vScreenClearRenderTargetViewRaw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                                     const float colour[4]) {
    ctx->ClearRenderTargetView(rtv, colour);
}
}

using namespace edvr;
// Production timer destruction is inert for process exit. Local test owners
// explicitly reset their content/timers while the WARP device is still alive.
struct TestUiContent : UiContent { ~TestUiContent() { reset(); } };
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
        else if (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM) values[y * td.Width + x] = p[channel] / 255.0f;
        else if (td.Format == DXGI_FORMAT_R24G8_TYPELESS)
            values[y * td.Width + x] = static_cast<float>(*reinterpret_cast<const UINT*>(p) & 0xffffff) / 16777215.0f;
        else values[y * td.Width + x] = reinterpret_cast<const float*>(p)[channel];
    }
    ctx->Unmap(stage.Get(), 0); return values;
}
#include "planet_coverage_test.h"
#include "corona_coverage_test.h"
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
    check(gpuTimingBind(dev.Get(),ctx.Get()),"bind canonical WARP timer owner");
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
        ComPtr<ID3D11DepthStencilState> before; UINT beforeRef=0;
        ctx->OMGetDepthStencilState(&before,&beforeRef);
        detail::g_uiDepthOn = true; detail::g_uiDepthMode = menu ? Mode::kReissue : Mode::kReissueScene;
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
        check(d.Get() == before.Get() && ref == beforeRef, "depth state and stencil reference restored");
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
    detail::g_uiDepthOn = false; check(!uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"disabled feature does not publish"); detail::g_uiDepthOn = true;
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
    // The escape profile draws over nearer cockpit holograms with depth off.
    ds.DepthEnable=FALSE;
    ComPtr<ID3D11DepthStencilState> overlay;hr(dev->CreateDepthStencilState(&ds,&overlay));
    for(bool depthTest:{true,false}) {
        uiDepthFrameBoundary(ctx.Get());ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,.8f,0);
        bind(menu.dsv.Get());setZ(.96f);
        if(!depthTest)ctx->OMSetDepthStencilState(overlay.Get(),13);
        coverage(true,true);
        check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"overlay publishes depth");ui->GetResource(privateRes.ReleaseAndGetAddressOf());
        values=read(dev.Get(),ctx.Get(),privateRes.Get());
        for(int y=0;y<8;++y)for(int x=0;x<8;++x)
            check(std::fabs(values[y*8+x]-(!depthTest && x<4?.24f:.8f))<1e-5f,"visible menu replaces foreground depth only under its alpha; depth-tested menu stays occluded");
        for(float v:read(dev.Get(),ctx.Get(),scene.tex.Get()))check(std::fabs(v-.8f)<1e-5f,"overlay never changes game depth");
    }
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
    for (uint64_t material : {kHoloLitPs,kHoloUnlitPs})
    for (float distance : {.6f, 16000.0f}) for (float alpha : {124.0f/255,33.0f/255,1.0f/255,.001f,0.0f,.8f}) {
        surface[3]=alpha; ctx->UpdateSubresource(surf.Get(),0,nullptr,surface,sizeof(surface),0);
        uiDepthFrameBoundary(ctx.Get()); ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        bind(scene.dsv.Get()); setZ(distance); ctx->VSSetShader(holoVs.Get(),nullptr,0);
        ID3D11ShaderResourceView* none=nullptr;
        const unsigned slot=holoSurfaceSlot(material);
        ctx->PSSetShaderResources(slot==1?2:1,1,&none);
        ctx->PSSetShaderResources(slot,1,surfSrv.GetAddressOf()); ctx->PSSetSamplers(1,1,sampler.GetAddressOf());
        detail::g_uiDepthOn=true; detail::g_uiDepthMode=Mode::kReissueScene; g_reissueShader=depthShaderFor(ctx.Get(),material,kHoloPanel,slot);
        check(g_reissueShader && holoShader(g_reissueShader) && g_reissueShader->slot==slot,"verified holo material selects its own surface binding and motion family");
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
    std::ifstream source("src/d3d11/temporal_shader_source.h");
    std::string temporal((std::istreambuf_iterator<char>(source)), {});
    std::string merge;
    for(const char* start : {"Texture2D<float> Z :", "Texture2D<float> ZS :", "Texture2D<float> ZUI :", "Texture2D<float4> Screen :"}) {
        auto begin=temporal.find(start); check(begin!=std::string::npos,"temporal depth source found");
        merge += temporal.substr(begin,temporal.find('\n',begin)-begin)+"\n";
    }
    merge+="static const float4 probe=0;\n";
    auto depthBegin=temporal.find("float zSceneAt("),depthEnd=temporal.find("float zAt(",depthBegin);
    check(depthBegin!=std::string::npos && depthEnd!=std::string::npos,"complete temporal depth accessor found");
    merge+=temporal.substr(depthBegin,depthEnd-depthBegin);
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
            detail::g_uiDepthOn=true;g_trained=trained;g_reactive=0;g_alphaFloor=.5f;
            detail::g_uiDepthMode=Mode::kReissueScene;g_reissueShader=&g_depthShaders[1];g_drawEye=0;
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
            detail::g_uiDepthOn=true;g_trained=trained;g_reactive=0;detail::g_uiDepthMode=Mode::kReissueScene;
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
        // The inline write guard (ui_depth.h): the frame boundary recomputes
        // it from both eyes' geometry maps, so with neither holding a corona
        // it stands down, and a resource write then skips the body -- which
        // could only have missed two empty maps.
        g_holoMotion[1]=HoloMotion{};detail::g_holoGeometryTracked.store(true);uiDepthFrameBoundary(ctx.Get());
        check(!detail::g_holoGeometryTracked.load(),"write guard stands down with no corona geometry tracked");
        auto code=compile(kOrbitalCoverageVs,"vs_5_0");hr(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&g_orbitalVs));
        D3D11_INPUT_ELEMENT_DESC elements[]={{"POSTANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"OSTOWST",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1},{"OSTOWSR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,16,D3D11_INPUT_PER_INSTANCE_DATA,1},{"OSTOWSS",0,DXGI_FORMAT_R32G32B32_FLOAT,1,32,D3D11_INPUT_PER_INSTANCE_DATA,1},{"COLOUR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,44,D3D11_INPUT_PER_INSTANCE_DATA,1}};
        ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(elements,5,code->GetBufferPointer(),code->GetBufferSize(),&layout));
        auto make=[&](UINT bytes,UINT bind,const void* data){D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.BindFlags=bind;D3D11_SUBRESOURCE_DATA init{data,0,0};ComPtr<ID3D11Buffer> b;hr(dev->CreateBuffer(&desc,&init,&b));return b;};
        float sc[333*4]{};sc[270*4]=sc[271*4+1]=sc[272*4+3]=sc[277*4]=sc[278*4+1]=sc[279*4+2]=1;sc[273*4+2]=.025f;sc[332*4+2]=sc[332*4+3]=.125f;
        float material[8]{};material[6]=1;UINT orbitalSentinel[4]={91,0,0,0};auto sceneCb=make(sizeof(sc),D3D11_BIND_CONSTANT_BUFFER,sc),materialCb=make(sizeof(material),D3D11_BIND_CONSTANT_BUFFER,material),savedCb=make(sizeof(orbitalSentinel),D3D11_BIND_CONSTANT_BUFFER,orbitalSentinel);
        float vertices[16]={-.5f,0,1,0,-.5f,0,1,0,.5f,0,1,0,.5f,0,1,0},instances[30]{};
        for(int i=0;i<2;++i){float* p=instances+i*15;p[1]=i?.5f:-.5f;p[2]=1;p[3]=1;p[7]=1;p[8]=p[9]=p[10]=p[11]=p[12]=p[13]=p[14]=1;}
        auto vb0=make(sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,vertices),vb1=make(sizeof(instances),D3D11_BIND_VERTEX_BUFFER,instances);
        auto bindOrbital=[&]() {
            ID3D11Buffer* vb[]={vb0.Get(),vb1.Get()};UINT strides[]={16,60},offsets[]={0,0};ctx->IASetVertexBuffers(0,2,vb,strides,offsets);ctx->IASetInputLayout(layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->VSSetConstantBuffers(1,1,sceneCb.GetAddressOf());ctx->PSSetConstantBuffers(2,1,materialCb.GetAddressOf());ctx->VSSetConstantBuffers(12,1,savedCb.GetAddressOf());ctx->PSSetConstantBuffers(12,1,savedCb.GetAddressOf());
            ctx->RSSetState(raster.Get());D3D11_VIEWPORT orbitalVp{0,0,8,8,0,1};ctx->RSSetViewports(1,&orbitalVp);ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),scene.dsv.Get());
        };
        ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);testScene=scene.tex.Get();bindOrbital();
        detail::g_uiDepthOn=true;detail::g_uiDepthMode=Mode::kReissueScene;g_reissueShader=&g_depthShaders[7];g_drawEye=0;g_reissueMaskSlot=0;g_rebindW=g_rebindH=8;g_wantMask=true;g_holoDraw={'N',4,2,0,0,0};
        check(uiDepthReissueBegin(ctx.Get()),"orbital private reissue begins");ctx->DrawInstanced(4,2,0,0);uiDepthReissueEnd(ctx.Get());
        ComPtr<ID3D11VertexShader> afterVs;ctx->VSGetShader(&afterVs,nullptr,nullptr);check(afterVs.Get()==vs.Get(),"orbital reissue restores original vertex shader");
        ComPtr<ID3D11Buffer> afterCb;ctx->VSGetConstantBuffers(12,1,&afterCb);check(afterCb.Get()==savedCb.Get(),"orbital reissue restores VS constants");afterCb.Reset();ctx->PSGetConstantBuffers(12,1,&afterCb);check(afterCb.Get()==savedCb.Get(),"orbital reissue restores PS constants");
        ID3D11ShaderResourceView* views[2]{};g_holoMotion[0].views(scene.tex.Get(),views);ComPtr<ID3D11Resource> orbitalCoverage;views[0]->GetResource(&orbitalCoverage);auto indices=read(dev.Get(),ctx.Get(),orbitalCoverage.Get());auto orbitalDepth=read(dev.Get(),ctx.Get(),orbitalCoverage.Get(),1);unsigned counts[3]{};for(float v:indices)if(v>=0&&v<=2)++counts[unsigned(v)];
        std::printf("orbital indices %u/%u/%u\n",counts[0],counts[1],counts[2]);check(counts[1]>0&&counts[2]>0,"orbital pixels carry their actual instance index");for(size_t i=0;i<indices.size();++i)if(indices[i]>0)check(std::fabs(orbitalDepth[i]-.025f)<1e-6f,"orbital HC preserves exact raster depth");for(float v:read(dev.Get(),ctx.Get(),scene.tex.Get()))check(v==0,"orbital coverage preserves live game depth");
        ID3D11ShaderResourceView* privateOrbital=nullptr;check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&privateOrbital)&&privateOrbital,"orbital publishes its private depth seed");ComPtr<ID3D11Resource> privateOrbitalResource;privateOrbital->GetResource(&privateOrbitalResource);
        for(float v:read(dev.Get(),ctx.Get(),privateOrbitalResource.Get()))check(v==0,"orbital coverage leaves the private scene-depth seed unchanged");
        // Orbital geometry has its own HC/record motion path.  It must not
        // enter the ordinary text cleanup mask: the temporal consumer gets
        // the record and exact coverage depth, while ui_resolve sees zero.
        for(float v:read(dev.Get(),ctx.Get(),g_mask[0].tex))
            check(v==0,"orbital coverage does not seed ordinary UI resolve");
        // A nearer physical hull in the seeded private DSV must prevent the
        // orbital HC record from being written, even though depth writes are
        // disabled for this coverage family.
        ctx->ClearState();uiDepthFrameBoundary(ctx.Get());ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,.05f,0);
        testScene=scene.tex.Get();bindOrbital();
        detail::g_uiDepthOn=true;detail::g_uiDepthMode=Mode::kReissueScene;g_reissueShader=&g_depthShaders[7];g_drawEye=0;g_reissueMaskSlot=0;g_rebindW=g_rebindH=8;g_wantMask=true;g_holoDraw={'N',4,2,0,0,0};
        check(uiDepthReissueBegin(ctx.Get()),"orbital coverage begins behind a nearer hull");
        const float hcSentinel[4]={91,.123f,0,0};ctx->ClearRenderTargetView(g_holoMotion[0].target(),hcSentinel);
        // The sentinel is cleared after Begin, then the depth-tested draw is
        // issued; a nearer hull must leave the existing HC record untouched.
        ctx->DrawInstanced(4,2,0,0);uiDepthReissueEnd(ctx.Get());
        views[0]=views[1]=nullptr;g_holoMotion[0].views(scene.tex.Get(),views);check(views[0]!=nullptr,"occluded orbital coverage resource remains available");ComPtr<ID3D11Resource> occludedOrbital;views[0]->GetResource(&occludedOrbital);auto occludedIndices=read(dev.Get(),ctx.Get(),occludedOrbital.Get()),occludedDepth=read(dev.Get(),ctx.Get(),occludedOrbital.Get(),1);
        for(float v:occludedIndices)check(v==91,"nearer physical hull preserves the prior HC record");
        for(float v:occludedDepth)check(std::fabs(v-.123f)<1e-6f,"nearer physical hull preserves the prior HC depth");
        for(float v:read(dev.Get(),ctx.Get(),scene.tex.Get()))check(v==.05f,"orbital coverage never writes the original scene depth");
        // Put the second line behind the first with exactly the same
        // projected footprint. The original draw blends in instance order
        // without writing depth, so the later visible line owns HC.
        ctx->ClearState();uiDepthFrameBoundary(ctx.Get());
        std::memcpy(instances+15,instances,15*sizeof(float));
        instances[16]*=2;instances[17]*=2;
        instances[23]*=2;instances[24]*=2;instances[25]*=2;
        ctx->UpdateSubresource(vb1.Get(),0,nullptr,instances,0,0);
        ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,.00625f,0);bindOrbital();
        detail::g_uiDepthOn=true;detail::g_uiDepthMode=Mode::kReissueScene;g_reissueShader=&g_depthShaders[7];g_drawEye=0;g_reissueMaskSlot=0;g_rebindW=g_rebindH=8;g_wantMask=true;g_holoDraw={'N',4,2,0,0,0};
        check(uiDepthReissueBegin(ctx.Get()),"overlapping orbital coverage begins over farther physical depth");ctx->DrawInstanced(4,2,0,0);uiDepthReissueEnd(ctx.Get());
        views[0]=views[1]=nullptr;g_holoMotion[0].views(scene.tex.Get(),views);ComPtr<ID3D11Resource> overlapCoverage;views[0]->GetResource(&overlapCoverage);
        auto overlapIndices=read(dev.Get(),ctx.Get(),overlapCoverage.Get()),overlapDepth=read(dev.Get(),ctx.Get(),overlapCoverage.Get(),1);unsigned overlapCount=0;
        for(size_t i=0;i<overlapIndices.size();++i)if(overlapIndices[i]>0){++overlapCount;check(std::fabs(overlapDepth[i]-.0125f)<1e-6f,"later overlapping orbital line retains its own motion depth");}
        check(overlapCount==counts[1],"overlapping lines draw the original single-line footprint");
        privateOrbital=nullptr;check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&privateOrbital)&&privateOrbital,"overlapping orbitals publish private depth");privateOrbitalResource.Reset();privateOrbital->GetResource(&privateOrbitalResource);
        for(float v:read(dev.Get(),ctx.Get(),privateOrbitalResource.Get()))check(v==.00625f,"overlapping orbital coverage preserves finite physical depth");
        for(float v:read(dev.Get(),ctx.Get(),scene.tex.Get()))check(v==.00625f,"overlapping orbital coverage preserves the game's finite depth");
        ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
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
        reset();mask.assign(64,1);for(int i=0;i<64;++i)old[i*4+3]=255;stroke(3,3,false);
        reactivity=run();check(reactivity[27]>.999f,"erased glyph on marked dark panel rejects its own history despite matching black neighbours");
        reset();stroke(3,3,true);reactivity=run();check(reactivity[27]>.999f,"appearing UI is fresh");
        reset();stroke(3,3,false);reactivity=run();check(reactivity[27]>.999f,"erased isolated stroke clears its history");
        check(reactivity[35]<.001f,"empty history does not extend rejection beyond reprojected coverage");
        check(reactivity[36]<.001f,"empty diagonal history stays unchanged");
        check(reactivity[63]<.001f,"unrelated sky retains history");
        reset();stroke(4,3,true);stroke(3,3,false);
        reactivity=run();check(reactivity[28]>.999f,"new stroke without aligned raster history cannot borrow an unrelated neighbour");
        params.offset[0]=-1;
        reactivity=run();check(reactivity[28]<.001f,"motion aligns UI evidence");
        reset();stroke(3,3,true);stroke(3,3,false);
        for(int i=0;i<64;++i)if(!mask[i])now[i*4]=255;
        for(float v:run())check(v<.001f,"background change outside UI preserves UI history");
        reset();stroke(3,3,true);stroke(3,3,false);mask[27]=2;
        for(float v:run())check(v<.001f,"attached UI uses adaptation as well as floating UI");
        params.jit[0]=.49f;params.jit[1]=-.49f;
        reactivity=run();check(reactivity[27]<.001f,"raster evidence colour and coverage remain at the same pixel under jitter");
        reset();stroke(3,3,true);stroke(2,3,false);params.offset[0]=-.75f;
        reactivity=run();check(reactivity[27]<.001f,"fractional history normalizes coverage without mixing unmarked background into UI colour");
        params.offset[0]=20;reactivity=run();check(reactivity[27]>.999f,"offscreen history cannot blur current UI");
        reset();stroke(3,3,true);mask[27]=3;
        for(float v:run())check(v<.001f,"smoke coverage cannot trigger adaptive UI rejection");
        reset();stroke(3,3,true);mask[27]=255;
        for(float v:run())check(v<.001f,"reactive smoke is distinct from UI at every bias strength");
        reset();stroke(3,3,true);params.probe[2]=0;
        for(float v:run())check(v<.001f,"unbound coverage cannot mark scene content as UI");
        std::puts("PASS: production adaptive UI shader keeps stable strokes and rejects changed, new and erased UI.");
    }
    // Actual source edits, independent of projection, jitter and both eyes.
    {
        ctx->ClearState();
        auto make=[&](UINT w,UINT h,DXGI_FORMAT f=DXGI_FORMAT_R8G8B8A8_UNORM,UINT flags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET){
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=f;d.BindFlags=flags;
            ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&d,nullptr,&t));return t;
        };
        auto view=[&](ID3D11Texture2D* t){ComPtr<ID3D11ShaderResourceView> v;hr(dev->CreateShaderResourceView(t,nullptr,&v));return v;};
        auto valuesOf=[&](ID3D11ShaderResourceView* v){check(v!=nullptr,"UI edit view available");ComPtr<ID3D11Resource> r;v->GetResource(&r);return read(dev.Get(),ctx.Get(),r.Get());};
        for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM}) {
            TestUiContent tracker;auto t=make(13,9,format);auto v=view(t.Get());
            std::vector<unsigned char> bytes(13*9*4,0);
            bytes[4*55]=128;bytes[4*55+3]=124;
            ctx->UpdateSubresource(t.Get(),0,nullptr,bytes.data(),13*4,0);
            for(float a:valuesOf(tracker.prepare(ctx.Get(),v.Get(),100)))check(a==0,"new source starts with no fabricated edits");
            check(tracker.lastDecision==UiContent::Decision::kReset&&tracker.lastAge==0,"census: a brand new entry is a reset at age 0");
            for(float a:valuesOf(tracker.prepare(ctx.Get(),v.Get(),101)))check(a==0,"identical source stays stable across frames and SRV decoding");
            check(tracker.lastDecision==UiContent::Decision::kUpdated&&tracker.lastAge==1,"census: a consecutive frame is an update at age 1");
            // An alpha-only erasure and a newly visible dim stroke are real
            // edits. 13x9 at 4x4 blocks is a 4x3 grid (12 blocks); texel 55
            // (row 4, col 3) is block 4, texel 58 (row 4, col 6) is block 5
            // -- the only two the digest should mark changed. (Invisible-
            // RGB-behind-zero-alpha and isolation to a single block are
            // covered on their own below, at block-aligned coordinates.)
            bytes[4*55+3]=0;bytes[4*58]=8;bytes[4*58+3]=1;
            ctx->UpdateSubresource(t.Get(),0,nullptr,bytes.data(),13*4,0);
            auto changed=valuesOf(tracker.prepare(ctx.Get(),v.Get(),102));
            check(tracker.lastDecision==UiContent::Decision::kUpdated&&tracker.lastAge==1,"census: still updating one frame later");
            check(changed.size()==12,"13x9 at 4x4 blocks is a 4x3 grid");
            for(unsigned i=0;i<changed.size();++i)check(changed[i]==(i==4||i==5?1.f:0.f),"source edit footprint includes erased/dim strokes' blocks only");
            auto* same=tracker.prepare(ctx.Get(),v.Get(),102);
            check(tracker.lastDecision==UiContent::Decision::kHit&&tracker.lastAge==0,"census: a repeated same-frame call is a hit");
            check(tracker.totals.updates==2 && tracker.totals.hits==1,"both eyes and repeated meshes compare once per frame");
            check(valuesOf(same)==changed,"second eye observes the same edit age");
            for(unsigned f=103;f<=134;++f)changed=valuesOf(tracker.prepare(ctx.Get(),v.Get(),f));
            for(float a:changed)check(a==0,"unchanged edits expire after 32 frames");
            bytes[4*55+3]=123;ctx->UpdateSubresource(t.Get(),0,nullptr,bytes.data(),13*4,0);
            check(valuesOf(tracker.prepare(ctx.Get(),v.Get(),135))[4]==1,"later change rearms edit history");
            GpuTimer outer;
            check(outer.begin(dev.Get(),ctx.Get()),"outer interval surrounds production UI sample");
            auto* resetFrame=tracker.prepare(ctx.Get(),v.Get(),137,true);
            check(outer.end(ctx.Get()),"outer interval ends after UI sample");
            check(tracker.lastDecision==UiContent::Decision::kReset&&tracker.lastAge==2,"census: a frame gap on an existing entry is a reset at its real age");
            for(float a:valuesOf(resetFrame))check(a==0,"skipped surface frame resets history");
            double outerMs=0;GpuTimerPoll outerStatus=GpuTimerPoll::Pending;
            const auto deadline=GetTickCount64()+1500;
            do {
                tracker.gpu.poll(ctx.Get());
                if(outerStatus==GpuTimerPoll::Pending)outerStatus=outer.poll(ctx.Get(),outerMs);
                if(tracker.gpu.totals.samples && outerStatus!=GpuTimerPoll::Pending)break;
                Sleep(1);
            } while(GetTickCount64()<deadline);
            check(tracker.gpu.totals.samples==1 && !tracker.gpu.totals.invalid && outerStatus==GpuTimerPoll::Ready,
                  "production UI timer drains under an outer shared interval");
            check(outerMs>=tracker.gpu.totals.ms,"UI timestamp pair stays inside parent interval");
            outer.reset(ctx.Get());
            tracker.retire(258);check(tracker.allocated==0,"idle source releases retained textures");
        }
        // Block granularity, isolated at an 8x8 surface (exactly a 2x2
        // block grid): a changed texel marks only its own block, an
        // unchanged frame decays that block by exactly 1/32, an
        // alpha-to-zero erasure is itself a detected change, and a reseed
        // (a frame gap, not just an unchanged frame) wipes a block that had
        // a real, recent, nonzero age back to nothing.
        {
            TestUiContent blocks;auto bt=make(8,8);auto bv=view(bt.Get());
            std::vector<unsigned char> px(8*8*4,0);
            ctx->UpdateSubresource(bt.Get(),0,nullptr,px.data(),8*4,0);
            auto changed=valuesOf(blocks.prepare(ctx.Get(),bv.Get(),1));
            check(changed.size()==4,"8x8 at 4x4 blocks is a 2x2 grid");
            for(float a:changed)check(a==0,"seed: a brand new entry marks nothing");
            // Texel (5,5) is block (1,1) -- row-major index 1*2+1=3.
            px[4*(5*8+5)]=200;px[4*(5*8+5)+3]=255;
            ctx->UpdateSubresource(bt.Get(),0,nullptr,px.data(),8*4,0);
            changed=valuesOf(blocks.prepare(ctx.Get(),bv.Get(),2));
            for(unsigned i=0;i<4;++i)check(changed[i]==(i==3?1.f:0.f),"one changed texel marks exactly its own 4x4 block, and no other");
            changed=valuesOf(blocks.prepare(ctx.Get(),bv.Get(),3));
            // age is R8_UNORM -- 256 levels, so the decayed value quantizes
            // to the nearest 1/255 rather than landing on 1-1/32 exactly.
            check(std::fabs(changed[3]-(1.0f-1.0f/32.0f))<1.0f/255.0f,"an unchanged frame decays its block's age by about 1/32");
            for(unsigned i=0;i<4;++i)if(i!=3)check(changed[i]==0,"blocks nothing ever touched stay at 0");
            px[4*(5*8+5)+3]=0; // erase: alpha to 0 is itself a content change
            ctx->UpdateSubresource(bt.Get(),0,nullptr,px.data(),8*4,0);
            changed=valuesOf(blocks.prepare(ctx.Get(),bv.Get(),4));
            check(changed[3]==1,"an alpha-to-zero erasure is detected like any other change");
            changed=valuesOf(blocks.prepare(ctx.Get(),bv.Get(),6)); // gap of 2: reseed, not compare
            for(float a:changed)check(a==0,"a seed -- a frame gap, here -- marks nothing, even a block with a real recent age");
        }
        // Budget arithmetic as a pure function: the 5895x5158 R8G8B8A8
        // panel behind issue 2026-09-24's ghosting digits (fix.ui_quality
        // 1.25 at HMD Quality 0.5) was 182 MB and permanently over budget
        // under the old per-texel formula; block digests fit it in about
        // 11 MB. No device or texture needed -- this is pure arithmetic on
        // the same bytesFor() prepare() itself calls.
        check(UiContent::bytesFor(5895,5158)<=UiContent::kBudget,"block budget: the 5895x5158 ghosting-digits panel now fits");
        check(UiContent::bytesFor(20000,20000)>UiContent::kBudget,"block budget: a large enough surface still declines");
        // Preserve the game's compute bindings, including a live UAV.
        auto t=make(13,9);auto v=view(t.Get());std::vector<unsigned char> bytes(13*9*4,127);
        ctx->UpdateSubresource(t.Get(),0,nullptr,bytes.data(),13*4,0);TestUiContent tracker;
        tracker.prepare(ctx.Get(),v.Get(),1);
        auto sentinelT=make(8,8,DXGI_FORMAT_R8_UNORM,D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11UnorderedAccessView> sentinelU;hr(dev->CreateUnorderedAccessView(sentinelT.Get(),nullptr,&sentinelU));
        auto code=compile("[numthreads(1,1,1)]void main(){}","cs_5_0");ComPtr<ID3D11ComputeShader> sentinelCs;
        hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&sentinelCs));
        ctx->CSSetShader(sentinelCs.Get(),nullptr,0);ctx->CSSetUnorderedAccessViews(0,1,sentinelU.GetAddressOf(),nullptr);
        for(UINT slot=0;slot<3;++slot)ctx->CSSetShaderResources(slot,1,v.GetAddressOf());
        tracker.prepare(ctx.Get(),v.Get(),2);
        ComPtr<ID3D11ComputeShader> afterCs;ctx->CSGetShader(&afterCs,nullptr,nullptr);check(afterCs==sentinelCs,"UI edit compute shader restored");
        ComPtr<ID3D11UnorderedAccessView> afterU;ctx->CSGetUnorderedAccessViews(0,1,&afterU);check(afterU==sentinelU,"UI edit compute UAV restored");
        for(UINT slot=0;slot<3;++slot){ComPtr<ID3D11ShaderResourceView> after;ctx->CSGetShaderResources(slot,1,&after);check(after==v,"UI edit compute input restored");}
        ctx->ClearState();
        TestUiContent bounded;std::vector<ComPtr<ID3D11Texture2D>> textures;std::vector<ComPtr<ID3D11ShaderResourceView>> views;
        for(unsigned i=0;i<25;++i){textures.push_back(make(4,4));views.push_back(view(textures.back().Get()));
            check((bounded.prepare(ctx.Get(),views.back().Get(),1)!=nullptr)==(i<24),"cache count bounded without evicting active-frame surfaces");}
        check(bounded.inUse()==24,"census: inUse counts exactly the filled table");
        check(bounded.lastDecision==UiContent::Decision::kDeclinedNoFreeEntry&&bounded.lastEvicted==0,
              "census: a full table with nothing evictable this frame declines with no eviction");
        check(bounded.prepare(ctx.Get(),views.back().Get(),2)!=nullptr && bounded.totals.evicted==1,"older cache entry can be evicted safely");
        check(bounded.lastDecision==UiContent::Decision::kReset&&bounded.lastAge==0&&bounded.lastEvicted==1,
              "census: the new entry that forced the eviction is a reset, one evicted this call");
        auto atlas=make(4,4,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);auto atlasV=view(atlas.Get());
        check(!bounded.prepare(ctx.Get(),atlasV.Get(),3),"static atlas is not copied each frame");
        check(bounded.lastDecision==UiContent::Decision::kDeclinedNoRenderTarget,"census: a SHADER_RESOURCE-only surface declines as no-render-target");
        // A 4096x1536 surface, big enough to matter under the old
        // per-texel budget, is under a megabyte of blocks now -- real
        // multi-entry byte pressure would need surfaces in the tens of
        // millions of texels each (the pure-function block above), so
        // there is no live-texture "byte cap before entry cap" case left
        // at a size this rig can allocate; UiContent::bytesFor's own
        // threshold arithmetic is what covers it.
        auto big=make(4096,1536);auto bigV=view(big.Get());
        TestUiContent budget;check(budget.prepare(ctx.Get(),bigV.Get(),1)!=nullptr,"bounded large UI surface supported");
        check(budget.allocated<=UiContent::kBudget,"allocated UI history fits budget");
        // The reason codes nothing above exercises: a null surface, an
        // unsupported format, a non-Texture2D resource, and an array view
        // (a single surface bigger than the whole budget is the pure
        // bytesFor() function above -- see its own comment for why not a
        // live texture).
        {
            TestUiContent reasons;
            check(reasons.prepare(ctx.Get(),nullptr,1)==nullptr&&reasons.lastDecision==UiContent::Decision::kDeclinedNoSurface,
                  "census: a null surface declines as no-surface");
            auto badFormat=make(4,4,DXGI_FORMAT_R32_FLOAT);auto badFormatV=view(badFormat.Get());
            check(reasons.prepare(ctx.Get(),badFormatV.Get(),1)==nullptr&&reasons.lastDecision==UiContent::Decision::kDeclinedFormat,
                  "census: an unsupported format declines as format");
            D3D11_BUFFER_DESC bufDesc{};bufDesc.ByteWidth=256;bufDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Buffer> buf;hr(dev->CreateBuffer(&bufDesc,nullptr,&buf));
            D3D11_SHADER_RESOURCE_VIEW_DESC bufSd{};bufSd.Format=DXGI_FORMAT_R32_FLOAT;
            bufSd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;bufSd.Buffer.NumElements=64;
            ComPtr<ID3D11ShaderResourceView> bufV;hr(dev->CreateShaderResourceView(buf.Get(),&bufSd,&bufV));
            check(reasons.prepare(ctx.Get(),bufV.Get(),1)==nullptr&&reasons.lastDecision==UiContent::Decision::kDeclinedNotTexture2D,
                  "census: a non-Texture2D resource declines as not-Texture2D");
            D3D11_TEXTURE2D_DESC arrDesc{};arrDesc.Width=arrDesc.Height=4;arrDesc.MipLevels=1;arrDesc.ArraySize=2;arrDesc.SampleDesc.Count=1;
            arrDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;arrDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> arrTex;hr(dev->CreateTexture2D(&arrDesc,nullptr,&arrTex));
            D3D11_SHADER_RESOURCE_VIEW_DESC arrSd{};arrSd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            arrSd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;arrSd.Texture2DArray.MipLevels=1;arrSd.Texture2DArray.ArraySize=2;
            ComPtr<ID3D11ShaderResourceView> arrV;hr(dev->CreateShaderResourceView(arrTex.Get(),&arrSd,&arrV));
            check(reasons.prepare(ctx.Get(),arrV.Get(),1)==nullptr&&reasons.lastDecision==UiContent::Decision::kDeclinedViewShape,
                  "census: an array view declines as view-shape");
        }
        // End-to-end source -> existing coverage draw -> borrowed eye SRV.
        // t14 must be restored and erased glyphs must keep their edit mark
        // even though the current source alpha is zero.
        auto uiSurface=make(4,4);auto uiView=view(uiSurface.Get());std::vector<unsigned char> pixels(4*4*4,0);pixels[4*5+3]=124;
        ctx->UpdateSubresource(uiSurface.Get(),0,nullptr,pixels.data(),16,0);
        g_trained=true;
        for(unsigned f=0;f<2;++f){
            uiDepthFrameBoundary(ctx.Get());ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
            bind(scene.dsv.Get());setZ(.6f);ctx->PSSetShaderResources(0,1,uiView.GetAddressOf());ctx->PSSetShaderResources(14,1,v.GetAddressOf());
            coverage(false,true);
            ComPtr<ID3D11ShaderResourceView> after;ctx->PSGetShaderResources(14,1,&after);check(after==v,"source edit PS binding restored");
            auto edit=valuesOf(uiDepthContentChanges(8,8,0));
            check(!uiDepthContentChanges(8,8,1) && !uiDepthContentChanges(7,8,0),"source edits respect eye and render dimensions");
            if(f==0)for(float a:edit)check(a==0,"unchanged first UI frame has no projected edits");
            else {
                check(edit[3*8+3]==1,"erased glyph edit survives the source alpha discard");
                for(float z:read(dev.Get(),ctx.Get(),scene.tex.Get()))check(z==0,"erased UI never writes game depth");
            }
            pixels.assign(4*4*4,0);ctx->UpdateSubresource(uiSurface.Get(),0,nullptr,pixels.data(),16,0);
        }
        ctx->ClearState();uiDepthFrameBoundary(ctx.Get());check(!uiDepthContentChanges(8,8,0),"projected edit mask clears after both eyes submit");g_trained=false;
        // The census gate itself: g_uiContentCensusOn should latch true for
        // exactly the frame after objectProbeLedgerActive() is first seen
        // active, print its summary and drop again at the next boundary,
        // and never re-arm while the same (possibly many-frame) run stays
        // active -- the rising edge ui_depth.cpp's own comment describes.
        detail::g_uiDepthOn=true;detail::g_uiDepthStoodDown=false;
        detail::g_objectProbeLedgerOn=false;uiDepthFrameBoundary(ctx.Get());
        check(!g_uiContentCensusOn,"census: idle while no eye run is armed");
        detail::g_objectProbeLedgerOn=true;uiDepthFrameBoundary(ctx.Get());
        check(g_uiContentCensusOn,"census: the frame after the ledger arms is armed for logging");
        uiDepthFrameBoundary(ctx.Get());
        check(!g_uiContentCensusOn,"census: a second frame of the same still-armed run is not re-armed");
        uiDepthFrameBoundary(ctx.Get());
        check(!g_uiContentCensusOn,"census: stays idle for the rest of a long run");
        detail::g_objectProbeLedgerOn=false;uiDepthFrameBoundary(ctx.Get());
        detail::g_objectProbeLedgerOn=true;uiDepthFrameBoundary(ctx.Get());
        check(g_uiContentCensusOn,"census: a later, separate run re-arms exactly the same way");
        detail::g_objectProbeLedgerOn=false;uiDepthFrameBoundary(ctx.Get());
        check(!g_uiContentCensusOn&&!detail::g_objectProbeLedgerOn,"census: settles idle again with the run closed");
        // Scrolling sprite ticks must not retain depth or edit footprints
        // where their source has become transparent. Dim current strokes
        // still carry coverage, and visible changed pixels still reject
        // stale text. Exercise the actual reissue with source tracking.
        pixels.assign(4*4*4,0);pixels[4*5+3]=29;
        ctx->UpdateSubresource(uiSurface.Get(),0,nullptr,pixels.data(),16,0);g_trained=true;
        for(unsigned f=0;f<3;++f) {
            uiDepthFrameBoundary(ctx.Get());ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
            bind(scene.dsv.Get());setZ(.6f);ctx->PSSetShaderResources(0,1,uiView.GetAddressOf());
            detail::g_uiDepthOn=true;g_reactive=0;detail::g_uiDepthMode=Mode::kReissueScene;g_reissueShader=&g_depthShaders[5];g_drawEye=0;g_reissueMaskSlot=1;
            g_rebindW=g_rebindH=8;g_wantMask=true;g_wantRebind=false;
            check(uiDepthReissueBegin(ctx.Get()),"scrolling sprite coverage begins");ctx->Draw(3,0);uiDepthReissueEnd(ctx.Get());ctx->OMSetRenderTargets(0,nullptr,nullptr);
            auto mark=valuesOf(g_mask[0].srv),edit=valuesOf(uiDepthContentChanges(8,8,0));
            if(f<2)check(mark[3*8+3]>0,"faint sprite stroke retains motion and antialiasing coverage");
            if(f==1)check(edit[3*8+3]>0,"changed visible sprite retains fresh reconstruction");
            if(f==2) {
                for(float a:mark)check(a==0,"erased scrolling tick leaves no rectangle of sprite coverage");
                for(float a:edit)check(a==0,"erased tick cannot force spatial reconstruction over terrain");
                check(uiDepthTemporalDepth(8,8,0,scene.tex.Get(),&ui),"sprite private depth available after erasure");ui->GetResource(privateRes.ReleaseAndGetAddressOf());
                for(float z:read(dev.Get(),ctx.Get(),privateRes.Get()))check(z==0,"erased tick leaves private scene depth intact");
            }
            pixels[4*5+3]=f==0?77:0;ctx->UpdateSubresource(uiSurface.Get(),0,nullptr,pixels.data(),16,0);
        }
        ctx->ClearState();uiDepthFrameBoundary(ctx.Get());g_trained=false;
        std::puts("PASS: UI source edits preserve stable/dim text, detect erasure, expire, restore state, and respect eye/cache/byte bounds.");
    }
    // Exercise the model-independent resolve itself. Odd dimensions and
    // noninteger output ratios catch holes/overlap in block ownership.
    {
        ctx->ClearState();
        auto code=compile(kUiResolve,"cs_5_0");ComPtr<ID3D11ComputeShader> cs;
        hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs));
        auto texture=[&](UINT w,UINT h,DXGI_FORMAT f){
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=f;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&d,nullptr,&t));return t;
        };
        auto srv=[&](ID3D11Texture2D* t){ComPtr<ID3D11ShaderResourceView> v;hr(dev->CreateShaderResourceView(t,nullptr,&v));return v;};
        auto uav=[&](ID3D11Texture2D* t){ComPtr<ID3D11UnorderedAccessView> v;hr(dev->CreateUnorderedAccessView(t,nullptr,&v));return v;};
        constexpr UINT w=13,h=9;
        auto raw=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM),mask=texture(w,h,DXGI_FORMAT_R8_UNORM),edits=texture(w,h,DXGI_FORMAT_R8_UNORM);
        auto editsV=srv(edits.Get());
        auto screen=texture(w+4,h+6,DXGI_FORMAT_R32G32B32A32_FLOAT);auto screenV=srv(screen.Get());
        auto previous=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM),next=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto velocity=texture(w,h,DXGI_FORMAT_R32G32_FLOAT);auto velocityV=srv(velocity.Get());
        auto rawV=srv(raw.Get()),maskV=srv(mask.Get()),previousV=srv(previous.Get());auto nextU=uav(next.Get());
        struct Params {int region[4]={0,0,w,h};int size[2]={w,h};int texSize[2]={w,h};float tn[4]={},tp[4]={},jit[4]={};} p;
        D3D11_BUFFER_DESC resolveDesc{};resolveDesc.ByteWidth=sizeof(p);resolveDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb;hr(dev->CreateBuffer(&resolveDesc,nullptr,&cb));
        // b1: the clamp's bound tolerance. Bound only when a check passes a
        // nonzero tolerance to run(); every existing call leaves it unbound,
        // which the shader reads as zero -- the old exact clamp.
        D3D11_BUFFER_DESC tolDesc{};tolDesc.ByteWidth=16;tolDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> tolCb;hr(dev->CreateBuffer(&tolDesc,nullptr,&tolCb));
        for(auto dims:{std::pair{13u,9u},std::pair{26u,18u},std::pair{21u,14u},std::pair{7u,5u}}){
            const UINT ow=dims.first,oh=dims.second;
            auto trained=texture(ow,oh,DXGI_FORMAT_R8G8B8A8_UNORM),output=texture(ow,oh,DXGI_FORMAT_R8G8B8A8_UNORM);
            auto trainedV=srv(trained.Get());auto outputU=uav(output.Get());
            std::vector<unsigned char> colour(w*h*4,0),mark(w*h,0),old(w*h*4,0),model(ow*oh*4,0);
            std::vector<float> motion(w*h*2,0);
            std::vector<unsigned char> edit(w*h,0);
            std::vector<float> screenPixels((w+4)*(h+6)*4,0);
            for(UINT i=0;i<ow*oh;++i){model[4*i]=static_cast<unsigned char>(64+i%100);model[4*i+3]=127;}
            auto run=[&](bool haveHistory,float tolerance=0.0f,float hold=0.0f){
                ctx->UpdateSubresource(raw.Get(),0,nullptr,colour.data(),w*4,0);ctx->UpdateSubresource(mask.Get(),0,nullptr,mark.data(),w,0);
                ctx->UpdateSubresource(previous.Get(),0,nullptr,old.data(),w*4,0);ctx->UpdateSubresource(trained.Get(),0,nullptr,model.data(),ow*4,0);
                ctx->UpdateSubresource(cb.Get(),0,nullptr,&p,0,0);
                ctx->UpdateSubresource(velocity.Get(),0,nullptr,motion.data(),w*8,0);
                ctx->UpdateSubresource(edits.Get(),0,nullptr,edit.data(),w,0);
                ctx->UpdateSubresource(screen.Get(),0,nullptr,screenPixels.data(),(w+4)*16,0);
                ID3D11ShaderResourceView* in[]={rawV.Get(),trainedV.Get(),maskV.Get(),haveHistory?previousV.Get():nullptr,velocityV.Get(),editsV.Get(),screenV.Get()};
                ID3D11UnorderedAccessView* out[]={outputU.Get(),nextU.Get()};
                const float poison[4]={1,1,1,1};ctx->ClearUnorderedAccessViewFloat(outputU.Get(),poison);
                ctx->CSSetShader(cs.Get(),nullptr,0);ctx->CSSetShaderResources(0,7,in);ctx->CSSetUnorderedAccessViews(0,2,out,nullptr);
                ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
                if(tolerance!=0.0f||hold!=0.0f){
                    const float t[4]={tolerance,hold,0,0};ctx->UpdateSubresource(tolCb.Get(),0,nullptr,t,0,0);
                    ctx->CSSetConstantBuffers(1,1,tolCb.GetAddressOf());
                }
                ctx->Dispatch((w+7)/8,(h+7)/8,1);ctx->ClearState();
                return read(dev.Get(),ctx.Get(),output.Get());
            };
            auto resolvedValues=run(false);
            for(UINT i=0;i<ow*oh;++i)check(std::fabs(resolvedValues[i]-model[4*i]/255.f)<1e-6,"UI resolve copies every world output pixel unchanged");
            for(float a:read(dev.Get(),ctx.Get(),output.Get(),3))check(std::fabs(a-127/255.f)<1e-6,"UI resolve preserves submission alpha");
            for(float a:read(dev.Get(),ctx.Get(),next.Get(),3))check(a==0,"unmarked scene cannot grow UI influence");
            // An exposed world pixel carries last frame's UI six pixels
            // sideways and four down. Neither current coverage nor the old
            // screen position includes the new trail (104513 RIKEN dump).
            old[4*(2*w+2)+3]=255;
            motion[2*(6*w+8)]=-6;motion[2*(6*w+8)+1]=-4;
            resolvedValues=run(true);
            for(UINT y=0;y<oh;++y)for(UINT x=0;x<ow;++x){
                const UINT qx=x*w/ow,qy=y*h/oh;
                const bool owned=(qx==2&&qy==2)||(qx==8&&qy==6);
                check(owned?resolvedValues[y*ow+x]==0:std::fabs(resolvedValues[y*ow+x]-model[4*(y*ow+x)]/255.f)<1e-6,"transported UI trail is clipped while unrelated world output stays identical");
            }
            // A fractional reprojection must attenuate transported support.
            // With the old four-tap max, even a .001-pixel vector selected a
            // full-alpha neighbour, making stale colour grow one input cell
            // per frame.  Use a stale model over a black raw frame so that
            // this is observable as a false dark pixel, then feed Next back
            // into Previous for the following cells and directions.
            if(ow==w&&oh==h) {
                struct TinyTransport {float dx,dy;UINT firstX,firstY,farX,farY;};
                const TinyTransport tiny[]={
                    {.001f,0,4,4,3,4},{-.001f,0,8,4,9,4},
                    {0,.001f,6,2,6,1},{0,-.001f,6,6,6,7},
                    {.001f,.001f,4,2,3,1},{-.001f,-.001f,8,6,9,7}};
                for(const auto& t:tiny) {
                    colour.assign(w*h*4,0);old.assign(w*h*4,0);mark.assign(w*h,0);motion.assign(w*h*2,0);
                    for(UINT i=0;i<ow*oh;++i)model[4*i]=220;
                    mark[4*w+6]=1;
                    run(false);auto tinySeed=read(dev.Get(),ctx.Get(),next.Get(),3);
                    mark.assign(w*h,0);for(UINT i=0;i<w*h;++i)old[4*i+3]=static_cast<unsigned char>(std::lround(tinySeed[i]*255.0f));
                    for(UINT i=0;i<w*h;++i){motion[2*i]=t.dx;motion[2*i+1]=t.dy;}
                    for(unsigned frame=0;frame<3;++frame) {
                        resolvedValues=run(true);auto tinyInfluence=read(dev.Get(),ctx.Get(),next.Get(),3);
                        const UINT first=t.firstY*w+t.firstX,farCell=t.farY*w+t.farX;
                        check(tinyInfluence[first]==0,"fractional transport leaves the first off-glyph cell unowned");
                        // The current dispatch may still protect this cell before
                        // its fractional age reaches zero; only the next-frame
                        // influence is the transport contract here.
                        if(frame==2) {
                            check(tinyInfluence[farCell]==0,"tiny motion does not grow a one-cell-per-frame history halo");
                            check(std::fabs(resolvedValues[farCell]-220/255.f)<1e-6,"tiny-motion halo does not darken a farther background cell");
                        }
                        for(UINT i=0;i<w*h;++i)old[4*i+3]=static_cast<unsigned char>(std::lround(tinyInfluence[i]*255.0f));
                    }
                }
                // A half-pixel vector keeps only half of the departed support;
                // the exact integer path below remains a full transported cell.
                colour.assign(w*h*4,0);old.assign(w*h*4,0);mark.assign(w*h,0);motion.assign(w*h*2,0);for(UINT i=0;i<w*h;++i)motion[2*i]=.5f;
                mark[4*w+6]=1;run(false);auto halfSeed=read(dev.Get(),ctx.Get(),next.Get(),3);
                mark.assign(w*h,0);for(UINT i=0;i<w*h;++i)old[4*i+3]=static_cast<unsigned char>(std::lround(halfSeed[i]*255.0f));
                resolvedValues=run(true);auto halfInfluence=read(dev.Get(),ctx.Get(),next.Get(),3);
                check(halfInfluence[4*w+4]>.4f&&halfInfluence[4*w+4]<.55f,"bilinear fractional history retains partial age");
                check(resolvedValues[4*w+4]==0,"partial history still protects the stale glyph footprint");
            }
            // Widened clamp tolerance (issue 36): NVIDIA's own DLSS output can
            // leave the raw 2x2 bound by a few steps on the star corona; a
            // tolerance widens the bound so an ordinary offset like that
            // survives, while a real departed glyph, tens of steps away, is
            // still pulled back to within the tolerance and still marks the
            // footprint stale. jit is zero and ow==w/oh==h here, so input
            // texel and output pixel are the same address (the partial-
            // history check above reads the same cell).
            if(ow==w&&oh==h) {
                const UINT cell=4*w+4;
                colour.assign(w*h*4,100);old.assign(w*h*4,0);mark.assign(w*h,0);
                motion.assign(w*h*2,0);edit.assign(w*h,0);
                old[4*cell+3]=128;                  // previous alpha 0.5: a live footprint
                model.assign(ow*oh*4,105);          // raw 100/255, model +5/255 over it
                resolvedValues=run(true,8.0f/255.0f);
                auto expireAlpha=read(dev.Get(),ctx.Get(),next.Get(),3);
                check(std::fabs(resolvedValues[cell]-105/255.f)<1.0f/255.0f && expireAlpha[cell]==0,
                      "reconstruction offset inside the tolerance is left alone and the footprint expires");

                colour.assign(w*h*4,0);old.assign(w*h*4,0);old[4*cell+3]=128;
                model.assign(ow*oh*4,64);           // a departed glyph, tens of steps from raw
                resolvedValues=run(true,8.0f/255.0f);
                auto keptAlpha=read(dev.Get(),ctx.Get(),next.Get(),3);
                check(resolvedValues[cell]<=9/255.f && resolvedValues[cell]>=7/255.f &&
                      std::fabs(keptAlpha[cell]-(0.5f-1.0f/32.0f))<1.0f/255.0f,
                      "a ghost beyond the tolerance is clamped to it and keeps the footprint");

                resolvedValues=run(true,0.0f);
                check(resolvedValues[cell]==0,"tolerance zero is the exact clamp");
            }
            // The edited branch's own rebuild gate (issue 36): while a
            // label's distance readout ticks, its whole crop is marked
            // edited; rebuilding unconditionally pulled the label's
            // unchanged background down to the raw cubic. Gate it on the
            // same tolerance so only a genuine disagreement rebuilds.
            // With raw perfectly uniform, v.rgb (already bounded to within
            // the tolerance of raw) and fresh (which equals raw exactly at
            // zero jitter/1:1 scale, since the cubic collapses to a single
            // tap) can never disagree by more than the tolerance -- only
            // reach it exactly, which a strict '>' cannot reliably resolve
            // either way in floating point. Nudge the one 2x2 corner that
            // fresh's own tap does not read, so the bound v.rgb saturates
            // against is wider than what fresh reflects, giving the
            // disagreement in case (e) a clear, non-boundary margin.
            if(ow==w&&oh==h) {
                const UINT cell=4*w+4;
                colour.assign(w*h*4,100);old.assign(w*h*4,0);mark.assign(w*h,0);
                motion.assign(w*h*2,0);edit.assign(w*h,0);edit[cell]=255;
                colour[4*(5*w+5)+0]=116;colour[4*(5*w+5)+1]=116;colour[4*(5*w+5)+2]=116;
                model.assign(ow*oh*4,105);   // fresh (=raw at this texel, 100/255) agrees with the temporal value within tolerance
                resolvedValues=run(false,8.0f/255.0f);
                check(std::fabs(resolvedValues[cell]-105/255.f)<1.0f/255.0f,
                      "an edited texel keeps the temporal output where the fresh frame agrees within the tolerance");

                model.assign(ow*oh*4,160);   // the temporal value saturates the wider bound; fresh does not follow it
                resolvedValues=run(false,8.0f/255.0f);
                check(std::fabs(resolvedValues[cell]-100/255.f)<1.0f/255.0f,
                      "an edited texel is rebuilt where the fresh frame disagrees");
                // Restore the resting state (raw 0, nothing edited) that the
                // tests below assume without re-asserting it themselves.
                colour.assign(w*h*4,0);edit.assign(w*h,0);
            }
            // The corona-smear hold (advanced.corona_smear_level, issue 36): faint,
            // flat, non-UI pixels are pulled back within one step of the raw
            // 2x2 range, the way UI pixels already are above. jit is zero
            // and ow==w/oh==h here, so corner==centre==(ox,oy) at the cell,
            // as in the tolerance checks above.
            if(ow==w&&oh==h) {
                const UINT cell=4*w+4;
                // (f) A flat faint raw region: the hold pulls a trained
                // sample lifted above it back to within a step of raw.
                colour.assign(w*h*4,5);old.assign(w*h*4,0);mark.assign(w*h,0);
                motion.assign(w*h*2,0);edit.assign(w*h,0);
                model.assign(ow*oh*4,8);   // raw 5/255, trained lifted to 8/255
                resolvedValues=run(true,0.0f,64.0f/255.0f);
                check(std::fabs(resolvedValues[cell]-5.0f/255.0f)<=1.0f/255.0f+1e-4f,
                      "a flat faint region is held within a step of its raw level");

                // (g) Hold zero is bit-exact old behaviour: no hold at all.
                resolvedValues=run(true,0.0f,0.0f);
                check(std::fabs(resolvedValues[cell]-8.0f/255.0f)<1e-6f,
                      "hold zero leaves the trained output exactly as it was");

                // (h) A flat BRIGHT region sits above the brightness limit:
                // untouched, however far its trained sample lifts above it.
                colour.assign(w*h*4,200);model.assign(ow*oh*4,203);
                resolvedValues=run(true,0.0f,64.0f/255.0f);
                check(std::fabs(resolvedValues[cell]-203.0f/255.0f)<1e-6f,
                      "a bright flat region above the brightness limit is left untouched");

                // (i) A textured faint region: a checker of 5/255 and 40/255
                // gives the 3x3 window a 35-step spread, past the 16-step
                // texture cutoff, so a lifted trained sample is untouched.
                for(UINT i=0;i<w*h;++i){
                    const UINT x=i%w,y=i/w;
                    const unsigned char v=((x+y)%2==0)?5:40;
                    colour[4*i]=colour[4*i+1]=colour[4*i+2]=v;colour[4*i+3]=0;
                }
                model.assign(ow*oh*4,8);   // raw at the cell is 5/255 (even), trained = raw+3
                resolvedValues=run(true,0.0f,64.0f/255.0f);
                check(std::fabs(resolvedValues[cell]-8.0f/255.0f)<1e-6f,
                      "a textured neighbourhood past the texture cutoff is left untouched by the hold");

                // (j) A marked UI pixel keeps the ghost-tolerance rule; the
                // hold never applies where active is true.
                colour.assign(w*h*4,5);model.assign(ow*oh*4,8);mark[cell]=1;
                resolvedValues=run(true,12.0f/255.0f,64.0f/255.0f);
                check(std::fabs(resolvedValues[cell]-8.0f/255.0f)<1.0f/255.0f,
                      "a marked UI pixel keeps the ghost-tolerance rule, not the corona hold");

                // Restore the resting state (raw 0, nothing marked) that the
                // tests below assume without re-asserting it themselves.
                colour.assign(w*h*4,0);mark.assign(w*h,0);
            }
            // Invalid/off-screen motion cannot smear border UI inward.
            old.assign(w*h*4,0);motion.assign(w*h*2,0);mark.assign(w*h,0);
            old[4*(2*w+2)+3]=255;motion[2*(6*w+8)]=-1000;motion[2*(6*w+8)+1]=-4;resolvedValues=run(true);
            for(UINT y=0;y<oh;++y)for(UINT x=0;x<ow;++x)
                if(x*w/ow==8&&y*h/oh==6)check(std::fabs(resolvedValues[y*ow+x]-model[4*(y*ow+x)]/255.f)<1e-6,"off-screen transported history is rejected");
            old.assign(w*h*4,0);motion.assign(w*h*2,0);
            mark.assign(w*h,3);resolvedValues=run(false);
            for(UINT i=0;i<ow*oh;++i)check(std::fabs(resolvedValues[i]-model[4*i]/255.f)<1e-6,"smoke is excluded from UI resolve");
            mark.assign(w*h,1);resolvedValues=run(false);
            for(float v:resolvedValues)check(v==0,"dark marked panel rejects obsolete bright text after DLSS");
            for(float a:read(dev.Get(),ctx.Get(),next.Get(),3))check(a==1,"visible panel retains UI influence");
            mark.assign(w*h,2);resolvedValues=run(false);
            for(float v:resolvedValues)check(v==0,"attached marked panel rejects obsolete bright text after DLSS");
            for(float a:read(dev.Get(),ctx.Get(),next.Get(),3))check(a==1,"attached panel retains UI influence");
            // A text mark seeds carried influence when it disappears.  This
            // is the control that the orbital zero-mask case must avoid.
            mark.assign(w*h,1);old.assign(w*h*4,0);motion.assign(w*h*2,0);
            for(UINT i=0;i<ow*oh;++i)model[4*i]=220;
            resolvedValues=run(false);auto seeded=read(dev.Get(),ctx.Get(),next.Get(),3);
            check(std::any_of(seeded.begin(),seeded.end(),[](float a){return a>0;}),"text mark seeds retained UI influence");
            mark.assign(w*h,0);for(UINT i=0;i<w*h;++i)old[4*i+3]=static_cast<unsigned char>(std::lround(seeded[i]*255.0f));
            for(UINT i=0;i<ow*oh;++i)model[4*i]=32;
            resolvedValues=run(true);auto carried=read(dev.Get(),ctx.Get(),next.Get(),3);
            check(std::any_of(carried.begin(),carried.end(),[](float a){return a>0;}),"departing text influence carries into the next resolve frame");
            // Mode-2 orbital records may still carry HC/depth and motion,
            // but their zero UI mask must not seed or enlarge that footprint.
            mark.assign(w*h,0);old.assign(w*h*4,0);for(size_t i=0;i<motion.size();i+=2){motion[i]=-1.0f;motion[i+1]=.25f;}
            for(unsigned frame=0;frame<4;++frame) {
                for(UINT i=0;i<ow*oh;++i)model[4*i]=static_cast<unsigned char>(32+frame*31+(i%17));
                resolvedValues=run(frame!=0);
                check(std::fabs(resolvedValues[0]-model[0]/255.f)<1e-6,"mark-zero orbital resolve preserves current trained colour");
                auto orbitalInfluence=read(dev.Get(),ctx.Get(),next.Get(),3);
                check(std::none_of(orbitalInfluence.begin(),orbitalInfluence.end(),[](float a){return a>0;}),"mark-zero orbital resolve never grows UI influence");
                for(UINT i=0;i<w*h;++i)old[4*i+3]=static_cast<unsigned char>(std::lround(orbitalInfluence[i]*255.0f));
            }
            std::fill(motion.begin(),motion.end(),0.0f);mark.assign(w*h,0);for(UINT i=0;i<w*h;++i)old[4*i+3]=255;resolvedValues=run(true);
            for(float v:resolvedValues)check(v==0,"erased UI rejects a model trail after raw coverage disappears");
            auto influence=read(dev.Get(),ctx.Get(),next.Get(),3);
            for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
                bool owns=((x*ow+w-1)/w)<(((x+1)*ow+w-1)/w)&&((y*oh+h-1)/h)<(((y+1)*oh+h-1)/h);
                check(owns?(influence[y*w+x]>0&&influence[y*w+x]<1):influence[y*w+x]==0,"departing influence decays while its output block contains stale colour");
            }
            for(UINT i=0;i<w*h;++i)old[4*i+3]=1;run(true);
            for(float a:read(dev.Get(),ctx.Get(),next.Get(),3))check(a==0,"old UI influence expires even if world detail continues to differ");
            model.assign(ow*oh*4,0);run(true);
            for(float a:read(dev.Get(),ctx.Get(),next.Get(),3))check(a==0,"completed model trail releases influence");
            // Retain antialiasing inside the current colour range; a clip is
            // not a replacement of trained detail with the raw input pixel.
            mark.assign(w*h,2);for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)colour[4*(y*w+x)]=(x&1)?255:0;
            for(UINT i=0;i<ow*oh;++i)model[4*i]=128;
            p.jit[0]=.49f;p.jit[1]=-.49f;resolvedValues=run(false);
            for(UINT y=1;y+1<oh;++y)for(UINT x=2;x+2<ow;++x)check(std::fabs(resolvedValues[y*ow+x]-128/255.f)<1e-6,"valid subpixel UI colour retains trained antialiasing under jitter");
            p.jit[0]=p.jit[1]=0;
            // Two obsolete model images both fit within the raw bounds.
            // A real edit must produce the same fresh samples for either,
            // without changing the static text elsewhere in the frame.
            edit[4*w+6]=255;
            for(UINT i=0;i<ow*oh;++i)model[4*i+3]=127;
            for(UINT i=0;i<ow*oh;++i)model[4*i]=80;
            auto a=run(false);
            for(UINT i=0;i<ow*oh;++i)model[4*i]=170;
            auto b=run(false);unsigned changed=0,stable=0;
            for(UINT y=1;y+1<oh;++y)for(UINT x=2;x+2<ow;++x){
                const UINT qx=x*w/ow,qy=y*h/oh;
                if(qx>=5&&qx<=7&&qy>=3&&qy<=5){check(a[y*ow+x]==b[y*ow+x],"changed digit does not inherit model history even inside valid colour bounds");++changed;}
                else {check(a[y*ow+x]!=b[y*ow+x],"static UI retains model antialiasing beside changed text");++stable;}
            }
            check(changed && stable,"dynamic/static resolve controls both exercised at every output ratio");
            edit.assign(w*h,0);mark.assign(w*h,0);screenPixels[4*((4+3)*(w+4)+6+2)+3]=3;
            p.region[0]=2;p.region[1]=3;
            for(UINT i=0;i<ow*oh;++i)model[4*i]=80;
            auto screenA=run(false);
            for(UINT i=0;i<ow*oh;++i)model[4*i]=170;
            auto screenB=run(false);
            for(UINT y=1;y+1<oh;++y)for(UINT x=2;x+2<ow;++x){
                const UINT qx=x*w/ow,qy=y*h/oh;const bool sourceUi=qx>=5&&qx<=7&&qy>=3&&qy<=5;
                check(sourceUi?screenA[y*ow+x]==screenB[y*ow+x]:screenA[y*ow+x]!=screenB[y*ow+x],"source UI resolves fresh without eye coverage while world keeps trained detail");
            }
            for(float alpha:read(dev.Get(),ctx.Get(),output.Get(),3))check(std::fabs(alpha-127/255.f)<1e-6,"dynamic reconstruction preserves submission alpha");
            p.region[0]=p.region[1]=0;
        }
        std::puts("PASS: post-DLSS UI resolve bounds stale colour, retains AA/alpha, excludes world/smoke, and covers noninteger output sizes.");
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
    testPlanetCoverage(dev.Get(),ctx.Get());
    testCoronaCoverage(dev.Get(),ctx.Get());
    if(info) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<unsigned char> storage(size);
        auto* m=reinterpret_cast<D3D11_MESSAGE*>(storage.data()); hr(info->GetMessage(i,m,&size));
        if(m->ID == D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET) continue;
        if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
            std::puts(m->pDescription); check(false,"D3D debug-layer warning/error");
        }
    }
    ctx->ClearState(); uiDepthShutdown();
    check(gpuTimingShutdown(ctx.Get()),"explicit shared timer shutdown before WARP release");
    std::printf("PASS: %d checks; production UI coverage isolates smoke, preserves depth/alpha/occlusion/state, and handles menus and frame/eye/format changes.\n",checks);
}
