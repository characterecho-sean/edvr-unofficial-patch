// F97b checks (native on-foot VR), run97b below: the interaction label sized like a panel at its anchor facing the eye (native_on_foot_f97b.h) through the real
// row patch and display map (draw_record_tag.h). Header only, run by native_frame_test.
//
// The frames, as the game's: the eye somewhere in the world, turned 20 deg; the scene rows (b1, row-vector form) project world points less o_w (o_w the world's
// origin here) through it, with a TAA jitter. The game's camera, whose axes are the HUD frame's, laid the label out: the record X0 (HUD) is the anchor v plus the
// designed offset scale (-off.x, off.y, 0), the scale 0.3821 times X0's HUD z (the F95 flight's indicator lines), and the label's mesh lies in the HUD's x-y plane
// at the record, scaled the same. The marker rows take HUD points less o_hud (the eye in the HUD frame) through the eye turned by the lag against the HUD's axes.
// The F95 flight's key 1 offset and the F94b flight's focal scales (1536 x 1632).
#pragma once

#include "../../src/d3d11/draw_record_tag.h"
#include "../../src/d3d11/native_on_foot_f97b.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace f97btest {

namespace dt = edvr::drawtag;
namespace f97b = edvr::nofoot::f97b;

constexpr double kPi97 = 3.14159265358979323846;
constexpr double kFx = 0.90326, kFy = 0.83612, kCx = -0.12, kCy = 0.04, kK = 0.3821, kOffX = -0.4791, kOffY = -0.3320, kHalfW = 768.0, kHalfH = 816.0;

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
inline V3 v3(double x, double y, double z) {
    V3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}
// world -> camera at yaw deg (xv = c x - s z, zv = s x + c z), and back
inline V3 toCam(double yaw, V3 p) {
    const double s = std::sin(yaw * kPi97 / 180.0), c = std::cos(yaw * kPi97 / 180.0);
    return v3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
}
inline V3 fromCam(double yaw, V3 p) {
    const double s = std::sin(yaw * kPi97 / 180.0), c = std::cos(yaw * kPi97 / 180.0);
    return v3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
}

