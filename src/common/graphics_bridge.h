#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDVR_GRAPHICS_BRIDGE_VERSION_1 1u

typedef struct EdvrGraphicsBridgeRequest {
    uint32_t size;
    uint32_t version;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
} EdvrGraphicsBridgeRequest;

typedef HRESULT (WINAPI *EdvrGraphicsBridgeExecute)(void* lease,
                                                     ID3D11CommandList* list);
typedef void (WINAPI *EdvrGraphicsBridgeRelease)(void* lease);

typedef struct EdvrGraphicsBridgeTable {
    uint32_t size;
    uint32_t version;
    void* lease;
    EdvrGraphicsBridgeExecute execute;
    EdvrGraphicsBridgeRelease release;
} EdvrGraphicsBridgeTable;

// Version 1 requires exact structure sizes and version in BOTH inputs. On
// failure, table fields within the supplied size are cleared. Even a short
// table must provide readable storage for its four-byte size field.
//
// This is a trusted paired-module API, not a command-list inspector. The caller
// guarantees that every write targets its private resources or acquired XR
// images; game buffers/textures and EDVR motion histories must never be written.
// The caller already owns the immediate context exclusively while acquiring
// and executing. A lease admits execution only on its acquisition thread; that
// restriction does not establish ownership against arbitrary game calls.
//
// Exactly one lease is admitted. The caller holds a module load reference until
// after release returns. Release is externally serialized against every lease
// operation and may run on another thread after execution has stopped. No
// callback is valid after release. No game thread handoff, loading scheduling,
// or D3D11Multithread mode change is performed by this bridge.

typedef HRESULT (WINAPI *EdvrAcquireGraphicsBridge)(
    const EdvrGraphicsBridgeRequest* request, EdvrGraphicsBridgeTable* table);

HRESULT WINAPI edvrAcquireGraphicsBridge(
    const EdvrGraphicsBridgeRequest* request, EdvrGraphicsBridgeTable* table);

#ifdef __cplusplus
}
#ifdef _WIN64
static_assert(sizeof(EdvrGraphicsBridgeRequest) == 24, "graphics bridge request ABI");
static_assert(sizeof(EdvrGraphicsBridgeTable) == 32, "graphics bridge table ABI");
#endif
#endif
