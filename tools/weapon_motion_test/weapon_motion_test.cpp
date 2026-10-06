// Production post-VS history, animated perspective projection and state
// restoration. No game assets or CPU readback exists in the production path.
#include "../../src/d3d11/weapon_motion.cpp"
#include "../../src/d3d11/flat_animated_identity_ledger.h"
#include "../../src/d3d11/flat_foreground_motion.h"
#include "../../src/d3d11/flat_foreground_phase.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <DirectXPackedVector.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool b,const char* s){++checks;if(!b){std::printf("FAIL: %s\n",s);std::exit(1);}}
void hr(HRESULT h){if(FAILED(h))std::printf("HRESULT %08X\n",unsigned(h));check(SUCCEEDED(h),"D3D operation");}
ComPtr<ID3DBlob> compile(const char* s,const char* profile){ComPtr<ID3DBlob> c,e;auto h=D3DCompile(s,strlen(s),nullptr,nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&c,&e);if(FAILED(h)&&e)std::puts(static_cast<const char*>(e->GetBufferPointer()));hr(h);return c;}
namespace edvr {
ID3D11VertexShader* shaderSwapCreateVs(ID3D11DeviceContext* ctx,const void* bytes,size_t size,const char*,const char*) {
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11VertexShader* shader=nullptr;
    if(FAILED(dev->CreateVertexShader(bytes,size,nullptr,&shader)))return nullptr;return shader;
}
ID3D11PixelShader* shaderSwapCreatePs(ID3D11DeviceContext* ctx,const void* bytes,size_t size,const char*,const char*) {
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11PixelShader* shader=nullptr;
    if(FAILED(dev->CreatePixelShader(bytes,size,nullptr,&shader)))return nullptr;return shader;
}
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx,const void* bytes,size_t size,const char*,const char*) {
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11ComputeShader* shader=nullptr;
    if(FAILED(dev->CreateComputeShader(bytes,size,nullptr,&shader)))return nullptr;return shader;
}

