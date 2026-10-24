// fix.sharp_weapons, in the build's gate (docs/dlss-blur-elements-2026-10-14.md, "Built, 2026-10-16: weapons fire in the HDR layer").
//
//   THE RULE (pure). The weapons-fire family is the exact pair (never the vertex shader alone): the three pairs name kWeaponsFire on the HDR branch of the
//   production uiLayerFamilyFor and nothing else does (one-token mutants of each pair, a post-tonemap target, the refracting partner 820E/6EAA). It is
//   taken into the HDR layer and exempt from the density rule. The jitter-cancel table defaults to cancel and is read per pair. The A/B key's words.
//   THE TWO KEYS (pure). fix.ui_quality and fix.sharp_weapons are independent: for each of the four combinations the family gate, the decisions of every
//   family and the liveness of the layer's two halves. The layer's machinery runs when either half is live; the interface-only parts (the world route,
//   the on-foot gate, the after-UI take) when ui_quality's is.
//   THE PLAN (pure). The tonemap re-issues of an eye-frame (weapons_fire.h sharpTonePlan) for every (HUD, weapons, mode, matched-ready) combination
//   against a frozen reference table, and the invariants a plan must hold, with mutants of the plan caught by them.
//   THE CENSUS (pure). Three recorded census frames of the 2026-10-16 flight (log edvr_gfx_20261016_070029: censuses 1, 3 and 4, frame 0; 346, 626 and 719
//   draw rows) through the production rule and decision as they were before this arc (a frozen copy) and as they are: exactly the weapon rows change
//   decision, from "not UI" to redirected, and the 820E/6EAA partner and every other row are decided as before; one-token mutants of a weapon row are
//   refused for the right reason; and the four key combinations over the whole frames.
//   THE WIRING (source scans, each with a control that must trip it): the layer's liveness, the independent key reads, the family gate in the decision, the
//   interface-only parts, the jitter table, the PS hash read for the new vertex shaders, the post-tonemap rules skipped when ui_quality is off, the loop that
//   issues the weapons steps, the eye-dump watch of the three vertex shaders.
//   THE COMPOSITE (WARP). A synthetic game tonemap (a scalar exposure, a 3D LUT and a compressing curve, so T(F + L) != T(F) + T(L)) driven through the
//   production source pass, blend states and step bindings of sharp_weapons.h: the matched composite equals T(F + L) over a lit background to within the 8-bit
//   rounding, where the plain add is far off, and over black the two agree; mutants of the sequence (the source pass without its add, the lit step replacing,
//   the base step missing) are each caught; the HUD's add and the HUD-alone replace; the immediate context's state is back after the source pass; and the
//   debug layer reports nothing.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include "../../src/d3d11/flat_ui_layer_math.h"
#include "../../src/d3d11/menu_flat_rows.h"
#include "../../src/d3d11/menu_schema.h"
#include "../../src/d3d11/sharp_weapons.h"
#include "../../src/d3d11/sharp_weapons_shaders.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "temporal_shader_bytecode.h"

namespace edvr {
#include "menu_schema.inc"   // the rows the build generated from edvr.ini (build\gen)
}

using namespace edvr;
using Microsoft::WRL::ComPtr;

static unsigned g_checks = 0;
static void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
}
static void hr(HRESULT r, const char* what) { check(SUCCEEDED(r), what); }

static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ============================================================================================ the rule
static UiFamilyFacts hdrFacts(uint64_t vs, uint64_t ps) {
    UiFamilyFacts f;
    f.targetKind = 1;
    f.vs = vs;
    f.ps = ps;
    return f;
}

static void ruleTests() {
    check(kWeaponPairCount == 3, "three weapons-fire pairs");
    for (size_t i = 0; i < kWeaponPairCount; ++i) {
        const WeaponPair& p = kWeaponPairs[i];
        check(uiLayerFamilyFor(hdrFacts(p.vs, p.ps)) == UiLayerFamily::kWeaponsFire, "each weapons-fire pair is the family on the HDR target");
        check(weaponPairIndex(p.vs, p.ps) == static_cast<int>(i) && weaponPairIndexByVs(p.vs) == static_cast<int>(i), "the pair table finds its own rows");
        check(weaponsFireVs(p.vs), "the draw hook reads the pixel shader for each of the vertex shaders");
        // The pair is the key: the vertex shader with another pixel shader, or a pixel shader alone, is nothing.
        for (int bit = 0; bit < 64; bit += 9) {
            check(uiLayerFamilyFor(hdrFacts(p.vs, p.ps ^ (1ull << bit))) != UiLayerFamily::kWeaponsFire, "a one-bit change of the pixel shader is not the family");
            check(uiLayerFamilyFor(hdrFacts(p.vs ^ (1ull << bit), p.ps)) != UiLayerFamily::kWeaponsFire, "a one-bit change of the vertex shader is not the family");
        }
        check(uiLayerFamilyFor(hdrFacts(p.vs, 0)) != UiLayerFamily::kWeaponsFire, "the vertex shader with no pixel shader read is not the family");
        UiFamilyFacts ldr = hdrFacts(p.vs, p.ps);
        ldr.targetKind = 2;
        check(uiLayerFamilyFor(ldr) != UiLayerFamily::kWeaponsFire, "the post-tonemap target never names the weapons fire");
        ldr.targetKind = 0;
        check(uiLayerFamilyFor(ldr) == UiLayerFamily::kNone, "no eye target, no family");
    }
    // The shield's refracting partner (820E/6EAA) is NOT a weapons-fire row and never goes into the weapons layer (it samples the pre-DLSS eye HDR copy at the
    // screen UV); since the shield ripple arc it is the family kShieldRipple, taken into its own displacement layer under fix.sharp_weapons (shield_ripple_test).
    check(uiLayerFamilyFor(hdrFacts(0x820E5C131B99361Dull, 0x6EAA86EFE135B2D4ull)) == UiLayerFamily::kShieldRipple,
          "the shield's refracting partner (820E/6EAA) is the shield-ripple family, not weapons fire");
    check(weaponPairIndex(0x820E5C131B99361Dull, 0x6EAA86EFE135B2D4ull) < 0, "...and not in the weapons-fire pair table");
    check(uiLayerFamilyTakesHdr(UiLayerFamily::kWeaponsFire), "the weapons fire is taken into the HDR layer");
    check(!uiLayerFamilyIsSceneLines(UiLayerFamily::kWeaponsFire), "...and is exempt from the density rule");
    check(std::strcmp(uiLayerFamilyName(UiLayerFamily::kWeaponsFire), "weapons fire") == 0, "the family's name is the log's");
    // A layer not wider than the render does not decline the weapons fire (it declines the lines).
    UiLayerDrawFacts d;
    d.family = UiLayerFamily::kWeaponsFire;
    d.eyeTarget = true;
    d.ldrView = false;
    d.crispHdr = true;
    d.eye = 0;
    d.armed = true;
    d.layerWider = false;
    d.blend = UiBlendShape::kScaledAdditive;
    check(uiLayerDecide(d) == UiLayerDecision::kRedirect, "a layer no wider than the render still takes the weapons fire");
    UiLayerDrawFacts lines = d;
    lines.family = UiLayerFamily::kOrbitLines;
    check(uiLayerDecide(lines) == UiLayerDecision::kNoDensityGain, "control: the same facts decline a scene line");
    // Last in the enums: no older number moved.
    check(static_cast<int>(UiLayerFamily::kWeaponsFire) == static_cast<int>(UiLayerFamily::kMapSprite) + 1 &&
              static_cast<int>(UiLayerFamily::kShieldRipple) == static_cast<int>(UiLayerFamily::kWeaponsFire) + 1 &&
              static_cast<int>(UiLayerFamily::kShieldRipple) + 1 == static_cast<int>(UiLayerFamily::kCount),
          "the family is last but one in its enum, the shield ripple last (no older number moved)");
    check(static_cast<int>(UiLayerDecision::kFamilyOff) == static_cast<int>(UiLayerDecision::kLayerFailed) + 1 &&
              static_cast<int>(UiLayerDecision::kFamilyOff) + 1 == static_cast<int>(UiLayerDecision::kCount) && static_cast<int>(UiLayerDecision::kCount) <= 24,
          "the new decision is last in its enum and the 24-wide tables hold it");

    // The jitter-cancel table: default cancel, read per pair, a vertex shader it does not name cancels.
    for (size_t i = 0; i < kWeaponPairCount; ++i) check(weaponsFireJitterCancel(kWeaponPairs[i].vs), "every pair cancels the jitter by default (as the space dust does)");
    check(weaponsFireJitterCancel(0x1234ull), "a vertex shader the table does not name cancels");
    {
        // A table with an unjittered pair: the lookup answers per pair (the production lookup over a copy of the table).
        WeaponPair copy[kWeaponPairCount];
        for (size_t i = 0; i < kWeaponPairCount; ++i) copy[i] = kWeaponPairs[i];
        copy[1].cancelJitter = false;
        const auto lookup = [&](uint64_t vs) {
            for (size_t i = 0; i < kWeaponPairCount; ++i)
                if (copy[i].vs == vs) return copy[i].cancelJitter;
            return true;
        };
        check(lookup(kUiVsWeaponBeams) && !lookup(kUiVsMulticannon) && lookup(kUiVsShieldStrike) && lookup(0x1234ull),
              "an unjittered pair set false in the table takes no cancel and leaves the others alone");
    }

    // The key's words: on | plain | off, the old spellings of on and off, and the mapping onto the composite and the route.
    bool known = true;
    check(sharpWeaponsParse("on", &known) == SharpWeapons::kOn && known, "on");
    check(sharpWeaponsParse("plain", &known) == SharpWeapons::kPlain && known, "plain");
    check(sharpWeaponsParse("off", &known) == SharpWeapons::kOff && known, "off");
    check(sharpWeaponsParse("true", &known) == SharpWeapons::kOn && known && sharpWeaponsParse("Yes", &known) == SharpWeapons::kOn && known && sharpWeaponsParse("1", &known) == SharpWeapons::kOn && known,
          "the old spellings of on (true, yes, 1, any case) stay on, silently");
    check(sharpWeaponsParse("false", &known) == SharpWeapons::kOff && known && sharpWeaponsParse("NO", &known) == SharpWeapons::kOff && known && sharpWeaponsParse("0", &known) == SharpWeapons::kOff && known,
          "the old spellings of off (false, no, 0) stay off, silently");
    check(sharpWeaponsParse("matched", &known) == SharpWeapons::kOn && !known, "anything else (the old blend word included) reads as on, and says it was not recognised");
    check(sharpWeaponsParse("", &known) == SharpWeapons::kOn && !known, "an empty value reads as on");
    check(sharpWeaponsParse(nullptr, &known) == SharpWeapons::kOn && !known, "no value reads as on");
    check(sharpWeaponsTakes(SharpWeapons::kOn) && sharpWeaponsTakes(SharpWeapons::kPlain) && !sharpWeaponsTakes(SharpWeapons::kOff),
          "on and plain take the weapons fire (and the ripple); off leaves all of it in the scene");
    check(sharpWeaponsBlend(SharpWeapons::kOn) == SharpBlend::kMatched && sharpWeaponsBlend(SharpWeapons::kPlain) == SharpBlend::kPlain,
          "on is the matched composite, plain the plain add");
    check(std::strcmp(sharpBlendName(SharpBlend::kMatched), "matched") == 0 && std::strcmp(sharpBlendName(SharpBlend::kPlain), "plain") == 0,
          "the words the log prints are the composite's");
}

