#pragma once
// F189 (#144): the seat origin is applied in EDVR's pose math, and the runtime is never asked for a reference space for the seat. Before, every origin change (an eye alignment, a
// look turn, an ADS pitch, a position recentre, a reset) made a new reference space and destroyed the old one; the game's poses and the layers' physical poses were the numbers
// of that space. This case is the equivalence of the two: one fake runtime that knows the natural space and nothing else (launch_centre_cases.h: a call to xrCreateReferenceSpace
// or xrDestroySpace is counted, a locate against any other space fails argumentsValid), a tracked head with velocity, and a sequence of origin changes each made by the real
// host function that makes it. At every step, for the origin the host holds at that call:
//   1. the head, the two eye views and the head's velocity the host hands the game (locateGeometry and locateHeadAt on the frame's snapshot, exactly as waitPoses calls them, and
//      locateHeadFor, the owner's answer to the game's own pose calls) are the numbers the OLD seated reference space gave: origin^-1 * natural, velocities turned by the inverse
//      rotation, within 1e-5 (the oracle is launch_centre_cases.h's oldSpacePose, written out without seat_transform.h);
//   2. the pose each submitted layer view carries, mapped to the natural space (the layer's space is the natural one now), is the physical pose the old path produced: the pose the
//      runtime located the eye at. Both through the function the renderers call (seatxf::toNatural, pinned in openxr_stereo_test on a real layer) and through the tree's matrix
//      helpers on the origin and the seated view;
//   3. nothing was created or destroyed in the runtime, and each origin change was one stored origin.
// A late pair replayed after the origin moved goes out at the physical pose it was drawn at (its own frame's origin), which is not the pose the current origin would give it.
//
// Not waitPoses itself: it needs a live frame boundary, graphics and the feature client no fixture here sets up (frame_end_overlap_cases.h says so). The calls under test are the
// ones waitPoses makes, in its order, on the same host members (takeFrameSeat, frameSeat). The renderers are not driven here (openxr_stereo_test drives them).
#include "position_recentre_cases.h"
#include <dxgi.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace edvr::openxr::test {
namespace seat_origin_fixture {
// The binding is initialized against fakes that make the handles the launch fixture already uses (session, LOCAL, VIEW, STAGE), and never count into the fixture's creates and
// destroys: those are the seat path's, which must stay 0.
inline LUID luid{};
inline D3D_FEATURE_LEVEL level{};
inline XrResult XRAPI_PTR requirements(XrInstance,XrSystemId,XrGraphicsRequirementsD3D11KHR* out) {out->adapterLuid=luid;out->minFeatureLevel=level;return XR_SUCCESS;}
inline XrResult XRAPI_PTR makeSession(XrInstance,const XrSessionCreateInfo*,XrSession* out) {*out=launch_fixture::session();return XR_SUCCESS;}
inline XrResult XRAPI_PTR dropSession(XrSession) {return XR_SUCCESS;}
inline XrResult XRAPI_PTR spaces(XrSession,uint32_t capacity,uint32_t* count,XrReferenceSpaceType* values) {
  *count=2;if(!capacity)return XR_SUCCESS;if(capacity<2)return XR_ERROR_SIZE_INSUFFICIENT;
  values[0]=XR_REFERENCE_SPACE_TYPE_LOCAL;values[1]=XR_REFERENCE_SPACE_TYPE_VIEW;return XR_SUCCESS;
}
inline XrResult XRAPI_PTR makeSpace(XrSession,const XrReferenceSpaceCreateInfo* info,XrSpace* out) {
  *out=info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL?launch_fixture::local():
       info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW?launch_fixture::view():launch_fixture::stage();
  return XR_SUCCESS;
}
inline XrResult XRAPI_PTR dropSpace(XrSpace) {return XR_SUCCESS;}
inline bool poseNear(const XrPosef& a,const XrPosef& b,float tolerance=1e-5f) {
  return std::fabs(a.position.x-b.position.x)<=tolerance&&std::fabs(a.position.y-b.position.y)<=tolerance&&std::fabs(a.position.z-b.position.z)<=tolerance&&
    std::fabs(a.orientation.x-b.orientation.x)<=tolerance&&std::fabs(a.orientation.y-b.orientation.y)<=tolerance&&
    std::fabs(a.orientation.z-b.orientation.z)<=tolerance&&std::fabs(a.orientation.w-b.orientation.w)<=tolerance;
}
inline bool vectorNear(const XrVector3f& a,const XrVector3f& b,float tolerance=1e-5f) {
  return std::fabs(a.x-b.x)<=tolerance&&std::fabs(a.y-b.y)<=tolerance&&std::fabs(a.z-b.z)<=tolerance;
}
// A pose as the OpenVR matrix the game reads, for comparing the game's head pose with the oracle's.
inline bool matrixNear(const vr::HmdMatrix34_t& m,const XrPosef& pose,float tolerance=1e-5f) {
  double expected[4][4];detail::rigid(pose,expected);
  for(int i=0;i<3;++i)for(int j=0;j<4;++j)if(std::fabs(double(m.m[i][j])-expected[i][j])>tolerance)return false;
  return true;
}
// The old path's physical pose of a layer view, by the tree's matrix helpers: the seated space's origin times the pose submitted in it.
inline bool physicalNear(const XrPosef& origin,const XrPosef& submittedSeated,const XrPosef& physical,float tolerance=1e-5f) {
  double o[4][4],s[4][4],product[4][4],want[4][4];
  detail::rigid(origin,o);detail::rigid(submittedSeated,s);detail::product(o,s,product);detail::rigid(physical,want);
  for(int i=0;i<3;++i)for(int j=0;j<4;++j)if(std::fabs(product[i][j]-want[i][j])>tolerance)return false;
  return true;
}
} // namespace seat_origin_fixture

