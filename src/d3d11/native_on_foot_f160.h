// F160 (native on-foot VR): the view's return from ADS blended through the release, and the holster bridge's line. Header only, no game or D3D11 state, so native_frame_test
// runs the very functions the hook and the frame thread use. Journal: the arc doc, 2026-10-28, F160.
//
// The flight edvr_gfx_20261028_125106 (build 1126ce79), the pilot: "when transitioning out of ADS, there's a delay and then the head moves up". Four F89 release traces
// ("eye - F_root up") are alike. With t = 0 at the request off: the lock weight falls 0.96 -> 0 in 0.29 s (F62b, F73) while the eye goes 2.0 to 4.1 cm down, then the eye sits on a
// plateau at -4.1..-3.8 cm for 0.34 s with no input and the game's camera steady, then at +0.634 s the raise returns, -2.7 -> +7.0 cm in 0.09 s (up to 2.1 cm a frame); the other
// exits started at +0.633, +0.666 and +0.634 s. The neck reference's raise (+11.0 cm, F94a/F98c) is published as 0 while the detach weight is above 0, and the settle window is
// invalid then, so after the 0.3 s release StanceSettle fills a fresh 0.3 s window before it publishes the stance's target; F94a's follower (0.1 s) then moves it at once:
// 0.31 + 0.30 + 0.03 = 0.63 s. The settled target after ADS was +11.7 cm against +11.8 cm before it, so the fresh window buys nothing.
//
// The return. The last settled target published outside ADS is kept with the held item's serial and the stance's class. While the detach weight falls (the release, the
// request off) and the item and the class still match, the target published is (1 - w) * the kept one in place of 0, so the raise comes back as the sights let go; the first
// call after the weight reaches 0 publishes the kept target whole. On the press (the weight rising, or held) the target is 0 as before, and it is 0 when nothing is kept or the
// item or the class changed. The settle window is as it was (invalid in the sights, 0.3 s to fill after): its first settled sample corrects any small difference, and a changed
// stance still re-settles 0.3 s after.
#pragma once

#include "native_on_foot_f99.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f160 {

// ---- the return from ADS ----------------------------------------------------------------------------------------------------------------------------------------------------

enum Why : uint8_t {
    kPress = 0,      // the press or the hold (the weight rising or full), or not in the sights: 0, as before F160
    kBlend = 1,      // the release with a kept target of this item and class: (1 - w) * the kept target
    kNoStored = 2,   // the release with no target kept (no settled stance published since the pin came up)
    kItem = 3,       // the release with another held item than the kept target's
    kStance = 4,     // the release in another stance class than the kept target's
    kWhyN = 5,
    kNone = 0xFF     // no decision this call (not in the sights)
};
inline const char* whyName(int w) noexcept {
    return w == kPress ? "the press" : w == kBlend ? "blended" : w == kNoStored ? "no stored raise" : w == kItem ? "another item" : w == kStance ? "another stance" : "none";
}

struct Stored {
    bool have = false;
    double targetM = 0.0;   // the last settled target published outside ADS (m)
    uint32_t item = 0;      // the held item's serial when it was taken
    int cls = -1;           // the stance class (nofoot::f94::EyeClass) when it was taken
};
struct Result {
    double targetM = 0.0;
    uint8_t why = kPress;
};
// The target to publish for one pin call in the sights or leaving them. detachW: the ADS detach weight (above 0 here); rising: the sights are asked for (the press and the
// hold); item and cls: the held item's serial and the stance class of this call.
inline Result adsReturnTarget(double detachW, bool rising, const Stored& s, uint32_t item, int cls) noexcept {
    Result r;
    if (rising || !std::isfinite(detachW) || !(detachW > 0.0)) return r;
    if (!s.have || !std::isfinite(s.targetM)) {
        r.why = kNoStored;
        return r;
    }
    if (s.item != item) {
        r.why = kItem;
        return r;
    }
    if (s.cls != cls) {
        r.why = kStance;
        return r;
    }
    const double w = detachW > 1.0 ? 1.0 : detachW;
    r.targetM = (1.0 - w) * s.targetM;
    r.why = kBlend;
    return r;
}