// ============================================================================================ the two keys
static void keyTests() {
    // The family gate over the four combinations, every family: the weapons fire follows fix.sharp_weapons alone, every other family fix.ui_quality alone.
    for (int q = 0; q < 2; ++q) {
        for (int s = 0; s < 2; ++s) {
            for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f) {
                const UiLayerFamily fam = static_cast<UiLayerFamily>(f);
                const bool on = uiLayerFamilyEnabled(fam, q != 0, s != 0);
                check(on == (fam == UiLayerFamily::kWeaponsFire ? s != 0 : fam == UiLayerFamily::kShieldRipple ? true : q != 0), "a family is enabled by its own key and by no other (the ripple, asked without its key, defaults on)");
                UiLayerDrawFacts d;
                d.family = fam;
                d.familyEnabled = on;
                d.eyeTarget = true;
                d.crispHdr = true;
                d.eye = 0;
                d.armed = true;
                d.blend = UiBlendShape::kScaledAdditive;
                const UiLayerDecision dec = uiLayerDecide(d);
                if (on) check(dec != UiLayerDecision::kFamilyOff, "an enabled family is never refused for its key");
                else check(dec == UiLayerDecision::kFamilyOff, "a family whose key is off is refused for it, first after the verdict");
            }
        }
    }
    // The decision order: no family and a verdict that swallows the draw come before the key.
    {
        UiLayerDrawFacts d;
        d.family = UiLayerFamily::kWeaponsFire;
        d.familyEnabled = false;
        d.verdictForwards = false;
        check(uiLayerDecide(d) == UiLayerDecision::kVerdict, "a swallowed draw is the verdict's, whatever the key says");
        d.family = UiLayerFamily::kNone;
        check(uiLayerDecide(d) == UiLayerDecision::kNotUi, "no family is not UI, whatever the key says");
    }
    // Liveness: the layer's machinery runs when either half is live; the interface-only parts follow fix.ui_quality.
    for (int q = 0; q < 2; ++q) {
        for (int s = 0; s < 2; ++s) {
            for (int t = 0; t < 2; ++t) {
                const float target = q ? 1.0f : 0.0f;
                const bool quality = uiLayerLiveFor(target, t != 0, true, false);
                const bool weapons = uiLayerWeaponsLiveFor(s != 0, t != 0, true, false);
                check(quality == (q && t), "fix.ui_quality's half is live for its key and a temporal mode, whatever fix.sharp_weapons says");
                check(weapons == (s && t), "fix.sharp_weapons' half is live for its key and a temporal mode, whatever fix.ui_quality says");
                check(uiLayerMachineryLive(quality, weapons) == ((q || s) && t), "the machinery runs when either half is live");
                check(!uiLayerWeaponsLiveFor(s != 0, t != 0, true, true), "a stood-down layer is not live for the weapons");
            }
        }
    }
    // The four combinations end to end on the pure rules: the weapons rows are redirected iff fix.sharp_weapons is on, a HUD row iff fix.ui_quality is.
    for (int q = 0; q < 2; ++q) {
        for (int s = 0; s < 2; ++s) {
            UiLayerDrawFacts w;
            w.family = UiLayerFamily::kWeaponsFire;
            w.familyEnabled = uiLayerFamilyEnabled(w.family, q != 0, s != 0);
            w.eyeTarget = true;
            w.crispHdr = true;
            w.eye = 0;
            w.armed = true;
            w.blend = UiBlendShape::kScaledAdditive;
            UiLayerDrawFacts h = w;
            h.family = UiLayerFamily::kFlightHud;
            h.familyEnabled = uiLayerFamilyEnabled(h.family, q != 0, s != 0);
            h.blend = UiBlendShape::kPremulOver;
            check((uiLayerDecide(w) == UiLayerDecision::kRedirect) == (s != 0), "the weapons fire is taken iff fix.sharp_weapons is on");
            check((uiLayerDecide(h) == UiLayerDecision::kRedirect) == (q != 0), "the flight HUD is taken iff fix.ui_quality is on");
        }
    }
}

// ============================================================================================ the plan
struct PlanRef {
    bool hud, weapons;
    SharpBlend mode;
    bool ready;
    uint8_t count;
    ToneStep steps[3];
    bool clearEye, sourcePass, coverage;
};
static const PlanRef kPlanRef[] = {
    // the HUD alone is the one replace it always was; nothing at all is nothing
    {false, false, SharpBlend::kMatched, true, 0, {}, false, false, false},
    {false, false, SharpBlend::kPlain, false, 0, {}, false, false, false},
    {true, false, SharpBlend::kMatched, true, 1, {ToneStep::kHudReplace}, false, false, true},
    {true, false, SharpBlend::kPlain, false, 1, {ToneStep::kHudReplace}, false, false, true},
    {true, false, SharpBlend::kMatched, false, 1, {ToneStep::kHudReplace}, false, false, true},
    // weapons alone
    {false, true, SharpBlend::kMatched, true, 2, {ToneStep::kWeaponsBase, ToneStep::kWeaponsLit}, true, true, false},
    {false, true, SharpBlend::kMatched, false, 1, {ToneStep::kWeaponsReplace}, true, false, false},
    {false, true, SharpBlend::kPlain, true, 1, {ToneStep::kWeaponsReplace}, true, false, false},
    {false, true, SharpBlend::kPlain, false, 1, {ToneStep::kWeaponsReplace}, true, false, false},
    // weapons first, the HUD added after
    {true, true, SharpBlend::kMatched, true, 3, {ToneStep::kWeaponsBase, ToneStep::kWeaponsLit, ToneStep::kHudAdd}, true, true, true},
    {true, true, SharpBlend::kMatched, false, 2, {ToneStep::kWeaponsReplace, ToneStep::kHudAdd}, true, false, true},
    {true, true, SharpBlend::kPlain, true, 2, {ToneStep::kWeaponsReplace, ToneStep::kHudAdd}, true, false, true},
    {true, true, SharpBlend::kPlain, false, 2, {ToneStep::kWeaponsReplace, ToneStep::kHudAdd}, true, false, true},
};

static bool planMatches(const TonePlan& p, const PlanRef& r) {
    if (p.count != r.count || p.clearEye != r.clearEye || p.sourcePass != r.sourcePass || p.coverage != r.coverage) return false;
    for (uint8_t i = 0; i < r.count; ++i)
        if (p.steps[i] != r.steps[i]) return false;
    return true;
}

// The invariants a plan holds, independent of the table above: a mutant of the plan must fail one of them.
static bool planInvariants(const TonePlan& p, bool hud, bool weapons, SharpBlend mode, bool ready) {
    if (p.count > 3) return false;
    if (!hud && !weapons) return p.count == 0 && !p.clearEye && !p.sourcePass && !p.coverage;
    if (p.count == 0) return false;
    bool sawBase = false, sawLit = false, sawHud = false, sawReplaceW = false;
    for (uint8_t i = 0; i < p.count; ++i) {
        const ToneStep s = p.steps[i];
        if (s == ToneStep::kWeaponsBase) {
            if (sawLit) return false;  // the base term precedes the lit term (the lit step subtracts what the base left in the target)
            sawBase = true;
        }
        if (s == ToneStep::kWeaponsLit) {
            if (!sawBase) return false;
            sawLit = true;
        }
        if (s == ToneStep::kWeaponsReplace) sawReplaceW = true;
        if (s == ToneStep::kHudReplace || s == ToneStep::kHudAdd) {
            sawHud = true;
            if (i == 0 && weapons) return false;  // the weapons come first
            if (weapons && s != ToneStep::kHudAdd) return false;  // after the weapons the HUD is ADDED: a replace would erase them
            if (!weapons && s != ToneStep::kHudReplace) return false;
        } else if (!weapons) {
            return false;
        }
    }
    if (sawHud != hud) return false;
    if (weapons) {
        const bool matched = mode == SharpBlend::kMatched && ready;
        if (matched != (sawBase && sawLit) || matched == sawReplaceW) return false;
        if (p.sourcePass != matched || !p.clearEye) return false;
        if (toneStepWrite(ToneStep::kWeaponsLit) != ToneWrite::kSubtract || toneStepWrite(ToneStep::kWeaponsBase) != ToneWrite::kReplace) return false;
    } else if (p.sourcePass || p.clearEye) {
        return false;
    }
    if (p.coverage != hud) return false;  // the coverage pass transfers the HUD's alpha, and only the HUD has one
    return true;
}

static void planTests() {
    for (const PlanRef& r : kPlanRef) {
        const TonePlan p = sharpTonePlan(r.hud, r.weapons, r.mode, r.ready);
        check(planMatches(p, r), "the plan equals the frozen table");
        check(planInvariants(p, r.hud, r.weapons, r.mode, r.ready), "the plan holds its invariants");
    }
    // Every (hud, weapons, mode, ready) combination holds the invariants (the table above is a subset).
    for (int h = 0; h < 2; ++h)
        for (int w = 0; w < 2; ++w)
            for (int m = 0; m < 2; ++m)
                for (int rdy = 0; rdy < 2; ++rdy)
                    check(planInvariants(sharpTonePlan(h != 0, w != 0, m ? SharpBlend::kPlain : SharpBlend::kMatched, rdy != 0), h != 0, w != 0,
                                         m ? SharpBlend::kPlain : SharpBlend::kMatched, rdy != 0),
                          "every combination holds the plan's invariants");
    // The plan's mutants, each caught by an invariant (the matched weapons-and-HUD plan, mutated one token at a time).
    const TonePlan base = sharpTonePlan(true, true, SharpBlend::kMatched, true);
    check(planInvariants(base, true, true, SharpBlend::kMatched, true), "control: the real plan passes");
    auto mut = [&](auto edit) {
        TonePlan p = base;
        edit(p);
        return planInvariants(p, true, true, SharpBlend::kMatched, true);
    };
    check(!mut([](TonePlan& p) { p.steps[2] = ToneStep::kHudReplace; }), "mutant: the HUD replaces the weapons' light after them");
    check(!mut([](TonePlan& p) { std::swap(p.steps[0], p.steps[1]); }), "mutant: the lit term before the base term");
    check(!mut([](TonePlan& p) { p.steps[1] = ToneStep::kWeaponsReplace; }), "mutant: the lit step replaces instead of subtracting");
    check(!mut([](TonePlan& p) { p.sourcePass = false; }), "mutant: no source pass for the matched composite");
    check(!mut([](TonePlan& p) { p.clearEye = false; }), "mutant: the 8-bit layer is not cleared under the weapons' light");
    check(!mut([](TonePlan& p) { p.coverage = false; }), "mutant: no coverage pass for the HUD's alpha");
    check(!mut([](TonePlan& p) { p.count = 2; }), "mutant: the HUD step dropped");
    check(!mut([](TonePlan& p) { p.steps[0] = ToneStep::kHudAdd; }), "mutant: the HUD first");
    // The step mapping.
    check(toneStepSource(ToneStep::kWeaponsLit) == ToneSource::kWeapons && toneStepSource(ToneStep::kWeaponsBase) == ToneSource::kScratch &&
              toneStepSource(ToneStep::kHudAdd) == ToneSource::kHud && toneStepSource(ToneStep::kHudReplace) == ToneSource::kHud &&
              toneStepSource(ToneStep::kWeaponsReplace) == ToneSource::kWeapons,
          "each step reads its own layer");
}

