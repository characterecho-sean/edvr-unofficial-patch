#include "../common/native_temporal.h"

#include "temporal_pass.h"
#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/temporal_math.h"
#include "../common/supersample_math.h"
#include "../common/log.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <mutex>

extern "C" void edvrEyeCaptureUntreated(void*, int, const float*);

namespace {

struct History {
  bool valid = false;
  uint64_t sequence = 0, reference = 0;
  float head[12]{};
  float eye[12]{};
  float otherEye[12]{};
  float frustum[4]{};
  uint32_t width = 0, height = 0;
  uint32_t outputWidth = 0, outputHeight = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
struct Settings {
  bool on=false,dlaa=false,upscale=false,jitter=true,lag=false; int motion=3,signX=1,signY=1;
  float blend=.90f,clamp=1.f;
};

struct State {
  ID3D11Device* device = nullptr;
  DWORD thread = 0;
  uint64_t generation = 0;
  bool active = false, begun = false, standDown = false;
  uint64_t sequence = 0, reference = 0;
  float head[12]{}, eyes[2][12]{}, frusta[2][4]{};
  float shift[2][2]{};
  uint32_t width[2]{}, height[2]{};
  uint32_t recW=0,recH=0; bool flipped[2]{};
  uint32_t frameCounter = 0;
  bool treated[2]{};
  History history[2];
  float nearZ[2]{}, farZ[2]{};
  uint64_t projectionSequence[2]{};
  bool projectionKnown[2]{};
  Settings currentSettings{}, pendingSettings{};
  float renderedJitter[2][2]{},previousJitter[2][2]{};
  uint64_t treatedCount=0,projectionReads=0,missingProjections=0,jitterFrames=0,resets=0;
  bool flippedNoted=false,engagedNoted=false;
};

State pool[16]; unsigned used = 0; State* current = nullptr; std::mutex mutex;

State* identify(void* p) {
  for (unsigned i = 0; i < used; ++i) if (p == &pool[i]) return &pool[i];
  return nullptr;
}
bool finite(const float* p, size_t n) {
  for (size_t i=0;i<n;++i) if (!std::isfinite(p[i])) return false;
  return true;
}
bool rigid(const float* p) {
  if (!finite(p,12)) return false;
  for (int a=0;a<3;++a) for (int b=a;b<3;++b) {
    float d=0; for (int c=0;c<3;++c) d += p[a*4+c]*p[b*4+c];
    if (std::fabs(d-(a==b?1.f:0.f))>.002f) return false;
  }
  const float det=p[0]*(p[5]*p[10]-p[6]*p[9])-p[1]*(p[4]*p[10]-p[6]*p[8])+p[2]*(p[4]*p[9]-p[5]*p[8]);
  return std::fabs(det-1.f)<=.003f;
}
bool frustum(const float* p) { return finite(p,4) && p[0] < p[1] && p[2] < p[3]; }
void eyeWorld(const float* head,const float* eye,float r[9],float t[3]) {
  for(int i=0;i<3;++i)for(int j=0;j<3;++j){r[i*3+j]=0;for(int k=0;k<3;++k)r[i*3+j]+=head[i*4+k]*eye[k*4+j];}
  for(int i=0;i<3;++i)t[i]=head[i*4+3]+head[i*4]*eye[3]+head[i*4+1]*eye[7]+head[i*4+2]*eye[11];
}
void eyeDelta(const History& h,const State& s,const float* eyePrev,const float* eyeNow,float d[9],float t[3]) {
  float rp[9],rn[9],tp[3],tn[3],rt[9];eyeWorld(h.head,eyePrev,rp,tp);eyeWorld(s.head,eyeNow,rn,tn);
  edvr::temporalTranspose3(rp,rt);edvr::temporalMul3(rt,rn,d);float dt[3]={tn[0]-tp[0],tn[1]-tp[1],tn[2]-tp[2]};edvr::temporalApply3(rt,dt,t);
}
bool sameDevice(ID3D11Device* expected, ID3D11Texture2D* source) {
  if (!expected || !source) return false;
  ID3D11Device* owner=nullptr; source->GetDevice(&owner);
  if(!owner)return false; IUnknown *a=nullptr,*b=nullptr; const bool ok=SUCCEEDED(expected->QueryInterface(IID_IUnknown,(void**)&a))&&SUCCEEDED(owner->QueryInterface(IID_IUnknown,(void**)&b))&&a==b;
  if(a)a->Release();if(b)b->Release();owner->Release();return ok;
}
Settings readConfig() {
  Settings s; auto& c=edvr::Config::get(); const auto mode=c.getString("fix.temporal_aa","off");
  const bool known=_stricmp(mode.c_str(),"on")==0||_stricmp(mode.c_str(),"dlaa")==0||_stricmp(mode.c_str(),"dlss")==0;
  s.on=known; s.dlaa=_stricmp(mode.c_str(),"dlaa")==0||_stricmp(mode.c_str(),"dlss")==0;
  s.upscale=_stricmp(mode.c_str(),"dlss")==0;
  s.jitter=_stricmp(c.getString("experimental.temporal_aa_jitter","on").c_str(),"off")!=0;
  s.blend=c.getFloat("experimental.temporal_aa_blend",.90f); if(!std::isfinite(s.blend))s.blend=.90f;
  s.clamp=c.getFloat("experimental.temporal_aa_clamp",1.f); if(!std::isfinite(s.clamp))s.clamp=1.f;
  s.blend=(std::max)(.5f,(std::min)(.95f,s.blend)); s.clamp=(std::max)(.5f,(std::min)(3.f,s.clamp));
  const auto m=c.getString("advanced.temporal_aa_motion","depth");
  s.motion=_stricmp(m.c_str(),"none")==0?0:_stricmp(m.c_str(),"head")==0?1:3;
  const auto sign=c.getString("advanced.temporal_aa_jitter_sign","as_is"); s.signX=s.signY=1;
  if(_stricmp(sign.c_str(),"flip_x")==0)s.signX=-1; else if(_stricmp(sign.c_str(),"flip_y")==0)s.signY=-1;
  else if(_stricmp(sign.c_str(),"flip_both")==0)s.signX=s.signY=-1;
  s.lag=c.getFloat("advanced.temporal_aa_jitter_lag",0)>=.5f; return s;
}
void reset(State& s) { for(auto& h:s.history) h={}; s.treated[0]=s.treated[1]=false; }
bool sameHistorySettings(const Settings& a,const Settings& b) {
  return a.on==b.on&&a.dlaa==b.dlaa&&a.upscale==b.upscale&&a.jitter==b.jitter&&
      a.motion==b.motion&&a.signX==b.signX&&a.signY==b.signY&&a.lag==b.lag;
}
const char* mode(const Settings& s) { return !s.on?"off":s.upscale?"dlss":s.dlaa?"dlaa":"on"; }

HRESULT WINAPI begin(void* p,const EdvrNativeTemporalFrame* f,EdvrNativeTemporalProjection* out) {
  std::lock_guard<std::mutex> lock(mutex); State* s=identify(p);
  if(!s||!s->active||s!=current||!f||!out||f->size!=sizeof(*f)||f->version!=EDVR_NATIVE_TEMPORAL_VERSION_1||
     f->generation!=s->generation||!f->sequence||!f->referenceGeneration||
     !finite(f->head,12)||!rigid(f->head)) return E_INVALIDARG;
  if(out->size!=sizeof(*out)||out->version!=EDVR_NATIVE_TEMPORAL_VERSION_1)return E_INVALIDARG;
  if(f->sequence<=s->sequence||!f->recommendedWidth||!f->recommendedHeight||
     f->recommendedWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||f->recommendedHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)return E_INVALIDARG;
  for(int e=0;e<2;++e) if(!rigid(f->eyeToHead[e])||!frustum(f->frusta[e])) return E_INVALIDARG;
  const Settings next=s->pendingSettings;
  const bool settingsChanged=!sameHistorySettings(next,s->currentSettings);
  if(!s->begun||s->reference!=f->referenceGeneration||f->sequence!=s->sequence+1||settingsChanged) reset(*s);
  if(settingsChanged)edvr::Log::get().note("native temporal: mode=%s, motion=%d, jitter=%u; history reset at the frame boundary.",mode(next),next.motion,unsigned(next.jitter));
  std::memcpy(s->previousJitter,s->renderedJitter,sizeof(s->previousJitter));
  std::memset(s->renderedJitter,0,sizeof(s->renderedJitter));
  s->currentSettings=next;
  s->begun=true;s->sequence=f->sequence;s->reference=f->referenceGeneration;
  s->recW=f->recommendedWidth;s->recH=f->recommendedHeight;
  std::memcpy(s->head,f->head,sizeof(s->head));std::memcpy(s->eyes,f->eyeToHead,sizeof(s->eyes));std::memcpy(s->frusta,f->frusta,sizeof(s->frusta));
  s->treated[0]=s->treated[1]=false;
  s->projectionKnown[0]=s->projectionKnown[1]=false;
  std::memset(out,0,sizeof(*out));out->size=sizeof(*out);out->version=EDVR_NATIVE_TEMPORAL_VERSION_1;
  ++s->frameCounter; float jx=0,jy=0; std::memset(s->shift,0,sizeof(s->shift));
  if(s->currentSettings.on&&!s->standDown&&s->currentSettings.jitter) for(int e=0;e<2;++e) if(s->width[e]&&s->height[e]&&!s->flipped[e]) {
    edvr::temporalJitter(s->frameCounter,&jx,&jy); float dx=0,dy=0;
    edvr::temporalJitterToTangents(jx,jy,s->frusta[e],s->width[e],s->height[e],&dx,&dy);
    out->tangentShift[e][0]=dx;out->tangentShift[e][1]=dy;
    s->shift[e][0]=out->tangentShift[e][0];s->shift[e][1]=out->tangentShift[e][1];
  }
  if(s->shift[0][0]||s->shift[0][1]||s->shift[1][0]||s->shift[1][1])++s->jitterFrames;
  return S_OK;
}

HRESULT WINAPI noteProjection(void* p,uint64_t seq,uint32_t eye,float nearZ,float farZ) {
  std::lock_guard<std::mutex> lock(mutex); State* s=identify(p);
  if(!s||!s->active||s!=current||!s->begun||eye>1||seq!=s->sequence||
     !std::isfinite(nearZ)||!std::isfinite(farZ)||!(nearZ>0&&farZ>nearZ)) return E_INVALIDARG;
  if(!s->projectionKnown[eye]||nearZ<s->nearZ[eye]){s->nearZ[eye]=nearZ;s->farZ[eye]=farZ;}
  s->projectionSequence[eye]=seq;s->projectionKnown[eye]=true;++s->projectionReads;return S_OK;
}

HRESULT WINAPI treat(void* p,uint64_t seq,uint32_t eye,ID3D11Texture2D* source,const float* box,ID3D11Texture2D** output,float* outBox) {
  if(output)*output=nullptr;if(outBox)std::memset(outBox,0,16);std::lock_guard<std::mutex> lock(mutex);State* s=identify(p);
  if(!s||!s->active||s!=current||GetCurrentThreadId()!=s->thread||!source||!output||!outBox||eye>1||seq!=s->sequence||s->treated[eye]||!s->begun||!sameDevice(s->device,source))return E_INVALIDARG;
  if(box&&(!finite(box,4)||box[0]==box[2]||box[1]==box[3]))return E_INVALIDARG;
  if(box) for(int i=0;i<4;++i) if(box[i]<0.0f||box[i]>1.0f)return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
  if(!d.Width||!d.Height||d.ArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||
     d.Usage!=D3D11_USAGE_DEFAULT||d.CPUAccessFlags||FAILED(s->device->GetDeviceRemovedReason()))return E_INVALIDARG;
  const float b[4]={box?box[0]:0,box?box[1]:0,box?box[2]:1,box?box[3]:1};
  const bool flipU=b[0]>b[2],flipV=b[1]>b[3];
  uint32_t region[4]{}; bool fu=false,fv=false;
  if(!edvr::supersampleRegionFromBounds(d.Width,d.Height,box,region,&fu,&fv))return E_INVALIDARG;
  const uint32_t w=region[2]-region[0],h=region[3]-region[1];if(!w||!h)return E_INVALIDARG;
  if (s->flipped[eye] != (flipU||flipV)) { s->history[eye]={}; s->flipped[eye]=(flipU||flipV); }
  s->width[eye]=w;s->height[eye]=h;s->pendingSettings=readConfig();
  if (eye==0) {
    edvr::announceEyeTextureSize(w,h);
    const float outer=-s->frusta[0][0], inner=s->frusta[0][1];
    const float top=-s->frusta[0][2], bottom=s->frusta[0][3];
    if(std::isfinite(outer)&&std::isfinite(inner)&&outer>0&&inner>0)
      edvr::announceEyeTangents(outer,inner);
    if(std::isfinite(top)&&std::isfinite(bottom)&&top>0&&bottom>0)
      edvr::announceEyeTangentsVertical(top,bottom);
  }
  if(flipU||flipV) {
    if(!s->flippedNoted){s->flippedNoted=true;edvr::Log::get().note("native temporal: flipped eye bounds pass through; temporal reprojection for flipped inputs remains pending.");}
    s->history[eye]={};
  }
  if(!s->currentSettings.on||s->standDown||flipU||flipV){edvrEyeCaptureUntreated(source,int(eye),b);s->treated[eye]=true;return S_FALSE;}
  if(!s->projectionKnown[eye]||s->projectionSequence[eye]!=seq){
    if(s->missingProjections++<4)edvr::Log::get().note("native temporal: eye %u sequence %llu has no matrix query for this frame; history invalidated.",eye,(unsigned long long)seq);
    s->treated[eye]=true;s->history[eye]={};
    // A cached game projection is not evidence that the newly advertised
    // jitter was rendered. Do not submit such pixels with invented metadata.
    return (s->shift[eye][0]||s->shift[eye][1])?E_PENDING:S_FALSE;
  }
  unsigned outW=0,outH=0;
  if(s->currentSettings.upscale&&w*50<s->recW*49&&h*50<s->recH*49){outW=s->recW;outH=s->recH;}
  const bool resetHistory=!s->history[eye].valid||s->history[eye].reference!=s->reference||s->history[eye].sequence+1!=seq||
      s->history[eye].width!=w||s->history[eye].height!=h||s->history[eye].format!=d.Format||
      s->history[eye].outputWidth!=outW||s->history[eye].outputHeight!=outH;
  float delta[9]={1,0,0,0,1,0,0,0,1},trans[3]={},transOther[3]={}; const float* pd=nullptr;const float* pt=nullptr;const float* pto=nullptr;
  float headDegrees=0;
  if(!resetHistory){
    const auto& hst=s->history[eye];
    eyeDelta(hst,*s,hst.eye,s->eyes[eye],delta,trans);
    float swapped[9];eyeDelta(hst,*s,hst.otherEye,s->eyes[1-eye],swapped,transOther);
    float headDelta[9];edvr::temporalHeadDelta(hst.head,s->head,headDelta);headDegrees=edvr::temporalRotationAngleDeg(headDelta);
    pd=delta;pt=trans;pto=transOther;
  }
  const unsigned flags=(resetHistory?1u:0u)|(s->currentSettings.dlaa?2u:0u);float prev[4];std::memcpy(prev,s->history[eye].valid?s->history[eye].frustum:s->frusta[eye],16);
  const float outJx=s->shift[eye][0],outJy=s->shift[eye][1]; float px=0,py=0;
  if(w){px=-outJx*float(w)/(s->frusta[eye][1]-s->frusta[eye][0]);} if(h){py=outJy*float(h)/(s->frusta[eye][3]-s->frusta[eye][2]);}
  s->renderedJitter[eye][0]=px;s->renderedJitter[eye][1]=py;
  if(s->currentSettings.lag){px=s->previousJitter[eye][0];py=s->previousJitter[eye][1];}
  px*=s->currentSettings.signX;py*=s->currentSettings.signY;
  const float eyeOffset[3]={s->eyes[eye][3],s->eyes[eye][7],s->eyes[eye][11]};
  edvrTemporalAaNoteHead(int(eye),pd?s->history[eye].head:nullptr,pd?s->head:nullptr,pd?eyeOffset:nullptr);
  void* raw=edvrTemporalAa(source,int(eye),b,s->frusta[eye],prev,px,py,pd,pt,pto,s->nearZ[eye],s->farZ[eye],headDegrees,s->currentSettings.motion,s->currentSettings.blend,s->currentSettings.clamp,outW,outH,flags);
  s->treated[eye]=true;
  if(!raw){s->standDown=true;edvr::Log::get().note("native temporal: pass refused eye %u; future jitter disabled, current raw image retains its rendered FOV.",eye);return S_FALSE;}
  ID3D11Texture2D* result=static_cast<ID3D11Texture2D*>(raw);result->AddRef();*output=result;++s->treatedCount;
  if(resetHistory)++s->resets;
  if(!s->engagedNoted){s->engagedNoted=true;edvr::Log::get().note("native temporal: engaged mode=%s, input=%ux%u, output=%ux%u, sequence=%llu; producer filtering before menu and shared capture.",mode(s->currentSettings),w,h,outW?outW:w,outH?outH:h,(unsigned long long)seq);}
  outBox[0]=b[0]>b[2]?1.f:0.f;outBox[2]=b[0]>b[2]?0.f:1.f;outBox[1]=b[1]>b[3]?1.f:0.f;outBox[3]=b[1]>b[3]?0.f:1.f;
  auto& hst=s->history[eye];hst.valid=true;hst.sequence=seq;hst.reference=s->reference;
  std::memcpy(hst.head,s->head,sizeof(hst.head));std::memcpy(hst.eye,s->eyes[eye],sizeof(hst.eye));
  std::memcpy(hst.otherEye,s->eyes[1-eye],sizeof(hst.otherEye));std::memcpy(hst.frustum,s->frusta[eye],sizeof(hst.frustum));
  hst.width=w;hst.height=h;hst.outputWidth=outW;hst.outputHeight=outH;hst.format=d.Format;return S_OK;
}
HRESULT WINAPI invalidate(void* p){std::lock_guard<std::mutex> lock(mutex);State*s=identify(p);if(!s||!s->active||s!=current)return E_INVALIDARG;reset(*s);s->begun=false;std::memset(s->shift,0,sizeof(s->shift));std::memset(s->renderedJitter,0,sizeof(s->renderedJitter));return S_OK;}
HRESULT WINAPI close(void* p){
  std::lock_guard<std::mutex> lock(mutex);State*s=identify(p);if(!s)return E_INVALIDARG;if(!s->active)return S_FALSE;
  edvr::Log::get().note("native temporal totals: treated=%llu, jitter_frames=%llu, projection_reads=%llu, missing_projections=%llu, resets=%llu, stood_down=%u.",
      (unsigned long long)s->treatedCount,(unsigned long long)s->jitterFrames,(unsigned long long)s->projectionReads,
      (unsigned long long)s->missingProjections,(unsigned long long)s->resets,unsigned(s->standDown));
  reset(*s);s->active=false;s->begun=false;s->device=nullptr;if(current==s)current=nullptr;return S_OK;
}
}

extern "C" HRESULT WINAPI edvrAcquireNativeTemporal(const EdvrNativeTemporalRequest* r,EdvrNativeTemporalTable* t){
  if(!t||t->size!=sizeof(*t)||t->version!=EDVR_NATIVE_TEMPORAL_VERSION_1)return E_INVALIDARG;*t={sizeof(*t),EDVR_NATIVE_TEMPORAL_VERSION_1};
  if(!r||r->size!=sizeof(*r)||r->version!=EDVR_NATIVE_TEMPORAL_VERSION_1||!r->gameDevice||!r->generation)return E_INVALIDARG;
  std::lock_guard<std::mutex> lock(mutex);if(current||used==16)return E_PENDING;State&s=pool[used++];s.device=r->gameDevice;s.thread=GetCurrentThreadId();s.generation=r->generation;s.pendingSettings=readConfig();s.currentSettings=s.pendingSettings;s.active=true;current=&s;
  t->context=&s;t->beginFrame=begin;t->noteProjection=noteProjection;t->treatEye=treat;t->invalidate=invalidate;t->close=close;return S_OK;
}
