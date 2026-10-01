// Which way the flat resolver isolates the game's pipeline state from its own work and its backends', and how a DXMT device is
// told (docs\macos-dxmt-2026-09-30.md).
//
//   swap     ID3D11DeviceContext1::SwapDeviceContextState: one call out, one back. Proven on Windows and cheap. It is the
//            default everywhere but on DXMT, and on Windows nothing about it changes.
//   capture  the explicit state block (flat_context_state.h): Get calls out, ClearState, Set calls back. For DXMT, whose
//            SwapDeviceContextState is UNIMPLEMENTED() and so abort()s the process (CrossOver's log: "SwapDeviceContextState
//            is not implemented.", then raise(22), then LdrShutdownProcess; no SEH filter sees it).
//
// advanced.flat_context_isolation = auto | swap | capture. auto is the shipped value and picks capture when the device is
// DXMT. swap and capture force one or the other, for testing; swap on a DXMT device ends the process, and capture on Windows
// works and costs a few dozen calls a frame.
//
// HOW A DXMT DEVICE IS KNOWN. By what DXMT itself says, read from its source (github.com/3Shain/dxmt), so that a rename or a
// vendor spoof does not matter. Four independent markers, all asked together, and the line in the log names each that answered:
//
//   device interface   ID3D11Device::QueryInterface(IMTLD3D11DeviceExt {efc77ae6-2179-4c0a-b844-7661ca0dcde7}) succeeds.
//                      d3d11_interfaces.hpp defines it; MTLD3D11DXGIDevice::QueryInterface (d3d11_device.cpp) answers it, and
//                      MTLD3D11DeviceImpl::QueryInterface forwards to that. No other D3D11 implementation has the IID.
//   context interface  ID3D11DeviceContext::QueryInterface(IMTLD3D11ContextExt {43ace3ce-1956-448b-a4eb-aee68bdeb283})
//                      succeeds (d3d11_context_impl.cpp answers it; the same header; the interface DXMT's own nvngx shim
//                      reaches TemporalUpscale through). The device's own QueryInterface refuses this IID by name.
//   version resource   the module that holds the device's QueryInterface code carries a VERSIONINFO whose ProductName is DXMT
//                      (src\d3d11\version.rc: CompanyName DXMT, ProductName DXMT, FileDescription Direct3D 11 Runtime).
//   adapter name       the DXGI adapter description begins "Apple" ("Apple M4 Max" under CrossOver, with a spoofed NVIDIA
//                      vendor id). Only the fallback: it alone makes the decision when no other marker answered, because
//                      D3DMetal and MoltenVK devices carry Apple names too and the explicit capture is safe on any device.
//
// A device with none of them is a device whose swap works, and whose breadcrumbs file stays empty of the HDR route's crumbs
// (flatCrumbsWantedFor).
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

enum class FlatContextIsolation : uint8_t { Auto, Swap, Capture };

inline const char* flatContextIsolationName(FlatContextIsolation mode) {
    switch (mode) {
    case FlatContextIsolation::Swap: return "swap";
    case FlatContextIsolation::Capture: return "capture";
    default: return "auto";
    }
}
// The key's text; anything else, null included, is auto.
inline FlatContextIsolation flatContextIsolationFromText(const char* text) {
    if (text && _stricmp(text, "swap") == 0) return FlatContextIsolation::Swap;
    if (text && _stricmp(text, "capture") == 0) return FlatContextIsolation::Capture;
    return FlatContextIsolation::Auto;
}

// github.com/3Shain/dxmt, src/d3d11/d3d11_interfaces.hpp.
inline const GUID kDxmtDeviceExtIid = {0xefc77ae6, 0x2179, 0x4c0a, {0xb8, 0x44, 0x76, 0x61, 0xca, 0x0d, 0xcd, 0xe7}};
inline const GUID kDxmtContextExtIid = {0x43ace3ce, 0x1956, 0x448b, {0xa4, 0xeb, 0xae, 0xe6, 0x8b, 0xde, 0xb2, 0x83}};