// ============================================================================================ the census
struct IdDesc {
    uint32_t w = 0, h = 0, fmt = 0, view = 0;
};
struct Row {
    uint32_t ordinal = 0, count = 0, instances = 0;
    char kind = 0;
    int rtv = -1, dsv = -1;
    uint64_t vs = 0, ps = 0;
    std::string ds, st, bm, bl, so;
};
static std::string tokenOf(const std::string& row, const std::string& key) {
    const std::string needle = " " + key + "=";
    const size_t at = row.find(needle);
    if (at == std::string::npos) return "";
    const size_t from = at + needle.size(), to = row.find(' ', from);
    return row.substr(from, to == std::string::npos ? std::string::npos : to - from);
}
static int idOf(const std::string& tok) { return tok.size() > 1 && tok[0] == '@' ? std::atoi(tok.c_str() + 1) : -1; }
struct Census {
    std::map<int, IdDesc> ids;
    std::vector<Row> rows;
};
static Census loadCensus(const char* path) {
    const std::string text = readFile(path);
    check(text.size() > 10000, "the census fixture is readable");
    Census c;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.compare(0, 4, "ID @") == 0) {
            int id = 0;
            unsigned w = 0, h = 0, fmt = 0, vf = 0;
            check(std::sscanf(line.c_str(), "ID @%d tex %ux%u fmt=%u res=%*s vf=%u", &id, &w, &h, &fmt, &vf) == 5, "an ID line is 'ID @n tex WxH fmt=F res=.. vf=V'");
            IdDesc d;
            d.w = w;
            d.h = h;
            d.fmt = fmt;
            d.view = vf;
            c.ids[id] = d;
        } else if (line.compare(0, 4, "ROW ") == 0) {
            const std::string row = " " + line.substr(4);
            Row r;
            unsigned frame = 0, ordinal = 0;
            char kind = 0;
            check(std::sscanf(line.c_str(), "ROW DC %u #%u %c ", &frame, &ordinal, &kind) == 3, "a ROW line is 'ROW DC <frame> #<ordinal> <kind> ...'");
            r.ordinal = ordinal;
            r.kind = kind;
            r.count = static_cast<uint32_t>(std::strtoul(tokenOf(row, "n").c_str(), nullptr, 10));
            r.instances = static_cast<uint32_t>(std::strtoul(tokenOf(row, "i").c_str(), nullptr, 10));
            r.rtv = idOf(tokenOf(row, "r"));
            r.dsv = idOf(tokenOf(row, "d"));
            r.vs = std::strtoull(tokenOf(row, "vh").c_str(), nullptr, 16);
            r.ps = std::strtoull(tokenOf(row, "ph").c_str(), nullptr, 16);
            r.ds = tokenOf(row, "ds");
            r.st = tokenOf(row, "st");
            r.bm = tokenOf(row, "bm");
            r.bl = tokenOf(row, "bl");
            r.so = tokenOf(row, "so");
            // Rows whose target is not a described texture (a swap chain, a null target) are not draws into an eye: skipped, like the census's own readers.
            if (r.rtv < 0 || !r.vs || r.ds.empty() || r.st.empty() || r.bm.empty() || r.bl.empty() || r.so.empty()) continue;
            c.rows.push_back(r);
        }
    }
    return c;
}
static UiDsState dsStateOf(const Row& r, const IdDesc* dsv) {
    UiDsState s;
    s.depthEnable = r.ds[0] == '1';
    s.depthFunc = static_cast<uint8_t>(r.ds[1] - '0');
    s.depthWriteAll = r.ds.back() == 'A';
    s.stencilEnable = r.st[0] == '1';
    unsigned rd = 0xFF, wr = 0xFF, f = 8, fail = 1, dfail = 1, pass = 1;
    char rest[64] = "";
    check(std::sscanf(r.so.c_str(), "r%x/w%x/f%u/%u,%u,%u%63s", &rd, &wr, &f, &fail, &dfail, &pass, rest) >= 6, "the stencil-op column is r../w../f./.,.,.");
    s.readMask = static_cast<uint8_t>(rd);
    s.writeMask = static_cast<uint8_t>(wr);
    s.front.func = static_cast<uint8_t>(f);
    s.front.fail = static_cast<uint8_t>(fail);
    s.front.depthFail = static_cast<uint8_t>(dfail);
    s.front.pass = static_cast<uint8_t>(pass);
    s.back = s.front;
    unsigned bf = 0, bfail = 0, bdfail = 0, bpass = 0;
    if (rest[0] == '+' && std::sscanf(rest, "+f%u/%u,%u,%u", &bf, &bfail, &bdfail, &bpass) == 4) {
        s.back.func = static_cast<uint8_t>(bf);
        s.back.fail = static_cast<uint8_t>(bfail);
        s.back.depthFail = static_cast<uint8_t>(bdfail);
        s.back.pass = static_cast<uint8_t>(bpass);
    }
    s.stencilPlane = !dsv || dsv->view == 20;
    return s;
}
static UiBlendRt blendOf(const Row& r) {
    UiBlendRt b;
    char flag = 0;
    unsigned src = 0, dst = 0, op = 0, srcA = 0, dstA = 0, opA = 0;
    check(std::sscanf(r.bl.c_str(), "%1c%u,%u,%u/%u,%u,%u", &flag, &src, &dst, &op, &srcA, &dstA, &opA) == 7, "the blend column is <flag><src>,<dst>,<op>/<srcA>,<dstA>,<opA>");
    b.enable = flag == '1';
    b.src = static_cast<uint8_t>(src);
    b.dst = static_cast<uint8_t>(dst);
    b.op = static_cast<uint8_t>(op);
    b.srcA = static_cast<uint8_t>(srcA);
    b.dstA = static_cast<uint8_t>(dstA);
    b.opA = static_cast<uint8_t>(opA);
    b.mask = static_cast<uint8_t>(std::strtoul(r.bm.c_str(), nullptr, 16));
    return b;
}

// The family rule as it stood before this arc (uiLayerFamilyFor at 139a6ff2, the tree this work started from), frozen: no weapons-fire pair.
static UiLayerFamily frozenFamilyFor(const UiFamilyFacts& f) {
    UiLayerFamily out = UiLayerFamily::kNone;
    if (f.targetKind == 0) {
    } else if (f.targetKind == 1) {
        out = uiVsIsHoloPanel(f.vs)    ? UiLayerFamily::kHolo
              : f.vs == kUiVsFlightHud ? UiLayerFamily::kFlightHud
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
              : (f.vs == kUiVsOrbitLines && f.ps == kUiPsOrbitLines)           ? UiLayerFamily::kOrbitLines
              : (f.vs == kUiVsSupercruiseBars && f.ps == kUiPsSupercruiseBars) ? UiLayerFamily::kSupercruiseBars
              : (f.vs == kUiVsSpaceDust && f.ps == kUiPsSpaceDust)             ? UiLayerFamily::kSpaceDust
              : (uiHoloGenericHash(f.vs) || (f.vs == kHoloTargetSphere && (f.ps == 0xEA02FAC2BD6C643Cull || f.ps == 0xE95634B0F61D218Full)))
                  ? UiLayerFamily::kHoloGeneric : UiLayerFamily::kNone;
    } else if (f.excluded) {
    } else if (f.panelSized) {
        out = UiLayerFamily::kScreen;
    } else if (f.learnedSurface) {
        out = f.vs == kUiVsPanel       ? UiLayerFamily::kPanel
              : f.vs == kUiVsLoader    ? UiLayerFamily::kLoader
              : uiVsIsHoloPanel(f.vs)  ? UiLayerFamily::kHolo
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
                                       : UiLayerFamily::kSurface;
    } else if (f.vs == kUiVsPanel && uiKnownPs(kUiPanelPs, sizeof(kUiPanelPs) / sizeof(kUiPanelPs[0]), f.ps)) {
        out = UiLayerFamily::kPanel;
    } else if (f.vs == kUiVsLoader && uiKnownPs(kUiLoaderPs, sizeof(kUiLoaderPs) / sizeof(kUiLoaderPs[0]), f.ps)) {
        out = UiLayerFamily::kLoader;
    } else if (f.vs == kUiVsGuiVector || f.vs == kUiVsGuiText || f.vs == kUiVsGuiIcons) {
        out = UiLayerFamily::kGuiDirect;
    } else if (f.vs == kUiVsFlightHud) {
        out = UiLayerFamily::kFlightHud;
    }
    return out;
}

struct Tweak {
    bool overridePs = false;
    uint64_t ps = 0;
    int blendOp = -1;
    int kind = -1;
    bool dsUnreproducible = false;
    bool lateTone = false;
    bool qualityOn = true, sharpOn = true;
};
struct Verdict {
    UiLayerFamily family = UiLayerFamily::kNone;
    UiLayerDecision decision = UiLayerDecision::kNotUi;
};
static int kindOf(const IdDesc& t) { return t.view == 28 || t.view == 87 ? 2 : 1; }

// What a row is to the layer: the family the rule names (the production rule, or the frozen one) and the decision, with the key gate the DLL applies.
static Verdict verdictOf(const Census& c, const Row& r, bool production, const Tweak& t = Tweak{}) {
    Verdict v;
    const IdDesc& target = c.ids.at(r.rtv);
    const IdDesc* dsv = r.dsv >= 0 ? &c.ids.at(r.dsv) : nullptr;
    const int kind = t.kind >= 0 ? t.kind : kindOf(target);
    UiFamilyFacts ff;
    ff.targetKind = kind;
    ff.vs = r.vs;
    ff.ps = t.overridePs ? t.ps : r.ps;
    v.family = production ? uiLayerFamilyFor(ff) : frozenFamilyFor(ff);
    UiBlendRt blend = blendOf(r);
    if (t.blendOp >= 0) blend.op = blend.opA = static_cast<uint8_t>(t.blendOp);
    UiLayerDrawFacts g;
    g.family = v.family;
    g.verdictForwards = true;
    g.eyeTarget = true;
    g.ldrView = kind == 2;
    g.crispHdr = kind == 1 && uiLayerFamilyTakesHdr(v.family);
    g.eye = 0;
    g.armed = true;
    g.lateTone = t.lateTone && g.crispHdr;
    g.familyEnabled = production ? uiLayerFamilyEnabled(v.family, t.qualityOn, t.sharpOn) : true;
    const UiDsEffect effect = uiLayerDsEffect(dsStateOf(r, dsv), dsv != nullptr);
    g.ds = effect;
    g.dsReproducible = !t.dsUnreproducible;
    g.blend = uiLayerBlendShape(blend);
    if (g.blend == UiBlendShape::kMultiply && effect.writes()) g.blend = UiBlendShape::kRefused;
    v.decision = uiLayerDecide(g);
    return v;
}

static bool isWeaponRow(const Row& r) { return weaponPairIndex(r.vs, r.ps) >= 0; }

static void censusTests(const char* path, unsigned weaponRows, unsigned partnerRows) {
    const Census c = loadCensus(path);
    unsigned weapons = 0, partners = 0, changed = 0, same = 0;
    std::map<UiLayerFamily, unsigned> otherTaken;
    for (const Row& r : c.rows) {
        const Verdict was = verdictOf(c, r, false), now = verdictOf(c, r, true);
        if (isWeaponRow(r)) {
            ++weapons;
            check(was.family == UiLayerFamily::kNone && was.decision == UiLayerDecision::kNotUi, "a weapons row was not UI before this arc");
            check(now.family == UiLayerFamily::kWeaponsFire && now.decision == UiLayerDecision::kRedirect, "a weapons row is the family and is redirected into the HDR layer");
            ++changed;
        } else if (r.vs == 0x820E5C131B99361Dull && r.ps == 0x6EAA86EFE135B2D4ull) {
            // the shield's refraction: not UI before, the shield-ripple family since; redirected (blend over, depth GEQUAL no write, no stencil)
            ++partners;
            check(was.family == UiLayerFamily::kNone && was.decision == UiLayerDecision::kNotUi, "a shield-refraction row was not UI before the shield ripple arc");
            check(now.family == UiLayerFamily::kShieldRipple && now.decision == UiLayerDecision::kRedirect, "...and is the shield-ripple family, redirected");
            ++changed;
        } else {
            check(now.family == was.family && now.decision == was.decision, "every other row is decided exactly as before");
            ++same;
            if (now.decision == UiLayerDecision::kRedirect) ++otherTaken[now.family];
        }
    }
    check(weapons == weaponRows, "the census frame holds the weapons rows the log counted");
    check(partners == partnerRows, "...and the shield's refraction rows");
    check(changed == weapons + partners && same + changed == c.rows.size(), "exactly the weapons rows and the shield-refraction rows change decision");

    // One-token mutants of the first weapons row: each is refused for the right reason, or is no longer the family.
    const Row* w = nullptr;
    for (const Row& r : c.rows)
        if (isWeaponRow(r)) {
            w = &r;
            break;
        }
    check(w != nullptr, "the frame holds a weapons row");
    {
        Tweak t;
        check(verdictOf(c, *w, true, t).decision == UiLayerDecision::kRedirect, "control: the unmutated weapons row is redirected");
        t = Tweak{};
        t.overridePs = true;
        t.ps = w->ps ^ 1ull;
        check(verdictOf(c, *w, true, t).family == UiLayerFamily::kNone, "mutant: another pixel shader is not the family");
        t = Tweak{};
        t.blendOp = 4;   // MIN
        check(verdictOf(c, *w, true, t).decision == UiLayerDecision::kBlendRefused, "mutant: blend MIN is refused");
        t = Tweak{};
        t.kind = 2;
        check(verdictOf(c, *w, true, t).family == UiLayerFamily::kNone, "mutant: an 8-bit target is not the family");
        t = Tweak{};
        t.dsUnreproducible = true;
        check(verdictOf(c, *w, true, t).decision == UiLayerDecision::kDepthStencilTest, "mutant: a depth state the layer cannot reproduce is refused");
        t = Tweak{};
        t.lateTone = true;
        check(verdictOf(c, *w, true, t).decision == UiLayerDecision::kToneLate, "mutant: a late tonemap is refused");
        t = Tweak{};
        t.sharpOn = false;
        check(verdictOf(c, *w, true, t).decision == UiLayerDecision::kFamilyOff, "mutant: fix.sharp_weapons off is refused for its key");
        t = Tweak{};
        t.qualityOn = false;
        check(verdictOf(c, *w, true, t).decision == UiLayerDecision::kRedirect, "fix.ui_quality off does not refuse the weapons fire");
    }

    // The four key combinations over the whole frame: the weapons rows follow fix.sharp_weapons, every other row fix.ui_quality (a row another family names
    // is redirected exactly when it was, iff fix.ui_quality is on; with it off it is refused for the key).
    for (int q = 0; q < 2; ++q) {
        for (int s = 0; s < 2; ++s) {
            Tweak t;
            t.qualityOn = q != 0;
            t.sharpOn = s != 0;
            for (const Row& r : c.rows) {
                const Verdict base = verdictOf(c, r, true);
                const Verdict v = verdictOf(c, r, true, t);
                if (v.family == UiLayerFamily::kNone) {
                    check(v.decision == UiLayerDecision::kNotUi, "a row no family names is not UI under any key");
                } else if (v.family == UiLayerFamily::kWeaponsFire) {
                    check((v.decision == UiLayerDecision::kRedirect) == (s != 0) && (v.decision == UiLayerDecision::kFamilyOff) == (s == 0),
                          "a weapons row is redirected iff fix.sharp_weapons is on, and refused for the key iff it is off");
                } else if (v.family == UiLayerFamily::kShieldRipple) {
                    check(v.decision == UiLayerDecision::kRedirect, "the shield-refraction rows follow the ripple's own half (fix.sharp_weapons's, stood down alone), not fix.ui_quality's, whatever the two say here");
                } else if (q) {
                    check(v.decision == base.decision, "with fix.ui_quality on every other row is decided as it was, whatever fix.sharp_weapons says");
                } else {
                    check(v.decision == UiLayerDecision::kFamilyOff || base.decision == UiLayerDecision::kVerdict, "with fix.ui_quality off every other family is refused for its key");
                }
            }
        }
    }
}

