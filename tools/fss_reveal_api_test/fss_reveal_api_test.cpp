// System32 WARP coverage for the production FSS Reveal API-cost sites.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>

#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/plugin_cost.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/exposure_fix.h"
#include "../../src/d3d11/fss_reveal.h"
#include "../../src/d3d11/fss_reveal_cost_sites.h"

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;
namespace fs = std::filesystem;
static std::string g_configValue = "sync";

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
        const DWORD required = GetCurrentDirectoryW(0, nullptr);
        if (!required) throw std::runtime_error("query current directory length");
        m_old.resize(required);
        const DWORD oldLength = GetCurrentDirectoryW(required, m_old.data());
        if (!oldLength || oldLength >= required)
            throw std::runtime_error("read current directory");
        m_old.resize(oldLength);
        wchar_t temp[MAX_PATH]{};
        const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
        if (!tempLength || tempLength >= MAX_PATH)
            throw std::runtime_error("read temp directory");
        wchar_t candidate[MAX_PATH]{};
        if (!GetTempFileNameW(temp, L"edv", 0, candidate))
            throw std::runtime_error("create temp directory name");
        if (!DeleteFileW(candidate) || !CreateDirectoryW(candidate, nullptr))
            throw std::runtime_error("create temp directory");
        m_path = candidate;
        if (!SetCurrentDirectoryW(m_path.c_str())) {
            RemoveDirectoryW(m_path.c_str());
            throw std::runtime_error("enter temp directory");
        }
    }

    ~TempWorkingDirectory() {
        if (!SetCurrentDirectoryW(m_old.c_str())) {
            std::puts("NOTE: original directory could not be restored; temp retained");
            return;
        }
        std::error_code error;
        fs::remove_all(m_path, error);
        if (error) std::printf("NOTE: temp cleanup failed: %s\n", error.message().c_str());
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
    ComPtr<ID3D11Buffer> sceneCb;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;

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

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 5376;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        requireHr(device->CreateBuffer(&bd, nullptr, &sceneCb),
                  "create actual 5376-byte scene constant buffer");

        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 16;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::array<std::uint8_t, 16 * 16 * 4> pixels{};
        pixels.fill(0x6D);
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = pixels.data();
        initial.SysMemPitch = 16 * 4;
        initial.SysMemSlicePitch = static_cast<UINT>(pixels.size());
        requireHr(device->CreateTexture2D(&td, &initial, &texture),
                  "create actual WARP source texture");
        requireHr(device->CreateShaderResourceView(texture.Get(), nullptr, &srv),
                  "create actual WARP source SRV");
        bind(immediate.Get());
        seedScene(0x31);
    }

    ~Fixture() {
        edvr::fssRevealEnd(immediate.Get());
        edvr::fssRevealShutdown();
        immediate.Reset();
        deferred.Reset();
        srv.Reset();
        texture.Reset();
        sceneCb.Reset();
        device.Reset();
        if (systemD3d11) FreeLibrary(systemD3d11);
    }

    void bind(ID3D11DeviceContext* context) {
        ID3D11Buffer* cb = sceneCb.Get();
        ID3D11ShaderResourceView* views[4] = {srv.Get(), nullptr, nullptr, nullptr};
        context->PSSetConstantBuffers(1, 1, &cb);
        context->PSSetShaderResources(0, 4, views);
    }

    void seedScene(std::uint8_t value) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        requireHr(immediate->Map(sceneCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped), "map actual scene CB for test data");
        std::memset(mapped.pData, value, 5376);
        immediate->Unmap(sceneCb.Get(), 0);
    }
};

void configure(const char* value) {
    g_configValue = value;
    edvr::fssRevealConfigure(edvr::Config::get());
}

void startCollector(ID3D11DeviceContext* owner, bool nextApiSample = true) {
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(0x1u, static_cast<std::uint64_t>(edvr::qpcFrequency()));
    edvrPluginCostSetOwnerContext(owner);
    EdvrPluginCostWindowV1 ignored{};
    if (edvrPluginCostFrameBoundary(0, 0, 0, nextApiSample ? 1 : 0, 0, &ignored))
        throw std::runtime_error("collector bootstrap unexpectedly closed a window");
    if ((edvrPluginCostApiSampleContext(owner) != 0) != nextApiSample)
        throw std::runtime_error("API sample gate differs from setup");
}

