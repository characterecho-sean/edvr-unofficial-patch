#include "device_hook.h"

#include "shader_sig.h"

#include <windows.h>

#include <d3d11_4.h>   // ID3D11Multithread, for the protection probe
#include <dxgi1_2.h>

#include "../common/config.h"
#include "../common/eye_sync.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/hotkey.h"
#include "head_offset_gate.h"
#include "camera_view.h"
#include "fss_res.h"
#include "journal_watch.h"
#include "xinput_watch.h"
#include "elite_binds.h"
#include "../common/log.h"
#include "../common/proxy.h"
#include "../common/timing.h"
#include "../common/vtable_hook.h"
#include "binding_shadow.h"
#include "draw_census.h"
#include "quad_probe.h"
#include "exposure_fix.h"
#include "menu.h"
#include "perf_monitor.h"
#include "vscreen.h"
#include "glitch_frame.h"
#include "vscreen_res.h"

namespace edvr {
namespace {

// Frozen COM ABI. IUnknown occupies 0-2; the interface methods follow in
// declaration order. Each index is still range-checked before use.
// CreateTexture2D verified against the SDK's ID3D11DeviceVtbl on 2026-08-25,
// the same check the context slots got: a miscount here would silently hook
// CreateTexture1D or CreateTexture3D.
constexpr size_t kDevCreateTexture2D     = 5;
constexpr size_t kDevCreateVertexShader  = 12;
constexpr size_t kDevCreatePixelShader   = 15;
constexpr size_t kDevCreateComputeShader = 18;
// CreateSamplerState, counted against the SDK's ID3D11DeviceVtbl the same
// way: CreateBlendState 20, CreateDepthStencilState 21, CreateRasterizerState
// 22, CreateSamplerState 23.
constexpr size_t kDevCreateSamplerState  = 23;

// THE CREATES THAT CAN FAIL, hooked to say so and for no other reason.
//
// Elite treats a failing HRESULT from its DX11 backend as fatal: its own
// F3D_VERIFY excuses 0x887A000A -- DXGI_ERROR_WAS_STILL_DRAWING, and only
// that one -- and calls abort() for everything else, DEVICE_REMOVED very
// much included. Abort lands on a five-byte die-stub that every other fatal
// error in the game shares, so the process ends at an address that names
// nothing, with none of our code on the stack, and the refusal that actually
// caused it is never written down anywhere (issue #20). These hooks change
// nothing at all. They exist so that the line before the crash says which
// call D3D11 refused and with what.
//
// Counted against the SDK's ID3D11DeviceVtbl the same way the four above
// were: CreateBuffer 3, CreateTexture1D 4, CreateTexture2D 5,
// CreateTexture3D 6, CreateShaderResourceView 7, CreateUnorderedAccessView
// 8, CreateRenderTargetView 9, CreateDepthStencilView 10.
constexpr size_t kDevCreateBuffer        = 3;
constexpr size_t kDevCreateTexture1D     = 4;
constexpr size_t kDevCreateTexture3D     = 6;
constexpr size_t kDevCreateSrv           = 7;
constexpr size_t kDevCreateUav           = 8;
constexpr size_t kDevCreateRtv           = 9;
constexpr size_t kDevCreateDsv           = 10;
// One past the highest of them: the saved-original table is indexed by slot.
constexpr size_t kDevCreateSlots         = 11;

constexpr size_t kSwapPresent            = 8;
constexpr size_t kFactoryCreateSwapChain = 10;
constexpr size_t kFactory2CreateSwapChainForHwnd = 15;

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateShader)(ID3D11Device*, const void*, SIZE_T,
                                                     ID3D11ClassLinkage*, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
    ID3D11Texture2D**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSamplerState)(
    ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
// All seven of the creates above share one register shape: this, a pointer,
// a pointer, an out pointer. For a resource create the first is the desc and
// the second the initial data; for a view create the first is the resource
// and the second the view desc. One typedef covers both, which is what lets
// one template body stand behind all seven slots.
typedef HRESULT(STDMETHODCALLTYPE* PFN_DevCreate)(ID3D11Device*, const void*,
                                                  const void*, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*,
                                                        DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

struct State {
    VTableHook deviceHook;
    VTableHook swapChainHook;
    VTableHook factoryHook;
    // The swap-only and live-only probes' hook on the immediate context: a
    // private table with nothing patched in it -- frozen for one, live stubs
    // for the other -- standing in for BOTH context installers. See
    // advanced.context_hook_probe.
    VTableHook bareContextHook;

    ID3D11Device*   device = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    // The factory we hooked, as an identity token only: compared, never
    // dereferenced, and no reference is held (the factory is released right
    // after hooking, as it always was). Patching entries in place hooks every
    // factory sharing the table, so the swapchain hooks above need to know
    // which one is the game's.
    void*           factory = nullptr;

    PFN_CreateShader realCreateCS = nullptr;
    PFN_CreateShader realCreateVS = nullptr;
    PFN_CreateShader realCreatePS = nullptr;
    PFN_CreateTexture2D realCreateTexture2D = nullptr;
    PFN_CreateSamplerState realCreateSamplerState = nullptr;
    // Indexed by vtable slot, so the template hook can find its own original
    // from its own slot number. Slots we do not hook stay null and are never
    // reached, because an unpatched entry never routes here.
    PFN_DevCreate realDevCreate[kDevCreateSlots] = {};
    uint32_t      createFailNotes = 0;
    // The texture-filtering census. A repeating pattern on a distant
    // surface -- a station's ribbed panels, a hull's hatching -- shimmers
    // when the sampler picks a mip sharper than the pixel's footprint,
    // and no pass at the submit door can put back detail the game never
    // sampled. What the game ASKS for is the first thing to know, and
    // nothing printed it. Shapes are (filter, anisotropy, bias); the
    // table is small because a renderer reuses a handful.
    struct SamplerShape {
        uint32_t filter = 0;
        uint32_t aniso = 0;
        float    bias = 0.0f;
        uint32_t count = 0;
    };
    SamplerShape samplerShapes[12];
    uint32_t     samplerShapeCount = 0;
    uint32_t     samplerCreates = 0;
    uint32_t     samplerOther = 0;      // shapes past the table's end
    uint64_t     samplerFirstMs = 0;
    uint32_t     samplerCensusPrints = 0;
    uint32_t     samplerPrintedShapes = 0;
    // The overrides, both default-off. anisotropy promotes a plainly
    // linear or already-anisotropic sampler; bias shifts its mip choice,
    // positive for blurrier and quieter. Neither touches a comparison,
    // minimum or maximum filter (shadows and depth reductions), nor a
    // point sampler, whose look is deliberate.
    int          samplerAniso = 0;
    float        samplerBias = 0.0f;
    char         samplerBiasWhy[220] = {};   // where the bias came from, for the log
    bool         samplerBiasAuto = false;    // derived from Elite's own multiplier
    float        samplerBiasMult = 0.0f;     // ...and the multiplier it was derived from
    bool         samplerForceNoted = false;
    // The shader-swap arc's dump mode: while armed, every vertex and pixel
    // shader blob the game creates is written to <logdir>\shaders by hash,
    // and the glare draw logs which two hashes it binds -- the pair to
    // disassemble. Diagnostic; costs file writes on the streaming threads.
    bool         shaderDump = false;
    std::wstring shaderDumpDir;
    bool         shaderDumpDirMade = false;
    PFN_Present      realPresent = nullptr;
    PFN_CreateSwapChain        realCreateSwapChain = nullptr;
    PFN_CreateSwapChainForHwnd realCreateSwapChainForHwnd = nullptr;

    Hotkey toggleKey;
    Hotkey dumpKey;
    // The draw census key (issue 69074 instrumentation). Unbound by default;
    // the census costs nothing until this is both bound and pressed.
    Hotkey censusKey;
    // The external camera key, and only that one.
    //
    // A keypress is not a heuristic, and it is the whole reason this feature can
    // tell entering the external camera from boarding a ship: render state alone
    // cannot. Unbound by default, and the gate does nothing at all without it.
    //
    // There is no next-camera-view key here. It existed as a fallback for
    // counting the camera preset by hand, and EDVR reads the preset from the
    // game instead -- so it was a second binding to explain, to get wrong, and
    // to keep in step, in exchange for nothing a working install uses.
    Hotkey externalCamKey;
    Hotkey extCamNextKey;
    // And the other way round the ring. Elite binds the two directions
    // separately, so watching only one counts only half of what the player
    // does with the cycle.
    Hotkey extCamPrevKey;
    // The player's own FSS enter/quit keys, adopted from their Elite
    // bindings like the camera keys above; they give the theater's mode
    // latch its frame-exact edges.
    Hotkey fssEnterKey;
    Hotkey fssQuitKey;
    Hotkey fssZoomStepKey;
    Hotkey fssZoomKey;
    // The camera cycle on a gamepad. Elite's own defaults put the view
    // cycle on the D-pad and the camera toggle on the SAME D-pad direction
    // with a modifier, so these carry a veto as well as a button.
    XinputBinding extCamNextPad;
    XinputBinding extCamPrevPad;
    XinputBinding fssEnterPad;
    XinputBinding fssQuitPad;
    XinputBinding fssZoomStepPad;
    XinputBinding fssZoomPad;
    bool fssZoomPressPending = false;
    bool     fssTheaterWanted = false;
    bool     fssModeLatch = false;
    bool     fssLatchByKey = false;
    uint64_t fssLatchMs = 0;
    uint64_t fssQuitMs = 0;
    uint32_t fssLatchNotes = 0;
    // Both camera hotkeys come from the GAME's bindings files and follow
    // them live (0.7.1 removed the ini overrides outright) -- Elite
    // rewrites Options\Bindings the moment a rebind or preset switch is
    // applied, and a slow stat (below) notices within seconds.
    uint64_t bindsFingerprint = 0;
    uint64_t bindsPending = 0;       // a change waiting to hold for one beat
    // When the bindings directory was last stat'd. Was a 450-frame countdown
    // duplicating kBindsCheckFrames as a literal, so converting the constant
    // alone would have left the FIRST interval on the old value.
    uint64_t bindsCheckMs = 0;
    uint32_t lastJournalDisembarks = 0;
    uint32_t lastJournalEmbarks = 0;
    uint32_t lastCameraEnters = 0;
    uint64_t frameCounter = 0;
    uint64_t configPollMs = 0;
    // For the crash sentinel's confirm window: the previous Present, and the
    // frame time credited so far. See kSentinelConfirmMs.
    uint64_t lastPresentMs = 0;
    uint64_t presentingMs = 0;

    // Dump the camera history on every external-camera keypress. Diagnostic,
    // off by default: one line per frame of the ring, every press.
    bool     dumpOnExternalCam = false;
    // When the delayed dump is due. 0 means none is armed.
    uint64_t dumpDueMs = 0;
    uint32_t missedDumpNotes = 0;
    uint32_t missedCensusNotes = 0;
    bool     threadNoted = false;
    // Frames to hold across an external-camera transition. 0 = off, and it stays
    // there: see the note beside the setting in edvr.ini.
    uint32_t holdFramesOnExternalCam = 0;

    // Crash sentinel for the d3d11 half.
    //
    // The openvr half has had one since it was written, and this half -- which
    // hooks the device, the context, the swapchain and the factory, and is much
    // the larger of the two -- had none. So a configuration that crashed during
    // install or in the first frames crashed on EVERY launch, and the only cure
    // was finding the documentation and deleting the file. That is the state a
    // user cannot get themselves out of.
    //
    // WHAT IT DOES AND DOES NOT COVER, because the difference matters. It arms
    // before the first vtable write and confirms once the hooks have survived a
    // few seconds of presenting, so it catches the class that makes an install
    // unusable: a crash at install or shortly after, which repeats every launch.
    // It does NOT catch a crash half an hour in -- that session confirmed long
    // before, and disabling the hooks at the next launch would not obviously
    // help anyway, since such a crash is not reproducible from startup.
    Sentinel* sentinel = nullptr;
    uint32_t  framesSeen = 0;
    bool      sentinelConfirmed = false;
    // The advanced.d3d11_fixes = 0 paragraph, said once. hookDevice runs per
    // created device and the game creates more than one.
    bool      fixesOffNoted = false;

    // MULTITHREAD PROTECTION, watched but never touched.
    //
    // Issue #21's rig has Windows' own d3d11.dll rewriting the immediate
    // context's whole dispatch table every frame. Reading the binary
    // (10.0.26100.9278) says what that code IS: two complete sets of method
    // implementations, 97 entries each with no function in common, and a
    // three-instruction test picking between them --
    // `cmp byte ptr [rcx+0xE46], 0` in front of each writer block. A boolean in
    // the context object selects which whole table the object gets.
    //
    // D3D11 has exactly one documented boolean of that shape: multithread
    // protection. Turning it on makes every context method take the runtime's
    // critical section, and the natural way to implement that is a second
    // complete set of implementations rather than a branch in every method --
    // which is what the binary shows.
    //
    // That is an INFERENCE, and this exists to end it rather than repeat it.
    // Two facts are wanted and they are different questions: what the flag IS,
    // and whether it CHANGES. The table being rewritten every frame does not
    // prove the flag moves -- the writer routine could be reached for some
    // other reason and simply re-lay whichever table the flag currently
    // selects. If the value never changes while the rewriting continues, the
    // flag is a red herring and the hunt moves to what keeps calling that
    // routine, which is worth learning from one line of log rather than from
    // building the wrong thing.
    //
    // Read-only, always. Setting it would be a real change to the game's
    // threading contract -- on costs a lock in every D3D11 call, off is a data
    // race if something in the process needs it -- and neither belongs in a
    // measurement.
    ID3D11Multithread* multithread = nullptr;
    int       mtProtected = -1;      // -1 until asked; 0 or 1 after
    uint32_t  mtChanges = 0;         // how many times it has flipped
    uint32_t  mtReports = 0;         // change lines printed
    uint64_t  mtFrames = 0;          // frames it has been sampled over
    bool      mtSettledNoted = false;  // the standing answer, said once
};

// How long the hooks must survive before install is treated as having worked.
//
// Six seconds of play, which is past the loading screen and into a drawn scene.
// Long enough that the risky part -- the first frames through four patched
// vtables -- is behind us, short enough that a player who quits normally has
// confirmed long before, because a false trip costs them every fix for a
// session and that is the cost this must not impose casually.
//
// THIS ONE WAS BROKEN TWICE OVER as 600 presented frames. It was 8.3 seconds at
// 72Hz and 5 at 120, and worse, presented frames are not paced by the display
// during a loading screen -- the menu and loading screen present at about
// 1800fps, so 600 of them went by in a third of a second and the sentinel
// confirmed survival before the game had drawn anything at all. The window
// that was supposed to cover the risky period closed before it started.
//
// AND THEN IT WAS BROKEN A THIRD TIME, as six seconds of WALL CLOCK measured
// from the first Present -- which one stalled frame can spend on its own.
// Issue #20, 2026-09-05: the temporal pass compiles its HLSL synchronously in
// the first frame's vScreenFrameBoundary, and on the reporter's rig fxc took
// 6.26 seconds over it ("internal warning: optimization did not converge").
// Present #2 therefore arrived at +6.3 s, the window was satisfied by a single
// frame nobody had rendered, the .armed file was deleted, and the crash 2.2
// seconds later left no trip behind. Measured across twenty crashes in one
// breadcrumb file: fifteen at 8.6-9.6 s with the pass on and never a stand-down
// after them, four at 2.4 s with it off and a stand-down after every one. The
// protection was absent in exactly the configuration that needed it, and the
// user's report was "temporal_aa crashes the game" when what it did was hide
// the sentinel.
//
// So the window is now six seconds of PRESENTING, accumulated from the gaps
// between Presents, and a gap longer than kSentinelMaxFrameMs contributes
// nothing at all. A stall is not evidence the hooks survived anything; it is
// evidence that nothing happened. Time is only credited for frames that look
// like frames.
constexpr uint64_t kSentinelConfirmMs = 6000;

// The longest gap between two Presents that still counts as the game
// presenting.
//
// Four frames a second. Elite's menu and loading screens present at about
// 1800fps and the headset rates are 72 to 120, so no real frame is remotely
// near this; the gaps it throws away are one-shot startup work of ours (a
// shader compile, a first-frame allocation) and hitches severe enough that
// crediting them would be a lie either way. Deliberately far above any frame
// and far below the 6.26 s that produced issue #20: the value only has to
// separate those two populations, and they are four orders of magnitude apart.
constexpr uint64_t kSentinelMaxFrameMs = 250;

// Frames to wait after an external-camera keypress before dumping the history.
//
// Two seconds, which is comfortably past the mode change: the panel-to-scene
// delay alone has been measured at 2 to 86 frames, and the flash being chased
// is on the transition itself. The ring holds at least ten seconds at every
// supported rate, so this still leaves eight seconds of ordinary flight in
// front of the event to compare against.
//
// The openvr half's kPoseDumpDelayMs is held EQUAL to this, and the equality
// still matters for the same reason it always did: the two logs are read side
// by side. What changes is that they are now equal in a unit that means the
// same thing on both -- as frame counts they were already equal, and already
// meant 2.5 seconds on one headset and 1.5 on another while both logs said
// "about two seconds".
constexpr uint64_t kDumpDelayMs = 2000;

// How often the Elite bindings directory is stat'd for changes: one listing
// every five seconds. Two consecutive stable sightings commit a change, so a
// rebind lands in ten seconds at the outside, and a directory Elite is
// mid-writing is never parsed. Both of those figures are seconds, which is why
// this is no longer 450 frames -- as frames the "ten seconds at the outside"
// was twelve and a half on a 72Hz headset.
constexpr uint64_t kBindsCheckMs = 5000;

// How often edvr.ini is re-read so live tuning takes effect. Once a second.
constexpr uint64_t kConfigPollMs = 1000;

// How many times a session to point out that a history-key press was ignored
// because another window had focus. Three is enough to be noticed and few
// enough that a player who works with a browser focused is not papered with it.
constexpr uint32_t kMissedDumpNotes = 3;

State* g_state = nullptr;

// Which slot the flip timeline anchors its page on.
//
// DrawIndexed, because it is the entry the field evidence names: on the rig
// that hangs, slot 12 was measured switching from TID3D11DeviceContext_
// DrawIndexed_<1> to ..._DrawIndexed_Amortized<1> at the frame the GPU died,
// along with 23 of its neighbours. One PAGE is protected, not the whole table
// -- read-protecting a heap region can be megabytes and would fault on every
// unrelated write in it -- so the anchor decides which slots are covered, and
// the armed line prints that range rather than leaving it to be assumed.
constexpr size_t kFlipTimelineAnchorSlot = 12;

// Arm the flip timeline on the table the RUNTIME writes, if it was asked for.
//
// `table` must be the bottom hook's -- the context's own embedded table. Both
// call sites hand it one: the exposure hook's (it installs first, so its table
// is the runtime's in every mode) and, in the probe modes where no installer
// runs at all, the bare probe hook's.
void armFlipTimeline(Config& cfg, void** table, size_t span, const char* who) {
    if (!cfg.getBool("advanced.vtable_flip_timeline", false)) return;
    if (!table || span <= kFlipTimelineAnchorSlot) {
        Log::get().note(
            "advanced.vtable_flip_timeline = 1, but there is no context table to "
            "watch (%s). Nothing is armed. This needs the render context hooked, "
            "so it cannot work with advanced.d3d11_fixes = 0.",
            table ? "the table is shorter than the slot it anchors on" : "no hook took the context");
        return;
    }
    vtableWatchSlot(table, kFlipTimelineAnchorSlot, span, who, /*timeline=*/true);
}

// Defined below ensureState; used by the frame-path bindings-change check.
void readoptGameBindings();

// One budget per thing that can fail. Shader creation runs on whatever thread
// the game streams assets from; the frame boundary runs on the render thread and
// carries the exposure boundary, the vScreen boundary (which hosts the flash
// detector's per-frame work), the hotkeys and the config reload poll.
//
// Sharing one meant eight faults during asset streaming permanently stopped the
// entire frame heartbeat -- while the per-draw hooks, on their own budgets, kept
// running and mutating state. The log said only "FEATURE-DISABLED deviceHook",
// which does not tell anyone that the heartbeat is gone.
FaultBudget g_createBudget("deviceHook.createShader", 8);
FaultBudget g_frameBudget("deviceHook.frameBoundary", 8);

// Write one shader blob to the dump directory, named by its hash. Runs on
// the game's asset-streaming threads while armed; CreateDirectory once,
// CreateFile per blob, and a blob that already exists is skipped so a
// session's repeated creates cost one write each.
void dumpShaderBlob(const wchar_t* prefix, uint64_t hash, const void* bytecode,
                    SIZE_T len) {
    State* s = g_state;
    if (!s->shaderDumpDirMade) {
        s->shaderDumpDirMade = true;
        CreateDirectoryW(s->shaderDumpDir.c_str(), nullptr);
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\%s_%016llX.dxbc",
                 s->shaderDumpDir.c_str(), prefix,
                 static_cast<unsigned long long>(hash));
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // exists already, or unwritable
    DWORD written = 0;
    WriteFile(h, bytecode, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
}

HRESULT STDMETHODCALLTYPE hookedCreateVS(ID3D11Device* self, const void* bytecode,
                                         SIZE_T len, ID3D11ClassLinkage* linkage,
                                         void** out) {
    if (self != g_state->device) {
        return g_state->realCreateVS(self, bytecode, len, linkage, out);
    }
    const HRESULT hr = g_state->realCreateVS(self, bytecode, len, linkage, out);
    guardedBudget(g_createBudget, [&] {
        if (FAILED(hr) || !bytecode || len == 0 || !out || !*out) return;
        const uint64_t hash = fnv1a64(bytecode, len);
        registerShaderHash(*out, hash);
        // ...and its INPUT SIGNATURE, which is a different question from its
        // identity: whether the panel composite's shader reads the z of the
        // vertices it is handed decides whether the curved screen is possible
        // at all. See shader_sig.h.
        shaderSigRegister(*out, bytecode, static_cast<size_t>(len));
        if (g_state->shaderDump) dumpShaderBlob(L"vs", hash, bytecode, len);
    });
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreatePS(ID3D11Device* self, const void* bytecode,
                                         SIZE_T len, ID3D11ClassLinkage* linkage,
                                         void** out) {
    if (self != g_state->device) {
        return g_state->realCreatePS(self, bytecode, len, linkage, out);
    }
    const HRESULT hr = g_state->realCreatePS(self, bytecode, len, linkage, out);
    guardedBudget(g_createBudget, [&] {
        if (FAILED(hr) || !bytecode || len == 0 || !out || !*out) return;
        const uint64_t hash = fnv1a64(bytecode, len);
        registerShaderHash(*out, hash);
        if (g_state->shaderDump) dumpShaderBlob(L"ps", hash, bytecode, len);
    });
    return hr;
}

// Render targets made larger than asked: the FSS body layer, and surfaces
// named by size (fss_res.h). The match, the scaling and the refusal rules
// all live in that module; this hook only carries descs to it and created
// textures back. One bool per create when both matchers are off.
// Defined with the other six creates below; this one is hooked already, for a
// different reason, and only borrows the reporting.
void noteDeviceCreateFailure(size_t slot, HRESULT hr, const void* first,
                             const void* second, bool firstIsResource);

HRESULT STDMETHODCALLTYPE createTexture2DForwarded(ID3D11Device* self,
                                                   const D3D11_TEXTURE2D_DESC* desc,
                                                   const D3D11_SUBRESOURCE_DATA* init,
                                                   ID3D11Texture2D** out) {
    if (self != g_state->device || !desc || !fssResWantsCreates()) {
        return g_state->realCreateTexture2D(self, desc, init, out);
    }
    D3D11_TEXTURE2D_DESC d = *desc;
    bool inflated = false;
    guardedBudget(g_createBudget, [&] {
        inflated = fssResMaybeInflate(&d, init != nullptr);
    });
    if (!inflated) {
        return g_state->realCreateTexture2D(self, desc, init, out);
    }
    const HRESULT hr = g_state->realCreateTexture2D(self, &d, init, out);
    if (FAILED(hr)) {
        // The inflated create failed -- out of memory is the realistic way.
        // The game must still get its texture: fall back to the size it
        // asked for, untracked, exactly as if the fix were off.
        return g_state->realCreateTexture2D(self, desc, init, out);
    }
    if (out && *out) {
        guardedBudget(g_createBudget, [&] {
            // The factor comes from the two descs we already hold rather
            // than from the module, which would have to stash it between
            // the match and this call -- and this hook runs on the game's
            // streaming threads.
            fssResNoteCreated(*out, desc->Width, desc->Height,
                              desc->Width ? d.Width / desc->Width : 0);
        });
    }
    return hr;
}

// The same refusal line the other six creates get. Separate from the body
// above because that one has four returns and this has to see all of them.
HRESULT STDMETHODCALLTYPE hookedCreateTexture2D(ID3D11Device* self,
                                                const D3D11_TEXTURE2D_DESC* desc,
                                                const D3D11_SUBRESOURCE_DATA* init,
                                                ID3D11Texture2D** out) {
    const HRESULT hr = createTexture2DForwarded(self, desc, init, out);
    if (FAILED(hr) && self == g_state->device) {
        noteDeviceCreateFailure(kDevCreateTexture2D, hr, desc, init, false);
    }
    return hr;
}

// A fault while REPORTING a refusal must not switch off shader hashing, and
// the reads below are of the game's own desc -- a pointer we were handed and
// did not size. Its own budget, and a small one.
FaultBudget g_createFailBudget("deviceHook.createFailNote", 4);

// At most this many refusals a session. A create that fails once usually
// fails every frame, and the line that matters is the first.
constexpr uint32_t kCreateFailNotes = 12;

// The failures a create can actually come back with, named. A hex HRESULT in
// a bug report is a lookup somebody has to do before they can think, and the
// two families mean completely different things: 0x8007xxxx is D3D refusing
// the arguments, 0x887Axxxx is DXGI saying the device is gone.
const char* hresultName(HRESULT hr) {
    switch (hr) {
        case E_INVALIDARG:                    return "E_INVALIDARG";
        case E_OUTOFMEMORY:                   return "E_OUTOFMEMORY";
        case E_NOTIMPL:                       return "E_NOTIMPL";
        case E_FAIL:                          return "E_FAIL";
        case DXGI_ERROR_INVALID_CALL:         return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_UNSUPPORTED:          return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_DEVICE_REMOVED:       return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_HUNG:          return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_RESET:         return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_WAS_STILL_DRAWING:    return "DXGI_ERROR_WAS_STILL_DRAWING";
        default:                              return "unnamed";
    }
}

// GetDeviceRemovedReason's answers, which are a smaller set and carry the
// blame: HUNG is a TDR reset (something took longer than Windows allows),
// RESET is somebody else's fault landing on us, DRIVER_INTERNAL_ERROR is the
// driver, INVALID_CALL is a command we should never have submitted.
const char* deviceRemovedReasonName(HRESULT why) {
    switch (why) {
        case S_OK: return "the device does not report itself removed, which "
                          "means this was asked too early or too late";
        case DXGI_ERROR_DEVICE_HUNG:
            return "DXGI_ERROR_DEVICE_HUNG -- the GPU stopped responding and "
                   "Windows reset it, which is what work overrunning the TDR "
                   "timeout looks like";
        case DXGI_ERROR_DEVICE_RESET:
            return "DXGI_ERROR_DEVICE_RESET -- the device was reset by "
                   "something outside this process";
        case DXGI_ERROR_DEVICE_REMOVED:
            return "DXGI_ERROR_DEVICE_REMOVED -- the adapter itself went away";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return "DXGI_ERROR_DRIVER_INTERNAL_ERROR -- the display driver "
                   "failed on its own";
        case DXGI_ERROR_INVALID_CALL:
            return "DXGI_ERROR_INVALID_CALL -- an invalid command reached the "
                   "GPU, which is the one of these that would be ours";
        default: return "unnamed";
    }
}

const char* devCreateName(size_t slot) {
    switch (slot) {
        case kDevCreateBuffer:    return "CreateBuffer";
        case kDevCreateTexture1D: return "CreateTexture1D";
        case kDevCreateTexture2D: return "CreateTexture2D";
        case kDevCreateTexture3D: return "CreateTexture3D";
        case kDevCreateSrv:       return "CreateShaderResourceView";
        case kDevCreateUav:       return "CreateUnorderedAccessView";
        case kDevCreateRtv:       return "CreateRenderTargetView";
        case kDevCreateDsv:       return "CreateDepthStencilView";
        default:                  return "a device create";
    }
}

// D3D11 refused something the GAME asked for. See kDevCreateBuffer above for
// why this is worth a line: the crash that follows names nothing.
void noteDeviceCreateFailure(size_t slot, HRESULT hr, const void* first,
                             const void* second, bool firstIsResource) {
    if (g_state->createFailNotes >= kCreateFailNotes) return;
    ++g_state->createFailNotes;

    char detail[320];
    detail[0] = 0;
    guardedBudget(g_createFailBudget, [&] {
        if (firstIsResource) {
            // A view desc opens with format and dimension before its union,
            // and the resource is the thing that format has to be compatible
            // WITH -- so both, when the resource is a 2D texture, which every
            // render target and depth target here is.
            const uint32_t* vd = static_cast<const uint32_t*>(second);
            const unsigned fmt = second ? vd[0] : 0u;
            const unsigned dim = second ? vd[1] : 0u;
            ID3D11Texture2D* tex = nullptr;
            if (first) {
                ID3D11Resource* res = static_cast<ID3D11Resource*>(
                    const_cast<void*>(first));
                res->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&tex));
            }
            if (tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                _snprintf_s(detail, _TRUNCATE,
                            "view format %u dimension %u, over a %ux%u texture of "
                            "format %u, %u mip(s), array %u, %ux MSAA, usage %u, "
                            "bind 0x%X, cpu 0x%X, misc 0x%X",
                            fmt, dim, td.Width, td.Height,
                            static_cast<unsigned>(td.Format), td.MipLevels,
                            td.ArraySize, td.SampleDesc.Count,
                            static_cast<unsigned>(td.Usage), td.BindFlags,
                            td.CPUAccessFlags, td.MiscFlags);
                tex->Release();
            } else {
                _snprintf_s(detail, _TRUNCATE,
                            "view format %u dimension %u, over a resource that is "
                            "not a 2D texture", fmt, dim);
            }
        } else if (first) {
            // Six words: exactly the length of the shortest of these descs
            // (D3D11_BUFFER_DESC), so this never reads past one it was given.
            const uint32_t* d = static_cast<const uint32_t*>(first);
            _snprintf_s(detail, _TRUNCATE,
                        "desc words %u %u %u %u %u %u%s", d[0], d[1], d[2], d[3],
                        d[4], d[5], second ? ", with initial data" : "");
        }
    });

    // WHY THE DEVICE WENT, when it went. DEVICE_REMOVED on a call whose
    // arguments were perfectly good says only that the GPU was already gone;
    // the reason code says whether it hung (a TDR reset, which is what a
    // dispatch of ours overrunning two seconds would look like), was reset by
    // something else, or the driver fell over on its own. It is one call, it
    // is the discriminator, and asking for it needs the device we already
    // hold. Issue #20 got this far and no further without it.
    // 320, not 96. The first field build truncated the answer mid-sentence --
    // "(DXGI_ERROR_DEVICE_HUNG -- the GPU stopped responding" and then nothing,
    // losing the half that says what to do about it. The reason strings run to
    // about 180 characters because they are written to be read by whoever
    // pasted the log, not looked up.
    char removed[320];
    removed[0] = 0;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT why = g_state->device ? g_state->device->GetDeviceRemovedReason()
                                            : S_OK;
        _snprintf_s(removed, _TRUNCATE, " The device removal reason is 0x%08X (%s).",
                    static_cast<unsigned>(why), deviceRemovedReasonName(why));
    }

    Log::get().note(
        "D3D11 REFUSED the game's %s: hr 0x%08X (%s), %s.%s Reported because "
        "Elite treats every failure but DXGI_ERROR_WAS_STILL_DRAWING as fatal "
        "-- its own check calls abort(), and the process then dies at one fixed "
        "address that every unrelated fatal error in the game shares, with "
        "nothing of ours on the stack. If a crash follows this line, this line "
        "is why. At most %u a session.",
        devCreateName(slot), static_cast<unsigned>(hr), hresultName(hr),
        detail[0] ? detail : "no desc was passed", removed, kCreateFailNotes);
}

// One body behind all seven slots. It forwards, and on a failure it says so.
// Nothing else: the arguments go through untouched and the HRESULT comes back
// untouched, so a session with this hooked renders exactly as one without it.
template <size_t Slot, bool FirstIsResource>
HRESULT STDMETHODCALLTYPE hookedDevCreate(ID3D11Device* self, const void* first,
                                          const void* second, void** out) {
    const HRESULT hr = g_state->realDevCreate[Slot](self, first, second, out);
    // Patching in place hooks the CLASS, so another device sharing the table
    // arrives here too. It gets the same forward; only ours gets the line.
    if (FAILED(hr) && self == g_state->device) {
        noteDeviceCreateFailure(Slot, hr, first, second, FirstIsResource);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateCS(ID3D11Device* self, const void* bytecode,
                                         SIZE_T len, ID3D11ClassLinkage* linkage,
                                         void** out) {
    // Patching vtable entries in place hooks the CLASS, so any other device
    // sharing this table -- a wrapper mod's internal one, a second device the
    // game makes for a probe -- arrives here and must pass straight through.
    // See vtable_hook.h.
    if (self != g_state->device) {
        return g_state->realCreateCS(self, bytecode, len, linkage, out);
    }
    const HRESULT hr = g_state->realCreateCS(self, bytecode, len, linkage, out);
    guardedBudget(g_createBudget, [&] {
        if (FAILED(hr) || !bytecode || len == 0 || !out || !*out) return;
        const uint64_t hash = fnv1a64(bytecode, len);
        registerShaderHash(*out, hash);
        // COMPUTE shaders dump too (2026-09-07), and they had to start.
        //
        // This hook has registered their hashes since it was written, so a
        // census could NAME a dispatch -- and the dump wrote only vs_ and
        // ps_, so nothing could ever read one. That gap bit twice in one
        // day. The per-object motion work found the game reading the scene
        // depth's STENCIL plane from compute (5998146D464F5C0E and
        // EB0245DE0BB23BB6, the amortized tile renderer of
        // docs/fss-scanner.md), which is a consumer a stencil tag must not
        // disturb and which the draw-level so= column cannot see, because a
        // compute shader has no depth-stencil state to record. And the
        // temporal pass's own dispatch hash changes whenever its shader
        // does, which a dump makes checkable instead of inferable.
        if (g_state->shaderDump) dumpShaderBlob(L"cs", hash, bytecode, len);
    });
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* self, UINT syncInterval,
                                        UINT flags) {
    // Not our swapchain: forward and do no frame work. A second swapchain
    // (an overlay's, a mod's) shares this vtable and its Present is not our
    // frame boundary. See vtable_hook.h.
    if (self != g_state->swapChain) {
        return g_state->realPresent(self, syncInterval, flags);
    }
    // The time blocked in the real Present is the monitor's, with the time
    // blocked in WaitGetPoses: the frame period less the two is the render
    // thread's own busy time.
    const int64_t presentT0 = qpcNow();
    const HRESULT hr = g_state->realPresent(self, syncInterval, flags);
    if (qpcFrequency() > 0) {
        perfMonitorNotePresentWait(static_cast<double>(qpcNow() - presentT0) * 1000.0 /
                                   static_cast<double>(qpcFrequency()));
    }

    // OUTSIDE the fault budget, and that is the point. Confirming is a file
    // delete; putting it inside would mean a burst of faults anywhere in the
    // frame work stops the confirmation, the sentinel trips on the next launch,
    // and every fix switches itself off over something that never crashed.
    ++g_state->framesSeen;
    const uint64_t presentMs = stampMs();
    if (g_state->lastPresentMs != 0) {
        const uint64_t frameMs = presentMs - g_state->lastPresentMs;
        if (frameMs <= kSentinelMaxFrameMs) g_state->presentingMs += frameMs;
    }
    g_state->lastPresentMs = presentMs;
    if (!g_state->sentinelConfirmed && g_state->presentingMs >= kSentinelConfirmMs) {
        g_state->sentinelConfirmed = true;
        if (g_state->sentinel) g_state->sentinel->confirm();
    }

    // The other half of 1f's gate. See the note at hookedSubmit.
    if (!g_state->threadNoted) {
        g_state->threadNoted = true;
        Log::get().note(
            "Present is running on thread %lu. If the openvr log reports a "
            "different thread for Submit, a copy issued from Submit would touch "
            "the immediate context off its own thread.",
            static_cast<unsigned long>(GetCurrentThreadId()));
    }

    // The frame boundary's own CPU time, credited to the frame the monitor
    // just ringed inside it (menuTick runs perfMonitorFrame), so a dropped
    // frame's row says what EDVR's boundary work cost in it.
    const int64_t boundaryT0 = qpcNow();
    guardedBudget(g_frameBudget, [&] {
        if (g_state->toggleKey.pressed()) toggleExposureFix();
        // Deliberately not part of the toggle: it reports, it does not change
        // anything, so there is no reason for it to follow the fix being off.
        // The two diagnostic settings, re-read every frame.
        //
        // They were read once at install, like everything else in ensureState,
        // and both are numbers you find by FEEL from inside a headset -- which
        // means a relaunch per guess, which is not tuning. The same argument the
        // head offset made when it moved onto the reload path, and vscreen.h
        // records the same mistake before that: two settings documented as
        // changeable while the game runs that were never re-read, reported as
        // the fix being broken.
        //
        // Every frame rather than on a poll, because the cost is two lookups in
        // a map that is already in memory -- vScreenRefreshConfig does the file
        // check, so nothing here touches the disk.
        g_state->dumpOnExternalCam =
            Config::get().getBool("advanced.dump_camera_on_external_cam", false);
        g_state->holdFramesOnExternalCam = static_cast<uint32_t>(
            Config::get().getIntInRange("experimental.hold_frames_on_external_cam", 0, 0, 120));

        // The history key dumps TWICE: now, and again two seconds from now.
        //
        // A flash you react to sits about 300 ms back, which is the last few
        // rows of a ring that holds only what came BEFORE the press. And a bad
        // frame is one that leaves the line and RETURNS -- a shape that needs
        // frames on both sides of it, which an event at the ring's edge does
        // not have. So the one capture that is guaranteed to contain the thing
        // being chased is also the one least able to show it.
        //
        // The second dump costs one more press of nothing: same ring, two
        // seconds later, by which time the event has moved to the middle with
        // its recovery behind it. The pair is the point -- the first is the
        // reaction-time capture, the second is the one you read.
        if (g_state->dumpKey.pressed()) {
            dumpCameraRing("the history key");
            g_state->dumpDueMs = nowMs() + kDumpDelayMs;
        }
        // THE PRESS THAT WENT NOWHERE, said out loud.
        //
        // Hotkeys only fire while the game window has focus, which is correct --
        // GetAsyncKeyState is global, and Scroll Lock typed in a browser used to
        // toggle the brightness fix. But in VR another window holding focus is
        // ordinary rather than exceptional, and for THIS key the silent failure
        // is circular: the player presses the key that writes the log, nothing
        // is written, and the log that would explain why is the one that was not
        // written. Measured 2026-08-15: a session where the external-camera key
        // registered twice and Pause never did, so a reported flash had no
        // capture and the reason was invisible.
        //
        // Capped, because a player who keeps a browser focused could otherwise
        // paper the log with it -- and after three the point has been made.
        if (g_state->missedDumpNotes < kMissedDumpNotes &&
            g_state->dumpKey.takeMissedWhileUnfocused()) {
            ++g_state->missedDumpNotes;
            Log::get().note(
                "the camera history key was pressed, but another window had "
                "focus, so nothing was written. EDVR only acts on its hotkeys "
                "while Elite itself is the active window -- otherwise a key "
                "typed in a browser would reach it. Click on the game window "
                "(the flat one on your desktop) and press it again. Said at most "
                "%u times a session.",
                kMissedDumpNotes);
        }
        // The delayed dump, armed by either key.
        if (g_state->dumpDueMs != 0 && nowMs() >= g_state->dumpDueMs) {
            g_state->dumpDueMs = 0;
            dumpCameraRing("a key you pressed two seconds ago",
                           (uint32_t)(kDumpDelayMs / 1000));
        }
        // The draw census key (issue 69074). Same silent-failure shape as the
        // history key, same cure: a diagnostic keypress that another window
        // swallowed must say so, because the log it failed to write is the
        // place anyone would look for the reason.
        if (g_state->censusKey.pressed()) {
            drawCensusRequest();
            // Same key: the census says WHAT was drawn, the quad probe says
            // WHERE. Two instruments on one press keeps the two answers on
            // the same frame, which is the only way they can be compared.
            quadProbeRequest();
            perfMonitorNoteEvent(kEvCensus);
        }
        if (g_state->missedCensusNotes < kMissedDumpNotes &&
            g_state->censusKey.takeMissedWhileUnfocused()) {
            ++g_state->missedCensusNotes;
            Log::get().note(
                "the draw census key was pressed, but another window had "
                "focus, so nothing was captured. Click on the game window and "
                "press it again. Said at most %u times a session.",
                kMissedDumpNotes);
        }
        // The player's Elite bindings, re-read when the game rewrites them.
        // Elite saves Options\Bindings the moment a rebind or preset switch
        // is applied, so a slow stat notices within seconds and the adopted
        // hotkeys follow without a restart. The change must HOLD across two
        // checks before anything is re-read: an Apply writes several files,
        // and half a save is not a configuration.
        if (dueMs(g_state->bindsCheckMs, kBindsCheckMs)) {
            g_state->bindsCheckMs = stampMs();
            if (Config::get().getBool("hotkey.read_game_bindings", true)) {
                const uint64_t fp = eliteBindsFingerprint();
                if (fp == g_state->bindsFingerprint) {
                    g_state->bindsPending = 0;
                } else if (fp == g_state->bindsPending) {
                    g_state->bindsFingerprint = fp;
                    g_state->bindsPending = 0;
                    readoptGameBindings();
                } else {
                    g_state->bindsPending = fp;
                }
            }
        }
        // The game's own journal, polled about once a second: it states the
        // two boundaries EDVR used to infer -- gameplay starting (LoadGame)
        // and on-foot sessions beginning (Disembark, where the game resets
        // its camera view to 0).
        journalWatchTick();
        if (journalWatchActive()) {
            const uint32_t d = journalDisembarks();
            if (d != g_state->lastJournalDisembarks) {
                g_state->lastJournalDisembarks = d;
                headOffsetGateNewFootSession(
                    "the game's journal says you disembarked",
                    /*journalSaysSo=*/true);
            }
            const uint32_t e = journalEmbarks();
            if (e != g_state->lastJournalEmbarks) {
                g_state->lastJournalEmbarks = e;
                headOffsetGateNoteEmbark();
            }
        }
        // The game's live on-foot word, and the camera-entry edge. The first
        // is what makes a KEYLESS install work at all (the gate turns it into
        // intent, 6bb); the second nudges the view scanner so fresh
        // candidates exist while the player is still cycling to their view --
        // which is what the anchored two-step certification feeds on.
        headOffsetGateSetOnFootLive(journalOnFootKnown(), journalOnFoot(),
                                    journalStatusSamples());
        headOffsetGateSetWakeLive(journalSupercruiseKnown(), journalSupercruise(),
                                  journalInJumpTunnel());
        {
            const uint32_t entries = headOffsetGateEnterCount();
            if (entries != g_state->lastCameraEnters) {
                g_state->lastCameraEnters = entries;
                cameraViewNudgeRescan();
            }
        }
        // Camera keys mean the CAMERA only once gameplay has started. Before
        // LoadGame every press is menu navigation -- and the next-view key is
        // typically an arrow, which menus eat by the dozen; counting those
        // walked the view count away from reality before the game even began
        // (6ba). Without the journal, behaviour is exactly as before.
        const bool keysMeanGame = !journalWatchActive() || journalGameplay();
        // Told to the gate, not acted on here. These keys are the player's OWN
        // Elite bindings: EDVR does not send them, press them or interfere with
        // them -- it watches for the same press the game gets, so it knows
        // which mode the player just asked for.
        if (keysMeanGame && g_state->externalCamKey.pressed()) {
            headOffsetGateKeyPressed();
            // The camera history, triggered by the press but taken AFTER it.
            //
            // Entering and leaving the external camera is reported as flashing,
            // and it is a transition nobody can press Pause during: by the time
            // they reach that key the ten seconds of history are the ten seconds
            // after the thing they wanted.
            //
            // Dumping ON the press has the same fault in the other direction --
            // the ring holds the frames BEFORE it, so it would capture ten
            // seconds of standing still and none of the transition. The delay is
            // the whole point: two seconds later the ring holds the press, the
            // mode change and the flash, with eight seconds of ordinary flight
            // in front of them for comparison.
            if (g_state->dumpOnExternalCam) g_state->dumpDueMs = nowMs() + kDumpDelayMs;
            // Hold the last good frame across the transition.
            //
            // Asked for on the PRESS, which is the earliest possible moment and
            // the only one that is not a guess: the player has just told us a
            // transition is starting. Everything the detector does downstream of
            // this is inference; this is not.
            // ON FOOT ONLY, and the same state Explorer Cam gates on.
            //
            // This is the player's own Elite binding and it opens the SHIP's
            // vanity camera too. Holding frames there costs 83 ms each for a
            // transition this was never measured against and does not claim to
            // fix -- and a hold in the cockpit is the same shape of mistake as
            // the head offset arming there, which is the failure the gate exists
            // to prevent.
            //
            // Two ways to be in the right place, because the press means
            // opposite things at each end: entering, the flat panel is up and
            // settled; leaving, the gate is already published as on-foot
            // external. Neither alone covers both directions.
            const bool onFootContext =
                headOffsetGatePanelSettled() || externalCameraOnFoot();
            if (g_state->holdFramesOnExternalCam > 0 && onFootContext) {
                requestSubmitHold(g_state->holdFramesOnExternalCam);
            }
        }
        // The next-view key, promoted to the public build on 2026-08-15. It
        // was deliberately private-only while the game read covered
        // everything -- but near a planet the read dies for stretches, the
        // bridge holds the last confirmed view through them, and cycling
        // during a hold was then INVISIBLE: the offset stayed armed on every
        // preset the player cycled to. With this bound, the count follows
        // each press, so the offset drops the moment you cycle off the wanted
        // view and returns when you cycle back -- read or no read. The press
        // is also timestamped for the watcher: a candidate record stepping
        // exactly when the finger does is the certification no impostor has
        // matched (6aw).
        if (keysMeanGame && g_state->extCamNextKey.pressed()) {
            cameraViewNotePress();
            headOffsetGateViewBumped();
        }
        // The witness call is deliberately the same one: what certification
        // needs is that the player's finger moved at this instant, not which
        // way round the cycle it sent them.
        if (keysMeanGame && g_state->extCamPrevKey.pressed()) {
            cameraViewNotePress();
            headOffsetGateViewUnbumped();
        }
        // AND THE SAME PRESSES ON A GAMEPAD. Elite binds the view cycle to
        // the D-pad by default and the keyboard arrows only as a secondary,
        // so watching the keyboard alone misses every press a pad player
        // makes -- which reads downstream as a count that is quietly one or
        // two behind, with nothing to say why (field, 2026-09-02).
        if (keysMeanGame && xinputPressed(g_state->extCamNextPad)) {
            cameraViewNotePress();
            headOffsetGateViewBumped();
        }
        if (keysMeanGame && xinputPressed(g_state->extCamPrevPad)) {
            cameraViewNotePress();
            headOffsetGateViewUnbumped();
        }
        // The FSS theater's mode latch: the player's own FSS keys give
        // frame-exact edges -- press enter and the screen is up THIS
        // frame, press quit and it is gone. Underneath, the game's
        // GuiFocus is the authority that heals every path a key cannot
        // see: a press the game ignored (not in supercruise), an exit by
        // ESC or an interdiction, an entry from a UI, an unbound or
        // non-keyboard key. A key engage carries a grace while the
        // status file catches up; once the game confirms the mode, its
        // word alone ends it. A quit press suppresses the focus engage
        // until the game agrees the mode ended, so stale status cannot
        // re-open a screen the player just closed.
        if (g_state->fssTheaterWanted) {
            xinputWatchTick();
            const bool wasLatched = g_state->fssModeLatch;
            bool byKey = false;
            // Evaluate BOTH watchers every frame -- pressed() keeps edge
            // state, and a short-circuited call would miss its edge.
            bool enterPressed = g_state->fssEnterKey.pressed();
            if (xinputPressed(g_state->fssEnterPad)) enterPressed = true;
            bool quitPressed = g_state->fssQuitKey.pressed();
            if (xinputPressed(g_state->fssQuitPad)) quitPressed = true;
            if (keysMeanGame && enterPressed &&
                (!journalSupercruiseKnown() || journalSupercruise())) {
                g_state->fssModeLatch = true;
                g_state->fssLatchByKey = true;
                g_state->fssLatchMs = stampMs();
                byKey = true;
            }
            if (keysMeanGame && quitPressed) {
                g_state->fssModeLatch = false;
                g_state->fssQuitMs = stampMs();
                byKey = true;
            }
            // The zoom press: the earliest arrival marker there is --
            // ahead of the camera jump, ahead of any render signal. The
            // squares appear the frame the zoom begins (the field's own
            // timing), so the reveal's window must open no later.
            bool zoomPressed = g_state->fssZoomStepKey.pressed();
            if (g_state->fssZoomKey.pressed()) zoomPressed = true;
            if (xinputPressed(g_state->fssZoomStepPad)) zoomPressed = true;
            if (xinputPressed(g_state->fssZoomPad)) zoomPressed = true;
            if (zoomPressed && g_state->fssModeLatch) {
                g_state->fssZoomPressPending = true;
            }
            if (journalFssFocus()) {
                const bool quitRecent =
                    g_state->fssQuitMs != 0 &&
                    stampMs() - g_state->fssQuitMs < 3000;
                if (!g_state->fssModeLatch && !quitRecent) {
                    g_state->fssModeLatch = true;
                }
                if (g_state->fssModeLatch) g_state->fssLatchByKey = false;
            } else {
                g_state->fssQuitMs = 0;
                if (g_state->fssModeLatch && journalFssFocusKnown()) {
                    const bool grace =
                        g_state->fssLatchByKey &&
                        stampMs() - g_state->fssLatchMs < 4000;
                    if (!grace) g_state->fssModeLatch = false;
                }
            }
            if (wasLatched != g_state->fssModeLatch &&
                g_state->fssLatchNotes < 6) {
                ++g_state->fssLatchNotes;
                Log::get().note(
                    g_state->fssModeLatch
                        ? "fss theater: mode latch OPEN (%s)."
                        : "fss theater: mode latch closed (%s). Said at "
                          "most 6 times.",
                    byKey ? "your FSS key" : "the game's GuiFocus");
            }
        }
        // The settings menu (docs/settings-menu.md): its summon key, its
        // navigation keys and head-aim, its fade, the keyboard gate that
        // follows its draw, and the upload of a fresh raster. One key poll
        // when closed.
        menuTick(g_state->device);
        // Reading the view the game is actually on, and telling the gate.
        //
        // The keypress count above stays as the fallback, for when this cannot
        // answer: an offset a game update has moved, a record that has been
        // reused, a scan that found nothing. cameraViewCurrent returns -1 in
        // all of those and the gate goes back to counting.
        headOffsetGateSetView(cameraViewCurrent());
        // One per-frame invalidation for both fixes, before either boundary.
        //
        // This used to be two, with opposite policies: vscreen dropped its
        // derived answers and kept its pointers, exposure_fix dropped its
        // pointers. Each had a failure mode the other did not, and having two
        // guaranteed the next fix would copy one of them wrongly. device_hook
        // owns the frame; it owns this.
        bindingFrameBoundary();
        exposureFixFrameBoundary();
        vScreenFrameBoundary();

        // THE FAST PATROL on the two context hooks, every frame.
        //
        // Their opponent is not a tool that installs once. On issue #21's rig
        // the D3D11 runtime rewrites the same slots about once a second for the
        // whole session, and against a one-hertz rewriter the once-a-second
        // pass below is the worst cadence available: our thunks end up in the
        // table for part of every second and out of it for the rest, so the
        // fixes do not fail, they STROBE. This closes the window to a frame.
        //
        // Only the two CONTEXT hooks. The device, swapchain and factory tables
        // have never been contested by anything in the field, they carry no
        // per-thunk counters to vouch with, and their reclaim cannot heal
        // anything without one -- so running them at frame rate would buy a
        // VirtualQuery per foreign entry per frame and nothing else.
        //
        // These passes vouch NOTHING; see vScreenReclaimTick for why that is
        // the whole safety argument rather than a shortcut.
        //
        // The write watch's per-frame work comes FIRST and lives here rather
        // than inside vScreenReclaimTick, where the re-arm used to sit behind
        // `if (!g_state) return;`. In the two context probes vScreen never
        // installs, so g_state is null, so neither the re-arm nor the flip
        // timeline's drain ran at all -- in exactly the sessions the
        // instruments exist for. Both are no-ops when nothing is armed.
        vtableWatchRearm();
        vtableWatchFrameTick(g_state->frameCounter);
        vScreenReclaimTick();
        exposureFixReclaimTick();

        // The multithread-protection sample. One virtual call that reads a
        // flag, per frame, and a line only when the answer differs from last
        // time -- so a rig where nothing moves pays a compare and says nothing,
        // and a rig where it toggles every frame says so in the first second
        // and then at doublings. See the State field for what it settles.
        if (g_state->multithread) {
            ++g_state->mtFrames;
            const int now = g_state->multithread->GetMultithreadProtected() ? 1 : 0;
            if (now != g_state->mtProtected) {
                g_state->mtProtected = now;
                ++g_state->mtChanges;
                const uint32_t n = g_state->mtChanges;
                if (n <= 4 || (n & (n - 1)) == 0) {
                    ++g_state->mtReports;
                    Log::get().note(
                        "multithread protection CHANGED to %s (change #%u, "
                        "frame %llu). Every one of these makes Windows re-lay "
                        "the context's entire function table, which is what "
                        "removes EDVR's hooks from it. Reported for the first "
                        "few and then at doublings.",
                        now ? "ON" : "off", n,
                        static_cast<unsigned long long>(g_state->mtFrames));
                }
            }
            // The standing answer, said once, because a NEGATIVE has to be
            // stated to be read. A session where this never changes prints no
            // change lines at all, and "no lines" is indistinguishable from
            // "the probe never ran" -- which is the shape of mistake this
            // investigation has already made three times.
            if (g_state->mtFrames == 1800 && !g_state->mtSettledNoted) {
                g_state->mtSettledNoted = true;
                Log::get().note(
                    "multithread protection after 1800 frames: %s, changed %u "
                    "time(s). If that count is zero on a rig whose context "
                    "table is still being rewritten every frame, then this flag "
                    "is NOT what is causing it and the cause is something else "
                    "reaching the same routine.",
                    g_state->mtProtected ? "ON" : "off", g_state->mtChanges);
            }
        }
        // Polled rather than watched, twice a second by the journal watcher
        // and once a second here. The user is wearing a
        // headset and cannot see a text editor, so the settings that are worth
        // tuning by feel have to take effect without a restart. Was every 90
        // frames, which is once a second on exactly one of the three rates.
        ++g_state->frameCounter;
        // The menu asks for the poll NOW after each write it made, so the
        // change lands this frame through the same configure path a hand
        // edit takes -- nothing applies a value except the reload.
        if (menuTakeConfigPollRequest() || dueMs(g_state->configPollMs, kConfigPollMs)) {
            g_state->configPollMs = stampMs();
            vScreenRefreshConfig();
            g_state->fssTheaterWanted =
                Config::get().getFloat("experimental.fss_theater", 0.0f) > 0.0f ||
                eyeSyncFromConfig(Config::get()).any();
            journalWatchSetEagerStatus(g_state->fssTheaterWanted);
            // The liveness pass, on the same once-a-second cadence. In-place
            // patches are on a table other tools can write too, and one that
            // installs after EDVR and resolves its "original" pointers from a
            // clean vtable erases ours without a trace -- measured 2026-08-18:
            // OpenXR Toolkit under OpenComposite re-pointed the draw,
            // render-target-bind and dispatch slots at its XR session init, a
            // few seconds after install, and four fixes starved silently while
            // Map/Unmap kept arriving and made the log look half-alive.
            //
            // The three hooks here pass no vouch list, so they are DETECTION
            // ONLY: their slots are rare calls (CreateComputeShader at asset
            // loads, CreateSwapChain once) or the heartbeat itself (Present),
            // and "quiet" is a normal state for all of them -- which is
            // exactly the evidence a vouch must never be built on. A re-point
            // of one of these gets a named log line instead of silence, and
            // that is the whole improvement on offer for them; re-patching
            // without call evidence risks looping a chainer, which is worse
            // than the bypass it would heal. The context hooks below carry
            // per-thunk counters and do vouch. See VTableHook::reclaim.
            //
            // This rides the swapchain hook's Present: if THAT slot is ever the
            // one re-pointed, the heartbeat running this check dies with it.
            // Accepted, not overlooked -- the field case left Present alone
            // (the totals windows kept printing all session), and a watchdog
            // thread for a hook that has never been hit is machinery this
            // codebase would have to get right on every other axis too.
            g_state->deviceHook.reclaim("d3d11 device");
            g_state->swapChainHook.reclaim("game swapchain");
            g_state->factoryHook.reclaim("dxgi factory");
            // vScreen first: its return is the eye-draws-since-last-pass fact
            // the exposure vouch is gated on, because compute silence during
            // a loading screen is ordinary and only compute silence during a
            // RENDERED SCENE is evidence of bypass.
            const bool sceneRendered = vScreenReclaimHooks();
            exposureFixReclaimHooks(sceneRendered);
        }
    });
    if (qpcFrequency() > 0) {
        perfMonitorNoteCpu(kCpuBoundary, static_cast<double>(qpcNow() - boundaryT0) * 1000.0 /
                                             static_cast<double>(qpcFrequency()));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                DXGI_SWAP_CHAIN_DESC* desc,
                                                IDXGISwapChain** out) {
    const HRESULT hr = g_state->realCreateSwapChain(self, device, desc, out);
    // Only swapchains from the factory we attached to are the game's. A
    // wrapper mod makes its own through a factory sharing this vtable, and
    // hooking one of those would put our frame boundary on somebody else's
    // presentation. See vtable_hook.h.
    if (SUCCEEDED(hr) && out && *out && self == g_state->factory) {
        hookSwapChain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrictTo,
    IDXGISwapChain1** out) {
    const HRESULT hr =
        g_state->realCreateSwapChainForHwnd(self, device, hwnd, desc, fs, restrictTo, out);
    if (SUCCEEDED(hr) && out && *out &&
        static_cast<void*>(self) == g_state->factory) {
        hookSwapChain(*out);
    }
    return hr;
}

// Re-run the game-bindings adoption after Elite rewrote its files. Only the
// keys the ini leaves empty are touched -- an explicit ini value never moves.
// A keyboard binding that VANISHED (moved to a controller, unbound) clears
// the watch rather than leaving a phantom key: pressing a key the game no
// longer acts on would flip EDVR's idea of where you are while the game
// stands still, which is the missed-press desync class.
void readoptGameBindings() {
    char b[48];
    bool changed = false;
    perfMonitorNoteEvent(kEvBinds);
    {
        const auto before = g_state->externalCamKey.key();
        // The ON-FOOT element first: the game acts on _Humanoid on foot,
        // and the ship's PhotoCameraToggle only agrees by coincidence --
        // measured 12:14, a Humanoid rebind EDVR missed entirely while
        // faithfully watching the unchanged ship key.
        if (eliteBindsLookup("PhotoCameraToggle_Humanoid", b, sizeof(b),
                             "PhotoCameraToggle")) {
            g_state->externalCamKey.setBinding(b);
            if (g_state->externalCamKey.key() != before) {
                changed = true;
                Log::get().note("hotkey: your Elite bindings changed -- "
                                "external_camera is now %s.", b);
            }
        } else if (before != 0) {
            changed = true;
            g_state->externalCamKey.setBinding("");
            Log::get().note(
                "hotkey: your Elite bindings changed and the external camera "
                "is no longer on a keyboard key, so the old key is no longer "
                "watched. Bind a keyboard key for it in Elite to use Explorer "
                "Cam.");
        }
        headOffsetGateSetKeyBound(g_state->externalCamKey.key() != 0);
    }
    {
        const auto before = g_state->extCamNextKey.key();
        if (eliteBindsLookup("VanityCameraScrollRight", b, sizeof(b))) {
            g_state->extCamNextKey.setBinding(b);
            if (g_state->extCamNextKey.key() != before) {
                changed = true;
                Log::get().note("hotkey: your Elite bindings changed -- "
                                "external_camera_next is now %s.", b);
            }
        } else if (before != 0) {
            changed = true;
            g_state->extCamNextKey.setBinding("");
            Log::get().note(
                "hotkey: your Elite bindings changed and the next-view key is "
                "no longer on a keyboard key, so the old key is no longer "
                "watched.");
        }
    }
    {
        const auto before = g_state->extCamPrevKey.key();
        if (eliteBindsLookup("VanityCameraScrollLeft", b, sizeof(b))) {
            g_state->extCamPrevKey.setBinding(b);
            if (g_state->extCamPrevKey.key() != before) {
                changed = true;
                Log::get().note("hotkey: your Elite bindings changed -- "
                                "external_camera_prev is now %s.", b);
            }
        } else if (before != 0) {
            changed = true;
            g_state->extCamPrevKey.setBinding("");
            Log::get().note(
                "hotkey: your Elite bindings changed and the previous-view key "
                "is no longer on a keyboard key, so the old key is no longer "
                "watched.");
        }
    }
    {
        char fb[48];
        if (eliteBindsLookup("ExplorationFSSEnter", fb, sizeof(fb))) {
            g_state->fssEnterKey.setBinding(fb);
        } else {
            g_state->fssEnterKey.setBinding("");
        }
        if (eliteBindsLookup("ExplorationFSSQuit", fb, sizeof(fb))) {
            g_state->fssQuitKey.setBinding(fb);
        } else {
            g_state->fssQuitKey.setBinding("");
        }
        char camMod2[40] = {0};
        const bool haveCamMod2 =
            eliteBindsLookupPadMod("PhotoCameraToggle_Humanoid", camMod2,
                                   sizeof(camMod2)) ||
            eliteBindsLookupPadMod("PhotoCameraToggle", camMod2,
                                   sizeof(camMod2));
        g_state->extCamNextPad = XinputBinding{};
        if (eliteBindsLookupPad("VanityCameraScrollRight", fb, sizeof(fb)) &&
            xinputTranslate(fb, &g_state->extCamNextPad) && haveCamMod2) {
            xinputVeto(camMod2, &g_state->extCamNextPad);
        }
        g_state->extCamPrevPad = XinputBinding{};
        if (eliteBindsLookupPad("VanityCameraScrollLeft", fb, sizeof(fb)) &&
            xinputTranslate(fb, &g_state->extCamPrevPad) && haveCamMod2) {
            xinputVeto(camMod2, &g_state->extCamPrevPad);
        }
        g_state->fssEnterPad = XinputBinding{};
        if (eliteBindsLookupPad("ExplorationFSSEnter", fb, sizeof(fb))) {
            xinputTranslate(fb, &g_state->fssEnterPad);
        }
        g_state->fssQuitPad = XinputBinding{};
        if (eliteBindsLookupPad("ExplorationFSSQuit", fb, sizeof(fb))) {
            xinputTranslate(fb, &g_state->fssQuitPad);
        }
        headOffsetGateSetNextKeyBound(g_state->extCamNextKey.key() != 0);
        cameraViewSetPressWitness(g_state->extCamNextKey.key() != 0);
    }
    // Silence here cost a field session: the files changed, the re-read ran,
    // the answers matched -- and nothing said so, which is indistinguishable
    // from the mechanism being dead. The bindings: lines above name the file
    // each answer came from.
    if (!changed) {
        Log::get().note(
            "hotkey: your Elite bindings files changed, but both camera keys "
            "read the same as before.");
    }
}

// The Instruments page's rows: the same functions the diagnostic hotkeys
// fire, reachable from a headset with no key bound.
void menuActionDumpCamera(void*) { dumpCameraRing("the settings menu"); }
void menuActionCensus(void*) {
    drawCensusRequest();
    quadProbeRequest();
    perfMonitorNoteEvent(kEvCensus);
}
void menuActionResetView(void*) {
    headOffsetGateNewFootSession("the settings menu", /*journalSaysSo=*/false);
}
void menuActionMarker(void*) {
    static uint32_t n = 0;
    Log::get().note("----- marker %u, from the settings menu -----", ++n);
}

State& ensureState() {
    if (!g_state) {
        g_state = new State();
        g_state->toggleKey.setBinding(Config::get().getString("hotkey.toggle_exposure", "SCROLLLOCK").c_str());
        g_state->dumpKey.setBinding(Config::get().getString("hotkey.dump_camera", "PAUSE").c_str());
        // The settings menu, read here for install and on vScreen's reload
        // path for live changes; its Instruments page gets the diagnostic
        // keys' functions as rows.
        menuRegisterAction("Dump the camera history now",
                           "The PAUSE key's job: the last ten seconds of viewpoint history to the log.",
                           &menuActionDumpCamera, nullptr);
        menuRegisterAction("Take a draw census and quad probe",
                           "The dump_draws key's job: every draw into the eyes for a few frames.",
                           &menuActionCensus, nullptr);
        menuRegisterAction("Reset Explorer Cam's counted view to 0",
                           "For a keypress count that desynced: the manual twin of the wake reset.",
                           &menuActionResetView, nullptr);
        menuRegisterAction("Write a marker line to the graphics log",
                           "So a moment you noticed can be found in the log afterwards.",
                           &menuActionMarker, nullptr);
        menuConfigure(Config::get());
        // Empty default: the census is chased-bug instrumentation, and an
        // unbound key is how "off" is spelled for a hotkey.
        //
        // The bind is then SAID, because it failed silently once: dump_draws
        // was set to CTRL+SCROLLLOCK, which parsed and registered cleanly --
        // and the physical chord never arrived as Scroll Lock with Ctrl held
        // (on the classic keyboard matrix Ctrl+ScrollLock is Break, exactly
        // like Ctrl+Pause). Every path in EDVR stayed quiet: nothing matched,
        // so not even the missed-while-unfocused note had anything to say,
        // and the field session bought nothing. A diagnostic that can be
        // dead must say what it is watching, in the log it exists to write.
        {
            const std::string b = Config::get().getString("hotkey.dump_draws", "");
            g_state->censusKey.setBinding(b.c_str());
            if (g_state->censusKey.key() != 0) {
                Log::get().note(
                    "hotkey: draw census key bound: %s (vk 0x%02X, mods 0x%X). "
                    "Prefer a bare key here -- chords on the Pause/ScrollLock "
                    "cluster can reach Windows as a different key entirely.",
                    b.c_str(), g_state->censusKey.key(),
                    g_state->censusKey.mods());
            } else if (!b.empty()) {
                Log::get().note(
                    "hotkey: dump_draws is set but bound nothing (the line "
                    "above says why), so the draw census cannot be armed this "
                    "session.");
            }
        }
        // The camera keys come from the GAME's own key configuration, and
        // only from there (0.7.1 removed the ini overrides: two keys nobody
        // needed to set once adoption read the right element from the right
        // file). Non-keyboard bindings skip with a log line, and the keys
        // FOLLOW the game's files: rebind in Elite mid-session and the stat
        // cadence in the frame path picks it up within seconds.
        // These two mirror the GAME's own keys, so they are not filtered by
        // which window has focus -- Elite acts on them unfocused, and EDVR
        // disagreeing with the game is what a swallowed press costs. EDVR's
        // own keys above (the exposure toggle, the history dump) keep the
        // focus rule. See hotkey.h.
        g_state->externalCamKey.setGameMirrored(true);
        g_state->extCamNextKey.setGameMirrored(true);
        g_state->extCamPrevKey.setGameMirrored(true);
        g_state->fssEnterKey.setGameMirrored(true);
        g_state->fssQuitKey.setGameMirrored(true);
        if (Config::get().getBool("hotkey.read_game_bindings", true)) {
            char b[48];
            if (eliteBindsLookup("PhotoCameraToggle_Humanoid", b, sizeof(b),
                                 "PhotoCameraToggle")) {
                g_state->externalCamKey.setBinding(b);
                Log::get().note("hotkey: external_camera adopted from your "
                                "Elite bindings: %s", b);
            }
            if (eliteBindsLookup("VanityCameraScrollRight", b, sizeof(b))) {
                g_state->extCamNextKey.setBinding(b);
                Log::get().note("hotkey: external_camera_next adopted from "
                                "your Elite bindings: %s", b);
            }
            if (eliteBindsLookup("VanityCameraScrollLeft", b, sizeof(b))) {
                g_state->extCamPrevKey.setBinding(b);
                Log::get().note("hotkey: external_camera_prev adopted from "
                                "your Elite bindings: %s. Cycling backwards "
                                "now moves the counted view backwards too; "
                                "before this it moved the game and not the "
                                "count.", b);
            }
            if (eliteBindsLookup("ExplorationFSSEnter", b, sizeof(b))) {
                g_state->fssEnterKey.setBinding(b);
                Log::get().note("hotkey: the FSS enter key adopted from "
                                "your Elite bindings: %s", b);
            }
            if (eliteBindsLookup("ExplorationFSSQuit", b, sizeof(b))) {
                g_state->fssQuitKey.setBinding(b);
                Log::get().note("hotkey: the FSS quit key adopted from "
                                "your Elite bindings: %s", b);
            }
            if (eliteBindsLookup("ExplorationFSSMiniZoomIn", b,
                                 sizeof(b))) {
                g_state->fssZoomStepKey.setBinding(b);
                Log::get().note("hotkey: the FSS stepped-zoom key adopted "
                                "from your Elite bindings: %s", b);
            }
            if (eliteBindsLookup("ExplorationFSSZoomIn", b, sizeof(b))) {
                g_state->fssZoomKey.setBinding(b);
                Log::get().note("hotkey: the FSS zoom key adopted from "
                                "your Elite bindings: %s", b);
            }
            {
                char pk[40];
                // The camera toggle's gamepad chord, if it has one. Not
                // watched as a binding -- it is read only so the view-cycle
                // bindings know which presses are not theirs.
                char camMod[40] = {0};
                const bool haveCamMod =
                    eliteBindsLookupPadMod("PhotoCameraToggle_Humanoid",
                                           camMod, sizeof(camMod)) ||
                    eliteBindsLookupPadMod("PhotoCameraToggle", camMod,
                                           sizeof(camMod));
                g_state->extCamNextPad = XinputBinding{};
                if (eliteBindsLookupPad("VanityCameraScrollRight", pk,
                                        sizeof(pk)) &&
                    xinputTranslate(pk, &g_state->extCamNextPad)) {
                    const bool vetoed =
                        haveCamMod &&
                        xinputVeto(camMod, &g_state->extCamNextPad);
                    Log::get().note(
                        "hotkey: the next-view key is also on your gamepad: "
                        "%s.%s", pk,
                        vetoed ? " Your camera toggle chords on the same "
                                 "button, so a press with that modifier held "
                                 "is left to the toggle."
                               : "");
                }
                g_state->extCamPrevPad = XinputBinding{};
                if (eliteBindsLookupPad("VanityCameraScrollLeft", pk,
                                        sizeof(pk)) &&
                    xinputTranslate(pk, &g_state->extCamPrevPad)) {
                    if (haveCamMod) xinputVeto(camMod, &g_state->extCamPrevPad);
                    Log::get().note("hotkey: the previous-view key is also on "
                                    "your gamepad: %s.", pk);
                }
                g_state->fssEnterPad = XinputBinding{};
                if (eliteBindsLookupPad("ExplorationFSSEnter", pk,
                                        sizeof(pk)) &&
                    xinputTranslate(pk, &g_state->fssEnterPad)) {
                    Log::get().note("hotkey: the FSS enter key is also on "
                                    "your gamepad: %s.", pk);
                }
                g_state->fssQuitPad = XinputBinding{};
                if (eliteBindsLookupPad("ExplorationFSSQuit", pk,
                                        sizeof(pk)) &&
                    xinputTranslate(pk, &g_state->fssQuitPad)) {
                    Log::get().note("hotkey: the FSS quit key is also on "
                                    "your gamepad: %s.", pk);
                }
                g_state->fssZoomStepPad = XinputBinding{};
                if (eliteBindsLookupPad("ExplorationFSSMiniZoomIn", pk,
                                        sizeof(pk)) &&
                    xinputTranslate(pk, &g_state->fssZoomStepPad)) {
                    Log::get().note("hotkey: the FSS stepped zoom is also "
                                    "on your gamepad: %s.", pk);
                }
                g_state->fssZoomPad = XinputBinding{};
                if (eliteBindsLookupPad("ExplorationFSSZoomIn", pk,
                                        sizeof(pk)) &&
                    xinputTranslate(pk, &g_state->fssZoomPad)) {
                    Log::get().note("hotkey: the FSS zoom is also on your "
                                    "gamepad: %s.", pk);
                }
            }
            g_state->bindsFingerprint = eliteBindsFingerprint();
        }
        headOffsetGateSetNextKeyBound(g_state->extCamNextKey.key() != 0);
        cameraViewSetPressWitness(g_state->extCamNextKey.key() != 0);
        journalWatchConfigure();
        g_state->fssTheaterWanted =
            Config::get().getFloat("experimental.fss_theater", 0.0f) > 0.0f ||
            eyeSyncFromConfig(Config::get()).any();
        journalWatchSetEagerStatus(g_state->fssTheaterWanted);
        g_state->dumpOnExternalCam =
            Config::get().getBool("advanced.dump_camera_on_external_cam", false);
        g_state->holdFramesOnExternalCam = static_cast<uint32_t>(
            Config::get().getIntInRange("experimental.hold_frames_on_external_cam", 0, 0, 120));
        // A CONFIGURED key, not a pressed one. The gate refuses to arm without
        // this, so a fresh install with nothing bound is genuinely inert rather
        // than falling back to a heuristic that cannot tell the external camera
        // from the inside of your own ship.
        headOffsetGateSetKeyBound(g_state->externalCamKey.key() != 0);
        cameraViewConfigure();
    }
    return *g_state;
}

}  // namespace

// ELITE'S OWN VR RENDER-TARGET MULTIPLIER, for advanced.texture_lod_bias
// = auto.
//
// A mip bias is baked into a sampler when it is created and is immutable
// after, and this renderer makes its samplers up front (see the census
// below) -- before the eye targets exist, so before anything in this
// process can measure the render scale from a frame. Measuring it later
// would bias only the stragglers, which is a feature that does almost
// nothing and says nothing about it.
//
// So take it from the game's own settings, which are written before it
// starts: Options\Graphics\<preset>.<major>.<minor>.fxcfg carries
// <HMDRenderTargetMultiplier>, the fraction of the runtime's recommended
// size Elite renders at. The newest .fxcfg is the one it last wrote.
// SSAAMultiplier is read alongside only to be reported, so that if the
// bias ever disagrees with the render size in the log, the second number
// is already there to explain it.
//
// A format this file does not control, so every failure is silent-safe:
// no directory, no file, no field, or an unreasonable value all leave the
// bias at zero and say why.
bool eliteHmdMultiplier(float* mult, float* ssaa, char* fileOut, size_t fileLen) {
    if (mult) *mult = 0.0f;
    if (ssaa) *ssaa = 0.0f;
    if (fileOut && fileLen) fileOut[0] = 0;
    wchar_t appdata[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring dir = std::wstring(appdata) +
                       L"\\Frontier Developments\\Elite Dangerous\\Options\\Graphics\\";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"*.fxcfg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring best;
    FILETIME bestTime = {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            best = fd.cFileName;
            bestTime = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best.empty()) return false;
    HANDLE f = CreateFileW((dir + best).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    std::string text;
    if (size != INVALID_FILE_SIZE && size <= (1u << 20)) {
        text.resize(size);
        DWORD got = 0;
        if (!ReadFile(f, &text[0], size, &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(f);
    if (text.empty()) return false;
    if (fileOut && fileLen) {
        const int w = WideCharToMultiByte(CP_UTF8, 0, best.c_str(), -1, fileOut,
                                          static_cast<int>(fileLen), nullptr, nullptr);
        if (w <= 0) fileOut[0] = 0;
    }
    struct Field { const char* tag; float* out; };
    const Field fields[] = {{"<HMDRenderTargetMultiplier>", mult}, {"<SSAAMultiplier>", ssaa}};
    bool gotMult = false;
    for (const Field& fl : fields) {
        if (!fl.out) continue;
        const size_t at = text.find(fl.tag);
        if (at == std::string::npos) continue;
        const float v = static_cast<float>(atof(text.c_str() + at + strlen(fl.tag)));
        if (!(v > 0.0f) || !(v < 8.0f)) continue;
        *fl.out = v;
        if (fl.out == mult) gotMult = true;
    }
    return gotMult;
}

// The reduction type lives in bits 7-8 of a D3D11_FILTER: 0 standard,
// 1 comparison, 2 minimum, 3 maximum. Only a standard filter is ours to
// touch, and only a linear or anisotropic one -- promoting a point
// sampler would soften artwork that was asked for sharp.
bool samplerIsOurs(uint32_t filter) {
    if ((filter & 0x180u) != 0u) return false;
    return filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR ||
           filter == D3D11_FILTER_ANISOTROPIC;
}

const char* samplerFilterName(uint32_t f) {
    switch (f) {
        case D3D11_FILTER_MIN_MAG_MIP_POINT: return "point";
        case D3D11_FILTER_MIN_MAG_MIP_LINEAR: return "linear";
        case D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT: return "linear, point mips";
        case D3D11_FILTER_ANISOTROPIC: return "anisotropic";
        case D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR: return "linear, comparison";
        case D3D11_FILTER_COMPARISON_ANISOTROPIC: return "anisotropic, comparison";
        default: return "other";
    }
}

void samplerCensusNote(State& s) {
    char line[1100];
    size_t used = 0;
    for (uint32_t i = 0; i < s.samplerShapeCount && used < sizeof(line); ++i) {
        const State::SamplerShape& sh = s.samplerShapes[i];
        const int m = snprintf(line + used, sizeof(line) - used,
                               "%s%u x %s (0x%02X) aniso %u bias %+.2f", i ? "; " : "",
                               sh.count, samplerFilterName(sh.filter), sh.filter,
                               sh.aniso, static_cast<double>(sh.bias));
        if (m > 0) used += static_cast<size_t>(m);
    }
    Log::get().note(
        "texture filtering census: %u sampler(s) in %u shape(s)%s -- %s. A "
        "repeating pattern that shimmers on a distant surface is a mip chosen "
        "sharper than the pixel covers: low anisotropy or a negative bias is "
        "where that comes from, and neither the temporal pass nor NVIDIA's "
        "history can restore detail the game never sampled. "
        "advanced.texture_anisotropy and advanced.texture_lod_bias override "
        "them (0 = leave the game's own choice alone).",
        s.samplerCreates, s.samplerShapeCount,
        s.samplerOther ? " (and more past the table)" : "", line);
    s.samplerPrintedShapes = s.samplerShapeCount;
    ++s.samplerCensusPrints;
}

HRESULT STDMETHODCALLTYPE hookedCreateSamplerState(ID3D11Device* self,
                                                   const D3D11_SAMPLER_DESC* desc,
                                                   ID3D11SamplerState** out) {
    if (!g_state || !g_state->realCreateSamplerState) {
        return E_FAIL;
    }
    State& s = *g_state;
    if (!desc || self != s.device) return s.realCreateSamplerState(self, desc, out);
    D3D11_SAMPLER_DESC d = *desc;
    guardedBudget(g_createBudget, [&] {
        const uint32_t filter = static_cast<uint32_t>(desc->Filter);
        ++s.samplerCreates;
        if (!s.samplerFirstMs) s.samplerFirstMs = GetTickCount64();
        bool found = false;
        for (uint32_t i = 0; i < s.samplerShapeCount; ++i) {
            State::SamplerShape& sh = s.samplerShapes[i];
            if (sh.filter == filter && sh.aniso == desc->MaxAnisotropy &&
                sh.bias == desc->MipLODBias) {
                ++sh.count;
                found = true;
                break;
            }
        }
        if (!found) {
            const uint32_t n = sizeof(s.samplerShapes) / sizeof(s.samplerShapes[0]);
            if (s.samplerShapeCount < n) {
                State::SamplerShape& sh = s.samplerShapes[s.samplerShapeCount++];
                sh.filter = filter;
                sh.aniso = desc->MaxAnisotropy;
                sh.bias = desc->MipLODBias;
                sh.count = 1;
            } else {
                ++s.samplerOther;
            }
        }
        // The overrides. Off by default, so the census costs the game a
        // comparison and nothing else.
        if ((s.samplerAniso > 0 || s.samplerBias != 0.0f) && samplerIsOurs(filter)) {
            if (s.samplerAniso > 0) {
                d.Filter = D3D11_FILTER_ANISOTROPIC;
                d.MaxAnisotropy = static_cast<UINT>(s.samplerAniso);
            }
            d.MipLODBias = desc->MipLODBias + s.samplerBias;
            if (!s.samplerForceNoted) {
                s.samplerForceNoted = true;
                Log::get().note(
                    "texture filtering: the game's linear and anisotropic samplers are "
                    "being created with anisotropy %d and %+.2f added to their mip bias "
                    "(%s). Point, comparison, minimum and maximum filters are left alone. A "
                    "positive bias trades sharpness for quiet on distant repeating "
                    "detail; a negative one does the reverse.",
                    s.samplerAniso, static_cast<double>(s.samplerBias),
                    s.samplerBiasWhy[0] ? s.samplerBiasWhy : "no bias asked for");
            }
        }
        // Once the creates have settled: a renderer makes its samplers up
        // front, so ten seconds after the first is past the burst. Printed
        // again only if a shape appears that the first line did not carry.
        const bool settled = GetTickCount64() - s.samplerFirstMs > 10000;
        if (settled && (s.samplerCensusPrints == 0 ||
                        (s.samplerCensusPrints < 3 &&
                         s.samplerShapeCount > s.samplerPrintedShapes))) {
            samplerCensusNote(s);
        }
    });
    return s.realCreateSamplerState(self, &d, out);
}

void hookDevice(ID3D11Device* device) {
    if (!device) return;
    State& s = ensureState();
    if (s.device) {
        // SAID OUT LOUD, once per extra device (2026-08-24).
        //
        // Hooking only the first device is deliberate -- it is the one the game
        // renders through, and the vtable is patched per CLASS so a second
        // device's contexts reach the thunks anyway and are declined there. But
        // the decision was invisible: a session where the interesting rendering
        // happened on a LATER device looked exactly like a session where the
        // fixes simply found nothing, and there was no line anywhere to tell
        // the two apart. That cost a full session on the FSS ring split.
        //
        // Paired with vScreen's "declined" line: this says another device
        // exists, that one says whether anything is actually drawn on it.
        static uint32_t extra = 0;
        if (++extra <= 4) {
            D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
            Log::get().note(
                "a SECOND D3D11 device (%p, featureLevel=0x%04X) was created; EDVR "
                "hooks the first one only (%p) and this one is not hooked. Ordinary "
                "for a capability probe or another tool. If the game renders through "
                "it, every fix in d3d11.dll is blind to that rendering -- the "
                "\"vScreen declined\" line says whether it draws. Said at most 4 times.",
                (void*)device, static_cast<unsigned>(fl), (void*)s.device);
        }
        return;
    }

    Config& sentinelCfg = Config::get();

    // THE OFF SWITCH FOR THIS HALF, which until issue #21 did not exist.
    //
    // The sentinel disables these hooks for ONE launch after a crash, which is
    // right for a crash that happens once. It is the wrong shape entirely for a
    // rig where they crash EVERY launch: the game then alternates crash, play,
    // crash, play, and there is no setting anywhere that says "stop trying".
    // The reporter of #21 found the only lever the code left them -- making
    // edvr_logs\d3d11_hooks.armed read-only so the sentinel can never clear its
    // own trip -- and it worked, which is the part that should be embarrassing.
    // A user who has diagnosed their way to a workaround out of a file
    // permission was owed a documented setting three releases ago.
    //
    // What it leaves running is exactly what a tripped sentinel leaves running,
    // which is NOT nothing: the swapchain's Present hook and the DXGI factory
    // hooks are installed from hookSwapChain and hookFactoryForDevice, which
    // the proxy calls independently of this function, and the whole openvr half
    // never needed this device at all. The frame boundary the VR half's fixes
    // ride on is one of those, so the switch has to leave them.
    //
    // The order below is load-bearing. This check sits AFTER the Sentinel is
    // constructed, so that a .armed file left behind by an earlier crashing
    // session is still cleared: putting it above meant the first launch after a
    // user set the switch back to 1 was eaten by a stale SENTINEL TRIPPED, and
    // the log said "no hooks are installed" while never touching the file that
    // proves otherwise.
    if (!s.sentinel) {
        s.sentinel = new Sentinel(sentinelCfg.logDir().c_str(), L"d3d11_hooks");
    }
    if (!sentinelCfg.getBool("advanced.d3d11_fixes", true)) {
        if (s.sentinel->trippedOnStartup()) s.sentinel->clearTrip();
        // Said once. hookDevice runs per device, and Elite creates more than
        // one; the paragraph is for the reader, not for every device.
        if (!s.fixesOffNoted) {
            s.fixesOffNoted = true;
            Log::get().note(
                "d3d11 fixes are OFF by request (advanced.d3d11_fixes = 0), so no "
                "hooks are installed on the device or on its context, this session "
                "or any other: the black void, the panel distance, the exposure "
                "share, the transition flash detector, the anti-aliasing passes, "
                "the shader replacements and Explorer Cam's half of the gate are "
                "all inert, and the game renders as it would without this half "
                "installed. What is still hooked is the swapchain's Present and "
                "the DXGI factory, which carry the frame boundary the openvr half "
                "runs on -- so if a crash survives this setting, it is in one of "
                "those or in the openvr half, and that is worth reporting. Set it "
                "back to 1 to try the fixes again.");
        }
        return;
    }
    if (s.sentinel->trippedOnStartup() &&
        !sentinelCfg.getBool("advanced.ignore_sentinel", false)) {
        // Cleared here, so a false trip costs one session rather than every
        // future launch -- the same bargain the compositor hook struck, and for
        // the same reason: quitting from the menu before the confirmation looks
        // identical to crashing from in here.
        s.sentinel->clearTrip();
        Log::get().note(
            "SENTINEL TRIPPED: the previous run installed the d3d11 hooks and never "
            "confirmed them, which usually means it crashed -- though a session that "
            "ended in the first few seconds looks the same from here. EVERY fix in "
            "d3d11.dll is off for THIS session only, and it will try again next "
            "launch: the black void, the panel distance, the exposure share, the "
            "transition flash detector and Explorer Cam's half of the gate. The game "
            "renders exactly as it would without EDVR installed.\n"
            "  If this keeps happening, the hooks really are crashing and the log is "
            "worth reporting. To force them on anyway, set ignore_sentinel = 1 under "
            "[advanced].");
        return;
    }

    if (!s.deviceHook.attach(device) ||
        s.deviceHook.executablePrefix() <= kDevCreateComputeShader) {
        Log::get().note("device vtable unusable; fix not installed");
        s.deviceHook.uninstall();
        return;
    }

    // Armed before the first vtable WRITE, not before the attach: attaching only
    // reads the table and copies it, and a session that dies there did not die of
    // anything we changed.
    if (!s.sentinel->arm()) {
        Log::get().note("NOTE: the crash sentinel could not be written, so a crash in "
                        "these hooks will not disable them next launch.");
    }
    breadcrumb("gfx: arming d3d11 hooks");

    s.shaderDump = sentinelCfg.getBool("advanced.glare_shader_dump", false);
    s.shaderDumpDir = sentinelCfg.logDir() + L"\\shaders";
    if (s.shaderDump) {
        Log::get().note("shader dump ARMED: every vertex and pixel shader "
                        "the game creates is written to edvr_logs\\shaders "
                        "by hash. Park at a star with sun_glare_steady on; "
                        "the log names the glare train's pair. Set "
                        "glare_shader_dump = 0 afterwards -- this costs "
                        "file writes during loading.");
    }
    s.deviceHook.replace(kDevCreateVertexShader, &hookedCreateVS,
                         reinterpret_cast<void**>(&s.realCreateVS));
    s.deviceHook.replace(kDevCreatePixelShader, &hookedCreatePS,
                         reinterpret_cast<void**>(&s.realCreatePS));
    s.deviceHook.replace(kDevCreateComputeShader, &hookedCreateCS,
                         reinterpret_cast<void**>(&s.realCreateCS));
    s.deviceHook.replace(kDevCreateTexture2D, &hookedCreateTexture2D,
                         reinterpret_cast<void**>(&s.realCreateTexture2D));
    // The six remaining creates that can fail, hooked to REPORT and nothing
    // else -- see kDevCreateBuffer. The prefix check above already covers
    // every slot here: it demands more than kDevCreateComputeShader, which is
    // 18, and the highest of these is 10.
#define EDVR_HOOK_DEV_CREATE(slot, firstIsResource)                          \
    s.deviceHook.replace(slot, &hookedDevCreate<slot, firstIsResource>,      \
                         reinterpret_cast<void**>(&s.realDevCreate[slot]))
    EDVR_HOOK_DEV_CREATE(kDevCreateBuffer, false);
    EDVR_HOOK_DEV_CREATE(kDevCreateTexture1D, false);
    EDVR_HOOK_DEV_CREATE(kDevCreateTexture3D, false);
    EDVR_HOOK_DEV_CREATE(kDevCreateSrv, true);
    EDVR_HOOK_DEV_CREATE(kDevCreateUav, true);
    EDVR_HOOK_DEV_CREATE(kDevCreateRtv, true);
    EDVR_HOOK_DEV_CREATE(kDevCreateDsv, true);
#undef EDVR_HOOK_DEV_CREATE
    {
        const int aniso = sentinelCfg.getIntInRange("advanced.texture_anisotropy", 0, 0, 16);
        const std::string biasKey = sentinelCfg.getString("advanced.texture_lod_bias", "0");
        float bias = 0.0f;
        if (_stricmp(biasKey.c_str(), "auto") == 0) {
            float mult = 0.0f, ssaa = 0.0f;
            char file[80] = {};
            if (eliteHmdMultiplier(&mult, &ssaa, file, sizeof(file))) {
                bias = log2f(mult);
                s.samplerBiasAuto = true;
                s.samplerBiasMult = mult;
                snprintf(s.samplerBiasWhy, sizeof(s.samplerBiasWhy),
                         "auto: Elite renders at %.3f of the size the runtime recommends "
                         "(HMDRenderTargetMultiplier in %s; its SSAAMultiplier is %.3f), and "
                         "log2 of that is the bias the mips want",
                         static_cast<double>(mult), file[0] ? file : "its graphics preset",
                         static_cast<double>(ssaa));
            } else {
                // Say it here rather than leave it to the sampler line: a
                // failed auto leaves the bias at zero, which switches the
                // whole override off, which would print nothing at all.
                Log::get().note(
                    "texture filtering: advanced.texture_lod_bias = auto, but Elite's graphics "
                    "preset gave no HMDRenderTargetMultiplier (Options\\Graphics\\*.fxcfg under "
                    "%%LOCALAPPDATA%%\\Frontier Developments\\Elite Dangerous). No bias is applied. "
                    "Set a number instead, or check the game has written its settings once.");
            }
        } else {
            bias = static_cast<float>(atof(biasKey.c_str()));
            snprintf(s.samplerBiasWhy, sizeof(s.samplerBiasWhy),
                     "advanced.texture_lod_bias = %s", biasKey.c_str());
        }
        if (!(bias > -4.0f)) bias = -4.0f;
        if (!(bias < 4.0f)) bias = 4.0f;
        s.samplerAniso = aniso;
        s.samplerBias = bias;
    }
    s.deviceHook.replace(kDevCreateSamplerState, &hookedCreateSamplerState,
                         reinterpret_cast<void**>(&s.realCreateSamplerState));
    // Before the first create can arrive: the FSS resolution fix's flag is
    // read here for install and on vScreen's reload path for live flips.
    fssResConfigure(sentinelCfg);
    if (!s.deviceHook.commit()) {
        s.deviceHook.uninstall();
        // Nothing was patched, so there is nothing to be protecting against.
        // Leaving it armed would trip on the next launch over an install that
        // never happened.
        s.sentinel->confirm();
        return;
    }
    s.device = device;

    // The hook mechanism, decided ONCE from the immediate context and shared
    // by both context installers so they cannot split modes on the one object
    // (see device_hook.h). GetImmediateContext returns the same context each
    // time, so this is that context; released right after, identity only.
    HookMode ctxMode = HookMode::InPlace;
    {
        ID3D11DeviceContext* ctx = nullptr;
        device->GetImmediateContext(&ctx);
        if (ctx) {
            ctxMode = contextHookModeFor(ctx);

            // The multithread-protection probe, opened here because this is
            // where the immediate context is already in hand. Asked of the
            // CONTEXT first and the device second: the flag the disassembly
            // found lives in the context object, and the two are documented
            // inconsistently enough that trying both costs less than being
            // sure. Absent on neither, in practice -- and if it is, the log
            // says so once and nothing else changes.
            if (FAILED(ctx->QueryInterface(__uuidof(ID3D11Multithread),
                                           reinterpret_cast<void**>(&s.multithread)))) {
                s.multithread = nullptr;
                device->QueryInterface(__uuidof(ID3D11Multithread),
                                       reinterpret_cast<void**>(&s.multithread));
            }
            if (s.multithread) {
                s.mtProtected = s.multithread->GetMultithreadProtected() ? 1 : 0;
                Log::get().note(
                    "multithread protection is %s at install. It decides which "
                    "of two complete sets of context methods this device "
                    "dispatches through -- the protected set takes the "
                    "runtime's lock in every call -- and switching it makes "
                    "Windows rewrite the context's whole function table. EDVR "
                    "only reads it; every change is reported below. If it never "
                    "changes and the table is still being rewritten, the cause "
                    "is something else and that is worth knowing.",
                    s.mtProtected ? "ON" : "off");
            } else {
                Log::get().note(
                    "multithread protection could not be queried on this device "
                    "(no ID3D11Multithread), so this session cannot say whether "
                    "it changes. Nothing else is affected.");
            }
            ctx->Release();
        }
    }

    // THE SWAP-ONLY PROBE, which stands in for both context installers.
    //
    // Two users' machines die 1.7 seconds after the private-copy mode goes in.
    // The in-place mode is stable on both -- and on both, the runtime
    // overwrites every in-place thunk before the first frame, so in that mode
    // EDVR's thunks never run at all. That is the one clean difference between
    // the mode that dies and the mode that lives: whether our code executes on
    // the context's calls. And it leaves exactly two suspects with nothing in
    // between: the vptr swap itself, or what twenty-nine thunks DO once they are
    // running.
    //
    // This removes the second. The context gets a private copy of its table
    // with nothing patched -- every call runs the code it always ran, from a
    // different address -- and neither installer runs, so no thunk exists to
    // execute. If the game still dies, the swap is fatal on its own and the
    // thunks were never the problem. If it lives, the mechanism is innocent,
    // the bug is in what a thunk does, and that is a bisection rather than a
    // new hooking design.
    //
    // A pass-through thunk was considered and is not built, because from the
    // GPU's side it is the swap plus one extra call frame -- it would answer
    // the same question as this, less cleanly.
    // AND ITS TWIN, which takes the other half away instead.
    //
    // The swap-only probe removes the thunks and keeps two things: the vptr is
    // RELOCATED and the table is FROZEN. If it crashes, that is still ambiguous
    // -- "relocating the vptr is fatal" and "freezing the table is fatal" are
    // different bugs with different fixes and the one probe cannot separate
    // them. The LIVE-ONLY probe keeps the relocation and removes the freeze:
    // every entry of the private table is a stub that reads the context's own
    // slot at the moment of the call, so no call can ever reach an
    // implementation the runtime has moved on from, and still no EDVR code runs
    // inside any context method. Run the two and the answer is arithmetic.
    {
        const std::string probe =
            sentinelCfg.getString("advanced.context_hook_probe", "off");
        const bool wantSwap = _stricmp(probe.c_str(), "swap") == 0;
        const bool wantLive = _stricmp(probe.c_str(), "live") == 0;
        if (wantSwap || wantLive) {
            ID3D11DeviceContext* ctx = nullptr;
            device->GetImmediateContext(&ctx);
            bool swapped = false;
            if (ctx) {
                if (s.bareContextHook.attach(ctx) &&
                    s.bareContextHook.setMode(wantLive ? HookMode::LiveCopy
                                                       : HookMode::CopyVptr) &&
                    (wantLive ? s.bareContextHook.commitLive()
                              : s.bareContextHook.commitUnpatched())) {
                    swapped = true;
                }
                ctx->Release();
            }
            if (wantLive) {
                Log::get().note(
                    swapped
                        ? "LIVE-ONLY PROBE: the immediate context now dispatches "
                          "through a private table of EDVR's in which every entry "
                          "is a jump stub that reads the context's OWN slot at "
                          "the moment of the call. Nothing is patched, neither "
                          "context installer ran, and no EDVR code is in the path "
                          "of any context call -- so every d3d11 fix is off this "
                          "session. The only thing not stock is WHERE the vptr "
                          "points. If this dies the way the private-copy mode "
                          "does, relocating the vptr is fatal on its own and "
                          "following the runtime perfectly does not help; if it "
                          "lives while the swap-only probe dies, the frozen table "
                          "was the cause. Set advanced.context_hook_probe back to "
                          "off afterwards."
                        : "LIVE-ONLY PROBE asked for, but the live table could "
                          "not be installed (see above). The context installers "
                          "were skipped anyway, so this session tests nothing -- "
                          "set advanced.context_hook_probe back to off.");
            } else {
                Log::get().note(
                    swapped
                        ? "SWAP-ONLY PROBE: the immediate context now dispatches "
                          "through a byte-identical private copy of its table, with "
                          "NOTHING patched in it and neither context installer run. "
                          "No EDVR code is in the path of any context call. Every "
                          "d3d11 fix is therefore off this session. If this session "
                          "dies the way the normal private-copy mode does, the vptr "
                          "swap is fatal on its own; if it survives, the swap is "
                          "innocent and the cause is inside a thunk. Set "
                          "advanced.context_hook_probe back to off afterwards."
                        : "SWAP-ONLY PROBE asked for, but the bare copy could not be "
                          "installed (see above). The context installers were "
                          "skipped anyway, so this session tests nothing -- set "
                          "advanced.context_hook_probe back to off.");
            }
            if (swapped) {
                // No installer ran, so the bare probe hook is the only thing
                // holding the context's own table -- and in a probe session it
                // is the table the runtime writes, which is what the timeline
                // has to watch.
                armFlipTimeline(sentinelCfg, s.bareContextHook.originalVTable(),
                                s.bareContextHook.executablePrefix(),
                                "probe context");
            }
        } else {
            if (!probe.empty() && _stricmp(probe.c_str(), "off") != 0) {
                Log::get().note(
                    "edvr.ini: advanced.context_hook_probe = \"%s\" is not one of "
                    "off, swap or live, so it was IGNORED. Check the spelling.",
                    probe.c_str());
            }
            installExposureFix(device, ctxMode);
            // Before the vScreen fixes, which ask it whether it needs the
            // eye-draw count. It installs no hooks of its own -- it is driven
            // from vScreen's Map and Unmap -- so nothing else depends on the
            // order.
            installGlitchFrameFix();
            installVScreenFixes(device, ctxMode);

            // AFTER BOTH INSTALLERS, and the order is the whole point. EDVR's
            // own commit writes two dozen entries of this table in the shared
            // mode; arming before that would spend the timeline's first two
            // dozen lines on EDVR patching itself in, which is the one set of
            // changes nobody needs a timeline to know about.
            //
            // The exposure hook's table and not vScreen's: it installs first,
            // so its is the runtime's own in every mode. See
            // exposureFixContextTable.
            size_t span = 0;
            void** table = exposureFixContextTable(&span);
            armFlipTimeline(sentinelCfg, table, span, "context");
        }
    }

    // The panel resolution, if asked for. Applied here because it has to land
    // before the game builds its render chain, and the device exists first.
    //
    // Unlike everything else in this DLL, this writes to the game's code. It
    // identifies what it edits by shape rather than by build, refuses if what it
    // finds does not look right, and undoes itself on unload. Asking for the
    // stock resolution -- which is what the shipped ini does -- is a no-op it
    // takes before scanning anything.
    //
    // It is NOT part of the toggle hotkey, and cannot be: it changes what size
    // the game ALLOCATES, so images already made keep the size they were made
    // at. Switching it mid-session would leave a mix of both, which renders
    // worse than either. Comparing it means changing the value and restarting.
    {
        Config& cfg = Config::get();
        const uint32_t w = static_cast<uint32_t>(cfg.getInt("fix.vscreen_res_width", 0));
        const uint32_t h = static_cast<uint32_t>(cfg.getInt("fix.vscreen_res_height", 0));
        // Elite's own on-foot panel size, and what the panel still renders at if
        // the patch is not asked for or refuses.
        const uint32_t kStockW = 1920, kStockH = 1080;

        const bool applied = (w && h) && applyVScreenModeResolution(w, h);

        // Tell vScreen what the panel ACTUALLY renders at, from the outcome
        // rather than the request. This return value used to be discarded, and
        // vScreen took the requested size from config behind a >= 2048 test of
        // its own -- so a refused patch left it recognising a panel that was
        // never created, and an applied 2560x1440 left it recognising the stock
        // size. Either way the panel distance fix silently stopped matching.
        vScreenSetPanelSize(applied ? w : kStockW, applied ? h : kStockH);
    }
    hookFactoryForDevice(device);
}

void hookSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain) return;
    State& s = ensureState();
    if (s.swapChain) return;

    if (!s.swapChainHook.attach(swapChain) ||
        s.swapChainHook.executablePrefix() <= kSwapPresent) {
        s.swapChainHook.uninstall();
        return;
    }
    s.swapChainHook.replace(kSwapPresent, &hookedPresent,
                            reinterpret_cast<void**>(&s.realPresent));
    if (!s.swapChainHook.commit()) {
        s.swapChainHook.uninstall();
        return;
    }
    s.swapChain = swapChain;
    Log::get().note("Present hook installed");
}

void hookFactoryForDevice(ID3D11Device* device) {
    if (!device) return;
    State& s = ensureState();
    if (s.factoryHook.attached()) return;

    IDXGIDevice*  dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;

    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(&dxgiDevice))) ||
        !dxgiDevice) {
        return;
    }
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
        adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    }
    if (adapter) adapter->Release();
    dxgiDevice->Release();
    if (!factory) return;

    IDXGIFactory2* factory2 = nullptr;
    factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2));
    const size_t needed =
        factory2 ? kFactory2CreateSwapChainForHwnd : kFactoryCreateSwapChain;

    s.factory = factory;
    if (s.factoryHook.attach(factory) && s.factoryHook.executablePrefix() > needed) {
        s.factoryHook.replace(kFactoryCreateSwapChain, &hookedCreateSwapChain,
                              reinterpret_cast<void**>(&s.realCreateSwapChain));
        if (factory2) {
            s.factoryHook.replace(kFactory2CreateSwapChainForHwnd,
                                  &hookedCreateSwapChainForHwnd,
                                  reinterpret_cast<void**>(&s.realCreateSwapChainForHwnd));
        }
        if (!s.factoryHook.commit()) s.factoryHook.uninstall();
    } else {
        s.factoryHook.uninstall();
    }

    if (factory2) factory2->Release();
    factory->Release();
}

bool deviceHookFssModeLatch() {
    return g_state != nullptr && g_state->fssModeLatch;
}

bool deviceHookTakeFssZoomPress() {
    if (!g_state || !g_state->fssZoomPressPending) return false;
    g_state->fssZoomPressPending = false;
    return true;
}

bool deviceHookAutoBiasSource(float* multiplier, float* bias) {
    State& s = ensureState();
    if (!s.samplerBiasAuto) return false;
    if (multiplier) *multiplier = s.samplerBiasMult;
    if (bias) *bias = s.samplerBias;
    return true;
}

void deviceHookNoteCleanExit() {
    // REACHING THIS IS THE PROOF, and it is the only proof there is.
    //
    // An orderly exit is not a crash, whether or not the confirm window had
    // elapsed. Without this a session that ends cleanly inside the first six
    // seconds arms the next launch's refusal, and the player -- who did
    // nothing wrong and saw no crash -- gets a run with every d3d11 fix
    // switched off. Measured in the field on 2026-08-17: a 5-second session at
    // 07:27 that installed everything, reached LoadGame and published the eye
    // size, then a 09:53 launch that reported SENTINEL TRIPPED and rendered a
    // grey void because the black-void fix never ran.
    //
    // This reasoning was already written, and already correct, on
    // shutdownDeviceHooks -- which runs only from FreeLibrary. A game closing
    // is process termination, a fact this codebase has recorded twice before
    // (the totals lines that never printed, 6-guard). So the fix existed on the
    // one path that never executes, which is worse than not existing: it reads
    // as handled.
    //
    // WHY THIS DOES NOT EXCUSE A REAL CRASH. An unhandled access violation does
    // not come here. The default handler terminates the process, and
    // TerminateProcess delivers no DLL_PROCESS_DETACH -- so the hook crash this
    // sentinel exists to catch still leaves the file behind, exactly as before.
    // What changes is that a normal quit no longer looks the same as one.
    //
    // Safe on the termination path: one DeleteFileW, no allocation, no lock,
    // no loader work. The branch that calls it already writes a breadcrumb.
    if (!g_state) return;
    if (g_state->sentinel && !g_state->sentinelConfirmed) {
        g_state->sentinelConfirmed = true;
        g_state->sentinel->confirm();
    }
}

void shutdownDeviceHooks() {
    // The keyboard first: a gate left set past the module's life is a
    // keyboard the game never gets back.
    menuShutdown();
    journalWatchShutdown();
    // The probe's reference on the device. Read-only for its whole life, so
    // there is nothing to put back -- only the reference to let go.
    if (g_state && g_state->multithread) {
        g_state->multithread->Release();
        g_state->multithread = nullptr;
    }
    // The write watch, before anything that could free the page it is holding
    // read-only.
    //
    // shutdownVScreenFixes does this too, and until the flip timeline that was
    // enough, because the only thing that armed a watch was vScreen's own
    // install. The timeline arms in the two context probes as well, where
    // vScreen never installs and its shutdown returns immediately -- so without
    // this, a probe session's page would be left read-only after EDVR had gone,
    // which is not a thing to do to a process. Idempotent: the second call sees
    // no armed watch and returns.
    vtableWatchStop();
    // Reverse of install order: vScreen's vtable copy was taken on top of the
    // exposure fix's, so it comes off first.
    revertVScreenModeResolution();
    shutdownGlitchFrameFix();
    shutdownVScreenFixes();
    shutdownExposureFix();
    // The swap-only or live-only probe's bare table, if that was what ran
    // instead of the two installers above. Restoring the vptr frees nothing
    // anyone dispatches through: the table held no thunks, so nothing chained
    // into it. (The live mode's stub page is leaked rather than freed, by
    // VTableHook, for a reason that has nothing to do with chaining -- a thread
    // can still be inside a stub. See m_stubs.)
    if (g_state) g_state->bareContextHook.uninstall();
    if (!g_state) return;
    deviceHookNoteCleanExit();
    g_state->factoryHook.uninstall();
    g_state->swapChainHook.uninstall();
    g_state->deviceHook.uninstall();
}

}  // namespace edvr

