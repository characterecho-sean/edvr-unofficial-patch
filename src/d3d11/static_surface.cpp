#include "static_surface.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdio>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "depth_probe.h"
#include "dxbc_static_surface.h"
#include "binding_shadow.h"
#include "vscreen.h"
#include "../common/log.h"
#include "../common/timing.h"

namespace edvr {
namespace static_surface_detail {

template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
constexpr size_t kShaderLimit=2048;
constexpr size_t kShaderByteLimit=64u*1024u*1024u;
constexpr size_t kSingleShaderByteLimit=4u*1024u*1024u;
constexpr size_t kPatchLimit=1024;
constexpr size_t kResourceLimit=4096;
constexpr size_t kLayoutLimit=1024;
constexpr size_t kKeyBufferLimit=8192;
constexpr size_t kBlendLimit=128;
constexpr size_t kDepthStateLimit=128;

std::atomic<bool> enabled{false};
bool failed=false;
uint64_t frame=1;
uint64_t geometryEpoch=1;
std::recursive_mutex stateMutex;

enum class Decline : unsigned {
    Shape,Context,Depth,Eye,Viewport,DepthState,Blend,Stages,Predicate,StreamOutput,
    Uav,Targets,Shaders,Linked,ShaderInputs,Bindings,Pool,Geometry,GpuWritable,Cache,HistoryConsumed,
    Count
};
const char* declineName(Decline d){
    static const char* n[]={"shape","context","depth","eye","viewport","depth-state","blend","stages","predicate","stream-output","uav","targets","shaders","linked","shader-inputs","reserved-bindings","pool","geometry","gpu-writable","cache","history-consumed"};
    return n[unsigned(d)];
}
struct Counters {
    uint64_t entries=0,accepted=0,patchFailed=0,shaderMiss=0;
    uint64_t declined[unsigned(Decline::Count)]{};
    uint64_t sampled=0;double sampledMs=0;
    void clear(){*this={};}
} counters;

struct VsEntry {Ptr<ID3D11VertexShader> object;StaticSurfaceShaderInputs inputs{};bool linked=false,valid=false;std::string reason;};
struct PsEntry {Ptr<ID3D11PixelShader> object;std::vector<BYTE> bytes;bool linked=false;};
std::unordered_map<ID3D11VertexShader*,VsEntry> vertexShaders;
std::unordered_map<ID3D11PixelShader*,PsEntry> pixelShaders;
size_t pixelShaderBytes=0;
thread_local bool creatingPatch=false;

struct PatchKey {
    ID3D11PixelShader* ps=nullptr;uint32_t identity=0,component=0,position=0;
    bool operator==(const PatchKey& x)const{return ps==x.ps&&identity==x.identity&&component==x.component&&position==x.position;}
};
struct PatchHash {size_t operator()(const PatchKey& k)const{
    size_t h=reinterpret_cast<size_t>(k.ps);h^=size_t(k.identity)*0x9e3779b1u;h^=size_t(k.component)<<17;h^=size_t(k.position)<<25;return h;
}};
struct PatchEntry {Ptr<ID3D11PixelShader> shader;bool attempted=false;std::string reason;};
std::unordered_map<PatchKey,PatchEntry,PatchHash> patches;
std::unordered_map<std::string,uint64_t> patchFailureReasons;

void notePatchFailure(const std::string& reason){
    ++counters.patchFailed;
    auto it=patchFailureReasons.find(reason);if(it!=patchFailureReasons.end()){++it->second;return;}
    if(patchFailureReasons.size()<32)patchFailureReasons.emplace(reason,1);
}

struct Slice {uint64_t begin=0,end=0,epoch=0;};
struct ResourceStamp {Ptr<ID3D11Resource> object;D3D11_BUFFER_DESC desc{};uint64_t epoch=0;std::vector<Slice> slices;};
std::unordered_map<ID3D11Resource*,ResourceStamp> resources;
struct LayoutInfo {Ptr<ID3D11InputLayout> object;uint32_t vertexMask=0,instanceMask=0;unsigned slots=0;bool valid=false;};
std::unordered_map<ID3D11InputLayout*,LayoutInfo> layouts;

struct Key96 {uint32_t v[3]{};bool operator==(const Key96& x)const{return v[0]==x.v[0]&&v[1]==x.v[1]&&v[2]==x.v[2];}};
struct Key96Hash {size_t operator()(const Key96& k)const{return size_t(k.v[0])^(size_t(k.v[1])<<1)^(size_t(k.v[2])<<7);}};
struct KeyBuffer {Key96 key{};Ptr<ID3D11Buffer> buffer;};
std::unordered_map<Key96,KeyBuffer,Key96Hash> keyBuffers;

struct BlendEntry {Ptr<ID3D11BlendState> original,twin;D3D11_BLEND_DESC desc{};};
std::unordered_map<ID3D11BlendState*,BlendEntry> blends;
Ptr<ID3D11BlendState> defaultBlendTwin;
struct DepthEntry {Ptr<ID3D11DepthStencilState> object;D3D11_DEPTH_STENCIL_DESC desc{};};
std::unordered_map<ID3D11DepthStencilState*,DepthEntry> depthStates;

struct Eye {
    Ptr<ID3D11Texture2D> scene;
    Ptr<ID3D11Texture2D> tex[2];
    Ptr<ID3D11RenderTargetView> rtv[2];
    Ptr<ID3D11ShaderResourceView> srv[2];
    unsigned write=0,width=0,height=0;
    bool cleared=false,wrote=false,invalid=false,consumed=false,havePrevious=false;
    uint64_t previousFrame=0;
};
Eye eyes[2];

struct Active {
    bool on=false,sample=false;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11Buffer> cb;
    Ptr<ID3D11ShaderResourceView> srv;
    std::array<Ptr<ID3D11RenderTargetView>,8> rt;
    Ptr<ID3D11DepthStencilView> depth;
    Ptr<ID3D11BlendState> blend;
    FLOAT factors[4]{};UINT mask=~0u;
    Eye* eye=nullptr;
    void clear(){*this=Active{};}
};
thread_local Active active;

Ptr<ID3D11Texture2D> dump[2];
bool dumpAvailable=false;
std::string dumpReason="not staged";
unsigned dumpSceneFrame=~0u,dumpEye=~0u,dumpWidth=0,dumpHeight=0;
uint64_t dumpOwnerFrame=0;

void decline(Decline d){++counters.declined[unsigned(d)];}

bool rigidFamily(uint64_t h){
    return h==0xEB5234DB6ADB491Dull || h==0xDE545DC8EE4FBB87ull ||
        h==0x61AE8EB05FDC18DDull || h==0x66DE2CADB1F4AE6Bull ||
        h==0xAACFDCF2FB9AD809ull;
}

void mix(Key96& h,uint64_t x){
    for(unsigned i=0;i<3;++i){uint32_t lo=uint32_t(x),hi=uint32_t(x>>32);h.v[i]^=lo+0x9e3779b9u*(i+1);h.v[i]*=(0x01000193u+0x1f123bb5u*i)|1u;h.v[i]^=hi+(h.v[i]>>13);h.v[i]*=(0x85ebca6bu+0x1020304u*i)|1u;}
}
Key96 startKey(){Key96 h{{2166136261u,0x9e3779b9u,0x85ebca6bu}};return h;}

ResourceStamp* resourceStamp(ID3D11Resource* resource,bool& gpuWritable){
    auto it=resources.find(resource);
    if(it!=resources.end()){gpuWritable=(it->second.desc.BindFlags&(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_STREAM_OUTPUT))!=0;return &it->second;}
    if(resources.size()>=kResourceLimit)return nullptr;
    D3D11_RESOURCE_DIMENSION dim{};resource->GetType(&dim);if(dim!=D3D11_RESOURCE_DIMENSION_BUFFER)return nullptr;
    ResourceStamp s{};s.object=resource;static_cast<ID3D11Buffer*>(resource)->GetDesc(&s.desc);s.epoch=geometryEpoch;
    gpuWritable=(s.desc.BindFlags&(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_STREAM_OUTPUT))!=0;
    return &resources.emplace(resource,std::move(s)).first->second;
}
uint64_t indexEpoch(ResourceStamp& s,uint64_t begin,uint64_t end){
    for(auto& x:s.slices)if(x.begin==begin&&x.end==end)return x.epoch;
    if(s.slices.size()>=64)return 0;
    s.slices.push_back({begin,end,s.epoch});return s.epoch;
}

ID3D11PixelShader* patchedShader(ID3D11DeviceContext* ctx,ID3D11PixelShader* ps,const StaticSurfaceShaderInputs& in){
    PatchKey key{ps,in.identityRegister,in.identityComponent,in.positionRegister};
    auto found=patches.find(key);if(found!=patches.end())return found->second.shader.Get();
    if(patches.size()>=kPatchLimit)return nullptr;
    PatchEntry entry{};entry.attempted=true;
    auto p=pixelShaders.find(ps);
    if(p==pixelShaders.end()||p->second.linked){entry.reason=p==pixelShaders.end()?"pixel shader was not remembered":"pixel shader used class linkage";notePatchFailure(entry.reason);patches.emplace(key,std::move(entry));return nullptr;}
    std::vector<BYTE> output;std::string reason;
    if(!staticSurfacePatch(p->second.bytes.data(),p->second.bytes.size(),in,output,reason)){
        entry.reason=reason;notePatchFailure(entry.reason);patches.emplace(key,std::move(entry));return nullptr;
    }
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);creatingPatch=true;
    const HRESULT hr=dev->CreatePixelShader(output.data(),output.size(),nullptr,&entry.shader);
    creatingPatch=false;
    if(FAILED(hr)||!entry.shader){char text[64];_snprintf_s(text,sizeof(text),_TRUNCATE,"CreatePixelShader failed 0x%08X",unsigned(hr));entry.reason=text;notePatchFailure(entry.reason);}
    auto result=entry.shader.Get();patches.emplace(key,std::move(entry));return result;
}

ID3D11BlendState* blendTwin(ID3D11DeviceContext* ctx,ID3D11BlendState* original){
    if(!original&&defaultBlendTwin)return defaultBlendTwin.Get();
    if(original){auto it=blends.find(original);if(it==blends.end())return nullptr;if(it->second.twin)return it->second.twin.Get();}
    D3D11_BLEND_DESC d{};
    if(original)d=blends.find(original)->second.desc;else{auto& t=d.RenderTarget[0];t.SrcBlend=D3D11_BLEND_ONE;t.DestBlend=D3D11_BLEND_ZERO;t.BlendOp=D3D11_BLEND_OP_ADD;t.SrcBlendAlpha=D3D11_BLEND_ONE;t.DestBlendAlpha=D3D11_BLEND_ZERO;t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;}
    if(!d.IndependentBlendEnable)for(unsigned i=1;i<8;++i)d.RenderTarget[i]=d.RenderTarget[0];
    d.IndependentBlendEnable=TRUE;auto& t=d.RenderTarget[7];t.BlendEnable=FALSE;t.SrcBlend=D3D11_BLEND_ONE;t.DestBlend=D3D11_BLEND_ZERO;t.BlendOp=D3D11_BLEND_OP_ADD;t.SrcBlendAlpha=D3D11_BLEND_ONE;t.DestBlendAlpha=D3D11_BLEND_ZERO;t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);Ptr<ID3D11BlendState> twin;if(FAILED(dev->CreateBlendState(&d,&twin)))return nullptr;
    if(!original){defaultBlendTwin=twin;return twin.Get();}
    auto& entry=blends.find(original)->second;entry.twin=twin;return entry.twin.Get();
}

