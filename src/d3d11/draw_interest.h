#pragma once
// The interest mask (VR draw path CPU, build 6, item 1; docs\vr-draw-path-cpu-2026-11-01.md, "Build 6"). Pure: no module, no D3D.
//
// An unclaimed draw reads about thirty module-scope flags on its way down vscreen.cpp's ladder, one cache line or two per module, and the game's own draw between two
// hooks evicts them. The mask keeps one bit for each flag (or each flag's stored part) in DrawHot, State's first cache line, so the ladder tests a bit in place of a load.
//
// WHAT A BIT IS. A bit equals, at every read, the expression its site replaced: the flag, or the stored first test of an accessor that does more (the rest stays behind
// the bit, unchanged and in order). It is write-through: the module that owns the flag recomputes the bits of its flags from its real expressions after every write to
// one of them, before its next D3D call or return, and that includes a write through an alias or a reference parameter. The module's own `...InterestBits()` is the one
// copy of the expressions; its `...RecomputeInterest()` stores them. vscreen.cpp reads the bits where each site stands, never from a local held across a call: the
// UI layer's fence rises in the middle of a draw and is read again later in it.
//
// THE POINTER. vscreen.cpp points g_drawInterestPtr at State::interest after `new State` and clears it before State is deleted (both deletes). Null means a write does
// nothing: before install, in transport-only mode, and in every rig that compiles a module without vscreen.cpp. A rig that tests a module's recompute points it at a local.
//
// THREADS. Every writer that is mirrored runs on the render thread (Present, the tk ticks, the config refresh, menu actions, mid-draw), and the ladder reads on it, so the
// mask is a plain field. Flags with a writer on another thread are not mirrored: drawGateWanted, fssResActive(), nativeOnFootBodyCullWanted(); nor are computed wants
// (introPanelWants, introCurveWants, sunglareWantsDraws).
#include <cstdint>

