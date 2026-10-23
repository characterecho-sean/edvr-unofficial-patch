#pragma once
// The order of a runtime recentre against the frame that sees it (review of main 498e4b16, finding 3; its scratch reproducer is
// build\review-main-498e4b16\switch\recenter_order_repro.cpp, which reported first_pose_x=1.000 second_pose_x=0.000).
//
// The runtime moves LOCAL at a change time. The seat memory answers by making the head, as it is in the new LOCAL, the seated origin, and the
// seated space cannot be replaced while a frame is open. The old order polled the seat before the frame's pending changes were advanced, so on
// the first frame at or after the change time the answer was not yet due, the frame located and published a pose in the OLD seated space, and
// the origin moved a poll later. Now every frame opener advances through NativeRuntimeHost::advanceReferenceChanges, which gives the frame up
// (Withheld: nothing located, nothing published) when a LOCAL change takes effect on it and the seat memory answers.
//
// These cases drive that production function with the fixture's synthetic runtime, whose locate returns the head minus the seated origin.
// They exercise the function every frame opener calls, not WaitGetPoses end to end.
#include "launch_centre_cases.h"
#include <cstring>

namespace edvr::openxr::test {
namespace seat_order_fixture {
struct Rig {
  launch_fixture::Fixture f;
  NativeRuntimeHost& h=f.host;
  unsigned armedCreates=0;
  unsigned answers() const {return f.fake.creates-armedCreates;}     // seated spaces made since the rig was armed: one per answer
  explicit Rig(bool active,bool stage=false):f(stage) {
    using namespace launch_fixture;
    h.seatEnsureLoaded();
    SeatContext c{};
    if(active)c.published=c.keep=c.gameplay=c.seatedKnown=c.seated=true;
    h.seat.setSeat(SeatPose{1,0,0,0});
    h.seat.onContext(c,GetTickCount64());
    h.seated.replace(xrFromSeatPose(h.seat.seat()));
    h.currentOrigin=SeatPose{1,0,0,0};
    h.seat.noteOrigin(true);
    armedCreates=f.fake.creates;     // counted from here: the arming replace is not an answer
  }
  void note(XrTime changeTime) {
    using namespace launch_fixture;
    XrEventDataReferenceSpaceChangePending event{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
    event.session=session();event.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;event.changeTime=changeTime;event.poseValid=XR_FALSE;
    XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
    std::memcpy(&buffer,&event,sizeof(event));
    NativeRuntimeHost::referenceEvent(buffer,&h);
  }
  NativeRuntimeHost::ChangeStep frame(XrTime displayTime) {
    Frame fr{};fr.predictedDisplayTime=displayTime;
    XrResult error=XR_SUCCESS;
    return h.advanceReferenceChanges(fr,error,true);
  }
  // The pose a frame at `t` would publish from the seated space as it stands.
  double poseX(XrTime t) {
    using namespace launch_fixture;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    return locate(view(),h.seated.space(),t,&loc)==XR_SUCCESS?double(loc.pose.position.x):1e9;
  }
};
// The state after the game's reset was answered "no press" with the stored seat (x=1) and the frame poll then saw the player's press: a late
// press is pending, the head is at x=2 (what honouring it would make the seat). Published through the same shared words the graphics half
// uses, so seatSync finds it the way it does in the game. Review of main 9fe5b257, finding 2 (late_press_repro.cpp).
inline void armLatePress(Rig& r,uint32_t pressSeq=1) {
  auto& h=r.h;
  SeatContext c{};c.published=c.keep=c.gameplay=c.seatedKnown=c.seated=true;
  publishSeatContext(encodeSeatContext(c));
  publishHmdResetPress(0,0);     // a previous case's press must not be this host's "already seen" sequence
  h.seatSync(GetTickCount64());
  h.seat.applyDone();h.seat.noteOrigin(true);     // the start-of-game apply the Rig's first context queued is done: only the late press is outstanding
  r.f.fake.head.position.x=2;
  const uint64_t now=GetTickCount64();
  h.seat.noteReplacedByNoPress(now-50);h.seatNoPressAt=now-50;
  HmdResetPress press{};press.status=HmdPressStatus::Observable;press.seq=pressSeq;
  publishHmdResetPress(encodeHmdPress(press),now);
  h.seatSync(now);
}
inline void removeTree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd{};
  HANDLE find=FindFirstFileW((dir+L"\\*").c_str(),&fd);
  if(find!=INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name=fd.cFileName;
      if(name==L"."||name==L"..")continue;
      const std::wstring full=dir+L"\\"+name;
      if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)removeTree(full);else DeleteFileW(full.c_str());
    } while(FindNextFileW(find,&fd));
    FindClose(find);
  }
  RemoveDirectoryW(dir.c_str());
}
} // namespace seat_order_fixture

