// F131 (native on-foot VR): look to aim holds while a panel or a menu is open, and while the game runs no look. Header only, no game or D3D11 state, so
// native_frame_test runs the very functions the frame thread uses. Journal: the arc doc, 2026-10-27, F131.
//
// The pilot (edvr_gfx_20261027_064839, 06:59:40 to 07:01): "with look to aim on, the floating panel menus move with the headset, or try to; on the main menu they kind
// of just judder in place. Only the HUD/aim should move with look to aim." The panels and the menu are anchored to the aim, which look to aim moves to follow the head:
//  - with a full-screen menu up (the maps gate held the 2D screen: gate=panel, world-frames=0, panel-frames=900) it kept asking for the head's tremor, 0.01 to 0.09 deg a
//    second, all applied: the menu juddered;
//  - with on-foot panels open (the placement hook saw them, 90 a second) the aim followed 45 deg head turns, and the panels with it;
//  - from 07:00:55 the game ran no look processor at all (F129: comps now 0) and the follower kept asking (+117 deg asked in one second, 0 applied), queueing a backlog.
// So while one of the three is true the follower asks nothing (no ask, no probe, no snap), the player's look is the game's own, and the aim stays where it is; when
// all three have been clear for kLeaveClearMs the follower starts again from a fresh centring, as an engage does.
// F131b: the menu input is not fed (native_on_foot.cpp, f131Step). The maps gate was not a menu signal: in VR it holds the 2D screen through nearly all play, so the
// flight edvr_gfx_20261027_075042 was held from 108 ms after going on foot to the end and look to aim asked nothing. The menu's judder is likely the third case (no look
// processor: each tick's ask published ahead to the view and never applied; not yet confirmed by a flight); a real menu signal can feed Sample::menu when one is found.
#pragma once

