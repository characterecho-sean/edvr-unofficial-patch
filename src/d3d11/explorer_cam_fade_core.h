// Explorer Cam's comfort fade: the timeline (docs\design-explorer-cam-free-camera-2026-10-07.md, "Comfort fade").
//
// Switching Explorer Cam on or off shows things nobody should see: the camera's selfie preset, its on-screen controls, the free camera settling in the body,
// the unlock. So the view is blacked out around both. This file is the pure state machine that decides, once a frame on the frame thread, how black the view
// is (0 clear .. 1 black) and when the F5 sequence may begin its presses. The glue (explorer_cam.cpp) feeds it the facts and publishes the level through
// comfort_fade.h; the OpenXR runtime blends black over the eye images by it (d3d11_stereo.cpp).
//
//   ENTER  F5 -> fade out 200 ms -> only then is F5's request released (the first press) -> black holds through open, TAB, placement, lock and the UI hide
//          -> once PLACED, the lock is CONFIRMED (+0x48C = 4), the camera UI's hide has SETTLED and the eye has been STEADY for 10 updates (moved under 2 cm
//          each, either source) -> fade in 300 ms.
//   EXIT   F5 -> fade out 200 ms -> the exit request is released (UI back, camera closed) -> once the camera reads mode 0 and 10 controller updates have passed
//          -> fade in 300 ms.
//   REATTACH  the placement is released with the session still on (a detach): 100 ms out, black until placed and locked and steady, 200 ms in.
//   MOTION (entering and re-attaching only) once all of that holds, the view stays black until the engine's motion is live for the eye path: its views were handed
//          to the compose for kFadeViewsLiveFrames consecutive frames, and, when the frame has skinned jobs (a character is loaded), the second skin's join is live
//          for it (the F12 flight: the first F5 entry showed the NPC for about 0.7 s with no exact motion, aliased until the history rebuilt). A scene with no
//          characters waits for the views alone, and with the engine's motion not armed at all nothing is waited for. The hold never exceeds kFadeMotionHoldMs
//          beyond the moment the view would otherwise have faded in: then it fades in anyway and says which condition was missing. Exits are unchanged.
//   SAFETY black never lasts more than 3 s from the F5 press: at 3 s it fades in regardless and says which condition was unmet. A refused or dropped F5, a
//          session that ends under it, a second F5 before the camera opened (a cancel), Explorer Cam standing down or the hotkey cleared: back to clear at once,
//          or fading in without waiting.
// The ramp is a smoothstep of a linear clock, so an interrupted ramp turns round where it stands.
#pragma once
#include <cmath>
#include <cstdint>

#include "explorer_cam_core.h"

