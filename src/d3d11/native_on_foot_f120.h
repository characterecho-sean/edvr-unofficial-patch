// F120 (native on-foot VR): the panel cursor stays on the panel when the head turns, pure parts. Header only, no game or D3D11 state, so native_frame_test
// runs the very functions the placement hook uses. Journal: the arc doc, 2026-10-25, F120.
//
// F117 put the cursor at the true uv on turned panels and the clicks always landed there; what was left is that the cursor rode with the head. The mouse
// sets a uv that ignores the head (log edvr_gfx_20261025_113157: 0 drift per degree of head turn with the mouse still), the game places the sprite from
// that uv on the panel's pick mesh and writes the sprite's position at the controller +0x140. That position is in the pick camera's frame, and the pick
// camera follows the head (it matches the eye at +1.00 on all axes), so a uv that stays puts the sprite where the head points. On the screen-space panels
// the pick mesh is camera-locked while the drawn panel stays in the HUD canvas's frame (the HRC matrix at +0x430). So the sprite is carried by what the
// head has turned against the HUD frame since the panel opened: p' = E Ht H0 E0t p (E the eye's view rows, H the HUD frame's rows, both normalised; E0 and
// H0 their values at the reference, the first screen-space 2D placement after 30 placement-free frames). Nothing moved gives the identity; a head turn with
// the HUD still counter-rotates p; the head and the HUD turning together leave p alone. The rotation is about the camera's origin, so the 7-29 cm between
// the HUD frame's origin and the eye is ignored (about 3 deg at a 20 deg head turn).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f120 {

// ---- the build's addresses (334925) ----
// The screen-space owner function (the pick mesh owner's vtable +0x10): mov r11,rsp; mov [r11+0x18],rbx; push rdi; sub rsp,0x140; movaps [r11-0x18],xmm6; ...
constexpr uintptr_t kOwnerRva = 0x47299F0;
constexpr uint8_t kOwnerPrologue[16] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00, 0x41};
constexpr uint32_t kOffOwnerCount = 0x1B0;    // owner +0x1B0: a qword count (the function leaves when it is 0)
constexpr uint32_t kOffOwnerArray = 0x1B8;    // owner +0x1B8: an array of pointers; [array] is the first entry (rdx in 0x47299F0)
constexpr uint32_t kOffScreenFlag = 0x1C0;    // the entry's byte at +0x1C0: set = the screen-space branch (the camera-frame scaling)
constexpr uint32_t kDropFrames = 30;          // a reference drops after this many frames without a placement
constexpr uint32_t kRunGapFrames = 3;         // a mouse-still run goes on across at most this many frames between placements
constexpr double kMinTurnDeg = 5.0;           // the health rule judges seconds with more head turn than this ...
constexpr double kMaxDriftMm = 5.0;           // ... and wants the cursor's drift on the panel under this
constexpr double kMaxNonOrtho = 0.05;         // the largest dot product of two normalised rows still taken for a rotation

inline bool ownerPinned(const uint8_t bytes[16]) noexcept { return std::memcmp(bytes, kOwnerPrologue, sizeof(kOwnerPrologue)) == 0; }
// The pin's verdict when it is first read. F154's lever hook overwrites the first bytes with its jump, and only installs after checking the same 16 bytes, so
// with that hook armed the function is the pinned one whatever its bytes now read (the 2026-10-28 flight read the jump and refused every panel).
inline bool ownerPinnedOrHooked(const uint8_t bytes[16], bool leverHookArmed) noexcept { return leverHookArmed || ownerPinned(bytes); }

// The screen-space test: the owner function is the pinned one, its list is not empty and the first entry's flag is set.
inline bool screenSpace(uintptr_t ownerFnRva, uint64_t count, uint8_t flag) noexcept { return ownerFnRva == kOwnerRva && count > 0 && flag != 0; }

