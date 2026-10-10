#include "../../src/openxr/native_fov_trim.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace edvr::openxr;

unsigned checks=0,failures=0;
#define CHECK(value) do { ++checks; if(!(value)){++failures;std::printf("FAIL: line %d: %s\n",__LINE__,#value);} }while(false)
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(!std::strcmp(argv[1],"--dry-run")){std::puts("native_trim_test: dry-run (no device or files)");return 0;}
  if(std::strcmp(argv[1],"--self-test"))return 2;

  // ---- what the game is told is even on both axes --------------------------
  // Elite applies its HMD quality multiplier with integer truncation, so an odd
  // recommendation can lose a pixel at quality 0.5 and fall below NGX's strict
  // minimum (a width of 4857 became 2428 against a minimum of 2429). The
  // game-facing recommendation rounds up to an even size while preserving the
  // eye pair, trimmed or not.
  const float outer=std::tan(.99168f),inner=std::tan(.80133f),vertical=std::tan(.90179f);
  NativeTrimFrustum pimax[2]{{-outer,inner,-vertical,vertical},{-inner,outer,-vertical,vertical}};
  NativeTrimDimensions oddDims[2]{{4068,4016},{4067,4016}};
  NativeTrimSettings oddTrim{}; oddTrim.trimOuterDeg=5; oddTrim.trimVerticalDeg=3;
  NativeFovTrim oddTrimmed;
  CHECK(oddTrimmed.beginFrame(oddTrim,pimax,oddDims,1)==NativeTrimStage::Adopting);
  const auto odd0=oddTrimmed.recommended(0), odd1=oddTrimmed.recommended(1);
  CHECK(odd0.width<4068 && odd0.height<4016);
  CHECK((odd0.width&1u)==0u && (odd0.height&1u)==0u);
  CHECK((odd1.width&1u)==0u && (odd1.height&1u)==0u);
  // The next flight failed before any trim: 3964x3913 became 1982x1956.
  // The off path must also give NGX an exact half of an even target.
  NativeTrimDimensions menuDims[2]{{3964,3913},{3965,3913}};
  NativeTrimSettings offSettings{};
  NativeFovTrim offGuard;
  CHECK(offGuard.beginFrame(offSettings,pimax,menuDims,1)==NativeTrimStage::Off);
  CHECK(offGuard.recommended(0).width==3964 && offGuard.recommended(0).height==3914);
  CHECK(offGuard.recommended(1).width/2==1983 && offGuard.recommended(1).height/2==1957);
  CHECK(offGuard.recommended(2).width==0 && offGuard.recommended(2).height==0);
  CHECK(menuDims[0].height==3913 && menuDims[1].width==3965);
  CHECK(gameFacingDimension(4800)==4800 && gameFacingDimension(4801)==4802);
  CHECK(gameFacingDimension(16383)==16384 && gameFacingDimension(16384)==16384 && gameFacingDimension(0)==0);
  CHECK(gameFacingDimension(16385)==0 && gameFacingDimension(UINT32_MAX)==0);
  NativeTrimFrustum invalid[2]{pimax[0],pimax[1]}; invalid[0].right=std::numeric_limits<float>::quiet_NaN();
  NativeFovTrim ig; CHECK(ig.beginFrame(oddTrim,invalid,oddDims,1)==NativeTrimStage::Off);

  // ---- the field of view trim -------------------------------------------
  // Sean's Crystal Super, from the 2026-09-16 flights: outer 56.81 deg, nasal
  // 45.91, up and down 51.67, at 3964x3914 per eye.
  const float csOuter=1.529f,csNasal=1.032f,csVertical=1.265f;
  NativeTrimFrustum crystal[2]{{-csOuter,csNasal,-csVertical,csVertical},
                               {-csNasal,csOuter,-csVertical,csVertical}};
  NativeTrimDimensions crystalDims[2]{{3964,3914},{3964,3914}};
  const float verticalSpan=2*csVertical;
  // Two complete pairs land a stage: one at the size the game was already
  // rendering, which seeds the baseline, and one at the size it was asked
  // for, which is the evidence that it rebuilt.
  const auto land=[](NativeFovTrim& g,const NativeTrimSettings& st,const NativeTrimFrustum (&f)[2],
                     const NativeTrimDimensions (&d)[2]){
    g.noteSubmittedSize(0,d[0].width,d[0].height);g.noteSubmittedSize(1,d[1].width,d[1].height);
    g.beginFrame(st,f,d,1);
    const auto a=g.recommended(0),b=g.recommended(1);
    g.noteSubmittedSize(0,a.width,a.height);g.noteSubmittedSize(1,b.width,b.height);
    return g.beginFrame(st,f,d,1);
  };

  // (a) a vertical trim alone: the same width, a shorter target, the crop
  // untouched and the placement a band inside the eye's own field.
  NativeTrimSettings trimOnly{}; trimOnly.trimVerticalDeg=10;
  NativeFovTrim trimGuard;
  // A trim alone does not wait for the scene (2026-09-23): it is the frustum
  // the headset shows. The game has asked (the default), so its rebuild is
  // what lands it; the first ask has its own section at the end.
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(trimGuard.route()==NativeTrimRoute::Rebuild);
  CHECK(trimGuard.treatedFor(0).width==3964&&trimGuard.treatedFor(0).height==3914);
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,1)==NativeTrimStage::Adopting&&!trimGuard.changed());
  // 51.673 degrees less ten is 41.673, whose tangent is 0.8901.
  const float trimmedUp=std::tan(std::atan(csVertical)-10.0f*3.1415926535f/180.0f);
  CHECK(std::fabs(trimmedUp-0.8901f)<.001f);
  const auto trimRec=trimGuard.recommended(0);
  CHECK(trimRec.width==3964);
  const uint32_t wantHeight=uint32_t(std::lround(3914.f*(2*trimmedUp/verticalSpan)));
  CHECK(trimRec.height==wantHeight+(wantHeight&1u)&&(trimRec.height&1u)==0u);
  CHECK(std::fabs(trimGuard.factorWidth()-1.0f)<1e-6f);
  CHECK(std::fabs(trimGuard.factorHeight()-2*trimmedUp/verticalSpan)<.0005f);
  CHECK(std::fabs(trimGuard.appliedVerticalDeg(0)-10.f)<.001f&&trimGuard.appliedOuterDeg(0)==0);
  // The projection stays true until the game has actually shrunk its targets.
  CHECK(trimGuard.gameFrustum(0).up==csVertical&&trimGuard.placementBounds(0).top==0);
  trimGuard.noteSubmittedSize(0,3964,3914);trimGuard.noteSubmittedSize(1,3964,3914);
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  trimGuard.noteSubmittedSize(0,3964,2740);trimGuard.noteSubmittedSize(1,3964,2740); // within 3%
  CHECK(trimGuard.beginFrame(trimOnly,crystal,crystalDims,1)==NativeTrimStage::Live);
  CHECK(std::fabs(trimGuard.gameFrustum(0).up-trimmedUp)<.0005f&&
        std::fabs(trimGuard.gameFrustum(0).down+trimmedUp)<.0005f);
  CHECK(trimGuard.gameFrustum(0).left==-csOuter&&trimGuard.gameFrustum(0).right==csNasal);
  const auto trimPlace=trimGuard.placementBounds(0);
  const float band=(csVertical-trimmedUp)/verticalSpan;
  CHECK(std::fabs(trimPlace.top-band)<.0005f&&std::fabs(trimPlace.bottom-(1-band))<.0005f);
  CHECK(trimPlace.left==0.f&&trimPlace.right==1.f);
  CHECK(std::fabs(trimGuard.contentFrustum(0).up-trimmedUp)<.0005f);
  CHECK(trimGuard.runtimeFrustum(0).up==csVertical);

  // (b) outer 5 and nasal 10: each eye trims its own sides, mirrored.
  NativeTrimSettings sides{}; sides.trimOuterDeg=5; sides.trimNasalDeg=10;
  NativeFovTrim sideGuard;
  CHECK(sideGuard.beginFrame(sides,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  const auto sideRec=sideGuard.recommended(0);
  sideGuard.noteSubmittedSize(0,3964,3914);sideGuard.noteSubmittedSize(1,3964,3914);
  CHECK(sideGuard.beginFrame(sides,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  sideGuard.noteSubmittedSize(0,sideRec.width,sideRec.height);
  sideGuard.noteSubmittedSize(1,sideGuard.recommended(1).width,sideGuard.recommended(1).height);
  CHECK(sideGuard.beginFrame(sides,crystal,crystalDims,1)==NativeTrimStage::Live);
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

  // (d) the limits. 30 degrees off a 51.67 degree edge leaves 21.67 and is
  // honoured; the same ask on a 15 degree edge is cut back to leave the
  // minimum span, and both edges give up the same share of it.
  NativeTrimSettings deep{}; deep.trimVerticalDeg=30;
  NativeFovTrim deepGuard;
  CHECK(deepGuard.beginFrame(deep,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(std::fabs(deepGuard.appliedVerticalDeg(0)-30.f)<.01f);
  CHECK(land(deepGuard,deep,crystal,crystalDims)==NativeTrimStage::Live);
  CHECK(std::fabs(deepGuard.contentFrustum(0).up-std::tan(std::atan(csVertical)-30.0f*3.1415926535f/180.0f))<.001f);
  NativeTrimSettings deepOuter{}; deepOuter.trimOuterDeg=30;
  NativeFovTrim outerGuard;
  CHECK(outerGuard.beginFrame(deepOuter,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(std::fabs(outerGuard.appliedOuterDeg(0)-30.f)<.01f&&outerGuard.appliedNasalDeg(0)==0);
  CHECK(land(outerGuard,deepOuter,crystal,crystalDims)==NativeTrimStage::Live);
  // The nasal edge of eye 0 is the 45.91 degree one and keeps every degree.
  CHECK(outerGuard.gameFrustum(0).right==csNasal&&outerGuard.gameFrustum(1).left==-csNasal);
  CHECK(outerGuard.recommended(0).width<3964&&outerGuard.recommended(0).height==3914);
  const float narrow=std::tan(15.0f*3.1415926535f/180.0f);
  NativeTrimFrustum tight[2]{{-csOuter,csNasal,-narrow,narrow},{-csNasal,csOuter,-narrow,narrow}};
  NativeTrimSettings tightTrim{}; tightTrim.trimVerticalDeg=10;
  NativeFovTrim tightGuard;
  CHECK(tightGuard.beginFrame(tightTrim,tight,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(std::fabs(tightGuard.appliedVerticalDeg(0)-5.f)<.02f); // 30 asked, 20 the floor, halved
  CHECK(land(tightGuard,tightTrim,tight,crystalDims)==NativeTrimStage::Live);
  CHECK(std::fabs(tightGuard.contentFrustum(0).up-std::tan(10.0f*3.1415926535f/180.0f))<.001f);
  CHECK(tightGuard.contentFrustum(0).up>0&&tightGuard.contentFrustum(0).down<0);

  // An out-of-range or non-finite trim refuses the whole settings block.
  NativeTrimSettings wild{}; wild.trimVerticalDeg=31;
  NativeFovTrim wildGuard;
  CHECK(wildGuard.beginFrame(wild,crystal,crystalDims,1)==NativeTrimStage::Off);
  wild.trimVerticalDeg=std::numeric_limits<float>::quiet_NaN();
  CHECK(wildGuard.beginFrame(wild,crystal,crystalDims,1)==NativeTrimStage::Off);
  wild.trimVerticalDeg=-1;
  CHECK(wildGuard.beginFrame(wild,crystal,crystalDims,1)==NativeTrimStage::Off);
  // A trim too small to be worth a rebuild is not one.
  NativeTrimSettings slight{}; slight.trimNasalDeg=0.2f;
  NativeFovTrim slightGuard;
  CHECK(slightGuard.beginFrame(slight,crystal,crystalDims,1)==NativeTrimStage::Inert);

  // ---- adoption after a change while live (flight 3, 2026-09-16) ---------
  // The game renders at 65% of what it is told (DLSS quality). Outer 10 and
  // vertical 10 went live in two seconds; outer 10 to 5 then sat at stage 2
  // for two minutes, because the rebuilt 2317x1790 could never reach 0.899 x
  // the 2111x1790 the game already had. The rebuild is measured against the
  // ask the baseline was rendered for.
  const auto q=[](NativeTrimDimensions d){return NativeTrimDimensions{uint32_t(std::lround(d.width*.65f)),uint32_t(std::lround(d.height*.65f))};};
  const auto pair=[](NativeFovTrim& g,NativeTrimDimensions d){g.noteSubmittedSize(0,d.width,d.height);g.noteSubmittedSize(1,d.width,d.height);};
  const auto true65=q({3964,3914});
  CHECK(true65.width==2577&&true65.height==2544);
  NativeTrimSettings flight{}; flight.trimOuterDeg=10; flight.trimVerticalDeg=10;
  NativeFovTrim flightGuard;
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(flightGuard,true65);
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(flightGuard.baselineReady()&&flightGuard.baselineAsk(0).width==3964&&flightGuard.baselineAsk(0).height==3914);
  // The flight's tangents are a hair wider than this rig's rounded ones, so
  // its 3248 is 3246 here and its 2111 is 2110; outer 5 lands on 3566 in both.
  const auto ask1=q(flightGuard.recommended(0));
  CHECK(flightGuard.recommended(0).width==3246&&flightGuard.recommended(0).height==2754&&ask1.width==2110&&ask1.height==1790);
  pair(flightGuard,ask1);
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Live);
  flight.trimOuterDeg=5;
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Adopting&&flightGuard.changed());
  // Outer 10 to 5 leaves the height alone, which the game does not rebuild
  // for on its own (flight 5): it is told two rows under the floor, and the
  // rebuild is measured against that.
  CHECK(flightGuard.nudge()==NativeTrimNudge::Started&&flightGuard.nudgeFloor()==2754);
  CHECK(flightGuard.recommended(0).width==3566&&flightGuard.recommended(0).height==2752&&flightGuard.trueRecommended(0).height==2754);
  // The frame in hand was rendered for the previous ask: the temporal pass is
  // sized by that until the rebuild lands, while the game is told the new one.
  CHECK(flightGuard.treatedFor(0).width==3246&&flightGuard.treatedFor(0).height==2754);
  // The first pair after the change is still the old target, rendered for
  // the old ask: that is the baseline, and its ask is the old one.
  pair(flightGuard,ask1);
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(flightGuard.baselineAsk(0).width==3246&&flightGuard.baselineAsk(0).height==2754);
  CHECK(flightGuard.treatedFor(0).width==3246&&flightGuard.treatedFor(0).height==2754);
  pair(flightGuard,ask1);
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Adopting); // unchanged is never evidence
  const auto ask2=q(flightGuard.recommended(0));
  CHECK(ask2.width==2318&&ask2.height==1789);
  pair(flightGuard,ask2);
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Live);
  CHECK(std::fabs(flightGuard.appliedOuterDeg(0)-5.f)<.001f&&std::fabs(flightGuard.appliedVerticalDeg(0)-10.f)<.001f);
  CHECK(flightGuard.treatedFor(0).width==3566&&flightGuard.treatedFor(0).height==2752); // live: the new ask, dip and all
  CHECK(flightGuard.adoptingFrames()<10);
  // Back to no trim at all, then a trim again: the baseline for the second
  // adoption is rendered for the runtime's own size, which the game was
  // told while off. The game kept the targets it had through the taller
  // ask, so the same trim again is told two rows under the floor it left.
  NativeTrimSettings none{};
  CHECK(flightGuard.beginFrame(none,crystal,crystalDims,1)==NativeTrimStage::Off&&flightGuard.changed());
  CHECK(flightGuard.recommended(0).width==3964&&flightGuard.recommended(0).height==3914);
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(flightGuard.nudge()==NativeTrimNudge::Started&&flightGuard.nudgeFloor()==2752);
  CHECK(flightGuard.recommended(0).width==3566&&flightGuard.recommended(0).height==2750);
  pair(flightGuard,true65);
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(flightGuard.baselineAsk(0).width==3964&&flightGuard.baselineAsk(0).height==3914);
  CHECK(flightGuard.treatedFor(0).width==3964&&flightGuard.treatedFor(0).height==3914);
  pair(flightGuard,q(flightGuard.recommended(0)));
  CHECK(flightGuard.beginFrame(flight,crystal,crystalDims,1)==NativeTrimStage::Live);

  // A change before the first rebuild has landed keeps the baseline. The pair
  // after the second change may be the old target or the first ask's; only
  // the second ask's size promotes.
  NativeTrimSettings twice{}; twice.trimVerticalDeg=10;
  NativeFovTrim twiceGuard;
  CHECK(twiceGuard.beginFrame(twice,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(twiceGuard,true65);
  CHECK(twiceGuard.beginFrame(twice,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  const auto first=q(twiceGuard.recommended(0));
  twice.trimVerticalDeg=15;
  CHECK(twiceGuard.beginFrame(twice,crystal,crystalDims,1)==NativeTrimStage::Adopting&&twiceGuard.changed());
  CHECK(twiceGuard.baselineReady()&&twiceGuard.baseline(0).height==true65.height&&twiceGuard.baselineAsk(0).height==3914);
  const auto second=q(twiceGuard.recommended(0));
  CHECK(second.height<first.height&&second.width==first.width);
  pair(twiceGuard,first); // the game had reached the first ask, not the second
  CHECK(twiceGuard.beginFrame(twice,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(twiceGuard,second);
  CHECK(twiceGuard.beginFrame(twice,crystal,crystalDims,1)==NativeTrimStage::Live);
  CHECK(std::fabs(twiceGuard.appliedVerticalDeg(0)-15.f)<.001f);

  // The game's own multiplier moved between the baseline and the rebuild
  // (0.65 to 0.5 in flight 3): no ratio matches, so a target that moved at
  // all is taken as the rebuild once the grace period has passed, and one
  // that never moved never is.
  NativeTrimSettings grace{}; grace.trimOuterDeg=10;
  NativeFovTrim graceGuard;
  CHECK(graceGuard.beginFrame(grace,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(graceGuard,true65);
  CHECK(graceGuard.beginFrame(grace,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  const auto askG=graceGuard.recommended(0);
  const NativeTrimDimensions half{askG.width/2,askG.height/2};
  CHECK(askG.width==3246&&askG.height==3914&&half.height<uint32_t(true65.height*.9f));
  NativeTrimStage graceStage=NativeTrimStage::Adopting;unsigned graceFrames=0;
  while(graceStage==NativeTrimStage::Adopting&&graceFrames<400){pair(graceGuard,half);graceStage=graceGuard.beginFrame(grace,crystal,crystalDims,1);++graceFrames;}
  CHECK(graceStage==NativeTrimStage::Live&&graceFrames>kNativeTrimAdoptGraceFrames-5&&graceFrames<kNativeTrimAdoptGraceFrames+5);
  NativeFovTrim idleGuard;
  CHECK(idleGuard.beginFrame(grace,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  NativeTrimStage idleStage=NativeTrimStage::Adopting;
  for(unsigned i=0;i<400&&idleStage==NativeTrimStage::Adopting;++i){pair(idleGuard,true65);idleStage=idleGuard.beginFrame(grace,crystal,crystalDims,1);}
  CHECK(idleStage==NativeTrimStage::Adopting&&idleGuard.adoptingFrames()>=400);

  // ---- the nudge (flight 5, 2026-09-16) -----------------------------------
  // The game rebuilt its targets on its own only when the height it read
  // shrank; a width-only change sat at stage 2 for 52 s until an apply. So a
  // change that does not take the height under the floor (the lowest height
  // told since the rebuild last seen) is told two rows under that floor.
  NativeTrimSettings nudge{}; nudge.trimVerticalDeg=10; nudge.trimOuterDeg=10;
  NativeFovTrim nudgeGuard;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::First);
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::None);
  CHECK(land(nudgeGuard,nudge,crystal,crystalDims)==NativeTrimStage::Live);
  const auto base=nudgeGuard.recommended(0);
  CHECK(base.width==3246&&base.height==2754&&nudgeGuard.nudge()==NativeTrimNudge::None);
  // A change of the angles alone, the size untouched: nothing to rebuild.
  nudge.trimVerticalDeg=10.001f;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Same);
  CHECK(nudgeGuard.recommended(0).height==2754);
  pair(nudgeGuard,q(base));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(nudgeGuard,q(base));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Live);
  // Outer 10 to 5: the width grows, the height would stay. Told 2752, both
  // eyes; the temporal pass keeps the old ask until the rebuild lands, then
  // takes the dipped one, which is what the game built.
  nudge.trimOuterDeg=5;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Started);
  CHECK(nudgeGuard.recommended(0).width==3566&&nudgeGuard.recommended(0).height==2752&&nudgeGuard.recommended(1).height==2752);
  CHECK(nudgeGuard.trueRecommended(0).height==2754&&nudgeGuard.nudgeFloor()==2754&&nudgeGuard.treatedFor(0).height==2754);
  pair(nudgeGuard,q(base));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::None);
  pair(nudgeGuard,q(base));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting); // no rebuild yet
  pair(nudgeGuard,q(nudgeGuard.recommended(0)));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Live);
  CHECK(nudgeGuard.recommended(0).height==2752&&nudgeGuard.treatedFor(0).height==2752);
  // A second width-only change dips under the floor re-based at that rebuild.
  nudge.trimOuterDeg=10;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Started);
  CHECK(nudgeGuard.recommended(0).width==3246&&nudgeGuard.recommended(0).height==2750&&nudgeGuard.nudgeFloor()==2752);
  pair(nudgeGuard,q({3566,2752}));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(nudgeGuard,q(nudgeGuard.recommended(0)));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Live);
  // More vertical trim takes the height under the floor: the game rebuilds
  // by itself, the ask is told as it is, and the floor follows it down.
  nudge.trimVerticalDeg=15;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Shrink);
  const auto shrunk=nudgeGuard.recommended(0);
  CHECK(shrunk.height<2750&&shrunk.height==nudgeGuard.trueRecommended(0).height);
  pair(nudgeGuard,q({3246,2750}));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(nudgeGuard,q(shrunk));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Live);
  nudge.trimOuterDeg=5;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Started);
  CHECK(nudgeGuard.recommended(0).height==shrunk.height-2&&nudgeGuard.nudgeFloor()==shrunk.height);
  pair(nudgeGuard,q(shrunk));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(nudgeGuard,q(nudgeGuard.recommended(0)));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Live);
  // Less vertical trim asks for more rows than the floor stood for: never
  // nudged, told as it is, and it waits for the game's next apply. A
  // width-only change during that wait is just as tall. The apply lands the
  // ask, and the floor re-bases there.
  nudge.trimVerticalDeg=10;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Taller);
  CHECK(nudgeGuard.recommended(0).width==3566&&nudgeGuard.recommended(0).height==2754);
  const auto tallerBase=q({3566,shrunk.height-2});
  pair(nudgeGuard,tallerBase);
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  nudge.trimOuterDeg=10;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Taller);
  CHECK(nudgeGuard.recommended(0).width==3246&&nudgeGuard.recommended(0).height==2754);
  pair(nudgeGuard,tallerBase);
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(nudgeGuard,q({3246,2754}));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Live);
  nudge.trimOuterDeg=5;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Started);
  CHECK(nudgeGuard.recommended(0).height==2752&&nudgeGuard.nudgeFloor()==2754);
  // The floor outlives a scene change; a new scene does not rebuild targets.
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::None);
  pair(nudgeGuard,q({3246,2754}));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  pair(nudgeGuard,q(nudgeGuard.recommended(0)));
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Live);
  // Since 2026-09-23 a trim alone is not held for the scene at all: the
  // change is told on the frame it arrives, and the scene arriving later is
  // no change.
  nudge.trimOuterDeg=10;
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::Started);
  CHECK(nudgeGuard.beginFrame(nudge,crystal,crystalDims,1)==NativeTrimStage::Adopting&&nudgeGuard.nudge()==NativeTrimNudge::None&&!nudgeGuard.changed());
  CHECK(nudgeGuard.recommended(0).height==2750&&nudgeGuard.nudgeFloor()==2752);
  // Every width-only change costs two more rows until a shrink or an apply
  // re-bases the floor; past kNativeTrimNudgeMaxPx the ask is told as it is.
  NativeTrimSettings walk{}; walk.trimVerticalDeg=10; walk.trimOuterDeg=10;
  NativeFovTrim walkGuard;
  CHECK(walkGuard.beginFrame(walk,crystal,crystalDims,1)==NativeTrimStage::Adopting);
  CHECK(land(walkGuard,walk,crystal,crystalDims)==NativeTrimStage::Live);
  unsigned started=0,capped=0; uint32_t lastTold=walkGuard.recommended(0).height;
  CHECK(lastTold==2754);
  for(unsigned i=0;i<24;++i) {
    walk.trimOuterDeg=(i&1)?10.f:5.f;
    CHECK(walkGuard.beginFrame(walk,crystal,crystalDims,1)==NativeTrimStage::Adopting);
    const uint32_t told=walkGuard.recommended(0).height;
    if(walkGuard.nudge()==NativeTrimNudge::Started){++started;CHECK(told==lastTold-2);}
    else if(walkGuard.nudge()==NativeTrimNudge::Capped){++capped;CHECK(told==2754&&lastTold==2714);}
    else CHECK(false);
    pair(walkGuard,q({(i&1)?3566u:3246u,lastTold}));
    CHECK(walkGuard.beginFrame(walk,crystal,crystalDims,1)==NativeTrimStage::Adopting);
    pair(walkGuard,q(walkGuard.recommended(0))); // the game's rebuild, or the apply after a capped one
    CHECK(walkGuard.beginFrame(walk,crystal,crystalDims,1)==NativeTrimStage::Live);
    lastTold=told;
  }
  CHECK(started==23&&capped==1);

  // ---- the first ask (2026-09-23) ------------------------------------------
  // Sean's Crystal Super at 3070x3032 and HMD Quality 0.65, from the runtime
  // log of 2026-09-23: the trim knew the frustum at 19:23:50.914 (runtime
  // startup, frame 1), the game first read the size at 19:23:51.000, and a
  // scene gate (the terrain guard's, since removed) held the trim until
  // 19:24:11.212, when the menu's hangar arrived; the game then rebuilt, DLSS
  // and the UI layer with it. His ini: vertical 2, outer 7, nasal 5.
  NativeTrimFrustum sean[2]{{-1.5293f,1.0324f,-1.2648f,1.2648f},{-1.0324f,1.5293f,-1.2648f,1.2648f}};
  NativeTrimDimensions seanDims[2]{{3070,3032},{3070,3032}};
  NativeTrimSettings seanIni{};
  seanIni.trimOuterDeg=7; seanIni.trimNasalDeg=5; seanIni.trimVerticalDeg=2;
  NativeFovTrim askGuard;
  // Frame 1: no scene, no size read yet. Told the trimmed size at once.
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,false)==NativeTrimStage::Adopting&&askGuard.changed());
  CHECK(askGuard.route()==NativeTrimRoute::FirstAsk&&askGuard.nudge()==NativeTrimNudge::First);
  CHECK(askGuard.recommended(0).width==2458&&askGuard.recommended(0).height==2824);
  CHECK(askGuard.recommended(1).width==2458&&askGuard.recommended(1).height==2824);
  CHECK(std::fabs(askGuard.factorWidth()-0.80056f)<.0005f&&std::fabs(askGuard.factorHeight()-0.93126f)<.0005f);
  CHECK(std::fabs(askGuard.appliedOuterDeg(0)-7.f)<.001f&&std::fabs(askGuard.appliedNasalDeg(0)-5.f)<.001f&&std::fabs(askGuard.appliedVerticalDeg(0)-2.f)<.001f);
  // The temporal pass is sized by the ask from the first frame: nothing was
  // ever rendered for another, so DLSS is made once, at the trimmed output.
  CHECK(askGuard.treatedFor(0).width==2458&&askGuard.treatedFor(0).height==2824&&!askGuard.baselineReady());
  // The projection and the placement stay the runtime's until a pair is seen.
  CHECK(askGuard.gameFrustum(0).left==sean[0].left&&askGuard.gameFrustum(0).up==sean[0].up);
  CHECK(askGuard.placementBounds(0).left==0.f&&askGuard.placementBounds(0).bottom==1.f);
  // Frame 2 of the runtime's startup: nothing moves.
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,false)==NativeTrimStage::Adopting&&!askGuard.changed());
  // The game reads 2458x2824 (asked from here on), builds 1597x1835 (0.65,
  // truncated), and renders its first frame with the runtime's projection.
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting&&!askGuard.changed());
  askGuard.noteSubmittedSize(0,1597,1835);
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting&&!askGuard.changed()); // half a pair is not one
  askGuard.noteSubmittedSize(0,1597,1835);askGuard.noteSubmittedSize(1,1597,1835);
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Live&&askGuard.changed());
  CHECK(askGuard.route()==NativeTrimRoute::FirstAsk&&askGuard.adoptingFrames()<6);
  // Live with no scene and no rebuild, at the flight's own target and
  // placement (its stage=3 line, 19:24:12.982).
  const auto seanTarget=askGuard.contentFrustum(0);
  CHECK(std::fabs(seanTarget.left+1.1841f)<.0005f&&std::fabs(seanTarget.right-0.8666f)<.0005f&&
        std::fabs(seanTarget.down+1.1779f)<.0005f&&std::fabs(seanTarget.up-1.1779f)<.0005f);
  const auto seanPlace=askGuard.placementBounds(0);
  CHECK(std::fabs(seanPlace.left-0.1347f)<.0005f&&std::fabs(seanPlace.top-0.0344f)<.0005f&&
        std::fabs(seanPlace.right-0.9353f)<.0005f&&std::fabs(seanPlace.bottom-0.9656f)<.0005f);
  CHECK(std::fabs(askGuard.gameFrustum(0).left+1.1841f)<.0005f&&std::fabs(askGuard.gameFrustum(0).left+1.1841f)<.0005f);
  // The size never moved: 2458x2824 from frame 1 through live.
  CHECK(askGuard.recommended(0).width==2458&&askGuard.recommended(0).height==2824&&askGuard.treatedFor(0).height==2824);
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Live&&!askGuard.changed()); // the scene is no event
  // A change once the game has asked is landed by its rebuild, as before,
  // and still without the scene. Outer 7 to 10 narrows the width alone, so
  // the nudge dips under the floor the first ask left.
  seanIni.trimOuterDeg=10;
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting&&askGuard.route()==NativeTrimRoute::Rebuild);
  CHECK(askGuard.nudge()==NativeTrimNudge::Started&&askGuard.nudgeFloor()==2824&&askGuard.recommended(0).height==2822);
  CHECK(askGuard.treatedFor(0).width==2458&&askGuard.treatedFor(0).height==2824); // what the game holds
  askGuard.noteSubmittedSize(0,1597,1835);askGuard.noteSubmittedSize(1,1597,1835);
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting&&askGuard.baselineReady());
  const auto narrower=askGuard.recommended(0);
  const NativeTrimDimensions narrower65{uint32_t(narrower.width*.65f),uint32_t(narrower.height*.65f)};
  CHECK(narrower.width<2458&&narrower65.height==1834);
  askGuard.noteSubmittedSize(0,narrower65.width,narrower65.height);askGuard.noteSubmittedSize(1,narrower65.width,narrower65.height);
  CHECK(askGuard.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Live);
  seanIni.trimOuterDeg=7;

  // A trim with no other setting is the whole of what is configured now: told
  // from the first frame, before any ask, with nothing else to wait for.
  NativeTrimSettings offTrim{}; offTrim.trimOuterDeg=7; offTrim.trimNasalDeg=5; offTrim.trimVerticalDeg=2;
  NativeFovTrim ot;
  CHECK(ot.beginFrame(offTrim,sean,seanDims,1,false)==NativeTrimStage::Adopting&&ot.route()==NativeTrimRoute::FirstAsk);
  CHECK(ot.recommended(0).width==2458&&ot.recommended(0).height==2824);

  // The frustum not yet known at the first ask: the host holds its frames
  // back (no located geometry), the game reads the runtime's own size and
  // builds for it. The first frame the frustum is known the trim is told,
  // with no scene, and landed as a change: the pair the game holds seeds the
  // baseline, its own rebuild (the height shrank) promotes.
  NativeTrimFrustum unknown[2]{sean[0],sean[1]}; unknown[0].right=std::numeric_limits<float>::quiet_NaN();
  NativeFovTrim late;
  CHECK(late.beginFrame(seanIni,unknown,seanDims,1,false)==NativeTrimStage::Off&&late.route()==NativeTrimRoute::None);
  late.noteSubmittedSize(0,1995,1970);late.noteSubmittedSize(1,1995,1970); // while off: not evidence
  CHECK(late.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting&&late.changed());
  CHECK(late.route()==NativeTrimRoute::Rebuild&&late.recommended(0).width==2458&&late.recommended(0).height==2824);
  CHECK(late.treatedFor(0).width==3070&&late.treatedFor(0).height==3032); // what the game holds until it rebuilds
  CHECK(late.gameFrustum(0).left==sean[0].left&&!late.baselineReady());
  late.noteSubmittedSize(0,1995,1970);late.noteSubmittedSize(1,1995,1970);
  CHECK(late.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting&&late.baselineReady());
  CHECK(late.baselineAsk(0).width==3070&&late.baselineAsk(0).height==3032);
  late.noteSubmittedSize(0,1995,1970);late.noteSubmittedSize(1,1995,1970);
  CHECK(late.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting); // unchanged is never evidence
  late.noteSubmittedSize(0,1597,1835);late.noteSubmittedSize(1,1597,1835);
  CHECK(late.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Live&&late.route()==NativeTrimRoute::Rebuild);
  CHECK(std::fabs(late.contentFrustum(0).left+1.1841f)<.0005f&&late.treatedFor(0).width==2458);
  // The trim not yet known at the first ask (no headset resolved): there is nothing
  // to trim, so nothing is told, and the first frame the trim arrives it is told.
  NativeTrimSettings unresolved{};
  NativeFovTrim lateTrim;
  CHECK(lateTrim.beginFrame(unresolved,sean,seanDims,1,false)==NativeTrimStage::Off);
  CHECK(lateTrim.recommended(0).width==3070&&lateTrim.recommended(0).height==3032);
  CHECK(lateTrim.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Adopting&&lateTrim.route()==NativeTrimRoute::Rebuild);
  CHECK(lateTrim.recommended(0).width==2458&&lateTrim.treatedFor(0).width==3070&&lateTrim.treatedFor(0).height==3032);
  // ... and known before the ask after all (the runtime's second startup
  // frame): still the first ask.
  NativeFovTrim earlyTrim;
  CHECK(earlyTrim.beginFrame(unresolved,sean,seanDims,1,false)==NativeTrimStage::Off);
  CHECK(earlyTrim.beginFrame(seanIni,sean,seanDims,1,false)==NativeTrimStage::Adopting&&earlyTrim.route()==NativeTrimRoute::FirstAsk);
  CHECK(earlyTrim.treatedFor(0).width==2458&&earlyTrim.treatedFor(0).height==2824&&earlyTrim.nudge()==NativeTrimNudge::First);
  // A stand-down ends a first ask like any other adoption.
  earlyTrim.standDown();
  CHECK(earlyTrim.beginFrame(seanIni,sean,seanDims,1,true)==NativeTrimStage::Inert&&earlyTrim.route()==NativeTrimRoute::None);
  CHECK(earlyTrim.recommended(0).width==3070&&earlyTrim.recommended(0).height==3032);
  std::printf("native_trim_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
