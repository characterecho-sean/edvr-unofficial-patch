// System32 WARP coverage for loader-panel staging capture and readback costs.
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "../../src/common/config.h"
#include "../../src/common/plugin_cost.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/loader_panel.h"
#include "../../src/d3d11/loader_panel_cost_sites.h"

using Microsoft::WRL::ComPtr;
namespace pc = edvr::plugin_cost;

namespace {
unsigned g_failures = 0;
bool check(bool value, const char* what) {
    if (!value) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
    return value;
}

void requireHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(what);
}

struct Vertex {
    float x, y;
    uint32_t rgba;
    uint8_t idx1;
    uint8_t padding[3];
    uint8_t idx2;
    uint8_t tail[7];
};
static_assert(sizeof(Vertex) == 24, "fixture matches the loader's measured 24-byte vertex stride");

struct Fixture {
    HMODULE systemD3d11 = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11Buffer> indices;
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11Buffer> table;
    ComPtr<ID3D11ShaderResourceView> tableSrv;
    ComPtr<ID3D11Buffer> constants;

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
        requireHr(device->CreateDeferredContext(0, &deferred), "create WARP deferred context");
        createInputs();
        bind(immediate.Get());
        bind(deferred.Get());
    }

    ~Fixture() {
        edvr::loaderPanelShutdown();
        deferred.Reset();
        immediate.Reset();
        device.Reset();
        // The injected GetDevice SEH path intentionally demonstrates the
        // production lambda's partial-prefix cleanup behavior. Keep System32
        // d3d11 pinned until this short-lived rig process exits.
    }

    void createInputs() {
        const uint16_t indicesData[30] = {
            0, 1, 2, 2, 1, 3, 0, 1, 2, 2, 1, 3, 0, 1, 2,
            2, 1, 3, 0, 1, 2, 2, 1, 3, 0, 1, 2, 2, 1, 3
        };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(indicesData);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = indicesData;
        requireHr(device->CreateBuffer(&bd, &init, &indices), "create real WARP index buffer");

        const Vertex vertexData[4] = {
            {-1.0f, -1.0f, 0x66000000u, 0, {}, 0, {}},
            {-1.0f,  1.0f, 0x66000000u, 0, {}, 0, {}},
            { 1.0f, -1.0f, 0x66000000u, 0, {}, 0, {}},
            { 1.0f,  1.0f, 0x66000000u, 0, {}, 0, {}},
        };
        bd = {};
        bd.ByteWidth = sizeof(vertexData);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        init = {};
        init.pSysMem = vertexData;
        requireHr(device->CreateBuffer(&bd, &init, &vertices), "create real WARP vertex buffer");

        float rows[40]{};
        rows[0] = 1.0f;
        rows[5] = 1.0f;
        bd = {};
        bd.ByteWidth = sizeof(rows);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 160;
        init = {};
        init.pSysMem = rows;
        requireHr(device->CreateBuffer(&bd, &init, &table), "create real structured widget table");
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = 1;
        requireHr(device->CreateShaderResourceView(table.Get(), &srvDesc, &tableSrv),
                  "create actual widget-table SRV");

        uint8_t cbData[48]{};
        const uint32_t flags = 0x4000u;
        std::memcpy(cbData + 32, &flags, sizeof(flags));
        bd = {};
        bd.ByteWidth = sizeof(cbData);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        init = {};
        init.pSysMem = cbData;
        requireHr(device->CreateBuffer(&bd, &init, &constants), "create actual VS b2 constants");
    }

    void bind(ID3D11DeviceContext* context) {
        const UINT stride = sizeof(Vertex), offset = 0;
        context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
        ID3D11Buffer* vb = vertices.Get();
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ID3D11ShaderResourceView* srv = tableSrv.Get();
        context->VSSetShaderResources(0, 1, &srv);
        ID3D11Buffer* cb = constants.Get();
        context->VSSetConstantBuffers(2, 1, &cb);
    }
};

uint64_t allLoaderSites() {
    uint64_t mask = 0;
    for (uint16_t id = 91; id <= 110; ++id)
        mask |= uint64_t{1} << (id - 64);
    return mask;
}

void startCollector(ID3D11DeviceContext* owner) {
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(1, 1000000);
    edvrPluginCostSetOwnerContext(owner);
    EdvrPluginCostWindowV1 discarded{};
    if (edvrPluginCostFrameBoundary(0, 0, 0, 1, 0, &discarded))
        throw std::runtime_error("owner bootstrap unexpectedly reported a window");
    if (!edvrPluginCostApiSampleContext(owner))
        throw std::runtime_error("registered immediate context is not sample eligible");
}

