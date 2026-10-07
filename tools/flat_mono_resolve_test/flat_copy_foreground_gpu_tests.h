// The first-person contract on the COPY route (FlatMonoResolveFrame::hdr = false), on WARP (design section 104).
//
// The runtime's weapon support asks the foreground contract at the game's final copy for the frames the copy route judges (a render below
// the output with DLSS or FSR, where the HDR route does not treat the frame): a frame the model selected mixed-camera asks for the qualified
// first-person map, the resolver takes it or refuses the frame, exactly as it does for the HDR route's frames (flat_sdk_foreground_gpu_tests.h).
// Until then the resolver's clause demanded the HDR route of every frame that carried the contract, so a copy-route frame that asked could
// only be refused, and this file is what pins its removal. The fixture is steady_depth's Rig: the copy route's 16 x 16 render, an RGBA8 game
// frame (the tone pass's output, not H), a still camera, a first-person mark at texel (8, 8) at the pixel's own depth, and a qualified map
// that gives that texel a motion, a depth and w = 1. The render is below the output (16 -> 32 and 16 -> 24) for DLSS and FSR.
//   what is pinned:   a qualified map on a copy-route frame reaches the DLSS and FSR stubs, and they are handed the real foreground motion and
//                     the map's canonical depth, at the render size in and the output size out, history kept;
//   what refuses:     a stale map (not this frame's), an unqualified one, a required contract with no map, and a map of the wrong format are
//                     each refused by the resolver before the backend, by name; a mask on the copy route still refuses (the copy route never
//                     takes one: the map takes its place), with a map and without one;
//   what is untouched: EDVR's own TAA on the copy route never asks the contract and is accepted with the fields in any state.
// What fails if the change is reverted (the foreground clause demanding the HDR route again), checked on a private copy of the resolver:
//   every "a copy-route frame that carries a qualified map is resolved" check (the reset frame and the continuing one, DLSS and FSR, both
//   sizes), and the "reaches the backend" checks after them; the refusals and the mask checks keep passing, which is what they are for.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace copyfg {
constexpr UINT R = steadygpu::W;   // the render size, 16

