// The VR world route's camera injection mode (src/d3d11/flat_camera_vr.h, and the detour in src/d3d11/flat_camera_inject.cpp that
// runs it; design doc section 82, stage 2). Nothing here needs the game: what the detour DECIDES is a pure header this rig runs,
// and what it WRITES is held by source pins and by the flat path's own text, hashed.
//
//   R1   the role of a kind-3 camera: the 4% tolerance edges, the 1.5x near ratio edges, the 0.92 field-of-view ratio edge (the
//        stage 2 experiment build), non-finite and zero, and the anchors (the near plane's, kept; the field of view's, per frame)
//   R2   the admission matrix: every combination of kind 0..7, unreadable, gate verdict, pass-through / observe / inject, window,
//        phase and role, against a table written from the brief -- and the rule that Inject needs all of them
//   R3   the injected-kind invariant, exhaustively: no input combination increments injectedKind[k] for k != 3
//   R4   the counters: every owner-thread call reaches exactly one outcome, over random call sequences; off-thread is lock-free
//   R5   THE CAMERA-OBJECT REUSE SCENARIO as a scripted call sequence: one camera pointer is kind 5, then kind 3 scene and
//        first-person (injected), kind 3 auxiliary (excluded), then kind 5 again; admission is per call, never per pointer, and the
//        flush fires exactly once when the injected object is next seen un-injected
//   R6   the flush decision for every reason
//   R7   the excluded-signature table: first eight distinct, tolerance, lock-free publication under a reader
//   R8   the per-frame mode word, the census's frame step, and quiet
//   R9   the injection arithmetic (bit-equal to the flat injector's), the frame-window lapse, the stand-down window
//   R10  the flat profile: flatCameraAdmit and the flush table answer exactly what they answered, and their text is hashed
//   R11  source pins on the detour: the flat path's regions are byte-for-byte (hashed), the VR branch's place, the order of the
//        call's steps, no write reachable from a pass-through or observe-only call, no call-path I/O, one outcome per call
//   R12  the weapon's role from the struct as flight 2 logged it (the stage 2 experiment build): flight 2's frame of 78 world calls,
//        the first-person calls it credits, the field-of-view range the counters keep, and the frames that must stay all-scene
//
// Every check prints "  ok    <label>: <what>" or "  FAIL  <label>: <what>"; --self-test [root] runs them all (root: the repo root,
// default "."; tools\flat_camera_vr_test\mutants.py points it at a scratch copy with one rule flipped) and prints
// "flat camera vr: PASS" only when every check holds.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../src/d3d11/flat_camera_vr.h"

namespace {

using namespace edvr;
using Role = FlatCameraVrRole;
using Admit = FlatCameraVrAdmit;
using Mode = FlatCameraVrMode;
using Gate = FlatCameraGateVerdict;

int g_failures = 0;
std::string g_root = ".";

void check(bool ok, const char* label, const char* what) {
    if (ok) { std::printf("  ok    %s: %s\n", label, what); return; }
    ++g_failures;
    std::printf("  FAIL  %s: %s\n", label, what);
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
const float kS = 5040.0f / 2835.0f;   // the world's panel aspect, 1.7778

// A deterministic generator: the same sequence on every machine.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 16); }
    bool chance(uint32_t percent) { return next() % 100u < percent; }
    uint32_t below(uint32_t n) { return next() % n; }
};

// ---------------------------------------------------------------------------
// R1: the role.
// ---------------------------------------------------------------------------
void testRole() {
    std::printf("role\n");
    auto role = [](float aspect, float nearZ, float screen, float sceneNear) { return flatCameraVrRole(aspect, nearZ, screen, sceneNear); };
    check(role(kS, 0.025f, kS, 0.025f) == Role::Scene && role(kS * 1.039f, 0.025f, kS, 0.025f) == Role::Scene &&
          role(kS * 0.961f, 0.025f, kS, 0.025f) == Role::Scene,
          "R1a", "an aspect within 3.9% of the screen's is a screen view (scene role)");
    check(role(kS * 1.041f, 0.025f, kS, 0.025f) == Role::Auxiliary && role(kS * 0.959f, 0.025f, kS, 0.025f) == Role::Auxiliary,
          "R1b", "an aspect 4.1% off the screen's is auxiliary, wider or narrower");
    bool sweep = true;
    for (int tenth = -80; tenth <= 80; ++tenth) {   // -8.0% .. +8.0% in 0.1% steps; the exact 4.0% step is left to float rounding
        if (tenth == 40 || tenth == -40) continue;
        const float deviation = static_cast<float>(tenth) / 1000.0f;
        const bool screenView = std::fabs(deviation) < 0.04f;
        const Role got = role(kS * (1.0f + deviation), 0.025f, kS, 0.025f);
        if ((got != Role::Auxiliary) != screenView) sweep = false;
    }
    check(sweep, "R1c", "a sweep of the aspect from -8% to +8% in 0.1% steps: screen views exactly inside 4%, both sides");
    // The anchor and the weapon: sceneNear 1.0, near ratios around 1.5 (exactly representable, so the edge is exact).
    check(role(kS, 1.5f, kS, 1.0f) == Role::FirstPerson, "R1d", "a near plane exactly 1.5x the scene's is the first-person camera (>=, not >)");
    check(role(kS, 1.4999f, kS, 1.0f) == Role::Scene && role(kS, 1.0f, kS, 1.0f) == Role::Scene && role(kS, 0.5f, kS, 1.0f) == Role::Scene,
          "R1e", "a near plane just under 1.5x, equal to, or under the scene's is the scene camera");
    check(role(kS, 1.5001f, kS, 1.0f) == Role::FirstPerson && role(kS, 3.0f, kS, 1.0f) == Role::FirstPerson,
          "R1f", "a near plane just over 1.5x, or well over, is the first-person camera");
    check(role(kS, 0.0675f, kS, 0.025f) == Role::FirstPerson && role(kS, 0.025f, kS, 0.025f) == Role::Scene &&
          role(kS, 0.0374f, kS, 0.025f) == Role::Scene && role(kS, 0.0376f, kS, 0.025f) == Role::FirstPerson,
          "R1g", "the census's numbers: near 0.0675 against a scene near of 0.025 is the weapon; 0.0374 and 0.0376 straddle 1.5x");
    check(role(kS, 0.0675f, kS, 0.0f) == Role::Scene && role(kS, 0.0675f, kS, -1.0f) == Role::Scene && role(kS, 0.0675f, kS, kNaN) == Role::Scene &&
          role(kS, 0.0675f, kS, kInf) == Role::Scene && role(kS, 0.0675f, kS, -kInf) == Role::Scene,
          "R1h", "no anchor (zero, negative, NaN, infinite scene near): a screen view is a scene call, never a guess at the weapon");
    check(role(kS, kNaN, kS, 0.025f) == Role::Scene && role(kS, -1.0f, kS, 0.025f) == Role::Scene && role(kS, 0.0f, kS, 0.025f) == Role::Scene,
          "R1i", "a near plane that is NaN, negative or zero is a scene call");
    check(role(kNaN, 0.025f, kS, 0.025f) == Role::Auxiliary && role(kInf, 0.025f, kS, 0.025f) == Role::Auxiliary &&
          role(-kInf, 0.025f, kS, 0.025f) == Role::Auxiliary && role(0.0f, 0.025f, kS, 0.025f) == Role::Auxiliary &&
          role(-kS, 0.025f, kS, 0.025f) == Role::Auxiliary,
          "R1j", "an aspect that is NaN, infinite, zero or negative is auxiliary");
    check(role(kS, 0.025f, 0.0f, 0.025f) == Role::Auxiliary && role(kS, 0.025f, -1.0f, 0.025f) == Role::Auxiliary &&
          role(kS, 0.025f, kNaN, 0.025f) == Role::Auxiliary && role(kS, 0.025f, kInf, 0.025f) == Role::Auxiliary &&
          role(0.0f, 0.025f, 0.0f, 0.025f) == Role::Auxiliary,
          "R1k", "a screen aspect that is not positive and finite makes every camera auxiliary (even one that matches it)");
    check(role(1.0f, 0.1f, kS, 0.025f) == Role::Auxiliary && role(1.03441f, 0.025f, kS, 0.025f) == Role::Auxiliary &&
          role(0.5f * kS, 0.025f, kS, 0.025f) == Role::Auxiliary && role(2.0f * kS, 0.025f, kS, 0.025f) == Role::Auxiliary,
          "R1l", "the census's odd cameras (a square 90-degree one, an eye-shaped 1.03441) and halves and doubles of the aspect are auxiliary");
    check(role(2.07f, 0.025f, 2.0f, 0.025f) == Role::Scene && role(2.09f, 0.025f, 2.0f, 0.025f) == Role::Auxiliary &&
          role(1.039f, 0.025f, 1.0f, 0.025f) == Role::Scene && role(1.041f, 0.025f, 1.0f, 0.025f) == Role::Auxiliary,
          "R1m", "the tolerance is relative to the route's screen aspect, whatever it is (2.0 and 1.0 here)");
    check(std::strcmp(flatCameraVrRoleName(Role::Scene), "scene") == 0 && std::strcmp(flatCameraVrRoleName(Role::FirstPerson), "first-person") == 0 &&
          std::strcmp(flatCameraVrRoleName(Role::Auxiliary), "auxiliary") == 0 &&
          static_cast<int>(Role::Scene) == 0 && static_cast<int>(Role::FirstPerson) == 1 && static_cast<int>(Role::Auxiliary) == 2,
          "R1n", "the roles have the numbers the observer's role field documents (0 scene, 1 first-person, 2 auxiliary) and their names");

    // THE FIELD-OF-VIEW TEST (the stage 2 experiment build). Flight 2 logged the struct of the weapon's calls with the SAME near plane as the
    // scene's (0.025: the 0.0675 is in the composed rows) and a different field of view (0.8203 against about 0.9831 rad), so the near test
    // never fired and every first-person call was counted as a scene call. The weapon is the call whose fov is at most 0.92 x the frame's
    // scene fov. 25.0 x 0.92 = 23.0 exactly in doubles, so the edge is exact there.
    auto roleF = [](float fov, float sceneFov, float nearZ = 0.025f, float sceneNear = 0.025f) {
        return flatCameraVrRole(kS, nearZ, kS, sceneNear, fov, sceneFov);
    };
    check(roleF(0.8203f, 0.9831f) == Role::FirstPerson && roleF(0.9831f, 0.9831f) == Role::Scene,
          "R1t", "flight 2's struct: the weapon's fov 0.8203 against the scene's 0.9831 is the first-person camera although the near plane is equal (0.025); the scene's own fov is a scene call");
    check(roleF(23.0f, 25.0f) == Role::FirstPerson && roleF(23.001f, 25.0f) == Role::Scene && roleF(22.999f, 25.0f) == Role::FirstPerson,
          "R1u", "the edge is 0.92 of the scene's fov, inclusive: 23.0 against 25.0 is the weapon (<=, not <), 23.001 is not");
    check(roleF(0.9f, 1.0f) == Role::FirstPerson && roleF(0.95f, 1.0f) == Role::Scene && roleF(0.5f, 1.0f) == Role::FirstPerson,
          "R1v", "a fov 0.90 of the scene's is the weapon, 0.95 is not (the ratio is neither 0.85 nor 0.97), and a much tighter one is");
    check(roleF(0.8203f, 0.0f) == Role::Scene && roleF(0.8203f, -1.0f) == Role::Scene && roleF(0.8203f, kNaN) == Role::Scene &&
          roleF(0.8203f, kInf) == Role::Scene && roleF(0.8203f, -kInf) == Role::Scene,
          "R1w", "no scene field of view yet (zero, negative, NaN, infinite): a screen view is a scene call, never a guess at the weapon");
    check(roleF(kNaN, 0.9831f) == Role::Scene && roleF(0.0f, 0.9831f) == Role::Scene && roleF(-0.5f, 0.9831f) == Role::Scene &&
          roleF(kInf, 0.9831f) == Role::Scene && roleF(-kInf, 0.9831f) == Role::Scene,
          "R1x", "a field of view that is NaN, zero, negative or infinite is a scene call");
    check(roleF(1.2f, 1.0f) == Role::Scene && roleF(1.0f, 1.0f) == Role::Scene,
          "R1y", "a field of view as wide as the scene's or wider is a scene call (only a TIGHTER camera is the weapon)");
    check(flatCameraVrRole(kS, 0.0675f, kS, 0.025f, 0.9831f, 0.9831f) == Role::FirstPerson && flatCameraVrRole(kS, 0.0675f, kS, 0.025f) == Role::FirstPerson &&
          flatCameraVrRole(1.0f, 0.025f, kS, 0.025f, 0.5f, 1.0f) == Role::Auxiliary,
          "R1z", "the near test still holds with or without a field of view, and the aspect test comes first: a square camera is auxiliary whatever its fov");
    {   // The fov anchor: the frame's own, following Scene calls only, reset by beginFrame; the near anchor is kept.
        FlatCameraVrRoleTracker t;
        const Role weaponFirst = t.classify(kS, 0.025f, kS, 0.8203f);   // before any scene call: no anchor, a scene call, and it sets the anchor
        const float anchorAfterWeapon = t.sceneFov();
        const Role scene = t.classify(kS, 0.025f, kS, 0.9831f);          // the widest so far: raises the anchor
        const float anchorAfterScene = t.sceneFov();
        const Role weapon = t.classify(kS, 0.025f, kS, 0.8203f);         // now recognised
        check(weaponFirst == Role::Scene && anchorAfterWeapon == 0.8203f && scene == Role::Scene && anchorAfterScene == 0.9831f &&
                  weapon == Role::FirstPerson && t.sceneFov() == 0.9831f,
              "R1aa", "the fov anchor is the widest Scene call of the frame: a weapon call that arrives first is taken for a scene call, the scene call raises the anchor, every later weapon call is right, and a first-person call never raises it");
        t.classify(1.0f, 0.025f, kS, 2.0f);   // auxiliary: no update
        t.classify(kS, 0.025f, kS, kNaN);     // not a number: no update
        t.classify(kS, 0.025f, kS, 0.0f);     // zero: no update
        t.classify(kS, 0.025f, kS, -1.0f);    // negative: no update
        t.classify(kS, 0.025f, kS, kInf);     // infinite: no update
        t.classify(kS, 0.025f, kS, 0.7f);     // tighter than the anchor (a first-person call): never moves it
        check(t.sceneFov() == 0.9831f, "R1ab", "an auxiliary camera, a fov that is NaN, zero, negative or infinite, and a first-person call never move the fov anchor");
        t.beginFrame();
        check(t.sceneFov() == 0.0f && t.sceneNear() == 0.025f, "R1ac", "beginFrame clears the fov anchor and keeps the near anchor");
        t.classify(kS, 0.025f, kS, 0.6f);     // a scene camera that zoomed: the new frame's own anchor
        check(t.classify(kS, 0.025f, kS, 0.6f) == Role::Scene && t.sceneFov() == 0.6f,
              "R1ad", "a zoomed scene camera is a scene call in its own frame: the anchor is not last frame's wider one");
        t.reset();
        check(t.sceneFov() == 0.0f && t.sceneNear() == 0.0f, "R1ae", "reset clears both anchors");
    }
    // The anchor: the smallest near among screen views, kept across calls.
    {
        FlatCameraVrRoleTracker t;
        const Role first = t.classify(kS, 0.0675f, kS);     // the weapon before any scene call: no anchor, a scene call
        const float anchorAfterWeapon = t.sceneNear();
        const Role scene = t.classify(kS, 0.025f, kS);
        const float anchorAfterScene = t.sceneNear();
        const Role weapon = t.classify(kS, 0.0675f, kS);    // now classified
        check(first == Role::Scene && anchorAfterWeapon == 0.0675f && scene == Role::Scene && anchorAfterScene == 0.025f && weapon == Role::FirstPerson && t.sceneNear() == 0.025f,
              "R1o", "the anchor is the smallest near among screen views: a weapon call that arrives first is taken for a scene call, the scene call lowers the anchor, every later call is right");
        t.classify(1.0f, 0.001f, kS);                        // auxiliary: no update
        t.classify(kS, kNaN, kS);                            // not a number: no update
        t.classify(kS, 0.0f, kS);                            // zero: no update
        t.classify(kS, -0.5f, kS);                           // negative: no update
        t.classify(kS, kInf, kS);                            // infinite: no update
        check(t.sceneNear() == 0.025f, "R1p", "an auxiliary camera, and a near plane that is NaN, zero, negative or infinite, never move the anchor");
        t.classify(kS, 0.02f, kS);
        check(t.sceneNear() == 0.02f && t.classify(kS, 0.025f, kS) == Role::Scene, "R1q", "a smaller near plane moves the anchor down and never up");
        t.reset();
        check(t.sceneNear() == 0.0f, "R1r", "reset clears the anchor");
    }
    // The core keeps the anchor across frames (the first call of a frame is already classified).
    {
        FlatCameraVrCore core;
        FlatCameraInjectedSet set;
        FlatCameraVrFrame f; f.inject = true; f.phaseX = 0.3f; f.phaseY = 0.2f; f.renderW = 5040; f.renderH = 2835; f.screenAspect = kS;
        core.beginFrame(f, 1000);
        FlatCameraVrCallIn in; in.camera = 0x1000; in.readable = true; in.kind = 3; in.gate = Gate::Admit; in.mode = Mode::Inject;
        FlatCameraVrFrustum scene; scene.aspect = kS; scene.fov = 1.0f; scene.nearZ = 0.025f; scene.farZ = 50000.0f;
        core.plan(in, scene, set);
        core.beginFrame(f, 1100);
        FlatCameraVrFrustum weapon = scene; weapon.nearZ = 0.0675f; weapon.fov = 0.8f;
        const FlatCameraVrPlan p = core.plan(in, weapon, set);
        check(core.sceneNear() == 0.025f && p.role == Role::FirstPerson && p.admit == Admit::Inject,
              "R1s", "the anchor survives the frame step: the first call of the next frame (a weapon call) is already a first-person call");
    }
}

// ---------------------------------------------------------------------------
// R2: the admission matrix, against a table written from the brief.
// ---------------------------------------------------------------------------
Admit oracleAdmit(const FlatCameraVrAdmitInput& in) {
    if (in.gate == Gate::OffThread) return Admit::OffThread;
    if (!in.readable) return Admit::Unreadable;
    if (in.kind == 4 || in.kind == 5) return Admit::Unsupported;
    if (in.kind != 3) return Admit::OtherKind;
    if (in.mode == Mode::Observe) return Admit::Observed;
    if (in.gate != Gate::Admit) return Admit::Stale;
    if (in.mode == Mode::PassThrough) return Admit::NotActive;
    if (in.role == Role::Auxiliary) return Admit::RoleExcluded;
    if (!in.windowOpen) return Admit::AfterTrigger;
    if (!in.phaseNonzero) return Admit::Warming;
    return Admit::Inject;
}

const uint32_t kKinds[] = {0, 1, 2, 3, 4, 5, 6, 7, 100, 0xFFFFFFFFu};
const Gate kGates[] = {Gate::Admit, Gate::Disarmed, Gate::Expired, Gate::OffThread};
const Mode kModes[] = {Mode::PassThrough, Mode::Observe, Mode::Inject};
const Role kRoles[] = {Role::Scene, Role::FirstPerson, Role::Auxiliary};

void testAdmission() {
    std::printf("admission\n");
    unsigned combos = 0, injects = 0, mismatches = 0, injectViolations = 0, wantsBad = 0, roleLeaks = 0;
    for (int readable = 0; readable < 2; ++readable)
        for (uint32_t kind : kKinds)
            for (Gate gate : kGates)
                for (Mode mode : kModes)
                    for (int window = 0; window < 2; ++window)
                        for (int phase = 0; phase < 2; ++phase)
                            for (Role role : kRoles) {
                                FlatCameraVrAdmitInput in;
                                in.readable = readable != 0; in.kind = kind; in.gate = gate; in.mode = mode;
                                in.windowOpen = window != 0; in.phaseNonzero = phase != 0; in.role = role;
                                const Admit got = flatCameraVrAdmit(in);
                                ++combos;
                                if (got != oracleAdmit(in)) ++mismatches;
                                if (got == Admit::Inject) {
                                    ++injects;
                                    if (!(readable && kind == 3 && gate == Gate::Admit && mode == Mode::Inject && window && phase && role != Role::Auxiliary))
                                        ++injectViolations;
                                }
                                const bool wants = readable && kind == 3 && gate == Gate::Admit && mode == Mode::Inject;
                                if (flatCameraVrWantsRole(in) != wants) ++wantsBad;
                                if (!wants)
                                    for (Role other : kRoles) {
                                        FlatCameraVrAdmitInput again = in;
                                        again.role = other;
                                        if (flatCameraVrAdmit(again) != got) ++roleLeaks;
                                    }
                            }
    std::printf("  note  %u input combinations, %u of them inject\n", combos, injects);
    check(mismatches == 0, "R2a", "every combination answers what the table says: kind and readability first (off-thread, unreadable, kinds 4 and 5 unsupported, other kinds), then observe, the frame window, pass-through, role, trigger window, phase");
    check(injectViolations == 0 && injects == 2, "R2b", "a call is injected only for a readable kind-3 camera, on the owner thread, in an open frame window, in an injecting frame, before the trigger, with a phase, in a scene or first-person role (two combinations)");
    check(wantsBad == 0 && roleLeaks == 0, "R2c", "the role is asked for exactly when the call could be injected (kind 3, owner thread, open window, injecting frame); without it the answer cannot depend on the role");
    {
        FlatCameraVrAdmitInput in; in.readable = true; in.gate = Gate::Admit; in.mode = Mode::Inject; in.windowOpen = true; in.phaseNonzero = true; in.role = Role::Scene;
        bool eyesNeverInjected = true, othersNamed = true;
        for (uint32_t kind : kKinds) {
            in.kind = kind;
            const Admit a = flatCameraVrAdmit(in);
            if (kind != 3 && a == Admit::Inject) eyesNeverInjected = false;
            if ((kind == 4 || kind == 5) != (a == Admit::Unsupported)) othersNamed = false;
            if (kind != 3 && kind != 4 && kind != 5 && a != Admit::OtherKind) othersNamed = false;
        }
        check(eyesNeverInjected && othersNamed, "R2d", "in a fully open injecting frame only kind 3 is admitted: kinds 4 and 5 (the eyes) answer Unsupported, every other kind OtherKind");
    }
    {
        const Admit all[] = {Admit::Inject, Admit::Warming, Admit::AfterTrigger, Admit::RoleExcluded, Admit::NotActive, Admit::Observed,
                             Admit::Stale, Admit::OffThread, Admit::Unsupported, Admit::OtherKind, Admit::Unreadable};
        bool named = true;
        for (size_t i = 0; i < 11; ++i) {
            const char* a = flatCameraVrAdmitName(all[i]);
            if (!a || !a[0] || a[0] == '?') named = false;
            for (size_t j = i + 1; j < 11; ++j) if (std::strcmp(a, flatCameraVrAdmitName(all[j])) == 0) named = false;
        }
        check(named, "R2e", "the eleven reasons have eleven distinct names");
    }
    // The translation is the flat table's own: the VR answer for a kind, a window and an observe switch is flatCameraAdmit's.
    {
        FlatCameraVrAdmitInput in; in.readable = true; in.kind = 3; in.gate = Gate::Admit; in.mode = Mode::Inject; in.windowOpen = true; in.phaseNonzero = true; in.role = Role::Scene;
        const FlatCameraAdmitInput base = flatCameraVrBaseInput(in, in.phaseNonzero);
        check(base.readable && base.kind == 3 && base.gate == Gate::Admit && base.upstreamOwns && base.phaseNonzero && !base.observeOnly &&
              flatCameraAdmit(base) == FlatCameraAdmit::Inject, "R2f", "an injecting frame maps onto the flat table as the Upstream owner with the frame's phase, and the flat table says Inject");
        in.mode = Mode::Observe;
        const FlatCameraAdmitInput obs = flatCameraVrBaseInput(in, in.phaseNonzero);
        in.mode = Mode::PassThrough;
        const FlatCameraAdmitInput pass = flatCameraVrBaseInput(in, in.phaseNonzero);
        check(obs.observeOnly && !obs.upstreamOwns && flatCameraAdmit(obs) == FlatCameraAdmit::Observed &&
              !pass.observeOnly && !pass.upstreamOwns && flatCameraAdmit(pass) == FlatCameraAdmit::NotUpstream,
              "R2g", "an observe frame is the flat table's observe-only switch; a pass-through frame is 'no route owns it'");
    }
    {   // An input nobody gave a role to is an exclusion, never an injection: an unknown role is not guessed.
        FlatCameraVrAdmitInput in;
        in.readable = true; in.kind = 3; in.gate = Gate::Admit; in.mode = Mode::Inject; in.windowOpen = true; in.phaseNonzero = true;
        const FlatCameraVrAdmitInput blank;
        check(in.role == Role::Auxiliary && flatCameraVrAdmit(in) == Admit::RoleExcluded && blank.role == Role::Auxiliary && blank.mode == Mode::PassThrough &&
              !blank.windowOpen && !blank.phaseNonzero && !blank.readable,
              "R2h", "an input with no role set is auxiliary (excluded), and a blank input is a pass-through of nothing: the defaults are the safe answers");
    }
}

// ---------------------------------------------------------------------------
// The harness: one camera call, in the order the detour runs it (refreshPreVr): the planner, the flush, the injection, the outcome.
// ---------------------------------------------------------------------------
struct Sim {
    FlatCameraVrCore core;
    FlatCameraInjectedSet set;
    uint32_t bits = 0;
    uint64_t now = 1000;
    uint32_t flushWrites = 0;
    struct Out { FlatCameraVrPlan plan; bool frustumRead = false; bool landed = false; bool flushed = false; };