bool blendDescription(ID3D11BlendState* state,D3D11_BLEND_DESC& out){
    if(!state){out={};auto& t=out.RenderTarget[0];t.SrcBlend=D3D11_BLEND_ONE;t.DestBlend=D3D11_BLEND_ZERO;t.BlendOp=D3D11_BLEND_OP_ADD;t.SrcBlendAlpha=D3D11_BLEND_ONE;t.DestBlendAlpha=D3D11_BLEND_ZERO;t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;return true;}
    auto it=blends.find(state);if(it!=blends.end()){out=it->second.desc;return true;}if(blends.size()>=kBlendLimit)return false;
    BlendEntry e{};e.original=state;state->GetDesc(&e.desc);out=e.desc;blends.emplace(state,std::move(e));return true;
}
bool depthDescription(ID3D11DepthStencilState* state,D3D11_DEPTH_STENCIL_DESC& out){
    if(!state){out={};out.DepthEnable=TRUE;out.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;out.DepthFunc=D3D11_COMPARISON_LESS;return true;}
    auto it=depthStates.find(state);if(it!=depthStates.end()){out=it->second.desc;return true;}if(depthStates.size()>=kDepthStateLimit)return false;
    DepthEntry e{};e.object=state;state->GetDesc(&e.desc);out=e.desc;depthStates.emplace(state,std::move(e));return true;
}

