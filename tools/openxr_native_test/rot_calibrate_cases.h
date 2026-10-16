#pragma once
// F61 (TEMPORARY, the rotation calibration, fix.native_on_foot_rot_calibrate): the runtime's pitch overlay (src/openxr/native_runtime_host.h
// seatPosePitched). A seated space turned by +a about the head's right axis through the head leaves the head where it was and turns its
// reported pitch by -a with its yaw unchanged; zero is the pose itself; +a then -a about the same pivot is the pose again.
#include "../../src/openxr/native_runtime_host.h"
#include <cmath>
#include <cstring>

namespace edvr::openxr::test {
namespace rot_calibrate_fixture {
inline void qmul(const double a[4],const double b[4],double o[4]) {   // Hamilton product, (x, y, z, w)
  o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
  o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
  o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
  o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
// The head (position h, orientation hq, LOCAL) as the space S sees it: S^-1 * H.
inline void inSpace(const XrPosef& s,const double h[3],const double hq[4],double pos[3],double q[4]) {
  const double si[4]={-s.orientation.x,-s.orientation.y,-s.orientation.z,s.orientation.w};
  const double d[3]={h[0]-s.position.x,h[1]-s.position.y,h[2]-s.position.z};
  seatQuatApply(si,d,pos);
  qmul(si,hq,q);
}
inline double pitchDeg(const double q[4]) {const double back[3]={0,0,-1};double f[3];seatQuatApply(q,back,f);return std::asin(f[1])*180.0/3.14159265358979323846;}
inline double yawDeg(const double q[4]) {return std::atan2(2.0*(q[0]*q[2]+q[3]*q[1]),1.0-2.0*(q[0]*q[0]+q[1]*q[1]))*180.0/3.14159265358979323846;}
}  // namespace rot_calibrate_fixture

template<class Check> void runRotCalibrateCases(Check&& check) {
  namespace f=rot_calibrate_fixture;
  const double kPi=3.14159265358979323846;
  {
    const double q[4]={std::sin(kPi/4),0,0,std::cos(kPi/4)},v[3]={0,0,-1};double o[3];
    seatQuatApply(q,v,o);
    check(std::fabs(o[0])<1e-9&&std::fabs(o[1]-1.0)<1e-9&&std::fabs(o[2])<1e-9,"F61: +90 deg about +x turns -z (forward) to +y (up)");
  }
  // The head at (0.1, 1.6, -0.2), yawed 30 deg (the seat's sign), pitched 10 deg up; the seated space a seat at yaw 20 deg, 0.3 m off.
  const double yaw=30.0*kPi/180.0,pitch=10.0*kPi/180.0;
  const double qy[4]={0,std::sin(yaw/2),0,std::cos(yaw/2)},qx[4]={std::sin(pitch/2),0,0,std::cos(pitch/2)};
  double hq[4];f::qmul(qy,qx,hq);
  const double h[3]={0.1,1.6,-0.2};
  const SeatPose seat{0.3,0.0,-0.1,20.0*kPi/180.0};
  const XrPosef base=xrFromSeatPose(seat);
  double axis[3];{const double right[3]={1,0,0};seatQuatApply(hq,right,axis);}
  const XrPosef zero=seatPosePitched(base,h,axis,0.0);
  check(std::memcmp(&zero,&base,sizeof(base))==0,"F61: a zero pitch is the pose itself (the key off changes nothing)");
  double p0[3],q0[4],p1[3],q1[4];
  f::inSpace(base,h,hq,p0,q0);
  const XrPosef turned=seatPosePitched(base,h,axis,5.0*kPi/180.0);
  f::inSpace(turned,h,hq,p1,q1);
  const double moved=std::sqrt((p1[0]-p0[0])*(p1[0]-p0[0])+(p1[1]-p0[1])*(p1[1]-p0[1])+(p1[2]-p0[2])*(p1[2]-p0[2]));
  check(moved<1e-5,"F61: the pitch about the head's right axis through the head leaves the head where it was in the seated space");
  check(std::fabs((f::pitchDeg(q1)-f::pitchDeg(q0))+5.0)<1e-3&&std::fabs(f::yawDeg(q1)-f::yawDeg(q0))<1e-3,
        "F61: a seated pitch of +5 deg turns the reported head's pitch by -5 deg and leaves its yaw (the seat's yaw sign: 30 - 20 = 10 deg)");
  check(std::fabs(f::yawDeg(q0)-10.0)<1e-3&&std::fabs(f::pitchDeg(q0)-10.0)<1e-3,"F61: the fixture's head reads yaw 10 deg and pitch 10 deg in the seated space");
  const XrPosef back=seatPosePitched(turned,h,axis,-5.0*kPi/180.0);
  const bool same=std::fabs(back.position.x-base.position.x)<1e-5f&&std::fabs(back.position.y-base.position.y)<1e-5f&&
                  std::fabs(back.position.z-base.position.z)<1e-5f&&std::fabs(back.orientation.x-base.orientation.x)<1e-6f&&
                  std::fabs(back.orientation.y-base.orientation.y)<1e-6f&&std::fabs(back.orientation.z-base.orientation.z)<1e-6f&&
                  std::fabs(back.orientation.w-base.orientation.w)<1e-6f;
  check(same,"F61: +5 deg then -5 deg about the same pivot and axis is the seated pose again (the return is exact)");
  // F62b (TEMPORARY): the ADS head-aim's pitch overlay turns about the SEAT's right axis (seatRightAxis of the seat's yaw) through the head.
  // After a seat yaw of 90 deg (the head turned with it) it is still a pitch of the reported head, never a roll; the head keeps its place.
  {
    double ax0[3];seatRightAxis(0.0,ax0);
    const double q90[4]={0,std::sin(kPi/4),0,std::cos(kPi/4)},x[3]={1,0,0};double r90[3];seatQuatApply(q90,x,r90);
    double ax90[3];seatRightAxis(kPi/2,ax90);
    check(std::fabs(ax0[0]-1.0)<1e-12&&std::fabs(ax0[2])<1e-12&&std::fabs(ax90[0]-r90[0])<1e-9&&std::fabs(ax90[2]-r90[2])<1e-9&&std::fabs(ax90[1])<1e-12,
          "F62b: the seat's right axis is +x at yaw 0 and +x turned by the seat's yaw (xrFromSeatPose's turn) at 90 deg");
    const SeatPose seat90{0.3,0.0,-0.1,kPi/2};
    const XrPosef b90=xrFromSeatPose(seat90);
    const double hy[4]={0,std::sin(kPi/4),0,std::cos(kPi/4)};   // the head yawed 90 deg in LOCAL: straight ahead in the seat
    double pa[3],qa[4],pb[3],qb[4];
    f::inSpace(b90,h,hy,pa,qa);
    const XrPosef pitched=seatPosePitched(b90,h,ax90,8.0*kPi/180.0);
    f::inSpace(pitched,h,hy,pb,qb);
    const double mv=std::sqrt((pb[0]-pa[0])*(pb[0]-pa[0])+(pb[1]-pa[1])*(pb[1]-pa[1])+(pb[2]-pa[2])*(pb[2]-pa[2]));
    const double right[3]={1,0,0};double ra[3],rb[3];
    seatQuatApply(qa,right,ra);seatQuatApply(qb,right,rb);
    const double rollA=std::asin(ra[1])*180.0/kPi,rollB=std::asin(rb[1])*180.0/kPi;   // the right axis's rise: roll
    check(mv<1e-5&&std::fabs((f::pitchDeg(qb)-f::pitchDeg(qa))+8.0)<1e-3&&std::fabs(rollB-rollA)<1e-3&&std::fabs(f::yawDeg(qb)-f::yawDeg(qa))<1e-3,
          "F62b: after a seat yaw of 90 deg a seated pitch of +8 deg about the seat's right axis turns the reported head's pitch by -8 deg (no roll), "
          "the head keeping its place");
    const XrPosef zeroAds=seatPosePitched(b90,h,ax90,0.0);
    check(std::memcmp(&zeroAds,&b90,sizeof(b90))==0,"F62b: a zero ADS pitch is the pose itself (the feature off changes nothing)");
  }
}
}  // namespace edvr::openxr::test