    void frame(bool inject, bool observe, float px = 0.0f, float py = 0.0f, uint32_t w = 5040, uint32_t h = 2835) {
        FlatCameraVrFrame f;
        f.inject = inject; f.observe = observe; f.phaseX = px; f.phaseY = py; f.renderW = w; f.renderH = h;
        f.screenAspect = static_cast<float>(w) / static_cast<float>(h);
        core.beginFrame(f, now += 16);
        bits = flatCameraVrBitsForFrame(inject, observe, true);
    }
    void censusStep() { bits = flatCameraVrBitsAfterCensusFrame(bits); }
    Out call(uintptr_t camera, bool readable, uint32_t kind, const FlatCameraVrFrustum& frustum, Gate gate = Gate::Admit,
             uint64_t caller = 0, bool phaseWritesLand = true) {
        Out out;
        if (gate == Gate::OffThread) { core.tally().noteOffThread(); out.plan.admit = Admit::OffThread; return out; }
        FlatCameraVrCallIn in;
        in.camera = camera; in.readable = readable; in.kind = kind; in.gate = gate; in.mode = flatCameraVrModeOfBits(bits); in.callerRva = caller;
        FlatCameraVrFrustum read;
        out.frustumRead = core.wantsFrustum(in);
        if (out.frustumRead) read = frustum;
        out.plan = core.plan(in, read, set);
        if (out.plan.flush) { ++flushWrites; core.tally().noteFlushed(); out.flushed = true; }
        out.landed = out.plan.inject && phaseWritesLand;
        if (out.landed) set.noteInjected(camera);
        core.finish(out.plan, in, out.landed);
        return out;
    }
};
FlatCameraVrFrustum sceneCam() { FlatCameraVrFrustum f; f.aspect = kS; f.fov = 1.0122f; f.nearZ = 0.025f; f.farZ = 50000.0f; return f; }
FlatCameraVrFrustum weaponCam() { FlatCameraVrFrustum f; f.aspect = kS; f.fov = 0.8229f; f.nearZ = 0.0675f; f.farZ = 50000.0f; return f; }
FlatCameraVrFrustum squareCam() { FlatCameraVrFrustum f; f.aspect = 1.0f; f.fov = 1.5708f; f.nearZ = 0.1f; f.farZ = 1000.0f; return f; }
FlatCameraVrFrustum zoomCam() { FlatCameraVrFrustum f; f.aspect = 1.4f; f.fov = 0.236f; f.nearZ = 0.3f; f.farZ = 9000.0f; return f; }
FlatCameraVrFrustum eyeCam() { FlatCameraVrFrustum f; f.aspect = 1.03441f; f.fov = 1.59971f; f.nearZ = 0.025f; f.farZ = 50000.0f; return f; }

// ---------------------------------------------------------------------------
// R12: the weapon's role from the struct as flight 2 logged it (the stage 2 experiment build): the weapon's calls carry the scene's near
// plane (0.025) and a tighter field of view (0.8203 against 0.9831 rad), so the role is the call's fov against the SAME FRAME's scene
// camera. Flight 2's frame, from the census (draws 1201..1465 of frame 2): the world camera's 78 calls are, in order, 3 weapon calls,
// 15 scene, 3 weapon, 12 scene, 3 weapon, 24 scene, 3 weapon, 12 scene, 3 weapon (15 weapon calls in five groups, 63 scene calls).
// ---------------------------------------------------------------------------
void testWeaponRole() {
    std::printf("weapon role\n");
    const auto sceneS = [] { FlatCameraVrFrustum f; f.aspect = kS; f.fov = 0.9831f; f.nearZ = 0.025f; f.farZ = 50000.0f; return f; };
    const auto weaponS = [] { FlatCameraVrFrustum f; f.aspect = kS; f.fov = 0.8203f; f.nearZ = 0.025f; f.farZ = 50000.0f; return f; };
    const uintptr_t world = 0x28074A12250;
    const int groups[] = {3, 15, 3, 12, 3, 24, 3, 12, 3};
    auto feed = [&](Sim& sim, bool refuseWeapon, const FlatCameraVrFrustum& wf, const FlatCameraVrFrustum& sf) {
        std::vector<Role> roles;
        bool weapon = true;
        for (int count : groups) {
            for (int i = 0; i < count; ++i)
                roles.push_back(sim.call(world, true, 3, weapon ? wf : sf, Gate::Admit, 0x594E13, !(refuseWeapon && weapon)).plan.role);
            weapon = !weapon;
        }
        return roles;
    };
    auto countRole = [](const std::vector<Role>& roles, Role r) { return static_cast<unsigned>(std::count(roles.begin(), roles.end(), r)); };
    {
        Sim sim;
        sim.frame(true, false, 0.25f, -0.125f);
        const std::vector<Role> roles = feed(sim, false, weaponS(), sceneS());
        const FlatCameraVrCounters c = sim.core.tally().snapshot();
        check(roles.size() == 78 && countRole(roles, Role::FirstPerson) == 12 && countRole(roles, Role::Scene) == 66 &&
                  roles[0] == Role::Scene && roles[1] == Role::Scene && roles[2] == Role::Scene && roles[18] == Role::FirstPerson,
              "R12a", "flight 2's frame (78 world calls, the weapon's three at the start): 12 of the 15 weapon calls are first-person, the first group arrives before any scene call and is counted as scene");
        check(c.calls == 78 && c.firstPersonInjected == 12 && c.sceneInjected == 66 && c.firstPersonRefused == 0 && c.sceneRefused == 0 &&
                  flatCameraVrOutcomeSum(c) == c.calls,
              "R12b", "the injector's counters read inj-fp 12 and inj-scene 66, nothing refused, and still one outcome per call: the route's 'first-person injected, none refused' holds");
        check(c.fovNarrowest == 0.8203f && c.fovWidest == 0.9831f,
              "R12c", "the frame's fov range is the struct's own: 0.8203 (the weapon) to 0.9831 (the scene)");
        sim.frame(true, false, -0.375f, 0.0625f);   // the next frame: the same sequence
        const std::vector<Role> again = feed(sim, false, weaponS(), sceneS());
        check(countRole(again, Role::FirstPerson) == 12 && again[0] == Role::Scene && sim.core.tally().snapshot().firstPersonInjected == 12,
              "R12d", "the fov anchor is not carried across frames: the next frame's first weapon group is a scene call again (12 first-person, not 15) and the counters were reset");
    }
    {   // A zoomed scene camera: every call is the scene's, in a frame of its own.
        Sim sim;
        sim.frame(true, false, 0.25f, -0.125f);
        feed(sim, false, weaponS(), sceneS());
        sim.frame(true, false, 0.125f, 0.25f);
        FlatCameraVrFrustum zoomed = sceneS(); zoomed.fov = 0.6f;
        std::vector<Role> roles;
        for (int i = 0; i < 63; ++i) roles.push_back(sim.call(world, true, 3, zoomed, Gate::Admit, 0x594EAB).plan.role);
        const FlatCameraVrCounters c = sim.core.tally().snapshot();
        check(countRole(roles, Role::FirstPerson) == 0 && c.sceneInjected == 63 && c.firstPersonInjected == 0 && c.fovNarrowest == 0.6f && c.fovWidest == 0.6f,
              "R12e", "a frame in which the scene camera zoomed (fov 0.6 against last frame's 0.9831) is all scene calls: nothing is first-person against last frame's wider anchor");
    }
    {   // Warming (a zero phase) reads the frustum too; an auxiliary camera, an eye and an unreadable camera never touch the fov range.
        Sim sim;
        sim.frame(true, false, 0.0f, 0.0f);
        feed(sim, false, weaponS(), sceneS());
        sim.call(0x5000, true, 3, squareCam());            // auxiliary: aspect 1.0, fov 1.5708
        sim.call(0x5001, true, 5, eyeCam());               // an eye: never read
        sim.call(0x5002, false, 3, sceneS());              // unreadable kind
        const FlatCameraVrCounters c = sim.core.tally().snapshot();
        check(c.warming == 78 && c.auxiliary == 1 && c.fovNarrowest == 0.8203f && c.fovWidest == 0.9831f,
              "R12f", "a zero-phase (warming) frame reads the same fov range, and an auxiliary camera (fov 1.5708), an eye and an unreadable camera do not widen it");
        Sim idle;
        idle.frame(true, false, 0.25f, -0.125f);
        idle.call(0x5001, true, 5, eyeCam());
        idle.call(0x5003, true, 1, sceneS());
        const FlatCameraVrCounters e = idle.core.tally().snapshot();
        check(std::isnan(e.fovNarrowest) && std::isnan(e.fovWidest), "R12g", "a frame whose calls read no screen view's frustum has no fov range (NaN, NaN)");
    }
    {   // A weapon call whose write failed is a refused FIRST-PERSON call once it is recognised: the route declines the frame, as for any refusal.
        Sim sim;
        sim.frame(true, false, 0.25f, -0.125f);
        feed(sim, true, weaponS(), sceneS());
        const FlatCameraVrCounters c = sim.core.tally().snapshot();
        check(c.firstPersonRefused == 12 && c.sceneRefused == 3 && c.firstPersonInjected == 0 && c.sceneInjected == 63 && flatCameraVrOutcomeSum(c) == c.calls,
              "R12h", "when the weapon's writes fail, 12 calls are refused first-person and the 3 that were counted as scene are refused scene: every refusal reaches a counter the route's decline test reads");
    }
    {   // The struct where the field of view is the same for both (the weapon indistinguishable): nothing is guessed, inj-fp stays 0.
        Sim sim;
        sim.frame(true, false, 0.25f, -0.125f);
        const std::vector<Role> roles = feed(sim, false, sceneS(), sceneS());
        const FlatCameraVrCounters c = sim.core.tally().snapshot();
        check(countRole(roles, Role::FirstPerson) == 0 && c.firstPersonInjected == 0 && c.sceneInjected == 78 && c.fovNarrowest == c.fovWidest,
              "R12i", "when the struct carries one field of view the weapon cannot be told from the scene and no call is called first-person (the route's mode stays 2, as in flight 2), and the fov range says why (narrowest == widest)");
    }
}

// ---------------------------------------------------------------------------
// R3: the injected-kind invariant.
// ---------------------------------------------------------------------------
void testInjectedKind() {
    std::printf("injected kind\n");
    unsigned combos = 0, offKind = 0, sumBad = 0, forced = 0, directBad = 0, planBad = 0;
    const FlatCameraVrFrustum frusta[] = {sceneCam(), weaponCam(), squareCam(), FlatCameraVrFrustum{}};
    for (int readable = 0; readable < 2; ++readable)
        for (uint32_t kind : kKinds)
            for (Gate gate : kGates)
                for (int mode = 0; mode < 3; ++mode)
                    for (int phase = 0; phase < 2; ++phase)
                        for (int close = 0; close < 2; ++close)
                            for (const FlatCameraVrFrustum& fr : frusta)
                                for (int pass = 0; pass < 2; ++pass) {
                                    Sim sim;
                                    sim.frame(mode == 2, mode == 1, phase ? 0.31f : 0.0f, phase ? -0.27f : 0.0f);
                                    if (close) sim.core.closeWindow();
                                    FlatCameraVrCallIn in;
                                    in.camera = 0x4000; in.readable = readable != 0; in.kind = kind; in.gate = gate;
                                    in.mode = flatCameraVrModeOfBits(sim.bits);
                                    if (gate == Gate::OffThread) { sim.core.tally().noteOffThread(); continue; }
                                    const bool read = sim.core.wantsFrustum(in);
                                    const FlatCameraVrPlan plan = sim.core.plan(in, read ? fr : FlatCameraVrFrustum{}, sim.set);
                                    if (plan.inject != (plan.admit == Admit::Inject) || plan.flush) ++planBad;   // the set was empty: nothing to flush
                                    // pass 0: the writes land exactly when the plan injects; pass 1: the tally is told `landed` for EVERY call, even one
                                    // that was not injected (a bug upstream must not be able to move injectedKind).
                                    sim.core.finish(plan, in, pass == 0 ? plan.inject : true);
                                    const FlatCameraVrCounters c = sim.core.tally().snapshot();
                                    ++combos;
                                    if (pass == 1) ++forced;
                                    for (int k = 0; k < 8; ++k) if (k != 3 && c.injectedKind[k] != 0) ++offKind;
                                    const uint32_t injectedTotal = c.sceneInjected + c.firstPersonInjected;
                                    if (c.injectedKind[3] != injectedTotal) ++sumBad;
                                    if (pass == 1 && plan.admit != Admit::Inject && injectedTotal != 0) ++directBad;
                                }
    std::printf("  note  %u single-call runs (%u with landed forced true)\n", combos, forced);
    check(offKind == 0, "R3a", "across every combination (kinds 0..7, 100, 0xFFFFFFFF and unreadable; gates; all three modes; phase; window; four camera shapes) injectedKind[k] for k != 3 is never incremented");
    check(sumBad == 0, "R3b", "injectedKind[3] is exactly the injected scene plus first-person calls in every run");
    check(directBad == 0, "R3c", "a tally told every call landed still counts an injection only for a call the admission injected");
    check(planBad == 0, "R3e", "the plan injects exactly when the admission says Inject, and never flushes a camera the set does not hold");
    {   // The tally itself: only an Inject that landed moves injectedKind; by kind index.
        FlatCameraVrTally t;
        bool ok = true;
        for (Admit a : {Admit::Warming, Admit::AfterTrigger, Admit::RoleExcluded, Admit::NotActive, Admit::Observed, Admit::Stale,
                        Admit::Unsupported, Admit::OtherKind, Admit::Unreadable})
            for (Role r : kRoles)
                for (int landed = 0; landed < 2; ++landed) t.note(a, r, true, 3, landed != 0);
        FlatCameraVrCounters c = t.snapshot();
        for (int k = 0; k < 8; ++k) if (c.injectedKind[k]) ok = false;
        t.note(Admit::Inject, Role::Scene, true, 3, false);
        c = t.snapshot();
        for (int k = 0; k < 8; ++k) if (c.injectedKind[k]) ok = false;
        t.note(Admit::Inject, Role::Scene, true, 3, true);
        t.note(Admit::Inject, Role::FirstPerson, true, 3, true);
        c = t.snapshot();
        check(ok && c.injectedKind[3] == 2 && flatCameraVrKindIndex(true, 3) == 3 && flatCameraVrKindIndex(true, 5) == 5 &&
              flatCameraVrKindIndex(true, 6) == 6 && flatCameraVrKindIndex(true, 0xFFFFFFFFu) == 6 && flatCameraVrKindIndex(false, 3) == 7,
              "R3d", "no reason but a landed Inject moves injectedKind; a refused Inject does not; the kind index is 0..5, 6 other, 7 unreadable");
    }
}

// ---------------------------------------------------------------------------
// R4: the counters, exactly one outcome per call.
// ---------------------------------------------------------------------------
void testOutcomes() {
    std::printf("outcomes\n");
    Rng rng(0xC0FFEE);
    const uintptr_t pool[] = {0x25FEFC53770, 0x25FED68B4D0, 0x25FF2358330, 0x25FEFE00010, 0x25FEFE00020, 0x25FEFE00030};
    unsigned frames = 0, calls = 0, sumBad = 0, callsBad = 0, offBad = 0, kindBad = 0, flushBad = 0, failBad = 0, refusedBad = 0, windowBad = 0;
    unsigned oracleBad = 0, bucketBad = 0;
    Sim sim;   // one simulated detour for the whole run: the injected set lives across frames, as it does in the DLL
    for (int frameNo = 0; frameNo < 4000; ++frameNo) {
        const uint32_t modeRoll = rng.below(100);
        const bool inject = modeRoll < 55, observe = !inject && modeRoll < 80;
        const bool zeroPhase = rng.chance(20);
        sim.frame(inject, observe, zeroPhase ? 0.0f : 0.4f * (static_cast<float>(rng.below(200)) / 100.0f - 1.0f),
                  zeroPhase ? 0.0f : 0.4f * (static_cast<float>(rng.below(200)) / 100.0f - 1.0f));
        const uint32_t n = 20 + rng.below(80);
        const uint32_t closeAt = rng.chance(60) ? rng.below(n) : n + 1;
        unsigned fedOwner = 0, fedOff = 0, fedFlush = 0, fedFail = 0;
        unsigned expectSceneRefused = 0, expectFpRefused = 0;
        struct Expect { unsigned sceneInjected = 0, fpInjected = 0, sceneRefused = 0, fpRefused = 0, warming = 0, auxiliary = 0, afterTrigger = 0,
                        notActive = 0, unsupported = 0, otherKind = 0, unreadable = 0, stale = 0; } exp;
        for (uint32_t i = 0; i < n; ++i) {
            if (i == closeAt) sim.core.closeWindow();
            if (rng.chance(10)) sim.censusStep();
            const uint32_t kindRoll = rng.below(100);
            const uint32_t kind = kindRoll < 50 ? 3 : kindRoll < 60 ? 5 : kindRoll < 70 ? 0 : kindRoll < 80 ? 1 : kindRoll < 85 ? 4 : kindRoll < 90 ? 2 : kindRoll < 95 ? 6 : 3;
            const bool readable = !rng.chance(3);
            const uint32_t gateRoll = rng.below(100);
            const Gate gate = gateRoll < 86 ? Gate::Admit : gateRoll < 91 ? Gate::Disarmed : gateRoll < 95 ? Gate::Expired : Gate::OffThread;
            const uint32_t shape = rng.below(5);
            const FlatCameraVrFrustum fr = shape == 0 ? sceneCam() : shape == 1 ? weaponCam() : shape == 2 ? squareCam() : shape == 3 ? zoomCam() : FlatCameraVrFrustum{};
            const bool writeLands = !rng.chance(6);
            const Sim::Out out = sim.call(pool[rng.below(6)], readable, kind, fr, gate, rng.below(4) * 0x1000, writeLands);
            if (gate == Gate::OffThread) { ++fedOff; continue; }
            ++fedOwner;
            if (out.flushed) ++fedFlush;
            // What the call should have been counted as, from the table written from the brief (R2), with the role the planner decided.
            FlatCameraVrAdmitInput oi;
            oi.readable = readable; oi.kind = kind; oi.gate = gate; oi.mode = flatCameraVrModeOfBits(sim.bits);
            oi.windowOpen = sim.core.windowOpen(); oi.phaseNonzero = sim.core.phaseNonzero();
            oi.role = out.plan.roleKnown ? out.plan.role : Role::Scene;
            const Admit oa = oracleAdmit(oi);
            if (oa != out.plan.admit || out.plan.inject != (oa == Admit::Inject)) ++oracleBad;
            switch (oa) {
                case Admit::Inject:
                    if (out.landed) { if (out.plan.role == Role::FirstPerson) ++exp.fpInjected; else ++exp.sceneInjected; }
                    else { if (out.plan.role == Role::FirstPerson) ++exp.fpRefused; else ++exp.sceneRefused; }
                    break;
                case Admit::Warming: ++exp.warming; break;
                case Admit::AfterTrigger: ++exp.afterTrigger; break;
                case Admit::RoleExcluded: ++exp.auxiliary; break;
                case Admit::NotActive:
                case Admit::Observed: ++exp.notActive; break;
                case Admit::Stale: ++exp.stale; break;
                case Admit::Unsupported: ++exp.unsupported; break;
                case Admit::OtherKind: ++exp.otherKind; break;
                case Admit::Unreadable: ++exp.unreadable; break;
                case Admit::OffThread: break;
            }
            if (out.plan.inject && !out.landed) {
                ++fedFail;
                sim.core.tally().noteWriteFailure();
                if (out.plan.role == Role::FirstPerson) ++expectFpRefused; else ++expectSceneRefused;
            }
        }
        const FlatCameraVrCounters c = sim.core.tally().snapshot();
        if (c.sceneInjected != exp.sceneInjected || c.firstPersonInjected != exp.fpInjected || c.sceneRefused != exp.sceneRefused ||
            c.firstPersonRefused != exp.fpRefused || c.warming != exp.warming || c.auxiliary != exp.auxiliary || c.afterTrigger != exp.afterTrigger ||
            c.notActive != exp.notActive || c.unsupported != exp.unsupported || c.otherKind != exp.otherKind || c.unreadable != exp.unreadable ||
            c.stale != exp.stale)
            ++bucketBad;
        ++frames;
        calls += fedOwner;
        if (c.calls != fedOwner) ++callsBad;
        if (flatCameraVrOutcomeSum(c) != c.calls) ++sumBad;
        if (c.offThread != fedOff) ++offBad;
        uint32_t byKind = 0;
        for (int k = 0; k < 8; ++k) { byKind += c.injectedKind[k]; if (k != 3 && c.injectedKind[k]) ++kindBad; }
        if (byKind != c.sceneInjected + c.firstPersonInjected) ++kindBad;
        if (c.flushed != fedFlush) ++flushBad;
        if (c.writeFailures != fedFail) ++failBad;
        if (c.sceneRefused != expectSceneRefused || c.firstPersonRefused != expectFpRefused) ++refusedBad;
        if (!inject && (c.sceneInjected || c.firstPersonInjected || c.warming || c.auxiliary || c.afterTrigger)) ++windowBad;
    }
    std::printf("  note  %u random frames, %u owner-thread calls\n", frames, calls);
    check(callsBad == 0 && sumBad == 0, "R4a", "in every frame calls == the sum of the first-level outcomes (injected, refused, warming, auxiliary, after-trigger, not-active, unsupported, other-kind, unreadable, stale): every call reaches exactly one");
    check(offBad == 0, "R4b", "an off-thread call is counted in offThread alone, never in calls");
    check(kindBad == 0, "R4c", "injectedKind sums to the injected calls and only index 3 is ever non-zero, over the random frames");
    check(flushBad == 0 && failBad == 0 && refusedBad == 0, "R4d", "flushed, writeFailures and the refused outcomes count what the harness did, no more and no less");
    check(windowBad == 0, "R4e", "a frame that does not inject has no injected, warming, auxiliary or after-trigger call (its kind-3 calls are not-active or stale)");
    check(oracleBad == 0 && bucketBad == 0, "R4g", "every call was classified as the table says, and every frame's counters are exactly the table's buckets: each reason is counted in its own counter");
    // The reset: the frame step clears the counters; offThread is lock-free from any thread.
    {
        Sim s;
        s.frame(true, false, 0.3f, 0.3f);
        s.call(0x1000, true, 3, sceneCam());
        s.core.tally().noteOffThread();
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) threads.emplace_back([&s] { for (int i = 0; i < 25000; ++i) s.core.tally().noteOffThread(); });
        for (std::thread& t : threads) t.join();
        const uint32_t off = s.core.tally().snapshot().offThread;
        s.frame(true, false, 0.3f, 0.3f);
        const FlatCameraVrCounters c = s.core.tally().snapshot();
        FlatCameraVrCounters zero;
        check(off == 100001 && c.calls == 0 && c.offThread == 0 && flatCameraVrOutcomeSum(c) == 0 && c.sceneInjected == 0 && c.injectedKind[3] == 0 &&
              std::memcmp(&c, &zero, sizeof c) == 0,
              "R4f", "offThread counts from four threads without loss, and the frame step resets every counter");
    }
}

