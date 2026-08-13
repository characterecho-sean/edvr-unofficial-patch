#include "vscreen.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/vtable_hook.h"
#include "glitch_frame.h"

namespace edvr {
namespace {

// ID3D11DeviceContext vtable indices.
//
// A frozen COM ABI: IUnknown occupies 0-2, ID3D11DeviceChild 3-6, and the
// ID3D11DeviceContext methods follow in d3d11.h declaration order. None of
// these collide with the exposure fix's slots, so the two hooks coexist.
constexpr size_t kSlotVSSetConstantBuffers  = 7;
constexpr size_t kSlotPSSetShaderResources  = 8;
constexpr size_t kSlotDrawIndexed           = 12;
constexpr size_t kSlotDraw                  = 13;
constexpr size_t kSlotMap                   = 14;
constexpr size_t kSlotUnmap                 = 15;
constexpr size_t kSlotDrawIndexedInstanced  = 20;
constexpr size_t kSlotDrawInstanced         = 21;
constexpr size_t kSlotOMSetRenderTargets    = 33;
// The other way to bind render targets. Same effect on slot 0, and it is the
// call an engine makes whenever a UAV is bound alongside -- so leaving it out
// meant the binding could change without us seeing it.
constexpr size_t kSlotOMSetRtvAndUav        = 34;
constexpr size_t kSlotClearRenderTargetView = 50;
// The two calls that drop every binding at once without naming any of them.
//
// Neither was hooked, so after either one curRtv0/curPsSrv0 still named views
// the context had just released, and the answers about them stayed "known".
// A draw after a mid-frame ClearState was still treated as the panel composite
// -- measured at 2 overrides per frame where there should be 1 -- and the
// restore then bound a constant buffer the game had deliberately unbound.
//
// ExecuteCommandList is the same story: a command list carries its own state,
// and unless RestoreContextState is TRUE the context comes back cleared.
constexpr size_t kSlotExecuteCommandList    = 58;
constexpr size_t kSlotClearState            = 110;
constexpr size_t kHighestSlotUsed           = 110;

typedef void(STDMETHODCALLTYPE* PFN_SetConstantBuffers)(ID3D11DeviceContext*, UINT, UINT,
                                                        ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetShaderResources)(ID3D11DeviceContext*, UINT, UINT,
                                                        ID3D11ShaderResourceView* const*);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT,
                                                   UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT,
                                                          UINT, INT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                            D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D11DeviceContext*, UINT,
                                                        ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_ClearRtv)(ID3D11DeviceContext*,
                                              ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRtvAndUav)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
    UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_ClearState)(ID3D11DeviceContext*);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandList)(ID3D11DeviceContext*,
                                                        ID3D11CommandList*, BOOL);

struct State {
    VTableHook hook;

    PFN_SetConstantBuffers   realVSSetConstantBuffers = nullptr;
    PFN_SetShaderResources   realPSSetShaderResources = nullptr;
    PFN_Draw                 realDraw = nullptr;
    PFN_DrawIndexed          realDrawIndexed = nullptr;
    PFN_DrawInstanced        realDrawInstanced = nullptr;
    PFN_DrawIndexedInstanced realDrawIndexedInstanced = nullptr;
    PFN_Map                  realMap = nullptr;
    PFN_Unmap                realUnmap = nullptr;
    PFN_OMSetRenderTargets   realOMSetRenderTargets = nullptr;
    PFN_OMSetRtvAndUav       realOMSetRtvAndUav = nullptr;
    PFN_ClearRtv             realClearRtv = nullptr;
    PFN_ClearState           realClearState = nullptr;
    PFN_ExecuteCommandList   realExecuteCommandList = nullptr;
    bool sawClearState = false;
    bool sawExecuteCommandList = false;

    // How long the learned panel buffer has gone without being used, and how
    // many times we have given up on one.
    //
    // compositeCb is learned exactly once, behind `if (!compositeCb)`. If the
    // game destroys that buffer and makes another at a DIFFERENT address, the
    // learn site can never fire again and the panel distance fix is dead for the
    // session with nothing said. A stall is the only symptom available from in
    // here, so a long enough one drops the pointer and lets it re-learn.
    //
    // Costs at most one frame of override when it fires, and firing while the
    // player is simply not on foot is harmless for the same reason.
    uint64_t overridesAtLastCheck = 0;
    uint32_t framesSinceOverride = 0;
    uint32_t relearns = 0;

    bool  blackVoid = true;
    bool  distanceEnabled = false;
    float distanceScale = 1.0f;
    uint32_t distanceIndex = 47;

    void* curRtv0 = nullptr;
    void* curVsCb0 = nullptr;
    // Slot 0 of the pixel shader's resources: what the draw is sampling. The
    // panel composite reads the panel, and that is how it is recognised.
    void* curPsSrv0 = nullptr;

    // The panel's transform, as the game last wrote it. Captured from the Unmap
    // the game wrote it through, so reading it costs nothing.
    void*    compositeCb = nullptr;
    uint8_t  shadow[512] = {};
    uint32_t shadowBytes = 0;
    ID3D11Buffer* ourCb = nullptr;
    uint32_t ourCbBytes = 0;

    void*    mappedResource = nullptr;
    void*    mappedData = nullptr;
    uint32_t mappedBytes = 0;