bool createEye(ID3D11DeviceContext* ctx,ID3D11Texture2D* scene,Eye& e){
    D3D11_TEXTURE2D_DESC sd{};scene->GetDesc(&sd);
    e=Eye{};e.scene=scene;e.width=sd.Width;e.height=sd.Height;
    D3D11_TEXTURE2D_DESC d{};d.Width=sd.Width;d.Height=sd.Height;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R32G32B32A32_UINT;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    for(unsigned i=0;i<2;++i)if(FAILED(dev->CreateTexture2D(&d,nullptr,&e.tex[i]))||FAILED(dev->CreateRenderTargetView(e.tex[i].Get(),nullptr,&e.rtv[i]))||FAILED(dev->CreateShaderResourceView(e.tex[i].Get(),nullptr,&e.srv[i]))){e=Eye{};return false;}
    return true;
}

ID3D11Buffer* keyBuffer(ID3D11DeviceContext* ctx,const Key96& key){
    auto it=keyBuffers.find(key);if(it!=keyBuffers.end())return it->second.buffer.Get();
    if(keyBuffers.size()>=kKeyBufferLimit)return nullptr;
    struct Data{uint32_t v[4];} data{{key.v[0],key.v[1],key.v[2],0}};
    D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA init{};init.pSysMem=&data;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);KeyBuffer value{};value.key=key;if(FAILED(dev->CreateBuffer(&d,&init,&value.buffer)))return nullptr;
    auto result=value.buffer.Get();keyBuffers.emplace(key,std::move(value));return result;
}

