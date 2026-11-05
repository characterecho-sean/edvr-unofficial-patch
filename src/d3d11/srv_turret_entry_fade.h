#pragma once
// The SRV turret's entry fade (issue 140; docs/srv-turret-hud-2026-10-30.md, "2026-11-03: the entry fade"): the pure half, shared by the module (srv_turret_entry_fade.cpp) and
// tools/srv_turret_entry_fade_test, which runs these very functions.
//
// THE FAULT. Switching into the Scorpion's turret view, the game fades to black. When its fade ends the camera is not yet where EDVR puts it (experimental.srv_turret_camera_back
// moves it back by a published eye alignment, which the runtime applies a frame or two after the d3d11 half publishes it, and the game's own camera then follows), and the player
// sees it settle. The fix holds both eyes at black from the turret-view-on edge until the camera has settled, then fades back in.
//
// WHERE THE BLACK IS DRAWN. Not here: the machine only says how black the view is, 0 to 1, once a frame. The level joins the comfort fade (src/common/comfort_fade.h) as one more
// term of nofoot::f123::FadeMix's max; the OpenXR runtime blends it over each finished eye image when it composes (src/openxr/d3d11_stereo.cpp, the fade pass), before the plugins'
// overlay, so EDVR's own menu panel stays visible. VR only: the comfort fade is the runtime's.
//
// THE EDGE. The turret-view bit comes from Status.json, which the game writes some time after it switches (a drawn tool's name took 288 to 686 ms in the F98 flight) and the
// watcher reads at most every 500 ms (100 ms in an SRV, journalWatchSetEagerStatusSrv). So the edge can come after the game's own fade has ended; whether it does is what the
// instrument's luma trace answers. The hold starts at the edge either way. It did come after the game's fade ended (Sean's dump 144324256001 of 2026-11-03), so the edge is no longer
// Status.json's alone: the gunner sheet's first draw raises it earlier (srv_turret_sheet_latch.h, srvTurretViewOn), and the lead below shows the game's own fade against that edge.
//
// THE SETTLE SIGNAL. alignTick publishes the move under a serial (f89AlignPublish); the runtime reports, with each frame's pose, the serial of the alignment that pose carries
// (EdvrNativeNowPose::eyeAlignApplied: the serial + 1, 0 when the runtime does not report). Once that is the move's serial + 1 the runtime has applied it, and the game's camera
// follows over the margin below. A runtime that never reports falls back to the published move plus a longer margin.
//
// THE PRESS. The player's press of the turret key (srv_turret_sheet_latch.h EntryGate) opens the window in which the gunner sheet may raise the view; it does not black the view. The
// press's time is kept so the entry's line can say how long the sheet and Status.json took after it.
//
// NEVER BLACK BY ACCIDENT. The hold ends by the deadline whatever the signal says (kCapMs), the turret view going off ends it at once, and nothing here runs outside the Scorpion's
// turret view in VR. The comfort fade's own staleness rule (a level not renewed for 200 ms decays to 0 over 100 ms) is the last net.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace srv_turret {
namespace entryfade {

// How long after the runtime reports the move applied the camera is taken to have settled. Not measured; built from what is: the game damps a moved placement over about 65 ms
// (docs/srv-turret-hud-2026-10-30.md, the delta mode's refutation), and a frame rendered from the applied pose reaches the compositor one to three frames after (about 40 ms at
// 72 Hz). 150 ms is that with a margin; the instrument's trace (luma against time) is what tunes it, and a black hold costs the player nothing he can see.
constexpr double kSettleMarginMs = 150.0;
// With no applied-serial signal (a runtime that does not report one): the published move plus this. The runtime applies a published alignment a frame or two later (F89), so
// the longer margin is the same 150 ms plus a generous 150 ms for that.
constexpr double kNoSignalMs = 300.0;
// alignTick publishes the move on the edge's own frame. A move not published this long after the edge means it is not publishing (nothing to wait for), and the hold ends then
// rather than at the cap.
constexpr double kPublishWaitMs = 250.0;
// The fade back in, linear. Long enough not to be a pop, short enough not to be a wait: the game's own fade is of the same order.
constexpr double kFadeInMs = 200.0;
// The whole run, from the edge to the level back at 0, is held to this: the hold phase ends at kCapMs - kFadeInMs at the latest. A missing signal cannot leave the headset black.
constexpr double kCapMs = 1000.0;
// The instrument: the luma window starts this long before the edge, and the report is written this long after the run ends (the readbacks land one to three frames late), and the
// trace reaches this far past the run's end.
constexpr double kLeadMs = 1000.0;   // was 300; the trace shows the game's own fade against the early edge
constexpr double kTailMs = 100.0;
constexpr double kReportAfterMs = 250.0;
// The log's hard cap: this many entries a session are reported, each with at most kMaxTraceLines trace lines of kSamplesPerLine samples.
constexpr uint32_t kMaxReports = 12;
constexpr uint32_t kMaxTraceLines = 7;   // a run of lead 1000 + cap 1000 + tail 100 ms is 190 pairs at 90 Hz and 252 at 120 Hz: 280 samples keep them all up to 120 Hz, thinned above
constexpr uint32_t kSamplesPerLine = 40;   // about 19 characters each: a line stays inside the log's 1200
constexpr uint32_t kMaxSamples = kMaxTraceLines * kSamplesPerLine;

enum class Phase : uint8_t { kIdle, kHold, kFadeIn };
// What ended the hold phase.
enum class Settle : uint8_t { kNone, kApplied, kMargin, kNoPublish, kCap, kViewOff };
// Why an edge did not start a hold.
enum class Skip : uint8_t { kNone, kNoMove };

struct Input {
    double nowMs = 0.0;       // a monotonic clock, milliseconds
    uint32_t frame = 0;       // the frame thread's frame number
    bool on = false;          // the Scorpion's turret view is on (VR)
    bool moving = false;      // the camera move is wanted (experimental.srv_turret_camera_back above 0): there is something to wait for
    uint32_t moveCount = 0;   // how many times the move has been published (srvTurretNotePublished), ever
    uint32_t moveSerial = 0;  // ... and the serial of the latest
    uint32_t appliedWord = 0; // EdvrNativeNowPose::eyeAlignApplied: the serial + 1 of the alignment the latest pose carries; 0 when the runtime does not report (or there is no frame)
    // The press gate's words (EntryGate): all idle at their defaults.
    double pressAgeMs = -1.0; // ms since the counted press while it is live, else -1
    bool sheetEdge = false;   // this step's edge came from the gunner sheet's draw
    bool noPress = false;     // this step's edge came with no counted press
    uint8_t fallback = 0;     // the gate's FallbackWhy: 0 none, 1 the key cannot be watched, 2 the watcher was blind
    double sheetLagMs = -1.0; // the sheet draw that raised the view, ms after the press; -1 unknown
    double statusLagMs = -1.0;// Status.json's first on, ms after the press; -1 not yet
};

// What one run did, kept after it ends for the report.
struct Run {
    uint32_t entry = 0;          // the run's number this session
    double startMs = 0.0;
    uint32_t startFrame = 0;
    bool published = false;
    double publishMs = 0.0;
    uint32_t publishFrame = 0, serial = 0;
    bool sawReport = false;      // the runtime reported an applied serial at some tick of the run
    bool appliedSeen = false;
    double appliedMs = 0.0;
    uint32_t appliedFrame = 0;
    Settle settle = Settle::kNone;
    double settleMs = 0.0;       // when the hold phase ended (the fade-in began)
    uint32_t settleFrame = 0;
    bool fadeEnded = false;
    double endMs = 0.0;          // when the level reached 0
    uint32_t endFrame = 0;
    bool capFired = false;
    bool cutShort = false;       // a new entry began before this run's fade-in ended
    Skip skip = Skip::kNone;
    bool pressed = false;        // the entry followed a counted press
    double pressMs = 0.0;
    uint32_t pressFrame = 0;
    bool noPress = false;        // the edge came with no counted press
    bool viaSheet = false;       // the edge came from the gunner sheet's draw (else Status.json)
    uint8_t fallback = 0;
    double sheetLagMs = -1.0, statusLagMs = -1.0;
};

struct Events {
    bool started = false;        // a run began (the hold is on from this step)
    bool skipped = false;        // an edge that did not start a hold (Run::skip says why)
    bool published = false;      // the move's publish was seen this step
    bool appliedSeen = false;    // the runtime reported the move applied this step
    bool settled = false;        // the hold phase ended this step (the fade-in began)
    bool ended = false;          // the level reached 0 this step
};

// The state machine, one step a frame. level() is how black the view is, 0 to 1.
class Machine {
public:
    // Returns the level for this frame. events says what happened in it.
    double step(const Input& in, Events& ev) {
        ev = Events{};
        const bool edge = in.on && !wasOn_;
        wasOn_ = in.on;
        // The press: tracked from the step the gate reports it live to the step it is not (the view going off, or the window ending).
        if (in.pressAgeMs >= 0.0 && !pressTracked_) {
            pressTracked_ = true;
            pressStartMs_ = in.nowMs - in.pressAgeMs;
            pressFrame_ = in.frame;
        } else if (in.pressAgeMs < 0.0) {
            pressTracked_ = false;
        }
        if (edge) {
            // A new entry. A run still holding or fading in is cut short (the new hold starts at 1, which the player sees as the level jumping up, not a second pop).
            if (phase_ == Phase::kHold || phase_ == Phase::kFadeIn) run_.cutShort = true;
            prev_ = run_;
            havePrev_ = true;
            run_ = Run{};
            run_.entry = ++entries_;
            run_.startMs = in.nowMs;
            run_.startFrame = in.frame;
            run_.pressed = in.pressAgeMs >= 0.0;
            run_.pressMs = pressStartMs_;
            run_.pressFrame = pressFrame_;
            run_.noPress = in.noPress;
            run_.viaSheet = in.sheetEdge;
            run_.fallback = in.fallback;
            baseCount_ = lastCount_;  // the publish counted before this frame's tick is the last entry's; one in this frame is the move for this one
            if (!in.moving) {
                run_.skip = Skip::kNoMove;
                phase_ = Phase::kIdle;
                level_ = 0.0;
                ev.skipped = true;
            } else {
                phase_ = Phase::kHold;
                level_ = 1.0;
                ev.started = true;
            }
        }
        lastCount_ = in.moveCount;
        if (!edge && in.pressAgeMs >= 0.0) {   // the lags are told as the gate learns them; the run (if any) keeps the latest
            if (in.sheetLagMs >= 0.0) run_.sheetLagMs = in.sheetLagMs;
            if (in.statusLagMs >= 0.0) run_.statusLagMs = in.statusLagMs;
        } else if (edge) {
            run_.sheetLagMs = in.sheetLagMs;
            run_.statusLagMs = in.statusLagMs;
        }
        if (phase_ == Phase::kHold) holdStep(in, ev);
        if (phase_ == Phase::kFadeIn) fadeStep(in, ev);
        return level_;
    }

