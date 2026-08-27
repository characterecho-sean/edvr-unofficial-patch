#include "head_offset.h"

#include <cmath>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "pose_offset.h"

namespace edvr {
namespace {

// How long nothing may report the player's mode before saying so. Forty
// seconds -- the figure the note that documents this warning already used.
// Was 3600 frames, which is fifty seconds at 72Hz and thirty at 120, and the
// log line printed the raw 3600 at the reader as if it were meaningful.
constexpr uint64_t kOrphanWarnMs = 40000;

struct State {
    // In the OpenVR tracking frame: +x right, +y up, -z forward.
    float    offset[3] = {};
    // Stored as sin/cos rather than degrees: this runs once per frame on the
    // render thread's critical path, and only changes when the ini does.
    float    yawSin = 0.0f, yawCos = 1.0f;
    float    yawDeg = 0.0f;
    // The values as the PLAYER wrote them, kept for the log. Reporting the
    // internal ones tells somebody their head_offset_forward = 1.25 is
    // "-1.250" and invites them to correct a sign that is already right.
    float    shown[3] = {};

    bool     gamePoses = true;
    bool     externalOnly = true;
    uint32_t maxStale = 90;

    bool     configured = false;
    bool     activeNoted = false;
    bool     staleNoted = false;
    bool     orphanNoted = false;
    uint64_t orphanMs = 0;
    uint32_t applied = 0;
};

State g;

// openvr_min.h declares EVRCompositorError as a bare int32_t rather than
// reproducing the enum, so the success value is named here. 0 is
// VRCompositorError_None in every generation of the interface.
constexpr vr::EVRCompositorError kCompositorOk = 0;

// One axis: reject what is not a number, clamp what is out of range, and report
// both in the units the player typed.
//
// Ten metres, not two. Two was set from "the on-foot target is under a metre",
// which was wrong about the job: the external camera starts several metres
// behind the commander, so reaching the head means travelling that whole
// distance. The user hit the limit at 2.3 while positioning it, and a limit that
// a correct value trips is not a safety limit.
//
// CLAMPED, not zeroed. Refusing to 0 threw away the whole offset, so a value one
// notch too far snapped the viewpoint several metres in one frame -- a large
// involuntary jump for someone wearing the headset, which is exactly what a
// limit protecting them should not cause. It also read as a bug rather than as a
// limit, because from inside a headset there is no log to see. Clamping stops
// the movement at the boundary, which is self-evident without reading anything.
float clampAxis(const char* key, float v) {
    constexpr float kMax = 10.0f;
    // Not a self-compare. NaN fails every ordered test so a min/max clamp passes
    // it through, and a NaN in a transform propagates to every vertex: both eyes
    // go blank with nothing in the log to say why. isfinite also catches INF,
    // which self-comparison does NOT -- that was a real hole here, on the yaw.
    if (!std::isfinite(v)) {
        Log::get().note("%s is not a finite number; using 0.", key);
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

}  // namespace

void headOffsetConfigure() {
    Config& cfg = Config::get();

    // Each key appears TWICE on purpose -- once as the get's argument, once as
    // the name used to report it. check_config_contract requires the literal to
    // be the argument of the get, and that check is what stops a documented
    // setting and a read setting drifting apart. Hiding the keys behind a
    // helper that takes only the name would buy three lines and lose that.
    //
    // THE CODE DEFAULT IS ZERO WHILE THE INI SHIPS REAL VALUES, and that is
    // deliberate rather than the mismatch EDVR-96 was about.
    //
    // The rule "code defaults must equal ini defaults" exists because a partial
    // ini otherwise behaves unlike the file somebody is reading -- and for
    // advanced.head_offset_view that was dangerous, because the code's -1 armed in
    // every camera view while the file said one. The direction of the error is
    // what matters. Here, zero means "do not move anybody", so a missing or
    // partial ini leaves the game exactly as it was. Matching the file instead
    // would move a player's viewpoint using a number they had never seen.
    //
    // FORWARD IS STORED NEGATED, and this is the only place it happens. The
    // tracking frame has -z forward, which is correct and reads backwards to
    // anybody placing a camera: the shipped value is z = -1.25 for a viewpoint
    // 1.25 m in FRONT of where the camera starts.
    g.shown[0] = clampAxis("openvr.head_offset_right",
                          cfg.getFloat("openvr.head_offset_right", 0.0f));
    g.shown[1] = clampAxis("openvr.head_offset_up",
                          cfg.getFloat("openvr.head_offset_up", 0.0f));
    g.shown[2] = clampAxis("openvr.head_offset_forward",
                          cfg.getFloat("openvr.head_offset_forward", 0.0f));
    g.offset[0] = g.shown[0];
    g.offset[1] = g.shown[1];
    g.offset[2] = -g.shown[2];

    // Wrapped rather than clamped. A yaw is periodic, so 190 and -170 are the
    // same heading and neither is a mistake worth refusing -- unlike a
    // translation, where a big number really does put the viewpoint somewhere
    // useless.
    const float rawYaw = cfg.getFloat("openvr.head_yaw_degrees", 0.0f);
    g.yawDeg = std::isfinite(rawYaw) ? fmodf(rawYaw, 360.0f) : 0.0f;
    if (!std::isfinite(rawYaw)) {
        Log::get().note("openvr.head_yaw_degrees is not a finite number; using 0. "
                        "An infinity here becomes a NaN rotation and both eyes go "
                        "black, which is why it is checked rather than clamped.");
    }
    const float rad = g.yawDeg * 3.14159265358979f / 180.0f;
    g.yawSin = sinf(rad);
    g.yawCos = cosf(rad);

    g.gamePoses = cfg.getBool("openvr.head_offset_game_poses", true);
    g.externalOnly = cfg.getBool("openvr.head_offset_external_only", true);
    // Bounded at BOTH ends, which its own comment always claimed. Zero disarms
    // the offset on any frame the two halves do not line up exactly; a huge
    // value turns the guard off without saying so -- and this guard is the only
    // thing standing between a frozen gate and an offset applied in the cockpit
    // for the rest of the session.
    g.maxStale = static_cast<uint32_t>(
        cfg.getIntInRange("openvr.head_offset_max_stale_frames", 90, 2, 900));

    g.configured = true;
    g.activeNoted = false;   // say it again after a change
    g.staleNoted = false;
}

bool headOffsetConfigured() { return g.configured; }

uint32_t headOffsetAppliedCount() { return g.applied; }

bool headOffsetIsSet() {
    return g.offset[0] != 0.0f || g.offset[1] != 0.0f || g.offset[2] != 0.0f ||
           g.yawDeg != 0.0f;
}

namespace {

// An invalid pose is left alone. The runtime marks a pose invalid when it has
// nothing to report -- tracking lost, device asleep -- and the matrix behind it
// is not required to be meaningful. Offsetting a matrix that is not a pose
// produces a pose, which is worse than the flag the game already knows how to
// handle.
void applyTo(vr::TrackedDevicePose_t* poses, uint32_t count) {
    if (!poses || count <= vr::k_unTrackedDeviceIndex_Hmd) return;
    vr::TrackedDevicePose_t& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmd.bPoseIsValid) return;
    applyPoseOffset(hmd.mDeviceToAbsoluteTracking.m, g.offset, g.yawSin, g.yawCos);
    ++g.applied;
}

}  // namespace

void headOffsetApply(vr::EVRCompositorError err,
                     vr::TrackedDevicePose_t* renderPoses, uint32_t renderCount,
                     vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount) {
    const bool anyChange = g.offset[0] != 0.0f || g.offset[1] != 0.0f ||
                           g.offset[2] != 0.0f || g.yawDeg != 0.0f;
    if (!anyChange) return;

    // Only on a successful call. This adds IN PLACE to the array the runtime
    // filled, so a call that left the array untouched -- lost focus, a
    // compositor error -- would get the same metres added on top of last
    // frame's, every frame, and the viewpoint would fly away rather than sit
    // still. The arrays are the caller's, so bPoseIsValid can be a frame stale
    // and cannot be relied on to catch this.
    if (err != kCompositorOk) return;

    // LIVE, not merely last-written. The d3d11 gate runs inside a fault-budgeted
    // guard that stops running permanently after a few faults, and it would then
    // freeze at its last answer -- a frozen "yes" leaving the offset applied in
    // the cockpit for the rest of the session while both logs still say it is
    // gated.
    const bool gateOn = !g.externalOnly || externalCameraOnFootLive(g.maxStale);

    if (g.externalOnly && !gateOn && !g.staleNoted && externalCameraOnFoot()) {
        g.staleNoted = true;
        Log::get().note(
            "head offset gate STALE: d3d11 still has the flag set to on-foot "
            "external camera, but has not refreshed it for %u frames, so it is "
            "being treated as off. The gate has stopped running -- its fault "
            "budget on the Present path, or a lost hook after the device or "
            "swapchain was recreated. The offset is off from here rather than "
            "left applied in whatever mode you are actually in.",
            g.maxStale);
    }

    // Configured, but nothing on the other side is publishing an answer.
    //
    // Silent before this: openvr_api.dll installed without its d3d11 partner,
    // or with fix.head_offset_gate = 0, produces a session where the offset is
    // set up correctly and simply never happens, with no line anywhere saying
    // why. The flash fix already warns about its missing half; this is the same
    // warning for the same shape of install.
    //
    // Gated on NOTHING HAVING EVER PUBLISHED, which is the difference between
    // this warning and a lie. Without that test it fired on every healthy
    // session about forty seconds in, because until the player first enters the
    // camera the gate publishes "no" continuously -- and "publishing no" read
    // identically to "absent" from here. A warning that appears on correct
    // installs is worse than no warning: it trains people to ignore the log.
    if (g.externalOnly && !gateOn && !g.orphanNoted &&
        !externalCameraEverPublished()) {
        if (g.orphanMs == 0) g.orphanMs = stampMs();
        if (elapsedMs(g.orphanMs, kOrphanWarnMs)) {
            g.orphanNoted = true;
            Log::get().note(
                "head offset: configured (%+.2f right, %+.2f up, %+.2f forward) but "
                "nothing has reported the player's mode for %u seconds, so it has "
                "not been applied once. Either d3d11.dll is not installed beside "
                "the game, or fix.head_offset_gate = 0, or you have not been on "
                "foot in the external camera yet.",
                g.shown[0], g.shown[1], g.shown[2],
                (unsigned)(kOrphanWarnMs / 1000));
        }
    }
    if (!gateOn) return;
    g.orphanMs = 0;

    applyTo(renderPoses, renderCount);
    // gamePoses too, unless it is the same array. A caller that passes one
    // buffer for both would otherwise get the offset twice -- double the
    // distance, from a config that looks right.
    if (g.gamePoses && gamePoses != renderPoses) applyTo(gamePoses, gameCount);

    if (!g.activeNoted) {
        g.activeNoted = true;
        // Reported as the player wrote them, WITH the key names. The internal
        // form negates forward, so echoing that prints "-1.250" for a
        // head_offset_forward of 1.25 and invites somebody to correct a sign
        // that is already correct.
        Log::get().note(
            "HEAD OFFSET ACTIVE: head_offset_right %+.3f, head_offset_up %+.3f, "
            "head_offset_forward %+.3f m, head_yaw_degrees %+.1f. Applied to "
            "renderPoses%s. Gated to the on-foot external camera: %s.",
            g.shown[0], g.shown[1], g.shown[2], g.yawDeg,
            g.gamePoses ? " and gamePoses" : " only",
            g.externalOnly ? "yes"
                           : "NO, it applies in every mode including the cockpit");
    }
}

}  // namespace edvr
