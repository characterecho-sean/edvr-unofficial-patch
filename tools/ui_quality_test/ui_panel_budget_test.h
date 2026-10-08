// Included last in ui_quality_test.cpp (check, near1; afterui::squeeze and functionBody; worldroute::readText and has).
//
// THE PANEL FACTOR'S SUPERSAMPLING TERM AND SIZE BUDGET (docs/ui-layer-2026-09-23.md, "2026-10-07: Supersampling and the
// panel budget"). A Frontier VR launch at Elite's Supersampling 2.0 with fix.ui_quality 125 and HMD Quality 0.5 died within
// seconds: the panel patch's f (0.4, from W_ui = 2016 alone) asked D3D11 for a 19200x10800 render-to-texture panel, which is
// over the 16384 limit, and Elite aborts on any refused create. This rig reproduces those numbers with the production
// arithmetic (uiPanelPlanFor, uiPanelDivisors, uiPanelSize) and holds:
//   * the request the game's own arithmetic makes -- stage x c / (1920 x f), c the UI record's width -- stays within 16384 at
//     the crash numbers and at every point of a grid of headsets, qualities, targets, Supersamplings, displays and 2D-screen
//     widths, whatever the factor is made of;
//   * at Supersampling <= 1 the factor is the pre-change expression bit for bit, on that grid and the flights' states;
//   * ui_quality 100 and HMD Quality 0.4; an 8K display, where the budget (not the Supersampling term) is what holds;
//   * the MUTANTS -- the Supersampling term removed, the budget removed, the term twice, the term inverted, the whole change
//     removed -- each run through the very comparators the real plan passes, and each caught;
//   * the orbit lines' factor follows: the layer/render ratio the width patch needs is the factor with the Supersampling in
//     it and without the budget (orbital_width.h's own decision is run on it);
//   * the file reader for the display's size, and the wiring by source scan (with controls that edit a copy and must trip).
// (orbital_width.h is included at the top of ui_quality_test.cpp, outside the anonymous namespace this file sits in.)

