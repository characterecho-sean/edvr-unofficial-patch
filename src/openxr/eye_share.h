#pragma once
// F106 (native on foot, fix.native_on_foot_ads_sight_both_eyes): while the sights are up the LEFT eye shows the RIGHT eye's image, turned into the left eye's view by
// rotation only, so both eyes see the scene from the aiming eye's position and the sight sits centred in both. The pure parts: the mapping from a left swapchain pixel
// to the texel of the right eye's captured image (the share shader, kStereoShareHlsl in stereo_shader_source.h, computes the same in float), its coverage, the right
// eye's hidden-area mask and the figures of the per-press line. Design and journal: docs\design-native-onfoot-vr-2026-10-13.md, F106.
//
// The mapping. A left pixel's centre (u, v as fractions of the left swapchain image, v down) is the direction (lerp(tl, tr, u), lerp(tu, td, v), -1) in the left view
// (OpenXR's view space: x right, y up, -z forward; tl, tr, tu, td the tangents of views[0].fov). inverse(q_right) * q_left, from views[i].pose.orientation (both in the
// layer's space), turns it into the right view, where it projects with views[1].fov to the right swapchain's u, v; the eye-1 blit's own mapping (its viewport within
// that swapchain, then the capture's bounds and clamp) takes it on to the texel of the right eye's capture. These are the views the projection layer hands the
// compositor, so the mapping holds on canted displays (each view carries its cant) and with asymmetric fovs. No position enters: the right image is shown as the right
// eye saw it. The layer keeps submitting each eye's own pose and fov.
//
// Coverage. A left pixel whose direction the right eye did not draw keeps the left eye's own image: behind the right view, outside its fov or outside the part of its
// swapchain its blit drew (a trimmed placement), or inside its hidden-area mesh (the masked corners). The share fades in over kBandDeg from the edge of what the right
// eye drew, starting kEdgeMarginDeg inside it, and from a zero band past the hidden area (two mask texels and two right-eye pixels, so neither the mask's nor the
// image's filtering reaches a hidden texel).
#include <openxr/openxr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace edvr::openxr::shareview {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBandDeg = 3.0;         // the cross-fade from the left eye's own image to the right eye's, at the edge of what the right eye drew
constexpr double kEdgeMarginDeg = 0.25;  // ... starting this far inside that edge (the clamp and the filter of the last texels)
constexpr uint32_t kMaskSize = 256;      // the hidden-area mask's texels a side, over the right view's fov
constexpr uint32_t kCoverageGrid = 48;   // the per-press coverage figure's samples a side, over the left eye's placement

struct Tangents { double left = 0, right = 0, up = 0, down = 0; };
inline Tangents tangentsOf(const XrFovf& f) {
    return {std::tan(double(f.angleLeft)), std::tan(double(f.angleRight)), std::tan(double(f.angleUp)), std::tan(double(f.angleDown))};
}
inline bool tangentsUsable(const Tangents& t) {
    for (double v : {t.left, t.right, t.up, t.down})
        if (!std::isfinite(v)) return false;
    return t.right - t.left > 1e-6 && t.up - t.down > 1e-6;
}

// The rotation of a unit quaternion, row-major: r * v turns a vector of the quaternion's frame into its parent's.
inline void rotationOf(const XrQuaternionf& q, double r[3][3]) {
    double x = q.x, y = q.y, z = q.z, w = q.w;
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 0) { x /= n; y /= n; z /= n; w /= n; }
    r[0][0] = 1 - 2 * (y * y + z * z); r[0][1] = 2 * (x * y - z * w);     r[0][2] = 2 * (x * z + y * w);
    r[1][0] = 2 * (x * y + z * w);     r[1][1] = 1 - 2 * (x * x + z * z); r[1][2] = 2 * (y * z - x * w);
    r[2][0] = 2 * (x * z - y * w);     r[2][1] = 2 * (y * z + x * w);     r[2][2] = 1 - 2 * (x * x + y * y);
}
// inverse(q_right) * q_left as a matrix: a direction in the left view to the same direction in the right view.
inline void relativeRotation(const XrQuaternionf& left, const XrQuaternionf& right, double m[3][3]) {
    double l[3][3], r[3][3];
    rotationOf(left, l);
    rotationOf(right, r);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = r[0][i] * l[0][j] + r[1][i] * l[1][j] + r[2][i] * l[2][j];
}
// The relative rotation as yaw (about y; positive turns the left view's forward to the left of the right view's), then pitch (about x), then roll (about z), in
// degrees: m = Ry(yaw) Rx(pitch) Rz(roll). Displays canted outward by c each give yaw 2c; parallel views give 0.
inline void yawPitchRollDeg(const double m[3][3], double out[3]) {
    const double s = (std::max)(-1.0, (std::min)(1.0, -m[1][2]));
    out[0] = std::atan2(m[0][2], m[2][2]) * 180.0 / kPi;
    out[1] = std::asin(s) * 180.0 / kPi;
    out[2] = std::atan2(m[1][0], m[1][1]) * 180.0 / kPi;
}

