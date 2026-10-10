#pragma once
// The handedness correction an eye-to-head answer carries (docs/canted-projection.md; src/openxr/canted_display.h): Elite composes each eye's matrix with
// its head pose in a z-negated space but takes the matrix raw, so the game is given S*E*S, S = diag(1,1,-1). The math is checked against the definition
// written out independently here (a 4x4 product) and against a hand-built canted pair; the host's one decision that still touches this arc, whether the
// runtime's hidden-area mesh may be served, against a fixture host. The answer to the game itself is held in tools\openxr_system_test.
#include "launch_centre_cases.h"
#include "../../src/openxr/canted_display.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace edvr::openxr::test {
namespace canted_fixture {
constexpr double kPi=3.14159265358979323846;
inline double rad(double degrees){return degrees*kPi/180.0;}
inline bool bitwiseEqual(const vr::HmdMatrix34_t& a,const vr::HmdMatrix34_t& b){return std::memcmp(&a,&b,sizeof(a))==0;}
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
}
template<class Check> void runCantedDisplayCases(Check&& check) {
  using namespace canted_fixture;
  // ---- the handedness correction ----------------------------------------------------------------------------------------------------------------------
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
    // A canted pair as a runtime would locate it: the left eye turned outward by 10 degrees, the right by the same the other way. Each corrected eye reads
    // the mirror of its located yaw, and the translation keeps its x and y.
    const auto left=eyeMatrix(10,0,-0.032f,0,0),right=eyeMatrix(-10,0,0.032f,0,0);
    const auto gl=gameHandedness(left),gr=gameHandedness(right);
    check(std::fabs(forwardYawDegrees(left)+10.0)<1e-4&&std::fabs(forwardYawDegrees(right)-10.0)<1e-4&&
              std::fabs(forwardYawDegrees(gl)-10.0)<1e-4&&std::fabs(forwardYawDegrees(gr)+10.0)<1e-4&&
              gl.m[0][3]==left.m[0][3]&&gr.m[0][3]==right.m[0][3]&&gl.m[1][3]==left.m[1][3]&&gr.m[1][3]==right.m[1][3],
          "a canted pair: the located left eye is yawed -10 and the right +10; corrected they read +10 and -10, with their x and y translation kept");
  }
  // ---- the host's one decision that still touches this arc -----------------------------------------------------------------------------------------------
  {
    using namespace launch_fixture;
    // The runtime's hidden-area mesh is cut for the located frustum, so a trim, which changes that frustum, withholds it.
    Fixture g2;auto& k=g2.host;
    check(k.hiddenMeshCompatible(),"no provider answer yet: the hidden-area mesh is served");
    k.featureFrameKnown=true;
    check(k.hiddenMeshCompatible(),"...an answer with no trim says the same");
    float* const trims[3]={&k.featureFrame.trimOuterDeg,&k.featureFrame.trimNasalDeg,&k.featureFrame.trimVerticalDeg};
    for(float* trim:trims) {
      *trim=3.0f;
      check(!k.hiddenMeshCompatible(),"...and a trim, outer, nasal or vertical, withholds it");
      *trim=0.0f;
    }
    check(k.hiddenMeshCompatible(),"...and nothing configured serves it again");
  }
}
} // namespace edvr::openxr::test