namespace panelbudget {

constexpr double kLimit = kUiPanelTextureLimit;  // 16384, D3D11's

// A state the rig builds a plan for: the runtime's recommendation and the game's HMD Quality as the flight logs say, the
// frustum (up == down), the target, and what the budget reads.
struct Rig {
    uint32_t askW = 4032;       // what the game is told (Pimax Crystal Super at fix.openxr_resolution 4032)
    float hmd = 0.5f;           // HMD Quality
    uint32_t outW = 4032;       // the runtime's untrimmed recommendation
    float tangent = 1.0293f;    // up == down, the told and the true frustum alike (91.7 degrees)
    float target = 1.25f;       // fix.ui_quality 125
    float ss = 2.0f;            // Elite's Supersampling
    uint32_t displayW = 3840;   // DisplaySettings.xml
    uint32_t screenW = 3712;    // the 2D screen's forced width (vScreen auto)
};

UiPanelInputs inputsOf(const Rig& r) {
    UiPanelInputs in;
    in.renderW = uiQualityInternalDim(r.askW, r.hmd);
    in.fovTangent = uiQualityFovTangent(r.tangent, r.tangent);
    in.outputW = r.outW;
    in.trueTangent = uiQualityFovTangent(r.tangent, r.tangent);
    in.target = r.target;
    in.supersampling = r.ss;
    in.displayW = r.displayW;
    in.screenW = r.screenW;
    return in;
}

// The pre-change expression, frozen: uiPanelFactor before 2026-10-07, which knew neither the Supersampling nor the budget.
double legacyFactor(const UiPanelInputs& in) {
    const double k = uiSizingK(in.fovTangent), kOut = uiSizingK(in.trueTangent);
    double v = (static_cast<double>(in.renderW) * k) / (static_cast<double>(in.outputW) * kOut) /
               static_cast<double>(in.target);
    if (v < 0.25)
        v = 0.25;
    else if (v > 1.0)
        v = 1.0;
    return v;
}

bool sameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

// The width of the widest panel the game's own arithmetic asks for at factor f: the 1920-wide stage, a UI record whose
// c is `c` (c = the record's width x k x Supersampling; k is 1 in the states that matter, see below), the divisor the
// patched operand holds. The production functions, not a model.
uint32_t requestWidth(double c, double f) {
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(f, &d1080, &d1920);
    return uiPanelSize(1920, c, d1920);
}
uint32_t requestHeight(double c, double f) {
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(f, &d1080, &d1920);
    return uiPanelSize(1080, c, d1920);
}

// The widths of c the record can hold in a state (ui_sizing_math.h): the display, the 2D screen's forced width and the
// observed base, each times the Supersampling the game applies (above 1 only); the scene's own width x k likewise.
void statesOf(const Rig& r, const UiPanelInputs& in, double out[4]) {
    const double s = r.ss > 1.0f ? static_cast<double>(r.ss) : 1.0;
    out[0] = static_cast<double>(r.displayW) * s;
    out[1] = static_cast<double>(r.screenW) * s;
    out[2] = kUiPanelObservedBase * s;
    out[3] = static_cast<double>(in.renderW) * uiSizingK(in.fovTangent) * s;
}

// THE COMPARATORS the real plan passes and every mutant must fail. `trueSs` is the Supersampling the game really applies.
//   limitHolds: no state's request, at the factor under test, is over 16384 on either axis (when f < 1; at f = 1 the
//               request is the game's own, not this patch's).
bool limitHolds(const Rig& r, double f) {
    const UiPanelInputs in = inputsOf(r);
    double c[4];
    statesOf(r, in, c);
    if (f >= 1.0) return true;
    for (double v : c)
        if (requestWidth(v, f) > kLimit || requestHeight(v, f) > kLimit) return false;
    return true;
}
bool factorIs(double f, double expected) { return std::fabs(f - expected) < 1e-9; }

double planF(const Rig& r) {
    UiPanelPlan p;
    return uiPanelPlanFor(inputsOf(r), &p) ? p.f : -1.0;
}

// The variants. `real` is production; the rest are the wrong things that must be caught.
enum class Variant { kReal, kNoSupersampling, kNoBudget, kSupersamplingTwice, kSupersamplingInverted, kWholeChangeRemoved };
double factorOf(const Rig& r, Variant v) {
    const UiPanelInputs in = inputsOf(r);
    UiPanelPlan p;
    if (!uiPanelPlanFor(in, &p)) return -1.0;
    const double formula = p.formula;
    const double ss = p.ss;
    switch (v) {
        case Variant::kReal:
            return p.f;
        case Variant::kNoSupersampling: {  // the term left out, the budget kept as it would be if it saw S = 1
            UiPanelInputs m = in;
            m.supersampling = 1.0f;
            UiPanelPlan q;
            return uiPanelPlanFor(m, &q) ? q.f : -1.0;
        }
        case Variant::kNoBudget:  // the Supersampling term kept, the budget dropped
            return uiPanelClampF(formula * ss);
        case Variant::kSupersamplingTwice:
            return uiPanelClampF(formula * ss * ss);
        case Variant::kSupersamplingInverted:
            return uiPanelClampF(formula / ss);
        case Variant::kWholeChangeRemoved:
            return legacyFactor(in);
    }
    return -1.0;
}

void testCrashNumbers() {
    Rig r;  // Pimax Crystal Super, 4032 wide, HMD Quality 0.5, ui_quality 125, Supersampling 2.0, a 4K window
    const UiPanelInputs in = inputsOf(r);
    check(in.renderW == 2016 && near1(uiSizingK(in.fovTangent), 0.9649, 5e-4),
          "the crash flight's state: W_ui 2016 and k 0.9649, as the log's panel line has them");
    const double legacy = legacyFactor(in);
    check(near1(legacy, 0.4, 1e-6), "without the term f is 0.4 -- the log's 'f 0.4000' and 'x2.5000'");
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(legacy, &d1080, &d1920);
    check(near1(d1080, 432.0, 1e-3) && near1(d1920, 768.0, 1e-3),
          "...and the floats read 432 and 768, as the log's line has them");
    // The refused create, reproduced: the 1920x1080 stage at a record of 3840 x 2.0 = 7680, divided by 1920 x 0.4.
    const double cCrash = 3840.0 * 2.0;
    check(requestWidth(cCrash, legacy) == 19200 && requestHeight(cCrash, legacy) == 10800,
          "the refused create reproduced: a 1920x1080 stage at Supersampling 2.0 on a 3840 window asks for 19200x10800");
    check(requestWidth(cCrash, legacy) > kLimit, "...which is over D3D11's 16384");
    // The same state under the plan.
    UiPanelPlan p;
    check(uiPanelPlanFor(in, &p), "the plan is made from the crash flight's inputs");
    check(factorIs(p.f, 0.8) && p.ssActs && !p.budgetActs && p.clamp == UiPanelClamp::kNone,
          "at Supersampling 2.0 the Supersampling term doubles f: 0.8, x1.25 -- the game is already twice as dense");
    check(requestWidth(cCrash, p.f) == 9600 && requestHeight(cCrash, p.f) == 5400,
          "the request the same state makes under the plan: 9600x5400, within 16384");
    std::printf("  panel budget: the crash flight (HMD 0.5, ui_quality 125, Supersampling 2.0, a 3840 window): f %.4f -> %.4f, "
                "the 1920x1080 stage asks %ux%u -> %ux%u (D3D11 limit 16384)\n",
                legacy, p.f, requestWidth(cCrash, legacy), requestHeight(cCrash, legacy), requestWidth(cCrash, p.f),
                requestHeight(cCrash, p.f));
    check(limitHolds(r, p.f), "...and so does every other state the record takes (display, 2D screen, observed base, scene)");
    check(near1(p.largest, 9600.0, 1e-6) && near1(p.largestBefore, 19200.0, 1e-6) && p.base == 3840.0 &&
              p.baseFrom == UiPanelBase::kObserved,
          "the plan's own prediction: 9600 px at f 0.8, 19200 px at the old f -- from the 3840 base the refused create measured");
    check(near1(p.lineF, 0.8, 1e-9) && near1(p.beforeF, 0.4, 1e-6), "the factor before and after, as the line reports them");
    // The comparators on the real plan, and on the original behaviour.
    check(limitHolds(r, factorOf(r, Variant::kReal)), "comparator: the real factor passes the limit check");
    check(!limitHolds(r, factorOf(r, Variant::kWholeChangeRemoved)),
          "comparator: the pre-change factor fails it (the crash)");
}

void testSupersamplingOne() {
    // At Supersampling <= 1 the term is exactly 1 and the budget is not binding for any 4K window: today's factor, bit
    // for bit, on the flights' states and on a grid.
    struct State { uint32_t ask; float hmd; uint32_t out; float tangent; float target; };
    const State flights[] = {
        {3070, 0.65f, 3070, 1.2648f, 1.0f},  {3070, 0.65f, 3070, 1.2648f, 1.25f}, {2458, 0.65f, 3070, 1.1779f, 1.25f},
        {3072, 0.65f, 3072, 0.9657f, 1.25f}, {4032, 0.50f, 4032, 1.0293f, 1.25f},  {4032, 0.50f, 4032, 1.0293f, 1.0f},
        {3296, 0.65f, 3296, 1.0293f, 1.25f}, {4032, 1.25f, 4032, 1.0293f, 1.25f}, {4032, 1.50f, 4032, 1.0293f, 1.25f},
        {4508, 0.50f, 4508, 1.2648f, 1.25f}};
    // (Below HMD Quality 0.5 -- Elite floors it there -- f at 125 is under 0.36 and a 5040 wide 2D screen or a 5K window
    // can reach the budget: testTargets and testBudgetBinds hold those, as the places the budget is allowed to act.)
    unsigned n = 0, bad = 0;
    for (const State& s : flights) {
        for (float ss : {0.5f, 0.85f, 1.0f}) {
            for (uint32_t display : {0u, 1920u, 2560u, 3840u}) {
                for (uint32_t screen : {0u, 3712u, 5040u}) {
                    Rig r;
                    r.askW = s.ask; r.hmd = s.hmd; r.outW = s.out; r.tangent = s.tangent; r.target = s.target;
                    r.ss = ss; r.displayW = display; r.screenW = screen;
                    UiPanelPlan p;
                    const UiPanelInputs in = inputsOf(r);
                    ++n;
                    if (!uiPanelPlanFor(in, &p) || !sameBits(p.f, legacyFactor(in)) || p.ssActs || p.budgetActs) ++bad;
                }
            }
        }
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg), "Supersampling <= 1: the factor is the pre-change expression bit for bit, %u states (%u differ)", n, bad);
    check(bad == 0, msg);
    std::printf("  panel budget: %u states at Supersampling <= 1 compared bit for bit with the pre-change factor, %u differ\n", n, bad);
    // The same through the public uiPanelFactor, with its clamp label, on the first flight.
    Rig r;
    r.ss = 1.0f;
    double f = 0.0;
    UiPanelClamp clamp = UiPanelClamp::kBudget;
    check(uiPanelFactor(inputsOf(r), &f, &clamp) && sameBits(f, legacyFactor(inputsOf(r))) && clamp == UiPanelClamp::kNone,
          "uiPanelFactor at Supersampling 1.0, the crash flight otherwise: today's 0.4, no clamp");
    // The Supersampling below 1 is the game's own scene being smaller; the panels keep W_ui (the max of the record's two
    // sizes), so the term is exactly 1 and the log never says it acted.
    r.ss = 0.5f;
    UiPanelPlan q;
    check(uiPanelPlanFor(inputsOf(r), &q) && q.ss == 1.0 && !q.ssActs && sameBits(q.f, legacyFactor(inputsOf(r))),
          "Supersampling 0.5: no term (the record's larger size is W_ui)");
}

