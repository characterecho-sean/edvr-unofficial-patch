#pragma once

// The flat CPU census already samples kEngineDraw on the owner/render thread.
// Keep frame-end reference cleanup in that same measured family. The callback
// overload lets the fake-clock rig pin the timing wrapper independently; the
// no-argument overload is the production operation used by the WARP fixture.
#include "flat_cpu.h"

namespace edvr {
void engineVelocityFlatFrameEnd() noexcept;

template <class Release>
inline void engineVelocityFlatFrameEndWithCost(Release&& release)
    noexcept(noexcept(release())) {
    flatcpu::Scope cleanup(flatcpu::kEngineDraw);
    release();
}

inline void engineVelocityFlatFrameEndWithCost() noexcept {
    engineVelocityFlatFrameEndWithCost([]() noexcept {
        engineVelocityFlatFrameEnd();
    });
}
}  // namespace edvr
