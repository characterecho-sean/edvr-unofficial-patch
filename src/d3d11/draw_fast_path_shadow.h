#pragma once
// TEMPORARY, a Phase 1 shadow instrument (built 2026-10-27; docs\engine-render-performance-2026-09-19.md, "Desk design: the repeat-draw fast path").
// No Windows and no D3D here, so tools\aa_off_test drives every decision the draw path takes.
//
// WHAT THIS MEASURES. The design would let an owner DrawIndexedInstanced that repeats the previous draw's key skip most of the draw thunk: the rest of
// beginPanelOverride, all of forwardWithVerdict, the lambda's owner block, the tail's normally-off checks. This build skips nothing. Every draw takes
// today's full path; this module only decides, for each owner X draw, whether it WOULD have taken the fast path, and then compares what the full path
// actually did with what the previous draw's ticket promised. A mismatch names an input the key misses. Phase 2 (the skipping) is built only once a
// flight reads mismatches at zero over millions of would-take draws.
//
// THE KEY. Everything the ordinary outcome is a function of, that is not a dynamic flag: the bindings' aggregate generation (binding_shadow.h
// bindingEyeKeyGeneration: it moves on any VS, PS, PS SRV 0-3, RTV0 or DSV0 set and at every frame boundary, so a first-of-frame event can never be a
// repeat), ui_depth's classifier epoch, and the UI layer's family epoch.
//
// THE TICKET. After every owner X draw: the key as it stands then, the class the draw landed in, and whether the draw was ORDINARY -- every term below at
// its ordinary value -- and in the args class. The next owner X draw WOULD take the fast path when the ticket is valid, the key is equal, its own args
// are in the class, no dynamic flag is up, nothing normally off is armed, and every subscriber the draw gate lists as wanting draws is certified.
//
// THE CONTRACT FOR FUTURE CODE (certified). A draw feature added to vscreen.cpp's drawGateSubscribed list gets a bit in draw_gate_subscribers.h, and
// stays out of kCertified until someone has shown that its behaviour on an ordinary draw is captured by the key, the ticket or the args class. A bit
// that is not certified, while its subscriber is wanting, turns the would-take verdict off by itself. aa_off_test scans the two lists against each other.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace fastshadow {

// ---- The draw gate's subscribers, one bit per term of drawGateSubscribed (vscreen.cpp), in its order -----------------------------------------------
constexpr uint32_t kSubDistance = 1u << 0;        // s->distanceEnabled
constexpr uint32_t kSubCensusSkip = 1u << 1;      // the census skip specs, the range and the off-screen form
constexpr uint32_t kSubQuadSkip = 1u << 2;        // s->quadSkipArmed
constexpr uint32_t kSubCensusAuto = 1u << 3;      // s->censusAutoW
constexpr uint32_t kSubFssRes = 1u << 4;          // fssResActive()
constexpr uint32_t kSubFssPanel = 1u << 5;        // fssPanelWantsDraws()
constexpr uint32_t kSubRemlok = 1u << 6;          // remlokWantsDraws()
constexpr uint32_t kSubHolo = 1u << 7;            // holoWantsDraws()
constexpr uint32_t kSubTargetSharp = 1u << 8;     // targetSharpWantsDraws()
constexpr uint32_t kSubUiDepth = 1u << 9;         // uiDepthWantsDraws()
constexpr uint32_t kSubSunglare = 1u << 10;       // sunglareWantsDraws()
constexpr uint32_t kSubCensus = 1u << 11;         // drawCensusArmed()
constexpr uint32_t kSubBlendProbe = 1u << 12;     // blendProbeWantsDraws()
constexpr uint32_t kSubObjectProbe = 1u << 13;    // objectProbeWantsDraws()
constexpr uint32_t kSubPanelCurve = 1u << 14;     // panelCurveWants()
constexpr uint32_t kSubParticles = 1u << 15;      // particleWantsDraws()
constexpr uint32_t kSubBackdrop = 1u << 16;       // backdropWantsDraws()
constexpr uint32_t kSubScrim = 1u << 17;          // scrimWantsDraws()
constexpr uint32_t kSubQuadProbe = 1u << 18;      // quadProbeWants()
constexpr uint32_t kSubLoaderPanel = 1u << 19;    // loaderPanelWants()
constexpr uint32_t kSubIntroPanel = 1u << 20;     // introPanelWants()
constexpr uint32_t kSubWakePulse = 1u << 21;      // wakePulseWantsDraws()
constexpr uint32_t kSubNightVision = 1u << 22;    // nightVisionWantsDraws()
constexpr uint32_t kSubSpaceDust = 1u << 23;      // spaceDustHidden()
constexpr uint32_t kSubWitchspaceStars = 1u << 24;  // witchspaceStarsHidden()
constexpr uint32_t kSubDepthProbe = 1u << 25;     // depthProbeWanted()
constexpr uint32_t kSubFootprint = 1u << 26;      // vscreenFootprintWanted()
constexpr uint32_t kSubIntroCurve = 1u << 27;     // introCurveWants()
constexpr uint32_t kSubscriberCount = 28;
constexpr uint32_t kSubscriberAll = (1u << kSubscriberCount) - 1u;