EdvrPluginCostWindowV1 finishWindow(std::uint8_t firstSample = 1,
                                   bool allUnsampled = false) {
    EdvrPluginCostWindowV1 report{};
    for (std::uint32_t frame = 1; frame <= pc::kWindowFrameCount; ++frame) {
        const std::uint8_t closedSample = allUnsampled ? 0 : frame == 1 ? firstSample : 1;
        const auto completed = edvrPluginCostFrameBoundary(
            frame, 0, closedSample, allUnsampled ? 0 : 1, 0, &report);
        if (completed != (frame == pc::kWindowFrameCount ? 1 : 0))
            throw std::runtime_error("cost window closed on an unexpected frame");
    }
    return report;
}

struct SiteMask final { std::uint64_t low = 0, high = 0; };
SiteMask maskFor(std::initializer_list<std::uint16_t> sites) {
    SiteMask mask{};
    for (const auto site : sites) {
        if (site < 64) mask.low |= std::uint64_t{1} << site;
        else mask.high |= std::uint64_t{1} << (site - 64);
    }
    return mask;
}

bool exactCalls(const EdvrPluginCostWindowV1& report, std::uint64_t read,
                std::uint64_t work, std::uint64_t transfer, std::uint64_t state,
                SiteMask expectedMask, const char* label) {
    const auto& owner = report.owners[static_cast<std::uint8_t>(pc::Owner::Scanners)];
    const bool counts = owner.apiObserved == ((read + work + transfer + state) ? 1 : 0) &&
        owner.apiCalls[static_cast<std::uint8_t>(pc::ApiClass::ReadQuery)] == read &&
        owner.apiCalls[static_cast<std::uint8_t>(pc::ApiClass::Work)] == work &&
        owner.apiCalls[static_cast<std::uint8_t>(pc::ApiClass::Transfer)] == transfer &&
        owner.apiCalls[static_cast<std::uint8_t>(pc::ApiClass::State)] == state &&
        owner.apiCalls[static_cast<std::uint8_t>(pc::ApiClass::Instrumentation)] == 0;
    const bool sites = owner.apiSiteMask[0] == expectedMask.low &&
                       owner.apiSiteMask[1] == expectedMask.high;
    bool otherClear = true;
    for (std::uint8_t i = 0; i < EDVR_PLUGIN_COST_OWNER_COUNT; ++i) {
        if (i == static_cast<std::uint8_t>(pc::Owner::Scanners)) continue;
        for (std::uint8_t c = 0; c < EDVR_PLUGIN_COST_API_CLASS_COUNT; ++c)
            otherClear &= report.owners[i].apiCalls[c] == 0;
        otherClear &= report.owners[i].apiSiteMask[0] == 0 &&
                      report.owners[i].apiSiteMask[1] == 0;
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%s (literal classes and two-word site mask)", label);
    return check(counts && sites && otherClear, text);
}

bool reportHeader(const EdvrPluginCostWindowV1& report,
                  std::uint64_t samples = pc::kWindowFrameCount) {
    return check(report.version == pc::kWindowVersion && report.profileBit == 1 &&
                     report.firstFrame == 1 && report.lastFrame == pc::kWindowFrameCount &&
                     report.windowFrames == pc::kWindowFrameCount &&
                     report.completedApiSampleFrames == samples,
                 "report covers exactly 1800 frames and requested sample count");
}

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                           D3D11_MAP, UINT,
                                           D3D11_MAPPED_SUBRESOURCE*);
MapFn g_realMap = nullptr;
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
UnmapFn g_realUnmap = nullptr;
ID3D11DeviceContext* g_mappedContext = nullptr;
ID3D11Resource* g_mappedResource = nullptr;
UINT g_mappedSubresource = 0;
void* g_mappedData = nullptr;

void clearMappedObservation() noexcept {
    g_mappedContext = nullptr;
    g_mappedResource = nullptr;
    g_mappedSubresource = 0;
    g_mappedData = nullptr;
}
ID3D11Buffer* g_sceneBuffer = nullptr;
std::array<std::uint8_t, 5376> g_lastRevealMap{};
bool g_revealMapCaptured = false;
bool g_failRevealMap = false;
std::uint32_t g_revealMapAttempts = 0;

