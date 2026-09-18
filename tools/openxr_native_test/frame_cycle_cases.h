#pragma once
#include "../../src/openxr/frame_cycle_stats.h"

namespace edvr::openxr::test {
template<class Check> void runFrameCycleCases(Check check) {
  auto cStorage=std::make_unique<FrameCycleStats>();auto& c=*cStorage; FrameCycleStats::Shape shape{};shape.width[0]=shape.width[1]=2481;
  shape.height[0]=shape.height[1]=2121;shape.outputWidth[0]=shape.outputWidth[1]=3072;
  shape.outputHeight[0]=shape.outputHeight[1]=3264;shape.generation=7;shape.featureEpoch=3;shape.shouldRender=1;shape.sceneReady=1;
  auto wait=[&](uint64_t seq,uint64_t us,uint64_t ms,uint32_t thread=11){auto t=c.waitCallerBegin(us,thread);c.waitOwnerBegin(t,us+100);c.waitOwnerEnd(t,us+300);c.waitCallerEnd(t,seq,us+500,ms,thread,shape,true);};
  auto submit=[&](uint64_t seq,unsigned eye,uint64_t us,uint32_t thread=11){auto t=c.submitCallerBegin(eye,us,thread);c.submitOwnerBegin(t,us+100);c.submitOwnerEnd(t,us+300);c.submitCallerEnd(t,eye,seq,us+500,thread,0.25,true);};
  wait(100,1000,1000);submit(100,1,3000);submit(100,0,5000);wait(101,16000,16000);
  check(c.firstComplete(),"frame-cycle reverse-eye complete");
  // Delayed owner body remains nested while the exclusive roundtrip closes.
  submit(101,0,18000);submit(101,1,24000);wait(102,31000,31000);
  check(c.missingForTest()[FrameCycleStats::BadClock]==0,"frame-cycle delayed owner ordered");
  FrameCycleStats::Report report{};check(c.takeReport(report),"frame-cycle 30 second report reachable");
  check(report.valid==2&&report.firstSequence==100&&report.lastSequence==101,"frame-cycle sequence coverage");
  check(report.residual.mean==0.0&&report.cycle.mean>0,"frame-cycle paired partition closes");
  check(report.cycle.mean==15.0&&report.beforeFirst.mean==1.5&&report.firstSubmit.mean==0.5&&
    report.secondSubmit.mean==0.5&&report.nextWait.mean==0.5,"frame-cycle known exclusive phases");
  check(report.waitOwner.mean==0.2&&report.submitOwner[0].mean==0.2&&report.renderPark[1].mean==0.25,
    "frame-cycle nested owner and render spans");
  check(report.shape.generation==7&&report.shape.featureEpoch==3,"frame-cycle coherent scope reported");
  // Deliberately different timing/provider sequence is irrelevant: submits use
  // the compositor sequence published by the successful Wait return.
  submit(102,1,33000);submit(102,0,35000);wait(103,62000,62000);

  auto invalidStorage=std::make_unique<FrameCycleStats>();auto& invalid=*invalidStorage;auto wt=invalid.waitCallerBegin(1000,1);invalid.waitOwnerBegin(wt,1100);
  invalid.waitOwnerEnd(wt,1200);invalid.waitCallerEnd(wt,1,1300,1,1,shape,true);
  auto a=invalid.submitCallerBegin(0,1400,1);check(invalid.submitCallerBegin(1,1450,1)==0,"frame-cycle rejects reentrant submit");
  invalid.submitOwnerBegin(a,1500);invalid.submitOwnerEnd(a,1600);invalid.submitCallerEnd(a,0,1,1700,2,0.1,true);
  check(invalid.missingForTest()[FrameCycleStats::WrongThread]>0,"frame-cycle wrong caller rejected");
  invalid.noteDirectOwner();check(invalid.missingForTest()[FrameCycleStats::DirectOwner]>0,"frame-cycle direct owner counted");
  auto duplicateStorage=std::make_unique<FrameCycleStats>();auto& duplicate=*duplicateStorage;auto d=duplicate.waitCallerBegin(1000,1);duplicate.waitOwnerBegin(d,1010);duplicate.waitOwnerEnd(d,1020);duplicate.waitCallerEnd(d,9,1030,1,1,shape,true);
  auto s=duplicate.submitCallerBegin(0,1100,1);duplicate.submitOwnerBegin(s,1110);duplicate.submitOwnerEnd(s,1120);duplicate.submitCallerEnd(s,0,9,1130,1,0.01,true);
  check(duplicate.submitCallerBegin(0,1200,1)==0&&duplicate.missingForTest()[FrameCycleStats::DuplicateEye]>0,"frame-cycle duplicate eye rejected");
  FrameCycleStats::Shape changed=shape;changed.pacing=1;

  auto zeroGapStorage=std::make_unique<FrameCycleStats>();auto& zeroGap=*zeroGapStorage;auto z=zeroGap.waitCallerBegin(1000,4);zeroGap.waitOwnerBegin(z,1010);zeroGap.waitOwnerEnd(z,1020);zeroGap.waitCallerEnd(z,20,1030,1,4,shape,true);
  auto z0=zeroGap.submitCallerBegin(1,1100,4);zeroGap.submitOwnerBegin(z0,1110);zeroGap.submitOwnerEnd(z0,1120);zeroGap.submitCallerEnd(z0,1,20,1130,4,0.01,true);
  auto z1=zeroGap.submitCallerBegin(0,1200,4);zeroGap.submitOwnerBegin(z1,1210);zeroGap.submitOwnerEnd(z1,1220);zeroGap.submitCallerEnd(z1,0,20,1230,4,0.01,true);
  auto zw=zeroGap.waitCallerBegin(1230,4);zeroGap.waitOwnerBegin(zw,1240);zeroGap.waitOwnerEnd(zw,1250);zeroGap.waitCallerEnd(zw,21,1260,2,4,shape,true);
  check(zeroGap.firstComplete()&&zeroGap.missingForTest()[FrameCycleStats::BadClock]==0,"frame-cycle zero post-submit gap valid");
  auto transition=zeroGap.waitCallerBegin(1270,4);zeroGap.waitOwnerBegin(transition,1280);zeroGap.waitOwnerEnd(transition,1290);zeroGap.waitCallerEnd(transition,22,1300,3,4,changed,true);
  FrameCycleStats::Report transitionReport{};check(zeroGap.takeReport(transitionReport)&&transitionReport.missing[FrameCycleStats::ShapeChange]>0,"frame-cycle transition flushes prior scope");
  auto badSeq=zeroGap.submitCallerBegin(0,1400,4);zeroGap.submitOwnerBegin(badSeq,1410);zeroGap.submitOwnerEnd(badSeq,1420);zeroGap.submitCallerEnd(badSeq,0,9999,1430,4,0.01,true);
  check(zeroGap.missingForTest()[FrameCycleStats::BadSequence]>0,"frame-cycle mismatched sequence rejected");

  auto inertStorage=std::make_unique<FrameCycleStats>();auto& inert=*inertStorage;auto i0=inert.waitCallerBegin(1000,8);inert.waitCallerEnd(i0,1,1100,1,8,shape,false);
  auto i1=inert.waitCallerBegin(2000,8);inert.waitCallerEnd(i1,2,2100,31002,8,shape,false);
  FrameCycleStats::Report inertReport{};check(inert.takeReport(inertReport)&&inertReport.valid==0&&inertReport.admitted>0,"frame-cycle zero-valid window reports failures");
}
}
