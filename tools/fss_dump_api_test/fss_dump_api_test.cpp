// System32 WARP coverage for direct FSS dump capture/readback API costs.
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstring>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/log.h"
#include "../../src/common/plugin_cost.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/fss_dump.h"
#include "../../src/d3d11/fss_dump_cost_sites.h"
#include "../../src/d3d11/shader_swap.h"

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;
namespace fs = std::filesystem;

namespace {

unsigned g_checks = 0;
unsigned g_failures = 0;
bool check(bool value, const char* label) {
    ++g_checks;
    if (!value) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
    return value;
}

void requireHr(HRESULT hr, const char* label) {
    if (FAILED(hr)) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s (0x%08lx)", label,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

class TempWorkingDirectory final {
public:
    TempWorkingDirectory() {
        // A zero-length query returns the required capacity including NUL;
        // a successful second call returns characters excluding NUL.
        const DWORD required = GetCurrentDirectoryW(0, nullptr);
        if (!required)
            throw std::runtime_error("read current working directory");
        m_old.resize(required);
        const DWORD oldLength = GetCurrentDirectoryW(required, m_old.data());
        if (!oldLength || oldLength >= required)
            throw std::runtime_error("read current working directory");
        m_old.resize(oldLength);

        wchar_t temp[MAX_PATH]{};
        const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
        if (!tempLength || tempLength >= MAX_PATH)
            throw std::runtime_error("read temporary directory");
        wchar_t candidate[MAX_PATH]{};
        if (!GetTempFileNameW(temp, L"edv", 0, candidate))
            throw std::runtime_error("create temporary path name");
        if (!DeleteFileW(candidate))
            throw std::runtime_error("remove temporary name file");
        if (!CreateDirectoryW(candidate, nullptr))
            throw std::runtime_error("create temporary working directory");
        m_path = candidate;
        if (!SetCurrentDirectoryW(m_path.c_str())) {
            RemoveDirectoryW(m_path.c_str());
            throw std::runtime_error("enter temporary working directory");
        }
    }

    ~TempWorkingDirectory() {
        if (!SetCurrentDirectoryW(m_old.c_str())) {
            std::puts("NOTE: original working directory could not be restored; temporary output retained");
            return;
        }
        std::error_code error;
        fs::remove_all(m_path, error);
        if (error) std::printf("NOTE: temporary output cleanup failed: %s\n",
                               error.message().c_str());
    }

private:
    std::wstring m_old;
    std::wstring m_path;
};

struct Fixture final {
    HMODULE systemD3d11 = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> targetView;
    ComPtr<ID3D11Texture1D> target1d;
    ComPtr<ID3D11RenderTargetView> targetView1d;

    Fixture() {
        const auto system = edvr::openSystemD3D11();
        if (!system.module || system.result != S_OK)
            throw std::runtime_error("System32 d3d11.dll is unavailable");
        systemD3d11 = system.module;
        const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
            GetProcAddress(systemD3d11, "D3D11CreateDevice"));
        if (!create) throw std::runtime_error("System32 D3D11CreateDevice is unavailable");
        D3D_FEATURE_LEVEL level{};
        requireHr(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &device, &level, &immediate),
                  "create System32 WARP device");
        requireHr(device->CreateDeferredContext(0, &deferred),
                  "create WARP deferred context");
        createTargets();
        bind2d(immediate.Get());
    }

    ~Fixture() {
        edvr::fssDumpShutdown();
        deferred.Reset();
        immediate.Reset();
        targetView1d.Reset();
        target1d.Reset();
        targetView.Reset();
        target.Reset();
        device.Reset();
        if (systemD3d11) FreeLibrary(systemD3d11);
    }

    void createTargets() {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 4;
        td.Height = 4;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        requireHr(device->CreateTexture2D(&td, nullptr, &target),
                  "create WARP FSS source texture");
        requireHr(device->CreateRenderTargetView(target.Get(), nullptr, &targetView),
                  "create WARP FSS source RTV");
        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        immediate->ClearRenderTargetView(targetView.Get(), red);

        D3D11_TEXTURE1D_DESC d1{};
        d1.Width = 4;
        d1.MipLevels = 1;
        d1.ArraySize = 1;
        d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d1.Usage = D3D11_USAGE_DEFAULT;
        d1.BindFlags = D3D11_BIND_RENDER_TARGET;
        requireHr(device->CreateTexture1D(&d1, nullptr, &target1d),
                  "create WARP one-dimensional RTV source");
        requireHr(device->CreateRenderTargetView(target1d.Get(), nullptr,
                                                 &targetView1d),
                  "create WARP one-dimensional RTV");
    }

