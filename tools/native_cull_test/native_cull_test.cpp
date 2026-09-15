#include "../../src/openxr/native_cull_guard.h"
#include <cstdio>
#include <cmath>
#include <iostream>
#include <limits>

using namespace edvr::openxr;
static NativeCullFrustum f0{-1.0f,1.2f,-.8f,1.0f};
static NativeCullFrustum f1{-.9f,1.1f,-.75f,1.05f};
static NativeCullDimensions d0{1000,900}, d1{1100,920};

unsigned checks=0,failures=0;
#define CHECK(value) do { ++checks; if(!(value)){++failures;std::printf("FAIL: line %d: %s\n",__LINE__,#value);} }while(false)
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(!std::strcmp(argv[1],"--dry-run")){std::puts("native_cull_test: dry-run (no device or files)");return 0;}
  if(std::strcmp(argv[1],"--self-test"))return 2;
  NativeCullSettings s{}; s.mode=NativeCullMode::Symmetric; s.horizontalFraction=1; s.verticalFraction=1;
  NativeCullGuard g;
  NativeCullFrustum fs[2]{f0,f1}; NativeCullDimensions ds[2]{d0,d1};
  CHECK(g.beginFrame(s,fs,ds,false,1)==NativeCullStage::WaitingScene);
  CHECK(g.beginFrame(s,fs,ds,true,1)==NativeCullStage::Adopting);
  CHECK(g.gameFrustum(0).left==f0.left);
  auto r0=g.recommended(0); CHECK(r0.width>=d0.width&&r0.height>=d0.height);
  g.noteSubmittedSize(0,r0.width,r0.height); g.noteSubmittedSize(1,g.recommended(1).width,g.recommended(1).height);
  CHECK(g.beginFrame(s,fs,ds,true,1)==NativeCullStage::Adopting);
  g.noteSubmittedSize(0,r0.width*2,r0.height*2); g.noteSubmittedSize(1,g.recommended(1).width*2,g.recommended(1).height*2);
  CHECK(g.beginFrame(s,fs,ds,true,1)==NativeCullStage::Live);
  const auto b=g.cropBounds(0); CHECK(std::fabs(b.left-1.0f/12.0f)<.001f&&std::fabs(b.right-1.0f)<.001f&&std::fabs(b.top)<.001f&&std::fabs(b.bottom-.9f)<.001f);
  auto ca=g.canonical(0); CHECK(ca.width==2017&&ca.height==1890);
  g.standDown(); CHECK(g.stage()==NativeCullStage::Live);
  CHECK(g.beginFrame(s,fs,ds,true,1)==NativeCullStage::Inert);

  // The native cull recommendation is also the game's DLSS output size.
  // Elite floors HMD Quality 0.5, so an odd widened width would turn
  // 4857 into 2428 and miss NGX Performance's 2429 minimum. The game-facing
  // recommendation rounds up to an even width while preserving the eye pair.
  NativeCullSettings odd=s;
  const float outer=std::tan(.99168f),inner=std::tan(.80133f),vertical=std::tan(.90179f);
  NativeCullFrustum pimax[2]{{-outer,inner,-vertical,vertical},{-inner,outer,-vertical,vertical}};
  NativeCullDimensions oddDims[2]{{4068,4016},{4067,4016}};
  NativeCullGuard oddGuard;
  CHECK(oddGuard.beginFrame(odd,pimax,oddDims,true,1)==NativeCullStage::Adopting);
  const auto odd0=oddGuard.recommended(0), odd1=oddGuard.recommended(1);
  CHECK(std::lround(4068.f*oddGuard.factorWidth())==4857);
  CHECK(odd0.width==4858 && odd0.width/2==2429 && odd0.height==4016 && odd0.height/2==2008);
  CHECK((odd0.width&1u)==0u && (odd0.height&1u)==0u);
  CHECK((odd1.width&1u)==0u && (odd1.height&1u)==0u);
  oddGuard.noteSubmittedSize(0,odd0.width,odd0.height);oddGuard.noteSubmittedSize(1,odd1.width,odd1.height);
  CHECK(oddGuard.beginFrame(odd,pimax,oddDims,true,1)==NativeCullStage::Adopting);
  oddGuard.noteSubmittedSize(0,odd0.width*2,odd0.height*2);oddGuard.noteSubmittedSize(1,odd1.width*2,odd1.height*2);
  CHECK(oddGuard.beginFrame(odd,pimax,oddDims,true,1)==NativeCullStage::Live);
  CHECK(oddGuard.recommended(0).width==4858 && oddGuard.recommended(0).height==4016);
  // The next flight failed before widening: 3964x3913 became 1982x1956.
  // Waiting/off paths must also give NGX an exact half of an even target.
  NativeCullDimensions menuDims[2]{{3964,3913},{3965,3913}};
  NativeCullSettings waitingSettings=odd;
  NativeCullGuard waitingGuard;
  CHECK(waitingGuard.beginFrame(waitingSettings,pimax,menuDims,false,1)==NativeCullStage::WaitingScene);
  CHECK(waitingGuard.recommended(0).width==3964 && waitingGuard.recommended(0).height==3914);
  CHECK(waitingGuard.recommended(1).width==3966 && waitingGuard.recommended(1).height==3914);
  NativeCullSettings offSettings{};
  NativeCullGuard offGuard;
  CHECK(offGuard.beginFrame(offSettings,pimax,menuDims,true,1)==NativeCullStage::Off);
  CHECK(offGuard.recommended(0).width==3964 && offGuard.recommended(0).height==3914);
  CHECK(offGuard.recommended(1).width/2==1983 && offGuard.recommended(1).height/2==1957);
  CHECK(offGuard.recommended(2).width==0 && offGuard.recommended(2).height==0);
  CHECK(menuDims[0].height==3913 && menuDims[1].width==3965);
  NativeCullSettings evenSettings{}; evenSettings.mode=NativeCullMode::Percent; evenSettings.percent=20;
  NativeCullDimensions evenDims[2]{{4000,3000},{4000,3000}};
  NativeCullGuard evenGuard;
  CHECK(evenGuard.beginFrame(evenSettings,fs,evenDims,true,1)==NativeCullStage::Adopting);
  CHECK(evenGuard.recommended(0).width==4800 && evenGuard.recommended(0).height==3600);
  NativeCullDimensions verticalDims[2]{{4000,4001},{4000,4001}};
  NativeCullGuard verticalGuard;
  CHECK(verticalGuard.beginFrame(evenSettings,fs,verticalDims,true,1)==NativeCullStage::Adopting);
  CHECK(verticalGuard.recommended(0).height==4802 && verticalGuard.recommended(0).height/2==2401);
  CHECK(gameFacingDimension(4800)==4800 && gameFacingDimension(4801)==4802);
  CHECK(gameFacingDimension(16383)==16384 && gameFacingDimension(16384)==16384 && gameFacingDimension(0)==0);
  CHECK(gameFacingDimension(16385)==0 && gameFacingDimension(UINT32_MAX)==0);

  NativeCullSettings bad=s; bad.signatureCount=1; bad.signatures[0]={999,999};
  NativeCullGuard gated; CHECK(gated.beginFrame(bad,fs,ds,true,1)==NativeCullStage::Inert);
  NativeCullDimensions oddInert[2]{{4857,3913},{4857,3913}}; NativeCullGuard oddInertGuard;
  CHECK(oddInertGuard.beginFrame(bad,pimax,oddInert,true,1)==NativeCullStage::Inert);
  CHECK(oddInertGuard.recommended(0).width==4858 && oddInertGuard.recommended(0).height==3914);
  bad.signatures[0]={95,84}; NativeCullGuard allowed; CHECK(allowed.beginFrame(bad,fs,ds,true,1)==NativeCullStage::Adopting);
  NativeCullSettings pct{}; pct.mode=NativeCullMode::Percent; pct.percent=10;
  NativeCullGuard pg; CHECK(pg.beginFrame(pct,fs,ds,true,1)==NativeCullStage::Adopting);
  auto pf=pg.gameFrustum(0); CHECK(pf.left==f0.left); // projection stays true during adoption
  NativeCullFrustum invalid[2]{f0,f1}; invalid[0].right=std::numeric_limits<float>::quiet_NaN();
  NativeCullGuard ig; CHECK(ig.beginFrame(pct,invalid,ds,true,1)==NativeCullStage::Off);
  NativeCullGuard scaled;
  CHECK(scaled.beginFrame(s,fs,ds,true,1)==NativeCullStage::Adopting);
  scaled.noteSubmittedSize(1,600,400);scaled.noteSubmittedSize(0,640,480);
  CHECK(scaled.beginFrame(s,fs,ds,true,1)==NativeCullStage::Adopting);
  scaled.noteSubmittedSize(0,640,480);scaled.noteSubmittedSize(1,600,400);
  CHECK(scaled.beginFrame(s,fs,ds,true,1)==NativeCullStage::Adopting); // unchanged is never evidence
  scaled.noteSubmittedSize(0,704,560);scaled.noteSubmittedSize(0,16384,16384); // duplicate ignored
  CHECK(scaled.beginFrame(s,fs,ds,true,1)==NativeCullStage::Adopting); // partial pair ignored
  scaled.noteSubmittedSize(1,660,467);scaled.noteSubmittedSize(0,704,560);
  CHECK(scaled.beginFrame(s,fs,ds,true,1)==NativeCullStage::Live);
  CHECK(scaled.canonical(0).width==645&&scaled.canonical(0).height==504);
  CHECK(std::fabs(scaled.gameFrustum(1).left+1.1f)<.0001f&&std::fabs(scaled.gameFrustum(1).down+1.05f)<.0001f);
  CHECK(scaled.beginFrame(s,fs,ds,true,2)==NativeCullStage::Live&&!scaled.changed());
  CHECK(scaled.canonical(0).width==645&&scaled.canonical(0).height==504); // recenter preserves adopted target evidence
  NativeCullSettings off;fs[0].left=-1.3f;ds[0]={900,800};
  CHECK(scaled.beginFrame(off,fs,ds,true,2)==NativeCullStage::Off&&scaled.changed());
  CHECK(scaled.gameFrustum(0).left==-1.3f&&scaled.recommended(0).width==900);
  CHECK(scaled.beginFrame(s,fs,ds,true,2)==NativeCullStage::Adopting);
  CHECK(scaled.beginFrame(s,fs,ds,true,3)==NativeCullStage::Adopting&&!scaled.changed());
  NativeCullSettings excessive=pct;excessive.percent=51;NativeCullGuard bounded;
  CHECK(bounded.beginFrame(excessive,fs,ds,true,1)==NativeCullStage::Off&&bounded.recommended(0).width==900);
  NativeCullDimensions tooWide[2]{{16384,16384},{16384,16384}};NativeCullGuard dimensions;
  CHECK(dimensions.beginFrame(pct,fs,tooWide,true,1)==NativeCullStage::Inert&&dimensions.recommended(0).width==16384);
  NativeCullDimensions oddCap[2]{{16383,16000},{16383,16000}};NativeCullGuard cappedOdd;
  CHECK(cappedOdd.beginFrame(pct,fs,oddCap,true,1)==NativeCullStage::Inert&&cappedOdd.recommended(0).width==16384);
  NativeCullFrustum equal[2]{{-1,1,-1,1},{-1,1,-1,1}};NativeCullGuard noMargin;
  CHECK(noMargin.beginFrame(s,equal,ds,true,1)==NativeCullStage::Inert);
  CHECK(noMargin.beginFrame(s,equal,ds,true,1)==NativeCullStage::Inert&&!noMargin.changed());
  std::printf("native_cull_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