#include "native_on_foot_f109.h"   // Follower, ProbePair

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f131 {

constexpr uint64_t kPanelSeenMs = 250;    // a panel placement, or one of the menu hooks, seen within this long: a panel or a menu is open
constexpr uint64_t kProcStallMs = 150;    // the player's look processor has not run for this long (while the look hooks are in)
constexpr uint64_t kLeaveClearMs = 200;   // all reasons clear this long before the hold ends: no flapping

enum Reason : uint32_t { kPanelOpen = 1, kMenuOrMap = 2, kNoLook = 4 };
inline const char* reasonText(uint32_t mask, char* buf, size_t cap) noexcept {
    if (!buf || cap == 0) return "";
    buf[0] = 0;
    size_t used = 0;
    const struct { uint32_t bit; const char* text; } parts[3] = {{kPanelOpen, "an on-foot panel or menu is open"},
                                                                   {kMenuOrMap, "a menu or map holds the 2D screen"},
                                                                   {kNoLook, "the game's look processor has not run for 150 ms"}};
    for (const auto& p : parts) {
        if (!(mask & p.bit)) continue;
        const int n = std::snprintf(buf + used, cap - used, "%s%s", used ? " + " : "", p.text);
        if (n < 0 || static_cast<size_t>(n) >= cap - used) break;
        used += static_cast<size_t>(n);
    }
    if (!used) std::snprintf(buf, cap, "nothing");
    return buf;
}

// The reasons in a few characters for the 1 s line ("panel+menu"), "none" for no reason.
inline const char* reasonShort(uint32_t mask, char* buf, size_t cap) noexcept {
    if (!buf || cap == 0) return "";
    buf[0] = 0;
    size_t used = 0;
    const struct { uint32_t bit; const char* text; } parts[3] = {{kPanelOpen, "panel"}, {kMenuOrMap, "menu"}, {kNoLook, "no look"}};
    for (const auto& p : parts) {
        if (!(mask & p.bit)) continue;
        const int n = std::snprintf(buf + used, cap - used, "%s%s", used ? "+" : "", p.text);
        if (n < 0 || static_cast<size_t>(n) >= cap - used) break;
        used += static_cast<size_t>(n);
    }
    if (!used) std::snprintf(buf, cap, "none");
    return buf;
}

// One frame's inputs.
struct Sample {
    uint64_t nowMs = 0;
    bool active = false;       // look to aim is engaged on foot (else the hold has nothing to hold)
    uint32_t panelSeq = 0;     // the placement hook's count of panels reached (it only grows)
    // F131c: when the F111 cursor window hook or the F114b owner transform hook last ran (the same clock as nowMs; 0: never). The flight edvr_gfx_20261027_100213 (the F135
    // probe) saw both rise within 0.15 s of each Escape press and fall when the menu closed, mouse still or not, while the placement count above moved only once the
    // pointer was on a panel (11 s after the menu opened in flight 091608).
    uint64_t menuHookMs = 0;
    bool menu = false;         // the maps gate holds the 2D screen as a panel (a menu or a map), the layer taking it
    bool procKnown = false;    // the look hooks are in: the processor's calls can be judged
    uint32_t procCalls = 0;    // the player's own look processor calls (F129: LOCAL calls only)
};
struct Out {
    bool held = false;
    int edge = 0;              // +1 the hold began on this sample, -1 it ended on it
    uint32_t reasons = 0;      // the reasons true on this sample
    uint32_t seen = 0;         // every reason true since the hold began (at its end: the hold's)
    double heldSec = 0.0;      // at the end: how long the hold lasted
};
struct Hold {
    bool held = false;
    bool panelInit = false, procInit = false;
    uint32_t panelSeqLast = 0, procLast = 0;
    uint64_t panelMs = 0;      // when the placement count last moved
    uint64_t procMs = 0;       // when the processor's count last moved (or the baseline was taken)
    uint64_t clearSince = 0;   // when the reasons all went clear, 0 while one holds
    uint64_t heldSinceMs = 0;
    uint32_t seen = 0;
    void reset() noexcept { *this = Hold{}; }
    Out step(const Sample& s) noexcept {
        Out o;
        if (!s.active) {   // nothing to hold: the baselines are taken again at the next engage
            reset();
            return o;
        }
        if (!panelInit) {
            panelInit = true;
            panelSeqLast = s.panelSeq;
            panelMs = 0;
        } else if (s.panelSeq != panelSeqLast) {
            panelSeqLast = s.panelSeq;
            panelMs = s.nowMs ? s.nowMs : 1;
        }
        if (!procInit) {
            procInit = true;
            procLast = s.procCalls;
            procMs = s.nowMs;
        } else if (s.procCalls != procLast) {
            procLast = s.procCalls;
            procMs = s.nowMs;
        }
        const bool placed = panelMs != 0 && s.nowMs >= panelMs && s.nowMs - panelMs <= kPanelSeenMs;
        const bool hooked = s.menuHookMs != 0 && s.nowMs >= s.menuHookMs && s.nowMs - s.menuHookMs <= kPanelSeenMs;
        const bool panel = placed || hooked;
        const bool noLook = s.procKnown && s.nowMs >= procMs && s.nowMs - procMs >= kProcStallMs;
        uint32_t r = 0;
        if (panel) r |= kPanelOpen;
        if (s.menu) r |= kMenuOrMap;
        if (noLook) r |= kNoLook;
        o.reasons = r;
        if (r) {
            clearSince = 0;
            if (!held) {
                held = true;
                heldSinceMs = s.nowMs;
                seen = 0;
                o.edge = +1;
            }
            seen |= r;
        } else if (held) {
            if (clearSince == 0) clearSince = s.nowMs ? s.nowMs : 1;
            if (s.nowMs >= clearSince && s.nowMs - clearSince >= kLeaveClearMs) {
                held = false;
                o.edge = -1;
                o.heldSec = s.nowMs >= heldSinceMs ? static_cast<double>(s.nowMs - heldSinceMs) / 1000.0 : 0.0;
                o.seen = seen;
                clearSince = 0;
            }
        }
        o.held = held;
        if (!o.seen) o.seen = seen;
        return o;
    }
};

// ---- the follower's ask ---------------------------------------------------------------------------------------------------------------------------------
// Following: the follower's ask. Blocked (the game dropped the asks): one zero-sum probe every kProbeMs. Held (F131): nothing at all, no ask and no probe, and the
// probe's phase is not advanced.
enum FollowMode : uint8_t { kFollowing = 0, kBlocked = 1, kHeld = 2 };
inline FollowMode followMode(bool held, bool blocked) noexcept { return held ? kHeld : blocked ? kBlocked : kFollowing; }
inline void followAsk(FollowMode m, const f109::Follower& f, f109::ProbePair& probe, bool probeDue, double dt, double tau, double inFlightYaw, double inFlightPitch,
                      double* askYaw, double* askPitch) noexcept {
    *askYaw = *askPitch = 0.0;
    if (m == kHeld) return;
    if (m == kFollowing) {
        f.ask(dt, tau, inFlightYaw, inFlightPitch, askYaw, askPitch);
        return;
    }
    if (probeDue) probe.next(f.left(0, inFlightYaw), f.left(1, inFlightPitch), askYaw, askPitch);
}

}  // namespace f131
}  // namespace nofoot
}  // namespace edvr
