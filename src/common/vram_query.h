// The one place that asks DXGI how full the GPU's memory is (vram_watch.h says what is done with the answer).
//
// Both halves use it: the graphics half's once-a-second watch (src\d3d11\vram_tick.cpp) and the OpenXR runtime's
// SLOW line, which names the figures beside the frame time it blames on someone (src\openxr\slow_regime.h).
//
// IDXGIAdapter3 is Windows 10 1607 and later, and DXVK and Wine answer for it only when their Vulkan driver does.
// A stack that does not offer it is not an error: the caller says so once and stops asking (vramAdapterOf's `why`).
//
// Nothing here writes to a device or an adapter. QueryVideoMemoryInfo is a read of the OS's own books, a few
// microseconds, and DXGI's adapter objects are free-threaded, so the caller's thread does not matter.
#pragma once

#include <windows.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <cstring>

#include "vram_watch.h"

namespace edvr {

// One segment group of node 0 (the only node a single-GPU machine has), through any object that has
// QueryVideoMemoryInfo -- IDXGIAdapter3, or a rig's stand-in for one. `hr` receives the failure, if any.
template <class Adapter>
inline VramSegment vramReadSegment(Adapter* adapter, DXGI_MEMORY_SEGMENT_GROUP group, HRESULT* hr = nullptr) {
    VramSegment s;
    if (!adapter) return s;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    const HRESULT result = adapter->QueryVideoMemoryInfo(0, group, &info);
    if (FAILED(result)) {
        if (hr) *hr = result;
        return s;
    }
    s.valid = true;
    s.usage = info.CurrentUsage;
    s.budget = info.Budget;
    return s;
}

template <class Adapter>
inline VramFigures vramRead(Adapter* adapter, HRESULT* hr = nullptr) {
    VramFigures f;
    f.local = vramReadSegment(adapter, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, hr);
    f.nonLocal = vramReadSegment(adapter, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, hr);
    return f;
}

// The IDXGIAdapter3 of the adapter `device` sits on (an ID3D11Device, or anything that answers for IDXGIDevice), with
// one reference that is the caller's, or null with `*why` naming what is missing. `name` (if given) receives the
// adapter's description as UTF-8, empty when it could not be read.
inline IDXGIAdapter3* vramAdapterOf(IUnknown* device, const char** why, char* name = nullptr, size_t nameSize = 0) {
    const char* unwanted = nullptr;
    const char** reason = why ? why : &unwanted;
    if (name && nameSize) name[0] = 0;
    if (!device) {
        *reason = "no device to ask";
        return nullptr;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) || !dxgiDevice) {
        *reason = "the device has no IDXGIDevice";
        return nullptr;
    }
    IDXGIAdapter* adapter = nullptr;
    const HRESULT got = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(got) || !adapter) {
        *reason = "the device reports no DXGI adapter";
        return nullptr;
    }
    if (name && nameSize) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            char utf8[512];   // 128 UTF-16 units are at most 384 bytes of UTF-8
            const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            if (n > 0) {
                size_t cut = static_cast<size_t>(n - 1);
                if (cut > nameSize - 1) {
                    cut = nameSize - 1;
                    while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80) --cut;   // not inside a character
                }
                std::memcpy(name, utf8, cut);
                name[cut] = 0;
            }
        }
    }
    IDXGIAdapter3* adapter3 = nullptr;
    const HRESULT asked = adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3));
    adapter->Release();
    if (FAILED(asked) || !adapter3) {
        *reason = "IDXGIAdapter3 is not offered, and QueryVideoMemoryInfo needs it (Windows 10 before 1607, or a DXVK or Wine without it)";
        return nullptr;
    }
    return adapter3;
}

}  // namespace edvr
