// The VR on-foot world route: the pure half and the detector's glue (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// WHAT IS PINNED
//   1. THE KEY-OFF CONTRACT: with experimental.temporal_aa_on_foot_world off (or any value that is not "auto") the machine
//      never leaves Off whatever else the frame shows, nothing is owned, the eye shift is never suppressed, the layer is
//      never told the world is the route's, the door never runs layer-only; and the VR hooks' added lines are guarded so
//      that with the key off each costs one load (source scans).
//   2. THE OWNERSHIP MACHINE: warm-up, ownership, the grace for single refusals, release by the gate, the layer, a scene
//      reset, a run of untreated frames and the late-write latch (with its reset by the key).
//   3. THE SELECTOR: each reason in order, each alone.
//   4. THE TRIGGER on the VR on-foot chain the 2026-09-30 census retake showed (design doc section 82, census retake table).
//      THERE IS NO TRACE of that chain: the fixture below is SYNTHETIC, built from the retake's chain table (its q numbers,
//      sizes, formats, who reads what) and the 66 census lines of q 8150-8215 that were read from the log
//      (edvr_gfx_20260930_104653.log). It runs the real flatHdrObserveDraw through the route's own glue
//      (vrWorldFillObservation) and asserts the trigger is q 8203, the tone, with H at t1, and only there, with each of the
//      rules (i)-(iv) and the mid-frame copy of H and the late writes mutated in turn.
//   5. THE 5 s LINE and the take/release lines, bounded; and the curved screen in them (curve=, curve-reissues=, the OWNS line's sentence).
//   6. SOURCE PINS for the hooks (the internal-call early returns on the nine thunks, the detector's call sites, the write
//      observers, the boundary tick), for what the route may do with the camera detour (stage 2: drive it only while the key
//      is auto and only through the two helpers, close its window at the trigger, take the phase it confirms) and for the
//      key-off contract (the boundary's early return keeps today's single test; nothing of stage 2 runs before it), and for the
//      curved route (a curved screen does not hold the route off; its curve is named in two lines), each held by controls: copies of
//      the source with one edit that must trip the pin named.
//   7. STAGE 2, THE PURE HALF (design doc section 82, stage 2): the A/B key and the decision table that turns it, the
//      weapon fold-in's mode, the injector counters' window and its line, the STOP on an injected kind other than 3, the
//      excluded-call, no-scene and rows-mismatch lines, the decline log's cap per run of declines, and the RELEASED line's
//      last decline.
//   8. THE ON-FOOT MAPS GATE AND THE ROUTE (docs/design-world-camera-motion-2026-09-30.md, Phase 1): the gate's release count is
//      the route's grace, so a map's opening releases both on one boundary; the gate holds the world again two named frames
//      after the map closes and the route, which needs the gate, owns again kVrWorldWarmFrames treated frames after that, and is
//      never owned while the gate is open. Run through the two real machines the way the boundaries order them (the layer's
//      gate first, then the route's, which reads it).
// --self-test <repo root> runs everything and prints "vr world route: PASS" only when every check holds.

#include "../../src/d3d11/vr_world_route_math.h"
#include "../../src/d3d11/ui_maps_math.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace edvr;

int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}

// ---- 1. the key ------------------------------------------------------------------------------------------------------
void keyCases() {
    check(vrWorldKeyFromText("auto") == VrWorldKey::Auto, "key: auto reads auto");
    check(vrWorldKeyFromText("AUTO") == VrWorldKey::Auto && vrWorldKeyFromText("Auto") == VrWorldKey::Auto,
          "key: any case of auto reads auto");
    const char* notAuto[] = {"off", "", "on", "autox", "aut", "auto ", " auto", "true", "1", "dlss", "automatic", "yes"};
    bool allOff = true;
    for (const char* t : notAuto) allOff = allOff && vrWorldKeyFromText(t) == VrWorldKey::Off;
    check(allOff && vrWorldKeyFromText(nullptr) == VrWorldKey::Off,
          "key: anything that is not exactly auto, a typo included, reads off: the route is never switched on by accident");
    check(std::strcmp(vrWorldKeyName(VrWorldKey::Auto), "auto") == 0 && std::strcmp(vrWorldKeyName(VrWorldKey::Off), "off") == 0,
          "key: the names are the ini's words");
}

// ---- 2. the machine --------------------------------------------------------------------------------------------------
VrWorldFrameEnd frame(bool key, bool layer, bool gate, bool treated, bool late = false, bool reset = false) {
    VrWorldFrameEnd f;
    f.keyOn = key; f.layerLive = layer; f.gate = gate; f.treated = treated; f.lateWrites = late; f.sceneReset = reset;
    return f;
}
VrWorldStep run(VrWorldMachine& m, unsigned n, const VrWorldFrameEnd& f) {
    VrWorldStep last;
    for (unsigned i = 0; i < n; ++i) last = m.frameEnd(f);
    return last;
}
VrWorldMachine owned() {
    VrWorldMachine m;
    run(m, kVrWorldWarmFrames, frame(true, true, true, true));
    return m;
}

void keyOffContract() {
    // With the key off, whatever the frames show, the route is not there.
    VrWorldMachine m;
    bool neverOff = true, neverOwned = true, neverWants = true, neverEvents = true, neverSuppress = true, neverTake = true;
    uint32_t bits = 1;
    for (unsigned i = 0; i < 4096; ++i) {
        bits = bits * 1664525u + 1013904223u;
        VrWorldFrameEnd f = frame(false, bits & 1u, bits & 2u, bits & 4u, bits & 8u, bits & 16u);
        const VrWorldStep s = m.frameEnd(f);
        neverOff = neverOff && m.state == VrWorldState::Off;
        neverOwned = neverOwned && !m.owned();
        neverWants = neverWants && !m.wantsDraws();
        neverEvents = neverEvents && !s.entered && s.released == VrWorldRelease::None;
        neverSuppress = neverSuppress && !vrWorldSuppressesEyeShift(m.state);
        neverTake = neverTake && !vrWorldLayerMayTake(m.state, (bits & 4u) != 0) && !vrWorldDoorLayerOnly(0, bits);
    }
    check(neverOff && neverOwned && neverWants && neverEvents,
          "key off: 4096 frames of every combination of the other facts leave the machine Off, unowned, wanting no draws, with no events");
    check(neverSuppress && neverTake,
          "key off: the eye shift is never suppressed, the layer is never told the world is the route's, the door never runs layer-only");
    // A route that was owned when the key goes off lets go AT ONCE, with its reason.
    VrWorldMachine o = owned();
    const VrWorldStep s = o.frameEnd(frame(false, true, true, true));
    check(o.state == VrWorldState::Off && s.released == VrWorldRelease::KeyOff && !o.owned(),
          "key off: an owned route lets go at once and says why");
}

void machineCases() {
    VrWorldMachine m;
    check(m.state == VrWorldState::Off && !m.owned() && !m.wantsDraws(), "machine: starts Off");
    // The key comes on: the first frame is observed, a treated frame warms.
    m.frameEnd(frame(true, true, true, false));
    check(m.state == VrWorldState::Observing && m.wantsDraws() && !m.owned(), "machine: key on with no treated frame is Observing and wants draws");
    VrWorldStep s = m.frameEnd(frame(true, true, true, true));
    check(m.state == VrWorldState::Warming && !m.owned() && !s.entered, "machine: a treated frame makes it Warming");
    bool warm = true;
    for (unsigned i = 2; i < kVrWorldWarmFrames; ++i) {
        s = m.frameEnd(frame(true, true, true, true));
        warm = warm && m.state == VrWorldState::Warming && !s.entered;
    }
    check(warm, "machine: still Warming one treated frame short of the warm-up");
    s = m.frameEnd(frame(true, true, true, true));
    check(m.state == VrWorldState::Owned && m.owned() && s.entered && s.released == VrWorldRelease::None,
          "machine: the kVrWorldWarmFrames-th treated frame in a row makes it Owned, once");
    s = m.frameEnd(frame(true, true, true, true));
    check(m.owned() && !s.entered, "machine: an owned route that keeps treating stays owned and does not re-enter");

    // The warm-up is CONSECUTIVE frames: one refusal restarts it.
    VrWorldMachine w;
    run(w, kVrWorldWarmFrames - 1, frame(true, true, true, true));
    w.frameEnd(frame(true, true, true, false));
    check(w.state == VrWorldState::Observing, "machine: a refusal while warming drops to Observing");
    run(w, kVrWorldWarmFrames - 1, frame(true, true, true, true));
    check(!w.owned(), "machine: and the warm-up starts over (kVrWorldWarmFrames - 1 more treated frames is not enough)");
    w.frameEnd(frame(true, true, true, true));
    check(w.owned(), "machine: ... the next treated frame completes a fresh run");

    // The grace: a single refusal is not a release; kVrWorldGraceFrames in a row is.
    VrWorldMachine g = owned();
    bool graced = true;
    for (unsigned i = 0; i + 1 < kVrWorldGraceFrames; ++i) {
        s = g.frameEnd(frame(true, true, true, false));
        graced = graced && g.owned() && s.released == VrWorldRelease::None;
    }
    check(graced, "machine: fewer than kVrWorldGraceFrames untreated frames in a row keep the route owned");
    g.frameEnd(frame(true, true, true, true));
    for (unsigned i = 0; i + 1 < kVrWorldGraceFrames; ++i) g.frameEnd(frame(true, true, true, false));
    check(g.owned(), "machine: a treated frame in between resets the count of untreated ones");
    s = g.frameEnd(frame(true, true, true, false));
    check(!g.owned() && g.state == VrWorldState::Observing && s.released == VrWorldRelease::Declined,
          "machine: kVrWorldGraceFrames untreated frames in a row release it, with the reason frames-not-treated");

    // Releases by the gate, the layer and a scene reset, each immediate, each named; a warming route drops without an event.
    VrWorldMachine a = owned();
    s = a.frameEnd(frame(true, true, false, true));
    check(a.state == VrWorldState::Observing && s.released == VrWorldRelease::GateLost,
          "machine: the gate lost releases an owned route at once (on-foot-gate-lost)");
    VrWorldMachine b = owned();
    s = b.frameEnd(frame(true, false, true, true));
    check(b.state == VrWorldState::Observing && s.released == VrWorldRelease::LayerNotLive, "machine: the layer not live releases at once (layer-not-live)");
    VrWorldMachine c = owned();
    s = c.frameEnd(frame(true, true, true, true, false, true));
    check(c.state == VrWorldState::Observing && s.released == VrWorldRelease::SceneReset, "machine: a scene reset releases at once (scene-reset)");
    run(c, kVrWorldWarmFrames - 1, frame(true, true, true, true));
    check(!c.owned(), "machine: after a scene reset the warm-up starts over");
    VrWorldMachine d;
    run(d, 3, frame(true, true, true, true));
    s = d.frameEnd(frame(true, true, false, true));
    check(d.state == VrWorldState::Observing && s.released == VrWorldRelease::None, "machine: a warming route that loses the gate drops to Observing with no release event");

    // The latch: three treated frames with late writes turn the route off until the key is flipped.
    VrWorldMachine l = owned();
    s = l.frameEnd(frame(true, true, true, true, true));
    s = l.frameEnd(frame(true, true, true, true, true));
    check(l.owned(), "latch: two treated frames with late writes do not trip it");
    s = l.frameEnd(frame(true, true, true, true, true));
    check(l.state == VrWorldState::Latched && !l.owned() && !l.wantsDraws() && s.released == VrWorldRelease::Latched,
          "latch: the third treated frame with late writes latches the route, and an owned route says so");
    run(l, 100, frame(true, true, true, true));
    check(l.state == VrWorldState::Latched, "latch: it stays latched with the key on whatever the frames show");
    l.frameEnd(frame(false, true, true, true));
    check(l.state == VrWorldState::Off, "latch: the key off ends it");
    l.frameEnd(frame(true, true, true, false));
    check(l.state == VrWorldState::Observing, "latch: and the key back on starts the route afresh");
    VrWorldMachine scattered = owned();
    for (int i = 0; i < 3; ++i) { scattered.frameEnd(frame(true, true, true, true, true)); scattered.frameEnd(frame(true, true, true, true, false)); }
    check(scattered.state == VrWorldState::Latched, "latch: it counts frames with late writes, not a streak: three scattered ones trip it too");

    // The state names and release names are distinct words.
    bool distinct = true;
    const VrWorldState states[] = {VrWorldState::Off, VrWorldState::Observing, VrWorldState::Warming, VrWorldState::Owned, VrWorldState::Latched};
    for (size_t i = 0; i < 5; ++i) for (size_t j = i + 1; j < 5; ++j) distinct = distinct && std::strcmp(vrWorldStateName(states[i]), vrWorldStateName(states[j])) != 0;
    const VrWorldRelease rel[] = {VrWorldRelease::None, VrWorldRelease::KeyOff, VrWorldRelease::LayerNotLive, VrWorldRelease::GateLost,
                                  VrWorldRelease::Declined, VrWorldRelease::SceneReset, VrWorldRelease::Latched};
    for (size_t i = 0; i < 7; ++i) for (size_t j = i + 1; j < 7; ++j) distinct = distinct && std::strcmp(vrWorldReleaseName(rel[i]), vrWorldReleaseName(rel[j])) != 0;
    check(distinct, "names: every state and every release reason has its own name");
}

void predicateCases() {
    const VrWorldState all[] = {VrWorldState::Off, VrWorldState::Observing, VrWorldState::Warming, VrWorldState::Owned, VrWorldState::Latched};
    bool suppress = true, take = true;
    for (VrWorldState s : all) {
        suppress = suppress && vrWorldSuppressesEyeShift(s) == (s == VrWorldState::Owned);
        for (bool treated : {false, true}) take = take && vrWorldLayerMayTake(s, treated) == (s == VrWorldState::Owned && treated);
    }
    check(suppress, "eye shift: suppressed in exactly one state, Owned (the frame after ownership began, until it ends)");
    check(take, "layer: may take the screen draw only when the route is Owned AND treated this frame");
    check(!vrWorldDoorLayerOnly(0, 0) && !vrWorldDoorLayerOnly(0, 5) && !vrWorldDoorLayerOnly(4, 5) && vrWorldDoorLayerOnly(5, 5),
          "door: layer-only only for an eye whose screen draw the layer took in THIS sequence; sequence 0 is never");
}

