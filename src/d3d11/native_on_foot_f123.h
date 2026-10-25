// F123 (native on-foot VR): head containment, the leash and the wall contact. Header only, no game or D3D11 state, so native_frame_test runs the very machine the
// frame thread steps. Journal: the arc doc, 2026-10-25, F123. F119 measured what sets the numbers (its flight edvr_gfx_20261025_134757); nothing here has a key.
//
// On foot the game's collision keeps the commander's head out of walls, and EDVR adds the player's physical head translation to the game's camera, so a lean can put
// the eye through a wall (a PvP cheat). Nothing in the game or EDVR stops the eye (F119's flight: the camera follows the head one to one up to the near plane of a
// wall, the character root never moves). Two rules, both on the physical offset or the depth EDVR already measures:
//
//  * The leash, on the physical head's offset from the rest point (EDVR's own eye offsets out, as F119 measures it). Horizontal: the view fades linearly from kSoftCm to
//    black at kHardCm; past kHardCm for kHardHoldS the position is recentred. Vertical, upward only (a crouched character with the player standing up to look over
//    cover): kUpSoftCm to kUpHardCm the same way. The bands come from F119's flight: horizontal normal play p99.5 <= 15 cm (soft 18 = +3, hard 23 = soft + 5, the pilot's
//    numbers); vertical, the p99.5 of the per-second largest upward offset over the 607 seconds outside the leans -0.2 cm, so soft +3 (-0.2 + 3 = 2.8) and hard +8.
//    The ADS eye shift and the other terms of EDVR's alignment are not in the offset: the leash is the head's own motion.
//  * Wall contact, a backstop for any wall: the world's cells (the stencil-marked player cells left out) within the near plane + 3 cm on kContactCells of 64 or more in
//    either eye for kContactFrames consecutive frames. It goes black at once and latches; after kContactHoldS the position is recentred; it releases when the contact has
//    cleared for kClearFrames frames AND the head is back inside the soft radius. It applies in ADS too (the ADS eye shift can bring the eye to a wall).
//
// The position-only recentre: the view fades out over kFadeOutS, the runtime moves the seated origin under the head (its yaw kept exactly, the look compensation
// included), the view fades in over kFadeInS. It never touches the stored seat (F115: kOnFootIsSeated is false) and is not asked at the boarding edge.
//
// The levels combine by max: the leash's, the recentre's and the contact's (and, outside this header, F109's snap fade).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f123 {

constexpr double kSoftCm = 18.0, kHardCm = 23.0;     // the horizontal leash: fade from kSoftCm, black at kHardCm
constexpr double kUpSoftCm = 3.0, kUpHardCm = 8.0;   // the vertical leash, upward only: the head this far above the rest point
constexpr double kHardHoldS = 0.3;                   // past a hard radius this long: the recentre
constexpr double kFadeOutS = 0.1, kFadeInS = 0.2;    // the recentre's fades
constexpr double kWaitMaxS = 0.6;                    // the head must be inside soft by then, or the recentre is said not to have brought it
constexpr double kContactHoldS = 0.3;                // a latched contact this long: the recentre
constexpr double kReleaseS = 0.2;                    // a released contact fades out over this
constexpr double kMinGapS = 0.5;                     // between two recentre requests
constexpr double kAfterS = 1.0;                      // the contact still latched this long after a recentre is said
constexpr double kStaleOffsetS = 0.2;                // an offset older than this is no offset
constexpr uint32_t kContactCells = 32, kContactFrames = 3, kClearFrames = 3;

// The fade level for a value between a soft and a hard edge: 0 up to soft, 1 from hard, linear between.
inline double ramp(double v, double soft, double hard) noexcept {
    if (!(v == v)) return 0.0;
    if (!(hard > soft)) return v >= hard ? 1.0 : 0.0;
    const double t = (v - soft) / (hard - soft);
    return t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
}
inline double leashLevel(double hCm, double upCm) noexcept { return std::max(ramp(hCm, kSoftCm, kHardCm), ramp(upCm, kUpSoftCm, kUpHardCm)); }
inline bool insideSoft(double hCm, double upCm) noexcept { return hCm < kSoftCm && upCm < kUpSoftCm; }
inline bool pastHard(double hCm, double upCm) noexcept { return hCm >= kHardCm || upCm >= kUpHardCm; }