EdvrPluginCostWindowV1 finishWindow(uint32_t first = 1) {
    EdvrPluginCostWindowV1 report{};
    uint8_t completed = edvrPluginCostFrameBoundary(first, 0, 1, 1, 0, &report);
    if (completed) throw std::runtime_error("first boundary unexpectedly closed the 1800-frame window");
    for (uint32_t frame = first + 1; frame < first + 1799; ++frame) {
        completed = edvrPluginCostFrameBoundary(frame, 0, 1, 1, 0, &report);
        if (completed) throw std::runtime_error("cost window closed before its 1800th sample frame");
    }
    completed = edvrPluginCostFrameBoundary(first + 1799, 0, 1, 1, 0, &report);
    if (!completed) throw std::runtime_error("cost report did not close after exactly 1800 frames");
    return report;
}

bool reportHeader(const EdvrPluginCostWindowV1& report) {
    return check(report.version == pc::kWindowVersion && report.profileBit == 1 &&
                 report.firstFrame == 1 && report.lastFrame == 1800 &&
                 report.windowFrames == 1800 && report.completedApiSampleFrames == 1800,
                 "report denominator is exactly 1800 completed API-sample frames");
}

const EdvrPluginCostOwnerV1& introOwner(const EdvrPluginCostWindowV1& report) {
    return report.owners[static_cast<uint8_t>(pc::Owner::Intro)];
}

bool exactOwnerCalls(const EdvrPluginCostWindowV1& report,
                     uint64_t readQuery, uint64_t work, uint64_t transfer,
                     uint64_t mask, const char* label) {
    const auto& owner = introOwner(report);
    char text[256];
    const uint8_t read = static_cast<uint8_t>(pc::ApiClass::ReadQuery);
    const uint8_t workIx = static_cast<uint8_t>(pc::ApiClass::Work);
    const uint8_t transferIx = static_cast<uint8_t>(pc::ApiClass::Transfer);
    const bool calls = owner.apiCalls[read] == readQuery &&
                       owner.apiCalls[workIx] == work &&
                       owner.apiCalls[transferIx] == transfer &&
                       owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 0 &&
                       owner.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 0;
    const bool masks = owner.apiSiteMask[0] == 0 && owner.apiSiteMask[1] == mask;
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s (exact class counts and Intro site mask)", label);
    return check(calls && masks, text);
}

constexpr char kKind = 'X';
constexpr uint32_t kCount = 30;
constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 480;

bool panelDraw(ID3D11DeviceContext* context, bool textured = false) {
    return edvr::loaderPanelOnDraw(context, kKind, kCount, 1, 0, 0,
                                   kWidth, kHeight, textured);
}

void resetPanelFixture(ID3D11DeviceContext* context) {
    // Shutdown resets measurements, but deliberately leaves the current frame's
    // ordinal/first-panel state. Close that frame through the real disabled tick
    // before starting a fresh lifecycle; it performs no capture/readback calls.
    edvr::loaderPanelShutdown();
    edvr::detail::g_loaderPanelOn = false;
    edvr::loaderPanelTick(context, false);
    edvr::detail::g_loaderPanelOn = true;
}

void armCollection(ID3D11DeviceContext* context) {
    if (!panelDraw(context))
        throw std::runtime_error("first panel did not enter the production speculative path");
    if (!edvr::loaderPanelSubstitute(context, nullptr, 1, 0))
        throw std::runtime_error("speculative substitute was not consumed by production path");
    edvr::loaderPanelTick(context, false);
}

void waitForCopies(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_QUERY_DESC desc{};
    desc.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> event;
    requireHr(device->CreateQuery(&desc, &event), "create WARP copy-completion event");
    context->End(event.Get());
    context->Flush();
    BOOL complete = FALSE;
    while (context->GetData(event.Get(), &complete, sizeof(complete), 0) == S_FALSE)
        Sleep(0);
    if (!complete) throw std::runtime_error("WARP event query did not complete");
}

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                                   const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
CreateBufferFn g_realCreateBuffer = nullptr;
uint32_t g_createBufferHits = 0;
HRESULT STDMETHODCALLTYPE countCreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc,
                                             const D3D11_SUBRESOURCE_DATA* initial,
                                             ID3D11Buffer** out) {
    ++g_createBufferHits;
    return g_realCreateBuffer(self, desc, initial, out);
}

using IaGetVertexBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                       ID3D11Buffer**, UINT*, UINT*);
using IaGetIndexBufferFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer**,
                                                     DXGI_FORMAT*, UINT*);