void testTargets() {
    // ui_quality 100: the target 1.0.
    Rig r;
    r.target = 1.0f;
    r.ss = 1.0f;
    UiPanelPlan p;
    check(uiPanelPlanFor(inputsOf(r), &p) && factorIs(p.f, 0.5) && !p.ssActs, "ui_quality 100 at Supersampling 1: f 0.5, as before");
    check(near1(p.largest, 7680.0, 1e-6), "...the widest panel it could ask for: 7680 px");
    r.ss = 2.0f;
    check(uiPanelPlanFor(inputsOf(r), &p) && factorIs(p.f, 1.0) && p.ssActs && !p.budgetActs && p.clamp == UiPanelClamp::kNone,
          "ui_quality 100 at Supersampling 2.0: f 1.0 -- the game's own panels are already at the target; the patch adds nothing");
    check(limitHolds(r, p.f) && near1(p.largest, 7680.0, 1e-6), "...and the request is the game's own 7680 px at most");
    check(near1(orbital_width::target([&] {
                    orbital_width::Inputs o;
                    o.vr = o.ready = o.panelLive = true;
                    o.panelFactor = p.lineF;
                    return o;
                }()), 1.0, 1e-12),
          "...so the orbit lines' width patch stays off (f = 1), as the panels do");
    r.ss = 2.0f;
    r.target = 1.25f;
    // HMD Quality 0.4 (below Elite's floor of 0.5, but the arithmetic must hold there too).
    r.hmd = 0.4f;
    r.ss = 1.0f;
    check(uiPanelPlanFor(inputsOf(r), &p) && near1(p.f, 0.32, 2e-3) && !p.budgetActs && p.largest <= kUiPanelBudget,
          "HMD Quality 0.4 at 125, Supersampling 1: f 0.32 (x3.1), the widest panel 12000 px, inside the budget");
    r.ss = 2.0f;
    check(uiPanelPlanFor(inputsOf(r), &p) && near1(p.f, 0.64, 4e-3) && p.ssActs && p.largest <= kUiPanelBudget && limitHolds(r, p.f),
          "HMD Quality 0.4 at 125, Supersampling 2.0: f 0.64, the same 12000 px, not 24000");
    r.target = 1.0f;
    check(uiPanelPlanFor(inputsOf(r), &p) && near1(p.f, 0.8, 4e-3) && limitHolds(r, p.f),
          "HMD Quality 0.4 at 100, Supersampling 2.0: f 0.8");
    // HMD Quality 0.3 at 125, Supersampling 1: f would be 0.24, the 4x cap's 0.25 -- and 3840 / 0.25 is over the budget.
    r.hmd = 0.3f;
    r.target = 1.25f;
    r.ss = 1.0f;
    check(uiPanelPlanFor(inputsOf(r), &p) && p.budgetActs && p.clamp == UiPanelClamp::kBudget && p.largest <= kUiPanelBudget + 1e-6 &&
              p.f > 0.25 && limitHolds(r, p.f),
          "HMD Quality 0.3 at 125: the 4x cap's 0.25 would ask for 15360 px; the budget raises f until 14336 holds");
}

