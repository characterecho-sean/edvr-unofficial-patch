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
    Ptr<ID3D11Buffer> vertices,indices,positions[2];
    Ptr<ID3D11ShaderResourceView> views[2];
    Ptr<ID3D11GeometryShader> capture;
    unsigned count=0,start=0,offset=0,stride=0,indexOffset=0,frame=~0u,write=0;
    int base=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
};
struct State {
    unsigned frame=0,sourceFrame=~0u,mapFrame=~0u,width=0,height=0,bytes=0;
    bool failed=false,ambiguous=false,noted=false,declined=false;
    Ptr<ID3D11Texture2D> source,map;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
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
    if(!g.vs || !g.ps)return false;
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
    return true;
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
    key.count=count;key.start=start;key.base=base;
    auto matches=[&](const Record& r){return r.original==key.original && r.layout==key.layout && r.vertices==key.vertices && r.indices==key.indices &&
        r.count==count && r.start==start && r.base==base && r.offset==key.offset && r.stride==key.stride && r.format==key.format && r.indexOffset==key.indexOffset;};
    auto found=std::find_if(g.records.begin(),g.records.end(),matches);
    if(found!=g.records.end() && found->frame==g.frame){
        g.ambiguous=true;if(!g.declined){g.declined=true;Log::get().note("weapon motion: repeated mesh identity; rejecting this frame's weapon history.");}return;
    }
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(found==g.records.end()){
        if(g.records.size()>=maxRecords || uint64_t(g.bytes)+count*32>maxBytes)return;
        if(!allocate(dev.Get(),key)){g.failed=true;Log::get().note("weapon motion: stream-output creation failed; weapon history rejected.");return;}
        g.bytes+=count*32;g.records.push_back(std::move(key));found=g.records.end()-1;
    }
    if(!prepare(ctx,dev.Get())){g.failed=true;Log::get().note("weapon motion: resource creation failed; weapon history rejected.");return;}
    Record& r=*found;const unsigned next=1-r.write;const bool valid=r.frame!=~0u && r.frame+1==g.frame;
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
    Ptr<ID3D11Buffer> cb;ctx->PSGetConstantBuffers(0,1,&cb);ID3D11ShaderResourceView* srvs[2]{};ctx->VSGetShaderResources(0,2,srvs);
    float settings[4]={float(g.width),float(g.height),valid?1.f:0.f,0};ctx->UpdateSubresource(g.settings.Get(),0,nullptr,settings,0,0);
    ID3D11ShaderResourceView* in[2]={r.views[next].Get(),r.views[valid?r.write:next].Get()};
    vScreenSetRenderTargetsRaw(ctx,1,g.rtv.GetAddressOf(),depth.Get());ctx->OMSetDepthStencilState(g.depth.Get(),16);ctx->OMSetBlendState(g.blend.Get(),nullptr,~0u);
    ctx->VSSetShader(g.vs.Get(),nullptr,0);ctx->VSSetShaderResources(0,2,in);ctx->PSSetShader(g.ps.Get(),nullptr,0);ctx->PSSetConstantBuffers(0,1,g.settings.GetAddressOf());
    ctx->IASetInputLayout(nullptr);ctx->IASetIndexBuffer(g.sequential.Get(),DXGI_FORMAT_R32_UINT,0);draw(ctx,count,1,0,0,0);
    ID3D11ShaderResourceView* none[2]{};ctx->VSSetShaderResources(0,2,none);
    ctx->IASetInputLayout(r.layout.Get());ctx->IASetIndexBuffer(r.indices.Get(),r.format,r.indexOffset);
    ctx->VSSetShader(vs.Get(),vc,nc);ctx->VSSetShaderResources(0,2,srvs);ctx->PSSetShader(ps.Get(),pc,np);ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf());
    vScreenSetRenderTargetsRaw(ctx,8,rt,depth.Get());ctx->OMSetDepthStencilState(ds.Get(),ref);ctx->OMSetBlendState(blend.Get(),factors,mask);
    for(auto* p:rt)if(p)p->Release();for(auto* p:srvs)if(p)p->Release();for(UINT i=0;i<np;++i)pc[i]->Release();for(UINT i=0;i<nc;++i)vc[i]->Release();
    r.frame=g.frame;r.write=next;
    if(valid && !g.noted){g.noted=true;Log::get().note("weapon motion: original animated vertices supply source motion at %ux%u; aiming, skinning and projection included. GPU-only, bounded 32 MiB vertex history.",g.width,g.height);}
}
ID3D11ShaderResourceView* weaponMotionView(){using namespace weapon_motion_detail;return enabled && !g.failed && !g.ambiguous && g.mapFrame==g.frame?g.srv.Get():nullptr;}
void weaponMotionFrameBoundary(){
    using namespace weapon_motion_detail;++g.frame;g.ambiguous=false;
    for(auto it=g.records.begin();it!=g.records.end();)if(g.frame-it->frame>2){g.bytes-=it->count*32;it=g.records.erase(it);}else ++it;
    if(g.source && g.frame-g.sourceFrame>120){unsigned frame=g.frame;g=State{};g.frame=frame;}
}
void weaponMotionShutdown(){weapon_motion_detail::g=weapon_motion_detail::State{};}
void weaponMotionResourceWritten(ID3D11Resource* resource){
    using namespace weapon_motion_detail;if(!enabled)return;
    for(auto& r:g.records)if(!resource || resource==r.vertices.Get() || resource==r.indices.Get()){
        r.frame=~0u;g.mapFrame=~0u;
    }
}
}
