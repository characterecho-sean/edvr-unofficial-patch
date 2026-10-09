#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../../src/common/native_temporal.h"
#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/d3d11/temporal_pass.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/ui_surfaces.h"
#include "../../src/d3d11/vr_camera_census.h"
#include "../../src/d3d11/dlss_floor.h"
#include "../../src/openxr/native_temporal_client.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/glitch_frame.h"
#include "../../src/d3d11/transition_flash_eye_base.h"
#pragma comment(linker, "/EXPORT:edvrAcquireNativeTemporal")
using Microsoft::WRL::ComPtr;
unsigned checks=0,failures=0,dumps=0;
void check(bool x,const char* why){++checks;if(!x){++failures;std::printf("FAIL: %s\n",why);}}
void require(bool x,const char* why){check(x,why);if(!x)throw std::runtime_error(why);}
bool closeFloat(float a,float b,float epsilon=1e-5f){return std::fabs(a-b)<=epsilon;}
struct Call {
  unsigned eye=0,flags=0,outW=0,outH=0;float jx=0,jy=0,nearZ=0,farZ=0,headDeg=0;
  float delta[9]{},translation[3]{},swapped[3]{},tanPrev[4]{},tanNow[4]{};
  bool motion=false,head=false;float previousHead[12]{},currentHead[12]{},offset[3]{};
};
Call note;std::vector<Call> calls;bool passSucceeds=true;
// P1-1 (review 2026-09-23): the pass runs INSIDE treat(), which holds the
// channel's mutex, and makes its targets through the device's
// CreateTexture2D -- where fix.ui_quality's surfaces ask for the
// recommendation. The stub asks from there, exactly as the create hook
// would; MSVC's std::mutex throws on a re-lock from its own thread.
unsigned reentries=0,reentryThrows=0;uint32_t reentryRecW=0,reentryRecH=0;bool reentryJitter=true,reentryWarm=true,reentryGeometry=true;
float reentryUp=0,reentryDown=0;
void askFromInsideTreat(){
  ++reentries;
  try{uint32_t w=0,h=0;if(edvr::nativeTemporalRecommended(&w,&h)){reentryRecW=w;reentryRecH=h;}
      edvr::nativeTemporalVerticalTangents(&reentryUp,&reentryDown);
      uint64_t sq=0;float x=0,y=0;uint32_t a=0,b=0;reentryJitter=edvr::nativeTemporalDrawJitter(0,&sq,&x,&y,&a,&b);
      float fr[4]{},sh[2]{};reentryGeometry=edvr::nativeTemporalEyeGeometry(0,&sq,fr,sh);
      ID3D11Device* dv=nullptr;unsigned long th=0;reentryWarm=edvr::nativeTemporalWarmTarget(&dv,&th);}
  catch(...){++reentryThrows;}
}
extern "C" void edvrTemporalAaNoteHead(int eye,const float* previous,const float* current,const float* offset) {
  note={};note.eye=unsigned(eye);note.head=previous&&current&&offset;
  if(note.head){std::memcpy(note.previousHead,previous,48);std::memcpy(note.currentHead,current,48);std::memcpy(note.offset,offset,12);}
}
extern "C" void* edvrTemporalAa(void* source,int eye,const float*,const float* now,const float* previous,
    float jx,float jy,const float* delta,const float* translation,const float* swapped,float nearZ,float farZ,
    float headDeg,int,float,float,unsigned outW,unsigned outH,unsigned flags) {
  askFromInsideTreat();
  Call c=note;c.eye=unsigned(eye);c.jx=jx;c.jy=jy;c.nearZ=nearZ;c.farZ=farZ;c.headDeg=headDeg;
  c.outW=outW;c.outH=outH;c.flags=flags;c.motion=delta&&translation;
  if(delta)std::memcpy(c.delta,delta,36);if(translation)std::memcpy(c.translation,translation,12);
  if(swapped)std::memcpy(c.swapped,swapped,12);std::memcpy(c.tanNow,now,16);std::memcpy(c.tanPrev,previous,16);
  calls.push_back(c);return passSucceeds?source:nullptr; // borrowed; provider AddRefs
}
extern "C" void edvrEyeCaptureUntreated(void*,int,const float*){++dumps;}
// fix.ui_quality's door hook (src/d3d11/ui_layer.cpp): the pass tells the
// layer which eyes it handed on, so the layer arms only behind a treated
// frame. Recorded here, asserted below.
unsigned uiLayerNotes=0;uint64_t uiLayerNoteSeq=0;uint32_t uiLayerNoteEye=9;const void* uiLayerNoteOut=nullptr;
unsigned uiLayerSubmits=0;const void* uiLayerSubmitted[2]{};
// The VR world route's scene reset (src/d3d11/vr_world_route.cpp, docs section 82): skipEye tells it a withheld frame.
unsigned worldRouteSceneResets=0;
// guard.cpp's crash breadcrumb (src/common/proxy.cpp in the product): the glue cases link guard.cpp for CodeHook.
namespace edvr{void breadcrumb(const char*){}}
namespace edvr{void uiLayerNoteTemporal(uint64_t seq,uint32_t eye,const void* out){++uiLayerNotes;uiLayerNoteSeq=seq;uiLayerNoteEye=eye;uiLayerNoteOut=out;}
void uiLayerNoteSubmitted(uint64_t,uint32_t eye,const void* submitted){++uiLayerSubmits;if(eye<2)uiLayerSubmitted[eye]=submitted;}
void vrWorldRouteNoteSceneReset(){++worldRouteSceneResets;}}
// The served floor's NGX query (dlaa.cpp), shaped the way the flights'
// modes lines read (edvr_gfx_20260923_153446.log line 435): quality,
// balanced and performance take half the output up to the output, ultra
// performance only its own point at a third. `stingy` raises the floor
// three pixels at every output under 4000, as a runtime rounding its own
// way might, so the rule's first cut misses and NGX's answer must step it
// down; a nonzero `onlyW` answers for that output width alone.
bool ngxAnswers=true,stingy=false;unsigned rangeQueries=0;uint32_t onlyW=0;
namespace edvr{bool dlssModeRanges(ID3D11Device*,uint32_t outW,uint32_t outH,DlssModeRange modes[kDlssModeCount]){
  ++rangeQueries;for(int k=0;k<kDlssModeCount;++k)modes[k]=DlssModeRange{};
  if(!ngxAnswers||!outW||!outH||(onlyW&&outW!=onlyW))return false;
  const unsigned extra=stingy&&outW<4000?3u:0u,hw=(outW+1)/2+extra,hh=(outH+1)/2+extra;
  const unsigned optW[3]={(outW*2+1)/3,(outW*58+50)/100,hw},optH[3]={(outH*2+1)/3,(outH*58+50)/100,hh};
  for(int k=0;k<3;++k){auto& m=modes[k];m.ok=true;m.optW=optW[k];m.optH=optH[k];m.minW=hw;m.minH=hh;m.maxW=outW;m.maxH=outH;}
  auto& u=modes[3];u.ok=true;u.optW=u.minW=u.maxW=(outW+1)/3;u.optH=u.minH=u.maxH=(outH+1)/3;return true;}}
