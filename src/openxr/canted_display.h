#pragma once

// The canted-display arc's two temporary test instruments (docs\canted-projection.md).
// Pure math and no runtime state, so a rig can drive every case.
//
//   simulateCant       gives the located stereo pair an outward cant, so a parallel-panel
//                      headset can stand in for a canted one (advanced.simulate_cant).
//   gameHandedness     S*E*S of an eye-to-head matrix, S = diag(1,1,-1): the eye matrix in
//                      the handedness Elite composes it in (advanced.canted_eye_fix).
//   forwardYawDegrees  the signed yaw of an eye's forward axis in head space.
//
// Both keys go when the arc closes.
#include "geometry_snapshot.h"
#include <cmath>

namespace edvr::openxr {

constexpr float kMaxSimulatedCantDegrees = 15.0f;

// Degrees in, degrees out: 0 (off) to 15, anything that is not a number off.
inline float sanitizeSimulatedCant(float degrees) {
  if (!(degrees > 0.0f) || !std::isfinite(degrees)) return 0.0f;
  return degrees > kMaxSimulatedCantDegrees ? kMaxSimulatedCantDegrees : degrees;
}

enum class CantOutcome { Off, Applied, StoodDown };

// The fov each eye was located with and the one it now carries, for the log.
struct CantReport {
  XrFovf trueFov[2]{};
  XrFovf toldFov[2]{};
};

namespace detail {
// Hamilton product a*b: the rotation b first, then a.
inline XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
  const double ax = a.x, ay = a.y, az = a.z, aw = a.w, bx = b.x, by = b.y, bz = b.z, bw = b.w;
  return {float(aw*bx + ax*bw + ay*bz - az*by),
          float(aw*by - ax*bz + ay*bw + az*bx),
          float(aw*bz + ax*by - ay*bx + az*bw),
          float(aw*bw - ax*bx - ay*by - az*bz)};
}

// Turn one eye by `angle` radians about its own +y (positive turns its forward
// (0,0,-1) toward -x) and re-express the same frustum in the turned frame.
// False, leaving `out` unwritten, when a corner ray would fall on or behind the
// turned eye's image plane, or the fov is not a usable frustum.
inline bool cantEye(const XrView& in, double angle, XrView& out) {
  const double c = std::cos(angle), s = std::sin(angle);
  const double xs[2] = {std::tan(double(in.fov.angleLeft)), std::tan(double(in.fov.angleRight))};
  const double ys[2] = {std::tan(double(in.fov.angleDown)), std::tan(double(in.fov.angleUp))};
  double minX = 0, maxX = 0, minY = 0, maxY = 0;
  bool first = true;
  for (const double dx : xs) for (const double dy : ys) {
    // The corner ray (dx, dy, -1) seen from the turned eye is Ry(-angle) * d:
    // x' = c*dx + s, y' = dy, -z' = c - s*dx.
    const double depth = c - s*dx;
    if (!(depth > 1.0e-6) || !std::isfinite(depth)) return false;
    const double tx = (c*dx + s)/depth, ty = dy/depth;
    if (!std::isfinite(tx) || !std::isfinite(ty)) return false;
    if (first) { minX = maxX = tx; minY = maxY = ty; first = false; }
    else {
      if (tx < minX) minX = tx;
      if (tx > maxX) maxX = tx;
      if (ty < minY) minY = ty;
      if (ty > maxY) maxY = ty;
    }
  }
  XrView made = in;
  made.fov = {float(std::atan(minX)), float(std::atan(maxX)), float(std::atan(maxY)), float(std::atan(minY))};
  const XrQuaternionf turn = {0.0f, float(std::sin(angle*0.5)), 0.0f, float(std::cos(angle*0.5))};
  XrQuaternionf q = multiply(in.pose.orientation, turn);
  const double norm = std::sqrt(double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w);
  if (!(norm > 0.0) || !std::isfinite(norm)) return false;
  q = {float(q.x/norm), float(q.y/norm), float(q.z/norm), float(q.w/norm)};
  made.pose.orientation = q;
  if (!(made.fov.angleLeft < made.fov.angleRight) || !(made.fov.angleDown < made.fov.angleUp)) return false;
  out = made;
  return true;
}
}  // namespace detail

// Give a located stereo pair an outward cant of `degrees` per eye: the left eye
// (index 0) is turned by Ry(+theta) so its forward points toward -x, the right
// by Ry(-theta); positions are untouched. Each eye's fov becomes the bounding
// box, in the turned frame's tangent plane, of the four corner rays of the
// located fov, which holds the turned frustum exactly (a rotation keeps the
// frustum's edges straight in the tangent plane).
//
// Off (0, or not a number) leaves `views` bit-identical. StoodDown leaves them
// untouched too: a corner ray fell behind a turned eye.
inline CantOutcome simulateCant(XrView (&views)[2], float degrees, CantReport* report = nullptr) {
  const float clamped = sanitizeSimulatedCant(degrees);
  if (clamped <= 0.0f) return CantOutcome::Off;
  const double theta = double(clamped)*(3.14159265358979323846/180.0);
  XrView made[2]{};
  for (unsigned eye = 0; eye < 2; ++eye)
    if (!detail::cantEye(views[eye], eye == 0 ? theta : -theta, made[eye])) return CantOutcome::StoodDown;
  if (report) for (unsigned eye = 0; eye < 2; ++eye) {
    report->trueFov[eye] = views[eye].fov;
    report->toldFov[eye] = made[eye].fov;
  }
  views[0] = made[0];
  views[1] = made[1];
  return CantOutcome::Applied;
}

// S*E*S, S = diag(1,1,-1): the 3x4's m[0][2], m[1][2], m[2][0], m[2][1] and
// m[2][3] change sign, everything else stays. 0 - x rather than -x so a zero
// stays +0.
inline vr::HmdMatrix34_t gameHandedness(const vr::HmdMatrix34_t& eyeToHead) {
  vr::HmdMatrix34_t out = eyeToHead;
  out.m[0][2] = 0.0f - eyeToHead.m[0][2];
  out.m[1][2] = 0.0f - eyeToHead.m[1][2];
  out.m[2][0] = 0.0f - eyeToHead.m[2][0];
  out.m[2][1] = 0.0f - eyeToHead.m[2][1];
  out.m[2][3] = 0.0f - eyeToHead.m[2][3];
  return out;
}

// The signed yaw, degrees, of an eye's forward axis from the head's forward:
// the axis (0,0,-1) carried through the matrix's rotation, measured about +y,
// positive toward +x. A left eye canted outward reads negative.
inline float forwardYawDegrees(const vr::HmdMatrix34_t& eyeToHead) {
  const double x = -double(eyeToHead.m[0][2]), z = -double(eyeToHead.m[2][2]);
  return float(std::atan2(x, -z)*(180.0/3.14159265358979323846));
}

}  // namespace edvr::openxr