namespace edvr {
namespace ecm {

constexpr uint32_t kFadeOutMs = 200, kFadeInMs = 300;
constexpr uint32_t kFadeReattachOutMs = 100, kFadeReattachInMs = 200;
constexpr uint32_t kFadeMaxBlackMs = 3000;           // black never lasts longer than this from the press
constexpr uint32_t kFadeViewsLiveFrames = 3;         // consecutive frames both eyes must have been given the engine-motion views before an entry fades in
constexpr uint32_t kFadeMotionHoldMs = 1000;         // the most the motion wait may add to the black, from the moment the entry was otherwise ready
constexpr uint32_t kFadeSteadyUpdates = 10;          // consecutive placing updates with an eye step under kFadeSteadyMetres
constexpr float kFadeSteadyMetres = 0.02f;
constexpr uint32_t kFadeClosedUpdates = 10;          // controller updates at mode 0 before an exit fades in
constexpr uint32_t kFadeDeadFrames = 6;              // frames with no pending request and no session before a released entry is called refused
constexpr uint32_t kFadeGoneFrames = 2;              // frames without a session, once it was seen, before a transition is called aborted

// QPC ticks to microseconds, for the clock the timeline is fed (ComfortInputs::nowUs). The product ticks * 1000000 wraps a uint64 at tick 18,446,744,073,710
// (about 21 days of counter at 10 MHz, the interval depends on the frequency), and a time that wraps jumps back below every deadline the timeline stored. So
// the whole seconds are converted first and only the remainder, under one second of ticks, is multiplied: exact for every tick count a machine can reach.
// Both of explorer_cam.cpp's conversions (the boundary's input and realNowUs) call this one; tools\explorer_cam_fade_test pins them to it.
inline uint64_t qpcTicksToUs(uint64_t ticks, uint64_t freq) {
    if (freq == 0) freq = 1;
    return (ticks / freq) * 1000000ull + (ticks % freq) * 1000000ull / freq;
}

enum class ComfortKind : uint8_t { None = 0, Enter, Exit, Reattach };
enum class ComfortPhase : uint8_t { Clear = 0, Out, Black, In };
enum ComfortUnmet : uint32_t {
    kComfortUnmetPlaced = 1, kComfortUnmetLock = 2, kComfortUnmetSteady = 4, kComfortUnmetUi = 8, kComfortUnmetSession = 16, kComfortUnmetClosed = 32, kComfortUnmetUpdates = 64,
    kComfortUnmetViews = 128, kComfortUnmetSkin = 256   // the motion wait (an entry that is otherwise ready): the engine-motion views, the second skin's join
};
enum class ComfortEv : uint8_t { None = 0, Start, FadeIn, TimedOut, Aborted, Cancelled, Cleared, Dropped, MotionTimedOut };
enum class ComfortWhy : uint8_t { None = 0, NotTaken, SessionEnded, StoodDown };

inline const char* comfortKindText(ComfortKind k) {
    switch (k) {
        case ComfortKind::Enter: return "entering";
        case ComfortKind::Exit: return "leaving";
        case ComfortKind::Reattach: return "re-attaching";
        default: return "idle";
    }
}
inline const char* comfortPhaseText(ComfortPhase p) {
    switch (p) {
        case ComfortPhase::Out: return "fading-out";
        case ComfortPhase::Black: return "black";
        case ComfortPhase::In: return "fading-in";
        default: return "clear";
    }
}

struct ComfortInputs {
    uint64_t nowUs = 0;
    bool active = false;            // Explorer Cam armed (the hotkey set), every hook it needs in place, the fault budget not spent
    bool sessionActive = false;
    bool requestPending = false;    // a released F5 request the controller has not taken yet
    uint8_t mode = 0;               // the camera controller's mode byte
    bool placed = false;            // a placement stands
    uint8_t state = 0;              // the free camera's +0x48C as last read
    uint32_t steady = 0;            // consecutive placing updates whose eye moved under kFadeSteadyMetres
    bool uiSettled = false;         // the camera UI's hide has run its course (hidden, or nothing to hide)
    uint64_t ctlCalls = 0;          // the controller's update count
    bool pressEnter = false, pressExit = false;   // this frame's F5 decision
    // The engine's motion for the eye path (engine_motion_ready.h), as of the last frame boundary. With motionArmed false there is nothing to wait for.
    bool motionArmed = false;
    uint32_t viewsRun = 0;          // consecutive frames both eyes were handed the engine-motion views
    bool skinJobs = false;          // the last frame's palette chain ran with jobs: a character is loaded
    bool skinLive = false;          // ...and the second skin's join was live for it
};

struct ComfortLine {
    ComfortEv ev = ComfortEv::None;
    ComfortKind kind = ComfortKind::None;
    ComfortWhy why = ComfortWhy::None;
    uint32_t heldMs = 0;            // since the press (Start: 0; FadeIn/TimedOut/Aborted/Cancelled: the black so far; Cleared: all of it)
    uint32_t unmet = 0;             // ComfortUnmet bits (TimedOut)
    uint32_t steady = 0, mode = 0, state = 0, closedUpdates = 0;
    bool released = false;          // the F5 request had been released
    uint32_t motionMs = 0;          // the motion wait so far (FadeIn after a hold, MotionTimedOut: how long the view was held for the engine's motion)
    uint32_t viewsRun = 0;          // the frames the views had been live in a row (FadeIn, MotionTimedOut)
    bool motionArmed = false, skinJobs = false, skinLive = false;   // the engine's motion as it stood (FadeIn, MotionTimedOut): what the line reports of it
};
struct ComfortStep {
    float alpha = 0.0f;
    bool releaseEnter = false, releaseExit = false;   // publish F5's request NOW (the view is black)
    uint8_t nev = 0;
    ComfortLine ev[2];
};

// How far the eye moved between two placing updates, in metres (the comfort fade's "steady" test: under kFadeSteadyMetres, either source).
inline float eyeStepMetres(const Eye& a, const Eye& b) {
    const float du = a.up - b.up, df = a.forward - b.forward, dr = a.right - b.right;
    return std::sqrt(du * du + df * df + dr * dr);
}

inline float comfortSmooth(float x) { return x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : x * x * (3.0f - 2.0f * x)); }

class ComfortTimeline {
public:
    ComfortPhase phase() const { return m_phase; }
    ComfortKind kind() const { return m_kind; }
    float alpha() const { return m_phase == ComfortPhase::Clear ? 0.0f : comfortSmooth(m_x); }
    bool busy() const { return m_kind != ComfortKind::None || m_phase != ComfortPhase::Clear; }
    // Can the next step() read the engine's motion inputs (ComfortInputs::motionArmed, viewsRun, skinJobs, skinLive)? Only an entry or a re-attach that is already fading
    // out or black can fade in on them (an F5 that starts one this step releases nothing yet, and a re-attach starts unplaced); idle, an exit and a fade in never
    // look at them. The glue asks the engine for them only when this is true: engineMotionReady() takes the engine's mutex.
    bool wantsMotion() const {
        return (m_kind == ComfortKind::Enter || m_kind == ComfortKind::Reattach) && (m_phase == ComfortPhase::Out || m_phase == ComfortPhase::Black);
    }
    void reset() { *this = ComfortTimeline(); }