// ============================================================================================ the wiring
static std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    check(at != std::string::npos, "a control's text is in the source (the control is stale otherwise)");
    return text.replace(at, from.size(), to);
}
static bool has(const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; }
static bool before(const std::string& t, const std::string& a, const std::string& b) {
    const size_t i = t.find(a), j = t.find(b);
    return i != std::string::npos && j != std::string::npos && i < j;
}

static bool wiredKeys(const std::string& t) {
    return has(t, "cfg.getString(\"fix.sharp_weapons\", \"on\")") && has(t, "const SharpWeapons sharpMode = sharpWeaponsParse(sharpText.c_str(), &sharpKnown);") &&
           has(t, "const bool sharp = sharpWeaponsTakes(sharpMode);") && has(t, "const SharpBlend blend = sharpWeaponsBlend(sharpMode);") && !has(t, "advanced.sharp_weapons_blend\", ");
}
// The two halves are computed apart: fix.ui_quality's from its key alone, the weapons' from fix.sharp_weapons alone; the machinery is their union.
static bool wiredLiveness(const std::string& t) {
    return has(t, "const bool quality = uiLayerLiveFor(g_target, g_temporal, g_jitterAsShipped, g_stoodDown);") &&
           has(t, "g_weaponsLive = uiLayerWeaponsLiveFor(g_sharpWeapons, g_temporal, g_jitterAsShipped, g_stoodDown || g_weaponsStoodDown);") &&
           has(t, "detail::g_uiQualityLive = quality;") && has(t, "detail::g_uiLayerLive = uiLayerMachineryLive(quality, g_weaponsLive || g_rippleLive);") &&
           before(t, "const bool quality = uiLayerLiveFor(", "detail::g_uiLayerLive = uiLayerMachineryLive(");
}
static bool wiredDecision(const std::string& t) {
    return has(t, "f.familyEnabled = uiLayerFamilyEnabled(family, detail::g_uiQualityLive, g_weaponsLive, g_rippleLive);") &&
           has(t, "family == UiLayerFamily::kWeaponsFire\n                    ? weaponsTakeReady(ctx, f.eye)") &&
           has(t, "const bool cancel = !g_draw.weapons || weaponsFireJitterCancel(g_draw.vsHash);") &&
           has(t, "g_draw.jx = cancel ? jx : 0.0f;") && has(t, "g_draw.weapons = family == UiLayerFamily::kWeaponsFire;") &&
           before(t, "f.familyEnabled = uiLayerFamilyEnabled(", "UiLayerDecision d = uiLayerDecide(f);");
}
// The interface-only parts keep fix.ui_quality's liveness.
static bool wiredQualityOnly(const std::string& t) {
    return has(t, "bool uiLayerLiveForWorldRoute() { return detail::g_uiQualityLive; }") &&
           has(t, "    if (!detail::g_uiQualityLive) {\n        mapsLayerNotLive(gateFrame);") &&
           has(t, "if (!detail::g_uiQualityLive || uiLayerIssueBlocked()) {\n            why = UiWorldRefuse::kBeginRefused;") &&
           has(t, "    seedWritersCheck(ctx, count, instances, verdict, drawKind);\n") &&
           before(t, "    seedWritersCheck(ctx, count, instances, verdict, drawKind);\n", "    if (!detail::g_uiQualityLive) return false;\n    if (!taken[0] && !taken[1]) return false;");
}
static bool wiredVscreen(const std::string& t) {
    const size_t fn = t.find("UiLayerFamily uiLayerFamilyOf(State* s, char kind, UINT count) {");
    if (fn == std::string::npos) return false;
    const size_t skip = t.find("if (f.targetKind == 2 && !uiLayerQualityLive()) return UiLayerFamily::kNone;", fn);
    const size_t vs = t.find("f.vs = bindingShaderHash(BindSlot::Vs);", fn);
    const size_t ps = t.find("else if (f.targetKind == 1 && (weaponsFireVs(f.vs) || shieldRefractVs(f.vs)))", fn);
    if (skip == std::string::npos || vs == std::string::npos || ps == std::string::npos || !(skip < vs) || !(vs < ps)) return false;
    // the steps loop: after the game's tonemap draw is re-issued once, before the layer's End
    const size_t loop = t.find("while (uiLayerCrispToneNext(self)) pureDrawReissue(self, kind, count, instances, args);");
    const size_t first = t.rfind("pureDrawReissue(self, kind, count, instances, args);", loop);
    const size_t end = t.find("uiLayerCrispToneEnd(self);", loop);
    return loop != std::string::npos && first != std::string::npos && end != std::string::npos && first < loop && loop < end &&
           has(t, "} else if (owner && uiLayerQualityLive() && v != DrawVerdict::kQuadSkip) {");
}
static bool wiredSnapshot(const std::string& t) {
    const size_t fn = t.find("static bool watches(uint64_t vs) {");
    if (fn == std::string::npos) return false;
    const size_t end = t.find("default: return false;", fn);
    for (const char* v : {"case 0x1F17BF54DB6EE407ull:", "case 0x939D01F28D1FEEA8ull:", "case 0x5C1D8EF529324A22ull:"}) {
        const size_t at = t.find(v, fn);
        if (at == std::string::npos || at > end) return false;
    }
    return true;
}
static bool wiredIni(const std::string& t) {
    return has(t, "\nsharp_weapons = on\n") && has(t, "# ui: Sharp weapons | choices on=On, plain=On (plain blend), off=Off (stock) | live | menu performance") &&
           !has(t, "sharp_weapons_blend = ") && !has(t, "shield_ripple");
}

static void lostPins(const std::string& layer);   // below, with the instrument it pins
static void wiringTests() {
    const std::string layer = readFile("src/d3d11/ui_layer.cpp"), vscreen = readFile("src/d3d11/vscreen.cpp"), snap = readFile("src/d3d11/eye_draw_snapshot.h"),
                      ini = readFile("edvr.ini");
    check(layer.size() > 100000 && vscreen.size() > 100000 && snap.size() > 1000 && ini.size() > 10000, "the sources are readable from the repo root");

    check(wiredKeys(layer), "ui_layer.cpp reads fix.sharp_weapons as one word (default on), on = matched, plain = plain, off = stock, and no second key");
    check(!wiredKeys(replaced(layer, "cfg.getString(\"fix.sharp_weapons\", \"on\")", "cfg.getString(\"fix.sharp_weapons\", \"off\")")), "control: a key that defaults off trips the pin");
    check(!wiredKeys(replaced(layer, "const SharpBlend blend = sharpWeaponsBlend(sharpMode);", "const SharpBlend blend = SharpBlend::kMatched;")), "control: a plain that is never plain trips the pin");
    check(!wiredKeys(replaced(layer, "const bool sharp = sharpWeaponsTakes(sharpMode);", "const bool sharp = true;")), "control: an off that is never off trips the pin");
    check(!wiredKeys(layer + "\ncfg.getString(\"advanced.sharp_weapons_blend\", \"matched\");"), "control: a surviving second key trips the pin");

    check(wiredLiveness(layer), "ui_layer.cpp: fix.ui_quality's half and the weapons' half are computed apart and the machinery is their union");
    check(!wiredLiveness(replaced(layer, "const bool quality = uiLayerLiveFor(g_target, g_temporal, g_jitterAsShipped, g_stoodDown);",
                                  "const bool quality = uiLayerLiveFor(g_sharpWeapons ? 1.0f : g_target, g_temporal, g_jitterAsShipped, g_stoodDown);")),
          "control: a ui_quality half that follows the weapons' key trips the pin");
    check(!wiredLiveness(replaced(layer, "detail::g_uiLayerLive = uiLayerMachineryLive(quality, g_weaponsLive || g_rippleLive);", "detail::g_uiLayerLive = quality;")),
          "control: a machinery that ignores the weapons' half trips the pin");
    check(!wiredLiveness(replaced(layer, "detail::g_uiQualityLive = quality;", "detail::g_uiQualityLive = detail::g_uiLayerLive;")),
          "control: a quality flag that is the union trips the pin");

    check(wiredDecision(layer), "ui_layer.cpp: the decision gates each family by its own key, takes the weapons through their own readiness and cancels by the table");
    check(!wiredDecision(replaced(layer, "f.familyEnabled = uiLayerFamilyEnabled(family, detail::g_uiQualityLive, g_weaponsLive, g_rippleLive);", "f.familyEnabled = true;")), "control: no key gate trips the pin");
    check(!wiredDecision(replaced(layer, "uiLayerFamilyEnabled(family, detail::g_uiQualityLive, g_weaponsLive, g_rippleLive)", "uiLayerFamilyEnabled(family, detail::g_uiLayerLive, g_weaponsLive, g_rippleLive)")),
          "control: a ui_quality gate that reads the union trips the pin");
    check(!wiredDecision(replaced(layer, "g_draw.jx = cancel ? jx : 0.0f;", "g_draw.jx = jx;")), "control: a cancel that ignores the table trips the pin");
    check(!wiredDecision(replaced(layer, "weaponsFireJitterCancel(g_draw.vsHash)", "true")), "control: a table that is never read trips the pin");

    check(wiredQualityOnly(layer), "ui_layer.cpp: the world route, the on-foot gate and the after-UI take stay fix.ui_quality's");
    check(!wiredQualityOnly(replaced(layer, "bool uiLayerLiveForWorldRoute() { return detail::g_uiQualityLive; }", "bool uiLayerLiveForWorldRoute() { return detail::g_uiLayerLive; }")),
          "control: a world route that runs for the weapons trips the pin");
    check(!wiredQualityOnly(replaced(layer, "    if (!detail::g_uiQualityLive) return false;\n    if (!taken[0] && !taken[1]) return false;", "    if (!taken[0] && !taken[1]) return false;")),
          "control: an after-UI take that runs for the weapons trips the pin");

    check(wiredVscreen(vscreen), "vscreen.cpp: the pixel shader is read for the weapons' vertex shaders, the post-tonemap rules wait for fix.ui_quality, the steps are issued");
    check(!wiredVscreen(replaced(vscreen, "else if (f.targetKind == 1 && (weaponsFireVs(f.vs) || shieldRefractVs(f.vs)))", "else if (f.targetKind == 1 && false)")), "control: no pixel shader read for the pairs trips the pin");
    check(!wiredVscreen(replaced(vscreen, "if (f.targetKind == 2 && !uiLayerQualityLive()) return UiLayerFamily::kNone;", "")), "control: post-tonemap rules that run for the weapons trip the pin");
    check(!wiredVscreen(replaced(vscreen, "while (uiLayerCrispToneNext(self)) pureDrawReissue(self, kind, count, instances, args);", "")), "control: no loop over the steps trips the pin");

    check(wiredSnapshot(snap), "eye_draw_snapshot.h: the next eye dump watches the three weapons-fire vertex shaders (their constant buffers settle W3)");
    check(!wiredSnapshot(replaced(snap, "case 0x939D01F28D1FEEA8ull: // multicannon rounds\n", "")), "control: an unwatched pair trips the pin");

    lostPins(layer);
    {
        const size_t at = ini.find("\nsharp_weapons = on\n");
        const size_t from = ini.rfind("# Draw the ships' weapons fire sharp.", at);
        const std::string seg = (at != std::string::npos && from != std::string::npos) ? ini.substr(from, at - from) : std::string();
        check(!seg.empty() && has(seg, "shield-strike ripple"), "edvr.ini: fix.sharp_weapons says it covers the shield-strike ripple");
        check(has(seg, "plain shades the light alone") && has(seg, "off leaves all of it"), "...and says what on, plain and off do");
    }
    check(wiredIni(ini), "edvr.ini documents fix.sharp_weapons (on | plain | off, with one menu row), no second key and no ripple key");
    check(!wiredIni(replaced(ini, "\nsharp_weapons = on\n", "\nsharp_weapons = off\n")), "control: a key that ships off trips the pin");
}

