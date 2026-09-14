#include "graphics_bridge.h"
#include "render_boundary.h"

#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/native_graphics.h"
#include "../common/vtable_hook.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;
constexpr size_t kSlotExecuteCommandList = 58;
using PFN_ExecuteCommandList = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext *,
                                                           ID3D11CommandList *, BOOL);

struct Owner {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::atomic<bool> hookReady{false};
    std::atomic<bool> leaseLive{false};
};

struct Transport {
    edvr::VTableHook hook;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    PFN_ExecuteCommandList realExecuteCommandList = nullptr;
    std::atomic<bool> active{false};
};

std::mutex g_ownerMutex;
Owner* g_owner = nullptr;
std::atomic<uint64_t> g_privateExecutions{0};
std::atomic<uint64_t> g_unknownExecutions{0};
std::mutex g_transportMutex;
std::atomic<Transport*> g_transport{nullptr};
bool g_transportAttempted = false;

struct Lease {
    Owner* owner;
    DWORD thread;
    std::atomic_flag executing = ATOMIC_FLAG_INIT;
};

struct Permit {
    ID3D11DeviceContext* context = nullptr;
    ID3D11CommandList* list = nullptr;
    bool consumed = false;
};

thread_local Permit g_permit;

struct ExecutionScope {
    Lease* lease;
    ~ExecutionScope() {
        g_permit = {};
        lease->executing.clear(std::memory_order_release);
    }
};

void STDMETHODCALLTYPE hookedTransportExecuteCommandList(ID3D11DeviceContext* self,
                                                          ID3D11CommandList* list,
                                                          BOOL restoreContextState) {
    Transport* transport = g_transport.load(std::memory_order_acquire);
    if (!transport || !transport->realExecuteCommandList) return;

    // InPlace mode shares the context table with every context of that type.
    // Only the exact immediate context that was admitted as the owner may
    // consume a private permit or affect bridge diagnostics.
    const bool ownerContext = transport->active.load(std::memory_order_acquire) &&
                              self == transport->context.Get();
    if (ownerContext) {
        if (!edvr::graphicsBridgeConsumePermit(self, list, restoreContextState))
            edvr::graphicsBridgeNoteUnknownExecution();
    }
    // The transport is deliberately pass-through: one original call, once,
    // after the owner-context bookkeeping above.
    transport->realExecuteCommandList(self, list, restoreContextState);
}

void noteTransport(const char* result) noexcept {
    try {
        edvr::Log::get().note("graphics bridge: minimal ExecuteCommandList transport %s",
                              result);
    } catch (...) {
    }
}

bool identity(IUnknown* object, ComPtr<IUnknown>& result) {
    result.Reset();
    return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&result)));
}

bool sameIdentity(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> ai, bi;
    return identity(a, ai) && identity(b, bi) && ai.Get() == bi.Get();
}

void clearTable(EdvrGraphicsBridgeTable* table) {
    if (!table) return;
    const uint32_t supplied = table->size;
    if (supplied >= sizeof(uint32_t)) table->size = 0;
    if (supplied >= sizeof(uint32_t) * 2) table->version = 0;
    if (supplied >= offsetof(EdvrGraphicsBridgeTable, lease) + sizeof(table->lease)) table->lease = nullptr;
    if (supplied >= offsetof(EdvrGraphicsBridgeTable, execute) + sizeof(table->execute)) table->execute = nullptr;
    if (supplied >= sizeof(EdvrGraphicsBridgeTable)) table->release = nullptr;
}

HRESULT WINAPI executeLease(void* opaque, ID3D11CommandList* list) {
    Lease* lease = static_cast<Lease*>(opaque);
    if (!lease || !list || GetCurrentThreadId() != lease->thread) return E_ACCESSDENIED;
    if (lease->executing.test_and_set(std::memory_order_acquire)) return E_PENDING;
    ExecutionScope scope{lease};
    g_permit = {};
    try {
        Owner* owner = lease->owner;
        if (!owner || !owner->hookReady.load(std::memory_order_acquire)) return E_NOINTERFACE;
        const HRESULT removed = owner->device->GetDeviceRemovedReason();
        if (FAILED(removed)) return removed;
        ComPtr<ID3D11Device> listDevice;
        list->GetDevice(&listDevice);
        if (!listDevice ||
            !sameIdentity(listDevice.Get(), owner->device.Get())) return E_INVALIDARG;

        g_permit.context = owner->context.Get();
        g_permit.list = list;
        owner->context->ExecuteCommandList(list, TRUE);
        // Work may have reached the driver even if a later hook bypassed ours.
        // Submit it before reporting the missing capability, with no retry.
        owner->context->Flush();
        return g_permit.consumed ? owner->device->GetDeviceRemovedReason() : E_NOINTERFACE;
    } catch (...) {
        return E_FAIL;
    }
}

