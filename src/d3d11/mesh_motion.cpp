#include "mesh_motion.h"
#include "mesh_motion_shader.h"
#include "vscreen.h"
#include "binding_shadow.h"
#include "depth_probe.h"
#include "shader_swap.h"
#include "gpu_interval.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
namespace edvr { namespace mesh_motion_detail {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
constexpr unsigned maxRecords=512,stride=240;
constexpr uint64_t materialVs=0xEB5234DB6ADB491Dull,faceVs=0xDE545DC8EE4FBB87ull;
constexpr uint64_t multiUvVs=0x61AE8EB05FDC18DDull,litMultiUvVs=0x66DE2CADB1F4AE6Bull,detailVs=0xAACFDCF2FB9AD809ull;
enum CoverageKind { Material,Face,MultiUv,LitMultiUv,Detail,CoverageCount };
CoverageKind coverageKind(uint64_t hash){
    switch(hash){case materialVs:return Material;case faceVs:return Face;case multiUvVs:return MultiUv;
    case litMultiUvVs:return LitMultiUv;case detailVs:return Detail;default:return CoverageCount;}
}
struct History {
    Ptr<ID3D11Buffer> buffer;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11UnorderedAccessView> uav;
    Ptr<IUnknown> sources[maxRecords][4];
    unsigned count=0;
};
struct Eye {
    Ptr<ID3D11Texture2D> scene,coverage;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    History history[2];
    unsigned write=0,width=0,height=0;
    bool cleared=false,matched=false;
} eyes[2];
bool enabled=false,failed=false,noted=false,capped=false;
Ptr<ID3D11ComputeShader> capture,match;
Ptr<ID3D11PixelShader> coverageShaders[CoverageCount];
Ptr<ID3D11Buffer> settings,instances;
Ptr<ID3D11ShaderResourceView> instanceView;
Ptr<ID3D11DepthStencilState> depthState;
Ptr<ID3D11BlendState> blendState;
GpuIntervals<16> drawGpu,matchGpu;
unsigned frames=0,draws=0;
struct IndexStamp { unsigned epoch=0,seen=0; };
struct GeometryStamp {
    unsigned epoch=0,seen=0;
    // The game's shared IB also receives transient geometry uploads. Its
    // draw slices are exact; a write elsewhere must not drop hull history.
    // Entries expire with the two-frame history, bounding this cache by
    // the per-eye draw cap. Vertex inputs remain conservative whole buffers.
    std::unordered_map<uint64_t,IndexStamp> indices;
};
std::unordered_map<ID3D11Resource*,GeometryStamp> watched;
unsigned geometryEpoch=0,geometryWrites=0,unknownWrites=0,rangeWrites=0,disjointIndices=0;
Ptr<ID3D11Buffer> dump;
unsigned dumpCount=0;
struct Settings { UINT info[4],key[16];float dimensions[4]; };
struct ComputeState {
    ID3D11DeviceContext* ctx;
    Ptr<ID3D11ComputeShader> shader;
    ID3D11ClassInstance* classes[256]{};UINT count=256;
    ID3D11Buffer* cb[4]{};ID3D11ShaderResourceView* srv[3]{};Ptr<ID3D11UnorderedAccessView> uav;
    explicit ComputeState(ID3D11DeviceContext* c):ctx(c){ctx->CSGetShader(&shader,classes,&count);ctx->CSGetConstantBuffers(0,4,cb);ctx->CSGetShaderResources(0,3,srv);ctx->CSGetUnorderedAccessViews(0,1,&uav);}
    ~ComputeState(){
        ID3D11ShaderResourceView* none[3]{};ID3D11UnorderedAccessView* noUav=nullptr;
        ctx->CSSetShaderResources(0,3,none);ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
        ctx->CSSetShader(shader.Get(),classes,count);ctx->CSSetConstantBuffers(0,4,cb);ctx->CSSetShaderResources(0,3,srv);
        UINT keep=~0u;ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),&keep);
        for(auto* p:cb)if(p)p->Release();for(auto* p:srv)if(p)p->Release();for(UINT i=0;i<count;++i)classes[i]->Release();
    }
};
bool prepare(ID3D11DeviceContext* ctx,ID3D11Device* dev){
    if(capture)return true;
    capture.Attach(shaderSwapCompileCs(ctx,kMeshMotionHlsl,sizeof(kMeshMotionHlsl)-1,"capture","mesh capture",nullptr,"mesh motion"));
    match.Attach(shaderSwapCompileCs(ctx,kMeshMotionHlsl,sizeof(kMeshMotionHlsl)-1,"match","mesh match",nullptr,"mesh motion"));
    const char* entries[CoverageCount]={"material","face","multiUv","litMultiUv","detail"};
    for(unsigned i=0;i<CoverageCount;++i){
        coverageShaders[i].Attach(shaderSwapCompilePs(ctx,kMeshCoverageHlsl,sizeof(kMeshCoverageHlsl)-1,entries[i],"mesh coverage",nullptr,"mesh motion"));
        if(!coverageShaders[i])return false;
    }
    D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Settings);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&settings)))return false;
    b.ByteWidth=64*8;b.BindFlags=D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32_TYPELESS;s.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;s.BufferEx.NumElements=b.ByteWidth/4;s.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&instances)) || FAILED(dev->CreateShaderResourceView(instances.Get(),&s,&instanceView)))return false;
    D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=TRUE;d.DepthFunc=D3D11_COMPARISON_EQUAL;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=3;
    return capture && match && SUCCEEDED(dev->CreateDepthStencilState(&d,&depthState)) && SUCCEEDED(dev->CreateBlendState(&blend,&blendState));
}
bool createEye(ID3D11Device* dev,ID3D11Texture2D* source,Eye& e){
    D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);e=Eye{};e.scene=source;e.width=td.Width;e.height=td.Height;
    td={};td.Width=e.width;td.Height=e.height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&e.coverage)) || FAILED(dev->CreateRenderTargetView(e.coverage.Get(),nullptr,&e.rtv)) || FAILED(dev->CreateShaderResourceView(e.coverage.Get(),nullptr,&e.srv)))return false;
    D3D11_BUFFER_DESC b{};b.ByteWidth=maxRecords*stride;b.StructureByteStride=stride;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    for(auto& h:e.history)if(FAILED(dev->CreateBuffer(&b,nullptr,&h.buffer)) || FAILED(dev->CreateShaderResourceView(h.buffer.Get(),nullptr,&h.srv)) || FAILED(dev->CreateUnorderedAccessView(h.buffer.Get(),nullptr,&h.uav)))return false;
    return true;
}
void fail(){failed=true;Log::get().note("mesh motion: resource setup failed; existing camera/object motion retained.");}
} // namespace mesh_motion_detail
void meshMotionConfigure(bool on){using namespace mesh_motion_detail;if(on!=enabled){meshMotionShutdown();enabled=on;}}
void meshMotionDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn issue,unsigned count,unsigned n,unsigned start,int base,unsigned startInstance,uint64_t hash){
    using namespace mesh_motion_detail;
    const auto kind=coverageKind(hash);
    if(!enabled || failed || kind==CoverageCount || !ctx || !issue || !n || n>64 || !count || count%3 || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    auto* bound=static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));if(!bound)return;
    Ptr<ID3D11Resource> res;bound->GetResource(&res);Ptr<ID3D11Texture2D> scene;if(FAILED(res.As(&scene)) || !depthProbeIsSceneDepth(scene.Get()))return;
    D3D11_TEXTURE2D_DESC td{};scene->GetDesc(&td);if(td.ArraySize!=1 || td.SampleDesc.Count!=1)return;
    int eye=-1;for(int i=0;i<2;++i){ID3D11Texture2D* s=nullptr;uint32_t fmt=0;if(depthProbeSceneDepthFormat(td.Width,td.Height,i,&s,&fmt) && s==scene.Get()){eye=i;break;}}
    if(eye<0)return;
    Eye& e=eyes[eye];if(e.matched)return; // no history mutation after this eye is consumed
    Ptr<ID3D11DepthStencilState> ds;UINT ref=0;ctx->OMGetDepthStencilState(&ds,&ref);
    D3D11_DEPTH_STENCIL_DESC dd{};if(ds)ds->GetDesc(&dd);else {dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}
    if(!dd.DepthEnable || dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL)return;
    Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;ctx->OMGetBlendState(&blend,factors,&mask);
    D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);if(bd.AlphaToCoverageEnable || bd.RenderTarget[0].BlendEnable)return;
    UINT nv=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&nv,&vp);
    if(nv!=1 || vp.TopLeftX || vp.TopLeftY || vp.Width!=td.Width || vp.Height!=td.Height || vp.MinDepth!=0 || vp.MaxDepth!=1)return;
    D3D11_PRIMITIVE_TOPOLOGY topology;ctx->IAGetPrimitiveTopology(&topology);if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return;
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> dom;ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&dom,nullptr,nullptr);if(gs || hs || dom)return;
    Ptr<ID3D11Predicate> predicate;BOOL pred=FALSE;ctx->GetPredication(&predicate,&pred);if(predicate)return;
    ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);bool busy=false;for(auto* p:so)if(p){busy=true;p->Release();}if(busy)return;
    ID3D11UnorderedAccessView* om[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,om);for(auto* p:om)if(p){busy=true;p->Release();}if(busy)return;
    Ptr<ID3D11Buffer> vb,ids,ib,cb;UINT vertexStride=0,offset=0,idStride=0,idOffset=0,indexOffset=0;DXGI_FORMAT format{};
    ctx->IAGetVertexBuffers(0,1,&ids,&idStride,&idOffset);ctx->IAGetVertexBuffers(1,1,&vb,&vertexStride,&offset);ctx->IAGetIndexBuffer(&ib,&format,&indexOffset);ctx->VSGetConstantBuffers(1,1,&cb);
    if(!vb || !ids || !ib || !cb || idStride!=8 || vertexStride!=40)return;
    const UINT indexBytes=format==DXGI_FORMAT_R16_UINT?2:format==DXGI_FORMAT_R32_UINT?4:0;
    const uint64_t indexFirst=uint64_t(indexOffset)+uint64_t(start)*indexBytes;
    const uint64_t indexEnd=indexFirst+uint64_t(count)*indexBytes;
    if(!indexBytes || indexEnd>UINT64_C(0xffffffff))return;
    D3D11_BUFFER_DESC idd{},cbd{};ids->GetDesc(&idd);cb->GetDesc(&cbd);uint64_t at=uint64_t(idOffset)+uint64_t(startInstance)*8;
    if(at%4 || at+n*8>idd.ByteWidth || cbd.ByteWidth<276*16)return;
    Ptr<ID3D11ShaderResourceView> pool;ctx->VSGetShaderResources(33,1,&pool);if(!pool)return;
    D3D11_SHADER_RESOURCE_VIEW_DESC pd{};pool->GetDesc(&pd);Ptr<ID3D11Resource> pr;pool->GetResource(&pr);Ptr<ID3D11Buffer> pb;
    if(pd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || pd.Buffer.FirstElement || FAILED(pr.As(&pb)))return;
    D3D11_BUFFER_DESC pbd{};pb->GetDesc(&pbd);if(pbd.StructureByteStride!=336 || pd.Buffer.NumElements!=pbd.ByteWidth/336)return;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(!prepare(ctx,dev.Get()) || (e.scene!=scene && !createEye(dev.Get(),scene.Get(),e))){fail();return;}
    auto& now=e.history[e.write];auto& prev=e.history[1-e.write];if(now.count+n>maxRecords){if(!capped){capped=true;Log::get().note("mesh motion: 512 instances per eye reached; excess geometry retains existing motion.");}return;}
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11InputLayout> layout;ctx->VSGetShader(&vs,nullptr,nullptr);ctx->IAGetInputLayout(&layout);
    Settings data{};data.info[0]=now.count;data.info[1]=prev.count;data.info[2]=n;data.dimensions[0]=float(e.width);data.dimensions[1]=float(e.height);
    IUnknown* objects[4]={vs.Get(),vb.Get(),ib.Get(),layout.Get()};
    for(unsigned i=0;i<4;++i){uint64_t key=reinterpret_cast<uint64_t>(objects[i]);data.key[2*i]=UINT(key);data.key[2*i+1]=UINT(key>>32);now.sources[now.count][i]=objects[i];}
    data.key[8]=UINT(base);data.key[9]=start;data.key[10]=count;data.key[11]=vertexStride;data.key[12]=offset;data.key[13]=indexOffset;data.key[14]=UINT(format);
    auto& vertexStamp=watched[vb.Get()];vertexStamp.seen=frames;
    // Inserting the index entry can rehash the map; retain the value, not
    // an iterator. Epochs remain globally ordered for both geometry inputs.
    const unsigned vertexEpoch=vertexStamp.epoch;
    auto& indexStamp=watched[ib.Get()];indexStamp.seen=frames;
    auto& slice=indexStamp.indices.try_emplace((indexFirst<<32)|indexEnd,IndexStamp{indexStamp.epoch,frames}).first->second;
    slice.seen=frames;data.key[15]=std::max(vertexEpoch,slice.epoch);
    bool timed=(++draws&63u)==0 && drawGpu.begin(ctx);
    ctx->UpdateSubresource(settings.Get(),0,nullptr,&data,0,0);
    D3D11_BOX box{UINT(at),0,0,UINT(at+n*8),1,1};ctx->CopySubresourceRegion(instances.Get(),0,0,0,0,ids.Get(),0,&box);
    {
        ComputeState saved(ctx);ID3D11Buffer* cbs[4]={nullptr,cb.Get(),nullptr,settings.Get()};ID3D11ShaderResourceView* srvs[3]={pool.Get(),instanceView.Get(),nullptr};
        ctx->CSSetShader(capture.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,4,cbs);ctx->CSSetShaderResources(0,3,srvs);ctx->CSSetUnorderedAccessViews(0,1,now.uav.GetAddressOf(),nullptr);ctx->Dispatch(1,1,1);
    }
    if(!e.cleared){float zero[4]{};ctx->ClearRenderTargetView(e.rtv.Get(),zero);e.cleared=true;}
    ID3D11RenderTargetView* rt[8]{};Ptr<ID3D11DepthStencilView> originalDepth;ctx->OMGetRenderTargets(8,rt,&originalDepth);
    Ptr<ID3D11PixelShader> ps;ID3D11ClassInstance* classes[256]{};UINT nc=256;ctx->PSGetShader(&ps,classes,&nc);
    Ptr<ID3D11Buffer> psCb;Ptr<ID3D11ShaderResourceView> psSrv;ctx->PSGetConstantBuffers(13,1,&psCb);ctx->PSGetShaderResources(15,1,&psSrv);
    vScreenSetRenderTargetsRaw(ctx,1,e.rtv.GetAddressOf(),originalDepth.Get());ctx->OMSetDepthStencilState(depthState.Get(),0);ctx->OMSetBlendState(blendState.Get(),nullptr,mask);
    ctx->PSSetShader(coverageShaders[kind].Get(),nullptr,0);ctx->PSSetConstantBuffers(13,1,settings.GetAddressOf());ctx->PSSetShaderResources(15,1,now.srv.GetAddressOf());
    issue(ctx,count,n,start,base,startInstance);
    ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(15,1,&none);
    ctx->PSSetShader(ps.Get(),classes,nc);ctx->PSSetConstantBuffers(13,1,psCb.GetAddressOf());ctx->PSSetShaderResources(15,1,psSrv.GetAddressOf());
    vScreenSetRenderTargetsRaw(ctx,8,rt,originalDepth.Get());ctx->OMSetDepthStencilState(ds.Get(),ref);ctx->OMSetBlendState(blend.Get(),factors,mask);
    for(auto* p:rt)if(p)p->Release();for(UINT i=0;i<nc;++i)classes[i]->Release();
    now.count+=n;if(timed)drawGpu.end(ctx);
    if(!noted){noted=true;Log::get().note("mesh motion: exact rigid draw transforms at %ux%u, independent of ship-metres split; original VS coverage, 512 instances per eye, batched GPU history matching. Animated/ambiguous geometry retains existing motion.",e.width,e.height);}
}
void meshMotionViews(ID3D11DeviceContext* ctx,ID3D11Texture2D* scene,ID3D11ShaderResourceView** views){
    using namespace mesh_motion_detail;views[0]=views[1]=nullptr;if(!enabled || failed)return;
    for(auto& e:eyes)if(e.scene.Get()==scene && e.cleared){
        auto& now=e.history[e.write];auto& prev=e.history[1-e.write];
        if(!e.matched){
            bool timed=matchGpu.begin(ctx);ComputeState saved(ctx);Settings data{};data.info[1]=prev.count;
            ctx->UpdateSubresource(settings.Get(),0,nullptr,&data,0,0);
            ctx->CSSetConstantBuffers(3,1,settings.GetAddressOf());ctx->CSSetShaderResources(2,1,prev.srv.GetAddressOf());ctx->CSSetUnorderedAccessViews(0,1,now.uav.GetAddressOf(),nullptr);ctx->CSSetShader(match.Get(),nullptr,0);ctx->Dispatch(now.count,1,1);
            if(timed)matchGpu.end(ctx);e.matched=true;
        }
        views[0]=e.srv.Get();views[1]=now.srv.Get();return;
    }
}
void meshMotionFrameBoundary(ID3D11DeviceContext* ctx){
    using namespace mesh_motion_detail;if(!enabled)return;
    if(ctx){drawGpu.poll(ctx);matchGpu.poll(ctx);}
    if(++frames%1800==0){const auto& d=drawGpu.totals;const auto& m=matchGpu.totals;
        Log::get().note("mesh motion GPU: %u coverage reissues/%u frames; sampled draw+capture %.3f us (%u samples), batched match %.3f us/eye (%u samples); no waits/readbacks.",draws,frames,d.samples?d.ms*1000/d.samples:0,d.samples,m.samples?m.ms*1000/m.samples:0,m.samples);}
    for(auto& e:eyes){
        e.write=1-e.write;auto& h=e.history[e.write];for(unsigned i=0;i<h.count;++i)for(auto& p:h.sources[i])p.Reset();h.count=0;e.cleared=e.matched=false;
        auto& prev=e.history[1-e.write];for(unsigned i=0;i<prev.count;++i)for(unsigned j=1;j<=2;++j)if(prev.sources[i][j])watched[static_cast<ID3D11Resource*>(prev.sources[i][j].Get())].seen=frames;
    }
    for(auto it=watched.begin();it!=watched.end();){
        if(it->second.seen!=frames){it=watched.erase(it);continue;}
        auto& indices=it->second.indices;
        for(auto i=indices.begin();i!=indices.end();)if(frames-i->second.seen>1)i=indices.erase(i);else ++i;
        ++it;
    }
}
void meshMotionResourceWritten(ID3D11Resource* resource,uint64_t first,uint64_t end){
    using namespace mesh_motion_detail;if(!enabled || (resource && watched.find(resource)==watched.end()))return;
    if(resource){
        if(first==end)return; // Empty D3D11 box performs no write.
        if(first>end){first=0;end=~uint64_t(0);} // Unknown extent fails closed.
        ++geometryWrites;
        if(++geometryEpoch){
            auto& stamp=watched[resource];stamp.epoch=geometryEpoch;
            if(first || end!=~uint64_t(0))++rangeWrites;
            for(auto& entry:stamp.indices){
                const uint64_t begin=entry.first>>32,finish=UINT(entry.first);
                if(first<finish && begin<end)entry.second.epoch=geometryEpoch;
                else ++disjointIndices;
            }
            return;
        }
        // Epoch wrap is an unknown generation, so discard conservatively.
    }else ++unknownWrites;
    // A known write changes future correspondence only for its dependents.
    // The old geometry was already rasterised into this eye; its coverage
    // remains correct. Unknown command-list writes still invalidate all.
    for(auto& e:eyes){for(auto& h:e.history){for(unsigned i=0;i<h.count;++i)for(auto& p:h.sources[i])p.Reset();h.count=0;}e.cleared=e.matched=false;}
    watched.clear();
}
void meshMotionShutdown(){
    using namespace mesh_motion_detail;for(auto& e:eyes)e=Eye{};capture.Reset();match.Reset();for(auto& p:coverageShaders)p.Reset();settings.Reset();instances.Reset();instanceView.Reset();depthState.Reset();blendState.Reset();
    failed=noted=capped=false;drawGpu={};matchGpu={};frames=draws=0;watched.clear();geometryEpoch=geometryWrites=unknownWrites=rangeWrites=disjointIndices=0;dump.Reset();dumpCount=0;
}
void meshMotionStageDump(ID3D11DeviceContext* ctx,ID3D11Texture2D* scene){
    using namespace mesh_motion_detail;dump.Reset();dumpCount=0;
    if(!enabled || failed)return;
    for(auto& e:eyes)if(e.scene.Get()==scene && e.cleared && e.matched){
        auto& h=e.history[e.write];if(!h.count)return;
        D3D11_BUFFER_DESC b{};b.ByteWidth=maxRecords*stride;b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(SUCCEEDED(dev->CreateBuffer(&b,nullptr,&dump))){ctx->CopyResource(dump.Get(),h.buffer.Get());dumpCount=h.count;}return;
    }
}
void meshMotionWriteDump(ID3D11DeviceContext* ctx,const wchar_t* directory,const wchar_t* stamp){
    using namespace mesh_motion_detail;
    if(!dump){Log::get().note("mesh motion: eye run %ls has no captured mesh records.",stamp);return;}
    D3D11_MAPPED_SUBRESOURCE mapped{};auto result=ctx->Map(dump.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if(SUCCEEDED(result)){
        unsigned valid=0,matched=0;for(unsigned i=0;i<dumpCount;++i){auto* p=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+i*stride);valid+=p[56]==1;matched+=p[59]==1;}
        wchar_t path[MAX_PATH];_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_Mesh.bin",directory,stamp);FILE* f=nullptr;_wfopen_s(&f,path,L"wb");bool ok=false;
        if(f){const UINT header[2]={dumpCount,stride};ok=fwrite("EDVRMSH1",1,8,f)==8 && fwrite(header,sizeof(header),1,f)==1 && fwrite(mapped.pData,stride,dumpCount,f)==dumpCount;fclose(f);}
        ctx->Unmap(dump.Get(),0);
        Log::get().note("mesh motion: eye run %ls matched %u/%u rigid records (%u total); %s. Sampled capture/reissue %.3f us, match %.3f us/eye.",stamp,matched,valid,dumpCount,ok?"written":"WRITE FAILED",drawGpu.totals.samples?drawGpu.totals.ms*1000/drawGpu.totals.samples:0,matchGpu.totals.samples?matchGpu.totals.ms*1000/matchGpu.totals.samples:0);
        Log::get().note("mesh motion: %u known geometry writes isolated by generation, %u unknown writes reset all history; %zu geometry resources retained.",geometryWrites,unknownWrites,watched.size());
        Log::get().note("mesh motion: %u bounded writes preserved %u disjoint index histories; vertex writes remain conservative.",rangeWrites,disjointIndices);
    }else Log::get().note("mesh motion: eye run %ls readback unavailable (0x%08X).",stamp,unsigned(result));
    dump.Reset();dumpCount=0;
}
}
