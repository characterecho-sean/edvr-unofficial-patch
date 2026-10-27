#pragma once
// F123: the runtime's position-only recentre (src/openxr/native_runtime_host.h applyPositionRecentre, asked for by positionRecentrePoll from a version 10 answer's positionRecentre).
// The leash and the wall contact move the seated origin under the head and nothing else: the seated space's yaw (the look compensation included) is kept exactly, the head's own
// heading is never read, the origin generation does not move, the game is told nothing, and the stored seat is never touched (F115: an on-foot origin is not the seat). Through the
// real host on its own owner thread, against the fake XR runtime (its head is the pose set here, in natural LOCAL).
#include "pose_cache_invalidation_cases.h"
#include <cmath>
#include <cstring>

namespace position_recentre_provider {
inline int context=0;
inline HRESULT WINAPI begin(void*,const EdvrNativeFrameInput*,EdvrNativeFrameOutput*) {return S_FALSE;}
inline HRESULT WINAPI latch(void*,uint64_t,EdvrNativeFrameDecision*) {return S_FALSE;}
inline HRESULT WINAPI close(void*) {return S_OK;}
}
// A real module-owned provider table admits the host's normal poll entry points.
// No synthetic production setter or private-layout access is needed by the rig.
#pragma comment(linker,"/EXPORT:edvrAcquireNativeFrame")
extern "C" HRESULT WINAPI edvrAcquireNativeFrame(const EdvrNativeFrameRequest* request,EdvrNativeFrameTable* table) {
  if(!request||!table)return E_INVALIDARG;
  *table={sizeof(*table),EDVR_NATIVE_FRAME_VERSION_1,&position_recentre_provider::context,&position_recentre_provider::begin,nullptr,
          &position_recentre_provider::latch,&position_recentre_provider::close,&position_recentre_provider::close};
  return S_OK;
}

