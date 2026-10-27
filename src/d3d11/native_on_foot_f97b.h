// F97b (native on-foot VR): the interaction label sized like a panel in the world. Journal: the arc doc, F97b.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// The game sizes a label's record by its depth along the game camera's forward axis, not by its distance: in the F95 flight (log 152150) the record scale over
// the record's HUD z read 0.3820-0.3835 at 9 to 77 deg off the axis, while over its distance it fell from 0.378 to 0.084. With the label drawn in the camera's
// plane, that gives it one size in NDC wherever it sits in the view (key 3's layout offset +74.7 px at 8.9 deg, +73.1 at 23.7, +73.2 at 41.3), which is the flat
// game's design: a fixed size on the screen. In the headset a fixed NDC size is not a fixed angle: the projection's NDC per radian grows as sec^2 of the angle
// off the eye's axis along it and as sec across it, so a label the per-eye marker fix holds on its anchor shrinks to cos^2 (width) and cos (height) of its size at
// the view's centre as a head turn carries it off the axis: 0.75 and 0.87 at 30 deg, 0.57 and 0.75 at 41 deg. A panel fixed in the world keeps its angle through
// any head turn. The plain markers' fix (F62c/F64/F66f) changes no size beyond the marker projection's constant scale sigma, so the dots and waypoints keep the
// game's fixed NDC size too (small icons).
//
// The label is given the size of a panel at its anchor facing the eye, of the angular size the game designs at the centre of its own view: a layout vector of
// UI NDC dn stands for the angle Jm^-1 dn / z_game (Jm the marker rows' Jacobian along the HUD frame's x and y at the record, per metre; z_game the record's
// HUD z, by which the game scaled it), and is shown as Je (that angle), Je the scene rows' Jacobian per radian along the panel's right and up axes (perpendicular
// to the eye's line to the anchor). G = Je Jm^-1 / z_game maps the game's layout to the panel's; at the view's centre with the eye at the game's camera it is
// 1/sigma (the game's own unpatched size). The row patch applies G about the anchor (affineRowsG), so the anchor stays exactly on its dot.
#pragma once

#include "draw_record_tag.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f97b {

struct M2 {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0;   // [a b; c d]
};
inline double det(const M2& m) noexcept { return m.a * m.d - m.b * m.c; }
inline bool inverse(const M2& m, M2& out) noexcept {
    const double D = det(m);
    if (!(std::fabs(D) > 1e-18) || !std::isfinite(D)) return false;
    out.a = m.d / D;
    out.b = -m.b / D;
    out.c = -m.c / D;
    out.d = m.a / D;
    return true;
}
inline M2 mul(const M2& x, const M2& y) noexcept {
    M2 r;
    r.a = x.a * y.a + x.b * y.c;
    r.b = x.a * y.b + x.b * y.d;
    r.c = x.c * y.a + x.d * y.c;
    r.d = x.c * y.b + x.d * y.d;
    return r;
}
inline void apply(const M2& m, const double v[2], double out[2]) noexcept {
    const double x = m.a * v[0] + m.b * v[1], y = m.c * v[0] + m.d * v[1];
    out[0] = x;
    out[1] = y;
}

// The marker rows' NDC Jacobian along the HUD frame's x and y, per metre, at the HUD point p (relative to o_hud): columns x and y.
inline bool markerJacobianHud(const float r4[4], const float r5[4], const float r7[4], const double p[3], M2& J) noexcept {
    const double q[4] = {p[0], p[1], p[2], 1.0};
    double X = 0.0, Y = 0.0, Wm = 0.0;
    for (int i = 0; i < 4; ++i) {
        X += r4[i] * q[i];
        Y += r5[i] * q[i];
        Wm += r7[i] * q[i];
    }
    if (!(Wm > 1e-9)) return false;
    const double w2 = Wm * Wm;
    J.a = (r4[0] * Wm - X * r7[0]) / w2;
    J.b = (r4[1] * Wm - X * r7[1]) / w2;
    J.c = (r5[0] * Wm - Y * r7[0]) / w2;
    J.d = (r5[1] * Wm - Y * r7[1]) / w2;
    return std::isfinite(J.a) && std::isfinite(J.b) && std::isfinite(J.c) && std::isfinite(J.d);
}

