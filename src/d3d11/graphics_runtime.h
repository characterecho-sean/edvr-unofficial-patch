#pragma once

#include <atomic>

namespace edvr {

// Latched for the lifetime of the graphics DLL before installing any fixes.
// Submit-side entry points also use this gate: skipping device/context hooks
// alone does not prevent the OpenVR DLL from invoking a compute pass directly.
inline std::atomic<bool> g_graphicsRuntimeDisabled{false};

inline bool graphicsRuntimeDisabled() {
    return g_graphicsRuntimeDisabled.load(std::memory_order_relaxed);
}

inline void disableGraphicsRuntime() {
    g_graphicsRuntimeDisabled.store(true, std::memory_order_relaxed);
}

}  // namespace edvr