void invalidateHistory(){for(auto& e:eyes){e.havePrevious=false;e.invalid=true;}dumpAvailable=false;}

bool saveAndBind(ID3D11DeviceContext* ctx,Eye& eye,ID3D11PixelShader* replacement,ID3D11Buffer* cb,ID3D11ShaderResourceView* pool,ID3D11BlendState* twin,ID3D11PixelShader* originalPs,ID3D11Buffer* originalCb,ID3D11ShaderResourceView* originalSrv,const std::array<Ptr<ID3D11RenderTargetView>,8>& originalRt,ID3D11DepthStencilView* originalDepth,ID3D11BlendState* originalBlend,const FLOAT* originalFactors,UINT originalMask){
    active.clear();
    active.ps=originalPs;active.cb=originalCb;active.srv=originalSrv;active.rt=originalRt;active.depth=originalDepth;active.blend=originalBlend;for(unsigned i=0;i<4;++i)active.factors[i]=originalFactors[i];active.mask=originalMask;
    if(!eye.cleared){const float zero[4]{};ctx->ClearRenderTargetView(eye.rtv[eye.write].Get(),zero);eye.cleared=true;}
    ID3D11RenderTargetView* rt[8]{};for(unsigned i=0;i<8;++i)rt[i]=active.rt[i].Get();rt[7]=eye.rtv[eye.write].Get();vScreenSetRenderTargetsRaw(ctx,8,rt,active.depth.Get());ctx->OMSetBlendState(twin,active.factors,active.mask);ctx->PSSetConstantBuffers(13,1,&cb);ctx->PSSetShaderResources(127,1,&pool);vScreenPSSetShaderRaw(ctx,replacement,nullptr,0);
    active.eye=&eye;active.on=true;return true;
}

void writeTexture(ID3D11DeviceContext* ctx,ID3D11Texture2D* tex,const wchar_t* path,bool& ok){
    ok=false;if(!tex)return;D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);D3D11_MAPPED_SUBRESOURCE map{};if(FAILED(ctx->Map(tex,0,D3D11_MAP_READ,0,&map)))return;
    FILE* f=nullptr;_wfopen_s(&f,path,L"wb");if(f){const uint32_t header[9]={1,d.Width,d.Height,uint32_t(d.Format),d.Width*16,dumpSceneFrame,0,0,0};ok=fwrite("EDVRTEX1",1,8,f)==8&&fwrite(header,sizeof(header),1,f)==1;for(uint32_t y=0;y<d.Height&&ok;++y)ok=fwrite(static_cast<const char*>(map.pData)+y*map.RowPitch,1,d.Width*16,f)==d.Width*16;if(fclose(f)!=0)ok=false;}ctx->Unmap(tex,0);
}

} // namespace static_surface_detail

void staticSurfaceConfigure(bool on){using namespace static_surface_detail;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(on==enabled.load())return;staticSurfaceShutdown();enabled.store(on);if(on)Log::get().note("static surface ownership: enabled; eligible original rigid draws append a 96-bit geometry/pose owner and raw device depth in MRT7. Shader and input-layout metadata is captured at creation, so enabling this live requires a restart before draws can be owned.");}

