#pragma once
// F189 (#144): the seat origin as pose math. A reference space made with poseInReferenceSpace = O is only a rigid transform of the natural space N: a pose X located in
// N reads O^-1 * X in that space, and a pose S submitted in it stands for O * S in N. The runtime used to be asked for a new space at every origin change (82-90 a second
// on foot); layer-generating API layers reset their history on each layer-space change. Now the runtime sees N alone and EDVR does this arithmetic at two edges:
//   the locate edge: what xrLocateSpace / xrLocateViews returned in N becomes seated numbers (toSeated, velocities turned by the inverse rotation),
//   the submit edge: every pose of a composition layer becomes a pose in N (toNatural), the layer's space being N.
// Everything between the edges stays in seated coordinates, as before. Doubles inside, floats at the edges. An exactly-identity origin (the state until the first
// origin is set) returns its input bit for bit, so the numbers before the first reset are the runtime's own.
#include <openxr/openxr.h>
#include <cmath>
#include <cstdint>

namespace edvr::openxr {

// What a frame needs to locate and submit in seated coordinates: the natural space the runtime knows, and the origin the seated space stood for when the frame began.
// A frame keeps this snapshot for its locates and its submit, whatever the origin does after.
struct SeatedFrame {
  XrSpace space = XR_NULL_HANDLE;
  XrPosef origin{{0, 0, 0, 1}, {0, 0, 0}};
};

namespace seatxf {

inline bool isIdentity(const XrPosef& p) {
  return p.orientation.x == 0.0f && p.orientation.y == 0.0f && p.orientation.z == 0.0f && p.orientation.w == 1.0f &&
         p.position.x == 0.0f && p.position.y == 0.0f && p.position.z == 0.0f;
}

struct Quat { double x, y, z, w; };
inline Quat unit(const XrQuaternionf& q) {
  const double x = q.x, y = q.y, z = q.z, w = q.w;
  const double n = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(n > 0.0)) return {0, 0, 0, 1};
  return {x / n, y / n, z / n, w / n};
}
inline Quat conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
// Hamilton product a * b: b's rotation first, then a's.
inline Quat mul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline void rotate(const Quat& q, const double v[3], double out[3]) {
  // v' = v + 2w (u x v) + 2 u x (u x v), u = (q.x, q.y, q.z).
  const double tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
  out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
  out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
  out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}
inline XrQuaternionf toXr(const Quat& q) {
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(n > 0.0)) return {0, 0, 0, 1};
  return {float(q.x / n), float(q.y / n), float(q.z / n), float(q.w / n)};
}

// The seated pose of a pose located in the natural space: origin^-1 * natural.
inline XrPosef toSeated(const XrPosef& origin, const XrPosef& natural) {
  if (isIdentity(origin)) return natural;
  const Quat inverse = conj(unit(origin.orientation));
  const double d[3] = {double(natural.position.x) - origin.position.x, double(natural.position.y) - origin.position.y,
                       double(natural.position.z) - origin.position.z};
  double r[3];
  rotate(inverse, d, r);
  XrPosef out;
  out.orientation = toXr(mul(inverse, unit(natural.orientation)));
  out.position = {float(r[0]), float(r[1]), float(r[2])};
  return out;
}
// The pose in the natural space of a pose in seated coordinates: origin * seated. The layer's pose, with the layer's space the natural one.
inline XrPosef toNatural(const XrPosef& origin, const XrPosef& seated) {
  if (isIdentity(origin)) return seated;
  const Quat rotation = unit(origin.orientation);
  const double p[3] = {seated.position.x, seated.position.y, seated.position.z};
  double r[3];
  rotate(rotation, p, r);
  XrPosef out;
  out.orientation = toXr(mul(rotation, unit(seated.orientation)));
  out.position = {float(double(origin.position.x) + r[0]), float(double(origin.position.y) + r[1]), float(double(origin.position.z) + r[2])};
  return out;
}
// A velocity (linear or angular) located in the natural space, in seated coordinates. The origin is a fixed rigid transform, so a velocity is only turned (by the
// inverse of the origin's rotation); there is no lever-arm term.
inline XrVector3f velocityToSeated(const XrPosef& origin, const XrVector3f& natural) {
  if (isIdentity(origin)) return natural;
  const double v[3] = {natural.x, natural.y, natural.z};
  double r[3];
  rotate(conj(unit(origin.orientation)), v, r);
  return {float(r[0]), float(r[1]), float(r[2])};
}

// The locate edge. Each part is converted only when the runtime called it valid: a part it did not vouch for is left as it came, because nobody reads it, and the
// submit edge then maps it back to the physical pose a never-recreated seated space would have given it.
inline void poseToSeated(const XrPosef& origin, XrPosef& pose, bool orientationValid, bool positionValid) {
  if (isIdentity(origin) || !(orientationValid || positionValid)) return;
  const XrPosef converted = toSeated(origin, pose);
  if (orientationValid) pose.orientation = converted.orientation;
  if (positionValid) pose.position = converted.position;
}
inline void locationToSeated(const XrPosef& origin, XrSpaceLocation& location) {
  poseToSeated(origin, location.pose, (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0,
               (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
}
inline void velocityToSeated(const XrPosef& origin, XrSpaceVelocity& velocity) {
  if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) velocity.linearVelocity = velocityToSeated(origin, velocity.linearVelocity);
  if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) velocity.angularVelocity = velocityToSeated(origin, velocity.angularVelocity);
}
inline void viewsToSeated(const XrPosef& origin, XrViewStateFlags flags, XrView* views, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    poseToSeated(origin, views[i].pose, (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0, (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
}

} // namespace seatxf
} // namespace edvr::openxr
