#include "early_session.h"

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "compositor_hook.h"
#include "openvr_min.h"

namespace edvr {
namespace {

bool g_ran = false;

// Submit, as the compositor's vtable holds it. Same shape compositor_hook.cpp
// declares; repeated here rather than shared because that one is a private
// typedef of the hook and this call is not going through the hook.
typedef vr::EVRCompositorError(*PFN_Submit)(void* self, vr::EVREye eye,
                                            const vr::Texture_t* texture,
                                            const vr::VRTextureBounds_t* bounds,
                                            vr::EVRSubmitFlags flags);

// The 1x1 texture the handover is made with.
//
// Format and bind flags are TRANSCRIBED, not chosen: OpenComposite's own log
// records what the game's first skybox texture looked like when it made the
// swap chain that triggered the rebuild --
//
//     Texture desc format: 28      DXGI_FORMAT_R8G8B8A8_UNORM
//     Texture desc bind flags: 8   D3D11_BIND_RENDER_TARGET
//     Texture desc width: 1
//     Texture desc height: 1
//
// so this is the shape the runtime is already known to accept at this point
// in its life. Guessing a format here would be guessing at the one thing the
// log already answers.
ID3D11Texture2D* makeHandoverTexture(ID3D11Device* dev) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    // Initialised rather than left undefined. A one-pixel texture handed to a
    // compositor is going to be read, and reading uninitialised video memory
    // is the kind of thing that is invisible on the rig it was written on.
    const uint32_t black = 0xFF000000u;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &black;
    init.SysMemPitch = sizeof(black);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &init, &tex))) return nullptr;
    return tex;
}

// Ask the runtime for a compositor, trying the versions this build knows.
//
// The game has not asked yet, so there is no version to match -- the runtime
// decides what it implements. OpenComposite answers several generations; real
// SteamVR answers the ones it still supports. Either way the FIRST answer is
// used, and its own table row supplies Submit's slot, so the pair can never be
// mismatched.
void* acquireCompositor(PFN_RealGetGenericInterface get, size_t* submitSlotOut,
                        const char** versionOut) {
    for (size_t i = 0; i < knownCompositorCount(); ++i) {
        const char* version = knownCompositorVersion(i);
        if (!version) continue;
        vr::EVRInitError err = 0;
        void* iface = get(version, &err);
        if (!iface) continue;
        *submitSlotOut = knownCompositorSubmitSlot(i);
        *versionOut = version;
        return iface;
    }
    return nullptr;
}

void runInner(PFN_RealGetGenericInterface get) {
    Config& cfg = Config::get();

    // Values name what the player gets -- when the handover happens -- not the
    // mechanism underneath it.
    const std::string mode = cfg.getString("fix.vr_handover", "early");
    if (mode != "early") {
        if (mode != "stock") {
            Log::get().note(
                "fix.vr_handover = \"%s\" is not a value this build knows; "
                "treating it as stock. Use early or stock.",
                mode.c_str());
        }
        return;
    }

    ID3D11Device* dev = static_cast<ID3D11Device*>(gameDevice());
    if (!dev) {
        // Not a fault. d3d11.dll is optional, and even with it installed the
        // game could in principle reach VR init before creating a device --
        // it does not on any rig measured, but "measured on one rig" is not
        // "cannot happen", and the honest answer is to stand down and say so.
        Log::get().note(
            "vr handover: no D3D11 device has been published -- either "
            "d3d11.dll is not installed alongside this file, or the game "
            "reached VR init before creating its device. Standing down; the "
            "handover happens on the game's own schedule, exactly as it does "
            "with fix.vr_handover = stock.");
        return;
    }

    size_t submitSlot = 0;
    const char* version = nullptr;
    void* comp = acquireCompositor(get, &submitSlot, &version);
    if (!comp) {
        Log::get().note(
            "vr handover: the runtime answered none of the %zu IVRCompositor "
            "versions this build knows, so there is nothing to hand a frame "
            "to. Standing down.",
            knownCompositorCount());
        return;
    }

    void** vtable = *reinterpret_cast<void***>(comp);
    if (!vtable) {
        Log::get().note("vr handover: the compositor has no vtable; standing down.");
        return;
    }
    PFN_Submit submit = reinterpret_cast<PFN_Submit>(vtable[submitSlot]);
    if (!submit) {
        Log::get().note("vr handover: %s has no method at Submit's slot %zu; "
                        "standing down.", version, submitSlot);
        return;
    }

    ID3D11Texture2D* tex = makeHandoverTexture(dev);
    if (!tex) {
        Log::get().note("vr handover: could not create the 1x1 handover texture "
                        "on the game's device; standing down.");
        return;
    }

    Log::get().note(
        "vr handover: EARLY -- submitting a 1x1 frame on the game's own device "
        "(%p) through %s now, so OpenComposite rebuilds its OpenXR session for "
        "the game's graphics API here instead of inside the game's first "
        "compositor call, where it lands part-way through the intro movie. "
        "The rebuild costs about 2.5 s on this rig either way. It does NOT "
        "cost the frame loop: the game runs VR init on a different thread from "
        "the one that presents, measured 2026-08-29 -- the render thread kept "
        "going the whole time, and no frame reached even 200 ms. The next line "
        "says how long it took.",
        static_cast<void*>(dev), version);

    vr::Texture_t t = {};
    t.handle = tex;
    t.eType = vr::TextureType_DirectX;
    t.eColorSpace = vr::ColorSpace_Auto;

    const uint64_t began = nowMs();
    vr::EVRCompositorError err = 0;
    const bool survived = guarded("earlySession/submit", [&] {
        // Left eye only. One submit is enough to make the runtime bind the
        // application's device, and an unpaired eye at this point cannot
        // reach a display: the game has not begun a frame, so a runtime that
        // does anything with this at all soft-aborts, which is what
        // OpenComposite is observed to do with the game's own first call.
        err = submit(comp, vr::Eye_Left, &t, nullptr, vr::Submit_Default);
    });
    const uint64_t took = nowMs() - began;

    if (!survived) {
        // guarded() has already logged the fault site. Say what it means here,
        // because the useful half is that the session is now in an unknown
        // state and the game is about to use it.
        Log::get().note(
            "vr handover: the early submit FAULTED after %llu ms. Nothing "
            "further is attempted this session. The game's own handover still "
            "happens on its own schedule, so the session recovers; what is "
            "lost is the early part, not the session. Please report this log.",
            static_cast<unsigned long long>(took));
    } else {
        Log::get().note(
            "vr handover: early submit returned %d after %llu ms. A time near "
            "2500 ms is the session rebuild happening HERE, which is the whole "
            "point -- compare the OpenComposite log's "
            "\"Recreating OpenXR session\" timestamp against the game's first "
            "eye submit. A time near zero means the rebuild did not happen "
            "here and will still happen later; the error code says why the "
            "runtime declined.",
            static_cast<int>(err), static_cast<unsigned long long>(took));
    }

    tex->Release();
}

}  // namespace

void earlySessionRun(PFN_RealGetGenericInterface get) {
    if (g_ran || !get) return;
    g_ran = true;
    // The whole body is guarded, not just the submit: acquireCompositor calls
    // into the runtime, and reading a vtable off an interface pointer is a
    // dereference of something another process's DLL owns. A fault anywhere in
    // here must cost the handover and nothing else -- the game has not started
    // rendering yet, and it must still be able to.
    guarded("earlySession", [&] { runInner(get); });
}

}  // namespace edvr
