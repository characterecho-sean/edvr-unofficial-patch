#pragma once
// A TINY RAY CASTER FOR THE JUNCTION TEST'S SCENES (flat_history_junction_tests.h; the rule is flat_history_depth_model.h's).
//
// The junction test carries a footprint texel from last frame into this one with the camera rows, so its scenes need real geometry and real rows: a
// symmetric perspective camera with reversed-Z depth (d = near / z, near 0.025, no far plane) whose b1 rows are laid out as the shader reads them
// (histdepth::Cam), and surfaces that two cameras see consistently. A scene is a list of axis-aligned rectangles (a floor, a wall, a hull); a camera pose
// renders it into a depth image the way the game's raster does: a texel (x, y) rendered at phase (jx, jy) holds the surface along the ray through the
// unjittered position (x + .5 - jx, y + .5 - jy), the same convention flat_history_depth_model.h's rig16 uses.
#include <algorithm>
#include <cmath>
#include <vector>
#include "flat_history_depth_model.h"

namespace histscene {
using histdepth::Cam;
using histdepth::Image;

constexpr float kNear = 0.025f;
constexpr float kEye = 1.7f;                  // the eye above the floor
constexpr int kW = 960, kH = 600;
constexpr float kTanX = 1.0f, kTanY = 1.0f * float(kH) / float(kW);   // 90 degrees across, square pixels

enum Surface : int { kSurfSky = -1, kSurfFloor = 0, kSurfWall = 1, kSurfHull = 2 };

// A rectangle perpendicular to `axis` at coordinate `value`, bounded on the other two axes by lo and hi.
struct Prim {
    int axis;
    float value;
    float lo[3], hi[3];
    int id;
};
struct World { std::vector<Prim> prims; };

struct Pose {
    float o[3] = {0, 0, 0};
    float yaw = 0, pitch = 0;
};
struct Basis { float right[3], up[3], fwd[3]; };

inline Basis basisOf(const Pose& p) {
    const float sy = std::sin(p.yaw), cy = std::cos(p.yaw), sp = std::sin(p.pitch), cp = std::cos(p.pitch);
    Basis b;
    b.fwd[0] = sy * cp; b.fwd[1] = sp; b.fwd[2] = cy * cp;
    b.right[0] = cy; b.right[1] = 0; b.right[2] = -sy;
    // up = fwd x right
    b.up[0] = b.fwd[1] * b.right[2] - b.fwd[2] * b.right[1];
    b.up[1] = b.fwd[2] * b.right[0] - b.fwd[0] * b.right[2];
    b.up[2] = b.fwd[0] * b.right[1] - b.fwd[1] * b.right[0];
    return b;
}
// The rows the game's b1 holds for a pose: row i < 3 is the clip image of the camera-relative world axis i, x = right / tanX, y = up / tanY, 3 = the view
// direction's component; row 3 the near term; row 4 the view direction; row 5 the camera origin. ndcX, ndcY: the shift a jittered frustum puts in the rows.
inline Cam camOf(const Pose& p, float ndcX = 0, float ndcY = 0) {
    const Basis b = basisOf(p);
    Cam c;
    for (int i = 0; i < 3; ++i) {
        c.rows[i][0] = b.right[i] / kTanX;
        c.rows[i][1] = b.up[i] / kTanY;
        c.rows[i][2] = 0;
        c.rows[i][3] = b.fwd[i];
        c.rows[4][i] = b.fwd[i];
        c.rows[5][i] = p.o[i];
    }
    c.rows[3][2] = kNear;
    c.ndcX = ndcX;
    c.ndcY = ndcY;
    if (ndcX != 0 || ndcY != 0)
        for (int i = 0; i < 3; ++i) {   // the injector's jitter: x += ndc.x * w, y += ndc.y * w
            c.rows[i][0] += ndcX * c.rows[i][3];
            c.rows[i][1] += ndcY * c.rows[i][3];
        }
    return c;
}

struct Hit { float t = 0; int id = kSurfSky; };
// The nearest surface along the ray through the NDC position (nx, ny): t is the view depth (the ray's direction has a unit component along the view).
inline Hit cast(const World& w, const Pose& p, float nx, float ny) {
    const Basis b = basisOf(p);
    float dir[3];
    for (int i = 0; i < 3; ++i) dir[i] = b.fwd[i] + nx * kTanX * b.right[i] + ny * kTanY * b.up[i];
    Hit best;
    best.t = 1e30f;
    for (const Prim& q : w.prims) {
        if (dir[q.axis] == 0.0f) continue;
        const float t = (q.value - p.o[q.axis]) / dir[q.axis];
        if (!(t > 0.01f) || t >= best.t) continue;
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            if (a == q.axis) continue;
            const float c = p.o[a] + dir[a] * t;
            inside = inside && c >= q.lo[a] && c < q.hi[a];
        }
        if (inside) { best.t = t; best.id = q.id; }
    }
    if (best.id == kSurfSky) best.t = 0;
    return best;
}
// The depth image value of a hit, and the NDC of a unit position (u, v) in [0,1] (v down).
inline float depthOf(const Hit& h) { return h.id == kSurfSky ? 0.0f : kNear / h.t; }
inline Hit castUv(const World& w, const Pose& p, float u, float v) { return cast(w, p, u * 2.0f - 1.0f, 1.0f - v * 2.0f); }

// Renders texels [x0, x1) x [y0, y1) of `w` seen from `p` at raster phase (jx, jy) into img, and what each one shows into ids (row-major over the whole image, if given).
inline void render(Image& img, std::vector<int>* ids, const World& w, const Pose& p, float jx, float jy, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, img.w); y1 = std::min(y1, img.h);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const Hit h = castUv(w, p, (float(x) + 0.5f - jx) / float(img.w), (float(y) + 0.5f - jy) / float(img.h));
            img.at(x, y) = depthOf(h);
            if (ids) (*ids)[size_t(y) * size_t(img.w) + size_t(x)] = h.id;
        }
}

// ---- the parts the scenes are made of -----------------------------------------------------------------------------------------------------
inline Prim floorPrim() { return {1, -kEye, {-1e3f, 0, 0.01f}, {1e3f, 0, 1e3f}, kSurfFloor}; }
// A wall across the view at distance z, from the floor up to `top` above the eye.
inline Prim wallPrim(float z, float top) { return {2, z, {-1e3f, -kEye, 0}, {1e3f, top, 0}, kSurfWall}; }
// A hull: a rectangle across the view at distance z.
inline Prim hullPrim(float z, float x0, float x1, float y0, float y1) { return {2, z, {x0, y0, 0}, {x1, y1, 0}, kSurfHull}; }

}  // namespace histscene