// The contact's trigger and latch, fed one depth readback frame at a time in frame order. cells: the larger count, over the eyes read, of world cells within the near plane +
// 3 cm. A frame no eye was read for is ignored, and the gap its number leaves in the next read frame's restarts the counts (the run must be of consecutive frames).
struct ContactTracker {
    uint32_t streak = 0, clearStreak = 0;
    bool latched = false, have = false;
    uint32_t last = 0;
    // True when this frame latched the contact.
    bool frame(uint32_t frameNo, bool read, uint32_t cells) noexcept {
        if (!read) return false;
        if (have && frameNo != last + 1u) streak = clearStreak = 0;
        have = true;
        last = frameNo;
        if (cells >= kContactCells) {
            ++streak;
            clearStreak = 0;
            if (!latched && streak >= kContactFrames) {
                latched = true;
                return true;
            }
        } else {
            streak = 0;
            ++clearStreak;
        }
        return false;
    }
    bool cleared() const noexcept { return latched && clearStreak >= kClearFrames; }
    void release() noexcept { latched = false; streak = clearStreak = 0; }
    void reset() noexcept { *this = ContactTracker{}; }
};

enum Kind : int { kLeashSoft = 0, kLeashHard = 1, kContact = 2 };
inline const char* kindName(int k) noexcept { return k == kLeashSoft ? "leash soft" : k == kLeashHard ? "leash hard" : "wall contact"; }

// One episode, said when it ends.
struct Episode {
    int kind = kLeashSoft;
    double startS = 0.0, lengthS = 0.0, peakCm = 0.0, peakUpCm = 0.0, blackS = 0.0, pastHardS = 0.0;   // startS: seconds into the on-foot spell
    uint32_t peakCells = 0, weakestCells = 0, farCells = 0, frames = 0;   // the contact's: its largest coverage, the weakest contact frame's, the far cells (over 1 m) up to, the contact frames
    bool recentred = false;
};

struct In {
    double dt = 0.0;
    bool active = false;       // native stereo on foot, the commander's first-person camera: false anywhere else (the boarding edge) resets everything to clear
    bool haveOffset = false;   // a physical head offset was sampled this frame
    double hCm = 0.0, upCm = 0.0;
};

struct Out {
    double level = 0.0;          // how black the view is, 0 to 1
    uint32_t requestWord = 0;    // the position recentre the runtime is asked for: a serial while one stands, else 0
    bool asked = false;          // a recentre was asked this frame
    int askedBy = -1;            // kLeashHard or kContact
    bool done = false;           // the head came inside soft after the ask
    bool failed = false;         // ... or it did not within kWaitMaxS
    double waitedS = 0.0;        // how long the ask took (done or failed)
    bool contactAfter = false;   // the contact is still latched kAfterS after a recentre
    int ended = 0;
    Episode episode[2];
};

