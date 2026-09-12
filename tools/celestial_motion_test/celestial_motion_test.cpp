// WARP regression of the production terrain history, rasterised coverage
// and temporal consumer. No game/runtime or synchronous render-path readback.
#include "../../src/d3d11/celestial_motion.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <algorithm>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool ok,const char* why) { ++checks; if (!ok) { std::printf("FAIL: %s\n",why); std::exit(1); } }
void hr(HRESULT result) { check(SUCCEEDED(result),"D3D operation"); }
ComPtr<ID3DBlob> compile(const char* hlsl,const char* profile,const char* entry="main") {
    ComPtr<ID3DBlob> code,error;
    HRESULT result=D3DCompile(hlsl,std::strlen(hlsl),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if (FAILED(result) && error) std::puts(static_cast<const char*>(error->GetBufferPointer()));
    hr(result); return code;
}
namespace edvr {
ID3D11Texture2D* testScene=nullptr;
ID3D11DepthStencilView* testDepth=nullptr;
int testEye=0;
Log& Log::get() { static Log instance; return instance; }
Log::~Log()=default;
void Log::note(const char*,...) {}
void* bindingGet(BindSlot slot) { check(slot==BindSlot::Dsv0,"only scene depth shadow queried"); return testDepth; }
bool depthProbeIsSceneDepth(const void* resource) { return resource==testScene; }
bool depthProbeSceneDepthFormat(uint32_t,uint32_t,int eye,ID3D11Texture2D** tex,uint32_t* fmt) {
    *tex=eye==testEye?testScene:nullptr; *fmt=DXGI_FORMAT_D32_FLOAT; return *tex!=nullptr;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* ctx,UINT n,ID3D11RenderTargetView* const* rt,ID3D11DepthStencilView* ds) { ctx->OMSetRenderTargets(n,rt,ds); }
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx,const char* hlsl,size_t,const char* entry,const char*,const SwapMacro*,const char*) {
    ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev); auto code=compile(hlsl,"cs_5_0",entry);
    ID3D11ComputeShader* shader=nullptr; hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)); return shader;
}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* ctx,const char* hlsl,size_t,const char* entry,const char*,const SwapMacro*,const char*) {
    ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev); auto code=compile(hlsl,"ps_5_0",entry);
    ID3D11PixelShader* shader=nullptr; hr(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)); return shader;
}
}
using namespace edvr;
std::vector<float> readBuffer(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Buffer* src) {
    D3D11_BUFFER_DESC bd{}; src->GetDesc(&bd); bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;
    bd.Usage=D3D11_USAGE_STAGING; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage; hr(dev->CreateBuffer(&bd,nullptr,&stage)); ctx->CopyResource(stage.Get(),src);
    D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));
    std::vector<float> values(bd.ByteWidth/4); std::memcpy(values.data(),map.pData,bd.ByteWidth); ctx->Unmap(stage.Get(),0); return values;
}
std::vector<float> readTexture(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC td{}; src->GetDesc(&td); td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; hr(dev->CreateTexture2D(&td,nullptr,&stage)); ctx->CopyResource(stage.Get(),src);
    D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));
    unsigned channels=td.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?4:td.Format==DXGI_FORMAT_R32G8X24_TYPELESS?2:1;
    std::vector<float> values(td.Width*td.Height*channels);
    for (unsigned y=0;y<td.Height;++y) std::memcpy(values.data()+y*td.Width*channels,static_cast<const char*>(map.pData)+y*map.RowPitch,td.Width*channels*4);
    ctx->Unmap(stage.Get(),0); return values;
}
int main(int argc,char** argv) {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL level;
    HRESULT made=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
    if (made==DXGI_ERROR_SDK_COMPONENT_MISSING) made=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
    hr(made); ComPtr<ID3D11InfoQueue> info; dev.As(&info);
    D3D11_TEXTURE2D_DESC td{}; td.Width=td.Height=8; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_TYPELESS; td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> scene; ComPtr<ID3D11DepthStencilView> depth;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format=DXGI_FORMAT_D32_FLOAT; dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    hr(dev->CreateTexture2D(&td,nullptr,&scene)); hr(dev->CreateDepthStencilView(scene.Get(),&dd,&depth));
    testScene=scene.Get(); testDepth=depth.Get();
    float model[13][4]{},sc[280][4]{},patch[18][4]{};
    model[4][0]=model[5][1]=model[7][2]=1; model[6][3]=.025f;
    model[9][0]=model[10][1]=model[11][2]=1;
    for (int i=0;i<4;++i) for (int j=0;j<4;++j) sc[270+i][j]=model[4+j][i];
    patch[8][3]=1; patch[1][2]=100;
    ComPtr<ID3D11Buffer> cb[3];
    auto buffer=[&](unsigned bytes,ComPtr<ID3D11Buffer>& dst) { D3D11_BUFFER_DESC bd{}; bd.ByteWidth=bytes; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; hr(dev->CreateBuffer(&bd,nullptr,&dst)); };
    buffer(sizeof(model),cb[0]); buffer(sizeof(sc),cb[1]); buffer(sizeof(patch),cb[2]);
    auto upload=[&] { ctx->UpdateSubresource(cb[0].Get(),0,nullptr,model,0,0); ctx->UpdateSubresource(cb[1].Get(),0,nullptr,sc,0,0); ctx->UpdateSubresource(cb[2].Get(),0,nullptr,patch,0,0); };
    auto code=compile("float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),.00025,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> vs; hr(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    auto psCode=compile("float4 main():SV_Target{return 1;}","ps_5_0");
    ComPtr<ID3D11PixelShader> ps; hr(dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster; hr(dev->CreateRasterizerState(&rd,&raster));
    auto bind=[&] {
        ctx->OMSetRenderTargets(0,nullptr,depth.Get()); ctx->VSSetShader(vs.Get(),nullptr,0); ctx->PSSetShader(ps.Get(),nullptr,0);
        ID3D11Buffer* ptr[3]={cb[0].Get(),cb[1].Get(),cb[2].Get()}; ctx->VSSetConstantBuffers(0,3,ptr);
        ctx->PSSetConstantBuffers(13,1,cb[0].GetAddressOf()); ctx->CSSetConstantBuffers(0,3,ptr);
        ctx->RSSetState(raster.Get()); D3D11_VIEWPORT vp{0,0,8,8,0,1}; ctx->RSSetViewports(1,&vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    auto run=[&] {
        upload(); bind(); check(celestialMotionBegin(ctx.Get(),kTerrainDepth),"terrain pass begins");
        ctx->Draw(3,0); celestialMotionEnd(ctx.Get());
        ComPtr<ID3D11PixelShader> after; ctx->PSGetShader(&after,nullptr,nullptr); check(after.Get()==ps.Get(),"PS restored");
        ComPtr<ID3D11DepthStencilView> afterDepth; ctx->OMGetRenderTargets(0,nullptr,&afterDepth); check(afterDepth.Get()==depth.Get(),"DSV restored");
        ComPtr<ID3D11Buffer> afterCb; ctx->PSGetConstantBuffers(13,1,&afterCb); check(afterCb.Get()==cb[0].Get(),"PS constants restored");
        ctx->CSGetConstantBuffers(1,1,afterCb.ReleaseAndGetAddressOf()); check(afterCb.Get()==cb[1].Get(),"CS constants restored");
        return readBuffer(dev.Get(),ctx.Get(),g_eyes[testEye].records[g_eyes[testEye].write].buffer.Get());
    };
    celestialMotionConfigure(true); ctx->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH,.00025f,0);
    auto values=run(); check(values[55]==0,"first observation has no invented motion");
    // Overwrite reused game constants after the draw: previous state must
    // remain the draw-time state on the GPU, not its end-of-frame contents.
    patch[1][2]=-999; upload(); celestialMotionFrameBoundary();
    patch[1][0]=-2; patch[1][1]=1; patch[1][2]=90;
    values=run(); check(values[55]==1,"consecutive patch history matches");
    check(std::fabs(values[59]-2)<1e-5f && std::fabs(values[63]+1)<1e-5f && std::fabs(values[67]-10)<1e-5f,"draw-time approach translation survives buffer overwrite");
    celestialMotionStageDump(ctx.Get(),scene.Get());
    // Blocking only in this fixture: production reads after the eye-run
    // grace period with DO_NOT_WAIT. Ensure that copy is complete here.
    D3D11_MAPPED_SUBRESOURCE dumpMap{}; hr(ctx->Map(g_dump.Get(),0,D3D11_MAP_READ,0,&dumpMap)); ctx->Unmap(g_dump.Get(),0);
    celestialMotionWriteDump(ctx.Get(),L"build\\obj\\terrainmotion",L"fixture");
    for (float z:readTexture(dev.Get(),ctx.Get(),scene.Get())) check(z==.00025f,"game scene depth unchanged");
    ID3D11ShaderResourceView* views[3]{}; celestialMotionViews(scene.Get(),views);
    check(views[0] && views[1] && views[2],"complete terrain inputs published");
    // Compile the actual temporal consumer and run it over the private
    // index/depth plus a final scene depth, then test foreground rejection.
    std::ifstream source("src/d3d11/temporal_pass.cpp"); std::string text((std::istreambuf_iterator<char>(source)),{});
    const auto begin=text.find("bool terrainPixel("); const auto end=text.find("\n}\n",begin);
    check(begin!=std::string::npos && end!=std::string::npos,"production temporal consumer found");
    std::string hlsl="Texture2D<uint> TI:register(t9);Texture2D<float> TZ:register(t10);struct TerrainRecord{uint4 key[12];float4 q;float4 t;float4 r[3];};StructuredBuffer<TerrainRecord> TR:register(t11);Texture2D<float> Final:register(t0);RWTexture2D<float4> Out:register(u0);static const int4 region=0;static const int2 size=int2(8,8);static const float4 knobs=float4(0,1,.025,0);static const float4 probe=float4(0,0,0,8);static const float4 tanPrev=float4(-1,1,-1,1);bool uiCovered(int2 p){return p.x==0;}float zSceneAt(int2 p){return Final.Load(int3(p,0));}\n";
    hlsl+=text.substr(begin,end+3-begin);
    hlsl+="[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){float2 p=id.xy,pp;float zp;float3 d=float3(-1+(p.x+.5)*.25,1-(p.y+.5)*.25,-1);bool ok=terrainPixel(p,d,pp,zp);Out[id.xy]=float4(pp-p,zp,ok?1:0);}";
    auto testCode=compile(hlsl.c_str(),"cs_5_0"); ComPtr<ID3D11ComputeShader> consumer;
    hr(dev->CreateComputeShader(testCode->GetBufferPointer(),testCode->GetBufferSize(),nullptr,&consumer));
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output; ComPtr<ID3D11UnorderedAccessView> outputUav;
    hr(dev->CreateTexture2D(&td,nullptr,&output)); hr(dev->CreateUnorderedAccessView(output.Get(),nullptr,&outputUav));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=DXGI_FORMAT_R32_FLOAT; sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> finalSrv; hr(dev->CreateShaderResourceView(scene.Get(),&sd,&finalSrv));
    auto consume=[&] {
        ctx->OMSetRenderTargets(0,nullptr,nullptr); ctx->CSSetShader(consumer.Get(),nullptr,0);
        ctx->CSSetShaderResources(0,1,finalSrv.GetAddressOf()); ctx->CSSetShaderResources(9,3,views);
        ctx->CSSetUnorderedAccessViews(0,1,outputUav.GetAddressOf(),nullptr); ctx->Dispatch(1,1,1);
        auto result=readTexture(dev.Get(),ctx.Get(),output.Get()); ctx->ClearState(); return result;
    };
    auto motion=consume(); check(motion[3]==0,"UI-covered pixel rejects terrain motion");
    check(motion[4*27+3]==1 && std::fabs(motion[4*27+2]-110)<1e-4f,"terrain consumer uses physical prior depth");
    check(std::fabs(motion[4*27]-(4*(-12.5f+2)/110+.5f))<1e-5f,"motion in input pixels, correct sign and view handedness");
    ctx->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH,.1f,0); motion=consume();
    check(motion[4*27+3]==0,"nearer final scene rejects old terrain coverage");
    celestialMotionFrameBoundary(); celestialMotionViews(scene.Get(),views);
    check(!views[0] && !views[1] && !views[2],"missing draw cannot reuse a stale layer");
    patch[3][0]=10; values=run(); check(values[55]==0,"changed LOD/patch bounds decline history");
    celestialMotionFrameBoundary(); run(); run(); celestialMotionFrameBoundary();
    values=run(); check(values[55]==0,"duplicate predecessor keys decline history");
    celestialMotionFrameBoundary(); celestialMotionFrameBoundary(); values=run();
    check(values[55]==0,"missing frame breaks continuity");
    // Right-eye history is independent, even at the same dimensions and
    // with identical patch keys. Resizing/replacing its scene resets it.
    auto oldScene=scene; auto oldDepth=depth;
    td.Format=DXGI_FORMAT_R32_TYPELESS; td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    hr(dev->CreateTexture2D(&td,nullptr,scene.ReleaseAndGetAddressOf()));
    hr(dev->CreateDepthStencilView(scene.Get(),&dd,depth.ReleaseAndGetAddressOf()));
    testScene=scene.Get(); testDepth=depth.Get(); testEye=1;
    values=run(); check(values[55]==0,"right eye cannot consume left-eye history");
    celestialMotionFrameBoundary(); values=run(); check(values[55]==1,"right-eye history advances independently");
    scene=oldScene; depth=oldDepth; testScene=scene.Get(); testDepth=depth.Get(); testEye=0;
    // Exercise the parallel search across lanes and loop strides, including
    // the final slot. These are complete production keys/records from run().
    celestialMotionFrameBoundary();celestialMotionFrameBoundary();
    auto& searchEye=g_eyes[0];auto& previous=searchEye.records[1-searchEye.write];
    std::vector<float> searchHistory(kRecords*68,0);
    auto search=[&](std::initializer_list<unsigned> slots,const char* label,bool matched) {
        std::fill(searchHistory.begin(),searchHistory.end(),0.f);
        for(unsigned slot:slots)std::memcpy(searchHistory.data()+slot*68,values.data(),kRecordBytes);
        ctx->UpdateSubresource(previous.buffer.Get(),0,nullptr,searchHistory.data(),0,0);previous.count=kRecords;
        searchEye.records[searchEye.write].count=0;
        auto result=run();check((result[55]==1)==matched,label);
    };
    for(unsigned slot:{0u,63u,64u,511u})search({slot},"unique history matches across every search stride",true);
    search({0,511},"duplicate keys across distant lanes decline history",false);
    search({447,511},"duplicate keys in one lane's different strides decline history",false);
    search({},"full history with no matching key declines",false);
    g_eyes[0].records[g_eyes[0].write].count=kRecords;
    check(!celestialMotionBegin(ctx.Get(),kTerrainDepth),"draw limit declines safely");
    g_eyes[0].records[g_eyes[0].write].count=0;
    celestialMotionConfigure(false); check(!celestialMotionBegin(ctx.Get(),kTerrainDepth),"AA off does no terrain work");
    celestialMotionConfigure(true); check(!celestialMotionBegin(ctx.Get(),1),"unrelated shader unchanged");
    // Optional capture replay: pairs of real cb0[13], cb1[280], cb2[18],
    // followed by the double-precision reference translation (three floats).
    if (argc>1) {
        std::ifstream f(argv[1],std::ios::binary); check(bool(f),"open captured transform fixture");
        unsigned pairs=0; f.read(reinterpret_cast<char*>(&pairs),4);
        for (unsigned pair=0;pair<pairs;++pair) {
            celestialMotionFrameBoundary(); celestialMotionFrameBoundary();
            for (unsigned step=0;step<2;++step) {
                f.read(reinterpret_cast<char*>(model),sizeof(model)); f.read(reinterpret_cast<char*>(sc),sizeof(sc)); f.read(reinterpret_cast<char*>(patch),sizeof(patch));
                values=run(); if (!step) celestialMotionFrameBoundary();
            }
            float expected[3]{}; f.read(reinterpret_cast<char*>(expected),sizeof(expected)); check(bool(f),"complete captured pair");
            check(values[55]==1,"captured patch matches and projection validates");
            for (int j=0;j<3;++j) check(std::fabs(values[59+j*4]-expected[j])<100,"captured GPU translation agrees within float precision at celestial distances");
        }
        std::printf("Captured terrain replay: %u pairs\n",pairs);
    }
    // Original null-PS depth prepass: capture both coverage outputs during
    // the game's draw. Compare its D32S8 depth/stencil with an untouched
    // prepass, including rejected fragments, stencil failures and sample mask.
    celestialMotionFrameBoundary();celestialMotionFrameBoundary();
    td.Format=DXGI_FORMAT_R32G8X24_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    hr(dev->CreateTexture2D(&td,nullptr,scene.ReleaseAndGetAddressOf()));
    hr(dev->CreateDepthStencilView(scene.Get(),&dd,depth.ReleaseAndGetAddressOf()));
    testScene=scene.Get();testDepth=depth.Get();testEye=0;
    td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colour;ComPtr<ID3D11RenderTargetView> colourRtv;
    hr(dev->CreateTexture2D(&td,nullptr,&colour));hr(dev->CreateRenderTargetView(colour.Get(),nullptr,&colourRtv));
    D3D11_DEPTH_STENCIL_DESC state{};state.DepthEnable=TRUE;state.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    state.DepthFunc=D3D11_COMPARISON_GREATER;state.StencilEnable=TRUE;state.StencilReadMask=state.StencilWriteMask=0xff;
    state.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;
    state.FrontFace.StencilFailOp=D3D11_STENCIL_OP_INVERT;state.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_INCR_SAT;
    state.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;state.BackFace=state.FrontFace;
    ComPtr<ID3D11DepthStencilState> gameDepth;hr(dev->CreateDepthStencilState(&state,&gameDepth));
    for(unsigned mode=0;mode<6;++mode) {
        const float clearZ=mode==1?.5f:0.f,clearColour[4]={.375f,0,0,0};
        const UINT ref=mode==2?18:17,mask=mode==3?0:~0u;
        auto originalBind=[&] {
            upload();bind();ctx->PSSetShader(nullptr,nullptr,0);
            ctx->OMSetRenderTargets(1,colourRtv.GetAddressOf(),depth.Get());
            ctx->OMSetDepthStencilState(gameDepth.Get(),ref);ctx->OMSetBlendState(nullptr,nullptr,mask);
            if(mode==4) {
                auto biased=rd;biased.DepthBias=500000;
                ComPtr<ID3D11RasterizerState> b;hr(dev->CreateRasterizerState(&biased,&b));ctx->RSSetState(b.Get());
            }
            if(mode==5) {D3D11_VIEWPORT vp{0,0,8,8,.2f,.8f};ctx->RSSetViewports(1,&vp);}
            ctx->ClearRenderTargetView(colourRtv.Get(),clearColour);
            ctx->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,clearZ,17);
        };
        originalBind();ctx->Draw(3,0);auto stock=readTexture(dev.Get(),ctx.Get(),scene.Get());
        celestialMotionFrameBoundary();originalBind();
        check(celestialMotionBeginOriginal(ctx.Get(),kTerrainDepth),"null PS captured in original draw");
        ctx->Draw(3,0);celestialMotionEnd(ctx.Get());
        auto fused=readTexture(dev.Get(),ctx.Get(),scene.Get());
        // D32S8's unused upper 24 bits are unspecified. Compare depth and
        // the stencil byte independently, not that padding.
        bool equal=true;for(unsigned i=0;i<64;++i)equal=equal && stock[i*2]==fused[i*2] &&
            (reinterpret_cast<const unsigned*>(stock.data())[i*2+1]&255)==(reinterpret_cast<const unsigned*>(fused.data())[i*2+1]&255);
        check(equal,"original depth and stencil identical with coverage capture");
        auto c=readTexture(dev.Get(),ctx.Get(),colour.Get());check(std::all_of(c.begin(),c.end(),[](float x){return x==.375f;}),"original colour untouched");
        ComPtr<ID3D11DepthStencilState> restored;UINT restoredRef=0;ctx->OMGetDepthStencilState(&restored,&restoredRef);
        check(restored.Get()==gameDepth.Get() && restoredRef==ref,"game stencil/depth state restored");
        UINT restoredMask=0;ctx->OMGetBlendState(nullptr,nullptr,&restoredMask);check(restoredMask==mask,"original sample mask preserved");
        celestialMotionViews(scene.Get(),views);check(views[1]==g_eyes[0].originalDepthSrv.Get(),"original draw depth published");
        auto z=readTexture(dev.Get(),ctx.Get(),g_eyes[0].originalDepth.Get());
        auto ix=readTexture(dev.Get(),ctx.Get(),g_eyes[0].index.Get());
        check(z[27]==(mode==0 || mode>=4?fused[27*2]:0.f),"coverage depth follows visibility, raster bias and viewport depth");
        check(reinterpret_cast<unsigned*>(ix.data())[27]==(mode==0 || mode>=4?1u:0u),"coverage index follows original visibility");
        // Mixed fallback modes cannot leave mismatched index and depth.
        check(!celestialMotionBegin(ctx.Get(),kTerrainDepth),"mixed original/reissue layer declines safely");
    }
    celestialMotionFrameBoundary();bind();
    check(!celestialMotionBeginOriginal(ctx.Get(),kTerrainDepth),"non-null game PS retains reissue path");
    check(celestialMotionBegin(ctx.Get(),kTerrainDepth),"non-null PS fallback remains available");celestialMotionEnd(ctx.Get());
    celestialMotionConfigure(false);check(!celestialMotionBeginOriginal(ctx.Get(),kTerrainDepth),"AA off bypasses original capture too");
    // Both production temporal entries must compile with the added inputs.
    auto start=text.find("R\"HLSL(")+7;
    const auto last=text.find(")HLSL\";",start);
    // Extract adjacent literals of the first shader, stopping at its ;.
    std::string full; size_t at=start;
    for (;;) {
        auto close=text.find(")HLSL\"",at); check(close!=std::string::npos,"temporal literal complete");
        full+=text.substr(at,close-at);
        if (close==last) break;
        at=text.find("R\"HLSL(",close+6)+7;
    }
    compile(full.c_str(),"cs_5_0","main"); compile(full.c_str(),"cs_5_0","mv");
    ctx->ClearState(); celestialMotionShutdown();
    if (info) for (UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T n=0; info->GetMessage(i,nullptr,&n); std::vector<char> bytes(n); auto* msg=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); hr(info->GetMessage(i,msg,&n));
        if (msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) std::puts(msg->pDescription);
        check(msg->Severity>D3D11_MESSAGE_SEVERITY_WARNING,"no D3D debug warnings/errors");
    }
    std::printf("Terrain motion regression: %u checks passed\n",checks);
}
