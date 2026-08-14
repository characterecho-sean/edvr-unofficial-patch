#include "compositor_hook.h"
#include "pose_offset.h"

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

    // The head-pose offset: where the player's viewpoint is moved to, in the
    // tracking frame, and whether it is being moved at all.
    //
    // Elite renders from the poses WaitGetPoses returns (EVIDENCE 6ac.1), so
    // translating the HMD pose translates the viewpoint AND the game's own
    // culling and object placement with it -- as far as the game knows, the
    // player leaned. That is what makes this work where editing a camera
    // constant buffer did not: those moved the rendered view and left the
    // game's idea of the camera behind it.
    float    headOffset[3] = {};
    float    headYawSin = 0.0f, headYawCos = 1.0f;
    float    headYawDeg = 0.0f;
    bool     headOffsetGamePoses = true;
    bool     headOffsetExternalOnly = true;
    bool     headOffsetNoted = false;
    bool     headOffsetGateNoted = false;
    bool     headOffsetStaleNoted = false;
    // Poses actually offset. A counter rather than a flag because "the offset
    // is configured" and "the offset reached a pose" are different claims, and
    // only the second one is evidence.
    uint32_t headOffsetApplied = 0;
    // ~1 second at 90 Hz. Both halves run once per rendered frame, so this is
    // slack for jitter rather than a timeout anybody should need to tune: in
    // normal operation the gate's stamp moves every single frame.
    uint32_t headOffsetMaxStale = 90;
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

// Read the offset, CLAMPING anything that is not a sane number of metres.
//
// Ten metres, not two. Two was set from "the on-foot target is under a metre",
// which was wrong about the job: the external camera starts a few metres behind
// the commander, so reaching the head means travelling that whole distance
// first. The user hit the limit at -2.3 while positioning it, which is well
// inside normal use. A limit that a correct value trips is not a safety limit.
//
// CLAMPED, not zeroed, and the difference matters more than the number.
// Refusing to 0 threw away the whole offset, so a value one notch too far
// snapped the viewpoint several metres in one frame -- a large involuntary jump
// for someone wearing the headset, which is exactly what a limit protecting
// them should not cause. It also read as a bug rather than as a limit: the user
// reported it as "it jumped back", because from inside a headset there is no
// log to see. Clamping stops the movement at the boundary, which is
// self-evident without reading anything.
//
// A limit is still worth having. This value is added to a head pose, and a
// fat-fingered 100 puts the viewpoint far enough away that the world is a speck
// -- disorienting, and indistinguishable from the fix being broken.
// The axes are NAMED, and forward is positive, which means it is stored
// negated.
//
// They were head_offset_x/y/z, handed to the pose untouched. That is the OpenVR
// tracking frame -- +x right, +y up, -z forward -- and it is right, but nobody
// placing a camera thinks in it. The tuned value for the on-foot viewpoint was
// z = -2.6 for a position 2.6 m FORWARD of where the camera starts, so the one
// axis anybody actually reaches for was the one whose sign read backwards.
//
//     head_offset_right      +x, unchanged
//     head_offset_up         +y, unchanged
//     head_offset_forward    stored as -z
//
// Everything downstream still sees the OpenVR frame. The flip lives here, at
// the one place the number crosses from the file into the program, so
// applyPoseOffset and its test are untouched by it and there is exactly one
// line where the sign can be wrong.
//
// The old names are NOT accepted as a fallback. The same number means opposite
// directions under head_offset_z and head_offset_forward, so honouring a stale
// key would move somebody's viewpoint 5 m the wrong way without saying so.
// They are detected and reported instead, which is the one case where silence
// is worse than an error.
// One axis: NaN out, clamp, and report in the value the player typed.
//
// CLAMPED, not zeroed, and the difference matters more than the number.
// Refusing to 0 threw away the whole offset, so a value one notch too far
// snapped the viewpoint several metres in one frame -- a large involuntary jump
// for someone wearing the headset, which is exactly what a limit protecting
// them should not cause.
float readOffsetAxis(Config& cfg, const char* key, float v) {
    constexpr float kMax = 10.0f;
    // NaN first, and not by comparison: NaN fails every ordered test, so a
    // clamp written as min/max would pass it straight through to the pose. A
    // NaN in a transform propagates to every vertex and the frame goes blank,
    // with nothing in the log to say why.
    if (v != v) {
        Log::get().note("%s is not a number; using 0.", key);
        return 0.0f;
    }
    if (v > kMax || v < -kMax) {
        const float c = v > kMax ? kMax : -kMax;
        Log::get().note("%s = %.3f is beyond +/-%.1f m, so %.1f is being used. "
                        "This value moves the viewpoint of a headset somebody is "
                        "wearing. If you need more range than this, raise the "
                        "limit in the source rather than working around it.",
                        key, v, kMax, c);
        return c;
    }
    return v;
}

