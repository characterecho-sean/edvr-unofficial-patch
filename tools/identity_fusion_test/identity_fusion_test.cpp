#include "../../src/common/system_d3d11.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "../../src/d3d11/weapon_motion.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {

unsigned checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",what);}}
void hr(HRESULT h,const char* what){if(FAILED(h)){std::printf("HRESULT %08X: %s\n",unsigned(h),what);throw std::runtime_error(what);}}

ComPtr<ID3DBlob> compile(const char* text,const char* entry,const char* profile){
    ComPtr<ID3DBlob> code,errors;const HRESULT h=D3DCompile(text,std::strlen(text),"identity_fusion_test",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(h)&&errors)std::fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr);
    hr(h,"compile shader");return code;
}

const char* kVs=R"HLSL(
struct O {
 uint2 face:__USER_VERTEX_FACEINVARIANT;
 float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;
 float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;
 float2 tex:__USER_VERTEX_M_TEXCOORD;
 float4 position:SV_Position;
};
O main(float4 position:POSITION,uint vertex:SV_VertexID){
 O o;o.face=uint2(vertex,vertex^0x55aa55aa);o.normal=float3(1,2,3);
 o.tangent=float3(4,5,6);o.tex=float2(7,8);o.position=position;return o;
})HLSL";
const char* kGs=R"HLSL(
Buffer<uint> Index:register(t0);
struct Instance{uint4 row[21];};
StructuredBuffer<Instance> Pool:register(t1);
struct V{uint2 face:__USER_VERTEX_FACEINVARIANT;float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;float2 tex:__USER_VERTEX_M_TEXCOORD;float4 position:SV_Position;};
struct P{float4 position:SV_Position;};struct I{uint4 identity:IDENTITY;};
[maxvertexcount(2)]void main(point V v[1],uint primitive:SV_PrimitiveID,inout PointStream<P> positions,inout PointStream<I> identities){
 P p;p.position=v[0].position;positions.Append(p);
 if(primitive==0){uint n,stride;Pool.GetDimensions(n,stride);uint id=Index[0];I i;i.identity=id<n?uint4(Pool[id].row[0].x,Pool[id].row[1].w,1,0):0;identities.Append(i);}
})HLSL";
const char* kSentinelCs="[numthreads(1,1,1)]void main(){}";
const char* kConsumerCs="StructuredBuffer<uint4> Source:register(t0);RWStructuredBuffer<uint4> Copy:register(u0);[numthreads(1,1,1)]void main(){Copy[0]=Source[0];}";
const char* kTypedConsumerCs="Buffer<uint4> Source:register(t0);RWStructuredBuffer<uint4> Copy:register(u0);[numthreads(1,1,1)]void main(){Copy[0]=Source[0];}";