class Machine {
public:
    // Whether the camera is the commander's first-person one on foot, said before the frame's depth readbacks are fed (the step says it again): inactive clears everything.
    void gate(bool active) noexcept {
        if (!active) {
            if (active_) reset();
        } else {
            active_ = true;
        }
    }
    // A depth readback frame (in frame order). farCells: the larger count, over the eyes read, of world cells farther than 1 m on this frame.
    void contactFrame(uint32_t frameNo, bool read, uint32_t cells, uint32_t farCells_) noexcept {
        if (!active_) return;
        if (contact_.frame(frameNo, read, cells) && !cEp_.open) beginContact();
        if (read && cEp_.open && cells >= kContactCells) {
            ++cEp_.e.frames;
            cEp_.e.peakCells = std::max(cEp_.e.peakCells, cells);
            cEp_.e.weakestCells = cEp_.e.frames == 1u ? cells : std::min(cEp_.e.weakestCells, cells);
            cEp_.e.farCells = std::max(cEp_.e.farCells, farCells_);
        }
    }
    Out step(const In& in) noexcept {
        Out o;
        const double dt = in.dt > 0.0 && in.dt < 0.25 ? in.dt : 0.0;
        gate(in.active);
        if (!in.active) return o;
        t_ += dt;
        // the offset: a fresh one, else the last for kStaleOffsetS, else none
        if (in.haveOffset) {
            h_ = in.hCm;
            up_ = in.upCm;
            offsetAge_ = 0.0;
            offsetKnown_ = true;
        } else {
            offsetAge_ += dt;
            if (offsetAge_ > kStaleOffsetS) offsetKnown_ = false;
        }
        const double leash = offsetKnown_ ? leashLevel(h_, up_) : 0.0;
        const bool hard = offsetKnown_ && pastHard(h_, up_);
        const bool inside = offsetKnown_ && insideSoft(h_, up_);
        pastHardS_ = hard ? pastHardS_ + dt : 0.0;
        // the leash's episode
        if (leash > 0.0 && !lEp_.open) beginLeash();
        if (lEp_.open) {
            lEp_.e.peakCm = std::max(lEp_.e.peakCm, offsetKnown_ ? h_ : 0.0);
            lEp_.e.peakUpCm = std::max(lEp_.e.peakUpCm, offsetKnown_ ? up_ : 0.0);
            if (leash >= 1.0) lEp_.e.kind = kLeashHard;
            lEp_.e.pastHardS = std::max(lEp_.e.pastHardS, pastHardS_);
        }
        // the contact: latched since, then released
        if (contact_.latched) {
            latchedS_ += dt;
            relS_ = 0.0;
            releasing_ = false;
            if (contact_.cleared() && inside) {
                contact_.release();
                latchedS_ = 0.0;
                releasing_ = true;
                contactAsked_ = false;
            }
        } else if (releasing_) {
            relS_ += dt;
            if (relS_ >= kReleaseS) releasing_ = false;
        }
        double contactLevel = contact_.latched ? 1.0 : releasing_ ? std::max(0.0, 1.0 - relS_ / kReleaseS) : 0.0;
        // the recentre
        sinceAskS_ += dt;
        switch (phase_) {
            case kIdle:
                if (pastHardS_ >= kHardHoldS && sinceAskS_ >= kMinGapS) begin(kLeashHard);
                else if (contact_.latched && !contactAsked_ && latchedS_ >= kContactHoldS && sinceAskS_ >= kMinGapS) begin(kContact);
                break;
            case kFadeOut:
                phaseS_ += dt;
                if (phaseS_ >= kFadeOutS) {
                    ++serial_;
                    pending_ = true;
                    phase_ = kWait;
                    phaseS_ = 0.0;
                    o.asked = true;
                    o.askedBy = cause_;
                }
                break;
            case kWait:
                phaseS_ += dt;
                if (inside && phaseS_ >= kSettleS) {
                    pending_ = false;
                    phase_ = kFadeIn;
                    o.done = true;
                    o.waitedS = phaseS_;
                    phaseS_ = 0.0;
                    pastHardS_ = 0.0;
                    afterS_ = 0.0;
                    watchAfter_ = true;
                    markRecentred();
                } else if (phaseS_ >= kWaitMaxS) {
                    pending_ = false;
                    phase_ = kFadeIn;
                    o.failed = true;
                    o.waitedS = phaseS_;
                    phaseS_ = 0.0;
                }
                break;
            case kFadeIn:
                phaseS_ += dt;
                if (phaseS_ >= kFadeInS) {
                    phase_ = kIdle;
                    phaseS_ = 0.0;
                }
                break;
        }
        const double seq = phase_ == kFadeOut ? std::min(1.0, phaseS_ / kFadeOutS) : phase_ == kWait ? 1.0 : phase_ == kFadeIn ? std::max(0.0, 1.0 - phaseS_ / kFadeInS) : 0.0;
        if (watchAfter_) {
            afterS_ += dt;
            if (afterS_ >= kAfterS) {
                watchAfter_ = false;
                o.contactAfter = contact_.latched;
            }
        }
        o.level = std::max(leash, std::max(seq, contactLevel));
        o.requestWord = pending_ ? serial_ : 0u;
        // black time for the open episodes, and the episodes that end
        if (o.level >= 0.999) {
            if (lEp_.open) lEp_.e.blackS += dt;
            if (cEp_.open) cEp_.e.blackS += dt;
        }
        if (lEp_.open) lEp_.e.lengthS += dt;
        if (cEp_.open) cEp_.e.lengthS += dt;
        if (lEp_.open && leash <= 0.0 && phase_ == kIdle) {
            o.episode[o.ended++] = lEp_.e;
            lEp_ = Open{};
        }
        if (cEp_.open && !contact_.latched && !releasing_ && phase_ == kIdle) {
            o.episode[o.ended++] = cEp_.e;
            cEp_ = Open{};
        }
        level_ = o.level;
        return o;
    }
    double level() const noexcept { return level_; }
    bool pending() const noexcept { return pending_; }
    bool holdFrozen() const noexcept { return phase_ == kFadeOut || phase_ == kWait; }   // the standing-height hold waits while the origin is about to move
    bool contactLatched() const noexcept { return contact_.latched; }
    uint32_t serial() const noexcept { return serial_; }
    // Everything clear, any ask withdrawn; the serial goes on from where it was (the runtime tells a new ask by a new serial).
    void reset() noexcept {
        const uint32_t serial = serial_;
        *this = Machine{};
        serial_ = serial;
    }

private:
    enum Phase : int { kIdle, kFadeOut, kWait, kFadeIn };
    static constexpr double kSettleS = 0.03;   // the offset read after the ask is the runtime's answer only from the frame after
    struct Open {
        bool open = false;
        Episode e;
    };
    void beginLeash() noexcept {
        lEp_ = Open{};
        lEp_.open = true;
        lEp_.e.kind = kLeashSoft;
        lEp_.e.startS = t_;
    }
    void beginContact() noexcept {
        cEp_ = Open{};
        cEp_.open = true;
        cEp_.e.kind = kContact;
        cEp_.e.startS = t_;
        latchedS_ = 0.0;
    }
    void begin(int cause) noexcept {
        phase_ = kFadeOut;
        phaseS_ = 0.0;
        cause_ = cause;
        sinceAskS_ = 0.0;
        if (cause == kContact) contactAsked_ = true;   // once for a latch: a camera that is itself at the wall is not helped by another
    }
    void markRecentred() noexcept {
        if (lEp_.open) lEp_.e.recentred = true;
        if (cEp_.open) cEp_.e.recentred = true;
    }
    bool active_ = false, offsetKnown_ = false, pending_ = false, releasing_ = false, watchAfter_ = false, contactAsked_ = false;
    double t_ = 0.0, h_ = 0.0, up_ = 0.0, offsetAge_ = 0.0, pastHardS_ = 0.0, latchedS_ = 0.0, relS_ = 0.0, phaseS_ = 0.0, sinceAskS_ = 1e9, afterS_ = 0.0, level_ = 0.0;
    int phase_ = kIdle, cause_ = kLeashHard;
    uint32_t serial_ = 0;
    ContactTracker contact_;
    Open lEp_, cEp_;
};

