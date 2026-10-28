// F154 (native on-foot VR): the panel cursor's lever, read from the game, and a signed turn instrument, pure parts. Header only, no game or D3D11 state, so
// native_frame_test runs the very functions the placement hook and the owner transform hook use. Journal: the arc doc, 2026-10-28, F154.
//
// The F120 flight (log edvr_gfx_20261025_134757) read the pick mesh turning about twice the head's turn in the pick camera's frame. Static RE of build 334925
// (analysis\decomp\verify_20261028_onfoot_panel_pick_frame.txt) puts the cause in the screen-space owner's transform function (exe+0x47299F0, the pick mesh
// owner's vtable +0x10): after it puts the owner's world transform S into the pick camera C2's frame (the same out block as the upright owner exe+0x4526E40,
// rows +0x10/+0x20/+0x30, position q' at +0x40, scale at +0x50) it multiplies q'.x, q'.y and the scale by rho = tan(0.5 fov_C1) / tan(0.5 fov_UI), about 2 in
// VR (the headset's fov against the UI's 56 deg) and 1 in flat. The pick mesh is the drawn panel magnified by rho about the point (0, 0, q'.z): a point p0 of
// the drawn panel becomes P = (rho p0.x, rho p0.y, q'.z + rho (p0.z - q'.z)). That lever is why the pick point turned about twice the head's turn.
//
// Where the cursor quad's own draw reads the sprite (controller +0x140) is not known, so this build reads the lever from the game and offers one fix per
// draw model, as a live A/B (fix.native_on_foot_panel_cursor_test): drawn, which is right if the cursor and the panel are drawn by the same transform from C2's
// frame (the sprite is divided by the lever), and mirrored, which is right if the cursor's draw divides by |rho|. The signed turn instrument reads, per
// mouse-still run, how much of the turn a point held on the panel would make each stream of points makes (k = 1 held on the panel, 0 locked to the camera).
#pragma once