struct Device {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ComPtr<ID3D11InfoQueue> info;bool debug=false;
    Device(D3D_DRIVER_TYPE driver,bool wantDebug){
        auto create=edvr::systemD3D11CreateDevice();if(!create)throw std::runtime_error("System32 D3D11CreateDevice unavailable");
        D3D_FEATURE_LEVEL level{};UINT flags=wantDebug?D3D11_CREATE_DEVICE_DEBUG:0;
        HRESULT h=create(nullptr,driver,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
        if(h==DXGI_ERROR_SDK_COMPONENT_MISSING&&wantDebug)h=create(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
        hr(h,driver==D3D_DRIVER_TYPE_WARP?"create WARP device":"create hardware device");
        debug=SUCCEEDED(dev.As(&info));
    }
};

ComPtr<ID3D11Buffer> buffer(ID3D11Device* dev,UINT bytes,UINT bind,const void* data=nullptr,UINT stride=0){
    D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=bind;d.StructureByteStride=stride;d.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;
    D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;ComPtr<ID3D11Buffer> out;hr(dev->CreateBuffer(&d,data?&initial:nullptr,&out),"create buffer");return out;
}
ComPtr<ID3D11ShaderResourceView> srv(ID3D11Device* dev,ID3D11Buffer* b,const D3D11_SHADER_RESOURCE_VIEW_DESC* desc=nullptr){ComPtr<ID3D11ShaderResourceView> out;hr(dev->CreateShaderResourceView(b,desc,&out),"create SRV");return out;}
bool fusedIdentityResource(ID3D11Device* dev,ComPtr<ID3D11Buffer>& stream,ComPtr<ID3D11Buffer>& consumer,ComPtr<ID3D11ShaderResourceView>& view,HRESULT* directResult=nullptr){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;const HRESULT attempted=dev->CreateBuffer(&d,nullptr,&stream);if(directResult)*directResult=attempted;if(SUCCEEDED(attempted)){consumer=stream;view=srv(dev,consumer.Get());return true;}stream=buffer(dev,16,D3D11_BIND_STREAM_OUTPUT);consumer=buffer(dev,16,D3D11_BIND_SHADER_RESOURCE,nullptr,16);view=srv(dev,consumer.Get());return false;}
std::vector<unsigned char> readBuffer(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Buffer* source,UINT bytes){
    D3D11_BUFFER_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;d.StructureByteStride=0;
    ComPtr<ID3D11Buffer> stage;hr(dev->CreateBuffer(&d,nullptr,&stage),"create staging buffer");ctx->CopyResource(stage.Get(),source);D3D11_MAPPED_SUBRESOURCE map{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map),"map staging buffer");
    std::vector<unsigned char> out(bytes);std::memcpy(out.data(),map.pData,bytes);ctx->Unmap(stage.Get(),0);return out;
}
bool prepareOutput(const fs::path& directory,bool dryRun){if(dryRun)return false;fs::create_directories(directory);return true;}

struct Stats {UINT64 written=0,needed=0;};
bool waitData(ID3D11DeviceContext* ctx,ID3D11Asynchronous* q,void* data,UINT bytes){
    const ULONGLONG until=GetTickCount64()+5000;ctx->Flush();HRESULT h=S_FALSE;
    while((h=ctx->GetData(q,data,bytes,D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE&&GetTickCount64()<until)Sleep(0);
    return h==S_OK;
}

struct Fixture {
    static constexpr UINT maxCount=131072,start=5;static constexpr INT base=3;static constexpr UINT vertexCount=4096;
    Device& d;ComPtr<ID3DBlob> vsCode;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11ComputeShader> cs,sentinelCs,consumerCs,typedConsumerCs;
    ComPtr<ID3D11GeometryShader> originalCapture,fusedCapture;
    ComPtr<ID3D11Buffer> vertices,indices,instanceCopy,instanceSource,pool,baselinePositions,fusedPositions,baselineIdentity,fusedIdentity,fusedIdentityConsumer,typedPositions,typedIdentity,consumerCopy,sentinelA,sentinelB,sentinelUavBuffer;
    ComPtr<ID3D11ShaderResourceView> instanceView,poolView,baselineIdentityView,fusedIdentityView,typedIdentityView,sentinelViewA,sentinelViewB;
    ComPtr<ID3D11UnorderedAccessView> baselineIdentityUav,consumerCopyUav,sentinelUav;
    bool fusedIdentityDirect=false;HRESULT fusedIdentityDirectResult=E_FAIL;
    std::vector<std::array<float,4>> vertexData;std::vector<UINT> indexData;
    explicit Fixture(Device& device):d(device){
        vsCode=compile(kVs,"main","vs_5_0");auto csCode=compile(edvr::kWeaponIdentityCs,"main","cs_5_0"),gsCode=compile(kGs,"main","gs_5_0"),sentinelCode=compile(kSentinelCs,"main","cs_5_0"),consumerCode=compile(kConsumerCs,"main","cs_5_0"),typedConsumerCode=compile(kTypedConsumerCs,"main","cs_5_0");
        hr(d.dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"create VS");hr(d.dev->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs),"create production identity CS");hr(d.dev->CreateComputeShader(sentinelCode->GetBufferPointer(),sentinelCode->GetBufferSize(),nullptr,&sentinelCs),"create sentinel CS");hr(d.dev->CreateComputeShader(consumerCode->GetBufferPointer(),consumerCode->GetBufferSize(),nullptr,&consumerCs),"create structured identity consumer CS");hr(d.dev->CreateComputeShader(typedConsumerCode->GetBufferPointer(),typedConsumerCode->GetBufferSize(),nullptr,&typedConsumerCs),"create typed identity consumer CS");
        D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};hr(d.dev->CreateInputLayout(&element,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout),"create layout");
        D3D11_SO_DECLARATION_ENTRY originalDecl{0,"SV_POSITION",0,0,4,0};UINT originalStride=16;hr(d.dev->CreateGeometryShaderWithStreamOutput(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&originalDecl,1,&originalStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&originalCapture),"create no-program SO shader from VS bytecode");
        D3D11_SO_DECLARATION_ENTRY fusedDecl[2]={{0,"SV_POSITION",0,0,4,0},{1,"IDENTITY",0,0,4,1}};UINT fusedStrides[2]={16,16};hr(d.dev->CreateGeometryShaderWithStreamOutput(gsCode->GetBufferPointer(),gsCode->GetBufferSize(),fusedDecl,2,fusedStrides,2,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&fusedCapture),"create programmable multi-stream GS");
        vertexData.resize(vertexCount);for(UINT i=0;i<vertexCount;++i)vertexData[i]={{float(i),float(int(i%97)-48),float(i%31),1.0f}};
        indexData.resize(maxCount+start);for(UINT i=0;i<UINT(indexData.size());++i)indexData[i]=(i%13==0)?17u:((i*37u+(i/11u)%23u)%(vertexCount-UINT(base)));
        vertices=buffer(d.dev.Get(),UINT(vertexData.size()*sizeof(vertexData[0])),D3D11_BIND_VERTEX_BUFFER,vertexData.data());indices=buffer(d.dev.Get(),UINT(indexData.size()*sizeof(UINT)),D3D11_BIND_INDEX_BUFFER,indexData.data());
        std::array<UINT,16> instanceWords{};instanceWords[0]=0;instanceWords[2]=2;instanceWords[4]=7;instanceWords[6]=1;instanceSource=buffer(d.dev.Get(),UINT(sizeof(instanceWords)),D3D11_BIND_VERTEX_BUFFER,instanceWords.data());instanceCopy=buffer(d.dev.Get(),16,D3D11_BIND_SHADER_RESOURCE);
        D3D11_SHADER_RESOURCE_VIEW_DESC idView{};idView.Format=DXGI_FORMAT_R32_UINT;idView.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;idView.Buffer.NumElements=4;instanceView=srv(d.dev.Get(),instanceCopy.Get(),&idView);
        std::array<UINT,4*21*4> poolWords{};for(UINT record=0;record<4;++record){UINT* p=poolWords.data()+record*84;p[0]=record==1?0u:0x11000000u+record;p[7]=record==1?0u:0x77000000u+record;}
        pool=buffer(d.dev.Get(),UINT(sizeof(poolWords)),D3D11_BIND_SHADER_RESOURCE,poolWords.data(),336);poolView=srv(d.dev.Get(),pool.Get());
        baselinePositions=buffer(d.dev.Get(),maxCount*16,D3D11_BIND_STREAM_OUTPUT);fusedPositions=buffer(d.dev.Get(),maxCount*16,D3D11_BIND_STREAM_OUTPUT);fusedIdentityDirect=fusedIdentityResource(d.dev.Get(),fusedIdentity,fusedIdentityConsumer,fusedIdentityView,&fusedIdentityDirectResult);typedPositions=buffer(d.dev.Get(),maxCount*16,D3D11_BIND_STREAM_OUTPUT);typedIdentity=buffer(d.dev.Get(),16,D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_SHADER_RESOURCE);D3D11_SHADER_RESOURCE_VIEW_DESC typedView{};typedView.Format=DXGI_FORMAT_R32G32B32A32_UINT;typedView.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;typedView.Buffer.NumElements=1;typedIdentityView=srv(d.dev.Get(),typedIdentity.Get(),&typedView);
        baselineIdentity=buffer(d.dev.Get(),16,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,nullptr,16);hr(d.dev->CreateUnorderedAccessView(baselineIdentity.Get(),nullptr,&baselineIdentityUav),"create identity UAV");baselineIdentityView=srv(d.dev.Get(),baselineIdentity.Get());consumerCopy=buffer(d.dev.Get(),16,D3D11_BIND_UNORDERED_ACCESS,nullptr,16);hr(d.dev->CreateUnorderedAccessView(consumerCopy.Get(),nullptr,&consumerCopyUav),"create identity consumer UAV");
        std::array<UINT,4> sentA{{1,2,3,4}},sentB{{5,6,7,8}};sentinelA=buffer(d.dev.Get(),16,D3D11_BIND_SHADER_RESOURCE,sentA.data(),16);sentinelB=buffer(d.dev.Get(),16,D3D11_BIND_SHADER_RESOURCE,sentB.data(),16);sentinelViewA=srv(d.dev.Get(),sentinelA.Get());sentinelViewB=srv(d.dev.Get(),sentinelB.Get());sentinelUavBuffer=buffer(d.dev.Get(),16,D3D11_BIND_UNORDERED_ACCESS,nullptr,16);hr(d.dev->CreateUnorderedAccessView(sentinelUavBuffer.Get(),nullptr,&sentinelUav),"create sentinel UAV");
        d.ctx->IASetInputLayout(layout.Get());UINT stride=16,offset=0;d.ctx->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);d.ctx->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R32_UINT,0);d.ctx->VSSetShader(vs.Get(),nullptr,0);
    }
    void sentinels(){
        d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);d.ctx->GSSetShader(nullptr,nullptr,0);ID3D11ShaderResourceView* views[2]={sentinelViewA.Get(),sentinelViewB.Get()};d.ctx->GSSetShaderResources(0,2,views);d.ctx->CSSetShader(sentinelCs.Get(),nullptr,0);d.ctx->CSSetShaderResources(0,2,views);ID3D11UnorderedAccessView* u=sentinelUav.Get();UINT keep=~0u;d.ctx->CSSetUnorderedAccessViews(0,1,&u,&keep);
    }
    void copyInstance(UINT byteOffset){D3D11_BOX box{byteOffset,0,0,byteOffset+4,1,1};d.ctx->CopySubresourceRegion(instanceCopy.Get(),0,0,0,0,instanceSource.Get(),0,&box);}
    void baseline(UINT count,UINT instanceOffset,ID3D11Query* stats=nullptr,ID3D11Buffer* position=nullptr,ID3D11UnorderedAccessView* identity=nullptr){
        if(!position)position=baselinePositions.Get();if(!identity)identity=baselineIdentityUav.Get();
        copyInstance(instanceOffset);ComPtr<ID3D11ComputeShader> savedCs;ID3D11ClassInstance* cc[256]{};UINT nc=256;d.ctx->CSGetShader(&savedCs,cc,&nc);ID3D11ShaderResourceView* savedSrvs[2]{};d.ctx->CSGetShaderResources(0,2,savedSrvs);ComPtr<ID3D11UnorderedAccessView> savedUav;d.ctx->CSGetUnorderedAccessViews(0,1,&savedUav);
        ID3D11ShaderResourceView* inputs[2]={instanceView.Get(),poolView.Get()};d.ctx->CSSetShaderResources(0,2,inputs);d.ctx->CSSetUnorderedAccessViews(0,1,&identity,nullptr);d.ctx->CSSetShader(cs.Get(),nullptr,0);d.ctx->Dispatch(1,1,1);
        ID3D11ShaderResourceView* noSrvs[2]{};ID3D11UnorderedAccessView* noUav=nullptr;d.ctx->CSSetShaderResources(0,2,noSrvs);d.ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);d.ctx->CSSetShader(savedCs.Get(),cc,nc);d.ctx->CSSetShaderResources(0,2,savedSrvs);UINT keep=~0u;d.ctx->CSSetUnorderedAccessViews(0,1,savedUav.GetAddressOf(),&keep);for(auto* p:savedSrvs)if(p)p->Release();for(UINT i=0;i<nc;++i)cc[i]->Release();
        D3D11_PRIMITIVE_TOPOLOGY topology{};d.ctx->IAGetPrimitiveTopology(&topology);ComPtr<ID3D11GeometryShader> savedGs;ID3D11ClassInstance* gc[256]{};UINT ng=256;d.ctx->GSGetShader(&savedGs,gc,&ng);d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);d.ctx->GSSetShader(originalCapture.Get(),nullptr,0);UINT zero=0;if(stats)d.ctx->Begin(stats);d.ctx->SOSetTargets(1,&position,&zero);d.ctx->DrawIndexed(count,start,base);d.ctx->SOSetTargets(0,nullptr,nullptr);if(stats)d.ctx->End(stats);d.ctx->GSSetShader(savedGs.Get(),gc,ng);d.ctx->IASetPrimitiveTopology(topology);for(UINT i=0;i<ng;++i)gc[i]->Release();
    }
    void fused(UINT count,UINT instanceOffset,ID3D11Query* stream0=nullptr,ID3D11Query* stream1=nullptr,ID3D11Buffer* position=nullptr,ID3D11Buffer* identity=nullptr,ID3D11Buffer* identityConsumer=nullptr){
        if(!position)position=fusedPositions.Get();if(!identity)identity=fusedIdentity.Get();if(!identityConsumer)identityConsumer=fusedIdentityConsumer.Get();
        copyInstance(instanceOffset);D3D11_PRIMITIVE_TOPOLOGY topology{};d.ctx->IAGetPrimitiveTopology(&topology);ComPtr<ID3D11GeometryShader> savedGs;ID3D11ClassInstance* gc[256]{};UINT ng=256;d.ctx->GSGetShader(&savedGs,gc,&ng);ID3D11ShaderResourceView* savedSrvs[2]{};d.ctx->GSGetShaderResources(0,2,savedSrvs);
        ID3D11ShaderResourceView* inputs[2]={instanceView.Get(),poolView.Get()};d.ctx->GSSetShaderResources(0,2,inputs);d.ctx->GSSetShader(fusedCapture.Get(),nullptr,0);d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);ID3D11Buffer* outputs[2]={position,identity};UINT offsets[2]{};if(stream0)d.ctx->Begin(stream0);if(stream1)d.ctx->Begin(stream1);d.ctx->SOSetTargets(2,outputs,offsets);d.ctx->DrawIndexed(count,start,base);d.ctx->SOSetTargets(0,nullptr,nullptr);if(identityConsumer!=identity)d.ctx->CopyResource(identityConsumer,identity);if(stream1)d.ctx->End(stream1);if(stream0)d.ctx->End(stream0);
        ID3D11ShaderResourceView* noSrvs[2]{};d.ctx->GSSetShaderResources(0,2,noSrvs);d.ctx->GSSetShader(savedGs.Get(),gc,ng);d.ctx->GSSetShaderResources(0,2,savedSrvs);d.ctx->IASetPrimitiveTopology(topology);for(auto* p:savedSrvs)if(p)p->Release();for(UINT i=0;i<ng;++i)gc[i]->Release();
    }
    void verifyState(){
        D3D11_PRIMITIVE_TOPOLOGY topology{};d.ctx->IAGetPrimitiveTopology(&topology);check(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,"IA topology restored");ComPtr<ID3D11GeometryShader> gotGs;UINT n=0;d.ctx->GSGetShader(&gotGs,nullptr,&n);check(!gotGs,"null GS restored");ID3D11ShaderResourceView* gs[2]{};d.ctx->GSGetShaderResources(0,2,gs);check(gs[0]==sentinelViewA.Get()&&gs[1]==sentinelViewB.Get(),"GS SRVs 0-1 restored while original GS is null");for(auto* p:gs)if(p)p->Release();
        ComPtr<ID3D11ComputeShader> gotCs;d.ctx->CSGetShader(&gotCs,nullptr,&n);check(gotCs==sentinelCs,"CS restored");ID3D11ShaderResourceView* csViews[2]{};d.ctx->CSGetShaderResources(0,2,csViews);check(csViews[0]==sentinelViewA.Get()&&csViews[1]==sentinelViewB.Get(),"CS SRVs restored");for(auto* p:csViews)if(p)p->Release();ComPtr<ID3D11UnorderedAccessView> u;d.ctx->CSGetUnorderedAccessViews(0,1,&u);check(u==sentinelUav,"CS UAV restored");ID3D11Buffer* so[2]{};d.ctx->SOGetTargets(2,so);check(!so[0]&&!so[1],"SO targets unbound");for(auto* p:so)if(p)p->Release();
    }
    std::vector<unsigned char> expectedPositions(UINT count)const{std::vector<unsigned char> out(size_t(count)*16);for(UINT i=0;i<count;++i){const UINT vertex=indexData[start+i]+UINT(base);std::memcpy(out.data()+size_t(i)*16,vertexData[vertex].data(),16);}return out;}
    std::vector<unsigned char> consumeIdentity(ID3D11ShaderResourceView* view,bool typed=false){ID3D11UnorderedAccessView* output=consumerCopyUav.Get();d.ctx->CSSetShaderResources(0,1,&view);d.ctx->CSSetUnorderedAccessViews(0,1,&output,nullptr);d.ctx->CSSetShader(typed?typedConsumerCs.Get():consumerCs.Get(),nullptr,0);d.ctx->Dispatch(1,1,1);ID3D11ShaderResourceView* noView=nullptr;ID3D11UnorderedAccessView* noUav=nullptr;d.ctx->CSSetShaderResources(0,1,&noView);d.ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);d.ctx->CSSetShader(nullptr,nullptr,0);return readBuffer(d.dev.Get(),d.ctx.Get(),consumerCopy.Get(),16);}
};

