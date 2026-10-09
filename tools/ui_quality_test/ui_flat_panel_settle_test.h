// Included in ui_quality_test.cpp after ui_panel_budget_test.h (check; afterui::functionBody and squeeze; worldroute::readText, has, countOf and inOrder).
//
// THE FLAT PANEL FACTOR AT A SUPERSAMPLING CHANGE (the 0.19.0 release review, finding 3; docs/design-flat-ui-quality-2026-10-05.md, "2026-10-09: the setter's factor is held
// until the scene follows it"). The game's Supersampling setter writes the new panel factor BEFORE the game reconfigures (ui_panel_scale.cpp's moveFactorTo through
// uiFlatPanelMove); the scene's size follows later. The review's numbers: a 3840x2160 screen, UI 125, Supersampling 1.0 -> 0.5: the factor was 0.800, the setter wrote
// 0.400, and a boundary that still saw the old size's plan (0.800, settled long since) wrote 0.800 back over it; the new size's plan then waited the settle again, 11
// boundaries, for the right one. The decision is UiFlatPanelSettle (ui_sizing_math.h), and these cases run THAT class, with the production plan and move arithmetic, in a
// model of the thunk and the boundary that is nothing but their glue:
//   * the first factor of a steady plan is written on the 11th boundary (the settle is as it was), a plain window resize waits for its settle, and two runs of one view change
//     read one factor;
//   * setter AFTER the last old-size copy (the review's case): the setter's 0.400 stands through the old-size boundaries, never 0.800; the new size's plan 0.400 is accepted on
//     the boundary it arrives, with nothing written and nothing waited; a new size that makes another factor (1952x1098: 0.4067) is written on that boundary;
//   * setter BEFORE the copy: the new size arrives on the next boundary and the factor was never anything but 0.400;
//   * unchanged dimensions: the factor is held for kUiPanelTransitionFrames boundaries and then the plan from the size there is stands, written at once; a setter that moves
//     nothing holds nothing;
//   * rapid successive changes: a second setter restarts the wait, before the copy of the first, after it, and between its copy and its boundary; every factor ever written
//     is the one the last setter made, and the stale ones are never written;
//   * the cap and the floor: an 8K screen at a quarter render and a 1080p screen at double render stay inside [1/4, 1] and inside the texture limit, held and written alike;
//   * a frame with no plan (a loading screen) neither writes nor ends the hold; the key off or the anti-aliasing off forgets it;
//   * the wiring (ui_panel_scale.cpp, by source scan, with controls): the boundary asks the class, returns on a hold and on a wait before it publishes or writes, the thunk
//     records the epoch and the scene's size, the plan publishes the size it was made at, and the key off, the anti-aliasing off and a refused frame tell the class.
// Every mutation of the class is seen to fail one of these cases (the 2026-10-09 entry of the design doc lists them).