// Everything the shader needs but the textures, in double.
struct Mapping {
    bool valid = false;
    double rot[3][3] = {};    // a left view direction to the right view
    Tangents left, right;     // the two views' fov tangents
    double place[4] = {};     // the right eye's drawn image within its swapchain: u0, v0, u1, v1 (its blit's viewport over the image's size)
    double ang[4] = {};       // that rectangle as axis angles in the right view (radians): x min, x max, y min, y max
    double marginRad = kEdgeMarginDeg * kPi / 180.0, bandRad = kBandDeg * kPi / 180.0;
};
inline Mapping makeMapping(const XrFovf& leftFov, const XrQuaternionf& leftQ, const XrFovf& rightFov, const XrQuaternionf& rightQ, const double place[4]) {
    Mapping m;
    m.left = tangentsOf(leftFov);
    m.right = tangentsOf(rightFov);
    if (!tangentsUsable(m.left) || !tangentsUsable(m.right)) return m;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(place[i]) || place[i] < 0.0 || place[i] > 1.0) return m;
        m.place[i] = place[i];
    }
    if (!(m.place[2] > m.place[0]) || !(m.place[3] > m.place[1])) return m;
    relativeRotation(leftQ, rightQ, m.rot);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!std::isfinite(m.rot[i][j])) return m;
    const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    m.ang[0] = std::atan(lerp(m.right.left, m.right.right, m.place[0]));
    m.ang[1] = std::atan(lerp(m.right.left, m.right.right, m.place[2]));
    m.ang[2] = std::atan(lerp(m.right.up, m.right.down, m.place[3]));
    m.ang[3] = std::atan(lerp(m.right.up, m.right.down, m.place[1]));
    m.valid = true;
    return m;
}

// One left pixel: where its direction lands in the right eye's swapchain image, and how much of the right image it may take (before the hidden-area mask).
struct Lookup {
    bool ahead = false;   // the direction is in front of the right view
    double fov = 0.0;     // 0..1: the edge band of what the right eye drew (0 outside it)
    double su = 0.0, sv = 0.0;   // the right swapchain image's u, v (v down)
};
inline Lookup lookup(const Mapping& m, double u, double v) {
    Lookup k;
    const double d[3] = {m.left.left + (m.left.right - m.left.left) * u, m.left.up + (m.left.down - m.left.up) * v, -1.0};
    double r[3];
    for (int i = 0; i < 3; ++i) r[i] = m.rot[i][0] * d[0] + m.rot[i][1] * d[1] + m.rot[i][2] * d[2];
    if (!(r[2] < -1e-6)) return k;
    k.ahead = true;
    const double ax = std::atan2(r[0], -r[2]), ay = std::atan2(r[1], -r[2]);
    const double edge = (std::min)((std::min)(ax - m.ang[0], m.ang[1] - ax), (std::min)(ay - m.ang[2], m.ang[3] - ay));
    k.fov = (std::max)(0.0, (std::min)(1.0, (edge - m.marginRad) / m.bandRad));
    const double tx = r[0] / -r[2], ty = r[1] / -r[2];
    k.su = (tx - m.right.left) / (m.right.right - m.right.left);
    k.sv = (m.right.up - ty) / (m.right.up - m.right.down);
    return k;
}