inline const char* msg(const char* fmt, ...) {   // one buffer: every message is consumed by check() before the next is made
    static char buffer[512];
    va_list args; va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

inline void scenario(steadygpu::Rig& r, edvr::FlatMonoResolveMode mode, const char* name, UINT D) {
    using namespace edvr;
    ID3D11Device* device = r.device;
    const UINT center = 8 * R + 8;
    std::vector<float> map(size_t(R) * R * 4, 0.f);
    map[4 * center] = 1.75f; map[4 * center + 1] = -.5f; map[4 * center + 2] = .0025f; map[4 * center + 3] = 1;
    auto motion = texture(device, R, R, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, map.data(), R * 16);
    auto motionView = view(device, motion.Get());
    auto wrong = texture(device, R, R, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    auto wrongView = view(device, wrong.Get());
    std::vector<unsigned char> coverage(size_t(R) * R, 255);
    auto mask = texture(device, R, R, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, coverage.data(), R);
    auto maskView = view(device, mask.Get());

    flatMonoResolveReset();
    r.start(mode);
    r.f.outputWidth = r.f.outputHeight = D;
    r.put(8, 8, -3, .01f);   // a first-person mark at the pixel's own raw depth (the scene's .01): the map names this texel
    // The frame as the copy route's treatment hands it over: the contract required, the map qualified, no mask.
    const auto ask = [&](bool required, ID3D11ShaderResourceView* mapView, bool qualified, UINT frameOffset, const char* label, bool wantOk) {
        r.f.foregroundRequired = required; r.f.foregroundMotion = mapView; r.f.foregroundQualified = qualified;
        r.f.foregroundFrame = r.f.frame - frameOffset;
        return r.run(label, wantOk);
    };
    // The label, copied before it is used (the argument may itself be a msg() buffer).
    const auto say = [&](const std::string& text) -> const char* {
        static std::string kept;
        kept = std::string("copy-route foreground (") + name + ", output " + std::to_string(D) + "): " + text;
        return kept.c_str();
    };
    const auto refusedBy = [&](const char* part, const char* label) {
        check(r.lastWhy && std::strstr(r.lastWhy, part), say(std::string(label) + ": refused by name (" + part + "), not by another clause"));
    };

    r.f.reset = true;
    ask(true, motionView.Get(), true, 0, say("the reset frame of a copy-route frame that carries a qualified map is resolved, not refused"), true);
    check(r.lastWhy == nullptr, say("the reset frame's resolve reports no refusal"));
    r.f.reset = false;
    const int before = backendCalls;
    ask(true, motionView.Get(), true, 0, say("the continuing frame of a copy-route frame that carries a qualified map is resolved"), true);
    check(backendCalls == before + 1 && !backendReset && observedMotion == 1.75f && observedMotionY == -.5f && observedDepth == .0025f && observedReject == 0,
          say("the backend is handed the real foreground motion and the map's canonical depth, history kept, on a frame that is not H"));
    check(observedInW == R && observedInH == R && observedOutW == D && observedOutH == D && !observedHdr,
          say("the backend runs on the game's RGBA8 frame at the render size in and the output size out: the copy route's sizes, not the HDR route's"));

    // What refuses: before the backend, by name.
    const auto refuses = [&](bool required, ID3D11ShaderResourceView* mapView, bool qualified, UINT frameOffset, ID3D11ShaderResourceView* maskV,
                             const char* part, const char* label) {
        const int calls = backendCalls;
        r.f.untrustedCameraCoverage = maskV;
        ask(required, mapView, qualified, frameOffset, say(label), false);
        refusedBy(part, label);
        check(backendCalls == calls, say(msg("%s: the backend was never called", label)));
        r.f.untrustedCameraCoverage = nullptr;
    };
    refuses(true, motionView.Get(), true, 1, nullptr, "foreground-contract-unqualified", "a stale map (the frame before this one's) refuses");
    refuses(true, motionView.Get(), false, 0, nullptr, "foreground-contract-unqualified", "a map that did not qualify refuses");
    refuses(true, nullptr, false, 0, nullptr, "foreground-contract-unqualified", "a required contract with no map refuses");
    refuses(true, wrongView.Get(), true, 0, nullptr, "foreground-view-mismatch", "a map of the wrong format refuses");
    // A mask on the copy route still refuses: the copy route never takes one, with the map beside it or without.
    refuses(true, motionView.Get(), true, 0, maskView.Get(), "untrusted-coverage-requires-native-HDR-TAA", "a mask beside a qualified map refuses");
    refuses(false, nullptr, false, 0, maskView.Get(), "untrusted-coverage-requires-native-HDR-TAA", "a mask with no map refuses");

    // The refusals poisoned nothing: a reset frame seeds again and the continuing one is resolved with the map.
    r.f.reset = true;
    ask(true, motionView.Get(), true, 0, say("after the refusals a reset frame is resolved again"), true);
    r.f.reset = false;
    const int again = backendCalls;
    ask(true, motionView.Get(), true, 0, say("and the next one continues with the map"), true);
    check(backendCalls == again + 1 && !backendReset && observedMotion == 1.75f && observedDepth == .0025f,
          say("after the refusals the continuing frame is handed the foreground motion again"));
    flatMonoResolveReset();
}

// EDVR's own TAA on the copy route never asks the contract, whatever the fields say.
inline void taaScenario(steadygpu::Rig& r, UINT D) {
    using namespace edvr;
    flatMonoResolveReset();
    r.start(FlatMonoResolveMode::Taa);
    r.f.outputWidth = r.f.outputHeight = D;
    r.f.foregroundRequired = true; r.f.foregroundMotion = nullptr; r.f.foregroundQualified = false; r.f.foregroundFrame = 0;
    const int calls = backendCalls;
    r.run(msg("copy-route foreground (TAA, output %u): a frame whose contract fields name a required map nobody made is not refused: TAA never asks", D), true);
    check(backendCalls == calls, msg("copy-route foreground (TAA, output %u): EDVR's own TAA calls no SDK", D));
    flatMonoResolveReset();
}
}  // namespace copyfg

inline void copyForegroundGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace edvr;
    steadygpu::Rig rig(device, context);
    copyfg::scenario(rig, FlatMonoResolveMode::Dlss, "DLSS", 32);
    copyfg::scenario(rig, FlatMonoResolveMode::Dlss, "DLSS", 24);
    copyfg::scenario(rig, FlatMonoResolveMode::Fsr, "FSR", 32);
    copyfg::scenario(rig, FlatMonoResolveMode::Fsr, "FSR", 24);
    copyfg::taaScenario(rig, 32);
    flatMonoResolveReset();
    context->ClearState();
}