    void bind2d(ID3D11DeviceContext* context) {
        ID3D11RenderTargetView* view = targetView.Get();
        context->OMSetRenderTargets(1, &view, nullptr);
    }

    void bind1d() {
        ID3D11RenderTargetView* view = targetView1d.Get();
        immediate->OMSetRenderTargets(1, &view, nullptr);
    }

    void unbind() { immediate->OMSetRenderTargets(0, nullptr, nullptr); }
};

void startCollector(ID3D11DeviceContext* owner, bool nextApiSample = true) {
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(0x1u, static_cast<uint64_t>(edvr::qpcFrequency()));
    edvrPluginCostSetOwnerContext(owner);
    EdvrPluginCostWindowV1 ignored{};
    if (edvrPluginCostFrameBoundary(0, 0, 0, nextApiSample ? 1 : 0, 0,
                                    &ignored))
        throw std::runtime_error("collector bootstrap unexpectedly closed a window");
    if ((edvrPluginCostApiSampleContext(owner) != 0) != nextApiSample)
        throw std::runtime_error("collector API sample gate does not match setup");
}

EdvrPluginCostWindowV1 finishWindow(uint8_t closedApiSample = 1,
                                    uint8_t nextApiSample = 1) {
    EdvrPluginCostWindowV1 report{};
    uint8_t completed = edvrPluginCostFrameBoundary(1, 0, closedApiSample,
                                                    nextApiSample, 0, &report);
    if (completed) throw std::runtime_error("collector closed before frame 1800");
    for (uint32_t frame = 2; frame < pc::kWindowFrameCount; ++frame) {
        completed = edvrPluginCostFrameBoundary(frame, 0, closedApiSample,
                                                nextApiSample, 0, &report);
        if (completed) throw std::runtime_error("collector closed before frame 1800");
    }
    completed = edvrPluginCostFrameBoundary(pc::kWindowFrameCount, 0,
                                            closedApiSample, nextApiSample,
                                            0, &report);
    if (!completed) throw std::runtime_error("collector did not close at frame 1800");
    return report;
}

uint64_t maskFor(std::initializer_list<uint16_t> sites) {
    uint64_t mask = 0;
    for (uint16_t site : sites) mask |= uint64_t{1} << site;
    return mask;
}

const EdvrPluginCostOwnerV1& scannerOwner(const EdvrPluginCostWindowV1& report) {
    return report.owners[static_cast<uint8_t>(pc::Owner::Scanners)];
}

bool exactCalls(const EdvrPluginCostWindowV1& report, uint64_t read,
                uint64_t work, uint64_t transfer, uint64_t mask,
                const char* label) {
    const auto& owner = scannerOwner(report);
    const bool counts = owner.apiObserved == ((read + work + transfer) ? 1 : 0) &&
        owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == read &&
        owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == work &&
        owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == transfer &&
        owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0 &&
        owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0;
    const bool sites = owner.apiSiteMask[0] == mask && owner.apiSiteMask[1] == 0;
    bool otherOwnersClear = true;
    for (uint8_t i = 0; i < EDVR_PLUGIN_COST_OWNER_COUNT; ++i) {
        if (i == static_cast<uint8_t>(pc::Owner::Scanners)) continue;
        for (uint8_t api = 0; api < EDVR_PLUGIN_COST_API_CLASS_COUNT; ++api)
            otherOwnersClear &= report.owners[i].apiCalls[api] == 0;
        otherOwnersClear &= report.owners[i].apiSiteMask[0] == 0 &&
                            report.owners[i].apiSiteMask[1] == 0;
    }
    char message[256];
    std::snprintf(message, sizeof(message), "%s (exact owner classes and site mask)", label);
    return check(counts && sites && otherOwnersClear, message);
}

bool reportHeader(const EdvrPluginCostWindowV1& report,
                  uint64_t expectedSamples = pc::kWindowFrameCount) {
    return check(report.version == pc::kWindowVersion && report.profileBit == 1 &&
                 report.firstFrame == 1 && report.lastFrame == pc::kWindowFrameCount &&
                 report.windowFrames == pc::kWindowFrameCount &&
                 report.completedApiSampleFrames == expectedSamples,
                 "report covers exactly 1800 frames and the requested API sample count");
}