// The certified set: a wanting subscriber here acts on an ordinary draw in no way the key, the ticket or the args class does not already hold.
//   - Shape-gated subscribers whose shapes the args class keeps out (a count of 64 or less, exactly 6, 240, or above 10000, or a DrawInstanced or
//     DrawIndexed): the RemLok overlay (N, 3), the hologram pattern (X, 6), the target indicator (X, 6), the menu backdrop's blit and composite (N 4,
//     X 6), the intro movie and the splash (X 6; N 4), night vision (X, 240), and the sun-glare train (N, 6). The sun-glare camera nomination
//     (above 10000) is the one place the glare reaches an X draw, and the class stops short of it.
//   - Subscribers whose decision on an ordinary draw is a function of the key: the interface depth and the hologram depth passes (the classifier
//     epoch and the bindings; an ordinary draw's pendings are terms below), the scrim wash (slot 0's view is in the key), particles (the billboard
//     vertex shaders are specials; the roll-free rebind is a dynamic flag), the wake pulse (a pulse surface keeps its target off the ticket).
//   - The object probe (its pool checks are a dynamic flag: objectProbeChecksLeft; its ledger is armed below) and the depth probe (its per-draw tallies
//     and its layout sample are depthProbeNoteDraw's, which the fast path keeps calling; the layout census's target is set every frame, so it is
//     not a gate -- flown 2026-10-27, Phase 1).
//   - The loader dialog's panel fix: it offers a draw only on a loader-shaped frame (the last frame drew fewer than kSceneEyeDraws eye draws), a
//     fact the key cannot hold across frames but that is constant inside one; the armed test below carries it.
//   - The footprint instrument (fix.vscreen_res_width = auto, which is the shipped default in VR, so it is wanting in an ordinary session): it acts
//     only at the 2D screen's composite, the vertex shader and pixel shader pair that vscreen.cpp's footprintEyeDraw compares first, and that pair's
//     vertex shader is one of the specials below. The design lists it among the normally-off probes; with the key at auto it is on.
//   - The panel distance (s->distanceEnabled) and the curved screen (panelCurveWants). Both are settings a player sets and leaves on, so they are wanting
//     in an ordinary session, and the args class excludes everything they act on. The distance fix returns kNone at `if (!srv0IsPanelSized(...))`
//     (vscreen.cpp, beginPanelOverride), and srv0IsPanelSized returns false at `count > 64` before any side effect; the curve's recognition
//     (`panelCurveWants() && srv0IsPanelSized(...)`) is behind the same test, its forward-scope effects (the substitution and the world route's reissue of
//     it) are behind curveThisDraw, and its surface strip (the intro movie's and the splash's composite) is behind a verdict or introCurveThisDraw and a
//     count of exactly 6. curveThisDraw and introCurveThisDraw are outcome terms. aa_off_test pins each of those gates in the source.
// Not certified, and why: the census and every skip and probe, the FSS keys, space dust and the witchspace stars hidden (the design lists them as
// normally off, and they are). Each of those is also tested directly in the armed list.
constexpr uint32_t kCertified = kSubDistance | kSubRemlok | kSubHolo | kSubTargetSharp | kSubUiDepth | kSubSunglare | kSubObjectProbe | kSubPanelCurve |
                                kSubParticles | kSubBackdrop | kSubScrim | kSubLoaderPanel | kSubIntroPanel | kSubWakePulse | kSubNightVision |
                                kSubDepthProbe | kSubFootprint | kSubIntroCurve;
static_assert((kCertified & ~kSubscriberAll) == 0, "the certified set names only subscribers the gate lists");

// True when every wanting subscriber is certified (an empty set is: with nothing listening, nothing acts).
inline bool certifiedOk(uint32_t wanting) { return (wanting & ~kCertified) == 0; }

// ---- The dynamic flags: plain loads, tested on every would-take draw; any one up sends the draw to the full path --------------------------------------
enum Dyn : uint32_t {
    kDynCameraWatch = 0,   // temporal_pass g_cameraDrawWatch (temporalPassWantsDraw)
    kDynIssueBlocked,      // ui_layer g_uiLayerIssueBlocked
    kDynCrispPending,      // ui_layer g_uiLayerCrispPending
    kDynRollFree,          // particle_fix g_rollFreePending
    kDynWorldReissue,      // ui_layer g_uiLayerWorldReissue
    kDynWorldWants,        // vr_world_route g_vrWorldWants
    kDynProbeChecks,       // object_probe g_checksLeft != 0
    kDynCount
};
inline const char* dynName(unsigned i) {
    static const char* const kNames[kDynCount] = {"camera watch", "issue blocked", "crisp pending", "roll-free", "world reissue", "world route", "probe checks"};
    return i < kDynCount ? kNames[i] : "?";
}

// ---- The normally-off flags (the design's "G" list): any one armed sends the draw to the full path ---------------------------------------------------
enum Armed : uint32_t {
    kArmCensus = 0,   // drawCensusArmed
    kArmSkip,         // the census skip specs, range and off-screen form
    kArmQuad,         // quadSkipArmed, quadProbeWants
    kArmAuto,         // censusAutoW
    kArmPixel,        // pixelProbeWantsDraws
    kArmBlend,        // blendProbeWantsDraws
    kArmLedger,       // objectProbeLedgerActive
    kArmMarker,       // nativeOnFootMarkerProbeLive (and with it the marker fix)
    kArmGpuProfile,   // gpuPassProfileCapturing
    kArmFss,          // fssResActive, fssPanelWantsDraws
    kArmDust,         // spaceDustHidden, witchspaceStarsHidden
    kArmBodyCull,     // nativeOnFootBodyCullWanted
    kArmSeedDiag,     // uiLayerSeedDiagnostics
    kArmLoaderFrame,  // loaderPanelWants on a loader-shaped frame
    kArmCount
};
inline const char* armedName(unsigned i) {
    static const char* const kNames[kArmCount] = {"census", "skip", "quad", "auto", "pixel", "blend", "ledger", "marker",
                                                  "gpu profile", "fss", "dust", "body cull", "seed diag", "loader frame"};
    return i < kArmCount ? kNames[i] : "?";
}

