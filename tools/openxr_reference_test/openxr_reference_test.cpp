#include "../../src/openxr/reference_changes.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using edvr::openxr::ReferenceChanges;
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* label){++checks;if(!value){++failures;std::printf("FAIL: %s\n",label);}}
const XrSession session=reinterpret_cast<XrSession>(1),foreign=reinterpret_cast<XrSession>(2);
XrEventDataReferenceSpaceChangePending event(XrTime time,XrBool32 valid=XR_FALSE) {
  XrEventDataReferenceSpaceChangePending result{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
  result.session=session;result.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
  result.changeTime=time;result.poseValid=valid;result.poseInPreviousSpace.orientation.w=1;
  return result;
}
void timeline() {
  ReferenceChanges policy;
  check(!policy.active()&&policy.epoch()==0,"initial retired");
  check(!policy.begin(XR_NULL_HANDLE),"null session");
  check(policy.begin(session)&&policy.active()&&policy.epoch()==1,"begin");
  check(!policy.begin(session),"double begin");
  for(int i=0;i<30;++i) {
    auto ignored=event(1);ignored.session=foreign;
    check(policy.note(ignored),"foreign session ignored");
    ignored.session=session;ignored.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE;
    check(policy.note(ignored),"other reference type ignored");
  }
  for(auto time:{30,10,20,20})check(policy.note(event(time)),"unsorted and equal events accepted");
  check(!policy.crosses(0,9)&&policy.crosses(0,10),"crosses inclusive upper endpoint");
  check(!policy.crosses(10,19)&&policy.crosses(10,20),"crosses exclusive lower endpoint");
  check(!policy.crosses(20,20)&&!policy.crosses(20,19),"empty and backwards interval");
  for(unsigned i=0;i<3;++i)check(policy.crosses(0,100)&&policy.epoch()==1,"crosses never advances epoch");
  uint64_t applied=99;
  check(policy.advance(9,applied)&&applied==0&&policy.epoch()==1,"before first change");
  check(policy.advance(10,applied)&&applied==1&&policy.epoch()==2,"exact first change");
  check(policy.advance(19,applied)&&applied==0&&policy.epoch()==2,"before equal pair");
  check(policy.advance(20,applied)&&applied==2&&policy.epoch()==4,"equal pair both applied");
  check(policy.advance(29,applied)&&applied==0,"before final change");
  check(policy.advance(30,applied)&&applied==1&&policy.epoch()==5,"final change");
  check(!policy.crosses(0,100),"all events consumed");
  check(policy.note(event(12))&&policy.advance(30,applied)&&applied==1&&policy.epoch()==6,"late event applied next render");
  applied=71;
  check(!policy.advance(29,applied)&&applied==71&&!policy.active()&&policy.epoch()==0,"backwards time retires without output write");
  check(!policy.note(event(31))&&!policy.advance(31,applied)&&applied==71,"retired rejects more operations");
  check(policy.begin(session)&&policy.epoch()==1&&!policy.crosses(0,100),"reinit clears history");
  policy.note(event(50));policy.clear();
  check(!policy.active()&&policy.epoch()==0&&policy.begin(session)&&!policy.crosses(0,100),"explicit clear drops pending");
}
void validation() {
  for(unsigned test=0;test<6;++test) {
    ReferenceChanges policy;policy.begin(session);auto malformed=event(10,XR_TRUE);
    if(test==0)malformed.type=XR_TYPE_EVENT_DATA_EVENTS_LOST;
    if(test==1)malformed.next=reinterpret_cast<const void*>(1);
    if(test==2)malformed.poseValid=2;
    if(test==3)malformed.poseInPreviousSpace.orientation.w=0;
    if(test==4)malformed.poseInPreviousSpace.orientation.w=NAN;
    if(test==5)malformed.poseInPreviousSpace.position.z=INFINITY;
    check(!policy.note(malformed)&&!policy.active()&&policy.epoch()==0,"malformed relevant event retires");
    uint64_t applied=42;
    check(!policy.advance(10,applied)&&applied==42,"malformed event cannot advance");
  }
  ReferenceChanges policy;policy.begin(session);auto unspecified=event(10,XR_FALSE);
  unspecified.poseInPreviousSpace.orientation={NAN,NAN,NAN,NAN};
  unspecified.poseInPreviousSpace.position={NAN,NAN,NAN};
  check(policy.note(unspecified),"unavailable pose data ignored");
  uint64_t applied=0;check(policy.advance(10,applied)&&applied==1,"unknown origin still marks discontinuity");
  check(policy.note(event(20,XR_TRUE))&&policy.advance(20,applied)&&applied==1,"valid known change accepted");
  policy.clear();policy.begin(session);
  for(unsigned i=0;i<16;++i)check(policy.note(event(100+i)),"bounded queue fill");
  check(!policy.note(event(200))&&!policy.active(),"overflow never drops silently");
  applied=44;check(!policy.advance(300,applied)&&applied==44,"overflow output unchanged");
  check(policy.begin(session)&&policy.advance(0,applied)&&applied==0&&policy.epoch()==1,"reinit after overflow");
}
} // namespace
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(std::strcmp(argv[1],"--dry-run")==0){std::puts("openxr_reference_test: dry-run (no runtime or writes)");return 0;}
  if(std::strcmp(argv[1],"--self-test")!=0)return 2;
  timeline();validation();
  std::printf("openxr_reference_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