ComPtr<ID3D11Query> query(ID3D11Device* d,D3D11_QUERY type){D3D11_QUERY_DESC desc{type,0};ComPtr<ID3D11Query> q;hr(d->CreateQuery(&desc,&q),"create query");return q;}
bool correctness(Device& d){
    Fixture f(d);if(d.info)d.info->ClearStoredMessages();const UINT counts[]={96,2094,13260,30216,54324,131070,131072};const UINT offsets[]={8,16,0,24,8,0,16};const std::array<std::array<UINT,4>,7> expectedId={{{0x11000002,0x77000002,1,0},{0,0,0,0},{0x11000000,0x77000000,1,0},{0,0,1,0},{0x11000002,0x77000002,1,0},{0x11000000,0x77000000,1,0},{0,0,0,0}}};
    for(UINT c=0;c<UINT(std::size(counts));++c){
        auto qb=query(d.dev.Get(),D3D11_QUERY_SO_STATISTICS_STREAM0),q0=query(d.dev.Get(),D3D11_QUERY_SO_STATISTICS_STREAM0),q1=query(d.dev.Get(),D3D11_QUERY_SO_STATISTICS_STREAM1);f.sentinels();f.baseline(counts[c],offsets[c],qb.Get());f.verifyState();Stats sb{};check(waitData(d.ctx.Get(),qb.Get(),&sb,sizeof(sb)),"baseline SO statistics ready");check(sb.written==counts[c]&&sb.needed==counts[c],"baseline SO writes every point");
        const auto expected=f.expectedPositions(counts[c]);const auto baseline=readBuffer(d.dev.Get(),d.ctx.Get(),f.baselinePositions.Get(),counts[c]*16);check(baseline==expected,"baseline positions are byte exact");const auto baselineId=readBuffer(d.dev.Get(),d.ctx.Get(),f.baselineIdentity.Get(),16);check(!std::memcmp(baselineId.data(),expectedId[c].data(),16),"baseline identity is byte exact");
        f.sentinels();f.fused(counts[c],offsets[c],q0.Get(),q1.Get());f.verifyState();Stats s0{},s1{};check(waitData(d.ctx.Get(),q0.Get(),&s0,sizeof(s0)),"fused stream 0 statistics ready");check(waitData(d.ctx.Get(),q1.Get(),&s1,sizeof(s1)),"fused stream 1 statistics ready");check(s0.written==counts[c]&&s0.needed==counts[c],"16-byte stream 1 does not truncate stream 0");check(s1.written==1&&s1.needed==1,"only primitive ID zero writes identity");
        const auto fused=readBuffer(d.dev.Get(),d.ctx.Get(),f.fusedPositions.Get(),counts[c]*16),fusedId=readBuffer(d.dev.Get(),d.ctx.Get(),f.fusedIdentity.Get(),16),consumedId=f.consumeIdentity(f.fusedIdentityView.Get());check(fused==expected&&fused==baseline,"fused positions are byte exact");check(!std::memcmp(fusedId.data(),expectedId[c].data(),16)&&fusedId==baselineId,"fused identity is byte exact");check(consumedId==fusedId,"consumer reads fused identity through structured SRV");
        f.sentinels();f.fused(counts[c],offsets[c],q0.Get(),q1.Get(),f.typedPositions.Get(),f.typedIdentity.Get(),f.typedIdentity.Get());f.verifyState();Stats t0{},t1{};check(waitData(d.ctx.Get(),q0.Get(),&t0,sizeof(t0)),"typed fused stream 0 statistics ready");check(waitData(d.ctx.Get(),q1.Get(),&t1,sizeof(t1)),"typed fused stream 1 statistics ready");check(t0.written==counts[c]&&t0.needed==counts[c]&&t1.written==1&&t1.needed==1,"typed direct streams have complete SO counts");const auto typedPositions=readBuffer(d.dev.Get(),d.ctx.Get(),f.typedPositions.Get(),counts[c]*16),typedId=readBuffer(d.dev.Get(),d.ctx.Get(),f.typedIdentity.Get(),16),typedConsumed=f.consumeIdentity(f.typedIdentityView.Get(),true);check(typedPositions==expected&&typedPositions==baseline,"typed direct positions are byte exact");check(typedId==baselineId&&typedConsumed==typedId,"typed direct identity bits and Buffer<uint4> consumer are exact");
    }
    f.sentinels();f.fused(96,8);f.fused(96,0);const std::array<UINT,4> repeatId{{0x11000000,0x77000000,1,0}};const auto repeated=readBuffer(d.dev.Get(),d.ctx.Get(),f.fusedIdentity.Get(),16);check(!std::memcmp(repeated.data(),repeatId.data(),16),"repeated draw restarts primitive ID and replaces identity");f.verifyState();
    const fs::path dryProbe=fs::temp_directory_path()/(L"edvr-identity-fusion-dry-run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));std::error_code dryError;check(!fs::exists(dryProbe,dryError),"dry-run probe starts absent");check(!prepareOutput(dryProbe,true)&&!fs::exists(dryProbe,dryError),"dry-run output creates and writes nothing");
    check(f.fusedIdentityDirect||f.fusedIdentityDirectResult==E_INVALIDARG,"structured SO|SRV is supported or deterministically rejected E_INVALIDARG");if(d.info){for(UINT64 i=0;i<d.info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;d.info->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());hr(d.info->GetMessage(i,message,&bytes),"read debug message");if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::puts(message->pDescription);check(false,"debug layer has no warning/error messages");}}}
    std::printf("correctness: counts=7 max=131072 start=%u base=%d repeated_indices=yes short_stream1=does_not_truncate_stream0 structured_so_srv=%s_hresult_%08X typed_so_srv=direct debug=%s\n",Fixture::start,Fixture::base,f.fusedIdentityDirect?"supported":"unsupported_copyresource",unsigned(f.fusedIdentityDirectResult),d.debug?"clean":"unavailable");return failures==0;
}

