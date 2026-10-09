// The on-foot maps gate, the pure half (docs/design-world-camera-motion-2026-09-30.md, Phase 1; the runtime is in ui_layer.cpp).
//
// WHY. On foot Odyssey draws everything, the world and the galaxy and system maps alike, into ONE flat 2D screen and
// shows it to each eye through one composite draw. The layer's world-screen gate (ui_layer_math.h: the journal's on-foot
// reading OR the screen's own busy depth) keeps that screen in the eye route, where the temporal pass filters it with
// motion the world camera supplies -- and no world camera names a map. So a dragged map is filtered as a still picture
// at infinity and smears. With the maps gate on (it always is) the question changes: the panel is the world only
// while a draw that reads the world camera NAMES it (screen_motion.cpp: the world's own terrain or pool draw into the
// screen-sized depth names the camera and depth screen motion maps to eye pixels). Anything else the 2D screen shows,
// a map or a menu, is not the world: the layer takes the composite, sharp, after the upscaler, and the eye that holds
// nothing else skips the upscaler altogether (the layer-only door the VR world route already uses).
//
// WHAT IS HERE, all pure (tools\on_foot_maps_test drives every function, with a mutation list for the rules):
//   - the gate's setting: on (always, since 2026-10-01) or off, and a typo reads as off;
//   - the step: 2 frames in a row that named the source hold the panel as the world, 3 in a row that did not release it
//     (3 is the VR world route's grace, kVrWorldGraceFrames: the route lets go on the same boundary);
//   - the gate's combine: with the key off the gate is today's byJournal || byDepth for every input;
//   - the door's predicate: the layer holds an eye's WHOLE picture when it TOOK that eye's 2D screen composite in this
//     sequence and every draw the game made into an eye-sized target this frame was taken;
//   - the 5 s window and the text of every line the feature logs (the reader, tools\edvr_log.py --maps-sharp, parses them).
// It never touches D3D: the runtime hands it facts.
//
// THE KEY-OFF CONTRACT. With the key off nothing here is ever consulted: uiMapsGateHeld answers byJournal || byDepth,
// the door predicate answers No, and the runtime logs nothing (tools\on_foot_maps_test and tools\ui_layer_world_test pin it).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

// ---- the key ----------------------------------------------------------------------------------------------------------
// The gate's setting: on or off. On since 2026-10-01. A text that is not "on" reads as off, so a typo
// leaves on-foot VR exactly as it was before the gate.
enum class UiMapsKey : uint8_t { Off, On };
inline UiMapsKey uiMapsKeyFromText(const char* text) {
    if (!text) return UiMapsKey::Off;
    const char* want = "on";
    for (; *want; ++want, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *want) return UiMapsKey::Off;
    }
    return *text == 0 ? UiMapsKey::On : UiMapsKey::Off;
}
inline const char* uiMapsKeyName(UiMapsKey key) { return key == UiMapsKey::On ? "on" : "off"; }

// ---- the step ---------------------------------------------------------------------------------------------------------
// Frames in a row whose draws named the screen's source before the panel counts as the world, and frames in a row that did
// not before it stops. Release equals the VR world route's grace (vr_world_route_math.h kVrWorldGraceFrames, asserted in
// tools\vr_world_route_test) so the route and the gate let go on one boundary; hold is the shortest run that is not one
// stray frame.
constexpr uint32_t kUiMapsHoldFrames = 2;
constexpr uint32_t kUiMapsReleaseFrames = 3;

struct UiMapsGate {
    bool world = false;   // the panel is the world: the last step held it
    uint32_t run = 0;     // frames in a row toward the other state
};
enum class UiMapsEdge : uint8_t { None, Hold, Release };

// One frame's verdict (named: a world camera's draw named the screen's source in the frame that ended). Returns the edge
// this frame made, if any; g.world is the gate for the frame that starts.
inline UiMapsEdge uiMapsStep(UiMapsGate& g, bool named) {
    if (!g.world) {
        if (!named) {
            g.run = 0;
            return UiMapsEdge::None;
        }
        if (++g.run >= kUiMapsHoldFrames) {
            g.world = true;
            g.run = 0;
            return UiMapsEdge::Hold;
        }
        return UiMapsEdge::None;
    }
    if (named) {
        g.run = 0;
        return UiMapsEdge::None;
    }
    if (++g.run >= kUiMapsReleaseFrames) {
        g.world = false;
        g.run = 0;
        return UiMapsEdge::Release;
    }
    return UiMapsEdge::None;
}

