#pragma once
// The pose cache never answers from a stale seated space (src/openxr/pose_cache.h PoseInvalidation, native_runtime_host.h dropPoseFrame and
// applySeatedResetCore), and it is read by the job thread while the owner writes it.
//
// The cached pose is a pose in the seated space as it stood when the owner published. Every way the seated origin can move must withdraw the
// publication before the next cached answer: the seat memory applying its seat, the game's own reset, a runtime recentre (the pending change
// and the frame it takes effect on), and the start-up and intro recentres. Each case below publishes a frame, proves the caller's display call
// is answered from the cache in the OLD space, moves the origin by that route, and proves the next call is not the cache's (it takes the hop,
// which locates in the NEW space) and that the window counted exactly that reason. Then the frame after is published and the cache answers
// again, in the new space. Through the real host on its own owner thread, against the fake XR runtime (whose locate returns the head minus the
// seated origin, so a pose's x is the head's x minus the origin's).
//
// The last cases are the cache as a shared structure: the reason mapping, the line, a reader and a writer that would tear a record if one
// field were read outside the lock, and the poll ring against the owner-hop path (the same events in the same order).
#include "seat_order_cases.h"
#include "pose_cache_cases.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace edvr::openxr::test {

template<class Check> void runPoseCacheInvalidationCases(Check&& check) {
  using namespace launch_fixture;
  using namespace head_pose_fixture;
  using namespace pose_cache_fixture;
  wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
  wchar_t leaf[96];std::swprintf(leaf,96,L"edvr_pose_cache_inval_%lu_%llu",GetCurrentProcessId(),(unsigned long long)GetTickCount64());
  const std::wstring scratch=std::wstring(temp)+leaf;
  CreateDirectoryW(scratch.c_str(),nullptr);
  wchar_t previous[MAX_PATH]{};
  const DWORD hadPrevious=GetEnvironmentVariableW(L"LOCALAPPDATA",previous,MAX_PATH);
  SetEnvironmentVariableW(L"LOCALAPPDATA",scratch.c_str());

  OwnerBuiltFixture f;
  check(f.route.bind()&&f.owner.start()&&f.owner.invoke([&]{f.build();})&&f.initialized,"(the invalidation cases' host is built on its own owner thread)");
  if(f.initialized) {
    auto& h=*f.hostPtr;
    h.headClock=readClock;
    h.poseCache.setClock(&clockUs);
    h.poseCacheOn.store(true);
    const DWORD me=GetCurrentThreadId();
    const XrTime display=5000000000,period=11111111;
    setNow(display-42000000);
    f.fake.head.orientation={0.0f,0.0f,0.0f,1.0f};
    f.fake.head.position={0.0f,0.0f,0.0f};
    // The seat memory armed as seat_order_cases.h's Rig arms it: a seat in force, the origin at x = 1.
    f.owner.invoke([&]{
      h.seatEnsureLoaded();
      SeatContext c{};
      c.published=c.keep=c.gameplay=c.seatedKnown=c.seated=true;
      h.seat.setSeat(SeatPose{1,0,0,0});
      h.seat.onContext(c,GetTickCount64());
      h.seated.replace(xrFromSeatPose(h.seat.seat()));
      h.currentOrigin=SeatPose{1,0,0,0};
      h.seat.noteOrigin(true);
    });
    const XrVector3f still{0.0f,0.0f,0.0f};
    const auto publish=[&](XrTime d){
      bool live=false;
      f.owner.invoke([&]{
        h.publishPoseGap(d,period,XrPosef{{0,0,0,1},{0,0,0}},true,0,still,1000,[](const char*){});
        h.publishPoseCache();
        live=h.poseCache.live();
      });
      return live;
    };
    const auto callDisplay=[&](vr::TrackedDevicePose_t& p){
      HeadCall call;call.thread=me;call.rva=0x4E3881;call.display=true;
      p={};
      return h.locateHeadFor(h.geometryGeneration,vr::TrackingUniverseSeated,0.0f,call,p);
    };
    // A call answered from the cache costs the runtime no locate; a hop costs at least one.
    const auto fromCache=[&](vr::TrackedDevicePose_t& p){
      const unsigned before=f.fake.locates;
      const bool ok=callDisplay(p);
      return ok&&f.fake.locates==before;
    };
    const auto x=[](const vr::TrackedDevicePose_t& p){return double(p.mDeviceToAbsoluteTracking.m[0][3]);};
    // What a pose is in the seated space as it now stands: the fake runtime's head minus the origin the host last put in.
    const auto expect=[&]{return double(f.fake.head.position.x)-double(h.currentOrigin.x);};
    const auto is=[&](const vr::TrackedDevicePose_t& p){return std::fabs(x(p)-expect())<1e-6;};
    const auto window=[&]{PoseCacheReport r{};h.poseCache.takeWindow(r);return r;};
    const auto sum=[](const PoseCacheReport& r){uint64_t n=0;for(unsigned i=0;i<unsigned(PoseInvalidation::Count);++i)n+=r.invalidations[i];return n;};
    // A reset sampled from the head as the fake runtime has it, as the production recentre callers do.
    const auto headNow=[&](XrSpaceLocation& head){
      head=XrSpaceLocation{XR_TYPE_SPACE_LOCATION};
      head.locationFlags=tracked;head.pose=f.fake.head;
    };
    const auto note=[&](XrTime changeTime){
      f.owner.invoke([&]{
        XrEventDataReferenceSpaceChangePending event{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
        event.session=session();event.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;event.changeTime=changeTime;event.poseValid=XR_FALSE;
        XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
        std::memcpy(&buffer,&event,sizeof(event));
        NativeRuntimeHost::referenceEvent(buffer,&h);
      });
    };
    vr::TrackedDevicePose_t p{};

    // ---- steady: published, served from the cache, in the seated space as it stands (head 0, origin 1: x = -1) ----------------------------
    check(publish(display),"(the first frame is published)");
    callDisplay(p);   // the caller's first call is the hop that teaches the cache its RVA
    check(fromCache(p)&&is(p)&&std::fabs(x(p)+1.0)<1e-6,"before any reset: a display call is answered from the cache, in the seated space as it stands (x = -1)");
    window();

    // ---- seat apply: the seat memory puts its stored seat in as the origin -------------------------------------------------------------------
    bool applied=false;
    f.owner.invoke([&]{
      h.seat.setSeat(SeatPose{3,0,0,0});
      applied=h.applyStoredSeat("seat_transition","transition_seat",true);
    });
    check(applied,"(the seat memory applied the stored seat: origin x = 3)");
    check(!h.poseCache.live()&&!fromCache(p)&&is(p)&&std::fabs(x(p)+3.0)<1e-6,
          "seat apply: the next display call is NOT the cache's (the old-space pose x = -1 is gone); the hop answers in the new space (x = -3)");
    PoseCacheReport w=window();
    check(w.invalidations[unsigned(PoseInvalidation::SeatApply)]==1&&sum(w)==1,"seat apply: counted once, under seat_apply, and nothing else");
    check(publish(display+period)&&fromCache(p)&&is(p),"seat apply: the next frame's publication is answered from the cache again, in the new space");
    window();

    // ---- the game's own reset, through the production entry point (ResetSeatedZeroPose) -----------------------------------------------------
    f.fake.head.position.x=2.0f;
    const bool reset=h.resetSeated(h.geometryGeneration);
    check(reset,"(the game's reset was honoured)");
    check(!fromCache(p)&&is(p),"game reset: the next display call is not the cache's (the old x = -3 is gone); the hop answers in the new space");
    w=window();
    check(w.invalidations[unsigned(PoseInvalidation::GameReset)]+w.invalidations[unsigned(PoseInvalidation::SeatApply)]==1&&sum(w)==1&&
          w.invalidations[unsigned(PoseInvalidation::RuntimeRecentre)]==0,"game reset: counted once, under the game's reset (or the seat's replacement of it), not under a runtime or start-up reason");
    // The reason itself: a reset applied under "seated_reset" is the game_reset category (seat memory off or on).
    check(publish(display+2*period)&&fromCache(p)&&is(p),"game reset: published again, answered from the cache in the new space");
    f.fake.head.position.x=5.0f;
    f.owner.invoke([&]{XrSpaceLocation head;headNow(head);h.applySeatedReset(head,"seated_reset",true);});
    w=window();
    check(w.invalidations[unsigned(PoseInvalidation::GameReset)]==1&&sum(w)==1,"game reset: a reset applied for \"seated_reset\" is counted under game_reset");
    check(!fromCache(p)&&is(p)&&std::fabs(x(p))<1e-6,"game reset: ...and the call after it is the hop's, in the new space (head 5, origin 5: x = 0)");

    // ---- a runtime recentre: the pending change, then the frame it takes effect on ---------------------------------------------------------
    // The game's reset above re-read the seat context from the shared word (seatSync), which no graphics half publishes in this rig: the seat
    // memory went back to game mode. The seat is put back in force, as the arming did, so the recentre below is the one the seat memory answers.
    f.owner.invoke([&]{
      SeatContext c{};
      c.published=c.keep=c.gameplay=c.seatedKnown=c.seated=true;
      h.seat.onContext(c,GetTickCount64());
      h.seat.noteOrigin(true);
    });
    check(publish(display+3*period)&&fromCache(p),"(published and served before the runtime moves LOCAL)");
    window();
    const XrTime cut=display+8*period;
    note(cut);
    check(!h.poseCache.live()&&!fromCache(p),"runtime recentre: the pending change withdraws the publication at once; calls take the hop until the next wait publishes");
    w=window();
    check(w.invalidations[unsigned(PoseInvalidation::RuntimeRecentre)]==1&&sum(w)==1,"runtime recentre: the pending change is counted under runtime_recentre");
    check(publish(display+4*period)&&fromCache(p),"(a frame before the cut is published and served)");
    window();
    f.fake.head.position.x=9.0f;    // the runtime recentred: the head is 9 in the new LOCAL
    NativeRuntimeHost::ChangeStep step=NativeRuntimeHost::ChangeStep::Ok;
    f.owner.invoke([&]{
      Frame fr{};fr.predictedDisplayTime=cut;
      XrResult error=XR_SUCCESS;
      step=h.advanceReferenceChanges(fr,error,true);
    });
    check(step==NativeRuntimeHost::ChangeStep::Withheld,"runtime recentre: the frame the change takes effect on is withheld (seat memory in force)");
    check(!h.poseCache.live()&&!fromCache(p)&&is(p)&&std::fabs(x(p))<1e-6,
          "runtime recentre: after the withheld frame no cached answer is served; the hop locates in the new space (head 9, seat 9: x = 0)");
    w=window();
    check(w.invalidations[unsigned(PoseInvalidation::RuntimeRecentre)]==1&&sum(w)==1,"runtime recentre: the advance and the seat memory's answer are one withdrawal (the publication was live once)");
    check(publish(cut+period)&&fromCache(p)&&is(p),"runtime recentre: the frame after the withheld one is published in the new space and answered from the cache");
    window();

    // ---- start-up and intro recentres ----------------------------------------------------------------------------------------------------------
    f.fake.head.position.x=4.0f;
    bool intro=false;
    f.owner.invoke([&]{intro=h.applyIntroRecentre();});
    check(intro,"(the intro recentre landed)");
    check(!fromCache(p)&&is(p),"intro recentre: the next call is the hop's, in the new space (head 4, origin 4)");
    w=window();
    check(w.invalidations[unsigned(PoseInvalidation::Startup)]==1&&sum(w)==1,"intro recentre: counted under startup");
    check(publish(cut+2*period)&&fromCache(p),"(published again)");
    f.fake.head.position.x=6.0f;
    f.owner.invoke([&]{
      XrSpaceLocation head;headNow(head);
      const XrPosef origin=head.pose;
      h.applySeatedReset(head,"launch_centre",false,&origin);
    });
    w=window();
    check(w.invalidations[unsigned(PoseInvalidation::Startup)]==1&&sum(w)==1&&!fromCache(p)&&is(p),
          "launch centre: counted under startup, and the next call is the hop's, in the new space");

    // ---- a drop that is not a reset, and a withdrawal of nothing --------------------------------------------------------------------------------
    check(publish(cut+3*period)&&fromCache(p),"(published again)");
    window();
    f.owner.invoke([&]{h.dropPoseFrame();});
    w=window();
    check(w.invalidations[unsigned(PoseInvalidation::Other)]==1&&sum(w)==1&&!fromCache(p),"session/geometry drop: counted under other, and the call after it takes the hop");
    f.owner.invoke([&]{h.dropPoseFrame();h.poseCache.invalidate(PoseInvalidation::GameReset);});
    w=window();
    check(sum(w)==0&&w.invalidationsTotal>=7,"withdrawing a publication that is already withdrawn counts nothing; the running total keeps what was counted");

    // ---- the key off: nothing of this runs --------------------------------------------------------------------------------------------------------
    h.poseCacheOn.store(false);
    check(publish(cut+4*period),"(published with the key off: the host does not publish, the rig does)");
    h.poseCacheOn.store(true);

    char line[256]{};
    PoseCacheReport sample{};
    sample.invalidations[0]=1;sample.invalidations[1]=2;sample.invalidations[2]=3;sample.invalidations[3]=4;sample.invalidations[4]=5;sample.invalidationsTotal=99;
    check(formatPoseInvalidationLine(7,sample,line,sizeof line)&&
          std::string(line)=="native_pose_cache_invalidations,window=7,seat_apply=1,game_reset=2,runtime_recentre=3,startup=4,other=5,total=99\n",
          "the invalidation line has every reason and the running total");

    // ---- the poll ring against the owner-hop path: the same events in the same order ------------------------------------------------------------
    // The resets the scenarios above queued (every honoured reset tells the game) are still waiting: drain them, ring and queue, so the two
    // paths below start from empty.
    for(const bool cacheOn:{true,false}) {
      h.poseCacheOn.store(cacheOn);
      vr::VREvent_t ev{};vr::TrackedDevicePose_t ep{};
      for(unsigned i=0;i<64&&h.pollEvent(h.geometryGeneration,vr::TrackingUniverseSeated,ev,ep);++i) {}
    }
    check(h.pollRing.size()==0,"(the earlier scenarios' queued resets are drained)");
    struct Seen {int type;bool valid;};
    const auto drain=[&](bool cacheOn){
      std::vector<Seen> seq;
      h.poseCacheOn.store(cacheOn);
      vr::TrackedDevicePose_t rp{};
      rp.bDeviceIsConnected=true;rp.bPoseIsValid=true;rp.eTrackingResult=vr::TrackingResult_Running_OK;
      const uint64_t og=h.poses.read().originGeneration;
      h.resetEvents.push(h.geometryGeneration,og,GetTickCount64(),rp);
      h.resetEvents.push(h.geometryGeneration,og,GetTickCount64(),rp);
      f.owner.invoke([&]{h.fillPollRing();});                 // a waitPoses ran between the pushes
      h.resetEvents.push(h.geometryGeneration,og+100,GetTickCount64(),rp);   // made under an origin that has since moved: its pose is invalid
      f.owner.invoke([&]{h.fillPollRing();h.queueQuitEvent(XR_SUCCESS);});
      for(unsigned i=0;i<8;++i) {
        vr::VREvent_t ev{};vr::TrackedDevicePose_t ep{};
        if(!h.pollEvent(h.geometryGeneration,vr::TrackingUniverseSeated,ev,ep))break;
        seq.push_back({int(ev.eventType),ep.bPoseIsValid});
      }
      return seq;
    };
    const auto viaOwner=drain(false);
    const auto viaRing=drain(true);
    const auto show=[](const char* name,const std::vector<Seen>& s){std::string out;for(const auto& e:s)out+=" "+std::to_string(e.type)+(e.valid?"v":"x");std::printf("pose cache event order, %s:%s\n",name,out.c_str());};
    if(viaOwner.size()!=viaRing.size()||viaOwner.size()!=4) {show("owner path",viaOwner);show("ring",viaRing);}
    bool same=viaOwner.size()==viaRing.size()&&viaOwner.size()==4;
    for(size_t i=0;same&&i<viaOwner.size();++i)same=viaOwner[i].type==viaRing[i].type&&viaOwner[i].valid==viaRing[i].valid;
    check(same&&viaRing[0].type==vr::VREvent_Quit&&viaRing[1].type==vr::VREvent_SeatedZeroPoseReset&&viaRing[1].valid&&
          viaRing[3].type==vr::VREvent_SeatedZeroPoseReset&&!viaRing[3].valid,
          "event ordering: the ring delivers the quit first, then the resets in the order they were queued, each with the pose the owner path gave it");
    h.poseCacheOn.store(true);
    f.owner.stop();
  }

  // ---- the reason mapping ---------------------------------------------------------------------------------------------------------------------
  check(poseInvalidationForReason("seat_transition")==PoseInvalidation::SeatApply&&poseInvalidationForReason("seated_reset")==PoseInvalidation::GameReset&&
        poseInvalidationForReason("runtime_recentre")==PoseInvalidation::RuntimeRecentre&&poseInvalidationForReason("reference_change")==PoseInvalidation::RuntimeRecentre&&
        poseInvalidationForReason("launch_centre")==PoseInvalidation::Startup&&poseInvalidationForReason("intro_recentre")==PoseInvalidation::Startup&&
        poseInvalidationForReason("session_restart")==PoseInvalidation::Startup&&poseInvalidationForReason("anything_else")==PoseInvalidation::Other&&
        poseInvalidationForReason(nullptr)==PoseInvalidation::Other,
        "every reason the host passes maps to its category; an unknown or missing reason is other (still a withdrawal)");

  // ---- a reader and a writer: no torn record ---------------------------------------------------------------------------------------------------
  {
    PoseCache cache;
    cache.setClock(&pose_cache_fixture::clockUs);
    pose_cache_fixture::gUs.store(1000000);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> torn{0},answered{0},misses{0};
    PoseCacheAnswer first;PoseCacheMiss why=PoseCacheMiss::Unpublished;
    // The writer stamps one number k into every field a reader could take: the display time, both slots' targets, and their pose figures.
    const auto record=[](uint64_t k){
      PoseCacheRecord r;
      r.valid=true;r.keys={1,1};r.display=XrTime(k)*1000000+5000000000;r.period=11111111;r.nowAtPublish=r.display;
      r.now.valid=r.next.valid=true;
      r.now.target=r.display;r.next.target=r.display;
      r.now.pose.mDeviceToAbsoluteTracking.m[0][3]=float(k%100000);r.next.pose.mDeviceToAbsoluteTracking.m[0][3]=float(k%100000);
      return r;
    };
    cache.publish(record(1),pose_cache_fixture::clockUs());
    cache.answer(1,1,0x4E3881,first,why);   // teaches the RVA
    // A record is torn when its fields do not all carry one k: the target is not the display time, or the pose figure is not the display's k.
    const auto tornAnswer=[](const PoseCacheAnswer& a){
      const uint64_t k=uint64_t((a.display-5000000000)/1000000);
      return a.target!=a.display||uint64_t(a.pose.mDeviceToAbsoluteTracking.m[0][3])!=k%100000;
    };
    {
      // The detector itself: an answer built from one record passes, one that mixes two records' fields is flagged (so a zero count below means something).
      PoseCacheAnswer whole,mixedPose,mixedTarget;
      const PoseCacheRecord a=record(41),b=record(42);
      whole.display=a.display;whole.target=a.now.target;whole.pose=a.now.pose;
      mixedPose=whole;mixedPose.pose=b.now.pose;
      mixedTarget=whole;mixedTarget.target=b.now.target;
      check(!tornAnswer(whole)&&tornAnswer(mixedPose)&&tornAnswer(mixedTarget),"the half-written detector passes a whole record and flags one that mixes the fields of two");
    }
    // What the test needs is overlap, not a wall-clock window: under CPU load the reader thread can go the whole 300 ms unscheduled (measured 2026-10-15,
    // 48 spinners beside 8 copies: 14 of 25 runs saw no hit at all, torn never above zero), and a check that waits for a duration then fails on "no hits"
    // is a scheduling check. So the writer runs for the minimum window AND until the reader has taken a floor of answers while the writer was publishing,
    // and the phase of the withdrawals varies so the reader cannot sit in lockstep behind one. A 30 s ceiling turns a reader that is never scheduled into a failure.
    constexpr uint64_t kAnswersWanted=2000;
    std::thread reader([&]{
      while(!stop.load()) {
        PoseCacheAnswer a;PoseCacheMiss m;
        if(cache.answer(1,1,0x4E3881,a,m)) {
          ++answered;
          if(tornAnswer(a))++torn;
        } else ++misses;
      }
    });
    std::thread taker([&]{while(!stop.load()){PoseCacheReport r;cache.takeWindow(r);std::this_thread::yield();}});
    const auto began=std::chrono::steady_clock::now();
    uint64_t k=2;
    uint32_t phase=2463534242u;
    for(;;) {
      const auto elapsed=std::chrono::steady_clock::now()-began;
      if(elapsed>=std::chrono::milliseconds(300)&&answered.load()>=kAnswersWanted)break;
      if(elapsed>=std::chrono::seconds(30))break;
      phase^=phase<<13;phase^=phase>>17;phase^=phase<<5;   // xorshift32: which publications are withdrawn or waited on is not a fixed cycle
      if(phase%7==0)cache.invalidate(PoseInvalidation::Other);
      else if(phase%5==0)cache.beginWait();
      cache.publish(record(k),pose_cache_fixture::clockUs());
      if(phase%64==0)std::this_thread::yield();            // lets a reader that the lock's hand-off order is passing over take its turn
      ++k;
    }
    stop.store(true);
    reader.join();taker.join();
    if(torn.load()!=0||answered.load()<kAnswersWanted)std::printf("pose cache torn-record stress: torn=%llu answered=%llu misses=%llu published=%llu\n",
        (unsigned long long)torn.load(),(unsigned long long)answered.load(),(unsigned long long)misses.load(),(unsigned long long)k);
    check(torn.load()==0&&answered.load()>=kAnswersWanted,"a reader on another thread never sees a record half-written while the owner publishes, waits and withdraws, and the window is taken");
  }

  SetEnvironmentVariableW(L"LOCALAPPDATA",hadPrevious&&hadPrevious<MAX_PATH?previous:nullptr);
  seat_order_fixture::removeTree(scratch);
}
} // namespace edvr::openxr::test
