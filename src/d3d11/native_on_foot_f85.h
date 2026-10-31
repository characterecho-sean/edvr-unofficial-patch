// F85 (native on-foot VR): the pure parts of the hands-at-speed trace (a), the weapon UI pinned to the weapon (b) and the reticle at the aim
// ray's hit depth (c). Design: reviews\native-onfoot-hands-ui-reticle-review-2026-10-23.md (sections 4.1-4.3); journal: the arc doc, F85.
//
// Header only, no game or D3D11 state, so native_frame_test and the reticle depth rig run the very functions the hook uses.
//
// Conventions. b0 rows 4, 5, 7 of the marker composite ("composite rows" B) and the world rows b1 270..273 of an eye's scene ("scene rows" W) are
// kept as 3x4 matrices in COLUMN form: clip_k = sum_i R[k][i] * v_i + R[k][3], k = 0 (x), 1 (y), 2 (w). The composite's v is X0 - o_hud, the
// scene's is P - o_world. The composite camera K maps the former to the latter (K: v -> P - o_world, 3x3 K3 and translation k4), so
//     B = A . W . K            with A the display affine (sigma, the principal points; lambda the w-row scale),
// and the weapon UI of a camera U (world = U3 t + p_U) drawn right is  A . W . U'  with U' = U with o_hud and o_world folded in. The rows
// that draw it are B' = B . (K^-1 U').
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f85 {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;

// ---- tiny linear algebra (double) ----------------------------------------------------------------------------------------------------------
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double len3(const double a[3]) noexcept { return std::sqrt(dot3(a, a)); }
inline bool inv3(const double a[3][3], double out[3][3]) noexcept {
    const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (!(std::fabs(det) > 1e-14) || !std::isfinite(det)) return false;
    const double id = 1.0 / det;
    out[0][0] = c00 * id;
    out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id;
    out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
    out[1][0] = c01 * id;
    out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id;
    out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
    out[2][0] = c02 * id;
    out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id;
    out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
    return true;
}
inline void mul33(const double a[3][3], const double b[3][3], double out[3][3]) noexcept {
    double t[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    std::memcpy(out, t, sizeof(t));
}
inline void mulVec3(const double a[3][3], const double v[3], double out[3]) noexcept {
    double t[3];
    for (int i = 0; i < 3; ++i) t[i] = a[i][0] * v[0] + a[i][1] * v[1] + a[i][2] * v[2];
    std::memcpy(out, t, sizeof(t));
}
// The angle in degrees of a rotation matrix (0 for the identity), from its trace.
inline double rotationAngleDeg(const double m[3][3]) noexcept {
    double c = (m[0][0] + m[1][1] + m[2][2] - 1.0) * 0.5;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * kDeg;
}
// max |K^T K - I|: how far a 3x3 is from a rotation (0 for one).
inline double rigidError(const double m[3][3]) noexcept {
    double e = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += m[k][i] * m[k][j];
            e = std::max(e, std::fabs(s - (i == j ? 1.0 : 0.0)));
        }
    return e;
}

// ---- rows ----------------------------------------------------------------------------------------------------------------------------------
struct Rows {
    double r[3][4] = {};
};
// The marker composite's b0 rows 4, 5, 7 (x, y, w).
inline Rows rowsOfComposite(const float r4[4], const float r5[4], const float r7[4]) noexcept {
    Rows o;
    for (int i = 0; i < 4; ++i) {
        o.r[0][i] = r4[i];
        o.r[1][i] = r5[i];
        o.r[2][i] = r7[i];
    }
    return o;
}
// The scene rows b1 270..273 in row-vector form (clip_j = v0 rows[j] + v1 rows[4 + j] + v2 rows[8 + j] + rows[12 + j]): x, y, w are the columns 0, 1, 3.
inline Rows rowsOfWorld(const float rows[16]) noexcept {
    static const int kWhich[3] = {0, 1, 3};
    Rows o;
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 3; ++i) o.r[k][i] = rows[4 * i + kWhich[k]];
        o.r[k][3] = rows[12 + kWhich[k]];
    }
    return o;
}
inline bool project(const Rows& R, const double v[3], double ndc[2], double* w) noexcept {
    double c[3];
    for (int k = 0; k < 3; ++k) c[k] = R.r[k][0] * v[0] + R.r[k][1] * v[1] + R.r[k][2] * v[2] + R.r[k][3];
    if (w) *w = c[2];
    if (!(c[2] > 1e-12)) return false;
    ndc[0] = c[0] / c[2];
    ndc[1] = c[1] / c[2];
    return true;
}