void WINAPI releaseLease(void* opaque) {
    Lease* lease = static_cast<Lease*>(opaque);
    if (!lease) return;
    Owner* owner = lease->owner;
    owner->leaseLive.store(false, std::memory_order_release);
    delete lease;
}

} // namespace

namespace edvr {

bool graphicsBridgeRegisterOwner(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    ComPtr<ID3D11DeviceContext> immediate;
    device->GetImmediateContext(&immediate);
    if (!immediate || !sameIdentity(immediate.Get(), context)) return false;
    std::lock_guard<std::mutex> lock(g_ownerMutex);
    if (g_owner) return false;
    Owner* owner = new (std::nothrow) Owner();
    if (!owner) return false;
    owner->device = device;
    owner->context = context;
    // The hook already retains its first context for the process lifetime.
    // Mirror that lifetime so discovery cannot return recycled COM addresses.
    owner->hookReady.store(true, std::memory_order_release);
    g_owner = owner;
    // Present-boundary discovery shares this exact, already-hooked owner.
    // Its failure must not change the established graphics bridge behavior.
    (void)renderBoundaryRegisterOwner(device, context);
    return true;
}

bool graphicsBridgeInstallTransport(ID3D11Device* device, HookMode mode) {
    std::lock_guard<std::mutex> lock(g_transportMutex);
    if (g_transportAttempted) return false;
    g_transportAttempted = true;
    if (!device) {
        noteTransport("unavailable (no device)");
        return false;
    }
    {
        std::lock_guard<std::mutex> ownerLock(g_ownerMutex);
        if (g_owner) {
            noteTransport("unavailable (graphics owner already registered)");
            return false;
        }
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        noteTransport("unavailable (no immediate context)");
        return false;
    }

    Transport* transport = new (std::nothrow) Transport();
    if (!transport) {
        context->Release();
        noteTransport("unavailable (allocation failed)");
        return false;
    }
    transport->device = device;
    transport->context = context;
    const bool usable = transport->hook.attach(context) &&
                        transport->hook.executablePrefix() > kSlotExecuteCommandList;
    if (!usable || !transport->hook.setMode(mode) ||
        !transport->hook.replace(kSlotExecuteCommandList,
                                 &hookedTransportExecuteCommandList,
                                 reinterpret_cast<void**>(&transport->realExecuteCommandList)) ||
        !transport->realExecuteCommandList) {
        transport->hook.uninstall();
        context->Release();
        noteTransport("unavailable (context vtable unusable)");
        return false;
    }

    transport->active.store(true, std::memory_order_release);
    // replace() has captured the exact forward before an InPlace commit can
    // expose the thunk. Publish the immutable state before that commit so a
    // concurrent game call can never observe an installed thunk without it.
    g_transport.store(transport, std::memory_order_release);
    if (!transport->hook.commit()) {
        transport->active.store(false, std::memory_order_release);
        transport->hook.uninstall();
        context->Release();
        noteTransport("unavailable (vtable commit failed)");
        return false;
    }
    if (!graphicsBridgeRegisterOwner(device, context)) {
        transport->active.store(false, std::memory_order_release);
        transport->hook.uninstall();
        context->Release();
        noteTransport("unavailable (owner registration failed)");
        return false;
    }
    context->Release();
    noteTransport("installed");
    return true;
}

void graphicsBridgeUninstallTransport() {
    std::lock_guard<std::mutex> lock(g_transportMutex);
    Transport* transport = g_transport.load(std::memory_order_acquire);
    if (!transport) return;
    {
        std::lock_guard<std::mutex> ownerLock(g_ownerMutex);
        if (g_owner && g_owner->device.Get() == transport->device.Get() &&
            g_owner->context.Get() == transport->context.Get()) {
            // Native acquisition must stop before the thunk is removed.
            g_owner->hookReady.store(false, std::memory_order_release);
        }
    }
    transport->active.store(false, std::memory_order_release);
    transport->hook.uninstall();
}

bool graphicsBridgeConsumePermit(ID3D11DeviceContext* context,
                                 ID3D11CommandList* list,
                                 BOOL restoreContextState) {
    if (restoreContextState != TRUE || !context || !list || g_permit.consumed ||
        g_permit.context != context || g_permit.list != list) return false;
    g_permit.consumed = true;
    g_privateExecutions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void graphicsBridgeNoteUnknownExecution() {
    g_unknownExecutions.fetch_add(1, std::memory_order_relaxed);
}

uint64_t graphicsBridgePrivateExecutionCount() {
    return g_privateExecutions.load(std::memory_order_relaxed);
}

uint64_t graphicsBridgeUnknownExecutionCount() {
    return g_unknownExecutions.load(std::memory_order_relaxed);
}

} // namespace edvr

extern "C" HRESULT WINAPI edvrAcquireGraphicsBridge(
    const EdvrGraphicsBridgeRequest* request, EdvrGraphicsBridgeTable* table) try {
    const uint32_t tableSize = table ? table->size : 0;
    const uint32_t tableVersion = table && tableSize >= sizeof(uint32_t) * 2
                                      ? table->version : 0;
    if (table) clearTable(table);
    if (!request || !table || request->size != sizeof(EdvrGraphicsBridgeRequest) ||
        request->version != EDVR_GRAPHICS_BRIDGE_VERSION_1 ||
        tableSize != sizeof(EdvrGraphicsBridgeTable) ||
        tableVersion != EDVR_GRAPHICS_BRIDGE_VERSION_1 ||
        !request->device || !request->context) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(g_ownerMutex);
    Owner* owner = g_owner;
    if (!owner || !owner->hookReady.load(std::memory_order_acquire)) return E_NOINTERFACE;
    if (owner->device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        return E_NOINTERFACE;
    const HRESULT removed = owner->device->GetDeviceRemovedReason();
    if (FAILED(removed)) return removed;
    if (!edvr::gameDevice() ||
        !sameIdentity(request->device, static_cast<IUnknown*>(owner->device.Get())) ||
        !sameIdentity(request->context, static_cast<IUnknown*>(owner->context.Get())) ||
        !sameIdentity(static_cast<IUnknown*>(request->device),
                      reinterpret_cast<IUnknown*>(edvr::gameDevice()))) return E_ACCESSDENIED;
    bool expected = false;
    if (!owner->leaseLive.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) return E_PENDING;
    Lease* lease = new (std::nothrow) Lease{owner, GetCurrentThreadId()};
    if (!lease) {
        owner->leaseLive.store(false, std::memory_order_release);
        return E_OUTOFMEMORY;
    }
    table->size = sizeof(EdvrGraphicsBridgeTable);
    table->version = EDVR_GRAPHICS_BRIDGE_VERSION_1;
    table->lease = lease;
    table->execute = &executeLease;
    table->release = &releaseLease;
    return S_OK;
} catch (...) {
    return E_FAIL;
}

extern "C" HRESULT WINAPI edvrAcquireNativeGraphics(
    const EdvrNativeGraphicsRequest* request, EdvrNativeGraphicsTable* table) try {
    const uint32_t tableSize = table ? table->size : 0;
    const uint32_t tableVersion = table && tableSize >= sizeof(uint32_t) * 2
                                      ? table->version : 0;
    if (table) {
        if (tableSize >= sizeof(uint32_t)) table->size = 0;
        if (tableSize >= sizeof(uint32_t) * 2) table->version = 0;
        if (tableSize >= offsetof(EdvrNativeGraphicsTable, device) + sizeof(table->device))
            table->device = nullptr;
        if (tableSize >= sizeof(EdvrNativeGraphicsTable)) table->context = nullptr;
    }
    if (!request || !table || request->size != sizeof(EdvrNativeGraphicsRequest) ||
        request->version != EDVR_NATIVE_GRAPHICS_VERSION_1 ||
        tableSize != sizeof(EdvrNativeGraphicsTable) ||
        tableVersion != EDVR_NATIVE_GRAPHICS_VERSION_1) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(g_ownerMutex);
    Owner* owner = g_owner;
    if (!owner || !owner->hookReady.load(std::memory_order_acquire) ||
        !owner->device || !owner->context) return E_NOINTERFACE;
    if (!edvr::renderBoundaryValidateOwner(owner->device.Get(), owner->context.Get()))
        return E_NOINTERFACE;
    if (!edvr::gameDevice() ||
        !sameIdentity(static_cast<IUnknown*>(owner->device.Get()),
                      reinterpret_cast<IUnknown*>(edvr::gameDevice())))
        return E_ACCESSDENIED;
    if (owner->device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        return E_NOINTERFACE;
    const HRESULT removed = owner->device->GetDeviceRemovedReason();
    if (FAILED(removed)) return removed;
    ComPtr<ID3D11Device> resultDevice = owner->device;
    ComPtr<ID3D11DeviceContext> resultContext = owner->context;
    table->size = sizeof(EdvrNativeGraphicsTable);
    table->version = EDVR_NATIVE_GRAPHICS_VERSION_1;
    table->device = resultDevice.Detach();
    table->context = resultContext.Detach();
    return S_OK;
} catch (...) {
    return E_FAIL;
}

// Read-only desktop fixture probe; callers must serialize the tested commands.
extern "C" void edvr_selftest_graphics_bridge(uint64_t* privateLists, uint64_t* unknownLists) {
    if (privateLists) *privateLists = edvr::graphicsBridgePrivateExecutionCount();
    if (unknownLists) *unknownLists = edvr::graphicsBridgeUnknownExecutionCount();
}
