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
    reset();pose(1,0);run();meshMotionFrameBoundary(ctx.Get());bind(0);ctx->ClearDepthStencilView(dsv[0].Get(),D3D11_CLEAR_DEPTH,0,0);issue(ctx.Get(),6,1,0,0,0);
    for(unsigned i=0;i<513;++i)meshMotionDraw(ctx.Get(),issue,6,1,0,0,0,materialVs);
    check(eyes[0].history[eyes[0].write].count==512,"record cap bounds excess draws");
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
    reset();pose(1,0);run();meshMotionFrameBoundary(ctx.Get());pose(1,.02f);a=run();
    std::filesystem::create_directories("build/obj/meshmotion");meshMotionStageDump(ctx.Get(),scene[0].Get());
    readBuffer(dev.Get(),ctx.Get(),eyes[0].history[eyes[0].write].buffer.Get()); // test-only readback completes the staged copy
    meshMotionWriteDump(ctx.Get(),L"build/obj/meshmotion",L"fixture");
    std::ifstream dumpFile("build/obj/meshmotion/eye_fixture_Mesh.bin",std::ios::binary);char magic[8];UINT dumpHeader[2];dumpFile.read(magic,8);dumpFile.read(reinterpret_cast<char*>(dumpHeader),8);
    check(bool(dumpFile) && !std::memcmp(magic,"EDVRMSH1",8) && dumpHeader[0]==1 && dumpHeader[1]==240,"explicit eye dump writes exact motion record format");
    std::ifstream header("src/d3d11/temporal_shader_source.h");std::string text((std::istreambuf_iterator<char>(header)),{});auto begin=text.find("bool meshPixel("),end=text.find("\n}\n",begin);check(begin!=std::string::npos && end!=std::string::npos,"production mesh consumer located");
    std::string consumer="Texture2D<float2> MC:register(t15);struct HoloRecord{uint4 key[8];float4 clip[3];float4 map[3];float4 meta;};StructuredBuffer<HoloRecord> MR:register(t16);Texture2D<float> Z:register(t0);RWTexture2D<float4> Out:register(u0);cbuffer P:register(b0){float4 holoJitter;float4 offset;}static const int4 region=0;static const int2 size=int2(64,64);static const float4 knobs=float4(0,1,.025,0);bool uiCovered(int2 q){return q.x==32 && q.y==31;}float zSceneAt(int2 q){return q.x==32 && q.y==30 ? .1 : Z.Load(int3(q,0));}\n";
    consumer+=text.substr(begin,end+3-begin);consumer+="[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){float2 pp;float zp;bool ok=meshPixel(id.xy,offset.xy,pp,zp);Out[id.xy]=float4(pp-id.xy,zp,ok?1:0);}";
    auto shaderCode=compile(consumer.c_str(),"cs_5_0");ComPtr<ID3D11ComputeShader> cs;hr(dev->CreateComputeShader(shaderCode->GetBufferPointer(),shaderCode->GetBufferSize(),nullptr,&cs));
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11UnorderedAccessView> uav;hr(dev->CreateTexture2D(&td,nullptr,&output));hr(dev->CreateUnorderedAccessView(output.Get(),nullptr,&uav));auto params=buffer(32,D3D11_BIND_CONSTANT_BUFFER);float parameters[8]={.25f,-.125f,1,0,0,0,0,0};
    auto consume=[&](){ctx->ClearState();ctx->UpdateSubresource(params.Get(),0,nullptr,parameters,0,0);ctx->CSSetShader(cs.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,1,params.GetAddressOf());ctx->CSSetShaderResources(0,1,z[0].GetAddressOf());ID3D11ShaderResourceView* views[2]{};meshMotionViews(ctx.Get(),scene[0].Get(),views);ctx->CSSetShaderResources(15,2,views);ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);ctx->Dispatch(8,8,1);auto pixels=readTexture(dev.Get(),ctx.Get(),output.Get());ctx->ClearState();return pixels;};
    auto pixels=consume();auto at=(32*W+32)*4;check(pixels[at+3]==1 && std::fabs(pixels[at]+.39f)<1e-5 && std::fabs(pixels[at+1]+.125f)<1e-5,"DLSS consumes exact motion and removes jitter");
    check(pixels[(30*W+32)*4+3]==0 && pixels[(31*W+32)*4+3]==0,"foreground and UI reject hull motion");
    parameters[4]=.25f;pixels=consume();check(pixels[at+3]==1 && std::fabs(pixels[at]+.39f)<1e-5,"native TAA grid returns same physical motion");parameters[2]=0;pixels=consume();check(pixels[at+3]==0,"nonconsecutive frame rejects mesh history");
    if(argc>2 && (std::strcmp(argv[1],"--replay")==0 || std::strcmp(argv[1],"--replay-hardware")==0))replayMeshes(dev.Get(),ctx.Get(),argv[2],hardware);
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;messages->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());hr(messages->GetMessage(i,m,&bytes));if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){std::puts(m->pDescription);check(false,"D3D debug layer");}}
    std::printf("mesh motion: %u checks passed (%s)\n",checks,hardware?"hardware":"WARP");
}