// The scene rows (b1 270..273, row-vector form) at the world point q (relative to o_w): the eye (where clip x, y and w vanish), the angle of q off the eye's
// axis, its distance from the eye, and the NDC Jacobian per radian along the right and up axes of a panel at q facing the eye (the eye's own x and y axes made
// perpendicular to the line of sight).
struct SceneAt {
    bool ok = false;
    M2 J;
    double angleDeg = 0.0, dist = 0.0;
};
inline double dot3(const double* a, const double* b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double det3(const double A[3][3]) noexcept {
    return A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
}
// The eye of the scene rows (relative to o_w): where clip x, y and w vanish, [cx; cy; cw] e = -t.
inline bool eyeOfRows(const float rows[16], double e[3]) noexcept {
    const double A[3][3] = {{rows[0], rows[4], rows[8]}, {rows[1], rows[5], rows[9]}, {rows[3], rows[7], rows[11]}};
    const double t[3] = {rows[12], rows[13], rows[15]};
    const double D = det3(A);
    if (!(std::fabs(D) > 1e-18) || !std::isfinite(D)) return false;
    for (int k = 0; k < 3; ++k) {
        double M[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M[r][c] = (c == k) ? -t[r] : A[r][c];
        e[k] = det3(M) / D;
    }
    return std::isfinite(e[0]) && std::isfinite(e[1]) && std::isfinite(e[2]);
}
// The direction from the eye of the scene rows' NDC n (n as the rows give it, jitter included): the line where cx.v = n_x cw.v and cy.v = n_y cw.v, in front.
inline bool dirOfNdc(const float rows[16], const double n[2], double dir[3]) noexcept {
    const double cx[3] = {rows[0], rows[4], rows[8]}, cy[3] = {rows[1], rows[5], rows[9]}, cw[3] = {rows[3], rows[7], rows[11]};
    const double a[3] = {cx[0] - n[0] * cw[0], cx[1] - n[0] * cw[1], cx[2] - n[0] * cw[2]}, b[3] = {cy[0] - n[1] * cw[0], cy[1] - n[1] * cw[1], cy[2] - n[1] * cw[2]};
    double v[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double l = std::sqrt(dot3(v, v));
    if (!(l > 1e-18) || !std::isfinite(l)) return false;
    const double sgn = dot3(cw, v) < 0.0 ? -1.0 : 1.0;
    for (int k = 0; k < 3; ++k) dir[k] = sgn * v[k] / l;
    return true;
}
// The right and up axes of a panel facing the eye along u: the eye's own x and y axes (the projection's decomposition) made perpendicular to u.
inline bool panelAxes(const float rows[16], const double u[3], double R[3], double U[3]) noexcept {
    const double cx[3] = {rows[0], rows[4], rows[8]}, cy[3] = {rows[1], rows[5], rows[9]}, cw[3] = {rows[3], rows[7], rows[11]};
    const double len = std::sqrt(dot3(cw, cw));
    if (!(len > 1e-12)) return false;
    const double f[3] = {cw[0] / len, cw[1] / len, cw[2] / len};
    const double ox = dot3(cx, f) / len, oy = dot3(cy, f) / len;
    double ex[3], ey[3];
    for (int k = 0; k < 3; ++k) {
        ex[k] = cx[k] / len - ox * f[k];
        ey[k] = cy[k] / len - oy * f[k];
    }
    const double a = dot3(ex, u);
    for (int k = 0; k < 3; ++k) R[k] = ex[k] - a * u[k];
    const double n = std::sqrt(dot3(R, R));
    if (!(n > 1e-9)) return false;
    for (int k = 0; k < 3; ++k) R[k] /= n;
    const double b = dot3(ey, u), c = dot3(ey, R);
    for (int k = 0; k < 3; ++k) U[k] = ey[k] - b * u[k] - c * R[k];
    const double m = std::sqrt(dot3(U, U));
    if (!(m > 1e-9)) return false;
    for (int k = 0; k < 3; ++k) U[k] /= m;
    return true;
}
// The scene rows' NDC (as the rows give it, jitter included) of the point q relative to o_w; false behind the eye.
inline bool sceneNdc(const float rows[16], const double q[3], double n[2]) noexcept {
    const double X = rows[0] * q[0] + rows[4] * q[1] + rows[8] * q[2] + rows[12], Y = rows[1] * q[0] + rows[5] * q[1] + rows[9] * q[2] + rows[13],
                 Wc = rows[3] * q[0] + rows[7] * q[1] + rows[11] * q[2] + rows[15];
    if (!(Wc > 1e-9)) return false;
    n[0] = X / Wc;
    n[1] = Y / Wc;
    return std::isfinite(n[0]) && std::isfinite(n[1]);
}
// The panel's Jacobian (per radian) at the point eye + d dir, relative to o_w.
inline SceneAt panelAt(const float rows[16], const double eye[3], const double dir[3], double d) noexcept;
inline SceneAt sceneJacobianPanel(const float rows[16], const double q[3]) noexcept {
    double e[3];
    if (!eyeOfRows(rows, e)) return SceneAt{};
    const double v[3] = {q[0] - e[0], q[1] - e[1], q[2] - e[2]};
    const double d = std::sqrt(dot3(v, v));
    if (!(d > 1e-6)) return SceneAt{};
    const double u[3] = {v[0] / d, v[1] / d, v[2] / d};
    return panelAt(rows, e, u, d);
}
inline SceneAt panelAt(const float rows[16], const double eye[3], const double u[3], double d) noexcept {
    SceneAt s;
    const double cx[3] = {rows[0], rows[4], rows[8]}, cy[3] = {rows[1], rows[5], rows[9]}, cw[3] = {rows[3], rows[7], rows[11]};
    const double t[3] = {rows[12], rows[13], rows[15]};
    const double len = std::sqrt(dot3(cw, cw));
    if (!(d > 1e-6) || !(len > 1e-12)) return s;
    const double q[3] = {eye[0] + d * u[0], eye[1] + d * u[1], eye[2] + d * u[2]};
    const double f[3] = {cw[0] / len, cw[1] / len, cw[2] / len};
    auto dot = [](const double* a, const double* b) { return dot3(a, b); };
    if (!(dot(u, f) > 0.05)) return s;   // behind or at the edge of the eye's view
    double R[3], U[3];
    if (!panelAxes(rows, u, R, U)) return s;
    const double X = dot(cx, q) + t[0], Y = dot(cy, q) + t[1], Wc = dot(cw, q) + t[2];
    if (!(Wc > 1e-9)) return s;
    const double w2 = Wc * Wc;
    s.J.a = d * (dot(cx, R) * Wc - X * dot(cw, R)) / w2;
    s.J.b = d * (dot(cx, U) * Wc - X * dot(cw, U)) / w2;
    s.J.c = d * (dot(cy, R) * Wc - Y * dot(cw, R)) / w2;
    s.J.d = d * (dot(cy, U) * Wc - Y * dot(cw, U)) / w2;
    s.angleDeg = std::acos(std::min(1.0, std::max(-1.0, dot(u, f)))) * 180.0 / 3.14159265358979323846;
    s.dist = d;
    s.ok = std::isfinite(s.J.a) && std::isfinite(s.J.b) && std::isfinite(s.J.c) && std::isfinite(s.J.d);
    return s;
}

// G = Je Jm^-1 / zGame; false (G left the identity) when an input is unusable or G would more than double or halve the label (a projection that is not this one).
constexpr double kGMin = 0.5, kGMax = 2.5;
inline bool panelMap(const M2& Je, const M2& Jm, double zGame, M2& G) noexcept {
    G = M2{};
    M2 Ji;
    if (!(zGame > 0.05) || !inverse(Jm, Ji)) return false;
    M2 g = mul(Je, Ji);
    g.a /= zGame;
    g.b /= zGame;
    g.c /= zGame;
    g.d /= zGame;
    const double D = det(g);
    if (!std::isfinite(D) || !(D > kGMin * kGMin) || !(D < kGMax * kGMax) || std::fabs(g.b) > kGMax || std::fabs(g.c) > kGMax || !(g.a > kGMin) || !(g.d > kGMin) ||
        g.a > kGMax || g.d > kGMax)
        return false;
    G = g;
    return true;
}

// The row patch with G about the anchor: shown (texture NDC) = T + G (n_M - nM0) for every vertex, nM0 the anchor through the unpatched rows.
// r4' = sigma_x (G.a r4 + G.b r5) + u_x r7, r5' = sigma_y (G.c r4 + G.d r5) + u_y r7, u = offM + sigma (T - axis - G nM0). G the identity gives
// drawtag::markerAffineU and affineOffsetRows exactly.
inline void affineRowsG(float r4[4], float r5[4], const float r7[4], const drawtag::MarkerAffine& a, const M2& G, const double T[2], const double nM0[2]) noexcept {
    double gn[2];
    apply(G, nM0, gn);
    const double u[2] = {a.offM[0] + a.sigma[0] * (T[0] - a.axis[0] - gn[0]), a.offM[1] + a.sigma[1] * (T[1] - a.axis[1] - gn[1])};
    float o4[4], o5[4];
    for (int i = 0; i < 4; ++i) {
        o4[i] = static_cast<float>(a.sigma[0] * (G.a * r4[i] + G.b * r5[i]) + u[0] * r7[i]);
        o5[i] = static_cast<float>(a.sigma[1] * (G.c * r4[i] + G.d * r5[i]) + u[1] * r7[i]);
    }
    for (int i = 0; i < 4; ++i) {
        r4[i] = o4[i];
        r5[i] = o5[i];
    }
}

// The label as a panel at its anchor facing the eye. The record (the label's own point, around which its mesh is drawn) goes where the panel puts it: from the
// anchor's direction, the game's designed angles D / z_game (D = X0 - v in the HUD frame, the layout offset) along the panel's right and up axes. Its layout is
// mapped by G taken there (Jm at the record, Je at the record's direction), about the record. why: 0 sized, 1 no projection (an eye, a direction or a Jacobian
// unusable), 2 out of range (G would more than halve or over double the label, or the record would move 0.4 NDC or more from the game's layout).
struct LabelPanel {
    int why = 1;
    M2 G;
    SceneAt at, atGame;    // the panel's Jacobian at the record's place, and at the place the game's layout puts it (for the measure of the game's size)
    double Trec[2] = {};   // the record's place in the texture (unjittered)
};
inline LabelPanel labelPanel(const float sceneRows[16], const float r4[4], const float r5[4], const float r7[4], const drawtag::MarkerAffine& aff, const double pRec[3],
                             double zGame, const double D[3], const double anchor[3], const double layoutAt[2]) noexcept {
    LabelPanel out;
    double eye[3];
    M2 Jm;
    if (!(zGame > 0.05) || !eyeOfRows(sceneRows, eye) || !markerJacobianHud(r4, r5, r7, pRec, Jm)) return out;
    const double va[3] = {anchor[0] - eye[0], anchor[1] - eye[1], anchor[2] - eye[2]};
    const double d = std::sqrt(dot3(va, va));
    if (!(d > 1e-6)) return out;
    const double ua[3] = {va[0] / d, va[1] / d, va[2] / d};
    double Ra[3], Ua[3];
    if (!panelAxes(sceneRows, ua, Ra, Ua)) return out;
    const double ta = std::tan(D[0] / zGame), tb = std::tan(D[1] / zGame);
    double ur[3] = {ua[0] + ta * Ra[0] + tb * Ua[0], ua[1] + ta * Ra[1] + tb * Ua[1], ua[2] + ta * Ra[2] + tb * Ua[2]};
    const double l = std::sqrt(dot3(ur, ur));
    if (!(l > 1e-9) || !std::isfinite(l)) return out;
    for (int k = 0; k < 3; ++k) ur[k] /= l;
    const double qr[3] = {eye[0] + d * ur[0], eye[1] + d * ur[1], eye[2] + d * ur[2]};
    double nr[2];
    if (!sceneNdc(sceneRows, qr, nr)) return out;
    out.Trec[0] = nr[0] - aff.jitter[0];
    out.Trec[1] = nr[1] - aff.jitter[1];
    out.at = panelAt(sceneRows, eye, ur, d);
    if (!out.at.ok) return out;
    {
        const double ng[2] = {layoutAt[0] + aff.jitter[0], layoutAt[1] + aff.jitter[1]};
        double ug[3];
        if (dirOfNdc(sceneRows, ng, ug)) out.atGame = panelAt(sceneRows, eye, ug, d);
    }
    M2 G;
    if (!panelMap(out.at.J, Jm, zGame, G) || !(std::fabs(out.Trec[0] - layoutAt[0]) < drawtag::kPatchGateNdc && std::fabs(out.Trec[1] - layoutAt[1]) < drawtag::kPatchGateNdc)) {
        out.why = 2;
        return out;
    }
    out.G = G;
    out.why = 0;
    return out;
}

// The size of a layout drawn through rows4/5/7 (shown through the affine's display map) against the world panel's: a 1% step along the HUD's x and y at the record
// p (relative to o_hud), shown, turned into angles by Je^-1, over the angle the game designs for it (the step over zGame). 1 = the panel's size; the game's own
// rows give cos^2 and cos of the angle off the eye's axis.
inline bool sizeFactor(const float r4[4], const float r5[4], const float r7[4], const drawtag::MarkerAffine& a, const M2& Je, const double p[3], double zGame,
                       double* width, double* height) noexcept {
    M2 Jei;
    if (!(zGame > 0.05) || !inverse(Je, Jei)) return false;
    const double eps = 0.01 * zGame;
    auto shown = [&](const double q[3], double out[2]) {
        const double o[3] = {0.0, 0.0, 0.0};
        double mc[3];
        drawtag::markerClip(r4, r5, r7, q, o, mc);
        if (!(mc[2] > 1e-9)) return false;
        const double n[2] = {mc[0] / mc[2], mc[1] / mc[2]};
        drawtag::displayOfUi(a, n, out);
        return true;
    };
    const double px[3] = {p[0] + eps, p[1], p[2]}, py[3] = {p[0], p[1] + eps, p[2]};
    double s0[2], sx[2], sy[2];
    if (!shown(p, s0) || !shown(px, sx) || !shown(py, sy)) return false;
    const double dx[2] = {sx[0] - s0[0], sx[1] - s0[1]}, dy[2] = {sy[0] - s0[0], sy[1] - s0[1]};
    double ax[2], ay[2];
    apply(Jei, dx, ax);
    apply(Jei, dy, ay);
    const double design = eps / zGame;
    *width = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1]) / design;
    *height = std::sqrt(ay[0] * ay[0] + ay[1] * ay[1]) / design;
    return std::isfinite(*width) && std::isfinite(*height);
}

// The second's figures of one eye, by the anchor's angle off the eye's axis: 0-10, 10-20, 20-30, 30 deg and over.
constexpr int kBins = 4;
inline int binOf(double angleDeg) noexcept { return angleDeg < 10.0 ? 0 : angleDeg < 20.0 ? 1 : angleDeg < 30.0 ? 2 : 3; }
struct Sec {
    uint64_t sized = 0, refusedProj = 0, refusedRange = 0, n[kBins] = {};
    double gameW[kBins] = {}, gameH[kBins] = {}, drawnW[kBins] = {}, drawnH[kBins] = {}, depthOverDist[kBins] = {}, maxAngle = 0.0;
    void add(double angleDeg, double gw, double gh, double dw, double dh, double zOverD) noexcept {
        const int b = binOf(angleDeg);
        ++n[b];
        gameW[b] += gw;
        gameH[b] += gh;
        drawnW[b] += dw;
        drawnH[b] += dh;
        depthOverDist[b] += zOverD;
        maxAngle = std::max(maxAngle, angleDeg);
    }
    double mean(const double* v, int b) const noexcept { return n[b] ? v[b] / static_cast<double>(n[b]) : 0.0; }
};

}  // namespace f97b
}  // namespace nofoot
}  // namespace edvr
