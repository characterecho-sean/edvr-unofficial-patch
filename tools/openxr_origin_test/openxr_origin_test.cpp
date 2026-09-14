#include "../../src/openxr/seated_origin.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

using namespace edvr::openxr;
static unsigned checks=0, failures=0;
static void check(bool x,const char* n){++checks;if(!x){++failures;std::cerr<<"FAIL: "<<n<<"\n";}}
static bool close(float a,float b){return std::fabs(a-b)<1e-5f;}
static bool same(const XrPosef&a,const XrPosef&b){return std::memcmp(&a,&b,sizeof a)==0;}
static XrSpaceLocationFlags vf(){return XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;}
static XrQuaternionf mul(XrQuaternionf a,XrQuaternionf b){return{a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
static XrQuaternionf axis(float x,float y,float z,float a){float s=static_cast<float>(std::sin(a*.5)),c=static_cast<float>(std::cos(a*.5));return{x*s,y*s,z*s,c};}
static XrPosef input(XrQuaternionf q){XrPosef p{};p.orientation=q;p.position={4,5,6};return p;}
static void expectYaw(const XrPosef& p,float yaw,const char* tag){
  const float c=static_cast<float>(std::cos(yaw)),s=static_cast<float>(std::sin(yaw));
  check(close(p.position.x,4),tag);check(close(p.position.y,5),tag);check(close(p.position.z,6),tag);
  check(close(p.orientation.x,0),tag);check(close(p.orientation.z,0),tag);
  check(close(p.orientation.y,static_cast<float>(std::sin(yaw*.5))),tag);
  check(close(p.orientation.w,static_cast<float>(std::cos(yaw*.5))),tag);
  check(close(1-2*p.orientation.y*p.orientation.y,c),tag);
  check(close(2*p.orientation.y*p.orientation.w,s),tag);
}
int main(int argc,char**argv){
  if(argc==2&&std::string(argv[1])=="--dry-run"){std::cout<<"openxr_origin_test: dry-run\n";return 0;}
  if(argc!=2||std::string(argv[1])!="--self-test")return 2;
  XrPosef out{},before{}; const float yaw=.7f;
  for(float y:{-yaw,yaw}) for(float pitch:{-.4f,.4f}) for(float roll:{-.5f,.5f}){
    auto q=mul(mul(axis(0,1,0,y),axis(1,0,0,pitch)),axis(0,0,1,roll));
    check(seatedOriginFromHead(input(q),vf(),out),"combined yaw pitch roll");
    expectYaw(out,y,"combined yaw matrix");
  }
  for(float y:{-3.14159265f/2,0.f,3.14159265f/2}){
    auto q=axis(0,1,0,y); check(seatedOriginFromHead(input(q),vf(),out),"identity/cardinal yaw"); expectYaw(out,y,"cardinal yaw matrix");
  }
  auto q=axis(0,1,0,-yaw); check(seatedOriginFromHead(input(q),vf(),out),"quaternion sign source"); before=out;
  q={-q.x,-q.y,-q.z,-q.w}; check(seatedOriginFromHead(input(q),vf(),out),"quaternion sign"); expectYaw(out,-yaw,"negative yaw");
  XrPosef bad=input({0,0,0,2}); check(!seatedOriginFromHead(bad,vf(),out)&&same(out,before),"nonunit");
  bad=input({NAN,0,0,1}); check(!seatedOriginFromHead(bad,vf(),out)&&same(out,before),"quaternion NaN");
  bad=input({0,0,0,1}); bad.position.z=std::numeric_limits<float>::infinity();
  check(!seatedOriginFromHead(bad,vf(),out)&&same(out,before),"position infinity");
  bad=input({0,0,0,0}); check(!seatedOriginFromHead(bad,vf(),out)&&same(out,before),"zero quaternion");
  for(XrSpaceLocationFlags f:{XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,XR_SPACE_LOCATION_POSITION_VALID_BIT}){
    bad=input({NAN,0,0,1}); check(!seatedOriginFromHead(bad,f,out)&&same(out,before),"missing validity with NaN");
  }
  const float pi=3.14159265358979323846f;
  const float h=static_cast<float>(std::sqrt(.5));
  for(float sign:{-1.f,1.f}){
    bad=input({sign*h,0,0,h}); before=out; check(!seatedOriginFromHead(bad,vf(),out)&&same(out,before),"vertical singularity");
    bad=input(axis(1,0,0,sign*(pi/2-1e-5f))); check(seatedOriginFromHead(bad,vf(),out),"near threshold accepted");
  }
  std::cout<<"openxr_origin_test: "<<(failures?"FAILED":"self-test passed")<<" ("<<checks<<" checks, "<<failures<<" failures)\n";
  return failures?1:0;
}
