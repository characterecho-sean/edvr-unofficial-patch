// Real System32 WARP coverage for the precompiled shader creation cost slice.
// Only GetDevice and Create*Shader attempts inside shaderSwapCreate* are counted;
// Releases and D3DCompile fixture setup are outside this bounded metric.
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "../../src/common/plugin_cost.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/shader_cost_sites.h"
#include "../../src/d3d11/shader_swap.h"

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;

namespace {
unsigned g_failures = 0;
bool check(bool value, const char* message) {
    if (!value) {
        ++g_failures;
        std::printf("FAIL: %s\n", message);
    }
    return value;
}

void requireHr(HRESULT hr, const char* message) {
    if (FAILED(hr)) throw std::runtime_error(message);
}

ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    requireHr(D3DCompile(source, std::strlen(source), "shader_swap_api_test", nullptr,
                         nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                         &code, &errors), "compile WARP shader fixture");
    return code;
}

struct Fixture {
    HMODULE systemD3d11 = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> cs;
    ComPtr<ID3DBlob> ps;

    Fixture() {
        auto system = edvr::openSystemD3D11();
        if (!system.module || system.result != S_OK) throw std::runtime_error("System32 d3d11 unavailable");
        systemD3d11 = system.module;
        const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
            GetProcAddress(systemD3d11, "D3D11CreateDevice"));
        if (!create) throw std::runtime_error("System32 D3D11CreateDevice unavailable");
        D3D_FEATURE_LEVEL level{};
        requireHr(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &device, &level, &immediate),
                  "create actual System32 WARP device");
        if (!edvr::reportSystemD3D11Only("shader_swap_api_test"))
            throw std::runtime_error("unexpected non-System32 d3d11 module");
        requireHr(device->CreateDeferredContext(0, &deferred), "create deferred context");
        vs = compile("float4 main(uint id:SV_VertexID):SV_Position { return float4(0,0,0,1); }", "vs_5_0");
        cs = compile("[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {}", "cs_5_0");
        ps = compile("float4 main():SV_Target { return float4(1,0,0,1); }", "ps_5_0");
    }
    ~Fixture() {
        deferred.Reset();
        immediate.Reset();
        device.Reset();
        if (systemD3d11) FreeLibrary(systemD3d11);
    }
};

uint64_t bit(uint16_t site) { return uint64_t{1} << (site - 64); }

void startWindow(ID3D11DeviceContext* owner) {
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(1, 1000000);
    edvrPluginCostSetOwnerContext(owner);
    EdvrPluginCostWindowV1 discarded{};
    if (edvrPluginCostFrameBoundary(0, 0, 0, 1, 0, &discarded))
        throw std::runtime_error("bootstrap boundary unexpectedly emitted a window");
    if (!edvrPluginCostApiSampleContext(owner))
        throw std::runtime_error("registered immediate owner did not become sample eligible");
}

EdvrPluginCostWindowV1 finishWindow(uint32_t first) {
    EdvrPluginCostWindowV1 report{};
    uint8_t completed = edvrPluginCostFrameBoundary(first, 0, 1, 1, 0, &report);
    if (completed) throw std::runtime_error("first boundary unexpectedly emitted full window");
    for (uint32_t frame = first + 1; frame < first + 1799; ++frame) {
        completed = edvrPluginCostFrameBoundary(frame, 0, 1, 1, 0, &report);
        if (completed) throw std::runtime_error("window emitted before its 1800th frame");
    }
    completed = edvrPluginCostFrameBoundary(first + 1799, 0, 1, 1, 0, &report);
    if (!completed) throw std::runtime_error("1800-frame window did not close");
    return report;
}

bool reportHeader(const EdvrPluginCostWindowV1& report, uint32_t first) {
    return check(report.version == pc::kWindowVersion && report.profileBit == 1 &&
                 report.firstFrame == first && report.lastFrame == first + 1799 &&
                 report.windowFrames == 1800 && report.completedApiSampleFrames == 1800,
                 "report carries the exact 1800-frame API sample denominator");
}