void testBudgetBinds() {
    // An 8K window: at Supersampling 1 the record's base is 7680, and f 0.4 would ask for 19200 px. The Supersampling term
    // cannot be what holds here -- the budget is.
    Rig r;
    r.ss = 1.0f;
    r.displayW = 7680;
    UiPanelPlan p;
    check(uiPanelPlanFor(inputsOf(r), &p) && p.budgetActs && p.clamp == UiPanelClamp::kBudget && !p.ssActs &&
              p.baseFrom == UiPanelBase::kDisplay && near1(p.f, 7680.0 / kUiPanelBudget, 1e-9) &&
              near1(p.largest, kUiPanelBudget, 1e-6),
          "an 8K window at Supersampling 1: the budget raises f from 0.4 to 0.5357; the widest panel is 14336 px");
    check(limitHolds(r, p.f) && !limitHolds(r, legacyFactor(inputsOf(r))), "...within 16384, where the old f is not");
    check(near1(p.lineF, 0.4, 1e-6) && p.f > p.lineF, "...the orbit lines' factor stays the layer/render ratio, 0.4, not the thinned panels'");
    r.ss = 2.0f;
    check(uiPanelPlanFor(inputsOf(r), &p) && p.budgetActs && p.ssActs && factorIs(p.f, 1.0) && near1(p.lineF, 0.8, 1e-9) &&
              near1(p.largest, 15360.0, 1e-6) && p.largest <= kLimit,
          "an 8K window at Supersampling 2.0: f 1.0 (the game's own, 15360 px, still within 16384); the lines keep 0.8");
    // The 2D screen's forced width (fix.vscreen_res_width) is a base too.
    r.ss = 1.0f;
    r.displayW = 3840;
    r.screenW = 8192;
    check(uiPanelPlanFor(inputsOf(r), &p) && p.baseFrom == UiPanelBase::kScreen && p.budgetActs && limitHolds(r, p.f),
          "a 2D screen forced to 8192 at Supersampling 1: the base is that width, and the budget holds");
    r.screenW = 5040;  // the legacy cap of the auto rule
    check(uiPanelPlanFor(inputsOf(r), &p) && !p.budgetActs && near1(p.f, 0.4, 1e-6),
          "a 2D screen of 5040 (the auto rule's cap) at Supersampling 1 leaves f alone: 12600 px is inside the budget");
    // The scene's own width: an enormous headset.
    r = Rig{};
    r.ss = 1.0f;
    r.askW = 12000;
    r.outW = 12000;
    r.hmd = 1.0f;
    check(uiPanelPlanFor(inputsOf(r), &p) && p.baseFrom == UiPanelBase::kScene && limitHolds(r, p.f),
          "a 12000 wide scene: its own width x k is the base");
}

