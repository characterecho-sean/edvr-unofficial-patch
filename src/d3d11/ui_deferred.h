#pragma once
#include <cstdint>
#include <cstddef>
struct ID3D11DeviceChild;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11View;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct D3D11_BOX;
namespace edvr {
class Config;
void uiDeferredConfigure(Config&);
void uiDeferredRemember(ID3D11DeviceChild*,const void*,size_t,bool linked);
void uiDeferredBeforeDraw(ID3D11DeviceContext*);
void uiDeferredBeforeDispatch(ID3D11DeviceContext*);
// Captures supported UI for native replay, or mirrors an interleaved world
// draw into the clean image. Original colour/depth/stencil remain intact.
// End must follow every Begin, including world draws returning false.
bool uiDeferredBegin(ID3D11DeviceContext*, int eye, char kind, uint32_t count,
                     uint32_t instances, uint32_t start, int32_t base,
                     uint32_t startInstance);
void uiDeferredEnd(ID3D11DeviceContext*);
void uiDeferredBeforeTone(ID3D11DeviceContext*, char kind, uint32_t count, uint32_t instances,
                          uint32_t start, int32_t base, uint32_t startInstance);
// Builds the entire output command list before publishing a clean DLSS input.
ID3D11ShaderResourceView* uiDeferredPrepare(ID3D11DeviceContext*, ID3D11Texture2D* submitted,
    ID3D11ShaderResourceView* sceneDepth, ID3D11Texture2D* output, int eye,
    uint32_t width, uint32_t height, float jitterX, float jitterY);
void uiDeferredApply(ID3D11DeviceContext*, int eye);
bool uiDeferredFallbackReset(int eye);
void uiDeferredResourceWrite(ID3D11DeviceContext*, ID3D11Resource*);
void uiDeferredViewWrite(ID3D11DeviceContext*, ID3D11View*);
void uiDeferredCopy(ID3D11Resource* destination, ID3D11Resource* source, bool complete);
void uiDeferredCopyRegion(ID3D11Resource* destination,uint32_t destinationSub,uint32_t x,uint32_t y,uint32_t z,ID3D11Resource* source,uint32_t sourceSub,const D3D11_BOX*);
void uiDeferredUnknownWrite(ID3D11DeviceContext*);
void uiDeferredFrameBoundary(ID3D11DeviceContext*);
void uiDeferredShutdown();
bool uiDeferredInternal();
}