struct Rig {
    float scene[16] = {}, r4[4] = {}, r5[4] = {}, r7[4] = {};
    dt::MarkerAffine aff;
    V3 eye, gameCam, ohud;
    double yaw = 20.0, lag = 0.0;
    // the record at `theta` deg off the eye's axis (horizontally), 2.4 m away; sigma the marker projection's focal scale over the scene's
    V3 X0, v, P;   // the record and the anchor (HUD), the anchor (world)
    double D[3] = {};
    Rig(double theta, double sigma, double lagDeg, V3 ohudIn, double jx) {
        lag = lagDeg;
        ohud = ohudIn;
        eye = v3(0.4, 1.6, -0.2);
        const double s = std::sin(yaw * kPi97 / 180.0), c = std::cos(yaw * kPi97 / 180.0), cx = kCx + jx;
        scene[0] = static_cast<float>(kFx * c + cx * s);
        scene[4] = 0.0f;
        scene[8] = static_cast<float>(-kFx * s + cx * c);
        scene[1] = static_cast<float>(kCy * s);
        scene[5] = static_cast<float>(kFy);
        scene[9] = static_cast<float>(kCy * c);
        scene[3] = static_cast<float>(s);
        scene[7] = 0.0f;
        scene[11] = static_cast<float>(c);
        scene[10] = 0.5f;
        scene[12] = static_cast<float>(-(scene[0] * eye.x + scene[8] * eye.z));
        scene[13] = static_cast<float>(-(scene[1] * eye.x + scene[5] * eye.y + scene[9] * eye.z));
        scene[14] = 0.01f;
        scene[15] = static_cast<float>(-(scene[3] * eye.x + scene[11] * eye.z));
        // the game's camera: its axes turned `lag` behind the eye's, the eye at ohud in its frame
        const V3 eo = fromCam(yaw - lag, ohud);
        gameCam = v3(eye.x - eo.x, eye.y - eo.y, eye.z - eo.z);
        const double sl = std::sin(lag * kPi97 / 180.0), cl = std::cos(lag * kPi97 / 180.0), fx = kFx * sigma, fy = kFy * sigma;
        r4[0] = static_cast<float>(fx * cl + kCx * sl), r4[1] = 0.0f, r4[2] = static_cast<float>(-fx * sl + kCx * cl), r4[3] = 0.0f;
        r5[0] = static_cast<float>(kCy * sl), r5[1] = static_cast<float>(fy), r5[2] = static_cast<float>(kCy * cl), r5[3] = 0.0f;
        r7[0] = static_cast<float>(sl), r7[1] = 0.0f, r7[2] = static_cast<float>(cl), r7[3] = 0.0f;
        // the record at theta in the eye, then the anchor from the designed offset
        const double th = theta * kPi97 / 180.0;
        const V3 recEye = v3(2.4 * std::sin(th), -0.1, 2.4 * std::cos(th));
        const V3 rw = fromCam(yaw, recEye);
        const V3 recW = v3(eye.x + rw.x, eye.y + rw.y, eye.z + rw.z);
        X0 = toCam(yaw - lag, v3(recW.x - gameCam.x, recW.y - gameCam.y, recW.z - gameCam.z));
        const double scale = kK * X0.z;
        D[0] = -scale * kOffX;
        D[1] = scale * kOffY;
        D[2] = 0.0;
        v = v3(X0.x - D[0], X0.y - D[1], X0.z);
        const V3 vw = fromCam(yaw - lag, v);
        P = v3(gameCam.x + vw.x, gameCam.y + vw.y, gameCam.z + vw.z);
        dt::ProjDecomp dM, dW;
        const double axis[2] = {kCx, kCy};
        if (dt::projDecomposeMarker(r4, r5, r7, dM) && dt::projDecomposeWorld(scene, dW)) aff = dt::markerAffine(dM, dW, axis);
    }
    void hudRel(V3 p, double out[3]) const {
        out[0] = p.x - ohud.x;
        out[1] = p.y - ohud.y;
        out[2] = p.z - ohud.z;
    }
    bool ui(V3 p, double n[2]) const {
        double q[3], mc[3];
        hudRel(p, q);
        const double o[3] = {0.0, 0.0, 0.0};
        dt::markerClip(r4, r5, r7, q, o, mc);
        if (!(mc[2] > 1e-9)) return false;
        n[0] = mc[0] / mc[2];
        n[1] = mc[1] / mc[2];
        return true;
    }
    bool shownWorld(V3 q, double n[2]) const {   // the texture NDC (unjittered) of a world point
        const double p[3] = {q.x, q.y, q.z};
        double s[2];
        if (!f97b::sceneNdc(scene, p, s)) return false;
        n[0] = s[0] - aff.jitter[0];
        n[1] = s[1] - aff.jitter[1];
        return true;
    }
};
inline double angleBetween(const double a[3], const double b[3]) {
    const double c = f97b::dot3(a, b) / std::sqrt(f97b::dot3(a, a) * f97b::dot3(b, b));
    return std::acos(std::min(1.0, std::max(-1.0, c)));
}