void testGrid() {
    // The property, on a grid far wider than any flight: for every headset, quality, target, Supersampling, window and
    // 2D screen, wherever the patch is acting (f < 1) the widest panel the formula could ask for is inside the budget, and
    // the request the game's own arithmetic makes from every state of the record is inside D3D11's limit on both axes.
    unsigned n = 0, overBudget = 0, overLimit = 0, outOfRange = 0, unknownWrong = 0;
    for (uint32_t out : {2500u, 3070u, 4032u, 5000u, 7000u}) {
        for (float hmd : {0.2f, 0.3f, 0.4f, 0.5f, 0.65f, 0.8f, 1.0f, 1.25f, 1.5f}) {
            for (float target : {1.0f, 1.25f}) {
                for (float ss : {0.5f, 0.85f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f}) {
                    for (uint32_t display : {0u, 1280u, 1920u, 2560u, 3840u, 5120u, 7680u}) {
                        for (uint32_t screen : {0u, 3712u, 5040u, 8192u}) {
                            Rig r;
                            r.askW = out; r.outW = out; r.hmd = hmd; r.target = target; r.ss = ss;
                            r.displayW = display; r.screenW = screen;
                            const UiPanelInputs in = inputsOf(r);
                            UiPanelPlan p;
                            if (!in.renderW) continue;
                            ++n;
                            if (!uiPanelPlanFor(in, &p)) { ++unknownWrong; continue; }
                            if (p.f < 0.25 || p.f > 1.0) ++outOfRange;
                            if (p.f < 1.0 && p.largest > kUiPanelBudget + 1e-6) ++overBudget;
                            if (!limitHolds(r, p.f)) ++overLimit;
                        }
                    }
                }
            }
        }
    }
    char msg[260];
    std::snprintf(msg, sizeof(msg), "grid of %u states: f is always in [1/4, 1] (%u outside)", n, outOfRange);
    check(outOfRange == 0 && unknownWrong == 0, msg);
    std::snprintf(msg, sizeof(msg), "grid of %u states: wherever f < 1 the widest panel the formula could ask for is inside the budget (%u over)", n, overBudget);
    check(overBudget == 0, msg);
    std::snprintf(msg, sizeof(msg), "grid of %u states: no state of the record asks for a panel over 16384 on either axis, by the game's own arithmetic (%u over)", n, overLimit);
    check(overLimit == 0, msg);
    std::printf("  panel budget: grid of %u states (5 headsets x 9 qualities x 2 targets x 8 Supersamplings x 7 windows x 4 2D screens): "
                "%u outside [1/4, 1], %u over the budget while f < 1, %u over 16384\n", n, outOfRange, overBudget, overLimit);
}

