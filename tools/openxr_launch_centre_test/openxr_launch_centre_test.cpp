#define XR_USE_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/launch_centre_policy.h"
#include <cstdio>
#include <cmath>
#include <initializer_list>

using namespace edvr::openxr;
namespace {
unsigned checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",text);}}
constexpr XrSpaceLocationFlags allTracked=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|
  XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
XrPosef pose(float x,float y,float z){XrPosef p{};p.orientation.w=1;p.position={x,y,z};return p;}
XrQuaternionf mul(const XrQuaternionf&a,const XrQuaternionf&b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
XrQuaternionf axis(float x,float y,float z,float a){const float s=std::sin(a*.5f);return {x*s,y*s,z*s,std::cos(a*.5f)};}
void acceptsTrustedPoses(){
  LaunchCentrePolicy p;
  check(p.pending(),"native centering starts pending");
  check(p.consider(pose(0,1,0),allTracked)==LaunchCentreDecision::Centre,"translated head centers");
  p=LaunchCentrePolicy{};
  check(p.consider(pose(0,100000,0),allTracked)==LaunchCentreDecision::Centre,"large finite translation is accepted");
  XrPosef yaw180=pose(0,-1,0);yaw180.orientation=axis(0,1,0,3.14159265f);
  p=LaunchCentrePolicy{};check(p.consider(yaw180,allTracked)==LaunchCentreDecision::Centre,"yaw and negative translation centers");
  XrPosef mixed=pose(1.25f,-.4f,2.0f);
  mixed.orientation=mul(mul(axis(0,1,0,.7f),axis(1,0,0,.3f)),axis(0,0,1,-.2f));
  XrPosef seated{};check(seatedOriginFromHead(mixed,allTracked,seated)&&seated.position.x==mixed.position.x&&
    seated.position.y==mixed.position.y&&seated.position.z==mixed.position.z,"yaw pitch roll keeps translation");
  double headMatrix[4][4],originMatrix[4][4],inverse[4][4],relative[4][4];
  detail::rigid(mixed,headMatrix);detail::rigid(seated,originMatrix);
  detail::inverseRigid(originMatrix,inverse);detail::product(inverse,headMatrix,relative);
  check(std::fabs(relative[0][3])<1e-6&&std::fabs(relative[1][3])<1e-6&&std::fabs(relative[2][3])<1e-6,
    "head lands at seated origin");
  check(std::fabs(relative[0][2])<1e-6&&relative[2][2]>0&&originMatrix[1][1]==1&&seated.orientation.x==0&&seated.orientation.z==0,
    "horizontal facing becomes forward and world stays upright");
  const auto residual=mul(axis(1,0,0,.3f),axis(0,0,1,-.2f));double expected[3][3];detail::rotation(residual,expected);
  bool retained=true;for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)retained&=std::fabs(relative[i][j]-expected[i][j])<1e-6;
  check(retained,"startup preserves head pitch and roll in upright world");
  p=LaunchCentrePolicy{};check(p.consider(mixed,allTracked)==LaunchCentreDecision::Centre,"yaw pitch roll centers");
}
void rejectsUntrustedPoses(){
  const auto missing={XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT,
    XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT,
    XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT,
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT};
  for(auto flags:missing){LaunchCentrePolicy p;check(p.consider(pose(0,1,0),flags)==LaunchCentreDecision::Wait,"each missing validity/tracking bit waits");}
  LaunchCentrePolicy p;XrPosef h=pose(0,1,0);h.position.x=NAN;check(p.consider(h,allTracked)==LaunchCentreDecision::Wait,"NaN position waits");
  p=LaunchCentrePolicy{};h=pose(0,1,0);h.orientation.x=NAN;check(p.consider(h,allTracked)==LaunchCentreDecision::Wait,"NaN quaternion waits");
  p=LaunchCentrePolicy{};h=pose(0,1,0);h.orientation={0,0,0,2};check(p.consider(h,allTracked)==LaunchCentreDecision::Wait,"unnormalized quaternion waits");
  for(float sign:{1.0f,-1.0f}){p=LaunchCentrePolicy{};h=pose(0,0,0);h.orientation.w=sign;check(p.consider(h,allTracked)==LaunchCentreDecision::Wait,"identity zero placeholder waits");}
  p=LaunchCentrePolicy{};h=pose(0,1,0);h.orientation=axis(1,0,0,1.5707963f);check(p.consider(h,allTracked)==LaunchCentreDecision::Wait,"near vertical forward waits");
  p=LaunchCentrePolicy{};h=pose(0,1,0);h.orientation={1,0,0,0};
  check(p.consider(h,allTracked)==LaunchCentreDecision::Centre,"upside down horizontal forward still has a heading");
}
void expiryAndLatch(){
  LaunchCentrePolicy p;const auto bad=pose(0,0,0);bool allWait=true;
  for(unsigned i=0;i<599;++i)allWait&=p.consider(bad,allTracked)==LaunchCentreDecision::Wait;
  check(allWait&&p.pending(),"invalid startup samples wait through bound");
  check(p.consider(bad,allTracked)==LaunchCentreDecision::Expired&&!p.pending(),"six hundred invalid samples expire");
  check(p.expire()==LaunchCentreDecision::Expired&&p.consider(pose(0,1,0),allTracked)==LaunchCentreDecision::Expired,
    "expiry cannot rearm or center later");
  p=LaunchCentrePolicy{};check(p.consider(pose(0,1,0),allTracked)==LaunchCentreDecision::Centre&&
    p.expire()==LaunchCentreDecision::Centre,"center cannot be changed by expiry");
}
}
int wmain(int argc,wchar_t**argv){
  if(argc==2&&!wcscmp(argv[1],L"--dry-run")){std::puts("openxr_launch_centre_test: dry-run (no runtime or files)");return 0;}
  if(argc!=2||wcscmp(argv[1],L"--self-test")){std::fputs("usage: --self-test | --dry-run\n",stderr);return 2;}
  acceptsTrustedPoses();rejectsUntrustedPoses();expiryAndLatch();
  std::printf("openxr_launch_centre_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
