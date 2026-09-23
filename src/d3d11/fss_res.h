// Render targets created larger than the game asked for: the Full System
// Scanner's body layer, and any surface named by size.
//
// Two matchers share one mechanism here -- the tracking, the viewport
// scaling and the eye-test exclusion are the same three moves whichever
// rule fired. The FSS rule is described first because it is the one that
// was measured into existence; the size-named rule is a developer
// instrument added 2026-09-02 and is described under THE SECOND MATCHER.
// (If that instrument ever graduates to a shipped fix, this module wants
// renaming: it is no longer only about the scanner.)
//
// WHY THIS EXISTS (docs/fss-scanner.md, 2026-08-25)
//
// The FSS renders the zoomed body -- planet and rings -- once per frame,
// MONO, into one offscreen target of exactly HALF the eye size per axis,
// then pastes that image into each eye through per-eye transforms. Measured
// end to end across three instrumented flights: the halving is why the body
// reads as a pixelated sprite, and the per-eye RESAMPLE of a half-res image
// is the last mechanism standing for the ring split -- a thin, low-contrast,
// edge-on ring aliases differently under two different sample grids, so one
// eye shows segments the other misses while the ring streams in. Every other
// channel was measured symmetric: the content (round two), the constants
// (round three), and the delivery (the snapshot A/B, null).
//
// WHAT IT DOES
//
// Three small moves, all identity-tracked, all off unless
// experimental.fss_res = 1:
//
//   1. CreateTexture2D (device_hook's hook, this module's match): a
//      render-target or depth texture asked for at exactly eye/2 x eye/2 is
//      created at DOUBLE the requested size instead. The FSS body target
//      and its depth buffer are the only textures that shape ever drawn --
//      measured by the auto-census's own trigger, which needed 15,654
//      frames of flight, menus and supercruise to see a single draw land
//      in one.
//   2. RSSetViewports (vscreen's hook, this module's answer): a viewport of
//      exactly the ORIGINAL half size, set while an inflated texture is the
//      bound target, is scaled x2 -- so the game's draws fill what was
//      actually allocated. A draw-time backstop catches a viewport set
//      before the target was bound.
//   3. targetIsEyeSized (vscreen): the inflated textures are now exactly
//      eye-sized, and are excluded from the eye test BY IDENTITY -- the
//      same collision the on-foot panel documents, solved the same way.
//
// The composite needs no help: it samples the body through normalized UVs,
// so a fuller texture simply arrives sharper. Round two measured the body
// target taking only draws -- no copies, no compute, no resolves land in it
// -- which is what makes inflation this small.
//
// REFUSAL: any doubt (initial data, mips, MSAA, arrays, no eye size
// published yet, a failed create) falls through to the stock size, and the
// game renders exactly as without EDVR.
//
// THE SECOND MATCHER: advanced.surface_inflate (2026-09-02)
//
// Elite builds the cockpit's holographic panels in offscreen INTERFACE
// SURFACES -- vector geometry (vs 666EF0C4C616F67E, whose pixel shader has
// no sample instruction at all) and text from a 2048x2048 glyph atlas,
// rasterised into a texture that the cockpit's own meshes then sample onto
// the panel surfaces. Read out of a cockpit census: an interface surface is
// an odd, non-power-of-two render target with a depth partner of the same
// size, and the cockpit's mesh draws carry it in a sampler slot.
//
// Those surfaces are a fixed fraction of the game's INTERNAL render
// resolution, not of the submitted eye texture. Measured across two
// sessions on one rig: a panel came out 908x1361 with the scene rendering
// at 4340x4284, and 1363x2042 with the scene at 6510x6426 -- the same
// fraction to four significant figures, three surfaces agreeing. So the
// game already has this knob, and it is its own supersampling setting:
// turning it up re-rasterises the panels sharper, and charges 2.25x the
// pixels for the WHOLE SCENE to do it.
//
// This matcher separates the two. A size named in advanced.surface_inflate
// is created N times larger and its viewport scaled to match, so the game
// rasterises that one panel bigger while the scene is left alone. The
// content is vector and a large glyph atlas, so what comes back is real
// detail and not a resample -- which is why this is here rather than in the
// FSR path. docs/intro-video.md is the opposite case and says so: a fixed
// 1920x1080 decode, nothing to re-render, resampling the only option.
//
// The strongest evidence that this is safe to do is that the game already
// does it to itself: the same GUI renderer produces correct panels at both
// 1.0x and 1.5x, so it is not carrying a hardcoded surface size. What EDVR
// changes is which textures get the larger one.
//
// A DEVELOPER INSTRUMENT, and named by size on purpose: which surface is
// which is not knowable from outside a session. Take a cockpit census with
// advanced.census_offscreen = 1, read the odd sizes that have a depth
// partner, and name one. Off by default, and free when off.
#pragma once

#include <cstdint>

struct D3D11_TEXTURE2D_DESC;