// ---- 3. the selector -------------------------------------------------------------------------------------------------
VrWorldSelectFacts goodFacts() {
    VrWorldSelectFacts f;
    f.triggered = true; f.ambiguous = false; f.depthKnown = true; f.depthNamed = true; f.engineViews = true;
    f.cameraRows = true; f.extentOk = true; f.engineKnown = true;
    return f;
}
void selectorCases() {
    check(vrWorldSelect(goodFacts()) == VrWorldSelect::Selected, "selector: every fact good selects the frame");
    struct Case { const char* what; void (*mut)(VrWorldSelectFacts&); VrWorldSelect want; };
    const Case cases[] = {
        {"no trigger", [](VrWorldSelectFacts& f) { f.triggered = false; }, VrWorldSelect::NoTrigger},
        {"ambiguous H", [](VrWorldSelectFacts& f) { f.ambiguous = true; }, VrWorldSelect::Ambiguous},
        {"no engine", [](VrWorldSelectFacts& f) { f.engineKnown = false; }, VrWorldSelect::NoEngine},
        {"extent", [](VrWorldSelectFacts& f) { f.extentOk = false; }, VrWorldSelect::ExtentMismatch},
        {"depth mixed", [](VrWorldSelectFacts& f) { f.depthKnown = false; }, VrWorldSelect::DepthMixed},
        {"depth not named", [](VrWorldSelectFacts& f) { f.depthNamed = false; }, VrWorldSelect::DepthNotNamed},
        {"engine views", [](VrWorldSelectFacts& f) { f.engineViews = false; }, VrWorldSelect::NoEngineViews},
        {"camera rows", [](VrWorldSelectFacts& f) { f.cameraRows = false; }, VrWorldSelect::NoCameraRows},
    };
    for (const auto& c : cases) {
        VrWorldSelectFacts f = goodFacts();
        c.mut(f);
        char what[160];
        std::snprintf(what, sizeof(what), "selector: %s alone refuses the frame with its own reason", c.what);
        check(vrWorldSelect(f) == c.want, what);
    }
    // The order of the tests is the order of the reasons: for EVERY pair of facts false at once the earlier test's reason
    // answers (a swap of any two tests in the function changes one of these).
    bool orderOk = true;
    const size_t cn = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < cn; ++i)
        for (size_t j = i + 1; j < cn; ++j) {
            VrWorldSelectFacts f = goodFacts();
            cases[i].mut(f);
            cases[j].mut(f);
            orderOk = orderOk && vrWorldSelect(f) == cases[i].want;
        }
    check(orderOk, "selector: every pair of false facts reports the earlier test's reason (the order is the contract)");
    VrWorldSelectFacts none;
    check(vrWorldSelect(none) == VrWorldSelect::NoTrigger, "selector: with every fact false the first test answers (no trigger)");
    VrWorldSelectFacts two = goodFacts();
    two.depthNamed = false; two.cameraRows = false;
    check(vrWorldSelect(two) == VrWorldSelect::DepthNotNamed, "selector: two failures report the earlier test's reason");
    bool distinct = true;
    const VrWorldSelect names[] = {VrWorldSelect::Selected, VrWorldSelect::NoTrigger, VrWorldSelect::Ambiguous, VrWorldSelect::NoEngine,
                                   VrWorldSelect::ExtentMismatch, VrWorldSelect::DepthMixed, VrWorldSelect::DepthNotNamed,
                                   VrWorldSelect::NoEngineViews, VrWorldSelect::NoCameraRows};
    for (size_t i = 0; i < 9; ++i) for (size_t j = i + 1; j < 9; ++j) distinct = distinct && std::strcmp(vrWorldSelectName(names[i]), vrWorldSelectName(names[j])) != 0;
    check(distinct, "selector: every reason has its own name");
}

// ---- 4. the glue and the trigger on the retake chain ---------------------------------------------------------------------
// Resources (identity only). Sizes and formats as the census retake showed them (design doc section 82).
int g_res[32];
const void* R(int i) { return &g_res[i]; }
enum { RH = 1, RDEPTH, RG2, RG4, RCOPY, RFOG, RSMALL1, RSMALL2, RSMALL3, RTONE, RSCREEN, RHUDDEPTH, REYE0, REYE1, RH2, RDEPTH2, RBLOOM };
constexpr uint32_t W = 5040, Hh = 2835;
constexpr uint32_t F_H = 26, F_RGBA8 = 27, F_R10 = 23;

VrWorldView view(int res, uint32_t w, uint32_t h, uint32_t fmt, bool known = true) {
    VrWorldView v;
    v.resource = R(res); v.width = w; v.height = h; v.format = fmt; v.known = known; v.texture2d = true;
    return v;
}
struct Drawn { uint32_t q; int target; uint32_t tw, th, tf; int depth; uint32_t dw, dh; int srv[4]; uint64_t vs, ps; };
struct Replay {
    FlatHdrFrame f;
    FlatContractObservation k;
    uint32_t triggerQ = 0, triggers = 0;
    Replay() { flatHdrBeginFrame(f, W, Hh); }
    bool draw(const Drawn& d) {
        VrWorldView c = view(d.target, d.tw, d.th, d.tf);
        VrWorldView dv = d.depth ? view(d.depth, d.dw, d.dh, 39) : VrWorldView{};
        if (!vrWorldFillObservation(k, &c, d.depth ? &dv : nullptr, &c, d.depth ? &dv : nullptr, d.vs, d.ps)) return false;
        const void* srv[4] = {};
        bool known = false;
        if (!f.triggered && flatHdrCouldConsume(f, k)) {
            for (int i = 0; i < 4; ++i) srv[i] = d.srv[i] ? R(d.srv[i]) : nullptr;
            known = true;
        }
        const bool t = flatHdrObserveDraw(f, k, d.q, srv, known);
        if (t) { triggerQ = d.q; ++triggers; }
        return t;
    }
};
Drawn none() { Drawn d{}; return d; }
Drawn into(uint32_t q, int target, uint32_t w, uint32_t h, uint32_t fmt, int depth = 0) {
    Drawn d = none();
    d.q = q; d.target = target; d.tw = w; d.th = h; d.tf = fmt; d.depth = depth; d.dw = w; d.dh = h;
    return d;
}

// The retake chain, q numbers as the census numbered them. World draws into the G-buffer and H's draws carry the screen
// depth; the exposure reduction runs on a COPY of H (a copy, not a draw); the tone reads H at t1.
void chain(Replay& r, bool withTone = true, int toneDepth = 0, int toneTarget = RTONE, bool toneReadsH = true,
           uint32_t smallW = 630, uint32_t smallH = 354) {
    for (uint32_t q = 3794; q < 3800; ++q) r.draw(into(q, RG2, W, Hh, F_R10, RDEPTH));          // the world: G-buffer with the screen depth
    for (uint32_t q = 7846; q <= 7856; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));            // 11 deferred-light draws into H
    for (uint32_t q = 7859; q < 7870; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));             // sky, glass, particles, holograms
    // q 8157: CopySubresourceRegion(H -> RCOPY): not a draw, nothing reaches the detector.
    Drawn e = into(8158, RBLOOM, 1261, 709, F_RGBA8); e.srv[0] = RCOPY; r.draw(e);              // the exposure reduction reads the COPY
    e = into(8159, RFOG, 320, 180, F_RGBA8); e.srv[0] = RCOPY; r.draw(e);
    for (uint32_t q = 8162; q <= 8196; ++q) { Drawn l = into(q, RH, W, Hh, F_H, RDEPTH); l.srv[1] = RCOPY; r.draw(l); }   // 35 late draws
    for (uint32_t q = 8198; q <= 8200; ++q) { Drawn s = into(q, RSMALL1, smallW, smallH, F_RGBA8); s.srv[0] = RH; r.draw(s); }   // 630x354 reading H
    if (withTone) {
        Drawn t = into(8203, toneTarget, W, Hh, F_RGBA8, toneDepth);                              // THE TONE: no depth, not H, reads H at t1
        t.srv[0] = RFOG; t.srv[1] = toneReadsH ? RH : RCOPY;
        t.vs = 0xF9CFC798F21E9AEAull; t.ps = 0xFEE777E92850B390ull;
        r.draw(t);
    }
}
void afterTone(Replay& r) {
    Drawn c = into(8204, RSCREEN, W, Hh, F_RGBA8); c.srv[0] = RTONE; r.draw(c);                   // the game copy into the screen texture
    for (uint32_t q = 8205; q <= 8211; ++q) { Drawn h = into(q, RSCREEN, W, Hh, F_RGBA8, RHUDDEPTH); r.draw(h); }   // the HUD, own depth
    Drawn e0 = into(8213, REYE0, 2620, 2533, F_RGBA8); e0.srv[0] = RSCREEN; r.draw(e0);           // the eye composites
    Drawn e1 = into(8220, REYE1, 2620, 2533, F_RGBA8); e1.srv[0] = RSCREEN; r.draw(e1);
}

void glueCases() {
    FlatContractObservation k;
    VrWorldView c = view(RH, W, Hh, F_H), d = view(RDEPTH, W, Hh, 39);
    check(vrWorldFillObservation(k, &c, &d, R(100), R(101), 7, 8) && k.color == R(RH) && k.depth == R(RDEPTH) && k.dsv == R(101) &&
              k.rtv == R(100) && k.width == W && k.height == Hh && k.format == F_H && k.depthWidth == W && k.depthHeight == Hh && k.vs == 7 && k.ps == 8,
          "glue: a coloured draw with a readable depth fills every field the detector reads");
    VrWorldView unknown = view(RH, W, Hh, F_H, false);
    check(!vrWorldFillObservation(k, &unknown, nullptr, nullptr, nullptr, 0, 0) && !vrWorldFillObservation(k, nullptr, nullptr, nullptr, nullptr, 0, 0),
          "glue: a colour target the shadow could not resolve is ignored");
    VrWorldView buffer = c; buffer.texture2d = false;
    check(!vrWorldFillObservation(k, &buffer, nullptr, nullptr, nullptr, 0, 0), "glue: a colour target that is not a 2D texture is ignored");
    VrWorldView badDepth = view(RDEPTH, W, Hh, 39, false);
    FlatContractObservation k2;
    check(vrWorldFillObservation(k2, &c, &badDepth, R(100), R(101), 0, 0) && k2.depth == nullptr && k2.dsv == R(101),
          "glue: a depth target the shadow could not read still counts as a bound depth (dsv set, resource null)");
    FlatHdrFrame f;
    flatHdrBeginFrame(f, W, Hh);
    FlatContractObservation h;
    VrWorldView hv = view(RH, W, Hh, F_H), dv = view(RDEPTH, W, Hh, 39);
    vrWorldFillObservation(h, &hv, &dv, R(100), R(101), 0, 0);
    flatHdrObserveDraw(f, h, 1, nullptr, false);
    FlatContractObservation tone;
    VrWorldView tv = view(RTONE, W, Hh, F_RGBA8);
    vrWorldFillObservation(tone, &tv, &badDepth, R(102), R(103), 0, 0);
    check(!flatHdrCouldConsume(f, tone), "glue: rule (i) is 'no depth bound': an unreadable bound depth is not 'none', so it cannot be the consumer");
}

void triggerCases() {
    {
        Replay r;
        chain(r);
        check(r.triggers == 1 && r.triggerQ == 8203, "trigger: on the retake chain the trigger is q 8203, the tone, once");
        check(r.f.triggered && r.f.trigger.hdr == R(RH) && r.f.trigger.srvSlot == 1 && r.f.trigger.srvKnown && !r.f.trigger.ambiguous,
              "trigger: H is the scene target and the tone reads it at t1, unambiguously");
        check(r.f.trigger.targetWidth == W && r.f.trigger.targetHeight == Hh && r.f.trigger.targetFormat == F_RGBA8 &&
                  r.f.trigger.vs == 0xF9CFC798F21E9AEAull && r.f.trigger.ps == 0xFEE777E92850B390ull,
              "trigger: the consumer is the tone pair's 5040x2835 RGBA8 draw (VS F9CFC798, PS FEE777E9)");
        check(r.f.candidateCount == 1 && r.f.candidates[0].resource == R(RH) && !r.f.candidateOverflow,
              "trigger: H is the only candidate: the copy of H the exposure chain reads is never drawn into");
        afterTone(r);
        check(flatHdrLateWrites(r.f) == 0 && r.triggers == 1,
              "trigger: nothing writes H after the tone on this chain (the game copy and the HUD write the screen texture; the eye composites read it)");
    }
    {   // rule (iv): the 630x354 draws that read H (q 8198..8200) are an eighth per axis and are not the consumer.
        Replay r;
        chain(r, false);
        check(r.triggers == 0 && !r.f.triggered, "trigger: rule (iv): without the tone the three 630x354 draws that read H do not trigger");
    }
    {   // rule (iv) mutated: a half-size target reading H IS accepted (the rule is the extent, not the q).
        Replay r;
        chain(r, false, 0, RTONE, true, 2520, 1418);
        check(r.triggers == 1 && r.triggerQ == 8198, "trigger: a half-size target reading H (rule iv's edge) triggers at q 8198");
    }
    {   // rule (i): a depth bound on the tone
        Replay r;
        chain(r, true, RHUDDEPTH);
        check(r.triggers == 0, "trigger: rule (i): a tone with a depth target bound is not the consumer");
    }
    {   // rule (ii): the target is H itself
        Replay r;
        chain(r, true, 0, RH);
        check(r.triggers == 0, "trigger: rule (ii): a draw whose target is H is not its consumer (it is a write)");
    }
    {   // rule (iii): H not bound as a pixel-shader resource
        Replay r;
        chain(r, true, 0, RTONE, false);
        check(r.triggers == 0, "trigger: rule (iii): a tone that does not read H (it reads the copy) does not trigger");
    }
    {   // a frame with no H at all (a 3D map, a menu): nothing to trigger
        Replay r;
        for (uint32_t q = 1; q < 50; ++q) r.draw(into(q, RG2, W, Hh, F_R10, RDEPTH));
        Drawn t = into(60, RTONE, W, Hh, F_RGBA8); t.srv[1] = RCOPY; r.draw(t);
        check(r.triggers == 0 && flatHdrFrameVerdict(r.f) == FlatHdrFrameVerdict::NoHdr, "trigger: a frame with no HDR scene target has no trigger (NoHdr)");
    }
    {   // a second H candidate read by the consumer: ambiguous, never guessed
        Replay r;
        for (uint32_t q = 10; q < 13; ++q) { r.draw(into(q, RH, W, Hh, F_H, RDEPTH)); r.draw(into(q + 100, RH2, W, Hh, F_H, RDEPTH2)); }
        Drawn t = into(200, RTONE, W, Hh, F_RGBA8); t.srv[0] = RH; t.srv[1] = RH2; r.draw(t);
        check(r.triggers == 1 && r.f.trigger.ambiguous, "trigger: two H candidates both bound at the consumer: ambiguous (the selector refuses it)");
    }
    {   // the eye composites alone (an eye-sized target) never trigger, with or without an H
        Replay r;
        for (uint32_t q = 10; q < 13; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));
        Drawn e0 = into(300, REYE0, 2620, 2533, F_RGBA8); e0.srv[0] = RSCREEN; r.draw(e0);
        check(r.triggers == 0, "trigger: an eye composite that reads the screen texture, not H, is not a consumer");
    }
    {   // late writes after the trigger: a draw, an explicit write and a dispatch into H count; writes elsewhere do not
        Replay r;
        chain(r);
        r.draw(into(8300, RH, W, Hh, F_H, RDEPTH));
        flatHdrObserveExplicitWrite(r.f, R(RH));
        flatHdrObserveDispatchWrite(r.f, R(RH));
        flatHdrObserveExplicitWrite(r.f, R(RCOPY));
        flatHdrObserveDispatchWrite(r.f, R(RSCREEN));
        check(r.f.lateDraws == 1 && r.f.lateExplicit == 1 && r.f.lateDispatches == 1 && flatHdrLateWrites(r.f) == 3,
              "late writes: a draw, a copy/clear and a UAV write into H after the tone count; writes into the copy and the screen texture do not");
    }
    {   // the copy of H mid-frame is normal: a detector that counted the copy's source as a consumer would fire before the late draws
        Replay r;
        for (uint32_t q = 7846; q <= 7856; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));
        Drawn e = into(8158, RBLOOM, 1261, 709, F_RGBA8); e.srv[0] = RCOPY; r.draw(e);
        check(r.triggers == 0, "trigger: the exposure draws that read the COPY of H (and run before the 35 late draws) do not trigger");
    }
}

