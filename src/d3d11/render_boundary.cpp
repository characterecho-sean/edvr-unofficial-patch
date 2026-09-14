#include "render_boundary.h"

#include "../common/render_boundary.h"
#include "../common/frame_flag.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

struct Lease;

struct Owner {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IUnknown> firstPresentIdentity;
    DWORD renderThread = 0;
    bool threadRecorded = false;
    Lease* lease = nullptr;
};

struct Lease {
    Owner* owner;
    EdvrRenderBoundaryRequest request;
    HMODULE module;
    std::atomic<bool> admitted{true};
    std::atomic<uint32_t> active{0};
};

std::mutex g_mutex;
Owner* g_owner = nullptr;
ComPtr<IUnknown> g_firstPresentIdentity;
DWORD g_firstPresentThread = 0;
bool g_firstPresentRecorded = false;

bool identity(IUnknown* object, ComPtr<IUnknown>& result) {
    result.Reset();
    return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&result)));
}

bool sameIdentity(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> ai, bi;
    return identity(a, ai) && identity(b, bi) && ai.Get() == bi.Get();
}

void clearTable(EdvrRenderBoundaryTable* table) noexcept {
    if (!table) return;
    const uint32_t supplied = table->size;
    if (supplied >= sizeof(uint32_t)) table->size = 0;
    if (supplied >= sizeof(uint32_t) * 2) table->version = 0;
    if (supplied >= offsetof(EdvrRenderBoundaryTable, lease) + sizeof(table->lease))
        table->lease = nullptr;
    if (supplied >= offsetof(EdvrRenderBoundaryTable, close) + sizeof(table->close))
        table->close = nullptr;
    if (supplied >= sizeof(EdvrRenderBoundaryTable)) table->release = nullptr;
}

HRESULT WINAPI closeLease(void* opaque) noexcept {
    Lease* lease = static_cast<Lease*>(opaque);
    if (!lease) return E_INVALIDARG;
    // Serialize admission with Present's active increment. Independent atomics
    // alone do not guarantee that close observes admission racing on another
    // thread before reporting that no callback remains.
    std::lock_guard<std::mutex> lock(g_mutex);
    lease->admitted.store(false, std::memory_order_release);
    return lease->active.load(std::memory_order_acquire) == 0 ? S_OK : E_PENDING;
}

HRESULT WINAPI releaseLease(void* opaque) noexcept {
    Lease* lease = static_cast<Lease*>(opaque);
    if (!lease) return E_INVALIDARG;

    HMODULE module = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        lease->admitted.store(false, std::memory_order_release);
        if (lease->active.load(std::memory_order_acquire) != 0) return E_PENDING;
        if (lease->owner && lease->owner->lease == lease) lease->owner->lease = nullptr;
        module = lease->module;
        lease->module = nullptr;
    }
    delete lease;
    if (module) FreeLibrary(module);
    return S_OK;
}

} // namespace

namespace edvr {

bool renderBoundaryRegisterOwner(ID3D11Device* device,
                                 ID3D11DeviceContext* context) noexcept {
    if (!device || !context) return false;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_owner) return false;
        ComPtr<ID3D11DeviceContext> immediate;
        device->GetImmediateContext(&immediate);
        if (!immediate || !sameIdentity(immediate.Get(), context)) return false;
        ComPtr<IUnknown> first;
        if (!identity(static_cast<IUnknown*>(device), first)) return false;
        // The first device seen at the real Present boundary is canonical.
        // A different COM identity must never be rebound to this provider.
        if (g_firstPresentIdentity && g_firstPresentIdentity.Get() != first.Get()) return false;
        if (!g_firstPresentIdentity) g_firstPresentIdentity = first;
        Owner* owner = new (std::nothrow) Owner();
        if (!owner) return false;
        owner->device = device;
        owner->context = context;
        owner->firstPresentIdentity = g_firstPresentIdentity;
        if (g_firstPresentRecorded) {
            owner->renderThread = g_firstPresentThread;
            owner->threadRecorded = true;
        }
        g_owner = owner;
        return true;
    } catch (...) {
        return false;
    }
}