// ---- the display affine (A) ----------------------------------------------------------------------------------------------------------------
// From drawtag::MarkerAffine's sigma and the two projections' principal points: a world point at scene NDC n_W is drawn by the composite at
// n_M = offM + sigma (n_W - offW). In homogeneous form x_M = lambda (sigma_x x_W + ox w_W), y_M likewise, w_M = lambda w_W, where lambda is the
// ratio of the two w rows' lengths. A is that map on (x, y, w).
struct AffParams {
    double sigma[2] = {1.0, 1.0}, offM[2] = {}, offW[2] = {};
};

// ---- the composite camera K ----------------------------------------------------------------------------------------------------------------
struct CamK {
    bool ok = false;
    double K3[3][3] = {}, k4[3] = {};   // v (= X0 - o_hud) -> P - o_world:  P - o_world = K3 v + k4
    double lambda = 1.0, rigid = 1.0;   // the w rows' scale ratio; max |K3^T K3 - I|
};
inline CamK solveK(const Rows& B, const Rows& W, const AffParams& a) noexcept {
    CamK k;
    const double bw[3] = {B.r[2][0], B.r[2][1], B.r[2][2]}, ww[3] = {W.r[2][0], W.r[2][1], W.r[2][2]};
    const double lb = len3(bw), lw = len3(ww);
    if (!(lb > 1e-12) || !(lw > 1e-12)) return k;
    const double lam = lb / lw;
    if (!(a.sigma[0] > 1e-6) || !(a.sigma[1] > 1e-6)) return k;
    const double ox = a.offM[0] - a.sigma[0] * a.offW[0], oy = a.offM[1] - a.sigma[1] * a.offW[1];
    // Bp = A^-1 B (x: (x_M - ox w_M) / (lam sigma_x), y alike, w: w_M / lam).
    double Bp[3][4];
    for (int c = 0; c < 4; ++c) {
        Bp[0][c] = (B.r[0][c] - ox * B.r[2][c]) / (lam * a.sigma[0]);
        Bp[1][c] = (B.r[1][c] - oy * B.r[2][c]) / (lam * a.sigma[1]);
        Bp[2][c] = B.r[2][c] / lam;
    }
    double W3[3][3], W3i[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) W3[r][c] = W.r[r][c];
    if (!inv3(W3, W3i)) return k;
    double Bl[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) Bl[r][c] = Bp[r][c];
    mul33(W3i, Bl, k.K3);
    const double t[3] = {Bp[0][3] - W.r[0][3], Bp[1][3] - W.r[1][3], Bp[2][3] - W.r[2][3]};
    mulVec3(W3i, t, k.k4);
    k.lambda = lam;
    k.rigid = rigidError(k.K3);
    k.ok = std::isfinite(k.rigid) && std::isfinite(k.k4[0]) && std::isfinite(k.k4[1]) && std::isfinite(k.k4[2]);
    return k;
}
// The composite camera's origin in absolute world (X0 = 0, i.e. v = -o_hud) and its forward (the image of local +z).
inline void cameraOrigin(const CamK& k, const double oHud[3], const double oWorld[3], double out[3]) noexcept {
    double m[3];
    const double v[3] = {-oHud[0], -oHud[1], -oHud[2]};
    mulVec3(k.K3, v, m);
    for (int i = 0; i < 3; ++i) out[i] = m[i] + k.k4[i] + oWorld[i];
}
inline bool cameraForward(const CamK& k, double out[3]) noexcept {
    const double f[3] = {k.K3[0][2], k.K3[1][2], k.K3[2][2]};
    const double l = len3(f);
    if (!(l > 1e-12)) return false;
    for (int i = 0; i < 3; ++i) out[i] = f[i] / l;
    return true;
}
// Two eyes' K estimates of one frame: the origins' distance (m) and the rotation between them (deg).
struct KAgree {
    bool ok = false;
    double distM = 0.0, angleDeg = 0.0;
};
inline KAgree kAgree(const CamK& a, const double oHudA[3], const double oWorldA[3], const CamK& b, const double oHudB[3], const double oWorldB[3]) noexcept {
    KAgree g;
    double Oa[3], Ob[3], ai[3][3], r[3][3];
    cameraOrigin(a, oHudA, oWorldA, Oa);
    cameraOrigin(b, oHudB, oWorldB, Ob);
    if (!inv3(a.K3, ai)) return g;
    mul33(ai, b.K3, r);
    const double d[3] = {Oa[0] - Ob[0], Oa[1] - Ob[1], Oa[2] - Ob[2]};
    g.distM = len3(d);
    g.angleDeg = rotationAngleDeg(r);
    g.ok = std::isfinite(g.distM) && std::isfinite(g.angleDeg);
    return g;
}
constexpr double kAgreeMaxM = 0.001, kAgreeMaxDeg = 0.05;   // the review's refusal bounds: 1 mm and 0.05 degrees
constexpr double kRigidMax = 0.01;                          // a K3 further than this from a rotation is not a camera: the display affine was wrong
inline bool kAgreeWithin(const KAgree& g) noexcept { return g.ok && g.distM <= kAgreeMaxM && g.angleDeg <= kAgreeMaxDeg; }

