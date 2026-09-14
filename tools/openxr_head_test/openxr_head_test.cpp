#include "../../src/openxr/head_locator.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace edvr::openxr;
namespace {
unsigned checks=0,failures=0;
void check(bool v,const char* why){++checks;if(!v){++failures;std::printf("FAIL: %s\n",why);}}
const auto instance=reinterpret_cast<XrInstance>(1);
const auto view=reinterpret_cast<XrSpace>(2),origin=reinterpret_cast<XrSpace>(3);
struct Fake {
  XrTime time=7000000000,located=0;
  XrResult convertResult=XR_SUCCESS,locateResult=XR_SUCCESS;
  bool counter=true;unsigned variant=0;std::vector<unsigned> trace;
} fake;
bool now(LARGE_INTEGER* q){fake.trace.push_back(1);q->QuadPart=123456789;return fake.counter;}
XrResult XRAPI_PTR convert(XrInstance i,const LARGE_INTEGER* q,XrTime* t){
  check(i==instance&&q->QuadPart==123456789,"typed clock conversion");fake.trace.push_back(2);*t=fake.time;return fake.convertResult;
}
XrResult XRAPI_PTR locate(XrSpace from,XrSpace base,XrTime t,XrSpaceLocation* out){
  check(from==view&&base==origin&&out->type==XR_TYPE_SPACE_LOCATION&&!out->next,"typed exact-origin locate");
  fake.trace.push_back(3);fake.located=t;
  out->locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  out->pose={{0,0,0,1},{1,2,3}};
  switch(fake.variant){
    case 1:out->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;out->pose.position.x=NAN;break;
    case 2:out->pose.position.x=INFINITY;break;case 3:out->pose.orientation.w=2;break;
    case 4:out->type=XR_TYPE_UNKNOWN;break;case 5:out->next=reinterpret_cast<void*>(1);break;
  }
  return fake.locateResult;
}
int selfTest(){
  HeadLocator h;LocatorDispatch api{convert,locate};XrSpaceLocation out{XR_TYPE_SPACE_LOCATION};XrTime time=0;
  auto attempt=[&](const LocatorDispatch& d,XrInstance i,XrSpace v,XrSpace o,float p,CounterNow clock){return h.locate(d,i,v,o,p,out,&time,clock);};
  for(float prediction:{0.f,.5f,-.5f,.000001f}){
    fake={};check(attempt(api,instance,view,origin,prediction,now)==XR_SUCCESS,"valid prediction");
    const XrTime offset=prediction==.5f?500000000:prediction==-.5f?-500000000:prediction==0?0:1000;
    check(time==7000000000+offset&&time==fake.located,"now plus independent expected prediction");
    check(fake.trace==std::vector<unsigned>({1,2,3})&&out.pose.position.y==2,"call order/native pose");
  }
  const auto canary=out;const auto timeCanary=time;
  for(unsigned test=0;test<15;++test){
    fake={};XrResult r=XR_SUCCESS;
    switch(test){
      case 0:r=attempt({nullptr,locate},instance,view,origin,0,now);break;
      case 1:r=attempt(api,XR_NULL_HANDLE,view,origin,0,now);break;
      case 2:r=attempt(api,instance,XR_NULL_HANDLE,origin,0,now);break;
      case 3:r=attempt(api,instance,view,XR_NULL_HANDLE,0,now);break;
      case 4:r=attempt(api,instance,view,origin,NAN,now);break;
      case 5:r=attempt(api,instance,view,origin,INFINITY,now);break;
      case 6:r=attempt(api,instance,view,origin,(std::numeric_limits<float>::max)(),now);break;
      case 7:r=attempt(api,instance,view,origin,-(std::numeric_limits<float>::max)(),now);break;
      case 8:r=attempt(api,instance,view,origin,0,nullptr);break;
      case 9:fake.counter=false;r=attempt(api,instance,view,origin,0,now);break;
      case 10:fake.time=(std::numeric_limits<XrTime>::max)();r=attempt(api,instance,view,origin,.5f,now);break;
      case 11:fake.time=(std::numeric_limits<XrTime>::min)();r=attempt(api,instance,view,origin,-.5f,now);break;
      case 12:r=attempt({convert,nullptr},instance,view,origin,0,now);break;
      case 13:r=attempt(api,instance,view,origin,std::ldexp(1.f,34),now);break;
      case 14:r=attempt(api,instance,view,origin,-std::ldexp(1.f,34),now);break;
    }
    check(XR_FAILED(r),"bad inputs/overflow rejected");
    check(std::memcmp(&out,&canary,sizeof(out))==0&&time==timeCanary,"bad input output/time canary");
    check(fake.trace.empty()||fake.trace.back()!=3,"invalid input never locates");
  }
  for(bool atLocate:{false,true})for(XrResult r:{XR_ERROR_INSTANCE_LOST,XR_ERROR_TIME_INVALID,XR_SESSION_LOSS_PENDING,XR_TIMEOUT_EXPIRED}){
    fake={};if(atLocate)fake.locateResult=r;else fake.convertResult=r;
    check(attempt(api,instance,view,origin,.5f,now)==r,"exact positive/negative result retained");
    check(std::memcmp(&out,&canary,sizeof(out))==0&&time==timeCanary,"runtime failure output/time canary");
    check(fake.trace.back()==(atLocate?3u:2u),"failure stops dispatch");
  }
  for(unsigned variant=2;variant<=5;++variant){
    fake={};fake.variant=variant;check(XR_FAILED(attempt(api,instance,view,origin,0,now)),"malformed valid pose/struct rejected");
    check(std::memcmp(&out,&canary,sizeof(out))==0&&time==timeCanary,"malformed output canary");
  }
  fake={};fake.variant=1;check(attempt(api,instance,view,origin,0,now)==XR_SUCCESS,"partial tracking not runtime error");
  check(out.locationFlags==0&&out.pose.orientation.w==1&&out.pose.position.x==0,"partial tracking initialized invalid");
  fake={};fake.time=(std::numeric_limits<XrTime>::max)()-500000000;check(attempt(api,instance,view,origin,.5f,now)==XR_SUCCESS&&time==(std::numeric_limits<XrTime>::max)(),"exact upper addition boundary");
  fake={};fake.time=(std::numeric_limits<XrTime>::min)()+500000000;check(attempt(api,instance,view,origin,-.5f,now)==XR_SUCCESS&&time==(std::numeric_limits<XrTime>::min)(),"exact lower addition boundary");
  std::printf("openxr_head_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
}
int main(int argc,char** argv){
  if(argc!=2)return 2;
  if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would test exact-time HMD location; no runtime/device/files.");return 0;}
  return !std::strcmp(argv[1],"--self-test")?selfTest():2;
}