    ComfortStep step(const ComfortInputs& raw) {
        ComfortStep out;
        // The timeline runs on a clock that never goes backwards. The deadlines it stores (the press, the motion hold) are readings of that clock; one that
        // stepped back (a counter that wrapped in a conversion, a source swapped under it) would leave them in the future of "now", heldMs would read 0, and
        // the 3 s cap and the motion hold would not fire until the clock caught up, which can be never. A step back is taken as no time passing, and the clock
        // goes on from the reading it was at: the cap is the time that really passed, not the time the caller's clock claims.
        ComfortInputs in = raw;
        if (m_have && raw.nowUs + m_shiftUs < m_lastUs) m_shiftUs = m_lastUs - raw.nowUs;
        in.nowUs = raw.nowUs + m_shiftUs;
        double dtMs = m_have && in.nowUs > m_lastUs ? static_cast<double>(in.nowUs - m_lastUs) / 1000.0 : 0.0;
        m_have = true;
        m_lastUs = in.nowUs;
        if (!in.active) {
            if (busy()) {
                ComfortLine l = line(ComfortEv::Dropped, in);
                l.why = ComfortWhy::StoodDown;
                emit(out, l);
            }
            reset();
            m_have = true;
            m_lastUs = in.nowUs;
            m_prevPlaced = false;
            return out;   // alpha 0
        }

        // ---- F5 -----------------------------------------------------------------------------------------------------------------------------------------------
        if (in.pressExit) {
            if (!(m_kind == ComfortKind::Exit && m_phase != ComfortPhase::In)) start(ComfortKind::Exit, in, out);   // a second press while leaving is the same press
        } else if (in.pressEnter) {
            if (m_kind == ComfortKind::Enter && !m_released && m_phase != ComfortPhase::In) {
                m_phase = ComfortPhase::In;   // pressed again before the camera opened: change of mind
                m_cancelled = true;
                emit(out, line(ComfortEv::Cancelled, in));
            } else if (m_kind == ComfortKind::None || m_phase == ComfortPhase::In || m_kind == ComfortKind::Exit || m_kind == ComfortKind::Reattach) {
                start(ComfortKind::Enter, in, out);
            }   // else: the request is out and the entry is under way: nothing more to do
        }
        // ---- a detach with the session still on --------------------------------------------------------------------------------------------------------------
        if (m_prevPlaced && !in.placed && in.sessionActive && m_kind != ComfortKind::Exit && (m_phase == ComfortPhase::Clear || m_phase == ComfortPhase::In))
            start(ComfortKind::Reattach, in, out);
        m_prevPlaced = in.placed;

        // ---- the clock -----------------------------------------------------------------------------------------------------------------------------------------
        if (m_startedNow) dtMs = 0.0;   // a ramp that began this frame starts from its first frame, not from the gap before it
        m_startedNow = false;
        bool enteredBlack = m_enteredBlack;
        m_enteredBlack = false;
        if (m_phase == ComfortPhase::Out) {
            m_x += static_cast<float>(dtMs / m_outMs);
            if (m_x >= 1.0f) {
                m_x = 1.0f;
                m_phase = ComfortPhase::Black;
                enteredBlack = true;
            }
        }
        if (m_phase == ComfortPhase::Black) blackStep(in, out, enteredBlack);
        else if (m_phase == ComfortPhase::In) {
            m_x -= static_cast<float>(dtMs / m_inMs);
            if (m_x <= 0.0f) {
                m_x = 0.0f;
                ComfortLine l = line(ComfortEv::Cleared, in);
                emit(out, l);
                const bool keepPlaced = m_prevPlaced;
                reset();
                m_have = true;
                m_lastUs = in.nowUs;
                m_prevPlaced = keepPlaced;
            }
        }
        out.alpha = alpha();
        return out;
    }

private:
    uint32_t heldMs(const ComfortInputs& in) const { return in.nowUs > m_startUs ? static_cast<uint32_t>((in.nowUs - m_startUs) / 1000) : 0; }
    ComfortLine line(ComfortEv ev, const ComfortInputs& in) const {
        ComfortLine l;
        l.ev = ev;
        l.kind = m_kind;
        l.heldMs = heldMs(in);
        l.steady = in.steady;
        l.mode = in.mode;
        l.state = in.state;
        l.closedUpdates = m_modeZero ? static_cast<uint32_t>(in.ctlCalls - m_zeroCalls) : 0;
        l.released = m_released;
        l.viewsRun = in.viewsRun;
        l.motionArmed = in.motionArmed;
        l.skinJobs = in.skinJobs;
        l.skinLive = in.skinLive;
        l.motionMs = m_motionHoldUs && in.nowUs > m_motionHoldUs ? static_cast<uint32_t>((in.nowUs - m_motionHoldUs) / 1000) : 0;
        return l;
    }
    static void emit(ComfortStep& out, const ComfortLine& l) {
        if (out.nev < 2) out.ev[out.nev++] = l;
    }
    void start(ComfortKind kind, const ComfortInputs& in, ComfortStep& out) {
        const bool re = kind == ComfortKind::Reattach;
        m_kind = kind;
        m_startedNow = true;
        m_released = re;
        m_cancelled = false;
        m_sawSession = re;
        m_dead = m_gone = 0;
        m_modeZero = false;
        m_motionHoldUs = 0;
        m_startUs = in.nowUs;
        m_outMs = static_cast<float>(re ? kFadeReattachOutMs : kFadeOutMs);
        m_inMs = static_cast<float>(re ? kFadeReattachInMs : kFadeInMs);
        if (m_x >= 1.0f) {
            m_phase = ComfortPhase::Black;
            m_enteredBlack = true;
        } else {
            m_phase = ComfortPhase::Out;
        }
        ComfortLine l = line(ComfortEv::Start, in);
        l.heldMs = 0;
        emit(out, l);
    }
    void beginIn(ComfortEv ev, ComfortWhy why, uint32_t unmet, const ComfortInputs& in, ComfortStep& out) {
        ComfortLine l = line(ev, in);
        l.why = why;
        l.unmet = unmet;
        emit(out, l);
        m_phase = ComfortPhase::In;
    }
    void blackStep(const ComfortInputs& in, ComfortStep& out, bool enteredBlack) {
        // A cancelled entry only has to fade back in.
        if (m_cancelled) {
            m_phase = ComfortPhase::In;
            return;
        }
        uint32_t unmet = 0;
        bool aborted = false;
        ComfortWhy why = ComfortWhy::None;
        if (m_kind == ComfortKind::Exit) {
            if (m_released) {
                if (in.mode == 0) {
                    if (!m_modeZero) {
                        m_modeZero = true;
                        m_zeroCalls = in.ctlCalls;
                    }
                } else {
                    m_modeZero = false;
                }
                if (!m_modeZero) unmet |= kComfortUnmetClosed;
                else if (in.ctlCalls - m_zeroCalls < kFadeClosedUpdates) unmet |= kComfortUnmetUpdates;
                // The exit sequence gave up with the camera still open: nothing more will close it.
                if (!in.sessionActive && in.mode != 0) {
                    if (++m_gone >= kFadeGoneFrames + 2) {
                        aborted = true;
                        why = ComfortWhy::SessionEnded;
                    }
                } else {
                    m_gone = 0;
                }
            } else {
                unmet |= kComfortUnmetClosed;
            }
        } else {
            if (in.sessionActive) m_sawSession = true;
            if (!in.placed) unmet |= kComfortUnmetPlaced;
            if (in.state != kStateRelativeLock) unmet |= kComfortUnmetLock;
            if (in.steady < kFadeSteadyUpdates) unmet |= kComfortUnmetSteady;
            if (!in.uiSettled) unmet |= kComfortUnmetUi;
            if (!in.sessionActive) unmet |= kComfortUnmetSession;
            if (!m_released) {
                unmet |= kComfortUnmetSession;
            } else if (!m_sawSession) {
                if (!in.requestPending) {
                    if (++m_dead >= kFadeDeadFrames) {
                        aborted = true;
                        why = ComfortWhy::NotTaken;
                    }
                } else {
                    m_dead = 0;
                }
            } else if (!in.sessionActive) {
                if (++m_gone >= kFadeGoneFrames) {
                    aborted = true;
                    why = ComfortWhy::SessionEnded;
                }
            } else {
                m_gone = 0;
            }
        }
        // The view is dark: F5's request may go (it was not before, so the first press happens in the dark). Only on a step after the one that reached black, so
        // the full level has been published once.
        if (!m_released && !enteredBlack) {
            m_released = true;
            if (m_kind == ComfortKind::Enter) out.releaseEnter = true;
            else if (m_kind == ComfortKind::Exit) out.releaseExit = in.sessionActive;
            if (m_kind == ComfortKind::Exit && !in.sessionActive) m_gone = 0;
        }
        // The motion wait: an entry (or a re-attach) that is otherwise ready keeps the view black until the engine's motion is live for the eye path, for at
        // most kFadeMotionHoldMs from the moment it was otherwise ready. The hold starts over if the entry stops being otherwise ready.
        uint32_t motionUnmet = 0;
        if (m_kind != ComfortKind::Exit && in.motionArmed) {
            if (in.viewsRun < kFadeViewsLiveFrames) motionUnmet |= kComfortUnmetViews;
            if (in.skinJobs && !in.skinLive) motionUnmet |= kComfortUnmetSkin;
        }
        if (unmet != 0 || !m_released) m_motionHoldUs = 0;
        if (aborted) {
            beginIn(ComfortEv::Aborted, why, unmet, in, out);
        } else if (m_released && unmet == 0) {
            if (motionUnmet == 0) {
                beginIn(ComfortEv::FadeIn, ComfortWhy::None, 0, in, out);
            } else {
                if (m_motionHoldUs == 0) m_motionHoldUs = in.nowUs ? in.nowUs : 1;
                if (in.nowUs > m_motionHoldUs && in.nowUs - m_motionHoldUs >= static_cast<uint64_t>(kFadeMotionHoldMs) * 1000u)
                    beginIn(ComfortEv::MotionTimedOut, ComfortWhy::None, motionUnmet, in, out);
            }
        } else if (heldMs(in) >= kFadeMaxBlackMs) {
            beginIn(ComfortEv::TimedOut, ComfortWhy::None, unmet, in, out);
        }
    }

