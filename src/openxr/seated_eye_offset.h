#pragma once
// F134: the runtime's eye offsets (the provider's body-align / ADS sight lock / F88 weapon eye offset, "eyeWanted") are SEATED vectors, and the look turns the seated origin
// about the physical head. Header only, no OpenXR: the host (native_runtime_host.h lookPoll and eyePoll) and the rig (tools\openxr_native_test\eye_offset_cases.h) run these
// very functions. Journal: the arc doc, 2026-10-27, F134.
//
// The flight (edvr_gfx_20261027_080326; runtime log edvr_openxr_20261027_080327_777_2588): after pistol ADS the pilot's head pose was "shifted unnaturally to the right" while
// his physical head stayed within 3 cm. Two defects:
//  1. The pivot. lookPoll turned the seated origin O about P = head - eyeApplied (the rendered eye) and folded the applied offset e by the turn. With e applied the origin
//     turned about the physical head, but the offset-free origin U = O - e turned about h - e: U' = rot(U) - (I - R) e. That is hidden while e is applied and left behind
//     when e is removed: |leftover| = 2 sin(|dpsi| / 2) |e_xz| for the look's net yaw dpsi between the sight lock going on and off (press 1: 13.3 deg, 18 cm -> 4.3 cm; press
//     3: 25 deg, 24 cm -> 10.4 cm). The origin now turns about the physical head and e is folded by the same turn, so U turns about the head exactly.
//  2. The frame. eyePoll added eyeWanted, a vector in SEATED axes (the provider builds it from body axes with seatedFromBody(.., 0.0), and F88's (-right, -up, +forward)), to
//     lookSeated, a LOCAL position, without turning it: an offset landed rotated by the seat's yaw (23 deg in that flight, plus the look compensation): a weapon eye 10 cm
//     forward became 4.6 cm to the side at 28 deg. The LOCAL translation of the origin is now Ry(seat yaw) . eyeWanted.
// Conventions (the host's): a SeatPose {x, y, z, yaw} is the seated space in LOCAL, xrFromSeatPose turns +x about +y by yaw, so a seated vector v is the LOCAL vector
// Ry(yaw) v with Ry the host's ryApply [c 0 s; 0 1 0; -s 0 c] (+x -> (cos, 0, -sin)), the same matrix that turns the origin's position about the head. The head's heading
// in LOCAL is its heading in the seated space plus the seat's yaw.
#include "seat_memory.h"

#include <cmath>

namespace edvr::openxr::eyeoffset {

// v (seated axes) -> LOCAL, for a seated space of heading `yaw`.
inline void localFromSeated(double yaw, const double v[3], double out[3]) {
  const double c=std::cos(yaw),s=std::sin(yaw);
  const double x=c*v[0]+s*v[2],z=-s*v[0]+c*v[2],y=v[1];
  out[0]=x;out[1]=y;out[2]=z;
}
// v (LOCAL) -> seated axes, for a seated space of heading `yaw`.
inline void seatedFromLocal(double yaw,const double v[3],double out[3]) {localFromSeated(-yaw,v,out);}

// eyePoll's target: the LOCAL translation the origin should carry for the wanted seated offset, at the seat's current heading.
inline void localTarget(const SeatPose& seat,const double wantedSeated[3],double out[3]) {localFromSeated(seat.yaw,wantedSeated,out);}

// lookPoll's turn: the origin turned by `delta` about the vertical through the physical head (hx, hz in LOCAL). The head keeps its place in the seated space, and the turn
// is the same rotation that carries the seated axes, so a seated offset stays the same seated offset (the caller folds the applied LOCAL offset with foldLocal).
inline SeatPose turnAboutHead(const SeatPose& seat,double hx,double hz,double delta) {
  const double dx=seat.x-hx,dz=seat.z-hz;
  const double c=std::cos(delta),s=std::sin(delta);
  return SeatPose{hx+c*dx+s*dz,seat.y,hz-s*dx+c*dz,seat.yaw+delta};
}
// The applied LOCAL offset turned with the seat (the host's fold): the vertical is kept.
inline void foldLocal(double e[3],double delta) {
  const double ce=std::cos(delta),se=std::sin(delta);
  const double ex=e[0],ez=e[2];
  e[0]=ce*ex+se*ez;e[2]=-se*ex+ce*ez;
}

// The head (LOCAL position h) in the seated space of `seat`: Ry(-yaw) (h - O).
inline void headInSeated(const SeatPose& seat,const double h[3],double out[3]) {
  const double v[3]={h[0]-seat.x,h[1]-seat.y,h[2]-seat.z};
  seatedFromLocal(seat.yaw,v,out);
}

// The runtime's state for these two steps, for the rig (the host's members of the same names).
struct State {
  SeatPose lookSeated;
  double eyeWanted[3]={0,0,0},eyeApplied[3]={0,0,0};
  // eyePoll: apply the change in the wanted offset. Returns the largest component of the delta it applied (0: nothing to do).
  double eyeStep() {
    double target[3];localTarget(lookSeated,eyeWanted,target);
    double delta[3]={target[0]-eyeApplied[0],target[1]-eyeApplied[1],target[2]-eyeApplied[2]};
    double biggest=std::fmax(std::fabs(delta[0]),std::fmax(std::fabs(delta[1]),std::fabs(delta[2])));
    if(biggest<=1e-6) return 0.0;
    lookSeated.x+=delta[0];lookSeated.y+=delta[1];lookSeated.z+=delta[2];
    for(int i=0;i<3;++i)eyeApplied[i]+=delta[i];
    return biggest;
  }
  // lookPoll: turn the origin by delta about the physical head h and fold the applied offset.
  void lookStep(double delta,const double h[3]) {
    lookSeated=turnAboutHead(lookSeated,h[0],h[2],delta);
    foldLocal(eyeApplied,delta);
  }
};

}  // namespace edvr::openxr::eyeoffset
