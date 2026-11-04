// The VR Galaxy Map's pointer (docs/galaxy-map-vr-2026-11-03.md, Fix 3): the hover reads the arrow's direction, and the map's drawn UI follows (galaxy_map_ui.h: the icons, labels and rings
// fixed in the world, the arrow at the stars' depth when its tip is off the panels). Two game hooks make the hover: a capture of the mouse cursor object (its per-frame
// update) and a gate-first relay on the projector the hover and the stars share. mapPointerFrame runs once a frame on the render thread that installs the other hooks
// (device_hook.cpp, tkMapCamera, after mapCameraFrame): it installs both hooks (bounded retries), opens the projector's gate only while the Galaxy Map is open, publishes
// the draw path's gates (galaxy_map_ui.h setGates) and says the 30 s line. If either hook cannot arm, the hover, the arrow's depth and the panel mask stay off; the icons
// fixed in the world do not need them. VR profile only: a live switch to flat closes the gate.
#pragma once

namespace edvr {

void mapPointerFrame() noexcept;
// Both hooks are in, so the hover and the arrow may act.
bool mapPointerArmed() noexcept;

} // namespace edvr
