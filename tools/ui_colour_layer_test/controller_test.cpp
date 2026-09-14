// End-to-end controller state test. Graphics calls use system D3D11; only
// game classification/binding-shadow and coverage discovery are supplied.
#include "../../src/d3d11/ui_separation.cpp"
#include "../../third_party/dxbc_hash/DxilHash.cpp"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
using Microsoft::WRL::ComPtr;
static ID3D11RenderTargetView* gameBinding=nullptr;
static uint32_t generation=0;
static uint64_t vsHash=0,psHash=0;
static int targetEye=-1;
static bool removed=false,coverageReady=true;
static ID3D11Texture2D* maskTexture=nullptr;
static ID3D11ShaderResourceView* coverageView=nullptr;
namespace edvr {
Log& Log::get(){static Log l;return l;}Log::~Log()=default;
void Log::note(const char*,...){}
std::string Config::getString(const char*,const char*)const{return "dlss";}
void* bindingGet(BindSlot){return gameBinding;}
uint32_t bindingGeneration(BindSlot){return generation;}
bool bindingResolve(void* view,ResourceInfo* info){
 if(!view)return false;ID3D11Resource* r=nullptr;static_cast<ID3D11RenderTargetView*>(view)->GetResource(&r);info->resource=r;if(r)r->Release();return r!=nullptr;
}
uint64_t bindingShaderHash(BindSlot slot){return slot==BindSlot::Vs?vsHash:psHash;}
int uiDepthTargetSpriteEye(){return targetEye;}
void uiDepthSetTargetSeparated(bool x){removed=x;}
void uiDepthSeparatedInvalidate(int){}
bool uiDepthSeparatedCoverage(uint32_t,uint32_t,int,ID3D11Texture2D*,ID3D11Texture2D** mask,ID3D11ShaderResourceView** holo,ID3D11ShaderResourceView** edits,ID3D11ShaderResourceView** depth){
 if(!coverageReady)return false;*mask=maskTexture;*holo=*edits=*depth=coverageView;return true;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c,uint32_t n,ID3D11RenderTargetView*const* r,ID3D11DepthStencilView* d){c->OMSetRenderTargets(n,r,d);}
}
#include "gpu_tests.h"
int main(){
 ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;D3D_FEATURE_LEVEL level{};
 hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c));
 auto hdr=surface(d.Get(),DXGI_FORMAT_R11G11B10_FLOAT),ldr=surface(d.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
 maskTexture=hdr.tex.Get();coverageView=hdr.srv.Get();edvr::enabled=true;
 auto vsCode=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,.5,1);}","vs_5_0");
 ComPtr<ID3D11VertexShader>vs;hr(d->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));c->VSSetShader(vs.Get(),nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 auto psCode=compile("float4 main():SV_Target{return float4(.3,.2,.1,.5);}","ps_5_0");ComPtr<ID3D11PixelShader>ps;hr(d->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
 edvr::uiSeparationRemember(ps.Get(),psCode->GetBufferPointer(),psCode->GetBufferSize(),false);c->PSSetShader(ps.Get(),nullptr,0);
 ComPtr<ID3D11PixelShader>workerPs;hr(d->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&workerPs));
 {
  edvr::InternalScope scope;
  std::thread worker([&]{edvr::uiSeparationRemember(workerPs.Get(),psCode->GetBufferPointer(),psCode->GetBufferSize(),false);});worker.join();
 }
 UINT byteCount=0;workerPs->GetPrivateData(edvr::bytesKey,&byteCount,nullptr);
 check(byteCount==psCode->GetBufferSize(),"render-thread internal scope preserves worker shader capture");
 D3D11_VIEWPORT vp{0,0,8,8,0,1};c->RSSetViewports(1,&vp);D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
 ComPtr<ID3D11RasterizerState>rs;hr(d->CreateRasterizerState(&rd,&rs));c->RSSetState(rs.Get());
 D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
 ComPtr<ID3D11DepthStencilState>depth;hr(d->CreateDepthStencilState(&dd,&depth));c->OMSetDepthStencilState(depth.Get(),0);
 auto over=blend(d.Get(),true,D3D11_BLEND_INV_SRC_ALPHA);c->OMSetBlendState(over.Get(),nullptr,~0u);
 auto bind=[&](Surface& s){gameBinding=s.rtv.Get();++generation;c->OMSetRenderTargets(1,&gameBinding,nullptr);};
 const float bg[4]={.25f,.5f,.75f,1};c->ClearRenderTargetView(hdr.rtv.Get(),bg);bind(hdr);targetEye=0;
 check(edvr::uiSeparationBegin(c.Get()),"recognized target starts layer");c->Draw(3,0);edvr::uiSeparationEnd(c.Get());check(removed,"coverage opted into separation");
 check(!edvr::uiSeparationFailed(),"no decline after target");targetEye=-1;
 check(edvr::uiSeparationBegin(c.Get()),"later world draw is fanned out");c->Draw(3,0);edvr::uiSeparationEnd(c.Get());
 auto toneCode=compile("Texture2D<float3> H:register(t1);float4 main(float4 p:SV_Position):SV_Target{return float4(H.Load(int3(p.xy,0)),1);}","ps_5_0");
 ComPtr<ID3D11PixelShader>tone;hr(d->CreatePixelShader(toneCode->GetBufferPointer(),toneCode->GetBufferSize(),nullptr,&tone));c->PSSetShader(tone.Get(),nullptr,0);
 bind(ldr);auto*input=hdr.srv.Get();c->PSSetShaderResources(1,1,&input);c->OMSetBlendState(nullptr,nullptr,~0u);
 vsHash=edvr::EyeTonemapSnapshot::kVs;psHash=edvr::EyeTonemapSnapshot::kPs;c->Draw(3,0);
 const auto full=read(d.Get(),c.Get(),ldr.tex.Get());
 check(edvr::uiSeparationToneBegin(c.Get(),'N',3,1),"measured tone-map state accepted");c->Draw(3,0);edvr::uiSeparationToneEnd(c.Get());
 check(full==read(d.Get(),c.Get(),ldr.tex.Get()),"tone replay preserves original LDR");
 ComPtr<ID3D11ShaderResourceView>restored;c->PSGetShaderResources(1,1,&restored);check(restored.Get()==input,"tone input restored");
 edvr::UiSeparatedInputs result;check(edvr::uiSeparationInputs(ldr.tex.Get(),hdr.tex.Get(),0,8,8,result),"complete clean inputs published");
 check(result.colour&&result.influence&&result.depth&&result.holo&&result.edits,"all colour/metadata outputs present");
 check(read(d.Get(),c.Get(),result.colour)!=full,"clean tone output actually excludes target");
 coverageReady=false;check(!edvr::uiSeparationInputs(ldr.tex.Get(),hdr.tex.Get(),0,8,8,result),"incomplete metadata declines");
 check(edvr::uiSeparationFailed()&&!result.colour,"decline publishes no partial input");check(full==read(d.Get(),c.Get(),ldr.tex.Get()),"fallback retains original LDR");
 edvr::uiSeparationShutdown();edvr::enabled=true;edvr::uiSeparationUnknownWrite();check(!edvr::uiSeparationFailed(),"untracked draw before seed is harmless");
 vsHash=psHash=0;targetEye=0;bind(hdr);c->PSSetShader(ps.Get(),nullptr,0);c->OMSetBlendState(over.Get(),nullptr,~0u);
 check(edvr::uiSeparationBegin(c.Get()),"new session seeds");c->Draw(3,0);edvr::uiSeparationEnd(c.Get());
 edvr::uiSeparationResourceWrite(hdr.tex.Get());check(edvr::uiSeparationFailed(),"non-draw HDR write declines");check(!edvr::uiSeparationBegin(c.Get()),"failure sticky across later draws");
 edvr::uiSeparationShutdown();edvr::enabled=true;
 check(edvr::uiSeparationBegin(c.Get()),"view-write session seeds");c->Draw(3,0);edvr::uiSeparationEnd(c.Get());
 edvr::uiSeparationViewWrite(nullptr);edvr::uiSeparationViewWrite(ldr.rtv.Get());
 check(!edvr::uiSeparationFailed(),"null and unrelated view writes are harmless");
 {edvr::InternalScope scope;edvr::uiSeparationViewWrite(hdr.rtv.Get());}
 check(!edvr::uiSeparationFailed(),"internal view write is guarded");
 edvr::uiSeparationViewWrite(hdr.rtv.Get());check(edvr::uiSeparationFailed(),"tracked view write declines");
 edvr::uiSeparationShutdown();c->ClearState();printf("UI separation controller: %u checks passed.\n",checks);return 0;
}
