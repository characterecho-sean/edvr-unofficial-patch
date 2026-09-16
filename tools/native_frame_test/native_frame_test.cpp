#include "../../src/common/native_frame.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/config.h"
#include "../../src/common/system_d3d11.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

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
    edvr::announceCullGuardState(0, 1.0f, 1.0f);
    edvr::requestSubmitHold(0);
    edvr::setExternalCameraOnFoot(false);
    // Published after acquire; doing so also exercises the device identity
    // check without making setup depend on a pre-existing channel mapping.

    edvr::Config::get().set("openvr.head_offset_right", "12");
    edvr::Config::get().set("openvr.head_offset_up", "-11");
    edvr::Config::get().set("openvr.head_offset_forward", "2.5");
    edvr::Config::get().set("openvr.head_yaw_degrees", "450");
    edvr::Config::get().set("openvr.head_offset_game_poses", "0");
    edvr::Config::get().set("openvr.head_offset_external_only", "0");
    edvr::Config::get().set("openvr.head_offset_max_stale_frames", "1");
    edvr::Config::get().set("fix.cull_guard", "PeRcEnT");
    edvr::Config::get().set("fix.cull_guard_percent", "75");
    edvr::Config::get().set("fix.cull_guard_fraction_h", "-1");
    edvr::Config::get().set("fix.cull_guard_fraction_v", "nan");
    edvr::Config::get().set("fix.cull_guard_headsets",
                            "94x99, 120x130junk, 95X84, 10x20, 95x84 ");
    edvr::Config::get().set("fix.transition_flash", "1");
    edvr::Config::get().set("advanced.transition_flash_resubmit", "0");

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
                                      EDVR_NATIVE_FRAME_VERSION_1};
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
    check(firstOutput.headOffset[0] == 10.0f &&
              firstOutput.headOffset[1] == -10.0f &&
              firstOutput.headOffset[2] == -2.5f,
          "offsets clamp and negate forward once");
    check(std::fabs(firstOutput.yawRadians - 1.57079632679f) < 0.0001f,
          "yaw wraps to radians");
    check(firstOutput.offsetEnabled && !firstOutput.offsetGamePoses,
          "offset flags reflect config");
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
    // publish a new pose and closes the external-only offset gate.
    edvr::Config::get().set("openvr.head_offset_external_only", "1");
    edvr::setExternalCameraOnFoot(true);
    edvr::markGlitchFrame();
    EdvrNativeFrameInput lost = input(41, 7, 2, false);
    lost.physicalHead[0] = std::numeric_limits<float>::quiet_NaN();
    EdvrNativeFrameOutput lostOutput{sizeof(lostOutput),
                                     EDVR_NATIVE_FRAME_VERSION_1};
    check(table.beginFrame(table.context, &lost, &lostOutput) == S_OK &&
              !lostOutput.offsetEnabled,
          "lost pose succeeds but disables offset gate");
    EdvrNativeFrameDecision lostDecision{
        sizeof(lostDecision), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 2, &lostDecision) == S_OK &&
              !lostDecision.withhold,
          "begin clears a mark from the previous frame");
    EdvrNativeFrameInput bad = input(41, 7, 3);
    bad.physicalHead[0] = 2.0f;
    EdvrNativeFrameOutput badOutput{sizeof(badOutput),
                                    EDVR_NATIVE_FRAME_VERSION_1};
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

    check(table.close(table.context) == S_OK && !edvr::glitchConsumerPresent(),
          "close retires announced consumer");
    check(table.close(table.context) == S_FALSE, "close is idempotent");
    check(table.beginFrame(table.context, &next, &lostOutput) == E_INVALIDARG,
          "closed context rejects callbacks");

    std::printf("native_frame_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