// The right eye's hidden area as a mask over its fov: texel (i, j) is u = (i + 0.5) / size, v = (j + 0.5) / size of the right swapchain image (row 0 at the top), 255 where
// the share may take the right image, 0 in the hidden area and its zero band, a ramp between. The GPU samples it with the linear clamp sampler at the right image's u, v.
struct HiddenMask {
    uint32_t size = 0;
    std::vector<uint8_t> texels;
    Tangents domain;               // the right view's fov tangents the mask was built over
    uint32_t triangles = 0;        // the triangles it was built from
    uint32_t hiddenTexels = 0;     // texels touched by a triangle
    // Bilinear, clamped at the edges, as the GPU's sampler: 0..1.
    double sample(double su, double sv) const {
        if (!size || texels.size() != size_t(size) * size || !std::isfinite(su) || !std::isfinite(sv)) return 1.0;
        const double x = (std::max)(0.0, (std::min)(double(size) - 1.0, su * size - 0.5)), y = (std::max)(0.0, (std::min)(double(size) - 1.0, sv * size - 0.5));
        const uint32_t x0 = uint32_t(x), y0 = uint32_t(y), x1 = (std::min)(x0 + 1, size - 1), y1 = (std::min)(y0 + 1, size - 1);
        const double fx = x - x0, fy = y - y0;
        const auto t = [&](uint32_t i, uint32_t j) { return texels[size_t(j) * size + i] / 255.0; };
        return (t(x0, y0) * (1 - fx) + t(x1, y0) * fx) * (1 - fy) + (t(x0, y1) * (1 - fx) + t(x1, y1) * fx) * fy;
    }
};

// Built from OpenXR's visibility mask for the right view (XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR: points on the view's z = -1 plane, x right, y up), over
// `domain` (the right view's fov tangents), for a right swapchain image of width x height pixels. A texel is hidden when any part of it touches a triangle (each
// triangle is clipped to the texel row and its x extent marked), so the mask never misses a hidden point; the zero band and the ramp are measured from hidden texel
// centres on the tangent plane, the ramp's width taken as kBandDeg at the texel's own direction. False, with the mask emptied, for input it cannot use.
inline bool buildHiddenMask(const XrVector2f* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const Tangents& domain, uint32_t width,
                            uint32_t height, HiddenMask& out) {
    out = HiddenMask{};
    if (!tangentsUsable(domain) || !width || !height || (indexCount && (!vertices || !indices)) || indexCount % 3) return false;
    const uint32_t n = kMaskSize;
    const double spanX = domain.right - domain.left, spanY = domain.up - domain.down;
    std::vector<uint8_t> hidden(size_t(n) * n, 0);
    uint32_t triangles = 0;
    for (uint32_t t = 0; t + 2 < indexCount; t += 3) {
        double px[8], py[8];
        int count = 3;
        bool usable = true;
        for (int k = 0; k < 3; ++k) {
            const uint32_t index = indices[t + k];
            if (index >= vertexCount || !std::isfinite(vertices[index].x) || !std::isfinite(vertices[index].y)) { usable = false; break; }
            px[k] = (double(vertices[index].x) - domain.left) / spanX * n;
            py[k] = (domain.up - double(vertices[index].y)) / spanY * n;
        }
        if (!usable) return false;
        ++triangles;
        double minY = (std::min)(py[0], (std::min)(py[1], py[2])), maxY = (std::max)(py[0], (std::max)(py[1], py[2]));
        if (maxY < 0.0 || minY >= double(n)) continue;
        const int rowFirst = int((std::max)(0.0, std::floor(minY))), rowLast = int((std::min)(double(n) - 1.0, std::floor(maxY)));
        for (int row = rowFirst; row <= rowLast; ++row) {
            // the triangle clipped to y in [row, row + 1] (Sutherland-Hodgman against the two lines); its x extent covers the texels it touches in this row
            double ax[8], ay[8], bx[8], by[8];
            int na = 0, nb = 0;
            for (int k = 0; k < count; ++k) { ax[k] = px[k]; ay[k] = py[k]; }
            na = count;
            const auto clip = [](const double* sx, const double* sy, int ns, double* dx, double* dy, double edge, bool keepBelow) {
                int nd = 0;
                for (int k = 0; k < ns; ++k) {
                    const int j = (k + 1) % ns;
                    const bool inK = keepBelow ? sy[k] >= edge : sy[k] <= edge, inJ = keepBelow ? sy[j] >= edge : sy[j] <= edge;
                    if (inK) { dx[nd] = sx[k]; dy[nd] = sy[k]; ++nd; }
                    if (inK != inJ) {
                        const double f = (edge - sy[k]) / (sy[j] - sy[k]);
                        dx[nd] = sx[k] + (sx[j] - sx[k]) * f;
                        dy[nd] = edge;
                        ++nd;
                    }
                }
                return nd;
            };
            nb = clip(ax, ay, na, bx, by, double(row), true);
            if (nb == 0) continue;
            na = clip(bx, by, nb, ax, ay, double(row + 1), false);
            if (na == 0) continue;
            double x0 = ax[0], x1 = ax[0];
            for (int k = 1; k < na; ++k) { x0 = (std::min)(x0, ax[k]); x1 = (std::max)(x1, ax[k]); }
            if (x1 < 0.0 || x0 >= double(n)) continue;
            const int c0 = int((std::max)(0.0, std::floor(x0))), c1 = int((std::min)(double(n) - 1.0, std::floor(x1)));
            for (int c = c0; c <= c1; ++c) hidden[size_t(row) * n + c] = 1;
        }
    }
    // the distance on the tangent plane from each texel centre to the nearest hidden texel centre (two-pass chamfer, 3 x 3)
    const double cx = spanX / n, cy = spanY / n, cd = std::sqrt(cx * cx + cy * cy);
    const double inf = 1e30;
    std::vector<double> dist(size_t(n) * n, inf);
    uint32_t hiddenTexels = 0;
    for (size_t i = 0; i < dist.size(); ++i)
        if (hidden[i]) { dist[i] = 0.0; ++hiddenTexels; }
    if (hiddenTexels) {
        const auto at = [&](int i, int j) -> double& { return dist[size_t(j) * n + i]; };
        for (int j = 0; j < int(n); ++j)
            for (int i = 0; i < int(n); ++i) {
                double& d = at(i, j);
                if (i > 0) d = (std::min)(d, at(i - 1, j) + cx);
                if (j > 0) {
                    d = (std::min)(d, at(i, j - 1) + cy);
                    if (i > 0) d = (std::min)(d, at(i - 1, j - 1) + cd);
                    if (i + 1 < int(n)) d = (std::min)(d, at(i + 1, j - 1) + cd);
                }
            }
        for (int j = int(n) - 1; j >= 0; --j)
            for (int i = int(n) - 1; i >= 0; --i) {
                double& d = at(i, j);
                if (i + 1 < int(n)) d = (std::min)(d, at(i + 1, j) + cx);
                if (j + 1 < int(n)) {
                    d = (std::min)(d, at(i, j + 1) + cy);
                    if (i + 1 < int(n)) d = (std::min)(d, at(i + 1, j + 1) + cd);
                    if (i > 0) d = (std::min)(d, at(i - 1, j + 1) + cd);
                }
            }
    }
    const double pixel = (std::max)(spanX / width, spanY / height);
    const double zero = 2.0 * cd + 2.0 * pixel;
    const double bandRad = kBandDeg * kPi / 180.0;
    out.texels.assign(size_t(n) * n, 255);
    for (uint32_t j = 0; j < n; ++j)
        for (uint32_t i = 0; i < n; ++i) {
            const double d = dist[size_t(j) * n + i];
            if (d >= inf) continue;
            const double tx = domain.left + (i + 0.5) * cx, ty = domain.up - (j + 0.5) * cy;
            const double bandTan = bandRad * (1.0 + tx * tx + ty * ty);   // the radial scale: kBandDeg or a little more in angle
            const double f = (std::max)(0.0, (std::min)(1.0, (d - zero) / bandTan));
            out.texels[size_t(j) * n + i] = uint8_t(std::lround(f * 255.0));
        }
    out.size = n;
    out.domain = domain;
    out.triangles = triangles;
    out.hiddenTexels = hiddenTexels;
    return true;
}