const UINT workload[]={54324,37227,972,3765,948,1806,1728,2184,13260,30216,2364,2874,312,360,66,66,228,96,756,10929,552,288,204,558,558,96,144,234,234,37080,13260,30216,23784,19332,2094,2094,2364,24042,13674,7452};
struct BenchRecord {ComPtr<ID3D11Buffer> baselinePositions,baselineIdentity,fusedPositions,fusedIdentity,fusedIdentityConsumer,typedPositions,typedIdentity;ComPtr<ID3D11ShaderResourceView> baselineIdentityView,fusedIdentityView,typedIdentityView;ComPtr<ID3D11UnorderedAccessView> baselineIdentityUav;};
std::vector<BenchRecord> records(ID3D11Device* d){std::vector<BenchRecord> out(std::size(workload));for(UINT i=0;i<UINT(out.size());++i){auto&r=out[i];r.baselinePositions=buffer(d,workload[i]*16,D3D11_BIND_STREAM_OUTPUT);r.baselineIdentity=buffer(d,16,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,nullptr,16);hr(d->CreateUnorderedAccessView(r.baselineIdentity.Get(),nullptr,&r.baselineIdentityUav),"create benchmark baseline UAV");r.baselineIdentityView=srv(d,r.baselineIdentity.Get());r.fusedPositions=buffer(d,workload[i]*16,D3D11_BIND_STREAM_OUTPUT);fusedIdentityResource(d,r.fusedIdentity,r.fusedIdentityConsumer,r.fusedIdentityView);r.typedPositions=buffer(d,workload[i]*16,D3D11_BIND_STREAM_OUTPUT);r.typedIdentity=buffer(d,16,D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_SHADER_RESOURCE);D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32B32A32_UINT;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=1;r.typedIdentityView=srv(d,r.typedIdentity.Get(),&view);}return out;}
struct Sample {unsigned round=0,order=0;const char* path="";double cpuMs=0,gpuMs=0;UINT64 frequency=0,ticks=0;bool disjoint=false,ready=false,valid=false;};
enum class Path {Baseline,TypedDirect,StructuredCopy};
const char* pathName(Path p){return p==Path::Baseline?"baseline":p==Path::TypedDirect?"fused_typed_direct":"fused_structured_copy";}
Sample measure(Fixture& f,const std::vector<BenchRecord>& outputs,Path path,unsigned round,unsigned order){
    auto dis=query(f.d.dev.Get(),D3D11_QUERY_TIMESTAMP_DISJOINT),begin=query(f.d.dev.Get(),D3D11_QUERY_TIMESTAMP),end=query(f.d.dev.Get(),D3D11_QUERY_TIMESTAMP);LARGE_INTEGER frequency{},a{},b{};QueryPerformanceFrequency(&frequency);D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};UINT64 first=0,last=0;
    f.d.ctx->Begin(dis.Get());f.d.ctx->End(begin.Get());QueryPerformanceCounter(&a);for(UINT i=0;i<UINT(std::size(workload));++i){const UINT offset=(i%4)*8;const auto&r=outputs[i];if(path==Path::Baseline)f.baseline(workload[i],offset,nullptr,r.baselinePositions.Get(),r.baselineIdentityUav.Get());else if(path==Path::TypedDirect)f.fused(workload[i],offset,nullptr,nullptr,r.typedPositions.Get(),r.typedIdentity.Get(),r.typedIdentity.Get());else f.fused(workload[i],offset,nullptr,nullptr,r.fusedPositions.Get(),r.fusedIdentity.Get(),r.fusedIdentityConsumer.Get());}QueryPerformanceCounter(&b);f.d.ctx->End(end.Get());f.d.ctx->End(dis.Get());
    Sample s;s.round=round;s.order=order;s.path=pathName(path);s.cpuMs=double(b.QuadPart-a.QuadPart)*1000.0/double(frequency.QuadPart);const bool rd=waitData(f.d.ctx.Get(),dis.Get(),&dj,sizeof(dj)),ra=waitData(f.d.ctx.Get(),begin.Get(),&first,sizeof(first)),rb=waitData(f.d.ctx.Get(),end.Get(),&last,sizeof(last));s.ready=rd&&ra&&rb;s.disjoint=s.ready&&dj.Disjoint!=FALSE;s.frequency=s.ready?dj.Frequency:0;s.ticks=s.ready&&last>=first?last-first:0;s.valid=s.ready&&!s.disjoint&&s.frequency&&last>first;if(s.valid)s.gpuMs=double(s.ticks)*1000.0/double(s.frequency);return s;
}
std::string adapterName(ID3D11Device* d){ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};if(FAILED(d->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetDesc(&desc)))return "unknown";char out[512]{};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,out,UINT(sizeof(out)),nullptr,nullptr);return out;}
std::string jsonEscape(const std::string& s){std::string out;for(char ch:s){if(ch=='"'||ch=='\\')out.push_back('\\');if(static_cast<unsigned char>(ch)>=0x20)out.push_back(ch);}return out;}
double median(std::vector<double> values){std::sort(values.begin(),values.end());return values.empty()?0.0:values[values.size()/2];}
bool benchmark(Device& d,const fs::path* output){
    Fixture f(d);auto outputRecords=records(d.dev.Get());constexpr unsigned rounds=15,maxWarmupBatches=10000;const Path paths[3]={Path::Baseline,Path::TypedDirect,Path::StructuredCopy};LARGE_INTEGER qpf{},warmStart{},warmNow{};QueryPerformanceFrequency(&qpf);QueryPerformanceCounter(&warmStart);unsigned warmupBatches=0;double warmupGpuMs=0,warmupWallMs=0;do{const auto s=measure(f,outputRecords,paths[warmupBatches%3],0,warmupBatches%3);if(s.valid)warmupGpuMs+=s.gpuMs;++warmupBatches;QueryPerformanceCounter(&warmNow);warmupWallMs=double(warmNow.QuadPart-warmStart.QuadPart)*1000.0/double(qpf.QuadPart);}while((warmupWallMs<250.0||warmupGpuMs<250.0)&&warmupBatches<maxWarmupBatches);if(warmupWallMs<250.0||warmupGpuMs<250.0)throw std::runtime_error("warmup failed to reach 250 ms of wall and valid GPU work");
    std::vector<Sample> samples;samples.reserve(rounds*3);for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<3;++order)samples.push_back(measure(f,outputRecords,paths[(round+order)%3],round,order));
    std::vector<double> baseline,typed,structured;unsigned unhealthy=0;for(const auto& s:samples){if(!s.valid)++unhealthy;else if(!std::strcmp(s.path,"baseline"))baseline.push_back(s.gpuMs);else if(!std::strcmp(s.path,"fused_typed_direct"))typed.push_back(s.gpuMs);else structured.push_back(s.gpuMs);}
    const UINT64 total=std::accumulate(std::begin(workload),std::end(workload),UINT64(0));std::printf("benchmark: adapter=%s workload_draws=%zu workload_indices=%llu rounds=%u warmup_batches=%u warmup_wall_ms=%.3f warmup_gpu_ms=%.3f unhealthy=%u\n",adapterName(d.dev.Get()).c_str(),std::size(workload),(unsigned long long)total,rounds,warmupBatches,warmupWallMs,warmupGpuMs,unhealthy);std::printf("GPU median whole batch: baseline %.6f ms, fused_typed_direct %.6f ms, fused_structured_copy %.6f ms (descriptive only; no pass/fail threshold)\n",median(baseline),median(typed),median(structured));check(unhealthy==0,"all GPU timestamp/disjoint samples healthy with positive delta");
    if(output&&prepareOutput(*output,false)){const fs::path json=*output/L"identity_fusion_benchmark.json",csv=*output/L"identity_fusion_benchmark.csv";std::ofstream j(json,std::ios::binary),c(csv,std::ios::binary);if(!j||!c)throw std::runtime_error("open benchmark output");j<<"{\n  \"schema\": 1,\n  \"scope\": {\"driver\":\"hardware\",\"debug\":false,\"vertex_shader\":\"synthetic trivial fixture\",\"sv_position_output_register\":4,\"workload_source\":\"build/identity-fusion/historical-workloads.json capture 102403\",\"structured_so_srv_supported\":"<<(f.fusedIdentityDirect?"true":"false")<<",\"structured_so_srv_hresult\":\"0x"<<std::hex<<unsigned(f.fusedIdentityDirectResult)<<std::dec<<"\",\"structured_fused_consumer_path\":\""<<(f.fusedIdentityDirect?"direct":"CopyResource to structured SRV")<<"\"},\n  \"adapter\": \""<<jsonEscape(adapterName(d.dev.Get()))<<"\",\n  \"workload_draws\": "<<std::size(workload)<<",\n  \"workload_indices\": "<<total<<",\n  \"warmup_batches\": "<<warmupBatches<<",\n  \"warmup_wall_ms\": "<<warmupWallMs<<",\n  \"warmup_gpu_ms\": "<<warmupGpuMs<<",\n  \"measured_rounds\": "<<rounds<<",\n  \"baseline_gpu_median_ms\": "<<median(baseline)<<",\n  \"fused_typed_direct_gpu_median_ms\": "<<median(typed)<<",\n  \"fused_structured_copy_gpu_median_ms\": "<<median(structured)<<",\n  \"unhealthy_samples\": "<<unhealthy<<",\n  \"samples\": [\n";for(size_t i=0;i<samples.size();++i){const auto&s=samples[i];j<<"    {\"round\":"<<s.round<<",\"order\":"<<s.order<<",\"path\":\""<<s.path<<"\",\"cpu_ms\":"<<s.cpuMs<<",\"gpu_ms\":"<<s.gpuMs<<",\"ticks\":"<<s.ticks<<",\"frequency\":"<<s.frequency<<",\"disjoint\":"<<(s.disjoint?"true":"false")<<",\"ready\":"<<(s.ready?"true":"false")<<",\"valid\":"<<(s.valid?"true":"false")<<"}"<<(i+1==samples.size()?"\n":",\n");}j<<"  ]\n}\n";
        c<<"driver,debug,vertex_shader,sv_position_register,workload_source,workload_draws,workload_indices,round,order,path,cpu_ms,gpu_ms,ticks,frequency,disjoint,ready,valid\n";for(const auto&s:samples)c<<"hardware,false,synthetic_trivial,4,historical-workloads_102403,"<<std::size(workload)<<','<<total<<','<<s.round<<','<<s.order<<','<<s.path<<','<<s.cpuMs<<','<<s.gpuMs<<','<<s.ticks<<','<<s.frequency<<','<<(s.disjoint?1:0)<<','<<(s.ready?1:0)<<','<<(s.valid?1:0)<<'\n';j.close();c.close();if(!j||!c)throw std::runtime_error("write benchmark output");std::printf("artifacts: %ls, %ls\n",json.c_str(),csv.c_str());
    }return failures==0;
}