// ---- the weapon UI's camera U and the correction -------------------------------------------------------------------------------------------
// U is the getter's camera as the game stores it (a row-vector affine, world = t R + p). Folded into the composite's frame:
//     P - o_world = U3 v + u4 with v = X0 - o_hud, where U3 = R^T and u4 = U3 o_hud + p - o_world.
struct UFrame {
    double U3[3][3] = {}, u4[3] = {};
};
inline UFrame uFrame(const float U[16], const double oHud[3], const double oWorld[3]) noexcept {
    UFrame f;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) f.U3[a][b] = U[4 * b + a];
    double m[3];
    mulVec3(f.U3, oHud, m);
    for (int a = 0; a < 3; ++a) f.u4[a] = m[a] + U[12 + a] - oWorld[a];
    return f;
}
struct Corr {
    bool ok = false;
    double M3[3][3] = {}, m4[3] = {};
    double angleDeg = 0.0, yawDeg = 0.0, pitchDeg = 0.0;   // the rotation K -> U, and U's forward in K's frame (yaw about up, pitch up)
};
inline Corr makeCorrection(const CamK& k, const UFrame& u) noexcept {
    Corr c;
    double Ki[3][3];
    if (!k.ok || !inv3(k.K3, Ki)) return c;
    mul33(Ki, u.U3, c.M3);
    const double t[3] = {u.u4[0] - k.k4[0], u.u4[1] - k.k4[1], u.u4[2] - k.k4[2]};
    mulVec3(Ki, t, c.m4);
    c.angleDeg = rotationAngleDeg(c.M3);
    const double f[3] = {c.M3[0][2], c.M3[1][2], c.M3[2][2]};
    const double fl = len3(f);
    if (fl > 1e-12) {
        c.yawDeg = std::atan2(f[0], f[2]) * kDeg;
        c.pitchDeg = std::asin(std::max(-1.0, std::min(1.0, f[1] / fl))) * kDeg;
    }
    c.ok = std::isfinite(c.angleDeg);
    return c;
}
// B' = B . M: the rows that draw the same vertices where the camera U puts them.
inline Rows applyCorrection(const Rows& B, const Corr& c) noexcept {
    Rows o;
    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 3; ++j) o.r[k][j] = B.r[k][0] * c.M3[0][j] + B.r[k][1] * c.M3[1][j] + B.r[k][2] * c.M3[2][j];
        o.r[k][3] = B.r[k][0] * c.m4[0] + B.r[k][1] * c.m4[1] + B.r[k][2] * c.m4[2] + B.r[k][3];
    }
    return o;
}
inline void rowsToFloat(const Rows& R, float r4[4], float r5[4], float r7[4]) noexcept {
    for (int i = 0; i < 4; ++i) {
        r4[i] = static_cast<float>(R.r[0][i]);
        r5[i] = static_cast<float>(R.r[1][i]);
        r7[i] = static_cast<float>(R.r[2][i]);
    }
}
// Where the composite draws a vertex X0 (UI NDC) and where it is SHOWN in the texture, then the scene's unjittered NDC of a world point: the
// proof's three numbers. showOf = axis + (n_M - offM) / sigma; the scene's point is drawn at its NDC less the jitter (offW - axis).
inline bool shownNdc(const AffParams& a, const double axis[2], const Rows& B, const double v[3], double out[2]) noexcept {
    double n[2];
    if (!project(B, v, n, nullptr)) return false;
    for (int k = 0; k < 2; ++k) out[k] = axis[k] + (n[k] - a.offM[k]) / a.sigma[k];
    return true;
}
inline bool targetNdc(const AffParams& a, const double axis[2], const Rows& W, const double pRel[3], double out[2]) noexcept {
    double n[2];
    if (!project(W, pRel, n, nullptr)) return false;
    for (int k = 0; k < 2; ++k) out[k] = n[k] - (a.offW[k] - axis[k]);
    return true;
}