// ---------------------------------------------------------------------------
// R5: the camera-object reuse scenario.
// ---------------------------------------------------------------------------
void testReuseScenario() {
    std::printf("camera-object reuse\n");
    const uintptr_t C = 0x25FEFC53770;       // the left eye camera in the cockpit, the world camera on foot
    {
        Sim sim;
        // Frame 1, the cockpit: the route is not injecting. The camera is kind 5.
        sim.frame(false, false);
        Sim::Out a = sim.call(C, true, 5, eyeCam());
        check(a.plan.admit == Admit::Unsupported && !a.plan.inject && !a.frustumRead && !a.plan.roleKnown && !a.plan.flush && sim.set.empty(),
              "R5a", "cockpit, kind 5: Unsupported, not injected, no frustum read, nothing flushed (it was never injected)");
        // Frame 2, on foot: the route injects. The SAME pointer is now kind 3.
        sim.frame(true, false, 0.31f, -0.27f);
        Sim::Out scene = sim.call(C, true, 3, sceneCam(), Gate::Admit, 0x594E13);
        check(scene.plan.admit == Admit::Inject && scene.plan.role == Role::Scene && scene.frustumRead && scene.landed && sim.set.contains(C) && !scene.flushed,
              "R5b", "on foot, the same pointer as kind 3 with the screen aspect and near 0.025: Scene, injected; it joins the flush set");
        Sim::Out weapon = sim.call(C, true, 3, weaponCam(), Gate::Admit, 0x594EAB);
        check(weapon.plan.admit == Admit::Inject && weapon.plan.role == Role::FirstPerson && weapon.landed && sim.set.contains(C) && !weapon.flushed,
              "R5c", "the same pointer with near 0.0675 and a tighter fov: FirstPerson, injected (same phase as the scene camera)");
        Sim::Out aux = sim.call(C, true, 3, squareCam(), Gate::Admit, 0x58DE73);
        check(aux.plan.admit == Admit::RoleExcluded && aux.plan.role == Role::Auxiliary && aux.plan.roleKnown && !aux.plan.inject && !aux.landed &&
              aux.flushed && sim.flushWrites == 1 && !sim.set.contains(C) && sim.set.empty(),
              "R5d", "the same pointer as a square-aspect kind-3 camera: excluded by role, not injected, and the flush fires NOW, exactly once (the injected object seen un-injected)");
        Sim::Out eye = sim.call(C, true, 5, eyeCam());
        check(eye.plan.admit == Admit::Unsupported && !eye.plan.inject && !eye.flushed && sim.flushWrites == 1 && sim.set.empty(),
              "R5e", "then kind 5 again: Unsupported, not injected, and no second flush (the set no longer holds it)");
        const FlatCameraVrCounters c = sim.core.tally().snapshot();
        FlatCameraVrExcluded rows[8];
        const size_t n = sim.core.excluded().copy(rows, 8);
        check(c.calls == 4 && c.sceneInjected == 1 && c.firstPersonInjected == 1 && c.auxiliary == 1 && c.unsupported == 1 && c.notActive == 0 &&
              c.flushed == 1 && c.injectedKind[3] == 2 && c.injectedKind[5] == 0 && flatCameraVrOutcomeSum(c) == c.calls &&
              n == 1 && rows[0].aspect == 1.0f && rows[0].fov == 1.5708f && rows[0].nearZ == 0.1f && rows[0].farZ == 1000.0f && rows[0].callerRva == 0x58DE73 && rows[0].calls == 1,
              "R5f", "the frame's counters and the excluded signature (aspect, fov, near, far, caller) say exactly that");
        // The same sequence again on the next frame: admission is decided per call, never cached per pointer.
        sim.frame(true, false, -0.11f, 0.42f);
        const Admit want[] = {Admit::Inject, Admit::Inject, Admit::RoleExcluded, Admit::Unsupported, Admit::Inject, Admit::Unsupported, Admit::Inject};
        const FlatCameraVrFrustum shapes[] = {sceneCam(), weaponCam(), squareCam(), eyeCam(), sceneCam(), eyeCam(), sceneCam()};
        const uint32_t kinds[] = {3, 3, 3, 5, 3, 5, 3};
        bool perCall = true;
        unsigned flushes = 0;
        for (int i = 0; i < 7; ++i) {
            const Sim::Out o = sim.call(C, true, kinds[i], shapes[i]);
            if (o.plan.admit != want[i]) perCall = false;
            if (o.flushed) ++flushes;
        }
        // flushes: the auxiliary call flushes the injected camera (injected twice before it), then the eye call finds an empty set, the next scene
        // call injects again, the next eye call (kind 5 after an injection) flushes it once -- the kind change on a reused object.
        check(perCall && flushes == 2 && sim.flushWrites == 3,
              "R5g", "the next frame's sequence scene, weapon, square, eye, scene, eye, scene is admitted call by call (inject, inject, excluded, unsupported, inject, unsupported, inject): the pointer's history decides nothing; each injected-then-not edge flushes once");
        // A camera this session never injected is never flushed, whatever its calls are (the kind-3 scene call last: it injects it).
        Sim fresh;
        fresh.frame(true, false, 0.3f, 0.1f);
        bool freshNever = true;
        for (uint32_t kind : {0u, 1u, 2u, 4u, 5u, 6u, 3u})
            if (fresh.call(0x7770, true, kind, squareCam()).flushed) freshNever = false;
        check(freshNever && fresh.flushWrites == 0, "R5h", "a camera this session never injected is never flushed, whatever kinds and roles its calls show");
    }
    // Variants: every way the injected object can be seen un-injected flushes it once, and only when the call is one that may.
    {
        auto injectedSim = []() { auto s = std::make_unique<Sim>(); s->frame(true, false, 0.25f, 0.25f); s->call(0x2000, true, 3, sceneCam()); return s; };
        bool ok = true;
        {   // a kind change straight to kind 5
            auto s = injectedSim();
            const Sim::Out o = s->call(0x2000, true, 5, eyeCam());
            if (!(o.flushed && o.plan.admit == Admit::Unsupported && s->set.empty())) ok = false;
            if (s->call(0x2000, true, 5, eyeCam()).flushed) ok = false;
        }
        {   // kind 0 (another camera type reusing the pointer)
            auto s = injectedSim();
            if (!(s->call(0x2000, true, 0, squareCam()).flushed && s->set.empty())) ok = false;
        }
        {   // after the trigger, in the same frame
            auto s = injectedSim();
            s->core.closeWindow();
            const Sim::Out o = s->call(0x2000, true, 3, sceneCam());
            if (!(o.flushed && o.plan.admit == Admit::AfterTrigger && !o.plan.inject && s->set.empty())) ok = false;
        }
        {   // a warm-up frame (zero phase)
            auto s = injectedSim();
            s->frame(true, false, 0.0f, 0.0f);
            const Sim::Out o = s->call(0x2000, true, 3, sceneCam());
            if (!(o.flushed && o.plan.admit == Admit::Warming && s->set.empty())) ok = false;
        }
        {   // a released route: pass-through
            auto s = injectedSim();
            s->frame(false, false);
            const Sim::Out o = s->call(0x2000, true, 3, sceneCam());
            if (!(o.flushed && o.plan.admit == Admit::NotActive && s->set.empty())) ok = false;
        }
        {   // a released route with the census watching: observe-only
            auto s = injectedSim();
            s->frame(false, true);
            const Sim::Out o = s->call(0x2000, true, 3, sceneCam());
            if (!(o.flushed && o.plan.admit == Admit::Observed && s->set.empty())) ok = false;
        }
        {   // a lapsed frame window
            auto s = injectedSim();
            const Sim::Out o = s->call(0x2000, true, 3, sceneCam(), Gate::Expired);
            if (!(o.flushed && o.plan.admit == Admit::Stale && s->set.empty())) ok = false;
        }
        check(ok, "R5i", "an injected camera seen un-injected flushes once for each reason: kind 5 or 0 on a reused object, after the trigger, a warm-up, a release (pass-through or observe-only), a lapsed window");
        bool holds = true;
        {   // seen unreadable or on another thread: the flush waits (nothing about the memory is known; the set is the owner thread's)
            auto s = injectedSim();
            if (s->call(0x2000, false, 0, sceneCam()).flushed || s->set.empty()) holds = false;
            if (s->call(0x2000, true, 3, sceneCam(), Gate::OffThread).flushed || s->set.empty()) holds = false;
            if (!s->call(0x2000, true, 3, squareCam()).flushed || !s->set.empty()) holds = false;   // then it fires, once
        }
        check(holds, "R5j", "an unreadable call and an off-thread call leave an injected camera in the set; its first readable un-injected owner-thread call flushes it");
    }
}

