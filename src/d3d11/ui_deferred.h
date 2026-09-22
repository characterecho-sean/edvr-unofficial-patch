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

// CAN THE PER-DRAW FAMILY BELOW DO ANYTHING THIS FRAME?
//
// forwardWithVerdict calls eight of these per owner-context draw --
// TraceDrawEnter, BeforeDraw, TraceBeforeTone, BeforeTone, Begin,
// TraceOriginalIssued, WorldReplayBegin, End -- about 18k times a frame each,
// and in a session without an external temporal engine every one of them
// returned without effect: 0.33 ms a frame of cross-TU calls in the flown
// profile of 2026-09-22 (this build is /O2 with no /GL). The draw path now
// asks this once per draw and skips the family when it is false.
//
// WHY FALSE MEANS NOTHING WOULD HAPPEN. False is `enabled` off and no
// diagnostic window ever requested (diagnosticUntilGeneration == 0), so
// diagnosticWindow() is false too. Then, in ui_deferred.cpp:
//   - TraceDrawEnter returns on `!enabled && !diagnosticWindow()`.
//   - BeforeDraw returns on `!active && !diagnosticWindow()`, and `active`
//     (either eye's capture count) is zero: counts grow only inside begin(),
//     which requires `enabled`, and uiDeferredFrameBoundary zeroes them.
//   - BeforeTone reports or counts only while `observe` (the same counts, or
//     the diagnostic window) or `enabled` -- all false.
//   - Begin: begin() opens with `if(!enabled ...) return false`, so
//     `deferred` is false either way.
//   - TraceBeforeTone and TraceOriginalIssued act only on writerTracePending
//     >= 0, which uiDeferredFrameBoundary resets to -1 every frame and which
//     only TraceDrawEnter's body (not reached) sets.
//   - WorldReplayBegin and End act only on state begin() sets and End clears
//     within the same draw.
// The entry resets the family performs (writerTracePending = -1,
// routeHandledThisDraw = false) are therefore either already in their reset
// state, or -- routeHandledThisDraw after a draw that returned early -- never
// read before the next draw that runs the family resets it first, since
// BeforeDraw always precedes Begin within one call.
//
// AND IT CANNOT CHANGE MID-FRAME: `enabled` and diagnosticUntilGeneration are
// written by uiDeferredConfigure (the config poll, at Present), by
// uiDeferredShutdown, and by decline() -- which clears `enabled` but opens a
// diagnostic window in the same statement, so this stays true.
namespace detail {
extern bool g_uiDeferredEnabled;
extern uint64_t g_uiDeferredDiagnosticUntil;
}  // namespace detail
inline bool uiDeferredMayAct() {
    return detail::g_uiDeferredEnabled || detail::g_uiDeferredDiagnosticUntil != 0;
}

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
                              char kind, uint32_t count, uint32_t instances, uint32_t verdict);
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