namespace edvr {

class Config;

// Which matcher inflated a tracked texture, so the viewport/scissor
// backstops and the log can attribute a rescale to the right key. kNone is
// never stored -- it is what fssResSourceOf returns for an untracked
// resource -- so it must sort first for any caller that treats 0 as
// "nothing".
enum class InflateSource : uint8_t { kNone = 0, kFss, kNamed, kMatch };

// Reads experimental.fss_res and advanced.surface_inflate. Called at install
// and on the ini reload path, so both are live -- but only for textures
// created AFTERWARDS. For the FSS that is the next zoom; an interface
// surface is built when its panel is, so a named size takes effect on the
// next trip through the main menu, not mid-flight.
void fssResConfigure(Config& cfg);

// One bool for the CreateTexture2D hot gate.
bool fssResWantsCreates();

// fix.hud_quality alone, independent of experimental.fss_res and
// advanced.surface_inflate: is a target quality named at all. Asked by the
// per-draw tick (fssResHudQualityTick) and the draw-time backstops, which
// must run even in a session where nothing has matched yet.
bool fssResWantsMatch();

// The match: if *d is the half-eye body-layer shape, a size named by
// advanced.surface_inflate, or (fix.hud_quality) a shape whose RATIO to the
// game's internal render resolution matches one already confirmed across
// two sessions at different resolutions, multiply its Width and Height in
// place and return true; the caller creates with the modified desc and
// reports the texture back through fssResNoteCreated. The ratio match does
// NOT depend on ui_depth.cpp's classifier -- that learns a surface's size
// from draws INTO it, which happen after this call, too late to inform it
// -- the classifier is consulted only afterward, as a cross-check, to
// LABEL a match's family for the log. *scaleOut is the EXACT float factor
// applied -- read it back from here rather than dividing the two descs'
// widths, which rounds to the wrong answer for a fractional factor
// (1297/908 truncates to 1 under integer division). *sourceOut says which
// matcher fired, and *familyOut ('V'/'T'/'I', or 0 for "other, unlabelled")
// is set only when *sourceOut is kMatch, for the resize summary's
// per-family line. All three out-params may be null when the caller does
// not need them. false leaves *d and every out-param untouched.
bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData,
                        float* scaleOut, InflateSource* sourceOut,
                        char* familyOut);

// Track a texture created inflated: the size the game ASKED for (the size
// its viewports will arrive in), the size it actually got, the float factor
// between them, which matcher decided it, and (kMatch only) which family.
// The caller has all of this in hand already and passes it rather than this
// module stashing it between the two calls: CreateTexture2D runs on the
// game's streaming threads, and a pending value stashed between calls would
// be a race that mis-attributes one create's result to another's.
void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH,
                       uint32_t newW, uint32_t newH, float scale,
                       InflateSource source, char family);

// Identity test for the eye-classification exclusion and the viewport
// hooks. Compares pointers only; a stale entry for a texture the game
// released costs a compare and nothing else.
bool fssResIsInflated(void* resource);

// The original (pre-inflation) size recorded for an inflated texture. False
// when the resource is not one of ours.
bool fssResOrigSize(void* resource, uint32_t* w, uint32_t* h);

// The factor that texture grew by, and 1 for anything untracked. The
// viewport and scissor paths multiply by this rather than by a constant 2:
// the FSS rule always doubles, but a named surface -- or fix.hud_quality --
// may ask for a different, and not necessarily whole, amount.
float fssResScaleOf(void* resource);

// Which matcher inflated this texture, kNone for anything untracked. Used
// to attribute a viewport/scissor rescale to fix.hud_quality's own counters
// without a second, parallel tracking table.
InflateSource fssResSourceOf(void* resource);

// Anything tracked at all? The viewport paths gate on this so a session
// that never opens the FSS never pays a resolve.
//
// Inline: asked on the viewport paths, and the build has no /GL to fold a
// cross-TU getter for one scalar load.
namespace detail {
extern uint32_t g_fssResCount;
}  // namespace detail
inline bool fssResActive() { return detail::g_fssResCount != 0; }

// The viewport paths' receipts: scaled at RSSetViewports, or caught late by
// the draw-time backstop. Capped log lines; the counts land in the note.
// Takes the resource that was scaled so a fix.hud_quality-sourced rescale
// can be counted separately for that key's own summary line.
void fssResNoteViewportScaled(void* resource, bool late);

// The scissor half of the same draw-time backstop. There is no hook on the
// game's own RSSetScissorRects -- nothing needed one before now -- so this
// is the only place a scissor rect set at the pre-inflation size can be
// caught, and it is caught the same way the draw-time viewport backstop
// is: read the state that is about to be used, and fix it if it still
// looks like it was set for the original size. Counted the same way.
void fssResNoteScissorScaled(void* resource);

// A copy or resolve the game issued with a tracked (inflated) texture as
// either side. This does NOT rescale the copy -- no such copy has ever been
// observed landing in one of these surfaces (the FSS body layer's own
// census found none either), and guessing at box math for a shape nobody
// has seen would be a fix built on an untested hypothesis. It counts and
// logs instead, capped, so the first flight says whether this ever
// actually happens and, if so, exactly what shape it is.
void fssResNoteCopyMaybeMismatched(void* dst, void* src);

// Cheap per-draw (or any sufficiently frequent, already-guarded) tick for
// fix.hud_quality's own log lines: due at most once for "on but nothing
// matched in the first minute", and every 30s thereafter for the resize
// summary once something has. A no-op call while the key is off is two
// integer compares.
void fssResHudQualityTick();

}  // namespace edvr