// ---------------------------------------------------------------------------
// R6: the flush.
// ---------------------------------------------------------------------------
void testFlush() {
    std::printf("flush\n");
    const Admit all[] = {Admit::Inject, Admit::Warming, Admit::AfterTrigger, Admit::RoleExcluded, Admit::NotActive, Admit::Observed,
                         Admit::Stale, Admit::OffThread, Admit::Unsupported, Admit::OtherKind, Admit::Unreadable};
    const bool want[] = {false, true, true, true, true, true, true, false, true, true, false};
    bool table = true;
    for (size_t i = 0; i < 11; ++i) if (flatCameraVrFlushEligible(all[i]) != want[i]) table = false;
    check(table, "R6a", "eligible: every readable owner-thread call that was not injected (warming, after-trigger, role-excluded, not-active, observed, stale, kinds 4/5, other kinds); not: inject, off-thread, unreadable");
    bool oncePerInjection = true, bystander = true, holdsOnIneligible = true;
    for (size_t i = 0; i < 11; ++i) {
        FlatCameraInjectedSet set;
        set.noteInjected(0x3000);
        if (!want[i]) {
            if (flatCameraVrFlushDecision(set, 0x3000, all[i]) || !set.contains(0x3000)) holdsOnIneligible = false;
        } else {
            if (!flatCameraVrFlushDecision(set, 0x3000, all[i])) oncePerInjection = false;
            if (flatCameraVrFlushDecision(set, 0x3000, all[i]) || set.contains(0x3000)) oncePerInjection = false;
            set.noteInjected(0x3000);   // injected again: one more flush is due
            if (!flatCameraVrFlushDecision(set, 0x3000, all[i])) oncePerInjection = false;
        }
        if (flatCameraVrFlushDecision(set, 0x4000, all[i])) bystander = false;   // never injected
    }
    check(oncePerInjection, "R6b", "a flush is due once per injection: the camera leaves the set when it fires and a second call does not flush; a fresh injection makes another due");
    check(holdsOnIneligible, "R6c", "an ineligible call does not flush and does not forget the camera");
    check(bystander, "R6d", "a camera never injected is never flushed, for any reason");
    {
        FlatCameraInjectedSet set;
        set.noteInjected(0x5000); set.noteInjected(0x6000);
        const bool a = flatCameraVrFlushDecision(set, 0x5000, Admit::AfterTrigger);
        const bool stillB = set.contains(0x6000) && !set.empty();
        const bool b = flatCameraVrFlushDecision(set, 0x6000, Admit::Unsupported);
        check(a && stillB && b && set.empty() && !flatCameraVrFlushDecision(set, 0, Admit::Warming), "R6e", "cameras flush independently; a null camera never does");
    }
    // The flat table is not the VR table: the census's observe-only call never flushes there, and that answer is untouched.
    check(!flatCameraFlushEligible(FlatCameraAdmit::Observed) && flatCameraFlushEligible(FlatCameraAdmit::Warming) &&
          flatCameraFlushEligible(FlatCameraAdmit::NotUpstream) && flatCameraFlushEligible(FlatCameraAdmit::GateClosed) &&
          !flatCameraFlushEligible(FlatCameraAdmit::Unsupported) && !flatCameraFlushEligible(FlatCameraAdmit::Inject),
          "R6f", "the flat profile's flush table is as it was: observed, unsupported and inject are not eligible (the VR table differs on purpose, for a camera the route injected)");
}

