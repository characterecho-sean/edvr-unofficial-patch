// The vehicle rig probe (advanced.vehicle_rig_probe, TEMPORARY, issue 186): a read-only flight instrument. It hooks ICameraTargetShip::SetTarget (0x1096640 in build 334925, found by
// signature), learns the player's ship / ShipCockpit / Turret / module IKinematicRig interfaces, re-reads each rig's record array from the game's own data every few ms, and counts which
// rig each FUN_144315A20 emit belongs to. It logs; it writes nothing into the game and changes no draw, state or shader. The pure half is vehicle_rig_core.h.
//
// Both profiles. Off (the key) installs no hook. A build that is not 334925 reads nothing (one line). See vehicle_rig_probe.cpp for the lines it writes.
#pragma once

#include <atomic>
#include <cstdint>

namespace edvr {

// Once at start-up, from the proxy's device creation, in both profiles. Reads advanced.vehicle_rig_probe once.
void vehicleRigProbeInstall();
// Once a second (device_hook's poll): the 5 s emit line, the 30 s summary, and a refresh of the rig table when no emit is refreshing it.
void vehicleRigProbePoll(uint64_t nowMs);

// The flat pixel capture's tap (flat_pixel_capture.h calls it at "flat pixels: copied", on the render thread, with the capture's frame and camera row 5, the eye). An inline cell so the
// capture header links to nothing; null until the probe is live.
using VehicleRigCaptureFn = void (*)(uint64_t frame, const float* eye) noexcept;
inline std::atomic<VehicleRigCaptureFn> g_vehicleRigCaptureTap{nullptr};

}  // namespace edvr