template<class Check> void runSeatOriginCases(Check&& check) {
  using namespace launch_fixture;
  using namespace seat_origin_fixture;
  wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
  wchar_t leaf[96];std::swprintf(leaf,96,L"edvr_seat_origin_%lu_%llu",GetCurrentProcessId(),(unsigned long long)GetTickCount64());
  const std::wstring scratch=std::wstring(temp)+leaf;
  CreateDirectoryW(scratch.c_str(),nullptr);
  wchar_t previous[MAX_PATH]{};
  const DWORD hadPrevious=GetEnvironmentVariableW(L"LOCALAPPDATA",previous,MAX_PATH);
  SetEnvironmentVariableW(L"LOCALAPPDATA",scratch.c_str());

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  const auto createDevice=systemD3D11CreateDevice();
  check(createDevice&&SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,nullptr)),"(seat origin: a WARP device for the binding)");
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterDesc{};
  const bool haveAdapter=device&&SUCCEEDED(device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&adapterDesc));
  check(haveAdapter,"(seat origin: the device's adapter)");
  luid=adapterDesc.AdapterLuid;

  OwnerBuiltFixture f;
  check(haveAdapter&&f.route.bind()&&f.owner.start()&&f.owner.invoke([&]{f.build();})&&f.initialized,"(the seat origin cases' host is built on its own owner thread)");
  if(haveAdapter&&f.initialized) {
    auto& h=*f.hostPtr;
    const BindingDispatch bindingApi{requirements,makeSession,dropSession,spaces,makeSpace,dropSpace};
    XrResult bound=XR_ERROR_RUNTIME_FAILURE;
    f.owner.invoke([&]{bound=h.binding.initialize(bindingApi,instance(),1,device.Get(),XR_REFERENCE_SPACE_TYPE_LOCAL);});
    check(bound==XR_SUCCESS&&h.binding.localSpace()==local()&&h.binding.viewSpace()==view(),"(seat origin: the host's binding is initialized against the fake runtime's handles)");
    bool provider=false;
    f.owner.invoke([&]{provider=h.features.acquire(GetModuleHandleW(nullptr),reinterpret_cast<ID3D11Device*>(uintptr_t(1)),1)==S_OK;});
    check(provider,"(seat origin: the actual host look/eye/ADS polls are admitted through the module-owned test provider)");
    if(bound==XR_SUCCESS&&provider) {
      XrViewConfigurationView sizes[2]{};
      for(auto& s:sizes){s={XR_TYPE_VIEW_CONFIGURATION_VIEW};s.recommendedImageRectWidth=3072;s.recommendedImageRectHeight=3264;}
      auto& fake=f.fake;
      // A tracked head turned 60 degrees and pitched, off the origin, with a velocity: nothing about it is axis-aligned, so a missed or doubled conversion cannot hide.
      const auto setHead=[&](double yaw,double pitch,float x,float y,float z) {
        const double cy=std::cos(yaw/2),sy=std::sin(yaw/2),cp=std::cos(pitch/2),sp=std::sin(pitch/2);
        const XrQuaternionf yawQ{0,float(sy),0,float(cy)},pitchQ{float(sp),0,0,float(cp)};   // yaw about +y, then pitch about the turned x
        fake.head.orientation=product(yawQ,pitchQ);
        fake.head.position={x,y,z};
      };
      const SeatPose storedSeat{1.0,0.0,0.0,0.5};
      struct Frame3 {XrPosef origin;XrPosef seatedViews[2];XrPosef naturalViews[2];};
      std::vector<Frame3> frames;
      int step=0;
      const auto frameAt=[&](const char* what) {
        ++step;
        const XrPosef origin=h.seated.origin();
        const std::string label=std::string("seat origin, ")+what+": ";
        Frame frame{};frame.status=FrameStatus::Open;frame.sequence=uint64_t(100+step);frame.predictedDisplayTime=XrTime(1000*step);frame.predictedDisplayPeriod=10;
        h.takeFrameSeat();   // waitPoses: the natural handle and the origin it stands for, taken together
        GeometryInput located{};XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        const unsigned createsBefore=fake.creates,destroysBefore=fake.destroys;
        const XrResult located_r=locateGeometry({h.api.locateViews,h.api.locateSpace},h.binding,frame,h.geometryGeneration,sizes,located,&velocity,h.frameSeat());
        TimedHeadPose game{};
        const XrResult game_r=locateHeadAt(h.api.locateSpace,h.view,h.frameSeat(),frame.predictedDisplayTime+frame.predictedDisplayPeriod,true,game);
        check(located_r==XR_SUCCESS&&game_r==XR_SUCCESS&&fake.argumentsValid&&fake.creates==createsBefore&&fake.destroys==destroysBefore,
          (label+"the frame's locates succeed, all against the natural space, and make no reference space").c_str());
        check(poseNear(h.frameOrigin,origin,0)&&h.frameSpace==fake.naturalSpace,(label+"the frame snapshot is the natural handle and the origin the host holds").c_str());
        // 1. what the game is handed.
        const XrPosef wantHead=oldSpacePose(origin,fake.head);
        check(poseNear(located.headPose,wantHead)&&located.headFlags==fake.flags,(label+"the located head is origin^-1 * natural (the old space's numbers), flags kept").c_str());
        XrPosef naturalViews[2];
        for(unsigned eye=0;eye<2;++eye) {
          naturalViews[eye]=fake.head;naturalViews[eye].position.x+=eye?.032f:-.032f;   // launch_fixture::locateViews' views
          check(poseNear(located.views[eye].pose,oldSpacePose(origin,naturalViews[eye])),(label+(eye?"the right":"the left")+" eye view is origin^-1 * natural").c_str());
        }
        check(game.pose.bPoseIsValid&&matrixNear(game.pose.mDeviceToAbsoluteTracking,oldSpacePose(origin,fake.head)),
          (label+"the gameplay head the game's pose array carries is origin^-1 * natural").c_str());
        const XrVector3f wantLinear=oldSpaceVelocity(origin,fake.linearVelocity),wantAngular=oldSpaceVelocity(origin,fake.angularVelocity);
        const vr::HmdVector3_t& gl=game.pose.vVelocity;const vr::HmdVector3_t& ga=game.pose.vAngularVelocity;
        check(game.linearVelocityValid&&game.angularVelocityValid&&vectorNear(XrVector3f{gl.v[0],gl.v[1],gl.v[2]},wantLinear)&&
          vectorNear(XrVector3f{ga.v[0],ga.v[1],ga.v[2]},wantAngular)&&velocity.velocityFlags==fake.velocityFlags&&
          vectorNear(velocity.linearVelocity,wantLinear)&&vectorNear(velocity.angularVelocity,wantAngular),
          (label+"linear and angular velocity (the render pose's and the game's) are turned by the inverse rotation, flags kept").c_str());
        vr::TrackedDevicePose_t asked{};HeadCall call;call.thread=GetCurrentThreadId();call.rva=0x4E3881;
        check(h.locateHeadFor(h.geometryGeneration,vr::TrackingUniverseSeated,0.0f,call,asked)&&asked.bPoseIsValid&&
          matrixNear(asked.mDeviceToAbsoluteTracking,oldSpacePose(origin,fake.head)),(label+"the game's own pose call is answered with origin^-1 * natural").c_str());
        // 2. what the layer is submitted at: each view's seated pose with the frame's origin applied is where the runtime put the eye.
        for(unsigned eye=0;eye<2;++eye) {
          const XrPosef submitted=seatxf::toNatural(h.frameOrigin,located.views[eye].pose);
          check(poseNear(submitted,naturalViews[eye])&&physicalNear(origin,located.views[eye].pose,naturalViews[eye]),
            (label+(eye?"the right":"the left")+" layer view, mapped to the natural space, is the physical pose the old path produced").c_str());
        }
        Frame3 kept{origin,{located.views[0].pose,located.views[1].pose},{naturalViews[0],naturalViews[1]}};
        frames.push_back(kept);
      };
      f.owner.invoke([&]{
        h.seatEnsureLoaded();
        SeatContext c{};c.published=c.keep=c.gameplay=true;c.onFootKnown=c.onFoot=true;
        h.seat.setSeat(storedSeat);h.seat.onContext(c,GetTickCount64());h.seat.noteOrigin(true);
        h.featureFrame={sizeof(EdvrNativeFrameOutput),EDVR_NATIVE_FRAME_VERSION_10};h.featureFrameKnown=true;
        h.featureFrame.lookYawReset=h.lookResetSeen;h.featureFrame.lookYawTotal=0.f;
        h.eyeProviderSeen=true;h.eyeSerialSeen=h.eyeAppliedSerial=1;
        h.rotWanted[0]=h.rotWanted[1]=h.rotApplied[0]=h.rotApplied[1]=0;
        fake.velocityFlags=XR_SPACE_VELOCITY_LINEAR_VALID_BIT|XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        fake.linearVelocity={0.30f,-0.20f,0.50f};fake.angularVelocity={0.10f,0.70f,-0.40f};
        setHead(1.05,0.18,5.0f,1.6f,-2.0f);

        // Step 0: no origin yet. The seated numbers are the runtime's own, bit for bit.
        check(h.seated.originSets()==0&&seatxf::isIdentity(h.seated.origin()),"seat origin: before the first origin the seated origin is identity");
        {
          Frame frame{};frame.status=FrameStatus::Open;frame.sequence=99;frame.predictedDisplayTime=500;frame.predictedDisplayPeriod=10;
          h.takeFrameSeat();GeometryInput located{};
          locateGeometry({h.api.locateViews,h.api.locateSpace},h.binding,frame,h.geometryGeneration,sizes,located,nullptr,h.frameSeat());
          check(std::memcmp(&located.headPose,&fake.head,sizeof(XrPosef))==0,"seat origin: with no origin set the located head is the runtime's, bit for bit");
        }
        frameAt("no origin yet");

        const auto moved=[&](const char* what,uint64_t before,bool applied){
          check(applied&&h.seated.originSets()==before+1&&fake.creates==0&&fake.destroys==0,
            (std::string("seat origin, ")+what+": one origin stored, no reference space made or destroyed").c_str());
        };
        // A reset (the game's own, from the head: heading and position under it).
        {
          XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};head.locationFlags=tracked;head.pose=fake.head;
          const uint64_t before=h.seated.originSets();
          h.seatTraceDecision="honour_press";
          moved("a reset",before,h.applySeatedReset(head,"seated_reset",true));
          frameAt("after a reset");
        }
        // An eye alignment apply: the seated offset the provider wants, folded into the origin.
        {
          h.eyeWanted[0]=0.05;h.eyeWanted[1]=-0.03;h.eyeWanted[2]=0.08;
          const uint64_t before=h.seated.originSets();
          h.eyePoll();
          moved("an eye alignment apply",before,true);
          frameAt("after an eye alignment apply");
        }
        // A look turn: the origin turns about the physical head by the compensation the provider's total asks for.
        {
          fake.head.position.x+=0.07f;fake.head.position.z-=0.04f;fake.linearVelocity={-0.10f,0.30f,0.20f};
          h.lookWanted=h.lookBase+0.35;
          const uint64_t before=h.seated.originSets();
          h.lookPoll();
          moved("a look turn",before,h.lookApplies==1);
          frameAt("after a look turn");
        }
        // An ADS pitch overlay: the origin pitched about the head's right axis.
        {
          fake.head.position.y-=0.05f;fake.angularVelocity={-0.20f,0.10f,0.60f};
          h.adsWanted=0.21;
          const uint64_t before=h.seated.originSets();
          h.adsPoll();
          moved("an ADS pitch",before,h.adsApplies==1);
          frameAt("after an ADS pitch");
        }
        // A position-only recentre: the origin goes under the head, its yaw and the live account kept.
        {
          fake.head.position.x-=0.11f;fake.head.position.z+=0.09f;
          h.adsWanted=h.adsApplied;   // the overlay stays as applied
          const uint64_t before=h.seated.originSets();
          const bool applied=h.applyPositionRecentre(7);
          moved("a position recentre",before,applied);
          frameAt("after a position recentre");
        }
        // A second reset, with the head elsewhere and turned: the origin is the head's heading and position again.
        {
          setHead(-0.4,-0.25,-1.5f,1.45f,3.0f);fake.linearVelocity={0.5f,0.1f,-0.3f};
          XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};head.locationFlags=tracked;head.pose=fake.head;
          const uint64_t before=h.seated.originSets();
          h.seatTraceDecision="honour_press";
          moved("a second reset",before,h.applySeatedReset(head,"seated_reset",true));
          frameAt("after a second reset");
        }
        // The origins differ from step to step (so the cases above could tell a stale or missing conversion), and the runtime stayed out of it.
        bool allDistinct=frames.size()==7;
        for(size_t i=1;i<frames.size();++i)allDistinct=allDistinct&&!poseNear(frames[i].origin,frames[i-1].origin,1e-4f);
        check(allDistinct,"seat origin: every step's origin differs from the step before it (no step is a no-op the conversion could pass vacuously)");
        check(h.seated.originSets()==6&&fake.creates==0&&fake.destroys==0&&fake.argumentsValid,
          "seat origin: six origin changes were six stored origins; the runtime was never asked for a reference space and never located any space but the natural one");
        // A late pair replayed after the origin moved is submitted with the origin it was located with.
        if(frames.size()==7) {
          const Frame3& drawn=frames[3];            // the frame after the look turn
          const Frame3& now=frames[4];              // after the ADS pitch: the origin has moved on
          bool replayAtItsOwnPose=true,currentWouldMove=false;
          for(unsigned eye=0;eye<2;++eye) {
            const XrPosef withPrevious=seatxf::toNatural(drawn.origin,drawn.seatedViews[eye]);
            const XrPosef withCurrent=seatxf::toNatural(now.origin,drawn.seatedViews[eye]);
            replayAtItsOwnPose=replayAtItsOwnPose&&poseNear(withPrevious,drawn.naturalViews[eye])&&physicalNear(drawn.origin,drawn.seatedViews[eye],drawn.naturalViews[eye]);
            currentWouldMove=currentWouldMove||!poseNear(withCurrent,drawn.naturalViews[eye],1e-3f);
          }
          check(replayAtItsOwnPose,"seat origin: a kept pair submitted with its own frame's origin is at the physical pose it was drawn at");
          check(currentWouldMove,"seat origin: ...which the current origin would not give it (so the replay must carry the origin it was located with, as sceneFinished stores it)");
        }
        h.binding.shutdown();
        h.features.close();
      });
    }
    f.owner.stop();
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA",hadPrevious&&hadPrevious<MAX_PATH?previous:nullptr);
  seat_order_fixture::removeTree(scratch);
}
} // namespace edvr::openxr::test