// ---------------------------------------------------------------------------
// R7: the excluded signatures.
// ---------------------------------------------------------------------------
void testExcluded() {
    std::printf("excluded signatures\n");
    {
        FlatCameraVrExcludedTable t;
        for (int i = 0; i < 12; ++i) t.note(1.0f + 0.1f * static_cast<float>(i), 1.5f, 0.1f, 1000.0f, 0x58DE73);
        FlatCameraVrExcluded rows[16];
        const size_t n = t.copy(rows, 16);
        check(n == 8 && t.used() == 8 && t.overflow() == 4 && rows[0].aspect == 1.0f && rows[7].aspect == 1.0f + 0.1f * 7.0f,
              "R7a", "twelve distinct signatures keep the first eight, oldest first; the other four are counted as overflow");
        FlatCameraVrExcluded few[3];
        check(t.copy(few, 3) == 3 && few[2].aspect == rows[2].aspect && t.copy(nullptr, 3) == 0 && t.copy(rows, 0) == 0, "R7b", "copy honours max and a null buffer");
    }
    {
        FlatCameraVrExcludedTable t;
        t.note(1.0f, 1.5708f, 0.1f, 1000.0f, 0x58DE73);
        t.note(1.0f, 1.5708f, 0.1f, 1000.0f, 0x58DE73);                 // the same
        t.note(1.0f, 1.5708f * 1.0005f, 0.1f, 1000.0f, 0x58DE73);      // 0.05% off in fov: the same
        t.note(1.0f, 1.5708f * 1.002f, 0.1f, 1000.0f, 0x58DE73);       // 0.2% off: another
        t.note(1.0f, 1.5708f, 0.1f, 1000.0f, 0x594E13);                // another call site: another
        t.note(kNaN, kNaN, kNaN, kNaN, 0);                              // unreadable
        t.note(kNaN, kNaN, kNaN, kNaN, 0);                              // the same unreadable
        t.note(kInf, 1.0f, 0.1f, 1000.0f, 0);
        t.note(kInf, 1.0f, 0.1f, 1000.0f, 0);
        FlatCameraVrExcluded rows[8];
        const size_t n = t.copy(rows, 8);
        check(n == 5 && rows[0].calls == 3 && rows[1].calls == 1 && rows[2].calls == 1 && rows[3].calls == 2 && rows[4].calls == 2 &&
              rows[3].aspect != rows[3].aspect && rows[4].aspect == kInf,
              "R7c", "the same signature to 0.1% and the same call site is one row that counts its calls; another fov, another caller, a NaN row (once) and an infinite one (once) are rows");
        check(t.overflow() == 0, "R7d", "a repeated signature is not overflow");
    }
    {   // lock-free publication: a reader on another thread only ever sees whole rows the writer wrote. The writer waits for the reader to
        // have seen each row it published (so every intermediate state is read, deterministically), and keeps counting calls meanwhile.
        constexpr int kTrials = 200;
        std::vector<std::unique_ptr<FlatCameraVrExcludedTable>> tables;
        std::vector<std::atomic<uint32_t>> seen(kTrials);
        for (int i = 0; i < kTrials; ++i) { tables.emplace_back(new FlatCameraVrExcludedTable()); seen[i].store(0); }
        std::atomic<int> current{-1};
        std::atomic<bool> stop{false};
        std::atomic<uint32_t> bad{0};
        std::atomic<uint64_t> reads{0};
        std::thread reader([&] {
            FlatCameraVrExcluded rows[8];
            size_t lastCount = 0;
            int lastTable = -1;
            while (!stop.load(std::memory_order_acquire)) {
                const int idx = current.load(std::memory_order_acquire);
                if (idx < 0) continue;
                if (idx != lastTable) { lastTable = idx; lastCount = 0; }
                const size_t n = tables[idx]->copy(rows, 8);
                if (n < lastCount) bad.fetch_add(1);
                lastCount = n;
                for (size_t i = 0; i < n; ++i) {
                    const float k = rows[i].aspect;   // the writer makes row i from its own index: aspect k, fov 2k, near 3k, far 4k, caller 5k
                    if (!(rows[i].fov == 2.0f * k && rows[i].nearZ == 3.0f * k && rows[i].farZ == 4.0f * k && rows[i].callerRva == static_cast<uint64_t>(5.0f * k) && rows[i].calls >= 1))
                        bad.fetch_add(1);
                }
                uint32_t prev = seen[idx].load();
                while (n > prev && !seen[idx].compare_exchange_weak(prev, static_cast<uint32_t>(n))) {}
                reads.fetch_add(1);
            }
        });
        unsigned stuck = 0;
        for (int trial = 0; trial < kTrials; ++trial) {
            current.store(trial, std::memory_order_release);
            for (int i = 0; i < 8; ++i) {
                const float k = static_cast<float>(1 + i);
                tables[trial]->note(k, 2.0f * k, 3.0f * k, 4.0f * k, static_cast<uint64_t>(5.0f * k));
                for (int again = 0; again < 50; ++again) tables[trial]->note(k, 2.0f * k, 3.0f * k, 4.0f * k, static_cast<uint64_t>(5.0f * k));   // calls++ while the reader copies
                for (int spins = 0; seen[trial].load() < static_cast<uint32_t>(i + 1) && spins < 2000000; ++spins) std::this_thread::yield();
                if (seen[trial].load() < static_cast<uint32_t>(i + 1)) { ++stuck; break; }
            }
        }
        stop.store(true, std::memory_order_release);
        reader.join();
        bool allFull = true;
        for (int i = 0; i < kTrials; ++i) if (tables[i]->used() != 8) allFull = false;
        check(bad.load() == 0 && stuck == 0 && reads.load() > 0 && allFull, "R7e",
              "a reader copying while the writer notes signatures only ever sees whole rows, sees every row the writer published, and the count never goes down (200 tables, rows published one at a time against a live reader)");
    }
    {
        FlatCameraVrExcluded out;
        FlatCameraVrCore core;
        FlatCameraInjectedSet set;
        FlatCameraVrFrame f; f.inject = true; f.phaseX = 0.3f; f.renderW = 5040; f.renderH = 2835; f.screenAspect = kS;
        core.beginFrame(f, 500);
        FlatCameraVrCallIn in; in.camera = 0x100; in.readable = true; in.kind = 3; in.gate = Gate::Admit; in.mode = Mode::Inject; in.callerRva = 0x58DE73;
        core.plan(in, squareCam(), set);
        core.plan(in, sceneCam(), set);                   // a screen view: no row
        in.kind = 5;
        core.plan(in, eyeCam(), set);                     // an eye: no row
        in.kind = 3; in.gate = Gate::Expired;
        core.plan(in, squareCam(), set);                  // a lapsed window: not excluded by role (stale), no row
        in.gate = Gate::Admit; in.mode = Mode::Observe;
        core.plan(in, squareCam(), set);                  // observe-only: no role, no row
        check(core.excluded().used() == 1 && core.excluded().copy(&out, 1) == 1 && out.aspect == 1.0f, "R7f", "only a role exclusion makes a row: not a screen view, an eye, a stale call or an observed one");
    }
}

// ---------------------------------------------------------------------------
// R8: the mode word.
// ---------------------------------------------------------------------------
void testModeWord() {
    std::printf("mode word\n");
    const uint32_t D = kFlatCameraVrBitDriven, I = kFlatCameraVrBitInject, O = kFlatCameraVrBitObserve;
    check(D == 1 && I == 2 && O == 4 && flatCameraVrBitsForFrame(false, false, true) == D && flatCameraVrBitsForFrame(true, false, true) == (D | I) &&
          flatCameraVrBitsForFrame(false, true, true) == (D | O) && flatCameraVrBitsForFrame(true, true, true) == (D | I | O),
          "R8a", "a stepped frame is the Driven bit plus Inject and Observe as asked");
    check(flatCameraVrBitsForFrame(true, false, false) == D && flatCameraVrBitsForFrame(true, true, false) == D && flatCameraVrBitsForFrame(false, true, false) == D &&
          flatCameraVrModeOfBits(flatCameraVrBitsForFrame(true, true, false)) == Mode::PassThrough,
          "R8b", "a hook that is not live honours nothing: inject is not honoured, the frame is pass-through");
    check(flatCameraVrModeOfBits(0) == Mode::PassThrough && flatCameraVrModeOfBits(D) == Mode::PassThrough && flatCameraVrModeOfBits(D | O) == Mode::Observe &&
          flatCameraVrModeOfBits(D | I) == Mode::Inject && flatCameraVrModeOfBits(D | I | O) == Mode::Inject &&
          flatCameraVrObserves(D | O) && flatCameraVrObserves(D | I | O) && !flatCameraVrObserves(D | I) && !flatCameraVrObserves(D) && !flatCameraVrObserves(0),
          "R8c", "injection wins over observation as the mode, and Observe says whether the census hears the calls (alone or with injection)");
    // THE CENSUS'S FRAME STEP.
    bool injectKept = true, undriven = true, othersObserve = true;
    for (uint32_t bits = 0; bits < 8; ++bits) {
        const uint32_t after = flatCameraVrBitsAfterCensusFrame(bits);
        if (bits & I) { if (after != bits || flatCameraVrModeOfBits(after) != Mode::Inject) injectKept = false; }
        if (!(bits & D)) { if (after != bits) undriven = false; }
        else if (!(bits & I)) { if (after != (bits | O) || flatCameraVrModeOfBits(after) != Mode::Observe) othersObserve = false; }
    }
    check(injectKept, "R8d", "the census's frame step never switches an injection frame to observe-only: a word with Inject comes back unchanged, mode Inject");
    check(undriven, "R8e", "an undriven word (the flat profile's, or a VR process the route never steps) comes back untouched: the census alone keeps the flat code's observe-only switch");
    check(othersObserve, "R8f", "any other frame of a driven detour becomes observe-only for the census, as it is for the census alone");
    // Quiet.
    check(flatCameraVrQuietFor(0, true) && flatCameraVrQuietFor(D, true) && !flatCameraVrQuietFor(D, false) && !flatCameraVrQuietFor(D | I, true) &&
          !flatCameraVrQuietFor(D | O, true) && !flatCameraVrQuietFor(D | I | O, true) && !flatCameraVrQuietFor(D | I, false) && !flatCameraVrQuietFor(0, false),
          "R8g", "quiet is: neither injection nor observation this frame, and no camera waiting for its flush");
    // A script: quiet before any install, not quiet while a frame injects or observes, not quiet with a camera pending its flush, quiet after.
    {
        Sim sim;
        const bool beforeAny = flatCameraVrQuietFor(sim.bits, sim.set.empty());
        sim.frame(true, false, 0.3f, 0.3f);
        sim.call(0x7000, true, 3, sceneCam());
        const bool whileInjecting = flatCameraVrQuietFor(sim.bits, sim.set.empty());
        sim.frame(false, true);
        const bool whileObserving = flatCameraVrQuietFor(sim.bits, sim.set.empty());
        sim.frame(false, false);
        const bool pendingFlush = flatCameraVrQuietFor(sim.bits, sim.set.empty());   // released, but the camera still holds its phase
        const bool flushed = sim.call(0x7000, true, 3, sceneCam()).flushed;
        const bool afterFlush = flatCameraVrQuietFor(sim.bits, sim.set.empty());
        check(beforeAny && !whileInjecting && !whileObserving && !pendingFlush && flushed && afterFlush,
              "R8h", "quiet before any install; not while a frame injects or observes; not with an injected camera pending its flush; quiet again after it");
    }
    // The census's step inside a scripted boundary: the route's step first, the census's second.
    {
        Sim sim;
        sim.frame(true, false, 0.3f, 0.2f);
        sim.censusStep();
        const Sim::Out o = sim.call(0x7100, true, 3, sceneCam());
        sim.frame(false, false);
        sim.censusStep();
        const Sim::Out p = sim.call(0x7100, true, 3, sceneCam());
        check(o.plan.admit == Admit::Inject && o.landed && p.plan.admit == Admit::Observed && p.flushed,
              "R8i", "an injection frame stays injecting after the census's step; a released frame with the census watching is observe-only, and still flushes what was injected");
    }
}