// ============================================================================================ the flat profile
// fix.sharp_weapons covers both profiles (2026-10-16): the flat adapter (flat_ui_layer.cpp) asks the shared decision for the weapons fire and the shield
// refraction when their half of the layer is live, whatever fix.ui_quality says, leaves the HUD alone when only the weapons are live, runs the tone bracket's
// steps, and hands the door the fact that the picture is EDVR's resolve. The key is on flat's allow-list and its row sits under UI quality on the flat panel.
static bool wiredFlat(const std::string& adapter, const std::string& runtimeH, const std::string& keys) {
    return has(adapter, "if (!flatUiFamilyLive(family, uiLayerQualityLive(), uiLayerWeaponsLive(), uiLayerRippleLive())) return g_lastAsk;") &&
           before(adapter, "if (!flatUiLayerTakesFamily(family)) return g_lastAsk;", "if (!flatUiFamilyLive(family,") &&
           before(adapter, "if (!flatUiFamilyLive(family,", "++g_w.asked[fi];") &&
           has(adapter, "uiLayerComposite(frame, 0, picture.Get(), region, uv, false, treated)") &&
           has(adapter, "bool flatUiLayerToneNext(ID3D11DeviceContext* ctx) {") && has(adapter, "return uiLayerCrispToneNext(ctx);") &&
           has(runtimeH, "if (uiTone && flatUiLayerToneBegin(ctx)) { issue(); while (flatUiLayerToneNext(ctx)) issue(); flatUiLayerToneEnd(ctx); }") &&
           has(keys, "std::strcmp(key, \"fix.sharp_weapons\") == 0");
}
static void flatTests() {
    // The family rule.
    check(flatUiLayerTakesFamily(UiLayerFamily::kWeaponsFire) && flatUiLayerTakesFamily(UiLayerFamily::kShieldRipple), "the flat adapter takes the weapons fire and the shield ripple");
    check(flatUiLayerTakesFamily(UiLayerFamily::kFlightHud) && !flatUiLayerTakesFamily(UiLayerFamily::kHoloGeneric) && !flatUiLayerTakesFamily(UiLayerFamily::kOrbitLines) &&
              !flatUiLayerTakesFamily(UiLayerFamily::kSpaceDust),
          "...and still only the cockpit HUD families besides (not the holograms, not the scene lines)");
    // The pairs name their family through the shared rule the flat adapter uses.
    check(flatUiFamilyOf(kUiVsWeaponBeams, kUiPsWeaponBeams) == UiLayerFamily::kWeaponsFire && flatUiFamilyOf(kUiVsMulticannon, kUiPsMulticannon) == UiLayerFamily::kWeaponsFire &&
              flatUiFamilyOf(kUiVsShieldStrike, kUiPsShieldStrike) == UiLayerFamily::kWeaponsFire,
          "flat names the three weapons-fire pairs as the weapons family");
    check(flatUiFamilyOf(kUiVsShieldRefract, kUiPsShieldRefract) == UiLayerFamily::kShieldRipple, "...and the refraction pair as the shield ripple");
    check(flatUiFamilyOf(kUiVsWeaponBeams, kUiPsShieldRefract) != UiLayerFamily::kWeaponsFire && flatUiFamilyOf(kUiVsShieldRefract, kUiPsWeaponBeams) != UiLayerFamily::kShieldRipple,
          "...by the exact pair: a mixed pair is neither");
    // The three halves are asked separately: every combination.
    for (int q = 0; q < 2; ++q)
        for (int w = 0; w < 2; ++w)
            for (int r = 0; r < 2; ++r) {
                check(flatUiFamilyLive(UiLayerFamily::kWeaponsFire, q != 0, w != 0, r != 0) == (w != 0), "the weapons fire is asked when the weapons half is live, whatever else says");
                check(flatUiFamilyLive(UiLayerFamily::kShieldRipple, q != 0, w != 0, r != 0) == (r != 0), "the shield ripple is asked when its half is live, whatever else says");
                check(flatUiFamilyLive(UiLayerFamily::kFlightHud, q != 0, w != 0, r != 0) == (q != 0), "the HUD families are asked when fix.ui_quality's half is live and not otherwise");
                check(flatUiFamilyLive(UiLayerFamily::kMapCanvas, q != 0, w != 0, r != 0) == (q != 0), "...the System Map's too");
            }
    // The key and its row.
    check(flatPageHasRow("fix", "sharp_weapons") && !flatPageHasRow("advanced", "sharp_weapons_blend"), "the flat panel has the Sharp weapons row and no Weapons blend row");
    int quality = -1, sharp = -1;
    for (size_t i = 0; i < kFlatPageRowCount; ++i) {
        if (std::strcmp(kFlatPageRows[i].key, "ui_quality") == 0) quality = static_cast<int>(i);
        if (std::strcmp(kFlatPageRows[i].key, "sharp_weapons") == 0) sharp = static_cast<int>(i);
    }
    check(quality >= 0 && sharp == quality + 1, "...directly under UI quality, as in VR");
    check(sharp >= 0 && kFlatPageRows[sharp].read == FlatRowRead::Allowlist, "...read through the allow-listed getters (flat_profile_keys.h names the key)");
    const std::string adapter = readFile("src/d3d11/flat_ui_layer.cpp"), runtimeH = readFile("src/d3d11/flat_runtime.h"), keys = readFile("src/common/flat_profile_keys.h");
    check(adapter.size() > 10000 && runtimeH.size() > 1000 && keys.size() > 500, "the flat sources are readable from the repo root");
    check(wiredFlat(adapter, runtimeH, keys), "flat: the adapter asks the half that governs the family, the tone bracket runs its steps, the door is told the picture is resolved, the key is allow-listed");
    check(!wiredFlat(replaced(adapter, "if (!flatUiFamilyLive(family, uiLayerQualityLive(), uiLayerWeaponsLive(), uiLayerRippleLive())) return g_lastAsk;", ""), runtimeH, keys),
          "control: an adapter that asks the HUD with fix.ui_quality off (or the weapons with them off) trips the pin");
    check(!wiredFlat(replaced(adapter, "uv, false, treated)", "uv)"), runtimeH, keys), "control: a door never told whether the picture is resolved trips the pin");
    check(!wiredFlat(adapter, replaced(runtimeH, "while (flatUiLayerToneNext(ctx)) issue(); ", ""), keys), "control: a tone bracket with no steps (the matched composite would be one issue short) trips the pin");
    check(!wiredFlat(adapter, runtimeH, replaced(keys, "std::strcmp(key, \"fix.sharp_weapons\") == 0 ||", "")), "control: a key flat's gate refuses (getString answers off) trips the pin");
}

// ============================================================================================ the menu rows
// The one Sharp weapons row (On, On (plain blend), Off (stock)): Fix tier, the Performance page, live, directly under UI quality in the table the build generated
// from the real edvr.ini; there is no Weapons blend row. Dimmed, and refusing steps and typing, with anti-aliasing off.
static int rowIndex(const char* section, const char* key) {
    const int count = static_cast<int>(sizeof(kMenuRows) / sizeof(kMenuRows[0]));
    for (int i = 0; i < count; ++i)
        if (std::strcmp(kMenuRows[i].section, section) == 0 && std::strcmp(kMenuRows[i].key, key) == 0) return i;
    return -1;
}
// menu.cpp's gating of the two rows: one function decides, the page draws them dimmed, a step and a typed value are refused, and the profile's own reading of
// fix.temporal_aa is the one asked.
static bool gatedMenu(const std::string& t) {
    return has(t, "bool isWeaponsRow(const MenuRowDef& d) {") && has(t, "strcmp(d.key, \"sharp_weapons\") == 0") &&
           has(t, "return weaponsRowDim(temporalModeEnabledFor(flat ? cfg.requestedTemporalMode() : cfg.getString(\"fix.temporal_aa\", \"off\"), flat));") &&
           has(t, "(weaponsRow && weaponsRowDisabled())") && has(t, "if (isWeaponsRow(d) && weaponsRowDisabled()) {") &&
           has(t, "if (isWeaponsRow(kMenuRows[defIndex]) && weaponsRowDisabled()) {") && has(t, "Needs temporal AA");
}
static void menuTests() {
    const int count = static_cast<int>(sizeof(kMenuRows) / sizeof(kMenuRows[0]));
    check(count > 10, "the generated menu table is there");
    const int quality = rowIndex("fix", "ui_quality"), sharp = rowIndex("fix", "sharp_weapons");
    check(quality >= 0 && sharp >= 0, "UI quality and Sharp weapons are rows");
    check(sharp == quality + 1, "Sharp weapons is directly under UI quality");
    check(rowIndex("advanced", "sharp_weapons_blend") < 0, "there is no Weapons blend row (and so no second one in the developer tier)");
    if (sharp >= 0) {
        const MenuRowDef& r = kMenuRows[sharp];
        check(r.tier == MenuTier::Fix && std::strcmp(r.page, "performance") == 0, "Sharp weapons is a Fix-tier row on the Performance page");
        check(r.kind == MenuKind::Choice && r.applies == 1 && std::strcmp(r.shipped, "on") == 0, "...a live choice that ships on");
        check(std::strcmp(r.label, "Sharp weapons") == 0, "...labelled plainly");
        check(std::strcmp(r.choices, "on=On|plain=On (plain blend)|off=Off (stock)") == 0, "...with On, On (plain blend) and Off (stock) as its choices");
    }

    // The gate: with anti-aliasing off both rows are dimmed; on, neither. The same table in both profiles (the caller passes the profile's own reading).
    check(weaponsRowDim(false) && !weaponsRowDim(true), "the weapons row dims exactly when anti-aliasing is off");
    const std::string menu = readFile("src/d3d11/menu.cpp");
    check(menu.size() > 100000, "menu.cpp is readable from the repo root");
    check(gatedMenu(menu), "menu.cpp dims, refuses steps and refuses typing on the row, asking each profile's own reading of fix.temporal_aa");
    check(!gatedMenu(replaced(menu, "(weaponsRow && weaponsRowDisabled())", "false")), "control: a row that never dims is caught");
    check(!gatedMenu(replaced(menu, "if (isWeaponsRow(d) && weaponsRowDisabled()) {", "if (false) {")), "control: a dimmed row that still takes a step is caught");
    check(!gatedMenu(replaced(menu, "if (isWeaponsRow(kMenuRows[defIndex]) && weaponsRowDisabled()) {", "if (false) {")), "control: a dimmed row that still takes typing is caught");
    check(!gatedMenu(replaced(menu, "flat ? cfg.requestedTemporalMode() : cfg.getString(\"fix.temporal_aa\", \"off\"), flat)", "cfg.getString(\"fix.temporal_aa\", \"off\"), false)")),
          "control: a flat profile read as the VR one (it would miss a requested mode the allow-list hides) is caught");
    check(!gatedMenu(replaced(menu, "strcmp(d.key, \"sharp_weapons\") == 0", "false")), "control: the row left out of the gate is caught");
}

