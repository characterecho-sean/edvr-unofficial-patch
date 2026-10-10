// The game's final copy, admitted by what it is rather than by the tone pass in front of it
// (docs/design-flat-temporal-aa-2026-09-23.md, section 83).
//
// WHY. The copy route resolves at the game's final copy, after tone, and admitted a frame only when a whitelisted tone
// pass (3 vertex shaders x 4 pixel shaders) wrote the copy's source (flatSelectMonoFrame). Elite's other post chains
// (the DoF composite, the bloom composite, game AA, a mod's grade) put passes the whitelist has never met between H
// and the copy, and every frame was refused: no-known-tone-pass, a stand-down, a warning that named a setting. The HDR
// route (flat_hdr_route.h) fixed that for R >= D by resolving H before the chain. Below it (the game rendering at
// supersampling under 1.0, which is how Sean upscales in flat) the copy route still served, and still refused.
//
// WHAT IT ADMITS. At the final copy (the exact copy pair into the output, unchanged: the reducer's own checks) a frame is
// admitted when the copy's source S is
//   - an R8G8B8A8 image rendered this frame by one full-viewport pass with no depth, and not written outside a draw after it
//     (no Clear, Copy, Update or Map: the prefix model marks such a target bad),
//   - the scene's own size R (the size of the R11G11B10F target the scene was drawn into), a uniform scale of the output
//     D between half and twice, for a mode that has a route at that size (EDVR's `dlaa` has none below the output),
//   - written after the first pass that read the scene HDR (the HDR detector's trigger, seen this frame), with every
//     draw into H before it,
//   - with no other R-sized R8G8B8A8 image written between that first consumer and S,
// and the rest is the HDR selector's (H's camera, depth and the supported sources, one camera hash, in order). Nothing
// is said about the passes that make S from H, whatever their shaders (bloom, depth of field, any tone variant, a mod's
// grade): DLSS, FSR or TAA resolve S at the copy, exactly as they do for a known tone pass, and the game's own copy
// then shows the result. The whitelist is not consulted and not changed; a frame it selects is never touched, and where
// the HDR route serves the frame (R >= D) this does nothing at all.
//
// THE RISK, AND WHAT IS DONE ABOUT IT. A game anti-aliasing filter between the tone pass and the copy would make S from
// an image the game has already filtered: FXAA gives a softer double AA, and the game's own temporal AA jitters the
// camera itself, which fights EDVR's phase. Neither can be told from the structure, but the filter leaves a mark the
// tone pass does not: it writes an R-sized R8G8B8A8 target (the tone pass's own) and then S. So a frame with one or
// more such targets between the scene's first consumer and S is declined, refused exactly as it was before this
// existed, and the F8 panel's Anti-aliasing advice names it. The corpus has 42 frames with a known tone pass (stock and
// EDHM tone pairs, menu, flight and on foot, R from half of D to 1.5 D) and 3 with Bloom 3 and DoF 2 and an unknown
// one, and none has such a target; the chain length rides every decline and the 5 s census, so a flight with the
// game's AA on reads what it was. Relaxing the rule (FXAA is probably fine) is one line, and waits for that flight.
//
// WHAT IT NAMES. A frame with a copy and no scene at all (startup, a loading screen, a 2D menu) is no-3d-scene: it
// stands the work down like no-known-output-copy and never warns. A scene whose size is not a uniform scale of the
// output between half and twice (Elite's resolution is not the screen's shape: 2176x1224 on 2560x1600) is
// render-size-does-not-fit-output, with the measured sizes, so the stand-down line and the F8 panel can say what it is.
//
// Pure, like the rest of the HDR route: tools\flat_temporal_test drives it on the trace corpus and on mutated streams
// and the runtime calls the same function.
#pragma once
#include "flat_hdr_route.h"
#include <cstdio>
#include <cstring>