// ---------------------------------------------------------------------------
// R9: the arithmetic, the frame-window lapse, the stand-down.
// ---------------------------------------------------------------------------
void testArithmetic() {
    std::printf("injection arithmetic\n");
    // The flat injector's own two lines, reproduced here from its source (they are hashed in R11): boundX += jx / W, boundY -= jy / H.
    auto flatBound = [](float entryX, float entryY, float jx, float jy, uint32_t rw, uint32_t rh, float* ox, float* oy) {
        *ox = entryX + jx / static_cast<float>(rw);
        *oy = entryY - jy / static_cast<float>(rh);
    };
    Rng rng(12345);
    unsigned differ = 0;
    for (int i = 0; i < 200000; ++i) {
        const float ex = (static_cast<float>(rng.below(20001)) - 10000.0f) / 10000.0f * 0.3f, ey = (static_cast<float>(rng.below(20001)) - 10000.0f) / 10000.0f * 0.3f;
        const float jx = (static_cast<float>(rng.below(2001)) - 1000.0f) / 1000.0f, jy = (static_cast<float>(rng.below(2001)) - 1000.0f) / 1000.0f;
        const uint32_t w = 1 + rng.below(8000), h = 1 + rng.below(5000);
        float a, b, c, d;
        flatBound(ex, ey, jx, jy, w, h, &a, &b);
        flatCameraVrBound(ex, ey, jx, jy, w, h, &c, &d);
        if (std::memcmp(&a, &c, 4) != 0 || std::memcmp(&b, &d, 4) != 0) ++differ;
    }
    check(differ == 0, "R9a", "the bound pair the VR path writes is bit-for-bit the flat injector's, over 200000 random phases, sizes and entry values");
    {
        float x, y;
        flatCameraVrBound(0.0f, 0.0f, 1.0f, 1.0f, 5040, 2835, &x, &y);
        check(x == 1.0f / 5040.0f && y == -1.0f / 2835.0f, "R9b", "the sign convention: content right by one pixel is boundX + 1/W, content down by one pixel is boundY - 1/H");
    }
    {
        FlatCameraVrCore core;
        FlatCameraVrFrame f; f.inject = true; f.phaseX = 0.5f; f.phaseY = -0.25f; f.renderW = 2520; f.renderH = 1418; f.screenAspect = 2520.0f / 1418.0f;
        core.beginFrame(f, 10);
        float x, y, wx, wy;
        core.bound(0.125f, -0.5f, &x, &y);
        flatCameraVrBound(0.125f, -0.5f, 0.5f, -0.25f, 2520, 1418, &wx, &wy);
        check(x == wx && y == wy && core.phaseNonzero() && core.screenAspect() == f.screenAspect && core.stepAtMs() == 10,
              "R9c", "the phase and the render size come from the frame step's state (not from the flat runtime)");
    }
    check(flatCameraVrPhaseNonzero(0.3f, 0.0f, 5040, 2835) && flatCameraVrPhaseNonzero(0.0f, -0.3f, 5040, 2835) && !flatCameraVrPhaseNonzero(0.0f, 0.0f, 5040, 2835) &&
          !flatCameraVrPhaseNonzero(-0.0f, 0.0f, 5040, 2835) && !flatCameraVrPhaseNonzero(0.3f, 0.3f, 0, 2835) && !flatCameraVrPhaseNonzero(0.3f, 0.3f, 5040, 0) &&
          !flatCameraVrPhaseNonzero(kNaN, 0.3f, 5040, 2835) && !flatCameraVrPhaseNonzero(0.3f, kInf, 5040, 2835) && !flatCameraVrPhaseNonzero(0.0f, kNaN, 5040, 2835),
          "R9d", "a phase is non-zero when either axis is and both are finite and the render size is known: zero, a size of zero, NaN and infinity write nothing (warming)");
    // The frame-window lapse, in an injecting frame.
    const Mode inj = Mode::Inject;
    check(flatCameraVrEffectiveGate(inj, Gate::Admit, 1000, 1000) == Gate::Admit && flatCameraVrEffectiveGate(inj, Gate::Admit, 1000, 1000 + FlatCameraGate::kExpiryMs) == Gate::Admit &&
          flatCameraVrEffectiveGate(inj, Gate::Admit, 1000, 1001 + FlatCameraGate::kExpiryMs) == Gate::Expired && flatCameraVrEffectiveGate(inj, Gate::Admit, 0, 1000) == Gate::Expired &&
          flatCameraVrEffectiveGate(inj, Gate::Admit, 1000, 900) == Gate::Admit,
          "R9e", "in an injecting frame a window the route's step armed is honoured for the frame window's expiry from the STEP, then lapses, whatever keeps the gate itself open");
    check(flatCameraVrEffectiveGate(inj, Gate::Disarmed, 1000, 1000) == Gate::Disarmed && flatCameraVrEffectiveGate(inj, Gate::Expired, 1000, 1000) == Gate::Expired &&
          flatCameraVrEffectiveGate(inj, Gate::OffThread, 1000, 9999) == Gate::OffThread,
          "R9f", "the lapse only ever closes an open window: the other verdicts pass through");
    {
        bool othersKeepTheGate = true;
        for (Mode other : {Mode::PassThrough, Mode::Observe})
            for (Gate g : kGates)
                for (uint64_t now : {1000ull, 1500ull, 1501ull, 99999ull})
                    if (flatCameraVrEffectiveGate(other, g, 0, now) != g || flatCameraVrEffectiveGate(other, g, 1000, now) != g) othersKeepTheGate = false;
        check(othersKeepTheGate, "R9h", "outside an injecting frame the gate's own verdict stands whatever the step's age: the census's window field is what it is for the census alone");
    }
    {   // the stand-down
        FlatCameraVrFailureWindow w;
        bool any = false;
        for (int i = 0; i < 7; ++i) any |= w.note(1000 + 10 * i, 1);
        const bool sevenOk = !any && w.total() == 7;
        const bool eighth = w.note(1100, 1);
        FlatCameraVrFailureWindow w2;
        w2.note(1000, 7);
        const bool splitOk = !w2.note(6001, 1) && w2.total() == 1;   // the next window: the count restarted
        FlatCameraVrFailureWindow w3;
        const bool burst = w3.note(2000, 8);
        FlatCameraVrFailureWindow w4;
        bool quiet = true;
        for (int i = 0; i < 1000; ++i) if (w4.note(1000 + 100 * i, 0)) quiet = false;
        check(sevenOk && eighth && splitOk && burst && quiet && kFlatCameraWriteFailureLimit == 8 && FlatCameraVrFailureWindow::kWindowMs == 5000,
              "R9g", "eight failed camera writes in one 5 s window stand the hook down (the flat path's rule); seven do not; a window that ended forgets its count; no failures never do");
    }
}

// ---------------------------------------------------------------------------
// R10: the flat profile's table.
// ---------------------------------------------------------------------------
FlatCameraAdmit oracleFlat(const FlatCameraAdmitInput& in) {
    if (in.gate == Gate::OffThread) return FlatCameraAdmit::OffThread;
    if (!in.readable) return FlatCameraAdmit::Unreadable;
    if (in.kind == 4 || in.kind == 5) return FlatCameraAdmit::Unsupported;
    if (in.kind != 3) return FlatCameraAdmit::OtherKind;
    if (in.observeOnly) return FlatCameraAdmit::Observed;
    if (in.gate != Gate::Admit) return FlatCameraAdmit::GateClosed;
    if (!in.upstreamOwns) return FlatCameraAdmit::NotUpstream;
    if (!in.phaseNonzero) return FlatCameraAdmit::Warming;
    return FlatCameraAdmit::Inject;
}

void testFlatTable() {
    std::printf("flat profile\n");
    unsigned combos = 0, mismatches = 0, injects = 0;
    for (int readable = 0; readable < 2; ++readable)
        for (uint32_t kind : kKinds)
            for (Gate gate : kGates)
                for (int upstream = 0; upstream < 2; ++upstream)
                    for (int phase = 0; phase < 2; ++phase)
                        for (int observe = 0; observe < 2; ++observe) {
                            FlatCameraAdmitInput in;
                            in.readable = readable != 0; in.kind = kind; in.gate = gate; in.upstreamOwns = upstream != 0; in.phaseNonzero = phase != 0; in.observeOnly = observe != 0;
                            const FlatCameraAdmit a = flatCameraAdmit(in);
                            ++combos;
                            if (a != oracleFlat(in)) ++mismatches;
                            if (a == FlatCameraAdmit::Inject) ++injects;
                        }
    std::printf("  note  %u flat input combinations, %u of them inject\n", combos, injects);
    check(mismatches == 0 && injects == 1, "R10a", "flatCameraAdmit answers what it always answered over its whole input space, and injects for exactly one combination (the flat profile's mutation set is unchanged)");
    check(static_cast<int>(FlatCameraAdmit::Inject) == 0 && static_cast<int>(FlatCameraAdmit::Unreadable) == 7 && static_cast<int>(FlatCameraAdmit::Observed) == 8,
          "R10b", "the flat enumerators keep their numbers (Observed is the last)");
}