// The gate's combine. decide: the key is on, the layer is live and screen motion is live -- the world camera's naming alone
// decides. Otherwise the gate is today's, for every input.
inline bool uiMapsGateHeld(bool decide, const UiMapsGate& g, bool byJournal, bool byDepth) {
    return decide ? g.world : (byJournal || byDepth);
}
// The key came on (or the layer and screen motion became live) while today's gate held or did not hold the screen: carry
// that, so the first frames of a switch do not hand a held world to the layer for the length of a hold.
inline void uiMapsCarry(UiMapsGate& g, bool heldNow) {
    g.world = heldNow;
    g.run = 0;
}

// ---- the door ---------------------------------------------------------------------------------------------------------
// The layer holds an eye's whole picture, so the door hands the black frame on and runs no upscaler for it, when the layer
// TOOK this eye's 2D screen composite in this sequence (takenSeq, the layer's mark; 0 never) and every draw the game made
// into an eye-sized target this frame was taken by the layer (eyeDraws counts both eyes' draws through the hooks, taken the
// layer's own count): nothing else is in the eye's image for the upscaler to filter. NotEmpty is that take with something
// else in the eye: the eye keeps the upscaler, exactly as it does without the key.
enum class UiMapsDoor : uint8_t { No, Yes, NotEmpty };
inline UiMapsDoor uiMapsDoor(uint64_t takenSeq, uint64_t seq, uint32_t eyeDraws, uint32_t takenDraws) {
    if (seq == 0 || takenSeq != seq) return UiMapsDoor::No;
    return eyeDraws <= takenDraws ? UiMapsDoor::Yes : UiMapsDoor::NotEmpty;
}

// ---- the 5 s window ---------------------------------------------------------------------------------------------------
// Counted while the key is on, zeros included: an absent line is what "the code never ran" looks like.
struct UiMapsWindow {
    uint32_t frames = 0;        // frames the gate judged by naming (the key on, the layer and screen motion live)
    uint32_t named = 0;         // ... in which a world camera named the screen's source
    uint32_t unnamed = 0;       // ... in which none did
    uint32_t worldFrames = 0;   // frames the verdict for the next frame was "the world"
    uint32_t panelFrames = 0;   // ... and "a panel the layer takes"
    uint32_t holds = 0;         // panel -> world edges
    uint32_t releases = 0;      // world -> panel edges
    uint32_t screenTakes = 0;   // 2D screen composites the layer took while the gate gave it the panel
    uint32_t recognised = 0;    // ... of which screen motion's recognition matched the composite's shader pair
    uint32_t doorLayerOnly = 0; // eyes whose door ran layer-only for a taken panel (once per eye and sequence)
    uint32_t doorNotEmpty = 0;  // eyes the layer took the screen for but the game drew something else into: the upscaler ran
    uint32_t notLive = 0;       // frames the key was on and the gate could not be decided by naming
    // Every 2D screen composite the layer's decision SAW while the gate was on, taken or not (the route's re-issue, a composite left in
    // the game's frame and a take all count). Next to screenTakes it tells "nothing was drawn" (a cockpit, a load: draws 0) from "drawn
    // and refused" (draws above takes): a window with panel frames and no takes was ambiguous without it. The first flight's boarding is
    // the case (docs\design-world-camera-motion-2026-09-30.md, 8.10).
    uint32_t screenDraws = 0;
    void reset() { *this = UiMapsWindow{}; }
};