// ---- 5. the lines -------------------------------------------------------------------------------------------------------
void lineCases() {
    VrWorldWindow w;
    char line[1024];
    int n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Observing, true, true, w);
    check(n > 0 && n < 1024 && std::strstr(line, "vr world route 5s: key=auto state=observing layer=live gate=held frames=0") &&
              std::strstr(line, "selection=none"),
          "line: the 5 s line prints zeros and 'selection=none' when nothing happened: an absent line is what 'never ran' looks like");
    w.hdr.frames = 450; w.gateFrames = 450; w.hdr.hdrFrames = 450; w.hdr.triggerFrames = 448; w.hdr.noTriggerFrames = 2;
    w.hdr.treated = 440; w.hdr.declined = 8; w.ownedFrames = 300; w.takes = 600; w.layerOnly = 600; w.enters = 1; w.releases = 1; w.gateFlips = 2;
    w.lastRelease = "on-foot-gate-lost"; w.resets = 2; w.hdr.lastVerdict = "treated";
    w.hdr.noteSelection("selected"); w.hdr.noteSelection("engine-views-unavailable");
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && n < 1024 && std::strstr(line, "state=owned") && std::strstr(line, "gate-flips=2") && std::strstr(line, "treated=440 declined=8") &&
              std::strstr(line, "eye-takes=600 door-layer-only=600") && std::strstr(line, "releases=1 (last=on-foot-gate-lost)") &&
              std::strstr(line, "selection=selected:1,engine-views-unavailable:1"),
          "line: state, treated, declined, takes, layer-only doors, releases with their reason and the selector's tally are all there");
    // Worst realistic case: a 5 s window is at most a few hundred frames at 90 Hz (a thousand with a generous margin), two
    // takes a frame; the widths are at their widest, every selection slot full, the longest reason names.
    VrWorldWindow big;
    big.hdr.frames = big.hdr.hdrFrames = big.hdr.triggerFrames = big.hdr.noTriggerFrames = big.hdr.ambiguousFrames = 99999;
    big.hdr.treated = big.hdr.declined = big.hdr.lateWrites = big.hdr.lateWriteFrames = 99999;
    big.gateFrames = big.gateFlips = big.ownedFrames = big.takes = big.layerOnly = big.enters = big.releases = big.resets = 99999;
    big.lastRelease = "late-writes-latched"; big.hdr.lastVerdict = "actual-hdr-binding-or-depth-view-refused";
    big.jitter = "no-hook"; big.phaseX = big.rowsX = -0.4999f; big.phaseY = big.rowsY = -0.4999f;
    big.foldMode[0] = big.foldMode[1] = big.foldMode[2] = 99999;
    std::snprintf(big.curve, sizeof(big.curve), "1.000/256/99999.999"); big.curveReissues = 99999;   // the curve's tokens at their widest
    big.hdr.lastTriggerVs = big.hdr.lastTriggerPs = ~0ull;
    big.hdr.lastHdrWidth = big.hdr.lastHdrHeight = big.hdr.lastTargetWidth = big.hdr.lastTargetHeight = ~0u;
    const char* names[] = {"selected", "hdr-depth-not-single", "depth-not-screen-motion-source", "engine-views-unavailable",
                           "camera-rows-unavailable", "hdr-not-screen-sized"};
    for (const char* s : names) for (int i = 0; i < 3; ++i) big.hdr.noteSelection(s);
    big.hdr.noteSelection("ambiguous-hdr");
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Latched, false, false, big);
    std::printf("        (the worst realistic 5 s line is %d characters; the log keeps about 1166)\n", n);
    check(n > 0 && n < 1100, "line: at its worst the 5 s line fits the log's line (about 1166 characters) and is never truncated");
    check(std::strstr(line, "other:1") != nullptr, "line: a selection beyond the table's six falls into 'other'");
    // Stage 2's fields: the jitter decision, the phase the frame that ended was given and the rows it carried, the fold-in's modes.
    w.jitter = "on"; w.phaseX = 0.4375f; w.phaseY = -0.3125f; w.rowsX = 0.4375f; w.rowsY = -0.3125f;
    w.foldMode[0] = 4; w.foldMode[1] = 300; w.foldMode[2] = 2;
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && n < 1100 && std::strstr(line, "jitter=on phase=0.4375,-0.3125 rows=0.4375,-0.3125 fp-mode=4/300/2"),
          "line: the 5 s line carries the jitter decision, the world phase in use, the rows' phase and the fold-in's modes");
    // The phase count (2026-10-01; temporal_math.h): the route resolves at the game's render size, so it runs the fixed eight, and its
    // 5 s line says so before the jitter decision -- a window nothing has set prints eight, one handed another count prints that one.
    check(n > 0 && std::strstr(line, " phases=8 jitter=on phase=0.4375,-0.3125 "),
          "line: the 5 s line carries the phase count the route ran, eight by default, just before the jitter decision");
    w.phases = 32;
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && n < 1100 && std::strstr(line, " phases=32 jitter=on phase=0.4375,-0.3125 "),
          "line: ...and prints the window's own count, not a constant");
    VrWorldWindow idle;
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Observing, true, true, idle);
    check(n > 0 && std::strstr(line, "jitter=idle phase=0.0000,0.0000 rows=0.0000,0.0000 fp-mode=0/0/0"),
          "line: a route that does not jitter prints jitter=idle and a zero phase (an absent token would read as 'never built')");
    check(n > 0 && std::strstr(line, " phases=8 jitter=idle "), "line: ...and the fixed eight (a default window says what every path has always run)");
    char buf[512];
    n = vrWorldFormatEntered(buf, sizeof(buf), 1234, kVrWorldWarmFrames);
    check(n > 0 && std::strstr(buf, "OWNS the world from frame=1234") && std::strstr(buf, "8 treated frames"), "line: the ownership line names the frame and the warm-up");
    n = vrWorldFormatReleased(buf, sizeof(buf), 99, VrWorldRelease::SceneReset, 77);
    check(n > 0 && std::strstr(buf, "RELEASED the world at frame=99 (scene-reset) after 77 owned frame(s)"), "line: the release line names the frame, the reason and the owned frames");
    check(std::strstr(buf, "last decline") == nullptr, "line: a release that followed no decline names none (a gate lost or a scene reset is not a decline)");
    Replay r;
    chain(r);
    n = vrWorldFormatFirstTrigger(buf, sizeof(buf), 5, r.f);
    check(n > 0 && std::strstr(buf, "first trigger at frame=5 seq=8203") && std::strstr(buf, "target=5040x2835 fmt=27") && std::strstr(buf, "at t1"),
          "line: the first trigger line names the tone draw, its target and the slot it reads H at");
}

// ---- 5b. the curved screen in the route's lines ---------------------------------------------------------------------------
// With fix.panel_curvature above 0 the route re-issues the screen through the game's own strip (panel_curve.h), and its lines say which
// curve: the 5 s line's curve= and curve-reissues= (the window's strips), and a sentence on the OWNS line. The reader parses the 5 s
// tokens, so these texts are the contract; with the curve off every line is what it was.
void curveCases() {
    char t[64];
    // vrWorldFormatCurve: off, pending, stood-down, or curvature/columns/gain.
    int n = vrWorldFormatCurve(t, sizeof(t), false, false, false, 0.0f, 64, 0.0f);
    check(n == 3 && std::strcmp(t, "off") == 0, "curve: a screen nothing curves (curvature 0, the default column count) reads off");
    n = vrWorldFormatCurve(t, sizeof(t), true, false, false, 0.3f, 64, 0.0f);
    check(n == 7 && std::strcmp(t, "pending") == 0, "curve: asked for and no strip drawn yet (the substitution is still learning the panel's SIZE) reads pending");
    n = vrWorldFormatCurve(t, sizeof(t), true, true, false, 0.3f, 64, 0.0f);
    check(n == 10 && std::strcmp(t, "stood-down") == 0, "curve: a substitution that stood itself down for the session reads stood-down");
    n = vrWorldFormatCurve(t, sizeof(t), true, false, true, 0.3f, 64, 35.556f);
    check(n == 15 && std::strcmp(t, "0.300/64/35.556") == 0, "curve: a strip in hand reads curvature/columns/gain, three decimals each (0.300/64/35.556)");
    n = vrWorldFormatCurve(t, sizeof(t), true, false, true, 0.05f, 256, 1234.5678f);
    check(n > 0 && std::strcmp(t, "0.050/256/1234.568") == 0, "curve: the columns print as an integer and the gain rounds to three decimals (0.050/256/1234.568)");
    // The states have an order: not asked for beats everything, a stand-down beats ready and pending.
    vrWorldFormatCurve(t, sizeof(t), false, true, true, 0.3f, 64, 35.556f);
    check(std::strcmp(t, "off") == 0, "curve: not asked for reads off whatever else is set (a stood-down flag left over from nothing says nothing)");
    vrWorldFormatCurve(t, sizeof(t), true, true, true, 0.3f, 64, 35.556f);
    check(std::strcmp(t, "stood-down") == 0, "curve: a stood-down substitution reads stood-down even with a strip in hand");
    // The identity test's column count alone (curvature 0, 64 columns changed) is a curve that is asked for.
    vrWorldFormatCurve(t, sizeof(t), true, false, true, 0.0f, 1, 20.0f);
    check(std::strcmp(t, "0.000/1/20.000") == 0, "curve: asked for by the column count alone, curvature 0, still prints its numbers");
    vrWorldFormatCurve(t, sizeof(t), true, false, true, 1.0f, 256, 999999.999f);
    check(std::strlen(t) < sizeof(VrWorldWindow::curve), "curve: the widest text fits the window's field");

    // The window: a default window says off and 0; reset() goes back to it; the 5 s line carries both tokens right after steady-detail.
    VrWorldWindow w;
    check(std::strcmp(w.curve, "off") == 0 && w.curveReissues == 0, "curve: a default window says curve off and 0 strips (the boundary sets both before every line)");
    char line[1200];
    w.steady = "on";
    std::snprintf(w.curve, sizeof(w.curve), "0.300/64/35.556");
    w.curveReissues = 1234;
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && std::strstr(line, " fp-mode=0/0/0 steady-detail=on curve=0.300/64/35.556 curve-reissues=1234 last-trigger=VS="),
          "curve: the 5 s line carries curve=<text> curve-reissues=<n> immediately after steady-detail=<on|off> and before last-trigger");
    w.steady = "off";
    std::snprintf(w.curve, sizeof(w.curve), "pending");
    w.curveReissues = 0;
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && std::strstr(line, " steady-detail=off curve=pending curve-reissues=0 last-trigger="), "curve: pending and zero strips read as such");
    std::snprintf(w.curve, sizeof(w.curve), "stood-down");
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && std::strstr(line, " steady-detail=off curve=stood-down curve-reissues=0 last-trigger="), "curve: stood-down reads as such");
    w.curveReissues = 77;
    w.reset();
    check(std::strcmp(w.curve, "off") == 0 && w.curveReissues == 0, "curve: reset() takes a window back to curve off and 0 strips (each 5 s window counts its own)");

    // The OWNS line: with no curve (null, or off) it is what it was, byte for byte; with one, it adds ONE sentence that says what is true of the
    // screen in that state. A strip in hand: the layer draws the very strip the game's own draw is substituted with. Pending (no strip built
    // yet) and stood-down (the substitution is off for the session): the flat quad, drawn by the game and re-issued flat by the layer.
    const std::string flat = "vr world route: OWNS the world from frame=1234 after 8 treated frames in a row; the eye shift is off and the layer takes "
                             "the screen draw on every frame the route treats (the eye route serves the rest)";
    char buf[512];
    n = vrWorldFormatEntered(buf, sizeof(buf), 1234, 8);
    check(flat == buf && n == static_cast<int>(flat.size()), "curve: the OWNS line without a curve argument is the old text, byte for byte");
    n = vrWorldFormatEntered(buf, sizeof(buf), 1234, 8, nullptr);
    check(flat == buf && n == static_cast<int>(flat.size()), "curve: ... and with a null curve");
    n = vrWorldFormatEntered(buf, sizeof(buf), 1234, 8, "off");
    check(flat == buf && n == static_cast<int>(flat.size()), "curve: ... and with curve=off (a flat screen's line is unchanged)");
    const std::string stripHead = "; the screen is curved (curve=";
    const std::string stripTail = "): the layer draws the same strip the game's own draw is substituted with, so the bend and the placement are the game's";
    const std::string stripSentence = stripHead + "0.300/64/35.556" + stripTail;
    const std::string pendingSentence = "; the screen is set to curve (curve=pending): the strip is not built yet, so the game and the layer both draw the flat quad until it is";
    const std::string stoodDownSentence = "; the screen is set to curve but the curve stood down (curve=stood-down): the game draws its own flat quad and the layer re-issues it flat";
    // One pin per state: the line is the flat line plus exactly that state's sentence, byte for byte.
    const auto ownsIs = [&](const char* curveText, const std::string& sentence) {
        char b[512];
        const int len = vrWorldFormatEntered(b, sizeof(b), 1234, 8, curveText);
        return flat + sentence == b && len == static_cast<int>(flat.size() + sentence.size());
    };
    check(ownsIs("0.300/64/35.556", stripSentence),
          "curve: a strip in hand (0.300/64/35.556): the OWNS line is the flat line plus the sentence that the layer draws the game's own strip, byte for byte");
    check(ownsIs("pending", pendingSentence),
          "curve: pending: the OWNS line is the flat line plus the sentence that the strip is not built yet and the game and the layer both draw the flat quad, byte for byte");
    check(ownsIs("stood-down", stoodDownSentence),
          "curve: stood-down: the OWNS line is the flat line plus the sentence that the curve stood down, the game draws its own flat quad and the layer re-issues it flat, byte for byte");
    {   // Controls: each pin rejects the other two states' sentences, so a formatter that gave pending the strip sentence, or stood-down the pending
        // one, would fail it.
        const char* texts[3] = {"0.300/64/35.556", "pending", "stood-down"};
        const std::string* sentences[3] = {&stripSentence, &pendingSentence, &stoodDownSentence};
        const char* names[3] = {"strip", "pending", "stood-down"};
        for (int s = 0; s < 3; ++s) {
            bool rejects = true;
            for (int o = 0; o < 3; ++o)
                if (o != s) rejects = rejects && !ownsIs(texts[s], *sentences[o]);
            char label[200];
            std::snprintf(label, sizeof(label), "control: the %s pin rejects the other two states' sentences (a formatter that swapped them would fail it)", names[s]);
            check(rejects, label);
        }
    }
    {   // Every state keeps "(curve=<text>)" once: the part a reader keys on.
        bool keyed = true;
        const char* texts[3] = {"0.300/64/35.556", "pending", "stood-down"};
        for (const char* text : texts) {
            vrWorldFormatEntered(buf, sizeof(buf), 1234, 8, text);
            const std::string owns(buf);
            const size_t first = owns.find("(curve=");
            const size_t second = first == std::string::npos ? first : owns.find("(curve=", first + 1);
            keyed = keyed && first != std::string::npos && second == std::string::npos &&
                    owns.find(std::string("(curve=") + text + ")") != std::string::npos;
        }
        check(keyed, "curve: every state's sentence carries (curve=<text>) exactly once, the part a reader keys on");
    }
    {   // The strip sentence with the widest curve text a window holds is whole: the suffix buffer does not cut it.
        const std::string wide(sizeof(VrWorldWindow::curve) - 1, '7');
        const int len = vrWorldFormatEntered(buf, sizeof(buf), 1234, 8, wide.c_str());
        const std::string want = flat + stripHead + wide + stripTail;
        check(want == buf && len == static_cast<int>(want.size()),
              "curve: the strip sentence with the widest curve text a window holds (47 characters) is whole: the suffix buffer does not cut it");
    }
    // The route's buffer for this line is 384 bytes (vr_world_route.cpp): the widest realistic line of each state, a ten-digit frame number (over
    // a year at 90 Hz) and a wide curve text, fits with room to spare, and the end of its sentence is not cut.
    {
        char route[384];
        bool fits = true;
        for (const char* text : {"1.000/256/1234.567", "pending", "stood-down"}) {
            n = vrWorldFormatEntered(route, sizeof(route), 4294967295ull, kVrWorldWarmFrames, text);
            fits = fits && n > 0 && n < static_cast<int>(sizeof(route)) - 8 && static_cast<size_t>(n) == std::strlen(route);
        }
        check(fits, "curve: the OWNS line of every state, with a ten-digit frame and a wide curve, fits the route's 384-byte buffer (8 bytes to spare) uncut");
    }
}


