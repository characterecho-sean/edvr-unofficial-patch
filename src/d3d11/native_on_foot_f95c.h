// F95c (native on-foot VR): the commander's head kept in the shadows. The pure parts. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the avatar visibility job's hook (native_on_foot.cpp, eyeProbeHooked) and the fade hook (native_on_foot_hooks.cpp, headHideStep) use. Journal: the arc doc,
// 2026-10-24, F95c.
//
// The game's avatar visibility job (EliteDangerous64.exe+0x3DA58A0, build 334925; rcx = the AvatarModelComponent, rdx = the job context; read with capstone on the
// 334925 exe) zeroes every part instance's view mask (AMC+0x17A0 + idx*0x680 + 8k), then walks the context's views (at +0x40, 0x6A0 bytes each, the count at
// +0x1A940) and ORs the view's bit (view+0x580) into the mask of every instance whose part admits that view. The part's visibility class (its entry +0x670, so
// AMC+0x1820 + idx*0x680; the census's `st`) decides, at 0x3DA5D9A..0x3DA5DE7: 1 every view, 2 the shadow views only, 3 the main views only, anything else none.
// A view's flags (view+0x688) classify it with the game's own test 0x2856680, (flags & 0x7FF0) != 0: class 2 takes such a view when it also has a bit of 0x1030,
// class 3 takes the views without one. The game gives class 2 to a part of this very avatar on its own (EVASuit_Gear_Back, st=2 in the 2026-10-24 13:55 census).
// The class is written only from the avatar's pending requests (0x3D9CA90 and the item apply it calls, 0x3D9D560, each writing it when it differs); the
// visibility job is the only reader found in the avatar code (0x3D90000..0x3DF0000, every +0x670 and +0x1820 operand).
//
// EDVR hides the head by zeroing its masks after the fade (headHideStep), which takes it out of the shadow views too: the shadow is headless. F95c gives the head
// parts class 2 for the length of the local third-person avatar's visibility call and puts the game's class back when it returns, so the game's own rule puts the
// head into the shadow views and no other view. The fade hook then keeps those masks, less every bit a main or other view of that call carries (a guard: a bit two
// views share is taken as the main view's), instead of zeroing them. A call the swap did not cover leaves no ticket, and the fade hook zeroes as before.
#pragma once

#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f95c {

// ---- the job context and its views (build 334925) ----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kViewsOff = 0x40;        // the first view
constexpr uint32_t kViewStride = 0x6A0;     // one view
constexpr uint32_t kViewCountOff = 0x1A940; // the view count, a qword
constexpr uint32_t kViewBitOff = 0x580;     // the view's bit, a qword ORed into an admitted instance's mask
constexpr uint32_t kViewFlagsOff = 0x688;   // the view's flags, a dword
constexpr uint32_t kMaxViews = 64;          // a mask is 64 bits; a larger count is not a context this reads
constexpr uint32_t kShadowish = 0x7FF0;     // 0x2856680: the view is shadow-ish
constexpr uint32_t kShadowCast = 0x1030;    // class 2's second test (0x3DA5DDD)
constexpr uint32_t kOffPartClass = 0x1820;  // AMC + this + idx*0x680: the part's visibility class (avatar_hooks_core.h, kOffAmcPartState)

// ---- the part classes ----------------------------------------------------------------------------------------------------------------------------------------------
constexpr int32_t kClassAll = 1, kClassShadow = 2, kClassMain = 3, kClassNever = 0;

// Whether a part of class c takes a view with these flags: the visibility job's switch, as read.
inline bool admits(int32_t c, uint32_t flags) {
    const bool shadowish = (flags & kShadowish) != 0;
    if (c == kClassAll) return true;
    if (c == kClassShadow) return shadowish && (flags & kShadowCast) != 0;
    if (c == kClassMain) return !shadowish;
    return false;
}
// The class a head part takes through the visibility call: every view becomes the shadow views only; the main views only becomes none (it casts no shadow either way,
// and it must not reach an eye); none and the shadow views only stay; a value the job does not know is left alone.
inline int32_t shadowClass(int32_t c) {
    if (c == kClassAll) return kClassShadow;
    if (c == kClassMain) return kClassNever;
    return c;
}
// The class put back when the call returns: the game's own value, unless the game changed the class during the call (then the game's new value stands).
inline int32_t restoredClass(int32_t now, int32_t set, int32_t was) {
    return now == set ? was : now;
}

// ---- the views of one call ------------------------------------------------------------------------------------------------------------------------------------------
enum class ViewKind : uint8_t { Main, Shadow, Other };   // Main: not shadow-ish (the eyes); Shadow: what class 2 takes; Other: shadow-ish, not class 2's
inline ViewKind viewKind(uint32_t flags) {
    if ((flags & kShadowish) == 0) return ViewKind::Main;
    return (flags & kShadowCast) != 0 ? ViewKind::Shadow : ViewKind::Other;
}
inline uint32_t popcount64(uint64_t x) {
    uint32_t n = 0;
    while (x) {
        x &= x - 1;
        ++n;
    }
    return n;
}
struct Views {
    uint32_t n = 0, main = 0, shadow = 0, other = 0;
    uint64_t shadowBits = 0;   // the bits of the shadow views
    uint64_t deny = 0;         // the bits of every main and other view: never left in a head mask
    uint32_t shared = 0;       // shadow-view bits a main or other view carries too (removed with deny)
};
inline Views splitViews(const uint32_t* flags, const uint64_t* bits, uint32_t n) {
    Views v;
    v.n = n;
    for (uint32_t i = 0; i < n; ++i) {
        switch (viewKind(flags[i])) {
            case ViewKind::Main: ++v.main; v.deny |= bits[i]; break;
            case ViewKind::Shadow: ++v.shadow; v.shadowBits |= bits[i]; break;
            case ViewKind::Other: ++v.other; v.deny |= bits[i]; break;
        }
    }
    v.shared = popcount64(v.shadowBits & v.deny);
    return v;
}
// The head mask the fade hook leaves: no bit of a main or other view.
inline uint64_t guarded(uint64_t mask, uint64_t deny) {
    return mask & ~deny;
}

// ---- the fade hook's step for the head parts --------------------------------------------------------------------------------------------------------------------------
enum class Action : uint8_t { Hold, Zero, Keep };
// write: headHideStep may write now (a placement stands, or native stereo's body in the game's first-person camera). ticket: the swap covered this avatar's latest
// visibility call. Keep leaves the masks to the shadow views (less deny); Zero is the hide as before F95c; Hold writes nothing (the third-person camera, F93d).
inline Action fadeAction(bool write, bool ticket) {
    if (!write) return Action::Hold;
    return ticket ? Action::Keep : Action::Zero;
}

// ---- the swap the visibility hook keeps across the game's call ----------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxParts = 16;
struct Swap {
    uint8_t* amc = nullptr;
    uint32_t n = 0;                 // parts whose class was changed for the call
    uint32_t part[kMaxParts] = {};  // their indices
    int32_t was[kMaxParts] = {};    // the game's class
    int32_t set[kMaxParts] = {};    // the class written for the call
    Views views;
    bool armed = false;             // the views were read and the classes written: End restores and leaves the ticket
};

}  // namespace f95c
}  // namespace nofoot
}  // namespace edvr