void staticSurfaceRememberVs(ID3D11VertexShader* shader,uint64_t hash,const void* bytecode,size_t bytes,bool linked){
    using namespace static_surface_detail;if(creatingPatch||!enabled.load()||!bytecode||!bytes||!rigidFamily(hash))return;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(!enabled.load()||!shader||vertexShaders.find(shader)!=vertexShaders.end()||vertexShaders.size()>=kShaderLimit)return;
    VsEntry e{};e.object=shader;e.linked=linked;if(linked)e.reason="vertex shader used class linkage";else e.valid=staticSurfaceDeriveShaderInputs(bytecode,bytes,e.inputs,e.reason);vertexShaders.emplace(shader,std::move(e));
}
void staticSurfaceRememberPs(ID3D11PixelShader* shader,const void* bytecode,size_t bytes,bool linked){
    using namespace static_surface_detail;if(creatingPatch||!enabled.load())return;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(!enabled.load()||!shader||!bytecode||!bytes||bytes>kSingleShaderByteLimit||pixelShaderBytes>kShaderByteLimit-bytes||pixelShaders.find(shader)!=pixelShaders.end()||pixelShaders.size()>=kShaderLimit)return;
    PsEntry e{};e.object=shader;e.linked=linked;e.bytes.assign(static_cast<const BYTE*>(bytecode),static_cast<const BYTE*>(bytecode)+bytes);pixelShaderBytes+=bytes;pixelShaders.emplace(shader,std::move(e));
}
void staticSurfaceRememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* elements,unsigned count,uint64_t vsHash){
    using namespace static_surface_detail;if(!enabled.load()||!rigidFamily(vsHash))return;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(!enabled.load()||!layout||!elements||!count||layouts.find(layout)!=layouts.end()||layouts.size()>=kLayoutLimit)return;
    LayoutInfo info{};info.object=layout;info.valid=true;bool haveModelIndex=false;
    for(unsigned i=0;i<count;++i){
        const auto& e=elements[i];const unsigned slot=e.InputSlot;
        if(slot>=D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT||!e.SemanticName){info.valid=false;continue;}
        const uint32_t bit=1u<<slot;
        const bool modelIndex=e.SemanticIndex==0&&_stricmp(e.SemanticName,"INSTANCEANDMODELDATAINDEX")==0;
        if(modelIndex){
            if(haveModelIndex||e.InputSlotClass!=D3D11_INPUT_PER_INSTANCE_DATA||e.InstanceDataStepRate!=1)info.valid=false;
            else {haveModelIndex=true;info.instanceMask|=bit;}
        }else if(e.InputSlotClass==D3D11_INPUT_PER_VERTEX_DATA&&e.InstanceDataStepRate==0)info.vertexMask|=bit;
        else info.valid=false;
        info.slots=std::max(info.slots,slot+1);
    }
    if(!haveModelIndex||!info.vertexMask||!info.instanceMask||(info.vertexMask&info.instanceMask))info.valid=false;layouts.emplace(layout,std::move(info));
}

