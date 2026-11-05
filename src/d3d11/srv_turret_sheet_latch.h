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

// ---- THE PRESS GATE (issue 179; docs/srv-turret-hud-2026-10-30.md, "2026-11-03: the press") --------------------------------------------------------------------------------------
// a4ebcbec kept the draw gate open the whole time the commander was in the Scorpion with the view off, to catch the sheet; that cost the AA-off fast path there. The press gate opens it
// only when it is needed: the player presses Elite's turret key (ToggleBuggyTurretButton, watched by srv_turret_press.cpp), and the sheet is watched from the press until kPressWaitMs
// after it. Wrapped around the latch above, which still raises the view and still lets Status.json take over; the gate adds what a press means:
//   * a press counts when the Scorpion is known, the seat is the SRV, the view is off and Status.json's GuiFocus is 0; presses in any other state do nothing;
//   * a counted press opens the window; only a sheet draw at or after the press raises the view; Status.json reporting the turret ends the wait and raises it too; no view within
//     kPressWaitMs and the press did nothing (the window closes);
//   * a watcher that cannot be trusted ends the feature's dependence on it. FALLBACK, for the rest of the session or until the bindings change: when no slot of the action can be
//     observed (a mouse button past the fifth, an axis, an unbound action, no readable .binds), or when the view goes on (by Status.json or by a sheet) with no press seen in the
//     preceding kBlindWindowMs (the watcher is blind: a joystick the game reads in a way it misses). In fallback the gate is open while the commander is in the Scorpion with the view
//     off, the sheet raises the view unfiltered and the hold starts there: a4ebcbec's behaviour.
constexpr uint64_t kPressWaitMs = 2000;
// Longer than the press window: a press whose view arrives after kPressWaitMs times out first, and the view that follows it must still find that press recent, or the watcher is declared blind
// until the bindings change.
constexpr uint64_t kBlindWindowMs = 5000;
static_assert(kBlindWindowMs > kPressWaitMs, "a press that timed out must still count as seen when its late view arrives");

enum class FallbackWhy : uint8_t { kNone, kUnobservable, kBlind };
enum class GateEvent : uint8_t {
    kNone,
    kCounted,      // a press counted this step: the window opened
    kTimedOut,     // the window ended with no view (or the state it needed went away): the press did nothing
    kFellBack,     // the watcher cannot be trusted: fallback begins (GateOut::fallback says why)
    kFallbackOff   // the bindings changed: the press is trusted again
};

struct GateIn {
    uint64_t nowMs = 0;
    bool pressEdge = false;        // the watcher saw a press edge this frame
    bool observable = false;       // every slot of the action can be watched
    uint32_t epoch = 0;            // the watcher's bindings epoch (changes when the bindings are re-read)
    bool scorpion = false;         // journalSrvScorpion
    bool srvSeat = false;          // journalSeatKind() & 4
    bool guiFocusZero = true;      // Status.json GuiFocus is 0 (or absent)
    bool statusOn = false;         // Status.json's turret-view bit
    uint64_t sheetSeenMs = 0;      // the raw sheet stamp
};

struct GateOut {
    bool on = false;               // the view is on (the latch's answer, filtered by the press)
    Result latch;                  // the latch's own result: its event is kRaised, kStatusAgreed ...
    bool gateOpen = false;         // the draw gate: the sheet is being watched
    bool waiting = false;          // a counted press, the view still off, inside the window
    int64_t pressAgeMs = -1;       // ms since the counted press while it is live (waiting, or the view on), else -1
    int64_t sheetLagMs = -1;       // the sheet draw that raised the view, ms after the press; -1 unknown
    int64_t statusLagMs = -1;      // Status.json's first on after the press, ms after it; -1 not yet
    bool sheetEdge = false;        // the view went on this step by the sheet (the latch's kRaised)
    bool noPress = false;          // the view went on this step with no counted press
    FallbackWhy fallback = FallbackWhy::kNone;
    GateEvent event = GateEvent::kNone;
};

