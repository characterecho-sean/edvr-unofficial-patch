#include "vscreen.h"
#include "head_offset_gate.h"
#include "camera_view.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>   // _snprintf_s, for the sizes list in the starvation line
#include <cstring>

#include "../common/config.h"
#include "../common/frame_flag.h"  // the eye-texture size, from the openvr half
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/vtable_hook.h"
#include "binding_shadow.h"
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

    // The context these hooks were installed for. Identity only -- it is
    // compared, never dereferenced, and no reference is held on it.
    //
    // Patching vtable entries in place hooks the CLASS, so every context
    // sharing this table arrives at our thunks: deferred contexts the game
    // creates, and the internal ones a wrapper mod like ReShade owns. Acting
    // on those would attribute another context's draws to the panel and
    // corrupt state nobody can see is wrong. See vtable_hook.h.
    ID3D11DeviceContext* ownerCtx = nullptr;

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

    // The bound views themselves live in binding_shadow, shared with the
    // exposure fix so the two cannot drift into opposite policies again. What
    // stays here is only what this file DERIVES from them, tagged with the
    // generation it was derived at.
    bool     rtv0Eye = false;
    uint32_t rtv0EyeGen = 0;
    bool     psSrv0Panel = false;
    uint32_t psSrv0PanelGen = 0;

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
    // Draws that sampled the flat on-foot panel this frame. The head-offset
    // gate's other input: the panel being composited is direct evidence of
    // on-foot first person, which is what tells it from the external camera
    // that mode turns into.
    uint32_t panelCompositeDraws = 0;
    // Frames since the hooks were installed. Only the gate's log lines use it,
    // to say when the panel was first counted.
    uint32_t frameNo = 0;
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

    // The size the panel has been raised to, or 0 when it has not been. Used to
    // keep the panel out of the eye-draw count -- see targetIsEyeSized.
    uint32_t panelW = 0, panelH = 0;

    // What openvr_api.dll says the headset is actually being given, or 0 when
    // nobody has said. Refreshed once a frame -- one interlocked read -- rather
    // than at install, because the openvr half publishes only after its Submit
    // hook validates, which is several seconds after this module installs.
    uint32_t eyeW = 0, eyeH = 0;
    bool     eyeSizeNoted = false;
    bool     collisionNoted = false;
    bool     sixteenNineEyeNoted = false;

    // Render targets that were looked at and NOT counted, and how many times
    // the panel exclusion was the reason. Diagnosis only: a recogniser that
    // never says yes is otherwise indistinguishable from a game that never drew.
    uint32_t rtSeenW[8] = {}, rtSeenH[8] = {};
    uint32_t rtSeenCount = 0;
    uint64_t panelExclusions = 0;
    bool     starvationNoted = false;
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
// Resolving a bound view had a third budget here. It moved to binding_shadow
// with the probe itself, which is the right place for it: the exposure fix runs
// the same probe, and a fault resolving a view should disable view resolution
// for both rather than one fix's copy path.
FaultBudget g_panelCbBudget("vScreen.panelBuffer", 5);  // reading the panel's transform
FaultBudget g_cameraBudget("vScreen.cameraRead", 5);    // reading the scene camera

// Is this target the shape of the on-foot panel rather than of an eye?
//
// 16:9 exactly. Elite's panel is 16:9 at every resolution the ini allows and
// at its stock 1920x1080; per-eye render targets are square-ish or taller
// than wide on every headset measured. Integer cross-multiply so there is no
// float tolerance to widen it.
bool isPanelShaped(uint32_t w, uint32_t h) {
    return h != 0 && w * 9u == h * 16u;
}

// Remember a target that was NOT counted, so the log can say what was seen.
//
// A recogniser that answers "no" to everything produces no lines at all, which
// is how a session where NOTHING was recognised reads exactly like a session
// where nothing needed to be. Eight distinct sizes, once each, and only ones
// big enough to be an eye texture on any headset -- the UI and shadow maps are
// not what a reader is trying to identify.
void noteUncountedTarget(State* s, uint32_t w, uint32_t h) {
    if (w < 1024 && h < 1024) return;
    for (uint32_t i = 0; i < s->rtSeenCount; ++i) {
        if (s->rtSeenW[i] == w && s->rtSeenH[i] == h) return;
    }
    if (s->rtSeenCount >= 8) return;
    s->rtSeenW[s->rtSeenCount] = w;
    s->rtSeenH[s->rtSeenCount] = h;
    ++s->rtSeenCount;
}