#include "native_on_foot_f120.h"   // Mat, the algebra, Out, finalTarget, the owner's pinned address and entry offsets

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace edvr {
namespace nofoot {
namespace f154 {

// ---- the build's addresses (334925) ----
// The hooked function is F120's pinned one (exe+0x47299F0): mov r11,rsp; mov [r11+0x18],rbx; push rdi; sub rsp,0x140. Its slot is the pick mesh owner's vtable
// (exe+0x5607E58) +0x10. The first two instructions are 3 + 4 bytes: whole instructions covering the 5-byte patch.
constexpr uintptr_t kOwnerRva = f120::kOwnerRva, kOwnerSlotRva = 0x5607E68;
constexpr size_t kOwnerStolen = 7;
constexpr uint32_t kOffEntryScale = 0xE0;            // the owner's first entry S (owner +0x1B8, first pointer): its world transform's scale (float)
constexpr uint32_t kOffOutPos = 0x40, kOffOutScale = 0x50;   // the output block (rdx): the position q' (three floats) and the scale (float)
// The cross-check's chain, all guarded reads that may fail: A = [owner+8]; Y = [A+0x838] (the camera object), Cm = Y+0x10, C1 = Cm+0x70 (fov float at C1+0x260);
// W = [A+0x830] (the render state), W18 = [W+0x18] (fov float at W18+0x17C), the render mode dword at W+0x3694.
constexpr uint32_t kOffOwnerService = 0x8, kOffServiceRender = 0x830, kOffServiceCamera = 0x838, kCameraObjectToCm = 0x10, kCmToC1 = 0x70, kOffC1Fov = 0x260;
constexpr uint32_t kOffRenderUi = 0x18, kOffUiFov = 0x17C, kOffRenderMode = 0x3694;

constexpr double kMinRho = 0.05;                     // drawn and mirrored divide by the lever: refused below this size
constexpr double kJudgedTurnDeg = 2.0;               // a placement counts toward k once the head has turned this far against the HUD frame since its run began
constexpr double kMinAxisRad2 = 3.0e-4;              // a k is read only from a turn of about 1 deg or more (the sum of squares of the model's rotation) about its axis
constexpr size_t kLeverSlots = 4;
constexpr size_t kFrameRecords = 64;                 // the dump-sync lines kept for the frame thread to write (one eye run's window is 20 frames)

// ---- the setting (fix.native_on_foot_panel_cursor_test) ----
enum class Setting : uint8_t { kShipped = 0, kUv, kGame, kDrawn, kMirrored, kCount };
inline const char* settingName(Setting s) noexcept {
    switch (s) {
        case Setting::kUv: return "uv";
        case Setting::kGame: return "game";
        case Setting::kDrawn: return "drawn";
        case Setting::kMirrored: return "mirrored";
        default: return "shipped";
    }
}
// True for a known word; an unknown or missing value gives shipped and false.
inline bool parseSetting(const char* text, Setting* out) noexcept {
    *out = Setting::kShipped;
    if (!text) return false;
    for (int i = 0; i < static_cast<int>(Setting::kCount); ++i) {
        const Setting s = static_cast<Setting>(i);
        if (std::strcmp(text, settingName(s)) == 0) {
            *out = s;
            return true;
        }
    }
    return false;
}
inline bool needsLever(Setting s) noexcept { return s == Setting::kDrawn || s == Setting::kMirrored; }
inline bool appliesF117(Setting s) noexcept { return s == Setting::kShipped || s == Setting::kUv; }
inline bool appliesF120(Setting s) noexcept { return s == Setting::kShipped; }

// ---- the lever ----
struct Lever {
    bool valid = false;
    uintptr_t owner = 0;
    uint32_t frame = 0;          // the native frame the hook saw it in
    double rho = 0.0;            // out scale / S scale, signed
    double scale = 0.0;          // S +0xE0
    double q[3] = {};            // the owner's position in C2's frame after the lever (out +0x40)
};

enum class ReadResult : uint8_t { kLever = 0, kNotScreen, kEmpty, kUnreadable, kBadScale };

// The lever from the owner and the output block the original function just filled. read(address, destination, bytes) returns false for an unreadable address;
// nothing is dereferenced but through it.
template <class ReadFn>
inline ReadResult readLever(uintptr_t owner, uintptr_t out, ReadFn&& read, Lever* lever) {
    uint64_t count = 0;
    uintptr_t array = 0, entry = 0;
    uint8_t flag = 0;
    if (!read(owner + f120::kOffOwnerCount, &count, sizeof(count))) return ReadResult::kUnreadable;
    if (!count) return ReadResult::kEmpty;
    if (!read(owner + f120::kOffOwnerArray, &array, sizeof(array)) || !array || !read(array, &entry, sizeof(entry)) || !entry ||
        !read(entry + f120::kOffScreenFlag, &flag, 1))
        return ReadResult::kUnreadable;
    if (!flag) return ReadResult::kNotScreen;
    float scale = 0.0f, outScale = 0.0f, q[3] = {};
    if (!read(entry + kOffEntryScale, &scale, sizeof(scale)) || !read(out + kOffOutScale, &outScale, sizeof(outScale)) || !read(out + kOffOutPos, q, sizeof(q)))
        return ReadResult::kUnreadable;
    const double rho = static_cast<double>(outScale) / static_cast<double>(scale);
    if (!(std::fabs(static_cast<double>(scale)) > 1e-9) || !std::isfinite(rho) || !std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2]))
        return ReadResult::kBadScale;
    lever->valid = true;
    lever->owner = owner;
    lever->rho = rho;
    lever->scale = scale;
    for (int k = 0; k < 3; ++k) lever->q[k] = q[k];
    return ReadResult::kLever;
}

// The cross-check: the fov ratio the function should have used, from the game's own fields. Each field is read on its own, so a chain that breaks part way
// still reports what it reached.
struct Cross {
    bool haveC1 = false, haveUi = false, haveMode = false;
    double fovC1 = 0.0, fovUi = 0.0;   // radians
    int32_t mode = -1;
    double rhoFov() const noexcept {
        if (!haveC1 || !haveUi) return std::numeric_limits<double>::quiet_NaN();
        const double a = std::tan(0.5 * fovC1), b = std::tan(0.5 * fovUi);
        return std::fabs(b) > 1e-9 ? a / b : std::numeric_limits<double>::quiet_NaN();
    }
};
template <class ReadFn>
inline Cross readCross(uintptr_t owner, ReadFn&& read) {
    Cross c;
    uintptr_t a = 0, camera = 0, render = 0, ui = 0;
    if (!read(owner + kOffOwnerService, &a, sizeof(a)) || !a) return c;
    float f = 0.0f;
    if (read(a + kOffServiceCamera, &camera, sizeof(camera)) && camera && read(camera + kCameraObjectToCm + kCmToC1 + kOffC1Fov, &f, sizeof(f)) && std::isfinite(f)) {
        c.haveC1 = true;
        c.fovC1 = f;
    }
    if (read(a + kOffServiceRender, &render, sizeof(render)) && render) {
        if (read(render + kOffRenderUi, &ui, sizeof(ui)) && ui && read(ui + kOffUiFov, &f, sizeof(f)) && std::isfinite(f)) {
            c.haveUi = true;
            c.fovUi = f;
        }
        int32_t m = 0;
        if (read(render + kOffRenderMode, &m, sizeof(m))) {
            c.haveMode = true;
            c.mode = m;
        }
    }
    return c;
}

// The last lever per owner, with the frame it was read in. A placement uses its owner's if it is from this frame or the one before.
class LeverTable {
public:
    void put(const Lever& l) noexcept {
        Lever* at = nullptr;
        for (Lever& s : slot_)
            if (s.valid && s.owner == l.owner) at = &s;
        if (!at)
            for (Lever& s : slot_)
                if (!s.valid) {
                    at = &s;
                    break;
                }
        if (!at) {
            at = &slot_[0];
            for (Lever& s : slot_)
                if (s.frame < at->frame) at = &s;
        }
        *at = l;
    }
    bool fresh(uintptr_t owner, uint32_t frame, Lever* out) const noexcept {
        for (const Lever& s : slot_)
            if (s.valid && s.owner == owner && frame >= s.frame && frame - s.frame <= 1u) {
                *out = s;
                return true;
            }
        return false;
    }
    // The cross-check is read once a frame, whichever owner the call was for.
    bool crossDue(uint32_t frame) const noexcept { return !crossValid_ || crossFrame_ != frame; }
    void crossTaken(uint32_t frame) noexcept {
        crossValid_ = true;
        crossFrame_ = frame;
    }
    void clear() noexcept { *this = LeverTable{}; }

private:
    Lever slot_[kLeverSlots];
    bool crossValid_ = false;
    uint32_t crossFrame_ = 0;
};

// ---- the lever as a transform of points in C2's frame (z forward, metres) ----
// What the game does to a point of the drawn panel.
inline void applyLever(double rho, double qz, const double p0[3], double P[3]) noexcept {
    P[0] = rho * p0[0];
    P[1] = rho * p0[1];
    P[2] = qz + rho * (p0[2] - qz);
}

enum class Refusal : uint8_t { kNone = 0, kNoLever, kTinyRho, kNonFinite };
inline const char* refusalName(Refusal r) noexcept {
    switch (r) {
        case Refusal::kNoLever: return "no lever";
        case Refusal::kTinyRho: return "tiny rho";
        case Refusal::kNonFinite: return "not finite";
        default: return "none";
    }
}
inline Refusal checkLever(const Lever* lever, const double P[3]) noexcept {
    if (!lever || !lever->valid) return Refusal::kNoLever;
    if (!std::isfinite(lever->rho) || !std::isfinite(lever->q[2]) || !std::isfinite(P[0]) || !std::isfinite(P[1]) || !std::isfinite(P[2])) return Refusal::kNonFinite;
    if (std::fabs(lever->rho) < kMinRho) return Refusal::kTinyRho;
    return Refusal::kNone;
}
// drawn: the game's sprite P put back on the drawn panel, the lever undone.
inline Refusal undoLever(const Lever* lever, const double P[3], double p0[3]) noexcept {
    const Refusal r = checkLever(lever, P);
    if (r != Refusal::kNone) return r;
    const double qz = lever->q[2];
    p0[0] = P[0] / lever->rho;
    p0[1] = P[1] / lever->rho;
    p0[2] = qz + (P[2] - qz) / lever->rho;
    return Refusal::kNone;
}
// mirrored: the same shape of lever with only the sign of rho taken as the cursor's draw reads it (the draw divides by |rho|).
inline Refusal mirrorLever(const Lever* lever, const double P[3], double out[3]) noexcept {
    const Refusal r = checkLever(lever, P);
    if (r != Refusal::kNone) return r;
    const double s = lever->rho < 0.0 ? -1.0 : 1.0, qz = lever->q[2];
    out[0] = s * P[0];
    out[1] = s * P[1];
    out[2] = qz + s * (P[2] - qz);
    return Refusal::kNone;
}

// What one placement does with its sprite under a setting. The game's sprite (read before any write), F117's decision and F120's outcome are the inputs.
struct Pick {
    bool applies = true;          // false: drawn or mirrored on a panel that is not screen-space (its sprite is the game's, which is right there)
    bool write = false;
    Refusal refused = Refusal::kNone;   // drawn and mirrored: why the game's place stands on a screen-space panel
    double target[3] = {};
};
inline Pick choose(Setting s, bool screen, bool f117Moved, const double f117Target[3], const f120::Out& f120out, const double game[3], const Lever* lever) noexcept {
    Pick p;
    switch (s) {
        case Setting::kShipped:
            p.write = f120::finalTarget(f117Moved, f117Target, f120out, p.target);
            break;
        case Setting::kUv:
            if (f117Moved) {
                p.write = true;
                for (int k = 0; k < 3; ++k) p.target[k] = f117Target[k];
            }
            break;
        case Setting::kGame:
            break;
        case Setting::kDrawn:
        case Setting::kMirrored:
            if (!screen) {
                p.applies = false;
                break;
            }
            p.refused = s == Setting::kDrawn ? undoLever(lever, game, p.target) : mirrorLever(lever, game, p.target);
            p.write = p.refused == Refusal::kNone;
            break;
        default:
            break;
    }
    return p;
}

// ---- the signed turn instrument ----
enum Stream : int { kGame = 0, kUv = 1, kWritten = 2, kStreams = 3 };
inline const char* streamName(int s) noexcept { return s == kGame ? "game" : s == kUv ? "uv" : "written"; }

// The rotation that takes the direction a to the direction b (the shortest arc), as an axis times its angle in radians; false for a zero vector or opposite ones.
inline bool rotVec(const double a[3], const double b[3], double out[3]) noexcept {
    const double na = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), nb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!(na > 1e-9) || !(nb > 1e-9) || !std::isfinite(na) || !std::isfinite(nb)) return false;
    const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double sc = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (sc < 1e-12 * na * nb) {
        out[0] = out[1] = out[2] = 0.0;
        return d > 0.0;
    }
    const double angle = std::atan2(sc, d);
    for (int k = 0; k < 3; ++k) out[k] = c[k] / sc * angle;
    return true;
}