namespace edvr {

// ---- the scene's size ------------------------------------------------------------------------------------------
// The scene as the prefix model saw it: the R11G11B10F target with a depth of its own size that the most draws went
// into, at least kFlatSceneMinDraws of them (the scene's own H takes 22 to 184 draws a frame in every trace; a probe or a
// cubemap face takes a handful). On-shape when its size is a uniform scale of D from half to twice (what the whitelist
// and the HDR detector call a screen target); a frame with no on-shape candidate and an off-shape one is the game
// rendering at a size that does not fit the output.
constexpr uint32_t kFlatSceneMinDraws = 8;
struct FlatSceneFacts {
    bool onShape = false;
    bool offShape = false;             // no on-shape candidate, and an off-shape one
    uint32_t width = 0, height = 0, draws = 0;
    const void* resource = nullptr;
};
inline bool flatRenderFitsOutput(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    return flatUniformScale(w, h, ow, oh) && uint64_t(w) * 2 >= ow && uint64_t(h) * 2 >= oh &&
           uint64_t(w) <= uint64_t(ow) * 2 && uint64_t(h) <= uint64_t(oh) * 2;
}
inline FlatSceneFacts flatSceneFacts(const FlatRuntimePrefix& p, uint32_t outW, uint32_t outH) {
    FlatSceneFacts on{}, off{};
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& t = p.targets[i];
        const auto& k = t.writes.key;
        if (t.writes.draws < kFlatSceneMinDraws || k.format != 26 || !k.color || !k.rtv || !k.depth || !k.dsv ||
            !k.width || !k.height || k.depthWidth != k.width || k.depthHeight != k.height)
            continue;
        FlatSceneFacts& slot = flatRenderFitsOutput(k.width, k.height, outW, outH) ? on : off;
        if (slot.draws < t.writes.draws) {
            slot.width = k.width; slot.height = k.height; slot.draws = t.writes.draws; slot.resource = t.resource;
        }
    }
    if (on.draws) { on.onShape = true; return on; }
    if (off.draws) { off.offShape = true; return off; }
    return FlatSceneFacts{};
}

// ---- where the weapon's passes are judged -----------------------------------------------------------------------
// A frame with a weapon up has two kinds of draw the scene-camera model cannot take as they are: the first-person pool cohort
// (an alternate camera at the scene's depth) and the glow pass (an HDR draw from a second camera). The HDR route judges both when
// it treats the frame: a protected overlay for the glow pass and the qualified alternate source, with its coverage map, for the
// cohort. The copy route has none of that, and until the copy route learned the weapon every such frame was refused (section 104).
//   Hdr:       the frame is the HDR route's (the key auto, the route not latched, a mode that resolves at the render size): what
//              the runtime does for it has not changed.
//   Copy:      the copy route judges it, for DLSS and FSR: the glow pass is admitted as an alternate HDR writer (no overlay), the
//              cohort stays out of the sources and makes the frame mixed-camera, and the foreground contract qualifies it at the
//              final copy. This is every frame the HDR route will not treat, which includes the key off at any size.
//   Unchanged: EDVR's TAA and DLAA, where the HDR route will not treat the frame: they keep what they always had.
enum class FlatWeaponRoute : uint8_t { Hdr, Copy, Unchanged };
inline const char* flatWeaponRouteName(FlatWeaponRoute r) {
    switch (r) {
    case FlatWeaponRoute::Hdr: return "hdr";
    case FlatWeaponRoute::Copy: return "copy";
    case FlatWeaponRoute::Unchanged: return "unchanged";
    }
    return "?";
}
// The copy route's mixed-camera verdict (the first-person contract's trigger) has two witnesses. The model's own: the first-person cohort
// it was told of (FlatRuntimeDraw::firstPersonCohort, FlatMonoFrame::mixedCamera), which keeps SUPPORTED alternate-camera pool draws from
// making the frame ambiguous. And the domain's: a first-person draw it planned into the frame's depth (the count the depth candidate keeps
// per frame), seen whatever the model took the draw for. The plasma weapon's first-person draws are unsupported pairs, so they reach the
// model as nothing at all and the cohort flag never fires for them; the domain, which proves a draw by its own bytes, planned them. Either
// witness asks the contract; the domain's is taken only where the copy route judges the weapon (flatWeaponRoute == Copy), so EDVR's TAA
// and DLAA keep what they had. A holstered-arms frame is planned too: it qualifies as it does on the HDR route, with the arms' own motion.
inline bool flatCopyMixedCamera(bool modelMixed, FlatWeaponRoute route, uint32_t domainForeignPlanned) {
    return modelMixed || (route == FlatWeaponRoute::Copy && domainForeignPlanned > 0);
}
inline FlatWeaponRoute flatWeaponRoute(bool keyAuto, bool routeLatched, FlatMonoResolveMode mode,
                                       uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH) {
    if (keyAuto && !routeLatched && flatHdrRouteEvaluatesAtRender(mode, renderW, renderH, outputW, outputH))
        return FlatWeaponRoute::Hdr;
    return mode == FlatMonoResolveMode::Dlss || mode == FlatMonoResolveMode::Fsr ? FlatWeaponRoute::Copy
                                                                                : FlatWeaponRoute::Unchanged;
}

