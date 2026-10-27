// World scale (visual.world_scale, docs\world-scale.md): the one number that moves the two eye origins Elite is told about, about their midpoint.
//
// A percent. 100 is Elite's own scale and changes nothing. Above 100 the eyes are closer together and the world looks bigger; below 100 they are
// further apart and it looks smaller: the direction and the units of OpenXR Toolkit's and SteamVR's world scale. Head movement, orientation and field of
// view are not touched, and the layer EDVR hands the compositor keeps the located eyes (geometry_snapshot.h says where the scale is applied).
//
// The graphics half reads the key and hands it to the runtime in EdvrNativeFrameOutput::worldScalePercent (a version 11 answer; native_frame.h). The runtime
// latches it once a frame. Anything that measures the eye pair Elite drew with takes its bounds from worldScaleEyeFactor() rather than from fixed centimetres.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

constexpr uint32_t kWorldScaleDefault = 100, kWorldScaleMin = 50, kWorldScaleMax = 200;

inline bool worldScaleInRange(uint32_t percent) { return percent >= kWorldScaleMin && percent <= kWorldScaleMax; }

// A percent as a reader believes it: 0 (a word no provider wrote, as an older answer leaves it) is the default, anything else is held to the range.
inline uint32_t worldScaleClamp(uint32_t percent) {
    if (percent == 0) return kWorldScaleDefault;
    return percent < kWorldScaleMin ? kWorldScaleMin : percent > kWorldScaleMax ? kWorldScaleMax : percent;
}

// What the distance between Elite's two eyes is multiplied by: the located separation times this is the one Elite draws with. 1 at 100, 0.77 at 130.
inline double worldScaleEyeFactor(uint32_t percent) { return worldScaleInRange(percent) ? 100.0 / static_cast<double>(percent) : 1.0; }

// The two lines the runtime writes (tools\edvr_log.py --health reads both, and its self-test renders them from these literals). The first is said when the value
// latched for the frame changes: the located eye separation and the one Elite is told. The second is said by the first answer to Elite's eye-transform call that
// carries a new value, with the geometry sequence it was answered from. A run that stays at 100 writes neither.
inline int worldScaleChangeLine(char* out, size_t size, uint32_t percent, double locatedMm, double eliteMm) {
    return std::snprintf(out, size, "world_scale: %u%% located_icd=%.1fmm elite_icd=%.1fmm", percent, locatedMm, eliteMm);
}
inline int worldScaleReadLine(char* out, size_t size, uint32_t percent, unsigned long long sequence) {
    return std::snprintf(out, size, "world_scale: elite read eye transform at %u%% (geometry sequence %llu)", percent, sequence);
}

}  // namespace edvr
