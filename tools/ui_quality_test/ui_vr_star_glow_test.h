// Included in ui_quality_test.cpp after ui_star_glow_test.h (check; worldroute::readText, has, inOrder, countOf, bodyOf; afterui::squeeze; starglowtest::fileHas64).
//
// visual.star_glow in the headset (docs/star-glow-2026-10-22.md, "2026-10-27: VR star glow, built"). What these cases hold:
//   * when it runs: the mode for every setting and every state of Elite's Bloom (off at 100, paused while Bloom is known off, running when Bloom is on or
//     unknown), and that paused is the menus' hint condition itself;
//   * the first level: found by its pixel shader alone (143AAE0597E2F7BF), never the exposure reduction, the composite or the chain's other passes that read
//     the same H, with the verdict for the H it reads (feed, unknown H, no light, repeat);
//   * the slot table and the pre-pass table: one slot per H, found by resource identity whatever the order the eyes' draws arrive in; full, evicting and idle
//     rules; a pre-pass keyed by the depth view it shares with its eye's H, for its own frame;
//   * the per-H run on draw lists transcribed from the 10-27 census (edvr_gfx_20261027_124810, #97/#197 the pre-passes, #316-#321 and #329-#334 the star
//     passes of the two eyes), in the census order, fully interleaved and with the eyes swapped: both eyes replay every pass, the first level of one eye
//     refuses nothing of the other, a pre-pass of one eye does not unlock the other, another eye's unrelated draw does not end this eye's run, a jet's
//     source unlocks the jets of both;
//   * the first level fed once an eye and only at its own shader, the exposure reduction and the composite left alone, H' without a HUD term;
//   * the window lines (the head, one for an H) with exact text, the lines for pausing and returning to 100, and every refusal named;
//   * the wiring, by source scan with controls: the module is in the DLL's source list; the four draw thunks ask one relaxed load and call the module before
//     and after the game's issue; the re-issue is the planet re-issue's shape; the frame boundary follows menuTick; the first level is found by its shader
//     only while some H holds light; H' is bound for one draw and the game's view comes back through the raw setter; the replay's bracket restores through
//     its own guard; S is the layer of the H the first level reads; the pass has no HUD; the Visual page carries the row.
namespace vrstarglowtest {

using worldroute::bodyOf;
using worldroute::countOf;
using worldroute::has;
using worldroute::inOrder;
using worldroute::readText;

// ------------------------------------------------------------------ when it runs

void testMode() {
    const sgb::Bloom states[] = {sgb::Bloom::Unknown, sgb::Bloom::Off, sgb::Bloom::On};
    bool offAt100 = true, pausedOnlyWhenOff = true, runsWhenNotOff = true, sameAsHint = true;
    for (int pct : {-5, 0, 99, 100}) {
        for (sgb::Bloom b : states) offAt100 = offAt100 && vrStarMode(pct, b) == VrStarMode::kOff;
    }
    for (int pct : {125, 150, 175, 200}) {
        pausedOnlyWhenOff = pausedOnlyWhenOff && vrStarMode(pct, sgb::Bloom::Off) == VrStarMode::kPaused;
        runsWhenNotOff = runsWhenNotOff && vrStarMode(pct, sgb::Bloom::On) == VrStarMode::kOn && vrStarMode(pct, sgb::Bloom::Unknown) == VrStarMode::kOn;
        for (sgb::Bloom b : states) sameAsHint = sameAsHint && (vrStarMode(pct, b) == VrStarMode::kPaused) == (sgb::hint(pct, b) != nullptr);
    }
    check(offAt100, "vr star glow: at 100 (and under) the mode is off whatever Elite's Bloom is: nothing runs and nothing is created");
    check(pausedOnlyWhenOff, "vr star glow: above 100 with Elite's Bloom known to be off the mode is paused (no first level to feed)");
    check(runsWhenNotOff, "vr star glow: above 100 with Bloom on, or not known, the glow runs (unknown says nothing: a wrong pause is worse than a wasted pass)");
    check(sameAsHint, "vr star glow: paused is exactly the menus' hint condition (sgb::hint), so the headset's menu and the driver cannot disagree");
    check(vrStarMode(150, sgb::Bloom::Off) != VrStarMode::kOn && vrStarMode(100, sgb::Bloom::On) != VrStarMode::kOn,
          "vr star glow: only above 100 and not paused does anything replay");
}

// ------------------------------------------------------------------ the first level

// The pixel shaders of one eye's chain in the 10-27 census (#744-#755, #339/#340), in order: the first level, the downsample to 504, 252, 126 and 63, the
// upsample-adds, the exposure reduction (three draws) and the composite with its tail. Full hashes, from the census lines.
constexpr uint64_t kDownsamplePs = 0x5ECFE7085464069Bull, kUpsampleAddPs = 0x8826CACC6382C78Dull, kExposurePs = 0x831DF02EBA8AE814ull,
                   kCompositePs = 0xD0A16B9E55BF22CCull, kTailPs = 0xDED8796049C7BB4Aull;
const uint64_t kChain[] = {kVrStarFirstLevelPs, kDownsamplePs, kDownsamplePs, kDownsamplePs, kDownsamplePs, kUpsampleAddPs, kUpsampleAddPs, kUpsampleAddPs, kUpsampleAddPs,
                           kExposurePs,         kExposurePs,   kExposurePs,   kCompositePs,   kTailPs};

void testFirstLevel() {
    check(kVrStarFirstLevelPs == 0x143AAE0597E2F7BFull && vrStarIsFirstLevel(0x143AAE0597E2F7BFull), "vr star glow: the first level is pixel shader 143AAE0597E2F7BF");
    const std::string math = readText("src/d3d11/vr_star_glow_math.h");
    check(!math.empty() && starglowtest::fileHas64(math, kVrStarFirstLevelPs), "vr star glow: the header carries the census line's full hash");
    int asked = 0;
    for (uint64_t ps : kChain) asked += vrStarIsFirstLevel(ps) ? 1 : 0;
    check(asked == 1, "vr star glow: of one eye's chain only the first level asks for H' (the exposure reduction, the composite and the rest keep H)");
    check(!vrStarIsFirstLevel(kExposurePs) && !vrStarIsFirstLevel(kCompositePs) && !vrStarIsFirstLevel(kDownsamplePs) && !vrStarIsFirstLevel(kUpsampleAddPs) &&
              !vrStarIsFirstLevel(kTailPs) && !vrStarIsFirstLevel(0),
          "vr star glow: the exposure reduction, the composite, the chain's downsample, upsample and tail, and no pixel shader, are not the first level");
    check(!vrStarIsFirstLevel(kVrStarFirstLevelPs ^ 1ull) && !vrStarIsFirstLevel(kVrStarFirstLevelPs >> 8) && !vrStarIsFirstLevel(kFlatStarSurfacePs),
          "vr star glow: a hash one bit off, and a star pass's, are not it");
    // The verdict for the H it reads.
    check(vrStarFirstLevel(true, true, false) == VrFirstLevel::kFeed, "vr star glow: an H that holds light and has not been fed is fed");
    check(vrStarFirstLevel(true, false, false) == VrFirstLevel::kHoldsNoLight && vrStarFirstLevel(true, false, true) == VrFirstLevel::kRepeat,
          "vr star glow: a known H with no light is not fed (no light); one already fed this frame is not fed twice (repeat)");
    check(vrStarFirstLevel(true, true, true) == VrFirstLevel::kRepeat, "vr star glow: a second first level reading an H with light is a repeat, never a second feed");
    check(vrStarFirstLevel(false, false, false) == VrFirstLevel::kUnknownH && vrStarFirstLevel(false, true, true) == VrFirstLevel::kUnknownH,
          "vr star glow: an H the table does not hold (the 2D screen's scene, an H no star pass touched) is unknown, whatever the other facts say");
}

// ------------------------------------------------------------------ the tables

const void* hKey(int h) { return reinterpret_cast<const void*>(0x10000 + static_cast<uintptr_t>(h) * 0x100); }
const void* dsvOf(int h) { return reinterpret_cast<const void*>(0x20000 + static_cast<uintptr_t>(h) * 0x100); }

void testTables() {
    {
        VrStarSlotTable t;
        bool fresh = false;
        const int a = t.claim(hKey(0), 10, &fresh);
        check(a == 0 && fresh, "vr star glow: the first H takes the first slot");
        const int b = t.claim(hKey(1), 10, &fresh);
        check(b == 1 && fresh, "vr star glow: the second H takes the second slot");
        check(t.claim(hKey(0), 11, &fresh) == 0 && !fresh && t.claim(hKey(1), 11, &fresh) == 1 && !fresh, "vr star glow: an H found again is the same slot, not fresh");
        VrStarSlotTable swapped;
        const int sb = swapped.claim(hKey(1), 10, &fresh), sa = swapped.claim(hKey(0), 10, &fresh);
        check(sb == 0 && sa == 1 && swapped.find(hKey(0)) == 1 && swapped.find(hKey(1)) == 0,
              "vr star glow: a slot belongs to the H it was claimed for, whichever eye's draw comes first");
        check(t.find(hKey(7)) == -1 && t.find(nullptr) == -1 && t.claim(nullptr, 10, &fresh) == -1 && !fresh, "vr star glow: an H the table does not hold, and no H, find nothing");
    }
    {
        VrStarSlotTable t;
        bool fresh = false;
        for (int i = 0; i < static_cast<int>(kVrStarSlots); ++i) t.claim(hKey(i), 100 + static_cast<uint64_t>(i), &fresh);
        check(t.claim(hKey(40), 101, &fresh) == -1 && !fresh, "vr star glow: a full table of H used this frame or the one before holds no more (slots-full)");
        check(t.claim(hKey(41), 104, &fresh) == 0 && fresh && t.find(hKey(0)) == -1 && t.find(hKey(41)) == 0,
              "vr star glow: a full table gives an H its slot by evicting the oldest slot that was not used for two frames");
        check(t.claim(hKey(42), 104, &fresh) == 1 && fresh, "vr star glow: ...and the next oldest for the next newcomer");
        check(t.claim(hKey(44), 104, &fresh) == 2 && fresh, "vr star glow: ...a slot last used two frames ago (102 at frame 104) is stale too");
        check(t.claim(hKey(45), 104, &fresh) == -1 && !fresh && t.find(hKey(3)) == 3, "vr star glow: ...but not slot 3, used one frame ago (103): the table is full of H in use again");
        t.release(2);
        check(t.find(hKey(44)) == -1 && t.claim(hKey(43), 104, &fresh) == 2 && fresh, "vr star glow: a released slot is free again");
        VrStarSlotTable idle;
        idle.claim(hKey(0), 10, &fresh);
        check(!idle.idle(0, 10 + kVrStarSlotIdleFrames) && idle.idle(0, 11 + kVrStarSlotIdleFrames) && !idle.idle(1, 10000),
              "vr star glow: a slot unused for more than 600 frames is idle (released: its H is gone); a free slot is never idle");
    }
    // The pre-pass table: keyed by the depth view, for its own frame.
    {
        VrStarPreTable p;
        p.note(dsvOf(0), 10);
        check(p.seen(dsvOf(0), 10) && !p.seen(dsvOf(1), 10) && !p.seen(dsvOf(0), 11) && !p.seen(dsvOf(0), 9),
              "vr star glow: a pre-pass counts for the depth view it was drawn with and for its own frame only");
        p.note(dsvOf(1), 10);
        check(p.seen(dsvOf(0), 10) && p.seen(dsvOf(1), 10), "vr star glow: both eyes' pre-passes stand side by side");
        p.note(nullptr, 10);
        check(!p.seen(nullptr, 10), "vr star glow: no depth view is no pre-pass");
        VrStarPreTable many;
        for (int i = 0; i < static_cast<int>(kVrStarPreSlots) + 1; ++i) many.note(dsvOf(i), 7);
        check(!many.seen(dsvOf(0), 7) && many.seen(dsvOf(static_cast<int>(kVrStarPreSlots)), 7) && many.seen(dsvOf(1), 7),
              "vr star glow: more depth views than slots overwrite the oldest, and a view noted again takes no second slot");
        VrStarPreTable again;
        again.note(dsvOf(0), 3);
        again.note(dsvOf(0), 4);
        again.note(dsvOf(1), 4);
        again.note(dsvOf(2), 4);
        again.note(dsvOf(3), 4);
        check(again.seen(dsvOf(0), 4) && again.seen(dsvOf(3), 4), "vr star glow: a view noted again updates its slot and takes no second one");
    }
}

// ------------------------------------------------------------------ the per-H run, on the census's draws

// One event of a frame, in submission order: a pre-pass into the G-buffer with an eye's depth view, the jet's source into its own target, a draw into an
// eye's H, or a draw that reads an H at t0 (the chain).
struct Ev {
    enum Kind { kPre, kJetSource, kDraw, kRead } kind;
    int h = 0;                      // the eye's H (kDraw, kRead) or depth view (kPre)
    uint64_t vs = 0, ps = 0;
    bool oneOne = true, plain = true;
};
Ev preE(int h) { return {Ev::kPre, h, kFlatStarPreVs, 0, true, true}; }
Ev jetSrcE() { return {Ev::kJetSource, 0, kFlatStarJetSourceVs, kFlatStarJetSourcePs, true, true}; }
Ev drawE(int h, uint64_t vs, uint64_t ps, bool oneOne = true, bool plain = true) { return {Ev::kDraw, h, vs, ps, oneOne, plain}; }
Ev surfE(int h) { return drawE(h, kFlatStarSurfaceVs, kFlatStarSurfacePs); }
Ev qaE(int h) { return drawE(h, kFlatStarQuadAVs, kFlatStarQuadAPs); }
Ev instE(int h) { return drawE(h, kFlatStarInstancedVs, kFlatStarInstancedPs); }
Ev qbE(int h) { return drawE(h, kFlatStarQuadBVs, kFlatStarQuadBPs); }
Ev ringE(int h) { return drawE(h, kFlatStarRingVs, kFlatStarRingPs); }
Ev arcsE(int h) { return drawE(h, kFlatStarArcsVs, kFlatStarArcsPs); }
Ev jetAddE(int h, bool oneOne = true) { return drawE(h, kFlatStarJetAddVs, kFlatStarJetAddPs, oneOne); }
Ev flareE(int h) { return drawE(h, kFlatStarFlareVs, kFlatStarFlarePs); }
Ev otherE(int h) { return drawE(h, 0x9BFC7FD232328391ull, 0xDBF1725726018F52ull); }   // a draw into H that no pair names
Ev readE(int h, uint64_t ps) { return {Ev::kRead, h, 0xDFED8E1C9E191BECull, ps, true, true}; }

// The module's per-draw sequence (vr_star_glow.cpp classify and bindFirstLevel), on the header's pieces alone.
struct Sim {
    VrStarSlotTable table;
    VrStarPreTable pre;
    VrStarH run[kVrStarSlots];
    VrStarWindow win;
    uint64_t frame = 50, jetFrame = 0;
    std::vector<int> fed;   // the slot of each H' bound, in order