// Is this render target one of the two textures sent to the headset?
//
// PREFERABLY BY THE SIZE THE HEADSET WAS ACTUALLY GIVEN. openvr_api.dll is
// handed the texture at Submit and publishes its size over the shared channel
// (frame_flag.h); this side matches against it. Nothing else in the answer is
// inferred when that value is present.
//
// The 2048x2048 threshold is the FALLBACK ONLY, for a session where no openvr
// proxy is installed to answer. Where the headset has named a size, that size
// is the whole test and the threshold is not consulted -- keeping it alongside
// was tried and measured harmful: it counted 28 atlas draws a frame on a Quest
// 3 while the real eye textures went uncounted, which is a false positive in
// the same feature the false negative was breaking.
//
// Resolved through binding_shadow, which owns the guard and the budget. A view
// that can no longer be resolved answers "no" -- see the note there about why
// callers must read a failed resolve as "do nothing" rather than as a verdict.
bool targetIsEyeSized(void* rtv) {
    State* s = g_state;
    if (!rtv) return false;

    ResourceInfo info;
    if (!bindingResolve(rtv, &info) || !info.isTexture2D) return false;

    // ORDER MATTERS, and getting it wrong is a regression rather than a miss.
    //
    // What the headset was actually handed is a FACT; everything below it is
    // a heuristic. The first version of this checked the 16:9 shape rule
    // first, and that inverts the two: a Pimax 8KX renders 3840x2160 an eye
    // and the 5K series 2560x1440, both exactly 16:9, so the shape veto threw
    // away the runtime's own answer and left those headsets with zero eye
    // draws and all four fixes dead -- worse than the guess it replaced,
    // which at least counted them for clearing 2048. The fact goes first.
    //
    // A tolerance of two pixels, not equality. The published size is a float
    // fraction of a texture width rounded to an integer, so bounds that are
    // not exactly one half (an inset, a guard band) land a pixel out and an
    // equality test would then match nothing at all -- silently, which is the
    // failure this whole change exists to end.
    const auto near2 = [](uint32_t a, uint32_t b) {
        return (a > b ? a - b : b - a) <= 2u;
    };
    if (s->eyeW && near2(info.a, s->eyeW) && near2(info.b, s->eyeH)) {
        // ...unless it is ALSO exactly the panel, which is the one genuinely
        // ambiguous case: two textures of one size cannot be told apart by
        // size. Counted rather than excluded, because excluding costs four
        // fixes at once and counting costs only the panel-distance fix
        // matching a draw into the panel. Reported once, either way.
        if (s->panelW && info.a == s->panelW && info.b == s->panelH &&
            !s->sixteenNineEyeNoted) {
            s->sixteenNineEyeNoted = true;
            Log::get().note(
                "vScreen: your eye textures and the on-foot panel are BOTH %ux%u, "
                "so nothing here can tell one from the other by size. They are "
                "being counted as eye textures, which keeps the black void, the "
                "transition flash fix and Explorer Cam fed; the panel distance fix "
                "may match a draw into the panel and place it wrongly. Set "
                "fix.vscreen_res_width/height to a size your eye textures are not "
                "if the panel sits at the wrong distance.",
                info.a, info.b);
        }
        return true;
    }

    // The panel, by SHAPE. 16:9 is what Elite's flat panel is at every size
    // it can be set to, and a per-eye target is square-ish on every headset
    // measured here (Quest 3 1456x1560, Pimax 4184x4132, Index 1440x1600,
    // Beyond 2560x2560). Reached only when the published size did not claim
    // this target, so a 16:9 headset is no longer caught by it.
    if (isPanelShaped(info.a, info.b)) {
        ++s->panelExclusions;
        noteUncountedTarget(s, info.a, info.b);
        return false;
    }

    bool out;
    if (s->eyeW) {
        // The headset named a size and this is not it. With the real answer
        // in hand the old threshold is not a second opinion worth having: it
        // counted 28 draws a frame of atlas targets on a Quest 3 while the
        // actual 1456x1560 eye textures went uncounted and the void stayed
        // grey.
        out = false;
    } else {
        // Nobody published: openvr_api.dll is not installed, or its hook has
        // not validated yet. Fall back to the old guess, which is all this
        // side can do alone -- and keep the panel-size exclusion with it,
        // because the shape rule does NOT cover a panel the player set to a
        // non-16:9 size. vscreen_res warns about those and applies them
        // anyway (see vscreen_res.cpp), so 3840x2400 is a configuration a
        // user can really be in, and without this it would be counted as an
        // eye texture on every scene draw into it.
        out = info.a >= 2048 && info.b >= 2048;
        if (out && s->panelW && info.a == s->panelW && info.b == s->panelH) {
            out = false;
            ++s->panelExclusions;
        }
    }

    if (!out) noteUncountedTarget(s, info.a, info.b);
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
    void* srv = bindingGet(BindSlot::PsSrv0);
    if (!srv) return false;

    // The answer is remembered against the generation it was computed at, so it
    // cannot outlive the binding it describes OR the frame it was computed in --
    // binding_shadow bumps that generation on both. The old tri-state was reset
    // only from the two hooks this file happens to own.
    const uint32_t gen = bindingGeneration(BindSlot::PsSrv0);
    if (s->psSrv0PanelGen == gen) return s->psSrv0Panel;

    const uint32_t w = s->panelW ? s->panelW : 1920;
    const uint32_t h = s->panelH ? s->panelH : 1080;

    ResourceInfo info;
    const bool resolved = bindingResolve(srv, &info) && info.isTexture2D;
    const uint32_t lastW = resolved ? info.a : 0;
    const uint32_t lastH = resolved ? info.b : 0;
    const bool out = resolved && lastW == w && lastH == h;
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

    s->psSrv0Panel = out;
    s->psSrv0PanelGen = gen;
    return out;
}

