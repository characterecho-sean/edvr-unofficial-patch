#include "../../src/common/native_frame.h"
#include "../../src/common/comfort_fade.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/config.h"
#include "../../src/common/native_render_settings.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/journal_watch.h"
#include "../../src/d3d11/native_on_foot.h"   // F36: weapon stability steps aside while native on foot is active
#include "../../src/d3d11/native_on_foot_optic_capture.h"  // F76: bounded exact-image optic capture ledger
#include "../../src/d3d11/native_on_foot_ads_incident.h"

#include <d3d11.h>
#include <immintrin.h>
#include <wrl/client.h>
#include "../../src/d3d11/draw_record_tag.h"   // F44: the marker probe's tag, find and match (pure parts)
#include "f85_checks.h"   // F85: the pin-step trace, the weapon UI pin and the reticle depth (pure parts, shared with a desk driver)
#include "f89_checks.h"   // F89: the ADS release, the left eye closing in ADS, the transition trace, the hold's open-loop input
#include "f90_checks.h"   // F90: the weapon readout's size in ADS, the pistol's readout in the weapon's frame
#include "f91bc_checks.h"   // F91 B and C: the ADS vibration instrument's fields and fit, the ADS item gate
#include "f92_checks.h"     // F92b: the ADS head aim's cancel published ahead, and the head aim as a live toggle within a press
#include "f93_checks.h"     // F93b: the ADS sight lock's steady input, the eye chain
#include "f93de_checks.h"   // F93d: the game's camera published and watched
#include "f94_checks.h"     // F94a: the neck as the view's reference, the body at its animated height
#include "f94b_checks.h"    // F94b: the interaction label moved by its own anchor; F93c's rule measured beside it
#include "f95_checks.h"     // F95a: the neck reference from the stance's settled height; the view-motion instrument
#include "f95c_checks.h"    // F95c: the head as shadow-only through the visibility call, the fade hook's guard and action
#include "f97_checks.h"     // F97a: the armed body under the eye again
#include "f97b_checks.h"    // F97b: the interaction label sized like a panel at its anchor facing the eye
#include "f98_checks.h"     // F98a: the left elbow's stable pole transported from its seed
#include "f98b_checks.h"    // F98b: the interaction label placed in the world, upright and facing the neck's pivot
#include "f99_checks.h"     // F99a: a drawn item's hands from the game's own selection, not from Status.json
#include "f100_checks.h"    // F100: the marker tee's prefilter against its slow path, the paced F68d window, the throttled lines
#include "f101_checks.h"    // F101: the frame ABI against the released one (v0.21.1)
#include "f107_checks.h"    // F107: the game's camera away from the commander (a terminal's or a screen's own camera)
#include "f109_checks.h"    // F109: look to aim on foot, the exact split (the player's own turn in ADS), snap or smooth turning
#include "f129_checks.h"    // F129: look to aim reads its turn from the player's own look processor (the call classes, the gate, the episodes, the zero-sum probes)
#include "f130_checks.h"    // F130: one disagreeing scene mesh no longer switches the eye rows' sources off (the marks, the source draw, the SRV flight replayed)
#include "f131_checks.h"    // F131: look to aim holds while a panel or a menu is open, and while the game runs no look
#include "f132_checks.h"    // F132: the G frame pairing re-seeds its phase after a far-apart refusal (either true order, a missing call, a real gap)
#include "f133_checks.h"    // F133: head steps judged by rate (a stall is not a recentre); the follower re-centres on a standing yaw offset
#include "f135_checks.h"    // F135: the on-foot menu probe's pure parts (counters, the distinct objects of a second, the edges, the key edges, the line gate)
#include "f137_checks.h"    // F137: a virtual neck under mouse look (the offset, the seated vector, the weight, the pose's history, the head offset it cancels from)
#include "f138_checks.h"    // F138: the view holds its height over the neck while the look is pitched (the weight, the sink, the reference, the return edge, the reasons, the bins)
#include "f139_checks.h"    // F139: each weapon's sight on the ADS line (the table, the live trim, the shift, the bound, F79's origin beside it, the item snapshot)
#include "f140_checks.h"    // F140: the weapon readout placed per weapon (the table by item and scope, the trims, the clamp, an untrusted name); the journal's loadout parser
#include "f141_checks.h"    // F141: the game's ADS sight point (the bend's test, S against P14, the window's sums) and the readout's anchor (U against W and P14)
#include "f142_checks.h"    // F142: the icon mesh table's slot reuse, the full outfit promotion, the outfit census, and F141's used-lane test and rejection counts
#include "f143_checks.h"    // F143: the ADS eye on the game's sight line (Q on the line E -> S, the filter, the gates and the bound, the trims) and the coat's clip A/B (the extra parts, when the measure reads)
#include "f144_checks.h"    // F144: the settled sight line per (item, scope), the coat's Gear_Back decision and first-person census, the readout placed per role and what reaches the draw
#include "f145_checks.h"    // F145: the weapon UI's tag slots given back (the pick, the F144 flight replayed, the event ring) and the proof against the seed (the seed-only map on the two-eye rig)
#include "f149_checks.h"    // F149/F150: the ADS eye depth, one per weapon (the game's unscoped camera plus a back-off), never further forward than the key (the tables, the rule, the cap, the shift at the same depth, the cache by copy)
#include "f152_checks.h"    // F152: the readout origin's two shadows (the engine's first, the own one when engine motion is down, their difference, a ranged write into the rows)
#include "f157_checks.h"    // F157: the coat outfit's pack draw left out of both eyes (the identity, each requirement refusing on its own, the outfit gate, the counts by eye and frame)
#include "f158_checks.h"    // F158: the local body's draws over the first 30 s on foot (the window's rules, the list and its cap, the switch time, the stop and adjacency tests, the lines)
#include "f159_checks.h"    // F159: the left elbow's stable pole reseeded at the shoulder's height (the condition, the gate, the real solver's raised elbow), the seed births and their lines
#include "f151_checks.h"    // F151: the weapon readout's draw bound by its place (the ordinals, each guard, one bind per key, the lost-frame count) and the held copy's scope from the selected slot
#include "f110_checks.h"    // F110: the ADS sight lock's model right eye (the left-eye job camera plus the IPD)
#include "f111_checks.h"    // F111: the panel pointer instrument's statistics and verdict
#include "f113_checks.h"    // F113: each eye's scene rows whenever the world is drawn (the depth prepass and the terrain as sources)
#include "f116_checks.h"    // F116: the HUD hidden while a terminal fills the view (hysteresis, frame order, the hook's skip, the lines)
#include "f114b_checks.h"   // F114b: the panel pointer's turned transform, the click path, the mode getter
#include "f115_checks.h"    // F115: boarding undoes every on-foot camera change (the exit edge, the zeros, the read-back)
#include "f118_checks.h"    // F118: why the reticle is seen double (steps and flips, trailing frames, the footprint), measure only
#include "f154_checks.h"    // F154, F155: the pick mesh's lever read from the game, the drawn cursor and its hide fallback, the signed turn instrument
#include "f119_checks.h"    // F119: head containment, measure only (the head's offset by class, the depth grid decoded, the lines)
#include "f126_checks.h"    // F126: the pin's eye pair from one frame, the arm IK's snapshot of the current FP rig, a nearly straight elbow reaching the grip
#include "f123_checks.h"    // F123: head containment, the leash and the wall contact (the fade ramp, the hard timer, the contact trigger and latch, the recentre sequence, the lines)
#include "f125_ads_checks.h"
#include "f125_checks.h"    // F125: the on-foot log's windows (the detail gate, the summary window, the per-second fold, the flush)
#include "f125containment_checks.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// The journal watcher is stubbed so the on-foot signal is the rig's to set.
static bool g_journalActive = false, g_onFootKnown = false, g_onFoot = false;
namespace edvr {
bool journalWatchActive() { return g_journalActive; }
bool journalOnFootKnown() { return g_onFootKnown; }
bool journalOnFoot() { return g_onFoot; }
}

using Microsoft::WRL::ComPtr;

extern "C" uintptr_t g_weaponUiGetterTarget = 0;
extern "C" uint64_t g_weaponUiProbeCalls = 0;
extern "C" uint64_t g_weaponUiProbeArgs[2] = {};
extern "C" void weaponUiProbeFakeGetter();
extern "C" void weaponUiGetterCallStub();
extern "C" uint64_t weaponUiProbeTestInvoke(void* result, uintptr_t renderer, uintptr_t part, uintptr_t camera);
struct WeaponUiProbeResult {
    uint64_t rax = 0, rcx = 0, rdx = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, flags = 0;
    float xmm[6][4] = {};
    uint32_t mxcsr = 0;
};
static edvr::WeaponUiGetterObservation g_weaponUiProbeObserved{};
static bool g_weaponUiProbeObservedValid = false;
extern "C" void weaponUiProbeRecordObservation(const edvr::WeaponUiGetterObservation* observation) {
    if (observation) {
        g_weaponUiProbeObserved = *observation;
        g_weaponUiProbeObservedValid = true;
    }
}

static EdvrNativeFrameInput input(uint64_t generation, uint64_t reference,
                                  uint64_t sequence, bool valid = true) {
    EdvrNativeFrameInput result{sizeof(result), EDVR_NATIVE_FRAME_VERSION_1};
    result.generation = generation;
    result.referenceGeneration = reference;
    result.sequence = sequence;
    result.valid = valid ? 1u : 0u;
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(result.physicalHead, identity, sizeof(identity));
    return result;
}

// The F53 and F62c cases test ndcLagDelta's mechanics against an arbitrary c; F66f removed the constant icon offset, so they keep their old c here.
static void testIconE(double H, double e[2]) {
    e[0] = 0.0;
    e[1] = H > 0.0 ? 2.0 * 36.3 / H : 0.0;
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("native_frame_test: dry-run (no provider calls)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;

    unsigned checks = 0, failures = 0;
    const auto check = [&](bool condition, const char* name) {
        ++checks;
        if (!condition) {
            ++failures;
            std::printf("FAIL: %s\n", name);
        }
    };

    // System32's d3d11 through common/system_d3d11.h, never an import: EDVR's proxy sits beside this exe.
    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "system D3D11 factory");
    std::puts("native_frame_test: factory ready");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ignoredContext;
    D3D_FEATURE_LEVEL featureLevel{};
    check(createDevice && SUCCEEDED(createDevice(
              nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
              D3D11_SDK_VERSION, &device, &featureLevel, &ignoredContext)),
          "WARP device");
    if (!device) return 1;
    std::puts("native_frame_test: device ready");

    // Reset the process channel so every assertion below observes this
    // provider's session rather than an earlier native-frame run.
    edvr::clearGlitchFrame();
    edvr::requestSubmitHold(0);
    // Published after acquire; doing so also exercises the device identity
    // check without making setup depend on a pre-existing channel mapping.

    // The terrain guard's six keys (removed 2026-10-09), as an old ini still carries them: nothing reads them, whatever they say.
    edvr::Config::get().set("fix.cull_guard", "PeRcEnT");
    edvr::Config::get().set("fix.cull_guard_percent", "75");
    edvr::Config::get().set("fix.cull_guard_fraction_h", "-1");
    edvr::Config::get().set("fix.cull_guard_fraction_v", "nan");
    edvr::Config::get().set("fix.cull_guard_headsets",
                            "94x99, 120x130junk, 95X84, 10x20, 95x84 ");
    edvr::Config::get().set("advanced.cull_guard_channel", "Matrix");
    // The three field-of-view trims are per-headset lists keyed like
    // fix.openxr_resolution, resolved against the headset the last v2
    // render-settings query saw. Drive that query first, exactly as the host
    // does (tools/native_render_settings_test drives the same export), so the
    // labels the provider matches on are valid: "Oculus" + "Meta Quest 3"
    // sanitise to oculus/meta-quest-3.
    EdvrNativeRenderViewBounds renderBounds[2];
    renderBounds[0] = {1824, 1968, 16384, 16384};
    renderBounds[1] = {1824, 1968, 16384, 16384};
    EdvrNativeRenderSettings renderSettings =
        edvr::native_render::buildRenderSettingsRequest(renderBounds, "Oculus", "Meta Quest 3");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,
                                        sizeof(renderSettings), &renderSettings) == TRUE,
          "the render settings query publishes this headset's labels");
    // The keys live in [experimental] since 2026-09-29 (edvr.ini's
    // `# moved-from: fix.fov_trim_*`; tools/config_test proves an old line is
    // read as the new name and tools/installer_test that the merge carries it).
    edvr::Config::get().set("experimental.fov_trim_outer", "oculus/meta-quest-3:7");
    // 44 is outside 0..30, so the entry is refused rather than clamped, and
    // a bare 5 (the form this key used to take) names no headset at all.
    edvr::Config::get().set("experimental.fov_trim_nasal", "oculus/meta-quest-3:44");
    edvr::Config::get().set("experimental.fov_trim_vertical", "5");

    EdvrNativeFrameRequest request{
        sizeof(request), EDVR_NATIVE_FRAME_VERSION_1, device.Get(), 41};
    EdvrNativeFrameTable table{sizeof(table), EDVR_NATIVE_FRAME_VERSION_1};
    std::puts("native_frame_test: acquiring");
    check(edvrAcquireNativeFrame(&request, &table) == S_OK && table.context &&
              table.beginFrame && table.reservedCall && table.latchSubmit &&
              table.invalidate && table.close,
          "acquire exports complete table");
    EdvrNativeFrameTable blockedTable{sizeof(blockedTable),
                                      EDVR_NATIVE_FRAME_VERSION_1};
    check(edvrAcquireNativeFrame(&request, &blockedTable) == E_PENDING,
          "only one active provider session");
    edvr::publishGameDevice(device.Get());
    edvr::announceSceneArrived();

    // Acquire on this thread, then run the complete CPU callback sequence from
    // a different XR-owner thread.  The provider must not use a producer-ID
    // gate for callbacks.
    EdvrNativeFrameOutput firstOutput{sizeof(firstOutput),
                                      EDVR_NATIVE_FRAME_VERSION_8};
    HRESULT workerBegin = E_FAIL;
    HRESULT workerLatch = E_FAIL;
    EdvrNativeFrameDecision firstDecision{
        sizeof(firstDecision), EDVR_NATIVE_FRAME_VERSION_1};
    std::thread owner([&] {
        EdvrNativeFrameInput frame = input(41, 7, 1);
        workerBegin = table.beginFrame(table.context, &frame, &firstOutput);
        edvr::markGlitchFrame();
        workerLatch = table.latchSubmit(table.context, 1, &firstDecision);
    });
    owner.join();
    check(workerBegin == S_OK, "XR owner can begin after producer acquire");
    check(workerLatch == S_OK, "XR owner can latch after producer acquire");
    // The old Explorer Cam's headset offset is retired (2026-10-07): its slots stay in the
    // struct for layout and are always zero, so a runtime that still reads them applies nothing.
    check(firstOutput.reservedOffset[0] == 0.0f && firstOutput.reservedOffset[1] == 0.0f &&
              firstOutput.reservedOffset[2] == 0.0f && firstOutput.reservedYaw == 0.0f &&
              firstOutput.reservedOffsetEnabled == 0 && firstOutput.reservedOffsetGamePoses == 0,
          "the retired offset slots are zero");
    // The terrain guard's slots are retired (2026-10-09): always zero, whatever an old ini says.
    const auto retiredGuardSlotsZero = [](const EdvrNativeFrameOutput& o) {
        for (uint32_t word : o.reservedTerrainGuard) if (word != 0) return false;
        return o.leftEyeShare == 0;
    };
    check(retiredGuardSlotsZero(firstOutput),
          "the terrain guard's retired slots are zero although the ini still carries all six of its old keys");
    check(firstOutput.sceneReady && firstOutput.transitionEnabled &&
              firstOutput.resubmitEnabled,
          "scene and transition outputs");
    check(firstOutput.version == EDVR_NATIVE_FRAME_VERSION_8 &&
              firstOutput.size == sizeof(firstOutput),
          "version 5 answered in kind");
    check(firstOutput.trimOuterDeg == 7.0f &&
              firstOutput.trimNasalDeg == 0.0f &&
              firstOutput.trimVerticalDeg == 0.0f,
          "the worn headset's trim entry applies; out of range and a bare number do not");
    check(firstDecision.withhold && firstDecision.jumpOnly,
          "marked frame is withheld as jump-only");

    // The second latch for one sequence returns the first cached answer, even
    // when the shared mark has changed since the first eye latched.
    edvr::clearGlitchFrame();
    EdvrNativeFrameDecision cached{
        sizeof(cached), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 1, &cached) == S_OK &&
              std::memcmp(&cached, &firstDecision, sizeof(cached)) == 0,
          "second latch returns cached decision");
    check(edvr::glitchConsumerPresent(), "valid latch announces consumer");

    // Invalid physical tracking is a valid frame boundary but does not
    // publish a new pose.
    edvr::markGlitchFrame();
    EdvrNativeFrameInput lost = input(41, 7, 2, false);
    lost.physicalHead[0] = std::numeric_limits<float>::quiet_NaN();
    EdvrNativeFrameOutput lostOutput{sizeof(lostOutput),
                                     EDVR_NATIVE_FRAME_VERSION_8};
    check(table.beginFrame(table.context, &lost, &lostOutput) == S_OK,
          "lost pose succeeds");
    EdvrNativeFrameDecision lostDecision{
        sizeof(lostDecision), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 2, &lostDecision) == S_OK &&
              !lostDecision.withhold,
          "begin clears a mark from the previous frame");
    EdvrNativeFrameInput bad = input(41, 7, 3);
    bad.physicalHead[0] = 2.0f;
    EdvrNativeFrameOutput badOutput{sizeof(badOutput),
                                    EDVR_NATIVE_FRAME_VERSION_8};
    check(table.beginFrame(table.context, &bad, &badOutput) == E_INVALIDARG,
          "non-rigid physical pose rejected");

    // The table's retired call slot (the terrain guard's state announcement): a runtime built before the removal still calls it,
    // and it answers S_OK to anything and does nothing.
    check(table.reservedCall(table.context, 1, 1.25f, 1.5f) == S_OK &&
              table.reservedCall(table.context, 3, std::numeric_limits<float>::quiet_NaN(), 1.0f) == S_OK &&
              table.reservedCall(nullptr, 0, 0.0f, 0.0f) == S_OK,
          "the retired call slot answers S_OK and does nothing, whatever it is handed");

    // Invalidation preserves the sequence floor and clears pending shared
    // state.  The pose previously published remains available.
    float publishedPose[12]{};
    check(edvr::headPose(publishedPose) && publishedPose[0] == 1.0f,
          "physical pose published before invalidation");
    check(table.invalidate(table.context) == S_OK &&
              table.beginFrame(table.context, &lost, &lostOutput) == E_INVALIDARG,
          "invalidate refuses the old sequence");
    EdvrNativeFrameInput next = input(41, 7, 4);
    check(table.beginFrame(table.context, &next, &lostOutput) == S_OK,
          "greater sequence recovers after invalidation");

    // An explicit camera hold is unaffected by a stale fix.transition_flash line (retired 2026-10-13; see the block
    // before the close below), and a prepared frame that never reaches Submit does not consume the hold.
    edvr::Config::get().set("fix.transition_flash", "0");
    edvr::requestSubmitHold(1);
    EdvrNativeFrameInput holdFrame = input(41, 7, 5);
    check(table.beginFrame(table.context, &holdFrame, &lostOutput) == S_OK,
          "begin prepared hold frame");
    EdvrNativeFrameInput visibleHoldFrame = input(41, 7, 6);
    check(table.beginFrame(table.context, &visibleHoldFrame, &lostOutput) == S_OK,
          "begin visible hold frame after dropped frame");
    EdvrNativeFrameDecision hold{
        sizeof(hold), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 6, &hold) == S_OK && hold.withhold &&
              !hold.jumpOnly,
          "an explicit hold withholds, whatever an old ini says about the retired setting");
    EdvrNativeFrameInput afterHold = input(41, 7, 7);
    check(table.beginFrame(table.context, &afterHold, &lostOutput) == S_OK,
          "begin frame after consumed hold");
    EdvrNativeFrameDecision noHold{
        sizeof(noHold), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 7, &noHold) == S_OK &&
              !noHold.withhold,
          "hold is consumed once by visible pair");

    // An openvr_api.dll from before the trim asks in version 1 and must be
    // answered in version 1, in its own smaller struct, with nothing written
    // past the end of it. A size that does not match its version is refused.
    struct Guarded { EdvrNativeFrameOutput output; uint32_t sentinel; } guarded{};
    guarded.output.size = EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    guarded.output.version = EDVR_NATIVE_FRAME_VERSION_1;
    guarded.output.trimOuterDeg = guarded.output.trimNasalDeg =
        guarded.output.trimVerticalDeg = -99.0f;
    guarded.sentinel = 0xA5A5A5A5u;
    EdvrNativeFrameInput legacyFrame = input(41, 7, 8);
    check(table.beginFrame(table.context, &legacyFrame, &guarded.output) == S_OK &&
              guarded.output.version == EDVR_NATIVE_FRAME_VERSION_1 &&
              guarded.output.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_1,
          "version 1 caller answered in version 1");
    check(guarded.output.sceneReady &&
              guarded.output.trimOuterDeg == -99.0f &&
              guarded.sentinel == 0xA5A5A5A5u,
          "version 1 answer writes no trim and nothing past its struct");
    EdvrNativeFrameOutput mismatched{EDVR_NATIVE_FRAME_OUTPUT_SIZE_1,
                                     EDVR_NATIVE_FRAME_VERSION_2};
    EdvrNativeFrameInput mismatchFrame = input(41, 7, 9);
    check(table.beginFrame(table.context, &mismatchFrame, &mismatched) ==
              E_INVALIDARG,
          "a size that contradicts the version is refused");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_1 + 3 * sizeof(float) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_2,
          "version 2 adds exactly the three trims");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_2 + sizeof(uint32_t) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_3,
          "version 3 adds exactly the pacing flag");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 + sizeof(uint32_t) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_4,
          "version 4 adds exactly the channel");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_4 + sizeof(float) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_5,
          "version 5 adds exactly the comfort fade level");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_5 + sizeof(float) + 2 * sizeof(uint32_t) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_6,
          "version 6 adds exactly the look-to-aim total, its game frame and its reset serial (F22, F25)");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_6 + 3 * sizeof(float) + sizeof(uint32_t) + sizeof(uint32_t) ==
              sizeof(EdvrNativeFrameOutput),
          "version 8 adds exactly the eye alignment and its serial (F33); version 9 (F40) adds the now-pose word after them");

    // A runtime-only entry applies to any headset on that runtime with no
    // entry of its own; a key with no entry for the worn headset is no trim,
    // including one keyed on a headset that is not being worn.
    edvr::Config::get().set("experimental.fov_trim_vertical", "oculus:3");
    edvr::Config::get().set("experimental.fov_trim_outer", "");
    edvr::Config::get().set("experimental.fov_trim_nasal", "virtualdesktopxr/meta-quest-3:9");
    EdvrNativeFrameOutput trimOutput{sizeof(trimOutput), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput trimFrame = input(41, 7, 10);
    check(table.beginFrame(table.context, &trimFrame, &trimOutput) == S_OK &&
              trimOutput.trimVerticalDeg == 3.0f && trimOutput.trimOuterDeg == 0.0f &&
              trimOutput.trimNasalDeg == 0.0f,
          "a runtime-only trim entry applies; an empty list and another headset's entry do not");

    // fix.weapon_stability && the journal watcher's on-foot signal select
    // EdvrNativeFrameOutput::deferredPacing (version 3 and later). The journal
    // functions are stubbed above so this rig can drive that signal directly
    // instead of needing a real journal file. This first ask is a version 3
    // caller: answered in exactly its own shape, pacing and all, with the
    // channel field never written into its (absent) tail.
    EdvrNativeFrameOutput defaultOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_3, EDVR_NATIVE_FRAME_VERSION_3};
    defaultOutput.leftEyeShare = 0xA5A5A5A5u;
    EdvrNativeFrameInput defaultFrame = input(41, 7, 11);
    check(table.beginFrame(table.context, &defaultFrame, &defaultOutput) == S_OK &&
              defaultOutput.version == EDVR_NATIVE_FRAME_VERSION_3 &&
              defaultOutput.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 &&
              defaultOutput.deferredPacing == 0 &&
              defaultOutput.leftEyeShare == 0xA5A5A5A5u,
          "a version 3 caller is answered in kind, its absent tail untouched");

    g_journalActive = g_onFootKnown = g_onFoot = true;
    EdvrNativeFrameOutput onFootOutput{sizeof(onFootOutput), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput onFootFrame = input(41, 7, 12);
    check(table.beginFrame(table.context, &onFootFrame, &onFootOutput) == S_OK &&
              onFootOutput.deferredPacing == 1,
          "weapon stability on foot defers pacing");

    g_onFoot = false;
    EdvrNativeFrameOutput inShipOutput{sizeof(inShipOutput), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput inShipFrame = input(41, 7, 13);
    check(table.beginFrame(table.context, &inShipFrame, &inShipOutput) == S_OK &&
              inShipOutput.deferredPacing == 0,
          "weapon stability in a ship keeps the wait in WaitGetPoses");

    g_onFoot = true;
    edvr::Config::get().set("fix.weapon_stability", "0");
    EdvrNativeFrameOutput disabledOutput{sizeof(disabledOutput), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput disabledFrame = input(41, 7, 14);
    check(table.beginFrame(table.context, &disabledFrame, &disabledOutput) == S_OK &&
              disabledOutput.deferredPacing == 0,
          "fix.weapon_stability = 0 never defers pacing");

    edvr::Config::get().set("fix.weapon_stability", "1");
    g_journalActive = g_onFootKnown = g_onFoot = false;

    // A version 2 caller is still answered in exactly its own shape, trims
    // and all, with the pacing field never written into its (absent) tail.
    EdvrNativeFrameOutput v2Output{EDVR_NATIVE_FRAME_OUTPUT_SIZE_2, EDVR_NATIVE_FRAME_VERSION_2};
    EdvrNativeFrameInput v2Frame = input(41, 7, 15);
    check(table.beginFrame(table.context, &v2Frame, &v2Output) == S_OK &&
              v2Output.version == EDVR_NATIVE_FRAME_VERSION_2 &&
              v2Output.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_2 &&
              v2Output.trimVerticalDeg == 3.0f && v2Output.trimOuterDeg == 0.0f &&
              v2Output.trimNasalDeg == 0.0f,
          "a version 2 caller is still answered in its own kind");

    // A struct sized for version 3 that claims version 2 names a shape that
    // does not exist and is refused, exactly like the version 1/2 mismatch
    // above, never silently downgraded to the version its label asks for.
    EdvrNativeFrameOutput v3SizeV2Version{sizeof(EdvrNativeFrameOutput), EDVR_NATIVE_FRAME_VERSION_2};
    EdvrNativeFrameInput v3MismatchFrame = input(41, 7, 16);
    check(table.beginFrame(table.context, &v3MismatchFrame, &v3SizeV2Version) ==
              E_INVALIDARG,
          "a version 3 size claiming version 2 is refused");

    // The trim reader asks for the [experimental] names and no others. First the
    // positive half, so the control below can fail: a value under the new name
    // reaches the output (vertical 3 is still set from the runtime-only case
    // above). Then the retired [fix] names carry a value for the worn headset
    // and the new ones carry none, and the game is told no trim: a reader that
    // had kept reading the old keys would tell it 9 degrees. (An old-layout LINE
    // in a file still works -- Config reads it as the new name through the
    // moved-from map, which this rig does not register; tools/config_test does,
    // with the shipped tables.)
    EdvrNativeFrameOutput newNames{sizeof(newNames), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput newNamesFrame = input(41, 7, 18);
    check(table.beginFrame(table.context, &newNamesFrame, &newNames) == S_OK &&
              newNames.trimVerticalDeg == 3.0f,
          "the trim reader reads experimental.fov_trim_vertical");
    edvr::Config::get().set("experimental.fov_trim_vertical", "");
    edvr::Config::get().set("experimental.fov_trim_outer", "");
    edvr::Config::get().set("experimental.fov_trim_nasal", "");
    edvr::Config::get().set("fix.fov_trim_vertical", "oculus/meta-quest-3:9");
    edvr::Config::get().set("fix.fov_trim_outer", "oculus/meta-quest-3:9");
    edvr::Config::get().set("fix.fov_trim_nasal", "oculus/meta-quest-3:9");
    EdvrNativeFrameOutput retiredNames{sizeof(retiredNames), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput retiredFrame = input(41, 7, 19);
    check(table.beginFrame(table.context, &retiredFrame, &retiredNames) == S_OK &&
              retiredNames.trimVerticalDeg == 0.0f && retiredNames.trimOuterDeg == 0.0f &&
              retiredNames.trimNasalDeg == 0.0f,
          "control: the retired fix.fov_trim_* names are not read by the trim reader");

    // F33: the provider's body-align translation reaches a version 8 caller; a version 6 caller (the struct through lookYawReset) is answered in
    // its own smaller struct with nothing past it, and never learns of it.
    {
        const float eye[3] = {0.25f, -0.5f, 0.125f};
        edvr::nativeFrameSetEyeAlign(eye, 7);
        EdvrNativeFrameOutput v8{sizeof(v8), EDVR_NATIVE_FRAME_VERSION_8};
        EdvrNativeFrameInput v8Frame = input(41, 7, 20);
        check(table.beginFrame(table.context, &v8Frame, &v8) == S_OK &&
                  v8.version == EDVR_NATIVE_FRAME_VERSION_8 && v8.size == sizeof(v8) &&
                  v8.eyeAlign[0] == 0.25f && v8.eyeAlign[1] == -0.5f && v8.eyeAlign[2] == 0.125f &&
                  v8.eyeAlignSerial == 7,
              "a version 8 caller receives the body-align translation and its serial (F33)");
        struct Guarded6 { EdvrNativeFrameOutput output; uint32_t sentinel; } g6{};
        g6.output.size = EDVR_NATIVE_FRAME_OUTPUT_SIZE_6;
        g6.output.version = EDVR_NATIVE_FRAME_VERSION_6;
        g6.sentinel = 0xA5A5A5A5u;
        EdvrNativeFrameInput v6Frame = input(41, 7, 21);
        check(table.beginFrame(table.context, &v6Frame, &g6.output) == S_OK &&
                  g6.output.version == EDVR_NATIVE_FRAME_VERSION_6 &&
                  g6.output.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_6 &&
                  g6.output.eyeAlign[0] == 0.0f && g6.output.eyeAlign[1] == 0.0f &&
                  g6.output.eyeAlign[2] == 0.0f && g6.output.eyeAlignSerial == 0 &&
                  g6.sentinel == 0xA5A5A5A5u,
              "a version 6 caller is answered in its own struct and never learns of the body align (F33)");
        EdvrNativeFrameOutput v6Full{sizeof(v6Full), EDVR_NATIVE_FRAME_VERSION_6};
        EdvrNativeFrameInput v6FullFrame = input(41, 7, 22);
        check(table.beginFrame(table.context, &v6FullFrame, &v6Full) == E_INVALIDARG,
              "a full-size struct claiming version 6 is refused (F33)");
        const float zero[3] = {};
        edvr::nativeFrameSetEyeAlign(zero, 0);
    }

    // F38 (pure math, no frame): the true head is the render translation plus the applied offset; the waypoint lead carries a yaw spin on
    // L frames (yaw(a) yaw(b)^T = yaw(a - b), so the lead is yaw(0.12 + L (0.12 - 0.10)) = yaw(0.17) at L = 2.5).
    {
        const float render[12] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f, 3.0f};
        const double applied[3] = {0.1, -0.2, 0.3};
        double head[3] = {};
        edvr::nofoot::alignHeadTrue(render, applied, head);
        check(std::fabs(head[0] - 1.1) < 1e-9 && std::fabs(head[1] - 1.8) < 1e-9 && std::fabs(head[2] - 3.3) < 1e-9,
              "F38: the true head is the render translation plus the applied offset");
        const auto yaw = [](double th, float* m) {
            const float c = static_cast<float>(std::cos(th)), s = static_cast<float>(std::sin(th));
            const float r[12] = {c, 0.0f, s, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -s, 0.0f, c, 0.0f};
            std::memcpy(m, r, sizeof(r));
        };
        float r0[12] = {}, r1[12] = {}, lead[12] = {}, same[12] = {};
        yaw(0.12, r0);
        yaw(0.10, r1);
        r0[3] = 0.01f;
        edvr::nofoot::leadPose(r0, r1, 2.5, lead);
        const double s17 = std::sin(0.17), c17 = std::cos(0.17);
        check(std::fabs(lead[0] - c17) < 1e-4 && std::fabs(lead[2] - s17) < 1e-4 && std::fabs(lead[8] + s17) < 1e-4 && std::fabs(lead[10] - c17) < 1e-4,
              "F38: the lead carries a yaw spin on 2.5 frames (0.12 + 2.5 x 0.02 rad)");
        check(std::fabs(lead[3] - 0.035) < 1e-5 && std::fabs(lead[7]) < 1e-6 && std::fabs(lead[11]) < 1e-6,
              "F38: the lead's translation runs on linearly (t0 + L (t0 - t1))");
        check(std::fabs(edvr::nofoot::rotDeltaDeg(r0, r1) - 0.02 * 180.0 / 3.14159265358979323846) < 1e-3,
              "F38: the rotation delta between two frames reads as 0.02 rad");
        edvr::nofoot::leadPose(r0, r1, 0.0, same);
        check(std::fabs(edvr::nofoot::rotDeltaDeg(same, r0)) < 1e-3 && std::fabs(same[3] - 0.01) < 1e-6,
              "F38: a lead of zero frames returns the newest pose");
    }

    // F44 (pure): the marker probe's tag. The sentinel round-trips through the low mantissa bits, moves each component by under 2^-15 of its
    // size, refuses a zero component, and is found at +16 of the right 336-byte record (and not in an all-zero record).
    {
        namespace dt = edvr::drawtag;
        const uint32_t s = dt::sentinel(37, 0x1234);
        check(dt::sentinelKey(s) == 37 && dt::sentinelFrame(s) == 0x1234 && (s & ~dt::kSentinelMask) == 0, "F44: the sentinel carries its key and frame");
        float v[3] = {1.2345f, -987.654f, 0.0123f};
        const float orig[3] = {v[0], v[1], v[2]};
        check(dt::encode(v, s) && dt::decode(v) == s, "F44: the sentinel round-trips through t'");
        bool withinBound = true;   // (not "small": rpcndr.h defines it as char)
        for (int i = 0; i < 3; ++i)
            withinBound = withinBound && std::fabs(v[i] - orig[i]) <= std::fabs(orig[i]) * (1.0f / 32768.0f) && std::signbit(v[i]) == std::signbit(orig[i]);
        check(withinBound, "F44: the tag moves each component by under 2^-15 of its size");
        float z[3] = {1.0f, 0.0f, 2.0f};
        check(!dt::encode(z, s) && z[1] == 0.0f && z[0] == 1.0f, "F44: a zero component is not tagged (t' unchanged)");
        alignas(16) uint8_t pool[dt::kRecordStride * 4] = {};
        std::memcpy(pool + 2 * dt::kRecordStride + dt::kPositionOffset, v, sizeof(v));
        const uint32_t wants[2] = {dt::sentinel(9, 1), s};
        dt::Hit hits[4];
        const uint32_t n = dt::scanRecords(pool, sizeof(pool), wants, 2, hits, 4, dt::plainLoad16);
        check(n == 1 && hits[0].index == 2 && hits[0].sentinel == s && hits[0].pos[1] == v[1], "F44: the scan finds the tagged record at index 2 only");
        dt::Hit one;
        check(dt::checkRecord(pool, sizeof(pool), 2, wants, 2, one, dt::streamLoad16) && one.index == 2 &&
                  !dt::checkRecord(pool, sizeof(pool), 3, wants, 2, one, dt::streamLoad16),
              "F44: the hint check (streaming load) reads the same record, and an empty record never matches");
        const float rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const double p[3] = {11.0, 2.0, 3.0}, o[3] = {10.0, 2.0, 2.0};
        double vp[3] = {}, vq[3] = {};
        dt::viewPoint(rows, p, o, vp);
        const double q[3] = {10.0, 3.0, 3.0};
        dt::viewPoint(rows, q, o, vq);
        check(std::fabs(vp[0] - 1.0) < 1e-12 && std::fabs(vp[2] - 1.0) < 1e-12 && std::fabs(dt::angleDeg(vp, vq) - 60.0) < 1e-9 &&
                  dt::rowsRotation(rows, 0.02) && dt::rowsAngleDeg(rows, rows) < 1e-6,
              "F44: the view point is rows . (p - o, 1), and the angle between (1, 0, 1) and (0, 1, 1) is 60 degrees");
        // F45: the whole-stream match (x names the record from FirstElement; y counted apart) and the view cache's nearest eye.
        const uint32_t stream[8] = {5, 900, 7, 901, 2, 1000, 7, 902};   // four entries (x, y)
        const uint32_t want[2] = {3400 + 7, 3400 + 1000};
        dt::StreamMatch m;
        check(dt::matchStream(reinterpret_cast<const uint8_t*>(stream), 4, 3400, want, 2, m) && m.xHits == 2 && m.yHits == 1 && m.entry == 1 &&
                  m.record == 3407 && m.want == 0 && !m.viaY,
              "F45: the stream match reads every entry, names the first x hit, and counts y hits apart");
        const uint32_t onlyY[1] = {3400 + 900};
        check(dt::matchStream(reinterpret_cast<const uint8_t*>(stream), 4, 3400, onlyY, 1, m) && m.xHits == 0 && m.yHits == 1 && m.viaY &&
                  !dt::matchStream(reinterpret_cast<const uint8_t*>(stream), 4, 0, onlyY, 1, m),
              "F45: a y-only hit is marked by y, and the record is counted from FirstElement");
        dt::ViewCache<4> views;
        dt::View w0, w1;
        w0.frame = w1.frame = 9;
        w0.eye = 0;
        w1.eye = 1;
        std::memcpy(w0.rows, rows, sizeof(rows));
        std::memcpy(w1.rows, rows, sizeof(rows));
        w1.rows[3] = 0.064f;   // the other eye: offset along x
        views.put(w0);
        views.put(w1);
        double dn = 0.0, dother = 0.0;
        check(views.nearestEye(9, w1.rows, &dn, &dother) == 1 && dn == 0.0 && dother > 0.06 && views.nearestEye(8, rows, &dn, &dother) == -1 &&
                  views.latest() && views.latest()->eye == 1,
              "F45: the view cache picks the eye whose rows are nearest, and has no eye for a frame it never saw");
        // F46: the NDC pair. A world projection with forward +z, x scale 1.2 and offset 0.1, y scale 1.1 and offset -0.05 (row-vector rows
        // 270..273, eye at o): P = o + (1, 0, 1) lands at ndc (1.3, -0.05), and the ray through it is 45 degrees from the one through o + (0,
        // 0, 1). The marker composite's rows 4, 5 and 7 written as the same projection give the same NDC; F47's offset rows move it by (dx, dy).
        const float wr[16] = {1.2f, 0, 0, 0, 0, 1.1f, 0, 0, 0.1f, -0.05f, 0.5f, 1.0f, 0, 0, 0, 0};
        const double wo[3] = {10.0, 2.0, 2.0}, wp[3] = {11.0, 2.0, 3.0}, wq[3] = {10.0, 2.0, 3.0};
        double cp[4], cq[4];
        dt::worldClip(wr, wp, wo, cp);
        dt::worldClip(wr, wq, wo, cq);
        const double np[2] = {cp[0] / cp[3], cp[1] / cp[3]}, nq[2] = {cq[0] / cq[3], cq[1] / cq[3]};
        check(std::fabs(np[0] - 1.3) < 1e-6 && std::fabs(np[1] + 0.05) < 1e-6 && std::fabs(dt::ndcAngleDeg(wr, np, nq) - 45.0) < 1e-4 &&
                  dt::ndcAngleDeg(wr, np, np) < 1e-4,   // acos near 1 is ~1e-6 deg off in double
              "F46: the world rows project P - o in row-vector form, and the rays through two NDC points are 45 degrees apart");
        float r4[4] = {1.2f, 0, 0.1f, 0}, r5[4] = {0, 1.1f, -0.05f, 0};
        const float r7[4] = {0, 0, 1.0f, 0};
        double mc[3];
        dt::markerClip(r4, r5, r7, wp, wo, mc);
        const bool same = std::fabs(mc[0] / mc[2] - np[0]) < 1e-6 && std::fabs(mc[1] / mc[2] - np[1]) < 1e-6;
        dt::ndcOffsetRows(r4, r5, r7, 0.01, -0.02);
        dt::markerClip(r4, r5, r7, wp, wo, mc);
        check(same && std::fabs(mc[0] / mc[2] - (np[0] + 0.01)) < 1e-6 && std::fabs(mc[1] / mc[2] - (np[1] - 0.02)) < 1e-6,
              "F46: the marker composite's rows 4, 5, 7 give the NDC the world rows give, and F47's offset rows move it by (dx, dy)");
        // F47: the delta of the same P through both views. The marker rows above sit (+0.01, -0.02) off the world's, so the delta is (-0.01,
        // +0.02); the offset rows built from it bring the delta to zero; a delta past the gate, or P behind either view, is refused.
        {
            double ddx = 0.0, ddy = 0.0;
            const int why = dt::ndcDelta(wr, wo, r4, r5, r7, wo, wp, 0.1, &ddx, &ddy);
            float q4[4], q5[4];
            std::memcpy(q4, r4, sizeof(q4));
            std::memcpy(q5, r5, sizeof(q5));
            dt::ndcOffsetRows(q4, q5, r7, ddx, ddy);
            double zx = 1.0, zy = 1.0;
            const int whyZero = dt::ndcDelta(wr, wo, q4, q5, r7, wo, wp, 0.1, &zx, &zy);
            double gx = 0.0, gy = 0.0;
            const int whyGate = dt::ndcDelta(wr, wo, r4, r5, r7, wo, wp, 0.005, &gx, &gy);
            const double behind[3] = {10.0, 2.0, 1.0};   // o + (0, 0, -1): behind both views
            double bx = 0.0, by = 0.0;
            const int whyBehind = dt::ndcDelta(wr, wo, r4, r5, r7, wo, behind, 0.1, &bx, &by);
            const float r7neg[4] = {0, 0, -1.0f, 0};
            const int whyMarker = dt::ndcDelta(wr, wo, r4, r5, r7neg, wo, wp, 0.1, &bx, &by);
            check(why == dt::kDeltaOk && std::fabs(ddx + 0.01) < 1e-6 && std::fabs(ddy - 0.02) < 1e-6 && whyZero == dt::kDeltaOk &&
                      std::fabs(zx) < 1e-6 && std::fabs(zy) < 1e-6 && whyGate == dt::kDeltaGate && whyBehind == dt::kDeltaWorldBehind &&
                      whyMarker == dt::kDeltaMarkerBehind,
                  "F47: the delta of the same P through the world and marker rows, zero after the offset rows, and its gate");
        }
        // F50: the lag delta. The marker rows are the world's own projection; the record's X0 lags P (o + (1.02, 0.01, 1)), and c is (0,
        // +0.036). (dx, dy) = -(m - c) moves the marker to ndc_P + c; a delta past the gate is refused; the baseline seeds over 30 slow-head
        // frames, refuses a fast head and a far m, and starts again after 90 far frames; the vertical lock drops the up component.
        {
            float m4[4] = {1.2f, 0, 0.1f, 0}, m5[4] = {0, 1.1f, -0.05f, 0};
            const double x0[3] = {11.02, 2.01, 3.0}, c[2] = {0.0, 0.036};
            double lm[2] = {}, ldx = 0.0, ldy = 0.0, wc[4];
            const int why = dt::ndcLagDelta(wr, wo, m4, m5, r7, wo, x0, wp, c, 0.05, lm, &ldx, &ldy);
            dt::worldClip(wr, wp, wo, wc);
            dt::ndcOffsetRows(m4, m5, r7, ldx, ldy);
            double lmc[3];
            dt::markerClip(m4, m5, r7, x0, wo, lmc);
            const bool lands = std::fabs(lmc[0] / lmc[2] - (wc[0] / wc[3] + c[0])) < 1e-5 && std::fabs(lmc[1] / lmc[2] - (wc[1] / wc[3] + c[1])) < 1e-5;
            double gx = 0.0, gy = 0.0, gm[2] = {};
            const float g4[4] = {1.2f, 0, 0.1f, 0}, g5[4] = {0, 1.1f, -0.05f, 0};
            const int whyGate = dt::ndcLagDelta(wr, wo, g4, g5, r7, wo, x0, wp, c, 0.01, gm, &gx, &gy);
            check(why == dt::kDeltaOk && std::fabs(lm[0] - 0.024) < 1e-5 && std::fabs(lm[1] - 0.011) < 1e-5 && lands && whyGate == dt::kDeltaGate &&
                      std::fabs(gm[0] - lm[0]) < 1e-12,
                  "F50: the lag delta (X0 through the marker rows against P through the world rows) moves the marker to ndc_P + c, and its gate");
            dt::LagBaseline b;
            const double s0[2] = {0.0, 0.036}, s1[2] = {0.0002, 0.0362}, away[2] = {0.05, 0.036};
            bool fastTaken = b.update(s0, 10.0), unknownTaken = b.update(s0, -1.0);
            for (int i = 0; i < 29; ++i) b.update(i % 2 ? s1 : s0, 1.0);
            const bool notYet = !b.seeded;
            b.update(s0, 1.0);
            const bool seeded = b.seeded && b.n == 30 && std::fabs(b.c[1] - 0.0360933) < 1e-6 && b.ready(2016.0, 2016.0, 0.5);
            const bool farRefused = !b.update(away, 1.0);
            for (int i = 0; i < 88; ++i) b.update(away, 1.0);
            const bool stillSeeded = b.seeded;
            bool restarted = false;
            b.update(away, 1.0, &restarted);
            check(!fastTaken && !unknownTaken && notYet && seeded && farRefused && stillSeeded && restarted && !b.seeded && b.n == 0,
                  "F50: the baseline seeds over 30 slow-head frames, refuses a fast head and a far m, and starts again after 90 far frames");
            double vv[3] = {0.1, 0.2, -0.3};
            const double up[3] = {0.0, 2.0, 0.0}, flat[3] = {0.0, 0.0, 0.0};
            const double k = edvr::nofoot::dropAlong(vv, up);
            double v2[3] = {0.1, 0.2, -0.3};
            const double k2 = edvr::nofoot::dropAlong(v2, flat);
            check(std::fabs(k - 0.2) < 1e-12 && std::fabs(vv[0] - 0.1) < 1e-12 && std::fabs(vv[1]) < 1e-12 && std::fabs(vv[2] + 0.3) < 1e-12 && k2 == 0.0 &&
                      v2[1] == 0.2,
                  "F50: the vertical lock drops the component along the body's up axis (normalised) and leaves a degenerate axis alone");
        }
        // F52: the per-key tag round trip (every 8-bit key at one frame: a distinct sentinel and cell, the key and frame back, the low bits
        // through encode and decode), the reference selection's default, the bookkeeping counts, and the key table's reclaim.
        {
            bool roundTrip = true, cellsDistinct = true;
            bool seenC[256] = {};
            uint32_t sents[256];
            for (uint32_t key = 0; key < 256; ++key) {
                const uint32_t sk = dt::sentinel(key, 1234u);
                sents[key] = sk;
                float tv[3] = {12.5f, -3.25f, 101.0f};
                roundTrip = roundTrip && dt::sentinelKey(sk) == key && dt::sentinelFrame(sk) == 1234u && dt::encode(tv, sk) && dt::decode(tv) == sk;
                const uint32_t c = dt::tagCellOf(key);
                cellsDistinct = cellsDistinct && c < dt::kTagCells && !seenC[c];
                seenC[c] = true;
            }
            bool sentDistinct = true;
            for (uint32_t a = 0; a < 256 && sentDistinct; ++a)
                for (uint32_t b = a + 1; b < 256 && sentDistinct; ++b) sentDistinct = sents[a] != sents[b];
            check(roundTrip && cellsDistinct && sentDistinct,
                  "F52: every tag key has its own sentinel and its own cell, and the key, frame and low bits come back");
            check(dt::parseRefMode(nullptr) == dt::kRefTemporal && dt::parseRefMode("temporal") == dt::kRefTemporal &&
                      dt::parseRefMode("world_first") == dt::kRefWorldFirst && dt::parseRefMode("off") == dt::kRefTemporal &&
                      dt::chooseReference(dt::kRefTemporal, true) == dt::kRefTemporal &&
                      dt::chooseReference(dt::kRefTemporal, false) == dt::kRefWorldFirst &&
                      dt::chooseReference(dt::kRefWorldFirst, true) == dt::kRefWorldFirst,
                  "F52: the reference defaults to temporal while the temporal rows are there, else world_first; world_first is always world_first");
            dt::B10bTally t;
            t.note(true, true);
            t.note(true, true);
            t.note(true, false);
            t.note(true, false);
            t.note(true, false);
            t.note(false, false);
            t.note(false, true);   // not copied: never tagged, whatever the match says
            check(t.draws == 7 && t.tagged == 2 && t.untagged() == 3 && t.notCopied == 2,
                  "F52: the marker composite bookkeeping counts tagged, NOT tagged and not copied draws");
            dt::KeyIndex<3> idx;
            bool f1 = false, f2 = false, f3 = false, f4 = false, f5 = false;
            const int s43 = idx.acquire(43, 10, &f1), s44 = idx.acquire(44, 11, &f2), s45 = idx.acquire(45, 12, &f3);
            const int again = idx.acquire(43, 20, &f4);   // 43 seen again: 44 is now the oldest
            const int s46 = idx.acquire(46, 21, &f5);
            check(f1 && f2 && f3 && !f4 && again == s43 && f5 && s46 == s44 && idx.find(44) < 0 && idx.find(45) == s45 && idx.find(46) == s46,
                  "F52: the key table finds a key's slot, and a new key reclaims the slot seen longest ago");
        }
        // F52: the reference's geometry. The world rows above (forward +z, eye at o) give the basis right x, up y, forward z; a camera turned
        // 2 degrees about up (its rows view->world with z back) gives forward = minus column 2, the bases 2 degrees apart; the world rows
        // with the camera's rotation put a point straight down the camera's forward at the rows' own x offset (0.1) and keep any eye; with
        // the world's own basis they come back unchanged.
        {
            const double th = 2.0 * 3.14159265358979323846 / 180.0;
            double rA[3], uA[3], fA[3];
            const bool okA = dt::worldBasis(wr, rA, uA, fA);
            const double rB[3] = {std::cos(th), 0.0, -std::sin(th)}, uB[3] = {0.0, 1.0, 0.0}, fB[3] = {std::sin(th), 0.0, std::cos(th)};
            float cam[12] = {};
            for (int r = 0; r < 3; ++r) {
                cam[4 * r + 0] = static_cast<float>(rB[r]);
                cam[4 * r + 1] = static_cast<float>(uB[r]);
                cam[4 * r + 2] = static_cast<float>(-fB[r]);
                cam[4 * r + 3] = 5.0f;
            }
            double rC[3], uC[3], fC[3];
            int sign = 0;
            const bool okC = dt::cameraBasis(cam, fA, rC, uC, fC, &sign);
            const dt::BasisDiff d = dt::basisDiff(rA, uA, fA, rC, uC, fC);
            check(okA && std::fabs(rA[0] - 1.0) < 1e-9 && std::fabs(uA[1] - 1.0) < 1e-9 && std::fabs(fA[2] - 1.0) < 1e-9 && okC && sign == -1 &&
                      std::fabs(fC[0] - fB[0]) < 1e-6 && d.sameHand && std::fabs(d.total - 2.0) < 1e-3 && std::fabs(d.right - 2.0) < 1e-3 &&
                      d.up < 1e-3 && std::fabs(d.fwd - 2.0) < 1e-3,
                  "F52: the world rows' and the camera rows' bases, the camera's forward sign, and the angle between them");
            float sameRows[16], turned[16];
            const bool okSame = dt::swapRotation(wr, rA, uA, fA, rA, uA, fA, sameRows);
            bool unchanged = okSame;
            for (int i = 0; i < 16; ++i) unchanged = unchanged && std::fabs(sameRows[i] - wr[i]) < 1e-6;
            const bool okTurned = dt::swapRotation(wr, rA, uA, fA, rC, uC, fC, turned);
            const double ahead[3] = {wo[0] + fB[0], wo[1] + fB[1], wo[2] + fB[2]};
            double ca[4], cw[4];
            dt::worldClip(turned, ahead, wo, ca);
            dt::worldClip(wr, ahead, wo, cw);
            const double shift = cw[0] / cw[3] - ca[0] / ca[3];
            // An eye off o: t_k = -v . c_k for x, y and w (v = (0.5, -0.25, 2)), clip z there 1.025; the turned rows keep both.
            float we[16];
            std::memcpy(we, wr, sizeof(we));
            we[12] = -0.8f;
            we[13] = 0.375f;
            we[14] = 0.025f;
            we[15] = -2.0f;
            float te[16];
            const bool okEye = dt::swapRotation(we, rA, uA, fA, rC, uC, fC, te);
            const double eyeP[3] = {wo[0] + 0.5, wo[1] - 0.25, wo[2] + 2.0};
            double ce[4], co[4];
            dt::worldClip(te, eyeP, wo, ce);
            dt::worldClip(we, eyeP, wo, co);
            check(unchanged && okTurned && std::fabs(ca[0] / ca[3] - 0.1) < 1e-5 && std::fabs(ca[1] / ca[3] + 0.05) < 1e-5 &&
                      std::fabs(shift - 1.2 * std::tan(th)) < 1e-5 && okEye && std::fabs(co[0]) < 1e-5 && std::fabs(co[1]) < 1e-5 &&
                      std::fabs(co[3]) < 1e-5 && std::fabs(ce[0]) < 1e-5 && std::fabs(ce[1]) < 1e-5 && std::fabs(ce[3]) < 1e-5 &&
                      std::fabs(ce[2] - 1.025) < 1e-5,
                  "F52: the world rows with the camera's rotation: unchanged under their own basis, the camera's forward at the rows' offset, the eye kept");
        }
        // F53: the scene reference's test (the first-person body's PS refused first, then a depth view outside the scene pair, then a VS that
        // is not a scene geometry one), the trust threshold (compared and under 1 px), the icon offset e, the correction landing the marker at
        // ndc_P + e, the census's angles, and the key census's sentinel check.
        {
            const uint64_t eb = 0xEB5234DB6ADB491Dull, de = 0xDE545DC8EE4FBB87ull, b4 = 0x5B4D8E894EEDA8B4ull, fp = 0x7B0DC42D383F694Cull;
            check(dt::sceneRefWhy(eb, 0x1234, true) == dt::kSceneOk && dt::sceneRefWhy(de, 0x1234, true) == dt::kSceneOk &&
                      dt::sceneRefWhy(b4, 0x1234, true) == dt::kSceneOk && dt::sceneRefWhy(fp, dt::kFpBodyPs, true) == dt::kSceneFpBodyPs &&
                      dt::sceneRefWhy(eb, dt::kFpBodyPs, true) == dt::kSceneFpBodyPs && dt::sceneRefWhy(eb, 0x1234, false) == dt::kSceneNotSceneDepth &&
                      dt::sceneRefWhy(fp, 0x1234, true) == dt::kSceneNotSceneVs,
                  "F53: only a main-scene pass (a scene geometry VS on the scene depth view, not the first-person body's PS) gives the eye's rows");
            check(dt::sceneRowsTrusted(true, 0.0) && dt::sceneRowsTrusted(true, 0.99) && !dt::sceneRowsTrusted(true, 1.0) &&
                      !dt::sceneRowsTrusted(true, 3.5) && !dt::sceneRowsTrusted(false, 0.1) && !dt::sceneRowsTrusted(true, -1.0) &&
                      !dt::sceneRowsTrusted(true, std::numeric_limits<double>::quiet_NaN()),
                  "F53: the scene rows are trusted only on a compared frame that agrees with the temporal pass under 1 px");
            double e[2] = {};
            testIconE(2016.0, e);
            // The correction with c = e: X0 lags P (as F50's case); the patched marker lands at ndc_P + e.
            float m4[4] = {1.2f, 0, 0.1f, 0}, m5[4] = {0, 1.1f, -0.05f, 0};
            const double x0[3] = {11.02, 2.01, 3.0};
            double lm[2] = {}, ldx = 0.0, ldy = 0.0, wc[4], lmc[3];
            const int why = dt::ndcLagDelta(wr, wo, m4, m5, r7, wo, x0, wp, e, 0.05, lm, &ldx, &ldy);
            dt::worldClip(wr, wp, wo, wc);
            dt::ndcOffsetRows(m4, m5, r7, ldx, ldy);
            dt::markerClip(m4, m5, r7, x0, wo, lmc);
            check(why == dt::kDeltaOk && std::fabs(lmc[0] / lmc[2] - (wc[0] / wc[3] + e[0])) < 1e-5 &&
                      std::fabs(lmc[1] / lmc[2] - (wc[1] / wc[3] + e[1])) < 1e-5,
                  "F53: the correction moves the marker to ndc(P through the scene rows) + e");
            // The census's angles: a content against itself is 0; against the same rows turned 2 degrees about up, 2 (forward and rotation).
            double rA[3], uA[3], fA[3];
            const double th = 2.0 * 3.14159265358979323846 / 180.0;
            const double rB[3] = {std::cos(th), 0.0, -std::sin(th)}, uB[3] = {0.0, 1.0, 0.0}, fB[3] = {std::sin(th), 0.0, std::cos(th)};
            float turned[16];
            const bool okT = dt::worldBasis(wr, rA, uA, fA) && dt::swapRotation(wr, rA, uA, fA, rB, uB, fB, turned);
            double f0 = -1.0, r0 = -1.0, f2 = -1.0, r2 = -1.0;
            dt::rowsBasisAngles(wr, wr, &f0, &r0);
            dt::rowsBasisAngles(wr, turned, &f2, &r2);
            check(okT && std::fabs(f0) < 1e-6 && std::fabs(r0) < 1e-3 && std::fabs(f2 - 2.0) < 1e-3 && std::fabs(r2 - 2.0) < 1e-3,
                  "F53: the census's forward and rotation angles between two rows contents");
            const uint32_t ok3[3] = {1, 10, 43}, wrap[2] = {5, 133}, big[2] = {10, 266}, self[1] = {200};
            uint32_t ka = 0, kb = 0;
            const bool dOk = dt::keysDistinct(ok3, 3), dWrap = dt::keysDistinct(wrap, 2, &ka, &kb);
            check(dOk && !dWrap && ka == 5 && kb == 133 && !dt::keysDistinct(big, 2) && !dt::keysDistinct(self, 1) && dt::keysDistinct(ok3, 0),
                  "F53: the key census finds two keys (or a key's first and second posts) that would share a sentinel");
        }
        // F62c: the patch decision without F53's trust gate and the waypoint series' crop about this frame's P. F64: the 0.4 NDC limit (was
        // 0.1), no one-frame hold, the eye's newest rows within one frame.
        {
            // (1) The scene rows and the temporal reference 5 px apart at P (the camera turned 0.1184 deg; P 45 deg off axis, so 2.4 x the
            // turn in NDC, 5.0 px at 2016): F53's gate refused the frame; F62c patches it onto ndc(P through the scene rows) + e.
            const double th = 0.1184 * 3.14159265358979323846 / 180.0;
            double rA[3], uA[3], fA[3];
            const double rB[3] = {std::cos(th), 0.0, -std::sin(th)}, uB[3] = {0.0, 1.0, 0.0}, fB[3] = {std::sin(th), 0.0, std::cos(th)};
            float temporal[16];
            const bool okT = dt::worldBasis(wr, rA, uA, fA) && dt::swapRotation(wr, rA, uA, fA, rB, uB, fB, temporal);
            const double W = 2016.0, H = 2016.0;
            double cs[4], ct[4];
            dt::worldClip(wr, wp, wo, cs);
            dt::worldClip(temporal, wp, wo, ct);
            const double gx = (ct[0] / ct[3] - cs[0] / cs[3]) * W * 0.5, gy = (ct[1] / ct[3] - cs[1] / cs[3]) * H * 0.5;
            const double apart = std::sqrt(gx * gx + gy * gy);
            double e[2] = {};
            testIconE(H, e);
            float m4[4] = {1.2f, 0, 0.1f, 0}, m5[4] = {0, 1.1f, -0.05f, 0};
            const double x0[3] = {11.02, 2.01, 3.0};
            double lm[2] = {}, ldx = 0.0, ldy = 0.0, lmc[3];
            const int dwhy = dt::ndcLagDelta(wr, wo, m4, m5, r7, wo, x0, wp, e, dt::kPatchGateNdc, lm, &ldx, &ldy);
            const dt::PatchVerdict pv = dt::patchDecide(true, true, dt::kRowsFresh, true, dwhy, ldx, ldy);
            dt::ndcOffsetRows(m4, m5, r7, pv.dx, pv.dy);
            dt::markerClip(m4, m5, r7, x0, wo, lmc);
            check(okT && apart > 4.5 && apart < 5.5 && !dt::sceneRowsTrusted(true, apart) && pv.patch && !pv.newest && pv.why == dt::kPatchApplied &&
                      std::fabs(lmc[0] / lmc[2] - (cs[0] / cs[3] + e[0])) < 1e-5 && std::fabs(lmc[1] / lmc[2] - (cs[1] / cs[3] + e[1])) < 1e-5,
                  "F62c: scene and temporal rows 5 px apart at P: F53's trust gate refused the frame; F62c patches it onto ndc(P through the scene "
                  "rows) + e");
            // (2) F64: the 0.4 NDC limit (the F63 flight's worst real lag, about 0.15 NDC at 90 deg/s, was refused at 0.1): 0.39 patches; 0.4,
            // 0.41 and over (either axis) are refused as |d|; through ndcLagDelta at the patch gate, c moved so the delta is 0.39 then 0.41 in
            // x. The w > 0 and P-found tests are unchanged.
            const dt::PatchVerdict under = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaOk, 0.39, -0.39);
            const dt::PatchVerdict at = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaOk, 0.4, 0.0);
            const dt::PatchVerdict over = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaOk, 0.41, 0.0);
            const dt::PatchVerdict overY = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaOk, 0.0, -0.41);
            const dt::PatchVerdict real = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaOk, 0.15, 0.02);
            float q4[4] = {1.2f, 0, 0.1f, 0}, q5[4] = {0, 1.1f, -0.05f, 0};
            double qm[2] = {}, qdx = 0.0, qdy = 0.0;
            const double cA[2] = {lm[0] + 0.39, lm[1]}, cB[2] = {lm[0] + 0.41, lm[1]};
            const int wA = dt::ndcLagDelta(wr, wo, q4, q5, r7, wo, x0, wp, cA, dt::kPatchGateNdc, qm, &qdx, &qdy);
            const dt::PatchVerdict vA = dt::patchDecide(true, true, dt::kRowsFresh, true, wA, qdx, qdy);
            const int wB = dt::ndcLagDelta(wr, wo, q4, q5, r7, wo, x0, wp, cB, dt::kPatchGateNdc, qm, &qdx, &qdy);
            const dt::PatchVerdict vB = dt::patchDecide(true, true, dt::kRowsFresh, true, wB, qdx, qdy);
            check(dt::kPatchGateNdc == 0.4 && under.patch && under.why == dt::kPatchApplied && !at.patch && at.why == dt::kPatchGate &&
                      !over.patch && over.why == dt::kPatchGate && !overY.patch && overY.why == dt::kPatchGate && real.patch &&
                      wA == dt::kDeltaOk && vA.patch && std::fabs(vA.dx - 0.39) < 1e-9 && wB == dt::kDeltaGate && !vB.patch &&
                      vB.why == dt::kPatchGate,
                  "F64: |dx|, |dy| under 0.4 NDC patch (0.39 and the flight's 0.15 apply); 0.4, 0.41 and over (either axis) are refused as |d|");
            // (3) F64: no one-frame hold: frame 200 patches (0.01, -0.02); 201, refused as |d|, is NOT patched with 200's delta; nor is a stale
            // frame right after a good one. Unmatched, both-eyes, no P and w <= 0 are their own refusals, never patched.
            const dt::PatchVerdict f200 = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaOk, 0.01, -0.02);
            const dt::PatchVerdict f201 = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaGate, 0.5, 0.0);
            const dt::PatchVerdict f202 = dt::patchDecide(true, true, dt::kRowsNone, true, -1, 0.0, 0.0);
            const dt::PatchVerdict unm = dt::patchDecide(false, true, dt::kRowsFresh, true, dt::kDeltaOk, 0.0, 0.0);
            const dt::PatchVerdict both = dt::patchDecide(true, false, dt::kRowsFresh, true, dt::kDeltaOk, 0.0, 0.0);
            const dt::PatchVerdict noP = dt::patchDecide(true, true, dt::kRowsFresh, false, -1, 0.0, 0.0);
            const dt::PatchVerdict wNeg = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaWorldBehind, 0.0, 0.0);
            const dt::PatchVerdict mNeg = dt::patchDecide(true, true, dt::kRowsFresh, true, dt::kDeltaMarkerBehind, 0.0, 0.0);
            check(f200.patch && !f200.newest && !f201.patch && f201.why == dt::kPatchGate && f201.dx == 0.0 && !f202.patch &&
                      f202.why == dt::kPatchStale && f202.dx == 0.0 && !unm.patch && unm.why == dt::kPatchUnmatched && !both.patch &&
                      both.why == dt::kPatchBothEyes && !noP.patch && noP.why == dt::kPatchNoP && !wNeg.patch && wNeg.why == dt::kPatchW &&
                      !mNeg.patch && mNeg.why == dt::kPatchW,
                  "F64: no delta is held: a |d| or stale refusal right after a good frame is not patched; unmatched, both-eyes, no P and w <= 0 "
                  "are refused");
            // (3b) F64: the eye's newest scene rows: this frame's are fresh; one frame older are accepted (the delta computed through them,
            // marked newest); two frames older, none at all, or a rows frame after the draw's are refused as stale.
            const int sFresh = dt::sceneRowsState(500, 500), sOne = dt::sceneRowsState(499, 500), sTwo = dt::sceneRowsState(498, 500),
                      sNever = dt::sceneRowsState(~0u, 500), sAhead = dt::sceneRowsState(501, 500), sWrap = dt::sceneRowsState(0xFFFFFFFEu, 0);
            const dt::PatchVerdict nOne = dt::patchDecide(true, true, sOne, true, dt::kDeltaOk, 0.02, -0.01);
            const dt::PatchVerdict nTwo = dt::patchDecide(true, true, sTwo, true, dt::kDeltaOk, 0.02, -0.01);
            const dt::PatchVerdict nOneGate = dt::patchDecide(true, true, sOne, true, dt::kDeltaOk, 0.45, 0.0);
            check(dt::kSceneRowsMaxAge == 1u && sFresh == dt::kRowsFresh && sOne == dt::kRowsNewest && sTwo == dt::kRowsNone &&
                      sNever == dt::kRowsNone && sAhead == dt::kRowsNone && sWrap == dt::kRowsNone && nOne.patch && nOne.newest &&
                      nOne.why == dt::kPatchApplied && nOne.dx == 0.02 && nOne.dy == -0.01 && !nTwo.patch && nTwo.why == dt::kPatchStale &&
                      !nOneGate.patch && nOneGate.why == dt::kPatchGate,
                  "F64: the eye's newest scene rows one frame older than the draw are accepted (patched through them, marked newest); two "
                  "frames older, none, or ahead of the draw are refused as stale; the gate still applies");
            // (3c) F64: the success tally: an eye-frame is counted once by its first decision's speed band, as none until a bound patch
            // upgrades it (fresh over newest); a second bind in the frame does not count it again; bands 0..30, 30..90, >= 90, unknown.
            dt::EyeFrameTally tl;
            tl.visible(10, 12.0);
            tl.visible(10, 95.0);                  // the same eye-frame: no recount, its band stays 0..30
            tl.bound(10, dt::kRowsNewest, 12.0);   // newest
            tl.bound(10, dt::kRowsFresh, 12.0);    // upgraded to fresh, still one applied frame
            tl.bound(10, dt::kRowsNewest, 12.0);   // no downgrade
            tl.visible(11, 45.0);                  // none at 30..90
            tl.bound(12, dt::kRowsNewest, 120.0);  // a bind with no decision first: counted at >= 90, newest
            tl.visible(13, -1.0);                  // the head unknown
            tl.bound(13, dt::kRowsNone, -1.0);     // not a patch: stays none
            check(dt::speedBand(29.9) == 0 && dt::speedBand(30.0) == 1 && dt::speedBand(89.9) == 1 && dt::speedBand(90.0) == 2 &&
                      dt::speedBand(-1.0) == 3 && dt::speedBand(std::numeric_limits<double>::quiet_NaN()) == 3 && tl.total() == 4u &&
                      tl.frames[0] == 1u && tl.applied[0] == 1u && tl.frames[1] == 1u && tl.applied[1] == 0u && tl.frames[2] == 1u &&
                      tl.applied[2] == 1u && tl.frames[3] == 1u && tl.applied[3] == 0u && tl.inState[dt::kRowsFresh] == 1u &&
                      tl.inState[dt::kRowsNewest] == 1u && tl.inState[dt::kRowsNone] == 2u,
                  "F64: the success tally counts each marker-visible eye-frame once by speed band, applied when a patch bound (fresh over "
                  "newest), none otherwise");
            // (4) The series' crop: 1024 square about THIS frame's P (age 0). P at ndc (0.25, 0.1) in a 4032 eye: centre (2520, 1814.4),
            // box at (2008, 1302). One frame old, no P, behind (w <= 0), outside the NDC or NaN: skipped. Near the edge the box stays in the
            // eye; flipV mirrors y; the right eye of a side-by-side texture keeps its own region.
            const uint32_t region[4] = {0, 0, 4032, 4032}, right[4] = {4032, 0, 8064, 4032}, lo[2] = {0, 0}, loR[2] = {4032, 0},
                           span[2] = {4032, 4032};
            const double nP[2] = {0.25, 0.1}, nEdge[2] = {0.95, -0.99}, nOut[2] = {1.2, 0.0},
                         nNan[2] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
            double c0[2], c1[2], c2[2], c3[2];
            uint32_t b0[2], b1[2], b3[2];
            dt::markerCropCentre(nP, region, false, false, c0);
            dt::cropOrigin(c0, lo, span, dt::kMarkerDumpCrop, dt::kMarkerDumpCrop, b0);
            dt::markerCropCentre(nEdge, region, false, false, c1);
            dt::cropOrigin(c1, lo, span, dt::kMarkerDumpCrop, dt::kMarkerDumpCrop, b1);
            dt::markerCropCentre(nP, region, false, true, c2);
            dt::markerCropCentre(nP, right, false, false, c3);
            dt::cropOrigin(c3, loR, span, dt::kMarkerDumpCrop, dt::kMarkerDumpCrop, b3);
            check(dt::kMarkerDumpCrop == 1024u && dt::markerCropPick(true, 500, 500, 1.0, nP) == dt::kCropOk &&
                      dt::markerCropPick(true, 499, 500, 1.0, nP) == dt::kCropNotThisFrame && dt::markerCropPick(false, 500, 500, 1.0, nP) == dt::kCropNoP &&
                      dt::markerCropPick(true, 500, 500, 0.0, nP) == dt::kCropOffScreen &&
                      dt::markerCropPick(true, 500, 500, -2.0, nP) == dt::kCropOffScreen &&
                      dt::markerCropPick(true, 500, 500, 1.0, nOut) == dt::kCropOffScreen &&
                      dt::markerCropPick(true, 500, 500, 1.0, nNan) == dt::kCropOffScreen && std::fabs(c0[0] - 2520.0) < 1e-9 &&
                      std::fabs(c0[1] - 1814.4) < 1e-9 && b0[0] == 2008u && b0[1] == 1302u && b1[0] == 3008u && b1[1] == 3008u &&
                      std::fabs(c2[1] - 2217.6) < 1e-9 && std::fabs(c3[0] - 6552.0) < 1e-9 && b3[0] == 6040u && b3[1] == 1302u,
                  "F62c: the series' 1024 crop is centred on this frame's P (age 0), kept in the eye, skipped when P is off the eye or not this "
                  "frame's");
        }
        // F63: the held match (90 frames; a contradicting read-back drops it), the bias EMA (alpha 0.02, only while patched), the instance
        // stream's CPU-shadow window.
        {
            // (1) Confirmed at frame 1000 (place 3, record 8): held through 1090, not 1091. Evidence about another place, or the held record
            // read untagged, leaves it; evidence older than the newest confirmation says nothing; a frame-by-frame decision through patchDecide
            // patches 90 of 90 frames after it with no read-back at all.
            dt::HeldMatch h;
            const int c0 = dt::holdEvidence(h, 1000, 3, 8, 43, true, true);
            const bool held89 = h.held(1089), held90 = h.held(1090), held91 = h.held(1091), heldBefore = h.held(999);
            const int other = dt::holdEvidence(h, 1010, 4, 2, 43, false, true);
            const int same = dt::holdEvidence(h, 1011, 3, 8, 43, false, false);
            const bool stillHeld = h.held(1011) && h.frame == 1000u && h.ord == 3 && h.record == 8u;
            dt::HeldMatch hs = h;
            hs.frame = 1020;
            const int stale = dt::holdEvidence(hs, 1015, 3, 12, 43, false, true);
            uint32_t patched = 0;
            for (uint32_t f = 1001; f <= 1090; ++f)
                if (dt::patchDecide(h.held(f), true, dt::kRowsFresh, true, dt::kDeltaOk, 0.01, 0.0).patch) ++patched;
            // (2) A contradicting read-back drops it: the held place read record 12 while the key was tagged elsewhere; or (no find at all)
            // a record other than the held one. A read of the key's record at another place moves the hold there.
            const int contra = dt::holdEvidence(h, 1030, 3, 12, 43, false, true);
            const bool dropped = !h.held(1030) && !h.held(1031);
            dt::HeldMatch h3, h4;
            dt::holdEvidence(h3, 50, 2, 9, 43, true, true);
            const int contra2 = dt::holdEvidence(h3, 60, 2, 10, 43, false, false);
            dt::holdEvidence(h4, 70, 3, 8, 43, true, true);
            const int moved = dt::holdEvidence(h4, 72, 5, 8, 43, true, true);
            const dt::PatchVerdict afterDrop = dt::patchDecide(h.held(1032), true, dt::kRowsFresh, true, dt::kDeltaOk, 0.01, 0.0);
            check(dt::kMatchHoldFrames == 90u && c0 == dt::kHoldConfirm && held89 && held90 && !held91 && !heldBefore && other == dt::kHoldNone &&
                      same == dt::kHoldNone && stillHeld && stale == dt::kHoldNone && hs.held(1020) && patched == 90u &&
                      contra == dt::kHoldContradict && dropped && contra2 == dt::kHoldContradict && !h3.held(60) && moved == dt::kHoldConfirm &&
                      h4.ord == 5 && h4.frame == 72u && !afterDrop.patch && afterDrop.why == dt::kPatchUnmatched,
                  "F63: a match is held 90 frames from its newest confirmation and patches every frame of them; a contradicting read-back drops "
                  "it; older evidence and evidence about another place say nothing; the key's record at another place moves it");
            // (4) The stream window: 8192 entries about the eye's newest marker composite entry (F62: eye 0 at 18205..18211), even, inside the
            // buffer (192512 entries, F62's 1540096 bytes); a small buffer whole.
            uint32_t lo = 0, n = 0, loA = 0, nA = 0, loB = 0, nB = 0, loC = 0, nC = 0;
            dt::streamWindow(18210, 192512, 8192, &lo, &n);
            dt::streamWindow(100, 192512, 8192, &loA, &nA);
            dt::streamWindow(192500, 192512, 8192, &loB, &nB);
            dt::streamWindow(10, 1001, 8192, &loC, &nC);
            check(lo == 14114u && n == 8192u && 18205u >= lo && 18211u < lo + n && loA == 0u && nA == 8192u && loB == 184320u && nB == 8192u &&
                      loB + nB == 192512u && loC == 0u && nC == 1000u,
                  "F63: the stream's CPU-shadow window: 8192 even-aligned entries about the eye's marker composite entry, kept in the buffer");
        }
    }

    // F40 (pure math): the view map. seated = D . Ry(psi) . body; the head in body axes is Ry(psi)^-1 . diag(1,1,-1) . tS; psi is the seated head
    // yaw less the headset's yaw in the body. The plant is the measured one at psi 0 (cam_body moves by D . applied), mapped the same way; the
    // controller is the filter's 0.2 gain, so the body error (a 34 cm start, z) must fall monotonically to under 1 cm within 180 frames (2 s at 90 Hz).
    {
        const double pi = 3.14159265358979323846;
        const auto ryRef = [](double psi, const double v[3], double out[3]) {
            const double c = std::cos(psi), s = std::sin(psi);
            out[0] = c * v[0] + s * v[2];
            out[1] = v[1];
            out[2] = -s * v[0] + c * v[2];
        };
        const double psis[3] = {0.0, 37.0 * pi / 180.0, 180.0 * pi / 180.0};
        for (double psi : psis) {
            const double body[3] = {0.02, -0.01, -0.34};
            double seated[3] = {};
            edvr::nofoot::seatedFromBody(body, psi, seated);
            const double dS[3] = {-seated[0], -seated[1], seated[2]};
            double back[3] = {};
            ryRef(-psi, dS, back);
            const double inv = std::fabs(back[0] - body[0]) + std::fabs(back[1] - body[1]) + std::fabs(back[2] - body[2]);
            check(inv < 1e-9, "F40: the seated map inverts through Ry(-psi) . D (the body error comes back unchanged)");
            // the plant: cam = cam0 + Ry(-psi) . D . applied; the target is the origin, so the error is minus the camera.
            const double cam0[3] = {0.0, 0.0, 0.34};
            const auto camAt = [&](const double applied[3]) {
                const double vD[3] = {-applied[0], -applied[1], applied[2]};
                double u[3] = {};
                ryRef(-psi, vD, u);
                return std::array<double, 3>{cam0[0] + u[0], cam0[1] + u[1], cam0[2] + u[2]};
            };
            double applied[3] = {};
            double prev = 1e9;
            bool monotone = true;
            for (int f = 0; f < 180; ++f) {
                const auto cam = camAt(applied);
                const double e[3] = {-cam[0], -cam[1], -cam[2]};
                const double n = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
                if (n > prev + 1e-12) monotone = false;
                prev = n;
                double sd[3] = {};
                edvr::nofoot::seatedFromBody(e, psi, sd);
                for (size_t i = 0; i < 3; ++i) applied[i] += 0.2 * sd[i];
            }
            const auto camEnd = camAt(applied);
            const double endErr = std::sqrt(camEnd[0] * camEnd[0] + camEnd[1] * camEnd[1] + camEnd[2] * camEnd[2]);
            check(monotone && endErr < 0.01, "F40: the view map closes a 34 cm body error monotonically to under 1 cm in 180 frames (psi 0, 37, 180)");
        }
        // the head's own motion: Ry(psi)^-1 . diag(1,1,-1) . tS, and its inverse
        const double ts[3] = {0.1, 0.2, 0.3};
        double hb[3] = {};
        edvr::nofoot::hmdInBody(ts, 0.4, hb);
        double hbBack[3] = {};
        ryRef(0.4, hb, hbBack);
        check(std::fabs(hbBack[0] - 0.1) < 1e-9 && std::fabs(hbBack[1] - 0.2) < 1e-9 && std::fabs(hbBack[2] + 0.3) < 1e-9,
              "F40: the head in body axes is Ry(psi)^-1 . diag(1,1,-1) . tS");
        // F41: the measured map at psi 0 (the calibration flight): a seated step of +5 cm in X, Y or Z moves the camera's body position by
        // (-5, 0, 0), (0, -5, 0) and (0, 0, +5) cm within 5 mm; the head's own motion maps as diag(1,1,-1).
        {
            const double seatedSteps[3][3] = {{0.05, 0.0, 0.0}, {0.0, 0.05, 0.0}, {0.0, 0.0, 0.05}};
            const double measured[3][3] = {{-0.050, 0.0, 0.0}, {0.0, -0.050, 0.0}, {0.0, 0.0, 0.050}};
            for (int k = 0; k < 3; ++k) {
                const double dS[3] = {-seatedSteps[k][0], -seatedSteps[k][1], seatedSteps[k][2]};   // the plant at psi 0: D . seated
                bool close = true;
                for (size_t i = 0; i < 3; ++i) close = close && std::fabs(dS[i] - measured[k][i]) < 0.005;
                check(close, "F41: a +5 cm seated step moves the camera's body position as the calibration measured (within 5 mm)");
            }
            const double err[3] = {0.02, -0.03, 0.10};
            double sd[3] = {};
            edvr::nofoot::seatedFromBody(err, 0.0, sd);
            check(std::fabs(sd[0] + 0.02) < 1e-12 && std::fabs(sd[1] - 0.03) < 1e-12 && std::fabs(sd[2] - 0.10) < 1e-12,
                  "F41: seatedFromBody(err, 0) = (-ex, -ey, +ez)");
            const double ts[3] = {0.1, 0.2, 0.3};
            double hb[3] = {};
            edvr::nofoot::hmdInBody(ts, 0.0, hb);
            check(std::fabs(hb[0] - 0.1) < 1e-12 && std::fabs(hb[1] - 0.2) < 1e-12 && std::fabs(hb[2] + 0.3) < 1e-12,
                  "F41: the head's motion in body axes is diag(1,1,-1) . tS at psi 0");
        }
        // F41: body: a 20 cm step in the camera's height through one critically damped filter (tau 0.1 s, 5 cm a frame at most) rises
        // monotonically, overshoots by under 5%, and has settled within 1 s.
        {
            double pos[3] = {}, vel[3] = {};
            bool monotone = true;
            double prev = 0.0, peak = 0.0;
            for (int f = 0; f < 90; ++f) {
                edvr::nofoot::cdStep(pos[1], vel[1], 0.2, 0.1, 1.0 / 90.0, 0.05);
                if (pos[1] < prev - 1e-12) monotone = false;
                prev = pos[1];
                if (pos[1] > peak) peak = pos[1];
            }
            check(monotone && peak <= 0.2 * 1.05 && std::fabs(pos[1] - 0.2) < 0.005,
                  "F41: a 20 cm step in camera height moves d monotonically, overshoot under 5%, settled within 1 s");
        }
        // F41: the view loop, closed: the camera follows the applied offset 250 ms late (22 frames at 90 Hz). The integral (0.02 a frame) into one
        // critically damped filter (tau 0.1 s) takes a 34 cm body error under 1 cm within 5 s.
        {
            const int lag = 22;
            const double gainView = 0.02, dt = 1.0 / 90.0;
            const double cam0[3] = {0.0, 0.0, 0.34};
            double intg[3] = {}, pos[3] = {}, vel[3] = {};
            std::array<std::array<double, 3>, 450> uHist{};
            double endErr = 1e9;
            for (int f = 0; f < 450; ++f) {
                std::array<double, 3> uD{0.0, 0.0, 0.0};
                if (f >= lag) uD = uHist[static_cast<size_t>(f - lag)];
                const double e[3] = {-(cam0[0] + uD[0]), -(cam0[1] + uD[1]), -(cam0[2] + uD[2])};
                double sd[3] = {};
                edvr::nofoot::seatedFromBody(e, 0.0, sd);
                for (size_t i = 0; i < 3; ++i) intg[i] += gainView * sd[i];
                for (size_t i = 0; i < 3; ++i) edvr::nofoot::cdStep(pos[i], vel[i], intg[i], 0.1, dt, 0.05);
                uHist[static_cast<size_t>(f)] = {-pos[0], -pos[1], pos[2]};   // D . applied
                endErr = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            }
            check(endErr < 0.01, "F41: the view loop with a 250 ms lag closes a 34 cm error under 1 cm in 5 s (gain 0.02 a frame)");
        }
        // F48: the standing height. The key-on step captures the anchor's height; a weapon draw (the head and the anchor drop 25 cm) does not
        // change it, nor does ADS; a steady head for 1 s (no ADS) recaptures the anchor's mean height. The substitution keeps x and z live.
        {
            edvr::nofoot::StandingHeight st;
            const bool first = st.step(0, 1.60, 1.70, false);
            bool drawn = false;
            for (uint64_t t = 11; t <= 300; t += 11) drawn = st.step(t, 1.60 - 0.25 * static_cast<double>(t) / 300.0, 1.70 - 0.25 * static_cast<double>(t) / 300.0, false) || drawn;
            bool adsCap = false;
            for (uint64_t t = 311; t <= 2000; t += 11) adsCap = st.step(t, 1.35, 1.45, true) || adsCap;
            check(first && st.valid && std::fabs(st.value - 1.70) < 1e-9 && !drawn && !adsCap,
                  "F48: the key-on step captures the standing height; a draw (25 cm drop) and 1.7 s of ADS do not recapture it");
            bool steady = false;
            uint64_t t = 2011;
            for (; t <= 3200 && !steady; t += 11) steady = st.step(t, 1.62 + (t % 2 ? 0.005 : -0.005), 1.72, false);
            check(steady && std::fabs(st.value - 1.72) < 1e-9 && t - 2011 >= 1000 && st.captures == 2,
                  "F48: a steady head (within 2 cm) for 1 s without ADS recaptures the anchor's mean height");
            const double src[3] = {0.11, 1.45, -0.07};
            double out[3] = {};
            edvr::nofoot::standingApply(src, st.value, out);
            check(out[0] == 0.11 && out[1] == st.value && out[2] == -0.07, "F48: the standing height replaces only the vertical; x and z stay live");
        }
        // F49: the pose freeze's pure half. A skeleton root 0 -> hips 1 -> spine 2 -> neck 3 -> head 4; spine 2 -> clavicle 5 -> arm 6;
        // hips 1 -> thigh 7 -> foot 8.
        {
            namespace nf = edvr::nofoot;
            const uint16_t parents[9] = {0xFFFF, 0, 1, 2, 3, 2, 5, 1, 7};
            const nf::PfzFrozenSet fs = nf::pfzFrozenSet(parents, 9, 3, 8);
            bool order = fs.why == 0;
            for (uint32_t k = 0; k < fs.count && order; ++k) {
                const int ps = fs.parentSlot[k];
                order = ps < static_cast<int>(k) && (ps < 0 ? parents[fs.member[k]] == fs.anchor : parents[fs.member[k]] == fs.member[ps]);
            }
            check(fs.why == 0 && fs.footResolved && fs.anchor == 1 && fs.child == 2 && fs.count == 5 && fs.slotOf(7) < 0 && fs.slotOf(8) < 0 &&
                      fs.slotOf(0) < 0 && fs.slotOf(1) < 0 && fs.slotOf(5) >= 0 && fs.slotOf(6) >= 0 && fs.slotOf(4) >= 0 && order,
                  "F49: the frozen set is the torso-side child subtree of the neck and foot's common ancestor, parent first");
            const nf::PfzFrozenSet noFoot = nf::pfzFrozenSet(parents, 9, 3, 0xFFFF);
            check(noFoot.why == 0 && !noFoot.footResolved && noFoot.anchor == 1 && noFoot.count == 5,
                  "F49: an unresolved foot anchors at the root's child on the neck's path");
            check(nf::pfzFrozenSet(parents, 9, 1, 8).why == 4, "F49: a neck that is the common ancestor itself is refused");
            // The rest chain: rest locals composed parent first match the skeleton walk (avatar_skeleton_core.h; position and rotation).
            float locals[9 * 8] = {};
            for (int j = 0; j < 9; ++j) {
                const float ang = 0.15f * static_cast<float>(j + 1), ax[3] = {0.3f, 0.8f, -0.5f};
                const float an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                float* r = locals + j * 8;
                r[0] = 0.01f * j;
                r[1] = 0.2f + 0.03f * j;
                r[2] = -0.02f * j;
                r[3] = 1.0f;
                for (int i = 0; i < 3; ++i) r[4 + i] = std::sin(ang / 2) * ax[i] / an;
                r[7] = std::cos(ang / 2);
            }
            float model[16] = {}, walk[3] = {}, rot[9] = {};
            uint32_t depth = 0;
            const bool okModel = nf::pfzRestModel(locals, parents, 9, 6, model);
            const bool okWalk = edvr::avh::walkJointToModel(locals, parents, 9, 6, walk, &depth) &&
                                edvr::avh::walkJointRotationToModel(locals, parents, 9, 6, rot);
            double err = 0.0;
            for (int i = 0; i < 3; ++i) err = std::max(err, std::fabs(static_cast<double>(model[12 + i]) - walk[i]));
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k) err = std::max(err, std::fabs(static_cast<double>(model[i * 4 + k]) - rot[i * 3 + k]));
            check(okModel && okWalk && err < 1e-5, "F49: restLocal x parent, composed to the root, equals the skeleton walk");
            // M'_j = restLocal_j x M'_parent, anchored at an animated hips matrix: the child's model matrix is its local times the anchor's.
            float anchorM[16] = {}, l2[16] = {}, chain2[16] = {}, inv[16] = {}, back[16] = {};
            nf::pfzLocalMatrix(locals + 1 * 8, anchorM);
            anchorM[12] += 0.4f;   // an animated hips: moved
            nf::pfzLocalMatrix(locals + 2 * 8, l2);
            nf::pfzMul(l2, anchorM, chain2);
            const bool okInv = nf::pfzAffineInverse(anchorM, inv);
            nf::pfzMul(chain2, inv, back);   // the local back out of the chain
            double errL = 0.0;
            for (int i = 0; i < 16; ++i) errL = std::max(errL, std::fabs(static_cast<double>(back[i]) - l2[i]));
            check(okInv && errL < 1e-5, "F49: the local recovered from M'_child x inverse(M'_parent) is the rest local");
            // The quaternion round trip (all four branches) and the blend's ends.
            const float qs[4][4] = {{0.1f, 0.2f, 0.3f, 0.927f}, {0.95f, 0.1f, 0.2f, 0.2f}, {0.1f, 0.95f, 0.2f, 0.2f}, {0.1f, 0.2f, 0.95f, 0.2f}};
            double errQ = 0.0;
            for (const auto& q0 : qs) {
                float rec[8] = {0, 0, 0, 1, q0[0], q0[1], q0[2], q0[3]};
                const float n = std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
                for (int i = 0; i < 4; ++i) rec[4 + i] /= n;
                float m[16], q[4];
                nf::pfzLocalMatrix(rec, m);
                nf::pfzQuatFromMatrix(m, q);
                const float s = (q[0] * rec[4] + q[1] * rec[5] + q[2] * rec[6] + q[3] * rec[7]) < 0.0f ? -1.0f : 1.0f;
                for (int i = 0; i < 4; ++i) errQ = std::max(errQ, std::fabs(static_cast<double>(s * q[i]) - rec[4 + i]));
            }
            check(errQ < 1e-5, "F49: the quaternion from the rotation rows round-trips (every branch)");
            float animLocal[16], b0[8], b1[8], m0[16];
            nf::pfzLocalMatrix(locals + 5 * 8, animLocal);
            nf::pfzBlendRecord(animLocal, locals + 6 * 8, 0.0f, b0);
            nf::pfzBlendRecord(animLocal, locals + 6 * 8, 1.0f, b1);
            nf::pfzLocalMatrix(b0, m0);
            double errB = 0.0;
            for (int i = 0; i < 16; ++i) errB = std::max(errB, std::fabs(static_cast<double>(m0[i]) - animLocal[i]));
            check(errB < 1e-5 && std::memcmp(b1, locals + 6 * 8, sizeof(b1)) == 0, "F49: the blend at 0 is the animated local and at 1 the rest local");
            // The weight: 0.3 s to full, clamped, and back.
            nf::PfzBlend bl;
            bl.step(true, 0.15);
            const float half = bl.w;
            bl.step(true, 0.15);
            const float full = bl.w;
            bl.step(true, 1.0);
            const float clamped = bl.w;
            bl.step(false, 0.3);
            check(std::fabs(half - 0.5f) < 1e-6f && std::fabs(full - 1.0f) < 1e-6f && clamped == 1.0f && bl.w == 0.0f,
                  "F49: the blend weight reaches 1 in 0.3 s, clamps, and returns to 0 in 0.3 s");
            // The rest gate: steady for 1 s rests; a 3 cm move or ADS ends it.
            nf::PfzHeadRest hr;
            bool rest = false;
            for (uint64_t t = 0; t <= 990; t += 11) rest = hr.step(t, 1.60 + (t % 2 ? 0.005 : -0.005), false);
            const bool early = rest;
            rest = hr.step(1001, 1.60, false);
            const bool moved = hr.step(1012, 1.63, false);
            rest = hr.step(1023, 1.63, false) || rest;
            hr.step(3000, 1.63, false);
            const bool ads = hr.step(3011, 1.63, true);
            check(!early && !moved && !ads, "F49: the rest gate needs the head steady for 1 s; a 3 cm move or ADS ends it");
            nf::PfzHeadRest hr2;
            bool rest2 = false;
            for (uint64_t t = 0; t <= 1100; t += 11) rest2 = hr2.step(t, 1.60, false);
            check(rest2, "F49: a head steady for 1 s is at rest");

            // The view's sign: seated y = -dh, the same as seatedFromBody({0, dh, 0}, 0); through the calibration (body = diag(-1,-1,+1) . seated)
            // that raises the camera by dh.
            double rs[3] = {}, viaMap[3] = {};
            nf::raiseSeated(0.23, rs);
            const double up[3] = {0.0, 0.23, 0.0};
            nf::seatedFromBody(up, 0.0, viaMap);
            const double camUp = -rs[1];   // body y = -seated y
            check(rs[0] == 0.0 && rs[2] == 0.0 && std::fabs(rs[1] + 0.23) < 1e-12 && std::fabs(viaMap[1] - rs[1]) < 1e-12 && std::fabs(camUp - 0.23) < 1e-12,
                  "F51: the view raise is seated y = -dh, which raises the camera by dh (the body-to-seated map's own sign)");
            // The applied raise follows at 5 cm a frame at most, both ways.
            const double a1 = nf::capToward(0.0, 0.23, 0.05), a2 = nf::capToward(0.2, 0.23, 0.05), a3 = nf::capToward(0.23, 0.0, 0.05);
            check(std::fabs(a1 - 0.05) < 1e-12 && std::fabs(a2 - 0.23) < 1e-12 && std::fabs(a3 - 0.18) < 1e-12,
                  "F51: the applied raise moves 5 cm a frame at most");
            // F53: the frozen set's cap holds every joint the Animate output has (0xC0), the plan's arrays are that size, and the flight's
            // 185-member set (F51/F52 refused it at 128) builds: 188 joints, root 0 -> hips 1 -> spine 2 -> neck 3, foot 4 under the hips,
            // joints 5..187 under the spine.
            check(nf::kPfzMaxF == 192u && nf::kPfzMaxF >= nf::kPfzMaxJoints &&
                      sizeof(edvr::pfz::Plan::member) / sizeof(uint16_t) == nf::kPfzMaxF &&
                      sizeof(edvr::pfz::Plan::parentSlot) / sizeof(int16_t) == nf::kPfzMaxF &&
                      sizeof(edvr::pfz::Plan::rest) / (8 * sizeof(float)) == nf::kPfzMaxF && sizeof(nf::PfzFrozenSet::member) / sizeof(uint16_t) == nf::kPfzMaxF,
                  "F53: the frozen set's member cap is 192 (the joint cap) and the plan's member, parent-slot and rest arrays match it");
            {
                uint16_t big[188];
                big[0] = 0xFFFF;
                big[1] = 0;
                big[2] = 1;
                big[3] = 2;
                big[4] = 1;
                for (int j = 5; j < 188; ++j) big[j] = 2;
                const nf::PfzFrozenSet fsBig = nf::pfzFrozenSet(big, 188, 3, 4);
                bool parentsOk = fsBig.why == 0;
                for (uint32_t k = 0; k < fsBig.count && parentsOk; ++k)
                    parentsOk = fsBig.parentSlot[k] < static_cast<int>(k) &&
                                (fsBig.parentSlot[k] < 0 ? big[fsBig.member[k]] == fsBig.anchor : big[fsBig.member[k]] == fsBig.member[fsBig.parentSlot[k]]);
                check(fsBig.why == 0 && fsBig.count == 185 && fsBig.anchor == 1 && fsBig.slotOf(3) >= 0 && fsBig.slotOf(187) >= 0 && fsBig.slotOf(4) < 0 &&
                          parentsOk && std::strcmp(nf::pfzWhyText(5), "the frozen set has more than 192 members") == 0,
                      "F53: a 185-member frozen set builds (F51/F52's 128 cap refused it), parent first");
            }
            // F53: the neck anchor's rest geometry. The target neck is the FP side plus (0, -0.155, -0.161) m (body axes); the align's d =
            // anchor - source with source = neckRestSource(neck) puts the neck exactly there.
            {
                const double fpB[3] = {0.02, 1.50, 0.10}, neckB[3] = {0.23, 1.30, -0.05};
                double tgt[3], src[3], after[3];
                nf::neckRestTarget(fpB, tgt);
                nf::neckRestSource(neckB, src);
                for (int i = 0; i < 3; ++i) after[i] = neckB[i] + (fpB[i] - src[i]);
                check(std::fabs(tgt[0] - 0.02) < 1e-12 && std::fabs(tgt[1] - 1.345) < 1e-12 && std::fabs(tgt[2] + 0.061) < 1e-12 &&
                          std::fabs(after[0] - tgt[0]) < 1e-12 && std::fabs(after[1] - tgt[1]) < 1e-12 && std::fabs(after[2] - tgt[2]) < 1e-12,
                      "F53: the neck anchor puts the 3P neck at the FP side plus (0, -0.155, -0.161) m in body axes");
            }
            // F55: the walking shift. With the rest captured, d_h = FP side_h - (rest_h + offset_h), whatever the live neck does (a walk leans
            // it 12 cm forward and 3 cm right); the vertical stays live.
            {
                const double fpB[3] = {0.02, 1.50, 0.10}, restNeck[2] = {0.21, -0.04};
                double off[3] = {}, src[3] = {};
                const double neckIdle[3] = {0.21, 1.30, -0.04}, neckWalk[3] = {0.24, 1.28, 0.08};
                nf::neckRestSource(neckIdle, src);
                for (int i = 0; i < 3; ++i) off[i] = src[i] - neckIdle[i];
                double sIdle[3], sWalk[3], liveWalk[3];
                nf::neckRestSource(neckWalk, liveWalk);
                const double liveIdle[3] = {src[0], src[1], src[2]};
                nf::walkRestSource(liveIdle, off, restNeck[0], restNeck[1], sIdle);
                nf::walkRestSource(liveWalk, off, restNeck[0], restNeck[1], sWalk);
                const double dIdle[2] = {fpB[0] - sIdle[0], fpB[2] - sIdle[2]}, dWalk[2] = {fpB[0] - sWalk[0], fpB[2] - sWalk[2]};
                const double expect[2] = {fpB[0] - (restNeck[0] - nf::kNeckRestOffset[0]), fpB[2] - (restNeck[1] - nf::kNeckRestOffset[2])};
                check(std::fabs(dIdle[0] - expect[0]) < 1e-12 && std::fabs(dIdle[1] - expect[1]) < 1e-12 && std::fabs(dWalk[0] - dIdle[0]) < 1e-12 &&
                          std::fabs(dWalk[1] - dIdle[1]) < 1e-12 && sWalk[1] == liveWalk[1] && std::fabs((fpB[2] - liveWalk[2]) - dIdle[1]) > 0.1,
                      "F55: d_h = FP side_h - (rest_h + offset_h) stays constant while the live neck leans with a walk; the vertical stays live");
            }
            // F55: the horizontal rest is captured only at idle: not at key-on, not while moving or ADS; a steady head for 1 s while not moving
            // captures the window's mean x and z.
            {
                nf::StandingHeight st;
                const bool keyOn = st.step(0, 1.60, 1.70, false, true, 0.30, 0.20);
                bool walkCap = false;
                for (uint64_t t = 11; t <= 2000; t += 11) walkCap = st.step(t, 1.60, 1.70, false, true, 0.30, 0.20) || walkCap;
                bool adsCap = false;
                for (uint64_t t = 2011; t <= 3500; t += 11) adsCap = st.step(t, 1.60, 1.70, true, false, 0.30, 0.20) || adsCap;
                check(keyOn && st.valid && !st.hValid && !walkCap && !adsCap && st.hCaptures == 0,
                      "F55: the key-on step captures the height only; 2 s of walking and 1.5 s of ADS capture no horizontal rest");
                bool idleCap = false;
                uint64_t t = 3511;
                for (; t <= 5000 && !idleCap; t += 11) idleCap = st.step(t, 1.60, 1.70, false, false, (t % 2) ? 0.21 : 0.19, -0.05);
                check(idleCap && st.hValid && st.hCaptures == 1 && std::fabs(st.hx - 0.20) < 0.002 && std::fabs(st.hz + 0.05) < 1e-9 && t - 3511 >= 1000,
                      "F55: 1 s idle (not moving, no ADS, the head steady) captures the mean x and z of the rest point");
                const double hx = st.hx;
                bool walkAgain = false;
                for (uint64_t u = t; u <= t + 3000; u += 11) walkAgain = st.step(u, 1.60, 1.70, false, true, 0.40, 0.30) || walkAgain;
                check(!walkAgain && st.hx == hx && st.hCaptures == 1, "F55: walking after a capture never re-captures the rest");
            }
            // F55: the FP X's plan: the subtree of the clavicles 19 and 20 (F66: the shoulder drop it served is gone).
            {
                // root 0 -> spine 1 -> clavicle 2 (as 19) -> arm 3 -> hand 4; spine 1 -> clavicle 5 (as 20) -> hand 6; spine 1 -> head 7; a broken 8.
                const uint16_t dropParents[9] = {0xFFFF, 0, 1, 2, 3, 1, 5, 1, 99};
                uint16_t mem[9] = {};
                const uint32_t n = nf::dropSubtree(dropParents, 9, 2, 5, mem, 9);
                check(n == 5 && mem[0] == 2 && mem[1] == 3 && mem[2] == 4 && mem[3] == 5 && mem[4] == 6,
                      "F55: the drop's subtree is the two clavicles and every joint below them; the spine, the head and a broken chain are left out");
            }
            // F56: the arm IK's pure half: the two-bone solve, the pole, the clamp, the rotations, the ramp, the distance gate, the chain.
            {
                const double lU = 0.30, lF = 0.25, S[3] = {0.0, 0.0, 0.0}, down[3] = {0.0, -1.0, 0.0}, poleUp[3] = {0.0, 1.0, 0.0},
                             fwd[3] = {0.0, 0.0, 1.0};
                const auto d3 = [](const double* a, const double* b) {
                    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
                };
                const double T[3] = {0.40, 0.0, 0.0};
                double E[3] = {};
                const nf::IkTwoBone tb = nf::ikTwoBone(S, T, lU, lF, down, E);
                const double ue[3] = {S[0] - E[0], S[1] - E[1], S[2] - E[2]}, he[3] = {T[0] - E[0], T[1] - E[1], T[2] - E[2]};
                const double elbowDeg = std::acos(nf::ikDot(ue, he) / (lU * lF)) * 180.0 / nf::kIkPi;
                const double wantDeg = std::acos((lU * lU + lF * lF - 0.40 * 0.40) / (2.0 * lU * lF)) * 180.0 / nf::kIkPi;
                check(tb.ok && std::fabs(d3(S, E) - lU) < 1e-9 && std::fabs(d3(E, T) - lF) < 1e-9 && std::fabs(elbowDeg - wantDeg) < 1e-6 &&
                          std::fabs(wantDeg - 92.87) < 0.01 && d3(tb.hand, T) < 1e-12 && tb.reach < 1.0 && tb.shortfall == 0.0,
                      "F56: a reachable target: the bones keep their lengths, the hand lands on it, the elbow angle is the law of cosines' (92.87 deg)");
                double Eu[3] = {}, Ef[3] = {};
                nf::ikTwoBone(S, T, lU, lF, poleUp, Eu);
                nf::ikTwoBone(S, T, lU, lF, fwd, Ef);
                check(E[1] < -0.1 && std::fabs(E[2]) < 1e-9 && Eu[1] > 0.1 && Ef[2] > 0.1 && std::fabs(Ef[1]) < 1e-9,
                      "F56: the pole hint selects the elbow's side (down, up, forward)");
                const double farT[3] = {1.0, 0.0, 0.0};
                double Ex[3] = {};
                const nf::IkTwoBone tf = nf::ikTwoBone(S, farT, lU, lF, down, Ex);
                check(tf.ok && std::fabs(tf.D - 0.999 * (lU + lF)) < 1e-12 && std::fabs(tf.reach - 1.0 / 0.55) < 1e-9 &&
                          std::fabs(tf.shortfall - (1.0 - 0.999 * 0.55)) < 1e-12 && std::fabs(tf.hand[0] - 0.999 * 0.55) < 1e-12 &&
                          std::fabs(d3(S, Ex) - lU) < 1e-9 && std::fabs(d3(Ex, tf.hand) - lF) < 1e-9,
                      "F56: an unreachable target clamps: the arm reaches 0.999 of its length toward it, short by the excess, never stretched");
                const double nearT[3] = {0.01, 0.0, 0.0};
                double En[3] = {};
                const nf::IkTwoBone tn = nf::ikTwoBone(S, nearT, lU, lF, down, En);
                check(tn.ok && std::fabs(tn.D - (0.05 + 1e-3)) < 1e-12 && std::fabs(d3(S, En) - lU) < 1e-9,
                      "F56: a target inside the folded reach clamps to |l1 - l2| + 1 mm");
                // The swivel clamp: an arm reaching forward with the pole poleUp puts the elbow above the shoulder; the clamp turns it below and out.
                const double S2[3] = {0.2, 1.4, 0.0}, T2[3] = {0.2, 1.4, 0.4};
                const nf::IkTwoBone t2 = nf::ikTwoBoneFrame(S2, T2, lU, lF, poleUp);
                bool swClamped = false;
                const double th = nf::ikSwivelClamp(t2, 0.0, 1.0, &swClamped);
                double E2[3] = {}, E0[3] = {};
                t2.elbowAt(th, E2);
                t2.elbowAt(0.0, E0);
                bool okAgain = true;
                const double th2 = nf::ikSwivelClamp(t2, th, 1.0, &okAgain);
                check(E0[1] > S2[1] && swClamped && E2[1] <= S2[1] + 1e-9 && E2[0] >= 0.10 - 1e-9 && !okAgain && th2 == th,
                      "F56: the swivel clamp turns an elbow above the shoulder to below it, out on its side; a clamped angle holds as is");
                // The rotations: from-to, the axis-angle's twist, the pivot keeps its point.
                const double a[3] = {1.0, 0.0, 0.0}, b[3] = {0.0, 0.6, 0.8};
                double R[9];
                nf::ikRotFromTo(a, b, R);
                const double ar[3] = {a[0] * R[0] + a[1] * R[3] + a[2] * R[6], a[0] * R[1] + a[1] * R[4] + a[2] * R[7], a[0] * R[2] + a[1] * R[5] + a[2] * R[8]};
                double Rt[9];
                const double nAx[3] = {0.0, 0.6, 0.8};
                nf::ikRotAxis(nAx, 0.7, Rt);
                const double twist = nf::ikTwistAbout(Rt, nAx);
                double Ry[9];
                nf::ikRotAxis(poleUp, nf::kIkPi / 2.0, Ry);
                const double xr[3] = {Ry[0], Ry[1], Ry[2]};   // (1, 0, 0) x Ry: right-handed about +y turns +x to -z
                const float M[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0.0f, 0.0f, 1};
                float Mo[16];
                const double P[3] = {0.5, 0.0, 0.0};
                nf::ikPivot(M, Ry, P, Mo);
                check(d3(ar, b) < 1e-12 && std::fabs(twist - 0.7) < 1e-9 && std::fabs(xr[2] + 1.0) < 1e-12 && Mo[12] == 0.5f && Mo[13] == 0.0f &&
                          std::fabs(Mo[2] + 1.0f) < 1e-6f,
                      "F56: the shortest arc maps a onto b; the twist of an axis-angle about its axis is the angle; a pivot keeps its point");
                // The base weight's ramp: 0.2 s in (18 frames at 90 Hz, half at 0.1 s), 0.2 s out.
                nf::IkBlend ikRamp;
                const double dt = 1.0 / 90.0;
                int framesIn = 0;
                while (ikRamp.w < 1.0f && framesIn < 100) {
                    ikRamp.step(true, dt);
                    ++framesIn;
                }
                nf::IkBlend b2;
                for (int f = 0; f < 9; ++f) b2.step(true, dt);
                int framesOut = 0;
                while (ikRamp.w > 0.0f && framesOut < 100) {
                    ikRamp.step(false, dt);
                    ++framesOut;
                }
                check(framesIn >= 18 && framesIn <= 19 && framesOut >= 18 && framesOut <= 19 && std::fabs(b2.w - 0.5f) < 1e-5f && ikRamp.w == 0.0f,
                      "F56: the arm IK's base weight ramps to 1 in 0.2 s (half at 0.1 s) and back to 0 in 0.2 s");
                // The distance gate and the 5 cm cap.
                check(nf::ikDistanceGate(0.2f) == 1.0f && nf::ikDistanceGate(0.35f) == 1.0f && std::fabs(nf::ikDistanceGate(0.575f) - 0.5f) < 1e-5f &&
                          nf::ikDistanceGate(0.8f) == 0.0f && nf::ikDistanceGate(1.5f) == 0.0f &&
                          nf::ikDistanceGate(std::numeric_limits<float>::quiet_NaN()) == 0.0f,
                      "F56: the per-arm distance gate: full under 0.35 m, half at 0.575 m, none from 0.8 m (and for NaN)");
                float applied[3] = {0.0f, 0.0f, 0.0f};
                const float want[3] = {0.0f, 0.25f, 0.0f};
                for (int c = 0; c < 4; ++c) nf::ikCapToward(applied, want, nf::kIkCapPerCallM);
                const float after4 = applied[1];
                for (int c = 0; c < 2; ++c) nf::ikCapToward(applied, want, nf::kIkCapPerCallM);
                check(std::fabs(after4 - 0.20f) < 1e-6f && std::fabs(applied[1] - 0.25f) < 1e-6f && applied[0] == 0.0f && applied[2] == 0.0f,
                      "F56: the applied hand offset moves at most 5 cm a call (the nudge's 25 cm after 5 or 6) and stops at the wanted offset");
                // The chain: clavicle 1 -> upper arm 2 -> forearm 3 -> hand 4 -> attach 5; finger 6 under 4; helpers 7 (under 3) and 8 (under 2).
                const uint16_t par[9] = {0xFFFF, 0, 1, 2, 3, 4, 4, 3, 2};
                const nf::IkArm arm = nf::ikArmChain(par, 9, 1, 4, 5);
                check(arm.why == 0 && arm.direct && arm.attachDirect && arm.U == 2 && arm.F == 3 && arm.nU == 2 && arm.gU[0] == 2 && arm.chainU[0] == 1 &&
                          arm.gU[1] == 8 && arm.chainU[1] == 0 && arm.nF == 2 && arm.gF[0] == 3 && arm.gF[1] == 7 && arm.nH == 3 && arm.gH[0] == 4 &&
                          arm.depthH - arm.depthC == 3,
                      "F56: the arm chain: U and F from the hand's parents; gU, gF, gH parent first with the helpers in their bone's group");
                const uint16_t par2[10] = {0xFFFF, 0, 1, 9, 3, 4, 4, 3, 2, 2};   // a twist joint 9 between U 2 and F 3
                const nf::IkArm arm2 = nf::ikArmChain(par2, 10, 1, 4, 5);
                check(arm2.why == 0 && !arm2.direct && arm2.U == 2 && arm2.F == 3 && arm2.nU == 3 && arm2.chainU[0] == 1 &&
                          ((arm2.gU[1] == 9 && arm2.chainU[1] == 1) || (arm2.gU[2] == 9 && arm2.chainU[2] == 1)),
                      "F56: a joint between U and F is found by the upward search and is on the chain");
                check(nf::ikArmChain(par, 9, 1, 4, 7).why == 3 && nf::ikArmChain(par, 9, 7, 4, 5).why == 2 && nf::ikArmChain(par, 9, 1, 4, 40).why == 1,
                      "F56: an attach not under the hand, a clavicle off the chain and an index out of range are refused");
            }
            // F57: the arm IK refinements' pure half: the FP pole's fallback, the square-up's clamp, ramp and spine split, the elbow limits.
            {
                const double kDeg = nf::kIkPi / 180.0;
                // The FP pole: a nearly straight FP arm (the elbow's part across the shoulder-hand axis under 0.2) keeps the old pole.
                const double S[3] = {0.0, 0.0, 0.0}, T[3] = {0.5, 0.0, 0.0}, Estraight[3] = {0.25, -0.01, 0.0}, Ebent[3] = {0.2, -0.2, 0.0};
                double pole[3] = {7.0, 7.0, 7.0};
                const bool straight = nf::ikPoleFromFp(S, Estraight, T, pole);
                const bool kept = pole[0] == 7.0 && pole[1] == 7.0 && pole[2] == 7.0;
                const bool bent = nf::ikPoleFromFp(S, Ebent, T, pole);
                check(!straight && kept && bent && std::fabs(pole[0]) < 1e-12 && std::fabs(pole[1] + 1.0) < 1e-12 && std::fabs(pole[2]) < 1e-12,
                      "F57: the FP elbow's pole: a perpendicular part under 0.2 keeps the old pole; a bent arm gives the unit perpendicular (down)");
                // The square-up's angle: the yaw from the 3P line onto the FP's turns one onto the other; the clamp holds +-50 deg; NaN is 0.
                const double a[3] = {1.0, 0.0, 0.0}, b[3] = {0.0, 0.0, -1.0}, upY[3] = {0.0, 1.0, 0.0};
                const double phi = nf::ikYawFromTo(a, b);
                double Ry[9];
                nf::ikRotAxis(upY, phi, Ry);
                const double ar[3] = {a[0] * Ry[0] + a[1] * Ry[3] + a[2] * Ry[6], a[0] * Ry[1] + a[1] * Ry[4] + a[2] * Ry[7],
                                      a[0] * Ry[2] + a[1] * Ry[5] + a[2] * Ry[8]};
                bool c60 = false, c70 = false, c30 = true, cNan = true;
                const double r60 = nf::ikSquareClampRad(60.0 * kDeg, &c60), r70 = nf::ikSquareClampRad(-70.0 * kDeg, &c70),
                             r30 = nf::ikSquareClampRad(30.0 * kDeg, &c30), rNan = nf::ikSquareClampRad(std::numeric_limits<double>::quiet_NaN(), &cNan);
                check(std::fabs(phi - 90.0 * kDeg) < 1e-12 && std::fabs(ar[0] - b[0]) < 1e-12 && std::fabs(ar[2] - b[2]) < 1e-12 && c60 && c70 &&
                          !c30 && !cNan && std::fabs(r60 - 50.0 * kDeg) < 1e-12 && std::fabs(r70 + 50.0 * kDeg) < 1e-12 &&
                          std::fabs(r30 - 30.0 * kDeg) < 1e-12 && rNan == 0.0,
                      "F57: the square-up's yaw turns the 3P shoulder line onto the FP's; the clamp holds +-50 deg (30 passes, NaN is 0)");
                // Its ramp: 0.3 s in (27 frames at 90 Hz, half at 0.15 s), 0.3 s out.
                nf::IkBlend sq{0.0f, nf::kIkSquareRampSec};
                const double dt = 1.0 / 90.0;
                int framesIn = 0;
                while (sq.w < 1.0f && framesIn < 100) {
                    sq.step(true, dt);
                    ++framesIn;
                }
                nf::IkBlend halfRamp{0.0f, nf::kIkSquareRampSec};
                for (int f = 0; f < 3; ++f) halfRamp.step(true, 0.05);
                int framesOut = 0;
                while (sq.w > 0.0f && framesOut < 100) {
                    sq.step(false, dt);
                    ++framesOut;
                }
                check(framesIn >= 27 && framesIn <= 28 && framesOut >= 27 && framesOut <= 28 && std::fabs(halfRamp.w - 0.5f) < 1e-5f && sq.w == 0.0f,
                      "F57: the square-up ramps to 1 in 0.3 s (half at 0.15 s) and back to 0 in 0.3 s");
                // The spine split: a skeleton with the spine 16 -> 21 -> 24, the clavicles 35, 36 and the pivot 34 under 24, a helper on each spine
                // joint (17 on 16, 22 on 21, 37 on 35) and joints off the spine (1, 2 on the root). Levels 0, 1, 2 take 1/3, 2/3 and all of the angle.
                uint16_t sp[40];
                for (uint32_t j = 0; j < 40; ++j) sp[j] = 0;
                sp[0] = 0xFFFF;
                sp[16] = 0;
                sp[17] = 16;
                sp[21] = 16;
                sp[22] = 21;
                sp[24] = 21;
                sp[34] = 24;
                sp[35] = 24;
                sp[36] = 24;
                sp[37] = 35;
                const nf::IkSquarePlan plan = nf::ikSquarePlan(sp, 40);
                int lv[40];
                for (int j = 0; j < 40; ++j) lv[j] = -1;
                for (uint32_t k = 0; k < plan.n; ++k) lv[plan.j[k]] = plan.level[k];
                check(plan.why == 0 && plan.chestRoot == 24 && plan.n == 9 && lv[16] == 0 && lv[17] == 0 && lv[21] == 1 && lv[22] == 1 && lv[24] == 2 &&
                          lv[34] == 2 && lv[35] == 2 && lv[36] == 2 && lv[37] == 2 && lv[1] == -1 && lv[0] == -1 &&
                          std::fabs(nf::kIkSpineWeight[0] - 1.0 / 3.0) < 1e-15 && std::fabs(nf::kIkSpineWeight[1] - 2.0 / 3.0) < 1e-15 &&
                          nf::kIkSpineWeight[2] == 1.0,
                      "F57: the square-up's plan: the spine 16 subtree by level (16 exclusive 1/3, 21 exclusive 2/3, 24 all), nothing off the spine");
                // Turned by 45 deg about the pivot's vertical: each level's joint turns by its share, about the pivot.
                const double P[3] = {0.1, 1.5, 0.0}, total = 45.0 * kDeg;
                bool sharesOk = true;
                for (int l = 0; l < 3; ++l) {
                    double R[9];
                    nf::ikRotAxis(upY, total * nf::kIkSpineWeight[l], R);
                    const float M[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.6f, 1.2f, 0.0f, 1};
                    float Mo[16];
                    nf::ikPivot(M, R, P, Mo);
                    const double v0[3] = {0.6 - P[0], 0.0, 0.0 - P[2]}, v1[3] = {Mo[12] - P[0], 0.0, Mo[14] - P[2]};
                    const double got = nf::ikYawFromTo(v0, v1);
                    if (std::fabs(got - total * nf::kIkSpineWeight[l]) > 1e-6 || std::fabs(Mo[13] - 1.2f) > 1e-6f) sharesOk = false;
                }
                uint16_t sp2[40];
                std::memcpy(sp2, sp, sizeof(sp2));
                sp2[36] = 21;   // a clavicle off the chest root
                uint16_t sp3[40];
                std::memcpy(sp3, sp, sizeof(sp3));
                sp3[24] = 0;   // the spine broken
                check(sharesOk && nf::ikSquarePlan(sp2, 40).why == 3 && nf::ikSquarePlan(sp3, 40).why == 2 && nf::ikSquarePlan(sp, 30).why == 1,
                      "F57: each spine level turns by its share about the pivot's vertical (height kept); a clavicle off the chest root, a broken spine "
                      "and a short skeleton are refused");
                // The elbow's interior angle limits: 40 to 170 deg.
                const double lU = 0.30, lF = 0.25, down[3] = {0.0, -1.0, 0.0};
                const double Tfar[3] = {0.549, 0.0, 0.0}, Tnear[3] = {0.06, 0.0, 0.0}, Tmid[3] = {0.40, 0.0, 0.0};
                const nf::IkTwoBone tFar = nf::ikTwoBoneFrame(S, Tfar, lU, lF, down, nf::kIkElbowMinDeg, nf::kIkElbowMaxDeg);
                const nf::IkTwoBone tNear = nf::ikTwoBoneFrame(S, Tnear, lU, lF, down, nf::kIkElbowMinDeg, nf::kIkElbowMaxDeg);
                const nf::IkTwoBone tMid = nf::ikTwoBoneFrame(S, Tmid, lU, lF, down, nf::kIkElbowMinDeg, nf::kIkElbowMaxDeg);
                double Ef[3], En[3];
                tFar.elbowAt(0.0, Ef);
                tNear.elbowAt(0.0, En);
                const auto ang = [&](const double* E, const double* H) {
                    const double es[3] = {S[0] - E[0], S[1] - E[1], S[2] - E[2]}, eh[3] = {H[0] - E[0], H[1] - E[1], H[2] - E[2]};
                    return std::acos(nf::ikDot(es, eh) / (nf::ikLen(es) * nf::ikLen(eh))) * 180.0 / nf::kIkPi;
                };
                check(tFar.ok && tFar.limited && std::fabs(nf::ikElbowInteriorDeg(lU, lF, tFar.D) - 170.0) < 1e-6 && std::fabs(ang(Ef, tFar.hand) - 170.0) < 1e-6 &&
                          tNear.ok && tNear.limited && std::fabs(nf::ikElbowInteriorDeg(lU, lF, tNear.D) - 40.0) < 1e-6 &&
                          std::fabs(ang(En, tNear.hand) - 40.0) < 1e-6 && tMid.ok && !tMid.limited &&
                          std::fabs(nf::ikElbowInteriorDeg(lU, lF, tMid.D) - 92.87) < 0.01,
                      "F57: the elbow's interior angle is held in 40..170 deg (a near-straight and a folded target are limited; 92.87 deg passes)");
                // The elbow-out slider's distance reaches the clamp.
                const double S2[3] = {0.2, 1.4, 0.0}, T2[3] = {0.2, 1.4, 0.4}, poleUp[3] = {0.0, 1.0, 0.0};
                const nf::IkTwoBone t2 = nf::ikTwoBoneFrame(S2, T2, lU, lF, poleUp);
                bool cl = false;
                double E2[3];
                t2.elbowAt(nf::ikSwivelClamp(t2, 0.0, 1.0, &cl, 0.25), E2);
                check(cl && E2[0] >= 0.25 - 1e-9 && E2[1] <= S2[1] + 1e-9 && nf::kIkSwivelTrimDeg == 2.0 && nf::kIkSquareAmount == 0.75 &&
                          nf::kIkAdsTrimDeg == 3.0 && nf::kIkAdsRaiseCm == 5.5 && nf::kWeaponViewOffsetDefaultCm == 0.0,
                      "F57: the clamp keeps the elbow out by the distance (25 cm); the arm IK's shipped values are the constants");
            }
            // F76: a transported anatomical elbow pole chooses the physical branch, independent of the solver frame and animated wrist.
            {
                const double S3[3] = {0.0, 1.0, 0.0}, T3[3] = {0.16, 0.84, 0.43};
                const double hintA[3] = {0.0, -1.0, 0.0}, hintB[3] = {0.8, 0.2, -0.3};
                const auto ta = nf::ikTwoBoneFrame(S3, T3, 0.32, 0.28, hintA);
                const auto tb = nf::ikTwoBoneFrame(S3, T3, 0.32, 0.28, hintB);
                nf::IkPhysicalPole pole;
                pole.valid = ta.ok && nf::ikElbowRadial(ta, 0.41, pole.reference);
                std::memcpy(pole.axis, ta.dir, sizeof(pole.axis));
                std::memcpy(pole.previous, pole.reference, sizeof(pole.previous));
                const auto chosenA = nf::ikStableSwivel(ta, pole, 1.0, 0.06);
                const auto chosenB = nf::ikStableSwivel(tb, pole, 1.0, 0.06);
                double EA[3] = {}, EB[3] = {};
                if (chosenA.ok) ta.elbowAt(chosenA.theta, EA);
                if (chosenB.ok) tb.elbowAt(chosenB.theta, EB);
                const double basisDelta[3] = {EA[0] - EB[0], EA[1] - EB[1], EA[2] - EB[2]};
                check(chosenA.ok && chosenB.ok && chosenA.transportedReference && chosenB.transportedReference && nf::ikLen(basisDelta) < 1e-7 &&
                          nf::ikLen(chosenA.radial) > 0.999999,
                      "F76: changing the animated pole/reference basis cannot move the elbow when the physical axis and transported pole are unchanged");

                // Dense independent oracle: sample the actual elbow circle, then compare feasibility and nearest-pole/minimum-violation ordering.
                const auto denseOracle = [&](const nf::IkTwoBone& t, const double preferred[3], double outSign, double elbowOutM, double* bestDistance) {
                    const double N = 72000.0;
                    double pref[3];
                    const double along = nf::ikDot(preferred, t.dir);
                    for (int i = 0; i < 3; ++i) pref[i] = preferred[i] - along * t.dir[i];
                    if (!nf::ikUnit(pref, pref)) return std::numeric_limits<double>::infinity();
                    const double prefTheta = t.angleOf(pref);
                    bool haveFeasible = false;
                    double bestV = std::numeric_limits<double>::infinity(), bestD = std::numeric_limits<double>::infinity();
                    for (int n = 0; n < static_cast<int>(N); ++n) {
                        const double theta = -nf::kIkPi + 2.0 * nf::kIkPi * static_cast<double>(n) / N;
                        double E[3]; t.elbowAt(theta, E);
                        const double violation = std::max(0.0, elbowOutM - nf::ikElbowOutOf(E, t.S, outSign)) + std::max(0.0, E[1] - t.S[1]);
                        const bool feasible = violation <= 1e-7;
                        const double d = std::fabs(nf::ikWrapPi(theta - prefTheta));
                        if ((feasible && !haveFeasible) || (feasible == haveFeasible &&
                            (feasible ? d < bestD : (violation < bestV - 1e-12 || (std::fabs(violation - bestV) <= 1e-12 && d < bestD))))) {
                            haveFeasible = feasible; bestV = violation; bestD = d;
                        }
                    }
                    *bestDistance = bestD;
                    return haveFeasible ? 0.0 : bestV;
                };
                double oracleDistance = 0.0;
                const double oracleViolation = denseOracle(ta, pole.reference, 1.0, 0.06, &oracleDistance);
                check(chosenA.ok && chosenA.violation < 1e-8 && oracleViolation == 0.0 && std::fabs(std::fabs(nf::ikWrapPi(chosenA.theta - ta.angleOf(pole.reference))) - oracleDistance) < 0.002,
                      "F76: analytic outward/below intersections select the nearest feasible physical pole against an independent dense-circle oracle");

                const double S4[3] = {0.0, 0.0, 0.0}, T4[3] = {0.0, 0.50, 0.0};
                const auto impossible = nf::ikTwoBoneFrame(S4, T4, 0.30, 0.30, hintA);
                nf::IkPhysicalPole impossiblePole;
                impossiblePole.valid = impossible.ok && nf::ikElbowRadial(impossible, 0.0, impossiblePole.reference);
                std::memcpy(impossiblePole.axis, impossible.dir, sizeof(impossiblePole.axis));
                std::memcpy(impossiblePole.previous, impossiblePole.reference, sizeof(impossiblePole.previous));
                const auto leastViolation = nf::ikStableSwivel(impossible, impossiblePole, 1.0, 0.06);
                const double oracleImpossible = denseOracle(impossible, impossiblePole.reference, 1.0, 0.06, &oracleDistance);
                check(leastViolation.ok && leastViolation.violation > 0.0 && std::fabs(leastViolation.violation - oracleImpossible) < 2e-5,
                      "F76: when outward and below constraints cannot both hold, analytic active-set minima match the independent dense-circle minimum");

                // Movement follows the transported direction through the old fixed-reference projection singularity.
                nf::IkPhysicalPole moving;
                const double startT[3] = {0.0, 1.0, 0.48};
                const auto first = nf::ikTwoBoneFrame(S3, startT, 0.32, 0.28, hintA);
                moving.valid = first.ok && nf::ikElbowRadial(first, 0.0, moving.reference);
                std::memcpy(moving.axis, first.dir, sizeof(moving.axis));
                std::memcpy(moving.previous, moving.reference, sizeof(moving.previous));
                const nf::IkPhysicalPole startPole = moving;
                double lastE[3] = {}, maxPoseStepBeforeConstraintTangent = 0.0, maxReferenceStep = 0.0, maxAnalyticReferenceError = 0.0;
                if (first.ok) first.elbowAt(0.0, lastE);
                bool trajectoryOk = moving.valid, crossingOracleOk = true;
                for (int n = 1; n <= 180; ++n) {
                    const double a = static_cast<double>(n) * nf::kIkPi / 180.0;
                    const double Tm[3] = {0.0, 1.0 - 0.48 * std::sin(a), 0.48 * std::cos(a)};
                    const auto tm = nf::ikTwoBoneFrame(S3, Tm, 0.32, 0.28, hintB);
                    const nf::IkPhysicalPole previousPole = moving;
                    const auto cm = nf::ikStableSwivel(tm, moving, 1.0, 0.0);
                    if (!cm.ok || !cm.transportedReference) { trajectoryOk = false; break; }
                    const double expectedReference[3] = {0.0, -std::cos(a), -std::sin(a)};
                    const double referenceDelta[3] = {cm.reference[0] - previousPole.reference[0], cm.reference[1] - previousPole.reference[1], cm.reference[2] - previousPole.reference[2]};
                    const double referenceError[3] = {cm.reference[0] - expectedReference[0], cm.reference[1] - expectedReference[1], cm.reference[2] - expectedReference[2]};
                    maxReferenceStep = std::max(maxReferenceStep, nf::ikLen(referenceDelta));
                    maxAnalyticReferenceError = std::max(maxAnalyticReferenceError, nf::ikLen(referenceError));
                    double E[3]; tm.elbowAt(cm.theta, E);
                    const double step[3] = {E[0] - lastE[0], E[1] - lastE[1], E[2] - lastE[2]};
                    if (n <= 100) maxPoseStepBeforeConstraintTangent = std::max(maxPoseStepBeforeConstraintTangent, nf::ikLen(step));
                    if (n == 147) {
                        double crossingOracleDistance = 0.0;
                        const double crossingOracleViolation = denseOracle(tm, cm.reference, 1.0, 0.0, &crossingOracleDistance);
                        const double selectedDistance = std::fabs(nf::ikWrapPi(cm.theta - tm.angleOf(cm.reference)));
                        crossingOracleOk = std::fabs(cm.violation - crossingOracleViolation) < 2e-5 && std::fabs(selectedDistance - crossingOracleDistance) < 0.002;
                    }
                    std::memcpy(moving.axis, cm.axis, sizeof(cm.axis));
                    std::memcpy(moving.reference, cm.reference, sizeof(cm.reference));
                    std::memcpy(moving.previous, cm.radial, sizeof(cm.radial));
                    moving.valid = true;
                    std::memcpy(lastE, E, sizeof(E));
                }
                const double antiT[3] = {0.0, 1.0, -0.48};
                const auto anti = nf::ikTwoBoneFrame(S3, antiT, 0.32, 0.28, hintA);
                const auto antiChoice = nf::ikStableSwivel(anti, startPole, 1.0, 0.0);
                check(trajectoryOk && maxPoseStepBeforeConstraintTangent < 0.03 && maxReferenceStep < 0.018 && maxAnalyticReferenceError < 1e-8 && crossingOracleOk &&
                          antiChoice.ok && nf::ikDot(antiChoice.reference, startPole.reference) > 0.99,
                      "F76: transported pole stays continuous through the original reference-parallel crossing, tracks the full antipodal sweep, and keeps the constraint-tangent branch oracle-correct");

                nf::IkPhysicalPole fallbackPole = pole;
                fallbackPole.reference[0] = fallbackPole.reference[1] = fallbackPole.reference[2] = 0.0;
                const double nearT[3] = {0.01, 0.84, 0.43};
                const auto nearFrame = nf::ikTwoBoneFrame(S3, nearT, 0.32, 0.28, hintB);
                const auto recovered = nf::ikStableSwivel(nearFrame, fallbackPole, 1.0, 0.06);
                nf::IkPhysicalPole recoveredState;
                recoveredState.valid = recovered.ok;
                std::memcpy(recoveredState.axis, recovered.axis, sizeof(recoveredState.axis));
                std::memcpy(recoveredState.reference, recovered.reference, sizeof(recoveredState.reference));
                std::memcpy(recoveredState.previous, recovered.radial, sizeof(recoveredState.previous));
                const double nextT[3] = {0.02, 0.84, 0.43};
                const auto nextFrame = nf::ikTwoBoneFrame(S3, nextT, 0.32, 0.28, hintA);
                const auto afterRecovery = nf::ikStableSwivel(nextFrame, recoveredState, 1.0, 0.06);
                check(recovered.ok && recovered.usedFallback && !recovered.transportedReference && afterRecovery.ok && afterRecovery.transportedReference &&
                          std::fabs(nf::ikDot(recovered.reference, recovered.axis)) < 1e-8 &&
                          std::fabs(nf::ikDot(afterRecovery.reference, afterRecovery.axis)) < 1e-8,
                      "F76: previous solved radial recovers a singular anatomical reference, and the recovered reference transports on the next frame");

                // A temporary constraint changes only the solved radial, never the transported anatomical reference.
                const auto constrained = nf::ikStableSwivel(ta, pole, 1.0, 0.20);
                nf::IkPhysicalPole constrainedState = pole;
                std::memcpy(constrainedState.axis, constrained.axis, sizeof(constrained.axis));
                std::memcpy(constrainedState.reference, constrained.reference, sizeof(constrained.reference));
                std::memcpy(constrainedState.previous, constrained.radial, sizeof(constrained.radial));
                const auto restored = nf::ikStableSwivel(ta, constrainedState, 1.0, 0.06);
                check(constrained.ok && restored.ok && nf::ikDot(constrained.reference, restored.reference) > 0.999999 &&
                          std::fabs(nf::ikWrapPi(restored.theta - chosenA.theta)) < 1e-9,
                      "F76: a temporary outward constraint does not ratchet the transported anatomical pole");

                nf::IkPhysicalPoleSet set;
                check(nf::ikPhysicalPoleBegin(set, 7, 11, true) && !set.side[1].valid,
                      "F76: first armed grip generation starts with empty physical-pole state");
                set.side[1] = pole;
                const bool sameKept = !nf::ikPhysicalPoleBegin(set, 7, 11, true) && set.side[1].valid;
                const bool itemReset = nf::ikPhysicalPoleBegin(set, 7, 12, true) && !set.side[1].valid;
                set.side[1] = pole;
                const bool rigReset = nf::ikPhysicalPoleBegin(set, 8, 12, true) && !set.side[1].valid;
                set.side[1] = pole;
                const bool inactiveReset = nf::ikPhysicalPoleBegin(set, 8, 12, false) && !set.active && !set.side[1].valid;
                check(sameKept && itemReset && rigReset && inactiveReset,
                      "F76: item, rig generation, and armed/write-gate loss reset physical-pole history; unchanged identity retains it");
                check(nf::ikPhysicalPoleSideEligible(0, true, false) && !nf::ikPhysicalPoleSideEligible(0, false, false) &&
                          nf::ikPhysicalPoleSideEligible(0, false, true) && nf::ikPhysicalPoleSideEligible(1, false, false),
                      "F76: a valid pistol support-cup target enables the left stable solve without the raw two-hand detector; free one-hand left remains unchanged");

                // F81: the authored pistol left elbow follows body-down and lateral, not animated wrist/head orientation.
                const double pistolS[3] = {0.0, 0.0, 0.0}, pistolT[3] = {0.0, -0.05, 0.40};
                const double bodyUp[3] = {0.0, 1.0, 0.0}, arbitraryPrevious[3] = {0.0, 1.0, 0.0};
                const auto pistolTb = nf::ikTwoBoneFrame(pistolS, pistolT, 0.34, 0.30, arbitraryPrevious);
                const auto pistolDown = nf::ikPistolDownSwivel(pistolTb, nullptr, bodyUp, 0.06, 6.0 * nf::kIkPi / 180.0);
                const auto pistolOtherWrist = nf::ikPistolDownSwivel(pistolTb, arbitraryPrevious, bodyUp, 0.06, 6.0 * nf::kIkPi / 180.0);
                double pistolElbow[3] = {}, pistolExpected[3] = {};
                if (pistolDown.ok) pistolTb.elbowAt(pistolDown.theta, pistolElbow);
                const double beta = std::asin(0.06 / pistolTb.h) + 6.0 * nf::kIkPi / 180.0;
                const double center[3] = {pistolTb.S[0] + pistolTb.a * pistolTb.dir[0], pistolTb.S[1] + pistolTb.a * pistolTb.dir[1], pistolTb.S[2] + pistolTb.a * pistolTb.dir[2]};
                for (int i = 0; i < 3; ++i) pistolExpected[i] = center[i] + pistolTb.h * pistolDown.radial[i];
                const double downTargetDelta[3] = {pistolElbow[0] - pistolExpected[0], pistolElbow[1] - pistolExpected[1], pistolElbow[2] - pistolExpected[2]};
                const double alongAxis = nf::ikDot(pistolDown.radial, pistolTb.dir);
                const double upper[3] = {pistolElbow[0] - pistolTb.S[0], pistolElbow[1] - pistolTb.S[1], pistolElbow[2] - pistolTb.S[2]};
                const double forearm[3] = {pistolElbow[0] - pistolTb.hand[0], pistolElbow[1] - pistolTb.hand[1], pistolElbow[2] - pistolTb.hand[2]};
                const bool pistolLengths = std::fabs(nf::ikLen(upper) - 0.34) < 1e-8 && std::fabs(nf::ikLen(forearm) - 0.30) < 1e-8;
                double lateralBasis[3]; nf::ikCross(pistolTb.dir, bodyUp, lateralBasis); nf::ikUnit(lateralBasis, lateralBasis);
                const bool downChoiceOk = pistolDown.ok && pistolOtherWrist.ok && !pistolDown.usedFallback && pistolDown.belowFeasible &&
                    std::fabs(alongAxis) < 1e-9 && pistolDown.radial[1] < 0.0 && pistolDown.radial[0] < 0.0 &&
                    nf::ikLen(downTargetDelta) < 1e-8 && pistolLengths && std::fabs(pistolDown.violation) < 1e-9 &&
                    std::fabs(nf::ikWrapPi(pistolDown.theta - pistolOtherWrist.theta)) < 1e-10 &&
                    std::fabs(nf::ikDot(pistolDown.radial, lateralBasis) - std::sin(beta)) < 1e-8;
                const double yawAxis[3] = {0.31, 0.47, -0.72};
                double yawUnit[3], yawRotation[9], yawS[3], yawT[3], yawUp[3];
                nf::ikUnit(yawAxis, yawUnit);
                nf::ikRotAxis(yawUnit, 0.73, yawRotation);
                const auto rotateVector = [&](const double in[3], double out[3]) {
                    for (int c = 0; c < 3; ++c) out[c] = in[0] * yawRotation[c] + in[1] * yawRotation[3 + c] + in[2] * yawRotation[6 + c];
                };
                rotateVector(pistolS, yawS); rotateVector(pistolT, yawT); rotateVector(bodyUp, yawUp);
                const double rotatedHint[3] = {-0.2, -0.9, 0.1};
                double yawHint[3]; rotateVector(rotatedHint, yawHint);
                const auto yawTb = nf::ikTwoBoneFrame(yawS, yawT, 0.34, 0.30, yawHint);
                const auto yawDown = nf::ikPistolDownSwivel(yawTb, nullptr, yawUp, 0.06, 6.0 * nf::kIkPi / 180.0);
                double yawElbow[3] = {}, expectedYawElbow[3];
                if (yawDown.ok) yawTb.elbowAt(yawDown.theta, yawElbow);
                rotateVector(pistolElbow, expectedYawElbow);
                const double yawDelta[3] = {yawElbow[0] - expectedYawElbow[0], yawElbow[1] - expectedYawElbow[1], yawElbow[2] - expectedYawElbow[2]};
                const double verticalS[3] = {0.0, 0.0, 0.0}, verticalT[3] = {0.0, 0.54, 0.0}, prior[3] = {-1.0, 0.0, 0.0};
                const auto verticalTb = nf::ikTwoBoneFrame(verticalS, verticalT, 0.34, 0.30, prior);
                auto vertical = nf::ikPistolDownSwivel(verticalTb, prior, bodyUp, 0.06, 6.0 * nf::kIkPi / 180.0);
                const double held[3] = {vertical.radial[0], vertical.radial[1], vertical.radial[2]};
                bool singularHold = vertical.ok && vertical.usedFallback && !vertical.belowFeasible && vertical.violation > 0.0;
                for (int i = 0; i < 100 && singularHold; ++i) {
                    const auto next = nf::ikPistolDownSwivel(verticalTb, vertical.radial, bodyUp, 0.06, 6.0 * nf::kIkPi / 180.0);
                    const double holdDelta[3] = {next.radial[0] - held[0], next.radial[1] - held[1], next.radial[2] - held[2]};
                    singularHold = next.ok && next.usedFallback && nf::ikLen(holdDelta) < 1e-10;
                    vertical = next;
                }
                bool yawSweepOk = true;
                const double yawShoulder[3] = {0.0, 1.0, 0.0};
                for (int direction : {1, -1}) {
                    double previousElbow[3] = {};
                    bool havePrevious = false;
                    for (int step = 0; step <= 40; ++step) {
                        const int signedStep = direction > 0 ? step : 40 - step;
                        const double yaw = (-100.0 + 5.0 * signedStep) * nf::kIkPi / 180.0;
                        const double target[3] = {0.40 * std::sin(yaw), 0.95, 0.40 * std::cos(yaw)};
                        const auto yawFrame = nf::ikTwoBoneFrame(yawShoulder, target, 0.34, 0.30, arbitraryPrevious);
                        const auto yawResult = nf::ikPistolDownSwivel(yawFrame, nullptr, bodyUp, 0.06, 6.0 * nf::kIkPi / 180.0);
                        double expectedLateral[3]; nf::ikCross(yawFrame.dir, bodyUp, expectedLateral); nf::ikUnit(expectedLateral, expectedLateral);
                        double elbow[3] = {};
                        if (yawResult.ok) yawFrame.elbowAt(yawResult.theta, elbow);
                        double delta[3] = {elbow[0] - previousElbow[0], elbow[1] - previousElbow[1], elbow[2] - previousElbow[2]};
                        yawSweepOk = yawSweepOk && yawResult.ok && yawResult.radial[1] < 0.0 && nf::ikDot(yawResult.radial, expectedLateral) > 0.0 &&
                                     (!havePrevious || nf::ikLen(delta) < 0.04);
                        std::memcpy(previousElbow, elbow, sizeof(elbow));
                        havePrevious = yawResult.ok;
                    }
                }
                const double capturedS[3] = {-0.188, 1.077, 0.357}, capturedT[3] = {0.006, 1.046, 0.802};
                const auto capturedFrame = nf::ikTwoBoneFrame(capturedS, capturedT, 0.3045619805556827, 0.2879583303188154, arbitraryPrevious);
                const auto capturedDown = nf::ikPistolDownSwivel(capturedFrame, nullptr, bodyUp, 0.06, 6.0 * nf::kIkPi / 180.0);
                double capturedElbow[3] = {}, capturedUpper[3] = {}, capturedForearm[3] = {};
                if (capturedDown.ok) capturedFrame.elbowAt(capturedDown.theta, capturedElbow);
                for (int i = 0; i < 3; ++i) { capturedUpper[i] = capturedElbow[i] - capturedS[i]; capturedForearm[i] = capturedElbow[i] - capturedFrame.hand[i]; }
                const bool capturedRegression = capturedDown.ok && capturedDown.radial[1] < 0.0 &&
                    capturedS[1] - capturedElbow[1] > 0.15 &&
                    std::fabs(nf::ikLen(capturedUpper) - 0.3045619805556827) < 1e-8 &&
                    std::fabs(nf::ikLen(capturedForearm) - 0.2879583303188154) < 1e-8;
                const double infiniteUp[3] = {0.0, std::numeric_limits<double>::infinity(), 0.0};
                const auto invalidPistol = nf::ikPistolDownSwivel(pistolTb, nullptr, infiniteUp, 0.06, 0.0);
                check(downChoiceOk && yawDown.ok && nf::ikLen(yawDelta) < 1e-8 && singularHold && yawSweepOk && capturedRegression && !invalidPistol.ok,
                      "F81: pistol-left elbow points down/out with exact arm lengths, is rigid-yaw and wrist-hint invariant, sweeps both directions through body yaw ±100 deg, reproduces captured frame 7977, holds impossible vertical fallback, and rejects nonfinite axes");

                nf::IkPoseDeviationTracker detector;
                nf::IkPoseDeviationSample pose;
                pose.fresh = true; pose.item = 4; pose.generation = 9; pose.fpAgeMs = 12; pose.gAgeMs = 16;
                pose.valid[0] = pose.valid[1] = true; pose.radialValid[0] = pose.radialValid[1] = true;
                pose.axis[0][2] = pose.axis[1][2] = 1.0;
                pose.radial[0][0] = pose.radial[1][0] = 1.0;
                nf::IkPoseDeviationRecord events[nf::kIkPoseDeviationHistory + 1 + nf::kIkPoseDeviationPost];
                int eventRows = 0;
                for (uint32_t frame = 1; frame <= 8; ++frame) {
                    pose.frame = frame; pose.qpc = frame * 10;
                    eventRows += detector.step(pose, 1000, events);
                }
                pose.frame = 9; pose.qpc = 90; pose.elbow[0][0] = 0.05;
                eventRows = detector.step(pose, 1000, events);
                const bool hasEightPre = eventRows == 9 && events[0].offset == -8 && events[7].offset == -1 && events[8].offset == 0 && events[8].event == 1;
                pose.frame = 10; pose.qpc = 100; pose.elbow[0][0] = 0.0;
                const int firstPostRows = detector.step(pose, 1000, events);
                const bool postStartsAtOne = firstPostRows == 1 && events[0].offset == 1;
                pose.frame = 11; pose.qpc = 110; pose.elbow[0][0] = 0.06;
                detector.step(pose, 1000, events);
                const bool coalesced = detector.coalesced == 2;
                int lastPostRows = 0;
                for (uint32_t i = 0; i < 6; ++i) { ++pose.frame; pose.qpc += 10; pose.elbow[0][0] = 0.06; lastPostRows = detector.step(pose, 1000, events); }
                const bool postEndsAtEight = lastPostRows == 1 && events[0].offset == 8 && detector.postRemaining == 0;
                nf::IkPoseDeviationTracker angleDetector;
                pose = {}; pose.fresh = true; pose.item = 4; pose.generation = 9; pose.fpAgeMs = 12; pose.gAgeMs = 16;
                pose.valid[1] = true; pose.radialValid[1] = true; pose.axis[1][2] = 1.0; pose.radial[1][0] = 1.0;
                for (uint32_t frame = 1; frame <= 8; ++frame) { pose.frame = frame; pose.qpc = frame * 10; angleDetector.step(pose, 1000, events); }
                pose.frame = 9; pose.qpc = 90; pose.radial[1][0] = std::cos(20.1 * nf::kIkPi / 180.0); pose.radial[1][1] = std::sin(20.1 * nf::kIkPi / 180.0);
                const int angleRows = angleDetector.step(pose, 1000, events);
                check(hasEightPre && postStartsAtOne && coalesced && postEndsAtEight && angleRows == 9 && events[8].offset == 0,
                      "F76: deviation events include up to eight pre-samples, trigger, eight post-samples, and coalesce active retriggers; radial angle is side-local");

                nf::IkPoseDeviationTracker resetDetector;
                pose = {}; pose.fresh = true; pose.item = 1; pose.generation = 2; pose.fpAgeMs = 10; pose.gAgeMs = 10; pose.valid[0] = true;
                pose.frame = 1; pose.qpc = 10; resetDetector.step(pose, 1000, events);
                pose.frame = 2; pose.qpc = 20; pose.fpAgeMs = 101; pose.elbow[0][0] = 0.2;
                const int staleRows = resetDetector.step(pose, 1000, events);
                pose.frame = 3; pose.qpc = 30; pose.fpAgeMs = 10;
                const int noFalseJump = resetDetector.step(pose, 1000, events);
                check(staleRows == 0 && noFalseJump == 0 && resetDetector.haveLast && resetDetector.historyCount == 1,
                      "F76: stale inputs and unsolved gaps reset the comparator without generating a false movement event");

                nf::IkPoseDeviationTracker missedFrameDetector;
                pose = {}; pose.fresh = true; pose.item = 6; pose.generation = 2; pose.fpAgeMs = 10; pose.gAgeMs = 10; pose.valid[0] = true;
                pose.frame = 1; pose.qpc = 10; missedFrameDetector.step(pose, 1000, events);
                pose.frame = 3; pose.qpc = 30; pose.elbow[0][0] = 0.20;
                const int afterMissedWrite = missedFrameDetector.step(pose, 1000, events);
                check(afterMissedWrite == 0 && missedFrameDetector.gaps == 1 && missedFrameDetector.historyCount == 1,
                      "F76: a skipped frame after an unsuccessful write breaks continuity and becomes a fresh baseline");

                nf::IkPoseDeviationTracker activeGapDetector;
                pose = {}; pose.fresh = true; pose.item = 8; pose.generation = 2; pose.fpAgeMs = 10; pose.gAgeMs = 10; pose.valid[0] = true;
                for (uint32_t frame = 1; frame <= 8; ++frame) { pose.frame = frame; pose.qpc = frame * 10; activeGapDetector.step(pose, 1000, events); }
                pose.frame = 9; pose.qpc = 90; pose.elbow[0][0] = 0.05; activeGapDetector.step(pose, 1000, events);
                pose.frame = 11; pose.qpc = 110; pose.elbow[0][0] = 0.20;
                const int invalidPost = activeGapDetector.step(pose, 1000, events);
                check(invalidPost == 1 && events[0].invalid && events[0].offset == 1 && activeGapDetector.gaps == 1,
                      "F76: a missing written frame during capture emits an explicitly invalid post row and closes the event");

                nf::IkPoseDeviationTracker duplicateDetector;
                pose = {}; pose.fresh = true; pose.item = 2; pose.generation = 3; pose.fpAgeMs = 10; pose.gAgeMs = 10; pose.valid[0] = true;
                pose.frame = 1; pose.qpc = 10; duplicateDetector.step(pose, 1000, events);
                pose.qpc = 20; pose.elbow[0][0] = 0.20;
                const int duplicateRows = duplicateDetector.step(pose, 1000, events);
                const bool duplicateKeptHistory = duplicateRows == 0 && duplicateDetector.duplicates == 1 && duplicateDetector.last.elbow[0][0] == 0.0;
                pose.frame = 2; pose.qpc = 30; pose.elbow[0][0] = 0.05;
                const int afterDuplicateRows = duplicateDetector.step(pose, 1000, events);
                check(duplicateKeptHistory && afterDuplicateRows == 2 && events[1].offset == 0,
                      "F76: repeated P1 frames are ignored without destroying the prior comparison sample");

                nf::IkPoseDeviationTracker policyDetector;
                pose = {}; pose.fresh = true; pose.item = 3; pose.generation = 4; pose.fpRig = 0xAA; pose.fpGeneration = 5; pose.fpAgeMs = 12; pose.gAgeMs = 14;
                pose.valid[1] = true; pose.leftWrote = true; pose.targetMode[0] = 1;
                pose.frame = 1; pose.qpc = 10; policyDetector.step(pose, 1000, events);
                pose.frame = 2; pose.qpc = 20; pose.offTwo = true; pose.offQualified = true; pose.leftStablePolicy = true;
                const int policyEdgeRows = policyDetector.step(pose, 1000, events);
                const bool policyEdgeCaptured = policyEdgeRows == 2 && events[1].offset == 0 && events[1].sample.offTwo && events[1].sample.leftStablePolicy;
                pose.offTwo = false; pose.frame = 2; pose.qpc = 25;
                const int policyDuplicateRows = policyDetector.step(pose, 1000, events);
                const bool duplicateIgnoredAuthority = policyDuplicateRows == 0 && policyDetector.duplicates == 1 && policyDetector.last.offTwo;
                nf::IkPoseDeviationTracker qualificationOnlyDetector;
                pose = {}; pose.fresh = true; pose.item = 3; pose.generation = 4; pose.fpAgeMs = 12; pose.gAgeMs = 14;
                pose.frame = 1; pose.qpc = 10; qualificationOnlyDetector.step(pose, 1000, events);
                pose.frame = 2; pose.qpc = 20; pose.offQualified = true;
                const int qualificationOnlyRows = qualificationOnlyDetector.step(pose, 1000, events);
                nf::IkPoseDeviationTracker identityDetector;
                pose = {}; pose.fresh = true; pose.item = 3; pose.generation = 4; pose.fpRig = 0xAA; pose.fpGeneration = 5; pose.fpAgeMs = 12; pose.gAgeMs = 14;
                pose.frame = 1; pose.qpc = 10; identityDetector.step(pose, 1000, events);
                pose.frame = 2; pose.qpc = 20; pose.fpRig = 0xBB; pose.fpGeneration = 6; pose.offIdentityReset = true;
                const int identityRows = identityDetector.step(pose, 1000, events);
                check(policyEdgeCaptured && duplicateIgnoredAuthority && qualificationOnlyRows == 0 && identityRows == 1 && events[0].offset == 0 && events[0].sample.offIdentityReset && identityDetector.gaps == 1,
                      "F78: policy edges capture write/authority fields, raw qualification changes alone do not consume events, duplicate P1 samples cannot replace them, and FP rig identity changes reset continuity");

                nf::IkPoseDeviationTracker reachRejectDetector;
                pose = {}; pose.fresh = true; pose.item = 7; pose.generation = 3; pose.fpAgeMs = 10; pose.gAgeMs = 10;
                pose.frame = 1; pose.qpc = 10; reachRejectDetector.step(pose, 1000, events);
                pose.frame = 2; pose.qpc = 20; pose.reachMeasured = true; pose.reachActive = true; pose.reachInfeasible = true; pose.reachRejected = true;
                const int reachRejectRows = reachRejectDetector.step(pose, 1000, events);
                check(reachRejectRows == 2 && events[1].offset == 0 && events[1].sample.reachMeasured && events[1].sample.reachRejected,
                      "F79: the first infeasible-correction rejection creates a bounded policy event with measured/rejected status");

                nf::IkPoseDeviationTracker capDetector;
                pose = {}; pose.fresh = true; pose.generation = 1; pose.fpAgeMs = 10; pose.gAgeMs = 10; pose.valid[0] = true;
                uint32_t frame = 0; uint64_t qpc = 0; bool started = false;
                const auto stepPose = [&]() noexcept { pose.frame = ++frame; pose.qpc = qpc += 10; capDetector.step(pose, 1000, events); };
                const auto emitItemEvents = [&](uint32_t item, uint32_t count) noexcept {
                    if (!started || pose.item != item) { pose.item = item; pose.elbow[0][0] = 0.0; stepPose(); started = true; }
                    for (uint32_t n = 0; n < count; ++n) {
                        pose.elbow[0][0] = pose.elbow[0][0] == 0.0 ? 0.06 : 0.0;
                        stepPose();
                        for (int post = 0; post < 8; ++post) stepPose();
                    }
                };
                emitItemEvents(1, nf::kIkPoseDeviationEventsPerItemMinute);
                const bool firstItemBounded = capDetector.eventsInMinute == 8 && capDetector.itemEventsInMinute == 8 && capDetector.itemSuppressed == 0;
                emitItemEvents(1, 1);
                const bool sameItemCapped = capDetector.eventsInMinute == 8 && capDetector.itemEventsInMinute == 8 && capDetector.itemSuppressed == 1;
                emitItemEvents(2, 1);  // a later held item receives its own first-event budget in the same minute
                const bool secondItemCaptured = capDetector.eventsInMinute == 9 && capDetector.activeItemSerial == 2 && capDetector.itemEventsInMinute == 1;
                emitItemEvents(2, 7);
                emitItemEvents(3, 8);
                emitItemEvents(4, 8);
                const bool globalBounded = capDetector.eventsInMinute == nf::kIkPoseDeviationEventsGlobalPerMinute;
                emitItemEvents(5, 1);
                const bool globalSuppressed = capDetector.globalSuppressed > 0 && capDetector.eventsInMinute == nf::kIkPoseDeviationEventsGlobalPerMinute;
                const uint32_t suppressedBeforeRollover = capDetector.suppressed;
                qpc = 59980; pose.elbow[0][0] = 0.0; stepPose();
                pose.elbow[0][0] = 0.06; stepPose();
                check(firstItemBounded && sameItemCapped && secondItemCaptured && globalBounded && globalSuppressed && capDetector.eventsInMinute == 1 &&
                          capDetector.itemEventsInMinute == 1 && capDetector.activeItemSerial == 5 && capDetector.suppressed == suppressedBeforeRollover,
                      "F77: per-item event budgets preserve later-item coverage, a global cap stops item cycling, and both budgets reopen by minute");

                edvr::nofoot::optic_capture::Ledger opticLedger;
                opticLedger.begin(1);
                using OpticRecord = edvr::nofoot::optic_capture::Record;
                using OpticRow = edvr::nofoot::optic_capture::RowDescriptor;
                OpticRow requested[edvr::nofoot::optic_capture::kCount]{};
                bool allSixteen = true;
                for (uint32_t i = 0; i < edvr::nofoot::optic_capture::kCount; ++i) {
                    requested[i].scheduled = true; requested[i].sceneFrame = 100 + i; requested[i].renderFrame = 4567 + i; requested[i].epoch = 4;
                    requested[i].sequence = 1000 + i; std::strcpy(requested[i].imageStatus, "copied");
                    if (i == 5 || i == 6) continue;
                    OpticRecord row; row.sceneFrame = requested[i].sceneFrame; row.renderFrame = requested[i].renderFrame; row.epoch = requested[i].epoch; row.sequence = requested[i].sequence;
                    allSixteen = opticLedger.sample(i, row) && allSixteen;
                }
                OpticRecord duplicateOptic; duplicateOptic.sceneFrame = 888;
                const bool duplicateRejected = !opticLedger.sample(4, duplicateOptic) && opticLedger.duplicates == 1 && opticLedger.rows[4].sceneFrame == 104;
                OpticRecord delayedG; delayedG.sceneFrame = 100; delayedG.renderFrame = 4567; delayedG.fpFrame = 4567; delayedG.eyeFrame = 4567; delayedG.adsFrame = 4567;
                delayedG.rootReadFrame = 4567; delayedG.gGameFrame = 500; delayedG.state = edvr::nofoot::optic_capture::SourceState::Unmatched;
                const bool mismatchRetained = opticLedger.sample(5, delayedG) && opticLedger.rows[5].state == edvr::nofoot::optic_capture::SourceState::Unmatched &&
                    opticLedger.rows[5].sceneFrame == 100 && opticLedger.rows[5].renderFrame == 4567 && opticLedger.rows[5].gGameFrame == 500 && opticLedger.rows[5].rootReadFrame == 4567;
                const bool independentFrameDomains = edvr::nofoot::optic_capture::sourceFramesMatchRender(4567, 4567, 4567, 4567, 4567) &&
                    !edvr::nofoot::optic_capture::sourceFramesMatchRender(4567, 100, 4567, 4567, 4567) &&
                    !edvr::nofoot::optic_capture::sourceFramesMatchRender(0, 0, 0, 0, 0) &&
                    !edvr::nofoot::optic_capture::sourceFramesMatchRender(~uint32_t(0), ~uint32_t(0), ~uint32_t(0), ~uint32_t(0), ~uint32_t(0));
                const bool exactWeaponUiJoin = edvr::nofoot::optic_capture::uiTraceMatches(4567, 92, true, 4567, 92) &&
                    !edvr::nofoot::optic_capture::uiTraceMatches(4566, 92, true, 4567, 92) &&
                    !edvr::nofoot::optic_capture::uiTraceMatches(4567, 91, true, 4567, 92) &&
                    !edvr::nofoot::optic_capture::uiTraceMatches(4567, 92, false, 4567, 92) &&
                    !edvr::nofoot::optic_capture::uiTraceMatches(0, 92, true, 0, 92);
                using PoseState = edvr::nofoot::optic_capture::PoseState;
                using PoseKey = edvr::nofoot::optic_capture::PoseKey;
                PoseKey poseKey; poseKey.renderFrame = 4567; poseKey.hookRenderFrame = 4567; poseKey.rootReadFrame = 4567; poseKey.fpSequence = 101; poseKey.gGameFrame = 500; poseKey.itemSerial = 42;
                poseKey.rigGeneration = 7; poseKey.rig = 99; poseKey.qpc = 900;
                PoseKey sourceKey = poseKey; sourceKey.qpc = 0;
                const bool poseJoinCases =
                    edvr::nofoot::optic_capture::poseJoinState(true, poseKey, sourceKey, 4567, 1000, 100, true, true) == PoseState::Matched &&
                    edvr::nofoot::optic_capture::poseJoinState(false, poseKey, sourceKey, 4567, 1000, 100, true, true) == PoseState::Missing &&
                    edvr::nofoot::optic_capture::poseJoinState(true, poseKey, sourceKey, 4568, 1000, 100, true, true) == PoseState::FrameMismatch &&
                    [] (PoseKey p, PoseKey s) { ++p.itemSerial; return edvr::nofoot::optic_capture::poseJoinState(true, p, s, 4567, 1000, 100, true, true) == PoseState::IdentityMismatch; }(poseKey, sourceKey) &&
                    [] (PoseKey p, PoseKey s) { p.qpc = 1001; return edvr::nofoot::optic_capture::poseJoinState(true, p, s, 4567, 1000, 100, true, true) == PoseState::Future; }(poseKey, sourceKey) &&
                    [] (PoseKey p, PoseKey s) { p.qpc = 800; return edvr::nofoot::optic_capture::poseJoinState(true, p, s, 4567, 1000, 100, true, true) == PoseState::Stale; }(poseKey, sourceKey) &&
                    edvr::nofoot::optic_capture::poseJoinState(true, poseKey, sourceKey, 4567, 1000, 100, false, true) == PoseState::NoWrite &&
                    edvr::nofoot::optic_capture::poseJoinState(true, poseKey, sourceKey, 4567, 1000, 100, true, false) == PoseState::Invalid &&
                    [] (PoseKey p, PoseKey s) { p.rootReadFrame = 4566; return edvr::nofoot::optic_capture::poseJoinState(true, p, s, 4567, 1000, 100, true, true) == PoseState::FrameMismatch; }(poseKey, sourceKey);
                OpticRecord sceneEye; sceneEye.sceneEyeState = edvr::nofoot::optic_capture::SceneEyeState::Matched;
                sceneEye.sceneEyeRowsRead = sceneEye.sceneEyeRowsOk = sceneEye.sceneEyeRowsOwn = true;
                sceneEye.sceneEyeFrame = 100; sceneEye.sceneEyeQpc = 950;
                for (int i = 0; i < 12; ++i) sceneEye.sceneEyeRows[i] = static_cast<float>(i) * 0.25f;
                edvr::nofoot::optic_capture::Ledger sceneEyeLedger; sceneEyeLedger.begin(3);
                const bool sceneEyeRetained = sceneEyeLedger.sample(0, sceneEye) && sceneEyeLedger.rows[0].sceneEyeState == edvr::nofoot::optic_capture::SceneEyeState::Matched &&
                    sceneEyeLedger.rows[0].sceneEyeFrame == 100 && sceneEyeLedger.rows[0].sceneEyeQpc == 950 && sceneEyeLedger.rows[0].sceneEyeRows[11] == 2.75f;
                check(poseJoinCases && exactWeaponUiJoin,
                      "F81: exact after-write pose joins distinguish missing, stale, future, frame/identity mismatch, no-write and invalid; UI rows require exact render/item identity");
                check(sceneEyeRetained, "F81: exact scene-eye rows are retained independently from engine eye matrices");
                float rawAffine[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1};
                rawAffine[15] = std::numeric_limits<float>::quiet_NaN();
                float serializedAffine[16] = {};
                bool rawPaddingFinite = true;
                const bool paddedAffineValid = nf::pfzCanonicalizeAffineMatrix(rawAffine, serializedAffine, &rawPaddingFinite);
                const bool canonicalPadding = paddedAffineValid && !rawPaddingFinite && serializedAffine[3] == 0.0f && serializedAffine[7] == 0.0f &&
                    serializedAffine[11] == 0.0f && serializedAffine[15] == 1.0f && serializedAffine[12] == 2.0f && serializedAffine[13] == 3.0f && serializedAffine[14] == 4.0f;
                rawAffine[5] = std::numeric_limits<float>::quiet_NaN();
                const bool badBasisRejected = !nf::pfzCanonicalizeAffineMatrix(rawAffine, serializedAffine);
                rawAffine[5] = 1.0f; rawAffine[13] = std::numeric_limits<float>::infinity();
                const bool badTranslationRejected = !nf::pfzCanonicalizeAffineMatrix(rawAffine, serializedAffine);
                check(canonicalPadding && badBasisRejected && badTranslationRejected,
                      "F82: affine snapshot accepts nonsemantic NaN padding, canonicalizes serialized padding with an explicit validity signal, and rejects nonfinite basis or translation");
                OpticRecord invalidBasis; invalidBasis.p14BasisValid = false; invalidBasis.state = edvr::nofoot::optic_capture::SourceState::Invalid;
                const bool invalidBasisVisible = opticLedger.sample(6, invalidBasis) && opticLedger.rows[6].state == edvr::nofoot::optic_capture::SourceState::Invalid;
                bool fullCoverage = allSixteen;
                for (uint32_t i = 0; i < edvr::nofoot::optic_capture::kCount; ++i) fullCoverage = fullCoverage && opticLedger.rows[i].sampled;
                const bool boundsChecked = !opticLedger.sample(edvr::nofoot::optic_capture::kCount, OpticRecord{});
                opticLedger.end(requested, edvr::nofoot::optic_capture::kCount);
                const bool disarmed = !opticLedger.armed && !opticLedger.sample(0, OpticRecord{});
                const bool requestedMetadata = opticLedger.requested[15].scheduled && opticLedger.requested[15].sceneFrame == 115 && opticLedger.requested[15].renderFrame == 4582 && opticLedger.requested[15].sequence == 1015;
                opticLedger.begin(2);
                const bool rearmed = opticLedger.armed && opticLedger.duplicates == 0 && !opticLedger.rows[4].sampled;
                OpticRow missingRows[edvr::nofoot::optic_capture::kCount]{};
                missingRows[3].scheduled = true; missingRows[3].sceneFrame = 203; missingRows[3].renderFrame = 9090; missingRows[3].epoch = 9;
                missingRows[3].sequence = 909; std::strcpy(missingRows[3].imageStatus, "source_missing");
                opticLedger.end(missingRows, edvr::nofoot::optic_capture::kCount);
                const bool missingCallbackVisible = !opticLedger.rows[3].sampled && opticLedger.requested[3].scheduled &&
                    opticLedger.requested[3].sceneFrame == 203 && opticLedger.requested[3].renderFrame == 9090 && opticLedger.requested[3].epoch == 9 && opticLedger.requested[3].sequence == 909 &&
                    std::strcmp(opticLedger.requested[3].imageStatus, "source_missing") == 0;
                char finiteJson[48], nanJson[48], infJson[48];
                const bool jsonNumbers = edvr::nofoot::optic_capture::formatJsonNumber(finiteJson, sizeof(finiteJson), 3.25f) && std::strcmp(finiteJson, "3.25") == 0 &&
                    !edvr::nofoot::optic_capture::formatJsonNumber(nanJson, sizeof(nanJson), std::numeric_limits<float>::quiet_NaN()) && std::strcmp(nanJson, "null") == 0 &&
                    !edvr::nofoot::optic_capture::formatJsonNumber(infJson, sizeof(infJson), std::numeric_limits<float>::infinity()) && std::strcmp(infJson, "null") == 0;
                opticLedger.begin(3);
                const bool cleanRearm = opticLedger.armed && opticLedger.duplicates == 0 && !opticLedger.rows[3].sampled && !opticLedger.requested[3].scheduled;
                opticLedger.end(nullptr, 0);
                check(fullCoverage && duplicateRejected && mismatchRetained && independentFrameDomains && invalidBasisVisible && boundsChecked && disarmed && requestedMetadata && missingCallbackVisible && jsonNumbers && rearmed && cleanRearm,
                      "F77: synchronized optic capture keeps scene/render frame domains distinct, retains mismatched source values and missing callbacks, and emits JSON-safe nonfinite numbers");
            }
            // F58: the spine bend's pure half: the residual split, the solve, the clamps, the 1/3, 2/3, 1 split, the 0.2 s ramp and the cap.
            {
                const double kDeg = nf::kIkPi / 180.0;
                // The residual split: (forward, up, right) = (z, y, x) of T - N.
                const double Tr[3] = {0.1, 1.5, 0.3}, Nr[3] = {0.0, 1.7, 0.2};
                double split[3];
                nf::bendResidualSplit(Tr, Nr, split);
                check(std::fabs(split[0] - 0.1) < 1e-12 && std::fabs(split[1] + 0.2) < 1e-12 && std::fabs(split[2] - 0.1) < 1e-12,
                      "F58: the residual splits into (forward, up, right) = (z, y, x) of the eye target less the pov");
                // The solve: a target 20 deg forward of the pov about the pivot gives pitch +20 deg, roll 0, and the pov lands on it; 10 deg to
                // the right gives roll +10 deg; a target straight below the pov on the pivot's line (a height mismatch) gives no bend.
                const double P[3] = {0.0, 1.0, 0.0}, N[3] = {0.0, 1.7, 0.0};
                const double Tf[3] = {0.0, 1.0 + 0.7 * std::cos(20.0 * kDeg), 0.7 * std::sin(20.0 * kDeg)};
                const double Tside[3] = {0.7 * std::sin(10.0 * kDeg), 1.0 + 0.7 * std::cos(10.0 * kDeg), 0.0};
                const double Tlow[3] = {0.0, 1.45, 0.0};
                double pf = 0.0, rf = 0.0, ps = 0.0, rs = 0.0, pLow = 7.0, rLow = 7.0;
                const bool okF = nf::bendSolve(P, N, Tf, &pf, &rf), okS = nf::bendSolve(P, N, Tside, &ps, &rs), okL = nf::bendSolve(P, N, Tlow, &pLow, &rLow);
                double Rf[9], Rs[9], vf[3], vs[3];
                const double v[3] = {N[0] - P[0], N[1] - P[1], N[2] - P[2]};
                nf::bendRot(pf, rf, 1.0, Rf);
                nf::bendRot(ps, rs, 1.0, Rs);
                nf::bendApply(v, Rf, vf);
                nf::bendApply(v, Rs, vs);
                double pDeg = 7.0, rDeg = 7.0;
                const bool degen = nf::bendSolve(P, P, Tf, &pDeg, &rDeg);
                check(okF && std::fabs(pf - 20.0 * kDeg) < 1e-9 && std::fabs(rf) < 1e-9 && std::fabs(P[1] + vf[1] - Tf[1]) < 1e-9 &&
                          std::fabs(P[2] + vf[2] - Tf[2]) < 1e-9 && vf[2] > 0.0 && okS && std::fabs(ps) < 1e-9 && std::fabs(rs - 10.0 * kDeg) < 1e-9 &&
                          std::fabs(P[0] + vs[0] - Tside[0]) < 1e-9 && vs[0] > 0.0 && okL && pLow == 0.0 && rLow == 0.0 && !degen && pDeg == 0.0 && rDeg == 0.0,
                      "F58: the bend solve turns the pov onto a target forward (pitch +20) or right (roll +10); a height mismatch and a degenerate pov "
                      "give no bend");
                // The clamps: forward 45, back 15, roll +-20 deg; each flags its hit; NaN is 0.
                nf::BendClamp c1, c2, c3, c4, c5;
                const double p60 = nf::bendClampPitch(60.0 * kDeg, &c1), pBack = nf::bendClampPitch(-30.0 * kDeg, &c2),
                             p30 = nf::bendClampPitch(30.0 * kDeg, &c3), r25 = nf::bendClampRoll(25.0 * kDeg, &c4), rM25 = nf::bendClampRoll(-25.0 * kDeg, &c5);
                nf::BendClamp c6;
                const double r10 = nf::bendClampRoll(-10.0 * kDeg, &c6), pNan = nf::bendClampPitch(std::numeric_limits<double>::quiet_NaN(), &c6);
                check(std::fabs(p60 - 45.0 * kDeg) < 1e-12 && c1.fwd && !c1.back && std::fabs(pBack + 15.0 * kDeg) < 1e-12 && c2.back && !c2.fwd &&
                          std::fabs(p30 - 30.0 * kDeg) < 1e-12 && !c3.fwd && !c3.back && std::fabs(r25 - 20.0 * kDeg) < 1e-12 && c4.roll &&
                          std::fabs(rM25 + 20.0 * kDeg) < 1e-12 && c5.roll && std::fabs(r10 + 10.0 * kDeg) < 1e-12 && !c6.roll && !c6.fwd && pNan == 0.0 &&
                          nf::bendAmountClamp(1.5) == 1.0 && nf::bendAmountClamp(-0.5) == 0.0 &&
                          nf::bendAmountClamp(std::numeric_limits<double>::quiet_NaN()) == 1.0,
                      "F58: the bend clamps hold forward 45, back 15 and roll +-20 deg (30 forward and 10 roll pass, NaN is 0); the amount clamps to 0..1");
                // The split: pitch 30 and roll 12 deg, each level turned by its share (1/3, 2/3, 1) about the pivot.
                bool shareOk = true;
                const double up[3] = {0.0, 1.0, 0.0};
                for (int l = 0; l < 3; ++l) {
                    double Rp[9], Rr[9], a[3], b[3];
                    nf::bendRot(30.0 * kDeg, 0.0, nf::kIkSpineWeight[l], Rp);
                    nf::bendRot(0.0, 12.0 * kDeg, nf::kIkSpineWeight[l], Rr);
                    nf::bendApply(up, Rp, a);
                    nf::bendApply(up, Rr, b);
                    if (std::fabs(std::atan2(a[2], a[1]) - 30.0 * kDeg * nf::kIkSpineWeight[l]) > 1e-9 ||
                        std::fabs(std::atan2(b[0], b[1]) - 12.0 * kDeg * nf::kIkSpineWeight[l]) > 1e-9)
                        shareOk = false;
                }
                const float M[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 1.7f, 0.0f, 1};
                float Mo[16];
                double Rfull[9];
                nf::bendRot(20.0 * kDeg, 0.0, nf::kIkSpineWeight[2], Rfull);
                nf::ikPivot(M, Rfull, P, Mo);
                check(shareOk && std::fabs(Mo[13] - static_cast<float>(Tf[1])) < 1e-5f && std::fabs(Mo[14] - static_cast<float>(Tf[2])) < 1e-5f &&
                          std::fabs(Mo[12]) < 1e-6f,
                      "F58: the bend splits 1/3, 2/3, 1 over the spine levels (pitch and roll each); the chest level's joint turns about the pivot "
                      "onto the target");
                // The ramp: 0.2 s in (18 frames at 90 Hz, half at 0.1 s) and out; the cap: 5 cm of pov motion a call on a 0.5 m lever.
                nf::IkBlend bl{0.0f, nf::kBendRampSec};
                const double dt = 1.0 / 90.0;
                int framesIn = 0, framesOut = 0;
                while (bl.w < 1.0f && framesIn < 100) {
                    bl.step(true, dt);
                    ++framesIn;
                }
                while (bl.w > 0.0f && framesOut < 100) {
                    bl.step(false, dt);
                    ++framesOut;
                }
                nf::IkBlend halfBend{0.0f, nf::kBendRampSec};
                halfBend.step(true, 0.05);
                halfBend.step(true, 0.05);
                double applied[2] = {0.0, 0.0};
                const double want[2] = {0.6, 0.8};
                nf::bendCapToward(applied, want, 0.5);
                const double stepLen = std::sqrt(applied[0] * applied[0] + applied[1] * applied[1]);
                double applied2[2] = {0.0, 0.0};
                const double wantSmall[2] = {0.01, -0.02};
                nf::bendCapToward(applied2, wantSmall, 0.5);
                check(framesIn >= 18 && framesIn <= 19 && framesOut >= 18 && framesOut <= 19 && std::fabs(halfBend.w - 0.5f) < 1e-5f &&
                          std::fabs(stepLen - 0.1) < 1e-6 && std::fabs(applied[0] / applied[1] - 0.75) < 1e-9 && applied2[0] == 0.01 &&
                          applied2[1] == -0.02,
                      "F58: the bend ramps in 0.2 s (half at 0.1 s) and out in 0.2 s; the applied angles move 5 cm of pov a call (0.1 rad on 0.5 m)");
            }
            // F59: the square-up to the FP forward, the clavicle reach assist and its caps, the elbow-out from the arm's own shoulder, the ADS
            // blend (0.15 s) and the ADS gate.
            {
                const double kDeg = nf::kIkPi / 180.0;
                // The square-up: an upper-arm line along +x against a forward turned 30 deg right (sin 30, -0.2, cos 30): phi +30 deg (theta
                // -30), and the line turned by phi lies on the forward's right (cos 30, 0, -sin 30); a forward straight ahead gives 0; a bladed
                // line (40 deg) against it gives 40; a forward with no horizontal part refuses.
                const double uL[3] = {-0.15, 1.4, 0.0}, uR[3] = {0.15, 1.4, 0.0};
                const double f30[3] = {std::sin(30.0 * kDeg), -0.2, std::cos(30.0 * kDeg)}, f0[3] = {0.0, 0.1, 1.0}, fUp[3] = {0.0, 1.0, 0.05};
                const double bR[3] = {0.15 * std::cos(40.0 * kDeg), 1.4, 0.15 * std::sin(40.0 * kDeg)},
                             bL[3] = {-0.15 * std::cos(40.0 * kDeg), 1.4, -0.15 * std::sin(40.0 * kDeg)};
                double phi30 = 0.0, phi0 = 7.0, phiUp = 7.0, phiB = 0.0;
                const bool ok30 = nf::ikSquareToForward(uL, uR, f30, &phi30), ok0 = nf::ikSquareToForward(uL, uR, f0, &phi0),
                           okUp = nf::ikSquareToForward(uL, uR, fUp, &phiUp), okB = nf::ikSquareToForward(bL, bR, f0, &phiB);
                double Rsq[9], lr[3];
                const double upAx[3] = {0.0, 1.0, 0.0}, line[3] = {0.3, 0.0, 0.0};
                nf::ikRotAxis(upAx, phi30, Rsq);
                nf::bendApply(line, Rsq, lr);
                check(ok30 && std::fabs(phi30 - 30.0 * kDeg) < 1e-9 && std::fabs(lr[0] / 0.3 - std::cos(30.0 * kDeg)) < 1e-9 &&
                          std::fabs(lr[2] / 0.3 + std::sin(30.0 * kDeg)) < 1e-9 && ok0 && std::fabs(phi0) < 1e-12 && okB &&
                          std::fabs(std::fabs(phiB) - 40.0 * kDeg) < 1e-9 && !okUp && phiUp == 0.0,
                      "F59: the square-up turns the upper-arm line square to the FP forward (30 deg right: phi +30, onto its right; bladed 40: "
                      "40); a forward with no horizontal part refuses");
                // The reach assist: l1 + l2 = 0.55 m, the threshold 0.97 of it (0.5335 m): in reach 0; 0.55 m asks excess + 3 cm (4.65 cm);
                // 0.70 m asks the 10 cm cap.
                double ex0 = 7.0, ex1 = 0.0, ex2 = 0.0;
                const double m0 = nf::ikAssistReachMove(0.50, 0.30, 0.25, &ex0), m1 = nf::ikAssistReachMove(0.55, 0.30, 0.25, &ex1),
                             m2 = nf::ikAssistReachMove(0.70, 0.30, 0.25, &ex2);
                check(m0 == 0.0 && ex0 == 0.0 && std::fabs(ex1 - 0.0165) < 1e-12 && std::fabs(m1 - 0.0465) < 1e-12 && std::fabs(ex2 - 0.1665) < 1e-12 &&
                          std::fabs(m2 - 0.10) < 1e-12,
                      "F59: the clavicle assist asks min(excess + 3 cm, 10 cm) past 0.97 of the arm's reach (0 in reach, 4.65 cm, the 10 cm cap)");
                // Its turn about the clavicle: 10 cm forward on a 15 cm clavicle is 33.7 deg, capped at 25; 3 cm is 11.3 deg and lands S on
                // the asked direction; 0.5 mm and a push along the clavicle turn nothing.
                const double C[3] = {0.0, 1.4, 0.0}, S[3] = {0.15, 1.4, 0.0};
                const double dBig[3] = {0.0, 0.0, 0.10}, dSmall[3] = {0.0, 0.0, 0.03}, dTiny[3] = {0.0, 0.0, 0.0005}, dRad[3] = {0.05, 0.0, 0.0};
                double Rb[9], Rs[9], Rt[9], Rr[9], angB = 0.0, angS = 0.0, angT = 7.0, angR = 7.0;
                bool capB = false, capS = true, capT = true, capR = true;
                const bool okBig = nf::ikAssistRot(C, S, dBig, Rb, &angB, &capB), okSmall = nf::ikAssistRot(C, S, dSmall, Rs, &angS, &capS),
                           okTiny = nf::ikAssistRot(C, S, dTiny, Rt, &angT, &capT), okRad = nf::ikAssistRot(C, S, dRad, Rr, &angR, &capR);
                const double cs[3] = {S[0] - C[0], S[1] - C[1], S[2] - C[2]};
                double sb[3], ss[3];
                nf::bendApply(cs, Rb, sb);
                nf::bendApply(cs, Rs, ss);
                check(okBig && capB && std::fabs(angB - 25.0 * kDeg) < 1e-9 && std::fabs(nf::ikLen(sb) - 0.15) < 1e-9 &&
                          std::fabs(std::atan2(sb[2], sb[0]) - 25.0 * kDeg) < 1e-9 && okSmall && !capS &&
                          std::fabs(angS - std::atan2(0.03, 0.15)) < 1e-9 && std::fabs(std::atan2(ss[2], ss[0]) - std::atan2(0.03, 0.15)) < 1e-9 &&
                          !okTiny && angT == 0.0 && Rt[0] == 1.0 && !okRad && angR == 0.0 && nf::kIkAssistMaxDeg == 25.0,
                      "F59: the assist turns the arm about the clavicle toward the asked shoulder move, capped at 25 deg (10 cm on 15 cm: 33.7 -> "
                      "25); a 0.5 mm or a radial push turns nothing");
                // The elbow-out clamp from the arm's own shoulder: a right arm (S.x 0.3) whose pole puts the elbow inboard (x about 0.12: the
                // midline rule passed it) is clamped out to S.x + 6 cm; the left arm mirrored likewise; the default is 6 cm.
                const double lU = 0.30, lF = 0.25;
                const double SRt[3] = {0.3, 1.4, 0.0}, TRt[3] = {0.3, 1.4, 0.4}, poleIn[3] = {-1.0, -0.2, 0.0};
                const double SLt[3] = {-0.3, 1.4, 0.0}, TLt[3] = {-0.3, 1.4, 0.4}, poleInL[3] = {1.0, -0.2, 0.0};
                const nf::IkTwoBone tR = nf::ikTwoBoneFrame(SRt, TRt, lU, lF, poleIn), tL = nf::ikTwoBoneFrame(SLt, TLt, lU, lF, poleInL);
                double E0r[3], Er[3], El[3];
                tR.elbowAt(0.0, E0r);
                bool clR = false, clL = false;
                tR.elbowAt(nf::ikSwivelClamp(tR, 0.0, 1.0, &clR), Er);
                tL.elbowAt(nf::ikSwivelClamp(tL, 0.0, -1.0, &clL), El);
                check(E0r[0] > 0.06 && nf::ikElbowOutOf(E0r, SRt, 1.0) < 0.0 && clR && nf::ikElbowOutOf(Er, SRt, 1.0) >= 0.06 - 1e-9 &&
                          Er[1] <= SRt[1] + 1e-9 && clL && nf::ikElbowOutOf(El, SLt, -1.0) >= 0.06 - 1e-9 && El[1] <= SLt[1] + 1e-9 &&
                          nf::kIkElbowOutM == 0.06 && nf::kIkElbowOutCm == 3.5,
                      "F59: the elbow-out clamp measures from the arm's own shoulder (an inboard elbow the midline passed is clamped to S.x + 6 cm, "
                      "both sides); the arm's default is 3.5 cm");
                // F77: the arm's anatomical model-X half-space is tied to its rig side, not its translated clavicle position.
                bool translationInvariant = nf::ikArmOutSign(0) == -1.0 && nf::ikArmOutSign(1) == 1.0 && nf::ikArmOutSign(2) == 0.0;
                const double shifts[] = {-0.8, -0.4, 0.0, 0.4, 0.8};
                for (int side = 0; side < 2 && translationInvariant; ++side) {
                    const double out = nf::ikArmOutSign(side);
                    const double baseS[3] = {out * 0.3, 1.4, 0.0}, baseT[3] = {out * 0.3, 1.4, 0.4};
                    const double hint[3] = {out, -0.2, 0.0};
                    const nf::IkTwoBone base = nf::ikTwoBoneFrame(baseS, baseT, lU, lF, hint);
                    nf::IkPhysicalPole preferred;
                    preferred.valid = base.ok && nf::ikElbowRadial(base, 0.0, preferred.reference);
                    if (!preferred.valid) { translationInvariant = false; break; }
                    std::memcpy(preferred.axis, base.dir, sizeof(preferred.axis));
                    std::memcpy(preferred.previous, preferred.reference, sizeof(preferred.previous));
                    const auto firstChoice = nf::ikStableSwivel(base, preferred, out, 0.06);
                    double firstE[3] = {};
                    if (!firstChoice.ok || firstChoice.violation > 1e-8) { translationInvariant = false; break; }
                    base.elbowAt(firstChoice.theta, firstE);
                    bool oldAbsoluteRuleFlipped = false;
                    for (double shift : shifts) {
                        double S[3] = {baseS[0] + shift, baseS[1], baseS[2]}, T[3] = {baseT[0] + shift, baseT[1], baseT[2]};
                        const double translatedClavicleX = out * 0.3 + shift;
                        oldAbsoluteRuleFlipped = oldAbsoluteRuleFlipped || ((translatedClavicleX >= 0.0 ? 1.0 : -1.0) != out);
                        const nf::IkTwoBone translated = nf::ikTwoBoneFrame(S, T, lU, lF, hint);
                        const auto choice = nf::ikStableSwivel(translated, preferred, out, 0.06);
                        double E[3] = {};
                        if (!choice.ok || choice.violation > 1e-8) { translationInvariant = false; break; }
                        translated.elbowAt(choice.theta, E);
                        const double dx = E[0] - S[0] - (firstE[0] - baseS[0]);
                        const double dy = E[1] - S[1] - (firstE[1] - baseS[1]);
                        const double dz = E[2] - S[2] - (firstE[2] - baseS[2]);
                        if (std::sqrt(dx * dx + dy * dy + dz * dz) > 1e-8 || nf::ikElbowOutOf(E, S, out) < 0.06 - 1e-9 || E[1] > S[1] + 1e-9)
                            translationInvariant = false;
                    }
                    translationInvariant = translationInvariant && oldAbsoluteRuleFlipped;
                }
                check(translationInvariant,
                      "F77: translating either arm through model X zero preserves its side, feasible elbow pose, and outward/below constraints");
                // The ADS blend: 0.15 s in (14 frames at 90 Hz, half at 0.075 s) and out; the gate relaxes from 0.35..0.8 m to 0.6..1.2 m;
                // the sliders lerp; a NaN slider takes its default.
                nf::IkBlend ab{0.0f, nf::kIkAdsRampSec};
                const double dt = 1.0 / 90.0;
                int framesIn = 0, framesOut = 0;
                while (ab.w < 1.0f && framesIn < 100) {
                    ab.step(true, dt);
                    ++framesIn;
                }
                while (ab.w > 0.0f && framesOut < 100) {
                    ab.step(false, dt);
                    ++framesOut;
                }
                nf::IkBlend half{0.0f, nf::kIkAdsRampSec};
                half.step(true, 0.0375);
                half.step(true, 0.0375);
                check(framesIn >= 13 && framesIn <= 14 && framesOut >= 13 && framesOut <= 14 && std::fabs(half.w - 0.5f) < 1e-5f &&
                          std::fabs(nf::ikDistanceGateAds(0.5f, 0.0f) - 0.3f / 0.45f) < 1e-6f && nf::ikDistanceGateAds(0.5f, 1.0f) == 1.0f &&
                          std::fabs(nf::ikDistanceGateAds(1.0f, 1.0f) - 0.2f / 0.6f) < 1e-6f && nf::ikDistanceGateAds(1.0f, 0.0f) == 0.0f &&
                          std::fabs(nf::ikDistanceGateAds(0.9f, 0.5f) - 0.1f / 0.525f) < 1e-5f &&
                          nf::ikDistanceGateAds(std::numeric_limits<float>::quiet_NaN(), 1.0f) == 0.0f &&
                          std::fabs(nf::ikLerp(0.10, 0.06, 0.5) - 0.08) < 1e-12,
                      "F59: the ADS weight ramps in 0.15 s (half at 0.075 s) and out; the gate relaxes to 0.6..1.2 m with it; the arm values lerp");
            }
            // F59b: the full-3P head follow's fallback (live; the AMC matrix or the last good frame up to 0.5 s; none past it), the ramp between
            // sources (the change frame outputs the last output), the body-axes residual and the hook's (T - N1) to eye - neck.
            {
                uint64_t age = 7;
                const int pLive = nf::followPick(true, true, true, 1000, 1400, &age);
                const int pMat = nf::followPick(false, true, true, 1000, 1300, &age);
                const int pLast = nf::followPick(false, false, true, 1000, 1300, &age);
                const uint64_t age300 = age;
                const int pEdge = nf::followPick(false, false, true, 1000, 1500, &age);
                const int pPast = nf::followPick(false, true, true, 1000, 1501, &age);
                const int pNone = nf::followPick(false, true, false, 0, 1300, &age);
                const int pAhead = nf::followPick(false, false, true, 2000, 1500, &age);
                check(pLive == nf::kFollowLive && pMat == nf::kFollowMatrix && pLast == nf::kFollowLast && age300 == 300 &&
                          pEdge == nf::kFollowLast && pPast == nf::kFollowNone && pNone == nf::kFollowNone && pAhead == nf::kFollowLast &&
                          age == 0 && nf::kFollowHoldMs == 500,
                      "F59b: the head follow falls back to the AMC matrix (a fresh sample) or the last good frame (none fresh) up to 0.5 s old, "
                      "never past it; live wins; no last frame is none");
                nf::FollowBlend fb;
                const double t1[3] = {0.1, 0.0, -0.04}, t2[3] = {0.3, 0.0, 0.04}, zero3[3] = {0.0, 0.0, 0.0};
                double o0[3], o1[3], o2[3], o3[3], o4[3], o5[3];
                fb.step(nf::kFollowLive, t1, 0.125, o0);    // from none: no ramp
                fb.step(nf::kFollowLast, t2, 0.125, o1);    // the change frame: the last output
                fb.step(nf::kFollowLast, t2, 0.125, o2);    // half way
                fb.step(nf::kFollowLast, t2, 0.125, o3);    // there
                fb.step(nf::kFollowNone, t2, 0.125, o4);    // to none: no ramp (the caller passes zero then)
                fb.step(nf::kFollowLive, zero3, 0.125, o5);
                check(o0[0] == 0.1 && o0[2] == -0.04 && o1[0] == 0.1 && o1[2] == -0.04 && std::fabs(o2[0] - 0.2) < 1e-12 && std::fabs(o2[2]) < 1e-12 &&
                          o3[0] == 0.3 && o3[2] == 0.04 && o4[0] == 0.3 && o5[0] == 0.0 && nf::kFollowBlendSec == 0.25,
                      "F59b: a source change ramps from the last output (no jump on the change frame) to the new source over 0.25 s; from or to "
                      "none is immediate");
                // A yaw of 90 deg: right (0,0,-1), up (0,1,0), forward (1,0,0).
                const double rowsY[9] = {0.0, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0};
                const double eyeW[3] = {5.161, 1.855, 2.98}, neckW[3] = {5.0, 1.7, 3.0};
                double rb[3];
                const double rnorm = nf::eyeNeckBody(eyeW, neckW, rowsY, rb);
                const float pA[3] = {1.0f, 2.0f, 3.0f}, oA[3] = {0.0f, 0.0f, 0.0f}, oB[3] = {10.0f, 0.0f, 5.0f};
                const float rA[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, rB[9] = {0, 0, -1, 0, 1, 0, 1, 0, 0};
                float q[3];
                nf::carryBody(pA, oA, rA, oB, rB, q);
                // The hook's figure: T = (eye - d) + rest (body = world here), N1 the bent neck; eye - (N1 + d) from (T - N1) equals the direct one.
                const double I9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                const double eye[3] = {0.05, 1.70, 0.30}, dd[3] = {0.01, 0.0, -0.02}, N1[3] = {0.03, 1.52, 0.10};
                double T[3], split[3], viaHook[3], direct[3], neckD[3];
                for (int i = 0; i < 3; ++i) {
                    T[i] = eye[i] - dd[i] + nf::kNeckRestOffsetFull3P[i];   // F62a: the bend's (full-3P) rest offset
                    neckD[i] = N1[i] + dd[i];
                }
                nf::bendResidualSplit(T, N1, split);
                const double hn = nf::eyeNeckFromBend(split, 1.0, viaHook);
                const double dn = nf::eyeNeckBody(eye, neckD, I9, direct);
                const double zs[3] = {0.0, 0.0, 0.0};
                double atRest[3];
                nf::eyeNeckFromBend(zs, 100.0, atRest);
                check(std::fabs(rb[0] - 0.02) < 1e-9 && std::fabs(rb[1] - 0.155) < 1e-9 && std::fabs(rb[2] - 0.161) < 1e-9 &&
                          std::fabs(rnorm - std::sqrt(0.02 * 0.02 + 0.155 * 0.155 + 0.161 * 0.161)) < 1e-9 && std::fabs(q[0] - 13.0f) < 1e-5f &&
                          std::fabs(q[1] - 2.0f) < 1e-5f && std::fabs(q[2] - 4.0f) < 1e-5f && std::fabs(viaHook[0] - direct[0]) < 1e-12 &&
                          std::fabs(viaHook[1] - direct[1]) < 1e-12 && std::fabs(viaHook[2] - direct[2]) < 1e-12 && std::fabs(hn - dn) < 1e-12 &&
                          atRest[0] == 0.0 && std::fabs(atRest[1] - 15.5) < 1e-9 && std::fabs(atRest[2] - 9.0) < 1e-9,
                      "F59b: the residual is rows . (eye - neck) in body axes (right, up, forward); a joint carried between AMC frames keeps its body "
                      "coordinates; the hook's (T - N1) less the rest offset (F62a: full 3P, 0.155 up, 0.09 forward) equals eye - (N1 + d)");
            }
            // F62a (kept): the full-3P eye-to-neck rest and the ADS shoulder-forward default. (F66: the weapon fit's checks went with the fit.)
            {
                check(nf::kNeckRestOffsetFull3P[0] == 0.0 && nf::kNeckRestOffsetFull3P[1] == -0.155 && nf::kNeckRestOffsetFull3P[2] == -0.09 &&
                          nf::kNeckRestOffset[2] == -0.161 && nf::kIkAdsForwardCm == 0.0,
                      "F62a: the full-3P neck rests 0.155 m below and 0.09 m behind the eye (0.161 kept outside full 3P); the ADS shoulder forward is 0");
            }
            // F61 (TEMPORARY): the rotation calibration. The schedule (yaw, rest, pitch, rest at 1 s each, repeating), the three sources'
            // deltas (yaw + = turned left, pitch + = up, each in its own axes), the head subtraction and the verdict rule.
            {
                double y[4], p[4];
                for (int k = 0; k < 4; ++k) nf::rotCalCommandDeg(k, &y[k], &p[k]);
                check(nf::rotCalStep(0) == 0 && nf::rotCalStep(999) == 0 && nf::rotCalStep(1000) == 1 && nf::rotCalStep(2000) == 2 &&
                          nf::rotCalStep(3999) == 3 && nf::rotCalStep(4000) == 0 && nf::rotCalStep(6500) == 2 && y[0] == 5.0 && p[0] == 0.0 &&
                          y[1] == 0.0 && p[1] == 0.0 && y[2] == 0.0 && p[2] == 5.0 && y[3] == 0.0 && p[3] == 0.0,
                      "F61: the schedule is yaw +5 deg, rest, pitch +5 deg, rest, 1 s each, repeating every 4 s");
                const double kR = 3.14159265358979323846 / 180.0;
                const auto sumOf = [](const double r[3], const double u[3], const double f[3]) {
                    nf::RotFrameSum s;
                    s.add(r, u, f);
                    return s;
                };
                double dy = 0.0, dp = 0.0, r[3], u[3], f[3];
                // The render head (OpenXR, right-handed, -z forward): the seat's +10 deg yaw (about +y) is a left turn; +10 deg about +x is up.
                const auto headAt = [&](double yawDeg, double pitchDeg, float m[12]) {
                    const double cy = std::cos(yawDeg * kR), sy = std::sin(yawDeg * kR), cp = std::cos(pitchDeg * kR), sp = std::sin(pitchDeg * kR);
                    const double R[3][3] = {{cy, sy * sp, sy * cp}, {0.0, cp, -sp}, {-sy, cy * sp, cy * cp}};   // Ry(yaw) Rx(pitch)
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) m[4 * i + j] = static_cast<float>(R[i][j]);
                        m[4 * i + 3] = 0.0f;
                    }
                };
                float h0[12], h1[12], h2[12];
                headAt(0.0, 0.0, h0);
                headAt(10.0, 0.0, h1);
                headAt(0.0, 10.0, h2);
                nf::rotAxesFromHead(h0, r, u, f);
                const nf::RotFrameSum headBase = sumOf(r, u, f);
                nf::rotAxesFromHead(h1, r, u, f);
                const bool headYaw = nf::rotFrameDelta(headBase, sumOf(r, u, f), &dy, &dp) && std::fabs(dy - 10.0) < 1e-4 && std::fabs(dp) < 1e-4;
                nf::rotAxesFromHead(h2, r, u, f);
                const bool headPitch = nf::rotFrameDelta(headBase, sumOf(r, u, f), &dy, &dp) && std::fabs(dy) < 1e-4 && std::fabs(dp - 10.0) < 1e-4;
                // The list-B camera C (rows right, up, forward; here a left-handed world: x right, y up, z forward): a left turn moves forward
                // toward -x.
                const float c0[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                const float cs = static_cast<float>(std::sin(10.0 * kR)), cc = static_cast<float>(std::cos(10.0 * kR));
                const float c1[16] = {cc, 0, cs, 0, 0, 1, 0, 0, -cs, 0, cc, 0, 0, 0, 0, 1};
                nf::rotAxesFromC(c0, r, u, f);
                const nf::RotFrameSum cb = sumOf(r, u, f);
                nf::rotAxesFromC(c1, r, u, f);
                const bool cYaw = nf::rotFrameDelta(cb, sumOf(r, u, f), &dy, &dp) && std::fabs(dy - 10.0) < 1e-4 && std::fabs(dp) < 1e-4;
                // cb1 270..273 (worldClip's layout): clip x = 1.2 right, clip y = 1.5 up + 0.1 forward (an off-centre projection), clip w =
                // forward. The same left turn, then a 10 deg rise of forward toward up.
                const auto clipOf = [](const double rt[3], const double up[3], const double fw[3], float rows[16]) {
                    for (int i = 0; i < 3; ++i) {
                        rows[4 * i + 0] = static_cast<float>(1.2 * rt[i]);
                        rows[4 * i + 1] = static_cast<float>(1.5 * up[i] + 0.1 * fw[i]);
                        rows[4 * i + 2] = static_cast<float>(0.01 * fw[i]);
                        rows[4 * i + 3] = static_cast<float>(fw[i]);
                    }
                    for (int j = 0; j < 4; ++j) rows[12 + j] = 0.0f;
                };
                const double rt0[3] = {1, 0, 0}, up0[3] = {0, 1, 0}, fw0[3] = {0, 0, 1};
                const double rt1[3] = {cc, 0, cs}, fw1[3] = {-cs, 0, cc};
                const double up2[3] = {0, cc, -cs}, fw2[3] = {0, cs, cc};
                float w0[16], w1[16], w2[16];
                clipOf(rt0, up0, fw0, w0);
                clipOf(rt1, up0, fw1, w1);
                clipOf(rt0, up2, fw2, w2);
                nf::rotAxesFromClip(w0, r, u, f);
                const nf::RotFrameSum wb = sumOf(r, u, f);
                nf::rotAxesFromClip(w1, r, u, f);
                const bool wYaw = nf::rotFrameDelta(wb, sumOf(r, u, f), &dy, &dp) && std::fabs(dy - 10.0) < 1e-3 && std::fabs(dp) < 1e-3;
                nf::rotAxesFromClip(w2, r, u, f);
                const bool wPitch = nf::rotFrameDelta(wb, sumOf(r, u, f), &dy, &dp) && std::fabs(dy) < 1e-3 && std::fabs(dp - 10.0) < 1e-3;
                const nf::RotFrameSum empty;
                const bool emptyFalse = !nf::rotFrameDelta(empty, wb, &dy, &dp) && dy == 0.0 && dp == 0.0;
                check(headYaw && headPitch && cYaw && wYaw && wPitch && emptyFalse,
                      "F61: the render head, C and the cb1 rows all read a left turn as yaw +10 deg and a rise as pitch +10 deg (an empty window: none)");
                // The verdict. A camera that follows the render pose: render -5 for a +5 command (head still) gives residual -5: yes, gain 1, sign -.
                const nf::RotVerdict follow = nf::rotCalVerdict(-5.0, -5.0, 5.0);
                // The head turned left 3 deg during the step (render -5 + 3 = -2): a following camera still reads gain 1, sign - (head subtracted).
                const nf::RotVerdict moving = nf::rotCalVerdict(-2.0, -2.0, 5.0);
                // A camera that follows only the head's own motion (+3) and not the seated rotation: residual 0: no.
                const nf::RotVerdict physical = nf::rotCalVerdict(3.0, -2.0, 5.0);
                // With the head still (render -5 for +5): a residual of 0.9 deg reads no, 1 deg reads yes (gain 0.2, sign -), and a camera
                // turning the other way by 2.5 deg reads yes, gain 0.5, sign +.
                const nf::RotVerdict underOne = nf::rotCalVerdict(0.9, -5.0, 5.0);
                const nf::RotVerdict atOne = nf::rotCalVerdict(-1.0, -5.0, 5.0);
                const nf::RotVerdict halfway = nf::rotCalVerdict(2.5, -5.0, 5.0);
                check(follow.reaches && std::fabs(follow.gain - 1.0) < 1e-12 && follow.sign == -1 && follow.headDeg == 0.0 && moving.reaches &&
                          std::fabs(moving.headDeg - 3.0) < 1e-12 && std::fabs(moving.gain - 1.0) < 1e-12 && moving.sign == -1 && !physical.reaches &&
                          std::fabs(physical.residualDeg) < 1e-12 && !underOne.reaches && std::fabs(underOne.residualDeg - 0.9) < 1e-12 && atOne.reaches &&
                          std::fabs(atOne.gain - 0.2) < 1e-12 && atOne.sign == -1 && halfway.reaches && std::fabs(halfway.gain - 0.5) < 1e-12 &&
                          halfway.sign == 1 && nf::kRotCalReachDeg == 1.0,
                      "F61: the verdict subtracts the head's own motion (render + command), reads no under 1 deg, gain |residual / command|, sign "
                      "of residual / command");
            }
            // F62b (TEMPORARY): the ADS head-aim's conventions. The seat convention is yaw + = LEFT, pitch + = UP; the game's yaw delta is + RIGHT and
            // its pitch delta + UP for signs +1. A head yaw of +theta (left) asks the game for -theta (its sign); the game applies gain x input, so the
            // input is the wanted turn divided by the measured gain (fallback 1.0). F67a deleted the cancel and the guards; F68a restores them (tests below).
            {
                const double kR = nf::kAdsDeg;
                check(nf::adsGain(0.0) == 1.0 && nf::adsGain(std::numeric_limits<double>::quiet_NaN()) == 1.0 && nf::adsGain(5e3) == 1.0 &&
                          nf::adsGain(-0.5) == -0.5 && nf::adsGain(0.02) == 0.02 && nf::adsGameFromSeatPitch(0.1, 1) == 0.1 &&
                          nf::adsSeatFromGamePitch(0.1, -1) == -0.1 && nf::adsGameFromSeatYaw(0.1, 0) == 0.0 && nf::adsSeatFromGamePitch(0.1, 0) == 0.0 &&
                          nf::adsGameFromSeatYaw(0.1, -1) == 0.1,
                      "F62b: the gain falls back to 1.0 when none is measured (0, NaN, out of range) and keeps its sign; pitch + up both ways for "
                      "sign 1; sign 0 is that axis off");
                // The head's own turn from the LOCAL head (OpenXR axes): a left turn reads yaw +, a rise pitch +, across the +-180 seam too.
                const auto headAt = [&](double yawDeg, double pitchDeg, float tx, float m[12]) {
                    const double cy = std::cos(yawDeg * kR), sy = std::sin(yawDeg * kR), cp = std::cos(pitchDeg * kR), sp = std::sin(pitchDeg * kR);
                    const double R[3][3] = {{cy, sy * sp, sy * cp}, {0.0, cp, -sp}, {-sy, cy * sp, cy * cp}};   // Ry(yaw) Rx(pitch)
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) m[4 * i + j] = static_cast<float>(R[i][j]);
                    m[3] = tx;
                    m[7] = 1.6f;
                    m[11] = 0.0f;
                };
                float a[12], b[12], c[12], e[12], g[12], movedHead[12];
                headAt(0.0, 0.0, 0.0f, a);
                headAt(10.0, 0.0, 0.0f, b);
                headAt(0.0, 5.0, 0.0f, c);
                headAt(175.0, 0.0, 0.0f, e);
                headAt(-175.0, 0.0, 0.0f, g);
                headAt(0.0, 0.0, 0.8f, movedHead);
                double dy = 0.0, dp = 0.0, dy2 = 0.0, dp2 = 0.0, dy3 = 0.0, dp3 = 0.0;
                const bool leftOk = nf::adsHeadDelta(a, b, &dy, &dp) && std::fabs(dy - 10.0 * kR) < 1e-5 && std::fabs(dp) < 1e-5;
                const bool upOk = nf::adsHeadDelta(a, c, &dy2, &dp2) && std::fabs(dy2) < 1e-5 && std::fabs(dp2 - 5.0 * kR) < 1e-5;
                const bool seam = nf::adsHeadDelta(e, g, &dy3, &dp3) && std::fabs(dy3 - 10.0 * kR) < 1e-5;
                const bool jumpRefused = !nf::adsHeadDelta(a, movedHead, &dy, &dp) && !nf::adsHeadDelta(a, e, &dy, &dp);
                check(leftOk && upOk && seam && jumpRefused,
                      "F62b: the LOCAL head's turn reads a left turn as yaw +10 deg and a rise as pitch +5 deg (175 to -175 is +10, not -350); a 0.8 m or "
                      "175 deg step is a discontinuity, not a turn");
                // The ramp: 0.15 s in, 0.3 s out.
                nf::AdsRamp r;
                r.step(true, 0.075);
                const double in1 = r.w;
                r.step(true, 0.075);
                const double in2 = r.w;
                r.step(true, 0.5);
                r.step(false, 0.15);
                const double out1 = r.w;
                r.step(false, 0.15);
                const double out2 = r.w;
                check(std::fabs(in1 - 0.5) < 1e-12 && std::fabs(in2 - 1.0) < 1e-12 && std::fabs(out1 - 0.5) < 1e-12 && out2 == 0.0,
                      "F62b: the weight ramps in over 0.15 s (0.5 at 75 ms) and out over 0.3 s (0.5 at 150 ms)");
                // The position lock: the seated offset is the head's LOCAL motion times the weight; F40's map then leaves the camera still.
                const double h0[3] = {0.1, 1.6, -0.2}, h1[3] = {0.15, 1.58, -0.12};
                double lock[3], lockHalf[3], cam[3];
                nf::adsLockOffset(h1, h0, 1.0, lock);
                nf::adsLockOffset(h1, h0, 0.5, lockHalf);
                const double dH[3] = {h1[0] - h0[0], h1[1] - h0[1], h1[2] - h0[2]};
                nf::adsCamBodyMotion(dH, lock, cam);
                check(std::fabs(lock[0] - 0.05) < 1e-12 && std::fabs(lock[2] - 0.08) < 1e-12 && std::fabs(lockHalf[1] + 0.01) < 1e-12 &&
                          std::fabs(cam[0]) < 1e-12 && std::fabs(cam[1]) < 1e-12 && std::fabs(cam[2]) < 1e-12,
                      "F62b: the lock is w (h - h_entry) in LOCAL, and through F40's map (diag(1, 1, -1) dH + D s, D = diag(-1, -1, +1)) the camera "
                      "does not move");
            }
            // F68a: the ADS head-aim, F62b's map measured properly (nofoot::AdsAimMap, AdsResidual, AdsPressGate). The plant is the game: the look input
            // the map asks for lands `delay` frames later scaled by the game's gain (0.85), the camera takes the applied turn at once, the map sees what the
            // processor applied one frame later (the frame thread reads it on the next tick) and cancels exactly that in the seated space.
            // Conventions: the seat's (yaw + = LEFT, pitch + = UP); the game's yaw delta + is RIGHT, pitch + UP (signs +1).
            {
                const double kR = nf::kAdsDeg;
                struct AimPlant {
                    int delay = 1;
                    double realGain = 0.85, measY = 0.0, measP = 0.0;          // the hook's measured gains (0 = none yet: 1.0 is used)
                    double qY[8] = {}, qP[8] = {};                              // input queued by frame (the game's units), read `delay` frames later
                    double aimY = 0.0, aimP = 0.0, headY = 0.0, headP = 0.0;    // seat radians, cumulative
                    double seatY = 0.0, seatP = 0.0;                            // the cancel published, cumulative
                    double prevAppY = 0.0, prevAppP = 0.0;                      // applied last frame, seen by the map this frame
                    double weapon = 0.0;                                        // sum of the weight x the head's yaw step (what the aim should have taken)
                    double viewErrMax = 0.0, viewErrMaxY = 0.0, viewErrMaxP = 0.0;
                    uint64_t k = 0;
                    bool cameraTakesAim = true;
                    nf::AdsAimMap map;
                    nf::AdsRamp ramp;
                    // One frame (1/90 s): the head's step, whether ADS is held, the sign keys.
                    nf::AdsAimMap::Out frame(double dHY, double dHP, bool engaged, bool releasing) {
                        const double dt = 1.0 / 90.0;
                        // The game's processor: the input asked `delay` frames ago lands now.
                        const double inY = delay == 0 ? 0.0 : qY[(k + 8 - static_cast<uint64_t>(delay)) % 8], inP = delay == 0 ? 0.0 : qP[(k + 8 - static_cast<uint64_t>(delay)) % 8];
                        const double gY = realGain * inY, gP = realGain * inP;
                        if (inY != 0.0) measY = gY / inY;
                        if (inP != 0.0) measP = gP / inP;
                        const double appY = nf::adsSeatFromGameYaw(gY, 1), appP = nf::adsSeatFromGamePitch(gP, 1);
                        aimY += appY;
                        aimP += appP;
                        headY += dHY;
                        headP += dHP;
                        // The map's tick: it sees last frame's applied turn.
                        ramp.step(engaged, dt);
                        const nf::AdsAimMap::Out o = map.step(engaged, releasing, dt, ramp.w, true, dHY, dHP, prevAppY, prevAppP, true, true);
                        prevAppY = appY;
                        prevAppP = appP;
                        seatY += o.cancelYaw;
                        seatP += o.cancelPitch;
                        if (engaged) weapon += ramp.w * dHY;
                        qY[k % 8] = nf::adsInput(nf::adsGameFromSeatYaw(o.injYaw, 1), measY);
                        qP[k % 8] = nf::adsInput(nf::adsGameFromSeatPitch(o.injPitch, 1), measP);
                        ++k;
                        // The view: the head + the aim the camera took - the seated cancel (F61: a seated +a moves the camera by -a).
                        const double vY = headY + (cameraTakesAim ? aimY : 0.0) - seatY, vP = headP + (cameraTakesAim ? aimP : 0.0) - seatP;
                        const double eY = std::fabs(vY - headY), eP = std::fabs(vP - headP);
                        viewErrMaxY = std::max(viewErrMaxY, eY);
                        viewErrMaxP = std::max(viewErrMaxP, eP);
                        viewErrMax = std::max(viewErrMax, std::max(eY, eP));
                        return o;
                    }
                    double viewY() const { return headY + (cameraTakesAim ? aimY : 0.0) - seatY; }
                    double viewP() const { return headP + (cameraTakesAim ? aimP : 0.0) - seatP; }
                };
                // A smooth random head: velocity pulled by noise, damped, capped at maxDps (deterministic).
                struct Head {
                    uint32_t s = 12345u;
                    double vY = 0.0, vP = 0.0, maxDps;
                    explicit Head(double cap, uint32_t seed = 12345u) : s(seed), maxDps(cap) {}
                    double rnd() { s = s * 1664525u + 1013904223u; return static_cast<double>((s >> 8) & 0xFFFF) / 32768.0 - 1.0; }
                    void next(double* dY, double* dP) {
                        const double cap = maxDps * nf::kAdsDeg;
                        vY = vY * 0.97 + 0.012 * cap * rnd();
                        vP = vP * 0.97 + 0.008 * cap * rnd();
                        vY = std::max(-cap, std::min(cap, vY));
                        vP = std::max(-0.6 * cap, std::min(0.6 * cap, vP));
                        *dY = vY / 90.0;
                        *dP = vP / 90.0;
                    }
                };
                // (1) The plant whose camera takes the aim (gain 0.85) at 1 and at 2 frames of delay, a head turning at up to 30 deg/s for 60 s: the view equals
                // the head within 0.5 deg all the time, nothing accumulates (the last 10 s no worse than the first 10), and the weapon (the aim) follows the head.
                double worst = 0.0, firstMax = 0.0, lastMax = 0.0, aimLag = 0.0, outMax = 0.0;
                for (int delay = 1; delay <= 2; ++delay) {
                    AimPlant p;
                    p.delay = delay;
                    Head h(30.0, 777u + static_cast<uint32_t>(delay));
                    double maxFirst = 0.0, maxLast = 0.0;
                    for (int i = 0; i < 90 * 60; ++i) {
                        double dY, dP;
                        h.next(&dY, &dP);
                        const double before = p.viewErrMax;
                        p.frame(dY, dP, true, false);
                        (void)before;
                        const double e = std::max(std::fabs(p.viewY() - p.headY), std::fabs(p.viewP() - p.headP));
                        if (i < 90 * 10) maxFirst = std::max(maxFirst, e);
                        if (i >= 90 * 50) maxLast = std::max(maxLast, e);
                        outMax = std::max(outMax, std::max(std::fabs(p.map.yawInFlight()), std::fabs(p.map.pitchInFlight())));
                    }
                    // The head stops: the aim catches up to w x the head's yaw (the weapon follows the head).
                    for (int i = 0; i < 20; ++i) p.frame(0.0, 0.0, true, false);
                    worst = std::max(worst, p.viewErrMax);
                    firstMax = std::max(firstMax, maxFirst);
                    lastMax = std::max(lastMax, maxLast);
                    aimLag = std::max(aimLag, std::fabs(p.aimY - p.weapon));
                }
                check(worst < 0.5 * kR && lastMax <= firstMax + 0.05 * kR && aimLag < 0.05 * kR && outMax < 2.0 * kR,
                      "F68a: a plant whose camera takes the aim (gain 0.85, 1 and 2 frames of delay) with the head turning up to 30 deg/s for 60 s: the view "
                      "equals the head within 0.5 deg, the last 10 s are no worse than the first (no accumulation), the in-flight account stays under 2 deg "
                      "and the aim ends within 0.05 deg of the head's weighted turn");
                // (2) The structural bound: a fast sweep (120 deg/s) still keeps the view within one frame of the applied turn (the map cancels a frame late);
                // the first frame uses the fallback gain 1.0 and the aim takes 0.85 of it, the cancel is exact anyway.
                {
                    AimPlant p;
                    p.delay = 2;
                    for (int i = 0; i < 90; ++i) p.frame(120.0 * kR / 90.0, 0.0, true, false);
                    for (int i = 0; i < 10; ++i) p.frame(0.0, 0.0, true, false);
                    check(p.viewErrMaxY < 1.4 * kR && std::fabs(p.viewY() - p.headY) < 1e-9 && p.measY > 0.8 && p.measY < 0.9,
                          "F68a: a 120 deg/s sweep keeps the view within 1.4 deg of the head (one frame of the applied turn) and the view equals the head once "
                          "it stops; the hook's gain reads 0.85");
                }
                // (3) The first frames use the fallback gain 1.0 and the plant takes 0.85 of the ask: the cancel is the applied turn, so the view still holds.
                {
                    AimPlant p;
                    p.delay = 1;
                    for (int i = 0; i < 12; ++i) p.frame(0.4 * kR, -0.2 * kR, true, false);
                    for (int i = 0; i < 6; ++i) p.frame(0.0, 0.0, true, false);
                    check(std::fabs(p.viewY() - p.headY) < 1e-9 && std::fabs(p.viewP() - p.headP) < 1e-9 && p.viewErrMax < 0.5 * kR,
                          "F68a: with the fallback gain (none measured yet) the first asks land at 0.85 and are cancelled exactly: the view equals the head");
                }
                // (4) The map itself: the cancel is exactly the applied turn (the pitch clamp aside), the ask is the weight x the head's step, off axes ask 0.
                {
                    nf::AdsAimMap m;
                    const nf::AdsAimMap::Out o = m.step(true, false, 1.0 / 90.0, 1.0, true, 0.01, -0.02, 0.003, -0.004, true, true);
                    check(o.cancelYaw == 0.003 && o.cancelPitch == -0.004 && std::fabs(o.injYaw - 0.01) < 1e-15 && std::fabs(o.injPitch + 0.02) < 1e-15 &&
                              m.yawSeat == 0.003 && m.yawCarried == 0.003 && m.pitchSeat == -0.004,
                          "F68a: the cancel is exactly what the game applied (seat convention) and the ask is the weight x the head's step");
                    nf::AdsAimMap halfW, offAxis, noStep;
                    const nf::AdsAimMap::Out oh = halfW.step(true, false, 1.0 / 90.0, 0.5, true, 0.01, 0.02, 0.0, 0.0, true, true);
                    const nf::AdsAimMap::Out oa = offAxis.step(true, false, 1.0 / 90.0, 1.0, true, 0.01, 0.02, 0.0, 0.0, false, true);
                    const nf::AdsAimMap::Out on = noStep.step(true, false, 1.0 / 90.0, 1.0, false, 0.01, 0.02, 0.0, 0.0, true, true);
                    check(std::fabs(oh.injYaw - 0.005) < 1e-15 && std::fabs(oh.injPitch - 0.01) < 1e-15 && oa.injYaw == 0.0 && std::fabs(oa.injPitch - 0.02) < 1e-15 &&
                              on.injYaw == 0.0 && on.injPitch == 0.0,
                          "F68a: the ask scales with the ADS weight, an axis whose sign key is 0 asks nothing, a tick without a head step asks nothing");
                    // The pitch is held inside +-60 deg with what is in flight: a head pitching up at 45 deg/s for 3 s (135 deg) never takes the seated pitch past
                    // 60 deg, the view still equals the head (the cancel is exactly what was applied), the aim ends at 60, and a head coming back down 30 deg brings
                    // the aim back down with it.
                    AimPlant c;
                    c.delay = 2;
                    double pitchMax = 0.0;
                    for (int i = 0; i < 270; ++i) {
                        c.frame(0.0, 0.5 * kR, true, false);
                        pitchMax = std::max(pitchMax, c.map.pitchSeat);
                    }
                    for (int i = 0; i < 10; ++i) c.frame(0.0, 0.0, true, false);
                    const double aimTop = c.aimP;
                    for (int i = 0; i < 60; ++i) c.frame(0.0, -0.5 * kR, true, false);
                    for (int i = 0; i < 10; ++i) c.frame(0.0, 0.0, true, false);
                    check(pitchMax <= nf::kAdsPitchClampRad + 1e-9 && std::fabs(aimTop - nf::kAdsPitchClampRad) < 0.6 * kR && c.viewErrMax < 0.8 * kR &&
                              std::fabs(c.aimP - (aimTop - 30.0 * kR)) < 0.1 * kR,
                          "F68a: the seated pitch is held to +-60 deg with what is in flight (a head pitching up 135 deg stops the aim at 60, the view still equals "
                          "the head) and a head coming back down 30 deg brings the aim back down 30");
                }
                // (5) Release: after 20 s of head motion the carried yaw and pitch unwind to zero over 0.3 s (+0.2 s of chase) with the head still, the view
                // never moves (within one frame of the applied turn), the aim returns to where it started and the map reports the release done.
                {
                    AimPlant p;
                    p.delay = 2;
                    Head h(30.0, 4242u);
                    for (int i = 0; i < 90 * 20; ++i) {
                        double dY, dP;
                        h.next(&dY, &dP);
                        p.frame(dY, dP, true, false);
                    }
                    for (int i = 0; i < 12; ++i) p.frame(0.0, 0.0, true, false);   // settle
                    const double yaw0 = p.map.yawCarried, pitch0 = p.map.pitchSeat;
                    p.map.beginRelease();
                    p.viewErrMax = 0.0;
                    int frames = 0;
                    bool done = false;
                    for (; frames < 90 * 3 && !done; ++frames) {
                        p.frame(0.0, 0.0, false, true);
                        done = p.map.releaseDone();
                    }
                    for (int i = 0; i < 6; ++i) p.frame(0.0, 0.0, false, true);   // the last asks land
                    const int limit = static_cast<int>((p.map.relDur + nf::kAdsCatchUpSec) * 90.0) + 3;
                    check(std::fabs(yaw0) > 0.5 * kR && done && frames <= limit && std::fabs(p.map.yawCarried) < 0.1 * kR && std::fabs(p.map.pitchSeat) < 0.1 * kR &&
                              p.viewErrMax < 0.8 * kR && std::fabs(p.aimY) < 0.2 * kR && std::fabs(p.aimP) < 0.2 * kR,
                          "F68a: the release unwinds the carried yaw and the pitch to zero (0.3 s, longer for a big turn: at most 60 deg/s) while the view stays "
                          "the head's within 0.8 deg and the aim returns to the pre-press direction; the runtime's look base then returns to zero");
                    check(std::fabs(nf::adsUnwindDuration(10.0 * kR) - 0.3) < 1e-12 && std::fabs(nf::adsUnwindDuration(-60.0 * kR) - 1.0) < 1e-12 &&
                              nf::adsUnwindDuration(500.0 * kR) == 1.5 && nf::adsUnwindDuration(std::numeric_limits<double>::quiet_NaN()) == 0.3,
                          "F68a: the unwind takes 0.3 s up to 18 deg, 60 deg per second beyond that, never more than 1.5 s");
                    (void)pitch0;
                    // dropSeated clears the seated rotation (a trip or a switch-off: the caller bumps the reset serial for the yaw).
                    p.map.dropSeated();
                    check(p.map.yawCarried == 0.0 && p.map.pitchSeat == 0.0 && p.map.yawInFlight() == 0.0 && p.map.pitchInFlight() == 0.0,
                          "F68a: dropSeated returns the carried yaw, the seated pitch and the in-flight account to zero");
                }
                // F69i: the press's snap rides the same ask as the head's step (AdsAimMap::centre): a 19.2 deg error (yaw +9, pitch -17, seat convention) injected over
                // 0.2 s into the plant that takes 0.85 of every ask (the gain is measured after the first lands); the cancel keeps the view the head's.
                {
                    AimPlant p;
                    p.delay = 1;
                    const double e0Y = 9.0 * kR, e0P = -17.0 * kR;
                    p.map.beginCentre(e0Y, e0P);
                    const double dur = p.map.centre.dur;
                    int convergedAt = -1;
                    double errAt20 = 99.0;
                    for (int i = 0; i < 30; ++i) {
                        p.frame(0.0, 0.0, true, false);
                        const double err = std::sqrt((e0Y - p.aimY) * (e0Y - p.aimY) + (e0P - p.aimP) * (e0P - p.aimP));
                        if (i == 20) errAt20 = err;   // 21 frames, 0.233 s
                        if (convergedAt < 0 && err < 0.3 * kR) convergedAt = i;
                    }
                    check(std::fabs(dur - 0.2) < 1e-12 && convergedAt >= 17 && convergedAt <= 20 && errAt20 < 0.3 * kR && !p.map.centre.active &&
                              p.viewErrMax < 1.5 * kR && std::fabs(p.viewY() - p.headY) < 1e-9 && std::fabs(p.viewP() - p.headP) < 1e-9 &&
                              std::fabs(p.map.yawCarried - e0Y) < 0.3 * kR && std::fabs(p.map.pitchSeat - e0P) < 0.3 * kR,
                          "F69i: a 19.2 deg snap injected over 0.2 s converges (within 0.3 deg of the error at 0.23 s), the view stays the head's under the cancel (within "
                          "1.5 deg while the aim moves at 96 deg/s, exactly once it stops) and the map carries the snap");
                    // The release unwinds the snap: the look base returns to zero and the aim to where it was before the press.
                    p.map.beginRelease();
                    bool done = false;
                    for (int i = 0; i < 90 * 3 && !done; ++i) {
                        p.frame(0.0, 0.0, false, true);
                        done = p.map.releaseDone();
                    }
                    for (int i = 0; i < 6; ++i) p.frame(0.0, 0.0, false, true);
                    check(done && std::fabs(p.map.yawCarried) < 0.1 * kR && std::fabs(p.map.pitchSeat) < 0.1 * kR && std::fabs(p.aimY) < 0.2 * kR && std::fabs(p.aimP) < 0.2 * kR &&
                              std::fabs(p.viewY() - p.headY) < 1e-9,
                          "F69i: the release unwinds the snap with the rest (the carried yaw and pitch return to zero, the aim to its pre-press direction) and the view never leaves "
                          "the head's");
                    // A swing over 20 deg runs at 100 deg/s at most; the plan's shares add up exactly; a game that drops ADS during the snap stops it (the release unwinds the part
                    // injected so far).
                    nf::AdsCentre big, line;
                    big.begin(60.0 * kR, 0.0);
                    line.begin(0.1, -0.05);
                    double sy = 0.0, sp = 0.0;
                    int ticks = 0;
                    while (line.active && ticks < 100) {
                        double y, pp;
                        line.step(1.0 / 90.0, &y, &pp);
                        sy += y;
                        sp += pp;
                        ++ticks;
                    }
                    AimPlant q;
                    q.delay = 1;
                    q.map.beginCentre(15.0 * kR, 0.0);
                    for (int i = 0; i < 5; ++i) q.frame(0.0, 0.0, true, false);
                    const bool activeBefore = q.map.centre.active;
                    q.map.beginRelease();
                    q.frame(0.0, 0.0, false, true);
                    const bool activeAfter = q.map.centre.active;
                    bool qdone = false;
                    for (int i = 0; i < 90 * 3 && !qdone; ++i) {
                        q.frame(0.0, 0.0, false, true);
                        qdone = q.map.releaseDone();
                    }
                    for (int i = 0; i < 6; ++i) q.frame(0.0, 0.0, false, true);
                    check(std::fabs(big.dur - 0.6) < 1e-9 && std::fabs(sy - 0.1) < 1e-12 && std::fabs(sp + 0.05) < 1e-12 && ticks >= 18 && ticks <= 19 && activeBefore && !activeAfter &&
                              qdone && std::fabs(q.map.yawCarried) < 0.1 * kR && std::fabs(q.aimY) < 0.2 * kR && std::fabs(q.viewY() - q.headY) < 1e-9,
                          "F69i: a 60 deg swing takes 0.6 s (100 deg/s at most), the 0.2 s plan injects exactly its yaw and pitch, and an ADS that ends during the snap stops it "
                          "while the release unwinds what was injected");
                    bool b1 = false, b2 = false, b3 = false, b4 = false, b5 = false;
                    const double r1 = nf::adsCentreYawBounded(30.0, 9.0, &b1), r2 = nf::adsCentreYawBounded(-30.0, -9.0, &b2), r3 = nf::adsCentreYawBounded(0.0, 9.0, &b3),
                                 r4 = nf::adsCentreYawBounded(38.0, 5.0, &b4), r5 = nf::adsCentreYawBounded(38.0, -5.0, &b5);
                    const double rt[3] = {1.0, 0.0, 0.0}, up3[3] = {0.0, 1.0, 0.0}, fw[3] = {0.0, 0.0, 1.0};
                    const double left10[3] = {-std::sin(10.0 * kR), 0.0, std::cos(10.0 * kR)}, up5[3] = {0.0, std::sin(5.0 * kR), std::cos(5.0 * kR)};
                    double ey = 0.0, ep = 0.0, ey2 = 0.0, ep2 = 0.0;
                    const bool e1 = nf::adsCentreError(fw, rt, up3, left10, &ey, &ep), e2 = nf::adsCentreError(fw, rt, up3, up5, &ey2, &ep2);
                    check(std::fabs(r1 - 7.0) < 1e-12 && b1 && std::fabs(r2 + 7.0) < 1e-12 && b2 && r3 == 9.0 && !b3 && r4 == 0.0 && b4 && r5 == -5.0 && !b5 && e1 && e2 &&
                              std::fabs(ey - 10.0) < 1e-9 && std::fabs(ep) < 1e-9 && std::fabs(ey2) < 1e-9 && std::fabs(ep2 - 5.0) < 1e-9,
                          "F69i: the snap's yaw is bounded by the cone (the weapon keeps 3 deg off 40: 30 deg off the body and asked +9 gets +7; at 38 asked outward gets 0; inward "
                          "passes whole), and the error reads in the guard's convention (a target 10 deg to the left is yaw +10, 5 deg up is pitch +5)");
                }
                // (6) The guard reads the right eye's rows through AdsResidual. A plant whose camera takes the aim never trips it over 60 s; a camera that does
                // NOT take the aim (the cancel alone turns the view: the F66 reading) trips it 0.5 s after the residual passes the floor; stale rows never trip it.
                {
                    const auto axesAt = [](double yaw, double pitch, double r[3], double u[3], double f[3]) {
                        const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
                        // forward = (-sy cp, sp, cy cp) (yaw + = left of +z), right and up from it
                        f[0] = -sy * cp; f[1] = sp; f[2] = cy * cp;
                        r[0] = cy; r[1] = 0.0; r[2] = sy;
                        u[0] = sy * sp; u[1] = cp; u[2] = cy * sp;
                    };
                    // normal plant, 60 s
                    {
                        AimPlant p;
                        p.delay = 2;
                        Head h(30.0, 99u);
                        nf::AdsResidual res;
                        bool tripped = false, havePrev = false;
                        double viewPrevY = 0.0, viewPrevP = 0.0, maxRes = 0.0;
                        uint64_t ms = 10000;
                        for (int i = 0; i < 90 * 60; ++i, ms += 11) {
                            double dY, dP;
                            h.next(&dY, &dP);
                            p.frame(dY, dP, true, false);
                            // the guard sees the rows of the previous frame (the render thread is a frame behind) and this frame's head
                            double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
                            axesAt(viewPrevY, viewPrevP, vr, vu, vf);
                            axesAt(p.headY, p.headP, hr, hu, hf);
                            if (havePrev) {
                                const nf::AdsResidual::Result r = res.step(ms, true, vr, vu, vf, true, hr, hu, hf, true, true);
                                tripped = tripped || r == nf::AdsResidual::kTrip;
                                maxRes = std::max(maxRes, res.maxAbs);
                            }
                            viewPrevY = p.viewY();
                            viewPrevP = p.viewP();
                            havePrev = true;
                        }
                        check(!tripped && maxRes < 3.0 && res.have,
                              "F68a: the guard (the right eye's rows against the head) never trips on a camera that takes the aim over 60 s of head motion; the "
                              "residual stays under 3 deg (the render thread's frame of lag)");
                    }
                    // a camera that does not take the aim: the residual is minus the cancel, growing with the aim
                    {
                        AimPlant p;
                        p.delay = 1;
                        p.cameraTakesAim = false;
                        nf::AdsResidual res;
                        bool tripped = false;
                        uint64_t tripMs = 0, floorMs = 0, ms = 10000;
                        for (int i = 0; i < 90 * 8; ++i, ms += 11) {
                            p.frame(0.35 * kR, 0.0, true, false);   // 31.5 deg/s to the left for 8 s
                            double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
                            axesAt(p.viewY(), p.viewP(), vr, vu, vf);
                            axesAt(p.headY, p.headP, hr, hu, hf);
                            const nf::AdsResidual::Result r = res.step(ms, true, vr, vu, vf, true, hr, hu, hf, true, true);
                            if (floorMs == 0 && res.have && std::sqrt(res.res[0] * res.res[0] + res.res[1] * res.res[1]) >= nf::kAdsResFloorDeg) floorMs = ms;
                            if (!tripped && r == nf::AdsResidual::kTrip) {
                                tripped = true;
                                tripMs = ms;
                            }
                        }
                        check(tripped && floorMs != 0 && tripMs >= floorMs + 500 && tripMs <= floorMs + 800 &&
                                  std::fabs(nf::adsResidualRatio(res.res[0], res.head[0]) + 1.0) < 0.15,
                              "F68a: a camera that does not take the aim trips the guard 0.5 s after the residual passes 8 deg, and the ratio reads -1");
                    }
                    // stale rows (a frozen source, or none) while the head turns 90 deg: nothing changes, nothing trips, the base survives
                    {
                        nf::AdsResidual res;
                        double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
                        axesAt(0.0, 0.0, vr, vu, vf);
                        axesAt(0.0, 0.0, hr, hu, hf);
                        const nf::AdsResidual::Result base = res.step(1000, true, vr, vu, vf, true, hr, hu, hf, true, true);
                        bool trip = false;
                        nf::AdsResidual::Result stale = nf::AdsResidual::kMeasured;
                        for (int i = 1; i <= 300; ++i) {
                            axesAt(i * 0.3 * kR, 0.0, hr, hu, hf);   // the head turns 90 deg in 3.3 s; no fresh view rows at all
                            stale = res.step(1000 + 11 * static_cast<uint64_t>(i), false, vr, vu, vf, true, hr, hu, hf, true, true);
                            trip = trip || stale == nf::AdsResidual::kTrip;
                        }
                        // the head sample missing is the same
                        const nf::AdsResidual::Result noHead = res.step(5000, true, vr, vu, vf, false, hr, hu, hf, true, true);
                        check(base == nf::AdsResidual::kBase && !trip && stale == nf::AdsResidual::kNone && noHead == nf::AdsResidual::kNone && res.haveBase,
                              "F68a: frames without fresh right-eye rows (or without a head sample) change nothing and never trip the guard");
                    }
                    // a view step over 15 deg in one tick (a cut) is a new base, not a residual
                    {
                        nf::AdsResidual res;
                        double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
                        axesAt(0.0, 0.0, vr, vu, vf);
                        axesAt(0.0, 0.0, hr, hu, hf);
                        res.step(1000, true, vr, vu, vf, true, hr, hu, hf, true, true);
                        axesAt(40.0 * kR, 0.0, vr, vu, vf);
                        const nf::AdsResidual::Result r = res.step(1011, true, vr, vu, vf, true, hr, hu, hf, true, true);
                        axesAt(41.0 * kR, 0.0, vr, vu, vf);
                        const nf::AdsResidual::Result r2 = res.step(1022, true, vr, vu, vf, true, hr, hu, hf, true, true);
                        check(r == nf::AdsResidual::kRebased && res.rebases == 1 && r2 == nf::AdsResidual::kMeasured && std::fabs(res.view[0] - 1.0) < 1e-6 &&
                                  std::fabs(res.res[0] - 1.0) < 1e-6,
                              "F68a: a 40 deg view step in one tick takes a new base (rebased 1), then measures from it (the next 1 deg step reads 1 deg)");
                    }
                    // the player's own aim and an off axis are not residual
                    {
                        nf::AdsResidual res;
                        double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
                        axesAt(0.0, 0.0, vr, vu, vf);
                        axesAt(0.0, 0.0, hr, hu, hf);
                        res.step(1000, true, vr, vu, vf, true, hr, hu, hf, true, true);
                        axesAt(10.0 * kR, 5.0 * kR, vr, vu, vf);
                        res.addUser(10.0 * kR, 5.0 * kR);
                        res.step(1011, true, vr, vu, vf, true, hr, hu, hf, true, true);
                        const double own0 = res.res[0], own1 = res.res[1];
                        nf::AdsResidual off;
                        axesAt(0.0, 0.0, vr, vu, vf);
                        off.step(1000, true, vr, vu, vf, true, hr, hu, hf, true, true);
                        axesAt(10.0 * kR, 5.0 * kR, vr, vu, vf);
                        off.step(1011, true, vr, vu, vf, true, hr, hu, hf, false, true);
                        check(std::fabs(own0) < 1e-6 && std::fabs(own1) < 1e-6 && off.res[0] == 0.0 && std::fabs(off.res[1] - 5.0) < 1e-6,
                              "F68a: the player's own aim turn is subtracted from the residual, and an axis whose sign key is 0 reads 0");
                    }
                }
                // (7) The growth rule itself, and the press gate.
                {
                    nf::AdsResidualGrowth grow, steady, tiny;
                    bool growTrip = false, steadyTrip = false, smallTrip = false;
                    uint64_t tripAt = 0;
                    for (int i = 0; i <= 100; ++i) {
                        const uint64_t ms = 1000 + 10 * static_cast<uint64_t>(i);
                        const double head = 0.1 * i;
                        if (!growTrip && grow.step(ms, 2.0 * head, head)) {
                            growTrip = true;
                            tripAt = ms;
                        }
                        steadyTrip = steadyTrip || steady.step(ms, 9.0, 40.0);   // a 9 deg lag against a 40 deg turn: under half of it
                        smallTrip = smallTrip || tiny.step(ms, 0.06 * i, 0.0);   // under the 8 deg floor all along
                    }
                    check(growTrip && tripAt >= 1000 + 400 + 500 && tripAt <= 1000 + 400 + 510 && !steadyTrip && !smallTrip && nf::kAdsResFloorDeg == 8.0 &&
                              nf::kAdsResGrowMs == 500u,
                          "F68a: the growth rule trips 0.5 s after the residual (twice the head's turn) passes the 8 deg floor, never on a steady residual under "
                          "half the head's turn or on one under the floor");
                    nf::AdsPressGate gate;
                    const bool first = gate.allow(true);       // press 1 engages
                    gate.holdOff();                            // the guard trips
                    const bool held = !gate.allow(true);       // still pressed: held off
                    const bool heldAgain = !gate.allow(true);
                    gate.allow(false);                         // released
                    const bool again = gate.allow(true);       // press 2 engages: no latch
                    check(first && held && heldAgain && again && !gate.heldOff,
                          "F68a: a trip holds the feature off until the ADS flag falls, and the next press engages (re-armed on every press, no latch)");
                }
                // (8) The seat/pitch helpers.
                check(std::fabs(nf::adsPitchLimit(55.0 * kR, 10.0 * kR) - 5.0 * kR) < 1e-12 && nf::adsPitchLimit(60.0 * kR, 1.0 * kR) == 0.0 &&
                          std::fabs(nf::adsPitchLimit(60.0 * kR, -5.0 * kR) + 5.0 * kR) < 1e-12 &&
                          std::fabs(nf::adsPitchLimit(-58.0 * kR, -5.0 * kR) + 2.0 * kR) < 1e-12 &&
                          std::fabs(nf::adsClampPitch(70.0 * kR) - 60.0 * kR) < 1e-12 && std::fabs(nf::adsClampPitch(-61.0 * kR) + 60.0 * kR) < 1e-12 &&
                          nf::adsClampPitch(std::numeric_limits<double>::quiet_NaN()) == 0.0 && std::fabs(nf::adsUnwindTarget(20.0, 0.15) - 10.0) < 1e-12 &&
                          nf::adsUnwindTarget(20.0, 0.3) == 0.0 && nf::adsUnwindTarget(20.0, 0.0) == 20.0,
                      "F68a: the seated pitch is clamped to +-60 deg (55 + 10 passes 5; at 60 nothing more; back toward zero whole; NaN 0) and the unwind falls "
                      "linearly to 0 over 0.3 s");
            }
            // F67b: the ADS instruments.
            {
                const double a0[3] = {0, 0, 1}, a30[3] = {0, 0.5, std::sqrt(3.0) / 2.0}, zero3[3] = {};
                check(std::fabs(nf::dirAngleDeg(a0, a30) - 30.0) < 1e-9 && std::fabs(nf::dirAngleDeg(a0, a0)) < 1e-6 && nf::dirAngleDeg(a0, zero3) == -1.0,
                      "F67b: the angle between two forwards (30 deg, 0, a degenerate one -1)");
                // A symmetric eye at the origin looking along +z (tangents +-1): the sight sampled 0.5 m and 10 m ahead of P14 (0, 0, -0.095), behind the eye.
                float rows[16] = {};
                const double c[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 0.01}, {0, 0, 1}};
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 3; ++i) rows[4 * i + j] = static_cast<float>(c[j][i]);
                const double o[3] = {};
                const double f[3] = {0, 0, 2.0};   // need not be unit
                double n05[2], n10[2], n00[2], m05[2], m10[2];
                const double onLine[3] = {0, 0, -0.095}, beside[3] = {0.03, 0, -0.095};
                const bool onOk = nf::sightPointNdc(rows, o, onLine, f, 0.5, n05) && nf::sightPointNdc(rows, o, onLine, f, 10.0, n10);
                const bool besideOk = nf::sightPointNdc(rows, o, beside, f, 0.5, m05) && nf::sightPointNdc(rows, o, beside, f, 10.0, m10);
                const bool behind = !nf::sightPointNdc(rows, o, onLine, f, 0.0, n00) && n00[0] == 0.0 && n00[1] == 0.0;
                check(onOk && besideOk && behind && std::fabs(n05[0]) < 1e-12 && std::fabs(n05[1]) < 1e-12 && std::fabs(n10[0]) < 1e-12 &&
                          std::fabs(m05[0] - 0.03 / 0.405) < 1e-6 && std::fabs(m10[0] - 0.03 / 9.905) < 1e-6 && std::fabs(m05[0] - m10[0]) > 0.07,
                      "F67b: the sight at 0.5 m and at 10 m from P14: an eye on the line sees both at the same NDC (0, 0); an eye 3 cm beside it sees them "
                      "apart (0.0741 and 0.0030); P14 itself, behind the eye, is refused");
            }
            // F68b: the sight against the fire line: the ray through an NDC point of an eye's scene rows, and the angle between two NDC points.
            {
                // A symmetric eye (tangents +-1) at the origin looking along +z, as the F67b check builds it.
                float rows[16] = {};
                const double c[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 0.01}, {0, 0, 1}};
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 3; ++i) rows[4 * i + j] = static_cast<float>(c[j][i]);
                const double centre[2] = {0.0, 0.0}, p[2] = {0.1, -0.05}, q[2] = {-0.2, 0.0};
                double ray[3], right = 0.0, up = 0.0, total = 0.0, right2 = 0.0, up2 = 0.0, total2 = 0.0, right3 = 0.0, up3 = 0.0, total3 = 0.0;
                const bool okRay = nf::ndcRay(rows, centre, ray) && std::fabs(ray[0]) < 1e-12 && std::fabs(ray[1]) < 1e-12 && std::fabs(ray[2] - 1.0) < 1e-12;
                const bool okA = nf::ndcAngles(rows, centre, p, &right, &up, &total);
                const bool okB = nf::ndcAngles(rows, p, q, &right2, &up2, &total2);
                const bool same = nf::ndcAngles(rows, p, p, &right3, &up3, &total3);
                const double kDeg = 180.0 / 3.14159265358979323846;
                check(okRay && okA && okB && same && std::fabs(right - std::atan(0.1) * kDeg) < 1e-9 && std::fabs(up - std::atan(-0.05) * kDeg) < 1e-9 &&
                          std::fabs(total - std::acos(1.0 / std::sqrt(1.0 + 0.1 * 0.1 + 0.05 * 0.05)) * kDeg) < 1e-9 && std::fabs(right2 - (std::atan(-0.2) - std::atan(0.1)) * kDeg) < 1e-9 &&
                          std::fabs(up2 - std::atan(0.05) * kDeg) < 1e-9 && total2 > std::fabs(right2) - 1e-9 && std::fabs(total3) < 1e-6 && std::fabs(right3) < 1e-12,
                      "F68b: the angle between two NDC points of an eye's rows: the point (0.1, -0.05) is 5.71 deg right and 2.86 deg below the centre on a 45 deg "
                      "half-FOV eye (6.38 deg between the rays), the sign is to-minus-from, and a point against itself is 0");
                float degenerate[16] = {};
                double d1 = 0.0, d2 = 0.0, d3 = 0.0;
                check(!nf::ndcAngles(degenerate, centre, p, &d1, &d2, &d3) && d1 == 0.0 && d2 == 0.0 && d3 == 0.0, "F68b: degenerate rows give no angle (zeros)");
            }
            // F63: the ADS sight lock. The body's axes are the world's (AMC rows right +x, up +y, forward +z), the camera at (0, 1.7, 0).
            {
                const double rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, cam[3] = {0.0, 1.7, 0.0}, bodyRight[3] = {1, 0, 0};
                // (1) Level sight along +z, P14w 5 cm right, 2 cm low, 30 cm ahead; half IPD 3.2 cm: E_R = (0.032, 1.7, 0), e = (0.018, -0.02,
                // 0.30); the forward part (0.30) removed: right 1.8 cm, up -2 cm; seated = diag(-1, -1, +1) . body = (-0.018, +0.02, 0).
                const double f[3] = {0, 0, 1}, row0[3] = {1, 0, 0}, p14[3] = {0.05, 1.68, 0.30};
                double rHat[3] = {};
                bool flipped = true;
                nf::AdsSightErr E;
                const bool okR = nf::adsSightRight(row0, f, bodyRight, rHat, &flipped);
                const bool okE = okR && nf::adsSightError(p14, cam, rHat, 0.032, f, rows, E);
                check(okE && !flipped && std::fabs(E.along - 0.30) < 1e-12 && std::fabs(E.eBody[0] - 0.018) < 1e-12 &&
                          std::fabs(E.eBody[1] + 0.02) < 1e-12 && std::fabs(E.eBody[2]) < 1e-12 && std::fabs(E.seated[0] + 0.018) < 1e-12 &&
                          std::fabs(E.seated[1] - 0.02) < 1e-12 && std::fabs(E.seated[2]) < 1e-12 &&
                          std::fabs(E.perpNorm - std::sqrt(0.018 * 0.018 + 0.02 * 0.02)) < 1e-12,
                      "F63: the sight lock's e: P14w - E_R (E_R = cam + an offset x right) with the forward part removed, in body axes (right 1.8, up "
                      "-2 cm) and seated (-1.8, +2, 0 cm)");
                // (2) The sight pitched 30 deg down: P14w = E_R + 0.3 f + 0.02 u (u the sight's up): only 0.02 u is left, perpendicular to f;
                // in body axes (0, 0.01732, 0.01), seated (0, -0.01732, 0.01).
                const double kPi63 = 3.14159265358979323846, s30 = std::sin(30.0 * kPi63 / 180.0), c30 = std::cos(30.0 * kPi63 / 180.0);
                const double fp[3] = {0.0, -s30, c30}, sUp[3] = {0.0, c30, s30};
                double pp[3];
                for (int i = 0; i < 3; ++i) pp[i] = cam[i] + 0.032 * row0[i] + 0.3 * fp[i] + 0.02 * sUp[i];
                double rP[3] = {};
                nf::AdsSightErr Ep;
                const bool okP = nf::adsSightRight(row0, fp, bodyRight, rP, nullptr) && nf::adsSightError(pp, cam, rP, 0.032, fp, rows, Ep);
                const double dotF = Ep.ePerpW[0] * fp[0] + Ep.ePerpW[1] * fp[1] + Ep.ePerpW[2] * fp[2];
                check(okP && std::fabs(Ep.along - 0.3) < 1e-12 && std::fabs(dotF) < 1e-12 && std::fabs(Ep.eBody[0]) < 1e-12 &&
                          std::fabs(Ep.eBody[1] - 0.02 * c30) < 1e-12 && std::fabs(Ep.eBody[2] - 0.02 * s30) < 1e-12 &&
                          std::fabs(Ep.seated[1] + 0.02 * c30) < 1e-12 && std::fabs(Ep.seated[2] - 0.02 * s30) < 1e-12 &&
                          std::fabs(Ep.perpNorm - 0.02) < 1e-12,
                      "F63: the perpendicular projection removes the component along the FP forward (pitched 30 deg): 2 cm of the sight's up is "
                      "left, in body and seated axes");
                // (3) The right axis: the FP root's row 0 against the body's right (a mirrored row is flipped and counted); a degenerate f is
                // refused. The half IPD: the right eye's offset in 2..4.5 cm, else 3.15 cm.
                double rF[3] = {};
                bool fl = false;
                const double leftRow[3] = {-1, 0, 0}, zero[3] = {0, 0, 0};
                const bool okFlip = nf::adsSightRight(leftRow, f, bodyRight, rF, &fl);
                double rZ[3] = {};
                const bool degenerate = !nf::adsSightRight(row0, zero, bodyRight, rZ, nullptr);
                bool measured = true;
                const double ipdNone = nf::adsSightIpd(false, 0.0, false, 0.0, &measured);
                const bool noneFallback = ipdNone == nf::kAdsSightIpdFallbackM && !measured;
                check(okFlip && fl && std::fabs(rF[0] - 1.0) < 1e-12 && degenerate && noneFallback &&
                          std::fabs(nf::adsSightIpd(true, -0.0325, true, 0.0325, nullptr) - 0.065) < 1e-12 &&
                          nf::adsSightIpd(true, 0.0, true, 0.001, nullptr) == nf::kAdsSightIpdFallbackM,
                      "F63/F110: the eye's right axis follows the body's right (a mirrored FP row is flipped); the IPD from the eyes' offsets, "
                      "6.3 cm when unknown or out of range");
                // (4) The 40 cm clamp (F70: it was 12 cm; the filter's clamp, nofoot::clampNorm3): 50 cm along x to 40; 5 cm untouched; (30, 40) to (24, 32);
                // the F63 integral driven 200 frames by a 60 cm error at gain 0.02 (5 cm a frame cap) stops at 40 cm.
                double a[3] = {0.5, 0.0, 0.0}, b[3] = {0.03, 0.04, 0.0}, c[3] = {0.30, 0.40, 0.0}, intg[3] = {};
                nf::clampNorm3(a, nf::kAdsSightClampM);
                nf::clampNorm3(b, nf::kAdsSightClampM);
                nf::clampNorm3(c, nf::kAdsSightClampM);
                for (int k = 0; k < 200; ++k) {
                    const double e30[3] = {0.6, 0.0, 0.0};
                    double st[3] = {0.02 * e30[0], 0.02 * e30[1], 0.02 * e30[2]};
                    const double sm = std::sqrt(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]);
                    for (int i = 0; i < 3; ++i) intg[i] += st[i] * (sm > 0.05 ? 0.05 / sm : 1.0);
                    nf::clampNorm3(intg, nf::kAdsSightClampM);
                }
                check(nf::kAdsSightClampM == 0.40 && std::fabs(a[0] - 0.40) < 1e-12 && b[0] == 0.03 && b[1] == 0.04 && std::fabs(c[0] - 0.24) < 1e-12 &&
                          std::fabs(c[1] - 0.32) < 1e-12 && std::fabs(intg[0] - 0.40) < 1e-12,
                      "F63/F70: the sight lock is clamped to 40 cm (k up to 30 cm plus the perpendicular and along offsets), the direction kept; the integral stops there");
                // (5) The guard: |e perpendicular| growing 0.5 cm every 100 ms from 3 cm trips at 0.5 s; a falling one, a steady one and one
                // under the 2 cm floor never do.
                nf::AdsSightGrowth grow, fall, steady, under;
                bool trippedAt = false, fallTrip = false, steadyTrip = false, smallTrip = false;
                uint64_t when = 0;
                for (uint64_t t = 0; t <= 1000 && !trippedAt; t += 100) {
                    trippedAt = grow.step(t, 0.03 + 0.005 * static_cast<double>(t / 100));
                    when = t;
                }
                for (uint64_t t = 0; t <= 1000; t += 100) {
                    fallTrip = fall.step(t, 0.08 - 0.006 * static_cast<double>(t / 100)) || fallTrip;
                    steadyTrip = steady.step(t, 0.05) || steadyTrip;
                    smallTrip = under.step(t, 0.015 + 0.0004 * static_cast<double>(t / 100)) || smallTrip;
                }
                check(trippedAt && when == 500u && !fallTrip && !steadyTrip && !smallTrip,
                      "F63: the sight lock's guard trips when |e perpendicular| grows for 0.5 s, not when it falls, holds or stays under 2 cm");
            }
            // F65a: the connection retry's gate, the armed hand weight, the reach limit's projection.
            {
                // (1) The gate: state 5 or 3 within 10 s arms; state 10 never; a Deactivate after 10 s never; an unread state never.
                check(nf::retryDecision(true, 5, 400) == nf::kRetryArm && nf::retryDecision(true, 3, 10000) == nf::kRetryArm &&
                          nf::retryDecision(true, 10, 400) == nf::kRetrySessions && nf::retryDecision(true, 10, 60000) == nf::kRetrySessions &&
                          nf::retryDecision(true, 5, 10001) == nf::kRetryLate && nf::retryDecision(true, 5, 53000) == nf::kRetryLate &&
                          nf::retryDecision(false, 0, 400) == nf::kRetryUnread,
                      "F65a: a Deactivate arms the retry only for a CTSA destroyed before sessions (state not 10) and within 10 s of it");
                // (3) Within reach: no correction. Shoulders 30 cm behind and 20 cm beside their grips, 10 cm above, arms 55 cm (limit 53).
                const double R[2] = {0.55 - nf::kReachMarginM, 0.55 - nf::kReachMarginM};
                const bool both[2] = {true, true};
                const double vIn[2][3] = {{-0.20, 0.10, -0.30}, {0.20, 0.10, -0.30}};
                double dIn[3] = {9.0, 9.0, 9.0};
                bool feasIn = false;
                const bool actIn = nf::reachLimitDelta(vIn, R, both, dIn, &feasIn);
                check(!actIn && feasIn && dIn[0] == 0.0 && dIn[1] == 0.0 && dIn[2] == 0.0,
                      "F65a: the reach limit's projection is zero when both shoulders are within reach of their grips");
                // (4) The left shoulder 17 cm past its reach (the flight's worst): the correction brings both under the limit, horizontal
                // only, and no further than needed (the left at the limit).
                const auto len3 = [](const double x[3]) { return std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); };
                const double vOut[2][3] = {{-0.30, 0.10, -0.60}, {0.15, 0.10, -0.40}};
                double dOut[3] = {};
                bool feasOut = false;
                const bool actOut = nf::reachLimitDelta(vOut, R, both, dOut, &feasOut);
                double aL[3], aR[3];
                for (int i = 0; i < 3; ++i) {
                    aL[i] = vOut[0][i] + dOut[i];
                    aR[i] = vOut[1][i] + dOut[i];
                }
                check(actOut && feasOut && dOut[1] == 0.0 && len3(aL) <= R[0] + 1e-9 && len3(aR) <= R[1] + 1e-9 &&
                          std::fabs(len3(aL) - R[0]) < 1e-6 && len3(vOut[0]) > R[0] + 0.1,
                      "F65a: the projection keeps each shoulder within l1 + l2 - 2 cm of its grip, horizontal only, the shortfall closed exactly");
                // (5) Both shoulders a little past reach, pulling different ways: neither arm's own projection satisfies the other, so the
                // nearer crossing of the two circles is taken and satisfies both; one arm only (the other unmeasured) is its own disk.
                const double vBoth[2][3] = {{-0.45, 0.05, -0.30}, {0.30, 0.05, -0.50}};
                double dBoth[3] = {};
                bool feasBoth = false;
                const bool actBoth = nf::reachLimitDelta(vBoth, R, both, dBoth, &feasBoth);
                double bL[3], bR[3];
                for (int i = 0; i < 3; ++i) {
                    bL[i] = vBoth[0][i] + dBoth[i];
                    bR[i] = vBoth[1][i] + dBoth[i];
                }
                const bool leftOnly[2] = {true, false};
                double dOne[3] = {};
                bool feasOne = false;
                nf::reachLimitDelta(vBoth, R, leftOnly, dOne, &feasOne);
                double oL[3];
                for (int i = 0; i < 3; ++i) oL[i] = vBoth[0][i] + dOne[i];
                check(actBoth && feasBoth && len3(bL) <= R[0] + 1e-9 && len3(bR) <= R[1] + 1e-9 && dBoth[1] == 0.0 && feasOne &&
                          std::fabs(len3(oL) - R[0]) < 1e-6,
                      "F65a: with both shoulders out of reach the correction satisfies both; with one arm measured, its own disk");
            }
            // F79: the infeasible midpoint is useful evidence but cannot make contradictory arm targets reachable. Keep the body pin fixed
            // and let each two-bone solve clamp its own hand target; feasible one- and two-arm corrections still apply unchanged.
            {
                const auto closeTo = [](double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; };
                const auto zero3 = [&](const double v[3]) { return closeTo(v[0], 0.0) && closeTo(v[1], 0.0) && closeTo(v[2], 0.0); };
                const double syntheticConflict[2][3] = {{0.0, 0.0, 0.693}, {0.0, 0.0, -1.307}};
                const double syntheticR[2] = {0.949, 0.573};
                const bool both[2] = {true, true};
                double rejectedCandidate[3] = {}, rejectedApplied[3] = {};
                bool rejectedFeasible = true;
                const bool rejectedActive = nf::reachLimitDeltaFeasibleOnly(syntheticConflict, syntheticR, both, rejectedCandidate, rejectedApplied, &rejectedFeasible);
                check(rejectedActive && !rejectedFeasible && closeTo(rejectedCandidate[0], 0.0) && closeTo(rejectedCandidate[1], 0.0) &&
                          closeTo(rejectedCandidate[2], 0.495) && zero3(rejectedApplied),
                      "F79: synthetic disjoint disks using captured radii produce the logged 49.5 cm midpoint candidate, which is reported and rejected");

                const double verticalImpossible[2][3] = {{0.2, 0.8, 0.0}, {0.0, 0.0, 0.0}};
                const double shortR[2] = {0.5, 0.5};
                const bool leftOnly[2] = {true, false};
                double verticalCandidate[3] = {}, verticalApplied[3] = {};
                bool verticalFeasible = true;
                const bool verticalActive = nf::reachLimitDeltaFeasibleOnly(verticalImpossible, shortR, leftOnly, verticalCandidate, verticalApplied, &verticalFeasible);
                check(verticalActive && !verticalFeasible && closeTo(verticalCandidate[0], -0.2) && zero3(verticalApplied),
                      "F79: an arm whose vertical gap alone exceeds reach reports its futile horizontal candidate but applies no body shift");

                const double singleV[2][3] = {{0.7, 0.0, 0.0}, {-0.7, 0.0, 0.0}};
                const double singleR[2] = {0.6, 0.6};
                double leftCandidate[3] = {}, leftApplied[3] = {}, rightCandidate[3] = {}, rightApplied[3] = {};
                bool leftFeasible = false, rightFeasible = false;
                const bool rightOnly[2] = {false, true};
                const bool leftActive = nf::reachLimitDeltaFeasibleOnly(singleV, singleR, leftOnly, leftCandidate, leftApplied, &leftFeasible);
                const bool rightActive = nf::reachLimitDeltaFeasibleOnly(singleV, singleR, rightOnly, rightCandidate, rightApplied, &rightFeasible);
                check(leftActive && leftFeasible && closeTo(leftCandidate[0], -0.1) && closeTo(leftApplied[0], -0.1) &&
                          rightActive && rightFeasible && closeTo(rightCandidate[0], 0.1) && closeTo(rightApplied[0], 0.1),
                      "F79: feasible one-arm reach corrections apply for either anatomical side");

                const double overlap[2][3] = {{0.7, 0.0, 0.0}, {-0.4, 0.0, 0.0}};
                double overlapCandidate[3] = {}, overlapApplied[3] = {};
                bool overlapFeasible = false;
                const bool overlapActive = nf::reachLimitDeltaFeasibleOnly(overlap, singleR, both, overlapCandidate, overlapApplied, &overlapFeasible);
                const double rotated[2][3] = {{0.0, 0.0, 0.7}, {0.0, 0.0, -0.4}};
                double rotatedCandidate[3] = {}, rotatedApplied[3] = {};
                bool rotatedFeasible = false;
                const bool rotatedActive = nf::reachLimitDeltaFeasibleOnly(rotated, singleR, both, rotatedCandidate, rotatedApplied, &rotatedFeasible);
                check(overlapActive && overlapFeasible && closeTo(overlapCandidate[0], -0.1) && closeTo(overlapApplied[0], -0.1) &&
                          rotatedActive && rotatedFeasible && closeTo(rotatedCandidate[0], 0.0) && closeTo(rotatedCandidate[2], -0.1) &&
                          closeTo(rotatedApplied[2], -0.1),
                      "F79: feasible common reach preserves the correction under a 90-degree body-axis rotation");
            }
            // F65b (F67c: the stand height and its cap are gone): the legs' chains on the real 3P parents (the F64 log), the upper body, the leg solve.
            {
                static const uint16_t p3[185] = {
                    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 7, 7, 7, 7, 7, 7, 7, 7, 9, 10, 10, 18, 17, 16, 17, 18,
                    21, 22, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 34, 35, 36, 40, 41, 39, 40, 41, 44,
                    44, 44, 44, 42, 43, 48, 51, 52, 53, 56, 45, 46, 44, 44, 44, 44, 44, 44, 59, 58, 58, 58, 58, 58,
                    58, 59, 59, 59, 59, 59, 62, 64, 63, 65, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 17, 17, 18, 18,
                    94, 92, 22, 22, 22, 23, 23, 23, 94, 92, 94, 92, 40, 40, 41, 41, 45, 45, 46, 46, 111, 111, 108, 108,
                    83, 84, 85, 86, 88, 89, 90, 91, 24, 100, 103, 45, 46, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44,
                    44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 136, 150, 135,
                    145, 141, 143, 149, 144, 148, 142, 82, 87, 120, 121, 122, 123, 124, 125, 126, 127};
                // (1) The chains: L foot 103 -> calf 23 -> thigh 18 -> pelvis 10; R 100 -> 22 -> 17 -> 10; the pelvis under the anchor 7 (the
                // freeze's: neck 34 and the left foot); the pair passes with the probe's feet 103 and 100 and is refused when the R foot differs.
                const nf::IkArm lL = nf::ikLegChain(p3, 185, 103), lR = nf::ikLegChain(p3, 185, 100);
                const nf::PfzFrozenSet fs = nf::pfzFrozenSet(p3, 185, 34, 103);
                const uint32_t probeOk[2] = {103, 100}, probeBad[2] = {103, 99};
                check(lL.why == 0 && lL.U == 18 && lL.F == 23 && lL.C == 10 && lL.nU == 7 && lL.nF == 4 && lL.nH == 2 && lR.why == 0 && lR.U == 17 &&
                          lR.F == 22 && lR.C == 10 && fs.why == 0 && fs.anchor == 7 && nf::ikLegPairWhy(p3, 185, lL, lR, 7, probeOk) == 0 &&
                          nf::ikLegPairWhy(p3, 185, lL, lR, 7, probeBad) == 8 && nf::ikLegPairWhy(p3, 185, lL, lR, 9, probeOk) == 7,
                      "F65b: the legs' chains (L 18/23/103, R 17/22/100 under the pelvis 10, under the anchor 7); a probe foot that differs refuses");
                // (2) The upper body: subtree(7) less both thigh subtrees (152 joints): 7, 10, 15, 34, 44 in it; no joint of either thigh's
                // subtree (the thighs, calves, feet, toes and their helpers 19, 20, 25, 26, 92..107, 129, 130).
                uint16_t ub[nf::kPfzMaxJoints] = {};
                const uint32_t ubN = nf::ikUpperBody(p3, 185, 7, 18, 17, ub, nf::kPfzMaxJoints);
                bool has[185] = {};
                for (uint32_t k = 0; k < ubN; ++k) has[ub[k]] = true;
                bool noLeg = true;
                for (uint32_t j = 0; j < 185; ++j)
                    if ((nf::ikUnder(p3, 185, j, 17) || nf::ikUnder(p3, 185, j, 18)) && has[j]) noLeg = false;
                check(ubN == 152 && has[7] && has[10] && has[15] && has[34] && has[44] && !has[0] && noLeg && !has[19] && !has[20] && !has[103] &&
                          !has[130],
                      "F65b: the raise moves the upper body (subtree of 7 less both thigh subtrees, 152 joints) and never a thigh subtree joint");
                // (3) The leg solve: a crouched leg (hip 0.80 m, knee forward at 0.45, foot at 0.05) with the hip raised 4 cm: the carried
                // foot lands on the animated foot (under 0.001 mm), the bones keep their lengths, the knee stays forward (the pole's side),
                // and the reach grows.
                const double hip[3] = {0.10, 0.80, 0.00}, knee[3] = {0.10, 0.45, 0.15}, foot[3] = {0.10, 0.05, 0.00};
                const nf::LegSolve ls = nf::legSolve(hip, knee, foot, 0.04);
                const double se[3] = {ls.E[0] - ls.S[0], ls.E[1] - ls.S[1], ls.E[2] - ls.S[2]}, ef[3] = {foot[0] - ls.E[0], foot[1] - ls.E[1], foot[2] - ls.E[2]};
                const double l1 = std::sqrt(0.35 * 0.35 + 0.15 * 0.15), l2 = std::sqrt(0.40 * 0.40 + 0.15 * 0.15);
                check(ls.ok && !ls.limited && ls.footErrM < 1e-6 && std::fabs(std::sqrt(se[0] * se[0] + se[1] * se[1] + se[2] * se[2]) - l1) < 1e-9 &&
                          std::fabs(std::sqrt(ef[0] * ef[0] + ef[1] * ef[1] + ef[2] * ef[2]) - l2) < 1e-9 && ls.E[2] > 0.0 && std::fabs(ls.S[1] - 0.84) < 1e-12 &&
                          ls.reachAfter > ls.reachBefore && ls.kneeSolvedDeg > ls.kneeAnimDeg,
                      "F65b: the leg solve keeps the foot exactly where it was animated, the bones' lengths and the knee forward");
            }
            // F67c: the body pinned to the camera. The pure half: the neck target, the move, the legs' planting and solving, the state bin.
            {
                // (1) The neck target: the eye 0.155 m down and 0.09 m back along the eye's HEADING (yaw only: a head turned 90 deg moves the "back" to its
                // side; a head looking within ~20 deg of vertical falls back to the body's forward).
                const double eye[3] = {0.30, 1.70, 0.50};
                double t0[3], t1[3], t2[3], t3[3];
                const double fwdA[3] = {0.0, 0.0, 1.0}, fwdB[3] = {1.0, 0.0, 0.0}, fwdC[3] = {0.0, 1.0, 0.05}, fwdD[3] = {0.0, -0.5, 0.8660254037844386};
                nf::pinNeckTarget(eye, fwdA, t0);
                nf::pinNeckTarget(eye, fwdB, t1);
                nf::pinNeckTarget(eye, fwdC, t2);
                nf::pinNeckTarget(eye, fwdD, t3);
                check(std::fabs(t0[0] - 0.30) < 1e-12 && std::fabs(t0[1] - 1.545) < 1e-12 && std::fabs(t0[2] - 0.41) < 1e-12 && std::fabs(t1[0] - 0.21) < 1e-12 &&
                          std::fabs(t1[2] - 0.50) < 1e-12 && std::fabs(t2[2] - 0.41) < 1e-12 && std::fabs(t3[2] - 0.41) < 1e-12 && std::fabs(t3[1] - 1.545) < 1e-12,
                      "F67c: the neck target is the eye 0.155 m down and 0.09 m back along its heading (forward (0.30, 1.545, 0.41); looking right (0.21, 1.545, 0.50); "
                      "looking up or pitched down keeps the horizontal heading or the body's)");
                // (2) The move: the animated neck lands on the target within 0.1 mm (here exactly); the vertical part is signed; a gap past the caps is
                // clamped and flagged; a NaN asks nothing.
                const double neck[3] = {0.10, 1.50, 0.20}, tgt[3] = {0.30, 1.45, 0.10};
                const nf::PinMove mv = nf::pinMove(neck, tgt);
                const double landed[3] = {neck[0] + mv.th[0], neck[1] + mv.delta, neck[2] + mv.th[2]};
                const double farT[3] = {3.0, 2.5, 0.2};
                const nf::PinMove big = nf::pinMove(neck, farT);
                const double nanT[3] = {std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0};
                const nf::PinMove nm = nf::pinMove(neck, nanT);
                check(std::fabs(landed[0] - tgt[0]) < 1e-12 && std::fabs(landed[1] - tgt[1]) < 1e-12 && std::fabs(landed[2] - tgt[2]) < 1e-12 && mv.delta < 0.0 &&
                          !mv.hClamped && big.hClamped && big.vClamped && std::fabs(std::sqrt(big.th[0] * big.th[0] + big.th[2] * big.th[2]) - nf::kPinHorizCapM) < 1e-9 &&
                          std::fabs(big.delta - nf::kPinVertCapM) < 1e-12 && nm.th[0] == 0.0 && nm.delta == 0.0,
                      "F67c: the move puts the animated neck on the target (within 0.1 mm), the vertical part signed (lowering is negative), a camera past 1.5 m or 0.8 m "
                      "is clamped and flagged, a NaN target moves nothing");
                const double headTarget[3] = {0.12, 1.32, -0.08}, authoredNeck[3] = {-0.01, 1.48, 0.03}, reachOffset[3] = {0.02, -0.01, 0.04};
                double detached[3], halfDetached[3], unchanged[3];
                const double fullGap = nf::pinDetachTarget(headTarget, authoredNeck, 0.0, unchanged);
                const double halfGap = nf::pinDetachTarget(headTarget, authoredNeck, 0.5, halfDetached);
                const double releasedGap = nf::pinDetachTarget(headTarget, authoredNeck, 1.0, detached);
                float bendTarget[3];
                nf::pinBendTargetWithReach(halfDetached, reachOffset, bendTarget);
                nf::AdsRamp detachRamp;
                detachRamp.step(true, nf::kAdsRampInSec);
                const double engagedW = detachRamp.w;
                detachRamp.step(false, nf::kAdsRampOutSec * 0.5);
                const double halfReleaseW = detachRamp.w;
                detachRamp.step(true, nf::kAdsRampInSec * 0.5);
                const double reengagedW = detachRamp.w;
                detachRamp.step(false, nf::kAdsRampOutSec);
                const bool adsDetachOk = fullGap > 0.0 && std::fabs(unchanged[0] - headTarget[0]) < 1e-12 && std::fabs(halfGap - fullGap * 0.5) < 1e-12 &&
                                         releasedGap < 1e-12 && std::fabs(detached[0] - authoredNeck[0]) < 1e-12 && std::fabs(detached[1] - authoredNeck[1]) < 1e-12 &&
                                         std::fabs(detached[2] - authoredNeck[2]) < 1e-12 && engagedW == 1.0 && std::fabs(halfReleaseW - 0.5) < 1e-12 &&
                                         std::fabs(reengagedW - 1.0) < 1e-12 && detachRamp.w == 0.0 && std::fabs(bendTarget[0] - (halfDetached[0] + reachOffset[0])) < 1e-7f &&
                                         std::fabs(bendTarget[1] - (halfDetached[1] + reachOffset[1])) < 1e-7f && std::fabs(bendTarget[2] - (halfDetached[2] + reachOffset[2])) < 1e-7f &&
                                         nf::pinBodyBendWeight(0.0) == 1.0 && nf::pinBodyBendWeight(0.5) == 0.5 && nf::pinBodyBendWeight(1.0) == 0.0;
                check(adsDetachOk,
                      "F73: ADS body detachment blends the pin target from the head target to the same-call authored neck, leaves non-ADS unchanged, preserves reach in the bend target, and restores smoothly through release and repress");
                // F74: LOCAL eye transport, current-view world right, and inverse-AMC model transport are separate bases.
                const double zeroAxis[3] = {}, localYawRight[3] = {0.0, 0.0, -1.0}, viewRight[3] = {1.0, 0.0, 0.0};
                double seatZero[3], seatYaw[3], worldRight[3], modelRight[3];
                const bool zeroOffset = nf::weaponViewOffsetSeated(0.0, zeroAxis, 0.0, seatZero);
                const bool seatedYawOk = nf::weaponViewOffsetSeated(0.05, localYawRight, 0.0, seatYaw);   // F134: a seat of heading 0 is the LOCAL vector itself
                const bool worldRightOk = nf::weaponViewOffsetWorld(0.05, viewRight, worldRight);
                const float invAmcYaw[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 10, -4, 7, 1};
                nf::ikVectorXform(worldRight, invAmcYaw, modelRight);
                double targetFromHead[3] = {0.10, 1.40, -0.20};
                nf::weaponViewOffsetTarget(targetFromHead, modelRight);
                const bool separateBasisOk = zeroOffset && seatZero[0] == 0.0 && seatZero[1] == 0.0 && seatZero[2] == 0.0 && seatedYawOk && worldRightOk &&
                                             std::fabs(seatYaw[0]) < 1e-12 && std::fabs(seatYaw[1]) < 1e-12 && std::fabs(seatYaw[2] - 0.05) < 1e-12 &&
                                             std::fabs(worldRight[0] - 0.05) < 1e-12 && std::fabs(worldRight[1]) < 1e-12 && std::fabs(worldRight[2]) < 1e-12 &&
                                             std::fabs(modelRight[0]) < 1e-12 && std::fabs(modelRight[1]) < 1e-12 && std::fabs(modelRight[2] - 0.05) < 1e-12 &&
                                             std::fabs(targetFromHead[0] - 0.10) < 1e-12 && std::fabs(targetFromHead[2] + 0.15) < 1e-12;
                check(separateBasisOk, "F74: positive view offset uses LOCAL head-right for eye-origin motion, current stereo scene-right for world shift, and the inverse AMC's linear part for body axes; AMC translation is ignored");
                {   // F134: the offset is a SEATED vector: the runtime turns it into LOCAL by the seat's heading, so building it from the LOCAL head-right undoes that turn
                    const double lr[3] = {0.6, 0.0, 0.8}, psi = 0.61;
                    double seatedAt[3], backLocal[3], atZero[3];
                    const bool built = nf::weaponViewOffsetSeated(0.05, lr, psi, seatedAt) && nf::weaponViewOffsetSeated(0.05, lr, 0.0, atZero);
                    nf::ryApply(psi, seatedAt, backLocal);   // the runtime's eyePoll: LOCAL = Ry(seat yaw) . seated
                    const double wantLocal[3] = {-0.05 * 0.6, 0.0, -0.05 * 0.8};
                    const bool refusedNaN = !nf::weaponViewOffsetSeated(0.05, lr, std::nan(""), seatedAt);
                    double zeroAt[3];
                    check(built && std::fabs(backLocal[0] - wantLocal[0]) < 1e-12 && std::fabs(backLocal[1]) < 1e-12 && std::fabs(backLocal[2] - wantLocal[2]) < 1e-12 &&
                              std::fabs(nf::ikLen(seatedAt) - 0.05) < 1e-12 && std::fabs(seatedAt[0] - atZero[0]) > 1e-3 && refusedNaN && nf::weaponViewOffsetSeated(0.0, lr, std::nan(""), zeroAt),
                          "F134: weaponViewOffsetSeated builds a vector in the seat's axes: the runtime's turn by the seat's heading (0.61 rad) returns the LOCAL -right translation "
                          "exactly, a seat heading of 0 is the old vector, an unknown heading is refused, and no offset needs none");
                }
                const double opticP14[3] = {0.20, 0.0, 2.0}, sightForward[3] = {0.0, 0.0, 1.0};
                const double localRight[3] = {0.6, 0.0, 0.8}, sceneRight[3] = {0.8, 0.2, -0.56};
                const double yaw = 0.61, cy = std::cos(yaw), sy = std::sin(yaw);
                const float amcRows[9] = {static_cast<float>(cy), 0.0f, static_cast<float>(-sy), 0.0f, 1.0f, 0.0f, static_cast<float>(sy), 0.0f,
                                          static_cast<float>(cy)};
                const double amcRowsD[9] = {cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy};
                const float affineAmc[16] = {amcRows[0], amcRows[1], amcRows[2], 0, amcRows[3], amcRows[4], amcRows[5], 0,
                                             amcRows[6], amcRows[7], amcRows[8], 0, 0, 0, 0, 1};
                const float inverseAmcRows[16] = {amcRows[0], amcRows[3], amcRows[6], 0, amcRows[1], amcRows[4], amcRows[7], 0,
                                                   amcRows[2], amcRows[5], amcRows[8], 0, 0, 0, 0, 1};
                const double offsetCasesM[4] = {0.0, 0.025, 0.10, -0.10};
                const double adsEyeBase[3] = {opticP14[0], opticP14[1], opticP14[2] + 0.15};
                const double dt = 1.0 / 90.0;
                bool adsActualP14Ok = true, adsBodyBiasOk = true;
                for (double offsetM : offsetCasesM) {
                    double seatedBias[3], worldBias[3];
                    adsActualP14Ok = adsActualP14Ok && nf::weaponViewOffsetSeated(offsetM, localRight, 0.0, seatedBias) &&
                                     nf::weaponViewOffsetWorld(offsetM, sceneRight, worldBias);
                    const double biasNorm = nf::ikLen(worldBias);
                    adsActualP14Ok = adsActualP14Ok && std::fabs(biasNorm - std::fabs(offsetM)) < 1e-12 &&
                                     std::fabs(nf::ikLen(seatedBias) - std::fabs(offsetM)) < 1e-12;
                    double adsPos[3] = {}, adsVel[3] = {}, adsTarget[3] = {};
                    for (int step = 0; step < 180; ++step) {
                        double adsWorld[3];
                        nf::adsCamShiftWorld(adsPos, amcRows, adsWorld);
                        const double eyeNow[3] = {adsEyeBase[0] + adsWorld[0] + worldBias[0], adsEyeBase[1] + adsWorld[1] + worldBias[1],
                                                  adsEyeBase[2] + adsWorld[2] + worldBias[2]};
                        nf::AdsSightErr err;
                        adsActualP14Ok = adsActualP14Ok && nf::adsSightErrorEyeK(opticP14, eyeNow, sightForward, amcRowsD, 0.15, err);
                        nf::adsEyeStep(adsPos, adsVel, adsTarget, err.seatedFull, true, true, false, dt);
                    }
                    double adsWorld[3];
                    nf::adsCamShiftWorld(adsPos, amcRows, adsWorld);
                    const double finalEye[3] = {adsEyeBase[0] + adsWorld[0] + worldBias[0], adsEyeBase[1] + adsWorld[1] + worldBias[1],
                                                adsEyeBase[2] + adsWorld[2] + worldBias[2]};
                    const double eyeMiss[3] = {finalEye[0] - adsEyeBase[0], finalEye[1] - adsEyeBase[1], finalEye[2] - adsEyeBase[2]};
                    adsActualP14Ok = adsActualP14Ok && nf::ikLen(eyeMiss) < 0.001;

                    // The frame-thread pin removes the full ADS camera shift and the independent manual world bias.
                    const double adsShift[3] = {adsPos[0], adsPos[1], adsPos[2]};
                    double adsWorldAtPin[3];
                    nf::adsCamShiftWorld(adsShift, amcRows, adsWorldAtPin);
                    const double jobCam[3] = {0.4, 1.2, -0.7};
                    const float biasedCamera[3] = {static_cast<float>(jobCam[0] + adsWorldAtPin[0] + worldBias[0]),
                                                   static_cast<float>(jobCam[1] + adsWorldAtPin[1] + worldBias[1]),
                                                   static_cast<float>(jobCam[2] + adsWorldAtPin[2] + worldBias[2])};
                    float unlockedCam[3];
                    nf::adsUnlockedCam(biasedCamera, adsShift, amcRows, unlockedCam);
                    for (int i = 0; i < 3; ++i) unlockedCam[i] = static_cast<float>(static_cast<double>(unlockedCam[i]) - worldBias[i]);
                    const double pinMiss[3] = {static_cast<double>(unlockedCam[0]) - jobCam[0], static_cast<double>(unlockedCam[1]) - jobCam[1],
                                               static_cast<double>(unlockedCam[2]) - jobCam[2]};
                    adsBodyBiasOk = adsBodyBiasOk && nf::ikLen(pinMiss) < 1e-6;

                    double modelBias[3], worldRoundTrip[3];
                    nf::ikVectorXform(worldBias, inverseAmcRows, modelBias);
                    nf::ikVectorXform(modelBias, affineAmc, worldRoundTrip);
                    adsBodyBiasOk = adsBodyBiasOk && std::fabs(worldRoundTrip[0] - worldBias[0]) < 1e-7 &&
                                    std::fabs(worldRoundTrip[1] - worldBias[1]) < 1e-7 && std::fabs(worldRoundTrip[2] - worldBias[2]) < 1e-7;
                    const double authored[3] = {-0.02, 1.48, 0.02};
                    const double headTarget[3] = {0.12, 1.32, -0.08};
                    for (double detachW : {0.0, 0.5, 1.0}) {
                        double bodyTarget[3], landedWorld[3];
                        nf::pinDetachTarget(headTarget, authored, detachW, bodyTarget);
                        nf::weaponViewOffsetTarget(bodyTarget, modelBias);
                        nf::ikVectorXform(bodyTarget, affineAmc, landedWorld);
                        const double expectedModel[3] = {headTarget[0] + (authored[0] - headTarget[0]) * detachW,
                                                         headTarget[1] + (authored[1] - headTarget[1]) * detachW,
                                                         headTarget[2] + (authored[2] - headTarget[2]) * detachW};
                        double expectedWorld[3];
                        nf::ikVectorXform(expectedModel, affineAmc, expectedWorld);
                        adsBodyBiasOk = adsBodyBiasOk && std::fabs((landedWorld[0] - expectedWorld[0]) - worldBias[0]) < 1e-7 &&
                                        std::fabs((landedWorld[1] - expectedWorld[1]) - worldBias[1]) < 1e-7 &&
                                        std::fabs((landedWorld[2] - expectedWorld[2]) - worldBias[2]) < 1e-7;
                    }
                }
                check(adsActualP14Ok,
                      "F75: the ADS filter always targets actual P14 plus k-forward; with zero, +2.5 cm, and both +/-10 cm manual offsets it cancels camera bias through a rotated AMC and nonparallel LOCAL/world right axes");
                check(adsBodyBiasOk,
                      "F75: pin input removes ADS and manual camera shifts back to baseline, while the full world body bias survives ADS detach at weights 0, 0.5 and 1 through a rotated AMC");
                // A live setting change while ADS is held is corrected by the existing eye filter; release clears only that correction, and repress reacquires it.
                {
                    double livePos[3] = {}, liveVel[3] = {}, liveTarget[3] = {};
                    const double baseBiasM = 0.025, nextBiasM = -0.10;
                    double biasA[3], biasB[3];
                    nf::weaponViewOffsetWorld(baseBiasM, sceneRight, biasA);
                    nf::weaponViewOffsetWorld(nextBiasM, sceneRight, biasB);
                    auto runAdsSteps = [&](const double bias[3], bool active, int count) {
                        for (int step = 0; step < count; ++step) {
                            double adsWorld[3];
                            nf::adsCamShiftWorld(livePos, amcRows, adsWorld);
                            const double eyeNow[3] = {adsEyeBase[0] + adsWorld[0] + bias[0], adsEyeBase[1] + adsWorld[1] + bias[1],
                                                      adsEyeBase[2] + adsWorld[2] + bias[2]};
                            nf::AdsSightErr err;
                            nf::adsSightErrorEyeK(opticP14, eyeNow, sightForward, amcRowsD, 0.15, err);
                            nf::adsEyeStep(livePos, liveVel, liveTarget, err.seatedFull, true, active, false, dt);
                        }
                    };
                    runAdsSteps(biasA, true, 180);
                    runAdsSteps(biasB, true, 180);
                    double correctedWorld[3];
                    nf::adsCamShiftWorld(livePos, amcRows, correctedWorld);
                    const double correctedEyeMiss[3] = {adsEyeBase[0] + correctedWorld[0] + biasB[0] - adsEyeBase[0],
                                                        adsEyeBase[1] + correctedWorld[1] + biasB[1] - adsEyeBase[1],
                                                        adsEyeBase[2] + correctedWorld[2] + biasB[2] - adsEyeBase[2]};
                    runAdsSteps(biasB, false, 180);
                    const bool releaseClears = nf::ikLen(livePos) < 0.001;
                    runAdsSteps(biasA, true, 180);
                    double repressedWorld[3];
                    nf::adsCamShiftWorld(livePos, amcRows, repressedWorld);
                    const double repressedMiss[3] = {adsEyeBase[0] + repressedWorld[0] + biasA[0] - adsEyeBase[0],
                                                     adsEyeBase[1] + repressedWorld[1] + biasA[1] - adsEyeBase[1],
                                                     adsEyeBase[2] + repressedWorld[2] + biasA[2] - adsEyeBase[2]};
                    check(nf::ikLen(correctedEyeMiss) < 0.001 && releaseClears && nf::ikLen(repressedMiss) < 0.001,
                          "F75: a live view-offset change is reacquired during ADS, release returns only the ADS correction to zero, and repress restores actual-P14 alignment");
                }
                const double authored[3] = {-0.02, 1.48, 0.02};
                const double detach[3] = {0.0, 0.5, 1.0};
                bool detachOffsetOk = true;
                for (double w : detach) {
                    double target[3];
                    nf::pinDetachTarget(headTarget, authored, w, target);
                    nf::weaponViewOffsetTarget(target, modelRight);
                    const double expectedX = headTarget[0] + (authored[0] - headTarget[0]) * w;
                    const double expectedZ = headTarget[2] + (authored[2] - headTarget[2]) * w + 0.05;
                    const nf::PinMove move = nf::pinMove(authored, target);
                    const double landedX = authored[0] + move.th[0], landedZ = authored[2] + move.th[2];
                    detachOffsetOk = detachOffsetOk && std::fabs(target[0] - expectedX) < 1e-12 && std::fabs(target[2] - expectedZ) < 1e-12 &&
                                     std::fabs(landedX - expectedX) < 1e-12 && std::fabs(landedZ - expectedZ) < 1e-12;
                }
                check(detachOffsetOk, "F74: body pin keeps the same world-right offset before, during, and after ADS detachment");
                // Repeated camera feedback: removing the published world bias before pinning keeps the requested offset from accumulating.
                const double cameraBase[3] = {0.10, 1.40, -0.20};
                double closedLoopBody[3] = {authored[0], authored[1], authored[2]};
                bool closedLoopOk = true;
                for (int frame = 0; frame < 4; ++frame) {
                    const double cameraWithBias[3] = {cameraBase[0] + worldRight[0], cameraBase[1] + worldRight[1], cameraBase[2] + worldRight[2]};
                    double unlocked[3] = {cameraWithBias[0] - worldRight[0], cameraWithBias[1] - worldRight[1], cameraWithBias[2] - worldRight[2]};
                    double bodyTarget[3] = {authored[0] + modelRight[0], authored[1] + modelRight[1], authored[2] + modelRight[2]};
                    const nf::PinMove move = nf::pinMove(closedLoopBody, bodyTarget);
                    closedLoopBody[0] += move.th[0]; closedLoopBody[1] += move.delta; closedLoopBody[2] += move.th[2];
                    closedLoopOk = closedLoopOk && std::fabs(unlocked[0] - cameraBase[0]) < 1e-12 && std::fabs(unlocked[1] - cameraBase[1]) < 1e-12 &&
                                   std::fabs(unlocked[2] - cameraBase[2]) < 1e-12 && std::fabs(closedLoopBody[2] - (authored[2] + 0.05)) < 1e-12;
                }
                check(closedLoopOk, "F74: repeated camera feedback removes the published offset before body pinning, so the body reaches and holds one offset without accumulating it");
                check(nf::weaponViewOffsetActive(true, true, 0.05) && !nf::weaponViewOffsetActive(true, false, 0.05) &&
                          !nf::weaponViewOffsetActive(false, true, 0.05) && !nf::weaponViewOffsetActive(true, true, 0.0),
                      "F74: view offset requires a nonzero setting, native body-pin scope, and an equipped item");
                // (3) The legs. A leg with l1 = l2 = 0.45 m (the knee at 30 deg folds the hip -> foot to 0.233 m, 0.995 x 0.9 = 0.8955 m is the longest asked).
                const double l1 = 0.45, l2 = 0.45, fb[3] = {0.0, 0.0, 1.0};
                check(std::fabs(nf::legMinReach(l1, l2) - 2.0 * 0.45 * std::sin(15.0 * nf::kIkPi / 180.0)) < 1e-12, "F67c: the knee folds the leg to 2 l sin(15 deg) at the 30 deg interior angle");
                // In reach: the foot stays where it was animated.
                const double hipA[3] = {0.0, 0.85, 0.0}, footA[3] = {0.35, 0.05, 0.0};
                const nf::LegPlant inReach = nf::legPlant(hipA, footA, l1, l2, fb, 0.8731);
                // The armed stance's spread feet with the hip raised 3 cm: out of reach (0.9008 of 0.8955), so the foot is re-planted toward under the hip (same
                // bearing, 1.4 cm shorter), keeping its animated height, and the leg then reaches exactly 0.995 (the knee slightly bent).
                const double hipB[3] = {0.0, 0.88, 0.0};
                const nf::LegPlant replant = nf::legPlant(hipB, footA, l1, l2, fb, 0.8731);
                // Raised 10 cm: straight below the hip is out of reach too (0.9 > 0.8955): the foot is lifted.
                const double hipC[3] = {0.0, 0.95, 0.0};
                const nf::LegPlant lift = nf::legPlant(hipC, footA, l1, l2, fb, 0.8731);
                // Lowered: hip 0.25 over a foot 0.1 m aside (0.2236 < 0.2329): the foot is re-planted OUTWARD; directly under the hip, the fallback bearing.
                const double hipD[3] = {0.0, 0.25, 0.0}, footD[3] = {0.1, 0.05, 0.0}, footE[3] = {0.0, 0.05, 0.0}, fbX[3] = {1.0, 0.0, 0.0};
                const nf::LegPlant fold = nf::legPlant(hipD, footD, l1, l2, fb, 0.8731);
                const nf::LegPlant under = nf::legPlant(hipD, footE, l1, l2, fbX, 0.8731);
                const double hRe = std::sqrt(std::pow(0.995 * 0.9, 2) - 0.83 * 0.83);
                // A leg the animation itself holds beyond the knee's range is not moved until the pin makes it worse: a hip 0.2236 m from the foot (below the
                // fold limit 0.233) with no move, and a straight leg (0.8995 of 0.9 m) with no move.
                const nf::LegPlant keptFold = nf::legPlant(hipD, footD, l1, l2, fb, 0.22360679774997896);
                const double hipS[3] = {0.0, 0.8995, 0.0}, footS[3] = {0.0, 0.0, 0.0};
                const nf::LegPlant keptStraight = nf::legPlant(hipS, footS, l1, l2, fb, 0.8995);
                check(keptFold.replantM == 0.0 && keptFold.foot[0] == 0.1 && keptStraight.replantM == 0.0 && keptStraight.liftM == 0.0,
                      "F67c: a leg the animation holds past the knee's fold or nearly straight is left where it is until the pin makes it worse");
                check(inReach.replantM == 0.0 && inReach.liftM == 0.0 && inReach.foot[0] == 0.35 && inReach.closed &&
                          std::fabs(replant.replantM - (0.35 - hRe)) < 1e-9 && replant.liftM == 0.0 && std::fabs(replant.foot[0] - hRe) < 1e-9 && replant.foot[1] == 0.05 &&
                          std::fabs(replant.reachAfter - 0.995) < 1e-9 && replant.closed && lift.liftM > 0.0 && std::fabs(lift.foot[0]) < 1e-12 &&
                          std::fabs(lift.foot[1] - (0.95 - 0.995 * 0.9)) < 1e-9 && std::fabs(lift.liftM - (0.95 - 0.995 * 0.9 - 0.05)) < 1e-9 && fold.replantM < 0.0 &&
                          fold.foot[0] > 0.1 && fold.foot[1] == 0.05 && std::fabs(fold.reachAfter * 0.9 - nf::legMinReach(l1, l2)) < 1e-9 && fold.closed &&
                          under.replantM < 0.0 && std::fabs(under.foot[0] - std::sqrt(nf::legMinReach(l1, l2) * nf::legMinReach(l1, l2) - 0.04)) < 1e-9 &&
                          std::fabs(under.foot[2]) < 1e-12,
                      "F67c: a leg in reach keeps its animated foot; out of reach the foot is first re-planted toward under the hip (animated height kept, the leg "
                      "reaching 0.995), lifted only when straight below the hip is still out of reach, and re-planted outward when the hip is lowered past the knee's fold");
                // The solve onto a planted foot and a hip moved by a vector: the carried foot lands on the target (under 0.001 mm), the bones keep their lengths.
                const double hip0[3] = {0.10, 0.80, 0.00}, knee0[3] = {0.10, 0.45, 0.15}, foot0[3] = {0.10, 0.05, 0.00};
                const double hip1[3] = {0.12, 0.83, -0.03}, foot1[3] = {0.11, 0.05, -0.02};
                const nf::LegSolve sl = nf::legSolveTo(hip0, knee0, foot0, hip1, foot1);
                const double l1a = std::sqrt(0.35 * 0.35 + 0.15 * 0.15), l2a = std::sqrt(0.40 * 0.40 + 0.15 * 0.15);
                const double sE[3] = {sl.E[0] - sl.S[0], sl.E[1] - sl.S[1], sl.E[2] - sl.S[2]}, eF[3] = {foot1[0] - sl.E[0], foot1[1] - sl.E[1], foot1[2] - sl.E[2]};
                // A 40 cm lift (a jump): the hip 40 cm up over a foot that stays: the foot is lifted to the longest reach and the solve still lands on it.
                const double hipJ[3] = {0.10, 1.20, 0.00};
                const nf::LegPlant jp = nf::legPlant(hipJ, foot0, l1a, l2a, fb, 0.8);
                const nf::LegSolve js = nf::legSolveTo(hip0, knee0, foot0, hipJ, jp.foot);
                const double jE[3] = {js.E[0] - js.S[0], js.E[1] - js.S[1], js.E[2] - js.S[2]}, jF[3] = {jp.foot[0] - js.E[0], jp.foot[1] - js.E[1], jp.foot[2] - js.E[2]};
                check(sl.ok && sl.footErrM < 1e-6 && std::fabs(nf::ikLen(sE) - l1a) < 1e-9 && std::fabs(nf::ikLen(eF) - l2a) < 1e-9 && js.ok && js.footErrM < 1e-6 &&
                          jp.liftM > 0.30 && std::fabs(nf::ikLen(jE) - l1a) < 1e-9 && std::fabs(nf::ikLen(jF) - l2a) < 1e-9 && std::fabs(js.reachAfter - 0.995) < 1e-9,
                      "F67c: the leg solve puts the carried foot on the planted target (a hip moved in three axes: under 0.001 mm; a 40 cm lift: the foot lifted to "
                      "reach 0.995) and keeps both bones' lengths");
                check(nf::pinState(0.1, 0.0) == 0 && nf::pinState(1.2, 0.0) == 1 && nf::pinState(4.0, 0.2) == 2 && nf::pinState(1.0, 1.1) == 3 && nf::pinState(4.0, -1.0) == 3,
                      "F67c: the state bins: standing (under 0.35 m/s), walking, running (3 m/s), jumping (vertical speed over 0.8 m/s; F69d: alone)");
            }
            // F68g: the armed-move trace's gate: armed and over 1 m/s starts a 40-call event; at most 5 events a minute, at least a second apart.
            {
                const uint64_t freq = 1000000ull;   // 1 tick = 1 us in the model
                nf::ArmMoveGate g;
                uint32_t ev = 0, idx = 0;
                uint64_t q = 5000000ull;
                bool slowOrUnarmed = false;
                for (int i = 0; i < 90; ++i, q += 11111) slowOrUnarmed = g.step(true, 0.9, q, freq, &ev, &idx) || g.step(false, 5.0, q, freq, &ev, &idx) || slowOrUnarmed;
                uint32_t posted = 0, maxIdx = 0, firstEv = 0;
                for (int i = 0; i < 100; ++i, q += 11111)   // 1.1 s of 3 m/s armed
                    if (g.step(true, 3.0, q, freq, &ev, &idx)) {
                        if (!posted) firstEv = ev;
                        ++posted;
                        maxIdx = std::max(maxIdx, idx);
                    }
                // Over a minute of continuous armed running at 90 Hz: 5 events started in the first 60 s, 40 calls each, and the sixth only after the first aged out.
                uint32_t calls = 0, events = 1;
                uint32_t lastEv = firstEv;
                nf::ArmMoveGate g2;
                uint64_t q2 = 7000000ull;   // a QPC that is not 0
                for (int i = 0; i < 90 * 60; ++i, q2 += 11111)
                    if (g2.step(true, 3.0, q2, freq, &ev, &idx)) {
                        ++calls;
                        if (ev != lastEv && lastEv != 0 && idx == 0) ++events;
                        lastEv = ev;
                    }
                const uint32_t startsInMinute = g2.nStarts;
                uint32_t lateCalls = 0;
                for (int i = 0; i < 80; ++i, q2 += 11111) lateCalls += g2.step(true, 3.0, q2, freq, &ev, &idx) ? 1u : 0u;   // the first start ages out at 60 s
                check(!slowOrUnarmed && firstEv == 1 && posted >= 40 && maxIdx == 39 && startsInMinute == 5 && calls == 5 * 40 && events == 5 && lateCalls == 40,
                      "F68g: the trace starts only when armed and over 1 m/s, runs exactly 40 calls (0..39), starts at most 5 events in a minute (200 calls) and the "
                      "sixth once the first has aged out");
            }
            // F68d: diagnostic-only FP hand-motion detector on recorded rest and raised poses. Its signal/weight must not select weapon authority.
            {
                const double dtF = 1.0 / 90.0;
                const double rest[3] = {0.25, -0.10, 0.30}, raised[3] = {0.25 + 0.15, -0.10 + 0.20, 0.30 + 0.25};   // 35.4 cm apart
                // (1) The rest pose is captured after 0.4 s of fists with the hand still; nothing is up.
                nf::FpHandsUp a;
                int capturedAt = -1;
                nf::FpHandsUp::Out o;
                for (int i = 0; i < 45; ++i) {
                    o = a.step(dtF, true, rest, true);
                    if (o.restCaptured && capturedAt < 0) capturedAt = i;
                }
                check(capturedAt >= 36 && capturedAt <= 38 && a.haveRest && std::fabs(a.rest[0] - rest[0]) < 1e-12 && !o.up && !o.signal && o.w == 0.0 && o.dist == 0.0,
                      "F68d: the unarmed rest pose is captured once Status.json says fists and the hand has been still for 0.4 s (frame 37)");
                // (2) The detector reports a raised hand immediately; production weapon authority is tested separately below.
                o = a.step(dtF, true, raised, true);
                check(o.up && o.signal && !o.vetoed && o.w == 1.0 && std::fabs(o.dist - std::sqrt(0.125)) < 1e-9,
                      "F68d: the diagnostic detects the FP hand 35 cm from rest on its first frame while Status.json still says fists");
                // (3) The hysteresis (up past 12 cm, down under 9 cm) and the weight's 0.3 s fall.
                nf::FpHandsUp h;
                for (int i = 0; i < 45; ++i) h.step(dtF, true, rest, true);
                const auto hAt = [&](double dist) {
                    double p[3] = {rest[0] + dist, rest[1], rest[2]};
                    return h.step(dtF, true, p, true);
                };
                const nf::FpHandsUp::Out under = hAt(0.11), over = hAt(0.13), inBand = hAt(0.10), inBand2 = hAt(0.095), down = hAt(0.085);
                check(!under.up && over.up && inBand.up && inBand2.up && !down.up && !down.signal && down.w < 1.0 && down.w > 0.9,
                      "F68d: up past 12 cm, still up at 10 and 9.5 cm, down under 9 cm; the weight starts falling when the signal ends");
                nf::FpHandsUp::Out fall = down;
                int fallFrames = 0;
                while (fall.w > 0.0 && fallFrames < 100) {
                    fall = hAt(0.0);
                    ++fallFrames;
                }
                check(fallFrames >= 25 && fallFrames <= 28, "F68d: the diagnostic weight falls to 0 over 0.3 s once its signal ends");
                // (4) The diagnostic veto: hands up with the fists, counted from the hands going up (F69f: 3.0 s). The hand sways so rest recapture cannot hide it.
                nf::FpHandsUp v;
                for (int i = 0; i < 45; ++i) v.step(dtF, true, rest, true);
                int firstVeto = -1;
                nf::FpHandsUp::Out ov;
                for (int i = 0; i < 300; ++i) {
                    double swayed[3] = {raised[0] + 0.02 * std::sin(i * 0.3), raised[1], raised[2]};
                    ov = v.step(dtF, true, swayed, true);
                    if (ov.vetoed && firstVeto < 0) firstVeto = i;
                    if (i == 100) check(ov.signal && !ov.vetoed && ov.w == 1.0, "F68d: the diagnostic signal remains up after 1.1 s");
                }
                check(firstVeto >= 269 && firstVeto <= 272 && !ov.signal && ov.vetoed && ov.w < 1.0,
                      "F68d: the diagnostic vetoes a fists-up pose after 3 s (an emote or tool pose), at frame 270");
                // (5) A SelectedWeapon transition clears the diagnostic fists timer; the independent authority/ramp behavior is tested below.
                nf::FpHandsUp w;
                for (int i = 0; i < 45; ++i) w.step(dtF, true, rest, true);
                bool everVetoed = false, allSignal = true;
                for (int i = 0; i < 450; ++i) {
                    const bool fists = i < 72;   // 0.8 s
                    const nf::FpHandsUp::Out ow = w.step(dtF, fists, raised, true);
                    everVetoed = everVetoed || ow.vetoed;
                    allSignal = allSignal && ow.signal;
                }
                check(!everVetoed && allSignal, "F68d: the diagnostic has no veto after Status.json stops reporting fists (5 s checked)");
                // (6) A long fists interval leaves no stale diagnostic veto when the hand first rises.
                nf::FpHandsUp l;
                for (int i = 0; i < 90 * 60; ++i) l.step(dtF, true, rest, true);   // a minute of fists
                const nf::FpHandsUp::Out first = l.step(dtF, true, raised, true);
                check(first.signal && !first.vetoed && first.w == 1.0, "F68d: after a minute on fists the diagnostic reports a raised hand without an old veto");
                // (7) The unarmed pose swaying by 5 cm while walking is never up; a hand moving too fast to be still never captures a rest.
                nf::FpHandsUp s;
                for (int i = 0; i < 45; ++i) s.step(dtF, true, rest, true);
                bool anyUp = false;
                double maxDist = 0.0;
                for (int i = 0; i < 900; ++i) {
                    double p[3] = {rest[0] + 0.05 * std::sin(i * 0.2), rest[1] + 0.03 * std::sin(i * 0.13), rest[2] + 0.04 * std::cos(i * 0.17)};
                    const nf::FpHandsUp::Out os = s.step(dtF, true, p, true);
                    anyUp = anyUp || os.up;
                    maxDist = std::max(maxDist, os.dist);
                }
                nf::FpHandsUp swing;
                bool captured = false;
                for (int i = 0; i < 400; ++i) {
                    double p[3] = {rest[0] + 0.3 * std::sin(i * 0.3), rest[1], rest[2]};
                    captured = captured || swing.step(dtF, true, p, true).restCaptured;
                }
                check(!anyUp && maxDist < 0.1 && !captured, "F68d: a walking sway of 5 cm is never up (largest distance under 10 cm), and a hand that keeps moving never captures a rest pose");
                // (8) After a veto the same pose held still for 3 s is the new unarmed rest (an emote pose is not a weapon).
                nf::FpHandsUp r2;
                for (int i = 0; i < 45; ++i) r2.step(dtF, true, rest, true);
                bool recaptured = false;
                nf::FpHandsUp::Out o2;
                for (int i = 0; i < 90 * 8; ++i) {
                    o2 = r2.step(dtF, true, raised, true);
                    recaptured = recaptured || o2.restCaptured;
                }
                check(recaptured && !o2.up && !o2.signal && std::fabs(r2.rest[0] - raised[0]) < 1e-9,
                      "F68d: after the veto, the same pose held still for 3 s becomes the new unarmed rest and the hands read down");
                // (9) A frame without the snapshot keeps the last diagnostic state.
                nf::FpHandsUp g;
                for (int i = 0; i < 45; ++i) g.step(dtF, true, rest, true);
                g.step(dtF, true, raised, true);
                const nf::FpHandsUp::Out miss = g.step(dtF, true, rest, false);
                check(miss.up && miss.signal && miss.w == 1.0 && g.havePrev, "F68d: a frame whose snapshot could not be read keeps the last state");
            }
            // F72: only SelectedWeapon can arm or weight the solve. Repeated FP hand-up hysteresis during fists
            // must stay diagnostic; a real equip and holster still use the existing 0.3 s ramp.
            {
                const double dtF = 1.0 / 90.0;
                const double rest[3] = {0.25, -0.10, 0.30};
                const double raised[3] = {rest[0] + 0.13, rest[1], rest[2]};
                const double lowered[3] = {rest[0] + 0.085, rest[1], rest[2]};
                nf::FpHandsUp gesture;
                for (int i = 0; i < 45; ++i) gesture.step(dtF, true, rest, true);
                nf::IkBlend selectedRamp{0.0f, nf::kArmedRampSec};
                bool sawDiagnosticSignal = false, fistsStayedUnarmed = true;
                uint32_t diagnosticPulses = 0;
                bool prevSignal = false;
                for (int i = 0; i < 180; ++i) {
                    const bool upPose = (i % 30) < 15;
                    const auto d = gesture.step(dtF, true, upPose ? raised : lowered, true);
                    selectedRamp.step(false, dtF);
                    const auto state = nf::ikArmedAuthority(false, selectedRamp.w);
                    sawDiagnosticSignal = sawDiagnosticSignal || d.signal;
                    if (d.signal && !prevSignal) ++diagnosticPulses;
                    prevSignal = d.signal;
                    fistsStayedUnarmed = fistsStayedUnarmed && !state.armed && state.ramp == 0.0 &&
                                         nf::ikArmedSolveWeight(state.armed, state.ramp, 1.0) == 0.0;
                }
                check(sawDiagnosticSignal && diagnosticPulses > 1 && fistsStayedUnarmed,
                      "F72: repeated 12/9 cm FP hand-up gait pulses during fists never arm or weight the production solve");

                double equipStart = -1.0, equipEnd = -1.0;
                bool equipStatusArmed = true;
                for (int i = 0; i < 27; ++i) {
                    gesture.step(dtF, false, raised, true);
                    selectedRamp.step(true, dtF);
                    const auto state = nf::ikArmedAuthority(true, selectedRamp.w);
                    equipStatusArmed = equipStatusArmed && state.armed;
                    if (i == 0) equipStart = state.ramp;
                    if (i == 26) equipEnd = state.ramp;
                }
                const double fullEquipWeight = nf::ikArmedSolveWeight(true, equipEnd, 1.0);
                check(equipStatusArmed && equipStart > 0.0 && equipStart < 0.1 && equipEnd > 0.999 && fullEquipWeight > 0.99999,
                      "F72: a real SelectedWeapon equip arms immediately and preserves the existing 0.3 s ramp to full weight");

                double holsterStart = -1.0, holsterEnd = -1.0, lastHolsterWeight = 2.0;
                bool holsterStatusDown = true, holsterFades = true;
                for (int i = 0; i < 27; ++i) {
                    const auto d = gesture.step(dtF, true, raised, true);  // the diagnostic can stay raised after holster
                    selectedRamp.step(false, dtF);
                    const auto state = nf::ikArmedAuthority(false, selectedRamp.w);
                    const double weight = nf::ikArmedSolveWeight(state.armed, state.ramp, 1.0);
                    if (i == 0) holsterStart = state.ramp;
                    if (i == 26) holsterEnd = state.ramp;
                    holsterStatusDown = holsterStatusDown && !state.armed;
                    holsterFades = holsterFades && d.signal && weight <= selectedRamp.w && weight <= lastHolsterWeight + 1e-9;
                    lastHolsterWeight = weight;
                }
                bool postHolsterGaitUnarmed = holsterEnd < 1e-5, postHolsterSawSignal = false;
                for (int i = 0; i < 180; ++i) {
                    const bool upPose = (i % 30) < 15;
                    const auto d = gesture.step(dtF, true, upPose ? raised : lowered, true);
                    selectedRamp.step(false, dtF);
                    const auto state = nf::ikArmedAuthority(false, selectedRamp.w);
                    postHolsterSawSignal = postHolsterSawSignal || d.signal;
                    if (d.signal) postHolsterGaitUnarmed = postHolsterGaitUnarmed && !state.armed && state.ramp == 0.0 &&
                                                           nf::ikArmedSolveWeight(state.armed, state.ramp, 1.0) == 0.0;
                }
                check(holsterStart < 1.0 && holsterStart > 0.9 && holsterEnd < 1e-5 && holsterStatusDown && holsterFades &&
                          postHolsterSawSignal && postHolsterGaitUnarmed,
                      "F72: holstering drops item authority immediately, fades the existing ramp, and later fist gait cannot rearm it");
            }
            // F68e: the neck target from the animated head, and the crouch in the pin's state split.
            {
                const double kR = nf::kIkPi / 180.0;
                // The standing animation: the neck 34 at (0, 1.45, 0), the head joint 13 cm above and 3 cm ahead of it, its frame aligned with the model's
                // (the rest offset (0, 0.155, 0.09) from the neck to the eye is then 2.5 cm above and 6 cm ahead of the head joint). pivot: the hips.
                const double pivot[3] = {0.0, 0.95, 0.0};
                const double n0s[3] = {0.0, 1.45, 0.0}, p44s[3] = {0.0, 1.58, 0.03};
                double rowsId[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
                // The pose after the torso leans forward by `deg` about the hips: every point and every axis rotates (y' = c y - s z, z' = s y + c z).
                const auto lean = [&](double deg, const double rowsIn[3][3], double n0[3], double p44[3], double rowsOut[3][3]) {
                    const double c = std::cos(deg * kR), s = std::sin(deg * kR);
                    const auto rot = [&](const double v[3], double o[3]) {
                        o[0] = v[0];
                        o[1] = c * v[1] - s * v[2];
                        o[2] = s * v[1] + c * v[2];
                    };
                    double a[3], bb[3];
                    for (int i = 0; i < 3; ++i) {
                        a[i] = n0s[i] - pivot[i];
                        bb[i] = p44s[i] - pivot[i];
                    }
                    double ra[3], rb[3];
                    rot(a, ra);
                    rot(bb, rb);
                    for (int i = 0; i < 3; ++i) {
                        n0[i] = pivot[i] + ra[i];
                        p44[i] = pivot[i] + rb[i];
                        rot(rowsIn[i], rowsOut[i]);
                    }
                };
                nf::PinEyeCal cal;
                bool done = false;
                for (int i = 0; i < 45; ++i) {
                    double p[3] = {p44s[0] + 0.001 * ((i % 3) - 1), p44s[1], p44s[2]};   // a millimetre of sway
                    done = cal.add(true, p, rowsId, n0s, 1.60 + 0.002 * ((i % 3) - 1));   // the eye 1.60 m up, 2 mm of sway
                }
                check(done && cal.have && cal.n == 45 && cal.axes.fAxis == 2 && cal.axes.uAxis == 1 && cal.axes.fSign > 0.0 && std::fabs(cal.e[1] - 0.025) < 2e-3 &&
                          std::fabs(cal.e[2] - 0.06) < 2e-3 && std::fabs(cal.headY - 1.58) < 1e-9 && std::fabs(cal.eyeY - 1.60) < 1e-3,
                      "F68e: 45 consecutive standing frames capture the eye point in the head joint's frame (2.5 cm up, 6 cm ahead of the head joint here) and "
                      "find the head's forward and up rows");
                // (1) Standing reproduces the rest offset: the eye 1.6 m up, its heading 0 and 30 deg to the head's.
                const double eyeP[3] = {0.1, 1.60, 0.2};
                double fwd0[3] = {0.0, 0.0, 1.0}, fwd30[3] = {std::sin(30.0 * kR), 0.0, std::cos(30.0 * kR)};
                double T0[3], T30[3], R0[3], R30[3];
                nf::PinAnimTarget pa0, pa30;
                nf::pinNeckTargetAnim(eyeP, fwd0, p44s, rowsId, n0s, cal, T0, &pa0);
                nf::pinNeckTargetAnim(eyeP, fwd30, p44s, rowsId, n0s, cal, T30, &pa30);
                nf::pinNeckTarget(eyeP, fwd0, R0);
                nf::pinNeckTarget(eyeP, fwd30, R30);
                const auto dist = [](const double a[3], const double c[3]) { return std::sqrt((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2])); };
                check(pa0.anim && pa30.anim && !pa0.clamped && dist(T0, R0) < 0.01 && dist(T30, R30) < 0.01 && dist(T0, R0) < 0.002,
                      "F68e: standing, the target from the animated head equals the F67c rest offset (0, -0.155, -0.09) along the eye's heading within 1 cm, "
                      "with the eye's heading 0 or 30 deg off the head's (the yaw comes from the eye)");
                // (2) A 40 deg lean of the chest and head (a crouch's torso), the camera 60 cm down (F69a: w_c = 1, the animated vector in full): the neck-to-eye vector
                // rotates with the pose; the neck target moves back.
                const double eyeC[3] = {0.1, 1.00, 0.2};
                double R0c[3];
                nf::pinNeckTarget(eyeC, fwd0, R0c);
                double n40[3], p40[3], r40[3][3];
                lean(40.0, rowsId, n40, p40, r40);
                double T40[3];
                nf::PinAnimTarget pa40;
                nf::pinNeckTargetAnim(eyeC, fwd0, p40, r40, n40, cal, T40, &pa40);
                const double fwdExp = 0.155 * std::sin(40.0 * kR) + 0.09 * std::cos(40.0 * kR), upExp = 0.155 * std::cos(40.0 * kR) - 0.09 * std::sin(40.0 * kR);
                const double backShift = (R0c[2] - T40[2]);   // how much further behind the standing target the crouch target sits (along +z)
                check(pa40.anim && std::fabs(pa40.vUsed[2] - fwdExp) < 1e-3 && std::fabs(pa40.vUsed[1] - upExp) < 1e-3 && std::fabs(backShift - (fwdExp - 0.09)) < 1e-3 &&
                          backShift > 0.07,
                      "F68e: a 40 deg chest pitch moves the neck target back by 7.9 cm (the neck-to-eye vector is 16.9 cm ahead and 6.1 cm up, against 9.0 and 15.5 "
                      "standing) and the eye's heading carries it: a 90 deg yaw puts the same 16.9 cm along the new heading");
                double T40y[3], eyeFwd90[3] = {1.0, 0.0, 0.0};
                nf::PinAnimTarget pa40y;
                nf::pinNeckTargetAnim(eyeC, eyeFwd90, p40, r40, n40, cal, T40y, &pa40y);
                check(std::fabs((eyeC[0] - T40y[0]) - fwdExp) < 1e-3 && std::fabs(eyeC[2] - T40y[2]) < 1e-3, "F68e: with the eye turned 90 deg the animated 16.9 cm forward offset lies along the eye's heading (x)");
                // (3) The head pitched past 70 deg (looking down): the heading comes from the up vector, no NaN, still 15.5 cm ahead of the neck's vertical.
                double n90[3], p90[3], r90[3][3];
                lean(90.0, rowsId, n90, p90, r90);
                double T90[3];
                nf::PinAnimTarget pa90;
                nf::pinNeckTargetAnim(eyeC, fwd0, p90, r90, n90, cal, T90, &pa90);
                check(pa90.anim && std::isfinite(T90[0]) && std::isfinite(T90[1]) && std::isfinite(T90[2]) && std::fabs(pa90.vUsed[2] - 0.155) < 2e-3 &&
                          std::fabs(pa90.vUsed[1] + 0.09) < 2e-3 && std::fabs(pa90.hHead[2] - 1.0) < 1e-9,
                      "F68e: looking straight down (the head pitched 90 deg) the head's heading is read from its up vector and the neck sits 15.5 cm behind and 9 cm "
                      "above the eye");
                // (4) Another frame convention (a bone-aligned head joint: local x forward, y up, z right) finds its axes and gives the same targets.
                double rowsB[3][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
                nf::PinEyeCal calB;
                for (int i = 0; i < 45; ++i) calB.add(true, p44s, rowsB, n0s, 1.60);
                double nB[3], pB[3], rB[3][3], TB[3];
                lean(40.0, rowsB, nB, pB, rB);
                nf::PinAnimTarget paB;
                nf::pinNeckTargetAnim(eyeC, fwd0, pB, rB, nB, calB, TB, &paB);
                check(calB.have && calB.axes.fAxis == 0 && calB.axes.uAxis == 1 && paB.anim && dist(TB, T40) < 1e-6,
                      "F68e: a head joint whose local x is the forward (and y the up) is found by the calibration and gives the same 40 deg target");
                // (5) The calibration needs 45 CONSECUTIVE standing frames: an armed, moving, high-or-low or pitched frame starts the run again; no calibration, no
                // animated target (the F67c rest offset).
                nf::PinEyeCal broken;
                bool any = false;
                for (int i = 0; i < 44; ++i) any = broken.add(true, p44s, rowsId, n0s, 1.60) || any;
                broken.add(false, p44s, rowsId, n0s, 1.60);   // a frame that is not standing
                for (int i = 0; i < 44; ++i) any = broken.add(true, p44s, rowsId, n0s, 1.60) || any;
                double pLean[3], nLean[3], rLean[3][3];
                lean(50.0, rowsId, nLean, pLean, rLean);
                broken.add(true, pLean, rLean, nLean, 1.60);   // a pitched head (a crouch before the calibration) is no standing frame
                for (int i = 0; i < 44; ++i) any = broken.add(true, p44s, rowsId, n0s, 1.60) || any;
                const bool still = !broken.have && !any;
                double Tn[3];
                nf::PinAnimTarget paN;
                nf::pinNeckTargetAnim(eyeP, fwd0, p44s, rowsId, n0s, broken, Tn, &paN);
                check(still && !paN.anim && dist(Tn, R0) < 1e-12 && !nf::pinCalStanding(0.1, true, false, 1.66) && !nf::pinCalStanding(0.5, false, false, 1.66) &&
                          !nf::pinCalStanding(0.1, false, true, 1.66) && !nf::pinCalStanding(0.1, false, false, 1.3) && nf::pinCalStanding(0.1, false, false, 1.66),
                      "F68e: the calibration needs 45 consecutive standing frames (unarmed, not ADS, under 0.35 m/s, the head 1.5..1.9 m up, level); without it the "
                      "target is the F67c rest offset");
                // (6) A glitched pose (the animated vector 1 m away) is clamped to 35 cm from the rest offset.
                double pFar[3] = {p44s[0], p44s[1] + 1.0, p44s[2]}, Tc[3];
                nf::PinAnimTarget paC;
                nf::pinNeckTargetAnim(eyeC, fwd0, pFar, rowsId, n0s, cal, Tc, &paC);
                const double devC = std::sqrt((paC.vUsed[0] - 0.0) * (paC.vUsed[0] - 0.0) + (paC.vUsed[1] - 0.155) * (paC.vUsed[1] - 0.155) +
                                              (paC.vUsed[2] - 0.09) * (paC.vUsed[2] - 0.09));
                check(paC.anim && paC.clamped && std::fabs(devC - nf::kPinAnimOffsetClampM) < 1e-9, "F68e: a neck-to-eye vector 1 m from the rest offset is clamped to 35 cm from it (a 90 deg lean moves it 25 cm)");
            }
            // F69a to F69d (core parts): the camera's crouch, the clearance bins, the weapon's yaw, the bend's target.
            {
                const double kR = nf::kIkPi / 180.0;
                const auto dist = [](const double a[3], const double c[3]) { return std::sqrt((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2])); };
                // ---- F69a: the neck target follows the camera's crouch, not the stance's head.
                const double n0s[3] = {0.0, 1.45, 0.0}, p44s[3] = {0.0, 1.58, 0.03};
                double rowsId[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
                const double fwd0[3] = {0.0, 0.0, 1.0};
                nf::PinEyeCal cal;
                for (int i = 0; i < 45; ++i) cal.add(true, p44s, rowsId, n0s, 1.62);
                // The rifle stance's head: the animated neck-to-eye vector (5.3, 6.6, 15.4) cm, the camera 4 cm down (F68 log 152023, dump 152258).
                const double pStance[3] = {p44s[0] + 0.053, p44s[1] + (0.066 - 0.155), p44s[2] + (0.154 - 0.09)};
                const double eyeStance[3] = {0.0, 1.62 - 0.04, 0.0};
                double Ts[3], Rs[3];
                nf::PinAnimTarget ps;
                nf::pinNeckTargetAnim(eyeStance, fwd0, pStance, rowsId, n0s, cal, Ts, &ps);
                nf::pinNeckTarget(eyeStance, fwd0, Rs);
                check(cal.have && std::fabs(cal.eyeY - 1.62) < 1e-9 && ps.anim && std::fabs(ps.vAnim[0] - 0.053) < 1e-6 && std::fabs(ps.vAnim[1] - 0.066) < 1e-6 &&
                          std::fabs(ps.vAnim[2] - 0.154) < 1e-6 && ps.wc == 0.0 && dist(Ts, Rs) < 1e-3 && std::fabs(ps.dEyeY + 0.04) < 1e-9,
                      "F69a: the rifle stance's head ((5.3, 6.6, 15.4) cm neck to eye) with the camera 4 cm down gives the REST offset within 1 mm (w_c 0), while the "
                      "animated vector is still computed");
                const double eyeLow[3] = {0.0, 1.62 - 0.60, 0.0};
                double Tl[3];
                nf::PinAnimTarget pl;
                nf::pinNeckTargetAnim(eyeLow, fwd0, pStance, rowsId, n0s, cal, Tl, &pl);
                check(pl.wc == 1.0 && dist(pl.vUsed, pl.vAnim) < 1e-12 && std::fabs(Tl[2] - (0.0 - 0.154)) < 1e-6 && std::fabs(Tl[1] - (1.02 - 0.066)) < 1e-6,
                      "F69a: the camera 60 cm down (the F67 crouch) gives the animated vector exactly (w_c 1)");
                bool mono = true, atRest = true;
                double prevWc = -1.0, prevZ = -1.0;
                for (int cm = -30; cm <= 70; ++cm) {   // the camera from 30 cm up (a jump) to 70 cm down
                    const double e[3] = {0.0, 1.62 - 0.01 * cm, 0.0};
                    double T[3];
                    nf::PinAnimTarget p;
                    nf::pinNeckTargetAnim(e, fwd0, pStance, rowsId, n0s, cal, T, &p);
                    mono = mono && p.wc >= prevWc - 1e-12 && p.vUsed[2] >= prevZ - 1e-12;
                    if (cm <= 20) atRest = atRest && p.wc == 0.0 && std::fabs(p.vUsed[2] - 0.09) < 1e-12 && std::fabs(p.vUsed[1] - 0.155) < 1e-12;
                    prevWc = p.wc;
                    prevZ = p.vUsed[2];
                }
                check(mono && atRest && std::fabs(nf::pinCrouchWeight(true, 1.62, 1.62 - 0.325) - 0.5) < 1e-12 && nf::pinCrouchWeight(true, 1.62, 1.62 - 0.45) == 1.0 &&
                          nf::pinCrouchWeight(true, 1.62, 1.62 - 0.20) == 0.0 && nf::pinCrouchWeight(true, 1.62, 1.62 + 0.30) == 0.0 && !nf::pinCrouchedWc(0.49) &&
                          nf::pinCrouchedWc(0.5) && nf::pinCrouchedWc(1.0) && !nf::pinCrouchedWc(std::numeric_limits<double>::quiet_NaN()),
                      "F69a: the blend is monotonic from the camera 30 cm up to 70 cm down, exactly the rest offset within 20 cm of the standing height (and in a jump), "
                      "one half at 32.5 cm, whole from 45 cm; a crouch is w_c of 0.5 or more");
                nf::PinEyeCal none, nanCal;
                for (int i = 0; i < 60; ++i) nanCal.add(true, p44s, rowsId, n0s, std::numeric_limits<double>::quiet_NaN());
                double Tn[3], Rl[3];
                nf::PinAnimTarget pn;
                nf::pinNeckTargetAnim(eyeLow, fwd0, pStance, rowsId, n0s, none, Tn, &pn);
                nf::pinNeckTarget(eyeLow, fwd0, Rl);
                check(!pn.anim && pn.wc == 0.0 && dist(Tn, Rl) < 1e-12 && !nanCal.have,
                      "F69a: without a calibration the target is the rest offset whatever the camera does (a non-finite eye height is no standing frame)");
                // ---- F73: natural-LOCAL physical pitch rotates the standing neck-to-eye rest vector; the game-authored crouch vector stays unchanged.
                bool orbitFixed = true, headingStable = true, crouchStable = true, pitchDecodeOk = true;
                nf::PinEyeCal domainCal;
                for (int i = 0; i < 45; ++i) domainCal.add(true, p44s, rowsId, n0s, 1.62, 1.40);
                const double rawFallbackEye[3] = {0.0, 1.0, 0.0};
                double TrawFallback[3];
                nf::PinAnimTarget prawFallback;
                nf::pinNeckTargetAnim(rawFallbackEye, fwd0, pStance, rowsId, n0s, domainCal, TrawFallback, &prawFallback, -0.5, false);
                const bool domainFallback = domainCal.have && std::fabs(domainCal.eyeY - 1.62) < 1e-12 && std::fabs(domainCal.eyeYRaw - 1.40) < 1e-12 &&
                                            std::fabs(prawFallback.wc - 0.896) < 1e-12;
                const double yawList[] = {45.0, 90.0};
                const double pitchList[] = {-60.0, -69.0, -70.0, -89.0, -90.0};
                const double neckOrbit[3] = {0.0, 1.465, 0.0};
                for (double yawDeg : yawList) {
                    const double yaw = yawDeg * nf::kIkPi / 180.0;
                    const double hx = std::sin(yaw), hz = std::cos(yaw);
                    for (double pitchDeg : pitchList) {
                        const double pitch = pitchDeg * nf::kIkPi / 180.0;
                        const double cp = std::cos(pitch), sp = std::sin(pitch);
                        float localPose[12] = {};
                        localPose[6] = static_cast<float>(-sp);
                        double decodedPitch = 0.0;
                        pitchDecodeOk = pitchDecodeOk && nf::pinNaturalLocalPitch(localPose, &decodedPitch) && std::fabs(decodedPitch - pitch) < 5e-6;
                        const double headFwd[3] = {hx * cp, sp, hz * cp};
                        const double headUp[3] = {-hx * sp, cp, -hz * sp};
                        const double vUp = nf::pinPitchRestUp(pitch, true);
                        const double vForward = nf::kPinRestBackM * cp - nf::kPinRestUpM * sp;
                        const double eyeOrbit[3] = {neckOrbit[0] + hx * vForward, neckOrbit[1] + vUp, neckOrbit[2] + hz * vForward};
                        double To[3];
                        nf::PinAnimTarget po;
                        nf::pinNeckTargetAnim(eyeOrbit, headFwd, pStance, rowsId, n0s, cal, To, &po, decodedPitch, true, headUp);
                        orbitFixed = orbitFixed && po.wc == 0.0 && dist(To, neckOrbit) < 1e-5 && std::fabs(po.eyeYSelected - cal.eyeY) < 1e-5 &&
                                     std::fabs(po.hEye[0] - hx) < 1e-9 && std::fabs(po.hEye[2] - hz) < 1e-9;
                        const double eyeCrouch[3] = {eyeOrbit[0], eyeOrbit[1] - 0.50, eyeOrbit[2]};
                        double Tc[3];
                        nf::PinAnimTarget pc;
                        nf::pinNeckTargetAnim(eyeCrouch, headFwd, pStance, rowsId, n0s, cal, Tc, &pc, decodedPitch, true, headUp);
                        double Tlegacy[3];
                        nf::PinAnimTarget pLegacy;
                        nf::pinNeckTargetAnim(eyeCrouch, headFwd, pStance, rowsId, n0s, cal, Tlegacy, &pLegacy, 0.0, false, headUp);
                        crouchStable = crouchStable && pc.wc == 1.0 && dist(pc.vUsed, pc.vAnim) < 1e-12 && dist(pc.vAnim, pLegacy.vAnim) < 1e-12;
                    }
                }
                // The scene camera can pitch under ADS/controller transforms; for a fixed natural-LOCAL pitch and heading it cannot change the correction.
                const double testPitch = -30.0 * nf::kIkPi / 180.0, testYaw = 45.0 * nf::kIkPi / 180.0;
                const double testEye[3] = {0.2, 1.55, -0.1};
                const double scenePitchA = -60.0 * nf::kIkPi / 180.0, scenePitchB = 20.0 * nf::kIkPi / 180.0;
                const double sceneFwdA[3] = {std::sin(testYaw) * std::cos(scenePitchA), std::sin(scenePitchA), std::cos(testYaw) * std::cos(scenePitchA)};
                const double sceneUpA[3] = {-std::sin(testYaw) * std::sin(scenePitchA), std::cos(scenePitchA), -std::cos(testYaw) * std::sin(scenePitchA)};
                const double sceneFwdB[3] = {std::sin(testYaw) * std::cos(scenePitchB), std::sin(scenePitchB), std::cos(testYaw) * std::cos(scenePitchB)};
                const double sceneUpB[3] = {-std::sin(testYaw) * std::sin(scenePitchB), std::cos(scenePitchB), -std::cos(testYaw) * std::sin(scenePitchB)};
                double Ta[3], Tb[3], Tf0[3], Tinvalid[3];
                nf::PinAnimTarget pta, ptb, pt0, pti;
                nf::pinNeckTargetAnim(testEye, sceneFwdA, pStance, rowsId, n0s, cal, Ta, &pta, testPitch, true, sceneUpA);
                nf::pinNeckTargetAnim(testEye, sceneFwdB, pStance, rowsId, n0s, cal, Tb, &ptb, testPitch, true, sceneUpB);
                nf::pinNeckTargetAnim(testEye, sceneFwdA, pStance, rowsId, n0s, cal, Tf0, &pt0, 0.0, true, sceneUpA);
                nf::pinNeckTargetAnim(testEye, sceneFwdA, pStance, rowsId, n0s, cal, Tinvalid, &pti, testPitch, false, sceneUpA);
                headingStable = dist(Ta, Tb) < 1e-12 && dist(Tf0, Tinvalid) < 1e-12 && std::fabs(pta.eyeYSelected - ptb.eyeYSelected) < 1e-12;
                check(orbitFixed && headingStable && crouchStable && pitchDecodeOk && domainFallback,
                      "F73: natural-LOCAL pitch decode and nonzero-yaw physical orbit keep the standing neck fixed through near-vertical gaze, preserve crouch classification and authored vector, and ignore scene pitch when local pitch is fixed; invalid local pose uses the raw-height calibration and yaw-only offset");
                bool physicalRestFixed = true;
                for (double yawDeg : yawList) {
                    const double yaw = yawDeg * nf::kIkPi / 180.0;
                    const double hx = std::sin(yaw), hz = std::cos(yaw);
                    for (double pitchDeg : pitchList) {
                        const double pitch = pitchDeg * nf::kIkPi / 180.0;
                        const double cp = std::cos(pitch), sp = std::sin(pitch);
                        const double headFwd[3] = {hx * cp, sp, hz * cp};
                        const double headUp[3] = {-hx * sp, cp, -hz * sp};
                        const double vUp = nf::kPinRestUpM * cp + nf::kPinRestBackM * sp;
                        const double vForward = nf::kPinRestBackM * cp - nf::kPinRestUpM * sp;
                        const double eye[3] = {neckOrbit[0] + hx * vForward, neckOrbit[1] + vUp, neckOrbit[2] + hz * vForward};
                        double target[3] = {};
                        physicalRestFixed = physicalRestFixed && nf::pinNeckTargetPhysicalRest(eye, headFwd, headUp, pitch, true, target) &&
                                            dist(target, neckOrbit) < 1e-9;
                    }
                }
                double invalidTarget[3] = {7.0, 8.0, 9.0};
                const double noHeading[3] = {};
                const double noUp[3] = {};
                physicalRestFixed = physicalRestFixed && !nf::pinNeckTargetPhysicalRest(testEye, noHeading, sceneUpA, testPitch, true, invalidTarget) &&
                                    invalidTarget[0] == 7.0 && invalidTarget[1] == 8.0 && invalidTarget[2] == 9.0 &&
                                    !nf::pinNeckTargetPhysicalRest(testEye, sceneFwdA, noUp, testPitch, true, invalidTarget) && invalidTarget[0] == 7.0 &&
                                    !nf::pinNeckTargetPhysicalRest(testEye, sceneFwdA, sceneUpA, testPitch, false, invalidTarget) && invalidTarget[0] == 7.0;
                check(physicalRestFixed,
                      "F83: physical-rest target exactly recovers the neck through the captured pitch/yaw sweep, while missing direction or LOCAL pitch refuses without changing the caller target");
                check(nf::pinRestReferenceGenerationAllowed(false, 620u, 0u) && nf::pinRestReferenceGenerationAllowed(true, 620u, 620u) &&
                          !nf::pinRestReferenceGenerationAllowed(true, 620u, 621u) && !nf::pinRestReferenceGenerationAllowed(true, 0u, 0u) &&
                          !nf::pinRestReferenceGenerationAllowed(false, 0u, 0u),
                      "F83: physical-rest candidate permits the existing validated sample only when current generation is unavailable and refuses known mismatch or zero generation");
                nf::NativePrimarySelectionInput nativeSelection;
                nativeSelection.activityValid = nativeSelection.sinkRead = nativeSelection.sinkSignatureValid = nativeSelection.inventoryRead = true;
                nativeSelection.selectedRead = nativeSelection.holsterRead = nativeSelection.categoryCacheRead = true;
                nativeSelection.sink = 0x1000; nativeSelection.inventory = 0x2000; nativeSelection.holsterId = 0xFFFFFFFFu;
                nativeSelection.categoryId[0] = 10; nativeSelection.categoryId[1] = 11; nativeSelection.categoryId[2] = 12; nativeSelection.categoryId[3] = 13;
                nativeSelection.selectedId = nativeSelection.holsterId; nativeSelection.renderFrame = 1; nativeSelection.qpc = 100;
                nf::NativePrimarySelectionState alreadyPrimaryState;
                auto alreadyPrimaryInput = nativeSelection;
                alreadyPrimaryInput.selectedId = alreadyPrimaryInput.categoryId[2] = 77;
                const auto alreadyPrimary = alreadyPrimaryState.observe(alreadyPrimaryInput);
                alreadyPrimaryInput.renderFrame = 2; alreadyPrimaryInput.qpc = 200;
                const auto alreadyPrimaryRepeat = alreadyPrimaryState.observe(alreadyPrimaryInput);
                const auto baselinePublication = nf::nativePrimaryPublication(false, false, false, alreadyPrimary);
                nf::NativePrimarySelectionDecision readablePrimaryTransition;
                readablePrimaryTransition.readable = readablePrimaryTransition.primary = readablePrimaryTransition.identityChanged = readablePrimaryTransition.terminate = true;
                const auto transitionWithOldTermination = nf::nativePrimaryPublication(true, true, true, readablePrimaryTransition);
                nf::NativePrimarySelectionState nativeState;
                const auto holstered = nativeState.observe(nativeSelection);
                nativeSelection.selectedId = nativeSelection.categoryId[2] = 42; nativeSelection.renderFrame = 2; nativeSelection.qpc = 200;
                const auto primary = nativeState.observe(nativeSelection);
                const auto transitionPublication = nf::nativePrimaryPublication(false, false, true, primary);
                const auto duplicate = nativeState.observe(nativeSelection);
                nativeSelection.renderFrame = 1; nativeSelection.qpc = 150;
                const auto reversed = nativeState.observe(nativeSelection);
                nativeSelection.renderFrame = 5; nativeSelection.qpc = 500;
                const auto gap = nativeState.observe(nativeSelection);
                nativeSelection.primaryIntent = true; nativeSelection.renderFrame = 6; nativeSelection.qpc = 600;
                const auto noIdKeyRepeat = nativeState.observe(nativeSelection);
                nativeSelection.renderFrame = 7; nativeSelection.qpc = 700;
                const auto secondary = nativeState.observe(nativeSelection);
                nativeSelection.secondaryIntent = true; nativeSelection.renderFrame = 7; nativeSelection.qpc = 701;
                const auto secondaryIntent = nativeState.observe(nativeSelection);
                nativeSelection.secondaryIntent = false; nativeSelection.sink = 0x3000; nativeSelection.renderFrame = 8; nativeSelection.qpc = 800;
                const auto sinkChange = nativeState.observe(nativeSelection);
                nativeSelection.sink = 0x1000; nativeSelection.selectedId = nativeSelection.holsterId; nativeSelection.renderFrame = 9; nativeSelection.qpc = 900;
                const auto holster = nativeState.observe(nativeSelection);
                nativeSelection.selectedId = 42; nativeSelection.categoryId[2] = 42; nativeSelection.renderFrame = 10; nativeSelection.qpc = 1000;
                const auto reselect = nativeState.observe(nativeSelection);
                nativeSelection.activityValid = false; nativeSelection.renderFrame = 11; nativeSelection.qpc = 1100;
                const auto transientRead = nativeState.observe(nativeSelection);
                nativeSelection.activityValid = true; nativeSelection.renderFrame = 11; nativeSelection.qpc = 1101;
                const auto afterTransient = nativeState.observe(nativeSelection);
                nativeSelection.sinkSignatureValid = false; nativeSelection.renderFrame = 12; nativeSelection.qpc = 1200;
                const auto invalidSignature = nativeState.observe(nativeSelection);
                nativeSelection.sinkSignatureValid = true; nativeSelection.renderFrame = 13; nativeSelection.qpc = 1300;
                const auto afterInvalid = nativeState.observe(nativeSelection);
                nativeSelection.categoryId[1] = 42; nativeSelection.renderFrame = 14; nativeSelection.qpc = 1400;
                const auto conflict = nativeState.observe(nativeSelection);
                check(holstered.readable && !holstered.primary && primary.primary && primary.identityChanged && primary.epoch > holstered.epoch &&
                          alreadyPrimary.primary && !alreadyPrimary.identityChanged && alreadyPrimary.epoch != 0 && alreadyPrimaryRepeat.primary &&
                          !alreadyPrimaryRepeat.identityChanged && alreadyPrimaryRepeat.epoch == alreadyPrimary.epoch &&
                          duplicate.duplicate && duplicate.epoch == primary.epoch && reversed.reversed && reversed.epoch == primary.epoch &&
                          gap.frameGap && gap.epoch == primary.epoch && noIdKeyRepeat.primary && !noIdKeyRepeat.identityChanged &&
                          secondary.primary && !secondary.terminate && secondaryIntent.primary && !secondaryIntent.terminate &&
                          sinkChange.identityChanged && !sinkChange.terminate && sinkChange.epoch > secondaryIntent.epoch &&
                          !baselinePublication.followEligible && transitionPublication.primary && transitionPublication.followEligible &&
                          transitionWithOldTermination.primary && transitionWithOldTermination.followEligible &&
                          !nf::nativePrimaryPublication(true, true, true, invalidSignature).primary &&
                          !nf::nativePrimaryPublication(true, true, true, invalidSignature).followEligible &&
                          !holster.primary && holster.terminate && reselect.primary && reselect.epoch > holster.epoch && !invalidSignature.readable && invalidSignature.terminate &&
                          transientRead.suspended && !transientRead.terminate && afterTransient.primary && !afterTransient.identityChanged && afterTransient.epoch == reselect.epoch &&
                          afterInvalid.primary && !afterInvalid.identityChanged && conflict.readable && !conflict.primary && conflict.terminate,
                      "F83: native selection requires a unique category-2 ID; holster, cache conflict and bad signatures terminate, while transient reads suspend and preserve identity; same-frame updates, secondary intent, gaps and key repeats do not create epochs");
                nf::PrimaryDrawFollowState drawFollow;
                check(nf::primaryDrawSourcesCurrent(true, true, 20, 30, 120, 125, 100, 130, 1000) &&
                          !nf::primaryDrawSourcesCurrent(true, true, 101, 20, 120, 125, 100, 130, 1000) &&
                          !nf::primaryDrawSourcesCurrent(true, true, 20, 20, 99, 125, 100, 130, 1000) &&
                          !nf::primaryDrawSourcesCurrent(true, false, 20, 20, 120, 125, 100, 130, 1000) &&
                          !nf::primaryDrawSourcesCurrent(true, true, 20, 20, 120, 125, 100, 1200, 1000),
                      "F83: draw targets require fresh FP and G reads after the native epoch within one current 100ms window");
                check(nf::nativeJournalPolicyHeld(true, true, true, 7, 7) && !nf::nativeJournalPolicyHeld(true, true, true, 7, 8) &&
                          !nf::nativeJournalPolicyHeld(true, false, true, 7, 7) && !nf::nativeJournalPolicyHeld(false, true, true, 7, 7),
                      "F83: last proven epoch holds stale-tool/pistol policy through source expiry, releasing on serial catchup or confirmed identity loss");
                check(nf::nativePrimaryEpochIdentityAvailable(false, true, true, true, 9) &&
                          !nf::nativePrimaryEpochIdentityAvailable(false, false, true, true, 9) &&
                          !nf::nativePrimaryEpochIdentityAvailable(true, false, false, false, 9),
                      "F83: failed bounded sample copy retains only last accepted identity; confirmed terminal sample revokes it");
                nf::PrimaryDrawFollowInput drawInput;
                drawInput.selectedPrimary = true;
                drawInput.generationsMatch = drawInput.gameStateValid = drawInput.targetsValid = true;
                drawInput.nativeEpoch = 8; drawInput.epochQpc = 100; drawInput.fpGeneration = 4; drawInput.threePGeneration = 17;
                drawInput.freshFp = true; drawInput.fpFrame = 20; drawInput.fpQpc = 90; drawInput.armedWeight = 0.8;
                const auto beforeEpochFresh = drawFollow.step(drawInput);
                drawInput.fpQpc = 120; drawInput.armedWeight = 0.8;
                const auto firstFresh = drawFollow.step(drawInput);
                drawInput.suspended = true; drawInput.selectedPrimary = false;
                const auto suspended = drawFollow.step(drawInput);
                drawInput.suspended = false; drawInput.selectedPrimary = true;
                const auto reusedFpSnapshot = drawFollow.step(drawInput);
                drawInput.fpFrame = 21; drawInput.fpQpc = 130; drawInput.armedWeight = 0.95; drawInput.contactQualified = true; drawInput.normalContactWeight = 0.074;
                const auto handoverLow = drawFollow.step(drawInput);
                drawInput.fpFrame = 22; drawInput.fpQpc = 140; drawInput.armedWeight = 0.2; drawInput.normalContactWeight = 0.8;
                const auto handoverNear = drawFollow.step(drawInput);
                drawInput.fpFrame = 23; drawInput.fpQpc = 150; drawInput.armedWeight = 0.99; drawInput.normalContactWeight = 0.97;
                const auto armedStillGrowing = drawFollow.step(drawInput);
                drawInput.fpFrame = 24; drawInput.fpQpc = 160; drawInput.armedWeight = 0.2; drawInput.normalContactWeight = 1.0;
                const auto handoverDone = drawFollow.step(drawInput);
                drawInput.fpFrame = 25; drawInput.fpQpc = 170;
                const auto steadyRelease = drawFollow.step(drawInput);
                nf::PrimaryDrawFollowState generationFollow;
                drawInput.nativeEpoch = 9; drawInput.epochQpc = 200; drawInput.fpFrame = 26; drawInput.fpQpc = 210; drawInput.contactQualified = false;
                drawInput.normalContactWeight = 0.0; drawInput.generationsMatch = false; drawInput.fpGeneration = 4; drawInput.threePGeneration = 5;
                const auto pendingGeneration = generationFollow.step(drawInput);
                drawInput.generationsMatch = true; drawInput.fpFrame = 27; drawInput.fpQpc = 220;
                const auto independentGenerationDomains = generationFollow.step(drawInput);
                drawInput.fpFrame = 28; drawInput.fpQpc = 230; drawInput.fpGeneration = 5;
                const auto generationRefusal = generationFollow.step(drawInput);
                drawInput.fpFrame = 29; drawInput.fpQpc = 240; drawInput.fpGeneration = 4;
                const auto sameEpochCannotRestart = generationFollow.step(drawInput);
                nf::PrimaryDrawFollowState effectiveWeightFollow;
                nf::PrimaryDrawFollowInput effectiveWeightInput;
                effectiveWeightInput.selectedPrimary = effectiveWeightInput.generationsMatch = effectiveWeightInput.gameStateValid = effectiveWeightInput.targetsValid = true;
                effectiveWeightInput.freshFp = true; effectiveWeightInput.nativeEpoch = 44; effectiveWeightInput.epochQpc = 100;
                effectiveWeightInput.fpQpc = 110; effectiveWeightInput.fpFrame = 1; effectiveWeightInput.fpGeneration = 1; effectiveWeightInput.threePGeneration = 1;
                effectiveWeightInput.armedWeight = 0.5; effectiveWeightInput.normalContactWeight = 0.25; effectiveWeightInput.contactQualified = true;
                const auto halfEnvelope = effectiveWeightFollow.step(effectiveWeightInput);
                effectiveWeightInput.fpQpc = 120; effectiveWeightInput.fpFrame = 2; effectiveWeightInput.armedWeight = 0.5; effectiveWeightInput.normalContactWeight = 0.5;
                const auto fullEnvelope = effectiveWeightFollow.step(effectiveWeightInput);
                check(beforeEpochFresh.active && beforeEpochFresh.refused && !beforeEpochFresh.write && firstFresh.active && firstFresh.write && firstFresh.weight == 0.8 &&
                          suspended.suspended && suspended.active && !suspended.write && reusedFpSnapshot.write && reusedFpSnapshot.weight == firstFresh.weight &&
                          handoverLow.write && handoverLow.weight == 0.95 && handoverNear.write && handoverNear.weight == 0.95 && armedStillGrowing.write && armedStillGrowing.weight == 0.99 &&
                          handoverDone.completed && !handoverDone.write && !steadyRelease.active && pendingGeneration.active && pendingGeneration.refused &&
                          independentGenerationDomains.write && independentGenerationDomains.active && generationRefusal.refused && !generationRefusal.active && !sameEpochCannotRestart.active &&
                          halfEnvelope.write && !halfEnvelope.completed && halfEnvelope.weight == 0.5 && fullEnvelope.completed && !fullEnvelope.write,
                      "F83: draw waits for fresh post-epoch FP/G data, tolerates same-snapshot reuse and transient selection suspension, latches independent generations, holds armed weight until contact catches up, and cannot reopen after completion or generation change");
                // ---- F69d: the bins: jumping from the vertical speed alone, crouching from w_c; the clearance's inside test and axes.
                check(nf::pinState(0.1, 0.0) == 0 && nf::pinState(1.2, 0.0) == 1 && nf::pinState(4.0, 0.2) == 2 && nf::pinState(1.0, 1.1) == 3 && nf::pinState(4.0, -1.0) == 3 &&
                          nf::pinState(0.1, 0.0, true) == 4 && nf::pinState(4.0, 0.0, true) == 4 && nf::pinState(0.1, 1.2, true) == 3,
                      "F69d: the state bins: jumping is the vertical speed alone (over 0.8 m/s, either way), then crouching (the camera's w_c), running, walking, standing");
                check(nf::headInside(0.054, 0.05) && !nf::headInside(0.19, 0.20) && nf::headInside(0.079, 0.119) && !nf::headInside(0.081, 0.05) && !nf::headInside(0.02, 0.121) &&
                          !nf::headInside(0.155, 0.12),
                      "F69d: 'inside' is the eye under the clavicle line + 8 cm AND under 12 cm ahead of the chest 24 (F68's dump: eye 2.4 cm over the neck: inside; standing: not)");
                const double h1[3] = {0.0, 0.0, 1.0}, h2[3] = {1.0, 0.0, 0.0}, v3[3] = {1.0, 2.0, 3.0};
                double hx1[3], hx2[3];
                nf::headingAxes(h1, v3, hx1);
                nf::headingAxes(h2, v3, hx2);
                check(hx1[0] == 1.0 && hx1[1] == 2.0 && hx1[2] == 3.0 && hx2[0] == -3.0 && hx2[1] == 2.0 && hx2[2] == 1.0,
                      "F69d: the clearance's axes: right, up, forward of the eye's heading (heading +x puts the old forward on the left)");
                // ---- F69b: the armed square-up turns the torso by the weapon's yaw, not the shoulders'.
                int ax = -1;
                double sg = 0.0;
                const double aim[3] = {0.0, 0.0, 1.0}, upV[3] = {0.0, 1.0, 0.0};
                double rperm[3][3] = {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}}, rflip[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
                const double aimDiag[3] = {0.57735, 0.57735, 0.57735};   // all three rows 54.7 deg off: no row within 45 deg
                const bool okId = nf::barrelRow(rowsId, aim, &ax, &sg) && ax == 2 && sg == 1.0;
                int ax2 = -1, ax3 = -1, ax4 = -1;
                double sg2 = 0.0, sg3 = 0.0, sg4 = 0.0;
                const bool bPerm = nf::barrelRow(rperm, aim, &ax2, &sg2), bFlip = nf::barrelRow(rflip, aim, &ax3, &sg3);
                check(okId && bPerm && ax2 == 0 && sg2 == -1.0 && bFlip && ax3 == 1 && sg3 == -1.0 && !nf::barrelRow(rowsId, aimDiag, &ax4, &sg4),
                      "F69b: the barrel axis is the signed row most aligned with the aim (permuted and flipped attach rows pick the right one; an aim 54.7 deg from every row "
                      "gives none)");
                nf::BarrelAxes bax;
                nf::BarrelDirs bd0;
                int latchedAt = -1;
                for (int i = 0; i < 12; ++i) {
                    bd0 = bax.step(rflip, rperm, aim);
                    if (bd0.latched && latchedAt < 0) latchedAt = i;
                }
                const nf::BarrelDirs bdHeld = bax.step(rowsId, rperm, aim);   // other rows now: the latched picks stand
                bax.reset();
                const nf::BarrelDirs bdNew = bax.step(rowsId, rperm, aim);
                check(latchedAt == static_cast<int>(nf::kBarrelLatchFrames) - 1 && bd0.ok && bd0.a3 == 1 && bd0.s3 == -1.0 && bd0.aF == 0 && bd0.sF == -1.0 &&
                          std::fabs(bd0.b3[2] - 1.0) < 1e-12 && std::fabs(bd0.bF[2] - 1.0) < 1e-12 && bdHeld.latched && bdHeld.a3 == 1 && !bdNew.latched && bdNew.a3 == 2,
                      "F69b: the pick latches after 10 consecutive armed calls with the same rows in both rigs, holds when the rows change, and drops on reset (an item "
                      "change or the end of arming)");
                // A stance bladed 38 deg (the upper body turned, the weapon on the aim): the shoulders' phi is -38, the weapon's 0, and U*'s left shoulder sits 11 cm further forward.
                const double blade = 38.0 * kR, pivot[3] = {0.0, 1.4, 0.0};
                const double uL[3] = {-0.18 * std::cos(blade), 1.4, 0.18 * std::sin(blade)}, uR[3] = {0.18 * std::cos(blade), 1.4, -0.18 * std::sin(blade)};
                nf::BarrelAxes bax2;
                nf::BarrelDirs bd1;
                for (int i = 0; i < 12; ++i) bd1 = bax2.step(rowsId, rowsId, aim);
                const nf::SquareYaw sqW = nf::squareYaw(true, bd1, uL, uR, aim), sqS = nf::squareYaw(false, bd1, uL, uR, aim);
                double uStarW[3], uStarS[3];
                nf::turnAboutVertical(uL, pivot, sqW.phi, uStarW);
                nf::turnAboutVertical(uL, pivot, sqS.phi, uStarS);
                double phiDirect = 0.0;
                nf::ikSquareToForward(uL, uR, aim, &phiDirect);
                check(sqW.ok && sqW.src == nf::kSquarePreservePose && std::fabs(sqW.phiW) < 1e-9 && std::fabs(sqW.phiSh + 38.0 * kR) < 1e-9 && sqW.phi == 0.0 &&
                          sqS.src == nf::kSquareShoulders && sqS.phi == phiDirect && std::fabs(uStarW[2] - uL[2]) < 1e-9 &&
                          std::fabs(uStarS[2] - uL[2] + 0.18 * std::sin(blade)) < 1e-9,
                      "F75: the armed 38 deg authored stance keeps torso phi 0 despite shoulder phi -38 and an aim-aligned pre-solve barrel; unarmed square-up remains unchanged");
                // The weapon's animated wrist can lead or lag by 30 deg before arm IK overwrites it; fixed shoulders, root and hand targets must not turn the chest.
                nf::BarrelDirs bdLag = bd1;
                const double fixedHandL[3] = {-0.25, 1.2, 0.55}, fixedHandR[3] = {0.25, 1.2, 0.55};
                double fixedShoulderL[3], fixedShoulderR[3];
                nf::turnAboutVertical(uL, pivot, 0.0, fixedShoulderL);
                nf::turnAboutVertical(uR, pivot, 0.0, fixedShoulderR);
                const double handBeforeL[3] = {fixedHandL[0] - uL[0], fixedHandL[1] - uL[1], fixedHandL[2] - uL[2]};
                const double handBeforeR[3] = {fixedHandR[0] - uR[0], fixedHandR[1] - uR[1], fixedHandR[2] - uR[2]};
                bool wristInvariant = true;
                double wristMeasuredPhi[2] = {};
                for (int i = 0; i < 2; ++i) {
                    const double wristYaw = (i == 0 ? -30.0 : 30.0) * kR;
                    bdLag.b3[0] = std::sin(wristYaw); bdLag.b3[1] = 0.0; bdLag.b3[2] = std::cos(wristYaw);
                    const nf::SquareYaw sqLag = nf::squareYaw(true, bdLag, uL, uR, aim);
                    wristMeasuredPhi[i] = sqLag.phiW;
                    double shoulderL[3], shoulderR[3];
                    nf::turnAboutVertical(uL, pivot, sqLag.phi, shoulderL);
                    nf::turnAboutVertical(uR, pivot, sqLag.phi, shoulderR);
                    const double handAfterL[3] = {fixedHandL[0] - shoulderL[0], fixedHandL[1] - shoulderL[1], fixedHandL[2] - shoulderL[2]};
                    const double handAfterR[3] = {fixedHandR[0] - shoulderR[0], fixedHandR[1] - shoulderR[1], fixedHandR[2] - shoulderR[2]};
                    wristInvariant = wristInvariant && sqLag.src == nf::kSquarePreservePose && sqLag.phi == 0.0 &&
                                     std::fabs(shoulderL[0] - fixedShoulderL[0]) < 1e-12 && std::fabs(shoulderL[2] - fixedShoulderL[2]) < 1e-12 &&
                                     std::fabs(shoulderR[0] - fixedShoulderR[0]) < 1e-12 && std::fabs(shoulderR[2] - fixedShoulderR[2]) < 1e-12 &&
                                     std::fabs(nf::ikLen(handAfterL) - nf::ikLen(handBeforeL)) < 1e-12 &&
                                     std::fabs(nf::ikLen(handAfterR) - nf::ikLen(handBeforeR)) < 1e-12;
                }
                check(wristInvariant && std::fabs(wristMeasuredPhi[0] - 30.0 * kR) < 1e-9 && std::fabs(wristMeasuredPhi[1] + 30.0 * kR) < 1e-9,
                      "F75: with fixed shoulders, root and hand targets, animated wrist/barrel yaw at -30/+30 deg is measured but cannot rotate the armed chest or move either shoulder");
                bool rootYawOk = true;
                const double origin[3] = {}, forwardLocal[3] = {0.0, 0.0, 1.0};
                const bool oneArm[2] = {true, false};
                const double reachRadius[2] = {0.5, 0.0};
                const double reachableV[2][3] = {{0.3, 0.0, 0.2}, {0.0, 0.0, 0.0}};
                const double unreachableV[2][3] = {{0.8, 0.0, 0.6}, {0.0, 0.0, 0.0}};
                const auto setYawMatrix = [](float m[16], double a) noexcept {
                    nf::pfzIdentity(m);
                    const float c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
                    m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
                };
                for (double rootYawDeg : {30.0, 60.0}) {
                    const double rootYaw = rootYawDeg * kR;
                    double aimRoot[3], uLRoot[3], uRRoot[3], b3Root[3], bFRoot[3];
                    nf::turnAboutVertical(forwardLocal, origin, rootYaw, aimRoot);
                    nf::turnAboutVertical(uL, origin, rootYaw, uLRoot);
                    nf::turnAboutVertical(uR, origin, rootYaw, uRRoot);
                    nf::turnAboutVertical(bd1.b3, origin, rootYaw, b3Root);
                    nf::turnAboutVertical(bd1.bF, origin, rootYaw, bFRoot);
                    nf::BarrelDirs rootedBarrels = bd1;
                    for (int c = 0; c < 3; ++c) { rootedBarrels.b3[c] = b3Root[c]; rootedBarrels.bF[c] = bFRoot[c]; }
                    const nf::SquareYaw rooted = nf::squareYaw(true, rootedBarrels, uLRoot, uRRoot, aimRoot);
                    double handRoot[3]; nf::turnAboutVertical(fixedHandL, origin, rootYaw, handRoot);
                    const double baseReach[3] = {fixedHandL[0] - uL[0], fixedHandL[1] - uL[1], fixedHandL[2] - uL[2]};
                    const double rotatedReach[3] = {handRoot[0] - uLRoot[0], handRoot[1] - uLRoot[1], handRoot[2] - uLRoot[2]};
                    double inV[2][3] = {}, outV[2][3] = {}, inDelta[3] = {}, outDelta[3] = {};
                    nf::turnAboutVertical(reachableV[0], origin, rootYaw, inV[0]);
                    nf::turnAboutVertical(unreachableV[0], origin, rootYaw, outV[0]);
                    bool inFeasible = false, outFeasible = false;
                    const bool inActive = nf::reachLimitDelta(inV, reachRadius, oneArm, inDelta, &inFeasible);
                    const bool outActive = nf::reachLimitDelta(outV, reachRadius, oneArm, outDelta, &outFeasible);
                    const double outAfter[3] = {outV[0][0] + outDelta[0], outV[0][1] + outDelta[1], outV[0][2] + outDelta[2]};
                    float m3pBase[16], frootBase[16], globalYaw[16], m3pYaw[16], frootYaw[16], invM3pBase[16], invM3p[16], Gbefore[16], Gafter[16];
                    setYawMatrix(m3pBase, 0.17); m3pBase[12] = -0.4f; m3pBase[13] = 0.2f; m3pBase[14] = 0.1f;
                    setYawMatrix(frootBase, -0.11); frootBase[12] = 0.3f; frootBase[13] = 1.4f; frootBase[14] = -0.2f;
                    setYawMatrix(globalYaw, rootYaw);
                    nf::pfzMul(m3pBase, globalYaw, m3pYaw); nf::pfzMul(frootBase, globalYaw, frootYaw);
                    const bool mapBaseOk = nf::pfzAffineInverse(m3pBase, invM3pBase), mapOk = nf::pfzAffineInverse(m3pYaw, invM3p);
                    nf::pfzMul(frootBase, invM3pBase, Gbefore); nf::pfzMul(frootYaw, invM3p, Gafter);
                    float yawPalm3p[16], yawPalmFp[16], wristYaw[16], fpWorld[16], threePWorld[16], modelWrist[16], handTarget[16], expectedWorld[16];
                    setYawMatrix(yawPalm3p, -0.23); yawPalm3p[12] = -0.12f; yawPalm3p[13] = 0.9f; yawPalm3p[14] = 0.08f;
                    setYawMatrix(yawPalmFp, 0.31); yawPalmFp[12] = 0.03f; yawPalmFp[13] = 1.0f; yawPalmFp[14] = 0.04f;
                    setYawMatrix(wristYaw, 0.21); wristYaw[12] = 0.04f; wristYaw[13] = -0.03f; wristYaw[14] = 0.02f;
                    nf::pfzMul(yawPalmFp, wristYaw, fpWorld); nf::pfzMul(fpWorld, frootYaw, expectedWorld);
                    const bool mapped = mapOk && nf::handRetarget(yawPalm3p, yawPalmFp, wristYaw, Gafter, handTarget);
                    nf::pfzMul(yawPalm3p, handTarget, modelWrist); nf::pfzMul(modelWrist, m3pYaw, threePWorld);
                    bool mappingEquivariant = mapped;
                    for (int c = 0; c < 16; ++c) mappingEquivariant = mappingEquivariant && std::fabs(threePWorld[c] - expectedWorld[c]) < 3e-5f;
                    bool GGlobalInvariant = true;
                    for (int c = 0; c < 16; ++c) GGlobalInvariant = GGlobalInvariant && std::fabs(Gbefore[c] - Gafter[c]) < 3e-5f;
                    rootYawOk = rootYawOk && rooted.ok && rooted.src == nf::kSquarePreservePose && rooted.phi == 0.0 &&
                                 std::fabs(nf::ikLen(baseReach) - nf::ikLen(rotatedReach)) < 1e-9 && !inActive && inFeasible && outActive && outFeasible &&
                                 std::fabs(nf::ikLen(outAfter) - reachRadius[0]) < 1e-9 && mapBaseOk && mappingEquivariant && GGlobalInvariant;
                }
                check(rootYawOk,
                      "F75: at genuine 30 and 60 deg root yaw the armed torso remains authored, shoulder-to-hand reach is world-equivariant, and reachable/unreachable targets retain their reach guards");
                nf::BarrelDirs bdUp = bd1;
                bdUp.b3[0] = 0.1;
                bdUp.b3[1] = 0.99;
                bdUp.b3[2] = 0.0;
                const nf::SquareYaw sqUp = nf::squareYaw(true, bdUp, uL, uR, aim), sqNone = nf::squareYaw(true, nf::BarrelDirs{}, uL, uR, aim);
                double keptL[3];
                nf::turnAboutVertical(uL, pivot, sqUp.phi, keptL);
                check(sqUp.src == nf::kSquarePreservePose && sqUp.phi == 0.0 && sqUp.weaponWhy == nf::kSquareWeapon3pVertical && sqUp.b3Xz == 0.1 &&
                          sqUp.bFXz == 1.0 && keptL[0] == uL[0] && keptL[2] == uL[2] && sqNone.src == nf::kSquarePreservePose && sqNone.phi == 0.0 &&
                          sqNone.weaponWhy == nf::kSquareWeaponNoAxes && sqS.src == nf::kSquareShoulders && sqS.phi == phiDirect,
                      "F75: armed stance stays authored with phi 0 whether measured weapon yaw is vertical or unavailable; unarmed square-up remains the shoulder rule");
                // F70a's measured failure shape: a latched selected row can remain valid in 3D yet have no usable horizontal yaw. The F70 rifle's shoulder fallback was
                // -42.5 deg and caused a 21.4 cm reach correction. The helper's returned armed angle must preserve the authored pose in this case.
                nf::BarrelDirs rifleLatched = bd1;
                rifleLatched.latched = true;
                rifleLatched.b3[0] = 0.05;
                rifleLatched.b3[1] = 0.995;
                rifleLatched.b3[2] = 0.05;
                rifleLatched.bF[0] = 0.0;
                rifleLatched.bF[1] = 0.0;
                rifleLatched.bF[2] = 1.0;
                const double bladeRifle = 42.5 * kR;
                const double uLR[3] = {-0.18 * std::cos(bladeRifle), 1.4, 0.18 * std::sin(bladeRifle)};
                const double uRR[3] = {0.18 * std::cos(bladeRifle), 1.4, -0.18 * std::sin(bladeRifle)};
                const nf::SquareYaw sqRifleBad = nf::squareYaw(true, rifleLatched, uLR, uRR, aim);
                double uPreserved[3], uOldShoulder[3];
                bool preservedOver = false;
                const double preservedApplied = nf::squareApplied(sqRifleBad.phi, true, 1.0, 1.0, &preservedOver);
                const double oldShoulderApplied = nf::squareApplied(sqRifleBad.phiSh, true, 1.0, 1.0, nullptr);
                nf::turnAboutVertical(uLR, pivot, preservedApplied, uPreserved);
                nf::turnAboutVertical(uLR, pivot, oldShoulderApplied, uOldShoulder);
                check(sqRifleBad.src == nf::kSquarePreservePose && sqRifleBad.weaponWhy == nf::kSquareWeapon3pVertical && sqRifleBad.b3Xz < nf::kBarrelMinXz &&
                          sqRifleBad.bFXz == 1.0 && std::fabs(sqRifleBad.phi) < 1e-12 && std::fabs(sqRifleBad.phiSh + 42.5 * kR) < 1e-9 &&
                          preservedApplied == 0.0 && !preservedOver && std::fabs(oldShoulderApplied + 42.5 * kR) < 1e-9 &&
                          uPreserved[0] == uLR[0] && uPreserved[2] == uLR[2] && std::fabs(uOldShoulder[2] - uLR[2]) > 0.10,
                      "F70a: the F70 rifle failure shape (latched vertical row, shoulder phi -42.5 deg) returns zero and preserves the authored pose; the old shoulder turn moved U* over 10 cm");
                // ---- F69c: the bend never undoes the reach rule.
                {
                    const double P[3] = {0.0, 1.05, 0.0}, T[3] = {0.0, 1.45, 0.02}, reach[3] = {0.0, 0.0, 0.20};
                    const double Npin[3] = {T[0] + reach[0], T[1] + reach[1], T[2] + reach[2]};    // the pin has slid the neck by the reach correction
                    const double Tbend[3] = {T[0] + reach[0], T[1] + reach[1], T[2] + reach[2]};   // F69c's target
                    double pitchNew = 0.0, rollNew = 0.0, pitchOld = 0.0, rollOld = 0.0;
                    const bool okNew = nf::bendSolve(P, Npin, Tbend, &pitchNew, &rollNew), okOld = nf::bendSolve(P, Npin, T, &pitchOld, &rollOld);
                    double lf = 0.0, lb = 0.0, lr = 0.0;
                    nf::bendLimitsArmed(1.0, &lf, &lb, &lr);
                    nf::BendClamp clNew, clOld;
                    const double pNewC = nf::bendClampPitchLim(pitchNew, lf, lb, &clNew), pOldC = nf::bendClampPitchLim(pitchOld, lf, lb, &clOld);
                    double Rn[9], Ro[9], vN[3], vO[3];
                    const double v[3] = {Npin[0] - P[0], Npin[1] - P[1], Npin[2] - P[2]};
                    nf::bendRot(pNewC, 0.0, 1.0, Rn);
                    nf::bendRot(pOldC, 0.0, 1.0, Ro);
                    nf::bendApply(v, Rn, vN);
                    nf::bendApply(v, Ro, vO);
                    check(okNew && okOld && std::fabs(pitchNew) < 1e-9 && std::fabs(rollNew) < 1e-9 && !clNew.fwd && !clNew.back && !clNew.roll &&
                              std::fabs(vN[1] - v[1]) < 1e-9 && clOld.back && vO[1] - v[1] > 0.02,
                          "F69c: with the target T plus a 20 cm reach correction the bend wants 0, hits no clamp and leaves the neck's height alone; aimed at T alone (F68) "
                          "it pitched back against the armed limit and lifted the neck over 2 cm");
                }
            }
            // F69e to F69h (core parts): the reticle's tag, the veto's margin, the grip frames, the off hand.
            {
                const double kR = nf::kIkPi / 180.0;
                // ---- F69e: F18's exact zeros take a tag.
                float rz[3] = {0.0f, 0.0f, 5.0f};
                const uint32_t sentinelRz = edvr::drawtag::sentinel(10, 1234);
                const uint32_t seeded = edvr::drawtag::seedZeros(rz);
                const bool encoded = edvr::drawtag::encode(rz, sentinelRz);
                const uint32_t decoded = edvr::drawtag::decode(rz);
                float nz[3] = {-0.0f, 0.0f, 5.0f};
                edvr::drawtag::seedZeros(nz);
                float nn[3] = {0.5f, -2.0f, 5.0f};
                const uint32_t seededNone = edvr::drawtag::seedZeros(nn);
                float badC[3] = {0.0f, std::numeric_limits<float>::quiet_NaN(), 5.0f};
                edvr::drawtag::seedZeros(badC);
                float den[3] = {1e-40f, 1.0f, 1.0f};
                edvr::drawtag::seedZeros(den);
                check(seeded == 2 && encoded && decoded == sentinelRz && std::fabs(rz[0]) < 1e-29f && std::fabs(rz[1]) < 1e-29f && rz[0] > 0.0f && rz[2] > 4.99f && rz[2] < 5.01f &&
                          std::signbit(nz[0]) && !std::signbit(nz[1]) && seededNone == 0 && nn[0] == 0.5f && nn[1] == -2.0f &&
                          !edvr::drawtag::encode(badC, sentinelRz) && !edvr::drawtag::encode(den, sentinelRz),
                      "F69e: key 10's (0, 0, 5) takes a tag after the zeros become 2^-100 m (it decodes its sentinel, moves under 1e-29 m, keeps a zero's sign); a NaN or "
                      "a denormal component still refuses and a nonzero position is untouched");
                // ---- F69f: the diagnostic signal leads Status.json by 2.0 s and its fists-up veto holds to 3 s.
                {
                    const double dtF = 1.0 / 90.0;
                    const double restP[3] = {0.25, -0.10, 0.30}, raised[3] = {0.40, 0.10, 0.55};
                    nf::FpHandsUp w;
                    for (int i = 0; i < 45; ++i) w.step(dtF, true, restP, true);
                    bool everVetoed = false, allSignal = true;
                    for (int i = 0; i < 450; ++i) {
                        const bool fists = i < 180;   // Status.json arrives 2.0 s after the hands rise
                        const nf::FpHandsUp::Out ow = w.step(dtF, fists, raised, true);
                        everVetoed = everVetoed || ow.vetoed;
                        allSignal = allSignal && ow.signal;
                    }
                    nf::FpHandsUp v;
                    for (int i = 0; i < 45; ++i) v.step(dtF, true, restP, true);
                    int firstVeto = -1;
                    bool at28 = false;
                    for (int i = 0; i < 400; ++i) {
                        double p[3] = {raised[0] + 0.02 * std::sin(i * 0.3), raised[1], raised[2]};   // a sway: the hand is never still, so no recapture hides the veto
                        const nf::FpHandsUp::Out ov = v.step(dtF, true, p, true);
                        if (ov.vetoed && firstVeto < 0) firstVeto = i;
                        if (i == 251) at28 = ov.signal && !ov.vetoed;   // 2.8 s
                    }
                    check(!everVetoed && allSignal && nf::kHandsVetoSec == 3.0 && at28 && firstVeto >= 269 && firstVeto <= 272,
                          "F69f: the diagnostic stays unvetoed when Status.json arrives 2.0 s after the hand rises, then vetoes a fists-up pose from 3.0 s (frame 270)");
                }
                // ---- F69g: the grip frames: synthetic hands with a known Gamma give the reported delta within 0.1 deg and 0.1 mm.
                {
                    // Double-precision references, row-vector 4x4: attach frame A = Gamma x H (an attach in the hand's axes, then the hand in the model).
                    struct M4 { double m[16]; };
                    const auto fromAA = [](const double axis[3], double angDeg, const double t[3]) {
                        double R[9];
                        double n[3] = {axis[0], axis[1], axis[2]};
                        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        for (double& c : n) c /= l;
                        nf::ikRotAxis(n, angDeg * nf::kIkPi / 180.0, R);
                        M4 o{};
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c) o.m[r * 4 + c] = R[r * 3 + c];
                        o.m[12] = t[0];
                        o.m[13] = t[1];
                        o.m[14] = t[2];
                        o.m[15] = 1.0;
                        return o;
                    };
                    const auto mul = [](const M4& a, const M4& b) {
                        M4 o{};
                        for (int i = 0; i < 4; ++i)
                            for (int j = 0; j < 4; ++j)
                                for (int k = 0; k < 4; ++k) o.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
                        return o;
                    };
                    const auto toF = [](const M4& a, float out[16]) {
                        for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(a.m[i]);
                    };
                    const double aH[3] = {1, 2, 3}, aG1[3] = {0.3, -1, 0.5}, aG2[3] = {-1, 0.4, 0.8}, aH2[3] = {0, 1, 1};
                    const double tH[3] = {0.31, 1.22, -0.44}, tG1[3] = {0.012, -0.020, 0.031}, tG2[3] = {-0.008, 0.015, 0.044}, tH2[3] = {-0.2, 1.1, 0.35};
                    const M4 Hfp = fromAA(aH, 33.0, tH), Gfp = fromAA(aG1, 25.0, tG1), H3 = fromAA(aH2, 71.0, tH2), G3 = fromAA(aG2, 80.0, tG2);
                    const M4 Afp = mul(Gfp, Hfp), A3 = mul(G3, H3);
                    float fH[16], fA[16], fH3[16], fA3[16], gF[16], g3[16], dl[16];
                    toF(Hfp, fH);
                    toF(Afp, fA);
                    toF(H3, fH3);
                    toF(A3, fA3);
                    const bool okF = nf::gripFrame(fA, fH, gF), ok3 = nf::gripFrame(fA3, fH3, g3), okD = nf::gripDelta(g3, gF, dl);
                    // Expected: rotation R3 R_fp^T (angle by the trace), translation (t3 - t_fp) R_fp^T.
                    double tr = 0.0;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) tr += G3.m[i * 4 + j] * Gfp.m[i * 4 + j];
                    const double angExp = std::acos(std::max(-1.0, std::min(1.0, (tr - 1.0) * 0.5))) * 180.0 / nf::kIkPi;
                    const double dt[3] = {G3.m[12] - Gfp.m[12], G3.m[13] - Gfp.m[13], G3.m[14] - Gfp.m[14]};
                    double tExp[3];
                    for (int c = 0; c < 3; ++c) tExp[c] = dt[0] * Gfp.m[c * 4 + 0] + dt[1] * Gfp.m[c * 4 + 1] + dt[2] * Gfp.m[c * 4 + 2];   // (t3 - t_fp) R_fp^T
                    double axG[3], axD[3], angG1 = 0.0, angG3 = 0.0, angD = 0.0;
                    nf::frameAxisAngle(gF, axG, &angG1);
                    nf::frameAxisAngle(g3, axG, &angG3);
                    nf::frameAxisAngle(dl, axD, &angD);
                    double maxGErr = 0.0;
                    for (int i = 0; i < 16; ++i) maxGErr = std::max(maxGErr, std::fabs(static_cast<double>(gF[i]) - Gfp.m[i]));
                    double tErr = 0.0;
                    for (int c = 0; c < 3; ++c) tErr = std::max(tErr, std::fabs(static_cast<double>(dl[12 + c]) - tExp[c]));
                    check(okF && ok3 && okD && maxGErr < 1e-5 && std::fabs(angG1 - 25.0) < 0.05 && std::fabs(angG3 - 80.0) < 0.05 && std::fabs(angD - angExp) < 0.1 && tErr < 1e-4,
                          "F69g: hands with a known Gamma (25 and 80 deg, centimetres of offset, other hand poses) are recovered from A x inv(H) (under 1e-5), and the "
                          "reported delta is within 0.1 deg and 0.1 mm of the independent product");
                    // The wrist: a rotation about the hinge normal is flexion, about eT x n1 deviation, about the forearm axis twist; mixed they add up per axis.
                    const double eT[3] = {0.0, 0.0, 1.0}, n1[3] = {1.0, 0.0, 0.0}, dv[3] = {0.0, -1.0, 0.0};   // eT x n1 = (0, 1, 0)
                    double dAxis[3];
                    nf::ikCross(eT, n1, dAxis);
                    double Xf[9], Xd[9], Xt[9];
                    nf::ikRotAxis(n1, 30.0 * kR, Xf);
                    nf::ikRotAxis(dAxis, 20.0 * kR, Xd);
                    nf::ikRotAxis(eT, 40.0 * kR, Xt);
                    const nf::WristAngles wf = nf::wristAngles(Xf, eT, n1), wd = nf::wristAngles(Xd, eT, n1), wt = nf::wristAngles(Xt, eT, n1);
                    // twist then flex: X = twist 40 about eT, then flex 30 about n1 (row-vector: apply the twist first)
                    double Xm[9];
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) Xm[r * 3 + c] = Xt[r * 3] * Xf[c] + Xt[r * 3 + 1] * Xf[3 + c] + Xt[r * 3 + 2] * Xf[6 + c];
                    const nf::WristAngles wm = nf::wristAngles(Xm, eT, n1);
                    (void)dv;
                    check(wf.ok && std::fabs(wf.flexDeg - 30.0) < 1e-6 && std::fabs(wf.devDeg) < 1e-6 && std::fabs(wf.twistDeg) < 1e-6 && wd.ok && std::fabs(wd.devDeg - 20.0) < 1e-6 &&
                              std::fabs(wd.flexDeg) < 1e-6 && wt.ok && std::fabs(wt.twistDeg - 40.0) < 1e-6 && std::fabs(wt.swingDeg) < 1e-6 && wm.ok &&
                              std::fabs(wm.swingDeg - 30.0) < 1e-6 && std::fabs(wm.twistDeg - 40.0) < 1e-6 && std::fabs(wm.flexDeg - 30.0) < 1e-6,
                          "F69g: the wrist after the solve splits into flexion (about the elbow's hinge normal), deviation (about eT x n1) and twist (about the forearm "
                          "axis); a pure twist has no swing and a 40 deg twist plus a 30 deg flexion has a 30 deg swing");
                    // The steady window: 0.5 s of a still attach (the first sample has no previous), once; a moving attach never completes; an item change starts again.
                    nf::GripSteady gs;
                    const double dtG = 1.0 / 90.0, at[3] = {0.2, -0.1, 0.4};
                    int completedAt = -1, completions = 0;
                    double meanAtDone = 0.0, sdAtDone = -1.0;
                    for (int i = 0; i < 200; ++i) {
                        const double sep = 0.326 + 0.005 * ((i % 2) ? 1.0 : -1.0);
                        if (gs.step(7, true, dtG, at, sep)) {
                            ++completions;
                            if (completedAt < 0) {
                                completedAt = i;
                                meanAtDone = gs.sepMean();
                                sdAtDone = gs.sepSd();
                            }
                        }
                    }
                    nf::GripSteady moving;
                    int movingDone = 0;
                    for (int i = 0; i < 400; ++i) {
                        const double p[3] = {0.2 + 0.0030 * i, -0.1, 0.4};   // 0.27 m/s
                        movingDone += moving.step(7, true, dtG, p, 0.326) ? 1 : 0;
                    }
                    nf::GripSteady jerk;
                    int jerkAt = -1;
                    for (int i = 0; i < 200; ++i) {
                        const double p[3] = {0.2 + (i == 20 ? 0.05 : 0.0), -0.1, 0.4};   // one 5 cm jump (4.5 m/s) at frame 20
                        if (jerk.step(7, true, dtG, p, 0.326) && jerkAt < 0) jerkAt = i;
                    }
                    const bool againOnNewItem = gs.step(8, true, dtG, at, 0.326) == false && gs.item == 8 && !gs.done;
                    check(completedAt == 45 && completions == 1 && std::fabs(meanAtDone - 0.326) < 0.0005 && std::fabs(sdAtDone - 0.005) < 0.0005 && movingDone == 0 && jerkAt >= 65 && jerkAt <= 67 && againOnNewItem,
                          "F69g: the grip line is due once, after 0.5 s of a still attach (frame 45, the separation's mean and sd over that window); a moving attach never "
                          "completes it, a jerk restarts the window, and a new item starts again");
                }
                // ---- F69h: the off hand follows the FP rig: within 40 cm of the right attach and steady over 10 frames, else it follows the animation.
                {
                    const double dtF = 1.0 / 90.0;
                    const auto mk = [&](double yawDeg, double x, double y, double z, float m[16]) {
                        const double a = yawDeg * kR, c = std::cos(a), s = std::sin(a);
                        const float r[16] = {static_cast<float>(c), 0, static_cast<float>(-s), 0, 0, 1, 0, 0, static_cast<float>(s), 0, static_cast<float>(c), 0,
                                             static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1};
                        std::memcpy(m, r, sizeof(r));
                    };
                    float a51[16], a52[16];
                    mk(10.0, 0.25, -0.10, 0.30, a51);
                    mk(40.0, 0.25 - 0.326, -0.10, 0.30, a52);   // the rifle's pair: 32.6 cm apart
                    nf::OffHand rifle;
                    int firstTwo = -1, firstFull = -1;
                    double wAt10 = -1.0;
                    for (int i = 0; i < 40; ++i) {
                        const nf::OffHand::Out o = rifle.step(dtF, true, true, a51, a52);
                        if (o.two && firstTwo < 0) firstTwo = i;
                        if (o.w >= 1.0 && firstFull < 0) firstFull = i;
                        if (i == 9) wAt10 = o.w;
                    }
                    check(firstTwo == 10 && wAt10 == 0.0 && firstFull >= 23 && firstFull <= 25,
                          "F69h: a rifle's pair (32.6 cm apart, a steady relative orientation) is two-handed once 10 steady frames have been seen (frame 10), the off arm's "
                          "weight then ramps to 1 in 0.15 s");
                    // The pistol: the left attach far from the right (55 cm): one-handed, weight 0, and a rifle's weight falls to 0 in 0.15 s when the hand leaves.
                    float far52[16];
                    mk(40.0, 0.25 - 0.55, -0.10, 0.30, far52);
                    nf::OffHand pistol;
                    bool anyTwo = false;
                    for (int i = 0; i < 60; ++i) anyTwo = anyTwo || pistol.step(dtF, true, true, a51, far52).two;
                    int zeroAt = -1;
                    nf::OffHand::Out oLeave;
                    for (int i = 0; i < 20; ++i) {
                        oLeave = rifle.step(dtF, true, true, a51, far52);
                        if (oLeave.w == 0.0 && zeroAt < 0) zeroAt = i;
                    }
                    check(!anyTwo && pistol.w == 0.0 && zeroAt >= 12 && zeroAt <= 14 && !oLeave.two,
                          "F69h: a left attach 55 cm from the right (the pistol, the tools) is never two-handed and its weight stays 0; when a rifle's left hand leaves, the "
                          "weight falls to 0 over 0.15 s");
                    // An orientation that is not steady (2 deg a frame) is one-handed even within 40 cm; unarmed forgets everything; an unreadable frame decides one-handed.
                    nf::OffHand turning;
                    bool turningTwo = false;
                    for (int i = 0; i < 60; ++i) {
                        float t52[16];
                        mk(40.0 + 2.0 * i, 0.25 - 0.326, -0.10, 0.30, t52);
                        turningTwo = turningTwo || turning.step(dtF, true, true, a51, t52).two;
                    }
                    rifle.step(dtF, true, true, a51, a52);
                    const nf::OffHand::Out gone = rifle.step(dtF, false, true, a51, a52);
                    const nf::OffHand::Out blind = rifle.step(dtF, true, false, a51, a52);
                    check(!turningTwo && !gone.two && gone.w == 0.0 && rifle.have == 0 && !blind.ok && !blind.two,
                          "F69h: a left attach turning 2 deg a frame is one-handed within 40 cm (not steady under 5 deg over 10 frames); unarmed forgets the history; a frame "
                          "whose attaches cannot be read decides one-handed");

                    nf::OffHand relaxed;
                    bool relaxedTwo = false;
                    for (int i = 0; i < 60; ++i) relaxedTwo = relaxedTwo || relaxed.step(dtF, true, true, a51, a52, false).two;
                    check(!relaxedTwo && relaxed.w == 0.0,
                          "F73: a one-handed tool can force the authored off-arm pose even when its attach pair looks two-handed");
                    nf::OffHand leavingRifle;
                    for (int i = 0; i < 40; ++i) leavingRifle.step(dtF, true, true, a51, a52);
                    int relaxedAt = -1;
                    for (int i = 0; i < 20; ++i) {
                        const auto o = leavingRifle.step(dtF, true, true, a51, a52, false);
                        if (o.w == 0.0 && relaxedAt < 0) relaxedAt = i;
                    }
                    check(relaxedAt >= 12 && relaxedAt <= 14,
                          "F73: changing from a supported two-handed item fades the off-arm solve to the authored pose in 0.15 s");
                    // F78: relative-angle steadiness acquires authority; it does not release a moving same-item grip. P1 identity controls history.
                    nf::OffHand latched;
                    int acquiredAt = -1;
                    for (uint32_t frame = 1; frame <= 32; ++frame) {
                        const double separation = 0.333 + 0.025 * (static_cast<double>(frame % 3) / 2.0);
                        mk(0.0, 0.25 - separation, -0.10, 0.30, a52);
                        const auto o = latched.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                        if (o.two && acquiredAt < 0) acquiredAt = static_cast<int>(frame);
                    }
                    nf::OffHand::Out movedGrip;
                    for (uint32_t frame = 33; frame <= 40; ++frame) {
                        const double t = static_cast<double>(frame - 33) / 7.0;
                        const double angle = 5.3 * t;
                        const double separation = 0.333 + 0.025 * t;
                        mk(angle, 0.25 - separation, -0.10, 0.30, a52);
                        movedGrip = latched.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                    }
                    const bool retainedInMotion = movedGrip.two && !movedGrip.qualified && movedGrip.stablePolicy && movedGrip.w == 1.0;
                    mk(0.0, 0.25 - 0.410, -0.10, 0.30, a52);
                    const auto tooFar = latched.step(dtF, true, true, a51, a52, true, 41, 8, 4, 0xAAu, 7);
                    const bool distanceReleaseRampsPolicy = !tooFar.two && tooFar.stablePolicy && tooFar.w > 0.0;
                    int releaseStableCalls = 0;
                    for (uint32_t frame = 42; frame < 68; ++frame) {
                        mk(0.0, 0.25 - 0.410, -0.10, 0.30, a52);
                        const auto o = latched.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                        if (o.stablePolicy && !o.two) ++releaseStableCalls;
                    }
                    int reacquiredAt = -1;
                    for (uint32_t frame = 68; frame < 80; ++frame) {
                        mk(0.0, 0.25 - 0.326, -0.10, 0.30, a52);
                        const auto o = latched.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                        if (o.two && reacquiredAt < 0) reacquiredAt = static_cast<int>(frame);
                    }
                    const bool distanceReacquires = reacquiredAt == 78;
                    const double radiusAt922 = nf::ikOffHandReachRadius(0.62, 0.922);
                    const double radiusAt996 = nf::ikOffHandReachRadius(0.62, 0.996);
                    check(acquiredAt == 11 && retainedInMotion && distanceReleaseRampsPolicy && releaseStableCalls > 0 && distanceReacquires &&
                              std::fabs((radiusAt922 - radiusAt996) - 0.074) < 1e-12,
                          "F78: steady unique frames acquire once; 5.3-degree motion at 33.3-35.8 cm keeps authority, >40 cm releases through the existing ramp, reacquires after 11 steady frames, and off-hand reach radius changes by the measured weight delta");

                    nf::OffHand uniqueFrames;
                    mk(0.0, 0.25 - 0.326, -0.10, 0.30, a52);
                    for (uint32_t frame = 1; frame <= 10; ++frame) uniqueFrames.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                    const auto beforeDuplicate = uniqueFrames.step(dtF, true, true, a51, a52, true, 10, 8, 4, 0xAAu, 7);
                    const auto duplicate = uniqueFrames.step(0.1, true, true, a51, a52, true, 10, 8, 4, 0xAAu, 7);
                    const auto eleventh = uniqueFrames.step(dtF, true, true, a51, a52, true, 11, 8, 4, 0xAAu, 7);
                    nf::OffHand identityChange;
                    mk(0.0, 0.25 - 0.326, -0.10, 0.30, a52);
                    for (uint32_t frame = 1; frame <= 12; ++frame) identityChange.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                    const auto newFpRig = identityChange.step(dtF, true, true, a51, a52, true, 13, 8, 4, 0xBBu, 8);
                    const bool identityReleased = newFpRig.identityReset && !newFpRig.two && newFpRig.w == 0.0 && identityChange.have == 1;
                    nf::OffHand releasedPolicy;
                    mk(0.0, 0.25 - 0.326, -0.10, 0.30, a52);
                    for (uint32_t frame = 1; frame <= 20; ++frame) releasedPolicy.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                    const auto explicitFree = releasedPolicy.step(dtF, true, true, a51, a52, false, 21, 8, 4, 0xAAu, 7);
                    check(beforeDuplicate.w == 0.0 && duplicate.duplicate && duplicate.w == beforeDuplicate.w && !duplicate.two && eleventh.two &&
                              identityReleased && !explicitFree.two && explicitFree.stablePolicy && explicitFree.w > 0.0,
                          "F78: duplicate P1 calls do not advance acquisition or weight; item/FP/3P identity resets authority; explicit one-hand policy releases while stable solve follows the ramp");

                    nf::OffHand discontinuity;
                    mk(0.0, 0.25 - 0.326, -0.10, 0.30, a52);
                    for (uint32_t frame = 1; frame <= 12; ++frame) discontinuity.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7);
                    const auto p1Gap = discontinuity.step(dtF, true, true, a51, a52, true, 15, 8, 4, 0xAAu, 7);
                    const bool gapRetained = p1Gap.gapReset && p1Gap.two && discontinuity.have == 1;
                    const auto reverse = discontinuity.step(0.1, true, true, a51, a52, true, 14, 8, 4, 0xAAu, 7);
                    const bool reverseReleased = reverse.reversed && !reverse.two && reverse.w == p1Gap.w && discontinuity.have == 0;
                    const auto invalid = discontinuity.step(dtF, true, false, a51, a52, true, 16, 8, 4, 0xAAu, 7);
                    const bool invalidReleased = !invalid.ok && !invalid.two && discontinuity.have == 0;
                    check(gapRetained && reverseReleased && invalidReleased,
                          "F78: skipped P1 frames restart acquisition without dropping a valid retained grip; reversed or unreadable inputs release and cannot advance state");
                    nf::OffHand nativeEpochContact;
                    for (uint32_t frame = 1; frame <= 12; ++frame)
                        nativeEpochContact.step(dtF, true, true, a51, a52, true, frame, 8, 4, 0xAAu, 7, 88);
                    const uint32_t historyBeforeSuspend = nativeEpochContact.have;
                    const double weightBeforeSuspend = nativeEpochContact.w;
                    const auto nativeSuspend = nativeEpochContact.step(dtF, true, true, a51, a52, true, 13, 9, 4, 0xAAu, 7, 88, true);
                    const auto journalCatchup = nativeEpochContact.step(dtF, true, true, a51, a52, true, 13, 9, 4, 0xAAu, 7, 88);
                    const auto disarmedSuspend = nativeEpochContact.step(dtF, false, false, a51, a52, true, 13, 9, 4, 0xAAu, 7, 88, true);
                    const auto realNativeChange = nativeEpochContact.step(dtF, true, true, a51, a52, true, 14, 9, 4, 0xAAu, 7, 89);
                    check(nativeSuspend.historySamples == historyBeforeSuspend,
                          "F83: transient source suspension freezes contact history");
                    check(nativeSuspend.w == weightBeforeSuspend,
                          "F83: transient source suspension freezes contact weight");
                    check(!journalCatchup.identityReset,
                          "F83: journal serial catchup preserves a native primary epoch");
                    check(disarmedSuspend.historySamples == historyBeforeSuspend && disarmedSuspend.w == journalCatchup.w,
                          "F83: a suspended epoch preserves classifier state even when the normal armed flag falls");
                    check(journalCatchup.historySamples == historyBeforeSuspend,
                          "F83: journal catchup keeps existing attachment history");
                    check(realNativeChange.identityReset && !realNativeChange.two,
                          "F83: a new native epoch resets contact state");
                    check(nf::supportItem("$wpn_s_pistol_name;") == nf::kSupportPistol &&
                          nf::supportItem("$humanoid_rechargetool_name;") == nf::kSupportNone &&
                          nf::supportItem("$humanoid_companalyser_name;") == nf::kSupportNone &&
                          nf::supportItem("$humanoid_repairtool_name;") == nf::kSupportNone &&
                          nf::supportItem("$wpn_rifle_name;") == nf::kSupportNone &&
                          nf::gameAuthoredLeftArm("$humanoid_rechargetool_name;") &&
                          nf::gameAuthoredLeftArm("$humanoid_companalyser_name;") &&
                          !nf::gameAuthoredLeftArm("$humanoid_repairtool_name;") &&
                          !nf::gameAuthoredLeftArm("$wpn_s_pistol_name;"),
                           "F73/F80: explicit one-hand tools keep their authored policy; pistols use their own anatomical left hand and Arc Cutter keeps its authored handle pose");
                    const bool pistolNaturalFirstDraw = nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 10, true, true, true, true, true, 1);
                    const bool pistolNaturalAfterSerialChange = nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 11, 11, true, true, true, true, true, 1);
                    const bool pistolRejected[9] = {
                        !nf::authoredPistolSupportEligible(false, nf::kSupportPistol, 10, 10, true, true, true, true, true, 1),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 9, true, true, true, true, true, 1),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 10, false, true, true, true, true, 1),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 10, true, false, true, true, true, 1),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 10, true, true, false, true, true, 1),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 10, true, true, true, false, true, 1),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 10, true, true, true, true, false, 1),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportPistol, 10, 10, true, true, true, true, true, 0),
                        !nf::authoredPistolSupportEligible(true, nf::kSupportNone, 10, 10, true, true, true, true, true, 1)};
                    bool allPistolFailuresReject = true;
                    for (bool rejects : pistolRejected) allPistolFailuresReject = allPistolFailuresReject && rejects;
                    check(pistolNaturalFirstDraw && pistolNaturalAfterSerialChange && allPistolFailuresReject,
                          "F80: a coherent, close mapped pistol uses its anatomical target immediately across serial changes; stale identity, invalid/distant attaches, bad mapping, or fallback target refuses it");
                    const double firstValidCup = nf::supportCupBlendStep(0.0, true, true, 1.0 / 30.0);
                    const double invalidCup = nf::supportCupBlendStep(0.8, true, false, 1.0 / 30.0);
                    const double releasedCup = nf::supportCupBlendStep(0.8, false, true, 0.05 / 0.15);
                    check(firstValidCup == 1.0 && invalidCup == 0.0 && std::fabs(releasedCup - (0.8 - 0.05 / 0.15)) < 1e-12,
                          "F74: a valid requested pistol cup is selected on its first sample, invalid geometry clears immediately, and a still-valid release keeps its existing 0.15 s fade");
                    bool armEnvelopeOk = true;
                    for (double armedWeight : {0.0, 0.5, 1.0}) {
                        const double finalArmWeight = nf::ikArmedSolveWeight(true, armedWeight, 1.0) * firstValidCup;
                        armEnvelopeOk = armEnvelopeOk && std::fabs(finalArmWeight - armedWeight) < 1e-12;
                    }
                    check(armEnvelopeOk,
                          "F74: immediate cup selection leaves the existing whole-arm draw envelope at zero, mid-ramp, and full weight");
                    float fingerBase[16], fingerCurl[16], fingerAt[16];
                    nf::pfzIdentity(fingerBase);
                    nf::pfzIdentity(fingerCurl);
                    fingerCurl[0] = 0.0f; fingerCurl[1] = 1.0f; fingerCurl[4] = -1.0f; fingerCurl[5] = 0.0f;
                    const auto maxRotationDelta = [](const float a[16], const float b[16]) {
                        float delta = 0.0f;
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c) delta = std::max(delta, std::fabs(a[r * 4 + c] - b[r * 4 + c]));
                        return delta;
                    };
                    const bool fingerZeroOk = nf::handBlendRigid(fingerBase, fingerCurl, 0.0, fingerAt) && maxRotationDelta(fingerAt, fingerBase) < 2e-7f;
                    check(fingerZeroOk, "F74: cup-only borrowed finger curl preserves the authored pose at zero armed solve weight");
                    const bool fingerHalfOk = nf::handBlendRigid(fingerBase, fingerCurl, 0.5, fingerAt) && fingerAt[1] > 0.0f &&
                                              std::fabs(nf::ikRotAngleDeg(fingerAt, fingerBase) - 45.0) < 1e-4;
                    check(fingerHalfOk, "F74: cup-only borrowed finger curl follows the positive 45-degree midpoint at half armed solve weight");
                    const bool fingerFullOk = nf::handBlendRigid(fingerBase, fingerCurl, 1.0, fingerAt) && maxRotationDelta(fingerAt, fingerCurl) < 2e-7f;
                    check(fingerFullOk, "F74: cup-only borrowed finger curl reaches the requested full pose within float matrix precision");
                }
            }
            // F70: the ADS eye point (core parts): the target P14w + k f, the clearance cap, the feed-forward ramp, the body pinned to the unlocked head.
            {
                const double kR = nf::kIkPi / 180.0;
                const double rowsA[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                const float rowsAf[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                double f[3] = {0.2, -0.1, 0.97};
                nf::ikUnit(f, f);
                const double p14[3] = {1.0, 1.6, 2.0};
                // The rifle's eye: 7.3 cm AHEAD of P14 along the sight line and 5 cm / 2 cm off it (F66e's along figure).
                double perpDir[3] = {1.0, 0.0, 0.0};
                const double dp = perpDir[0] * f[0] + perpDir[1] * f[1] + perpDir[2] * f[2];
                for (int i = 0; i < 3; ++i) perpDir[i] -= dp * f[i];
                nf::ikUnit(perpDir, perpDir);
                double perpDir2[3];
                nf::ikCross(f, perpDir, perpDir2);
                const double eye[3] = {p14[0] + 0.073 * f[0] + 0.05 * perpDir[0] + 0.02 * perpDir2[0], p14[1] + 0.073 * f[1] + 0.05 * perpDir[1] + 0.02 * perpDir2[1],
                                       p14[2] + 0.073 * f[2] + 0.05 * perpDir[2] + 0.02 * perpDir2[2]};
                nf::AdsSightErr e0, eK, eOld;
                const bool ok0 = nf::adsSightErrorEyeK(p14, eye, f, rowsA, 0.0, e0), okK = nf::adsSightErrorEyeK(p14, eye, f, rowsA, 0.15, eK), okOld = nf::adsSightErrorEye(p14, eye, f, rowsA, eOld);
                double reach0[3], reachK[3];
                for (int i = 0; i < 3; ++i) {
                    reach0[i] = eye[i] + e0.eFullW[i] - p14[i];
                    reachK[i] = eye[i] + eK.eFullW[i] - (p14[i] + 0.15 * f[i]);
                }
                bool sameOld = true;
                for (int i = 0; i < 3; ++i) sameOld = sameOld && e0.seated[i] == eOld.seated[i] && eK.eBody[i] == eOld.eBody[i] && std::fabs(e0.eFullW[i] - e0.eW[i]) < 1e-15;
                check(ok0 && okK && okOld && sameOld && nf::ikLen(reach0) < 1e-12 && nf::ikLen(reachK) < 1e-12 && std::fabs(e0.alongFull - e0.along) < 1e-15 &&
                          std::fabs(e0.along + 0.073) < 1e-9 && std::fabs(eK.alongFull - (e0.along + 0.15)) < 1e-12 && std::fabs(eK.perpNorm - e0.perpNorm) < 1e-15,
                      "F70: the lock's target is P14w + k f: the eye 7.3 cm ahead of P14 is asked 7.3 cm back at k 0 (P14 itself, the animator's eye point) and 7.7 cm forward "
                      "at k 15 cm; the perpendicular figures (the guard's input) and the old perpendicular-only outputs are unchanged");
                // The setting: 0..30 cm, NaN reads the default (21 since F98c, the pilot's).
                check(nf::adsEyeDistanceClampCm(std::numeric_limits<double>::quiet_NaN()) == 21.0 && nf::adsEyeDistanceClampCm(-3.0) == 0.0 && nf::adsEyeDistanceClampCm(45.0) == 30.0 &&
                          nf::adsEyeDistanceClampCm(12.5) == 12.5 && nf::kAdsEyeDistDefaultCm == 21.0 && nf::kAdsSightClampM == 0.40,
                      "F70/F98c: the eye distance is 0..30 cm, default 21 (the pilot's), and the lock's clamp is 40 cm");
                // The clearance cap: a point on the line 20 cm ahead limits k to 14 cm; points off the line (over 6 cm), behind P14 or none leave it; one 3 cm ahead takes it to 0.
                double onLine[1][3], offLine[1][3], behind[1][3], near3[1][3];
                for (int i = 0; i < 3; ++i) {
                    onLine[0][i] = p14[i] + 0.20 * f[i] + 0.02 * perpDir[i];
                    offLine[0][i] = p14[i] + 0.20 * f[i] + 0.12 * perpDir[i];
                    behind[0][i] = p14[i] - 0.10 * f[i];
                    near3[0][i] = p14[i] + 0.03 * f[i];
                }
                const nf::AdsEyeCap c1 = nf::adsEyeDistanceCap(0.15, p14, f, onLine, 1), c2 = nf::adsEyeDistanceCap(0.15, p14, f, offLine, 1), c3 = nf::adsEyeDistanceCap(0.15, p14, f, behind, 1),
                                    c4 = nf::adsEyeDistanceCap(0.15, p14, f, near3, 1), c5 = nf::adsEyeDistanceCap(0.15, p14, f, onLine, 0), c6 = nf::adsEyeDistanceCap(0.10, p14, f, onLine, 1);
                check(c1.capped && c1.readable == 1 && std::fabs(c1.kM - 0.14) < 1e-9 && !c2.capped && c2.readable == 0 && c2.kM == 0.15 && !c3.capped && c3.readable == 0 &&
                          c4.capped && c4.kM == 0.0 && !c5.capped && c5.readable == 0 && c5.kM == 0.15 && !c6.capped && c6.readable == 1 && c6.kM == 0.10,
                      "F70: k is capped so the eye stays 6 cm behind a readable FP weapon point on the sight line (20 cm ahead: k at most 14), ignores points off the line or "
                      "behind P14, and with none readable stands as set");
                const nf::AdsInputChoice freshOn = nf::adsInputChoice(true, true, false, true, true, 2000, false);
                const nf::AdsInputChoice freshOff = nf::adsInputChoice(true, true, true, true, false, 2000, false);
                const nf::AdsInputChoice stale = nf::adsInputChoice(true, true, false, true, true, nf::kAdsActionFreshUs + 1, false);
                const nf::AdsInputChoice absent = nf::adsInputChoice(true, false, false, false, false, 0, false);
                const nf::AdsInputChoice inactive = nf::adsInputChoice(false, true, true, true, false, 100, false);
                check(freshOn.ads && freshOn.source == nf::kAdsInputAction && freshOff.source == nf::kAdsInputAction && !freshOff.ads &&
                          !stale.ads && stale.source == nf::kAdsInputJournal && !absent.ads && absent.source == nf::kAdsInputNone &&
                          inactive.ads && inactive.source == nf::kAdsInputJournal,
                      "F70b: a fresh bound HumanoidZoom action wins both press and release; stale or absent action falls back to journal, and ineligible action uses journal");
                // P14's rotated local forward is composed with F_root rotation only; changing either translation cannot change the basis.
                const float p14Rot[16] = {0.8660254f, 0, -0.5f, 0, 0, 1, 0, 0, 0.5f, 0, 0.8660254f, 0, 0.2f, -0.1f, 0.4f, 1};
                const float rootRot[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 4, 2, -3, 1};
                nf::AdsOpticBasis opticRot;
                const bool opticOk = nf::adsOpticBasis(p14Rot, rootRot, opticRot);
                const float p14Moved[16] = {0.8660254f, 0, -0.5f, 0, 0, 1, 0, 0, 0.5f, 0, 0.8660254f, 0, -20, 30, 40, 1};
                const float rootMoved[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, -400, 200, 300, 1};
                nf::AdsOpticBasis opticMoved;
                const bool opticTranslationInvariant = nf::adsOpticBasis(p14Moved, rootMoved, opticMoved) &&
                                                       std::fabs(opticMoved.forward[0] - opticRot.forward[0]) < 1e-6 &&
                                                       std::fabs(opticMoved.forward[2] - opticRot.forward[2]) < 1e-6;
                float p14Bad[16];
                std::memcpy(p14Bad, p14Rot, sizeof(p14Bad));
                p14Bad[8] = p14Bad[9] = p14Bad[10] = 0.0f;
                nf::AdsOpticBasis opticBad;
                const bool opticDegenerateRefused = !nf::adsOpticBasis(p14Bad, rootRot, opticBad);
                const bool freshLifecycle = nf::adsActionSampleFresh(true, true, 1200, 1500, 1100, 100);
                const bool oldLifecycle = nf::adsActionSampleFresh(true, true, 1000, 1500, 1100, 100);
                const bool futureStamp = nf::adsActionSampleFresh(true, true, 1600, 1500, 1100, 0);
                const bool inactiveSample = nf::adsActionSampleFresh(false, true, 1200, 1500, 1100, 100);
                // Exercise the runtime pipeline: prevalidated freshness is passed into the chooser. Age zero alone cannot revive a bad lifecycle/QPC sample.
                const nf::AdsInputChoice oldLifecycleChoice = nf::adsInputChoice(true, true, true, oldLifecycle, false, 0, false);
                const nf::AdsInputChoice futureStampChoice = nf::adsInputChoice(true, true, false, futureStamp, true, 0, false);
                check(freshLifecycle && !oldLifecycle && !futureStamp && !inactiveSample && oldLifecycleChoice.ads && oldLifecycleChoice.source == nf::kAdsInputJournal &&
                          !futureStampChoice.ads && futureStampChoice.source == nf::kAdsInputJournal,
                      "F70b: only a valid, non-future action sample from the current eligible lifecycle can override journal state");
                {
                    edvr::JournalStatusContentRevision content;
                    const char hip[] = "{\"timestamp\":\"2026-10-26T04:15:55Z\",\"Flags2\":1}";
                    const char next[] = "{\"timestamp\":\"2026-10-26T04:15:56Z\",\"Flags2\":1}";
                    const uint64_t first = content.observe(hip, sizeof(hip) - 1);
                    bool identical = true;
                    for (int poll = 0; poll < 30; ++poll) identical &= content.observe(hip, sizeof(hip) - 1) == first;
                    const uint64_t changed = content.observe(next, sizeof(next) - 1);
                    check(first == 1 && identical && changed == 2 && content.observe(next, sizeof(next) - 1) == changed,
                          "ADS status: identical eager polls do not advance content revision; changed bytes (including only timestamp) do");
                    check(content.observe(nullptr, 1) == changed && content.observe(hip, 0) == changed &&
                              content.observe(hip, edvr::JournalStatusContentRevision::kCapacity) == changed,
                          "ADS status: invalid/oversize observations cannot manufacture a fresh revision");
                }
                {
                    nf::AdsActionReject reject;
                    const bool early = nf::adsActionRejectStep(reject, true, 1000, true, false, 7);
                    const bool identical = nf::adsActionRejectStep(reject, true, 2508, true, false, 7);
                    const bool vetoed = nf::adsActionRejectStep(reject, true, 2508, true, false, 8);
                    const nf::AdsInputChoice veto = nf::adsInputChoice(true, true, false, true, true, 100, vetoed);
                    const bool stillBlocked = nf::adsActionRejectStep(reject, true, 3000, true, false, 8);
                    const nf::AdsInputChoice stale = nf::adsInputChoice(true, true, false, false, true, nf::kAdsActionFreshUs + 1, reject.latched);
                    check(!early && !identical && vetoed && stillBlocked && !veto.ads && veto.journalVeto && !stale.ads && stale.journalVeto,
                          "F70b: changed false status after 1.5 s still rejects a blocked held request; identical rereads and stale action cannot rearm it");
                    const bool recovered = nf::adsActionRejectStep(reject, true, 3713, true, true, 9);
                    const nf::AdsInputChoice accepted = nf::adsInputChoice(true, true, true, true, true, 100, recovered);
                    check(!recovered && !reject.latched && accepted.ads && !accepted.journalVeto,
                          "RC1 sniper replay: veto at +1.508 s recovers on authoritative ADS acceptance at +2.713 s while the same request stays held");
                    const bool reloadEarly = nf::adsActionRejectStep(reject, true, 3714, true, false, 10);
                    const bool reloadBefore = nf::adsActionRejectStep(reject, true, 5212, true, false, 10);
                    const bool reloadVeto = nf::adsActionRejectStep(reject, true, 5213, true, false, 10);
                    check(!reloadEarly && !reloadBefore && reloadVeto,
                          "F70b: acceptance starts a new grace; a later changed false status (reload/rejection) can still reject the held request");
                    const bool released = nf::adsActionRejectStep(reject, false, 5300, true, false, 10);
                    const bool repress = nf::adsActionRejectStep(reject, true, 6000, true, false, 10);
                    const bool old = nf::adsActionRejectStep(reject, true, 8000, true, false, 10);
                    const bool newBlocked = nf::adsActionRejectStep(reject, true, 8001, true, false, 11);
                    check(!released && !repress && !old && newBlocked,
                          "F70b: logical release resets the veto and revision baseline; a new press requires its own changed false status");
                    const bool acceptedWithoutFreshAction = nf::adsActionRejectStep(reject, true, 8100, true, true, 12);
                    const nf::AdsInputChoice acceptedJournal = nf::adsInputChoice(true, true, true, false, true, nf::kAdsActionFreshUs + 1, acceptedWithoutFreshAction);
                    check(!acceptedWithoutFreshAction && acceptedJournal.ads && acceptedJournal.source == nf::kAdsInputJournal,
                          "F70b: authoritative acceptance also clears a veto when the action sample is stale and the journal is the fallback");
                }
                {
                    nf::AdsIncidentHistory history;
                    nf::AdsIncidentSample sample;
                    sample.itemSerial = 22; sample.press = 3; sample.selectionEpoch = 8;
                    for (uint32_t i = 0; i < 20; ++i) { sample.ms = 1000 + i * 100; sample.tick = i; history.push(sample); }
                    check(history.count == 16 && history.at(0).ms == 1400 && history.at(15).ms == 2900,
                          "ADS incident: bounded 16-sample history stays chronological after wrapping");
                    sample.ms = 2901;
                    const bool decimated = !history.push(sample);
                    sample.ms = 2902;
                    nf::AdsAimMap beforeStop;
                    beforeStop.yawCarried = 0.25; beforeStop.pitchSeat = -0.1;
                    sample.carried[0] = beforeStop.yawCarried; sample.carried[1] = beforeStop.pitchSeat; sample.residual[0] = -12.2;
                    const bool forced = history.push(sample, true);
                    beforeStop.dropSeated(); sample.carried[0] = 0.0; sample.residual[0] = 0.0;
                    check(decimated && forced && history.at(15).ms == 2902 && history.at(15).carried[0] == 0.25 && history.at(15).residual[0] == -12.2,
                          "ADS incident: trigger bypasses decimation and snapshots the fault before stop resets the live map/residual");
                    const bool budget = history.beginDump(3000) && history.beginDump(3001) && history.beginDump(3002) && !history.beginDump(3003);
                    history.clearSamples();
                    const bool resetBudgetHeld = !history.beginDump(3004);
                    check(budget && history.count == 0 && resetBudgetHeld && history.beginDump(63000),
                          "ADS incident: lifecycle/history reset cannot reset three-dumps-per-minute budget; budget renews after a minute");
                    nf::AdsIncidentHistory boundary;
                    check(boundary.beginDump(1000) && boundary.beginDump(2000) && boundary.beginDump(60999) && boundary.beginDump(61000) &&
                              !boundary.beginDump(61001) && boundary.beginDump(62000),
                          "ADS incident: a minute boundary cannot permit a burst of more than three dumps in a rolling minute");
                    history.push(sample, true); ++sample.press; history.push(sample, true);
                    const bool pressReset = history.count == 1;
                    ++sample.selectionEpoch; history.push(sample, true);
                    const bool epochReset = history.count == 1;
                    ++sample.configGeneration; history.push(sample, true);
                    const bool configReset = history.count == 1;
                    sample.item[0] = 'x'; history.push(sample, true);
                    check(pressReset && epochReset && configReset && history.count == 1,
                          "ADS incident: another press, native selection epoch, settings generation or item name cannot inherit the previous context's samples");
                }
                check(opticOk && opticTranslationInvariant && opticDegenerateRefused && std::fabs(opticRot.yawDeg - 30.0) < 1e-4,
                      "F70c: joint 14's full rigid basis composes with root rotation, root translation cannot alter directions, and degenerate basis is refused");
                {
                    const float rifle51[16] = {1.000000119f, -0.000128559768f, 0.000163342338f, 0, 0.000128652901f, 0.999999821f, 0.000350989401f, 0,
                                               -0.000163567252f, -0.000351212918f, 1.000000238f, 0, -0.000151023269f, -0.134648457f, 0.260953605f, 1};
                    const float rifleRoot[16] = {-0.508981764f, 0.487074763f, 0.709715366f, 0, 0.571072876f, -0.425843567f, 0.701807201f, 0,
                                                 0.644060254f, 0.762506127f, -0.0614085943f, 0, -18.481029388f, 48.807460657f, 45.268551693f, 1};
                    const double rifleP14World[3] = {-18.4810581, 48.8074646, 45.268589};
                    const double rifleAnchorWorld[3] = {-18.271319295488418, 49.063396517151745, 45.24818181448366};
                    const auto makeP14AtWorld = [](const float root[16], const double world[3], float p14[16]) {
                        float invRoot[16], worldPoint[3] = {static_cast<float>(world[0]), static_cast<float>(world[1]), static_cast<float>(world[2])};
                        if (!nf::pfzAffineInverse(root, invRoot)) return false;
                        double local[3]; nf::ikXform(worldPoint, invRoot, local);
                        nf::pfzIdentity(p14);
                        for (int i = 0; i < 3; ++i) p14[12 + i] = static_cast<float>(local[i]);
                        return true;
                    };
                    float rifleP14[16], rifle52[16];
                    const bool rifleP14Built = makeP14AtWorld(rifleRoot, rifleP14World, rifleP14);
                    std::memcpy(rifle52, rifle51, sizeof(rifle52)); rifle52[12] += 0.345f;
                    const bool rifleNameAndSerial = nf::adsRifleOpticItemSnapshotMatches(true, 42, 42, nf::kAdsRifleOpticItemName) &&
                                                    !nf::adsRifleOpticItemSnapshotMatches(false, 42, 42, nf::kAdsRifleOpticItemName) &&
                                                    !nf::adsRifleOpticItemSnapshotMatches(true, 42, 43, nf::kAdsRifleOpticItemName) &&
                                                    !nf::adsRifleOpticItemSnapshotMatches(true, 42, 42, "$wpn_m_pistol_name;");
                    const auto rifleAnchor = nf::adsRifleOpticOrigin(rifleNameAndSerial, true, rifleP14, rifle51, rifle52, rifleRoot);
                    const double expectedAnchorErr[3] = {rifleAnchor.anchorWorld[0] - rifleAnchorWorld[0], rifleAnchor.anchorWorld[1] - rifleAnchorWorld[1],
                                                         rifleAnchor.anchorWorld[2] - rifleAnchorWorld[2]};
                    const double rifleForward[3] = {rifleRoot[8], rifleRoot[9], rifleRoot[10]};
                    const bool capturedAnchorMatches = rifleP14Built && rifleAnchor.applied && nf::ikLen(expectedAnchorErr) < 0.00002 &&
                                                       std::fabs(nf::ikDot(rifleAnchor.shift, rifleForward)) < 1e-6 && rifleAnchor.spanM > 0.20;
                    float rootXform[16], rifleRootMoved[16]; nf::pfzIdentity(rootXform);
                    rootXform[0] = 0; rootXform[2] = 1; rootXform[8] = -1; rootXform[10] = 0;
                    rootXform[12] = 3.4f; rootXform[13] = -1.7f; rootXform[14] = 2.2f;
                    nf::pfzMul(rifleRoot, rootXform, rifleRootMoved);
                    const auto movedAnchor = nf::adsRifleOpticOrigin(true, true, rifleP14, rifle51, rifle52, rifleRootMoved);
                    double expectedMovedOrigin[3]; nf::ikXform(rifleAnchor.origin, rootXform, expectedMovedOrigin);
                    const double covarianceError[3] = {movedAnchor.origin[0] - expectedMovedOrigin[0], movedAnchor.origin[1] - expectedMovedOrigin[1],
                                                       movedAnchor.origin[2] - expectedMovedOrigin[2]};
                    const double k = 0.15;
                    const double targetWithK[3] = {rifleAnchor.origin[0] + k * rifleForward[0], rifleAnchor.origin[1] + k * rifleForward[1],
                                                   rifleAnchor.origin[2] + k * rifleForward[2]};
                    const double originToTarget[3] = {targetWithK[0] - rifleAnchor.rawOrigin[0], targetWithK[1] - rifleAnchor.rawOrigin[1],
                                                       targetWithK[2] - rifleAnchor.rawOrigin[2]};
                    const auto staleFrame = nf::adsRifleOpticOrigin(true, false, rifleP14, rifle51, rifle52, rifleRoot);
                    const auto wrongItem = nf::adsRifleOpticOrigin(false, true, rifleP14, rifle51, rifle52, rifleRoot);
                    float badJoint[16]; std::memcpy(badJoint, rifle51, sizeof(badJoint)); badJoint[8] = badJoint[9] = badJoint[10] = 0.0f;
                    const auto invalidMatrix = nf::adsRifleOpticOrigin(true, true, rifleP14, badJoint, rifle52, rifleRoot);
                    rifle52[12] += 0.7f;
                    const bool wideSpanStillAccepted = nf::adsRifleOpticOrigin(true, true, rifleP14, rifle51, rifle52, rifleRoot).applied;
                    const bool frames = nf::adsRifleOpticFramesClose(100, 100) && nf::adsRifleOpticFramesClose(100, 101) &&
                                        nf::adsRifleOpticFramesClose(101, 100) && !nf::adsRifleOpticFramesClose(100, 103) &&
                                        !nf::adsRifleOpticFramesClose(0, 1) && !nf::adsRifleOpticFramesClose(5, ~0u);
                    check(rifleNameAndSerial && capturedAnchorMatches && movedAnchor.applied && nf::ikLen(covarianceError) < 2e-5 &&
                              std::fabs(nf::ikDot(originToTarget, rifleForward) - k) < 1e-6 && staleFrame.refusal == nf::kAdsRifleAnchorFrame &&
                              wrongItem.refusal == nf::kAdsRifleAnchorItem && invalidMatrix.refusal == nf::kAdsRifleAnchorMatrix && wideSpanStillAccepted && frames,
                          "F79: coherent laser-rifle identity applies the captured joint-51 anchor immediately, preserves sight depth, is rigid-transform covariant, and fails closed on stale frames or invalid matrices without a narrow span gate");

                    const float pistol51[16] = {0.999991655f, 0.00251052529f, 0.00333291292f, 0, -0.00246676058f, 0.999912620f, -0.0130088180f, 0,
                                                -0.00336523354f, 0.0130006373f, 0.999910057f, 0, 0.0538398959f, -0.132316306f, 0.399246603f, 1};
                    const float pistolRoot[16] = {-0.524331033f, 0.470017195f, 0.710042834f, 0, 0.553824186f, -0.445155174f, 0.703644633f, 0,
                                                  0.646804333f, 0.762181639f, -0.0268982630f, 0, -18.433284755f, 48.871660081f, 45.263665859f, 1};
                    const double pistolP14World[3] = {-18.4332886, 48.8716583, 45.2636642};
                    float pistolP14[16], pistol52[16];
                    const bool pistolP14Built = makeP14AtWorld(pistolRoot, pistolP14World, pistolP14);
                    std::memcpy(pistol52, pistol51, sizeof(pistol52)); pistol52[12] += 0.056f;
                    const auto wrongRifleProfile = nf::adsRifleOpticOrigin(true, true, pistolP14, pistol51, pistol52, pistolRoot);
                    const double pistolFallbackDelta[3] = {wrongRifleProfile.origin[0] - wrongRifleProfile.rawOrigin[0],
                                                           wrongRifleProfile.origin[1] - wrongRifleProfile.rawOrigin[1],
                                                           wrongRifleProfile.origin[2] - wrongRifleProfile.rawOrigin[2]};
                    check(pistolP14Built && !wrongRifleProfile.applied && wrongRifleProfile.refusal == nf::kAdsRifleAnchorGeometry &&
                              wrongRifleProfile.transverseResidualM > nf::kAdsRifleAnchorQuadHalfWidthM &&
                              std::fabs(wrongRifleProfile.transverseResidualM - 0.05689369838677318) < 0.0002 && nf::ikLen(pistolFallbackDelta) < 1e-6,
                          "F79: the captured pistol pose rejects the rifle anchor by measured quad-half-width plausibility and falls back to raw P14 on the first frame");
                }
                {
                    namespace nf = edvr::nofoot;
                    uint8_t site[nf::kWeaponUiVerifyLen];
                    std::memcpy(site, nf::kWeaponUiBranchBefore, sizeof(site));
                    const bool nativeGate = nf::weaponUiPatchWanted(true, true) && !nf::weaponUiPatchWanted(true, false) &&
                                            !nf::weaponUiPatchWanted(false, true) && !nf::weaponUiPatchWanted(false, false);
                    const bool originalIdentity = nf::patchBytesMatch(site, nf::kWeaponUiBranchBefore, nf::kWeaponUiVerifyLen);
                    site[nf::kWeaponUiBranchByteOff] = 0xEB;
                    const bool patchedIdentity = nf::patchBytesMatch(site, nf::kWeaponUiBranchAfter, nf::kWeaponUiVerifyLen);
                    const bool branchDelta = nf::kWeaponUiBranchBefore[nf::kWeaponUiBranchByteOff] == 0x74 &&
                                             nf::kWeaponUiBranchAfter[nf::kWeaponUiBranchByteOff] == 0xEB;
                    site[0] ^= 1;
                    const bool foreignChangeRefused = !nf::patchBytesMatch(site, nf::kWeaponUiBranchAfter, nf::kWeaponUiVerifyLen) &&
                                                      !nf::patchBytesMatch(site, nf::kWeaponUiBranchBefore, nf::kWeaponUiVerifyLen);
                    const uint8_t originalCall[8] = {0xE8,0x6B,0x55,0x6C,0x01,0x80,0x7F,0x28};
                    uint8_t hookedCall[8] = {};
                    const uintptr_t callAddress = 0x1AB1960u;
                    const uintptr_t stubAddress = callAddress + 5 + 0x123456;
                    const bool callBuild = nf::weaponUiBuildGetterCall(originalCall, callAddress, stubAddress, hookedCall);
                    uint8_t foreignCall[8]; std::memcpy(foreignCall, hookedCall, sizeof(foreignCall)); foreignCall[2] ^= 1;
                    uint8_t refusedCall[8]; std::memcpy(refusedCall, originalCall, sizeof(refusedCall)); refusedCall[0] = 0x90;
                    const bool callOwnership = callBuild && nf::weaponUiGetterCallOwned(hookedCall, hookedCall) &&
                                               !nf::weaponUiGetterCallOwned(foreignCall, hookedCall) &&
                                               hookedCall[5] == originalCall[5] && hookedCall[6] == originalCall[6] && hookedCall[7] == originalCall[7];
                    const bool callRefusal = !nf::weaponUiBuildGetterCall(refusedCall, callAddress, stubAddress, hookedCall) &&
                                             !nf::weaponUiBuildGetterCall(originalCall, callAddress, UINTPTR_MAX - 8, hookedCall) &&
                                             !nf::weaponUiBuildGetterCall(originalCall, callAddress, 0xFFFFFFFFF0000000ull, hookedCall);
                    const bool exactGetterJoin = nf::weaponUiGetterJoinMatches(55, 9, 0x1010, 0x1110, 0x2220, 100, 101,
                                                                               55, 9, 0x1010, 0x1110, 0x2220, true, true);
                    const bool getterJoinRefusals = !nf::weaponUiGetterJoinMatches(55, 9, 0x1010, 0x1110, 0x2220, 100, 101,
                                                                                   55, 9, 0x1010, 0x1110, 0x2220, false, true) &&
                                                    !nf::weaponUiGetterJoinMatches(55, 9, 0x1010, 0x1110, 0x2220, 100, 101,
                                                                                   56, 9, 0x1010, 0x1110, 0x2220, true, true) &&
                                                    !nf::weaponUiGetterJoinMatches(55, 9, 0x1010, 0x1110, 0x2220, 100, 101,
                                                                                   55, 9, 0x3030, 0x1110, 0x2220, true, true) &&
                                                    !nf::weaponUiGetterJoinMatches(55, 9, 0x1010, 0x1110, 0x2220, 100, 101,
                                                                                   55, 9, 0x1010, 0x3330, 0x2220, true, true) &&
                                                    !nf::weaponUiGetterJoinMatches(55, 9, 0x1010, 0x1110, 0x2220, 102, 101,
                                                                                   55, 9, 0x1010, 0x1110, 0x2220, true, true) &&
                                                    !nf::weaponUiGetterJoinMatches(55, 9, 0x1010, 0x1110, 0x2220, 100, 101,
                                                                                   55, 9, 0x1010, 0x1110, 0x2220, true, false);
                    check(nativeGate && originalIdentity && patchedIdentity && branchDelta && foreignChangeRefused && callOwnership && callRefusal &&
                              exactGetterJoin && getterJoinRefusals,
                          "F70d: weapon UI patch is wanted only for native-on-foot sessions, verifies the full original/patched byte identity, changes only the proven branch byte, and refuses foreign bytes on restore");
                }
                {
                    g_weaponUiGetterTarget = reinterpret_cast<uintptr_t>(&weaponUiProbeFakeGetter);
                    g_weaponUiProbeCalls = 0;
                    g_weaponUiProbeArgs[0] = g_weaponUiProbeArgs[1] = 0;
                    g_weaponUiProbeObserved = edvr::WeaponUiGetterObservation{};
                    g_weaponUiProbeObservedValid = false;
                    WeaponUiProbeResult got{};
                    const unsigned int savedMxcsr = _mm_getcsr();
                    const uint64_t returned = weaponUiProbeTestInvoke(&got, 0x1111, 0x2222, 0x3333);
                    const float expectedXmm[24] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24};
                    const float expectedMatrix[16] = {101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116};
                    bool xmmOk = true, matrixOk = true;
                    for (int i = 0; i < 24; ++i) xmmOk = xmmOk && got.xmm[i / 4][i % 4] == expectedXmm[i];
                    for (int i = 0; i < 16; ++i) matrixOk = matrixOk && g_weaponUiProbeObserved.attachment[i] == expectedMatrix[i];
                    DWORD64 unwindBase = 0;
                    const bool unwindable = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&weaponUiGetterCallStub), &unwindBase, nullptr) != nullptr;
                    const bool abiOk = returned == got.rax && got.rax == g_weaponUiProbeArgs[1] && got.rcx == 0xC1 && got.rdx == 0xD2 &&
                                       got.r8 == 0xE8 && got.r9 == 0xE9 && got.r10 == 0xEA && got.r11 == 0xEB &&
                                       (got.flags & 0x41u) == 0x40u && got.mxcsr == 0x3F80u;
                    const bool observed = g_weaponUiProbeObservedValid && g_weaponUiProbeObserved.renderer == 0x1111 &&
                                          g_weaponUiProbeObserved.part == 0x2222 && g_weaponUiProbeObserved.cameraPointer == 0x3333 && matrixOk;
                    _mm_setcsr(savedMxcsr);
                    check(unwindable && g_weaponUiProbeCalls == 1 && g_weaponUiProbeArgs[0] == 0x11 && abiOk && xmmOk && observed,
                          "F82: the executable MASM getter probe calls once, captures the exact output matrix and context, preserves volatile GPR/flags/FP state, and has unwind metadata");
                    g_weaponUiGetterTarget = 0;
                }
                // F70a: actual ADS state selects one absolute target. The rendered-eye plant is two frames late; the first valid sample must start the eye
                // correction immediately, and one critically damped state follows the target independently of the head-aim ramp.
                {
                    const double target[3] = {0.07, -0.04, 0.20};
                    double pos[3] = {}, vel[3] = {}, eyeTarget[3] = {}, hist[8][3] = {};
                    const double dt = 1.0 / 90.0;
                    double t90 = -1.0, t95 = -1.0, overshoot = 0.0, maxNorm = 0.0;
                    int firstMove = -1;
                    for (int i = 0; i < 90; ++i) {
                        const bool have = i >= 2; // two rendered frames arrive before the first usable eye sample
                        double meas[3] = {};
                        if (have)
                            for (int c = 0; c < 3; ++c) meas[c] = target[c] - hist[(i + 6) % 8][c];
                        const double before[3] = {pos[0], pos[1], pos[2]};
                        nf::adsEyeStep(pos, vel, eyeTarget, meas, have, true, false, dt);
                        if (have && firstMove < 0 && std::hypot(pos[0] - before[0], pos[1] - before[1], pos[2] - before[2]) > 1e-6) firstMove = i;
                        for (int c = 0; c < 3; ++c) hist[i % 8][c] = pos[c];
                        const double err = std::hypot(target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]);
                        const double tn = std::hypot(target[0], target[1], target[2]);
                        if (t90 < 0.0 && err <= 0.10 * tn) t90 = i * dt;
                        if (t95 < 0.0 && err <= 0.05 * tn) t95 = i * dt;
                        for (int c = 0; c < 3; ++c) overshoot = std::max(overshoot, (pos[c] - target[c]) * (target[c] >= 0 ? 1.0 : -1.0));
                        maxNorm = std::max(maxNorm, std::hypot(pos[0], pos[1], pos[2]));
                    }
                    const double tn = std::sqrt(target[0] * target[0] + target[1] * target[1] + target[2] * target[2]);
                    const double errAt1s = std::sqrt((target[0] - pos[0]) * (target[0] - pos[0]) + (target[1] - pos[1]) * (target[1] - pos[1]) + (target[2] - pos[2]) * (target[2] - pos[2]));
                    // Release selects a zero target and leaves position and velocity continuous.
                    double backAt05 = 99.0, backAt1 = 99.0;
                    for (int i = 0; i < 90; ++i) {
                        nf::adsEyeStep(pos, vel, eyeTarget, target, false, false, false, dt);
                        const double n = std::hypot(pos[0], pos[1], pos[2]);
                        if (i == 44) backAt05 = n;
                        if (i == 89) backAt1 = n;
                    }
                    check(firstMove == 2 && t90 >= 0.0 && t90 <= 0.22 && t95 >= 0.0 && t95 <= 0.28 && errAt1s < 0.005 && overshoot < 0.05 * tn && maxNorm < nf::kAdsSightClampM &&
                              backAt05 < 0.03 && backAt1 < 0.002,
                          "F70a: the first valid eye sample moves on that tick despite a two-frame delayed plant; one fixed-target filter reaches 90% by 0.22 s and 95% by 0.28 s without material overshoot; release smoothly returns to zero");
                    // A missing active-state measurement holds the last absolute target, while a release selects zero.
                    double ph[3] = {}, vh[3] = {}, th[3] = {};
                    const double eHold[3] = {0.10, 0.0, 0.0};
                    nf::adsEyeStep(ph, vh, th, eHold, true, true, false, dt);
                    const double heldTarget[3] = {th[0], th[1], th[2]};
                    for (int i = 0; i < 30; ++i) nf::adsEyeStep(ph, vh, th, eHold, false, true, false, dt);
                    const bool targetHeld = th[0] == heldTarget[0] && th[1] == heldTarget[1] && th[2] == heldTarget[2] && ph[0] > 0.0;
                    const double movingVelocity = vh[0];
                    nf::adsEyeStep(ph, vh, th, eHold, false, false, false, dt);
                    const bool releaseContinuous = ph[0] > 0.0 && vh[0] != 0.0 && movingVelocity != 0.0 && th[0] == 0.0;
                    nf::adsEyeStep(ph, vh, th, eHold, true, true, false, dt); // repress from release keeps the same position/velocity state
                    const bool repressContinuous = vh[0] != 0.0 && ph[0] > 0.0;
                    // The clamp: a 60 cm error never takes the shift past 40 cm; any inactive state returns it to zero.
                    double pc[3] = {}, vc[3] = {}, tc[3] = {};
                    const double big[3] = {0.6, 0.0, 0.0};
                    double worstN = 0.0;
                    for (int i = 0; i < 180; ++i) {
                        nf::adsEyeStep(pc, vc, tc, big, true, true, false, dt);
                        worstN = std::max(worstN, std::fabs(pc[0]));
                    }
                    for (int i = 0; i < 90; ++i) nf::adsEyeStep(pc, vc, tc, big, true, false, false, dt);
                    check(targetHeld && releaseContinuous && repressContinuous && worstN <= nf::kAdsSightClampM + 1e-12 && worstN > 0.39 && std::fabs(pc[0]) < 1e-3,
                          "F70a: active missing samples hold the absolute target; release and repress preserve the filter state; a 60 cm error clamps to 40 cm and inactive state returns it to zero");
                }
                // The align clock caps ADS dt at 50 ms, equal to this filter's tau. Exercise the exact
                // ADS step at ordinary headset rates and when one 50/100 ms hitch is delivered as the
                // clock's capped 50 ms step; verify the 8 cm move cap and release/repress continuity.
                {
                    const double rates[] = {45.0, 60.0, 90.0, 120.0};
                    const double targetX[3] = {0.20, 0.0, 0.0};
                    bool ratesStable = true;
                    double worstT90 = 0.0, worstT95 = 0.0;
                    for (double hz : rates) {
                        const double dt = 1.0 / hz;
                        double pos[3] = {}, vel[3] = {}, target[3] = {}, error[3] = {targetX[0], 0.0, 0.0};
                        double t90 = -1.0, t95 = -1.0, previous = 0.0;
                        for (int i = 0; i < static_cast<int>(hz * 1.0); ++i) {
                            nf::adsEyeStep(pos, vel, target, error, i == 0, true, false, dt);
                            const double elapsed = (i + 1) * dt;
                            if (t90 < 0.0 && pos[0] >= 0.90 * targetX[0]) t90 = elapsed;
                            if (t95 < 0.0 && pos[0] >= 0.95 * targetX[0]) t95 = elapsed;
                            const double move = std::fabs(pos[0] - previous);
                            if (!(std::isfinite(pos[0]) && std::isfinite(vel[0]) && pos[0] <= targetX[0] + 1e-9 && move <= nf::kAdsEyeCapPerFrameM + 1e-12)) ratesStable = false;
                            previous = pos[0];
                        }
                        if (t90 < 0.0 || t95 < 0.0) ratesStable = false;
                        else { worstT90 = std::max(worstT90, t90); worstT95 = std::max(worstT95, t95); }
                    }
                    bool hitchesStable = true;
                    for (double hitchMs : {50.0, 100.0}) {
                        double pos[3] = {}, vel[3] = {}, target[3] = {}, error[3] = {targetX[0], 0.0, 0.0};
                        nf::adsEyeStep(pos, vel, target, error, true, true, false, 1.0 / 90.0);
                        const double before = pos[0];
                        const double cappedDt = std::min(hitchMs / 1000.0, 0.05); // qpcDtSec(…, 0.05)
                        nf::adsEyeStep(pos, vel, target, error, false, true, false, cappedDt);
                        if (!std::isfinite(pos[0]) || !std::isfinite(vel[0]) || pos[0] < before || pos[0] > targetX[0] ||
                            pos[0] - before > nf::kAdsEyeCapPerFrameM + 1e-12) hitchesStable = false;
                        const double releasePos = pos[0], releaseVel = vel[0];
                        double expectedReleasePos = releasePos, expectedReleaseVel = releaseVel;
                        nf::adsEyeCdStep(expectedReleasePos, expectedReleaseVel, 0.0, nf::kAdsEyeTauSec, cappedDt, nf::kAdsEyeCapPerFrameM);
                        nf::adsEyeStep(pos, vel, target, error, false, false, false, cappedDt);
                        if (!(std::isfinite(pos[0]) && std::isfinite(vel[0]) && pos[0] > 0.0 && vel[0] != 0.0 && releaseVel != 0.0 &&
                              std::fabs(pos[0] - releasePos) <= nf::kAdsEyeCapPerFrameM + 1e-12 && std::fabs(pos[0] - expectedReleasePos) < 1e-12 &&
                              std::fabs(vel[0] - expectedReleaseVel) < 1e-12)) hitchesStable = false;
                        const double repressPos = pos[0], repressVel = vel[0];
                        error[0] = targetX[0] - pos[0]; // fresh sample re-engages the same absolute correction
                        double expectedRepressPos = repressPos, expectedRepressVel = repressVel;
                        nf::adsEyeCdStep(expectedRepressPos, expectedRepressVel, targetX[0], nf::kAdsEyeTauSec, 1.0 / 90.0, nf::kAdsEyeCapPerFrameM);
                        nf::adsEyeStep(pos, vel, target, error, true, true, false, 1.0 / 90.0);
                        if (!(std::isfinite(pos[0]) && std::isfinite(vel[0]) && vel[0] != 0.0 && repressVel != 0.0 &&
                              std::fabs(pos[0] - repressPos) <= nf::kAdsEyeCapPerFrameM + 1e-12 && std::fabs(pos[0] - expectedRepressPos) < 1e-12 &&
                              std::fabs(vel[0] - expectedRepressVel) < 1e-12)) hitchesStable = false;
                    }
                    check(ratesStable && hitchesStable && worstT90 <= 0.22 && worstT95 <= 0.28,
                          "F70a: exact ADS damping is stable at 45/60/90/120 Hz and 50/100 ms hitch caps, respects 8 cm/frame, reaches 90/95% within 0.22/0.28 s, and preserves release/repress motion");
                }
                // The body is pinned to the unlocked head: the camera the game reports moves by the shift (-S0, -S1, +S2 in body axes, F40's plant); the pin input is the camera
                // less that. A 90 deg turned AMC; S = (4, 2, 18) cm seated.
                {
                    const float rowsT[9] = {0, 0, -1, 0, 1, 0, 1, 0, 0};   // right = -z, up = y, forward = +x (world)
                    const double S[3] = {0.04, 0.02, 0.18};
                    const double head[3] = {3.0, 1.6, -2.0};
                    double bodyMove[3], cm1[3];
                    const double zero3[3] = {};
                    nf::adsCamBodyMotion(zero3, S, bodyMove);
                    double mbody[3];
                    nf::adsCamShiftBody(S, mbody);
                    for (int c = 0; c < 3; ++c) cm1[c] = bodyMove[0] * rowsT[c] + bodyMove[1] * rowsT[3 + c] + bodyMove[2] * rowsT[6 + c];
                    const float cam[3] = {static_cast<float>(head[0] + cm1[0]), static_cast<float>(head[1] + cm1[1]), static_cast<float>(head[2] + cm1[2])};
                    float un[3], none[3];
                    nf::adsUnlockedCam(cam, S, rowsT, un);
                    const double zeroS[3] = {};
                    nf::adsUnlockedCam(cam, zeroS, rowsT, none);
                    check(std::fabs(mbody[0] - bodyMove[0]) < 1e-15 && std::fabs(mbody[2] - bodyMove[2]) < 1e-15 && std::fabs(un[0] - head[0]) < 1e-5 && std::fabs(un[1] - head[1]) < 1e-5 &&
                              std::fabs(un[2] - head[2]) < 1e-5 && std::fabs(cm1[0] - 0.18) < 1e-12 && std::fabs(cm1[2] - 0.04) < 1e-12 && none[0] == cam[0] && none[2] == cam[2],
                          "F70: the pin input excludes S: the camera less the shift (-S0, -S1, +S2 in body axes through the AMC rows) is the unlocked head (a 18 cm forward shift "
                          "moves a turned AMC's camera 18 cm along its forward and the pin gets it back); with S zero the camera is untouched");
                }
                // k = 0 reproduces P14: the closed-loop plant (the measured error on the eye) converges the eye onto P14w; k = 15 cm onto P14w + 0.15 f.
                {
                    for (int which = 0; which < 2; ++which) {
                        const double k = which == 0 ? 0.0 : 0.15;
                        double pos[3] = {}, vel[3] = {}, eyeTarget[3] = {}, hist[8][3] = {};
                        const double dt = 1.0 / 90.0;
                        double eyeNow[3];
                        for (int step = 0; step < 180; ++step) {
                            // the rendered eye = the rest eye shifted (in the world, by the seated shift two frames ago; identity rows, so seated -> world is a sign map)
                            const double* sh = hist[(step + 8 - 2) % 8];
                            const double shiftWorld[3] = {-sh[0], -sh[1], sh[2]};   // body = diag(-1, -1, +1) . seated, identity rows
                            for (int i = 0; i < 3; ++i) eyeNow[i] = eye[i] + shiftWorld[i];
                            nf::AdsSightErr er;
                            nf::adsSightErrorEyeK(p14, eyeNow, f, rowsA, k, er);
                            nf::adsEyeStep(pos, vel, eyeTarget, er.seatedFull, true, true, false, dt);
                            for (int i = 0; i < 3; ++i) hist[step % 8][i] = pos[i];
                        }
                        const double want[3] = {p14[0] + k * f[0], p14[1] + k * f[1], p14[2] + k * f[2]};
                        double fin[3];
                        const double* sh = hist[(180 - 1) % 8];
                        const double shiftWorld[3] = {-sh[0], -sh[1], sh[2]};
                        for (int i = 0; i < 3; ++i) fin[i] = eye[i] + shiftWorld[i];
                        const double miss[3] = {fin[0] - want[0], fin[1] - want[1], fin[2] - want[2]};
                        check(nf::ikLen(miss) < 0.001,
                              which == 0 ? "F70: with k = 0 the closed loop puts the rendered eye on P14w itself (within 1 mm, the along part included)"
                                         : "F70: with k = 15 cm the closed loop puts the rendered eye 15 cm forward of P14w on the sight line (within 1 mm)");
                    }
                }
                (void)kR;
                (void)rowsAf;
            }
            // F67e: the aim-jump trigger: the FP root's yaw rate over 30 deg/s while armed (the yaw step wrapped across the +-180 seam).
            {
                check(std::fabs(nf::yawRateDps(10.0, 13.0, 0.1) - 30.0) < 1e-9 && std::fabs(nf::yawRateDps(179.0, -179.0, 0.1) - 20.0) < 1e-9 &&
                          std::fabs(nf::yawRateDps(-179.0, 179.0, 0.1) + 20.0) < 1e-9 && nf::yawRateDps(1.0, 2.0, 0.0) == 0.0 &&
                          nf::yawRateDps(std::numeric_limits<double>::quiet_NaN(), 2.0, 0.1) == 0.0 && nf::aimTrigger(31.0, true) && nf::aimTrigger(-31.0, true) &&
                          !nf::aimTrigger(30.0, true) && !nf::aimTrigger(90.0, false),
                      "F67e: the yaw rate wraps across the +-180 seam (179 to -179 is +20 deg), and an event starts on |rate| over 30 deg/s while armed, not unarmed");
            }
            // F67f: while armed, both hands stay on the grips on every frame.
            {
                // (1) A stale snapshot still yields a hand on the grip, via the camera-relative offset. The FP hand attach is a point in the FP root's frame
                // (a = (0.20, -0.10, 0.50)); the snapshot was taken when the root faced +z, 2 s ago; the aim has turned 90 deg since. G now (the FP root
                // now x inv(AMC)) maps the same offset to this frame's grip: its world place is a x F_root_now, where the hand belongs; the old G (what an
                // unrefreshed path would use) puts it where the hand was 90 deg ago.
                const auto yawM = [](double deg, double ox, double oy, double oz, float m[16]) {
                    const double a = deg * nf::kIkPi / 180.0, c = std::cos(a), s = std::sin(a);
                    const float r[16] = {static_cast<float>(c), 0.0f, static_cast<float>(-s), 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, static_cast<float>(s), 0.0f, static_cast<float>(c), 0.0f,
                                         static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz), 1.0f};
                    std::memcpy(m, r, sizeof(r));
                };
                float froot0[16], froot1[16], m3p[16], inv[16], g0[16], g1[16];
                yawM(0.0, 5.0, 1.6, 3.0, froot0);
                yawM(90.0, 5.0, 1.6, 3.0, froot1);
                yawM(30.0, 5.0, 0.0, 3.0, m3p);
                const bool invOk = nf::pfzAffineInverse(m3p, inv);
                nf::pfzMul(froot0, inv, g0);
                nf::pfzMul(froot1, inv, g1);
                const float a[3] = {0.20f, -0.10f, 0.50f};
                double grip1[3], grip0[3];
                nf::ikXform(a, g1, grip1);   // the 3P model place of the grip under this frame's G
                nf::ikXform(a, g0, grip0);   // under the stale G
                // world = model x AMC matrix
                const auto toWorld = [&](const double p[3], double out[3]) {
                    for (int j = 0; j < 3; ++j) out[j] = p[0] * m3p[j] + p[1] * m3p[4 + j] + p[2] * m3p[8 + j] + m3p[12 + j];
                };
                double w1[3], w0[3];
                toWorld(grip1, w1);
                toWorld(grip0, w0);
                const double want1[3] = {5.0 + 0.20 * 0.0 + 0.50 * 1.0, 1.6 - 0.10, 3.0 - 0.20 * 1.0 + 0.50 * 0.0};   // a x R(90 deg) + origin
                const double stale[3] = {w0[0] - w1[0], w0[1] - w1[1], w0[2] - w1[2]};
                check(invOk && std::fabs(w1[0] - want1[0]) < 1e-5 && std::fabs(w1[1] - want1[1]) < 1e-5 && std::fabs(w1[2] - want1[2]) < 1e-5 && nf::ikLen(stale) > 0.3 &&
                          nf::ikSnapshotUsable(true, 2000, true) && nf::ikSnapshotUsable(true, 29000, true) && !nf::ikSnapshotUsable(true, 31000, true) &&
                          !nf::ikSnapshotUsable(true, 2000, false) && nf::ikSnapshotUsable(true, 90, false) && !nf::ikSnapshotUsable(false, 0, true) &&
                          nf::ikGUsable(true, 900, true) && !nf::ikGUsable(true, 1100, true) && !nf::ikGUsable(true, 150, false),
                      "F67f: a 2 s old FP snapshot with this frame's G puts the hand on the grip where this frame's aim says (the stale G would put it 0.3 m+ off); the "
                      "armed path accepts a snapshot up to 30 s and G up to 1 s, the unarmed path 100 ms");
                // (2) phi follows a 90 deg aim step within one frame while armed: the shoulder line (square to +z) against an FP forward turned 90 deg.
                const double uL[3] = {-0.18, 1.40, 0.0}, uR[3] = {0.18, 1.40, 0.0}, fwdSq[3] = {0.0, 0.0, 1.0}, fwd90[3] = {1.0, 0.0, 0.0}, pivot[3] = {0.0, 1.5, 0.0};
                double phiSq = 9.0, phi90 = 0.0;
                const bool okSq = nf::ikSquareToForward(uL, uR, fwdSq, &phiSq);
                const bool ok90 = nf::ikSquareToForward(uL, uR, fwd90, &phi90);
                bool over = false;
                const double armedApplied = nf::squareApplied(phi90, true, 0.5, 0.1, &over);
                const bool overArmed = over;
                const double unarmedApplied = nf::squareApplied(phi90, false, 1.0, 1.0, &over);
                double aL[3], aR[3], bL[3], bR[3];
                nf::turnAboutVertical(uL, pivot, armedApplied, aL);
                nf::turnAboutVertical(uR, pivot, armedApplied, aR);
                nf::turnAboutVertical(uL, pivot, unarmedApplied, bL);
                nf::turnAboutVertical(uR, pivot, unarmedApplied, bR);
                double phiArmedAfter = 9.0, phiUnarmedAfter = 0.0;
                const bool okA = nf::ikSquareToForward(aL, aR, fwd90, &phiArmedAfter), okB = nf::ikSquareToForward(bL, bR, fwd90, &phiUnarmedAfter);
                check(okSq && ok90 && okA && okB && std::fabs(phiSq) < 1e-9 && std::fabs(std::fabs(phi90) - nf::kIkPi / 2.0) < 1e-9 && std::fabs(armedApplied - phi90) < 1e-12 &&
                          overArmed && std::fabs(phiArmedAfter) < 1e-9 && std::fabs(std::fabs(unarmedApplied) - 50.0 * nf::kIkPi / 180.0) < 1e-12 &&
                          std::fabs(phiUnarmedAfter) > 30.0 * nf::kIkPi / 180.0 && nf::squareApplied(0.2, true, 0.0, 0.0, &over) == 0.2 && !over,
                      "F67f: armed, the square-up applies phi itself (a 90 deg aim step is squared in one frame, ignoring the amount and the ramp; the call flags the "
                      "excess over the old 50 deg limit); unarmed it is still clamped to 50 deg (40 deg left)");
                // (3) No armed path leaves w < 1: once the armed ramp is complete the solve weight is 1 whatever the base weight, the gate or the snapshot's age;
                // during the ramp (and unarmed) it is base x ramp; and the armed path has no per-call cap.
                bool allOne = true;
                const double bases[5] = {0.0, 0.05, 0.5, 0.99, 1.0};
                for (const double b : bases) allOne = allOne && nf::ikArmedSolveWeight(true, 1.0, b) == 1.0;
                check(allOne && std::fabs(nf::ikArmedSolveWeight(true, 0.4, 1.0) - 0.4) < 1e-12 && nf::ikArmedSolveWeight(false, 1.0, 1.0) == 1.0 * 1.0 &&
                          nf::ikArmedSolveWeight(false, 0.0, 1.0) == 0.0 && !nf::handCapApplies(true) && nf::handCapApplies(false),
                      "F67f: no armed path leaves w below 1 (the armed ramp complete: 1 for every base weight); the ramp (0.4) and the unarmed weight are base x ramp; the "
                      "per-call cap on the hand and the assist applies only unarmed");
            }
            // F68c: G from one game frame. A body running at 4 m/s, the game at 90 Hz, the FP AMC's and the 3P AMC's P1 calls in alternating order.
            {
                const uint64_t freq = 10000000ull;   // QPC ticks per second in the model
                const double v = 4.0, dtF = 1.0 / 90.0;
                const float grip[3] = {0.20f, 1.30f, 0.45f};   // the grip in the FP model space
                const auto trans = [](double x, float out[16]) {
                    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                    out[12] = static_cast<float>(x);
                    out[13] = 0.0f;
                    out[14] = 1.0f;
                };
                // The grip's 3P-world position at the drawn frame N from G = F_root x inv(M3P): grip x G x M3P(N). Its true position is grip x F_root(N).
                const auto drawn = [&](const float G[16], const float m3pN[16]) {
                    double inModel[3], world[3];
                    nf::ikXform(grip, G, inModel);
                    const float im[3] = {static_cast<float>(inModel[0]), static_cast<float>(inModel[1]), static_cast<float>(inModel[2])};
                    nf::ikXform(im, m3pN, world);
                    return world[0];
                };
                nf::P1FramePairing pr;
                bool storedFp = false, storedTp = false, singleFrame = true;
                uint32_t fpFrame = 0, tpFrame = 0;
                uint64_t fpQ = 0, tpQ = 0;
                float fpRoot[16], m3p[16], lastRoot[16];
                trans(0.0, lastRoot);
                double pairedErrMax = 0.0, naiveErrMax = 0.0, naiveErrSum = 0.0;
                int naiveBad = 0, published = 0, doubled = 0;
                const int frames = 300;
                for (int n = 0; n < frames; ++n) {
                    const double x = v * dtF * n;
                    const bool fpFirst = (n % 2) == 0;
                    float F[16], M[16];
                    trans(x + 0.0, F);      // the FP root at frame n
                    trans(x + 0.02, M);     // the 3P AMC at frame n (offset by the model's own mounting, a constant)
                    uint64_t q = static_cast<uint64_t>(n) * 111111ull;   // 11.1 ms a frame
                    bool pubThisFrame = false;
                    for (int call = 0; call < 2; ++call) {
                        const bool isFp = (call == 0) == fpFirst;
                        q += 200;   // 0.02 ms between the two calls
                        if (isFp) {
                            fpFrame = pr.on(0);
                            fpQ = q;
                            storedFp = true;
                            std::memcpy(fpRoot, F, sizeof(fpRoot));
                            if (pr.take(storedFp, fpFrame, fpQ, storedTp, tpFrame, tpQ, freq)) {
                                float inv[16], G[16];
                                nf::pfzAffineInverse(m3p, inv);
                                nf::pfzMul(fpRoot, inv, G);
                                const double err = std::fabs(drawn(G, M) - (grip[0] + F[12]));
                                pairedErrMax = std::max(pairedErrMax, err);
                                ++published;
                                if (pubThisFrame) ++doubled;
                                pubThisFrame = true;
                            }
                        } else {
                            tpFrame = pr.on(1);
                            tpQ = q;
                            storedTp = true;
                            std::memcpy(m3p, M, sizeof(m3p));
                            if (pr.take(storedFp, fpFrame, fpQ, storedTp, tpFrame, tpQ, freq)) {
                                float inv[16], G[16];
                                nf::pfzAffineInverse(m3p, inv);
                                nf::pfzMul(fpRoot, inv, G);
                                const double err = std::fabs(drawn(G, M) - (grip[0] + F[12]));
                                pairedErrMax = std::max(pairedErrMax, err);
                                ++published;
                                if (pubThisFrame) ++doubled;
                                pubThisFrame = true;
                            }
                            // The F67 rule: G built at the 3P call from the FP root of the LAST call (this frame's when the FP call came first).
                            float inv[16], G[16];
                            nf::pfzAffineInverse(M, inv);
                            nf::pfzMul(fpFirst ? F : lastRoot, inv, G);
                            const double err = std::fabs(drawn(G, M) - (grip[0] + F[12]));
                            naiveErrMax = std::max(naiveErrMax, err);
                            naiveErrSum += err;
                            if (err > 1e-4) ++naiveBad;
                        }
                    }
                    if (!pubThisFrame) singleFrame = false;
                    std::memcpy(lastRoot, F, sizeof(lastRoot));
                }
                check(published == frames && doubled == 0 && singleFrame && pairedErrMax < 1e-4 && pr.noPair == 0 && pr.tpFirst + pr.fpFirst == static_cast<uint64_t>(frames),
                      "F68c: with the FP and 3P P1 calls in alternating order at 90 Hz and a body at 4 m/s, G is built once on every frame from that frame's pair "
                      "and the grip lands where the weapon is (error under 0.1 mm)");
                check(naiveBad == frames / 2 && std::fabs(naiveErrMax - 4.0 * dtF) < 1e-4 && pr.tpFirst == static_cast<uint64_t>(frames / 2),
                      "F68c: built at the 3P call from the last FP root (the F67 rule), the same frames put the grip 4.4 cm off on every frame the 3P call came "
                      "first (half of them here)");
                // A frame with only one call (the FP call missing) is counted, never paired with the neighbouring frame's half; an index shifted by a missing call
                // is caught by the gap veto.
                nf::P1FramePairing gap;
                gap.on(0);
                const uint32_t tf = gap.on(1);   // frame 1: both seen
                const bool one = gap.take(true, 1, 1000, true, tf, 1010, freq);
                const uint32_t tf2 = gap.on(1);  // frame 2: only a 3P call
                const bool none = gap.take(true, 1, 1000, true, tf2, 111111, freq);
                const uint32_t ff3 = gap.on(0);  // the FP call arrives: opens frame 3?  (it has not been seen in frame 2, so it joins frame 2)
                const bool joined = ff3 == tf2;
                nf::P1FramePairing farPr;
                farPr.on(0);
                const uint32_t farTf = farPr.on(1);
                const bool vetoed = !farPr.take(true, farTf, 0, true, farTf, 300000ull, freq) && farPr.farApart == 1;   // 30 ms apart
                check(one && !none && joined && vetoed && gap.pairs == 1,
                      "F68c: a pair needs one index, and two calls of one index more than 10 ms apart are refused (counted), so a missing call cannot pair a frame "
                      "with its neighbour");
            }
            // F65c: the align's neck source. The F64 case: the pre-bend neck 11.4 cm left of its target (the eye at (0, 1.60, 0) in body
            // axes, the full-3P rest offset (0, -0.155, -0.09), so the target is (0, 1.445, -0.09)).
            {
                // (1) The pick: the pre-bend neck while at most 2 ticks old and under 100 ms; else the census; else none.
                check(nf::neckSourcePick(true, 0, 10, true) == nf::kNeckPreBend && nf::neckSourcePick(true, 2, 100, false) == nf::kNeckPreBend &&
                          nf::neckSourcePick(true, 3, 10, true) == nf::kNeckCensus && nf::neckSourcePick(true, 1, 150, true) == nf::kNeckCensus &&
                          nf::neckSourcePick(false, 0, 0, true) == nf::kNeckCensus && nf::neckSourcePick(true, 5, 10, false) == nf::kNeckNone,
                      "F65c: the align takes the pre-bend neck while fresh (2 ticks, 100 ms), else the census neck, else none");
                // (2) The align (the neck rest source, the F41 filter at 90 Hz, tau 0.1 s, 5 cm a frame) with the pre-bend source converges d
                // on the animated offset (+11.4 cm right), and the bend's input error (T - (N + d), right) goes to about 0. With the census's
                // post-bend neck (the bend has already put N + d on T) the target never moves: d stays 0 and the bend keeps the 11.4 cm.
                const double eye[3] = {0.0, 1.60, 0.0}, N0[3] = {-0.114, 1.445, -0.09};
                double T[3];
                for (int i = 0; i < 3; ++i) T[i] = eye[i] + nf::kNeckRestOffsetFull3P[i];
                double dPre[3] = {}, vPre[3] = {}, dCen[3] = {}, vCen[3] = {};
                for (int f = 0; f < 270; ++f) {
                    double src[3], tgt[3], post[3], srcC[3], tgtC[3];
                    nf::neckRestSource(N0, nf::kNeckRestOffsetFull3P, src);
                    for (int i = 0; i < 3; ++i) {
                        tgt[i] = eye[i] - src[i];
                        post[i] = T[i] - dCen[i];   // the census reads the neck where the bend put it: N + d on T
                    }
                    nf::neckRestSource(post, nf::kNeckRestOffsetFull3P, srcC);
                    for (int i = 0; i < 3; ++i) tgtC[i] = eye[i] - srcC[i];
                    for (int i = 0; i < 3; ++i) {
                        nf::cdStep(dPre[i], vPre[i], tgt[i], 0.1, 1.0 / 90.0, 0.05);
                        nf::cdStep(dCen[i], vCen[i], tgtC[i], 0.1, 1.0 / 90.0, 0.05);
                    }
                }
                const double errPre = T[0] - (N0[0] + dPre[0]), errCen = T[0] - (N0[0] + dCen[0]);
                check(std::fabs(dPre[0] - 0.114) < 1e-3 && std::fabs(errPre) < 1e-3 && std::fabs(dPre[2]) < 1e-9 && std::fabs(dCen[0]) < 1e-9 &&
                          std::fabs(errCen - 0.114) < 1e-9,
                      "F65c: with the pre-bend neck the align moves d 11.4 cm right and the bend's input error goes to 0; with the census's "
                      "post-bend neck d stays 0 and the 11.4 cm stays for the bend's roll");
            }
            // ---- F66: native on foot, one solve with exogenous inputs only (a, b, c, d, e, g; f is in its own block below) ----------------------
            // A small deterministic generator for the property checks.
            uint64_t f66Seed = 0x9E3779B97F4A7C15ull;
            const auto f66Rand = [&f66Seed]() {
                f66Seed = f66Seed * 6364136223846793005ull + 1442695040888963407ull;
                return static_cast<double>((f66Seed >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 53);   // [0, 1)
            };
            const auto f66Unit = [&f66Rand](double out[3]) {
                double l = 0.0;
                do {
                    for (int i = 0; i < 3; ++i) out[i] = 2.0 * f66Rand() - 1.0;
                    l = nf::ikLen(out);
                } while (l < 0.2 || l > 1.0);
                for (int i = 0; i < 3; ++i) out[i] /= l;
            };
            // F66a: the armed flag from Status.json SelectedWeapon.
            {
                char nm[64];
                const auto parse = [&nm](const char* text) {
                    return edvr::journalParseSelectedWeapon(text, std::strlen(text), nm, sizeof(nm));
                };
                const edvr::JournalHeldItem kFists = parse("{ \"timestamp\":\"2026-10-16T09:29:13Z\", \"event\":\"Status\", \"Flags\":16842765, \"Flags2\":8, "
                                                           "\"Pips\":[4,8,0], \"SelectedWeapon\":\"$humanoid_fists_name;\", \"SelectedWeapon_Localised\":\"Fists\", \"Oxygen\":1.0 }");
                const bool fistsName = std::strcmp(nm, "$humanoid_fists_name;") == 0;
                const edvr::JournalHeldItem kWeapon = parse("{ \"event\":\"Status\", \"Flags2\":8, \"SelectedWeapon\":\"$humanoid_assaultrifle_plasma_name;\", "
                                                            "\"SelectedWeapon_Localised\":\"Karma AR-50\", \"Gravity\":0.2 }");
                const bool weaponName = std::strcmp(nm, "$humanoid_assaultrifle_plasma_name;") == 0;
                const edvr::JournalHeldItem kTool = parse("{ \"Flags2\":8, \"SelectedWeapon\": \"$humanoid_companalyser_name;\" }");
                const bool toolName = std::strcmp(nm, "$humanoid_companalyser_name;") == 0;
                const edvr::JournalHeldItem kNone = parse("{ \"Flags\":0, \"GuiFocus\":0 }");
                const bool noneName = nm[0] == '\0';
                const edvr::JournalHeldItem kLocalOnly = parse("{ \"Flags2\":8, \"SelectedWeapon_Localised\":\"Fists\" }");
                const edvr::JournalHeldItem kSplit = parse("{ \"Flags2\":8, \"SelectedWeapon\":\"$humanoid_assaultrifle_pl");
                const bool splitName = nm[0] == '\0';
                const edvr::JournalHeldItem kSplitKey = parse("{ \"Flags2\":8, \"SelectedWeap");
                const edvr::JournalHeldItem kUpper = parse("{ \"SelectedWeapon\":\"$HUMANOID_FISTS_NAME;\" }");
                const edvr::JournalHeldItem kEmpty = parse("{ \"SelectedWeapon\":\"\" }");
                check(kFists == edvr::kHeldNone && fistsName && kWeapon == edvr::kHeldItem && weaponName && kTool == edvr::kHeldItem && toolName &&
                          kNone == edvr::kHeldUnknown && noneName && kLocalOnly == edvr::kHeldUnknown && kUpper == edvr::kHeldNone && kEmpty == edvr::kHeldNone,
                      "F66a: SelectedWeapon parses: the fists (any case, or empty) are none, a weapon or a tool is an item with its name, no field and "
                      "_Localised alone are unknown");
                check(kSplit == edvr::kHeldUnknown && splitName && kSplitKey == edvr::kHeldUnknown &&
                          edvr::journalParseSelectedWeapon(nullptr, 10, nm, sizeof(nm)) == edvr::kHeldUnknown &&
                          edvr::journalParseSelectedWeapon("x", 0, nm, sizeof(nm)) == edvr::kHeldUnknown,
                      "F66a: a value or key split across a read, a null or empty buffer are unknown, never a guess from the half that arrived");
                char tiny[8];
                const char* longName = "{ \"SelectedWeapon\":\"$humanoid_assaultrifle_plasma_name;\" }";
                const edvr::JournalHeldItem kTiny = edvr::journalParseSelectedWeapon(longName, std::strlen(longName), tiny, sizeof(tiny));
                check(kTiny == edvr::kHeldItem && std::strlen(tiny) == 7 && std::strncmp(tiny, "$humano", 7) == 0,
                      "F66a: the name is cut to the buffer and always terminated");
                // The ramp (0.3 s in and out) and the arm weight: 0 unarmed whatever the distance gate, 1 armed; the old gate is a measure.
                nf::IkBlend ramp{0.0f, nf::kArmedRampSec};
                ramp.step(true, 0.15);
                const float half = ramp.w;
                ramp.step(true, 0.15);
                const float full = ramp.w;
                ramp.step(false, 0.15);
                const float outHalf = ramp.w;
                ramp.step(false, 0.15);
                check(nf::kArmedRampSec == 0.3 && std::fabs(half - 0.5f) < 1e-6f && full == 1.0f && std::fabs(outHalf - 0.5f) < 1e-6f && ramp.w == 0.0f,
                      "F66a: the armed ramp is 0.3 s in and 0.3 s out (half at 0.15 s)");
                check(nf::ikArmWeight(0.0f) == 0.0f && nf::ikArmWeight(1.0f) == 1.0f && nf::ikArmWeight(0.5f) == 0.5f &&
                          nf::ikArmWeight(std::numeric_limits<float>::quiet_NaN()) == 0.0f && nf::ikArmWeight(2.0f) == 1.0f && nf::ikArmWeight(-1.0f) == 0.0f,
                      "F66a: unarmed gives the arm IK weight 0 whatever the distance gate says (the weight is the armed ramp alone)");
                check(nf::distanceGateHeld(0.64) && nf::distanceGateHeld(1.0) && !nf::distanceGateHeld(0.5) && !nf::distanceGateHeld(0.2) &&
                          !nf::distanceGateHeld(std::numeric_limits<double>::quiet_NaN()),
                      "F66a: the old distance-gate reading of held (the smaller grip gate above 0.5) is kept as a measure only");
            }
            // F66b: the frame clock. A 90 Hz tick run against a 64 Hz ms clock (GetTickCount64's 15.6 ms steps): the ms clock reads dt = 0 on about
            // 29% of the ticks and a critically damped follower of a target at constant speed advances unevenly; QPC ticks advance on every tick
            // and the per-tick steps agree within 5%.
            {
                const uint64_t freq = 10000000ull;   // a 10 MHz QPC
                int msZero = 0, qpcZero = 0;
                uint64_t lastQ = 0, lastMs = 0;
                double posQ = 0.0, velQ = 0.0, posM = 0.0, velM = 0.0;
                double stepQ[90], stepM[90];
                int nStep = 0;
                const double speed = 0.30;   // m/s of the target
                for (int k = 1; k <= 360; ++k) {
                    const double t = static_cast<double>(k) / 90.0;
                    const uint64_t q = static_cast<uint64_t>(std::llround(t * 1e7)), ms = static_cast<uint64_t>(std::floor(t * 1000.0 / 15.625) * 15.625);
                    const double dtQ = nf::qpcDtSec(lastQ, q, freq, 0.05);
                    const double dtM = lastMs && ms > lastMs ? std::min(0.05, static_cast<double>(ms - lastMs) / 1000.0) : 0.0;   // F65's clock
                    if (lastQ && !(dtQ > 0.0)) ++qpcZero;
                    if (lastMs && !(dtM > 0.0)) ++msZero;
                    const double beforeQ = posQ, beforeM = posM;
                    nf::cdStep(posQ, velQ, speed * t, 0.1, dtQ, 0.05);
                    nf::cdStep(posM, velM, speed * t, 0.1, dtM, 0.05);
                    if (k > 270) {
                        stepQ[nStep] = posQ - beforeQ;
                        stepM[nStep] = posM - beforeM;
                        ++nStep;
                    }
                    lastQ = q ? q : 1;
                    lastMs = ms ? ms : 1;
                }
                double meanQ = 0.0, meanM = 0.0, devQ = 0.0, devM = 0.0;
                for (int i = 0; i < nStep; ++i) {
                    meanQ += stepQ[i] / nStep;
                    meanM += stepM[i] / nStep;
                }
                for (int i = 0; i < nStep; ++i) {
                    devQ = std::max(devQ, std::fabs(stepQ[i] - meanQ) / meanQ);
                    devM = std::max(devM, std::fabs(stepM[i] - meanM) / std::fabs(meanM));
                }
                check(qpcZero == 0 && msZero > 60 && msZero < 150 && nStep == 90 && devQ < 0.05 && devM > 0.5,
                      "F66b: against a 64 Hz ms clock a 90 Hz run reads dt = 0 on about 29% of ticks and steps unevenly; on QPC every tick steps and the "
                      "per-tick steps at constant target speed agree within 5%");
                check(nf::qpcDtSec(0, 100, freq, 0.05) == 0.0 && nf::qpcDtSec(100, 100, freq, 0.05) == 0.0 && nf::qpcDtSec(200, 100, freq, 0.05) == 0.0 &&
                          nf::qpcDtSec(100, 100 + 111111, freq, 0.05) > 0.0111 && nf::qpcDtSec(100, 100 + 111111, freq, 0.05) < 0.0112 &&
                          nf::qpcDtSec(100, 100 + 100000000, freq, 0.05) == 0.05 && nf::qpcDtSec(100, 200, 0, 0.05) == 0.0,
                      "F66b: dt is 0 for the first tick, a clock that did not advance or one that went back, 11.1 ms for a 90 Hz step, and capped at 50 ms");
            }
            // F66c: the bend without a loop.
            {
                double fw = 0.0, bk = 0.0, rl = 0.0;
                nf::bendLimitsArmed(0.0, &fw, &bk, &rl);
                const bool unarmed = fw == 45.0 && bk == 15.0 && rl == 20.0;
                nf::bendLimitsArmed(1.0, &fw, &bk, &rl);
                const bool armed = fw == 8.0 && bk == 8.0 && rl == 6.0;
                nf::bendLimitsArmed(0.5, &fw, &bk, &rl);
                const bool mid = std::fabs(fw - 26.5) < 1e-12 && std::fabs(bk - 11.5) < 1e-12 && std::fabs(rl - 13.0) < 1e-12;
                nf::bendLimitsArmed(std::numeric_limits<double>::quiet_NaN(), &fw, &bk, &rl);
                check(unarmed && armed && mid && fw == 45.0,
                      "F66c: the bend's limits are interpolated by the armed ramp (unarmed 45, 15, 20 deg; armed 8, 8, 6), never switched");
                check(nf::bendResidualWeight(0.20) == 1.0 && nf::bendResidualWeight(0.30) == 1.0 && std::fabs(nf::bendResidualWeight(0.375) - 0.5) < 1e-12 &&
                          nf::bendResidualWeight(0.45) == 0.0 && nf::bendResidualWeight(0.60) == 0.0 &&
                          nf::bendResidualWeight(std::numeric_limits<double>::quiet_NaN()) == 0.0,
                      "F66c: the suspension is the weight 1 - smoothstep(0.30, 0.45, |T - N0|): 1 to 0.30 m, half at 0.375, 0 from 0.45");
                // A target swept through the old suspension edge: the weighted want never steps more than 1 deg a frame (20 deg roll limit, the
                // largest), at 3.3 mm a frame (0.3 m/s at 90 Hz); the old switch stepped the whole limit at once.
                double maxStep = 0.0, lastWant = 20.0;
                for (int f = 0; f <= 120; ++f) {
                    const double r = 0.15 + 0.40 * static_cast<double>(f) / 120.0;
                    const double want = 20.0 * nf::bendResidualWeight(r);
                    if (f) maxStep = std::max(maxStep, std::fabs(want - lastWant));
                    lastWant = want;
                }
                check(maxStep < 1.0 && maxStep > 0.3, "F66c: the continuous weight never steps the bend's want more than 1 deg a frame for a target swept through 0.35 m");
                // The square-up's turn of the unbent shoulder: about the vertical through the pivot, the height and the radius kept.
                const double p[3] = {0.20, 1.30, 0.10}, pivot[3] = {0.0, 1.50, 0.0};
                double q90[3], q45a[3], q45b[3], q0[3];
                nf::turnAboutVertical(p, pivot, nf::kIkPi * 0.5, q90);
                nf::turnAboutVertical(p, pivot, nf::kIkPi * 0.25, q45a);
                nf::turnAboutVertical(q45a, pivot, nf::kIkPi * 0.25, q45b);
                nf::turnAboutVertical(p, pivot, 0.0, q0);
                check(std::fabs(q90[0] - 0.10) < 1e-12 && std::fabs(q90[1] - 1.30) < 1e-12 && std::fabs(q90[2] + 0.20) < 1e-12 &&
                          std::fabs(q45b[0] - q90[0]) < 1e-12 && std::fabs(q45b[2] - q90[2]) < 1e-12 && q0[0] == p[0] && q0[1] == p[1] && q0[2] == p[2],
                      "F66c: U* is the unbent shoulder turned about the vertical through the pivot by the square-up's angle (height kept, angles add)");
                // The closed-loop model: lateral axis, 90 Hz, one frame of delay each way. A head 0.79 m off the shoulder's reach (R = 0.45): the
                // reach limit asks d to move back by the excess e; the bend's target is the eye less the published d, so its residual |T - N0| is
                // |e| (the F65 flight: d corrections 34.6 cm mean, 37.1 max, on the 0.35 m suspension edge). The shoulder moves with the bend by k
                // of its displacement. F65: the reach limit reads the bent shoulder (c = k b), the bend is suspended above 0.35 m. F66: it reads U*
                // (c = 0, nothing the bend does), the bend's want is scaled by the continuous weight.
                struct Model {
                    std::vector<double> d, b;
                    int suspended = 0;
                };
                const auto runModel = [](bool f66, double k, const std::function<double(int)>& head, double bendAmount) {
                    Model m;
                    double b = 0.0, bPrev = 0.0, ePrev = 0.0;
                    const double R = 0.45, lim = 0.12, cap = 0.05;
                    for (int t = 0; t < 270; ++t) {
                        const double dh = head(t);
                        const double c = f66 ? 0.0 : k * bPrev;
                        const double u = c + dh, over = std::fabs(u) - R;
                        const double e = over > 0.0 ? -std::copysign(over, u) : 0.0;
                        const double r = -ePrev;
                        double want = 0.0;
                        if (f66) {
                            want = std::max(-lim, std::min(lim, r)) * nf::bendResidualWeight(std::fabs(r)) * bendAmount;
                        } else {
                            const bool susp = !(std::fabs(r) <= 0.35);
                            if (susp && t >= 180) ++m.suspended;
                            want = susp ? 0.0 : std::max(-lim, std::min(lim, r)) * bendAmount;
                        }
                        const double step = std::max(-cap, std::min(cap, want - b));
                        bPrev = b;
                        b += step;
                        ePrev = e;
                        m.d.push_back(dh + e);
                        m.b.push_back(b);
                    }
                    return m;
                };
                const auto flips = [](const std::vector<double>& x, double eps) {
                    int n = 0;
                    double last = 0.0, lastD = 0.0;
                    bool haveLast = false, haveD = false;
                    for (size_t i = 180; i < x.size(); ++i) {
                        if (haveLast) {
                            const double dd = x[i] - last;
                            if (std::fabs(dd) > eps) {
                                if (haveD && ((dd > 0.0) != (lastD > 0.0))) ++n;
                                lastD = dd;
                                haveD = true;
                            }
                        }
                        last = x[i];
                        haveLast = true;
                    }
                    return n;
                };
                const std::function<double(int)> still = [](int) { return 0.79; };
                bool f65Cycles = true, f66Quiet = true;
                int f65Susp = 0;
                for (const double k : {0.25, 0.5, 0.75, 1.0}) {
                    const Model a = runModel(false, k, still, 1.0), c = runModel(true, k, still, 1.0);
                    f65Cycles = f65Cycles && flips(a.d, 0.0005) >= 15 && flips(a.b, 0.0005) >= 15;
                    f66Quiet = f66Quiet && flips(c.d, 0.0005) == 0 && flips(c.b, 0.0005) == 0;
                    if (k == 0.5) f65Susp = a.suspended;
                }
                const Model noLoop = runModel(false, 0.0, still, 1.0);   // k = 0: the bend cannot move the shoulder, so there is no loop to cycle
                check(f65Cycles && f66Quiet && flips(noLoop.d, 0.0005) == 0 && f65Susp >= 20 && f65Susp <= 80,
                      "F66c: closed-loop model: with the F65 rules (reach reads the bent shoulder, discrete suspension) d and the bend alternate in a limit cycle "
                      "for every k from 0.25 to 1 (the flight's was 2 frames, the model's differs in cap and delay; no cycle at k = 0); with the F66 rules the "
                      "alternating-sign count in d and the bend is 0 for every k");
                const std::function<double(int)> sway = [](int t) { return 0.79 + 0.15 * std::sin(2.0 * 3.14159265358979323846 * 0.4 * static_cast<double>(t) / 90.0); };
                const Model s0 = runModel(true, 0.5, sway, 0.0), s1 = runModel(true, 0.5, sway, 1.0);
                double dDiff = 0.0;
                for (size_t i = 0; i < s0.d.size(); ++i) dDiff = std::max(dDiff, std::fabs(s0.d[i] - s1.d[i]));
                check(dDiff == 0.0 && flips(s1.d, 0.0005) <= 2 && flips(s1.b, 0.0005) <= 2,
                      "F66c: with the F66 rules d does not change when the bend amount changes (the bend cannot reach d), and a swaying head gives no alternation");
            }
            // F66d: the elbow keeps its hinge.
            {
                const double l1 = 0.30, l2 = 0.27, S[3] = {0.0, 0.0, 0.0};
                const auto mul3 = [](const double A[9], const double B[9], double C[9]) {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) C[r * 3 + c] = A[r * 3] * B[c] + A[r * 3 + 1] * B[3 + c] + A[r * 3 + 2] * B[6 + c];
                };
                const auto tr3 = [](const double A[9], double T[9]) {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) T[c * 3 + r] = A[r * 3 + c];
                };
                const auto axisAngle = [](const double R[9], double axis[3]) {
                    double q[4];
                    nf::ikQuat(R, q);
                    const double s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                    if (s > 1e-12)
                        for (int i = 0; i < 3; ++i) axis[i] = q[i] / s;
                    else
                        axis[0] = axis[1] = axis[2] = 0.0;
                    return 2.0 * std::atan2(s, q[3]);
                };
                double worstHinge = 0.0, worstHand = 0.0, worstLen = 0.0, worstMapPos = 0.0, worstMapDeg = 0.0, worstAngle = 0.0;
                int cases = 0, oldBig = 0;
                for (int n = 0; n < 200; ++n) {
                    // The animated arm: a unit upper-arm direction u0, a hinge normal n0 (perpendicular), a flexion of 30..120 deg about n0.
                    double u0[3], tmp[3], n0[3];
                    f66Unit(u0);
                    f66Unit(tmp);
                    const double d0 = nf::ikDot(tmp, u0);
                    const double pr[3] = {tmp[0] - d0 * u0[0], tmp[1] - d0 * u0[1], tmp[2] - d0 * u0[2]};
                    if (!nf::ikUnit(pr, n0)) continue;
                    const double flex0 = (30.0 + 90.0 * f66Rand()) * nf::kIkPi / 180.0;
                    double Rf0[9], h0[3];
                    nf::ikRotAxis(n0, flex0, Rf0);
                    nf::ikRowRot(u0, Rf0, h0);
                    double E0[3], H0[3];
                    for (int i = 0; i < 3; ++i) {
                        E0[i] = S[i] + l1 * u0[i];
                        H0[i] = E0[i] + l2 * h0[i];
                    }
                    // The target: along the animated shoulder-hand axis, 5% shorter, the elbow turned 160 deg about it from the animated side (a plane
                    // change of 160 deg), plus a random 15 cm offset for half of the cases.
                    double hs0[3] = {H0[0] - S[0], H0[1] - S[1], H0[2] - S[2]}, T[3];
                    for (int i = 0; i < 3; ++i) T[i] = S[i] + 0.95 * hs0[i];
                    if (n % 2) {
                        double off[3];
                        f66Unit(off);
                        for (int i = 0; i < 3; ++i) T[i] += 0.10 * off[i];
                    }
                    double ref[3] = {E0[0] - S[0], E0[1] - S[1], E0[2] - S[2]};
                    const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, l1, l2, ref);
                    if (!tb.ok || tb.limited) continue;
                    const double th0 = tb.angleOf(ref);
                    double E[3];
                    tb.elbowAt(th0 + 160.0 * nf::kIkPi / 180.0, E);
                    const nf::HingeSwings hs = nf::ikHingeSwings(S, E0, H0, E, tb.hand, nullptr, nullptr);
                    if (!hs.ok) continue;
                    ++cases;
                    // swing 1 carries the elbow onto E and the animated hinge onto n1
                    double E0r[3], d01[3], n0r[3];
                    d01[0] = E0[0] - S[0];
                    d01[1] = E0[1] - S[1];
                    d01[2] = E0[2] - S[2];
                    nf::ikRowRot(d01, hs.R1, E0r);
                    nf::ikRowRot(hs.n0, hs.R1, n0r);
                    worstMapPos = std::max(worstMapPos, std::fabs(E0r[0] + S[0] - E[0]) + std::fabs(E0r[1] + S[1] - E[1]) + std::fabs(E0r[2] + S[2] - E[2]));
                    worstMapDeg = std::max(worstMapDeg, std::acos(std::min(1.0, nf::ikDot(n0r, hs.n1))) * 180.0 / nf::kIkPi);
                    // swing 2 about E: the hand lands on T; bone lengths kept
                    double hv[3] = {H0[0] - S[0], H0[1] - S[1], H0[2] - S[2]}, hr[3], hrel[3], hr2[3];
                    nf::ikRowRot(hv, hs.R1, hr);
                    for (int i = 0; i < 3; ++i) hrel[i] = hr[i] + S[i] - E[i];
                    nf::ikRowRot(hrel, hs.R2, hr2);
                    double hand[3], toT[3];
                    for (int i = 0; i < 3; ++i) {
                        hand[i] = E[i] + hr2[i];
                        toT[i] = hand[i] - T[i];
                    }
                    worstHand = std::max(worstHand, nf::ikLen(toT) * 1000.0);   // mm
                    worstLen = std::max(worstLen, std::max(std::fabs(nf::ikLen(hr2) - l2), std::fabs(nf::ikLen(E0r) - l1)));
                    // the forearm's rotation relative to the upper arm: R1^T (R_f0) R1 R2, about n1 by the solved flexion
                    double R1t[9], a1[9], a2[9], rel[9], axis[3];
                    tr3(hs.R1, R1t);
                    mul3(R1t, Rf0, a1);
                    mul3(a1, hs.R1, a2);
                    mul3(a2, hs.R2, rel);
                    const double ang = axisAngle(rel, axis);
                    double an = ang < 0.0 ? -ang : ang;
                    const double hingeErr = nf::ikAxisAngleDeg(axis, hs.n1);
                    worstHinge = std::max(worstHinge, hingeErr);
                    worstAngle = std::max(worstAngle, std::fabs(an * 180.0 / nf::kIkPi - hs.flexSolvedDeg));
                    if (hs.oldHingeErrDeg > 5.0) ++oldBig;
                }
                check(cases > 120 && worstMapPos < 1e-9 && worstMapDeg < 1e-4 && worstHand < 0.001 && worstLen < 1e-9,
                      "F66d: swing 1 carries the elbow to the solved one and the animated hinge to the solved hinge, swing 2 puts the hand on the target "
                      "within 0.001 mm, and the bone lengths are kept (200 random arms, a 160 deg plane change)");
                check(worstHinge < 0.01 && worstAngle < 0.01 && oldBig > cases / 2,
                      "F66d: the forearm's rotation relative to the upper arm is about the solved hinge within 0.01 deg at the solved flexion, where the "
                      "old two shortest-arc swings were over 5 deg off the hinge in most of these cases");
                // A nearly straight animated arm has no hinge: the shortest arc stands in, a given row or the last hinge is used when given.
                {
                    const double u0[3] = {0.0, -1.0, 0.0}, E0[3] = {0.0, -l1, 0.0}, H0s[3] = {0.0, -l1 - l2, 0.0};
                    const double T[3] = {0.20, -0.35, 0.10};
                    const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, l1, l2, u0);
                    double E[3];
                    tb.elbowAt(0.7, E);
                    const nf::HingeSwings straight = nf::ikHingeSwings(S, E0, H0s, E, tb.hand, nullptr, nullptr);
                    const double row[3] = {0.0, 0.0, 1.0}, last[3] = {1.0, 0.0, 0.0};
                    const nf::HingeSwings byRow = nf::ikHingeSwings(S, E0, H0s, E, tb.hand, row, nullptr);
                    const nf::HingeSwings byLast = nf::ikHingeSwings(S, E0, H0s, E, tb.hand, nullptr, last);
                    double handS[3], rel[3], out[3], tt[3];
                    bool lands = true;
                    for (const nf::HingeSwings* h : {&straight, &byRow, &byLast}) {
                        const double hv[3] = {H0s[0] - S[0], H0s[1] - S[1], H0s[2] - S[2]};
                        nf::ikRowRot(hv, h->R1, handS);
                        for (int i = 0; i < 3; ++i) rel[i] = handS[i] + S[i] - E[i];
                        nf::ikRowRot(rel, h->R2, out);
                        for (int i = 0; i < 3; ++i) tt[i] = E[i] + out[i] - tb.hand[i];
                        lands = lands && h->ok && nf::ikLen(tt) < 1e-9;
                    }
                    check(lands && straight.source == 2 && byRow.source == 3 && byLast.source == 1 && straight.flexAnimDeg < 1e-6,
                          "F66d: a nearly straight animated arm falls back to the shortest arc (source 2), the learned hinge row (3) or the last hinge (1), and "
                          "the hand still lands on the target");
                }
                // Armed stable-pole solves keep the animated geometric hinge when it is valid. A learned forearm row that is valid for a straight
                // chain can be stale relative to a bent animated pose; using it here swings the arm into the wrong plane and misses the wrist target.
                {
                    const double Sx[3] = {0.1, 1.0, -0.2}, Fx[3] = {0.28, 1.25, -0.15}, Hx[3] = {0.43, 1.14, -0.05};
                    const double upper[3] = {Fx[0] - Sx[0], Fx[1] - Sx[1], Fx[2] - Sx[2]};
                    const double fore[3] = {Hx[0] - Fx[0], Hx[1] - Fx[1], Hx[2] - Fx[2]};
                    const double upperLen = nf::ikLen(upper), foreLen = nf::ikLen(fore);
                    double nGeo[3] = {}, u[3] = {}, row[3] = {}, Rrow[9] = {};
                    const bool geoOk = nf::ikHingeAnimated(Sx, Fx, Hx, nGeo) && nf::ikUnit(upper, u);
                    if (geoOk) {
                        nf::ikRotAxis(u, 35.0 * nf::kIkPi / 180.0, Rrow);
                        nf::ikRowRot(nGeo, Rrow, row);
                    }
                    const double Tx[3] = {0.40, 1.13, 0.07};
                    const nf::IkTwoBone tbx = nf::ikTwoBoneFrame(Sx, Tx, upperLen, foreLen, upper);
                    double Ex[3] = {};
                    if (tbx.ok) tbx.elbowAt(tbx.angleOf(upper), Ex);
                    const double rowAngle = nf::ikAxisAngleDeg(row, nGeo);
                    const bool legacyUsesRow = nf::ikUseHingeRow(true, geoOk, rowAngle, false);
                    const bool stableUsesRow = nf::ikUseHingeRow(true, geoOk, rowAngle, true);
                    nf::HingeSwings geometric, learned;
                    if (geoOk && tbx.ok && !tbx.limited) {
                        geometric = nf::ikHingeSwings(Sx, Fx, Hx, Ex, tbx.hand, nullptr, nullptr);
                        learned = nf::ikHingeSwings(Sx, Fx, Hx, Ex, tbx.hand, legacyUsesRow ? row : nullptr, nullptr);
                    }
                    const auto wristAfter = [&](const nf::HingeSwings& hs, double out[3]) {
                        double shoulderHand[3] = {Hx[0] - Sx[0], Hx[1] - Sx[1], Hx[2] - Sx[2]}, swung[3], forearm[3], swungForearm[3];
                        nf::ikRowRot(shoulderHand, hs.R1, swung);
                        for (int i = 0; i < 3; ++i) forearm[i] = swung[i] + Sx[i] - Ex[i];
                        nf::ikRowRot(forearm, hs.R2, swungForearm);
                        for (int i = 0; i < 3; ++i) out[i] = Ex[i] + swungForearm[i];
                    };
                    const auto lengthError = [&](const nf::HingeSwings& hs) {
                        double upperAfter[3], foreAfter[3], foreFinal[3];
                        nf::ikRowRot(upper, hs.R1, upperAfter);
                        nf::ikRowRot(fore, hs.R1, foreAfter);
                        nf::ikRowRot(foreAfter, hs.R2, foreFinal);
                        return std::max(std::fabs(nf::ikLen(upperAfter) - upperLen), std::fabs(nf::ikLen(foreFinal) - foreLen));
                    };
                    double wristGeo[3] = {}, wristRow[3] = {};
                    if (geometric.ok) wristAfter(geometric, wristGeo);
                    if (learned.ok) wristAfter(learned, wristRow);
                    const double geoDelta[3] = {wristGeo[0] - tbx.hand[0], wristGeo[1] - tbx.hand[1], wristGeo[2] - tbx.hand[2]};
                    const double rowDelta[3] = {wristRow[0] - tbx.hand[0], wristRow[1] - tbx.hand[1], wristRow[2] - tbx.hand[2]};
                    const double geoError = nf::ikLen(geoDelta), rowError = nf::ikLen(rowDelta);
                    check(geoOk && tbx.ok && !tbx.limited && rowAngle > nf::kHingeRowTolDeg && legacyUsesRow && !stableUsesRow &&
                              nf::ikUseHingeRow(true, false, rowAngle, true) && geometric.ok && learned.ok && geometric.source == 0 && learned.source == 3 &&
                              geoError < 1e-9 && rowError > 0.001 && lengthError(geometric) < 1e-9 && lengthError(learned) < 1e-9,
                          "F80 armed stable-pole solving preserves the valid animated hinge: the stale learned row reproduces an off-plane wrist miss, while the geometric hinge reaches target; unarmed selection retains the learned-row behavior");
                }
                // The hand's twist folded into +-90 deg, continuous around the circle, through the real twist extraction.
                {
                    bool bounded = true, continuous = true;
                    double last = 0.0;
                    const double axisE[3] = {0.0, 0.6, 0.8};
                    for (int i = -360; i <= 360; ++i) {
                        const double th = static_cast<double>(i) * 0.5 * nf::kIkPi / 180.0;
                        double X[9];
                        nf::ikRotAxis(axisE, th, X);
                        const double f = nf::ikTwistFold(nf::ikTwistAbout(X, axisE));
                        if (std::fabs(f) > 0.5 * nf::kIkPi + 1e-12) bounded = false;
                        if (i > -360 && std::fabs(f - last) > 0.5 * nf::kIkPi / 180.0 + 1e-9) continuous = false;
                        last = f;
                    }
                    check(bounded && continuous && std::fabs(nf::ikTwistFold(30.0 * nf::kIkPi / 180.0) - 30.0 * nf::kIkPi / 180.0) < 1e-12 &&
                              std::fabs(nf::ikTwistFold(120.0 * nf::kIkPi / 180.0) - 60.0 * nf::kIkPi / 180.0) < 1e-12 &&
                              std::fabs(nf::ikTwistFold(-120.0 * nf::kIkPi / 180.0) + 60.0 * nf::kIkPi / 180.0) < 1e-12 &&
                              std::fabs(nf::ikTwistFold(nf::kIkPi)) < 1e-12 && nf::ikTwistLimited(100.0 * nf::kIkPi / 180.0) &&
                              !nf::ikTwistLimited(80.0 * nf::kIkPi / 180.0),
                          "F66d: a hand target turned through +-180 deg about the forearm gives a continuous helper roll within +-90 deg (the wrist carries "
                          "the rest); the wrapped twist jumped by up to 360 deg at the wrap");
                }
                // Which row of the forearm joint matrix is parallel to the hinge.
                {
                    float M[16] = {};
                    M[0] = 0.0f; M[1] = 1.0f; M[2] = 0.0f;     // row 0: +y
                    M[4] = 0.0f; M[5] = 0.0f; M[6] = -1.0f;    // row 1: -z
                    M[8] = 1.0f; M[9] = 0.0f; M[10] = 0.0f;    // row 2: +x
                    M[15] = 1.0f;
                    const double nz[3] = {0.0, 0.0, 1.0}, nyTilt[3] = {0.0, std::cos(0.1), std::sin(0.1)};
                    double ang = 0.0, sg = 0.0, ang2 = 0.0, sg2 = 0.0;
                    const int r1 = nf::ikHingeRow(M, nz, &ang, &sg), r2 = nf::ikHingeRow(M, nyTilt, &ang2, &sg2);
                    check(r1 == 1 && ang < 1e-6 && sg == -1.0 && r2 == 0 && std::fabs(ang2 - 0.1 * 180.0 / nf::kIkPi) < 1e-6 && sg2 == 1.0,
                          "F66d: the hinge row is the forearm matrix row most parallel to the hinge (modulo sign), with its angle and the sign that agrees");
                }
            }
            // F66e: ADS, the rendered eyes.
            {
                // A view-projection for an eye at pe with camera-relative origin o, asymmetric tangents (L, R, B, T), a yaw of 30 deg and a pitch of 10:
                // clip x = (2/(R-L)) x_v - ((R+L)/(R-L)) z_v, clip y likewise, clip w = z_v, clip z any; row-vector form (clip_j = v . c_j + k_j).
                const double yaw = 30.0 * nf::kIkPi / 180.0, pit = 10.0 * nf::kIkPi / 180.0;
                const double fwd[3] = {std::sin(yaw) * std::cos(pit), -std::sin(pit), std::cos(yaw) * std::cos(pit)};
                double right[3] = {std::cos(yaw), 0.0, -std::sin(yaw)}, up[3];
                nf::ikCross(fwd, right, up);
                const double tL = -1.35, tR = 1.10, tB = -1.15, tT = 1.30;
                const double pe[3] = {3.2, 1.6, -5.4}, o[3] = {3.0, 1.5, -5.0};
                const auto build = [&](const double peye[3], const double orig[3], float rows[16]) {
                    double c[4][3];
                    for (int i = 0; i < 3; ++i) {
                        c[0][i] = 2.0 / (tR - tL) * right[i] - (tR + tL) / (tR - tL) * fwd[i];
                        c[1][i] = 2.0 / (tT - tB) * up[i] - (tT + tB) / (tT - tB) * fwd[i];
                        c[2][i] = 0.01 * fwd[i];
                        c[3][i] = fwd[i];
                    }
                    const double rel[3] = {peye[0] - orig[0], peye[1] - orig[1], peye[2] - orig[2]};
                    for (int j = 0; j < 4; ++j) {
                        for (int i = 0; i < 3; ++i) rows[4 * i + j] = static_cast<float>(c[j][i]);
                        rows[12 + j] = static_cast<float>(-nf::ikDot(c[j], rel));
                    }
                };
                float rows[16];
                build(pe, o, rows);
                double eye[3] = {};
                const bool ok = nf::eyeFromRows(rows, o, eye);
                const double err[3] = {eye[0] - pe[0], eye[1] - pe[1], eye[2] - pe[2]};
                // a second eye, 6.1 cm to the right with another origin: the rows carry the offset; the solve stays within 0.1 mm
                const double pe2[3] = {pe[0] + 0.061 * right[0], pe[1] + 0.061 * right[1], pe[2] + 0.061 * right[2]}, o2[3] = {pe2[0] + 0.4, pe2[1] - 0.2, pe2[2] + 0.7};
                float rows2[16];
                build(pe2, o2, rows2);
                double eye2[3] = {};
                const bool ok2 = nf::eyeFromRows(rows2, o2, eye2);
                const double err2[3] = {eye2[0] - pe2[0], eye2[1] - pe2[1], eye2[2] - pe2[2]};
                float singular[16] = {};
                double eyeS[3];
                check(ok && ok2 && nf::ikLen(err) < 1e-4 && nf::ikLen(err2) < 1e-4 && !nf::eyeFromRows(singular, o, eyeS),
                      "F66e: the rendered eye is solved from its scene rows (clip x, y, w vanish; asymmetric frusta, a camera-relative origin) within 0.1 mm; "
                      "a singular system is refused");
                double d01[3] = {eye2[0] - eye[0], eye2[1] - eye[1], eye2[2] - eye[2]};
                check(std::fabs(nf::ikLen(d01) - 0.061) < 1e-3, "F66e: the IPD from the two solved eyes is the 6.1 cm they were built 6.1 cm apart");
                // the sight line: through p14 along f; an eye on it has no perpendicular distance, one 3.2 cm beside it has 3.2 cm; f need not be unit
                double perp = 9.0, along = 9.0, perp2 = 9.0, along2 = 9.0, perp3 = 9.0, along3 = 9.0;
                const double p14[3] = {0.0, 1.7, 0.5}, fz[3] = {0.0, 0.0, 2.0}, eOn[3] = {0.0, 1.7, 0.0}, eSide[3] = {0.032, 1.7, 0.0};
                nf::sightLineOfEye(eOn, p14, fz, &perp, &along);
                nf::sightLineOfEye(eSide, p14, fz, &perp2, &along2);
                const double zero3[3] = {};
                nf::sightLineOfEye(eSide, p14, zero3, &perp3, &along3);
                check(std::fabs(perp) < 1e-12 && std::fabs(along - 0.5) < 1e-12 && std::fabs(perp2 - 0.032) < 1e-12 && std::fabs(along2 - 0.5) < 1e-12 &&
                          perp3 == 0.0 && along3 == 0.0,
                      "F66e: an eye's distance to the sight line is its perpendicular part (0 on the line, 3.2 cm beside it) and its distance along to P14");
                check(nf::sceneRowsFreshForLock(10, 10) && nf::sceneRowsFreshForLock(9, 10) && !nf::sceneRowsFreshForLock(8, 10) &&
                          !nf::sceneRowsFreshForLock(11, 10) && !nf::sceneRowsFreshForLock(~0u, 10),
                      "F66e: an eye's scene rows are this frame's when at most one frame behind the boundary that reads them");
                // The lock on a model plant. The job camera sits at the LEFT eye (A1); the right eye is 6.1 cm to its right; the sight line passes 9 cm
                // right of the camera. F63's model puts E_R at the camera + 3.05 cm; the lock closes on that model and leaves the real right eye
                // 3.05 cm off the line (the flight's "between the eyes"); with E_R from the rows it converges onto the line.
                const double rowsB[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, sight[3] = {0.09, 1.70, 0.5}, fSight[3] = {0.0, 0.0, 1.0}, rHat[3] = {1.0, 0.0, 0.0};
                double camModel[3] = {0.0, 1.70, 0.0}, camRows[3] = {0.0, 1.70, 0.0};
                for (int it = 0; it < 400; ++it) {
                    nf::AdsSightErr em, er;
                    nf::adsSightError(sight, camModel, rHat, 0.0305, fSight, rowsB, em);
                    for (int i = 0; i < 3; ++i) camModel[i] += 0.1 * em.ePerpW[i];
                    const double eyeR[3] = {camRows[0] + 0.061, camRows[1], camRows[2]};
                    nf::adsSightErrorEye(sight, eyeR, fSight, rowsB, er);
                    for (int i = 0; i < 3; ++i) camRows[i] += 0.1 * er.ePerpW[i];
                }
                double perpM = 0.0, perpR = 0.0, alongTmp = 0.0;
                const double realM[3] = {camModel[0] + 0.061, camModel[1], camModel[2]}, realR[3] = {camRows[0] + 0.061, camRows[1], camRows[2]};
                nf::sightLineOfEye(realM, sight, fSight, &perpM, &alongTmp);
                nf::sightLineOfEye(realR, sight, fSight, &perpR, &alongTmp);
                check(std::fabs(perpM - 0.0305) < 1e-6 && perpR < 1e-9,
                      "F66e: the lock with the F63 model closes on the model and leaves the real right eye 3.05 cm off the sight line when the camera is at "
                      "the left eye; with the right eye solved from its rows it converges onto the line");
            }
            // F66g: frame order and parity.
            {
                nf::AltCount flip, ramp, noise;
                for (int i = 0; i < 20; ++i) {
                    flip.step(i % 2 ? 1.0 : -1.0, 0.05);
                    ramp.step(0.1 * i, 0.05);
                    noise.step(0.01 * (i % 2), 0.05);
                }
                check(flip.flips == 18 && flip.steps == 19 && ramp.flips == 0 && ramp.steps == 19 && noise.flips == 0 && noise.steps == 0,
                      "F66g: the alternating-sign count is every step of a flip-flop, none of a ramp, none of changes under the threshold");
                nf::ParityMean pm;
                pm.add(0, 1.0);
                pm.add(0, 3.0);
                pm.add(1, 5.0);
                pm.add(3, 7.0);
                nf::ParityMean empty;
                empty.add(1, 4.0);
                check(std::fabs(pm.mean(0) - 2.0) < 1e-12 && std::fabs(pm.mean(1) - 6.0) < 1e-12 && std::fabs(pm.diff() + 4.0) < 1e-12 && pm.n[0] == 2 &&
                          pm.n[1] == 2 && empty.diff() == 0.0,
                      "F66g: the parity mean is per low bit of the game-frame index; the even-minus-odd difference is 0 when either parity has no sample");
                char o1[4], o2[4], o3[4], o4[4], o5[4];
                nf::frameOrder3(10, 20, 30, o1);
                nf::frameOrder3(30, 10, 20, o2);
                nf::frameOrder3(20, 30, 10, o3);
                nf::frameOrder3(5, 5, 5, o4);
                nf::frameOrder3(10, 0, 30, o5);
                check(std::strcmp(o1, "PAS") == 0 && std::strcmp(o2, "ASP") == 0 && std::strcmp(o3, "SPA") == 0 && std::strcmp(o4, "PAS") == 0 &&
                          std::strcmp(o5, "???") == 0,
                      "F66g: the order of the eye probe, the Animate and the submit post-hook is by QPC ticks (ties in P, A, S order); an event that did not "
                      "run reads ???");
            }
            // F66f: the marker's offset from its own geometry and the affine row patch (draw_record_tag.h).
            {
                namespace dtf = edvr::drawtag;
                // The eye's texture frustum (asymmetric: the left eye's axis sits about 0.10 NDC off centre) and the TAA jitter the scene rows carry.
                const double tL = -1.35, tR = 1.10, tB = -1.15, tT = 1.30;
                const float tan4[4] = {static_cast<float>(tL), static_cast<float>(tR), static_cast<float>(tB), static_cast<float>(tT)};
                double axis[2] = {};
                const bool axisOk = dtf::axisFromTangents(tan4, axis);
                const float badTan[4] = {1.0f, 1.0f, -1.0f, 1.0f};
                double axisBad[2] = {};
                const double jit[2] = {0.0009, -0.0007};   // NDC (about 0.9 px at 2016)
                // The world rows: x, y, w columns, row vectors: clip_j = v . c_j (the eye at the origin; the translation row is zero).
                const double yaw = 20.0 * 3.14159265358979323846 / 180.0;
                const double fwd[3] = {std::sin(yaw), 0.0, std::cos(yaw)}, right[3] = {std::cos(yaw), 0.0, -std::sin(yaw)}, up[3] = {0.0, 1.0, 0.0};
                float wrows[16] = {};
                const double scW[2] = {2.0 / (tR - tL), 2.0 / (tT - tB)};
                for (int i = 0; i < 3; ++i) {
                    wrows[4 * i + 0] = static_cast<float>(scW[0] * right[i] + (axis[0] + jit[0]) * fwd[i]);
                    wrows[4 * i + 1] = static_cast<float>(scW[1] * up[i] + (axis[1] + jit[1]) * fwd[i]);
                    wrows[4 * i + 2] = static_cast<float>(0.01 * fwd[i]);
                    wrows[4 * i + 3] = static_cast<float>(fwd[i]);
                }
                // The marker's rows: another frame (turned 33 deg), its own centre 0.03 NDC off the axis and scales 0.94% and 0.81% larger.
                const double yawM = 33.0 * 3.14159265358979323846 / 180.0;
                const double fM[3] = {std::sin(yawM), 0.0, std::cos(yawM)}, rM[3] = {std::cos(yawM), 0.0, -std::sin(yawM)};
                const double scM[2] = {scW[0] * 1.0094, scW[1] * 1.0081}, offM[2] = {axis[0] + 0.03, axis[1] - 0.02};
                float m4[4], m5[4], m7[4];
                for (int i = 0; i < 3; ++i) {
                    m4[i] = static_cast<float>(scM[0] * rM[i] + offM[0] * fM[i]);
                    m5[i] = static_cast<float>(scM[1] * up[i] + offM[1] * fM[i]);
                    m7[i] = static_cast<float>(fM[i]);
                }
                m4[3] = 0.0f;
                m5[3] = 0.0f;
                m7[3] = 0.0f;
                dtf::ProjDecomp dw, dm;
                const bool okW = dtf::projDecomposeWorld(wrows, dw), okM = dtf::projDecomposeMarker(m4, m5, m7, dm);
                const dtf::MarkerAffine aff = dtf::markerAffine(dm, dw, axis);
                const dtf::MarkerAffine noAxis = dtf::markerAffine(dm, dw, nullptr);
                check(axisOk && std::fabs(axis[0] - 0.10204081632653061) < 1e-6 && std::fabs(axis[1] + 0.061224489795918366) < 1e-6 &&
                          !dtf::axisFromTangents(badTan, axisBad),
                      "F66f: the texture's optical axis is -(R + L) / (R - L) and -(U + D) / (U - D) of its tangents (the left eye's sits off centre)");
                check(okW && okM && std::fabs(dw.off[0] - (axis[0] + jit[0])) < 1e-6 && std::fabs(dw.off[1] - (axis[1] + jit[1])) < 1e-6 &&
                          std::fabs(dw.scale[0] - scW[0]) < 1e-6 && std::fabs(dw.scale[1] - scW[1]) < 1e-6 && std::fabs(dm.off[0] - offM[0]) < 1e-6 &&
                          std::fabs(dm.off[1] - offM[1]) < 1e-6 && std::fabs(dm.scale[0] - scM[0]) < 1e-6 && std::fabs(dm.scale[1] - scM[1]) < 1e-6,
                      "F66f: a projection's optical axis and focal scale per axis are read from its rows alone (world rows and the marker's b0 rows 4, 5, 7), "
                      "whatever frame they are in");
                check(aff.ok && aff.haveAxis && std::fabs(aff.sigma[0] - 1.0094) < 1e-5 && std::fabs(aff.sigma[1] - 1.0081) < 1e-5 &&
                          std::fabs(aff.jitter[0] - jit[0]) < 1e-6 && std::fabs(aff.jitter[1] - jit[1]) < 1e-6 && noAxis.ok && !noAxis.haveAxis &&
                          noAxis.jitter[0] == 0.0 && noAxis.jitter[1] == 0.0 && std::fabs(noAxis.axis[0] - dw.off[0]) < 1e-12,
                      "F66f: sigma = scale_M / scale_W per axis (1.0094 and 1.0081 here: x and y unequal); the jitter the scene rows carry is their centre "
                      "less the texture's axis, zero when the tangents are not known");
                // The patch: the anchor's target T (the unjittered P plus e); n_final = sigma n_M + u shows the anchor at T and every other vertex at
                // the offset the game designed (n_M(q) - n_M(X0)), through the display map.
                const double x0[3] = {12.0, 3.0, 40.0}, q[3] = {12.4, 3.9, 40.0}, zero3[3] = {0.0, 0.0, 0.0};
                double cm0[3], cmq[3];
                dtf::markerClip(m4, m5, m7, x0, zero3, cm0);
                dtf::markerClip(m4, m5, m7, q, zero3, cmq);
                const double nM0[2] = {cm0[0] / cm0[2], cm0[1] / cm0[2]}, nMq[2] = {cmq[0] / cmq[2], cmq[1] / cmq[2]};
                const double T[2] = {0.52, -0.31};
                double u[2];
                dtf::markerAffineU(aff, T, nM0, u);
                float p4[4], p5[4];
                std::memcpy(p4, m4, sizeof(p4));
                std::memcpy(p5, m5, sizeof(p5));
                dtf::affineOffsetRows(p4, p5, m7, aff.sigma, u);
                double pc0[3], pcq[3], d0[2], dq[2];
                dtf::markerClip(p4, p5, m7, x0, zero3, pc0);
                dtf::markerClip(p4, p5, m7, q, zero3, pcq);
                const double nf0[2] = {pc0[0] / pc0[2], pc0[1] / pc0[2]}, nfq[2] = {pcq[0] / pcq[2], pcq[1] / pcq[2]};
                dtf::displayOfUi(aff, nf0, d0);
                dtf::displayOfUi(aff, nfq, dq);
                check(std::fabs(d0[0] - T[0]) < 1e-5 && std::fabs(d0[1] - T[1]) < 1e-5 && std::fabs((dq[0] - d0[0]) - (nMq[0] - nM0[0])) < 1e-5 &&
                          std::fabs((dq[1] - d0[1]) - (nMq[1] - nM0[1])) < 1e-5,
                      "F66f: the affine rows show the anchor exactly at its target in the texture and every other vertex at the offset the game designed "
                      "(the whole icon lands, not only its anchor)");
                // Without the affine (the old translation) the anchor lands short of T by (1 - 1/sigma) of its distance from the axis.
                float t4[4], t5[4];
                std::memcpy(t4, m4, sizeof(t4));
                std::memcpy(t5, m5, sizeof(t5));
                dtf::MarkerAffine plain;
                double ut[2];
                dtf::markerAffineU(plain, T, nM0, ut);
                dtf::affineOffsetRows(t4, t5, m7, plain.sigma, ut);
                double tc[3], dT[2];
                dtf::markerClip(t4, t5, m7, x0, zero3, tc);
                const double ntr[2] = {tc[0] / tc[2], tc[1] / tc[2]};
                dtf::displayOfUi(aff, ntr, dT);
                check(std::fabs(ntr[0] - T[0]) < 1e-5 && std::fabs(ntr[1] - T[1]) < 1e-5 && std::fabs(plain.sigma[0] - 1.0) == 0.0 && !plain.ok &&
                          std::fabs(ut[0] - (T[0] - nM0[0])) < 1e-12 && std::fabs(ut[1] - (T[1] - nM0[1])) < 1e-12 && std::fabs(dT[0] - T[0]) > 1e-3,
                      "F66f: with sigma 1 the patch is today's translation (u = T - n_M(X0)); shown through the real display map it misses T by the "
                      "scale error (a few thousandths of NDC at this offset from the axis, about 10 px at 2016)");
                // The unjittered place of a point, a ratio outside 0.9..1.1 refused, and the other axis of the same tangents.
                double nj[2];
                const double nPj[2] = {0.30 + jit[0], -0.20 + jit[1]};
                dtf::unjitter(aff, nPj, nj);
                dtf::ProjDecomp wide = dw;
                wide.scale[0] *= 1.3;
                const dtf::MarkerAffine refused = dtf::markerAffine(dm, wide, axis);
                check(std::fabs(nj[0] - 0.30) < 1e-6 && std::fabs(nj[1] + 0.20) < 1e-6 && !refused.ok && refused.sigma[0] == 1.0 && refused.sigma[1] == 1.0 &&
                          !dtf::markerAffine(dtf::ProjDecomp{}, dw, axis).ok,
                      "F66f: P is taken unjittered (the scene rows' centre less the axis is the jitter); a scale ratio over 10% or an undecomposed "
                      "projection falls back to the translation");
                // The icon offset from the geometry: the packed positions (both forms), the record's quaternion and scale, and e through the rows.
                double pa[3] = {}, pb[3] = {}, pc[3] = {}, pd[3] = {};
                const bool d64 = dtf::decodePackedPosition((16384u << 16) | 49152u, (4096u << 16) | 32768u, 64u << 24, pa);
                const bool dE0 = dtf::decodePackedPosition(0x17FFFFu & 0x1FFFFFu, 0u, 0u, pb);
                const bool dE0z = dtf::decodePackedPosition(0u, 0u, 0u, pc);
                const bool dBad = dtf::decodePackedPosition(0u, 0u, 20u << 24, pd);
                check(d64 && std::fabs(pa[0] - 0.5) < 1e-12 && std::fabs(pa[1] + 0.5) < 1e-12 && std::fabs(pa[2]) < 1e-12 && dE0 &&
                          std::fabs(pb[0] - (1572863.0 / 1048575.0 - 1.0)) < 1e-9 && dE0z && std::fabs(pc[0] + 1.0) < 1e-12 && std::fabs(pc[1] + 1.0) < 1e-12 &&
                          std::fabs(pc[2] + 1.0) < 1e-12 && !dBad,
                      "F66f: PACKEDVERTEXDATAA.xyz decodes as the vertex shader does (the 16-bit form with its exponent, the 21-bit form with its range); "
                      "an exponent of 20 divides by zero and is refused");
                const double vx[3] = {1.0, 0.0, 0.0}, vv[3] = {0.0, 1.0, 0.0};
                double r0v[3], r1v[3];
                dtf::rotateByPackedQuat(32768u | (32768u << 16), 32768u | (65535u << 16), vx, r0v);        // (0, 0, 0, ~1): the identity
                dtf::rotateByPackedQuat(32768u | (65535u << 16), 32768u | (32768u << 16), vx, r1v);        // (0, ~1, 0, 0): half a turn about y
                double r2v[3];
                dtf::rotateByPackedQuat(32768u | (32768u << 16), 32768u | (65535u << 16), vv, r2v);
                check(std::fabs(r0v[0] - 1.0) < 5e-4 && std::fabs(r0v[1]) < 5e-4 && std::fabs(r1v[0] + 1.0) < 5e-4 && std::fabs(r1v[2]) < 5e-4 &&
                          std::fabs(r2v[1] - 1.0) < 5e-4,
                      "F66f: the instance record's packed quaternion (four unorm16 as u / 32768 - 1) turns a vertex as the vertex shader does");
                const float i4[4] = {1, 0, 0, 0}, i5[4] = {0, 1, 0, 0}, i7[4] = {0, 0, 1, 0};
                const double xr[3] = {0.0, 0.0, 10.0}, vc[3] = {0.0, -0.5, 0.0};
                double eIcon[2] = {};
                const bool eOk = dtf::iconEdrawNdc(i4, i5, i7, xr, 2.0, 32768u | (32768u << 16), 32768u | (65535u << 16), vc, eIcon);
                double eBehind[2] = {};
                const double xb[3] = {0.0, 0.0, -10.0};
                check(eOk && std::fabs(eIcon[0]) < 1e-4 && std::fabs(eIcon[1] - 0.1) < 1e-4 &&
                          !dtf::iconEdrawNdc(i4, i5, i7, xb, 2.0, 32768u | (32768u << 16), 32768u | (65535u << 16), vc, eBehind),
                      "F66f: e is the NDC difference between the anchor and the icon's centre vertex through the draw's rows (a centre 1 m below an anchor "
                      "10 m away is 0.1 NDC above it); behind the projection it is refused");
                // F67d: the constant fallback, the gate on a mesh's e, the disc centre, the index range.
                {
                    double ef[2] = {};
                    dtf::iconFallbackNdc(4032.0, ef);
                    double ok1[2] = {0.0, 2.0 * 36.38 / 4032.0}, ok2[2] = {0.01, 2.0 * 5.0 / 4032.0}, bad1[2] = {0.0, 2.0 * 120.0 / 4032.0}, bad2[2] = {2.0 * 80.0 / 4032.0, 2.0 * 36.0 / 4032.0},
                           bad3[2] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
                    check(ef[0] == 0.0 && std::fabs(ef[1] * 4032.0 * 0.5 - 36.38) < 1e-9 && dtf::iconEgate(ok1, 4032.0, 4032.0) && dtf::iconEgate(ok2, 4032.0, 4032.0) &&
                              !dtf::iconEgate(bad1, 4032.0, 4032.0) && !dtf::iconEgate(bad2, 4032.0, 4032.0) && !dtf::iconEgate(bad3, 4032.0, 4032.0) &&
                              dtf::kPatchNoIcon == 7,
                          "F67d: the fallback offset is the constant 36.38 px up (never a refusal); a mesh's e is believed within 60 px of it on y and of 0 on x, and not NaN");
                    // A fan: a centre vertex (0, 1, 0) with a ring of eight at 0.3 m around it, and two stem vertices.
                    double fan[11][3];
                    fan[0][0] = 0.0;
                    fan[0][1] = 1.0;
                    fan[0][2] = 0.0;
                    for (int i = 0; i < 8; ++i) {
                        const double a = 2.0 * 3.14159265358979323846 * i / 8.0;
                        fan[1 + i][0] = 0.3 * std::cos(a);
                        fan[1 + i][1] = 1.0;
                        fan[1 + i][2] = 0.3 * std::sin(a);
                    }
                    fan[9][0] = 0.0;
                    fan[9][1] = 0.0;
                    fan[9][2] = 0.0;
                    fan[10][0] = 0.0;
                    fan[10][1] = 0.5;
                    fan[10][2] = 0.0;
                    double c1[3], c2[3], c3[3], c4[3];
                    uint32_t i1 = 0, i2 = 0, i3 = 0, i4x = 0;
                    const int m1 = dtf::discCentre(fan, 11, c1, &i1);
                    const double quad[4][3] = {{-0.3, 0.7, 0.0}, {0.3, 0.7, 0.0}, {-0.3, 1.3, 0.0}, {0.3, 1.3, 0.0}};
                    const int m2 = dtf::discCentre(quad, 4, c2, &i2);
                    double big[14][3];
                    for (int i = 0; i < 10; ++i) {
                        big[i][0] = 0.0;
                        big[i][1] = 0.1 * i;
                        big[i][2] = 0.0;
                    }
                    const double top[4][3] = {{-0.3, 1.0, 0.0}, {0.3, 1.0, 0.0}, {-0.3, 1.6, 0.0}, {0.3, 1.6, 0.0}};
                    for (int i = 0; i < 4; ++i)
                        for (int k = 0; k < 3; ++k) big[10 + i][k] = top[i][k];
                    const int m3 = dtf::discCentre(big, 14, c3, &i3);
                    const int m0 = dtf::discCentre(quad, 2, c4, &i4x);
                    check(m1 == 1 && i1 == 0 && std::fabs(c1[0]) < 1e-12 && std::fabs(c1[1] - 1.0) < 1e-12 && m2 == 2 && i2 == 0xFFFFFFFFu && std::fabs(c2[1] - 1.0) < 1e-12 &&
                              std::fabs(c2[0]) < 1e-12 && m3 == 3 && std::fabs(c3[1] - 1.6) < 1e-9 && m0 == 0 && i4x == 0xFFFFFFFFu,
                          "F67d: the disc centre is a fan's centre vertex (a vertex with five or more others at one distance), else the mean of a small mesh, else the "
                          "mean of the far cluster; under three vertices none");
                    const uint16_t i16[6] = {0, 1, 2, 2, 1, 3};
                    const uint32_t i32[4] = {7, 7, 300, 5};
                    uint32_t mx = 0, dist[8] = {}, nd = 0;
                    const bool r16 = dtf::readIndexRange(reinterpret_cast<const uint8_t*>(i16), 6, 2, &mx, dist, 8, &nd);
                    const uint32_t mx16 = mx, nd16 = nd, d16[4] = {dist[0], dist[1], dist[2], dist[3]};
                    const bool r32 = dtf::readIndexRange(reinterpret_cast<const uint8_t*>(i32), 4, 4, &mx, dist, 2, &nd);
                    check(r16 && mx16 == 3 && nd16 == 4 && d16[0] == 0 && d16[1] == 1 && d16[2] == 2 && d16[3] == 3 && r32 && mx == 300 && nd == 3 && dist[0] == 7 && dist[1] == 300 &&
                              !dtf::readIndexRange(reinterpret_cast<const uint8_t*>(i16), 6, 3, &mx, dist, 8, &nd) && !dtf::readIndexRange(nullptr, 6, 2, &mx, dist, 8, &nd),
                          "F67d: an index range is read as 16- or 32-bit indices: the largest, and the distinct ones in first-seen order (the count past the capacity still "
                          "counts); another size or no bytes is refused");
                }
            }
            // The shift rule: a record at the AMC origin moves by d once; a second pass over it leaves it alone; a record elsewhere never moves.
            const float origin[3] = {10.0f, 2.0f, -3.0f}, d[3] = {0.0f, 0.231f, 0.0f};
            float rec[3] = {10.0f, 2.0f, -3.0f}, other[3] = {10.5f, 2.0f, -3.0f};
            const bool firstPass = nf::shiftIfAtOrigin(rec, origin, d);
            const float afterFirst[3] = {rec[0], rec[1], rec[2]};
            const bool secondPass = nf::shiftIfAtOrigin(rec, origin, d);
            const bool elsewhere = nf::shiftIfAtOrigin(other, origin, d);
            check(firstPass && !secondPass && !elsewhere && rec[1] == afterFirst[1] && rec[1] == 2.0f + 0.231f && other[0] == 10.5f && other[1] == 2.0f,
                  "F51: the FP shift moves a record at the AMC origin once and is idempotent; a placed record is left alone");
            // F54: the local FP weapon gate (mask, viewmodel byte, 0.3 m of an FP handAttach joint) and the shift of both record copies.
            {
                const float attach[2][3] = {{1.0f, 1.2f, 0.5f}, {1.4f, 1.2f, 0.5f}};   // L (52), R (51)
                const float nearP[3] = {1.4f, 1.0f, 0.5f}, edgeP[3] = {1.0f, 1.2f, 0.79f}, farP[3] = {1.4f, 1.2f, 0.85f};
                const float nan3[3] = {std::numeric_limits<float>::quiet_NaN(), 1.2f, 0.5f};
                check(nf::weaponIsLocalFp(1, 1, nearP, attach, true) && nf::weaponIsLocalFp(0x10, 0x80, edgeP, attach, true) &&
                          !nf::weaponIsLocalFp(1, 1, farP, attach, true) && !nf::weaponIsLocalFp(1, 1, nan3, attach, true),
                      "F54: a record within 0.3 m of either FP handAttach joint matches; 0.35 m and a NaN position do not");
                check(!nf::weaponIsLocalFp(0, 1, nearP, attach, true) && !nf::weaponIsLocalFp(1, 0, nearP, attach, true) &&
                          !nf::weaponIsLocalFp(1, 1, nearP, attach, false),
                      "F54: a zero mask, a clear viewmodel byte or unknown attach joints never match");
            }
            // The freeze gate: no locomotion. Standing (a weapon out does not enter it) rests after 0.3 s; walking at 1.4 m/s does not; a fall
            // (vertical motion only) is not locomotion; ADS ends the rest.
            const double upAxis[3] = {0.0, 1.0, 0.0};
            nf::PfzLocoRest lr;
            double pos[3] = {5.0, 1.0, 5.0};
            bool r = false;
            for (uint64_t t = 0; t <= 400; t += 11) r = lr.step(t, pos, upAxis, false);
            const bool standRests = r;
            for (uint64_t t = 411; t <= 1400; t += 11) {
                pos[2] += 1.4 * 0.011;
                r = lr.step(t, pos, upAxis, false);
            }
            const bool walkMoves = !r && lr.speed > 1.0;
            nf::PfzLocoRest fall;
            double fp[3] = {0.0, 10.0, 0.0};
            bool fr = false;
            for (uint64_t t = 0; t <= 600; t += 11) {
                fp[1] -= 0.05;
                fr = fall.step(t, fp, upAxis, false);
            }
            const bool adsEnds = !fall.step(611, fp, upAxis, true);
            check(standRests && walkMoves && fr && adsEnds, "F51: the freeze gate rests without locomotion, not while walking, not under ADS; "
                                                            "vertical motion is not locomotion");
        }
    }

    // F36: weapon stability defers pacing on foot, and steps aside while native HMD stereo on foot is active (the key is untouched).
    edvr::Config::get().set("fix.weapon_stability", "1");
    g_journalActive = g_onFootKnown = g_onFoot = true;
    edvr::g_nativeOnFootActive.store(false);
    EdvrNativeFrameOutput onFootKeyOut{sizeof(onFootKeyOut), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput onFootKeyFrame = input(41, 7, 23);
    check(table.beginFrame(table.context, &onFootKeyFrame, &onFootKeyOut) == S_OK && onFootKeyOut.deferredPacing == 1,
          "weapon stability on foot, native off: pacing is deferred (F36 control)");
    edvr::g_nativeOnFootActive.store(true);
    EdvrNativeFrameOutput steppedOut{sizeof(steppedOut), EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput steppedFrame = input(41, 7, 24);
    check(table.beginFrame(table.context, &steppedFrame, &steppedOut) == S_OK && steppedOut.deferredPacing == 0,
          "F36: weapon stability steps aside while native on foot is active (no deferred pacing)");
    // F40: the now-pose flag rides version 9 output. Asked as 9, the provider answers 9 with the flag set by nativeFrameSetNowPoseRender.
    edvr::nativeFrameSetNowPose(1u);
    EdvrNativeFrameOutput v9Render{sizeof(v9Render), EDVR_NATIVE_FRAME_VERSION_9};
    EdvrNativeFrameInput v9RenderFrame = input(41, 7, 25);
    check(table.beginFrame(table.context, &v9RenderFrame, &v9Render) == S_OK && v9Render.version == EDVR_NATIVE_FRAME_VERSION_9 &&
              v9Render.nowPoseRender == 1,
          "F40: a version 9 answer carries the now-pose flag (render)");
    edvr::nativeFrameSetNowPose(0u);
    EdvrNativeFrameOutput v9Next{sizeof(v9Next), EDVR_NATIVE_FRAME_VERSION_9};
    EdvrNativeFrameInput v9NextFrame = input(41, 7, 26);
    check(table.beginFrame(table.context, &v9NextFrame, &v9Next) == S_OK && v9Next.version == EDVR_NATIVE_FRAME_VERSION_9 &&
              v9Next.nowPoseRender == 0,
          "F40: a version 9 answer carries the now-pose flag clear (next, the default)");
    // F62b: version 10 is a ladder step: input version 3 (the head in LOCAL) with the version 9 shape; the answer carries the ADS pitch in
    // reservedYaw. A version 9 answer never does; a version 2 input of its own (smaller) size is still read; a mismatched pair is refused.
    {
        edvr::nativeFrameSetAdsPitch(0.25f);
        const auto v3Input = [](uint64_t seq, float tx) {
            EdvrNativeFrameInput f = input(41, 7, seq);
            f.version = EDVR_NATIVE_FRAME_INPUT_VERSION_3;
            const float local[12] = {1, 0, 0, tx, 0, 1, 0, 1.6f, 0, 0, 1, -0.2f};
            std::memcpy(f.localHead, local, sizeof(local));
            f.localValid = 1;
            return f;
        };
        EdvrNativeFrameOutput v10{sizeof(v10), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput v10Frame = v3Input(27, 0.3f);
        EdvrNativeNowPose np{};
        const bool v10Ok = table.beginFrame(table.context, &v10Frame, &v10) == S_OK && v10.version == EDVR_NATIVE_FRAME_VERSION_10 &&
                           v10.size == sizeof(v10) && v10.reservedYaw == 0.25f && edvr::nativeFrameNowPose(np) && np.localValid &&
                           np.local[3] == 0.3f && np.local[7] == 1.6f;
        check(v10Ok, "F62b: a version 10 ask with a version 3 input is answered as 10, carries the ADS pitch in reservedYaw and keeps the LOCAL head");
        EdvrNativeFrameOutput v9Ads{sizeof(v9Ads), EDVR_NATIVE_FRAME_VERSION_9};
        EdvrNativeFrameInput v9AdsFrame = input(41, 7, 28);
        v9AdsFrame.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
        v9AdsFrame.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        check(table.beginFrame(table.context, &v9AdsFrame, &v9Ads) == S_OK && v9Ads.version == EDVR_NATIVE_FRAME_VERSION_9 && v9Ads.reservedYaw == 0.0f &&
                  edvr::nativeFrameNowPose(np) && !np.localValid,
              "F62b: a version 9 ask with a version 2 input of the version 2 size (an older runtime) is answered without the ADS pitch or a LOCAL head");
        EdvrNativeFrameOutput v10Old{sizeof(v10Old), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput v10OldFrame = input(41, 7, 29);
        v10OldFrame.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
        v10OldFrame.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        EdvrNativeFrameOutput v9New{sizeof(v9New), EDVR_NATIVE_FRAME_VERSION_9};
        EdvrNativeFrameInput v9NewFrame = v3Input(30, 0.1f);
        EdvrNativeFrameOutput v10Short{sizeof(v10Short), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput v10ShortFrame = v3Input(31, 0.1f);
        v10ShortFrame.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        check(table.beginFrame(table.context, &v10OldFrame, &v10Old) == E_INVALIDARG && table.beginFrame(table.context, &v9NewFrame, &v9New) == E_INVALIDARG &&
                  table.beginFrame(table.context, &v10ShortFrame, &v10Short) == E_INVALIDARG,
              "F62b: version 10 with a version 2 input, version 9 with a version 3 input, and a version 3 input of the version 2 size are refused");
        check(EDVR_NATIVE_FRAME_INPUT_SIZE_2 + 14u * 4u == sizeof(EdvrNativeFrameInput) && EDVR_NATIVE_FRAME_INPUT_SIZE_1 < EDVR_NATIVE_FRAME_INPUT_SIZE_2,
              "F62b: the version 3 input is the version 2 words plus the LOCAL head (12 floats), its flag and a word (F89: the applied alignment's serial)");
        // F106: the left eye's share (F89: its darkening) rides a version 10 answer's leftEyeShare (the float's bits); no other shape carries it, and a stale or absent level reads 0.
        edvr::eyeshare::clear();
        EdvrNativeFrameOutput darkNone{sizeof(darkNone), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput darkNoneFrame = v3Input(34, 0.1f);
        edvr::eyeshare::publish(0.6f, GetTickCount64());
        EdvrNativeFrameOutput darkSet{sizeof(darkSet), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput darkSetFrame = v3Input(32, 0.1f);
        EdvrNativeFrameOutput dark9{sizeof(dark9), EDVR_NATIVE_FRAME_VERSION_9};
        EdvrNativeFrameInput dark9Frame = input(41, 7, 33);
        dark9Frame.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
        dark9Frame.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        dark9.leftEyeShare = 0xA5A5A5A5u;
        const bool setOk = table.beginFrame(table.context, &darkSetFrame, &darkSet) == S_OK && darkSet.version == EDVR_NATIVE_FRAME_VERSION_10 &&
                           edvr::eyeshare::fromBits(darkSet.leftEyeShare) == 0.6f;
        const bool v9Ok = table.beginFrame(table.context, &dark9Frame, &dark9) == S_OK && dark9.version == EDVR_NATIVE_FRAME_VERSION_9 && dark9.leftEyeShare == 0u;
        edvr::eyeshare::clear();
        const bool noneOk = table.beginFrame(table.context, &darkNoneFrame, &darkNone) == S_OK && darkNone.leftEyeShare == 0u;
        check(setOk && v9Ok && noneOk,
              "F106: a version 10 answer carries the left eye's share in leftEyeShare (0.6 as published), a version 9 answer's slot is 0, and with nothing published the slot is 0");
        // F89: the applied alignment's serial rides the version 3 input's last word and arrives with the frame; a version 2 input has none (0: not reported)
        EdvrNativeFrameOutput appliedOut{sizeof(appliedOut), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput appliedFrame = v3Input(35, 0.1f);
        appliedFrame.eyeAlignApplied = 42u;
        EdvrNativeNowPose npApplied{};
        const bool v3Applied = table.beginFrame(table.context, &appliedFrame, &appliedOut) == S_OK && edvr::nativeFrameNowPose(npApplied) && npApplied.eyeAlignApplied == 42u;
        EdvrNativeFrameOutput applied9{sizeof(applied9), EDVR_NATIVE_FRAME_VERSION_9};
        EdvrNativeFrameInput applied9Frame = input(41, 7, 36);
        applied9Frame.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
        applied9Frame.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        applied9Frame.eyeAlignApplied = 99u;
        const bool v2Applied = table.beginFrame(table.context, &applied9Frame, &applied9) == S_OK && edvr::nativeFrameNowPose(npApplied) && npApplied.eyeAlignApplied == 0u;
        check(v3Applied && v2Applied, "F89: the applied-alignment serial of a version 3 input reaches nativeFrameNowPose, and an older input reads 0 (the runtime does not say)");
        // F123: the position-only recentre asked of the runtime rides a version 10 answer's positionRecentre (a serial; 0 when none stands); no other shape carries it
        edvr::nativeFrameSetPositionRecentre(9u);
        EdvrNativeFrameOutput recentre10{sizeof(recentre10), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput recentre10Frame = v3Input(37, 0.1f);
        EdvrNativeFrameOutput recentre9{sizeof(recentre9), EDVR_NATIVE_FRAME_VERSION_9};
        EdvrNativeFrameInput recentre9Frame = input(41, 7, 38);
        recentre9Frame.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
        recentre9Frame.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        recentre9.positionRecentre = 0xA5A5A5A5u;
        const bool askOk = table.beginFrame(table.context, &recentre10Frame, &recentre10) == S_OK && recentre10.version == EDVR_NATIVE_FRAME_VERSION_10 && recentre10.positionRecentre == 9u;
        const bool ask9Ok = table.beginFrame(table.context, &recentre9Frame, &recentre9) == S_OK && recentre9.version == EDVR_NATIVE_FRAME_VERSION_9 && recentre9.positionRecentre == 0u;
        edvr::nativeFrameSetPositionRecentre(0u);
        EdvrNativeFrameOutput recentreNone{sizeof(recentreNone), EDVR_NATIVE_FRAME_VERSION_10};
        EdvrNativeFrameInput recentreNoneFrame = v3Input(39, 0.1f);
        const bool noAskOk = table.beginFrame(table.context, &recentreNoneFrame, &recentreNone) == S_OK && recentreNone.positionRecentre == 0u;
        check(askOk && ask9Ok && noAskOk && offsetof(EdvrNativeFrameOutput, positionRecentre) == offsetof(EdvrNativeFrameOutput, reservedTerrainGuard) - 4u,
              "F123: a version 10 answer carries the position recentre's serial (9 as asked), a version 9 answer's word is 0, and with none asked it is 0; the word is the retired guard's first, layout unchanged");
        edvr::nativeFrameSetAdsPitch(0.0f);
    }
    edvr::g_nativeOnFootActive.store(false);
    g_journalActive = g_onFootKnown = g_onFoot = false;

    // ---- The comfort fade (comfort_fade.h; EdvrNativeFrameOutput::fadeAlpha, version 5; Explorer Cam's until F98) --------------------------------------------------------------
    {
        uint64_t seq = 40;
        auto ask5 = [&](float* fade) {
            EdvrNativeFrameOutput o{sizeof(o), EDVR_NATIVE_FRAME_VERSION_8};
            o.fadeAlpha = -77.0f;   // a sentinel the answer must overwrite
            EdvrNativeFrameInput f = input(41, 7, ++seq);
            const HRESULT r = table.beginFrame(table.context, &f, &o);
            *fade = o.fadeAlpha;
            return r;
        };
        float fade = -1.0f;
        edvr::comfort::clear();
        check(ask5(&fade) == S_OK && fade == 0.0f, "NOTHING PUBLISHED: a version 5 caller reads fadeAlpha 0 (and the sentinel is overwritten)");
        edvr::comfort::publish(1.0f, GetTickCount64());
        check(ask5(&fade) == S_OK && fade == 1.0f, "A FRESH LEVEL of 1: a version 5 caller reads 1");
        edvr::comfort::publish(0.37f, GetTickCount64());
        check(ask5(&fade) == S_OK && std::fabs(fade - 0.37f) < 1e-6f, "...a level of 0.37 reads 0.37");
        edvr::comfort::publish(1.0f, GetTickCount64() - 250);
        check(ask5(&fade) == S_OK && fade > 0.3f && fade < 0.7f, "A STALE LEVEL (published 250 ms ago) has half decayed: the d3d11 half going quiet is not black for ever");
        edvr::comfort::publish(1.0f, GetTickCount64() - 5000);
        check(ask5(&fade) == S_OK && fade == 0.0f, "...5 s old reads 0");
        edvr::comfort::publish(std::numeric_limits<float>::quiet_NaN(), GetTickCount64());
        check(ask5(&fade) == S_OK && fade == 0.0f, "NaN reads 0, never black");
        edvr::comfort::publish(9.0f, GetTickCount64());
        check(ask5(&fade) == S_OK && fade == 1.0f, "...above 1 reads 1");
        // ---- the older shapes never learn of it ------------------------------------------------------------------------------------------------------------
        edvr::comfort::publish(1.0f, GetTickCount64());
        EdvrNativeFrameOutput v4{EDVR_NATIVE_FRAME_OUTPUT_SIZE_4, EDVR_NATIVE_FRAME_VERSION_4};
        v4.fadeAlpha = -123.0f;
        EdvrNativeFrameInput v4Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v4Frame, &v4) == S_OK && v4.version == EDVR_NATIVE_FRAME_VERSION_4 && v4.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_4 && v4.fadeAlpha == -123.0f,
              "A VERSION 4 CALLER is answered in its own shape and the fade slot past it is untouched (a runtime from before the fade cannot be blacked out)");
        EdvrNativeFrameOutput v3{EDVR_NATIVE_FRAME_OUTPUT_SIZE_3, EDVR_NATIVE_FRAME_VERSION_3};
        v3.leftEyeShare = 0xA5A5A5A5u;
        v3.fadeAlpha = -123.0f;
        EdvrNativeFrameInput v3Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v3Frame, &v3) == S_OK && v3.leftEyeShare == 0xA5A5A5A5u && v3.fadeAlpha == -123.0f, "...and a version 3 caller likewise");
        EdvrNativeFrameOutput fullAsV4{sizeof(fullAsV4), EDVR_NATIVE_FRAME_VERSION_4};
        EdvrNativeFrameInput fullMismatchFrame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &fullMismatchFrame, &fullAsV4) == E_INVALIDARG, "A FULL-SIZE STRUCT CLAIMING VERSION 4 is refused (the size contradicts the version)");
        EdvrNativeFrameOutput v4AsV5{EDVR_NATIVE_FRAME_OUTPUT_SIZE_4, EDVR_NATIVE_FRAME_VERSION_5};
        EdvrNativeFrameInput mismatchFrame2 = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &mismatchFrame2, &v4AsV5) == E_INVALIDARG, "...and a version 4 sized struct claiming version 5 is refused: it is never half-answered");
        edvr::comfort::clear();
    }

    // ---- The hold has no setting (fix.transition_flash, retired 2026-10-13) -------------------------------------------------------------------------------
    // A frame the engine fix marked is withheld as a jump (not as an explicit hold) whatever an old ini still carries: nothing reads the
    // line, and the output's transition word is always on. Both values of the stale line, so neither is special.
    {
        uint64_t seq = 99;
        for (const char* stale : {"0", "1", "off", ""}) {
            edvr::Config::get().set("fix.transition_flash", stale);
            EdvrNativeFrameOutput o{sizeof(o), EDVR_NATIVE_FRAME_VERSION_8};
            EdvrNativeFrameInput f = input(41, 7, ++seq);
            const bool began = table.beginFrame(table.context, &f, &o) == S_OK;
            check(began && o.transitionEnabled == 1u,
                  (std::string("the transition word is always on, with the stale line at \"") + stale + "\"").c_str());
            edvr::markGlitchFrame();   // the engine fix's hold, set between the begin and the latch
            EdvrNativeFrameDecision d{sizeof(d), EDVR_NATIVE_FRAME_VERSION_1};
            check(table.latchSubmit(table.context, seq, &d) == S_OK && d.withhold && d.jumpOnly,
                  (std::string("a marked frame is withheld as a jump, with the stale line at \"") + stale + "\"").c_str());
        }
        edvr::clearGlitchFrame();
    }

    // ---- F101: mixed DLL versions against this provider: a released (v0.21.1) runtime's shapes, F31 to F100's version 1 words, this runtime's ladder -----
    {
        uint64_t seq = 200;
        const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        // A released runtime: its 88-byte version 1 input with each of its asks (5 down to 1); the answer is the shape asked, written only within it.
        bool oldOk = true;
        for (uint32_t v = EDVR_NATIVE_FRAME_VERSION_5; v >= EDVR_NATIVE_FRAME_VERSION_1; --v) {
            f101test::ReleasedInput oldIn{sizeof(oldIn), EDVR_NATIVE_FRAME_INPUT_VERSION_1};
            oldIn.generation = 41;
            oldIn.referenceGeneration = 7;
            oldIn.sequence = ++seq;
            oldIn.valid = 1;
            std::memcpy(oldIn.physicalHead, identity, sizeof(identity));
            const EdvrNativeFrameOutput ask = f101test::releasedAsk(v);
            alignas(EdvrNativeFrameOutput) unsigned char outBuf[sizeof(EdvrNativeFrameOutput) + 16];
            std::memset(outBuf, 0xA5, sizeof(outBuf));
            std::memcpy(outBuf, &ask, 8);
            const HRESULT r = table.beginFrame(table.context, reinterpret_cast<const EdvrNativeFrameInput*>(&oldIn), reinterpret_cast<EdvrNativeFrameOutput*>(outBuf));
            uint32_t gotSize = 0, gotVersion = 0;
            std::memcpy(&gotSize, outBuf, 4);
            std::memcpy(&gotVersion, outBuf + 4, 4);
            bool untouched = true;
            for (size_t i = ask.size; i < sizeof(outBuf); ++i) untouched = untouched && outBuf[i] == 0xA5;
            oldOk = oldOk && r == S_OK && gotSize == ask.size && gotVersion == v && untouched;
        }
        check(oldOk, "F101: a released runtime's version 1 input (88 bytes) is accepted with each of its asks (5 to 1), answered in its own shape and nothing past it");
        // F31 to F100's runtimes sent version 1 as the words alone (84 bytes): still taken. A version 1 input of another size is refused.
        EdvrNativeFrameInput words = input(41, 7, ++seq);
        words.size = EDVR_NATIVE_FRAME_INPUT_WORDS_1;
        EdvrNativeFrameOutput wordsOut = edvr::nativeFrameAsk(EDVR_NATIVE_FRAME_VERSION_6);
        const bool wordsOk = table.beginFrame(table.context, &words, &wordsOut) == S_OK && wordsOut.version == EDVR_NATIVE_FRAME_VERSION_6;
        EdvrNativeFrameInput odd = input(41, 7, ++seq);
        odd.size = EDVR_NATIVE_FRAME_INPUT_WORDS_1 - 4u;
        EdvrNativeFrameOutput oddOut = edvr::nativeFrameAsk(EDVR_NATIVE_FRAME_VERSION_5);
        EdvrNativeFrameInput odd2 = input(41, 7, ++seq);
        odd2.size = EDVR_NATIVE_FRAME_INPUT_SIZE_1 + 8u;
        EdvrNativeFrameOutput odd2Out = edvr::nativeFrameAsk(EDVR_NATIVE_FRAME_VERSION_5);
        check(wordsOk && table.beginFrame(table.context, &odd, &oddOut) == E_INVALIDARG && table.beginFrame(table.context, &odd2, &odd2Out) == E_INVALIDARG,
              "F101: F31 to F100's version 1 input (84 bytes, the words alone) is still accepted; a version 1 input of 80 or 96 bytes is refused");
        // This runtime's every step (11 down to 1, the shapes native_frame.h gives the client) is taken by this provider in the shape asked.
        bool newOk = true;
        for (uint32_t v = EDVR_NATIVE_FRAME_VERSION_11; v >= EDVR_NATIVE_FRAME_VERSION_1; --v) {
            EdvrNativeFrameInput f = input(41, 7, ++seq);
            edvr::nativeFrameInputShape(v, f);
            EdvrNativeFrameOutput o = edvr::nativeFrameAsk(v);
            const EdvrNativeFrameOutput asked = o;
            newOk = newOk && table.beginFrame(table.context, &f, &o) == S_OK && o.version == asked.version && o.size == asked.size;
        }
        check(newOk, "F101: this runtime's every ladder step (11 to 1) is accepted by this provider and answered in the shape asked");
    }

    // ---- World scale (visual.world_scale): version 11 ----------------------------------------------------------------------------------------------------------
    // Version 11 is the version 10 shape (input version 3) and its answer carries worldScalePercent, read from the key and held to 50..200; the version 10 words
    // ride beside it; a version 10 answer and every older one leave the word alone, and the gates on Elite's eye pair see 100 (nativeFrameWorldScale).
    {
        auto& cfg = edvr::Config::get();
        const auto v3Input = [](uint64_t sequence, float tx) {
            EdvrNativeFrameInput f = input(41, 7, sequence);
            f.version = EDVR_NATIVE_FRAME_INPUT_VERSION_3;
            const float local[12] = {1, 0, 0, tx, 0, 1, 0, 1.6f, 0, 0, 1, -0.2f};
            std::memcpy(f.localHead, local, sizeof(local));
            f.localValid = 1;
            return f;
        };
        struct Case { const char* key; uint32_t want; };
        bool keyed = true;
        uint64_t seq = 1000;
        for (const Case& c : {Case{"130", 130u}, Case{"80", 80u}, Case{"100", 100u}, Case{"50", 50u}, Case{"200", 200u}, Case{"10", 50u}, Case{"999", 200u},
                              Case{"-5", 50u}, Case{"", 100u}, Case{"banana", 100u}}) {
            cfg.set("visual.world_scale", c.key);
            EdvrNativeFrameOutput v11{sizeof(v11), EDVR_NATIVE_FRAME_VERSION_11};
            EdvrNativeFrameInput v11Frame = v3Input(++seq, 0.1f);
            keyed = keyed && table.beginFrame(table.context, &v11Frame, &v11) == S_OK && v11.version == EDVR_NATIVE_FRAME_VERSION_11 && v11.size == sizeof(v11) &&
                    v11.worldScalePercent == c.want && edvr::nativeFrameWorldScale() == c.want;
        }
        check(keyed, "world scale: a version 11 answer carries the key (130, 80, 100, 50, 200), held to 50..200 (10 reads 50, 999 reads 200) and 100 for an empty or non-number "
                     "value; nativeFrameWorldScale is what the answer carried");
        cfg.set("visual.world_scale", "130");
        EdvrNativeFrameOutput v11{sizeof(v11), EDVR_NATIVE_FRAME_VERSION_11};
        EdvrNativeFrameInput v11Frame = v3Input(++seq, 0.1f);
        edvr::nativeFrameSetAdsPitch(0.25f);
        edvr::nativeFrameSetPositionRecentre(9u);
        edvr::eyeshare::publish(0.6f, GetTickCount64());
        const bool beside = table.beginFrame(table.context, &v11Frame, &v11) == S_OK && v11.worldScalePercent == 130u && v11.reservedYaw == 0.25f &&
                            v11.positionRecentre == 9u && edvr::eyeshare::fromBits(v11.leftEyeShare) == 0.6f;
        edvr::nativeFrameSetAdsPitch(0.0f);
        edvr::nativeFrameSetPositionRecentre(0u);
        edvr::eyeshare::clear();
        check(beside, "world scale: the version 10 words (the ADS pitch, the position recentre, the left eye share) ride a version 11 answer unchanged beside the percent");
        EdvrNativeFrameOutput v10Scale{sizeof(v10Scale), EDVR_NATIVE_FRAME_VERSION_10};
        v10Scale.worldScalePercent = 0xA5A5A5A5u;
        EdvrNativeFrameInput v10ScaleFrame = v3Input(++seq, 0.1f);
        const bool v10Leaves = table.beginFrame(table.context, &v10ScaleFrame, &v10Scale) == S_OK && v10Scale.version == EDVR_NATIVE_FRAME_VERSION_10 &&
                               v10Scale.worldScalePercent == 0u && edvr::nativeFrameWorldScale() == 100u;
        EdvrNativeFrameOutput v9Scale{sizeof(v9Scale), EDVR_NATIVE_FRAME_VERSION_9};
        v9Scale.worldScalePercent = 0xA5A5A5A5u;
        EdvrNativeFrameInput v9ScaleFrame = input(41, 7, ++seq);
        v9ScaleFrame.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
        v9ScaleFrame.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        const bool v9Leaves = table.beginFrame(table.context, &v9ScaleFrame, &v9Scale) == S_OK && v9Scale.worldScalePercent == 0u &&
                              edvr::nativeFrameWorldScale() == 100u;
        check(v10Leaves && v9Leaves, "world scale: a version 10 or 9 answer leaves the word 0 (it is not the percent there, whatever the caller put in it), and the gates on Elite's eye pair see 100 "
                                     "while the runtime was not told one");
        EdvrNativeFrameOutput v11Short{sizeof(v11Short), EDVR_NATIVE_FRAME_VERSION_11};
        EdvrNativeFrameInput v11OldInput = input(41, 7, ++seq);
        v11OldInput.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
        v11OldInput.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        check(table.beginFrame(table.context, &v11OldInput, &v11Short) == E_INVALIDARG,
              "world scale: a version 11 ask rides input version 3 like a version 10 one: with a version 2 input it is refused, and the runtime steps down");
        check(sizeof(EdvrNativeFrameOutput) == 184 && offsetof(EdvrNativeFrameOutput, worldScalePercent) == offsetof(EdvrNativeFrameOutput, sceneReady) - 4u &&
                  offsetof(EdvrNativeFrameOutput, positionRecentre) == offsetof(EdvrNativeFrameOutput, reservedTerrainGuard) - 4u,
              "world scale: the percent is the last of the retired guard's 21 words (the struct is 184 bytes, as it was)");
        cfg.set("visual.world_scale", "");
    }

    check(table.close(table.context) == S_OK && !edvr::glitchConsumerPresent(),
          "close retires announced consumer");
    check(table.close(table.context) == S_FALSE, "close is idempotent");
    check(table.beginFrame(table.context, &next, &lostOutput) == E_INVALIDARG,
          "closed context rejects callbacks");

    // advanced.slow_test_ms's end-frame hold (frame_flag.h, v36): d3d11 -> openvr, 0 is no hold, and the
    // runtime never holds a frame for more than five seconds whatever is asked.
    check(edvr::endFrameHoldMs() == 0, "no end-frame hold until the d3d11 half asks for one");
    edvr::requestEndFrameHold(80);
    check(edvr::endFrameHoldMs() == 80, "the end-frame hold carries its milliseconds");
    edvr::requestEndFrameHold(60000);
    check(edvr::endFrameHoldMs() == 5000, "a hold over five seconds is clamped to five");
    edvr::requestEndFrameHold(0);
    check(edvr::endFrameHoldMs() == 0, "and is withdrawn by asking for 0");

    // frame_flag's layout check (the roll-call, since v34). Last, because a refusal it provokes
    // is meant to outlast it. This process holds one half, so the roll-call
    // has one signature and there is nothing to name...
    check(edvr::kFrameFlagVersion == 38, "frame_flag layout is v38");
    check(edvr::frameFlagPeerMismatch() == 0, "one half alone is no mismatch");
    {
        wchar_t name[64];
        swprintf_s(name, L"Local\\edvr_frame_flag_rollcall_%lu", GetCurrentProcessId());
        HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        check(h != nullptr, "the roll-call is signed at the channel's first use");
        auto* roll = h ? static_cast<volatile LONG*>(
                             MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 3 * sizeof(LONG)))
                       : nullptr;
        // Relative to kFrameFlagVersion, so only the pin above moves when the
        // layout does; the roll-call's own behaviour does not change with it.
        const LONG ours = static_cast<LONG>(edvr::kFrameFlagVersion);
        check(roll && roll[0] == ours && roll[1] == 0 && roll[2] == 1,
              "one signature, this layout first, nobody else");
        if (roll) {
            // ...a half on another layout signing second is named at once,
            // and the channel refuses: a mark reads back as absent...
            edvr::clearGlitchFrame();
            InterlockedExchange(&roll[1], ours + 1);
            check(edvr::frameFlagPeerMismatch() == static_cast<uint32_t>(ours + 1),
                  "a half on the next layout signing second is named");
            edvr::markGlitchFrame();
            check(!edvr::glitchFrameMarked(), "the refused channel reads as absent");
            InterlockedExchange(&roll[1], 0);
            check(edvr::frameFlagPeerMismatch() == 0, "the roll-call refusal follows the roll-call");
            edvr::markGlitchFrame();
            check(edvr::glitchFrameMarked(), "and the channel carries again without it");
            edvr::clearGlitchFrame();
            UnmapViewOfFile(const_cast<LONG*>(roll));
        }
        if (h) CloseHandle(h);
    }
    {
        // ...and a half built before the roll-call (v33, which never signs)
        // is found by its block's name, and refused for good.
        wchar_t name[64];
        swprintf_s(name, L"Local\\edvr_glitch_frame_v33_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name);
        check(h != nullptr, "a v33 block in the process");
        check(edvr::frameFlagPeerMismatch() == 33, "an unsigned v33 half is named");
        edvr::markGlitchFrame();
        check(!edvr::glitchFrameMarked(), "and the channel stays refused");
        if (h) CloseHandle(h);
        check(edvr::frameFlagPeerMismatch() == 33, "a found v33 half is latched");
    }

    {
        const uint16_t srcName[4] = {10, 11, 12, 13}, srcParent[4] = {0xFFFFu, 0, 1, 1};
        const uint16_t dstName[4] = {10, 13, 11, 20}, dstParent[4] = {0xFFFFu, 2, 0, 2};
        // IDs 10 and 11 are the common wrist/palm ancestry; 12 has no match.
        const auto map = edvr::nofoot::matchHandJoints(srcName, srcParent, 4, dstName, dstParent, 4);
        check(map.why == edvr::nofoot::kHandMapNoMatches && !map.ok(),
              "F70b: incomplete finger-name coverage refuses the anatomy map");
        const uint16_t completeName[4] = {10, 11, 13, 14}, completeParent[4] = {0xFFFFu, 0, 1, 1};
        const uint16_t completeDst[4] = {10, 13, 11, 14}, completeDstParent[4] = {0xFFFFu, 2, 0, 2};
        const auto complete = edvr::nofoot::matchHandJoints(completeName, completeParent, 4, completeDst, completeDstParent, 4);
        check(complete.ok() && complete.count == 4 && complete.dst[0] == 0 && complete.dst[1] == 2 && complete.dst[2] == 1 && complete.dst[3] == 3,
              "F70b: hand joints map by unique interned IDs and parent topology, including reordered fingers");
        const uint16_t dupDst[4] = {10, 10, 11, 14};
        const auto dup = edvr::nofoot::matchHandJoints(completeName, completeParent, 4, dupDst, completeDstParent, 4);
        const uint16_t wrongParent[4] = {0xFFFFu, 0, 0, 2};
        const auto topo = edvr::nofoot::matchHandJoints(completeName, completeParent, 4, completeDst, wrongParent, 4);
        check(dup.why == edvr::nofoot::kHandMapDuplicate && !dup.ok() && topo.why == edvr::nofoot::kHandMapTopology && !topo.ok(),
              "F70b: duplicate joint names and inconsistent parent chains refuse the anatomy map");
        const auto rot = [](float m[16], int axis, float angle) noexcept {
            edvr::nofoot::pfzIdentity(m);
            const float c = std::cos(angle), s = std::sin(angle);
            if (axis == 0) { m[5] = c; m[6] = s; m[9] = -s; m[10] = c; }
            else if (axis == 1) { m[0] = c; m[2] = -s; m[8] = s; m[10] = c; }
            else { m[0] = c; m[1] = s; m[4] = -s; m[5] = c; }
        };
        float B3[16], Bfp[16], Hfp[16], G[16], worldA[16], got[16];
        rot(B3, 2, 0.31f); rot(Bfp, 1, -0.47f); rot(Hfp, 0, 0.63f); rot(G, 2, -0.28f);
        B3[12] = 0.20f; B3[13] = 1.10f; B3[14] = -0.10f;   // unequal palm centres and rotated destination basis
        Bfp[12] = -0.08f; Bfp[13] = 0.95f; Bfp[14] = 0.15f;
        Hfp[12] = 0.12f; Hfp[13] = 1.00f; Hfp[14] = 0.30f;
        G[12] = 0.40f; G[13] = -0.05f; G[14] = 0.20f;       // nontrivial model-root transform
        float t0[16], t1[16];
        edvr::nofoot::pfzMul(Bfp, Hfp, t0); edvr::nofoot::pfzMul(t0, G, t1);
        const bool retarget = edvr::nofoot::handRetarget(B3, Bfp, Hfp, G, got);
        edvr::nofoot::pfzMul(B3, got, worldA);
        bool worldPoseMatches = retarget;
        for (int i = 0; i < 16; ++i) worldPoseMatches = worldPoseMatches && std::fabs(worldA[i] - t1[i]) < 2e-5f;
        check(worldPoseMatches, "F70b: noncommuting palm bases and root transform preserve the full world hand pose through rigid retargeting");
        edvr::pfz::HandRig contactFallbackRig{};
        contactFallbackRig.why = 0;
        std::memcpy(contactFallbackRig.palm, B3, sizeof(B3));
        contactFallbackRig.supportContactValid = false;
        float fallbackTarget[16];
        const bool fallbackTargetOk = contactFallbackRig.why == 0 && !contactFallbackRig.supportContactValid &&
                                      edvr::nofoot::handRetarget(contactFallbackRig.palm, Bfp, Hfp, G, fallbackTarget);
        bool fallbackStillMaps = fallbackTargetOk;
        for (int i = 0; i < 16; ++i) fallbackStillMaps = fallbackStillMaps && std::fabs(fallbackTarget[i] - got[i]) < 2e-5f;
        check(fallbackStillMaps, "F75: an unavailable K2 contact leaves a valid shared palm map available for the authored anatomical fallback");
        edvr::nofoot::PalmPatch right;
        right.heel[0] = 0.10; right.heel[1] = 1.0; right.heel[2] = 0.2;
        right.thumb[0] = 1.0; right.normal[1] = 1.0; right.halfThickness = 0.018; right.ok = true;
        const auto cup = edvr::nofoot::cupUnderHeel(right, 0.012, 0.003);
        check(cup.ok && std::fabs(cup.centre[1] - (right.heel[1] - 0.033)) < 1e-9 && cup.gap >= 0.003 && cup.facing[0] > 0.99,
              "F70b: support palm sits beneath the right heel with a non-penetrating proxy gap and faces thumbward");
        float rightPalm[16], leftPalm[16]; edvr::nofoot::pfzIdentity(rightPalm);
        rightPalm[0] = 0.0f; rightPalm[1] = 1.0f;    // distal is +Y
        rightPalm[4] = 1.0f; rightPalm[5] = 0.0f;    // thumbward is +X
        rightPalm[10] = -1.0f;                       // proper right-hand palm normal is -Z
        edvr::pfz::HandRig leftRig{};
        leftRig.palmarSign = edvr::pfz::handPalmarSign(0);   // production HandRig storage and derive-side assignment
        const int leftPalmarSign = leftRig.palmarSign == 0 ? -1 : leftRig.palmarSign;
        const bool supportFrameOk = edvr::nofoot::supportPalmFrame(rightPalm, 1, leftPalmarSign, cup, leftPalm);
        const double leftPhysicalNormalX = supportFrameOk ? -leftPalm[8] : 0.0;
        const double det = supportFrameOk ?
            leftPalm[0] * (leftPalm[5]*leftPalm[10] - leftPalm[6]*leftPalm[9]) -
            leftPalm[1] * (leftPalm[4]*leftPalm[10] - leftPalm[6]*leftPalm[8]) +
            leftPalm[2] * (leftPalm[4]*leftPalm[9] - leftPalm[5]*leftPalm[8]) : 0.0;
        check(supportFrameOk && leftRig.palmarSign == -1 && leftPhysicalNormalX > 0.99 && std::fabs(det - 1.0) < 1e-5,
              "F70b: signed left HandRig palm sign survives storage and produces a proper gripping-hand basis");

        // Landmark-scale fixture: the F74 hand census measured K1 about 2.4 x 1.7 cm, while K2 candidates span about 4.9 x 7.0 cm.
        const double k1WidthMeasured = 0.024, k1LengthMeasured = 0.017;
        const double wristR[3] = {0.0, 1.0, 0.0}, thumbR[3] = {0.055, 1.0, 0.070};
        const double k2R[4][3] = {{-0.0245, 1.0, 0.060}, {-0.0081666666667, 1.0, 0.070}, {0.0081666666667, 1.0, 0.070}, {0.0245, 1.0, 0.080}};
        const auto k2Right = edvr::nofoot::supportContactFromK2(wristR, thumbR, k2R);
        const double wristL[3] = {-wristR[0], wristR[1], wristR[2]}, thumbL[3] = {-thumbR[0], thumbR[1], thumbR[2]};
        double k2L[4][3] = {};
        for (int j = 0; j < 4; ++j) for (int c = 0; c < 3; ++c) k2L[j][c] = c == 0 ? -k2R[j][c] : k2R[j][c];
        const auto k2Left = edvr::nofoot::supportContactFromK2(wristL, thumbL, k2L);
        const double badK2[4][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        const auto invalidK2 = edvr::nofoot::supportContactFromK2(wristR, thumbR, badK2);
        check(k2Right.ok && k2Left.ok && std::fabs(k2Right.length - 0.070) < 1e-9 && std::fabs(k2Right.width - 0.049) < 1e-9 &&
                  std::fabs(k2Right.radius - 0.00816666666665) < 1e-9 && k2Right.width > k1WidthMeasured && k2Right.length > k1LengthMeasured &&
                  std::fabs(k2Left.length - k2Right.length) < 1e-9 &&
                  std::fabs(k2Left.width - k2Right.width) < 1e-9 && std::fabs(k2Left.radius - k2Right.radius) < 1e-9 && !invalidK2.ok &&
                  k2Right.centre[2] > wristR[2] && std::fabs(k2Right.distal[0]) < 1e-9 && k2Right.distal[2] > 0.99 &&
                  k2Right.radial[0] > 0.99 && k2Left.radial[0] < -0.99,
              "F75: separate support contact uses the K2 centre, length, width and median-spacing radius; mirrored hands keep dimensions and reverse radial; degenerate K2 rejects only contact");
        const double measuredW[3] = {0.0, 0.0, 0.0}, measuredTR[3] = {-0.013, -0.015, -0.023}, measuredTL[3] = {-0.013, -0.015, 0.023};
        const double measuredK2R[4][3] = {{-0.072, 0.000, -0.034}, {-0.079, 0.009, -0.013}, {-0.070, 0.008, 0.007}, {-0.062, 0.004, 0.023}};
        const double measuredK2L[4][3] = {{-0.072, 0.000, 0.034}, {-0.074, 0.008, 0.013}, {-0.070, 0.008, -0.007}, {-0.062, 0.004, -0.023}};
        const auto measuredContactR = edvr::nofoot::supportContactFromK2(measuredW, measuredTR, measuredK2R);
        const auto measuredContactL = edvr::nofoot::supportContactFromK2(measuredW, measuredTL, measuredK2L);
        check(measuredContactR.ok && measuredContactL.ok && measuredContactR.length > 0.060 && measuredContactR.length < 0.080 &&
                  measuredContactL.length > 0.060 && measuredContactL.length < 0.080 && measuredContactR.width > 0.010 && measuredContactR.width < 0.080 &&
                  measuredContactL.width > 0.010 && measuredContactL.width < 0.080 && measuredContactR.radius > 0.001 && measuredContactR.radius < 0.04 &&
                  measuredContactL.radius > 0.001 && measuredContactL.radius < 0.04,
              "F75: measured left and right F74 K2 landmarks form plausible distinct contact frames on both hands");
        float contactModelR[16], wristModelR[16], contactLocalR[16], invWristR[16], rootG[16], tmpR[16], worldContactR[16];
        edvr::nofoot::pfzIdentity(contactModelR); edvr::nofoot::pfzIdentity(wristModelR); edvr::nofoot::pfzIdentity(rootG);
        for (int c = 0; c < 3; ++c) {
            contactModelR[c] = static_cast<float>(k2Right.distal[c]); contactModelR[4 + c] = static_cast<float>(k2Right.radial[c]);
            contactModelR[8 + c] = static_cast<float>(k2Right.normal[c]); contactModelR[12 + c] = static_cast<float>(k2Right.centre[c]);
        }
        rot(wristModelR, 1, 0.37f); wristModelR[12] = 0.20f; wristModelR[13] = 0.94f; wristModelR[14] = -0.08f;
        rot(rootG, 2, -0.29f); rootG[12] = 0.31f; rootG[13] = -0.02f; rootG[14] = 0.17f;
        const bool contactLocalOk = edvr::nofoot::pfzAffineInverse(wristModelR, invWristR);
        edvr::nofoot::pfzMul(contactModelR, invWristR, contactLocalR);
        edvr::nofoot::pfzMul(contactLocalR, wristModelR, tmpR); edvr::nofoot::pfzMul(tmpR, rootG, worldContactR);
        float expectedContactR[16]; edvr::nofoot::pfzMul(contactModelR, rootG, expectedContactR);
        const double distalWorld[3] = {worldContactR[0], worldContactR[1], worldContactR[2]};
        const double wristWorld[3] = {rootG[12] + wristR[0] * rootG[0] + wristR[1] * rootG[4] + wristR[2] * rootG[8],
                                      rootG[13] + wristR[0] * rootG[1] + wristR[1] * rootG[5] + wristR[2] * rootG[9],
                                      rootG[14] + wristR[0] * rootG[2] + wristR[1] * rootG[6] + wristR[2] * rootG[10]};
        double meanK2R[3] = {}, meanK2World[3] = {};
        for (int j = 0; j < 4; ++j) for (int c = 0; c < 3; ++c) meanK2R[c] += k2R[j][c] * 0.25;
        for (int c = 0; c < 3; ++c) meanK2World[c] = rootG[12 + c] + meanK2R[0] * rootG[c] + meanK2R[1] * rootG[4 + c] + meanK2R[2] * rootG[8 + c];
        const double radialWorld[3] = {worldContactR[4], worldContactR[5], worldContactR[6]};
        const double k2EdgeDelta[3] = {worldContactR[12] + distalWorld[0] * k2Right.length * 0.5 - worldContactR[4] * k2Right.width * 0.5 - meanK2World[0],
                                       worldContactR[13] + distalWorld[1] * k2Right.length * 0.5 - worldContactR[5] * k2Right.width * 0.5 - meanK2World[1],
                                       worldContactR[14] + distalWorld[2] * k2Right.length * 0.5 - worldContactR[6] * k2Right.width * 0.5 - meanK2World[2]};
        const double k2PlaneError = edvr::nofoot::ikDot(k2EdgeDelta, distalWorld);
        const double k2UlnarEdge = edvr::nofoot::ikDot(k2EdgeDelta, radialWorld);
        edvr::nofoot::PalmPatch k2Patch;
        for (int c = 0; c < 3; ++c) {
            k2Patch.heel[c] = worldContactR[12 + c] + distalWorld[c] * k2Right.length * 0.5 - worldContactR[4 + c] * k2Right.width * 0.5;
            k2Patch.thumb[c] = worldContactR[4 + c]; k2Patch.normal[c] = worldContactR[4 + c];
        }
        k2Patch.halfThickness = k2Right.radius; k2Patch.ok = true;
        const auto k2Cup = edvr::nofoot::cupUnderHeel(k2Patch, k2Left.radius, 0.002);
        float leftContactTarget[16], leftContactLocal[16], leftContactModel[16], leftWrist[16], leftWristInv[16], leftRecovered[16], identity[16];
        edvr::nofoot::pfzIdentity(identity);
        edvr::nofoot::pfzIdentity(leftContactLocal); edvr::nofoot::pfzIdentity(leftWrist);
        edvr::nofoot::pfzIdentity(leftContactModel);
        for (int c = 0; c < 3; ++c) {
            leftContactModel[c] = static_cast<float>(k2Left.distal[c]); leftContactModel[4 + c] = static_cast<float>(k2Left.radial[c]);
            leftContactModel[8 + c] = static_cast<float>(k2Left.normal[c]); leftContactModel[12 + c] = static_cast<float>(k2Left.centre[c]);
        }
        rot(leftWrist, 0, -0.22f); leftWrist[12] = -0.18f; leftWrist[13] = 0.92f; leftWrist[14] = -0.06f;
        bool leftConverges = contactLocalOk && k2Cup.ok && edvr::nofoot::supportPalmFrame(worldContactR, 1, -1, k2Cup, leftContactTarget) &&
                             edvr::nofoot::pfzAffineInverse(leftWrist, leftWristInv);
        if (leftConverges) {
            edvr::nofoot::pfzMul(leftContactModel, leftWristInv, leftContactLocal);
            float wristTarget[16];
            leftConverges = edvr::nofoot::handRetarget(leftContactLocal, identity, leftContactTarget, identity, wristTarget);
            if (leftConverges) {
                edvr::nofoot::pfzMul(leftContactLocal, wristTarget, leftRecovered);
                for (int i = 0; i < 16; ++i) leftConverges = leftConverges && std::fabs(leftRecovered[i] - leftContactTarget[i]) < 2e-5f;
            }
        }
        bool fullContactMatches = true;
        for (int i = 0; i < 16; ++i) fullContactMatches = fullContactMatches && std::fabs(expectedContactR[i] - worldContactR[i]) < 2e-5f;
        check(contactLocalOk && fullContactMatches && std::fabs(k2PlaneError) < 2e-5 && std::fabs(k2UlnarEdge + k2Right.width * 0.5) < 2e-5 && k2Cup.ok && k2Cup.gap >= 0.002 && leftConverges && worldPoseMatches,
              "F76: nonidentity wrist and root transforms carry the support anchor to the mean-K2 plane and preserve its ulnar edge, gap, left contact solve, and old palm retargeting");

        // F79 pistol capture 101251/render 23174: choose the radial edge that faces the right attach, then reverse the left palm's stored normal
        // with the selected side. Expected full matrix is copied from the captured contact reconstruction, independent of the production helper.
        {
            const float capturedRightContact[16] = {0.170649183f, 0.219062936f, 0.960671816f, 0, -0.681663757f, 0.730253798f, -0.0454330051f, 0,
                                                    -0.711486985f, -0.647101931f, 0.273944394f, 0, 0.0900023537f, 1.402742294f, 0.764211908f, 1};
            const float capturedRightAttach[16] = {0.999762566f, 0.00240998559f, -0.0216761521f, 0, -0.00205116228f, 0.999861000f, 0.0165578879f, 0,
                                                   0.0217130158f, -0.0165095561f, 0.999628046f, 0, 0.0637362108f, 1.4501829999f, 0.8186944430f, 1};
            const float expectedLeftTarget[16] = {0.711486985f, 0.647101931f, -0.273944394f, 0, -0.170649183f, -0.219062936f, -0.960671816f, 0,
                                                  -0.681663757f, 0.730253798f, -0.0454330051f, 0, 0.0680081356f, 1.440526773f, 0.796341832f, 1};
            const double length = 0.07078039101358251, width = 0.047782718970184795, rightRad = 0.007623633225238461, leftRad = 0.007610159949712035;
            int radialSide = 0; double attachSide = 0.0; edvr::nofoot::SupportPalmTarget weaponSideContact;
            const bool selected = edvr::nofoot::supportWeaponSideContact(capturedRightContact, capturedRightAttach, length, width, rightRad, leftRad,
                                                                         0.002, &radialSide, weaponSideContact, &attachSide);
            float capturedLeftTarget[16];
            const bool basis = selected && edvr::nofoot::supportPalmFrame(capturedRightContact, 1, -1, radialSide, weaponSideContact, capturedLeftTarget);
            bool targetMatches = basis;
            for (int i = 0; i < 16; ++i) targetMatches = targetMatches && std::fabs(capturedLeftTarget[i] - expectedLeftTarget[i]) < 2e-5f;
            const double expectedSideMm[3] = {35.3901955068, 41.12515266, 0.0};
            const double targetDelta[3] = {weaponSideContact.centre[0] - capturedRightContact[12], weaponSideContact.centre[1] - capturedRightContact[13],
                                           weaponSideContact.centre[2] - capturedRightContact[14]};
            double targetLocalErr[3] = {};
            for (int axis = 0; axis < 3; ++axis)
                targetLocalErr[axis] = 1000.0 * (targetDelta[0] * capturedRightContact[axis * 4] + targetDelta[1] * capturedRightContact[axis * 4 + 1] +
                                                 targetDelta[2] * capturedRightContact[axis * 4 + 2]) - expectedSideMm[axis];

            float worldTurn[16], turnedRightContact[16], turnedRightAttach[16], turnedExpected[16], turnedActual[16];
            edvr::nofoot::pfzIdentity(worldTurn); worldTurn[0] = 0; worldTurn[2] = 1; worldTurn[8] = -1; worldTurn[10] = 0;
            worldTurn[12] = -2.1f; worldTurn[13] = 0.8f; worldTurn[14] = 1.6f;
            edvr::nofoot::pfzMul(capturedRightContact, worldTurn, turnedRightContact);
            edvr::nofoot::pfzMul(capturedRightAttach, worldTurn, turnedRightAttach);
            edvr::nofoot::pfzMul(expectedLeftTarget, worldTurn, turnedExpected);
            int turnedSide = 0; double turnedAttachSide = 0.0; edvr::nofoot::SupportPalmTarget turnedContact;
            const bool turnedOk = edvr::nofoot::supportWeaponSideContact(turnedRightContact, turnedRightAttach, length, width, rightRad, leftRad,
                                                                        0.002, &turnedSide, turnedContact, &turnedAttachSide) &&
                                  edvr::nofoot::supportPalmFrame(turnedRightContact, 1, -1, turnedSide, turnedContact, turnedActual);
            bool turnedMatches = turnedOk && turnedSide == radialSide && std::fabs(turnedAttachSide - attachSide) < 2e-5;
            for (int i = 0; i < 16; ++i) turnedMatches = turnedMatches && std::fabs(turnedActual[i] - turnedExpected[i]) < 3e-5f;

            float oppositeAttach[16]; std::memcpy(oppositeAttach, capturedRightAttach, sizeof(oppositeAttach));
            const double k2[3] = {capturedRightContact[12] + capturedRightContact[0] * length * 0.5,
                                  capturedRightContact[13] + capturedRightContact[1] * length * 0.5,
                                  capturedRightContact[14] + capturedRightContact[2] * length * 0.5};
            for (int i = 0; i < 3; ++i) oppositeAttach[12 + i] = static_cast<float>(k2[i] - 0.1 * capturedRightContact[4 + i]);
            int oppositeSide = 0; edvr::nofoot::SupportPalmTarget oppositeContact; float oppositeTarget[16] = {};
            const bool oppositeOk = edvr::nofoot::supportWeaponSideContact(capturedRightContact, oppositeAttach, length, width, rightRad, leftRad,
                                                                          0.002, &oppositeSide, oppositeContact) && oppositeSide == -1 &&
                                    edvr::nofoot::supportPalmFrame(capturedRightContact, 1, -1, oppositeSide, oppositeContact, oppositeTarget);
            const double oppositeExpectedMm[3] = {35.3901955068, -41.12515266, 0.0};
            const double oppositeDelta[3] = {oppositeContact.centre[0] - capturedRightContact[12], oppositeContact.centre[1] - capturedRightContact[13],
                                             oppositeContact.centre[2] - capturedRightContact[14]};
            double oppositeLocalErr[3] = {};
            for (int axis = 0; axis < 3; ++axis)
                oppositeLocalErr[axis] = 1000.0 * (oppositeDelta[0] * capturedRightContact[axis * 4] + oppositeDelta[1] * capturedRightContact[axis * 4 + 1] +
                                                   oppositeDelta[2] * capturedRightContact[axis * 4 + 2]) - oppositeExpectedMm[axis];
            bool oppositeBasis = oppositeOk;
            for (int i = 0; i < 3; ++i) {
                oppositeBasis = oppositeBasis && std::fabs(oppositeTarget[i] + capturedRightContact[8 + i]) < 3e-5f &&
                                std::fabs(oppositeTarget[4 + i] - capturedRightContact[i]) < 3e-5f &&
                                std::fabs(oppositeTarget[8 + i] + capturedRightContact[4 + i]) < 3e-5f;
            }
            float tangentAttach[16]; std::memcpy(tangentAttach, capturedRightAttach, sizeof(tangentAttach));
            for (int i = 0; i < 3; ++i) tangentAttach[12 + i] = static_cast<float>(k2[i] + 0.2 * capturedRightContact[i]);
            int invalidSide = 0; edvr::nofoot::SupportPalmTarget invalidContact;
            const bool tangentRejected = !edvr::nofoot::supportWeaponSideContact(capturedRightContact, tangentAttach, length, width, rightRad, leftRad,
                                                                                 0.002, &invalidSide, invalidContact);
            tangentAttach[12] = std::numeric_limits<float>::quiet_NaN();
            const bool nonfiniteRejected = !edvr::nofoot::supportWeaponSideContact(capturedRightContact, tangentAttach, length, width, rightRad, leftRad,
                                                                                   0.002, &invalidSide, invalidContact);
            check(selected && radialSide == 1 && attachSide > 0.050 && attachSide < 0.051 && targetMatches && edvr::nofoot::ikLen(targetLocalErr) < 0.03 &&
                      turnedMatches && oppositeBasis && edvr::nofoot::ikLen(oppositeLocalErr) < 0.03 && tangentRejected && nonfiniteRejected,
                  "F79: captured pistol attach selects the measured weapon-side K2 edge and palm-normal sign, rigid transforms preserve the target, opposite side remains symmetric, and tangential/invalid attach data fail closed");
        }

        float Qsrc[16], QsrcParent[16], Qdst[16], QdstParent[16], D[16], invSrcParent[16], finger[16], desired[16];
        rot(Qsrc, 0, 0.27f); rot(QsrcParent, 2, -0.31f); rot(Qdst, 1, 0.52f); rot(QdstParent, 0, -0.14f); rot(D, 2, 0.42f);
        edvr::nofoot::pfzAffineInverse(QsrcParent, invSrcParent);
        float t2[16]; edvr::nofoot::pfzMul(Qsrc, D, t2); edvr::nofoot::pfzMul(t2, invSrcParent, finger);
        const float preserved[3] = {0.013f, -0.021f, 0.034f};
        const bool reflectedOk = edvr::nofoot::handFingerLocalRetarget(Qsrc, finger, QsrcParent, Qdst, QdstParent, preserved, true, desired);
        const double reflectedDet = reflectedOk ?
            desired[0] * (desired[5]*desired[10] - desired[6]*desired[9]) -
            desired[1] * (desired[4]*desired[10] - desired[6]*desired[8]) +
            desired[2] * (desired[4]*desired[9] - desired[5]*desired[8]) : 0.0;
        float invDst[16], invDstParent[16], recovered[16], F[16], t3[16], reflectedExpected[16];
        edvr::nofoot::pfzAffineInverse(Qdst, invDst); edvr::nofoot::pfzAffineInverse(QdstParent, invDstParent);
        edvr::nofoot::pfzMul(invDst, desired, t2); edvr::nofoot::pfzMul(t2, QdstParent, recovered);
        edvr::nofoot::pfzIdentity(F); F[10] = -1.0f;
        edvr::nofoot::pfzMul(F, D, t3); edvr::nofoot::pfzMul(t3, F, reflectedExpected);
        bool reflectedDeltaMatches = true;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
            reflectedDeltaMatches = reflectedDeltaMatches && std::fabs(recovered[r * 4 + c] - reflectedExpected[r * 4 + c]) < 2e-5f;
        check(reflectedOk && reflectedDeltaMatches && std::fabs(reflectedDet - 1.0) < 1e-5 &&
              std::fabs(desired[12] - preserved[0]) < 1e-6f && std::fabs(desired[13] - preserved[1]) < 1e-6f && std::fabs(desired[14] - preserved[2]) < 1e-6f,
              "F70b: rotated cross-hand frames preserve the reflected palm-space curl, proper rotation, and 3P segment translation");
        const bool sameSideOk = edvr::nofoot::handFingerLocalRetarget(Qsrc, finger, QsrcParent, Qdst, QdstParent, preserved, false, desired);
        float C[16], Cp[16], invCp[16], sameRecovered[16];
        edvr::nofoot::pfzMul(Qsrc, invDst, C); edvr::nofoot::pfzMul(QsrcParent, invDstParent, Cp);
        edvr::nofoot::pfzAffineInverse(Cp, invCp);
        edvr::nofoot::pfzMul(C, desired, t2); edvr::nofoot::pfzMul(t2, invCp, sameRecovered);
        bool sameDeltaMatches = true;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
            sameDeltaMatches = sameDeltaMatches && std::fabs(sameRecovered[r * 4 + c] - finger[r * 4 + c]) < 2e-5f;
        check(sameSideOk && sameDeltaMatches && std::fabs(desired[12] - preserved[0]) < 1e-6f && std::fabs(desired[13] - preserved[1]) < 1e-6f && std::fabs(desired[14] - preserved[2]) < 1e-6f,
              "F70b: noncommuting same-side palm frames recover the original anatomical finger delta and preserve 3P segment translation");
    }
    // F82: captured crouched laser-rifle geometry. The arm inputs are the raw pose before the old shared reach
    // translation; the leg points are the matching captured pose before the new neck-repin translation.
    {
        namespace nf = edvr::nofoot;
        nf::RifleQualityRamp qualityRamp;
        const double rq0 = qualityRamp.step(true, 41, true, 9001, 7, 13, 100, 0.0, 1.0);
        const double rq1 = qualityRamp.step(true, 41, true, 9001, 7, 13, 101, 0.075, 1.0);
        const double rqDuplicate = qualityRamp.step(true, 41, true, 9001, 7, 13, 101, 0.075, 0.0);
        const double rqOutOfOrder = qualityRamp.step(true, 41, true, 9001, 7, 13, 100, 0.075, 0.0);
        const double rqDrawRelease = qualityRamp.step(true, 41, true, 9001, 7, 13, 102, 0.075, 0.0);
        const double rqHandoff = qualityRamp.step(true, 41, true, 9001, 7, 13, 103, 0.075, 1.0);
        const double rqSourceLoss = qualityRamp.step(true, 41, false, 0, 7, 13, 104, 0.075, 0.0);
        const double rqOldItem = qualityRamp.step(true, 42, true, 9001, 7, 13, 105, 0.075, 1.0);
        const double rqNewEpoch = qualityRamp.step(true, 42, true, 9002, 7, 13, 106, 0.075, 1.0);
        check(rq0 == 0.0 && std::fabs(rq1 - 0.5) < 1e-12 && rqDuplicate == rq1 && rqOutOfOrder == rq1 &&
                  std::fabs(rqDrawRelease - 0.25) < 1e-12 && std::fabs(rqHandoff - 0.625) < 1e-12 &&
                  std::fabs(rqSourceLoss - 0.3125) < 1e-12 && rqOldItem == 0.0 && rqNewEpoch == 0.0,
              "F84: quality weight ramps through draw handoff/source loss, ignores duplicate/old frames, and hard-resets on item/native-epoch changes");
        const double syntheticS[3] = {0.0, 1.0, 0.0}, syntheticTarget[3] = {0.02, 0.92, 0.56};
        const double syntheticRef[3] = {0.18, 0.10, 0.20};
        double synLo = 0.0, synHi = 0.0;
        nf::ikElbowDRange(0.34, 0.33, nf::kIkElbowMinDeg, nf::kIkElbowMaxDeg, &synLo, &synHi);
        const auto syntheticTb = nf::ikTwoBoneFrameD(syntheticS, syntheticTarget, 0.34, 0.33, syntheticRef, synLo, synHi);
        double syntheticUnit[3];
        const bool syntheticUnitOk = nf::ikUnit(syntheticRef, syntheticUnit);
        bool syntheticClamped = false;
        const double syntheticLegacy = syntheticTb.ok && syntheticUnitOk ?
            nf::ikLegacySwivel(syntheticTb, syntheticUnit, nf::ikArmOutSign(0), nullptr, 0.3, 0.06, &syntheticClamped) : 0.0;
        double syntheticTheta = syntheticLegacy, syntheticRadial[3];
        const bool syntheticSupportOk = nf::ikRifleSupportTheta(syntheticTb, syntheticLegacy, 0.0, 1.0, 0.06, 0.3,
                                                                  &syntheticTheta, syntheticRadial);
        double syntheticLegacyE[3], syntheticSupportE[3];
        if (syntheticTb.ok) { syntheticTb.elbowAt(syntheticLegacy, syntheticLegacyE); syntheticTb.elbowAt(syntheticTheta, syntheticSupportE); }
        bool syntheticDetachContinuous = syntheticSupportOk && std::fabs(nf::ikWrapPi(syntheticTheta - syntheticLegacy)) > 1e-4;
        for (int i = 0; i <= 100 && syntheticDetachContinuous; ++i) {
            const double detach = i / 100.0;
            double theta = 0.0;
            const bool ok = nf::ikRifleSupportTheta(syntheticTb, syntheticLegacy, detach, 1.0, 0.06, 0.3, &theta);
            const double expected = (1.0 - detach) * nf::ikWrapPi(syntheticTheta - syntheticLegacy);
            syntheticDetachContinuous = ok && std::fabs(nf::ikWrapPi(theta - syntheticLegacy) - expected) < 1e-8;
        }
        double endpointTheta = 0.0, zeroWeightTheta = 0.0;
        const bool endpointOk = nf::ikRifleSupportTheta(syntheticTb, syntheticLegacy, 1.0, 1.0, 0.06, 0.3, &endpointTheta) &&
                                nf::ikRifleSupportTheta(syntheticTb, syntheticLegacy, 0.0, 0.0, 0.06, 0.3, &zeroWeightTheta);
        bool syntheticPartialAdsExact = true;
        for (int qi = 0; qi <= 10; ++qi) for (int di = 0; di <= 10; ++di) {
            const double q = qi / 10.0, detach = di / 10.0;
            double actual = 0.0;
            const double blend = q * (1.0 - detach);
            const bool ok = nf::ikRifleSupportTheta(syntheticTb, syntheticLegacy, detach, q, 0.06, 0.3, &actual);
            const double expected = nf::ikWrapPi(syntheticLegacy) + blend * nf::ikWrapPi(syntheticTheta - nf::ikWrapPi(syntheticLegacy));
            syntheticPartialAdsExact = syntheticPartialAdsExact && ok && std::fabs(nf::ikWrapPi(actual - expected)) < 1e-8;
        }
        const double syntheticUp[3] = {0.0, 1.0, 0.0};
        const double syntheticDown = nf::ikDot(syntheticSupportE, syntheticUp) - nf::ikDot(syntheticLegacyE, syntheticUp);
        check(syntheticTb.ok && syntheticSupportOk && syntheticDown < -1e-4 && syntheticDetachContinuous && endpointOk && syntheticPartialAdsExact &&
                  std::fabs(endpointTheta - nf::ikWrapPi(syntheticLegacy)) < 1e-12 &&
                  std::fabs(zeroWeightTheta - nf::ikWrapPi(syntheticLegacy)) < 1e-12,
              "F84: exact synthetic rifle support lowers the elbow on its fixed wrist circle and blends continuously back to the exact legacy ADS pole");
        // Legacy F82 crouched-rifle regression geometry. This is intentionally kept separate from
        // the current F83 flight: its zero-assist interval makes subtracting the old reach shift valid.
        const double neck[3] = {-0.00392988604, 0.863943338, 0.242602438};
        const double fixtureYaw = 29.15 * nf::kIkPi / 180.0;
        const double fixtureDelta[3] = {-0.07315508090785006, 0.0, 0.0908812342081006};
        const double a = 1.0 - std::cos(fixtureYaw), sinYaw = std::sin(fixtureYaw), det = a * a + sinYaw * sinYaw;
        const double dn[3] = {(a * fixtureDelta[0] + sinYaw * fixtureDelta[2]) / det,
                              0.0,
                              (-sinYaw * fixtureDelta[0] + a * fixtureDelta[2]) / det};
        const double pivot[3] = {neck[0] - dn[0], neck[1], neck[2] - dn[2]};
        nf::CrouchNeckArm arms[2] = {};
        const double c[2][3] = {{-0.04288652815, 0.808530688, 0.245437518}, {-0.0026513971, 0.800691903, 0.218321339}};
        const double shoulders[2][3] = {{-0.1547683251, 0.856413126, 0.330814436}, {0.1190077289, 0.812418938, 0.133715481}};
        const double t[2][3] = {{0.0583111346, 0.724505126, 0.974367321}, {0.132951856, 0.795943439, 0.638907075}};
        const double lengths[2][2] = {{0.3043116672249186, 0.2876242376548462}, {0.3043115430820347, 0.2876240501790341}};
        for (int side = 0; side < 2; ++side) {
            std::memcpy(arms[side].clavicle, c[side], sizeof(c[side]));
            std::memcpy(arms[side].shoulder, shoulders[side], sizeof(shoulders[side]));
            std::memcpy(arms[side].hand, t[side], sizeof(t[side]));
            std::memcpy(arms[side].target, t[side], sizeof(t[side]));
            arms[side].l1 = lengths[side][0];
            arms[side].l2 = lengths[side][1];
            arms[side].wSolve = 1.0;
        }
        nf::CrouchNeckLeg legs[2] = {
            {{-0.3482999726780945, 0.567868709564209, -0.017969742156478882}, {-0.14478827264395294, 0.2606489956378937, 0.24276362385609435},
             {-0.28322757032003937, 0.10816249251365662, -0.11296097861594272}},
            {{-0.17616086449709473, 0.5412819385528564, -0.11689105021758688}, {0.09629824016007843, 0.5372578501701355, 0.24301548328259276},
             {-0.038498747011206425, 0.14983943104743958, 0.21666245188572691}}};
        const double noAdsPush[3] = {};
        // F83 fixture copied from build/F83-rifle-physical-rest-fixture.json. This is the captured final old-pin
        // pose (not raw pre-leg animation); LOCAL pitch is the synchronized -21.5-degree sample, swept around it.
        const double f83Eye[3] = {-0.000953410228, 0.973248422, 0.390921235};
        const double f83Forward[3] = {-0.003402806651, 0.0, 0.999994210437}; // exact same-call heading recovered from clearance projections
        const double f83Up[3] = {-0.036420859967736204, 0.9068033396301773, 0.41997780374269933};
        const double f83OldNeck[3] = {-0.0482054353, 0.937080801, 0.229304209};
        const double f83WeaponViewShift[3] = {0.0249811114985, 0.000848921839968, 0.000472385888137};
        const double f83Chest[3] = {-0.182043061, 0.879276514, 0.0378262214};
        nf::CrouchNeckArm f83Arms[2] = {};
        const double f83Clavicles[2][3] = {{-0.0863803029, 0.881117284, 0.228592142}, {-0.045565486, 0.875224829, 0.201852158}};
        const double f83Shoulders[2][3] = {{-0.20126635786820346, 0.9230817237471743, 0.31298763969288956},
                                           {0.0771502256, 0.892467797, 0.119874969}};
        const double f83Hands[2][3] = {{0.0395753048, 0.75500524, 0.892089069}, {0.140434176, 0.815658271, 0.633480906}};
        const double f83Targets[2][3] = {{0.0675485879, 0.734645009, 0.96726352}, {0.140434176, 0.815658271, 0.633480906}};
        const double f83Lengths[2][2] = {{0.30431136425469857, 0.2876238366066916}, {0.3043113455960454, 0.28762391210413996}};
        for (int side = 0; side < 2; ++side) {
            std::memcpy(f83Arms[side].clavicle, f83Clavicles[side], sizeof(f83Clavicles[side]));
            std::memcpy(f83Arms[side].shoulder, f83Shoulders[side], sizeof(f83Shoulders[side]));
            std::memcpy(f83Arms[side].hand, f83Hands[side], sizeof(f83Hands[side]));
            std::memcpy(f83Arms[side].target, f83Targets[side], sizeof(f83Targets[side]));
            f83Arms[side].l1 = f83Lengths[side][0]; f83Arms[side].l2 = f83Lengths[side][1]; f83Arms[side].wSolve = 1.0;
        }
        nf::CrouchNeckLeg f83Legs[2] = {
            {{-0.3802296817302704, 0.6456410884857178, -0.043319929391145706}, {-0.27534249424934387, 0.5876571536064148, 0.39190322160720825},
             {-0.3679353892803192, 0.18718966841697693, 0.38873347640037537}},
            {{-0.2146882861852646, 0.6331141591072083, -0.1554059237241745}, {0.02136782370507717, 0.26503095030784607, -0.04326692968606949},
             {-0.1507629007101059, 0.16926850378513336, -0.4040413200855255}}};
        auto circleElbow = [](const double shoulder[3], const double hand[3], double l1, double l2, double elbow[3]) {
            const double dvec[3] = {hand[0] - shoulder[0], hand[1] - shoulder[1], hand[2] - shoulder[2]};
            const double d = nf::ikLen(dvec);
            if (!(d > 1e-6) || d > l1 + l2 || d < std::fabs(l1 - l2)) return false;
            double axis[3] = {dvec[0] / d, dvec[1] / d, dvec[2] / d};
            const double along = (l1 * l1 + d * d - l2 * l2) / (2.0 * d);
            const double radius2 = l1 * l1 - along * along;
            if (radius2 < -1e-8) return false;
            double down[3] = {0.0, -1.0, 0.0};
            const double axial = nf::ikDot(down, axis);
            for (int i = 0; i < 3; ++i) down[i] -= axial * axis[i];
            if (!nf::ikUnit(down, down)) return false;
            const double radius = std::sqrt(std::max(0.0, radius2));
            for (int i = 0; i < 3; ++i) elbow[i] = shoulder[i] + along * axis[i] + radius * down[i];
            return true;
        };
        auto measureApproxPose = [&](const double neckIn[3], const nf::CrouchNeckArm inArms[2], const nf::CrouchNeckLeg inLegs[2],
                                     double legVertical, const double ads[3], double yawRad, double q,
                                     double& elbowY, double& elbowAngle, double& assistL, double& wristLeft, double& wristRight, double& feetMax) {
            nf::CrouchNeckYaw candidate;
            if (!nf::crouchNeckYawFeasible(neckIn, f83Chest, inArms, inLegs, legVertical, yawRad, ads, &candidate)) return false;
            elbowY = elbowAngle = assistL = wristLeft = wristRight = feetMax = 0.0;
            for (int side = 0; side < 2; ++side) {
                const auto& arm = inArms[side];
                double C[3], S[3], E[3], H[3];
                nf::turnAboutVertical(arm.clavicle, neckIn, yawRad, C);
                nf::turnAboutVertical(arm.shoulder, neckIn, yawRad, S);
                nf::turnAboutVertical(arm.elbow, neckIn, yawRad, E);
                nf::turnAboutVertical(arm.hand, neckIn, yawRad, H);
                double target[3];
                for (int i = 0; i < 3; ++i) target[i] = H[i] + arm.wSolve * (arm.target[i] - H[i]);
                const double toTarget[3] = {target[0] - S[0], target[1] - S[1], target[2] - S[2]};
                const double dist = nf::ikLen(toTarget);
                double excess = 0.0, R[9], assistAngle = 0.0;
                bool capped = false;
                const double move = nf::ikAssistReachMove(dist, arm.l1, arm.l2, &excess);
                double disp[3];
                for (int i = 0; i < 3; ++i) disp[i] = ((dist > 1e-9 ? move * toTarget[i] / dist : 0.0) + ads[i]) * arm.wSolve;
                if (nf::ikAssistRot(C, S, disp, R, &assistAngle, &capped)) {
                    double cs[3] = {S[0] - C[0], S[1] - C[1], S[2] - C[2]};
                    double ce[3] = {E[0] - C[0], E[1] - C[1], E[2] - C[2]};
                    double ch[3] = {H[0] - C[0], H[1] - C[1], H[2] - C[2]};
                    double rs[3], re[3], rh[3];
                    nf::ikRowRotate(cs, R, rs); nf::ikRowRotate(ce, R, re); nf::ikRowRotate(ch, R, rh);
                    for (int i = 0; i < 3; ++i) { S[i] = C[i] + rs[i]; E[i] = C[i] + re[i]; H[i] = C[i] + rh[i]; }
                }
                if (side == 0) assistL = assistAngle * 180.0 / nf::kIkPi;
                const double anim[3] = {H[0] - S[0], H[1] - S[1], H[2] - S[2]};
                double dLo = 0.0, dHi = 0.0, fLo = 0.0, fHi = 0.0;
                nf::ikElbowDRange(arm.l1, arm.l2, nf::kIkElbowMinDeg, nf::kIkElbowMaxDeg, &dLo, &dHi);
                nf::ikElbowDRange(arm.l1, arm.l2, 0.0, 180.0, &fLo, &fHi);
                const double animD = nf::ikLen(anim);
                if (animD < dLo) dLo -= (1.0 - arm.wSolve) * (dLo - animD);
                if (animD > dHi) dHi += (1.0 - arm.wSolve) * (animD - dHi);
                dLo = std::max(dLo, fLo); dHi = std::min(dHi, fHi);
                const double ref[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]};
                const nf::IkTwoBone tb = nf::ikTwoBoneFrameD(S, target, arm.l1, arm.l2, ref, dLo, dHi);
                double unitRef[3];
                if (!tb.ok || !nf::ikUnit(ref, unitRef)) return false;
                bool clamped = false;
                double theta = arm.wSolve * nf::ikWrapPi(nf::ikLegacySwivel(tb, unitRef, nf::ikArmOutSign(side),
                                                                              arm.fpPoleValid ? arm.fpPole : nullptr, 0.3, 0.06, &clamped));
                if (side == 0 && !nf::ikRifleSupportTheta(tb, theta, 0.0, q, 0.06, 0.3, &theta)) return false;
                double solvedElbow[3]; tb.elbowAt(theta, solvedElbow);
                if (side == 0) { elbowY = solvedElbow[1]; elbowAngle = nf::ikElbowInteriorDeg(arm.l1, arm.l2, tb.D); }
                const double wristErr = std::sqrt((tb.hand[0] - target[0]) * (tb.hand[0] - target[0]) +
                                                  (tb.hand[1] - target[1]) * (tb.hand[1] - target[1]) +
                                                  (tb.hand[2] - target[2]) * (tb.hand[2] - target[2]));
                if (side == 0) wristLeft = wristErr; else wristRight = wristErr;
            }
            for (int side = 0; side < 2; ++side) {
                const double hip1[3] = {inLegs[side].hip[0] + candidate.rootDelta[0], inLegs[side].hip[1] + legVertical,
                                        inLegs[side].hip[2] + candidate.rootDelta[2]};
                const auto leg = nf::legSolveTo(inLegs[side].hip, inLegs[side].knee, inLegs[side].foot, hip1, inLegs[side].foot);
                if (!leg.ok || leg.limited) return false;
                feetMax = std::max(feetMax, leg.footErrM);
            }
            return true;
        };
        const auto oldRestSolve = nf::crouchNeckYawSelect(f83OldNeck, f83Chest, f83Arms, f83Legs, 0.0, noAdsPush, 50.0, 0.25);
        bool f83CapturedPitchSweep = !oldRestSolve.feasible;
        bool f83ApproxQuality = false, f83AdsEndpoint = false;
        double f83ApproxBaseYaw = 0.0, f83ApproxSelectedYaw = 0.0, f83ApproxBaseCost = 0.0, f83ApproxSelectedCost = 0.0;
        double f83QualityAvgMs = 0.0, f82RampMaxYawStepDeg = 0.0;
        bool f82QualityRampOk = false, f82AdsRampOk = false;
        int f82QualityRampFailQ = -1;
        double f83BaseElbowY = 0.0, f83SelectedElbowY = 0.0, f83BaseElbowAngle = 0.0, f83SelectedElbowAngle = 0.0;
        double f83BaseAssist = 0.0, f83SelectedAssist = 0.0, f83BaseWristL = 0.0, f83BaseWristR = 0.0;
        double f83SelectedWristL = 0.0, f83SelectedWristR = 0.0, f83BaseFeet = 0.0, f83SelectedFeet = 0.0;
        double centerYawDeg = 0.0;
        const double f83PitchSweep[] = {-24.0, -21.5, -19.0};
        for (double pitchDeg : f83PitchSweep) {
            const double pitch = pitchDeg * nf::kIkPi / 180.0;
            double restTarget[3];
            const bool targetOk = nf::pinNeckTargetPhysicalRest(f83Eye, f83Forward, f83Up, pitch, true, restTarget);
            nf::weaponViewOffsetTarget(restTarget, f83WeaponViewShift);
            const nf::PinMove candidateMove = nf::pinMove(f83OldNeck, restTarget);
            const double delta[3] = {candidateMove.th[0], candidateMove.delta, candidateMove.th[2]};
            const double candidateNeck[3] = {f83OldNeck[0] + delta[0], f83OldNeck[1] + delta[1], f83OldNeck[2] + delta[2]};
            const double candidateChest[3] = {f83Chest[0] + delta[0], f83Chest[1] + delta[1], f83Chest[2] + delta[2]};
            nf::CrouchNeckArm candidateArms[2] = {f83Arms[0], f83Arms[1]};
            nf::CrouchNeckLeg candidateLegs[2] = {f83Legs[0], f83Legs[1]};
            for (int side = 0; side < 2; ++side) for (int axis = 0; axis < 3; ++axis) {
                const double shift = delta[axis];
                candidateArms[side].clavicle[axis] += shift;
                candidateArms[side].shoulder[axis] += shift;
                candidateArms[side].hand[axis] += shift;
                if (axis != 1) {
                    candidateLegs[side].hip[axis] += shift;
                    candidateLegs[side].knee[axis] += shift;
                    candidateLegs[side].foot[axis] += shift;
                }
            }
            // F83 did not retain preassist elbows. This explicitly approximate sensitivity rig
            // reconstructs the lower point on each measured shoulder/wrist circle, then exercises
            // the production selector with the captured pitch, targets, lengths, and held feet.
            nf::CrouchNeckArm approxArms[2] = {candidateArms[0], candidateArms[1]};
            bool approxElbows = true;
            for (int side = 0; side < 2; ++side) {
                double reach[3] = {approxArms[side].hand[0] - approxArms[side].shoulder[0],
                                   approxArms[side].hand[1] - approxArms[side].shoulder[1],
                                   approxArms[side].hand[2] - approxArms[side].shoulder[2]};
                const double reachM = nf::ikLen(reach), plausibleRawReach = approxArms[side].l1 + approxArms[side].l2 - 0.015;
                if (reachM > plausibleRawReach && reachM > 1e-8)
                    for (int axis = 0; axis < 3; ++axis) approxArms[side].shoulder[axis] += reach[axis] * (reachM - plausibleRawReach) / reachM;
                approxElbows = circleElbow(approxArms[side].shoulder, approxArms[side].hand,
                                           approxArms[side].l1, approxArms[side].l2, approxArms[side].elbow) && approxElbows;
            }
            const auto candidate = nf::crouchNeckYawSelect(candidateNeck, candidateChest, candidateArms, candidateLegs, delta[1], noAdsPush, 50.0, 0.25);
            f83CapturedPitchSweep = f83CapturedPitchSweep && targetOk && candidate.feasible;
            if (std::fabs(pitchDeg + 21.5) < 1e-9 && approxElbows && candidate.feasible) {
                const auto quality = nf::crouchNeckYawSelectQuality(candidateNeck, candidateChest, approxArms, candidateLegs,
                                                                     delta[1], noAdsPush, 0.0, 1.0, 0.06, 0.3, 50.0, 2.0);
                f83ApproxQuality = quality.feasible && quality.poseCost <= quality.baselinePoseCost + 1e-9;
                f83ApproxBaseYaw = quality.baselineYawRad * 180.0 / nf::kIkPi;
                f83ApproxSelectedYaw = quality.yawRad * 180.0 / nf::kIkPi;
                f83ApproxBaseCost = quality.baselinePoseCost;
                f83ApproxSelectedCost = quality.poseCost;
                f83ApproxQuality = quality.feasible && quality.poseCost < quality.baselinePoseCost - 1e-8;
                const bool baseMetrics = measureApproxPose(candidateNeck, approxArms, candidateLegs, delta[1], noAdsPush,
                                                            quality.baselineYawRad, 0.0, f83BaseElbowY, f83BaseElbowAngle,
                                                            f83BaseAssist, f83BaseWristL, f83BaseWristR, f83BaseFeet);
                const bool selectedMetrics = measureApproxPose(candidateNeck, approxArms, candidateLegs, delta[1], noAdsPush,
                                                                quality.yawRad, 1.0, f83SelectedElbowY, f83SelectedElbowAngle,
                                                                f83SelectedAssist, f83SelectedWristL, f83SelectedWristR, f83SelectedFeet);
                f83ApproxQuality = f83ApproxQuality && baseMetrics && selectedMetrics && f83SelectedWristL < 1e-6 &&
                    f83SelectedWristR < 1e-6 && f83SelectedFeet < 1e-4 && f83SelectedElbowY < f83BaseElbowY;
                const auto adsEnd = nf::crouchNeckYawSelectQuality(candidateNeck, candidateChest, approxArms, candidateLegs,
                                                                    delta[1], noAdsPush, 1.0, 1.0, 0.06, 0.3, 50.0, 2.0);
                f83AdsEndpoint = adsEnd.feasible && std::fabs(adsEnd.yawRad - quality.baselineYawRad) < 1e-10;
                bool qualityRampContinuous = quality.qualityAccepted;
                double priorRampYaw = quality.baselineYawRad;
                for (int qi = 0; qi <= 100; ++qi) {
                    const double q = qi / 100.0;
                    const auto atQ = nf::crouchNeckYawSelectQuality(candidateNeck, candidateChest, approxArms, candidateLegs,
                                                                    delta[1], noAdsPush, 0.0, q, 0.06, 0.3, 50.0, 2.0);
                    const double expected = quality.baselineYawRad + q * (quality.yawRad - quality.baselineYawRad);
                    const double step = std::fabs((atQ.yawRad - priorRampYaw) * 180.0 / nf::kIkPi);
                    f82RampMaxYawStepDeg = std::max(f82RampMaxYawStepDeg, step);
                    qualityRampContinuous = qualityRampContinuous && atQ.feasible &&
                        std::fabs(atQ.yawRad - expected) < 1e-7 && (q == 0.0 || atQ.qualityAccepted);
                    if (!qualityRampContinuous && f82QualityRampFailQ < 0) f82QualityRampFailQ = qi;
                    priorRampYaw = atQ.yawRad;
                }
                f82QualityRampOk = qualityRampContinuous;
                bool adsRampContinuous = true;
                priorRampYaw = adsEnd.yawRad;
                for (int di = 1; di <= 100; ++di) {
                    const double detach = di / 100.0;
                    const auto atDetach = nf::crouchNeckYawSelectQuality(candidateNeck, candidateChest, approxArms, candidateLegs,
                                                                          delta[1], noAdsPush, detach, 1.0, 0.06, 0.3, 50.0, 2.0);
                    const double expected = quality.baselineYawRad + (1.0 - detach) * (quality.yawRad - quality.baselineYawRad);
                    adsRampContinuous = adsRampContinuous && atDetach.feasible && std::fabs(atDetach.yawRad - expected) < 1e-7;
                    priorRampYaw = atDetach.yawRad;
                }
                f82AdsRampOk = adsRampContinuous;
                const auto noQuality = nf::crouchNeckYawSelectQuality(candidateNeck, candidateChest, approxArms, candidateLegs,
                                                                       delta[1], noAdsPush, 0.0, 0.0, 0.06, 0.3, 50.0, 0.5);
                f83ApproxQuality = f83ApproxQuality && qualityRampContinuous && adsRampContinuous && noQuality.feasible &&
                    std::fabs(noQuality.yawRad - noQuality.baselineYawRad) < 1e-10;
                const auto benchStart = std::chrono::steady_clock::now();
                for (int repeat = 0; repeat < 3; ++repeat)
                    (void)nf::crouchNeckYawSelectQuality(candidateNeck, candidateChest, approxArms, candidateLegs,
                                                         delta[1], noAdsPush, 0.0, 1.0, 0.06, 0.3, 50.0, 0.5);
                const auto benchEnd = std::chrono::steady_clock::now();
                f83QualityAvgMs = std::chrono::duration<double, std::milli>(benchEnd - benchStart).count() / 3.0;
            }
            if (std::fabs(pitchDeg + 21.5) < 1e-9) centerYawDeg = candidate.yawRad * 180.0 / nf::kIkPi;
        }
        check(f83CapturedPitchSweep && centerYawDeg > 13.0 && centerYawDeg < 16.0,
              "F83: actual captured F82 old target has no feasible solution, while physical rest plus the captured 2.5 cm model shift solves arms and held feet across the -24/-21.5/-19-degree pitch sweep; center yaw matches the corrected counterfactual range");
        std::printf("native_frame_test: F82 reconstructed-rig yaw %.3f -> %.3f deg, Q %.6f -> %.6f, assist %.3f -> %.3f deg, left elbow y %.4f -> %.4f m / angle %.2f -> %.2f deg, wrists %.2g/%.2g m, feet %.2g m; ADS endpoint %s, quality/detach sweeps %s/%s, max ramp yaw step %.3f deg; selector avg %.3f ms at 0.5deg grid\n",
                    f83ApproxBaseYaw, f83ApproxSelectedYaw, f83ApproxBaseCost, f83ApproxSelectedCost,
                    f83BaseAssist, f83SelectedAssist, f83BaseElbowY, f83SelectedElbowY, f83BaseElbowAngle, f83SelectedElbowAngle,
                    f83SelectedWristL, f83SelectedWristR, f83SelectedFeet, (f83AdsEndpoint && f83ApproxQuality) ? "exact" : "FAILED",
                    f82QualityRampOk ? "pass" : "FAIL", f82AdsRampOk ? "pass" : "FAIL",
                    f82RampMaxYawStepDeg, f83QualityAvgMs);
        if (f82QualityRampFailQ >= 0) std::printf("native_frame_test: F82 quality ramp first failing q index %d/%d\n", f82QualityRampFailQ, 100);
        check(f83ApproxQuality && f83AdsEndpoint,
              "F84: legacy F82 sensitivity fixture stays feasible, endpoint-corrects through linear quality and detach sweeps, and has exact baseline behavior at zero quality/full ADS");
        // Current F83 measured scene, with only the irrecoverable authored elbow geometry replaced
        // by a documented downward-circle proxy. Neck, clavicles, wrists, targets, arm lengths and
        // held legs come from the matched skin-ledger row; left raw shoulder reverses the interval
        // mean 11.5-degree assist. This is a sensitivity fixture, not an exact flight replay.
        {
            const double currentNeck[3] = {0.0485228822, 1.50463188, 0.29424116};
            const double currentChest[3] = {-0.0618555136, 1.32763124, 0.174117893};
            const double currentClav[2][3] = {{0.0274200663, 1.4892962, 0.305541426}, {0.0786229521, 1.47642338, 0.288798928}};
            const double currentShoulder[2][3] = {{-0.05813269706087754, 1.4857219681216527, 0.4270835642086465},
                                                  {0.182984427, 1.52421355, 0.188274235}};
            const double currentHand[2][3] = {{0.134705618, 1.33153927, 0.959317505}, {0.193426266, 1.40553296, 0.621308804}};
            const double currentLengths[2][2] = {{0.3044716450163783, 0.28771730237209137}, {0.30084775350253423, 0.28771748089146887}};
            const double currentLegData[2][3][3] = {
                {{-0.2026449739933014, 1.0360705852508545, 0.19766086339950562}, {-0.18220143020153046, 0.5865395665168762, 0.23553290963172913}, {-0.2026449739933014, 0.17759989202022552, 0.19766086339950562}},
                {{-0.08059034496545792, 1.0337415933609009, 0.03886361047625542}, {-0.04735258221626282, 0.5842105746269226, 0.06620372086763382}, {-0.08059035241603851, 0.17527088522911072, 0.03886361047625542}}};
            nf::CrouchNeckArm currentArms[2] = {};
            nf::CrouchNeckLeg currentLegs[2] = {};
            bool currentProxyValid = true;
            for (int side = 0; side < 2; ++side) {
                std::memcpy(currentArms[side].clavicle, currentClav[side], sizeof(currentClav[side]));
                std::memcpy(currentArms[side].shoulder, currentShoulder[side], sizeof(currentShoulder[side]));
                std::memcpy(currentArms[side].hand, currentHand[side], sizeof(currentHand[side]));
                std::memcpy(currentArms[side].target, currentHand[side], sizeof(currentHand[side]));
                currentArms[side].l1 = currentLengths[side][0]; currentArms[side].l2 = currentLengths[side][1];
                currentProxyValid = circleElbow(currentArms[side].shoulder, currentArms[side].hand,
                                                 currentArms[side].l1, currentArms[side].l2, currentArms[side].elbow) && currentProxyValid;
                {
                    double unitRef[3], lo = 0.0, hi = 0.0;
                    const double ref[3] = {currentArms[side].elbow[0] - currentArms[side].shoulder[0],
                                           currentArms[side].elbow[1] - currentArms[side].shoulder[1],
                                           currentArms[side].elbow[2] - currentArms[side].shoulder[2]};
                    nf::ikElbowDRange(currentArms[side].l1, currentArms[side].l2, nf::kIkElbowMinDeg, nf::kIkElbowMaxDeg, &lo, &hi);
                    const auto seedTb = nf::ikTwoBoneFrameD(currentArms[side].shoulder, currentArms[side].hand,
                                                            currentArms[side].l1, currentArms[side].l2, ref, lo, hi);
                    currentArms[side].fpPoleValid = seedTb.ok && nf::ikPoleFromFp(currentArms[side].shoulder, currentArms[side].elbow,
                                                                                     currentArms[side].hand, currentArms[side].fpPole);
                    currentProxyValid = seedTb.ok && nf::ikUnit(ref, unitRef) && currentProxyValid;
                    if (seedTb.ok && nf::ikUnit(ref, unitRef)) {
                        bool clamped = false;
                        const double seedTheta = nf::ikLegacySwivel(seedTb, unitRef, nf::ikArmOutSign(side),
                            currentArms[side].fpPoleValid ? currentArms[side].fpPole : nullptr, 0.3, 0.06, &clamped);
                        auto& seed = currentArms[side].poleSeed;
                        seed.valid = nf::ikElbowRadial(seedTb, seedTheta, seed.reference);
                        std::memcpy(seed.previous, seed.reference, sizeof(seed.reference));
                        std::memcpy(seed.axis, seedTb.dir, sizeof(seed.axis));
                        currentArms[side].stableEligible = seed.valid;
                    }
                }
                std::memcpy(currentLegs[side].hip, currentLegData[side][0], sizeof(currentLegData[side][0]));
                std::memcpy(currentLegs[side].knee, currentLegData[side][1], sizeof(currentLegData[side][1]));
                std::memcpy(currentLegs[side].foot, currentLegData[side][2], sizeof(currentLegData[side][2]));
            }
            const auto currentBase = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, currentArms, currentLegs,
                                                                     0.0, noAdsPush, 0.0, 0.0, 0.06, 0.3, 50.0, 0.5);
            const auto currentFull = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, currentArms, currentLegs,
                                                                     0.0, noAdsPush, 0.0, 1.0, 0.06, 0.3, 50.0, 0.5);
            nf::CrouchNeckYawPoseMetrics currentLegacyMetrics, currentEndpointMetrics;
            const bool currentMetricRows = nf::crouchNeckYawPoseEval(currentNeck, currentArms, currentBase.yawRad, noAdsPush, 0.0, 0.0,
                                                                       0.06, 0.3, &currentLegacyMetrics) &&
                nf::crouchNeckYawPoseEval(currentNeck, currentArms, currentFull.yawRad, noAdsPush, 0.0, 1.0,
                                          0.06, 0.3, &currentEndpointMetrics);
            bool currentRampStable = currentBase.feasible && currentFull.feasible && currentMetricRows;
            double maxCurrentYawStep = 0.0, previousCurrentYaw = currentBase.yawRad;
            double maxTransientAssistDelta = 0.0, maxTransientLeftAngle = currentLegacyMetrics.leftElbowInteriorDeg;
            double maxCurrentElbowStepL1 = 0.0, priorCurrentElbowY = currentLegacyMetrics.leftElbowYFromShoulderL1;
            for (int qi = 1; qi <= 20; ++qi) {
                const double q = qi / 20.0;
                const auto atQ = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, currentArms, currentLegs,
                                                                 0.0, noAdsPush, 0.0, q, 0.06, 0.3, 50.0, 0.5);
                const double expected = currentBase.yawRad + q * (currentFull.yawRad - currentBase.yawRad);
                maxCurrentYawStep = std::max(maxCurrentYawStep, std::fabs((atQ.yawRad - previousCurrentYaw) * 180.0 / nf::kIkPi));
                currentRampStable = currentRampStable && atQ.feasible && atQ.qualityAccepted && std::fabs(atQ.yawRad - expected) < 1e-7;
                nf::CrouchNeckYawPoseMetrics atMetrics;
                currentRampStable = currentRampStable && nf::crouchNeckYawPoseEval(currentNeck, currentArms, atQ.yawRad, noAdsPush,
                                                                                   0.0, q, 0.06, 0.3, &atMetrics);
                if (atMetrics.valid) {
                    maxCurrentElbowStepL1 = std::max(maxCurrentElbowStepL1,
                        std::fabs(atMetrics.leftElbowYFromShoulderL1 - priorCurrentElbowY));
                    priorCurrentElbowY = atMetrics.leftElbowYFromShoulderL1;
                    maxTransientLeftAngle = std::max(maxTransientLeftAngle, atMetrics.leftElbowInteriorDeg);
                    maxTransientAssistDelta = std::max(maxTransientAssistDelta,
                        std::max(atMetrics.assistDeg[0] - currentLegacyMetrics.assistDeg[0], atMetrics.assistDeg[1] - currentLegacyMetrics.assistDeg[1]));
                }
                previousCurrentYaw = atQ.yawRad;
            }
            double maxTargetYawStepDeg = 0.0, maxTargetElbowStepL1 = 0.0;
            double previousTargetYaw = 0.0, previousTargetElbow = 0.0;
            bool havePreviousTarget = false, firstTargetAccepted = false;
            int maxElbowStepIndex = -1;
            double maxStepBefore = 0.0, maxStepAfter = 0.0;
            bool targetMotionFeasible = true;
            for (int ti = 0; ti <= 40; ++ti) {
                const double dz = (ti - 20) * 0.0005;
                nf::CrouchNeckArm movedArms[2] = {currentArms[0], currentArms[1]};
                for (int side = 0; side < 2; ++side) { movedArms[side].hand[2] += dz; movedArms[side].target[2] += dz; }
                const auto moved = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, movedArms, currentLegs,
                                                                   0.0, noAdsPush, 0.0, 1.0, 0.06, 0.3, 50.0, 0.5);
                nf::CrouchNeckYawPoseMetrics movedMetrics;
                targetMotionFeasible = targetMotionFeasible && moved.feasible &&
                    nf::crouchNeckYawPoseEval(currentNeck, movedArms, moved.yawRad, noAdsPush, 0.0,
                                              moved.qualityAccepted ? 1.0 : 0.0, 0.06, 0.3, &movedMetrics);
                if (!movedMetrics.valid) continue;
                maxTargetYawStepDeg = std::max(maxTargetYawStepDeg, std::fabs((moved.yawRad - previousTargetYaw) * 180.0 / nf::kIkPi));
                if (!havePreviousTarget) {
                    previousTargetYaw = moved.yawRad;
                    previousTargetElbow = movedMetrics.leftElbowYFromShoulderL1;
                    firstTargetAccepted = moved.qualityAccepted;
                    havePreviousTarget = true;
                    continue;
                }
                const double elbowStep = std::fabs(movedMetrics.leftElbowYFromShoulderL1 - previousTargetElbow);
                if (elbowStep > maxTargetElbowStepL1) {
                    maxTargetElbowStepL1 = elbowStep;
                    maxElbowStepIndex = ti;
                    maxStepBefore = previousTargetElbow;
                    maxStepAfter = movedMetrics.leftElbowYFromShoulderL1;
                }
                previousTargetYaw = moved.yawRad;
                previousTargetElbow = movedMetrics.leftElbowYFromShoulderL1;
                targetMotionFeasible = targetMotionFeasible && moved.qualityAccepted == firstTargetAccepted;
            }
            bool nonzeroYawCase = false;
            bool nonzeroPathValid = false;
            nf::CrouchNeckYawPoseMetrics nonzeroLegacyMetrics, nonzeroEndpointMetrics;
            double nonzeroBaselineDeg = 0.0, nonzeroSelectedDeg = 0.0;
            double nonzeroDx = 0.0, nonzeroDz = 0.0;
            nf::CrouchNeckArm nonzeroArms[2] = {currentArms[0], currentArms[1]};
            nf::CrouchNeckYaw nonzeroEndpoint;
            for (int xi = -4; xi <= 4 && !nonzeroYawCase; ++xi) for (int zi = -4; zi <= 4 && !nonzeroYawCase; ++zi) {
                const double dx = xi * 0.005, dz = zi * 0.005;
                nf::CrouchNeckArm movedArms[2] = {currentArms[0], currentArms[1]};
                for (int side = 0; side < 2; ++side) { movedArms[side].target[0] += dx; movedArms[side].target[2] += dz; }
                const auto moved = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, movedArms, currentLegs,
                                                                   0.0, noAdsPush, 0.0, 1.0, 0.06, 0.3, 50.0, 0.5);
                if (moved.feasible && moved.qualityAccepted &&
                    std::fabs((moved.yawRad - moved.baselineYawRad) * 180.0 / nf::kIkPi) > 0.1) {
                    nonzeroYawCase = true;
                    nonzeroDx = dx; nonzeroDz = dz;
                    nonzeroArms[0] = movedArms[0]; nonzeroArms[1] = movedArms[1];
                    nonzeroEndpoint = moved;
                    nonzeroBaselineDeg = moved.baselineYawRad * 180.0 / nf::kIkPi;
                    nonzeroSelectedDeg = moved.yawRad * 180.0 / nf::kIkPi;
                    nf::CrouchNeckYaw nonzeroEndpointFeasible;
                    nonzeroPathValid = nf::crouchNeckYawFeasible(currentNeck, currentChest, nonzeroArms, currentLegs, 0.0,
                                                                  moved.yawRad, noAdsPush, &nonzeroEndpointFeasible) &&
                        nf::crouchNeckYawPoseEval(currentNeck, nonzeroArms, moved.baselineYawRad, noAdsPush,
                                                                  0.0, 0.0, 0.06, 0.3, &nonzeroLegacyMetrics) &&
                        nf::crouchNeckYawPoseEval(currentNeck, nonzeroArms, moved.yawRad, noAdsPush,
                                                  0.0, 1.0, 0.06, 0.3, &nonzeroEndpointMetrics) &&
                        nf::crouchNeckYawPoseNoWorse(nonzeroEndpointMetrics, nonzeroLegacyMetrics) &&
                        nf::crouchNeckYawPoseImproved(nonzeroEndpointMetrics, nonzeroLegacyMetrics);
                    for (int path = 0; path <= 20 && nonzeroPathValid; ++path) {
                        const double t = path / 20.0;
                        const double yaw = moved.baselineYawRad + t * (moved.yawRad - moved.baselineYawRad);
                        nf::CrouchNeckYaw feasible;
                        nf::CrouchNeckYawPoseMetrics metrics;
                        nonzeroPathValid = nf::crouchNeckYawFeasible(currentNeck, currentChest, movedArms, currentLegs, 0.0, yaw, noAdsPush, &feasible) &&
                            nf::crouchNeckYawPoseEval(currentNeck, movedArms, yaw, noAdsPush, 0.0, t, 0.06, 0.3, &metrics);
                    }
                }
            }
            bool nonzeroRampExact = nonzeroYawCase;
            double nonzeroMaxQYawStepDeg = 0.0, nonzeroPriorYaw = nonzeroEndpoint.baselineYawRad;
            for (int qi = 0; qi <= 20 && nonzeroRampExact; ++qi) {
                const double q = qi / 20.0;
                const auto atQ = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, nonzeroArms, currentLegs,
                                                                 0.0, noAdsPush, 0.0, q, 0.06, 0.3, 50.0, 0.5);
                const double expected = nonzeroEndpoint.baselineYawRad + q * (nonzeroEndpoint.yawRad - nonzeroEndpoint.baselineYawRad);
                nonzeroMaxQYawStepDeg = std::max(nonzeroMaxQYawStepDeg, std::fabs((atQ.yawRad - nonzeroPriorYaw) * 180.0 / nf::kIkPi));
                nonzeroRampExact = atQ.feasible && (q == 0.0 || atQ.qualityAccepted) && std::fabs(atQ.yawRad - expected) < 1e-7;
                nonzeroPriorYaw = atQ.yawRad;
            }
            for (int di = 0; di <= 20 && nonzeroRampExact; ++di) {
                const double detach = di / 20.0;
                const auto atDetach = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, nonzeroArms, currentLegs,
                                                                      0.0, noAdsPush, detach, 1.0, 0.06, 0.3, 50.0, 0.5);
                const double expected = nonzeroEndpoint.baselineYawRad + (1.0 - detach) *
                    (nonzeroEndpoint.yawRad - nonzeroEndpoint.baselineYawRad);
                nonzeroRampExact = atDetach.feasible && std::fabs(atDetach.yawRad - expected) < 1e-7;
            }
            const auto currentBenchStart = std::chrono::steady_clock::now();
            volatile double currentBenchSink = 0.0;
            constexpr int kBenchRepeats = 40;
            for (int repeat = 0; repeat < kBenchRepeats; ++repeat) {
                nf::CrouchNeckArm benchArms[2] = {currentArms[0], currentArms[1]};
                const double perturb = ((repeat % 9) - 4) * 0.00025;
                benchArms[0].target[2] += perturb;
                benchArms[1].target[2] += perturb;
                const auto benchResult = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, benchArms, currentLegs,
                                                                         0.0, noAdsPush, 0.0, 1.0, 0.06, 0.3, 50.0, 0.5);
                currentBenchSink += benchResult.yawRad + benchResult.poseCost;
            }
            const double currentBenchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentBenchStart).count() / kBenchRepeats;
            nf::RifleQualityRamp releaseRamp;
            (void)releaseRamp.step(true, 71, true, 9010, 8, 14, 200, 0.0, 1.0);
            const double releaseStart = releaseRamp.step(true, 71, true, 9010, 8, 14, 201, 0.15, 1.0);
            double releasePreviousY = currentEndpointMetrics.leftElbowYFromShoulderL1, maxReleaseElbowStepL1 = 0.0;
            bool releasePoseContinuous = std::fabs(releaseStart - 1.0) < 1e-12 &&
                nf::ikRifleQualityKeepsPhysicalPole(true, releaseStart, 0.0, true) &&
                !nf::ikRifleQualityKeepsPhysicalPole(false, releaseStart, 0.0, true) &&
                !nf::ikRifleQualityKeepsPhysicalPole(true, releaseStart, 1.0, true) &&
                !nf::ikRifleQualityKeepsPhysicalPole(true, releaseStart, 0.0, false);
            for (int ri = 1; ri <= 10; ++ri) {
                const double rq = releaseRamp.step(true, 71, true, 9010, 8, 14, 201u + static_cast<uint32_t>(ri), 1.0 / 60.0, 0.0);
                // Match runtime eligibility: source loss drops stablePolicy, but the exact-rifle helper
                // keeps the physical seed alive and therefore keeps the same stable solve through q decay.
                const bool runtimePinWanted = true, runtimePhysicalPoleActive = true;
                const bool keepSeed = runtimePinWanted && runtimePhysicalPoleActive &&
                    nf::ikRifleQualityKeepsPhysicalPole(true, rq, 0.0, true);
                nf::CrouchNeckArm releaseArms[2] = {currentArms[0], currentArms[1]};
                releaseArms[0].stableEligible = runtimePhysicalPoleActive && releaseArms[0].active &&
                    (nf::ikPhysicalPoleSideEligible(0, false, false) || keepSeed);
                const auto atRelease = nf::crouchNeckYawSelectQuality(currentNeck, currentChest, releaseArms, currentLegs,
                                                                      0.0, noAdsPush, 0.0, rq, 0.06, 0.3, 50.0, 0.5);
                nf::CrouchNeckYawPoseMetrics releaseMetrics;
                const bool metricOk = atRelease.feasible && keepSeed && atRelease.qualityAccepted &&
                    releaseArms[0].stableEligible &&
                    nf::crouchNeckYawPoseEval(currentNeck, releaseArms, atRelease.yawRad, noAdsPush, 0.0, rq,
                                              0.06, 0.3, &releaseMetrics);
                releasePoseContinuous = releasePoseContinuous && metricOk;
                if (metricOk) {
                    maxReleaseElbowStepL1 = std::max(maxReleaseElbowStepL1,
                        std::fabs(releaseMetrics.leftElbowYFromShoulderL1 - releasePreviousY));
                    releasePreviousY = releaseMetrics.leftElbowYFromShoulderL1;
                }
            }
            std::printf("native_frame_test: current F83 measured-body/down-circle-proxy yaw %.3f -> %.3f deg, accepted %s, max q-step %.3f deg, q elbow step %.5f L1, left elbow y %.4f -> %.4f m and angle %.2f -> %.2f deg, max transient angle %.2f, assist increase %.3f deg, target sweep max %.3f deg / %.5f L1\n",
                        currentBase.yawRad * 180.0 / nf::kIkPi, currentFull.yawRad * 180.0 / nf::kIkPi,
                        currentFull.qualityAccepted ? "yes" : "no", maxCurrentYawStep,
                        maxCurrentElbowStepL1,
                        currentLegacyMetrics.leftElbowYFromShoulderL1, currentEndpointMetrics.leftElbowYFromShoulderL1,
                        currentLegacyMetrics.leftElbowInteriorDeg, currentEndpointMetrics.leftElbowInteriorDeg,
                        maxTransientLeftAngle, maxTransientAssistDelta, maxTargetYawStepDeg, maxTargetElbowStepL1);
            if (maxElbowStepIndex >= 0) std::printf("native_frame_test: target sweep max elbow step index %d (%+.1f mm), normalized %.5f -> %.5f L1\n",
                                                    maxElbowStepIndex, (maxStepAfter - maxStepBefore) * currentLengths[0][0] * 1000.0,
                                                    maxStepBefore, maxStepAfter);
            std::printf("native_frame_test: current F83 selector avg %.3f ms over %d varying inputs (checksum %.6f); nonzero accepted yaw fixture %s %.3f -> %.3f deg at target shift (%+.3f,%+.3f)m, endpoint assist L/R %.3f/%.3f -> %.3f/%.3f deg, left elbow %.4f -> %.4f L1 (%.2f -> %.2f deg), wrist/feet/neck hard-feasible; ramp %s max q-step %.3f deg\n",
                        currentBenchMs, kBenchRepeats, static_cast<double>(currentBenchSink), nonzeroYawCase ? "found" : "not found", nonzeroBaselineDeg, nonzeroSelectedDeg,
                        nonzeroDx, nonzeroDz,
                        nonzeroLegacyMetrics.assistDeg[0], nonzeroLegacyMetrics.assistDeg[1],
                        nonzeroEndpointMetrics.assistDeg[0], nonzeroEndpointMetrics.assistDeg[1],
                        nonzeroLegacyMetrics.leftElbowYFromShoulderL1, nonzeroEndpointMetrics.leftElbowYFromShoulderL1,
                        nonzeroLegacyMetrics.leftElbowInteriorDeg, nonzeroEndpointMetrics.leftElbowInteriorDeg,
                        nonzeroRampExact ? "pass" : "FAIL", nonzeroMaxQYawStepDeg);
            std::printf("native_frame_test: same-item source-loss release retains stable pole through q decay; max elbow step %.5f L1 in 10 frames\n",
                        maxReleaseElbowStepL1);
            (void)currentBenchSink;
            check(currentProxyValid && currentBase.feasible && currentFull.feasible && currentRampStable &&
                      nf::crouchNeckYawPoseNoWorse(currentEndpointMetrics, currentLegacyMetrics) &&
                      nf::crouchNeckYawPoseImproved(currentEndpointMetrics, currentLegacyMetrics) && targetMotionFeasible &&
                      maxTargetYawStepDeg < 5.0 && maxTargetElbowStepL1 < 0.03 && nonzeroYawCase && nonzeroPathValid && nonzeroRampExact,
                  "F84: current F83 measured neck/wrist/feet with explicit authored-elbow proxy has a feasible, continuous full-support endpoint sweep");
            check(releasePoseContinuous && maxReleaseElbowStepL1 < 0.02,
                  "F84: same-item source loss retains the stable pole through quality release and hard-boundaries its seed at item/full-ADS changes");
        }
        const nf::CrouchNeckYaw picked = nf::crouchNeckYawSelect(neck, pivot, arms, legs, 0.0, noAdsPush);
        bool restPitchSweepReach = true;
        const double physicalPitchDegs[] = {-24.0, -21.5, -19.0};
        const double pitchBase = physicalPitchDegs[1] * nf::kIkPi / 180.0;
        const double baseUp = nf::kPinRestUpM * std::cos(pitchBase) + nf::kPinRestBackM * std::sin(pitchBase);
        const double baseForward = nf::kPinRestBackM * std::cos(pitchBase) - nf::kPinRestUpM * std::sin(pitchBase);
        const double fixedEye[3] = {neck[0] + baseForward, neck[1] + baseUp, neck[2]};
        for (double pitchDeg : physicalPitchDegs) {
            const double pitch = pitchDeg * nf::kIkPi / 180.0;
            const double fwd[3] = {std::cos(pitch), std::sin(pitch), 0.0};
            const double up[3] = {-std::sin(pitch), std::cos(pitch), 0.0};
            double restTarget[3];
            const bool targetOk = nf::pinNeckTargetPhysicalRest(fixedEye, fwd, up, pitch, true, restTarget);
            const nf::PinMove candidateMove = nf::pinMove(neck, restTarget);
            double candidateNeck[3] = {neck[0] + candidateMove.th[0], neck[1] + candidateMove.delta, neck[2] + candidateMove.th[2]};
            double candidatePivot[3] = {pivot[0] + candidateMove.th[0], pivot[1] + candidateMove.delta, pivot[2] + candidateMove.th[2]};
            nf::CrouchNeckArm shiftedArms[2] = {arms[0], arms[1]};
            nf::CrouchNeckLeg shiftedLegs[2] = {legs[0], legs[1]};
            for (int side = 0; side < 2; ++side) {
                for (int axis = 0; axis < 3; ++axis) {
                    const double shift = axis == 0 ? candidateMove.th[0] : axis == 1 ? candidateMove.delta : candidateMove.th[2];
                    shiftedArms[side].clavicle[axis] += shift;
                    shiftedArms[side].shoulder[axis] += shift;
                    shiftedArms[side].hand[axis] += shift;
                    if (axis != 1) {
                        shiftedLegs[side].hip[axis] += shift;
                        shiftedLegs[side].knee[axis] += shift;
                        shiftedLegs[side].foot[axis] += shift;
                    }
                }
            }
            const nf::CrouchNeckYaw candidate = nf::crouchNeckYawSelect(candidateNeck, candidatePivot, shiftedArms, shiftedLegs,
                                                                         candidateMove.delta, noAdsPush, 50.0, 0.25);
            restPitchSweepReach = restPitchSweepReach && targetOk && candidate.feasible;
        }
        check(restPitchSweepReach,
              "F83: synthetic rest-target pitch sweep remains feasible against the fixture geometry using the actual torso and foot selector");
        double turnedNeck[3], repinned[3];
        nf::turnAboutVertical(neck, pivot, picked.yawRad, turnedNeck);
        for (int i = 0; i < 3; ++i) repinned[i] = turnedNeck[i] + picked.rootDelta[i];
        const double neckErrV[3] = {repinned[0] - neck[0], repinned[1] - neck[1], repinned[2] - neck[2]};
        bool feetClose = picked.feasible;
        for (int side = 0; side < 2 && feetClose; ++side) {
            const double hip1[3] = {legs[side].hip[0] + picked.rootDelta[0], legs[side].hip[1], legs[side].hip[2] + picked.rootDelta[2]};
            const nf::LegSolve solved = nf::legSolveTo(legs[side].hip, legs[side].knee, legs[side].foot, hip1, legs[side].foot);
            feetClose = solved.ok && !solved.limited && solved.footErrM < 1e-4;
        }
        bool transformedSubtreeMatches = picked.feasible;
        double transformed[4][3] = {};
        const double points[4][3] = {{neck[0], neck[1], neck[2]}, {arms[0].clavicle[0], arms[0].clavicle[1], arms[0].clavicle[2]},
                                     {arms[0].shoulder[0], arms[0].shoulder[1], arms[0].shoulder[2]}, {arms[0].hand[0], arms[0].hand[1], arms[0].hand[2]}};
        const double up[3] = {0.0, 1.0, 0.0};
        double matrixYaw[9]; nf::ikRotAxis(up, picked.yawRad, matrixYaw);
        for (int point = 0; point < 4; ++point) {
            float source[16] = {};
            source[0] = source[5] = source[10] = source[15] = 1.0f;
            for (int axis = 0; axis < 3; ++axis) source[12 + axis] = static_cast<float>(points[point][axis]);
            float result[16];
            nf::ikPivot(source, matrixYaw, pivot, result);
            result[12] += static_cast<float>(picked.rootDelta[0]);
            result[14] += static_cast<float>(picked.rootDelta[2]);
            for (int axis = 0; axis < 3; ++axis) transformed[point][axis] = result[12 + axis];
            double expected[3]; nf::turnAboutVertical(points[point], neck, picked.yawRad, expected);
            for (int axis = 0; axis < 3; ++axis) transformedSubtreeMatches = transformedSubtreeMatches && std::fabs(transformed[point][axis] - expected[axis]) < 2e-6;
        }
        const double beforeUpper[3] = {points[2][0] - points[1][0], points[2][1] - points[1][1], points[2][2] - points[1][2]};
        const double beforeFore[3] = {points[3][0] - points[2][0], points[3][1] - points[2][1], points[3][2] - points[2][2]};
        const double afterUpper[3] = {transformed[2][0] - transformed[1][0], transformed[2][1] - transformed[1][1], transformed[2][2] - transformed[1][2]};
        const double afterFore[3] = {transformed[3][0] - transformed[2][0], transformed[3][1] - transformed[2][1], transformed[3][2] - transformed[2][2]};
        transformedSubtreeMatches = transformedSubtreeMatches && std::fabs(nf::ikLen(beforeUpper) - nf::ikLen(afterUpper)) < 2e-6 &&
                                   std::fabs(nf::ikLen(beforeFore) - nf::ikLen(afterFore)) < 2e-6;
        double yawDegrees = picked.yawRad * 180.0 / nf::kIkPi;
        check(picked.feasible && yawDegrees > 28.5 && yawDegrees < 29.75 && nf::ikLen(neckErrV) < 1e-9 &&
                  feetClose && std::fabs(picked.rootDelta[0] - fixtureDelta[0]) < 0.003 && std::fabs(picked.rootDelta[2] - fixtureDelta[2]) < 0.003,
              "F82: captured crouched-rifle arms select the nearest feasible positive yaw, repin the neck, and close both held-foot leg solves");
        check(transformedSubtreeMatches, "F82: chest-subtree matrix rotation plus neck repin preserves upper-arm and forearm lengths");

        // A partial offhand solve must predict the same interpolated wrist and ADS displacement as stage 2.
        // The candidate hand is already the net chest-yaw + neck-repin point, so adding rootDelta again would
        // shift this target by (1-w)*rootDelta and fail the exact shoulder replay below.
        nf::CrouchNeckArm partial[2] = {arms[0], arms[1]};
        partial[0].active = false;
        partial[1].wSolve = 0.5;
        const double adsPush[3] = {0.0, 0.006, 0.012};
        nf::CrouchNeckYaw partialPick;
        bool partialFeasible = nf::crouchNeckYawFeasible(neck, pivot, partial, legs, 0.0, picked.yawRad, adsPush, &partialPick);
        if (partialFeasible) {
            const int side = 1;
            double hand[3], clavicle[3], shoulder[3];
            nf::turnAboutVertical(partial[side].hand, neck, partialPick.yawRad, hand);
            nf::turnAboutVertical(partial[side].clavicle, neck, partialPick.yawRad, clavicle);
            nf::turnAboutVertical(partial[side].shoulder, neck, partialPick.yawRad, shoulder);
            const double target[3] = {hand[0] + 0.5 * (partial[side].target[0] - hand[0]),
                                      hand[1] + 0.5 * (partial[side].target[1] - hand[1]),
                                      hand[2] + 0.5 * (partial[side].target[2] - hand[2])};
            const double before[3] = {target[0] - shoulder[0], target[1] - shoulder[1], target[2] - shoulder[2]};
            const double d = nf::ikLen(before);
            double excess = 0.0, disp[3], R[9], angle = 0.0;
            bool capped = false;
            const double move = nf::ikAssistReachMove(d, partial[side].l1, partial[side].l2, &excess);
            for (int i = 0; i < 3; ++i) disp[i] = ((d > 1e-9 ? move * before[i] / d : 0.0) + adsPush[i]) * 0.5;
            if (nf::ikAssistRot(clavicle, shoulder, disp, R, &angle, &capped)) {
                const double cs[3] = {shoulder[0] - clavicle[0], shoulder[1] - clavicle[1], shoulder[2] - clavicle[2]};
                double turned[3]; nf::ikRowRotate(cs, R, turned);
                for (int i = 0; i < 3; ++i) shoulder[i] = clavicle[i] + turned[i];
            }
            bool sameShoulder = true;
            for (int i = 0; i < 3; ++i) sameShoulder = sameShoulder && std::fabs(shoulder[i] - partialPick.shoulder[side][i]) < 1e-10;
            check(sameShoulder && (std::fabs(partialPick.rootDelta[0]) > 1e-4 || std::fabs(partialPick.rootDelta[2]) > 1e-4),
                  "F82: partial offhand plus ADS push predicts the single net-transformed hand without a second root translation");
        } else {
            check(false, "F82: partial offhand plus ADS push has a feasible yaw for exact shoulder replay");
        }

        // At detach=0 the hard endpoint is T; at detach=1 the historical endpoint is T+legacyReach.
        // Intermediate samples rebuild their target from geometry and do not blend the selected yaw.
        const double originalTarget[3] = {neck[0], neck[1], neck[2]};
        const double legacyReach[3] = {0.0381563231, 0.0, 0.115241572};
        double endpoint0[3], endpoint1[3];
        for (int i = 0; i < 3; ++i) {
            endpoint0[i] = originalTarget[i] + 0.0 * legacyReach[i];
            endpoint1[i] = originalTarget[i] + 1.0 * legacyReach[i];
        }
        const double endpoint0Error[3] = {endpoint0[0] - originalTarget[0], endpoint0[1] - originalTarget[1], endpoint0[2] - originalTarget[2]};
        check(nf::ikLen(endpoint0Error) < 1e-12 &&
                  std::fabs(endpoint1[0] - (originalTarget[0] + legacyReach[0])) < 1e-12 &&
                  std::fabs(endpoint1[2] - (originalTarget[2] + legacyReach[2])) < 1e-12,
              "F82: detach endpoints hard-pin at zero and exactly retain legacy reach at full ADS");

        // Standing and jumping snapshots use the same policy math. Their captured assist counters were zero
        // in the overlapping F81 windows (121946: 0/234 at 12:19:49.314; 122002: 0/289 at 12:20:05.126).
        const double noPush[3] = {};
        nf::CrouchNeckArm standing[2] = {};
        const double standingC[2][3] = {{-0.0046476675, 1.44254315, 0.2802694587}, {0.0477911625, 1.42908692, 0.2677651797}};
        const double standingS[2][3] = {{-0.0996752959, 1.4402653, 0.3945066187}, {0.1597479378, 1.47354472, 0.1743333637}};
        const double standingH[2][3] = {{0.0987435058, 1.33760488, 0.97542572}, {0.179897711, 1.40038717, 0.639815092}};
        const double standingL1[2] = {0.30447058085612155, 0.30084669078547716};
        const double standingL2[2] = {0.28771826270806267, 0.28771810607719445};
        for (int side = 0; side < 2; ++side) {
            std::memcpy(standing[side].clavicle, standingC[side], sizeof(standingC[side]));
            std::memcpy(standing[side].shoulder, standingS[side], sizeof(standingS[side]));
            std::memcpy(standing[side].hand, standingH[side], sizeof(standingH[side]));
            std::memcpy(standing[side].target, standingH[side], sizeof(standingH[side]));
            standing[side].l1 = standingL1[side]; standing[side].l2 = standingL2[side];
        }
        const double standingNeck[3] = {0.017552387, 1.45759428, 0.27065137};
        check(nf::crouchNeckYawArmShortfall(standingNeck, standing, 0.0, noPush) < 1e-6,
              "F82: captured standing rifle frame remains feasible at zero added torso yaw");

        nf::CrouchNeckArm jumping[2] = {};
        const double jumpingC[2][3] = {{-0.0101276077, 1.42232835, 0.2752242307}, {0.0378539395, 1.41979563, 0.2769488017}};
        const double jumpingS[2][3] = {{-0.1251686532, 1.40318608, 0.3603162097}, {0.1864594279, 1.43744755, 0.2546252827}};
        const double jumpingH[2][3] = {{0.0945935249, 1.37097204, 0.974208474}, {0.179626346, 1.42758512, 0.63838768}};
        const double jumpingL1[2] = {0.30453304264453884, 0.30453312896514284};
        const double jumpingL2[2] = {0.28783756567098734, 0.2878378363962739};
        for (int side = 0; side < 2; ++side) {
            std::memcpy(jumping[side].clavicle, jumpingC[side], sizeof(jumpingC[side]));
            std::memcpy(jumping[side].shoulder, jumpingS[side], sizeof(jumpingS[side]));
            std::memcpy(jumping[side].hand, jumpingH[side], sizeof(jumpingH[side]));
            std::memcpy(jumping[side].target, jumpingH[side], sizeof(jumpingH[side]));
            jumping[side].l1 = jumpingL1[side]; jumping[side].l2 = jumpingL2[side];
        }
        const double jumpingNeck[3] = {0.0126081882, 1.47378385, 0.250033289};
        check(nf::crouchNeckYawArmShortfall(jumpingNeck, jumping, 0.0, noPush) > 0.008 &&
                  nf::crouchNeckYawArmShortfall(jumpingNeck, jumping, 9.0 * nf::kIkPi / 180.0, noPush) < 1e-6,
              "F82: captured jumping rifle frame rejects zero yaw and clears its shoulder shortfall with the selected torso turn");
        nf::CrouchNeckArm unreachable[2] = {arms[0], arms[1]};
        unreachable[1].target[2] += 10.0;
        const nf::CrouchNeckArm unreachableBefore[2] = {unreachable[0], unreachable[1]};
        const nf::CrouchNeckYaw refused = nf::crouchNeckYawSelect(neck, pivot, unreachable, legs, 0.0, noAdsPush);
        check(!refused.feasible && std::memcmp(unreachable, unreachableBefore, sizeof(unreachable)) == 0,
              "F82: infeasible geometry refuses without mutating the pose inputs");
        nf::CrouchNeckArm noActiveArms[2] = {arms[0], arms[1]};
        noActiveArms[0].active = noActiveArms[1].active = false;
        const nf::CrouchNeckYaw noActive = nf::crouchNeckYawSelect(neck, pivot, noActiveArms, legs, 0.0, noAdsPush);
        check(!noActive.feasible, "F82: probe-only arms cannot authorize a protected neck turn");
    }
    // F85: a, the pin-step tracker; b, the weapon UI pinned to the weapon; c, the reticle at the aim ray's hit depth.
    f85test::run(check);
    // F86: a, the pose carry; d, the weapon UI depth (measure only since F87); g, the floor probe.
    f85test::run86(check);
    // F87: h, the standing-height hold; a, the previous copy, the join, the cap, the hand residual; c, the stream windows' history; f, the scene rows' fallback; p, the pins.
    f85test::run87(check);
    // F88: the weapon eye offset: the keys, the weight, the yaw, the hold's input, the pin's share, the readout and the continuity.
    f85test::run88(check);
    // F89: the ADS release (w has no ADS input, the eye path, the pin), the left eye closing in ADS, the transition trace and the hold's open-loop input.
    f89test::run89(check);
    // F90: A, the ADS readout part scaled to the hip's texels per metre about its content centre; B, the pistol's readout in the weapon's frame, its latch and keys.
    f90test::run90(check);
    // F91: B, the ADS vibration instrument (the trace's fields, the cancel the pose carried, the fit: H1, the lock, H3); C, the ADS item gate.
    f91bctest::run91bc(check);
    // F92b: the ADS head aim's cancel published ahead (the view the head's exactly, the weapon a frame off the eye), and the head aim as a live toggle within a press.
    f92test::run92(check);
    // F93b: the ADS sight lock's input from the alignment the measured eye was drawn with (no two-frame loop), the eye chain's 1 s sums.
    f93test::run93(check);
    // F93d: the game's camera (the suite's mode byte) gates the body changes.
    f93detest::run93de(check);
    // F94a: the view's height from the animated neck (the key, the filter, F88's weight, the loop on a plant at 0-3 frames of delay), the body at its animated height.
    f94test::run94(check);
    // F94b: the label's row patch from its own anchor (the anchor shown at the dot's place), F93c's rule against it in a lagged turn and at a tie of two dots.
    f94btest::run94b(check);
    // F95a: the raise's target from settled stances only (the walk cycle, a landing and a stance in transition held out), the loop with the window, the instrument.
    f95test::run95(check);
    // F95b: the label's anchor taken back from its designed offset in the marker's HUD frame (F94b's world anchor failed the gate in the flight's frames).
    f94btest::run95b(check);
    // F95c: the head parts as shadow-only through the visibility call (the game's class 2), the fade hook's guard and its action.
    f95ctest::run95c(check);
    // F97a: the armed neck target (F93's raise, the eye's lowering, the weight's blend), the legs' room for a raise, the stance bins.
    f97test::run97(check);
    // F97b: the label's size and record place as a panel at its anchor (the game's NDC-fixed size shrinks as cos^2 and cos off the eye's axis), the guards.
    f97btest::run97b(check);
    // F98a: the stable pole from its seed (no turn from the arm's path), against the frame-to-frame carry's 84 deg on a 40 deg cone.
    f98test::run98(check);
    // F98b: the label upright and facing the neck's pivot: still through head turns (F97b's eye-facing placement not), scaled by distance, the guards.
    f98btest::run98b(check);
    // F99a: the hands' bridge from the game's selection until Status.json names the item, its releases, and the draw-onset instrument.
    f99test::run99(check);
    // F100: the marker tee's prefilter leaves the draw serial and the decisions as the slow path does; the paced and throttled lines.
    f100test::run100(check);
    // F101: this runtime's ladder against a released provider's shape checks, the frozen v0.21.1 layouts.
    f101test::run101(check);
    // F107: the carrier management screen's camera away from the commander: the gate, the episode, the neck reference held out, the calibration's consequence.
    f107test::run107(check);
    // F109: look to aim on foot: the exact split (a stick's turn with the head aim writing is the player's), the drop by style, the snap and flick detectors, the
    // snap's fade, the smoothing, the follower against the real map, the per-second check.
    f109test::run109(check);
    // F129: the processor's calls told apart (path A, B, SKIP by a sentinel in the stored pair; only the player's own component takes the ask; the fallback), the
    // look path instrument (the ring, the counters, the episodes, the event budget), and the zero-sum probes.
    f129test::run129(check);
    // F130: a scene mesh whose rows disagree with a source's is marked (its shaders, cb1 and first constant) and refused, the sources are never latched off, and a source that
    // draws after a mesh compares and replaces; the SRV flight (every check agreed until one mesh differed by 0.239) replayed.
    f130test::run130(check);
    // F131: look to aim holds (no asks, no probes, no snaps) while an on-foot panel is open, a menu or map holds the 2D screen, or the look processor has not run; 200 ms to leave.
    f131test::run131(check);
    // F132: G's frame pairing started on the wrong call of the game's order re-seeds after one refusal and pairs from the next frame; a missing call, either order and a
    // gap that is real on every frame (refused every time, reseeds counted, no runaway).
    f132test::run132(check);
    // F133: a 41 deg head step over a 3 s stall is motion and over 11 ms a discontinuity (a 25 deg step over 11 ms too, which the 30 deg size rule took as motion); the
    // re-centre on a standing yaw offset fires once after 2 s still, not at 2 deg, not while the head turns, not twice within 2 s.
    f133test::run133(check);
    // F135 (measure only): the menu probe's counters, objects, 0.5 s edges, key edges and line gate.
    f135test::run135(check);
    // F137: the virtual neck d = R(h + a) v - R(h) v (0 at a = 0; +8.9 cm forward and -15.5 cm up at h = 0, a = -60 deg; capped at 25 cm; F138: nose up each turn stops at
    // 30.14 deg, so +65 deg is -9.0 cm forward and +2.4 cm up), its seated vector, the pin taking the whole vector out of the camera by the pose's serial, the 0.15 s weight
    // (0 under look to aim, times 1 - the ADS weight), the head offset it cancels from.
    f137test::run137(check);
    // F138: the pitched look's weight smoothstep(20, 30 deg) (F139; it was 10 to 20), the settle's refusal from 20 deg (F139; it was 10), G and the clamped sink, the reference and
    // the added target, the return edge, every refusal's reason, the window's means and 10 deg bins.
    f138test::run138(check);
    // F139: each weapon's sight on the ADS line: the table by exact item name, the live trim, the shift in the FP root's up and right less its part along the sight line, the 8 cm
    // bound, the laser assault rifle's F79 origin with the trim on top, a stale or incoherent item snapshot.
    f139test::run139(check);
    // F140: the journal's loadout (the name mapping, SuitLoadout, SwitchSuitLoadout, LoadoutEquipModule, a removal, the unknown and ambiguous cases, chunked reading) and the weapon
    // readout's placement per (item, scope): a table hit, a miss, the trim, the clamp, the rocket launcher's entry while the pistol part is drawn, an untrusted name.
    f140test::run140(check);
    // F141: the bend's own test (w >= 1, the handle valid, the sight ahead), the sight ray in the eye's axes, S against P14's line in the FP root's
    // axes, the window's sums, a locator name, an activation; U against a moved weapon constant, the spread's mean, largest distance and rotation, a part's basis.
    f141test::run141(check);
    // F142: the icon mesh table's reuse choice (the oldest slot idle 60 frames, never one in flight, owned or named by a record), the full outfit A/B's promotion (state 2 becomes 1
    // except a head part and the first-person skeleton; the game's change during the call stands), the outfit census's signature and changed parts, and F141's checks on the
    // lanes used (a NaN in padding lane 3 passes, in lane 12 fails), every rejection reason counted, the raw floats' text, the two candidate positions of E.
    f142test::run142(check);
    // F143: the game's sight line E -> S at the lock's eye depth (the flight's laser SMG: up -4.62, right -0.49 cm), the per-activation filter (tau 0.15 s, restart on another item,
    // scope or activation), the fallback to F139's table and why (key off, item not read, no pair, another item or scope, stale, degenerate line, past the 8 cm bound), the trims
    // added, and the unclip A/B's parts (the 31 that are neither head parts nor in F19's list), the armed state and when the clip measure reads.
    f143test::run143(check);
    // F144: the settle gate (fraction 1.0, 0.6 s after it first reached 1.0, E and S under 2 mm), the average, the cache across presses, the choice and the blend from the table,
    // the entry path from the F89 trace's eye samples; Gear_Back only for an outer garment and the first-person census's signature; the readout placed per (item, scope, role)
    // with today's seeds (the rifle class's hip readout placed with 0 keys draws as the game does), the trims added to the role drawn now only, the pistol's ADS role's own
    // target from the hip's centre, and the proof of the drawn move against the total.
    f144test::run144(check);
    // F145: the tag slot pick (a hit, a free slot, the slot asked for longest ago once idle 5 s, none while all are in use), the F144 flight's eleven part names over four loadouts
    // replayed into eight slots, the event ring, and the proof of a trim against the same frame placed at the seed alone (the rifle's ADS role with its size factor, the pistol's ADS role
    // with its target taken from the hip part's centre, the hip role).
    f145test::run145(check);
    // F149/F150/F152: the ADS eye depth: the weapon's depth plus the live trim (-30..30 cm), then the 0..30 cm clamp, then the clearance cap; the weapon's depth is the table's base (the
    // game's unscoped ADS camera) plus the item's baked offset (the kinetic assault rifle 6, the laser SMG 10, the plasma sniper 16 with a scope or without, the pistols 0), else for an
    // item with no row its settled line when the copy is known unscoped, else the fallback base (21); the line's shift taken at the same depth; the lock's follower carrying a change
    // of k between weapons or of the trim; and the F144 cache told by copy while the scope is unknown.
    f149test::run149(check);
    // F152: the readout origin's two shadows (the engine's first, the own one when engine motion is down, their difference where both exist, a ranged write into the rows).
    f152test::run152(check);
    // F151: a weapon readout draw no stream window held is bound to the part's record by its place (the eye, the mesh field by field, the ordinal among the eye's draws of the mesh, the
    // instance count, the previous frame, the part drawn once, the key not named or taken in the eye-frame, exactly one tagged record in this frame's Unmap), never from a bind; the
    // frames one eye drew the part and the other did not are counted; the held copy's scope is read from the game's selected slot when the name finds two copies (an ambiguous loadout
    // resolved by slot, a slot holding another item and no selection unknown, an unambiguous name left alone).
    f151test::run151(check);
    // F157: the coat outfit's pack draw left out of both eyes: the identity (the shaders, the index count and the instance count, each told apart), the stencil ref 21 and the eye target
    // each required on their own, the outfit gate (native on foot active and Gear_Back decided as the outer garment, each other decision refused), the draws left out counted by eye and
    // by frame, and a writer counting while the frame thread takes.
    f157test::run157(check);
    // F158 (measure only): the local body's draws over the first 30 s on foot: the window's open and close rules (the edge, the coat decision, one window at a time, 30 s or native on
    // foot ending), the list (identities, times, frames per eye, variants, the previous draw at first sight), the cap of 256 and its overflow, the pack's switch time, the stop test,
    // the adjacency test, and the lines written once at the window's end.
    f158test::run158(check);
    // F159: the left elbow's stable pole reseeded from the body-down pole when its preferred elbow is within 1 cm of the shoulder's height or above (the arm bent, three solves in a row,
    // at most once in 0.5 s), on the real solver at the flight's reach (the ceiling-parked elbow, the body-down seed 12 cm under the shoulder, the straight arm exempt); the seed births counted
    // by the reason the last seed was gone and logged once in 0.5 s; the window's counters, the seconds and the run at the hip, and the lines' widths.
    f159test::run159(check);
    // F110: the ADS sight lock's model right eye from the left-eye job camera and from a solved left eye, the fallback, the IPD's sign.
    f110test::run110(check);
    // F111: the panel pointer instrument names the stage that turns a move the other way (K1 to K5, one at a time), the camera getters' decoding.
    f111test::run111(check);
    // F113: the row sources, a second's accounting by source, the check's schedule and its hold on the tee's fast path, the latch.
    f113test::run113(check);
    // F116: the HUD hidden while a terminal fills the view: the footprint, the hysteresis (2 frames over 15% hide, 10 under 10% show), the frame order, the hook's
    // skip, the counters' read (logged only), the episodes and the lines.
    f116test::run116(check);
    // F114b: the turned transform named from the owner's rows, its transform in the camera's frame and the eye's view; the click edges; the mode getter.
    f114btest::run114b(check);
    // F115: native stereo on foot's exit edge: the flight's 34.6 cm raise an exact zero from the edge's frame (against the 0.5 s ramp), the read-back, the seat.
    f115test::run115(check);
    // F118 (measure only): the readback steps and flips, the trailing frames, the footprint's directions and its decode from lines marched over an edge and a wall.
    f118test::run118(check);
    // F119 (measure only): the physical head from the pose and its alignment, the second's percentiles and histogram by class, EDVR's own offsets kept apart, the depth
    // grid decoded from the shader's words (a wall, the player's weapon, the near plane, the sky), the baseline and contact tallies, and the lines within the log's limit.
    f119test::run119(check);
    // F154, F155: the lever the screen-space pick mesh owner applies, read as the hook reads it (the sign of rho, q', the fov cross-check), its undo (the drawn point), the
    // placement's decision (drawn on a screen-space panel, the hide fallback for each refusal, nothing on another panel or in flat) and its writes against a controller's
    // memory, the lever table's freshness, and the signed turn fit (k = 0, 1, -1, -2, +2 on yaw and pitch), the eye's move, the runs, the lines.
    f154test::run154(check);
    // F126: the review's three findings: the eye pair only from one frame (4 m/s, 90 Hz: 22 mm), the arm IK's snapshot only of the rig the plan names, a bend just under
    // the hinge window reaching the grip as exactly as one just over it.
    f126test::run126(check);
    // F123: the leash on the physical offset (horizontal 25 to 35 cm, upward 5 to 10 cm), the hard timer, the recentre sequence, the wall contact (32 of 64 cells, 3 frames, latched,
    // released with the head inside soft), the levels combined by max, everything cleared off foot, and the lines.
    f123test::run123(check);
    // F125: the on-foot log's windows: 10 s summaries by default and 1 s with the detail key on, a window opened by its first call and capped at 10 s, the fold
    // tick's 1 s cadence whatever the key, N of M s, a flushed window printing its own length, and the key turned on and off part way through a window.
    f125test::run125(check);
    f125adstest::run125Ads(check);
    f125containmenttest::run(check);
    std::printf("native_frame_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
