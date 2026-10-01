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
// save (lifecycle_tests.h), and the primary-pool cache's lifetime -- game
// buffers held only while the feature is live, released on stand-down, a full
// cache logged once, a mid-session activation nominating the pool from its
// binding (pin_tests.h), and the pool copier's observer running on job threads
// without the engine mutex (copier_tests.h). build.bat links
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
#include "allocation_counter.h"

#include "shader_tests.h"
#include "overlay_depth_gpu_tests.h"
#include "emit_tests.h"
#include "math_tests.h"
#include "consumer_tests.h"
#include "capture_tests.h"
#include "primary_copy_tests.h"
#include "panel_tests.h"
#include "corpus_identity.h"
#include "actual_vs_link_test.h"
#include "lifecycle_tests.h"
#include "flat_lazy_tests.h"
#include "pin_tests.h"
#include "copier_tests.h"
#include "unkeyed_tests.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/d3d11/engine_velocity_families.h"   // kSelfMarking
#include "../../third_party/dxbc_hash/DxilHash.cpp"

using Microsoft::WRL::ComPtr;

// The linked draw half uses the same internal binding guard as production;
// the rig does not link the flat readback module that normally defines it.
namespace edvr { thread_local bool g_flatComputeInternal = false; }

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
// --partial: the dump under test may lack some of the required pairs (each
// install's edvr_logs\shaders holds what that install drew, and the pairs below
// were measured across several). Strict stays the default: without the flag a
// missing file fails the run. With it, an absent pair is listed and skipped, and
// every pair the dump does hold must still pass whole -- so a candidate can be
// tried on the dump that has it without a full corpus on this machine.
bool g_partial = false;
unsigned g_absent = 0;
bool g_lastAbsent = false;
// One real pair through the whole harness, `ok` deciding what a failure does:
// `check` for a keyed pair (the run fails), softCheck for a candidate (the
// first failure is kept and printed). Each result is taken before its check,
// so a reason written by the call is the one printed.
bool onePair(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& root, const Pair& p,
             void (*ok)(bool, const char*)) {
    const auto vs = readFile(root + L"\\shaders\\" + p.vs + L".dxbc");
    const auto ps = readFile(root + L"\\shaders\\" + p.ps + L".dxbc");
    g_lastAbsent = false;
    if (g_partial && (vs.empty() || ps.empty())) {
        ++g_absent; g_lastAbsent = true;
        std::printf("  corpus: %ls + %ls: absent from this dump (--partial), skipped\n", p.vs, p.ps);
        return false;
    }
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
    if (std::wcscmp(p.vs, L"vs_BBE58E40FE88EC80") == 0 &&
        std::wcscmp(p.ps, L"ps_DB3E8D20CF53FBC0") == 0) {
        std::vector<BYTE> guarded;
        const bool patched = edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, guarded, why, true);
        ok(patched, why.c_str());
        if (!patched) return false;
        ComPtr<ID3D11PixelShader> guardObject;
        const bool created = SUCCEEDED(device->CreatePixelShader(guarded.data(), guarded.size(), nullptr, &guardObject));
        ok(created, "real DB3E guarded overlay shader created on WARP");
        if (!created) return false;
    }
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
    if (std::wcscmp(p.ps, L"ps_03B17F89B31C4788") == 0)
        ok(r.covered > 0 && r.uncoveredChecked > 0 && r.uncoveredBad == 0,
           "station alpha pair: the real corpus drives survivors and discard, with discarded MRT6 untouched");
    const bool actual = std::wcscmp(p.vs, L"vs_DE545DC8EE4FBB87") != 0 ||
                        std::wcscmp(p.ps, L"ps_91F8937EDA723663") != 0 ||
                        actual_vs_link_test::run(device, context, vs, ps, pps, ok);
    return r.driven && actual;
}
void corpus(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& root) {
    const Pair pairs[] = {
        // Captured flat edge shell: its front-face input occupies VS position's
        // numeric register. Exercise the actual VS linkage and both windings.
        {L"vs_DE545DC8EE4FBB87", L"ps_91F8937EDA723663", false},
        // Epic flat cockpit shell: exactly this pair, with the same G-buffer
        // and depth outputs and an additional record slot at MRT6.
        {L"vs_BFE51414CC3024B4", L"ps_DB79AE788E049DFD", false},
        {L"vs_EB5234DB6ADB491D", L"ps_CB9F297EFF264251", false}, {L"vs_EB5234DB6ADB491D", L"ps_9ABF60B4B51F2C1F", false},
        {L"vs_EB5234DB6ADB491D", L"ps_3434972DB5336AA4", false}, {L"vs_5B4D8E894EEDA8B4", L"ps_4375B72964F386CD", true},
        // 054658: current joined station records drawn by previously stock PSs.
        {L"vs_EB5234DB6ADB491D", L"ps_DC603C35BBE74B31", false},
        {L"vs_EB5234DB6ADB491D", L"ps_63B1524A9F805A4C", false},
        {L"vs_DE545DC8EE4FBB87", L"ps_03B17F89B31C4788", false},
        {L"vs_BBE58E40FE88EC80", L"ps_DB3E8D20CF53FBC0", false}, {L"vs_DE545DC8EE4FBB87", L"ps_E46E3E4832B2FDB0", false},
        {L"vs_AACFDCF2FB9AD809", L"ps_CF534B32F491561A", false}, {L"vs_66DE2CADB1F4AE6B", L"ps_864F1F949851B8DE", false},
        {L"vs_66DE2CADB1F4AE6B", L"ps_BBDE4E71FB78528A", false},   // the 15:46 session's stock station draws
        {L"vs_61AE8EB05FDC18DD", L"ps_FC43E42710010343", false},
        // Eye run 055427 (2026-09-26, parked close to a coriolis port): the
        // family's stock pixel shader at close range -- part of the hull sat
        // on the camera term while it drew.
        {L"vs_61AE8EB05FDC18DD", L"ps_4504BC268E109C31", false},
        // The station (eye run 143416): the two stock station pairs keyed.
        {L"vs_DE545DC8EE4FBB87", L"ps_CB429E043DBB2506", false}, {L"vs_61AE8EB05FDC18DD", L"ps_451A82D4DD1BA254", false},
        // Flight 6 (153446, the shader dump armed at a station): the station's
        // biggest pool shader and its only pixel shader; vs_889A with both of
        // its pixel shaders (ps_B46E at the station, ps_EBA9 elsewhere).
        {L"vs_436193B352A2897E", L"ps_16940F576006BE65", false},
        {L"vs_889A5279E68F0672", L"ps_B46E52A1E0B2F39C", false}, {L"vs_889A5279E68F0672", L"ps_EBA95E15B0A66102", false},
        // Eye run 055427 (2026-09-26, parked close to a coriolis port): the
        // station's close-range stock pixel shaders -- the hull's pixel path
        // while the keyed pairs of the same families draw nothing. Dumped by
        // the 2026-09-27 glare_shader_dump flight, harnessed, keyed.
        {L"vs_436193B352A2897E", L"ps_51EE1F922FD220B0", false},
        {L"vs_889A5279E68F0672", L"ps_D31DCAFA7C05CB47", false},
        // Epic 20260929, the Krait's main-menu F10 capture: the hull plating's stock
        // pixel shader, drawn unkeyed until now (a flat-only companion, flatPs). Its
        // 5 draws left 22.5% of the frame's slots stale. Passes whole on WARP: patched,
        // reflected, created, o0..o3 and depth bit-identical to the stock pair.
        {L"vs_66DE2CADB1F4AE6B", L"ps_235567BE2840B3ED", false},
    };
    for (const auto& p : pairs) onePair(device, context, root, p, &check);
    // Candidates: pairs seen drawing stock that are not keyed. Each is tried
    // and its first failure printed, never failing the run -- a pair joins the
    // keyed list above (and kFamilies) only once it passes here whole.
    // Flight 6 (153446): another DE54 stock pixel shader. It is not keyed
    // while its live owner/coverage is assessed.
    const Pair candidates[] = {
        {L"vs_DE545DC8EE4FBB87", L"ps_A6070F9DD1CFB601", false},
        // Historical double-patch candidate: BA58 is EDVR's generated 4375
        // substitution, proven by production-patcher hashes in 162120.
        {L"vs_5B4D8E894EEDA8B4", L"ps_BA58469C3D6120A7", true},
        // Left for later (2026-09-29, the Krait's main-menu hangar): the flat census
        // names them when they draw. CAD1F585 is EDHM-patched (reads t120), so its
        // hash belongs to a mod's build; 7311054A takes an SV_Position input.
        {L"vs_AACFDCF2FB9AD809", L"ps_CAD1F585EDDC5641", false},
        {L"vs_BBE58E40FE88EC80", L"ps_7311054AB3AAE1DC", false},
    };
    for (const auto& p : candidates) {
        g_softWhy.clear();
        const bool passed = onePair(device, context, root, p, &softCheck) && g_softWhy.empty();
        if (g_lastAbsent) continue;   // onePair already said it was absent
        std::printf("  candidate: %ls + %ls: %s%s\n", p.vs, p.ps, passed ? "PASSES the harness" : "not keyable -- ",
                    passed ? "" : g_softWhy.c_str());
    }
    // Historical kSelfMarking compatibility pairs are EDVR-generated
    // substitutions, not native game producers (162120 hash proof). Preserve
    // their checks: a second patch is refused and the already-patched shader
    // writes the exact marker. S4 still drives the compatibility branch.
    std::vector<BYTE> vs4361;
    for (const auto& p : edvr::engine_velocity_family::kSelfMarking) {
        wchar_t wvs[24], wps[24];
        std::swprintf(wvs, 24, L"vs_%016llX", static_cast<unsigned long long>(p.vs));
        std::swprintf(wps, 24, L"ps_%016llX", static_cast<unsigned long long>(p.ps));
        const auto vsb = readFile(root + L"\\shaders\\" + wvs + L".dxbc");
        const auto psb = readFile(root + L"\\shaders\\" + wps + L".dxbc");
        if (g_partial && (vsb.empty() || psb.empty())) {
            ++g_absent;
            std::printf("  corpus: %ls + %ls: self-marking pair absent from this dump (--partial), skipped\n", wvs, wps);
            continue;
        }
        check(!vsb.empty() && !psb.empty(), "self-marking pair's dxbc present");
        if (vsb.empty() || psb.empty()) continue;
        if (p.vs == 0x436193B352A2897Eull) vs4361 = vsb;
        edvr::EngineVelocityInputs in;
        std::string why;
        check(edvr::engineVelocityDeriveInputs(vsb.data(), vsb.size(), in, why), why.c_str());
        std::vector<BYTE> patched;
        const bool refused = !edvr::engineVelocityPatchPs(psb.data(), psb.size(), in, patched, why) &&
                             why.find("target 6") != std::string::npos;
        check(refused, "self-marking pair: the patcher refuses it -- its target 6 is occupied by design");
        char name[96];
        std::snprintf(name, sizeof(name), "%ls + %ls", wvs, wps);
        const corpus_identity::Result r = corpus_identity::selfMarked(device, context, psb, in, &check, name);
        check(r.driven, "self-marking pair driven, not skipped");
        check(r.slotChecked > 0 && r.slotBad == 0,
              "self-marking compatibility: the already-patched shader writes exact slot/depth at MRT6");
    }
    if (!vs4361.empty()) lifecycle_tests::selfMarkingCase({device, context, &check}, vs4361);
}} // namespace

