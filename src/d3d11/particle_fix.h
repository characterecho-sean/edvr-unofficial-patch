// The particle billboards' orientation, and the probe that measures it.
//
// Elite draws smoke, steam and similar particles as quads whose basis is
// built from two vectors in the vertex shader's second constant buffer:
//
//     right = normalize(cross(cb1[278], cb1[279]))
//     up    = normalize(cross(cb1[279], right))
//
// cb1[279] is the camera's view direction (the same vector feeds the
// near-fade's depth term); cb1[278] is its up. A camera basis in a headset
// is a HEAD basis, so head roll rolls every particle quad -- the geyser
// plumes rotating with the headset. Read from the game's own bytecode:
// docs/particle-vs.asm, with the whole story in docs/particle-billboards.md.
//
// The probe logs those registers at a matched draw so the inference above
// can be confirmed by measurement before anything is substituted -- which
// vector tracks head ROLL is the one a fix must replace, and guessing it
// costs a field session.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.particle_billboard (stock | steady) and
// advanced.particle_probe. Both live on save.
void particleConfigure(Config& cfg);

// Whether the substitution is on -- the draw chain asks before matching.
bool particleSteady();

// The matched draw, for the verdict chain: this draw is a particle
// billboard AND a substitute is ready to bind.
bool particleOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t instances);

// Bind the substituted constants for one draw, and put the game's back.
// End is safe to call when Begin did nothing.
void particleBegin(ID3D11DeviceContext* ctx);
void particleEnd(ID3D11DeviceContext* ctx);

// The Map/Unmap tee: the buffer whose writes to shadow, and the write.
void* particleTarget();
void particleCapture(const void* data, uint32_t bytes);

// The emitter's own constants, tee'd the same way. cb0[9..11] is its
// model matrix and the translation column is where the plume sits
// relative to the camera, which is what points the billboards AT the
// viewer instead of along the view axis.
void* particleTargetCb0();
void particleCaptureCb0(const void* data, uint32_t bytes);

// Whether anything here wants to see draws at all -- false is free.
bool particleWantsDraws();

// Called for every eye draw while the probe is armed. Recognises the
// particle billboard shader by its content hash (the one key that cannot
// collide with the terrain and prop pipelines it shares every size-level
// signature with) and samples its constants at most once a second.
void particleOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances);

// Drop the staging buffer. Safe to call twice.
void particleShutdown();

}  // namespace edvr