namespace flatsettle {

using edvr::UiFlatPanelSettle;
using edvr::UiFlatPanelInputs;
using edvr::UiFlatPanelRefuse;
using edvr::UiPanelPlan;
using Act = UiFlatPanelSettle::Act;

constexpr uint64_t pack(uint32_t w, uint32_t h) { return (static_cast<uint64_t>(w) << 32) | h; }
bool isNear(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// The thunk and the boundary around the class, as ui_panel_scale.cpp has them (moveFactorTo's flat branch, flatFrameBoundary after the refusal).
struct World {
    UiFlatPanelSettle settle;
    uint32_t renderW = 3840, renderH = 2160;   // the scene's size as the flat runtime's last copy measured it
    uint32_t outW = 3840, outH = 2160;         // the display
    float target = 1.25f;                      // fix.ui_quality 125
    float ss = 1.0f;                           // the game's live Supersampling
    bool live = false;                         // a factor has been written
    double floatsF = 1.0;                      // the factor in the floats
    std::vector<double> written;               // every factor put in the floats, the thunk's and the boundary's, in order
    bool pubReady = false;                     // the render thread's published plan (the thunk's input)
    double pubFormula = 0.0, pubBase = 0.0;
    float pubSs = 0.0f;
    uint32_t epoch = 0;                        // the thunk's record
    uint64_t at = 0;
    unsigned publishes = 0, refused = 0, held = 0, arrivals = 0, timeouts = 0;
    double worstLargest = 0.0;                 // the widest panel any plan or move could ask for
    UiFlatPanelSettle::Step last;

    void copy(uint32_t w, uint32_t h) { renderW = w; renderH = h; }

    // The game's setter: the thunk makes the factor for the new value from the plan last published and writes it if it is not there already.
    void setter(float newSs) {
        ss = newSs;
        if (!pubReady) return;
        UiPanelPlan p;
        if (!edvr::uiFlatPanelMove(pubFormula, pubBase, pubSs, newSs, &p)) return;
        worstLargest = std::max(worstLargest, p.largest);
        if (std::fabs(p.f - floatsF) <= 1e-9) return;
        floatsF = p.f;
        written.push_back(p.f);
        at = pack(renderW, renderH);
        ++epoch;
    }

    UiFlatPanelSettle::Step boundary() {
        UiFlatPanelInputs in;
        in.renderW = renderW; in.renderH = renderH; in.outputW = outW; in.outputH = outH; in.target = target;
        UiPanelPlan plan;
        if (edvr::uiFlatPanelPlanFor(in, &plan) != UiFlatPanelRefuse::kNone) {
            ++refused;
            settle.unsettle();
            last = UiFlatPanelSettle::Step{};
            return last;
        }
        worstLargest = std::max(worstLargest, plan.largest);
        const UiFlatPanelSettle::Step s = settle.step(plan.f, live, floatsF, epoch, static_cast<uint32_t>(at >> 32), static_cast<uint32_t>(at & 0xFFFFFFFFu), renderW, renderH);
        last = s;
        arrivals += s.arrived ? 1u : 0u;
        timeouts += s.timedOut ? 1u : 0u;
        if (s.act == Act::kHold) { ++held; return s; }
        if (s.act == Act::kWait) return s;
        pubReady = true;                       // published: the plan, and the Supersampling it was made beside
        pubFormula = plan.formula;
        pubBase = plan.base;
        pubSs = ss;
        ++publishes;
        if (s.act == Act::kKeep) return s;
        floatsF = plan.f;
        live = true;
        written.push_back(plan.f);
        return s;
    }
    unsigned run(unsigned n) { for (unsigned i = 0; i < n; ++i) boundary(); return n; }
    // Boundaries until the first factor is written (a cap of 40).
    unsigned untilWritten() { unsigned n = 0; while (written.empty() && n < 40) { boundary(); ++n; } return n; }
};

// A world settled on the review's old size: 3840x2160, UI 125, Supersampling 1.0, the factor 0.800 written and published.
World steady() {
    World w;
    w.untilWritten();
    return w;
}

bool onlyThese(const std::vector<double>& written, std::initializer_list<double> expect) {
    if (written.size() != expect.size()) return false;
    size_t i = 0;
    for (double e : expect) if (!isNear(written[i++], e, 1e-6)) return false;
    return true;
}

void testWiring();

void testAll() {
    // ---- the settle as it was --------------------------------------------------------------------------------------------------
    {
        World w;
        const unsigned n = w.untilWritten();
        check(n == 11 && onlyThese(w.written, {0.8}) && w.live && w.publishes == 1,
              "flat panel settle: a steady plan (3840x2160, UI 125) is written on the 11th boundary: 0.800, published, as before the change");
        check(w.run(5) == 5 && w.written.size() == 1 && w.publishes == 6 && w.last.act == Act::kKeep, "...and later boundaries keep it (settled, published, nothing more written)");
    }
    {
        World w = steady();
        w.copy(2560, 1440);
        unsigned n = 0;
        const size_t before = w.written.size();
        while (w.written.size() == before && n < 40) { w.boundary(); ++n; }
        check(n == 11 && isNear(w.floatsF, (1440.0 / 2160.0) / 1.25, 1e-9), "...a window resize with no setter still waits its 11 boundaries for the new size's factor");
    }
    {
        World w = steady();
        w.copy(2560, 1440);
        w.run(3);
        w.copy(1920, 1080);   // the second run of one view change
        w.run(3);
        check(w.written.size() == 1, "...and two runs of one view change write neither: the plan has to hold for the settle");
        w.run(40);
        check(w.written.size() == 2 && isNear(w.floatsF, 0.4), "...the last size's factor is the one written");
    }

    // ---- the review's case: the setter after the last old-size copy --------------------------------------------------------------
    {
        World w = steady();
        w.setter(0.5f);
        check(isNear(w.floatsF, 0.4) && w.epoch == 1 && onlyThese(w.written, {0.8, 0.4}), "flat panel setter: 3840x2160, UI 125, Supersampling 1.0 -> 0.5: the factor 0.800, the setter writes 0.400");
        bool reverted = false, published = false;
        unsigned holds = 0;
        for (int i = 0; i < 30; ++i) {   // the boundaries that still see the old size
            const UiFlatPanelSettle::Step s = w.boundary();
            reverted = reverted || isNear(w.floatsF, 0.8);
            published = published || s.publish;
            holds += s.act == Act::kHold ? 1u : 0u;
        }
        check(!reverted && holds == 30 && !published && isNear(w.floatsF, 0.4) && onlyThese(w.written, {0.8, 0.4}),
              "...30 boundaries on the old size's plan hold the setter's 0.400: never written back to 0.800, nothing published from the old size");
        w.copy(1920, 1080);
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.arrived && s.act == Act::kKeep && s.publish && s.waited == 30 && isNear(w.floatsF, 0.4) && onlyThese(w.written, {0.8, 0.4}),
              "...the new size arrives: its plan 0.400 is accepted on that boundary, it is the factor already there, nothing is written and nothing waits the 11 boundaries");
        check(w.run(3) == 3 && w.last.act == Act::kKeep && onlyThese(w.written, {0.8, 0.4}) && w.settle.holding() == false,
              "...and it stays settled: the boundaries after it keep 0.400, with nothing held");
    }
    {
        World w = steady();
        w.setter(0.5f);
        w.run(4);
        w.copy(1952, 1098);   // a render size that is not exactly half: the plan is 0.4067, 1.6% from the setter's
        const UiFlatPanelSettle::Step s = w.boundary();
        const bool ok = s.arrived && s.act == Act::kWrite && isNear(w.floatsF, (1098.0 / 2160.0) / 1.25, 1e-9) && w.written.size() == 3;
        if (!ok) std::fprintf(stderr, "  1952x1098: arrived %d act %d floats %.6f written %zu\n", s.arrived ? 1 : 0, static_cast<int>(s.act), w.floatsF, w.written.size());
        check(ok, "...a new size whose plan is not the setter's factor (1952x1098: 0.4067) is written on the boundary it arrives, not 11 boundaries later");
    }

    // ---- the setter before the copy ----------------------------------------------------------------------------------------------
    {
        World w = steady();
        w.setter(0.5f);      // no boundary has seen the old size since
        w.copy(1920, 1080);  // the final copy of the new size comes first
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.arrived && s.act == Act::kKeep && s.waited == 0 && isNear(w.floatsF, 0.4) && onlyThese(w.written, {0.8, 0.4}) && w.held == 0,
              "flat panel setter before the copy: the next boundary sees the new size, accepts its plan 0.400 at once, and the factor was never anything else");
    }

