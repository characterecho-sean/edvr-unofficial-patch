// visual.star_glow's flat mechanism (flat only; the rules are flat_star_glow_math.h's, the setting's reader is common/star_glow.h).
//
// WHAT. Each star pass the game issues into the scene's HDR target H (the surface, the halo quads, the ring, the arcs, and a neutron star's or
// white dwarf's jet add and flare) is issued once more right after the game's own issue, into a layer S of its own: the same depth view, the
// game's own blend, constants and resources, a depth-stencil state with its writes off. S therefore holds the star's light alone, made by the
// game's shaders at the game's resolution and jitter. At the bloom chain's first level -- the HDR route's trigger draw when it writes a target
// half of H's size -- the draw reads H' = H + HUD + m * S instead of H (ui_layer.cpp uiLayerGlowFeed adds m * S to the HUD feed's texture, or to
// a copy of H when the HUD layer is not live), so the game's own bloom draws a halo k = 1 + m times as strong around stars and the same one
// everywhere else. The tonemap and every other reader keep the original H.
//
// COST AND REACH. At 100 (the default) nothing runs and nothing is created: the draw scope asks one relaxed atomic load. Above 100 a star frame costs
// the star passes again at S's size (a cleared R11G11B10F layer at H's size) and, at the first level, one copy or one more fullscreen pass.
// A frame with no star pass costs the classification of each draw's shader pair.
//
// THREADS. The flat runtime's owner thread only (its draw scope and its frame boundary).
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Device;

namespace edvr {

struct UiGlowStar;   // ui_layer.h: the star input of the first level's pass

// One load. True while visual.star_glow is above 100 in the flat profile.
bool flatStarGlowOn();

// Once a frame from the flat runtime's Present path, before any of its early returns (`nextFrame`: the frame that starts now; `watched`: the frame
// that just ended was watched by the flat runtime's draw scope, State::frameLive -- not g_flatRuntimeLive, which the frame's last hook clears before
// Present). Re-reads the setting when the settings changed (live: it applies on the next frame), starts the frame's state, and prints the 5 s window
// while the setting is above 100 and one line when it returns to 100.
void flatStarGlowFrame(uint64_t nextFrame, bool watched);

// One draw the scope watched, as the module needs it.
struct FlatStarDraw {
    uint64_t vs = 0, ps = 0;
    bool hCandidate = false;     // flatHdrCandidateDraw: an R11G11B10F target at the scene's extent with a depth bound
    bool triggered = false;      // the HDR route's trigger has come this frame
    uint32_t width = 0, height = 0;   // the draw's colour target
};
// The scope's question per watched draw, while flatStarGlowOn(). `*touched` is set when the draw is a star pass of any target (it is then kept out
// of the offscreen fast path, which would serve a repeat of it without asking). True: re-issue it into S right after the game's own issue, and
// `*role` names the pass (FlatStarRole) for flatStarGlowReplayBegin.
bool flatStarGlowObserve(ID3D11DeviceContext* ctx, const FlatStarDraw& draw, bool* touched, uint8_t* role);
// Around that re-issue (FlatRuntimeDrawScope::uiReissue): Begin binds S as the one colour target with the draw's own depth view and a
// depth-stencil state of the game's with its writes off (false: nothing was bound, do not issue; the reason is counted; true: counted as replayed);
// End puts the game's targets and state back, past the binding shadow, which keeps describing the game's.
bool flatStarGlowReplayBegin(ID3D11DeviceContext* ctx, uint8_t role, uint32_t width, uint32_t height);
void flatStarGlowReplayEnd(ID3D11DeviceContext* ctx);

// The bloom chain's first level. flatStarGlowLive: S holds star light from this frame, so the trigger can ask for it. flatStarGlowFeedInput fills the
// input for uiLayerGlowFeed. flatStarGlowNoteFeed: what the pass came to for it (asked, fed, a size mismatch, a failed pass); the first one logs.
// flatStarGlowNoteNoBloom: the frame's trigger is not the bloom chain's first level.
bool flatStarGlowLive();
bool flatStarGlowFeedInput(UiGlowStar* star);
void flatStarGlowNoteFeed(const UiGlowStar& star, bool bound, uint64_t frame, uint32_t slot, uint64_t vs, uint64_t ps, uint32_t hdrW, uint32_t hdrH,
                          uint32_t targetW, uint32_t targetH);
void flatStarGlowNoteNoBloom();

// ResizeBuffers: S and the cloned states released (the next star frame builds them again). A new device: the same, and the device is held for identity.
void flatStarGlowRelease();
void flatStarGlowNoteDevice(ID3D11Device* device);

}  // namespace edvr