// k = S_ab / S_aa per axis: a is the rotation the model (a point held on the panel) makes of a point from the run's start, b the one the stream made. Axis 0
// is about x (pitch), axis 1 about y (yaw). 0: locked to the camera; 1: held on the panel in the eye's frame; negative: against it.
struct Fit {
    double sab[2] = {0.0, 0.0}, saa[2] = {0.0, 0.0};
    void add(const double a[3], const double b[3]) noexcept {
        for (int i = 0; i < 2; ++i) {
            sab[i] += a[i] * b[i];
            saa[i] += a[i] * a[i];
        }
    }
    void merge(const Fit& o) noexcept {
        for (int i = 0; i < 2; ++i) {
            sab[i] += o.sab[i];
            saa[i] += o.saa[i];
        }
    }
    bool valid(int axis) const noexcept { return saa[axis] >= kMinAxisRad2; }
    double k(int axis) const noexcept { return saa[axis] > 0.0 ? sab[axis] / saa[axis] : 0.0; }
};

// One second's readings (or a flight's): the placements and writes, the owner transform hook's lever, the turn instrument.
struct Sec {
    // the placement hook (panelCursorApply, the key on, VR)
    uint32_t placements = 0, screen = 0, written = 0, writeFailed = 0, refusedLever[3] = {};   // refusedLever: no lever, tiny rho, not finite
    // the owner transform hook
    uint32_t calls = 0, leverReads = 0, notScreen = 0, unread = 0;
    double rhoSum = 0.0, rhoMin = std::numeric_limits<double>::infinity(), rhoMax = -std::numeric_limits<double>::infinity(), qzSum = 0.0;
    bool haveCross = false;
    Cross cross;   // the last reading
    // the turn instrument
    uint32_t refusedTurn[3] = {};   // no eye view, no HRC matrix, degenerate row
    uint32_t runs = 0, judgedRuns = 0, judged = 0;
    Fit fit[kStreams];
    double turnMaxDeg = 0.0, eyeMoveMaxMm = 0.0;