// ---- the admission --------------------------------------------------------------------------------------------
struct FlatCopyPolicy {
    bool structure = false;              // admit by structure (the HDR route is on)
    FlatMonoResolveMode mode = FlatMonoResolveMode::Taa;
    bool routeLatched = false;           // the HDR route turned itself off this session (flat_hdr_route.h's latch)
};
enum class FlatCopyOutcome : uint8_t {
    Untouched,       // the whitelist selected, or refused for a reason this has nothing to say about
    Admitted,        // selected by structure
    RouteServes,     // the frame is the HDR route's (R >= D, the mode resolves at R): the whitelist's answer stands
    Disabled,        // the key is off: the whitelist's answer stands
    Declined,        // the structure was looked at and is not one this recognises: the whitelist's answer stands
    Refused,         // the structure is recognised and the selector refuses it for its own reason (that reason is returned)
    NoScene,         // a final copy and no scene in the frame
    RenderSize       // the scene's size does not fit the output
};
inline const char* flatCopyOutcomeName(FlatCopyOutcome o) {
    switch (o) {
    case FlatCopyOutcome::Untouched: return "untouched";
    case FlatCopyOutcome::Admitted: return "admitted";
    case FlatCopyOutcome::RouteServes: return "route-serves";
    case FlatCopyOutcome::Disabled: return "disabled";
    case FlatCopyOutcome::Declined: return "declined";
    case FlatCopyOutcome::Refused: return "selector-refused";
    case FlatCopyOutcome::NoScene: return "no-scene";
    case FlatCopyOutcome::RenderSize: return "render-size";
    }
    return "unknown";
}
// What the admission saw, for the log and the census. Everything is a value (no pointer outlives the call).
struct FlatCopyDiag {
    FlatCopyOutcome outcome = FlatCopyOutcome::Untouched;
    const char* why = "";                // the decline's cause, a literal; empty unless Declined
    FlatMonoReason whitelist = FlatMonoReason::Selected;   // what the whitelist said
    uint32_t srcWidth = 0, srcHeight = 0, srcFormat = 0, srcDraws = 0;
    uint64_t srcVs = 0, srcPs = 0;
    uint32_t sceneWidth = 0, sceneHeight = 0, outputWidth = 0, outputHeight = 0;
    uint64_t triggerVs = 0, triggerPs = 0;
    // R-sized R8G8B8A8 targets written after the first consumer of H and before S (0 where S is the first LDR pass),
    // and the draws into them: how long the chain between the scene and the copy was.
    uint32_t ldrTargetsBefore = 0, ldrDrawsBefore = 0;
    bool menu = false;                   // the frame came through the verified menu HDR copy
};

namespace flat_copy_detail {
// The copy draw's own checks, which flatSelectMonoFrame makes only after the reducer has found a tone pass.
inline bool plainCopy(const FlatRuntimePrefix& p, const FlatContractObservation& k, const FlatContractRecord& copy) {
    using namespace flat_mono_detail;
    return oneDraw(copy) && k.kind == kFlatContractOutput && k.rtv && !k.depth && !k.dsv && k.format == p.format &&
           k.width == p.width && k.height == p.height && fullViewport(k, p.width, p.height) &&
           k.srvView[0] && k.srvResource[0] && k.srvResource[0] != p.output;
}
inline const FlatRuntimeTarget* findTarget(const FlatRuntimePrefix& p, const void* resource) {
    if (!resource) return nullptr;
    for (uint32_t i = 0; i < p.targetsUsed; ++i) if (p.targets[i].resource == resource) return &p.targets[i];
    return nullptr;
}
}  // namespace flat_copy_detail

