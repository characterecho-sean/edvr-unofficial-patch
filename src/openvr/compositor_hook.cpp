#include "compositor_hook.h"

#include "../common/timing.h"
#include "head_offset.h"

#include <windows.h>
#include <d3d11.h>   // GetDesc on the submitted texture, for its size only

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/hotkey.h"
#include "../d3d11/elite_binds.h"   // the camera key, from the game's own bindings
#include "../common/log.h"
#include "../common/proxy.h"  // breadcrumb(), EDVR_BREADCRUMB_ONCE
#include "../common/vtable_hook.h"
#include "guard_crop.h"
#include "openvr_min.h"
#include "resubmit_shadow.h"
#include "system_hook.h"

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

// Time between a diagnostic keypress and the delayed dump it arms. Held equal
// to the d3d11 side's kDumpDelayMs on purpose -- the two logs are read side by
// side, and a different offset in each would mean subtracting a different
// number from each before they could be compared at all.
//
// The equality argument survived the move to a clock; the VALUE is what needed
// it. As 180 frames the two sides were equal in frames and therefore equal in
// time as well, since both count the same frames -- but the "about two seconds"
// both logs printed was 2.5 s at 72Hz and 1.5 at 120, so the label was wrong on
// two of three headsets even though the correlation was right on all of them.
constexpr uint64_t kPoseDumpDelayMs = 2000;

// How often edvr.ini is re-read here. Twin of the d3d11 side's kConfigPollMs;
// both were a bare 90-frame counter described as "about once a second".
constexpr uint64_t kConfigPollMs = 1000;

// How often the submitted eye size is re-read. See the note on eyeSizeNextMs.
constexpr uint64_t kEyeSizeRecheckMs = 6000;

struct State {
    VTableHook       compositorHook;
    // The compositor interface we hooked, as an identity token: compared,
    // never dereferenced. Patching vtable entries in place hooks every object
    // sharing the table, and the runtime can hand out more than one interface
    // pointer over a session. See vtable_hook.h.
    void*            ownerIface = nullptr;
    PFN_Submit       realSubmit = nullptr;
    PFN_WaitGetPoses realWaitGetPoses = nullptr;

    Sentinel* sentinel = nullptr;

    uint64_t configPollMs = 0;

    // Submit's call counter and quiet streak, for the reclaim pass -- the
    // same chainer/bypasser evidence the d3d11 half keeps per thunk (see
    // vScreenReclaimHooks and VTableHook::reclaim). Submit runs twice a frame
    // whenever the game is in VR at all, so a multi-pass silence while
    // WaitGetPoses keeps driving the pass is a starved hook, not a quiet
    // session. WaitGetPoses itself is never vouched: it IS the pass driver,
    // and if it dies the pass dies with it -- the same accepted blind spot as
    // Present on the other side.
    uint32_t submitHits = 0;
    uint8_t  submitQuietPasses = 0;
    bool     submitEverHit = false;
    size_t   submitSlotUsed = 0;

    bool     inert = false;        // hook installed but doing nothing
    bool     validated = false;
    uint32_t submitCalls = 0;
    uint32_t eyesSeen = 0;         // bit 0 left, bit 1 right

    // THE VERDICT FOR A WHOLE FRAME, DECIDED ONCE, AT THE FIRST EYE.
    //
    // Both eyes used to read the flag independently, and the flag legitimately
    // changes mid-frame: the detector re-decides on every new furthest camera
    // and can set, withdraw and re-raise it inside one frame by design. A
    // transition landing between Submit(left) and Submit(right) therefore shows
    // one eye this frame and the other a reprojection of the last one -- a
    // one-frame binocular mismatch, which is exactly what a flash feels like,
    // produced by the fix for flashes.
    //
    // The shared channel carries no frame identity (EDVR-31), so this cannot be
    // done by comparing frame numbers; it is done by sampling once and making
    // the second eye follow. Consistent-late beats split in both directions: two
    // eyes showing the previous frame is a dropped frame, which VR runtimes and
    // people are used to. Two eyes disagreeing is not.
    //
    // Cleared at WaitGetPoses, which is the frame boundary this file already
    // uses for exactly this purpose. The latch itself lives in frame_flag.h,
    // shared, so this file and its private fork cannot drift on it.
    SubmitPairLatch pairLatch;

    // This frame is inside a hold the player asked for by pressing their
    // external-camera key. Taken at WaitGetPoses, which is the frame boundary,
    // so it applies to the NEXT frame's submits -- the same place and the same
    // reasoning as the pair latch's reset.
    bool     holdThisFrame = false;
    bool     threadNoted = false;

    // What has been published to the d3d11 half about the eye textures, and how
    // many submits until the next look.
    //
    // Re-read on a cadence rather than once, because the size is not a property
    // of the session: Elite recreates its eye textures when SteamVR's render
    // resolution changes under it, and a value published once would then name a
    // texture that no longer exists -- which is worse than never publishing,
    // since the reader is being asked to match against it.
    //
    // Every six seconds, and it costs one GetDesc. The comment here used to say
    // "every 600 submits ... roughly every three seconds at 90Hz (two submits a
    // frame)", which stopped being true when the left-eye filter went in ahead
    // of the decrement: only one submit per frame reaches it, so 600 was 600
    // FRAMES, and the code and its comment disagreed by 2x. A clock removes the
    // arithmetic that was getting it wrong.
    uint64_t eyeSizeStampedMs = 0;
    uint32_t eyeSizeW = 0, eyeSizeH = 0;
    uint32_t holdFramesSeen = 0;