// The per-press coverage: the mean share the left eye takes from the right at full weight, and the part of it taken wholly, on a kCoverageGrid square over the left
// eye's placement (left0..1 as fractions of the left swapchain image: u0, v0, u1, v1). mask may be null (no hidden area).
struct Coverage { double mean = 0.0, whole = 0.0; };
inline Coverage coverageOf(const Mapping& m, const double leftPlace[4], const HiddenMask* mask) {
    Coverage c;
    if (!m.valid) return c;
    const uint32_t g = kCoverageGrid;
    double sum = 0.0;
    uint32_t whole = 0;
    for (uint32_t j = 0; j < g; ++j)
        for (uint32_t i = 0; i < g; ++i) {
            const double u = leftPlace[0] + (leftPlace[2] - leftPlace[0]) * (i + 0.5) / g, v = leftPlace[1] + (leftPlace[3] - leftPlace[1]) * (j + 0.5) / g;
            const Lookup k = lookup(m, u, v);
            if (!k.ahead) continue;
            double a = k.fov;
            if (a > 0.0 && mask && mask->size) a *= mask->sample(k.su, k.sv);
            sum += a;
            if (a >= 0.999) ++whole;
        }
    c.mean = sum / double(g * g);
    c.whole = double(whole) / double(g * g);
    return c;
}

}  // namespace edvr::openxr::shareview
