#pragma once

#include <windows.h>
#include <cwchar>

namespace edvr {

// QueryInterface can change a proxy's inner object. ReShade upgrades its
// IDXGISwapChain proxy to the interface returned by its wrapped object; some
// 3Dmigoto builds return the raw DXGI swap chain for IDXGISwapChain3. Asking
// ReShade for that interface would therefore discard 3Dmigoto's Present path.
// Probe the optional ResizeBuffers1 slot only on a system DXGI object.
inline bool swapChainResize1ProbeSafe(void* presentMethod) {
    if (!presentMethod) return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(presentMethod), &owner)) return false;

    wchar_t systemDxgi[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(systemDxgi, MAX_PATH);
    constexpr wchar_t suffix[] = L"\\dxgi.dll";
    if (!n || n >= MAX_PATH || n + (sizeof(suffix) / sizeof(wchar_t)) > MAX_PATH)
        return false;
    wcscat_s(systemDxgi, suffix);
    const HMODULE systemModule = GetModuleHandleW(systemDxgi);
    return systemModule && owner == systemModule;
}

}  // namespace edvr
