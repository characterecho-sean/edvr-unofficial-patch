// F176 (native on foot VR): in the sights with the head aiming, the mouse and the stick no longer look up and down. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the game's look update and the frame thread use. Journal: the arc doc, F176.
//
// Sean's word: with fix.native_on_foot_ads_head_aim on and look to aim off (mouse look), aiming down sights the head aims (F62b: the headset's turn is sent to the game as look input and
// cancelled in the seated space); the mouse must not add vertical motion there, so the control scheme mirrors look to aim's, where the mouse and stick do not look up and down. The
// horizontal part stays as it is: the mouse and the stick turn the player, continuously (no snap here). Behaviour, no key.
//
// The decision, made on the frame thread once a tick and read by the game's look update (lookUpdateRun, every call): the pitch is held while the ADS press is active, the head aim is on
// (kAimOn) and driving the sights in mouse look (look to aim neither engaged nor unwinding), the update hook is on and the guard has not tripped. A trip stops the head aim for the rest of
// the press (adsStop with the hold-off clears the hook and the flag), so the mouse's pitch comes back and the player can still aim vertically. Only the RATE input is dropped: the pitch the
// view had before the press stays, the ramp, the centring at each press and the 0.3 s release unwind are the head aim's own and are not touched; the head aim's own ask (the queued pitch,
// added after) still rides the mouse's action value. The processor's split is told the values the call ran with, so the player's pitch share reads 0 there and the head aim's share is
// cancelled exactly as before.
//
// The evidence, on a line that prints without log.on_foot_detail: one line per ADS press when it ends: how long the press lasted, how long the pitch was held, the game's look updates it
// was held in, those with a mouse or stick pitch to drop, the sum of the |mouse pitch| and the |stick pitch| dropped (game input units), whether the guard tripped. A press where the hold
// never ran (the head aim off, look to aim engaged: "held 0.00 s") reads differently from one where it ran and the mouse was still ("held 2.30 s ... 0 with a pitch to drop"), and from one
// where it was held and the update never ran ("held 2.30 s, 0 update calls": the hook is not taking the look update).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f176 {

// True while the mouse's and the stick's pitch are held: an active ADS press (not releasing), the head aim on, look to aim neither engaged nor unwinding, the update hook on, the guard
// not tripped.
inline bool holds(bool adsActive, bool aimOn, bool lookToAimEngaged, bool lookToAimUnwinding, bool hookOn, bool guardTripped) noexcept {
    return adsActive && aimOn && !lookToAimEngaged && !lookToAimUnwinding && hookOn && !guardTripped;
}

// The pitches a look update runs with: both zero while held (the yaws are not its business), and what was dropped. A value that is not a number is dropped as 0.
struct Pitches {
    double mouse = 0.0, stick = 0.0;
    double droppedMouse = 0.0, droppedStick = 0.0;   // |the player's value|, 0 when nothing was held or the value was 0
};
inline Pitches apply(bool held, double mousePitch, double stickPitch) noexcept {
    Pitches p;
    p.mouse = mousePitch;
    p.stick = stickPitch;
    if (!held) return p;
    p.droppedMouse = std::isfinite(mousePitch) ? std::fabs(mousePitch) : 0.0;
    p.droppedStick = std::isfinite(stickPitch) ? std::fabs(stickPitch) : 0.0;
    p.mouse = 0.0;
    p.stick = 0.0;
    return p;
}

// One ADS press's account, the frame thread's: the ticks and seconds of the press, those the pitch was held in, those look to aim was engaged in, and the game thread's counters at the
// press's start (the counters run on; the line prints the difference).
struct Press {
    bool open = false;
    uint32_t number = 0;
    uint64_t startMs = 0;
    uint32_t ticks = 0, heldTicks = 0, ltaTicks = 0;
    double heldSec = 0.0;
    uint32_t callsBase = 0, inputBase = 0;
    double mouseBase = 0.0, stickBase = 0.0;
    bool aimOnSeen = false;   // the head aim was on in some tick of the press (in mouse look or look to aim)
    void begin(uint32_t pressNumber, uint64_t nowMs, uint32_t calls, uint32_t input, double mouse, double stick) noexcept {
        *this = Press{};
        open = true;
        number = pressNumber;
        startMs = nowMs;
        callsBase = calls;
        inputBase = input;
        mouseBase = mouse;
        stickBase = stick;
    }
    // One frame-thread tick of an active press: dt seconds, whether the pitch is held now, whether look to aim is engaged, whether the head aim is on.
    void tick(double dt, bool held, bool lookToAim, bool aimOn) noexcept {
        if (!open) return;
        ++ticks;
        if (held) {
            ++heldTicks;
            heldSec += dt > 0.0 ? dt : 0.0;
        }
        if (lookToAim) ++ltaTicks;
        if (aimOn) aimOnSeen = true;
    }
    bool lookToAimPress() const noexcept { return ticks && ltaTicks * 2 > ticks; }
};

// The press's line. calls, withInput, mouseAbs, stickAbs: the game thread's counters now. ended: how the press ended ("released", "stopped (<why>)").
// The log's lines end at 1200 bytes; this one runs to about 330.
inline int formatPress(char* buf, size_t n, const Press& p, uint64_t nowMs, uint32_t calls, uint32_t withInput, double mouseAbs, double stickAbs, bool guardTripped, const char* ended) noexcept {
    const double lasted = nowMs >= p.startMs ? static_cast<double>(nowMs - p.startMs) * 1e-3 : 0.0;
    return std::snprintf(buf, n,
                         "native on foot ADS pitch held (F176) press %u (%s, head aim %s): lasted %.2f s; the mouse's and the stick's pitch held for %.2f s (%u ticks) of it; the game's look updates "
                         "it was held in %u, of them with a pitch to drop %u; mouse |pitch| dropped %.3f, stick |pitch| dropped %.3f (game input units); guard tripped %s; ended %s",
                         p.number, p.lookToAimPress() ? "look to aim" : "mouse look", p.aimOnSeen ? "on" : "off", lasted, p.heldSec, p.heldTicks, calls - p.callsBase, withInput - p.inputBase,
                         mouseAbs - p.mouseBase, stickAbs - p.stickBase, guardTripped ? "yes" : "no", ended ? ended : "?");
}

}  // namespace f176
}  // namespace nofoot
}  // namespace edvr