    void addLever(double rho, double qz) noexcept {
        ++leverReads;
        rhoSum += rho;
        qzSum += qz;
        if (rho < rhoMin) rhoMin = rho;
        if (rho > rhoMax) rhoMax = rho;
    }
    void addCall(ReadResult r) noexcept {
        ++calls;
        if (r == ReadResult::kNotScreen) ++notScreen;
        else if (r != ReadResult::kLever) ++unread;
    }
    void merge(const Sec& o) noexcept {
        placements += o.placements;
        screen += o.screen;
        written += o.written;
        writeFailed += o.writeFailed;
        for (int i = 0; i < 3; ++i) {
            refusedLever[i] += o.refusedLever[i];
            refusedTurn[i] += o.refusedTurn[i];
        }
        calls += o.calls;
        leverReads += o.leverReads;
        notScreen += o.notScreen;
        unread += o.unread;
        rhoSum += o.rhoSum;
        qzSum += o.qzSum;
        if (o.rhoMin < rhoMin) rhoMin = o.rhoMin;
        if (o.rhoMax > rhoMax) rhoMax = o.rhoMax;
        if (o.haveCross) {
            haveCross = true;
            cross = o.cross;
        }
        runs += o.runs;
        judgedRuns += o.judgedRuns;
        judged += o.judged;
        for (int s = 0; s < kStreams; ++s) fit[s].merge(o.fit[s]);
        if (o.turnMaxDeg > turnMaxDeg) turnMaxDeg = o.turnMaxDeg;
        if (o.eyeMoveMaxMm > eyeMoveMaxMm) eyeMoveMaxMm = o.eyeMoveMaxMm;
    }
    // What the placement hook counted for one placement: a screen-space panel's refusal or write.
    void addPlacement(bool isScreen, const Pick& pick, bool wrote) noexcept {
        ++placements;
        if (isScreen) ++screen;
        if (pick.refused != Refusal::kNone) ++refusedLever[static_cast<int>(pick.refused) - 1];
        if (pick.write) {
            if (wrote) ++written;
            else ++writeFailed;
        }
    }
    bool judgedSecond() const noexcept { return judged > 0; }
};