// ============================================================================================ the lost-layer instrument
// "N layers' content never reached a tonemap" (0.5 to 5 percent of the weapon eye-frames of the 2026-10-16 combat flights, none for the HUD layer):
// the cause is open, so each loss prints the eye's state and the tonemap-shaped draws the admission saw (ui_layer.cpp noteWeaponsLost / noteToneSight).
// The bookkeeping of one eye's weapons layer, frame by frame, with the drop test as it was (frozen: the layer's content frame against the latest TONEMAP's
// frame, which the HUD's re-issue advances every frame) and as it is (weaponsLayerDropped: against the last frame a re-issue CARRIED the content). The
// 2026-10-16 10:36 flight's lost-layer lines were one per burst end: a layer holding one draw, then ~200 quiet frames, then the next burst's first draw
// comparing the content's frame with the latest tonemap's. Nothing had been dropped.
struct EyeModel {
    uint64_t wSeq = 0, wToneSeq = 0, wPubSeq = 0;
    uint32_t wDraws = 0;
    unsigned lostOld = 0, lostNew = 0, carriedFrames = 0, contentFrames = 0;
    // One eye-frame: `draws` weapons draws (0: none), a re-issue ran (the HUD's, every frame in flight), and it carried the weapons content.
    void frame(uint64_t f, uint32_t draws, bool tone, bool carries) {
        if (draws) {
            if (wSeq && wDraws && wToneSeq != wSeq) ++lostOld;                       // the frozen rule
            if (weaponsLayerDropped(wSeq, wDraws, wPubSeq)) ++lostNew;               // the production rule
            wSeq = f;
            wDraws = draws;
            ++contentFrames;
        }
        if (tone) {
            wToneSeq = f;
            if (draws && carries) {
                wPubSeq = f;
                ++carriedFrames;
            }
        }
    }
};
static void dropTests() {
    // A burst ending in one draw, then quiet frames, then the next burst: nothing was dropped.
    {
        EyeModel m;
        uint64_t f = 1;
        for (int i = 0; i < 5; ++i) m.frame(f++, 3, true, true);   // a multicannon burst
        m.frame(f++, 1, true, true);                               // its last draw, alone
        const uint64_t lone = f - 1;
        for (int i = 0; i < 200; ++i) m.frame(f++, 0, true, true);  // quiet, the HUD's tonemap running every frame
        check(m.wPubSeq == lone && m.carriedFrames == 6, "the lone final draw was carried by its own frame's re-issue");
        m.frame(f++, 2, true, true);                               // the next burst
        check(m.lostNew == 0, "a burst's last draw followed by quiet frames is not a drop");
        check(m.lostOld == 1, "fail-before: the frozen rule called it lost when the next burst began");
        std::printf("burst ending in one draw, 200 quiet frames, next burst: frozen rule %u lost, production rule %u lost, %u of %u eye-frames carried\n",
                    m.lostOld, m.lostNew, m.carriedFrames, m.contentFrames);
    }
    // Sparse bolts (one lone draw, quiet, repeat): the frozen rule counted nearly every one.
    {
        EyeModel m;
        uint64_t f = 1;
        for (int b = 0; b < 50; ++b) {
            m.frame(f++, 1, true, true);
            for (int i = 0; i < 30; ++i) m.frame(f++, 0, true, true);
        }
        check(m.lostNew == 0 && m.contentFrames == 50 && m.carriedFrames == 50, "fifty lone draws, all carried, none dropped");
        check(m.lostOld == 49, "fail-before: the frozen rule counted 49 of the 50 as lost");
    }
    // A real drop is still caught: content drawn, and no re-issue at all in its frame (the next frame's re-issue has nothing of it)...
    {
        EyeModel m;
        m.frame(10, 2, false, false);
        m.frame(11, 0, true, true);
        m.frame(12, 1, true, true);
        check(m.lostNew == 1, "content with no re-issue in its frame is a drop");
    }
    // ...and so is content a re-issue ran past without carrying (the frozen rule missed this one: its latest-tonemap frame equalled the content's).
    {
        EyeModel m;
        m.frame(10, 2, true, false);
        m.frame(12, 1, true, true);
        check(m.lostNew == 1, "a re-issue that ran in the content's frame but did not carry it is a drop");
        check(m.lostOld == 0, "fail-before: the frozen rule could not see that one");
    }
    // Consecutive frames of weapons fire: every frame carried, none dropped, by either rule.
    {
        EyeModel m;
        for (uint64_t f = 1; f <= 100; ++f) m.frame(f, 4, true, true);
        check(m.lostNew == 0 && m.lostOld == 0 && m.carriedFrames == 100, "a continuous burst drops nothing");
    }
    check(!weaponsLayerDropped(0, 0, 0) && !weaponsLayerDropped(5, 0, 0) && !weaponsLayerDropped(0, 3, 0) && !weaponsLayerDropped(7, 2, 7) && weaponsLayerDropped(7, 2, 6) &&
              weaponsLayerDropped(7, 2, 0),
          "weaponsLayerDropped: an empty layer, a never-used layer and a carried layer are not drops; a layer carried at another frame, or never, is");
}

static bool wiredLost(const std::string& t) {
    const size_t loss = t.find("++g_win.weaponsLost;\n            noteWeaponsLost(g_draw.eye, e, g_draw.seq);");
    size_t sights = 0, at = 0;
    while ((at = t.find("noteToneSight(seq, seenRes, ", at)) != std::string::npos) {
        ++sights;
        ++at;
    }
    return loss != std::string::npos && sights == 4 && has(t, "void noteToneSight(uint64_t seq, const void* res, int eye, uint8_t outcome) {") &&
           has(t, "if (e.wTarget != g_draw.targetRes) ++e.wOtherTargets;") && has(t, "e.wOtherTargets = 0;") && has(t, "if (!seenRes) seenRes = r.Get();") &&
           has(t, "if (weaponsLayerDropped(e.wSeq, e.wDraws, e.wPubSeq)) {") &&
           has(t, "    if (hadWeapons) {\n        // Stamped ONLY by a bracket that carried the weapons layer's content") && has(t, "        e.wPubSeq = seq;\n        e.wMissStreak = 0;") &&
           has(t, "if (g_lostLines >= 24) return;") && before(t, "noteToneSight(seq, seenRes, eye, 0);", "crispArm(eye, seq, hdrSlot, ta.rtvRes, hdrRes, vs, ps);");
}
static void lostPins(const std::string& layer) {
    check(wiredLost(layer), "ui_layer.cpp: a lost weapons layer prints the eye's state and the last tonemap-shaped draws, rate-limited, and every outcome of the admission is recorded");
    check(!wiredLost(replaced(layer, "++g_win.weaponsLost;\n            noteWeaponsLost(g_draw.eye, e, g_draw.seq);", "++g_win.weaponsLost;")), "control: no line at the loss trips the pin");
    check(!wiredLost(replaced(layer, "        noteToneSight(seq, seenRes, eye, 3);\n", "")), "control: an unrecorded outcome trips the pin");
    check(!wiredLost(replaced(layer, "    noteToneSight(seq, seenRes, eye, 0);\n", "")), "control: an unrecorded admission trips the pin");
    check(!wiredLost(replaced(layer, "if (g_lostLines >= 24) return;", "")), "control: an unlimited line trips the pin");
    check(!wiredLost(replaced(layer, "if (weaponsLayerDropped(e.wSeq, e.wDraws, e.wPubSeq)) {", "if (e.wSeq && e.wDraws && e.wToneSeq != e.wSeq) {")),
          "control: the frozen drop test (against the latest tonemap's frame) trips the pin");
    check(!wiredLost(replaced(layer, "    if (hadWeapons) {\n        // Stamped ONLY", "    {\n        // Stamped ONLY")), "control: a publication stamped by every re-issue trips the pin");
}

// ============================================================================================ the composite (WARP)
static const char* kToneVs = R"HLSL(
struct V { float4 p : SV_POSITION; float2 uv : TEXCOORD0; };
V main(uint id : SV_VertexID) {
    float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(-1, 3) : float2(3, -1));
    V v;
    v.p = float4(p, 0, 1);
    v.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return v;
}
)HLSL";
// The synthetic game tonemap: the shape of Elite's (a 3D LUT at t0, the HDR source at t1, a constant buffer at b2 with the exposure, a linear sampler), with a
// compressing curve in front of the LUT so that T(F + L) is not T(F) + T(L). T(0) = 0.
static const char* kTonePs = R"HLSL(
Texture3D<float4> Lut : register(t0);
Texture2D<float4> Hdr : register(t1);
SamplerState Lin : register(s0);
cbuffer B2 : register(b2) { float4 k; }
float4 main(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = Hdr.Sample(Lin, uv).rgb * k.x;
    c = c / (1.0 + c);
    float3 o = Lut.Sample(Lin, (c * 7.0 + 0.5) / 8.0).rgb;
    return float4(o, 1);
}
)HLSL";

constexpr uint32_t kLw = 16, kLh = 16, kFw = 8, kFh = 8;   // the layer's size and the render's: a 2x upscale, as the flights'

static uint16_t toHalf(float v) { return DirectX::PackedVector::XMConvertFloatToHalf(v); }
static float fromHalf(uint16_t v) { return DirectX::PackedVector::XMConvertHalfToFloat(v); }
static float roundHalf(float v) { return fromHalf(toHalf(v)); }

struct Rgb { float r = 0, g = 0, b = 0; };

// The scene's HDR input F (render size): a lit planet-like half (left) and dark space (right), with a gradient so the resample is not a copy.
static Rgb sceneAt(uint32_t x, uint32_t y) {
    if (x >= kFw / 2) return {0, 0, 0};
    return {1.5f + 0.25f * x, 1.0f + 0.1f * y, 0.5f};
}
// The weapons' light L (layer size): a diagonal band across both halves.
static Rgb lightAt(uint32_t x, uint32_t y) {
    const int d = static_cast<int>(x) - static_cast<int>(y);
    if (d >= -1 && d <= 1) return {2.4f, 1.2f, 0.6f};
    return {0, 0, 0};
}
// The HUD's light H (layer size): a block in a corner of the lit half and a block in space.
static Rgb hudAt(uint32_t x, uint32_t y) {
    if (x < 3 && y >= 12) return {0.8f, 0.8f, 0.2f};
    if (x >= 12 && y < 3) return {0.3f, 0.9f, 0.9f};
    return {0, 0, 0};
}

// What the R11G11B10 target holds for a value: the stored (packed) value, the one the GPU reads back.
static Rgb packRound(Rgb c) {
    DirectX::PackedVector::XMFLOAT3PK pk;
    DirectX::PackedVector::XMStoreFloat3PK(&pk, DirectX::XMVectorSet(c.r, c.g, c.b, 0));
    const DirectX::XMVECTOR v = DirectX::PackedVector::XMLoadFloat3PK(&pk);
    return {DirectX::XMVectorGetX(v), DirectX::XMVectorGetY(v), DirectX::XMVectorGetZ(v)};
}
// The first layer column that is dark space in the resample (the lit/dark seam of the render is between its columns 3 and 4, layer columns 7 and 8).
constexpr uint32_t kDarkFrom = 9;