// The comfort fade has one signal and two publishers (F109's snap turn and this): each sets its own level, the level sent is their max, once a frame. A frame with both clear
// publishes nothing, except the first one after a level, which publishes 0 (a reader that was last told 0.4 would otherwise decay it).
struct FadeMix {
    double snap = 0.0, contain = 0.0;
    bool published = false;
    // The level to publish this frame, or -1 when there is nothing to say.
    double next() noexcept {
        const double lvl = std::max(snap > 0.0 ? snap : 0.0, contain > 0.0 ? contain : 0.0);
        if (lvl > 0.0 || published) {
            published = lvl > 0.0;
            return lvl > 1.0 ? 1.0 : lvl;
        }
        return -1.0;
    }
};

// ---- the lines -------------------------------------------------------------------------------------------------------------------------------------
inline int formatSession(char* out, size_t cap) noexcept {
    return std::snprintf(out, cap,
                         "native on foot head containment (F123): the leash and the wall contact are on (no key), native on foot only; leash on the physical head offset from the rest "
                         "point: horizontal fade from %.0f cm to black at %.0f cm, upward from %.0f cm to black at %.0f cm; past a hard radius for %.1f s, or a latched contact for "
                         "%.1f s, a position-only recentre (fade out %.1f s, the origin under the head with its yaw kept, fade in %.1f s); contact: world cells within the near plane + 3 cm "
                         "on %u of 64 or more in either eye for %u frames, black at once, released when it has cleared for %u frames and the head is inside the soft radius",
                         kSoftCm, kHardCm, kUpSoftCm, kUpHardCm, kHardHoldS, kContactHoldS, kFadeOutS, kFadeInS, static_cast<unsigned>(kContactCells), static_cast<unsigned>(kContactFrames),
                         static_cast<unsigned>(kClearFrames));
}
inline int formatEpisode(char* out, size_t cap, const Episode& e, double beganS, bool ads) noexcept {
    if (e.kind == kContact)
        return std::snprintf(out, cap,
                             "native on foot head containment (F123): wall contact episode: began %.2f s into this on-foot spell, lasted %.2f s, black %.2f s%s; peak coverage %u of 64 cells "
                             "(world cells within the near plane + 3 cm), weakest contact frame %u cells, far cells (over 1 m) up to %u, contact frames %u; recentred %s",
                             beganS, e.lengthS, e.blackS, ads ? ", ADS at the end" : "", static_cast<unsigned>(e.peakCells), static_cast<unsigned>(e.weakestCells),
                             static_cast<unsigned>(e.farCells), static_cast<unsigned>(e.frames), e.recentred ? "yes" : "no");
    return std::snprintf(out, cap,
                         "native on foot head containment (F123): %s episode: began %.2f s into this on-foot spell, lasted %.2f s, black %.2f s%s; peak head offset %.1f cm horizontal "
                         "and %+.1f cm up, %.2f s past a hard radius; recentred %s",
                         kindName(e.kind), beganS, e.lengthS, e.blackS, ads ? ", ADS at the end" : "", e.peakCm, e.peakUpCm, e.pastHardS, e.recentred ? "yes" : "no");
}
inline int formatAsk(char* out, size_t cap, uint32_t serial, int by, double hCm, double upCm, uint32_t cells) noexcept {
    return std::snprintf(out, cap,
                         "native on foot head containment (F123): position recentre %u asked by %s: the head %.1f cm horizontal and %+.1f cm up from the rest point, contact %u of 64 cells",
                         static_cast<unsigned>(serial), by == kContact ? "a latched wall contact" : "the leash past its hard radius", hCm, upCm, static_cast<unsigned>(cells));
}
inline int formatResult(char* out, size_t cap, uint32_t serial, bool done, double waitedS, double hCm, double upCm) noexcept {
    if (done)
        return std::snprintf(out, cap, "native on foot head containment (F123): position recentre %u done after %.2f s: the head is %.1f cm horizontal and %+.1f cm up from the new rest point (inside soft)",
                             static_cast<unsigned>(serial), waitedS, hCm, upCm);
    return std::snprintf(out, cap,
                         "native on foot head containment (F123): position recentre %u DID NOT bring the head inside soft within %.2f s: the head is still %.1f cm horizontal and %+.1f cm up from the rest point",
                         static_cast<unsigned>(serial), waitedS, hCm, upCm);
}
inline int formatAfter(char* out, size_t cap, uint32_t serial) noexcept {
    return std::snprintf(out, cap, "native on foot head containment (F123): wall contact still present %.0f s after position recentre %u", kAfterS, static_cast<unsigned>(serial));
}

}  // namespace f123
}  // namespace nofoot
}  // namespace edvr