void testMutants() {
    // Each wrong thing, through the comparators the real plan passes. A comparator that cannot fail proves nothing.
    Rig crash;  // the flight that crashed
    Rig big;    // an 8K window, Supersampling 2.0: the budget, not the term, is what holds
    big.displayW = 7680;
    Rig big1 = big;  // ...and at Supersampling 1
    big1.ss = 1.0f;

    // The real plan passes everything.
    for (const Rig* r : {&crash, &big, &big1})
        check(limitHolds(*r, factorOf(*r, Variant::kReal)), "comparator: the real plan keeps every request within 16384");
    check(factorIs(factorOf(crash, Variant::kReal), 0.8), "comparator: the real plan's crash factor is 0.8");
    check(factorIs(factorOf(big, Variant::kReal), 1.0), "comparator: the real plan's 8K Supersampling 2.0 factor is 1.0");

    // MUTANT, the Supersampling term removed (the budget left seeing S = 1): the crash flight's factor is the old 0.4, whose
    // request (19200 px) is over the limit, and its factor is not the expected 0.8.
    check(!limitHolds(crash, factorOf(crash, Variant::kNoSupersampling)) && !factorIs(factorOf(crash, Variant::kNoSupersampling), 0.8),
          "MUTANT, the Supersampling term removed: the crash flight's request is over 16384 and its factor is not 0.8");
    // MUTANT, the budget removed: the term alone is not enough on an 8K window.
    check(!limitHolds(big, factorOf(big, Variant::kNoBudget)) && !limitHolds(big1, factorOf(big1, Variant::kNoBudget)),
          "MUTANT, the budget removed: an 8K window at Supersampling 2.0 and 1 asks for 19200 px");
    check(limitHolds(crash, factorOf(crash, Variant::kNoBudget)) && factorIs(factorOf(crash, Variant::kNoBudget), 0.8),
          "...and the crash flight alone does not need it (the term holds there): each part is proved by a flight of its own");
    // MUTANT, the term applied twice / inverted / the whole change removed.
    check(!factorIs(factorOf(crash, Variant::kSupersamplingTwice), 0.8) &&
              factorIs(factorOf(crash, Variant::kSupersamplingTwice), 1.0),
          "MUTANT, the term twice: the crash flight's factor is 1.0, not 0.8 (the extra density is thrown away)");
    check(!factorIs(factorOf(crash, Variant::kSupersamplingInverted), 0.8) && !limitHolds(crash, factorOf(crash, Variant::kSupersamplingInverted)),
          "MUTANT, the term divided instead of multiplied: 0.25, and the request is 30720 px");
    check(!limitHolds(crash, factorOf(crash, Variant::kWholeChangeRemoved)) && !limitHolds(big1, factorOf(big1, Variant::kWholeChangeRemoved)),
          "MUTANT, the whole change removed: the crash and the 8K window both ask for more than 16384");
    // MUTANT, the budget's constant: 16384 itself as the budget leaves no margin, and a stage 1% wider than the 1920 one
    // would be refused -- the margin is a number the rig pins.
    check(kUiPanelBudget < kUiPanelTextureLimit && kUiPanelBudget >= 0.85 * kUiPanelTextureLimit,
          "the budget keeps a margin under the limit (7/8) and is not a hair under it");
}

// ---- the orbit lines follow ---------------------------------------------------------------------------------------------------

double orbitFactor(const UiPanelPlan& p, bool live = true) {
    orbital_width::Inputs o;
    o.vr = true;
    o.ready = true;
    o.refused = false;
    o.panelLive = live;
    o.panelFactor = p.lineF;  // what orbital_width.cpp reads: uiPanelScaleLineFactor()
    return orbital_width::target(o);
}

void testOrbitLines() {
    // The width patch's f is the layer/render ratio: the layer is the door's size x the target wide, the scene is W_ui x S.
    Rig r;
    UiPanelPlan p;
    uiPanelPlanFor(inputsOf(r), &p);
    const double layerW = 4032.0 * 1.25, sceneW = 2016.0 * 2.0;
    check(near1(orbitFactor(p) * layerW, sceneW, 1e-6),
          "the orbit lines' factor x the layer's width is the scene's width: 0.8 x 5040 = 4032 (Supersampling 2.0, the scene drawn twice as wide)");
    check(near1(orbitFactor(p), p.f, 1e-12), "...and with the budget not acting it is the panels' factor");
    // Before the change the lines would have taken 0.4: lines half as wide in the layer as the scene's, against a layer 1.25
    // times a 4032 scene.
    check(!near1(legacyFactor(inputsOf(r)) * layerW, sceneW, 1.0), "(the pre-change 0.4 does not satisfy it: 2016, not 4032)");
    // The budget acts: the panels thin, the layer does not, and the lines keep the ratio.
    Rig big;
    big.displayW = 7680;
    big.ss = 1.0f;
    uiPanelPlanFor(inputsOf(big), &p);
    check(p.f > p.lineF && near1(orbitFactor(p), 0.4, 1e-6) && near1(orbitFactor(p) * layerW, 2016.0, 1e-6),
          "the budget acting (8K window, Supersampling 1): the panels take 0.5357, the orbit lines keep 0.4 = 2016 / 5040");
    // Off, or not live, or at f = 1: nothing (orbital_width.h's own decision).
    check(near1(orbitFactor(p, false), 1.0, 1e-12), "the panel patch not live: the lines are the game's own width");
    Rig hundred;
    hundred.target = 1.0f;
    uiPanelPlanFor(inputsOf(hundred), &p);
    check(near1(orbitFactor(p), 1.0, 1e-12), "ui_quality 100 at Supersampling 2.0 (f = 1): the lines are the game's own width");
    // The consumers' accessors: the line factor is always the panels' or less-thinned.
    unsigned bad = 0, n = 0;
    for (float ss : {1.0f, 1.5f, 2.0f, 3.0f})
        for (uint32_t display : {3840u, 7680u}) {
            Rig g;
            g.ss = ss;
            g.displayW = display;
            uiPanelPlanFor(inputsOf(g), &p);
            ++n;
            if (!(p.lineF <= p.f + 1e-12) || (!p.budgetActs && p.lineF != p.f)) ++bad;
        }
    char msg[160];
    std::snprintf(msg, sizeof(msg), "the line factor is the panels' factor unless the budget acted, and then the smaller (%u states, %u wrong)", n, bad);
    check(bad == 0, msg);
}

