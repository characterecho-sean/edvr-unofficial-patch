// The pure half of the exposure sharing's engine path: which of the second eye's exposure dispatches are NOT sent to the GPU. No D3D, no game, no
// logging -- exposure_fix.cpp owns the hooks and the copy and feeds this one the facts of each compute dispatch; tools/exposure_chain_test drives it.
//
// WHAT THE CHAIN IS (disassembly of the two compute shaders, docs/eye-brightness.md "The engine path", 2026-10-13). Per eye, in this order:
//   three small draws (252x252 at a 2016x2017 eye) -> CS B8E727E9 -> CS F1FB2EEF -> the eye's tonemap draw
//   B8E727E9  reads the two small textures, writes u0 (an 8-byte min/max of log luminance, fully rewritten each frame) and u1 (a 252x252 luminance
//             map). It has no history.
//   F1FB2EEF  reads t0 (that map), u0, and u1[0] of the eye's own strip (THE ONLY HISTORY in the chain: the adapted exposure, read at the shader's
//             `ld_uav_typed u1` and written back); writes the 6x1 strip u1 and the 384x1 histograms u2, u3, u4.
// Both eyes' chains run back to back (B8 F1 B8 F1), each followed by that eye's tonemap, whose VS reads strip[0] (`vs_2D78DC3F`: sample_l t0 at 0,0).
//
// WHAT TODAY'S FIX DOES: after the second F1FB2EEF, CopyResource eye 0's u0-u3 over eye 1's. Eye 1's own chain therefore ran for nothing: everything
// it wrote that anything reads was overwritten, and its history (strip[0]) is overwritten with eye 0's every frame, so it never feeds back.
// The engine path does not run it: the second B8E727E9 and the second F1FB2EEF are not forwarded, and the same copy (now u0-u4, the fifth
// UAV included) lands at the point where the second F1FB2EEF would have run. The copy stays -- see exposure_fix.cpp's header for why it is a copy
// and not a binding substitution.
//
// FAIL-SAFE. Nothing is skipped until the chain has been seen, whole and unchanged, for kLearnEyes eye-chains: the predecessor of the exposure
// shader is learned (not hard-coded: the shader hashes change with every game build) as "the compute dispatch immediately before it that shares its
// UAV slot 0", the exposure shader must bind exactly UAV slots 0-4, and the copy must have covered all five. Any later disagreement runs that
// frame's chain as stock (counted as a fallback and said once); a disagreement that could leave the second eye unrefreshed latches the engine off
// for the session. Running the second chain is always safe: the copy lands after it exactly as before.
#pragma once

#include <cstdint>