    bool m_have = false;
    uint64_t m_lastUs = 0, m_startUs = 0;
    uint64_t m_shiftUs = 0;   // what the caller's clock has been stepped back by, summed: nowUs + this is the timeline's own, monotonic clock (a reset
                              // that goes on running sets m_lastUs to a reading of that clock, which is how the next step finds the shift again)
    ComfortPhase m_phase = ComfortPhase::Clear;
    ComfortKind m_kind = ComfortKind::None;
    float m_x = 0.0f;
    float m_outMs = static_cast<float>(kFadeOutMs), m_inMs = static_cast<float>(kFadeInMs);
    bool m_released = false, m_cancelled = false, m_sawSession = false, m_enteredBlack = false, m_prevPlaced = false, m_modeZero = false, m_startedNow = false;
    uint32_t m_dead = 0, m_gone = 0;
    uint64_t m_zeroCalls = 0;
    uint64_t m_motionHoldUs = 0;   // when the entry was first otherwise ready and waiting for the engine's motion (0: not waiting)
};

// ---- the lines ---------------------------------------------------------------------------------------------------------------------------------
inline const char* prefixComfort() { return "explorer cam: comfort fade:"; }
inline void putComfortUnmet(Line& o, const ComfortLine& l) {
    bool any = false;
    auto item = [&](uint32_t bit, const char* fmt, unsigned a = 0, unsigned b = 0) {
        if (!(l.unmet & bit)) return;
        o.put("%s", any ? "; " : "");
        o.put(fmt, a, b);
        any = true;
    };
    item(kComfortUnmetSession, "no F5 session yet");
    item(kComfortUnmetPlaced, "the view is not placed");
    item(kComfortUnmetLock, "the lock is not confirmed (+0x48C = %u, want 4)", l.state);
    item(kComfortUnmetSteady, "the eye is not steady (%u of 10 updates)", l.steady);
    item(kComfortUnmetUi, "the camera UI's hide has not settled");
    item(kComfortUnmetClosed, "the camera does not read closed (mode %u)", l.mode);
    item(kComfortUnmetUpdates, "only %u of 10 controller updates since it closed", l.closedUpdates);
    item(kComfortUnmetViews, "the engine's motion views were live for %u of %u frames in a row", l.viewsRun, kFadeViewsLiveFrames);
    item(kComfortUnmetSkin, "the second skin's join is not live for the frame's skinned jobs");
    if (!any) o.put("none");
}
inline void formatComfort(char* out, size_t cap, const ComfortLine& l) {
    Line o(out, cap);
    const double held = static_cast<double>(l.heldMs) / 1000.0;
    switch (l.ev) {
        case ComfortEv::Start:
            if (l.kind == ComfortKind::Enter)
                o.put("%s F5: fading to black over %u ms BEFORE the camera opens, so the preset view and the camera's controls are never seen; black holds until the view is "
                      "placed, locked and steady, and for 3 s at most (then, if the engine's motion is not live yet, up to %u ms more)",
                      prefixComfort(), kFadeOutMs, kFadeMotionHoldMs);
            else if (l.kind == ComfortKind::Exit)
                o.put("%s F5: fading to black over %u ms BEFORE the camera's controls come back and the camera closes; black holds until the camera reads closed, and for 3 s at most",
                      prefixComfort(), kFadeOutMs);
            else
                o.put("%s the placement was released with the session still on (a detach): fading to black over %u ms until the view is placed again, and for 3 s at most",
                      prefixComfort(), kFadeReattachOutMs);
            break;
        case ComfortEv::FadeIn:
            if (l.kind == ComfortKind::Exit)
                o.put("%s %s: the camera reads closed and %u controller updates have passed; black lasted %.2f s; fading in over %u ms", prefixComfort(), comfortKindText(l.kind),
                      kFadeClosedUpdates, held, kFadeInMs);
            else
                o.put("%s %s: placed, locked, the camera UI hidden and the eye steady for %u updates; black lasted %.2f s; fading in over %u ms", prefixComfort(), comfortKindText(l.kind),
                      kFadeSteadyUpdates, held, l.kind == ComfortKind::Reattach ? kFadeReattachInMs : kFadeInMs);
            if (l.kind != ComfortKind::Exit) {
                if (l.motionMs)
                    o.put(" (held %u ms of that for the engine's motion: its views live, and the second skin's join for the frame's skinned jobs)", l.motionMs);
                if (!l.motionArmed) o.put(" [engine motion: not running, so nothing was waited for]");
                else o.put(" [engine motion: views live for %u frames in a row, %s]", l.viewsRun,
                           l.skinJobs ? "skinned jobs, the second skin's join live" : "no skinned jobs");   // (armed and fading in means the join is live whenever there are jobs)
            }
            break;
        case ComfortEv::MotionTimedOut:
            o.put("%s %s: placed, locked and steady, but the engine's motion was not live after the %u ms hold, so the view fades in anyway (black lasted %.2f s). Unmet: ", prefixComfort(),
                  comfortKindText(l.kind), kFadeMotionHoldMs, held);
            putComfortUnmet(o, l);
            break;
        case ComfortEv::TimedOut:
            o.put("%s %s: black reached the %u s cap, so the view fades in anyway (black lasted %.2f s). Unmet: ", prefixComfort(), comfortKindText(l.kind), kFadeMaxBlackMs / 1000, held);
            putComfortUnmet(o, l);
            break;
        case ComfortEv::Aborted:
            if (l.why == ComfortWhy::NotTaken)
                o.put("%s %s: F5's request was not taken or was refused (the lines above say why), so the view fades back in at once (black lasted %.2f s)", prefixComfort(),
                      comfortKindText(l.kind), held);
            else
                o.put("%s %s: the session ended under it (the lines above say why), so the view fades back in at once (black lasted %.2f s). Unmet: ", prefixComfort(),
                      comfortKindText(l.kind), held);
            if (l.why != ComfortWhy::NotTaken) putComfortUnmet(o, l);
            break;
        case ComfortEv::Cancelled:
            o.put("%s F5 pressed again before the camera opened: the entry is cancelled and the view fades back in (black lasted %.2f s)", prefixComfort(), held);
            break;
        case ComfortEv::Cleared:
            o.put("%s clear again; the view was dark for %.2f s in all (%s)", prefixComfort(), held, comfortKindText(l.kind));
            break;
        case ComfortEv::Dropped:
            o.put("%s Explorer Cam stood down or its hotkey was cleared while the view was dark (%s, %.2f s): the view is clear at once", prefixComfort(), comfortKindText(l.kind), held);
            break;
        default:
            o.put("%s event %u (unnamed)", prefixComfort(), static_cast<unsigned>(l.ev));
            break;
    }
}

}  // namespace ecm
}  // namespace edvr