int wmain(int argc, wchar_t** argv) {
    bool selfTest = false;
    std::wstring corpusRoot;
    std::wstring realLinkRoot;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--self-test" || a == L"--dry-run") selfTest = true;
        else if (a == L"--verbose") lifecycle_tests::g_verbose = true;
        else if (a == L"--corpus" && i + 1 < argc) corpusRoot = argv[++i];
        else if (a == L"--partial") g_partial = true;
        else if (a == L"--real-link" && i + 1 < argc) realLinkRoot = argv[++i];
        else {
            std::fprintf(stderr, "usage: engine_velocity_test --self-test | --dry-run [--corpus <edvr_logs dir> [--partial]] [--real-link <edvr_logs dir>]\n");
            return 2;
        }
    }
    if (!selfTest && corpusRoot.empty() && realLinkRoot.empty()) {
        std::fprintf(stderr, "usage: engine_velocity_test --self-test | --dry-run [--corpus <edvr_logs dir> [--partial]] [--real-link <edvr_logs dir>]\n");
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
    constexpr uint64_t edgeVs = 0xDE545DC8EE4FBB87ull;
    constexpr uint64_t edgePs = 0x91F8937EDA723663ull;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    check(!edvr::engineVelocityPoolFamilyPair(edgeVs, edgePs) &&
          edvr::engineVelocityPoolFamilyPair(edgeVs, 0xE46E3E4832B2FDB0ull),
          "front-face pair stays excluded from VR while existing DE54 pair remains eligible");
    check(!edvr::engineVelocityPoolFamilyVs(shellVs) &&
          !edvr::engineVelocityPoolFamilyPair(shellVs, shellPs), "cockpit shell is excluded from VR motion producer");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(edvr::engineVelocityPoolFamilyPair(edgeVs, edgePs) &&
          !edvr::engineVelocityPoolFamilyPair(edgeVs, 0xA6070F9DD1CFB601ull),
          "flat admits only the draw-qualified front-face pair");
    check(edvr::engineVelocityPoolFamilyVs(shellVs) &&
          edvr::engineVelocityPoolFamilyPair(shellVs, shellPs), "exact cockpit shell pair is eligible in flat");
    check(!edvr::engineVelocityPoolFamilyPair(shellVs, 0xCB9F297EFF264251ull),
          "cockpit shell does not admit another pixel shader");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    check(!edvr::engineVelocityPoolFamilyPair(edgeVs, edgePs),
          "legacy VR also excludes the flat-only front-face pair");
    constexpr uint64_t stationVs = 0xEB5234DB6ADB491Dull;
    constexpr uint64_t stationPs = 0xDC603C35BBE74B31ull;
    constexpr uint64_t detailPs = 0x63B1524A9F805A4Cull;
    constexpr uint64_t alphaPs = 0x03B17F89B31C4788ull;
    for (const auto profile : {edvr::RuntimeProfile::Vr, edvr::RuntimeProfile::LegacyVr, edvr::RuntimeProfile::Flat}) {
        edvr::g_runtimeProfile = profile;
        check(edvr::engineVelocityPoolFamilyPair(stationVs, stationPs) &&
              edvr::engineVelocityPoolFamilyPair(stationVs, detailPs) &&
              edvr::engineVelocityPoolFamilyPair(edgeVs, alphaPs),
              "Coriolis: the three exact qualified station pairs are eligible in each runtime profile");
        check(!edvr::engineVelocityPoolFamilyPair(stationVs, alphaPs) &&
              !edvr::engineVelocityPoolFamilyPair(edgeVs, stationPs) &&
              !edvr::engineVelocityPoolFamilyPair(edgeVs, detailPs),
              "Coriolis: a qualified pixel shader stays excluded from the other family");
        check(!edvr::engineVelocityPoolFamilyPair(stationVs, 0x123456789abcdef0ull) &&
              !edvr::engineVelocityPoolFamilyPair(0x123456789abcdef0ull, stationPs) &&
              !edvr::engineVelocityPoolFamilyPair(0x61AE8EB05FDC18DDull, 0x06D24ACAB0DC11B3ull),
              "Coriolis: unknown pairs and the camera-following unrelated rig remain unkeyed");
    }
    // The Krait hull pair (2026-09-29): keyed for flat through the family's flat-only
    // companion slot, never for VR. Its sibling stays keyed everywhere, and the pixel
    // shaders left for later (the EDHM-patched CAD1F585, BBE58E40's 7311054A) and a
    // third 66DE2CAD pixel shader stay unkeyed in every profile.
    constexpr uint64_t hullVs = 0x66DE2CADB1F4AE6Bull, hullPs = 0x235567BE2840B3EDull;
    constexpr uint64_t hullSiblingPs = 0x864F1F949851B8DEull;
    for (const auto profile : {edvr::RuntimeProfile::Vr, edvr::RuntimeProfile::LegacyVr}) {
        edvr::g_runtimeProfile = profile;
        check(!edvr::engineVelocityPoolFamilyPair(hullVs, hullPs) &&
              edvr::engineVelocityPoolFamilyPair(hullVs, hullSiblingPs),
              "VR: the hull pair stays unkeyed while its keyed sibling is untouched");
    }
    for (const auto profile : {edvr::RuntimeProfile::Vr, edvr::RuntimeProfile::LegacyVr, edvr::RuntimeProfile::Flat}) {
        edvr::g_runtimeProfile = profile;
        check(!edvr::engineVelocityPoolFamilyPair(0xAACFDCF2FB9AD809ull, 0xCAD1F585EDDC5641ull) &&
              !edvr::engineVelocityPoolFamilyPair(0xBBE58E40FE88EC80ull, 0x7311054AB3AAE1DCull) &&
              !edvr::engineVelocityPoolFamilyPair(hullVs, 0x818212B5F404C002ull),
              "the pixel shaders left for later stay unkeyed in every profile");
    }
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(edvr::engineVelocityPoolFamilyPair(hullVs, hullPs) &&
          edvr::engineVelocityPoolFamilyPair(hullVs, hullSiblingPs),
          "flat keys the hull pair beside its sibling");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    shader_tests::run({device.Get(), context.Get(), &check});
    overlay_depth_gpu_tests::run(device.Get(), context.Get(), &check);
    emit_tests::run({&check});
    math_tests::run({device.Get(), context.Get(), &check});
    consumer_tests::run({device.Get(), context.Get(), &check});
    for (const auto& primary : emit_tests::g_joinedPrimary)
        consumer_tests::run({device.Get(), context.Get(), &check},primary.data(),101);
    capture_tests::run(device.Get(),context.Get(),&check);
    primary_copy_tests::run(device.Get(),context.Get(),&check);
    panel_tests::run({device.Get(), context.Get(), &check});
    lifecycle_tests::run({device.Get(), context.Get(), &check});
    flat_lazy_tests::run({device.Get(), context.Get(), &check});
    pin_tests::run({device.Get(), context.Get(), &check});
    copier_tests::run({device.Get(), context.Get(), &check});
    unkeyed_tests::run({&check});
    if (!realLinkRoot.empty()) {
        const Pair edge{L"vs_DE545DC8EE4FBB87", L"ps_91F8937EDA723663", false};
        check(onePair(device.Get(), context.Get(), realLinkRoot, edge, &check),
              "captured DE54/PS91 real VS link and G-buffer equivalence");
    }
    if (!corpusRoot.empty()) corpus(device.Get(), context.Get(), corpusRoot);
    std::printf("engine_velocity_test: %u checks passed%s%s%s.\n", g_checks,
                corpusRoot.empty() ? "" : (g_absent ? " including the real shader corpus (PARTIAL: some pairs absent from the dump)"
                                                    : " including the real shader corpus"),
                realLinkRoot.empty() ? "" : " including the captured real VS link",
                g_absent ? (" [" + std::to_string(g_absent) + " absent]").c_str() : "");
    return 0;
}