// The hook's state (one writer): the kept target, and whether the last call was already a release (so a release is counted once, at its first call).
struct AdsReturn {
    Stored stored;
    bool falling = false;
    void reset() noexcept { *this = AdsReturn{}; }
    // A settled stance's target, as published outside ADS.
    void store(double targetM, uint32_t item, int cls) noexcept {
        if (!std::isfinite(targetM)) return;
        stored.have = true;
        stored.targetM = targetM;
        stored.item = item;
        stored.cls = cls;
    }
    // A call that is not in the sights for a reason other than the release running its course (the game's camera away from the commander).
    void leave() noexcept { falling = false; }
    // The first call after a release ran to its end (the weight back at 0): the formula's last value, the kept target whole, when the item and the class still match. Without
    // it the last blended call (the weight of one tick, 4% at 90 Hz) would hold the raise that much short until the settle window is full 0.3 s later.
    struct Done {
        bool publish = false;
        double targetM = 0.0;
    };
    Done finish(uint32_t item, int cls) noexcept {
        Done d;
        if (!falling) return d;
        falling = false;
        if (stored.have && std::isfinite(stored.targetM) && stored.item == item && stored.cls == cls) {
            d.publish = true;
            d.targetM = stored.targetM;
        }
        return d;
    }
    struct Out {
        Result r;
        bool releaseBegins = false;   // the first call of a release
    };
    // A call in the sights or leaving them (the weight above 0).
    Out step(double detachW, bool rising, uint32_t item, int cls) noexcept {
        Out o;
        o.r = adsReturnTarget(detachW, rising, stored, item, cls);
        const bool release = !rising && std::isfinite(detachW) && detachW > 0.0;
        o.releaseBegins = release && !falling;
        falling = release;
        return o;
    }
};

// The window's figures: the releases (counted at their first call), how each was classed, and the pin calls that published a blend.
struct Counts {
    uint32_t releases = 0, blended = 0, notBlended[3] = {}, callsBlended = 0;   // notBlended: kNoStored, kItem, kStance
    void add(uint8_t why, bool releaseBegins) noexcept {
        if (why == kBlend) ++callsBlended;
        if (!releaseBegins) return;
        ++releases;
        if (why == kBlend) ++blended;
        else if (why >= kNoStored && why <= kStance) ++notBlended[why - kNoStored];
    }
    void merge(const Counts& o) noexcept {
        releases += o.releases;
        blended += o.blended;
        callsBlended += o.callsBlended;
        for (int i = 0; i < 3; ++i) notBlended[i] += o.notBlended[i];
    }
    bool any() const noexcept { return releases || callsBlended; }
};

// ---- the holster bridge's line ----------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint64_t kHolsterLineGapMs = 500;
// One line in 0.5 s: the first passes, the rest are counted and the next to pass says how many were not written.
class LineGate {
public:
    bool take(uint64_t nowMs, uint32_t* skippedBefore) noexcept {
        if (have_ && nowMs >= lastMs_ && nowMs - lastMs_ < kHolsterLineGapMs) {
            if (skipped_ < 0xFFFFFFFFu) ++skipped_;
            return false;
        }
        if (skippedBefore) *skippedBefore = skipped_;
        skipped_ = 0;
        have_ = true;
        lastMs_ = nowMs;
        return true;
    }

private:
    bool have_ = false;
    uint64_t lastMs_ = 0;
    uint32_t skipped_ = 0;
};

inline const char* holsterEndName(int e) noexcept {
    return e == f99::kHolsterStatus ? "Status.json named no item" : e == f99::kHolsterDrawn ? "the game selected an item again" : "the 4 s limit";
}

// One bridge: the held item's name (Status.json's, at the start), the id the game had selected, how long it ran and how it ended. A bridge that ended by Status.json ran
// the game's lead over Status.json (the hands were unarmed that much sooner); one that ended otherwise ran at least that long.
struct HolsterLine {
    char item[64] = {};
    uint32_t fromId = f99::kNoItem;
    uint8_t end = f99::kHolsterStatus;
    double ranMs = 0.0;
    uint32_t skipped = 0;   // bridges since the last line that the rate limit kept out
};
inline int formatHolster(char* buf, size_t n, const HolsterLine& h) noexcept {
    int len = std::snprintf(buf, n,
                            "native on foot holster bridge (F160): item %s; the game's selection read none after id %08X while Status.json still named the item; the "
                            "bridge ran %.0f ms and ended: %s",
                            h.item[0] ? h.item : "none", h.fromId, h.ranMs, holsterEndName(h.end));
    if (len < 0 || static_cast<size_t>(len) >= n) return len;
    if (h.skipped) {
        const int add = std::snprintf(buf + len, n - static_cast<size_t>(len), "; %u earlier bridge(s) not logged (one line in 0.5 s)", h.skipped);
        if (add > 0) len += add;
    }
    return len;
}

}  // namespace f160
}  // namespace nofoot
}  // namespace edvr
