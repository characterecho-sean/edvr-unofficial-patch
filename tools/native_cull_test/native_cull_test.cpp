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

  // ---- the field of view trim -------------------------------------------
  // Sean's Crystal Super, from the 2026-09-16 flights: outer 56.81 deg, nasal
  // 45.91, up and down 51.67, at 3964x3914 per eye.
  const float csOuter=1.529f,csNasal=1.032f,csVertical=1.265f;
  NativeCullFrustum crystal[2]{{-csOuter,csNasal,-csVertical,csVertical},
                               {-csNasal,csOuter,-csVertical,csVertical}};
  NativeCullDimensions crystalDims[2]{{3964,3914},{3964,3914}};
  const float verticalSpan=2*csVertical;
  // Two complete pairs land a stage: one at the size the game was already
  // rendering, which seeds the baseline, and one at the size it was asked
  // for, which is the evidence that it rebuilt.
  const auto land=[](NativeCullGuard& g,const NativeCullSettings& st,const NativeCullFrustum (&f)[2],
                     const NativeCullDimensions (&d)[2]){
    g.noteSubmittedSize(0,d[0].width,d[0].height);g.noteSubmittedSize(1,d[1].width,d[1].height);
    g.beginFrame(st,f,d,true,1);
    const auto a=g.recommended(0),b=g.recommended(1);
    g.noteSubmittedSize(0,a.width,a.height);g.noteSubmittedSize(1,b.width,b.height);
    return g.beginFrame(st,f,d,true,1);
  };

  // (a) a vertical trim alone: the same width, a shorter target, the crop
  // untouched and the placement a band inside the eye's own field.
  NativeCullSettings trimOnly{}; trimOnly.trimVerticalDeg=10;
  NativeCullGuard trimGuard;
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,false,1)==NativeCullStage::WaitingScene);
  CHECK(trimGuard.recommended(0).width==3964&&trimGuard.recommended(0).height==3914);
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  // 51.673 degrees less ten is 41.673, whose tangent is 0.8901.
  const float trimmedUp=std::tan(std::atan(csVertical)-10.0f*3.1415926535f/180.0f);
  CHECK(std::fabs(trimmedUp-0.8901f)<.001f);
  const auto trimRec=trimGuard.recommended(0);
  CHECK(trimRec.width==3964);
  const uint32_t wantHeight=uint32_t(std::lround(3914.f*(2*trimmedUp/verticalSpan)));
  CHECK(trimRec.height==wantHeight+(wantHeight&1u)&&(trimRec.height&1u)==0u);
  CHECK(std::fabs(trimGuard.factorWidth()-1.0f)<1e-6f);
  CHECK(std::fabs(trimGuard.factorHeight()-2*trimmedUp/verticalSpan)<.0005f);
  CHECK(trimGuard.widenFactorWidth()==1.0f&&trimGuard.widenFactorHeight()==1.0f);
  CHECK(std::fabs(trimGuard.appliedVerticalDeg(0)-10.f)<.001f&&trimGuard.appliedOuterDeg(0)==0);
  // The projection stays true until the game has actually shrunk its targets.
  CHECK(trimGuard.gameFrustum(0).up==csVertical&&trimGuard.placementBounds(0).top==0);
  trimGuard.noteSubmittedSize(0,3964,3914);trimGuard.noteSubmittedSize(1,3964,3914);
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  trimGuard.noteSubmittedSize(0,3964,2740);trimGuard.noteSubmittedSize(1,3964,2740); // within 3%
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,true,1)==NativeCullStage::Live);
  CHECK(std::fabs(trimGuard.gameFrustum(0).up-trimmedUp)<.0005f&&
        std::fabs(trimGuard.gameFrustum(0).down+trimmedUp)<.0005f);
  CHECK(trimGuard.gameFrustum(0).left==-csOuter&&trimGuard.gameFrustum(0).right==csNasal);
  const auto trimCrop=trimGuard.cropBounds(0);
  CHECK(trimCrop.left==0.f&&trimCrop.top==0.f&&trimCrop.right==1.f&&trimCrop.bottom==1.f);
  const auto trimPlace=trimGuard.placementBounds(0);
  const float band=(csVertical-trimmedUp)/verticalSpan;
  CHECK(std::fabs(trimPlace.top-band)<.0005f&&std::fabs(trimPlace.bottom-(1-band))<.0005f);
  CHECK(trimPlace.left==0.f&&trimPlace.right==1.f);
  CHECK(std::fabs(trimGuard.contentFrustum(0).up-trimmedUp)<.0005f);
  CHECK(trimGuard.runtimeFrustum(0).up==csVertical);

  // (b) outer 5 and nasal 10: each eye trims its own sides, mirrored.
  NativeCullSettings sides{}; sides.trimOuterDeg=5; sides.trimNasalDeg=10;
  NativeCullGuard sideGuard;
  CHECK(sideGuard.beginFrame(sides,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  const auto sideRec=sideGuard.recommended(0);
  sideGuard.noteSubmittedSize(0,3964,3914);sideGuard.noteSubmittedSize(1,3964,3914);
  CHECK(sideGuard.beginFrame(sides,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  sideGuard.noteSubmittedSize(0,sideRec.width,sideRec.height);
  sideGuard.noteSubmittedSize(1,sideGuard.recommended(1).width,sideGuard.recommended(1).height);
  CHECK(sideGuard.beginFrame(sides,crystal,crystalDims,true,1)==NativeCullStage::Live);
  const float outerTrimmed=std::tan(std::atan(csOuter)-5.0f*3.1415926535f/180.0f);  // 56.81 - 5
  const float nasalTrimmed=std::tan(std::atan(csNasal)-10.0f*3.1415926535f/180.0f); // 45.91 - 10
  CHECK(std::fabs(outerTrimmed-1.2712f)<.001f&&std::fabs(nasalTrimmed-0.7243f)<.001f);
  const auto left=sideGuard.gameFrustum(0),right=sideGuard.gameFrustum(1);
  CHECK(std::fabs(left.left+outerTrimmed)<.001f&&std::fabs(left.right-nasalTrimmed)<.001f);
  CHECK(std::fabs(right.right-outerTrimmed)<.001f&&std::fabs(right.left+nasalTrimmed)<.001f);
  CHECK(left.up==csVertical&&left.down==-csVertical&&sideGuard.recommended(0).height==3914);
  const auto lp=sideGuard.placementBounds(0),rp=sideGuard.placementBounds(1);
  const float horizontalSpan=csOuter+csNasal;
  CHECK(std::fabs(lp.left-(csOuter-outerTrimmed)/horizontalSpan)<.001f);
  CHECK(std::fabs(1-lp.right-(csNasal-nasalTrimmed)/horizontalSpan)<.001f);
  CHECK(std::fabs(lp.left-(1-rp.right))<.001f&&std::fabs(lp.right-(1-rp.left))<.001f); // mirrored
  CHECK(lp.top==0.f&&lp.bottom==1.f);
  CHECK(std::fabs(sideGuard.appliedOuterDeg(1)-5.f)<.001f&&std::fabs(sideGuard.appliedNasalDeg(1)-10.f)<.001f);

  // (c) a trim under a widening: the game renders the widened frustum, the
  // crop brings it back to the trimmed one, the placement puts that inside
  // the eye's full field. The size factor falls below 1; the widening the
  // graphics half is told never does.
  NativeCullSettings both=sides; both.mode=NativeCullMode::Percent; both.percent=10;
  NativeCullGuard bothGuard;
  CHECK(bothGuard.beginFrame(both,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  CHECK(bothGuard.factorWidth()<1.0f&&bothGuard.widenFactorWidth()>1.0f);
  CHECK(std::fabs(bothGuard.widenFactorWidth()-1.1f)<.0001f&&std::fabs(bothGuard.widenFactorHeight()-1.1f)<.0001f);
  const auto bothRec=bothGuard.recommended(0);
  CHECK(bothRec.width<3964&&bothRec.height>3914); // narrower sides, a widened top and bottom
  bothGuard.noteSubmittedSize(0,3964,3914);bothGuard.noteSubmittedSize(1,3964,3914);
  CHECK(bothGuard.beginFrame(both,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  bothGuard.noteSubmittedSize(0,bothRec.width,bothRec.height);
  bothGuard.noteSubmittedSize(1,bothGuard.recommended(1).width,bothGuard.recommended(1).height);
  CHECK(bothGuard.beginFrame(both,crystal,crystalDims,true,1)==NativeCullStage::Live);
  const auto bothCrop=bothGuard.cropBounds(0);
  CHECK(bothCrop.left>0.f&&bothCrop.right<1.f&&bothCrop.top>0.f&&bothCrop.bottom<1.f);
  CHECK(std::fabs(bothCrop.right-bothCrop.left-1.0f/1.1f)<.001f);
  const auto bothPlace=bothGuard.placementBounds(0);
  CHECK(std::fabs(bothPlace.left-lp.left)<.001f&&std::fabs(bothPlace.right-lp.right)<.001f);
  CHECK(bothPlace.top==0.f&&bothPlace.bottom==1.f); // no vertical trim to place
  CHECK(std::fabs(bothGuard.contentFrustum(0).left+outerTrimmed)<.001f);

  // (d) the limits. 30 degrees off a 51.67 degree edge leaves 21.67 and is
  // honoured; the same ask on a 15 degree edge is cut back to leave the
  // minimum span, and both edges give up the same share of it.
  NativeCullSettings deep{}; deep.trimVerticalDeg=30;
  NativeCullGuard deepGuard;
  CHECK(deepGuard.beginFrame(deep,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  CHECK(std::fabs(deepGuard.appliedVerticalDeg(0)-30.f)<.01f);
  CHECK(land(deepGuard,deep,crystal,crystalDims)==NativeCullStage::Live);
  CHECK(std::fabs(deepGuard.contentFrustum(0).up-std::tan(std::atan(csVertical)-30.0f*3.1415926535f/180.0f))<.001f);
  NativeCullSettings deepOuter{}; deepOuter.trimOuterDeg=30;
  NativeCullGuard outerGuard;
  CHECK(outerGuard.beginFrame(deepOuter,crystal,crystalDims,true,1)==NativeCullStage::Adopting);
  CHECK(std::fabs(outerGuard.appliedOuterDeg(0)-30.f)<.01f&&outerGuard.appliedNasalDeg(0)==0);
  CHECK(land(outerGuard,deepOuter,crystal,crystalDims)==NativeCullStage::Live);
  // The nasal edge of eye 0 is the 45.91 degree one and keeps every degree.
  CHECK(outerGuard.gameFrustum(0).right==csNasal&&outerGuard.gameFrustum(1).left==-csNasal);
  CHECK(outerGuard.recommended(0).width<3964&&outerGuard.recommended(0).height==3914);
  const float narrow=std::tan(15.0f*3.1415926535f/180.0f);
  NativeCullFrustum tight[2]{{-csOuter,csNasal,-narrow,narrow},{-csNasal,csOuter,-narrow,narrow}};
  NativeCullSettings tightTrim{}; tightTrim.trimVerticalDeg=10;
  NativeCullGuard tightGuard;
  CHECK(tightGuard.beginFrame(tightTrim,tight,crystalDims,true,1)==NativeCullStage::Adopting);
  CHECK(std::fabs(tightGuard.appliedVerticalDeg(0)-5.f)<.02f); // 30 asked, 20 the floor, halved
  CHECK(land(tightGuard,tightTrim,tight,crystalDims)==NativeCullStage::Live);
  CHECK(std::fabs(tightGuard.contentFrustum(0).up-std::tan(10.0f*3.1415926535f/180.0f))<.001f);
  CHECK(tightGuard.contentFrustum(0).up>0&&tightGuard.contentFrustum(0).down<0);

  // (e) zero trims change nothing at all, on the guard's own earlier cases.
  // fs and ds were edited above; the first case's own inputs are f0/f1/d0/d1.
  NativeCullFrustum zf[2]{f0,f1}; NativeCullDimensions zd[2]{d0,d1};
  NativeCullSettings zeroed=s; zeroed.trimOuterDeg=zeroed.trimNasalDeg=zeroed.trimVerticalDeg=0;
  NativeCullGuard zeroGuard;
  CHECK(zeroGuard.beginFrame(zeroed,zf,zd,false,1)==NativeCullStage::WaitingScene);
  CHECK(zeroGuard.beginFrame(zeroed,zf,zd,true,1)==NativeCullStage::Adopting);
  CHECK(zeroGuard.gameFrustum(0).left==zf[0].left&&!zeroGuard.trimmed());
  const auto z0=zeroGuard.recommended(0);
  zeroGuard.noteSubmittedSize(0,z0.width,z0.height);zeroGuard.noteSubmittedSize(1,zeroGuard.recommended(1).width,zeroGuard.recommended(1).height);
  CHECK(zeroGuard.beginFrame(zeroed,zf,zd,true,1)==NativeCullStage::Adopting);
  zeroGuard.noteSubmittedSize(0,z0.width*2,z0.height*2);zeroGuard.noteSubmittedSize(1,zeroGuard.recommended(1).width*2,zeroGuard.recommended(1).height*2);
  CHECK(zeroGuard.beginFrame(zeroed,zf,zd,true,1)==NativeCullStage::Live);
  const auto zb=zeroGuard.cropBounds(0);
  CHECK(std::fabs(zb.left-1.0f/12.0f)<.001f&&std::fabs(zb.right-1.0f)<.001f&&std::fabs(zb.top)<.001f&&std::fabs(zb.bottom-.9f)<.001f);
  const auto zp=zeroGuard.placementBounds(0);
  CHECK(zp.left==0.f&&zp.top==0.f&&zp.right==1.f&&zp.bottom==1.f);
  CHECK(zeroGuard.widenFactorWidth()==zeroGuard.factorWidth()&&zeroGuard.widenFactorHeight()==zeroGuard.factorHeight());
  CHECK(zeroGuard.canonical(0).width==2017&&zeroGuard.canonical(0).height==1890);
  // An out-of-range or non-finite trim refuses the whole settings block.
  NativeCullSettings wild{}; wild.trimVerticalDeg=31;
  NativeCullGuard wildGuard;
  CHECK(wildGuard.beginFrame(wild,crystal,crystalDims,true,1)==NativeCullStage::Off);
  wild.trimVerticalDeg=std::numeric_limits<float>::quiet_NaN();
  CHECK(wildGuard.beginFrame(wild,crystal,crystalDims,true,1)==NativeCullStage::Off);
  wild.trimVerticalDeg=-1;
  CHECK(wildGuard.beginFrame(wild,crystal,crystalDims,true,1)==NativeCullStage::Off);
  // A trim too small to be worth a rebuild is not one.
  NativeCullSettings slight{}; slight.trimNasalDeg=0.2f;
  NativeCullGuard slightGuard;
  CHECK(slightGuard.beginFrame(slight,crystal,crystalDims,true,1)==NativeCullStage::Inert);
  std::printf("native_cull_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
