#include "compositor_hook.h"
#include "head_offset.h"

#include <windows.h>

#include <cstring>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/proxy.h"  // breadcrumb(), EDVR_BREADCRUMB_ONCE
#include "../common/vtable_hook.h"
#include "openvr_min.h"

namespace edvr {
namespace {

// ---------------------------------------------------------------------------
// Vtable slot table.
//
// Every index here comes from the public openvr.h declaration order and is
// UNVERIFIED against whichever runtime the game actually loads. Three
// independent guards make a wrong value degrade rather than crash: the allowlist
// below, argument validation on the first calls, and the startup crash sentinel.
// ---------------------------------------------------------------------------

struct CompositorSlots {
    const char* version;
    size_t      waitGetPoses;
    size_t      submit;
};

constexpr CompositorSlots kCompositorTable[] = {
    // The build in hand links IVRCompositor_014. Established by scanning
    // EliteDangerous64.exe for IVR*_nnn literals: it contains exactly
    // IVRSystem_012, IVRCompositor_014, IVROverlay_011, IVRChaperone_003 and
    // IVRExtendedDisplay_001, and nothing else.
    {"IVRCompositor_014", 2, 5},
    // Later generations kept the first six methods in the same order. Listed so
    // a game update that moves forward does not silently stop being handled.
    {"IVRCompositor_015", 2, 5},
    {"IVRCompositor_016", 2, 5},
    {"IVRCompositor_020", 2, 5},
    {"IVRCompositor_021", 2, 5},
    {"IVRCompositor_022", 2, 5},
    {"IVRCompositor_024", 2, 5},
    {"IVRCompositor_026", 2, 5},
    {"IVRCompositor_027", 2, 5},
    {"IVRCompositor_028", 2, 5},
};

// IVRSystem is deliberately never hooked and never called from inside the game.
//
// An earlier version queried GetProjectionMatrix from the WaitGetPoses hook. It
// crashed the game with a stack cookie failure AFTER appearing to succeed: the
// matrices logged correctly, then the frame returned onto a corrupted stack. At
// the IVRSystem_012 generation that method takes a fourth parameter which later
// versions dropped, so a signature copied from the current openvr.h mismatches
// the frame. Nothing here needs that data, so nothing here asks for it.

typedef vr::EVRCompositorError(*PFN_Submit)(void* self, vr::EVREye eye,
                                            const vr::Texture_t* texture,
                                            const vr::VRTextureBounds_t* bounds,
                                            vr::EVRSubmitFlags flags);

typedef vr::EVRCompositorError(*PFN_WaitGetPoses)(void* self,
                                                  vr::TrackedDevicePose_t* renderPoses,
                                                  uint32_t renderCount,
                                                  vr::TrackedDevicePose_t* gamePoses,
                                                  uint32_t gameCount);

struct State {
    VTableHook       compositorHook;
    PFN_Submit       realSubmit = nullptr;
    PFN_WaitGetPoses realWaitGetPoses = nullptr;

    Sentinel* sentinel = nullptr;

    uint32_t configPollCounter = 0;

    bool     inert = false;        // hook installed but doing nothing
    bool     validated = false;
    uint32_t submitCalls = 0;
    uint32_t eyesSeen = 0;         // bit 0 left, bit 1 right