IaGetVertexBuffersFn g_realIaGetVertexBuffers = nullptr;
IaGetIndexBufferFn g_realIaGetIndexBuffer = nullptr;
uint32_t g_iaGetVertexBuffersHits = 0;
uint32_t g_iaGetIndexBufferHits = 0;
void STDMETHODCALLTYPE countIaGetVertexBuffers(ID3D11DeviceContext* self, UINT start, UINT count,
                                                ID3D11Buffer** buffers, UINT* strides, UINT* offsets) {
    ++g_iaGetVertexBuffersHits;
    g_realIaGetVertexBuffers(self, start, count, buffers, strides, offsets);
}
void STDMETHODCALLTYPE countIaGetIndexBuffer(ID3D11DeviceContext* self, ID3D11Buffer** buffer,
                                              DXGI_FORMAT* format, UINT* offset) {
    ++g_iaGetIndexBufferHits;
    g_realIaGetIndexBuffer(self, buffer, format, offset);
}

struct CreateBufferObserver {
    edvr::VTableHook hook;
    bool attach(ID3D11Device* device) {
        g_createBufferHits = 0;
        return hook.attach(device, 128) && hook.setMode(edvr::HookMode::CopyVptr) &&
               hook.replace(3, reinterpret_cast<void*>(&countCreateBuffer),
                            reinterpret_cast<void**>(&g_realCreateBuffer)) && hook.commit();
    }
    void detach() { hook.uninstall(); }
};

struct DeferredQueryObserver {
    edvr::VTableHook hook;
    bool attach(ID3D11DeviceContext* context) {
        g_iaGetVertexBuffersHits = 0;
        g_iaGetIndexBufferHits = 0;
        return hook.attach(context, 128) && hook.setMode(edvr::HookMode::CopyVptr) &&
               // ID3D11DeviceChild occupies slots 3-6; the D3D11 context
               // declaration places IAGetVertexBuffers/IAGetIndexBuffer at 79/80.
               // Slots 32/33 are GSSetSamplers/OMSetRenderTargets, with different ABIs.
               hook.replace(79, reinterpret_cast<void*>(&countIaGetVertexBuffers),
                            reinterpret_cast<void**>(&g_realIaGetVertexBuffers)) &&
               hook.replace(80, reinterpret_cast<void*>(&countIaGetIndexBuffer),
                            reinterpret_cast<void**>(&g_realIaGetIndexBuffer)) && hook.commit();
    }
    void detach() { hook.uninstall(); }
};