HRESULT STDMETHODCALLTYPE measuredMap(ID3D11DeviceContext* self, ID3D11Resource* resource,
                                      UINT subresource, D3D11_MAP type, UINT flags,
                                      D3D11_MAPPED_SUBRESOURCE* mapped) {
    const bool revealWrite = resource != g_sceneBuffer &&
                             type == D3D11_MAP_WRITE_DISCARD;
    if (revealWrite) {
        clearMappedObservation();
        ++g_revealMapAttempts;
        if (g_failRevealMap) {
            if (mapped) *mapped = {};
            return E_FAIL;
        }
    }
    const HRESULT hr = g_realMap(self, resource, subresource, type, flags, mapped);
    if (revealWrite && SUCCEEDED(hr) && mapped && mapped->pData) {
        g_mappedContext = self;
        g_mappedResource = resource;
        g_mappedSubresource = subresource;
        g_mappedData = mapped->pData;
    }
    return hr;
}

void STDMETHODCALLTYPE measuredUnmap(ID3D11DeviceContext* self,
                                     ID3D11Resource* resource, UINT subresource) {
    if (self == g_mappedContext && resource == g_mappedResource &&
        subresource == g_mappedSubresource && g_mappedData) {
        // Production has now copied its snapshot; WRITE_DISCARD data was
        // undefined at Map return. Inspect while mapped, then forward Unmap.
        std::memcpy(g_lastRevealMap.data(), g_mappedData, g_lastRevealMap.size());
        g_revealMapCaptured = true;
        clearMappedObservation();
    }
    g_realUnmap(self, resource, subresource);
}

bool installMapHook(edvr::VTableHook& hook, ID3D11DeviceContext* context) {
    clearMappedObservation();
    void* originalMap = nullptr;
    void* originalUnmap = nullptr;
    if (!hook.attach(context, 128) || !hook.setMode(edvr::HookMode::CopyVptr) ||
        !hook.replace(14, reinterpret_cast<void*>(&measuredMap), &originalMap) ||
        !hook.replace(15, reinterpret_cast<void*>(&measuredUnmap), &originalUnmap) ||
        !originalMap || !originalUnmap) return false;
    g_realMap = reinterpret_cast<MapFn>(originalMap);
    g_realUnmap = reinterpret_cast<UnmapFn>(originalUnmap);
    return hook.commit();
}

void updateShadowFromProduction(Fixture& fixture, std::uint8_t mappedValue,
                                std::uint8_t updateValue) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    requireHr(fixture.immediate->Map(fixture.sceneCb.Get(), 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped), "map scene CB for production tee");
    edvr::fssRevealNoteMap(fixture.sceneCb.Get(), mapped.pData);
    std::memset(mapped.pData, mappedValue, 5376);
    edvr::fssRevealNoteUnmap(fixture.sceneCb.Get());
    fixture.immediate->Unmap(fixture.sceneCb.Get(), 0);
    std::array<std::uint8_t, 5376> update{};
    update.fill(updateValue);
    edvr::fssRevealNoteUpdate(fixture.sceneCb.Get(), update.data());
}

bool actualStateRestored(Fixture& fixture) {
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* views[4]{};
    fixture.immediate->PSGetConstantBuffers(1, 1, &cb);
    fixture.immediate->PSGetShaderResources(0, 4, views);
    bool okay = cb == fixture.sceneCb.Get() && views[0] == fixture.srv.Get() &&
                !views[1] && !views[2] && !views[3];
    if (cb) cb->Release();
    for (auto* view : views) if (view) view->Release();
    return okay;
}

bool copiedTextureMatches(Fixture& fixture, ID3D11ShaderResourceView* view) {
    if (!view) return false;
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource) return false;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT query = resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    if (FAILED(query) || !texture) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    const HRESULT created = fixture.device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(created) || !staging) {
        texture->Release();
        return false;
    }
    fixture.immediate->CopyResource(staging.Get(), texture);
    texture->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(fixture.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) ||
        !mapped.pData) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    bool matches = true;
    for (UINT y = 0; y < 16; ++y) {
        const auto* row = bytes + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < 16 * 4; ++x) matches &= row[x] == 0x6D;
    }
    fixture.immediate->Unmap(staging.Get(), 0);
    return matches;
}

