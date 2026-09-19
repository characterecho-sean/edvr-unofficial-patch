#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct D3D11_INPUT_ELEMENT_DESC;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11VertexShader;

namespace edvr {

// Original-draw ownership for rigid scene surfaces. The feature appends an
// integer owner target to an eligible game draw; it never reissues the draw.
void staticSurfaceConfigure(bool on);
void staticSurfaceRememberVs(ID3D11VertexShader*, uint64_t shaderHash,
                             const void* bytecode, size_t bytes, bool linked);
void staticSurfaceRememberPs(ID3D11PixelShader*, const void* bytecode,
                             size_t bytes, bool linked);
void staticSurfaceRememberLayout(ID3D11InputLayout*,
                                 const D3D11_INPUT_ELEMENT_DESC*, unsigned count,
                                 uint64_t vertexShaderHash);

// Begin replaces only the pixel shader and the state needed for MRT slot 7.
// End is safe after a false Begin and restores every state changed by Begin.
bool staticSurfaceBegin(ID3D11DeviceContext*, unsigned count, unsigned instances,
                        unsigned start, int base, unsigned startInstance,
                        uint64_t vsHash);
void staticSurfaceEnd(ID3D11DeviceContext*);

// AddRef-owned SRVs ordered as [current, previous]; the caller releases both.
// True means both belong to consecutive completed frames of this exact
// eye/depth identity. On false the entries are null, so callers cannot
// accidentally use stale ownership.
bool staticSurfaceViews(ID3D11DeviceContext*, ID3D11Texture2D* sceneDepth,
                        ID3D11ShaderResourceView** views);
void staticSurfaceFrameBoundary(ID3D11DeviceContext*);
void staticSurfaceShutdown();

// A null resource means an unknown command-list write and invalidates all
// ownership history. Byte intervals are [first,end); omitted means all.
void staticSurfaceResourceWritten(ID3D11Resource*, uint64_t first = 0,
                                  uint64_t end = ~uint64_t(0));

// Eye-run support. Staging is asynchronous; WriteDump maps the already staged
// pair and always emits a JSON marker, including the unavailable reason.
void staticSurfaceStageDump(ID3D11DeviceContext*, ID3D11Texture2D* sceneDepth,
                            unsigned sceneFrame = ~0u);
void staticSurfaceWriteDump(ID3D11DeviceContext*, const wchar_t* directory,
                            const wchar_t* stamp);

}  // namespace edvr
