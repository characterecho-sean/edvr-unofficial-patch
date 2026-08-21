#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

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
// kMatch: a train draw with nothing to skip or clamp -- the steady path
// still needs to know it happened.
enum class SunglareAction { kStock, kSkip, kClamp, kMatch };

void sunglareConfigure(Config& cfg);
bool sunglareWantsDraws();
bool sunglareSteady();
SunglareAction sunglareOnEyeDraw(char kind, uint32_t count,
                                 uint32_t instances);
uint32_t sunglareKeep();

// The steady wrap around a matched train draw: rotate the shared corner
// stream by the head's roll (measured, never written, from the camera
// rows), bind the rotated copy for this one draw, restore after.
void sunglareBegin(ID3D11DeviceContext* ctx);
void sunglareEnd(ID3D11DeviceContext* ctx);

// The train's identity test, exported for the constant-buffer peek: the
// steer needs to read the 208-byte CB of exactly these draws, and two
// matchers for one family is how the witchstar era learned wrong things.
bool sunglareIsGlareTrain(char kind, uint32_t count, uint32_t instances);

}  // namespace edvr