// Swaps in a modified copy of the panel's transform for one draw.
//
// Nothing of the game's is written to. Its own values are copied into a buffer
// of ours with one number changed, ours is used for the draw, and the original
// is put back immediately after.
// Is this call for the context we installed on? Patching vtable entries in
// place hooks the CLASS, so deferred contexts and a wrapper mod's internal
// ones reach every thunk in this file. Treating one of those as the immediate
// context would count its draws as eye draws and fire the panel override on
// somebody else's work -- silently, since it all looks like ordinary
// rendering from here. See vtable_hook.h.
inline bool foreignContext(ID3D11DeviceContext* self) {
    return self != g_state->ownerCtx;
}

bool beginPanelOverride(ID3D11DeviceContext* self) {
    State* s = g_state;
    // A draw on somebody else's context is not our panel and not an eye draw.
    // This one early return covers all four draw thunks, and it covers them
    // where the counting actually happens rather than four times over.
    if (foreignContext(self)) return false;
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
    // Three subscribers now, and this early return has learned each one late.
    //
    // It predates both the flash fix and the head-offset gate, and each time it
    // was the SAME bug: a feature whose only source of eye-draw and panel counts
    // is this function, silently starved because two unrelated settings were
    // off. The flash fix added countForFlashFix; the gate is the third, and it
    // starves in the configuration a user reaches by turning the panel distance
    // fix off and leaving the flash fix off -- the gate then sees zeros forever,
    // never arms, and nothing anywhere says why.
    //
    // The install-time gate already asked headOffsetGateWantsPanel(); this
    // per-draw one did not, so the hooks were installed and then fed nothing.
    //
    // The real fix is structural: counters this load-bearing belong in frame
    // state that features subscribe to, not inside one fix's fast path, so the
    // next feature cannot make this mistake a fourth time.
    if (!s->distanceEnabled && !s->countForFlashFix &&
        !headOffsetGateWantsPanel()) {
        return false;
    }
    const uint32_t rtvGen = bindingGeneration(BindSlot::Rtv0);
    if (s->rtv0EyeGen != rtvGen) {
        s->rtv0Eye = targetIsEyeSized(bindingGet(BindSlot::Rtv0));
        s->rtv0EyeGen = rtvGen;
    }
    if (!s->rtv0Eye) return false;
    ++s->eyeDrawsThisFrame;

    // The head-offset gate's signal, recorded BEFORE the "does anything want to
    // act" test below, and NOT conditional on the distance fix.
    //
    // This is an observation, not an intervention: it says the flat panel was
    // on screen this frame. Putting it after the early return would tie one
    // feature's inputs to another feature's setting, so turning the panel
    // distance off would silently stop the head offset ever arming -- with
    // every other part of it working and nothing saying why.
    //
    // srv0IsPanelSized memoises against the binding generation, so asking here
    // and again below is one resolve per draw, not two. It is skipped entirely
    // when the gate is off, because the answer costs a GetDesc and nothing
    // wants it.
    if (headOffsetGateWantsPanel() && srv0IsPanelSized(s)) ++s->panelCompositeDraws;

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

    void* cb = bindingGet(BindSlot::VsCb0);
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
    if (foreignContext(self)) {
        s->realClearRtv(self, rtv, c);
        return;
    }
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
    if (foreignContext(self)) {
        g_state->realOMSetRenderTargets(self, n, rtvs, dsv);
        return;
    }
    // Unconditionally, even when the pointer looks unchanged: an identical
    // address after a rebind is not evidence of an identical view. bindingSet
    // bumps the generation either way.
    bindingSet(BindSlot::Rtv0, (n && rtvs) ? rtvs[0] : nullptr);
    g_state->realOMSetRenderTargets(self, n, rtvs, dsv);
}

