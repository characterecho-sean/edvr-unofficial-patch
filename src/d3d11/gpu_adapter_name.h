// The adapter's own name, for the "first asked for on %s" line both trained
// engines print (dlaa.cpp's own DLSS line, fsr3_engine.cpp's AMD twin).
//
// Read once per process -- there is no reason to ask the adapter twice in a
// session -- and kept as a static return so a later call is free. Header-only
// and inline so the one cache is shared by both callers' translation units
// (an inline function's local statics are merged across the program, so
// whichever engine asks first fills it for both -- correct, since both are
// asking about the same device).
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>

namespace edvr {

inline const char* adapterName(ID3D11Device* dev) {
    static char name[128] = "unknown (no device)";
    static bool tried = false;
    if (tried || !dev) return name;
    tried = true;
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
        dxgiDev) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr,
                                    nullptr);
            } else {
                snprintf(name, sizeof(name), "unknown (adapter desc refused)");
            }
            adapter->Release();
        } else {
            snprintf(name, sizeof(name), "unknown (no adapter)");
        }
        dxgiDev->Release();
    } else {
        snprintf(name, sizeof(name), "unknown (device is not a DXGI device)");
    }
    return name;
}

}  // namespace edvr