// ---- 7. stage 2, the pure half ---------------------------------------------------------------------------------------------
void stage2Cases() {
    // The A/B key. Present-but-not-"on" is off, so a typo can never start writing the game's cameras.
    check(vrWorldJitterKeyFromText("on") == VrWorldJitterKey::On && vrWorldJitterKeyFromText("ON") == VrWorldJitterKey::On &&
              vrWorldJitterKeyFromText("On") == VrWorldJitterKey::On,
          "jitter key: on, in any case, reads on");
    const char* notOn[] = {"off", "", "auto", "onx", "o", " on", "on ", "true", "1", "yes", "dlss", "of"};
    bool allOff = true;
    for (const char* t : notOn) allOff = allOff && vrWorldJitterKeyFromText(t) == VrWorldJitterKey::Off;
    check(allOff && vrWorldJitterKeyFromText(nullptr) == VrWorldJitterKey::Off,
          "jitter key: anything that is not exactly on, a typo included, reads off: a typo can never start writing the game's cameras");
    check(std::strcmp(vrWorldJitterKeyName(VrWorldJitterKey::On), "on") == 0 && std::strcmp(vrWorldJitterKeyName(VrWorldJitterKey::Off), "off") == 0,
          "jitter key: the names are the ini's words");

    // The decision table: the reasons in order, each alone. Only On asks the injector to write a camera. `named`: the frame that
    // just ended named the screen's source (the window's rule: the scene camera refreshes before the draws that name the source, so the frame that starts
    // cannot be asked, and a map frame refreshes about thirty kind-3 cameras that must never pick up the world's phase).
    using J = VrWorldJitter;
    const auto decide = [](bool auto_, bool keyOn, bool global, bool wants, bool named, bool hook, bool fault) {
        return vrWorldJitterDecide(auto_, keyOn ? VrWorldJitterKey::On : VrWorldJitterKey::Off, global, wants, named, hook, fault);
    };
    check(decide(true, true, true, true, true, true, false) == J::On,
          "jitter: route auto, key on, global on, route wants, last frame named, hook live, no fault: On");
    check(decide(false, true, true, true, true, true, false) == J::Idle && decide(false, true, true, true, true, true, true) == J::Idle,
          "jitter: the route key not auto is Idle, a fault included (nothing is asked of the injector with the route off)");
    check(decide(true, true, true, true, true, true, true) == J::Fault, "jitter: a fault (a STOP) is Fault whatever else holds");
    check(decide(true, false, true, true, true, true, false) == J::KeyOff,
          "A/B: experimental.temporal_aa_on_foot_world_jitter off is KeyOff with everything else holding: the route stays, the phase is zero");
    check(decide(true, true, false, true, true, true, false) == J::GlobalOff,
          "jitter: experimental.temporal_aa_jitter off is GlobalOff (one key that stops every jitter the profile would apply)");
    check(decide(true, true, true, false, true, true, false) == J::Idle,
          "jitter: a route that is not Warming or Owned (or not watching) is Idle: nothing is written");
    check(decide(true, true, true, true, true, false, false) == J::NoHook,
          "jitter: a camera hook that is not live is NoHook: the world stays unjittered and the line says so");
    check(decide(true, true, true, true, false, true, false) == J::Unnamed,
          "NAMING: a route that is Warming or Owned after a frame that named no source for the screen (a map, a menu, a transition) is Unnamed: the window stays shut");
    check(decide(true, true, true, true, false, false, false) == J::Unnamed,
          "NAMING: the naming rule ranks above the hook: an unnamed frame is shut whether or not the hook is live");
    check(decide(true, true, true, false, false, true, false) == J::Idle,
          "NAMING: a route that is neither Warming nor Owned is Idle, named or not (the rule only shuts a window the route would have opened)");
    check(decide(true, false, true, true, false, true, false) == J::KeyOff && decide(true, true, false, true, false, true, false) == J::GlobalOff &&
              decide(true, true, true, true, false, true, true) == J::Fault,
          "NAMING: the key, the global key and a fault keep their own reasons on an unnamed frame");
    bool onlyOnInjects = true, namedOrShut = true;
    for (int mask = 0; mask < 128; ++mask) {
        const bool a = mask & 1, k = mask & 2, g = mask & 4, w = mask & 8, n = mask & 16, h = mask & 32, f = mask & 64;
        const J d = decide(a, k, g, w, n, h, f);
        onlyOnInjects = onlyOnInjects && (d == J::On) == (a && k && g && w && n && h && !f);
        // The invariant the flight plan's STOP reads: no combination in which the last frame named nothing ever decides On.
        namedOrShut = namedOrShut && (n || d != J::On);
    }
    check(onlyOnInjects, "jitter: over all 128 combinations the decision is On exactly when every condition holds, the last frame named its source and no fault is set");
    check(namedOrShut, "NAMING: over all 128 combinations a frame after an unnamed one is never On (no combination of keys, route state or hook opens the window)");
    check(std::strcmp(vrWorldJitterName(J::On), "on") == 0 && std::strcmp(vrWorldJitterName(J::KeyOff), "off") == 0 &&
              std::strcmp(vrWorldJitterName(J::GlobalOff), "off") == 0 && std::strcmp(vrWorldJitterName(J::Idle), "idle") == 0 &&
              std::strcmp(vrWorldJitterName(J::Unnamed), "unnamed") == 0 &&
              std::strcmp(vrWorldJitterName(J::NoHook), "no-hook") == 0 && std::strcmp(vrWorldJitterName(J::Fault), "fault") == 0,
          "jitter: the tokens of the 5 s line are on, off, idle, unnamed, no-hook and fault");

    // The weapon fold-in's mode (decision (a): the first-person camera takes the world's phase).
    const VrWorldAppliedPhase z, p{0.25f, -0.125f}, q{-0.375f, 0.0625f};
    check(vrWorldFirstPersonMode(z, z, z, z) == 0, "fold-in: no phase anywhere in either frame (the world is unjittered): mode 0, the map as given");
    check(vrWorldFirstPersonMode(p, q, p, q) == 1, "fold-in: the first-person camera carried the world's phase in both frames: mode 1, the phase term is added");
    check(vrWorldFirstPersonMode(p, z, p, z) == 1, "fold-in: the first jittered frame (last frame unjittered, both cameras agree): mode 1");
    check(vrWorldFirstPersonMode(z, q, z, q) == 1, "fold-in: the last jittered frame (this frame unjittered, both cameras agree): mode 1");
    check(vrWorldFirstPersonMode(p, q, z, q) == 2 && vrWorldFirstPersonMode(p, q, p, z) == 2 && vrWorldFirstPersonMode(p, q, q, p) == 2,
          "fold-in: a first-person camera that did not carry the world's phase in either frame rejects the attached pixels (mode 2), it is never guessed");
    check(vrWorldFirstPersonMode(z, z, p, z) == 2 && vrWorldFirstPersonMode(z, z, z, p) == 2,
          "fold-in: a first-person phase with no world phase (a camera the route did not write) is mode 2, not 0");
    check(vrWorldFirstPersonMode(p, q, p, p) == 2, "fold-in: a first-person camera that carried THIS frame's phase last frame too is not the world's last frame: mode 2");

    // The injector counters' window.
    VrWorldInjectWindow win;
    VrWorldInjectFrame f1;
    f1.sceneInjected = 48; f1.firstPersonInjected = 6; f1.warming = 1; f1.auxiliary = 2; f1.afterTrigger = 3; f1.unsupported = 12;
    f1.otherKind = 1; f1.unreadable = 0; f1.writeFailures = 0; f1.offThread = 0; f1.sceneRefused = 1; f1.firstPersonRefused = 2;
    f1.injectedKind[3] = 54;
    vrWorldAddInjectFrame(win, f1);
    vrWorldAddInjectFrame(win, f1);
    check(win.scene == 96 && win.firstPerson == 12 && win.refused == 6 && win.warming == 2 && win.auxiliary == 4 && win.afterTrigger == 6 &&
              win.unsupported == 24 && win.otherKind == 2 && win.injectedKind[3] == 108,
          "inject window: two frames' counters add (refused is the scene's and the first-person's together)");
    check(win.fovNarrowest == 0.0f && win.fovWidest == 0.0f,
          "inject window: frames whose calls read no screen view's frustum (the fields stay NaN) leave the window's field-of-view range empty (0, 0)");
    {   // The stage 2 experiment build: the struct's narrowest and widest field of view (rad) any screen view showed in the window.
        VrWorldInjectWindow w;
        VrWorldInjectFrame weapon, plain, zoom;
        weapon.fovNarrowest = 0.8203f; weapon.fovWidest = 0.9831f;   // a frame with the weapon drawn: flight 2's two fields of view
        plain.fovNarrowest = 0.9831f; plain.fovWidest = 0.9831f;     // a frame with the hands empty: one
        zoom.fovNarrowest = 0.7f; zoom.fovWidest = 1.2f;
        vrWorldAddInjectFrame(w, weapon);
        check(w.fovNarrowest == 0.8203f && w.fovWidest == 0.9831f, "fov range: the first frame that read a frustum sets both ends of the window's range");
        vrWorldAddInjectFrame(w, plain);
        vrWorldAddInjectFrame(w, VrWorldInjectFrame{});
        check(w.fovNarrowest == 0.8203f && w.fovWidest == 0.9831f,
              "fov range: a frame inside the range, and a frame that read no frustum, change nothing (a window of weapon and empty-handed frames keeps the weapon's field of view)");
        vrWorldAddInjectFrame(w, zoom);
        check(w.fovNarrowest == 0.7f && w.fovWidest == 1.2f, "fov range: a frame beyond either end widens the window's range to it");
        VrWorldInjectWindow plainOnly;
        vrWorldAddInjectFrame(plainOnly, plain);
        vrWorldAddInjectFrame(plainOnly, plain);
        check(plainOnly.fovNarrowest == plainOnly.fovWidest && plainOnly.fovWidest == 0.9831f,
              "fov range: a window with the hands empty has one field of view, narrowest == widest (so a weapon that is drawn shows as a second one)");
        VrWorldInjectFrame bogus;
        bogus.fovNarrowest = 0.0f; bogus.fovWidest = -1.0f;
        VrWorldInjectWindow w2;
        vrWorldAddInjectFrame(w2, bogus);
        bogus.fovNarrowest = 0.5f; bogus.fovWidest = std::numeric_limits<float>::quiet_NaN();
        vrWorldAddInjectFrame(w2, bogus);
        check(w2.fovNarrowest == 0.0f && w2.fovWidest == 0.0f, "fov range: a zero, a negative or a NaN end is no reading and never enters the window's range");
        char fovLine[1100];
        int fovN = vrWorldFormatInjectWindow(fovLine, sizeof(fovLine), w);
        const char* fovTail = " fov=0.7000..1.2000";
        const size_t fovTailLen = std::strlen(fovTail);
        check(fovN > 0 && static_cast<size_t>(fovN) > fovTailLen && std::strcmp(fovLine + fovN - fovTailLen, fovTail) == 0,
              "inject line: the window's field-of-view range is its last token, 'fov=<narrowest>..<widest>' in radians to four places");
        VrWorldInjectWindow empty;
        fovN = vrWorldFormatInjectWindow(fovLine, sizeof(fovLine), empty);
        check(fovN > 0 && static_cast<size_t>(fovN) > 6 && std::strcmp(fovLine + fovN - 6, " fov=-") == 0,
              "inject line: a window in which no screen view's frustum was read says fov=- (not a range of zeros)");
    }
    check(!vrWorldInjectedWrongKind(f1), "STOP: a frame that injected kind 3 only is not a wrong-kind frame");
    bool eachWrong = true;
    for (int k = 0; k < 8; ++k) {
        VrWorldInjectFrame f;
        f.injectedKind[k] = 1;
        eachWrong = eachWrong && vrWorldInjectedWrongKind(f) == (k != 3);
    }
    check(eachWrong && !vrWorldInjectedWrongKind(VrWorldInjectFrame{}),
          "STOP: an injected call of any kind but 3 (0, 1, 2, 4, 5, other, unreadable) is a wrong-kind frame; an empty frame is not");
    char line[1100];
    int n = vrWorldFormatInjectWindow(line, sizeof(line), win);
    check(n > 0 && std::strstr(line, "vr world route inject 5s: inj-scene=96 inj-fp=12 inj-refused=6 warming=2 aux=4 after=6 unsupported=24 other-kind=2") &&
              std::strstr(line, "inj-kinds=3:108 pair-checked=0 pair-bad=0"),
          "inject line: the injector's window counters and the injected kinds are named (only kind 3)");
    VrWorldInjectWindow none;
    n = vrWorldFormatInjectWindow(line, sizeof(line), none);
    check(n > 0 && std::strstr(line, "inj-kinds=none pair-checked=0"),
          "inject line: a window with nothing injected says inj-kinds=none, zeros included (an absent line is what 'never ran' looks like)");
    VrWorldInjectWindow naming;
    naming.unnamedFrames = 2; naming.shutInjected = 0;
    n = vrWorldFormatInjectWindow(line, sizeof(line), naming);
    check(n > 0 && std::strstr(line, "pair-bad=0 inj-unnamed=2 inj-shut=0"),
          "inject line: the naming rule's two counters are named, zeros included: inj-unnamed (expected, one a map opening) and inj-shut (must stay 0)");
    VrWorldInjectFrame unnamedFrame;
    unnamedFrame.sceneInjected = 46; unnamedFrame.firstPersonInjected = 0;
    n = vrWorldFormatUnnamedOpen(line, sizeof(line), 7000, unnamedFrame);
    check(n > 0 && std::strstr(line, "frame=7000 named no source for the screen but its camera window was open") &&
              std::strstr(line, "46 scene and 0 first-person call(s) carried the phase this once") &&
              std::strstr(line, "the window stays shut until a frame names its source again"),
          "naming line: the first frame of a map, a menu or a transition (window open, nothing named) says what it cost and that the window shuts");
    n = vrWorldFormatStopShut(line, sizeof(line), 7001, "unnamed", unnamedFrame);
    check(n > 0 && std::strstr(line, "STOP at frame=7001") && std::strstr(line, "46 scene and 0 first-person camera call(s) were INJECTED on a frame whose window the route had shut (decision: unnamed)") &&
              std::strstr(line, "a shut window is never written through") && std::strstr(line, "switched off for this session"),
          "STOP line: a write on a frame the route had shut names the frame, the calls, the decision that shut it and what the route did about it");

    VrWorldInjectWindow wrong;
    wrong.injectedKind[3] = 10; wrong.injectedKind[5] = 2; wrong.injectedKind[7] = 1; wrong.pairChecked = 400; wrong.pairBad = 3;
    n = vrWorldFormatInjectWindow(line, sizeof(line), wrong);
    check(n > 0 && std::strstr(line, "inj-kinds=3:10,other:3 pair-checked=400 pair-bad=3"),
          "inject line: an injected kind other than 3 is counted under 'other' next to kind 3 and is what the census STOP reads");
    VrWorldInjectWindow only5;
    only5.injectedKind[5] = 6;
    n = vrWorldFormatInjectWindow(line, sizeof(line), only5);
    check(n > 0 && std::strstr(line, "inj-kinds=other:6 "), "inject line: only wrong kinds print 'other:N' with no stray comma");
    VrWorldInjectWindow big;
    big.scene = big.firstPerson = big.refused = big.warming = big.auxiliary = big.afterTrigger = big.unsupported = big.otherKind = 99999;
    big.unreadable = big.offThread = big.writeFail = big.pairChecked = big.pairBad = 99999;
    for (int k = 0; k < 8; ++k) big.injectedKind[k] = 99999;
    big.fovNarrowest = 3.1416f; big.fovWidest = 3.1416f;
    n = vrWorldFormatInjectWindow(line, sizeof(line), big);
    std::printf("        (the worst realistic injector line is %d characters; the route's buffer is 512)\n", n);
    check(n > 0 && n < 512, "inject line: at its worst it fits the route's 512-character buffer and the log's line");

    // The other stage 2 lines.
    VrWorldInjectFrame bad;
    bad.injectedKind[5] = 2; bad.injectedKind[4] = 1; bad.injectedKind[3] = 40;
    n = vrWorldFormatStopWrongKind(line, sizeof(line), 4242, bad);
    check(n > 0 && std::strstr(line, "STOP at frame=4242") && std::strstr(line, "4: 1") && std::strstr(line, "5: 2") &&
              std::strstr(line, "only kind 3 may be") && std::strstr(line, "switched off for this session"),
          "STOP line: names the frame, the kinds that were injected and what the route did about it");
    n = vrWorldFormatExcluded(line, sizeof(line), 1.0f, 1.0472f, 0.1f, 50000.0f, 0x34C9322ull, 1.7778f, 77);
    check(n > 0 && std::strstr(line, "EXCLUDED, not a screen view: kind 3 aspect=1.0000") && std::strstr(line, "caller=+0x34C9322") &&
              std::strstr(line, "(the screen's aspect is 1.7778; a screen view is within 4%)") && std::strstr(line, "77 call(s) so far, never injected"),
          "excluded line: a kind-3 call whose role is not known is named with its signature and never injected");
    VrWorldInjectFrame none1;
    none1.warming = 4; none1.auxiliary = 6; none1.sceneRefused = 0;
    n = vrWorldFormatNoSceneInjection(line, sizeof(line), 900, none1);
    check(n > 0 && std::strstr(line, "no scene camera call was injected at frame=900") && std::strstr(line, "warming 4") && std::strstr(line, "the frame resolves unjittered"),
          "no-scene line: a frame the route meant to jitter and could not says why and resolves unjittered");
    VrWorldInjectFrame ok;
    ok.sceneInjected = 48; ok.firstPersonInjected = 6;
    n = vrWorldFormatJitterLive(line, sizeof(line), 1000, 0.25f, -0.125f, 5040, 2835, ok);
    check(n > 0 && std::strstr(line, "JITTERED from frame=1000: phase (0.2500,-0.1250) px in 5040x2835") && std::strstr(line, "48 scene and 6 first-person") &&
              std::strstr(line, "eye cameras, kind 5, are never written") && std::strstr(line, "eye shift stays off while the route owns the world"),
          "jitter line: the first jittered frame of an ownership episode names the phase, the render size, the calls injected and that the eyes are untouched");
    n = vrWorldFormatRowsMismatch(line, sizeof(line), 1234, 0.25f, -0.125f, 0.0f, 0.0f, 0.0002f);
    check(n > 0 && std::strstr(line, "camera rows disagree with the phase at frame=1234") && std::strstr(line, "still resolved"),
          "rows line: a pair of frames whose rows did not differ by the phases they carry is named and the frame is still resolved");

    // The decline log: a cap per RUN of declines, not per session.
    check(kVrWorldDeclineLinesPerRun == 3 && kVrWorldDeclineLinesPerSession >= 32, "decline log: three lines a run, a session cap behind it");
    check(vrWorldDeclineLogAllowed(0, 0) && vrWorldDeclineLogAllowed(2, 0) && !vrWorldDeclineLogAllowed(3, 0) && !vrWorldDeclineLogAllowed(99, 0),
          "decline log: a run logs its first three declines and then stops");
    check(vrWorldDeclineLogAllowed(0, kVrWorldDeclineLinesPerSession - 1) && !vrWorldDeclineLogAllowed(0, kVrWorldDeclineLinesPerSession),
          "decline log: the session cap is a backstop for a flapping selector, behind the run cap");
    {   // flight 1: twelve session lines were spent on the entry and the first seconds of one gap. Model: 10 gaps of 600 declines.
        uint32_t runLines = 0, sessionLines = 0, logged = 0, perRunMax = 0;
        for (int gap = 0; gap < 10; ++gap) {
            uint32_t inRun = 0;
            for (int d = 0; d < 600; ++d)
                if (vrWorldDeclineLogAllowed(runLines, sessionLines)) { ++runLines; ++sessionLines; ++logged; ++inRun; }
            perRunMax = inRun > perRunMax ? inRun : perRunMax;
            runLines = 0;   // a treated frame ends the run and re-arms the cap
        }
        check(logged == 30 && perRunMax == 3,
              "decline log: ten long gaps log three lines each (the old per-session cap of twelve would have said nothing after the fourth)");
        runLines = sessionLines = 0; logged = 0;
        for (int d = 0; d < 1000; ++d) {   // a selector that flaps: decline, treated, decline, ...
            if (vrWorldDeclineLogAllowed(runLines, sessionLines)) { ++runLines; ++sessionLines; ++logged; }
            runLines = 0;
        }
        check(logged == kVrWorldDeclineLinesPerSession, "decline log: a selector that flaps a thousand times logs the session cap and no more");
    }

    // The RELEASED line names its last decline.
    char rel[512];
    n = vrWorldFormatReleased(rel, sizeof(rel), 500, VrWorldRelease::Declined, 120, "actual-hdr-binding-or-depth-view-refused", 3);
    check(n > 0 && std::strstr(rel, "(frames-not-treated; last decline: actual-hdr-binding-or-depth-view-refused x3)") &&
              std::strstr(rel, "after 120 owned frame(s)"),
          "RELEASED line: a release by untreated frames names the last decline's reason and how many in a row");
    n = vrWorldFormatReleased(rel, sizeof(rel), 500, VrWorldRelease::Declined, 120, "no-trigger", 3);
    check(n > 0 && std::strstr(rel, "last decline: no-trigger x3"), "RELEASED line: a run of frames with no trigger at all says no-trigger");
    n = vrWorldFormatReleased(rel, sizeof(rel), 500, VrWorldRelease::Declined, 120, nullptr, 0);
    check(n > 0 && std::strstr(rel, "last decline") == nullptr, "RELEASED line: with no decline to name there is no tail");
    n = vrWorldFormatReleased(rel, sizeof(rel), 500, VrWorldRelease::Declined, 120, "x", 0);
    check(n > 0 && std::strstr(rel, "last decline") == nullptr, "RELEASED line: a reason with a zero run is not printed");
    n = vrWorldFormatReleased(rel, sizeof(rel), 500, VrWorldRelease::Declined, 0, "actual-hdr-binding-or-depth-view-refused", 99999);
    check(n > 0 && n < 400, "RELEASED line: at its longest reason it stays well inside the log's line");
}

