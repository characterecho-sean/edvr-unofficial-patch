#pragma once
// advanced.explorer_cam_probe (docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 0b: instruments built").
//
// A TEMPORARY, LOG-ONLY instrument for the Explorer Cam redesign's flight F0, removed when the arc closes. With the key
// off (the default) nothing here runs and no hook is installed. With it on, three instruments report what the free
// camera is doing; none writes the game's memory, a render state or a camera:
//   I3  an observer on FreeCameraActivity's update (EliteDangerous64.exe+0x1071980, build 332841): the original is called
//       first and the activity's pose and state bytes copied after it (explorer_cam_probe_core.h). The hook is Explorer Cam's
//       (explorer_cam.cpp; one target gets one CodeHook) and the probe attaches to it;
//   I1  the VR camera census (advanced.vr_camera_census, switched on implicitly) tallied per camera kind and call site;
//   I2  the 5376-byte scene blocks, fingerprinted for a skinned object: the nearest is the candidate commander root.
// The pure half (the decisions, the seqlock, every log line's text) is explorer_cam_probe_core.h, driven by
// tools\explorer_cam_probe_test.
#include <cstdint>

namespace edvr {

// The scene block's size: the camera buffer the temporal pass and the flash detector already tee (glitch_frame.cpp's
// advanced.camera_buffer_bytes default).
constexpr uint32_t kExplorerCamProbeSceneBlockBytes = 5376;

// Once a frame at the Present boundary (device_hook.cpp, tkExplorerCamProbe, right after explorerCamFrameBoundary). Reads the key (so it is live), attaches to
// the free-camera hook (installed by explorer_cam.cpp the first time anyone wants it) when the key is on, and runs the consumer: the 5 s heartbeats, the immediate change lines and the
// 1 Hz detail lines. Render thread. Never call it from inside a game hook.
void explorerCamProbeFrameBoundary(uint32_t frameNo);

// I2's tee, from vscreen.cpp's Map and Unmap hooks. Wants() is true only while the key is on; the caller then keeps the
// mapped pointer of a 5376-byte buffer and hands it to Note() at Unmap, before the real Unmap (the same rule as every
// tee in that hook). Note() reads at most a dozen floats and writes nothing.
bool explorerCamProbeWantsSceneBlocks();
void explorerCamProbeNoteSceneBlock(const void* resource, const void* data, uint32_t bytes);

}  // namespace edvr