    // A second mapped buffer, tracked only so the transition-flash detector can
    // read the camera the game wrote. Separate from the pair above because the
    // panel transform and the scene camera live in different buffers and can be
    // mapped at the same time -- sharing one slot let whichever unmapped second
    // overwrite the other's record.
    void*    camResource = nullptr;
    void*    camData = nullptr;
    uint32_t camBytes = 0;

    uint32_t eyeDrawsThisFrame = 0;
    uint32_t eyeDrawsLastFrame = 0;
    // Keep counting eye draws even when the panel distance fix is off, because
    // the transition-flash detector cannot act without the count.
    bool     countForFlashFix = false;
    uint64_t voidClears = 0;
    // Grey voids forced to black in one frame, and the smallest and largest
    // counts seen SINCE THE LAST REPORT.
    //
    // Both eyes are cleared the same way, so a healthy window has one count and
    // repeats it: min == max. A window where they differ had frames that treated
    // one eye and not the other, which is what a one-eye grey void looks like
    // from in here -- and it is worth being able to read that off a log instead
    // of asking whether it looked right.
    //
    // Per report window, not per session. Session-wide extremes never recover: a
    // single odd frame during a mode change pins the low end at 1 and every
    // later report then accuses the fix of a fault that stopped happening
    // minutes ago. A window that resets says what is true NOW, which is the only
    // thing a reader can act on.
    uint32_t voidThisFrame = 0;
    uint32_t voidFrameMin = 0xFFFFFFFFu;
    uint32_t voidFrameMax = 0;
    // Largest eye-draw count seen. At 1920x1080 nothing but the composite is
    // eye-sized and this stays at 2; if raising the panel scales intermediate
    // targets past 2048 they get miscounted and this climbs, which is the
    // suspected reason the distance fix dies above the stock resolution.
    uint32_t eyeDrawsMax = 0;
    uint32_t totalsTick = 0;
    uint32_t panelMissW[8] = {}, panelMissH[8] = {};
    uint32_t panelMissCount = 0;
    uint64_t panelOverrides = 0;

    // Answers about the CURRENTLY BOUND views, computed on first use and thrown
    // away the moment the binding changes. -1 unknown, 0 no, 1 yes.
    //
    // These replace two maps keyed by view pointer. Nothing keyed by a view
    // pointer can be kept across a binding: D3D reuses freed addresses, so an
    // entry outlives its view and then answers for a different one. Both maps
    // did exactly that in shipped builds -- panelSrcCache in 0.5.2, which left
    // one eye at the wrong panel distance, and eyeSizedCache in 0.5.2 as well,
    // which left one eye's void grey after an external-camera/on-foot switch
    // recreated the eye textures.
    //
    // A bound view cannot be destroyed while it is bound -- the context holds a
    // reference to it -- so an answer is true for as long as the binding it was
    // computed from lasts. That is the invariant, and it is only worth as much as
    // our ability to SEE the binding end.
    //
    // Which is where the first version of this was wrong. It claimed the point
    // was settled and stale entries were no longer a question. But OMSetRenderTargets
    // is not the only way a binding ends: OMSetRenderTargetsAndUnorderedAccessViews,
    // ClearState and ExecuteCommandList all end one without naming it, and none
    // of the three were hooked. All four are hooked now, and the answers are
    // dropped once a frame regardless, so an end we still cannot see costs one
    // frame rather than the session.
    int8_t curRtv0Eye = -1;
    int8_t curPsSrv0Panel = -1;

    // The size the panel has been raised to, or 0 when it has not been. Used to
    // keep the panel out of the eye-draw count -- see targetIsEyeSized.
    uint32_t panelW = 0, panelH = 0;
};

State* g_state = nullptr;

// One budget per thing that can fail, not one for the file.
//
// A budget that is exhausted stops running its body at all, so sharing one
// across unrelated features means a fault in any of them switches off all of
// them. That happened: a bad camera_buffer_offset faulted in the transition
// flash reader, burned the shared budget, and the black void fix -- which has
// nothing to do with it -- stopped clearing, silently, with a log line naming
// only "vScreen". Splitting them also makes the FEATURE-DISABLED line say which
// one actually failed.
FaultBudget g_viewBudget("vScreen.views", 5);        // resolving a bound view
FaultBudget g_panelCbBudget("vScreen.panelBuffer", 5);  // reading the panel's transform
FaultBudget g_cameraBudget("vScreen.cameraRead", 5);    // reading the scene camera

