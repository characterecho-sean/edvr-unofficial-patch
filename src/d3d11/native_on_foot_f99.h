// F99a (native on-foot VR): a drawn item's hands from the game's own selection, not from Status.json. Header only, no game or D3D11 state, so native_frame_test
// runs the very functions the frame thread uses. Journal: the arc doc, 2026-10-24, F99a.
//
// The F98 flight (edvr_gfx_20261024_192353): drawing a tool while moving, the tool appeared in the hand's place and the hand reached it noticeably later. The hands'
// solve authority is the armed flag and its 0.3 s ramp (F66a); for a tool or the pistol the flag came from Status.json's SelectedWeapon only, which the game wrote
// 288-686 ms after its own selection (F83's native read: companalyser 19:27:01.132 against Status.json 19:27:01.818, repairtool 09.377 against 09.809,
// rechargetool 35.705 against 36.057 and 19:29:35.314 against 35.602); the FP hand had begun the draw 50 ms after the game's selection and held the tool at its
// place 0.18 s before Status.json named it, and the hand's onset followed Status.json by 0-15 ms, at full weight 297 ms later. Only the rifle (a primary, F83's
// draw follow) armed from the game's selection, and its hand was on the rifle before Status.json named it. Standing, the game's own 3P draw kept the hand near the
// FP hand while it waited (the distance gate held 74 of 77 unarmed frames); moving, the locomotion kept it away (13 of 62, 34 of 57, 30 of 45): the wait showed.
//
// The bridge: a new selection of an item (any slot: a tool, the pistol, a weapon) made by the game while Status.json still names none arms the hands at once, until
// Status.json names an item (it then arms them as before), the game puts the item away, or kBridgeMaxS passes without Status.json naming one (released through the
// same 0.3 s ramp). The first selection read after the hook comes up, or after the scope opens again, is not a draw. Holstering and swaps keep Status.json's timing.
#pragma once

#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f99 {

constexpr double kBridgeMaxS = 2.0;      // Status.json named the item 0.29-1.39 s after the game's selection in the F98 flight (swaps the longest)
constexpr uint32_t kNoItem = 0xFFFFFFFFu; // the game's selection with nothing drawn (the fists)
constexpr double kOnsetMaxS = 3.0;        // a draw's record closes this long after it opened, complete or not

struct BridgeIn {
    bool active = false;      // native stereo on foot
    bool nativeOk = false;    // the game's selection read this tick: fresh, readable, not suspended
    uint32_t nativeId = kNoItem;
    bool nativeHeld = false;  // the selection is an item (not the holster, not kNoItem)
    bool statusHeld = false;  // Status.json names a weapon or tool
    double dt = 0.0;
};
struct BridgeOut {
    bool armed = false;       // the hands' authority from the game's selection this tick
    bool started = false, confirmed = false, expired = false, putAway = false;
};
struct Bridge {
    bool have = false, bridging = false, lastHeld = false;
    uint32_t lastId = kNoItem;
    double age = 0.0;
    void reset() noexcept { *this = Bridge{}; }
    BridgeOut step(const BridgeIn& in) noexcept {
        BridgeOut o;
        if (!in.active) {
            reset();
            return o;
        }
        if (in.nativeOk) {
            const bool changed = have && (in.nativeId != lastId || in.nativeHeld != lastHeld);
            have = true;
            lastId = in.nativeId;
            lastHeld = in.nativeHeld;
            if (changed && in.nativeHeld && !in.statusHeld) {
                bridging = true;
                age = 0.0;
                o.started = true;
            } else if (changed && !in.nativeHeld && bridging) {
                bridging = false;
                o.putAway = true;
            }
        }
        if (bridging && !o.started) {
            if (in.statusHeld) {
                bridging = false;
                o.confirmed = true;
            } else {
                age += in.dt > 0.0 ? in.dt : 0.0;
                if (age > kBridgeMaxS) {
                    bridging = false;
                    o.expired = true;
                }
            }
        }
        o.armed = bridging;
        return o;
    }
};

// The instrument: one record per draw (the game's selection of an item while Status.json names none, or Status.json naming one with no such selection seen),
// times from its opening: when Status.json named an item, when the hands' authority came and from where, when the armed ramp was full. Independent of the
// bridge: it reads the selection itself, so a build whose bridge never armed the hands shows the authority from Status.json at the game's lead.
enum Source : int { kFromStatus = 0, kFromSelection = 1, kFromPrimary = 2 };
inline const char* sourceName(int s) noexcept {
    return s == kFromSelection ? "the game's selection" : s == kFromPrimary ? "the native primary selection" : "Status.json";
}
struct OnsetIn {
    bool active = false;
    bool nativeOk = false;
    uint32_t nativeId = kNoItem;
    bool nativeHeld = false;
    bool statusHeld = false;
    bool armed = false;       // the hands' authority this tick (the armed flag)
    int source = kFromStatus; // where it came from this tick
    double w = 0.0;           // the armed ramp
    bool moving = false;      // the body moving (walking or airborne, the pin's thresholds)
    double dt = 0.0;
};
struct OnsetRecord {
    bool native = false;      // opened by the game's selection (else by Status.json)
    uint32_t id = kNoItem;
    bool moving = false;      // the body moving when it opened
    bool statusSeen = false, onsetSeen = false, fullSeen = false;
    int source = kFromStatus;
    double tStatus = 0.0, tOnset = 0.0, tFull = 0.0;   // s from the opening
};
struct Onset {
    bool have = false, open = false, statusPrev = false, lastHeld = false;
    uint32_t lastId = kNoItem;
    double t = 0.0;
    OnsetRecord r;
    void reset() noexcept { *this = Onset{}; }
    // Returns true when a record closed this tick (out holds it).
    bool step(const OnsetIn& in, OnsetRecord* out) noexcept {
        bool closed = false;
        if (!in.active) {
            reset();
            return false;
        }
        if (open) t += in.dt > 0.0 ? in.dt : 0.0;
        const bool statusBecame = in.statusHeld && !statusPrev;
        statusPrev = in.statusHeld;
        bool nativeDraw = false;
        if (in.nativeOk) {
            nativeDraw = have && (in.nativeId != lastId || in.nativeHeld != lastHeld) && in.nativeHeld && !in.statusHeld;
            have = true;
            lastId = in.nativeId;
            lastHeld = in.nativeHeld;
        }
        const auto emit = [&]() {
            if (open && out) {
                *out = r;
                closed = true;
            }
            open = false;
        };
        if (nativeDraw || (statusBecame && !open)) {
            if (open) emit();   // a record still open (another draw before it completed) closes as it stands
            open = true;
            t = 0.0;
            r = OnsetRecord{};
            r.native = nativeDraw;
            r.id = nativeDraw ? in.nativeId : kNoItem;
            r.moving = in.moving;
        }
        if (!open) return closed;
        if (statusBecame && !r.statusSeen) {
            r.statusSeen = true;
            r.tStatus = t;
        }
        if (in.armed && !r.onsetSeen) {
            r.onsetSeen = true;
            r.tOnset = t;
            r.source = in.source;
        }
        if (r.onsetSeen && !r.fullSeen && in.w >= 1.0) {
            r.fullSeen = true;
            r.tFull = t;
        }
        if ((r.statusSeen && r.fullSeen) || t > kOnsetMaxS) emit();
        return closed;
    }
};

}  // namespace f99
}  // namespace nofoot
}  // namespace edvr