    // Refused this session, so a SECOND compositor request does not sail past.
    //
    // clearTrip() resets the sentinel in memory as well as on disk, so after a
    // refusal trippedOnStartup() is false -- and the very scenarios the refusal
    // message cites (SteamVR restarting, the headset waking) are exactly the ones
    // that ask for the interface again. The hook would install mid-"refused"
    // session, which is not what the user was told.
    bool     refusedThisSession = false;
    uint32_t framesWithheld = 0;
    uint32_t notesLeft = 80;
};

State* g_state = nullptr;

// Cheap sanity check on the arguments of what we believe is Submit. If the
// vtable index were wrong we would be reading another method's registers, and
// these would almost certainly fail.
bool submitArgsLookSane(vr::EVREye eye, const vr::Texture_t* texture) {
    if (eye != vr::Eye_Left && eye != vr::Eye_Right) return false;
    if (!texture) return false;

    vr::ETextureType type = vr::TextureType_Invalid;
    vr::EColorSpace  space = vr::ColorSpace_Auto;
    void*            handle = nullptr;
    if (!guarded("submitArgsLookSane/read", [&] {
            handle = texture->handle;
            type   = texture->eType;
            space  = texture->eColorSpace;
        })) {
        return false;
    }
    if (type < vr::TextureType_Invalid || type > vr::TextureType_Metal) return false;
    if (space < vr::ColorSpace_Auto || space > vr::ColorSpace_Linear) return false;
    if (!handle) return false;
    return true;
}

// ---------------------------------------------------------------------------

vr::EVRCompositorError hookedSubmit(void* self, vr::EVREye eye,
                                    const vr::Texture_t* texture,
                                    const vr::VRTextureBounds_t* bounds,
                                    vr::EVRSubmitFlags flags) {
    EDVR_BREADCRUMB_ONCE("vr: hookedSubmit entered");
    State* s = g_state;
    if (!s || !s->realSubmit) return 0;

    if (s->inert) return s->realSubmit(self, eye, texture, bounds, flags);

    // Validate before doing anything else. A hook on the wrong slot fails here
    // and goes inert instead of interfering with the headset.
    if (!s->validated) {
        if (!submitArgsLookSane(eye, texture)) {
            breadcrumb("vr: hookedSubmit VALIDATION FAILED, going inert");
            Log::get().note(
                "compositor hook VALIDATION FAILED at call %u (eye=%d texture=%p). Going "
                "inert; the Submit vtable index for this interface version is wrong. The "
                "game renders normally from here.",
                s->submitCalls, static_cast<int>(eye), static_cast<const void*>(texture));
            s->inert = true;
            return s->realSubmit(self, eye, texture, bounds, flags);
        }
        s->eyesSeen |= (eye == vr::Eye_Left) ? 1u : 2u;
        if (++s->submitCalls >= 8 && s->eyesSeen == 3u) {
            s->validated = true;
            if (s->sentinel) s->sentinel->confirm();
            // Announced only now, not at install: the d3d11 side is asking
            // "will anything actually act on what I detect", and a hook that has
            // not proved it is on the right slot is not an answer to that.
            announceGlitchConsumer();
            Log::get().note("compositor hook validated after %u calls (both eyes seen)",
                            s->submitCalls);
        }
    }

    // The frame the d3d11 side marked as drawn from the wrong viewpoint: do not
    // pass it on. SteamVR reprojects the previous frame, exactly as it does for
    // any frame a game fails to deliver in time.
    //
    // Substituting the previous frame's texture was tried instead and is a
    // no-op: Elite reuses ONE texture per eye for the whole session, so the
    // "previous" texture IS this one and already holds the bad pixels -- 30 of
    // 30 substitutions had identical pointers. That path is not kept as an
    // option, because leaving it in place alongside this one is exactly what
    // caused a regression once: the detector improved while this side was still
    // quietly substituting, so nothing was withheld at all.
    //
    // Declining costs roughly 80 ms on the following frame, because the
    // compositor is waiting for a submit that never comes. That is the price of
    // the fix, and it is only worth paying once per event -- which is what the
    // d3d11 side's "did the camera come back?" test ensures.
    //
    // Only once validated: a hook that has not proved it is on the right slot
    // has no business withholding anything.
    if (s->validated && glitchFrameMarked()) {
        ++s->framesWithheld;
        if (s->notesLeft > 0) {
            --s->notesLeft;
            Log::get().note("transition flash: frame NOT submitted (eye %d). SteamVR will "
                            "reproject the previous frame. %u eye-submits withheld so far "
                            "-- two per frame, so half this many frames.",
                            static_cast<int>(eye), s->framesWithheld);
        }
        // Success without submitting: what the game is told about every frame
        // the compositor later drops for timing reasons. 0 rather than a named
        // constant because openvr_min.h declares EVRCompositorError as a plain
        // int32_t and does not carry the enumerators.
        return 0;   // VRCompositorError_None
    }

    return s->realSubmit(self, eye, texture, bounds, flags);
}





vr::EVRCompositorError hookedWaitGetPoses(void* self,
                                          vr::TrackedDevicePose_t* renderPoses,
                                          uint32_t renderCount,
                                          vr::TrackedDevicePose_t* gamePoses,
                                          uint32_t gameCount) {
    State* s = g_state;
    if (!s || !s->realWaitGetPoses) return 0;

    // WaitGetPoses blocks until the compositor releases the app, which makes it
    // the natural frame boundary.
    const vr::EVRCompositorError result =
        s->realWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);

    if (s->inert) return result;

    if (renderCount > vr::k_unMaxTrackedDeviceCount ||
        gameCount > vr::k_unMaxTrackedDeviceCount) {
        breadcrumb("vr: hookedWaitGetPoses VALIDATION FAILED, going inert");
        Log::get().note("WaitGetPoses argument check failed (counts %u/%u); going inert",
                        renderCount, gameCount);
        s->inert = true;
        return result;
    }

    // This is where a mark stops applying. Without it one detection would
    // suppress every frame after it, and the headset would freeze rather than
    // skip a single frame.
    clearGlitchFrame();