// The VR world route's surface as native_temporal.cpp sees it (src/d3d11/vr_world_route.h), the UI layer's preflight
// for a layer-only eye (src/d3d11/ui_layer.cpp) and the raw clear the black frame uses (src/d3d11/vscreen.cpp), each a
// stub the cases at the end of run() drive. At their defaults -- the route owns nothing and took no eye -- every case
// before that block is the key-off behaviour the route must leave exactly as it was.
bool routeOwns=false;bool routeTookEye[2]={false,false};uint64_t routeTookSeq=0;int gapAnswer=0;
unsigned gapCalls=0,rawClears=0;ID3D11Texture2D* gapFrame=nullptr;
namespace edvr{
bool vrWorldRouteOwnsNextFrame(){return routeOwns;}
// The layer's one predicate for a layer-only eye (src/d3d11/ui_layer.h): the route's re-issued world, or a map's or menu's 2D screen
// the layer took under experimental.on_foot_maps_sharp. The door cannot tell which, so one stub answers for both.
bool uiLayerDoorLayerOnly(uint32_t eye,uint64_t seq){return eye<2&&routeTookEye[eye]&&seq!=0&&routeTookSeq==seq;}
int uiLayerWorldDoorGap(uint64_t,uint32_t,ID3D11Texture2D* frame){++gapCalls;gapFrame=frame;return gapAnswer;}
void vScreenClearRenderTargetViewRaw(ID3D11DeviceContext* c,ID3D11RenderTargetView* v,const float colour[4]){++rawClears;c->ClearRenderTargetView(v,colour);}
}
struct Device {
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  Device(){auto create=edvr::systemD3D11CreateDevice();require(create!=nullptr,"system D3D11");
    D3D_FEATURE_LEVEL level{};require(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)),"WARP device");}
  ComPtr<ID3D11Texture2D> texture(unsigned w=320,unsigned h=240){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> t;
    require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t)),"texture");return t;}
};
EdvrNativeTemporalFrame frame(uint64_t generation,uint64_t sequence){
  EdvrNativeTemporalFrame f{sizeof(f),1};f.generation=generation;f.sequence=sequence;f.referenceGeneration=1;
  f.recommendedWidth=480;f.recommendedHeight=360;f.head[0]=f.head[5]=f.head[10]=1;
  for(unsigned e=0;e<2;++e){f.eyeToHead[e][0]=f.eyeToHead[e][5]=f.eyeToHead[e][10]=1;f.eyeToHead[e][3]=e?.032f:-.032f;
    f.frusta[e][0]=e?-.8f:-1.2f;f.frusta[e][1]=e?1.4f:.7f;f.frusta[e][2]=-.9f;f.frusta[e][3]=1.1f;}return f;
}
EdvrNativeTemporalTable acquire(Device& d,uint64_t generation,const char* mode="on"){
  edvr::Config::get().set("fix.temporal_aa",mode);EdvrNativeTemporalRequest r{sizeof(r),1,d.device.Get(),generation};EdvrNativeTemporalTable t{sizeof(t),1};
  require(edvrAcquireNativeTemporal(&r,&t)==S_OK,"acquire");return t;
}
EdvrNativeTemporalProjection begin(EdvrNativeTemporalTable& t,const EdvrNativeTemporalFrame& f,bool project=true){
  EdvrNativeTemporalProjection p{sizeof(p),1};HRESULT result=E_FAIL,a=S_OK,b=S_OK;
  std::thread cpu([&]{result=t.beginFrame(t.context,&f,&p);if(project){a=t.noteProjection(t.context,f.sequence,0,.025f,50000);b=t.noteProjection(t.context,f.sequence,1,.025f,50000);}});cpu.join();
  require(result==S_OK,"CPU owner begins frame");require(a==S_OK&&b==S_OK,"CPU projection query notes");return p;
}
HRESULT treat(EdvrNativeTemporalTable& t,uint64_t seq,unsigned eye,ID3D11Texture2D* source,const float* bounds=nullptr){
  ID3D11Texture2D* output=nullptr;float box[4]{};const auto result=t.treatEye(t.context,seq,eye,source,bounds,&output,box);
  if(result==S_OK){check(output!=nullptr,"successful output");check(box[0]==0&&box[1]==0&&box[2]==1&&box[3]==1,"full output bounds");}
  else check(!output&&box[0]==0&&box[1]==0&&box[2]==0&&box[3]==0,"passthrough/failure clears output");
  if(output)output->Release();return result;
}