// The final copy's verdict. `whitelist` is what the reducer said at this copy draw (flatRuntimeObserveContract);
// `d` is the copy draw; `f` the HDR detector's state for the frame so far. Returns what the frame is: the whitelist's
// own answer unless this has something better to say. A frame is only ever touched when the whitelist refused it for
// its tone pass (no-known-tone-pass, invalid-tone-pass): every other refusal is the same refusal it was.
inline FlatMonoFrame flatCopyAdmit(FlatRuntimePrefix& p, const FlatHdrFrame& f, const FlatRuntimeDraw& d,
                                   const FlatMonoFrame& whitelist, const FlatCopyPolicy& policy,
                                   FlatCopyDiag* diag = nullptr) {
    using namespace flat_mono_detail;
    FlatCopyDiag local;
    FlatCopyDiag& g = diag ? *diag : local;
    g = FlatCopyDiag{};
    g.whitelist = whitelist.reason;
    g.outputWidth = p.width; g.outputHeight = p.height;
    if (whitelist.selected() ||
        (whitelist.reason != FlatMonoReason::NoTonePass && whitelist.reason != FlatMonoReason::InvalidTonePass))
        return whitelist;

    // What the scene is, before anything about the chain.
    const FlatSceneFacts scene = flatSceneFacts(p, p.width, p.height);
    g.sceneWidth = scene.width; g.sceneHeight = scene.height;
    if (!scene.onShape && !scene.offShape) {
        g.outcome = FlatCopyOutcome::NoScene;
        FlatMonoFrame out = whitelist; out.reason = FlatMonoReason::NoScene;
        return out;
    }
    if (!scene.onShape) {
        g.outcome = FlatCopyOutcome::RenderSize;
        FlatMonoFrame out = whitelist; out.reason = FlatMonoReason::RenderSize;
        out.renderWidth = scene.width; out.renderHeight = scene.height;
        out.outputWidth = p.width; out.outputHeight = p.height;
        return out;
    }
    if (!policy.structure) { g.outcome = FlatCopyOutcome::Disabled; return whitelist; }
    // Where the HDR route serves the frame it is the route's, byte for byte as before this existed.
    if (!policy.routeLatched &&
        flatHdrRouteEvaluatesAtRender(policy.mode, scene.width, scene.height, p.width, p.height)) {
        g.outcome = FlatCopyOutcome::RouteServes;
        return whitelist;
    }

    const auto decline = [&](const char* why) {
        g.outcome = FlatCopyOutcome::Declined; g.why = why;
        return whitelist;
    };
    // A mode whose route is refused at this size resolves nothing here whatever the frame is (EDVR's `dlaa` below the output:
    // flatResolveRoute says dlaa-requires-native). Admitting the frame would make it Treatable and the resolver would then refuse
    // every one in silence, with no stand-down and no warning; declining keeps the whitelist's answer, and so the old stand-down.
    if (flatResolveRoute(policy.mode, scene.width, scene.height, p.width, p.height).refused)
        return decline("the-mode-has-no-route-at-this-render-size");
    const FlatContractObservation& k = d.key;
    const FlatContractRecord current = flatRuntimeRecord(d, p.sequence, p.frame);
    if (!flat_copy_detail::plainCopy(p, k, current)) return decline("the-copy-is-not-a-plain-full-screen-copy");
    const FlatRuntimeTarget* src = flat_copy_detail::findTarget(p, k.srvResource[0]);
    if (!src || !src->writes.draws) return decline("the-copy-source-was-not-rendered-this-frame");
    const auto& sk = src->writes.key;
    g.srcWidth = sk.width; g.srcHeight = sk.height; g.srcFormat = sk.format; g.srcDraws = src->writes.draws;
    g.srcVs = sk.vs; g.srcPs = sk.ps;
    if (!oneDraw(src->writes)) return decline("the-copy-source-is-not-written-by-one-pass");
    if (sk.format != 27) return decline("the-copy-source-is-not-r8g8b8a8");
    // An explicit write into S after its pass (a Clear, a Copy, an Update or a Map: the prefix model marks the target bad) is what
    // the whitelist's tone count refuses (flatRuntimeWritten zeroes it); the unknown tone pass has no count to lose, so it is
    // said here. For an R8G8B8A8 target the mark means nothing else (the model's other conflicts are for HDR and image-source
    // targets), and writes before S's pass, the game clearing the target for its own use, are not marked.
    if (src->hdrBad) return decline("the-copy-source-was-written-outside-a-draw");
    if (!sk.rtv || sk.depth || sk.dsv) return decline("the-copy-source-was-drawn-with-a-depth-buffer");
    if (!fullViewport(sk, sk.width, sk.height)) return decline("the-copy-source-pass-is-not-full-viewport");
    if (sk.width != scene.width || sk.height != scene.height) return decline("the-copy-source-is-not-the-scene-size");
    if (!f.triggered) return decline("no-pass-read-the-scene-hdr");
    if (f.trigger.ambiguous) return decline("more-than-one-hdr-candidate");
    if (f.trigger.hdr != scene.resource) return decline("the-first-consumer-reads-another-target");
    if (f.trigger.sequence > src->writes.first) return decline("the-copy-source-was-written-before-the-scene-was-read");
    g.triggerVs = f.trigger.vs; g.triggerPs = f.trigger.ps;
    // The chain between the scene and the copy, measured: R-sized R8G8B8A8 targets other than S that were written after
    // the first consumer of H and before S. Bloom, depth of field and every tone variant leave none (the corpus has 44
    // frames of them, all zero): the tone pass writes S itself. A game anti-aliasing filter after the tone pass writes
    // the target the tone pass made and then S, so it leaves one or more. The resolve then would run on an image the
    // game has already filtered (FXAA: softer, double AA) or, for the game's temporal AA, whose camera jitter fights
    // EDVR's; neither can be told from the structure, so a frame with one stays refused exactly as it was, and the
    // Anti-aliasing advice stays (the F8 panel names it). The count rides the decline, so a flight can read the chain.
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& t = p.targets[i];
        if (t.resource == k.srvResource[0] || !t.writes.draws || t.writes.key.format != 27 ||
            t.writes.key.width != scene.width || t.writes.key.height != scene.height ||
            t.writes.first < f.trigger.sequence || t.writes.first >= src->writes.first)
            continue;
        ++g.ldrTargetsBefore; g.ldrDrawsBefore += t.writes.draws;
    }
    if (g.ldrTargetsBefore) return decline("r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr");

    FlatMonoFrame sel = flatSelectHdrRouteAt(p, f, [](uint64_t, uint64_t) { return true; }, src->writes.first,
                                             FlatHdrExtentGate::UniformHalfToDouble);
    if (!sel.selected()) {
        g.outcome = FlatCopyOutcome::Refused;
        return sel;
    }
    // A first-person cohort the model left out of the sources (FlatRuntimeDraw::firstPersonCohort) makes the admitted frame
    // mixed-camera, as it does for the whitelist's own selection (flatRuntimeObserve's copy branch).
    if (flatRuntimeFirstPerson(p, sel.depth)) sel.mixedCamera = true;
    // The frame is the copy's: its source is S, and a frame that came through the verified menu copy names that copy's
    // inherited destination as its HDR (the 3D menu's stale-slot policy asks for it: flatFrameThroughMenuCopy).
    sel.color = k.srvResource[0];
    sel.toneSequence = src->writes.first;
    sel.copySequence = current.first;
    sel.firstLaterOutput = 0;
    if (p.menuCopiesAccepted) {
        const FlatRuntimeTarget* h = flat_copy_detail::findTarget(p, scene.resource);
        for (uint32_t i = 0; h && i < p.targetsUsed; ++i) {
            const auto& t = p.targets[i];
            if (t.menuInherited && t.resource != scene.resource && t.writes.key.depth == h->writes.key.depth &&
                t.writes.key.width == scene.width && t.writes.key.height == scene.height) {
                sel.hdr = t.resource; g.menu = true;
                break;
            }
        }
    }
    g.outcome = FlatCopyOutcome::Admitted;
    return sel;
}