// ---- the weapon UI's tag keys and gate -----------------------------------------------------------------------------------------------------
constexpr uint32_t kWeaponKeyBase = 0x70u, kWeaponKeySlots = 8u;
inline uint32_t weaponKey(uint32_t slot) noexcept { return kWeaponKeyBase + (slot % kWeaponKeySlots); }
// A tag key (second posts carry bit 7) that names a weapon UI part, and its slot.
inline bool isWeaponKey(uint32_t key, uint32_t* slot = nullptr) noexcept {
    const uint32_t k = key & 0x7Fu;
    if (k < kWeaponKeyBase || k >= kWeaponKeyBase + kWeaponKeySlots) return false;
    if (slot) *slot = k - kWeaponKeyBase;
    return true;
}
// The two eyes' K of one frame and the latch the draws read: the last frame in which both eyes' estimates were in and agreed. A draw is
// corrected only while the latch holds: from the pairing's frame to kLatchHoldFrames frames after it. The latch drops at once on a disagreement.
// Each draw is corrected with the K its own frame solved; the latch only vouches that the two eyes' K agreed recently, so it holds over the frames
// in which one eye drew no readout and no new pair was formed (F148: the one-frame hold refused 112 of eye 0's draws in 54 of 342 part-eye seconds).
constexpr uint32_t kLatchHoldFrames = 4u;
struct KLatch {
    uint32_t frame[2] = {~0u, ~0u};
    CamK k[2];
    double oHud[2][3] = {}, oWorld[2][3] = {};
    uint32_t pairedFrame = ~0u;
    bool agreed = false;
    KAgree last;
    uint64_t pairs = 0, disagreed = 0, solo = 0;
    // Records eye e's estimate; when the other eye's is of the same frame, compares them. Returns true if this call made a comparison.
    bool put(int e, uint32_t f, const CamK& ck, const double oh[3], const double ow[3]) noexcept {
        if (e < 0 || e > 1) return false;
        frame[e] = f;
        k[e] = ck;
        for (int i = 0; i < 3; ++i) {
            oHud[e][i] = oh[i];
            oWorld[e][i] = ow[i];
        }
        if (frame[1 - e] != f || !k[1 - e].ok || !ck.ok || pairedFrame == f) return false;
        pairedFrame = f;
        last = kAgree(k[0], oHud[0], oWorld[0], k[1], oHud[1], oWorld[1]);
        ++pairs;
        agreed = kAgreeWithin(last);
        if (!agreed) ++disagreed;
        return true;
    }
    // B3 (the scope zoom's window): while eye 0 is drawn as the zoomed view its camera is a different one and cannot agree with eye 1's, so eye 1 alone vouches: its estimate stands as
    // the frame's pairing (the hold runs from it as from any pairing) without a comparison. Eye 0's estimate is not put while the window stands (the caller). Counted.
    bool putSolo(int e, uint32_t f, const CamK& ck, const double oh[3], const double ow[3]) noexcept {
        if (e < 0 || e > 1 || !ck.ok) return false;
        frame[e] = f;
        k[e] = ck;
        for (int i = 0; i < 3; ++i) {
            oHud[e][i] = oh[i];
            oWorld[e][i] = ow[i];
        }
        pairedFrame = f;
        agreed = true;
        ++solo;
        return true;
    }
    // May a draw of frame f use the corrected rows: the latch holds from the pairing's frame to kLatchHoldFrames frames after it, while the pair agreed.
    bool usable(uint32_t f) const noexcept { return agreed && pairedFrame != ~0u && f >= pairedFrame && f - pairedFrame <= kLatchHoldFrames; }
    // Frames from the pairing to frame f (a usable frame's: 0 or 1 is the pairing of this frame or the last, 2 to kLatchHoldFrames is the latch held over a missed pair).
    uint32_t gap(uint32_t f) const noexcept { return f - pairedFrame; }
    // An agreeing pair whose hold ran out before frame f: the draw is refused for want of a recent pair, not for a disagreement.
    bool pastHold(uint32_t f) const noexcept { return agreed && pairedFrame != ~0u && f > pairedFrame && f - pairedFrame > kLatchHoldFrames; }
};

// ---- the VALUE path: a record whose position is bit-equal to a published translation ---------------------------------------------------------
// The fallback identification of the weapon UI's record when the packer does not carry the tag (review 4.2, risks): the translation is left as the game
// wrote it, published, and the pool is searched for a record (stride 336) whose position (+16) is bit-equal to it. The record is then named like a
// tagged one: by the draw's own instance stream entry (the F63 CPU identification).
// A plain 16-byte load (the rig's; the hook passes drawtag::streamLoad16).
inline void plainLoadForTest(const uint8_t* p, uint32_t out[4]) noexcept { std::memcpy(out, p, 16); }
struct ValueTarget {
    float pos[3] = {};
    uint32_t sentinel = 0;
};
struct ValueHit {
    uint32_t index = 0, target = 0;
};
template <class Load>
inline bool checkRecordValue(const uint8_t* base, uint32_t bytes, uint32_t index, const ValueTarget* t, uint32_t n, ValueHit& hit, Load load) noexcept {
    const uint64_t at = static_cast<uint64_t>(index) * 336u + 16u;
    if (at + 16 > bytes) return false;
    uint32_t w[4];
    load(base + at, w);
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t b[3];
        std::memcpy(b, t[k].pos, sizeof(b));
        if (w[0] == b[0] && w[1] == b[1] && w[2] == b[2]) {
            hit.index = index;
            hit.target = k;
            return true;
        }
    }
    return false;
}
template <class Load>
inline uint32_t scanRecordsValue(const uint8_t* base, uint32_t bytes, const ValueTarget* t, uint32_t n, ValueHit* hits, uint32_t maxHits, Load load) noexcept {
    if (!base || !n || !maxHits) return 0;
    const uint32_t records = bytes / 336u;
    uint32_t c = 0;
    for (uint32_t i = 0; i < records && c < maxHits; ++i)
        if (checkRecordValue(base, bytes, i, t, n, hits[c], load)) ++c;
    return c;
}