    double level() const { return level_; }
    Phase phase() const { return phase_; }
    const Run& run() const { return run_; }
    // The run before the current one (set when a new entry began), for the report of one cut short.
    bool havePrev() const { return havePrev_; }
    const Run& prev() const { return prev_; }
    uint32_t entries() const { return entries_; }

    // Back to idle at 0 (the module is stood down: not VR, the session ended). Keeps the counts.
    void reset() {
        phase_ = Phase::kIdle;
        level_ = 0.0;
        wasOn_ = false;
        pressTracked_ = false;
    }

private:
    void holdStep(const Input& in, Events& ev) {
        if (!run_.published && in.moveCount > baseCount_ && in.moveSerial != 0) {
            run_.published = true;
            run_.publishMs = in.nowMs;
            run_.publishFrame = in.frame;
            run_.serial = in.moveSerial;
            ev.published = true;
        } else if (run_.published && in.moveCount > baseCount_ && in.moveSerial > run_.serial) {
            run_.serial = in.moveSerial;   // a later publish of the move (the key changed in the view): the camera settles on that one
            run_.appliedSeen = false;
        }
        if (in.appliedWord != 0) run_.sawReport = true;
        if (run_.published && !run_.appliedSeen && in.appliedWord != 0 && in.appliedWord >= run_.serial + 1u) {
            run_.appliedSeen = true;
            run_.appliedMs = in.nowMs;
            run_.appliedFrame = in.frame;
            ev.appliedSeen = true;
        }
        Settle why = Settle::kNone;
        if (!in.on) {
            why = Settle::kViewOff;
        } else if (run_.appliedSeen && in.nowMs >= run_.appliedMs + kSettleMarginMs) {
            why = Settle::kApplied;
        } else if (run_.published && !run_.sawReport && in.nowMs >= run_.publishMs + kNoSignalMs) {
            why = Settle::kMargin;
        }
        if (why == Settle::kNone && !run_.published && in.nowMs - run_.startMs >= kPublishWaitMs) why = Settle::kNoPublish;
        if (why == Settle::kNone && in.nowMs - run_.startMs >= kCapMs - kFadeInMs) {
            why = Settle::kCap;
            run_.capFired = true;
        }
        if (why != Settle::kNone) {
            run_.settle = why;
            run_.settleMs = in.nowMs;
            run_.settleFrame = in.frame;
            phase_ = Phase::kFadeIn;
            ev.settled = true;
        }
    }

