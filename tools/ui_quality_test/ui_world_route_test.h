// Included after the rig's helpers (check) and ui_after_ui_test.h (squeeze, functionBody).
//
// THE LAYER'S HALF OF THE VR ON-FOOT WORLD ROUTE (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// On a frame the route owns, the 2D screen's composite is not TAKEN by the layer: the game's own draw lands in its
// eye image, and the layer draws it a second time into the eye's layer from the route's mipped copy of the resolved
// screen; the door then runs layer-only. The decision is ui_layer_math.h's uiLayerDecide with one new fact,
// worldRoute, and the rest of the route's layer half is pure too: the mode, the tallies, the refusal reasons and
// their once-each log, the layer's liveness, the door's preflight. What this proves, with the very functions the
// DLL runs:
//
//   1. THE KEY-OFF CONTRACT, exhaustively: with worldRoute false the decision equals the decision as shipped before
//      the route (a frozen copy, decideV0) over EVERY combination of the facts -- the families, the eye, the blends,
//      the depth-stencil effects and the thirteen booleans. The route changes nothing until it owns a frame.
//   2. THE ROUTE'S DECISION: with worldRoute true, the 2D screen's composite, while the screen shows the world,
//      reaches every later test and is decided exactly as any opaque, no-depth eye draw is (kRedirect when they pass,
//      kNotArmed when not armed, kLate when late, ...); every other draw, and a screen that is not the world, is
//      decided as it was.
//   3. THE MODE and THE TALLIES: the route's mode is the screen family, held, owned -- nothing else; a draw in the
//      mode that passed is counted as a re-issue, every other decision as it always was (kWorldScreen included, on the
//      frames the route does not own).
//   4. THE REFUSAL REASONS: each named, distinct, and logged once each for the first eight a session.
//   5. LIVENESS and the DOOR'S PREFLIGHT as truth tables; liveness equals the expression it replaced.
//   6. THE WIRING, by source scan, is ui_world_route_wiring_test.h (the rig runs from the repo root): the gatherer asks the route only for the screen
//      family; the re-issue runs under VrWorldInternalScope, between the game's draw and the verdict's undo, and tells
//      the route of an eye only after everything is back; the tail of the game's draw skips the per-eye motion
//      reissues for a draw the route re-issues and still recognises; the door's three places; the sharpen order.
namespace worldroute {

// The decision as shipped before the route (the tree at 5ca1d4c5). A frozen copy, on purpose: the rig must compare
// the new function against the old behaviour, not against itself.
UiLayerDecision decideV0(const UiLayerDrawFacts& f) {
    if (f.family == UiLayerFamily::kNone) return UiLayerDecision::kNotUi;
    if (!f.verdictForwards) return UiLayerDecision::kVerdict;
    if (f.worldScreen && f.family == UiLayerFamily::kScreen) return UiLayerDecision::kWorldScreen;
    if (!f.eyeTarget) return UiLayerDecision::kNotEyeTarget;
    if (!f.ldrView && !f.crispHdr) return UiLayerDecision::kHdrTarget;
    if (f.eye < 0 || f.eye > 1) return UiLayerDecision::kNoEye;
    if (!f.targetMatchesEye) return UiLayerDecision::kTargetSize;
    if (f.late) return UiLayerDecision::kLate;
    if (f.crispHdr && f.lateTone) return UiLayerDecision::kToneLate;
    if (!f.armed) return UiLayerDecision::kNotArmed;
    if (f.mrt) return UiLayerDecision::kMrt;
    if (f.ds.tests() && !f.dsReproducible) return UiLayerDecision::kDepthStencilTest;
    if (f.ds.writes() && f.substituted) return UiLayerDecision::kSubstitutedWrite;
    if (f.blend == UiBlendShape::kRefused) return UiLayerDecision::kBlendRefused;
    if (f.blend == UiBlendShape::kMultiply && f.substituted) return UiLayerDecision::kBlendRefused;
    if (f.crispHdr && f.blend == UiBlendShape::kMultiply) return UiLayerDecision::kBlendRefused;
    if (!f.layerReady) return UiLayerDecision::kLayerFailed;
    return UiLayerDecision::kRedirect;
}
// ...and the same without its world-screen line: what "every later test applies to the screen draw" means.
UiLayerDecision decideV0NoWorldTest(const UiLayerDrawFacts& f) {
    UiLayerDrawFacts g = f;
    g.worldScreen = false;
    return decideV0(g);
}

// One combination of the facts: a 13-bit mask of the booleans, a family, an eye, a blend and a depth-stencil effect.
UiLayerDrawFacts factsOf(uint32_t bits, uint32_t family, int eye, uint32_t blend, uint32_t ds) {
    UiLayerDrawFacts f;
    f.verdictForwards = (bits & (1u << 0)) != 0;
    f.worldScreen = (bits & (1u << 1)) != 0;
    f.eyeTarget = (bits & (1u << 2)) != 0;
    f.ldrView = (bits & (1u << 3)) != 0;
    f.targetMatchesEye = (bits & (1u << 4)) != 0;
    f.late = (bits & (1u << 5)) != 0;
    f.lateTone = (bits & (1u << 6)) != 0;
    f.armed = (bits & (1u << 7)) != 0;
    f.mrt = (bits & (1u << 8)) != 0;
    f.dsReproducible = (bits & (1u << 9)) != 0;
    f.substituted = (bits & (1u << 10)) != 0;
    f.layerReady = (bits & (1u << 11)) != 0;
    f.crispHdr = (bits & (1u << 12)) != 0;
    f.family = static_cast<UiLayerFamily>(family);
    f.eye = eye;
    f.blend = static_cast<UiBlendShape>(blend);
    f.ds.depthTest = (ds & 1u) != 0;
    f.ds.depthWrite = (ds & 2u) != 0;
    return f;
}

constexpr uint32_t kFamilies = static_cast<uint32_t>(UiLayerFamily::kCount);
constexpr uint32_t kBlends = static_cast<uint32_t>(UiBlendShape::kRefused) + 1;
constexpr int kEyes[4] = {-1, 0, 1, 2};

// Calls fn(facts) for every combination; returns how many.
template <class Fn>
uint64_t forEveryFacts(Fn fn) {
    uint64_t n = 0;
    for (uint32_t bits = 0; bits < (1u << 13); ++bits)
        for (uint32_t fam = 0; fam < kFamilies; ++fam)
            for (int eye : kEyes)
                for (uint32_t blend = 0; blend < kBlends; ++blend)
                    for (uint32_t ds = 0; ds < 4; ++ds) {
                        fn(factsOf(bits, fam, eye, blend, ds));
                        ++n;
                    }
    return n;
}

// ------------------------------------------------------------ 1, 2: the decision

void testDecision() {
    // 1. The key-off contract: worldRoute false is today's decision, everywhere.
    uint64_t differing = 0;
    const uint64_t combos = forEveryFacts([&](const UiLayerDrawFacts& f) {
        if (uiLayerDecide(f) != decideV0(f)) ++differing;
    });
    check(differing == 0, "worldRoute false: the decision equals the decision as shipped, for EVERY combination of the facts");
    std::printf("  ui_world_route: %llu combinations of the facts\n", static_cast<unsigned long long>(combos));

    // 2. worldRoute true: only the world-screen line of the 2D screen changes. The screen draw then faces every later
    //    test as any eye draw does; the rest -- another family, a screen that is not the world, a verdict that refuses
    //    first -- is decided as before.
    uint64_t wrong = 0, sawScreen = 0, sawRedirect = 0, sawNotArmed = 0, sawLate = 0;
    forEveryFacts([&](UiLayerDrawFacts f) {
        f.worldRoute = true;
        const bool screenHeld = f.worldScreen && f.family == UiLayerFamily::kScreen;
        const UiLayerDecision want = screenHeld ? decideV0NoWorldTest(f) : decideV0(f);
        const UiLayerDecision got = uiLayerDecide(f);
        if (got != want) ++wrong;
        if (screenHeld && f.verdictForwards) {
            ++sawScreen;
            sawRedirect += got == UiLayerDecision::kRedirect ? 1 : 0;
            sawNotArmed += got == UiLayerDecision::kNotArmed ? 1 : 0;
            sawLate += got == UiLayerDecision::kLate ? 1 : 0;
            if (got == UiLayerDecision::kWorldScreen) ++wrong;  // never, in the route's mode
        }
    });
    check(wrong == 0, "worldRoute true: only the held 2D screen's world-screen line changes; every other draw is decided as before");
    check(sawScreen > 0 && sawRedirect > 0 && sawNotArmed > 0 && sawLate > 0,
          "...and the held screen reaches kRedirect, kNotArmed and kLate (the combinations are not vacuous)");

    // ...and "as any opaque, no-depth eye draw": the held screen decides exactly as a menu panel with the same facts.
    uint64_t asAny = 0, asAnyWrong = 0;
    forEveryFacts([&](UiLayerDrawFacts f) {
        if (f.family != UiLayerFamily::kScreen) return;
        UiLayerDrawFacts screen = f, panel = f;
        screen.worldScreen = true;
        screen.worldRoute = true;
        panel.family = UiLayerFamily::kPanel;
        panel.worldScreen = false;
        panel.worldRoute = false;
        ++asAny;
        if (uiLayerDecide(screen) != uiLayerDecide(panel)) ++asAnyWrong;
    });
    check(asAny > 0 && asAnyWrong == 0,
          "the held screen in the route's mode is decided exactly as a menu panel with the same facts (every later test applies)");

    // Named cases, each the reason a reader of the log would see.
    UiLayerDrawFacts f;
    f.family = UiLayerFamily::kScreen;
    f.worldScreen = true;
    f.worldRoute = true;
    f.eyeTarget = true;
    f.ldrView = true;
    f.eye = 0;
    f.targetMatchesEye = true;
    f.armed = true;
    f.blend = UiBlendShape::kOpaque;
    f.layerReady = true;
    check(uiLayerDecide(f) == UiLayerDecision::kRedirect, "owned, armed, opaque, no depth: the layer re-issues the screen draw (kRedirect)");
    UiLayerDrawFacts notArmed = f;
    notArmed.armed = false;
    check(uiLayerDecide(notArmed) == UiLayerDecision::kNotArmed, "owned but the layer is not armed: kNotArmed, and the eye route serves the eye");
    UiLayerDrawFacts late = f;
    late.late = true;
    check(uiLayerDecide(late) == UiLayerDecision::kLate, "owned but the eye's door already ran: kLate");
    UiLayerDrawFacts noEye = f;
    noEye.eye = -1;
    check(uiLayerDecide(noEye) == UiLayerDecision::kNoEye, "owned but the eye is unknown: kNoEye");
    UiLayerDrawFacts notRoute = f;
    notRoute.worldRoute = false;
    check(uiLayerDecide(notRoute) == UiLayerDecision::kWorldScreen,
          "the same draw on a frame the route does not own: kWorldScreen, left in the picture, exactly as before");
    UiLayerDrawFacts menu = f;
    menu.worldScreen = false;
    check(uiLayerDecide(menu) == UiLayerDecision::kRedirect && uiLayerDecide(f) == uiLayerDecide(menu),
          "a screen that is not the world is taken whatever the route says (the route's flag changes nothing there)");
    UiLayerDrawFacts holo = f;
    holo.family = UiLayerFamily::kHolo;
    holo.ldrView = false;
    holo.crispHdr = true;
    check(uiLayerDecide(holo) == decideV0(holo), "another family with worldRoute true is decided as before");
}

// ------------------------------------------------------------ 3, 4: the mode, the tally, the reasons

void testModeAndTally() {
    // The mode: the screen family, held, owned -- nothing else.
    uint32_t modes = 0;
    for (uint32_t fam = 0; fam < kFamilies; ++fam)
        for (int held = 0; held < 2; ++held)
            for (int owned = 0; owned < 2; ++owned) {
                const bool mode = uiLayerWorldRouteMode(static_cast<UiLayerFamily>(fam), held != 0, owned != 0);
                const bool want = fam == static_cast<uint32_t>(UiLayerFamily::kScreen) && held && owned;
                check(mode == want, "the route's mode is the 2D screen family, held, on a frame the route owns -- nothing else");
                modes += mode ? 1 : 0;
            }
    check(modes == 1, "...exactly one combination of the forty-four");

    // The tally: a draw in the route's mode that passed is a re-issue; everything else is counted as it always was.
    for (uint32_t d = 0; d < static_cast<uint32_t>(UiLayerDecision::kCount); ++d) {
        const UiLayerDecision dec = static_cast<UiLayerDecision>(d);
        check(uiLayerWorldCount(dec, false) == UiWorldCount::kDecided,
              "off the route's mode every decision is counted as a decision (kWorldScreen and kRedirect included)");
        check(uiLayerWorldCount(dec, true) == (dec == UiLayerDecision::kRedirect ? UiWorldCount::kReissue : UiWorldCount::kDecided),
              "in the route's mode only the decision that passed is counted as a re-issue; a refusal is counted as that refusal");
    }
}

void testReasons() {
    // Every route reason has its own text, and none collides with another or with a decision's.
    std::vector<std::string> names;
    bool distinct = true, nonEmpty = true;
    for (uint32_t r = 1; r < static_cast<uint32_t>(UiWorldRefuse::kCount); ++r) {
        const std::string n = uiWorldRefuseName(static_cast<UiWorldRefuse>(r));
        nonEmpty = nonEmpty && !n.empty() && n != "?";
        for (const auto& other : names) distinct = distinct && other != n;
        names.push_back(n);
    }
    for (uint32_t d = 1; d < static_cast<uint32_t>(UiLayerDecision::kCount); ++d) {
        const std::string n = uiLayerDecisionName(static_cast<UiLayerDecision>(d));
        for (const auto& other : names) distinct = distinct && other != n;
        names.push_back(n);
    }
    check(nonEmpty && distinct, "every refusal reason, the route's and the decision's, has its own non-empty text");
    check(static_cast<uint32_t>(UiWorldRefuse::kCount) < kUiWorldDecisionBase &&
              uiWorldReasonId(UiLayerDecision::kNotArmed) >= kUiWorldDecisionBase &&
              std::strcmp(uiWorldReasonName(uiWorldReasonId(UiLayerDecision::kNotArmed)),
                          uiLayerDecisionName(UiLayerDecision::kNotArmed)) == 0 &&
              std::strcmp(uiWorldReasonName(uiWorldReasonId(UiWorldRefuse::kMipsNull)),
                          uiWorldRefuseName(UiWorldRefuse::kMipsNull)) == 0,
          "the route's reasons and the decision's share one id space without colliding, and each id names its own");
    check(uiWorldReasonId(UiLayerDecision::kLayerFailed) < 256, "...and an id fits the log's table");

    // The 30 s line names reasons by short keys (Log's line holds 1200 characters): each has one, distinct, and all of them
    // together leave room for the line's own text and their counts.
    std::vector<std::string> keys;
    size_t keyBytes = 0;
    bool keysOk = true;
    for (uint32_t r = 1; r < static_cast<uint32_t>(UiWorldRefuse::kCount); ++r) {
        const std::string k = uiWorldRefuseKey(static_cast<UiWorldRefuse>(r));
        keysOk = keysOk && !k.empty() && k != "?";
        for (const auto& other : keys) keysOk = keysOk && other != k;
        keys.push_back(k);
        keyBytes += k.size() + 10;   // "=", a count of up to eight digits, ", "
    }
    for (uint32_t d = 0; d < static_cast<uint32_t>(UiLayerDecision::kCount); ++d) {
        const std::string k = uiLayerDecisionKey(static_cast<UiLayerDecision>(d));
        keysOk = keysOk && !k.empty() && k != "?";
        for (const auto& other : keys) keysOk = keysOk && other != k;
        keys.push_back(k);
        keyBytes += k.size() + 10;
    }
    check(keysOk && uiWorldRefuseKey(UiWorldRefuse::kCount) == std::string("?") && keyBytes < 650,
          "every route reason and every decision has its own short key, and the keys of the 30 s line fit beside its text (1200 characters)");

    // The first eight DISTINCT reasons, once each -- and no ninth, however often any repeats.
    static_assert(kUiWorldReasonLines == 8, "the route logs the first EIGHT distinct reasons (the brief's number)");
    UiWorldReasonLog log;
    bool ok = true;
    for (uint16_t id = 1; id <= 8; ++id) ok = ok && log.first(id) && !log.first(id);
    check(ok, "the first eight distinct reasons are logged, each once (a repeat is not logged again)");
    check(!log.first(9) && !log.first(40) && !log.first(1), "...and the ninth distinct reason is not logged, nor is a repeat of the first");
    UiWorldReasonLog mixed;
    check(mixed.first(uiWorldReasonId(UiWorldRefuse::kDepthState)) && !mixed.first(uiWorldReasonId(UiWorldRefuse::kDepthState)) &&
              mixed.first(uiWorldReasonId(UiLayerDecision::kNotArmed)) && !mixed.first(uiWorldReasonId(UiLayerDecision::kNotArmed)),
          "a route reason and a decision's are distinct entries");

    // The line, as a reader of the log sees it.
    char line[400];
    uiWorldFormatRefusal(line, sizeof(line), 1, uiWorldReasonId(UiLayerDecision::kNotArmed));
    check(std::strcmp(line, "vr world route: layer did not take the screen draw for eye 1: layer not armed") == 0,
          "\"vr world route: layer did not take the screen draw for eye N: <reason>\" (a decision's reason)");
    uiWorldFormatRefusal(line, sizeof(line), 0, uiWorldReasonId(UiWorldRefuse::kMipsNull));
    check(std::strncmp(line, "vr world route: layer did not take the screen draw for eye 0: ", 62) == 0 &&
              std::strstr(line, uiWorldRefuseName(UiWorldRefuse::kMipsNull)) != nullptr,
          "...and a route's own");
    uiWorldFormatRefusal(line, sizeof(line), -1, uiWorldReasonId(UiLayerDecision::kNoEye));
    check(std::strncmp(line, "vr world route: layer did not take the screen draw for an unknown eye: ", 70) == 0,
          "...and for an eye that could not be told");
}

// ------------------------------------------------------------ 5: liveness, the door's preflight

void testLiveAndDoor() {
    // Liveness: equal to the expression it replaced, over every state; the reason names the first failure.
    const float targets[] = {0.0f, -1.0f, 1.0f, 1.25f, NAN};
    uint32_t live = 0, states = 0;
    bool same = true, reasonMatches = true;
    for (float target : targets)
        for (int t = 0; t < 2; ++t)
            for (int j = 0; j < 2; ++j)
                for (int s = 0; s < 2; ++s) {
                    const bool v0 = target > 0.0f && t && j && !s;  // refreshLive() as shipped
                    const bool now = uiLayerLiveFor(target, t != 0, j != 0, s != 0);
                    const char* why = uiLayerNotLiveReasonFor(target, t != 0, j != 0, s != 0);
                    same = same && now == v0;
                    reasonMatches = reasonMatches && (why == nullptr) == now;
                    live += now ? 1 : 0;
                    ++states;
                }
    check(same, "uiLayerLiveFor equals the expression refreshLive() evaluated before, over every state");
    check(reasonMatches && live == 2 && states == 5 * 8, "the not-live reason is null exactly when the layer is live (two live states: 1.0 and 1.25)");
    check(std::strcmp(uiLayerNotLiveReasonFor(0.0f, false, false, true), "fix.ui_quality is off") == 0 &&
              std::strstr(uiLayerNotLiveReasonFor(1.0f, false, true, false), "fix.temporal_aa") != nullptr &&
              std::strstr(uiLayerNotLiveReasonFor(1.0f, true, false, false), "jitter") != nullptr &&
              std::strstr(uiLayerNotLiveReasonFor(1.0f, true, true, true), "stood down") != nullptr,
          "each reason names its own cause (off, no temporal mode, jitter not as shipped, stood down), the first failing in that order");

    // The door's preflight: each missing fact refuses, in order; all present is ready; each gap has a text.
    UiWorldDoorFacts ready;
    ready.live = ready.layerMade = ready.holdsContent = ready.canComposite = ready.aspectMatches = true;
    check(uiWorldDoorGap(ready) == UiWorldDoorGap::kNone, "the door's preflight: a live layer that holds the frame, able to composite: ready");
    auto without = [&](auto undo) {
        UiWorldDoorFacts f = ready;
        undo(f);
        return uiWorldDoorGap(f);
    };
    check(without([](UiWorldDoorFacts& f) { f.live = false; }) == UiWorldDoorGap::kNotLive, "...not ready: the layer is not live");
    check(without([](UiWorldDoorFacts& f) { f.layerMade = false; }) == UiWorldDoorGap::kNoLayer, "...the eye has no layer");
    check(without([](UiWorldDoorFacts& f) { f.holdsContent = false; }) == UiWorldDoorGap::kNoContent, "...the layer holds nothing of this frame");
    check(without([](UiWorldDoorFacts& f) { f.alreadyComposited = true; }) == UiWorldDoorGap::kAlreadyDone, "...it was already composited this frame");
    check(without([](UiWorldDoorFacts& f) { f.runtimeDisabled = true; }) == UiWorldDoorGap::kRuntimeDisabled, "...the graphics runtime is shutting down");
    check(without([](UiWorldDoorFacts& f) { f.canComposite = false; }) == UiWorldDoorGap::kCannotComposite, "...the composite cannot run over the frame");
    check(without([](UiWorldDoorFacts& f) { f.aspectMatches = false; }) == UiWorldDoorGap::kAspect, "...the frame is not the layer's shape");
    bool named = true;
    for (uint32_t g = 1; g < static_cast<uint32_t>(UiWorldDoorGap::kCount); ++g) {
        const char* n = uiWorldDoorGapName(static_cast<UiWorldDoorGap>(g));
        named = named && n && n[0] && std::strcmp(n, "none") != 0;
    }
    check(named, "every gap has its own text");
    // With nothing ready the FIRST failing fact is the one named (the order is the order of the list above).
    check(uiWorldDoorGap(UiWorldDoorFacts{}) == UiWorldDoorGap::kNotLive, "a layer that is not live is named first");
}

// Everything here that needs no source file and no device. The wiring scans are ui_world_route_wiring_test.h's.
void testAll() {
    testDecision();
    testModeAndTally();
    testReasons();
    testLiveAndDoor();
}

}  // namespace worldroute
