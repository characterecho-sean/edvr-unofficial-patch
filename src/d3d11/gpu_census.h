#pragma once
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// How much of a frame's GPU time is EDVR's own work, feature by feature
// (issue #38): one line every 30 s. Each section wraps a CALL SITE, so any
// timer inside it closes first, and counts every occurrence. Only one
// section is timed per frame, round-robin, and at most K occurrences of it
// (4 at the door, 8 per draw), because the shared disjoint clock fits
// about 31 spans a frame across the whole DLL (gpu_disjoint_clock.h).
// ms/frame = mean timed ms per occurrence x occurrences per frame.
// temporal_pass.cpp's price report times some of the same door work on its
// own event-driven windows; this census reads neither it nor the per-pass
// timers, so every figure comes from this window's live calls.
//
// A call site must be where GPU work is actually issued, not merely where a
// feature's entry point is invoked: most calls into engine velocity or
// screen motion return without submitting anything, and a timestamp pair
// around a no-op mostly measures its own pipeline-drain cost. To correct for
// that, a section's turn also times one empty begin/end pair (nothing
// between) at its first timed call, in a second sampler; ms/frame above
// subtracts that pair's own mean cost from the real one's, floored at zero.
// How many fixes wrap Elite's draws and are named in the census (AlteredFix below: one for each
// verdict that can reach the altered-draw site, and a last one for "unnamed").
constexpr int kAlteredFixCount = 16;

enum class GpuCensusSection : uint8_t {
    // Door: once or twice a frame, at Submit. K = 2 (both eyes) while active.
    DoorTemporalWhole = 0,    // the whole temporalInner call, both eyes (edvrTemporalAa)
    DoorUpscaler,             // fsr/dlaa, and the fovea crops (periphery + centre) -- nested inside DoorTemporalWhole
    DoorMotionPrep,           // the motion-vector dispatch (mvCs) -- nested inside DoorTemporalWhole
    DoorHologramResolve,      // uiDepthHologramResolve + uiDepthTemporalDepth -- nested inside DoorTemporalWhole
    DoorUiResolve,            // applyUiResolve -- nested inside DoorTemporalWhole
    DoorSharpen,              // sharpen_pass.cpp's dispatch
    DoorMenu,                 // menu_panel.cpp's dispatch
    DoorUiLayerComposite,     // ui_layer.cpp's kComposite dispatch
    DoorFssHeal,              // fss_heal.cpp's dispatch
    // In-frame: per game draw. K = 8 while active.
    FrameHologramPasses,      // the two hologram/icon depth reissues
    FrameUiDepthCoverage,     // the UI-depth family reissue (UiContent::prepare, stellar coverage nest inside)
    FramePlanet,              // the planet/solar terrain reissue
    FrameScreenMotion,        // screen_motion.cpp's own GPU work: the UI mask clear+reissue,
                              // the eye's buffer/size copies, clear and projection draw, the
                              // panel-count readback -- not screenMotionUiDraw/screenMotionDraw's
                              // call sites, most of which return without issuing anything
    FrameWeaponMotion,        // the weapon motion-vector reissue
    FrameEngineVelocity,      // engine_velocity.cpp's own GPU work: the eye-frame clear, the
                              // pool+scene snapshot copy and the append refresh copy -- not
                              // engineVelocityBeforeDraw's call sites, which mostly return
                              // without reaching the slow path at all
    FrameUiLayerReissues,     // the UI layer's multiply/write-back reissues
    FrameUiLayerHdrSeed,      // the HDR HUD layer's depth-stencil seed: the copy of the game's depth-stencil and the
                              // Seeder's passes into the layer's own target (ui_layer.cpp seedLayerDepth), counted
                              // once for each seed and only for that layer (GpuCensusSeedScope below)
    // The VR on-foot world route's own GPU work (vr_world_route.cpp, design doc section 82), EDVR's cost like the
    // sections above. They run only while experimental.temporal_aa_on_foot_world is auto and the route works; the
    // rotation gives a turn to none of the three until one has been called (nextTurnOwner, gpu_census.cpp), so with
    // the key off the census samples exactly as it did before they existed.
    FrameWorldResolve,        // the route's resolve at the tone: the input copy, prep, upscaler and the finish into H
    FrameWorldMips,           // the screen texture's copy into the mipped texture and its GenerateMips
    FrameWorldLayer,          // the layer's re-issue of each eye's screen draw with the resolved, mipped screen
    // Elite's OWN draws that EDVR alters (see AlteredDrawClass below): the game's
    // draw timed whole, so each figure holds the game's own work in it plus what
    // EDVR adds by binding its target or swapping its shader. NOT EDVR's cost,
    // and never part of "EDVR ~X". Per draw like the sections above (K = 8, the
    // same stride and rotation); they own the line after the main one.
    AlteredPoolFamily,        // a pool-family draw with EDVR's MRT6 slot target bound and its shaders substituted
    AlteredUiLayer,           // a UI draw redirected into EDVR's UI layer target (fix.ui_quality)
    // A draw wrapped in another fix's state change (RemLok, the loading hologram, scrim,
    // particles, the panel ...): ONE SECTION PER FIX, kAlteredFixCount of them, in AlteredFix's
    // order. They share ONE turn in the rotation (the first owns it, turnOwnerOf), one K and one
    // stride, so the rotation is no longer than it was with a single section for all of them and
    // every fix's draws are sampled in the same frames.
    AlteredFixFirst,
    Count = AlteredFixFirst + kAlteredFixCount
};

