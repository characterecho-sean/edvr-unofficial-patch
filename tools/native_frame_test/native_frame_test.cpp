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
#include <limits>
#include <string>
#include <thread>

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

    std::printf("native_frame_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