// Is this render target one of the two textures sent to the headset?
//
// By size. The graphics layer never sees what is submitted to the headset, and
// nothing else in an Elite frame is drawn into at 2048x2048 or larger.
//
// Uncached. ClearRenderTargetView is handed a live view, so that caller is safe
// by construction. The draw path asks about the view it last SAW bound, which is
// a weaker thing -- hence the guard below rather than a claim. Callers that ask
// repeatedly remember the answer for as long as the binding lasts, see curRtv0Eye.
bool targetIsEyeSized(void* rtv) {
    State* s = g_state;
    if (!rtv) return false;

    bool out = false;
    // Under SEH, because the pointer is only as good as the binding that named
    // it. The hooks below see every rebind we know how to see, but not every one
    // that exists -- ClearState and ExecuteCommandList both drop the binding
    // without passing through here -- so a released view can still be named by
    // curRtv0 for the rest of a frame. Resolving it then reads freed memory.
    // The fix for that is the frame-boundary reset in vScreenFrameBoundary; this
    // is what turns the remaining window into a skipped draw instead of a crash.
    guardedBudget(g_viewBudget, [&] {
    ID3D11Resource* res = nullptr;
    static_cast<ID3D11View*>(rtv)->GetResource(&res);
    if (res) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            out = d.Width >= 2048 && d.Height >= 2048;
            // ...except the on-foot panel itself, once it has been raised.
            //
            // The comment above this test says nothing else in an Elite frame is
            // drawn into at 2048x2048 or larger. That was true when it was
            // written, and the resolution fix made it false: at 3840x2160 the
            // panel clears the threshold on both axes, so every scene draw into
            // it is counted as an eye draw. eyeDrawsLastFrame goes from 3 to
            // hundreds, the panel composite is never recognised, and the panel
            // distance fix silently stops working -- at 4K but not at 2880x1620,
            // whose height is still under 2048.
            //
            // Excluded by value rather than by raising the threshold, which
            // would only defer the same collision to the next size someone picks.
            if (out && s->panelW && d.Width == s->panelW && d.Height == s->panelH) {
                out = false;
            }
        }
        res->Release();
    }
    });  // guardedBudget
    return out;
}

// A flat, non-black grey -- what the void around the panel is cleared to.
// Matched by shape rather than by the exact value, so a game update that picks
// a different grey still matches, and one that already clears to black needs no
// help.
bool isFlatGrey(const FLOAT c[4]) {
    if (c[0] <= 0.0f || c[0] >= 0.5f) return false;
    return fabsf(c[0] - c[1]) < 1e-4f && fabsf(c[1] - c[2]) < 1e-4f;
}

// Does this draw sample the on-foot panel?
//
// The panel is whatever size the game forces for that view mode -- 1920x1080 by
// default, or the raised size when the resolution fix is on. Nothing else an
// eye-sized draw samples has exactly those dimensions.
bool srv0IsPanelSized(State* s) {
    if (!s->curPsSrv0) return false;
    if (s->curPsSrv0Panel >= 0) return s->curPsSrv0Panel != 0;
    const uint32_t w = s->panelW ? s->panelW : 1920;
    const uint32_t h = s->panelH ? s->panelH : 1080;

    bool out = false;
    uint32_t lastW = 0, lastH = 0;
    // Guarded for the same reason as targetIsEyeSized: a view we last saw bound
    // is not proof of a view that is still alive.
    guardedBudget(g_viewBudget, [&] {
        ID3D11Resource* res = nullptr;
        static_cast<ID3D11View*>(s->curPsSrv0)->GetResource(&res);
        if (res) {
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            res->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d{};
                static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
                lastW = d.Width;
                lastH = d.Height;
                out = (d.Width == w && d.Height == h);
            }
            res->Release();
        }
    });
    // What an eye-sized draw sampled when it was NOT the panel.
    //
    // In HMD Cinema Mode the override applies once a frame rather than twice, so
    // one of the two composite draws reads something else -- and one eye is
    // corrected while the other is not. Guessing at what it reads has been the
    // expensive move all day; this records the sizes and says them once.
    //
    // "Once" means once per distinct size, up to eight, which is what the table
    // below enforces. A panelMissNoted flag used to appear in this condition; it
    // was never assigned anywhere, so it said nothing about anything.
    if (!out && s->panelMissCount < 8) {
        bool known = false;
        for (uint32_t i = 0; i < s->panelMissCount; ++i) {
            if (s->panelMissW[i] == lastW && s->panelMissH[i] == lastH) { known = true; break; }
        }
        if (!known) {
            s->panelMissW[s->panelMissCount] = lastW;
            s->panelMissH[s->panelMissCount] = lastH;
            ++s->panelMissCount;
            Log::get().note("vScreen: an eye-sized draw sampled %ux%u, which is not the "
                            "panel (%ux%u), so it was left alone. If a mode corrects only "
                            "one eye, this is what the other one is reading.",
                            lastW, lastH, w, h);
        }
    }

    // Remembered only until this view is unbound -- see curPsSrv0Panel.
    s->curPsSrv0Panel = out ? 1 : 0;
    return out;
}

