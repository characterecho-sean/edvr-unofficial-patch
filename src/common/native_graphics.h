#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDVR_NATIVE_GRAPHICS_VERSION_1 1u

typedef struct EdvrNativeGraphicsRequest {
    uint32_t size;
    uint32_t version;
} EdvrNativeGraphicsRequest;

typedef struct EdvrNativeGraphicsTable {
    uint32_t size;
    uint32_t version;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
} EdvrNativeGraphicsTable;

// CPU-only discovery of the already-created game device and its immediate
// context. The caller owns one reference to each returned interface and must
// release them after all paired bridge/boundary leases have been released.
// The caller retains the trusted provider module through those releases and
// serializes its own use. This snapshot neither reserves a bridge lease nor
// establishes exclusive context ownership. No first Present is required.
//
// Version 1 requires exact sizes/versions in both arguments. Failure clears
// only complete output fields within the supplied size; the size field must
// always be readable. Missing registered graphics/render owners fail promptly
// with E_NOINTERFACE; a conflicting published device fails E_ACCESSDENIED.
// Single-threaded devices are unsupported; removal errors are preserved.
// Registration alone does not prove that Present can deliver callbacks: the
// startup consumer must observe a real callback before requiring render work.
HRESULT WINAPI edvrAcquireNativeGraphics(const EdvrNativeGraphicsRequest* request,
                                         EdvrNativeGraphicsTable* table);

#ifdef __cplusplus
}
#ifdef _WIN64
static_assert(sizeof(EdvrNativeGraphicsRequest) == 8, "native graphics request ABI");
static_assert(sizeof(EdvrNativeGraphicsTable) == 24, "native graphics table ABI");
#endif
#endif
