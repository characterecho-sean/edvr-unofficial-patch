#include "../../src/openxr/seated_space.h"
#include "../../src/openxr/reset_events.h"
#include "../../src/openxr/system_publication.h"
#include "../../src/openxr/compositor_publication.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace edvr::openxr;
namespace {
unsigned checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
const XrSession session=reinterpret_cast<XrSession>(1);
const XrSpace local=reinterpret_cast<XrSpace>(2);
// F189 (#144): the seated space is the natural space and a stored origin. It holds no create or destroy call, so there is no runtime for these cases to fake: the session handle
// below is a dummy nothing could call, and a setOrigin that reached for one would not link. The host-level rig (openxr_native_test) counts a fake runtime's creates and
// destroys over real origin changes; this one pins the arithmetic and the contract of the object.
using seatxf::isIdentity;
const XrPosef identityPose{{0,0,0,1},{0,0,0}};
bool nearPose(const XrPosef& a,const XrPosef& b,float tol=1e-5f) {
  // q and -q are one rotation; the rig's poses keep w >= 0, so a plain compare is enough, with the sign folded in for the products.
  const float sign=(a.orientation.x*b.orientation.x+a.orientation.y*b.orientation.y+a.orientation.z*b.orientation.z+a.orientation.w*b.orientation.w)<0?-1.f:1.f;
  return std::fabs(a.orientation.x-sign*b.orientation.x)<=tol&&std::fabs(a.orientation.y-sign*b.orientation.y)<=tol&&
         std::fabs(a.orientation.z-sign*b.orientation.z)<=tol&&std::fabs(a.orientation.w-sign*b.orientation.w)<=tol&&
         std::fabs(a.position.x-b.position.x)<=tol&&std::fabs(a.position.y-b.position.y)<=tol&&std::fabs(a.position.z-b.position.z)<=tol;
}
bool nearVec(const XrVector3f& v,float x,float y,float z,float tol=1e-5f) {return std::fabs(v.x-x)<=tol&&std::fabs(v.y-y)<=tol&&std::fabs(v.z-z)<=tol;}
XrPosef poseOf(double yaw,double pitch,double roll,float x,float y,float z) {
  // yaw about +y, then pitch about +x, then roll about +z: a general rotation, from half angles.
  const double cy=std::cos(yaw/2),sy=std::sin(yaw/2),cp=std::cos(pitch/2),sp=std::sin(pitch/2),cr=std::cos(roll/2),sr=std::sin(roll/2);
  const seatxf::Quat qy{0,sy,0,cy},qp{sp,0,0,cp},qr{0,0,sr,cr};
  const seatxf::Quat q=seatxf::mul(qy,seatxf::mul(qp,qr));
  XrPosef p;p.orientation=seatxf::toXr(q);p.position={x,y,z};return p;
}
// The tree's own matrix helpers (geometry_snapshot.h) as the independent check: inverse(rigid(origin)) * rigid(natural) is the seated pose as a matrix.
double matrixDifference(const XrPosef& origin,const XrPosef& natural,const XrPosef& seated) {
  double o[4][4],n[4][4],s[4][4],inverse[4][4],expected[4][4];
  detail::rigid(origin,o);detail::rigid(natural,n);detail::rigid(seated,s);detail::inverseRigid(o,inverse);detail::product(inverse,n,expected);
  double worst=0;
  for(int i=0;i<3;++i)for(int j=0;j<4;++j)worst=std::fmax(worst,std::fabs(expected[i][j]-s[i][j]));
  return worst;
}
void spaces() {
  SeatedSpace owned;
  check(!owned.begin(nullptr,local),"null session rejected");
  check(!owned.begin(session,nullptr),"null natural space rejected");
  check(!owned.begin(session,local,XR_REFERENCE_SPACE_TYPE_VIEW),"only LOCAL and STAGE are natural spaces");
  check(owned.setOrigin(identityPose)==XR_ERROR_CALL_ORDER_INVALID&&!owned.space(),"before begin there is no origin to set");
  check(owned.begin(session,local)&&owned.space()==local,"starts with borrowed LOCAL");
  check(isIdentity(owned.origin())&&owned.originSets()==0&&SeatedSpace::spaceCreates()==0,"the origin is identity until the first set, and nothing has been created");
  check(!owned.begin(session,local),"double begin rejected");

  XrPosef bad=poseOf(0.4,0,0,1,2,3);bad.orientation.w*=2;
  check(owned.setOrigin(bad)==XR_ERROR_POSE_INVALID&&owned.space()==local&&isIdentity(owned.origin())&&owned.originSets()==0,"a bad pose changes nothing");
  XrPosef notFinite=poseOf(0.4,0,0,1,2,3);notFinite.position.y=std::numeric_limits<float>::quiet_NaN();
  check(owned.setOrigin(notFinite)==XR_ERROR_POSE_INVALID&&isIdentity(owned.origin()),"a non-finite pose changes nothing");

  const XrPosef first=poseOf(0.5,0,0,1,2,3);
  check(owned.setOrigin(first)==XR_SUCCESS,"first origin");
  check(owned.space()==local&&std::memcmp(&owned.origin(),&first,sizeof(XrPosef))==0&&owned.originSets()==1,"the exact origin is stored and the handle is still the natural space");
  const SeatedFrame snapshot=owned.frame();
  const XrPosef second=poseOf(-0.9,0.17,0,4,5,6);
  check(owned.setOrigin(second)==XR_SUCCESS&&owned.space()==local&&std::memcmp(&owned.origin(),&second,sizeof(XrPosef))==0&&owned.originSets()==2,
    "second origin: the same handle, the new origin, two sets");
  check(snapshot.space==local&&std::memcmp(&snapshot.origin,&first,sizeof(XrPosef))==0&&std::memcmp(&owned.frame().origin,&second,sizeof(XrPosef))==0,
    "a frame's snapshot keeps the origin it was taken with whatever the origin does after");
  check(SeatedSpace::spaceCreates()==0,"two origin changes made no reference space");
  check(owned.shutdown()==XR_SUCCESS&&!owned.space()&&isIdentity(owned.origin())&&owned.originSets()==0,"shutdown clears the handle and the origin");
  check(owned.shutdown()==XR_SUCCESS,"idempotent shutdown");
  check(owned.setOrigin(first)==XR_ERROR_CALL_ORDER_INVALID,"no origin after shutdown");
  check(owned.begin(session,local)&&owned.space()==local&&isIdentity(owned.origin()),"reinit borrowed state, identity origin");
  owned.shutdown();

  {SeatedSpace stageSeat;
    check(stageSeat.begin(session,local,XR_REFERENCE_SPACE_TYPE_STAGE)&&stageSeat.naturalType()==XR_REFERENCE_SPACE_TYPE_STAGE,"stage natural space begins");
    check(stageSeat.setOrigin(first)==XR_SUCCESS&&stageSeat.space()==local,"stage origin: the natural STAGE handle again");
    check(stageSeat.shutdown()==XR_SUCCESS&&stageSeat.naturalType()==XR_REFERENCE_SPACE_TYPE_LOCAL,"stage seat shut down");}

  // ---- the arithmetic ------------------------------------------------------------------------------------------------------------
  // Hand-worked: origin = yaw 90 degrees about +y at (1,0,2). Its inverse turns (x,y,z) into (-z,y,x) after the offset is taken off.
  {
    const float h=0.70710678118f;
    const XrPosef origin{{0,h,0,h},{1,0,2}};
    const XrPosef natural{{0,0,0,1},{10,20,30}};
    const XrPosef seated=SeatedSpace::toSeated(origin,natural);
    check(nearVec(seated.position,-28,20,9)&&std::fabs(seated.orientation.y+h)<=1e-6f&&std::fabs(seated.orientation.w-h)<=1e-6f&&seated.orientation.x==0&&seated.orientation.z==0,
      "hand-worked: toSeated of (10,20,30) at origin yaw 90 / (1,0,2) is position (-28,20,9), turned back 90 degrees");
    const XrPosef back=SeatedSpace::toNatural(origin,seated);
    check(nearPose(back,natural),"hand-worked: toNatural returns the located pose");
    const XrPosef atOrigin=SeatedSpace::toNatural(origin,identityPose);
    check(nearPose(atOrigin,origin),"the seated space's own origin point is the origin pose in the natural space");
    check(nearVec(SeatedSpace::velocityToSeated(origin,XrVector3f{1,2,3}),-3,2,1)&&nearVec(SeatedSpace::velocityToSeated(origin,XrVector3f{4,5,6}),-6,5,4),
      "hand-worked: a velocity is turned by the inverse rotation and not shifted by the origin's position");
    owned.begin(session,local);owned.setOrigin(origin);
    check(nearPose(owned.toSeated(natural),seated,0)&&nearPose(owned.toNatural(seated),back,0)&&nearVec(owned.velocityToSeated(XrVector3f{1,2,3}),-3,2,1),
      "the member forms use the stored origin and give the explicit-origin forms' numbers");
    owned.shutdown();
  }
  // toSeated(toNatural(p)) == p, and toSeated agrees with the tree's matrix helpers, over many origins (yaw, pitch and roll, so the F61/F62b pitch overlays are covered) and poses.
  {
    uint32_t seed=12345;
    const auto next=[&]{seed=seed*1664525u+1013904223u;return double((seed>>8)&0xFFFF)/65535.0;};
    double worstRound=0,worstMatrix=0,worstVelocity=0;bool allFinite=true;
    for(int i=0;i<2000;++i) {
      const XrPosef origin=poseOf((next()-0.5)*6.2,(next()-0.5)*1.2,(next()-0.5)*0.6,float((next()-0.5)*20),float((next()-0.5)*4),float((next()-0.5)*20));
      const XrPosef p=poseOf((next()-0.5)*6.2,(next()-0.5)*3.0,(next()-0.5)*3.0,float((next()-0.5)*40),float((next()-0.5)*6),float((next()-0.5)*40));
      const XrPosef seated=seatxf::toSeated(origin,p),again=seatxf::toNatural(origin,seated);
      const double dot=std::fabs(double(again.orientation.x)*p.orientation.x+double(again.orientation.y)*p.orientation.y+double(again.orientation.z)*p.orientation.z+double(again.orientation.w)*p.orientation.w);
      worstRound=std::fmax(worstRound,std::fmax(1.0-dot,std::fmax(std::fabs(again.position.x-p.position.x),std::fmax(std::fabs(again.position.y-p.position.y),std::fabs(again.position.z-p.position.z)))));
      worstMatrix=std::fmax(worstMatrix,matrixDifference(origin,p,seated));
      const XrVector3f v{float((next()-0.5)*10),float((next()-0.5)*10),float((next()-0.5)*10)};
      const XrVector3f s=seatxf::velocityToSeated(origin,v);
      worstVelocity=std::fmax(worstVelocity,std::fabs(std::sqrt(double(s.x)*s.x+double(s.y)*s.y+double(s.z)*s.z)-std::sqrt(double(v.x)*v.x+double(v.y)*v.y+double(v.z)*v.z)));
      allFinite=allFinite&&detail::poseValid(seated)&&detail::poseValid(again);
    }
    check(worstRound<=5e-5,"toSeated(toNatural(p)) == p over 2000 random origins and poses (float round trip)");
    check(worstMatrix<=5e-5,"toSeated agrees with inverse(rigid(origin)) * rigid(pose), the tree's own matrix helpers");
    check(worstVelocity<=5e-5&&allFinite,"a velocity keeps its length (it is only turned), and every converted pose is a valid pose");
  }
  // An identity origin returns its input bit for bit; a hair off identity is a real transform.
  {
    const XrPosef p=poseOf(0.3,0.2,0.1,1.5f,-2.5f,3.5f);
    const XrPosef a=seatxf::toSeated(identityPose,p),b=seatxf::toNatural(identityPose,p);
    check(std::memcmp(&a,&p,sizeof(XrPosef))==0&&std::memcmp(&b,&p,sizeof(XrPosef))==0,"an identity origin changes nothing, bit for bit");
    XrPosef slight=identityPose;slight.position.x=1e-4f;
    check(seatxf::toSeated(slight,p).position.x!=p.position.x,"an origin 0.1 mm off identity is applied");
  }
  // An origin whose quaternion is a little off unit (the pose check allows 1e-3 of squared length) is used as the rotation it stands for: no scaling creeps into positions.
  {
    XrPosef origin=poseOf(0.7,0.1,0,2,0,-1);origin.orientation.x*=1.0004f;origin.orientation.y*=1.0004f;origin.orientation.z*=1.0004f;origin.orientation.w*=1.0004f;
    const XrPosef p=poseOf(0.2,0,0,3,1,4);
    const XrPosef seated=seatxf::toSeated(origin,p);
    const double offset=std::sqrt(double(p.position.x-origin.position.x)*(p.position.x-origin.position.x)+
      double(p.position.y-origin.position.y)*(p.position.y-origin.position.y)+double(p.position.z-origin.position.z)*(p.position.z-origin.position.z));
    const double lengthAfter=std::sqrt(double(seated.position.x)*seated.position.x+double(seated.position.y)*seated.position.y+double(seated.position.z)*seated.position.z);
    check(offset>1&&std::fabs(lengthAfter-offset)<=1e-5&&detail::poseValid(origin)&&detail::poseValid(seated),"a near-unit origin turns a position without scaling it");
  }
  // The locate edge's parts: a pose, a location, a velocity, two views, each converted by the validity the runtime gave it and the flags untouched.
  {
    const float h=0.70710678118f;
    const XrPosef origin{{0,h,0,h},{1,0,2}};
    const XrPosef natural{{0,0,0,1},{10,20,30}};
    XrSpaceLocation both{XR_TYPE_SPACE_LOCATION};both.pose=natural;both.locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    seatxf::locationToSeated(origin,both);
    check(nearVec(both.pose.position,-28,20,9)&&std::fabs(both.pose.orientation.y+h)<=1e-6f&&both.locationFlags==(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT),
      "a fully valid location is converted and its flags are kept");
    XrSpaceLocation positionOnly{XR_TYPE_SPACE_LOCATION};positionOnly.pose=natural;positionOnly.locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT;
    seatxf::locationToSeated(origin,positionOnly);
    check(nearVec(positionOnly.pose.position,-28,20,9)&&positionOnly.pose.orientation.w==1&&positionOnly.pose.orientation.y==0,"a position-only location converts the position and leaves the orientation alone");
    XrSpaceLocation orientationOnly{XR_TYPE_SPACE_LOCATION};orientationOnly.pose=natural;orientationOnly.locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    seatxf::locationToSeated(origin,orientationOnly);
    check(nearVec(orientationOnly.pose.position,10,20,30)&&std::fabs(orientationOnly.pose.orientation.y+h)<=1e-6f,"an orientation-only location converts the orientation and leaves the position alone");
    XrSpaceLocation neither{XR_TYPE_SPACE_LOCATION};neither.pose=natural;
    seatxf::locationToSeated(origin,neither);
    check(std::memcmp(&neither.pose,&natural,sizeof(XrPosef))==0,"a location the runtime did not vouch for is not touched");
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};velocity.linearVelocity={1,2,3};velocity.angularVelocity={4,5,6};
    velocity.velocityFlags=XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    seatxf::velocityToSeated(origin,velocity);
    check(nearVec(velocity.linearVelocity,-3,2,1)&&nearVec(velocity.angularVelocity,4,5,6)&&velocity.velocityFlags==XR_SPACE_VELOCITY_LINEAR_VALID_BIT,"only the valid velocity is turned, flags kept");
    velocity.velocityFlags=XR_SPACE_VELOCITY_LINEAR_VALID_BIT|XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    seatxf::velocityToSeated(origin,velocity);
    check(nearVec(velocity.angularVelocity,-6,5,4),"the angular velocity is turned when valid");
    XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};views[0].pose=natural;views[1].pose=natural;
    seatxf::viewsToSeated(origin,XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT,views,2);
    check(nearVec(views[0].pose.position,-28,20,9)&&nearVec(views[1].pose.position,-28,20,9),"both views are converted");
    views[0].pose=natural;seatxf::viewsToSeated(origin,0,views,2);
    check(std::memcmp(&views[0].pose,&natural,sizeof(XrPosef))==0,"views the runtime did not vouch for are not touched");
  }
}
void events() {
  ResetEvents queue;vr::VREvent_t event{};auto pose=invalidHeadPose(true);pose.bPoseIsValid=true;
  pose.eTrackingResult=vr::TrackingResult_Running_OK;pose.mDeviceToAbsoluteTracking.m[0][3]=3;
  vr::TrackedDevicePose_t output{};
  check(!queue.begin(0)&&queue.begin(1)&&!queue.begin(2),"queue init generation");
  check(!queue.room(2)&&queue.room(1),"queue capacity matching generation");
  check(!queue.push(2,1,0,pose)&&!queue.push(1,0,0,pose),"stale/zero origin push rejected");
  check(queue.push(1,4,1000,pose),"event push");
  check(!queue.pop(2,4,vr::TrackingUniverseSeated,1250,event,output),"stale pop does not consume");
  check(queue.pop(1,4,vr::TrackingUniverseSeated,1250,event,output),"event pop");
  check(event.eventType==vr::VREvent_SeatedZeroPoseReset&&event.trackedDeviceIndex==vr::k_unTrackedDeviceIndexInvalid&&
    !event.data.seatedZeroPoseReset.bResetBySystemMenu&&event.eventAgeSeconds==.25f,"historical reset event fields and age");
  check(output.bPoseIsValid&&output.mDeviceToAbsoluteTracking.m[0][3]==3,"event-time pose retained");
  check(!queue.pop(1,4,vr::TrackingUniverseSeated,2000,event,output),"event consumed once");
  queue.push(1,4,1000,pose);
  check(queue.pop(1,5,vr::TrackingUniverseSeated,2000,event,output)&&!output.bPoseIsValid,"old origin pose invalidated but event retained");
  for(auto origin:{vr::TrackingUniverseStanding,vr::TrackingUniverseRawAndUncalibrated}) {
    queue.push(1,5,2000,pose);
    check(queue.pop(1,5,origin,1900,event,output)&&!output.bPoseIsValid&&event.eventAgeSeconds==0,"unavailable origin invalid and clock reversal bounded");
  }
  for(unsigned i=0;i<ResetEvents::capacity;++i)check(queue.push(1,5,2000+i,pose),"bounded fill");
  check(!queue.room(1)&&!queue.push(1,5,3000,pose),"full queue rejects mutation");
  for(unsigned i=0;i<ResetEvents::capacity;++i)
    check(queue.pop(1,5,vr::TrackingUniverseSeated,3000,event,output)&&std::fabs(event.eventAgeSeconds-float(1000-i)/1000)<1e-6,"FIFO wraps correctly");
  queue.push(1,5,0,pose);queue.retire(2);
  check(queue.pop(1,5,vr::TrackingUniverseSeated,0,event,output),"stale retirement ignored");
  queue.push(1,5,0,pose);queue.retire(1);
  check(!queue.pop(1,5,vr::TrackingUniverseSeated,0,event,output)&&queue.begin(2)&&queue.room(2),"retire clears all old events");
}
void publications() {
  CompositorPublication compositor;const auto generation=compositor.begin();
  auto sample=compositor.read();sample.sequence=1;sample.posesAvailable=true;
  sample.renderPose=sample.gamePose=invalidHeadPose(true);sample.renderTime=123;sample.gameTime=134;
  check(compositor.publish(sample),"publication initial pose");
  check(!compositor.resetOrigin(generation+1)&&compositor.read().posesAvailable,"stale reset preserves cache");
  check(compositor.resetOrigin(generation),"same-enum reset increments origin");
  const auto reset=compositor.read();
  check(!reset.posesAvailable&&reset.renderTime==0&&reset.gameTime==0&&reset.originGeneration==sample.originGeneration+1&&reset.origin==sample.origin,"reset invalidates both predictions");
  ++sample.sequence;check(!compositor.publish(sample),"old origin candidate rejected");
  sample.originGeneration=reset.originGeneration;check(compositor.publish(sample),"new origin candidate accepted");

  SystemPublication system;SystemRead meta{};meta.connected=true;const auto gen=system.begin(meta);
  GeometryInput geometry{};geometry.generation=gen;geometry.sequence=1;geometry.headFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  geometry.viewFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;geometry.headPose.orientation.w=1;
  for(unsigned i=0;i<2;++i){geometry.width[i]=geometry.height[i]=100;geometry.views[i].pose.orientation.w=1;geometry.views[i].fov={-.7f,.7f,.7f,-.7f};}
  check(system.publish(geometry,true,true),"System geometry fixture");
  system.invalidate(gen+1);check(system.read().geometryValid,"stale System invalidation ignored");
  system.invalidate(gen);check(!system.read().geometryValid&&system.read().connected,"System invalidated while connected");
  check(!system.publish(geometry,true,true),"old frame cannot repopulate invalidated cache");
  ++geometry.sequence;check(system.publish(geometry,true,true),"next frame repopulates System geometry");
}
} // namespace
int main(int argc,char** argv) {
  if(argc==2&&std::string(argv[1])=="--dry-run"){std::puts("openxr_space_test: dry-run (no runtime or writes)");return 0;}
  if(argc!=2||std::string(argv[1])!="--self-test")return 2;
  spaces();events();publications();
  std::printf("openxr_space_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