    void apply(const Ev& e) {
        switch (e.kind) {
            case Ev::kPre: pre.note(dsvOf(e.h), frame); ++win.prePasses; return;
            case Ev::kJetSource: jetFrame = frame; ++win.jetSources; return;
            case Ev::kRead: read(e); return;
            case Ev::kDraw: draw(e); return;
        }
    }
    void draw(const Ev& e) {
        ++win.hDraws;
        const FlatStarRole role = flatStarRoleOf(e.vs, e.ps, true);
        if (!flatStarRoleReplays(role)) {
            const int at = table.find(hKey(e.h));
            if (at >= 0) flatStarNoteOther(run[at].f, true);
            return;
        }
        bool fresh = false;
        const int si = table.claim(hKey(e.h), frame, &fresh);
        if (si < 0) {
            ++win.slotsFull;
            return;
        }
        if (fresh) {
            run[si] = VrStarH{};
            win.slot[si] = VrStarSlotWindow{};
        }
        VrStarSlotWindow& w = win.slot[si];
        VrStarFacts facts;
        facts.preSeen = pre.seen(dsvOf(e.h), frame);
        facts.jetSource = jetFrame == frame;
        if (flatStarRoleNeedsBlend(role) && !run[si].done && (run[si].f.pre || facts.preSeen) && (run[si].f.jetSource || facts.jetSource)) facts.blendOneOne = e.oneOne;
        const FlatStarVerdict v = vrStarDecide(run[si], role, facts);
        const int ri = vrStarRoleIndex(role);
        if (ri >= 0) ++w.seen[ri];
        if (!v.replay) {
            vrStarCountRefusal(w, vrStarRefusalOf(v.refusal));
            return;
        }
        if (!e.plain) {
            vrStarCountRefusal(w, VrStarRefusal::kNotPlain);
            return;
        }
        ++w.replayed[ri];
        run[si].f.anyReplay = true;
        run[si].lit = true;
    }
    void read(const Ev& e) {
        if (!vrStarIsFirstLevel(e.ps)) return;
        const int si = table.find(hKey(e.h));
        const bool known = si >= 0;
        switch (vrStarFirstLevel(known, known && run[si].lit, known && run[si].done)) {
            case VrFirstLevel::kUnknownH: ++win.firstLevelUnknownH; return;
            case VrFirstLevel::kHoldsNoLight: ++win.firstLevelNoLight; return;
            case VrFirstLevel::kRepeat: ++win.firstLevelRepeat; return;
            case VrFirstLevel::kFeed: break;
        }
        ++win.slot[si].firstLevels;
        run[si].done = true;
        ++win.slot[si].fed;
        fed.push_back(si);
    }
    void run_(const std::vector<Ev>& evs) { for (const Ev& e : evs) apply(e); }
    // The frame's end (foldFrame): star frames, a lit H nobody read, then the next frame clean.
    void endFrame() {
        for (size_t i = 0; i < kVrStarSlots; ++i) {
            if (!table.key[i]) continue;
            if (run[i].f.anyStar) ++win.slot[i].starFrames;
            if (run[i].lit && !run[i].done) vrStarCountRefusal(win.slot[i], VrStarRefusal::kNoFirstLevel);
            vrStarHBeginFrame(run[i]);
        }
        ++frame;
    }
};

// One eye's star run as the census has it: the six passes of #316-#321 (eye A) or #329-#334 (eye B).
std::vector<Ev> starRun(int h) { return {surfE(h), qaE(h), instE(h), qbE(h), ringE(h), arcsE(h)}; }
uint64_t replayedTotal(const VrStarSlotWindow& w) {
    uint64_t n = 0;
    for (uint64_t v : w.replayed) n += v;
    return n;
}
uint64_t refusedTotal(const VrStarSlotWindow& w) {
    uint64_t n = 0;
    for (uint64_t v : w.refused) n += v;
    return n;
}
bool replayedEvery(const VrStarSlotWindow& w) {   // surf, qa, inst, qb, ring, arcs once; the jet's add and flare not
    return w.replayed[0] == 1 && w.replayed[1] == 1 && w.replayed[2] == 1 && w.replayed[3] == 1 && w.replayed[4] == 1 && w.replayed[5] == 1 && w.replayed[6] == 0 &&
           w.replayed[7] == 0;
}

void testRuns() {
    // The census order: eye A's pre-pass, hundreds of unrelated draws (into the G-buffer and into H), A's run; eye B's the same; then the chain of each.
    std::vector<Ev> census;
    census.push_back(preE(0));
    for (int i = 0; i < 300; ++i) census.push_back(otherE(i % 3 == 0 ? 0 : 1));
    for (const Ev& e : starRun(0)) census.push_back(e);
    census.push_back(preE(1));
    for (int i = 0; i < 300; ++i) census.push_back(otherE(1));
    for (const Ev& e : starRun(1)) census.push_back(e);
    // The chain: the first level reads the H, then the exposure reduction and the composite read it too.
    for (int h = 0; h < 2; ++h)
        for (uint64_t ps : kChain) census.push_back(readE(h, ps));
    {
        Sim s;
        s.run_(census);
        const int a = s.table.find(hKey(0)), b = s.table.find(hKey(1));
        check(a >= 0 && b >= 0 && a != b, "vr star glow: the census frame gives each eye's H a slot of its own");
        if (a >= 0 && b >= 0) {
            check(replayedEvery(s.win.slot[a]) && replayedEvery(s.win.slot[b]) && refusedTotal(s.win.slot[a]) == 0 && refusedTotal(s.win.slot[b]) == 0,
                  "vr star glow: the census frame replays all six passes of both eyes and refuses nothing (a pre-pass for each eye's depth view came first)");
            check(s.win.slot[a].firstLevels == 1 && s.win.slot[a].fed == 1 && s.win.slot[b].firstLevels == 1 && s.win.slot[b].fed == 1 && s.fed.size() == 2 &&
                      s.fed[0] == a && s.fed[1] == b && s.win.firstLevelUnknownH == 0 && s.win.firstLevelNoLight == 0 && s.win.firstLevelRepeat == 0,
                  "vr star glow: each eye's chain feeds H' exactly once, to the H its first level reads; the exposure reduction and the composite read the same H and feed nothing");
        }
        s.endFrame();
        check(a >= 0 && b >= 0 && s.win.slot[a].starFrames == 1 && s.win.slot[b].starFrames == 1 && refusedTotal(s.win.slot[a]) == 0 && refusedTotal(s.win.slot[b]) == 0 &&
                  !s.run[a].lit && !s.run[a].done && !s.run[b].lit,
              "vr star glow: the frame's end counts a star frame for each eye and starts the next one clean (nothing lit, nothing fed)");
    }
    // The same draws fully interleaved (one pass of A, one of B, ...), and with the eyes swapped: the slots and the counts are the same.
    {
        std::vector<Ev> inter;
        inter.push_back(preE(1));
        inter.push_back(preE(0));
        const std::vector<Ev> ra = starRun(0), rb = starRun(1);
        for (size_t i = 0; i < ra.size(); ++i) {
            inter.push_back(rb[i]);
            inter.push_back(ra[i]);
        }
        for (uint64_t ps : kChain) {
            inter.push_back(readE(1, ps));
            inter.push_back(readE(0, ps));
        }
        Sim s;
        s.run_(inter);
        const int a = s.table.find(hKey(0)), b = s.table.find(hKey(1));
        check(a == 1 && b == 0, "vr star glow: the slot follows the H, not the order: the eye whose draw came first has the first slot");
        check(a >= 0 && b >= 0 && replayedEvery(s.win.slot[a]) && replayedEvery(s.win.slot[b]) && refusedTotal(s.win.slot[a]) + refusedTotal(s.win.slot[b]) == 0 &&
                  s.fed.size() == 2 && s.fed[0] == b && s.fed[1] == a,
              "vr star glow: with the eyes' draws interleaved pass by pass both eyes still replay all six passes and each is fed once, in the order its first level came");
    }
    // A first level of one eye refuses nothing of the other (flat's trigger is frame-wide; here it is per H).
    {
        Sim s;
        s.run_({preE(0), preE(1)});
        s.run_(starRun(0));
        s.apply(surfE(1));
        s.apply(qaE(1));
        for (uint64_t ps : kChain) s.apply(readE(0, ps));   // eye A's chain runs while eye B is still drawing its stars
        for (const Ev& e : {instE(1), qbE(1), ringE(1), arcsE(1)}) s.apply(e);
        const int a = s.table.find(hKey(0)), b = s.table.find(hKey(1));
        check(a >= 0 && b >= 0 && replayedEvery(s.win.slot[b]) && refusedTotal(s.win.slot[b]) == 0 && s.run[a].done && !s.run[b].done,
              "vr star glow: eye A's first level does not refuse eye B's later star passes: the trigger is per H");
        s.apply(readE(1, kVrStarFirstLevelPs));
        check(s.fed.size() == 2 && s.fed[0] == a && s.fed[1] == b, "vr star glow: eye B is then fed on its own first level");
        // A star pass of A after its own first level is too late for it.
        s.apply(surfE(0));
        check(s.win.slot[a].refused[0] == 1 && s.win.slot[a].replayed[0] == 1, "vr star glow: a star pass of an H after that H's first level is refused (after-trigger)");
    }
    // The pre-pass is keyed by the depth view: one eye's does not unlock the other.
    {
        Sim s;
        s.apply(preE(0));
        s.run_(starRun(0));
        s.run_(starRun(1));
        const int a = s.table.find(hKey(0)), b = s.table.find(hKey(1));
        check(a >= 0 && b >= 0 && replayedEvery(s.win.slot[a]) && replayedTotal(s.win.slot[b]) == 0 && s.win.slot[b].refused[1] == 6 && refusedTotal(s.win.slot[b]) == 6,
              "vr star glow: a pre-pass bound to eye A's depth view does not make eye B's passes a star frame (no-pre-pass for all six)");
        Sim none;
        none.run_(starRun(0));
        check(none.win.slot[0].refused[1] == 6 && replayedTotal(none.win.slot[0]) == 0, "vr star glow: a frame with no pre-pass at all replays nothing");
        // The next frame's pre-pass is not this frame's.
        Sim next;
        next.apply(preE(0));
        next.endFrame();
        next.run_(starRun(0));
        check(replayedTotal(next.win.slot[0]) == 0 && next.win.slot[0].refused[1] == 6, "vr star glow: the pre-pass of the frame before is no pre-pass for this one");
    }
    // The ring's run is per H: another eye's unrelated draw does not end it; this eye's own does.
    {
        Sim s;
        s.run_({preE(0), preE(1), qaE(1), qaE(0), otherE(1), ringE(0), ringE(1)});
        const int a = s.table.find(hKey(0)), b = s.table.find(hKey(1));
        check(a >= 0 && b >= 0 && s.win.slot[a].replayed[4] == 1 && s.win.slot[a].refused[2] == 0 && s.win.slot[b].replayed[4] == 0 && s.win.slot[b].refused[2] == 1,
              "vr star glow: eye B's unrelated draw between eye A's quad and ring does not end A's run (the ring replays), and ends B's own (its ring is refused)");
        Sim t;
        t.run_({preE(0)});
        t.apply(qaE(0));
        t.apply(otherE(0));
        t.apply(ringE(0));
        check(t.win.slot[0].replayed[4] == 0 && t.win.slot[0].refused[2] == 1, "vr star glow: eye A's own unrelated draw between its quad and ring ends the run (ring-not-in-run)");
        Sim lone;
        lone.run_({preE(0), otherE(0), ringE(0), preE(1), qbE(1), ringE(1)});
        check(lone.win.slot[0].refused[2] == 1 && lone.win.slot[1].replayed[4] == 1,
              "vr star glow: a ring with no quad before it is refused for its H (the drives' smoke ribbon); another H's ring after its own quad replays");
    }
    // The jets: the source is evidence of the frame in any target; the add needs ONE / ONE; the flare needs the source only.
    {
        Sim s;
        s.run_({preE(0), preE(1), jetSrcE(), qaE(0), ringE(0), jetAddE(0), flareE(0), qaE(1), ringE(1), jetAddE(1, false), flareE(1)});
        check(s.win.slot[0].replayed[6] == 1 && s.win.slot[0].replayed[7] == 1 && refusedTotal(s.win.slot[0]) == 0,
              "vr star glow: with the jet's source drawn, eye A's jet add (ONE / ONE) and flare replay");
        check(s.win.slot[1].replayed[6] == 0 && s.win.slot[1].refused[3] == 1 && s.win.slot[1].replayed[7] == 1,
              "vr star glow: eye B's add with another blend is refused (jet-no-source); its flare, which needs only the source, replays");
        Sim late;
        late.run_({preE(0), jetAddE(0), jetSrcE(), flareE(0)});
        check(late.win.slot[0].replayed[6] == 0 && late.win.slot[0].replayed[7] == 1 && late.win.slot[0].refused[3] == 1,
              "vr star glow: a source drawn after the add is no source for it");
        Sim noSource;
        noSource.run_({preE(0), jetAddE(0), flareE(0)});
        check(noSource.win.slot[0].refused[3] == 2 && replayedTotal(noSource.win.slot[0]) == 0, "vr star glow: a jet with no source in the frame replays nothing");
        s.endFrame();
        s.run_({preE(0), jetAddE(0)});
        check(s.win.slot[0].replayed[6] == 1 && s.win.slot[0].refused[3] == 1,
              "vr star glow: the previous frame's jet source does not carry over: the next frame's add, with a source of its own missing, is refused");
    }
    // A draw another fix altered or withheld is not repeated.
    {
        Sim s;
        s.run_({preE(0), drawE(0, kFlatStarSurfaceVs, kFlatStarSurfacePs, true, false), qaE(0)});
        check(s.win.slot[0].refused[4] == 1 && s.win.slot[0].replayed[0] == 0 && s.win.slot[0].replayed[1] == 1, "vr star glow: a star pass the game's own draw of which another fix altered is refused (not-plain)");
    }
    // More H than slots.
    {
        Sim s;
        for (int h = 0; h < static_cast<int>(kVrStarSlots) + 1; ++h) {
            s.apply(preE(h));
            s.apply(surfE(h));
        }
        check(s.table.find(hKey(static_cast<int>(kVrStarSlots))) == -1 && s.win.slotsFull == 1,
              "vr star glow: a star pass of an H beyond the slots is counted under slots-full and takes no slot");
    }
    // A first level with no light, and a frame that ends with light and no first level.
    {
        Sim s;
        s.run_({preE(0), qaE(0), readE(0, kVrStarFirstLevelPs), readE(3, kVrStarFirstLevelPs)});
        check(s.fed.size() == 1 && s.win.firstLevelUnknownH == 1, "vr star glow: a first level reading an H the table does not hold is counted unknown-h and feeds nothing");
        Sim noLight;
        noLight.run_({otherE(0), preE(0)});
        noLight.table.claim(hKey(0), noLight.frame, nullptr);
        noLight.apply(readE(0, kVrStarFirstLevelPs));
        check(noLight.fed.empty() && noLight.win.firstLevelNoLight == 1, "vr star glow: a first level reading a known H that holds no light is counted no-light");
        Sim twice;
        twice.run_({preE(0), qaE(0), readE(0, kVrStarFirstLevelPs), readE(0, kVrStarFirstLevelPs)});
        check(twice.fed.size() == 1 && twice.win.firstLevelRepeat == 1, "vr star glow: a second first level reading the same H is counted repeat and feeds nothing");
        Sim bloomOff;
        bloomOff.run_({preE(0), qaE(0), ringE(0)});
        bloomOff.endFrame();
        check(bloomOff.win.slot[0].refused[8] == 1 && bloomOff.fed.empty(), "vr star glow: a frame that ends with S holding light and no first level read is counted no-first-level");
    }
}

// ------------------------------------------------------------------ the first level's input

void testComposition() {
    // In the headset the HUD is not in H: the feed is H + m * S, flat's arithmetic with no HUD term, each channel under the format's clamp.
    bool exact = true;
    for (int pct : {100, 125, 150, 175, 200}) {
        const double m = flatStarGlowM(pct);
        for (float h : {0.0f, 0.37f, 12.0f, 2500.0f}) {
            for (float s : {0.0f, 1.0f, 164.9f, 528.1f}) {
                const double want = static_cast<double>(h) + m * s;
                for (int c = 0; c < 3; ++c) exact = exact && std::fabs(flatStarFirstLevel(h, 0.0f, s, m, c) - want) <= 1e-3 + want * 1e-6;
            }
        }
    }
    check(exact, "vr star glow: H' = H + m * S per channel at every setting (no HUD term)");
    check(std::fabs(flatStarFirstLevel(300.0f, 0.0f, 300.0f, flatStarGlowM(150), 0) - 450.0) < 1e-3 && std::fabs(flatStarFirstLevel(300.0f, 0.0f, 0.0f, flatStarGlowM(200), 0) - 300.0) < 1e-3,
          "vr star glow: where the star is all of H, H' reads k times H; away from a star H' is H");
}

// ------------------------------------------------------------------ the lines

void testLines() {
    VrStarWindow w;
    char line[1400];
    int n = vrStarWindowLine(line, sizeof(line), 150, 0, w);
    check(n > 0 && std::strcmp(line, "vr star glow: window=10s setting=150 k=1.50 h-slots=0 slots-full=0 draws=0 h-draws=0 pre-passes=0 jet-sources=0 "
                                     "first-level-not-fed[unknown-h,no-light,repeat]=0,0,0") == 0 &&
              n == static_cast<int>(std::strlen(line)),
          "vr star glow: the window head with every count zero is the format the log shows (the hook never ran: draws=0)");
    Sim s;
    s.run_({preE(0), preE(1), surfE(0), qaE(0), ringE(0), surfE(1), qaE(1), qaE(1)});
    s.apply(readE(0, kVrStarFirstLevelPs));
    s.apply(readE(2, kVrStarFirstLevelPs));
    s.win.draws = 9000;
    s.endFrame();
    n = vrStarWindowLine(line, sizeof(line), 200, 2, s.win);
    check(n > 0 && std::strcmp(line, "vr star glow: window=10s setting=200 k=2.00 h-slots=2 slots-full=0 draws=9000 h-draws=6 pre-passes=2 jet-sources=0 "
                                     "first-level-not-fed[unknown-h,no-light,repeat]=1,0,0") == 0,
          "vr star glow: the head prints the hook's draws, the draws into an eye H, the evidence and the first levels nothing was fed to");
    n = vrStarSlotLine(line, sizeof(line), 0, s.win.slot[0]);
    check(n > 0 && std::strcmp(line, "vr star glow: h1 0x0 star-frames=1 seen[surf,qa,inst,qb,ring,arcs,jet,flare]=1,1,0,0,1,0,0,0 "
                                     "replayed[surf,qa,inst,qb,ring,arcs,jet,flare]=1,1,0,0,1,0,0,0 layers=0 first-levels=1 fed=1 "
                                     "refused[after-trigger,no-pre-pass,ring-not-in-run,jet-no-source,not-plain,state-fault,layer-fail,depth-clone-fail,no-first-level,"
                                     "feed-fail]=0,0,0,0,0,0,0,0,0,0") == 0 &&
              n == static_cast<int>(std::strlen(line)),
          "vr star glow: an H's line prints its star frames, what was seen and replayed by pass, the first levels and feeds, and every refusal by name");
    VrStarSlotWindow sized;
    sized.w = 2016;
    sized.h = 2017;
    vrStarCountRefusal(sized, VrStarRefusal::kFeedFail);
    vrStarCountRefusal(sized, VrStarRefusal::kNone);
    n = vrStarSlotLine(line, sizeof(line), 1, sized);
    check(n > 0 && std::strstr(line, "vr star glow: h2 2016x2017 star-frames=0 ") == line && std::strstr(line, "]=0,0,0,0,0,0,0,0,0,1") != nullptr,
          "vr star glow: the second H is h2 with its size, and a refusal lands in its own slot (none is not counted)");
    char tiny[40];
    n = vrStarSlotLine(tiny, sizeof(tiny), 0, sized);
    check(n > 0 && n < static_cast<int>(sizeof(tiny)) && std::strlen(tiny) == static_cast<size_t>(n), "vr star glow: a short buffer cuts the line and stays terminated");
    n = vrStarWindowLine(tiny, sizeof(tiny), 125, 1, w);
    check(n > 0 && n < static_cast<int>(sizeof(tiny)) && std::strlen(tiny) == static_cast<size_t>(n), "vr star glow: ...the head too");
    n = vrStarOffLine(line, sizeof(line), 150);
    check(n > 0 && std::strcmp(line, "vr star glow: off (setting=100, was 150); nothing runs and nothing is created at 100") == 0, "vr star glow: the line for the setting returning to 100");
    n = vrStarPausedLine(line, sizeof(line), 175);
    check(n > 0 && std::strstr(line, "vr star glow: paused at setting=175: Elite's Bloom is known to be off") == line, "vr star glow: the line for pausing on a Bloom that is off");
    for (size_t i = 1; i <= kVrStarRefusals; ++i) {
        const char* name = vrStarRefusalName(static_cast<VrStarRefusal>(i));
        bool unique = std::strcmp(name, "?") != 0;
        for (size_t j = 1; j < i; ++j) unique = unique && std::strcmp(name, vrStarRefusalName(static_cast<VrStarRefusal>(j))) != 0;
        check(unique, "vr star glow: every refusal has its own name");
    }
    for (size_t i = 0; i < kVrStarRoles; ++i) check(std::strcmp(vrStarRoleName(i), "?") != 0, "vr star glow: every pass an H can have replayed has a name");
    check(vrStarRoleIndex(FlatStarRole::kSurface) == 0 && vrStarRoleIndex(FlatStarRole::kFlare) == 7 && vrStarRoleIndex(FlatStarRole::kPre) == -1 &&
              vrStarRoleIndex(FlatStarRole::kJetSource) == -1 && vrStarRoleIndex(FlatStarRole::kNone) == -1,
          "vr star glow: the eight passes have slots in the line's order; the pre-pass, the jet's source and nothing have none");
    check(vrStarRefusalOf(FlatStarRefusal::kAfterTrigger) == VrStarRefusal::kAfterTrigger && vrStarRefusalOf(FlatStarRefusal::kNoPrePass) == VrStarRefusal::kNoPrePass &&
              vrStarRefusalOf(FlatStarRefusal::kRingNotInRun) == VrStarRefusal::kRingNotInRun && vrStarRefusalOf(FlatStarRefusal::kJetNoSource) == VrStarRefusal::kJetNoSource &&
              vrStarRefusalOf(FlatStarRefusal::kNone) == VrStarRefusal::kNone,
          "vr star glow: flat's four decisions keep their meaning in the headset's names");
}

// ------------------------------------------------------------------ the wiring

struct Pin {
    const char* id;
    bool ok;
    const char* what;
};

struct Sources {
    std::string vscreen, deviceHook, mod, modH, uiLayer, buildBat, ini, menu;
};

std::vector<Pin> wiringPins(const Sources& src) {
    std::vector<Pin> pins;
    const std::string vs = afterui::squeeze(src.vscreen), dh = afterui::squeeze(src.deviceHook), mod = afterui::squeeze(src.mod), hdr = afterui::squeeze(src.modH),
                      ui = afterui::squeeze(src.uiLayer), menu = afterui::squeeze(src.menu);
    pins.push_back({"in-dll", has(src.buildBat, "\"src\\d3d11\\vr_star_glow.cpp\""), "build.bat compiles src\\d3d11\\vr_star_glow.cpp into the DLL"});
    // Off is one load: each of the four thunks asks vrStarGlowWanted() once (owner context only) and calls the module before and after the game's issue.
    pins.push_back({"thunks-ask-once",
                    countOf(vs, "constboolstarGlow=vrStarGlowWanted()&&self==g_state->ownerCtx;") == 4 && countOf(vs, "if(starGlow)vrStarGlowBeforeDraw(self);") == 4 &&
                        has(vs, "if(starGlow)starGlowAfterDraw(self,'D',count,1,args,v);") && has(vs, "if(starGlow)starGlowAfterDraw(self,'I',count,1,args,v);") &&
                        has(vs, "if(starGlow)starGlowAfterDraw(self,'N',perInstance,drawn,args,v);") &&
                        has(vs, "if(starGlow)starGlowAfterDraw(self,'X',perInstance,instances,args,v);") && countOf(vs, "starGlowAfterDraw(self,'") == 4,
                    "the four draw thunks ask the one relaxed load (vrStarGlowWanted, owner context only), call the module before the game's issue and starGlowAfterDraw "
                    "after it, with the thunk's own arguments"});
    {
        struct Thunk {
            const char* sig;
            const char* name;
            const char* issue;
            const char* after;
        };
        const Thunk thunks[] = {
            {"void STDMETHODCALLTYPE hookedDraw(", "hookedDraw", "}elseforwardWithVerdict(self,v,'D'", "if(starGlow)starGlowAfterDraw(self,'D'"},
            {"void STDMETHODCALLTYPE hookedDrawIndexed(", "hookedDrawIndexed", "}elseforwardWithVerdict(self,v,'I'", "if(starGlow)starGlowAfterDraw(self,'I'"},
            {"void STDMETHODCALLTYPE hookedDrawInstanced(", "hookedDrawInstanced", "}elseforwardWithVerdict(self,v,'N'", "if(starGlow)starGlowAfterDraw(self,'N'"},
            {"void STDMETHODCALLTYPE hookedDrawIndexedInstanced(", "hookedDrawIndexedInstanced", "forwardWithVerdict(self,v,'X'", "if(starGlow)starGlowAfterDraw(self,'X'"},
        };
        bool all = true;
        for (const Thunk& t : thunks) {
            std::string body;
            if (!afterui::functionBody(src.vscreen, t.sig, &body)) {
                all = false;
                continue;
            }
            body = afterui::squeeze(body);
            all = all && inOrder(body, {"vrWorldRouteDraw(self);", "constboolstarGlow=", "if(starGlow)vrStarGlowBeforeDraw(self);", t.issue, "endPanelOverride(self);", t.after,
                                        "pixelProbeAfterEye("});
        }
        pins.push_back({"thunk-order", all,
                        "in each thunk the module is called after the world route's detector and before the game's issue, and the re-issue comes after the verdict's own "
                        "end (endPanelOverride) and before the pixel probe's after-half"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.vscreen, "__declspec(noinline) void starGlowAfterDraw(", &body);
        body = afterui::squeeze(body);
        pins.push_back({"after-reissue",
                        found && inOrder(body, {"if(vrStarGlowAfterDraw(self,v==DrawVerdict::kNone&&!uiLayerIssueBlocked())){", "pureDrawReissue(self,kind,count,instances,args);",
                                                "vrStarGlowReplayEnd(self);"}),
                        "starGlowAfterDraw issues the star pass once more between the module's Begin and End, with the game's arguments, and only for a draw no verdict altered "
                        "and the layer has not blocked"});
    }
    pins.push_back({"boundary",
                    has(vs, "tkVrStarGlow.run([&]{vrStarGlowFrameBoundary();});") && countOf(vs, "vrStarGlowFrameBoundary()") == 1 &&
                        inOrder(dh, {"tkMenu.run([]{menuTick(g_state->device);});", "tkVscreenRest.run([]{vScreenFrameBoundary();});"}),
                    "the glow's frame tick runs inside vScreenFrameBoundary, which runs after menuTick has refreshed the Bloom reading"});
    // The module.
    pins.push_back({"hook-shape",
                    has(hdr, "inlinevoidvrStarGlowBeforeDraw(ID3D11DeviceContext*ctx){if(detail::g_vrStarGlowPending.load(std::memory_order_relaxed))vrStarGlowFirstLevel(ctx);}") &&
                        has(hdr, "inlineboolvrStarGlowWanted(){returndetail::g_vrStarGlowWanted.load(std::memory_order_relaxed);}"),
                    "the draw hooks cost one relaxed load while off, and the first-level test runs only while some H holds light its first level has not read"});
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "void vrStarGlowFirstLevel(ID3D11DeviceContext* ctx) {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"first-level-by-shader",
                        found && inOrder(body, {"vrStarIsFirstLevel(bindingShaderHash(BindSlot::Ps))", "if(g_t0.active)return;", "VrWorldInternalScopeinternal;",
                                                "guardedBudget(g_budget,[&]{bindFirstLevel(ctx);});"}),
                        "the first level is the draw whose pixel shader is the first level's, and the H' pass runs under the helper scope and the fault budget"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "void bindFirstLevel(ID3D11DeviceContext* ctx) {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"h-prime-for-one-draw",
                        found && inOrder(body, {"constintsi=g_table.find(info.resource);", "vrStarFirstLevel(known,", "Slot&s=g_slots[si];", "s.run.done=true;",
                                                "star.srv=s.layer.srv.Get();", "uiLayerStarFeed(ctx,0,info.resource,&star,&verdict);", "vrStarCountRefusal(w,VrStarRefusal::kFeedFail);",
                                                "vScreenPSSetShaderResourcesRaw(ctx,0,1,&bound);", "g_t0.game=game;", "g_t0.active=true;", "++w.fed;"}) &&
                            countOf(mod, "g_t0.active=true;") == 1,
                        "H' is built from the star layer of the H the first level reads (found by its resource), the H is marked fed first, a feed that fails is counted, "
                        "H' is bound at t0 through the raw setter and the game's view is held for the end"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "void endFirstLevel(ID3D11DeviceContext* ctx) {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"game-view-back",
                        found && inOrder(body, {"g_t0.active=false;", "guarded(\"vrStarGlow.restoreT0\"", "vScreenPSSetShaderResourcesRaw(ctx,0,1,&game);", "g_budget.charge();"}) &&
                            has(mod, "if(g_t0.active)endFirstLevel(ctx);++g_win.draws;"),
                        "the game's view of H goes back at t0 through the raw setter under a guard of its own, before anything else looks at the draw"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "bool vrStarGlowAfterDraw(ID3D11DeviceContext* ctx, bool plain) {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"counted-before-classify",
                        found && inOrder(body, {"if(g_t0.active)endFirstLevel(ctx);", "++g_win.draws;", "guardedBudget(g_budget,[&]{replay=classify(ctx,plain);})", "if(g_saved.active)restoreSaved(ctx);"}),
                        "every draw the hook sees is counted (draws) before it is classified, and a fault inside the classification puts the game's bindings back"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "bool classify(ID3D11DeviceContext* ctx, bool plain) {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"star-pass-keyed",
                        found && inOrder(body, {"Target&t=currentTarget();", "if(!t.hFormat||!dsv){", "g_pre.note(dsv,g_frame);", "kFlatStarJetSourcePs", "++g_win.hDraws;",
                                                "flatStarRoleOf(bindingShaderHash(BindSlot::Vs),ps,true)", "g_table.claim(t.res,g_frame,&fresh)", "++g_win.slotsFull;",
                                                "facts.preSeen=g_pre.seen(dsv,g_frame);", "facts.jetSource=g_jetSourceFrame==g_frame;", "vrStarDecide(s.run,role,facts)",
                                                "vrStarRefusalOf(v.refusal)", "VrStarRefusal::kNotPlain", "replayBegin(ctx,static_cast<size_t>(si),t.w,t.h,&why)",
                                                "s.run.f.anyReplay=true;", "s.run.lit=true;"}),
                        "a star pass is looked at only when the target is an eye H with a depth view; its slot is the H's, the pre-pass is the depth view's, the verdict is flat's "
                        "run machine on that slot, and the pass is bound only after the gates and a plain game draw"});
        pins.push_back({"lit-after-bind", found && countOf(mod, "s.run.lit=true;") == 1 && countOf(mod, "s.run.f.anyReplay=true;") == 1 &&
                                              inOrder(body, {"replayBegin(", "++w.replayed[ri];", "s.run.f.anyReplay=true;", "if(!s.run.lit){", "s.run.lit=true;", "returntrue;}"}),
                        "an H holds light only after a replay of it was bound (its first level is asked for nothing before)"});
    }
    pins.push_back({"bracket",
                    countOf(mod, "g_saved.active=true;") == 1 &&
                        inOrder(mod, {"g_saved.active=true;", "vScreenSetRenderTargetsRaw(ctx,1,&target,g_saved.dsv.Get());", "vScreenOMSetDepthStencilStateRaw(ctx,replay,ref);",
                                      "returnVrStarRefusal::kNone;"}) &&
                        inOrder(mod, {"boolreplayBegin(", "VrWorldInternalScopeinternal;", "guardedBudget(g_budget,[&]{r=beginReplay(", "if(g_saved.active)restoreSaved(ctx);",
                                      "releaseSaved();", "*why=r;", "returnfalse;}"}) &&
                        inOrder(mod, {"voidvrStarGlowReplayEnd(", "if(!g_saved.active)return;", "restoreSaved(ctx);", "releaseSaved();"}) &&
                        inOrder(mod, {"boolrestoreSaved(", "guarded(\"vrStarGlow.restore\"", "vScreenSetRenderTargetsRaw(ctx,n,raw,g_saved.dsv.Get());",
                                      "vScreenOMSetDepthStencilStateRaw(ctx,g_saved.ds.Get(),g_saved.stencilRef);", "g_budget.charge();"}),
                    "the replay binds S through the raw setters with the saved state live from the first rebinding; a failed Begin and ReplayEnd put the game's targets and depth "
                    "state back through restoreSaved, guarded on its own so a spent fault budget cannot skip it"});
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "void vrStarGlowFrameBoundary() {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"off-is-nothing",
                        found && inOrder(body, {"if(runtimeFlatProfile()){", "standDown();", "if(g_mode==VrStarMode::kOn)foldFrame();", "starGlowPercent(cfg,&recognised)",
                                                "constVrStarModemode=vrStarMode(g_percent,g_percent>100?menuStarGlowBloom():sgb::Bloom::Unknown);", "standDown();",
                                                "if(mode==VrStarMode::kOn){", "g_vrStarGlowWanted.store(true,", "vrStarPausedLine(", "vrStarOffLine("}) &&
                            countOf(mod, "g_vrStarGlowWanted.store(true,") == 1 && has(mod, "voidstandDown(){detail::g_vrStarGlowWanted.store(false,std::memory_order_relaxed);releaseAll();}"),
                        "the hooks are armed only in the ON mode, the mode is the setting and the hint's Bloom reading, every other mode (and a switch to the flat profile) stands "
                        "everything down first, and the setting is read live by the config's generation"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "void foldFrame() {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"fold-counts",
                        found && inOrder(body, {"if(s.run.f.anyStar)++w.starFrames;", "if(s.run.lit&&!s.run.done)vrStarCountRefusal(w,VrStarRefusal::kNoFirstLevel);",
                                                "vrStarHBeginFrame(s.run);", "s.cleared=false;", "++g_frame;"}),
                        "the frame's end counts a star frame and a lit H nobody read, then starts every H clean (S is cleared at the next frame's first replay)"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.uiLayer, "ID3D11ShaderResourceView* uiLayerStarFeed(", &body);
        body = afterui::squeeze(body);
        pins.push_back({"no-hud-term", found && has(body, "glowFeedPass(ctx,nullptr,slot,hdr,v,star)") && !has(body, "g_eye[0]") && !has(body, "runtimeFlatProfile"),
                        "the headset's H' is glowFeedPass with no HUD (a copy of H, then the star pass), and is not gated on the flat profile or eye 0's HUD layer"});
    }
    pins.push_back({"visual-page", has(src.ini, "| live | menu visual\nstar_glow = 100") && !has(src.ini, "| live | menu flat_visual\nstar_glow = 100"),
                    "the Star glow row is `menu visual` in the shipped edvr.ini: VR's Visual page lists it (and the hint beside it) and the flat tab still does (kFlatVisualRows)"});
    pins.push_back({"bloom-reading", has(menu, "sgb::BloommenuStarGlowBloom(){returng_s.starGlowBloom.state();}"),
                    "the Bloom reading the glow pauses on is the menu's own Watch (the hint's reader), not a second parser"});
    return pins;
}

