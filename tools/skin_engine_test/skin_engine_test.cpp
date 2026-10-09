// skin_engine_test: the second skin on the engine side, run on WARP (docs\kinematic-motion-injection-2026-09-19.md, "F2 built").
//
//   --dry-run          the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test [root] every check; the optional argument is the repository root the rig reads the shipped HLSL from (the mutation tool hands it a
//                      temp root holding an edited copy), and the working directory the helper suites read their sources from
//
// Cases (every check is labelled "C<case>.<what>" or "L<case>.<what>"; tools\skin_engine_test\mutants.py names the case that must catch each mutation):
//   C1  the compose's arithmetic for a skinned surface (tools\engine_velocity_test\skin_compose_tests.h): previous position = world + (nCam - bCam) + E
//   C2  the production mv pass on real resources: valid E, zero E, masked pixels, rigid records untouched, the counters and the |E| bins
//   C3  the derived blend state's target-7 modes
//   L1..L9  the engine's draw half (the linked engine_velocity.cpp, skin_join_gpu.cpp) end to end: arming, the first frame's honest "no history",
//       exact zero for a steady character in both eyes, E for a moving one, a changed job table, what is bound and put back, the hook's list as the
//       identity, the periodic lines, the guard on a small previous palette buffer (tools\engine_velocity_test\skin_lifecycle_tests.h)
//   L10, L11  the live record of a base (a stale second record in the pool; the draws' instance-stream entries decide) and the no-history write of a
//       skinned family's pixel shader that exports no E
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../engine_velocity_test/shader_tests.h"
#include "../engine_velocity_test/emit_tests.h"
#include "../engine_velocity_test/math_tests.h"
#include "../engine_velocity_test/consumer_tests.h"
#include "../engine_velocity_test/corpus_identity.h"
#include "../engine_velocity_test/lifecycle_tests.h"
#include "../engine_velocity_test/skin_compose_tests.h"
#include "../engine_velocity_test/skin_lifecycle_tests.h"
#include "../../src/common/runtime_profile.h"
#include "../../third_party/dxbc_hash/DxilHash.cpp"

using Microsoft::WRL::ComPtr;

// The linked draw half uses the same internal binding guard as production; the rig does not link the flat readback module that defines it.
namespace edvr { thread_local bool g_flatComputeInternal = false; }

// The case ids the suites label their checks with (tools\skin_engine_test\mutants.py reads this list to hold each case to a mutation).
static const char* const kCases[] = {"C1.arithmetic", "C2.production-mv", "C3.blend",      "L1.arming",    "L2.first-frame", "L3.steady",
                                     "L4.moving",     "L5.job-table",     "L6.bindings",   "L7.hook-list", "L8.periodic-lines", "L9.small-buffer",
                                     "L10.live-record", "L11.no-history-write"};

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
        std::fflush(stdout);
    }
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (i == 2 && argv[1] == std::string("--self-test")) root = a;
        else {
            std::fprintf(stderr, "usage: skin_engine_test --self-test [repository root] | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: skin_engine_test --self-test [repository root] | --dry-run\n");
        return 2;
    }
    if (!root.empty() && !SetCurrentDirectoryA(root.c_str())) {
        std::fprintf(stderr, "FAIL: the repository root %s cannot be entered\n", root.c_str());
        return 2;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context)) || level < D3D_FEATURE_LEVEL_11_0) {
        std::printf("FAIL: C1.0 WARP at feature level 11 is needed\n");
        return 1;
    }
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;   // the second skin is a VR feature; the lifecycle suite runs under this profile too
    const skin_compose_tests::Harness h{device.Get(), context.Get(), &check};
    skin_compose_tests::runMath(h);
    skin_compose_tests::runConsumer(h);
    skin_compose_tests::runBlend(h);
    skin_lifecycle_tests::run({device.Get(), context.Get(), &check});
    if (g_failures) {
        std::fprintf(stderr, "FAIL: skin_engine_test: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u skin engine checks (%zu cases:", g_checks, sizeof(kCases) / sizeof(kCases[0]));
    for (const char* c : kCases) std::printf(" %s", c);
    std::printf(")\n");
    return 0;
}
