// Production post-VS history, animated perspective projection and state
// restoration. No game assets or CPU readback exists in the production path.
#include "../../src/d3d11/weapon_motion.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <DirectXPackedVector.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool b,const char* s){++checks;if(!b){std::printf("FAIL: %s\n",s);std::exit(1);}}
void hr(HRESULT h){if(FAILED(h))std::printf("HRESULT %08X\n",unsigned(h));check(SUCCEEDED(h),"D3D operation");}
ComPtr<ID3DBlob> compile(const char* s,const char* profile){ComPtr<ID3DBlob> c,e;auto h=D3DCompile(s,strlen(s),nullptr,nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&c,&e);if(FAILED(h)&&e)std::puts(static_cast<const char*>(e->GetBufferPointer()));hr(h);return c;}
namespace edvr {
Log& Log::get(){static Log l;return l;}Log::~Log()=default;void Log::note(const char*,...){}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* r,ID3D11DepthStencilView* d){c->OMSetRenderTargets(n,r,d);}
ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext* c,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){auto b=compile(s,"vs_5_0");ComPtr<ID3D11Device> d;c->GetDevice(&d);ID3D11VertexShader* v=nullptr;hr(d->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* c,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){auto b=compile(s,"ps_5_0");ComPtr<ID3D11Device> d;c->GetDevice(&d);ID3D11PixelShader* v=nullptr;hr(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* c,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){auto b=compile(s,"cs_5_0");ComPtr<ID3D11Device> d;c->GetDevice(&d);ID3D11ComputeShader* v=nullptr;hr(d->CreateComputeShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}

}
using namespace edvr;
void __stdcall issue(ID3D11DeviceContext* c,unsigned n,unsigned instances,unsigned start,int base,unsigned si){c->DrawIndexedInstanced(n,instances,start,base,si);}
struct F4 {double x,y,z,w;};
struct Pose {float mouse=0,projection=1,nearBone=0,farBone=0,clip=.025f,skeleton=92,pad[2]{};};
F4 vertex(int i,Pose p){const double x=i==0||i==3?-.6:.6,y=i<2?-.6:.6,t=i<2?0:1;double z=1+t*.25;return {(x+p.mouse+p.nearBone*(1-t)+p.farBone*t)*p.projection,y,p.clip,z};}
int main(int argc,char** argv){
 const UINT W=128,H=96;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL fl;
 const auto driver=argc>1&&!strcmp(argv[1],"--hardware")?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP;
 auto made=D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
 if(made==DXGI_ERROR_SDK_COMPONENT_MISSING)made=D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);hr(made);
 ComPtr<ID3D11InfoQueue> queue;dev.As(&queue);
 auto buffer=[&](const void* data,UINT bytes,UINT bind,UINT stride=0){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=bind;d.StructureByteStride=stride;d.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;D3D11_SUBRESOURCE_DATA sd{};sd.pSysMem=data;ComPtr<ID3D11Buffer>b;hr(dev->CreateBuffer(&d,data?&sd:nullptr,&b));return b;};
 D3D11_TEXTURE2D_DESC td{};td.Width=W;td.Height=H;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G8X24_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture2D> depth,colour;hr(dev->CreateTexture2D(&td,nullptr,&depth));D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
 ComPtr<ID3D11DepthStencilView> dsv;hr(dev->CreateDepthStencilView(depth.Get(),&dd,&dsv));
 td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;hr(dev->CreateTexture2D(&td,nullptr,&colour));ComPtr<ID3D11RenderTargetView> rtv;hr(dev->CreateRenderTargetView(colour.Get(),nullptr,&rtv));
 D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.StencilEnable=TRUE;ds.StencilWriteMask=255;ds.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;ds.BackFace=ds.FrontFace;
 ComPtr<ID3D11DepthStencilState> state;hr(dev->CreateDepthStencilState(&ds,&state));
 D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;hr(dev->CreateRasterizerState(&rs,&raster));ctx->RSSetState(raster.Get());
 const char* source="cbuffer P:register(b1){float mouse,projection,a,b;float clip;float3 pad;} StructuredBuffer<float4> Bones:register(t38);struct O{uint2 extra:DATA0;float4 normal:DATA1;float4 p:SV_Position;};O main(float4 p:POSITION){O o;o.extra=0;o.normal=1;o.p=float4((p.x+mouse+lerp(Bones[0].x,Bones[1].x,p.w))*projection,p.y,clip,p.z);return o;}";
 auto code=compile(source,"vs_5_0"),pc=compile("float4 main():SV_Target{return 1;}","ps_5_0");ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;hr(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));hr(dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps));
 weaponMotionRememberShader(vs.Get(),0x7B0DC42D383F694Cull,code->GetBufferPointer(),code->GetBufferSize());
 D3D11_INPUT_ELEMENT_DESC el{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D11_INPUT_PER_VERTEX_DATA,0};ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(&el,1,code->GetBufferPointer(),code->GetBufferSize(),&layout));
 float vertices[]={-.6f,-.6f,1,0,.6f,-.6f,1,0,.6f,.6f,1.25f,1,-.6f,.6f,1.25f,1};UINT indices[]={0,1,2,0,2,3,0,0,0};
 auto vb=buffer(vertices,sizeof(vertices),D3D11_BIND_VERTEX_BUFFER),ib=buffer(indices,sizeof(indices),D3D11_BIND_INDEX_BUFFER),cb=buffer(nullptr,32,D3D11_BIND_CONSTANT_BUFFER),bones=buffer(nullptr,32,D3D11_BIND_SHADER_RESOURCE,16);
 unsigned poolData[168]{};poolData[0]=92;poolData[84]=740;poolData[7]=poolData[91]=1631;
 auto pool=buffer(poolData,sizeof(poolData),D3D11_BIND_SHADER_RESOURCE,336),instance=buffer(nullptr,16,D3D11_BIND_VERTEX_BUFFER);
 ComPtr<ID3D11ShaderResourceView> poolView;hr(dev->CreateShaderResourceView(pool.Get(),nullptr,&poolView));
 ComPtr<ID3D11ShaderResourceView> boneView;hr(dev->CreateShaderResourceView(bones.Get(),nullptr,&boneView));
 auto bind=[&](Pose p,bool advance=true){if(advance)weaponMotionFrameBoundary();weaponMotionSource(depth.Get());ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());ctx->OMSetDepthStencilState(state.Get(),21);ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0,4);D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster.Get());ctx->IASetInputLayout(layout.Get());UINT stride=16,off=0;ctx->IASetVertexBuffers(1,1,vb.GetAddressOf(),&stride,&off);UINT instanceStride=8;ctx->IASetVertexBuffers(0,1,instance.GetAddressOf(),&instanceStride,&off);
 unsigned ids[4]={p.skeleton==740?1u:0u,526606,0,0};ctx->UpdateSubresource(instance.Get(),0,nullptr,ids,0,0);ctx->VSSetShaderResources(33,1,poolView.GetAddressOf());ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->VSSetConstantBuffers(1,1,cb.GetAddressOf());ctx->UpdateSubresource(cb.Get(),0,nullptr,&p,0,0);float b[8]={p.nearBone,0,0,0,p.farBone,0,0,0};ctx->UpdateSubresource(bones.Get(),0,nullptr,b,0,0);ctx->VSSetShaderResources(38,1,boneView.GetAddressOf());issue(ctx.Get(),9,1,0,0,0);};
 auto motion=[&](){weaponMotionDraw(ctx.Get(),issue,9,1,0,0,0);};
 auto read=[&](){auto& g=weapon_motion_detail::g;D3D11_TEXTURE2D_DESC d{};g.map->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> s;hr(dev->CreateTexture2D(&d,nullptr,&s));ctx->CopyResource(s.Get(),g.map.Get());D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(s.Get(),0,D3D11_MAP_READ,0,&m));std::vector<float> out(W*H*4);for(UINT y=0;y<H;++y)for(UINT x=0;x<W*4;++x)out[y*W*4+x]=DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(m.pData)+y*m.RowPitch)[x]);ctx->Unmap(s.Get(),0);return out;};
 weaponMotionConfigure(true);Pose old{};bind(old);motion();check(weaponMotionView()!=nullptr,"first mesh writes rejection coverage");auto first=read();unsigned covered=0;for(UINT i=0;i<W*H;++i)if(first[i*4+3]){++covered;check(first[i*4+3]==2,"new mesh has no fabricated history");}check(covered>2000,"rasterized weapon coverage present");
 for(Pose now: {Pose{},Pose{.08f,1,0,0},Pose{-.06f,1.15f,.01f,-.025f},Pose{.03f,.9f,-.02f,.04f}}){
  bind(now);motion();check(weaponMotionView()!=nullptr,"animated mesh has valid motion");auto a=read();covered=0;
  for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){UINT at=(y*W+x)*4;if(a[at+3]!=1)continue;++covered;bool matched=false;
   for(int tri=0;tri<2&&!matched;++tri){F4 n[3],b[3];double sx[3],sy[3];for(int k=0;k<3;++k){int id=indices[tri*3+k];n[k]=vertex(id,now);b[k]=vertex(id,old);sx[k]=(n[k].x/n[k].w*.5+.5)*W;sy[k]=(-n[k].y/n[k].w*.5+.5)*H;}
    double det=(sy[1]-sy[2])*(sx[0]-sx[2])+(sx[2]-sx[1])*(sy[0]-sy[2]);double l[3];l[0]=((sy[1]-sy[2])*(x+.5-sx[2])+(sx[2]-sx[1])*(y+.5-sy[2]))/det;l[1]=((sy[2]-sy[0])*(x+.5-sx[2])+(sx[0]-sx[2])*(y+.5-sy[2]))/det;l[2]=1-l[0]-l[1];if(*std::min_element(l,l+3)<-.001)continue;
    double px=0,py=0,pw=0;for(int k=0;k<3;++k){px+=l[k]*b[k].x/n[k].w;py+=l[k]*b[k].y/n[k].w;pw+=l[k]*b[k].w/n[k].w;}
    check(std::fabs(a[at]-((px/pw*.5+.5)*W-x-.5))<.02 && std::fabs(a[at+1]-((-py/pw*.5+.5)*H-y-.5))<.02,"per-pixel motion matches independent animated perspective projection");matched=true;
   }check(matched,"every covered pixel belongs to current mesh");
  }check(covered>2000,"valid history covers animated mesh");
  ComPtr<ID3D11VertexShader> restored;ctx->VSGetShader(&restored,nullptr,nullptr);check(restored==vs,"original VS restored");ComPtr<ID3D11InputLayout> il;ctx->IAGetInputLayout(&il);check(il==layout,"layout restored");ComPtr<ID3D11Buffer> ix;DXGI_FORMAT fmt;UINT offset;ctx->IAGetIndexBuffer(&ix,&fmt,&offset);check(ix==ib&&fmt==DXGI_FORMAT_R32_UINT&&offset==0,"index binding restored");ComPtr<ID3D11DepthStencilState> d;UINT ref;ctx->OMGetDepthStencilState(&d,&ref);check(d==state&&ref==21,"depth/stencil restored");
  old=now;
 }
 // ClearState changes no resource content. Rebinding the next frame
 // must retain post-VS history, as it does through the production hook.
 ctx->ClearState();bind(old);motion();first=read();covered=0;
 for(UINT i=0;i<W*H;++i)if(first[i*4+3]==1)++covered;
 check(covered>2000,"ClearState and rebind preserve original-vertex history");
 // World arms and viewmodel share topology, with distinct captured clip Z.
 // Reorder both occurrences, then remove one: match by projection on GPU,
 // never by volatile instance number or the occurrence's CPU slot.
 weaponMotionShutdown();Pose world{-.1f,1,0,0,.0675f},view{.1f,1,0,0,.025f,740};
 bind(world);motion();bind(view,false);motion();
 for(int order=0;order<3;++order){
     Pose a=order%2?world:view,b=order%2?view:world;
     bind(a);motion();first=read();covered=0;
     for(UINT i=0;i<W*H;++i)if(first[i*4+3]){check(first[i*4+3]==1 && std::fabs(first[i*4])<.02 && std::fabs(first[i*4+1])<.02,"reordered mesh selects its own projection history");++covered;}
     check(covered>2000,"first projection coverage");
     bind(b,false);motion();first=read();covered=0;
     for(UINT i=0;i<W*H;++i)if(first[i*4+3]==1 && std::fabs(first[i*4])<.02 && std::fabs(first[i*4+1])<.02)++covered;
     check(covered>2000 && weaponMotionView(),"both projections retain motion map");
 }
 // Aiming can put both skeletons under the same near plane. Their
 // separate skeleton identities must still match, even in reversed order.
 world.clip=.025f;bind(world);motion();bind(view,false);motion();
 for(Pose p:{view,world}){bind(p,p.skeleton==740);motion();first=read();covered=0;
  for(UINT i=0;i<W*H;++i)if(first[i*4+3]==1 && std::fabs(first[i*4])<.02 && std::fabs(first[i*4+1])<.02)++covered;
  check(covered>2000,"same-projection body and viewmodel match their own skeletons");}
 bind(view);motion();first=read();covered=0;for(UINT i=0;i<W*H;++i)if(first[i*4+3]==1)++covered;
 check(covered>2000,"missing world draw does not mispair viewmodel");
 // Same-projection duplicates are ambiguous. Reject only their coverage;
 // the map remains available to unrelated weapon/tool geometry.
 motion();bind(view);motion();first=read();for(UINT i=0;i<W*H;++i)check(first[i*4+3]!=1,"ambiguous equal-projection histories are never guessed");
 check(weaponMotionView()!=nullptr,"ambiguous mesh does not discard the whole weapon map");
 weaponMotionFrameBoundary();weaponMotionFrameBoundary();weaponMotionFrameBoundary();check(!weaponMotionView()&&weapon_motion_detail::g.records.empty(),"missing frames discard stale mesh history");
 bind(old);motion();first=read();for(UINT i=0;i<W*H;++i)check(first[i*4+3]!=1,"returning mesh has no old animation history");
 weaponMotionConfigure(false);check(!weaponMotionView()&&!weapon_motion_detail::g.map,"off frees temporal weapon resources");bind(old);motion();check(!weaponMotionView(),"live off stays inactive");
 weaponMotionConfigure(true);bind(old);motion();check(weaponMotionView()!=nullptr,"live on resumes with fresh coverage");
 // Bound GPU resource growth; these states must decline before a draw.
 unsigned allocated=weapon_motion_detail::g.bytes;weaponMotionDraw(ctx.Get(),issue,131073,1,0,0,0);weaponMotionDraw(ctx.Get(),issue,9,2,0,0,0);check(weapon_motion_detail::g.bytes==allocated,"oversized and instanced meshes allocate nothing");
 weaponMotionResourceWritten(bones.Get());check(weaponMotionView()!=nullptr,"animation updates preserve vertex correspondence");
 weaponMotionResourceWritten(ib.Get());check(!weaponMotionView(),"rewritten mesh indices invalidate motion");bind(old);motion();first=read();for(UINT i=0;i<W*H;++i)check(first[i*4+3]!=1,"rewritten mesh cannot reuse old correspondence");
 weaponMotionShutdown();ctx->ClearState();
 if(queue)for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;queue->GetMessage(i,nullptr,&n);std::vector<char> bytes(n);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());hr(queue->GetMessage(i,m,&n));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::puts(m->pDescription);check(false,"no D3D warnings/errors");}}
 std::printf("weapon motion: %u checks passed (%s)\n",checks,driver==D3D_DRIVER_TYPE_WARP?"WARP":"hardware");
}