template<class Check> void runSeatOrderCases(Check&& check) {
  using namespace launch_fixture;
  using namespace seat_order_fixture;
  using Step=NativeRuntimeHost::ChangeStep;
  wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
  wchar_t leaf[96];std::swprintf(leaf,96,L"edvr_seat_order_%lu_%llu",GetCurrentProcessId(),(unsigned long long)GetTickCount64());
  const std::wstring scratch=std::wstring(temp)+leaf;
  CreateDirectoryW(scratch.c_str(),nullptr);
  wchar_t previous[MAX_PATH]{};
  const DWORD hadPrevious=GetEnvironmentVariableW(L"LOCALAPPDATA",previous,MAX_PATH);
  SetEnvironmentVariableW(L"LOCALAPPDATA",scratch.c_str());

  {   // one event: the first frame at the change time is given up, and the first frame returned after it is in the new space
    Rig r(true);auto& h=r.h;
    check(r.f.initialized&&h.seat.keep(),"seat order: the host is armed with a seat in force");
    r.f.fake.head.position.x=2;     // the runtime recentred: the head is 2 in the new LOCAL, the seat still carries 1
    r.note(200);
    check(r.frame(150)==Step::Ok&&r.poseX(150)==1.0,"seat order: before the change time nothing is given up (the old seat, as before)");
    check(r.frame(200)==Step::Withheld,"seat order: the frame at the change time is given up, not located and published from the old space");
    check(h.localHandled==1&&h.referenceChanges==1&&r.answers()==1,"seat order: the origin was replaced inside that call (one answer)");
    check(h.seat.seat().x==2.0&&h.seat.originIsSeat()&&!h.seat.storePending(),"seat order: the head in the new LOCAL is the seat, stored at once");
    bool stalePublished=false;
    for(XrTime t=210;t<=260;t+=10)if(r.frame(t)==Step::Ok&&r.poseX(t)!=0.0)stalePublished=true;
    check(!stalePublished,"seat order: every frame returned after the change is in the new seated space (the pose is the head minus the new origin: 0)");
    check(r.frame(270)==Step::Ok&&r.poseX(270)==0.0,"seat order: the first pose returned after the change reads 0, not the old 1 the review's reproducer published");
  }
  {   // two events pending when the next frame opens (the flat empty-frame path stopped advancing): one frame given up, one answer
    Rig r(true);auto& h=r.h;
    r.f.fake.head.position.x=2;
    r.note(200);r.note(300);
    check(r.frame(350)==Step::Withheld,"seat order: two changes due at once give up one frame");
    check(h.referenceChanges==2&&h.localHandled==2&&r.answers()==1,"seat order: ...answered once, both counted as answered");
    check(r.frame(360)==Step::Ok&&r.poseX(360)==0.0,"seat order: ...and the next frame is in the new space");
  }
  {   // SteamVR's dashboard changes LOCAL only while the seated renderer uses STAGE.
    Rig r(true,true);auto& h=r.h;
    check(r.f.initialized&&r.f.fake.naturalType==XR_REFERENCE_SPACE_TYPE_STAGE&&
          h.seated.space()!=XR_NULL_HANDLE,"stage seat order: STAGE-based seated route is armed");
    r.f.fake.head.position.x=2;
    r.note(200); // LOCAL event; no STAGE event follows
    check(r.frame(150)==Step::Ok&&r.poseX(150)==1.0,"stage seat order: old seat remains before LOCAL change time");
    check(r.frame(200)==Step::Withheld&&h.referenceChanges==1&&h.localHandled==1&&r.answers()==1,
          "stage seat order: LOCAL-only dashboard change withholds stale STAGE frame and replaces origin");
    check(h.seat.seat().x==2.0&&h.seat.originIsSeat()&&h.currentOrigin.x==2.0&&r.f.fake.argumentsValid,
          "stage seat order: current head in STAGE becomes the remembered player seat");
    check(r.frame(210)==Step::Ok&&r.poseX(210)==0.0,
          "stage seat order: next STAGE frame uses the new dashboard-recentered seat");
    check(h.seatKeyText.size()>=6&&h.seatKeyText.substr(h.seatKeyText.size()-6)=="-stage",
          "stage seat order: persisted seat remains keyed to STAGE coordinates");
  }
  {   // Without head tracking, the newly recentered LOCAL origin is a seat in STAGE coordinates.
    Rig r(true,true);auto& h=r.h;
    r.f.fake.flags=0;r.f.fake.localOriginInStage.position.x=2;
    r.note(200);
    check(r.frame(200)==Step::Withheld&&h.localHandled==1&&h.seat.seat().x==2.0&&h.seat.applyPending(),
          "stage seat order: untracked dashboard recenter stores LOCAL origin in STAGE, not STAGE identity");
    check(r.f.fake.argumentsValid,"stage seat order: LOCAL origin was located against STAGE");
  }
  {   // If neither the head nor LOCAL origin can be related to STAGE, keep the old seat for a retry.
    Rig r(true,true);auto& h=r.h;
    r.f.fake.flags=0;r.f.fake.localOriginFlags=0;
    r.note(200);
    check(r.frame(200)==Step::Withheld&&h.localHandled==0&&h.seat.seat().x==1.0,
          "stage seat order: unavailable LOCAL-in-STAGE pose cannot overwrite the saved seat");
    r.f.fake.flags=launch_fixture::tracked;r.f.fake.head.position.x=3;
    h.seatProcessRuntimeRecentre();
    check(h.localHandled==1&&h.seat.seat().x==3.0&&h.currentOrigin.x==3.0,
          "stage seat order: deferred dashboard recenter lands when tracking returns");
  }
  {   // two events with different change times: each answered by its own frame
    Rig r(true);auto& h=r.h;
    r.f.fake.head.position.x=2;
    r.note(200);r.note(400);
    check(r.frame(250)==Step::Withheld&&h.localHandled==1&&h.seat.seat().x==2.0,"seat order: the first of two changes is answered on its frame");
    r.f.fake.head.position.x=5;     // the second recentre moves LOCAL again
    check(r.frame(300)==Step::Ok&&r.poseX(300)==3.0,"seat order: a frame between the change times is not given up (the second change has not happened)");
    check(r.frame(400)==Step::Withheld&&h.localHandled==2&&h.seat.seat().x==5.0,"seat order: the second is answered on its own frame, sampling the head then");
    check(r.frame(410)==Step::Ok&&r.poseX(410)==0.0&&r.answers()==2,"seat order: two answers, two replacements, and the next frame reads 0");
  }
  {   // the seat memory inactive (the graphics half has not spoken): nothing is given up, the advance is what it always was
    Rig r(false);auto& h=r.h;
    r.f.fake.head.position.x=2;
    r.note(200);
    check(!h.seat.keep()&&r.frame(200)==Step::Ok&&r.answers()==0,"seat order: with the seat memory inactive a change is advanced and the frame goes on");
    h.seatProcessRuntimeRecentre();
    check(h.localHandled==1&&r.answers()==0,"seat order: ...and the poll only records that it ignored it");
  }
  {   // the headset is not tracked when the change takes effect: LOCAL's own origin is the seat, the frame is still given up
    Rig r(true);auto& h=r.h;
    r.f.fake.head.position.x=2;r.f.fake.flags=0;
    r.note(200);
    check(r.frame(200)==Step::Withheld,"seat order: an untracked head still gives the frame up");
    check(h.localHandled==1&&h.seat.seat().x==0.0&&h.seat.seat().y==0.0&&h.seat.applyPending(),"seat order: ...the seat is LOCAL's origin, applied when a frame allows");
  }

  // A late recenter press stays pending until its correction is applied (review of main 9fe5b257, finding 2). The old seatPoll cleared the
  // request before locating the head, so one transient failure lost the player's press for good.
  {   // the first poll's locate fails, the second is healthy: the press is applied on the second
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    check(h.seatLatePressPending&&h.seat.seat().x==1.0,"late press: the press after the reset is pending, the stored seat still in force");
    const unsigned creates=r.f.fake.creates;
    r.f.fake.locateResult=XR_ERROR_TIME_INVALID;
    const bool first=h.seatPoll();
    check(!first&&h.seatLatePressPending,"late press: a failed locate keeps the request pending");
    check(h.seat.seat().x==1.0&&h.currentOrigin.x==1.0&&r.f.fake.creates==creates,"late press: ...and changed nothing");
    r.f.fake.locateResult=XR_SUCCESS;
    const bool second=h.seatPoll();
    check(second&&!h.seatLatePressPending,"late press: the retry applies the press and clears the request");
    check(h.seat.seat().x==2.0&&h.currentOrigin.x==2.0&&r.f.fake.creates==creates+1,"late press: ...the head is the seat and the origin, one new seated space");
    check(!h.seatPoll()&&r.f.fake.creates==creates+1&&h.seatLatePressDrops==0,"late press: once applied it is not applied again, and nothing was dropped");
  }
  {   // the reset itself is rejected (the head locates but is not tracked, so no origin can be made from it), then accepted
    // (a failed seated-space create is not a retry case: SeatedSpace::replace then stays unusable on purpose)
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    r.f.fake.flags=0;
    check(!h.seatPoll()&&h.seatLatePressPending,"late press: a rejected reset keeps the request pending");
    r.f.fake.flags=launch_fixture::tracked;
    check(h.seatPoll()&&!h.seatLatePressPending&&h.seat.seat().x==2.0,"late press: ...and the next poll applies it");
  }
  {   // never applicable: retried for kLatePressRetryMs, then dropped once, and a healthy poll afterwards does not resurrect it
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    r.f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(!h.seatPoll()&&h.seatLatePressPending&&h.seatLatePressDrops==0,"late press expiry: inside the retry time it is kept");
    h.seatLatePressAt=GetTickCount64()-kLatePressRetryMs-1;     // the request is now older than the retry time
    check(!h.seatPoll()&&!h.seatLatePressPending&&h.seatLatePressDrops==1,"late press expiry: past the retry time it is dropped, once");
    r.f.fake.locateResult=XR_SUCCESS;
    const unsigned creates=r.f.fake.creates;
    check(!h.seatPoll()&&h.seat.seat().x==1.0&&r.f.fake.creates==creates&&h.seatLatePressDrops==1,"late press expiry: a healthy poll afterwards leaves the seat alone");
  }
  {   // a transition window opens while it is pending: the transition's seat supersedes the press
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    r.f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(!h.seatPoll()&&h.seatLatePressPending,"late press supersede: pending after a failed poll");
    SeatContext c{};c.published=c.keep=c.gameplay=c.seatedKnown=c.seated=true;c.transitions=1;     // a journal event: a window opens
    publishSeatContext(encodeSeatContext(c));
    h.seatSync(GetTickCount64());
    check(!h.seatLatePressPending&&h.seatLatePressDrops==1,"late press supersede: the transition window drops it, once");
    r.f.fake.locateResult=XR_SUCCESS;
    check(!h.seatPoll()&&h.seat.seat().x==1.0,"late press supersede: the dropped press is not applied afterwards");
  }
  // The retry is bound to the reset it corrects (review follow-up of main 4bd06f12, F1; scratch repro seat_supersession_repro.cpp).
  {   // a later SUCCESSFUL reset retires it: the next poll must not recentre again on a newer head pose
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    r.f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(!h.seatPoll()&&h.seatLatePressPending,"late press supersession: pending after a failed poll");
    r.f.fake.locateResult=XR_SUCCESS;
    r.f.fake.head.position.x=3;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    head.locationFlags=launch_fixture::tracked;head.pose=r.f.fake.head;
    h.seatTraceDecision="honour_press";
    const bool reset=h.applySeatedReset(head,"seated_reset",true);
    if(reset)h.seat.noteReset(SeatResetDecision::HonourPress,h.currentOrigin,GetTickCount64());
    check(reset&&h.currentOrigin.x==3.0,"late press supersession: the later reset set the origin at the head (x=3)");
    r.f.fake.head.position.x=4;     // the head moves on
    const unsigned creates=r.f.fake.creates;
    check(!h.seatPoll()&&h.currentOrigin.x==3.0&&r.f.fake.creates==creates,"late press supersession: the next poll makes no second recentre on the newer head pose");
    check(!h.seatLatePressPending&&h.seatLatePressDrops==1,"late press supersession: the request was dropped, once");
  }
  {   // keep mode ends (a profile switch, the key off): the retry must not apply or save under game mode
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    r.f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(!h.seatPoll()&&h.seatLatePressPending,"late press mode switch: pending after a failed poll");
    SeatContext game{};game.published=true;game.keep=false;game.gameplay=true;game.seatedKnown=game.seated=true;
    publishSeatContext(encodeSeatContext(game));
    h.seatSync(GetTickCount64());
    r.f.fake.locateResult=XR_SUCCESS;
    const unsigned creates=r.f.fake.creates;
    check(!h.seat.keep()&&!h.seatLatePressPending&&h.seatLatePressDrops==1,"late press mode switch: keep to game mode drops the request");
    check(!h.seatPoll()&&h.currentOrigin.x==1.0&&h.seat.seat().x==1.0&&r.f.fake.creates==creates,"late press mode switch: no recentre and no save followed");
  }
  {   // the seat context leaves the cockpit (a menu or a load), where the game's resets pass through
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    r.f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(!h.seatPoll()&&h.seatLatePressPending,"late press left seat: pending after a failed poll");
    SeatContext menu{};menu.published=true;menu.keep=true;     // neither seated nor on foot
    publishSeatContext(encodeSeatContext(menu));
    h.seatSync(GetTickCount64());
    r.f.fake.locateResult=XR_SUCCESS;
    check(!h.seatLatePressPending&&h.seatLatePressDrops==1&&!h.seatPoll()&&h.seat.seat().x==1.0,"late press left seat: a menu or load drops the request, and nothing is applied");
  }
  {   // the geometry is republished (a session restart)
    Rig r(true);auto& h=r.h;
    armLatePress(r);
    r.f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(!h.seatPoll()&&h.seatLatePressPending,"late press geometry: pending after a failed poll");
    ++h.geometryGeneration;
    r.f.fake.locateResult=XR_SUCCESS;
    const unsigned creates=r.f.fake.creates;
    check(!h.seatPoll()&&!h.seatLatePressPending&&h.seatLatePressDrops==1&&r.f.fake.creates==creates,"late press geometry: a new geometry generation drops the request");
  }
  publishSeatContext(0);publishHmdResetPress(0,0);     // leave the shared words as found

  SetEnvironmentVariableW(L"LOCALAPPDATA",hadPrevious&&hadPrevious<MAX_PATH?previous:nullptr);
  removeTree(scratch);
}
} // namespace edvr::openxr::test