bool captureColdAndWarm(Fixture& fixture) {
    configure("off");
    edvr::fssRevealShutdown();
    edvr::fssRevealFrameBoundary();
    configure("sync");
    fixture.bind(fixture.immediate.Get());
    edvr::VTableHook mapHook;
    if (!check(installMapHook(mapHook, fixture.immediate.Get()),
               "install a real WARP Map observer for revealed CB bytes")) return false;
    g_sceneBuffer = fixture.sceneCb.Get();

    startCollector(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    updateShadowFromProduction(fixture, 0x51, 0xA7);
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    auto report = finishWindow();
    bool okay = reportHeader(report, pc::kWindowFrameCount);
    okay &= exactCalls(report, 9, 2, 1, 2,
        maskFor({53,54,55,56,57,58,59,60,61,62,63,64,65,72}),
        "cold first-eye learning and lockstep capture; snapshot waits for production write tee");

    edvr::fssRevealFrameBoundary();
    startCollector(fixture.immediate.Get());
    g_revealMapCaptured = false;
    g_revealMapAttempts = 0;
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    ID3D11Buffer* substitutedCb = nullptr;
    ID3D11ShaderResourceView* substitutedSrv = nullptr;
    fixture.immediate->PSGetConstantBuffers(1, 1, &substitutedCb);
    fixture.immediate->PSGetShaderResources(0, 1, &substitutedSrv);
    okay &= check(substitutedCb && substitutedCb != fixture.sceneCb.Get() &&
                      substitutedSrv && substitutedSrv != fixture.srv.Get(),
                  "production second eye binds a real snapshot CB and copied WARP SRV");
    okay &= check(copiedTextureMatches(fixture, substitutedSrv),
                  "actual copied lockstep WARP texture retains source pixel data");
    if (substitutedCb) substitutedCb->Release();
    if (substitutedSrv) substitutedSrv->Release();
    edvr::fssRevealEnd(fixture.immediate.Get());
    okay &= check(actualStateRestored(fixture),
                  "production End restores the actual WARP PS CB and SRV bindings");
    report = finishWindow();
    okay &= reportHeader(report);
    okay &= exactCalls(report, 9, 1, 3, 4,
        maskFor({53,55,56,57,58,59,63,64,65,66,67,68,69,70,71,72,73}),
        "cold scene-buffer creation on a warm lockstep texture pair");
    std::array<std::uint8_t, 5376> expected{};
    expected.fill(0xA7);
    okay &= check(g_revealMapCaptured && g_revealMapAttempts == 1 &&
                      g_lastRevealMap == expected,
                  "real production Map receives the bytes from the NoteMap/Unmap/Update shadow");

    edvr::fssRevealFrameBoundary();
    startCollector(fixture.immediate.Get());
    g_revealMapCaptured = false;
    g_revealMapAttempts = 0;
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    okay &= check(actualStateRestored(fixture), "warm repeated End restores the actual PS state");
    report = finishWindow();
    okay &= reportHeader(report);
    okay &= exactCalls(report, 8, 0, 3, 4,
        maskFor({53,55,56,57,58,59,63,64,65,68,69,70,71,72,73}),
        "warm repeated lockstep and scene snapshot path");
    okay &= check(g_revealMapCaptured && g_revealMapAttempts == 1 &&
                      g_lastRevealMap == expected,
                  "warm repeated Map preserves the actual production snapshot bytes");

    mapHook.uninstall();
    clearMappedObservation();
    g_realUnmap = nullptr;
    g_realMap = nullptr;
    g_sceneBuffer = nullptr;
    edvr::fssRevealFrameBoundary();
    edvrPluginCostShutdown();
    return okay;
}

bool steadyCosts(Fixture& fixture) {
    configure("off");
    edvr::fssRevealShutdown();
    edvr::fssRevealFrameBoundary();
    configure("steady");
    fixture.bind(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get()); // production scene-CB learning
    updateShadowFromProduction(fixture, 0x52, 0xB4);
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealFrameBoundary();
    edvr::VTableHook mapHook;
    if (!check(installMapHook(mapHook, fixture.immediate.Get()),
               "install WARP Map observer for steady scene substitution")) return false;
    g_sceneBuffer = fixture.sceneCb.Get();
    const auto run = [&](bool cold, const char* label) {
        startCollector(fixture.immediate.Get());
        g_revealMapCaptured = false;
        g_revealMapAttempts = 0;
        edvr::fssRevealBegin(fixture.immediate.Get());
        edvr::fssRevealEnd(fixture.immediate.Get());
        edvr::fssRevealBegin(fixture.immediate.Get());
        edvr::fssRevealEnd(fixture.immediate.Get());
        const bool restored = actualStateRestored(fixture);
        const auto report = finishWindow();
        bool okay = reportHeader(report) && restored &&
            exactCalls(report, cold ? 3 : 2, cold ? 1 : 0, 2, 2,
                       cold ? maskFor({53,66,67,68,69,70,71,73})
                            : maskFor({53,68,69,70,71,73}), label);
        std::array<std::uint8_t, 5376> expected{};
        expected.fill(0xB4);
        okay &= check(g_revealMapCaptured && g_revealMapAttempts == 1 &&
                          g_lastRevealMap == expected,
                      "steady Map exposes actual shadow bytes and End restores PS b1");
        edvr::fssRevealFrameBoundary();
        edvrPluginCostShutdown();
        return okay;
    };
    bool okay = run(true, "steady first buffer creation and CB restore");
    okay &= run(false, "steady warm CB Map and restore");
    mapHook.uninstall();
    clearMappedObservation();
    g_realUnmap = nullptr;
    g_realMap = nullptr;
    g_sceneBuffer = nullptr;
    return okay;
}

void resetReveal(Fixture& fixture, const char* mode) {
    edvr::fssRevealEnd(fixture.immediate.Get());
    configure("off");
    edvr::fssRevealShutdown();
    edvr::fssRevealFrameBoundary();
    fixture.bind(fixture.immediate.Get());
    configure(mode);
}

using GetResourceFn = void(STDMETHODCALLTYPE*)(ID3D11ShaderResourceView*,
                                               ID3D11Resource**);
std::uint32_t g_getResourceAttempts = 0;
void STDMETHODCALLTYPE nullGetResource(ID3D11ShaderResourceView*,
                                        ID3D11Resource** resource) {
    ++g_getResourceAttempts;
    if (resource) *resource = nullptr;
}

using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
std::uint32_t g_queryTextureAttempts = 0;
QueryInterfaceFn g_realQueryInterface = nullptr;
HRESULT STDMETHODCALLTYPE rejectTextureQuery(IUnknown* self, REFIID iid, void** out) {
    if (IsEqualGUID(iid, __uuidof(ID3D11Texture2D))) {
        ++g_queryTextureAttempts;
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    return g_realQueryInterface(self, iid, out);
}

using GetDeviceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceChild*, ID3D11Device**);
std::uint32_t g_getDeviceAttempts = 0;
void STDMETHODCALLTYPE nullGetDevice(ID3D11DeviceChild*, ID3D11Device** device) {
    ++g_getDeviceAttempts;
    if (device) *device = nullptr;
}

std::uint32_t g_createTextureAttempts = 0;
HRESULT STDMETHODCALLTYPE failCreateTexture(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                            const D3D11_SUBRESOURCE_DATA*,
                                            ID3D11Texture2D** texture) {
    ++g_createTextureAttempts;
    if (texture) *texture = nullptr;
    return E_OUTOFMEMORY;
}

bool apiPrefixFailures(Fixture& fixture) {
    bool okay = true;
    resetReveal(fixture, "sync");
    {
        edvr::VTableHook hook;
        void* original = nullptr;
        const bool hooked = hook.attach(fixture.srv.Get(), 9) &&
            hook.setMode(edvr::HookMode::CopyVptr) &&
            hook.replace(7, reinterpret_cast<void*>(&nullGetResource), &original) &&
            hook.commit();
        okay &= check(hooked, "hook the real WARP SRV GetResource slot");
        g_getResourceAttempts = 0;
        startCollector(fixture.immediate.Get());
        edvr::fssRevealBegin(fixture.immediate.Get());
        edvr::fssRevealEnd(fixture.immediate.Get());
        hook.uninstall();
        auto report = finishWindow();
        okay &= reportHeader(report);
        okay &= exactCalls(report, 5, 0, 0, 0, maskFor({53,54,55,56,57}),
                           "null GetResource output stops before QI and description");
        okay &= check(g_getResourceAttempts == 1,
                      "the null-producing GetResource call was actually attempted once");
        edvrPluginCostShutdown();
    }

    resetReveal(fixture, "sync");
    {
        edvr::VTableHook hook;
        void* original = nullptr;
        const bool hooked = hook.attach(static_cast<ID3D11Resource*>(fixture.texture.Get()), 8) &&
            hook.setMode(edvr::HookMode::CopyVptr) &&
            hook.replace(0, reinterpret_cast<void*>(&rejectTextureQuery), &original) &&
            hook.commit();
        g_realQueryInterface = reinterpret_cast<QueryInterfaceFn>(original);
        okay &= check(hooked, "hook actual WARP resource QueryInterface for HRESULT prefix");
        g_queryTextureAttempts = 0;
        startCollector(fixture.immediate.Get());
        edvr::fssRevealBegin(fixture.immediate.Get());
        edvr::fssRevealEnd(fixture.immediate.Get());
        hook.uninstall();
        auto report = finishWindow();
        okay &= reportHeader(report);
        okay &= exactCalls(report, 6, 0, 0, 0, maskFor({53,54,55,56,57,58}),
                           "failed Texture2D QI stops before desc and copy work");
        okay &= check(g_queryTextureAttempts == 1,
                      "the HRESULT-failing texture QI was attempted once");
        g_realQueryInterface = nullptr;
        edvrPluginCostShutdown();
    }

    resetReveal(fixture, "sync");
    {
        edvr::VTableHook hook;
        const bool hooked = hook.attach(fixture.immediate.Get(), 128) &&
            hook.setMode(edvr::HookMode::CopyVptr) &&
            hook.replace(3, reinterpret_cast<void*>(&nullGetDevice), nullptr) &&
            hook.commit();
        okay &= check(hooked, "hook WARP context GetDevice to return null");
        g_getDeviceAttempts = 0;
        startCollector(fixture.immediate.Get());
        edvr::fssRevealBegin(fixture.immediate.Get());
        edvr::fssRevealEnd(fixture.immediate.Get());
        hook.uninstall();
        auto report = finishWindow();
        okay &= reportHeader(report);
        okay &= exactCalls(report, 4, 0, 0, 0, maskFor({53,54,55,56}),
                           "null GetDevice result stops lockstep resource iteration");
        okay &= check(g_getDeviceAttempts == 1,
                      "the null-producing context GetDevice call was attempted once");
        edvrPluginCostShutdown();
    }

    resetReveal(fixture, "sync");
    {
        edvr::VTableHook hook;
        const bool hooked = hook.attach(fixture.device.Get(), 43) &&
            hook.setMode(edvr::HookMode::InPlace) &&
            hook.replace(5, reinterpret_cast<void*>(&failCreateTexture), nullptr) &&
            hook.commit();
        okay &= check(hooked, "hook WARP device CreateTexture2D for HRESULT prefix");
        g_createTextureAttempts = 0;
        startCollector(fixture.immediate.Get());
        edvr::fssRevealBegin(fixture.immediate.Get());
        edvr::fssRevealEnd(fixture.immediate.Get());
        hook.uninstall();
        auto report = finishWindow();
        okay &= reportHeader(report);
        okay &= exactCalls(report, 8, 1, 0, 0,
                           maskFor({53,54,55,56,57,58,59,60,61}),
                           "failed CreateTexture2D is counted while SRV creation and copy stay lazy");
        okay &= check(g_createTextureAttempts == 1,
                      "the failed WARP texture creation was attempted once");
        edvrPluginCostShutdown();
    }
    resetReveal(fixture, "off");
    return okay;
}

void primeSteady(Fixture& fixture) {
    resetReveal(fixture, "steady");
    edvr::fssRevealBegin(fixture.immediate.Get());
    updateShadowFromProduction(fixture, 0x64, 0xC2);
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealFrameBoundary();
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealFrameBoundary();
}

bool steadyMapFailure(Fixture& fixture) {
    primeSteady(fixture);
    // Retire only the output object; the production-learned scene pointer and
    // its shadow snapshot remain valid so the following Map is a cold create.
    edvr::fssRevealShutdown();
    edvr::VTableHook hook;
    if (!check(installMapHook(hook, fixture.immediate.Get()),
               "install WARP Map failure hook")) return false;
    g_sceneBuffer = fixture.sceneCb.Get();
    g_failRevealMap = true;
    g_revealMapAttempts = 0;
    startCollector(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    g_failRevealMap = false;
    hook.uninstall();
    clearMappedObservation();
    g_realUnmap = nullptr;
    auto report = finishWindow();
    bool okay = reportHeader(report);
    okay &= exactCalls(report, 2, 1, 1, 0, maskFor({53,66,67,68}),
                       "failed real Map stops before Unmap, getters, and state substitution");
    okay &= check(g_revealMapAttempts == 1,
                  "Map failure injection observes exactly one production attempt");
    okay &= check(actualStateRestored(fixture),
                  "failed Map leaves the real original scene CB bound");
    g_realMap = nullptr;
    g_sceneBuffer = nullptr;
    edvr::fssRevealFrameBoundary();
    edvrPluginCostShutdown();
    return okay;
}

enum class LatchClear { MismatchedEnd, NewBegin, Configure, Shutdown, FrameBoundary };

bool latchLifecycle(Fixture& fixture, LatchClear how, const char* label) {
    primeSteady(fixture);
    startCollector(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get()); // eye A
    if (how == LatchClear::NewBegin) {
        // The collector's frame gate turns off before occurrence 2. Its
        // Begin must replace the first occurrence's true sample latch.
        edvrPluginCostSetApiSampleFrame(0);
        edvr::fssRevealBegin(fixture.immediate.Get()); // eye B still applies
        // End must honor eye B's false latch even when the collector would
        // admit a fresh note now. Keep eye A's read in this sampled frame.
        edvrPluginCostSetApiSampleFrame(1);
        edvr::fssRevealEnd(fixture.immediate.Get());
    } else {
        edvr::fssRevealEnd(fixture.immediate.Get());
        edvr::fssRevealBegin(fixture.immediate.Get()); // eye B, actual CB apply
    }
    switch (how) {
    case LatchClear::MismatchedEnd:
        edvr::fssRevealEnd(fixture.deferred.Get());
        break;
    case LatchClear::NewBegin:
        break;
    case LatchClear::Configure:
        configure("steady");
        edvr::fssRevealEnd(fixture.immediate.Get());
        break;
    case LatchClear::Shutdown:
        edvr::fssRevealShutdown();
        edvr::fssRevealEnd(fixture.immediate.Get());
        break;
    case LatchClear::FrameBoundary:
        edvr::fssRevealFrameBoundary();
        edvr::fssRevealEnd(fixture.immediate.Get());
        break;
    }
    const bool actualRestored = actualStateRestored(fixture);
    const auto report = finishWindow();
    bool okay = reportHeader(report);
    if (how == LatchClear::NewBegin) {
        okay &= exactCalls(report, 1, 0, 0, 0, maskFor({53}), label);
    } else {
        okay &= exactCalls(report, 2, 0, 2, 1,
                           maskFor({53,68,69,70,71}), label);
    }
    if (how == LatchClear::MismatchedEnd) {
        // End runs against its supplied deferred context. The immediate
        // context remains substituted and is explicitly repaired below.
        ID3D11Buffer* current = nullptr;
        fixture.immediate->PSGetConstantBuffers(1, 1, &current);
        okay &= check(current && current != fixture.sceneCb.Get(),
                      "mismatched End leaves the owner context's real binding untouched");
        if (current) current->Release();
        ID3D11Buffer* original = fixture.sceneCb.Get();
        fixture.immediate->PSSetConstantBuffers(1, 1, &original);
    } else {
        okay &= check(actualRestored,
                      "lifecycle-cleared End still restores the actual WARP binding");
    }
    edvr::fssRevealFrameBoundary();
    configure("off");
    edvr::fssRevealShutdown();
    edvrPluginCostShutdown();
    fixture.bind(fixture.immediate.Get());
    return okay;
}

bool lifecycleAndMapFailure(Fixture& fixture) {
    bool okay = steadyMapFailure(fixture);
    okay &= latchLifecycle(fixture, LatchClear::MismatchedEnd,
                           "mismatched End does not inherit the owner's sample latch");
    okay &= latchLifecycle(fixture, LatchClear::NewBegin,
                           "new Begin replaces the prior pair's sample latch");
    okay &= latchLifecycle(fixture, LatchClear::Configure,
                           "configure clears the sample latch before actual CB restore");
    okay &= latchLifecycle(fixture, LatchClear::Shutdown,
                           "shutdown clears the sample latch before actual CB restore");
    okay &= latchLifecycle(fixture, LatchClear::FrameBoundary,
                           "frame boundary clears the sample latch before actual CB restore");
    return okay;
}

bool unsampledDeferredWorkerDisabled(Fixture& fixture) {
    bool okay = true;
    configure("steady");
    edvr::fssRevealFrameBoundary();
    startCollector(fixture.immediate.Get(), false);
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    auto report = finishWindow(0, true);
    okay &= reportHeader(report, 0);
    okay &= exactCalls(report, 0, 0, 0, 0, {},
                       "owner-context operations in unsampled frames emit no API notes");
    edvr::fssRevealFrameBoundary();
    edvrPluginCostShutdown();

    startCollector(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.deferred.Get());
    edvr::fssRevealEnd(fixture.deferred.Get());
    report = finishWindow();
    okay &= reportHeader(report);
    okay &= exactCalls(report, 0, 0, 0, 0, {},
                       "deferred-context Reveal calls are collector-filtered");
    edvr::fssRevealFrameBoundary();
    edvrPluginCostShutdown();

    startCollector(fixture.immediate.Get());
    std::thread worker([&] {
        edvr::fssRevealBegin(fixture.immediate.Get());
        edvr::fssRevealEnd(fixture.immediate.Get());
    });
    worker.join();
    report = finishWindow();
    okay &= reportHeader(report);
    okay &= exactCalls(report, 0, 0, 0, 0, {},
                       "foreign worker thread is rejected by the collector sample gate");
    edvr::fssRevealFrameBoundary();
    edvrPluginCostShutdown();

    configure("off");
    startCollector(fixture.immediate.Get());
    edvr::fssRevealBegin(fixture.immediate.Get());
    edvr::fssRevealEnd(fixture.immediate.Get());
    report = finishWindow();
    okay &= reportHeader(report);
    okay &= exactCalls(report, 0, 0, 0, 0, {},
                       "disabled Reveal exits before D3D calls and API sampling");
    edvr::fssRevealFrameBoundary();
    edvrPluginCostShutdown();
    return okay;
}

bool runSelfTest() {
    TempWorkingDirectory temporary;
    Fixture fixture;
    bool okay = captureColdAndWarm(fixture);
    okay &= steadyCosts(fixture);
    okay &= apiPrefixFailures(fixture);
    okay &= lifecycleAndMapFailure(fixture);
    okay &= unsampledDeferredWorkerDisabled(fixture);
    edvr::fssRevealShutdown();
    edvrPluginCostShutdown();
    return okay && g_failures == 0;
}

} // namespace

namespace edvr {
Config& Config::get() { static Config config; return config; }
std::string Config::getString(const char*, const char*) const { return ::g_configValue; }
Log& Log::get() { static Log logger; return logger; }
Log::~Log() {}
void Log::note(const char*, ...) {}
// The real guard/vtable implementations retain their fault semantics. This
// standalone rig suppresses only their emergency diagnostic file boundary.
void breadcrumb(const char*) {}
std::int64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
std::int64_t qpcFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}
std::uint64_t lookupShaderHash(void*) { return 0; }
} // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("fss_reveal_api_test: dry-run (no device or collector access)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: fss_reveal_api_test.exe --dry-run|--self-test");
        return 2;
    }
    try {
        if (!runSelfTest()) {
            std::printf("fss_reveal_api_test: %u of %u checks failed\n",
                        g_failures, g_checks);
            return 1;
        }
        std::printf("fss_reveal_api_test: %u checks passed\n", g_checks);
        return 0;
    } catch (const std::exception& error) {
        edvr::fssRevealEnd(nullptr);
        edvr::fssRevealShutdown();
        edvrPluginCostShutdown();
        std::printf("fss_reveal_api_test: failed: %s\n", error.what());
        return 1;
    }
}