bool staticSurfaceBegin(ID3D11DeviceContext* ctx,unsigned count,unsigned instances,unsigned start,int base,unsigned,uint64_t vsHash){
    using namespace static_surface_detail;if(!enabled.load()||!rigidFamily(vsHash))return false;std::unique_lock<std::recursive_mutex> stateLock(stateMutex);if(!enabled.load()||failed)return false;
    ++counters.entries;const bool sample=(counters.entries&255u)==0;const int64_t t0=sample?qpcNow():0;
    auto finish=[&](Decline d){decline(d);if(sample){++counters.sampled;counters.sampledMs+=double(qpcNow()-t0)*1000.0/double(qpcFrequency());}return false;};
    if(active.on||!ctx||!count||count%3||!instances)return finish(Decline::Shape);
    if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return finish(Decline::Context);
    ID3D11DepthStencilView* dsv=static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));int eyeIndex=-1,target=-1;if(!dsv||!depthProbeCurrentSceneEyeOf(dsv,&eyeIndex,&target))return finish(Decline::Eye);
    Ptr<ID3D11Resource> depthRes;dsv->GetResource(&depthRes);Ptr<ID3D11Texture2D> scene;if(!depthRes||FAILED(depthRes.As(&scene)))return finish(Decline::Depth);
    D3D11_TEXTURE2D_DESC sd{};scene->GetDesc(&sd);if(sd.ArraySize!=1||sd.SampleDesc.Count!=1)return finish(Decline::Depth);
    UINT nv=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&nv,&vp);if(nv!=1||vp.TopLeftX||vp.TopLeftY||vp.Width!=sd.Width||vp.Height!=sd.Height||vp.MinDepth!=0||vp.MaxDepth!=1)return finish(Decline::Viewport);
    Ptr<ID3D11DepthStencilState> ds;UINT stencilRef=0;ctx->OMGetDepthStencilState(&ds,&stencilRef);D3D11_DEPTH_STENCIL_DESC dd{};if(!depthDescription(ds.Get(),dd))return finish(Decline::Cache);if(!dd.DepthEnable||dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL)return finish(Decline::DepthState);
    Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;ctx->OMGetBlendState(&blend,factors,&mask);D3D11_BLEND_DESC bd{};if(!blendDescription(blend.Get(),bd))return finish(Decline::Cache);if(bd.AlphaToCoverageEnable||bd.RenderTarget[0].BlendEnable)return finish(Decline::Blend);
    D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> dom;ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&dom,nullptr,nullptr);if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST||gs||hs||dom)return finish(Decline::Stages);
    Ptr<ID3D11Predicate> predicate;BOOL pred=FALSE;ctx->GetPredication(&predicate,&pred);if(predicate)return finish(Decline::Predicate);
    ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);bool occupied=false;for(auto* p:so)if(p){occupied=true;p->Release();}if(occupied)return finish(Decline::StreamOutput);
    ID3D11UnorderedAccessView* uav[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uav);for(auto* p:uav)if(p){occupied=true;p->Release();}if(occupied)return finish(Decline::Uav);
    ID3D11RenderTargetView* rawRtv[8]{};Ptr<ID3D11DepthStencilView> actualDepth;ctx->OMGetRenderTargets(8,rawRtv,&actualDepth);std::array<Ptr<ID3D11RenderTargetView>,8> originalRt;for(unsigned i=0;i<8;++i){originalRt[i].Attach(rawRtv[i]);if(i>=6&&rawRtv[i])occupied=true;}if(occupied||actualDepth.Get()!=dsv)return finish(Decline::Targets);
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;UINT vsClasses=0,psClasses=0;ctx->VSGetShader(&vs,nullptr,&vsClasses);ctx->PSGetShader(&ps,nullptr,&psClasses);if(!vs||!ps)return finish(Decline::Shaders);
    auto vi=vertexShaders.find(vs.Get());auto pi=pixelShaders.find(ps.Get());if(vi==vertexShaders.end()||pi==pixelShaders.end()){++counters.shaderMiss;return finish(Decline::Shaders);}if(vsClasses||psClasses||vi->second.linked||pi->second.linked)return finish(Decline::Linked);if(!vi->second.valid)return finish(Decline::ShaderInputs);
    Ptr<ID3D11Buffer> originalCb;Ptr<ID3D11ShaderResourceView> originalSrv;ctx->PSGetConstantBuffers(13,1,&originalCb);ctx->PSGetShaderResources(127,1,&originalSrv);if(originalCb||originalSrv)return finish(Decline::Bindings);
    Ptr<ID3D11ShaderResourceView> pool;ctx->VSGetShaderResources(33,1,&pool);if(!pool)return finish(Decline::Pool);D3D11_SHADER_RESOURCE_VIEW_DESC pd{};pool->GetDesc(&pd);Ptr<ID3D11Resource> poolResource;pool->GetResource(&poolResource);Ptr<ID3D11Buffer> poolBuffer;if(pd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER||pd.Buffer.FirstElement||FAILED(poolResource.As(&poolBuffer)))return finish(Decline::Pool);D3D11_BUFFER_DESC pbd{};poolBuffer->GetDesc(&pbd);if(pbd.StructureByteStride!=336||pd.Buffer.NumElements!=pbd.ByteWidth/336)return finish(Decline::Pool);
    Ptr<ID3D11InputLayout> layout;ctx->IAGetInputLayout(&layout);if(!layout)return finish(Decline::Geometry);auto layoutIt=layouts.find(layout.Get());if(layoutIt==layouts.end()||!layoutIt->second.valid)return finish(Decline::Geometry);const LayoutInfo& layoutInfo=layoutIt->second;
    ID3D11Buffer* rawVb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{},offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};ctx->IAGetVertexBuffers(0,layoutInfo.slots,rawVb,strides,offsets);std::array<Ptr<ID3D11Buffer>,D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vb;for(unsigned i=0;i<layoutInfo.slots;++i)vb[i].Attach(rawVb[i]);
    Ptr<ID3D11Buffer> ib;DXGI_FORMAT indexFormat{};UINT indexOffset=0;ctx->IAGetIndexBuffer(&ib,&indexFormat,&indexOffset);const UINT indexBytes=indexFormat==DXGI_FORMAT_R16_UINT?2:indexFormat==DXGI_FORMAT_R32_UINT?4:0;if(!ib||!indexBytes)return finish(Decline::Geometry);
    Key96 key=startKey();mix(key,vsHash);mix(key,reinterpret_cast<uintptr_t>(vs.Get()));mix(key,reinterpret_cast<uintptr_t>(ps.Get()));mix(key,reinterpret_cast<uintptr_t>(layout.Get()));mix(key,uint64_t(uint32_t(base)));mix(key,start);mix(key,count);mix(key,indexFormat);mix(key,indexOffset);
    bool haveVertex=false;
    for(unsigned i=0;i<layoutInfo.slots;++i)if(layoutInfo.vertexMask&(1u<<i)){if(!vb[i])return finish(Decline::Geometry);haveVertex=true;bool gpu=false;auto* stamp=resourceStamp(vb[i].Get(),gpu);if(!stamp)return finish(Decline::Cache);if(gpu)return finish(Decline::GpuWritable);mix(key,i);mix(key,reinterpret_cast<uintptr_t>(vb[i].Get()));mix(key,strides[i]);mix(key,offsets[i]);mix(key,stamp->epoch);}
    if(!haveVertex)return finish(Decline::Geometry);
    bool gpu=false;auto* indexStamp=resourceStamp(ib.Get(),gpu);if(!indexStamp)return finish(Decline::Cache);if(gpu)return finish(Decline::GpuWritable);const uint64_t first=uint64_t(indexOffset)+uint64_t(start)*indexBytes,end=first+uint64_t(count)*indexBytes;if(end>indexStamp->desc.ByteWidth)return finish(Decline::Geometry);const uint64_t ie=indexEpoch(*indexStamp,first,end);if(!ie)return finish(Decline::Cache);mix(key,reinterpret_cast<uintptr_t>(ib.Get()));mix(key,ie);
    auto* replacement=patchedShader(ctx,ps.Get(),vi->second.inputs);if(!replacement)return finish(Decline::ShaderInputs);auto* cb=keyBuffer(ctx,key);if(!cb)return finish(Decline::Cache);auto* twin=blendTwin(ctx,blend.Get());if(!twin)return finish(Decline::Cache);
    Eye& eye=eyes[eyeIndex];if(eye.scene.Get()!=scene.Get()&&!createEye(ctx,scene.Get(),eye)){failed=true;return finish(Decline::Cache);}if(eye.consumed)return finish(Decline::HistoryConsumed);
    if(!saveAndBind(ctx,eye,replacement,cb,pool.Get(),twin,ps.Get(),originalCb.Get(),originalSrv.Get(),originalRt,actualDepth.Get(),blend.Get(),factors,mask))return finish(Decline::Cache);
    active.sample=sample;++counters.accepted;if(sample){++counters.sampled;counters.sampledMs+=double(qpcNow()-t0)*1000.0/double(qpcFrequency());}stateLock.release();return true;
}