// Which of the classes above a draw of Elite's is, decided where forwardWithVerdict
// issues the game's own draw. One class per draw, in this priority: a pool-family
// draw (only when no verdict claimed it, as engineVelocityBeforeDraw is), a UI-layer
// redirect, then any other verdict's wrapper. None for a draw EDVR leaves as the game
// issued it, for a foreign context, and for every reissue. (A "terrain original", the
// null-pixel-shader prepass advanced.terrain_motion captured in its own draw, was a
// class here until that hook retired on 2026-10-01.)
enum class AlteredDrawClass : uint8_t { None = 0, PoolFamily, UiLayer, Verdict };
inline AlteredDrawClass classifyAlteredDraw(bool owner, bool verdictNone, bool poolSubstituted,
                                            bool uiLayered) noexcept {
    if (!owner) return AlteredDrawClass::None;
    if (verdictNone && poolSubstituted) return AlteredDrawClass::PoolFamily;
    if (uiLayered) return AlteredDrawClass::UiLayer;
    if (!verdictNone) return AlteredDrawClass::Verdict;
    return AlteredDrawClass::None;
}

// The fix that wraps a Verdict-class draw, one name each (gpu_census.cpp's kAlteredFixNames), in
// the order of the sections that follow AlteredUiLayer. vscreen.cpp maps its DrawVerdict onto
// these in one switch that must name every verdict (a new one is a compile error there), so a
// wrapped draw is never attributed to the wrong fix by an enum that grew.
enum class AlteredFix : uint8_t {
    Panel = 0,     // the panel-distance override (kPanel)
    Remlok,        // the RemLok overlay, outer mode (kRemlok)
    Holo,          // the loading hologram's pattern (kHolo)
    TargetSharp,   // the target indicator's reconstruction (kTargetSharp)
    NightVision,   // night vision (kNightVision)
    IntroPanel,    // the intro movie's panel (kIntroPanel)
    GlareClamp,    // the sun glare train, its instance count clamped (kGlareClamp)
    GlareSteady,   // the sun glare train, world-locked (kGlareSteady)
    Particle,      // the particle billboards (kParticle)
    FssPanel,      // the FSS panel composite (kFssPanel)
    FssReveal,     // the FSS body composite at one dissolve moment (kFssReveal)
    FssDump,       // the FSS dump pass (kFssDump)
    ResolveBind,   // the deferred lighting resolve with the scanner-body input lend (kResolveBind)
    Scrim,         // the loader dialog's dimming wash (kScrim)
    Backdrop,      // the menu backdrop blit (kBackdrop)
    Unnamed,       // a verdict nobody gave a name (none reaches the altered-draw site today): visible, never silent
    Count
};
static_assert(static_cast<int>(AlteredFix::Count) == kAlteredFixCount, "one census section for each named fix");

// What the scope is told about a draw: its class, and for the Verdict class the fix that wraps it.
struct AlteredDraw {
    AlteredDrawClass cls = AlteredDrawClass::None;
    AlteredFix fix = AlteredFix::Unnamed;
    constexpr AlteredDraw() noexcept = default;
    // Implicit, so a class that names no fix (None, the pool family, the UI layer) is written as itself.
    constexpr AlteredDraw(AlteredDrawClass c) noexcept : cls(c) {}
    constexpr AlteredDraw(AlteredDrawClass c, AlteredFix f) noexcept : cls(c), fix(f) {}
};
inline GpuCensusSection alteredFixSectionOf(AlteredFix f) noexcept {
    return static_cast<GpuCensusSection>(static_cast<int>(GpuCensusSection::AlteredFixFirst) + static_cast<int>(f));
}
inline GpuCensusSection alteredSectionOf(AlteredDraw d) noexcept {
    switch (d.cls) {
    case AlteredDrawClass::PoolFamily: return GpuCensusSection::AlteredPoolFamily;
    case AlteredDrawClass::UiLayer:    return GpuCensusSection::AlteredUiLayer;
    case AlteredDrawClass::Verdict:    return alteredFixSectionOf(d.fix);
    case AlteredDrawClass::None:       break;
    }
    return GpuCensusSection::Count;
}

// The rotation gives each section one turn -- except the fix sections, which share one: the turn
// belongs to the first of them, and a call for any of them is timed on it.
inline GpuCensusSection turnOwnerOf(GpuCensusSection section) noexcept {
    return section >= GpuCensusSection::AlteredFixFirst && section < GpuCensusSection::Count
               ? GpuCensusSection::AlteredFixFirst
               : section;
}