// ---- C: the reticle's depth ----------------------------------------------------------------------------------------------------------------
constexpr double kReticleMinM = 0.5, kReticleMaxM = 50.0;
// F191: which key is the aim reticle's is no longer a constant here (it was 10, a pool slot, not an identity): native_on_foot_f191.h tracks the key the reticle's marker
// type (1001) uses, and the hit depth moves only that key.
constexpr int kMarchSteps = 32, kMarchBisect = 10;
enum MarchStatus : uint32_t { kMarchSky = 0, kMarchHit = 1, kMarchNear = 2, kMarchOffscreen = 3 };
// The scene rows' depth column as depth = A + B / w (reversed-Z with an infinite far plane: A = 0, B = near). False when the z column is not
// that function of w (then the depth cannot be linearized from the rows).
inline bool zwFit(const float rows[16], double* A, double* B) noexcept {
    const double zl[3] = {rows[2], rows[6], rows[10]}, wl[3] = {rows[3], rows[7], rows[11]};
    const double ww = dot3(wl, wl);
    if (!(ww > 1e-18)) return false;
    // z_clip = a w_clip + b, depth = z_clip / w_clip = a + b / w_clip.
    const double a = dot3(zl, wl) / ww;
    double res[3] = {zl[0] - a * wl[0], zl[1] - a * wl[1], zl[2] - a * wl[2]};
    const double zn = std::max(len3(zl), 1e-9);
    if (len3(res) > 1e-4 * zn + 1e-9) return false;
    *A = a;
    *B = static_cast<double>(rows[14]) - a * static_cast<double>(rows[15]);
    return std::isfinite(*A) && std::isfinite(*B);
}
// The view depth (clip w) the buffer's value stands for; infinity at the far plane.
inline double wOfDepth(double depth, double A, double B) noexcept {
    const double d = depth - A;
    if (!(std::fabs(d) > 1e-30)) return 1e30;
    const double w = B / d;
    return w > 0.0 ? w : 1e30;
}
struct MarchView {
    const float* rows = nullptr;   // b1 270..273 (16 floats, row-vector form)
    double ow[3] = {}, A = 0.0, B = 0.0;
    uint32_t width = 0, height = 0;
    const float* depth = nullptr;  // width * height values
};
// One sample of the aim line: false when the point is behind the camera or off the texture; else wPoint and wSurface (the buffered surface's view depth).
inline bool marchSample(const MarchView& v, const double O[3], const double u[3], double d, double* wPoint, double* wSurface) noexcept {
    const double p[3] = {O[0] + d * u[0] - v.ow[0], O[1] + d * u[1] - v.ow[1], O[2] + d * u[2] - v.ow[2]};
    double c[4];
    for (int j = 0; j < 4; ++j) c[j] = p[0] * v.rows[j] + p[1] * v.rows[4 + j] + p[2] * v.rows[8 + j] + v.rows[12 + j];
    if (!(c[3] > 1e-6)) return false;
    const double nx = c[0] / c[3], ny = c[1] / c[3];
    const double px = (nx * 0.5 + 0.5) * static_cast<double>(v.width), py = (0.5 - ny * 0.5) * static_cast<double>(v.height);
    if (!(px >= 0.0 && py >= 0.0 && px < static_cast<double>(v.width) && py < static_cast<double>(v.height))) return false;
    const uint32_t ix = static_cast<uint32_t>(px), iy = static_cast<uint32_t>(py);
    *wPoint = c[3];
    *wSurface = wOfDepth(static_cast<double>(v.depth[static_cast<size_t>(iy) * v.width + ix]), v.A, v.B);
    return true;
}
struct MarchOut {
    double d = kReticleMaxM;
    uint32_t status = kMarchSky;
};
// The aim line O + d u marched over d in [kReticleMinM, kReticleMaxM] in 32 geometric steps; the first step behind the buffered surface brackets
// the hit, which ten bisection steps refine. A first sample already behind a surface is a surface nearer than the minimum: ignored (status
// kMarchNear, the caller holds). The GPU pass runs this exact procedure (reticle_depth_shader.h).
inline MarchOut marchCpu(const MarchView& v, const double O[3], const double u[3]) noexcept {
    MarchOut o;
    const double ratio = std::pow(kReticleMaxM / kReticleMinM, 1.0 / (kMarchSteps - 1));
    double prev = kReticleMinM;
    bool off = false;
    for (int i = 0; i < kMarchSteps; ++i) {
        const double d = kReticleMinM * std::pow(ratio, static_cast<double>(i));
        double wp = 0.0, ws = 0.0;
        if (!marchSample(v, O, u, d, &wp, &ws)) {
            off = true;
            prev = d;
            continue;
        }
        if (wp >= ws) {
            if (i == 0) {
                o.status = kMarchNear;
                o.d = kReticleMinM;
                return o;
            }
            double lo = prev, hi = d;
            for (int b = 0; b < kMarchBisect; ++b) {
                const double mid = 0.5 * (lo + hi);
                double wm = 0.0, sm = 0.0;
                if (marchSample(v, O, u, mid, &wm, &sm) && wm >= sm) hi = mid;
                else lo = mid;
            }
            o.d = 0.5 * (lo + hi);
            o.status = kMarchHit;
            return o;
        }
        prev = d;
    }
    o.d = kReticleMaxM;
    o.status = off ? kMarchOffscreen : kMarchSky;
    return o;
}
// The angle in degrees between the game's P and the aim line O + d u (0 on it); -1 when P is at O.
inline double lineOffAngleDeg(const double O[3], const double u[3], const double P[3]) noexcept {
    const double d[3] = {P[0] - O[0], P[1] - O[1], P[2] - O[2]};
    const double l = len3(d);
    if (!(l > 1e-9)) return -1.0;
    double c = dot3(d, u) / l;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * kDeg;
}
// D_f: a critically damped step in 1/D (SmoothDamp's smoothing time: omega = 2 / T, the closed form of the constant-target step, so the answer
// does not depend on the frame time). T = 40 ms: 90% of a step in 78 ms, no overshoot.
struct InvDepthSmoother {
    bool have = false;
    double x = 0.0, v = 0.0;   // 1 / D and its rate
    static constexpr double kSmoothTimeS = 0.040;
    static double clampD(double d) noexcept { return d < kReticleMinM ? kReticleMinM : d > kReticleMaxM ? kReticleMaxM : d; }
    void reset(double d) noexcept {
        x = 1.0 / clampD(d);
        v = 0.0;
        have = true;
    }
    double step(double targetD, double dt) noexcept {
        const double xt = 1.0 / clampD(targetD);
        if (!have) {
            reset(targetD);
            return 1.0 / x;
        }
        if (!(dt > 0.0)) return 1.0 / x;
        if (dt > 0.25) dt = 0.25;
        const double w = 2.0 / kSmoothTimeS;
        const double e = x - xt, c = v + w * e, ex = std::exp(-w * dt);
        x = xt + (e + c * dt) * ex;
        v = (v - w * c * dt) * ex;
        return 1.0 / x;
    }
    double depth() const noexcept { return have ? 1.0 / x : 0.0; }
};

