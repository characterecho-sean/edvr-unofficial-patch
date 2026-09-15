// Exercises the production controller. Only game classification and shader
// creation/logging are supplied by this standalone D3D11 harness.
#include "../../src/d3d11/ui_deferred.cpp"
#include "../../third_party/dxbc_hash/DxilHash.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <chrono>
#include <fstream>
using Microsoft::WRL::ComPtr;
static unsigned checks=0;
static uint64_t vsHash=0,psHash=0;
static void check(bool x,const char* why){++checks;if(!x){fprintf(stderr,"FAIL: %s\n",why);exit(1);}}
static void hr(HRESULT h){if(FAILED(h))fprintf(stderr,"HRESULT %08x\n",unsigned(h));check(SUCCEEDED(h),"D3D operation");}
static ComPtr<ID3DBlob> compile(const char*s,size_t n,const char*entry,const char*profile){ComPtr<ID3DBlob>b,e;auto h=D3DCompile(s,n,nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&b,&e);if(FAILED(h)&&e)fprintf(stderr,"%s",(char*)e->GetBufferPointer());hr(h);return b;}
namespace edvr {
Log& Log::get(){static Log l;return l;} Log::~Log()=default;
void Log::note(const char*,...){}
std::string Config::getString(const char*,const char*)const{return "dlss";}
float Config::getFloat(const char*,float def)const{return def;}
uint64_t bindingShaderHash(BindSlot s){return s==BindSlot::Vs?vsHash:psHash;}
void vScreenExecuteCommandListRaw(ID3D11DeviceContext*c,ID3D11CommandList*l,int r){c->ExecuteCommandList(l,r);}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext*c,uint32_t n,ID3D11RenderTargetView*const*r,ID3D11DepthStencilView*d){c->OMSetRenderTargets(n,r,d);}
ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext*c,const char*s,size_t n,const char*e,const char*,const SwapMacro*,const char*){auto b=compile(s,n,e,"vs_5_0");ComPtr<ID3D11Device>d;c->GetDevice(&d);ID3D11VertexShader*v=nullptr;hr(d->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext*c,const char*s,size_t n,const char*e,const char*,const SwapMacro*,const char*){auto b=compile(s,n,e,"ps_5_0");ComPtr<ID3D11Device>d;c->GetDevice(&d);ID3D11PixelShader*v=nullptr;hr(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext*c,const char*s,size_t n,const char*e,const char*,const SwapMacro*,const char*){auto b=compile(s,n,e,"cs_5_0");ComPtr<ID3D11Device>d;c->GetDevice(&d);ID3D11ComputeShader*v=nullptr;hr(d->CreateComputeShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&v));return v;}
}
static std::vector<BYTE> read(ID3D11Device*d,ID3D11DeviceContext*c,ID3D11Texture2D*t){D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D>s;hr(d->CreateTexture2D(&td,nullptr,&s));c->CopyResource(s.Get(),t);D3D11_MAPPED_SUBRESOURCE m{};hr(c->Map(s.Get(),0,D3D11_MAP_READ,0,&m));std::vector<BYTE> b(size_t(td.Width)*td.Height*4);for(UINT y=0;y<td.Height;++y)memcpy(b.data()+size_t(y)*td.Width*4,(BYTE*)m.pData+y*m.RowPitch,td.Width*4);c->Unmap(s.Get(),0);return b;}
static edvr::Surface surf(ID3D11Device*d,UINT w,UINT h,DXGI_FORMAT f){edvr::Surface s;check(s.ensure(d,w,h,f),"surface created");return s;}
struct Harness {
 ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;
 ComPtr<ID3D11VertexShader>vs;ComPtr<ID3D11PixelShader>ui,tone;
 ComPtr<ID3D11DepthStencilState>noDepth,uiDepth;
 ComPtr<ID3D11BlendState>over,add,destAlpha,unsupported;
 ComPtr<ID3D11RasterizerState>rs;ComPtr<ID3D11SamplerState>sampler;
 ComPtr<ID3D11Buffer>cb;
 edvr::Surface hdr,ldr,output,expected,expectedHdr;
 ComPtr<ID3D11Texture2D>z;ComPtr<ID3D11DepthStencilView>dsv;
 ComPtr<ID3D11ShaderResourceView>zSrv;
 UINT inW=8,inH=8,outW=8,outH=8;
 float bg[4]={.25,.5,.75,1};
 Harness(bool hardware,UINT input=8,UINT out=8):inW(input),inH(input),outW(out),outH(out){
  D3D_FEATURE_LEVEL level{};hr(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c));
  const char*v="cbuffer C:register(b0){float4 colour;float2 jitter;float2 cut;}struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};O main(uint i:SV_VertexID){O o;float2 p=float2(i==2?3:-1,i==1?3:-1);o.p=float4(p+float2(2*jitter.x,-2*jitter.y),.5,1);o.uv=p*float2(.5,-.5)+.5;return o;}";
  auto b=compile(v,strlen(v),"main","vs_5_0");hr(d->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&vs));edvr::uiDeferredRemember(vs.Get(),b->GetBufferPointer(),b->GetBufferSize(),false);
  const char*p="cbuffer C:register(b0){float4 colour;float2 jitter;float2 cut;}float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{if(uv.x<cut.x || uv.x>cut.y || uv.y<.2 || uv.y>.8)discard;return colour;}";
  b=compile(p,strlen(p),"main","ps_5_0");hr(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&ui));edvr::uiDeferredRemember(ui.Get(),b->GetBufferPointer(),b->GetBufferSize(),false);
  const char*t="Texture2D<float3> H:register(t1);SamplerState S:register(s0);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{float3 h=H.SampleLevel(S,uv,0);return float4(h/(1+h),1);}";
  b=compile(t,strlen(t),"main","ps_5_0");hr(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&tone));edvr::uiDeferredRemember(tone.Get(),b->GetBufferPointer(),b->GetBufferSize(),false);
  D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hr(d->CreateBuffer(&bd,nullptr,&cb));
  D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthFunc=D3D11_COMPARISON_ALWAYS;hr(d->CreateDepthStencilState(&dd,&noDepth));dd.StencilEnable=TRUE;dd.StencilReadMask=1;dd.StencilWriteMask=5;dd.FrontFace=dd.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_EQUAL};hr(d->CreateDepthStencilState(&dd,&uiDepth));
  D3D11_BLEND_DESC blend{};auto&r=blend.RenderTarget[0];r.BlendEnable=TRUE;r.SrcBlend=r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=15;hr(d->CreateBlendState(&blend,&over));r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_ONE;hr(d->CreateBlendState(&blend,&add));r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_DEST_ALPHA;hr(d->CreateBlendState(&blend,&destAlpha));r.SrcBlend=D3D11_BLEND_DEST_COLOR;hr(d->CreateBlendState(&blend,&unsupported));
  D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;hr(d->CreateRasterizerState(&rd,&rs));
  D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;hr(d->CreateSamplerState(&sd,&sampler));
  hdr=surf(d.Get(),input,input,DXGI_FORMAT_R11G11B10_FLOAT);ldr=surf(d.Get(),input,input,DXGI_FORMAT_R8G8B8A8_UNORM);output=surf(d.Get(),out,out,DXGI_FORMAT_R8G8B8A8_UNORM);expected=surf(d.Get(),out,out,DXGI_FORMAT_R8G8B8A8_UNORM);expectedHdr=surf(d.Get(),out,out,DXGI_FORMAT_R11G11B10_FLOAT);
  D3D11_TEXTURE2D_DESC zd{};zd.Width=zd.Height=input;zd.MipLevels=zd.ArraySize=zd.SampleDesc.Count=1;zd.Format=DXGI_FORMAT_R24G8_TYPELESS;zd.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;hr(d->CreateTexture2D(&zd,nullptr,&z));D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;hr(d->CreateDepthStencilView(z.Get(),&dv,&dsv));D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;hr(d->CreateShaderResourceView(z.Get(),&sv,&zSrv));edvr::enabled=true;
 }
 void constants(float jx,float jy,float alpha=.5){float values[8]={.3f,.12f,.03f,alpha,jx/inW,jy/inH,.25f,.75f};edvr::uiDeferredResourceWrite(c.Get(),cb.Get());c->UpdateSubresource(cb.Get(),0,nullptr,values,0,0);}
 void setup(edvr::Surface&s,UINT w,UINT h,ID3D11PixelShader*p,ID3D11BlendState*b,ID3D11DepthStencilView*depth){c->ClearState();c->RSSetState(rs.Get());D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};c->RSSetViewports(1,&vp);c->OMSetRenderTargets(1,s.rtv.GetAddressOf(),depth);c->OMSetBlendState(b,nullptr,~0u);c->OMSetDepthStencilState(depth?uiDepth.Get():noDepth.Get(),15);c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(p,nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetConstantBuffers(0,1,cb.GetAddressOf());c->PSSetConstantBuffers(0,1,cb.GetAddressOf());c->PSSetSamplers(0,1,sampler.GetAddressOf());}
 void start(){edvr::uiDeferredFrameBoundary(c.Get());edvr::enabled=true;edvr::failed=edvr::routeNoted=false;edvr::resetPending[0]=edvr::resetPending[1]=false;vsHash=psHash=0;c->ClearState();c->ClearRenderTargetView(hdr.rtv.Get(),bg);c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.6f,1);}
 void uiDraw(float jx,float jy,ID3D11BlendState*blend,bool defer){constants(jx,jy);setup(hdr,inW,inH,ui.Get(),blend,dsv.Get());edvr::uiDeferredBeforeDraw(c.Get());bool captured=defer&&edvr::uiDeferredBegin(c.Get(),0,'N',3,1,0,0,0);check(!defer||captured,"recognized UI captured");c->DrawInstanced(3,1,0,0);if(captured)edvr::uiDeferredEnd(c.Get());}
 void toneDraw(bool defer){constants(0,0);setup(ldr,inW,inH,tone.Get(),nullptr,nullptr);c->PSSetShaderResources(1,1,hdr.srv.GetAddressOf());vsHash=edvr::EyeTonemapSnapshot::kVs;psHash=edvr::EyeTonemapSnapshot::kPs;edvr::uiDeferredBeforeDraw(c.Get());if(defer)edvr::uiDeferredBeforeTone(c.Get(),'N',3,1,0,0,0);c->DrawInstanced(3,1,0,0);}
 void baselineNative(ID3D11BlendState*b){constants(0,0);c->ClearRenderTargetView(expectedHdr.rtv.Get(),bg);setup(expectedHdr,outW,outH,ui.Get(),b,nullptr);c->DrawInstanced(3,1,0,0);setup(expected,outW,outH,tone.Get(),nullptr,nullptr);c->PSSetShaderResources(1,1,expectedHdr.srv.GetAddressOf());c->DrawInstanced(3,1,0,0);}
 ~Harness(){edvr::uiDeferredShutdown();c->ClearState();}
};
int main(int argc,char**argv){
 if(argc==3 && strcmp(argv[1],"--fanout")==0){
  std::ifstream input(argv[2],std::ios::binary);check(bool(input),"captured shader opened");
  std::vector<BYTE> code((std::istreambuf_iterator<char>(input)),{}),patched;std::string why;
  check(edvr::uiColourFanout(code.data(),code.size(),patched,why),why.c_str());
  ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;D3D_FEATURE_LEVEL level{};hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c));
  ComPtr<ID3D11PixelShader>ps;hr(d->CreatePixelShader(patched.data(),patched.size(),nullptr,&ps));
  printf("Captured shader fanout: %zu -> %zu bytes, %u checks passed.\n",code.size(),patched.size(),checks);return 0;
 }
 if(argc>1 && strcmp(argv[1],"--benchmark")==0){
  Harness h(true,2913,4482);auto*c=h.c.Get();auto*d=h.d.Get();
  ComPtr<IDXGIDevice> gd;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterDesc{};hr(d->QueryInterface(IID_PPV_ARGS(&gd)));hr(gd->GetAdapter(&adapter));hr(adapter->GetDesc(&adapterDesc));wprintf(L"GPU: %s\n",adapterDesc.Description);
  D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT,0};ComPtr<ID3D11Query> freq,a,b;hr(d->CreateQuery(&qd,&freq));qd.Query=D3D11_QUERY_TIMESTAMP;hr(d->CreateQuery(&qd,&a));hr(d->CreateQuery(&qd,&b));double gpu=0,cpu=0;unsigned samples=0;
  for(unsigned frame=0;frame<24;++frame){h.start();h.uiDraw(.25f,-.25f,h.over.Get(),true);h.toneDraw(true);auto begin=std::chrono::steady_clock::now();check(edvr::uiDeferredPrepare(c,h.ldr.tex.Get(),h.zSrv.Get(),h.output.tex.Get(),0,h.inW,h.inH,.25f,-.25f)!=nullptr,"benchmark prepare");auto end=std::chrono::steady_clock::now();c->Begin(freq.Get());c->End(a.Get());edvr::uiDeferredApply(c,0);c->End(b.Get());c->End(freq.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT f{};HRESULT result;do{result=c->GetData(freq.Get(),&f,sizeof(f),0);}while(result==S_FALSE);hr(result);UINT64 ta=0,tb=0;hr(c->GetData(a.Get(),&ta,sizeof(ta),0));hr(c->GetData(b.Get(),&tb,sizeof(tb),0));check(!f.Disjoint,"stable timestamps");if(frame>=4){gpu+=double(tb-ta)*1000/f.Frequency;cpu+=std::chrono::duration<double,std::milli>(end-begin).count();++samples;}}
  printf("Native replay 2913^2 -> 4482^2: GPU %.4f ms/eye, prepare CPU %.4f ms/eye, snapshots %.2f MiB, stencil-in-PS=%d (%u samples). Synthetic one UI draw/simple nonlinear tone; excludes DLSS, pre-tone work and full game materials.\n",gpu/samples,cpu/samples,edvr::snapshots.allocatedBytes()/1048576.,edvr::renderer.depth.usesSpecifiedStencilRef(),samples);return 0;
 }
 bool hardware=argc>1&&strcmp(argv[1],"--hardware")==0;
 for(UINT scale:{1u,2u})for(float j:{0.f,.25f,-.25f}){
  Harness h(hardware,8,8*scale);auto*d=h.d.Get();auto*c=h.c.Get();
  for(auto*b:{h.over.Get(),h.add.Get()}){
   h.start();h.uiDraw(j,-j,b,false);h.toneDraw(false);auto fallback=read(d,c,h.ldr.tex.Get());
   h.start();h.uiDraw(j,-j,b,true);h.toneDraw(true);check(read(d,c,h.ldr.tex.Get())==fallback,"fallback retains original UI and stencil");check(edvr::eyes[0].complete,"tone recognized");
   auto alias=surf(d,8,8,DXGI_FORMAT_R8G8B8A8_UNORM);edvr::uiDeferredResourceWrite(c,alias.tex.Get());edvr::uiDeferredCopy(alias.tex.Get(),h.ldr.tex.Get(),true);c->CopyResource(alias.tex.Get(),h.ldr.tex.Get());
   auto*clean=edvr::uiDeferredPrepare(c,alias.tex.Get(),h.zSrv.Get(),h.output.tex.Get(),0,8,8,j,-j);check(clean!=nullptr,"copied submit route matched");ComPtr<ID3D11Resource> cr;clean->GetResource(&cr);ComPtr<ID3D11Texture2D> ct;hr(cr.As(&ct));auto input=read(d,c,ct.Get());if(input==fallback){fprintf(stderr,"identical input/fallback center %u %u hdr %u\n",input[4*(4*8+4)],fallback[4*(4*8+4)],read(d,c,h.hdr.tex.Get())[4*(4*8+4)]);}check(input!=fallback,"DLSS input contains no UI");
   // A stationary world reconstruction is the exact nonlinear tone of the
   // constant background. The UI itself must rasterize at output resolution.
   float world[4]={h.bg[0]/(1+h.bg[0]),h.bg[1]/(1+h.bg[1]),h.bg[2]/(1+h.bg[2]),1};c->ClearRenderTargetView(h.output.rtv.Get(),world);edvr::uiDeferredApply(c,0);auto actual=read(d,c,h.output.tex.Get());h.baselineNative(b);auto expected=read(d,c,h.expected.tex.Get());
   size_t different=0;for(size_t i=0;i<actual.size();++i)if(abs(int(actual[i])-int(expected[i]))>1)++different;if(different){fprintf(stderr,"scale=%u jitter=%g differences=%zu\n",scale,j,different);for(size_t i=0;i<actual.size();i+=4)if(actual[i]!=expected[i]){auto bt=read(d,c,edvr::renderer.base.tex.Get()),ut=read(d,c,edvr::renderer.ui.tex.Get()),trans=read(d,c,edvr::renderer.transmission.tex.Get());float tv;memcpy(&tv,trans.data()+i,4);fprintf(stderr,"(%zu,%zu) %u vs %u base%u ui%u t%g\n",i/4%(8*scale),i/4/(8*scale),actual[i],expected[i],bt[i],ut[i],tv);}}check(!different,"native UI matches original shader without jitter");
   edvr::uiDeferredResourceWrite(c,alias.tex.Get());check(!edvr::uiDeferredPrepare(c,alias.tex.Get(),h.zSrv.Get(),h.output.tex.Get(),0,8,8,j,-j),"modified submit is rejected");
  }
  h.start();h.uiDraw(0,0,h.over.Get(),true);h.constants(0,0);h.setup(h.hdr,8,8,h.ui.Get(),h.unsupported.Get(),h.dsv.Get());check(!edvr::uiDeferredBegin(c,0,'N',3,1,0,0,0),"unsupported UI flushes sequence");check(edvr::eyes[0].aborted&&edvr::eyes[0].restored,"decline retains prior UI");check(!edvr::enabled&&edvr::failed,"late failure latches off");check(edvr::uiDeferredFallbackReset(0)&&!edvr::uiDeferredFallbackReset(0)&&edvr::uiDeferredFallbackReset(1)&&!edvr::uiDeferredFallbackReset(1),"one reset per eye after fallback");check(!edvr::uiDeferredBegin(c,0,'N',3,1,0,0,0),"disabled controller leaves subsequent UI to existing path");
  h.start();h.uiDraw(0,0,h.over.Get(),true);h.constants(0,0);h.setup(h.hdr,8,8,h.ui.Get(),h.destAlpha.Get(),h.dsv.Get());check(!edvr::uiDeferredBegin(c,0,'N',3,1,0,0,0),"destination-alpha blend remains unsupported for UI");check(edvr::eyes[0].aborted&&edvr::failed,"destination-alpha UI declines the sequence");
  for(auto* blend:{static_cast<ID3D11BlendState*>(nullptr),h.add.Get(),h.over.Get(),h.destAlpha.Get()}){
   auto world=[&](bool mirror){h.constants(0,0,.25);h.setup(h.hdr,8,8,h.ui.Get(),blend,h.dsv.Get());edvr::uiDeferredBeforeDraw(c);if(mirror)check(!edvr::uiDeferredBegin(c,-1,'N',3,1,0,0,0),"world is mirrored without UI classification");c->DrawInstanced(3,1,0,0);edvr::uiDeferredEnd(c);};
   h.start();world(false);h.toneDraw(false);auto cleanExpected=read(d,c,h.ldr.tex.Get());
   h.start();h.uiDraw(0,0,h.over.Get(),false);world(false);h.uiDraw(0,0,h.add.Get(),false);auto originalHdr=read(d,c,h.hdr.tex.Get());h.toneDraw(false);auto original=read(d,c,h.ldr.tex.Get());
   h.start();h.uiDraw(0,0,h.over.Get(),true);world(true);h.uiDraw(0,0,h.add.Get(),true);check(read(d,c,h.hdr.tex.Get())==originalHdr,"interleaved world/UI preserves original HDR bit for bit");h.toneDraw(true);check(read(d,c,h.ldr.tex.Get())==original,"interleaved world/UI preserves original LDR bit for bit");
   auto*clean=edvr::uiDeferredPrepare(c,h.ldr.tex.Get(),h.zSrv.Get(),h.output.tex.Get(),0,8,8,0,0);check(clean,"interleaved world path prepares");ComPtr<ID3D11Resource>r;clean->GetResource(&r);ComPtr<ID3D11Texture2D>t;hr(r.As(&t));check(read(d,c,t.Get())==cleanExpected,"clean image preserves interleaved world and removes both UI groups");
  }
  h.start();h.uiDraw(0,0,h.over.Get(),true);h.toneDraw(true);
  check(edvr::uiDeferredPrepare(c,h.ldr.tex.Get(),h.zSrv.Get(),h.output.tex.Get(),0,8,8,0,0),"prepare before later failure");
  edvr::decline(edvr::eyes[1],"injected later failure");
  float world[4]={.2f,1.f/3.f,3.f/7.f,1};c->ClearRenderTargetView(h.output.rtv.Get(),world);edvr::uiDeferredApply(c,0);h.baselineNative(h.over.Get());
  check(read(d,c,h.output.tex.Get())==read(d,c,h.expected.tex.Get()),"published command still applies after controller is disabled");
  h.start();h.uiDraw(0,0,h.over.Get(),true);
  D3D11_DEPTH_STENCIL_DESC stencil{};h.uiDepth->GetDesc(&stencil);stencil.StencilReadMask=4;ComPtr<ID3D11DepthStencilState> dependent;hr(d->CreateDepthStencilState(&stencil,&dependent));c->OMSetDepthStencilState(dependent.Get(),5);
  check(!edvr::uiDeferredBegin(c,-1,'N',3,1,0,0,0)&&edvr::failed,"world depending on changed UI stencil declines");
  h.start();h.uiDraw(0,0,h.over.Get(),true);auto other=surf(d,8,8,DXGI_FORMAT_R8G8B8A8_UNORM),other2=surf(d,8,8,DXGI_FORMAT_R8G8B8A8_UNORM);ID3D11RenderTargetView* mrt[2]={other.rtv.Get(),other2.rtv.Get()};c->OMSetRenderTargets(2,mrt,nullptr);
  check(!edvr::uiDeferredBegin(c,-1,'N',3,1,0,0,0)&&!edvr::failed,"unrelated MRT draws do not invalidate clean world");
 }
 printf("Deferred UI controller: %u checks passed (%s).\n",checks,hardware?"hardware":"WARP");
 return 0;
}
