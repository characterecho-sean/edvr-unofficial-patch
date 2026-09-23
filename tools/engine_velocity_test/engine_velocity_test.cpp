// engine_velocity_test: the build gate for engine-record velocity
// (fix.engine_motion=on, phase 1; docs/kinematic-motion-injection-2026-09-19.md,
// 2026-09-23 "Phase 1 built").
//
//   --self-test   every check below, on WARP; writes nothing
//   --dry-run     the same run (the rig never writes a file), for the gate's
//                 --dry-run convention
//   --corpus DIR  also the REAL pool-family shaders from an edvr_logs dump
//                 (DIR\shaders\*.dxbc): each pair derives, patches, reflects
//                 and creates on WARP. Local only: the game's shaders are not
//                 in the repository.
//
// What it covers: the DXBC patcher end to end (shader_tests.h), the emit
// bracket against a fake engine laid out as build 332841 (emit_tests.h), and
// the compose's arithmetic from the shipped HLSL text (math_tests.h).
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

// The real families, from a dump: every measured (VS, PS) pair.
void corpus(ID3D11Device* device, const std::wstring& root) {
    struct Pair { const wchar_t* vs; const wchar_t* ps; bool vsPatch; };
    const Pair pairs[] = {
        {L"vs_EB5234DB6ADB491D", L"ps_CB9F297EFF264251", false}, {L"vs_EB5234DB6ADB491D", L"ps_9ABF60B4B51F2C1F", false},
        {L"vs_EB5234DB6ADB491D", L"ps_3434972DB5336AA4", false}, {L"vs_5B4D8E894EEDA8B4", L"ps_4375B72964F386CD", true},
        {L"vs_BBE58E40FE88EC80", L"ps_DB3E8D20CF53FBC0", false}, {L"vs_DE545DC8EE4FBB87", L"ps_E46E3E4832B2FDB0", false},
        {L"vs_AACFDCF2FB9AD809", L"ps_CF534B32F491561A", false}, {L"vs_66DE2CADB1F4AE6B", L"ps_864F1F949851B8DE", false},
        {L"vs_61AE8EB05FDC18DD", L"ps_FC43E42710010343", false},
    };
    for (const auto& p : pairs) {
        const auto vs = readFile(root + L"\\shaders\\" + p.vs + L".dxbc");
        const auto ps = readFile(root + L"\\shaders\\" + p.ps + L".dxbc");
        check(!vs.empty() && !ps.empty(), "real corpus pair present");
        edvr::EngineVelocityInputs in;
        std::string why;
        check(edvr::engineVelocityDeriveInputs(vs.data(), vs.size(), in, why), why.c_str());
        check(in.slotFromVsPatch == p.vsPatch, "real family: VS patch needed exactly for the UV-only family");
        std::vector<BYTE> pvs = vs, pps;
        if (in.slotFromVsPatch) check(edvr::engineVelocityPatchVs(vs.data(), vs.size(), in, pvs, why), why.c_str());
        check(edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, pps, why), why.c_str());
        ComPtr<ID3D11VertexShader> v;
        ComPtr<ID3D11PixelShader> f;
        check(SUCCEEDED(device->CreateVertexShader(pvs.data(), pvs.size(), nullptr, &v)), "real patched VS created on WARP");
        check(SUCCEEDED(device->CreatePixelShader(pps.data(), pps.size(), nullptr, &f)), "real patched PS created on WARP");
        ComPtr<ID3D11ShaderReflection> reflect;
        check(SUCCEEDED(D3DReflect(pps.data(), pps.size(), IID_PPV_ARGS(&reflect))), "real patched PS reflects");
        std::printf("  corpus: %ls + %ls: slot v%u.%c, SV_Position v%u%s -- patched, reflected, created\n", p.vs, p.ps,
                    in.identityRegister, "xyzw"[in.identityComponent], in.positionRegister,
                    in.slotFromVsPatch ? ", VS exports EDVRPOOLSLOT" : "");
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    bool selfTest = false;
    std::wstring corpusRoot;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--self-test" || a == L"--dry-run") selfTest = true;
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
    shader_tests::run({device.Get(), context.Get(), &check});
    emit_tests::run({&check});
    math_tests::run({device.Get(), context.Get(), &check});
    if (!corpusRoot.empty()) corpus(device.Get(), corpusRoot);
    std::printf("engine_velocity_test: %u checks passed%s.\n", g_checks, corpusRoot.empty() ? "" : " including the real shader corpus");
    return 0;
}
