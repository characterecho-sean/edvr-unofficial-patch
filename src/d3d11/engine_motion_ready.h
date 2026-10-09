#pragma once
// What Explorer Cam's entry fade waits for besides the placement (explorer_cam_fade_core.h, "MOTION"): the engine's motion for the eye path. Said by
// engine_velocity.cpp as of the last frame boundary; read by the frame thread once a frame. The explorer cam rig supplies its own, scripted.
#include <cstdint>

namespace edvr {

struct EngineMotionReady {
    bool armed = false;      // the engine-motion path is live (the feature on, its emit hook up) AND the temporal pass has asked for its views within the last 30
                             // frames (it consumes them): there is something to wait for. False: nothing is (a pass that never asks would never be satisfied).
    uint32_t viewsRun = 0;   // consecutive frames at whose boundary BOTH eyes had been handed the engine-motion views since the boundary before
    bool skinJobs = false;   // the last frame's palette chain ran with jobs: a character is loaded (the second skin is on)
    bool skinLive = false;   // ...and the second skin's join was live for that frame
};

EngineMotionReady engineMotionReady();

}  // namespace edvr
