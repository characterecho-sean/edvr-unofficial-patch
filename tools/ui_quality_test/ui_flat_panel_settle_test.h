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
//   * the two sections as ONE operation under the floats' lock (the follow-up review of dbbbcf03, finding 2): the setter landing at each point of the boundary's section, a
//     boundary that has decided to write included, and the boundary landing in the setter's, never overwrite the setter's factor (testRace);
// Every mutation of the class and of the sections is seen to fail one of these cases (the design doc's entries list them).

namespace flatsettle {

using edvr::UiFlatPanelSettle;
using edvr::UiFlatPanelInputs;
using edvr::UiFlatPanelRefuse;
using edvr::UiPanelPlan;
using Act = UiFlatPanelSettle::Act;

constexpr uint64_t pack(uint32_t w, uint32_t h) { return (static_cast<uint64_t>(w) << 32) | h; }
bool isNear(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// The thunk and the boundary around the class, as ui_panel_scale.cpp has them: THESE are the production statements (ui_sizing_math.h's uiFlatPanelBoundarySection and
// uiPanelSetterSection), run over this Env. The Env's lock is a depth count: a thread that arrives while it is held (an injected setter or boundary, see injectAt) waits for it
// and runs when the holder lets go, as the real one does.
struct World {
    UiFlatPanelSettle settle;
    uint32_t renderW = 3840, renderH = 2160;   // the scene's size as the flat runtime's last copy measured it
    uint32_t outW = 3840, outH = 2160;         // the display
    float target = 1.25f;                      // fix.ui_quality 125
    float ss = 1.0f;                           // the game's live Supersampling
    bool live = false;                         // a factor has been written
    double floatsF = 1.0, floatsLine = 1.0;    // the factor in the floats, and the orbit lines' beside it
    std::vector<double> written;               // every factor put in the floats, the thunk's and the boundary's, in order
    bool pubReady = false;                     // the render thread's published plan (the thunk's input)
    double pubFormula = 0.0, pubBase = 0.0;
    float pubSs = 0.0f;
    uint64_t pubDims = 0;
    uint32_t epoch = 0;                        // the thunk's record
    uint64_t at = 0;
    unsigned publishes = 0, refused = 0, held = 0, arrivals = 0, timeouts = 0;
    double worstLargest = 0.0;                 // the widest panel any plan or move could ask for
    UiFlatPanelSettle::Step last;

    // ---- the Env of the two sections ----
    int depth = 0;                                          // the floats' lock: held while above 0
    std::vector<std::function<void()>> deferred;            // threads that arrived while it was held
    struct Injection { bool armed = false; edvr::UiFlatSeam where = edvr::UiFlatSeam::kBeforeLock; std::function<void()> action; } inj;
    void lock() { ++depth; }
    void unlock() {
        if (--depth != 0) return;
        while (!deferred.empty()) {   // the waiting thread takes the lock now (and may be injected into in its turn)
            std::function<void()> next = deferred.front();
            deferred.erase(deferred.begin());
            next();
        }
    }
    // An injection point: the armed action fires once, at this seam. Holding the lock, it waits for it; not holding it (the lock was left out), it runs on the spot.
    void seam(edvr::UiFlatSeam where) {
        if (!inj.armed || inj.where != where) return;
        inj.armed = false;
        if (depth > 0) deferred.push_back(inj.action);
        else inj.action();
    }
    void injectAt(edvr::UiFlatSeam where, std::function<void()> action) { inj.armed = true; inj.where = where; inj.action = std::move(action); }
    uint32_t setterEpoch() const { return epoch; }
    uint64_t setterAt() const { return at; }
    bool isLive() const { return live; }
    double factorNow() const { return floatsF; }
    double lineFactorNow() const { return floatsLine; }
    void publish(const UiPanelPlan& plan, uint32_t w, uint32_t h) {   // published: the plan, and the Supersampling it was made beside
        pubReady = true;
        pubFormula = plan.formula;
        pubBase = plan.base;
        pubSs = ss;
        pubDims = pack(w, h);
        ++publishes;
    }
    bool writeFactors(double f, double lineF, double) {
        floatsF = f;
        floatsLine = lineF;
        written.push_back(f);
        return true;
    }
    edvr::UiPublishedPlan published() const {
        edvr::UiPublishedPlan r;
        r.ready = pubReady;
        r.flat = true;
        r.formula = pubFormula;
        r.base = pubBase;
        r.ss = pubSs;
        r.dims = pubDims;
        return r;
    }
    void noteMove(bool flat, uint64_t publishedDims) {
        if (!flat) return;
        at = renderW && renderH ? pack(renderW, renderH) : publishedDims;
        ++epoch;
    }

    void copy(uint32_t w, uint32_t h) { renderW = w; renderH = h; }

    // The game's setter: the thunk makes the factor for the new value from the plan last published and writes it if it is not there already; the game stores the value after.
    void setter(float newSs) {
        UiPanelPlan p;
        if (pubReady && edvr::uiFlatPanelMove(pubFormula, pubBase, pubSs, newSs, &p)) worstLargest = std::max(worstLargest, p.largest);
        edvr::uiPanelSetterSection(*this, newSs);
        ss = newSs;
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
        bool wrote = false;
        const UiFlatPanelSettle::Step s = edvr::uiFlatPanelBoundarySection(settle, *this, plan, renderW, renderH, &wrote);
        last = s;
        arrivals += s.arrived ? 1u : 0u;
        timeouts += s.timedOut ? 1u : 0u;
        if (s.act == Act::kHold) ++held;
        if (wrote) live = true;
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
void testRace();

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

    testRace();
    testWiring();
}

// ---- the deterministic interleavings (the follow-up review of dbbbcf03, finding 2) ---------------------------------------------------------
// The review's order: the boundary reads the epoch (0), the setter writes 0.400 and publishes epoch 1, the boundary reads the factor (0.400) and decides on a settled plan of
// 0.800, writes it, and the next boundary holds the overwritten value. The sections are one operation under the floats' lock now, so the thread that arrives in the middle of
// the other waits for it (World::seam defers it to the holder's unlock, or runs it on the spot where the lock was left out). The setter lands at each point inside the
// boundary's section, the boundary at the point inside the setter's, and before the boundary takes the lock.

const char* seamName(edvr::UiFlatSeam s) {
    switch (s) {
        case edvr::UiFlatSeam::kBeforeLock: return "before the boundary takes the lock";
        case edvr::UiFlatSeam::kEpochRead: return "between the boundary's epoch read and its decision";
        case edvr::UiFlatSeam::kDecided: return "between the boundary's decision and its publish";
        case edvr::UiFlatSeam::kPublished: return "between the boundary's publish and its write";
        case edvr::UiFlatSeam::kWritten: return "after the boundary's write";
        case edvr::UiFlatSeam::kSetterWritten: return "in the setter, between its write and its epoch";
    }
    return "?";
}

void testRace() {
    using edvr::UiFlatSeam;
    const UiFlatSeam inside[] = {UiFlatSeam::kEpochRead, UiFlatSeam::kDecided, UiFlatSeam::kPublished, UiFlatSeam::kWritten};
    char msg[420];
    // A. The review's case: the plan is settled at 0.800 and the boundary keeps it; the setter (0.5) lands in the boundary's section.
    for (const UiFlatSeam where : inside) {
        World w = steady();
        w.injectAt(where, [&w] { w.setter(0.5f); });
        const UiFlatPanelSettle::Step s = w.boundary();
        const bool landed = !w.inj.armed && w.depth == 0 && w.deferred.empty();
        std::snprintf(msg, sizeof(msg), "flat panel race: a setter that lands %s waits for the section and then writes 0.400; the settled 0.800 is never written over it", seamName(where));
        check(landed && s.act == Act::kKeep && isNear(w.floatsF, 0.4) && w.epoch == 1 && onlyThese(w.written, {0.8, 0.4}), msg);
        const UiFlatPanelSettle::Step next = w.boundary();
        std::snprintf(msg, sizeof(msg), "...and the next boundary sees the epoch and holds 0.400, as designed (%s)", seamName(where));
        check(next.act == Act::kHold && w.settle.holding() && isNear(w.floatsF, 0.4) && onlyThese(w.written, {0.8, 0.4}), msg);
        w.run(3);
        w.copy(1920, 1080);
        const UiFlatPanelSettle::Step arrived = w.boundary();
        std::snprintf(msg, sizeof(msg), "...and the new size arriving is accepted at once with nothing written (%s)", seamName(where));
        check(arrived.arrived && arrived.act == Act::kKeep && w.held == 4 && onlyThese(w.written, {0.8, 0.4}), msg);
    }
    // B. The setter lands after the boundary has decided to write (a window resize settled at 0.533): the write goes first, then the setter moves the factor from the plan
    //    the boundary has just published. Nothing writes over the setter.
    for (const UiFlatSeam where : inside) {
        World w = steady();
        w.copy(2560, 1440);
        w.run(10);   // the new size has held ten boundaries: the next one writes its factor
        const double resized = (1440.0 / 2160.0) / 1.25, moved = resized * 0.5;
        w.injectAt(where, [&w] { w.setter(0.5f); });
        const UiFlatPanelSettle::Step s = w.boundary();
        std::snprintf(msg, sizeof(msg), "flat panel race: a setter that lands %s a boundary that writes 0.533 waits for it; the factor ends at the setter's 0.267, never 0.533 over it", seamName(where));
        check(s.act == Act::kWrite && isNear(w.floatsF, moved, 1e-6) && w.epoch == 1 && onlyThese(w.written, {0.8, resized, moved}) && w.depth == 0 && w.deferred.empty(), msg);
        const UiFlatPanelSettle::Step next = w.boundary();
        std::snprintf(msg, sizeof(msg), "...and the next boundary holds it (%s)", seamName(where));
        check(next.act == Act::kHold && isNear(w.floatsF, moved, 1e-6) && onlyThese(w.written, {0.8, resized, moved}), msg);
        w.copy(1280, 720);
        const UiFlatPanelSettle::Step arrived = w.boundary();
        std::snprintf(msg, sizeof(msg), "...and the size it was moved for arrives: 0.267, kept (%s)", seamName(where));
        check(arrived.arrived && arrived.act == Act::kKeep && onlyThese(w.written, {0.8, resized, moved}), msg);
    }
    // C. The setter ran before the boundary took the lock: the boundary sees the epoch and holds, whether its plan is the settled 0.800 or a resize's 0.533 about to be written.
    {
        World w = steady();
        w.injectAt(UiFlatSeam::kBeforeLock, [&w] { w.setter(0.5f); });
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.act == Act::kHold && isNear(w.floatsF, 0.4) && onlyThese(w.written, {0.8, 0.4}), "flat panel race: a setter that completed before the boundary took the lock is seen: the boundary holds 0.400");
    }
    {
        World w = steady();
        w.copy(2560, 1440);
        w.run(10);
        w.injectAt(UiFlatSeam::kBeforeLock, [&w] { w.setter(0.5f); });
        const UiFlatPanelSettle::Step s = w.boundary();
        check(s.act == Act::kHold && isNear(w.floatsF, 0.4) && onlyThese(w.written, {0.8, 0.4}), "...and so is one whose resize plan was about to be written: 0.533 is not written over the setter's 0.400");
    }
    // D. The other direction: a boundary arrives in the setter's section, between its write and its epoch. It waits, then sees the epoch with the factor it goes with.
    {
        World w = steady();
        w.injectAt(UiFlatSeam::kSetterWritten, [&w] { w.boundary(); });
        w.setter(0.5f);
        check(w.depth == 0 && w.deferred.empty() && w.held == 1 && w.last.act == Act::kHold && w.settle.holding() && isNear(w.floatsF, 0.4) && w.epoch == 1 && onlyThese(w.written, {0.8, 0.4}),
              "flat panel race: a boundary that arrives in the setter's section (its 0.400 written, its epoch not yet) waits for it, then holds 0.400 under epoch 1 (it used to write 0.800 back)");
    }
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
    // PanelEnv is a struct, whose closing brace is "};" in column 0
    std::string env;
    {
        const size_t s = scale.find("struct PanelEnv {");
        const size_t e = s == std::string::npos ? s : scale.find("\n};\n", s);
        if (e != std::string::npos) env = squeeze(scale.substr(s, e - s + 3));
    }
    const std::string frame = body("void flatFrameBoundary(float target) {");
    const std::string move = body("void moveFactorTo(float ss) {");
    const std::string locked = body("bool writeFloats(double f, double lineF = 1.0, double ss = 1.0) {");
    const std::string unlocked = body("bool writeFloatsUnlocked(double f, double lineF, double ss) {");
    std::vector<Pin> out;
    out.push_back({"boundary-section", has(frame, "uiFlatPanelBoundarySection(g_flatSettle,env,plan,in.renderW,in.renderH,&wrote);") && !has(frame, "g_flatSettle.step(") &&
                                           !has(frame, "g_flatSetterEpoch") && !has(frame, "writeFloats(plan") && !has(frame, "++g_settle") && !has(frame, "g_pending=plan.f"),
                   "the flat boundary's epoch read, decision, publish and write are ONE call, uiFlatPanelBoundarySection, under the floats' lock; the boundary itself reads no epoch, decides nothing and writes nothing"});
    out.push_back({"live-before-lock-log-after", inOrder(frame, {"readLiveSupersampling(&cur,&lo,&hi)", "uiFlatPanelBoundarySection(", "Log::get().note("}),
                   "the game's live Supersampling is read before the section, and the log is written after it"});
    out.push_back({"hold-then-wait-return", inOrder(frame, {"step.act==UiFlatPanelSettle::Act::kHold", "return;", "step.act==UiFlatPanelSettle::Act::kWait", "step.act==UiFlatPanelSettle::Act::kKeep",
                                                            "return;", "if(!wrote)return;", "g_written=plan.f;"}),
                   "a held factor, an unsettled plan and a kept one return, and the boundary's bookkeeping follows only a write that happened"});
    out.push_back({"env-lock", has(env, "voidlock(){AcquireSRWLockExclusive(&g_floatLock);}") && has(env, "voidunlock(){ReleaseSRWLockExclusive(&g_floatLock);}"),
                   "the sections' lock is the floats' lock, the one writeFloats has always taken"});
    out.push_back({"env-nothing-slow", !env.empty() && !has(env, "Log::") && !has(env, "readLiveSupersampling(") && !has(env, "writeFloats(") && !has(env, "readGame(") &&
                                           !has(env, "Sleep") && !has(env, "WaitFor") && has(env, "boolwriteFactors(doublef,doublelineF,doubless){returnwriteFloatsUnlocked(f,lineF,ss);}"),
                   "under the lock the Env does atomic loads and stores, the floats' write without the lock again, and one load of the scene's size: no log, no read of the game's context, no wait"});
    out.push_back({"write-split", !has(unlocked, "SRWLock") && has(unlocked, "VirtualProtect(") && inOrder(locked, {"AcquireSRWLockExclusive(&g_floatLock);", "writeFloatsUnlocked(f,lineF,ss);", "ReleaseSRWLockExclusive(&g_floatLock);"}),
                   "writeFloats is the lock around writeFloatsUnlocked, which takes none (the sections call the second under the lock they hold)"});
    out.push_back({"move-through-section", has(move, "PanelEnvenv;") && has(move, "uiPanelSetterSection(env,ss);") && !has(move, "writeFloats("),
                   "the setter thunk's move is uiPanelSetterSection: the published plan, the factor, its write and the epoch as one operation"});
    out.push_back({"thunk-records", inOrder(env, {"voidnoteMove(boolflat,uint64_tpublishedDims){", "flatRuntimeSceneSizes(&w,&h,&ow,&oh)", "g_flatSetterAt.store(at,std::memory_order_release);",
                                                  "g_flatSetterEpoch.fetch_add(1,std::memory_order_acq_rel);"}),
                   "the setter's move records the scene's size and then counts itself"});
    out.push_back({"publishes-size", has(env, "g_pubFlatDims.store((static_cast<uint64_t>(renderW)<<32)|renderH,std::memory_order_release);"),
                   "the published plan carries the scene size it was made at (the thunk's fallback when the runtime cannot say)"});
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
        {"boundary-section", "uiFlatPanelBoundarySection(g_flatSettle, env,", "uiFlatPanelBoundaryUnlocked(g_flatSettle, env,"},
        {"boundary-section", "bool wrote = false;\n", "bool wrote = false;\n    const uint32_t again = g_flatSetterEpoch.load(std::memory_order_acquire);\n"},
        {"live-before-lock-log-after", "env.liveOk = readLiveSupersampling(&cur, &lo, &hi) == UiSsRead::kOk && uiLiveSupersamplingValid(cur, lo, hi, nullptr);", "env.liveOk = false;"},
        {"hold-then-wait-return", "if (step.act == UiFlatPanelSettle::Act::kHold) {\n        ++g_flatHeldFrames;\n        return;\n    }", "++g_flatHeldFrames;"},
        {"hold-then-wait-return", "if (step.act == UiFlatPanelSettle::Act::kWait || step.act == UiFlatPanelSettle::Act::kKeep) return;", ""},
        {"hold-then-wait-return", "if (!wrote) return;", ""},
        {"env-lock", "void lock() { AcquireSRWLockExclusive(&g_floatLock); }", "void lock() {}"},
        {"env-nothing-slow", "bool writeFactors(double f, double lineF, double ss) { return writeFloatsUnlocked(f, lineF, ss); }",
                             "bool writeFactors(double f, double lineF, double ss) { return writeFloats(f, lineF, ss); }"},
        {"env-nothing-slow", "    void noteMove(bool flat, uint64_t publishedDims) {\n", "    void noteMove(bool flat, uint64_t publishedDims) {\n        Log::get().note(\"moved\");\n"},
        {"write-split", "    const bool ok = writeFloatsUnlocked(f, lineF, ss);\n    ReleaseSRWLockExclusive(&g_floatLock);", "    const bool ok = writeFloatsUnlocked(f, lineF, ss);"},
        {"move-through-section", "uiPanelSetterSection(env, ss);", ""},
        {"thunk-records", "g_flatSetterEpoch.fetch_add(1, std::memory_order_acq_rel);", ""},
        {"thunk-records", "g_flatSetterAt.store(at, std::memory_order_release);", ""},
        {"publishes-size", "g_pubFlatDims.store((static_cast<uint64_t>(renderW) << 32) | renderH, std::memory_order_release);", ""},
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