    // The head offset, applied BEFORE the game sees the poses.
    //
    // Outside the telemetry guard below on purpose. That guard has a fault
    // budget and disables itself after five faults, which is right for logging
    // -- losing a log line is nothing -- but wrong for something that changes
    // what the player sees.
    //
    // The feature lives in head_offset.cpp, which is SHARED. It was written
    // inline here, in a file that is FORKED, and this copy lost the
    // install-time config read in the hand-copy -- so this build ran every
    // session with the offsets still zero and did nothing at all. That is not
    // a mistake sync_common can see in a forked file. It can see it there.
    headOffsetApply(result, renderPoses, renderCount, gamePoses, gameCount);

    // The config reload poll, BACK, and the reason it was removed is the
    // reason it returns.
    //
    // It went because nothing in this module read config after install, so a
    // GetFileAttributesEx per second bought nothing. The head offset changed
    // that: it is a position in metres that has to be found by feel, from
    // inside a headset, where a text editor cannot be seen. A startup-only
    // offset costs a relaunch per guess, which is not tuning.
    //
    // Still cheap, and for the same reason as before: one GetFileAttributesEx,
    // and a full reparse only when the write time actually moved.
    if (++s->configPollCounter >= 90) {
        s->configPollCounter = 0;
        if (Config::get().reloadIfChanged()) {
            Log::get().note("config reloaded");
            headOffsetConfigure();
        }
    }

    return result;
}

}  // namespace