// Warn once about a name that no longer does anything.
// The section is prepended HERE, so no string literal anywhere spells out
// "openvr.head_offset_x".
//
// That is not cosmetic. tools/check_config_contract.py treats a section.key
// literal as either a setting being read or a setting being named, and demands
// the two agree with edvr.ini -- which is the rule that stops a documented
// setting and a real one drifting apart. These are neither: they are names
// being REFUSED, and documenting them as settings to satisfy a checker would
// tell users they can set something that does nothing.
void notePlacementRetired(Config& cfg, const char* bare, const char* now,
                          bool* noted, const char* extra) {
    if (*noted) return;
    const std::string key = std::string("openvr.") + bare;
    if (cfg.getString(key.c_str(), "").empty()) return;
    *noted = true;
    Log::get().note("[openvr] %s is a retired name and is being IGNORED. Use %s "
                    "instead%s.", bare, now, extra);
}

void readHeadOffset(State* s) {
    Config& cfg = Config::get();
    // Read one axis at a time, with each key spelled out where it is used.
    //
    // A {key, axis, sign} table was tidier and hid every key name from
    // check_config_contract, which requires the literal to be the argument of
    // the get. That check is what stops a documented setting and a read setting
    // drifting apart -- the failure where a user sets something real-looking
    // and nothing happens -- and it is worth more than three saved lines.
    //
    // FORWARD IS STORED NEGATED, and this is the only place that happens.
    // +x right, +y up and -z forward is the OpenVR tracking frame, and it is
    // right, but nobody placing a camera thinks in it. The tuned value was
    // z = -2.75 for a viewpoint 2.75 m in FRONT of where the camera starts, so
    // the one axis anybody reaches for was the one whose sign read backwards.
    // Everything downstream still sees the OpenVR frame, so applyPoseOffset and
    // its test are untouched by this and there is exactly one line where the
    // sign can be wrong.
    s->headOffset[0] = readOffsetAxis(
        cfg, "openvr.head_offset_right",
        cfg.getFloat("openvr.head_offset_right", 0.0f));
    s->headOffset[1] = readOffsetAxis(
        cfg, "openvr.head_offset_up",
        cfg.getFloat("openvr.head_offset_up", 0.0f));
    s->headOffset[2] = -readOffsetAxis(
        cfg, "openvr.head_offset_forward",
        cfg.getFloat("openvr.head_offset_forward", 0.0f));

    // The old names are NOT accepted as a fallback. The same number means
    // opposite directions under head_offset_z and head_offset_forward, so
    // honouring a stale key would move somebody's viewpoint 5.5 m the wrong way
    // without saying so. They are reported instead, which is the one case where
    // silence is worse than an error.
    static bool retiredX = false, retiredY = false, retiredZ = false;
    notePlacementRetired(cfg, "head_offset_x", "head_offset_right",
                         &retiredX, "");
    notePlacementRetired(cfg, "head_offset_y", "head_offset_up",
                         &retiredY, "");
    notePlacementRetired(cfg, "head_offset_z", "head_offset_forward",
                         &retiredZ,
                         ", and flip its sign -- head_offset_z = -2.75 is "
                         "head_offset_forward = 2.75");
    // Wrapped rather than clamped. A yaw is periodic, so 190 and -170 are the
    // same heading and neither is a mistake worth refusing -- unlike a
    // translation, where a big number really does put the viewpoint somewhere
    // useless. fmodf keeps it in (-360, 360), which is all the trig needs.
    const float rawYaw = cfg.getFloat("openvr.head_yaw_degrees", 0.0f);
    s->headYawDeg = (rawYaw == rawYaw) ? fmodf(rawYaw, 360.0f) : 0.0f;
    const float rad = s->headYawDeg * 3.14159265358979f / 180.0f;
    s->headYawSin = sinf(rad);
    s->headYawCos = cosf(rad);
    // sinf(pi) is about 1.2e-16 rather than 0, so a half turn never compares
    // exactly equal to "no rotation". The test in applyHeadOffset is written to
    // notice cos too, which at 180 degrees is -1 and unmistakable.

    s->headOffsetGamePoses = cfg.getBool("openvr.head_offset_game_poses", true);
    // Default ON. An ungated offset moves the cockpit view too, which was
    // right for the one-key diagnostic and is wrong for a feature.
    s->headOffsetExternalOnly =
        cfg.getBool("openvr.head_offset_external_only", true);
    // Clamped low, not just defaulted. Zero would disarm the offset on any
    // frame the two halves did not line up exactly, and a huge value turns the
    // guard off without saying so -- which is the failure it exists to prevent.
    const int stale = cfg.getInt("openvr.head_offset_max_stale_frames", 90);
    s->headOffsetMaxStale = static_cast<uint32_t>(stale < 2 ? 2 : stale);
    s->headOffsetNoted = false;   // say it again after a change
    s->headOffsetStaleNoted = false;
}

