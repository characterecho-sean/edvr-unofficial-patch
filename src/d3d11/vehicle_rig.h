// The player vehicle's rigs, and the vehicle frame they supply. It hooks ICameraTargetShip::SetTarget (0x1096640 in build 334925, found by signature), learns the player's ship and cockpit
// IKinematicRig interfaces, re-reads each rig's record array from the game's own data every few ms, and takes the pose of the cockpit rig's root record at the emit (the vehicle frame the flat
// resolve uses, vehicle_frame.h). It writes nothing into the game and changes no draw, state or shader. The pure half is vehicle_rig_core.h.
//
// Both profiles install it. A build that is not 334925, or whose signatures or data addresses do not resolve, installs nothing and says why in one line (see vehicle_rig.cpp for the lines it writes).
#pragma once

#include <atomic>
#include <cstdint>

#include "vehicle_frame.h"

namespace edvr {

// Once at start-up, from the proxy's device creation, in both profiles.
void vehicleRigInstall();
// From device_hook's poll: the 30 s health line, and a refresh of the rigs when no emit is refreshing them.
void vehicleRigPoll(uint64_t nowMs);

// The flat profile's vehicle frame (vehicle_frame.h): the cockpit rig root's pose at this frame's emit and last frame's, and its sphere in its own frame. `stamp` is the present-frame clock the emit
// folds into its markers (engineVelocityPresentClock; the value the prep reads as EN[276].x). Anything but kOk leaves `out` alone and the resolve on the old path. Counts into the window below.
struct VehicleFrameRead {
    vframe::Frame frame;
    float sphere[4] = {};   // centre xyz, radius, in the root's own frame
};
vframe::Why vehicleRigFrameTake(uint32_t stamp, VehicleFrameRead* out);
// The counts since the last call (they restart), for the flat runtime's 30 s line; the newest ready rig's name and root sphere radius.
// fromOlder: of `with`, the frames that came from a rig older than the one the camera target names now (the countdown and the tunnel, vframe::kRecentRigs).
struct VehicleFrameWindow {
    uint64_t asked = 0, with = 0, fromOlder = 0, missing = 0, notAdjacent = 0, changed = 0;
    bool live = false, rigKnown = false;
    float sphereRadius = 0;
    char name[48] = {};
};
void vehicleRigFrameWindow(VehicleFrameWindow* out);

// The flat pixel capture's tap (flat_pixel_capture.h calls it at "flat pixels: copied", on the render thread, with the capture's frame and camera row 5, the eye). An inline cell so the
// capture header links to nothing; null until the hook is live.
using VehicleRigCaptureFn = void (*)(uint64_t frame, const float* eye) noexcept;
inline std::atomic<VehicleRigCaptureFn> g_vehicleRigCaptureTap{nullptr};

}  // namespace edvr
