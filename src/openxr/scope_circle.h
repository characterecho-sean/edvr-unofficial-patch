#pragma once
// F205/F206: the runtime's pure half of the ADS view circle (src\common\view_circle.h is the request). The renderer draws black outside a circle about a direction of each eye's own view; this is the
// arithmetic it and its rig share: the circle's constants for the share shader's circle mode (misc.w = 2), the black weight the shader computes, and the radius that keeps the full-detail
// scope's (B3) side bars inside the black. F206: that radius IS the circle's radius: the widest circle that still hides the bars, worked out every frame from the actual centre in each eye.
//
// The shader (kStereoShareHlsl, circle mode). A pixel's direction d in THIS eye's view is (lerp(leftTan.x, leftTan.y, u), lerp(leftTan.z, leftTan.w, v), -1); its angle from the centre's unit
// direction c is theta = acos(dot(d / |d|, c)); the black weight is weight x (1 - saturate((E - theta - margin) / band)) with E = R + margin + band / 2, so the pixel is half black at theta = R
// (the radius, as the d3d11 half states it), clear from R - band / 2 inward and fully black from R + band / 2 outward. margin and band are the share's (eye_share.h: 0.25 and 3 degrees):
// the same soft edge the black bars have. The constants ride the share's constant buffer unchanged in size: rightAng holds {c.x, c.y, c.z, E}, band {margin, 1 / band, weight, 0}, misc.w = 2.
//
// F210: nothing here depends on an eye's pixel aspect or on how asymmetric its frustum is. A pixel's direction is the tangent plane's point lerp'd across THAT eye's own image with THAT eye's own tangents,
// the centre is a direction in the same plane, and every distance is an angle between unit directions, so the circle is round in angle and sits on the centre whatever the image's size and the frustum's
// shape. openxr_stereo_test's circleAspectSelfTest renders the real frame into the Quest 3's eyes (3072 x 3264 and 2064 x 2208; eye 0 left -53.42, right +39.41, up +41.99, down -53.08 degrees, eye 1 the
// mirror; displays parallel and canted) and a synthetic one, and measures the drawn edge in the pixels: it is at one angle from the centre to 0.01 degree and its centre is the centre to 0.001 degree.
// What a Quest 3 eye shows is the centre's PLACE in the image: a sight on the forward axis is at (0.621, 0.403) of the left eye's image, 372 px right of and 315 px above its middle at 3072 x 3264.
#include "eye_share.h"

#include <cmath>

namespace edvr::openxr::scopecircle {

using shareview::kPi;

// The unit direction (x, y, -1) / |.| of a tangent-plane point in an eye's view (x right, y up, -z forward).
inline bool unitOf(const float tc[2], double u[3]) {
    if (!std::isfinite(tc[0]) || !std::isfinite(tc[1])) return false;
    const double x = tc[0], y = tc[1], n = std::sqrt(x * x + y * y + 1.0);
    u[0] = x / n;
    u[1] = y / n;
    u[2] = -1.0 / n;
    return true;
}

// F210b: the circle sits at the middle of the eye's field vertically. The sight's direction is pitched by delta, the mean over both eyes of (angleUp + angleDown) / 2 with OpenXR's signed angles
// (on the Quest 3: (+41.99 - 53.08) / 2 = -5.5 degrees, down), a rotation about the HEAD's horizontal axis, so the two eyes' centres stay one world direction even when their displays are canted
// (a rotation about each eye's own x axis would not). The head's horizontal axis is the mean of the two eyes' right axes in the layer's space (displays canted about the head's y axis give the head's
// x), seen from an eye as R_eye^T x_head. A headset whose up and down angles are opposite has delta exactly 0 and the centre is not touched: the Pimax's frames stay what they were.
inline double fieldMiddleRad(const XrFovf& a, const XrFovf& b) {
    return 0.25 * (double(a.angleUp) + double(a.angleDown) + double(b.angleUp) + double(b.angleDown));
}
// The head's right axis in the layer's space: the sum of the two eyes' right axes, unit. False when the eyes' axes cancel.
inline bool headRightAxis(const XrQuaternionf& q0, const XrQuaternionf& q1, double out[3]) {
    double r0[3][3], r1[3][3];
    shareview::rotationOf(q0, r0);
    shareview::rotationOf(q1, r1);
    const double v[3] = {r0[0][0] + r1[0][0], r0[1][0] + r1[1][0], r0[2][0] + r1[2][0]};
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(n) || !(n > 1e-6)) return false;
    for (int i = 0; i < 3; ++i) out[i] = v[i] / n;
    return true;
}
// A direction of the layer's space seen from an eye's view: R_eye^T d.
inline void inEyeFrame(const XrQuaternionf& q, const double d[3], double out[3]) {
    double r[3][3];
    shareview::rotationOf(q, r);
    for (int i = 0; i < 3; ++i) out[i] = r[0][i] * d[0] + r[1][i] * d[1] + r[2][i] * d[2];
}
// The tangent-plane point tc turned by delta radians about the unit axis (right-handed: about +x, a positive delta takes the forward direction up), as a tangent-plane point again. False when tc is not a
// number or the turned direction is no longer in front of the view.
inline bool pitchCentre(const float tc[2], const double axis[3], double delta, float out[2]) {
    double u[3];
    if (!unitOf(tc, u)) return false;
    const double c = std::cos(delta), s = std::sin(delta);
    const double cr[3] = {axis[1] * u[2] - axis[2] * u[1], axis[2] * u[0] - axis[0] * u[2], axis[0] * u[1] - axis[1] * u[0]};
    const double dot = axis[0] * u[0] + axis[1] * u[1] + axis[2] * u[2];
    double v[3];
    for (int i = 0; i < 3; ++i) v[i] = u[i] * c + cr[i] * s + axis[i] * dot * (1.0 - c);
    if (!(v[2] < -1e-6)) return false;
    const double tx = v[0] / -v[2], ty = v[1] / -v[2];
    if (!std::isfinite(tx) || !std::isfinite(ty)) return false;
    out[0] = static_cast<float>(tx);
    out[1] = static_cast<float>(ty);
    return true;
}