// One label decision on a rig, as fixDecide makes it: the panel, the patched rows, the size factors, the record's angle from the anchor (as shown) over the design.
struct Result {
    bool ok = false;
    f97b::LabelPanel lp;
    double gameW = 0.0, gameH = 0.0, drawnW = 0.0, drawnH = 0.0, sepBefore = 0.0, sepAfter = 0.0, anchorPx = 1e9;
};
inline Result decide(const Rig& g) {
    Result r;
    double T[2], nM0[2], nRec[2];
    const double Pq[3] = {g.P.x, g.P.y, g.P.z};
    if (!g.shownWorld(g.P, T) || !g.ui(g.v, nM0) || !g.ui(g.X0, nRec)) return r;
    double pRec[3];
    g.hudRel(g.X0, pRec);
    const double layoutAt[2] = {T[0] + nRec[0] - nM0[0], T[1] + nRec[1] - nM0[1]};
    r.lp = f97b::labelPanel(g.scene, g.r4, g.r5, g.r7, g.aff, pRec, g.X0.z, g.D, Pq, layoutAt);
    if (r.lp.why != 0 || !r.lp.atGame.ok) return r;
    float q4[4], q5[4];
    for (int i = 0; i < 4; ++i) q4[i] = g.r4[i], q5[i] = g.r5[i];
    f97b::affineRowsG(q4, q5, g.r7, g.aff, r.lp.G, r.lp.Trec, nRec);
    if (!f97b::sizeFactor(g.r4, g.r5, g.r7, g.aff, r.lp.atGame.J, pRec, g.X0.z, &r.gameW, &r.gameH) ||
        !f97b::sizeFactor(q4, q5, g.r7, g.aff, r.lp.at.J, pRec, g.X0.z, &r.drawnW, &r.drawnH))
        return r;
    // the record's angle from the anchor as shown, before (F95b's map: T + the layout) and after, over the designed angle
    double e[3];
    f97b::eyeOfRows(g.scene, e);
    auto dirOfShown = [&](const double n[2], double d[3]) {
        const double ns[2] = {n[0] + g.aff.jitter[0], n[1] + g.aff.jitter[1]};
        return f97b::dirOfNdc(g.scene, ns, d);
    };
    double da[3], db[3], dc[3];
    const double ua[3] = {Pq[0] - e[0], Pq[1] - e[1], Pq[2] - e[2]};
    if (!dirOfShown(layoutAt, db) || !dirOfShown(r.lp.Trec, dc)) return r;
    for (int k = 0; k < 3; ++k) da[k] = ua[k];
    const double design = std::sqrt(g.D[0] * g.D[0] + g.D[1] * g.D[1]) / g.X0.z;
    r.sepBefore = angleBetween(da, db) / design;
    r.sepAfter = angleBetween(da, dc) / design;
    // the anchor through the patched rows: where its dot is when the record is the pivot (not a vertex of the label; measured, not required)
    double mcA[3], qa[3];
    g.hudRel(g.v, qa);
    const double o[3] = {0.0, 0.0, 0.0};
    dt::markerClip(q4, q5, g.r7, qa, o, mcA);
    if (mcA[2] > 1e-9) {
        double shown[2];
        const double nA[2] = {mcA[0] / mcA[2], mcA[1] / mcA[2]};
        dt::displayOfUi(g.aff, nA, shown);
        r.anchorPx = std::sqrt(std::pow((shown[0] - T[0]) * kHalfW, 2) + std::pow((shown[1] - T[1]) * kHalfH, 2));
    }
    r.ok = true;
    return r;
}

