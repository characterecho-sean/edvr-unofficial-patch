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
  FrameCycleStats::Completed zeroCompleted{};check(zeroGap.takeCompleted(zeroCompleted)&&zeroCompleted.sequence==20,"completed cycle extracted once");
  auto transition=zeroGap.waitCallerBegin(1270,4);zeroGap.waitOwnerBegin(transition,1280);zeroGap.waitOwnerEnd(transition,1290);zeroGap.waitCallerEnd(transition,22,1300,3,4,changed,true);
  check(!zeroGap.takeCompleted(zeroCompleted),"scope-changed cycle has no completed marker");
  FrameCycleStats::Report transitionReport{};check(zeroGap.takeReport(transitionReport)&&transitionReport.missing[FrameCycleStats::ShapeChange]>0,"frame-cycle transition flushes prior scope");
  auto badSeq=zeroGap.submitCallerBegin(0,1400,4);zeroGap.submitOwnerBegin(badSeq,1410);zeroGap.submitOwnerEnd(badSeq,1420);zeroGap.submitCallerEnd(badSeq,0,9999,1430,4,0.01,true);
  check(zeroGap.missingForTest()[FrameCycleStats::BadSequence]>0,"frame-cycle mismatched sequence rejected");

  auto makeTrace=[](){EdvrNativePresentTrace t{};t.size=sizeof(t);t.version=EDVR_NATIVE_PRESENT_TRACE_VERSION_1;t.generation=7;t.totalObserved=1;return t;};
  auto startFrame=[&](FrameCycleStats& stats,uint64_t seq,uint64_t base,uint64_t ms,uint32_t thread=4){
    auto w=stats.waitCallerBegin(base,thread);stats.waitOwnerBegin(w,base+10);stats.waitOwnerEnd(w,base+20);stats.waitCallerEnd(w,seq,base+30,ms,thread,shape,true);
    for(unsigned eye=0;eye<2;++eye){const auto at=base+100+eye*100;auto s=stats.submitCallerBegin(eye,at,thread);stats.submitOwnerBegin(s,at+10);stats.submitOwnerEnd(s,at+20);stats.submitCallerEnd(s,eye,seq,at+30,thread,0.01,true);}
  };
  auto closeStart=[&](FrameCycleStats& stats,uint64_t seq,uint64_t base,uint64_t ms,const EdvrNativePresentTrace* trace,uint32_t thread=4){
    auto w=stats.waitCallerBegin(base,thread,trace);stats.waitOwnerBegin(w,base+10);stats.waitOwnerEnd(w,base+20);stats.waitCallerEnd(w,seq,base+30,ms,thread,shape,true);
    for(unsigned eye=0;eye<2;++eye){const auto at=base+100+eye*100;auto s=stats.submitCallerBegin(eye,at,thread);stats.submitOwnerBegin(s,at+10);stats.submitOwnerEnd(s,at+20);stats.submitCallerEnd(s,eye,seq,at+30,thread,0.01,true);}
  };

  auto exactStorage=std::make_unique<FrameCycleStats>();auto& exact=*exactStorage;startFrame(exact,1,1000,1);
  auto request=exact.postRequest();check(request.sequence==1&&request.beginUs==1230&&request.thread==4,"post accounting request identifies completed stereo interval");
  auto one=makeTrace();one.count=1;one.spans[0]={1300,1310,1360,1370,1400,4,0,0,0};
  exact.noteHandoff(1260,1360,1,4,true);exact.noteHandoff(1260,1360,1,4,true);closeStart(exact,2,1500,2,&one);
  FrameCycleStats::Completed completed{};
  check(exact.takeCompleted(completed)&&completed.sequence==1&&completed.generation==7&&completed.featureEpoch==3&&
    completed.waitReturnUs==1030&&completed.secondSubmitReturnUs==1230&&completed.nextWaitEntryUs==1500&&
    completed.nextWaitReturnUs==1530&&completed.presentBeginUs==1300&&completed.presentEndUs==1400&&
    completed.callerThread==4&&completed.nextWaitThread==4&&completed.sceneReady==1&&
    completed.postValid&&completed.singlePresent&&completed.postUnavailable==FrameCycleStats::PostAvailable,
    "completed event exposes admitted same-cycle bounds");
  check(!exact.takeCompleted(completed),"completed event extraction is one-shot");
  auto missingRequest=exact.postRequest();check(missingRequest.sequence==2&&missingRequest.beginUs==1730,"post accounting request advances with accepted cycle");
  closeStart(exact,3,2500,31002,nullptr);
  check(exact.takeCompleted(completed)&&completed.sequence==2&&!completed.postValid&&!completed.singlePresent&&
    completed.postUnavailable==FrameCycleStats::ProviderMissing&&completed.presentBeginUs==0&&completed.presentEndUs==0,
    "completed event reports unavailable Present without fabricated bounds");
  FrameCycleStats::Report exactReport{};check(exact.takeReport(exactReport),"post accounting report reachable");
  check(exactReport.valid==2&&exactReport.postValid==1&&exactReport.postUnavailable[FrameCycleStats::ProviderMissing]==1&&exactReport.firstPostSequence==1,
    "post accounting distinguishes coverage from unavailable provider");
  check(exactReport.rawPresent.mean==0.05&&exactReport.edvrBeforePresent.mean==0.01&&exactReport.edvrAfterPresent.mean==0.01&&
    exactReport.edvrPresent.mean==0.02&&exactReport.trailingCallback.mean==0.03&&exactReport.outsidePresent.mean==0.17&&exactReport.postResidual.mean==0.0,
    "post accounting exact single-Present phase partition");
  check(exactReport.singlePresentValid==1&&exactReport.beforePresent.mean==0.07&&exactReport.afterPresent.mean==0.10,
    "post accounting single-Present before and after intervals");
  check(exactReport.postGap.mean==0.27&&exactReport.handoffValid==2&&exactReport.handoffCount.mean==1.0&&exactReport.handoffNested.mean==0.05,
    "post accounting nested handoff unions duplicates and includes measured zero");

  auto staleStorage=std::make_unique<FrameCycleStats>();auto& stale=*staleStorage;startFrame(stale,1,1000,1);closeStart(stale,2,2000,1,nullptr);
  auto failedWait=stale.waitCallerBegin(3000,4);stale.waitCallerEnd(failedWait,3,3010,1,4,shape,false);
  check(!stale.takeCompleted(completed),"failed next wait clears unconsumed completed marker");
  startFrame(stale,4,4000,1);closeStart(stale,5,5000,1,nullptr);stale.reset();
  check(!stale.takeCompleted(completed),"explicit reset clears unconsumed completed marker");

  auto boundedStorage=std::make_unique<FrameCycleStats>();auto& bounded=*boundedStorage;startFrame(bounded,1,1000,1);
  for(uint64_t seq=2;seq<=FrameCycleStats::capacity+1;++seq) {
    closeStart(bounded,seq,seq*1000,1,nullptr);check(bounded.takeCompleted(completed),"bounded admitted cycle produces completed marker");
  }
  closeStart(bounded,FrameCycleStats::capacity+2,uint64_t(FrameCycleStats::capacity+2)*1000,1,nullptr);
  FrameCycleStats::Report boundedReport{};
  check(!bounded.takeCompleted(completed)&&bounded.takeReport(boundedReport)&&boundedReport.missing[FrameCycleStats::Overflow]==1,
    "overflow-dropped sample produces no completed marker");

  auto casesStorage=std::make_unique<FrameCycleStats>();auto& cases=*casesStorage;startFrame(cases,10,1000,1);
  auto zero=makeTrace();closeStart(cases,11,2000,2,&zero);
  auto many=makeTrace();many.count=2;many.spans[0]={2240,2245,2255,2265,2280,4,1,0,0};many.spans[1]={2330,2340,2380,2390,2410,4,0,0,0};closeStart(cases,12,3000,3,&many);
  auto partial=makeTrace();partial.count=1;partial.spans[0]={3229,3240,3250,3260,3270,4,0,0,0};closeStart(cases,13,4000,4,&partial);
  check(cases.takeCompleted(completed)&&completed.sequence==12&&!completed.postValid&&
    completed.postUnavailable==FrameCycleStats::PartialPresent&&completed.presentBeginUs==0&&completed.presentEndUs==0,
    "completed event preserves rejected partial Present status");
  auto crossThread=makeTrace();crossThread.count=1;crossThread.spans[0]={4240,4250,4260,4270,4280,99,0,0,0};closeStart(cases,14,5000,5,&crossThread);
  auto overflow=makeTrace();overflow.overflow=1;closeStart(cases,15,6000,6,&overflow);
  auto noGeneration=makeTrace();noGeneration.generation=8;closeStart(cases,16,7000,7,&noGeneration);
  auto badVersion=makeTrace();badVersion.version=99;closeStart(cases,17,8000,8,&badVersion);
  auto badSize=makeTrace();badSize.size=sizeof(badSize)-1;closeStart(cases,18,9000,9,&badSize);
  auto failed=makeTrace();failed.count=1;failed.spans[0]={9240,9250,9260,9270,9280,4,0,0,-1};closeStart(cases,19,10000,10,&failed);
  auto testPresent=makeTrace();testPresent.count=1;testPresent.spans[0]={10240,10250,10260,10270,10280,4,0,1,0};closeStart(cases,20,11000,31002,&testPresent);
  FrameCycleStats::Report casesReport{};check(cases.takeReport(casesReport),"post accounting rejection report reachable");
  check(casesReport.postValid==2&&casesReport.zeroPresentValid==1&&casesReport.multiplePresentValid==1&&casesReport.singlePresentValid==0&&
    casesReport.presentCount.mean==1.0&&casesReport.rawPresent.mean==0.025&&casesReport.outsidePresent.mean==0.71&&casesReport.postGap.mean==0.77&&casesReport.syncNonzeroPresent==1,
    "post accounting zero and multiple Presents are explicit distributions");
  check(casesReport.postUnavailable[FrameCycleStats::PartialPresent]==1&&casesReport.postUnavailable[FrameCycleStats::WrongPresentThread]==1&&
    casesReport.postUnavailable[FrameCycleStats::LostPresentHistory]==1&&casesReport.postUnavailable[FrameCycleStats::BadProviderGeneration]==1&&
    casesReport.postUnavailable[FrameCycleStats::BadProviderVersion]==1&&casesReport.postUnavailable[FrameCycleStats::BadProviderSize]==1&&
    casesReport.postUnavailable[FrameCycleStats::FailedPresent]==1&&casesReport.postUnavailable[FrameCycleStats::TestPresent]==1,
    "post accounting rejects partial cross-thread lost failed and test Present data");
  check(casesReport.valid==10&&casesReport.postValid==2,"post accounting failures retain base cycle validity");

  auto handoffStorage=std::make_unique<FrameCycleStats>();auto& handoff=*handoffStorage;startFrame(handoff,30,1000,1);
  handoff.noteHandoff(1240,1250,999,4,true);handoff.noteHandoff(1240,1250,30,99,true);handoff.noteHandoff(1240,1250,30,4,false);
  for(unsigned i=0;i<17;++i)handoff.noteHandoff(1260+i,1260+i,30,4,true);
  auto handoffTrace=makeTrace();handoffTrace.count=1;handoffTrace.spans[0]={1240,1250,1260,1270,1280,4,0,0,0};
  auto handoffWait=handoff.waitCallerBegin(2000,4,&handoffTrace);handoff.noteHandoff(1990,2010,30,4,true);
  handoff.waitOwnerBegin(handoffWait,2010);handoff.waitOwnerEnd(handoffWait,2020);handoff.waitCallerEnd(handoffWait,31,2030,2,4,shape,true);
  for(unsigned eye=0;eye<2;++eye){const auto at=2100+eye*100;auto s=handoff.submitCallerBegin(eye,at,4);handoff.submitOwnerBegin(s,at+10);handoff.submitOwnerEnd(s,at+20);handoff.submitCallerEnd(s,eye,31,at+30,4,0.01,true);}
  closeStart(handoff,32,3000,31002,nullptr);FrameCycleStats::Report handoffReport{};check(handoff.takeReport(handoffReport),"handoff rejection report reachable");
  check(handoffReport.handoffMissing==1&&handoffReport.handoffInvalid==3&&handoffReport.handoffOverflow==1&&handoffReport.handoffValid==1,
    "post accounting counts missing invalid and overflow handoffs without corrupting valid samples");

  auto inertStorage=std::make_unique<FrameCycleStats>();auto& inert=*inertStorage;auto i0=inert.waitCallerBegin(1000,8);inert.waitCallerEnd(i0,1,1100,1,8,shape,false);
  auto i1=inert.waitCallerBegin(2000,8);inert.waitCallerEnd(i1,2,2100,31002,8,shape,false);
  FrameCycleStats::Report inertReport{};check(inert.takeReport(inertReport)&&inertReport.valid==0&&inertReport.admitted>0,"frame-cycle zero-valid window reports failures");
}
}