class EntryGate {
public:
    GateOut step(const GateIn& in) {
        GateOut o;
        const uint64_t now = in.nowMs;
        if (in.epoch != epoch_) {
            epoch_ = in.epoch;
            if (why_ != FallbackWhy::kNone) {
                why_ = FallbackWhy::kNone;
                o.event = GateEvent::kFallbackOff;
            }
        }
        if (!in.observable && why_ == FallbackWhy::kNone) {
            why_ = FallbackWhy::kUnobservable;
            o.event = GateEvent::kFellBack;
        }
        if (in.pressEdge) {
            anyPress_ = true;
            lastPressMs_ = now;
        }
        const bool inState = in.scorpion && in.srvSeat;
        // A counted press: everything the view needs is in place and the view is off.
        if (in.pressEdge && why_ == FallbackWhy::kNone && inState && !prevOn_ && in.guiFocusZero && pressMs_ == 0) {
            pressMs_ = now;
            sheetLag_ = statusLag_ = -1;
            o.event = GateEvent::kCounted;
        }
        // The window ends: no view in time, or the state it needed is gone.
        if (pressMs_ != 0 && !prevOn_ && o.event != GateEvent::kCounted && (now - pressMs_ > kPressWaitMs || !inState)) {
            pressMs_ = 0;
            o.event = GateEvent::kTimedOut;
        }
        // The latch. In press mode only a sheet draw at or after the counted press may raise the view.
        uint64_t sheet = in.sheetSeenMs;
        if (why_ == FallbackWhy::kNone) sheet = (pressMs_ != 0 && sheet >= pressMs_) ? sheet : 0;
        Input li;
        li.nowMs = now;
        li.statusOn = in.statusOn;
        li.scorpion = in.scorpion;
        li.sheetSeenMs = sheet;
        o.latch = latch_.step(li);
        o.on = o.latch.on;
        o.sheetEdge = o.latch.event == Event::kRaised;
        if (o.sheetEdge && pressMs_ != 0 && sheet >= pressMs_) sheetLag_ = static_cast<int64_t>(sheet - pressMs_);
        if (in.statusOn && pressMs_ != 0 && statusLag_ < 0) statusLag_ = static_cast<int64_t>(now - pressMs_);
        if (o.on && !prevOn_) {   // the view went on this step
            o.noPress = pressMs_ == 0;
            if (pressMs_ == 0 && why_ == FallbackWhy::kNone && armed_ && (!anyPress_ || now - lastPressMs_ > kBlindWindowMs)) {
                why_ = FallbackWhy::kBlind;
                o.event = GateEvent::kFellBack;
            }
        }
        if (!o.on && prevOn_) pressMs_ = 0;   // leaving the turret ends the press
        armed_ = inState && !o.on;
        prevOn_ = o.on;
        o.fallback = why_;
        o.waiting = pressMs_ != 0 && !o.on;
        o.gateOpen = why_ != FallbackWhy::kNone ? (inState && !o.on) : (o.waiting || latch_.latched());
        o.pressAgeMs = pressMs_ != 0 ? static_cast<int64_t>(now - pressMs_) : -1;
        o.sheetLagMs = sheetLag_;
        o.statusLagMs = statusLag_;
        return o;
    }
    // Stood down (not VR, the session ended): back to the start. A fallback and the press seen stay.
    void reset() {
        latch_.reset();
        pressMs_ = 0;
        prevOn_ = armed_ = false;
    }
    FallbackWhy fallback() const { return why_; }

private:
    Latch latch_;
    uint64_t pressMs_ = 0;       // the counted press, 0 none
    uint64_t lastPressMs_ = 0;
    bool anyPress_ = false;      // a press edge of any kind was ever seen
    bool prevOn_ = false;
    bool armed_ = false;         // last step: in the Scorpion's SRV seat with the view off, so a view going on now is a real entry
    uint32_t epoch_ = 0;
    FallbackWhy why_ = FallbackWhy::kNone;
    int64_t sheetLag_ = -1, statusLag_ = -1;
};

}  // namespace sheetlatch
}  // namespace srv_turret
}  // namespace edvr
