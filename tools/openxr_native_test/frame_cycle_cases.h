#pragma once
#include <cstring>
#include <string>
#include "../../src/openxr/frame_cycle_stats.h"
#include "../../src/openxr/long_cycle_line.h"

namespace edvr::openxr::test {
template<class Check> void runFrameCycleCases(Check check) {
  auto cStorage=std::make_unique<FrameCycleStats>();auto& c=*cStorage; FrameCycleStats::Shape shape{};shape.width[0]=shape.width[1]=2481;
  shape.height[0]=shape.height[1]=2121;shape.outputWidth[0]=shape.outputWidth[1]=3072;
  shape.outputHeight[0]=shape.outputHeight[1]=3264;shape.generation=7;shape.featureEpoch=3;shape.shouldRender=1;shape.sceneReady=1;
  {
    FrameCycleStats linked;FrameCycleStats::Completed completed{};
    auto linkedWait=[&](uint64_t xr,uint64_t gpu,uint64_t base,const FrameCycleStats::Shape& scope,bool ok=true,uint32_t caller=4){
      auto w=linked.waitCallerBegin(base,4);linked.waitOwnerBegin(w,base+10);linked.waitOwnerEnd(w,base+20);
      linked.waitCallerEnd(w,xr,base+30,base/1000,caller,scope,ok,gpu);
    };
    auto linkedStereo=[&](uint64_t xr,uint64_t base){for(unsigned eye=0;eye<2;++eye){
      const auto at=base+100+eye*100;auto s=linked.submitCallerBegin(eye,at,4);
      linked.submitOwnerBegin(s,at+10);linked.submitOwnerEnd(s,at+20);linked.submitCallerEnd(s,eye,xr,at+30,4,0.01,true);
    }};
    linkedWait(100,9001,1000,shape);linkedStereo(100,1000);linkedWait(105,9017,2000,shape);
    check(linked.takeCompleted(completed)&&completed.sequence==100&&completed.producerSequence==9001,
      "completed cycle retains its explicit producer token, not XR or next-wait token");
    linkedStereo(105,2000);linkedWait(106,0,3000,shape);
    check(linked.takeCompleted(completed)&&completed.sequence==105&&completed.producerSequence==9017,
      "divergent XR and producer counters remain independently linked");
    linkedStereo(106,3000);linkedWait(107,9100,4000,shape);
    check(linked.takeCompleted(completed)&&completed.sequence==106&&completed.producerSequence==0,
      "unavailable producer mapping never inherits a previous token");
    linkedStereo(107,4000);auto nextGeneration=shape;++nextGeneration.generation;
    linkedWait(200,9200,5000,nextGeneration);
    check(!linked.takeCompleted(completed),"generation transition drops previous cycle producer identity");
    linkedStereo(200,5000);linkedWait(201,9201,6000,nextGeneration);
    check(linked.takeCompleted(completed)&&completed.sequence==200&&completed.producerSequence==9200,
      "new generation has only its newly opened producer mapping");
    linked.reset();check(!linked.takeCompleted(completed),"reset drops completed producer mapping");
    linkedWait(300,9300,7000,shape);linkedStereo(300,7000);linkedWait(301,9301,8000,shape,false);
    check(!linked.takeCompleted(completed),"failed wait produces no linked cycle");
    linkedWait(302,9302,9000,shape);linkedStereo(302,9000);linkedWait(303,9303,10000,shape,true,5);
    check(!linked.takeCompleted(completed),"foreign caller produces no linked cycle");
    auto loading=shape;loading.sceneReady=0;
    linkedWait(400,9400,11000,loading);linkedStereo(400,11000);linkedWait(401,9401,12000,loading);
    check(linked.takeCompleted(completed)&&completed.producerSequence==0,"loading cycle maps producer as unavailable");
    linked.reset();auto withheld=shape;withheld.shouldRender=0;
    linkedWait(500,9500,13000,withheld);linkedStereo(500,13000);linkedWait(501,9501,14000,withheld);
    check(linked.takeCompleted(completed)&&completed.producerSequence==0,"withheld frame maps producer as unavailable");
  }
  auto wait=[&](uint64_t seq,uint64_t us,uint64_t ms,uint32_t thread=11){auto t=c.waitCallerBegin(us,thread);c.waitOwnerBegin(t,us+100);c.waitOwnerEnd(t,us+300);c.waitCallerEnd(t,seq,us+500,ms,thread,shape,true);};
  auto submit=[&](uint64_t seq,unsigned eye,uint64_t us,uint32_t thread=11){auto t=c.submitCallerBegin(eye,us,thread);c.submitOwnerBegin(t,us+100);c.submitOwnerEnd(t,us+300);c.submitCallerEnd(t,eye,seq,us+500,thread,0.25,true);};
  double callerWork=-1;
  wait(100,1000,1000);
  check(!c.callerWorkForCurrent(callerWork),"frame-cycle caller work absent before any cycle completes");
  submit(100,1,3000);submit(100,0,5000);wait(101,16000,16000);
  check(c.firstComplete(),"frame-cycle reverse-eye complete");
  // EdvrNativeTimingFrame v5 callerWorkMs: cycle 15.0 - next wait 0.5 = the
  // wait return at 1500 us to the next wait's entry at 16000 us, handed to
  // the cycle that next wait return opened (101), whose frame publishes it.
  check(c.callerWorkForCurrent(callerWork)&&std::fabs(callerWork-14.5)<1e-9,
    "frame-cycle caller work is cycle minus next wait, for the cycle in progress");
  // Delayed owner body remains nested while the exclusive roundtrip closes.
  submit(101,0,18000);submit(101,1,24000);wait(102,31000,31000);
  check(c.missingForTest()[FrameCycleStats::BadClock]==0,"frame-cycle delayed owner ordered");
  check(c.callerWorkForCurrent(callerWork)&&std::fabs(callerWork-14.5)<1e-9,
    "frame-cycle caller work follows each completed cycle");
  FrameCycleStats::Report report{};check(c.takeReport(report),"frame-cycle 30 second report reachable");
  check(report.valid==2&&report.firstSequence==100&&report.lastSequence==101,"frame-cycle sequence coverage");
  check(report.residual.mean==0.0&&report.cycle.mean>0,"frame-cycle paired partition closes");
  check(report.cycle.mean==15.0&&report.beforeFirst.mean==1.5&&report.firstSubmit.mean==0.5&&
    report.secondSubmit.mean==0.5&&report.nextWait.mean==0.5,"frame-cycle known exclusive phases");
  // Both admitted cycles (100, 101) run this fixture's identical timing
  // deltas, so their cycle time is exactly 15.0 twice over: p99 and max
  // equal mean/p50/p95 here, not a coincidence to generalize from.
  check(report.cycle.p99==15.0&&report.cycle.max==15.0,"frame-cycle p99 and max reach the Dist a two-sample window produces");
  check(report.waitOwner.mean==0.2&&report.submitOwner[0].mean==0.2&&report.renderPark[1].mean==0.25,
    "frame-cycle nested owner and render spans");
  check(report.shape.generation==7&&report.shape.featureEpoch==3,"frame-cycle coherent scope reported");
  // Deliberately different timing/provider sequence is irrelevant: submits use
  // the compositor sequence published by the successful Wait return.
  submit(102,1,33000);submit(102,0,35000);wait(103,62000,62000);
  check(c.callerWorkForCurrent(callerWork)&&std::fabs(callerWork-30.5)<1e-9,
    "frame-cycle caller work stands across a 30-second window boundary");

  auto invalidStorage=std::make_unique<FrameCycleStats>();auto& invalid=*invalidStorage;auto wt=invalid.waitCallerBegin(1000,1);invalid.waitOwnerBegin(wt,1100);
  invalid.waitOwnerEnd(wt,1200);invalid.waitCallerEnd(wt,1,1300,1,1,shape,true);
  auto a=invalid.submitCallerBegin(0,1400,1);check(invalid.submitCallerBegin(1,1450,1)==0,"frame-cycle rejects reentrant submit");
  invalid.submitOwnerBegin(a,1500);invalid.submitOwnerEnd(a,1600);invalid.submitCallerEnd(a,0,1,1700,2,0.1,true);
  check(invalid.missingForTest()[FrameCycleStats::WrongThread]>0,"frame-cycle wrong caller rejected");
  invalid.noteDirectOwner();check(invalid.missingForTest()[FrameCycleStats::DirectOwner]>0,"frame-cycle direct owner counted");
  check(!invalid.callerWorkForCurrent(callerWork),"frame-cycle a failed cycle carries no caller work");
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
  check(zeroGap.callerWorkForCurrent(callerWork)&&std::fabs(callerWork-0.2)<1e-9,"frame-cycle zero post-submit gap caller work");
  auto transition=zeroGap.waitCallerBegin(1270,4);zeroGap.waitOwnerBegin(transition,1280);zeroGap.waitOwnerEnd(transition,1290);zeroGap.waitCallerEnd(transition,22,1300,3,4,changed,true);
  check(!zeroGap.takeCompleted(zeroCompleted),"scope-changed cycle has no completed marker");
  check(!zeroGap.callerWorkForCurrent(callerWork),"frame-cycle a scope change hands on no caller work");
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
  // native_long_cycle: the same completed event also
  // carries this ONE cycle's own phase partition, not a window Dist. Derived
  // by hand from the fixture above: wait 1030, before-first submit at 1100
  // (0.07 ms), first submit returns 1130 (0.03 ms round trip, 0.01 ms owner
  // body), second submit opens 1200 (0.07 ms between), returns 1230 (0.03 ms
  // round trip, 0.01 ms owner body), next wait enters 1500 (0.27 ms after
  // the second submit) and returns 1530 (0.03 ms round trip, 0.01 ms owner
  // body) -- cycle 1530-1030 = 0.5 ms, the same six parts reportFrameCycles
  // prints as native_frame_cycle_phase summing exactly (residual 0).
  check(std::fabs(completed.cycleMs-0.5)<1e-9&&std::fabs(completed.beforeFirstMs-0.07)<1e-9&&
    std::fabs(completed.firstSubmitMs-0.03)<1e-9&&std::fabs(completed.betweenEyesMs-0.07)<1e-9&&
    std::fabs(completed.secondSubmitMs-0.03)<1e-9&&std::fabs(completed.afterSecondMs-0.27)<1e-9&&
    std::fabs(completed.nextWaitMs-0.03)<1e-9&&std::fabs(completed.waitOwnerMs-0.01)<1e-9&&
    std::fabs(completed.submitOwnerMs[0]-0.01)<1e-9&&std::fabs(completed.submitOwnerMs[1]-0.01)<1e-9&&
    std::fabs(completed.renderParkMs[0]-0.01)<1e-9&&std::fabs(completed.renderParkMs[1]-0.01)<1e-9,
    "completed event carries this one cycle's own phase partition");
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

  // ---- native_long_cycle's Present split ----------------------------------------------------------------
  // post_second_submit_to_next_wait cut at Elite's Present from the graphics half's trace, one cycle at a
  // time. The fixture is the `exact` one above: second Submit returns at 1230, the hook runs 1300..1400 with
  // its marks at 1310/1360/1370, the next wait enters at 1500. By hand: pre_present 0.07, present_hook 0.10
  // (before the real call 0.01, the real Present 0.05, after it 0.01, the render callback 0.03),
  // post_present 0.10 -- and 0.07+0.10+0.10 is the 0.27 the un-split phase has always reported.
  const auto closeTo=[](double a,double b){return std::fabs(a-b)<1e-9;};
  const auto splitCycle=[&](const EdvrNativePresentTrace* trace,FrameCycleStats::Completed& out){
    auto storage=std::make_unique<FrameCycleStats>();auto& stats=*storage;
    startFrame(stats,40,1000,1);closeStart(stats,41,1500,2,trace);
    return stats.takeCompleted(out)&&out.sequence==40;
  };
  FrameCycleStats::Completed splitDone{};
  auto splitOne=makeTrace();splitOne.count=1;splitOne.spans[0]={1300,1310,1360,1370,1400,4,0,0,0};
  check(splitCycle(&splitOne,splitDone)&&splitDone.presentSplit&&splitDone.presentCount==1&&
    closeTo(splitDone.prePresentMs,0.07)&&closeTo(splitDone.presentHookMs,0.10)&&closeTo(splitDone.postPresentMs,0.10),
    "long-cycle Present split: pre_present, present_hook and post_present of one valid Present");
  check(closeTo(splitDone.hookBeforeRealMs,0.01)&&closeTo(splitDone.hookRealMs,0.05)&&closeTo(splitDone.hookAfterRealMs,0.01)&&
    closeTo(splitDone.hookCallbackMs,0.03),
    "long-cycle Present split: the hook's own four parts, from the trace's five marks");
  check(closeTo(splitDone.prePresentMs+splitDone.presentHookMs+splitDone.postPresentMs,splitDone.afterSecondMs)&&
    closeTo(splitDone.hookBeforeRealMs+splitDone.hookRealMs+splitDone.hookAfterRealMs+splitDone.hookCallbackMs,splitDone.presentHookMs),
    "long-cycle Present split adds up to post_second_submit_to_next_wait, and the hook's parts to the hook");
  char splitLine[1024]{};
  const size_t splitLength=formatLongCycleLine(splitLine,sizeof(splitLine),1234,11.1111,splitDone);
  check(std::string(splitLine)==
    "native_long_cycle,sequence=1234,cycle_ms=0.5000,period_ms=11.1111,"
    "game_before_first_submit=0.0700,first_submit_roundtrip=0.0300,first_submit_owner_body=0.0100,"
    "between_eye_calls=0.0700,second_submit_roundtrip=0.0300,second_submit_owner_body=0.0100,"
    "first_submit_render_park=0.0100,second_submit_render_park=0.0100,"
    "post_second_submit_to_next_wait=0.2700,present_split=ok,pre_present=0.0700,present_hook=0.1000,"
    "hook_before_real=0.0100,hook_real_present=0.0500,hook_after_real=0.0100,hook_render_callback=0.0300,"
    "post_present=0.1000,next_wait_roundtrip=0.0300,next_wait_owner_body=0.0100,units=wall_ms"&&
    splitLength==std::strlen(splitLine),
    "native_long_cycle line: every existing field unchanged and in place, the Present split added beside the phase it cuts");

  // A cycle without a valid single-Present trace says why and prints none of the split's numbers: never a
  // zero that reads as a free hook.
  struct SplitCase {const char* name;const char* reason;bool valid;uint32_t count;};
  const auto reasonOf=[&](const EdvrNativePresentTrace* trace,const SplitCase& expected){
    FrameCycleStats::Completed done{};
    const bool got=splitCycle(trace,done);
    char text[1024]{};formatLongCycleLine(text,sizeof(text),7,11.0,done);
    const bool ok=got&&!done.presentSplit&&done.postValid==expected.valid&&done.presentCount==expected.count&&
      std::strstr(text,(std::string("present_split=")+expected.reason+",next_wait_roundtrip=").c_str())&&
      !std::strstr(text,"pre_present=")&&!std::strstr(text,"present_hook=")&&!std::strstr(text,"post_present=")&&
      std::strstr(text,"post_second_submit_to_next_wait=0.2700")&&std::strstr(text,"units=wall_ms");
    check(ok,expected.name);
  };
  reasonOf(nullptr,{"long-cycle Present split absent without the graphics half's provider","provider_missing",false,0});
  auto splitZero=makeTrace();
  reasonOf(&splitZero,{"long-cycle Present split absent when the cycle held no Present","no_present",true,0});
  auto splitTwo=makeTrace();splitTwo.count=2;
  splitTwo.spans[0]={1240,1245,1255,1265,1280,4,0,0,0};splitTwo.spans[1]={1330,1340,1380,1390,1410,4,0,0,0};
  reasonOf(&splitTwo,{"long-cycle Present split absent for two Presents in one cycle","multiple_present",true,2});
  auto splitPartial=makeTrace();splitPartial.count=1;splitPartial.spans[0]={1229,1240,1250,1260,1270,4,0,0,0};
  reasonOf(&splitPartial,{"long-cycle Present split names a partial Present","partial_present",false,0});
  auto splitFailed=makeTrace();splitFailed.count=1;splitFailed.spans[0]={1300,1310,1360,1370,1400,4,0,0,-1};
  reasonOf(&splitFailed,{"long-cycle Present split names a failed Present","failed_present",false,0});
  auto splitOldProvider=makeTrace();splitOldProvider.version=99;
  reasonOf(&splitOldProvider,{"long-cycle Present split names a provider of another version","provider_version",false,0});
  auto splitOtherThread=makeTrace();splitOtherThread.count=1;splitOtherThread.spans[0]={1300,1310,1360,1370,1400,99,0,0,0};
  reasonOf(&splitOtherThread,{"long-cycle Present split names a Present on another thread","other_present_thread",false,0});

  // The line has room for its worst case: every number at the widest a wall-clock ms can print, split present.
  FrameCycleStats::Completed widest=splitDone;
  widest.cycleMs=widest.beforeFirstMs=widest.firstSubmitMs=widest.betweenEyesMs=widest.secondSubmitMs=widest.afterSecondMs=
    widest.nextWaitMs=widest.waitOwnerMs=widest.submitOwnerMs[0]=widest.submitOwnerMs[1]=widest.renderParkMs[0]=
    widest.renderParkMs[1]=widest.prePresentMs=widest.presentHookMs=widest.hookBeforeRealMs=widest.hookRealMs=
    widest.hookAfterRealMs=widest.hookCallbackMs=widest.postPresentMs=123456789.1234;
  char widestLine[1024]{};
  const size_t widestLength=formatLongCycleLine(widestLine,sizeof(widestLine),18446744073709551615ull,123456789.1234,widest);
  check(widestLength>0&&widestLength<sizeof(widestLine)-1&&std::strstr(widestLine,"units=wall_ms")&&
    std::strlen(widestLine)==widestLength,"native_long_cycle line fits its buffer at the widest numbers, tail intact");
  check(std::string(postUnavailableName(FrameCycleStats::PostAvailable))=="available"&&
    std::string(postUnavailableName(FrameCycleStats::MalformedPresent))=="malformed_or_overlapping_present"&&
    std::string(postUnavailableName(FrameCycleStats::PostUnavailableCount))=="unknown",
    "post-submit rejection names cover the enum and refuse past it");
}
}
