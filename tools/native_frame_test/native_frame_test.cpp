#include "../../src/common/native_frame.h"
#include "../../src/common/comfort_fade.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/config.h"
#include "../../src/common/native_render_settings.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/journal_watch.h"
#include "../../src/d3d11/native_on_foot.h"   // F36: weapon stability steps aside while native on foot is active

#include <d3d11.h>
#include <wrl/client.h>
#include "../../src/d3d11/draw_record_tag.h"   // F44: the marker probe's tag, find and match (pure parts)

#include <array>
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
        return o.reservedProjectionChannel == 0;
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
    defaultOutput.reservedProjectionChannel = 0xA5A5A5A5u;
    EdvrNativeFrameInput defaultFrame = input(41, 7, 11);
    check(table.beginFrame(table.context, &defaultFrame, &defaultOutput) == S_OK &&
              defaultOutput.version == EDVR_NATIVE_FRAME_VERSION_3 &&
              defaultOutput.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 &&
              defaultOutput.deferredPacing == 0 &&
              defaultOutput.reservedProjectionChannel == 0xA5A5A5A5u,
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
            // The rest chain: rest locals composed parent first match the explorer cam walk (position and rotation).
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
            const bool okWalk = edvr::ecm::walkJointToModel(locals, parents, 9, 6, walk, &depth) &&
                                edvr::ecm::walkJointRotationToModel(locals, parents, 9, 6, rot);
            double err = 0.0;
            for (int i = 0; i < 3; ++i) err = std::max(err, std::fabs(static_cast<double>(model[12 + i]) - walk[i]));
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k) err = std::max(err, std::fabs(static_cast<double>(model[i * 4 + k]) - rot[i * 3 + k]));
            check(okModel && okWalk && err < 1e-5, "F49: restLocal x parent, composed to the root, equals the explorer cam walk");
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
                check(cl && E2[0] >= 0.25 - 1e-9 && E2[1] <= S2[1] + 1e-9 && nf::ikElbowOutClampCm(45.0) == 30.0 && nf::ikElbowOutClampCm(-1.0) == 0.0 &&
                          nf::ikSwivelTrimClampDeg(120.0) == 90.0 && nf::ikSquareAmountClamp(std::numeric_limits<double>::quiet_NaN()) == 1.0,
                      "F57: the clamp keeps the elbow out by the slider's distance (25 cm); the sliders clamp to their ranges");
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
                          nf::kIkElbowOutM == 0.06 && nf::kIkElbowOutCmDefault == 6.0,
                      "F59: the elbow-out clamp measures from the arm's own shoulder (an inboard elbow the midline passed is clamped to S.x + 6 cm, "
                      "both sides); the default is 6 cm");
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
                          std::fabs(nf::ikLerp(0.10, 0.06, 0.5) - 0.08) < 1e-12 &&
                          nf::ikClampOr(std::numeric_limits<double>::quiet_NaN(), 0.0, 10.0, 3.0) == 3.0 && nf::ikClampOr(12.0, 0.0, 10.0, 3.0) == 10.0,
                      "F59: the ADS weight ramps in 0.15 s (half at 0.075 s) and out; the gate relaxes to 0.6..1.2 m with it; the sliders lerp and "
                      "clamp (NaN takes the default)");
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
                          nf::kNeckRestOffset[2] == -0.161 && nf::kIkAdsForwardCmDefault == 0.0,
                      "F62a: the full-3P neck rests 0.155 m below and 0.09 m behind the eye (0.161 kept outside full 3P); the ADS shoulder forward defaults to 0");
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
                      "F63: the sight lock's e: P14w - E_R (E_R = cam + half IPD x right) with the forward part removed, in body axes (right 1.8, up "
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
                check(okFlip && fl && std::fabs(rF[0] - 1.0) < 1e-12 && degenerate && nf::adsSightHalfIpd(false, 0.0) == 0.0315 &&
                          nf::adsSightHalfIpd(true, 0.0325) == 0.0325 && nf::adsSightHalfIpd(true, -0.031) == 0.031 &&
                          nf::adsSightHalfIpd(true, 0.06) == 0.0315 && nf::adsSightHalfIpd(true, 0.001) == 0.0315,
                      "F63: the eye's right axis follows the body's right (a mirrored FP row is flipped); the half IPD from the right eye's offset, "
                      "3.15 cm when unknown or out of range");
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
                check(sqW.ok && sqW.src == nf::kSquareWeapon && std::fabs(sqW.phiW) < 1e-9 && std::fabs(sqW.phiSh + 38.0 * kR) < 1e-9 && sqW.phi == sqW.phiW &&
                          sqS.src == nf::kSquareShoulders && sqS.phi == phiDirect && std::fabs(uStarW[2] - uStarS[2] - 0.18 * std::sin(blade)) < 1e-9 &&
                          uStarW[2] - uStarS[2] > 0.08 && uStarW[2] - uStarS[2] < 0.115,
                      "F69b: a stance bladed 38 deg with the weapon on the aim gives phi 0 (the shoulders' would be -38) and U*'s left shoulder 11 cm further forward; "
                      "unarmed it is ikSquareToForward unchanged, bit for bit");
                // A 3P body 30 deg off the FP aim (the aim-lag case): phi = -30, and the turn lands the barrel on the aim.
                nf::BarrelDirs bdLag = bd1;
                bdLag.b3[0] = std::sin(30.0 * kR);
                bdLag.b3[1] = 0.0;
                bdLag.b3[2] = std::cos(30.0 * kR);
                const nf::SquareYaw sqLag = nf::squareYaw(true, bdLag, uL, uR, aim);
                double Rt[9];
                nf::ikRotAxis(upV, sqLag.phi, Rt);
                const double turned[3] = {bdLag.b3[0] * Rt[0] + bdLag.b3[1] * Rt[3] + bdLag.b3[2] * Rt[6], bdLag.b3[0] * Rt[1] + bdLag.b3[1] * Rt[4] + bdLag.b3[2] * Rt[7],
                                          bdLag.b3[0] * Rt[2] + bdLag.b3[1] * Rt[5] + bdLag.b3[2] * Rt[8]};
                nf::BarrelDirs bdUp = bd1;
                bdUp.b3[0] = 0.1;
                bdUp.b3[1] = 0.99;
                bdUp.b3[2] = 0.0;
                const nf::SquareYaw sqUp = nf::squareYaw(true, bdUp, uL, uR, aim), sqNone = nf::squareYaw(true, nf::BarrelDirs{}, uL, uR, aim);
                double keptL[3];
                nf::turnAboutVertical(uL, pivot, sqUp.phi, keptL);
                check(std::fabs(sqLag.phi + 30.0 * kR) < 1e-9 && std::fabs(turned[0]) < 1e-9 && std::fabs(turned[2] - 1.0) < 1e-9 &&
                          sqUp.src == nf::kSquarePreservePose && sqUp.phi == 0.0 && sqUp.weaponWhy == nf::kSquareWeapon3pVertical && sqUp.b3Xz == 0.1 &&
                          sqUp.bFXz == 1.0 && keptL[0] == uL[0] && keptL[2] == uL[2] && sqNone.src == nf::kSquarePreservePose && sqNone.phi == 0.0 &&
                          sqNone.weaponWhy == nf::kSquareWeaponNoAxes && sqS.src == nf::kSquareShoulders && sqS.phi == phiDirect,
                      "F70a: when armed weapon yaw is unavailable (latched 3P row nearly vertical or no axes), preserve the authored stance with phi 0; the old shoulder fallback "
                      "would de-blade this 38 deg pose. Unarmed square-up remains the shoulder rule");
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
                // The setting: 0..30 cm, NaN reads the default 15.
                check(nf::adsEyeDistanceClampCm(std::numeric_limits<double>::quiet_NaN()) == 15.0 && nf::adsEyeDistanceClampCm(-3.0) == 0.0 && nf::adsEyeDistanceClampCm(45.0) == 30.0 &&
                          nf::adsEyeDistanceClampCm(12.5) == 12.5 && nf::kAdsEyeDistDefaultCm == 15.0 && nf::kAdsSightClampM == 0.40,
                      "F70: the eye distance is 0..30 cm, default 15, and the lock's clamp is 40 cm");
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
                nf::AdsActionReject reject;
                const bool earlyReject = nf::adsActionRejectStep(reject, true, 1000, true, false, 7);
                const bool oldJournal = nf::adsActionRejectStep(reject, true, 2600, true, false, 7);
                const bool freshJournal = nf::adsActionRejectStep(reject, true, 2600, true, false, 8);
                const nf::AdsInputChoice veto = nf::adsInputChoice(true, true, true, true, true, 100, freshJournal);
                const bool heldLatch = nf::adsActionRejectStep(reject, true, 4000, true, true, 9);
                const nf::AdsInputChoice latched = nf::adsInputChoice(true, true, true, true, true, 100, heldLatch);
                const bool staleLatchHeld = reject.latched; // runtime skips reject-step on stale samples and carries this latch into the chooser
                const nf::AdsInputChoice staleLatched = nf::adsInputChoice(true, true, true, false, true, nf::kAdsActionFreshUs + 1, staleLatchHeld);
                const bool releasedLatch = nf::adsActionRejectStep(reject, false, 4001, true, false, 9);
                check(!earlyReject && !oldJournal && freshJournal && veto.journalVeto && !veto.ads &&
                          !latched.ads && latched.journalVeto && staleLatchHeld && !staleLatched.ads && staleLatched.journalVeto && !releasedLatch && !reject.latched,
                      "F70b: a post-action journal sample after the 1.5 s grace vetoes a rejected held action until logical release");
                check(opticOk && opticTranslationInvariant && opticDegenerateRefused && std::fabs(opticRot.yawDeg - 30.0) < 1e-4,
                      "F70c: joint 14's full rigid basis composes with root rotation, root translation cannot alter directions, and degenerate basis is refused");
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
                    check(nativeGate && originalIdentity && patchedIdentity && branchDelta && foreignChangeRefused,
                          "F70d: weapon UI patch is wanted only for native-on-foot sessions, verifies the full original/patched byte identity, changes only the proven branch byte, and refuses foreign bytes on restore");
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
              "F62b: the version 3 input is the version 2 words plus the LOCAL head (12 floats), its flag and a reserved word");
        edvr::nativeFrameSetAdsPitch(0.0f);
    }
    edvr::g_nativeOnFootActive.store(false);
    g_journalActive = g_onFootKnown = g_onFoot = false;

    // ---- Explorer Cam's comfort fade (comfort_fade.h; EdvrNativeFrameOutput::fadeAlpha, version 5) --------------------------------------------------------------
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
        v3.reservedProjectionChannel = 0xA5A5A5A5u;
        v3.fadeAlpha = -123.0f;
        EdvrNativeFrameInput v3Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v3Frame, &v3) == S_OK && v3.reservedProjectionChannel == 0xA5A5A5A5u && v3.fadeAlpha == -123.0f, "...and a version 3 caller likewise");
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
        const bool supportFrameOk = edvr::nofoot::supportPalmFrame(rightPalm, 1, -1, cup, leftPalm);
        const double leftPhysicalNormalX = supportFrameOk ? -leftPalm[8] : 0.0;
        const double det = supportFrameOk ?
            leftPalm[0] * (leftPalm[5]*leftPalm[10] - leftPalm[6]*leftPalm[9]) -
            leftPalm[1] * (leftPalm[4]*leftPalm[10] - leftPalm[6]*leftPalm[8]) +
            leftPalm[2] * (leftPalm[4]*leftPalm[9] - leftPalm[5]*leftPalm[8]) : 0.0;
        check(supportFrameOk && leftPhysicalNormalX > 0.99 && std::fabs(det - 1.0) < 1e-5,
              "F70b: support palm faces the gripping hand with a proper rigid basis");

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
    std::printf("native_frame_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
