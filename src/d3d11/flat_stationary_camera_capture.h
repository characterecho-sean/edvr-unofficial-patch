#pragma once

#include <cstdint>

namespace edvr {

// Temporary, passive F1 instrument. Frame/SceneCandidate/Selected run on the
// flat Present thread. A SceneCandidate is pre-admission evidence, never a
// selected GPU donor. Candidate/rejection callbacks accumulate one frame;
// Frame finalizes that completed frame before the runtime starts the next.
// Rejection dominates, and missing callbacks reset the arming budgets.
// Refresh uses the existing camera-refresh pre-hook.
void flatStationaryCameraFrame(uint64_t completedFrame) noexcept;
void flatStationaryCameraSceneCandidate(uint64_t frame, const float camera[6][4],
                                        const void* depth) noexcept;
void flatStationaryCameraSceneRejected(uint64_t frame, const char* reason) noexcept;
void flatStationaryCameraSelected(uint64_t frame, const float camera[6][4],
                                  bool reset, bool menuContract, const void* depth) noexcept;
void flatStationaryCameraRefresh(uintptr_t camera, uintptr_t ctx, uint64_t callNo,
                                 uint64_t callerRva, uint32_t kind) noexcept;

} // namespace edvr
