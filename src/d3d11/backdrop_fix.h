// The main menu's backdrop, unbanded.
//
// THE MEASURED DEFECT. The menu's background is a still stored BC1 -- the
// oldest block format, two R5G6B5 endpoints per 4x4 block and two bits of
// interpolation weight per texel. The census names it outright: a large BC1
// texture sampled by a four-vertex triangle strip with no depth, blitted into
// 1920x1080 targets which the eye composites then magnify.
//
// Its SIZE is not fixed, which cost a wrong claim in the first draft of this
// file. The census caught a 1920x1080 one; the first field run matched a
// 3840x2160 one minutes later. That is why the signature below carries a size
// FLOOR and not an exact size -- an exact one would have missed the field's
// asset entirely. Measured off a field screenshot (the numbers are
// in docs/menu-backdrop.md): the whole image lives in 0..91 of 255, and 80%
// of red, 80% of green and 60% of blue sit EXACTLY on the R5G6B5 grid
// against a chance level of 12%, 25% and 12%. Two thirds of it is flat runs
// of seven pixels or more.
//
// So the backdrop is not soft, it is STEPPED: a dark gradient carrying about
// eleven usable blue levels, its contours magnified until they read as
// blocks. Nothing in the render path did this. The art arrived this way, and
// a flat monitor never shows it because the same still subtends a fifth of
// the angle there.
//
// WHY THIS SUBSTITUTES A TEXTURE AND NOT A SHADER. The first plan was a
// replacement pixel shader for the blit. Two facts killed it, both from the
// same census. The blit's pixel shader (831DF02EBA8AE814) is SHARED by eight
// draws in the frame, so it cannot be swapped by hash without touching seven
// innocent ones; and replacing it for one draw would mean reproducing
// behaviour whose disassembly nobody has -- a guess, in a project whose whole
// method is to transcribe what the game does rather than assume it.
//
// The census also made the shader unnecessary. The source texture is STATIC:
// never written, never a copy destination, not once in a session. Something
// that never changes can be fixed ONCE. So the backdrop is debanded into an
// EDVR-owned texture at the first matched draw, and from then on that one
// draw samples ours instead of the game's -- holo_fix's substitution exactly,
// applied to slot 0 instead of slot 1. The game's shader runs untouched. It
// simply reads a smoother still.
//
// What that buys, beyond avoiding the guess: the cost is one dispatch per
// session rather than two per frame, and the per-eye symmetry problem does
// not exist. The blit is a single offscreen draw that BOTH eyes then sample,
// so identical content per eye is guaranteed by construction rather than by
// discipline -- which is the one thing the FSS arc proved is worth paying for
// (docs/fss-scanner.md, rounds 33-49).
//
// WHAT IT CANNOT DO. No pass restores what BC1 discarded. This reconstructs
// the ramp between the steps the compressor left; it does not invent detail,
// and the still is still one still magnified across the field of view. The threshold is what separates
// the two: a neighbourhood already flat to within a step or two is averaged,
// and anything with real structure in it -- a star, an edge -- exceeds the
// threshold and passes through untouched. Set the threshold too high and this
// becomes a blur that eats stars, which is why it is live-tunable and why it
// is off until the field has looked at it.
//
// ONE INTERACTION WORTH KNOWING. The blit's TARGETS were 1920x1080 in the
// census, so the backdrop reaches the composite at that size whatever the
// source was, and raising vscreen_res_width/_height does not raise it -- it
// magnifies it further while sharpening everything around it. A high panel
// resolution makes this artifact MORE visible, not less. That is the opposite
// of the intuition, and it is why the fix is here and not in fss_res.
//
// UNFINISHED, and the reason the size surprise matters: if the field's
// 3840x2160 source is being blitted into a 1920x1080 target, the engine is
// throwing away three quarters of an asset it already has, and inflating that
// target (fss_res already rewrites CreateTexture2D descs) would be a real
// resolution win rather than only a smoother one. That is a census away from
// being known and has NOT been established -- the field run only proves the
// SOURCE was 3840x2160.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.menu_backdrop (stock | smooth | splash),
// advanced.menu_backdrop_threshold
// and advanced.menu_backdrop_dither. Install and reload; the tunables are
// live, and changing either rebuilds the still at the next matched draw.
void backdropConfigure(Config& cfg);

// False in stock mode, which keeps the per-draw path free when off.
bool backdropWantsDraws();

// Is this draw the backdrop blit? Matched by shape and by what it reads: a
// four-vertex non-indexed draw, one instance, whose pixel shader slot 0
// resolves to a large BC1 texture. In the census that signature was unique --
// exactly one BC1 of that size existed, and the six sibling draws sharing the
// blit's vertex and pixel shaders all sampled eye-sized targets instead.
//
// Deliberately NOT hash-pinned. A size-and-format signature survives a game
// update that merely recompiles shaders, which the FSS panel's transcribed
// vertex shaders do not.
//
// True means the substitute is built and ready to bind, so the caller should
// wrap the draw in backdropBegin/backdropEnd. A first match builds it; a
// build that fails answers false forever after and says why once.
bool backdropOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t instances);

// The composite: the six-index quad that lifts a blit target into an eye.
// Matched by IDENTITY -- the blit above recorded exactly which resource its
// target is, so this asks "is slot 0 that texture" and nothing else can say
// yes. True means the caller should wrap the draw in backdropBegin/End, which
// hands the composite our FULL-RESOLUTION bake and so bypasses the engine's
// downsample of the still into a smaller intermediate.
bool backdropOnComposite(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                         uint32_t instances);

// Around the real draw: bind the debanded still into PS slot 0, restore the
// game's texture after. End is safe to call when Begin did nothing.
void backdropBegin(ID3D11DeviceContext* ctx);
void backdropEnd(ID3D11DeviceContext* ctx);

// Drop the built textures and the kernel. Safe to call twice.
void backdropShutdown();

}  // namespace edvr
