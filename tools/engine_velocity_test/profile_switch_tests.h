#pragma once
// engine_velocity_test: a pool family survives a live VR/flat switch (issue 186).
//
// The game creates each shader once; the profile can change after that (a live VR-to-flat or flat-to-VR switch).
// engineVelocityRememberVs/Ps used to ask the profile active AT CREATION, so a flat-only family's shaders created under VR
// (and a VR-only family's created under flat) were never kept, and the family read "not created by the game this session"
// while its vertex shader drew every frame. The cases:
//   W1  a flat-only family's shaders created under VR are remembered, run nowhere in VR, and substitute once flat
//   W2  a VR-only family's shaders created under flat are remembered, run nowhere in flat, and substitute once VR
//   W3  a family derived under one profile is derived again under the other (the second skin and the patched pixel
//       shaders are the profile's own), in both directions, and substitutes in each
// This runs before remember_cap_tests.h, which fills the tables for the rest of the process.

#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "lifecycle_tests.h"
#include "shader_tests.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/engine_velocity_families.h"

namespace edvr {
// Rig-only accessors in engine_velocity.cpp (EDVR_ENGINE_VELOCITY_RIG).
size_t engineVelocityRememberedForRig(bool pixel);
bool engineVelocityFamilyStateForRig(int f, bool* derivedFlat, bool* skin, bool* derivedSkin = nullptr);
}  // namespace edvr