    void fadeStep(const Input& in, Events& ev) {
        const double t = (in.nowMs - run_.settleMs) / kFadeInMs;
        if (t >= 1.0) {
            phase_ = Phase::kIdle;
            level_ = 0.0;
            run_.fadeEnded = true;
            run_.endMs = in.nowMs;
            run_.endFrame = in.frame;
            ev.ended = true;
        } else {
            level_ = t <= 0.0 ? 1.0 : 1.0 - t;
        }
    }

    Phase phase_ = Phase::kIdle;
    double level_ = 0.0;
    bool wasOn_ = false, havePrev_ = false;
    uint32_t entries_ = 0, lastCount_ = 0, baseCount_ = 0;
    bool pressTracked_ = false;
    double pressStartMs_ = 0.0;
    uint32_t pressFrame_ = 0;
    Run run_{}, prev_{};
};

// ---- the lines --------------------------------------------------------------------------------------------------------------------------------------------------
// Every line starts "srv turret entry fade:". They avoid the phrases tools/edvr_log.py's turret rule reads ("srv turret view", "srv turret camera", ...), so that rule never meets them.

inline const char* settleName(Settle s) {
    switch (s) {
        case Settle::kApplied: return "the runtime's applied serial plus the margin";
        case Settle::kMargin: return "the published move plus the margin (the runtime reports no applied serial)";
        case Settle::kNoPublish: return "the move never being published";
        case Settle::kCap: return "the cap";
        case Settle::kViewOff: return "the view going off";
        default: return "none";
    }
}

// The one line for an edge that started no hold. The line is how a log tells "the hold did not run" from a build that never had it: every on-edge of the turret view is followed by one of
// this line or the run's report (formatRun).
inline int formatSkipped(char* out, size_t cap, const Run& r) {
    return std::snprintf(out, cap,
                         "srv turret entry fade: entry %u at frame %u: no hold ran: the camera move is off (experimental.srv_turret_camera_back is 0), so there is no move to wait for",
                         static_cast<unsigned>(r.entry), static_cast<unsigned>(r.startFrame));
}

// The run's report: the frames and times of every step, the signal and whether the cap fired. Times are milliseconds from the edge.
inline int formatRun(char* out, size_t cap, const Run& r) {
    char applied[112], settled[96], ended[80];
    if (r.appliedSeen)
        std::snprintf(applied, sizeof(applied), "the runtime reported it applied at frame %u (+%.0f ms)", static_cast<unsigned>(r.appliedFrame), r.appliedMs - r.startMs);
    else if (r.sawReport)
        std::snprintf(applied, sizeof(applied), "the runtime reports an applied serial and never reported this one");
    else
        std::snprintf(applied, sizeof(applied), "the runtime reported no applied serial");
    if (r.settle != Settle::kNone)
        std::snprintf(settled, sizeof(settled), "settled at frame %u (+%.0f ms) by %s", static_cast<unsigned>(r.settleFrame), r.settleMs - r.startMs, settleName(r.settle));
    else
        std::snprintf(settled, sizeof(settled), "never settled");
    if (r.fadeEnded)
        std::snprintf(ended, sizeof(ended), "fade-in ended at frame %u (+%.0f ms)", static_cast<unsigned>(r.endFrame), r.endMs - r.startMs);
    else
        std::snprintf(ended, sizeof(ended), "fade-in %s", r.cutShort ? "cut short by the next entry" : "did not end");
    char published[80];
    if (r.published)
        std::snprintf(published, sizeof(published), "move published at frame %u (serial %u)", static_cast<unsigned>(r.publishFrame), static_cast<unsigned>(r.serial));
    else
        std::snprintf(published, sizeof(published), "move never published");
    return std::snprintf(out, cap, "srv turret entry fade: entry %u: hold started at frame %u; %s; %s; %s; %s; cap (%.0f ms) %s",
                         static_cast<unsigned>(r.entry), static_cast<unsigned>(r.startFrame), published, applied, settled, ended, kCapMs, r.capFired ? "FIRED" : "not fired");
}

// The press line of an entry (one a run, within the same cap as the run's report): which press started it and how long the sheet and Status.json took, so one flight measures the
// press-to-sheet time. A run with no counted press says so and why (the watcher cannot see the key, or it was blind, or just no press was counted) and what raised the view.
inline int formatPressEntry(char* out, size_t cap, const Run& r) {
    if (r.pressed) {
        char sheet[64], status[80];
        if (r.sheetLagMs >= 0.0) std::snprintf(sheet, sizeof(sheet), "sheet +%.0f ms after the press", r.sheetLagMs);
        else std::snprintf(sheet, sizeof(sheet), "no sheet draw seen");
        if (r.statusLagMs >= 0.0) std::snprintf(status, sizeof(status), "Status.json +%.0f ms after the press", r.statusLagMs);
        else std::snprintf(status, sizeof(status), "Status.json had not reported when this was written");
        return std::snprintf(out, cap, "srv turret entry fade: entry %u: press at frame %u; %s; %s", static_cast<unsigned>(r.entry), static_cast<unsigned>(r.pressFrame), sheet, status);
    }
    const char* why = r.fallback == 1 ? "the press watcher cannot see the key: fallback" : r.fallback == 2 ? "the press watcher saw nothing before the view went on: fallback" : "no press was counted";
    return std::snprintf(out, cap, "srv turret entry fade: entry %u: no press was seen (%s); the view was raised by %s", static_cast<unsigned>(r.entry), why,
                         r.viaSheet ? "the gunner sheet's draw" : "Status.json");
}

// One frame of the luma trace: the game's eye image mean luminance, both eyes (a negative mean is a reading that never came back), at tMs from the edge. phase is ' ' before the hold,
// 'H' in the hold, 'F' in the fade-in, 'a' after.
struct Sample {
    double tMs = 0.0;
    float mean[2] = {-1.0f, -1.0f};
    char phase = ' ';
};

inline char phaseAt(const Run& r, double tMs) {
    const double at = r.startMs + tMs;
    if (tMs < 0.0) return ' ';
    const bool settled = r.settle != Settle::kNone;
    if (!settled || at < r.settleMs) return 'H';
    if (!r.fadeEnded || at < r.endMs) return 'F';
    return 'a';
}

// The trace line header and the sample list. n samples of s[first..], at most kSamplesPerLine, appended to a line that starts with the header. Returns the line's length.
inline int formatTraceHeader(char* out, size_t cap, const Run& r, uint32_t total, uint32_t stride) {
    return std::snprintf(out, cap,
                         "srv turret entry fade: entry %u luma (the game's eye image mean luminance, left/right, '-' unread; ms from the edge; H hold, F fade-in; %u samples%s):",
                         static_cast<unsigned>(r.entry), static_cast<unsigned>(total), stride > 1 ? ", thinned" : "");
}
inline int appendSample(char* out, size_t cap, size_t at, const Sample& s) {
    char l[12], rr[12];
    if (s.mean[0] >= 0.0f) std::snprintf(l, sizeof(l), "%.3f", static_cast<double>(s.mean[0])); else std::snprintf(l, sizeof(l), "-");
    if (s.mean[1] >= 0.0f) std::snprintf(rr, sizeof(rr), "%.3f", static_cast<double>(s.mean[1])); else std::snprintf(rr, sizeof(rr), "-");
    const char tag[3] = {s.phase == ' ' || s.phase == 'a' ? '\0' : s.phase, '\0', '\0'};
    return std::snprintf(out + at, cap > at ? cap - at : 0, " %s%+.0f:%s/%s", tag, s.tMs, l, rr);
}

// The "no luma at all" line: the module saw no reading in the window (the temporal pass did not run, or its readbacks never finished).
inline int formatNoLuma(char* out, size_t cap, const Run& r) {
    return std::snprintf(out, cap,
                         "srv turret entry fade: entry %u luma: no readings in the window (the temporal pass did not sample the game's eye image in the SRV, or its readbacks never finished)",
                         static_cast<unsigned>(r.entry));
}
inline int formatCapLine(char* out, size_t cap) {
    return std::snprintf(out, cap, "srv turret entry fade: %u entries reported; later entries are not logged (the holds still run)", static_cast<unsigned>(kMaxReports));
}

}  // namespace entryfade
}  // namespace srv_turret

// The module (srv_turret_entry_fade.cpp). Frame thread.
// Once a frame at the Present boundary, AFTER alignTick (so the move published on the edge's own frame is known): runs the machine, keeps the luma window, writes the report lines,
// and returns how black the view is, 0 to 1, for the comfort fade's mix (native_on_foot.cpp, nofoot::f123::FadeMix::turret). Exactly 0.0 outside a hold.
double srvTurretEntryFadeTick(uint32_t frameNo);
// srvTurretNotePublished, when it publishes the camera move: the serial the move went out under.
void srvTurretEntryFadeNoteMove(uint32_t serial);
// srvTurretFrameTick, once a frame BEFORE the tick above: the press gate's words for this frame (srv_turret_sheet_latch.h GateOut), copied into the machine's input.
struct SrvTurretPressWords {
    double ageMs = -1.0;
    bool sheetEdge = false, noPress = false;
    uint8_t fallback = 0;
    double sheetLagMs = -1.0, statusLagMs = -1.0;
};
void srvTurretEntryFadeNotePress(const SrvTurretPressWords& w);

}  // namespace edvr