enum FlatDxmtMarker : unsigned {
    kDxmtDeviceInterface = 1u,
    kDxmtContextInterface = 2u,
    kDxmtVersionResource = 4u,
    kDxmtAdapterName = 8u,
};

struct FlatDxmtDetection {
    unsigned markers = 0;     // the FlatDxmtMarker bits that answered
    char adapter[96] = {};    // the adapter description, ASCII (anything else as '?'); empty when it could not be read
    bool dxmt() const { return markers != 0; }
    // A marker that is DXMT's own word, not the adapter's name.
    bool positive() const { return (markers & ~static_cast<unsigned>(kDxmtAdapterName)) != 0; }
};

// Does this object answer QueryInterface for the IID? The reference a success returns is released at once.
inline bool flatAnswersInterface(IUnknown* object, const GUID& iid) {
    if (!object) return false;
    void* out = nullptr;
    if (FAILED(object->QueryInterface(iid, &out)) || !out) return false;
    static_cast<IUnknown*>(out)->Release();
    return true;
}

// A VS_VERSIONINFO blob (an RT_VERSION resource) whose StringFileInfo names "ProductName" and gives it the value "DXMT". The
// strings are UTF-16; a value follows its key's terminator after at most one zero word of padding.
inline bool flatVersionBlobNamesDxmt(const void* blob, size_t size) {
    static const wchar_t kKey[] = L"ProductName";
    static const wchar_t kValue[] = L"DXMT";
    if (!blob) return false;
    const unsigned char* b = static_cast<const unsigned char*>(blob);
    for (size_t i = 0; i + sizeof(kKey) <= size; i += 2) {
        if (std::memcmp(b + i, kKey, sizeof(kKey)) != 0) continue;
        size_t v = i + sizeof(kKey);
        for (int pad = 0; pad < 2; ++pad, v += 2)
            if (v + sizeof(kValue) <= size && std::memcmp(b + v, kValue, sizeof(kValue)) == 0) return true;
    }
    return false;
}
inline bool flatModuleNamesDxmt(HMODULE module) {
    if (!module) return false;
    // VS_VERSION_INFO is resource 1 of type RT_VERSION (16), spelt out so the wide call gets wide names in any build.
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
    if (!found) return false;
    HGLOBAL loaded = LoadResource(module, found);
    const DWORD size = SizeofResource(module, found);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    return data && size && flatVersionBlobNamesDxmt(data, size);
}
// The module the object's first virtual method (IUnknown::QueryInterface, which nothing of EDVR's patches) lives in: the D3D11
// implementation itself. No module reference is taken.
inline bool flatObjectModuleNamesDxmt(IUnknown* object) {
    if (!object) return false;
    void** vtable = *reinterpret_cast<void***>(object);
    HMODULE module = nullptr;
    if (!vtable || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(vtable[0]), &module))
        return false;
    return flatModuleNamesDxmt(module);
}

inline bool flatAdapterNameIsApple(const char* name) { return name && _strnicmp(name, "Apple", 5) == 0; }
// The description DXGI gives the device's adapter, as ASCII into `out` (empty when it cannot be had).
inline void flatAdapterDescription(IUnknown* device, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!device) return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()))) || FAILED(dxgi->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetDesc(&desc)))
        return;
    size_t n = 0;
    for (; desc.Description[n] && n + 1 < cap; ++n) out[n] = (desc.Description[n] >= 0x20 && desc.Description[n] < 0x7f) ? static_cast<char>(desc.Description[n]) : '?';
    out[n] = '\0';
}

// Every marker, asked together. `device` and `context` are the game's own objects; both may be null (that marker is silent).
inline FlatDxmtDetection flatDetectDxmt(IUnknown* device, IUnknown* context) {
    FlatDxmtDetection d;
    if (flatAnswersInterface(device, kDxmtDeviceExtIid)) d.markers |= kDxmtDeviceInterface;
    if (flatAnswersInterface(context, kDxmtContextExtIid)) d.markers |= kDxmtContextInterface;
    if (flatObjectModuleNamesDxmt(device)) d.markers |= kDxmtVersionResource;
    flatAdapterDescription(device, d.adapter, sizeof(d.adapter));
    if (flatAdapterNameIsApple(d.adapter)) d.markers |= kDxmtAdapterName;
    return d;
}