// ---- The glue rig (review of 33ffc76e, finding F) ---------------------------------------------------------------------------
// The chain no other rig covers end to end, on the production pieces: the engine fix's consume classifier -> event ->
// the detector's camera-buffer Unmap tap (glitch_frame.cpp's glitchFrameObserve) -> the hold mark -> the two eye submits as
// native_frame.cpp's latchSubmit hands them to the temporal channel (marked -> skipEye with the verdict word read at that
// moment, otherwise treat) -> the frame boundary that owes the verdict -> the first resumed treat. Linked: glitch_frame.cpp,
// transition_flash_eye_base.cpp, frame_flag.cpp, native_temporal.cpp. Not linked: the game hooks (the consume is fed a
// synthetic mailbox through transitionFlashEyeBaseConsumeForTest) and native_frame.cpp's latch, which the step below mirrors.
float g_glueCbHead[1344], g_glueCbWorld[1344];   // 5376-byte scene CBs: row 275 head-only / a world-space eye
const float kGlueReset[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,0};
const float kGlueRefilled[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 5,0,0,1};
struct GlueRig {
  EdvrNativeTemporalTable* t=nullptr;ID3D11Texture2D* src=nullptr;uint64_t gen=0,seq=0;unsigned boundary=0;
  unsigned marks=0,skippedEyes=0,resets=0,treated=0;
};
// One frame period, in the order the flights showed: native begin (clears the mark), the consume, the render taps, the
// two submits, Present's boundary.
void glueStep(GlueRig& r,bool consumeReset,unsigned headFills,unsigned worldFills){
  ++r.seq;auto f=frame(r.gen,r.seq);begin(*r.t,f);edvr::clearGlitchFrame();
  edvr::transitionFlashEyeBaseConsumeForTest(1,kGlueReset);   // the mode-1 peek: carries no information
  edvr::transitionFlashEyeBaseConsumeForTest(2,consumeReset?kGlueReset:kGlueRefilled);
  for(unsigned i=0;i<worldFills;++i)edvr::glitchFrameObserve(g_glueCbWorld,5376,reinterpret_cast<const void*>(0x7001));
  for(unsigned i=0;i<headFills;++i)edvr::glitchFrameObserve(g_glueCbHead,5376,reinterpret_cast<const void*>(0x7002));
  const bool marked=edvr::glitchFrameMarked();const uint32_t verdict=edvr::jumpVerdictPacked();   // latchSubmit, once per frame
  if(marked)++r.marks;
  for(unsigned eye=0;eye<2;++eye){
    if(marked){check(r.t->skipEye(r.t->context,r.seq,eye,1,verdict)==S_OK,"glue: the held eye is skipped");++r.skippedEyes;}
    else{check(treat(*r.t,r.seq,eye,r.src)==S_OK,"glue: an unheld eye is treated");++r.treated;if(calls.back().flags&1)++r.resets;}
  }
  edvr::glitchFrameBoundary(0);
  edvr::transitionFlashEyeBaseFrameBoundary(++r.boundary);
}
void glueSettle(GlueRig& r){for(unsigned i=0;i<8;++i)glueStep(r,false,0,1);}
struct GlueNums{uint64_t events,held,noHead,notHonoured,skipped,kept,returned,unjudged;};
GlueNums glueNums(){
  GlueNums n{};edvr::transitionFlashEyeBaseCountersForTest(&n.events,&n.held,&n.noHead,&n.notHonoured);
  edvr::nativeTemporalOmissionCounters(&n.skipped,&n.kept,&n.returned,&n.unjudged);return n;
}
void glue(EdvrNativeTemporalTable& t,ID3D11Texture2D* source,uint64_t gen,uint64_t seq0){
  std::memset(g_glueCbHead,0,sizeof(g_glueCbHead));std::memset(g_glueCbWorld,0,sizeof(g_glueCbWorld));
  g_glueCbHead[1100]=0.05f;g_glueCbHead[1101]=-0.016f;g_glueCbHead[1102]=0.016f;      // the bad frame's row 275 (flight 060955)
  g_glueCbWorld[1100]=6.119f;g_glueCbWorld[1101]=-3.432f;g_glueCbWorld[1102]=11.511f; // an ordinary eye origin
  edvr::Config::get().set("fix.transition_flash","1");
  edvr::installGlitchFrameFix();
  char why[256]="";
  check(edvr::glitchFrameEngineTapAvailable(why,sizeof(why)),"glue: the tap is available with the default buffer");
  edvr::transitionFlashEyeBaseArmForTest();
  edvr::announceGlitchConsumer();
  GlueRig r;r.t=&t;r.src=source;r.gen=gen;r.seq=seq0;   // the channel the omission cases above used (the pool of 16 is spent)
  glueSettle(r);
  const unsigned baseResets=r.resets;

  { // a one-frame gap
    const GlueNums a=glueNums();const unsigned m0=r.marks;
    glueStep(r,true,0,1);        // S: the consume reads the reset mailbox -> an event
    glueStep(r,false,3,2);       // S+1: the bad render; three duplicate head-only fills among world fills
    glueStep(r,false,0,1);       // S+2: resumed
    glueSettle(r);
    const GlueNums b=glueNums();
    check(b.events==a.events+1,"glue: one-frame gap: one event");
    check(r.marks==m0+1&&b.held==a.held+1,"glue: one-frame gap: exactly one mark, despite three head-only fills");
    check(b.skipped==a.skipped+2&&b.kept==a.kept+2&&b.unjudged==a.unjudged,"glue: one-frame gap: both eyes skipped once, both histories kept, no unjudged reset");
    check(r.resets==baseResets,"glue: one-frame gap: the first resumed treat does not reset history");
    check(b.notHonoured==a.notHonoured,"glue: one-frame gap: the compositor honoured the hold");
  }
  { // a two-frame gap
    const GlueNums a=glueNums();const unsigned m0=r.marks;
    glueStep(r,true,0,1);        // S
    glueStep(r,true,2,1);        // S+1: still a gap consume; the first bad render
    glueStep(r,false,2,1);       // S+2: the second bad render (its consume S+1 was a gap consume)
    glueStep(r,false,0,1);       // S+3: resumed
    glueSettle(r);
    const GlueNums b=glueNums();
    check(r.marks==m0+2&&b.held==a.held+2,"glue: two-frame gap: two frames held, no more");
    check(b.skipped==a.skipped+4&&b.kept==a.kept+2&&b.unjudged==a.unjudged,"glue: two-frame gap: four eyes skipped, one kept run per eye, no unjudged reset");
    check(r.resets==baseResets,"glue: two-frame gap: history survives");
  }
  { // a gap longer than two frames holds two frames and no more
    const GlueNums a=glueNums();const unsigned m0=r.marks;
    glueStep(r,true,0,1);glueStep(r,true,2,1);glueStep(r,true,2,1);glueStep(r,true,2,1);glueStep(r,false,2,1);
    glueSettle(r);
    const GlueNums b=glueNums();
    check(r.marks==m0+2&&b.held==a.held+2,"glue: a long gap holds two frames, never more");
    check(r.resets==baseResets&&b.unjudged==a.unjudged,"glue: a long gap leaves the history intact");
  }
  { // a non-head-only fill, and an event with no head-only fill at all
    const GlueNums a=glueNums();const unsigned m0=r.marks;
    glueStep(r,true,0,1);glueStep(r,false,0,3);glueStep(r,false,0,1);glueSettle(r);
    const GlueNums b=glueNums();
    check(r.marks==m0&&b.held==a.held,"glue: world-space fills alone are never held");
    check(b.events==a.events+1&&b.noHead==a.noHead+1,"glue: the event whose window held no head-only fill is counted as such");
    check(b.skipped==a.skipped,"glue: nothing was skipped");
  }
  { // overlapping events: a second ENTRY two frames after the first replaces it
    const GlueNums a=glueNums();const unsigned m0=r.marks;
    glueStep(r,true,0,1);glueStep(r,false,2,1);   // event A: S, S+1 (held)
    glueStep(r,true,0,1);glueStep(r,false,2,1);   // event B replaces A at S+2; its bad render S+3 (held)
    glueStep(r,false,0,1);glueSettle(r);
    const GlueNums b=glueNums();
    check(b.events==a.events+2&&r.marks==m0+2&&b.held==a.held+2,"glue: overlapping events: two events, one hold each");
    check(b.skipped==a.skipped+4&&b.kept==a.kept+4&&b.unjudged==a.unjudged&&r.resets==baseResets,"glue: overlapping events: histories kept across both holds");
  }
  { // the window closes: fills after retirement are not delivered to the tap
    const GlueNums a=glueNums();const unsigned m0=r.marks;
    glueSettle(r);glueStep(r,false,4,0);glueStep(r,false,4,0);
    const GlueNums b=glueNums();
    check(r.marks==m0&&b.held==a.held,"glue: head-only fills outside any event are ignored");
  }
  { // failed activation: the engine fix is not armed, so nothing is held and nothing is counted
    edvr::transitionFlashEyeBaseDisarmForTest();
    const GlueNums a=glueNums();const unsigned m0=r.marks;
    glueStep(r,true,0,1);glueStep(r,false,3,1);glueStep(r,false,0,1);glueSettle(r);
    const GlueNums b=glueNums();
    check(r.marks==m0&&b.held==a.held&&b.events==a.events,"glue: an unarmed engine fix arms no event and marks no frame");
    check(b.skipped==a.skipped&&r.resets==baseResets,"glue: an unarmed engine fix leaves the temporal pass alone");
    edvr::transitionFlashEyeBaseArmForTest();
  }
  edvr::shutdownGlitchFrameFix();
}

