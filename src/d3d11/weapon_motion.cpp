#include "weapon_motion.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <algorithm>
#include "shader_swap.h"
#include "vscreen.h"
#include "../common/log.h"
namespace edvr { namespace weapon_motion_detail {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
const GUID bytecodeKey={0x65a40e9c,0xa4ee,0x473d,{0x85,0x4a,0xeb,0x10,0x35,0x8e,0x4f,0x20}};
constexpr unsigned maxVertices=131072,maxRecords=64,maxBytes=32*1024*1024;
bool enabled=false;
struct Record {
    Ptr<ID3D11VertexShader> original;
    Ptr<ID3D11InputLayout> layout;
    Ptr<ID3D11Buffer> vertices,indices,positions[2],identity[2];
    Ptr<ID3D11ShaderResourceView> identityViews[2];
    Ptr<ID3D11UnorderedAccessView> identityUavs[2];
    Ptr<ID3D11ShaderResourceView> views[2];
    Ptr<ID3D11GeometryShader> capture;
    unsigned count=0,start=0,offset=0,stride=0,indexOffset=0,frame[2]={~0u,~0u};
    int base=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
};
struct State {
    unsigned frame=0,sourceFrame=~0u,mapFrame=~0u,width=0,height=0,bytes=0,invalidationNotes=0;
    bool failed=false,ambiguous=false,noted=false,declined=false;
    Ptr<ID3D11Texture2D> source,map;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11ComputeShader> identify;
    Ptr<ID3D11Buffer> instance;
    Ptr<ID3D11ShaderResourceView> instanceView;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11DepthStencilState> depth;
    Ptr<ID3D11BlendState> blend;
    Ptr<ID3D11Buffer> settings,sequential;
    std::vector<Record> records;
} g;
bool family(uint64_t hash){
    return hash==0x7B0DC42D383F694Cull || hash==0x8B589D25B2A0ADDCull ||
        hash==0x114AF608F86D9ED8ull || hash==0xAACFDCF2FB9AD809ull || hash==0x174E8D76363BE337ull;
}
bool prepare(ID3D11DeviceContext* ctx,ID3D11Device* dev){
    if(g.rtv)return true;
    g.vs.Attach(shaderSwapCompileVs(ctx,kWeaponMotionVs,sizeof(kWeaponMotionVs)-1,"main","weapon motion",nullptr,"weapon motion"));
    g.ps.Attach(shaderSwapCompilePs(ctx,kWeaponMotionPs,sizeof(kWeaponMotionPs)-1,"main","weapon motion",nullptr,"weapon motion"));
    g.identify.Attach(shaderSwapCompileCs(ctx,kWeaponIdentityCs,sizeof(kWeaponIdentityCs)-1,"main","weapon identity",nullptr,"weapon motion"));
    if(!g.vs || !g.ps || !g.identify)return false;
    D3D11_BUFFER_DESC id{};id.ByteWidth=16;id.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_UINT;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=4;
    if(FAILED(dev->CreateBuffer(&id,nullptr,&g.instance)) || FAILED(dev->CreateShaderResourceView(g.instance.Get(),&view,&g.instanceView)))return false;
    D3D11_TEXTURE2D_DESC td{};td.Width=g.width;td.Height=g.height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    // Keep depth alongside motion so later, untracked opaque draws cannot
    // inherit vectors belonging to an occluded mesh. Half-depth comparison
    // at the consumer includes only its representational rounding error.
    td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&g.map)) || FAILED(dev->CreateRenderTargetView(g.map.Get(),nullptr,&g.rtv)) ||
       FAILED(dev->CreateShaderResourceView(g.map.Get(),nullptr,&g.srv)))return false;
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_EQUAL;ds.StencilEnable=TRUE;ds.StencilReadMask=16;
    ds.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;ds.BackFace=ds.FrontFace;
    D3D11_BLEND_DESC bd{};bd.RenderTarget[0].RenderTargetWriteMask=15;
    if(FAILED(dev->CreateDepthStencilState(&ds,&g.depth)) || FAILED(dev->CreateBlendState(&bd,&g.blend)))return false;
    D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&g.settings)))return false;
    std::vector<unsigned> indices(maxVertices);for(unsigned i=0;i<maxVertices;++i)indices[i]=i;
    b.ByteWidth=maxVertices*4;b.BindFlags=D3D11_BIND_INDEX_BUFFER;b.Usage=D3D11_USAGE_IMMUTABLE;D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=indices.data();
    return SUCCEEDED(dev->CreateBuffer(&b,&initial,&g.sequential));
}
bool allocate(ID3D11Device* dev,Record& r){
    for(const auto& cached:g.records)if(cached.original==r.original){r.capture=cached.capture;break;}
    if(!r.capture){
        UINT size=0;if(FAILED(r.original->GetPrivateData(bytecodeKey,&size,nullptr)) || !size || size>65536)return false;
        std::vector<unsigned char> bytes(size);if(FAILED(r.original->GetPrivateData(bytecodeKey,&size,bytes.data())))return false;
        D3D11_SO_DECLARATION_ENTRY entry{0,"SV_POSITION",0,0,4,0};UINT stride=16;
        if(FAILED(dev->CreateGeometryShaderWithStreamOutput(bytes.data(),size,&entry,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&r.capture)))return false;
    }
    D3D11_BUFFER_DESC b{};b.ByteWidth=r.count*16;b.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;s.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;s.Buffer.NumElements=r.count;
    for(int i=0;i<2;++i)if(FAILED(dev->CreateBuffer(&b,nullptr,&r.positions[i])) || FAILED(dev->CreateShaderResourceView(r.positions[i].Get(),&s,&r.views[i])))return false;
    b.ByteWidth=16;b.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=16;
    for(int i=0;i<2;++i)if(FAILED(dev->CreateBuffer(&b,nullptr,&r.identity[i])) ||
        FAILED(dev->CreateShaderResourceView(r.identity[i].Get(),nullptr,&r.identityViews[i])) ||
        FAILED(dev->CreateUnorderedAccessView(r.identity[i].Get(),nullptr,&r.identityUavs[i])))return false;
    return true;
}
void identify(ID3D11DeviceContext* ctx,ID3D11Buffer* instances,UINT offset,ID3D11ShaderResourceView* pool,Record& r,UINT next){
    // Copy only the current instance index. Resolve its stable skeleton
    // allocation on the GPU; instance slots themselves move every frame.
    D3D11_BOX box{offset,0,0,offset+4,1,1};ctx->CopySubresourceRegion(g.instance.Get(),0,0,0,0,instances,0,&box);
    Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT count=256;ctx->CSGetShader(&saved,classes,&count);
    ID3D11ShaderResourceView* srvs[2]{};ctx->CSGetShaderResources(0,2,srvs);Ptr<ID3D11UnorderedAccessView> uav;ctx->CSGetUnorderedAccessViews(0,1,&uav);
    ID3D11ShaderResourceView* in[2]={g.instanceView.Get(),pool};ctx->CSSetShaderResources(0,2,in);ctx->CSSetUnorderedAccessViews(0,1,r.identityUavs[next].GetAddressOf(),nullptr);
    ctx->CSSetShader(g.identify.Get(),nullptr,0);ctx->Dispatch(1,1,1);
    ID3D11ShaderResourceView* none[2]{};ID3D11UnorderedAccessView* noUav=nullptr;
    ctx->CSSetShaderResources(0,2,none);ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
    ctx->CSSetShader(saved.Get(),classes,count);ctx->CSSetShaderResources(0,2,srvs);UINT keep=~0u;ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),&keep);
    for(auto* p:srvs)if(p)p->Release();for(UINT i=0;i<count;++i)classes[i]->Release();
}
} // namespace weapon_motion_detail
void weaponMotionRememberShader(ID3D11VertexShader* vs,uint64_t hash,const void* bytes,size_t size){
    if(vs && bytes && size && size<=65536 && weapon_motion_detail::family(hash))
        vs->SetPrivateData(weapon_motion_detail::bytecodeKey,UINT(size),bytes);
}
void weaponMotionConfigure(bool on){if(on!=weapon_motion_detail::enabled){weaponMotionShutdown();weapon_motion_detail::enabled=on;}}
void weaponMotionSource(ID3D11Texture2D* source){
    using namespace weapon_motion_detail;if(!enabled || !source)return;
    D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || td.ArraySize!=1 || td.SampleDesc.Count!=1 || uint64_t(td.Width)*td.Height>16*1024*1024)return;
    if(g.width!=td.Width || g.height!=td.Height){unsigned frame=g.frame;g=State{};g.frame=frame;g.width=td.Width;g.height=td.Height;}
    g.source=source;g.sourceFrame=g.frame;
}
void weaponMotionDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,unsigned start,int base,unsigned startInstance){
    using namespace weapon_motion_detail;
    if(!enabled || g.failed || g.sourceFrame!=g.frame || !ctx || !draw || instances!=1 || !count || count%3 || count>maxVertices || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    Ptr<ID3D11DepthStencilState> ds;UINT ref=0;ctx->OMGetDepthStencilState(&ds,&ref);if(!ds || !(ref&16))return;
    D3D11_DEPTH_STENCIL_DESC dd{};ds->GetDesc(&dd);
    if(!dd.DepthEnable || dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL || !dd.StencilEnable || !(dd.StencilWriteMask&16) ||
       dd.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE || dd.BackFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE)return;
    Ptr<ID3D11DepthStencilView> depth;ctx->OMGetRenderTargets(0,nullptr,&depth);if(!depth)return;
    Ptr<ID3D11Resource> resource;depth->GetResource(&resource);if(resource.Get()!=g.source.Get())return;
    D3D11_VIEWPORT vp{};UINT nv=1;ctx->RSGetViewports(&nv,&vp);
    if(nv!=1 || vp.TopLeftX || vp.TopLeftY || vp.Width!=g.width || vp.Height!=g.height || vp.MinDepth!=0 || vp.MaxDepth!=1)return;
    D3D11_PRIMITIVE_TOPOLOGY topology;ctx->IAGetPrimitiveTopology(&topology);if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return;
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> dom;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&dom,nullptr,nullptr);if(gs || hs || dom)return;
    Ptr<ID3D11Predicate> predicate;BOOL pred=FALSE;ctx->GetPredication(&predicate,&pred);if(predicate)return;
    ID3D11Buffer* targets[4]{};ctx->SOGetTargets(4,targets);bool busy=false;for(auto* p:targets)if(p){busy=true;p->Release();}if(busy)return;
    ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);for(auto* p:uavs)if(p){busy=true;p->Release();}if(busy)return;
    Record key;ctx->VSGetShader(&key.original,nullptr,nullptr);ctx->IAGetInputLayout(&key.layout);
    ctx->IAGetVertexBuffers(1,1,&key.vertices,&key.stride,&key.offset);ctx->IAGetIndexBuffer(&key.indices,&key.format,&key.indexOffset);
    if(!key.original || !key.layout || !key.vertices || !key.indices)return;
    UINT codeSize=0;if(FAILED(key.original->GetPrivateData(bytecodeKey,&codeSize,nullptr)) || !codeSize)return;
    Ptr<ID3D11Buffer> instance;UINT instanceStride=0,instanceOffset=0;ctx->IAGetVertexBuffers(0,1,&instance,&instanceStride,&instanceOffset);
    Ptr<ID3D11ShaderResourceView> pool;ctx->VSGetShaderResources(33,1,&pool);if(!instance || !pool || instanceStride!=8)return;
    D3D11_BUFFER_DESC id{};instance->GetDesc(&id);const uint64_t address=uint64_t(instanceOffset)+uint64_t(startInstance)*8;
    D3D11_SHADER_RESOURCE_VIEW_DESC pd{};pool->GetDesc(&pd);Ptr<ID3D11Resource> pr;pool->GetResource(&pr);Ptr<ID3D11Buffer> pb;
    if(address%4 || address+4>id.ByteWidth || pd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || pd.Buffer.FirstElement || FAILED(pr.As(&pb)))return;
    D3D11_BUFFER_DESC bd{};pb->GetDesc(&bd);if(bd.StructureByteStride!=336 || pd.Buffer.NumElements!=bd.ByteWidth/336)return;
    key.count=count;key.start=start;key.base=base;
    auto matches=[&](const Record& r){return r.original==key.original && r.layout==key.layout && r.vertices==key.vertices && r.indices==key.indices &&
        r.count==count && r.start==start && r.base==base && r.offset==key.offset && r.stride==key.stride && r.format==key.format && r.indexOffset==key.indexOffset;};
    // The world-body and viewmodel draw the same arm mesh with distinct
    // skeleton allocations (92 and 740) and clip-Z projections. Instance slots
    // move every frame. Keep bounded occurrences, then match prior vertices
    // by skeleton and original clip Z on the GPU, independently of draw order.
    const unsigned next=g.frame&1,previous=1-next;
    ID3D11ShaderResourceView* histories[4]{},*identities[4]{};unsigned candidates=0,occurrences=0;
    for(const auto& r:g.records)if(matches(r)){
        if(r.frame[next]==g.frame)++occurrences;
        if(r.frame[previous]!=~0u && r.frame[previous]+1==g.frame){
            if(candidates==4){g.ambiguous=true;return;}
            histories[candidates]=r.views[previous].Get();identities[candidates++]=r.identityViews[previous].Get();
        }
    }
    if(occurrences==4){g.ambiguous=true;if(!g.declined){g.declined=true;Log::get().note("weapon motion: more than four mesh occurrences; rejecting this frame's weapon history.");}return;}
    auto found=std::find_if(g.records.begin(),g.records.end(),[&](const Record& r){return matches(r) && r.frame[next]!=g.frame;});
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(found==g.records.end()){
        if(g.records.size()>=maxRecords || uint64_t(g.bytes)+count*32>maxBytes)return;
        if(!allocate(dev.Get(),key)){g.failed=true;Log::get().note("weapon motion: stream-output creation failed; weapon history rejected.");return;}
        g.bytes+=count*32;g.records.push_back(std::move(key));found=g.records.end()-1;
    }
    if(!prepare(ctx,dev.Get())){g.failed=true;Log::get().note("weapon motion: resource creation failed; weapon history rejected.");return;}
    Record& r=*found;const bool valid=candidates!=0;
    identify(ctx,instance.Get(),UINT(address),pool.Get(),r,next);
    if(g.mapFrame!=g.frame){float zero[4]{};ctx->ClearRenderTargetView(g.rtv.Get(),zero);g.mapFrame=g.frame;}
    // Capture indexed vertices as POINTLIST. Unlike triangle SO this keeps
    // repeated/degenerate indices, so vertex k has a stable correspondence.
    UINT zero=0;ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);ctx->GSSetShader(r.capture.Get(),nullptr,0);
    ctx->SOSetTargets(1,r.positions[next].GetAddressOf(),&zero);draw(ctx,count,1,start,base,startInstance);
    ctx->SOSetTargets(0,nullptr,nullptr);ctx->GSSetShader(nullptr,nullptr,0);ctx->IASetPrimitiveTopology(topology);
    ID3D11RenderTargetView* rt[8]{};ctx->OMGetRenderTargets(8,rt,nullptr);
    Ptr<ID3D11BlendState> blend;FLOAT factors[4];UINT mask;ctx->OMGetBlendState(&blend,factors,&mask);
    Ptr<ID3D11PixelShader> ps;ID3D11ClassInstance* pc[256]{},*vc[256]{};UINT np=256,nc=256;
    ctx->PSGetShader(&ps,pc,&np);Ptr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,vc,&nc);
    Ptr<ID3D11Buffer> cb,vsCb;ctx->PSGetConstantBuffers(0,1,&cb);ctx->VSGetConstantBuffers(0,1,&vsCb);ID3D11ShaderResourceView* srvs[10]{};ctx->VSGetShaderResources(0,10,srvs);
    float settings[4]={float(g.width),float(g.height),float(candidates),0};ctx->UpdateSubresource(g.settings.Get(),0,nullptr,settings,0,0);
    ID3D11ShaderResourceView* in[10]={r.views[next].Get(),histories[0],histories[1],histories[2],histories[3],r.identityViews[next].Get(),identities[0],identities[1],identities[2],identities[3]};
    vScreenSetRenderTargetsRaw(ctx,1,g.rtv.GetAddressOf(),depth.Get());ctx->OMSetDepthStencilState(g.depth.Get(),16);ctx->OMSetBlendState(g.blend.Get(),nullptr,~0u);
    ctx->VSSetShader(g.vs.Get(),nullptr,0);ctx->VSSetShaderResources(0,10,in);ctx->VSSetConstantBuffers(0,1,g.settings.GetAddressOf());ctx->PSSetShader(g.ps.Get(),nullptr,0);ctx->PSSetConstantBuffers(0,1,g.settings.GetAddressOf());
    ctx->IASetInputLayout(nullptr);ctx->IASetIndexBuffer(g.sequential.Get(),DXGI_FORMAT_R32_UINT,0);draw(ctx,count,1,0,0,0);
    ID3D11ShaderResourceView* none[10]{};ctx->VSSetShaderResources(0,10,none);
    ctx->IASetInputLayout(r.layout.Get());ctx->IASetIndexBuffer(r.indices.Get(),r.format,r.indexOffset);
    ctx->VSSetShader(vs.Get(),vc,nc);ctx->VSSetShaderResources(0,10,srvs);ctx->VSSetConstantBuffers(0,1,vsCb.GetAddressOf());ctx->PSSetShader(ps.Get(),pc,np);ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf());
    vScreenSetRenderTargetsRaw(ctx,8,rt,depth.Get());ctx->OMSetDepthStencilState(ds.Get(),ref);ctx->OMSetBlendState(blend.Get(),factors,mask);
    for(auto* p:rt)if(p)p->Release();for(auto* p:srvs)if(p)p->Release();for(UINT i=0;i<np;++i)pc[i]->Release();for(UINT i=0;i<nc;++i)vc[i]->Release();
    r.frame[next]=g.frame;
    if(valid && !g.noted){g.noted=true;Log::get().note("weapon motion: original animated vertices supply skeleton/projection-matched source motion at %ux%u; aiming, skinning and projection included. GPU-only, bounded 32 MiB vertex history.",g.width,g.height);}
}
ID3D11ShaderResourceView* weaponMotionView(){using namespace weapon_motion_detail;return enabled && !g.failed && !g.ambiguous && g.mapFrame==g.frame?g.srv.Get():nullptr;}
void weaponMotionFrameBoundary(){
    using namespace weapon_motion_detail;++g.frame;g.ambiguous=false;
    for(auto it=g.records.begin();it!=g.records.end();)if((it->frame[0]==~0u || g.frame-it->frame[0]>2) && (it->frame[1]==~0u || g.frame-it->frame[1]>2)){g.bytes-=it->count*32;it=g.records.erase(it);}else ++it;
    if(g.source && g.frame-g.sourceFrame>120){unsigned frame=g.frame;g=State{};g.frame=frame;}
}
void weaponMotionShutdown(){weapon_motion_detail::g=weapon_motion_detail::State{};}
void weaponMotionResourceWritten(ID3D11Resource* resource){
    using namespace weapon_motion_detail;if(!enabled)return;
    for(auto& r:g.records)if(!resource || resource==r.vertices.Get() || resource==r.indices.Get()){
        const unsigned reason=!resource?1:resource==r.vertices.Get()?2:4;
        if(!(g.invalidationNotes&reason)){
            g.invalidationNotes|=reason;
            Log::get().note("weapon motion: history invalidated by %s; correspondence will restart on the next captured draw.",
                reason==1?"unknown command-list resource writes":reason==2?"mesh vertex-buffer write":"mesh index-buffer write");
        }
        r.frame[0]=r.frame[1]=~0u;g.mapFrame=~0u;
    }
}
}
