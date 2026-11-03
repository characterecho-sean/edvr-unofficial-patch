#pragma once
// The SRV turret view's early edge (issue 179; docs/srv-turret-hud-2026-10-30.md, "2026-11-03: the early edge"): the pure half, shared by srv_turret.cpp's frame tick and run by
// tools/srv_turret_entry_fade_test.
//
// THE FAULT. Status.json's turret-view bit lags the game: the game's own fade to black has ended and the image is back, with the camera unmoved, before the bit lands (Sean's eye dump
// 144324256001: black 12216-12219, the game fading in with the camera and the HUD unmoved 12222-12227, the jump at 12228, the frame the view went on). A hold that starts at the
// Status.json edge cannot hide that. What is needed is an earlier edge.
//
// THE SIGNAL. The gunner sheet's own draw (vs A888D510 or vs 5DAD41AA at 2016 indices, srv_turret_math.h isGunnerSheetSignature) in an eye pass, with the commander in the Scorpion. The
// census of the black frames has none, so it is absent in the fade's dark frames and expected to start when the turret's HUD appears. That is a hypothesis the next flight tests: the
// latch's log lines (Status.json agreed N ms after the sheet; the latch expired) show the lag and any false positive.
//
// THE LATCH. While Status.json has the view off, the Scorpion is known and a sheet draw was seen within kExpireMs, the view is on. Status.json reporting the turret takes over with no
// edge (the latch clears, `on` stays true). A latch with no sheet draw for kExpireMs and Status.json still off clears (a false positive turns off again). Turning off stays
// Status-driven: a sheet that stops does not turn the view off while Status.json says on, and a Status.json off edge while the sheet is still drawing latches again on its next draw. A raise needs a sheet draw NEWER than the last step that saw Status.json on: on leaving
// the turret the sheet stops when the game's fade starts and Status.json flips off half a second or more later, and a draw from before that off edge must not raise the view again (it
// would hold the camera moved back until the latch expired, a second after the last draw). The clock is GetTickCount64, so a draw in the same tick waits for a later one.
#include <cstdint>

namespace edvr {
namespace srv_turret {
namespace sheetlatch {

// A sheet draw not seen for this long, with Status.json still off, ends the latch.
constexpr uint64_t kExpireMs = 1000;

enum class Event : uint8_t {
    kNone,
    kRaised,        // the latch came on this step: the view is on before Status.json says so
    kStatusAgreed,  // Status.json reported the turret while the latch was on: lagMs after the raise
    kExpired,       // no sheet draw for kExpireMs with Status.json still off
    kDropped        // the journal stopped calling the Scorpion while the latch was on
};

struct Input {
    uint64_t nowMs = 0;        // a monotonic clock, milliseconds (the same one that stamps sheetSeenMs)
    bool statusOn = false;     // Status.json's turret-view bit (journalSrvTurretView)
    bool scorpion = false;     // the journal calls the vehicle the Scorpion (journalSrvScorpion; unknown is false)
    uint64_t sheetSeenMs = 0;  // when a sheet draw was last seen in an eye pass; 0 = never
};

struct Result {
    bool on = false;           // the view is on: Status.json's bit and the Scorpion, or the latch
    Event event = Event::kNone;
    uint64_t lagMs = 0;        // kStatusAgreed: ms from the raise; kExpired and kDropped: ms the latch held
};

class Latch {
public:
    Result step(const Input& in) {
        Result r;
        const bool fresh = in.sheetSeenMs != 0 && in.nowMs >= in.sheetSeenMs && in.nowMs - in.sheetSeenMs <= kExpireMs;
        const bool newer = in.sheetSeenMs > lastStatusOnMs_;   // a draw after the last step that saw Status.json on
        if (!in.scorpion) {
            if (latched_) {
                r.event = Event::kDropped;
                r.lagMs = in.nowMs - raisedMs_;
            }
            latched_ = false;
            r.on = false;   // a non-Scorpion SRV is never the early edge's, and Status.json's bit alone is not enough either: the Scorpion only
            return r;
        }
        if (in.statusOn) {
            if (latched_) {
                r.event = Event::kStatusAgreed;
                r.lagMs = in.nowMs - raisedMs_;
            }
            latched_ = false;
            lastStatusOnMs_ = in.nowMs;
            r.on = true;
            return r;
        }
        if (latched_ && !fresh) {
            r.event = Event::kExpired;
            r.lagMs = in.nowMs - raisedMs_;
            latched_ = false;
        } else if (!latched_ && fresh && newer) {
            latched_ = true;
            raisedMs_ = in.nowMs;
            r.event = Event::kRaised;
        }
        r.on = latched_;
        return r;
    }
    bool latched() const { return latched_; }
    void reset() {
        latched_ = false;
        lastStatusOnMs_ = 0;
    }

private:
    bool latched_ = false;
    uint64_t raisedMs_ = 0;
    uint64_t lastStatusOnMs_ = 0;   // nowMs of the last step with statusOn true
};

}  // namespace sheetlatch
}  // namespace srv_turret
}  // namespace edvr
