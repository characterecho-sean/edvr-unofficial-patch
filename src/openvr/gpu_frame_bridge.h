#pragma once
#include <windows.h>
#include <cstdint>
#include "../common/gpu_frame_protocol.h"
#include "../common/log.h"

namespace edvr {
using GpuFrameEventFn = uint64_t (WINAPI*)(unsigned, unsigned, uint64_t, unsigned, unsigned, void*);
inline uint64_t gpuFrameEvent(GpuFrameEvent event, uint64_t sequence = 0, unsigned eye = 0,
                              unsigned flags = 0, void* texture = nullptr) noexcept {
    static LONG notedMissingModule = 0;
    static LONG notedMissingExport = 0;
    struct ReceiverPath {
        wchar_t path[32768]{};
        bool valid = false;
        ReceiverPath() noexcept {
            const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
            if (!n || n >= 32768) return;
            wchar_t* slash = wcsrchr(path, L'\\');
            if (!slash || static_cast<size_t>(slash - path) + 11 >= 32768) return;
            wcscpy_s(slash + 1, 32768 - static_cast<size_t>(slash - path) - 1, L"d3d11.dll");
            valid = true;
        }
    };
    static const ReceiverPath receiver; // Executable path is immutable; no destructor work.
    if (!receiver.valid) return 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(0, receiver.path, &module)) {
        if (InterlockedCompareExchange(&notedMissingModule, 1, 0) == 0)
            Log::get().note("GPU frame bridge: sibling d3d11.dll is not loaded; frame timing unavailable.");
        return 0;
    }
    auto fn = reinterpret_cast<GpuFrameEventFn>(GetProcAddress(module, "edvrGpuFrameEvent"));
    uint64_t result = 0;
    if (fn) result = fn(kGpuFrameProtocol, static_cast<unsigned>(event), sequence, eye, flags, texture);
    else if (InterlockedCompareExchange(&notedMissingExport, 1, 0) == 0)
        Log::get().note("GPU frame bridge: sibling d3d11.dll has no edvrGpuFrameEvent export; frame timing unavailable.");
    FreeLibrary(module);
    return result;
}
}
