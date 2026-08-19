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
#include "../common/timing.h"
#include "../common/vtable_hook.h"
#include "binding_shadow.h"
#include "device_hook.h"  // contextHookModeFor
#include "draw_census.h"
#include "glitch_frame.h"

namespace edvr {
namespace {

// How often the totals line is written, and how long the starvation notice
// waits before it will speak.
//
// STATED AS TIME so that 72Hz, 90Hz and 120Hz all behave the same. Elite in VR
// runs at whichever of those the headset is set to, and a constant in frames
// silently means a different duration at each -- the previous 1800 frames was
// 25 seconds on a 72Hz Quest, 20 on a 90Hz headset and 15 on a 120Hz Pimax,
// so the same session reported at three cadences depending on hardware.
constexpr uint64_t kTotalsWindowMs = 20000;

// A floor on frames actually drawn, which is a DIFFERENT claim from time
// having passed: a game frozen on a shader compile satisfies the clock and has
// drawn nothing, and the starvation notice would then blame the recogniser for
// a game that never rendered. 300 is reached inside the window at 72Hz with an
// order of magnitude to spare, so it constrains only the frozen case and never
// the slow-headset one.
constexpr uint32_t kMinFramesDrawn = 300;

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

// The slots the reclaim pass may vouch for, and the order their call counters
// are kept in. Vouching means: "this thunk has measurably stopped being
// called, so whatever re-pointed its slot is a bypasser, not a chainer" --
// see VTableHook::reclaim for why that distinction is the whole game.
//
// Deliberately NOT here, because a vouch is a claim that silence proves
// bypass, and for these slots it does not:
//   - ExecuteCommandList: never seen on this game. A quiet counter is its
//     normal state, and vouching for a normally-quiet slot is how a chainer
//     gets adopted during an ordinary lull.
//   - ClearState: shared with the exposure fix, which reclaim refuses on its
//     own grounds, and quiet for whole sessions besides.
//   - DrawInstanced, DrawIndexedInstanced, OMSetRtvAndUav: scene-shaped
//     calls with no in-tree proof they fire during every menu or loading
//     stretch. A menu that issues no instanced draw for three seconds is
//     ordinary, and a chainer on one of those slots during it would be
//     mistaken for a bypasser -- the loop this whole design exists to
//     refuse. They get the detection line instead of a heal; the cost is
//     that a bypasser on them stays bypassed (instanced draws go uncounted,
//     degrading the eye-draw peak under such a tool), which the log now at
//     least SAYS.
// Everything listed fires every presented frame in every mode this game has
// been observed in -- menus draw, loading screens draw, and both bind
// buffers and resources to do it -- which is what makes three silent seconds
// evidence instead of idleness.
enum ReclaimHit : uint32_t {
    kHitVsCb = 0,   // 7
    kHitPsSrv,      // 8
    kHitDrawIndexed,// 12
    kHitDraw,       // 13
    kHitMap,        // 14
    kHitUnmap,      // 15
    kHitOmSet,      // 33
    kHitClearRtv,   // 50
    kHitCount
};
constexpr size_t kReclaimableSlots[kHitCount] = {
    kSlotVSSetConstantBuffers, kSlotPSSetShaderResources, kSlotDrawIndexed,
    kSlotDraw, kSlotMap, kSlotUnmap, kSlotOMSetRenderTargets,
    kSlotClearRenderTargetView};

// Quiet passes (about a second each) before a slot's silence is vouched to
// reclaim. One pass can straddle the moment of the clobber itself; three in a
// row of a call that otherwise fires every frame, while Present keeps
// running, is a starved hook and not a quiet game. The residual -- a chainer
// installing at the start of a genuine three-second lull in one of THESE
// calls while frames still present -- has no observed instance in this game:
// menus draw, loading screens draw, and the panel modes clear.
constexpr uint8_t kQuietPassesToVouch = 3;

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
    // Largest eye-draw count seen, for the whole session and for the current
    // totals window. Both are needed and they answer different questions: the
    // session peak says whether recognition EVER worked, the window peak says
    // whether it is working NOW. Only the session one existed, which is why a
    // session that recognised eye textures and then stopped -- the eye size
    // changing under a mode switch, a wrapper reloading -- reported nothing at
    // all. A monotonic maximum cannot fall, so it cannot report a loss.
    //
    // Both are counts of draws into targets that matched the eye size the
    // headset published (6bl). Before that they counted anything at least
    // 2048 square, which is why peaks in logs older than 0.7.3 run several
    // times higher and are not comparable with these.
    uint32_t eyeDrawsMax = 0;
    uint32_t eyeDrawsWindowMax = 0;