edvr::FssDumpApiTestState seededState(uint32_t pass = 0) {
    edvr::FssDumpApiTestState state{};
    state.frame = 1;
    state.done = false;
    state.seriesWant = 0;
    state.seriesDone = true;
    state.dumping = true;
    state.dumpPass = pass;
    state.pendingKind = 1;  // production composite checkpoint pair
    state.pendingEye = 0;
    return state;
}

void clearDumpState() {
    edvr::fssDumpShutdown();
    edvr::fssDumpApiTestSetState({});
}

void endAndBoundary(ID3D11DeviceContext* context) {
    edvr::fssDumpEnd(context);
    edvr::fssDumpFrameBoundary(context);
}

bool assertRedDump(const wchar_t* relativePath) {
    std::ifstream file(fs::path(relativePath), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (!check(file.good() || file.eof(), "open production dump output")) return false;
    if (!check(bytes.size() == 4u * 4u * 4u,
               "production WARP readback writes one tightly packed 4x4 RGBA image"))
        return false;
    bool red = true;
    for (size_t i = 0; i + 3 < bytes.size(); i += 4)
        red &= bytes[i] == 255 && bytes[i + 1] == 0 &&
               bytes[i + 2] == 0 && bytes[i + 3] == 255;
    return check(red, "dumped WARP pixels retain the caller RTV clear color");
}

bool coldAndWarm(Fixture& fixture) {
    bool ok = true;
    clearDumpState();
    fixture.bind2d(fixture.immediate.Get());
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());

    // This first actual End/boundary pair creates staging for c3 and c5.
    endAndBoundary(fixture.immediate.Get());
    const auto first = edvr::fssDumpApiTestState();
    ok &= check(first.dumping && first.dumpPass == 1 && !first.done,
                "production first boundary advances to pass 1 and keeps the dump armed");
    auto report = finishWindow();
    ok &= reportHeader(report);
    ok &= exactCalls(report, 8, 2, 6,
                     maskFor({39, 40, 41, 42, 43, 44, 45,
                              46, 47, 48, 49, 50, 51, 52}),
                     "cold composite End plus frame boundary");
    ok &= assertRedDump(L"edvr_logs\\dumps\\fssdump_p0_c3_post-comp_eye0.bin");
    ok &= assertRedDump(L"edvr_logs\\dumps\\fssdump_p0_c5_end-hdr_eye0.bin");

    // The real next pass reuses the staged textures and the production pass state.
    startCollector(fixture.immediate.Get());
    endAndBoundary(fixture.immediate.Get());
    const auto second = edvr::fssDumpApiTestState();
    ok &= check(!second.dumping && second.dumpPass == 1 && second.done,
                "production second boundary completes pass 1 and retires the dump");
    report = finishWindow();
    ok &= reportHeader(report);
    ok &= exactCalls(report, 6, 0, 6,
                     maskFor({39, 40, 41, 42, 45, 46, 47, 50, 51, 52}),
                     "warm composite End plus frame boundary");
    ok &= assertRedDump(L"edvr_logs\\dumps\\fssdump_p1_c3_post-comp_eye0.bin");
    ok &= assertRedDump(L"edvr_logs\\dumps\\fssdump_p1_c5_end-hdr_eye0.bin");

    ID3D11RenderTargetView* observed = nullptr;
    fixture.immediate->OMGetRenderTargets(1, &observed, nullptr);
    ok &= check(observed == fixture.targetView.Get(),
                "capture and readback leave the caller RTV bound");
    if (observed) observed->Release();
    clearDumpState();
    edvrPluginCostShutdown();
    return ok;
}

bool noRenderTargetPrefix(Fixture& fixture) {
    clearDumpState();
    fixture.unbind();
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());
    endAndBoundary(fixture.immediate.Get());
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= exactCalls(report, 1, 0, 0, maskFor({39}),
                     "missing RTV records only the attempted OM getter");
    clearDumpState();
    edvrPluginCostShutdown();
    fixture.bind2d(fixture.immediate.Get());
    return ok;
}

