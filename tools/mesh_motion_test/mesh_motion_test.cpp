// WARP regression of the production mesh history, rasterised coverage
// and temporal consumer. No game/runtime or synchronous render-path readback.
#include "../../src/d3d11/mesh_motion.cpp"
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
#include <filesystem>
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
// This rig exercises mesh rendering without installing game-code diagnostics.
const char* attachObjectRecordWriterHook(ObjectRecordWriterProbe*) noexcept {return "identity_mismatch";}
void detachObjectRecordWriterHook(ObjectRecordWriterProbe*) noexcept {}
bool objectRecordWriterHookMatches(uintptr_t) noexcept {return false;}
const char* attachKinematicEvalHooks(KinematicEvalProbe*) noexcept {return "identity_mismatch";}
bool kinematicEvalHooksMatch(uintptr_t) noexcept {return false;}
ID3D11Texture2D* testScene=nullptr;
ID3D11DepthStencilView* testDepth=nullptr;
int testEye=0;
bool testMenuOpen=false;
unsigned testMenuNotices=0;
const char* testMenuNotice=nullptr;
uint64_t testBenchmarkDisturbanceEpoch=0;
Log& Log::get() { static Log instance; return instance; }
Log::~Log()=default;
void Log::note(const char*,...) {}
bool menuOpen() { return testMenuOpen; }
void menuNotify(const char* message) { ++testMenuNotices;testMenuNotice=message; }
uint64_t perfMonitorBenchmarkDisturbanceEpoch() noexcept { return testBenchmarkDisturbanceEpoch; }
void* bindingGet(BindSlot slot) { check(slot==BindSlot::Dsv0,"only scene depth shadow queried"); return testDepth; }
bool depthProbeSceneTextureEye(uint32_t,uint32_t,const void* resource,int* eye) {
    if(!eye)return false;*eye=-1;if(!resource || resource!=testScene || testEye<0 || testEye>1)return false;*eye=testEye;return true;
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
UINT word(const std::vector<float>& values,size_t at) { UINT value=0;std::memcpy(&value,&values[at],sizeof(value));return value; }
UINT bits(float value) { UINT result=0;std::memcpy(&result,&value,sizeof(result));return result; }
std::vector<float> readTexture(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC td{}; src->GetDesc(&td); td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; hr(dev->CreateTexture2D(&td,nullptr,&stage)); ctx->CopyResource(stage.Get(),src);
    D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));
    unsigned channels=td.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?4:(td.Format==DXGI_FORMAT_R32G8X24_TYPELESS || td.Format==DXGI_FORMAT_R32G32_FLOAT)?2:1;
    std::vector<float> values(td.Width*td.Height*channels);
    for (unsigned y=0;y<td.Height;++y) std::memcpy(values.data()+y*td.Width*channels,static_cast<const char*>(map.pData)+y*map.RowPitch,td.Width*channels*4);
    ctx->Unmap(stage.Get(),0); return values;
}
void issue(ID3D11DeviceContext* c,UINT n,UINT instances,UINT start,INT base,UINT first){c->DrawIndexedInstanced(n,instances,start,base,first);}
#include "capture_replay.h"
int main(int argc,char** argv){
    using namespace edvr::mesh_motion_detail;
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    bool hardware=argc>1 && (std::strcmp(argv[1],"--hardware")==0 || std::strcmp(argv[1],"--replay-hardware")==0);
    auto driver=hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP;
    HRESULT made=D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
    if(made==DXGI_ERROR_SDK_COMPONENT_MISSING)made=D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
    hr(made);ComPtr<ID3D11InfoQueue> messages;dev.As(&messages);
    auto buffer=[&](UINT bytes,UINT bind,UINT step=0,const void* data=nullptr){D3D11_BUFFER_DESC b{};b.ByteWidth=bytes;b.BindFlags=bind;b.StructureByteStride=step;b.MiscFlags=step?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;D3D11_SUBRESOURCE_DATA init{};init.pSysMem=data;ComPtr<ID3D11Buffer> p;hr(dev->CreateBuffer(&b,data?&init:nullptr,&p));return p;};
    constexpr UINT W=64,H=64;
    D3D11_TEXTURE2D_DESC td{};td.Width=W;td.Height=H;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> scene[2];ComPtr<ID3D11DepthStencilView> dsv[2];ComPtr<ID3D11ShaderResourceView> z[2];
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    for(int i=0;i<2;++i){hr(dev->CreateTexture2D(&td,nullptr,&scene[i]));hr(dev->CreateDepthStencilView(scene[i].Get(),&dd,&dsv[i]));hr(dev->CreateShaderResourceView(scene[i].Get(),&sd,&z[i]));}
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> ds;hr(dev->CreateDepthStencilState(&depth,&ds));
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;hr(dev->CreateRasterizerState(&rs,&raster));
    const char* shader=R"HLSL(
cbuffer Scene:register(b1){float4 scene[276];}struct P{uint4 d[21];};StructuredBuffer<P> Pool:register(t33);
struct O{uint3 id:__USER_MATERIALMODULATION_DATAID;float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;float2 uv:__USER_VERTEX_M_TEXCOORD;float4 pos:SV_Position;};
O main(uint2 id:INSTANCEANDMODELDATAINDEX,float3 pos:POSITION){
P p=Pool[id.x];float4 q=float4(p.d[0].z&65535,p.d[0].z>>16,p.d[0].w&65535,p.d[0].w>>16)*(1.0/32767.0)-1;
pos=((2*q.w*q.w-1)*pos+2*dot(q.xyz,pos)*q.xyz+2*q.w*cross(q.xyz,pos))*asfloat(p.d[0].y)+asfloat(p.d[1].xyz)-scene[275].xyz;
O o=(O)0;o.id=uint3(0,id.x,0);o.pos=pos.x*scene[270]+pos.y*scene[271]+pos.z*scene[272]+scene[273];return o;}
)HLSL";
    auto code=compile(shader,"vs_5_0"),pc=compile("float4 main():SV_Target{return 1;}","ps_5_0");ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    hr(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));hr(dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps));
    D3D11_INPUT_ELEMENT_DESC el[2]={{"INSTANCEANDMODELDATAINDEX",0,DXGI_FORMAT_R32G32_UINT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1},{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,1,0,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(el,2,code->GetBufferPointer(),code->GetBufferSize(),&layout));
    float vertices[4][10]{};vertices[0][0]=vertices[0][1]=-.6f;vertices[1][0]=.6f;vertices[1][1]=-.6f;vertices[2][0]=vertices[2][1]=.6f;vertices[3][0]=-.6f;vertices[3][1]=.6f;
    UINT indices[6]={0,1,2,0,2,3},ids[128]{};UINT poolData[4][84]{};float sceneData[276][4]{};
    auto vb=buffer(sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,0,vertices),ib=buffer(sizeof(indices),D3D11_BIND_INDEX_BUFFER,0,indices),iv=buffer(sizeof(ids),D3D11_BIND_VERTEX_BUFFER),cb=buffer(sizeof(sceneData),D3D11_BIND_CONSTANT_BUFFER),pool=buffer(sizeof(poolData),D3D11_BIND_SHADER_RESOURCE,336);
    ComPtr<ID3D11ShaderResourceView> poolView;hr(dev->CreateShaderResourceView(pool.Get(),nullptr,&poolView));
    auto pose=[&](float distance,float x,unsigned slot=0){float* p=reinterpret_cast<float*>(poolData[slot]);p[1]=distance;p[4]=x;p[6]=distance;poolData[slot][2]=0x80008000;poolData[slot][3]=0xffff8000;};
    auto bind=[&](int eye){
        testEye=eye;testScene=scene[eye].Get();testDepth=dsv[eye].Get();ctx->OMSetRenderTargets(0,nullptr,dsv[eye].Get());ctx->OMSetDepthStencilState(ds.Get(),0);ctx->OMSetBlendState(nullptr,nullptr,~0u);
        ctx->RSSetState(raster.Get());D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};ctx->RSSetViewports(1,&vp);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->IASetInputLayout(layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* bufs[2]={iv.Get(),vb.Get()};UINT strides[2]={8,40},off[2]{};ctx->IASetVertexBuffers(0,2,bufs,strides,off);ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        ctx->VSSetConstantBuffers(1,1,cb.GetAddressOf());ctx->VSSetShaderResources(33,1,poolView.GetAddressOf());
        ctx->UpdateSubresource(cb.Get(),0,nullptr,sceneData,0,0);ctx->UpdateSubresource(pool.Get(),0,nullptr,poolData,0,0);ctx->UpdateSubresource(iv.Get(),0,nullptr,ids,0,0);
    };
    uint64_t testHash=materialVs;
    auto run=[&](int eye=0,UINT n=1){bind(eye);ctx->ClearDepthStencilView(dsv[eye].Get(),D3D11_CLEAR_DEPTH,0,0);issue(ctx.Get(),6,n,0,0,0);meshMotionDraw(ctx.Get(),issue,6,n,0,0,0,testHash);
        ComPtr<ID3D11PixelShader> after;ctx->PSGetShader(&after,nullptr,nullptr);check(after==ps,"original PS restored");ComPtr<ID3D11DepthStencilView> afterDepth;ctx->OMGetRenderTargets(0,nullptr,&afterDepth);check(afterDepth==dsv[eye],"depth target restored");
        ID3D11ShaderResourceView* views[2]{};meshMotionViews(ctx.Get(),scene[eye].Get(),views);check(views[0] && views[1],"mesh views available");return readBuffer(dev.Get(),ctx.Get(),eyes[eye].history[eyes[eye].write].buffer.Get());};
    auto reset=[&](){ctx->ClearState();meshMotionShutdown();std::memset(poolData,0,sizeof(poolData));std::memset(sceneData,0,sizeof(sceneData));std::memset(ids,0,sizeof(ids));sceneData[270][0]=sceneData[271][1]=sceneData[272][3]=1;sceneData[273][2]=.025f;};
    meshMotionConfigure(true);
    // Admission accounting uses no clock on ordinary unsampled rejects and
    // still explains zero-accepted workloads such as the on-foot path.
    reset();diagnostics={};admissionClockCalls=0;
    meshMotionDraw(nullptr,issue,6,1,0,0,0,materialVs);
    meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,0);
    meshMotionDraw(ctx.Get(),nullptr,6,1,0,0,0,materialVs);
    enabled=false;meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);enabled=true;
    failed=true;meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);failed=false;
    check(admissionClockCalls==0 && diagnostics.drawCpuSamples==0,"unsampled admission rejects make no performance-counter calls");
    check(diagnostics.admission.fastRejects[unsigned(FastReject::Context)]==1 && diagnostics.admission.fastRejects[unsigned(FastReject::UnknownVs)]==1 && diagnostics.admission.fastRejects[unsigned(FastReject::DrawShape)]==1 && diagnostics.admission.fastRejects[unsigned(FastReject::Disabled)]==1 && diagnostics.admission.fastRejects[unsigned(FastReject::SetupFailed)]==1,"fast rejection reasons explain context, shader, shape, disabled and setup-failed entries");
    diagnostics={};admissionClockCalls=0;uint64_t cleanupClock=0;
    {AdmissionTrace trace(true);struct CleanupMarker { uint64_t* clock;~CleanupMarker(){*clock=admissionClockCalls;} } cleanup{&cleanupClock};trace.complete();}
    check(admissionClockCalls==cleanupClock+1 && diagnostics.admission.completed==1,"successful admission takes its final timestamp after scoped cleanup");
    diagnostics={};admissionClockCalls=0;
    diagnostics.drawCalls=255;meshMotionDraw(nullptr,issue,6,1,0,0,0,materialVs);
    pose(1,0);bind(0);testDepth=nullptr;diagnostics.drawCalls=255;meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
    bind(0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);diagnostics.drawCalls=255;meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
    bind(0);ID3D11Buffer* noIds=nullptr;UINT noStride=0,noOffset=0;ctx->IASetVertexBuffers(0,1,&noIds,&noStride,&noOffset);diagnostics.drawCalls=255;meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
    bind(0);diagnostics.drawCalls=255;meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
    const auto& admission=diagnostics.admission;
    check(admission.completed==1 && admission.stages[unsigned(AdmissionStage::Fast)].rejected==1 && admission.stages[unsigned(AdmissionStage::Scene)].rejected==1 && admission.stages[unsigned(AdmissionStage::Pipeline)].rejected==1 && admission.stages[unsigned(AdmissionStage::Resources)].rejected==1,"sampled early returns and the completed WARP path land in exclusive admission stages");
    check(admission.stages[unsigned(AdmissionStage::Fast)].cpuSamples==5 && admission.stages[unsigned(AdmissionStage::Scene)].cpuSamples==4 && admission.stages[unsigned(AdmissionStage::Pipeline)].cpuSamples==3 && admission.stages[unsigned(AdmissionStage::Resources)].cpuSamples==2 && admission.stages[unsigned(AdmissionStage::Preparation)].cpuSamples==1 && admission.stages[unsigned(AdmissionStage::Accepted)].cpuSamples==1,"stage sample denominators expose which portions of the sampled hook actually ran");
    double admissionTotal=0;for(const auto& stage:admission.stages)admissionTotal+=stage.cpuMs;
    check(diagnostics.drawCpuSamples==5 && std::fabs(admissionTotal-diagnostics.drawCpuMs)<1e-6,"exclusive admission intervals sum to the same sampled full-hook duration");
    // Getter-reuse observation retains four COM identities per category for
    // one frame, counts overflow explicitly, and never substitutes for a call.
    reset();pose(1,0);bind(0);diagnostics={};descriptorShadows.clear();
    auto admit=[&](){meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);};
    admit();admit();
    auto& initialDepth=diagnostics.descriptors.kinds[unsigned(DescriptorKind::Depth)];auto& initialBlend=diagnostics.descriptors.kinds[unsigned(DescriptorKind::Blend)];auto& initialIds=diagnostics.descriptors.kinds[unsigned(DescriptorKind::Ids)];
    check(initialDepth.calls==2 && initialDepth.hits==1 && initialDepth.fills==1 && initialBlend.calls==0,"descriptor reuse counts literal non-null GetDesc calls and excludes the default blend state");
    check(initialIds.calls==2 && initialIds.hits==1 && initialIds.fills==1 && diagnostics.descriptors.kinds[unsigned(DescriptorKind::SceneCb)].hits==1 && diagnostics.descriptors.kinds[unsigned(DescriptorKind::PoolSrv)].hits==1 && diagnostics.descriptors.kinds[unsigned(DescriptorKind::PoolBuffer)].hits==1,"repeated ID, scene-CB, pool-SRV and pool-buffer identities are observed after their real getters");
    meshMotionFrameBoundary(ctx.Get());check(descriptorShadows.depth.count==0 && descriptorShadows.ids.count==0 && descriptorShadows.poolSrv.count==0,"frame boundary releases descriptor shadow identities");
    bind(0);admit();
    D3D11_BLEND_DESC observedBlendDesc{};observedBlendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;ComPtr<ID3D11BlendState> observedBlend;hr(dev->CreateBlendState(&observedBlendDesc,&observedBlend));ctx->OMSetBlendState(observedBlend.Get(),nullptr,~0u);admit();ctx->OMSetBlendState(observedBlend.Get(),nullptr,~0u);admit();
    const auto& observedBlendMetrics=diagnostics.descriptors.kinds[unsigned(DescriptorKind::Blend)];check(observedBlendMetrics.calls==2 && observedBlendMetrics.hits==1 && observedBlendMetrics.fills==1,"non-default blend descriptor identity records one fill then one hit");
    std::vector<ComPtr<ID3D11Buffer>> identityIds;for(unsigned i=0;i<5;++i){identityIds.push_back(buffer(sizeof(ids),D3D11_BIND_VERTEX_BUFFER));UINT identityStride=8,identityOffset=0;ctx->IASetVertexBuffers(0,1,identityIds.back().GetAddressOf(),&identityStride,&identityOffset);admit();}
    const auto& reboundIds=diagnostics.descriptors.kinds[unsigned(DescriptorKind::Ids)];
    check(reboundIds.calls==10 && reboundIds.hits==3 && reboundIds.fills==7 && reboundIds.evictions==2 && descriptorShadows.ids.count==4,"descriptor identity rebinds report fills and bounded shadow evictions explicitly");
    meshMotionShutdown();check(descriptorShadows.depth.count==0 && descriptorShadows.ids.count==0 && descriptorShadows.poolSrv.count==0,"shutdown releases descriptor shadow identities");meshMotionConfigure(true);
    // Depth metadata owns the view and texture for the frame. Callers borrow
    // both the texture identity and immutable descriptor without a per-draw
    // AddRef/Release pair; clearing the cache is the end of that lifetime.
    reset();ComPtr<ID3D11Texture2D> ownedScene;ComPtr<ID3D11DepthStencilView> ownedDepth;hr(dev->CreateTexture2D(&td,nullptr,&ownedScene));hr(dev->CreateDepthStencilView(ownedScene.Get(),&dd,&ownedDepth));
    ID3D11DepthStencilView* borrowedView=ownedDepth.Get();ID3D11Texture2D* borrowedScene=nullptr;const D3D11_TEXTURE2D_DESC* borrowedDesc=nullptr;
    check(depthMetadata.get(borrowedView,borrowedScene,borrowedDesc) && borrowedScene==ownedScene.Get() && borrowedDesc==&depthMetadata.desc,"depth metadata fill returns cache-owned borrowed values");
    ownedScene.Reset();ownedDepth.Reset();D3D11_TEXTURE2D_DESC retainedDesc{};borrowedScene->GetDesc(&retainedDesc);
    const ULONG referencesBefore=borrowedScene->AddRef();borrowedScene->Release();ID3D11Texture2D* borrowedAgain=nullptr;const D3D11_TEXTURE2D_DESC* descAgain=nullptr;
    check(depthMetadata.get(borrowedView,borrowedAgain,descAgain) && borrowedAgain==borrowedScene && descAgain==borrowedDesc && retainedDesc.Width==W && retainedDesc.Height==H,"cache ownership keeps borrowed depth metadata valid after external owners release");
    const ULONG referencesAfter=borrowedScene->AddRef();borrowedScene->Release();check(referencesAfter==referencesBefore,"depth metadata hit does not add a caller-owned texture reference");
    depthMetadata.clear();check(!depthMetadata.view && !depthMetadata.texture,"clearing depth metadata ends the borrowed lifetime and releases both owners");
    // The frame-local cache retains only immutable DSV metadata. Scene and
    // eye classification must still follow every live depth-probe answer.
    reset();pose(1,0);bind(0);
    auto observe=[&](){issue(ctx.Get(),6,1,0,0,0);meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);};
    observe();observe();
    check(diagnostics.depthMetadataFills==1 && diagnostics.depthMetadataHits==1,"repeated DSV reuses only immutable texture metadata");
    bind(1);observe();observe();
    check(diagnostics.depthMetadataFills==2 && diagnostics.depthMetadataHits==2,"DSV switch replaces the single metadata entry");
    ComPtr<ID3D11DepthStencilView> aliasDepth;hr(dev->CreateDepthStencilView(scene[1].Get(),&dd,&aliasDepth));
    testDepth=aliasDepth.Get();ctx->OMSetRenderTargets(0,nullptr,testDepth);observe();
    check(diagnostics.depthMetadataFills==3,"alias DSV gets its own retained identity while sharing the texture");
    auto fills=diagnostics.depthMetadataFills,hits=diagnostics.depthMetadataHits;
    testDepth=nullptr;observe();
    check(diagnostics.depthMetadataFills==fills && diagnostics.depthMetadataHits==hits,"null DSV bypasses the metadata cache");
    ComPtr<ID3D11Texture2D> foreignScene;ComPtr<ID3D11DepthStencilView> foreignDepth;
    hr(dev->CreateTexture2D(&td,nullptr,&foreignScene));hr(dev->CreateDepthStencilView(foreignScene.Get(),&dd,&foreignDepth));
    testDepth=foreignDepth.Get();ctx->OMSetRenderTargets(0,nullptr,testDepth);const auto candidates=diagnostics.eye[1].candidateDraws;
    observe();observe();
    check(diagnostics.depthMetadataFills==fills+1 && diagnostics.depthMetadataHits==hits+1 && diagnostics.eye[1].candidateDraws==candidates,"non-scene DSV metadata may hit but live classification still rejects it");
    testDepth=dsv[1].Get();testScene=scene[1].Get();ctx->OMSetRenderTargets(0,nullptr,testDepth);observe();
    const auto selectedCandidates=diagnostics.eye[1].candidateDraws;testScene=scene[0].Get();observe();
    check(diagnostics.eye[1].candidateDraws==selectedCandidates,"cached DSV cannot preserve a stale scene-pick decision");
    testScene=scene[1].Get();fills=diagnostics.depthMetadataFills;meshMotionFrameBoundary(ctx.Get());observe();
    check(diagnostics.depthMetadataFills==fills+1,"frame boundary expires retained DSV metadata");
    observe();fills=diagnostics.depthMetadataFills;meshMotionResourceWritten(nullptr);observe();
    check(diagnostics.depthMetadataFills==fills+1,"unknown writes expire retained DSV metadata");
    meshMotionShutdown();check(!depthMetadata.view && !depthMetadata.texture,"shutdown releases retained DSV metadata");
    reset();pose(2,1.25f,0);pose(3,-1.5f,1);ids[2]=1;sceneData[275][0]=.25f;sceneData[275][1]=.5f;sceneData[275][2]=.75f;
    auto raw=run(0,2);
    check(word(raw,20)==poolData[0][1] && word(raw,21)==poolData[0][2] && word(raw,22)==poolData[0][3] && word(raw,23)==poolData[0][4] && word(raw,24)==poolData[0][5] && word(raw,25)==poolData[0][6],"raw packed rigid pose is captured without conversion");
    check(word(raw,26)==bits(sceneData[275][0]) && word(raw,27)==bits(sceneData[275][1]) && word(raw,28)==bits(sceneData[275][2]),"raw scene rebase origin is captured");
    check(word(raw,29)==0 && word(raw,30)==2 && word(raw,31)==0 && word(raw,60+29)==0 && word(raw,60+30)==2 && word(raw,60+31)==1,"draw grouping and pool index use the spare key words");
    reset();pose(1,0);ids[2]=99;raw=run(0,2);
    check(raw[60+56]==0 && word(raw,60+29)==0 && word(raw,60+30)==2 && word(raw,60+31)==99,"invalid pool index keeps explicit draw grouping while its pose stays invalid");
    for(float distance:{1.f,12.f,120.f,1200.f}){
        reset();pose(distance,0);auto a=run();check(a[56]==1 && a[59]==0,"first rigid frame has no history at any distance");
        meshMotionFrameBoundary(ctx.Get());pose(distance,distance*.02f);a=run();check(a[59]==1,"rigid mesh matched independent of distance split");
        float expected=-distance*.02f;check(std::fabs(a[47]-expected)<distance*.000001f,"exact prior clip translation");
        auto coverage=readTexture(dev.Get(),ctx.Get(),eyes[0].coverage.Get());check(coverage[(32*W+32)*2]==1,"original VS produces visible coverage");
        meshMotionFrameBoundary(ctx.Get());poolData[0][0]=7;a=run();check(a[56]==0 && a[59]==0,"skinned draw rejects rigid correspondence");
    }
    reset();pose(1,0);reinterpret_cast<float*>(poolData[0])[6]=-3;
    for(auto& vertex:vertices)vertex[2]=5;ctx->UpdateSubresource(vb.Get(),0,nullptr,vertices,0,0);
    run();meshMotionFrameBoundary(ctx.Get());reinterpret_cast<float*>(poolData[0])[4]=.02f;auto behind=run();
    check(behind[59]==1 && behind[43]<0,"visible geometry with its local origin behind the eye retains exact motion");
    for(auto& vertex:vertices)vertex[2]=0;ctx->UpdateSubresource(vb.Get(),0,nullptr,vertices,0,0);
    reset();pose(1,0);run(0);pose(1,.1f);run(1);meshMotionFrameBoundary(ctx.Get());pose(1,.02f);auto left=run(0);pose(1,.13f);auto right=run(1);
    check(std::fabs(left[47]+.02f)<1e-5 && std::fabs(right[47]+.03f)<1e-5,"two eyes retain separate transforms despite shared overwritten buffers");
    check(std::fabs(left[23]-.02f)<1e-6 && std::fabs(right[23]-.13f)<1e-6,"both eyes retain their own raw current pose");
    reset();pose(1,0,0);run();meshMotionFrameBoundary(ctx.Get());pose(1,.02f,2);ids[0]=2;auto a=run();check(a[59]==1,"reordered pool index preserves mesh identity");
    meshMotionFrameBoundary(ctx.Get());meshMotionFrameBoundary(ctx.Get());a=run();check(a[59]==0,"missing frame cannot reuse stale history");
    reset();pose(1,-.05f,0);pose(1,.05f,1);ids[2]=1;run(0,2);meshMotionFrameBoundary(ctx.Get());a=run(0,2);check(a[59]==0 && a[119]==0,"ambiguous identical instances reject history");
    reset();pose(1,-.4f,0);pose(1,.4f,1);ids[2]=1;run(0,2);meshMotionFrameBoundary(ctx.Get());pose(1,-.38f,0);pose(1,.42f,1);a=run(0,2);check(a[59]==1 && a[119]==1,"separated instances match independently");
    meshMotionResourceWritten(nullptr);ID3D11ShaderResourceView* gone[2]{};meshMotionViews(ctx.Get(),scene[0].Get(),gone);check(!gone[0],"unknown writes invalidate coverage and history");
    // A deforming part must not discard the rigid hull's history. Both
    // are captured in the same eye and share an index buffer, as in the
    // reload capture with zero of 165 otherwise-valid records matched.
    reset();pose(1,0);
    auto changing=buffer(sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,0,vertices);
    auto mixed=[&](UINT n){
        bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
        issue(ctx.Get(),6,n,0,0,0);meshMotionDraw(ctx.Get(),issue,6,n,0,0,0,materialVs);
        UINT step=40,offset=0;ctx->IASetVertexBuffers(1,1,changing.GetAddressOf(),&step,&offset);
        issue(ctx.Get(),6,n,0,0,0);meshMotionDraw(ctx.Get(),issue,6,n,0,0,0,materialVs);
        ID3D11ShaderResourceView* views[2]{};meshMotionViews(ctx.Get(),scene[0].Get(),views);
        return readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get());
    };
    mixed(1);meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(changing.Get());
    pose(1,.02f);a=mixed(1);
    check(a[59]==1 && a[119]==0,"a changed part rejects only its own correspondence, preserving the hull");
    meshMotionResourceWritten(changing.Get());
    ID3D11ShaderResourceView* retained[2]{};meshMotionViews(ctx.Get(),scene[0].Get(),retained);
    check(retained[0] && retained[1],"a later write does not erase geometry already drawn into this eye");
    meshMotionFrameBoundary(ctx.Get());pose(1,.04f);a=mixed(1);
    check(a[59]==1 && a[119]==0,"write after consumption still rejects the edited part next frame");
    meshMotionFrameBoundary(ctx.Get());a=mixed(1);
    check(a[59]==1 && a[119]==1,"unchanged geometry recovers after one consecutive frame");
    meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(ib.Get());a=mixed(1);
    check(a[59]==0 && a[119]==0,"shared index edits reject every dependent mesh");
    meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(pool.Get());a=mixed(1);
    check(a[59]==1 && a[119]==1,"pool pose writes retain geometry generations");
    meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(changing.Get());
    pose(1,.1f);run(1);pose(1,.02f);a=mixed(1);
    check(a[59]==1 && a[119]==0,"other-eye rendering does not erase the unchanged eye history");
    check(watched.size()==3,"geometry generation cache retains only referenced geometry buffers");
    meshMotionFrameBoundary(ctx.Get());meshMotionFrameBoundary(ctx.Get());
    check(watched.empty(),"unused geometry generations expire with their source references");
    a=mixed(1);geometryEpoch=~0u;meshMotionResourceWritten(changing.Get());
    meshMotionViews(ctx.Get(),scene[0].Get(),gone);
    check(!gone[0] && watched.empty(),"geometry generation wrap discards history conservatively");
    // Two meshes sharing an IB: test exact byte intersections for both
    // index formats, including a nonzero IA offset and writes at edges.
    auto originalIb=ib;UINT sharedIndices[128]{};
    ib=buffer(sizeof(sharedIndices),D3D11_BIND_INDEX_BUFFER,0,sharedIndices);
    for(UINT bytes:{2u,4u}){
        reset();pose(1,0);
        const UINT offset=8,split=offset+6*bytes,finish=offset+12*bytes;
        auto slices=[&](){
            bind(0);ctx->IASetIndexBuffer(ib.Get(),bytes==2?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT,offset);
            ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
            for(UINT start:{0u,6u}){issue(ctx.Get(),6,1,start,0,0);meshMotionDraw(ctx.Get(),issue,6,1,start,0,0,materialVs);}
            ID3D11ShaderResourceView* views[2]{};meshMotionViews(ctx.Get(),scene[0].Get(),views);
            return readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get());
        };
        slices();meshMotionFrameBoundary(ctx.Get());
        meshMotionResourceWritten(ib.Get(),finish,finish+320);a=slices();
        check(a[59]==1 && a[119]==1,"unrelated transient IB upload preserves both rigid meshes");
        meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(ib.Get(),0,offset);a=slices();
        check(a[59]==1 && a[119]==1,"write immediately before the IA offset preserves both slices");
        meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(ib.Get(),split,finish);a=slices();
        check(a[59]==1 && a[119]==0,"write to the second index slice rejects only that mesh");
        meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(ib.Get(),split-1,split+1);a=slices();
        check(a[59]==0 && a[119]==0,"write crossing the shared boundary rejects both meshes");
        meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(ib.Get(),split,split);a=slices();
        check(a[59]==1 && a[119]==1,"empty box preserves correspondence and recovery");
        meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(ib.Get());a=slices();
        check(a[59]==0 && a[119]==0,"unknown write extent rejects all slices of its resource");
        meshMotionFrameBoundary(ctx.Get());meshMotionResourceWritten(vb.Get(),0,1);a=slices();
        check(a[59]==0 && a[119]==0,"vertex edits remain conservative without an exact vertex range");
        meshMotionFrameBoundary(ctx.Get());run();meshMotionFrameBoundary(ctx.Get());run();meshMotionFrameBoundary(ctx.Get());
        check(watched[ib.Get()].indices.size()==1,"unused slices expire even when another slice keeps the IB alive");
    }
    ib=originalIb;
    reset();pose(1,0);run();meshMotionFrameBoundary(ctx.Get());bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);issue(ctx.Get(),6,1,0,0,0);
    diagnostics={};frames=0;
    for(unsigned i=0;i<513;++i)meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
    check(eyes[0].history[eyes[0].write].count==512,"record cap bounds excess draws");
    check(diagnostics.eye[0].candidateDraws==513 && diagnostics.eye[0].acceptedDraws==512 && diagnostics.eye[0].capRejectedDraws==1 && diagnostics.eye[0].sampledCapDraws==1 && diagnostics.eye[0].eligibleCapRejectedDraws==1,"bounded cap sample separates fully eligible refusal from accepted candidates");
    diagnostics.eye[0].capProbes=capProbeBudget;
    const auto candidatesBefore=diagnostics.eye[0].candidateDraws;
    const auto pendingBefore=pending.count;
    comparison.stage=ComparisonStage::Running;comparison.measurement=true;comparison.metrics={};
    meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
    check(diagnostics.eye[0].capRejectedDraws==2 && diagnostics.eye[0].sampledCapDraws==1 && diagnostics.eye[0].candidateDraws==candidatesBefore && pending.count==pendingBefore,"ordinary capped draw keeps the raw early-return fast path");
    check(comparison.metrics.entryCalls==1 && comparison.metrics.acceptedDraws==0,"comparison full-entry gate counts capped calls without claiming accepted work");
    comparison={};
    check(diagnostics.drawCpuSamples>=2,"mesh draw CPU timing is sampled rather than measured on every draw");
    check(pending.count==512 && captureBatches==1,"full eye queues a bounded batch after the preceding frame");
    ID3D11ShaderResourceView* batchViews[2]{};meshMotionViews(ctx.Get(),scene[0].Get(),batchViews);
    a=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get());
    check(!pending.count && captureBatches==2 && a[511*60+56]==1,"one dispatch captures all 512 records including later thread groups");
    check(diagnostics.flushes[unsigned(FlushReason::EyeConsumption)]==1 && diagnostics.largestBatch==512,"eye consumption reports one bounded 512-record batch");
    auto queue=[&](UINT first=0,UINT n=1){issue(ctx.Get(),6,n,0,0,first);meshMotionDraw(ctx.Get(),issue,6,n,0,0,first,materialVs);};
    auto consumeBatch=[&](){meshMotionViews(ctx.Get(),scene[testEye].Get(),batchViews);return readBuffer(dev.Get(),ctx.Get(),eyes[testEye].history[eyes[testEye].write].buffer.Get());};
    // A write to the same pool must finish earlier captures before the
    // original bytes are replaced, without an extra GPU readback or wait.
    reset();pose(1,.1f);bind(0);queue();
    meshMotionResourceWritten(pool.Get());pose(1,.3f);ctx->UpdateSubresource(pool.Get(),0,nullptr,poolData,0,0);queue();a=consumeBatch();
    check(captureBatches==2 && std::fabs(a[35]-.1f)<1e-5 && std::fabs(a[95]-.3f)<1e-5,"pool rewrite preserves both draw-time poses");
    check(std::fabs(a[23]-.1f)<1e-6 && std::fabs(a[83]-.3f)<1e-6 && diagnostics.flushes[unsigned(FlushReason::WriteOrMap)]==1,"pool rewrite preserves both raw poses and reports write/map flush");
    reset();pose(1,.1f);pose(1,.7f,1);bind(0);queue();
    meshMotionResourceWritten(iv.Get());ids[0]=1;ctx->UpdateSubresource(iv.Get(),0,nullptr,ids,0,0);queue();a=consumeBatch();
    check(captureBatches==2 && std::fabs(a[35]-.1f)<1e-5 && std::fabs(a[95]-.7f)<1e-5,"instance rewrite refreshes the snapshot after preserving earlier IDs");
    // Map flushing happens BEFORE the real Map: the shader must never be
    // queued against an already-mapped scene constant buffer.
    reset();pose(1,.1f);bind(0);
    D3D11_BUFFER_DESC dynamicDesc{};dynamicDesc.ByteWidth=sizeof(sceneData);dynamicDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    dynamicDesc.Usage=D3D11_USAGE_DYNAMIC;dynamicDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA sceneInit{};sceneInit.pSysMem=sceneData;ComPtr<ID3D11Buffer> mappedScene;
    hr(dev->CreateBuffer(&dynamicDesc,&sceneInit,&mappedScene));ctx->VSSetConstantBuffers(1,1,mappedScene.GetAddressOf());queue();
    meshMotionBeforeMap(mappedScene.Get());check(!pending.count,"capture submitted before a source becomes mapped");
    D3D11_MAPPED_SUBRESOURCE mappedSceneData{};hr(ctx->Map(mappedScene.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mappedSceneData));
    sceneData[275][0]=1;std::memcpy(mappedSceneData.pData,sceneData,sizeof(sceneData));ctx->Unmap(mappedScene.Get(),0);queue();a=consumeBatch();
    check(std::fabs(a[35]-.1f)<1e-5 && std::fabs(a[95]+.9f)<1e-5,"source Map/Unmap cannot move an earlier draw's camera");
    // Switching eyes with unchanged sources still separates output batches.
    reset();pose(1,.2f);bind(0);queue();testEye=1;testScene=scene[1].Get();testDepth=dsv[1].Get();
    ctx->OMSetRenderTargets(0,nullptr,dsv[1].Get());ctx->ClearDepthStencilView(dsv[1].Get(),D3D11_CLEAR_DEPTH,0,0);queue();
    check(captureBatches==1 && pending.count==1,"eye switch flushes before the shared ID snapshot is reused");a=consumeBatch();
    auto firstEye=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get());
    check(captureBatches==2 && a[56]==1 && firstEye[56]==1,"both eyes receive their own queued records");
    check(diagnostics.flushes[unsigned(FlushReason::InputChange)]==1 && diagnostics.flushes[unsigned(FlushReason::EyeConsumption)]==1,"eye switch and eye consumption have exclusive flush reasons");
    check(diagnostics.inputChanges[unsigned(InputChange::Output)]==1 && diagnostics.inputChanges[unsigned(InputChange::Context)]==0 && diagnostics.inputChanges[unsigned(InputChange::Scene)]==0 && diagnostics.inputChanges[unsigned(InputChange::Ids)]==0 && diagnostics.inputChanges[unsigned(InputChange::Pool)]==0 && diagnostics.inputChanges[unsigned(InputChange::Oversized)]==0,"input-change attribution isolates an eye output switch");
    // Cause counters overlap when one actual flush changes several inputs.
    reset();pose(1,.2f);bind(0);queue();
    auto alternateIds=buffer(sizeof(ids),D3D11_BIND_VERTEX_BUFFER,0,ids);
    auto alternatePool=buffer(sizeof(poolData),D3D11_BIND_SHADER_RESOURCE,336,poolData);ComPtr<ID3D11ShaderResourceView> alternatePoolView;
    hr(dev->CreateShaderResourceView(alternatePool.Get(),nullptr,&alternatePoolView));UINT alternateStep=8,alternateOffset=0;
    ctx->IASetVertexBuffers(0,1,alternateIds.GetAddressOf(),&alternateStep,&alternateOffset);ctx->VSSetShaderResources(33,1,alternatePoolView.GetAddressOf());queue();a=consumeBatch();
    check(diagnostics.flushes[unsigned(FlushReason::InputChange)]==1 && diagnostics.inputChanges[unsigned(InputChange::Ids)]==1 && diagnostics.inputChanges[unsigned(InputChange::Pool)]==1,"one input flush attributes both changed ID and pool inputs");
    // GPU-writable pools cannot rely on CPU write notifications.
    reset();pose(1,.2f);bind(0);
    auto gpuPool=buffer(sizeof(poolData),D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,336,poolData);
    ComPtr<ID3D11ShaderResourceView> gpuPoolView;hr(dev->CreateShaderResourceView(gpuPool.Get(),nullptr,&gpuPoolView));
    ctx->VSSetShaderResources(33,1,gpuPoolView.GetAddressOf());queue();
    check(!pending.count && captureBatches==1,"UAV-writable pool remains immediate");a=consumeBatch();check(a[56]==1,"GPU-writable pool still captures correct motion inputs");
    check(diagnostics.flushes[unsigned(FlushReason::GpuWritable)]==1,"GPU-writable capture reports its own flush reason");
    // Very large instance streams append only each draw's IDs to the owned
    // buffer. Distinct far offsets must share one capture batch while both
    // the deferred capture and the immediate coverage draw read their own
    // compact destination offsets.
    std::vector<UINT> largeIds(maxInstanceBytes/4+4*maxRecords);const UINT farA=maxInstanceBytes/8+17,farB=farA+129;
    largeIds[farA*2]=2;largeIds[farB*2]=3;
    auto largeStream=buffer(UINT(largeIds.size()*4),D3D11_BIND_VERTEX_BUFFER,0,largeIds.data());UINT idStep=8,idStart=0;
    reset();check(comparison.mode==normalOversizedMode,"normal oversized capture defaults to packed batching");pose(1,0,2);pose(1,0,3);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
    ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);
    check(!captureBatches && pending.count==1,"normal oversized path retains its first bounded range until a real boundary");
    queue(farB);
    check(!captureBatches && pending.count==2,"normal oversized path packs distinct ranged source offsets into one batch");
    a=consumeBatch();
    check(captureBatches==1 && word(a,31)==2 && word(a,91)==3,"normal packed oversized path preserves each source offset and ID");
    check(diagnostics.flushes[unsigned(FlushReason::InputChange)]==0 && diagnostics.inputChanges[unsigned(InputChange::Oversized)]==0 && diagnostics.rangedIdCopies==2 && diagnostics.rangedIdInstances==2,"normal packed path removes size-only flushes while preserving bounded-copy diagnostics");
    check(comparison.metrics.copyGpuSubmitted==0 && comparisonCopyGpu.totals.samples==0 && comparisonCopyGpu.totals.skipped==0,"unarmed rendering never opens comparison GPU queries");

    // Phase A remains the prior immediate policy even though normal runtime is
    // packed. It must retain the old per-draw boundary for a controlled A/B/A.
    reset();comparison.phase=0;beginComparisonPhase(ctx.Get(),GetTickCount64());pose(1,0,2);pose(1,0,3);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
    ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);queue(farB);
    check(captureBatches==1 && pending.count==1 && comparison.mode==OversizedMode::Immediate,"comparison A phase retains the prior immediate oversized boundary");
    a=consumeBatch();check(captureBatches==2 && word(a,31)==2 && word(a,91)==3,"comparison A phase preserves both immediate ranged captures");

    // The comparison gate must exercise the real WARP command path. One
    // sampled oversized draw covers full-entry early return, accepted work,
    // bounded ID copy, coverage reissue and capture dispatch timing.
    reset();pose(1,0,2);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
    ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);
    beginComparisonPhase(ctx.Get(),GetTickCount64());meshMotionComparisonNativeSampling(true,901,902);
    diagnostics.drawCalls=255;meshMotionDraw(nullptr,issue,6,1,0,0,0,materialVs);
    comparison.metrics.idCopies=63;comparison.metrics.acceptedDraws=63;draws=63;diagnostics.flushCalls=15;
    diagnostics.drawCalls=511;
    queue(farA);a=consumeBatch();
    for(unsigned i=0;i<5;++i)meshMotionFrameBoundary(ctx.Get());
    check(comparison.metrics.entryCpuSamples==2 && comparison.metrics.acceptedCpuSamples==1 && comparison.metrics.idCopyCpuSamples==1 && comparison.metrics.flushCpuSamples==1,"Sampling-only gate reaches rejected and completed full-entry samples plus accepted, ID-copy and flush CPU samplers");
    check(comparison.metrics.admission.completed==1 && comparison.metrics.admission.stages[unsigned(AdmissionStage::Fast)].rejected==1 && comparison.metrics.admission.stages[unsigned(AdmissionStage::Accepted)].cpuSamples==1,"comparison phase accumulates exclusive rejection and completed-path admission spans");
    check(comparison.metrics.descriptors.kinds[unsigned(DescriptorKind::Depth)].calls==1 && comparison.metrics.descriptors.kinds[unsigned(DescriptorKind::Ids)].calls==1 && comparison.metrics.descriptors.kinds[unsigned(DescriptorKind::PoolSrv)].calls==1,"comparison phase accumulates actual descriptor getter observations");
    check(comparison.metrics.oversizedDraws==1 && comparison.metrics.copyGpuSubmitted==1 && comparison.metrics.coverageGpuSubmitted==1 && comparison.metrics.flushGpuSubmitted==1,"sampled WARP draw submits every comparison GPU interval");
    check(comparisonCopyGpu.totals.samples==1 && comparisonCoverageGpu.totals.samples==1 && comparisonFlushGpu.totals.samples==1,"sampled WARP comparison GPU intervals retire naturally after frame polling");
    const auto gatedEntries=comparison.metrics.entryCalls;meshMotionComparisonNativeSampling(false,901,902);meshMotionDraw(nullptr,issue,6,1,0,0,0,materialVs);
    check(comparison.metrics.entryCalls==gatedEntries,"drain/inactive callbacks close component collection exactly at the Sampling boundary");

    reset();pose(1,0,2);pose(1,0,3);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
    ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);queue(farB);
    check(!captureBatches && pending.count==2,"oversized draws from distinct far offsets accumulate one batch");a=consumeBatch();
    check(captureBatches==1 && word(a,31)==2 && word(a,91)==3 && a[56]==1 && a[116]==1,"batched oversized draws retain distinct IDs and exact raw poses");
    auto largeCoverage=readTexture(dev.Get(),ctx.Get(),eyes[0].coverage.Get()),largeDepth=readTexture(dev.Get(),ctx.Get(),scene[0].Get());const UINT centre=32*W+32;
    check(largeCoverage[centre*2]==2 && largeCoverage[centre*2+1]==largeDepth[centre],"later oversized draw writes its exact coverage record ID and depth");
    check(diagnostics.flushes[unsigned(FlushReason::InputChange)]==0 && diagnostics.inputChanges[unsigned(InputChange::Oversized)]==0 && diagnostics.rangedIdCopies==2 && diagnostics.rangedIdInstances==2,"oversized batching uses bounded copies without a size-only flush");

    // The compact destination reaches exactly 4096 bytes at the 512-record
    // cap and the deferred shader must consume every thread group.
    reset();for(UINT i=0;i<4;++i)pose(1,float(i)*.05f,i);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
    std::vector<UINT> bulkIds(maxInstanceBytes/4+2*maxRecords);const UINT bulkFirst=maxInstanceBytes/8;
    for(UINT i=0;i<maxRecords;++i)bulkIds[(bulkFirst+i)*2]=i&3;
    auto bulkStream=buffer(UINT(bulkIds.size()*4),D3D11_BIND_VERTEX_BUFFER,0,bulkIds.data());ctx->IASetVertexBuffers(0,1,bulkStream.GetAddressOf(),&idStep,&idStart);
    for(UINT i=0;i<maxRecords;++i)queue(bulkFirst+i);
    check(!captureBatches && pending.count==maxRecords && diagnostics.rangedIdCopies==maxRecords && diagnostics.rangedIdInstances==maxRecords,"oversized compact offsets stay bounded through 512 queued records");a=consumeBatch();
    check(captureBatches==1 && a[(maxRecords-1)*60+56]==1 && word(a,31)==0 && word(a,(maxRecords-1)*60+31)==3 && diagnostics.largestBatch==maxRecords,"oversized 512-record batch captures later shader thread groups");

    // Switching between ranged and whole snapshots retains the ID resource
    // as a real flush boundary, so neither mode can consume the other's bytes.
    reset();pose(1,.1f,0);pose(1,.3f,2);pose(1,.4f,3);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
    ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);
    ctx->IASetVertexBuffers(0,1,iv.GetAddressOf(),&idStep,&idStart);queue();
    ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farB);a=consumeBatch();
    check(captureBatches==3 && word(a,31)==2 && word(a,91)==0 && word(a,151)==3 && std::fabs(a[23]-.3f)<1e-6 && std::fabs(a[83]-.1f)<1e-6 && std::fabs(a[143]-.4f)<1e-6,"large-small-large transitions preserve draw-time IDs and poses");
    check(diagnostics.flushes[unsigned(FlushReason::InputChange)]==2 && diagnostics.inputChanges[unsigned(InputChange::Ids)]==2 && diagnostics.inputChanges[unsigned(InputChange::Oversized)]==0,"large-small-large transitions attribute only real ID changes");

    // A source rewrite still submits the ranged batch before the original ID
    // bytes change; the next draw receives a fresh compact copy.
    reset();pose(1,.2f,2);pose(1,.7f,3);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
    largeIds[farA*2]=2;ctx->UpdateSubresource(largeStream.Get(),0,nullptr,largeIds.data(),0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);
    meshMotionResourceWritten(largeStream.Get());largeIds[farA*2]=3;ctx->UpdateSubresource(largeStream.Get(),0,nullptr,largeIds.data(),0,0);queue(farA);a=consumeBatch();
    check(captureBatches==2 && word(a,31)==2 && word(a,91)==3 && std::fabs(a[23]-.2f)<1e-6 && std::fabs(a[83]-.7f)<1e-6 && diagnostics.flushes[unsigned(FlushReason::WriteOrMap)]==1,"oversized ID rewrite preserves both draw-time transforms");

    // Pool writes and eye changes keep their existing boundaries with ranged
    // IDs, while a GPU-writable source remains immediate.
    reset();pose(1,.1f,2);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);largeIds[farA*2]=2;ctx->UpdateSubresource(largeStream.Get(),0,nullptr,largeIds.data(),0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);
    meshMotionResourceWritten(pool.Get());pose(1,.6f,2);ctx->UpdateSubresource(pool.Get(),0,nullptr,poolData,0,0);queue(farA);a=consumeBatch();
    check(captureBatches==2 && std::fabs(a[23]-.1f)<1e-6 && std::fabs(a[83]-.6f)<1e-6 && diagnostics.flushes[unsigned(FlushReason::WriteOrMap)]==1,"oversized pool rewrite preserves both draw-time poses");
    reset();pose(1,.2f,2);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);
    testEye=1;testScene=scene[1].Get();testDepth=dsv[1].Get();ctx->OMSetRenderTargets(0,nullptr,dsv[1].Get());ctx->ClearDepthStencilView(dsv[1].Get(),D3D11_CLEAR_DEPTH,0,0);queue(farA);a=consumeBatch();
    auto largeFirstEye=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get());
    check(captureBatches==2 && a[56]==1 && largeFirstEye[56]==1 && std::fabs(a[23]-.2f)<1e-6 && std::fabs(largeFirstEye[23]-.2f)<1e-6 && diagnostics.inputChanges[unsigned(InputChange::Output)]==1,"oversized eye switch preserves both output batches and draw-time poses");
    reset();pose(1,.2f);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);const UINT farGpu=farA+1;
    ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);ctx->VSSetShaderResources(33,1,gpuPoolView.GetAddressOf());queue(farGpu);
    check(!pending.count && captureBatches==1 && diagnostics.rangedIdCopies==1 && diagnostics.flushes[unsigned(FlushReason::GpuWritable)]==1,"GPU-writable oversized capture remains immediate");a=consumeBatch();check(a[56]==1,"GPU-writable oversized capture retains the ranged ID");
    // Normal capture timing admits only the first eligible flush in a selected
    // frame. Many immediate safety flushes must not create per-batch queries.
    reset();pose(1,.2f);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);ctx->VSSetShaderResources(33,1,gpuPoolView.GetAddressOf());
    for(unsigned i=0;i<8;++i)queue(farGpu);
    check(captureBatches==8 && normalCaptureGpuFrame==frameStamp && captureGpu.totals.skipped==0,"many capture batches admit one normal GPU query in a selected frame");
    a=consumeBatch(); // test-only readback lets the asynchronous timer retire
    for(unsigned i=0;i<5;++i)meshMotionFrameBoundary(ctx.Get());
    check(captureGpu.totals.samples==1 && captureGpu.totals.skipped==0,"the selected normal capture query retires without per-batch skips before the next selected frame");
    while(frameStamp<16)meshMotionFrameBoundary(ctx.Get());
    bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);ctx->VSSetShaderResources(33,1,gpuPoolView.GetAddressOf());
    for(unsigned i=0;i<8;++i)queue(farGpu);
    check(normalCaptureGpuFrame==16 && captureGpu.totals.samples==1 && captureGpu.totals.skipped==0,"frame 16 admits exactly the next normal first-flush query");
    a=consumeBatch(); // test-only synchronization, never used by production
    for(unsigned i=0;i<5;++i)meshMotionFrameBoundary(ctx.Get());
    check(captureGpu.totals.samples==2 && captureGpu.totals.skipped==0,"normal capture timing remains bounded across selected frames");
    // The 1800-frame diagnostics report clears window counters, not the
    // monotonic sampling stamp or in-flight timer ownership.
    frames=1799;frameStamp=1792;bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);ctx->VSSetShaderResources(33,1,gpuPoolView.GetAddressOf());
    for(unsigned i=0;i<8;++i)queue(farGpu);
    check(normalCaptureGpuFrame==1792 && captureGpu.totals.samples==2 && captureGpu.totals.skipped==0,"the diagnostic-window boundary admits only one normal query for its selected frame");
    a=consumeBatch();for(unsigned i=0;i<5;++i)meshMotionFrameBoundary(ctx.Get());
    check(frames>1800 && captureGpu.totals.samples==3 && captureGpu.totals.skipped==0,"a normal capture query retires across the 1800-frame diagnostics boundary");
    while(frameStamp<1808)meshMotionFrameBoundary(ctx.Get());
    bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);ctx->VSSetShaderResources(33,1,gpuPoolView.GetAddressOf());
    for(unsigned i=0;i<8;++i)queue(farGpu);
    check(normalCaptureGpuFrame==1808 && captureGpu.totals.samples==3 && captureGpu.totals.skipped==0,"the next monotonic selected frame admits exactly one normal query after diagnostics reporting");
    a=consumeBatch();for(unsigned i=0;i<5;++i)meshMotionFrameBoundary(ctx.Get());
    check(captureGpu.totals.samples==4 && captureGpu.totals.skipped==0,"normal capture timing remains bounded after the diagnostics boundary");
    reset();pose(1,.2f);bind(0);queue();meshMotionFrameBoundary(ctx.Get());
    check(!pending.count && diagnostics.flushes[unsigned(FlushReason::FrameBoundary)]==1,"frame boundary submits pending capture with an exclusive reason");
    // Deferred capture may run after the app enabled predication. Captures,
    // uploads and owned timestamps must execute, then restore that state.
    reset();pose(1,.25f);bind(0);queue();
    D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> occluded;
    hr(dev->CreatePredicate(&predicateDesc,&occluded));ctx->Begin(occluded.Get());ctx->End(occluded.Get());
    ctx->SetPredication(occluded.Get(),FALSE);ctx->CSSetShaderResources(3,1,poolView.GetAddressOf());
    meshMotionViews(ctx.Get(),scene[0].Get(),batchViews);
    ComPtr<ID3D11Predicate> restoredPredicate;BOOL restoredValue=TRUE;ctx->GetPredication(&restoredPredicate,&restoredValue);
    ComPtr<ID3D11ShaderResourceView> restoredInput;ctx->CSGetShaderResources(3,1,&restoredInput);
    check(restoredPredicate==occluded && !restoredValue && restoredInput==poolView,"deferred compute restores predication and CS input slot 3");
    ctx->SetPredication(nullptr,FALSE);a=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get());
    check(a[56]==1 && std::fabs(a[35]-.25f)<1e-5,"predication cannot suppress the capture upload or dispatch");
    reset();pose(1,.1f);bind(0);queue();meshMotionResourceWritten(nullptr);
    check(!pending.count && eyes[0].history[eyes[0].write].count==0,"unknown command-list writes flush then discard pending history");
    // Each material uses the original output register order. In particular
    // the lit multi-UV hull puts SV_POSITION in register 6, not register 4.
    const char* outputs[]={
        "uint id:__USER_VERTEX_FACEINVARIANT;float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;float2 uv:__USER_VERTEX_M_TEXCOORD;float4 pos:SV_Position;",
        "uint id:__USER_VERTEX_FACEINVARIANT;float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;float4 uv:__USER_VERTEX_M_TEXCOORD;float4 pos:SV_Position;",
        "uint id:__USER_VERTEX_FACEINVARIANT;float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;float3 position:__USER_VERTEX_M_LIGHTINGPOSITION;float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;float4 uv:__USER_VERTEX_M_TEXCOORD;float2 uv2:__USER_VERTEX_M_TEXCOORD2;float4 pos:SV_Position;",
        "uint2 id:__USER_VERTEX_FACEINVARIANT;float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;float2 uv:__USER_VERTEX_M_TEXCOORD;float4 pos:SV_Position;"};
    uint64_t hashes[]={faceVs,multiUvVs,litMultiUvVs,detailVs};auto originalVs=vs;
    for(unsigned i=0;i<4;++i){
        std::string source=shader;auto from=source.find("struct O{"),to=source.find("};",from);
        source.replace(from+9,to-from-9,outputs[i]);from=source.find("uint3(0,id.x,0)");source.replace(from,15,i==3?"uint2(id.x,0)":"id.x");
        auto shaderCode=compile(source.c_str(),"vs_5_0");vs.Reset();hr(dev->CreateVertexShader(shaderCode->GetBufferPointer(),shaderCode->GetBufferSize(),nullptr,&vs));testHash=hashes[i];
        reset();pose(12,0);run();meshMotionFrameBoundary(ctx.Get());pose(12,.24f);a=run();check(a[59]==1,"material family matches exact rigid motion");
        auto cov=readTexture(dev.Get(),ctx.Get(),eyes[0].coverage.Get()),dep=readTexture(dev.Get(),ctx.Get(),scene[0].Get());unsigned visible=0,covered=0;
        for(unsigned p=0;p<W*H;++p)if(dep[p]>0){++visible;covered+=cov[2*p]==1 && cov[2*p+1]==dep[p];}
        check(visible>0 && visible==covered,"every material raster sample retains its exact visible depth and record ID");
    }
    vs=originalVs;testHash=materialVs;
    // The consumer must obey exact scene depth, UI coverage, frame history
    // and raster jitter in both DLSS and native TAA coordinate grids.
    std::filesystem::create_directories("build/obj/meshmotion");
    auto dumpHeaderAt=[&](const char* path){std::ifstream file(path,std::ios::binary);char magic[8]{};UINT h[2]{};file.read(magic,8);file.read(reinterpret_cast<char*>(h),8);check(bool(file) && !std::memcmp(magic,"EDVRMSH1",8) && h[1]==240,"paired dump retains EDVRMSH1 record format");return h[0];};
    auto dumpText=[&](const char* path){std::ifstream file(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(file)),{});};
    reset();pose(1,0);a=run();meshMotionStageDump(ctx.Get(),scene[0].Get(),76);
    readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get());
    meshMotionWriteDump(ctx.Get(),L"build/obj/meshmotion",L"zero_prev");
    check(dumpHeaderAt("build/obj/meshmotion/eye_zero_prev_Mesh.bin")==1 && dumpHeaderAt("build/obj/meshmotion/eye_zero_prev_MeshPrev.bin")==0,"first-frame pair writes an explicit empty previous history");
    auto zeroMarker=dumpText("build/obj/meshmotion/eye_zero_prev_MeshProbe.json");
    check(zeroMarker.find("\"frame\": 76")!=std::string::npos && zeroMarker.find("\"meshFrame\": 0")!=std::string::npos && zeroMarker.find("\"previous\": {\"file\": \"eye_zero_prev_MeshPrev.bin\", \"count\": 0}")!=std::string::npos,"zero-history marker identifies cross-file frame and explicit previous count");
    meshMotionFrameBoundary(ctx.Get());pose(1,.02f);a=run();meshMotionStageDump(ctx.Get(),scene[0].Get(),77);
    readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get()); // test-only readback completes the staged copy
    meshMotionWriteDump(ctx.Get(),L"build/obj/meshmotion",L"fixture");
    check(dumpHeaderAt("build/obj/meshmotion/eye_fixture_Mesh.bin")==1 && dumpHeaderAt("build/obj/meshmotion/eye_fixture_MeshPrev.bin")==1,"explicit eye dump writes paired current and previous records");
    auto marker=dumpText("build/obj/meshmotion/eye_fixture_MeshProbe.json");
    check(marker.find("\"frame\": 77")!=std::string::npos && marker.find("\"meshFrame\": 1")!=std::string::npos && marker.find("\"current\": {\"file\": \"eye_fixture_Mesh.bin\", \"count\": 1}")!=std::string::npos && marker.find("\"previous\": {\"file\": \"eye_fixture_MeshPrev.bin\", \"count\": 1}")!=std::string::npos,"paired marker names compatible files and both record counts");
    check(std::system("python tools\\mesh_motion_probe.py --mesh build\\obj\\meshmotion\\eye_fixture_Mesh.bin --previous build\\obj\\meshmotion\\eye_fixture_MeshPrev.bin --probe build\\obj\\meshmotion\\eye_fixture_MeshProbe.json --json")==0,"production exporter is accepted by the offline mesh probe");
    std::ifstream header("src/d3d11/temporal_shader_source.h");std::string text((std::istreambuf_iterator<char>(header)),{});auto begin=text.find("bool meshPixel("),end=text.find("\n}\n",begin);check(begin!=std::string::npos && end!=std::string::npos,"production mesh consumer located");
    std::string consumer="Texture2D<float2> MC:register(t15);struct HoloRecord{uint4 key[8];float4 clip[3];float4 map[3];float4 meta;};StructuredBuffer<HoloRecord> MR:register(t16);Texture2D<float> Z:register(t0);RWTexture2D<float4> Out:register(u0);cbuffer P:register(b0){float4 holoJitter;float4 offset;}static const int4 region=0;static const int2 size=int2(64,64);static const float4 knobs=float4(0,1,.025,0);bool uiCovered(int2 q){return q.x==32 && q.y==31;}float zSceneAt(int2 q){return q.x==32 && q.y==30 ? .1 : Z.Load(int3(q,0));}\n";
    consumer+=text.substr(begin,end+3-begin);consumer+="[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){float2 pp;float zp;bool ok=meshPixel(id.xy,offset.xy,pp,zp);Out[id.xy]=float4(pp-id.xy,zp,ok?1:0);}";
    auto shaderCode=compile(consumer.c_str(),"cs_5_0");ComPtr<ID3D11ComputeShader> cs;hr(dev->CreateComputeShader(shaderCode->GetBufferPointer(),shaderCode->GetBufferSize(),nullptr,&cs));
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11UnorderedAccessView> uav;hr(dev->CreateTexture2D(&td,nullptr,&output));hr(dev->CreateUnorderedAccessView(output.Get(),nullptr,&uav));auto params=buffer(32,D3D11_BIND_CONSTANT_BUFFER);float parameters[8]={.25f,-.125f,1,0,0,0,0,0};
    auto consume=[&](){ctx->ClearState();ctx->UpdateSubresource(params.Get(),0,nullptr,parameters,0,0);ctx->CSSetShader(cs.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,1,params.GetAddressOf());ctx->CSSetShaderResources(0,1,z[0].GetAddressOf());ID3D11ShaderResourceView* views[2]{};meshMotionViews(ctx.Get(),scene[0].Get(),views);ctx->CSSetShaderResources(15,2,views);ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);ctx->Dispatch(8,8,1);auto pixels=readTexture(dev.Get(),ctx.Get(),output.Get());ctx->ClearState();return pixels;};
    auto pixels=consume();auto at=(32*W+32)*4;check(pixels[at+3]==1 && std::fabs(pixels[at]+.39f)<1e-5 && std::fabs(pixels[at+1]+.125f)<1e-5,"DLSS consumes exact motion and removes jitter");
    check(pixels[(30*W+32)*4+3]==0 && pixels[(31*W+32)*4+3]==0,"foreground and UI reject hull motion");
    parameters[4]=.25f;pixels=consume();check(pixels[at+3]==1 && std::fabs(pixels[at]+.39f)<1e-5,"native TAA grid returns same physical motion");parameters[2]=0;pixels=consume();check(pixels[at+3]==0,"nonconsecutive frame rejects mesh history");

    // Exercise the actual admission -> nomination -> temporal-consumption
    // wiring. These updates bypass vscreen in the rig, so feed the same
    // post-forward write notifications that its hooks supply in production.
    objectClassificationProbe.reset();reset();pose(1,0);run();
    check(!objectClassificationProbe.active() && objectClassificationProbe.drawCount()==0,
          "unarmed rendering does not nominate classification sources");
    reset();pose(1,0);meshMotionArmClassification();
    for(unsigned captureFrame=0;captureFrame<4;++captureFrame){
        if(captureFrame)meshMotionFrameBoundary(ctx.Get());
        bind(0);
        objectClassificationProbe.noteWrite(ctx.Get(),pool.Get(),ObjectClassificationProbe::Update);
        objectClassificationProbe.noteWrite(ctx.Get(),iv.Get(),ObjectClassificationProbe::Update);
        ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);
        issue(ctx.Get(),6,1,0,0,0);meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
        meshMotionStageClassification(ctx.Get(),scene[0].Get(),10000+captureFrame,nullptr);
        check(!objectClassificationProbe.sealed(),"classification cannot consume unmatched mesh records");
        ID3D11ShaderResourceView* classifierViews[2]{};
        meshMotionViews(ctx.Get(),scene[0].Get(),classifierViews);
        meshMotionStageClassification(ctx.Get(),scene[0].Get(),10000+captureFrame,nullptr);
        check(eyes[0].history[eyes[0].write].count==1 && classifierViews[0] && classifierViews[1],
              "classification preserves original motion admission and views");
        if(captureFrame<3)check(!objectClassificationProbe.sealed(),"discovery frames cannot seal a partial snapshot");
    }
    const auto classifierSummary=objectClassificationProbe.summary();
    check(classifierSummary.sealed && !classifierSummary.active && classifierSummary.selectedFrame==3 &&
          classifierSummary.sceneFrame==10003 && classifierSummary.draws==1 && classifierSummary.writes>=6,
          "classification stage joins exact scene frame to the later complete mesh eye");
    check(objectClassificationProbe.write(ctx.Get(),L"build/obj/meshmotion",L"pipeline"),
          "production mesh classification evidence writes successfully");
    check(std::system("python tools\\object_classification.py build\\obj\\meshmotion\\classification_pipeline.json --verify-pipeline")==0,
          "offline reader joins visible production coverage to the same source generation and upload");
    objectClassificationProbe.reset();

    // Arming is asynchronous. A pending normal batch must finish under the
    // packed policy before the first A phase switches to immediate mode.
    reset();pose(1,0,2);pose(1,0,3);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);
    queue(farA);testMenuOpen=true;meshMotionRequestComparison();queue(farB);
    check(comparison.stage==ComparisonStage::Armed && comparison.mode==normalOversizedMode && pending.count==2 && !captureBatches,"arming preserves the pending normal packed batch");
    meshMotionFrameBoundary(ctx.Get());check(comparison.stage==ComparisonStage::Armed && comparison.mode==normalOversizedMode && captureBatches==1,"the boundary flushes the normal packed batch before any A-phase switch");
    meshMotionRequestComparison();meshMotionFrameBoundary(ctx.Get());testMenuOpen=false;
    check(comparison.stage==ComparisonStage::Idle && comparison.mode==normalOversizedMode,"cancelling an armed comparison restores normal packed capture");

    // Comparison policy is driven only by a matching completed native report.
    // Keep these tests at the boundary/API level so they cover association,
    // retries, cancellation and restoration without inventing menu events.
    auto metadata=[&](UINT width=2481){NativeBenchmarkMetadata m{};m.inputWidth[0]=m.inputWidth[1]=width;m.inputHeight[0]=m.inputHeight[1]=2121;m.outputWidth[0]=m.outputWidth[1]=3072;m.outputHeight[0]=m.outputHeight[1]=3264;m.refreshMilliHz=90000;m.gameFov[0][0]=m.gameFov[1][0]=1.1f;m.treatments[0]=m.treatments[1]=3;m.featureEpoch=7;strcpy_s(m.runtime,"OpenXR");strcpy_s(m.headset,"VirtualDesktopXR");strcpy_s(m.aaMode,"DLSS");strcpy_s(m.dlssMode,"Quality");strcpy_s(m.build,"test");return m;};
    auto report=[&](uint64_t window,uint64_t scope,NativeBenchmarkMetadata meta=NativeBenchmarkMetadata{}){NativeBenchmarkReport r{};r.window=window;r.scope=scope;r.complete=true;r.abortReason=kNativeBenchmarkCompleted;r.cpu.available=r.gpu.available=true;r.metadata=meta;return r;};
    auto seedCompleteMetrics=[&](){auto& m=comparison.metrics;m.entryCpuSamples=m.acceptedCpuSamples=m.idCopyCpuSamples=m.flushCpuSamples=1;m.oversizedDraws=m.oversizedInstances=1;m.copyGpuSubmitted=m.coverageGpuSubmitted=m.flushGpuSubmitted=1;comparisonCopyGpu.totals={.01,1,0,0};comparisonCoverageGpu.totals={.02,1,0,0};comparisonFlushGpu.totals={.03,1,0,0};};
    auto armToRunning=[&](){reset();testMenuOpen=true;meshMotionRequestComparison();processComparisonBoundary(ctx.Get());testMenuOpen=false;processComparisonBoundary(ctx.Get());check(comparison.stage==ComparisonStage::Settling && comparison.mode==normalOversizedMode,"settling retains normal packed capture");comparison.settleUntilMs=0;processComparisonBoundary(ctx.Get());check(comparison.stage==ComparisonStage::Running && comparison.phase==0 && comparison.mode==OversizedMode::Immediate,"comparison starts A1 in prior immediate mode after menu close and settling");};
    auto expectRestored=[&](const char* why){check(comparison.stage==ComparisonStage::Idle && comparison.mode==normalOversizedMode && !comparison.measurement && !comparison.nativeSampling,why);};

    reset();testMenuOpen=true;meshMotionRequestComparison();
    check(comparison.mode==normalOversizedMode,"arming leaves normal packed capture active while the menu is open");
    auto stale=report(7,70,metadata());meshMotionComparisonNativeSampling(true,7,70);meshMotionComparisonNativeReport(stale);processComparisonBoundary(ctx.Get());
    check(comparison.stage==ComparisonStage::Armed && !comparison.nativeScope && !comparison.haveReport,"pre-phase callbacks cannot associate with or advance an armed comparison");
    testMenuOpen=false;processComparisonBoundary(ctx.Get());check(comparison.stage==ComparisonStage::Settling,"closing the menu snapshots the environment and enters settling");
    ++testBenchmarkDisturbanceEpoch;processComparisonBoundary(ctx.Get());expectRestored("a census, eye dump, setting or menu disturbance during settling aborts and restores baseline");

    armToRunning();
    meshMotionComparisonNativeSampling(false,10,100);check(!comparison.nativeScope && !comparison.nativeWindow,"inactive warmup tuples do not latch a phase identity");
    meshMotionComparisonNativeSampling(true,11,101);check(comparison.nativeScope==101 && comparison.nativeWindow==11 && comparison.measurement,"the first Sampling tuple authoritatively latches phase identity");
    meshMotionComparisonNativeReport(stale);check(!comparison.haveReport,"an older window/scope report cannot advance the current phase");
    seedCompleteMetrics();auto a1=report(11,101,metadata());meshMotionComparisonNativeReport(a1);processComparisonBoundary(ctx.Get());
    check(comparison.stage==ComparisonStage::Running && comparison.phase==1 && comparison.mode==OversizedMode::Packed,"matching completed A1 advances to packed phase B");
    check(!comparison.nativeScope && comparisonCopyGpu.totals.samples==0 && comparison.metrics.entryCpuSamples==0,"a new phase clears association and component metrics so late completions cannot be mislabeled");
    meshMotionComparisonNativeReport(a1);check(!comparison.haveReport && !comparison.nativeScope,"the completed A1 report is ignored after phase B starts");
    meshMotionComparisonNativeSampling(true,12,102);seedCompleteMetrics();auto b=report(12,102,metadata());meshMotionComparisonNativeReport(b);processComparisonBoundary(ctx.Get());
    check(comparison.phase==2 && comparison.mode==OversizedMode::Immediate,"matching completed B advances to restored A2 mode");
    meshMotionComparisonNativeSampling(true,13,103);seedCompleteMetrics();auto a2=report(13,103,metadata());meshMotionComparisonNativeReport(a2);processComparisonBoundary(ctx.Get());expectRestored("matching completed A2 finishes A/B/A and restores baseline");

    // Public frame boundaries must flush each phase's queued capture before
    // applying the next mode, including completion back to normal packed mode.
    armToRunning();pose(1,0,2);pose(1,0,3);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);
    meshMotionComparisonNativeSampling(true,60,600);seedCompleteMetrics();auto pendingA1=report(60,600,metadata());meshMotionComparisonNativeReport(pendingA1);meshMotionFrameBoundary(ctx.Get());
    auto phaseCapture=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[1-eyes[0].write].buffer.Get());
    check(comparison.phase==1 && comparison.mode==OversizedMode::Packed && !pending.count && word(phaseCapture,31)==2,"A1 pending IDs flush under immediate mode before the boundary starts B");
    bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);queue(farB);
    meshMotionComparisonNativeSampling(true,61,601);seedCompleteMetrics();auto pendingB=report(61,601,metadata());meshMotionComparisonNativeReport(pendingB);meshMotionFrameBoundary(ctx.Get());
    phaseCapture=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[1-eyes[0].write].buffer.Get());
    check(comparison.phase==2 && comparison.mode==OversizedMode::Immediate && !pending.count && word(phaseCapture,31)==2 && word(phaseCapture,91)==3,"B pending packed IDs flush before the boundary starts A2");
    bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farB);
    meshMotionComparisonNativeSampling(true,62,602);seedCompleteMetrics();auto pendingA2=report(62,602,metadata());meshMotionComparisonNativeReport(pendingA2);meshMotionFrameBoundary(ctx.Get());
    phaseCapture=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[1-eyes[0].write].buffer.Get());
    check(comparison.stage==ComparisonStage::Idle && comparison.mode==normalOversizedMode && !pending.count && word(phaseCapture,31)==3,"A2 pending IDs flush before completion restores normal packed capture");

    armToRunning();pose(1,0,2);bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);ctx->IASetVertexBuffers(0,1,largeStream.GetAddressOf(),&idStep,&idStart);queue(farA);meshMotionRequestComparison();meshMotionFrameBoundary(ctx.Get());
    phaseCapture=readBuffer(dev.Get(),ctx.Get(),eyes[0].history[1-eyes[0].write].buffer.Get());
    check(comparison.stage==ComparisonStage::Idle && comparison.mode==normalOversizedMode && !pending.count && word(phaseCapture,31)==2,"a pending A-phase capture flushes before cancellation restores normal packed mode");

    armToRunning();meshMotionRequestComparison();processComparisonBoundary(ctx.Get());expectRestored("cancellation at a frame boundary restores normal packed capture");
    testMenuOpen=true;meshMotionRequestComparison();check(comparison.stage==ComparisonStage::Armed && comparison.mode==normalOversizedMode,"a comparison can be re-armed after cancellation without changing normal mode");meshMotionRequestComparison();processComparisonBoundary(ctx.Get());testMenuOpen=false;expectRestored("cancelling the re-armed comparison restores normal packed capture");
    armToRunning();testMenuOpen=true;processComparisonBoundary(ctx.Get());testMenuOpen=false;expectRestored("reopening the menu during a phase aborts and restores normal packed capture");
    armToRunning();comparison.phaseDeadlineMs=0;processComparisonBoundary(ctx.Get());expectRestored("a phase timeout aborts and restores normal packed capture");
    armToRunning();++testBenchmarkDisturbanceEpoch;processComparisonBoundary(ctx.Get());expectRestored("a disturbance during native warmup or sampling aborts the whole comparison");
    armToRunning();failed=true;processComparisonBoundary(ctx.Get());expectRestored("a mesh setup failure aborts and restores normal packed capture");failed=false;

    armToRunning();meshMotionComparisonNativeSampling(true,20,200);meshMotionComparisonNativeSampling(false,20,201);check(comparison.failure!=nullptr && !comparison.measurement,"an inactive mixed-scope abort tick closes collection and marks the phase invalid");processComparisonBoundary(ctx.Get());expectRestored("mixed window/scope callbacks abort instead of advancing");
    armToRunning();meshMotionComparisonNativeSampling(true,21,201);auto aborted=report(21,201,metadata());aborted.complete=false;aborted.aborted=true;aborted.abortReason=kNativeBenchmarkScopeChanged;meshMotionComparisonNativeReport(aborted);processComparisonBoundary(ctx.Get());expectRestored("a matching aborted native report aborts the comparison");

    for(unsigned missing=0;missing<2;++missing){armToRunning();meshMotionComparisonNativeSampling(true,30+missing,300+missing);seedCompleteMetrics();auto invalid=report(30+missing,300+missing,metadata());if(!missing)invalid.cpu.available=false;else invalid.gpu.available=false;meshMotionComparisonNativeReport(invalid);processComparisonBoundary(ctx.Get());expectRestored(missing?"missing native GPU samples abort and restore baseline":"missing native CPU samples abort and restore baseline");}
    armToRunning();meshMotionComparisonNativeSampling(true,40,400);seedCompleteMetrics();comparison.metrics.oversizedDraws=0;auto noOversized=report(40,400,metadata());meshMotionComparisonNativeReport(noOversized);processComparisonBoundary(ctx.Get());expectRestored("a view with no eligible oversized draws aborts instead of reporting zero cost");
    armToRunning();meshMotionComparisonNativeSampling(true,41,401);seedCompleteMetrics();comparison.metrics.flushCpuSamples=0;auto missingComponent=report(41,401,metadata());meshMotionComparisonNativeReport(missingComponent);processComparisonBoundary(ctx.Get());expectRestored("missing required component coverage aborts instead of producing a partial comparison");
    armToRunning();meshMotionComparisonNativeSampling(true,42,402);seedCompleteMetrics();comparison.metrics.copyGpuSubmitted=2;auto pendingComponent=report(42,402,metadata());meshMotionComparisonNativeReport(pendingComponent);processComparisonBoundary(ctx.Get());expectRestored("pending component GPU queries after drain abort instead of leaking into another phase");

    armToRunning();meshMotionComparisonNativeSampling(true,50,500);seedCompleteMetrics();auto baselineReport=report(50,500,metadata());meshMotionComparisonNativeReport(baselineReport);processComparisonBoundary(ctx.Get());
    meshMotionComparisonNativeSampling(true,51,501);seedCompleteMetrics();auto changed=report(51,501,metadata(2480));meshMotionComparisonNativeReport(changed);processComparisonBoundary(ctx.Get());expectRestored("metadata changes between phases abort the entire comparison");
    armToRunning();meshMotionConfigure(false);check(!enabled && comparison.stage==ComparisonStage::Idle && comparison.mode==normalOversizedMode,"disabling mesh motion resets an active comparison to normal packed capture");meshMotionConfigure(true);

    if(argc>2 && (std::strcmp(argv[1],"--replay")==0 || std::strcmp(argv[1],"--replay-hardware")==0))replayMeshes(dev.Get(),ctx.Get(),argv[2],hardware);
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;messages->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());hr(messages->GetMessage(i,m,&bytes));if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){std::puts(m->pDescription);check(false,"D3D debug layer");}}
    std::printf("mesh motion: %u checks passed (%s)\n",checks,hardware?"hardware":"WARP");
}