namespace edvr {
namespace exposurechain {

// The exposure shader's UAV slots: u0 bounds, u1 strip, u2/u3 histograms, u4 the cumulative histogram. Nothing above u4 is bound.
constexpr uint32_t kTargetUavMask = 0x1Fu;
// The predecessor binds u0 and u1 (the bounds and the luminance map); what else it leaves bound is no concern of this.
constexpr uint32_t kPreUavNeed = 0x03u;
constexpr uint32_t kShareSlots = 5;          // u0..u4: the copy covers all five
// Eye-chains seen whole before the skip may engage: five frames of two, the same five the detection already asks for.
constexpr uint32_t kLearnEyes = 10;
// The GPU cost of one second-eye chain, in milliseconds, from the 2026-10-13 pass profile (edvr_gfx_20261013_192348.log, 24 frames):
// F1FB2EEF 0.113 and B8E727E9 0.037. The three small draws (0.0095) are NOT skipped. A constant: the line that reports a saving says so.
constexpr double kTargetMs = 0.113;
constexpr double kPreMs = 0.037;

enum class Action : uint8_t { Run, Skip };

// Why the engine is not (or no longer) skipping. Each is said once by the caller.
enum class Why : uint8_t {
    None = 0,            // engaged, or still learning
    Disabled,            // not the VR profile, or the fix rejected itself: stock, both chains run
    Learning,            // not yet kLearnEyes whole chains
    TargetShape,         // the exposure shader does not bind exactly u0-u4
    NoPredecessor,       // the dispatch before the exposure shader does not share its u0
    CopyShort,           // the copy did not cover all five slots
    LatchedUnfollowed,   // a second-eye predecessor was skipped and the second exposure dispatch never came
    LatchedCopy          // the copy came up short after a skip
};

// The words a log line uses for a reason (a fixed set, no formatting).
inline const char* whyText(Why w) {
    switch (w) {
        case Why::None:              return "engaged or counting";
        case Why::Disabled:          return "not the VR profile, or the fix rejected itself";
        case Why::Learning:          return "still learning the chain";
        case Why::TargetShape:       return "the exposure shader does not bind exactly UAV slots 0-4";
        case Why::NoPredecessor:     return "the dispatch before the exposure shader (or its u0) is not as learned";
        case Why::CopyShort:         return "the copy did not cover all five slots";
        case Why::LatchedUnfollowed: return "a skipped predecessor was not followed by the second exposure dispatch (stood down for the session)";
        case Why::LatchedCopy:       return "the copy came up short after a skip (stood down for the session)";
    }
    return "unknown";
}

struct Facts {
    uint64_t  hash = 0;        // the compute shader's content hash (0 = unknown)
    bool      isTarget = false;   // exposure_fix's own classifier says this is the exposure shader
    uintptr_t u0 = 0;          // UAV slot 0's view identity at this dispatch
    uint32_t  uavMask = 0;     // bit i set when UAV slot i is bound (non-null), slots 0-7
};

struct Decision {
    Action action = Action::Run;
    bool   fallback = false;   // engaged, but this occurrence ran as stock because a precondition failed
    Why    why = Why::None;    // set with fallback, and when learning gives up
};

class Chain {
public:
    // frameConfirmed: exposure_fix's own "runs exactly twice a frame for kConfirmFrames frames, or pinned". active: the fix is on and has not
    // rejected itself. Both are read at each dispatch; they cost nothing here.
    Decision onDispatch(const Facts& f, bool active, bool frameConfirmed) {
        Decision d;
        if (!active) {
            d.why = Why::Disabled;
            note(f);
            return d;
        }
        if (latched_ != Why::None) {
            d.why = latched_;
            note(f);
            return d;
        }
        if (f.isTarget) {
            ++targetSeen_;
            // Whether the engine was engaged BEFORE this occurrence taught it anything: the occurrence that completes the learning is not a
            // fallback, the chain before it ran whole by design.
            const bool wasEngaged = engaged(frameConfirmed);
            learn(f);
            const bool second = targetSeen_ == 2;
            if (second) eye1U0_ = f.u0; else if (targetSeen_ == 1) eye0U0_ = f.u0;
            if (second && (wasEngaged || preSkipped_)) {
                if (!preSkipped_) {
                    d.fallback = true;
                    d.why = Why::NoPredecessor;   // the predecessor ran: so the chain runs whole, copy after
                    ++fallbacks_;
                } else if (f.uavMask != kTargetUavMask) {
                    // The predecessor was already skipped and this one cannot be: the inputs it would read are stale. It runs (the copy lands
                    // after it, as in stock), and the engine stands down for the session rather than do that every frame.
                    d.fallback = true;
                    d.why = Why::TargetShape;
                    latched_ = Why::TargetShape;
                    ++fallbacks_;
                } else {
                    d.action = Action::Skip;
                    targetSkipped_ = true;
                    ++skippedTarget_;
                }
            }
        } else if (preHash_ != 0 && f.hash == preHash_) {
            ++preSeen_;
            if (preSeen_ == 2 && engaged(frameConfirmed)) {
                // The second predecessor: only in the order B8 F1 B8 (one exposure dispatch behind us), on the second eye's own u0, and binding
                // the two UAVs it writes. Anything else runs the chain whole.
                if (targetSeen_ == 1 && eye1U0_ != 0 && f.u0 == eye1U0_ && (f.uavMask & kPreUavNeed) == kPreUavNeed) {
                    d.action = Action::Skip;
                    preSkipped_ = true;
                    ++skippedPre_;
                } else {
                    d.fallback = true;
                    d.why = Why::NoPredecessor;
                    ++fallbacks_;
                }
            }
        }
        note(f);
        return d;
    }