// ---- 8. the on-foot maps gate and the route ------------------------------------------------------------------------------------
static_assert(kUiMapsReleaseFrames == kVrWorldGraceFrames, "the maps gate releases on the route's grace: both let go on one boundary");

// The two boundaries in the order vscreen.cpp runs them: the layer's gate steps with the frame's naming, then the route's machine
// reads that gate and the facts of the frame that ended. The route only watches draws while the gate held when the frame began
// (g_vrWorldWants), and a frame it watches is treated when the world camera named the screen's source in it.
struct Boundaries {
    UiMapsGate gate;
    VrWorldMachine route;
    uint64_t frames = 0;
    uint64_t gateHeldAt = 0, gateReleasedAt = 0, routeOwnedAt = 0, routeReleasedAt = 0;
    VrWorldRelease releaseReason = VrWorldRelease::None;
    bool invariantHeld = true;   // the route is never owned while the gate is open
    void frame(bool named) {
        const bool heldWhenItBegan = gate.world;
        const bool treated = named && heldWhenItBegan;
        const UiMapsEdge edge = uiMapsStep(gate, named);
        if (edge == UiMapsEdge::Hold) gateHeldAt = frames;
        if (edge == UiMapsEdge::Release) gateReleasedAt = frames;
        VrWorldFrameEnd f;
        f.keyOn = true; f.layerLive = true; f.gate = gate.world; f.treated = treated;
        const VrWorldStep s = route.frameEnd(f);
        if (s.entered) routeOwnedAt = frames;
        if (s.released != VrWorldRelease::None) { routeReleasedAt = frames; releaseReason = s.released; }
        invariantHeld = invariantHeld && (!route.owned() || gate.world);
        ++frames;
    }
    void run(unsigned n, bool named) { for (unsigned i = 0; i < n; ++i) frame(named); }
};

void mapsGateCases() {
    check(kUiMapsReleaseFrames == kVrWorldGraceFrames, "maps gate: the release count is the route's grace (3 unnamed frames let both go)");
    check(kUiMapsHoldFrames == 2, "maps gate: the hold count is two named frames");
    Boundaries b;
    b.run(40, true);   // the world: frames 0..39
    check(b.gateHeldAt == 1, "maps gate: from a gate that does not hold the world, the second named frame holds it");
    check(b.route.owned() && b.routeOwnedAt == b.gateHeldAt + kVrWorldWarmFrames,
          "maps gate: the route, which needs the gate, owns the world kVrWorldWarmFrames treated frames after the gate holds it");
    const uint64_t mapOpens = b.frames;
    b.run(kUiMapsReleaseFrames - 1, false);   // the first two frames of a map: the route's grace and the gate's run both still hold
    check(b.route.owned() && b.gate.world, "maps gate: two unnamed frames release neither the gate nor the route");
    b.run(1, false);   // the third
    check(!b.route.owned() && !b.gate.world && b.gateReleasedAt == mapOpens + 2 && b.routeReleasedAt == mapOpens + 2,
          "maps gate: the third unnamed frame releases the gate and the route on ONE boundary");
    check(b.releaseReason == VrWorldRelease::GateLost,
          "maps gate: ... and the route's reason is the gate (on-foot-gate-lost): the gate ran first and the route read it");
    b.run(30, false);   // the map goes on
    check(!b.route.owned() && !b.gate.world, "maps gate: a map that goes on keeps both released");
    const uint64_t mapCloses = b.frames;
    b.run(1, true);
    check(!b.gate.world && !b.route.owned(), "maps gate: the first named frame after a map holds nothing yet");
    b.run(1, true);
    check(b.gate.world && b.gateHeldAt == mapCloses + 1, "maps gate: the second named frame after a map holds the world again");
    check(!b.route.owned() && b.route.state == VrWorldState::Observing,
          "maps gate: the route has not owned anything yet; it needs the gate and then its warm-up");
    b.run(kVrWorldWarmFrames, true);
    check(b.route.owned() && b.routeOwnedAt == b.gateHeldAt + kVrWorldWarmFrames,
          "maps gate: the route owns again kVrWorldWarmFrames treated frames after the gate held, the eye route serving the frames between");
    check(b.invariantHeld, "maps gate: at no boundary of the whole replay was the route owned while the gate was open");

    // Flight 1's runs, through both machines: the world was named for 13,044 frames, a map showed for 1,597, the world for 98, a map for 929.
    Boundaries f1;
    f1.run(13044, true);
    f1.run(1597, false);
    f1.run(98, true);
    // The 98-frame world between the maps: two frames for the gate, then the route's eight treated frames fit in what is left.
    check(f1.route.owned() && f1.routeOwnedAt == 13044 + 1597 + 1 + kVrWorldWarmFrames,
          "flight 1 replay: the 98-frame world between the maps is long enough for the route to own it again");
    f1.run(929, false);
    f1.run(40, true);
    check(f1.invariantHeld, "flight 1 replay: the route is never owned while the gate is open");
    check(f1.gateReleasedAt == 13044 + 1597 + 98 + 2 && f1.routeReleasedAt == f1.gateReleasedAt,
          "flight 1 replay: the second map releases the gate and the route on one boundary");
    check(f1.route.owned(), "flight 1 replay: after the last map the route owns again");
}

