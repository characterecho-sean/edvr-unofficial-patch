#include "../../src/common/native_frame.h"
#include "../../src/common/comfort_fade.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/config.h"
#include "../../src/common/native_render_settings.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/journal_watch.h"
#include "../../src/d3d11/pose_latch_patch.h"
#include "cull_cycle_cases.h"
#include "mono_camera_cases.h"
#include "pose_latch_cases.h"

#include <d3d11.h>
#include <fcntl.h>
#include <io.h>
#include <wrl/client.h>

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
// production guard.cpp's crash-channel dependency (proxy.cpp), which the code hook the mono camera hooks use pulls in; as the other rigs stub it
void breadcrumb(const char*) {}
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
    if (!std::wcscmp(argv[1], L"--print-cull-fixture")) {
        const std::string text = cull_cases::fixtureLog();
        _setmode(_fileno(stdout), _O_BINARY);                 // LF only: tools\cull_cycle_fixture.log is this, byte for byte
        std::fwrite(text.data(), 1, text.size(), stdout);
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

    cull_cases::runCullCycleCases(check);   // the probe cycle's own logic: schedule, windows, discard, pairs, statuses, the log tool's fixture
    pose_cases::runPoseLatchCases(check);   // advanced.cull_pose: the modes, the RVA filter, the target time, the engine patch, its gate and its atomic store
    mono_cases::runMonoCameraCases(check);  // the mono camera hooks: the detour's decision, the gate, the lazy install, the observer, the real hooks

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
    edvr::announceCullGuardState(0, 1.0f, 1.0f);
    edvr::requestSubmitHold(0);
    // Published after acquire; doing so also exercises the device identity
    // check without making setup depend on a pre-existing channel mapping.

    edvr::Config::get().set("fix.cull_guard", "PeRcEnT");
    edvr::Config::get().set("fix.cull_guard_percent", "75");
    edvr::Config::get().set("fix.cull_guard_fraction_h", "-1");
    edvr::Config::get().set("fix.cull_guard_fraction_v", "nan");
    edvr::Config::get().set("fix.cull_guard_headsets",
                            "94x99, 120x130junk, 95X84, 10x20, 95x84 ");
    edvr::Config::get().set("fix.transition_flash", "1");
    edvr::Config::get().set("advanced.transition_flash_resubmit", "0");
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
              table.beginFrame && table.setCullState && table.latchSubmit &&
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
    check(firstOutput.cullMode == 2 && firstOutput.cullPercent == 50.0f &&
              firstOutput.cullHorizontalFraction == 0.0f &&
              firstOutput.cullVerticalFraction == 1.0f,
          "cull mode percent and bounds");
    check(firstOutput.cullSignatureCount == 2 &&
              firstOutput.cullSignatures[0][0] == 94 &&
              firstOutput.cullSignatures[0][1] == 99 &&
              firstOutput.cullSignatures[1][0] == 95 &&
              firstOutput.cullSignatures[1][1] == 84,
          "cull signatures reject trailing junk and bad dimensions");
    check(firstOutput.sceneReady && firstOutput.transitionEnabled &&
              !firstOutput.resubmitEnabled,
          "scene and transition outputs");
    check(firstOutput.version == EDVR_NATIVE_FRAME_VERSION_8 &&
              firstOutput.size == sizeof(firstOutput),
          "version 7 answered in kind");
    check(firstOutput.cullChannel == 2,
          "the cull channel parses case-insensitively");
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

    // Cull state is a CPU-only host announcement with strict argument checks.
    check(table.setCullState(table.context, 1, 1.25f, 1.5f) == S_OK,
          "valid cull state announced");
    check(edvr::decodeCullGuardState(edvr::cullGuardStatePacked()).stage == 1,
          "cull state reaches shared channel");
    check(table.setCullState(table.context, 0, 0.0f, 0.0f) == S_OK &&
              edvr::cullGuardStatePacked() == 0,
          "cull off accepts ignored factors");
    check(table.setCullState(table.context, 3, 1.0f, 1.0f) == E_INVALIDARG &&
              table.setCullState(table.context, 2,
                                 std::numeric_limits<float>::quiet_NaN(), 1.0f) ==
                  E_INVALIDARG,
          "invalid cull state rejected");

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

    // Explicit camera holds survive transition_flash being turned off, and a
    // prepared frame that never reaches Submit does not consume the hold.
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
          "explicit hold survives transition setting");
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
    check(guarded.output.cullMode == 2 && guarded.output.sceneReady &&
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
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_5 + sizeof(uint32_t) + sizeof(float) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_6,
          "version 6 adds exactly the handedness flag and the simulated cant");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_6 + sizeof(uint32_t) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_7,
          "version 7 adds exactly the cull probe");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_7 + sizeof(uint32_t) ==
              sizeof(EdvrNativeFrameOutput),
          "version 8 adds exactly the pose-time switch");

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
    defaultOutput.cullChannel = 0xA5A5A5A5u;
    EdvrNativeFrameInput defaultFrame = input(41, 7, 11);
    check(table.beginFrame(table.context, &defaultFrame, &defaultOutput) == S_OK &&
              defaultOutput.version == EDVR_NATIVE_FRAME_VERSION_3 &&
              defaultOutput.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 &&
              defaultOutput.deferredPacing == 0 &&
              defaultOutput.cullChannel == 0xA5A5A5A5u,
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

    // An unknown channel value is both, the guard's historical behaviour.
    edvr::Config::get().set("advanced.cull_guard_channel", "junk");
    EdvrNativeFrameOutput unknownChannel{sizeof(unknownChannel),
                                         EDVR_NATIVE_FRAME_VERSION_8};
    EdvrNativeFrameInput unknownFrame = input(41, 7, 17);
    check(table.beginFrame(table.context, &unknownFrame, &unknownChannel) ==
              S_OK && unknownChannel.cullChannel == 0,
          "an unknown channel value is both");

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

    // ---- Explorer Cam's comfort fade (comfort_fade.h; EdvrNativeFrameOutput::fadeAlpha, version 5) --------------------------------------------------------------
    {
        uint64_t seq = 40;
        auto ask5 = [&](float* fade) {
            // A real version 5 caller: its own shape, one short of the whole struct.
            EdvrNativeFrameOutput o{EDVR_NATIVE_FRAME_OUTPUT_SIZE_5, EDVR_NATIVE_FRAME_VERSION_5};
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
        v3.cullChannel = 0xA5A5A5A5u;
        v3.fadeAlpha = -123.0f;
        EdvrNativeFrameInput v3Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v3Frame, &v3) == S_OK && v3.cullChannel == 0xA5A5A5A5u && v3.fadeAlpha == -123.0f, "...and a version 3 caller likewise");
        EdvrNativeFrameOutput fullAsV4{sizeof(fullAsV4), EDVR_NATIVE_FRAME_VERSION_4};
        EdvrNativeFrameInput fullMismatchFrame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &fullMismatchFrame, &fullAsV4) == E_INVALIDARG, "A FULL-SIZE STRUCT CLAIMING VERSION 4 is refused (the size contradicts the version)");
        EdvrNativeFrameOutput v4AsV5{EDVR_NATIVE_FRAME_OUTPUT_SIZE_4, EDVR_NATIVE_FRAME_VERSION_5};
        EdvrNativeFrameInput mismatchFrame2 = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &mismatchFrame2, &v4AsV5) == E_INVALIDARG, "...and a version 4 sized struct claiming version 5 is refused: it is never half-answered");
        edvr::comfort::clear();
    }

    // ---- the canted-display arc's two temporary test keys (docs\canted-projection.md; EdvrNativeFrameOutput::cantedEyeFix and ::simulateCantDeg, version 6) ------
    {
        uint64_t seq = 60;
        auto ask6 = [&](uint32_t* fix, float* degrees) {
            // A real version 6 caller: its own shape, one short of the whole struct.
            EdvrNativeFrameOutput o{EDVR_NATIVE_FRAME_OUTPUT_SIZE_6, EDVR_NATIVE_FRAME_VERSION_6};
            o.cantedEyeFix = 0xA5A5A5A5u;   // sentinels the answer must overwrite
            o.simulateCantDeg = -77.0f;
            EdvrNativeFrameInput f = input(41, 7, ++seq);
            const HRESULT r = table.beginFrame(table.context, &f, &o);
            *fix = o.cantedEyeFix;
            *degrees = o.simulateCantDeg;
            return r;
        };
        uint32_t fix = 7;
        float degrees = -1.0f;
        edvr::Config::get().set("advanced.canted_eye_fix", "");
        edvr::Config::get().set("advanced.simulate_cant", "");
        check(ask6(&fix, &degrees) == S_OK && fix == 1u && degrees == 0.0f,
              "NEITHER KEY SET: the handedness correction is on and no cant is simulated (and both sentinels are overwritten)");
        edvr::Config::get().set("advanced.canted_eye_fix", "off");
        check(ask6(&fix, &degrees) == S_OK && fix == 0u, "canted_eye_fix = off reads 0");
        edvr::Config::get().set("advanced.canted_eye_fix", "0");
        check(ask6(&fix, &degrees) == S_OK && fix == 0u, "...0 reads 0");
        edvr::Config::get().set("advanced.canted_eye_fix", "On");
        check(ask6(&fix, &degrees) == S_OK && fix == 1u, "...On reads 1 (case does not matter)");
        edvr::Config::get().set("advanced.canted_eye_fix", "junk");
        check(ask6(&fix, &degrees) == S_OK && fix == 1u, "...a value that is no yes/no reads the default, on");
        edvr::Config::get().set("advanced.simulate_cant", "10");
        check(ask6(&fix, &degrees) == S_OK && degrees == 10.0f, "simulate_cant = 10 reads 10 degrees");
        edvr::Config::get().set("advanced.simulate_cant", "7.5");
        check(ask6(&fix, &degrees) == S_OK && degrees == 7.5f, "...7.5 reads 7.5");
        edvr::Config::get().set("advanced.simulate_cant", "40");
        check(ask6(&fix, &degrees) == S_OK && degrees == 15.0f, "...40 clamps to 15");
        edvr::Config::get().set("advanced.simulate_cant", "15");
        check(ask6(&fix, &degrees) == S_OK && degrees == 15.0f, "...15 is the top of the range");
        edvr::Config::get().set("advanced.simulate_cant", "-3");
        check(ask6(&fix, &degrees) == S_OK && degrees == 0.0f, "...a negative cant clamps to 0 (off)");
        for (const char* notANumber : {"nan", "inf", "-inf", "2,5", "10deg", "abc"}) {
            edvr::Config::get().set("advanced.simulate_cant", notANumber);
            check(ask6(&fix, &degrees) == S_OK && degrees == 0.0f,
                  "...a value that is not a whole finite number reads 0 (off): nan, inf, 2,5, 10deg, abc");
        }
        edvr::Config::get().set("advanced.simulate_cant", "10");
        // ---- the older shapes never learn of them ------------------------------------------------------------------------------------------------------------
        EdvrNativeFrameOutput v5{EDVR_NATIVE_FRAME_OUTPUT_SIZE_5, EDVR_NATIVE_FRAME_VERSION_5};
        v5.cantedEyeFix = 0xA5A5A5A5u;
        v5.simulateCantDeg = -123.0f;
        EdvrNativeFrameInput v5Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v5Frame, &v5) == S_OK && v5.version == EDVR_NATIVE_FRAME_VERSION_5 && v5.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_5 &&
                  v5.cantedEyeFix == 0xA5A5A5A5u && v5.simulateCantDeg == -123.0f,
              "A VERSION 5 CALLER is answered in its own shape and the two slots past it are untouched (a runtime from before the test keys cannot be canted)");
        EdvrNativeFrameOutput v6{EDVR_NATIVE_FRAME_OUTPUT_SIZE_6, EDVR_NATIVE_FRAME_VERSION_6};
        v6.cullProbe = 0xA5A5A5A5u;
        EdvrNativeFrameInput v6Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v6Frame, &v6) == S_OK && v6.version == EDVR_NATIVE_FRAME_VERSION_6 && v6.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_6 &&
                  v6.cullProbe == 0xA5A5A5A5u && v6.simulateCantDeg == 10.0f,
              "A VERSION 6 CALLER is answered in its own shape, the canted-display keys included, and the cull probe slot past it is untouched");
        EdvrNativeFrameOutput fullAsV5{sizeof(fullAsV5), EDVR_NATIVE_FRAME_VERSION_5};
        EdvrNativeFrameInput fullAsV5Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &fullAsV5Frame, &fullAsV5) == E_INVALIDARG, "A FULL-SIZE STRUCT CLAIMING VERSION 5 is refused (the size contradicts the version)");
        EdvrNativeFrameOutput fullAsV6{sizeof(fullAsV6), EDVR_NATIVE_FRAME_VERSION_6};
        EdvrNativeFrameInput fullAsV6Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &fullAsV6Frame, &fullAsV6) == E_INVALIDARG, "A FULL-SIZE STRUCT CLAIMING VERSION 6 is refused (the size contradicts the version)");
        EdvrNativeFrameOutput v5AsV6{EDVR_NATIVE_FRAME_OUTPUT_SIZE_5, EDVR_NATIVE_FRAME_VERSION_6};
        EdvrNativeFrameInput v5AsV6Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v5AsV6Frame, &v5AsV6) == E_INVALIDARG, "...and a version 5 sized struct claiming version 6 is refused: never half-answered");
        edvr::Config::get().set("advanced.cull_probe", "all");
        edvr::Config::get().set("advanced.cull_pose", "display_direct");
        EdvrNativeFrameOutput v7{EDVR_NATIVE_FRAME_OUTPUT_SIZE_7, EDVR_NATIVE_FRAME_VERSION_7};
        v7.cullProbe = 0xA5A5A5A5u;
        EdvrNativeFrameInput v7Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v7Frame, &v7) == S_OK && v7.version == EDVR_NATIVE_FRAME_VERSION_7 && v7.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_7 &&
                  v7.cullProbe == 1u && edvr::cullpose::driver().latch().kind() == edvr::cullpose::LatchBypass::Kind::Off,
              "A VERSION 7 CALLER is answered in its own shape, the cull probe included, and is never given a pose mode or the engine patch, whatever advanced.cull_pose says");
        EdvrNativeFrameOutput fullAsV7{sizeof(fullAsV7), EDVR_NATIVE_FRAME_VERSION_7};
        EdvrNativeFrameInput fullAsV7Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &fullAsV7Frame, &fullAsV7) == E_INVALIDARG, "A FULL-SIZE STRUCT CLAIMING VERSION 7 is refused (the size contradicts the version)");
        EdvrNativeFrameOutput v7AsV8{EDVR_NATIVE_FRAME_OUTPUT_SIZE_7, EDVR_NATIVE_FRAME_VERSION_8};
        EdvrNativeFrameInput v7AsV8Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v7AsV8Frame, &v7AsV8) == E_INVALIDARG, "...and a version 7 sized struct claiming version 8 is refused: never half-answered");
        edvr::Config::get().set("advanced.cull_probe", "");
        edvr::Config::get().set("advanced.cull_pose", "");
        // ---- advanced.cull_probe (the terrain-culling arc, docs\terrain-culling.md; EdvrNativeFrameOutput::cullProbe, version 7) ------------------------------------
        auto ask7 = [&](uint32_t* probe) {
            EdvrNativeFrameOutput o{sizeof(o), EDVR_NATIVE_FRAME_VERSION_8};
            o.cullProbe = 0xA5A5A5A5u;   // a sentinel the answer must overwrite
            EdvrNativeFrameInput f = input(41, 7, ++seq);
            const HRESULT r = table.beginFrame(table.context, &f, &o);
            *probe = o.cullProbe;
            return r;
        };
        uint32_t probe = 7;
        edvr::Config::get().set("advanced.cull_probe", "");
        check(ask7(&probe) == S_OK && probe == 0u, "cull_probe unset reads 0 (off), and the sentinel is overwritten");
        struct ProbeCase { const char* text; uint32_t code; };
        const ProbeCase probeCases[] = {{"off", 0}, {"all", 1}, {"camera", 2}, {"ui", 3}, {"sky", 4}, {"sizes", 5}, {"other", 6},
                                        {"ALL", 1}, {"Camera", 2}, {"UI", 3}, {"Sizes", 5}, {"cameras", 0}, {"6", 0}, {"junk", 0},
                                        {"mono", 0}, {"MONO", 0}, {"7", 0}};
        bool probesParse = true;
        for (const ProbeCase& c : probeCases) {
            edvr::Config::get().set("advanced.cull_probe", c.text);
            probesParse = probesParse && ask7(&probe) == S_OK && probe == c.code;
        }
        check(probesParse, "cull_probe parses off, all, camera, ui, sky, sizes, other (any case) to 0..6; a near miss, a number and junk read 0 (off), and mono (this half's own, a seventh group) is never sent to the runtime: it reads 0 too");
        // ---- advanced.cull_probe = cycle (cull_cycle.h): this half drives the groups and sends the active one in the same field ----
        {
            using namespace edvr::cullcycle;
            g_buildOverride.store(1);
            edvr::Config::get().set("fix.cull_guard", "off");
            edvr::Config::get().set("advanced.cull_probe", "cycle");
            uint64_t t = 5000000;
            auto askAt = [&](uint64_t us) { g_clockOverrideUs.store(us); return ask7(&probe); };
            probe = 99;
            check(askAt(t) == S_OK && probe == 0u && g_driver.status() == Status::Running && counting(),
                  "cull_probe = cycle on build 332841 with no guard: the cycle runs, counts, and the first window is off (group 0)");
            bool zeroUntil = true;
            for (int i = 1; i < 200; ++i) {
                t += 10000;
                noteTerrainDraw(0, 2304u, 1u);
                zeroUntil = zeroUntil && askAt(t) == S_OK && probe == 0u;
            }
            check(zeroUntil && g_driver.cycle().windowsClosed() == 0, "...it tells the runtime off for the 199 frames of the first 1.99 s");
            t += 10000;
            check(askAt(t) == S_OK && probe == 1u && g_driver.cycle().windowsClosed() == 1 && g_draws[0].load() == 0u,
                  "...and group 1 (all) from the frame 2.0 s in, the boundary having taken the render thread''s counts");
            for (int i = 0; i < 200; ++i) { t += 10000; askAt(t); }
            check(probe == 0u && g_driver.cycle().windowsClosed() == 2, "...then off again for the next window: the schedule alternates off with each group");
            // A caller whose struct cannot carry the field gets no cycle.
            EdvrNativeFrameOutput v6c{EDVR_NATIVE_FRAME_OUTPUT_SIZE_6, EDVR_NATIVE_FRAME_VERSION_6};
            v6c.cullProbe = 0xA5A5A5A5u;
            EdvrNativeFrameInput v6cFrame = input(41, 7, ++seq);
            g_clockOverrideUs.store(t + 10000);
            check(table.beginFrame(table.context, &v6cFrame, &v6c) == S_OK && v6c.cullProbe == 0xA5A5A5A5u && g_driver.status() == Status::Idle && !counting(),
                  "a version 6 caller, which has no cull probe slot, stops the cycle rather than running it unseen");
            // The gates, as the runtime''s probe has them, reported in the log by the cycle itself.
            g_buildOverride.store(0);
            t += 20000;
            check(askAt(t) == S_OK && probe == 0u && g_driver.status() == Status::StoodDownBuild && !counting(),
                  "on another build the cycle stands down: group 0, nothing counted, status StoodDownBuild");
            g_buildOverride.store(1);
            edvr::Config::get().set("fix.cull_guard", "symmetric");
            t += 10000;
            check(askAt(t) == S_OK && probe == 0u && g_driver.status() == Status::IgnoredGuard && !counting(), "with a cull guard configured it is ignored: group 0, status IgnoredGuard (a guard wins over a wrong build too)");
            g_buildOverride.store(0);
            t += 10000;
            check(askAt(t) == S_OK && g_driver.status() == Status::IgnoredGuard, "...");
            g_buildOverride.store(1);
            edvr::Config::get().set("fix.cull_guard", "off");
            edvr::Config::get().set("advanced.cull_probe", "CYCLE");
            t += 10000;
            check(askAt(t) == S_OK && g_driver.status() == Status::Running && probe == 0u && g_driver.cycle().windowsClosed() == 0,
                  "the guard off again: the key is case-insensitive and the cycle starts over from window 1");
            edvr::Config::get().set("advanced.cull_probe", "cycles");
            t += 10000;
            check(askAt(t) == S_OK && probe == 0u && g_driver.status() == Status::Idle && !counting(), "a near miss (cycles) is not the cycle: it parses as off");
            edvr::Config::get().set("advanced.cull_probe", "camera");
            t += 10000;
            check(askAt(t) == S_OK && probe == 2u && g_driver.status() == Status::Idle, "and a fixed group (camera) is sent as itself, with no cycle");
            // measure: the same counting with no lies, labelled by the cull guard's stage as the runtime last told this half (setCullState).
            edvr::Config::get().set("advanced.cull_probe", "measure");
            edvr::Config::get().set("fix.cull_guard", "symmetric");
            check(table.setCullState(table.context, 2, 1.1f, 1.0f) == S_OK, "(the runtime tells the guard's stage: live)");
            g_buildOverride.store(0);   // another build: measure has no lie to stand down
            t = 90000000;
            check(askAt(t) == S_OK && probe == 0u && g_driver.status() == Status::Measuring && counting() && g_driver.cycle().measuring() && g_driver.cycle().stage() == 3u,
                  "cull_probe = measure with a cull guard configured and live, on another build: it measures (stage live), tells group 0, and does not stand down");
            bool silent = true;
            for (int i = 0; i < 200; ++i) { t += 10000; silent = silent && askAt(t) == S_OK && probe == 0u; }
            check(silent && g_driver.cycle().windowsClosed() == 1, "...a window of 2.0 s closes while the guard runs, and nothing was ever told to the runtime");
            check(table.setCullState(table.context, 1, 1.1f, 1.0f) == S_OK, "(the guard goes back to adopting)");
            t += 10000;
            check(askAt(t) == S_OK && g_driver.cycle().stage() == 2u && g_driver.cycle().windowsClosed() == 1, "the stage becomes adopting and the window in progress is dropped");
            table.setCullState(table.context, 0, 1.0f, 1.0f);
            t += 10000;
            check(askAt(t) == S_OK && g_driver.cycle().stage() == 1u, "stage 0 with the guard configured is waiting");
            edvr::Config::get().set("fix.cull_guard", "off");
            t += 10000;
            check(askAt(t) == S_OK && g_driver.cycle().stage() == 0u && g_driver.status() == Status::Measuring, "with the guard off it is off");
            edvr::Config::get().set("advanced.cull_probe", "MEASURE");
            t += 10000;
            check(askAt(t) == S_OK && g_driver.status() == Status::Measuring, "the key is case-insensitive");
            edvr::Config::get().set("advanced.cull_probe", "measures");
            t += 10000;
            check(askAt(t) == S_OK && probe == 0u && g_driver.status() == Status::Idle && !counting(), "a near miss (measures) is not measure");
            // The hooks go in lazily, on the first frame cycle, measure or mono is set, and not before.
            edvr::monocam::uninstallForTest();
            g_buildOverride.store(1);
            edvr::Config::get().set("fix.cull_guard", "off");
            edvr::Config::get().set("advanced.cull_probe", "off");
            t += 10000;
            check(askAt(t) == S_OK && !edvr::monocam::g_lazy.attempted(), "with cull_probe off the mono camera hooks are not touched (not even read)");
            edvr::Config::get().set("advanced.cull_probe", "measure");
            t += 10000;
            check(askAt(t) == S_OK && edvr::monocam::g_lazy.attempted() && !edvr::monocam::g_lie.load(), "cull_probe = measure installs the observe side on its first frame, and tells no lie");
            edvr::monocam::uninstallForTest();
            edvr::Config::get().set("advanced.cull_probe", "cycle");
            t += 10000;
            check(askAt(t) == S_OK && edvr::monocam::g_lazy.attempted(), "cull_probe = cycle installs them on its first frame");
            edvr::monocam::uninstallForTest();
            edvr::Config::get().set("advanced.cull_probe", "camera");
            t += 10000;
            check(askAt(t) == S_OK && !edvr::monocam::g_lazy.attempted(), "a fixed runtime group (camera) does not touch the mono camera hooks");
            edvr::monocam::uninstallForTest();            // mono: the steady form, and the cycle's seventh group. The runtime is never told 7; this half switches the mono camera's lie.
            g_buildOverride.store(1);
            edvr::Config::get().set("fix.cull_guard", "off");
            edvr::Config::get().set("advanced.cull_probe", "mono");
            t += 10000;
            probe = 99;
            check(askAt(t) == S_OK && probe == 0u && g_steadyMono.status() == Status::Running && edvr::monocam::g_lie.load() && g_driver.status() == Status::Idle && !counting(),
                  "cull_probe = mono on build 332841 with no guard: the runtime is told off, the mono lie is on, and nothing is counted");
            check(edvr::monocam::g_lazy.attempted() && !edvr::monocam::g_lazy.live() && (edvr::monocam::g_lazy.reason() != nullptr && std::strcmp(edvr::monocam::g_lazy.reason(), "not build 332841") == 0) && !g_monoHookLive.load(),
                  "...the first such frame tried the hooks (this rig is not Elite, so they are inert, and no ', mono reads' field would be written)");
            edvr::Config::get().set("fix.cull_guard", "symmetric");
            t += 10000;
            check(askAt(t) == S_OK && probe == 0u && g_steadyMono.status() == Status::IgnoredGuard && !edvr::monocam::g_lie.load(), "mono with a cull guard configured: the lie is off, status IgnoredGuard");
            edvr::Config::get().set("fix.cull_guard", "off");
            g_buildOverride.store(0);
            t += 10000;
            check(askAt(t) == S_OK && g_steadyMono.status() == Status::StoodDownBuild && !edvr::monocam::g_lie.load(), "mono on another build: the lie is off, status StoodDownBuild");
            g_buildOverride.store(1);
            edvr::Config::get().set("advanced.cull_probe", "MONO");
            t += 10000;
            check(askAt(t) == S_OK && g_steadyMono.status() == Status::Running && edvr::monocam::g_lie.load(), "the key is case-insensitive");
            edvr::Config::get().set("advanced.cull_probe", "monos");
            t += 10000;
            check(askAt(t) == S_OK && probe == 0u && g_steadyMono.status() == Status::Idle && !edvr::monocam::g_lie.load(), "a near miss (monos) is not mono: the lie goes off");
            edvr::Config::get().set("advanced.cull_probe", "all");
            t += 10000;
            check(askAt(t) == S_OK && probe == 1u && !edvr::monocam::g_lie.load(), "`all` keeps its meaning: the runtime's group 1, and no mono lie");
            // The cycle: the seventh group is a mono window. The runtime is told 0 in it and the lie flag is on exactly there.
            edvr::Config::get().set("advanced.cull_probe", "cycle");
            t += 10000;
            bool sequenceOk = true;
            unsigned monoFrames = 0, allFrames = 0, otherFrames = 0;
            for (unsigned i = 0; i < 14 * 200 + 400; ++i) {
                t += 10000;
                const bool answered = askAt(t) == S_OK;
                const uint32_t scheduled = kSchedule[g_driver.cycle().slot()];
                sequenceOk = sequenceOk && answered && probe == (scheduled == kMonoGroup ? 0u : scheduled) &&
                             edvr::monocam::g_lie.load() == (scheduled == kMonoGroup);
                monoFrames += scheduled == kMonoGroup;
                allFrames += scheduled == 1u;
                otherFrames += scheduled == 6u;
            }
            check(sequenceOk && g_driver.cycle().cyclesDone() >= 1 && monoFrames > 150 && allFrames > 150 && otherFrames > 150,
                  "the cycle through beginFrame: a mono window tells the runtime group 0 and switches the lie on, every other window tells the runtime its own group with the lie off");
            edvr::Config::get().set("advanced.cull_probe", "off");
            t += 10000;
            check(askAt(t) == S_OK && probe == 0u && !edvr::monocam::g_lie.load() && g_driver.status() == Status::Idle && g_steadyMono.status() == Status::Idle,
                  "the key back at off: the lie flag is down and both statuses are idle");
            edvr::monocam::uninstallForTest();
            // advanced.cull_pose through beginFrame: the code the runtime is told, and what the engine patch does on an executable that is not Elite.
            {
                namespace cpose = edvr::cullpose;
                cpose::resetForTest();
                g_buildOverride.store(1);
                uint32_t told = 0;
                const auto askPose = [&](const char* text) {
                    edvr::Config::get().set("advanced.cull_pose", text);
                    EdvrNativeFrameOutput o{sizeof(o), EDVR_NATIVE_FRAME_VERSION_8};
                    o.cullPose = 0xA5A5A5A5u;   // a sentinel the answer must overwrite
                    EdvrNativeFrameInput f = input(41, 7, ++seq);
                    const HRESULT r = table.beginFrame(table.context, &f, &o);
                    told = o.cullPose;
                    return r;
                };
                struct PoseCase { const char* text; uint32_t code; };
                const PoseCase poseCases[] = {{"off", 0}, {"display", 1}, {"next", 2}, {"display_direct", 3}, {"next_direct", 4}, {"DISPLAY", 1}, {"Next_Direct", 4},
                                              {"displays", 0}, {"2", 0}, {"junk", 0}, {"", 0}};
                bool codes = true;
                for (const PoseCase& c : poseCases) codes = codes && askPose(c.text) == S_OK && told == c.code;
                check(codes, "advanced.cull_pose reaches a version 8 caller as 0 off, 1 display, 2 next, 3 display_direct, 4 next_direct (any case); a near miss, a number and junk are 0, and the sentinel is overwritten");
                check(askPose("display_direct") == S_OK && told == 3u && cpose::driver().latch().kind() == cpose::LatchBypass::Kind::Refused &&
                          std::strcmp(cpose::driver().latch().why(), "not build 332841") == 0,
                      "display_direct on an executable that is not Elite (this rig): the code is still told, and the engine patch is refused with the reason, nothing written");
                check(askPose("next") == S_OK && told == 2u && cpose::driver().latch().kind() == cpose::LatchBypass::Kind::Off, "leaving _direct clears the refusal");
                g_buildOverride.store(0);
                check(askPose("display") == S_OK && told == 0u && askPose("next_direct") == S_OK && told == 0u, "on another build the time modes stand down: the runtime is told 0, display and next_direct alike");
                check(askPose("off") == S_OK && told == 0u && cpose::driver().latch().kind() == cpose::LatchBypass::Kind::Off, "...and the key back at off is off");
                g_buildOverride.store(1);
                edvr::Config::get().set("advanced.cull_pose", "");
                cpose::resetForTest();
            }
            table.setCullState(table.context, 0, 1.0f, 1.0f);            g_clockOverrideUs.store(0);
            g_buildOverride.store(-1);
            edvr::Config::get().set("fix.cull_guard", "PeRcEnT");
        }        edvr::Config::get().set("advanced.cull_probe", "");
        edvr::Config::get().set("advanced.canted_eye_fix", "");
        edvr::Config::get().set("advanced.simulate_cant", "");
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
    check(edvr::kFrameFlagVersion == 37, "frame_flag layout is v37");
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