struct Options {bool selfTest=false,hardware=false,bench=false,dryRun=false;bool haveOutput=false;fs::path output;};
Options options(int argc,wchar_t** argv){Options o;for(int i=1;i<argc;++i){if(!std::wcscmp(argv[i],L"--self-test"))o.selfTest=true;else if(!std::wcscmp(argv[i],L"--hardware"))o.hardware=true;else if(!std::wcscmp(argv[i],L"--benchmark")){o.bench=true;o.hardware=true;}else if(!std::wcscmp(argv[i],L"--dry-run"))o.dryRun=true;else if(!std::wcscmp(argv[i],L"--output")&&i+1<argc){o.haveOutput=true;o.output=argv[++i];}else throw std::runtime_error("usage: identity_fusion_test --self-test | --hardware [--benchmark [--output DIR]] | --dry-run [--output DIR]");}if(!o.dryRun&&(o.selfTest==(o.hardware||o.bench)||o.haveOutput&&!o.bench))throw std::runtime_error("usage: identity_fusion_test --self-test | --hardware [--benchmark [--output DIR]] | --dry-run [--output DIR]");return o;}
} // namespace

int wmain(int argc,wchar_t** argv){try{const Options o=options(argc,argv);if(o.dryRun){std::printf("identity_fusion_test: dry-run (no device, directories, or files created)\n");return 0;}Device correctnessDevice(o.selfTest?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,true);if(!correctness(correctnessDevice))return 1;if(o.bench){correctnessDevice.ctx->ClearState();correctnessDevice.ctx->Flush();Device releaseDevice(D3D_DRIVER_TYPE_HARDWARE,false);if(!benchmark(releaseDevice,o.haveOutput?&o.output:nullptr))return 1;}std::printf("identity_fusion_test: %u checks, %u failures\n",checks,failures);return failures?1:0;}catch(const std::exception& e){std::fprintf(stderr,"identity_fusion_test: %s\n",e.what());return 1;}}