void staticSurfaceEnd(ID3D11DeviceContext* ctx){
    using namespace static_surface_detail;if(!active.on||!ctx)return;
    const bool sample=active.sample;const int64_t t0=sample?qpcNow():0;
    ID3D11ShaderResourceView* oldSrv=active.srv.Get();ctx->PSSetShaderResources(127,1,&oldSrv);ID3D11Buffer* oldCb=active.cb.Get();ctx->PSSetConstantBuffers(13,1,&oldCb);vScreenPSSetShaderRaw(ctx,active.ps.Get(),nullptr,0);
    ID3D11RenderTargetView* rt[8]{};for(unsigned i=0;i<8;++i)rt[i]=active.rt[i].Get();vScreenSetRenderTargetsRaw(ctx,8,rt,active.depth.Get());ctx->OMSetBlendState(active.blend.Get(),active.factors,active.mask);
    if(active.eye)active.eye->wrote=true;if(sample)counters.sampledMs+=double(qpcNow()-t0)*1000.0/double(qpcFrequency());active.clear();stateMutex.unlock();
}

bool staticSurfaceViews(ID3D11DeviceContext*,ID3D11Texture2D* scene,ID3D11ShaderResourceView** views){
    using namespace static_surface_detail;if(!views)return false;views[0]=views[1]=nullptr;if(!enabled.load())return false;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(!enabled.load()||failed||!scene)return false;
    for(auto& e:eyes)if(e.scene.Get()==scene){const bool current=e.wrote&&!e.invalid;if(current)e.consumed=true;const bool ok=current&&e.havePrevious&&e.previousFrame+1==frame;if(!ok)return false;views[0]=e.srv[e.write].Get();views[1]=e.srv[1-e.write].Get();views[0]->AddRef();views[1]->AddRef();return true;}return false;
}

void staticSurfaceFrameBoundary(ID3D11DeviceContext*){
    using namespace static_surface_detail;if(!enabled.load())return;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(!enabled.load())return;
    for(auto& e:eyes){if(e.wrote&&!e.invalid){e.havePrevious=true;e.previousFrame=frame;e.write=1-e.write;}else e.havePrevious=false;e.cleared=e.wrote=e.invalid=e.consumed=false;}
    if(frame%1800==0){Log::get().note("static surface ownership: %llu supported-family candidates, %llu accepted, %llu patch failures, %llu shader misses; sampled supported-hook CPU %.3f us (%llu samples).",(unsigned long long)counters.entries,(unsigned long long)counters.accepted,(unsigned long long)counters.patchFailed,(unsigned long long)counters.shaderMiss,counters.sampled?counters.sampledMs*1000.0/counters.sampled:0.0,(unsigned long long)counters.sampled);for(unsigned i=0;i<unsigned(Decline::Count);++i)if(counters.declined[i])Log::get().note("static surface ownership decline %s: %llu.",declineName(Decline(i)),(unsigned long long)counters.declined[i]);for(const auto& reason:patchFailureReasons)Log::get().note("static surface ownership patch decline (%s): %llu shader/input pairs.",reason.first.c_str(),(unsigned long long)reason.second);counters.clear();patchFailureReasons.clear();}
    ++frame;
}