// ---- 9. the stage 2 experiment build, the pure half ----------------------------------------------------------------------------
// experimental.temporal_aa_on_foot_world_steady_detail (on by default since flight 4: the stale-slot refusal relaxed on foot where last
// frame's depth confirms it; an explicit off, or any word but on, is the refusal as before), the refusal census's
// line and the two lines that say a key or the view changed. The reader parses these texts, so the line pins here are the contract.
void experimentCases() {
    check(vrWorldSteadyKeyFromText("on") == VrWorldSteadyKey::On && vrWorldSteadyKeyFromText("ON") == VrWorldSteadyKey::On &&
              vrWorldSteadyKeyFromText("On") == VrWorldSteadyKey::On,
          "steady-detail key: on, in any case, reads on");
    const char* notOn[] = {"off", "", "auto", "onx", "o", " on", "on ", "true", "1", "yes", "steady", "of"};
    bool allOff = true;
    for (const char* t : notOn) allOff = allOff && vrWorldSteadyKeyFromText(t) == VrWorldSteadyKey::Off;
    check(allOff && vrWorldSteadyKeyFromText(nullptr) == VrWorldSteadyKey::Off,
          "steady-detail key: anything that is not exactly on, an explicit off and a typo included, reads off: the file's off is the refusal exactly as before the key existed");
    check(std::strcmp(vrWorldSteadyKeyName(VrWorldSteadyKey::On), "on") == 0 && std::strcmp(vrWorldSteadyKeyName(VrWorldSteadyKey::Off), "off") == 0,
          "steady-detail key: the names are the ini's words");

    // The route line names the key's state, off in a window that never set it (the default window is the flight 2 route).
    char line[1200];
    VrWorldWindow w;
    int n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && std::strstr(line, " fp-mode=0/0/0 steady-detail=off curve=off curve-reissues=0 last-trigger="),
          "route line: steady-detail=off sits after fp-mode in a window that never turned it on (a default window is flight 2's route), with curve=off curve-reissues=0 after it");
    w.steady = vrWorldSteadyKeyName(VrWorldSteadyKey::On);
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && std::strstr(line, " steady-detail=on curve=off curve-reissues=0 last-trigger="), "route line: steady-detail=on when the key is on");

    // The refusal line: "ran, nothing refused" and "never ran" are different texts.
    VrWorldRefusalWindow r;
    char rl[1024];
    n = vrWorldFormatRefusalWindow(rl, sizeof(rl), r);
    check(n > 0 && std::strstr(rl, "vr world route refusal 5s: census=off every=0 treated=0 asked=0 sampled=0 read=0 dropped=0 size=0x0 pixels=0 refused=0 "
                                   "refused-pct=0.000 stale-refused=0 masked=0 corrupt=0 sentinel=0 unreprojectable=0 camera=0 range=0 depth=0 weapon=0 other=0 "
                                   "stale-kept=0 depth-check=0/0 steady-detail=off view=off"),
          "refusal line: every counter is printed, zero included, and an empty window reads census=off pixels=0");
    r.census = true; r.every = kFlatMonoRefusalEvery; r.treated = 450; r.asked = 450; r.sampled = 113; r.read = 112; r.dropped = 1;
    r.width = 5040; r.height = 2835; r.pixels = 112ull * 5040 * 2835;
    r.counts[kFlatMonoClassStale] = 1200000; r.counts[kFlatMonoClassMasked] = 3000; r.counts[kFlatMonoClassSentinel] = 11;
    r.counts[kFlatMonoClassRange] = 1200; r.counts[kFlatMonoClassWeaponRefused] = 2000; r.counts[kFlatMonoClassReset] = 7;
    r.counts[kFlatMonoRefusalStaleKept] = 0; r.steady = "off"; r.view = "on";
    n = vrWorldFormatRefusalWindow(rl, sizeof(rl), r);
    check(n > 0 && n < 700 && std::strstr(rl, "census=on every=4 treated=450 asked=450 sampled=113 read=112 dropped=1 size=5040x2835 pixels=1600300800 ") &&
              std::strstr(rl, "refused=1206218 refused-pct=0.075 stale-refused=1200000 masked=3000 corrupt=0 sentinel=11 unreprojectable=0 camera=0 range=1200 depth=0 "
                              "weapon=2000 other=7 stale-kept=0 depth-check=0/0 steady-detail=off view=on"),
          "refusal line: a sampled window prints its size, the pixels examined, the refused total and its share, each cause, the unnamed remainder as other, "
          "the key's state and the view");
    // The key on (the depth-validated form): the stale pixels are two numbers, the ones last frame's depth confirmed (stale-kept: not refused,
    // not in the refused total) and the ones it did not (stale-refused: refused, in the total), and the line says how many frames ran the check.
    r.steady = "on"; r.counts[kFlatMonoClassStale] = 10000; r.counts[kFlatMonoRefusalStaleKept] = 1190000; r.checked = 448; r.skipped = 2;
    n = vrWorldFormatRefusalWindow(rl, sizeof(rl), r);
    check(n > 0 && std::strstr(rl, "stale-refused=10000 ") && std::strstr(rl, "stale-kept=1190000 depth-check=448/2 steady-detail=on") &&
              std::strstr(rl, "refused=16218 refused-pct=0.001 "),
          "refusal line: with the key on the stale pixels split in two: stale-kept (confirmed by last frame's depth, not refused) and stale-refused (refused, counted), and the depth check's ran/skipped frames");
    {   // The old spellings are gone from the text: a reader that wants them reads the flight-3 logs by its own table.
        const bool oldSpellings = std::strstr(rl, " stale=") || std::strstr(rl, "forgiven=");
        check(!oldSpellings, "refusal line: the flight-3 tokens (stale=, forgiven=) are no longer printed: the new line has stale-refused= and stale-kept=");
    }
    VrWorldRefusalWindow ran;
    ran.census = true; ran.every = 4; ran.treated = 450; ran.asked = 450; ran.sampled = 113; ran.read = 113; ran.width = 5040; ran.height = 2835;
    ran.pixels = 113ull * 5040 * 2835;
    n = vrWorldFormatRefusalWindow(rl, sizeof(rl), ran);
    VrWorldRefusalWindow never;
    never.census = true; never.every = 4; never.treated = 450;
    char nl[1024];
    const int nn = vrWorldFormatRefusalWindow(nl, sizeof(nl), never);
    check(n > 0 && nn > 0 && std::strstr(rl, "read=113 dropped=0 size=5040x2835 pixels=1614589200 refused=0 refused-pct=0.000 ") &&
              std::strstr(nl, "sampled=0 read=0 dropped=0 size=0x0 pixels=0 refused=0 ") && std::strcmp(rl, nl) != 0,
          "refusal line: 'ran and refused nothing' (pixels > 0, refused=0) and 'on but never sampled' (sampled=0 pixels=0) are different texts");
    VrWorldRefusalWindow big;
    big.census = true; big.every = ~0u; big.treated = big.asked = big.sampled = big.read = big.dropped = ~0ull; big.width = big.height = ~0u; big.pixels = ~0ull;
    big.checked = big.skipped = ~0ull;
    for (uint32_t i = 0; i < kFlatMonoRefusalSlots; ++i) big.counts[i] = ~0ull;
    big.steady = "off"; big.view = "off";
    n = vrWorldFormatRefusalWindow(rl, sizeof(rl), big);
    std::printf("        (the widest refusal line is %d characters; the route's buffer is 1024)\n", n);
    check(n > 0 && n < 1000, "refusal line: at its widest (every counter at 2^64-1) it fits its buffer and the log's line");
    check(kFlatMonoRefusalStaleKept == 15 && kFlatMonoRefusalSlots == 16 && kFlatMonoClassReset == 14 && kFlatMonoClassRefusedBit == 0x80u,
          "refusal line: the census has sixteen slots, class 0..14 for refused pixels and slot 15 for the stale pixels the steady-detail rule kept");
    // The depth check's tolerance is one number, in one header (the HLSL and the resolver's rig are held to it there).
    check(kFlatMonoStaleDepthRelative == 0.01 && kFlatMonoStaleDepthFloor == 1e-6,
          "steady detail: the depth check's tolerance is 1% (floor 1e-6), the resolver's own TAA's");

    // The two change lines.
    char cl[768];
    n = vrWorldFormatSteadyChanged(cl, sizeof(cl), 4242, VrWorldSteadyKey::On);
    check(n > 0 && std::strstr(cl, "vr world route: steady-detail is ON from frame=4242") && std::strstr(cl, "where last frame's depth confirms it") &&
              std::strstr(cl, "is refused where it does not") && std::strstr(cl, "masked records and corrupt slots stay refused") &&
              std::strstr(cl, "pass the depth test and ghost"),
          "steady-detail line: coming on says what it does (only where last frame's depth confirms it) and what it costs (a lateral mover's interior can ghost), and that masked records stay refused");
    n = vrWorldFormatSteadyChanged(cl, sizeof(cl), 4300, VrWorldSteadyKey::Off);
    check(n > 0 && std::strstr(cl, "vr world route: steady-detail is OFF from frame=4300") && std::strstr(cl, "refuses its history, as in flight 2"),
          "steady-detail line: going off says the route is flight 2's again");
    n = vrWorldFormatViewChanged(cl, sizeof(cl), 77, true);
    check(n > 0 && n < 768 && std::strstr(cl, "vr world route: the refusal view is ON from frame=77 (advanced.temporal_aa_debug = motion_source)") &&
              std::strstr(cl, "yellow stale slot") && std::strstr(cl, "red masked record") && std::strstr(cl, "before the game's tone pass"),
          "view line: coming on names the key and the legend (yellow stale slot, red masked record) and the tone-pass caveat");
    n = vrWorldFormatViewChanged(cl, sizeof(cl), 99, false);
    check(n > 0 && std::strstr(cl, "vr world route: the refusal view is OFF from frame=99") != nullptr, "view line: going off is one short line");
    // The classes the census names are the numbers the shader writes (the resolver rig compares them to the HLSL).
    check(kFlatMonoClassNone == 0 && kFlatMonoClassJoined == 1 && kFlatMonoClassMasked == 2 && kFlatMonoClassNotRig == 3 && kFlatMonoClassStale == 4 &&
              kFlatMonoClassCorrupt == 5 && kFlatMonoClassStaleStamp == 6 && kFlatMonoClassSentinel == 7 && kFlatMonoClassUnreprojectable == 8 &&
              kFlatMonoClassCamera == 9 && kFlatMonoClassRange == 10 && kFlatMonoClassDepth == 11 && kFlatMonoClassWeapon == 12 &&
              kFlatMonoClassWeaponRefused == 13,
          "classes: 1..6 are the eye path's source kinds (the same numbers and colours), 7..13 are the resolver's own");
    bool namesOk = true;
    for (uint32_t c = 0; c <= kFlatMonoClassReset; ++c) namesOk = namesOk && std::strcmp(flatMonoClassName(c), "?") != 0;
    check(namesOk && std::strcmp(flatMonoClassName(15), "?") == 0 && std::strcmp(flatMonoClassName(kFlatMonoClassStale), "stale") == 0 &&
              std::strcmp(flatMonoClassName(kFlatMonoClassMasked), "masked") == 0,
          "classes: every class 0..14 has a name and nothing else does");
}

