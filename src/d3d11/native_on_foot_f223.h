// F223 (native on-foot VR): a jump of the model root goes into the whole-body move. Header only, no game or D3D11 state, so native_frame_test runs the very functions the pin uses.
//
// What the flight established. Recording 20261106_145502 (build 569e8c49): at each jump pack start with the arc cutter the game moves the model root 1.10 to 1.14 m up the body in one call and lets it
// back over about four more (56:06.19: +1.14, -0.25, -0.34, -0.18, -0.01) while the camera in the world runs smooth, so the camera in the model falls from 1.70 to 0.60 m and returns by 1.61. The
// neck pin chases it, the chest falls to about 0.65, and the left elbow stands 26 to 28 cm over the hand for a call. F215's step is limited to 3 m/s and F217 clamps at 0.6 m, so the whole-body
// move cannot absorb it. Replayed offline over four flights (about 1,050 s) the rule below takes all 8 jump pack events and nothing else, with the camera in the model staying within 1.35 to
// 1.76 m through them; the calls it refuses are the camera's own moves at the start of on foot.
//
// The rule. Per fresh game frame the camera's height in the model (through the unshifted world-to-model matrix) is compared with the last frame's. A change of kJumpM or more while the camera in
// the world, along the body's up, moved less than kCameraShare of it is the root jumping, not the camera: the change is added to pop (held to +-kCapM), which joins F215's shift in force. A
// change that large with the camera moving as much is the camera's own move: refused. Anything else lets pop go back toward 0 at kEaseMps.
//
// F224 (the whole-body move loses its call of lag) lives here too, because it needs this struct's last camera height. Recording 20261106_164037 (build 9e1f85d5): through the rifle's jumps both
// hands bounce against the chest on alternate calls, up to about 2.4 cm at 7.8 m/s, and the elbow follows. The camera's height in the model jitters about 1 cm a call in the air (the game's camera and
// model root do not step together), and F217's y is the last call's target, so it follows that jitter one call late: from 44:07.92 y read -0.086, -0.080, -0.089, -0.083 where the targets read -0.080,
// -0.089, -0.083, -0.091. The camera less y, the height the pin and the hands work in, alternated by about 1.5 cm (1.574, 1.559, 1.575, 1.560), about twice the camera's own. Replayed with the lead
// below, the residual of the camera less the move over 7 calls fell from 9.1 to 2.1 mm in the air, 4.7 to 2.7 mm running and 1.25 to 0.35 mm standing, with no call over the limit in the air.
// The lead is this call's change of the camera in the model when it is under kLeadMaxM and not a root jump; the whole shift is y + pop + lead, so the camera less the shift is the level camera with no
// lag when F215's rate limit is not engaged (y[n+1] is about E[n] - level, the shift for call n+1 is that plus E[n+1] - E[n]). F215's rule still reads the camera as before: internal E plus the
// whole shift is the true camera, so the target and y do not change and the lead stays in the camera the rule reads.
//
// Journal: the arc doc, F223, F224. Behaviour, no key beyond F215's.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f223 {

constexpr double kJumpM = 0.10;        // a change of the camera in the model this large is a jump candidate
constexpr double kCameraShare = 0.5;   // taken when the camera in the world moved less than this share of it
constexpr double kEaseMps = 0.5;       // otherwise pop goes back toward 0 at this speed
constexpr double kCapM = 1.5;          // pop is held to +-this
constexpr double kLeadMaxM = 0.08;     // F224: a change of the camera in the model up to this large (and not a root jump) leads the whole-body move
constexpr double kMaxGapS = 0.1;       // frames further apart than this are not compared, and an ease counts at most this long

struct RootJump {
    double pop = 0.0;
    double lastE = 0.0, lastCam[3] = {}, lastT = 0.0;
    bool have = false;
    bool taken = false, refused = false;   // what the last step did
    double jumpM = 0.0;                    // the change it took (signed)
    double lead = 0.0;                     // F224: this frame's change of the camera when it is a small one (kept by a call that is not new)
    bool leadOver = false;                 // F224: the last step compared two frames and the change was over kLeadMaxM without being taken