// ---- the census and the log lines -------------------------------------------------------------------------------
// One 5 s window, reset when it prints; the line is printed every window while a temporal mode runs, zeros included: an
// absent line is what "the admission never ran" looks like, and `copies=N whitelist=N admitted=0` is what it looks like
// when it ran and every frame had a known tone pass.
struct FlatCopyWindow {
    uint64_t copies = 0;            // final copy draws the reducer ruled on
    uint64_t whitelist = 0;         // ... the whitelist selected
    uint64_t admitted = 0, declined = 0, selectorRefused = 0, noScene = 0, renderSize = 0, routeServes = 0, disabled = 0;
    const char* last = "none";      // the last outcome's name
    uint32_t lastSceneW = 0, lastSceneH = 0, lastOutW = 0, lastOutH = 0, lastSrcW = 0, lastSrcH = 0;
    uint64_t lastSrcVs = 0, lastSrcPs = 0;
    uint32_t ldrBeforeMax = 0;      // the longest chain of R-sized image passes between the scene and the copy seen this window
    static constexpr uint32_t kWhys = 6;
    struct Why { const char* name = nullptr; uint64_t count = 0; };
    Why whys[kWhys]{};
    uint64_t whyOther = 0;
    void noteWhy(const char* name) {
        if (!name || !*name) return;
        for (auto& w : whys) {
            if (!w.name) { w.name = name; w.count = 1; return; }
            if (w.name == name || std::strcmp(w.name, name) == 0) { ++w.count; return; }
        }
        ++whyOther;
    }
    void noteWhitelist() { ++copies; ++whitelist; }
    void note(const FlatCopyDiag& g) {
        ++copies;
        switch (g.outcome) {
        case FlatCopyOutcome::Untouched: break;
        case FlatCopyOutcome::Admitted: ++admitted; break;
        case FlatCopyOutcome::Declined: ++declined; noteWhy(g.why); break;
        case FlatCopyOutcome::Refused: ++selectorRefused; break;
        case FlatCopyOutcome::NoScene: ++noScene; break;
        case FlatCopyOutcome::RenderSize: ++renderSize; break;
        case FlatCopyOutcome::RouteServes: ++routeServes; break;
        case FlatCopyOutcome::Disabled: ++disabled; break;
        }
        last = flatCopyOutcomeName(g.outcome);
        if (g.ldrTargetsBefore > ldrBeforeMax) ldrBeforeMax = g.ldrTargetsBefore;
        if (g.sceneWidth) { lastSceneW = g.sceneWidth; lastSceneH = g.sceneHeight; }
        lastOutW = g.outputWidth; lastOutH = g.outputHeight;
        if (g.srcWidth) { lastSrcW = g.srcWidth; lastSrcH = g.srcHeight; lastSrcVs = g.srcVs; lastSrcPs = g.srcPs; }
    }
    void reset() { *this = FlatCopyWindow{}; }
};
inline int flatCopyFormatWindow(char* out, size_t size, bool structureOn, const FlatCopyWindow& w) {
    int n = std::snprintf(out, size,
        "flat copy structure 5s: key=%s copies=%llu whitelist=%llu admitted=%llu declined=%llu selector-refused=%llu "
        "no-scene=%llu render-size=%llu route-serves=%llu key-off=%llu last=%s scene=%ux%u output=%ux%u "
        "source=%ux%u VS=%016llX PS=%016llX ldr-passes-before-max=%u declines=",
        structureOn ? "auto" : "off", static_cast<unsigned long long>(w.copies),
        static_cast<unsigned long long>(w.whitelist), static_cast<unsigned long long>(w.admitted),
        static_cast<unsigned long long>(w.declined), static_cast<unsigned long long>(w.selectorRefused),
        static_cast<unsigned long long>(w.noScene), static_cast<unsigned long long>(w.renderSize),
        static_cast<unsigned long long>(w.routeServes), static_cast<unsigned long long>(w.disabled), w.last,
        w.lastSceneW, w.lastSceneH, w.lastOutW, w.lastOutH, w.lastSrcW, w.lastSrcH,
        static_cast<unsigned long long>(w.lastSrcVs), static_cast<unsigned long long>(w.lastSrcPs), w.ldrBeforeMax);
    const auto appendText = [&](const char* text) {
        if (n < 0 || static_cast<size_t>(n) >= size) return;
        const int more = std::snprintf(out + n, size - static_cast<size_t>(n), "%s", text);
        if (more > 0) n += more;
    };
    bool any = false;
    for (const auto& why : w.whys) {
        if (!why.name) break;
        if (any) appendText(",");
        char one[160];
        std::snprintf(one, sizeof(one), "%s:%llu", why.name, static_cast<unsigned long long>(why.count));
        appendText(one);
        any = true;
    }
    if (w.whyOther) {
        char one[48];
        std::snprintf(one, sizeof(one), "%sother:%llu", any ? "," : "", static_cast<unsigned long long>(w.whyOther));
        appendText(one);
        any = true;
    }
    if (!any) appendText("none");
    return n;
}
// The first admission of a session, once. `routeName` is flatResolveRoute's name for the mode at this size.
inline int flatCopyFormatFirstAdmission(char* out, size_t size, uint64_t frame, const FlatCopyDiag& g, const char* routeName) {
    return std::snprintf(out, size,
        "flat copy structure: first admission at frame=%llu (the HDR route is on): the game's final "
        "copy reads a %ux%u R8G8B8A8 image written by one pass, VS=%016llX PS=%016llX, after the scene HDR's first consumer "
        "(VS=%016llX PS=%016llX), with no other R-sized image pass in between; no whitelisted tone pass wrote it (the "
        "whitelist said %s); the scene is %ux%u on a %ux%u output%s, route=%s; admitted by structure, so bloom, depth of "
        "field and the tone variant do not matter",
        static_cast<unsigned long long>(frame), g.srcWidth, g.srcHeight, static_cast<unsigned long long>(g.srcVs),
        static_cast<unsigned long long>(g.srcPs), static_cast<unsigned long long>(g.triggerVs),
        static_cast<unsigned long long>(g.triggerPs), flatMonoReasonName(g.whitelist),
        g.sceneWidth, g.sceneHeight, g.outputWidth, g.outputHeight, g.menu ? " (the 3D menu)" : "",
        routeName ? routeName : "unknown");
}
// A frame the structure looked at and did not recognise, a dozen a session (the runtime counts).
inline int flatCopyFormatDeclined(char* out, size_t size, uint64_t frame, const FlatCopyDiag& g) {
    return std::snprintf(out, size,
        "flat copy structure: declined at frame=%llu: %s (the whitelist said %s); the final copy reads a %ux%u fmt %u "
        "image with %u writer(s) (first VS=%016llX PS=%016llX), %u R-sized image pass(es) between the scene HDR's first "
        "consumer and it (%u draw(s)), the scene is %ux%u on a %ux%u output",
        static_cast<unsigned long long>(frame), g.why, flatMonoReasonName(g.whitelist), g.srcWidth, g.srcHeight,
        g.srcFormat, g.srcDraws, static_cast<unsigned long long>(g.srcVs), static_cast<unsigned long long>(g.srcPs),
        g.ldrTargetsBefore, g.ldrDrawsBefore, g.sceneWidth, g.sceneHeight, g.outputWidth, g.outputHeight);
}
// The render size's own words, for the stand-down line and the F8 panel ("Elite renders 2176x1224 on a 2560x1600 screen").
inline int flatRenderSizeWords(char* out, size_t size, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    return std::snprintf(out, size, "Elite renders %ux%u on a %ux%u screen", rw, rh, ow, oh);
}