// ---- 6. the source pins --------------------------------------------------------------------------------------------------
std::string g_root;
std::string readFile(const char* rel) {
    std::ifstream in(g_root + "\\" + rel, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
// The text of the function whose definition starts at `head` (first occurrence), up to its closing brace at column 0.
std::string functionBody(const std::string& src, const std::string& head) {
    const size_t at = src.find(head);
    if (at == std::string::npos) return {};
    const size_t end = src.find("\n}\n", at);
    return src.substr(at, end == std::string::npos ? std::string::npos : end - at + 2);
}
bool before(const std::string& s, const char* a, const char* b) {
    const size_t x = s.find(a), y = s.find(b);
    return x != std::string::npos && y != std::string::npos && x < y;
}
// `a` first occurs before the LAST occurrence of `b` (the game's own forward is the last real call in a thunk).
bool beforeLast(const std::string& s, const char* a, const char* b) {
    const size_t x = s.find(a), y = s.rfind(b);
    return x != std::string::npos && y != std::string::npos && x < y;
}
size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}
// `text` with the one occurrence of `from` replaced; empty when `from` is not there exactly once (a control with a stale anchor must fail).
std::string replaceOnce(const std::string& text, const std::string& from, const std::string& to) {
    if (count(text, from) != 1) return {};
    std::string out = text;
    out.replace(out.find(from), from.size(), to);
    return out;
}

// ---- the curved route's pins on the route's own source ------------------------------------------------------------------------------
// A curved screen (fix.panel_curvature above 0) no longer holds the route off: the layer re-issues it through the strip the game's own draw
// is substituted with. What the route's source must say about that is a function of its text, so curveBoundaryControls runs the very same
// function on copies with one edit each that put the old behaviour (or a likely slip) back, and requires the pin named to fail.
struct CurvePin { const char* id; bool ok; const char* what; };
std::vector<CurvePin> curveBoundaryPins(const std::string& rt) {
    std::vector<CurvePin> pins;
    const std::string boundary = functionBody(rt, "void vrWorldRouteFrameBoundary(");
    const std::string curveText = functionBody(rt, "void curveTextNow(");
    // The statement that says the route stays off: from the layer test to the line that clears the note.
    const size_t offAt = boundary.find("if (!layerLive) {");
    const size_t offEnd = offAt == std::string::npos ? offAt : boundary.find("g_notLiveNoted = nullptr;", offAt);
    const std::string staysOff = (offAt == std::string::npos || offEnd == std::string::npos) ? std::string() : boundary.substr(offAt, offEnd - offAt);
    const size_t npos = std::string::npos;
    pins.push_back({"layer-live",
                    !boundary.empty() && boundary.find("    const bool layerLive = uiLayerLiveForWorldRoute();\n") != npos &&
                        boundary.find("const bool curved") == npos && boundary.find("!curved") == npos && rt.find("panelCurveWants") == npos,
                    "curvature: layerLive is the layer's own predicate and nothing else, and the route never asks panel_curve whether a substitution is wanted (a curved screen does not hold the route off)"});
    pins.push_back({"stays-off-text",
                    !staysOff.empty() && staysOff.find("the UI layer, so keep fix.ui_quality on)\", why);") != npos && staysOff.find("panel_curvature") == npos &&
                        staysOff.find("uiLayerNotLiveReason()") != npos,
                    "curvature: the route's stays-off line gives the layer's reason and names fix.ui_quality alone (no fix.panel_curvature)"});
    pins.push_back({"curve-text",
                    !curveText.empty() && curveText.find("const PanelCurveInfo pc = panelCurveInfo();") != npos &&
                        curveText.find("const bool configured = pc.curvature > 0.0f || pc.segments != detail::kDefaultSegments;") != npos &&
                        curveText.find("vrWorldFormatCurve(out, size, configured, pc.standDown, pc.ready, pc.curvature, pc.segments, pc.gain);") != npos &&
                        curveText.find("if (reissues) *reissues = pc.reissues;") != npos,
                    "curvature: curveTextNow reads panelCurveInfo once and words it with vrWorldFormatCurve: asked for by the curvature or the column count, stood down, ready, and the strip's numbers; it hands back the cumulative strip count"});
    pins.push_back({"owns-line",
                    before(boundary, "curveTextNow(curveText, sizeof(curveText), nullptr);",
                           "vrWorldFormatEntered(line, sizeof(line), g_frameNo, kVrWorldWarmFrames, curveText);") &&
                        boundary.find("char curveText[sizeof(g_win.curve)];") != npos,
                    "curvature: the OWNS line is told the curve in use (curveTextNow, then vrWorldFormatEntered with its text)"});
    pins.push_back({"window-line",
                    before(boundary, "curveTextNow(g_win.curve, sizeof(g_win.curve), &curveReissues);", "g_win.curveReissues = curveReissues - g_curveReissuesSeen;") &&
                        before(boundary, "g_win.curveReissues = curveReissues - g_curveReissuesSeen;", "g_curveReissuesSeen = curveReissues;") &&
                        before(boundary, "g_curveReissuesSeen = curveReissues;", "vrWorldFormatWindow(line, sizeof(line), key,"),
                    "curvature: the 5 s line's curve= comes from curveTextNow and its curve-reissues= is the window's delta of panel_curve's cumulative count, kept for the next window, all before the line is printed"});
    pins.push_back({"one-reader",
                    count(rt, "panelCurveInfo()") == 1 && count(rt, "curveTextNow(") == 3 && count(rt, "uint64_t g_curveReissuesSeen = 0;") == 1 &&
                        count(rt, "g_curveReissuesSeen") == 3,
                    "curvature: the route reads panel_curve's state in one place (curveTextNow: defined once, called for the OWNS line and the 5 s line), and keeps one running count"});
    return pins;
}
// Each control: one edit of the route's text that must be caught by the pin it names.
void curveBoundaryControls(const std::string& rt) {
    struct Flip { const char* name; const char* from; const char* to; const char* caughtBy; };
    const Flip flips[] = {
        {"the old stand-aside returns", "    const bool layerLive = uiLayerLiveForWorldRoute();\n",
         "    const bool curved = panelCurveWants();\n    const bool layerLive = uiLayerLiveForWorldRoute() && !curved;\n", "layer-live"},
        {"another term joins layerLive", "    const bool layerLive = uiLayerLiveForWorldRoute();\n",
         "    const bool layerLive = uiLayerLiveForWorldRoute() && panelCurveInfo().ready;\n", "layer-live"},
        {"the stays-off line names fix.panel_curvature again", "so keep fix.ui_quality on)\", why);",
         "so keep fix.ui_quality on and fix.panel_curvature at 0)\", why);", "stays-off-text"},
        {"the stays-off line forgets the key to set", "so keep fix.ui_quality on)\", why);", "so keep the layer on)\", why);", "stays-off-text"},
        {"the OWNS line is not told the curve", "vrWorldFormatEntered(line, sizeof(line), g_frameNo, kVrWorldWarmFrames, curveText);",
         "vrWorldFormatEntered(line, sizeof(line), g_frameNo, kVrWorldWarmFrames);", "owns-line"},
        {"the OWNS line's curve text is never filled", "            curveTextNow(curveText, sizeof(curveText), nullptr);\n", "            curveText[0] = 0;\n", "owns-line"},
        {"the 5 s line's strip count is cumulative", "g_win.curveReissues = curveReissues - g_curveReissuesSeen;", "g_win.curveReissues = curveReissues;", "window-line"},
        {"the window forgets the count it reported", "            g_curveReissuesSeen = curveReissues;\n", "", "window-line"},
        {"the 5 s line's curve text is never filled", "curveTextNow(g_win.curve, sizeof(g_win.curve), &curveReissues);", "curveReissues = 0;", "window-line"},
        {"the curve text ignores a stood-down substitution", "configured, pc.standDown, pc.ready,", "configured, false, pc.ready,", "curve-text"},
        {"the curve text ignores the column count", "pc.curvature > 0.0f || pc.segments != detail::kDefaultSegments;", "pc.curvature > 0.0f;", "curve-text"},
        {"the curve text drops the strip count", "    if (reissues) *reissues = pc.reissues;\n", "", "curve-text"},
        {"a second reader of panel_curve's state", "    const bool layerLive = uiLayerLiveForWorldRoute();\n",
         "    const bool layerLive = uiLayerLiveForWorldRoute();\n    (void)panelCurveInfo();\n", "one-reader"},
    };
    for (const Flip& f : flips) {
        char label[320];
        const std::string mutated = replaceOnce(rt, f.from, f.to);
        std::snprintf(label, sizeof(label), "control: \"%s\" has its anchor in the route's source exactly once", f.name);
        check(!mutated.empty(), label);
        bool caught = false;
        if (!mutated.empty())
            for (const CurvePin& p : curveBoundaryPins(mutated)) caught = caught || (!p.ok && std::strcmp(p.id, f.caughtBy) == 0);
        std::snprintf(label, sizeof(label), "control: \"%s\" is caught by the %s pin", f.name, f.caughtBy);
        check(caught, label);
    }
    bool pristine = true;
    for (const CurvePin& p : curveBoundaryPins(rt)) pristine = pristine && p.ok;
    check(pristine, "control: with no edit every curved-route pin of the route's source holds (the controls above are the only way one fails)");
}

void sourcePins() {
    if (g_root.empty()) { check(false, "source pins: the repo root was not given (vr_world_route_test --self-test <root>)"); return; }
    const std::string vs = readFile("src\\d3d11\\vscreen.cpp");
    const std::string ex = readFile("src\\d3d11\\exposure_fix.cpp");
    const std::string rt = readFile("src\\d3d11\\vr_world_route.cpp");
    check(!vs.empty() && !ex.empty() && !rt.empty(), "source pins: the three sources are readable");

    // The internal-call early return on every draw and dispatch thunk, BEFORE anything the VR path does with the call.
    struct Thunk { const char* head; const char* first; };
    const Thunk thunks[] = {
        {"void STDMETHODCALLTYPE hookedDraw(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawAuto(", "uiLayerIssueBlocked()"},
        {"void STDMETHODCALLTYPE hookedDrawIndexed(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawInstanced(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawIndexedInstanced(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawIndexedInstancedIndirect(", "uiLayerIssueBlocked()"},
        {"void STDMETHODCALLTYPE hookedDrawInstancedIndirect(", "uiLayerIssueBlocked()"},
    };
    bool early = true, detector = true, afterFlat = true;
    for (const Thunk& t : thunks) {
        const std::string body = functionBody(vs, t.head);
        early = early && !body.empty() && before(body, "if (g_vrWorldInternal)", t.first);
        detector = detector && beforeLast(body, "if (g_vrWorldWants && self == g_state->ownerCtx) vrWorldRouteDraw(self);", "g_state->realDraw");
        afterFlat = afterFlat && before(body, "if (runtimeFlatProfile())", "if (g_vrWorldInternal)");
    }
    check(early, "hooks: all seven draw thunks step aside for the route's own draws (g_vrWorldInternal) before their VR path begins");
    check(detector, "hooks: all seven draw thunks call vrWorldRouteDraw, guarded by g_vrWorldWants and the owner context, before the game's draw is issued");
    check(afterFlat, "hooks: the early return sits after the flat profile's branch, so the flat path is untouched");
    check(count(vs, "if (g_vrWorldInternal)") == 7, "hooks: g_vrWorldInternal is tested in exactly the seven draw thunks (no other hook)");
    const std::string d1 = functionBody(ex, "void STDMETHODCALLTYPE hookedDispatch(");
    const std::string d2 = functionBody(ex, "void STDMETHODCALLTYPE hookedDispatchIndirect(");
    check(before(d1, "if (g_vrWorldInternal)", "gpuFrameCommand(self);") && before(d2, "if (g_vrWorldInternal)", "gpuFrameCommand(self);"),
          "hooks: both dispatch thunks step aside for the route's own dispatches (the exposure fix would read them as the game's)");
    check(before(d1, "vrWorldRouteNoteDispatch();", "drawCensusDispatch") , "hooks: a dispatch after the trigger tells the route what its UAVs write");
    const std::string cs = functionBody(ex, "void STDMETHODCALLTYPE hookedClearState(");
    check(before(cs, "if (g_vrWorldInternal)", "bindingForgetAll();") && count(ex, "if (g_vrWorldInternal)") == 3,
          "hooks: the exposure fix's ClearState hook steps aside for the route's own isolation of the context (it would wipe the binding shadow); the exposure file tests the flag in exactly three hooks");
    // The write observers.
    const char* writers[] = {"void STDMETHODCALLTYPE hookedCopyResource(", "void STDMETHODCALLTYPE hookedCopySubresourceRegion(",
                             "void STDMETHODCALLTYPE hookedUpdateSubresource(", "void STDMETHODCALLTYPE hookedClearRtv("};
    bool writes = true;
    for (const char* w : writers) writes = writes && functionBody(vs, w).find("g_vrWorldWatchWrites") != std::string::npos;
    check(writes, "hooks: copy, copy-region, update and clear-RTV tell the route what they write once the trigger has fired (g_vrWorldWatchWrites)");
    check(before(vs, "tkUiLayer.run(", "tkVrWorldRoute.run(") && before(vs, "tkVrWorldRoute.run(", "tkScreenMotion.run("),
          "hooks: the route's boundary runs after the UI layer's (it reads the gate that boundary computed) and before screen motion's");

    // The route itself.
    const std::string draw = functionBody(rt, "void vrWorldRouteDraw(");
    check(before(draw, "bindingGet(BindSlot::Rtv0)", "lookup(") && before(draw, "if (!rtv) return;", "lookup("),
          "cost: a draw with no colour target returns at one shadow load, before any view is looked up");
    check(draw.find("vrWorldFillObservation(") != std::string::npos && draw.find("flatHdrObserveDraw(") != std::string::npos &&
              draw.find("flatHdrCandidateDraw(") != std::string::npos,
          "trigger: the per-draw path runs the flat HDR route's own detector through the route's glue");
    check(rt.find("bindingResolveProbe(") != std::string::npos && rt.find("bindingResolve(") == std::string::npos,
          "cost: views are resolved through the shadow's guarded instrument resolver (its own fault budget), once a frame per view");
    check(draw.find("new ") == std::string::npos && draw.find("std::vector") == std::string::npos && draw.find("std::string") == std::string::npos &&
              draw.find("std::mutex") == std::string::npos && draw.find("lock_guard") == std::string::npos,
          "cost: the per-draw path allocates nothing and takes no lock");
    // The census's episodes (vr_camera_census.h): the join is reached from here, behind one null-tested pointer that is set only for an episode's sampled frame, so with the census off (or
    // outside that frame) a draw costs one load; it sits before the colour-target test because the join wants depth-only draws too.
    check(draw.find("if (detail::g_vrCensusJoinDraw) detail::g_vrCensusJoinDraw(ctx, f.draws);") != std::string::npos && before(draw, "++f.draws;", "detail::g_vrCensusJoinDraw") &&
              before(draw, "detail::g_vrCensusJoinDraw", "bindingGet(BindSlot::Rtv0)") && count(rt, "g_vrCensusJoinDraw") == 2,
          "census: the per-draw path counts the draw, then reaches the census's episode join through one null-tested pointer (set only for an episode's sampled frame), before the colour-target test; nothing else in the route touches it");
    const std::string boundary = functionBody(rt, "void vrWorldRouteFrameBoundary(");
    // THE KEY-OFF CONTRACT. With the key off, from the start, the boundary's first test returns and nothing below it runs: no
    // camera detour, no phase, no extra counter. Stage 2 adds exactly one thing to that test, the flag that says the injector
    // still has to be wound down after the key went off live; it is false from the start and set only by driveInjector.
    check(boundary.find("getString(\"experimental.temporal_aa_on_foot_world\", \"off\")") != std::string::npos &&
              before(boundary, "if (key == VrWorldKey::Off && g_machine.state == VrWorldState::Off && !g_census && !g_injectorEngaged)", "++g_frameNo"),
          "key off: the boundary reads the key with the default off and returns before doing anything when nothing is to be accounted");
    check(rt.find("bool g_injectorEngaged = false;") != std::string::npos && count(rt, "g_injectorEngaged = true;") == 1 &&
              before(rt, "void driveInjector(", "g_injectorEngaged = true;") && before(rt, "g_injectorEngaged = true;", "void windDownInjector("),
          "key off: the flag that keeps the boundary running starts false and is set in exactly one place, inside driveInjector");
    check(boundary.find("flatCameraVr") == std::string::npos && boundary.find("flatCameraInject") == std::string::npos,
          "key off: the boundary itself never touches the camera detour; only the two helpers it calls do, after the early return");
    {
        const size_t callAt = boundary.find("driveInjector(");
        const size_t guardAt = callAt == std::string::npos ? callAt : boundary.rfind("if (key == VrWorldKey::Auto) {", callAt);
        check(callAt != std::string::npos && guardAt != std::string::npos && callAt - guardAt < 400 &&
                  before(boundary, "const bool routeWorks = g_machine.state == VrWorldState::Warming || g_machine.state == VrWorldState::Owned;",
                         "driveInjector(routeWorks"),
              "jitter: the injector is driven only while the key is auto, and only for a route that is Warming or Owned");
    }
    check(count(rt, "driveInjector(") == 2 && count(rt, "windDownInjector(") == 2,
          "jitter: driveInjector is called once (the boundary) and windDownInjector once (the key-off branch), each defined once");
    const std::string drive = functionBody(rt, "void driveInjector(");
    check(!drive.empty() && drive.find("getString(\"experimental.temporal_aa_on_foot_world_jitter\", \"on\")") != std::string::npos &&
              drive.find("getBool(\"experimental.temporal_aa_jitter\", true)") != std::string::npos,
          "A/B: the jitter is decided from experimental.temporal_aa_on_foot_world_jitter (default on) and the global experimental.temporal_aa_jitter");
    // NAMING (design-world-camera-motion-2026-09-30.md section 5): the window opens only after a frame that named the screen's source.
    check(drive.find("vrWorldJitterDecide(true, g_jitterKey, globalJitter, wantsInjection && w && h, g_namedLast, true, g_injectFault)") != std::string::npos,
          "NAMING: the window is decided from the last frame's naming (g_namedLast); no other input can open it after an unnamed frame");
    check(count(rt, "vf.inject = true;") == 1 && before(drive, "if (want == VrWorldJitter::On) {", "vf.inject = true;") &&
              before(drive, "vrWorldJitterDecide(", "vf.inject = true;"),
          "jitter: the injector is asked to write only when the decision is On, and only in that one place");
    check(before(drive, "flatCameraVrFrame(vf);", "flatCameraVrQuiet()") && before(drive, "if (flatCameraVrQuiet()) g_injectorEngaged = false;", "g_jitter = want;"),
          "jitter: an injector that stopped being asked goes pass-through first and is let alone only once nothing it wrote waits for its flush");
    const std::string wind = functionBody(rt, "void windDownInjector(");
    check(!wind.empty() && wind.find("vf.inject = true") == std::string::npos && wind.find("flatCameraVrFrame(vf);") != std::string::npos &&
              wind.find("g_injectFault = false;") != std::string::npos,
          "key off live: the wind-down never asks for injection, passes the injector through, and clears a STOP (the key off and auto again clears it)");
    check(before(boundary, "windDownInjector();", "g_resourcesLive"), "key off live: the injector is wound down in the key-off branch");
    const std::string trig = functionBody(rt, "void onTrigger(");
    check(!trig.empty() && before(trig, "flatCameraVrCloseWindow();", "treatWorld(ctx);") && trig.find("if (g_injectorEngaged) flatCameraVrCloseWindow();") != std::string::npos,
          "window: the injection window closes at the trigger, before the route reads what the injector did, and only while it is engaged");
    const std::string treat = functionBody(rt, "void treatWorld(");
    check(!treat.empty() && treat.find("f.jitterX = worldApplied.x;") != std::string::npos && treat.find("f.rowsJitterX = worldApplied.x;") != std::string::npos &&
              treat.find("f.previousRowsJitterX = prevWorld.x;") != std::string::npos && treat.find("f.previousJitterX = prevWorld.x;") != std::string::npos &&
              treat.find("f.firstPersonPhaseMode = ") != std::string::npos,
          "phase: the resolver's jitter input and the rows' phase (this frame and last) are the phase the frame CONFIRMED, and the fold-in has its mode");
    check(treat.find("f.jitterX = g_framePhaseX") == std::string::npos && treat.find("f.rowsJitterX = g_framePhaseX") == std::string::npos &&
              treat.find("worldApplied = {g_framePhaseX, g_framePhaseY};") != std::string::npos &&
              before(treat, "inj.sceneInjected > 0", "worldApplied = {g_framePhaseX, g_framePhaseY};"),
          "phase: the chosen phase reaches the resolver only when a scene camera call took it (a chosen phase nothing wrote is never claimed)");
    check(treat.find("declineLine(\"camera-injection-incomplete\");") != std::string::npos && before(treat, "inj.sceneRefused || inj.firstPersonRefused || inj.writeFailures", "declineLine(\"camera-injection-incomplete\")"),
          "phase: a frame part of whose world cameras took the phase and part did not is declined, never resolved with one phase");
    check(before(treat, "flatCameraCheckRowPair(", "ComPtr<ID3D11ShaderResourceView> none;") && treat.find("++g_win.inject.pairBad;") != std::string::npos,
          "phase: the rows of consecutive frames are checked against the phases they claim to carry, and a disagreement is counted");
    check(!treat.empty() && before(treat, "facts.depthNamed = facts.depthKnown && engineVelocitySourceIsNamed(depthTex);", "g_frameNamed = facts.depthNamed;") &&
              before(treat, "g_frameNamed = facts.depthNamed;", "vrWorldSelect(facts)") && count(rt, "g_frameNamed = facts.depthNamed;") == 1,
          "NAMING: the frame's naming is the selector's own depthNamed fact, recorded once at the trigger, before the selection");
    check(before(boundary, "g_namedLast = g_frameNamed;", "driveInjector(routeWorks") && before(boundary, "driveInjector(routeWorks", "g_frameNamed = false;") &&
              count(rt, "g_namedLast = g_frameNamed;") == 1,
          "NAMING: the boundary carries the ended frame's naming into the next frame's decision, then clears it for the frame that starts (no trigger, no naming)");
    check(rt.find("bool g_namedLast = false;") != std::string::npos && rt.find("bool g_frameNamed = false;") != std::string::npos &&
              !functionBody(rt, "void windDownInjector(").empty() && functionBody(rt, "void windDownInjector(").find("g_namedLast = g_frameNamed = false;") != std::string::npos,
          "NAMING: both flags start false (nothing opens the window before a frame has named its source) and the key-off wind-down clears them");
    check(boundary.find("injected && g_jitter != VrWorldJitter::On") != std::string::npos && boundary.find("vrWorldFormatStopShut(") != std::string::npos &&
              boundary.find("g_win.inject.shutInjected") != std::string::npos && boundary.find("g_win.inject.unnamedFrames") != std::string::npos,
          "NAMING: a write on a frame the route shut is a STOP and is counted; a frame whose window was open and named nothing is counted as the expected first frame of a map");
    check(rt.find("vrWorldRouteWorldPhase(float* x, float* y) {") != std::string::npos && rt.find("WorldPhase worldPhase()") == std::string::npos,
          "phase: the route's world phase is the injector's (the zero-phase seam of the first build is gone)");
    check(rt.find("g_jitter == VrWorldJitter::On) g_phase.finish(") != std::string::npos,
          "phase: the phase machine's frame end is told whether the frame was treated and every scene injection landed");
    check(rt.find("flatMonoResolve(") != std::string::npos && before(rt, "VrWorldInternalScope internal;", "flatMonoResolve(") &&
              rt.find("kVrWorldFeatureSlot") != std::string::npos,
          "resolve: the resolver runs inside the internal scope, on the world's own upscaler slot");
    check(rt.find("GpuCensusSection::FrameWorldResolve") != std::string::npos, "census: the resolve is timed on its own GPU census section");
    // A curved screen no longer holds the route off: the layer re-issues it through the game's own strip (panel_curve.h panelCurveReissue,
    // vscreen.cpp). The boundary's pins are a function of the route's text, held by flipped copies (curveBoundaryControls).
    for (const CurvePin& p : curveBoundaryPins(rt)) check(p.ok, p.what);
    curveBoundaryControls(rt);
    // The census's eye draw: the screen composite's own shader pair, read only with the census key on.
    const std::string census = functionBody(vs, "__declspec(noinline) void cameraCensusEyeDraw(");
    check(!census.empty() && census.find("0x5C36AF051B98B9F1ull") != std::string::npos && census.find("0xCFE84157BC76E921ull") != std::string::npos &&
              census.find("vrCameraCensusEyeDraw(self,") != std::string::npos &&
              vs.find("if (g_vrWorldWants && vrCameraCensusWanted()) cameraCensusEyeDraw(self);") != std::string::npos &&
              count(vs, "cameraCensusEyeDraw(self)") == 1,
          "census: the screen composite tells the census its eye, behind one flag test (the route's watch flag, true while the census is wanted)");
    // The layer-only door is asked twice an eye and sequence (the temporal door, then the sharpen pass): the 5 s line's
    // door-layer-only counts an eye once, or it would read twice eye-takes on a healthy flight.
    const std::string door = functionBody(rt, "bool vrWorldRouteDoorLayerOnly(");
    check(!door.empty() && door.find("g_countedLayerOnly[eye] != sequence") != std::string::npos &&
              before(door, "g_countedLayerOnly[eye] != sequence", "++g_win.layerOnly") &&
              count(rt, "++g_win.layerOnly") == 1 && rt.find("g_countedLayerOnly[0] = g_countedLayerOnly[1] = 0;") != std::string::npos,
          "door: the layer-only door is counted once per eye and sequence, and the tag is cleared at the boundary");
    // Where the door asks. The temporal pass and the sharpen pass both read the route; nothing else does.
    const std::string nt = readFile("src\\d3d11\\native_temporal.cpp");
    const std::string ns = readFile("src\\d3d11\\native_sharpen.cpp");
    // The doors ask the layer's one predicate (ui_layer.h uiLayerDoorLayerOnly), which asks the route first and then, with
    // experimental.on_foot_maps_sharp on, the maps gate's; with that key off the layer answers the route's alone.
    const std::string ly = readFile("src\\d3d11\\ui_layer.cpp");
    check(!nt.empty() && !ns.empty() && count(nt, "uiLayerDoorLayerOnly(") == 1 && count(ns, "uiLayerDoorLayerOnly(") == 1 &&
              count(nt, "vrWorldRouteDoorLayerOnly(") == 0 && count(ns, "vrWorldRouteDoorLayerOnly(") == 0 &&
              count(ly, "vrWorldRouteDoorLayerOnly(") == 1,
          "door: the temporal pass and the sharpen pass each ask the layer's predicate once per eye, and only the layer asks the route");
    check(before(functionBody(ly, "bool uiLayerDoorLayerOnly(uint32_t eye, uint64_t sequence) {"), "if (vrWorldRouteDoorLayerOnly(eye, sequence)) return true;",
                 "if (!detail::g_uiLayerMapsOn || eye > 1) return false;"),
          "door: the layer's predicate answers the route's question first and the maps gate's only when the key is on");
    check(nt.find("&&!edvr::vrWorldRouteOwnsNextFrame()") != std::string::npos,
          "eye shift: native_temporal advertises the shift only while the route does not own the next frame");
    // The phase count (2026-10-01; temporal_math.h): the route resolves at the game's render size (render == output), so it hands its
    // phase machine the count the ratio rule gives for that -- the fixed eight, with experimental.temporal_aa_jitter_follows_upscale
    // on or off, and the route reads no such key -- and its 5 s window carries what the machine ran.
    const std::string routeCpp = readFile("src\\d3d11\\vr_world_route.cpp");
    check(!routeCpp.empty() &&
              count(routeCpp, "g_phase.beginFrame(true, g_prev.valid && g_f.treated, w, h, temporalJitterPhaseCount(w, h, w, h));") == 1 &&
              count(routeCpp, "g_win.phases = g_phase.phaseCount;") == 1 &&
              routeCpp.find("temporal_aa_jitter_follows_upscale\"") == std::string::npos,
          "phase count: the route's phase machine is given the ratio rule's count for render == output, its window prints what the machine ran, and the route reads no key for it");
    const std::string skip = functionBody(nt, "HRESULT WINAPI skipEye(");
    check(!skip.empty() && before(skip, "edvr::vrWorldRouteNoteSceneReset();", "s->history[eye]={}"),
          "scene reset: a withheld eye frame tells the route before it touches the eye's history");

    // THE STAGE 2 EXPERIMENT BUILD (design doc section 82). Three keys, read only while the route key is auto, and each handed to the
    // resolver as the resolver's own contract: the steady-detail key defaults to ON since flight 4, the debug view and the census to off;
    // with all three off the frame is flight 2's frame.
    {
        const std::string keys = functionBody(rt, "void readExperimentKeys(");
        check(!keys.empty() && keys.find("getString(\"experimental.temporal_aa_on_foot_world_steady_detail\", \"on\")") != std::string::npos &&
                  keys.find("getString(\"experimental.temporal_aa_on_foot_world_steady_detail\", \"off\")") == std::string::npos &&
                  keys.find("getString(\"advanced.temporal_aa_debug\", \"off\")") != std::string::npos &&
                  keys.find("\"motion_source\"") != std::string::npos && keys.find("vrWorldSteadyKeyFromText(") != std::string::npos,
              "experiment: the steady-detail key (default on, parsed by vrWorldSteadyKeyFromText so an explicit off or a typo reads off) and the view (the debug key's motion_source, default off) are read in one place");
        check(count(rt, "getString(\"experimental.temporal_aa_on_foot_world_steady_detail\"") == 1 && count(rt, "readExperimentKeys();") == 1,
              "experiment: the steady-detail key is read once, by readExperimentKeys, which the boundary calls once");
        const size_t callAt = boundary.find("readExperimentKeys();");
        const size_t guardAt = callAt == std::string::npos ? callAt : boundary.rfind("if (key == VrWorldKey::Auto) {", callAt);
        check(callAt != std::string::npos && guardAt != std::string::npos && callAt - guardAt < 80 &&
                  before(boundary, "if (key == VrWorldKey::Off && g_machine.state == VrWorldState::Off && !g_census && !g_injectorEngaged)", "readExperimentKeys();") &&
                  before(boundary, "readExperimentKeys();", "g_vrWorldWants = g_census ||"),
              "key off: the experiment keys are read only inside the key-auto branch, after the boundary's early return, before the frame that starts is armed");
        check(!treat.empty() && treat.find("f.steadyDetail = g_steadyKey == VrWorldSteadyKey::On;") != std::string::npos &&
                  treat.find("f.refusalCensus = g_census;") != std::string::npos && treat.find("f.refusalView = g_viewOn ? 1u : 0u;") != std::string::npos &&
                  count(rt, "f.steadyDetail") == 1 && count(rt, "f.refusalCensus") == 1 && count(rt, "f.refusalView") == 1 &&
                  before(treat, "f.reset = resetMissing", "f.steadyDetail = g_steadyKey") && before(treat, "f.steadyDetail = g_steadyKey", "flatMonoResolve(device.Get()"),
              "experiment: the resolver's stale-slot rule (the depth-validated form), census and view are set from the three keys in one place, before the resolve, and nowhere else (off is false, false, 0)");
        check(count(rt, "f.staticScene") == 0,
              "experiment: the route never sets the resolver's blanket form (staticScene is the flat 3D menu's alone): the key is the depth-checked form");
        check(boundary.find("flatMonoResolveTakeRefusalCensus()") != std::string::npos && count(rt, "flatMonoResolveTakeRefusalCensus()") == 1 &&
                  before(boundary, "vrWorldFormatInjectWindow(", "flatMonoResolveTakeRefusalCensus()") &&
                  before(boundary, "flatMonoResolveTakeRefusalCensus()", "vrWorldFormatRefusalWindow(") &&
                  before(boundary, "vrWorldFormatRefusalWindow(", "g_win.reset();") && boundary.find("if (g_census || rc.asked || rc.sampled || rc.frames || rc.checked || rc.skipped) {") != std::string::npos &&
                  boundary.find("rw.checked = rc.checked; rw.skipped = rc.skipped;") != std::string::npos,
              "experiment: the census's sums are taken once, in the 5 s window, and its line is printed only while the census key is on, samples are still in flight, or the steady-detail key's depth check counted frames");
        check(boundary.find("g_win.steady = vrWorldSteadyKeyName(g_steadyKey);") != std::string::npos &&
                  before(boundary, "g_win.steady = vrWorldSteadyKeyName(g_steadyKey);", "vrWorldFormatWindow("),
              "experiment: the route line names the steady-detail key's state");
        check(boundary.find("g_steadyKey = g_steadyReported = VrWorldSteadyKey::Off; g_viewOn = g_viewReported = false;") != std::string::npos &&
                  before(boundary, "g_steadyKey = g_steadyReported = VrWorldSteadyKey::Off;", "windDownInjector();"),
              "key off live: the experiment keys' state is cleared with the route's, so a later episode logs its state again");
        check(rt.find("VrWorldSteadyKey g_steadyKey = VrWorldSteadyKey::Off;") != std::string::npos && rt.find("bool g_viewOn = false, g_viewReported = false;") != std::string::npos,
              "experiment: both start off (nothing is relaxed or painted before a boundary has read the keys)");
        const std::string readInj = functionBody(rt, "VrWorldInjectFrame readInjectFrame(");
        check(!readInj.empty() && readInj.find("f.fovNarrowest = c.fovNarrowest; f.fovWidest = c.fovWidest;") != std::string::npos &&
                  count(rt, "vrWorldAddInjectFrame(g_win.inject, inj);") == 1,
              "weapon: the injector's field-of-view range reaches the window's inject line (its fov= token) through readInjectFrame, once a frame");
        check(treat.find("fpApplied = !fp.motion ? worldApplied") != std::string::npos &&
                  treat.find("((inj.firstPersonInjected > 0 && inj.firstPersonRefused == 0) ? worldApplied : VrWorldAppliedPhase{});") != std::string::npos,
              "weapon: a weapon drawn is credited the world's phase only when a first-person call was injected and none was refused (the injector's role, not a guess); otherwise mode 2 as before");
        const std::string cfgSource = readFile("edvr.ini");
        const size_t keyAt = cfgSource.find("\ntemporal_aa_on_foot_world_steady_detail = on");
        const size_t choicesAt = keyAt == std::string::npos ? keyAt : cfgSource.rfind("# dev: choices on, off", keyAt);
        check(keyAt != std::string::npos && choicesAt != std::string::npos && keyAt - choicesAt < 40,
              "experiment: the shipped edvr.ini ships the key on, with the developer tier's choices (the in-headset menu's Experimental page flips it live)");
    }
}

int runSelfTest() {
    keyCases();
    keyOffContract();
    machineCases();
    predicateCases();
    selectorCases();
    glueCases();
    triggerCases();
    lineCases();
    curveCases();
    stage2Cases();
    mapsGateCases();
    experimentCases();
    sourcePins();
    if (g_failures) {
        std::printf("vr world route: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("vr world route: PASS (%d checks)\n", g_checks);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--self-test") == 0) {
        if (argc >= 3) g_root = argv[2];
        return runSelfTest();
    }
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("vr world route test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: vr_world_route_test --self-test <repo root> | --dry-run\n");
    return 2;
}