// ---- The args class: the draws whose count the inventories found no test for ---------------------------------------------------------------------------
// Kept out: the quads (6), night vision (240), the sun-glare nomination (above 10000, which also reads VsCb0), the panel-size test and the after-UI
// watch budget (64 or less); every other matcher that reads instances requires exactly 1, so a larger value only declines more.
constexpr uint32_t kArgsCountOver = 64, kArgsCountMax = 10000, kArgsNightVisionCount = 240;
inline bool argsClass(char kind, uint32_t count, uint32_t instances) {
    return kind == 'X' && count > kArgsCountOver && count <= kArgsCountMax && count != kArgsNightVisionCount && instances >= 1;
}

// Which bound of the class an X draw is outside (Phase 1c, built 2026-10-27), the first that applies in this order: count <= 64, count > 10000,
// count == 240, no instances. Phase 1b flew with the args class failing on about 47% of owner X draws and the line unable to say which bound.
enum class Bound : uint8_t { kIn = 0, kSmall, kLarge, k240, kNoInstances, kCount };
inline Bound boundOf(uint32_t count, uint32_t instances) {
    if (count <= kArgsCountOver) return Bound::kSmall;
    if (count > kArgsCountMax) return Bound::kLarge;
    if (count == kArgsNightVisionCount) return Bound::k240;
    if (instances == 0) return Bound::kNoInstances;
    return Bound::kIn;
}
inline const char* boundName(Bound b) {
    static const char* const kNames[static_cast<size_t>(Bound::kCount)] = {"in", "count<=64", "count>10000", "count 240", "no instances"};
    return kNames[static_cast<size_t>(b)];
}
// A bound, or "in the class". A bool converts to it (true: in the class; false: outside, read as the first bound) for the callers that only know
// whether; the draw path passes the real bound.
struct ArgsBound {
    Bound b = Bound::kIn;
    ArgsBound() = default;
    ArgsBound(bool in) : b(in ? Bound::kIn : Bound::kSmall) {}
    explicit ArgsBound(Bound x) : b(x) {}
    bool in() const { return b == Bound::kIn; }
};
inline ArgsBound argsBound(uint32_t count, uint32_t instances) { return ArgsBound(boundOf(count, instances)); }

// ---- The key and the ticket -----------------------------------------------------------------------------------------------------------------------
struct Key {
    uint32_t bind = 0;         // bindingEyeKeyGeneration()
    uint32_t classifier = 0;   // uiDepthClassifierEpoch()
    uint32_t family = 0;       // uiLayerFamilyEpoch()
};
inline bool sameKey(const Key& a, const Key& b) { return a.bind == b.bind && a.classifier == b.classifier && a.family == b.family; }

// Where a draw landed: an eye texture or an off-screen target (rtv0Eye), or unknown when beginPanelOverride returned at the draw gate before it
// read the target and the memo is not current for this binding.
enum class Class : uint8_t { kEye = 0, kOff = 1, kUnresolved = 2, kCount = 3 };
inline const char* className(Class c) { return c == Class::kEye ? "eye" : c == Class::kOff ? "off-screen" : "unclassed"; }

// ---- The outcome terms: bit set = NOT at its ordinary value ---------------------------------------------------------------------------------------
// Each is something the full path did, or a fact about the draw, that a draw with the same key should not differ in. Where each is captured is in
// vscreen.cpp's shadow block and at the points it names.
enum Term : uint32_t {
    kTermVerdict = 0,       // beginPanelOverride returned a verdict other than kNone
    kTermUiDepth,           // t_uiDepthThisDraw
    kTermComposite,         // t_compositeThisDraw
    kTermHoloDepth,         // t_holoDepthThisDraw
    kTermCurve,             // curveThisDraw
    kTermIntroCurve,        // introCurveThisDraw
    kTermGlareClamp,        // glareClamp != 0
    kTermPlanet,            // ui_depth's planet pending
    kTermSolar,             // ui_depth's solar pending
    kTermWorldScreen,       // ui_depth's world-screen pending
    kTermCrisp,             // g_uiLayerCrispPending
    kTermRollFree,          // g_rollFreePending
    kTermFamily,            // forwardWithVerdict: the UI family was not none
    kTermLayered,           // the layer was engaged for the game's draw
    kTermAfterUi,           // uiLayerNoteOther changed a counter, the watch budget or the seed
    kTermCompositeCounted,  // the composite census ran
    kTermCelestialOrbit,    // the celestial-patch or orbit-line match fired
    kTermReissue,           // a reissue block ran
    kTermWeapon,            // weaponMotionWants held and its motion pass ran
    kTermScreenMotion,      // screenMotionUiDraw or screenMotionDraw recognised the draw
    kTermRigid,             // the rigid-draw query was true
    kTermCensus,            // the camera census or the footprint ran
    kTermSpecialVs,         // the VS is one of the Vs-keyed specials (the design's seven, and the 2D screen's composite)
    kTermUnsettled,         // off-screen only: uiDepthNoteOffscreenDraw's target is not settled
    kTermWake,              // off-screen only: the target is a wake-pulse surface
    kTermCount,
    kTermClass = kTermCount,   // not an outcome: the draw's class differs from the ticket's (a would-take draw only)
    kMismatchTermCount
};
static_assert(kMismatchTermCount <= 32, "the terms fit one word");
constexpr uint32_t termBit(Term t) { return 1u << static_cast<uint32_t>(t); }
constexpr uint32_t kOutcomeMask = (1u << kTermCount) - 1u;
inline const char* termName(unsigned i) {
    static const char* const kNames[kMismatchTermCount] = {
        "verdict", "ui depth", "composite", "holo depth", "curve", "intro curve", "glare clamp", "planet", "solar", "world screen", "crisp",
        "roll-free", "family", "layered", "after-ui", "composite count", "celestial/orbit", "reissue", "weapon", "screen motion", "rigid",
        "census", "special vs", "unsettled", "wake", "class"};
    return i < kMismatchTermCount ? kNames[i] : "?";
}

