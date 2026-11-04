#pragma once

#include <cstdint>

namespace edvr {
// Manual eye-run observer. Arm queues only; hook installation occurs at the next
// Present boundary. All matrices are passive observations, never game getters.
void flatStationaryCameraEyeArm(uint32_t selectedLedgerFrame, const wchar_t* captureStamp) noexcept;
void flatStationaryCameraEyeBoundary(uint32_t ledgerFrame, uint32_t nativeRenderFrame) noexcept;
bool flatStationaryCameraEyeWrite(const wchar_t* path) noexcept;
void flatStationaryCameraEyeEnd() noexcept;
void flatStationaryCameraEyeRefreshPre(uintptr_t returnSlot, uintptr_t camera, uintptr_t ctx,
                                      uintptr_t view, uint64_t callNo, bool offThread) noexcept;
void flatStationaryCameraEyeRefreshPost(uintptr_t camera, uint64_t callNo, bool injected) noexcept;

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
                                 // Already observed against the loaded module; never resolve as a build binding.
                                 uint64_t observedCallerOffset, uint32_t kind) noexcept;

// The VR map camera (docs/galaxy-map-vr-2026-11-03.md, Fix 2). In the VR profile the renderer hook stays installed and reads only stage 0 of each camera
// source ("GalaxyMapView", "SystemMapView"), publishing its head-free pose across threads. mapCameraFrame runs once a frame on the render thread that
// installs the other hooks (device_hook.cpp, its own tick, tkMapCamera, in presentFrameBoundary): it installs the hook (bounded retries), decides "open" (a view published this frame
// or within the last 8) and says the stage names it saw. The readers below are safe from any thread. In the flat profile the watch is off and they say
// closed, 0 changes and no pose.
struct MapCameraPose {
    float rows[3][3];
    float position[3];
};
void mapCameraFrame() noexcept;
bool mapCameraGalaxyOpen() noexcept;
bool mapCameraAnyOpen() noexcept;
// Publishes that moved the map: a different view, or a different bit in the rows or position of the same view's last publish.
uint64_t mapCameraChanges() noexcept;
bool mapCameraPose(MapCameraPose& out) noexcept;

} // namespace edvr
