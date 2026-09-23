#include "../common/native_sharpen.h"
#include "sharpen_pass.h"
#include "ui_layer.h"
#include "ui_layer_math.h"  // uiLayerUvFromRegion
#include "../common/config.h"
#include "../common/log.h"
#include "../common/supersample_math.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <atomic>

namespace {
struct State {
  ID3D11Device* device = nullptr;
  DWORD thread = 0;
  uint64_t generation = 0, floor = 0;
  bool active = false, invalidated = false, stoodDown = false;
  bool offNoted = false, engagedNoted = false; unsigned engagedMask = 0;
  bool consumed[2]{};
  float strength = 0;
  uint64_t treated = 0, off = 0, refusals = 0, invalidations = 0;
};
State pool[16]; unsigned used = 0; State* current = nullptr; std::mutex mutex;
std::atomic<bool> available{false};

State* identify(void* p) { for (unsigned i=0;i<used;++i) if (p==&pool[i]) return &pool[i]; return nullptr; }
bool finite(const float* p, size_t n) { for(size_t i=0;i<n;++i) if(!std::isfinite(p[i])) return false; return true; }
bool sameDevice(ID3D11Device* expected, ID3D11Texture2D* source) {
  if(!expected||!source)return false; ID3D11Device* owner=nullptr; source->GetDevice(&owner); if(!owner)return false;
  IUnknown *a=nullptr,*b=nullptr; bool ok=SUCCEEDED(expected->QueryInterface(IID_IUnknown,(void**)&a))&&SUCCEEDED(owner->QueryInterface(IID_IUnknown,(void**)&b))&&a==b;
  if(a)a->Release();if(b)b->Release();owner->Release();return ok;
}
bool validBounds(const float* b) {
  if(!b)return true; if(!finite(b,4))return false;
  return b[0]>=0&&b[0]<=1&&b[1]>=0&&b[1]<=1&&b[2]>=0&&b[2]<=1&&b[3]>=0&&b[3]<=1&&b[0]!=b[2]&&b[1]!=b[3];
}
bool validTexture(ID3D11Texture2D* t) {
  if(!t)return false; D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
  return d.Width&&d.Height&&d.MipLevels==1&&d.ArraySize==1&&d.SampleDesc.Count==1&&d.SampleDesc.Quality==0&&d.Usage==D3D11_USAGE_DEFAULT&&d.CPUAccessFlags==0;
}
float readStrength() {
  float v=edvr::Config::get().getFloat("fix.render_sharpness",0.f);
  if(!std::isfinite(v))return 0.f; return (v<0.f)?0.f:(v>1.f)?1.f:v;
}
void clear(ID3D11Texture2D** out,float* b) { if(out)*out=nullptr; if(b)std::memset(b,0,4*sizeof(float)); }

HRESULT WINAPI treat(void* p,uint64_t seq,uint32_t eye,ID3D11Texture2D* source,const float* bounds,ID3D11Texture2D** output,float* outBounds) {
  clear(output,outBounds); std::lock_guard<std::mutex> lock(mutex); State* s=identify(p);
  if(!s||!s->active||s!=current||GetCurrentThreadId()!=s->thread||eye>1||!seq||seq<s->floor||(s->invalidated&&seq<=s->floor)||!source||!output||!outBounds||!validBounds(bounds)||!validTexture(source)||FAILED(s->device->GetDeviceRemovedReason())||!sameDevice(s->device,source)) return E_INVALIDARG;
  if(seq==s->floor&&s->consumed[eye])return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc); uint32_t region[4]{}; bool flipU=false,flipV=false;
  if(!edvr::supersampleRegionFromBounds(desc.Width,desc.Height,bounds,region,&flipU,&flipV))return E_INVALIDARG;
  if(seq>s->floor) { s->floor=seq;s->consumed[0]=s->consumed[1]=false;s->invalidated=false;s->strength=readStrength(); }
  s->consumed[eye]=true;
  const float full[4]={flipU?1.f:0.f,flipV?1.f:0.f,flipU?0.f:1.f,flipV?0.f:1.f};
  // fix.ui_quality's door (ui_layer.h). This is the last d3d11 step before
  // the runtime composites EDVR's own menu and captures the eye, so the UI
  // layer is composited here, AFTER RCAS: the sharpener never rings the
  // text. It runs whether or not sharpening is on -- with it off the layer
  // lands on the frame as it arrived (the pass's output, or the game's own
  // image when the pass declined). The door is noted first, for every eye,
  // so the next frame's draws know it is there to composite them.
  edvr::uiLayerDoorSeen(seq,eye,source);
  // The layer's rectangle: this input region (rounded, unflipped) over the
  // source's size -- the same whether the frame is the source or the
  // sharpened copy of that region.
  float layerUv[4]; edvr::uiLayerUvFromRegion(region,desc.Width,desc.Height,layerUv);
  if(s->strength<=0.f||s->stoodDown) {
    ++s->off; if(s->strength<=0.f&&!s->offNoted){s->offNoted=true;edvr::Log::get().note("native sharpen: off (fix.render_sharpness=0); eyes consumed as passthrough.");}
    if(ID3D11Texture2D* layered=edvr::uiLayerComposite(seq,eye,source,region,layerUv)) { *output=layered; std::memcpy(outBounds,full,sizeof(full)); return S_OK; }
    return S_FALSE;
  }
  void* raw=edvrSharpen(source,int(eye),bounds,s->strength);
  if(!raw) {
    s->stoodDown=true;++s->refusals; edvr::Log::get().note("native sharpen: pass refused; standing down for this session.");
    if(ID3D11Texture2D* layered=edvr::uiLayerComposite(seq,eye,source,region,layerUv)) { *output=layered; std::memcpy(outBounds,full,sizeof(full)); return S_OK; }
    return S_FALSE;
  }
  ID3D11Texture2D* result=static_cast<ID3D11Texture2D*>(raw);
  // The sharpened texture is the input region, region-sized.
  const uint32_t whole[4]={0,0,region[2]-region[0],region[3]-region[1]};
  if(ID3D11Texture2D* layered=edvr::uiLayerComposite(seq,eye,result,whole,layerUv)) result=layered; else result->AddRef();
  *output=result; std::memcpy(outBounds,full,sizeof(full)); ++s->treated; s->engagedMask|=1u<<eye; if(s->engagedMask==3&&!s->engagedNoted){s->engagedNoted=true;edvr::Log::get().note("native sharpen: engaged for both eyes.");} return S_OK;
}
HRESULT WINAPI invalidate(void* p) { std::lock_guard<std::mutex> lock(mutex);State*s=identify(p);if(!s||!s->active||s!=current)return E_INVALIDARG;s->invalidated=true;s->consumed[0]=s->consumed[1]=false;++s->invalidations;return S_OK; }
HRESULT WINAPI close(void* p) { std::lock_guard<std::mutex> lock(mutex);State*s=identify(p);if(!s)return E_INVALIDARG;if(!s->active)return S_FALSE;edvr::Log::get().note("native sharpen totals: treated=%llu, off=%llu, refusals=%llu, invalidations=%llu, stood_down=%u.",(unsigned long long)s->treated,(unsigned long long)s->off,(unsigned long long)s->refusals,(unsigned long long)s->invalidations,unsigned(s->stoodDown));s->active=false;s->device=nullptr;s->invalidated=true;available.store(false);if(current==s)current=nullptr;return S_OK; }
}