    void reset() noexcept { *this = RootJump{}; }

    // fresh: a new game frame since the last step; tS seconds on any monotonic clock; eModelY the camera's height in the model, unshifted; cam the camera in the world; up the body's up in the world.
    // Returns pop.
    double step(bool fresh, double tS, double eModelY, const double cam[3], const double up[3]) noexcept {
        taken = refused = false;
        jumpM = 0.0;
        if (!fresh) return pop;
        leadOver = false;
        const double un = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (!std::isfinite(tS) || !std::isfinite(eModelY) || !std::isfinite(cam[0]) || !std::isfinite(cam[1]) || !std::isfinite(cam[2]) || !std::isfinite(un) || !(un > 1e-9)) {
            have = false;
            lead = 0.0;
            return pop;
        }
        lead = 0.0;
        if (have) {
            const double dt = tS - lastT;
            const double dE = eModelY - lastE;
            const double dC = ((cam[0] - lastCam[0]) * up[0] + (cam[1] - lastCam[1]) * up[1] + (cam[2] - lastCam[2]) * up[2]) / un;
            const bool gapOk = dt > 0.0 && dt < kMaxGapS;
            bool ease = true;
            if (gapOk && std::fabs(dE) >= kJumpM) {
                if (std::fabs(dC) < kCameraShare * std::fabs(dE)) {
                    pop += dE;
                    pop = pop > kCapM ? kCapM : pop < -kCapM ? -kCapM : pop;
                    taken = true;
                    jumpM = dE;
                    ease = false;
                } else {
                    refused = true;
                }
            }
            if (gapOk && !taken) {
                if (std::fabs(dE) <= kLeadMaxM) lead = dE;
                else leadOver = true;
            }
            if (ease && dt > 0.0) {
                const double d = kEaseMps * (dt < kMaxGapS ? dt : kMaxGapS);
                pop = pop > d ? pop - d : pop < -d ? pop + d : 0.0;
            }
        }
        have = true;
        lastE = eModelY;
        for (int i = 0; i < 3; ++i) lastCam[i] = cam[i];
        lastT = tS;
        return pop;
    }
};

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t taken = 0, refused = 0;
    double largestM = 0.0, moveM = 0.0;
    // One step: what it did, and the move in force after it.
    void add(const RootJump& r) noexcept {
        if (r.taken) {
            ++taken;
            if (std::fabs(r.jumpM) > std::fabs(largestM)) largestM = r.jumpM;
        }
        if (r.refused) ++refused;
        moveM = r.pop;
    }
    bool any() const noexcept { return taken + refused > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// F224: one window of fresh calls in an F215 active state: how many, the lead's sum of squares and largest, and the calls over the limit.
struct LeadStats {
    uint32_t active = 0, over = 0;
    double sumSq = 0.0, largestM = 0.0;
    void add(bool f215Active, const RootJump& r) noexcept {
        if (!f215Active) return;
        ++active;
        const double l = f215Active ? r.lead : 0.0;
        sumSq += l * l;
        if (std::fabs(l) > std::fabs(largestM)) largestM = l;
        if (r.leadOver) ++over;
    }
    bool any() const noexcept { return active > 0; }
    void clear() noexcept { *this = LeadStats{}; }
};

// The F224 window's line. tools\edvr_log.py reads it.
inline int formatLeadLine(char* buf, size_t n, uint32_t seconds, const LeadStats& s) noexcept {
    return std::snprintf(buf, n, "native on foot body move lead (F224) %u s: %u active calls, the lead rms %.1f mm, largest %+.1f mm; %u over the limit", seconds, s.active, s.active ? std::sqrt(s.sumSq / s.active) * 1000.0 : 0.0,
                         s.largestM * 1000.0, s.over);
}

// The window's line. tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Stats& s) noexcept {
    return std::snprintf(buf, n, "native on foot root jump (F223) %u s: %u calls taken, the largest %+.2f m; %u refused (the camera itself moved); the move now %+.2f m", seconds, s.taken, s.largestM, s.refused,
                         s.moveM);
}

}  // namespace f223
}  // namespace nofoot
}  // namespace edvr
