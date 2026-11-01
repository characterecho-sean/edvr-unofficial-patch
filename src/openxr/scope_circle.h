#pragma once
// F205: the runtime's pure half of the ADS view circle (src\common\view_circle.h is the request). The renderer draws black outside a circle about a direction of each eye's own view; this is the
// arithmetic it and its rig share: the circle's constants for the share shader's circle mode (misc.w = 2), the black weight the shader computes, and the radius that keeps the full-detail
// scope's (B3) side bars inside the black.
//
// The shader (kStereoShareHlsl, circle mode). A pixel's direction d in THIS eye's view is (lerp(leftTan.x, leftTan.y, u), lerp(leftTan.z, leftTan.w, v), -1); its angle from the centre's unit
// direction c is theta = acos(dot(d / |d|, c)); the black weight is weight x (1 - saturate((E - theta - margin) / band)) with E = R + margin + band / 2, so the pixel is half black at theta = R
// (the radius, as the d3d11 half states it), clear from R - band / 2 inward and fully black from R + band / 2 outward. margin and band are the share's (eye_share.h: 0.25 and 3 degrees):
// the same soft edge the black bars have. The constants ride the share's constant buffer unchanged in size: rightAng holds {c.x, c.y, c.z, E}, band {margin, 1 / band, weight, 0}, misc.w = 2.
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

// The largest radius at which the circle's black (fully black from R + band / 2 out) stays inside the black the full-detail scope's side bars already take, so the bars are always inside it: the
// distance from the centre to the nearest edge of what the other eye covers, less the margin and half the band. The left eye's circle is judged in the right view (the share's rectangle,
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
