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
// docs/shaders/particle-vs.asm, with the whole story in docs/particle-billboards.md.
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
//
// Inline: asked per draw, and the build has no /GL to fold a cross-TU
// getter for one scalar load. The enum lives here too, so this can name
// its kSteady value.
namespace detail {
enum class ParticleMode { kStock, kSteady };
extern ParticleMode g_particleMode;
// The two flags below are particle_fix.cpp's g_hideWitchspaceStars and
// g_probe, published so the draw path's per-draw calls can be skipped
// inline when they would return at their first line (the .cpp binds its old
// names to these, the depth probe's pattern, so its own code is unchanged).
extern bool g_particleHideStars;
extern bool g_particleProbe;
// The billboard transcriptions' vertex shader hashes, in kVariants order
// (particle_fix.cpp static_asserts the two lists agree).
inline constexpr uint64_t kParticleVariantVs[2] = {0xEB787F983BC1F5A3ull,
                                                   0x6041FD2D3D0164E1ull};
}  // namespace detail
inline bool particleSteady() { return detail::g_particleMode == detail::ParticleMode::kSteady; }

// Is this draw the witchspace starfield, with fix.witchspace_stars = off?
// True means do not forward it. Nothing is substituted: the draw is simply
// not made, which is what "off" should mean and is not how 0.12.3 removed
// this by accident. Costs one bool read per draw when the key is on --
// witchspaceStarsHidden() is that bool, inline, and witchspaceStarsSkip's
// own first test, so a caller that asks it first skips only calls that
// would have returned false.
inline bool witchspaceStarsHidden() { return detail::g_particleHideStars; }
bool witchspaceStarsSkip(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                         uint32_t instances);

// The matched draw, for the verdict chain: this draw is a particle
// billboard AND a substitute is ready to bind.
bool particleOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t instances);

// particleOnDraw's own rejections ahead of its first effect, inline, for a
// caller that has already asked particleSteady(). It is asked for nearly
// every draw in the frame (before the eye gate), and nearly every draw is
// instanced geometry that passes the shape test and fails only at the
// shader: 48 innermost samples of the 1355-frame parked-5 window across
// particleOnDraw, billboardVariantFor and boundVsHashFast, for a call that
// returned false. heldVsHash is the binding shadow's vertex shader hash when
// the shadow holds a pointer, else 0. Zero means "unknown" and answers true,
// because the callee then asks the context itself (boundVsHashFast); a
// non-zero held hash is exactly the hash the callee would compare, so a
// false here is a false there, reached without side effects.
inline bool particleOnDrawMayMatch(char kind, uint32_t count, uint32_t instances,
                                   uint64_t heldVsHash) {
    if (kind != 'X' && kind != 'N') return false;
    if (instances == 0 || count < 6) return false;
    if (heldVsHash == 0) return true;
    return heldVsHash == detail::kParticleVariantVs[0] ||
           heldVsHash == detail::kParticleVariantVs[1];
}

// Bind the substituted constants for one draw, and put the game's back.
// End is safe to call when Begin did nothing.
void particleBegin(ID3D11DeviceContext* ctx);
void particleEnd(ID3D11DeviceContext* ctx);

// The Map/Unmap tee: the buffer whose writes to shadow, and the write.
void* particleTarget();
void particleCapture(const void* data, uint32_t bytes);

// Whether anything here wants to see draws at all -- false is free.
bool particleWantsDraws();

// Called for every eye draw while the probe is armed. Recognises the
// particle billboard shader by its content hash (the one key that cannot
// collide with the terrain and prop pipelines it shares every size-level
// signature with) and samples its constants at most once a second.
// particleProbeOn() is its first test, inline: the draw path asks it before
// the call, which with the probe off (its default) was a call per draw that
// only returned.
inline bool particleProbeOn() { return detail::g_particleProbe; }
void particleOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances);

// Drop the staging buffer. Safe to call twice.
void particleShutdown();

}  // namespace edvr