// The copy route's weapon census (flatWeaponRoute == Copy), one 5 s window printed every window while a temporal mode runs, zeros
// included: an absent line is what "the weapon wiring never ran" looks like, and `cohort-draws=0` is "it ran and no weapon was up"
// (with the HDR route serving, or TAA and DLAA, every field is zero by construction). The H counters are the foreground contract's
// own, taken at the final copy: `H-attempts` is the frames the copy selected mixed-camera and asked for the coverage map, and
// `H-qualified` how many got one; the refusal's name rides the `flat foreground SDK domain` line (last-refusal), as on the HDR route.
struct FlatCopyWeaponWindow {
    uint64_t cohortDraws = 0;        // first-person pool draws flagged for the model (FlatRuntimeDraw::firstPersonCohort)
    uint64_t alternateDraws = 0;     // glow-pass draws admitted as alternate HDR writers (FlatRuntimeDraw::alternateHdr), no overlay
    uint64_t mixedFrames = 0;        // copy selections that came out mixed-camera (the cohort was in the frame, or the domain planned a first-person draw)
    uint64_t domainMixedFrames = 0;  // ... of those, the frames only the domain made mixed (the model saw no cohort: an unsupported weapon, holstered arms)
    uint64_t hAttempts = 0, hQualified = 0;
    void reset() { *this = FlatCopyWeaponWindow{}; }
};
inline int flatCopyWeaponFormatWindow(char* out, size_t size, const char* modeName, const FlatCopyWeaponWindow& w) {
    return std::snprintf(out, size,
        "flat copy weapon 5s: mode=%s cohort-draws=%llu alternate-glow-draws=%llu mixed-frames=%llu domain-mixed-frames=%llu "
        "H-attempts=%llu H-qualified=%llu; DLSS and FSR where the HDR route does not treat the frame, no overlay is planned there, and "
        "the refusal behind a missing map is the last-refusal on the flat foreground SDK domain line",
        modeName ? modeName : "?", static_cast<unsigned long long>(w.cohortDraws),
        static_cast<unsigned long long>(w.alternateDraws), static_cast<unsigned long long>(w.mixedFrames),
        static_cast<unsigned long long>(w.domainMixedFrames),
        static_cast<unsigned long long>(w.hAttempts), static_cast<unsigned long long>(w.hQualified));
}

}  // namespace edvr