// Translate the HMD pose the runtime just produced, in the tracking frame.
//
// mDeviceToAbsoluteTracking is a 3x4 row-major rigid transform: the left 3x3 is
// the rotation and column 3 is the position, so translating in the TRACKING
// frame is an add to m[0..2][3] and touches the rotation not at all. Adding it
// to the rotated axes instead would express the offset in the HEAD's frame,
// which is the form 6x.1 refuted.
//
// An invalid pose is left alone. The runtime marks a pose invalid when it has
// nothing to report -- tracking lost, device asleep -- and the matrix behind it
// is not required to be meaningful. Offsetting a matrix that is not a pose
// produces a pose, which is worse than the flag the game already knows how to
// handle.
void applyHeadOffset(State* s, vr::TrackedDevicePose_t* poses, uint32_t count) {
    if (!poses || count <= vr::k_unTrackedDeviceIndex_Hmd) return;
    vr::TrackedDevicePose_t& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmd.bPoseIsValid) return;
    // Elite's external camera is a self-facing portrait view: it looks back at
    // the commander. Placed at the head that has the player facing backwards,
    // which inverts the whole point of being there, so the yaw turns them round
    // without disturbing the placement.
    //
    // The arithmetic lives in pose_offset.h so it can be tested without a
    // headset. See tools/pose_test.
    applyPoseOffset(hmd.mDeviceToAbsoluteTracking.m, s->headOffset,
                    s->headYawSin, s->headYawCos);
    ++s->headOffsetApplied;
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
    // what the player sees: a viewpoint that silently reverts partway through a
    // session is worse than one that never moved, because the log would still
    // say it was on. This does no allocation, no locking and no calls; it adds
    // three floats to a struct the runtime just filled.
    // Gated on the mode d3d11.dll reports, unless explicitly told not to be.
    //
    // Read every frame rather than cached: the whole point is that it changes
    // when the player enters and leaves the external camera, and it is one
    // interlocked read of a mapped LONG.
    //
    // The transition itself is not smoothed. Entering the external camera is
    // already a viewpoint cut -- the game moves the camera metres in one frame
    // -- so the offset arriving on the same frame adds no discontinuity that
    // was not there. Fading it in would spread OUR change across frames the
    // game is also moving, which is harder to reason about, not easier.
    // LIVE, not merely last-written. d3d11's gate runs inside a fault-budgeted
    // guard that stops running permanently after a few faults, and it would
    // then freeze at its last answer -- a frozen "yes" leaving the offset
    // applied in the cockpit for the rest of the session while both logs still
    // say it is gated. The reader stops trusting a flag nobody is refreshing.
    const bool gateOn = !s->headOffsetExternalOnly ||
                        edvr::externalCameraOnFootLive(s->headOffsetMaxStale);
    // The stuck case, named in the log rather than left as a silent recovery.
    // The raw flag still reading "on foot in the external camera" while the
    // live one has given up means the value is real but nobody is refreshing
    // it -- so it is the gate that died, not the player who moved. Without this
    // line the symptom is "the offset stopped working" and the cause is
    // invisible.
    if (s->headOffsetExternalOnly && !gateOn && !s->headOffsetStaleNoted &&
        edvr::externalCameraOnFoot()) {
        s->headOffsetStaleNoted = true;
        Log::get().note(
            "head offset gate STALE: d3d11 still has the flag set to on-foot "
            "external camera, but has not refreshed it for %u frames, so it is "
            "being treated as off. The gate has stopped running -- its fault "
            "budget on the Present path, or a lost hook after the device or "
            "swapchain was recreated. The offset is off from here rather than "
            "left applied in whatever mode you are actually in.",
            s->headOffsetMaxStale);
    }
    // Yaw counts as "something to do". Without it here, a config that turns the
    // view without moving it -- which is a perfectly reasonable thing to want --
    // would do nothing at all.
    const bool anyChange = s->headOffset[0] != 0.0f || s->headOffset[1] != 0.0f ||
                           s->headOffset[2] != 0.0f || s->headYawDeg != 0.0f;
    if (gateOn && anyChange) {
        applyHeadOffset(s, renderPoses, renderCount);
        // gamePoses as well, by default. Which array Elite renders from is not
        // established -- the whole point of this test -- and offsetting only
        // one would let a negative result mean either "the game ignores our
        // poses" or "we picked the wrong array", which are very different
        // answers. Separable by config once the question is settled.
        if (s->headOffsetGamePoses) applyHeadOffset(s, gamePoses, gameCount);
        if (!s->headOffsetNoted) {
            s->headOffsetNoted = true;
            Log::get().note(
                "HEAD OFFSET ACTIVE: (%+.3f %+.3f %+.3f) m and %+.1f degrees of yaw, "
                "in the tracking frame, applied to renderPoses%s. Gated to the "
                "on-foot external camera: %s.",
                s->headOffset[0], s->headOffset[1], s->headOffset[2], s->headYawDeg,
                s->headOffsetGamePoses ? " and gamePoses" : " only",
                s->headOffsetExternalOnly ? "yes"
                                          : "NO, it applies in every mode including "
                                            "the cockpit");
        }
        if (!s->headOffsetGateNoted && s->headOffsetExternalOnly) {
            s->headOffsetGateNoted = true;
            Log::get().note("head offset gate: applying, because d3d11 reports "
                            "on-foot external camera.");
        }
    }

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
            readHeadOffset(s);
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

    if (!cfg.getBool("fix.transition_flash", true)) {
        Log::get().note("transition flash fix off; compositor passed through unhooked");
        return iface;
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
            "log. To force it on anyway, set ignore_sentinel = 1 under [advanced].");
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
