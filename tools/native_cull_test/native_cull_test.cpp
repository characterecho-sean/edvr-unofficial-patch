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

  NativeCullSettings bad=s; bad.signatureCount=1; bad.signatures[0]={999,999};
  NativeCullGuard gated; CHECK(gated.beginFrame(bad,fs,ds,true,1)==NativeCullStage::Inert);
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
  NativeCullFrustum equal[2]{{-1,1,-1,1},{-1,1,-1,1}};NativeCullGuard noMargin;
  CHECK(noMargin.beginFrame(s,equal,ds,true,1)==NativeCullStage::Inert);
  CHECK(noMargin.beginFrame(s,equal,ds,true,1)==NativeCullStage::Inert&&!noMargin.changed());
  std::printf("native_cull_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
