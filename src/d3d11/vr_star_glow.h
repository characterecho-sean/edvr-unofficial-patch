// visual.star_glow in the headset (vr_star_glow_math.h has the rules; docs/star-glow-2026-10-22.md, "2026-10-27: VR star glow, built", has the why).
//
// WHAT. The flat mechanism (flat_star_glow.h), per eye. Each star pass the game issues into an eye's HDR target H (the surface, the halo quads, the ring,
// the arcs; a jet's add and flare) is issued once more, right after the game's own issue, into a layer S of that H: the same depth view, the game's own
// blend, constants and resources, a depth-stencil state with its writes off. The bloom chain's first level -- the draw with pixel shader 143AAE05 reading an
// H at t0 -- then reads H' = H + m * S in place of H for that one draw (uiLayerStarFeed: a copy of H and the star pass, m = k - 1), and the game's own
// bloom draws a halo k times as strong around stars and the same one everywhere else. The exposure reduction, the composite and every other reader keep
// the real H. In the headset the HUD is not in H, so H' has no HUD term.
//
// KEYED BY H. The two eyes have two H textures and their draws interleave in submission order, so every piece of state -- S, the run of passes, the first
// level, the light -- belongs to the H a draw writes (or the H the first level reads), never to "the current eye" or to draw order.
//
// COST. Off (the setting at 100, or Elite's Bloom known off): the draw hooks pay one relaxed load of vrStarGlowWanted(), and nothing is created. On: the hook
// classifies each owner draw by Rtv0's binding generation (a resolve only when the binding changed); a draw into an eye H pays the shader-pair test; a draw
// that is not one pays a pixel-shader hash load and a compare or two (the pre-pass and the jet's source, which are evidence and write elsewhere); the
// first-level test (one pixel-shader compare) runs only while some H holds star light its first level has not read. S and H' are made lazily at H's size
// (R11G11B10_FLOAT, about 16 MB each at 2016x2017), S per H and one H' shared by the eyes (it is read by one draw, right after it is made).
//
// THREADS. The owner context's draw hooks and the frame boundary, all on the render thread.
#pragma once

#include <atomic>
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

namespace detail {
extern std::atomic<bool> g_vrStarGlowWanted;    // armed: VR profile, visual.star_glow above 100, Elite's Bloom not known to be off
extern std::atomic<bool> g_vrStarGlowPending;   // some H holds star light that its first level has not read this frame
}  // namespace detail

// True while the glow runs. One relaxed load: the whole price of the feature while it is off.
inline bool vrStarGlowWanted() { return detail::g_vrStarGlowWanted.load(std::memory_order_relaxed); }

// Once a frame from vScreenFrameBoundary, after menuTick (which refreshes the Bloom reading). Folds the frame that ended into the window, reads the setting
// (live), arms, pauses or stands down, and prints the 10 s lines while armed. Reads the VR profile's setting; in the flat profile it only makes sure nothing runs.
void vrStarGlowFrameBoundary();

// Right before the game's own issue of a draw on the owner context, while vrStarGlowWanted(). When this draw is the first level of an H that holds star light,
// H' is bound at t0 for it (vrStarGlowAfterDraw puts the game's view back). One relaxed load for every other draw.
void vrStarGlowFirstLevel(ID3D11DeviceContext* ctx);
inline void vrStarGlowBeforeDraw(ID3D11DeviceContext* ctx) {
    if (detail::g_vrStarGlowPending.load(std::memory_order_relaxed)) vrStarGlowFirstLevel(ctx);
}

// Right after the game's own issue (and every bracket of it), while vrStarGlowWanted(): the game's view of H goes back if the draw read H', the draw is
// classified, and `plain` says the game's own draw was issued as the game made it (no verdict altered or withheld it). True: this draw is a star pass and
// its replay is bound into S -- the caller issues the draw once more with the game's arguments, then calls vrStarGlowReplayEnd. False: nothing was
// bound, issue nothing (every refusal is counted).
bool vrStarGlowAfterDraw(ID3D11DeviceContext* ctx, bool plain);
void vrStarGlowReplayEnd(ID3D11DeviceContext* ctx);

}  // namespace edvr
