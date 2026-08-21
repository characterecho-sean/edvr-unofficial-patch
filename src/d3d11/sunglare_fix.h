#pragma once

#include <cstdint>

namespace edvr {

class Config;

// The sun-glare element train (the head-coupled glare hunt's terminus):
// ONE DrawInstanced call -- 6 vertices, 15 instances at the sun -- stamps
// every screen-space glare element from two 2048x1024 art sheets: both
// beams, the corona flare, the smudge, the rays. The elements are placed
// and oriented in screen space each frame, so they ride the head in VR;
// the star's own disc is a separate draw and is untouched.
//
// fix.sun_glare = stock keeps the game's behaviour. off skips the train
// entirely. first:K draws only the first K instances -- SV_InstanceID
// restarts at zero per draw call, so a prefix is the only subset that
// keeps every element's identity; the mapping run walks K to name each
// instance, and the kept set is whatever prefix survives the walk.
enum class SunglareAction { kStock, kSkip, kClamp };

void sunglareConfigure(Config& cfg);
bool sunglareWantsDraws();
SunglareAction sunglareOnEyeDraw(char kind, uint32_t count,
                                 uint32_t instances);
uint32_t sunglareKeep();

}  // namespace edvr