// Swaps in a modified copy of the panel's transform for one draw.
//
// Nothing of the game's is written to. Its own values are copied into a buffer
// of ours with one number changed, ours is used for the draw, and the original
// is put back immediately after.
bool beginPanelOverride(ID3D11DeviceContext* self) {
    State* s = g_state;
    // Counting eye draws is not part of the panel distance fix, even though it
    // happens here.
    //
    // The transition-flash detector needs this count to tell a rendered scene
    // from a menu, and it is the only place the count can be taken. It used to
    // sit below the distanceEnabled test, so with panel_distance at its shipped
    // default of 1.0 nothing counted, the count stayed 0, and the flash fix --
    // which is on by default and asks the user to replace a file in their game
    // install -- never withheld a single frame. It reported itself as armed
    // throughout. Two features that have nothing to do with each other, and one
    // silently switched the other off.
    if (!s->distanceEnabled && !s->countForFlashFix) return false;
    if (s->curRtv0Eye < 0) s->curRtv0Eye = targetIsEyeSized(s->curRtv0) ? 1 : 0;
    if (s->curRtv0Eye == 0) return false;
    ++s->eyeDrawsThisFrame;
    if (!s->distanceEnabled) return false;

    // An eye-sized target is not enough on its own: in the cockpit hundreds of
    // draws land in those textures and rebinding on all of them would corrupt
    // the view. So this draw has to be the panel composite -- and it is
    // recognised by WHAT IT READS, not by how many draws the frame made.
    //
    // The count was the old rule and it never worked on foot. In HMD Cinema Mode
    // the composite is 2 draws into the eye textures and the count passed; on
    // foot for real the helmet HUD is drawn into the eye textures too -- about
    // 60 draws, and 1174 measured in one frame -- so it rejected every frame and
    // the distance setting did nothing. It was verified in Cinema Mode, which
    // shares this rendering path, and shipped.
    //
    // The composite reads the panel: a texture of exactly the size the game
    // forces for that view mode. HUD draws read glyph sheets and atlases, so
    // they are excluded however many of them there are.
    if (!srv0IsPanelSized(s)) return false;

    void* cb = s->curVsCb0;
    if (!cb) return false;
    if (!s->compositeCb) {
        // Learn it now; its contents arrive with the next write, so the override
        // starts a frame later rather than acting on data we do not have.
        s->compositeCb = cb;
        return false;
    }
    if (cb != s->compositeCb || s->shadowBytes == 0) return false;
    // 64-bit, because the operands are not.
    //
    // panel_distance_index is read with strtol and cast to uint32_t, so a
    // negative in the ini arrives as a huge positive: -1 becomes 0xFFFFFFFF,
    // and 0xFFFFFFFF * 4u + 4u wraps to exactly 0. The check then passed and
    // the write below went about 16 GB past a 256-byte buffer. edvr.ini invites
    // the user to change this number if a game update moves the field, so it is
    // reachable from a documented, hand-edited setting -- and nothing here is
    // wrapped in guarded(), so it took the process down rather than degrading.
    if (static_cast<uint64_t>(s->distanceIndex) * 4ull + 4ull > s->shadowBytes) return false;

    const uint32_t bytes = s->shadowBytes;
    if (!s->ourCb || s->ourCbBytes != bytes) {
        ID3D11Device* dev = nullptr;
        self->GetDevice(&dev);
        if (!dev) return false;
        if (s->ourCb) { s->ourCb->Release(); s->ourCb = nullptr; }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT hr = dev->CreateBuffer(&bd, nullptr, &s->ourCb);
        dev->Release();
        if (FAILED(hr) || !s->ourCb) { s->ourCb = nullptr; return false; }
        s->ourCbBytes = bytes;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(s->realMap(self, s->ourCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        return false;
    }
    memcpy(m.pData, s->shadow, bytes);
    static_cast<float*>(m.pData)[s->distanceIndex] *= s->distanceScale;
    s->realUnmap(self, s->ourCb, 0);

    ID3D11Buffer* ours = s->ourCb;
    s->realVSSetConstantBuffers(self, 0, 1, &ours);
    if (++s->panelOverrides == 1) {
        Log::get().note("vScreen: panel distance x%.3f applied", s->distanceScale);
    }
    return true;
}

void endPanelOverride(ID3D11DeviceContext* self) {
    State* s = g_state;
    ID3D11Buffer* orig = static_cast<ID3D11Buffer*>(s->compositeCb);
    s->realVSSetConstantBuffers(self, 0, 1, &orig);
}

// --- hooks ------------------------------------------------------------------

void STDMETHODCALLTYPE hookedClearRtv(ID3D11DeviceContext* self,
                                      ID3D11RenderTargetView* rtv, const FLOAT c[4]) {
    State* s = g_state;
    // Cheap test first: four float compares, and only a match pays to resolve
    // the target.
    if (s->blackVoid && rtv && c && isFlatGrey(c) && targetIsEyeSized(rtv)) {
        const FLOAT black[4] = {0.0f, 0.0f, 0.0f, c[3]};
        ++s->voidThisFrame;
        if (++s->voidClears == 1) {
            Log::get().note("vScreen: void %.3f,%.3f,%.3f forced to black",
                            c[0], c[1], c[2]);
        }
        s->realClearRtv(self, rtv, black);
        return;
    }
    s->realClearRtv(self, rtv, c);
}

void STDMETHODCALLTYPE hookedOMSetRenderTargets(ID3D11DeviceContext* self, UINT n,
                                                ID3D11RenderTargetView* const* rtvs,
                                                ID3D11DepthStencilView* dsv) {
    g_state->curRtv0 = (n && rtvs) ? rtvs[0] : nullptr;
    // Unconditionally, even when the pointer looks unchanged: an identical
    // address after a rebind is not evidence of an identical view.
    g_state->curRtv0Eye = -1;
    g_state->realOMSetRenderTargets(self, n, rtvs, dsv);
}

void STDMETHODCALLTYPE hookedOMSetRtvAndUav(ID3D11DeviceContext* self, UINT n,
                                            ID3D11RenderTargetView* const* rtvs,
                                            ID3D11DepthStencilView* dsv, UINT uavStart,
                                            UINT uavCount,
                                            ID3D11UnorderedAccessView* const* uavs,
                                            const UINT* counts) {
    // D3D11_KEEP_RENDER_TARGETS_UNCHANGED asks for the UAVs to be set while the
    // render targets are left alone, so it says nothing about slot 0 and must
    // not be treated as a rebind. Spelled out rather than named: the SDK header
    // this builds against does not define the constant.
    constexpr UINT kKeepRenderTargetsUnchanged = 0xFFFFFFFFu;
    if (n != kKeepRenderTargetsUnchanged) {
        g_state->curRtv0 = (n && rtvs) ? rtvs[0] : nullptr;
        g_state->curRtv0Eye = -1;
    }
    g_state->realOMSetRtvAndUav(self, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
}

void STDMETHODCALLTYPE hookedPSSetShaderResources(ID3D11DeviceContext* self, UINT start,
                                                  UINT n,
                                                  ID3D11ShaderResourceView* const* srvs) {
    if (start == 0 && n && srvs) {
        g_state->curPsSrv0 = srvs[0];
        g_state->curPsSrv0Panel = -1;
    }
    g_state->realPSSetShaderResources(self, start, n, srvs);
}

void STDMETHODCALLTYPE hookedVSSetConstantBuffers(ID3D11DeviceContext* self, UINT start,
                                                  UINT n, ID3D11Buffer* const* bufs) {
    if (start == 0 && n && bufs) g_state->curVsCb0 = bufs[0];
    g_state->realVSSetConstantBuffers(self, start, n, bufs);
}

// Everything is unbound. Forget all of it -- this is the one place where
// forgetting the pointers is the truth rather than a guess.
void forgetBindings(State* s) {
    s->curRtv0 = nullptr;
    s->curPsSrv0 = nullptr;
    s->curVsCb0 = nullptr;
    s->curRtv0Eye = -1;
    s->curPsSrv0Panel = -1;
}

// Both of these say so the first time they run.
//
// Their vtable slots were derived by counting declaration order, not measured,
// and a miscount would silently replace an unrelated method -- FinishCommandList
// and ClearState are neighbours in that table. One line each is what turns "the
// count looks right" into evidence, on whatever machine the log came from.
void STDMETHODCALLTYPE hookedClearState(ID3D11DeviceContext* self) {
    State* s = g_state;
    if (!s->sawClearState) {
        s->sawClearState = true;
        Log::get().note("vScreen: ClearState seen (slot %zu); bindings dropped with it",
                        kSlotClearState);
    }
    forgetBindings(s);
    s->realClearState(self);
}

void STDMETHODCALLTYPE hookedExecuteCommandList(ID3D11DeviceContext* self,
                                                ID3D11CommandList* list,
                                                BOOL restoreContextState) {
    State* s = g_state;
    if (!s->sawExecuteCommandList) {
        s->sawExecuteCommandList = true;
        Log::get().note("vScreen: ExecuteCommandList seen (slot %zu, restore=%d). Draws "
                        "recorded on a deferred context replay past every hook here, so "
                        "they are neither counted nor corrected.",
                        kSlotExecuteCommandList, restoreContextState ? 1 : 0);
    }
    s->realExecuteCommandList(self, list, restoreContextState);
    // After the call, and only when the context was not restored: with
    // RestoreContextState TRUE the bindings we recorded are put back, so
    // dropping them would cost the fix a frame for no reason.
    if (!restoreContextState) forgetBindings(s);
}

HRESULT STDMETHODCALLTYPE hookedMap(ID3D11DeviceContext* self, ID3D11Resource* res,
                                    UINT sub, D3D11_MAP type, UINT flags,
                                    D3D11_MAPPED_SUBRESOURCE* mapped) {
    State* s = g_state;
    const HRESULT hr = s->realMap(self, res, sub, type, flags, mapped);
    // Only the one buffer we care about, so this is a pointer compare on a very
    // hot path and nothing more.
    if (SUCCEEDED(hr) && mapped && sub == 0 && res == s->compositeCb) {
        // GetType FIRST here too, for the reason spelled out in the branch below.
        //
        // This branch was the one that did not do it. compositeCb is a raw
        // pointer with no reference held, so after the game destroys that buffer
        // the address can come back as a TEXTURE -- at which point
        // ID3D11Buffer::GetDesc writes 44 bytes of texture description into the
        // 20-byte local below. That is a /GS stack-smash fast-fail: not an
        // exception, not catchable by SEH, and this hook has no guard anyway.
        // The neighbouring branch carried the warning and the fix; this one
        // carried neither.
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
            if (d.ByteWidth <= sizeof(s->shadow)) {
                s->mappedResource = res;
                s->mappedData = mapped->pData;
                s->mappedBytes = d.ByteWidth;
            }
        } else {
            // The address is no longer our buffer. Forget it so the next
            // composite draw can learn the real one.
            s->compositeCb = nullptr;
            s->shadowBytes = 0;
        }
    } else if (SUCCEEDED(hr) && mapped && sub == 0 && res) {
        // The scene camera buffer, for the transition-flash detector, recognised
        // by size.
        //
        // GetType FIRST. Map is called on textures as well as buffers, and
        // ID3D11Buffer::GetDesc and ID3D11Texture2D::GetDesc occupy the same
        // vtable slot on their respective interfaces -- so calling the buffer
        // one on a texture writes a 44-byte texture description into the
        // 20-byte buffer description below. That is a stack smash, and it
        // brought the whole process down on the first frame.
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
            if (glitchFrameWantsBuffer(d.ByteWidth)) {
                s->camResource = res;
                s->camData = mapped->pData;
                s->camBytes = d.ByteWidth;
            }
        }
    }
    return hr;
}

void STDMETHODCALLTYPE hookedUnmap(ID3D11DeviceContext* self, ID3D11Resource* res,
                                   UINT sub) {
    State* s = g_state;
    // Read before forwarding: after the real Unmap the memory is no longer ours
    // to look at.
    if (res == s->mappedResource && s->mappedData) {
        guardedBudget(g_panelCbBudget, [&] {
            memcpy(s->shadow, s->mappedData, s->mappedBytes);
            s->shadowBytes = s->mappedBytes;
        });
        s->mappedResource = nullptr;
        s->mappedData = nullptr;
        s->mappedBytes = 0;
    }
    if (res == s->camResource && s->camData) {
        // Same rule as above: read before forwarding, because after the real
        // Unmap the memory is no longer ours to look at.
        guardedBudget(g_cameraBudget, [&] { glitchFrameObserve(s->camData, s->camBytes); });
        s->camResource = nullptr;
        s->camData = nullptr;
        s->camBytes = 0;
    }
    s->realUnmap(self, res, sub);
}

void STDMETHODCALLTYPE hookedDraw(ID3D11DeviceContext* self, UINT count, UINT start) {
    const bool on = beginPanelOverride(self);
    g_state->realDraw(self, count, start);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawIndexed(ID3D11DeviceContext* self, UINT count,
                                         UINT startIndex, INT baseVertex) {
    const bool on = beginPanelOverride(self);
    g_state->realDrawIndexed(self, count, startIndex, baseVertex);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawInstanced(ID3D11DeviceContext* self, UINT perInstance,
                                           UINT instances, UINT startVertex,
                                           UINT startInstance) {
    const bool on = beginPanelOverride(self);
    g_state->realDrawInstanced(self, perInstance, instances, startVertex, startInstance);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawIndexedInstanced(ID3D11DeviceContext* self,
                                                  UINT perInstance, UINT instances,
                                                  UINT startIndex, INT baseVertex,
                                                  UINT startInstance) {
    const bool on = beginPanelOverride(self);
    g_state->realDrawIndexedInstanced(self, perInstance, instances, startIndex, baseVertex,
                                      startInstance);
    if (on) endPanelOverride(self);
}

// Read panel_distance_index, refusing anything that cannot be a float index.
//
// getInt goes through strtol, so the ini can supply a negative or something
// enormous, and a cast to uint32_t turns both into a huge unsigned. The shadow
// buffer holds sizeof(shadow)/4 floats and nothing outside that range can ever
// be the field we want, so the value is rejected here rather than defended
// against at the point of the write.
uint32_t readDistanceIndex(Config& cfg) {
    constexpr int kMaxIndex = static_cast<int>(sizeof(State::shadow) / sizeof(float)) - 1;
    const int raw = cfg.getInt("advanced.panel_distance_index", 47);
    if (raw < 0 || raw > kMaxIndex) {
        Log::get().note("vScreen: panel_distance_index = %d is outside 0..%d and cannot "
                        "be a position in that buffer. Using 47. Panel distance is "
                        "unaffected by the bad value rather than acting on it.",
                        raw, kMaxIndex);
        return 47u;
    }
    return static_cast<uint32_t>(raw);
}

}  // namespace

void vScreenSetPanelSize(uint32_t width, uint32_t height) {
    State* s = g_state;
    if (!s || !width || !height) return;
    s->panelW = width;
    s->panelH = height;

    // Only worth a line when it is a size that collides with the eye-texture
    // test, because that is the case that used to break the fix silently.
    if (width >= 2048 && height >= 2048) {
        Log::get().note(
            "vScreen: the panel renders at %ux%u, so targets of exactly that size are "
            "NOT counted as eye textures. Without that exclusion every scene draw into "
            "the panel is mistaken for an eye draw and the panel distance fix stops "
            "matching.",
            width, height);
    } else {
        Log::get().note("vScreen: the panel renders at %ux%u", width, height);
    }
}

void vScreenRefreshConfig() {
    State* s = g_state;
    if (!s) return;
    Config& cfg = Config::get();
    // Cheap: one GetFileAttributesEx, and only when the write time moved.
    if (!cfg.reloadIfChanged()) return;

    const bool  wasVoid  = s->blackVoid;
    const float wasScale = s->distanceScale;

    s->blackVoid = cfg.getBool("fix.black_void", true);
    s->distanceScale = cfg.getFloat("fix.panel_distance", 1.0f);
    s->distanceEnabled = s->distanceScale != 1.0f;
    s->distanceIndex = readDistanceIndex(cfg);
    s->countForFlashFix = glitchFrameNeedsEyeDraws();

    if (wasVoid != s->blackVoid || wasScale != s->distanceScale) {
        Log::get().note("vScreen config reloaded: black void %s, panel distance x%.3f "
                        "(index %u)",
                        s->blackVoid ? "on" : "off", s->distanceScale, s->distanceIndex);
    }
}

void vScreenFrameBoundary() {
    State* s = g_state;
    if (!s) return;

    // Re-decide what the bound views are, once a frame -- but do NOT forget what
    // is bound.
    //
    // Nulling the pointers here was a bug. Nothing re-acquires them: they are
    // only ever written from OMSetRenderTargets and its UAV variant, so a game
    // that binds a target once and then draws for many frames loses the fix
    // after the first Present and never gets it back. Measured on a harness
    // that binds once and then draws each frame: 1800 overrides before that
    // change, 1 after it. Not "the first draws of a frame" -- the rest of the
    // session.
    //
    // Dropping the ANSWERS is still worth doing, and is all that was wanted: it
    // is the one-frame bound the deleted panelSrcCache clear used to provide by
    // accident. The pointers themselves are dropped by the hooks below, at the
    // points where the binding genuinely goes away.
    s->curRtv0Eye = -1;
    s->curPsSrv0Panel = -1;

    // Re-asked every frame, not on config reload. The answer changes when the
    // detector gives up on itself, which is not something an ini edit causes --
    // and vScreenRefreshConfig returns early unless the file's write time moved,
    // so asking there would never see it. One bool call a frame.
    s->countForFlashFix = glitchFrameNeedsEyeDraws();

    // Give up on a learned panel buffer that has stopped being used.
    //
    // 600 frames is about seven seconds at 90Hz -- long enough that walking
    // around, menus and loading do not trip it, short enough that a buffer
    // recreated at a new address costs seconds rather than the session. The
    // relearn cap stops this becoming per-second churn if the fix is simply
    // never going to match on this build.
    if (s->distanceEnabled && s->compositeCb) {
        if (s->panelOverrides != s->overridesAtLastCheck) {
            s->overridesAtLastCheck = s->panelOverrides;
            s->framesSinceOverride = 0;
        } else if (++s->framesSinceOverride >= 600) {
            s->framesSinceOverride = 0;
            s->compositeCb = nullptr;
            s->shadowBytes = 0;
            if (++s->relearns <= 3) {
                Log::get().note(
                    "vScreen: the panel's transform buffer has gone 600 frames unused, so "
                    "it is being forgotten and learned again. Expected when you are not on "
                    "foot; if it repeats while the panel IS visible, the buffer is being "
                    "recreated and the distance fix was silently dead before this.");
            }
        }
    }

    if (s->eyeDrawsThisFrame > s->eyeDrawsMax) s->eyeDrawsMax = s->eyeDrawsThisFrame;

    // Frames that forced nothing are menus and loading screens, not asymmetry.
    if (s->voidThisFrame) {
        if (s->voidThisFrame < s->voidFrameMin) s->voidFrameMin = s->voidThisFrame;
        if (s->voidThisFrame > s->voidFrameMax) s->voidFrameMax = s->voidThisFrame;
    }
    s->voidThisFrame = 0;

    // Report the totals periodically rather than at shutdown.
    //
    // shutdownVScreenFixes only runs on FreeLibrary, and a game closing is
    // process termination -- so anything logged there is never seen. That is why
    // this log has no shutdown lines at all, and why a totals line written there
    // produced nothing after a full session.
    if (++s->totalsTick >= 1800) {
        s->totalsTick = 0;
        Log::get().note(
            "vScreen totals: panel distance applied %llu time(s), void cleared to black "
            "%llu time(s) (%u-%u per frame over the last 1800), largest eye-draw count "
            "%u. Two eyes a frame, so these should climb steadily; if they stop, the fix "
            "engaged once and then stopped matching. The per-frame void range should be "
            "a single number repeated -- a low end below the high end means some frames "
            "in THIS window treated one eye and not the other. The eye-draw count is 2 "
            "at the stock panel resolution, and much larger means intermediate targets "
            "are being miscounted as eye textures.",
            static_cast<unsigned long long>(s->panelOverrides),
            static_cast<unsigned long long>(s->voidClears),
            s->voidFrameMin == 0xFFFFFFFFu ? 0u : s->voidFrameMin, s->voidFrameMax,
            s->eyeDrawsMax);
        // Reset for the next window. A session-wide extreme never recovers: one
        // odd frame during a mode change pins the low end at 1 and every later
        // report then accuses the fix of a fault that stopped happening long
        // ago. The reader needs to know what is true now.
        s->voidFrameMin = 0xFFFFFFFFu;
        s->voidFrameMax = 0;
    }

    s->eyeDrawsLastFrame = s->eyeDrawsThisFrame;
    // The flash detector needs the count for the frame that just ended, to tell
    // a rendered scene from a menu. It has to be told before the counter resets.
    glitchFrameBoundary(s->eyeDrawsLastFrame);
    s->eyeDrawsThisFrame = 0;
}

void installVScreenFixes(ID3D11Device* device) {
    if (!device || g_state) return;

    Config& cfg = Config::get();
    const bool wantVoid = cfg.getBool("fix.black_void", true);
    const float scale = cfg.getFloat("fix.panel_distance", 1.0f);
    // Install the hooks whenever EITHER fix could be wanted now or later. Both
    // are documented as changeable while the game runs, and a hook that was
    // never installed cannot be switched on by editing a file -- so returning
    // here on "nothing asked for" would make the documented behaviour impossible
    // for anyone who starts with both off.
    if (!wantVoid && scale == 1.0f && !cfg.getBool("advanced.panel_hooks_always", true)) {
        return;
    }

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;

    g_state = new State();
    g_state->blackVoid = wantVoid;
    g_state->distanceScale = scale;
    g_state->distanceEnabled = scale != 1.0f;
    g_state->distanceIndex = readDistanceIndex(cfg);
    // installGlitchFrameFix is called before this, deliberately, so this is its
    // settled answer rather than a guess about config it has not read yet.
    g_state->countForFlashFix = glitchFrameNeedsEyeDraws();

    // The panel size is NOT read from config here.
    //
    // It arrives through vScreenSetPanelSize once the resolution patch has run
    // and its result is known -- see the header for why the requested value is
    // the wrong thing to trust. Until then panelW/H stay 0 and the recogniser
    // falls back to the stock 1920x1080, which is correct for every session
    // that never asks for anything else.

    State& s = *g_state;
    if (!s.hook.attach(ctx) || s.hook.executablePrefix() <= kHighestSlotUsed) {
        Log::get().note("vScreen: context vtable unusable; not installing");
        s.hook.uninstall();
        ctx->Release();
        // g_state back to null, not merely leaked. Leaving it set makes the
        // guard at the top of this function refuse a later attempt, and leaves
        // the periodic totals reporting on a fix that was never installed.
        delete g_state;
        g_state = nullptr;
        return;
    }

    s.hook.replace(kSlotClearRenderTargetView, &hookedClearRtv,
                   reinterpret_cast<void**>(&s.realClearRtv));
    s.hook.replace(kSlotOMSetRenderTargets, &hookedOMSetRenderTargets,
                   reinterpret_cast<void**>(&s.realOMSetRenderTargets));
    s.hook.replace(kSlotClearState, &hookedClearState,
                   reinterpret_cast<void**>(&s.realClearState));
    s.hook.replace(kSlotExecuteCommandList, &hookedExecuteCommandList,
                   reinterpret_cast<void**>(&s.realExecuteCommandList));
    s.hook.replace(kSlotOMSetRtvAndUav, &hookedOMSetRtvAndUav,
                   reinterpret_cast<void**>(&s.realOMSetRtvAndUav));
    s.hook.replace(kSlotPSSetShaderResources, &hookedPSSetShaderResources,
                   reinterpret_cast<void**>(&s.realPSSetShaderResources));
    s.hook.replace(kSlotVSSetConstantBuffers, &hookedVSSetConstantBuffers,
                   reinterpret_cast<void**>(&s.realVSSetConstantBuffers));
    s.hook.replace(kSlotMap, &hookedMap, reinterpret_cast<void**>(&s.realMap));
    s.hook.replace(kSlotUnmap, &hookedUnmap, reinterpret_cast<void**>(&s.realUnmap));
    s.hook.replace(kSlotDraw, &hookedDraw, reinterpret_cast<void**>(&s.realDraw));
    s.hook.replace(kSlotDrawIndexed, &hookedDrawIndexed,
                   reinterpret_cast<void**>(&s.realDrawIndexed));
    s.hook.replace(kSlotDrawInstanced, &hookedDrawInstanced,
                   reinterpret_cast<void**>(&s.realDrawInstanced));
    s.hook.replace(kSlotDrawIndexedInstanced, &hookedDrawIndexedInstanced,
                   reinterpret_cast<void**>(&s.realDrawIndexedInstanced));

    if (!s.hook.commit()) {
        Log::get().note("vScreen: vtable commit failed; not installing");
        s.hook.uninstall();
        ctx->Release();
        delete g_state;
        g_state = nullptr;
        return;
    }

    Log::get().note("vScreen fixes installed: black void %s, panel distance %s, eye-draw "
                    "counting %s",
                    s.blackVoid ? "on" : "off",
                    s.distanceEnabled ? "on" : "off (1.0)",
                    (s.distanceEnabled || s.countForFlashFix)
                        ? "on"
                        : "OFF -- the transition flash fix cannot act without it");
    ctx->Release();
}

void shutdownVScreenFixes() {
    if (!g_state) return;

    // How often each fix actually did something.
    //
    // Both announce their FIRST application and nothing after it, so a fix that
    // engages once and then stops is indistinguishable in the log from one that
    // runs every frame. That ambiguity is costing test flights right now.
    Log::get().note("vScreen totals: panel distance applied %llu time(s), void cleared to "
                    "black %llu time(s). Two eyes a frame, so a working session is tens "
                    "of thousands of each; single digits mean it engaged once and then "
                    "stopped. Largest eye-draw count seen this session: %u -- it is 2 at "
                    "the stock panel resolution, and anything much larger means "
                    "intermediate targets are being miscounted as eye textures.",
                    static_cast<unsigned long long>(g_state->panelOverrides),
                    static_cast<unsigned long long>(g_state->voidClears),
                    g_state->eyeDrawsMax);

    g_state->distanceEnabled = false;
    if (g_state->ourCb) {
        g_state->ourCb->Release();
        g_state->ourCb = nullptr;
    }
    g_state->hook.uninstall();
}

}  // namespace edvr







