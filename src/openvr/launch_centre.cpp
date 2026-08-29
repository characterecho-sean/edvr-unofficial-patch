#include "launch_centre.h"

#include <windows.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "system_hook.h"

namespace edvr {
namespace {

bool     g_on = false;
bool     g_configured = false;
HMODULE  g_realModule = nullptr;

// Which runtime is underneath, decided by what its openvr_api exports.
//
// Neither DLL carries a version resource -- ProductName, FileDescription and
// CompanyName are all empty on both, checked on the field rig -- so there is
// no name to read. The export tables differ cleanly instead:
//
//   OpenComposite   VRClientCoreFactory, HmdSystemFactory, VR_Init,
//                   VR_InitInternal2 ... and NO VRDashboardManager
//   Valve           VRControlPanel, VRDashboardManager ... and no factories
//
// VRClientCoreFactory is the discriminator worth leaning on: it is the
// DRIVER-side factory, which in Valve's design lives in vrclient.dll and
// never in openvr_api.dll. OpenComposite is a single DLL pretending to be
// the whole stack, so it has to export it.
//
// The absent dashboard is the second witness, and the project already knew
// it independently -- openvr_proxy.cpp's interface-suppression comment says
// "OpenComposite has no dashboard to put an overlay in".
//
// Both tests must agree. A future Valve DLL growing one of these exports, or
// an OpenComposite build gaining a dashboard, then reads as "unsure" and
// auto stays off rather than guessing.
enum class Runtime { Unknown, OpenComposite, Valve };
Runtime g_runtime = Runtime::Unknown;

Runtime identifyRuntime(HMODULE m) {
    if (!m) return Runtime::Unknown;
    const bool coreFactory = GetProcAddress(m, "VRClientCoreFactory") != nullptr;
    const bool hmdFactory  = GetProcAddress(m, "HmdSystemFactory") != nullptr;
    const bool dashboard   = GetProcAddress(m, "VRDashboardManager") != nullptr;
    if (coreFactory && hmdFactory && !dashboard) return Runtime::OpenComposite;
    if (dashboard && !coreFactory) return Runtime::Valve;
    return Runtime::Unknown;
}
bool     g_done = false;      // the reset has been asked for, once
uint32_t g_waited = 0;        // frames spent waiting for the interface

// IVRSystem::ResetSeatedZeroPose, vtable slot 11.
//
// Read from Valve's openvr.h at the SDK whose IVRSystem_Version IS
// "IVRSystem_012" -- the generation Elite links, checked rather than assumed
// because the layout moved afterwards: GetOutputDevice was inserted at 8 in
// a later SDK and shifts everything below it. This build's own slot map for
// 0/1/2/4 (system_hook.cpp) agrees with that same header, which is a second
// witness for the table rather than a second guess.
//
// void, and no arguments. That matters more than the number.
// compositor_hook.cpp records why IVRSystem is never called from in here:
// GetProjectionMatrix returns HmdMatrix44_t BY VALUE, and the member-versus-C
// convention disagreement over the hidden return-slot argument crashed the
// game with a stack cookie failure after appearing to work. A method that
// takes nothing and returns nothing has no return slot and no arguments to
// marshal, so that failure mode has nothing to act on.
constexpr size_t kSlotResetSeatedZeroPose = 11;

typedef void(*PFN_ResetSeatedZeroPose)(void* self);

// Give up after this many frames rather than testing forever.
constexpr uint32_t kMaxWaitFrames = 600;

}  // namespace

void launchCentreNoteRuntime(void* realModule) {
    g_realModule = static_cast<HMODULE>(realModule);
}

void launchCentreConfigure() {
    Config& cfg = Config::get();
    const std::string v = cfg.getString("fix.launch_centre", "auto");

    if (g_runtime == Runtime::Unknown) g_runtime = identifyRuntime(g_realModule);
    const bool oc = (g_runtime == Runtime::OpenComposite);

    bool on = false;
    if (v == "on" || v == "1") {
        on = true;
    } else if (v == "off" || v == "0") {
        on = false;
    } else if (v == "auto" || v.empty()) {
        on = oc;
    } else {
        Log::get().note(
            "fix.launch_centre = \"%s\" is not a value this build knows; "
            "treating it as auto. Use auto, on or off.",
            v.c_str());
        on = oc;
    }

    if (!g_configured) {
        Log::get().note(
            "launch centre: the runtime under this proxy reads as %s -- by its "
            "exports, not its name, because neither DLL carries a version "
            "resource. OpenComposite exports VRClientCoreFactory and "
            "HmdSystemFactory and has no VRDashboardManager; Valve's is the "
            "other way round (src/openvr/launch_centre.h).%s",
            g_runtime == Runtime::OpenComposite ? "OPENCOMPOSITE"
            : g_runtime == Runtime::Valve       ? "SteamVR (Valve's own)"
                                                : "NEITHER clearly",
            (v == "auto" || v.empty())
                ? (oc ? " auto turns this ON here, because this is the runtime "
                        "whose origin moves between launches."
                      : " auto leaves this off: only OpenComposite has been "
                        "measured putting its origin somewhere new each launch.")
                : " A value other than auto is set, so that decides it.");
    }

    if (g_configured && on == g_on) return;
    g_configured = true;
    g_on = on;
    Log::get().note(
        on ? "launch centre: ON. Once the session is running, EDVR asks the "
             "runtime to put its seated origin where your head actually is -- "
             "one call to the same recentre the runtime offers, made for you "
             "before the movie starts. This exists because OpenComposite's "
             "origin lands somewhere different every launch, measured metres "
             "apart on two launches two minutes apart, which is what puts the "
             "splash and the intro movie in a new place each time "
             "(src/openvr/launch_centre.h). Nothing is subtracted from your "
             "poses afterwards: the runtime's own space moves, and everything "
             "downstream follows from it."
           : "launch centre: off. The game gets the runtime's origin as it "
             "finds it.");
}

void launchCentreApply(vr::EVRCompositorError err,
                       vr::TrackedDevicePose_t* renderPoses,
                       uint32_t renderCount,
                       vr::TrackedDevicePose_t* /*gamePoses*/,
                       uint32_t /*gameCount*/) {
    if (!g_on || g_done) return;
    // Anything but success and the arrays may be stale or untouched, so the
    // "is tracking up yet" test below would be reading nothing.
    if (err != 0) return;

    // Wait for a headset pose the runtime vouches for.
    //
    // Not impatience: OpenComposite's ResetSeatedZeroPose locates the view
    // space against the seated space and does NOTHING AT ALL if that location
    // comes back without valid position and orientation bits. Asking before
    // tracking is up spends the one call this makes and silently changes
    // nothing, which would look exactly like the feature being broken.
    if (!renderPoses || renderCount == 0 ||
        !renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
        if (++g_waited >= kMaxWaitFrames) {
            g_done = true;
            Log::get().note(
                "launch centre: gave up after %u frames without a valid "
                "headset pose. The origin is left as the runtime set it.",
                g_waited);
        }
        return;
    }

    void* sys = systemInterfaceV012();
    if (!sys) {
        if (++g_waited >= kMaxWaitFrames) {
            g_done = true;
            Log::get().note(
                "launch centre: gave up after %u frames -- no IVRSystem_012 "
                "was observed, so there is no interface whose slot map this "
                "build knows. The origin is left as the runtime set it.",
                g_waited);
        }
        return;
    }

    // Only once, whatever happens below. A recentre that runs every frame
    // would chase the player's head around the room.
    g_done = true;

    const auto& m = renderPoses[vr::k_unTrackedDeviceIndex_Hmd]
                        .mDeviceToAbsoluteTracking.m;
    const float bx = m[0][3], by = m[1][3], bz = m[2][3];

    void** vtable = *reinterpret_cast<void***>(sys);
    if (!vtable || !vtable[kSlotResetSeatedZeroPose]) {
        Log::get().note("launch centre: IVRSystem_012 has no method at slot "
                        "%zu; the origin is left as the runtime set it.",
                        kSlotResetSeatedZeroPose);
        return;
    }

    const bool survived = guarded("launchCentre/reset", [&] {
        reinterpret_cast<PFN_ResetSeatedZeroPose>(
            vtable[kSlotResetSeatedZeroPose])(sys);
    });

    if (!survived) {
        Log::get().note(
            "launch centre: the recentre call FAULTED. Nothing further is "
            "attempted this session and the origin stands as the runtime set "
            "it. Please report this log -- it means slot %zu is not "
            "ResetSeatedZeroPose on this runtime, and this build should stop "
            "believing it is.",
            kSlotResetSeatedZeroPose);
        return;
    }

    Log::get().note(
        "launch centre: asked the runtime to recentre. Your head was at "
        "%.3f %.3f %.3f when we asked; the NEXT handover pose line should "
        "read near zero for x and z if the runtime honoured it. If it reads "
        "the same as this, the call landed on a tracking space the game is "
        "not using -- OpenComposite moves its SEATED space here, and a game "
        "asking for standing poses would not follow.",
        static_cast<double>(bx), static_cast<double>(by),
        static_cast<double>(bz));
}

bool launchCentreLatched() { return g_done; }

}  // namespace edvr