extern "C" HRESULT WINAPI edvrAcquireNativeSharpen(const EdvrNativeSharpenRequest* r,EdvrNativeSharpenTable* t) {
  if(!t||t->size!=sizeof(*t)||t->version!=EDVR_NATIVE_SHARPEN_VERSION_1)return E_INVALIDARG;*t={sizeof(*t),EDVR_NATIVE_SHARPEN_VERSION_1};
  if(!r||r->size!=sizeof(*r)||r->version!=EDVR_NATIVE_SHARPEN_VERSION_1||!r->gameDevice||!r->generation)return E_INVALIDARG;
  IUnknown* identity=nullptr;if(FAILED(r->gameDevice->QueryInterface(IID_IUnknown,(void**)&identity)))return E_INVALIDARG;identity->Release();
  std::lock_guard<std::mutex> lock(mutex);if(current||used==16)return E_PENDING;State&s=pool[used++];s={};s.device=r->gameDevice;s.thread=GetCurrentThreadId();s.generation=r->generation;s.active=true;current=&s;available.store(true);t->context=&s;t->treatEye=treat;t->invalidate=invalidate;t->close=close;edvr::Log::get().note("native sharpen: acquired generation %llu.",(unsigned long long)r->generation);return S_OK;
}

namespace edvr { bool nativeSharpenActive() { return available.load(); } }
