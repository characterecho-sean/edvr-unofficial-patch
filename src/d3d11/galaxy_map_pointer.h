// The VR Galaxy Map's pointer (docs/galaxy-map-vr-2026-11-03.md, Fix 3): the hover reads the arrow's direction, and the panels and the arrow are
// drawn at the stars' depth (map_ui_depth.h). Two game hooks make it: a capture of the mouse cursor object (its per-frame update) and a gate-first
// relay on the projector the hover and the stars share. mapPointerFrame runs once a frame on the render thread that installs the other hooks
// (device_hook.cpp, tkMapCamera, after mapCameraFrame): it installs both hooks (bounded retries), opens the projector's gate only while the Galaxy
// Map is open, and says the 30 s line. If either hook cannot arm, both the hover and the panels' depth stay off. VR profile only: a live switch to
// flat closes the gate.
#pragma once

#include <atomic>

namespace edvr {

void mapPointerFrame() noexcept;
// Both hooks are in, so the hover and the panels' depth may act.
bool mapPointerArmed() noexcept;

// Armed and the Galaxy Map open, as of the last frame tick. The draw path reads only this: one relaxed load while no map is open.
extern std::atomic<bool> g_mapUiDepthOpen;
inline bool mapUiDepthOpen() noexcept { return g_mapUiDepthOpen.load(std::memory_order_relaxed); }

} // namespace edvr
