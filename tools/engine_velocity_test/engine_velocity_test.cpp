// engine_velocity_test: the build gate for engine-record velocity
// (part of fix.temporal_aa, phase 1; docs/kinematic-motion-injection-2026-09-19.md,
// 2026-09-23 "Phase 1 built").
//
//   --self-test   every check below, on WARP; writes nothing
//   --dry-run     the same run (the rig never writes a file), for the gate's
//                 --dry-run convention
//   --corpus DIR  also the REAL pool-family shaders from an edvr_logs dump
//                 (DIR\shaders\*.dxbc): each pair derives, patches, reflects
//                 and creates on WARP, and draws SV_Target0..3 and depth
//                 bit-identically to the stock pair (corpus_identity.h). Local
//                 only: the game's shaders are not in the repository.
//   --verbose     also print the log lines the linked engine_velocity.cpp wrote
//
// What it covers: the DXBC patcher end to end and the slot target's blend
// states (shader_tests.h), the emit bracket's history rules against a fake
// engine laid out as build 332841 (emit_tests.h), the compose's arithmetic
// from the shipped HLSL text (math_tests.h), the production `mv` entry with
// engine inputs bound -- joined exact, masked with no history, corrupt/stale/
// cleared declined (consumer_tests.h), the on-foot path through the
// production screen shader -- a record carried to its previous source UV and
// through the panel, the declined kinds on the camera term, the counts and
// the motion_source encoding (panel_tests.h) -- engine_velocity.cpp's draw
// half linked in and driven through the flight's and the review's cases and
// the on-foot source's slot target, plus the temporal pass's compute-state
// save (lifecycle_tests.h). build.bat links
// src\d3d11\engine_velocity.cpp with EDVR_ENGINE_VELOCITY_RIG and the binding
// shadow external; lifecycle_tests.h supplies the stubs.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "shader_tests.h"
#include "emit_tests.h"
#include "math_tests.h"
#include "consumer_tests.h"
#include "panel_tests.h"
#include "corpus_identity.h"
#include "lifecycle_tests.h"
#include "../../src/common/runtime_profile.h"
#include "../../third_party/dxbc_hash/DxilHash.cpp"

using Microsoft::WRL::ComPtr;

namespace {
unsigned g_checks = 0;
void check(bool value, const char* why) {
    ++g_checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}

std::vector<BYTE> readFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0 || size > 1024 * 1024) return {};
    std::vector<BYTE> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file ? bytes : std::vector<BYTE>{};
}

