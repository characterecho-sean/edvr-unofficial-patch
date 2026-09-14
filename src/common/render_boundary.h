#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDVR_RENDER_BOUNDARY_VERSION_1 1u

typedef struct EdvrRenderBoundaryRequest {
    uint32_t size;
    uint32_t version;
    ID3D11Device* device;
    HRESULT (WINAPI *callback)(void* user, ID3D11Device* device,
                               ID3D11DeviceContext* context);
    void* user;
} EdvrRenderBoundaryRequest;

typedef HRESULT (WINAPI *EdvrRenderBoundaryClose)(void* lease);
typedef HRESULT (WINAPI *EdvrRenderBoundaryRelease)(void* lease);

typedef struct EdvrRenderBoundaryTable {
    uint32_t size;
    uint32_t version;
    void* lease;
    EdvrRenderBoundaryClose close;
    EdvrRenderBoundaryRelease release;
} EdvrRenderBoundaryTable;

// Version 1 requires exact structure sizes and version in both inputs. The
// registration is CPU-only and may run on a foreign Init caller. The device
// must be the published, hooked game device. The first successful owned,
// non-TEST Present fixes the callback thread; a later different Present thread
// closes admission. Delivery follows existing Present frame work and borrows
// that caller's immediate context. The host must already serialize game use
// of that context; registration does not establish exclusive ownership.
// The caller externally serializes lease operations and keeps private
// resources, callback user data, and the
// callback module alive until release returns S_OK. Exactly one lease is
// admitted. close() stops admission immediately and returns E_PENDING while
// a callback is active; release() also closes admission, returns E_PENDING
// while active, and destroys the lease and module reference only after the
// callback has unwound. A callback may close/release its lease (E_PENDING).
// After close returns S_OK no callback can begin. Callback failure or an
// exception closes admission. No lease operation is valid after release S_OK.
// The output must provide at least its readable size field; invalid inputs
// clear only complete fields within the caller's supplied output size.

HRESULT WINAPI edvrAcquireRenderBoundary(const EdvrRenderBoundaryRequest* request,
                                         EdvrRenderBoundaryTable* table);

#ifdef __cplusplus
}
#ifdef _WIN64
static_assert(sizeof(EdvrRenderBoundaryRequest) == 32, "render boundary request ABI");
static_assert(sizeof(EdvrRenderBoundaryTable) == 32, "render boundary table ABI");
#endif
#endif