    // The per-eye Submit bounds, logged on first sight and on change.
    //
    // Collected for the terrain-culling investigation (72609): any overscan
    // compensation would work by narrowing exactly these bounds, so the fix's
    // design needs their real orientation -- which half of the double-wide
    // texture is which eye, and whether v runs backwards -- from the field
    // rather than from assumption. noteEyeTextureSize beside this one reads
    // the SPAN of the bounds; this records their placement and direction,
    // which the span deliberately discards (fabsf).
    float   boundsLogged[2][4] = {};   // per eye: uMin vMin uMax vMax
    uint8_t boundsState[2] = {};       // 0 never seen, 1 logged null, 2 logged values
    uint8_t boundsLinesLeft = 12;      // a pathological per-frame toggler stays bounded

    // THE POSE RING. Forensics, and only forensics.
    //
    // A tracking glitch -- inside-out feature loss, a lighthouse reflection, a
    // marginal cable, USB power management, an IMU correction -- produces a
    // one-frame wrong pose that is perceptually identical to the game drawing
    // one frame from the wrong place. The runtime does not reliably separate
    // them: bPoseIsValid and eTrackingResult catch gross failures, and small
    // snaps arrive flagged perfectly valid.
    //
    // EDVR CAN ATTRIBUTE THIS AND MUST NEVER TRY TO FIX IT, and the reason is
    // structural rather than cautious. The compositor's display-time
    // reprojection takes its pose from tracking directly, so withholding a frame
    // warps the previous one by the SAME bad pose; and filtering the pose handed
    // to the game would leave render and warp disagreeing, which is worse than
    // the snap. So: no detector, no marking, no withhold is ever keyed on any of
    // this. It records, and the log tells the difference afterwards.
    //
    // Recorded BEFORE headOffsetApply. The instrument measures the HEADSET, not
    // our modification of it -- with Explorer Cam engaged the offset moves the
    // pose by metres, and an instrument that recorded the result would report
    // EDVR's own work as a tracking fault.
    struct PoseEntry {
        uint64_t qpc;
        uint32_t frame;
        float    pos[3];
        int32_t  result;
        uint8_t  valid;
    };
    // TEN SECONDS AT THE FASTEST SUPPORTED RATE, not 900 frames. The whole
    // forensic argument for this ring is wall-clock -- "the window is ten
    // seconds and a player presses the key a second or two after seeing
    // something" -- and 900 frames delivered 7.5 s of it at 120Hz. Sized so
    // ten seconds is the floor at every rate. Six bare 900s used to be spelled
    // out at the wrap sites; they are this constant now, because the twin in
    // glitch_frame had a name and this one did not.
    static constexpr uint64_t kPoseRingFrames = 1200;
    PoseEntry poseRing[kPoseRingFrames] = {};
    uint64_t  poseHead = 0;
    uint32_t  poseFrame = 0;
    float     poseStepNoteMm = 50.0f;
    Hotkey    poseDumpKey;
    bool      poseKeyBound = false;
    // The external-camera key, watched on THIS side too.
    //
    // The camera history is dumped by two keys -- the history key and, when the
    // player is chasing a transition, their own camera key. The pose history had
    // only the first, so a session spent pressing the camera key produced four
    // camera histories and no pose histories at all, which is a diagnostic that
    // is present exactly when nobody is looking for it.
    //
    // Watched here rather than signalled across the shared block for the same
    // reason the history key is: a channel for a diagnostic keypress is more
    // machinery than the thing it carries.
    Hotkey    poseCamKey;
    bool      poseCamBound = false;
    bool      poseDumpOnCam = false;
    uint64_t  poseDumpDueMs = 0;   // 0 = no delayed dump armed

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

// Read at install AND on reload, because a threshold you tune by looking at logs
// is one you will want to change without relaunching.
void configurePoseRing(State* s) {
    if (!s) return;
    Config& cfg = Config::get();
    s->poseDumpKey.setBinding(cfg.getString("hotkey.dump_camera", "PAUSE").c_str());
    s->poseKeyBound = s->poseDumpKey.key() != 0;
    const float mm = cfg.getFloat("advanced.pose_step_note_mm", 50.0f);
    // Note-only, so a wrong value costs a misleading line rather than behaviour.
    // Clamped anyway: 0 would flag every frame and drown the verdict it exists
    // to make readable.
    s->poseStepNoteMm = (std::isfinite(mm) && mm > 0.0f && mm <= 1000.0f) ? mm : 50.0f;

    // The camera key comes from the GAME's bindings, like everywhere else
    // (0.7.1 retired the ini keys). ONCE, not on every reload: this runs on
    // the ~1 Hz config reload, and a directory walk per second to keep a
    // default-off diagnostic in step is not a trade worth making. Stated
    // limit: rebinding mid-session moves the camera ring's trigger (the
    // d3d11 side follows live) but not this pose ring's until a restart.
    static bool camKeyRead = false;
    if (!camKeyRead) {
        camKeyRead = true;
        char b[48];
        if (eliteBindsLookup("PhotoCameraToggle_Humanoid", b, sizeof(b),
                             "PhotoCameraToggle")) {
            s->poseCamKey.setBinding(b);
        }
        // The game's key, not ours: no focus filter (see hotkey.h). The
        // history key beside it is EDVR's own and keeps the filter.
        s->poseCamKey.setGameMirrored(true);
        s->poseCamBound = s->poseCamKey.key() != 0;
    }
    s->poseDumpOnCam = cfg.getBool("advanced.dump_camera_on_external_cam", false);

    // SAID OUT LOUD, once, because an instrument that records silently and
    // dumps on a key is indistinguishable from one that is not there. A whole
    // stage-0 run produced four camera histories and no pose histories, and
    // nothing in either log said why.
    static bool announced = false;
    if (!announced) {
        announced = true;
        Log::get().note(
            "headset pose history armed: recording where the runtime says your "
            "headset is, every frame, before any EDVR offset. Written by %s%s, "
            "alongside the viewpoint history in the other log. Nothing is "
            "withheld or changed because of it -- a bad pose from tracking is "
            "not something this can fix, only tell apart from the game's.",
            s->poseKeyBound ? "the history key" : "NO history key (unbound)",
            s->poseCamBound && s->poseDumpOnCam ? " and your external-camera key"
                                                : "");
    }
}

// The pose history, and the verdict that makes it worth having.
//
// The three-way discrimination is the point: a game camera jump, an HMD pose
// jump, a stall, and an EDVR withhold all feel identical in a headset and are
// completely different bugs. The camera ring on the d3d11 side answers the first
// and the last; this answers the second and the third, in the same capture.
//
// No threshold here decides anything. The step figure is a NOTE -- it names the
// largest one-frame movement in the window so a reader can judge it, and nothing
// in EDVR behaves differently because of it.
void dumpPoseRing(State* s, const char* trigger, uint32_t msAfterPress) {
    if (!s || s->poseHead == 0) return;
    const uint64_t have = s->poseHead < State::kPoseRingFrames ? s->poseHead : State::kPoseRingFrames;
    const uint64_t first = s->poseHead - have;
    const int64_t freq = qpcFrequency();
    const uint64_t newest = s->poseRing[(s->poseHead - 1) % State::kPoseRingFrames].qpc;

    Log::get().note(
        "--- headset pose history: %llu frames, oldest first, written on %s. "
        "ZERO MILLISECONDS IS %s -- the reaction time between seeing something "
        "and reaching a key is what this column exists to let you subtract. "
        "Columns are milliseconds before that point, frame number, the headset "
        "position the RUNTIME reported (before any EDVR offset), how far it "
        "moved since the frame before, and the tracking state. ---",
        static_cast<unsigned long long>(have), trigger,
        msAfterPress == 0
            ? "the moment you pressed"
            : "about two seconds AFTER the press, so the press is further back");

    // THE MOST RECENT qualifying step, not the largest anywhere in the window.
    //
    // The window is ten seconds and a player presses the key a second or two
    // after seeing something. Reporting the largest step meant a 150 mm recentre
    // seven seconds earlier outranked everything near the press and read as the
    // answer -- it is a bystander, and the line said nothing about when it was.
    // Recency is the property that matters here; size is on every row already.
    float    lastStepMm = 0.0f;
    uint32_t lastStepFrame = 0;
    double   lastStepMsAgo = 0.0;
    uint32_t qualifying = 0;
    float    biggestMm = 0.0f;
    uint32_t badStates = 0;
    const float* prev = nullptr;

    for (uint64_t i = first; i < s->poseHead; ++i) {
        const State::PoseEntry& e = s->poseRing[i % State::kPoseRingFrames];
        float stepMm = 0.0f;
        if (prev) {
            float d2 = 0.0f;
            for (uint32_t a = 0; a < 3; ++a) {
                const float d = e.pos[a] - prev[a];
                d2 += d * d;
            }
            stepMm = sqrtf(d2) * 1000.0f;
            if (stepMm > biggestMm) biggestMm = stepMm;
        }
        if (!e.valid || e.result != vr::TrackingResult_Running_OK) ++badStates;
        const double msAgo =
            freq ? static_cast<double>(static_cast<int64_t>(newest - e.qpc)) * 1000.0 /
                       static_cast<double>(freq)
                 : 0.0;
        // Oldest first, so the last one to pass is the most recent one.
        if (stepMm > s->poseStepNoteMm) {
            ++qualifying;
            lastStepMm = stepMm;
            lastStepFrame = e.frame;
            lastStepMsAgo = msAgo;
        }
        Log::get().note("HMD %8.1fms f%-7u pos=(%+.3f %+.3f %+.3f) step=%6.1fmm %s%s",
                        -msAgo, e.frame, e.pos[0], e.pos[1], e.pos[2], stepMm,
                        e.valid ? "" : "POSE-INVALID ",
                        e.result == vr::TrackingResult_Running_OK
                            ? "" : "TRACKING-NOT-OK");
        prev = e.pos;
    }

    // THE VERDICT. Two numbers and a count, so the next "was that EDVR?" session
    // does not have to be reconstructed by hand from three log files.
    if (qualifying > 0) {
        // DID IT COME BACK? The same question the camera side has always asked,
        // and for the same reason: a step that RETURNS is a one-frame tracking
        // fault, and a step that STAYS is the runtime re-establishing where zero
        // is. Those are completely different events and only one of them is a
        // fault -- calling a recentre a tracking glitch would send somebody after
        // their base stations over something working as designed.
        //
        // Measured within a frame of writing this: a 172 mm step to (0.001,
        // -0.001, -0.001) that stayed there, with the head moving sub-millimetre
        // either side. A recentre at a mode change, not a glitch.
        bool returned = false;
        {
            const float* before = nullptr;
            uint32_t seen = 0;
            for (uint64_t i = first; i < s->poseHead; ++i) {
                const State::PoseEntry& e = s->poseRing[i % State::kPoseRingFrames];
                if (e.frame == lastStepFrame) { seen = 1; continue; }
                if (!seen) { before = e.pos; continue; }
                if (++seen > 11) break;
                if (before) {
                    float d2 = 0.0f;
                    for (uint32_t a = 0; a < 3; ++a) {
                        const float d = e.pos[a] - before[a];
                        d2 += d * d;
                    }
                    if (sqrtf(d2) * 1000.0f <= s->poseStepNoteMm) { returned = true; break; }
                }
            }
        }
        if (returned) {
            Log::get().note(
                "--- the headset moved %.1f mm in ONE frame at f%u, %.0f ms before this "
                "was written, and came BACK. A head does not travel that far and "
                "return, so that frame's viewpoint came from the TRACKING and not "
                "from the game. EDVR cannot fix it -- the compositor reprojects from "
                "the same tracking data -- but it is a different bug from the game "
                "drawing a bad frame, and the two are identical from inside a "
                "headset. ---",
                lastStepMm, lastStepFrame, lastStepMsAgo);
        } else {
            Log::get().note(
                "--- the reported headset position moved %.1f mm in one frame at f%u, "
                "%.0f ms before this was written, and STAYED there. That is the runtime "
                "re-establishing where zero is, not a tracking fault -- Elite does it "
                "at some mode changes. It still moves your viewpoint, and EDVR neither "
                "causes it nor can prevent it, but nothing is wrong with your tracking. ---",
                lastStepMm, lastStepFrame, lastStepMsAgo);
        }
    } else {
        Log::get().note(
            "--- the largest one-frame headset movement anywhere in this window is "
            "%.1f mm, under the %.0f mm worth noting, so tracking looks clean "
            "across it. Anything seen came from somewhere else. ---",
            biggestMm, s->poseStepNoteMm);
    }
    if (badStates > 0) {
        Log::get().note(
            "--- %u of those frames had an invalid pose or a tracking state other "
            "than running. Those are the ones the runtime knew about; the "
            "millimetre column is for the ones it did not. ---",
            badStates);
    }
    Log::get().note("--- end headset pose history ---");
}

// Tell the d3d11 half how big the headset's eye textures are.
//
// This side is handed the texture; the other side was left guessing at it from
// dimensions alone, and paid for the guess twice (see frame_flag.h). One
// GetDesc every few seconds, through the guard, on a handle that belongs to the
// game -- a stale one costs a fault entry and a session without the answer,
// which is exactly the state every build before this one was in.
//
// Nothing is submitted, copied or changed here. It reads a size and writes it
// into EDVR's own channel.
void noteEyeTextureSize(State* s, vr::EVREye eye, const vr::Texture_t* texture,
                        const vr::VRTextureBounds_t* bounds) {
    if (!s->validated) return;
    if (!texture || texture->eType != vr::TextureType_DirectX || !texture->handle) {
        return;
    }
    // ONE EYE ONLY. The two eyes carry different bounds -- that is how a
    // double-wide texture names them -- so sampling whichever Submit happens
    // to land on the cadence alternates between two answers, and every
    // alternation looks like the render resolution moving. Left is arbitrary
    // and consistent, which is all this needs.
    if (eye != vr::Eye_Left) return;
    // The FIRST read must happen immediately, which is why this is not a bare
    // elapsedMs: a zero stamp means "never sampled", and elapsedMs answers
    // false for that by design. The countdown this replaced started at zero
    // meaning "due now", so reading the sentinel the other way round would
    // have deferred the very first sample forever.
    if (s->eyeSizeStampedMs != 0 &&
        !elapsedMs(s->eyeSizeStampedMs, kEyeSizeRecheckMs)) {
        return;
    }
    // Re-armed HERE, before any of the early returns below. Setting it after
    // them meant the steady state -- size unchanged, the common case -- left
    // it unstamped, so every Submit from then on paid a guarded GetDesc: 180 a
    // second, for a value this exists to sample every six.
    s->eyeSizeStampedMs = stampMs();

    D3D11_TEXTURE2D_DESC desc = {};
    bool ok = false;
    guarded("vr: eye texture desc", [&] {
        static_cast<ID3D11Texture2D*>(texture->handle)->GetDesc(&desc);
        ok = true;
    });
    if (!ok || !desc.Width || !desc.Height) return;

    // THE BOUNDS ARE THE HALF OF THIS THAT MATTERS, and leaving them out is
    // what made the first version of this useless on the hardware it was
    // written for.
    //
    // Elite submits ONE double-wide texture holding both eyes and names each
    // eye by its bounds -- measured 2026-08-17 on a Quest 3 over Steam Link:
    // the submitted texture is 2912x1560 while the render targets the scene
    // is actually drawn into are 1456x1560, exactly half the width. Publishing
    // the whole texture told the other half to look for something twice as
    // wide as anything that exists, so it matched nothing at all and the black
    // void stayed grey with the answer, 1456x1560, sitting in its own list of
    // sizes it had seen and rejected.
    //
    // bounds may legitimately be null, which means the whole texture is the
    // eye. A span that is not a sane fraction is treated the same way rather
    // than trusted: this is somebody else's struct and the cost of believing
    // a bad one is publishing a size nothing can match.
    //
    // Absolute value, because OpenVR permits the bounds to run backwards:
    // vMin=1, vMax=0 is the ordinary way to say the texture's origin is
    // flipped, and it is common. A signed span fails the sanity test below,
    // leaves the multiplier at 1.0, and publishes the whole double-wide
    // width -- which matches nothing, kills all four fixes, and looks
    // exactly like the bug this function was written to end.
    float uSpan = 1.0f, vSpan = 1.0f;
    if (bounds) {
        const float u = fabsf(bounds->uMax - bounds->uMin);
        const float v = fabsf(bounds->vMax - bounds->vMin);
        if (u > 0.01f && u <= 1.0f) uSpan = u;
        if (v > 0.01f && v <= 1.0f) vSpan = v;
    }
    const uint32_t eyeW =
        static_cast<uint32_t>(static_cast<float>(desc.Width) * uSpan + 0.5f);
    const uint32_t eyeH =
        static_cast<uint32_t>(static_cast<float>(desc.Height) * vSpan + 0.5f);
    if (!eyeW || !eyeH) return;
    if (eyeW == s->eyeSizeW && eyeH == s->eyeSizeH) return;

    const bool first = (s->eyeSizeW == 0);
    const bool shared = (eyeW != desc.Width || eyeH != desc.Height);
    // Published BEFORE the cache is committed, so a refusal inside the
    // channel is retried on the next sample instead of being remembered as
    // done. The cache is what suppresses the log, not what proves it landed.
    announceEyeTextureSize(eyeW, eyeH);
    s->eyeSizeW = eyeW;
    s->eyeSizeH = eyeH;
    // Both halves want this in their own log: this one is where it was read,
    // and the d3d11 log is where the consequences are. Whichever log a report
    // arrives with, the size is in it.
    Log::get().note(
        "compositor: ONE EYE is %ux%u%s. Told the d3d11 half, which uses it to "
        "tell your eye textures from everything else it sees. The submitted "
        "texture is %ux%u%s.%s",
        eyeW, eyeH,
        first ? "" : " -- this CHANGED, so the render resolution moved under "
                     "the game",
        desc.Width, desc.Height,
        shared ? " and holds BOTH eyes, so the per-eye size above is that "
                 "texture narrowed by the bounds this Submit named"
               : ", one eye per texture",
        shared ? " Watch for the per-eye size in the graphics log, not this one."
               : "");
}

// Where the game says each eye lives in the submitted texture. One line per
// eye per distinct answer, because the values are design inputs, not events:
// a session's worth of identical bounds is one line.
void noteSubmitBounds(State* s, vr::EVREye eye, const vr::VRTextureBounds_t* bounds) {
    if (!s->validated || s->boundsLinesLeft == 0) return;
    const int e = (eye == vr::Eye_Left) ? 0 : 1;
    const char* name = e == 0 ? "left" : "right";

    if (!bounds) {
        if (s->boundsState[e] == 1) return;
        s->boundsState[e] = 1;
        --s->boundsLinesLeft;
        Log::get().note("Submit bounds (%s eye): null -- the whole texture is "
                        "this eye", name);
        return;
    }

    float v[4] = {};
    if (!guarded("noteSubmitBounds/read", [&] {
            v[0] = bounds->uMin;
            v[1] = bounds->vMin;
            v[2] = bounds->uMax;
            v[3] = bounds->vMax;
        })) {
        return;
    }
    if (s->boundsState[e] == 2) {
        bool moved = false;
        for (int i = 0; i < 4; ++i) {
            if (fabsf(v[i] - s->boundsLogged[e][i]) > 1e-4f) { moved = true; break; }
        }
        if (!moved) return;
    }
    const bool wasLogged = s->boundsState[e] == 2;
    s->boundsState[e] = 2;
    memcpy(s->boundsLogged[e], v, sizeof(v));
    --s->boundsLinesLeft;
    Log::get().note(
        "Submit bounds (%s eye): u %.4f..%.4f, v %.4f..%.4f%s%s%s",
        name, v[0], v[2], v[1], v[3],
        v[0] > v[2] ? " (u runs BACKWARDS)" : "",
        v[1] > v[3] ? " (v runs backwards: flipped origin, and OpenVR permits "
                      "it)" : "",
        wasLogged ? " -- CHANGED" : "");
}

vr::EVRCompositorError hookedSubmit(void* self, vr::EVREye eye,
                                    const vr::Texture_t* texture,
                                    const vr::VRTextureBounds_t* bounds,
                                    vr::EVRSubmitFlags flags) {
    EDVR_BREADCRUMB_ONCE("vr: hookedSubmit entered");
    State* s = g_state;
    if (!s || !s->realSubmit) return 0;
    // Before the owner test, like the d3d11 counters: raw invocation is the
    // reclaim evidence, whoever the caller was.
    ++s->submitHits;

    // Not the interface we attached to: forward untouched. In-place patching
    // hooks the class, so a second compositor pointer reaches this thunk and
    // withholding ITS frames would be acting on somebody else's submission.
    if (self != s->ownerIface) {
        return s->realSubmit(self, eye, texture, bounds, flags);
    }
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

    // Before any decision about this frame, and unconditional on all of them:
    // it is an observation about the texture's shape, and the half that needs
    // it is starved without it whether or not this frame is withheld.
    //
    // BOTH notes see the game's ORIGINAL bounds, and the order with the crop
    // below is load-bearing: the d3d11 half matches eye textures by the size
    // noteEyeTextureSize publishes, and the render target has not changed
    // size just because the compositor will sample less of it.
    noteEyeTextureSize(s, eye, texture, bounds);
    noteSubmitBounds(s, eye, bounds);

    // The guard's stage-1 adoption probe: while the game is being asked for
    // bigger render targets, report every submission's size so stage 2 can
    // wait for both eyes to arrive at the new one. Per-submit GetDesc, but
    // only during the transition -- stage 1 lasts seconds, and outside it
    // this is one flag test.
    if (systemHookSizeProbeWanted() && texture &&
        texture->eType == vr::TextureType_DirectX && texture->handle) {
        D3D11_TEXTURE2D_DESC pd{};
        bool ok = false;
        guarded("vr: size probe", [&] {
            static_cast<ID3D11Texture2D*>(texture->handle)->GetDesc(&pd);
            ok = true;
        });
        if (ok && pd.Width && pd.Height) {
            float uSpan = 1.0f, vSpan = 1.0f;
            if (bounds) {
                const float u = fabsf(bounds->uMax - bounds->uMin);
                const float v = fabsf(bounds->vMax - bounds->vMin);
                if (u > 0.01f && u <= 1.0f) uSpan = u;
                if (v > 0.01f && v <= 1.0f) vSpan = v;
            }
            systemHookNoteSubmittedSize(
                eye,
                static_cast<uint32_t>(pd.Width * uSpan + 0.5f),
                static_cast<uint32_t>(pd.Height * vSpan + 0.5f));
        }
    }

    // The cull guard's submit-side half: when the projection lie is live,
    // the compositor must be handed exactly the region holding the TRUE
    // frustum out of the wider-rendered image. Two mechanisms (see
    // guard_crop.h for why the copy is the default), and whichever runs, it
    // is applied identically to EVERY submit path below -- forwarding a
    // withheld frame's substitute unguarded would distort one frame, which
    // is exactly the kind of one-frame artifact this project exists to
    // remove.
    //
    // In copy mode the guard is applied where the SOURCE texture is finally
    // known (the forward and withhold paths below), through applyCullGuard;
    // in bounds mode narrowing the bounds once here covers both.
    float guardFractions[4];
    const bool guardLive = systemHookCropFractions(eye, guardFractions);
    const bool guardCopies = guardLive && systemHookSubmitCopyMode();
    vr::VRTextureBounds_t croppedBounds;
    const vr::VRTextureBounds_t* effBounds = bounds;
    if (guardLive && !guardCopies &&
        systemHookCropBounds(eye, bounds, &croppedBounds)) {
        effBounds = &croppedBounds;
    }
    // Applies the copy mechanism to whatever texture a path is about to
    // submit. On failure the guard stands down and the ORIGINAL texture and
    // bounds go through -- one wide-rendered frame displayed plainly, then
    // truth from the next boundary; never a mismatched crop.
    auto applyCullGuard = [&](vr::Texture_t* tex,
                              const vr::VRTextureBounds_t** bnds,
                              vr::VRTextureBounds_t* storage) {
        if (!guardCopies) return;
        if (tex->eType != vr::TextureType_DirectX) {
            systemHookGuardStandDown("the game is not submitting DirectX "
                                     "textures, which the copy path needs");
            return;
        }
        uint32_t snapW = 0, snapH = 0;
        systemHookCropTarget(eye, &snapW, &snapH);
        void* out = guardCropCopy(eye == vr::Eye_Left ? 0u : 1u, tex->handle,
                                  bounds, guardFractions, snapW, snapH,
                                  storage);
        if (!out) {
            systemHookGuardStandDown("the crop copy refused (its own log line "
                                     "above says why)");
            return;
        }
        tex->handle = out;
        *bnds = storage;
    };

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
    // Sampled once per frame, at whichever eye arrives first. See SubmitPairLatch.
    //
    // The hold is ORed in at the same sample, so it goes through the same latch
    // and both eyes agree about it too. A hold that split a pair would be the
    // one-eye-behind-the-other failure the latch exists to prevent, arriving by
    // a different door.
    // THREAD IDENTITY, logged once, and it gates a design rather than debugging
    // one. 1f proposes replacing a withheld Submit with a copy of the last
    // forwarded frame, which means calling CopyResource on the game's immediate
    // context from this callsite -- and D3D11 immediate contexts are single
    // threaded. The measured call order (Submit, Submit, Present, WaitGetPoses)
    // implies this is the render thread, but implies is not verified: if this id
    // and the one Present logs differ, the design is dead as written and the
    // deferred-copy variant needs its own spec. Verify, do not assume.
    EDVR_BREADCRUMB_ONCE("vr: submit thread");
    if (!s->threadNoted) {
        s->threadNoted = true;
        Log::get().note(
            "compositor: Submit is running on thread %lu. Compare with the "
            "Present thread in the d3d11 log -- if they differ, anything that "
            "touches the immediate context from here is unsafe.",
            static_cast<unsigned long>(GetCurrentThreadId()));
    }

    if (s->validated && s->pairLatch.verdict(glitchFrameMarked() || s->holdThisFrame)) {
        ++s->framesWithheld;
        // 1f: hand SteamVR the game's own previous frame instead of a missed
        // deadline. The copy is EDVR's, refreshed only from frames that were
        // actually forwarded -- never from this one -- so what goes out is the
        // last thing the player was already shown, on time, at the current
        // pose. The thread gate for the copy machinery PASSED 2026-08-15
        // (Submit and Present both on thread 3108). Whenever no acceptable
        // copy exists, the classic withhold below still happens: this path
        // can never be worse than the fix was without it.
        void* shadow = nullptr;
        if (texture && texture->eType == vr::TextureType_DirectX) {
            shadow = resubmitShadowForWithhold(eye == vr::Eye_Left ? 0u : 1u,
                                               texture->handle);
        }
        if (shadow) {
            if (s->notesLeft > 0) {
                --s->notesLeft;
                Log::get().note(
                    "transition flash: frame replaced with the previous frame's "
                    "copy (eye %d), submitted on time -- no reprojection stall. "
                    "%u eye-submits withheld so far -- two per frame, so half "
                    "this many frames.",
                    static_cast<int>(eye), s->framesWithheld);
            }
            vr::Texture_t sub = *texture;
            sub.handle = shadow;
            const vr::VRTextureBounds_t* subBounds = effBounds;
            vr::VRTextureBounds_t subStorage;
            applyCullGuard(&sub, &subBounds, &subStorage);
            return s->realSubmit(self, eye, &sub, subBounds, flags);
        }
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

    {
        vr::Texture_t fwd = *texture;
        const vr::VRTextureBounds_t* fwdBounds = effBounds;
        vr::VRTextureBounds_t fwdStorage;
        // Snapshot submission (experimental.submit_snapshot): refresh this
        // eye's copy NOW -- the CopyResource lands on the immediate context
        // behind everything this frame drew -- and hand the compositor the
        // copy instead of the live texture. Elite reuses one texture per eye
        // with no fence, so the compositor's read races the GPU per eye and
        // can catch the first eye finished while the second is still
        // drawing; two copies enqueued back to back here are latched at the
        // same point instead. Only on a copy that landed THIS call: a stale
        // copy for one eye is the asymmetry this mode exists to remove, so
        // any refusal falls through to the live texture, both eyes alike.
        //
        // BEFORE the cull guard on purpose: in copy mode the guard crops
        // from fwd.handle, and cropping from the snapshot keeps the one
        // latch point.
        bool snapped = false;
        const uint32_t eyeIdx = eye == vr::Eye_Left ? 0u : 1u;
        if (s->validated && resubmitShadowSnapshotWanted() && texture &&
            texture->eType == vr::TextureType_DirectX && texture->handle &&
            resubmitShadowNoteForwarded(eyeIdx, texture->handle)) {
            void* snap = resubmitShadowCurrent(eyeIdx);
            if (snap) {
                fwd.handle = snap;
                snapped = true;
            }
        }
        applyCullGuard(&fwd, &fwdBounds, &fwdStorage);
        const vr::EVRCompositorError result =
            s->realSubmit(self, eye, &fwd, fwdBounds, flags);
        // This frame was FORWARDED and accepted, so it becomes the copy a
        // later withhold can hand over. After realSubmit on purpose: the copy
        // is queued on the immediate context behind this frame's rendering,
        // and ordering on the context is what guarantees the copy holds the
        // completed frame (the same reasoning the private build's luminance
        // sampling rests on). The GAME's texture, not the guard's crop: the
        // shadow must hold the full frame so a withhold can crop it afresh.
        //
        // In snapshot mode the refresh already happened above, pre-submit,
        // and doing it again would be a second copy of the same frame. The
        // one semantic shift snapshot mode accepts: its refresh is not gated
        // on realSubmit succeeding, so a rejected submit can leave the
        // shadow holding a completed frame the compositor never showed --
        // still a rendered frame, and a later withhold handing it over is
        // indistinguishable from the classic case in a headset.
        if (!snapped && s->validated && result == 0 && texture &&
            texture->eType == vr::TextureType_DirectX && texture->handle) {
            resubmitShadowNoteForwarded(eyeIdx, texture->handle);
        }
        return result;
    }
}





vr::EVRCompositorError hookedWaitGetPoses(void* self,
                                          vr::TrackedDevicePose_t* renderPoses,
                                          uint32_t renderCount,
                                          vr::TrackedDevicePose_t* gamePoses,
                                          uint32_t gameCount) {
    State* s = g_state;
    if (!s || !s->realWaitGetPoses) return 0;
    if (self != s->ownerIface) {
        return s->realWaitGetPoses(self, renderPoses, renderCount, gamePoses,
                                   gameCount);
    }

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
    // ...and where the next frame's pair verdict starts undecided.
    s->pairLatch.reset();

    // One frame off the hold, if one is running. Consumed here rather than at
    // Submit because Submit runs twice a frame and this is a count of FRAMES.
    const bool wasHolding = s->holdThisFrame;
    s->holdThisFrame = takeSubmitHoldFrame();
    if (s->holdThisFrame) {
        ++s->holdFramesSeen;
    } else if (wasHolding) {
        Log::get().note(
            "transition hold: %u frame(s) held across the transition you asked "
            "for with the external-camera key. If that looked like a freeze "
            "rather than a steady image, lower hold_frames_on_external_cam; if "
            "the wrong viewpoint still showed, raise it. The camera history's "
            "millisecond column is what says which.",
            s->holdFramesSeen);
        s->holdFramesSeen = 0;
    }

    // The pose, recorded BEFORE the offset below touches it. See PoseEntry.
    if (result == 0 && renderPoses && renderCount > vr::k_unTrackedDeviceIndex_Hmd) {
        guarded("poseRing/record", [&] {
            const vr::TrackedDevicePose_t& hmd =
                renderPoses[vr::k_unTrackedDeviceIndex_Hmd];
            State::PoseEntry& e = s->poseRing[s->poseHead % State::kPoseRingFrames];
            e.qpc = static_cast<uint64_t>(qpcNow());
            e.frame = ++s->poseFrame;
            // Row-major 3x4: the translation is the last column.
            e.pos[0] = hmd.mDeviceToAbsoluteTracking.m[0][3];
            e.pos[1] = hmd.mDeviceToAbsoluteTracking.m[1][3];
            e.pos[2] = hmd.mDeviceToAbsoluteTracking.m[2][3];
            e.result = static_cast<int32_t>(hmd.eTrackingResult);
            e.valid = hmd.bPoseIsValid ? 1u : 0u;
            ++s->poseHead;

            // Ship-forward in the current head frame, for the sprite-pinning
            // fix on the d3d11 side. World-forward is seated -Z; its
            // head-local direction is R-transpose times that, which for a
            // row-major rotation is the negated third ROW. Published every
            // frame from the same raw pose the ring records -- before the
            // head offset touches it, so Explorer Cam does not steer the
            // pin. Rotation only, and that is correct: the pinned sprite
            // stands for a star at infinity, and infinity has no parallax.
            if (hmd.bPoseIsValid) {
                const auto& m = hmd.mDeviceToAbsoluteTracking.m;
                const float dx = -m[2][0], dy = -m[2][1];
                float fwd = m[2][2];   // == -(d.z); 1 facing forward
                if (fwd < 0.05f) fwd = 0.05f;
                announceHeadForward(dx / fwd, dy / fwd);
            }
        });
    }

    // The same key that dumps the camera history on the other side, polled here
    // too, so one press produces both halves of the picture in their own logs.
    //
    // Polled rather than signalled across the shared block: a second channel for
    // a diagnostic keypress would be more machinery than the thing it carries,
    // and the two logs are read together anyway.
    // The history key dumps twice, and this ring needs the second one MORE than
    // the camera ring does.
    //
    // The verdict here distinguishes a tracking fault from a re-origin by asking
    // whether the headset RETURNED to where it was or stayed put -- a question
    // answered entirely by the frames AFTER the step. A step in the last rows of
    // the ring has no after, so the one capture certain to hold the event is the
    // one where the test cannot run. Two seconds later it has frames on both
    // sides and the question is answerable.
    if (s->poseKeyBound && s->poseDumpKey.pressed()) {
        dumpPoseRing(s, "the history key", 0);
        s->poseDumpDueMs = nowMs() + kPoseDumpDelayMs;
    }
    // The camera key arms the same delay, and did first: the ring holds the
    // frames BEFORE it is written, so taking it on the press would capture the
    // approach and none of the transition.
    if (s->poseCamBound && s->poseDumpOnCam && s->poseCamKey.pressed()) {
        s->poseDumpDueMs = nowMs() + kPoseDumpDelayMs;
    }
    if (s->poseDumpDueMs != 0 && nowMs() >= s->poseDumpDueMs) {
        s->poseDumpDueMs = 0;
        dumpPoseRing(s, "a key you pressed two seconds ago",
                     (uint32_t)(kPoseDumpDelayMs / 1000));
    }

    // The head offset, applied BEFORE the game sees the poses.
    //
    // Outside the telemetry guard below on purpose. That guard has a fault
    // budget and disables itself after five faults, which is right for logging
    // -- losing a log line is nothing -- but wrong for something that changes
    // what the player sees.
    //
    // The feature lives in head_offset.cpp. It was written inline here, back
    // when this file was hand-copied between two repos, and this copy lost
    // the install-time config read -- so this build ran every session with
    // the offsets still zero and did nothing at all. One file, one call, is
    // what makes that failure impossible rather than merely unlikely.
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
    if (dueMs(s->configPollMs, kConfigPollMs)) {
        s->configPollMs = stampMs();
        if (Config::get().reloadIfChanged()) {
            Log::get().note("config reloaded");
            headOffsetConfigure();
            configurePoseRing(s);
            // The cull guard's margin is tuned from inside a headset, so its
            // keys are live; mode changes take effect at the next boundary.
            systemHookConfigure();
            // The snapshot toggle is an in-headset A/B experiment, so it is
            // live too; the flip logs its own receipt.
            resubmitShadowConfigure();
        }
        // The liveness pass, same cadence and same reason as the d3d11 half:
        // in-place patches sit on a table other tools can write, and under
        // OpenComposite the compositor being hooked here IS another mod's
        // object -- the environment where a second hooker is normal rather
        // than exotic. Submit is vouched only on measured silence (see the
        // State fields); WaitGetPoses is detection-only, and riding it has
        // the same accepted blind spot as riding Present over there: a
        // re-point of THIS slot kills the check with the hook. Sessions that
        // went inert never reach here on purpose -- inert thunks are pure
        // pass-throughs, and winning a slot back for one buys nothing.
        {
            size_t quiet[1];
            size_t n = 0;
            if (s->submitHits == 0) {
                if (s->submitEverHit && s->submitQuietPasses < 255) {
                    ++s->submitQuietPasses;
                }
            } else {
                s->submitEverHit = true;
                s->submitQuietPasses = 0;
                s->submitHits = 0;
            }
            if (s->submitEverHit && s->submitQuietPasses >= 3) {
                quiet[n++] = s->submitSlotUsed;
            }
            s->compositorHook.reclaim("openvr compositor", quiet, n);
        }
    }

    // The system hook's frame boundary: the cull guard's lie switches on and
    // off HERE, after the game is released from WaitGetPoses and before it
    // queries this frame's projections -- the one point where every answer
    // in the coming frame, and the submit crop at its end, can be made to
    // tell one story. Deferred log emission rides along.
    systemHookFrameBoundary();

    return result;
}

}  // namespace

void* interceptInterface(void* iface, const char* interfaceVersion) {
    if (!iface || !interfaceVersion) return iface;

    Config& cfg = Config::get();
    Log::get().note("VR_GetGenericInterface(\"%s\")", interfaceVersion);

    if (!g_state) g_state = new State();
    State& s = *g_state;

    // The system interface is observed, never modified -- see system_hook.cpp
    // (the terrain-culling investigation, frontier issue 72609). Dispatched
    // before the compositor test because both interfaces arrive through this
    // one wrapper.
    if (strncmp(interfaceVersion, "IVRSystem_", 10) == 0) {
        maybeObserveSystemInterface(iface, interfaceVersion);
        return iface;
    }

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
    s.ownerIface = iface;
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
    // The reclaim pass needs the slot number Submit actually landed on --
    // it is resolved per interface version above, not a constant.
    s.submitSlotUsed = submitSlot;
    if (!s.compositorHook.commit()) {
        Log::get().note("compositor vtable commit failed; passing through");
        s.compositorHook.uninstall();
        s.sentinel->confirm();
        return iface;
    }
    breadcrumb("vr: compositor hook committed");
    Log::get().note("compositor hook installed on %s (Submit slot %zu, WaitGetPoses "
                    "slot %zu)", interfaceVersion, submitSlot, posesSlot);

    configurePoseRing(&s);

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
    resubmitShadowConfigure();
    // The cull guard's twin of the same rule (its own install already read
    // config -- the system interface arrives first -- but this path is the
    // one check_install_reads.py can see, and a second read is free).
    systemHookConfigure();

    return iface;
}

void shutdownCompositorHook() {
    // Per-site fault totals, so "logged once" does not mean "counted once".
    reportFaultSites();
    if (!g_state) return;
    Log::get().note("transition flash: %u eye-submit(s) withheld this session "
                    "(%u replaced with the previous frame's copy on time, %u "
                    "fell back to a missed deadline).",
                    g_state->framesWithheld, resubmitShadowResubmits(),
                    resubmitShadowFallbacks());
    resubmitShadowShutdown();
    g_state->compositorHook.uninstall();
    if (g_state->sentinel) g_state->sentinel->confirm();
}

}  // namespace edvr
