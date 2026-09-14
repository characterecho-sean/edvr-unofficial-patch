#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace edvr {

// Read-only identity for the OpenXR migration census. An adapter match does
// not imply that two devices may share resources; callers log both identities.
inline bool d3d11AdapterLuid(ID3D11Device* device, LUID* luid) {
    if (!device || !luid) return false;
    *luid = {};
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) {
        return false;
    }
    *luid = desc.AdapterLuid;
    return true;
}

}  // namespace edvr