void STDMETHODCALLTYPE hookedOMSetRtvAndUav(ID3D11DeviceContext* self, UINT n,
                                            ID3D11RenderTargetView* const* rtvs,
                                            ID3D11DepthStencilView* dsv, UINT uavStart,
                                            UINT uavCount,
                                            ID3D11UnorderedAccessView* const* uavs,
                                            const UINT* counts) {
    if (foreignContext(self)) {
        g_state->realOMSetRtvAndUav(self, n, rtvs, dsv, uavStart, uavCount, uavs,
                                    counts);
        return;
    }
    // D3D11_KEEP_RENDER_TARGETS_UNCHANGED asks for the UAVs to be set while the
    // render targets are left alone, so it says nothing about slot 0 and must
    // not be treated as a rebind. Spelled out rather than named: the SDK header
    // this builds against does not define the constant.
    constexpr UINT kKeepRenderTargetsUnchanged = 0xFFFFFFFFu;
    if (n != kKeepRenderTargetsUnchanged) {
        bindingSet(BindSlot::Rtv0, (n && rtvs) ? rtvs[0] : nullptr);
    }
    g_state->realOMSetRtvAndUav(self, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
}

void STDMETHODCALLTYPE hookedPSSetShaderResources(ID3D11DeviceContext* self, UINT start,
                                                  UINT n,
                                                  ID3D11ShaderResourceView* const* srvs) {
    if (foreignContext(self)) {
        g_state->realPSSetShaderResources(self, start, n, srvs);
        return;
    }
    if (start == 0 && n && srvs) bindingSet(BindSlot::PsSrv0, srvs[0]);
    g_state->realPSSetShaderResources(self, start, n, srvs);
}

void STDMETHODCALLTYPE hookedVSSetConstantBuffers(ID3D11DeviceContext* self, UINT start,
                                                  UINT n, ID3D11Buffer* const* bufs) {
    if (foreignContext(self)) {
        g_state->realVSSetConstantBuffers(self, start, n, bufs);
        return;
    }
    if (start == 0 && n && bufs) bindingSet(BindSlot::VsCb0, bufs[0]);
    g_state->realVSSetConstantBuffers(self, start, n, bufs);
}

// Everything is unbound. Forget all of it -- this is the one place where
// forgetting the pointers is the truth rather than a guess.
void forgetBindings(State*) { bindingForgetAll(); }

// Both of these say so the first time they run.
//
// Their vtable slots were derived by counting declaration order, not measured,
// and a miscount would silently replace an unrelated method -- FinishCommandList
// and ClearState are neighbours in that table. One line each is what turns "the
// count looks right" into evidence, on whatever machine the log came from.
void STDMETHODCALLTYPE hookedClearState(ID3D11DeviceContext* self) {
    State* s = g_state;
    if (foreignContext(self)) {
        s->realClearState(self);
        return;
    }
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
    if (foreignContext(self)) {
        s->realExecuteCommandList(self, list, restoreContextState);
        return;
    }
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
    if (foreignContext(self)) {
        return s->realMap(self, res, sub, type, flags, mapped);
    }
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
    if (foreignContext(self)) {
        s->realUnmap(self, res, sub);
        return;
    }
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
        guardedBudget(g_cameraBudget, [&] { glitchFrameObserve(s->camData, s->camBytes, s->camResource); });
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
    // Every fix.head_offset_* key, on the reload path as well as the startup
    // one. A config reader on only one of the two is a specific repeatable bug
    // -- reload-only means the value stays its C++ initialiser for the whole
    // session -- and it cost a flight when fix.head_offset_gate did exactly
    // that.
    headOffsetGateConfigure();
    cameraViewConfigure();

    if (wasVoid != s->blackVoid || wasScale != s->distanceScale) {
        Log::get().note("vScreen config reloaded: black void %s, panel distance x%.3f "
                        "(index %u)",
                        s->blackVoid ? "on" : "off", s->distanceScale, s->distanceIndex);
    }
}

void vScreenFrameBoundary() {
    State* s = g_state;
    if (!s) return;

    // The per-frame invalidation lives in binding_shadow now, and device_hook
    // calls it once for both fixes. Doing it here as well would be harmless but
    // would put the policy back in two places, which is the thing that went
    // wrong: this file kept pointers and dropped answers while exposure_fix
    // dropped pointers, and each had its own failure mode.

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

    // What the headset is actually being given, if the other half is installed
    // and has validated. One interlocked read a frame.
    {
        uint32_t w = 0, h = 0;
        if (eyeTextureSize(&w, &h) && (w != s->eyeW || h != s->eyeH)) {
            s->eyeW = w;
            s->eyeH = h;
            if (!s->eyeSizeNoted) {
                s->eyeSizeNoted = true;
                Log::get().note(
                    "vScreen: openvr_api.dll says one eye is %ux%u, so that is now what "
                    "an eye texture IS here -- matched to within two pixels, and the "
                    "old 2048x2048 guess is no longer used at all. It stays only for "
                    "sessions where openvr_api.dll is not installed to answer.",
                    w, h);
            }
            // The collision that made four fixes inert at once, said outright
            // the moment it can be known -- it needs both sizes, and the panel
            // size is settled at install while this one arrives seconds later.
            if (!s->collisionNoted && s->panelW == w && s->panelH == h) {
                s->collisionNoted = true;
                Log::get().note(
                    "vScreen: the panel resolution you asked for (%ux%u) is EXACTLY the "
                    "size of your eye textures, so nothing here can tell one from the "
                    "other by size. The panel is no longer being excluded from the "
                    "eye-draw count, which is what keeps the black void, the transition "
                    "flash fix and Explorer Cam fed -- but the panel distance fix can now "
                    "match a draw INTO the panel and put it at the wrong distance. If the "
                    "panel sits wrong, set fix.vscreen_res_width/height to a size your "
                    "eye textures are not: 2880x1620 is a safe pick at any headset "
                    "resolution, and 1920x1080 turns the resolution fix off entirely.",
                    w, h);
            }
        }
    }

    // NOTHING has been recognised, for long enough that it is not startup.
    //
    // Every fix in this file plus two outside it read the eye-draw count, and a
    // count of zero switches all of them off together, silently -- the totals
    // line below carries the zero, but it carries it inside a paragraph that
    // says the number is not a fault indicator, which is true of every value it
    // can take except this one. A session that reported "everything except the
    // exposure fix stopped working in 0.7" was exactly this, and the log it came
    // with could not distinguish the two causes, so this line names both and
    // prints the sizes it did see.
    //
    // 1800 frames is the totals window -- twenty seconds at 90Hz. Menus and
    // loading legitimately draw nothing eye-sized, so this waits out a window
    // rather than firing at the first quiet frame.
    if (!s->starvationNoted && s->frameNo >= 1800 && s->eyeDrawsMax == 0) {
        s->starvationNoted = true;
        char sizes[192];
        sizes[0] = '\0';
        for (uint32_t i = 0; i < s->rtSeenCount; ++i) {
            char one[32];
            _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%ux%u", i ? ", " : "",
                        s->rtSeenW[i], s->rtSeenH[i]);
            strncat_s(sizes, sizeof(sizes), one, _TRUNCATE);
        }
        Log::get().note(
            "vScreen: NOT ONE eye-sized render target in %u frames. The black void, the "
            "panel distance, the transition flash fix and Explorer Cam all read that "
            "count, so all four are inert -- not broken, starved. They will say nothing "
            "further, which is why this line exists. Largest targets seen: %s. The panel "
            "is at %ux%u and its exclusion answered no %llu time(s); the headset %s. If "
            "one of those sizes is your eye texture, that is the collision -- change "
            "fix.vscreen_res_width/height (2880x1620 is safe, 1920x1080 is off). If none "
            "of them is, install openvr_api.dll as well so this side stops guessing at "
            "what your eye textures are.",
            s->frameNo, s->rtSeenCount ? sizes : "none big enough to be one",
            s->panelW, s->panelH,
            static_cast<unsigned long long>(s->panelExclusions),
            s->eyeW ? "has published its size" : "has published nothing");
    }

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
            "in THIS window treated one eye and not the other. The eye-draw count is a "
            "session peak and depends entirely on what you were doing: about 2 in HMD "
            "Cinema Mode, tens to over a thousand on foot with the helmet HUD drawn, and "
            "a few hundred in flight. It is NOT a fault indicator -- the flash detector "
            "needs it above 100 to consider a frame at all.",
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
    // The gate decides on the counts for the frame that just ended, so it is
    // told before they reset -- same rule as the flash detector above.
    headOffsetGateFrame(s->frameNo, s->panelCompositeDraws, s->eyeDrawsThisFrame);
    // The first frame the flat panel is seen is the earliest moment the game is
    // known to be loaded AND the player known to be on foot, which is what the
    // scan needs. At startup the process holds a fraction of the memory it
    // reaches in play, and a scan there finds nothing.
    // Asked every frame the player is settled on foot; the scan itself guards
    // against running twice. The old trigger was the FIRST panel sighting,
    // which on a default install is the main menu four seconds after launch --
    // 5 GB of an eventual 11 GB allocated, and the scan found nothing.
    // The scan's tick sits beside its trigger, where the frame's counters are.
    // It was in device_hook, which does not have them -- and the tick needs the
    // eye-draw count to tell "the game is being played" from "the main menu is
    // on screen", which is what stops a menu-dweller burning every attempt.
    cameraViewTick(s->eyeDrawsThisFrame);
    if (headOffsetGatePanelSettled()) cameraViewRequestScan();
    s->panelCompositeDraws = 0;
    s->eyeDrawsThisFrame = 0;
    ++s->frameNo;
}