namespace edvr {

enum DrawInterestBit : unsigned {
    kInterestSunOff = 0,           // optimizedStationSunOff(): g_sunOff
    kInterestDesktopWindowBack,    // desktopWindowBackIdentity() != nullptr
    kInterestNativeFootBody,       // nativeOnFootSpawnLive() || nativeOnFootCoatPackLive(); the per-draw countsMatch test stays behind it
    kInterestFlatTemporal,         // g_flatTemporalCapturing; flatTemporalCapturingSlow() stays behind it
    kInterestParticleSteady,       // particleSteady()
    kInterestParticleHideStars,    // witchspaceStarsHidden()
    kInterestParticleDust,         // spaceDustHidden()
    kInterestQuadProbe,            // quadProbeWants()
    kInterestDepthProbe,           // depthProbeWanted(); the per-draw dsv tail of depthProbeEyeDrawNeedsNote stays behind it
    kInterestNativeB3Counting,     // nativeOnFootB3Counting()
    kInterestUiDepth,              // uiDepthWantsDraws(): g_uiDepthOn && !g_uiDepthStoodDown
    kInterestHoloDepth,            // uiDepthHologramWantsDraws(): g_holoDepthOn && uiDepthWantsDraws()
    kInterestSunglareWorld,        // sunglareWorldActive()
    kInterestDrawCensus,           // drawCensusArmed()
    kInterestUiLayerCrispOn,       // uiLayerCrispOn()
    kInterestObjectProbe,          // objectProbeWantsDraws(): g_objectProbeOn || g_objectProbeLedgerOn
    kInterestRemlok,               // remlokWantsDraws()
    kInterestScrim,                // scrimWantsDraws()
    kInterestFssPanel,             // fssPanelWantsDraws()
    kInterestPanelCurve,           // panelCurveWants()
    kInterestUiLayerIssueBlocked,  // uiLayerIssueBlocked(): the fence a failed hologram restore raises
    kInterestBlendProbe,           // blendProbeWantsDraws() (TEMPORARY, the blend probe)
    kInterestObjectProbeLedger,    // objectProbeLedgerActive()
    kInterestUiSeedDiagnostics,    // uiLayerSeedDiagnostics()
    kInterestUiLayerLive,          // uiLayerLive()
    kInterestUiQualityLive,        // uiLayerQualityLive()
    kInterestUiLayerWatching,      // uiLayerWatching()
    kInterestUiLayerCrispPending,  // uiLayerCrispPending()
    kInterestCelestialLive,        // celestialMotionLive()
    kInterestLabelPending,         // nativeOnFootLabelPending()
    kInterestBitCount
};
static_assert(kInterestBitCount <= 64, "the interest mask is one uint64_t");

constexpr uint64_t interestMask(DrawInterestBit b) { return uint64_t(1) << static_cast<unsigned>(b); }

// Each module's bits, for its recompute and for the drift check's expected mask.
constexpr uint64_t kInterestOptimizedStation = interestMask(kInterestSunOff);
constexpr uint64_t kInterestDesktopWindow = interestMask(kInterestDesktopWindowBack);
constexpr uint64_t kInterestNativeOnFoot = interestMask(kInterestNativeFootBody) | interestMask(kInterestNativeB3Counting) | interestMask(kInterestLabelPending);
constexpr uint64_t kInterestFlatTemporalModule = interestMask(kInterestFlatTemporal);
constexpr uint64_t kInterestParticle = interestMask(kInterestParticleSteady) | interestMask(kInterestParticleHideStars) | interestMask(kInterestParticleDust);
constexpr uint64_t kInterestQuadProbeModule = interestMask(kInterestQuadProbe);
constexpr uint64_t kInterestDepthProbeModule = interestMask(kInterestDepthProbe);
constexpr uint64_t kInterestUiDepthModule = interestMask(kInterestUiDepth) | interestMask(kInterestHoloDepth);
constexpr uint64_t kInterestSunglare = interestMask(kInterestSunglareWorld);
constexpr uint64_t kInterestDrawCensusModule = interestMask(kInterestDrawCensus);
constexpr uint64_t kInterestObjectProbeModule = interestMask(kInterestObjectProbe) | interestMask(kInterestObjectProbeLedger);
constexpr uint64_t kInterestRemlokModule = interestMask(kInterestRemlok);
constexpr uint64_t kInterestScrimModule = interestMask(kInterestScrim);
constexpr uint64_t kInterestFssPanelModule = interestMask(kInterestFssPanel);
constexpr uint64_t kInterestPanelCurveModule = interestMask(kInterestPanelCurve);
constexpr uint64_t kInterestBlendProbeModule = interestMask(kInterestBlendProbe);
constexpr uint64_t kInterestCelestial = interestMask(kInterestCelestialLive);
constexpr uint64_t kInterestUiLayer = interestMask(kInterestUiLayerCrispOn) | interestMask(kInterestUiLayerIssueBlocked) | interestMask(kInterestUiSeedDiagnostics) |
                                      interestMask(kInterestUiLayerLive) | interestMask(kInterestUiQualityLive) | interestMask(kInterestUiLayerWatching) |
                                      interestMask(kInterestUiLayerCrispPending);
constexpr uint64_t kInterestAllBits = (uint64_t(1) << static_cast<unsigned>(kInterestBitCount)) - 1;
static_assert((kInterestOptimizedStation | kInterestDesktopWindow | kInterestNativeOnFoot | kInterestFlatTemporalModule | kInterestParticle | kInterestQuadProbeModule |
               kInterestDepthProbeModule | kInterestUiDepthModule | kInterestSunglare | kInterestDrawCensusModule | kInterestObjectProbeModule | kInterestRemlokModule |
               kInterestScrimModule | kInterestFssPanelModule | kInterestPanelCurveModule | kInterestBlendProbeModule | kInterestCelestial | kInterestUiLayer) == kInterestAllBits,
              "every bit belongs to a module");

// State::interest's address while hooks are installed; null otherwise (see the header comment).
inline uint64_t* g_drawInterestPtr = nullptr;

// Set or clear one bit. A no-op while the pointer is null.
inline void drawInterestSet(DrawInterestBit b, bool on) {
    uint64_t* const p = g_drawInterestPtr;
    if (!p) return;
    if (on) *p |= interestMask(b);
    else *p &= ~interestMask(b);
}

// Replace a module's bits with the ones its expressions give now. A no-op while the pointer is null.
inline void drawInterestStore(uint64_t moduleMask, uint64_t bits) {
    uint64_t* const p = g_drawInterestPtr;
    if (!p) return;
    *p = (*p & ~moduleMask) | (bits & moduleMask);
}

// One bit of a computed mask: `cond` as the bit when set.
constexpr uint64_t interestIf(bool cond, DrawInterestBit b) { return cond ? interestMask(b) : 0; }

// The bit's name, for the drift line.
inline const char* interestBitName(unsigned b) {
    static const char* const kNames[kInterestBitCount] = {
        "SunOff", "DesktopWindowBack", "NativeFootBody", "FlatTemporal", "ParticleSteady", "ParticleHideStars", "ParticleDust", "QuadProbe", "DepthProbe",
        "NativeB3Counting", "UiDepth", "HoloDepth", "SunglareWorld", "DrawCensus", "UiLayerCrispOn", "ObjectProbe", "Remlok", "Scrim", "FssPanel", "PanelCurve",
        "UiLayerIssueBlocked", "BlendProbe", "ObjectProbeLedger", "UiSeedDiagnostics", "UiLayerLive", "UiQualityLive", "UiLayerWatching", "UiLayerCrispPending",
        "CelestialLive", "LabelPending"};
    return b < kInterestBitCount ? kNames[b] : "?";
}

inline unsigned interestPopcount(uint64_t m) {
    unsigned n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}

// The lowest bit set in `diff`, for the drift line (kInterestBitCount when none).
inline unsigned interestLowestBit(uint64_t diff) {
    for (unsigned i = 0; i < 64; ++i)
        if (diff & (uint64_t(1) << i)) return i;
    return 64;
}

}  // namespace edvr