struct FlatContextIsolationChoice {
    FlatContextIsolation mode = FlatContextIsolation::Swap;   // Swap or Capture, never Auto
    bool forced = false;                                      // the key said so; the device was not consulted
};
// The decision. A forced mode is honoured as asked; auto is capture for a device any marker called DXMT, swap for the rest.
inline FlatContextIsolationChoice flatChooseContextIsolation(FlatContextIsolation request, const FlatDxmtDetection& d) {
    FlatContextIsolationChoice c;
    if (request == FlatContextIsolation::Swap || request == FlatContextIsolation::Capture) {
        c.mode = request;
        c.forced = true;
    } else {
        c.mode = d.dxmt() ? FlatContextIsolation::Capture : FlatContextIsolation::Swap;
    }
    return c;
}

// The HDR route's breadcrumbs (flat_hdr_crumbs.h) are DXMT's alone: a device that any marker calls DXMT opens their gate, and
// nothing else does. The isolation request is deliberately not an argument: advanced.flat_context_isolation=capture on a Windows
// device changes how the resolver isolates the game's state, and turns no crumb on.
inline bool flatCrumbsWantedFor(const FlatDxmtDetection& d) { return d.dxmt(); }

// "device interface IMTLD3D11DeviceExt, context interface IMTLD3D11ContextExt, module version resource ProductName=DXMT,
// adapter name "Apple M4 Max"": what answered, in that order. "none" when nothing did.
inline size_t flatFormatDxmtMarkers(const FlatDxmtDetection& d, char* out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    size_t n = 0;
    auto add = [&](const char* text) {
        if (n && n + 2 < cap) { out[n++] = ','; out[n++] = ' '; }
        for (; *text && n + 1 < cap; ++text) out[n++] = *text;
        out[n] = '\0';
    };
    if (d.markers & kDxmtDeviceInterface) add("device interface IMTLD3D11DeviceExt");
    if (d.markers & kDxmtContextInterface) add("context interface IMTLD3D11ContextExt");
    if (d.markers & kDxmtVersionResource) add("module version resource ProductName=DXMT");
    if (d.markers & kDxmtAdapterName) {
        char text[128];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "adapter name \"%s\"", d.adapter);
        add(text);
    }
    if (!n) add("none");
    return n;
}

// The one log line of a renderer initialisation: the mode, and why.
//   flat resolver: context isolation by explicit state capture (DXMT: device interface IMTLD3D11DeviceExt, ...)
//   flat resolver: context isolation by explicit state capture (advanced.flat_context_isolation=capture; DXMT markers: none)
//   flat resolver: context isolation by context state swap (no DXMT marker)
//   flat resolver: context isolation by context state swap (advanced.flat_context_isolation=swap; DXMT markers: ...)
inline size_t flatFormatContextIsolationLine(const FlatContextIsolationChoice& c, const FlatDxmtDetection& d, char* out, size_t cap) {
    if (!out || !cap) return 0;
    char markers[320];
    flatFormatDxmtMarkers(d, markers, sizeof(markers));
    const char* how = c.mode == FlatContextIsolation::Capture ? "explicit state capture" : "context state swap";
    int n;
    if (c.forced)
        n = _snprintf_s(out, cap, _TRUNCATE, "flat resolver: context isolation by %s (advanced.flat_context_isolation=%s; DXMT markers: %s%s)", how,
                        flatContextIsolationName(c.mode), markers,
                        (c.mode == FlatContextIsolation::Swap && d.dxmt()) ? "; DXMT aborts in the swap" : "");
    else if (c.mode == FlatContextIsolation::Capture)
        n = _snprintf_s(out, cap, _TRUNCATE, "flat resolver: context isolation by %s (DXMT: %s)", how, markers);
    else
        n = _snprintf_s(out, cap, _TRUNCATE, "flat resolver: context isolation by %s (no DXMT marker)", how);
    return n < 0 ? std::strlen(out) : static_cast<size_t>(n);
}

}  // namespace edvr