bool nonTexture2dPrefix(Fixture& fixture) {
    clearDumpState();
    fixture.bind1d();
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());
    endAndBoundary(fixture.immediate.Get());
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= exactCalls(report, 3, 0, 0, maskFor({39, 40, 41}),
                     "one-dimensional RTV stops at the actual Texture2D QI");
    clearDumpState();
    edvrPluginCostShutdown();
    fixture.bind2d(fixture.immediate.Get());
    return ok;
}

uint32_t g_getDeviceAttempts = 0;
void STDMETHODCALLTYPE injectedNullGetDevice(ID3D11DeviceChild*,
                                              ID3D11Device** device) {
    ++g_getDeviceAttempts;
    if (device) *device = nullptr;
}

bool getDeviceNullPrefix(Fixture& fixture) {
    clearDumpState();
    fixture.bind2d(fixture.immediate.Get());
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());

    edvr::VTableHook hook;
    if (!check(hook.attach(fixture.immediate.Get(), 128),
               "attach WARP context for GetDevice null-result injection")) return false;
    if (!check(hook.setMode(edvr::HookMode::CopyVptr),
               "select private context vtable for GetDevice null-result injection")) return false;
    if (!check(hook.replace(3, reinterpret_cast<void*>(&injectedNullGetDevice), nullptr),
               "replace the actual ID3D11DeviceChild::GetDevice slot")) return false;
    if (!check(hook.commit(), "commit GetDevice null-result injection")) return false;

    g_getDeviceAttempts = 0;
    endAndBoundary(fixture.immediate.Get());
    hook.uninstall();
    check(g_getDeviceAttempts == 2,
          "capture and remembered capture each attempt GetDevice once");
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= exactCalls(report, 8, 0, 0,
                     maskFor({39, 40, 41, 42, 43, 46, 47, 48}),
                     "null GetDevice results record only the attempted read prefix");
    clearDumpState();
    edvrPluginCostShutdown();
    return ok;
}

uint32_t g_createTextureAttempts = 0;
HRESULT STDMETHODCALLTYPE injectedCreateTextureFailure(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D** texture) {
    ++g_createTextureAttempts;
    if (texture) *texture = nullptr;
    return E_OUTOFMEMORY;
}

bool createTextureFailurePrefix(Fixture& fixture) {
    clearDumpState();
    fixture.bind2d(fixture.immediate.Get());
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());

    edvr::VTableHook hook;
    // ID3D11Device::CreateTexture2D is vtable slot 5 after IUnknown and
    // CreateBuffer / CreateTexture1D.
    if (!check(hook.attach(fixture.device.Get(), 43),
               "attach WARP device for CreateTexture2D failure injection")) return false;
    if (!check(hook.setMode(edvr::HookMode::InPlace),
               "select the physical WARP device vtable for the scoped injection")) return false;
    if (!check(hook.replace(5, reinterpret_cast<void*>(&injectedCreateTextureFailure), nullptr),
               "replace the actual ID3D11Device::CreateTexture2D slot")) return false;
    if (!check(hook.commit(), "commit CreateTexture2D failure injection")) return false;

    g_createTextureAttempts = 0;
    endAndBoundary(fixture.immediate.Get());
    hook.uninstall();
    check(g_createTextureAttempts == 2,
          "cold capture and remembered capture each attempt staging creation");
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= exactCalls(report, 8, 2, 0,
                     maskFor({39, 40, 41, 42, 43, 44,
                              46, 47, 48, 49}),
                     "failed staging creation records attempted work without copy or Map");
    clearDumpState();
    edvrPluginCostShutdown();
    return ok;
}

enum class MapFailure { HResult, NullData };
MapFailure g_mapFailure = MapFailure::HResult;
uint32_t g_mapAttempts = 0;
HRESULT STDMETHODCALLTYPE injectedMap(ID3D11DeviceContext*, ID3D11Resource*,
                                      UINT, D3D11_MAP, UINT,
                                      D3D11_MAPPED_SUBRESOURCE* mapped) {
    ++g_mapAttempts;
    if (mapped) *mapped = {};
    return g_mapFailure == MapFailure::HResult ? E_FAIL : S_OK;
}