    // ---- unchanged dimensions ----------------------------------------------------------------------------------------------------
    {
        World w = steady();
        w.setter(0.5f);
        bool allHeld = true;
        for (unsigned i = 0; i < edvr::kUiPanelTransitionFrames; ++i) allHeld = allHeld && w.boundary().act == Act::kHold;
        check(allHeld && isNear(w.floatsF, 0.4) && w.held == edvr::kUiPanelTransitionFrames, "flat panel setter, size unchanged: the setter's factor is held for kUiPanelTransitionFrames boundaries");
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.timedOut && !s.arrived && s.waited == edvr::kUiPanelTransitionFrames && s.act == Act::kWrite && isNear(w.floatsF, 0.8) && onlyThese(w.written, {0.8, 0.4, 0.8}),
              "...then the plan from the size there is stands, written on that boundary (it was settled long ago): 0.800");
        check(w.run(3) == 3 && w.last.act == Act::kKeep && w.written.size() == 3 && !w.settle.holding(), "...and the boundaries after it keep it");
    }
    {
        World w = steady();
        w.setter(1.0f);      // the value it already has: the thunk moves nothing
        check(w.epoch == 0 && w.written.size() == 1, "flat panel setter that moves nothing: no epoch, nothing written");
        check(w.boundary().act == Act::kKeep && w.held == 0, "...so nothing is held");
        World cold;
        cold.setter(0.5f);   // before any plan was published
        check(cold.epoch == 0 && cold.written.empty() && cold.boundary().act == Act::kWait, "...and a setter before the first published plan holds nothing either");
    }

    // ---- rapid successive changes ------------------------------------------------------------------------------------------------
    {
        World w = steady();
        w.setter(0.5f);
        w.run(100);
        w.setter(0.75f);     // a second change, 100 boundaries into the first wait
        check(isNear(w.floatsF, 0.6) && w.epoch == 2, "flat panel rapid changes: a second setter 100 boundaries into the wait writes its own factor (0.75 x 0.800 = 0.600)");
        bool allHeld = true;
        for (int i = 0; i < 100; ++i) allHeld = allHeld && w.boundary().act == Act::kHold;
        check(allHeld && w.timeouts == 0 && isNear(w.floatsF, 0.6), "...and the wait starts over: 100 more boundaries (200 in all, past the 120 of one wait) still hold it");
        w.copy(2880, 1620);
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.arrived && s.act == Act::kKeep && onlyThese(w.written, {0.8, 0.4, 0.6}), "...the second change's size arrives: 0.600, kept; the first change's 0.400 was the only other factor written");
    }
    {
        World w = steady();
        w.setter(0.5f);
        w.copy(1920, 1080);
        const UiFlatPanelSettle::Step first = w.boundary();   // the first change has arrived and been accepted
        w.setter(0.75f);                                       // the second, from the plan the first published
        w.run(5);
        w.copy(2880, 1620);
        const UiFlatPanelSettle::Step second = w.boundary();
        check(first.arrived && second.arrived && w.held == 5 && onlyThese(w.written, {0.8, 0.4, 0.6}) && isNear(w.floatsF, 0.6),
              "flat panel rapid changes: a second setter after the first's size arrived (0.5, then 0.75): 0.400 then 0.600, each held until its own size, nothing stale written");
    }
    {
        World w = steady();
        w.setter(0.5f);
        w.copy(1920, 1080);   // the first change's copy ...
        w.setter(0.75f);      // ... and the second setter before the boundary has seen it
        const UiFlatPanelSettle::Step held = w.boundary();
        w.copy(2880, 1620);
        const UiFlatPanelSettle::Step arrived = w.boundary();
        check(held.act == Act::kHold && arrived.arrived && onlyThese(w.written, {0.8, 0.4, 0.6}) && isNear(w.floatsF, 0.6),
              "...and one between the first's copy and its boundary: the 1920x1080 plan 0.400 is not written over the second setter's 0.600; its own size ends the wait");
    }

    // ---- the cap and the floor ---------------------------------------------------------------------------------------------------
    {
        World w;
        w.renderW = 7680; w.renderH = 4320; w.outW = 7680; w.outH = 4320;   // an 8K screen at full render: 0.800
        w.untilWritten();
        w.setter(0.25f);      // a quarter render: x5 would be asked, capped at x4
        check(isNear(w.floatsF, 0.25), "flat panel cap: an 8K screen, Supersampling 1.0 -> 0.25: the setter's factor is capped at 0.250 (x4), not 0.200");
        w.run(7);
        w.copy(1920, 1080);
        const UiFlatPanelSettle::Step s = w.boundary();
        bool inRange = true;
        for (double f : w.written) inRange = inRange && f >= 0.25 - 1e-12 && f <= 1.0 + 1e-12;
        check(s.arrived && s.act == Act::kKeep && isNear(w.floatsF, 0.25) && inRange && w.worstLargest <= edvr::kUiPanelBudget,
              "...held while the old size shows, kept when 1920x1080 arrives (its plan is the cap too); no factor outside [1/4, 1] and no panel over the budget");
    }
    {
        World w;
        w.renderW = 1920; w.renderH = 1080; w.outW = 1920; w.outH = 1080;   // a 1080p screen at full render, UI 125: 0.800
        w.untilWritten();
        w.setter(2.0f);       // double render: 1.600, floored at 1
        check(isNear(w.floatsF, 1.0), "flat panel floor: a 1080p screen, Supersampling 1.0 -> 2.0: the setter's factor is floored at 1.000 (the game's own panels)");
        w.run(4);
        w.copy(3840, 2160);
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.arrived && s.act == Act::kKeep && isNear(w.floatsF, 1.0) && w.worstLargest <= edvr::kUiPanelBudget && onlyThese(w.written, {0.8, 1.0}),
              "...kept when 3840x2160 arrives (its plan is the floor too); the widest panel stays inside the budget");
    }

    // ---- frames with no plan, the key off -----------------------------------------------------------------------------------------
    {
        World w = steady();
        w.copy(2560, 1440);
        w.run(5);              // half the settle
        w.copy(512, 512);      // a frame with no plan restarts it
        w.run(2);
        w.copy(2560, 1440);
        unsigned n = 0;
        const size_t before = w.written.size();
        while (w.written.size() == before && n < 40) { w.boundary(); ++n; }
        check(n == 10, "flat panel refused frames: a frame with no plan restarts the settle (10 more boundaries on the same size, not the 5 that were left)");
    }
    {
        World w = steady();
        w.setter(0.5f);
        w.run(5);
        w.copy(512, 512);     // a 512x512 preview: no plan (the aspect)
        w.run(3);
        check(w.refused == 3 && w.settle.holding() && w.held == 5 && isNear(w.floatsF, 0.4), "flat panel refused frames: a frame with no plan neither writes nor ends the hold, and does not count toward its wait");
        w.copy(1920, 1080);
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.arrived && s.waited == 5 && s.act == Act::kKeep, "...the wait resumes where it stopped: the new size arrives after 5 held boundaries");
    }
    {
        World w = steady();
        w.setter(0.5f);
        w.run(2);
        check(w.settle.holding(), "flat panel key off: a held factor is held");
        w.settle.forget();    // the key off, or the anti-aliasing off
        check(!w.settle.holding(), "...and forgotten when the key is turned off (the floats are the game's own then; nothing carries to the next time it is on)");
    }

    testWiring();
}