    // After the copy that follows the second exposure occurrence (run or skipped): how many of the five slots it covered. The learning gate and
    // the latch both read it.
    void onShareCopied(uint32_t slotsCopied, bool skipped) {
        if (skipped) {
            if (slotsCopied < kShareSlots && latched_ == Why::None) {
                latched_ = Why::LatchedCopy;
                ++fallbacks_;
            }
        } else {
            copyOk_ = slotsCopied >= kShareSlots;
        }
    }

    // Once a frame (Present). A skipped predecessor with no second exposure dispatch behind it left the second eye unrefreshed for a frame: latch.
    // The frame's learning also lands here: a frame with exactly two whole chains counts.
    void frameBoundary() {
        if (preSkipped_ && !targetSkipped_ && targetSeen_ < 2 && latched_ == Why::None) {
            latched_ = Why::LatchedUnfollowed;
            ++fallbacks_;
        }
        preSeen_ = 0;
        targetSeen_ = 0;
        preSkipped_ = false;
        targetSkipped_ = false;
        lastHash_ = 0;
        lastU0_ = 0;
        lastMask_ = 0;
    }

    bool engagedNow(bool frameConfirmed) const { return latched_ == Why::None && engaged(frameConfirmed); }
    bool learning() const { return preStreak_ < kLearnEyes; }
    Why latched() const { return latched_; }
    // Why learning has not engaged: TargetShape and NoPredecessor are the two a log line can name; None while it is still counting.
    Why learnVerdict() const { return learnWhy_; }
    uint64_t preHash() const { return preHash_; }
    uint32_t preStreak() const { return preStreak_; }
    uint64_t skippedPre() const { return skippedPre_; }
    uint64_t skippedTarget() const { return skippedTarget_; }
    uint64_t fallbacks() const { return fallbacks_; }
    double savedMs() const { return skippedPre_ * kPreMs + skippedTarget_ * kTargetMs; }
    bool preSkippedThisFrame() const { return preSkipped_; }

private:
    bool engaged(bool frameConfirmed) const {
        return frameConfirmed && preHash_ != 0 && preStreak_ >= kLearnEyes && copyOk_ && learnWhy_ == Why::None && eye0U0_ != 0 &&
               eye1U0_ != 0 && eye0U0_ != eye1U0_;
    }

    // Every exposure occurrence is a lesson, whichever eye it is: the dispatch right before it must be a different shader that shares u0 and binds
    // u0/u1, and the exposure shader itself must bind exactly u0-u4.
    void learn(const Facts& f) {
        if (f.uavMask != kTargetUavMask) {
            learnWhy_ = Why::TargetShape;
            preStreak_ = 0;
            return;
        }
        const bool pre = lastHash_ != 0 && lastHash_ != f.hash && lastU0_ != 0 && lastU0_ == f.u0 &&
                         (lastMask_ & kPreUavNeed) == kPreUavNeed;
        if (!pre) {
            learnWhy_ = Why::NoPredecessor;
            preStreak_ = 0;
            return;
        }
        learnWhy_ = Why::None;
        if (preHash_ != lastHash_) {
            preHash_ = lastHash_;
            preStreak_ = 1;
        } else if (preStreak_ < kLearnEyes) {
            ++preStreak_;
        }
    }

    void note(const Facts& f) {
        lastHash_ = f.hash;
        lastU0_ = f.u0;
        lastMask_ = f.uavMask;
    }

    uint64_t  preHash_ = 0;
    uint32_t  preStreak_ = 0;
    Why       learnWhy_ = Why::Learning;
    bool      copyOk_ = false;
    Why       latched_ = Why::None;

    uint32_t  preSeen_ = 0;
    uint32_t  targetSeen_ = 0;
    bool      preSkipped_ = false;
    bool      targetSkipped_ = false;
    uintptr_t eye0U0_ = 0;
    uintptr_t eye1U0_ = 0;
    uint64_t  lastHash_ = 0;
    uintptr_t lastU0_ = 0;
    uint32_t  lastMask_ = 0;

    uint64_t  skippedPre_ = 0;
    uint64_t  skippedTarget_ = 0;
    uint64_t  fallbacks_ = 0;
};

}  // namespace exposurechain
}  // namespace edvr