template <class Check>
inline void run97b(Check&& check) {
    auto approx = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    // ---- the cause: the game's label keeps one NDC size, so in the headset it shrinks as cos^2 (width) and cos (height) of its angle off the eye's axis ------------------
    {
        bool cause = true, fixed = true, placed = true;
        double worstGame = 0.0, worstDrawn = 0.0, worstSep = 0.0;
        for (double theta : {0.0, 15.0, 30.0, 45.0}) {
            const Rig g(theta, 1.0, 0.0, v3(0.0, 0.0, 0.0), 0.0);
            const Result r = decide(g);
            if (!r.ok) {
                cause = fixed = placed = false;
                continue;
            }
            const double a = r.lp.atGame.angleDeg * kPi97 / 180.0;
            cause = cause && approx(r.gameW, std::cos(a) * std::cos(a), 0.01) && approx(r.gameH, std::cos(a), 0.01);
            fixed = fixed && approx(r.drawnW, 1.0, 0.002) && approx(r.drawnH, 1.0, 0.002);
            placed = placed && approx(r.sepAfter, 1.0, 0.02);
            worstGame = std::max(worstGame, std::fabs(r.gameW - std::cos(a) * std::cos(a)));
            worstDrawn = std::max(worstDrawn, std::max(std::fabs(r.drawnW - 1.0), std::fabs(r.drawnH - 1.0)));
            worstSep = std::max(worstSep, std::fabs(r.sepAfter - 1.0));
        }
        const Rig g45(45.0, 1.0, 0.0, v3(0.0, 0.0, 0.0), 0.0);
        const Result r45 = decide(g45);
        check(cause && r45.ok && r45.gameW < 0.55 && r45.sepBefore < 0.7,
              "F97b: the game's label (scale 0.3821 z, drawn in the camera's plane) has the panel's width times cos^2 and height times cos of its angle off the eye's "
              "axis at 0, 15, 30 and 45 deg, its offset from the anchor shrunk alike (under 0.7 of the design at 45 deg)");
        check(fixed && placed, "F97b: drawn through the patched rows the label has the panel's size (width and height within 0.2%) and its record the design's angle from the "
                               "anchor (within 2%) at 0, 15, 30 and 45 deg");
    }
    // ---- the eye away from the game's camera (up 15, forward 9 cm), the marker rows a degree behind, the projection's own scale and a jitter: still the panel ------
    {
        const Rig g(30.0, 1.0094, 1.0, v3(0.03, 0.15, -0.09), 0.0004);
        const Result r = decide(g);
        check(r.ok && r.lp.why == 0 && approx(r.drawnW, 1.0, 0.003) && approx(r.drawnH, 1.0, 0.003) && approx(r.sepAfter, 1.0, 0.02) && r.gameW < 0.8 && r.lp.G.a > 1.3,
              "F97b: with the eye 15 cm over and 9 cm behind the game's camera, the marker rows 1 deg late, sigma 1.0094 and a jitter, the label at 30 deg is the panel's "
              "size and place (the game's width under 0.8, G over 1.3 across)");
    }
    // ---- at the view's centre the label is the game's own size (G 1/sigma, under 1%) --------------------------------------------------------------------------------
    {
        const Rig g(0.0, 1.0094, 0.0, v3(0.0, 0.0, 0.0), 0.0);
        const Result r = decide(g);
        check(r.ok && approx(r.lp.G.a, 1.0 / 1.0094, 0.01) && approx(r.lp.G.d, 1.0 / 1.0094, 0.01) && std::fabs(r.lp.G.b) < 0.01 && std::fabs(r.lp.G.c) < 0.01,
              "F97b: near the view's centre G is the game's own unpatched size (1/sigma within 1%)");
    }
    // ---- the guards: no projection, out of range, the record moved too far; the bins ---------------------------------------------------------------------------------
    {
        f97b::M2 Je, Jm, G;
        Je.a = 1.0;
        Je.d = 1.0;
        Jm.a = 0.4;
        Jm.d = 0.4;
        const bool okMap = f97b::panelMap(Je, Jm, 2.5, G) && approx(G.a, 1.0, 1e-12);
        const bool zero = !f97b::panelMap(Je, Jm, 0.0, G) && G.a == 1.0 && G.b == 0.0;
        f97b::M2 sing;
        sing.a = 1.0;
        sing.b = 2.0;
        sing.c = 2.0;
        sing.d = 4.0;
        const bool singular = !f97b::panelMap(Je, sing, 2.5, G);
        f97b::M2 big = Je;
        big.a = 10.0;
        const bool range = !f97b::panelMap(big, Jm, 2.5, G);
        const float r4[4] = {0.9f, 0.0f, 0.0f, 0.0f}, r5[4] = {0.0f, 0.9f, 0.0f, 0.0f}, r7[4] = {0.0f, 0.0f, 1.0f, 0.0f};
        const double behind[3] = {0.0, 0.0, -1.0};
        const bool noJm = !f97b::markerJacobianHud(r4, r5, r7, behind, Jm);
        float flat[16] = {};
        double e[3];
        const bool noEye = !f97b::eyeOfRows(flat, e);
        const bool bins = f97b::binOf(0.0) == 0 && f97b::binOf(9.99) == 0 && f97b::binOf(10.0) == 1 && f97b::binOf(29.9) == 2 && f97b::binOf(30.0) == 3 && f97b::binOf(80.0) == 3;
        f97b::Sec sec;
        sec.add(31.0, 0.7, 0.85, 1.0, 1.0, 0.86);
        sec.add(35.0, 0.6, 0.8, 1.0, 1.0, 0.82);
        const bool means = sec.n[3] == 2 && approx(sec.mean(sec.gameW, 3), 0.65, 1e-12) && sec.mean(sec.gameW, 0) == 0.0 && approx(sec.maxAngle, 35.0, 1e-12);
        check(okMap && zero && singular && range && noJm && noEye && bins && means,
              "F97b: the map refuses a depth at 0, a singular marker Jacobian and a size change past 2.5x; no Jacobian behind the marker camera, no eye from empty rows; "
              "the angle bins and the means");
    }
    // ---- a record the panel would carry 0.4 NDC or more from the game's layout is refused (the size map, not the move) ---------------------------------------------
    {
        const Rig g(30.0, 1.0, 0.0, v3(0.0, 0.0, 0.0), 0.0);
        double T[2], nM0[2], nRec[2], pRec[3];
        g.shownWorld(g.P, T);
        g.ui(g.v, nM0);
        g.ui(g.X0, nRec);
        g.hudRel(g.X0, pRec);
        const double Pq[3] = {g.P.x, g.P.y, g.P.z};
        const double farAway[2] = {T[0] + 0.9, T[1]};
        const f97b::LabelPanel lp = f97b::labelPanel(g.scene, g.r4, g.r5, g.r7, g.aff, pRec, g.X0.z, g.D, Pq, farAway);
        check(lp.why == 2 && lp.G.a == 1.0 && lp.G.d == 1.0, "F97b: a panel place 0.4 NDC or more from the game's layout is refused as out of range (G left the identity)");
    }
}

}  // namespace f97btest
