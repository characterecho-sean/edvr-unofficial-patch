// The particle billboards' constants shadow, pure: what the Unmap tee copies out of the game's mapped
// scene constant buffer (cb1, 5376 bytes), and when the learned target is let go.
//
// WHY THIS IS NOT A WHOLE-BLOCK MEMCPY (docs/particle-billboards.md, 2026-10-16). The Unmap tee reads the
// mapped pointer the game just wrote, and Elite writes this buffer 60+ times a frame. Mapped dynamic
// constant-buffer memory can be write-combined (uncached for CPU reads): DXVK's is, measured in issue 65
// (docs/issue-65-proton-cb-shadows-2026-10-02.md), and a native AMD driver is suspected. The fix reads
// 10 rows of the block. Copying all 5376 bytes to feed them was the cost.
//
// The rows read, and by whom (particle_fix.cpp):
//   solveCamera  rows 270..273: the clip rows the viewer position is solved from
//   shapeOk      rows 278, 279: the camera's up and forward, both required unit length
// so the copy is rows 270..279, byte offsets 4320..4480 (160 bytes). The shadow array keeps the block's
// own layout, so every reader indexes it exactly as before; the bytes outside the range are never read
// and are not kept. `size` is still the MAPPED size, because both readers gate on it.
#pragma once

#include <cstdint>
#include <cstring>

namespace edvr {
namespace particlecap {

constexpr uint32_t kMaxShadow = 8192;      // the largest block this ever accepts
constexpr uint32_t kMinBlock = 64;         // the smallest
constexpr uint32_t kFirstRow = 270;
constexpr uint32_t kLastRow = 279;         // inclusive
constexpr uint32_t kRangeBegin = kFirstRow * 16;
constexpr uint32_t kRangeEnd = (kLastRow + 1) * 16;
constexpr uint32_t kRangeBytes = kRangeEnd - kRangeBegin;
static_assert(kRangeBytes == 160, "rows 270..279");

// Frames with no plume or flare draw before the learned target is let go. Elite draws a plume or flare
// through the whole of a geyser field or a star approach, so a session near smoke never retires; one far
// from it stops paying for the shadow after about 4 seconds at 80 Hz.
constexpr uint32_t kRetireFrames = 300;

struct Shadow {
    uint8_t  bytes[kMaxShadow] = {};
    uint32_t size = 0;      // the mapped size of the last accepted capture
    bool     valid = false;
};

// One capture. `mapped` is the size the buffer was created with, `data` the pointer Map returned. Copies
// only the rows above that lie inside the mapping, through `copy(dst, src, n)` (a template parameter so a
// rig can count the bytes and check the bounds). Returns the bytes copied, 0 when the mapping is shorter
// than the range or the capture was refused (which also invalidates the shadow).
template <class Copy>
inline uint32_t capture(Shadow& s, bool steady, const void* data, uint32_t mapped, Copy&& copy) {
    if (!steady || !data || mapped < kMinBlock || mapped > kMaxShadow) {
        s.valid = false;
        return 0;
    }
    uint32_t copied = 0;
    if (mapped > kRangeBegin) {
        const uint32_t end = mapped < kRangeEnd ? mapped : kRangeEnd;
        copied = end - kRangeBegin;
        copy(s.bytes + kRangeBegin, static_cast<const uint8_t*>(data) + kRangeBegin, static_cast<size_t>(copied));
    }
    s.size = mapped;
    s.valid = true;
    return copied;
}

// The target's lifetime. The target is learned at a plume or flare draw (particle_fix.cpp particleOnDraw)
// and, until now, kept for the session, so the Map/Unmap tee copied the block on every write whether or not
// any smoke was on screen. noteMatch is told of every matched draw; frameBoundary once per frame.
struct Tracker {
    uint32_t frame = 0;
    uint32_t lastMatch = 0;
    uint64_t retired = 0;
    uint64_t rearmed = 0;
    bool     retiredPending = false;   // retired, and not learned again yet

    void noteMatch() { lastMatch = frame; }

    // True once, when a held target has gone kRetireFrames frames with no match. The caller drops its target.
    bool frameBoundary(bool haveTarget) {
        ++frame;
        if (!haveTarget) return false;
        if (frame - lastMatch < kRetireFrames) return false;
        ++retired;
        retiredPending = true;
        lastMatch = frame;
        return true;
    }

    // The caller learned a target. True when that is a re-arm after a retirement.
    bool noteLearned() {
        if (!retiredPending) return false;
        retiredPending = false;
        ++rearmed;
        return true;
    }
};

}  // namespace particlecap
}  // namespace edvr