// The two terms only an off-screen draw has. An eye draw's are always at their ordinary value.
inline uint32_t offscreenTerms(Class cls, bool settled, bool wake) {
    if (cls != Class::kOff) return 0;
    return (settled ? 0u : termBit(kTermUnsettled)) | (wake ? termBit(kTermWake) : 0u);
}

// The census term (kTermCensus), by what the two probes DO and not by whether they are switched on (Phase 1c): the world route's camera census when it
// ran, or the footprint instrument when it is on and the pair bound is the 2D screen's composite, the only draw it acts on. Phase 1b read the footprint's
// flag alone, which is on by default (fix.vscreen_res_width = auto), so the term was true on every eye geometry draw.
inline bool censusTerm(bool worldCensusRan, bool footprintOn, bool compositePairBound) { return worldCensusRan || (footprintOn && compositePairBound); }

// Ordinary: every term at its ordinary value.
inline bool ordinary(uint32_t terms) { return (terms & kOutcomeMask) == 0; }

// The ticket's validity after a draw: the draw was ordinary, its args are in the class, and its class is known (an off-screen draw that is unsettled
// or a wake surface is not ordinary, by the two terms above).
inline bool ticketValid(uint32_t terms, bool argsOk, Class cls) { return argsOk && cls != Class::kUnresolved && ordinary(terms); }

struct Ticket {
    Key key;
    Class cls = Class::kUnresolved;
    bool valid = false;
};

// What a would-take draw's actual outcome says against the ticket: every term not ordinary, and a class that changed.
inline uint32_t mismatchTerms(uint32_t terms, Class cls, Class ticketCls) {
    return (terms & kOutcomeMask) | (cls != ticketCls ? termBit(kTermClass) : 0u);
}

// ---- Eligibility ------------------------------------------------------------------------------------------------------------------------------------
// The refusal reason of an owner X draw, the first that applies in this order, or kTake.
// The args class is four reasons, one per bound, the first that applies (Phase 1c).
enum class Reason : uint8_t { kTake = 0, kNoTicket, kKeyMoved, kArgsSmall, kArgsLarge, kArgs240, kArgsNoInstances, kDynamic, kArmed, kNotCertified, kCount };
inline const char* reasonName(Reason r) {
    static const char* const kNames[static_cast<size_t>(Reason::kCount)] = {"take", "no ticket", "key moved", "args count<=64", "args count>10000", "args count 240",
                                                                           "args no instances", "dynamic", "armed", "not certified"};
    return kNames[static_cast<size_t>(r)];
}
// The refusal reason for a draw outside the class (the bound must not be kIn).
inline Reason argsReason(Bound b) {
    return b == Bound::kLarge ? Reason::kArgsLarge : b == Bound::k240 ? Reason::kArgs240 : b == Bound::kNoInstances ? Reason::kArgsNoInstances : Reason::kArgsSmall;
}

struct Verdict {
    Reason reason = Reason::kNoTicket;
    uint32_t dynamic = 0;   // the dynamic flags gathered, when the draw got that far
    uint32_t armed = 0;     // the armed flags gathered, when the draw got that far
};

inline unsigned lowestBit(uint32_t m) {
    unsigned i = 0;
    while (i < 32 && !((m >> i) & 1u)) ++i;
    return i;
}

// ---- Why a draw recorded no ticket (Phase 1b, built 2026-10-27) ----------------------------------------------------------------------------------------
// Phase 1 flew at 0% would-take with about 70% of owner X draws recording no ticket and no way to say which term stopped them: the comparison runs only
// on would-take draws. This counts, for every owner X draw that does NOT record a ticket, the FIRST reason in a fixed order, by class, and keeps the
// vertex shaders that reason was counted for. The order: the class unknown; the four bounds of the args class (Phase 1c: count <= 64, count > 10000,
// count == 240, no instances; the first that applies); an off-screen target unsettled; a wake-pulse surface; then the other 23 outcome terms in their
// own order (verdict first, the special vertex shaders last). A draw with several non-ordinary terms is counted under the first, so a later term is
// hidden behind an earlier one until that one is dealt with.
constexpr unsigned kNtClassUnknown = 0, kNtBoundSmall = 1, kNtBoundLarge = 2, kNtBound240 = 3, kNtBoundNoInstances = 4, kNtUnsettled = 5, kNtWake = 6,
                   kNtFirstTerm = 7;
