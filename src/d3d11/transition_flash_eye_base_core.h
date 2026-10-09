#pragma once
// Pure logic for the transition-flash engine fix (docs/design-transition-
// flash-engine-fix-2026-09-23.md). No Windows header, no game memory, no
// CodeHook -- takes plain data in and hands plain data back, so
// tools\transition_flash_prevent_test can drive it without the game.
// transition_flash_eye_base.cpp is the only includer that also touches the
// process.
//
// The mechanism (flights 050558 and 095137): the camera driver
// FUN_1428431d0 copies a 4x4 "mailbox" at ship+0x3330..+0x336F into a local
// before composing the eye views and (mode != 1) resets it to the identity
// constant below right after. The HMDCamera tick that refills it is absent on
// the frame after a camera switch (the camera is deactivated one frame and
// re-activated after the consume), so that frame's consume reads the reset
// value, the eye composes against the head pose alone, and the frame is
// drawn from the wrong place -- the flash. The fix recognises that consume
// (the ENTRY of a run of reset mailboxes) and holds the frame.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace tfeb {

// ---------------------------------------------------------------------------
// The reset value ship+0x3330..+0x336F is set to on every (mode != 1) call:
// rows 0-2 from the identity constant at VA 0x1450C8090 (verified against
// analysis\EliteDangerous64.exe's own bytes -- canonical 0x3F800000/
// 0x00000000, not some other bit pattern that merely prints as 1/0), row 3
// (the translation) zeroed by the two qword stores at +0x3360/+0x3368.
inline constexpr float kResetMailbox[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
};

// Bit-exact, not value-exact: a writer that stores a numerically-equal-but-
// differently-bitted value (-0.0 where the constant holds +0.0, say) has
// still WRITTEN, and value equality (-0.0f == +0.0f under IEEE ==) would hide
// that. memcmp on the raw bytes also does the right thing for NaN -- any NaN
// bit pattern differs from the constant's finite bits.
inline bool bitEquals16(const float a[16], const float b[16]) noexcept {
    return std::memcmp(a, b, sizeof(float) * 16) == 0;
}

inline bool isResetMailbox(const float m[16]) noexcept {
    return bitEquals16(m, kResetMailbox);
}

// ---------------------------------------------------------------------------
// ENTRY and EXIT of a run of un-refilled consumes. A low wake's idle stretch
// is thousands of consumes long (flight 091726: 5,041) -- only the EDGES are
// informative. ENTRY is the first un-refilled mode!=1 consume after a refilled
// one (a single-frame skip is its own entry, with no exit to follow); EXIT is
// the first refilled mode!=1 consume after a run of at least kModeSwitchExitRun
// consecutive un-refilled ones.
//
// updateConsecutiveUnrefilled: a mode==1 call carries no information about
// the mailbox either way and leaves the streak untouched.
// classifyModeSwitchEdge takes the streak's value from BEFORE this call folds
// in (updateConsecutiveUnrefilled's own `counter` argument, not its return),
// so entry/exit are each named on the one call that crosses the edge, not on
// every call inside the run.
inline constexpr uint32_t kModeSwitchExitRun = 30;

enum class ModeSwitchEdge : uint8_t { None, Entry, Exit };

inline uint32_t updateConsecutiveUnrefilled(uint32_t counter, int32_t gameMode, bool unrefilled) noexcept {
    if (gameMode == 1) return counter;
    if (!unrefilled) return 0u;
    return counter < 0xFFFFFFFFu ? counter + 1 : counter;
}

inline ModeSwitchEdge classifyModeSwitchEdge(int32_t gameMode, bool unrefilled,
                                              uint32_t consecutiveUnrefilledBeforeThisCall) noexcept {
    if (gameMode == 1) return ModeSwitchEdge::None;
    if (unrefilled) {
        return consecutiveUnrefilledBeforeThisCall == 0 ? ModeSwitchEdge::Entry : ModeSwitchEdge::None;
    }
    return consecutiveUnrefilledBeforeThisCall >= kModeSwitchExitRun ? ModeSwitchEdge::Exit : ModeSwitchEdge::None;
}

// ---------------------------------------------------------------------------
// The scene constant buffer the render tap reads: 5376 bytes, cb1[275] (floats
// [1100..1102]) the eye origin. On the bad frame it lands on the head pose
// alone -- within a metre of the frame origin -- while every ordinary eye
// origin is hundreds of metres to kilometres out in a ship's frame.
inline constexpr uint32_t kSceneCBBytes = 5376;
inline constexpr int kSceneCBOriginFloat = 1100;
inline constexpr float kHeadOnlyRadius2 = 1.0f;   // |row 275|^2 under this = head-only (1 m)

inline bool isHeadOnlyEye(const float origin[3]) noexcept {
    const float r2 = origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2];
    return r2 < kHeadOnlyRadius2;     // NaN compares false: not head-only
}

// ---------------------------------------------------------------------------
// The event window. The consume at frame `skip` (the ENTRY) is the skipped
// one; the render tap that drew from it is at tap frame skip+1 (consume frame
// numbers lag render taps by about one wall-clock frame -- the patch sim
// measured it 6 of 6 on flight 134813). kEventWindowFrames covers
// the taps N..N+2 around that with a frame of slack either side.
inline constexpr uint32_t kEventWindowFrames = 3;

inline bool eventWindowCovers(uint32_t frame, uint32_t skipFrame) noexcept {
    return frame > skipFrame && frame - skipFrame <= kEventWindowFrames;
}

// At most two bad renders per event, and only while the gap lasts. The render
// whose tap frame is T drew from the consume of frame T-1: T = skip+1 always,
// and T = skip+2 exactly when the consume of skip+1 was still a gap consume.
// `gapLastConsume` is the last gap (un-refilled mode-2) consume of the run.
inline constexpr uint32_t kEngineMaxFrames = 2;
inline bool engineActFrame(uint32_t tapFrame, uint32_t skipFrame, uint32_t gapLastConsume) noexcept {
    if (tapFrame < skipFrame + 1u || tapFrame > skipFrame + kEngineMaxFrames) return false;
    return gapLastConsume >= skipFrame && tapFrame - 1u <= gapLastConsume;
}

}  // namespace tfeb
}  // namespace edvr