// ---- the wiring, by source scan ----------------------------------------------------------------------------------------------------

struct Pin { const char* id; bool ok; const char* what; };

std::vector<Pin> pins(const std::string& scale) {
    using afterui::functionBody;
    using afterui::squeeze;
    using worldroute::countOf;
    using worldroute::has;
    using worldroute::inOrder;
    auto body = [&](const char* signature) {
        std::string b;
        return functionBody(scale, signature, &b) ? squeeze(b) : std::string();
    };
    const std::string frame = body("void flatFrameBoundary(float target) {");
    const std::string move = body("void moveFactorTo(float ss) {");
    std::vector<Pin> out;
    out.push_back({"boundary-asks", has(frame, "g_flatSettle.step(plan.f,g_live.load(std::memory_order_acquire),uiPanelScaleFactor(),setterEpoch,") &&
                                        has(frame, "in.renderW,in.renderH);") && !has(frame, "++g_settle") && !has(frame, "g_pending=plan.f"),
                   "the flat boundary asks UiFlatPanelSettle with the plan, whether a factor is live, the factor in the floats, the setter's epoch and size, and the scene's size; it keeps no settle of its own"});
    out.push_back({"hold-before-publish", inOrder(frame, {"step.act==UiFlatPanelSettle::Act::kHold", "return;", "step.act==UiFlatPanelSettle::Act::kWait", "return;",
                                                          "readLiveSupersampling(&cur,&lo,&hi)", "g_pubFlatSsBits.store(sb,", "step.act==UiFlatPanelSettle::Act::kKeep", "return;",
                                                          "writeFloats(plan.f,plan.lineF,1.0)"}),
                   "a held factor and an unsettled plan return before anything is published or written; a kept one returns after the publish and before the write"});
    out.push_back({"epoch-then-size", inOrder(frame, {"g_flatSetterEpoch.load(std::memory_order_acquire)", "g_flatSetterAt.load(std::memory_order_acquire)", "g_flatSettle.step("}),
                   "the boundary reads the setter's epoch before the size it was recorded with"});
    out.push_back({"publishes-size", has(frame, "g_pubFlatDims.store((static_cast<uint64_t>(in.renderW)<<32)|in.renderH,std::memory_order_release);"),
                   "the published plan carries the scene size it was made at (the thunk's fallback when the runtime cannot say)"});
    out.push_back({"thunk-records", inOrder(move, {"if(writeFloats(p.f,p.lineF,p.ss)){", "if(g_pubFlat.load(std::memory_order_acquire)){", "flatRuntimeSceneSizes(&w,&h,&ow,&oh)",
                                                   "g_flatSetterAt.store(at,std::memory_order_release);", "g_flatSetterEpoch.fetch_add(1,std::memory_order_acq_rel);"}),
                   "the setter thunk, after it writes a flat factor, records the scene's size and then counts the move"});
    out.push_back({"refused-unsettles", has(frame, "++g_flatRefusedFrames;g_flatSettle.unsettle();return;"),
                   "a refused frame restarts the settle and leaves the held factor alone"});
    out.push_back({"forgets", countOf(scale, "g_flatSettle.forget();") == 2,
                   "the key off and the anti-aliasing off both forget the class's state"});
    return out;
}