// ---- 3x3 algebra on row-major doubles; a frame is its axes as rows ----
struct Mat {
    double m[9];
};
inline Mat identity() noexcept {
    Mat r{};
    r.m[0] = r.m[4] = r.m[8] = 1.0;
    return r;
}
inline Mat mul(const Mat& a, const Mat& b) noexcept {
    Mat c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return c;
}
inline Mat transpose(const Mat& a) noexcept {
    Mat t{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t.m[j * 3 + i] = a.m[i * 3 + j];
    return t;
}
inline void apply(const Mat& a, const double v[3], double out[3]) noexcept {   // out = a v (v a column)
    for (int i = 0; i < 3; ++i) out[i] = a.m[i * 3] * v[0] + a.m[i * 3 + 1] * v[1] + a.m[i * 3 + 2] * v[2];
}
inline double det(const Mat& a) noexcept {
    return a.m[0] * (a.m[4] * a.m[8] - a.m[5] * a.m[7]) - a.m[1] * (a.m[3] * a.m[8] - a.m[5] * a.m[6]) + a.m[2] * (a.m[3] * a.m[7] - a.m[4] * a.m[6]);
}
// The turn between two frames, in degrees: the angle of a bt.
inline double angleDeg(const Mat& a, const Mat& b) noexcept {
    double tr = 0.0;
    for (int i = 0; i < 9; ++i) tr += a.m[i] * b.m[i];
    double c = (tr - 1.0) * 0.5;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}
// The rows of a frame normalised (the HRC matrix carries its scale in them); false when a row has no length. ortho gets the largest |dot| of two of them.
inline bool unitRows(const double raw[9], Mat& out, double* ortho) noexcept {
    for (int r = 0; r < 3; ++r) {
        const double n = std::sqrt(raw[r * 3] * raw[r * 3] + raw[r * 3 + 1] * raw[r * 3 + 1] + raw[r * 3 + 2] * raw[r * 3 + 2]);
        if (!(n > 1e-9) || !std::isfinite(n)) return false;
        for (int k = 0; k < 3; ++k) out.m[r * 3 + k] = raw[r * 3 + k] / n;
    }
    double worst = 0.0;
    for (int a = 0; a < 3; ++a) {
        const int b = (a + 1) % 3;
        const double d = std::fabs(out.m[a * 3] * out.m[b * 3] + out.m[a * 3 + 1] * out.m[b * 3 + 1] + out.m[a * 3 + 2] * out.m[b * 3 + 2]);
        if (d > worst) worst = d;
    }
    if (ortho) *ortho = worst;
    return true;
}
// R = Ry(yaw) Rx(pitch) Rz(roll), in degrees (the validity reading of E Ht at rest): the third column is (sin yaw cos pitch, -sin pitch, cos yaw cos pitch).
inline void ypr(const Mat& r, double* yaw, double* pitch, double* roll) noexcept {
    const double rad = 180.0 / 3.14159265358979323846;
    double sp = -r.m[5];
    if (sp > 1.0) sp = 1.0;
    if (sp < -1.0) sp = -1.0;
    const double y = std::atan2(r.m[2], r.m[8]), p = std::asin(sp);
    const double cy = std::cos(y), sy = std::sin(y);
    const double a00 = cy * r.m[0] - sy * r.m[6], a10 = r.m[3];   // Ry(yaw)t R = Rx(pitch) Rz(roll): column 0 is (cos roll, cos pitch sin roll, sin pitch sin roll)
    const double cp = std::cos(p);
    *yaw = y * rad;
    *pitch = p * rad;
    *roll = std::fabs(cp) > 1e-9 ? std::atan2(a10 / cp, a00) * rad : 0.0;
}

// ---- one placement ----
enum class Why : uint8_t { kApplied = 0, kFlat, kNoEye, kNoHrc, kDegenerate, kNotScreen, kNotPinned, kHead, kWriteFailed, kCount };
inline const char* whyName(Why w) noexcept {
    switch (w) {
        case Why::kApplied: return "applied";
        case Why::kFlat: return "flat profile";
        case Why::kNoEye: return "no eye view";
        case Why::kNoHrc: return "no HRC matrix";
        case Why::kDegenerate: return "degenerate row";
        case Why::kNotScreen: return "not screen-space";
        case Why::kNotPinned: return "owner bytes not pinned";
        case Why::kHead: return "setting head";
        default: return "write failed";
    }
}

struct Input {
    uint32_t frame = 0;
    bool flat = false, pinned = false, screen = false, follow = true;   // follow: the setting panel (not head)
    bool haveEye = false, haveHrc = false;
    double E[9] = {}, H[9] = {};   // the eye's view rows and the HUD canvas's rows, as read
    double u = 0.0, v = 0.0;       // the window's uv
    uintptr_t key = 0;             // the panel
    double p[3] = {};              // the sprite in the pick camera's frame: the game's, or F117's point where F117 moved it
};
struct Out {
    Why why = Why::kNotScreen;
    bool changes = false;          // the corrected point differs from the point given
    double target[3] = {};
};

// One second of readings (the head's turn against the HUD frame, the cursor's drift while the mouse is still) and the counts.
struct Sec {
    uint32_t placements = 0, screen = 0, corrected = 0, refs = 0, refused[static_cast<int>(Why::kCount)] = {};
    bool haveFirst = false, haveLast = false;
    Mat firstE{}, firstH{}, firstR{}, lastR{};
    double turnE = 0.0, turnH = 0.0, turnEH = 0.0;       // degrees, the most over the second from its first placement
    uint32_t stillN = 0;                                  // placements that continued a mouse-still run
    double stillTurn = 0.0, stillDrift = 0.0;             // deg of the head against the HUD frame and mm of the cursor's drift on the panel, the most over the runs
    void merge(const Sec& o) noexcept {
        placements += o.placements;
        screen += o.screen;
        corrected += o.corrected;
        refs += o.refs;
        for (int i = 0; i < static_cast<int>(Why::kCount); ++i) refused[i] += o.refused[i];
        if (o.turnE > turnE) turnE = o.turnE;
        if (o.turnH > turnH) turnH = o.turnH;
        if (o.turnEH > turnEH) turnEH = o.turnEH;
        stillN += o.stillN;
        if (o.stillTurn > stillTurn) stillTurn = o.stillTurn;
        if (o.stillDrift > stillDrift) stillDrift = o.stillDrift;
        if (o.haveLast) {
            haveLast = true;
            lastR = o.lastR;
        }
    }
};

// The head against the HUD frame, in words: whether the HUD frame turned with the head (E Ht constant), which is when the correction is near zero.
inline const char* frameText(const Sec& s) noexcept {
    if (!s.screen) return "no screen-space panel";
    if (s.turnE < kMinTurnDeg) return "the head did not turn";
    if (s.turnEH < 1.0) return "the HUD frame turns with the head, E.Ht holds constant, so the correction is near zero and a panel seen to hold still is not in the HRC frame";
    if (s.turnH < 1.0) return "the HUD frame holds still, so the correction follows the head's turn";
    return "the HUD frame turns, but not with the head";
}

class Tracker {
public:
    Sec sec;   // this second; the frame thread takes it and starts a new one

    Out place(const Input& in) noexcept {
        Out out;
        ++sec.placements;
        if (in.flat) return refuse(out, Why::kFlat);
        if (!in.pinned) return refuse(out, Why::kNotPinned);
        if (!in.screen) return refuse(out, Why::kNotScreen);
        ++sec.screen;
        if (!in.haveEye) return refuse(out, Why::kNoEye);
        if (!in.haveHrc) return refuse(out, Why::kNoHrc);
        Mat e{}, h{};
        double oe = 0.0, oh = 0.0;
        if (!unitRows(in.E, e, &oe) || !unitRows(in.H, h, &oh) || !(oe <= kMaxNonOrtho) || !(oh <= kMaxNonOrtho) || !std::isfinite(in.p[0]) ||
            !std::isfinite(in.p[1]) || !std::isfinite(in.p[2]))
            return refuse(out, Why::kDegenerate);
        const Mat r = mul(e, transpose(h));   // E Ht: the HUD frame's vectors in the eye's frame
        // The reference: the first placement after kDropFrames placement-free frames.
        bool gap = !ref_.valid || in.frame < ref_.last;
        if (!gap) {
            const uint32_t since = in.frame - ref_.last;
            gap = (since ? since - 1u : 0u) >= kDropFrames;
        }
        if (gap) {
            ref_.valid = true;
            ref_.r0 = r;
            ++sec.refs;
            for (Run& run : runs_) run.valid = false;
        }
        ref_.last = in.frame;
        // The correction: M = E Ht H0 E0t = R R0t.
        const Mat m = mul(r, transpose(ref_.r0));
        double moved[3];
        apply(m, in.p, moved);
        const double* shown = in.follow ? moved : in.p;
        // The readings: the head, the HUD frame and the two against each other since this second's first placement.
        if (!sec.haveFirst) {
            sec.haveFirst = true;
            sec.firstE = e;
            sec.firstH = h;
            sec.firstR = r;
        }
        const double te = angleDeg(e, sec.firstE), th = angleDeg(h, sec.firstH), teh = angleDeg(r, sec.firstR);
        if (te > sec.turnE) sec.turnE = te;
        if (th > sec.turnH) sec.turnH = th;
        if (teh > sec.turnEH) sec.turnEH = teh;
        sec.haveLast = true;
        sec.lastR = r;
        // The cursor on the panel: q = H Et p' = Rt p' (the HUD frame's own coordinates), in mm. While the mouse is still (the same uv on the same panel in
        // consecutive placements) it should not move however the head turns.
        double q[3];
        apply(transpose(r), shown, q);
        for (double& x : q) x *= 1000.0;
        Run* run = nullptr;
        for (Run& c : runs_)
            if (c.valid && c.key == in.key) run = &c;
        bool cont = false;
        if (run) {
            const bool sameUv = std::fabs(in.u - run->u) < 1e-6 && std::fabs(in.v - run->v) < 1e-6;
            cont = sameUv && in.frame >= run->frame && in.frame - run->frame <= kRunGapFrames;
        } else {
            run = &runs_[0];
            for (Run& c : runs_) {
                if (!c.valid) {
                    run = &c;
                    break;
                }
                if (c.frame < run->frame) run = &c;
            }
        }
        if (cont) {
            ++sec.stillN;
            const double d = std::sqrt((q[0] - run->q[0]) * (q[0] - run->q[0]) + (q[1] - run->q[1]) * (q[1] - run->q[1]) + (q[2] - run->q[2]) * (q[2] - run->q[2]));
            const double t = angleDeg(r, run->r);
            if (d > sec.stillDrift) sec.stillDrift = d;
            if (t > sec.stillTurn) sec.stillTurn = t;
            run->frame = in.frame;
        } else {
            run->valid = true;
            run->key = in.key;
            run->u = in.u;
            run->v = in.v;
            run->frame = in.frame;
            run->r = r;
            for (int k = 0; k < 3; ++k) run->q[k] = q[k];
        }
        if (!in.follow) return refuse(out, Why::kHead);   // the readings above stand: the drift under head is what panel is compared with
        out.why = Why::kApplied;
        ++sec.corrected;
        double delta = 0.0;
        for (int k = 0; k < 3; ++k) {
            out.target[k] = moved[k];
            const double d = std::fabs(moved[k] - in.p[k]);
            if (d > delta) delta = d;
        }
        out.changes = delta > 1e-7;
        return out;
    }

    // The write of an applied placement did not happen: counted as a refusal.
    void writeFailed() noexcept {
        if (sec.corrected) --sec.corrected;
        ++sec.refused[static_cast<int>(Why::kWriteFailed)];
    }

private:
    struct Ref {
        bool valid = false;
        uint32_t last = 0;
        Mat r0{};
    } ref_;
    struct Run {
        bool valid = false;
        uintptr_t key = 0;
        uint32_t frame = 0;
        double u = 0.0, v = 0.0, q[3] = {};
        Mat r{};
    } runs_[4];

    Out refuse(Out& out, Why why) noexcept {
        out.why = why;
        out.changes = false;
        ++sec.refused[static_cast<int>(why)];
        return out;
    }
};

// The sprite position to write once for a placement: F120's corrected point when it changes the point it was given (it started from F117's point where F117
// moved the sprite), else F117's point, else nothing.
inline bool finalTarget(bool f117Moved, const double f117Target[3], const Out& f120, double out[3]) noexcept {
    if (f120.why == Why::kApplied && f120.changes) {
        for (int k = 0; k < 3; ++k) out[k] = f120.target[k];
        return true;
    }
    if (f117Moved) {
        for (int k = 0; k < 3; ++k) out[k] = f117Target[k];
        return true;
    }
    return false;
}

}  // namespace f120
}  // namespace nofoot
}  // namespace edvr