// ---- A: the pin-step trace -----------------------------------------------------------------------------------------------------------------
// One 3P game frame's figures, joined by the P1 frame index from the sites that see them. Lengths in metres, speeds in m/s unless a name says cm.
struct StepFrame {
    uint32_t p1 = 0;
    uint64_t qpc = 0, qpcFp = 0, recQpc = 0;
    uint32_t tid3 = 0, tidFp = 0, recTid = 0;
    bool haveFp = false, haveRec = false, haveCam = false, havePin = false, haveHand = false, haveFpRoot = false;
    float m3pT[3] = {}, up[3] = {};          // the 3P AMC matrix (what G and the pin use): translation and the body's up row
    float cam[3] = {};                       // the job camera the pin follows (world)
    float rec[3] = {};                       // the body's world translation as the renderer packs it (the AMC writer's first record)
    float recSpread = 0.0f, amcOrg[3] = {};  // the farthest other record from the first; the AMC's own origin (+0x2E0)
    bool haveAmcOrg = false;
    uint32_t recCalls = 0, recRecords = 0;
    float fpRoot[3] = {};                    // the FP AMC's root (the FP P1 call)
    // the pin (pinRecord): the demanded and the applied moves, the clamp flags, the crouch weight, the eye height less the standing height
    float pinDemandV = 0.0f, pinAppliedV = 0.0f, pinDemandH = 0.0f, pinAppliedH = 0.0f, wc = 0.0f, dEyeY = 0.0f;
    bool vClamp = false, hClamp = false;
    float handErrCm[2] = {-1.0f, -1.0f};
    bool handWritten[2] = {};
    // F86a: the pose carry of this frame's submit (joined by the P1 frame): the figures the line prints and the hand-lag trigger reads
    bool haveCarry = false;
    int carryWhy = 0;
    uint32_t poseAge = 0, prevCase = 0;
    float fStep[3] = {}, fTurn = 0.0f, mStep[3] = {}, mTurn = 0.0f, carry[3] = {};
    bool stepsOk = false;
    float carryBeforeCm[2] = {-1.0f, -1.0f}, carryAfterCm[2] = {-1.0f, -1.0f};
    // derived by the tracker
    float speed = 0.0f, vy = 0.0f, stepUp = 0.0f, camBody[3] = {}, camMoveCm = 0.0f, dtMs = 0.0f;
    float recMinusM3pCm = -1.0f, recMinusOrgCm = -1.0f, handLagCm = -1.0f;
    uint32_t trig = 0;
};
// F86b: the camera-in-the-model trigger fired on every walk start and is gone; hand-lag (the pose carry's error before the carry, over 2 cm) takes its place.
enum StepTrigger : uint32_t { kTrigM3pStep = 1, kTrigPinCap = 2, kTrigHandLag = 4, kTrigVy = 8 };
constexpr double kStepM3pM = 0.3, kStepHandLagCm = 2.0, kStepVyMps = 0.8;
constexpr int kStepBins = 5;   // still < 0.3, walk 0.3-2.5, jog 2.5-6, sprint 6-20, step over 20 m/s
inline int speedBin(double mps) noexcept { return !(mps >= 0.3) ? 0 : mps < 2.5 ? 1 : mps < 6.0 ? 2 : mps < 20.0 ? 3 : 4; }
inline const char* speedBinName(int b) noexcept {
    static const char* const n[kStepBins] = {"still", "walk", "jog", "sprint", "step"};
    return n[b < 0 || b >= kStepBins ? 0 : b];
}
struct StepAcc {
    uint64_t n = 0;
    double sum = 0.0, mx = 0.0;
    void add(double v) noexcept {
        ++n;
        sum += v;
        mx = std::max(mx, v);
    }
};
struct StepSecond {
    uint64_t frames = 0, noRec = 0, noPin = 0, noFp = 0, eventsStarted = 0, eventsSuppressed = 0;
    uint64_t trigSeen[4] = {};
    uint64_t suppressedBy[4] = {};   // F86b: per trigger, the times its 8 a minute cap held it back
    StepAcc rec[kStepBins], recTrig, orgVsM3p;
    double maxStepUpCm = 0.0;
};
struct StepResult {
    uint32_t trig = 0;
    bool eventStart = false, suppressed = false, printCur = false;
    uint32_t eventNo = 0;
    int offset = 0;   // the printed frame's place in its event: -nPre..-1 before the trigger frame, 0 at it, 1..kPost after
    int nPre = 0;     // earlier frames to print first (oldest first, via pre()), when an event starts
};
struct StepTracker {
    static constexpr int kPre = 16, kPost = 24, kMaxPerMinute = 8, kRing = kPre + 1;
    StepFrame hist[kRing];
    int histN = 0, histHead = 0;   // frames kept (the newest is the one just stepped); the next write index
    bool have = false;
    StepFrame prev;
    int postLeft = 0, offset = 0, lastNPre = 0;
    uint32_t eventNo = 0;
    uint64_t starts[4][kMaxPerMinute] = {};   // F86b: the start times of the last events each trigger began, per trigger
    uint32_t nStarts[4] = {};
    StepSecond sec;
    uint64_t totalFrames = 0, totalEvents = 0, totalTrig[4] = {};
    bool prevAir = false;
    // The i-th (0 = oldest) of the nPre frames before the one the last step() returned an event start for.
    const StepFrame& pre(int i) const noexcept { return hist[(histHead - 1 - lastNPre + i + kRing * 2) % kRing]; }
    // One frame, in order. freq: QPC ticks a second. Fills f's derived figures (f.camBody is the caller's) and returns what the caller prints.
    // keepHistory (F125): false while the per-frame lines are not written (log.on_foot_detail off): the frame is not copied into the history of earlier frames,
    // which is kept empty, so an event that starts then has no earlier frames (nPre 0). Everything else (the triggers, the cap, the counts) is the same.
    StepResult step(StepFrame& f, uint64_t freq, bool keepHistory = true) noexcept {
        StepResult r;
        const double fr = freq ? static_cast<double>(freq) : 1.0;
        ++totalFrames;
        ++sec.frames;
        if (have && f.qpc > prev.qpc) {
            const double dt = static_cast<double>(f.qpc - prev.qpc) / fr;
            f.dtMs = static_cast<float>(dt * 1000.0);
            if (dt < 0.2) {
                const double d[3] = {static_cast<double>(f.m3pT[0]) - prev.m3pT[0], static_cast<double>(f.m3pT[1]) - prev.m3pT[1],
                                     static_cast<double>(f.m3pT[2]) - prev.m3pT[2]};
                f.speed = static_cast<float>(len3(d) / dt);
                const double up[3] = {f.up[0], f.up[1], f.up[2]};
                f.stepUp = static_cast<float>(dot3(d, up));
                f.vy = static_cast<float>(static_cast<double>(f.stepUp) / dt);   // F86b: along the body's up row, not world Y (world Y read 1-3.7 m/s walking with no up-step)
                if (std::fabs(static_cast<double>(f.stepUp)) > kStepM3pM) r.trig |= kTrigM3pStep;
                if (f.haveCam && prev.haveCam) {   // a figure of the line, no longer a trigger (it fired on every walk start)
                    const double c[3] = {static_cast<double>(f.camBody[0]) - prev.camBody[0], static_cast<double>(f.camBody[1]) - prev.camBody[1],
                                         static_cast<double>(f.camBody[2]) - prev.camBody[2]};
                    f.camMoveCm = static_cast<float>(len3(c) * 100.0);
                }
                const bool air = std::fabs(static_cast<double>(f.vy)) > kStepVyMps;
                if (air != prevAir) r.trig |= kTrigVy;
                prevAir = air;
            }
        }
        if (f.haveCarry) {   // F86b: the hand-lag trigger: the pose carry's error before the carry, the larger hand
            f.handLagCm = std::max(f.carryBeforeCm[0], f.carryBeforeCm[1]);
            if (f.handLagCm > static_cast<float>(kStepHandLagCm)) r.trig |= kTrigHandLag;
        }
        if (f.havePin && (f.vClamp || f.hClamp)) r.trig |= kTrigPinCap;
        for (int b = 0; b < 4; ++b)
            if (r.trig & (1u << b)) {
                ++sec.trigSeen[b];
                ++totalTrig[b];
            }
        // the drawn record against the matrix we read
        if (f.haveRec) {
            const double d[3] = {static_cast<double>(f.rec[0]) - f.m3pT[0], static_cast<double>(f.rec[1]) - f.m3pT[1], static_cast<double>(f.rec[2]) - f.m3pT[2]};
            f.recMinusM3pCm = static_cast<float>(len3(d) * 100.0);
            sec.rec[speedBin(f.speed)].add(f.recMinusM3pCm);
            if (r.trig) sec.recTrig.add(f.recMinusM3pCm);
            if (f.haveAmcOrg) {
                const double o[3] = {static_cast<double>(f.amcOrg[0]) - f.m3pT[0], static_cast<double>(f.amcOrg[1]) - f.m3pT[1], static_cast<double>(f.amcOrg[2]) - f.m3pT[2]};
                sec.orgVsM3p.add(len3(o) * 100.0);
                const double q[3] = {static_cast<double>(f.rec[0]) - f.amcOrg[0], static_cast<double>(f.rec[1]) - f.amcOrg[1], static_cast<double>(f.rec[2]) - f.amcOrg[2]};
                f.recMinusOrgCm = static_cast<float>(len3(q) * 100.0);
            }
        } else {
            ++sec.noRec;
        }
        if (!f.havePin) ++sec.noPin;
        if (!f.haveFp) ++sec.noFp;
        sec.maxStepUpCm = std::max(sec.maxStepUpCm, std::fabs(static_cast<double>(f.stepUp)) * 100.0);
        f.trig = r.trig;
        // the event: a trigger frame starts one (unless 8 started in the last minute); the 24 frames after it print whatever they carry
        if (postLeft > 0) {
            --postLeft;
            r.printCur = true;
            r.eventNo = eventNo;
            r.offset = ++offset;
        } else if (r.trig) {
            // F86b: the cap is per trigger (8 events a minute each): a trigger at its cap is counted and does not start an event on its own.
            uint32_t capped = 0;
            for (int b = 0; b < 4; ++b) {
                if (!(r.trig & (1u << b))) continue;
                uint32_t recent = 0;
                for (const uint64_t s : starts[b])
                    if (s && f.qpc >= s && f.qpc - s < 60ull * freq) ++recent;
                if (recent >= static_cast<uint32_t>(kMaxPerMinute)) {
                    capped |= 1u << b;
                    ++sec.suppressedBy[b];
                }
            }
            if ((r.trig & ~capped) == 0u) {
                r.suppressed = true;
                ++sec.eventsSuppressed;
            } else {
                for (int b = 0; b < 4; ++b)
                    if ((r.trig & ~capped) & (1u << b)) starts[b][nStarts[b]++ % kMaxPerMinute] = f.qpc;
                ++eventNo;
                ++totalEvents;
                ++sec.eventsStarted;
                r.eventStart = true;
                r.printCur = true;
                r.eventNo = eventNo;
                r.nPre = std::min(histN, kPre);
                lastNPre = r.nPre;
                offset = 0;
                r.offset = 0;
                postLeft = kPost;
            }
        }
        if (keepHistory) {
            hist[histHead] = f;   // the history keeps the frame after the decision, so an event's earlier frames are the ones before it
            histHead = (histHead + 1) % kRing;
            if (histN < kRing - 1) ++histN;
        } else {
            histN = 0;   // none kept: a later event with the history on prints only the frames kept since
        }
        prev = f;
        have = true;
        return r;
    }
};

}  // namespace f85
}  // namespace nofoot
}  // namespace edvr