    // The totals window is TWENTY SECONDS, measured, not 1800 frames.
    //
    // It was a frame count described in the code as "twenty seconds at 90Hz",
    // which it is on exactly one headset. A 72Hz Quest made it 25 seconds and
    // a 120Hz Pimax 15, so the same session reported at different cadences
    // depending on hardware and the windows could not be compared. Worse, the
    // rate is not the headset's: a loading screen measured at 1790fps passes
    // 1800 frames in a second, which is what let the starvation notice below
    // fire during startup.
    //
    // Asking the runtime for its nominal refresh rate would answer a
    // DIFFERENT question, and badly. Prop_DisplayFrequency_Float is an
    // IVRSystem call, and IVRSystem is the one interface this project refuses
    // to touch -- calling into IVRSystem_012 by vtable index crashed the game
    // with a stack cookie failure, documented at the top of compositor_hook.
    // The nominal rate would also not have caught the 1790fps loading screen,
    // because the game was running FASTER than the display, nor a session
    // running at half rate under reprojection. Frames counted against a clock
    // measure what actually happened; the display's rating does not.
    uint64_t windowStartMs = 0;
    uint32_t windowStartFrame = 0;
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
    // How many times targetIsEyeSized was ASKED, regardless of its answer.
    // Zero with the per-draw askers enabled means the draw hooks themselves
    // never ran -- the bypass signature -- and zero with them disabled means
    // the settings, not a fault. The starvation notice needs the difference:
    // it accused a healthy exposure-only configuration of being hooked over,
    // and solicited a bug report for it, before this existed.
    uint32_t recogniserAsks = 0;
    // Per-slot proof the hooked thunks are being CALLED, for the reclaim
    // pass. Incremented at the top of each thunk, before the foreign-context
    // test, because raw invocation is the evidence -- a chainer forwarding
    // the game's calls keeps these climbing, and that is exactly what makes
    // its slot unsafe to take back. Plain uint32 increments on hot paths;
    // a lost increment under a race reads as slightly quieter, and three
    // full seconds of losses on a per-frame call is not a real interleaving.
    uint32_t thunkHits[kHitCount] = {};
    uint8_t  quietPasses[kHitCount] = {};
    // Eye draws accumulated since the last reclaim pass -- the SCENE evidence
    // the exposure fix borrows for its own vouches. Its compute slots go
    // legitimately silent through loading screens (measured at 1790fps with
    // no compute at all), so silence-while-presenting proves nothing there;
    // silence while EYES ARE BEING DRAWN does, because the exposure pass is
    // how those eyes get tonemapped. Accumulated at the frame boundary,
    // consumed and zeroed by vScreenReclaimHooks.
    uint32_t eyeDrawsSinceReclaim = 0;
    bool     starvationNoted = false;
    bool     recognitionLostNoted = false;
    // When the fixes were installed, for the starvation check. Frames are not
    // a clock: a loading screen measured at 1790fps (see glitch_frame's
    // validation note) passes 1800 frames in one second, so a frame count
    // alone cannot say "long enough that this is not startup".
    uint64_t installMs = 0;
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
    // Counted before any early return: "was the question asked" is a
    // different fact from "what was the answer", and the starvation notice
    // needs the first one. A null rtv still counts -- the draw path asked.
    ++s->recogniserAsks;
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

// kind, count and instances describe the draw for the census and nothing
// else; every other consumer of this function is indifferent to them.
bool beginPanelOverride(ID3D11DeviceContext* self, char kind, UINT count,
                        UINT instances) {
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
    // The census is subscriber number four, added the way the paragraph above
    // says the next one should not be. The structural fix -- counters in frame
    // state that features subscribe to -- is still owed; until it lands, the
    // census at least fails towards silence: unarmed (the permanent state) it
    // adds nothing to this condition's answer, and the short-circuit means the
    // call is not even made while any ordinary subscriber is on.
    if (!s->distanceEnabled && !s->countForFlashFix &&
        !headOffsetGateWantsPanel() && !drawCensusArmed()) {
        return false;
    }
    const uint32_t rtvGen = bindingGeneration(BindSlot::Rtv0);
    if (s->rtv0EyeGen != rtvGen) {
        s->rtv0Eye = targetIsEyeSized(bindingGet(BindSlot::Rtv0));
        s->rtv0EyeGen = rtvGen;
    }
    if (!s->rtv0Eye) return false;
    ++s->eyeDrawsThisFrame;

    // The census line for this draw, recorded while its bindings are certainly
    // the ones it will run with. Armed is rare and brief; the cost of asking is
    // one call and one bool.
    if (drawCensusArmed()) {
        drawCensusEyeDraw(kind, count, instances, s->eyeDrawsThisFrame);
    }

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
    ++s->thunkHits[kHitClearRtv];
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
    ++g_state->thunkHits[kHitOmSet];
    if (foreignContext(self)) {
        g_state->realOMSetRenderTargets(self, n, rtvs, dsv);
        return;
    }
    // Unconditionally, even when the pointer looks unchanged: an identical
    // address after a rebind is not evidence of an identical view. bindingSet
    // bumps the generation either way.
    bindingSet(BindSlot::Rtv0, (n && rtvs) ? rtvs[0] : nullptr);
    bindingSet(BindSlot::Dsv0, dsv);
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
        bindingSet(BindSlot::Dsv0, dsv);
    }
    g_state->realOMSetRtvAndUav(self, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
}

void STDMETHODCALLTYPE hookedPSSetShaderResources(ID3D11DeviceContext* self, UINT start,
                                                  UINT n,
                                                  ID3D11ShaderResourceView* const* srvs) {
    ++g_state->thunkHits[kHitPsSrv];
    if (foreignContext(self)) {
        g_state->realPSSetShaderResources(self, start, n, srvs);
        return;
    }
    // Slots 0..3, not just 0: the census fingerprints a draw by everything it
    // samples, and a mask or gradient in a later slot is often what tells one
    // overlay from another. Recording a slot the call did not cover would
    // invent an unbind, so only [start, start+n) is touched. PsSrv1..3 are
    // contiguous after PsSrv0 by binding_shadow.h's contract.
    if (srvs) {
        for (UINT i = 0; i < n; ++i) {
            const UINT slot = start + i;
            if (slot >= 4) break;
            bindingSet(static_cast<BindSlot>(
                           static_cast<uint32_t>(BindSlot::PsSrv0) + slot),
                       srvs[i]);
        }
    }
    g_state->realPSSetShaderResources(self, start, n, srvs);
}

void STDMETHODCALLTYPE hookedVSSetConstantBuffers(ID3D11DeviceContext* self, UINT start,
                                                  UINT n, ID3D11Buffer* const* bufs) {
    ++g_state->thunkHits[kHitVsCb];
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
    ++s->thunkHits[kHitMap];
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
    ++s->thunkHits[kHitUnmap];
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
    ++g_state->thunkHits[kHitDraw];
    const bool on = beginPanelOverride(self, 'D', count, 1);
    g_state->realDraw(self, count, start);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawIndexed(ID3D11DeviceContext* self, UINT count,
                                         UINT startIndex, INT baseVertex) {
    ++g_state->thunkHits[kHitDrawIndexed];
    const bool on = beginPanelOverride(self, 'I', count, 1);
    g_state->realDrawIndexed(self, count, startIndex, baseVertex);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawInstanced(ID3D11DeviceContext* self, UINT perInstance,
                                           UINT instances, UINT startVertex,
                                           UINT startInstance) {
    const bool on = beginPanelOverride(self, 'N', perInstance, instances);
    g_state->realDrawInstanced(self, perInstance, instances, startVertex, startInstance);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawIndexedInstanced(ID3D11DeviceContext* self,
                                                  UINT perInstance, UINT instances,
                                                  UINT startIndex, INT baseVertex,
                                                  UINT startInstance) {
    const bool on = beginPanelOverride(self, 'X', perInstance, instances);
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

bool vScreenReclaimHooks() {
    State* s = g_state;
    if (!s) return false;
    // Turn the per-thunk call counters into the vouch list. A slot is vouched
    // when it has been silent for kQuietPassesToVouch consecutive passes WHILE
    // THE SAME CONTEXT'S OTHER THUNKS WERE FIRING -- which is the one
    // combination a chainer cannot produce, since a chainer forwards the
    // game's calls into our thunks and keeps its own slot's counter climbing.
    //
    // The gate used to be everHit -- "this slot fired at least once before" --
    // and the field refuted it within a day: OpenXR Toolkit's layer loads
    // with the process and re-points the draw slots BEFORE the game's first
    // draw call, so the draw counters never fired, the vouch was structurally
    // unearnable, and five slots stayed bypassed for the session with the
    // detection lines dutifully pointing at them (measured 2026-08-18, both
    // on the reporting user's rig and reproduced locally). Cross-slot
    // liveness has no such birth window: Map, Unmap and the bind calls fire
    // from the first frame of anything -- menus, loading screens, play --
    // so "this context is dispatching through EDVR somewhere, and THIS slot
    // alone is silent" is available from the very first pass, and it is
    // still evidence a chainer cannot fake, because a chainer's forwarding
    // IS traffic.
    bool ctxAlive = false;
    for (uint32_t i = 0; i < kHitCount; ++i) {
        if (s->thunkHits[i] != 0) { ctxAlive = true; break; }
    }
    size_t quiet[kHitCount];
    size_t n = 0;
    for (uint32_t i = 0; i < kHitCount; ++i) {
        if (s->thunkHits[i] != 0) {
            s->quietPasses[i] = 0;
            s->thunkHits[i] = 0;
        } else if (ctxAlive && s->quietPasses[i] < 255) {
            // Silence only counts against a demonstrably-dispatching context.
            // A minimized or frozen game freezes every streak with it.
            ++s->quietPasses[i];
        }
        if (s->quietPasses[i] >= kQuietPassesToVouch) {
            quiet[n++] = kReclaimableSlots[i];
        }
    }
    s->hook.reclaim("vScreen context", quiet, n);

    // The scene evidence the exposure fix's vouches ride on, returned to the
    // caller that runs both passes. Consumed here so one pass window means the
    // same thing to both readers.
    const bool sceneRendered = s->eyeDrawsSinceReclaim > 0;
    s->eyeDrawsSinceReclaim = 0;
    return sceneRendered;
}

void vScreenFrameBoundary() {
    State* s = g_state;
    if (!s) return;

    // Before this frame's counters are read or reset: a pending census starts
    // here, a running one advances, a spent one writes its tables.
    drawCensusFrameBoundary(s->frameNo);

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
    if (s->eyeDrawsThisFrame > s->eyeDrawsWindowMax) {
        s->eyeDrawsWindowMax = s->eyeDrawsThisFrame;
    }
    // For the reclaim pass's scene evidence; see the field's comment. Saturate
    // rather than wrap -- the consumer only asks "any at all".
    if (s->eyeDrawsSinceReclaim < 0xFFFFFFFFu - s->eyeDrawsThisFrame) {
        s->eyeDrawsSinceReclaim += s->eyeDrawsThisFrame;
    }

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
    // WAITING OUT A WINDOW MEANS TIME, NOT FRAMES. This read `frameNo >= 1800`
    // on the stated premise that 1800 frames is "twenty seconds at 90Hz". At
    // the three rates this mod has to work at that premise is wrong twice: 25
    // seconds at 72Hz, 15 at 120. During a loading screen it is not even close
    // -- the flash detector's validation note records one measured at 1790fps,
    // which reaches 1800 frames in a second and legitimately draws nothing
    // eye-sized while it does, which is how this notice fired during startup.
    //
    // So the duration is a duration. The frame floor stays, because "twenty
    // seconds have passed" is not the same claim as "the game has been
    // drawing" -- a frozen game satisfies the first and not the second -- but
    // it is set low enough to be reached at 72Hz and every rate above it,
    // rather than being a disguised second copy of the timeout.
    const uint64_t now = nowMs();
    const uint64_t windowMs = now - s->windowStartMs;
    const uint32_t windowFrames = s->frameNo - s->windowStartFrame;
    const uint32_t windowFps =
        windowMs ? static_cast<uint32_t>((windowFrames * 1000ull + windowMs / 2) / windowMs)
                 : 0u;

    const bool pastStartup =
        (now - s->installMs) >= kTotalsWindowMs && s->frameNo >= kMinFramesDrawn;

    if (!s->starvationNoted && pastStartup && s->eyeDrawsMax == 0) {
        s->starvationNoted = true;
        char sizes[192];
        sizes[0] = '\0';
        for (uint32_t i = 0; i < s->rtSeenCount; ++i) {
            char one[32];
            _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%ux%u", i ? ", " : "",
                        s->rtSeenW[i], s->rtSeenH[i]);
            strncat_s(sizes, sizeof(sizes), one, _TRUNCATE);
        }
        // Four starvations, four different next moves -- and the advice used
        // to be one string that fit only one of them. A user with the openvr
        // half installed and NOTHING seen was told to install what they had
        // (measured 2026-08-18, and the real cause was another tool
        // re-pointing the hooks themselves; the reclaim pass now exists for
        // exactly that, so point the reader at its lines). The bypass verdict
        // needs recogniserAsks, not the seen-counters: zero asks with the
        // per-draw askers ON means the draw hooks never ran, while zero asks
        // with them OFF is the settings -- and accusing a healthy
        // exposure-only install of being hooked over, with a solicited bug
        // report, is exactly the kind of lie this notice exists to end.
        const bool perDrawAskers = s->distanceEnabled || s->countForFlashFix ||
                                   headOffsetGateWantsPanel();
        const char* advice;
        if (!perDrawAskers) {
            // Settled BEFORE the ask count is consulted: with every per-draw
            // consumer off, a zero eye-draw peak is structural whatever the
            // clear path asked -- a black-void-only session where voids WERE
            // cleared still lands here, and its own totals line already says
            // whether the clearing worked.
            advice =
                "Eye draws are only counted when a fix that needs them per "
                "draw is on, and none is: the panel distance is at 1.0, the "
                "flash detector is not counting, and the head-offset gate is "
                "idle. The black void fix does not count draws -- the totals "
                "lines say whether it is clearing. This zero is those "
                "settings, not a fault.";
        } else if (s->recogniserAsks == 0) {
            advice =
                "The recogniser was never even ASKED -- with fixes enabled "
                "that ask on every draw, that means the draw and bind hooks "
                "themselves are not running: another tool has re-pointed the "
                "vtable entries EDVR patched (OpenXR Toolkit under "
                "OpenComposite is a known one). EDVR checks once a second and "
                "re-patches -- look for VTableHook lines near this one saying "
                "so. If there are none, report this log.";
        } else if (!s->eyeW) {
            advice =
                "If one of those sizes is your eye texture, that is the "
                "collision -- change fix.vscreen_res_width/height (2880x1620 "
                "is safe, 1920x1080 is off). If none of them is, install "
                "openvr_api.dll as well so this side stops guessing at what "
                "your eye textures are.";
        } else {
            advice =
                "If one of those sizes is your eye texture, that is the "
                "collision -- change fix.vscreen_res_width/height (2880x1620 "
                "is safe, 1920x1080 is off). If none of them matches the "
                "published size either, something between the game and the "
                "headset is resizing the image -- an upscaler's input "
                "resolution, or supersampling that moved mid-session. Report "
                "this log so the recogniser can learn that layout.";
        }
        Log::get().note(
            "vScreen: NOT ONE eye-sized render target in %u frames over %u seconds. The black void, the "
            "panel distance, the transition flash fix and Explorer Cam all read that "
            "count, so all four are inert -- not broken, starved. They will say nothing "
            "further, which is why this line exists. Largest targets seen: %s. The panel "
            "is at %ux%u and its exclusion answered no %llu time(s); the headset %s. %s",
            s->frameNo, static_cast<uint32_t>((now - s->installMs) / 1000u),
            s->rtSeenCount ? sizes : "none big enough to be one",
            s->panelW, s->panelH,
            static_cast<unsigned long long>(s->panelExclusions),
            s->eyeW ? "has published its size" : "has published nothing",
            advice);
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
    if (windowMs >= kTotalsWindowMs) {
        Log::get().note(
            "vScreen totals: panel distance applied %llu time(s), void cleared to black "
            "%llu time(s) (%u-%u per frame over the last %u frames), largest eye-draw "
            "count %u this window and %u this session. %u frames in %u ms is %u fps. "
            "Two eyes a frame, so these should climb steadily; if they stop, the fix "
            "engaged once and then stopped matching. The per-frame void range should be "
            "a single number repeated -- a low end below the high end means some frames "
            "in THIS window treated one eye and not the other. The eye-draw counts "
            "depend entirely on what you were doing: about 2 in HMD Cinema Mode, tens to "
            "hundreds on foot with the helmet HUD drawn, and a few hundred in flight. "
            "They are NOT a fault indicator -- the flash detector needs the count above "
            "100 to consider a frame at all. The fps is what the GAME produced, which is "
            "not the headset's refresh rate: far above it on a loading screen, half of "
            "it when the runtime is reprojecting.",
            static_cast<unsigned long long>(s->panelOverrides),
            static_cast<unsigned long long>(s->voidClears),
            s->voidFrameMin == 0xFFFFFFFFu ? 0u : s->voidFrameMin, s->voidFrameMax,
            windowFrames, s->eyeDrawsWindowMax, s->eyeDrawsMax,
            windowFrames, static_cast<uint32_t>(windowMs), windowFps);

        // A window that recognised NOTHING, after one that did.
        //
        // The session peak above cannot report this: a maximum never falls, so
        // once anything has been recognised the number stays reassuring for the
        // rest of the session even if recognition has since stopped dead. That
        // is not hypothetical -- the eye textures are recreated across an
        // external-camera or on-foot switch, and a size change there would
        // starve every fix in this file while the peak kept reading fine.
        if (!s->recognitionLostNoted && s->eyeDrawsMax > 0 &&
            s->eyeDrawsWindowMax == 0) {
            s->recognitionLostNoted = true;
            Log::get().note(
                "vScreen: eye textures were being recognised and now are not -- %u "
                "frames in this window and not one eye-sized draw, against a session "
                "peak of %u. The black void, the panel distance, the transition flash "
                "fix and Explorer Cam all read that count, so all four have gone inert "
                "as of this window. If you changed headset mode, resolution or "
                "supersampling mid-session, the eye size changed with it. Said once.",
                windowFrames, s->eyeDrawsMax);
        }

        // Reset for the next window. A session-wide extreme never recovers: one
        // odd frame during a mode change pins the low end at 1 and every later
        // report then accuses the fix of a fault that stopped happening long
        // ago. The reader needs to know what is true now.
        s->voidFrameMin = 0xFFFFFFFFu;
        s->voidFrameMax = 0;
        s->eyeDrawsWindowMax = 0;
        s->windowStartMs = now;
        s->windowStartFrame = s->frameNo;
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

void installVScreenFixes(ID3D11Device* device, HookMode mode) {
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
    g_state->installMs = stampMs();
    g_state->windowStartMs = g_state->installMs;
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

    // The mechanism, decided once per device by the caller and shared with the
    // exposure hooks so the two agree about this one object. Between attach
    // and the first replace, the only window setMode allows.
    s.hook.setMode(mode);

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
                    "counting %s, hooking %s",
                    s.blackVoid ? "on" : "off",
                    s.distanceEnabled ? "on" : "off (1.0)",
                    (s.distanceEnabled || s.countForFlashFix)
                        ? "on"
                        : "OFF -- the transition flash fix cannot act without it",
                    s.hook.mode() == HookMode::CopyVptr
                        ? "by private vtable copy (the context is the runtime's own, "
                          "which re-points its shared table between modes -- a copy is "
                          "immune)"
                        : "in place (the context is a wrapper's, e.g. ReShade; reclaim "
                          "watches for another tool re-pointing our slots)");
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