// F resampled to the layer's size by the source pass's filter, on the CPU (the interpolated uv is the pixel centre; clamp at the edges).
static Rgb bilinearAt(uint32_t lx, uint32_t ly) {
    const float qx = (lx + 0.5f) / kLw * kFw - 0.5f, qy = (ly + 0.5f) / kLh * kFh - 0.5f;
    const int x0 = static_cast<int>(std::floor(qx)), y0 = static_cast<int>(std::floor(qy));
    const float fx = qx - x0, fy = qy - y0;
    const auto at = [](int x, int y) {
        x = std::min(std::max(x, 0), static_cast<int>(kFw) - 1);
        y = std::min(std::max(y, 0), static_cast<int>(kFh) - 1);
        return packRound(sceneAt(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
    };
    const Rgb a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    const auto mix = [](float p, float q, float t) { return p + (q - p) * t; };
    return {mix(mix(a.r, b.r, fx), mix(c.r, d.r, fx), fy), mix(mix(a.g, b.g, fx), mix(c.g, d.g, fx), fy), mix(mix(a.b, b.b, fx), mix(c.b, d.b, fx), fy)};
}

struct Fx {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx, deferred;
    ComPtr<ID3D11InfoQueue> info;
    ComPtr<ID3D11VertexShader> srcVs, toneVs;
    ComPtr<ID3D11PixelShader> srcPs, tonePs;
    ComPtr<ID3D11ShaderResourceView> lutSrv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> b2;
    SharpPassStates states;
    ComPtr<ID3D11Texture2D> fTex, hTex, wTex, sTex, eTex, refTex;
    ComPtr<ID3D11ShaderResourceView> fSrv, hSrv, wSrv, sSrv;
    ComPtr<ID3D11RenderTargetView> wRtv, sRtv, eRtv;
    bool debugLayer = false;

    ComPtr<ID3DBlob> compile(const char* text, const char* profile) {
        ComPtr<ID3DBlob> code, err;
        const HRESULT r = D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &err);
        if (FAILED(r) && err) std::fprintf(stderr, "%s\n", static_cast<const char*>(err->GetBufferPointer()));
        hr(r, "the rig's shader compiles");
        return code;
    }
    ComPtr<ID3D11Texture2D> texture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, const void* data, UINT pitch) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
        ComPtr<ID3D11Texture2D> t;
        hr(dev->CreateTexture2D(&d, data ? &sd : nullptr, &t), "a rig texture");
        return t;
    }
    ComPtr<ID3D11ShaderResourceView> srvOf(ID3D11Texture2D* t) {
        ComPtr<ID3D11ShaderResourceView> s;
        hr(dev->CreateShaderResourceView(t, nullptr, &s), "a rig SRV");
        return s;
    }
    ComPtr<ID3D11RenderTargetView> rtvOf(ID3D11Texture2D* t) {
        ComPtr<ID3D11RenderTargetView> r;
        hr(dev->CreateRenderTargetView(t, nullptr, &r), "a rig RTV");
        return r;
    }
    ComPtr<ID3D11Texture2D> halfTexture(uint32_t w, uint32_t h, Rgb (*fn)(uint32_t, uint32_t)) {
        std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const Rgb c = fn(x, y);
                uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = toHalf(c.r);
                p[1] = toHalf(c.g);
                p[2] = toHalf(c.b);
                p[3] = toHalf(1.0f);
            }
        return texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, px.data(), w * 8);
    }

    Fx() {
        D3D_FEATURE_LEVEL level;
        UINT flags = D3D11_CREATE_DEVICE_DEBUG;
        HRESULT r = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        if (FAILED(r)) {
            flags = 0;
            r = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        }
        hr(r, "a WARP device");
        debugLayer = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
        if (debugLayer) dev.As(&info);
        hr(dev->CreateDeferredContext(0, &deferred), "a deferred context");
        // The build's bytecode for the source pass (the bytes the DLL runs); the rig's own text for the synthetic game tonemap.
        hr(dev->CreateVertexShader(kSharpWeaponsSourceVsBytecode, sizeof(kSharpWeaponsSourceVsBytecode), nullptr, &srcVs), "the build's source-pass VS");
        hr(dev->CreatePixelShader(kSharpWeaponsSourcePsBytecode, sizeof(kSharpWeaponsSourcePsBytecode), nullptr, &srcPs), "the build's source-pass PS");
        auto tv = compile(kToneVs, "vs_5_0"), tp = compile(kTonePs, "ps_5_0");
        hr(dev->CreateVertexShader(tv->GetBufferPointer(), tv->GetBufferSize(), nullptr, &toneVs), "the synthetic tonemap VS");
        hr(dev->CreatePixelShader(tp->GetBufferPointer(), tp->GetBufferSize(), nullptr, &tonePs), "the synthetic tonemap PS");
        check(states.create(dev.Get()), "the production blend states are made");
        // The LUT: 8x8x8, a gamma curve per channel with a little red in green.
        {
            std::vector<uint8_t> lut(8 * 8 * 8 * 4);
            for (int z = 0; z < 8; ++z)
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) {
                        const float lr = std::pow(x / 7.0f, 1.0f / 2.2f), g = 0.9f * std::pow(y / 7.0f, 1.0f / 2.2f) + 0.1f * lr, b = std::pow(z / 7.0f, 1.0f / 2.2f);
                        uint8_t* p = &lut[((static_cast<size_t>(z) * 8 + y) * 8 + x) * 4];
                        p[0] = static_cast<uint8_t>(std::lround(lr * 255.0f));
                        p[1] = static_cast<uint8_t>(std::lround(g * 255.0f));
                        p[2] = static_cast<uint8_t>(std::lround(b * 255.0f));
                        p[3] = 255;
                    }
            D3D11_TEXTURE3D_DESC d{};
            d.Width = d.Height = d.Depth = 8;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{lut.data(), 8 * 4, 8 * 8 * 4};
            ComPtr<ID3D11Texture3D> t;
            hr(dev->CreateTexture3D(&d, &sd, &t), "the LUT");
            hr(dev->CreateShaderResourceView(t.Get(), nullptr, &lutSrv), "the LUT's SRV");
        }
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr(dev->CreateSamplerState(&sd, &sampler), "the tonemap's sampler");
        const float k[4] = {1.0f, 0, 0, 0};   // the exposure
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA kd{k, 0, 0};
        hr(dev->CreateBuffer(&bd, &kd, &b2), "the tonemap's constants");

        // F as the game has it: R11G11B10_FLOAT at the render's size.
        {
            std::vector<uint32_t> px(kFw * kFh);
            for (uint32_t y = 0; y < kFh; ++y)
                for (uint32_t x = 0; x < kFw; ++x) {
                    const Rgb c = sceneAt(x, y);
                    DirectX::PackedVector::XMFLOAT3PK pk;
                    DirectX::PackedVector::XMStoreFloat3PK(&pk, DirectX::XMVectorSet(c.r, c.g, c.b, 0));
                    px[y * kFw + x] = pk.v;
                }
            fTex = texture(kFw, kFh, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_SHADER_RESOURCE, px.data(), kFw * 4);
            fSrv = srvOf(fTex.Get());
        }
        hTex = halfTexture(kLw, kLh, hudAt);
        hSrv = srvOf(hTex.Get());
        eTex = texture(kLw, kLh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        eRtv = rtvOf(eTex.Get());
    }

    // The weapons layer W holding L, and a fresh scratch.
    void freshLayers() {
        wTex = halfTexture(kLw, kLh, lightAt);
        wSrv = srvOf(wTex.Get());
        wRtv = rtvOf(wTex.Get());
        sTex = halfTexture(kLw, kLh, [](uint32_t, uint32_t) { return Rgb{}; });
        sSrv = srvOf(sTex.Get());
        sRtv = rtvOf(sTex.Get());
    }

    // The game's tonemap draw, issued once for a step: the HDR source at t1, the blend over E; everything else the game's own.
    void toneDraw(ID3D11ShaderResourceView* hdr, ID3D11RenderTargetView* target, ID3D11BlendState* blend) {
        ID3D11RenderTargetView* rt = target;
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFFu);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(toneVs.Get(), nullptr, 0);
        ctx->PSSetShader(tonePs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {lutSrv.Get(), hdr};
        ctx->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* s = sampler.Get();
        ctx->PSSetSamplers(0, 1, &s);
        ID3D11Buffer* cb = b2.Get();
        ctx->PSSetConstantBuffers(2, 1, &cb);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kLw), static_cast<float>(kLh), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, none);
    }
    void clearEye() {
        const float c[4] = {0, 0, 0, 1};
        ctx->ClearRenderTargetView(eRtv.Get(), c);
    }

    std::vector<uint8_t> read8(ID3D11Texture2D* t) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        hr(dev->CreateTexture2D(&d, nullptr, &stage), "a staging copy");
        ctx->CopyResource(stage.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map");
        std::vector<uint8_t> out(static_cast<size_t>(d.Width) * d.Height * 4);
        for (uint32_t y = 0; y < d.Height; ++y) std::memcpy(&out[static_cast<size_t>(y) * d.Width * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, d.Width * 4);
        ctx->Unmap(stage.Get(), 0);
        return out;
    }
    std::vector<float> readHalf(ID3D11Texture2D* t) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        hr(dev->CreateTexture2D(&d, nullptr, &stage), "a staging copy");
        ctx->CopyResource(stage.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map");
        std::vector<float> out(static_cast<size_t>(d.Width) * d.Height * 4);
        for (uint32_t y = 0; y < d.Height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(m.pData) + y * m.RowPitch);
            for (uint32_t i = 0; i < d.Width * 4; ++i) out[static_cast<size_t>(y) * d.Width * 4 + i] = fromHalf(row[i]);
        }
        ctx->Unmap(stage.Get(), 0);
        return out;
    }
    // The tonemap of a reference HDR texture (made from CPU values), into a throw-away 8-bit target.
    std::vector<uint8_t> toneOf(const std::vector<Rgb>& hdr) {
        std::vector<uint16_t> px(static_cast<size_t>(kLw) * kLh * 4);
        for (size_t i = 0; i < hdr.size(); ++i) {
            px[i * 4 + 0] = toHalf(hdr[i].r);
            px[i * 4 + 1] = toHalf(hdr[i].g);
            px[i * 4 + 2] = toHalf(hdr[i].b);
            px[i * 4 + 3] = toHalf(1.0f);
        }
        auto t = texture(kLw, kLh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, px.data(), kLw * 8);
        auto s = srvOf(t.Get());
        auto out = texture(kLw, kLh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0);
        auto r = rtvOf(out.Get());
        toneDraw(s.Get(), r.Get(), states.toneReplace.Get());
        return read8(out.Get());
    }

    // The production sequence for a plan, as ui_layer.cpp runs it: clear, the source pass, the steps in order.
    void runPlan(const TonePlan& plan, const SharpPassStates& st) {
        if (plan.clearEye) clearEye();
        if (plan.sourcePass) {
            check(sharpSourcePass(ctx.Get(), deferred.Get(), srcVs.Get(), srcPs.Get(), st, fSrv.Get(), sRtv.Get(), wRtv.Get(), kLw, kLh,
                                  [](ID3D11DeviceContext* c, ID3D11CommandList* l, int restore) { c->ExecuteCommandList(l, restore); }),
                  "the production source pass runs");
        }
        for (uint8_t i = 0; i < plan.count; ++i) {
            const SharpStepBinding b = sharpStepBinding(plan.steps[i], hSrv.Get(), wSrv.Get(), sSrv.Get(), st);
            toneDraw(b.source, eRtv.Get(), b.blend);
        }
    }
};

static int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t xLo, uint32_t xHi, bool onlyBeam) {
    int worst = 0;
    for (uint32_t y = 0; y < kLh; ++y)
        for (uint32_t x = xLo; x < xHi; ++x) {
            if (onlyBeam) {
                const Rgb l = lightAt(x, y);
                if (l.r == 0 && l.g == 0 && l.b == 0) continue;
            }
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(static_cast<int>(a[(y * kLw + x) * 4 + c]) - static_cast<int>(b[(y * kLw + x) * 4 + c])));
        }
    return worst;
}
static std::vector<uint8_t> addClamped(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> out(a.size());
    for (size_t i = 0; i < a.size(); ++i) out[i] = static_cast<uint8_t>(std::min(255, static_cast<int>(a[i]) + static_cast<int>(b[i])));
    return out;
}