bool mapFailurePrefix(Fixture& fixture, MapFailure failure) {
    clearDumpState();
    fixture.bind2d(fixture.immediate.Get());
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());

    edvr::VTableHook hook;
    if (!check(hook.attach(fixture.immediate.Get(), 128),
               "attach the real WARP immediate context for Map result injection")) return false;
    if (!check(hook.setMode(edvr::HookMode::CopyVptr),
               "select a private copy of the WARP context vtable")) return false;
    if (!check(hook.replace(14, reinterpret_cast<void*>(&injectedMap), nullptr),
               "replace only ID3D11DeviceContext::Map for the scoped test")) return false;
    if (!check(hook.commit(), "commit the scoped Map result injection")) return false;

    g_mapFailure = failure;
    g_mapAttempts = 0;
    endAndBoundary(fixture.immediate.Get());
    hook.uninstall();
    check(g_mapAttempts == 2, "Map is attempted once for each filled checkpoint");

    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= exactCalls(report, 8, 2, 4,
                     maskFor({39, 40, 41, 42, 43, 44, 45,
                              46, 47, 48, 49, 50, 51}),
                     failure == MapFailure::HResult
                         ? "failed Map attempts preserve the prefix with no Unmap"
                         : "successful null-data Map preserves the prefix with no Unmap");
    clearDumpState();
    edvrPluginCostShutdown();
    return ok;
}

bool zeroSamplingWindows(Fixture& fixture) {
    bool ok = true;

    clearDumpState();
    fixture.bind2d(fixture.immediate.Get());
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get(), false);
    endAndBoundary(fixture.immediate.Get());
    auto report = finishWindow(0, 0);
    ok &= reportHeader(report, 0);
    ok &= exactCalls(report, 0, 0, 0, 0,
                     "owner calls in an API-unsampled frame produce no notes");
    clearDumpState();
    edvrPluginCostShutdown();

    clearDumpState();
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());
    endAndBoundary(fixture.deferred.Get());
    report = finishWindow();
    ok &= reportHeader(report);
    ok &= exactCalls(report, 0, 0, 0, 0,
                     "deferred-context dump calls produce no owner API notes");
    clearDumpState();
    edvrPluginCostShutdown();

    clearDumpState();
    fixture.bind2d(fixture.immediate.Get());
    edvr::fssDumpApiTestSetState(seededState());
    startCollector(fixture.immediate.Get());
    std::thread worker([&] { endAndBoundary(fixture.immediate.Get()); });
    worker.join();
    report = finishWindow();
    ok &= reportHeader(report);
    ok &= exactCalls(report, 0, 0, 0, 0,
                     "foreign-worker immediate-context calls produce no owner API notes");
    clearDumpState();
    edvrPluginCostShutdown();
    return ok;
}

bool runSelfTest() {
    TempWorkingDirectory temporary;
    Fixture fixture;
    bool ok = coldAndWarm(fixture);
    ok &= noRenderTargetPrefix(fixture);
    ok &= nonTexture2dPrefix(fixture);
    ok &= getDeviceNullPrefix(fixture);
    ok &= createTextureFailurePrefix(fixture);
    ok &= mapFailurePrefix(fixture, MapFailure::HResult);
    ok &= mapFailurePrefix(fixture, MapFailure::NullData);
    ok &= zeroSamplingWindows(fixture);
    edvr::fssDumpShutdown();
    edvrPluginCostShutdown();
    return ok && g_failures == 0;
}

}  // namespace

namespace edvr {
// The rig seeds FSS dump state directly; this satisfies the unused config
// entry point in the linked production translation unit.
int Config::getInt(const char*, int def) const { return def; }
Log& Log::get() { static Log logger; return logger; }
Log::~Log() {}
void Log::note(const char*, ...) {}
int64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
int64_t qpcFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}
void breadcrumb(const char*) {}
void* submittedTexture(int) { return nullptr; }
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext*, const void*,
                                        size_t, const char*, const char*) {
    return nullptr;
}
uint64_t lookupShaderHash(void*) { return 0; }
}  // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("fss_dump_api_test: dry-run (no module, device, collector, or file access)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: fss_dump_api_test.exe --dry-run|--self-test");
        return 2;
    }
    try {
        if (!runSelfTest()) {
            std::printf("fss_dump_api_test: %u of %u checks failed\n",
                        g_failures, g_checks);
            return 1;
        }
        std::printf("fss_dump_api_test: %u checks passed\n", g_checks);
        return 0;
    } catch (const std::exception& error) {
        edvr::fssDumpShutdown();
        edvrPluginCostShutdown();
        std::printf("fss_dump_api_test: failed: %s\n", error.what());
        return 1;
    }
}
