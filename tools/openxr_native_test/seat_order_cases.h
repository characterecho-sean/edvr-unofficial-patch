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
  explicit Rig(bool active) {
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

  SetEnvironmentVariableW(L"LOCALAPPDATA",hadPrevious&&hadPrevious<MAX_PATH?previous:nullptr);
  removeTree(scratch);
}
} // namespace edvr::openxr::test