bool renderBoundaryValidateOwner(ID3D11Device* device,
                                 ID3D11DeviceContext* context) noexcept {
    if (!device || !context) return false;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_owner || !g_owner->device || !g_owner->context ||
            !g_owner->firstPresentIdentity) return false;
        if (!sameIdentity(static_cast<IUnknown*>(device), g_owner->device.Get()) ||
            !sameIdentity(static_cast<IUnknown*>(context), g_owner->context.Get()) ||
            !sameIdentity(static_cast<IUnknown*>(device), g_owner->firstPresentIdentity.Get()))
            return false;
        return true;
    } catch (...) {
        return false;
    }
}

void renderBoundaryNoteOwnedPresent(ID3D11Device* device) noexcept {
    if (!device) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_owner) {
            if (g_owner->device.Get() == device && !g_owner->threadRecorded) {
                g_owner->renderThread = GetCurrentThreadId();
                g_owner->threadRecorded = true;
            }
            return;
        }
        if (!g_firstPresentRecorded && identity(static_cast<IUnknown*>(device), g_firstPresentIdentity)) {
            g_firstPresentThread = GetCurrentThreadId();
            g_firstPresentRecorded = true;
        }
    } catch (...) {
    }
}

void renderBoundaryPresent(ID3D11Device* device) noexcept {
    Lease* lease = nullptr;
    ID3D11DeviceContext* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Owner* owner = g_owner;
        if (!owner || owner->device.Get() != device || !owner->threadRecorded) return;
        if (GetCurrentThreadId() != owner->renderThread) {
            if (owner->lease) owner->lease->admitted.store(false, std::memory_order_release);
            return;
        }
        lease = owner->lease;
        if (!lease || !lease->admitted.load(std::memory_order_acquire)) return;
        // A reentrant Present from the callback is still a real Present, but it
        // must not recursively invoke the consumer.
        if (lease->active.load(std::memory_order_acquire) != 0) return;
        lease->active.fetch_add(1, std::memory_order_acq_rel);
        if (!lease->admitted.load(std::memory_order_acquire)) {
            lease->active.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }
        context = owner->context.Get();
    }

    HRESULT hr = E_FAIL;
    try {
        hr = lease->request.callback(lease->request.user, device, context);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) lease->admitted.store(false, std::memory_order_release);
    lease->active.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace edvr

extern "C" HRESULT WINAPI edvrAcquireRenderBoundary(
    const EdvrRenderBoundaryRequest* request, EdvrRenderBoundaryTable* table) {
    const uint32_t tableSize = table ? table->size : 0;
    const uint32_t tableVersion = table && tableSize >= sizeof(uint32_t) * 2 ? table->version : 0;
    if (table) clearTable(table);
    if (!request || !table || request->size != sizeof(EdvrRenderBoundaryRequest) ||
        request->version != EDVR_RENDER_BOUNDARY_VERSION_1 ||
        tableSize != sizeof(EdvrRenderBoundaryTable) ||
        tableVersion != EDVR_RENDER_BOUNDARY_VERSION_1 || !request->device || !request->callback)
        return E_INVALIDARG;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(request->callback), &module) || !module)
        return E_INVALIDARG;

    try {
        HRESULT result = S_OK;
        {
        std::lock_guard<std::mutex> lock(g_mutex);
        Owner* owner = g_owner;
        if (!owner || !owner->device || !owner->context || !owner->firstPresentIdentity) {
            result = E_NOINTERFACE;
        } else if (owner->device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED ||
                   FAILED(owner->device->GetDeviceRemovedReason()) ||
                   !sameIdentity(static_cast<IUnknown*>(request->device), owner->firstPresentIdentity.Get()) ||
                   !sameIdentity(static_cast<IUnknown*>(request->device), owner->device.Get()) ||
                   !sameIdentity(static_cast<IUnknown*>(request->device),
                                 static_cast<IUnknown*>(edvr::gameDevice()))) {
            result = E_ACCESSDENIED;
        } else if (owner->lease) {
            result = E_PENDING;
        } else {
            Lease* lease = new (std::nothrow) Lease{owner, *request, module};
            if (!lease) {
                result = E_OUTOFMEMORY;
            } else {
                owner->lease = lease;
                table->size = sizeof(EdvrRenderBoundaryTable);
                table->version = EDVR_RENDER_BOUNDARY_VERSION_1;
                table->lease = lease;
                table->close = &closeLease;
                table->release = &releaseLease;
                module = nullptr; // owned by the lease now
            }
        }
        }
        if (result != S_OK && module) FreeLibrary(module);
        if (result != S_OK) return result;
        return S_OK;
    } catch (...) {
        FreeLibrary(module);
        return E_FAIL;
    }
}