bool validAndUnsampledCases(Fixture& f) {
    startWindow(f.immediate.Get());
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11PixelShader> ps;
    vs.Attach(edvr::shaderSwapCreateVs(f.immediate.Get(), f.vs->GetBufferPointer(),
                                 f.vs->GetBufferSize(), "test-vs", "rig"));
    cs.Attach(edvr::shaderSwapCreateCs(f.immediate.Get(), f.cs->GetBufferPointer(),
                                 f.cs->GetBufferSize(), "test-cs", "rig"));
    ps.Attach(edvr::shaderSwapCreatePs(f.immediate.Get(), f.ps->GetBufferPointer(),
                                 f.ps->GetBufferSize(), "test-ps", "rig"));
    if (!check(vs.Get() && cs.Get() && ps.Get(), "all three real WARP shader creations succeed")) return false;

    const uint8_t malformedBytecode[] = {0};
    ComPtr<ID3D11VertexShader> invalidBytecode;
    invalidBytecode.Attach(edvr::shaderSwapCreateVs(f.immediate.Get(), malformedBytecode,
                                              sizeof(malformedBytecode), "invalid-bytecode", "rig"));
    check(invalidBytecode == nullptr,
          "nonempty malformed bytecode reaches D3D and fails without producing a shader");

    // The public null/length guards must return before sampling or touching COM.
    check(edvr::shaderSwapCreateVs(nullptr, f.vs->GetBufferPointer(), f.vs->GetBufferSize(), "null", "rig") == nullptr,
          "null context exits through the original input guard");
    check(edvr::shaderSwapCreateCs(f.immediate.Get(), nullptr, f.cs->GetBufferSize(), "null", "rig") == nullptr,
          "null bytecode exits through the original input guard");
    check(edvr::shaderSwapCreatePs(f.immediate.Get(), f.ps->GetBufferPointer(), 0, "empty", "rig") == nullptr,
          "zero bytecode length exits through the original input guard");

    edvrPluginCostSetApiSampleFrame(0);
    ComPtr<ID3D11PixelShader> unsampled;
    unsampled.Attach(edvr::shaderSwapCreatePs(f.immediate.Get(), f.ps->GetBufferPointer(),
                                        f.ps->GetBufferSize(), "unsampled", "rig"));
    check(unsampled.Get() != nullptr, "unsampled invocation still performs a real shader creation");
    edvrPluginCostSetApiSampleFrame(1);

    check(edvrPluginCostApiSampleContext(f.deferred.Get()) == 0,
          "deferred context is not eligible while immediate context is registered");
    ComPtr<ID3D11VertexShader> deferred;
    deferred.Attach(edvr::shaderSwapCreateVs(f.deferred.Get(), f.vs->GetBufferPointer(),
                                       f.vs->GetBufferSize(), "deferred", "rig"));
    check(deferred.Get() != nullptr, "deferred-context create has real D3D effects without sampling");
    check(edvrPluginCostApiSampleContext(f.immediate.Get()) != 0,
          "owner sample eligibility remains open after deferred work");

    ComPtr<ID3D11ComputeShader> workerShader;
    uint8_t workerImmediateEligible = 1;
    uint8_t workerDeferredEligible = 1;
    std::thread worker([&] {
        workerImmediateEligible = edvrPluginCostApiSampleContext(f.immediate.Get());
        workerDeferredEligible = edvrPluginCostApiSampleContext(f.deferred.Get());
        workerShader.Attach(edvr::shaderSwapCreateCs(f.deferred.Get(), f.cs->GetBufferPointer(),
                                                f.cs->GetBufferSize(), "worker", "rig"));
    });
    worker.join();
    check(workerImmediateEligible == 0 && workerDeferredEligible == 0 && workerShader.Get() != nullptr,
          "foreign worker is ineligible even for the registered context and creates through deferred context");

    const auto report = finishWindow(1);
    const auto& core = report.owners[static_cast<uint8_t>(pc::Owner::Core)];
    const uint8_t read = static_cast<uint8_t>(pc::ApiClass::ReadQuery);
    const uint8_t work = static_cast<uint8_t>(pc::ApiClass::Work);
    bool ok = reportHeader(report, 1);
    ok &= check(core.apiCalls[read] == 4 && core.apiCalls[work] == 4 &&
                core.apiSiteMask[0] == 0 &&
                core.apiSiteMask[1] == (bit(116) | bit(117) | bit(118) | bit(119) | bit(120) | bit(121)),
                "sampled VS/CS/PS attempts count exact GetDevice and CreateShader sites only");
    ok &= check(core.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 0 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0,
                "shader create cohort attributes only the documented Core ReadQuery and Work classes");
    edvrPluginCostShutdown();
    return ok;
}

using GetDeviceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Device**);
GetDeviceFn g_realGetDevice = nullptr;
uint32_t g_faultHits = 0;
void STDMETHODCALLTYPE faultGetDevice(ID3D11DeviceContext*, ID3D11Device**) {
    ++g_faultHits;
    RaiseException(0xE042ED92u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

bool faultPrefixAndRecovery(Fixture& f) {
    startWindow(f.immediate.Get());
    edvr::VTableHook hook;
    if (!check(hook.attach(f.immediate.Get(), 128), "attach typed actual-context vtable hook")) return false;
    if (!check(hook.setMode(edvr::HookMode::CopyVptr), "use private vtable copy for fault injection")) return false;
    if (!check(hook.replace(3, reinterpret_cast<void*>(&faultGetDevice),
                           reinterpret_cast<void**>(&g_realGetDevice)),
               "stage ID3D11DeviceChild::GetDevice slot 3 fault")) return false;
    if (!check(hook.commit(), "commit typed GetDevice fault hook")) return false;
    const auto* bytes = f.ps->GetBufferPointer();
    auto* failed = edvr::shaderSwapCreatePs(f.immediate.Get(), bytes, f.ps->GetBufferSize(), "fault", "rig");
    check(failed == nullptr && g_faultHits == 1,
          "guard absorbs one actual WARP-context GetDevice fault before CreatePixelShader");
    hook.uninstall();
    ComPtr<ID3D11PixelShader> recovered;
    recovered.Attach(edvr::shaderSwapCreatePs(f.immediate.Get(), bytes,
                                        f.ps->GetBufferSize(), "recovery", "rig"));
    if (!check(recovered.Get() != nullptr, "same budget recovers after one guarded GetDevice fault")) return false;

    const auto report = finishWindow(1);
    const auto& core = report.owners[static_cast<uint8_t>(pc::Owner::Core)];
    bool ok = reportHeader(report, 1);
    ok &= check(core.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 2 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 1 &&
                core.apiSiteMask[0] == 0 &&
                core.apiSiteMask[1] == (bit(120) | bit(121)),
                "fault report records attempted GetDevice, omits skipped failed create, and records recovery");
    ok &= check(core.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 0 &&
                core.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0,
                "guarded prefix remains confined to the measured shader sites");
    edvrPluginCostShutdown();
    return ok;
}

bool selfTest() {
    Fixture fixture;
    bool ok = validAndUnsampledCases(fixture);
    ok &= faultPrefixAndRecovery(fixture);
    std::printf("shader_swap_api_test: %s (%u failures)\n",
                ok && g_failures == 0 ? "PASS" : "FAILED", g_failures);
    return ok && g_failures == 0;
}
} // namespace

// Nonhardware link boundaries only: link the real guard.cpp (including its
// filter and FaultBudget::charge) and the real vtable/code-hook implementations.
// The test does not arm a Sentinel, open a log, write files, or run monitor events.
namespace edvr {
Log& Log::get() { static Log logger; return logger; }
Log::~Log() {}
void Log::note(const char*, ...) {}
int64_t qpcNow() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }
void perfMonitorNoteEvent(uint32_t, double) {}
void breadcrumb(const char*) {}
} // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("shader_swap_api_test: dry-run (no module, device, logger, or file access)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::fprintf(stderr, "usage: shader_swap_api_test --dry-run | --self-test\n");
        return 2;
    }
    try {
        return selfTest() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "shader_swap_api_test: %s\n", e.what());
        edvrPluginCostShutdown();
        return 1;
    }
}
