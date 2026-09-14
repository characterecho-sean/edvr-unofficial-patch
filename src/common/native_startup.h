#pragma once

#include <stdint.h>

// Immutable build capability, exported as data and read by the installer
// without loading the DLL. The native graphics variant owns Elite's early
// backend selection; it must be installed together with the native facade.
#define EDVR_NATIVE_STARTUP_VERSION_1 1u
#define EDVR_NATIVE_STARTUP_ROUTE_OCULUS 1u

typedef struct EdvrNativeStartupRouting {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
} EdvrNativeStartupRouting;

#ifdef __cplusplus
static_assert(sizeof(EdvrNativeStartupRouting) == 16, "native startup marker ABI");
extern "C" const EdvrNativeStartupRouting edvrNativeStartupRouting;
#endif
