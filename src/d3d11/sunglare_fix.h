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
bool sunglareWorldActive();
SunglareAction sunglareOnEyeDraw(char kind, uint32_t count,
                                 uint32_t instances);
uint32_t sunglareKeep();

// The steady wrap around a matched train draw: rotate the shared corner
// stream by the head's roll (measured, never written, from the camera
// rows), bind the rotated copy for this one draw, restore after.
void sunglareBegin(ID3D11DeviceContext* ctx);
void sunglareEnd(ID3D11DeviceContext* ctx);

// This draw's DrawInstanced window, set by the thunk before the begin:
// the 2026-08-22 sweep pass showed the instance buffer multiplexes
// SEVERAL glare trains at different instance offsets, so telemetry must
// name each draw's own (start, count) window rather than the buffer
// head.
void sunglareDrawArgs(uint32_t instances, uint32_t startInstance);

// The true scene-camera feed: the glare system runs on the game's
// head-look camera, which clamps at 45 degrees from ship-forward, so
// past the clamp its constants no longer know where the star is. Every
// scene draw's own 208-byte constants DO know -- the same
// engine-standard layout, fully head-tracked. vscreen nominates the
// VsCb0 of big eye-target draws (sunglareSceneCb), its Map tee follows
// that pointer (sunglareSceneCbTarget) and hands each write to
// sunglareSceneRows, which shape-validates and keeps the rows the world
// shader reads through the b2 slot.
void  sunglareSceneCb(void* cb);
void* sunglareSceneCbTarget();
void  sunglareSceneRows(const void* data, uint32_t bytes);
void  sunglareSceneDump(const void* data, uint32_t bytes);

// The train's identity test, exported for the constant-buffer peek: the
// steer needs to read the 208-byte CB of exactly these draws, and two
// matchers for one family is how the witchstar era learned wrong things.
bool sunglareIsGlareTrain(char kind, uint32_t count, uint32_t instances);

// When the train last drew, in nowMs() time; 0 = never this session.
// The exposure damper scopes itself to this: adaptation is held only
// while a sun's glare is actually around -- which is the situation the
// damper was built for -- and everywhere else (menus, stations, on
// foot) the game runs stock without needing gates at all.
uint64_t sunglareLastSeenMs();

}  // namespace edvr