// Begin around a call site's GPU work, End right after it. Begin ALWAYS
// counts the occurrence (cheap: one branch and an increment when this is
// not this frame's rotated section). It returns true only when this IS
// that section and a timer lease was actually opened; End is safe to call
// unconditionally either way (a no-op when nothing is open -- gpu_interval.h),
// so a plain Begin/.../End pair never needs to branch on Begin's result.
//
// Wrap CALL SITES, not the functions they call: any pre-existing timer a
// wrapped call opens and closes on its own nests correctly for free, since
// it completes before the wrapping End() runs.
bool gpuCensusBegin(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept;
void gpuCensusEnd(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept;

// RAII alternative for a call site that is a clean lexical block: construct
// at the top, let it close at scope exit (including an early return) so a
// span can never outlive the work it times.
class GpuCensusScope {
public:
    GpuCensusScope(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept
        : ctx_(ctx), section_(section) { gpuCensusBegin(ctx_, section_); }
    ~GpuCensusScope() { gpuCensusEnd(ctx_, section_); }
    GpuCensusScope(const GpuCensusScope&) = delete;
    GpuCensusScope& operator=(const GpuCensusScope&) = delete;

private:
    ID3D11DeviceContext* ctx_;
    GpuCensusSection section_;
};

// The scope for one of Elite's altered draws: wraps ONLY the game's own real draw
// call (never the reissues or the extra work the thunk does after it, which have
// sections of their own), counts the occurrence, and times it on the section's
// turn. A None class is one compare and nothing else. Defined after
// gpuCensusBegin/End, which it calls.
class GpuCensusAlteredScope {
public:
    GpuCensusAlteredScope(ID3D11DeviceContext* ctx, AlteredDraw d) noexcept
        : ctx_(ctx), section_(alteredSectionOf(d)) {
        if (section_ != GpuCensusSection::Count) open_ = gpuCensusBegin(ctx_, section_);
    }
    ~GpuCensusAlteredScope() {
        if (open_) gpuCensusEnd(ctx_, section_);
    }
    GpuCensusAlteredScope(const GpuCensusAlteredScope&) = delete;
    GpuCensusAlteredScope& operator=(const GpuCensusAlteredScope&) = delete;

private:
    ID3D11DeviceContext* ctx_;
    GpuCensusSection section_;
    bool open_ = false;
};

// The target one HDR HUD seed was written to and the game buffer it was read from: what the 30 s line needs
// to say which size its figure was measured at (fix.ui_quality changes the layer's size, so the same section
// times a different amount of work at 100 than at 125). `format` names the layer target's depth-stencil
// format; the census copies it, so a literal or any string alive for the call will do.
struct GpuCensusSeedTarget {
    uint32_t layerW = 0, layerH = 0;   // the layer's own depth-stencil target (what the seed writes)
    uint32_t bytesPerPixel = 0;        // that target's format: depth plus stencil, so its memory is w x h x this
    uint32_t gameW = 0, gameH = 0;     // the game's depth-stencil (what the seed reads, copied first)
    const char* format = nullptr;
};
void gpuCensusNoteSeedTarget(const GpuCensusSeedTarget& target) noexcept;

// The scope for a UI layer seed: ONE statement at the top of the seed, RAII so both of its exits close the
// span. It times the HDR HUD layer's seed on GpuCensusSection::FrameUiLayerHdrSeed and notes the target it
// wrote; a seed of any other layer (the 8-bit UI layer's) is counted nowhere and noted nowhere, so this
// section can never hold a mix of the two. The stage test is the caller's one argument (`hdrHudLayer`), and
// gpu_census_test holds both halves: the scope's own behaviour, and a scan that the seed still constructs
// it with that argument.
class GpuCensusSeedScope {
public:
    GpuCensusSeedScope(ID3D11DeviceContext* ctx, bool hdrHudLayer, const GpuCensusSeedTarget& target) noexcept
        : ctx_(ctx), section_(hdrHudLayer ? GpuCensusSection::FrameUiLayerHdrSeed : GpuCensusSection::Count) {
        if (hdrHudLayer) gpuCensusNoteSeedTarget(target);
        gpuCensusBegin(ctx_, section_);
    }
    ~GpuCensusSeedScope() { gpuCensusEnd(ctx_, section_); }
    GpuCensusSeedScope(const GpuCensusSeedScope&) = delete;
    GpuCensusSeedScope& operator=(const GpuCensusSeedScope&) = delete;

private:
    ID3D11DeviceContext* ctx_;
    GpuCensusSection section_;
};

// Once a frame, from vScreenFrameBoundary (after the frame's own Begin/End
// calls, alongside the other features' *FrameBoundary calls): polls every
// section's sampler, advances the rotation for the frame about to start,
// and -- every 30 s of wall clock -- logs one summary line and resets the
// window. No ini key: this is always on.
void gpuCensusFrame(ID3D11DeviceContext* ctx) noexcept;

// Quiescent cleanup, alongside the other feature modules' Shutdown().
void gpuCensusShutdown() noexcept;

} // namespace edvr