void testWiring() {
    Sources src;
    src.vscreen = readText("src/d3d11/vscreen.cpp");
    src.deviceHook = readText("src/d3d11/device_hook.cpp");
    src.mod = readText("src/d3d11/vr_star_glow.cpp");
    src.modH = readText("src/d3d11/vr_star_glow.h");
    src.uiLayer = readText("src/d3d11/ui_layer.cpp");
    src.buildBat = readText("build.bat");
    src.ini = readText("edvr.ini");
    src.menu = readText("src/d3d11/menu.cpp");
    check(!src.vscreen.empty() && !src.deviceHook.empty() && !src.mod.empty() && !src.modH.empty() && !src.uiLayer.empty() && !src.buildBat.empty() && !src.ini.empty() &&
              !src.menu.empty(),
          "vr star glow wiring: vscreen.cpp, device_hook.cpp, vr_star_glow.cpp and .h, ui_layer.cpp, menu.cpp, edvr.ini and build.bat are readable from the working directory");
    for (const Pin& p : wiringPins(src)) {
        char msg[480];
        std::snprintf(msg, sizeof(msg), "vr star glow wiring [%s]: %s", p.id, p.what);
        check(p.ok, msg);
    }
    // Controls: each edit puts a likely slip back and must trip the pin it names.
    struct Control {
        const char* id;
        int file;   // 0 vscreen.cpp, 1 device_hook.cpp, 2 vr_star_glow.cpp, 3 vr_star_glow.h, 4 ui_layer.cpp, 5 build.bat, 6 edvr.ini, 7 menu.cpp
        const char* from;
        const char* to;
    };
    const Control controls[] = {
        {"in-dll", 5, "\"src\\d3d11\\vr_star_glow.cpp\"", "\"src\\d3d11\\vr_nothing.cpp\""},
        {"thunks-ask-once", 0, "const bool starGlow = vrStarGlowWanted() && self == g_state->ownerCtx;", "const bool starGlow = self == g_state->ownerCtx;"},
        {"thunks-ask-once", 0, "    if (starGlow) starGlowAfterDraw(self, 'D', count, 1, args, v);\n", ""},
        {"thunks-ask-once", 0, "    if (starGlow) starGlowAfterDraw(self, 'X', perInstance, instances, args, v);\n", ""},
        {"thunk-order", 0, "    if (starGlow) starGlowAfterDraw(self, 'N', perInstance, drawn, args, v);\n    if (self == g_state->ownerCtx) pixelProbeAfterEye(g_state, self, perInstance, drawn, false);",
         "    if (self == g_state->ownerCtx) pixelProbeAfterEye(g_state, self, perInstance, drawn, false);\n    if (starGlow) starGlowAfterDraw(self, 'N', perInstance, drawn, args, v);"},
        {"after-reissue", 0, "        pureDrawReissue(self, kind, count, instances, args);\n        vrStarGlowReplayEnd(self);\n", "        vrStarGlowReplayEnd(self);\n"},
        {"after-reissue", 0, "if (vrStarGlowAfterDraw(self, v == DrawVerdict::kNone && !uiLayerIssueBlocked())) {", "if (vrStarGlowAfterDraw(self, true)) {"},
        {"boundary", 0, "tkVrStarGlow.run([&] { vrStarGlowFrameBoundary(); });", ""},
        {"boundary", 1, "tkMenu.run([] { menuTick(g_state->device); });", ""},
        {"hook-shape", 3, "if (detail::g_vrStarGlowPending.load(std::memory_order_relaxed)) vrStarGlowFirstLevel(ctx);", "vrStarGlowFirstLevel(ctx);"},
        {"first-level-by-shader", 2, "vrStarIsFirstLevel(bindingShaderHash(BindSlot::Ps))", "true"},
        {"h-prime-for-one-draw", 2, "star.srv = s.layer.srv.Get();", "star.srv = g_slots[0].layer.srv.Get();"},
        {"h-prime-for-one-draw", 2, "    g_t0.active = true;\n", ""},
        {"game-view-back", 2, "vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &game); });", "(void)game; });"},
        {"game-view-back", 2, "    if (g_t0.active) endFirstLevel(ctx);   // only the first-level draw read H'; the game's view is back before anything else looks\n", ""},
        {"counted-before-classify", 2, "    ++g_win.draws;\n    bool replay = false;", "    bool replay = false;"},
        {"star-pass-keyed", 2, "si = g_table.claim(t.res, g_frame, &fresh);", "si = 0;"},
        {"star-pass-keyed", 2, "facts.preSeen = g_pre.seen(dsv, g_frame);", "facts.preSeen = true;"},
        {"star-pass-keyed", 2, "    if (!plain) {\n        vrStarCountRefusal(w, VrStarRefusal::kNotPlain);\n        return false;\n    }\n", ""},
        {"lit-after-bind", 2, "    if (!s.run.lit) {\n        s.run.lit = true;\n        refreshPending();\n    }\n    return true;", "    return true;"},
        {"bracket", 2, "    g_saved.active = true;\n    vScreenSetRenderTargetsRaw(ctx, 1, &target, g_saved.dsv.Get());", "    vScreenSetRenderTargetsRaw(ctx, 1, &target, g_saved.dsv.Get());\n    g_saved.active = true;"},
        {"bracket", 2, "        if (g_saved.active) restoreSaved(ctx);\n        releaseSaved();\n        *why = r;", "        releaseSaved();\n        *why = r;"},
        {"bracket", 2, "const bool ran = guarded(\"vrStarGlow.restore\", [&] {", "const bool ran = guardedBudget(g_budget, [&] {"},
        {"off-is-nothing", 2, "g_percent > 100 ? menuStarGlowBloom() : sgb::Bloom::Unknown", "sgb::Bloom::Unknown"},
        {"off-is-nothing", 2, "detail::g_vrStarGlowWanted.store(true, std::memory_order_relaxed);", "detail::g_vrStarGlowWanted.store(g_percent > 0, std::memory_order_relaxed);"},
        {"off-is-nothing", 2, "    if (runtimeFlatProfile()) {\n        if (g_mode != VrStarMode::kOff) {\n            standDown();\n            g_mode = VrStarMode::kOff;\n        }\n        return;\n    }\n", ""},
        {"fold-counts", 2, "        if (s.run.lit && !s.run.done) vrStarCountRefusal(w, VrStarRefusal::kNoFirstLevel);\n", ""},
        {"no-hud-term", 4, "glowFeedPass(ctx, nullptr, slot, hdr, v, star)", "glowFeedPass(ctx, &g_eye[0], slot, hdr, v, star)"},
        {"no-hud-term", 4, "    // No profile test and no HUD: the headset's first level has neither", "    if (!runtimeFlatProfile()) return nullptr;\n    // No profile test and no HUD: the headset's first level has neither"},
        {"visual-page", 6, "| live | menu visual\nstar_glow = 100", "| live | menu flat_visual\nstar_glow = 100"},
        {"bloom-reading", 7, "sgb::Bloom menuStarGlowBloom() { return g_s.starGlowBloom.state(); }", "sgb::Bloom menuStarGlowBloom() { return sgb::Bloom::Unknown; }"},
    };
    for (const Control& c : controls) {
        Sources edited = src;
        std::string* files[8] = {&edited.vscreen, &edited.deviceHook, &edited.mod, &edited.modH, &edited.uiLayer, &edited.buildBat, &edited.ini, &edited.menu};
        std::string& f = *files[c.file];
        const size_t at = f.find(c.from);
        char msg[240];
        std::snprintf(msg, sizeof(msg), "vr star glow wiring control [%s]: the edited text is present in the source", c.id);
        check(at != std::string::npos, msg);
        if (at == std::string::npos) continue;
        f.replace(at, std::strlen(c.from), c.to);
        bool tripped = false;
        for (const Pin& p : wiringPins(edited))
            if (std::strcmp(p.id, c.id) == 0 && !p.ok) tripped = true;
        std::snprintf(msg, sizeof(msg), "vr star glow wiring control [%s]: the edit trips the pin it names", c.id);
        check(tripped, msg);
    }
}

void testAll() {
    testMode();
    testFirstLevel();
    testTables();
    testRuns();
    testComposition();
    testLines();
    testWiring();
}

}  // namespace vrstarglowtest