void* interceptInterface(void* iface, const char* interfaceVersion) {
    if (!iface || !interfaceVersion) return iface;

    Config& cfg = Config::get();
    Log::get().note("VR_GetGenericInterface(\"%s\")", interfaceVersion);

    if (!g_state) g_state = new State();
    State& s = *g_state;

    if (strncmp(interfaceVersion, "IVRCompositor_", 14) != 0) return iface;
    if (s.compositorHook.attached()) return iface;  // already hooked

    // TWO features live behind this hook now, and it must install if EITHER
    // wants it.
    //
    // It installed only for the transition-flash fix, because that was the only
    // thing here when it was written. The head offset then moved in -- and
    // fix.transition_flash = 0 silently took the head offset down with it, in a
    // build where d3d11.dll went on logging "head offset ON" every time the
    // player entered the camera. Two halves disagreeing about whether a feature
    // is running, with only one of them able to say so.
    //
    // The gate is what decides whether the offset can ever be wanted; the
    // offsets themselves are read after install, so the honest test at this
    // point is "is the feature switched on at all".
    const bool wantFlash = cfg.getBool("fix.transition_flash", true);
    const bool wantOffset = cfg.getBool("fix.head_offset_gate", true);
    if (!wantFlash && !wantOffset) {
        Log::get().note("compositor passed through unhooked: fix.transition_flash "
                        "and fix.head_offset_gate are both off, and those are the "
                        "only two features that need this hook.");
        return iface;
    }
    if (!wantFlash) {
        Log::get().note("transition flash fix off, but the compositor hook is "
                        "installed anyway for the head offset -- no eye submits "
                        "will be withheld.");
    }

    size_t submitSlot = 0, posesSlot = 0;
    bool known = false;
    for (const CompositorSlots& e : kCompositorTable) {
        if (strcmp(e.version, interfaceVersion) == 0) {
            submitSlot = e.submit;
            posesSlot = e.waitGetPoses;
            known = true;
            break;
        }
    }

    const int submitOverride = cfg.getInt("advanced.submit_index", -1);
    const int posesOverride = cfg.getInt("advanced.waitgetposes_index", -1);
    if (submitOverride >= 0 && posesOverride >= 0) {
        // The two must be different slots.
        //
        // Only ">= 0" was checked. Setting both to the same number made the
        // second replace() take the FIRST hook as its "original", so
        // realWaitGetPoses became hookedSubmit: the first Submit reinterpreted
        // its arguments as a pose array, and once the hook went inert it called
        // the real Submit with garbage -- outside any SEH, on frame one. A typo
        // in a hand-edited advanced setting should not be able to do that.
        if (submitOverride == posesOverride) {
            Log::get().note(
                "IGNORING the vtable overrides: submit_index and waitgetposes_index are "
                "both %d, and they must name different slots. Using the layout this build "
                "knows instead.",
                submitOverride);
        } else {
            submitSlot = static_cast<size_t>(submitOverride);
            posesSlot = static_cast<size_t>(posesOverride);
            known = true;
            Log::get().note("using config vtable overrides: Submit=%d WaitGetPoses=%d",
                            submitOverride, posesOverride);
        }
    }

    if (!known) {
        Log::get().note(
            "compositor version %s is not one this build knows the layout of. Passing "
            "through unhooked -- the game renders normally, without the transition flash "
            "fix. Please report the version string above.",
            interfaceVersion);
        return iface;
    }

    // A previous run armed the hook and never confirmed it.
    //
    // Refuse this session, then clear the trip so the next one tries again.
    //
    // It used to refuse forever. confirm() runs on validation, on commit
    // failure, and from shutdownCompositorHook -- which only happens on
    // FreeLibrary, and a game closing is process termination, so it never runs
    // at all. Any session that ended before eight valid submits therefore left
    // the file behind: SteamVR restarting, the headset dropping to standby, or
    // the player quitting from the menu. Every later launch then announced that
    // the hook had "very likely crashed" when nothing had, and the only cure was
    // deleting a file the message named but nobody would find.
    //
    // One session is the right price. A hook that genuinely crashes still gets
    // thrown out on the launch after every crash, which is what the protection
    // is for; a false trip costs a single flash-fix-less session instead of all
    // of them.
    if (!s.sentinel) s.sentinel = new Sentinel(cfg.logDir().c_str(), L"compositor_hook");
    if (s.refusedThisSession) return iface;
    if (s.sentinel->trippedOnStartup() &&
        !cfg.getBool("advanced.ignore_sentinel", false)) {
        s.refusedThisSession = true;
        s.sentinel->clearTrip();
        Log::get().note(
            "SENTINEL TRIPPED: the previous run armed the compositor hook and never "
            "confirmed it. That usually means it crashed, but a session that simply "
            "ended early -- SteamVR restarting, or quitting from the menu -- looks the "
            "same from here. Skipping the hook for THIS session only; it will try again "
            "next launch. If it keeps happening, the hook really is crashing -- report the "
            "log. To force it on anyway, set ignore_sentinel = 1 under [advanced].\n"
            "  This takes down EVERY feature that needs this hook: the transition "
            "flash fix AND the on-foot head offset. d3d11.dll cannot tell, so it "
            "will go on logging \"head offset ON\" while nothing moves.");
        return iface;
    }

    if (!s.compositorHook.attach(iface)) {
        Log::get().note("compositor vtable attach failed; passing through");
        return iface;
    }
    // Range-check against the executable prefix, not the copy width: the copy is
    // deliberately over-wide so the host can call methods past the last one we
    // recognise.
    const size_t prefix = s.compositorHook.executablePrefix();
    if (prefix <= submitSlot || prefix <= posesSlot) {
        Log::get().note("compositor vtable has %zu methods, need >%zu; passing through",
                        prefix, submitSlot > posesSlot ? submitSlot : posesSlot);
        s.compositorHook.uninstall();
        return iface;
    }

    if (!s.sentinel->arm()) {
        // Say so rather than proceeding quietly. With log.enabled = 0 the log
        // directory may not exist, and arm() used to set its flag regardless --
        // so the file was never written, the next launch saw no trip, and a hook
        // that really was crashing re-armed every start. arm() creates the
        // directory now, but if it still cannot write, this session is running
        // without the protection and that is worth one line.
        Log::get().note("NOTE: the crash sentinel could not be written, so a crash in "
                        "this hook will not disable it next launch.");
    }
    breadcrumb("vr: arming compositor hook");

    s.compositorHook.replace(submitSlot, reinterpret_cast<void*>(&hookedSubmit),
                             reinterpret_cast<void**>(&s.realSubmit));
    s.compositorHook.replace(posesSlot, reinterpret_cast<void*>(&hookedWaitGetPoses),
                             reinterpret_cast<void**>(&s.realWaitGetPoses));
    if (!s.compositorHook.commit()) {
        Log::get().note("compositor vtable commit failed; passing through");
        s.compositorHook.uninstall();
        s.sentinel->confirm();
        return iface;
    }
    breadcrumb("vr: compositor hook committed");
    Log::get().note("compositor hook installed on %s (Submit slot %zu, WaitGetPoses "
                    "slot %zu)", interfaceVersion, submitSlot, posesSlot);

    // READ THE CONFIG NOW, not only when the ini changes.
    //
    // This line was missing, and its absence is what a whole shipped release of
    // the head offset amounted to: the values kept their zero initialisers for
    // the entire session, so the feature was configured, logged as gated, and
    // did nothing -- unless the player happened to save edvr.ini mid-flight,
    // which then snap-loaded the full offset into a live scene.
    //
    // The same class cost a flight on the d3d11 side and produced
    // tools/check_config_paths.py. Here the structural answer is that
    // head_offset.cpp is SHARED, so there is one reader and both builds call it
    // from install and from reload.
    headOffsetConfigure();

    return iface;
}

void shutdownCompositorHook() {
    if (!g_state) return;
    Log::get().note("transition flash: %u eye-submit(s) withheld this session.",
                    g_state->framesWithheld);
    g_state->compositorHook.uninstall();
    if (g_state->sentinel) g_state->sentinel->confirm();
}

}  // namespace edvr
