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
#include "../common/vtable_hook.h"
#include "binding_shadow.h"
#include "exposure_fix.h"
#include "vscreen.h"
#include "glitch_frame.h"
#include "vscreen_res.h"

namespace edvr {
namespace {

// Frozen COM ABI. IUnknown occupies 0-2; the interface methods follow in
// declaration order. Each index is still range-checked before use.
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

    PFN_CreateShader realCreateCS = nullptr;
    PFN_Present      realPresent = nullptr;
    PFN_CreateSwapChain        realCreateSwapChain = nullptr;
    PFN_CreateSwapChainForHwnd realCreateSwapChainForHwnd = nullptr;

    Hotkey toggleKey;
    Hotkey dumpKey;
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
    uint32_t lastJournalDisembarks = 0;
    uint32_t lastJournalEmbarks = 0;
    uint32_t lastCameraEnters = 0;
    uint64_t frameCounter = 0;

    // Dump the camera history on every external-camera keypress. Diagnostic,
    // off by default: 900 lines a press.
    bool     dumpOnExternalCam = false;
    uint32_t dumpCountdown = 0;
    uint32_t missedDumpNotes = 0;
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

// Presented frames before the hooks are treated as having survived install.
//
// About six seconds of play, which is past the loading screen and into a drawn
// scene. Long enough that the risky part -- the first frames through four
// patched vtables -- is behind us, short enough that a player who quits normally
// has confirmed long before, because a false trip costs them every fix for a
// session and that is the cost this must not impose casually.
constexpr uint32_t kSentinelConfirmFrames = 600;

// Frames to wait after an external-camera keypress before dumping the history.
//
// About two seconds, which is comfortably past the mode change: the panel-to-
// scene delay alone has been measured at 2 to 86 frames, and the flash being
// chased is on the transition itself. The ring is 900 frames, so this still
// leaves eight seconds of ordinary flight in front of the event to compare
// against.
constexpr uint32_t kDumpDelayFrames = 180;

// How many times a session to point out that a history-key press was ignored
// because another window had focus. Three is enough to be noticed and few
// enough that a player who works with a browser focused is not papered with it.
constexpr uint32_t kMissedDumpNotes = 3;

State* g_state = nullptr;
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

HRESULT STDMETHODCALLTYPE hookedCreateCS(ID3D11Device* self, const void* bytecode,
                                         SIZE_T len, ID3D11ClassLinkage* linkage,
                                         void** out) {
    const HRESULT hr = g_state->realCreateCS(self, bytecode, len, linkage, out);
    guardedBudget(g_createBudget, [&] {
        if (FAILED(hr) || !bytecode || len == 0 || !out || !*out) return;
        registerShaderHash(*out, fnv1a64(bytecode, len));
    });
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* self, UINT syncInterval,
                                        UINT flags) {
    const HRESULT hr = g_state->realPresent(self, syncInterval, flags);

    // OUTSIDE the fault budget, and that is the point. Confirming is a file
    // delete; putting it inside would mean a burst of faults anywhere in the
    // frame work stops the confirmation, the sentinel trips on the next launch,
    // and every fix switches itself off over something that never crashed.
    if (!g_state->sentinelConfirmed &&
        ++g_state->framesSeen >= kSentinelConfirmFrames) {
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
            g_state->dumpCountdown = kDumpDelayFrames;
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
        if (g_state->dumpCountdown > 0 && --g_state->dumpCountdown == 0) {
            dumpCameraRing("a key you pressed two seconds ago", kDumpDelayFrames);
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
            if (g_state->dumpOnExternalCam) g_state->dumpCountdown = kDumpDelayFrames;
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
        // Polled rather than watched, about once a second at 90Hz. The user is
        // wearing a headset and cannot see a text editor, so the settings that
        // are worth tuning by feel have to take effect without a restart.
        if ((++g_state->frameCounter % 90) == 0) vScreenRefreshConfig();
    });
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                DXGI_SWAP_CHAIN_DESC* desc,
                                                IDXGISwapChain** out) {
    const HRESULT hr = g_state->realCreateSwapChain(self, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) hookSwapChain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrictTo,
    IDXGISwapChain1** out) {
    const HRESULT hr =
        g_state->realCreateSwapChainForHwnd(self, device, hwnd, desc, fs, restrictTo, out);
    if (SUCCEEDED(hr) && out && *out) hookSwapChain(*out);
    return hr;
}

State& ensureState() {
    if (!g_state) {
        g_state = new State();
        g_state->toggleKey.setBinding(Config::get().getString("hotkey.toggle_exposure", "SCROLLLOCK").c_str());
        g_state->dumpKey.setBinding(Config::get().getString("hotkey.dump_camera", "PAUSE").c_str());
        g_state->externalCamKey.setBinding(Config::get().getString("hotkey.external_camera", "").c_str());
        g_state->extCamNextKey.setBinding(
            Config::get().getString("hotkey.external_camera_next", "").c_str());
        // Bindings the ini leaves empty are read from the GAME's own key
        // configuration (Options\Bindings), so a keyboard player needs no
        // setup at all. An explicit ini value always wins; a binding on a
        // controller is skipped (EDVR watches the keyboard) and the keyless
        // Status-driven detection covers that player instead.
        if (Config::get().getBool("hotkey.read_game_bindings", true)) {
            char b[48];
            if (g_state->externalCamKey.key() == 0 &&
                eliteBindsLookup("PhotoCameraToggle", b, sizeof(b))) {
                g_state->externalCamKey.setBinding(b);
                Log::get().note("hotkey: external_camera adopted from your "
                                "Elite bindings: %s", b);
            }
            if (g_state->extCamNextKey.key() == 0 &&
                eliteBindsLookup("VanityCameraScrollRight", b, sizeof(b))) {
                g_state->extCamNextKey.setBinding(b);
                Log::get().note("hotkey: external_camera_next adopted from "
                                "your Elite bindings: %s", b);
            }
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

    installExposureFix(device);
    // Before the vScreen fixes, which ask it whether it needs the eye-draw
    // count. It installs no hooks of its own -- it is driven from vScreen's Map
    // and Unmap -- so nothing else depends on the order.
    installGlitchFrameFix();
    installVScreenFixes(device);

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

void shutdownDeviceHooks() {
    journalWatchShutdown();
    // Reverse of install order: vScreen's vtable copy was taken on top of the
    // exposure fix's, so it comes off first.
    revertVScreenModeResolution();
    shutdownGlitchFrameFix();
    shutdownVScreenFixes();
    shutdownExposureFix();
    if (!g_state) return;
    // An orderly unload is not a crash, whether or not we got as far as the
    // frame count that normally confirms. Without this, a session that ends
    // cleanly inside the first six seconds would arm the next launch's refusal.
    if (g_state->sentinel && !g_state->sentinelConfirmed) {
        g_state->sentinelConfirmed = true;
        g_state->sentinel->confirm();
    }
    g_state->factoryHook.uninstall();
    g_state->swapChainHook.uninstall();
    g_state->deviceHook.uninstall();
}

}  // namespace edvr