// The real families, from a dump: every measured (VS, PS) pair. Each derives,
// patches, reflects and creates, and then (corpus_identity.h) draws stock and
// patched over the same inputs: SV_Target0..3 and depth must match byte for
// byte -- the substitution must not change the game's own G-buffer.
struct Pair { const wchar_t* vs; const wchar_t* ps; bool vsPatch; };
// A candidate's first failure, kept whole (its text can come from a local).
std::string g_softWhy;
void softCheck(bool value, const char* why) {
    if (!value && g_softWhy.empty()) g_softWhy = why && *why ? why : "(no reason given)";
}
// One real pair through the whole harness, `ok` deciding what a failure does:
// `check` for a keyed pair (the run fails), softCheck for a candidate (the
// first failure is kept and printed). Each result is taken before its check,
// so a reason written by the call is the one printed.
bool onePair(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& root, const Pair& p,
             void (*ok)(bool, const char*)) {
    const auto vs = readFile(root + L"\\shaders\\" + p.vs + L".dxbc");
    const auto ps = readFile(root + L"\\shaders\\" + p.ps + L".dxbc");
    ok(!vs.empty() && !ps.empty(), "real corpus pair present");
    if (vs.empty() || ps.empty()) return false;
    edvr::EngineVelocityInputs in;
    std::string why;
    const bool derived = edvr::engineVelocityDeriveInputs(vs.data(), vs.size(), in, why);
    ok(derived, why.c_str());
    if (!derived) return false;
    ok(in.slotFromVsPatch == p.vsPatch, "real family: VS patch needed exactly for the UV-only family");
    std::vector<BYTE> pvs = vs, pps;
    if (in.slotFromVsPatch) {
        const bool vsPatched = edvr::engineVelocityPatchVs(vs.data(), vs.size(), in, pvs, why);
        ok(vsPatched, why.c_str());
        if (!vsPatched) return false;
    }
    const bool psPatched = edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, pps, why);
    ok(psPatched, why.c_str());
    if (!psPatched) return false;
    ComPtr<ID3D11VertexShader> v;
    ComPtr<ID3D11PixelShader> f;
    const bool vsMade = SUCCEEDED(device->CreateVertexShader(pvs.data(), pvs.size(), nullptr, &v));
    const bool psMade = SUCCEEDED(device->CreatePixelShader(pps.data(), pps.size(), nullptr, &f));
    ok(vsMade, "real patched VS created on WARP");
    ok(psMade, "real patched PS created on WARP");
    if (!vsMade || !psMade) return false;
    ComPtr<ID3D11ShaderReflection> reflect;
    const bool reflects = SUCCEEDED(D3DReflect(pps.data(), pps.size(), IID_PPV_ARGS(&reflect)));
    ok(reflects, "real patched PS reflects");
    if (!reflects) return false;
    std::printf("  corpus: %ls + %ls: slot v%u.%c, SV_Position v%u%s -- patched, reflected, created\n", p.vs, p.ps,
                in.identityRegister, "xyzw"[in.identityComponent & 3], in.positionRegister,
                in.slotFromVsPatch ? ", VS exports EDVRPOOLSLOT" : "");
    char name[80];
    std::snprintf(name, sizeof(name), "%ls + %ls", p.vs, p.ps);
    const corpus_identity::Result r = corpus_identity::compare(device, context, ps, pps, in, ok, name);
    ok(r.driven, "real corpus pair driven for the o0..o3 identity check (not skipped)");
    return r.driven;
}
void corpus(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& root) {
    const Pair pairs[] = {
        // Epic flat cockpit shell: exactly this pair, with the same G-buffer
        // and depth outputs and an additional record slot at MRT6.
        {L"vs_BFE51414CC3024B4", L"ps_DB79AE788E049DFD", false},
        {L"vs_EB5234DB6ADB491D", L"ps_CB9F297EFF264251", false}, {L"vs_EB5234DB6ADB491D", L"ps_9ABF60B4B51F2C1F", false},
        {L"vs_EB5234DB6ADB491D", L"ps_3434972DB5336AA4", false}, {L"vs_5B4D8E894EEDA8B4", L"ps_4375B72964F386CD", true},
        {L"vs_BBE58E40FE88EC80", L"ps_DB3E8D20CF53FBC0", false}, {L"vs_DE545DC8EE4FBB87", L"ps_E46E3E4832B2FDB0", false},
        {L"vs_AACFDCF2FB9AD809", L"ps_CF534B32F491561A", false}, {L"vs_66DE2CADB1F4AE6B", L"ps_864F1F949851B8DE", false},
        {L"vs_61AE8EB05FDC18DD", L"ps_FC43E42710010343", false},
        // The station (eye run 143416): the two stock station pairs keyed.
        {L"vs_DE545DC8EE4FBB87", L"ps_CB429E043DBB2506", false}, {L"vs_61AE8EB05FDC18DD", L"ps_451A82D4DD1BA254", false},
        // Flight 6 (153446, the shader dump armed at a station): the station's
        // biggest pool shader and its only pixel shader; vs_889A with both of
        // its pixel shaders (ps_B46E at the station, ps_EBA9 elsewhere).
        {L"vs_436193B352A2897E", L"ps_16940F576006BE65", false},
        {L"vs_889A5279E68F0672", L"ps_B46E52A1E0B2F39C", false}, {L"vs_889A5279E68F0672", L"ps_EBA95E15B0A66102", false},
    };
    for (const auto& p : pairs) onePair(device, context, root, p, &check);
    // Candidates: pairs seen drawing stock that are not keyed. Each is tried
    // and its first failure printed, never failing the run -- a pair joins the
    // keyed list above (and kFamilies) only once it passes here whole.
    // Flight 6 (153446): vs_DE54's last two stock pixel shaders, which the
    // patcher refuses (the family's SV_Position input register holds another
    // semantic in them).
    const Pair candidates[] = {
        {L"vs_DE545DC8EE4FBB87", L"ps_91F8937EDA723663", false}, {L"vs_DE545DC8EE4FBB87", L"ps_A6070F9DD1CFB601", false},
    };
    for (const auto& p : candidates) {
        g_softWhy.clear();
        const bool passed = onePair(device, context, root, p, &softCheck) && g_softWhy.empty();
        std::printf("  candidate: %ls + %ls: %s%s\n", p.vs, p.ps, passed ? "PASSES the harness" : "not keyable -- ",
                    passed ? "" : g_softWhy.c_str());
    }
}} // namespace

int wmain(int argc, wchar_t** argv) {
    bool selfTest = false;
    std::wstring corpusRoot;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--self-test" || a == L"--dry-run") selfTest = true;
        else if (a == L"--verbose") lifecycle_tests::g_verbose = true;
        else if (a == L"--corpus" && i + 1 < argc) corpusRoot = argv[++i];
        else {
            std::fprintf(stderr, "usage: engine_velocity_test --self-test | --dry-run [--corpus <edvr_logs dir>]\n");
            return 2;
        }
    }
    if (!selfTest && corpusRoot.empty()) {
        std::fprintf(stderr, "usage: engine_velocity_test --self-test | --dry-run [--corpus <edvr_logs dir>]\n");
        return 2;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                      &level, &context)), "D3D11CreateDevice WARP");
    check(level >= D3D_FEATURE_LEVEL_11_0, "feature level 11");
    constexpr uint64_t shellVs = 0xBFE51414CC3024B4ull;
    constexpr uint64_t shellPs = 0xDB79AE788E049DFDull;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    check(!edvr::engineVelocityPoolFamilyVs(shellVs) &&
          !edvr::engineVelocityPoolFamilyPair(shellVs, shellPs), "cockpit shell is excluded from VR motion producer");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(edvr::engineVelocityPoolFamilyVs(shellVs) &&
          edvr::engineVelocityPoolFamilyPair(shellVs, shellPs), "exact cockpit shell pair is eligible in flat");
    check(!edvr::engineVelocityPoolFamilyPair(shellVs, 0xCB9F297EFF264251ull),
          "cockpit shell does not admit another pixel shader");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    shader_tests::run({device.Get(), context.Get(), &check});
    emit_tests::run({&check});
    math_tests::run({device.Get(), context.Get(), &check});
    consumer_tests::run({device.Get(), context.Get(), &check});
    panel_tests::run({device.Get(), context.Get(), &check});
    lifecycle_tests::run({device.Get(), context.Get(), &check});
    if (!corpusRoot.empty()) corpus(device.Get(), context.Get(), corpusRoot);
    std::printf("engine_velocity_test: %u checks passed%s.\n", g_checks, corpusRoot.empty() ? "" : " including the real shader corpus");
    return 0;
}
