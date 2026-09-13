#include "graphics_bridge.h"
#include "render_boundary.h"

#include "../common/frame_flag.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

struct Owner {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::atomic<bool> hookReady{false};
    std::atomic<bool> leaseLive{false};
};

std::mutex g_ownerMutex;
Owner* g_owner = nullptr;
std::atomic<uint64_t> g_privateExecutions{0};
std::atomic<uint64_t> g_unknownExecutions{0};

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

// Read-only desktop fixture probe; callers must serialize the tested commands.
extern "C" void edvr_selftest_graphics_bridge(uint64_t* privateLists, uint64_t* unknownLists) {
    if (privateLists) *privateLists = edvr::graphicsBridgePrivateExecutionCount();
    if (unknownLists) *unknownLists = edvr::graphicsBridgeUnknownExecutionCount();
}