static void gpuTests() {
    Fx f;
    // Sentinel state on the immediate context, to see the source pass put it back.
    ComPtr<ID3D11Texture2D> sentinelTex = f.texture(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    ComPtr<ID3D11RenderTargetView> sentinelRtv = f.rtvOf(sentinelTex.Get());

    // The CPU references. F_up and the sums, rounded to half as the layers store them.
    std::vector<Rgb> fUp(kLw * kLh), sum(kLw * kLh), lightOnly(kLw * kLh), hudOnly(kLw * kLh);
    for (uint32_t y = 0; y < kLh; ++y)
        for (uint32_t x = 0; x < kLw; ++x) {
            const Rgb up = bilinearAt(x, y), l = lightAt(x, y), h = hudAt(x, y);
            fUp[y * kLw + x] = {roundHalf(up.r), roundHalf(up.g), roundHalf(up.b)};
            sum[y * kLw + x] = {roundHalf(roundHalf(l.r) + up.r), roundHalf(roundHalf(l.g) + up.g), roundHalf(roundHalf(l.b) + up.b)};
            lightOnly[y * kLw + x] = {roundHalf(l.r), roundHalf(l.g), roundHalf(l.b)};
            hudOnly[y * kLw + x] = {roundHalf(h.r), roundHalf(h.g), roundHalf(h.b)};
        }
    const std::vector<uint8_t> Y = f.toneOf(fUp);          // T(F_up): what the upscaled picture holds in this model
    const std::vector<uint8_t> X = f.toneOf(sum);          // T(F_up + L): what the game would have drawn with the weapons in the scene
    const std::vector<uint8_t> TL = f.toneOf(lightOnly);   // T(L)
    const std::vector<uint8_t> TH = f.toneOf(hudOnly);     // T(H)

    // ---- the matched sequence, weapons alone
    f.freshLayers();
    {
        ID3D11RenderTargetView* s = sentinelRtv.Get();
        f.ctx->OMSetRenderTargets(1, &s, nullptr);
        const D3D11_VIEWPORT vp{1, 1, 3, 3, 0.1f, 0.9f};
        f.ctx->RSSetViewports(1, &vp);
        f.ctx->VSSetShader(f.toneVs.Get(), nullptr, 0);
        f.ctx->PSSetShader(f.tonePs.Get(), nullptr, 0);
        const TonePlan matched = sharpTonePlan(false, true, SharpBlend::kMatched, true);
        // the source pass alone first, to read S and W
        f.clearEye();
        check(sharpSourcePass(f.ctx.Get(), f.deferred.Get(), f.srcVs.Get(), f.srcPs.Get(), f.states, f.fSrv.Get(), f.sRtv.Get(), f.wRtv.Get(), kLw, kLh,
                              [](ID3D11DeviceContext* c, ID3D11CommandList* l, int restore) { c->ExecuteCommandList(l, restore); }),
              "the source pass runs");
        ComPtr<ID3D11RenderTargetView> rt;
        f.ctx->OMGetRenderTargets(1, &rt, nullptr);
        check(rt.Get() == sentinelRtv.Get(), "the immediate context's render target is back after the source pass");
        D3D11_VIEWPORT back{};
        UINT n = 1;
        f.ctx->RSGetViewports(&n, &back);
        check(n == 1 && back.TopLeftX == 1 && back.Width == 3 && back.MinDepth == 0.1f, "...and its viewport");
        ComPtr<ID3D11VertexShader> vsBack;
        ComPtr<ID3D11PixelShader> psBack;
        f.ctx->VSGetShader(&vsBack, nullptr, nullptr);
        f.ctx->PSGetShader(&psBack, nullptr, nullptr);
        check(vsBack.Get() == f.toneVs.Get() && psBack.Get() == f.tonePs.Get(), "...and its shaders");
        const std::vector<float> S = f.readHalf(f.sTex.Get()), W = f.readHalf(f.wTex.Get());
        float worstS = 0, worstW = 0;
        for (size_t i = 0; i < fUp.size(); ++i) {
            worstS = std::max({worstS, std::fabs(S[i * 4 + 0] - fUp[i].r), std::fabs(S[i * 4 + 1] - fUp[i].g), std::fabs(S[i * 4 + 2] - fUp[i].b)});
            worstW = std::max({worstW, std::fabs(W[i * 4 + 0] - sum[i].r), std::fabs(W[i * 4 + 1] - sum[i].g), std::fabs(W[i * 4 + 2] - sum[i].b)});
            check(W[i * 4 + 3] == 1.0f, "the weapons layer's alpha is untouched by the add");
        }
        check(worstS < 0.01f, "S holds the scene's HDR input resampled to the layer's size (bilinear, to half precision)");
        check(worstW < 0.01f, "W holds the light plus that resample");
        std::printf("source pass: S within %.5f of the CPU resample, W within %.5f of L + F_up\n", static_cast<double>(worstS), static_cast<double>(worstW));
        f.freshLayers();
        f.runPlan(matched, f.states);
        const std::vector<uint8_t> E = f.read8(f.eTex.Get());
        // E is the difference X - Y (clamped at 0, 8-bit): compare with the same difference of the references
        std::vector<uint8_t> delta(X.size());
        for (size_t i = 0; i < X.size(); ++i) delta[i] = (i % 4 == 3) ? 255 : static_cast<uint8_t>(std::max(0, static_cast<int>(X[i]) - static_cast<int>(Y[i])));
        const int dErr = maxDiff(E, delta, 0, kLw, false);
        check(dErr <= 3, "the matched sequence leaves T(F_up + L) - T(F_up) in the 8-bit layer, to the 8-bit rounding");
        // the composite: the upscaled picture plus the layer
        const std::vector<uint8_t> matchedOut = addClamped(Y, E);
        const int litMatched = maxDiff(matchedOut, X, 0, kFw, true), darkMatched = maxDiff(matchedOut, X, kDarkFrom, kLw, true);
        std::vector<uint8_t> plainE = TL;
        const std::vector<uint8_t> plainOut = addClamped(Y, plainE);
        const int litPlain = maxDiff(plainOut, X, 0, kFw, true), darkPlain = maxDiff(plainOut, X, kDarkFrom, kLw, true);
        std::printf("composite vs T(F + L), beam pixels, max error of 255: matched lit %d dark %d; plain lit %d dark %d\n", litMatched, darkMatched, litPlain, darkPlain);
        check(litMatched <= 3, "matched composite equals T(F + L) over the lit background");
        check(darkMatched <= 3, "matched composite equals T(F + L) over black");
        check(litPlain >= 40, "fail-before: the plain add is far from T(F + L) over the lit background");
        check(darkPlain <= 3, "the plain add agrees with T(F + L) over black");
        check(maxDiff(matchedOut, plainOut, kDarkFrom, kLw, true) <= 3, "over black the two forms agree");
        // Away from the beam the layer adds nothing: the picture is untouched.
        int offBeam = 0;
        for (uint32_t y = 0; y < kLh; ++y)
            for (uint32_t x = 0; x < kLw; ++x) {
                const Rgb l = lightAt(x, y);
                if (l.r != 0 || l.g != 0 || l.b != 0) continue;
                for (int c = 0; c < 3; ++c) offBeam = std::max(offBeam, static_cast<int>(E[(y * kLw + x) * 4 + c]));
            }
        check(offBeam <= 1, "away from the beam the layer is empty (no resample ghost of the scene)");
        check(E[3] == 255 && E[(kLw * kLh - 1) * 4 + 3] == 255, "the layer's alpha is one");
    }

    // ---- the plain sequence (the A/B key, and the matched composite's fall-back)
    f.freshLayers();
    f.runPlan(sharpTonePlan(false, true, SharpBlend::kPlain, false), f.states);
    {
        const std::vector<uint8_t> E = f.read8(f.eTex.Get());
        check(maxDiff(E, TL, 0, kLw, false) <= 1, "the plain sequence leaves T(L) in the 8-bit layer");
    }
    f.freshLayers();
    f.runPlan(sharpTonePlan(false, true, SharpBlend::kMatched, false), f.states);
    check(maxDiff(f.read8(f.eTex.Get()), TL, 0, kLw, false) <= 1, "a matched frame that is not ready is the plain sequence");

    // ---- the HUD: alone it is T(H) as it always was; with the weapons it is added to their contribution
    {
        const float junk[4] = {0.3f, 0.6f, 0.9f, 0.5f};
        f.ctx->ClearRenderTargetView(f.eRtv.Get(), junk);
        f.runPlan(sharpTonePlan(true, false, SharpBlend::kMatched, true), f.states);
        const std::vector<uint8_t> E = f.read8(f.eTex.Get());
        check(maxDiff(E, TH, 0, kLw, false) <= 1, "the HUD alone: T(H) replaces the target, as it always has");
        check(std::abs(static_cast<int>(E[3]) - static_cast<int>(std::lround(0.5f * 255.0f))) <= 1, "the HUD alone leaves the alpha to the coverage pass (colour channels only)");
        f.freshLayers();
        f.runPlan(sharpTonePlan(true, true, SharpBlend::kMatched, true), f.states);
        const std::vector<uint8_t> both = f.read8(f.eTex.Get());
        std::vector<uint8_t> want(X.size());
        for (size_t i = 0; i < X.size(); ++i) {
            const int delta = (i % 4 == 3) ? 255 : std::max(0, static_cast<int>(X[i]) - static_cast<int>(Y[i]));
            want[i] = (i % 4 == 3) ? 255 : static_cast<uint8_t>(std::min(255, delta + static_cast<int>(TH[i])));
        }
        check(maxDiff(both, want, 0, kLw, false) <= 4, "with the HUD: the weapons' difference plus T(H), both in the 8-bit layer");
        check(both[3] == 255, "the weapons-and-HUD frame starts from alpha one (the coverage pass then writes the HUD's)");
    }

    // ---- mutants of the sequence, each caught by the same comparator that passes the real thing
    const auto litError = [&](const std::vector<uint8_t>& E) { return maxDiff(addClamped(Y, E), X, 0, kFw, true); };
    {
        // 1. the source pass without its add: the weapons layer never gets the scene
        SharpPassStates st = f.states;
        D3D11_BLEND_DESC m{};
        m.IndependentBlendEnable = TRUE;
        for (int rt = 0; rt < 2; ++rt) {
            m.RenderTarget[rt].BlendEnable = FALSE;
            m.RenderTarget[rt].SrcBlend = D3D11_BLEND_ONE;
            m.RenderTarget[rt].DestBlend = D3D11_BLEND_ZERO;
            m.RenderTarget[rt].BlendOp = D3D11_BLEND_OP_ADD;
            m.RenderTarget[rt].SrcBlendAlpha = D3D11_BLEND_ONE;
            m.RenderTarget[rt].DestBlendAlpha = D3D11_BLEND_ZERO;
            m.RenderTarget[rt].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            m.RenderTarget[rt].RenderTargetWriteMask = 7;
        }
        st.sourceBlend.Reset();
        hr(f.dev->CreateBlendState(&m, &st.sourceBlend), "a mutant blend");
        f.freshLayers();
        f.runPlan(sharpTonePlan(false, true, SharpBlend::kMatched, true), st);
        const int e = litError(f.read8(f.eTex.Get()));
        check(e > 15, "mutant: a source pass that replaces W instead of adding to it is caught");
        std::printf("mutant source pass without its add: lit error %d\n", e);
        // 2. the lit step replaces instead of subtracting: the picture counts the scene twice
        SharpPassStates st2 = f.states;
        st2.toneSubtract = st2.toneReplace;
        f.freshLayers();
        f.runPlan(sharpTonePlan(false, true, SharpBlend::kMatched, true), st2);
        const int e2 = litError(f.read8(f.eTex.Get()));
        check(e2 > 15, "mutant: a lit step that replaces instead of subtracting is caught");
        std::printf("mutant lit step replacing: lit error %d\n", e2);
        // 3. the base step missing: the target holds nothing to subtract
        TonePlan p = sharpTonePlan(false, true, SharpBlend::kMatched, true);
        p.steps[0] = p.steps[1];
        p.count = 1;
        f.freshLayers();
        f.runPlan(p, f.states);
        const int e3 = litError(f.read8(f.eTex.Get()));
        check(e3 > 15, "mutant: a missing base step is caught");
        std::printf("mutant missing base step: lit error %d\n", e3);
        // 4. the lit step reading the scratch instead of the weapons layer: the difference is zero
        TonePlan q = sharpTonePlan(false, true, SharpBlend::kMatched, true);
        q.steps[1] = ToneStep::kWeaponsBase;
        f.freshLayers();
        f.runPlan(q, f.states);
        const int e4 = litError(f.read8(f.eTex.Get()));
        check(e4 > 15, "mutant: a lit step that reads the scratch is caught");
        std::printf("mutant lit step reading the scratch: lit error %d\n", e4);
        // control: the real sequence passes the very comparator the mutants fail
        f.freshLayers();
        f.runPlan(sharpTonePlan(false, true, SharpBlend::kMatched, true), f.states);
        check(litError(f.read8(f.eTex.Get())) <= 3, "control: the real sequence passes the comparator");
    }

    // ---- the debug layer saw nothing wrong
    if (f.info) {
        const UINT64 count = f.info->GetNumStoredMessages();
        unsigned bad = 0;
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(f.info->GetMessage(i, nullptr, &size)) || !size) continue;
            std::vector<uint8_t> raw(size);
            auto* msg = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
            if (FAILED(f.info->GetMessage(i, msg, &size))) continue;
            if (msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR || msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
                ++bad;
                std::fprintf(stderr, "debug layer: %s\n", msg->pDescription);
            }
        }
        check(bad == 0, "the debug layer reports no error or corruption for the production sequence");
    } else {
        std::puts("sharp_weapons_test: no debug layer on this machine (the sequence ran without it)");
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("sharp_weapons_test: dry-run (no device, no files)");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::fprintf(stderr, "usage: --self-test | --dry-run\n");
        return 2;
    }
    try {
        ruleTests();
        keyTests();
        planTests();
        censusTests("tools/sharp_weapons_test/fixtures/census_070029_c1_f0.txt", 2, 0);
        censusTests("tools/sharp_weapons_test/fixtures/census_070029_c3_f0.txt", 4, 2);
        censusTests("tools/sharp_weapons_test/fixtures/census_070029_c4_f0.txt", 4, 0);
        wiringTests();
        flatTests();
        menuTests();
        dropTests();
        gpuTests();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sharp_weapons_test: FAIL: %s (after %u checks)\n", e.what(), g_checks);
        return 1;
    }
    std::printf("sharp_weapons_test: %u checks passed.\n", g_checks);
    return 0;
}
