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
constexpr uint64_t kUiDeferredGlassVs=0xF512712C40D93C12ull;
constexpr uint64_t kUiDeferredGlassPs=0x4A71EB0D34E9F2EFull;
class Config;
void uiDeferredConfigure(Config&);
void uiDeferredRemember(ID3D11DeviceChild*,const void*,size_t,bool linked);
void uiDeferredBeforeDraw(ID3D11DeviceContext*, char kind, uint32_t count,
                          uint32_t instances, uint32_t start, int32_t base,
                          uint32_t startInstance, uint32_t verdict);
void uiDeferredBeforeDispatch(ID3D11DeviceContext*);
// Bounded diagnostic for the real draw wrapper. It retains a rolling history
// of eye-sized LDR targets so a sampled post-tone draw can name the draw that
// last produced its source. `verdict` is the wrapper's opaque DrawVerdict
// value; no rendering decision is made here.
void uiDeferredTraceDrawEnter(ID3D11DeviceContext*, bool eyeSizedTarget,
                              char kind, uint32_t count, uint32_t instances, uint32_t verdict,
                              uint64_t originalVs, uint64_t originalPs);
void uiDeferredTraceBeforeTone(ID3D11DeviceContext*);
void uiDeferredTraceOriginalIssued();
// Captures supported UI for native replay, or mirrors an interleaved world
// draw into the clean image. Original colour/depth/stencil remain intact.
// End must follow every Begin, including world draws returning false.
bool uiDeferredBegin(ID3D11DeviceContext*, int eye, char kind, uint32_t count,
                     uint32_t instances, uint32_t start, int32_t base,
                     uint32_t startInstance, uint32_t verdict=0,
                     bool countingQuery=false);
// Arms the exact retained dual-source glass replay after the original draw
// was issued. A true result means the caller must issue the same draw once
// through its raw draw pointer before uiDeferredEnd restores game state.
bool uiDeferredWorldReplayBegin(ID3D11DeviceContext*);
void uiDeferredEnd(ID3D11DeviceContext*);
void uiDeferredBeforeTone(ID3D11DeviceContext*, char kind, uint32_t count, uint32_t instances,
                          uint32_t start, int32_t base, uint32_t startInstance);
// Builds the entire output command list before publishing a clean DLSS input.
ID3D11ShaderResourceView* uiDeferredPrepare(ID3D11DeviceContext*, ID3D11Texture2D* submitted,
    ID3D11ShaderResourceView* sceneDepth, ID3D11Texture2D* output, int eye,
    uint32_t width, uint32_t height, float jitterX, float jitterY);
void uiDeferredApply(ID3D11DeviceContext*, int eye);
bool uiDeferredFallbackReset(int eye);
// A read-only snapshot of one eye's capture state, for the luma probe's
// report line: whether the feature is on, whether this eye has captured
// a post-tone draw at all (sampled), how many render targets it has had
// to alias onto the clean surfaces, how many draws are recorded, and
// whether its command list is complete (ready to replay). Never mutates
// anything; safe to call every frame regardless of capture state.
struct UiDeferredEyeState { bool enabled; bool sampled; unsigned aliases; unsigned draws; bool complete; };
UiDeferredEyeState uiDeferredEyeState(int eye);
void uiDeferredResourceWrite(ID3D11DeviceContext*, ID3D11Resource*);
void uiDeferredViewWrite(ID3D11DeviceContext*, ID3D11View*);
void uiDeferredCopy(ID3D11Resource* destination, ID3D11Resource* source, bool complete);
void uiDeferredCopyRegion(ID3D11Resource* destination,uint32_t destinationSub,uint32_t x,uint32_t y,uint32_t z,ID3D11Resource* source,uint32_t sourceSub,const D3D11_BOX*);
void uiDeferredUnknownWrite(ID3D11DeviceContext*);
void uiDeferredFrameBoundary(ID3D11DeviceContext*);
void uiDeferredShutdown();
bool uiDeferredInternal();
}