// The shader's black weight for a pixel at angle theta from the centre (radians), with the radius R, the margin and the band, all radians, and the request's weight.
inline double blackWeight(double theta, double radius, double marginRad, double bandRad, double weight) {
    const double edgeAt = radius + marginRad + 0.5 * bandRad;
    double clear = (edgeAt - theta - marginRad) / bandRad;
    clear = clear < 0.0 ? 0.0 : clear > 1.0 ? 1.0 : clear;
    return weight * (1.0 - clear);
}
// The angle between the ray through a tangent-plane point of an eye's view and the centre's unit direction c.
inline double angleFromCentre(double tx, double ty, const double c[3]) {
    const double n = std::sqrt(tx * tx + ty * ty + 1.0);
    double d = (tx * c[0] + ty * c[1] - c[2]) / n;
    d = d < -1.0 ? -1.0 : d > 1.0 ? 1.0 : d;
    return std::acos(d);
}

// How far inside a rectangle of axis angles a unit direction u (view coordinates) is, as the angle to the nearest of the four planes bounding it, radians; negative when it is outside, or
// behind the view. ang is eye_share.h Mapping::ang: x min, x max, y min, y max (radians). A plane x cos A + z sin A = 0 holds the points whose horizontal axis angle atan2(x, -z) is A, so the
// angle from u to it is asin(u . n) with n = (cos A, 0, sin A); the y planes are the same about the other axis. This is the distance the shader's own `edge` measures in axis angles, taken
// as a true angle between rays (the smaller of the two), so a circle that fits it fits whatever way it is measured.
inline double rectDistance(const double u[3], const double ang[4]) {
    if (!(u[2] < -1e-9)) return -1.0;
    const auto clampAsin = [](double v) { return std::asin(v < -1.0 ? -1.0 : v > 1.0 ? 1.0 : v); };
    const double xmin = clampAsin(u[0] * std::cos(ang[0]) + u[2] * std::sin(ang[0]));
    const double xmax = clampAsin(-(u[0] * std::cos(ang[1]) + u[2] * std::sin(ang[1])));
    const double ymin = clampAsin(u[1] * std::cos(ang[2]) + u[2] * std::sin(ang[2]));
    const double ymax = clampAsin(-(u[1] * std::cos(ang[3]) + u[2] * std::sin(ang[3])));
    return std::fmin(std::fmin(xmin, xmax), std::fmin(ymin, ymax));
}

// A direction of the left view into the right view by a Mapping's rotation.
inline void turn(const double rot[3][3], const double u[3], double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = rot[i][0] * u[0] + rot[i][1] * u[1] + rot[i][2] * u[2];
}

// The largest radius at which the circle hides the full-detail scope's side bars: the distance from the centre to the nearest edge of what the other eye covers, less the margin and half the band.
// Both ramps are 3 deg wide and linear, the bars' from fully black at the margin inside the edge to clear a band further in, the circle's from clear at R - band / 2 to fully black at R + band / 2;
// at R = D - margin - band / 2 they are the same ramp along the nearest direction, so the circle's black is at least the bars' at every pixel (a smaller R only blackens more) and a larger R lets a
// bar's ramp show inside the circle's clear part: this is the widest circle that hides them. (Half a band, not a whole one: the radius is where the circle is HALF black.) The left eye's circle is judged in the right view (the share's rectangle,
// share.rot turning the left view into it), the right eye's in the left view (the mirror's rectangle, mirror.rot turning the right view into it); the smaller wins. Radians; negative or
// NaN when a centre or a mapping is unusable. cl, cr: the circle's centre as tangents in the left and the right eye's view.
inline double coverageRadius(const shareview::Mapping& share, const shareview::Mapping& mirror, const float cl[2], const float cr[2]) {
    double ul[3], ur[3];
    if (!share.valid || !mirror.valid || !unitOf(cl, ul) || !unitOf(cr, ur)) return std::nan("");
    double inRight[3], inLeft[3];
    turn(share.rot, ul, inRight);
    turn(mirror.rot, ur, inLeft);
    const double d = std::fmin(rectDistance(inRight, share.ang), rectDistance(inLeft, mirror.ang));
    return d - share.marginRad - 0.5 * share.bandRad;
}

}  // namespace edvr::openxr::scopecircle