void installVScreenFixes(ID3D11Device* device) {
    if (!device || g_state) return;

    Config& cfg = Config::get();
    headOffsetGateConfigure();
    const bool wantVoid = cfg.getBool("fix.black_void", true);
    const float scale = cfg.getFloat("fix.panel_distance", 1.0f);
    // Install the hooks whenever EITHER fix could be wanted now or later. Both
    // are documented as changeable while the game runs, and a hook that was
    // never installed cannot be switched on by editing a file -- so returning
    // here on "nothing asked for" would make the documented behaviour impossible
    // for anyone who starts with both off.
    //
    // The flash fix is part of that test, not a bystander. These context hooks
    // are its ONLY source of the camera it watches, the eye-draw count it gates
    // on, and the frame boundary that drives its cooldowns -- it installs
    // nothing itself. Consulting only the two panel fixes meant that turning
    // both off, with panel_hooks_always = 0, silently took the flash fix with
    // them: armed in the log, then nothing, and not even the give-up notice,
    // because the frame counter it waits on also lives in here.
    if (!wantVoid && scale == 1.0f && !glitchFrameNeedsEyeDraws() &&
        !headOffsetGateWantsPanel() &&
        !cfg.getBool("advanced.panel_hooks_always", true)) {
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
    s.ownerCtx = ctx;
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

bool vScreenHooksSawClearState() { return g_state && g_state->sawClearState; }
bool vScreenHooksSawExecuteCommandList() {
    return g_state && g_state->sawExecuteCommandList;
}

}  // namespace edvr

// One extra export, for the build check only.
//
// smoke.exe loads this DLL the way the game does and therefore cannot call
// anything that is not exported. It needs to ask whether the ClearState and
// ExecuteCommandList hooks actually RAN, because checking that rendering still
// works afterwards cannot catch a slot miscount -- every plausible off-by-one
// lands on a method the test never calls.
//
// Additive: the proxy still exports everything the real d3d11.dll does, plus
// this. Nothing in the game imports it.
//
// bit 0 = ClearState hook ran, bit 1 = ExecuteCommandList hook ran.
extern "C" unsigned int edvr_selftest_hooks() {
    unsigned int bits = 0;
    if (edvr::vScreenHooksSawClearState()) bits |= 1u;
    if (edvr::vScreenHooksSawExecuteCommandList()) bits |= 2u;
    return bits;
}

namespace edvr {

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
                    "stopped. Largest eye-draw count seen this session: %u -- a peak that "
                    "depends on what you were doing (about 2 in HMD Cinema Mode, tens to "
                    "over a thousand on foot, a few hundred in flight), not a fault "
                    "indicator.",
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