Log& Log::get(){static Log l;return l;}Log::~Log()=default;void Log::note(const char*,...){}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* r,ID3D11DepthStencilView* d){c->OMSetRenderTargets(n,r,d);}
ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext* c,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){auto b=compile(s,"vs_5_0");ComPtr<ID3D11Device> d;c->GetDevice(&d);ID3D11VertexShader* v=nullptr;hr(d->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* c,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){auto b=compile(s,"ps_5_0");ComPtr<ID3D11Device> d;c->GetDevice(&d);ID3D11PixelShader* v=nullptr;hr(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* c,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){auto b=compile(s,"cs_5_0");ComPtr<ID3D11Device> d;c->GetDevice(&d);ID3D11ComputeShader* v=nullptr;hr(d->CreateComputeShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}

}
using namespace edvr;
unsigned issuedDraws=0;
void __stdcall issue(ID3D11DeviceContext* c,unsigned n,unsigned instances,unsigned start,int base,unsigned si){++issuedDraws;c->DrawIndexedInstanced(n,instances,start,base,si);}
struct F4 {double x,y,z,w;};
struct Pose {float mouse=0,projection=1,nearBone=0,farBone=0,clip=.025f,skeleton=92,pad[2]{};};
F4 vertex(int i,Pose p){const double x=i==0||i==3?-.6:.6,y=i<2?-.6:.6,t=i<2?0:1;double z=1+t*.25;return {(x+p.mouse+p.nearBone*(1-t)+p.farBone*t)*p.projection,y,p.clip,z};}
#include "flat_gpu_identity_tests.h"
#include "flat_identity_receipt_tests.h"
#include "flat_bench_history_pressure_tests.h"
int main(int argc,char** argv){
 check(flatIdentityReceiptTests()==0,"identity receipt regressions pass");
 const UINT W=128,H=96;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL fl;
 const auto driver=argc>1&&!strcmp(argv[1],"--hardware")?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP;
 auto made=D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
 if(made==DXGI_ERROR_SDK_COMPONENT_MISSING)made=D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);hr(made);
 ComPtr<ID3D11InfoQueue> queue;dev.As(&queue);
 auto buffer=[&](const void* data,UINT bytes,UINT bind,UINT stride=0){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=bind;d.StructureByteStride=stride;d.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;D3D11_SUBRESOURCE_DATA sd{};sd.pSysMem=data;ComPtr<ID3D11Buffer>b;hr(dev->CreateBuffer(&d,data?&sd:nullptr,&b));return b;};
 // Validate the phase witness against actual bound GPU publication bytes.
 // This readback is an offline oracle; production reads its complete CPU witness.
 {
  float constants[276][4]{};constants[270][0]=1.7f;constants[271][1]=2.1f;constants[272][3]=1;constants[273][2]=.025f;
  auto phaseBuffer=buffer(constants,sizeof(constants),D3D11_BIND_CONSTANT_BUFFER);
  auto boundRows=[&](){
   ComPtr<ID3D11Buffer> actual;ctx->VSGetConstantBuffers(1,1,&actual);check(actual==phaseBuffer,"phase witness reads currently bound b1");
   D3D11_BUFFER_DESC d{};actual->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Buffer> staging;hr(dev->CreateBuffer(&d,nullptr,&staging));ctx->CopyResource(staging.Get(),actual.Get());
   D3D11_MAPPED_SUBRESOURCE mapped{};hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
   std::array<std::array<float,4>,6> rows{};std::memcpy(rows.data(),static_cast<const unsigned char*>(mapped.pData)+270*16,64);ctx->Unmap(staging.Get(),0);return rows;
  };
  auto accepts=[&](float x,float y,float nearPlane){auto rows=boundRows();float actual[6][4]{};std::memcpy(actual,rows.data(),sizeof(actual));return flatForegroundRowsCarryPhase(actual,x,y,W,H,nearPlane);};
  ctx->VSSetConstantBuffers(1,1,phaseBuffer.GetAddressOf());
  check(accepts(0,0,.025f),"actual zero-phase alternate-near bound rows accepted");
  check(!accepts(.375f,-.25f,.025f),"global nonzero phase cannot certify unphased foreign bound rows");
  FlatProjectionJitter jitter{};check(flatProjectionJitter(.375f,-.25f,W,H,jitter),"common phase recipe valid");
  constants[272][0]=jitter.ndcX;constants[272][1]=jitter.ndcY;ctx->UpdateSubresource(phaseBuffer.Get(),0,nullptr,constants,0,0);
  check(accepts(.375f,-.25f,.025f),"actual nonzero common phase alternate-near bound rows accepted");
  check(!accepts(0,0,.025f),"zero phase claim rejects phased actual bound rows");
  check(!accepts(.375f,-.25f,.0675f),"phase witness pins actual bound near convention");
  constants[272][0]+=.02f;ctx->UpdateSubresource(phaseBuffer.Get(),0,nullptr,constants,0,0);
  check(!accepts(.375f,-.25f,.025f),"unknown off-center projection cannot certify common phase");
  // The recorded CFCA8 shader projects through four DP4 rows at VS b0[4].
  // Its near and phase must be read there, independently of the scene's b1.
  float dp4Constants[8][4]{};
  constants[272][0]=jitter.ndcX;
  for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)dp4Constants[4+r][c]=constants[270+c][r];
  auto dp4Buffer=buffer(dp4Constants,sizeof(dp4Constants),D3D11_BIND_CONSTANT_BUFFER);
  ctx->VSSetConstantBuffers(0,1,dp4Buffer.GetAddressOf());
  auto dp4Accepts=[&](float x,float y,float nearPlane){
   ComPtr<ID3D11Buffer> actual;ctx->VSGetConstantBuffers(0,1,&actual);check(actual==dp4Buffer,"DP4 witness reads actual bound b0");
   D3D11_BUFFER_DESC d{};actual->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Buffer> staging;hr(dev->CreateBuffer(&d,nullptr,&staging));ctx->CopyResource(staging.Get(),actual.Get());
   D3D11_MAPPED_SUBRESOURCE mapped{};hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
   float rows[4][4]{};std::memcpy(rows,static_cast<const unsigned char*>(mapped.pData)+4*16,sizeof(rows));ctx->Unmap(staging.Get(),0);
   return flatForegroundProjectionRowsCarryPhase(rows,FlatProjectionPatchLayout::ForwardDp4,x,y,W,H,nearPlane);
  };
  check(dp4Accepts(.375f,-.25f,.025f),"actual nonzero DP4 b0 phase and clip near accepted");
  check(!dp4Accepts(0,0,.025f),"DP4 actual nonzero phase rejects zero claim");
  check(!dp4Accepts(.375f,-.25f,.0675f),"DP4 actual near rejects scene near stand-in");
  dp4Constants[6][0]=.25f;ctx->UpdateSubresource(dp4Buffer.Get(),0,nullptr,dp4Constants,0,0);
  check(!dp4Accepts(.375f,-.25f,.025f),"DP4 world transform changing clip Z refuses constant near");
  dp4Constants[6][0]=0;dp4Constants[4][2]+=.02f;ctx->UpdateSubresource(dp4Buffer.Get(),0,nullptr,dp4Constants,0,0);
  check(!dp4Accepts(.375f,-.25f,.025f),"DP4 native off-center matrix refuses common phase");
  dp4Constants[4][2]=0;dp4Constants[5][2]=0;ctx->UpdateSubresource(dp4Buffer.Get(),0,nullptr,dp4Constants,0,0);
  check(dp4Accepts(0,0,.025f),"actual zero phase DP4 rows accepted");
  ID3D11Buffer* none=nullptr;ctx->VSSetConstantBuffers(1,1,&none);
  ctx->VSSetConstantBuffers(0,1,&none);
 }
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
 auto bind=[&](Pose p,bool advance=true){if(advance)weaponMotionFrameBoundary(ctx.Get());weaponMotionSource(depth.Get());ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());ctx->OMSetDepthStencilState(state.Get(),21);ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0,4);D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster.Get());ctx->IASetInputLayout(layout.Get());UINT stride=16,off=0;ctx->IASetVertexBuffers(1,1,vb.GetAddressOf(),&stride,&off);UINT instanceStride=8;ctx->IASetVertexBuffers(0,1,instance.GetAddressOf(),&instanceStride,&off);
 unsigned ids[4]={p.skeleton==740?1u:0u,526606,0,0};ctx->UpdateSubresource(instance.Get(),0,nullptr,ids,0,0);ctx->VSSetShaderResources(33,1,poolView.GetAddressOf());ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->VSSetConstantBuffers(1,1,cb.GetAddressOf());ctx->UpdateSubresource(cb.Get(),0,nullptr,&p,0,0);float b[8]={p.nearBone,0,0,0,p.farBone,0,0,0};ctx->UpdateSubresource(bones.Get(),0,nullptr,b,0,0);ctx->VSSetShaderResources(38,1,boneView.GetAddressOf());issue(ctx.Get(),9,1,0,0,0);};
 if(argc>1&&!std::strcmp(argv[1],"--bench-history")) {
  flatBenchHistoryPressureTests(dev.Get(),ctx.Get(),bind,true);
  return 0;
 }
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
 {auto d=weaponMotionGpuDiagnostics();check(d.collecting&&d.sourceFrames>0&&d.calls>=4&&d.selected>0,"weapon GPU diagnostics scope and rotating selection are active");check(d.submitted<=d.selected,"weapon GPU diagnostics distinguish selected and submitted calls");ctx->Flush();for(int i=0;i<8;++i)weaponMotionFrameBoundary(ctx.Get());d=weaponMotionGpuDiagnostics();check(d.identifyReady+d.identifyInvalid<=d.submitted&&d.captureReady+d.captureInvalid<=d.selected&&d.rasterReady+d.rasterInvalid<=d.selected,"weapon GPU diagnostics retire only selected samples");check(d.identifyReady+d.captureReady+d.rasterReady>0,"weapon GPU diagnostics retire WARP timestamp samples without waiting");}
 // World arms and viewmodel share topology, with distinct captured clip Z.
 // Reorder both occurrences, then remove one: match by projection on GPU,
 // never by volatile instance number or the occurrence's CPU slot.
 weaponMotionShutdown();{auto d=weaponMotionGpuDiagnostics();check(!d.collecting&&!d.draining&&!d.sourceFrames&&!d.calls,"weapon GPU diagnostics reset on shutdown");}Pose world{-.1f,1,0,0,.0675f},view{.1f,1,0,0,.025f,740};
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
 weaponMotionFrameBoundary(ctx.Get());weaponMotionFrameBoundary(ctx.Get());weaponMotionFrameBoundary(ctx.Get());check(!weaponMotionView()&&weapon_motion_detail::g.history.recordCount()==0,"missing frames discard stale mesh history");
 bind(old);motion();first=read();for(UINT i=0;i<W*H;++i)check(first[i*4+3]!=1,"returning mesh has no old animation history");
 weaponMotionConfigure(false);check(weaponMotionGpuDiagnostics().draining,"weapon GPU diagnostics close immediately on config change");check(!weaponMotionView()&&!weapon_motion_detail::g.map,"off frees temporal weapon resources");bind(old);motion();check(!weaponMotionView(),"live off stays inactive");
 weaponMotionConfigure(true);bind(old);motion();check(weaponMotionView()!=nullptr,"live on resumes with fresh coverage");
 // Bound GPU resource growth; these states must decline before a draw.
 unsigned allocated=weapon_motion_detail::g.history.bytes();weaponMotionDraw(ctx.Get(),issue,131073,1,0,0,0);weaponMotionDraw(ctx.Get(),issue,9,2,0,0,0);check(weapon_motion_detail::g.history.bytes()==allocated,"oversized and instanced meshes allocate nothing");
 weaponMotionResourceWritten(bones.Get());check(weaponMotionView()!=nullptr,"animation updates preserve vertex correspondence");
 weaponMotionResourceWritten(ib.Get());check(!weaponMotionView(),"rewritten mesh indices invalidate motion");bind(old);motion();first=read();for(UINT i=0;i<W*H;++i)check(first[i*4+3]!=1,"rewritten mesh cannot reuse old correspondence");
 // Deterministic source-boundary, drain-cutoff and same-frame budget checks.
 auto& diagnostics=weapon_motion_detail::gpu;diagnostics.reset(ctx.Get());diagnostics.noteSource(depth.Get(),W,H,weapon_motion_detail::g.frame);const uint64_t diagnosticScope=diagnostics.scope;
 D3D11_TEXTURE2D_DESC replacementDesc{};depth->GetDesc(&replacementDesc);ComPtr<ID3D11Texture2D> replacementDepth;hr(dev->CreateTexture2D(&replacementDesc,nullptr,&replacementDepth));diagnostics.noteSource(replacementDepth.Get(),W,H,weapon_motion_detail::g.frame);
 check(diagnostics.phase==weapon_motion_detail::GpuDiagnostics::Phase::Draining&&diagnostics.scope==diagnosticScope,"weapon GPU diagnostics source change closes the current scope");
 diagnostics.identify.submitted=1;diagnostics.drainFrames=weapon_motion_detail::gpuDrainFrames-1;diagnostics.tick(ctx.Get(),weapon_motion_detail::g.frame);
 check(diagnostics.phase==weapon_motion_detail::GpuDiagnostics::Phase::Idle&&!diagnostics.identify.submitted,"weapon GPU diagnostics abandon unresolved samples at the drain cutoff");
 diagnostics.start(depth.Get(),W,H,weapon_motion_detail::g.frame);bool admitted=false,budgetSkipped=false;
 for(unsigned n=0;n<192&&!budgetSkipped;++n){if(diagnostics.choose(weapon_motion_detail::g.frame))admitted=true;budgetSkipped=diagnostics.budgetSkipped!=0;}
 check(admitted&&budgetSkipped&&diagnostics.selected==2,"weapon GPU diagnostics enforce one draw admission per frame");diagnostics.reset(ctx.Get());
 weaponMotionShutdown();
 // The flat adapter shares original-VS capture before any world-source name,
 // with a material that clears stencil16. Its retained index must survive
 // later captures until the final H raster; position order includes repeats.
 weaponMotionConfigure(false);
 auto bytesOf=[&](ID3D11ShaderResourceView* view){
  ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Buffer> sourceBuffer;hr(resource.As(&sourceBuffer));
  D3D11_BUFFER_DESC d{};sourceBuffer->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;d.StructureByteStride=0;
  ComPtr<ID3D11Buffer> staging;hr(dev->CreateBuffer(&d,nullptr,&staging));ctx->CopyResource(staging.Get(),sourceBuffer.Get());
  D3D11_MAPPED_SUBRESOURCE map{};hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<unsigned char> result(d.ByteWidth);std::memcpy(result.data(),map.pData,result.size());ctx->Unmap(staging.Get(),0);return result;
 };
 auto wordsOf=[&](ID3D11ShaderResourceView* view){const auto bytes=bytesOf(view);std::vector<unsigned> words(bytes.size()/4);std::memcpy(words.data(),bytes.data(),bytes.size());return words;};
 AnimatedVertexHistory shared;AnimatedVertexHistory::Capture firstCapture,secondCapture,nextCapture;
 Pose capturedView{.13f,1.12f,.02f,-.04f,.0675f,740};
 bind(capturedView);ctx->OMSetDepthStencilState(state.Get(),5);
 check(shared.capture(ctx.Get(),issue,9,1,0,0,0,100,firstCapture,true),"shared core captures before world naming without stencil16");
 check(!weaponMotionView() && firstCapture.candidateCount==0,"shared capture does not create a VR map or invent prior history");
 check(firstCapture.retainedIndexBytes==4 && wordsOf(firstCapture.instanceIndex.Get())==std::vector<unsigned>{1},"deferred capture owns the exact scalar pool index");
 check(wordsOf(firstCapture.currentIdentity.Get())==std::vector<unsigned>({740,1631,1,0}),"shared GPU identity preserves original skeleton allocation and VR padding");
 const auto capturedPositions=bytesOf(firstCapture.currentPositions.Get());
 check(capturedPositions.size()==9*16,"shared position buffer has one float4 per original index invocation");
 for(unsigned i=0;i<9;++i){float got[4]{};std::memcpy(got,capturedPositions.data()+i*16,16);const auto expected=vertex(indices[i],capturedView);
  check(std::fabs(got[0]-expected.x)<.000001 && std::fabs(got[1]-expected.y)<.000001 && std::fabs(got[2]-expected.z)<.000001 && std::fabs(got[3]-expected.w)<.000001,"shared original animated positions match independent indexed VS math");}
 bind(world,false);ctx->OMSetDepthStencilState(state.Get(),5);
 check(shared.capture(ctx.Get(),issue,9,1,0,0,0,100,secondCapture),"shared core retains separate same-frame occurrences");
 check(secondCapture.retainedIndexBytes==0 && wordsOf(secondCapture.instanceIndex.Get())[0]==0,"default capture uses existing index scratch without another copy");
 check(wordsOf(firstCapture.instanceIndex.Get())==std::vector<unsigned>{1} && bytesOf(firstCapture.currentPositions.Get())==capturedPositions,"earlier deferred inputs survive another draw with the same geometry");
 check(shared.recordCount()==2 && shared.bytes()==9*32*2,"shared geometry history preserves the original position-allocation budget");
 shared.advance(101);bind(capturedView,false);ctx->OMSetDepthStencilState(state.Get(),5);
 check(shared.capture(ctx.Get(),issue,9,1,0,0,0,101,nextCapture,true) && nextCapture.candidateCount==2,"shared next frame offers both previous occurrences for exact GPU matching");
 check(wordsOf(nextCapture.previousIdentity[0].Get())==std::vector<unsigned>({740,1631,1,0}) && wordsOf(nextCapture.previousIdentity[1].Get())==std::vector<unsigned>({92,1631,1,0}),"shared prior candidates retain skeleton identities independently of instance ordering");
 check(shared.resourceWritten(bones.Get())==0 && shared.resourceWritten(ib.Get())==4,"shared history distinguishes animation updates from index-correspondence changes");
 bind(capturedView,false);ctx->OMSetDepthStencilState(state.Get(),5);
 check(shared.capture(ctx.Get(),issue,9,1,0,0,0,102,nextCapture) && nextCapture.candidateCount==0,"shared rewritten indices cannot reuse prior animated positions");
 const unsigned allocatedBytes=shared.bytes();AnimatedVertexHistory::Capture refused;
 check(!shared.capture(ctx.Get(),issue,131073,1,0,0,0,102,refused) && shared.bytes()==allocatedBytes,"shared unsupported draw shape allocates nothing");
 shared.advance(105);check(shared.recordCount()==0 && shared.bytes()==0,"shared history eviction is driven by frame progress rather than successful backend evaluation");
 check(wordsOf(firstCapture.instanceIndex.Get())==std::vector<unsigned>{1},"owned deferred index remains alive after history eviction");
 // CPU upload witnesses must agree with actual GPU identity dispatch, and
 // become unavailable on every incomplete or unobserved mutation.
 FlatAnimatedIdentityLedger ledger;FlatAnimatedIdentityLedger::Identity identity;
 D3D11_BUFFER_DESC dynamicDesc{};dynamicDesc.ByteWidth=16;dynamicDesc.Usage=D3D11_USAGE_DYNAMIC;
 dynamicDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;dynamicDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
 ComPtr<ID3D11Buffer> cpuInstance,cpuPool;hr(dev->CreateBuffer(&dynamicDesc,nullptr,&cpuInstance));
 dynamicDesc.ByteWidth=sizeof(poolData);dynamicDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 dynamicDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;dynamicDesc.StructureByteStride=336;
 hr(dev->CreateBuffer(&dynamicDesc,nullptr,&cpuPool));ComPtr<ID3D11ShaderResourceView> cpuPoolView;
 hr(dev->CreateShaderResourceView(cpuPool.Get(),nullptr,&cpuPoolView));
 check(ledger.demandInstances(cpuInstance.Get(),16) && ledger.demandPool(cpuPool.Get(),sizeof(poolData)),"bounded ledger demands dynamic instance and pool buffers");
 check(!ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"demand alone never claims authoritative CPU bytes");
 unsigned cpuIndices[4]={1,526606,0,0};
 auto uploadWitness=[&](ID3D11Buffer* target,const void* data,unsigned size){D3D11_MAPPED_SUBRESOURCE mapped{};
  hr(ctx->Map(target,0,D3D11_MAP_WRITE_DISCARD,0,&mapped));check(ledger.beginMap(target,D3D11_MAP_WRITE_DISCARD),"witness observes successful write map");
  std::memcpy(mapped.pData,data,size);check(ledger.endMap(target,mapped.pData,size),"complete mapped upload publishes before Unmap");ctx->Unmap(target,0);};
 uploadWitness(cpuInstance.Get(),cpuIndices,sizeof(cpuIndices));uploadWitness(cpuPool.Get(),poolData,sizeof(poolData));
 check(ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity) && identity.slot==1 && identity.skeleton==740 && identity.allocation==1631,"ledger reads actual indexed pool identity rows");
 const auto initialIdentityEpoch=identity.instanceEpoch;const auto initialMutationEpoch=ledger.mutationEpoch(cpuInstance.Get());
 UINT cpuStride=8,cpuOffset=0;ctx->IASetVertexBuffers(0,1,cpuInstance.GetAddressOf(),&cpuStride,&cpuOffset);ctx->VSSetShaderResources(33,1,cpuPoolView.GetAddressOf());
 AnimatedVertexHistory witnessedCore;AnimatedVertexHistory::Capture witnessed;
 check(witnessedCore.capture(ctx.Get(),issue,9,1,0,0,0,500,witnessed,true),"WARP captures the buffers witnessed by the CPU ledger");
 check(wordsOf(witnessed.currentIdentity.Get())==std::vector<unsigned>({identity.skeleton,identity.allocation,1,0}) && wordsOf(witnessed.instanceIndex.Get())[0]==identity.slot,"CPU identity certificate equals actual WARP identity and pool index");
 check(ledger.retainedBytes()==sizeof(cpuIndices)+2*8,"ledger retains only eight identity bytes per pool row");
 D3D11_MAPPED_SUBRESOURCE appended{};hr(ctx->Map(cpuInstance.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&appended));
 check(ledger.beginMap(cpuInstance.Get(),D3D11_MAP_WRITE_NO_OVERWRITE),"actual WARP append begins a CPU publication witness");
 unsigned appendedSlot=0;std::memcpy(appended.pData,&appendedSlot,4);
 check(ledger.endMap(cpuInstance.Get(),appended.pData,sizeof(cpuIndices)),"actual WARP append snapshots mapped current storage");ctx->Unmap(cpuInstance.Get(),0);
 check(ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity) && identity.slot==0 && identity.skeleton==92,"NO_OVERWRITE witness sees appended scalar and retained pool allocation");
 check(witnessedCore.capture(ctx.Get(),issue,9,1,0,0,0,500,witnessed,true) && wordsOf(witnessed.currentIdentity.Get())==std::vector<unsigned>({identity.skeleton,identity.allocation,1,0}),"NO_OVERWRITE CPU identity equals the actual current WARP GPU dispatch");
 check(ledger.demandPoolSlot(cpuPool.Get(),sizeof(poolData),336,1) && ledger.publishWhole(cpuPool.Get(),poolData,sizeof(poolData)),"pool publication samples only the demanded actual allocation slot");
 check(!ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"new actual pool slot cannot inherit another row's publication witness");
 check(ledger.demandPoolSlot(cpuPool.Get(),sizeof(poolData),336,0) && ledger.publishWhole(cpuPool.Get(),poolData,sizeof(poolData)) && ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"new pool slot qualifies after its next complete publication witness");
 check(!ledger.lookup(cpuInstance.Get(),1,cpuPool.Get(),identity) && !ledger.lookup(cpuInstance.Get(),16,cpuPool.Get(),identity),"unaligned and out of range instance offsets refuse");
 ledger.beginMap(cpuInstance.Get(),D3D11_MAP_WRITE_NO_OVERWRITE);
 check(ledger.endMap(cpuInstance.Get(),cpuIndices,sizeof(cpuIndices)) && ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"complete NO_OVERWRITE storage witness observes current index without assuming a full application rewrite");
 check(ledger.publishWhole(cpuInstance.Get(),cpuIndices,sizeof(cpuIndices)) && ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity) && identity.instanceEpoch!=initialIdentityEpoch && ledger.mutationEpoch(cpuInstance.Get())!=initialMutationEpoch,"complete CPU republish advances identity and resource epochs");
 ledger.beginMap(cpuInstance.Get(),D3D11_MAP_WRITE_DISCARD);
 check(!ledger.endMap(cpuInstance.Get(),cpuIndices,8) && !ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"partial mapped byte span remains unknown");
 ledger.publishWhole(cpuInstance.Get(),cpuIndices,sizeof(cpuIndices));ledger.invalidate(cpuPool.Get());
 check(!ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"unobserved GPU or copy mutation invalidates pool identity");
 ledger.publishWhole(cpuPool.Get(),poolData,sizeof(poolData));cpuIndices[0]=0x800001u;ledger.publishWhole(cpuInstance.Get(),cpuIndices,sizeof(cpuIndices));
 check(!ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"authoritative raw pool index is never silently masked");
 cpuIndices[0]=2;ledger.publishWhole(cpuInstance.Get(),cpuIndices,sizeof(cpuIndices));
 check(!ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity),"raw slot observes actual pool bounds");
 const auto boneEpoch=ledger.demandMutation(bones.Get());ledger.noteMutation(bones.Get());
 check(boneEpoch && ledger.mutationEpoch(bones.Get())!=boneEpoch,"non-shadow animated inputs carry mutation certificates");
 ledger.invalidate(nullptr);check(!ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity) && ledger.mutationEpoch(bones.Get())!=boneEpoch,"unknown mutation invalidates all identity witnesses and source epochs");
 ledger.erase(cpuInstance.Get());check(!ledger.lookup(cpuInstance.Get(),0,cpuPool.Get(),identity) && !ledger.mutationEpoch(cpuInstance.Get()),"released resource address has no retained authority");
 std::array<int,FlatAnimatedIdentityLedger::maxMutationResources+8> resourceKeys{};
 ledger.reset();for(unsigned i=0;i<resourceKeys.size();++i)ledger.demandMutation(&resourceKeys[i]);
 check(!ledger.mutationEpoch(&resourceKeys[0]) && ledger.mutationEpoch(&resourceKeys.back()),"bounded mutation ledger refuses evicted source certificates");
 std::vector<unsigned char> largeInstance(FlatAnimatedIdentityLedger::maxInstanceBytes);
 ledger.reset();for(unsigned i=0;i<5;++i){check(ledger.demandInstances(&resourceKeys[i],unsigned(largeInstance.size())),"bounded instance demand accepts exact cap");ledger.publishWhole(&resourceKeys[i],largeInstance.data(),largeInstance.size());}
 check(ledger.retainedBytes()==4*largeInstance.size() && !ledger.demandInstances(&resourceKeys[6],unsigned(largeInstance.size()+1)),"instance shadow storage has a hard four MiB bound");
 check(!ledger.demandPool(&resourceKeys[7],337) && !ledger.demandPool(&resourceKeys[7],336*(FlatAnimatedIdentityLedger::maxPoolRows+1)),"pool stride and row caps are structural refusals");
 // Final owner comes from the original material raster, including its exact
 // depth. The H adapter must retain animated correspondence across refusals
 // and exclude equal-depth world overdraw using that final owner.
 D3D11_TEXTURE2D_DESC ownerDesc{};ownerDesc.Width=W;ownerDesc.Height=H;ownerDesc.MipLevels=ownerDesc.ArraySize=ownerDesc.SampleDesc.Count=1;
 ownerDesc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;ownerDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture2D> ownerTexture;ComPtr<ID3D11RenderTargetView> ownerTarget;ComPtr<ID3D11ShaderResourceView> ownerView,rawDepthView;
 hr(dev->CreateTexture2D(&ownerDesc,nullptr,&ownerTexture));hr(dev->CreateRenderTargetView(ownerTexture.Get(),nullptr,&ownerTarget));hr(dev->CreateShaderResourceView(ownerTexture.Get(),nullptr,&ownerView));
 D3D11_SHADER_RESOURCE_VIEW_DESC depthViewDesc{};depthViewDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;depthViewDesc.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;depthViewDesc.Texture2D.MipLevels=1;
 hr(dev->CreateShaderResourceView(depth.Get(),&depthViewDesc,&rawDepthView));
 auto foreignMarkerCode=compile("float4 main(float4 p:SV_Position,uint primitive:SV_PrimitiveID):SV_Target{return float4(-5,p.z,float(primitive),1);}","ps_5_0");ComPtr<ID3D11PixelShader> foreignMarker;
 hr(dev->CreatePixelShader(foreignMarkerCode->GetBufferPointer(),foreignMarkerCode->GetBufferSize(),nullptr,&foreignMarker));
 auto finalRaster=[&](Pose pose){bind(pose,false);float empty[4]={-1,0,0,0};ctx->ClearRenderTargetView(ownerTarget.Get(),empty);ctx->OMSetRenderTargets(1,ownerTarget.GetAddressOf(),dsv.Get());ctx->PSSetShader(foreignMarker.Get(),nullptr,0);issue(ctx.Get(),9,1,0,0,0);};
 auto readFloatMap=[&](ID3D11ShaderResourceView* view){ComPtr<ID3D11Resource> res;view->GetResource(&res);ComPtr<ID3D11Texture2D> texture;hr(res.As(&texture));D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;hr(dev->CreateTexture2D(&d,nullptr,&staging));ctx->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));std::vector<float> pixels(W*H*4);
  for(unsigned y=0;y<H;++y)std::memcpy(pixels.data()+y*W*4,static_cast<unsigned char*>(m.pData)+y*m.RowPitch,W*16);ctx->Unmap(staging.Get(),0);return pixels;};
 FlatForegroundMotion flat;FlatForegroundMotion::Inputs flatInputs;FlatForegroundMotion::Output flatOutput;
 flatInputs.camera[3][2]=.025f;flatInputs.identity={1,740,1631,1,1,nullptr};flatInputs.writerToken=1;
 float worldCamera[6][4]{};worldCamera[3][2]=.0675f;
 {
  FlatForegroundMotion worldOnly;FlatForegroundMotion::Output output;
  float owner[4]={3,.5f,0,0};ctx->ClearRenderTargetView(ownerTarget.Get(),owner);
  ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.5f,0);
  worldOnly.beginFrame(590);
  check(worldOnly.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,590,W,H,output) &&
        output.qualified && output.motion && output.depthNear==worldCamera[3][2] && !output.resetRequired,
        "fresh world-only H qualifies without any SO captures");
  auto pixels=readFloatMap(output.motion.Get());
  check(std::all_of(pixels.begin(),pixels.end(),[](float value){return value==0;}),
        "world-only H emits a native empty foreground map");
  worldOnly.beginFrame(591);
  check(worldOnly.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,591,W,H,output) &&
        !output.resetRequired,"steady world-only H does not reset every frame");
  float nearerCamera[6][4]{};nearerCamera[3][2]=.025f;worldOnly.beginFrame(592);
  check(worldOnly.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),nearerCamera,592,W,H,output) &&
        output.resetRequired,"world-only common near transition requests one reset");
  worldOnly.beginFrame(593);
  check(worldOnly.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),nearerCamera,593,W,H,output) &&
        !output.resetRequired,"world-only near transition settles without repeated resets");
  worldOnly.fail("unknown-world-writer");
  check(!worldOnly.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),nearerCamera,593,W,H,output) &&
        !output.qualified && !output.motion,"world-only H still refuses an uncertified writer");
 }
 {
  // The largest render size H may qualify at (design doc section 104). The first-person map is RGBA32F at the render size, so 64M pixels
  // (8192x8192) is 1 GB; the cap covers a 4K screen supersampled 2.0. The bound it replaced, 16M pixels, refused every on-foot frame of a
  // 4K screen above SS 1.42: 5760x3240 (SS 1.5) and 7680x4320 (SS 2.0) fell back to the spatial recovery with no AA.
  struct Extent{unsigned width,height;bool allowed;const char* what;};
  const Extent extents[]={
   {3840,2160,true,"3840x2160 (4K at SS 1.0)"},{5760,3240,true,"5760x3240 (4K at SS 1.5, 18.7M pixels)"},
   {7680,4320,true,"7680x4320 (4K at SS 2.0, 33.2M pixels)"},{8192,8192,true,"8192x8192 (exactly 64M pixels)"},
   {8192,8193,false,"8192x8193 (one row past 64M pixels)"},{0,2160,false,"0x2160 (no width)"},{3840,0,false,"3840x0 (no height)"}};
  for(const auto& e:extents)
   check(flatForegroundExtentAllowed(e.width,e.height)==e.allowed,(std::string("foreground extent cap: ")+e.what+(e.allowed?" is allowed":" is refused")).c_str());
  check(kFlatForegroundMaxPixels==64ull*1024*1024,"the foreground extent cap is 64M pixels (8192x8192)");
  // The documented regression: the old 16M bound refuses 5760x3240 (and 7680x4320), the cap admits them.
  constexpr uint64_t oldBound=16ull*1024*1024;
  check(uint64_t(5760)*3240>oldBound && uint64_t(7680)*4320>oldBound && uint64_t(3840)*2160<=oldBound &&
        flatForegroundExtentAllowed(5760,3240) && flatForegroundExtentAllowed(7680,4320),
        "regression: the old 16M-pixel bound refused 4K at SS 1.5 (5760x3240) and SS 2.0 (7680x4320); the cap admits both");
  // prepareH takes its bound from the same function: an extent the cap refuses is "foreground-H-resources"; one it admits goes on to the
  // shape check, which these 128x96 textures fail ("foreground-H-resource-shape"), so the cap is not what refused it.
  FlatForegroundMotion capped;FlatForegroundMotion::Output sized;float cappedCamera[6][4]{};cappedCamera[3][2]=.025f;
  auto cappedRefusal=[&](unsigned frame,unsigned width,unsigned height)->const char* {
   capped.beginFrame(frame);
   return capped.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),cappedCamera,frame,width,height,sized)?"qualified":(sized.refusal?sized.refusal:"none");};
  check(!std::strcmp(cappedRefusal(5900,8192,8193),"foreground-H-resources"),"H refuses 8192x8193 as foreground-H-resources");
  check(!std::strcmp(cappedRefusal(5901,0,2160),"foreground-H-resources") && !std::strcmp(cappedRefusal(5902,3840,0),"foreground-H-resources"),
        "H refuses a zero extent as foreground-H-resources");
  check(!std::strcmp(cappedRefusal(5903,5760,3240),"foreground-H-resource-shape"),
        "H admits 5760x3240 past the extent cap (the 128x96 test textures fail the shape check instead): the 16M bound refused it as foreground-H-resources");
  check(!std::strcmp(cappedRefusal(5904,8192,8192),"foreground-H-resource-shape"),"H admits 8192x8192 past the extent cap");
 }
 Pose foreignOld{.1f,1,0,0,.025f,740},foreignNow{.2f,1,0,0,.025f,740};
 {
  FlatForegroundMotion rejected;auto badInputs=flatInputs;
  badInputs.identity.refusal="identity-pool-slot-unobserved";
  finalRaster(foreignOld);const unsigned before=issuedDraws;
  check(!rejected.capture(ctx.Get(),issue,9,1,0,0,0,594,badInputs),"unknown identity refuses capture");
  check(issuedDraws==before,"unknown identity submits no GPU draw and cannot warm usable next-frame identity");
  check(rejected.stats().attempts==1 && rejected.stats().preflightRefused==1 && !rejected.stats().gpuAttempts && !rejected.stats().submitted,
        "preflight receipt separates rejected attempts from GPU submissions");
  rejected.beginFrame(595);badInputs=flatInputs;badInputs.camera[3][2]=0;
  check(!rejected.capture(ctx.Get(),issue,9,1,0,0,0,595,badInputs) && issuedDraws==before,
        "missing camera submits no GPU draw");
 }
 {
  FlatForegroundMotion warming;warming.beginFrame(596);warming.fail("uncertified-other-writer");
  finalRaster(foreignOld);
  check(!warming.capture(ctx.Get(),issue,9,1,0,0,0,596,flatInputs),"sticky refusal retains its qualification refusal");
  check(warming.stats().gpuAttempts==1 && warming.stats().submitted==1 && warming.stats().warmedAfterRefusal==1,
        "successful GPU warming remains counted despite refused frame");
  finalRaster(foreignNow);
  check(warming.capture(ctx.Get(),issue,9,1,0,0,0,597,flatInputs) &&
        warming.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,597,W,H,flatOutput) && !flatOutput.resetRequired,
        "valid refused-frame capture warms exact identity motion for the next frame");
  const auto pixels=readFloatMap(flatOutput.motion.Get());unsigned matched=0;
  for(unsigned i=0;i<W*H;++i)if(pixels[4*i+3]==1)++matched;
  check(matched>2000,"refused-frame warming supplies real matched GPU history");
 }
 {
  ComPtr<ID3D11PixelShader> gpuMarkers[3][2];
  auto rangedCb=buffer(nullptr,1024,D3D11_BIND_CONSTANT_BUFFER);
  ComPtr<ID3D11DeviceContext1> rangedContext;ctx.As(&rangedContext);
  for(unsigned slot=0;slot<3;++slot)for(unsigned writer=1;writer<=2;++writer) {
   const std::string source="float4 main(float4 p:SV_Position,uint primitive:SV_PrimitiveID):SV_Target{return float4(-"+
       std::to_string(slot*2+3)+",p.z,float(primitive),"+std::to_string(writer)+");}";
   auto code=compile(source.c_str(),"ps_5_0");hr(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&gpuMarkers[slot][writer-1]));
  }
  auto gpuRaster=[&](Pose pose,unsigned slot,unsigned writer,unsigned flags) {
   bind(pose,false);unsigned ids[4]={slot|flags,526606,0,0};ctx->UpdateSubresource(instance.Get(),0,nullptr,ids,0,0);
   ID3D11ShaderResourceView* untouched[15];for(auto& view:untouched)view=poolView.Get();ctx->VSSetShaderResources(0,15,untouched);
   if(rangedContext){UINT first=16,count=16;rangedContext->VSSetConstantBuffers1(0,1,rangedCb.GetAddressOf(),&first,&count);
       first=32;rangedContext->PSSetConstantBuffers1(0,1,rangedCb.GetAddressOf(),&first,&count);}
   float empty[4]={-1,0,0,0};ctx->ClearRenderTargetView(ownerTarget.Get(),empty);
   ctx->OMSetRenderTargets(1,ownerTarget.GetAddressOf(),dsv.Get());ctx->PSSetShader(gpuMarkers[slot][writer-1].Get(),nullptr,0);
   issue(ctx.Get(),9,1,0,0,0);
  };
  flatGpuIdentityTests(ctx.Get(),ownerView.Get(),rawDepthView.Get(),W,H,flatInputs,worldCamera,
                       foreignOld,foreignNow,gpuRaster,readFloatMap,pool.Get(),poolData);
 }
 finalRaster(foreignOld);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,600,flatInputs),"flat captures a new foreign identity before H");
 check(flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,600,W,H,flatOutput) && flatOutput.resetRequired && flatOutput.depthNear==.025f,"new geometry requests one reset and common near spans foreign/world");
 auto flatPixels=readFloatMap(flatOutput.motion.Get());unsigned flatCovered=0;
 for(unsigned i=0;i<W*H;++i)if(flatPixels[i*4+3]==2){++flatCovered;check(flatPixels[i*4+2]>0 && flatPixels[i*4+2]<=1,"new geometry carries actual canonical depth");}
 check(flatCovered>2000,"first H map has final owned foreign samples");
 flatInputs.phaseX=.25f;flatInputs.phaseY=-.375f;finalRaster(foreignNow);
 check(flat.capture(ctx.Get(),issue,9,1,0,0,0,601,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,601,W,H,flatOutput) && !flatOutput.resetRequired,"steady foreign identity uses actual previous history without another reset");
 flatPixels=readFloatMap(flatOutput.motion.Get());flatCovered=0;
 for(unsigned i=0;i<W*H;++i)if(flatPixels[i*4+3]==1){++flatCovered;float expected=(foreignOld.mouse-foreignNow.mouse)*W*.5f*flatPixels[i*4+2]/foreignNow.clip+.25f;
  // Rasterized triangle coordinates use the D3D 8-bit subpixel grid, whereas
  // this oracle projects the original float vertices analytically.
  check(std::fabs(flatPixels[i*4]-expected)<1.f/256 && std::fabs(flatPixels[i*4+1]+.375f)<1.f/256,"H motion follows real previous animated coordinates with both phases removed");}
 check(flatCovered>2000,"steady H map contains matched motion");
 float worldOwner[4]={3,0,0,0};ctx->ClearRenderTargetView(ownerTarget.Get(),worldOwner);
 check(flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,601,W,H,flatOutput),"world overdraw still permits a qualified empty map");
 flatPixels=readFloatMap(flatOutput.motion.Get());check(std::all_of(flatPixels.begin(),flatPixels.end(),[](float f){return f==0;}),"equal-depth world ownership prevents stale foreign motion");
 flat.fail("unknown-final-writer");check(!flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,601,W,H,flatOutput),"unknown final writer refuses SDK foreground qualification");
 finalRaster(foreignOld);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,602,flatInputs),"capture resumes after refused backend frame");
 check(flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,602,W,H,flatOutput) && !flatOutput.resetRequired,"refused backend did not erase previous original-VS history");
 {
  // WHERE H IS ASKED (design section 104). The copy route asks the foreground contract at the game's final copy, after the post chain, not at
  // H's consumer. prepareH reads the depth, the owner plane and the captured geometry as they are AT ITS CALL, so what ran in between matters
  // only if it wrote the depth or the owner plane. Three instances are fed the same two captured frames and ask three ways: at once; after
  // passes that sample the depth through a shader view, write a scratch colour and leave their own state bound (a post chain); and after a
  // draw that wrote the depth over the weapon. The first two maps are equal bit for bit (the late call is as good as the immediate one); the
  // third has lost the ownership of every pixel the later draw covers (an empty map, still qualified), so a late map is a map of the picture.
  auto fullVsCode=compile("float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*2-1,.5,1);}","vs_5_0");
  ComPtr<ID3D11VertexShader> fullVs;hr(dev->CreateVertexShader(fullVsCode->GetBufferPointer(),fullVsCode->GetBufferSize(),nullptr,&fullVs));
  auto readDepthCode=compile("Texture2D<float> Depth:register(t0);float4 main(float4 p:SV_Position):SV_Target{return float4(Depth.Load(int3(p.xy,0)),0,0,1);}","ps_5_0");
  ComPtr<ID3D11PixelShader> readDepth;hr(dev->CreatePixelShader(readDepthCode->GetBufferPointer(),readDepthCode->GetBufferSize(),nullptr,&readDepth));
  D3D11_DEPTH_STENCIL_DESC writeAll{};writeAll.DepthEnable=TRUE;writeAll.DepthFunc=D3D11_COMPARISON_ALWAYS;writeAll.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
  D3D11_DEPTH_STENCIL_DESC noDepth{};noDepth.DepthEnable=FALSE;
  ComPtr<ID3D11DepthStencilState> writeAllState,noDepthState;hr(dev->CreateDepthStencilState(&writeAll,&writeAllState));hr(dev->CreateDepthStencilState(&noDepth,&noDepthState));
  auto fullScreen=[&](ID3D11PixelShader* pixel,ID3D11DepthStencilState* depthState){
   ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(fullVs.Get(),nullptr,0);
   ctx->PSSetShader(pixel,nullptr,0);ctx->OMSetDepthStencilState(depthState,0);ctx->Draw(3,0);};
  enum class Late{None,PostChain,DepthWrite};
  auto lateAsk=[&](unsigned frame,Late late,FlatForegroundMotion::Output& out){
   FlatForegroundMotion fresh;auto inputs=flatInputs;
   finalRaster(foreignOld);check(fresh.capture(ctx.Get(),issue,9,1,0,0,0,frame,inputs),"late ask: the first frame captures its prior pose");
   finalRaster(foreignNow);check(fresh.capture(ctx.Get(),issue,9,1,0,0,0,frame+1,inputs),"late ask: the second frame captures the pose the map is built from");
   if(late==Late::PostChain) {
    ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);ctx->PSSetShaderResources(0,1,rawDepthView.GetAddressOf());
    fullScreen(readDepth.Get(),noDepthState.Get());fullScreen(readDepth.Get(),noDepthState.Get());
    ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(0,1,&none);
   }
   if(late==Late::DepthWrite) {
    ctx->OMSetRenderTargets(0,nullptr,dsv.Get());fullScreen(nullptr,writeAllState.Get());ctx->OMSetRenderTargets(0,nullptr,nullptr);
   }
   const bool asked=fresh.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,frame+1,W,H,out);
   check(asked && out.qualified && out.motion,"late ask: the map qualifies");
   return readFloatMap(out.motion.Get());
  };
  FlatForegroundMotion::Output immediateOut,chainOut,writtenOut;
  const auto immediate=lateAsk(1100,Late::None,immediateOut);
  unsigned owned=0;for(unsigned i=0;i<W*H;++i)if(immediate[4*i+3]==1)++owned;
  check(owned>2000 && !immediateOut.resetRequired,"late ask: the immediate map names the weapon's pixels with real matched history (the baseline is not empty)");
  const auto afterChain=lateAsk(1200,Late::PostChain,chainOut);
  check(afterChain==immediate && chainOut.resetRequired==immediateOut.resetRequired && chainOut.depthNear==immediateOut.depthNear,
        "late ask: after passes that sample the depth and change state the map equals the immediate one, bit for bit");
  const auto afterWrite=lateAsk(1300,Late::DepthWrite,writtenOut);
  check(std::all_of(afterWrite.begin(),afterWrite.end(),[](float v){return v==0;}) && afterWrite!=immediate,
        "late ask: a draw that wrote the depth over the weapon takes those pixels' ownership: the late map names none of them");
 }
 // Same actual identity can legitimately appear twice. Only complete equal
 // input certificates make those previous GPU positions interchangeable.
 flat.reset();flatInputs.phaseX=flatInputs.phaseY=0;flatInputs.certificate.complete=true;
 flatInputs.certificate.constants.resize(sizeof(foreignOld));std::memcpy(flatInputs.certificate.constants.data(),&foreignOld,sizeof(foreignOld));
 flatInputs.certificate.resourceCount=1;flatInputs.certificate.resources[0]={bones,17};
 finalRaster(foreignOld);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,700,flatInputs) && flat.capture(ctx.Get(),issue,9,1,0,0,0,700,flatInputs),"equivalent duplicate poses capture independently");
 finalRaster(foreignNow);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,701,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,701,W,H,flatOutput) && !flatOutput.resetRequired,"complete equal prior input certificates admit duplicate histories");
 flat.reset();finalRaster(foreignOld);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,800,flatInputs),"first ambiguous scenario pose captures");
 flatInputs.certificate.resources[0].epoch=18;finalRaster(foreignNow);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,800,flatInputs),"changed bone epoch captures second actual pose");
 finalRaster(foreignNow);check(!flat.capture(ctx.Get(),issue,9,1,0,0,0,801,flatInputs) && !flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,801,W,H,flatOutput),"same identity with changed animated input epoch refuses ambiguous history");
 flat.reset();flatInputs.certificate.complete=false;flatInputs.camera[3][2]=.026f;finalRaster(foreignOld);
 check(flat.capture(ctx.Get(),issue,9,1,0,0,0,900,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,900,W,H,flatOutput),"metadata near mismatch reaches actual captured projection math");
 flatPixels=readFloatMap(flatOutput.motion.Get());unsigned actualDepthSamples=0;
 for(unsigned i=0;i<W*H;++i)if(flatPixels[i*4+3]==2){++actualDepthSamples;float y=float(i/W)+.5f;
  // Original vertices have bottom W=1/top W=1.25; recover perspective W
  // from the projected y coordinate and compare actual common-near depth.
  float ndcY=1-2*y/H;float physicalW=1.125f/(1-ndcY*.125f/.6f);
  check(std::fabs(flatPixels[i*4+2]-.026f/physicalW)<.000002f,"canonical SDK depth divides by actual captured clip-Z rather than mismatched metadata");}
 check(actualDepthSamples>2000,"metadata mismatch does not erase legitimate captured geometry motion/depth");
 flat.reset();flatInputs.camera[3][2]=.025f;Pose offscreenOld=foreignOld;offscreenOld.mouse=2;
 finalRaster(offscreenOld);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,910,flatInputs),"real offscreen previous geometry is captured");
 finalRaster(foreignNow);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,911,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,911,W,H,flatOutput),"real offscreen correspondence remains qualified");
 flatPixels=readFloatMap(flatOutput.motion.Get());unsigned actualOffscreen=0;
 for(unsigned i=0;i<W*H;++i)if(flatPixels[i*4+3]==1 && float(i%W)+.5f+flatPixels[i*4]>=W)++actualOffscreen;
 check(actualOffscreen>1000,"actual out of image previous coordinates retain finite real motion and matched history");
 ComPtr<ID3D11VertexShader> restoredVs;ComPtr<ID3D11PixelShader> restoredPs;ComPtr<ID3D11RenderTargetView> restoredTarget;ComPtr<ID3D11DepthStencilView> restoredDsv;
 ctx->VSGetShader(&restoredVs,nullptr,nullptr);ctx->PSGetShader(&restoredPs,nullptr,nullptr);ctx->OMGetRenderTargets(1,&restoredTarget,&restoredDsv);
 check(restoredVs==vs && restoredPs==foreignMarker && restoredTarget==ownerTarget && restoredDsv==dsv,"H raster restores original shaders and owner/depth target bindings");
 D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> testPredicate;hr(dev->CreatePredicate(&predicateDesc,&testPredicate));ctx->SetPredication(testPredicate.Get(),FALSE);
 check(!flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,911,W,H,flatOutput),"H refuses active predication before touching its map");ctx->SetPredication(nullptr,FALSE);
 flat.resourceWritten(ib.Get());check(!flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,911,W,H,flatOutput),"geometry mutation between capture and H invalidates qualification");
 // A triangle may straddle either camera's eye plane. Its visible current
 // fragments still have actual finite correspondence through old.xy/old.w;
 // an all-three-vertices positive-W test incorrectly discarded all of it.
 const char* straddleSource="cbuffer P:register(b1){float mouse,projection,a,b;float clip;float3 pad;} StructuredBuffer<float4> Bones:register(t38);struct O{uint2 extra:DATA0;float4 normal:DATA1;float4 p:SV_Position;};O main(float4 p:POSITION){O o;o.extra=0;o.normal=1;o.p=float4((p.x+mouse)*projection,p.y,clip,p.z+lerp(Bones[0].z,Bones[1].z,p.w));return o;}";
 auto straddleCode=compile(straddleSource,"vs_5_0");ComPtr<ID3D11VertexShader> straddleVs;hr(dev->CreateVertexShader(straddleCode->GetBufferPointer(),straddleCode->GetBufferSize(),nullptr,&straddleVs));
 AnimatedVertexHistory::rememberShader(straddleVs.Get(),straddleCode->GetBufferPointer(),straddleCode->GetBufferSize());
 auto straddleRaster=[&](float nearOffset,float farOffset=0){bind(foreignOld,false);ctx->VSSetShader(straddleVs.Get(),nullptr,0);float boneDepths[8]={0,0,nearOffset,0,0,0,farOffset,0};ctx->UpdateSubresource(bones.Get(),0,nullptr,boneDepths,0,0);
  float empty[4]={-1,0,0,0};ctx->ClearRenderTargetView(ownerTarget.Get(),empty);ctx->OMSetRenderTargets(1,ownerTarget.GetAddressOf(),dsv.Get());ctx->PSSetShader(foreignMarker.Get(),nullptr,0);issue(ctx.Get(),9,1,0,0,0);};
 flat.reset();straddleRaster(-2);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,920,flatInputs),"prior eye-straddling triangle retains original animated outputs");
 straddleRaster(0);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,921,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,921,W,H,flatOutput),"prior eye straddling does not refuse current visible triangles");
 flatPixels=readFloatMap(flatOutput.motion.Get());unsigned negativePrior=0;
 for(unsigned i=0;i<W*H;++i)if(flatPixels[i*4+3]==1){float currentW=.025f/flatPixels[i*4+2],t=(currentW-1)/.25f,oldW=currentW-2*(1-t);
  if(oldW<-.25f){++negativePrior;float x=float(i%W)+.5f,y=float(i/W)+.5f;
   float previousX=((x/W*2-1)*currentW/oldW*.5f+.5f)*W;
   float previousY=((y/H*2-1)*currentW/oldW*.5f+.5f)*H;
   check(std::fabs(flatPixels[i*4]-(previousX-x))<.025f && std::fabs(flatPixels[i*4+1]-(previousY-y))<.025f,"negative prior W preserves real projected correspondence");}}
 check(negativePrior>1000,"negative prior W pixels have actual motion instead of fake zero");
 flat.reset();straddleRaster(0);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,930,flatInputs),"positive previous pose captures for current eye-straddling geometry");
 straddleRaster(-2);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,931,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,931,W,H,flatOutput),"current eye straddling is clipped by the real raster rather than all-three W rejection");
 flatPixels=readFloatMap(flatOutput.motion.Get());unsigned currentStraddle=0;
 for(unsigned i=0;i<W*H;++i)if(flatPixels[i*4+3]==1){++currentStraddle;float currentW=.025f/flatPixels[i*4+2],t=(currentW+1)/2.25f,oldW=1+.25f*t;
  float x=float(i%W)+.5f,y=float(i/W)+.5f;float previousX=((x/W*2-1)*currentW/oldW*.5f+.5f)*W,previousY=((y/H*2-1)*currentW/oldW*.5f+.5f)*H;
  check(std::fabs(flatPixels[i*4]-(previousX-x))<.025f && std::fabs(flatPixels[i*4+1]-(previousY-y))<.025f,"current clipped fragments use actual positive fragment W and prior projection");}
 check(currentStraddle>1000,"current eye-straddling mesh produces qualified visible motion");
 flat.reset();straddleRaster(-1,-1.25f);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,940,flatInputs),"actual previous eye-plane position captures zero W with nonzero projected XY");
 straddleRaster(0);check(flat.capture(ctx.Get(),issue,9,1,0,0,0,941,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,941,W,H,flatOutput),"real infinite previous projection reaches representable SDK motion");
 flatPixels=readFloatMap(flatOutput.motion.Get());unsigned infiniteProjection=0;
 for(unsigned i=0;i<W*H;++i)if(flatPixels[i*4+3]==1){++infiniteProjection;
  float directionX=(float(i%W)+.5f-W*.5f)>0?1.f:-1.f,directionY=(float(i/W)+.5f-H*.5f)>0?1.f:-1.f;
  check(flatPixels[i*4]==directionX*65504 && flatPixels[i*4+1]==directionY*65504,"actual infinite previous projection saturates its true direction without inventing zero motion");}
 check(infiniteProjection>2000,"true eye-plane previous projection remains actual matched history");
 // Two current triangles coincide exactly, while their captured old bone
 // transforms differ. The original material discards the later primitive;
 // rerasterizing its VS alone must never replace the surviving motion.
 {
  const float coincidentVertices[]={-.6f,-.6f,1,0,.6f,-.6f,1,0,.6f,.6f,1,0,
                                   -.6f,-.6f,1,1,.6f,-.6f,1,1,.6f,.6f,1,1};
  const UINT coincidentIndices[]={0,1,2,3,4,5};
  auto cvb=buffer(coincidentVertices,sizeof(coincidentVertices),D3D11_BIND_VERTEX_BUFFER);
  auto cib=buffer(coincidentIndices,sizeof(coincidentIndices),D3D11_BIND_INDEX_BUFFER);
  auto materialCode=compile("cbuffer Stamp:register(b13){float token,discardPrimitive,discardAll,pad;} float4 main(float4 p:SV_Position,uint primitive:SV_PrimitiveID):SV_Target{if(discardAll!=0 || (discardPrimitive!=0 && primitive==1))discard;return float4(-5,p.z,float(primitive),token);}","ps_5_0");
  ComPtr<ID3D11PixelShader> material;hr(dev->CreatePixelShader(materialCode->GetBufferPointer(),materialCode->GetBufferSize(),nullptr,&material));
  auto stamp=buffer(nullptr,16,D3D11_BIND_CONSTANT_BUFFER);
  auto setup=[&](bool prior){
   bind(foreignOld,false);UINT stride=16,offset=0;ID3D11Buffer* v=cvb.Get();ctx->IASetVertexBuffers(1,1,&v,&stride,&offset);
   ctx->IASetIndexBuffer(cib.Get(),DXGI_FORMAT_R32_UINT,0);
   const float transforms[8]={prior?-.12f:0,0,0,0,prior?.12f:0,0,0,0};ctx->UpdateSubresource(bones.Get(),0,nullptr,transforms,0,0);
   const float empty[4]={-1,0,0,0};ctx->ClearRenderTargetView(ownerTarget.Get(),empty);
   ctx->OMSetRenderTargets(1,ownerTarget.GetAddressOf(),dsv.Get());ctx->PSSetShader(material.Get(),nullptr,0);ctx->PSSetConstantBuffers(13,1,stamp.GetAddressOf());
  };
  auto original=[&](unsigned token,unsigned count,unsigned start,bool discardPrimitive,bool discardAll){
   const float settings[4]={float(token),discardPrimitive?1.f:0.f,discardAll?1.f:0.f,0};ctx->UpdateSubresource(stamp.Get(),0,nullptr,settings,0,0);
   issue(ctx.Get(),count,1,start,0,0);
  };
  auto expectedSurvivor=[&](const char* message){
   auto pixels=readFloatMap(flatOutput.motion.Get());unsigned covered=0;
   for(unsigned i=0;i<W*H;++i)if(pixels[i*4+3]==1){++covered;check(std::fabs(pixels[i*4]+.12f*W*.5f)<1.f/256 && std::fabs(pixels[i*4+1])<1.f/256,message);}
   check(covered>1000,"coincident material retains substantial matched coverage");
  };
  flat.reset();flatInputs.phaseX=flatInputs.phaseY=0;flatInputs.writerToken=1;
  setup(true);original(1,6,0,true,false);check(flat.capture(ctx.Get(),issue,6,1,0,0,0,1000,flatInputs),"capture both actual prior primitive positions");
  setup(false);original(1,6,0,true,false);check(flat.capture(ctx.Get(),issue,6,1,0,0,0,1001,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,1001,W,H,flatOutput),"original discard plus final primitive receipt qualifies H");
  expectedSurvivor("discarded coincident primitive cannot overwrite surviving animated motion");
  // The second original draw starts its own primitive numbering at zero.
  // Only the writer token distinguishes these coincident same-slot draws.
  flat.reset();setup(true);original(1,3,0,false,false);flatInputs.writerToken=1;
  check(flat.capture(ctx.Get(),issue,3,1,0,0,0,1010,flatInputs),"first draw captures its real prior pose");
  original(2,3,3,false,true);flatInputs.writerToken=2;
  check(flat.capture(ctx.Get(),issue,3,1,3,0,0,1010,flatInputs),"discarded second draw retains independent prior geometry");
  setup(false);original(1,3,0,false,false);flatInputs.writerToken=1;
  check(flat.capture(ctx.Get(),issue,3,1,0,0,0,1011,flatInputs),"first current draw publishes surviving writer token");
  original(2,3,3,false,true);flatInputs.writerToken=2;
  check(flat.capture(ctx.Get(),issue,3,1,3,0,0,1011,flatInputs) && flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,1011,W,H,flatOutput),"different writer tokens qualify coincident cross-draw H");
  expectedSurvivor("discarded coincident draw cannot replace another writer's motion");
  D3D11_DEPTH_STENCIL_DESC conditionalDesc=ds;
  conditionalDesc.StencilReadMask=255;
  conditionalDesc.FrontFace.StencilFunc=conditionalDesc.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;
  ComPtr<ID3D11DepthStencilState> conditional;hr(dev->CreateDepthStencilState(&conditionalDesc,&conditional));
  for(unsigned pass=0;pass<2;++pass) {
   flat.reset();setup(true);flatInputs.writerToken=1;original(1,3,0,false,false);
   check(flat.capture(ctx.Get(),issue,3,1,0,0,0,1020+pass*2,flatInputs),"conditional case retains first prior pose");
   flatInputs.writerToken=2;original(2,3,3,false,false);
   check(flat.capture(ctx.Get(),issue,3,1,3,0,0,1020+pass*2,flatInputs),"conditional case retains second prior pose");
   setup(false);ctx->OMSetDepthStencilState(conditional.Get(),21);flatInputs.writerToken=1;original(1,3,0,false,false);
   check(flat.capture(ctx.Get(),issue,3,1,0,0,0,1021+pass*2,flatInputs),"original conditional stencil writes first passing tuple");
   ctx->OMSetDepthStencilState(conditional.Get(),pass?21:22);flatInputs.writerToken=2;original(2,3,3,false,false);
   check(flat.capture(ctx.Get(),issue,3,1,3,0,0,1021+pass*2,flatInputs) &&
         flat.prepareH(ctx.Get(),ownerView.Get(),rawDepthView.Get(),worldCamera,1021+pass*2,W,H,flatOutput),
         "H uses final tuple without rerunning original conditional stencil");
   auto pixels=readFloatMap(flatOutput.motion.Get());unsigned covered=0;
   for(unsigned i=0;i<W*H;++i)if(pixels[i*4+3]==1){++covered;
    check(std::fabs(pixels[i*4]-(pass?.12f:-.12f)*W*.5f)<1.f/256,
          "conditional stencil selects motion from the actual passing original writer");}
   check(covered>1000,"conditional stencil tuple preserves substantial coverage");
   ComPtr<ID3D11DepthStencilState> restored;UINT reference=0;ctx->OMGetDepthStencilState(&restored,&reference);
   check(restored==conditional && reference==(pass?21u:22u),"H restores actual conditional stencil state and reference");
  }
 }
 flatBenchHistoryPressureTests(dev.Get(),ctx.Get(),bind,false);
 ctx->ClearState();
 if(queue)for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;queue->GetMessage(i,nullptr,&n);std::vector<char> bytes(n);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());hr(queue->GetMessage(i,m,&n));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::puts(m->pDescription);check(false,"no D3D warnings/errors");}}
 std::printf("weapon motion: %u checks passed (%s)\n",checks,driver==D3D_DRIVER_TYPE_WARP?"WARP":"hardware");
}
