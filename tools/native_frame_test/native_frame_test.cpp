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
            dt::iconOffsetNdc(2016.0, e);
            double e0[2] = {1.0, 1.0};
            dt::iconOffsetNdc(0.0, e0);
            check(e[0] == 0.0 && std::fabs(e[1] * 2016.0 * 0.5 - 36.3) < 1e-9 && e[1] > 0.0 && e0[0] == 0.0 && e0[1] == 0.0,
                  "F53: e is 36.3 px up (NDC y up) at the target's height");
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
        // F62c: the patch decision without F53's trust gate, the 0.1 NDC limit, the one-frame hold, and the waypoint series' crop about this
        // frame's P.
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
            dt::iconOffsetNdc(H, e);
            float m4[4] = {1.2f, 0, 0.1f, 0}, m5[4] = {0, 1.1f, -0.05f, 0};
            const double x0[3] = {11.02, 2.01, 3.0};
            double lm[2] = {}, ldx = 0.0, ldy = 0.0, lmc[3];
            const int dwhy = dt::ndcLagDelta(wr, wo, m4, m5, r7, wo, x0, wp, e, dt::kPatchGateNdc, lm, &ldx, &ldy);
            dt::PatchHold ph;
            const dt::PatchVerdict pv = dt::patchDecide(true, true, true, true, dwhy, ldx, ldy, 100, ph);
            dt::ndcOffsetRows(m4, m5, r7, pv.dx, pv.dy);
            dt::markerClip(m4, m5, r7, x0, wo, lmc);
            check(okT && apart > 4.5 && apart < 5.5 && !dt::sceneRowsTrusted(true, apart) && pv.patch && !pv.held && pv.why == dt::kPatchApplied &&
                      std::fabs(lmc[0] / lmc[2] - (cs[0] / cs[3] + e[0])) < 1e-5 && std::fabs(lmc[1] / lmc[2] - (cs[1] / cs[3] + e[1])) < 1e-5 &&
                      ph.frame == 100u,
                  "F62c: scene and temporal rows 5 px apart at P: F53's trust gate refused the frame; F62c patches it onto ndc(P through the scene "
                  "rows) + e");
            // (2) The 0.1 NDC limit: just under it patches; at it or over it (either axis) is refused as |d| with nothing to hold; through
            // ndcLagDelta at the patch gate, c moved so the delta is 0.0999 then 0.1001 in x.
            dt::PatchHold h1, h2;
            const dt::PatchVerdict under = dt::patchDecide(true, true, true, true, dt::kDeltaOk, 0.0999, -0.0999, 10, h1);
            const dt::PatchVerdict at = dt::patchDecide(true, true, true, true, dt::kDeltaOk, 0.1, 0.0, 10, h2);
            const dt::PatchVerdict overY = dt::patchDecide(true, true, true, true, dt::kDeltaOk, 0.0, -0.15, 11, h2);
            float q4[4] = {1.2f, 0, 0.1f, 0}, q5[4] = {0, 1.1f, -0.05f, 0};
            double qm[2] = {}, qdx = 0.0, qdy = 0.0;
            const double cA[2] = {lm[0] + 0.0999, lm[1]}, cB[2] = {lm[0] + 0.1001, lm[1]};
            const int wA = dt::ndcLagDelta(wr, wo, q4, q5, r7, wo, x0, wp, cA, dt::kPatchGateNdc, qm, &qdx, &qdy);
            dt::PatchHold h3, h4;
            const dt::PatchVerdict vA = dt::patchDecide(true, true, true, true, wA, qdx, qdy, 20, h3);
            const int wB = dt::ndcLagDelta(wr, wo, q4, q5, r7, wo, x0, wp, cB, dt::kPatchGateNdc, qm, &qdx, &qdy);
            const dt::PatchVerdict vB = dt::patchDecide(true, true, true, true, wB, qdx, qdy, 20, h4);
            check(dt::kPatchGateNdc == 0.1 && under.patch && under.why == dt::kPatchApplied && !at.patch && at.why == dt::kPatchGate &&
                      !overY.patch && overY.why == dt::kPatchGate && wA == dt::kDeltaOk && vA.patch && std::fabs(vA.dx - 0.0999) < 1e-9 &&
                      wB == dt::kDeltaGate && !vB.patch && vB.why == dt::kPatchGate && h4.frame == ~0u,
                  "F62c: |dx|, |dy| under 0.1 NDC patch; 0.1 and over (either axis) are refused as |d|, with nothing to hold");
            // (3) The one-frame hold: frame 200 patches (0.01, -0.02); 201, refused as |d|, patches with 200's delta (held); 202, refused as
            // stale, is not patched (one frame only). Unmatched and both-eyes refusals never hold; a stale frame right after a good one does;
            // no P and w <= 0 are their own refusals.
            dt::PatchHold hh, hu;
            const dt::PatchVerdict f200 = dt::patchDecide(true, true, true, true, dt::kDeltaOk, 0.01, -0.02, 200, hh);
            const dt::PatchVerdict f201 = dt::patchDecide(true, true, true, true, dt::kDeltaGate, 0.3, 0.0, 201, hh);
            const dt::PatchVerdict f202 = dt::patchDecide(true, true, false, true, -1, 0.0, 0.0, 202, hh);
            const dt::PatchVerdict g300 = dt::patchDecide(true, true, true, true, dt::kDeltaOk, 0.01, 0.01, 300, hu);
            const dt::PatchVerdict unm = dt::patchDecide(false, true, true, true, dt::kDeltaOk, 0.0, 0.0, 301, hu);
            const dt::PatchVerdict both = dt::patchDecide(true, false, true, true, dt::kDeltaOk, 0.0, 0.0, 301, hu);
            const dt::PatchVerdict stale = dt::patchDecide(true, true, false, true, -1, 0.0, 0.0, 301, hu);
            const dt::PatchVerdict noP = dt::patchDecide(true, true, true, false, -1, 0.0, 0.0, 305, hu);
            const dt::PatchVerdict wNeg = dt::patchDecide(true, true, true, true, dt::kDeltaWorldBehind, 0.0, 0.0, 306, hu);
            const dt::PatchVerdict mNeg = dt::patchDecide(true, true, true, true, dt::kDeltaMarkerBehind, 0.0, 0.0, 307, hu);
            check(f200.patch && !f200.held && f201.patch && f201.held && f201.why == dt::kPatchGate && f201.dx == 0.01 && f201.dy == -0.02 &&
                      !f202.patch && f202.why == dt::kPatchStale && g300.patch && !unm.patch && unm.why == dt::kPatchUnmatched && !both.patch &&
                      both.why == dt::kPatchBothEyes && stale.patch && stale.held && stale.why == dt::kPatchStale && stale.dx == 0.01 &&
                      !noP.patch && noP.why == dt::kPatchNoP && !wNeg.patch && wNeg.why == dt::kPatchW && !mNeg.patch && mNeg.why == dt::kPatchW,
                  "F62c: the last good delta is held for the one frame after it, not two; unmatched and both-eyes refusals never hold; no P "
                  "and w <= 0 are refused");
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
            dt::PatchHold ph;
            for (uint32_t f = 1001; f <= 1090; ++f)
                if (dt::patchDecide(h.held(f), true, true, true, dt::kDeltaOk, 0.01, 0.0, f, ph).patch) ++patched;
            // (2) A contradicting read-back drops it: the held place read record 12 while the key was tagged elsewhere; or (no find at all)
            // a record other than the held one. A read of the key's record at another place moves the hold there.
            const int contra = dt::holdEvidence(h, 1030, 3, 12, 43, false, true);
            const bool dropped = !h.held(1030) && !h.held(1031);
            dt::HeldMatch h3, h4;
            dt::holdEvidence(h3, 50, 2, 9, 43, true, true);
            const int contra2 = dt::holdEvidence(h3, 60, 2, 10, 43, false, false);
            dt::holdEvidence(h4, 70, 3, 8, 43, true, true);
            const int moved = dt::holdEvidence(h4, 72, 5, 8, 43, true, true);
            const dt::PatchVerdict afterDrop = dt::patchDecide(h.held(1032), true, true, true, dt::kDeltaOk, 0.01, 0.0, 1032, ph);
            check(dt::kMatchHoldFrames == 90u && c0 == dt::kHoldConfirm && held89 && held90 && !held91 && !heldBefore && other == dt::kHoldNone &&
                      same == dt::kHoldNone && stillHeld && stale == dt::kHoldNone && hs.held(1020) && patched == 90u &&
                      contra == dt::kHoldContradict && dropped && contra2 == dt::kHoldContradict && !h3.held(60) && moved == dt::kHoldConfirm &&
                      h4.ord == 5 && h4.frame == 72u && !afterDrop.patch && afterDrop.why == dt::kPatchUnmatched,
                  "F63: a match is held 90 frames from its newest confirmation and patches every frame of them; a contradicting read-back drops "
                  "it; older evidence and evidence about another place say nothing; the key's record at another place moves it");
            // (3) The bias EMA: alpha 0.02 on fresh patched frames only; in the closed loop (drawn = ndc_P + e_used + beta, the residual
            // against the target used is beta) the drawn marker settles on ndc_P + e; each component clamped to 0.02 NDC; |r| >= 1 refused.
            dt::BiasEma be, idle, loop, big;
            const double r0[2] = {0.004, -0.002};
            be.update(r0, true);
            const bool firstStep = std::fabs(be.b[0] - 0.02 * 0.004) < 1e-15 && std::fabs(be.b[1] + 0.02 * 0.002) < 1e-15 && be.n == 1u;
            for (int i = 0; i < 100; ++i) idle.update(r0, false);
            const double beta[2] = {0.003, -0.001}, eFix[2] = {0.0, 0.036};
            double eUsed[2] = {};
            for (int i = 0; i < 400; ++i) loop.update(beta, true);
            loop.target(eFix, eUsed);
            const double drawn[2] = {eUsed[0] + beta[0], eUsed[1] + beta[1]};   // ndc_P = 0
            const double rBig[2] = {0.5, 0.0}, rHuge[2] = {2.0, 0.0};
            for (int i = 0; i < 2000; ++i) big.update(rBig, true);
            const uint64_t bigN = big.n;
            const bool huge = big.update(rHuge, true);
            check(dt::kBiasAlpha == 0.02 && firstStep && idle.b[0] == 0.0 && idle.b[1] == 0.0 && idle.n == 0u &&
                      std::fabs(drawn[0] - eFix[0]) < 1e-5 && std::fabs(drawn[1] - eFix[1]) < 1e-5 && std::fabs(big.b[0] - dt::kBiasMaxNdc) < 1e-12 &&
                      !huge && big.n == bigN,
                  "F63: the icon offset's bias: an EMA (alpha 0.02) of fresh patched frames' residuals, never of unpatched ones; the drawn marker "
                  "settles on ndc_P + e; clamped to 0.02 NDC");
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

            // F51: dh is clamped to 0..0.5 m and low-passed (tau 1 s) from 0; a frame without a measurement holds it.
            nf::RaiseDh dh;
            for (int i = 0; i < 90; ++i) dh.step(0.231, true, 1.0 / 90.0);   // 1 s at 90 Hz: 1 - 1/e of the way
            const bool oneTau = std::fabs(dh.lp - 0.231 * (1.0 - std::exp(-1.0))) < 1e-6;
            for (int i = 0; i < 900; ++i) dh.step(0.231, true, 1.0 / 90.0);
            const bool settled = std::fabs(dh.lp - 0.231) < 1e-4;
            const double before = dh.lp;
            dh.step(0.9, false, 1.0 / 90.0);
            const bool held = dh.lp == before;
            nf::RaiseDh hi, lo;
            for (int i = 0; i < 2000; ++i) {
                hi.step(0.9, true, 1.0 / 90.0);
                lo.step(-0.2, true, 1.0 / 90.0);
            }
            check(oneTau && settled && held && std::fabs(hi.lp - 0.5) < 1e-6 && lo.lp == 0.0 && std::fabs(lo.raw + 0.2) < 1e-12,
                  "F51: dh low-passes from 0 (tau 1 s), holds without a measurement, and is clamped to 0..0.5 m");
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
            // F53: the raise is the constant raise_cm while on (clamped 0..0.5 m), nothing while off; the live dh does not enter it.
            check(nf::raiseConstM(false, 10.0) == 0.0 && std::fabs(nf::raiseConstM(true, 10.0) - 0.10) < 1e-12 &&
                      std::fabs(nf::raiseConstM(true, nf::kRaiseDefaultCm) - 0.10) < 1e-12 && nf::raiseConstM(true, 80.0) == 0.5 &&
                      nf::raiseConstM(true, -5.0) == 0.0 && nf::raiseConstM(true, std::numeric_limits<double>::quiet_NaN()) == 0.0,
                  "F53: the raise is a constant 0.10 m by default while on, clamped to 0..0.5 m, and zero while off");
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
            // F55: the FP shoulder drop: the key clamps to 0..20 cm; the weight ramps linearly over 0.2 s in and out (ADS); the subtree of 19, 20.
            {
                check(nf::dropClampCm(-5.0) == 0.0 && nf::dropClampCm(0.0) == 0.0 && nf::dropClampCm(7.5) == 7.5 && nf::dropClampCm(25.0) == 20.0 &&
                          nf::dropClampCm(std::numeric_limits<double>::quiet_NaN()) == 0.0 && std::fabs(nf::dropM(1.0, 30.0) - 0.20) < 1e-12 &&
                          nf::dropM(0.0, 10.0) == 0.0 && std::fabs(nf::dropM(0.5, 10.0) - 0.05) < 1e-12,
                      "F55: the shoulder drop clamps to 0..20 cm (NaN and negatives are 0) and scales by the weight");
                nf::DropBlend b;
                const double dt = 1.0 / 90.0;
                int framesIn = 0;
                while (b.w < 1.0 && framesIn < 100) {
                    b.step(true, dt);
                    ++framesIn;
                }
                nf::DropBlend b2;
                for (int f = 0; f < 9; ++f) b2.step(true, dt);
                const double wHalf = b2.w;
                int framesOut = 0;
                while (b.w > 0.0 && framesOut < 100) {
                    b.step(false, dt);   // ADS starts
                    ++framesOut;
                }
                check(framesIn >= 18 && framesIn <= 19 && framesOut >= 18 && framesOut <= 19 && std::fabs(wHalf - 0.5) < 1e-9 && b.w == 0.0,
                      "F55: the drop's weight ramps to 1 in 0.2 s (18 frames at 90 Hz), is half at 0.1 s, and back to 0 in 0.2 s when ADS starts");
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
            // F60: the body fitted to the weapon: Sd from the grips and the FP forward (k_reach 0.82 of the arm behind, 0.10 m below; ADS raise
            // and forward push), d_w = dB + (Sd - Umid) and its independence of the d it was measured at, the blend, the 0.3 s ramp.
            {
                const auto near3 = [](const double* a, double x, double y, double z) {
                    return std::fabs(a[0] - x) < 1e-9 && std::fabs(a[1] - y) < 1e-9 && std::fabs(a[2] - z) < 1e-9;
                };
                const double gL[3] = {-0.10, 1.30, 0.50}, gR[3] = {0.10, 1.30, 0.50}, fwdPitched[3] = {0.0, -0.3, 2.0}, fwdYaw[3] = {1.0, 0.0, 0.0},
                             fwdUp[3] = {0.0, 1.0, 0.05};
                const double back = 0.82 * 0.59;
                double sd[3], sdYaw[3], sdAds[3], sdBad[3];
                const bool okSd = nf::fitShoulderTarget(gL, gR, fwdPitched, 0.59, 0.0, nf::kFitReach, sd);
                const bool okYaw = nf::fitShoulderTarget(gL, gR, fwdYaw, 0.59, 0.0, nf::kFitReach, sdYaw);
                // F62a: in ADS k_reach is 1.0 (fitReachK at weight 1) and the forward push no longer enters Sd.
                const bool okAds = nf::fitShoulderTarget(gL, gR, fwdPitched, 0.59, 0.03, nf::fitReachK(1.0), sdAds);
                const bool okBad = nf::fitShoulderTarget(gL, gR, fwdUp, 0.59, 0.0, nf::kFitReach, sdBad);
                check(okSd && near3(sd, 0.0, 1.20, 0.50 - back) && okYaw && near3(sdYaw, -back, 1.20, 0.50) && okAds &&
                          near3(sdAds, 0.0, 1.23, 0.50 - 0.59) && !okBad && near3(sdBad, 0.0, 0.0, 0.0),
                      "F60: Sd = the grip midpoint less the FP forward (xz, unit) x k_reach (l1 + l2) and 0.10 m down; ADS adds the raise, with "
                      "k_reach 1.0 and no forward push (F62a); a vertical forward refuses");
                const double umid[3] = {0.0, 1.35, -0.05}, dB[3] = {0.02, 0.0, 0.01};
                double dw[3];
                nf::fitBodyTarget(dB, sd, umid, dw);
                // G subtracts d: the same body measured at d + delta sees the grips (so Sd) moved by -delta; d_w does not move.
                const double delta[3] = {0.10, 0.0, 0.05}, gL2[3] = {gL[0] - 0.10, gL[1], gL[2] - 0.05}, gR2[3] = {gR[0] - 0.10, gR[1], gR[2] - 0.05};
                const double dB2[3] = {dB[0] + delta[0], dB[1] + delta[1], dB[2] + delta[2]};
                double sd2[3], dw2[3];
                nf::fitShoulderTarget(gL2, gR2, fwdPitched, 0.59, 0.0, nf::kFitReach, sd2);
                nf::fitBodyTarget(dB2, sd2, umid, dw2);
                check(near3(dw, 0.02, -0.15, 0.01 + (0.50 - back) + 0.05) && near3(dw2, dw[0], dw[1], dw[2]),
                      "F60: d_w = G's d + (Sd - Umid) in body axes, and it does not depend on the d it was measured at (open loop, no integral)");
                const double dHead[3] = {0.04, 0.02, -0.03};
                double b0[3], b1[3], bh[3], bHalf[3], bOver[3];
                nf::fitBlend(dHead, dw, 0.0, false, b0);
                nf::fitBlend(dHead, dw, 1.0, false, b1);
                nf::fitBlend(dHead, dw, 1.0, true, bh);
                nf::fitBlend(dHead, dw, 0.5, false, bHalf);
                nf::fitBlend(dHead, dw, 2.0, false, bOver);
                check(near3(b0, dHead[0], dHead[1], dHead[2]) && near3(b1, dw[0], dw[1], dw[2]) && near3(bh, dw[0], dHead[1], dw[2]) &&
                          near3(bHalf, 0.5 * (dHead[0] + dw[0]), 0.5 * (dHead[1] + dw[1]), 0.5 * (dHead[2] + dw[2])) && near3(bOver, dw[0], dw[1], dw[2]),
                      "F60: the blend returns d_head at fit 0 and d_w at fit 1 (the head's up kept with the vertical off; fit clamped to 0..1)");
                nf::IkBlend ramp{0.0f, nf::kFitRampSec};
                ramp.step(true, 0.15);
                const float rHalf = ramp.w;
                ramp.step(true, 0.15);
                const float rFull = ramp.w;
                ramp.step(false, 0.15);
                const float rOutHalf = ramp.w;
                ramp.step(false, 0.15);
                const float rOut = ramp.w;
                ramp.step(false, 0.15);
                check(nf::kFitRampSec == 0.3 && std::fabs(rHalf - 0.5f) < 1e-6f && rFull == 1.0f && std::fabs(rOutHalf - 0.5f) < 1e-6f && rOut == 0.0f &&
                          ramp.w == 0.0f,
                      "F60: the fit ramps in over 0.3 s and, with no weapon, out to 0 over 0.3 s (half at 0.15 s)");
                check(nf::kFitReach == 0.82 && nf::kFitDropM == 0.10 && nf::kFitArmLenDefaultM == 0.59 && nf::kFitWeaponDefault == 0.7 &&
                          nf::fitArmLen(std::numeric_limits<double>::quiet_NaN()) == 0.59 && nf::fitArmLen(0.1) == 0.59 && nf::fitArmLen(0.61) == 0.61 &&
                          nf::fitWeaponClamp(std::numeric_limits<double>::quiet_NaN()) == 0.7 && nf::fitWeaponClamp(1.5) == 1.0 &&
                          nf::fitWeaponClamp(-1.0) == 0.0,
                      "F60: k_reach 0.82, the 0.10 m drop, the 0.59 m arm fallback (outside 0.2..1.5 m), the slider's 0.7 default and its 0..1 clamp");
            }
            // F62a (TEMPORARY): the fit gated by raised-ness and disengaged by a head excursion, the lateral weight and the 12 cm pull clamp,
            // the trims on the final d (once), the ADS lateral blend, the bend's limits while the fit is engaged and its suspension, the
            // full-3P eye-to-neck rest and the ADS defaults.
            {
                const auto near3 = [](const double* a, double x, double y, double z) {
                    return std::fabs(a[0] - x) < 1e-9 && std::fabs(a[1] - y) < 1e-9 && std::fabs(a[2] - z) < 1e-9;
                };
                const double qNan = std::numeric_limits<double>::quiet_NaN();
                const double kDeg = nf::kIkPi / 180.0;
                check(nf::fitRaised(0.5) == 0.0 && nf::fitRaised(1.0) == 1.0 && std::fabs(nf::fitRaised(0.75) - 0.5) < 1e-12 && nf::fitRaised(0.2) == 0.0 &&
                          nf::fitRaised(0.0) == 0.0 && nf::fitRaised(qNan) == 0.0 && nf::fitRaised(0.6) > 0.0 && nf::fitRaised(0.9) < 1.0,
                      "F62a: the raised factor is smoothstep(0.5, 1.0, grip gate): 0 at 0.5 and below (the hands hanging, gate 0.0..0.2), 1 at 1.0");
                check(nf::fitDisengage(0.15) == 1.0 && nf::fitDisengage(0.35) == 0.0 && std::fabs(nf::fitDisengage(0.25) - 0.5) < 1e-12 &&
                          nf::fitDisengage(0.05) == 1.0 && nf::fitDisengage(0.6) == 0.0,
                      "F62a: the fit disengages as the horizontal |d_w - d_head| goes from 0.15 m (full) to 0.35 m (none), half at 0.25 m");
                const double zero3[3] = {0.0, 0.0, 0.0};
                // The lateral weight: forward 1.0, lateral 0.25, e (0.08, 0, -0.08) is inside 0.15 m: the pull is (0.02, 0, -0.08).
                const double dWl[3] = {0.08, 0.0, -0.08};
                double oL[3];
                nf::FitApplied aL;
                nf::fitApply(zero3, dWl, zero3, 1.0, 0.25, true, oL, &aL);
                // The clamp: both weights 1, e (0.09, 0, -0.12) (0.15 m, disengage 1): the pull 0.15 m is clamped to 0.12 m along it.
                const double dWc[3] = {0.09, 0.0, -0.12};
                double oC[3];
                nf::FitApplied aC;
                nf::fitApply(zero3, dWc, zero3, 1.0, 1.0, true, oC, &aC);
                // Disengaged: e 0.4 m horizontal pulls nothing.
                const double dWd[3] = {0.0, 0.0, -0.40};
                double oD[3];
                nf::FitApplied aD;
                nf::fitApply(zero3, dWd, zero3, 1.0, 1.0, true, oD, &aD);
                check(near3(oL, 0.02, 0.0, -0.08) && std::fabs(aL.wFwd - 1.0) < 1e-12 && std::fabs(aL.wLat - 0.25) < 1e-12 && !aL.clamped &&
                          near3(oC, 0.072, 0.0, -0.096) && aC.clamped &&
                          std::fabs(std::sqrt(aC.pull[0] * aC.pull[0] + aC.pull[2] * aC.pull[2]) - nf::kFitPullMaxM) < 1e-12 &&
                          near3(oD, 0.0, 0.0, 0.0) && aD.disengage == 0.0 && nf::kFitPullMaxM == 0.12,
                      "F62a: the lateral pull has its own weight (0.25 of the right offset, the forward 1.0), the horizontal pull is clamped to "
                      "0.12 m along its direction, and a 0.4 m excursion pulls nothing");
                // The trims on the final d: with no fit the target is d_head - trim; at full fit it is d_head + pull - trim, the trim once.
                const double dHead[3] = {0.04, 0.02, -0.03}, dW[3] = {0.10, -0.50, 0.05}, trim[3] = {0.03, 0.0, 0.05};
                double t0[3], t1[3], tV[3];
                nf::FitApplied a1, aV;
                nf::fitApply(dHead, dW, trim, 0.0, 0.0, true, t0, nullptr);
                nf::fitApply(dHead, dW, trim, 1.0, 1.0, true, t1, &a1);
                nf::fitApply(dHead, dW, trim, 1.0, 1.0, false, tV, &aV);
                check(near3(t0, 0.01, 0.02, -0.08) && near3(t1, 0.07, 0.02, 0.0) &&
                          near3(t1, t0[0] + a1.pull[0], t0[1] + a1.pull[1], t0[2] + a1.pull[2]) && near3(tV, 0.07, -0.50, 0.0),
                      "F62a: the trims are subtracted once from the final d (no fit: d_head - trim; full fit: d_head + pull - trim, so the trim "
                      "reaches the body in full); the up is pulled only with the vertical on");
                // The ADS lateral blend: the first call is live; the flag's change outputs the last output; half at 0.15 s; live at 0.3 s.
                nf::FitAdsLat lb;
                const double l0 = lb.step(false, 0.30, 0.011);
                const double lEdge = lb.step(true, 0.00, 0.011);
                const double lHalf = lb.step(true, 0.00, 0.15);
                const double lFull = lb.step(true, 0.00, 0.15);
                const double lAfter = lb.step(true, 0.05, 0.011);
                check(l0 == 0.30 && lEdge == 0.30 && std::fabs(lHalf - 0.15) < 1e-12 && lFull == 0.0 && lAfter == 0.05 && !lb.blending &&
                          nf::kFitAdsLatSec == 0.3,
                      "F62a: d_w's lateral is blended over 0.3 s at a change of the ADS flag (no jump on the change, half at 0.15 s)");
                // The bend: the fit's limits (forward and back 8, roll 6) against F58's (45, 15, 20); the suspension above 0.35 m.
                double fF = 0.0, fB = 0.0, fR = 0.0, nF = 0.0, nB = 0.0, nR = 0.0;
                nf::bendLimits(true, &fF, &fB, &fR);
                nf::bendLimits(false, &nF, &nB, &nR);
                nf::BendClamp c1, c2, c3, c4, c5;
                const double p20 = nf::bendClampPitchLim(20.0 * kDeg, fF, fB, &c1), pM20 = nf::bendClampPitchLim(-20.0 * kDeg, fF, fB, &c2),
                             r10 = nf::bendClampRollLim(10.0 * kDeg, fR, &c3), rM173 = nf::bendClampRollLim(-173.0 * kDeg, fR, &c4),
                             r5 = nf::bendClampRollLim(5.0 * kDeg, fR, &c5);
                const double Nb[3] = {0.0, 1.5, 0.1}, Tin[3] = {0.0, 1.5, 0.1 + 0.34}, Tout[3] = {0.0, 1.5, 0.1 + 0.36}, Tnan[3] = {qNan, 1.5, 0.1};
                check(fF == 8.0 && fB == 8.0 && fR == 6.0 && nF == 45.0 && nB == 15.0 && nR == 20.0 && std::fabs(p20 - 8.0 * kDeg) < 1e-12 &&
                          c1.fwd && std::fabs(pM20 + 8.0 * kDeg) < 1e-12 && c2.back && std::fabs(r10 - 6.0 * kDeg) < 1e-12 && c3.roll &&
                          std::fabs(rM173 + 6.0 * kDeg) < 1e-12 && c4.roll && std::fabs(r5 - 5.0 * kDeg) < 1e-12 && !c5.roll &&
                          !nf::bendSuspend(Tin, Nb) && nf::bendSuspend(Tout, Nb) && nf::bendSuspend(Tnan, Nb) && nf::kBendSuspendResM == 0.35,
                      "F62a: while the fit is engaged the bend is held to pitch +-8 and roll +-6 deg (F58's 45, 15, 20 otherwise; a -173 deg roll "
                      "clamps to -6); it is suspended above a 0.35 m residual before it");
                check(nf::kNeckRestOffsetFull3P[0] == 0.0 && nf::kNeckRestOffsetFull3P[1] == -0.155 && nf::kNeckRestOffsetFull3P[2] == -0.09 &&
                          nf::kNeckRestOffset[2] == -0.161 && nf::kIkAdsForwardCmDefault == 0.0 && nf::fitReachK(0.0) == 0.82 &&
                          nf::fitReachK(1.0) == 1.0 && std::fabs(nf::fitReachK(0.5) - 0.91) < 1e-12 && nf::fitReachK(qNan) == 0.82 &&
                          nf::kFitLateralDefault == 0.25 && nf::fitLateralClamp(qNan) == 0.25 && nf::fitLateralClamp(2.0) == 1.0 &&
                          nf::fitLateralClamp(-1.0) == 0.0,
                      "F62a: the full-3P neck rests 0.155 m below and 0.09 m behind the eye (0.161 kept outside full 3P); the ADS shoulder forward "
                      "defaults to 0; k_reach goes 0.82 to 1.0 with the ADS weight; Body fit sideways defaults to 0.25, clamped to 0..1");
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
            // F62b (TEMPORARY): the ADS head-aim. The convention verified here: the seat convention is yaw + = LEFT, pitch + = UP; the game's
            // yaw delta is + RIGHT and its pitch delta + UP for signs +1; F61: a seated +a moves the camera by -a (gain 1, sign -). A head
            // yaw of +theta (left) asks the game for -theta (its sign); the game applies it (gain g, the input divided by g); the cancel is a
            // seated yaw of +theta; the camera's net turn = aim (+theta) + head (+theta) - seated (+theta) = +theta: the view follows the head.
            {
                const double kR = nf::kAdsDeg;
                // A frame-by-frame plant: the game's real yaw sign and gain against the keys' sign and the measured gain (fallback 1.0).
                struct Plant {
                    int realSign = 1;
                    double realGain = 1.0, measured = 0.0;
                    double aim = 0.0, head = 0.0, seat = 0.0;   // seat convention, radians
                    double input = 0.0;
                    void frame(double dHead, int keySign) {
                        head += dHead;
                        input = nf::adsInput(nf::adsGameFromSeatYaw(dHead, keySign), measured);
                        const double d = realGain * input;                       // the processor's delta (the game's sign)
                        measured = input != 0.0 ? d / input : measured;          // the gain the hook measures
                        aim += nf::adsSeatFromGameYaw(d, realSign);              // the aim's true turn (seat convention)
                        seat += nf::adsSeatFromGameYaw(d, keySign);              // the cancel, as the keys read the delta
                    }
                    double camera() const { return aim + head - seat; }         // F61: the seated +a moves the camera by -a
                };
                Plant p;
                p.frame(10.0 * kR, 1);
                const bool oneStep = std::fabs(p.input + 10.0 * kR) < 1e-12 && std::fabs(p.seat - 10.0 * kR) < 1e-12 &&
                                     std::fabs(p.aim - 10.0 * kR) < 1e-12 && std::fabs(p.camera() - p.head) < 1e-12;
                Plant halfGain;
                halfGain.realGain = 0.5;
                halfGain.frame(10.0 * kR, 1);   // the first frame uses the fallback gain 1.0: the aim takes half, the view still follows the head
                const bool firstHalf = std::fabs(halfGain.aim - 5.0 * kR) < 1e-12 && std::fabs(halfGain.camera() - halfGain.head) < 1e-12 && std::fabs(halfGain.measured - 0.5) < 1e-12;
                halfGain.frame(10.0 * kR, 1);   // the second uses the measured 0.5: input -20 deg, the aim takes the whole 10
                const bool secondHalf = std::fabs(halfGain.input + 20.0 * kR) < 1e-12 && std::fabs(halfGain.aim - 15.0 * kR) < 1e-12 &&
                                        std::fabs(halfGain.camera() - halfGain.head) < 1e-12;
                Plant wrong;
                wrong.realSign = -1;
                wrong.frame(10.0 * kR, 1);   // the key's sign is wrong: the aim turns away, the cancel doubles it: residual -2 x the head
                const bool inverted = std::fabs(wrong.aim + 10.0 * kR) < 1e-12 &&
                                      std::fabs(nf::adsResidualRatio(wrong.camera() - wrong.head, wrong.head) + 2.0) < 1e-12;
                check(oneStep && firstHalf && secondHalf && inverted,
                      "F62b: a head yaw of +10 deg (left, the seat's sign) asks the game for -10 deg (its sign, + right) at gain 1, the cancel is a "
                      "seated yaw of +10 deg and the camera's net turn equals the head's (+10); the measured gain (0.5) doubles the input from the "
                      "second frame; an inverted sign reads a residual of -2 x the head");
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
                // The runaway guard: 270 deg of seated yaw within 3 s trips; the same turn spread over 4 s does not.
                nf::AdsTurnWindow fast, slow;
                bool fastOver = false, slowOver = false;
                for (int i = 0; i < 100; ++i) {   // 280 deg in 1 s
                    fast.add(10000 + 10 * static_cast<uint64_t>(i), 2.8 * kR);
                    fastOver = fastOver || fast.over(10000 + 10 * static_cast<uint64_t>(i));
                }
                for (int i = 0; i < 400; ++i) {   // 280 deg over 4 s (70 deg/s): at most 210 deg in any 3 s
                    slow.add(10000 + 10 * static_cast<uint64_t>(i), 0.7 * kR);
                    slowOver = slowOver || slow.over(10000 + 10 * static_cast<uint64_t>(i));
                }
                nf::AdsTurnWindow aged;
                aged.add(10000, 280.0 * kR);
                const bool agedOut = aged.over(10000) && !aged.over(13100);
                check(fastOver && !slowOver && agedOut && std::fabs(nf::kAdsGuardTurnRad - 270.0 * kR) < 1e-12 && nf::kAdsGuardWindowMs == 3000,
                      "F62b: the runaway guard trips on 280 deg of seated yaw in 1 s, not on 280 deg over 4 s; a burst ages out after 3 s");
                // The residual guard: growing for 0.5 s above the floor and half the head's turn trips it; a steady lag residual does not.
                nf::AdsResidualGrowth grow, steady, tiny;
                bool growTrip = false, steadyTrip = false, smallTrip = false;
                uint64_t tripAt = 0;
                for (int i = 0; i <= 100; ++i) {
                    const uint64_t ms = 1000 + 10 * static_cast<uint64_t>(i);
                    const double head = 0.1 * i;                          // the head turns 10 deg over 1 s
                    if (!growTrip && grow.step(ms, 2.0 * head, head)) {   // an inverted sign: the residual is twice the head's turn
                        growTrip = true;
                        tripAt = ms;
                    }
                    steadyTrip = steadyTrip || steady.step(ms, 9.0, 40.0);   // a 9 deg lag residual against a 40 deg turn: under half of it
                    smallTrip = smallTrip || tiny.step(ms, 0.06 * i, 0.0);  // under the 8 deg floor all along (6 deg at the end)
                }
                check(growTrip && tripAt >= 1000 + 400 + 500 && tripAt <= 1000 + 400 + 510 && !steadyTrip && !smallTrip,
                      "F62b: the residual guard trips 0.5 s after the residual (twice the head's turn) passes the 8 deg floor, never on a steady "
                      "residual under half the head's turn or on one under the floor");
                // The pitch clamp: the seated pitch is held to +-60 deg; a step back toward zero passes whole.
                check(std::fabs(nf::adsPitchLimit(55.0 * kR, 10.0 * kR) - 5.0 * kR) < 1e-12 && nf::adsPitchLimit(60.0 * kR, 1.0 * kR) == 0.0 &&
                          std::fabs(nf::adsPitchLimit(60.0 * kR, -5.0 * kR) + 5.0 * kR) < 1e-12 &&
                          std::fabs(nf::adsPitchLimit(-58.0 * kR, -5.0 * kR) + 2.0 * kR) < 1e-12 &&
                          std::fabs(nf::adsClampPitch(70.0 * kR) - 60.0 * kR) < 1e-12 && std::fabs(nf::adsClampPitch(-61.0 * kR) + 60.0 * kR) < 1e-12 &&
                          nf::adsClampPitch(std::numeric_limits<double>::quiet_NaN()) == 0.0,
                      "F62b: the seated pitch is clamped to +-60 deg (55 + 10 passes 5; at 60 nothing more; back toward zero whole; NaN 0)");
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
                // The release: the pitch unwinds over 0.3 s by injecting the opposite; with the cancel following what the game applies the
                // seated pitch reaches 0 at 0.3 s and the view never moves (frames of 1/90 s, the game's gain 1, one frame of latency).
                double seatP = 20.0 * kR, aimP = 20.0 * kR, queued = 0.0, t = 0.0, viewMax = 0.0;
                const double p0 = seatP, view0 = aimP - seatP;
                for (int i = 0; i < 40; ++i) {
                    const double applied = queued;   // last frame's input lands now
                    aimP += applied;
                    seatP += applied;
                    const double tPrev = t;
                    t += 1.0 / 90.0;
                    queued = tPrev < nf::kAdsUnwindSec ? nf::adsUnwindTarget(p0, t) - nf::adsUnwindTarget(p0, tPrev) : 0.0;
                    viewMax = std::max(viewMax, std::fabs((aimP - seatP) - view0));
                }
                check(std::fabs(nf::adsUnwindTarget(p0, 0.0) - p0) < 1e-12 && std::fabs(nf::adsUnwindTarget(p0, 0.15) - 0.5 * p0) < 1e-12 &&
                          nf::adsUnwindTarget(p0, 0.3) == 0.0 && nf::adsUnwindTarget(p0, 0.5) == 0.0 && std::fabs(seatP) < 1e-9 && std::fabs(aimP) < 1e-9 &&
                          viewMax < 1e-12,
                      "F62b: the release unwinds the seated pitch from 20 deg to 0 over 0.3 s (half at 0.15 s) while the game's pitch goes back the "
                      "same way: the view does not move");
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
                // The body: the head's target ignored (d_w less the trims) by the weight; the up kept with the vertical off.
                const double fitOut[3] = {0.02, 0.01, -0.05}, dW[3] = {0.30, 0.2, -0.40}, trim[3] = {0.01, 0.0, 0.02};
                double det[3], detHalf[3], det0[3];
                nf::adsFitDetach(fitOut, dW, trim, true, 1.0, det);
                nf::adsFitDetach(fitOut, dW, trim, true, 0.5, detHalf);
                nf::adsFitDetach(fitOut, dW, trim, false, 0.0, det0);
                check(std::fabs(det[0] - 0.29) < 1e-12 && std::fabs(det[2] + 0.42) < 1e-12 && det[1] == 0.01 && std::fabs(detHalf[0] - 0.155) < 1e-12 &&
                          det0[0] == 0.02 && det0[1] == 0.01 && det0[2] == -0.05,
                      "F62b: in ADS the body's target goes to d_w less the trims by the weight (half way at 0.5), the up kept with the vertical off");
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
                // (4) The 12 cm clamp (the filter's clamp, nofoot::clampNorm3): 20 cm along x to 12; 5 cm untouched; (12, 16) to (7.2, 9.6);
                // the integral driven 200 frames by a 30 cm error at gain 0.02 (5 cm a frame cap) stops at 12 cm.
                double a[3] = {0.2, 0.0, 0.0}, b[3] = {0.03, 0.04, 0.0}, c[3] = {0.12, 0.16, 0.0}, intg[3] = {};
                nf::clampNorm3(a, nf::kAdsSightClampM);
                nf::clampNorm3(b, nf::kAdsSightClampM);
                nf::clampNorm3(c, nf::kAdsSightClampM);
                for (int k = 0; k < 200; ++k) {
                    const double e30[3] = {0.3, 0.0, 0.0};
                    double st[3] = {0.02 * e30[0], 0.02 * e30[1], 0.02 * e30[2]};
                    const double sm = std::sqrt(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]);
                    for (int i = 0; i < 3; ++i) intg[i] += st[i] * (sm > 0.05 ? 0.05 / sm : 1.0);
                    nf::clampNorm3(intg, nf::kAdsSightClampM);
                }
                check(nf::kAdsSightClampM == 0.12 && std::fabs(a[0] - 0.12) < 1e-12 && b[0] == 0.03 && b[1] == 0.04 && std::fabs(c[0] - 0.072) < 1e-12 &&
                          std::fabs(c[1] - 0.096) < 1e-12 && std::fabs(intg[0] - 0.12) < 1e-12,
                      "F63: the sight lock is clamped to 12 cm, the direction kept; the integral stops there");
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
                float pos[3] = {2.0f, 3.0f, 4.0f}, copy[3] = {2.0f, 3.0f, 4.0f};
                const float dw[3] = {0.0f, 0.1f, 0.02f};
                nf::weaponShift(pos, copy, dw);
                check(pos[0] == 2.0f && pos[1] == 3.0f + 0.1f && pos[2] == 4.0f + 0.02f && std::memcmp(pos, copy, sizeof(pos)) == 0,
                      "F54: the weapon shift moves the record position and its copy by the same d");
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

    std::printf("native_frame_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