void staticSurfaceResourceWritten(ID3D11Resource* resource,uint64_t first,uint64_t end){
    using namespace static_surface_detail;if(!enabled.load())return;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(!enabled.load())return;if(!resource){++geometryEpoch;resources.clear();invalidateHistory();return;}auto it=resources.find(resource);if(it==resources.end()||first==end)return;if(first>end){first=0;end=~uint64_t(0);}const uint64_t epoch=++geometryEpoch;it->second.epoch=epoch;for(auto& s:it->second.slices)if(first<s.end&&s.begin<end)s.epoch=epoch;
}

void staticSurfaceStageDump(ID3D11DeviceContext* ctx,ID3D11Texture2D* scene,unsigned sceneFrame){
    using namespace static_surface_detail;std::lock_guard<std::recursive_mutex> lock(stateMutex);dump[0].Reset();dump[1].Reset();dumpAvailable=false;dumpReason=enabled.load()?(failed?"runtime setup failed":"no consecutive owner history for this eye/depth identity"):"feature disabled";dumpSceneFrame=sceneFrame;dumpEye=~0u;dumpWidth=dumpHeight=0;dumpOwnerFrame=frame;
    if(!ctx||!scene||!enabled.load()||failed)return;ID3D11ShaderResourceView* views[2]{};if(!staticSurfaceViews(ctx,scene,views))return;Ptr<ID3D11ShaderResourceView> held[2];held[0].Attach(views[0]);held[1].Attach(views[1]);
    for(unsigned eye=0;eye<2;++eye)if(eyes[eye].scene.Get()==scene){dumpEye=eye;dumpWidth=eyes[eye].width;dumpHeight=eyes[eye].height;break;}
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);for(unsigned i=0;i<2;++i){Ptr<ID3D11Resource> res;views[i]->GetResource(&res);Ptr<ID3D11Texture2D> tex;if(FAILED(res.As(&tex))){dumpReason="owner view was not a texture";return;}D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;if(FAILED(dev->CreateTexture2D(&d,nullptr,&dump[i]))){dumpReason="staging texture creation failed";return;}ctx->CopyResource(dump[i].Get(),tex.Get());}
    dumpAvailable=true;dumpReason="available";
}

void staticSurfaceWriteDump(ID3D11DeviceContext* ctx,const wchar_t* directory,const wchar_t* stamp){
    using namespace static_surface_detail;std::lock_guard<std::recursive_mutex> lock(stateMutex);bool currentOk=false,previousOk=false;if(dumpAvailable&&ctx&&dump[0]&&dump[1]){wchar_t path[MAX_PATH];_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_StaticOwner.bin",directory,stamp);writeTexture(ctx,dump[0].Get(),path,currentOk);_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_StaticOwnerPrev.bin",directory,stamp);writeTexture(ctx,dump[1].Get(),path,previousOk);if(!currentOk||!previousOk)dumpReason="texture readback/write failed";}
    wchar_t marker[MAX_PATH];_snwprintf_s(marker,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_StaticOwner.json",directory,stamp);FILE* f=nullptr;_wfopen_s(&f,marker,L"wb");bool markerOk=false;if(f){markerOk=fprintf(f,"{\n  \"version\": 1,\n  \"available\": %s,\n  \"reason\": \"%s\",\n  \"scene_frame\": %u,\n  \"owner_frame\": %llu,\n  \"eye\": %u,\n  \"width\": %u,\n  \"height\": %u,\n  \"current_written\": %s,\n  \"previous_written\": %s\n}\n",dumpAvailable&&currentOk&&previousOk?"true":"false",dumpReason.c_str(),dumpSceneFrame,(unsigned long long)dumpOwnerFrame,dumpEye,dumpWidth,dumpHeight,currentOk?"true":"false",previousOk?"true":"false")>0;markerOk=fclose(f)==0&&markerOk;}
    Log::get().note("static surface ownership: eye run %ls owner pair %s; marker %s (%s).",stamp,dumpAvailable&&currentOk&&previousOk?"written":"unavailable",markerOk?"written":"FAILED",dumpReason.c_str());dump[0].Reset();dump[1].Reset();dumpAvailable=false;
}

void staticSurfaceShutdown(){
    using namespace static_surface_detail;std::lock_guard<std::recursive_mutex> lock(stateMutex);if(active.on)active.clear();for(auto& e:eyes)e=Eye{};vertexShaders.clear();pixelShaders.clear();pixelShaderBytes=0;patches.clear();patchFailureReasons.clear();resources.clear();layouts.clear();keyBuffers.clear();blends.clear();depthStates.clear();defaultBlendTwin.Reset();dump[0].Reset();dump[1].Reset();dumpAvailable=false;dumpReason="not staged";enabled.store(false);failed=false;frame=geometryEpoch=1;counters.clear();
}

} // namespace edvr
