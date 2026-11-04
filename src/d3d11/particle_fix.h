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

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.particle_billboard (stock | steady). Live on save.
void particleConfigure(Config& cfg);

// Whether the substitution is on -- the draw chain asks before matching.
//
// Inline: asked per draw, and the build has no /GL to fold a cross-TU
// getter for one scalar load. The enum lives here too, so this can name
// its kSteady value.
namespace detail {
enum class ParticleMode { kStock, kSteady };
extern ParticleMode g_particleMode;
// The flag below is particle_fix.cpp's g_hideWitchspaceStars, published so
// the draw path's per-draw calls can be skipped inline when they would
// return at their first line (the .cpp binds its old name to this).
extern bool g_particleHideStars;
// The flags spaceDustHidden() reads: fix.supercruise_dust = off and
// fix.normal_space_dust = off (particle_fix.cpp).
extern bool g_supercruiseDustHidden;
extern bool g_normalSpaceDustHidden;
// The billboard transcriptions' vertex shader hashes, in kVariants order
// (particle_fix.cpp static_asserts the two lists agree).
inline constexpr uint64_t kParticleVariantVs[2] = {0xEB787F983BC1F5A3ull,
                                                   0x6041FD2D3D0164E1ull};
// Every camera-facing billboard vertex shader the fix acts on: the seven with a transcription first (the plume, the
// flare and the five explosion shaders, which face the viewer), then the eleven whose rows [277]/[278] are rolled
// out at vertex slot 1 (billboard_rollfree.h; the list and why each is safe are in
// docs/billboard-root-cause-2026-10-16.md). particle_fix.cpp static_asserts its own tables against this one.
inline constexpr uint64_t kBillboardVs[18] = {
    0xEB787F983BC1F5A3ull, 0x6041FD2D3D0164E1ull, 0x9F4BBCFCD3B68BC9ull, 0xBBAD1CA808E1E292ull, 0x9AEC596A2B036EA6ull,
    0xAFBCD3ADB9092F78ull, 0x4BDE4DBF88C2BA27ull,
    0x1B285CBC9F185D4Dull, 0x78F5F08D02EE38CCull, 0x1FBA99C4911D7ACFull,
    0x2BF557356A28AA0Dull, 0xDB113566B6A59C5Bull, 0x68DDDEF04D9894AFull, 0xF7A6E916F14A3B1Aull,
    0x12382D2EA45E9632ull, 0x94EA991C3A03DBD9ull, 0xC31238331D8AC3D4ull, 0xF1502353BE051A2Dull};
// A 64-bit filter over bits 8..13 of the hash, so the per-draw test is one shift and one compare for nearly every draw.
constexpr uint64_t billboardFilter() {
    uint64_t m = 0;
    for (uint64_t h : kBillboardVs) m |= 1ull << ((h >> 8) & 63);
    return m;
}
inline constexpr uint64_t kBillboardFilter = billboardFilter();
// The roll-free rebind's pending flag (one draw's, set by particleOnDraw, consumed by forwardWithVerdict) and its
// engaged flag, and the watched scene blocks' identities (billboard_rollfree.h Blocks), published for the Map tee.
extern bool g_rollFreePending;
extern bool g_rollFreeEngaged;
extern const void* g_watchedBlocks[4];
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

// Is this draw space dust that a key has withheld? Two pairs, both in
// supercruise_lines.h, each with its own key: the supercruise streaks
// (fix.supercruise_dust) and the normal-space specks (fix.normal_space_dust).
// The same shape as the witchspace pair above: the inline test is two bool
// reads per draw, and spaceDustSkip matches each pair's vertex AND pixel
// shader against its own flag. True means the draw is not forwarded, so it
// neither reaches the scene nor the UI layer.
inline bool spaceDustHidden() {
    return detail::g_supercruiseDustHidden || detail::g_normalSpaceDustHidden;
}
bool spaceDustSkip(ID3D11DeviceContext* ctx, char kind, uint32_t count,
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
//
// Since the roll-free families (2026-10-16) the draw's shape is no longer asked here: the transcribed pair keep their
// instanced-draw rule inside particleOnDraw, the others are told by their hash alone.
inline bool particleOnDrawMayMatch(char kind, uint32_t count, uint32_t instances,
                                   uint64_t heldVsHash) {
    (void)kind; (void)count; (void)instances;
    if (heldVsHash == 0) return true;
    if (!((detail::kBillboardFilter >> ((heldVsHash >> 8) & 63)) & 1ull)) return false;
    for (uint64_t h : detail::kBillboardVs)
        if (heldVsHash == h) return true;
    return false;
}

// The roll-free billboard block. particleOnDraw arms it (and returns false: the draw keeps its place in the ladder);
// forwardWithVerdict calls Begin once the draw is certain to be issued and End on every way out. Begin binds a copy of
// the game's scene block, with rows 277/278 roll-free, at VERTEX slot 1; End puts the game's block back. The pixel
// stage is never touched. Clear is called at the top of every draw's ladder so a flag cannot outlive its draw.
inline void particleRollFreeClear() { detail::g_rollFreePending = false; }
void particleRollFreeBegin(ID3D11DeviceContext* ctx);
void particleRollFreeEnd(ID3D11DeviceContext* ctx);

// Bind the substituted constants for one draw, and put the game's back.
// End is safe to call when Begin did nothing.
void particleBegin(ID3D11DeviceContext* ctx);
void particleEnd(ID3D11DeviceContext* ctx);

// The Map/Unmap tee: is this a scene block the fix watches (inline, four compares), and the write.
inline bool particleBlockWatched(const void* res) {
    return res && (res == detail::g_watchedBlocks[0] || res == detail::g_watchedBlocks[1] ||
                   res == detail::g_watchedBlocks[2] || res == detail::g_watchedBlocks[3]);
}
// Copies the rows the fix reads (270..279, 160 bytes) of the written block, not all of it; returns the bytes
// copied (particle_capture_core.h).
uint32_t particleCapture(const void* res, const void* data, uint32_t bytes);

// Once a frame on the owner thread: lets go of a target no plume or flare has matched for a while, so a
// session without smoke pays nothing for the Map/Unmap tee. The next matched draw learns it again.
void particleFrameBoundary();

// "particle target armed, retired N, re-armed M" for the unmap readers line.
void particleDescribeTarget(char* out, size_t cap);

// Whether anything here wants to see draws at all -- false is free.
bool particleWantsDraws();

// Release what the substitute holds. Safe to call twice.
void particleShutdown();

}  // namespace edvr
