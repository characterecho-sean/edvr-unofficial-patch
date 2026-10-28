// F157 (native on-foot VR): the coat outfit's pack draw, left out of both eyes. Header only, no game or D3D11 state, so native_frame_test runs the very functions the draw thread and
// the frame thread use.
//
// The coat outfit draws a backpack and an antenna that come into the eye with the head turned toward the left shoulder and with the kinetic AR in the sights. They belong to
// EVASuit_Gear_Back, the part the coat's torso needs (F142, F144), so the part stays promoted and the pack's own draw is left out instead. The body-draw dial of F154 to F156 found the
// draw: in flight edvr_gfx_20261028_105512 it was entry 79, with a rifle drawn and the antenna in view, and holding the dial on it (55 to 59 frames a second in each eye, from
// 10:57:57 for 10 s) took the antenna out of the view. Its vertex shader, pixel shader, index count and instance count are the identity; its index buffer, start index and base vertex
// change from frame to frame (the game moves its meshes about a pooled index buffer), so they are not part of it. The neighbouring entry 78 (index count 3474, the same shaders)
// stays drawn.
//
// The draw is left out when native on foot is active and F144's Gear_Back decision for the local avatar is the outer garment (the coat), and only for a draw into an eye-sized
// target with stencil ref 21 (the local player's body, F18: another person draws the same meshes with another ref). Each requirement refuses on its own.
#pragma once

#include <atomic>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f157 {

// The coat outfit's pack: the identity of its draw (flight edvr_gfx_20261028_105512, entry 79: first seen at index buffer 1DF15DD06A0, start index 1866026, base vertex 498878).
constexpr uint64_t kCoatPackVs = 0x7B0DC42D383F694Cull;   // the vertex shader's content hash
constexpr uint64_t kCoatPackPs = 0x0DF03E64DF9DBEF1ull;   // the pixel shader's content hash
constexpr uint32_t kCoatPackIndexCount = 30312;           // index count per instance
constexpr uint32_t kCoatPackInstances = 1;                // instance count
constexpr uint32_t kStencilRef = 21;                      // the local player's body

// The cheap part of the match, from the draw's own arguments: the index count and the instance count. The draw path asks this before it reads anything from the context.
inline bool countsMatch(uint32_t indexCount, uint32_t instances) noexcept {
    return indexCount == kCoatPackIndexCount && instances == kCoatPackInstances;
}

// The identity: the shaders bound, the index count and the instance count.
inline bool packIdentity(uint64_t vs, uint64_t ps, uint32_t indexCount, uint32_t instances) noexcept {
    return vs == kCoatPackVs && ps == kCoatPackPs && countsMatch(indexCount, instances);
}

// The coat outfit is worn and drawn: native on foot is active and F144 decided the local avatar's Gear_Back is the outer garment (promoted and unclipped:
// f144::gearBackPromoted of nativeOnFootGearBack(), which is true for the outer garment alone).
inline bool coatArmed(bool nativeOnFootActive, bool gearBackOuterGarment) noexcept { return nativeOnFootActive && gearBackOuterGarment; }

// The draw is the coat's pack in an eye and is left out: the coat outfit armed, an eye-sized render target, stencil ref 21 and the identity, all together.
inline bool hides(bool armed, bool eyeTarget, uint32_t stencilRef, uint64_t vs, uint64_t ps, uint32_t indexCount, uint32_t instances) noexcept {
    return armed && eyeTarget && stencilRef == kStencilRef && packIdentity(vs, ps, indexCount, instances);
}

// The eye a draw went into, for the counts: the target's table could not always name it, and an eye it could not name still counts as left out.
inline int eyeSlot(int eye) noexcept { return eye == 0 ? 0 : eye == 1 ? 1 : 2; }

// The draws left out, by eye (0, 1, unnamed) and the frames with at least one, since the frame thread last took them. The draw thread is the one writer (a draw at a time); the
// frame thread takes the counts and starts them again.
class SkipCounts {
public:
    struct Taken {
        uint32_t eye[3] = {0, 0, 0};
        uint32_t frames = 0;
        uint32_t total() const noexcept { return eye[0] + eye[1] + eye[2]; }
    };

    SkipCounts() noexcept {
        for (int k = 0; k < 3; ++k) skipped_[k].store(0, std::memory_order_relaxed);
    }

    // The draw thread: one draw left out in `eye`, in frame `frame` (the frame is counted once however many draws it held).
    void count(int eye, uint32_t frame) noexcept {
        skipped_[eyeSlot(eye)].fetch_add(1, std::memory_order_relaxed);
        if (lastFrame_.load(std::memory_order_relaxed) != frame) {
            lastFrame_.store(frame, std::memory_order_relaxed);
            frames_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // The frame thread: what was left out since the last take, and the counts started again.
    Taken take() noexcept {
        Taken t;
        for (int k = 0; k < 3; ++k) t.eye[k] = skipped_[k].exchange(0, std::memory_order_relaxed);
        t.frames = frames_.exchange(0, std::memory_order_relaxed);
        return t;
    }

private:
    std::atomic<uint32_t> skipped_[3];
    std::atomic<uint32_t> frames_{0};
    std::atomic<uint32_t> lastFrame_{~0u};   // the writer's: the last frame a skip was counted in
};

}  // namespace f157
}  // namespace nofoot
}  // namespace edvr
