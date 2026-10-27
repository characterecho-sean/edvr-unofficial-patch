// F122 (native on-foot VR): the on-foot panel mouse cursor, hidden by default, pure parts. Header only, no game or D3D11 state, so native_frame_test runs
// the very functions the placement hook uses. Journal: the arc doc, 2026-10-25, F122.
//
// On a locker's panel and the other on-foot panels the game draws a cursor sprite at the position it writes at the pointer controller's +0x140 (in the pick
// camera's frame: x right, y up, z forward), after its placement on a panel sets the hit byte at +0x100. F117 and F120 moved that sprite to where the
// uv put it on the drawn panel; the flight of F120 left it 258 mm off its row at the median, unexplained, and the pilot parked the cursor: hidden by
// default behind one advanced key. Hiding is the same kind of write F117 already made: the sprite's position, nothing else. The clicks go through the
// uv the cursor window returns and the mouse NDC at +0x84/+0x88, which are never written, so they land where the game's pointer is.
//
// Where it goes: behind the pick camera (z < 0) and so far to the side that its tangents leave every field of view whichever way the draw reads the
// position (projected through the camera, normalised and set at a depth, or offset after the projection). The draw then culls it.
#pragma once

#include "native_on_foot_f111.h"   // kOffHit, kOffPos

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f122 {

constexpr float kHidden[3] = {1.0e5f, 1.0e5f, -1.0f};   // x, y, z in the pick camera's frame

// The key: on draws the game's cursor (F117 panel and F120 panel, fixed); anything else, and the shipped default, hides it.
inline bool keyOn(const char* text) noexcept { return text && std::strcmp(text, "on") == 0; }

// Whether a sprite position cannot be seen from the pick camera: behind it, and more than 1000 times further off the axis than it is behind.
inline bool unseen(const float p[3]) noexcept {
    if (!(p[2] < 0.0f)) return false;
    const float depth = -p[2];
    return std::fabs(p[0]) >= 1000.0f * depth && std::fabs(p[1]) >= 1000.0f * depth;
}

// What a placement that reached the controller does with its sprite.
enum class Act : uint8_t { kNoHit = 0, kLeave, kFlat, kHide };
inline const char* actName(Act a) noexcept {
    switch (a) {
        case Act::kNoHit: return "no hit (no panel reached)";
        case Act::kLeave: return "left (the key is on)";
        case Act::kFlat: return "left (the flat profile)";
        default: return "hidden";
    }
}
inline Act decide(bool mouseOn, bool flat, bool hit) noexcept {
    if (!hit) return Act::kNoHit;
    if (mouseOn) return Act::kLeave;
    if (flat) return Act::kFlat;
    return Act::kHide;
}

// The one write: the sprite's position at ctl + kOffPos, behind the camera first (z), then to the side (x, y), so a write that stops part way has already
// taken the sprite out of view. Nothing else is written. writeFloat(address, value) returns false when the write was refused.
template <class WriteFloat>
inline bool writeHidden(uintptr_t ctl, WriteFloat&& writeFloat) {
    const int order[3] = {2, 0, 1};
    for (int k : order)
        if (!writeFloat(ctl + f111::kOffPos + 4u * static_cast<unsigned>(k), kHidden[k])) return false;
    return true;
}

// What to count for a placement: a panel seen (a hit), a sprite hidden, or a hide whose write failed.
struct Delta {
    uint32_t seen = 0, hidden = 0, failed = 0;
};
inline Delta delta(Act a, bool wrote) noexcept {
    Delta d;
    if (a == Act::kNoHit) return d;
    d.seen = 1;
    if (a == Act::kHide) {
        if (wrote) d.hidden = 1;
        else d.failed = 1;
    }
    return d;
}

// Which of the pointer instrument's hooks the frame thread opens. The placement hook is the one that hides, so it is open on foot whenever the key hides
// (VR) or the instruments run; the update, ray hit, cursor window and owner transform hooks, and with them F111's and F114b's readings and F117's and
// F120's corrections, run only with the key on.
struct Gates {
    bool instruments = false, placement = false;
};
inline Gates gates(bool open, bool mouseOn, bool flat) noexcept {
    Gates g;
    g.instruments = open && mouseOn;
    g.placement = g.instruments || (open && !mouseOn && !flat);
    return g;
}

}  // namespace f122
}  // namespace nofoot
}  // namespace edvr