enum class TurnWhy : uint8_t { kFed = 0, kNoEye, kNoHrc, kDegenerate };

struct TurnInput {
    uint32_t frame = 0;
    uintptr_t key = 0;             // the panel
    double u = 0.0, v = 0.0;       // the window's uv
    bool haveEye = false, haveHrc = false;
    double E[9] = {}, H[9] = {};   // the eye's view rows and the HUD canvas's rows, as read
    double eye[3] = {};            // the eye's origin, world
    double p[kStreams][3] = {};    // the game's point, F117's (the game's where F117 did not move it), the written one (the game's where nothing was written)
};

class TurnTracker {
public:
    // One screen-space placement. Counts into sec.
    TurnWhy place(const TurnInput& in, Sec& sec) noexcept {
        if (!in.haveEye) {
            ++sec.refusedTurn[0];
            return TurnWhy::kNoEye;
        }
        if (!in.haveHrc) {
            ++sec.refusedTurn[1];
            return TurnWhy::kNoHrc;
        }
        f120::Mat e{}, h{};
        double oe = 0.0, oh = 0.0;
        bool finite = true;
        for (int s = 0; s < kStreams; ++s)
            for (int k = 0; k < 3; ++k) finite = finite && std::isfinite(in.p[s][k]);
        for (int k = 0; k < 3; ++k) finite = finite && std::isfinite(in.eye[k]);
        if (!finite || !f120::unitRows(in.E, e, &oe) || !f120::unitRows(in.H, h, &oh) || !(oe <= f120::kMaxNonOrtho) || !(oh <= f120::kMaxNonOrtho)) {
            ++sec.refusedTurn[2];
            return TurnWhy::kDegenerate;
        }
        const f120::Mat r = f120::mul(e, f120::transpose(h));   // E Ht: the HUD frame's vectors in the eye's frame
        Run* run = nullptr;
        for (Run& c : runs_)
            if (c.valid && c.key == in.key) run = &c;
        const bool cont = run && std::fabs(in.u - run->u) < 1e-6 && std::fabs(in.v - run->v) < 1e-6 && in.frame >= run->frame &&
                          in.frame - run->frame <= f120::kRunGapFrames;
        if (!cont) {
            if (!run) {
                run = &runs_[0];
                for (Run& c : runs_) {
                    if (!c.valid) {
                        run = &c;
                        break;
                    }
                    if (c.frame < run->frame) run = &c;
                }
            }
            *run = Run{};
            run->valid = true;
            run->key = in.key;
            run->u = in.u;
            run->v = in.v;
            run->frame = in.frame;
            run->r0 = r;
            for (int k = 0; k < 3; ++k) run->e0[k] = in.eye[k];
            for (int s = 0; s < kStreams; ++s)
                for (int k = 0; k < 3; ++k) run->p0[s][k] = in.p[s][k];
            ++sec.runs;
            return TurnWhy::kFed;
        }
        run->frame = in.frame;
        const double turn = f120::angleDeg(r, run->r0);
        const double d[3] = {in.eye[0] - run->e0[0], in.eye[1] - run->e0[1], in.eye[2] - run->e0[2]};
        double ed[3];
        f120::apply(e, d, ed);   // the eye's move in its own axes
        const double mm = std::sqrt(ed[0] * ed[0] + ed[1] * ed[1] + ed[2] * ed[2]) * 1000.0;
        if (turn > sec.turnMaxDeg) sec.turnMaxDeg = turn;
        if (mm > sec.eyeMoveMaxMm) sec.eyeMoveMaxMm = mm;
        if (turn < kJudgedTurnDeg) return TurnWhy::kFed;
        const f120::Mat m = f120::mul(r, f120::transpose(run->r0));   // M = R R0t: the rotation that holds a point on the panel
        for (int s = 0; s < kStreams; ++s) {
            double held[3], a[3], b[3];
            f120::apply(m, run->p0[s], held);
            if (rotVec(run->p0[s], held, a) && rotVec(run->p0[s], in.p[s], b)) sec.fit[s].add(a, b);
        }
        ++sec.judged;
        if (!run->judged) {
            run->judged = true;
            ++sec.judgedRuns;
        }
        return TurnWhy::kFed;
    }
    void reset() noexcept { *this = TurnTracker{}; }

private:
    struct Run {
        bool valid = false, judged = false;
        uintptr_t key = 0;
        uint32_t frame = 0;
        double u = 0.0, v = 0.0, e0[3] = {}, p0[kStreams][3] = {};
        f120::Mat r0{};
    } runs_[4];
};

