#include "device_hook.h"

#include <windows.h>

#include <dxgi1_2.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/hotkey.h"
#include "head_offset_gate.h"
#include "../common/log.h"
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
    // The external camera, and the key that cycles its view.
    //
    // A keypress is not a heuristic, and it is the whole reason this feature
    // can tell entering the external camera from boarding a ship: render state
    // alone cannot (EVIDENCE 6ac.6b). Unbound by default, which leaves the gate
    // on its weaker timing-window path rather than guessing.
    Hotkey externalCamKey;
    Hotkey externalCamNextKey;
    uint64_t frameCounter = 0;
};

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
    guardedBudget(g_frameBudget, [&] {
        if (g_state->toggleKey.pressed()) toggleExposureFix();
        // Deliberately not part of the toggle: it reports, it does not change
        // anything, so there is no reason for it to follow the fix being off.
        if (g_state->dumpKey.pressed()) dumpCameraRing();
        // Told to the gate, not acted on here. These keys are the player's OWN
        // Elite bindings: EDVR does not send them, press them or interfere with
        // them -- it watches for the same press the game gets, so it knows
        // which mode the player just asked for.
        if (g_state->externalCamKey.pressed()) headOffsetGateKeyPressed();
        if (g_state->externalCamNextKey.pressed()) headOffsetGateViewBumped();
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
        g_state->toggleKey.setKey(virtualKeyFromName(
            Config::get().getString("hotkey.toggle_exposure", "SCROLLLOCK").c_str()));
        g_state->dumpKey.setKey(virtualKeyFromName(
            Config::get().getString("hotkey.dump_camera", "PAUSE").c_str()));
        g_state->externalCamKey.setKey(virtualKeyFromName(
            Config::get().getString("hotkey.external_camera", "").c_str()));
        g_state->externalCamNextKey.setKey(virtualKeyFromName(
            Config::get().getString("hotkey.external_camera_next", "").c_str()));
    }
    return *g_state;
}

}  // namespace

void hookDevice(ID3D11Device* device) {
    if (!device) return;
    State& s = ensureState();
    if (s.device) return;

    if (!s.deviceHook.attach(device) ||
        s.deviceHook.executablePrefix() <= kDevCreateComputeShader) {
        Log::get().note("device vtable unusable; fix not installed");
        s.deviceHook.uninstall();
        return;
    }
    s.deviceHook.replace(kDevCreateComputeShader, &hookedCreateCS,
                         reinterpret_cast<void**>(&s.realCreateCS));
    if (!s.deviceHook.commit()) {
        s.deviceHook.uninstall();
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
    // Reverse of install order: vScreen's vtable copy was taken on top of the
    // exposure fix's, so it comes off first.
    revertVScreenModeResolution();
    shutdownGlitchFrameFix();
    shutdownVScreenFixes();
    shutdownExposureFix();
    if (!g_state) return;
    g_state->factoryHook.uninstall();
    g_state->swapChainHook.uninstall();
    g_state->deviceHook.uninstall();
}

}  // namespace edvr