namespace profile_switch_tests {
using Microsoft::WRL::ComPtr;
using lifecycle_tests::Game;

inline void run(const lifecycle_tests::Harness& h) {
    using namespace edvr;
    using engine_velocity_family::familyOfVs;
    using engine_velocity_family::kFamilies;
    const RuntimeProfile previous = g_runtimeProfile;
    g_runtimeProfile = RuntimeProfile::Vr;

    Game g(h);
    g.setup();
    g.makeSource(40, 24);
    engineVelocityConfigure(true);

    const auto vsBlob = g.compile(std::string(shader_tests::kVsCommon) + shader_tests::kVsA, "vs_5_0");
    const auto psBlob = g.compile(shader_tests::kPsA, "ps_5_0");
    struct Pair {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        uint64_t vsHash = 0, psHash = 0;
    };
    // The game creates the pair once; `remember` is the creation hook's call, under whichever profile is active then.
    auto create = [&](uint64_t vsHash, uint64_t psHash) {
        Pair p;
        p.vsHash = vsHash;
        p.psHash = psHash;
        h.check(SUCCEEDED(g.dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &p.vs)) &&
                SUCCEEDED(g.dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &p.ps)),
                "profile switch: the family's shaders create on WARP");
        return p;
    };
    auto remember = [&](const Pair& p) {
        engineVelocityRememberVs(p.vs.Get(), p.vsHash, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), false);
        engineVelocityRememberPs(p.ps.Get(), p.psHash, psBlob->GetBufferPointer(), psBlob->GetBufferSize(), false);
    };
    // One pool draw of the pair as the active profile draws it: an eye pass in VR, the on-foot source pass in flat.
    auto drawPair = [&](const Pair& p) {
        const bool flat = runtimeFlatProfile();
        g.beginFrame();
        g.writeScene(g.sceneA.Get(), g.rows[0]);
        if (flat) {
            engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get());
            g.sourcePass(0, 5);
            g.setVs(p.vs.Get(), p.vsHash);
            g.setPs(p.ps.Get(), p.psHash);
            g.sourceDraw(5);
        } else {
            g.pass(0, 0);
            g.setVs(p.vs.Get(), p.vsHash);
            g.setPs(p.ps.Get(), p.psHash);
            g.draw(5);
        }
        const bool substituted = engineVelocityDrawSubstituted();
        if (flat) {
            engineVelocityAfterFlatDraw(g.ctx);
            ID3D11RenderTargetView* rt[4] = {g.sourceRtv[0].Get(), g.sourceRtv[1].Get(), g.sourceRtv[2].Get(), g.sourceRtv[3].Get()};
            g.ctx->OMSetRenderTargets(4, rt, g.sourceDsv.Get());
        }
        g.endFrame();
        return substituted;
    };
    g.ordinaryFrame();   // the engine path is live from the first draw of the frames below

    // W1: a flat-only family, created while VR is active.
    {
        const int f = familyOfVs(0xBFE51414CC3024B4ull);
        h.check(f >= 0 && kFamilies[f].flatOnly, "W1: vs_BFE5 is the flat-only family the case needs");
        const size_t vsBefore = engineVelocityRememberedForRig(false), psBefore = engineVelocityRememberedForRig(true);
        const Pair p = create(kFamilies[f].vs, kFamilies[f].ps[0]);
        g_runtimeProfile = RuntimeProfile::Vr;
        remember(p);
        h.check(engineVelocityRememberedForRig(false) == vsBefore + 1 && engineVelocityRememberedForRig(true) == psBefore + 1,
                "W1: a flat-only family's shaders created under VR are remembered");
        h.check(!drawPair(p), "W1: ... and the family does not run under VR");
        g_runtimeProfile = RuntimeProfile::Flat;
        h.check(drawPair(p), "W1: ... but substitutes once the profile is flat");
        g_runtimeProfile = RuntimeProfile::Vr;
        h.check(!drawPair(p), "W1: ... and stops again when VR returns");
    }

    // W2: a VR-only family, created while flat is active.
    {
        const int f = familyOfVs(0x8B589D25B2A0ADDCull);
        h.check(f >= 0 && kFamilies[f].vrOnly, "W2: vs_8B58 is the VR-only family the case needs");
        const size_t vsBefore = engineVelocityRememberedForRig(false), psBefore = engineVelocityRememberedForRig(true);
        const Pair p = create(kFamilies[f].vs, kFamilies[f].ps[0]);
        g_runtimeProfile = RuntimeProfile::Flat;
        remember(p);
        h.check(engineVelocityRememberedForRig(false) == vsBefore + 1 && engineVelocityRememberedForRig(true) == psBefore + 1,
                "W2: a VR-only family's shaders created under flat are remembered");
        h.check(!drawPair(p), "W2: ... and the family does not run under flat");
        g_runtimeProfile = RuntimeProfile::Vr;
        h.check(drawPair(p), "W2: ... but substitutes once the profile is VR");
        g_runtimeProfile = RuntimeProfile::Flat;
        h.check(!drawPair(p), "W2: ... and stops again when flat returns");
    }

    // W3: a family that runs in both profiles is derived again after a switch, in both directions.
    {
        const int f = familyOfVs(0x61AE8EB05FDC18DDull);
        h.check(f >= 0 && !kFamilies[f].flatOnly && !kFamilies[f].vrOnly && kFamilies[f].skinned,
                "W3: vs_61AE is a skinned family that runs in both profiles");
        const Pair p = create(kFamilies[f].vs, kFamilies[f].ps[0]);
        g_runtimeProfile = RuntimeProfile::Flat;
        remember(p);
        bool derivedFlat = false, skin = false;
        h.check(drawPair(p), "W3: derived under flat, the family substitutes");
        h.check(engineVelocityFamilyStateForRig(f, &derivedFlat, &skin) && derivedFlat && !skin,
                "W3: ... derived under flat, without the second skin");
        g_runtimeProfile = RuntimeProfile::Vr;
        h.check(drawPair(p), "W3: after a switch to VR the family substitutes");
        h.check(engineVelocityFamilyStateForRig(f, &derivedFlat, &skin) && !derivedFlat,
                "W3: ... and was derived again under VR, not kept from flat");
        g_runtimeProfile = RuntimeProfile::Flat;
        h.check(drawPair(p), "W3: after a switch back to flat it substitutes again");
        h.check(engineVelocityFamilyStateForRig(f, &derivedFlat, &skin) && derivedFlat && !skin,
                "W3: ... derived again under flat, with no second skin carried over from VR");
    }

    // W4: a session that starts in flat arms the second skin at its first switch to VR, not at the next restart of engine motion.
    // Engine motion is configured once when it goes live; the switch reaches engineVelocityConfigure(true) again with it already
    // live, and that call is what asks (the flight of 2026-11-04 13:03 had no `second skin is live` line from the switch to the
    // F7 restart 28 s later). Every check's text starts "profile switch W4:" (tools\engine_velocity_test\mutants.py names it).
    {
        const int f = familyOfVs(0x61AE8EB05FDC18DDull);
        h.check(f >= 0 && kFamilies[f].skinned, "profile switch W4: vs_61AE is a skinned family");
        engineVelocityShutdown();   // a new session: the families are underived, the remembered shaders stay
        lifecycle_fake::g_hookArms = 0;
        lifecycle_fake::g_hookGate = false;
        g_runtimeProfile = RuntimeProfile::Flat;
        engineVelocityConfigure(true);
        h.check(!engineVelocitySkinWanted() && lifecycle_fake::g_hookArms == 0 && !lifecycle_fake::g_hookGate,
                "profile switch W4: engine motion that goes live under flat declines the second skin");

        // The game draws one skinned family's pair after the switch and before the sweep that configures engine motion again.
        const Pair p = create(kFamilies[f].vs, kFamilies[f].ps[0]);
        g_runtimeProfile = RuntimeProfile::Vr;
        remember(p);
        g.ordinaryFrame();
        bool derivedFlat = true, skin = true, derivedSkin = true;
        h.check(drawPair(p), "profile switch W4: a skinned family substitutes in VR before the second skin is armed");
        h.check(engineVelocityFamilyStateForRig(f, &derivedFlat, &skin, &derivedSkin) && !derivedFlat && !derivedSkin && !skin,
                "profile switch W4: ... derived under VR without the second skin");

        // The sweep after the switch.
        const size_t mark = lifecycle_tests::g_log.size();
        engineVelocityConfigure(true);
        h.check(engineVelocitySkinWanted() && lifecycle_fake::g_hookArms == 1 && lifecycle_fake::g_hookGate,
                "profile switch W4: the configure after the switch to VR arms the second skin and opens the hook's gate");
        h.check(lifecycle_tests::logged("skin join: the second skin is live (VR)", mark) &&
                    lifecycle_tests::logged("skin join: the session started in the flat profile and is now in VR", mark),
                "profile switch W4: ... and the log says it armed at the switch");
        h.check(drawPair(p) && engineVelocityFamilyStateForRig(f, &derivedFlat, &skin, &derivedSkin) && !derivedFlat && derivedSkin,
                "profile switch W4: a skinned family derived before the arm is derived again once it is armed");
        engineVelocityConfigure(true);
        engineVelocityConfigure(true);
        h.check(lifecycle_fake::g_hookArms == 1, "profile switch W4: the second skin is armed once, not at every configure");

        // A session that starts in VR is unchanged: armed at the start, and nothing at a later configure arms it again.
        engineVelocityShutdown();
        lifecycle_fake::g_hookArms = 0;
        const size_t startMark = lifecycle_tests::g_log.size();
        engineVelocityConfigure(true);
        engineVelocityConfigure(true);
        h.check(engineVelocitySkinWanted() && lifecycle_fake::g_hookArms == 1 &&
                    !lifecycle_tests::logged("skin join: the session started in the flat profile", startMark),
                "profile switch W4: a session that starts in VR arms the second skin once, at the start");

        // A flat start that engine motion leaves (F7, a mode change) before any switch: the next start under VR arms once, like any VR start.
        engineVelocityShutdown();
        lifecycle_fake::g_hookArms = 0;
        g_runtimeProfile = RuntimeProfile::Flat;
        engineVelocityConfigure(true);
        engineVelocityShutdown();
        g_runtimeProfile = RuntimeProfile::Vr;
        engineVelocityConfigure(true);
        engineVelocityConfigure(true);
        h.check(lifecycle_fake::g_hookArms == 1, "profile switch W4: a flat start that engine motion left before the switch: the next start under VR arms once");
    }

    engineVelocityShutdown();
    g_runtimeProfile = previous;
    std::printf("  profile switch: a flat-only family's shaders created under VR, and a VR-only family's created under flat, "
                "are remembered and substitute in their own profile only; a family derived under one profile is derived again "
                "under the other; a session that started flat arms the second skin at its first switch to VR\n");
}
}  // namespace profile_switch_tests
