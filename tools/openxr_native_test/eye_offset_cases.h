#pragma once
// F134: the runtime's eye offsets are seated vectors and the look turns the seated origin about the physical head (src/openxr/seated_eye_offset.h, which lookPoll and eyePoll in
// src/openxr/native_runtime_host.h run). The flight (edvr_gfx_20261027_080326): after pistol ADS the head pose was shifted to the right by 4 to 10 cm with the physical head within
// 3 cm. These cases drive the host's two steps as a model (the same functions), against the physical head fixed in LOCAL, and against the F62b/F33 rule as it was.
#include "../../src/openxr/seated_eye_offset.h"
#include <cmath>

namespace edvr::openxr::test {

template<class Check> void runEyeOffsetCases(Check&& check) {
  namespace eo=eyeoffset;
  const double kPi=3.14159265358979323846,kDeg=kPi/180.0;
  const double head[3]={0.30,1.60,-0.20};   // the physical head in LOCAL, held still
  const double w[3]={0.08,-0.03,0.18};      // a wanted offset in SEATED axes: |E_xz| = 0.197 m
  const double wxz=std::sqrt(w[0]*w[0]+w[2]*w[2]);
  const auto dist=[](const double a[3],const double b[3]){return std::sqrt((a[0]-b[0])*(a[0]-b[0])+(a[1]-b[1])*(a[1]-b[1])+(a[2]-b[2])*(a[2]-b[2]));};
  const SeatPose origin{0.10,0.00,0.05,23.0*kDeg};   // the seat's heading: 23 deg, as the flight's

  {   // the conventions: a seated vector is Ry(yaw) of itself in LOCAL, the seat's +x is (cos, 0, -sin), and the two directions are inverses
    const double ex[3]={1,0,0},ez[3]={0,0,1};
    double lx[3],lz[3],back[3];
    eo::localFromSeated(kPi/2,ex,lx);
    eo::localFromSeated(kPi/2,ez,lz);
    eo::seatedFromLocal(0.61,w,back);
    double again[3];eo::localFromSeated(0.61,back,again);
    check(std::fabs(lx[0])<1e-12&&std::fabs(lx[2]+1.0)<1e-12&&std::fabs(lz[0]-1.0)<1e-12&&std::fabs(lz[2])<1e-12&&dist(again,w)<1e-12,
          "F134: a seated vector is Ry(yaw) of itself in LOCAL (seat +x at 90 deg is LOCAL -z, as seatRightAxis), and the two directions are inverses");
    double tgt0[3];eo::localTarget(SeatPose{1,2,3,0.0},w,tgt0);
    check(dist(tgt0,w)<1e-12,"F134: at a seat heading of 0 the LOCAL target is the wanted vector itself (the case before F134)");
  }
  {   // (b) the frame: with a seat yaw of 23 deg (28 with the look compensation), 10 cm forward in seated axes moves the eye 10 cm along the seat's forward, not 4.6 cm to the side
    const double fwd[3]={0.0,0.0,0.10};   // +z in the origin's translation: the head moves -z, forward
    for(double yawDeg:{23.0,28.0}) {
      eo::State s;
      s.lookSeated=SeatPose{origin.x,origin.y,origin.z,yawDeg*kDeg};
      double p0[3],p1[3];
      eo::headInSeated(s.lookSeated,head,p0);
      for(int i=0;i<3;++i)s.eyeWanted[i]=fwd[i];
      s.eyeStep();
      eo::headInSeated(s.lookSeated,head,p1);
      const double d[3]={p1[0]-p0[0],p1[1]-p0[1],p1[2]-p0[2]};
      // the old rule: the same vector added to the LOCAL position unturned
      SeatPose old=SeatPose{origin.x,origin.y,origin.z,yawDeg*kDeg};
      old.x+=fwd[0];old.y+=fwd[1];old.z+=fwd[2];
      double q0[3],q1[3];eo::headInSeated(SeatPose{origin.x,origin.y,origin.z,yawDeg*kDeg},head,q0);eo::headInSeated(old,head,q1);
      const double side=q1[0]-q0[0];
      check(std::fabs(d[0])<1e-12&&std::fabs(d[1])<1e-12&&std::fabs(d[2]+0.10)<1e-12&&std::fabs(std::fabs(side)-0.10*std::sin(yawDeg*kDeg))<1e-9,
            yawDeg==23.0?"F134: with a seat yaw of 23 deg, 10 cm forward in seated axes moves the eye exactly 10 cm along the seat's forward (the old rule: 3.9 cm to the side)"
                        :"F134: with a seat yaw of 28 deg the same offset moves the eye 10.0 cm forward and none sideways (the old rule: 4.7 cm to the side, the flight's 4.6)");
      const double lx=s.lookSeated.x-origin.x,lz=s.lookSeated.z-origin.z;
      check(std::fabs(lx-0.10*std::sin(yawDeg*kDeg))<1e-12&&std::fabs(lz-0.10*std::cos(yawDeg*kDeg))<1e-12,
            "F134: the origin moved 10 cm along the seat's +z axis in LOCAL (sin yaw, cos yaw), the vector eyePoll applied");
    }
  }
  {   // (a) apply e, turn the look 30 deg in steps about the still head, remove e: the head's seated position returns exactly; the old rule left 2 sin(15 deg) |E_xz|
    eo::State on,off;   // on: with the offset; off: the same turns without it
    on.lookSeated=off.lookSeated=origin;
    for(int i=0;i<3;++i)on.eyeWanted[i]=w[i];
    on.eyeStep();
    bool settled=true,constant=true;
    for(int k=0;k<30;++k) {
      on.lookStep(1.0*kDeg,head);
      off.lookStep(1.0*kDeg,head);
      settled=settled&&on.eyeStep()==0.0;   // the fold left nothing for eyePoll to re-apply: the offset is the same seated vector after the turn
      double pOn[3],pOff[3];eo::headInSeated(on.lookSeated,head,pOn);eo::headInSeated(off.lookSeated,head,pOff);
      // (c) the rendered eye stays on the same seated point relative to the head: head(on) - head(off) = -w at every step (nothing changes in ADS)
      constant=constant&&std::fabs(pOn[0]-pOff[0]+w[0])<1e-12&&std::fabs(pOn[1]-pOff[1]+w[1])<1e-12&&std::fabs(pOn[2]-pOff[2]+w[2])<1e-12;
    }
    check(settled,"F134: after each turn of the look the eye offset needs no re-apply (the fold carries it with the seat): eyePoll finds a delta of 0");
    check(constant,"F134: during the 30 deg turn with the offset applied the head sits at the same seated point relative to what it would be without it (-w, to a picometre): nothing visible changes");
    on.eyeWanted[0]=on.eyeWanted[1]=on.eyeWanted[2]=0.0;
    on.eyeStep();
    double pOn[3],pOff[3];eo::headInSeated(on.lookSeated,head,pOn);eo::headInSeated(off.lookSeated,head,pOff);
    const double left=dist(pOn,pOff);
    check(left<1e-9&&std::fabs(on.lookSeated.x-off.lookSeated.x)<1e-9&&std::fabs(on.lookSeated.z-off.lookSeated.z)<1e-9&&std::fabs(on.lookSeated.yaw-off.lookSeated.yaw)<1e-12,
          "F134: the offset removed after a 30 deg turn, the head's seated position (and the origin) is the one it would have had without the offset: leftover under a nanometre");

    // the rule as it was (F62b's pivot at the rendered eye, the offset added to the LOCAL position unturned) on the same turns
    SeatPose o=origin;double e[3]={0,0,0};
    const auto oldEye=[&](const double want[3]){for(int i=0;i<3;++i){const double d=want[i]-e[i];if(i==0)o.x+=d;else if(i==1)o.y+=d;else o.z+=d;e[i]=want[i];}};
    oldEye(w);
    for(int k=0;k<30;++k) {
      const double hx=head[0]-e[0],hz=head[2]-e[2];
      const double dx=o.x-hx,dz=o.z-hz,c=std::cos(1.0*kDeg),sn=std::sin(1.0*kDeg);
      o=SeatPose{hx+c*dx+sn*dz,o.y,hz-sn*dx+c*dz,o.yaw+1.0*kDeg};
      eo::foldLocal(e,1.0*kDeg);
      oldEye(w);   // the old eyePoll re-adds the wanted offset to the LOCAL position
    }
    const double zero[3]={0,0,0};
    oldEye(zero);
    double pOld[3];eo::headInSeated(o,head,pOld);
    const double oldLeft=dist(pOld,pOff);
    const double predicted=2.0*std::sin(15.0*kDeg)*wxz;
    check(oldLeft>0.8*predicted&&oldLeft<1.2*predicted&&oldLeft>0.05,"F134 (the rule as it was): the same turns leave the head 2 sin(15 deg) |E_xz| away from where it belongs (about 10 cm for a 20 cm offset)");
  }
  {   // the pivot, directly: turning the origin about the physical head keeps the head's seated position through any turn, with or without a folded offset
    SeatPose s=origin;
    double p0[3],p1[3];
    eo::headInSeated(s,head,p0);
    const SeatPose t=eo::turnAboutHead(s,head[0],head[2],0.37);
    eo::headInSeated(t,head,p1);
    // the head's seated position turns with the seat (it is the same LOCAL point in a space turned by 0.37): its distance from the origin is kept
    const double r0=std::sqrt(p0[0]*p0[0]+p0[2]*p0[2]),r1=std::sqrt(p1[0]*p1[0]+p1[2]*p1[2]);
    check(std::fabs(r0-r1)<1e-12&&std::fabs(p0[1]-p1[1])<1e-12&&std::fabs(t.yaw-s.yaw-0.37)<1e-12&&std::fabs(eo::turnAboutHead(s,head[0],head[2],0.0).x-s.x)<1e-15,
          "F134: lookPoll's turn about the head keeps the head's distance from the origin and its height, adds the turn to the heading, and zero is the identity");
    double e[3]={0.05,0.02,-0.07};
    eo::foldLocal(e,kPi/2);
    check(std::fabs(e[0]+0.07)<1e-12&&std::fabs(e[1]-0.02)<1e-12&&std::fabs(e[2]+0.05)<1e-12,"F134: the fold turns the applied offset the way the origin turns (x,z -> z,-x at 90 deg) and keeps its height");
  }
}

}  // namespace edvr::openxr::test
