#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include "../../src/d3d11/ui_holo_remap.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/log.h"
using Microsoft::WRL::ComPtr;
using namespace edvr::dxbc_container;
namespace edvr {Log& Log::get(){static Log log;return log;}Log::~Log(){}void Log::note(const char*,...){}void breadcrumb(const char*){}}
static unsigned checks=0;
void check(bool b,const char* why) { ++checks;if(!b)throw std::runtime_error(why); }
void ck(HRESULT h){if(FAILED(h)){std::printf("HRESULT=%08lx\n",h);throw std::runtime_error("D3D call");}}
std::vector<BYTE> bytes(const char* p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
uint64_t edvrHash(const std::vector<BYTE>& b){uint64_t h=1469598103934665603ull;for(BYTE x:b)h=(h^x)*1099511628211ull;return h;}
// Offline exact-byte identity; no name-based or shader-family admission.
std::vector<BYTE> patch(const std::vector<BYTE>& source,const std::vector<BYTE>& accepted) {
 check(source==accepted,"unknown original rejected");std::vector<BYTE> result;std::string why;
 check(edvr::ui_holo_remap::patch(source.data(),source.size(),edvrHash(source),result,why),why.c_str());return result;
}
ComPtr<ID3D11Buffer> cb(ID3D11Device* d,const std::vector<float>& v){D3D11_BUFFER_DESC b{};b.ByteWidth=(UINT)v.size()*4;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA init{v.data(),0,0};ComPtr<ID3D11Buffer> r;ck(d->CreateBuffer(&b,&init,&r));return r;}
ComPtr<ID3D11ShaderResourceView> texture(ID3D11Device* d,unsigned w,unsigned h,const std::vector<float>& values,bool scalar=false){
 D3D11_TEXTURE2D_DESC t{};t.Width=w;t.Height=h;t.MipLevels=t.ArraySize=1;t.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 std::vector<float> packed;if(scalar){packed.resize(w*h);for(size_t q=0;q<packed.size();++q)packed[q]=values[q*4];t.Format=DXGI_FORMAT_R32_TYPELESS;}D3D11_SUBRESOURCE_DATA i{scalar?packed.data():values.data(),w*(scalar?4:16),0};ComPtr<ID3D11Texture2D> r;ck(d->CreateTexture2D(&t,&i,&r));ComPtr<ID3D11ShaderResourceView> s;D3D11_SHADER_RESOURCE_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R32_FLOAT;vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;vd.Texture2D.MipLevels=1;ck(d->CreateShaderResourceView(r.Get(),scalar?&vd:nullptr,&s));return s;
}
struct Scene {unsigned sw,sh,ow,oh;float ax,ay,bx,by,jx,jy;D3D11_VIEWPORT vp;D3D11_RECT sc;};
std::vector<float> depth(const Scene& s,bool reference,bool uniform=false){
 unsigned w=reference?s.ow:s.sw,h=reference?s.oh:s.sh;std::vector<float> v(w*h*4,0);
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
  int ix=reference?(int)(((float)x+.5f-s.bx)/s.ax+s.jx):(int)x;
  int iy=reference?(int)(((float)y+.5f-s.by)/s.ay+s.jy):(int)y;
  float z=0;if(ix>=0&&iy>=0&&ix<(int)s.sw&&iy<(int)s.sh)z=uniform?3.f:(((ix/7+iy/5)&1)?3.f:1.f);
  for(unsigned k=0;k<4;++k)v[(y*w+x)*4+k]=z;
 }return v;
}
std::vector<float> draw(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11PixelShader* ps,const Scene& s,ID3D11ShaderResourceView* dep,ID3D11Buffer* transform){
 D3D11_TEXTURE2D_DESC t{};t.Width=s.ow;t.Height=s.oh;t.MipLevels=t.ArraySize=1;t.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_RENDER_TARGET;
 ComPtr<ID3D11Texture2D> target;ck(d->CreateTexture2D(&t,nullptr,&target));ComPtr<ID3D11RenderTargetView> rtv;ck(d->CreateRenderTargetView(target.Get(),nullptr,&rtv));auto r=rtv.Get();float clear[4]={-.25f,.125f,.375f,.5f};c->ClearRenderTargetView(r,clear);
 D3D11_TEXTURE2D_DESC dt=t;dt.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dt.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> ds;ck(d->CreateTexture2D(&dt,nullptr,&ds));ComPtr<ID3D11DepthStencilView> dsv;ck(d->CreateDepthStencilView(ds.Get(),nullptr,&dsv));c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
 ComPtr<ID3D11PixelShader> oldPs;ComPtr<ID3D11Buffer> oldCb;c->PSGetShader(&oldPs,nullptr,nullptr);c->PSGetConstantBuffers(13,1,&oldCb);
 c->OMSetRenderTargets(1,&r,dsv.Get());c->RSSetViewports(1,&s.vp);c->RSSetScissorRects(1,&s.sc);c->PSSetShaderResources(1,1,&dep);
 edvr::ui_holo_remap::Binding binding;const edvr::ui_holo_remap::Params p{1/s.ax,1/s.ay,-s.bx/s.ax+s.jx,-s.by/s.ay+s.jy};
 check(binding.begin(c,ps,transform,p),"production binding begins");c->Draw(3,0);check(binding.restore(c),"production binding restores");binding.clear();
 ComPtr<ID3D11Buffer> restored;ComPtr<ID3D11PixelShader> restoredPs;c->PSGetConstantBuffers(13,1,&restored);c->PSGetShader(&restoredPs,nullptr,nullptr);check(restored.Get()==oldCb.Get()&&restoredPs.Get()==oldPs.Get(),"PS and private CB restore");
 c->OMSetRenderTargets(0,nullptr,nullptr);t.BindFlags=0;t.Usage=D3D11_USAGE_STAGING;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> stage;ck(d->CreateTexture2D(&t,nullptr,&stage));c->CopyResource(stage.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};ck(c->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));std::vector<float> result(s.ow*s.oh*4);for(unsigned y=0;y<s.oh;++y)std::memcpy(result.data()+y*s.ow*4,(BYTE*)m.pData+y*m.RowPitch,s.ow*16);c->Unmap(stage.Get(),0);return result;
}
unsigned visible(const std::vector<float>& v){unsigned n=0;for(size_t i=3;i<v.size();i+=4)if(v[i]>.9f)++n;return n;}
bool exact(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(float));}
#include "failure_cases.h"
#include "writeback_cases.h"
#include "frosted_cases.h"
int main(int argc,char** argv){try{
 if(argc==2&&!std::strcmp(argv[1],"--dry-run")){std::puts("ui_holo_test: dry-run (no files)");return 0;}
 const bool hardware=argc==2&&!std::strcmp(argv[1],"--hardware-self-test");
 check(argc==2&&(hardware||!std::strcmp(argv[1],"--self-test")),"self-test requested");ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;ck(edvr::systemD3D11CreateDevice()(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
 std::printf("ui_holo_test: %s adapter\n",hardware?"hardware":"WARP");
 ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterDesc{};
 ck(d.As(&dxgi));ck(dxgi->GetAdapter(&adapter));ck(adapter->GetDesc(&adapterDesc));
 std::printf("adapter: %ls (vendor=%04X device=%04X)\n",adapterDesc.Description,adapterDesc.VendorId,adapterDesc.DeviceId);
 const char* vs=R"(struct O {float3 lighting:__USER_VERTEX_M_LIGHTINGPOSITION;float2 uv:__USER_VERTEX_M_TEXCOORD;float4 p:SV_POSITION;};O main(uint id:SV_VertexID){O o;float2 q=float2((id<<1)&2,id&2);o.p=float4(q*float2(2,-2)+float2(-1,1),0,1);o.lighting=float3(0,0,2);o.uv=q;return o;})";
 ComPtr<ID3DBlob> code,error;ck(D3DCompile(vs,std::strlen(vs),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&code,&error));ComPtr<ID3D11VertexShader> vertex;ck(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex));c->VSSetShader(vertex.Get(),nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=rs.ScissorEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;ck(d->CreateRasterizerState(&rs,&raster));c->RSSetState(raster.Get());
 D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;ds.StencilEnable=TRUE;ds.StencilReadMask=0;ds.StencilWriteMask=4;ds.FrontFace.StencilFunc=ds.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.BackFace.StencilFailOp=ds.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;ds.FrontFace.StencilPassOp=ds.BackFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;ComPtr<ID3D11DepthStencilState> state;ck(d->CreateDepthStencilState(&ds,&state));c->OMSetDepthStencilState(state.Get(),4);
 D3D11_BLEND_DESC bd{};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlend=b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=15;ComPtr<ID3D11BlendState> blend;ck(d->CreateBlendState(&bd,&blend));c->OMSetBlendState(blend.Get(),nullptr,~0u);
 std::vector<float> b1(280*4,0),b2(23*4,0);b1[279*4+2]=1;b1[120*4+2]=1;b2[13*4]=1;b2[13*4+1]=1;b2[14*4]=.2f;b2[14*4+1]=.5f;b2[14*4+2]=.8f;b2[15*4+3]=1;b2[19*4+1]=1;b2[20*4+3]=1;b2[21*4]=1;b2[21*4+1]=1;b2[21*4+2]=1;b2[21*4+3]=1;b2[22*4+2]=1;auto cb1=cb(d.Get(),b1),cb2=cb(d.Get(),b2),sentinel=cb(d.Get(),{7,8,9,10});ID3D11Buffer* buffers[]={cb1.Get(),cb2.Get()};c->PSSetConstantBuffers(1,2,buffers);auto sent=sentinel.Get();c->PSSetConstantBuffers(13,1,&sent);
 auto material=texture(d.Get(),4,4,std::vector<float>(4*4*4,1));ID3D11ShaderResourceView* srvs[7];for(auto& s:srvs)s=material.Get();c->PSSetShaderResources(0,7,srvs);D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;ck(d->CreateSamplerState(&sd,&sampler));ID3D11SamplerState* samplers[]={sampler.Get(),sampler.Get(),sampler.Get()};c->PSSetSamplers(0,3,samplers);
 for(int shader=1;shader<=2;++shader){b2[22*4+2]=1;b2[21*4+2]=1;c->UpdateSubresource(cb2.Get(),0,nullptr,b2.data(),0,0);auto constantView=material.Get();c->PSSetShaderResources(4,1,&constantView);auto stockBytes=bytes(shader==1?"tools/ui_holo_test/fixtures/ps_EA02FAC2BD6C643C.dxbc":"tools/ui_holo_test/fixtures/ps_E95634B0F61D218F.dxbc");check(!stockBytes.empty(),"saved PS readable");auto modified=patch(stockBytes,stockBytes);ComPtr<ID3D11PixelShader> stock,repaired;ck(d->CreatePixelShader(stockBytes.data(),stockBytes.size(),nullptr,&stock));
  edvr::ui_holo_remap::Cache cache;const auto psHash=edvrHash(stockBytes);
  check(!cache.prepare(c.Get(),stock.Get(),psHash),"untagged original refused before admission");
  check(!cache.remember(stock.Get(),psHash,stockBytes.data(),stockBytes.size(),true),"linked original refused");
  check(cache.remember(stock.Get(),psHash,stockBytes.data(),stockBytes.size(),false),"actual bytes captured");
  repaired=cache.prepare(c.Get(),stock.Get(),psHash);check(repaired&&cache.constants(),"production device cache prepares all dependencies");
  check(cache.prepare(c.Get(),stock.Get(),psHash)==repaired.Get(),"prepared shader reused");
  holoFailure::run(c.Get(),stock.Get(),repaired.Get(),cache.constants(),stockBytes);
  auto unknown=stockBytes;unknown.back()^=1;bool rejected=false;try{patch(unknown,stockBytes);}catch(const std::exception&){rejected=true;}check(rejected,"unknown bytes rejected");
  c->PSSetShader(stock.Get(),nullptr,0);Scene old{64,64,160,160,2.5f,2.5f,0,0,0,0,{0,0,160,160,0,1},{0,0,160,160}};auto originalDepth=texture(d.Get(),64,64,depth(old,false,true),true);auto refDepth=texture(d.Get(),160,160,depth(old,true,true),true);auto map=cb(d.Get(),{.4f,.4f,0,0});auto bad=draw(d.Get(),c.Get(),stock.Get(),old,originalDepth.Get(),map.Get());auto fixed=draw(d.Get(),c.Get(),repaired.Get(),old,originalDepth.Get(),map.Get());auto ref=draw(d.Get(),c.Get(),stock.Get(),old,refDepth.Get(),map.Get());check(visible(bad)==4096&&visible(fixed)==25600,"original pixel loss and repair");check(exact(fixed,ref),"constant-depth exact reference");
  std::vector<float> modelData(32*32*4);for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)for(unsigned k=0;k<4;++k)modelData[(y*32+x)*4+k]=1.f+.1f*x+.07f*y;auto model=texture(d.Get(),32,32,modelData);auto modelView=model.Get();c->PSSetShaderResources(4,1,&modelView);b2[22*4+2]=0;b2[21*4+2]=.25f;c->UpdateSubresource(cb2.Get(),0,nullptr,b2.data(),0,0);unsigned cases=0;
  for(float ratio:{.5f,.75f})for(float ui:{1.f,1.25f})for(unsigned eye:{0u,1u})for(unsigned sub:{0u,1u})for(unsigned jit:{0u,1u}){
   Scene s{};s.sw=96;s.sh=72;s.ow=(unsigned)(s.sw/ratio*ui);s.oh=(unsigned)(s.sh/ratio*ui);s.ax=s.ay=ui/ratio;s.bx=sub?3.25f:0;s.by=sub?-2.5f:0;s.jx=jit?(eye?.3125f:-.1875f):0;s.jy=jit?(eye?-.21875f:.15625f):0;
   s.vp={sub?12.f:0,sub?7.f:0,(float)s.ow-(sub?24.f:0),(float)s.oh-(sub?14.f:0),0,1};s.sc={sub?17:0,sub?11:0,(LONG)s.ow-(sub?19:0),(LONG)s.oh-(sub?13:0)};
   auto input=texture(d.Get(),s.sw,s.sh,depth(s,false),true);auto expanded=texture(d.Get(),s.ow,s.oh,depth(s,true),true);auto transform=cb(d.Get(),{1/s.ax,1/s.ay,-s.bx/s.ax+s.jx,-s.by/s.ay+s.jy});
   auto actual=draw(d.Get(),c.Get(),repaired.Get(),s,input.Get(),transform.Get());auto reference=draw(d.Get(),c.Get(),stock.Get(),s,expanded.Get(),transform.Get());check(exact(actual,reference),"nonuniform/subrect/jitter exact RGBA reference");check(std::all_of(actual.begin(),actual.end(),[](float v){return std::isfinite(v);}),"finite RGBA");unsigned changed=0,fractionalAlpha=0;for(size_t q=0;q<actual.size();q+=4){if(actual[q]!=-.25f)++changed;if(actual[q+3]>.5f&&actual[q+3]<1.f)++fractionalAlpha;}check(changed>0&&changed<s.ow*s.oh,"nontrivial occlusion");check(fractionalAlpha>0,"nonopaque alpha preserved");++cases;
  }
  std::printf("%s old=4096/25600 repaired=25600/25600; %u nonuniform exact RGBA cases\n",shader==1?"EA02":"E956",cases);
  writebackCases(d.Get(),c.Get(),stock.Get(),repaired.Get(),cache.constants());
 }
 frostedBase::run(d.Get(),c.Get());
 std::printf("PASS %u checks; production remap/cache/binding, actual game PS retained\n",checks);return 0;
 }catch(const std::exception& e){std::printf("FAIL %s (%u checks)\n",e.what(),checks);return 1;}}