void testWiring() {
    const std::string scale = worldroute::readText("src/d3d11/ui_panel_scale.cpp");
    check(!scale.empty(), "flat panel wiring: src/d3d11/ui_panel_scale.cpp is readable from the working directory (the rig runs from the repo root)");
    if (scale.empty()) return;
    for (const Pin& p : pins(scale)) {
        char msg[420];
        std::snprintf(msg, sizeof(msg), "flat panel wiring [%s]: %s", p.id, p.what);
        check(p.ok, msg);
    }
    // CONTROLS: one edit each that puts a likely slip back; the pin named must fail.
    struct Control { const char* pin; const char* from; const char* to; };
    const Control controls[] = {
        {"boundary-asks", "g_flatSettle.step(plan.f,", "g_flatSettle.stepOnce(plan.f,"},
        {"boundary-asks", "uiPanelScaleFactor(), setterEpoch,", "1.0, setterEpoch,"},
        {"hold-before-publish", "if (step.act == UiFlatPanelSettle::Act::kHold) {\n        ++g_flatHeldFrames;\n        return;\n    }", "++g_flatHeldFrames;"},
        {"hold-before-publish", "if (step.act == UiFlatPanelSettle::Act::kWait) return;", ""},
        {"hold-before-publish", "if (step.act == UiFlatPanelSettle::Act::kKeep) return;", ""},
        {"epoch-then-size", "g_flatSetterEpoch.load(std::memory_order_acquire);\n    const uint64_t setterAt = g_flatSetterAt.load(std::memory_order_acquire);",
                            "0;\n    const uint64_t setterAt = g_flatSetterAt.load(std::memory_order_acquire);"},
        {"publishes-size", "g_pubFlatDims.store((static_cast<uint64_t>(in.renderW) << 32) | in.renderH, std::memory_order_release);", ""},
        {"thunk-records", "g_flatSetterEpoch.fetch_add(1, std::memory_order_acq_rel);", ""},
        {"thunk-records", "g_flatSetterAt.store(at, std::memory_order_release);", ""},
        {"refused-unsettles", "g_flatSettle.unsettle();", ""},
        {"forgets", "g_flatSettle.forget();", ""},
    };
    for (const Control& c : controls) {
        const size_t at = scale.find(c.from);
        char msg[420];
        if (at == std::string::npos) {
            std::snprintf(msg, sizeof(msg), "flat panel control for [%s]: the text it edits ('%s') is in the source", c.pin, c.from);
            check(false, msg);
            continue;
        }
        std::string edited = scale;
        edited.replace(at, std::strlen(c.from), c.to);
        bool tripped = true;
        for (const Pin& p : pins(edited))
            if (std::strcmp(p.id, c.pin) == 0) tripped = !p.ok;
        std::snprintf(msg, sizeof(msg), "flat panel control: one edit ('%.60s') trips wiring [%s]", c.from, c.pin);
        check(tripped, msg);
    }
}

}  // namespace flatsettle