// ---- the text of every line ---------------------------------------------------------------------------------------------
// The reader (tools\edvr_log.py --maps-sharp) parses these; the first words of each are its anchors.
inline int uiMapsFormatOn(char* out, size_t size, uint64_t frame, bool carriedWorld, const char* journal) {
    return std::snprintf(out, size,
        "on foot maps sharp: ON at frame=%llu (the maps gate is on): the 2D screen is the world only while a world "
        "camera's draw names its source (%u frames in a row hold it, %u release it); anything else it shows, a map or a menu, is "
        "taken by the UI layer, sharp, after the upscaler. The gate starts as the journal's and the screen's own depth left it: %s "
        "(the journal: %s).",
        static_cast<unsigned long long>(frame), kUiMapsHoldFrames, kUiMapsReleaseFrames,
        carriedWorld ? "the world" : "not the world", journal ? journal : "?");
}
inline int uiMapsFormatOff(char* out, size_t size, uint64_t frame, const char* why, bool wasPanel) {
    return std::snprintf(out, size,
        "on foot maps sharp: OFF at frame=%llu (%s): the 2D screen is the world by the journal's reading or the screen's own depth "
        "again, as without the key%s.",
        static_cast<unsigned long long>(frame), why ? why : "?",
        wasPanel ? "; the layer held a panel at that moment and the gate now decides it again" : "");
}
inline int uiMapsFormatTake(char* out, size_t size, uint64_t frame, uint32_t unnamedRun, uint64_t worldFrames, double worldSeconds,
                            const char* journal) {
    return std::snprintf(out, size,
        "on foot maps sharp: the layer TAKES the 2D screen at frame=%llu: no world camera named its source for %u frames in a row "
        "(after %llu world frames, %.1f s; the journal: %s). A map or a menu is sharp from the layer and an eye that holds nothing "
        "else skips the upscaler (the layer-only door).",
        static_cast<unsigned long long>(frame), unnamedRun, static_cast<unsigned long long>(worldFrames), worldSeconds,
        journal ? journal : "?");
}
inline int uiMapsFormatHandBack(char* out, size_t size, uint64_t frame, uint64_t panelFrames, double panelSeconds,
                                uint64_t eyesLayerOnly, uint64_t eyesNotEmpty, const char* why) {
    return std::snprintf(out, size,
        "on foot maps sharp: the layer HANDS BACK the 2D screen at frame=%llu after %llu panel frames (%.1f s; %llu eyes through "
        "the layer-only door, %llu kept the upscaler because the game drew something else into them): %s.",
        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(panelFrames), panelSeconds,
        static_cast<unsigned long long>(eyesLayerOnly), static_cast<unsigned long long>(eyesNotEmpty), why ? why : "?");
}
inline int uiMapsFormatNamedWhy(char* out, size_t size, uint32_t namedRun) {
    return std::snprintf(out, size, "a world camera named the screen's source for %u frames in a row", namedRun);
}
inline int uiMapsFormatNotLive(char* out, size_t size, const char* why) {
    return std::snprintf(out, size,
        "on foot maps sharp: the maps gate is on but the 2D screen's gate stays the journal's and the screen's own "
        "depth, as without the key: %s.",
        why ? why : "?");
}
inline int uiMapsFormatNotEmpty(char* out, size_t size, uint32_t eye, uint64_t sequence, uint32_t eyeDraws, uint32_t takenDraws) {
    return std::snprintf(out, size,
        "on foot maps sharp: the layer took the 2D screen for eye %u (sequence %llu) but the game drew %u draw(s) into eye-sized targets "
        "this frame and the layer took %u: the eye keeps the upscaler, as without the key.",
        eye, static_cast<unsigned long long>(sequence), eyeDraws, takenDraws);
}
inline int uiMapsFormatWindow(char* out, size_t size, double seconds, const char* mode, bool world, const UiMapsWindow& w) {
    return std::snprintf(out, size,
        "on foot maps sharp 5s: key=on %.0f s mode=%s gate=%s frames=%u named=%u unnamed=%u world-frames=%u panel-frames=%u holds=%u "
        "releases=%u screen-takes=%u recognised=%u door-layer-only=%u door-not-empty=%u not-live-frames=%u screen-draws=%u",
        seconds, mode ? mode : "?", world ? "world" : "panel", w.frames, w.named, w.unnamed, w.worldFrames, w.panelFrames, w.holds,
        w.releases, w.screenTakes, w.recognised, w.doorLayerOnly, w.doorNotEmpty, w.notLive, w.screenDraws);
}

}  // namespace edvr
