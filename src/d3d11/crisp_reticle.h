// C1a: the crisp reticle (docs\design-vr-scope-magnification-2026-10-28.md, section 12).
//
// Every weapon draws one gunsight quad per eye with the pair the optical chevron uses (VS 025B4B9FF54622ED, PS 46F92DC71BF8DFA5, 6 indices in 1 instance, additive); only the
// per-weapon instance scale differs. ui_layer_math.h names that draw kScopeMark and takes it out of the scene into the eye's weapons layer, which is composited after the upscaler
// by the tonemap difference (stock brightness, out of the bloom input). The family is named only while its arming says so: scopeZoomMarkArmed (the lens qualifies) and, from C1,
// scopeZoomCrispArmed (this file's gate, published by the frame thread each tick). The gate arms the quad of every weapon the scope lens does not name, for the whole press of the
// sights (from the raise to the end of the lower), and never at the hip, where the quad is large. Pure, so the rigs drive the rule itself.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace crisp {

// What the frame thread knows on a tick of a press of the sights (native_on_foot.cpp s1::tick).
struct GateIn {
    bool keyOn = false;       // experimental.native_on_foot_ads_crisp_reticle
    bool onFoot = false;      // native on foot is active
    bool adsUp = false;       // a press of the sights is open: the game's ADS flag up, through the release (the frame thread's g_adsState is not idle)
    bool lensKeyOn = false;   // fix.native_on_foot_ads_scope_zoom
    bool itemKnown = false;   // the held item is coherent and its scope state is known
    bool lensOwned = false;   // the held (item, scope) is an optical sight of the lens table with M at least the lens's floor: the lens path names the quad for it, as before C1
    bool wFresh = false;      // the game's ADS weight was read this tick
    double w = 0.0;           // ... and is this (0 at the hip, 1 with the sights fully up)
};

// The press is one this gate may serve: the key on, native on foot, the sights up, and the lens not the owner of the quad. With the scope zoom key on, an item whose entry is not
// known yet waits (an optical sight must not move to this path for the first ticks of a press); with it off there is no lens and every sight is served.
inline bool eligible(const GateIn& g) noexcept {
    if (!g.keyOn || !g.onFoot || !g.adsUp) return false;
    if (g.lensKeyOn && (!g.itemKnown || g.lensOwned)) return false;
    return true;
}
// The quad is taken out of the scene on this tick: eligible, and not at the hip. The game's weight is zero at the hip and before the raise has moved it; a weight that was not read
// this tick, or is no number, says nothing and leaves the press's own state to decide.
inline bool armed(const GateIn& g) noexcept {
    if (!eligible(g)) return false;
    if (g.wFresh && std::isfinite(g.w) && !(g.w > 0.0)) return false;
    return true;
}

// The door-side counts of a press (scope_zoom.cpp): eye-frames at the weapons composite with the quad's arming up, and of them the ones in which no quad had been taken into the
// eye's weapons layer. Index 0 the left eye, 1 the right.
struct DoorStats {
    uint32_t armed[2] = {};
    uint32_t noQuad[2] = {};
};

// The press line. taken, path, composited and standDowns are the lens door's own counts of the same press (scope_zoom_s1.h LensDoorStats markRedirected, markPath, markComposite
// and markStandDowns): the layer counts the mark the same way whoever armed it.
struct PressLine {
    uint32_t press = 0;
    const char* item = "none";
    const char* scope = "unknown";
    uint32_t eligibleTicks = 0, armedTicks = 0;   // the frame thread's ticks of the press in which the gate was eligible, and armed
    DoorStats door;
    uint32_t taken[2] = {};
    uint32_t pathSeeded = 0, pathReadOnly = 0, pathTestsOff = 0, pathUntested = 0;
    uint32_t matched = 0, plain = 0, none = 0;
    uint32_t standDowns = 0;
};
constexpr size_t kPressLineBytes = 640;

// The tagged lines, built here so the --health rule's fixtures read their format literals (tools\edvr_log.py). The press line is written for every press of the sights the gate was
// eligible in with the key on: armed ticks 0 beside eligible ticks above 0 says the gate never armed, taken 0 beside armed ticks says the quad never reached the layer.
inline int formatAnnounce(char* buf, size_t n, bool keyOn) {
    return std::snprintf(buf, n,
                         "crisp reticle (C1): key %s; the gunsight quad of every weapon the scope lens does not name is drawn into the sharp weapons layer for the whole press of the sights "
                         "(from the raise to the end of the lower, never at the hip), composited at the game's tonemap and out of the bloom input; one line a press of those follows",
                         keyOn ? "on" : "off");
}
inline int formatPress(char* buf, size_t n, const PressLine& p) {
    return std::snprintf(buf, n,
                         "crisp reticle (C1): press %u item %s scope %s key on; eligible ticks %u, armed ticks %u; armed eye-frames L %u R %u, with no quad taken L %u R %u; "
                         "quad taken L %u R %u (depth path seeded %u read-only %u tests off %u untested %u); composited matched %u plain %u none %u; stood down %u",
                         p.press, p.item, p.scope, p.eligibleTicks, p.armedTicks, p.door.armed[0], p.door.armed[1], p.door.noQuad[0], p.door.noQuad[1], p.taken[0], p.taken[1],
                         p.pathSeeded, p.pathReadOnly, p.pathTestsOff, p.pathUntested, p.matched, p.plain, p.none, p.standDowns);
}

}  // namespace crisp
}  // namespace edvr
