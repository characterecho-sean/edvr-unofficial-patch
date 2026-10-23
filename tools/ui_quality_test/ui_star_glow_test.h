// Included in ui_quality_test.cpp after ui_tone_scene_test.h (check; worldroute::readText, has, inOrder, countOf; afterui::squeeze).
//
// visual.star_glow (docs/star-glow-2026-10-22.md, "the uniform glow, built"). What these cases hold:
//   * the setting: any percentage snaps to one of the five choices, k = percent / 100 and m = k - 1;
//   * the pair table: each of the ten (vertex shader, pixel shader) pairs is its pass, a pair with the wrong pixel shader is nothing, the
//     hashes are the ones eye_draw_snapshot.h lists, and the bloom blur pair is a jet's add only when it writes an H candidate;
//   * the run machine, on draw lists transcribed from the flights' census: a B star's frame (edvr_gfx_20261022_080341), the neutron star's frame
//     with its jet (edvr_gfx_20261022_114920 #338-#449) and the white dwarf's (080341 #461-#476), the 256x256 probe copies, a frame with no
//     pre-pass, the drives' smoke ribbon after a star's run and straight after its arcs, a bloom trigger in the middle of a run, a jet's add without
//     its source or with a blend that is not ONE / ONE, a flare without its source, and a source from the frame before; and the live sequence
//     of the 10-22 flight (edvr_gfx_20261022_193420): the depth pre-pass drawn into a format-23 G-buffer target (not an H candidate) long
//     before the star run, hundreds of unrelated draws, then the run into H -- the pre-pass is a frame marker, set whatever its target and kept
//     apart from the run (the flown gate order refused every star pass no-pre-pass);
//   * the first level's input, as arithmetic: H' = H + HUD + m * S per channel under the format's clamp, each term once (S holds no HUD), the blend
//     that builds it, and the shader's text (one texture, no constants, no filter);
//   * the window line and the line for the setting going back to 100;
//   * the wiring, by source scan with controls: the module is in the DLL's source list, the draw scope keeps a star pass out of the offscreen fast
//     path and re-issues it between Begin and End, the first level asks for the star light without the HUD layer, the HUD feed and the star pass
//     are one command list on the deferred context, the copy of H is the HUD-less branch only, and the setting is read before any early return.
namespace starglowtest {

using worldroute::countOf;
using worldroute::has;
using worldroute::inOrder;
using worldroute::readText;

// ------------------------------------------------------------------ the setting

void testSetting() {
    check(flatStarGlowSnap(100) == 100 && flatStarGlowSnap(125) == 125 && flatStarGlowSnap(150) == 150 && flatStarGlowSnap(175) == 175 &&
              flatStarGlowSnap(200) == 200,
          "star glow: the five choices snap to themselves");
    check(flatStarGlowSnap(0) == 100 && flatStarGlowSnap(-40) == 100 && flatStarGlowSnap(99) == 100 && flatStarGlowSnap(112) == 100,
          "star glow: anything under 100, and up to 112, is the stock 100");
    check(flatStarGlowSnap(113) == 125 && flatStarGlowSnap(137) == 125 && flatStarGlowSnap(138) == 150 && flatStarGlowSnap(162) == 150 &&
              flatStarGlowSnap(163) == 175 && flatStarGlowSnap(187) == 175 && flatStarGlowSnap(188) == 200,
          "star glow: a number between two choices snaps to the nearer, halves up");
    check(flatStarGlowSnap(201) == 200 && flatStarGlowSnap(1000) == 200, "star glow: anything over 200 is 200");
    check(near1(flatStarGlowK(100), 1.0) && near1(flatStarGlowK(125), 1.25) && near1(flatStarGlowK(150), 1.5) && near1(flatStarGlowK(175), 1.75) &&
              near1(flatStarGlowK(200), 2.0),
          "star glow: k is the percentage over 100");
    check(near1(flatStarGlowM(100), 0.0) && near1(flatStarGlowM(125), 0.25) && near1(flatStarGlowM(150), 0.5) && near1(flatStarGlowM(175), 0.75) &&
              near1(flatStarGlowM(200), 1.0),
          "star glow: m is k - 1, the weight of the star layer");
    check(!flatStarGlowActive(100) && !flatStarGlowActive(99) && flatStarGlowActive(125) && flatStarGlowActive(200),
          "star glow: only above 100 does anything run");
}

// ------------------------------------------------------------------ the pairs

struct Pair {
    uint64_t vs, ps;
    FlatStarRole role;
    const char* name;
};
const Pair kPairs[] = {
    {kFlatStarPreVs, 0, FlatStarRole::kPre, "pre-pass"},
    {kFlatStarSurfaceVs, kFlatStarSurfacePs, FlatStarRole::kSurface, "surface"},
    {kFlatStarQuadAVs, kFlatStarQuadAPs, FlatStarRole::kQuadA, "quad A"},
    {kFlatStarInstancedVs, kFlatStarInstancedPs, FlatStarRole::kInstanced, "instanced quads"},
    {kFlatStarQuadBVs, kFlatStarQuadBPs, FlatStarRole::kQuadB, "quad B"},
    {kFlatStarRingVs, kFlatStarRingPs, FlatStarRole::kRing, "ring"},
    {kFlatStarArcsVs, kFlatStarArcsPs, FlatStarRole::kArcs, "arcs"},
    {kFlatStarJetSourceVs, kFlatStarJetSourcePs, FlatStarRole::kJetSource, "jet source"},
    {kFlatStarFlareVs, kFlatStarFlarePs, FlatStarRole::kFlare, "flare"},
};

bool fileHas64(const std::string& text, uint64_t v) {
    char lit[40];
    std::snprintf(lit, sizeof(lit), "0x%016llXull", static_cast<unsigned long long>(v));
    return text.find(lit) != std::string::npos;
}

void testPairs() {
    for (const Pair& p : kPairs) {
        char msg[120];
        std::snprintf(msg, sizeof(msg), "star glow: the %s pair is its pass, whatever the target", p.name);
        check(flatStarRoleOf(p.vs, p.ps, true) == p.role && flatStarRoleOf(p.vs, p.ps, false) == p.role, msg);
        std::snprintf(msg, sizeof(msg), "star glow: the %s vertex shader with another pixel shader is nothing", p.name);
        check(flatStarRoleOf(p.vs, p.ps ^ 1ull, true) == FlatStarRole::kNone, msg);
    }
    check(flatStarRoleOf(kFlatStarPreVs, kFlatStarSurfacePs, true) == FlatStarRole::kNone, "star glow: the pre-pass has no pixel shader (a pixel shader makes it another draw)");
    check(flatStarRoleOf(0, 0, true) == FlatStarRole::kNone && flatStarRoleOf(kFlatStarSurfacePs, kFlatStarSurfaceVs, true) == FlatStarRole::kNone,
          "star glow: no shader, and a swapped pair, are nothing");
    // The bloom blur pair (CFA91824 with DE360261) is the chain's ordinary pass into depth-less targets all frame: a jet's add only into an H candidate.
    check(flatStarRoleOf(kFlatStarJetAddVs, kFlatStarJetAddPs, true) == FlatStarRole::kJet, "star glow: the bloom blur pair writing an H candidate is a jet's add");
    check(flatStarRoleOf(kFlatStarJetAddVs, kFlatStarJetAddPs, false) == FlatStarRole::kNone,
          "star glow: the same pair into any other target is the bloom chain's own blur, not a star pass");
    check(flatStarRoleOf(kFlatStarJetAddVs, 0x30F73C5E4A94DB52ull, true) == FlatStarRole::kNone, "star glow: the chain's first blur pair (30F73C5E) is not the jet's add");
    // Slots: nine counted, the jet's source has none, and every slot has a name.
    check(flatStarSlot(FlatStarRole::kPre) == 0 && flatStarSlot(FlatStarRole::kFlare) == 8 && flatStarSlot(FlatStarRole::kJet) == 7 &&
              flatStarSlot(FlatStarRole::kJetSource) == -1 && flatStarSlot(FlatStarRole::kNone) == -1,
          "star glow: nine slots in the line's order; the jet's source and nothing have none");
    for (size_t i = 0; i < kFlatStarSlots; ++i) check(std::strcmp(flatStarSlotName(i), "?") != 0, "star glow: every slot has a name");
    check(!flatStarRoleReplays(FlatStarRole::kNone) && !flatStarRoleReplays(FlatStarRole::kPre) && !flatStarRoleReplays(FlatStarRole::kJetSource) &&
              flatStarRoleReplays(FlatStarRole::kSurface) && flatStarRoleReplays(FlatStarRole::kJet) && flatStarRoleReplays(FlatStarRole::kFlare),
          "star glow: the pre-pass and the jet's source are evidence, never replayed");
    check(flatStarRoleNeedsBlend(FlatStarRole::kJet) && !flatStarRoleNeedsBlend(FlatStarRole::kRing) && !flatStarRoleNeedsBlend(FlatStarRole::kSurface),
          "star glow: only the jet's add asks for the blend");
    // The hashes are the ones the draw snapshot lists as the solar draws.
    const std::string snap = readText("src/d3d11/eye_draw_snapshot.h");
    check(!snap.empty(), "src/d3d11/eye_draw_snapshot.h is readable from the working directory");
    if (!snap.empty()) {
        bool all = true;
        for (const Pair& p : kPairs) {
            if (p.role == FlatStarRole::kJetSource || p.role == FlatStarRole::kFlare) continue;   // the jet chain is the 10-22 neutron star flight's, not the snapshot's
            all = all && fileHas64(snap, p.vs) && (p.ps == 0 || fileHas64(snap, p.ps));
        }
        check(all, "star glow: the pre-pass, surface, quads, ring and arcs hashes are eye_draw_snapshot.h's solarDraw / solarPixel lists");
    }
}

// ------------------------------------------------------------------ the run machine

struct D {
    uint64_t vs = 0, ps = 0;
    bool cand = false;        // writes an H candidate
    bool oneOne = false;      // the bound blend is ONE / ONE (read by the module only for the jet's add)
    bool trigger = false;     // this draw is the bloom trigger (not a star pass)
};
constexpr uint64_t kOtherVs = 0x9BFC7FD232328391ull, kOtherPs = 0xDBF1725726018F52ull;   // 9BFC7FD2: the draw after the arcs that no pair names
constexpr uint64_t kBlurFirstPs = 0x30F73C5E4A94DB52ull;

D pre(bool cand = true) { return {kFlatStarPreVs, 0, cand}; }
D preG() { return pre(false); }   // the live flight's: drawn in the G-buffer phase into a format-23 target, never an H candidate
D surf(bool cand = true) { return {kFlatStarSurfaceVs, kFlatStarSurfacePs, cand}; }
D qa(bool cand = true) { return {kFlatStarQuadAVs, kFlatStarQuadAPs, cand}; }
D inst(bool cand = true) { return {kFlatStarInstancedVs, kFlatStarInstancedPs, cand}; }
D qb(bool cand = true) { return {kFlatStarQuadBVs, kFlatStarQuadBPs, cand}; }
D ring(bool cand = true) { return {kFlatStarRingVs, kFlatStarRingPs, cand}; }
D arcs(bool cand = true) { return {kFlatStarArcsVs, kFlatStarArcsPs, cand}; }
D jetSrc() { return {kFlatStarJetSourceVs, kFlatStarJetSourcePs, false}; }   // a depth-less target of its own
D jetAdd(bool cand = true, bool oneOne = true) { return {kFlatStarJetAddVs, kFlatStarJetAddPs, cand, oneOne}; }
D flare(bool cand = true) { return {kFlatStarFlareVs, kFlatStarFlarePs, cand}; }
D other(bool cand) { return {kOtherVs, kOtherPs, cand}; }
D blur(bool second) { return {kFlatStarJetAddVs, second ? kFlatStarJetAddPs : kBlurFirstPs, false}; }
D trigger() { D d; d.vs = 0xDFED8E1C00000000ull; d.ps = 0x143AAE0500000000ull; d.trigger = true; return d; }

struct Run {
    FlatStarWindow win;
    std::vector<FlatStarVerdict> verdicts;
    FlatStarFrame frame;
};

// The scope's per-draw sequence (flat_star_glow.cpp flatStarGlowObserve): the pair's pass, the jet's blend read only after the cheaper gates, then the
// decision; a pass allowed is counted as replayed (the GPU half's bracket then binds it).
Run runFrame(const std::vector<D>& draws) {
    Run r;
    flatStarBeginFrame(r.frame);
    bool triggered = false;
    for (const D& d : draws) {
        if (d.trigger) triggered = true;
        const FlatStarRole role = flatStarRoleOf(d.vs, d.ps, d.cand);
        FlatStarFacts facts;
        facts.hCandidate = d.cand;
        facts.triggered = triggered;
        if (flatStarRoleNeedsBlend(role) && d.cand && !triggered && r.frame.pre && r.frame.jetSource) facts.blendOneOne = d.oneOne;
        const FlatStarVerdict v = flatStarDecide(r.frame, role, facts);
        flatStarCountVerdict(r.win, v);
        if (v.replay) flatStarCountReplayed(r.win, v.role);
        r.verdicts.push_back(v);
    }
    if (r.frame.anyStar) ++r.win.starFrames;
    return r;
}
uint64_t refused(const Run& r, FlatStarRefusal why) { return r.win.refused[static_cast<int>(why) - 1]; }
uint64_t totalRefused(const Run& r) {
    uint64_t n = 0;
    for (uint64_t v : r.win.refused) n += v;
    return n;
}
bool replayedIs(const Run& r, std::initializer_list<uint64_t> want) {
    size_t i = 0;
    for (uint64_t v : want) {
        if (i >= kFlatStarSlots || r.win.replayed[i++] != v) return false;
    }
    return i == kFlatStarSlots;
}

void testRuns() {
    // A B star's frame (edvr_gfx_20261022_080341, #439-#558): the pre-pass early, the starfield and other draws between, the surface, the probe's 240x150
    // copies of the quads (not H candidates), then quad A, the instanced quads, quad B, the ring and two arcs draws, and the draw no pair names.
    {
        const Run r = runFrame({pre(), other(true), other(true), surf(), other(false), other(false), other(false), qa(), inst(), qb(), ring(), arcs(), arcs(), other(true)});
        check(replayedIs(r, {0, 1, 1, 1, 1, 1, 2, 0, 0}), "star glow: a B star's frame replays the surface, both quads, the instanced quads, the ring and both arcs draws");
        check(r.win.seen[0] == 1 && r.win.seen[1] == 1 && r.win.seen[6] == 2 && r.win.starFrames == 1 && totalRefused(r) == 0,
              "star glow: ...every pass seen once (arcs twice), one star frame, and nothing refused");
        check(r.frame.anyStar && !r.frame.anyReplay, "star glow: a decision alone does not make S live: only a bound replay does (a failed bind would feed last frame's S)");
    }
    // The 256x256 probe copies of the same pairs, drawn into targets that are not H candidates, are seen and refused for their target.
    {
        const Run r = runFrame({pre(), surf(), qa(), qb(), ring(), arcs(),
                                surf(false), qa(false), inst(false), qb(false), ring(false), pre(false)});
        check(replayedIs(r, {0, 1, 1, 0, 1, 1, 1, 0, 0}), "star glow: the probe copies are not replayed");
        check(refused(r, FlatStarRefusal::kNotHCandidate) == 5 && totalRefused(r) == 5, "star glow: ...each is refused as not an H candidate, and for nothing else");
        check(r.win.seen[0] == 2, "star glow: a probe pre-pass is seen like any pass (and marks the frame, as every pre-pass does)");
    }
    // The pre-pass is the frame's marker whatever target it is drawn into; it is never replayed and never refused.
    {
        const Run r = runFrame({preG(), surf(), qa(), qb(), ring(), arcs()});
        check(replayedIs(r, {0, 1, 1, 0, 1, 1, 1, 0, 0}) && totalRefused(r) == 0,
              "star glow: a pre-pass into a target that is not an H candidate marks the frame: the passes after it are replayed");
        const Run both = runFrame({preG(), pre(), preG()});
        check(both.win.seen[0] == 3 && both.win.replayed[0] == 0 && totalRefused(both) == 0 && both.frame.pre,
              "star glow: a pre-pass, into an H candidate or not, is seen, never replayed and never refused");
    }
    // THE LIVE SEQUENCE (edvr_gfx_20261022_193420): the pre-pass in the G-buffer phase, then hundreds of unrelated draws -- into the G-buffer, into
    // H, into other targets -- then the star run into H, the 256x256 probe copies after it.
    {
        std::vector<D> live;
        live.push_back(preG());
        for (int i = 0; i < 400; ++i) live.push_back(other(i % 3 == 0));   // a third of them into an H candidate
        for (D d : {surf(), qa(), inst(), qb(), ring(), arcs(), arcs()}) live.push_back(d);
        for (D d : {surf(false), qa(false), inst(false), qb(false), ring(false), arcs(false), pre(false)}) live.push_back(d);
        live.push_back(other(true));
        const Run r = runFrame(live);
        check(replayedIs(r, {0, 1, 1, 1, 1, 1, 2, 0, 0}) && refused(r, FlatStarRefusal::kNoPrePass) == 0,
              "star glow: the live sequence replays the star run: the pre-pass mark survives hundreds of unrelated draws, H-candidate ones included");
        check(refused(r, FlatStarRefusal::kNotHCandidate) == 6 && totalRefused(r) == 6, "star glow: ...and the probe copies after it are the only refusals");
        // The run is another state: unrelated draws into H between a quad and the ring end the run, and leave the mark.
        std::vector<D> cut;
        cut.push_back(preG());
        for (int i = 0; i < 50; ++i) cut.push_back(other(true));
        for (D d : {qa(), other(true), ring()}) cut.push_back(d);
        const Run c = runFrame(cut);
        check(c.win.replayed[2] == 1 && c.win.replayed[5] == 0 && refused(c, FlatStarRefusal::kRingNotInRun) == 1 && refused(c, FlatStarRefusal::kNoPrePass) == 0,
              "star glow: an unrelated H draw between the quad and the ring ends the run (ring-not-in-run) and does not clear the frame's pre-pass mark");
        // The frame's start clears the mark.
        const Run nextFrame = runFrame({qa(), qb(), ring()});
        check(nextFrame.win.replayed[2] == 0 && refused(nextFrame, FlatStarRefusal::kNoPrePass) == 3, "star glow: the mark is the frame's: a frame with no pre-pass refuses its star passes");
        // The jet chain of the live frame: the pre-pass from the G-buffer phase, the source into its own target, the add into H.
        const Run jet = runFrame({preG(), other(true), jetSrc(), jetSrc(), blur(false), blur(true), qa(), qb(), ring(), jetAdd(), flare()});
        check(jet.win.replayed[7] == 1 && jet.win.replayed[8] == 1 && totalRefused(jet) == 0, "star glow: the jet's add and flare replay behind a G-buffer-phase pre-pass");
    }
    // Pairs with no pre-pass at all (a draw the pairs share with another effect).
    {
        const Run r = runFrame({qa(), qb(), ring(), arcs(), surf()});
        check(replayedIs(r, {0, 0, 0, 0, 0, 0, 0, 0, 0}) && refused(r, FlatStarRefusal::kNoPrePass) == 5, "star glow: no pre-pass in the frame, no replay");
    }
    // The bloom trigger in the middle of a run: what comes after it cannot reach the first level.
    {
        const Run r = runFrame({pre(), surf(), qa(), trigger(), qb(), ring(), arcs()});
        check(replayedIs(r, {0, 1, 1, 0, 0, 0, 0, 0, 0}) && refused(r, FlatStarRefusal::kAfterTrigger) == 3 && totalRefused(r) == 3,
              "star glow: the passes after the bloom trigger are refused as after-trigger, those before it are replayed");
    }
    // The ring pair is the drives' smoke ribbon too (ui_depth.cpp): a ring replays only straight after a quad of the same run.
    {
        const Run lone = runFrame({pre(), other(true), ring()});
        check(replayedIs(lone, {0, 0, 0, 0, 0, 0, 0, 0, 0}) && refused(lone, FlatStarRefusal::kRingNotInRun) == 1, "star glow: a ring after an unrelated draw is refused as not in a run");
        const Run afterStar = runFrame({pre(), surf(), qa(), qb(), ring(), arcs(), other(true), ring()});
        check(replayedIs(afterStar, {0, 1, 1, 0, 1, 1, 1, 0, 0}) && refused(afterStar, FlatStarRefusal::kRingNotInRun) == 1,
              "star glow: a smoke ribbon drawn after a star's run (another draw between) is refused; the star's own ring replays");
        const Run afterArcs = runFrame({pre(), surf(), qa(), qb(), ring(), arcs(), ring()});
        check(refused(afterArcs, FlatStarRefusal::kRingNotInRun) == 1 && afterArcs.win.replayed[5] == 1, "star glow: a ring straight after the arcs is not a star's ring");
        const Run twice = runFrame({pre(), qa(), ring(), ring()});
        check(refused(twice, FlatStarRefusal::kRingNotInRun) == 1 && twice.win.replayed[5] == 1, "star glow: a second ring straight after the first is refused");
        const Run afterSurface = runFrame({pre(), surf(), ring()});
        check(refused(afterSurface, FlatStarRefusal::kRingNotInRun) == 1, "star glow: a ring straight after the surface, with no quad, is refused");
        const Run afterInst = runFrame({pre(), inst(), ring()});
        check(afterInst.win.replayed[5] == 1, "star glow: a ring after the instanced quads is a star's");
        const Run afterA = runFrame({pre(), qa(), ring()});
        check(afterA.win.replayed[5] == 1, "star glow: a ring after quad A alone is a star's");
        const Run probeBetween = runFrame({pre(), qb(), other(false), other(false), ring()});
        check(probeBetween.win.replayed[5] == 1, "star glow: draws into other targets between the quad and the ring (the probes) do not end the run");
        const Run noPre = runFrame({other(true), ring()});
        check(refused(noPre, FlatStarRefusal::kNoPrePass) == 1, "star glow: a drive trail in a frame with no star is refused for want of a pre-pass (a ring replays 0)");
    }
    // The neutron star's frame (edvr_gfx_20261022_114920 census 2 #338-#449): the pre-pass, the jet's source into a depth-less target, the chain's own
    // blur passes, the quads and the ring, the jet's add into H (ONE / ONE) and the flare.
    {
        const Run r = runFrame({pre(), other(true), jetSrc(), jetSrc(), blur(false), blur(true), qa(), qb(), ring(), jetAdd(), flare(), other(true)});
        check(replayedIs(r, {0, 0, 1, 0, 1, 1, 0, 1, 1}), "star glow: the neutron star's frame replays both quads, the ring, the jet's add and the flare");
        check(totalRefused(r) == 0 && r.win.seen[7] == 1 && r.win.seen[8] == 1, "star glow: ...and refuses nothing (the chain's own blur passes into depth-less targets are not seen)");
    }
    // The white dwarf's frame carries the same chain (080341 #461-#476): its jets are drawn and replayed too.
    {
        const Run r = runFrame({pre(), jetSrc(), jetSrc(), blur(false), blur(true), jetAdd(), flare()});
        check(r.win.replayed[7] == 1 && r.win.replayed[8] == 1 && totalRefused(r) == 0,
              "star glow: a white dwarf's frame (the same jet chain in the 080341 log) replays the jet's add and flare, as it should: white dwarfs have jets");
    }
    // The ordinary bloom chain with no star and no jet: nothing to see.
    {
        const Run r = runFrame({other(true), blur(false), blur(true), other(true), blur(true)});
        check(r.win.starFrames == 0 && totalRefused(r) == 0 && replayedIs(r, {0, 0, 0, 0, 0, 0, 0, 0, 0}), "star glow: the bloom chain's blur passes alone are not a star frame");
    }
    // The jet's gates.
    {
        const Run noSource = runFrame({pre(), blur(true), jetAdd(), flare()});
        check(noSource.win.replayed[7] == 0 && noSource.win.replayed[8] == 0 && refused(noSource, FlatStarRefusal::kJetNoSource) == 2,
              "star glow: the jet's add and the flare with no source draw earlier are refused (jet-no-source)");
        const Run blendOff = runFrame({pre(), jetSrc(), jetAdd(true, false), flare()});
        check(blendOff.win.replayed[7] == 0 && refused(blendOff, FlatStarRefusal::kJetNoSource) == 1 && blendOff.win.replayed[8] == 1,
              "star glow: an add whose blend is not ONE / ONE is refused; the flare, which needs only the source, still replays");
        // The machine itself holds the source gate, whatever the blend read says (the module reads the blend only once a source is known).
        FlatStarFrame bare;
        flatStarBeginFrame(bare);
        FlatStarFacts both;
        both.hCandidate = true;
        both.blendOneOne = true;
        flatStarDecide(bare, FlatStarRole::kPre, both);
        const FlatStarVerdict direct = flatStarDecide(bare, FlatStarRole::kJet, both);
        check(!direct.replay && direct.refusal == FlatStarRefusal::kJetNoSource, "star glow: an add with ONE / ONE and no source draw is refused by the machine itself");
        flatStarDecide(bare, FlatStarRole::kJetSource, both);
        check(flatStarDecide(bare, FlatStarRole::kJet, both).replay, "star glow: ...and replays once the source is drawn");
        const Run late = runFrame({pre(), jetAdd(), jetSrc(), flare()});
        check(late.win.replayed[7] == 0 && late.win.replayed[8] == 1, "star glow: a source drawn after the add is no source for it");
        const Run noPre = runFrame({jetSrc(), jetAdd(), flare()});
        check(noPre.win.replayed[7] == 0 && refused(noPre, FlatStarRefusal::kNoPrePass) == 2, "star glow: a jet with no star pre-pass in the frame is refused (no-pre-pass)");
        const Run afterTrigger = runFrame({pre(), jetSrc(), trigger(), jetAdd(), flare()});
        check(refused(afterTrigger, FlatStarRefusal::kAfterTrigger) == 2, "star glow: a jet after the bloom trigger is refused");
        const Run probeAdd = runFrame({pre(), jetSrc(), jetAdd(false)});
        check(probeAdd.win.seen[7] == 0 && totalRefused(probeAdd) == 0, "star glow: the blur pair into a non-candidate target is the chain's own pass, neither seen nor refused");
        // A source from the frame before does not carry over.
        const Run first = runFrame({pre(), jetSrc()});
        const Run second = runFrame({pre(), jetAdd()});
        check(first.frame.jetSource && second.win.replayed[7] == 0 && refused(second, FlatStarRefusal::kJetNoSource) == 1,
              "star glow: the frame's start clears the jet's source");
    }
    // The pre-pass: only an H-candidate one counts, and it ends nothing; the surface alone needs no run.
    {
        const Run r = runFrame({pre(), surf(), surf()});
        check(r.win.replayed[1] == 2, "star glow: two surfaces (two stars) replay");
        const Run s2 = runFrame({other(true), pre(), surf()});
        check(s2.win.replayed[1] == 1, "star glow: the pre-pass may follow other draws");
    }
    // A non-star draw into an H candidate ends the run; one into another target does not.
    {
        FlatStarFrame f;
        flatStarBeginFrame(f);
        FlatStarFacts cand;
        cand.hCandidate = true;
        flatStarDecide(f, FlatStarRole::kPre, cand);
        flatStarDecide(f, FlatStarRole::kQuadA, cand);
        check(f.prev == FlatStarRole::kQuadA, "star glow: a quad is the run's last pass");
        flatStarNoteOther(f, false);
        check(f.prev == FlatStarRole::kQuadA, "star glow: a draw into another target leaves the run");
        flatStarNoteOther(f, true);
        check(f.prev == FlatStarRole::kNone, "star glow: a draw into an H candidate ends it");
    }
}

// ------------------------------------------------------------------ the first level's input

bool nearf(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

void testComposition() {
    const int percents[] = {100, 125, 150, 175, 200};
    const float hs[] = {0.0f, 0.37f, 12.0f, 2500.0f};
    const float huds[] = {0.0f, 0.5f, 40.0f};
    const float ss[] = {0.0f, 1.0f, 164.9f, 528.1f};
    bool exact = true, noHudInS = true;
    for (int pct : percents) {
        const double m = flatStarGlowM(pct);
        for (float h : hs) {
            for (float hud : huds) {
                for (float s : ss) {
                    const double want = static_cast<double>(h) + hud + m * s;
                    for (int c = 0; c < 3; ++c) exact = exact && nearf(flatStarFirstLevel(h, hud, s, m, c), want, 1e-3 + want * 1e-6);
                    // S holds the star draws alone: the HUD joins once (the HUD feed's pass), and S adds m of itself, no HUD.
                    noHudInS = noHudInS && nearf(flatStarFirstLevel(h, hud, s, m, 0) - flatStarFirstLevel(h, 0.0f, s, m, 0), hud, 1e-3 + hud * 1e-6);
                }
            }
        }
    }
    check(exact, "star glow: H' = H + HUD + m * S, per channel, at every setting");
    check(noHudInS, "star glow: the HUD enters once, whatever the star light is (S holds no HUD)");
    // 100 adds nothing; the setting's k is the halo's strength: at S = H the star pixel reads k times H.
    check(nearf(flatStarFirstLevel(300.0f, 0.0f, 300.0f, flatStarGlowM(100), 0), 300.0, 1e-3), "star glow: at 100 H' is H");
    check(nearf(flatStarFirstLevel(300.0f, 0.0f, 300.0f, flatStarGlowM(150), 0), 450.0, 1e-3) &&
              nearf(flatStarFirstLevel(300.0f, 0.0f, 300.0f, flatStarGlowM(200), 0), 600.0, 1e-3),
          "star glow: where the star is all of H, H' reads k times H (150: 1.5, 200: 2)");
    // Away from a star H' is H: S is zero there.
    check(nearf(flatStarFirstLevel(80.0f, 0.0f, 0.0f, flatStarGlowM(200), 1), 80.0, 1e-4), "star glow: away from a star H' is H");
    // The format's clamp.
    check(flatStarFirstLevel(65000.0f, 0.0f, 60000.0f, 1.0, 0) == kFlatStarR11Max && flatStarFirstLevel(64000.0f, 0.0f, 60000.0f, 1.0, 2) == kFlatStarB10Max,
          "star glow: R11G11B10_FLOAT tops out at 65024 (red, green) and 64512 (blue)");
    check(flatStarFirstLevel(-5.0f, 0.0f, 0.0f, 0.5, 0) == 0.0f, "star glow: the format stores no negative");
    check(flatStarSanitise(std::numeric_limits<float>::quiet_NaN()) == 0.0f && flatStarSanitise(std::numeric_limits<float>::infinity()) == 0.0f &&
              flatStarSanitise(-3.0f) == 0.0f && flatStarSanitise(1.0e9f) == 60000.0f && flatStarSanitise(7.5f) == 7.5f,
          "star glow: the pass reads a non-finite or negative texel as zero and caps the rest at 60000, as the HUD feed does");
    check(flatStarFirstLevel(10.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0, 0) == 10.0f, "star glow: a NaN texel of S cannot poison H'");
    // The blend that builds it: Src * BLEND_FACTOR + Dst * ONE, the factor (m, m, m, 0).
    check(kFlatStarBlendSrc == 14 && kFlatStarBlendDst == 2 && kFlatStarBlendOp == 1 && kFlatStarBlendSrcAlpha == 1 && kFlatStarBlendDstAlpha == 2,
          "star glow: the pass blends with source BLEND_FACTOR, destination ONE, ADD (alpha: zero, one)");
    bool blendIsFirstLevel = true;
    for (int pct : percents) {
        const double m = flatStarGlowM(pct);
        for (float s : ss)
            blendIsFirstLevel = blendIsFirstLevel && nearf(flatStarBlendRgb(flatStarSanitise(s), 123.0f, static_cast<float>(m)), 123.0 + m * s, 1e-3);
    }
    check(blendIsFirstLevel, "star glow: the blend equation evaluated on the sanitised texel is the first level's m * S added to the HUD feed's texture");
}

// The star pass's shader, as text: one texture at t0, Load only, no constants, no sampler, non-finite texels read zero, capped at 60000.
bool starShaderOk(const std::string& hlsl) {
    const auto has2 = [&](const char* piece) { return hlsl.find(piece) != std::string::npos; };
    return has2("Texture2D<float4> Star : register(t0)") && has2(".Load(int3(p.xy, 0))") && has2("isfinite(c)") && has2("float3(60000, 60000, 60000)") &&
           !has2("cbuffer") && !has2("ConstantBuffer") && !has2("register(b") && !has2("register(t1)") && !has2("register(s") && !has2("SamplerState") &&
           !has2(".Sample") && !has2("[loop]");
}
void testShaderText() {
    const std::string real = kStarGlowAddPsHlsl;
    // A copy of the shader with one piece of its text replaced (unchanged when the piece is not there: the real scan then speaks first).
    const auto edited = [&](const char* from, const char* to) {
        std::string t = real;
        const size_t at = t.find(from);
        if (at != std::string::npos) t.replace(at, std::strlen(from), to);
        return t;
    };
    check(starShaderOk(real), "star glow: the pass reads one texture at t0 with Load, no constant buffer, no sampler, finite and capped");
    check(!starShaderOk(real + "\ncbuffer C : register(b0) { float m; };"), "control: a constant buffer in the star pass trips the scan");
    check(!starShaderOk(edited(".Load(int3(p.xy, 0))", ".Sample(smp, p.xy)")), "control: a filtered read in the star pass trips the scan");
    check(!starShaderOk(edited("all(isfinite(c)) ? ", "true ? ")), "control: dropping the non-finite guard trips the scan");
    check(!starShaderOk(edited("float4 main", "Texture2D<float4> Hud : register(t1);\nfloat4 main")), "control: a second texture in the star pass trips the scan");
    check(!starShaderOk(edited("float3(60000, 60000, 60000)", "float3(1e30, 1e30, 1e30)")), "control: dropping the cap at 60000 trips the scan");
}

// ------------------------------------------------------------------ the lines

void testLines() {
    FlatStarWindow w;
    char line[1024];
    int n = flatStarWindowLine(line, sizeof(line), 150, w);
    check(n > 0 && std::strcmp(line, "flat star glow: window=5s setting=150 k=1.50 star-frames=0 seen[pre,surf,qa,inst,qb,ring,arcs,jet,flare]=0,0,0,0,0,0,0,0,0 "
                                      "replayed[pre,surf,qa,inst,qb,ring,arcs,jet,flare]=0,0,0,0,0,0,0,0,0 layer-built=0 asked=0 fed=0 "
                                      "refused[not-h-candidate,after-trigger,no-pre-pass,ring-not-in-run,jet-no-source,size-mismatch,state-fault,layer-fail,"
                                      "depth-clone-fail,bloom-off]=0,0,0,0,0,0,0,0,0,0") == 0 &&
              n == static_cast<int>(std::strlen(line)),
          "star glow: the window line with every count zero is the format the log shows (a setting above 100 whose scope never matched)");
    const Run r = runFrame({pre(), surf(), qa(), qb(), ring(), arcs(), arcs(), surf(false)});
    w = r.win;
    w.layerBuilt = 1;
    w.asked = 1;
    w.fed = 1;
    flatStarCountRefusal(w, FlatStarRefusal::kBloomOff);
    n = flatStarWindowLine(line, sizeof(line), 200, w);
    check(std::strstr(line, "setting=200 k=2.00 star-frames=1 seen[pre,surf,qa,inst,qb,ring,arcs,jet,flare]=1,2,1,0,1,1,2,0,0 ") != nullptr &&
              std::strstr(line, "replayed[pre,surf,qa,inst,qb,ring,arcs,jet,flare]=0,1,1,0,1,1,2,0,0 layer-built=1 asked=1 fed=1 ") != nullptr &&
              std::strstr(line, "]=1,0,0,0,0,0,0,0,0,1") != nullptr,
          "star glow: the counts print in their slots (seen, replayed, the window's layer, ask and feed, the refusals by name)");
    char tiny[40];
    n = flatStarWindowLine(tiny, sizeof(tiny), 125, w);
    check(n > 0 && n < static_cast<int>(sizeof(tiny)) && std::strlen(tiny) == static_cast<size_t>(n), "star glow: a short buffer cuts the line and stays terminated");
    n = flatStarOffLine(line, sizeof(line), 150);
    check(n > 0 && std::strstr(line, "flat star glow: off (setting=100, was 150)") == line, "star glow: the line for the setting returning to 100");
    for (size_t i = 1; i <= kFlatStarRefusals; ++i)
        check(std::strcmp(flatStarRefusalName(static_cast<FlatStarRefusal>(i)), "?") != 0, "star glow: every refusal has a name");
}

// ------------------------------------------------------------------ the wiring

struct Scan {
    const char* id;
    bool ok;
    const char* what;
};

std::vector<Scan> wiringPins(const std::string& runtimeCpp, const std::string& runtimeH, const std::string& uiLayerCpp, const std::string& flatMath,
                             const std::string& buildBat, const std::string& starCpp) {
    std::vector<Scan> pins;
    const std::string cpp = afterui::squeeze(runtimeCpp), hdr = afterui::squeeze(runtimeH), ui = afterui::squeeze(uiLayerCpp), math = afterui::squeeze(flatMath),
                      mod = afterui::squeeze(starCpp);
    pins.push_back({"in-dll", has(buildBat, "\"src\\d3d11\\flat_star_glow.cpp\""), "build.bat compiles src\\d3d11\\flat_star_glow.cpp into the DLL"});
    pins.push_back({"fast-path", has(cpp, "&&cameraProbeAttempt==0&&!starTouched;"),
                    "a star pass of any target is kept out of the offscreen fast path (starTouched is a term of `plain`)"});
    pins.push_back({"observe-gated",
                    inOrder(cpp, {"if(flatStarGlowOn()){", "starReplay=flatStarGlowObserve(ctx,star,&starTouched,&starRole);", "if(hdrTrigger){",
                                  "constboolstarLive=flatStarGlowLive();", "flatUiGlowWanted(t.srvKnown,t.ambiguous,t.srvSlot,t.hdrWidth,t.hdrHeight,t.targetWidth,"
                                  "t.targetHeight,starLive)", "starFeed=starLive;", "flatStarGlowNoteNoBloom();"}),
                    "the scope asks the module about each watched draw only while the setting is above 100, before the bloom trigger's own block, which asks "
                    "for the star light with starLive and counts bloom-off when the trigger is not the first level"});
    pins.push_back({"after-trigger-fact", has(cpp, "star.triggered=s.hdr.triggered;") && has(cpp, "star.hCandidate=flatHdrCandidateDraw(k,s.hdr.outputWidth,s.hdr.outputHeight);"),
                    "the facts handed over are the HDR route's trigger and flatHdrCandidateDraw at the frame's output size"});
    pins.push_back({"reissue",
                    inOrder(hdr, {"if(uiTaken&&flatUiLayerWriteBackBegin(ctx)){issue();flatUiLayerWriteBackEnd(ctx);}",
                                  "if(starReplay&&flatStarGlowReplayBegin(ctx,starRole,starW,starH)){issue();flatStarGlowReplayEnd(ctx);}"}),
                    "uiReissue issues the star pass between flatStarGlowReplayBegin and End, after the HUD write-back and with the hook's own call"});
    pins.push_back({"feed-input",
                    inOrder(cpp, {"if(uiGlow&&!glowOriginal){", "UiGlowStarstar;", "constboolhaveStar=starFeed&&flatStarGlowFeedInput(&star);", "flatUiGlowBegin(",
                                  "&glowOriginal,haveStar?&star:nullptr);", "if(haveStar)flatStarGlowNoteFeed("}),
                    "the first-level draw's Begin hands the star layer to the feed when the trigger asked for it, and reports what the pass came to"});
    pins.push_back({"restore", has(cpp, "if(glowOriginal){flatUiGlowEnd(ctx,glowSlot,glowOriginal);glowOriginal=nullptr;}") &&
                                   countOf(cpp, "if(glowOriginal){flatUiGlowEnd(ctx,glowSlot,glowOriginal);glowOriginal=nullptr;}") == 2,
                    "the game's view of H goes back after the first-level draw, at endActualDraw and in the destructor: only that draw reads H'"});
    pins.push_back({"present-first",
                    inOrder(cpp, {"voidflatRuntimePresent(IDXGISwapChain*swap,uint64_tframe,HRESULThr,UINTflags){", "if(s.thread&&!owner())return;",
                                  "flatStarGlowFrame(frame+1,s.frameLive);", "s.untrusted.pollMask("}) &&
                        !has(cpp, "flatStarGlowFrame(frame+1,flatRuntimeActive())"),
                    "the setting, the frame's state and the 5 s line are read at the top of Present, before any early return, and the frame that just ended is "
                    "called watched by State::frameLive (g_flatRuntimeLive, which flatRuntimeActive reads, is cleared before Present)"});
    pins.push_back({"release", has(cpp, "flatUiLayerRelease();flatStarGlowRelease();") && has(cpp, "flatStarGlowNoteDevice(s.device.Get());"),
                    "a resize releases the star layer and a new device releases its children"});
    // The first level's pass: the HUD pass, then (or after a copy of H, when no HUD is live) the star pass, one command list.
    pins.push_back({"pass-order",
                    inOrder(ui, {"voidcompileGlowFeedOnce", "boolstarAddReady(", "dc->PSSetShader(g_glowPs,nullptr,0);", "dc->Draw(3,0);", "}else{",
                                 "dc->CopyResource(g_glow.tex.Get(),hTex.Get());", "}", "if(use){", "dc->PSSetShader(g_starAddPs,nullptr,0);",
                                 "dc->OMSetBlendState(g_starAddBlend.Get(),factor,0xFFFFFFFFu);", "dc->Draw(3,0);", "dc->FinishCommandList(FALSE,&list)",
                                 "vScreenExecuteCommandListRaw(ctx,list.Get(),1);"}) &&
                        countOf(ui, "dc->CopyResource(g_glow.tex.Get(),hTex.Get());") == 1 && countOf(ui, "dc->FinishCommandList(FALSE,&list)") >= 1,
                    "the HUD's pass, else a copy of H (only without a HUD), then the star pass with the game's own bloom input as H', all in one command list"});
    pins.push_back({"blend-factor",
                    has(ui, "constfloatfactor[4]={use->m,use->m,use->m,0.0f};") && has(ui, "bd.RenderTarget[0].SrcBlend=D3D11_BLEND_BLEND_FACTOR;") &&
                        has(ui, "bd.RenderTarget[0].DestBlend=D3D11_BLEND_ONE;") && has(ui, "bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;") &&
                        has(ui, "static_assert(kFlatStarBlendSrc==D3D11_BLEND_BLEND_FACTOR"),
                    "the star pass adds m * S: blend factor (m, m, m, 0), source BLEND_FACTOR, destination ONE, ADD, held to the math header by a static_assert"});
    pins.push_back({"hud-only-binds-hud", countOf(ui, "hud->hdrSrv.Get()") == 1,
                    "the HUD layer's view is bound once, by the HUD's pass: S carries no HUD and the star pass binds only S"});
    pins.push_back({"live-after-bind",
                    inOrder(mod, {"boolflatStarGlowReplayBegin(", "flatStarCountReplayed(g_win,static_cast<FlatStarRole>(role));", "g_frame.anyReplay=true;",
                                  "returntrue;}"}) &&
                        has(mod, "returnflatStarGlowOn()&&g_frame.anyReplay&&g_layer.srv;") && countOf(mod, "g_frame.anyReplay=true;") == 1,
                    "the star layer is live for the first level only after a replay was bound (the module marks it, once, in flatStarGlowReplayBegin)"});
    // A fault between the first rebinding and the end of the bracket's start leaves S bound with no ReplayEnd to follow (Begin returned false): the failure
    // path must put the game's bindings back, with a guard of its own that a spent fault budget cannot skip, and ReplayEnd must use that same restore.
    pins.push_back({"begin-fault-restore",
                    has(mod, "g_saved.active=true;vScreenSetRenderTargetsRaw(ctx,1,&s,g_saved.dsv.Get());vScreenOMSetDepthStencilStateRaw(ctx,replay,ref);why=FlatStarRefusal::kNone;") &&
                        countOf(mod, "g_saved.active=true;") == 1 &&
                        inOrder(mod, {"boolflatStarGlowReplayBegin(", "if(!ran||!ok){", "if(g_saved.active)restoreSaved(ctx);", "releaseSaved();", "returnfalse;}"}) &&
                        inOrder(mod, {"voidflatStarGlowReplayEnd(", "if(!restoreSaved(ctx))flatStarCountRefusal(", "releaseSaved();"}) &&
                        inOrder(mod, {"boolrestoreSaved(", "guarded(\"starGlow.restore\"", "vScreenSetRenderTargetsRaw(ctx,n,raw,g_saved.dsv.Get());", "g_budget.charge();"}) &&
                        !has(mod, "guardedBudget(g_budget,[&]{ID3D11RenderTargetView*raw["),
                    "the saved state is live from the first rebinding, a fault inside Begin puts the game's targets and depth state back (guarded on its own, not through the budget), and ReplayEnd uses the same restore"});
    pins.push_back({"gate",
                    has(math, "returnflatUiGlowFeedWanted(layerOn||starLive,triggerSrvKnown,triggerAmbiguous,srvSlot,hdrW,hdrH,targetW,targetH);"),
                    "the first level is asked for by the HUD layer or by live star light: fix.ui_quality is not a condition of the star glow"});
    return pins;
}

void testWiring() {
    const std::string runtimeCpp = readText("src/d3d11/flat_runtime.cpp"), runtimeH = readText("src/d3d11/flat_runtime.h"), uiLayerCpp = readText("src/d3d11/ui_layer.cpp"),
                      flatMath = readText("src/d3d11/flat_ui_layer_math.h"), buildBat = readText("build.bat"),
                      starCpp = readText("src/d3d11/flat_star_glow.cpp");
    check(!runtimeCpp.empty() && !runtimeH.empty() && !uiLayerCpp.empty() && !flatMath.empty() && !buildBat.empty() && !starCpp.empty(),
          "star glow wiring: flat_runtime.cpp, flat_runtime.h, ui_layer.cpp, flat_ui_layer_math.h, flat_star_glow.cpp and build.bat are readable from the working directory");
    for (const Scan& p : wiringPins(runtimeCpp, runtimeH, uiLayerCpp, flatMath, buildBat, starCpp)) {
        char msg[420];
        std::snprintf(msg, sizeof(msg), "star glow wiring [%s]: %s", p.id, p.what);
        check(p.ok, msg);
    }
    // Controls: each edit puts a likely slip back and must trip the pin it names.
    struct Control {
        const char* id;
        int file;   // 0 flat_runtime.cpp, 1 flat_runtime.h, 2 ui_layer.cpp, 3 flat_ui_layer_math.h, 4 build.bat, 5 flat_star_glow.cpp
        const char* from;
        const char* to;
    };
    const Control controls[] = {
        {"in-dll", 4, "\"src\\d3d11\\flat_star_glow.cpp\"", "\"src\\d3d11\\flat_nothing.cpp\""},
        {"fast-path", 0, "&& cameraProbeAttempt == 0 && !starTouched;", "&& cameraProbeAttempt == 0;"},
        {"observe-gated", 0, "if (flatStarGlowOn()) {", "if (true) {"},
        {"after-trigger-fact", 0, "star.triggered = s.hdr.triggered;", "star.triggered = false;"},
        {"reissue", 1, "if (starReplay && flatStarGlowReplayBegin(ctx, starRole, starW, starH)) { issue(); flatStarGlowReplayEnd(ctx); }",
         "if (starReplay && flatStarGlowReplayBegin(ctx, starRole, starW, starH)) { flatStarGlowReplayEnd(ctx); }"},
        {"feed-input", 0, "flatUiGlowBegin(ctx,state().prefix.frame,glowSlot,glowHdr,glowVs,glowPs,glowHdrW,glowHdrH,glowTargetW,glowTargetH,&glowOriginal,haveStar?&star:nullptr);",
         "flatUiGlowBegin(ctx,state().prefix.frame,glowSlot,glowHdr,glowVs,glowPs,glowHdrW,glowHdrH,glowTargetW,glowTargetH,&glowOriginal,nullptr);"},
        {"present-first", 0, "flatStarGlowFrame(frame + 1, s.frameLive);", ""},
        {"present-first", 0, "flatStarGlowFrame(frame + 1, s.frameLive);", "flatStarGlowFrame(frame + 1, flatRuntimeActive());"},
        {"release", 0, "flatStarGlowNoteDevice(s.device.Get());", ""},
        {"pass-order", 2, "dc->CopyResource(g_glow.tex.Get(), hTex.Get());", ""},
        {"blend-factor", 2, "const float factor[4] = {use->m, use->m, use->m, 0.0f};", "const float factor[4] = {1.0f, 1.0f, 1.0f, 0.0f};"},
        {"hud-only-binds-hud", 2, "ID3D11ShaderResourceView* in[1] = {use->srv};", "ID3D11ShaderResourceView* in[1] = {hud->hdrSrv.Get()};"},
        {"gate", 3, "flatUiGlowFeedWanted(layerOn || starLive,", "flatUiGlowFeedWanted(layerOn,"},
        {"live-after-bind", 5, "    g_frame.anyReplay = true;\n    return true;", "    return true;"},
        {"begin-fault-restore", 5, "        if (g_saved.active) restoreSaved(ctx);\n", ""},
        {"begin-fault-restore", 5, "    g_saved.active = true;\n    vScreenSetRenderTargetsRaw(ctx, 1, &s, g_saved.dsv.Get());\n    vScreenOMSetDepthStencilStateRaw(ctx, replay, ref);\n",
         "    vScreenSetRenderTargetsRaw(ctx, 1, &s, g_saved.dsv.Get());\n    vScreenOMSetDepthStencilStateRaw(ctx, replay, ref);\n    g_saved.active = true;\n"},
        {"begin-fault-restore", 5, "    if (!restoreSaved(ctx)) flatStarCountRefusal(", "    if (!false) flatStarCountRefusal("},
    };
    for (const Control& c : controls) {
        std::string files[6] = {runtimeCpp, runtimeH, uiLayerCpp, flatMath, buildBat, starCpp};
        std::string& f = files[c.file];
        const size_t at = f.find(c.from);
        char msg[240];
        std::snprintf(msg, sizeof(msg), "star glow wiring control [%s]: the edited text is present in the source", c.id);
        check(at != std::string::npos, msg);
        if (at == std::string::npos) continue;
        f.replace(at, std::strlen(c.from), c.to);
        bool tripped = false;
        for (const Scan& p : wiringPins(files[0], files[1], files[2], files[3], files[4], files[5]))
            if (std::strcmp(p.id, c.id) == 0 && !p.ok) tripped = true;
        std::snprintf(msg, sizeof(msg), "star glow wiring control [%s]: the edit trips the pin it names", c.id);
        check(tripped, msg);
    }
}

void testAll() {
    testSetting();
    testPairs();
    testRuns();
    testComposition();
    testShaderText();
    testLines();
    testWiring();
}

}  // namespace starglowtest