// ---- the lines ----
struct Txt {
    char t[28];
};
inline Txt numText(bool valid, const char* fmt, double v) noexcept {
    Txt x;
    if (valid && std::isfinite(v)) std::snprintf(x.t, sizeof(x.t), fmt, v);
    else std::snprintf(x.t, sizeof(x.t), "n/a");
    return x;
}

// panel cursor turn (F154): window "1 s" or "so far, N s with a judged run". Every number a reader needs to read k, the lever and the refusals is on the line.
inline int formatTurn(char* out, size_t cap, const char* window, const char* setting, const Sec& s) noexcept {
    Txt k[2][kStreams];
    for (int axis = 0; axis < 2; ++axis)
        for (int st = 0; st < kStreams; ++st) k[axis][st] = numText(s.fit[st].valid(axis), "%+.2f", s.fit[st].k(axis));
    const bool haveRho = s.leverReads > 0;
    const Txt rhoMean = numText(haveRho, "%+.3f", haveRho ? s.rhoSum / s.leverReads : 0.0), rhoMin = numText(haveRho, "%+.3f", s.rhoMin),
              rhoMax = numText(haveRho, "%+.3f", s.rhoMax), qz = numText(haveRho, "%+.4f", haveRho ? s.qzSum / s.leverReads : 0.0);
    const Txt rhoFov = numText(s.haveCross, "%.3f", s.cross.rhoFov()), fovC1 = numText(s.haveCross && s.cross.haveC1, "%.2f", s.cross.fovC1 * 180.0 / 3.14159265358979323846),
              fovUi = numText(s.haveCross && s.cross.haveUi, "%.2f", s.cross.fovUi * 180.0 / 3.14159265358979323846),
              mode = numText(s.haveCross && s.cross.haveMode, "%.0f", static_cast<double>(s.cross.mode));
    return std::snprintf(out, cap,
                         "panel cursor turn (F154) %s: setting %s; placements %u: screen-space %u, written %u, refused: no lever %u, tiny rho %u, not finite %u, "
                         "write failed %u, no eye view %u, no HRC matrix %u, degenerate row %u; judged %u placements in %u of %u runs, the head turned %.1f deg at "
                         "most against the HUD frame, the eye moved %.1f mm at most; k yaw: game %s uv %s written %s; k pitch: game %s uv %s written %s; "
                         "rho mean %s min %s max %s from %u owner calls (screen-space %u, not %u, unread %u); rho_fov %s fov_C1 %s deg fov_UI %s deg mode %s; "
                         "q.z %s m",
                         window, setting, s.placements, s.screen, s.written, s.refusedLever[0], s.refusedLever[1], s.refusedLever[2], s.writeFailed,
                         s.refusedTurn[0], s.refusedTurn[1], s.refusedTurn[2], s.judged, s.judgedRuns, s.runs, s.turnMaxDeg, s.eyeMoveMaxMm, k[1][kGame].t, k[1][kUv].t,
                         k[1][kWritten].t, k[0][kGame].t, k[0][kUv].t, k[0][kWritten].t, rhoMean.t, rhoMin.t, rhoMax.t, s.calls, s.leverReads, s.notScreen,
                         s.unread, rhoFov.t, fovC1.t, fovUi.t, mode.t, qz.t);
}

