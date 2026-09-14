#include "../common/native_fss.h"
#include "fss_heal.h"
#include "../common/config.h"
#include "../common/eye_sync.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned kCapacity=16;
constexpr float kProjectionTolerance=.0025f;

struct Raw {
  ComPtr<ID3D11Texture2D> tex; D3D11_TEXTURE2D_DESC desc{},sourceDesc{}; uint32_t roi[4]{}; bool valid=false;
  void clear(){sourceDesc={};std::memset(roi,0,sizeof(roi));valid=false;}
  void reset(){tex.Reset();desc={};clear();}
};
struct State {
  bool active=false,frameOpen=false,consumed[2]{}; ID3D11Device* device=nullptr;
  ComPtr<ID3D11DeviceContext> ctx; DWORD thread=0; uint64_t generation=0,reference=0,floor=0,sequence=0;
  Raw current[2],previous[2]; EdvrNativeFssFrame frame{}; edvr::EyeSync sync{};
  bool projectionCompatible=false,healGate=false,submitLatched=false,stampsKnown=false; LONG arrival=0,chrome=0;
  uint64_t previousSequence=0;
  uint64_t arrivalChanged=0,chromeChanged=0;
  bool arrivalRecent=false,chromeRecent=false;
  uint64_t snapshots=0,healed=0,gatedOut=0,donorRefusals=0;
  bool healNoted=false,projectionNoted=false;
};
State pool[kCapacity]{}; unsigned used=0; State* live=nullptr; std::mutex mu;
State* find(void* p){for(unsigned i=0;i<used;++i)if(p==&pool[i])return &pool[i];return nullptr;}
bool finite(const float* p,size_t n){if(!p)return false;for(size_t i=0;i<n;++i)if(!std::isfinite(p[i]))return false;return true;}
bool sameDevice(ID3D11Device* expected,ID3D11Texture2D* source){
  if(!expected||!source)return false; ComPtr<ID3D11Device> owner;source->GetDevice(&owner);if(!owner)return false;
  ComPtr<IUnknown>a,b;if(FAILED(expected->QueryInterface(IID_PPV_ARGS(&a)))||FAILED(owner->QueryInterface(IID_PPV_ARGS(&b))))return false;return a.Get()==b.Get();
}
bool validTexture(ID3D11Texture2D* t,D3D11_TEXTURE2D_DESC* d){
  if(!t||!d)return false;t->GetDesc(d);return d->Width&&d->Height&&d->MipLevels==1&&d->ArraySize==1&&d->SampleDesc.Count==1&&d->SampleDesc.Quality==0&&d->Usage==D3D11_USAGE_DEFAULT&&d->CPUAccessFlags==0;
}
bool closeEnough(float a,float b){return std::fabs(a-b)<=kProjectionTolerance*(std::max)(1.f,(std::max)(std::fabs(a),std::fabs(b)));}
bool parallel(const EdvrNativeFssFrame& f){
  for(unsigned e=0;e<2;++e)if(!finite(f.frusta[e],4)||f.frusta[e][0]>=f.frusta[e][1]||f.frusta[e][2]>=f.frusta[e][3])return false;
  if(f.frusta[0][0]>=0||f.frusta[0][1]<=0)return false;
  if(!closeEnough(f.frusta[0][0],-f.frusta[1][1])||!closeEnough(f.frusta[0][1],-f.frusta[1][0])||!closeEnough(f.frusta[0][2],f.frusta[1][2])||!closeEnough(f.frusta[0][3],f.frusta[1][3]))return false;
  if(!finite(f.eyeToHead[0],12)||!finite(f.eyeToHead[1],12))return false;
  bool la=false,ra=false;for(unsigned i=0;i<12;++i){if(f.eyeToHead[0][i]!=0)la=true;if(f.eyeToHead[1][i]!=0)ra=true;}
  // OpenXR always supplies the rigid eye-to-head pair.  An absent pair cannot
  // prove that the runtime is using parallel displays, so fail closed.
  if(!la||!ra)return false;
  for(unsigned e=0;e<2;++e){
    const float* m=f.eyeToHead[e];
    for(unsigned r=0;r<3;++r){float n=0;for(unsigned c=0;c<3;++c)n+=m[r*4+c]*m[r*4+c];if(std::fabs(n-1.f)>.02f)return false;}
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b){float d=0;for(unsigned c=0;c<3;++c)d+=m[a*4+c]*m[b*4+c];if(std::fabs(d)>.02f)return false;}
    const float det=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);if(det<.9f||det>1.1f)return false;
  }
  for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)if(!closeEnough(f.eyeToHead[0][r*4+c],f.eyeToHead[1][r*4+c]))return false;
  return true;
}
bool bounds(const float* b,const D3D11_TEXTURE2D_DESC& d,uint32_t roi[4],bool* flip){
  if(!b||!finite(b,4)||!roi||!flip)return false;for(unsigned i=0;i<4;++i)if(b[i]<0||b[i]>1)return false;if(b[0]==b[2]||b[1]==b[3])return false;*flip=b[0]>b[2]||b[1]>b[3];if(*flip)return true;
  const double x0=std::floor(double(b[0])*d.Width),y0=std::floor(double(b[1])*d.Height),x1=std::ceil(double(b[2])*d.Width),y1=std::ceil(double(b[3])*d.Height);
  if(x1<=x0||y1<=y0||x0<0||y0<0||x1>d.Width||y1>d.Height)return false;roi[0]=(uint32_t)x0;roi[1]=(uint32_t)y0;roi[2]=(uint32_t)x1;roi[3]=(uint32_t)y1;return true;
}
bool compatible(const Raw& r,const D3D11_TEXTURE2D_DESC& d,const uint32_t roi[4]){
  return r.valid&&r.tex&&r.desc.Width==roi[2]-roi[0]&&r.desc.Height==roi[3]-roi[1]&&r.desc.Format==d.Format;
}
bool snapshot(State& s,unsigned e,ID3D11Texture2D* src,const D3D11_TEXTURE2D_DESC& d,const uint32_t roi[4]){
  Raw& r=s.current[e];D3D11_TEXTURE2D_DESC out=d;out.Width=roi[2]-roi[0];out.Height=roi[3]-roi[1];
  out.BindFlags=D3D11_BIND_SHADER_RESOURCE;out.MiscFlags=0;
  if(!r.tex||r.desc.Width!=out.Width||r.desc.Height!=out.Height||r.desc.Format!=out.Format||r.desc.SampleDesc.Count!=out.SampleDesc.Count||r.desc.SampleDesc.Quality!=out.SampleDesc.Quality){
    ComPtr<ID3D11Texture2D> made;if(FAILED(s.device->CreateTexture2D(&out,nullptr,&made)))return false;r.tex=made;r.desc=out;
  }
  D3D11_BOX box{roi[0],roi[1],0,roi[2],roi[3],1};s.ctx->CopySubresourceRegion(r.tex.Get(),0,0,0,0,src,0,&box);r.sourceDesc=d;std::memcpy(r.roi,roi,sizeof(r.roi));r.valid=true;return true;
}
void clearCopies(State& s){for(unsigned e=0;e<2;++e){s.current[e].reset();s.previous[e].reset();}edvr::publishSubmitTexture(0,nullptr);edvr::publishSubmitTexture(1,nullptr);}
void resetStamps(State& s) {
  s.arrival=edvr::fssArrivalStampValue();s.chrome=edvr::fssChromeStampValue();s.stampsKnown=true;
  s.arrivalChanged=s.chromeChanged=0;s.arrivalRecent=s.chromeRecent=s.healGate=false;
}
bool sameProjection(const EdvrNativeFssFrame& a,const EdvrNativeFssFrame& b) {
  for(unsigned e=0;e<2;++e) {
    for(unsigned i=0;i<4;++i)if(std::fabs(a.frusta[e][i]-b.frusta[e][i])>1.e-5f)return false;
    for(unsigned i=0;i<12;++i)if(std::fabs(a.eyeToHead[e][i]-b.eyeToHead[e][i])>1.e-5f)return false;
  }
  return true;
}
bool healRect(float out[4]){
  float c[16]{};if(!edvr::readFssPanelRect(c)||!finite(c,8))return false;float u0=c[0],u1=c[0],v0=c[1],v1=c[1];for(unsigned i=0;i<4;++i){u0=(std::min)(u0,c[2*i]);u1=(std::max)(u1,c[2*i]);v0=(std::min)(v0,c[2*i+1]);v1=(std::max)(v1,c[2*i+1]);}
  if(u0<0||u1>1||v0<0||v1>1||u0>=u1||v0>=v1)return false;const float cu=(u0+u1)*.5f,cv=(v0+v1)*.5f,sx=131.176f/164.700f,sy=71.874f/92.655f;out[0]=cu+(u0-cu)*sx;out[1]=cv+(v0-cv)*sy;out[2]=cu+(u1-cu)*sx;out[3]=cv+(v1-cv)*sy;return true;
}
void latch(State& s){
  if(s.submitLatched)return;s.submitLatched=true;const LONG a=edvr::fssArrivalStampValue(),c=edvr::fssChromeStampValue();if(!s.stampsKnown){s.stampsKnown=true;s.arrival=a;s.chrome=c;if(a)s.arrivalChanged=s.sequence;if(c)s.chromeChanged=s.sequence;}else{if(a!=s.arrival){s.arrival=a;s.arrivalChanged=s.sequence;}if(c!=s.chrome){s.chrome=c;s.chromeChanged=s.sequence;}}
  const uint64_t aa=s.arrivalChanged?s.sequence-s.arrivalChanged:99,cc=s.chromeChanged?s.sequence-s.chromeChanged:99;s.arrivalRecent=aa<=3;s.chromeRecent=cc<=10;s.healGate=s.projectionCompatible&&s.sync.healMode!=0&&s.arrivalRecent&&s.chromeRecent;
}
HRESULT WINAPI begin(void* p,const EdvrNativeFssFrame* f){
  std::lock_guard<std::mutex> lock(mu);State*s=find(p);if(!s||!s->active||s!=live||!f||f->size!=sizeof(*f)||f->version!=EDVR_NATIVE_FSS_VERSION_1||f->generation!=s->generation||!f->referenceGeneration||!f->sequence||f->sequence<=s->floor||!finite(f->frusta[0],8)||!finite(f->eyeToHead[0],24))return E_INVALIDARG;
  if(f->referenceGeneration!=s->reference){clearCopies(*s);s->reference=f->referenceGeneration;resetStamps(*s);s->previousSequence=0;}
  if(s->frameOpen&&!sameProjection(s->frame,*f))clearCopies(*s);
  const uint64_t oldSequence=s->sequence;const bool hadPrevious=s->frameOpen;
  for(unsigned e=0;e<2;++e){std::swap(s->current[e],s->previous[e]);s->current[e].clear();}
  s->previousSequence=hadPrevious?oldSequence:0;
  if(!s->previousSequence||f->sequence!=s->previousSequence+1)for(unsigned e=0;e<2;++e)s->previous[e].clear();
  s->frame=*f;s->sequence=s->floor=f->sequence;s->frameOpen=true;s->consumed[0]=s->consumed[1]=false;s->sync=edvr::eyeSyncFromConfig(edvr::Config::get());s->projectionCompatible=parallel(*f);if(!s->projectionCompatible&&!s->projectionNoted){s->projectionNoted=true;edvr::Log::get().note("native fss: unsupported canted or unequal projection; healing is guarded off.");}s->healGate=false;s->submitLatched=false;return S_OK;
}
HRESULT WINAPI treat(void* p,uint64_t seq,uint32_t e,ID3D11Texture2D* src,const float* b,ID3D11Texture2D** out){
  if(out)*out=nullptr;std::lock_guard<std::mutex> lock(mu);State*s=find(p);if(!s||!s->active||s!=live||!s->frameOpen||GetCurrentThreadId()!=s->thread||e>1||!out||!src||seq!=s->sequence||seq!=s->floor||s->consumed[e]||!sameDevice(s->device,src))return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC d{};uint32_t roi[4]{};bool flip=false;
  if(FAILED(s->device->GetDeviceRemovedReason())||!validTexture(src,&d)||!bounds(b,d,roi,&flip))return E_INVALIDARG;
  s->consumed[e]=true;latch(*s);edvr::publishSubmitTexture(int(e),nullptr);
  if(flip||s->sync.healMode==0||!s->projectionCompatible||(!s->arrivalRecent&&!s->chromeRecent)) {
    s->current[e].clear();++s->gatedOut;return S_FALSE;
  }
  if(!snapshot(*s,e,src,d,roi))return S_FALSE;
  ++s->snapshots;edvr::publishSubmitTexture(int(e),s->current[e].tex.Get());
  if(!s->healGate||(s->sync.healMode==1&&e!=0)||(s->sync.healMode==2&&e!=1)){++s->gatedOut;return S_FALSE;}
  // Mode 2 must use this frame's left even if right was submitted first. A
  // recycled allocation whose validity was cleared is never a usable donor.
  Raw& left=s->current[0];
  Raw& right=s->sync.healMode==1?(s->current[1].valid?s->current[1]:s->previous[1]):s->current[1];
  if(!compatible(left,d,roi)||!compatible(right,d,roi)){++s->donorRefusals;return S_FALSE;}
  float rect[4]{};if(!healRect(rect)){++s->donorRefusals;return S_FALSE;}
  void* raw=edvrFssHealLeft(left.tex.Get(),right.tex.Get(),-s->frame.frusta[0][0],s->frame.frusta[0][1],s->sync.healMode,rect);
  if(!raw){++s->donorRefusals;return S_FALSE;}
  auto* result=static_cast<ID3D11Texture2D*>(raw);result->AddRef();*out=result;++s->healed;
  if(!s->healNoted){s->healNoted=true;edvr::Log::get().note("native fss: arrival heal engaged.");}return S_OK;
}
HRESULT WINAPI invalidate(void* p){std::lock_guard<std::mutex>lock(mu);State*s=find(p);if(!s||!s->active||s!=live)return E_INVALIDARG;clearCopies(*s);s->frameOpen=false;s->consumed[0]=s->consumed[1]=false;s->healGate=false;return S_OK;}
HRESULT WINAPI close(void* p){std::lock_guard<std::mutex>lock(mu);State*s=find(p);if(!s)return E_INVALIDARG;if(!s->active)return S_FALSE;edvr::Log::get().note("native fss totals: snapshots=%llu, healed=%llu, gated_out=%llu, donor_refusals=%llu.",(unsigned long long)s->snapshots,(unsigned long long)s->healed,(unsigned long long)s->gatedOut,(unsigned long long)s->donorRefusals);clearCopies(*s);edvr::fssHealRelease();s->active=false;s->frameOpen=false;s->ctx.Reset();s->device=nullptr;if(live==s)live=nullptr;return S_OK;}
}
extern "C" HRESULT WINAPI edvrAcquireNativeFss(const EdvrNativeFssRequest*r,EdvrNativeFssTable*t){
  if(!t||t->size!=sizeof(*t)||t->version!=EDVR_NATIVE_FSS_VERSION_1)return E_INVALIDARG;*t={sizeof(*t),EDVR_NATIVE_FSS_VERSION_1};if(!r||r->size!=sizeof(*r)||r->version!=EDVR_NATIVE_FSS_VERSION_1||!r->gameDevice||!r->generation||FAILED(r->gameDevice->GetDeviceRemovedReason()))return E_INVALIDARG;std::lock_guard<std::mutex>lock(mu);if(live||used==kCapacity)return E_PENDING;State&s=pool[used++];s={};s.active=true;s.device=r->gameDevice;s.thread=GetCurrentThreadId();s.generation=r->generation;s.device->GetImmediateContext(&s.ctx);if(!s.ctx){s.active=false;return E_FAIL;}live=&s;t->context=&s;t->beginFrame=begin;t->treatEye=treat;t->invalidate=invalidate;t->close=close;return S_OK;
}