constexpr unsigned kNoTicketReasons = kNtFirstTerm + (static_cast<unsigned>(kTermCount) - 2);
static_assert(static_cast<unsigned>(kTermUnsettled) == static_cast<unsigned>(kTermCount) - 2 && static_cast<unsigned>(kTermWake) == static_cast<unsigned>(kTermCount) - 1,
              "the two off-screen terms are the last two, so reason 7 + i is outcome term i for the other 23");
static_assert(kNoTicketReasons <= 32, "the reasons fit the sketch table");
inline const char* noTicketName(unsigned r) {
    return r == kNtClassUnknown ? "class unknown" : r >= kNtBoundSmall && r <= kNtBoundNoInstances ? boundName(static_cast<Bound>(r - kNtBoundSmall + 1))
           : r == kNtUnsettled ? "unsettled" : r == kNtWake ? "wake surface" : r < kNoTicketReasons ? termName(r - kNtFirstTerm) : "?";
}
// The first failing reason of a draw, or -1 when it records a ticket (the same test as ticketValid, with the reason kept).
inline int noTicketReason(uint32_t terms, ArgsBound bound, Class cls) {
    if (cls == Class::kUnresolved) return static_cast<int>(kNtClassUnknown);
    if (!bound.in()) return static_cast<int>(kNtBoundSmall + static_cast<unsigned>(bound.b) - 1);
    if (terms & termBit(kTermUnsettled)) return static_cast<int>(kNtUnsettled);
    if (terms & termBit(kTermWake)) return static_cast<int>(kNtWake);
    const uint32_t rest = terms & kOutcomeMask & ~(termBit(kTermUnsettled) | termBit(kTermWake));
    if (rest) return static_cast<int>(kNtFirstTerm + lowestBit(rest));
    return -1;
}

// A space-saving sketch of the most frequent values of a stream (Metwally et al.): eight slots, each a value and a count; a value in a slot is counted;
// a value in none takes a free slot, or evicts the slot with the least count and inherits that count plus one. A value seen more than N/8 times in a
// stream of N is always in a slot, and a slot's count is its value's true count or an over-estimate by at most the count it inherited. A count of 0
// marks a free slot, so the hash 0 (a vertex shader the shadow does not know) is a value like any other.
struct VsSketch {
    static constexpr unsigned kSlots = 8;
    uint64_t key[kSlots] = {};
    uint64_t count[kSlots] = {};
    void add(uint64_t value) {
        for (unsigned i = 0; i < kSlots; ++i)
            if (count[i] && key[i] == value) { ++count[i]; return; }
        unsigned least = kSlots;
        for (unsigned i = 0; i < kSlots; ++i) {
            if (!count[i]) { key[i] = value; count[i] = 1; return; }
            if (least == kSlots || count[i] < count[least]) least = i;
        }
        key[least] = value;
        ++count[least];
    }
    // The `n` largest slots, largest count first (ties by slot), as slot indices; returns how many were filled.
    unsigned top(unsigned n, unsigned* out) const {
        bool used[kSlots] = {};
        unsigned got = 0;
        while (got < n) {
            unsigned best = kSlots;
            for (unsigned i = 0; i < kSlots; ++i)
                if (!used[i] && count[i] && (best == kSlots || count[i] > count[best])) best = i;
            if (best == kSlots) break;
            used[best] = true;
            out[got++] = best;
        }
        return got;
    }
};

// ---- What the sun-glare nomination does on a draw over 10000 (Phase 1c) -------------------------------------------------------------------------------
// beginPanelOverride's eye branch nominates the scene camera from a big draw's VsCb0: `if (sunglareWorldActive() && count > 10000) { cb = VsCb0; if (cb &&
// cb != sceneCbNominated) { resolve ... } }`. It is a no-op when the glare's world shader is inactive, when there is no VsCb0, or when VsCb0 is the one
// already nominated, and it only runs for eye draws. The thunk reads those three facts before the draw (the nomination moves sceneCbNominated), and the
// tally counts them by class so a widened args class can be sized.
constexpr uint32_t kFactOver = 1, kFactWorldInactive = 2, kFactCbNull = 4, kFactCbNominated = 8;
enum Over : unsigned { kOverDraws = 0, kOverWorldInactive, kOverCbNull, kOverCbNominated, kOverActs, kOverCount };

// What the draw path hands the tally beside the verdict and the terms: the bound vertex shader (bindingShaderHash(Vs); 0 is a value like any other) and the
// over-10000 facts above. Converts from the hash alone.
struct DrawFacts {
    uint64_t vs = 0;
    uint32_t over = 0;
    DrawFacts() = default;
    DrawFacts(uint64_t hash) : vs(hash) {}
    DrawFacts(uint64_t hash, uint32_t facts) : vs(hash), over(facts) {}
};

// ---- The window's counters -------------------------------------------------------------------------------------------------------------------------
struct Counters {
    uint64_t draws[3] = {};                                     // owner X draws by class
    uint64_t take[3] = {};                                      // ...of them, would-take
    uint64_t refused[static_cast<size_t>(Reason::kCount)] = {};  // by first reason (index kTake unused)
    uint64_t dynBy[kDynCount] = {};                             // dynamic refusals by the first flag up
    uint64_t armedBy[kArmCount] = {};                           // armed refusals by the first flag armed
    uint64_t tickets = 0;                                       // valid tickets recorded
    uint64_t compared = 0;                                      // would-take draws compared
    uint64_t mismatchDraws = 0;                                 // ...of them with at least one mismatch
    uint64_t mismatch[kMismatchTermCount] = {};                 // by term
    uint64_t noTicket[3][kNoTicketReasons] = {};                // draws that recorded no ticket: by class, by first failing reason (Phase 1b)
    VsSketch noTicketVs[kNoTicketReasons];                      // ...and the vertex shaders each reason was counted for, all classes together
    VsSketch boundVs[3][4];                                     // the four args bounds' vertex shaders by class (Phase 1c; bound b is index b - 1)
    uint64_t over[3][kOverCount] = {};                          // draws over 10000 by class, and the nomination's no-op cases among them (Phase 1c)
    void reset() { *this = Counters(); }
};