// panel cursor writes (F154): a window's placements and writes, whatever the head did (the turn line is only for seconds with a judged run).
inline int formatWrites(char* out, size_t cap, unsigned seconds, const char* setting, const Sec& s) noexcept {
    return std::snprintf(out, cap,
                         "panel cursor writes (F154) %u s: setting %s; placements %u: screen-space %u, written %u, refused: no lever %u, tiny rho %u, not finite %u, "
                         "write failed %u; owner calls %u (screen-space %u, not %u, unread %u)",
                         seconds, setting, s.placements, s.screen, s.written, s.refusedLever[0], s.refusedLever[1], s.refusedLever[2], s.writeFailed, s.calls,
                         s.leverReads, s.notScreen, s.unread);
}

// One placement in an eye run's window, for matching the dump's cursor quad (its VS constant rows) with the sprite offline.
struct FrameRec {
    bool used = false;
    uint32_t ledger = 0, render = 0;   // the eye run's frame number (the ledger's, as the dump's records carry it) and the native frame
    uintptr_t panel = 0;
    uint8_t setting = 0;
    double u = 0.0, v = 0.0;
    double p[kStreams][3] = {};
    bool wrote = false, haveLever = false;
    double rho = 0.0, q[3] = {};
    bool haveEye = false, haveHrc = false;
    int viewId = 0;
    double E[9] = {}, eye[3] = {}, H[9] = {};
};
inline int rowsText(char* out, size_t cap, bool have, const double v[9]) noexcept {
    if (!have) return std::snprintf(out, cap, "none");
    return std::snprintf(out, cap, "%+.6f %+.6f %+.6f / %+.6f %+.6f %+.6f / %+.6f %+.6f %+.6f", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
}
inline int formatFrame(char* out, size_t cap, const FrameRec& r) noexcept {
    char e[160], h[160], eo[80], lever[120];
    rowsText(e, sizeof(e), r.haveEye, r.E);
    rowsText(h, sizeof(h), r.haveHrc, r.H);
    if (r.haveEye) std::snprintf(eo, sizeof(eo), "%+.5f %+.5f %+.5f", r.eye[0], r.eye[1], r.eye[2]);
    else std::snprintf(eo, sizeof(eo), "none");
    if (r.haveLever) std::snprintf(lever, sizeof(lever), "rho %+.4f q' %+.5f %+.5f %+.5f", r.rho, r.q[0], r.q[1], r.q[2]);
    else std::snprintf(lever, sizeof(lever), "rho none q' none");
    return std::snprintf(out, cap,
                         "panel cursor frame (F154): ledger frame %u render frame %u panel %08X setting %s uv %.5f %.5f; game P %+.5f %+.5f %+.5f, F117 point "
                         "%+.5f %+.5f %+.5f, written %+.5f %+.5f %+.5f (%s); %s; eye view %d rows %s origin %s; HUD rows %s",
                         r.ledger, r.render, static_cast<unsigned>(r.panel & 0xFFFFFFFFu), settingName(static_cast<Setting>(r.setting)), r.u, r.v, r.p[kGame][0],
                         r.p[kGame][1], r.p[kGame][2], r.p[kUv][0], r.p[kUv][1], r.p[kUv][2], r.p[kWritten][0], r.p[kWritten][1], r.p[kWritten][2],
                         r.wrote ? "written" : "not written", lever, r.viewId, e, eo, h);
}

}  // namespace f154
}  // namespace nofoot
}  // namespace edvr
