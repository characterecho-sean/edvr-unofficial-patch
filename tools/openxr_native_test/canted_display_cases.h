#pragma once
// The canted-display arc's two temporary test instruments (docs\canted-projection.md): the handedness correction an eye-to-head answer carries
// (advanced.canted_eye_fix) and the simulated outward cant given to the located stereo pair (advanced.simulate_cant). The math is checked against formulas
// written out independently here; the host's own wiring (previous frame's value, change notes, the epoch) against a fixture host.
#include "launch_centre_cases.h"
#include "../../src/openxr/canted_display.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace edvr::openxr::test {
namespace canted_fixture {
constexpr double kPi=3.14159265358979323846;
inline double rad(double degrees){return degrees*kPi/180.0;}
inline bool bitwiseEqual(const vr::HmdMatrix34_t& a,const vr::HmdMatrix34_t& b){return std::memcmp(&a,&b,sizeof(a))==0;}
inline bool bitwiseEqual(const XrView& a,const XrView& b){
  return std::memcmp(&a.pose,&b.pose,sizeof(a.pose))==0&&std::memcmp(&a.fov,&b.fov,sizeof(a.fov))==0;
}
// A rigid eye matrix: R = Ry(yaw) * Rx(pitch), the given translation.
inline vr::HmdMatrix34_t eyeMatrix(double yawDeg,double pitchDeg,float tx,float ty,float tz){
  const double a=rad(yawDeg),p=rad(pitchDeg);
  const double ry[3][3]={{std::cos(a),0,std::sin(a)},{0,1,0},{-std::sin(a),0,std::cos(a)}};
  const double rx[3][3]={{1,0,0},{0,std::cos(p),-std::sin(p)},{0,std::sin(p),std::cos(p)}};
  vr::HmdMatrix34_t m{};
  for(int i=0;i<3;++i)for(int j=0;j<3;++j){double v=0;for(int k=0;k<3;++k)v+=ry[i][k]*rx[k][j];m.m[i][j]=float(v);}
  m.m[0][3]=tx;m.m[1][3]=ty;m.m[2][3]=tz;
  return m;
}
// S*E*S by 4x4 multiplication, S = diag(1,1,-1,1): the definition, with none of the sign bookkeeping in the product.
inline vr::HmdMatrix34_t sandwich(const vr::HmdMatrix34_t& e){
  const double s[4][4]={{1,0,0,0},{0,1,0,0},{0,0,-1,0},{0,0,0,1}};
  double a[4][4]={},t[4][4]={},o[4][4]={};
  for(int i=0;i<3;++i)for(int j=0;j<4;++j)a[i][j]=e.m[i][j];a[3][3]=1;
  for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)t[i][j]+=s[i][k]*a[k][j];
  for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)o[i][j]+=t[i][k]*s[k][j];
  vr::HmdMatrix34_t m{};for(int i=0;i<3;++i)for(int j=0;j<4;++j)m.m[i][j]=float(o[i][j]);return m;
}
inline XrQuaternionf quaternion(double yawDeg,double pitchDeg){
  // q = Ry(yaw) * Rx(pitch)
  const double a=rad(yawDeg)*.5,p=rad(pitchDeg)*.5;
  const double qy[4]={0,std::sin(a),0,std::cos(a)},qx[4]={std::sin(p),0,0,std::cos(p)};
  return {float(qy[3]*qx[0]+qy[0]*qx[3]+qy[1]*qx[2]-qy[2]*qx[1]),float(qy[3]*qx[1]-qy[0]*qx[2]+qy[1]*qx[3]+qy[2]*qx[0]),
          float(qy[3]*qx[2]+qy[0]*qx[1]-qy[1]*qx[0]+qy[2]*qx[3]),float(qy[3]*qx[3]-qy[0]*qx[0]-qy[1]*qx[1]-qy[2]*qx[2])};
}
inline XrFovf fovDegrees(double l,double r,double u,double d){return {float(rad(l)),float(rad(r)),float(rad(u)),float(rad(d))};}
inline double tanOf(float angle){return std::tan(double(angle));}
// Where the eye matrix puts a ray: R^T d (a direction in the old frame, expressed in the new), from the output pose alone.
inline void intoFrame(const XrQuaternionf& oldQ,const XrQuaternionf& newQ,const double d[3],double out[3]){
  double ro[3][3],rn[3][3];detail::rotation(oldQ,ro);detail::rotation(newQ,rn);
  double world[3]={};for(int i=0;i<3;++i)for(int k=0;k<3;++k)world[i]+=ro[i][k]*d[k];
  for(int i=0;i<3;++i){out[i]=0;for(int k=0;k<3;++k)out[i]+=rn[k][i]*world[k];}
}
}
template<class Check> void runCantedDisplayCases(Check&& check) {
  using namespace canted_fixture;
  const double tolerance=2e-6;
  // ---- (a) the handedness correction ----------------------------------------------------------------------------------------------------------------------
  {
    const auto pure=eyeMatrix(0,0,0.032f,0.0f,0.0f);
    check(bitwiseEqual(gameHandedness(pure),pure),"the correction is the identity on a pure x translation (no rotation, tz 0), bit for bit, and a zero never turns -0");
    const auto lifted=eyeMatrix(0,0,-0.032f,0.004f,0.0f);
    check(bitwiseEqual(gameHandedness(lifted),lifted),"...and on a translation with a height offset");
    const auto yawed=eyeMatrix(10,4,-0.032f,0.004f,-0.011f);
    const auto g=gameHandedness(yawed);
    bool flipped=true,kept=true;
    for(int i=0;i<3;++i)for(int j=0;j<4;++j){
      const bool expectFlip=(i==0&&j==2)||(i==1&&j==2)||(i==2&&j==0)||(i==2&&j==1)||(i==2&&j==3);   // R02, R12, R20, R21 and tz, by name
      if(expectFlip)flipped=flipped&&g.m[i][j]==0.0f-yawed.m[i][j]&&yawed.m[i][j]!=0.0f;
      else kept=kept&&g.m[i][j]==yawed.m[i][j];
    }
    check(flipped,"a yawed and pitched eye: R02, R12, R20, R21 and tz are negated (each of them non-zero in the input, so the test can tell)");
    check(kept,"...and the other seven entries (R00, R01, R10, R11, R22, tx, ty) are untouched");
    const auto reference=sandwich(yawed);
    double worst=0;for(int i=0;i<3;++i)for(int j=0;j<4;++j)worst=(std::max)(worst,std::fabs(double(g.m[i][j])-reference.m[i][j]));
    check(worst<1e-7,"...and that is S*E*S taken as a 4x4 product with S = diag(1,1,-1,1)");
    check(bitwiseEqual(gameHandedness(g),yawed),"the correction is its own inverse");
    // Its yaw sign: a left eye turned outward (yaw -10 degrees, forward toward -x) reads +10 once corrected: the rotation Elite composes is the mirror of the located one.
    check(std::fabs(forwardYawDegrees(eyeMatrix(10,0,-0.032f,0,0))+10.0)<1e-4&&std::fabs(forwardYawDegrees(gameHandedness(eyeMatrix(10,0,-0.032f,0,0)))-10.0)<1e-4&&
          std::fabs(forwardYawDegrees(eyeMatrix(0,0,0.032f,0,0)))<1e-6,
          "forwardYawDegrees: Ry(10) turns an eye's forward toward -x (-10), the corrected matrix reads +10, no rotation reads 0");
  }
  // ---- (b) simulate_cant 0 is the identity ----------------------------------------------------------------------------------------------------------------
  XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
  views[0].pose.orientation=quaternion(12,-7);views[0].pose.position={-0.032f,1.62f,-0.05f};views[0].fov=fovDegrees(-50,40,45,-48);
  views[1].pose.orientation=quaternion(11,-6);views[1].pose.position={0.032f,1.62f,-0.05f};views[1].fov=fovDegrees(-41,52,44,-47);
  {
    XrView same[2]={views[0],views[1]};CantReport report{};report.toldFov[0].angleLeft=123.0f;
    check(simulateCant(same,0.0f,&report)==CantOutcome::Off&&bitwiseEqual(same[0],views[0])&&bitwiseEqual(same[1],views[1])&&report.toldFov[0].angleLeft==123.0f,
          "simulate_cant 0 is off: pose and fov bit-identical, the report untouched");
    for(const float notAnAngle:{-3.0f,-0.0f,std::numeric_limits<float>::quiet_NaN(),-std::numeric_limits<float>::infinity()}) {
      XrView again[2]={views[0],views[1]};
      check(simulateCant(again,notAnAngle,nullptr)==CantOutcome::Off&&bitwiseEqual(again[0],views[0])&&bitwiseEqual(again[1],views[1]),
            "...so is a negative, a negative zero, a NaN and minus infinity");
    }
    check(sanitizeSimulatedCant(40.0f)==15.0f&&sanitizeSimulatedCant(15.0f)==15.0f&&sanitizeSimulatedCant(7.5f)==7.5f&&sanitizeSimulatedCant(0.0f)==0.0f&&
          sanitizeSimulatedCant(std::numeric_limits<float>::infinity())==0.0f&&sanitizeSimulatedCant(std::numeric_limits<float>::quiet_NaN())==0.0f,
          "the cant is clamped to 0..15 degrees, a NaN or an infinity is 0 (the provider side reads both as off, too)");
    XrView over[2]={views[0],views[1]},top[2]={views[0],views[1]};
    check(simulateCant(over,40.0f,nullptr)==CantOutcome::Applied&&simulateCant(top,15.0f,nullptr)==CantOutcome::Applied&&
          bitwiseEqual(over[0],top[0])&&bitwiseEqual(over[1],top[1]),"...and 40 degrees gives exactly what 15 gives");
  }
  // ---- (c) a 10 degree outward cant ---------------------------------------------------------------------------------------------------------------------------
  {
    const double theta=10.0;
    XrView canted[2]={views[0],views[1]};CantReport report{};
    check(simulateCant(canted,float(theta),&report)==CantOutcome::Applied,"10 degrees on a plausible pair is applied");
    check(std::memcmp(&canted[0].pose.position,&views[0].pose.position,sizeof(views[0].pose.position))==0&&
          std::memcmp(&canted[1].pose.position,&views[1].pose.position,sizeof(views[1].pose.position))==0&&
          canted[0].type==XR_TYPE_VIEW&&canted[1].type==XR_TYPE_VIEW&&canted[0].next==nullptr&&canted[1].next==nullptr,
          "...the eye positions are untouched");
    check(std::memcmp(&report.trueFov[0],&views[0].fov,sizeof(report.trueFov[0]))==0&&std::memcmp(&report.toldFov[1],&canted[1].fov,sizeof(report.toldFov[1]))==0,
          "...and the report names the located fov and the told one");
    // The turn each eye took, from the old and new orientations alone: Ry(+10) for the left eye, Ry(-10) for the right.
    for(unsigned eye=0;eye<2;++eye) {
      double ro[3][3],rn[3][3];detail::rotation(views[eye].pose.orientation,ro);detail::rotation(canted[eye].pose.orientation,rn);
      double rel[3][3]={};for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)rel[i][j]+=ro[k][i]*rn[k][j];   // R_old^T * R_new
      const double a=rad(eye==0?theta:-theta);
      const double expected[3][3]={{std::cos(a),0,std::sin(a)},{0,1,0},{-std::sin(a),0,std::cos(a)}};
      double worst=0;for(int i=0;i<3;++i)for(int j=0;j<3;++j)worst=(std::max)(worst,std::fabs(rel[i][j]-expected[i][j]));
      check(worst<1e-6,eye==0?"the left eye turned by Ry(+10) about its own y, whatever its base orientation":"the right eye turned by Ry(-10) about its own y, whatever its base orientation");
      const double q=canted[eye].pose.orientation.x*double(canted[eye].pose.orientation.x)+double(canted[eye].pose.orientation.y)*canted[eye].pose.orientation.y+
        double(canted[eye].pose.orientation.z)*canted[eye].pose.orientation.z+double(canted[eye].pose.orientation.w)*canted[eye].pose.orientation.w;
      check(std::fabs(q-1.0)<1e-6,"...to a unit quaternion");
    }
    // With the eyes level and facing forward: the left eye's forward leans to -x, the right's to +x (outward), by sin(10).
    {
      XrView level[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
      for(auto& v:level){v.pose.orientation={0,0,0,1};v.fov=fovDegrees(-45,45,45,-45);}
      level[0].pose.position={-0.032f,0,0};level[1].pose.position={0.032f,0,0};
      CantReport r{};check(simulateCant(level,10.0f,&r)==CantOutcome::Applied,"(level eyes) applied");
      double rl[3][3],rr[3][3];detail::rotation(level[0].pose.orientation,rl);detail::rotation(level[1].pose.orientation,rr);
      const double fl[3]={-rl[0][2],-rl[1][2],-rl[2][2]},fr[3]={-rr[0][2],-rr[1][2],-rr[2][2]};   // the eye's forward (0,0,-1) in the head's frame
      check(fl[0]<-0.17&&fr[0]>0.17&&std::fabs(fl[0]+std::sin(rad(10)))<1e-6&&std::fabs(fr[0]-std::sin(rad(10)))<1e-6&&fl[2]<0&&fr[2]<0&&std::fabs(fl[1])<1e-7,
            "level eyes at 10 degrees: the left eye's forward has x < 0 and the right's x > 0 (outward), both still facing forward");
      // The hand-checked frustum: a symmetric 45/45/45/45 degree eye turned by a = +10 (left). Written out by hand, a corner ray (dx, dy, -1) seen from the
      // turned eye has x' = (cos a dx + sin a)/(cos a - sin a dx) and y' = dy/(cos a - sin a dx); with dx = -1 and +1 and |dy| = 1 that is
      //   left  = -(cos a - sin a)/(cos a + sin a) = -tan(45 - a) = -tan 35,   right = (cos a + sin a)/(cos a - sin a) = tan(45 + a) = tan 55,
      //   up    = 1/(cos a - sin a) (the corner at dx = +1, the nearest to the edge of the turned image plane), down = -up.
      const double a=rad(10);
      const double handLeft=-std::tan(rad(35)),handRight=std::tan(rad(55)),handUp=1.0/(std::cos(a)-std::sin(a));
      check(std::fabs(tanOf(level[0].fov.angleLeft)-handLeft)<tolerance&&std::fabs(tanOf(level[0].fov.angleRight)-handRight)<tolerance&&
            std::fabs(tanOf(level[0].fov.angleUp)-handUp)<tolerance&&std::fabs(tanOf(level[0].fov.angleDown)+handUp)<tolerance,
            "the left eye of a symmetric 45/45/45/45 frustum at 10 degrees: left = -tan 35, right = tan 55, up = -down = 1/(cos 10 - sin 10)");
      check(std::fabs(double(level[0].fov.angleLeft)+rad(35))<1e-6&&std::fabs(double(level[0].fov.angleRight)-rad(55))<1e-6,
            "...which is -35 and +55 degrees as angles");
      check(std::fabs(tanOf(level[1].fov.angleLeft)+handRight)<tolerance&&std::fabs(tanOf(level[1].fov.angleRight)+handLeft)<tolerance&&
            std::fabs(tanOf(level[1].fov.angleUp)-handUp)<tolerance&&std::fabs(tanOf(level[1].fov.angleDown)+handUp)<tolerance,
            "...and the right eye is the mirror: left = -tan 55, right = tan 35, the same up and down");
      check(std::fabs(tanOf(level[0].fov.angleLeft)+0.7002)<1e-4&&std::fabs(tanOf(level[0].fov.angleRight)-1.4281)<1e-4&&std::fabs(tanOf(level[0].fov.angleUp)-1.2328)<1e-4,
            "...to the digits worked out by hand: -0.7002, 1.4281, 1.2328");
    }
    // Mirrored inputs give mirrored boxes, whatever the frustum: the right eye's fov is the left's flipped in x.
    {
      XrView pair[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
      for(auto& v:pair)v.pose.orientation={0,0,0,1};
      pair[0].fov=fovDegrees(-52,38,41,-49);
      pair[1].fov=fovDegrees(-38,52,41,-49);
      CantReport r{};check(simulateCant(pair,10.0f,&r)==CantOutcome::Applied,"(mirrored pair) applied");
      check(std::fabs(tanOf(pair[1].fov.angleLeft)+tanOf(pair[0].fov.angleRight))<tolerance&&std::fabs(tanOf(pair[1].fov.angleRight)+tanOf(pair[0].fov.angleLeft))<tolerance&&
            std::fabs(tanOf(pair[1].fov.angleUp)-tanOf(pair[0].fov.angleUp))<tolerance&&std::fabs(tanOf(pair[1].fov.angleDown)-tanOf(pair[0].fov.angleDown))<tolerance,
            "mirrored inputs give mirrored boxes: the right eye's box is the left eye's flipped in x");
    }
    // Every ray of the located frustum lands inside the new box, and the box is the tightest one: the four corners touch its four edges. The mapping is taken
    // from the output orientation alone (R_new^T R_old d), not from the formula the source uses.
    for(unsigned eye=0;eye<2;++eye) {
      const auto& box=canted[eye].fov;
      const double left=tanOf(box.angleLeft),right=tanOf(box.angleRight),up=tanOf(box.angleUp),down=tanOf(box.angleDown);
      const double lo[2]={tanOf(views[eye].fov.angleLeft),tanOf(views[eye].fov.angleDown)},hi[2]={tanOf(views[eye].fov.angleRight),tanOf(views[eye].fov.angleUp)};
      bool inside=true;double minX=1e9,maxX=-1e9,minY=1e9,maxY=-1e9;
      for(int i=0;i<=8;++i)for(int j=0;j<=8;++j) {
        const double d[3]={lo[0]+(hi[0]-lo[0])*i/8.0,lo[1]+(hi[1]-lo[1])*j/8.0,-1.0};
        double n[3];intoFrame(views[eye].pose.orientation,canted[eye].pose.orientation,d,n);
        if(!(n[2]<0)){inside=false;continue;}
        const double tx=n[0]/-n[2],ty=n[1]/-n[2];
        inside=inside&&tx>=left-tolerance&&tx<=right+tolerance&&ty>=down-tolerance&&ty<=up+tolerance;
        if((i==0||i==8)&&(j==0||j==8)){minX=(std::min)(minX,tx);maxX=(std::max)(maxX,tx);minY=(std::min)(minY,ty);maxY=(std::max)(maxY,ty);}
      }
      check(inside,eye==0?"every ray of the located left frustum (a 9 x 9 grid, edges and corners included) lands inside the new box":
                          "every ray of the located right frustum lands inside the new box");
      check(std::fabs(minX-left)<tolerance&&std::fabs(maxX-right)<tolerance&&std::fabs(minY-down)<tolerance&&std::fabs(maxY-up)<tolerance,
            eye==0?"...and the left box is the tightest: its four edges are the mapped corners'":"...and the right box is the tightest: its four edges are the mapped corners'");
    }
    // The pair must stay a valid pair for the rest of the runtime: fovToRaw accepts the told fov, and a snapshot made from it keeps the eyes' separation.
    {
      GeometryInput in{};in.generation=1;in.sequence=1;in.viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
      in.headFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;in.headPose.orientation={0,0,0,1};
      for(unsigned eye=0;eye<2;++eye){in.views[eye]=canted[eye];in.width[eye]=2000;in.height[eye]=2200;}
      GeometrySnapshot snapshot{};
      check(makeGeometrySnapshot(in,snapshot),"a snapshot of the canted pair is valid");
      RawFov raw[2]{};check(fovToRaw(canted[0].fov,raw[0])&&fovToRaw(canted[1].fov,raw[1])&&snapshot.raw[0].left==raw[0].left&&snapshot.raw[1].right==raw[1].right,
        "...and its raw tangents are the told fov's");
    }
    // The located eye-to-head transform carries the cant: with the head level and the eyes level, the left eye's forward is yawed 10 degrees toward -x in head
    // space and the right's 10 toward +x. This is what the game is handed with the correction off (and, mirrored, what it composes raw).
    {
      XrView level[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
      for(auto& v:level){v.pose.orientation={0,0,0,1};v.fov=fovDegrees(-45,45,45,-45);}
      level[0].pose.position={-0.032f,0,0};level[1].pose.position={0.032f,0,0};
      check(simulateCant(level,10.0f,nullptr)==CantOutcome::Applied,"(level eyes, snapshot) applied");
      GeometryInput in{};in.generation=1;in.sequence=1;in.viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
      in.headFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;in.headPose.orientation={0,0,0,1};
      for(unsigned eye=0;eye<2;++eye){in.views[eye]=level[eye];in.width[eye]=2000;in.height[eye]=2200;}
      GeometrySnapshot snapshot{};
      check(makeGeometrySnapshot(in,snapshot)&&std::fabs(forwardYawDegrees(snapshot.eyeToHead[0])+10.0f)<1e-3f&&std::fabs(forwardYawDegrees(snapshot.eyeToHead[1])-10.0f)<1e-3f&&
            std::fabs(forwardYawDegrees(gameHandedness(snapshot.eyeToHead[0]))-10.0f)<1e-3f,
            "the located eye-to-head transforms carry the cant: left yawed -10 degrees, right +10; the corrected left reads +10");
    }
    // A frustum so wide a corner ray falls behind the turned eye: stand down, everything untouched.
    {
      XrView wide[2]={views[0],views[1]};wide[0].fov=fovDegrees(-60,86,45,-45);wide[1].fov=fovDegrees(-86,60,45,-45);
      XrView before[2]={wide[0],wide[1]};CantReport r{};r.toldFov[0].angleUp=5.0f;
      check(simulateCant(wide,10.0f,&r)==CantOutcome::StoodDown&&bitwiseEqual(wide[0],before[0])&&bitwiseEqual(wide[1],before[1])&&r.toldFov[0].angleUp==5.0f,
            "a corner ray that falls behind a turned eye stands the simulation down: the pair and the report are untouched");
    }
  }
  // ---- the host's wiring -------------------------------------------------------------------------------------------------------------------------------------
  {
    using namespace launch_fixture;
    Fixture f;auto& h=f.host;
    GeometryInput located{};for(unsigned eye=0;eye<2;++eye)located.views[eye]=views[eye];
    const GeometryInput untouched=located;
    check(!h.applySimulatedCant(located)&&bitwiseEqual(located.views[0],untouched.views[0])&&bitwiseEqual(located.views[1],untouched.views[1])&&!h.cantTest.simNoted&&!h.cantTest.pending,
          "no provider answer yet: the located views are untouched and nothing is noted (so a log without the canted test lines means it never ran)");
    h.featureFrameKnown=true;h.featureFrame.simulateCantDeg=0.0f;
    check(!h.applySimulatedCant(located)&&bitwiseEqual(located.views[0],untouched.views[0])&&h.cantTest.simNoted&&h.cantTest.pending&&h.cantTest.outcome==CantOutcome::Off,
          "simulate_cant 0: the views are untouched, and the first use is noted (canted test: off)");
    h.cantTest.pending=false;
    check(!h.applySimulatedCant(located)&&!h.cantTest.pending,"...and noted once: the same value again is not a change");
    h.featureFrame.simulateCantDeg=10.0f;
    check(h.applySimulatedCant(located)&&!bitwiseEqual(located.views[0],untouched.views[0])&&h.cantTest.pending&&h.cantTest.outcome==CantOutcome::Applied&&h.cantTest.appliedDeg==10.0f,
          "10 degrees: the views are canted, and the change is noted");
    h.cantTest.pending=false;
    GeometryInput second=untouched;
    check(h.applySimulatedCant(second)&&bitwiseEqual(second.views[0],located.views[0])&&bitwiseEqual(second.views[1],located.views[1])&&!h.cantTest.pending,
          "...the next frame gets the same canted pair from the same located one, and no second note");
    h.featureFrame.simulateCantDeg=7.0f;GeometryInput third=untouched;
    check(h.applySimulatedCant(third)&&h.cantTest.pending&&h.cantTest.appliedDeg==7.0f&&!bitwiseEqual(third.views[0],second.views[0]),"a different cant is a change");
    h.cantTest.pending=false;
    h.featureFrame.simulateCantDeg=0.0f;GeometryInput fourth=untouched;
    check(!h.applySimulatedCant(fourth)&&bitwiseEqual(fourth.views[0],untouched.views[0])&&h.cantTest.pending&&h.cantTest.outcome==CantOutcome::Off,"back to 0: off again, noted");
    // The runtime's hidden-area mesh is cut for the located frustum, so a simulated cant withholds it exactly as a trim does.
    {
      Fixture g2;auto& k=g2.host;
      check(k.hiddenMeshCompatible(false)&&!k.hiddenMeshCompatible(true),"no provider answer yet: the hidden-area mesh is served, and withheld while a cant is applied");
      k.featureFrameKnown=true;
      check(k.hiddenMeshCompatible(false)&&!k.hiddenMeshCompatible(true),"...an answer with no trim says the same");
      float* const trims[3]={&k.featureFrame.trimOuterDeg,&k.featureFrame.trimNasalDeg,&k.featureFrame.trimVerticalDeg};
      for(float* trim:trims) {
        *trim=3.0f;
        check(!k.hiddenMeshCompatible(false)&&!k.hiddenMeshCompatible(true),"...and so does a trim, outer, nasal or vertical");
        *trim=0.0f;
      }
      check(k.hiddenMeshCompatible(false),"...and nothing configured serves it again");
    }
    // The handedness correction: published to the game's reads every frame, noted on change.
    h.cantTest.pending=false;
    h.publishCantedEyeFix(true);
    check(h.geometry.read().cantedEyeFix&&h.cantTest.fixNoted&&h.cantTest.fix&&h.cantTest.pending,"canted_eye_fix on is published to the game's reads and noted");
    h.cantTest.pending=false;h.publishCantedEyeFix(true);
    check(h.geometry.read().cantedEyeFix&&!h.cantTest.pending,"...the same value again is published but not noted twice");
    h.publishCantedEyeFix(false);
    check(!h.geometry.read().cantedEyeFix&&h.cantTest.pending&&!h.cantTest.fix,"canted_eye_fix off is published and noted");
    // The epoch: a change counts once the frame is published, and each eye reads it once.
    check(h.cantTest.epoch.load()==0,"nothing is counted while the frame is still being built");
    h.commitCantTestChange();
    check(h.cantTest.epoch.load()==1&&!h.cantTest.pending,"...the published frame counts the change");
    h.commitCantTestChange();
    check(h.cantTest.epoch.load()==1,"...once");
    const SystemRead none{};vr::HmdMatrix34_t m{};
    h.noteEyeToHead(none,0,m,m);h.noteEyeToHead(none,0,m,m);
    check(h.cantTest.eyeNoted[0].load()==1&&h.cantTest.eyeNoted[1].load()==0,"the first eye-to-head call per eye after a change is the one that is noted: eye 0 now, eye 1 not yet");
    h.noteEyeToHead(none,1,m,m);h.noteEyeToHead(none,7,m,m);
    check(h.cantTest.eyeNoted[1].load()==1,"...then eye 1; an eye index outside 0 and 1 is ignored");
    h.cantTest.pending=true;h.commitCantTestChange();
    check(h.cantTest.epoch.load()==2&&h.cantTest.eyeNoted[0].load()==1,"a later change makes the next call per eye a first call again");
  }
}
} // namespace edvr::openxr::test