// ---------------------------------------------------------------------------
// R11: source pins.
// ---------------------------------------------------------------------------
std::string slurp(const std::string& relative) {
    std::ifstream in(g_root + "/" + relative, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}
unsigned count(const std::string& text, const std::string& needle) {
    if (needle.empty()) return 0;
    unsigned n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
// The text of the function whose definition starts with `head`, up to the line "}" that closes it at column 0 (inclusive).
std::string functionBody(const std::string& text, const std::string& head) {
    const size_t at = text.find(head);
    if (at == std::string::npos) return std::string();
    const size_t end = text.find("\n}\n", at);
    return end == std::string::npos ? std::string() : text.substr(at, end + 3 - at);
}
// A region from an anchor to an end anchor (inclusive), or to the end of the function when there is none. Empty when an anchor is missing.
std::string region(const std::string& text, const char* start, const char* end) {
    const size_t s = text.find(start);
    if (s == std::string::npos) return std::string();
    size_t e;
    if (end) {
        e = text.find(end, s);
        if (e == std::string::npos) return std::string();
        e += std::strlen(end);
    } else {
        e = text.find("\n}\n", s);
        if (e == std::string::npos) return std::string();
        e += 3;
    }
    return text.substr(s, e - s);
}
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}
size_t pos(const std::string& text, const std::string& needle) { return text.find(needle); }
bool ordered(const std::string& text, std::initializer_list<const char*> needles) {
    size_t last = 0;
    bool first = true;
    for (const char* n : needles) {
        const size_t at = text.find(n);
        if (at == std::string::npos) return false;
        if (!first && at <= last) return false;
        last = at;
        first = false;
    }
    return true;
}
bool lacksAll(const std::string& text, std::initializer_list<const char*> needles) {
    if (text.empty()) return false;
    for (const char* n : needles) if (text.find(n) != std::string::npos) return false;
    return true;
}

struct Golden { const char* name; const char* start; const char* end; uint64_t hash; };

// The regions of flat_camera_inject.cpp that are the flat profile's and the census's own, hashed from the file as it was before the VR
// mode (commit 51a9b134). "preFlat" is the body of refreshPre from the census's switch on; "postFlat" is refreshPost from the observe
// branch on; the VR branch sits above both and the regions do not contain it.
const Golden kInjectGolden[] = {
    {"observeCall", "void observeCall(", nullptr, 0x7510B08D1E22A3EAull},
    {"observeOffThread", "void observeOffThread(uintptr_t r0, uintptr_t camera) noexcept {", nullptr, 0xFA29885A0B99C0E6ull},
    {"preThread", "    // The thread first. The phase machine, the decision, the census and the\n",
     "    const FlatCameraGateVerdict gate = g_inject.gate.check(GetCurrentThreadId(), GetTickCount64());\n", 0x10741B8A5965C7CCull},
    {"preFlat", "    // The VR camera census's switch: a process that observes only never leaves it (flatCameraInjectObserveFrame).\n", nullptr, 0xCED9706398525B45ull},
    {"postFlat", "    if (g_refreshTls.observe) {\n", nullptr, 0xD8A87AE0BF95ACF3ull},
    {"flatFrame", "void flatCameraInjectFrame(uint64_t frame, bool temporalModeEnabled) {", nullptr, 0x72226A8849CEA69Cull},
    {"flatPause", "void flatCameraInjectPause(bool paused) {", nullptr, 0x90A83A563BF10BA2ull},
    {"flatClose", "void flatCameraInjectClose(bool phaseNonzero, bool applied, bool clean, bool sceneNamed) {", nullptr, 0x28890A6E5363AED4ull},
    {"flatReset", "void flatCameraInjectReset() {", nullptr, 0xEF58F30F176668DAull},
    {"flatArmDisarm", "// The window opens once the frame's phase is chosen; every Present edge closes\n",
     "void flatCameraInjectDisarm() { g_inject.gate.disarm(GetCurrentThreadId()); }\n", 0xABEAABA979ABCC45ull},
    {"flatHelpers", "bool flatCameraInjectUpstreamOwns() {",
     "bool flatCameraInjectTakeHistoryReset() { return g_inject.wanted && g_inject.core.takeHistoryReset(); }\n", 0xC71200FE2FD8D993ull},
};
// The regions of flat_camera_phase.h the VR mode reuses and the brief says stay exactly as they are.
const Golden kPhaseGolden[] = {
    {"admitEnum", "enum class FlatCameraAdmit : uint8_t {", "};\n", 0xA0F1ABC1CFF180EAull},
    {"admitInput", "struct FlatCameraAdmitInput {", "};\n", 0x6B92B5BC2958A5B3ull},
    {"admitFn", "inline FlatCameraAdmit flatCameraAdmit(const FlatCameraAdmitInput& in) {", nullptr, 0x282E2983094E9D14ull},
    {"injectedSet", "class FlatCameraInjectedSet {", "};\n", 0xECE68BDC3BC5BA68ull},
    {"flushEligible", "inline bool flatCameraFlushEligible(FlatCameraAdmit a) {", nullptr, 0x5A23E7B4FED5F819ull},
    {"flushDecision", "inline bool flatCameraFlushDecision(FlatCameraInjectedSet& set, uintptr_t camera, FlatCameraAdmit a) {", nullptr, 0xB0B314452F0CBB37ull},
    {"gateClass", "class FlatCameraGate {", "};\n", 0xA18906A1E205AC35ull},
    {"verdictEnum", "enum class FlatCameraGateVerdict : uint8_t { Admit, Disarmed, Expired, OffThread };", "\n", 0x9912A0487B24AC42ull},
    {"failureLimit", "constexpr uint64_t kFlatCameraWriteFailureLimit = 8;", "\n}\n", 0x22980718EC9B6661ull},
};

void testSourcePins() {
    std::printf("source pins\n");
    const std::string cpp = slurp("src/d3d11/flat_camera_inject.cpp");
    const std::string inj = slurp("src/d3d11/flat_camera_inject.h");
    const std::string vrh = slurp("src/d3d11/flat_camera_vr.h");
    const std::string phase = slurp("src/d3d11/flat_camera_phase.h");
    const std::string censusCore = slurp("src/d3d11/vr_camera_census_core.h");
    check(!cpp.empty() && !inj.empty() && !vrh.empty() && !phase.empty() && !censusCore.empty(), "R11a", "the injector, its header, the pure VR header, the admission header and the census core are readable from the root");

    // ---- the flat profile and the census: byte-for-byte --------------------------------------------------------------------
    {
        std::string bad;
        for (const Golden& g : kInjectGolden) {
            const std::string r = region(cpp, g.start, g.end);
            if (r.empty() || count(cpp, g.start) != 1 || fnv1a(r) != g.hash) { bad += g.name; bad += ' '; }
        }
        check(bad.empty(), "R11b", "the flat profile's and the census's regions of the detour are byte-for-byte what they were before the VR mode: the observe-only call and its off-thread report, refreshPre from the census's switch on, refreshPost from its observe branch on, the flat frame step, pause, close, reset, arm and disarm, and the route accessors");
        if (!bad.empty()) std::printf("  note  changed regions: %s\n", bad.c_str());
        std::string badPhase;
        for (const Golden& g : kPhaseGolden) {
            const std::string r = region(phase, g.start, g.end);
            if (r.empty() || count(phase, g.start) != 1 || fnv1a(r) != g.hash) { badPhase += g.name; badPhase += ' '; }
        }
        check(badPhase.empty(), "R11c", "flatCameraAdmit, its enum and input, the flush table, the injected set, the frame window and the write-failure limit are byte-for-byte what they were");
        if (!badPhase.empty()) std::printf("  note  changed regions: %s\n", badPhase.c_str());
        // Control: the hash can fail (one byte of the region changed changes it).
        const Golden& g = kInjectGolden[3];
        std::string r = region(cpp, g.start, g.end);
        if (!r.empty()) r[r.size() / 2] ^= 1;
        check(!r.empty() && fnv1a(r) != g.hash, "R11d", "control: a region with one byte changed no longer hashes to the golden value");
    }

    // ---- the VR branch in refreshPre and refreshPost -------------------------------------------------------------------------
    const std::string pre = functionBody(cpp, "void refreshPre(uintptr_t r0, uintptr_t ctx, uintptr_t p2, uintptr_t camera) noexcept {");
    const std::string post = functionBody(cpp, "void refreshPost() noexcept {");
    {
        const char* branch = "    const uint32_t vrBits = g_vrBits.load(std::memory_order_acquire);\n    if (vrBits != 0) {\n        refreshPreVr(r0, ctx, p2, camera, callNo, gate, vrBits);\n        return;\n    }\n";
        check(!pre.empty() && count(pre, branch) == 1 && count(cpp, "refreshPreVr(r0, ctx, p2, camera, callNo, gate, vrBits);") == 1 &&
              ordered(pre, {"g_refreshTls.vr = 0;\n", "const uint64_t callNo = ", "const FlatCameraGateVerdict gate = g_inject.gate.check(GetCurrentThreadId(), GetTickCount64());\n", branch,
                            "const bool observe = g_inject.observeOnly.load(std::memory_order_acquire);"}),
              "R11e", "refreshPre clears the VR flag with the others, and hands the call to refreshPreVr once the gate is known and before the flat code reads its observe-only switch: a non-zero mode word takes the VR path, zero takes the old one");
        const size_t branchAt = pos(pre, branch);
        const std::string above = branchAt == std::string::npos ? std::string() : pre.substr(0, branchAt);
        check(!above.empty() && lacksAll(above, {"sehWrite", "flushCamera", "noteInjected", "flatRuntime", "observeCall(", "admission"}),
              "R11f", "nothing above the VR branch writes a camera, flushes, notes an injection, asks the flat runtime or reports to the census");
        const char* vrPost = "    if (g_refreshTls.vr) { // the VR world route's call: its own post half (the census, then the restore)\n        g_refreshTls.vr = 0;\n        refreshPostVr(camera);\n        return;\n    }\n";
        check(!post.empty() && count(post, vrPost) == 1 &&
              ordered(post, {"    g_refreshTls.armed = 0;\n", "    const uintptr_t camera = g_refreshTls.camera;\n", vrPost, "    if (g_refreshTls.observe) {\n",
                             "    sehWriteF32(camera + kCamBoundX, g_refreshTls.entryX);\n"}) &&
              lacksAll(post.substr(0, pos(post, vrPost)), {"sehWrite", "flatRuntimeNoteCameraApplied"}),
              "R11g", "refreshPost hands a VR call to refreshPostVr right after it disarms and reads the camera, ahead of the census-only branch and the flat restore, with no write above it");
        check(count(cpp, "g_vrBits.store(") == 4 && count(cpp, "g_vrBits.load(") == 3 && count(cpp, "g_vrBits") == 9,
              "R11h", "the mode word is touched in these places and no other: its declaration, refreshPre's load, the census step (a load and a store), the route's frame step (three stores), quiet's load (and one comment)");
        std::string flatOnly = region(cpp, "void flatCameraInjectFrame(uint64_t frame, bool temporalModeEnabled) {", nullptr);
        check(!flatOnly.empty() && flatOnly.find("g_vr") == std::string::npos && kInjectGolden[5].hash == fnv1a(flatOnly),
              "R11i", "the flat frame step never touches the mode word or the VR state: only flatCameraVrFrame sets it, and the flat profile never calls that");
    }

    // ---- the call's steps, in order -------------------------------------------------------------------------------------------
    const std::string vrpre = functionBody(cpp, "void refreshPreVr(");
    const std::string commit = functionBody(cpp, "bool vrCommit(");
    const std::string vrpost = functionBody(cpp, "void refreshPostVr(uintptr_t camera) noexcept {");
    const std::string frustum = functionBody(cpp, "FlatCameraVrFrustum readFrustum(uintptr_t camera) noexcept {");
    {
        check(!vrpre.empty() &&
              ordered(vrpre, {"if (gate == FlatCameraGateVerdict::OffThread) {", "sehReadU32(camera + kCamKind, &kind)", "flatCameraVrEffectiveGate(in.mode, gate, g_vr.stepAtMs(), GetTickCount64())",
                              "g_vr.wantsFrustum(in)", "readFrustum(camera)", "g_vr.plan(in, frustum, g_inject.injected)", "observer->pre(call)", "flushCamera(camera)",
                              "vrCommit(r0, ctx, camera, callNo, plan.inject, wantPost)", "g_vr.finish(plan, in, landed)"}),
              "R11j", "a VR call runs: off-thread exit, kind, the frame window as the route's step left it, the frustum (only when wanted), the plan, the census's report, the flush, the writes, the outcome -- in that order");
        check(!vrpre.empty() && count(cpp, "g_vr.finish(") == 1 && count(vrpre, "g_vr.finish(") == 1 && vrpre.rfind("g_vr.finish(") > vrpre.find("vrCommit(") &&
              vrpre.find("return;", vrpre.find("vrCommit(")) == std::string::npos,
              "R11k", "every owner-thread call reaches exactly one finish(): one call site, after the writes, with no return between the writes and it");
        check(!vrpre.empty() && vrpre.find("sehWrite") == std::string::npos && vrpre.find("noteInjected") == std::string::npos &&
              count(vrpre, "flushCamera(camera)") == 1 && vrpre.find("if (plan.flush) {") < vrpre.find("flushCamera(camera)"),
              "R11l", "refreshPreVr itself writes nothing: its only write is the flush, and only when the plan says the camera left the injected set");
        check(count(vrpre, "call.willInject = plan.inject;") == 1 && count(vrpre, "call.role = plan.roleKnown ? static_cast<uint8_t>(plan.role) : 255;") == 1 &&
              count(vrpre, "if (report && camera) {") == 1 && vrpre.find("if (report && camera) {") < vrpre.find("observer->pre(call)") &&
              count(vrpre, "wantPost = observer->pre(call) && observer->post != nullptr;") == 1,
              "R11m", "the census is told (when the frame observes and the camera is non-null) whether the call will be injected and in what role, and only then may it ask for the post half");
        check(count(vrpre, "call.window = in.gate == FlatCameraGateVerdict::Admit ? 0 : in.gate == FlatCameraGateVerdict::Expired ? 2 : 1;") == 1 &&
              count(cpp, "call.window = gate == FlatCameraGateVerdict::Admit ? 0 : gate == FlatCameraGateVerdict::Expired ? 2 : 1;") == 1,
              "R11n", "the observer's window field is computed the way observeCall computes it, from the window as the route's step left it");
        check(!vrpre.empty() && vrpre.find("if (report) observeOffThread(r0, camera);") != std::string::npos &&
              vrpre.find("g_vr.tally().noteOffThread();") < vrpre.find("if (report) observeOffThread(r0, camera);"),
              "R11o", "an off-thread call is counted lock-free and reported to the census's off-thread hook, and nothing else");
        // What the planner is told, statement by statement: a wrong kind, readability, mode or window here would inject a camera the
        // table would have refused.
        bool told = !vrpre.empty();
        for (const char* s : {"const bool report = flatCameraVrObserves(bits);", "const bool readable = camera && sehReadU32(camera + kCamKind, &kind);",
                              "in.camera = camera;", "in.readable = readable;", "in.kind = kind;", "in.mode = flatCameraVrModeOfBits(bits);", "in.callerRva = callerRva;",
                              "if (g_inject.gameBase && sehReadU64(r0, &ret) && ret > g_inject.gameBase) callerRva = ret - g_inject.gameBase;",
                              "FlatCameraVrCallIn in;"})
            if (count(vrpre, s) != 1) told = false;
        if (!ordered(vrpre, {"in.mode = flatCameraVrModeOfBits(bits);", "in.gate = flatCameraVrEffectiveGate(in.mode, gate, g_vr.stepAtMs(), GetTickCount64());"})) told = false;
        check(told, "R11ak", "the planner is told the call as it is: the camera, whether and what its kind read, the mode the word says, and the call site (each statement once)");
        bool reported = !vrpre.empty();
        for (const char* s : {"call.camera = camera;", "call.ctx = ctx;", "call.p2 = p2;", "call.callerRva = callerRva;", "call.callNo = callNo;", "call.kind = kind;",
                              "call.kindReadable = readable;", "FlatCameraObserveCall call;"})
            if (count(vrpre, s) != 1) reported = false;
        check(reported && count(vrpre, "if (flushCamera(camera)) g_vr.tally().noteFlushed(); else g_vr.tally().noteWriteFailure();") == 1,
              "R11al", "the census is given the call as the observe-only path gives it (camera, context, view, call site, call number, kind and its readability), and a flush that landed is counted as flushed, one that did not as a failed write");
    }
    {
        const size_t guard = commit.find("    if (!inject && !wantPost) return false;\n");
        const size_t firstWrite = std::min({commit.find("sehWriteF32("), commit.find("sehWriteU32("), commit.find("sehWriteU64("), commit.find("noteInjected"), commit.find("g_refreshTls.")});
        const size_t open = commit.find("{\n");
        const std::string guardLine = "    if (!inject && !wantPost) return false;\n";
        check(!commit.empty() && open != std::string::npos && commit.compare(open + 2, guardLine.size(), guardLine) == 0,
              "R11p", "vrCommit's first statement is 'if (!inject && !wantPost) return false;': a pass-through or observe-only call that asks for nothing ends there");
        check(guard != std::string::npos && firstWrite != std::string::npos && guard < firstWrite && count(commit, "if (!inject && !wantPost) return false;") == 2 &&
              commit.find("if (!inject && !wantPost) return false;", guard + 10) < commit.find("sehReadU64(r0, &realRet)"),
              "R11q", "no write is reachable from a call that neither injects nor asks for the census's post half: the guard precedes every write, and is repeated before the return-slot redirect");
        check(count(commit, "sehWriteU64(r0, static_cast<uint64_t>(g_stubB))") == 1 && commit.find("sehWriteU64(") > commit.rfind("sehWriteU32(camera + kCamFlags, flags | kFlagProj | kFlagVP)") &&
              commit.find("g_inject.injected.noteInjected(camera)") > commit.find("sehWriteU64(") && count(commit, "if (inject) g_inject.injected.noteInjected(camera);") == 1 &&
              commit.find("g_refreshTls.armed = 1u;") > commit.find("sehWriteU64("),
              "R11r", "the return-slot redirect is the last step (after the bound pair and the flag word), the TLS state is published after it, and the camera joins the flush set only when the phase landed");
        // The injection writes are the flat injector's own statements.
        const std::string flatRegion = region(cpp, "    // The VR camera census's switch: a process that observes only never leaves it (flatCameraInjectObserveFrame).\n", nullptr);
        const char* same[] = {"const bool wroteX = sehWriteF32(camera + kCamBoundX, jitX);", "const bool wroteY = wroteX && sehWriteF32(camera + kCamBoundY, jitY);",
                              "const bool wroteAll = wroteY && (!haveFlags || sehWriteU32(camera + kCamFlags, flags | kFlagProj | kFlagVP));",
                              "if (wroteX) sehWriteF32(camera + kCamBoundX, entryX);", "if (wroteY) sehWriteF32(camera + kCamBoundY, entryY);",
                              "sehWriteF32(camera + kCamBoundX, entryX);", "sehWriteF32(camera + kCamBoundY, entryY);", "if (haveFlags) sehWriteU32(camera + kCamFlags, flags);",
                              "!sehReadU64(r0, &realRet) || !sehWriteU64(r0, static_cast<uint64_t>(g_stubB))"};
        bool allSame = !flatRegion.empty() && !commit.empty();
        for (const char* s : same) if (count(flatRegion, s) == 0 || count(commit, s) == 0) allSame = false;
        check(allSame && count(commit, "flags | kFlagProj | kFlagVP") == 1 && count(flatRegion, "flags | kFlagProj | kFlagVP") == 1,
              "R11s", "the VR injection writes exactly what the flat injector writes: the same bound-pair writes, the same flag word with bits 4 and 8, the same rollback and the same return redirect");
        check(count(commit, "g_vr.bound(entryX, entryY, &jitX, &jitY);") == 1 && count(commit, "flatRuntime") == 0 && count(commit, "kFlagProj | kFlagVP") == 1,
              "R11t", "the bound pair comes from the VR frame's state (g_vr.bound), not from the flat runtime's phase");
        // The TLS state the post half reads: the same fields the flat injector saves, plus the three VR flags; each statement once.
        bool saved = !commit.empty();
        for (const char* s : {"g_refreshTls.realRet = realRet;", "g_refreshTls.ctx = ctx;", "g_refreshTls.camera = camera;", "g_refreshTls.callNo = callNo;",
                              "g_refreshTls.entryX = entryX;", "g_refreshTls.entryY = entryY;", "g_refreshTls.flags = flags;", "g_refreshTls.haveFlags = haveFlags ? 1u : 0u;",
                              "g_refreshTls.armed = 1u;", "g_refreshTls.vr = 1u;", "g_refreshTls.vrReport = wantPost ? 1u : 0u;", "g_refreshTls.vrRestore = inject ? 1u : 0u;"})
            if (count(commit, s) != 1) saved = false;
        for (const char* s : {"g_refreshTls.realRet = realRet;", "g_refreshTls.entryX = entryX;", "g_refreshTls.entryY = entryY;", "g_refreshTls.flags = flags;",
                              "g_refreshTls.haveFlags = haveFlags ? 1u : 0u;", "g_refreshTls.armed = 1u;"})
            if (count(region(cpp, "    // The VR camera census's switch: a process that observes only never leaves it (flatCameraInjectObserveFrame).\n", nullptr), s) != 1) saved = false;
        check(saved, "R11am", "vrCommit saves what the post half restores from, exactly as the flat injector does (the entry bound pair, the flag word and whether it was read), and marks the call VR, reporting and restoring as asked");
        // The failure branches: each failed read or write ends the injection, is counted, and leaves the body to run pristine.
        check(count(commit, "inject = false;") == 2 && count(commit, "g_vr.tally().noteWriteFailure();") == 3 && count(commit, "return false;") == 3 &&
              count(commit, "return inject;") == 1 && count(commit, "if (!wroteAll) { // roll back whatever landed; the body runs pristine") == 1,
              "R11an", "a failed bound read, a failed write and a failed redirect each count one write failure; the first two end the injection and the third undoes it and returns false");
    }
    {
        const size_t postCensus = vrpost.find("observer->post(camera, g_refreshTls.ctx);");
        const size_t restore = vrpost.find("sehWriteF32(camera + kCamBoundX, g_refreshTls.entryX);");
        check(!vrpost.empty() && postCensus != std::string::npos && restore != std::string::npos && postCensus < restore &&
              vrpost.find("sehWrite") == restore && count(vrpost, "sehWriteF32(camera + kCamBoundY, g_refreshTls.entryY);") == 1 &&
              count(vrpost, "sehWriteU32(camera + kCamFlags, g_refreshTls.flags)") == 1 && count(vrpost, "if (g_refreshTls.vrReport) {") == 1 && count(vrpost, "if (g_refreshTls.vrRestore) {") == 1 &&
              vrpost.find("if (g_refreshTls.vrReport) {") < vrpost.find("if (g_refreshTls.vrRestore) {"),
              "R11u", "the post half tells the census BEFORE it restores (the derived projection still carries the phase), then restores the bound pair and the flag word the call saved, and only when the phase landed");
        check(lacksAll(vrpost, {"flatRuntime", "injectedCalls", "observeRayCb", "Log::"}), "R11v", "the VR post half asks the flat runtime nothing and logs nothing");
    }
    {   // No call-path I/O, no allocation, no flat state: the 2026-09-28 crash lesson.
        const char* forbidden[] = {"Log::", "printf", "fopen", "fwrite", "OutputDebugString", "new ", "malloc", "std::string", "std::vector", "std::mutex", "lock_guard",
                                   "flatRuntime", "g_inject.core", "observeOnly", "g_inject.census", "kind3Seen", "Sleep(", "WaitFor"};
        bool clean = true;
        for (const std::string* body : {&vrpre, &commit, &vrpost, &frustum}) {
            if (body->empty()) clean = false;
            for (const char* f : forbidden) if (body->find(f) != std::string::npos) clean = false;
        }
        check(clean, "R11w", "refreshPreVr, vrCommit, refreshPostVr and readFrustum do no I/O, allocate nothing, take no lock and ask the flat runtime and the flat census nothing");
        check(count(cpp, "flatcpu::Scope timed(flatcpu::kInject);") == 2 && count(cpp, "flatRuntimePhaseState(&jx, &jy, &rw, &rh, &applied);") == 1 &&
              count(cpp, "flatRuntimeNoteCameraApplied();") == 1,
              "R11x", "the CPU timer is still in the detour's two halves and the VR path added none; the flat runtime is asked for its phase and told of an application in one place each, both the flat path's");
        check(count(frustum, "out.aspect = aspect; out.fov = fov; out.nearZ = nearZ; out.farZ = farZ;") == 1, "R11ao", "each of the four fields read lands in its own field of the frustum the role test is given");
        check(!frustum.empty() && count(frustum, "sehReadF32(") == 4 && frustum.find("sehReadF32(camera + kCamAspect, &aspect)") != std::string::npos &&
              frustum.find("sehReadF32(camera + kCamFov, &fov)") != std::string::npos && frustum.find("sehReadF32(camera + kCamNear, &nearZ)") != std::string::npos &&
              frustum.find("sehReadF32(camera + kCamFar, &farZ)") != std::string::npos,
              "R11y", "the four frustum fields are read through the SEH-guarded reader, each once, and the whole frustum stays NaN when any fails");
    }
    {   // The offsets are the census's (vr_camera_census_core.h).
        char want[160];
        std::snprintf(want, sizeof want, "constexpr uint32_t kCamNear = 0x%X, kCamFar = 0x%X, kCamAspect = 0x%X, kCamFov = 0x%X;", 0x254u, 0x258u, 0x260u, 0x280u);
        check(count(cpp, want) == 1 && count(censusCore, "constexpr uint32_t kVrCensusNear = 0x254;") == 1 && count(censusCore, "constexpr uint32_t kVrCensusFar = 0x258;") == 1 &&
              count(censusCore, "constexpr uint32_t kVrCensusAspect = 0x260;") == 1 && count(censusCore, "constexpr uint32_t kVrCensusFov = 0x280;") == 1 &&
              count(censusCore, "constexpr uint32_t kVrCensusKind = 0x264;") == 1 && count(cpp, "constexpr uint32_t kCamKind = 0x264;") == 1 &&
              count(cpp, "constexpr uint32_t kCamBoundX = 0x28C;") == 1 && count(cpp, "constexpr uint32_t kCamBoundY = 0x290;") == 1 && count(cpp, "constexpr uint32_t kCamFlags = 0x250;") == 1,
              "R11z", "the field offsets the role test reads are the census's table: aspect +0x260, near +0x254, far +0x258, fov +0x280, kind +0x264");
    }

    // ---- the frame step, the census's step, quiet, the install ------------------------------------------------------------------
    const std::string vf = functionBody(cpp, "bool flatCameraVrFrame(const FlatCameraVrFrame& frame) {");
    const std::string of = functionBody(cpp, "bool flatCameraInjectObserveFrame() {");
    {
        check(!vf.empty() &&
              ordered(vf, {"g_vrFailures.note(now, g_vr.tally().snapshot().writeFailures)", "standDown(why);", "g_inject.gate.disarm(GetCurrentThreadId());", "g_vr.beginFrame(frame, now);",
                           "const bool observe = frame.observe || g_observer.load(std::memory_order_acquire) != nullptr;",
                           "g_vrBits.store(flatCameraVrBitsForFrame(frame.inject, observe, true)", "installRefreshHook(observe,",
                           "g_vrBits.store(flatCameraVrBitsForFrame(frame.inject, observe, live)", "if (!live) return false;", "g_inject.gate.arm(GetTickCount64());",
                           "flatCameraInjectPause(flatCameraVrQuiet());", "return true;"}),
              "R11aa", "the route's frame step: the last frame's failures feed the stand-down, the window closes and this thread owns it, the frame state is set, a registered census observer counts as observing, the mode word is published BEFORE the install, then again with the hook's liveness, the window re-arms, the relay gate follows quiet");
        check(count(vf, "g_vrBits.store(") == 3 && count(vf, "!g_inject.permanentlyDown.load(std::memory_order_acquire)") == 1 && count(vf, "if (g_inject.relay) {") == 1 &&
              vf.find("return false;") != std::string::npos && count(vf, "flatRuntime") == 0 && count(vf, "g_inject.core") == 0 && count(vf, "observeOnly") == 0,
              "R11ab", "a hook that failed for good, never installed or stood down honours neither mode (inject is not honoured, the step returns false); the step never touches the flat runtime, the flat ownership machine or the observe-only switch");
        check(!of.empty() && ordered(of, {"g_inject.observeOnly.store(true, std::memory_order_release);",
                                         "g_vrBits.store(flatCameraVrBitsAfterCensusFrame(g_vrBits.load(std::memory_order_acquire)), std::memory_order_release);",
                                         "installRefreshHook(true);", "g_inject.gate.arm(GetTickCount64());"}) && count(of, "g_vrBits") == 2,
              "R11ac", "the census's frame step applies the mode word's rule (an injection frame keeps its mode) after the observe-only switch and before the install, then arms the window as it did");
        check(count(cpp, "bool flatCameraVrQuiet() { return flatCameraVrQuietFor(g_vrBits.load(std::memory_order_acquire), g_inject.injected.empty()); }") == 1 &&
              count(cpp, "void flatCameraVrCloseWindow() { g_vr.closeWindow(); }") == 1 && count(cpp, "FlatCameraVrCounters flatCameraVrCounters() { return g_vr.tally().snapshot(); }") == 1 &&
              count(cpp, "size_t flatCameraVrExcluded(FlatCameraVrExcluded* out, size_t max) { return g_vr.excluded().copy(out, max); }") == 1 &&
              count(cpp, "const char* flatCameraVrStatus() { return flatCameraInjectObserveStatus(); }") == 1,
              "R11ad", "quiet is the mode word and the injected set's emptiness; the accessors are one line each over the planner's state");
        check(count(cpp, "flat camera inject: refresh hook installed at EliteDangerous64.exe+0x%llX for the VR world route ") == 1 && count(cpp, "(asked for by %s)") == 1 &&
              count(cpp, "in OBSERVE-ONLY mode ") == 1 && count(cpp, "kind-3 cameras now get the temporal phase applied transiently at the source") == 1 &&
              count(cpp, "void installRefreshHook(bool observe, const char* vrAsked = nullptr) {") == 1 && count(cpp, "installRefreshHook(observe, frame.inject ?") == 1,
              "R11ae", "the install note says which mode asked: the VR route's variant names what the frame asked for (injection, injection observed by the census, observation), and the flat and census notes are as they were");
    }
    // ---- the interface ---------------------------------------------------------------------------------------------------------
    {
        const char* quiet = "// True when the detour is quiet for the route's purposes: this frame asked for neither injection nor observation, and no camera this session injected is still waiting for its first un-injected call (its flush). The route stops calling flatCameraVrFrame once its key is off and this is true. False while there is no live hook only if a flush could still be pending; a hook that never installed is quiet.\nbool flatCameraVrQuiet();\n";
        check(count(inj, quiet) == 1 && count(inj, "const char* flatCameraVrStatus();\n// True when the detour is quiet") == 1,
              "R11af", "the header declares flatCameraVrQuiet() right after flatCameraVrStatus(), with the coordinator's exact text");
        check(count(inj, "bool flatCameraVrFrame(const FlatCameraVrFrame& frame);") == 1 && count(inj, "void flatCameraVrCloseWindow();") == 1 &&
              count(inj, "FlatCameraVrCounters flatCameraVrCounters();") == 1 && count(inj, "size_t flatCameraVrExcluded(FlatCameraVrExcluded* out, size_t max);") == 1 &&
              count(inj, "const char* flatCameraVrStatus();") == 1 && count(inj, "bool willInject = false;") == 1 && count(inj, "uint8_t role = 255;") == 1 &&
              count(inj, "uint32_t injectedKind[8] = {};") == 1 && count(inj, "uint32_t notActive = 0;") == 1 && count(inj, "uint32_t flushed = 0;") == 1,
              "R11ag", "the interface the route codes against is declared as the brief has it, plus the two counters appended after it (notActive, flushed)");
        const size_t appended = inj.find("uint32_t notActive = 0;");
        check(appended != std::string::npos && appended > inj.find("uint32_t injectedKind[8] = {};") && appended > inj.find("uint32_t offThread = 0;") && appended > inj.find("uint32_t stale = 0;"),
              "R11ah", "the new counters come after every declared field: nothing that names a declared field moves");
        // The pure header stays pure.
        bool pure = true;
        for (const char* banned : {"windows.h", "Windows.h", "d3d11", "Log::", "printf", "fopen", "fstream", "#include <mutex>", "#include <thread>", "new ", "malloc", "std::string", "std::vector", "GetTickCount", "__try"})
            if (vrh.find(banned) != std::string::npos) pure = false;
        check(pure && count(vrh, "#include \"flat_camera_inject.h\"") == 1 && count(vrh, "#include \"flat_camera_phase.h\"") == 1 && count(vrh, "#include") == 7,
              "R11ai", "flat_camera_vr.h is pure: five standard headers and the two local ones, no OS, D3D, I/O, allocation or lock");
        check(count(vrh, "flatCameraAdmit(") == 2 && vrh.find("flatCameraVrBaseInput(in, true)") != std::string::npos && vrh.find("flatCameraVrBaseInput(in, in.phaseNonzero)") != std::string::npos &&
              count(vrh, "FlatCameraGateVerdict::") >= 3 && count(vrh, "in.kind == 3") == 0 && count(vrh, "kind == 3") == 0 && count(vrh, "kind == 4") == 0 && count(vrh, "kind == 5") == 0,
              "R11aj", "the VR admission reuses flatCameraAdmit for the kind, the gate and the observe logic and never spells a kind itself");
    }
}

int runSelfTest() {
    testRole();
    testWeaponRole();
    testAdmission();
    testInjectedKind();
    testOutcomes();
    testReuseScenario();
    testFlush();
    testExcluded();
    testModeWord();
    testArithmetic();
    testFlatTable();
    testSourcePins();
    if (g_failures == 0) {
        std::printf("flat camera vr: PASS\n");
        return 0;
    }
    std::printf("flat camera vr: %d FAILED check(s)\n", g_failures);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--self-test") == 0) {
        if (argc >= 3) g_root = argv[2];
        return runSelfTest();
    }
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("flat camera vr test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: flat_camera_vr_test --self-test [root]|--dry-run\n");
    return 2;
}
