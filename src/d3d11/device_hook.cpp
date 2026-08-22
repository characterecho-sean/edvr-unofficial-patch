#include "device_hook.h"

#include <windows.h>

#include <dxgi1_2.h>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/hotkey.h"
#include "head_offset_gate.h"
#include "camera_view.h"
#include "journal_watch.h"
#include "elite_binds.h"
#include "../common/log.h"
#include "../common/proxy.h"
#include "../common/timing.h"
#include "../common/vtable_hook.h"
#include "binding_shadow.h"
#include "draw_census.h"
#include "exposure_fix.h"
#include "vscreen.h"
#include "glitch_frame.h"
#include "vscreen_res.h"

namespace edvr {
namespace {

// Frozen COM ABI. IUnknown occupies 0-2; the interface methods follow in
// declaration order. Each index is still range-checked before use.
constexpr size_t kDevCreateVertexShader  = 12;
constexpr size_t kDevCreatePixelShader   = 15;
constexpr size_t kDevCreateComputeShader = 18;
constexpr size_t kSwapPresent            = 8;
constexpr size_t kFactoryCreateSwapChain = 10;
constexpr size_t kFactory2CreateSwapChainForHwnd = 15;

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateShader)(ID3D11Device*, const void*, SIZE_T,
                                                     ID3D11ClassLinkage*, void**);
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
    uint64_t firstFrameMs = 0;   // for the crash sentinel's confirm window

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
constexpr uint64_t kSentinelConfirmMs = 6000;

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
        registerShaderHash(*out, fnv1a64(bytecode, len));
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
    const HRESULT hr = g_state->realPresent(self, syncInterval, flags);

    // OUTSIDE the fault budget, and that is the point. Confirming is a file
    // delete; putting it inside would mean a burst of faults anywhere in the
    // frame work stops the confirmation, the sentinel trips on the next launch,
    // and every fix switches itself off over something that never crashed.
    ++g_state->framesSeen;
    if (g_state->firstFrameMs == 0) g_state->firstFrameMs = stampMs();
    if (!g_state->sentinelConfirmed &&
        elapsedMs(g_state->firstFrameMs, kSentinelConfirmMs)) {
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
            Config::get().getIntInRange("advanced.hold_frames_on_external_cam", 0, 0, 120));

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
        if (g_state->censusKey.pressed()) drawCensusRequest();
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
        // Polled rather than watched, twice a second by the journal watcher
        // and once a second here. The user is wearing a
        // headset and cannot see a text editor, so the settings that are worth
        // tuning by feel have to take effect without a restart. Was every 90
        // frames, which is once a second on exactly one of the three rates.
        ++g_state->frameCounter;
        if (dueMs(g_state->configPollMs, kConfigPollMs)) {
            g_state->configPollMs = stampMs();
            vScreenRefreshConfig();
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

State& ensureState() {
    if (!g_state) {
        g_state = new State();
        g_state->toggleKey.setBinding(Config::get().getString("hotkey.toggle_exposure", "SCROLLLOCK").c_str());
        g_state->dumpKey.setBinding(Config::get().getString("hotkey.dump_camera", "PAUSE").c_str());
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
            g_state->bindsFingerprint = eliteBindsFingerprint();
        }
        headOffsetGateSetNextKeyBound(g_state->extCamNextKey.key() != 0);
        cameraViewSetPressWitness(g_state->extCamNextKey.key() != 0);
        journalWatchConfigure();
        g_state->dumpOnExternalCam =
            Config::get().getBool("advanced.dump_camera_on_external_cam", false);
        g_state->holdFramesOnExternalCam = static_cast<uint32_t>(
            Config::get().getIntInRange("advanced.hold_frames_on_external_cam", 0, 0, 120));
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

void hookDevice(ID3D11Device* device) {
    if (!device) return;
    State& s = ensureState();
    if (s.device) return;

    Config& sentinelCfg = Config::get();
    if (!s.sentinel) {
        s.sentinel = new Sentinel(sentinelCfg.logDir().c_str(), L"d3d11_hooks");
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
            ctx->Release();
        }
    }

    installExposureFix(device, ctxMode);
    // Before the vScreen fixes, which ask it whether it needs the eye-draw
    // count. It installs no hooks of its own -- it is driven from vScreen's Map
    // and Unmap -- so nothing else depends on the order.
    installGlitchFrameFix();
    installVScreenFixes(device, ctxMode);

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
    journalWatchShutdown();
    // Reverse of install order: vScreen's vtable copy was taken on top of the
    // exposure fix's, so it comes off first.
    revertVScreenModeResolution();
    shutdownGlitchFrameFix();
    shutdownVScreenFixes();
    shutdownExposureFix();
    if (!g_state) return;
    deviceHookNoteCleanExit();
    g_state->factoryHook.uninstall();
    g_state->swapChainHook.uninstall();
    g_state->deviceHook.uninstall();
}

}  // namespace edvr