// How many mismatches are named in the log (the first five of the session).
constexpr uint32_t kNamedMismatches = 5;

// The shadow's whole state and its two entry points; vscreen.cpp is glue around these.
struct Tracker {
    Ticket ticket;
    Counters count;
    uint32_t named = 0;         // mismatches named so far this session
    uint32_t windowStart = 0;   // the frame the window's counters began at

    // The would-take decision for an owner X draw about to be issued. The dynamic and armed gathers are called only when the draw reaches that
    // stage, so a draw refused earlier pays for neither. `wanting` is the draw gate's wanting set (certified or not).
    template <class DynFn, class ArmFn>
    Verdict judge(const Key& now, ArgsBound args, uint32_t wanting, DynFn&& dynFn, ArmFn&& armFn) const {
        Verdict v;
        if (!ticket.valid) { v.reason = Reason::kNoTicket; return v; }
        if (!sameKey(ticket.key, now)) { v.reason = Reason::kKeyMoved; return v; }
        if (!args.in()) { v.reason = argsReason(args.b); return v; }
        v.dynamic = dynFn();
        if (v.dynamic) { v.reason = Reason::kDynamic; return v; }
        v.armed = armFn();
        if (v.armed) { v.reason = Reason::kArmed; return v; }
        if (!certifiedOk(wanting)) { v.reason = Reason::kNotCertified; return v; }
        v.reason = Reason::kTake;
        return v;
    }

    // One owner X draw, finished: the class it landed in, its non-ordinary terms, the bound of the args class it is outside (or in), the key as it stands
    // after the draw, and the facts beside it (the bound vertex shader, the over-10000 facts). Counts it, compares it when it was a would-take draw
    // (returning the mismatch terms, 0 for none), writes the next ticket, and when no ticket results counts the first reason why.
    uint32_t finish(const Verdict& v, Class cls, uint32_t terms, ArgsBound bound, const Key& after, const DrawFacts& facts = DrawFacts()) {
        const size_t c = static_cast<size_t>(cls);
        ++count.draws[c];
        if ((facts.over & kFactOver) && c < 2) {
            uint64_t* o = count.over[c];
            const bool inactive = (facts.over & kFactWorldInactive) != 0, noCb = (facts.over & kFactCbNull) != 0, nominated = (facts.over & kFactCbNominated) != 0;
            ++o[kOverDraws];
            o[kOverWorldInactive] += inactive;
            o[kOverCbNull] += noCb;
            o[kOverCbNominated] += nominated;
            o[kOverActs] += (cls == Class::kEye && !inactive && !noCb && !nominated);   // only an eye draw reaches the nomination, and only these proceed to its resolve
        }
        uint32_t mm = 0;
        if (v.reason == Reason::kTake) {
            ++count.take[c];
            ++count.compared;
            mm = mismatchTerms(terms, cls, ticket.cls);
            if (mm) {
                ++count.mismatchDraws;
                for (unsigned i = 0; i < kMismatchTermCount; ++i)
                    if ((mm >> i) & 1u) ++count.mismatch[i];
            }
        } else {
            ++count.refused[static_cast<size_t>(v.reason)];
            if (v.reason == Reason::kDynamic) ++count.dynBy[lowestBit(v.dynamic)];
            else if (v.reason == Reason::kArmed) ++count.armedBy[lowestBit(v.armed)];
        }
        ticket.key = after;
        ticket.cls = cls;
        ticket.valid = ticketValid(terms, bound.in(), cls);
        if (ticket.valid) {
            ++count.tickets;
        } else {
            const int why = noTicketReason(terms, bound, cls);
            if (why >= 0) {
                ++count.noTicket[c][why];
                count.noTicketVs[why].add(facts.vs);
                if (why >= static_cast<int>(kNtBoundSmall) && why <= static_cast<int>(kNtBoundNoInstances) && c < 2)
                    count.boundVs[c][why - static_cast<int>(kNtBoundSmall)].add(facts.vs);
            }
        }
        return mm;
    }

    // True for the first kNamedMismatches mismatched draws of the session, once each.
    bool takeNameSlot() {
        if (named >= kNamedMismatches) return false;
        ++named;
        return true;
    }
};

// ---- Text ---------------------------------------------------------------------------------------------------------------------------------------------
struct Out {
    char* buf;
    size_t cap;
    size_t len = 0;
    Out(char* b, size_t c) : buf(b), cap(c) { if (buf && cap) buf[0] = '\0'; }
    void put(const char* fmt, ...) {
        if (!buf || len + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n <= 0) return;
        const size_t room = cap - len - 1;
        len += static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room;
    }
};