namespace edvr::openxr::test {

template<class Check> void runPositionRecentreCases(Check&& check) {
  using namespace launch_fixture;
  using namespace head_pose_fixture;
  using namespace pose_cache_fixture;
  wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
  wchar_t leaf[96];std::swprintf(leaf,96,L"edvr_position_recentre_%lu_%llu",GetCurrentProcessId(),(unsigned long long)GetTickCount64());
  const std::wstring scratch=std::wstring(temp)+leaf;
  CreateDirectoryW(scratch.c_str(),nullptr);
  wchar_t previous[MAX_PATH]{};
  const DWORD hadPrevious=GetEnvironmentVariableW(L"LOCALAPPDATA",previous,MAX_PATH);
  SetEnvironmentVariableW(L"LOCALAPPDATA",scratch.c_str());

  OwnerBuiltFixture f;
  check(f.route.bind()&&f.owner.start()&&f.owner.invoke([&]{f.build();})&&f.initialized,"(the position recentre cases' host is built on its own owner thread)");
  if(f.initialized) {
    auto& h=*f.hostPtr;
    h.headClock=readClock;
    h.poseCache.setClock(&clockUs);
    setNow(5000000000-42000000);
    const double yawHead=60.0*3.14159265358979323846/180.0;
    f.fake.head.orientation={0.0f,float(std::sin(yawHead*0.5)),0.0f,float(std::cos(yawHead*0.5))};   // the head faces 60 deg off: its heading must not become the origin's
    f.fake.head.position={5.0f,1.6f,-2.0f};
    const SeatPose storedSeat{9.0,0.0,0.0,0.25};
    const double seatedYaw=0.8;   // the seated space's yaw: an origin yaw of 0.5 turned by the look compensation to 0.8
    f.owner.invoke([&]{
      SeatContext c{};
      c.published=c.keep=c.gameplay=true;c.onFootKnown=c.onFoot=true;
      h.seat.setSeat(storedSeat);
      h.seat.onContext(c,GetTickCount64());
      h.seat.noteOrigin(false);
      h.currentOrigin=SeatPose{1.0,0.0,0.0,0.5};
      h.lookSeated=SeatPose{1.2,-0.1,0.3,seatedYaw};   // carries an alignment translation as well
      h.lookSeatedKnown=true;
      h.lookBase=2.0;h.lookComp=0.3;h.lookWanted=2.3;
      h.eyeApplied[0]=0.2;h.eyeApplied[1]=-0.1;h.eyeApplied[2]=0.3;
      h.seated.replace(xrFromSeatPose(h.lookSeated));
    });
    check(h.seat.where()==SeatWhere::OnFoot,"(the seat memory places the player on foot)");
    const uint64_t generationBefore=h.poses.read().originGeneration;
    const unsigned long long recentersBefore=(unsigned long long)h.recenters;
    bool ok=false;
    f.owner.invoke([&]{ok=h.applyPositionRecentre(7);});
    check(ok,"F123: the position-only recentre is applied");
    check(h.currentOrigin.x==5.0&&h.currentOrigin.y==double(1.6f)&&h.currentOrigin.z==-2.0,"F123: the seated origin is under the head (x, y and z of the head in natural LOCAL)");
    check(h.currentOrigin.yaw==0.5&&h.lookSeated.yaw==seatedYaw,"F123: baseline yaw stays 0.5 and compensated yaw stays 0.8; neither the look turn nor the head's 60 deg heading becomes the origin");
    check(h.lookSeated.x==h.currentOrigin.x+0.2&&h.lookSeated.y==h.currentOrigin.y-0.1&&h.lookSeated.z==h.currentOrigin.z+0.3&&h.lookComp==0.3&&h.lookBase==2.0&&h.lookWanted==2.3&&
          h.eyeApplied[0]==0.2&&h.eyeApplied[1]==-0.1&&h.eyeApplied[2]==0.3,
          "F123: live look base/compensation/wanted and applied eye alignment are retained, with the alignment composed exactly once at the call");
    check(h.seat.seat().x==storedSeat.x&&h.seat.seat().y==storedSeat.y&&h.seat.seat().z==storedSeat.z&&h.seat.seat().yaw==storedSeat.yaw&&!h.seat.originIsSeat()&&h.seat.has(),
          "F123: the stored seat is untouched and the new origin is not the seat (an on-foot origin is never stored: F115)");
    check(h.poses.read().originGeneration==generationBefore&&(unsigned long long)h.recenters==recentersBefore,"F123: not a reset: the origin generation and the recentre count do not move, and the game is told nothing");
    check(h.positionRecentres==1&&h.positionRecentreRefusals==0,"F123: one recentre counted, none refused");

    // refused, and said: the seat memory does not place the player on foot
    f.owner.invoke([&]{
      h.currentOrigin=SeatPose{1.0,0.0,0.0,0.5};h.lookSeated=SeatPose{1.2,-0.1,0.3,seatedYaw};
      SeatContext off{};
      off.published=off.keep=off.gameplay=true;off.onFootKnown=true;off.onFoot=false;   // the journal says neither seated nor on foot: a menu, a load
      h.seat.onContext(off,GetTickCount64());
    });
    bool refused=true;
    f.owner.invoke([&]{refused=h.applyPositionRecentre(8);});
    check(!refused&&h.currentOrigin.x==1.0&&h.positionRecentreRefusals==1,"F123: with the seat memory not placing the player on foot (the boarding, a menu) the ask is refused and the origin stays");
    // refused: the rotation calibration is on (its yaw would be applied twice)
    f.owner.invoke([&]{
      SeatContext c{};
      c.published=c.keep=c.gameplay=true;c.onFootKnown=c.onFoot=true;
      h.seat.onContext(c,GetTickCount64());
      h.rotApplied[0]=0.05;
    });
    bool rot=true;
    f.owner.invoke([&]{rot=h.applyPositionRecentre(9);});
    check(!rot&&h.currentOrigin.x==1.0&&h.positionRecentreRefusals==2,"F123: with the rotation calibration on the ask is refused (its yaw would be applied twice)");
    f.owner.invoke([&]{h.rotApplied[0]=0.0;});
    bool again=false;
    f.owner.invoke([&]{again=h.applyPositionRecentre(10);});
    check(again&&h.currentOrigin.x==5.0&&h.currentOrigin.yaw==0.5&&h.positionRecentres==2,"F123: a later ask with everything in order is applied without baking look yaw into the baseline");

    // Delivery must survive a transient tracking dropout: a contact latch sends one serial, not a replacement every frame.
    const auto onFoot=[&](bool foot){
      SeatContext c{};c.published=c.keep=c.gameplay=true;c.onFootKnown=true;c.onFoot=foot;
      h.seat.onContext(c,GetTickCount64());
    };
    bool lost=false,recovered=false,duplicate=true;
    f.owner.invoke([&]{
      f.fake.flags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
      f.fake.head.position.x=6.0f;
      lost=h.positionRecentreDispatch(11,1000);
      f.fake.flags=launch_fixture::tracked;
      recovered=h.positionRecentreDispatch(11,1011);
      duplicate=h.positionRecentreDispatch(11,1022);
    });
    check(!lost&&recovered&&!duplicate&&h.currentOrigin.x==6.0&&h.positionRecentres==3,
          "F123: a valid but temporarily untracked head leaves the same request pending, recovery applies it exactly once");

    bool locateLost=false,withdrawn=true,resurrected=true;
    f.owner.invoke([&]{
      f.fake.locateResult=XR_ERROR_TIME_INVALID;
      locateLost=h.positionRecentreDispatch(12,2000);
      withdrawn=h.positionRecentreDispatch(0,2011);
      f.fake.locateResult=XR_SUCCESS;f.fake.head.position.x=7.0f;
      resurrected=h.positionRecentreDispatch(12,2022);
    });
    check(!locateLost&&!withdrawn&&!resurrected&&h.currentOrigin.x==6.0&&h.positionRecentres==3,
          "F123: withdrawal cancels a request waiting on a transient locate failure; the old serial cannot act later");

    bool boarded=true,afterBoarding=true;
    f.owner.invoke([&]{
      f.fake.flags=0;h.positionRecentreDispatch(13,3000);
      onFoot(false);f.fake.flags=launch_fixture::tracked;
      boarded=h.positionRecentreDispatch(13,3011);
      onFoot(true);afterBoarding=h.positionRecentreDispatch(13,3022);
    });
    check(!boarded&&!afterBoarding&&h.currentOrigin.x==6.0&&h.positionRecentres==3,
          "F123: leaving on foot cancels a tracking retry permanently, including after on foot begins again");

    bool changed=true;
    f.owner.invoke([&]{
      f.fake.flags=0;h.positionRecentreDispatch(14,4000);
      h.poses.resetOrigin(h.compositorGeneration);
      f.fake.flags=launch_fixture::tracked;
      changed=h.positionRecentreDispatch(14,4011);
    });
    check(!changed&&h.currentOrigin.x==6.0&&h.positionRecentres==3,
          "F123: a reference generation change cancels the old tracking retry before it can move the new origin");

    bool expired=true;
    f.owner.invoke([&]{
      f.fake.flags=0;h.positionRecentreDispatch(15,5000);
      f.fake.flags=launch_fixture::tracked;
      expired=h.positionRecentreDispatch(15,5000+h.kPositionRecentreRetryMs);
    });
    check(!expired&&h.currentOrigin.x==6.0&&h.positionRecentres==3&&h.positionRecentreHandled==15,
          "F123: tracking retries expire after 0.5 s, so an old request cannot move the origin long after its fade");

    bool provider=false;
    f.owner.invoke([&]{provider=h.features.acquire(GetModuleHandleW(nullptr),reinterpret_cast<ID3D11Device*>(uintptr_t(1)),1)==S_OK;});
    check(provider,"(the position-only roundtrip admits the actual host look/eye/ADS polls through a module-owned test provider)");
    if(provider) {
      const auto same=[](const XrPosef& a,const XrPosef& b) {
        return std::fabs(a.position.x-b.position.x)<2e-5&&std::fabs(a.position.y-b.position.y)<2e-5&&std::fabs(a.position.z-b.position.z)<2e-5&&
          std::fabs(a.orientation.x-b.orientation.x)<2e-6&&std::fabs(a.orientation.y-b.orientation.y)<2e-6&&std::fabs(a.orientation.z-b.orientation.z)<2e-6&&std::fabs(a.orientation.w-b.orientation.w)<2e-6;
      };
      for(int sign:{-1,1}) {
        bool callOnly=false,pollStable=false,repeatStable=false,eyeStable=false,unwind=false,reset=false;
        f.owner.invoke([&]{
          const double baselineYaw=sign*0.41,base=sign*1.8,comp=sign*0.8444,pitch=sign*0.17;
          const double eye[3]={0.12,-0.08,0.18}; // aggregate eye alignment, including ADS sight translation
          f.fake.head.position={5.0f,1.6f,-2.0f};f.fake.flags=launch_fixture::tracked;f.fake.locateResult=XR_SUCCESS;
          h.currentOrigin={1.0,0.0,0.0,baselineYaw};h.lookSeated={1.3,-0.2,0.4,baselineYaw+comp};h.lookSeatedKnown=true;
          h.lookBase=base;h.lookComp=comp;h.lookWanted=base+comp;h.lookRestorePending=false;
          h.featureFrame={sizeof(EdvrNativeFrameOutput),EDVR_NATIVE_FRAME_VERSION_10};h.featureFrameKnown=true;
          h.featureFrame.lookYawReset=h.lookResetSeen;h.featureFrame.lookYawTotal=float(base+comp);
          h.eyeProviderSeen=true;h.eyeSerialSeen=h.eyeAppliedSerial=99;
          // F134: eyeWanted is a SEATED vector, eyeApplied the LOCAL translation the origin carries: the seated offset turned by the seat's heading.
          double eyeLocal[3];eyeoffset::localFromSeated(baselineYaw+comp,eye,eyeLocal);
          for(int i=0;i<3;++i){h.eyeWanted[i]=eye[i];h.eyeApplied[i]=eyeLocal[i];}
          h.rotWanted[0]=h.rotWanted[1]=h.rotApplied[0]=h.rotApplied[1]=0;
          h.adsWanted=h.adsApplied=pitch;h.adsPivot[0]=5.0;h.adsPivot[1]=double(1.6f);h.adsPivot[2]=-2.0;
          h.seated.replace(h.seatedPoseFor(h.lookSeated,0,pitch));
          const XrQuaternionf beforeOrientation=f.fake.origin.orientation;
          const bool applied=h.applyPositionRecentre(100+sign);
          const XrPosef atCall=f.fake.origin;
          XrSpaceLocation headAtCall{XR_TYPE_SPACE_LOCATION};launch_fixture::locate(view(),h.seated.space(),200,&headAtCall);
          const XrPosef expected=h.seatedPoseFor(SeatPose{5.0+eyeLocal[0],double(1.6f)+eyeLocal[1],-2.0+eyeLocal[2],baselineYaw+comp},0,pitch);
          callOnly=applied&&same(atCall,expected)&&std::memcmp(&atCall.orientation,&beforeOrientation,sizeof(beforeOrientation))==0&&
            h.currentOrigin.yaw==baselineYaw&&h.lookBase==base&&h.lookComp==comp&&h.lookWanted==base+comp&&h.eyeAppliedSerial==99&&h.adsApplied==pitch;
          h.lookPoll();h.eyePoll();h.rotPoll();h.adsPoll();
          XrSpaceLocation headAfterPoll{XR_TYPE_SPACE_LOCATION};launch_fixture::locate(view(),h.seated.space(),200,&headAfterPoll);
          pollStable=same(atCall,f.fake.origin)&&same(headAtCall.pose,headAfterPoll.pose);
          // A normal requested eye-alignment change still lands once, then its inverse returns exactly.
          h.eyeWanted[0]+=0.02;h.eyePoll();h.eyeWanted[0]=eye[0];h.eyePoll();
          eyeStable=same(atCall,f.fake.origin)&&h.eyeAppliedSerial==99;
          f.fake.head.position.x+=0.2f;f.fake.head.position.y+=0.03f;f.fake.head.position.z-=0.1f;
          const bool repeated=h.applyPositionRecentre(110+sign);
          const XrPosef atRepeat=f.fake.origin;
          XrSpaceLocation headAtRepeat{XR_TYPE_SPACE_LOCATION};launch_fixture::locate(view(),h.seated.space(),200,&headAtRepeat);
          h.lookPoll();h.eyePoll();h.rotPoll();h.adsPoll();
          XrSpaceLocation headRepeatPoll{XR_TYPE_SPACE_LOCATION};launch_fixture::locate(view(),h.seated.space(),200,&headRepeatPoll);
          repeatStable=repeated&&same(atRepeat,f.fake.origin)&&same(headAtRepeat.pose,headRepeatPoll.pose)&&same(headAtCall.pose,headAtRepeat.pose)&&
            h.currentOrigin.yaw==baselineYaw&&h.lookBase==base&&h.lookComp==comp;
          for(int step=1;step<=6;++step) {
            const double left=1.0-double(step)/6.0;h.lookWanted=base+comp*left;h.adsWanted=pitch*left;
            h.lookPoll();h.eyePoll();h.adsPoll();
          }
          unwind=std::fabs(h.lookComp)<1e-9&&std::fabs(h.lookSeated.yaw-baselineYaw)<1e-9&&h.currentOrigin.yaw==baselineYaw&&std::fabs(h.adsApplied)<1e-9;
          ++h.featureFrame.lookYawReset;h.lookWanted=base;h.lookPoll();h.eyePoll();h.adsPoll();
          double eyeFinal[3];eyeoffset::localFromSeated(baselineYaw,eye,eyeFinal);   // F134: at the baseline heading the seated offset is this LOCAL translation
          const SeatPose final{double(f.fake.head.position.x)+eyeFinal[0],double(f.fake.head.position.y)+eyeFinal[1],double(f.fake.head.position.z)+eyeFinal[2],baselineYaw};
          reset=same(f.fake.origin,xrFromSeatPose(final))&&h.currentOrigin.yaw==baselineYaw&&h.lookComp==0&&h.lookBase==base;
        });
        check(callOnly,"F123 ADS roundtrip: position-only call preserves full live orientation, nonzero yaw base/compensation and eye/pitch overlays (both yaw signs)");
        check(pollStable,"F123 ADS roundtrip: the next real look/eye/rotation/ADS polls preserve the call's world-space transform and reported head pose");
        check(eyeStable,"F123 ADS roundtrip: an eye-alignment change is applied once and its inverse returns to the captured transform");
        check(repeatStable,"F123 ADS roundtrip: repeated recenter after head translation preserves the same reported head pose and original yaw account");
        check(unwind,"F123 ADS roundtrip: subsequent ADS yaw/pitch unwind reaches the original baseline yaw, without absorbing either sign of live compensation");
        check(reset,"F123 ADS roundtrip: later reset restores the translated baseline plus one eye offset, with no permanent ADS yaw or pitch");
      }
      f.owner.invoke([&]{h.features.close();});
    }
    f.owner.stop();
  }
  if(hadPrevious)SetEnvironmentVariableW(L"LOCALAPPDATA",previous);else SetEnvironmentVariableW(L"LOCALAPPDATA",nullptr);
}

}  // namespace edvr::openxr::test