uint32_t g_getDeviceFaults = 0;
void STDMETHODCALLTYPE faultGetDevice(ID3D11DeviceContext*, ID3D11Device**) {
    ++g_getDeviceFaults;
    RaiseException(0xE042ED92u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                          D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
MapFn g_realMap = nullptr;
uint32_t g_mapFaults = 0;
HRESULT STDMETHODCALLTYPE faultFirstMap(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                        D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*) {
    ++g_mapFaults;
    RaiseException(0xE042ED93u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return E_FAIL;
}

bool noOwnerCharges(const EdvrPluginCostWindowV1& report, const char* label) {
    const auto& owner = introOwner(report);
    bool zero = true;
    for (uint8_t i = 0; i < EDVR_PLUGIN_COST_API_CLASS_COUNT; ++i)
        zero &= owner.apiCalls[i] == 0;
    zero &= owner.apiSiteMask[0] == 0 && owner.apiSiteMask[1] == 0;
    return check(zero, label);
}

bool successfulCapture(Fixture& fixture, bool injectMapFault = false) {
    resetPanelFixture(fixture.immediate.Get());
    edvr::detail::g_loaderPanelOn = true;
    fixture.bind(fixture.immediate.Get());
    startCollector(fixture.immediate.Get());
    armCollection(fixture.immediate.Get());

    const bool speculative = panelDraw(fixture.immediate.Get());
    if (!check(speculative, "admitted capture reports the existing speculative substitute")) return false;
    check(edvr::loaderPanelSubstitute(fixture.immediate.Get(), nullptr, 1, 0),
          "production substitute consumes the admitted capture's draw arm");
    waitForCopies(fixture.device.Get(), fixture.immediate.Get());

    if (injectMapFault) {
        edvr::VTableHook hook;
        if (!check(hook.attach(fixture.immediate.Get(), 128), "attach actual WARP context for Map fault")) return false;
        if (!check(hook.setMode(edvr::HookMode::CopyVptr), "select private context vtable copy")) return false;
        if (!check(hook.replace(14, reinterpret_cast<void*>(&faultFirstMap),
                               reinterpret_cast<void**>(&g_realMap)),
                   "stage typed ID3D11DeviceContext::Map slot 14 fault")) return false;
        if (!check(hook.commit(), "commit first-Map fault hook")) return false;
        g_mapFaults = 0;
        edvr::loaderPanelTick(fixture.immediate.Get(), false);
        hook.uninstall();
        check(g_mapFaults == 1, "one attempted first Map fault is absorbed");
        const auto faultReport = finishWindow();
        bool ok = reportHeader(faultReport);
        ok &= exactOwnerCalls(faultReport, 10, 4, 5,
                              allLoaderSites() & ~(uint64_t{1} << (110 - 64)),
                              "first-Map fault records capture plus failed Map, with no unmap fabricated");

        // Keep the pending production readback, but start a fresh collector so
        // its successful Map/Unmap retry has a separate exact report.
        startCollector(fixture.immediate.Get());
        edvr::loaderPanelTick(fixture.immediate.Get(), false);
        panelDraw(fixture.immediate.Get());
        const auto retryReport = finishWindow();
        ok &= reportHeader(retryReport);
        const uint64_t mapUnmapMask = (uint64_t{1} << (109 - 64)) |
                                      (uint64_t{1} << (110 - 64));
        ok &= exactOwnerCalls(retryReport, 0, 0, 8, mapUnmapMask,
                              "readback recovery counts four Maps and four Unmaps only");
        edvr::loaderPanelShutdown();
        edvrPluginCostShutdown();
        return ok;
    } else {
        edvr::loaderPanelTick(fixture.immediate.Get(), false);
    }

    // A repeated, already measured shape takes the warm path: the production
    // classifier runs, but no capture/readback D3D calls are fabricated.
    panelDraw(fixture.immediate.Get());
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= exactOwnerCalls(report, 10, 4, 12, allLoaderSites(),
                          "cold loader staging capture and completed readback");
    const auto& core = report.owners[static_cast<uint8_t>(pc::Owner::Core)];
    for (uint8_t i = 0; i < EDVR_PLUGIN_COST_API_CLASS_COUNT; ++i)
        ok &= check(core.apiCalls[i] == 0,
                    "direct loader staging APIs are not double-counted under Core");
    edvr::loaderPanelShutdown();
    edvrPluginCostShutdown();
    return ok;
}

bool declineWindow(Fixture& fixture) {
    resetPanelFixture(fixture.immediate.Get());
    edvr::detail::g_loaderPanelOn = false;
    startCollector(fixture.immediate.Get());
    check(!panelDraw(fixture.immediate.Get()), "disabled loader feature declines before capture");
    edvr::detail::g_loaderPanelOn = true;
    check(!edvr::loaderPanelOnDraw(fixture.immediate.Get(), 'N', kCount, 1, 0, 0,
                                   kWidth, kHeight, false),
          "wrong draw kind declines before capture");
    armCollection(fixture.immediate.Get());
    // Texturing excludes staging capture, but the first panel is still
    // withheld speculatively by the independent production state machine.
    check(panelDraw(fixture.immediate.Get(), true),
          "textured panel keeps speculative withholding during collection");
    check(edvr::loaderPanelSubstitute(fixture.immediate.Get(), nullptr, 1, 0),
          "textured speculative panel consumes its actual substitute arm");
    edvr::loaderPanelTick(fixture.immediate.Get(), false);
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= noOwnerCharges(report, "disabled, wrong-kind, and textured capture exclusions produce no API notes");
    edvr::loaderPanelShutdown();
    edvrPluginCostShutdown();
    return ok;
}

bool foreignOrDeferredWindow(Fixture& fixture, bool deferred) {
    resetPanelFixture(fixture.immediate.Get());
    edvr::detail::g_loaderPanelOn = true;
    fixture.bind(fixture.immediate.Get());
    fixture.bind(fixture.deferred.Get());
    startCollector(fixture.immediate.Get());
    armCollection(fixture.immediate.Get());
    check(edvrPluginCostApiSampleContext(deferred ? fixture.deferred.Get() : fixture.immediate.Get()) ==
              (deferred ? 0 : 1),
          deferred ? "deferred context fails the registered immediate-context gate"
                   : "owner immediate context remains eligible before foreign-worker capture");

    CreateBufferObserver observer;
    if (!check(observer.attach(fixture.device.Get()), "observe actual WARP staging allocations")) return false;
    bool result = false;
    if (deferred) {
        DeferredQueryObserver queryObserver;
        if (!check(queryObserver.attach(fixture.deferred.Get()),
                   "observe actual deferred-context IA state queries")) return false;
        result = panelDraw(fixture.deferred.Get());
        queryObserver.detach();
        check(g_iaGetVertexBuffersHits == 1 && g_iaGetIndexBufferHits == 1,
              "deferred production call reaches both actual IA getters once");
        ComPtr<ID3D11CommandList> commands;
        requireHr(fixture.deferred->FinishCommandList(FALSE, &commands),
                  "finish actual deferred capture copies");
        fixture.immediate->ExecuteCommandList(commands.Get(), FALSE);
        check(commands.Get() != nullptr, "deferred capture records real D3D commands");
    } else {
        std::thread worker([&] { result = panelDraw(fixture.immediate.Get()); });
        worker.join();
    }
    observer.detach();
    check(result, deferred ? "deferred capture enters the real production panel path"
                           : "foreign worker enters the real production panel path");
    if (!deferred) {
        check(g_createBufferHits >= 2,
              "worker capture creates real WARP staging buffers");
    } else if (g_createBufferHits >= 2) {
        check(true, "deferred context returned bound resources and created WARP staging buffers");
    } else {
        std::puts("loader_panel_api_test: deferred capture stopped after the two real IA getters returned no resources");
    }
    edvr::loaderPanelShutdown();
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= noOwnerCharges(report, deferred
        ? "deferred capture has actual resource work and zero immediate-owner charges"
        : "foreign-worker capture has actual resource work and zero owner-thread charges");
    edvrPluginCostShutdown();
    return ok;
}

bool getDeviceFaultPrefix(Fixture& fixture) {
    resetPanelFixture(fixture.immediate.Get());
    edvr::detail::g_loaderPanelOn = true;
    fixture.bind(fixture.immediate.Get());
    startCollector(fixture.immediate.Get());
    armCollection(fixture.immediate.Get());
    edvr::VTableHook hook;
    if (!check(hook.attach(fixture.immediate.Get(), 128), "attach WARP context GetDevice fault hook")) return false;
    if (!check(hook.setMode(edvr::HookMode::CopyVptr), "select private GetDevice hook table")) return false;
    if (!check(hook.replace(3, reinterpret_cast<void*>(&faultGetDevice), nullptr),
               "stage ID3D11DeviceChild::GetDevice slot 3 fault")) return false;
    if (!check(hook.commit(), "commit typed GetDevice fault")) return false;
    g_getDeviceFaults = 0;
    panelDraw(fixture.immediate.Get());
    hook.uninstall();
    check(g_getDeviceFaults == 1, "exactly one production GetDevice attempt faults");
    const auto report = finishWindow();
    bool ok = reportHeader(report);
    ok &= exactOwnerCalls(report, 3, 0, 0,
                          (uint64_t{1} << (91 - 64)) |
                          (uint64_t{1} << (92 - 64)) |
                          (uint64_t{1} << (93 - 64)),
                          "guarded GetDevice fault preserves the three-query attempted prefix only");
    edvr::loaderPanelShutdown();
    edvrPluginCostShutdown();
    return ok;
}

} // namespace

namespace edvr {
Log& Log::get() { static Log logger; return logger; }
Log::~Log() {}
void Log::note(const char*, ...) {}
int64_t qpcNow() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }
std::string Config::getString(const char*, const char* def) const { return def; }
void breadcrumb(const char*) {}
} // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("loader_panel_api_test: dry-run (no module, device, logger, or file access)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::fprintf(stderr, "usage: loader_panel_api_test --dry-run | --self-test\n");
        return 2;
    }
    try {
        Fixture fixture;
        edvr::detail::g_loaderPanelOn = false;
        bool ok = declineWindow(fixture);
        ok &= successfulCapture(fixture);
        ok &= successfulCapture(fixture, true);
        ok &= foreignOrDeferredWindow(fixture, false);
        ok &= foreignOrDeferredWindow(fixture, true);
        ok &= getDeviceFaultPrefix(fixture);
        // The single injected GetDevice fault is followed by a full successful
        // cold capture, proving the existing loader guard budget remains usable.
        ok &= successfulCapture(fixture);
        edvr::loaderPanelShutdown();
        edvrPluginCostShutdown();
        std::printf("loader_panel_api_test: %s (%u failures)\n",
                    ok && g_failures == 0 ? "PASS" : "FAILED", g_failures);
        return ok && g_failures == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "loader_panel_api_test: %s\n", e.what());
        edvr::loaderPanelShutdown();
        edvrPluginCostShutdown();
        return 1;
    }
}