void run(){
  // N9: the warm target is the channel's raw pointer, not a live reference --
  // false with no channel acquired, the acquiring thread and device once one
  // is, false again once it closes.
  ID3D11Device* warmDev=reinterpret_cast<ID3D11Device*>(1);unsigned long warmThread=1;
  check(!edvr::nativeTemporalWarmTarget(&warmDev,&warmThread),"no warm target before any channel is acquired");
  Device d;auto source=d.texture();auto t=acquire(d,11);auto f=frame(11,1);auto p=begin(t,f);
  warmDev=nullptr;warmThread=0;
  check(edvr::nativeTemporalWarmTarget(&warmDev,&warmThread)&&warmDev==d.device.Get()&&warmThread==GetCurrentThreadId(),
      "warm target names the acquiring thread and device");
  check(p.tangentShift[0][0]==0&&p.tangentShift[1][1]==0,"unknown input size means no jitter");
  check(treat(t,1,1,source.Get())==S_OK&&treat(t,1,0,source.Get())==S_OK,"reversed first pair");
  check(reentries>=2&&reentryThrows==0,"asked from inside treat (as the create hook asks), nothing re-locks the channel's mutex");
  check(reentryRecW==480&&reentryRecH==360,"...and the recommendation is the frame's, read without the lock");
  check(closeFloat(reentryUp,.9f)&&closeFloat(reentryDown,1.1f),"...and so is its vertical frustum (the panel rule's field of view)");
  check(!reentryJitter&&!reentryWarm&&!reentryGeometry,"...and the readers that take the lock (the eye geometry the VR camera census reads included) answer no there instead of throwing");
  {uint32_t rw=0,rh=0;check(edvr::nativeTemporalRecommended(&rw,&rh)&&rw==480&&rh==360,"the recommendation outside treat");}
  {uint32_t aw=1,ah=1;check(!edvr::nativeTemporalAsked(&aw,&ah),"no ask when the host does not say (the recommendation is the answer)");}
  check(calls.back().flags&1,"first history reset");check(!calls.back().head,"first frame has no invented head pair");
  {float tu=1,td=1;check(!edvr::nativeTemporalTrueVerticalTangents(&tu,&td),"no true frustum when the host does not say");}
  f=frame(11,2);f.head[3]=.1f;f.askedWidth=384;f.askedHeight=441;f.trueUp=1.2648f;f.trueDown=-1.2648f;p=begin(t,f);t.noteProjection(t.context,2,0,.1f,1000);
  {uint32_t aw=0,ah=0,rw=0,rh=0;
   check(edvr::nativeTemporalAsked(&aw,&ah)&&aw==384&&ah==441&&edvr::nativeTemporalRecommended(&rw,&rh)&&rw==480&&rh==360,
         "an adoption's ask is published beside the frame's own recommendation, which it leads (review P3-1)");
   float tu=0,td=0,gu=0,gd=0;
   check(edvr::nativeTemporalTrueVerticalTangents(&tu,&td)&&closeFloat(tu,1.2648f)&&closeFloat(td,1.2648f)&&
         edvr::nativeTemporalVerticalTangents(&gu,&gd)&&closeFloat(gu,.9f)&&closeFloat(gd,1.1f),
         "the headset's true frustum is published beside the one the game is told (the panel sizing's k_out)");}
  check(treat(t,2,0,source.Get())==S_OK,"second left");auto c=calls.back();
  check(closeFloat(c.jx,-p.tangentShift[0][0]*320/1.9f)&&closeFloat(c.jy,p.tangentShift[0][1]*240/2.f),"consumer jitter uses pixels and correct signs");
  // fix.ui_quality reads the same jitter at DRAW time (ui_layer.h): the
  // frame's sequence, the pixel jitter the pass itself receives here (as_is,
  // no lag), and the region it was computed over.
  {uint64_t ds=0;float djx=9,djy=9;uint32_t dw=0,dh=0;
   check(edvr::nativeTemporalDrawJitter(0,&ds,&djx,&djy,&dw,&dh)&&ds==2&&closeFloat(djx,c.jx)&&closeFloat(djy,c.jy)&&dw==320&&dh==240,
         "draw-time jitter is the pixel jitter the pass receives, for the frame being drawn");
   check(!edvr::nativeTemporalDrawJitter(2,&ds,&djx,&djy,&dw,&dh),"draw-time jitter refuses an eye that does not exist");}
  // The VR camera census reads what EDVR advertised for an eye at its composite draw (vr_camera_census.h): the frame's
  // sequence, the frustum the host gave (frame()'s, {left, right, down, up}) and the tangent shift the jitter moved it by.
  {uint64_t es=0;float fr[4]{},sh[2]{};
   check(edvr::nativeTemporalEyeGeometry(0,&es,fr,sh)&&es==2&&closeFloat(fr[0],-1.2f)&&closeFloat(fr[1],.7f)&&closeFloat(fr[2],-.9f)&&
         closeFloat(fr[3],1.1f)&&closeFloat(sh[0],p.tangentShift[0][0])&&closeFloat(sh[1],p.tangentShift[0][1])&&
         (std::fabs(sh[0])>0||std::fabs(sh[1])>0),
         "the eye geometry is the frustum the host was given and the tangent shift the frame carried, for eye 0 of the frame being drawn");
   check(edvr::nativeTemporalEyeGeometry(1,&es,fr,sh)&&closeFloat(fr[0],-.8f)&&closeFloat(fr[1],1.4f)&&closeFloat(sh[0],p.tangentShift[1][0])&&closeFloat(sh[1],p.tangentShift[1][1]),
         "...and eye 1's own");
   check(!edvr::nativeTemporalEyeGeometry(2,&es,fr,sh),"the eye geometry refuses an eye that does not exist");
   check(edvr::nativeTemporalEyeGeometry(0,nullptr,nullptr,nullptr),"a caller may ask for any subset of the answer");}
  check(uiLayerNotes>=3&&uiLayerNoteSeq==2&&uiLayerNoteEye==0&&uiLayerNoteOut==source.Get(),"each treated eye is noted to the UI layer with the pass's output");
  check(uiLayerSubmits>=3&&uiLayerSubmitted[0]==source.Get()&&uiLayerSubmitted[1]==source.Get(),"the game's submitted texture is noted per eye for the UI layer's eye check");
  check(std::fabs(c.jx)>.01f||std::fabs(c.jy)>.01f,"known-size jitter engaged");
  check(c.head&&closeFloat(c.offset[0],-.032f)&&closeFloat(c.currentHead[3],.1f)&&closeFloat(c.previousHead[3],0),"world path receives exact head pair and eye offset");
  check(c.motion&&closeFloat(c.translation[0],.1f)&&closeFloat(c.swapped[0],.1f)&&closeFloat(c.delta[0],1),"pure translation expected reprojection");
  check(closeFloat(c.nearZ,.025f)&&closeFloat(c.farZ,50000),"smallest near retains its paired far");check(!(c.flags&1),"continuous history");
  check(treat(t,2,1,source.Get())==S_OK,"second right");check(closeFloat(calls.back().jx,c.jx)&&closeFloat(calls.back().jy,c.jy),"asymmetric eyes share pixel phase");
  check(treat(t,2,0,source.Get())==E_INVALIDARG,"duplicate eye rejected");check(t.beginFrame(t.context,&f,&p)==E_INVALIDARG,"duplicate begin rejected");
  f=frame(11,4);begin(t,f);check(treat(t,4,0,source.Get())==S_OK&&(calls.back().flags&1),"sequence gap reset");
  f=frame(11,5);f.referenceGeneration=2;begin(t,f);check(treat(t,5,0,source.Get())==S_OK&&(calls.back().flags&1),"reference change reset");
  check(t.invalidate(t.context)==S_OK,"CPU invalidation");check(treat(t,5,0,source.Get())==E_INVALIDARG,"invalidated frame cannot treat");check(t.beginFrame(t.context,&f,&p)==E_INVALIDARG,"invalidated sequence cannot reopen");
  {uint32_t rw=0,rh=0;float u=0,dn=0;check(!edvr::nativeTemporalRecommended(&rw,&rh)&&!edvr::nativeTemporalVerticalTangents(&u,&dn)&&!edvr::nativeTemporalAsked(&rw,&rh)&&!edvr::nativeTemporalTrueVerticalTangents(&u,&dn),"no recommendation, frustum, ask or true frustum once the channel is invalidated");}
  {uint64_t es=0;float fr[4]{},sh[2]{};check(!edvr::nativeTemporalEyeGeometry(0,&es,fr,sh),"no eye geometry once the channel is invalidated");}
  f=frame(11,6);p=begin(t,f);auto larger=d.texture(640,480);check(treat(t,6,0,larger.Get())==S_OK,"resize treatment");c=calls.back();
  check(c.flags&1,"resize reset");check(closeFloat(c.jx,-p.tangentShift[0][0]*640/1.9f),"resize converts jitter to actual pixels");
  Device other;auto foreign=other.texture();check(treat(t,6,1,foreign.Get())==E_INVALIDARG,"wrong device rejected");
  const float invalid[]={-.1f,0,1,1};check(treat(t,6,1,source.Get(),invalid)==E_INVALIDARG,"invalid bounds rejected");
  HRESULT wrong=E_FAIL;std::thread worker([&]{ID3D11Texture2D* out=nullptr;float box[4]{};wrong=t.treatEye(t.context,6,1,source.Get(),nullptr,&out,box);});worker.join();check(wrong==E_INVALIDARG,"wrong GPU thread rejected");
  check(treat(t,6,1,source.Get())==S_OK,"valid retry after rejected input");
  f=frame(11,7);begin(t,f);const float flipped[]={1,0,0,1};check(treat(t,7,0,source.Get(),flipped)==S_FALSE,"flipped bounds passthrough");
  f=frame(11,8);p=begin(t,f);check(p.tangentShift[0][0]==0&&p.tangentShift[0][1]==0,"flipped eye suppresses jitter");
  check(treat(t,8,0,source.Get())==S_OK&&(calls.back().flags&1),"unflipped image resets history");
  f=frame(11,9);begin(t,f,false);check(treat(t,9,0,source.Get())==E_PENDING,"no projection query cannot justify jittered image metadata");
  check(t.close(t.context)==S_OK&&t.close(t.context)==S_FALSE,"idempotent CPU close");
  warmDev=reinterpret_cast<ID3D11Device*>(1);warmThread=1;
  check(!edvr::nativeTemporalWarmTarget(&warmDev,&warmThread),"no warm target once the channel closes");
  {uint32_t rw=0,rh=0;float u=0,dn=0;check(!edvr::nativeTemporalRecommended(&rw,&rh)&&!edvr::nativeTemporalVerticalTangents(&u,&dn)&&!edvr::nativeTemporalAsked(&rw,&rh)&&!edvr::nativeTemporalTrueVerticalTangents(&u,&dn),"no recommendation, frustum, ask or true frustum once the channel closes");}
  auto fresh=acquire(d,12);f=frame(12,1);begin(fresh,f);check(treat(t,1,0,source.Get())==E_INVALIDARG,"stale table cannot address new generation");
  // Previous eye yaw 90 degrees, current yaw zero, head translates +X:
  // the translation is +Z in the previous eye and head rotation is zero.
  f.eyeToHead[0][0]=0;f.eyeToHead[0][2]=1;f.eyeToHead[0][8]=-1;f.eyeToHead[0][10]=0;f.sequence=2;
  begin(fresh,f);check(treat(fresh,2,0,source.Get())==S_OK,"canted baseline");
  f=frame(12,3);f.head[3]=1;begin(fresh,f);check(treat(fresh,3,0,source.Get())==S_OK,"changed eye transform");c=calls.back();
  check(closeFloat(c.delta[0],0)&&closeFloat(c.delta[2],-1)&&closeFloat(c.delta[6],1)&&closeFloat(c.delta[8],0),"changing cant composes both eye rotations");
  check(closeFloat(c.translation[0],0)&&closeFloat(c.translation[2],1)&&closeFloat(c.headDeg,0),"translation in previous canted eye frame");
  edvr::Config::get().set("fix.temporal_aa","off");check(treat(fresh,3,1,source.Get())==S_OK,"current pair freezes AA setting");
  f=frame(12,4);p=begin(fresh,f);const auto before=dumps;check(treat(fresh,4,0,source.Get())==S_FALSE&&dumps==before+1,"next frame AA off captures untreated eyes");check(p.tangentShift[0][0]==0,"AA off zeros jitter");check(fresh.close(fresh.context)==S_OK,"fresh close");
  auto dlss=acquire(d,13,"dlss");f=frame(13,1);begin(dlss,f);check(treat(dlss,1,0,source.Get())==S_OK,"DLSS call");c=calls.back();check(c.outW==480&&c.outH==360&&(c.flags&2),"DLSS requests recommended size");
  f=frame(13,2);begin(dlss,f);passSucceeds=false;const unsigned notesBefore=uiLayerNotes;check(treat(dlss,2,0,source.Get())==S_FALSE,"filter refusal preserves raw pixels");passSucceeds=true;
  check(uiLayerNotes==notesBefore,"a refused eye is not noted to the UI layer (it must not arm behind raw pixels)");
  f=frame(13,3);p=begin(dlss,f);check(p.tangentShift[0][0]==0&&p.tangentShift[0][1]==0,"refusal disables future jitter");check(dlss.close(dlss.context)==S_OK,"DLSS close");
  auto unknown=acquire(d,14,"invalid-mode");f=frame(14,1);begin(unknown,f);check(treat(unknown,1,0,source.Get())==S_FALSE,"unknown mode is off");check(unknown.close(unknown.context)==S_OK,"unknown close");
  edvr::Config::get().set("advanced.temporal_aa_jitter_sign","flip_both");
  edvr::Config::get().set("advanced.temporal_aa_jitter_lag","1");
  auto diagnostic=acquire(d,16);f=frame(16,1);begin(diagnostic,f);
  check(treat(diagnostic,1,1,source.Get())==S_OK&&treat(diagnostic,1,0,source.Get())==S_OK,"diagnostic baseline pair");
  f=frame(16,2);const auto p2=begin(diagnostic,f);
  check(treat(diagnostic,2,1,source.Get())==S_OK&&closeFloat(calls.back().jx,0)&&closeFloat(calls.back().jy,0),"lag consumes previous frame zero jitter");
  check(treat(diagnostic,2,0,source.Get())==S_OK&&closeFloat(calls.back().jx,0),"lag is per eye, independent of submit order");
  f=frame(16,3);begin(diagnostic,f);check(treat(diagnostic,3,0,source.Get())==S_OK,"lag third frame");
  c=calls.back();check(closeFloat(c.jx,p2.tangentShift[0][0]*320/1.9f)&&closeFloat(c.jy,-p2.tangentShift[0][1]*240/2),"sign and lag affect consumer pixels only");
  check(p2.tangentShift[0][0]<0,"game jitter keeps original sign despite diagnostic flip");
  check(diagnostic.close(diagnostic.context)==S_OK,"diagnostic close");
  edvr::Config::get().set("advanced.temporal_aa_jitter_sign","as_is");edvr::Config::get().set("advanced.temporal_aa_jitter_lag","0");
  edvr::Config::get().set("fix.temporal_aa","on");edvr::openxr::NativeTemporalClient client;
  require(client.acquire(GetModuleHandleW(nullptr),d.device.Get(),15)==S_OK,"actual client validates provider table");
  edvr::openxr::GeometryInput g{};g.generation=1;g.sequence=1;g.displayTime=1;g.headPose.orientation.w=1;
  g.headFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;g.viewFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  for(unsigned e=0;e<2;++e){g.views[e].pose.orientation.w=1;g.views[e].pose.position.x=e?.03f:-.03f;g.views[e].fov={-.8f,.7f,.7f,-.6f};g.width[e]=480;g.height[e]=360;}
  float shifts[2][2]{};check(client.begin(g,1,shifts)==S_OK,"client converts located geometry");client.noteProjection(1,0,.025f,50000);
  ComPtr<ID3D11Texture2D> out;vr::VRTextureBounds_t box{};check(client.treat(1,0,source.Get(),nullptr,out,box)==S_OK&&out.Get()==source.Get(),"client retains provider result");check(client.close()==S_OK&&client.close()==S_FALSE,"client close");
  auto omitted=acquire(d,17);f=frame(17,1);begin(omitted,f);
  check(treat(omitted,1,0,source.Get())==S_OK,"omission baseline");
  f=frame(17,2);begin(omitted,f);const auto count=calls.size();
  const unsigned resetsBefore=worldRouteSceneResets;
  check(omitted.skipEye(omitted.context,2,0,1,edvr::jumpVerdictPacked())==S_OK&&calls.size()==count,"withheld pixels never enter history");
  check(worldRouteSceneResets==resetsBefore+1,"a withheld eye tells the VR world route once (a scene reset: boarding, disembarking, a jump, a glitch frame)");
  check(omitted.skipEye(omitted.context,2,0,1,0)==E_INVALIDARG&&treat(omitted,2,0,source.Get())==E_INVALIDARG,"omitted eye cannot be consumed twice");
  check(worldRouteSceneResets==resetsBefore+1,"a refused skipEye is not a scene reset");
  edvr::noteJumpVerdict(2);f=frame(17,3);f.head[3]=.2f;begin(omitted,f);
  check(treat(omitted,3,0,source.Get())==S_OK&&!(calls.back().flags&1)&&closeFloat(calls.back().translation[0],.2f),"stayed verdict preserves real previous pose across omitted frame");
  f=frame(17,4);begin(omitted,f);omitted.skipEye(omitted.context,4,0,1,edvr::jumpVerdictPacked());
  edvr::noteJumpVerdict(1);f=frame(17,5);begin(omitted,f);
  check(treat(omitted,5,0,source.Get())==S_OK&&(calls.back().flags&1),"returned verdict resets history");
  f=frame(17,6);begin(omitted,f);omitted.skipEye(omitted.context,6,0,0,0);
  f=frame(17,7);begin(omitted,f);check(treat(omitted,7,0,source.Get())==S_OK&&(calls.back().flags&1),"healed/explicit hold resets history");
  f=frame(17,8);begin(omitted,f);omitted.skipEye(omitted.context,8,0,1,edvr::jumpVerdictPacked());
  for(unsigned seq=9;seq<=12;++seq) {
    f=frame(17,seq);begin(omitted,f);check(treat(omitted,seq,0,source.Get())==S_OK,"deferred verdict frame");
    check(bool(calls.back().flags&1)==(seq==12),"unknown verdict resets on fourth treat only");
  }
  // The engine fix's exact withhold (docs Build 2c, flight 091951). The verdict word is latched when the eye is withheld
  // (native_frame passes jumpVerdictPacked() to skipEye) and a CHANGE since is the verdict. Published BEFORE the latch it is
  // no change: the fourth treat resets the history (the unjudged blink). Published AFTER, at the frame boundary behind the
  // withheld frame's submit, it is a change and the history is kept -- through a second withheld frame too.
  uint64_t cSkipped0=0,cKept0=0,cReturned0=0,cUnjudged0=0;
  check(edvr::nativeTemporalOmissionCounters(&cSkipped0,&cKept0,&cReturned0,&cUnjudged0),"omission counters are readable");
  f=frame(17,13);begin(omitted,f);edvr::noteJumpVerdict(2);
  omitted.skipEye(omitted.context,13,0,1,edvr::jumpVerdictPacked());
  for(unsigned seq=14;seq<=17;++seq) {
    f=frame(17,seq);begin(omitted,f);check(treat(omitted,seq,0,source.Get())==S_OK,"verdict published before the latch: treat");
    check(bool(calls.back().flags&1)==(seq==17),"verdict published before the latch is no verdict: history resets (the blink)");
  }
  uint64_t cSkipped1=0,cKept1=0,cReturned1=0,cUnjudged1=0;
  edvr::nativeTemporalOmissionCounters(&cSkipped1,&cKept1,&cReturned1,&cUnjudged1);
  check(cSkipped1==cSkipped0+1&&cKept1==cKept0&&cUnjudged1==cUnjudged0+1&&cReturned1==cReturned0,
        "verdict before the latch: one omitted eye, no history kept, one unjudged reset");
  f=frame(17,18);begin(omitted,f);
  omitted.skipEye(omitted.context,18,0,1,edvr::jumpVerdictPacked());   // frame 1 held: latch the word as it stands
  edvr::noteJumpVerdict(2);                                           // the boundary behind it
  f=frame(17,19);begin(omitted,f);
  omitted.skipEye(omitted.context,19,0,1,edvr::jumpVerdictPacked());   // frame 2 held (the gap lasted two frames)
  edvr::noteJumpVerdict(2);
  f=frame(17,20);begin(omitted,f);
  check(treat(omitted,20,0,source.Get())==S_OK&&!(calls.back().flags&1),"verdict published after the latch keeps history across two held frames");
  // The counters' contract (review of 33ffc76e): skipped counts one per omitted eye per omitted frame, history_kept
  // once per resolved omission RUN per eye. Two consecutive holds of one eye = skipped +2, history_kept +1, no unjudged
  // reset. A flight is judged on unjudged_resets == 0 and on runs per eye, never on held frames == history_kept.
  uint64_t cSkipped2=0,cKept2=0,cReturned2=0,cUnjudged2=0;
  edvr::nativeTemporalOmissionCounters(&cSkipped2,&cKept2,&cReturned2,&cUnjudged2);
  check(cSkipped2==cSkipped1+2&&cKept2==cKept1+1&&cUnjudged2==cUnjudged1&&cReturned2==cReturned1,
        "two consecutive holds of one eye: skipped +2, history_kept +1 (one run), unjudged_resets +0");
  glue(omitted,source.Get(),17,20);
  omitted.close(omitted.context);

  // ---- the served floor (2026-09-23) ----------------------------------------
  // dlss at a 4074x4076 door (the Pimax at 90.4% of its runtime's size) and a
  // 3070x3032 one (the FOV-trim session's untrimmed size), the stub answering
  // as the flights' modes lines read. Where some mode serves the input the
  // pass is asked for the door's output; where none does, for twice the
  // input, NGX's own answer at that output agreeing -- never a refusal.
  {
    auto floorT=acquire(d,18,"dlss");uint64_t seq=0;
    const auto ask=[&](EdvrNativeTemporalTable& tab,uint64_t gen,uint32_t doorW,uint32_t doorH,uint32_t w,uint32_t h,
                       uint32_t wantW,uint32_t wantH,const char* why){
      auto fr=frame(gen,++seq);fr.recommendedWidth=doorW;fr.recommendedHeight=doorH;begin(tab,fr);
      auto src=d.texture(w,h);const bool ok=treat(tab,seq,0,src.Get())==S_OK&&!calls.empty();
      const bool right=ok&&calls.back().outW==wantW&&calls.back().outH==wantH;check(right,why);
      if(!right&&ok)std::printf("  the pass was asked for %ux%u, not %ux%u\n",calls.back().outW,calls.back().outH,wantW,wantH);
    };
    ask(floorT,18,4074,4076,2037,2038,4074,4076,"HMD Quality 0.5 at 4074x4076 sits exactly on the floor: the door's output stands");
    ask(floorT,18,4074,4076,2036,2037,4072,4074,"a pixel under the floor: twice the input (4072x4074), not the pass's own history");
    ask(floorT,18,4074,4076,1833,1834,3666,3668,"HMD Quality 0.45: 3666x3668, performance at 50%");
    {const auto q=rangeQueries;
     ask(floorT,18,4074,4076,1833,1834,3666,3668,"...and the next frame the same");
     check(rangeQueries==q,"an unchanged door and input ask NGX nothing (decided once, not per frame)");}
    ask(floorT,18,4074,4076,1358,1359,4074,4076,"exactly ultra performance's point (1358x1359): served as it stands");
    ask(floorT,18,4074,4076,1358,1358,2716,2716,"a pixel off that point: twice the input");
    ask(floorT,18,4074,4076,1300,1300,2600,2600,"under a third: twice the input");
    ask(floorT,18,4074,4076,2037,2038,4074,4076,"the input rises back to the floor: the cut is undone");
    ask(floorT,18,3070,3032,1535,1516,3070,3032,"3070x3032 at HMD Quality 0.5: the floor exactly");
    ask(floorT,18,3070,3032,1534,1515,3068,3030,"a pixel under it: 3068x3030");
    ask(floorT,18,3070,3032,1229,1412,2458,2824,"the trim's transient, 1229x1412 against the still-untrimmed door: 2458x2824");
    ask(floorT,18,2458,2824,1229,1412,2458,2824,"...which is the trimmed door the promotion hands next: one output, one feature");
    ask(floorT,18,4075,4077,2036,2037,4070,4072,"an odd door is cut to an even output");
    ask(floorT,18,4075,4077,2040,2037,4075,4072,"an axis the input serves keeps the door's size");
    // NGX's answer at the cut has the last word: a runtime whose floor sits
    // three pixels past half under 4000 is stepped down until it serves.
    stingy=true;{const auto q=rangeQueries;
     ask(floorT,18,4074,4076,1831,1832,3656,3658,"a stingier floor at the cut: stepped down two pixels at a time until NGX serves it");
     check(rangeQueries==q+5,"...the door's ranges, then four at the cut (three steps down)");}
    stingy=false;
    onlyW=4074;ask(floorT,18,4074,4076,1829,1830,4074,4076,"NGX silent at the cut: the door's output stands, the pass decides as before");onlyW=0;
    ngxAnswers=false;ask(floorT,18,4000,4000,1600,1600,4000,4000,"NGX will not say at all: the door's output stands, as before this existed");ngxAnswers=true;
    check(floorT.close(floorT.context)==S_OK,"floor channel close");
    // Only NVIDIA's ranges are the floor: fsr is asked for the door's output.
    auto fsr=acquire(d,19,"fsr");seq=0;const auto q=rangeQueries;
    ask(fsr,19,4074,4076,1833,1834,4074,4076,"fsr keeps the door's output (AMD's ranges are not NVIDIA's)");
    check(rangeQueries==q,"...and asks NGX nothing");
    check(fsr.close(fsr.context)==S_OK,"fsr channel close");
  }

  // ---- the VR world route: the eye shift and the layer-only door (design doc section 82) ----------------------------
  // With experimental.temporal_aa_on_foot_world off the route owns nothing and took no eye, which is every case above.
  // Here it owns the next frame, or took an eye's screen draw into the UI layer, and the door answers for it.
  {
    const auto shifted=[](const EdvrNativeTemporalProjection& p){
      return p.tangentShift[0][0]!=0||p.tangentShift[0][1]!=0||p.tangentShift[1][0]!=0||p.tangentShift[1][1]!=0;};
    const auto unshifted=[&](uint64_t gen,const char* mode,const char* why){
      auto t=acquire(d,gen,mode);auto fr=frame(gen,1);auto p0=begin(t,fr);(void)p0;
      treat(t,1,0,source.Get());treat(t,1,1,source.Get());fr=frame(gen,2);const auto p=begin(t,fr);
      check(!shifted(p),why);check(t.close(t.context)==S_OK,"close");};
    // 1. The eye shift: advertised every frame, except the frame after a boundary that left the route owning the world.
    {
      auto st=acquire(d,20);routeOwns=false;uint64_t q=1;
      auto fr=frame(20,q);begin(st,fr);check(treat(st,q,0,source.Get())==S_OK&&treat(st,q,1,source.Get())==S_OK,"shift baseline pair");
      fr=frame(20,++q);auto sp=begin(st,fr);
      check(shifted(sp),"the route owns nothing: the eye shift is advertised, exactly as it always was");
      treat(st,q,0,source.Get());treat(st,q,1,source.Get());
      routeOwns=true;fr=frame(20,++q);sp=begin(st,fr);
      check(!shifted(sp)&&sp.tangentShift[0][0]==0&&sp.tangentShift[0][1]==0&&sp.tangentShift[1][0]==0&&sp.tangentShift[1][1]==0,
            "the route owns the next frame: no eye shift for either eye");
      {uint64_t sq=0;float jx=9,jy=9;uint32_t w=0,h=0;
       check(edvr::nativeTemporalDrawJitter(0,&sq,&jx,&jy,&w,&h)&&sq==q&&jx==0&&jy==0&&edvr::nativeTemporalDrawJitter(1,&sq,&jx,&jy,&w,&h)&&jx==0&&jy==0,
             "...so the layer's jitter cancel follows to zero by construction (the draw-time jitter is zero for both eyes)");}
      const auto callsAt=calls.size();
      check(treat(st,q,0,source.Get())==S_OK&&calls.size()==callsAt+1&&calls.back().jx==0&&calls.back().jy==0,
            "...and the eye route, serving an eye the route did not take on such a frame, treats it with no shift");
      treat(st,q,1,source.Get());
      routeOwns=false;fr=frame(20,++q);sp=begin(st,fr);
      check(shifted(sp),"the route released the world: the eye shift is back, the very next frame");
      routeOwns=true;fr=frame(20,++q);sp=begin(st,fr);check(!shifted(sp),"...and it goes off again the moment the route owns the next frame");
      routeOwns=false;check(st.close(st.context)==S_OK,"shift channel close");
    }
    routeOwns=true;
    unshifted(24,"off","fix.temporal_aa off: no shift whether or not the route owns the world");
    routeOwns=false;
    unshifted(25,"off","fix.temporal_aa off: no shift (the route owns nothing)");

    // 2. The layer-only door.
    const auto outOf=[&](EdvrNativeTemporalTable& t,uint64_t seq,unsigned eye,ID3D11Texture2D* src){
      struct R{HRESULT hr=E_FAIL;ComPtr<ID3D11Texture2D> tex;float box[4]{};};R r;ID3D11Texture2D* raw=nullptr;
      r.hr=t.treatEye(t.context,seq,eye,src,nullptr,&raw,r.box);r.tex.Attach(raw);return r;};
    const auto descOf=[](ID3D11Texture2D* tex){D3D11_TEXTURE2D_DESC dd{};tex->GetDesc(&dd);return dd;};
    const auto pixel=[&](ID3D11Texture2D* tex,unsigned x,unsigned y){
      D3D11_TEXTURE2D_DESC dd=descOf(tex);if(x>=dd.Width||y>=dd.Height)return 0xDEADBEEFu;   // a pixel that is not there reads as a failure, not a fault
      dd.Usage=D3D11_USAGE_STAGING;dd.BindFlags=0;dd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Texture2D> st;require(SUCCEEDED(d.device->CreateTexture2D(&dd,nullptr,&st)),"staging");
      d.context->CopyResource(st.Get(),tex);D3D11_MAPPED_SUBRESOURCE m{};require(SUCCEEDED(d.context->Map(st.Get(),0,D3D11_MAP_READ,0,&m)),"map");
      const auto* p=static_cast<const unsigned char*>(m.pData)+y*m.RowPitch+x*4;unsigned v=p[0]|(p[1]<<8)|(p[2]<<16)|(unsigned(p[3])<<24);
      d.context->Unmap(st.Get(),0);return v;};
    const auto refs=[](ID3D11Texture2D* t){const auto n=t->AddRef();t->Release();return n-1;};
    uint64_t q=0;routeOwns=true;
    auto lo=acquire(d,21,"dlss");   // 320x240 into the door's 480x360
    ComPtr<ID3D11Texture2D> blank;
    {
      // frame 1: the eye route -- history is established, nothing is taken
      auto fr=frame(21,++q);begin(lo,fr);
      routeTookEye[0]=routeTookEye[1]=false;routeTookSeq=0;
      check(treat(lo,q,0,source.Get())==S_OK&&treat(lo,q,1,source.Get())==S_OK,"layer-only setup: eye route frame");
      const auto contFlagsAfter=calls.back().flags;(void)contFlagsAfter;
      // frame 2: the route took both eyes' screen draws
      fr=frame(21,++q);begin(lo,fr);routeTookEye[0]=routeTookEye[1]=true;routeTookSeq=q;gapAnswer=0;gapCalls=0;rawClears=0;
      const auto callsBefore=calls.size();const auto notesAt=uiLayerNotes;
      auto o0=outOf(lo,q,0,source.Get());
      check(o0.hr==S_OK&&o0.tex&&o0.tex.Get()!=source.Get(),"layer-only: S_OK with a frame of its own (never null, never S_FALSE)");
      check(calls.size()==callsBefore,"...and no upscaler runs for the eye (no motion prep, no pass call)");
      check(o0.box[0]==0&&o0.box[1]==0&&o0.box[2]==1&&o0.box[3]==1,"...the full output bounds, as the pass returns them");
      const auto dd=o0.tex?descOf(o0.tex.Get()):D3D11_TEXTURE2D_DESC{};
      check(dd.Width==480&&dd.Height==360&&dd.Format==DXGI_FORMAT_R8G8B8A8_UNORM&&(dd.BindFlags&D3D11_BIND_SHADER_RESOURCE)&&dd.ArraySize==1&&dd.MipLevels==1,
            "...of the size the upscaler's output would have had (the door's 480x360, not the input's 320x240), in the source's own format");
      check(uiLayerNotes==notesAt+1&&uiLayerNoteEye==0&&uiLayerNoteSeq==q&&uiLayerNoteOut==o0.tex.Get(),
            "...noted to the UI layer as the pass's output (so the layer arms for the next frame)");
      check(gapCalls==1&&gapFrame==o0.tex.Get(),"...after the layer's own preflight, asked about that very frame");
      check(o0.tex&&pixel(o0.tex.Get(),0,0)==0xFF000000u&&pixel(o0.tex.Get(),479,359)==0xFF000000u&&pixel(o0.tex.Get(),240,180)==0xFF000000u,
            "...black, alpha one, cleared once when it was made");
      auto again=outOf(lo,q,0,source.Get());
      check(again.hr==E_INVALIDARG&&!again.tex,"...and an eye cannot be treated twice in a frame (layer-only marks it treated)");
      auto o1=outOf(lo,q,1,source.Get());
      check(o1.hr==S_OK&&o1.tex.Get()==o0.tex.Get()&&rawClears==1,"...the other eye shares the one black frame, which was cleared exactly once");
      blank=o0.tex;
      // frame 3: again -- the same frame, never made or cleared again
      fr=frame(21,++q);begin(lo,fr);routeTookSeq=q;
      auto n0=outOf(lo,q,0,source.Get()),n1=outOf(lo,q,1,source.Get());
      check(n0.hr==S_OK&&n1.hr==S_OK&&n0.tex.Get()==blank.Get()&&n1.tex.Get()==blank.Get()&&rawClears==1,
            "the next layer-only frame hands on the same black frame: made once per size, cleared once, read by everyone after");
      // frame 4: the route let go of the eye -- the eye route serves it, and finds its history broken
      fr=frame(21,++q);begin(lo,fr);routeTookEye[0]=routeTookEye[1]=false;routeTookSeq=0;
      const auto c4=calls.size();
      check(treat(lo,q,0,source.Get())==S_OK&&calls.size()==c4+1&&(calls.back().flags&1),
            "the eye route after layer-only frames resets the eye's history (the layer-only frames left the continuity alone)");
      treat(lo,q,1,source.Get());
      // frame 5: no layer-only frame in between: the history is continuous (the contrast that gives frame 4 its teeth)
      fr=frame(21,++q);begin(lo,fr);
      check(treat(lo,q,0,source.Get())==S_OK&&!(calls.back().flags&1),"...whereas two eye-route frames in a row keep it");
      treat(lo,q,1,source.Get());
      // the layer declines (a preflight gap): the same call goes on through the pass, the game's own eye
      fr=frame(21,++q);begin(lo,fr);routeTookEye[0]=routeTookEye[1]=true;routeTookSeq=q;gapAnswer=3;
      const auto c6=calls.size();const auto notes6=uiLayerNotes;
      auto d0=outOf(lo,q,0,source.Get());
      check(d0.hr==S_OK&&calls.size()==c6+1&&d0.tex.Get()==source.Get()&&uiLayerNotes==notes6+1&&uiLayerNoteOut==source.Get(),
            "a layer that cannot certainly composite: the eye is served by the pass in the SAME call (the game's own eye, noted as the pass's output), never a black frame");
      gapAnswer=0;
      // a format the composite does not run over: the black frame cannot be made, the eye route serves it
      fr=frame(21,++q);begin(lo,fr);routeTookSeq=q;
      D3D11_TEXTURE2D_DESC fd{};fd.Width=320;fd.Height=240;fd.MipLevels=fd.ArraySize=fd.SampleDesc.Count=1;fd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;fd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      ComPtr<ID3D11Texture2D> half;require(SUCCEEDED(d.device->CreateTexture2D(&fd,nullptr,&half)),"half-float texture");
      const auto c7=calls.size();
      auto h0=outOf(lo,q,0,half.Get());
      check(h0.hr==S_OK&&calls.size()==c7+1&&h0.tex.Get()==half.Get(),"a frame the layer's composite does not run over: the black frame is not made, the pass serves the eye");
      // the route took eye 1 only: eye 0 is the eye route's
      fr=frame(21,++q);begin(lo,fr);routeTookEye[0]=false;routeTookEye[1]=true;routeTookSeq=q;
      const auto c8=calls.size();
      auto m0=outOf(lo,q,0,source.Get()),m1=outOf(lo,q,1,source.Get());
      check(m0.hr==S_OK&&m0.tex.Get()==source.Get()&&m1.hr==S_OK&&m1.tex.Get()==blank.Get()&&calls.size()==c8+1,
            "one eye taken and one not: each is served by its own route in the same frame");
      // a stale take (another sequence) is not this frame's
      fr=frame(21,++q);begin(lo,fr);routeTookEye[0]=routeTookEye[1]=true;routeTookSeq=q-1;
      const auto c9=calls.size();
      auto s0=outOf(lo,q,0,source.Get());
      check(s0.hr==S_OK&&calls.size()==c9+1&&s0.tex.Get()==source.Get(),"an eye the route took in an earlier sequence is the eye route's in this one");
      treat(lo,q,1,source.Get());
    }
    check(refs(blank.Get())>=2,"the channel holds the black frame while it is open");
    check(lo.close(lo.context)==S_OK,"layer-only channel close");
    check(refs(blank.Get())==1,"...and closing releases every reference it held (the two eyes' shared frame included)");
    blank.Reset();
    routeTookEye[0]=routeTookEye[1]=false;routeTookSeq=0;routeOwns=false;

    // The door's output is the pass's, wherever the pass would have put it: DLAA at the input's size; under the served
    // floor at the size the pass is asked for.
    {
      auto dl=acquire(d,22,"dlaa");uint64_t s=0;auto fr=frame(22,++s);begin(dl,fr);treat(dl,s,0,source.Get());treat(dl,s,1,source.Get());
      fr=frame(22,++s);begin(dl,fr);routeTookEye[0]=true;routeTookSeq=s;
      auto o=outOf(dl,s,0,source.Get());const auto dd=o.tex?descOf(o.tex.Get()):D3D11_TEXTURE2D_DESC{};
      check(o.hr==S_OK&&o.tex.Get()!=source.Get()&&dd.Width==320&&dd.Height==240,"DLAA: the layer-only frame is the input's size (no upscale)");
      routeTookEye[0]=false;routeTookSeq=0;check(dl.close(dl.context)==S_OK,"dlaa channel close");
    }
    {
      // 239x179 into a 480x360 door is under half: no mode serves it, and the pass is asked for less (twice the input).
      auto fl=acquire(d,23,"dlss");uint64_t s=0;auto under=d.texture(239,179);
      auto fr=frame(23,++s);begin(fl,fr);check(treat(fl,s,0,under.Get())==S_OK&&!calls.empty(),"floor control: the eye route");
      const auto askW=calls.back().outW,askH=calls.back().outH;
      check(askW!=480||askH!=360,"floor control: the pass is asked for a cut size, not the door's (the case is not vacuous)");
      treat(fl,s,1,under.Get());
      fr=frame(23,++s);begin(fl,fr);routeTookEye[0]=true;routeTookSeq=s;
      auto o=outOf(fl,s,0,under.Get());const auto dd=o.tex?descOf(o.tex.Get()):D3D11_TEXTURE2D_DESC{};
      check(o.hr==S_OK&&dd.Width==askW&&dd.Height==askH,"under the served floor the layer-only frame is exactly the size the pass is asked for (the layer never re-sizes between the two routes)");
      routeTookEye[0]=false;routeTookSeq=0;check(fl.close(fl.context)==S_OK,"floor channel close");
    }
    // The pass stood down, or AA is off: the route cannot take an eye's frame at all -- the ordinary passthrough answers.
    {
      auto off=acquire(d,26,"off");auto fr=frame(26,1);begin(off,fr);routeTookEye[0]=true;routeTookSeq=1;
      auto o=outOf(off,1,0,source.Get());check(o.hr==S_FALSE&&!o.tex,"fix.temporal_aa off: a layer-only claim changes nothing (the ordinary passthrough)");
      routeTookEye[0]=false;routeTookSeq=0;check(off.close(off.context)==S_OK,"off channel close");
    }
  }
}
int wmain(int argc,wchar_t** argv){SetErrorMode(3);if(argc==2&&!wcscmp(argv[1],L"--dry-run")){std::puts("native_temporal_test: dry-run (no device or files)");return 0;}
  if(argc!=2||wcscmp(argv[1],L"--self-test"))return 2;try{run();}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());if(!failures)++failures;}
  std::printf("native_temporal_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
