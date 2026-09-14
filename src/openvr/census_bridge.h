#pragma once

#include <cwchar>
#include "../common/vr_census.h"

namespace edvr {

// Called before observing the owned compositor's pose wait. The graphics DLL
// beside the executable is the paired proxy; a basename lookup may instead
// return the system d3d11.dll, which is also loaded in this process.
inline void vrCensusStartAtPoseWait() {
    if (!vrCensusBeginVr()) return;
    static std::atomic<bool> acknowledged{false};
    static std::atomic<unsigned> attempts{0};
    if (acknowledged.load(std::memory_order_acquire)) return;
    constexpr unsigned kMaxAttempts = 64;
    unsigned attempt = attempts.load(std::memory_order_relaxed);
    do { if (attempt >= kMaxAttempts) return; }
    while (!attempts.compare_exchange_weak(attempt, attempt + 1, std::memory_order_relaxed));

    wchar_t path[MAX_PATH]{};
    HMODULE module = nullptr;
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && static_cast<size_t>(slash - path) + 1 + wcslen(L"d3d11.dll") < MAX_PATH) {
            wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"d3d11.dll");
            module = GetModuleHandleW(path); // Lookup only: never LoadLibrary here.
        }
    }
    using Begin = BOOL (WINAPI*)(unsigned);
    auto begin = module ? reinterpret_cast<Begin>(GetProcAddress(module, "edvrCensusBeginVr")) : nullptr;
    const bool accepted = begin && begin(1) != FALSE;
    if (accepted) {
        if (!acknowledged.exchange(true, std::memory_order_acq_rel)) {
            Log::get().note("VR order census bridge: acknowledged by graphics DLL on attempt %u; "
                            "both DLLs selected their VR budgets. Compare QPC, not local frame fields.",
                            attempt + 1);
        }
    } else if (attempt == 0 || attempt + 1 == kMaxAttempts) {
        Log::get().note("VR order census bridge: %s on attempt %u/%u (%s); "
                        "graphics VR capture is not acknowledged.",
                        attempt + 1 == kMaxAttempts ? "exhausted" : "pending",
                        attempt + 1, kMaxAttempts,
                        !module ? "paired module unavailable" : !begin ? "export unavailable" :
                        "receiver not initialized or census disabled");
    }
}

} // namespace edvr