void testDisplayXml() {
    // DisplaySettings.xml, as the game writes it (the Frontier install, 2026-10-07).
    const char xml[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<DisplayConfig>\n\t<ScreenWidth>3840</ScreenWidth>\n"
        "\t<ScreenHeight>2160</ScreenHeight>\n\t<VSync>false</VSync>\n\t<FullScreen>2</FullScreen>\n</DisplayConfig>\n";
    uint32_t w = 0, h = 0;
    check(uiDisplaySizeFromXml(xml, sizeof(xml) - 1, &w, &h) && w == 3840 && h == 2160, "DisplaySettings.xml: 3840x2160");
    const char nowidth[] = "<DisplayConfig><ScreenHeight>2160</ScreenHeight></DisplayConfig>";
    const char zero[] = "<ScreenWidth>0</ScreenWidth><ScreenHeight>2160</ScreenHeight>";
    const char huge[] = "<ScreenWidth>99999</ScreenWidth><ScreenHeight>2160</ScreenHeight>";
    const char text[] = "<ScreenWidth>wide</ScreenWidth><ScreenHeight>2160</ScreenHeight>";
    w = h = 7;
    check(!uiDisplaySizeFromXml(nowidth, sizeof(nowidth) - 1, &w, &h) && !uiDisplaySizeFromXml(zero, sizeof(zero) - 1, &w, &h) &&
              !uiDisplaySizeFromXml(huge, sizeof(huge) - 1, &w, &h) && !uiDisplaySizeFromXml(text, sizeof(text) - 1, &w, &h) &&
              !uiDisplaySizeFromXml(nullptr, 0, &w, &h) && w == 7 && h == 7,
          "DisplaySettings.xml: no width, zero, 99999, a word and no text are unknown, and leave the outputs alone");
    // The factor is not made without the Supersampling, and does not need the display or the screen.
    Rig r;
    UiPanelInputs in = inputsOf(r);
    UiPanelPlan p;
    in.supersampling = 0.0f;
    check(!uiPanelPlanFor(in, &p), "no Supersampling read: no factor (the floats stay as they are, the game's own sizes at first)");
    in = inputsOf(r);
    in.displayW = 0;
    in.screenW = 0;
    check(uiPanelPlanFor(in, &p) && factorIs(p.f, 0.8) && p.baseFrom == UiPanelBase::kObserved,
          "no display and no 2D screen known: the observed base stands, and the crash flight is still 0.8");
}

// ---- the wiring, by source scan ---------------------------------------------------------------------------------------------

struct Pin { const char* id; bool ok; const char* what; };

std::vector<Pin> pins(const std::string& scale, const std::string& surfaces, const std::string& orbit, const std::string& hook) {
    std::vector<Pin> out;
    using afterui::functionBody;
    using afterui::squeeze;
    auto body = [](const std::string& text, const char* signature) {
        std::string b;
        return functionBody(text, signature, &b) ? squeeze(b) : std::string();
    };
    const std::string gather = body(scale, "bool gatherInputs(");
    const std::string frame = body(scale, "void uiPanelScaleFrameBoundary() {");
    const std::string reread = body(surfaces, "void hmdReadNow() {");
    const std::string orbFrame = body(orbit, "void orbitalWidthFrameBoundary(");
    const std::string panelSettings = body(hook, "bool deviceHookPanelSettings(");
    using worldroute::has;
    using worldroute::inOrder;
    out.push_back({"gather-ss", has(gather, "in->supersampling=uiSurfacesSupersampling();") && has(gather, "in->supersampling>0.0f"),
                   "gatherInputs reads the Supersampling and returns false (no factor) without it"});
    out.push_back({"gather-base", has(gather, "in->displayW=uiSurfacesDisplayWidth();") && has(gather, "in->screenW=vscreenModeAppliedWidth();"),
                   "gatherInputs hands the plan the display's width and the 2D screen's forced width"});
    out.push_back({"plan-written", inOrder(frame, {"uiPanelPlanFor(in,&plan)", "constdoublef=plan.f;", "writeFloats(f,plan.lineF,plan.ss)"}) &&
                                       !has(frame, "uiPanelFactor("),
                   "the frame boundary writes the plan's own f, with its line factor and Supersampling, and makes no second factor"});
    out.push_back({"adjusted-line", has(frame, "if(plan.ssActs||plan.budgetActs)") && has(scale, "factor adjusted -- Elite's Supersampling is %.2f"),
                   "the adjusted line is printed when the Supersampling term or the budget acted"});
    out.push_back({"reread", has(reread, "deviceHookPanelSettings(&q,&ss,&dw,&dh)") && has(reread, "g_ssaaBits.store(ssBits") && has(reread, "g_displayW.store(dw"),
                   "the cached read takes HMD Quality, the Supersampling and the display from one pass"});
    out.push_back({"orbit-line-factor", has(orbFrame, "in.panelFactor=in.panelLive?uiPanelScaleLineFactor():1.0;") && !has(orbFrame, "uiPanelScaleFactor()"),
                   "the orbit lines read the line factor (Supersampling in, budget out), not the panels' written one"});
    out.push_back({"hook-reads", has(panelSettings, "eliteHmdMultiplier(&q,&ss,nullptr,0)") && has(panelSettings, "uiDisplaySizeFromXml("),
                   "deviceHookPanelSettings reads the .fxcfg pair and DisplaySettings.xml"});
    return out;
}

void testWiring() {
    const std::string scale = worldroute::readText("src/d3d11/ui_panel_scale.cpp");
    const std::string surfaces = worldroute::readText("src/d3d11/ui_surfaces.cpp");
    const std::string orbit = worldroute::readText("src/d3d11/orbital_width.cpp");
    const std::string hook = worldroute::readText("src/d3d11/device_hook.cpp");
    check(!scale.empty() && !surfaces.empty() && !orbit.empty() && !hook.empty(),
          "the panel patch's sources are readable from the working directory (the rig runs from the repo root)");
    if (scale.empty() || surfaces.empty() || orbit.empty() || hook.empty()) return;
    for (const Pin& p : pins(scale, surfaces, orbit, hook)) {
        char msg[320];
        std::snprintf(msg, sizeof(msg), "wiring [%s]: %s", p.id, p.what);
        check(p.ok, msg);
    }
    // CONTROLS: one edit each that puts a likely slip back; the pin named must fail.
    auto replaced = [](std::string s, const char* from, const char* to) {
        const size_t at = s.find(from);
        if (at != std::string::npos) s.replace(at, std::strlen(from), to);
        return s;
    };
    auto pinOk = [&](const std::vector<Pin>& v, const char* id) {
        for (const Pin& p : v)
            if (std::strcmp(p.id, id) == 0) return p.ok;
        return true;
    };
    struct Control { const char* pin; int file; const char* from; const char* to; };
    const Control controls[] = {
        {"gather-ss", 0, "in->supersampling = uiSurfacesSupersampling();", "in->supersampling = 1.0f;"},
        {"plan-written", 0, "writeFloats(f, plan.lineF, plan.ss)", "writeFloats(f)"},
        {"plan-written", 0, "!uiPanelPlanFor(in, &plan)", "!uiPanelFactor(in, nullptr)"},
        {"adjusted-line", 0, "if (plan.ssActs || plan.budgetActs)", "if (false)"},
        {"reread", 1, "g_ssaaBits.store(ssBits, std::memory_order_release);", ""},
        {"orbit-line-factor", 2, "uiPanelScaleLineFactor()", "uiPanelScaleFactor()"},
        {"hook-reads", 3, "uiDisplaySizeFromXml(", "uiDisplaySizeFromXmlX("},
    };
    for (const Control& c : controls) {
        std::string s = scale, su = surfaces, o = orbit, h = hook;
        const std::string* target = c.file == 0 ? &scale : c.file == 1 ? &surfaces : c.file == 2 ? &orbit : &hook;
        if (target->find(c.from) == std::string::npos) {
            char msg[260];
            std::snprintf(msg, sizeof(msg), "control for [%s]: the text it edits ('%s') is in the source", c.pin, c.from);
            check(false, msg);
            continue;
        }
        (c.file == 0 ? s : c.file == 1 ? su : c.file == 2 ? o : h) = replaced(*target, c.from, c.to);
        char msg[260];
        std::snprintf(msg, sizeof(msg), "control: one edit ('%s' -> '%s') trips wiring [%s]", c.from, c.to, c.pin);
        check(!pinOk(pins(s, su, o, h), c.pin), msg);
    }
}

void testAll() {
    testCrashNumbers();
    testSupersamplingOne();
    testTargets();
    testBudgetBinds();
    testGrid();
    testMutants();
    testOrbitLines();
    testDisplayXml();
}

}  // namespace panelbudget