// "name n, name n" for the largest non-zero entries, at most `limit` of them, then "+k more" for the rest; "none" when all are zero.
template <class NameFn>
inline void putTop(Out& o, NameFn name, const uint64_t* v, unsigned n, unsigned limit) {
    bool used[32] = {};
    unsigned shown = 0, nonzero = 0;
    for (unsigned i = 0; i < n; ++i) nonzero += v[i] != 0;
    if (!nonzero) { o.put("none"); return; }
    while (shown < limit && shown < nonzero) {
        unsigned best = n;
        for (unsigned i = 0; i < n; ++i)
            if (!used[i] && v[i] != 0 && (best == n || v[i] > v[best])) best = i;
        if (best == n) break;
        used[best] = true;
        o.put("%s%s %llu", shown ? ", " : "", name(best), static_cast<unsigned long long>(v[best]));
        ++shown;
    }
    if (nonzero > shown) o.put(", +%u more", nonzero - shown);
}

// A term mask as text, "-" for none.
inline size_t formatTerms(char* out, size_t cap, uint32_t mask) {
    Out o(out, cap);
    bool first = true;
    for (unsigned i = 0; i < kMismatchTermCount; ++i) {
        if (!((mask >> i) & 1u)) continue;
        o.put("%s%s", first ? "" : ", ", termName(i));
        first = false;
    }
    if (first) o.put("-");
    return o.len;
}

inline void putShare(Out& o, uint64_t take, uint64_t draws, double frames) {
    if (!draws) { o.put("-"); return; }
    o.put("%.1f%% (%.1f a frame)", 100.0 * static_cast<double>(take) / static_cast<double>(draws), static_cast<double>(take) / frames);
}

// The window's line, written at the 1800-frame edge. Under 1150 characters at its worst (the rig pins it): the lists are the largest entries only,
// each with its exact count, and say how many were left out.
inline size_t formatLine(char* out, size_t cap, uint32_t endFrame, uint32_t frames, const Counters& c) {
    Out o(out, cap);
    const double f = frames ? static_cast<double>(frames) : 1.0;
    o.put("draw fast path (shadow): TEMPORARY (built 2026-10-27, Phase 1, nothing skipped), 1800-frame window ending %u, %u frames. "
          "Owner X draws a frame %.1f (eye %.1f, off-screen %.1f, unclassed %.1f). Would take: eye ",
          static_cast<unsigned>(endFrame), static_cast<unsigned>(frames),
          static_cast<double>(c.draws[0] + c.draws[1] + c.draws[2]) / f, static_cast<double>(c.draws[0]) / f, static_cast<double>(c.draws[1]) / f,
          static_cast<double>(c.draws[2]) / f);
    putShare(o, c.take[0], c.draws[0], f);
    o.put(", off-screen ");
    putShare(o, c.take[1], c.draws[1], f);
    o.put(". Refused by first reason (window totals):");
    bool any = false;
    for (size_t r = 1; r < static_cast<size_t>(Reason::kCount); ++r) {
        if (!c.refused[r]) continue;
        o.put("%s %s %llu", any ? "," : "", reasonName(static_cast<Reason>(r)), static_cast<unsigned long long>(c.refused[r]));
        if (static_cast<Reason>(r) == Reason::kDynamic) {
            o.put(" (");
            putTop(o, dynName, c.dynBy, kDynCount, 4);
            o.put(")");
        } else if (static_cast<Reason>(r) == Reason::kArmed) {
            o.put(" (");
            putTop(o, armedName, c.armedBy, kArmCount, 5);
            o.put(")");
        }
        any = true;
    }
    if (!any) o.put(" none");
    o.put(". Tickets recorded %llu. Mismatches: ", static_cast<unsigned long long>(c.tickets));
    if (!c.compared) {
        o.put("-");   // nothing to compare
    } else {
        o.put("%llu draws of %llu compared (", static_cast<unsigned long long>(c.mismatchDraws), static_cast<unsigned long long>(c.compared));
        putTop(o, termName, c.mismatch, kMismatchTermCount, 10);
        o.put(")");
    }
    o.put(".");
    return o.len;
}

// The indices of the `n` largest non-zero entries of v[0..len), largest first (ties by index); returns how many were found. len is at most 32.
inline unsigned topIndices(const uint64_t* v, unsigned len, unsigned n, unsigned* out) {
    bool used[32] = {};
    unsigned got = 0;
    while (got < n) {
        unsigned best = len;
        for (unsigned i = 0; i < len; ++i)
            if (!used[i] && v[i] && (best == len || v[i] > v[best])) best = i;
        if (best == len) break;
        used[best] = true;
        out[got++] = best;
    }
    return got;
}

// How many reasons the line lists for each class; the rest are counted as "+k more".
constexpr unsigned kNoTicketRowReasons = 6;

// The second line of the window (Phase 1b): per class, the reasons owner X draws recorded no ticket, a frame, largest first; then, for the two reasons
// with the most draws overall, their three most frequent vertex shaders with each one's share of that reason (the sketch's count, an upper bound,
// capped at 100%). '-' when no draw went without a ticket. Under 1150 characters at its worst (the rig pins it).
inline size_t formatNoTicketLine(char* out, size_t cap, uint32_t endFrame, uint32_t frames, const Counters& c) {
    Out o(out, cap);
    const double f = frames ? static_cast<double>(frames) : 1.0;
    uint64_t classTotal[3] = {}, reasonTotal[kNoTicketReasons] = {}, all = 0;
    for (unsigned cl = 0; cl < 3; ++cl)
        for (unsigned r = 0; r < kNoTicketReasons; ++r) {
            classTotal[cl] += c.noTicket[cl][r];
            reasonTotal[r] += c.noTicket[cl][r];
            all += c.noTicket[cl][r];
        }
    o.put("draw fast path (shadow) no ticket: TEMPORARY (built 2026-10-27, Phase 1c), 1800-frame window ending %u, %u frames. Owner X draws a frame that "
          "recorded no ticket, by first failing reason (class unknown, the four args bounds, unsettled, wake surface, then verdict to special vs): ",
          static_cast<unsigned>(endFrame), static_cast<unsigned>(frames));
    if (!all) {
        o.put("-.");
        return o.len;
    }
    for (unsigned cl = 0; cl < 3; ++cl) {
        if (!classTotal[cl]) continue;
        unsigned idx[kNoTicketRowReasons];
        const unsigned shown = topIndices(c.noTicket[cl], kNoTicketReasons, kNoTicketRowReasons, idx);
        unsigned nonzero = 0;
        for (unsigned r = 0; r < kNoTicketReasons; ++r) nonzero += c.noTicket[cl][r] != 0;
        o.put("%s %.1f (", className(static_cast<Class>(cl)), static_cast<double>(classTotal[cl]) / f);
        for (unsigned j = 0; j < shown; ++j) o.put("%s%s %.1f", j ? ", " : "", noTicketName(idx[j]), static_cast<double>(c.noTicket[cl][idx[j]]) / f);
        if (nonzero > shown) o.put(", +%u more", nonzero - shown);
        o.put("); ");
    }
    o.put("Most frequent vertex shaders of the two largest reasons: ");
    unsigned two[2];
    const unsigned reasons = topIndices(reasonTotal, kNoTicketReasons, 2, two);
    for (unsigned i = 0; i < reasons; ++i) {
        const unsigned r = two[i];
        const VsSketch& sk = c.noTicketVs[r];
        unsigned slot[3];
        const unsigned got = sk.top(3, slot);
        o.put("%s%s (%.1f a frame):", i ? "; " : "", noTicketName(r), static_cast<double>(reasonTotal[r]) / f);
        for (unsigned j = 0; j < got; ++j) {
            double share = 100.0 * static_cast<double>(sk.count[slot[j]]) / static_cast<double>(reasonTotal[r]);
            if (share > 100.0) share = 100.0;
            o.put("%s VS %016llX %.0f%%", j ? "," : "", static_cast<unsigned long long>(sk.key[slot[j]]), share);
        }
    }
    o.put(".");
    return o.len;
}

// The third line of the window (Phase 1c): the draws outside the args class, a frame, by class and by the first bound that applies (count <= 64, count >
// 10000, count == 240, no instances), each with its three most frequent vertex shaders and each one's share of that bound's draws in that class; then,
// for the draws over 10000, a frame by class, the shares (glare off / no cb / cb nominated / acts) where the sun-glare nomination is a no-op or would
// proceed to its resolve. '-' when no classed draw was outside the class. Under 1150 characters at its worst (the rig pins it).
inline size_t formatArgsLine(char* out, size_t cap, uint32_t endFrame, uint32_t frames, const Counters& c) {
    Out o(out, cap);
    const double f = frames ? static_cast<double>(frames) : 1.0;
    uint64_t any = 0, overAny = 0;
    for (unsigned cl = 0; cl < 2; ++cl) {
        for (unsigned b = 0; b < 4; ++b) any += c.noTicket[cl][kNtBoundSmall + b];
        overAny += c.over[cl][kOverDraws];
    }
    o.put("draw fast path (shadow) args: TEMPORARY (Phase 1c), 1800-frame window ending %u, %u frames. Owner X draws a frame outside the "
          "args class, by first bound, with top vertex shaders and shares:",
          static_cast<unsigned>(endFrame), static_cast<unsigned>(frames));
    if (!any) o.put(" -.");
    for (unsigned cl = 0; cl < 2; ++cl)
        for (unsigned b = 0; b < 4; ++b) {
            const uint64_t n = c.noTicket[cl][kNtBoundSmall + b];
            if (!n) continue;
            const VsSketch& sk = c.boundVs[cl][b];
            unsigned slot[3];
            const unsigned got = sk.top(3, slot);
            o.put(" %s %s %.1f:", className(static_cast<Class>(cl)), boundName(static_cast<Bound>(b + 1)), static_cast<double>(n) / f);
            for (unsigned j = 0; j < got; ++j) {
                double share = 100.0 * static_cast<double>(sk.count[slot[j]]) / static_cast<double>(n);
                if (share > 100.0) share = 100.0;
                o.put("%s %016llX %.0f%%", j ? "," : "", static_cast<unsigned long long>(sk.key[slot[j]]), share);
            }
            o.put(";");
        }
    o.put(" Over 10000 a frame, %% glare off/no cb/cb nominated/acts:");
    if (!overAny) o.put(" -.");
    for (unsigned cl = 0; cl < 2; ++cl) {
        const uint64_t n = c.over[cl][kOverDraws];
        if (!n) continue;
        const double d = static_cast<double>(n);
        o.put(" %s %.1f %.0f/%.0f/%.0f/%.0f%%;", className(static_cast<Class>(cl)), d / f, 100.0 * static_cast<double>(c.over[cl][kOverWorldInactive]) / d,
              100.0 * static_cast<double>(c.over[cl][kOverCbNull]) / d, 100.0 * static_cast<double>(c.over[cl][kOverCbNominated]) / d,
              100.0 * static_cast<double>(c.over[cl][kOverActs]) / d);
    }
    return o.len;
}

}  // namespace fastshadow
}  // namespace edvr
